"""Pathfinding over the player's fogged map view.

The board is a torus (wrapQ/wrapR in hex-map.hpp), flat-top axial, six
neighbours.  Move cost is TERRAIN_MC and 255 means impassable -- except the
River, which a Raft makes passable, so the authority on whether a *single*
step is legal right now is the server's own `vm` bitmask, not this module.
Use vm for the step you are about to take and this module to choose which
direction that should be.

Fog matters: an unrevealed cell is None.  Treating unknown as impassable would
strand a bot behind its own vision radius, so unknown cells are traversable at
an assumed cost and are themselves worth reaching -- they are where the
exploration points and the unseen piles are.

Everything here takes the *board* as an argument rather than reading
MAP_COLS/MAP_ROWS, because there are two of them.  The bunker tunnel board is
16x10 and walled: `world.wraps` is False, and a neighbour outside it is a wall,
not the far side.  Pass `obs.board` (or `obs.map` / `obs.tunnel` explicitly)
and the same Dijkstra serves both.

The tunnel board also has walls between cells: it is corridors one hex wide,
and a cell's `op` names which of its sides are open (tunnels.hpp "Open
sides").  A step through a closed side is refused server-side exactly like
rock, so the search never takes one -- checked from both cells, since either
may be the one we have seen.
"""
import heapq

from config import (DQ, DR, MAP_COLS, MAP_ROWS, TERRAIN_MC, IMPASSABLE,
                    NUM_TERRAIN, TERR_TUNNEL, tun_distance)

_NEIGH = tuple(zip(DQ, DR))
# TERRAIN_MC with the impassables as None, so the inner loop is one index.
_MC = tuple(None if mc == IMPASSABLE else mc for mc in TERRAIN_MC)
_BIG = 1 << 30

# What an unrevealed cell is assumed to cost.  Slightly above the average
# passable cost (1.50 measured over the real map) so a bot prefers a known
# road to a gamble, but not so high it refuses to explore.
UNKNOWN_COST = 2

# The Wasteland Daisy (docs/ecology-spec.md): a bloomed patch bites for 1 LL
# on entry, a bud blooms at the next dawn.  Priced as extra AP in
# WorldMap.extra_cost rather than made impassable, so a bot still crosses
# one when it is the only way through -- it just stops being the cheap way.
# 6 AP is most of a day's movement: a detour of up to five hexes is taken
# before a bite is.  Stage 1 shoots cost nothing; they are three dawns out.
DAISY_BLOOM_COST = 6
DAISY_BUD_COST = 1

# A booby trap we escaped (traps.hpp): stepping on it forces the scene again,
# with no walking away. Priced like a bloomed daisy and then some -- a detour
# of most of a day is taken first -- but never impassable, since it may be
# the only way out of a pocket.
TRAP_KNOWN_COST = 8

# Machine Gun Kane's reach (kane.hpp): open ground within KANE_RANGE of a jeep
# on the road. Standing there gets you asked, and still standing there a world
# tick later gets you shot, so a route through it is priced, never forbidden
# -- he moves a hex a tick and the next tick's ring is a guess. Once he has
# asked US, leaving cover costs most of a day's AP: the burst is 15 s away.
# Unrevealed hexes in his reach are priced at half, since scrub and dunes are
# most of the map but not all of it.  state.py fills WorldMap.danger_cost.
KANE_DANGER_COST = 4
KANE_CALLED_COST = 30
# The Creeping Doom (world-system.hpp resolveDoomProximity): from
# awareness 76 it burns the hex it stands on and strips the node under
# anyone adjacent; at 100 it also takes an LL every world tick.  Price
# the ground around it so no plan ends beside it -- the cost is additive,
# like Kane's reach, so a route *through* it still exists when there is
# no other.  state.py builds it into WorldMap.danger_cost with his.
DOOM_DANGER_AWARENESS = 76
# Radius 3 and a cost past any one hex's worth: on the first rerun a water
# pile one hex from the Doom was still the hunt's best value, and the bot
# walked back in the moment the flee layer let go.
DOOM_DANGER_RADIUS = 3
DOOM_DANGER_COST = 60


def step_cost(cell) -> int | None:
    """AP to enter, or None if impassable.  Unknown cells get UNKNOWN_COST."""
    if cell is None:
        return UNKNOWN_COST
    t = cell.terrain
    if t >= NUM_TERRAIN:
        return None
    mc = TERRAIN_MC[t]
    if mc == IMPASSABLE:
        return None
    return mc + (TRAP_KNOWN_COST if getattr(cell, "trap", False) else 0)


def walled(here, there, direction) -> bool:
    """True when a known closed side stands between two tunnel cells.  An
    unknown side (fog, or a server that sends no `op`) is assumed open, the
    same optimism unknown terrain gets."""
    if here is not None and getattr(here, "op", -1) >= 0 and not (here.op >> direction) & 1:
        return True
    back = (direction + 3) % 6
    if there is not None and getattr(there, "op", -1) >= 0 and there.terrain == TERR_TUNNEL             and not (there.op >> back) & 1:
        return True
    return False


# -- one decision, one search -------------------------------------------------
# SurvivorPolicy._decide() walks a priority list, and up to eight of its
# layers (the grave, the canteen errand, the staple hunt, the caravan, Kane's
# chapters, pursuit) each run their own search from the same hex over the
# same map, most of them only to find there is nothing to walk to.  Measured
# 2026-10-13 on a fully revealed map: a quester spent 35 ms a cycle this way,
# 5 bots x 3 cycles/s of it on the one asyncio thread that also reads every
# socket.  Within one decide() the map cannot change, so the searches are
# memoised for exactly that long: SurvivorPolicy.decide() opens a cycle and
# closes it, and outside a cycle nothing is cached, so a test that mutates a
# map between two searches sees the mutation.
_memo: dict | None = None


def begin_cycle() -> None:
    global _memo
    _memo = {}


def end_cycle() -> None:
    global _memo
    _memo = None


def dijkstra(world, start_q, start_r, max_cost=60, cost_fn=None, stop_at=None,
             danger=True):
    """Cost-to-reach every cell within max_cost.

    Returns (dist, first_dir): dist[(q, r)] -> AP, first_dir[(q, r)] -> the
    direction to step from the start to begin that path.  Frontier is capped
    by max_cost so this stays bounded on a 4275-cell map.

    `cost_fn(cell)` overrides step_cost -- return None to make a cell
    impassable for this search.

    `stop_at(cell, q, r)` marks a cell that can be *entered* but not walked
    through.  Hatches need this: stepping onto a Bunker Entrance or Vent
    Shaft crosses the boards as the last act of the move, so a route that
    merely passes over one does not exist -- it ends there, somewhere the
    caller did not choose.  Without it the pathfinder happily plots a line
    through a hatch and the survivor is ejected mid-journey.

    `danger=False` leaves out WorldMap.danger_cost (Machine Gun Kane's reach).
    Only the search for cover from him wants that: it is fine to cross his
    reach on the way, as long as the step that ends the move is out of it.
    """
    # Callers only read the result, so a memo hit hands back the same dicts.
    key = (id(world), start_q, start_r, max_cost, cost_fn, stop_at, danger)
    memo = _memo
    if memo is not None:
        hit = memo.get(key)
        if hit is not None:
            return hit
    start = (start_q, start_r)
    dist = {start: 0}
    first = {start: -1}
    pq = [(0, start_q, start_r)]
    wraps = getattr(world, "wraps", True)
    cols, rows = world.cols, world.rows
    grid = world.grid
    extra = getattr(world, "extra_cost", None) or None
    hazard = (getattr(world, "danger_cost", None) or None) if danger else None
    heappop, heappush = heapq.heappop, heapq.heappush
    while pq:
        d, q, r = heappop(pq)
        if d > dist.get((q, r), _BIG):
            continue
        if d >= max_cost:
            continue
        here = world[(q, r)]
        if stop_at is not None and (q, r) != start and stop_at(here, q, r):
            continue        # enterable, but the journey ends on it
        for direction, (dq, dr) in enumerate(_NEIGH):
            nq = q + dq
            nr = r + dr
            if wraps:
                nq %= cols
                nr %= rows
                cell = grid[nr][nq]
            else:
                if not (0 <= nq < cols and 0 <= nr < rows):
                    continue    # walled board: off the edge is rock
                cell = grid[nr][nq]
                if walled(here, cell, direction):
                    continue    # a wall between two cells, not a doorway
            # step_cost(), inlined: this loop is most of a decide cycle.
            if cost_fn is not None:
                c = cost_fn(cell)
                if c is None:
                    continue
            elif cell is None:
                c = UNKNOWN_COST
            else:
                t = cell.terrain
                if t >= NUM_TERRAIN:
                    continue
                c = _MC[t]
                if c is None:
                    continue
                if cell.trap:
                    c += TRAP_KNOWN_COST
            if extra:
                c += extra.get((nq, nr), 0)
            if hazard:
                c += hazard.get((nq, nr), 0)
            nd = d + c
            if nd < dist.get((nq, nr), _BIG):
                dist[(nq, nr)] = nd
                first[(nq, nr)] = direction if (q, r) == start else first[(q, r)]
                heappush(pq, (nd, nq, nr))
    if memo is not None:
        memo[key] = (dist, first)
    return dist, first


def best_target(world, start_q, start_r, score_fn, max_cost=40,
                cost_fn=None, stop_at=None, danger=True):
    """Pick the reachable cell maximising score_fn(cell, q, r, cost).

    score_fn returns None for "not a candidate".  Ties break toward the
    cheaper cell.  Returns (q, r, direction, cost, value) or None.
    """
    dist, first = dijkstra(world, start_q, start_r, max_cost, cost_fn, stop_at,
                           danger=danger)
    best = None
    for (q, r), cost in dist.items():
        if cost == 0:
            continue
        d = first.get((q, r), -1)
        if d < 0:
            continue
        v = score_fn(world[(q, r)], q, r, cost)
        if v is None:
            continue
        if best is None or v > best[4] or (v == best[4] and cost < best[3]):
            best = (q, r, d, cost, v)
    return best


def hex_distance(q1, r1, q2, r2) -> int:
    """Shortest axial distance on the torus, ignoring terrain -- the
    firmware's hexDistWrap(), via hex_dist_wrap().

    Until 2026-10-12 this folded each torus delta to its absolute value
    before combining them, which throws away the one thing the third cube
    coordinate needs: whether q and r moved the same way.  Every step with
    opposite-sign deltas was over-counted -- (22,20) to (19,22) came out 5
    instead of 3 -- so the coward's fire and doom radii and the rival's
    contest radius were effectively smaller than written, and the tunnel
    bots' and metrics.py's "surface hexes crossed" overstated diagonal
    trips.  Same-sign and single-axis deltas were always right."""
    return hex_dist_wrap(q1, r1, q2, r2)


def hex_dist_wrap(q1, r1, q2, r2) -> int:
    """Exact port of hexDistWrap() (hex-map.hpp): the axial distance to the
    nearest of the nine wrapped copies of (q2, r2).  hex_distance() is this;
    the firmware decides who Machine Gun Kane can reach with it."""
    best = 1 << 30
    for wq in (-1, 0, 1):
        for wr in (-1, 0, 1):
            aq = q2 + wq * MAP_COLS - q1
            ar = r2 + wr * MAP_ROWS - r1
            best = min(best, (abs(aq) + abs(aq + ar) + abs(ar)) // 2)
    return best


def frontier_bonus(world, q, r) -> int:
    """How many of this cell's neighbours are still fogged.  A cell with more
    unknown neighbours reveals more when stepped on, which is worth something
    to an explorer beyond the flat +1 for a new hex.

    On a walled board an off-board neighbour is rock, not fog -- counting it
    would make the tunnel board's edges look like the most promising ground on
    it and pin an explorer against the wall."""
    n = 0
    wraps = getattr(world, "wraps", True)
    cols, rows, grid = world.cols, world.rows, world.grid
    for dq, dr in _NEIGH:
        nq = q + dq
        nr = r + dr
        if wraps:
            nq %= cols
            nr %= rows
        elif not (0 <= nq < cols and 0 <= nr < rows):
            continue
        if grid[nr][nq] is None:
            n += 1
    return n


def board_distance(world, q1, r1, q2, r2) -> int:
    """Terrain-free hex distance on whichever board this is."""
    if getattr(world, "wraps", True):
        return hex_distance(q1, r1, q2, r2)
    return tun_distance(q1, r1, q2, r2)
