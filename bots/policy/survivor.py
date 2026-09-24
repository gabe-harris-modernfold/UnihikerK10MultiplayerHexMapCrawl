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

from config import (ACT_CRAFT, ACT_FORAGE, ACT_REST, ACT_SCAV, ACT_SHELTER,
                    ACT_WATER, can_salvage,
                    CANTEEN_ITEM, CANTEEN_RECIPE, EQUIPMENT, RECIPES,
                    TERR_SETTLEMENT, recipe_known,
                    EQUIP_STATS, INV_SLOTS_MAX, NAR_COLD_IMMUNE,
                    NAR_FIRE_STARTER, NAR_LAND_FORAGE, NAR_RIVER_FORAGE,
                    NAR_SCAV_DOUBLE, RES_FOOD, RES_SCRAP, RES_WATER,
                    ascend_cost, can_forage, gear_score, ground_days_left,
                    has_water, is_exposed, is_hatch_terrain)
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
# How far a survivor will walk to a Settlement to craft its canteen.  The
# errand runs only once the survival floor is quiet, and there are ~29
# Settlements on the 4275-hex map, so this is roughly "the nearest one we
# have seen, if it is not across the world".
CRAFT_SEARCH_COST = 40
# Seconds to stop asking after a craft that did not land.  doCraft() refuses
# with an err toast and no state change, so without this a refusal the bot
# did not predict (a full item grid, a recipe the board does not have) would
# repeat once a cycle forever -- the same shape as the stale-POI loop.
CRAFT_RETRY_S = 20.0
# Seconds before re-sending an identical equip_item.  equipItem() can refuse
# silently, and a socket the board has stopped servicing swallows it; with no
# cooldown gear_action() re-sent one swap 60 times in 25 s on 2026-09-23.
EQUIP_RETRY_S = 5.0
# The same guard for the two ground messages.  pickupGroundItem() and
# lootRemains() both refuse without changing anything -- a pile someone lifted
# a moment ago, tokens a rival looted first -- and until the ground_update that
# corrects our view arrives, the decision that sent them comes straight back.
PICKUP_RETRY_S = 5.0
LOOT_RETRY_S = 5.0
# Death drops (dropRemains): a fall leaves the whole pack on the hex, and the
# survivor who comes back for it is a fresh one.  How far it will walk: the
# same reach as the canteen errand, since the canteen it would otherwise
# rebuild from three scrap is usually lying on the grave.
RECOVER_SEARCH_COST = 40
# MP a day of walking is assumed to cover when asking whether a grave will
# still be there on arrival.  effectiveMP() is 6 + (ll+1)/2 -- about 10 fresh --
# so this errs short, which errs toward not setting off for a grave that ages
# out on the way.
RECOVER_MP_PER_DAY = 8
# Scrap-errand values: a pile is collected just by stepping on it; salvage
# terrain needs a 2 MP SCAV and a check, so it is worth less per MP.
SCRAP_PILE_VALUE = 100.0
SALVAGE_VALUE = 60.0
# How far to look for a way out when a surface policy falls down a hatch.
# The whole tunnel board is 16x10 and a corridor step costs 2 MP, so this
# reaches across all of it.
TUNNEL_ESCAPE_COST = 80


def token_room(me, res: int = -1) -> int:
    """Spare resource-token capacity — mirrors tokenRoomFor() in the firmware.

    `res` is the inv[] index being added.  Water also gets whatever canteen
    space is still empty: that sits outside the pack, so it is room even in a
    full or overfull one.  me.carried() is already tokenLoad().

    Only SCAVENGE is gated by this: FORAGE and WATER may overfill and pay the
    encumbrance penalty instead. Gating all three is what livelocked the first
    hardware run — four of five bots sat at room 0 asking for water they could
    not hold, 376 refused actions a minute, no MP spent and no way to tell.
    """
    room = max(0, me.inv_slots - me.carried())
    if res == RES_WATER:
        room += max(0, me.water_cap - me.inv[RES_WATER])
    return room


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
    # Every survivor gets a canteen and keeps it on: gather the scrap, craft
    # it at a Settlement, wear it, and never swap it back out.  Thirst was
    # every single death in the realtime runs of 2026-09-23, and the canteen
    # is the one piece of kit that answers it that anyone can make.  It
    # deliberately overrides gear_weights -- a policy that prefers the
    # Backpack's slots gives them up for the water.
    craft_canteen = True

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
                      "nodes_seen": set(), "recipes": 0, "downed": 0,
                      "crafted": 0,
                      # Death drops, as decided (not as acked): loot messages
                      # sent, and steps taken walking back to a grave of ours.
                      "loots": 0, "recover_moves": 0}
        self._craft_sent = 0.0      # monotonic time of the last CRAFT we sent
        self._equip_sent: dict[tuple[int, int], float] = {}   # (slot, item) -> t
        self._pickup_sent: dict[tuple[int, int], float] = {}  # (gslot, item) -> t
        self._loot_sent: dict[tuple[int, int], float] = {}    # (q, r) -> t
        self._had_canteen = False

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

        # Count a canteen the moment it appears.  Here rather than in
        # craft_action(), which gear_action() pre-empts on the very cycle
        # the crafted item lands in the pack.
        have = self.owns(me, CANTEEN_ITEM)
        if have and not self._had_canteen and self._craft_sent:
            self.stats["crafted"] += 1
        self._had_canteen = have

        if obs.encounter is not None:
            return self.decide_encounter(obs)

        # Stood on a grave: what is on it is free (no MP, no check) and ours
        # for the taking, so take it before any errand walks us off the hex.
        act = self.remains_action(obs)
        if act is not None:
            return act

        # A canteen within today's MP outranks topping up the pack.  The top-up
        # floor is often above what the economy sustains, so on hardware it
        # ate every MP of every day and a survivor sat on a pond 4 hexes from a
        # Settlement for ~100 s holding the scrap (2026-09-23).  Emergencies
        # still win -- this only pre-empts the routine harvest.  A grave of our
        # own within today's MP goes first: the canteen is usually lying on it.
        if not self.in_emergency(obs):
            act = self.recover_action(obs, max_cost=obs.me.mp)
            if act is not None:
                return act
            act = self.craft_action(obs, max_cost=obs.me.mp)
            if act is not None:
                return act

        act = self.survival_action(obs)
        if act is not None:
            return act

        # Put on anything useful we are carrying.  Costs no MP and no time, so
        # it sits above every goal-directed branch.
        act = self.gear_action(obs)
        if act is not None:
            return act

        # The walk back for our own pack, once the survival floor is quiet.
        # Ahead of the canteen errand, which stands down while this has a
        # grave to go to (see craft_action).
        act = self.recover_action(obs)
        if act is not None:
            return act

        # The canteen errand: after the survival floor (never walk off to
        # craft while dying of thirst) and after gear (a crafted canteen is
        # equipped by gear_action on the very next cycle).
        act = self.craft_action(obs)
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
        "vision": 1.0, "threat": 0.5, "terrain": 0.5, "water_cap": 3.0,
        "nar": {NAR_COLD_IMMUNE: 4.0, NAR_FIRE_STARTER: 2.0,
                NAR_LAND_FORAGE: 1.5, NAR_RIVER_FORAGE: 0.5,
                NAR_SCAV_DOUBLE: 1.0},
    }

    # Put an item in an empty slot even when gear_score() says it is not
    # worth wearing. Off by default -- a survival policy should leave a
    # net-negative item in the pack -- but a policy built to measure whether
    # penalties are actually applied has to be willing to wear one, or the
    # penalty is never observed. Only affects EMPTY slots; a swap still has
    # to justify itself.
    equip_anything = False

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

        act = self.pickup_gear(obs)
        if act is not None:
            return act

        # The canteen goes on first, over whatever holds the body slot, and
        # is pinned there: the loop below never proposes a swap out of it.
        # Without the pin a Backpack-preferring policy would take it straight
        # back off, and equipItem() returning the displaced item to the pack
        # would make the pair trade places once a cycle forever.
        ceslot = EQUIPMENT.get(CANTEEN_ITEM) if CANTEEN_ITEM else None
        if (self.craft_canteen and ceslot is not None
                and me.equip[ceslot] != CANTEEN_ITEM
                and CANTEEN_ITEM in me.inv_type):
            slot = me.inv_type.index(CANTEEN_ITEM)
            worn = me.equip[ceslot]
            if not worn or self.gear_swap_ok(obs, worn, CANTEEN_ITEM):
                verb = f"swap {worn} for" if worn else "equip"
                return self._equip(slot, CANTEEN_ITEM,
                                   f"{verb} canteen -> slot {ceslot}")

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
            if self.craft_canteen and worn and worn == CANTEEN_ITEM:
                continue                    # pinned
            gain = gear_score(item_id, self.gear_weights) - gear_score(worn, self.gear_weights)
            if gain <= 0 and not (self.equip_anything and not worn):
                continue
            if worn and not self.gear_swap_ok(obs, worn, item_id):
                continue
            if best is None or gain > best[0]:
                best = (gain, slot, item_id, eslot, worn)
        if best is None:
            return None
        _, slot, item_id, eslot, worn = best
        verb = f"swap {worn} for" if worn else "equip"
        return self._equip(slot, item_id, f"{verb} item {item_id} -> slot {eslot}")

    @staticmethod
    def pack_has_slot(me) -> bool:
        """A free typed slot -- what pickupGroundItem() needs for an item that
        does not stack onto one already carried."""
        return sum(1 for t in me.inv_type if t) < me.inv_slots

    def pickup_gear(self, obs, why="pick up gear {id} underfoot") -> Action | None:
        """Equipment lying on this hex, and room to take it.

        pickupGroundItem() refuses a different hex, a pack with no space, and
        (like every ground pile) a survivor underground, whose q/r are pinned
        to the hatch overhead -- so check all three here rather than firing a
        message that can only be refused.  Non-equipment is left where it is:
        only RivalPolicy wants litter.
        """
        me = obs.me
        if me.depth or not self.pack_has_slot(me):
            return None
        for gi in obs.ground_items:
            if gi.get("id") not in EQUIPMENT:
                continue
            if (gi.get("q"), gi.get("r")) != (me.q, me.r):
                continue
            act = self._pickup(gi, why.format(id=gi["id"]))
            if act is not None:
                return act
        return None

    def _pickup(self, gi, why) -> Action | None:
        """pickup_item for ground pile `gi`, at most once per PICKUP_RETRY_S
        for the same (gslot, item) -- see PICKUP_RETRY_S."""
        key = (gi.get("g", 0), gi.get("id"))
        now = time.monotonic()
        if now - self._pickup_sent.get(key, -PICKUP_RETRY_S) < PICKUP_RETRY_S:
            return None
        self._pickup_sent[key] = now
        return Action("pickup_item", gslot=gi.get("g", 0), why=why)

    def _equip(self, slot, item_id, why) -> Action | None:
        """equip_item, at most once per EQUIP_RETRY_S for the same (slot,
        item).  A refusal or a dead socket leaves the pack unchanged, so the
        same decision comes back every cycle; this is what stops it being
        re-sent every cycle."""
        now = time.monotonic()
        key = (slot, item_id)
        if now - self._equip_sent.get(key, -EQUIP_RETRY_S) < EQUIP_RETRY_S:
            return None
        self._equip_sent[key] = now
        return Action("equip_item", slot=slot, why=why)

    # --- crafting -------------------------------------------------------
    @staticmethod
    def owns(me, item_id) -> bool:
        return bool(item_id) and (item_id in me.equip or item_id in me.inv_type)

    def wants_canteen(self, obs) -> bool:
        """Known and not already owned.  Not weighed against the body slot:
        every policy wears one (see craft_canteen)."""
        me = obs.me
        if not self.craft_canteen or CANTEEN_RECIPE is None:
            return False
        if not recipe_known(me.known_recipes, CANTEEN_RECIPE):
            return False
        return not self.owns(me, CANTEEN_ITEM)

    @staticmethod
    def can_afford(me, rid) -> bool:
        rec = RECIPES.get(rid)
        if rec is None:
            return False
        if any(me.inv[i] < c for i, c in enumerate(rec["cost"])):
            return False
        for item, qty in rec["mats"]:
            if sum(q for t, q in zip(me.inv_type, me.inv_qty) if t == item) < qty:
                return False
        return True

    def in_emergency(self, obs) -> bool:
        """The states the staple hunt and the critical-LL rest exist for."""
        me = obs.me
        return (me.inv[RES_WATER] <= EMERGENCY_WATER
                or me.inv[RES_FOOD] <= EMERGENCY_FOOD
                or me.ll <= self.rest_below_ll)

    def craft_action(self, obs, max_cost=CRAFT_SEARCH_COST) -> Action | None:
        """Walk to a Settlement and craft the canteen.

        `max_cost` bounds the walk: decide() calls this with today's MP ahead
        of the survival floor, and with CRAFT_SEARCH_COST after it.

        doCraft(): Settlement terrain, 1 MP, the recipe bit, the tokens, and
        item room for the result -- and refused underground outright.  The
        crafted item lands in the pack; gear_action() puts it on.
        """
        me = obs.me
        if obs.underground or me.mp < 1 or not self.wants_canteen(obs):
            return None
        # The canteen we would craft is lying on a grave of ours that we can
        # still reach: walk back for it (recover_action) rather than spend
        # three scrap on a second one.  Only when the grave holds one -- a
        # grave of tokens alone does not make crafting pointless.
        if self.recovery_target(obs, canteen_only=True) is not None:
            return None
        if not self.can_afford(me, CANTEEN_RECIPE):
            return self.scrap_errand(obs, max_cost)
        # A free item slot for the result (applyRecipe's invRoomFor check).
        if sum(1 for t in me.inv_type[:me.inv_slots] if t) >= me.inv_slots:
            return None
        if time.monotonic() - self._craft_sent < CRAFT_RETRY_S:
            return None
        cell = obs.here()
        if cell is not None and cell.terrain == TERR_SETTLEMENT:
            self._craft_sent = time.monotonic()
            return Action("act", a=ACT_CRAFT, recipe=CANTEEN_RECIPE,
                          why=f"craft canteen ({me.inv[RES_SCRAP]} scrap)")
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(c, q, r, cost):
            if cost <= 0 or c is None or c.terrain != TERR_SETTLEMENT:
                return None
            return 100.0 / cost

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=max_cost, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, _ = target
        return Action("move", d=d, why=f"canteen: to settlement ({q},{r}) c={cost}")

    def scrap_errand(self, obs, max_cost) -> Action | None:
        """Short of the canteen's scrap: salvage here, or walk to a scrap pile
        or salvageable ground.  A pile pays on contact; salvage costs 2 MP and
        a check (and a threat tick on ruins).  Both need token room --
        collectResource() and doScav() refuse a full pack."""
        me = obs.me
        if token_room(me, RES_SCRAP) <= 0:
            return None
        cell = obs.here()
        if cell is not None and can_salvage(cell.terrain) and me.mp >= 2:
            return Action("act", a=ACT_SCAV, mp=2,
                          why=f"canteen: salvaging scrap ({me.inv[RES_SCRAP]} held)")
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(c, q, r, cost):
            if cost <= 0 or c is None:
                return None
            if c.resource == RES_SCRAP + 1:
                return SCRAP_PILE_VALUE / cost
            if can_salvage(c.terrain):
                return SALVAGE_VALUE / cost
            return None

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=max_cost, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, _ = target
        return Action("move", d=d, why=f"canteen: scrap -> ({q},{r}) c={cost}")

    # --- death drops ----------------------------------------------------
    # A downed survivor leaves everything it carried on the hex where it fell
    # (dropRemains, inventory_items.hpp): pack and worn gear as ordinary
    # ground piles, resource tokens in a remains record ("rm").  It is anyone's
    # for GROUND_AGE_DAYS.  Before this the bots lost their canteen to every
    # death -- respawn is a fresh survivor -- and had to scrape together the
    # scrap for another; now it waits on the grave.
    @staticmethod
    def room_for_remains(me, rm) -> bool:
        """Some kind of token on the record that the pack could take.
        lootRemains() caps each kind at tokenRoomFor(), so without room a loot
        is refused and changes nothing."""
        res = (rm.get("res") or [])[:5]
        return any(n > 0 and token_room(me, k) > 0 for k, n in enumerate(res))

    @staticmethod
    def is_my_grave(obs, rm) -> bool:
        """Our seat's record, or one a "fell" of ours created -- a respawn
        can land in another seat, and the old pid then names someone else."""
        return (rm.get("pid") == obs.pid
                or (rm.get("q"), rm.get("r")) in obs.my_graves)

    def worth_recovering(self, obs, rm) -> bool:
        """Is there anything on this grave we could actually take?

        Only what the two pickups will lift counts: tokens we have room for,
        and equipment with a free slot to put it in.  Litter the policy would
        leave there anyway is not worth the walk -- counting it would bounce
        a bot between the grave and wherever pursuit wanted it next.
        """
        me = obs.me
        if self.room_for_remains(me, rm):
            return True
        if not self.pack_has_slot(me):
            return False
        at = (rm.get("q"), rm.get("r"))
        return any(gi.get("id") in EQUIPMENT and (gi.get("q"), gi.get("r")) == at
                   for gi in obs.ground_items)

    def remains_action(self, obs) -> Action | None:
        """Stood on a grave: take what is on it before anything moves us off.

        Both halves are free -- no MP, no check -- which is why this runs
        ahead of every errand.  It has to: the canteen errand would otherwise
        walk a respawned survivor off its own grave to hunt scrap for a NEW
        canteen before gear_action() got round to lifting the old one, and
        recover_action() would then walk it straight back.

        Any grave, ours or a rival's.  Tokens first (a bare loot takes what
        fits, in inv[] order, so water first), then equipment.  Never below:
        remains are surface coordinates, and underground me.q/me.r are the
        hatch overhead -- which is exactly where a fall below leaves its grave.
        """
        me = obs.me
        if me.depth or me.in_encounter:
            return None
        rm = obs.remains_at(me.q, me.r)
        if rm is None:
            return None
        if self.room_for_remains(me, rm):
            key = (me.q, me.r)
            now = time.monotonic()
            if now - self._loot_sent.get(key, -LOOT_RETRY_S) >= LOOT_RETRY_S:
                self._loot_sent[key] = now
                self.stats["loots"] += 1
                whose = "our own" if self.is_my_grave(obs, rm) else f"{rm.get('nm') or 'someone'}'s"
                return Action("loot", why=f"loot {whose} remains {rm.get('res')}")
        return self.pickup_gear(obs, why="recover gear {id} from the grave")

    def recovery_target(self, obs, max_cost=RECOVER_SEARCH_COST,
                        canteen_only=False):
        """The grave of ours worth walking back to, as best_target()'s
        (q, r, dir, cost, value), or None.

        A grave counts while it holds something we can take
        (worth_recovering) and will still be there when we arrive: it goes at
        the dawn that makes it GROUND_AGE_DAYS old, and a walk longer than
        today's MP arrives RECOVER_MP_PER_DAY at a time.  Never one on a
        hatch -- stepping onto a hatch IS the descent, so a surface grave on
        one cannot be reached on foot (a fall underground leaves it there).
        `canteen_only` narrows it to graves with the canteen on them.
        """
        me = obs.me
        if me.depth or me.mp < 1 or not obs.remains:
            return None
        graves = {}
        for rm in obs.remains:
            at = (rm.get("q"), rm.get("r"))
            if None in at or at == (me.q, me.r):
                continue                # malformed, or remains_action's job
            if not self.is_my_grave(obs, rm) or not self.worth_recovering(obs, rm):
                continue
            if canteen_only and not any(
                    gi.get("id") == CANTEEN_ITEM and (gi.get("q"), gi.get("r")) == at
                    for gi in obs.ground_items):
                continue
            cell = obs.map[at]
            if cell is not None and is_hatch_terrain(cell.terrain):
                continue
            graves[at] = rm
        if not graves:
            return None
        mp_today = me.mp

        def value(cell, q, r, cost):
            rm = graves.get((q, r))
            if rm is None or cost <= 0:
                return None
            left = ground_days_left(obs.day, rm.get("d", obs.day))
            if cost <= mp_today:
                if left < 1:
                    return None
            else:
                days = -(-(cost - mp_today) // RECOVER_MP_PER_DAY)
                if left <= days + 1:    # a day of slack: MP is an estimate
                    return None
            return 100.0 / cost

        return best_target(obs.map, me.q, me.r, value,
                           max_cost=max_cost, stop_at=ends_journey)

    def recover_action(self, obs, max_cost=RECOVER_SEARCH_COST) -> Action | None:
        """Walk back toward a grave of ours (recovery_target).

        decide() calls this twice, the way it calls craft_action(): with
        today's MP ahead of the survival floor, and with RECOVER_SEARCH_COST
        once the floor is quiet.  Arriving is the whole errand --
        remains_action() takes it from there.
        """
        target = self.recovery_target(obs, max_cost)
        if target is None or target[2] not in obs.me.legal_dirs():
            return None
        q, r, d, cost, _ = target
        self.stats["recover_moves"] += 1
        return Action("move", d=d, why=f"recover: our grave at ({q},{r}) c={cost}")

    # --- survival floor -------------------------------------------------
    def survival_action(self, obs) -> Action | None:
        me = obs.me
        # obs.here() rather than obs.map[(me.q, me.r)]: underground those
        # coordinates still name the surface hatch, so the unqualified read
        # decides whether to forage based on a hex 30 metres overhead.
        cell = obs.here()
        terr = cell.terrain if cell else 0

        # Harvest right here first -- always cheaper than walking.  A worn
        # canteen raises the target by its capacity, but only while there is
        # room to put the water: canteen space is outside the pack, and it is
        # the whole reason to carry one -- stood on a pond with it empty is
        # the moment it pays for itself.
        water_goal = self.water_floor
        if me.water_cap and token_room(me, RES_WATER) > 0:
            water_goal += me.water_cap
        if me.inv[RES_WATER] < water_goal and has_water(terr) and me.mp >= 1:
            why = (f"water {me.inv[RES_WATER]} low" if me.inv[RES_WATER] < self.water_floor
                   else f"filling the canteen ({me.inv[RES_WATER]}/{water_goal})")
            return Action("act", a=ACT_WATER, mp=min(2, me.mp), why=why)
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
            v += self.staple_value(obs, cell, dry, starving)
            return None if v <= 0 else v / cost

        # Hunt on whichever board we are on.  Underground that means Tunnel
        # Floor cistern seeps and water piles only -- TERRAIN_FORAGE_DN[14] is
        # 0, so there is no food down there at all, which is exactly why the
        # tunnel policies have to surface for it.
        my_q, my_r = obs.pos()
        # stop_at: a route *through* a hatch does not exist (it ends on the
        # other board), so an emergency walk must never be planned across one.
        target = best_target(obs.board, my_q, my_r, value,
                             max_cost=STAPLE_SEARCH_COST, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, val = target
        need = "water" if dry else "food"
        return Action("move", d=d, why=f"EMERGENCY {need} -> ({q},{r}) c={cost}")

    def staple_value(self, obs, cell, dry: bool, starving: bool) -> float:
        """Subclass hook: extra worth of `cell` to an emergency staple hunt.
        The tunnel policies count a hatch as water."""
        return 0.0

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
