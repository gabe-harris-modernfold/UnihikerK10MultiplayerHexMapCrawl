"""Biker -- wants a Motorbike, and then all the fuel it can carry.

The Motorbike is a starter recipe (recipes.cfg id 20): everyone knows how to
build one at any Settlement, for 7 scrap + 1 fuel + 1 Sparky Bits.  What makes
it hard is carrying the parts there.  Resource tokens count against the pack
one per slot (tokenLoad() vs effectiveInvSlots()), so the build's 8 tokens fill
a bare 8-slot pack with no room for the food and water to get there.
In practice that means a Backpack first, which is the order a player has to
work out for themselves.  Nothing anywhere checks "is wearing a Backpack";
the pack size is the whole gate.

So the Biker plays three phases.  They are re-read off the observation every
cycle, never latched, so a death (the bike stays on the grave) or a free bike
from Jack's Chopper just moves it to the right one:

  pack   the pack cannot carry the build plus a dawn's food and water: craft a
         Backpack (5 food) and wear it.  A Mule's 12 slots skip this, and so
         does a Hoarder's Rig.
  parts  gather the build: the Sparky Bits first, from POI loot ranked by
         how much of each terrain's pool can pay them
         (encounters.item_drop_terrains), then the scrap from piles and
         salvage, and one fuel.  Food and water are kept to a dawn's worth
         while that is all the room there is, and anything past the build
         (KEEP_*) is junk.  With it all carried, craft_action() walks to the
         nearest Settlement and builds it, and gear_action() puts it on.
  ride   the bike is on: drive, and fill every slot the survival floor does
         not need with fuel.  Fuel piles first, fresh ground second.  The
         dawn takes one fuel for the +5 MP, so a full tank keeps it moving.

The canteen errand every other survivor runs is off: the canteen goes in the
body slot, which is where the Backpack has to stay.
"""
import time
from functools import lru_cache

from config import (ACT_FORAGE, ACT_SCAV, ACT_TREAT, BIKE_ITEM, BIKE_RECIPE,
                    NAR_COLD_IMMUNE, NAR_SCAV_DOUBLE, PACK_ITEM, PACK_RECIPE,
                    RECIPES, RES_FOOD, RES_FUEL, RES_MED, RES_SCRAP,
                    RES_WATER, TERR_SETTLEMENT, can_forage, can_salvage,
                    recipe_known)
from encounters import item_drop_terrains
from navigate import best_target, frontier_bonus, step_cost
from .base import Action
from .survivor import (CRAFT_SEARCH_COST, FOOD_FLOOR, WATER_FLOOR,
                       SurvivorPolicy, ends_journey, token_room)

# Food and water kept beside the build: one dawn's worth (1 food + 2 water).
# The pack has to hold the build's tokens AND this, which is what makes a bare
# 8-slot pack too small.
BUILD_RESERVE = 3
# The floors while that reserve is all the room there is.
LEAN_WATER, LEAN_FOOD = 2, 1
# Path values, per MP.
SETTLEMENT_VALUE = 200   # the build is carried: somewhere to craft it
MAT_POI_VALUE = 120      # a scene that can pay the Sparky Bits, x its terrain's share
MAT_GROUND_VALUE = 110   # a build material lying on the ground (usually a grave)
PILE_VALUE = {RES_FUEL: 80, RES_SCRAP: 60, RES_FOOD: 60}   # a pile the build wants
SALVAGE_VALUE = 30       # scrap by SCAV: 2 MP and a check
FORAGE_VALUE = 25        # food for the Backpack by FORAGE
STAPLE_BONUS = 14
NEW_HEX_VALUE = 1.5
FRONTIER_WEIGHT = 1.0
SEARCH_COST = 45
RIDE_SEARCH_COST = 60    # +5 MP a day buys a longer look
# How far to look for a Settlement with the build carried.  craft_action()
# only walks to one inside CRAFT_SEARCH_COST; past that, and with none seen,
# drive() goes looking -- the first mock run's Mule sat on a finished build
# for 150 game-days because none was inside 40 MP.
READY_SEARCH_COST = 120
# Seconds before re-sending a TREAT the board has not answered by changing
# anything.  Keyed on LL and medicine too, so one that lands (LL up, a
# medicine gone) can be followed straight away.  Without it a refusal the bot
# did not predict repeats once a cycle forever: the mock's tire-track bit made
# a Settlement read as no terrain at all, and it sent 700 TREATs in 3 minutes.
TREAT_RETRY_S = 20.0
# Extra MP a route pays to step on a pile of something the Biker cannot use.
# Stepping on a pile takes it -- there is no leaving it -- and a pack that
# fills with scrap it has no use for has no room for fuel or water: the mock
# Mule was riding round with 15 scrap and nothing to drink.  A detour, never
# a wall.
JUNK_PILE_COST = 3
# The most of each kind worth carrying beyond what the phase needs: a couple
# of scrap for shelters, one medicine for a heal, and -- until there is a
# bike to burn it -- one spare fuel for its first dawn.  Past that it is junk:
# trimmed at the bank (enc_bank keep[]), routed round on the ground, and put
# down (drop_res) when the pack needs the room.  Before any of this the mock
# Quartermaster sat on 11 fuel it could not burn, with no room left for the
# scrap the bike needed, for 100 game-days.
KEEP_SCRAP, KEEP_MED, KEEP_FUEL = 2, 1, 1
# Seconds before re-sending an identical drop_res that changed nothing.
DROP_RETRY_S = 10.0


@lru_cache(maxsize=None)
def _drop_share(item_id: int) -> dict:
    return item_drop_terrains(item_id)


def _build_cost(rid) -> int:
    rec = RECIPES.get(rid)
    return sum(rec["cost"]) if rec else 0


class BikerPolicy(SurvivorPolicy):
    """Builds a Motorbike (Backpack first), then rides it for fuel."""

    name = "biker"
    engage_encounters = True
    craft_canteen = False
    min_success = 0.40
    rest_below_ll = 2
    pack_headroom = 0
    # Carry space and mobility are the whole plan. Slots outrank everything so
    # the Backpack (or a Hoarder's Rig) is never swapped out of the body slot.
    gear_weights = {
        "slots": 4.0, "mp": 3.0, "ll": 1.0, "vision": 1.0, "rad": 0.5,
        "threat": 0.25, "terrain": 0.5, "water_cap": 0.5,
        "nar": {NAR_COLD_IMMUNE: 1.0, NAR_SCAV_DOUBLE: 1.0},
    }

    def __init__(self, rng, library=None):
        super().__init__(rng, library)
        # Read by arena.py's end-of-run block. bikes counts ones we built,
        # bikes_found ones that arrived some other way (Jack, a grave).
        # kickstarts: days the bike stalled at dawn and started later on the
        # first fuel found (ev "refuel"). That fuel can burn before a cycle
        # ever sees it in the pack, so fuel_gained may miss it.
        self.bike = {"phase": "pack", "backpacks": 0, "bikes": 0,
                     "bikes_found": 0, "pack_day": None, "bike_day": None,
                     "fuel": 0, "fuel_peak": 0, "fuel_gained": 0,
                     "ride_moves": 0, "kickstarts": 0}
        self._crafting = None       # recipe id of the last CRAFT we sent
        self._owned = {}            # item id -> owned at the last cycle
        self._fuel_seen = None
        self._treat_sent = {}       # (q, r, ll, med) -> monotonic time
        self._drop_sent = {}        # (res, held) -> monotonic time

    # --- the plan -------------------------------------------------------
    def build_tokens(self) -> int:
        return _build_cost(BIKE_RECIPE)

    @staticmethod
    def held(me, item) -> int:
        return sum(q for t, q in zip(me.inv_type, me.inv_qty) if t == item)

    def phase(self, obs) -> str:
        me = obs.me
        if BIKE_RECIPE is None or self.owns(me, BIKE_ITEM):
            return "ride"
        if me.inv_slots < self.build_tokens() + BUILD_RESERVE:
            return "pack"
        return "parts"

    def plan(self, obs, ph) -> tuple[dict, dict, int, int]:
        """What this phase still wants, and the survival floors that leave room
        for it: ({res idx: tokens}, {item id: count}, water floor, food floor).

        The floors are sized to the room actually left once everything else
        in the pack -- scrap, fuel, stray medicine -- and everything still to
        gather is counted: the default 4 water + 2 food where that fits, a
        lean dawn's worth where it does not.
        """
        me, inv = obs.me, obs.me.inv
        if ph == "ride":
            wf, ff = WATER_FLOOR, FOOD_FLOOR
            spare = (token_room(me) - max(0, wf - inv[RES_WATER])
                     - max(0, ff - inv[RES_FOOD]))
            return ({RES_FUEL: spare} if spare > 0 else {}), {}, wf, ff
        other = inv[RES_FUEL] + inv[RES_MED] + inv[RES_SCRAP]
        if ph == "pack":
            food = 0
            if PACK_RECIPE and not self.owns(me, PACK_ITEM):
                food = RECIPES[PACK_RECIPE]["cost"][RES_FOOD]
            wf, ff = self._floors(me.inv_slots - other - food)
            want = max(0, food + ff - inv[RES_FOOD]) if food else 0
            return ({RES_FOOD: want} if want else {}), {}, wf, ff
        rec = RECIPES[BIKE_RECIPE]
        mats = {i: n - self.held(me, i) for i, n in rec["mats"] if n > self.held(me, i)}
        toks = {k: c - inv[k] for k, c in enumerate(rec["cost"]) if c > inv[k]}
        # The part first.  Hauling the scrap around while hunting a POI for it
        # leaves room for one dawn of water, and the first run spent 90
        # game-days walking between ponds with the scrap on its back.
        if mats:
            toks.pop(RES_SCRAP, None)
        wf, ff = self._floors(me.inv_slots - other - sum(toks.values()))
        return toks, mats, wf, ff

    @staticmethod
    def _floors(room) -> tuple[int, int]:
        """(water, food) floors for `room` tokens of space for them."""
        if room >= WATER_FLOOR + FOOD_FLOOR:
            return WATER_FLOOR, FOOD_FLOOR
        return max(LEAN_WATER, min(WATER_FLOOR, room - LEAN_FOOD)), LEAN_FOOD

    # --- main entry -----------------------------------------------------
    def decide(self, obs) -> Action:
        me = obs.me
        act = None
        if me.connected and me.ll > 0:
            ph = self.phase(obs)
            _, _, self.water_floor, self.food_floor = self.plan(obs, ph)
            self._book(obs, ph)
            if obs.encounter is None:
                act = self.shed_junk(obs, ph)
        if act is None:
            act = super().decide(obs)
        if act.kind == "move" and self.bike["phase"] == "ride":
            self.bike["ride_moves"] += 1
        return act

    def _book(self, obs, ph) -> None:
        me, b = obs.me, self.bike
        b["phase"] = ph
        for item, rid, key, day in ((PACK_ITEM, PACK_RECIPE, "backpacks", "pack_day"),
                                    (BIKE_ITEM, BIKE_RECIPE, "bikes", "bike_day")):
            if not item:
                continue
            have = self.owns(me, item)
            if have and not self._owned.get(item):
                if self._crafting == rid:
                    b[key] += 1
                    self._crafting = None
                elif item == BIKE_ITEM:
                    b["bikes_found"] += 1
                if b[day] is None:
                    b[day] = obs.day
            self._owned[item] = have
        fuel = me.inv[RES_FUEL]
        if self._fuel_seen is not None and fuel > self._fuel_seen:
            b["fuel_gained"] += fuel - self._fuel_seen
        self._fuel_seen = fuel
        b["fuel"] = fuel
        b["fuel_peak"] = max(b["fuel_peak"], fuel)

    def content_score(self) -> dict:
        s = super().content_score()
        s["bike"] = dict(self.bike)
        return s

    def on_event(self, ev):
        super().on_event(ev)
        if ev.get("k") == "refuel" and ev.get("pid") == self.pid:
            self.bike["kickstarts"] += 1

    # --- crafting: the Backpack, then the bike --------------------------
    def craft_action(self, obs, max_cost=CRAFT_SEARCH_COST) -> Action | None:
        """With the next build's inputs carried, walk to a Settlement and
        craft it.  Replaces the canteen errand: gathering the inputs is
        pursue()'s job, after the survival floor."""
        me = obs.me
        if obs.underground or me.mp < 1:
            return None
        ph = self.phase(obs)
        if ph == "pack" and PACK_RECIPE and not self.owns(me, PACK_ITEM):
            rid, label = PACK_RECIPE, "backpack"
        elif ph == "parts":
            rid, label = BIKE_RECIPE, "bike"
        else:
            return None
        if not recipe_known(me.known_recipes, rid) or not self.can_afford(me, rid):
            return None
        # Ours is lying on a grave we can still reach: recover_action() walks
        # back for it rather than spending the parts on a second one.
        if self.recovery_target(obs, item=RECIPES[rid]["output"]) is not None:
            return None
        inv = "/".join(str(n) for n in me.inv)
        act = self.craft_at_settlement(obs, rid, max_cost, label,
                                       f"craft {label} (inv {inv})")
        if act is not None and act.kind == "act":
            self._crafting = rid
        return act

    # --- pursuit ----------------------------------------------------------
    def pursue(self, obs) -> Action:
        ph = self.phase(obs)
        toks, mats, _, _ = self.plan(obs, ph)
        act = self.pickup_mats(obs, mats)
        if act is not None:
            return act
        act = self.treat_here(obs, ph)
        if act is not None:
            return act
        act = self.harvest_here(obs, ph, toks)
        if act is not None:
            return act
        return self.drive(obs, ph, toks, mats)

    def pickup_mats(self, obs, mats) -> Action | None:
        """A build material lying underfoot -- a grave's, usually."""
        me = obs.me
        if not mats or me.depth:
            return None
        for gi in obs.ground_items:
            if gi.get("id") not in mats or (gi.get("q"), gi.get("r")) != (me.q, me.r):
                continue
            if not self.pack_has_slot(me) and gi.get("id") not in me.inv_type:
                continue
            act = self._pickup(gi, "bike: pick up part {id} underfoot".format(id=gi["id"]))
            if act is not None:
                return act
        return None

    def keeps(self, ph) -> dict:
        """{res idx: the most worth carrying} for the kinds that can be junk.
        Fuel has no ceiling once it is riding: that is the whole point."""
        if ph == "ride":
            return {RES_SCRAP: KEEP_SCRAP, RES_MED: KEEP_MED}
        cost = RECIPES[BIKE_RECIPE]["cost"] if ph == "parts" else [0] * 5
        return {RES_SCRAP: cost[RES_SCRAP] + (1 if ph == "parts" else KEEP_SCRAP),
                RES_MED: KEEP_MED,
                RES_FUEL: cost[RES_FUEL] + KEEP_FUEL}

    def junk(self, obs, ph, toks) -> set:
        """Token kinds a pile of which would only push us past keeps()."""
        me = obs.me
        return {k for k, cap in self.keeps(ph).items() if me.inv[k] >= cap}

    def shed_junk(self, obs, ph) -> Action | None:
        """Put down whatever is past keeps() when the pack needs the room --
        for the build's tokens and the survival floor's before the bike, for
        one more fuel after it.  Free: no MP, no check, and it lands as a pile
        on this hex (routed round from here on, being junk)."""
        me = obs.me
        if me.depth or me.in_encounter:
            return None
        toks, _, wf, ff = self.plan(obs, ph)
        staples = max(0, wf - me.inv[RES_WATER]) + max(0, ff - me.inv[RES_FOOD])
        if ph == "ride":
            short = 1 if not toks else 0            # no room for one more fuel
        else:
            short = sum(toks.values()) + staples - token_room(me)
        if short <= 0:
            return None
        now = time.monotonic()
        for k in (RES_MED, RES_SCRAP, RES_FUEL):
            cap = self.keeps(ph).get(k)
            if cap is None or me.inv[k] <= cap:
                continue
            key = (k, me.inv[k])
            if now - self._drop_sent.get(key, -DROP_RETRY_S) < DROP_RETRY_S:
                continue
            self._drop_sent[key] = now
            n = me.inv[k] - cap
            return Action("drop_res", res=k + 1, qty=n,
                          why=f"{ph}: dropping {n} {('water', 'food', 'fuel', 'med', 'scrap')[k]}"
                              f" for room (inv {'/'.join(str(x) for x in me.inv)})")
        return None

    def decide_encounter(self, obs) -> Action:
        """The shared scene driver, with the haul trimmed at the bank to what
        the plan can use -- enc_bank's keep[] is clamped server-side against
        the real haul, so 99 means "all of it"."""
        act = super().decide_encounter(obs)
        if act.kind != "enc_bank" or act.keep:
            return act
        me = obs.me
        ph = self.phase(obs)
        keep = [99] * 5
        for k, cap in self.keeps(ph).items():
            keep[k] = max(0, cap - me.inv[k])
        act.keep = keep
        return act

    def spare_scrap(self, obs, ph) -> int:
        """Scrap this phase has no use for."""
        scrap = obs.me.inv[RES_SCRAP]
        if ph == "parts":
            return max(0, scrap - RECIPES[BIKE_RECIPE]["cost"][RES_SCRAP])
        return scrap

    def wants_shelter(self, obs) -> bool:
        """Spare scrap goes on a roof: 1 scrap is a night with no LL loss at
        all (dawnUpkeep's shelter clause) and a pack slot back for water.
        Only on the day's last MP, though -- shelter_here() also fires at any
        MP once LL is low, and a Biker rides on off the roof it just built."""
        return (self.shelter_when_exposed
                or (obs.me.mp <= 1 and self.spare_scrap(obs, self.phase(obs)) > 0))

    def treat_here(self, obs, ph) -> Action | None:
        """In a Settlement with medicine and LL to recover: TREAT, which there
        is the heal (1 med + 1 MP = +1 LL, no roll) or, with a major wound,
        the stitch-up (2 MP and an Endure check).  Before the bike is built
        the token it frees matters as much as the LL -- a spawn kit's two
        medicine is two slots the build cannot use."""
        me = obs.me
        cell = obs.here()
        if cell is None or cell.terrain != TERR_SETTLEMENT or me.inv[RES_MED] < 1:
            return None
        major = len(me.wounds) > 1 and me.wounds[1] > 0
        if not major and me.ll >= me.ll_cap:
            return None
        cost = 2 if major else 1
        if me.mp < cost:
            return None
        key, now = (me.q, me.r, me.ll, me.inv[RES_MED]), time.monotonic()
        if now - self._treat_sent.get(key, -TREAT_RETRY_S) < TREAT_RETRY_S:
            return None
        self._treat_sent[key] = now
        return Action("act", a=ACT_TREAT, mp=cost,
                      why=f"{ph}: treat at a Settlement (LL {me.ll}, med {me.inv[RES_MED]})")

    def harvest_here(self, obs, ph, toks) -> Action | None:
        """FORAGE for the Backpack's food, SCAV for the bike's scrap.  Both
        cost 2 MP; stood on the right ground with less than that, rest and do
        it at dawn -- walking off to the next-best spot and back is what a
        value search does otherwise, and it paced two hexes for 50 days."""
        me = obs.me
        cell = obs.here()
        if cell is None:
            return None
        forage = bool(toks.get(RES_FOOD)) and ph == "pack" and can_forage(cell.terrain)
        salvage = (bool(toks.get(RES_SCRAP)) and can_salvage(cell.terrain)
                   and token_room(me, RES_SCRAP) > 0)
        if (forage or salvage) and me.mp < 2:
            return self.rest_once(obs, f"{ph}: waiting for dawn to harvest here")
        if forage:
            return Action("act", a=ACT_FORAGE,
                          why=f"backpack: foraging (food {me.inv[RES_FOOD]})")
        if salvage:
            return Action("act", a=ACT_SCAV, mp=2,
                          why=f"bike: salvaging scrap ({me.inv[RES_SCRAP]} held)")
        return None

    def drive(self, obs, ph, toks, mats) -> Action:
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")
        room = token_room(me)
        # Everything for the next build is carried: head for a Settlement.
        ready = ((ph == "parts" and not toks and not mats)
                 or (ph == "pack" and PACK_RECIPE and not toks
                     and not self.owns(me, PACK_ITEM)))
        need_water = me.inv[RES_WATER] < self.water_floor
        need_food = me.inv[RES_FOOD] < self.food_floor
        share = {}
        for item in mats:
            for terr, s in _drop_share(item).items():
                share[terr] = max(share.get(terr, 0.0), s)
        ground = {}
        if mats and (self.pack_has_slot(me) or any(i in me.inv_type for i in mats)):
            for gi in obs.ground_items:
                if gi.get("id") in mats and gi.get("q") is not None:
                    ground[(gi["q"], gi["r"])] = MAT_GROUND_VALUE

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = ground.get((q, r), 0.0)
            if cell is None:
                v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
                return v / cost
            res = cell.resource - 1 if cell.resource else -1
            if res >= 0 and room > 0:
                if toks.get(res):
                    v += PILE_VALUE.get(res, 20)
                elif (res == RES_WATER and need_water) or (res == RES_FOOD and need_food):
                    v += STAPLE_BONUS
            # A scene that can pay what the build is missing. Two tries a hex,
            # the same cap try_open_poi() keeps, or a consumed POI whose bit
            # our map still shows would draw us back to it forever.
            if (cell.poi and share.get(cell.terrain)
                    and self._poi_tries.get((0, q, r), 0) < 2):
                v += MAT_POI_VALUE * share[cell.terrain]
            if toks.get(RES_SCRAP) and room > 0 and can_salvage(cell.terrain):
                v += SALVAGE_VALUE
            if toks.get(RES_FOOD) and ph == "pack" and can_forage(cell.terrain):
                v += FORAGE_VALUE
            if ready and cell.terrain == TERR_SETTLEMENT:
                v += SETTLEMENT_VALUE
            if not cell.visited_by(obs.pid):
                v += NEW_HEX_VALUE
            v += FRONTIER_WEIGHT * frontier_bonus(obs.map, q, r)
            return None if v <= 0 else v / cost

        junk = self.junk(obs, ph, toks)

        def cost_fn(cell):
            c = step_cost(cell)
            if (c is not None and cell is not None and cell.resource
                    and cell.resource - 1 in junk):
                c += JUNK_PILE_COST
            return c

        reach = (READY_SEARCH_COST if ready
                 else RIDE_SEARCH_COST if ph == "ride" else SEARCH_COST)
        target = best_target(obs.map, me.q, me.r, value, max_cost=reach,
                             cost_fn=cost_fn, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d, why=f"{ph}: -> ({q},{r}) cost={cost} v={val:.1f}")
        return Action("move", d=self.rng.choice(legal), why=f"{ph}: fallback step")
