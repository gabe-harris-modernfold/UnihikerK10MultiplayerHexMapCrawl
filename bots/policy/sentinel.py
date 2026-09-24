"""Sentinel -- makes camp, keeps watch, sleeps last.  The soak bot's policy.

Why it exists: tickGame() starts a new day at DAY_TICKS (5 real minutes) or
the moment every connected player is resting, whichever is first.  An
all-bot fleet rests the instant it runs out of MP, so days collapse to
seconds -- fine for the supply economy, useless for everything on a real
clock (weather is floored at ~1.1-1.9 real minutes by weatherNextGapMs; fire,
flood and Creeping Doom tick on WORLD_TICK_INTERVAL).  One player awake
holds the day open.  The Sentinel is that player.

Why it still sleeps: a survivor who never rests never heals.  dawnUpkeep's
rest heal is the only LL recovery there is, and "resting in shelter
suppresses all LL losses" is what makes a night survivable at all -- a
first cut that never rested simply bled out over a long soak.  So the
Sentinel stays awake for REST_AT_FRACTION of each day, then rests in its
shelter.  Days still run ~90% of full length (a soak wants 5-minute days,
not 5-second ones), and every round ends with it asleep under cover.

The day it is in is timed off the broadcast's tick id: dawn is seen as the
day count changing, and tickGame() advances tickId and dayTick together.
The first day after joining is never timed, so it just stays awake until
dawn comes on its own.

What it does, in order:

  1. **Pick a camp.**  The best known surface hex by what it offers a
     survivor who will stand there for hours: drinkable water (ACT_WATER),
     forage, an existing shelter, a Settlement (cover, and the rest heal is
     guaranteed there).  Never radioactive, never a hatch (stepping on one
     is a descent), never impassable.  Re-picked while unbuilt if something
     clearly better comes into view; sticky once a shelter stands on it.
  2. **Get the scrap.**  A basic shelter is 1 scrap + 1 MP, an improved one
     2 + 2 (doShelter).  Improved is immune to Strangle Fog, basic only
     halves it; either is full cover from exposure and the chem storm.
     Piles first, SCAVENGE where the terrain salvages.
  3. **Build**, improved if it can afford it, and upgrade later if not.
     Quakes knock shelters down, so it checks and rebuilds every visit.
  4. **Soak.**  Stand in camp.  Top up water and food from the hex when it
     can, walk out for staples when it cannot, come back.  Never wander.
  5. **Sleep last.**  Late in the day, rest -- in camp if it is there, and
     under a quick basic shelter if it is caught out on exposed ground.

It plays nothing like ScoreMax any more, and its score is not comparable.
Read it for what the soak measures: real-clock hazards per real minute,
and whether a survivor who does everything right can live through them.

Never wrap it in sprint mode: SprintPolicy forces REST the moment MP runs
out, which collapses the day.  arena.py refuses the combination.
"""
import time

from config import (ACT_FORAGE, ACT_SCAV, ACT_SHELTER, ACT_WATER,
                    DAY_TICKS, IMPASSABLE, NUM_TERRAIN, RES_FOOD, RES_SCRAP,
                    RES_WATER, TERR_RIVER, TERR_SETTLEMENT, TERRAIN_IS_RAD,
                    TERRAIN_MC, TERRAIN_SV, can_forage, can_salvage, has_water,
                    is_exposed, is_hatch_terrain, is_tunnel_terrain)
from navigate import best_target, dijkstra
from .base import Action
from .scoremax import ScoreMaxPolicy
from .survivor import ends_journey, token_room

# Awake for this share of each day, then rest.
REST_AT_FRACTION = 0.9
# What to keep in the pack while camped: dawn takes 1 food and 2 water.
WATER_TARGET = 5
FOOD_TARGET = 3
CAMP_SEARCH_COST = 40
SCRAP_SEARCH_COST = 30
# Only walk out for upgrade scrap while there is day enough to come back.
UPGRADE_TRIP_COST = 6
# A new site must beat the current unbuilt one by this much to switch.
CAMP_HYSTERESIS = 2.0
# The act event's "cnd" updates the map (state.py), but until it lands the
# map still says "no shelter"; one SHELTER per hex per this long, not one per
# decide cycle.
SHELTER_RETRY_S = 4.0


def camp_value(cell, q, r, cost) -> float | None:
    """How good a place to stand for hours this hex is, net of getting there."""
    if cell is None:
        return None
    t = cell.terrain
    if t >= NUM_TERRAIN or TERRAIN_MC[t] == IMPASSABLE or t == TERR_RIVER:
        return None                 # River needs a Raft to stand on
    if is_hatch_terrain(t) or is_tunnel_terrain(t) or TERRAIN_IS_RAD[t]:
        return None
    v = 1.0
    if has_water(t):
        v += 6.0                    # the binding constraint, harvested in place
    if can_forage(t):
        v += 4.0
    if t == TERR_SETTLEMENT:
        v += 5.0                    # cover, and the rest heal always fires
    v += 2.5 * cell.shelter         # someone already paid the scrap
    if TERRAIN_SV[t] >= 2:
        v += 1.0
    return v - 0.15 * cost


class SentinelPolicy(ScoreMaxPolicy):
    name = "sentinel"
    rest_at_ticks = int(DAY_TICKS * REST_AT_FRACTION)

    def __init__(self, rng, **kw):
        super().__init__(rng, **kw)
        self.camp: tuple[int, int] | None = None
        self._camp_v = None
        self._day = None
        self._dawn_tick = None
        self._shelter_sent: dict[tuple[int, int], float] = {}
        self.camp_stats = {"camps": 0, "shelters": 0, "upgrades": 0,
                           "rests": 0, "rests_in_camp": 0}

    # -- the clock ---------------------------------------------------------
    def _clock(self, obs) -> None:
        if obs.day != self._day:
            if self._day is not None:           # a dawn we watched happen
                self._dawn_tick = obs.tick
            self._day = obs.day

    def late(self, obs) -> bool:
        """Past REST_AT_FRACTION of today.  Unknown (first day) reads as no."""
        return (self._dawn_tick is not None
                and obs.tick - self._dawn_tick >= self.rest_at_ticks)

    def rest_once(self, obs, why: str) -> Action | None:
        # Every rest path in SurvivorPolicy lands here, so this one gate is
        # what holds the day open: no rest until late, whatever the reason.
        if not self.late(obs):
            return None
        act = super().rest_once(obs, why)
        if act is not None:
            self.camp_stats["rests"] += 1
            if self.camp == (obs.me.q, obs.me.r):
                self.camp_stats["rests_in_camp"] += 1
        return act

    # -- main ----------------------------------------------------------------
    def decide(self, obs) -> Action:
        self._clock(obs)
        me = obs.me
        if not me.connected or me.ll == 0:
            return Action("noop", why="downed or disconnected")
        if obs.encounter is not None:
            return self.decide_encounter(obs)
        if obs.underground:
            return self.climb_out(obs, why="fell down a hatch")
        if me.resting:
            return Action("noop", why="asleep")
        act = self.gear_action(obs)
        if act is not None:
            return act

        here = obs.here()
        at_camp = self.camp is not None and (me.q, me.r) == self.camp

        if self.late(obs):
            # Never go to sleep uncovered if a scrap can prevent it: finish an
            # unbuilt camp, or -- caught out on exposed ground -- throw up a
            # basic shelter where we stand; it costs what one step would.
            if (here is not None and here.shelter == 0
                    and here.terrain != TERR_SETTLEMENT
                    and (at_camp or (is_exposed(here.terrain) and me.archetype != 5))
                    and me.inv[RES_SCRAP] >= 1 and me.mp >= 1):
                act = self._shelter(obs, "bedtime: " + ("finishing camp" if at_camp
                                                        else "cover where I stand"))
                if act is not None:
                    if at_camp:
                        self.camp_stats["shelters"] += 1
                    return act
            act = self.rest_once(obs, "bedtime" + (" in camp" if at_camp else ", away from camp"))
            if act is not None:
                return act

        self._choose_camp(obs)
        at_camp = self.camp is not None and (me.q, me.r) == self.camp
        if at_camp:
            act = self._keep_camp(obs, here)
            return act if act is not None else Action("noop", why="soaking in camp")

        # Away from camp: survival first (harvest underfoot, emergency staples).
        act = self.survival_action(obs)
        if act is not None:
            return act
        if self.camp is None:
            return self.pursue(obs)             # explore until somewhere will do
        cell = obs.map[self.camp]
        if self._needs_cover(cell) and me.inv[RES_SCRAP] < 1:
            act = self._fetch_scrap(obs, SCRAP_SEARCH_COST)
            if act is not None:
                return act
        return self._walk_to_camp(obs)

    # -- camp ------------------------------------------------------------------
    def _choose_camp(self, obs) -> None:
        me = obs.me
        if self.camp is not None:
            cell = obs.map[self.camp]
            if cell is not None and camp_value(cell, *self.camp, 0) is None:
                self.camp = None                # the site changed under us
            elif cell is not None and cell.shelter:
                return                          # built: sticky
        here = obs.map[(me.q, me.r)]
        best = None
        hv = camp_value(here, me.q, me.r, 0)
        if hv is not None:
            best = (me.q, me.r, hv)
        t = best_target(obs.map, me.q, me.r, camp_value,
                        max_cost=CAMP_SEARCH_COST, stop_at=ends_journey)
        if t is not None and (best is None or t[4] > best[2]):
            best = (t[0], t[1], t[4])
        if best is None:
            return
        if self.camp is None or best[2] > (self._camp_v or 0) + CAMP_HYSTERESIS:
            if self.camp != (best[0], best[1]):
                self.camp_stats["camps"] += 1
            self.camp, self._camp_v = (best[0], best[1]), best[2]

    @staticmethod
    def _needs_cover(cell) -> bool:
        """A Settlement is cover already; anywhere else wants a shelter."""
        return cell is None or (cell.terrain != TERR_SETTLEMENT and cell.shelter == 0)

    def _keep_camp(self, obs, here) -> Action | None:
        me = obs.me
        t = here.terrain if here is not None else 0
        scrap = me.inv[RES_SCRAP]
        if here is not None and t != TERR_SETTLEMENT:
            if here.shelter == 0:
                if scrap >= 1 and me.mp >= 1:
                    kind = "improved" if scrap >= 2 and me.mp >= 2 else "basic"
                    act = self._shelter(obs, f"making camp: {kind} shelter")
                    if act is not None:
                        self.camp_stats["shelters"] += 1
                        return act
                else:
                    act = self._fetch_scrap(obs, SCRAP_SEARCH_COST)
                    if act is not None:
                        return act
            elif here.shelter == 1 and scrap >= 2 and me.mp >= 2:
                act = self._shelter(obs, "upgrading camp: fog immunity")
                if act is not None:
                    self.camp_stats["upgrades"] += 1
                    return act
        if has_water(t) and me.inv[RES_WATER] < WATER_TARGET and me.mp >= 1:
            return Action("act", a=ACT_WATER, mp=min(3, me.mp),
                          why=f"camp water {me.inv[RES_WATER]}")
        if can_forage(t) and me.inv[RES_FOOD] < FOOD_TARGET and me.mp >= 2:
            return Action("act", a=ACT_FORAGE, why=f"camp food {me.inv[RES_FOOD]}")
        # The camp cannot supply what is running out: go and get it.
        act = self.staple_hunt(obs)
        if act is not None:
            return act
        # Spare MP and a basic shelter: a short trip for upgrade scrap.
        if (here is not None and here.shelter == 1 and scrap < 2 and me.mp >= 4
                and not self.late(obs)):
            return self._fetch_scrap(obs, UPGRADE_TRIP_COST)
        return None

    def _shelter(self, obs, why: str) -> Action | None:
        key = (obs.me.q, obs.me.r)
        now = time.monotonic()
        if now - self._shelter_sent.get(key, -1e9) < SHELTER_RETRY_S:
            return None
        self._shelter_sent[key] = now
        return Action("act", a=ACT_SHELTER, why=why)

    def _fetch_scrap(self, obs, max_cost) -> Action | None:
        me = obs.me
        here = obs.here()
        if (here is not None and can_salvage(here.terrain) and me.mp >= 2
                and token_room(me) > 0):
            return Action("act", a=ACT_SCAV, why="salvaging scrap for a shelter")
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(cell, q, r, cost):
            if cell is None or cost <= 0:
                return None
            if cell.resource == RES_SCRAP + 1:
                return 10.0 / cost
            if can_salvage(cell.terrain):
                return 4.0 / cost
            return None
        t = best_target(obs.map, me.q, me.r, value, max_cost=max_cost,
                        stop_at=ends_journey)
        if t is None or t[2] not in legal:
            return None
        return Action("move", d=t[2], why=f"scrap for the camp -> ({t[0]},{t[1]})")

    def _walk_to_camp(self, obs) -> Action:
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="out of MP on the way to camp; awake")
        _dist, first = dijkstra(obs.map, me.q, me.r, max_cost=80, stop_at=ends_journey)
        d = first.get(self.camp, -1)
        if d < 0:
            self.camp = None                    # unreachable after all
            return self.pursue(obs)
        if d not in legal:
            return Action("noop", why="camp route blocked this step; waiting")
        return Action("move", d=d, why=f"to camp {self.camp}")

    def content_score(self) -> dict:
        s = super().content_score()
        s["camp"] = dict(self.camp_stats, at=self.camp)
        return s
