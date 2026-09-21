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
"""
import heapq

from config import (DQ, DR, MAP_COLS, MAP_ROWS, TERRAIN_MC, IMPASSABLE,
                    NUM_TERRAIN, tun_distance)

# What an unrevealed cell is assumed to cost.  Slightly above the average
# passable cost (1.50 measured over the real map) so a bot prefers a known
# road to a gamble, but not so high it refuses to explore.
UNKNOWN_COST = 2


def step_cost(cell) -> int | None:
    """MP to enter, or None if impassable.  Unknown cells get UNKNOWN_COST."""
    if cell is None:
        return UNKNOWN_COST
    t = cell.terrain
    if t >= NUM_TERRAIN:
        return None
    mc = TERRAIN_MC[t]
    return None if mc == IMPASSABLE else mc


def dijkstra(world, start_q, start_r, max_cost=60, cost_fn=None, stop_at=None):
    """Cost-to-reach every cell within max_cost.

    Returns (dist, first_dir): dist[(q, r)] -> MP, first_dir[(q, r)] -> the
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
    """
    dist = {(start_q, start_r): 0}
    first = {(start_q, start_r): -1}
    pq = [(0, start_q, start_r)]
    wraps = getattr(world, "wraps", True)
    cols, rows = world.cols, world.rows
    cost = cost_fn or step_cost
    while pq:
        d, q, r = heapq.heappop(pq)
        if d > dist.get((q, r), 1 << 30):
            continue
        if d >= max_cost:
            continue
        if stop_at is not None and (q, r) != (start_q, start_r) \
                and stop_at(world[(q, r)], q, r):
            continue        # enterable, but the journey ends on it
        for direction in range(6):
            nq = q + DQ[direction]
            nr = r + DR[direction]
            if wraps:
                nq %= cols
                nr %= rows
            elif not (0 <= nq < cols and 0 <= nr < rows):
                continue        # walled board: off the edge is rock
            c = cost(world[(nq, nr)])
            if c is None:
                continue
            nd = d + c
            if nd < dist.get((nq, nr), 1 << 30):
                dist[(nq, nr)] = nd
                first[(nq, nr)] = direction if (q, r) == (start_q, start_r) else first[(q, r)]
                heapq.heappush(pq, (nd, nq, nr))
    return dist, first


def best_target(world, start_q, start_r, score_fn, max_cost=40,
                cost_fn=None, stop_at=None):
    """Pick the reachable cell maximising score_fn(cell, q, r, cost).

    score_fn returns None for "not a candidate".  Ties break toward the
    cheaper cell.  Returns (q, r, direction, cost, value) or None.
    """
    dist, first = dijkstra(world, start_q, start_r, max_cost, cost_fn, stop_at)
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
    """Shortest axial distance on the torus, ignoring terrain."""
    dq = min((q1 - q2) % MAP_COLS, (q2 - q1) % MAP_COLS)
    dr = min((r1 - r2) % MAP_ROWS, (r2 - r1) % MAP_ROWS)
    # Flat-top axial: the third cube coordinate moves opposite to q+r.
    ds = abs(dq + dr)
    return max(dq, dr, ds) if (dq and dr) else max(dq, dr, ds)


def frontier_bonus(world, q, r) -> int:
    """How many of this cell's neighbours are still fogged.  A cell with more
    unknown neighbours reveals more when stepped on, which is worth something
    to an explorer beyond the flat +1 for a new hex.

    On a walled board an off-board neighbour is rock, not fog -- counting it
    would make the tunnel board's edges look like the most promising ground on
    it and pin an explorer against the wall."""
    n = 0
    wraps = getattr(world, "wraps", True)
    for direction in range(6):
        nq = q + DQ[direction]
        nr = r + DR[direction]
        if wraps:
            nq %= world.cols
            nr %= world.rows
        elif not (0 <= nq < world.cols and 0 <= nr < world.rows):
            continue
        if world[(nq, nr)] is None:
            n += 1
    return n


def board_distance(world, q1, r1, q2, r2) -> int:
    """Terrain-free hex distance on whichever board this is."""
    if getattr(world, "wraps", True):
        return hex_distance(q1, r1, q2, r2)
    return tun_distance(q1, r1, q2, r2)
