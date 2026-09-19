"""Rival -- plays against the other survivors, not just the map.

Three hostile behaviours, each testing something specific:

**POI denial.** POIs are permanently consumed -- enc_start sets cell.poi = 0
and only an involuntary end restores it -- so reaching one first removes it
from the game for everyone. The Rival preferentially races for POIs that a
rival is closer to than the open map would suggest, which is the sharpest
zero-sum interaction the rules allow.

**Scavenging.** Ground items are free to whoever picks them up, so it grabs
anything dropped nearby.

**Exploitative trades.** It offers lopsided swaps -- asking for far more than
it gives -- to whichever neighbour is closest. The point is not that the offer
gets accepted; it is to find out whether a hostile client can grief a human
through the trade channel, and whether the 30 s TRADE_EXPIRE_MS window plus
repeated offers amounts to spam a player cannot escape.
"""
from config import RES_FOOD, RES_WATER
from navigate import best_target, frontier_bonus, hex_distance
from .base import Action
from .survivor import SurvivorPolicy

POI_VALUE = 50
CONTESTED_BONUS = 70      # a POI a rival is near is worth far more than a quiet one
CONTEST_RADIUS = 8
PILE_VALUE = 18
GROUND_ITEM_VALUE = 25
NEW_HEX_VALUE = 1
FRONTIER_WEIGHT = 0.5

TRADE_COOLDOWN_DECISIONS = 40    # do not spam every tick; ~15 s at 0.35 s cadence
TRADE_RANGE = 6


class RivalPolicy(SurvivorPolicy):
    name = "rival"
    engage_encounters = True
    min_success = 0.35
    bank_greed = 0.50
    rest_below_ll = 2

    def __init__(self, rng, library=None):
        super().__init__(rng, library)
        self._since_trade = TRADE_COOLDOWN_DECISIONS
        self.stats["trades_offered"] = 0
        self.stats["pois_denied"] = 0

    def on_event(self, ev):
        # The base class already drops events belonging to other players, so
        # this only ever counts POIs *we* actually consumed -- not the ones we
        # merely asked for and not rivals' successes.
        was_mine = self.pid < 0 or ev.get("pid") in (None, self.pid)
        super().on_event(ev)
        if ev.get("k") == "enc_start" and was_mine and ev.get("pid") == self.pid:
            self.stats["pois_denied"] += 1

    def pursue(self, obs) -> Action:
        me = obs.me
        self._since_trade += 1

        cell_here = obs.map[(me.q, me.r)]
        if cell_here is not None and cell_here.poi and me.mp > 0:
            act = self.try_open_poi(obs, "deny POI")
            if act is not None:
                return act

        # Anything on the floor here is free.
        for gi in obs.ground_items:
            if gi.get("q") == me.q and gi.get("r") == me.r:
                return Action("noop", why="ground item here (pickup not wired)")

        offer = self._maybe_trade(obs)
        if offer is not None:
            return offer

        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        full = self.pack_full(obs)
        others = [(p.q, p.r) for p in obs.rivals() if p.ll > 0]
        ground = {(g.get("q"), g.get("r")) for g in obs.ground_items}

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = 0.0
            if (q, r) in ground:
                v += GROUND_ITEM_VALUE
            if cell is None:
                v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
            else:
                if cell.poi:
                    v += POI_VALUE
                    # Contested POIs are the ones worth sprinting for: taking
                    # one a rival is circling removes it from them for good.
                    if any(hex_distance(q, r, oq, orr) <= CONTEST_RADIUS
                           for oq, orr in others):
                        v += CONTESTED_BONUS
                if cell.resource and not full:
                    v += PILE_VALUE
                if not cell.visited_by(obs.pid):
                    v += NEW_HEX_VALUE
                v += FRONTIER_WEIGHT * frontier_bonus(obs.map, q, r)
            return None if v <= 0 else v / cost

        target = best_target(obs.map, me.q, me.r, value, max_cost=45)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d, why=f"hunt -> ({q},{r}) v={val:.1f}")
        return Action("move", d=self.rng.choice(legal), why="fallback step")

    def _maybe_trade(self, obs) -> Action | None:
        """Lopsided offer to the nearest neighbour, on a cooldown.

        Gives one token of whatever we have most of, asks for three of each
        staple.  Deliberately unfair -- the experiment is whether the trade UI
        and the 30 s expiry protect a human from being pestered.
        """
        if self._since_trade < TRADE_COOLDOWN_DECISIONS:
            return None
        me = obs.me
        near = [p for p in obs.rivals()
                if p.ll > 0 and hex_distance(me.q, me.r, p.q, p.r) <= TRADE_RANGE]
        if not near:
            return None
        mark = min(near, key=lambda p: hex_distance(me.q, me.r, p.q, p.r))

        give = [0] * 5
        surplus = max(range(5), key=lambda i: me.inv[i])
        if me.inv[surplus] > 0:
            give[surplus] = 1
        want = [0] * 5
        want[RES_WATER] = 3
        want[RES_FOOD] = 3

        self._since_trade = 0
        self.stats["trades_offered"] += 1
        return Action("trade_offer", to=mark.pid, give=give, want=want,
                      why=f"lowball pid {mark.pid}")
