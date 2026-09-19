"""Shared competence for the non-random policies.

Everything here is about *not losing* -- staying alive, keeping the pack
usable, and driving an encounter to a sane conclusion.  What a bot actually
chases is left to `pursue()` in the subclass, which is where the policies
differ and where the experiment lives.

The survival model, from dawnUpkeep() in survival_state.hpp:

  inv[RES_FOOD]  feeds the 0-6 food track: 1 token consumed per dawn
  inv[RES_WATER] feeds the 0-6 water track: 2 tokens consumed per dawn

Running a token to zero does not just stall the track, it steps it *down*,
and crossing a threshold costs LL.  So the bots budget tokens, not tracks.

Water is the real constraint.  ACT_WATER needs Marsh, Flooded or River
terrain -- about 4.7% of the map once the raft-gated River is excluded --
so most water has to come from walking onto piles.
"""
import time

from config import (ACT_FORAGE, ACT_REST, ACT_WATER, RES_FOOD, RES_WATER,
                    can_forage, has_water)
from encounters import EncounterLibrary, EncounterRun, success_chance
from navigate import best_target
from .base import Policy, Action

# Dawn eats 1 food + 2 water.  Keep a day of slack on top of that so a bot is
# never one bad step from a threshold break.
FOOD_FLOOR = 2
WATER_FLOOR = 4

# Below this many tokens, finding more overrides everything else.  Measured
# death spiral from the first four-policy run: water tokens hit 0 on day 2,
# the track then fell 6 -> 4 -> 1 over four days, LL followed it down, and
# because MP is ll + 3 the survivor lost the mobility it needed to go find
# water.  Dead on day 7.  The window to fix it is while MP is still high.
EMERGENCY_WATER = 1
EMERGENCY_FOOD = 0
# Hunting staples justifies a much longer walk than ordinary pursuit.
STAPLE_SEARCH_COST = 60
# Seconds between REST retries when we cannot tell whether the last one
# landed. Short enough to unstick a dropped REST within one dawn, long
# enough that it is nothing like the old per-cycle spam.
REST_RETRY_S = 4.0


class SurvivorPolicy(Policy):
    """Base for scoremax / contentmax / coward / rival."""

    # --- knobs subclasses override -------------------------------------
    engage_encounters = True    # open a POI when standing on one
    min_success = 0.45          # do not take a branch below this success odds
    bank_greed = 0.60           # push on past a bankable node below this odds
    rest_below_ll = 2           # REST when LL drops to or below this
    pack_headroom = 1           # keep this many free slots for collecting

    def __init__(self, rng, library: EncounterLibrary | None = None):
        super().__init__(rng)
        self.lib = library if library is not None else EncounterLibrary()
        self.run: EncounterRun | None = None
        # REST is idempotent server-side (doRest returns early if already
        # resting) but the periodic broadcast carries no `rt` field, so a
        # naive "rest while mp <= 0" check re-sends it every cycle forever.
        # Track it against the game-day instead; dawnUpkeep clears resting.
        self._rested_day = None
        self._last_rest_sent = 0.0
        # enc_start is silent when the POI has already been consumed, and our
        # local map keeps the stale poi bit until the next vis disk. Cap the
        # attempts per hex so a stale bit cannot become an infinite loop.
        self._poi_tries: dict[tuple[int, int], int] = {}
        # Content metrics -- ContentMax is judged on these rather than score,
        # since it will structurally lose a score race.
        self.stats = {"encounters_opened": 0, "encounters_banked": 0,
                      "encounters_aborted": 0, "rolls": 0, "rolls_won": 0,
                      "nodes_seen": set(), "recipes": 0, "downed": 0}

    # --- event plumbing -------------------------------------------------
    def on_event(self, ev):
        # ev goes to every client via ws.textAll(), so without this filter a
        # bot counts all six survivors' rolls as its own.
        if self.pid >= 0 and ev.get("pid") not in (None, self.pid):
            return
        k = ev.get("k")
        if k == "enc_res":
            self.stats["rolls"] += 1
            if ev.get("out"):
                self.stats["rolls_won"] += 1
            if ev.get("rec"):
                self.stats["recipes"] += 1
            if self.run is not None:
                self.run.on_result(ev)
                if self.run.node_key:
                    self.stats["nodes_seen"].add(
                        f"{self.run.biome}/{self.run.eid}/{self.run.node_key}")
                if ev.get("ends"):
                    self.run = None
        elif k == "enc_bank":
            self.stats["encounters_banked"] += 1
            self.run = None
        elif k == "enc_end":
            self.run = None
        elif k == "downed":
            self.stats["downed"] += 1

    def content_score(self) -> dict:
        s = dict(self.stats)
        s["nodes_seen"] = len(self.stats["nodes_seen"])
        return s

    # --- main entry -----------------------------------------------------
    def decide(self, obs) -> Action:
        me = obs.me
        if not me.connected or me.ll == 0:
            return Action("noop", why="downed or disconnected")

        if obs.encounter is not None:
            return self.decide_encounter(obs)

        act = self.survival_action(obs)
        if act is not None:
            return act

        # Standing on an unopened POI: take it, whatever the policy is.
        # Walking past one is pure waste -- POIs are consumed permanently, so
        # the alternative is leaving it for a rival -- and exercising the
        # encounter content is the whole point of these runs.
        if self.engage_encounters:
            me = obs.me
            here = obs.map[(me.q, me.r)]
            if here is not None and here.poi and me.mp > 0:
                opened = self.try_open_poi(obs, "POI underfoot")
                if opened is not None:
                    return opened

        return self.pursue(obs)

    # --- survival floor -------------------------------------------------
    def survival_action(self, obs) -> Action | None:
        me = obs.me
        cell = obs.map[(me.q, me.r)]
        terr = cell.terrain if cell else 0

        # Harvest right here first -- always cheaper than walking.
        if me.inv[RES_WATER] < WATER_FLOOR and has_water(terr) and me.mp >= 1:
            return Action("act", a=ACT_WATER, mp=min(2, me.mp),
                          why=f"water {me.inv[RES_WATER]} low")
        if me.inv[RES_FOOD] < FOOD_FLOOR and can_forage(terr) and me.mp >= 2:
            return Action("act", a=ACT_FORAGE,
                          why=f"food {me.inv[RES_FOOD]} low")

        # Out of staples: go and get some while there is still MP to do it
        # with.  This deliberately outranks resting -- dawn consumes 1 food
        # and 2 water whether you moved or not, so resting through a shortage
        # makes it strictly worse.
        hunt = self.staple_hunt(obs)
        if hunt is not None:
            return hunt

        # Out of MP: resting both heals at dawn and, when every connected
        # player is resting, ends the day immediately (tickGame).
        if me.mp <= 0:
            return self.rest_once(obs, "out of MP")

        if me.ll <= self.rest_below_ll:
            return self.rest_once(obs, f"LL {me.ll} critical")

        return None

    def staple_hunt(self, obs) -> Action | None:
        """Walk toward water or food when critically short of either.

        Water is the hard one: ACT_WATER needs Marsh, Flooded or River, about
        4.7% of the map once the raft-gated River is excluded, so most water
        has to come from piles.  Searches much further than normal pursuit
        because dying of thirst two hexes from a pond is the failure mode this
        exists to prevent.
        """
        me = obs.me
        dry = me.inv[RES_WATER] <= EMERGENCY_WATER
        starving = me.inv[RES_FOOD] <= EMERGENCY_FOOD
        if not (dry or starving) or me.mp <= 0:
            return None
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(cell, q, r, cost):
            if cost <= 0 or cell is None:
                return None
            v = 0.0
            if dry:
                if cell.resource == RES_WATER + 1:
                    v += 100.0
                if has_water(cell.terrain):
                    v += 70.0
            if starving:
                if cell.resource == RES_FOOD + 1:
                    v += 80.0
                if can_forage(cell.terrain):
                    v += 40.0
            return None if v <= 0 else v / cost

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=STAPLE_SEARCH_COST)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, val = target
        need = "water" if dry else "food"
        return Action("move", d=d, why=f"EMERGENCY {need} -> ({q},{r}) c={cost}")

    def rest_once(self, obs, why: str) -> Action | None:
        """REST, with a cooldown rather than a once-per-day latch.

        Two failure modes to thread between.

        Re-sending REST every decision cycle is a no-op server-side (doRest
        returns early when already resting) but burns a message each time --
        2054 dead sends in one early run.

        A strict once-per-day latch is worse.  The decision is latched before
        the send clears the rate limiter, so a REST decided just before a
        reconnect never reaches the board while the bot still believes it
        rested.  It then sits at mp == 0, *not* resting, unable to move and
        unable to retry until dawn -- and because tickGame() only ends the day
        early when every connected player is resting, one bot stuck like that
        stops the whole fleet's day from collapsing.  Observed live: five bots
        frozen at day 16, days reverting from ~3 s to the full 5 minutes.

        There is no way to confirm it landed: the periodic broadcast carries no
        `rt` field, so "resting" and "simply out of MP" look identical.  So
        retry on a cooldown instead.  REST is idempotent, and one send every
        few seconds is 60x less traffic than the original spam while being
        self-healing.
        """
        if obs.me.resting:
            return None
        now = time.monotonic()
        if (self._rested_day == obs.day
                and now - self._last_rest_sent < REST_RETRY_S):
            return None
        self._rested_day = obs.day
        self._last_rest_sent = now
        return Action("act", a=ACT_REST, why=why)

    def pack_full(self, obs) -> bool:
        me = obs.me
        return me.carried() >= max(0, me.inv_slots - self.pack_headroom)

    def try_open_poi(self, obs, why: str) -> Action | None:
        """Send enc_start for the hex underfoot, at most a few times.

        A consumed POI makes enc_start a silent no-op while our cached map
        still shows the bit set, so this needs its own attempt cap rather than
        trusting the map.
        """
        me = obs.me
        key = (me.q, me.r)
        tries = self._poi_tries.get(key, 0)
        if tries >= 2:
            return None
        self._poi_tries[key] = tries + 1
        # q/r are required and are validated against our own position.
        return Action("enc_start", q=me.q, r=me.r, why=why)

    # --- encounters -----------------------------------------------------
    def ensure_run(self, obs) -> None:
        """Bind the open encounter to its local JSON the first time we see it."""
        enc = obs.encounter or {}
        biome, eid = enc.get("biome"), enc.get("id")
        if biome is None or eid is None:
            return
        if self.run is None or (self.run.biome, self.run.eid) != (biome, eid):
            self.run = EncounterRun(self.lib, biome, eid)
            self.stats["encounters_opened"] += 1
            if self.run.node_key:
                self.stats["nodes_seen"].add(f"{biome}/{eid}/{self.run.node_key}")

    def decide_encounter(self, obs) -> Action:
        self.ensure_run(obs)
        run = self.run
        if run is None or not run.enc:
            # No local copy (new content, or a biome path we do not know).
            # Banking keeps whatever was already won rather than gambling blind.
            return Action("enc_bank", why="unknown encounter, bank out")

        # Drive straight down the first branch until the node is bankable,
        # then take the haul.  Deliberately simple, and deliberately not
        # odds-weighted: the point of these runs is to find out whether the
        # encounter content is reachable and what it pays, and an
        # odds-weighted policy that walks away from anything risky answers a
        # different question while never finishing a scene.  Aborting also
        # consumes the POI for good (restorePoi=false), so it is pure loss.
        if run.can_bank():
            run.banked = True
            return Action("enc_bank", why=f"bankable at node {run.node_key}")

        choices = run.choices
        if not choices:
            # Terminal node with nothing to take: nothing to do but leave.
            self.stats["encounters_aborted"] += 1
            return Action("enc_abort", why=f"dead end at node {run.node_key}")

        run.choose(0)
        p = success_chance(choices[0], obs)
        return Action("enc_choice", ci=0,
                      why=f"first option at {run.node_key} (p={p:.2f})")

    # --- subclass hook --------------------------------------------------
    def pursue(self, obs) -> Action:
        raise NotImplementedError
