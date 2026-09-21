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

from config import (ACT_FORAGE, ACT_REST, ACT_SHELTER, ACT_WATER, EQUIPMENT,
                    EQUIP_STATS, INV_SLOTS_MAX, NAR_COLD_IMMUNE,
                    NAR_FIRE_STARTER, NAR_LAND_FORAGE, NAR_RIVER_FORAGE,
                    NAR_SCAV_DOUBLE, RES_FOOD, RES_SCRAP, RES_WATER,
                    ascend_cost, can_forage, gear_score, has_water,
                    is_exposed, is_hatch_terrain)
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
# How far to look for a way out when a surface policy falls down a hatch.
# The whole tunnel board is 16x10 and a corridor step costs 2 MP, so this
# reaches across all of it.
TUNNEL_ESCAPE_COST = 80


def token_room(me) -> int:
    """Spare resource-token capacity — mirrors tokenRoomFor() in the firmware.

    Only SCAVENGE is gated by this: FORAGE and WATER may overfill and pay the
    encumbrance penalty instead. Gating all three is what livelocked the first
    hardware run — four of five bots sat at room 0 asking for water they could
    not hold, 376 refused actions a minute, no MP spent and no way to tell.
    """
    return max(0, me.inv_slots - sum(me.inv))


def ends_journey(cell, q, r) -> bool:
    """A hatch or shaft is enterable but never walked through: the crossing
    happens as the last act of the step that lands on it, so a route that
    merely passes over one puts the survivor on the other board instead.
    Hand this to dijkstra/best_target as `stop_at` on either board."""
    return cell is not None and is_hatch_terrain(cell.terrain)


class SurvivorPolicy(Policy):
    """Base for scoremax / contentmax / coward / rival."""

    # --- knobs subclasses override -------------------------------------
    engage_encounters = True    # open a POI when standing on one
    # Does this policy know what to do underground?  Stepping onto a hatch IS
    # the descent -- there is no control to refuse and no confirmation -- and
    # terrain 12/13 costs 1 MP, which makes it the *cheapest* hex on the
    # surface map, so a pile-chasing pathfinder walks onto one sooner or
    # later.  A policy with no tunnel plan must climb straight back out
    # rather than flail on a board it is not reading.
    tunnel_capable = False
    min_success = 0.45          # do not take a branch below this success odds
    bank_greed = 0.60           # push on past a bankable node below this odds
    rest_below_ll = 2           # REST when LL drops to or below this
    pack_headroom = 1           # keep this many free slots for collecting
    # How deep to keep the pack. Defaults to the module floors; a policy that
    # has to carry a day's supplies somewhere there is nothing to eat raises
    # them, because the floor is also the only thing that makes a bot harvest
    # when it is standing on something harvestable.
    food_floor = FOOD_FLOOR
    water_floor = WATER_FLOOR
    # Build a basic shelter before sleeping on exposed ground.  Off by
    # default so the measured baseline stays comparable with earlier runs;
    # arena.py's --shelter turns it on for the other arm of the experiment.
    # It exists because the first cause-attributed runs put ~67% of all LL
    # lost on exposure, which no policy had any answer to -- measuring that
    # without this arm measures the bot's blind spot, not the game's balance.
    shelter_when_exposed = False

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

        # Put on anything useful we are carrying.  Costs no MP and no time, so
        # it sits above every goal-directed branch.
        act = self.gear_action(obs)
        if act is not None:
            return act

        # Standing on an unopened POI: take it, whatever the policy is.
        # Walking past one is pure waste -- POIs are consumed permanently, so
        # the alternative is leaving it for a rival -- and exercising the
        # encounter content is the whole point of these runs.
        # handleMsg_enc_start() is board-aware, so this works on either board.
        if self.engage_encounters:
            here = obs.here()
            if here is not None and here.poi and obs.me.mp > 0:
                opened = self.try_open_poi(obs, "POI underfoot")
                if opened is not None:
                    return opened

        if obs.underground and not self.tunnel_capable:
            return self.climb_out(obs, why="fell down a hatch")

        return self.pursue(obs)

    # --- equipment ------------------------------------------------------
    # What this policy wants out of a piece of gear.  One weight per stat that
    # items.cfg can carry; `nar` weights the narrative perks by NAR_* id.
    # Subclasses override to the degree they actually play differently -- the
    # point is that a run measures which gear suits which playstyle, not which
    # gear happened to drop first.  gear_score() in config.py does the maths.
    #
    # The default is the survival floor this class already encodes: staying
    # alive first, then the mobility to go fix a problem.
    gear_weights = {
        "ll": 3.0, "rad": 1.5, "mp": 1.5, "slots": 1.0,
        "vision": 1.0, "threat": 0.5, "terrain": 0.5,
        "nar": {NAR_COLD_IMMUNE: 4.0, NAR_FIRE_STARTER: 2.0,
                NAR_LAND_FORAGE: 1.5, NAR_RIVER_FORAGE: 0.5,
                NAR_SCAV_DOUBLE: 1.0},
    }

    def gear_swap_ok(self, obs, worn_id, cand_id) -> bool:
        """Would equipItem() actually accept this swap?

        The orphan guard refuses a swap that shrinks the pack around occupied
        slots.  Asking first matters: gear_action runs every cycle, so a swap
        the server will always refuse is an infinite retry at one message per
        cycle that never looks like an error from either end.
        """
        me = obs.me
        delta = (EQUIP_STATS.get(cand_id, {}).get("slots", 0)
                 - EQUIP_STATS.get(worn_id, {}).get("slots", 0))
        if delta >= 0:
            return True
        new_slots = max(1, min(INV_SLOTS_MAX, me.inv_slots + delta))
        # The incoming item vacates its own slot, so it is exempt -- same
        # allowance equipItem() makes.
        return not any(t for i, t in enumerate(me.inv_type)
                       if i >= new_slots and t != cand_id)

    def gear_action(self, obs):
        """Pick up equipment underfoot, then put on anything carried whose
        slot is still empty.

        The pickup half matters more than it looks: grantItemOrDrop() puts
        loot that does not fit the pack on the ground instead, and until the
        bots could pick it up again that gear was gone for good -- a full pack
        at the wrong moment silently deleted the drop from the experiment.

        Deliberately conservative: it fills empty slots and never swaps.  A
        swap needs a value judgement between two items, and getting that wrong
        is worse than wearing the first one found -- equipItem() also returns
        the displaced item to the pack, so a bad swap policy can thrash a slot
        forever, one message per cycle, and never notice.

        Nothing here existed before.  The bots parsed eq[] out of every state
        message and never sent a single equip_item, so every number in
        docs/bot-testing.md was measured on a survivor wearing nothing at all:
        no +LL ceiling, no vision, no carry slots, no fuel-gated MP.
        """
        me = obs.me
        if me.in_encounter:
            return None

        # Equipment lying on this hex, and room to take it. pickupGroundItem()
        # refuses a different hex and a pack with no space, so check both here
        # rather than firing a message that can only be refused.
        carried = sum(1 for t in me.inv_type if t)
        if carried < me.inv_slots:
            for gi in obs.ground_items:
                if gi.get("id") not in EQUIPMENT:
                    continue
                if (gi.get("q"), gi.get("r")) != (me.q, me.r) or me.depth:
                    continue
                return Action("pickup_item", gslot=gi.get("g", 0),
                              why=f"pick up gear {gi['id']} underfoot")

        # Best carried candidate per equip slot, against whatever is worn.
        # Strictly greater, so equal-scoring items never trade places -- a tie
        # that swapped would put the loser straight back in the pack and swap
        # again next cycle, forever.
        best = None
        for slot, item_id in enumerate(me.inv_type):
            if not item_id:
                continue
            eslot = EQUIPMENT.get(item_id)
            if eslot is None:
                continue
            worn = me.equip[eslot]
            gain = gear_score(item_id, self.gear_weights) - gear_score(worn, self.gear_weights)
            if gain <= 0:
                continue
            if worn and not self.gear_swap_ok(obs, worn, item_id):
                continue
            if best is None or gain > best[0]:
                best = (gain, slot, item_id, eslot, worn)
        if best is None:
            return None
        _, slot, item_id, eslot, worn = best
        verb = f"swap {worn} for" if worn else "equip"
        return Action("equip_item", slot=slot,
                      why=f"{verb} item {item_id} -> slot {eslot}")

    # --- survival floor -------------------------------------------------
    def survival_action(self, obs) -> Action | None:
        me = obs.me
        # obs.here() rather than obs.map[(me.q, me.r)]: underground those
        # coordinates still name the surface hatch, so the unqualified read
        # decides whether to forage based on a hex 30 metres overhead.
        cell = obs.here()
        terr = cell.terrain if cell else 0

        # Harvest right here first -- always cheaper than walking.
        if me.inv[RES_WATER] < self.water_floor and has_water(terr) and me.mp >= 1:
            return Action("act", a=ACT_WATER, mp=min(2, me.mp),
                          why=f"water {me.inv[RES_WATER]} low")
        if me.inv[RES_FOOD] < self.food_floor and can_forage(terr) and me.mp >= 2:
            return Action("act", a=ACT_FORAGE,
                          why=f"food {me.inv[RES_FOOD]} low")

        # About to sleep on exposed ground: 1 scrap and 1 MP buys a basic
        # shelter, and dawnUpkeep does more than cancel the exposure hit --
        # "resting in shelter suppresses all LL losses" zeroes llDelta
        # outright, thirst and hunger included.  It pays 4 points on top and
        # cannot fail.
        #
        # Deliberately ahead of the staple hunt: on the last MP of a day, one
        # more step cannot reach water (most terrain costs 2-4 MP to enter)
        # but the shelter cancels the whole night's loss, so building strictly
        # dominates walking there.  The hunt still outranks it on any earlier
        # move, which is where it actually saves a survivor.
        shelter = self.shelter_here(obs)
        if shelter is not None:
            return shelter

        # Out of staples: go and get some while there is still MP to do it
        # with.  This deliberately outranks resting -- dawn consumes 1 food
        # and 2 water whether you moved or not, so resting through a shortage
        # makes it strictly worse.
        hunt = self.staple_hunt(obs)
        if hunt is not None:
            return hunt

        # Out of MP: resting both heals at dawn and, when every connected
        # player is resting, ends the day immediately (tickGame).  This holds
        # underground too now -- REST is no longer refused at depth 1, so a
        # bot down a hole still pulls its weight in sprint mode instead of
        # holding the whole fleet's day open.
        if me.mp <= 0:
            return self.rest_once(obs, "out of MP underground"
                                       if obs.underground else "out of MP")

        # At critical LL the two boards want opposite things.  On the surface
        # a rest is free upside.  Underground it rolls TUNNEL_REST_LL_PCT for
        # bad air, and that point is netted against the rest-heal -- so it is
        # harmless while we still have the food and water to heal with, and it
        # is the thing that kills us when we do not.  Rest below only when the
        # heal will actually fire (survival_state.hpp: F>=2 and W>=2).
        if me.ll <= self.rest_below_ll:
            if not obs.underground:
                return self.rest_once(obs, f"LL {me.ll} critical")
            if me.food >= 2 and me.water >= 2:
                return self.rest_once(
                    obs, f"LL {me.ll} critical, stocked enough to out-heal bad air")

        return None

    def shelter_here(self, obs) -> Action | None:
        """Build a basic shelter if this is where the day is going to end.

        doShelter() takes 1 scrap and 1 MP and always succeeds -- there is no
        check to fail -- so the only judgement is *when*.  Firing it on the
        last MP of the day is what a player does: earlier wastes a move that
        could have been a step, later is a dawn too late.
        """
        me = obs.me
        if not self.shelter_when_exposed:
            return None
        # SHELTER is one of the three actions still refused at depth 1, and
        # the exposure check it answers is off underground anyway -- a
        # corridor is already cover.
        if obs.underground:
            return None
        # Archetype 5 (Endurer) needs no shelter -- dawnUpkeep exempts it, so
        # building one is a wasted scrap.
        if me.archetype == 5:
            return None
        if me.mp < 1 or me.inv[RES_SCRAP] < 1:
            return None
        # Only on the last move of the day, or when already hurt enough that
        # another exposure tick is the difference.
        if me.mp > 1 and me.ll > self.rest_below_ll:
            return None
        cell = obs.here()
        if cell is None or cell.shelter > 0 or not is_exposed(cell.terrain):
            return None
        return Action("act", a=ACT_SHELTER,
                      why=f"exposed at SV<2, {me.inv[RES_SCRAP]} scrap, mp={me.mp}")

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

        # Hunt on whichever board we are on.  Underground that means Tunnel
        # Floor cistern seeps and water piles only -- TERRAIN_FORAGE_DN[14] is
        # 0, so there is no food down there at all, which is exactly why the
        # tunnel policies have to surface for it.
        my_q, my_r = obs.pos()
        target = best_target(obs.board, my_q, my_r, value,
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
        # No depth guard: REST works at depth 1. It is not free down there --
        # dawnUpkeep rolls TUNNEL_REST_LL_PCT for bad air -- but that is the
        # caller's judgement, not this sender's, and the caller that cares
        # makes it (the critical-LL branch in emergency()).
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
        q, r = obs.pos()
        # Key on the board as well: (3,4) underground and (3,4) on the surface
        # are different hexes, and a shared attempt counter would let a dead
        # tunnel POI veto a live surface one.
        key = (obs.me.depth, q, r)
        tries = self._poi_tries.get(key, 0)
        if tries >= 2:
            return None
        self._poi_tries[key] = tries + 1
        # q/r are required and are validated against our own position --
        # against tq/tr while underground (handleMsg_enc_start is board-aware),
        # which is exactly what obs.pos() returns.
        return Action("enc_start", q=q, r=r, why=why)

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

    # --- crossing back to the surface -----------------------------------
    def climb_out(self, obs, prefer=None, why: str = "climbing out",
                  explore_if_blind: bool = True):
        """Head for a shaft and step onto it.

        Stepping onto a Bunker Entrance or Vent Shaft IS the ascent, so there
        is nothing to send but a move.  Two wrinkles:

        - **You cannot ascend from the shaft you are standing on.** You
          arrived on it by transition, not by a step, and only a fresh step
          onto one crosses the boards. So when we are already on a shaft the
          move is to step OFF it and come back -- which is also why an
          accidental descent costs about 4 MP to undo, not 1.
        - Shafts hang off the corridor as one-hex spurs, so pathing to one
          never routes *through* another and ejects us somewhere we did not
          choose.

        `prefer(cell, q, r, cost)` lets a subclass rank the exits (the tunnel
        runner wants a specific distant one); returning None rejects a shaft
        outright. The default prefers the cheapest exit, with a nudge toward
        Bunker Entrances because a Vent Shaft charges VENT_ASCEND_MP to climb.

        With `explore_if_blind=False` this returns None instead of wandering
        when no acceptable shaft is in sight, so a caller with its own idea of
        where to look -- a tunnel policy that would rather chase a pile while
        it hunts for the exit -- can take over.
        """
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why=f"{why}: no legal move underground")
        q, r = obs.pos()
        here = obs.tunnel[(q, r)]

        if here is not None and is_hatch_terrain(here.terrain):
            # On the shaft already: step off so the next step back on counts.
            return Action("move", d=self.rng.choice(legal),
                          why=f"{why}: stepping off the shaft to re-enter it")

        def value(cell, cq, cr, cost):
            if cell is None or not is_hatch_terrain(cell.terrain):
                return None
            if prefer is not None:
                return prefer(cell, cq, cr, cost)
            return (10.0 - ascend_cost(cell.terrain)) / cost

        target = best_target(obs.tunnel, q, r, value,
                             max_cost=TUNNEL_ESCAPE_COST, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            tq, tr, d, cost, _ = target
            return Action("move", d=d,
                          why=f"{why}: shaft ({tq},{tr}) c={cost}")

        if not explore_if_blind:
            return None

        # No shaft revealed yet -- the vision radius underground is 1, so this
        # is the normal state on the first few steps of a descent. Push into
        # the dark rather than standing still; the corridors are a connected
        # network, so any exploration finds one.
        def explore(cell, cq, cr, cost):
            if cost <= 0:
                return None
            return (3.0 if cell is None else 1.0) / cost

        target = best_target(obs.tunnel, q, r, explore,
                             max_cost=TUNNEL_ESCAPE_COST, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            return Action("move", d=target[2], why=f"{why}: searching for a shaft")
        return Action("move", d=self.rng.choice(legal), why=f"{why}: fallback step")

    # --- subclass hook --------------------------------------------------
    def pursue(self, obs) -> Action:
        raise NotImplementedError
