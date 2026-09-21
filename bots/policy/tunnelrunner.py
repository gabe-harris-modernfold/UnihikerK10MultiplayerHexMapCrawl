"""TunnelRunner -- the other half of the Subterranean Explorers.

Where `subterranean` lives in the corridors, this one only passes through
them.  It is a courier: dive at the nearest hatch, walk to the shaft that
surfaces furthest away, climb out, and work the fresh ground it has landed
on.  Then do it again.

It exists to measure one specific claim.  docs/tunnel-system-spec.md says,
from 150 generated worlds, that the tunnels save 44% of the MP on a 10-19 hex
crossing rising to 67% on a 40-49 hex one:

    surface gap 20-29 hexes:  39.6 MP over ground  vs  17.0 MP underground
    surface gap 40-49 hexes:  65.6 MP over ground  vs  21.7 MP underground

That was computed over the generator's own corridor paths with perfect
knowledge of the network.  A player has neither: underground vision is a
single ring, the surface-to-shaft pairing is never sent on the wire, and the
only way to learn where a shaft comes out is to climb it and look.  So the
number this bot reports -- `mp_per_surface_hex`, underground MP spent per
surface hex actually crossed -- is the *achievable* saving rather than the
theoretical one, and the gap between the two is the cost of not having the
map.  Anything under ~1.6 (the surface average MC) is a win.

Two behaviours are deliberate and worth not "fixing":

- **It will not climb out of the shaft it came down.**  That is a round trip
  to nowhere, and it is what a naive nearest-exit rule does every single
  time.  Only hunger or a failing LL overrides it (`self._urgent`).
- **It sleeps underground rather than abandoning a crossing.**  Running out
  of MP mid-corridor is not fatal: dawn refills the budget, REST works at
  depth 1, and no weather reaches down there.  What it costs is the
  `TUNNEL_REST_LL_PCT` bad-air roll, which the rest-heal absorbs while the
  supplies hold and which can take the last LL when they do not.  The hatch
  it dived through no longer changes that -- it only changes the climb out,
  1 MP against 2 -- which is still why it prefers a Bunker Entrance.
"""
from config import RES_FOOD, RES_WATER, is_hatch_terrain
from navigate import best_target, frontier_bonus, hex_distance
from .subterranean import SURFACE_FOOD, TunnelPolicy
from .survivor import ends_journey

# How much a surface hex of separation is worth when choosing an exit. A long
# crossing is the whole product, so this dominates the walk to reach it.
GAP_WEIGHT = 6.0
# An unpaired shaft is an unknown exit. Worth a lot: it is the only way to
# learn the pairing, and an unknown shaft is certainly not the one we came in
# through (that one paired itself the moment we descended).
UNKNOWN_EXIT_VALUE = 60.0
# Do not bother crossing for less than this. Below it the descend + ascend MP
# is most of the trip and the surface walk would have been cheaper.
MIN_WORTHWHILE_GAP = 6
# Diving straight back down the hatch we just climbed out of undoes the whole
# crossing.
RETURN_HATCH_PENALTY = 55.0

PILE_VALUE = 20
NEW_HEX_VALUE = 2
FRONTIER_WEIGHT = 1.0


class TunnelRunnerPolicy(TunnelPolicy):
    """Uses the bunker network as transport, not as a place to be."""

    name = "tunnelrunner"
    # Not down there to work the corridors: scavenging costs 2 MP that would
    # otherwise be a step toward the far exit.
    scav_underground = False

    def __init__(self, rng, library=None):
        super().__init__(rng, library)
        self._urgent = False          # hunger/LL, not transit: take any exit
        self._last_exit = None        # surface hex we last climbed out at

    def on_event(self, ev):
        super().on_event(ev)
        if self.pid >= 0 and ev.get("pid") not in (None, self.pid):
            return
        if ev.get("k") == "tun_out" and ev.get("q") is not None:
            self._last_exit = (ev["q"], ev["r"])

    # ── underground: always on the way somewhere ────────────────────────
    def surface_reason(self, obs) -> str | None:
        """A runner is never "staying down" -- it is always mid-crossing.

        Only two things make the exit urgent enough to take any shaft going,
        including the one we arrived through: there is nothing to eat
        underground, and a survivor who is hurt *and* short of supplies
        cannot absorb the bad-air roll a night down here charges.
        """
        me = obs.me
        self._urgent = False
        if me.inv[RES_FOOD] <= SURFACE_FOOD:
            self._urgent = True
            return f"food {me.inv[RES_FOOD]}: nothing to eat underground"
        if me.ll <= self.rest_below_ll + 1 and not (me.food >= 2 and me.water >= 2):
            self._urgent = True
            return f"LL {me.ll} on thin supplies: bad air can finish that"
        return "crossing to the far side"

    def exit_preference(self, obs):
        """Rank the shafts by how much ground climbing out of them covers.

        Read against the hatch this dive started at, which `tun_in` gave us.
        A shaft whose pairing we have never learned scores well on its own
        account -- the table is not on the wire, so using it is the only way
        to find out where it goes.
        """
        entry = self._dive or {}
        eq, er = entry.get("q"), entry.get("r")
        entry_idx = entry.get("hatch")
        urgent = self._urgent

        def prefer(cell, q, r, cost):
            if cell is None or not is_hatch_terrain(cell.terrain):
                return None
            h = obs.hatch_for_shaft(q, r)
            if h is None:
                # Unknown pairing. Cannot be the shaft we came down -- that
                # one was paired by our own descent -- so it always moves us.
                return UNKNOWN_EXIT_VALUE / cost
            if h.idx == entry_idx:
                # Back out the way we came: pointless unless we need out now.
                return (1.0 / cost) if urgent else None
            if eq is None:
                return 20.0 / cost
            gap = hex_distance(eq, er, h.sq, h.sr)
            if gap < MIN_WORTHWHILE_GAP and not urgent:
                return None
            return (gap * GAP_WEIGHT) / cost

        return prefer

    def below_target(self, obs):
        """Only reached when climb_out found no acceptable exit: push into
        the dark until a shaft shows up.  Vision underground is one ring, so
        most of a first crossing is spent doing exactly this."""
        q, r = obs.pos()

        def value(cell, cq, cr, cost):
            if cost <= 0:
                return None
            if cell is None:
                return (12.0 + 2.0) / cost
            if is_hatch_terrain(cell.terrain):
                return None
            v = 1.0
            if cell.resource and not self.pack_full(obs):
                v += PILE_VALUE          # free if it is on the way
            v += 2.0 * frontier_bonus(obs.tunnel, cq, cr)
            return v / cost

        return best_target(obs.tunnel, q, r, value, max_cost=60,
                           stop_at=ends_journey)

    # ── surface: spend what the crossing bought ─────────────────────────
    def hatch_bonus(self, obs, cell, q, r):
        v = super().hatch_bonus(obs, cell, q, r)
        if self._last_exit is not None and (q, r) == self._last_exit:
            # Straight back down the hole we just came out of.
            v -= RETURN_HATCH_PENALTY
        return max(1.0, v)

    def surface_value(self, obs, cell, q, r, cost):
        """Unlike the explorer, this one is up here to *use* the ground.

        The point of the crossing is the untouched region on the far side:
        piles pay 10 a token on contact and every new hex is a point, so the
        weighting is close to scoremax's -- run the two side by side and the
        difference is what the tunnels bought.
        """
        me = obs.me
        if cost <= 0:
            return None
        v = 0.0
        if cell is None:
            v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
        else:
            if is_hatch_terrain(cell.terrain):
                return None            # crossing is a decision, not an accident
            if cell.resource and not self.pack_full(obs):
                v += PILE_VALUE
                if me.inv[RES_WATER] < 4 and cell.resource == RES_WATER + 1:
                    v += 12.0
                if me.inv[RES_FOOD] < 2 and cell.resource == RES_FOOD + 1:
                    v += 12.0
            if not cell.visited_by(obs.pid):
                v += NEW_HEX_VALUE
            v += FRONTIER_WEIGHT * frontier_bonus(obs.map, q, r)
        return None if v <= 0 else v / cost
