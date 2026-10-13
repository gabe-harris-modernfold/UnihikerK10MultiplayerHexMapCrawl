"""The item half of SurvivorPolicy: using, buying, crafting, trading and
throwing away.  A mixin, so survivor.py stays about staying alive.

What it adds to every SurvivorPolicy (docs/bot-testing.md, "Items"):

* **use_item** -- consumables when they pay (items.choose_consumable: a
  critical LL, a wound, a track below a threshold it fell through, a
  radiation dose, a dawn with no tokens, LL that fits under the ceiling, AP
  when there is somewhere it must get to), a threat reducer mid-scene when it
  drops the clock a tier, and the fog-lifting items where the fog is.
* **drop_item** -- litter, when the pack has no free slot.
* **car_buy** -- consumables off the caravan shelf with tokens the survivor
  can spare, when it is within a short walk.
* **act CRAFT** -- any learned recipe whose output it would use or wear,
  when it is at (or near) a Settlement and can spare the inputs.
* **trade_accept / trade_decline** -- every offer made to it, on its merits.

Every sender is rate-limited per target the same way the equip and pickup
senders are (base.Throttle, on the policy's clock): a refusal changes
nothing, so an unguarded rule re-sends the same message every cycle forever.
"""
from config import (RECIPES, RES_FOOD, RES_WATER, recipe_known, MAP_COLS,
                    MAP_ROWS, is_hatch_terrain)
import items as IT
from navigate import dijkstra
from .base import Action

USE_RETRY_S = 4.0          # same (slot, item) again
USE_GAP_S = 1.0            # any two uses: let the item_result land first
DROP_RETRY_S = 5.0
TRADE_REPLY_RETRY_S = 4.0
CARAVAN_RETRY_S = 6.0
CARAVAN_BACKOFF_S = 45.0   # after a refusal we did not predict
CARAVAN_SEARCH_COST = 12   # how far a survivor walks to go shopping
CRAFT_EXTRA_COST = 10      # how far it walks to a Settlement to craft extras
REVEAL_RETRY_S = 20.0
# Use a fog-lifting item where at least this share of its disk is unseen.
REVEAL_FOG_SHARE = 0.4


def _disk(q0, r0, radius):
    for dq in range(-radius, radius + 1):
        for dr in range(max(-radius, -dq - radius), min(radius, -dq + radius) + 1):
            yield (q0 + dq) % MAP_COLS, (r0 + dr) % MAP_ROWS


class ItemUseMixin:
    # Knobs.  `use_items` switches the whole layer off (a baseline arm);
    # `shop` and `craft_extras` the two errands that walk.
    use_items = True
    shop = True
    craft_extras = True
    reply_trades = True

    def _items_init(self):
        # Cooldowns: self.throttle keys "use", ("use", slot, item), ("drop",
        # slot, item), ("trade", pid), "caravan" and "reveal".
        self._whole_map = None      # id() of the map the Net Map was read on
        self.stats.update({"items_used": {}, "junk_dropped": 0, "caravan_buys": 0,
                           "crafted_extra": {}, "trades_accepted": 0,
                           "trades_declined": 0, "reveals": 0, "threat_items": 0})

    # -- hooks a subclass may override ------------------------------------------
    def protected_items(self, obs) -> set:
        """Items never dropped, sold or used up: quest items the journal
        still needs (survivor.py adds them)."""
        return set()

    def wants_ap(self, obs) -> bool:
        """Spend an AP item at 0 AP?  Only to save a survivor that is out of
        a staple -- the staple hunt is the one walk that cannot wait a dawn."""
        me = obs.me
        return me.inv[RES_WATER] <= 1 or me.inv[RES_FOOD] <= 0

    def token_reserve(self, obs) -> list:
        """Tokens (inv[] order) never spent on the caravan or a craft."""
        me = obs.me
        scrap = 3 if getattr(self, "wants_canteen", lambda o: False)(obs) else 1
        return [99, self.food_floor + 2, 1, 1, scrap]

    def craft_also_wanted(self, obs, rid) -> bool:
        """Craft `rid` even when items.craft_wanted says no (the quester's
        workshop: every recipe made once)."""
        return False

    # -- sending ------------------------------------------------------------------
    def _use(self, slot, item, why, stat="items_used") -> Action | None:
        th = self.throttle
        if not (th.ready("use", USE_GAP_S) and th.ready(("use", slot, item), USE_RETRY_S)):
            return None
        th.mark("use")
        th.mark(("use", slot, item))
        if stat == "items_used":
            used = self.stats["items_used"]
            used[item] = used.get(item, 0) + 1
        else:
            self.stats[stat] += 1
        return Action("use_item", slot=slot, why=why)

    # -- the layer ---------------------------------------------------------------
    def item_action(self, obs) -> Action | None:
        """Free actions first: a consumable that pays now, then litter out of
        a full pack.  No AP, no walking."""
        if not self.use_items:
            return None
        me = obs.me
        if me.ll <= 0 or not me.connected:
            return None
        prot = self.protected_items(obs)
        pick = IT.choose_consumable(me, want_ap=self.wants_ap(obs), protected=prot)
        if pick is not None:
            act = self._use(*pick)
            if act is not None:
                return act
        if not obs.underground and IT.free_slots(me) <= 0 and not me.in_encounter:
            junk = IT.junk_slot(me, prot)
            if junk is not None:
                slot, iid = junk
                if self.throttle.ok(("drop", slot, iid), DROP_RETRY_S):
                    self.stats["junk_dropped"] += 1
                    return Action("drop_item", slot=slot, qty=me.inv_qty[slot] or 1,
                                  why=f"pack full: dropping the {IT.name(iid)}")
        return None

    def encounter_item_action(self, obs, run) -> Action | None:
        """Mid-scene, before a roll: a threat reducer that drops the clock a
        tier (computeEncounterDN reads it), or a heal on the last LL.  Items
        work inside a scene -- handleMsg_use_item has no encounter check."""
        if not self.use_items or not run.choices:
            return None
        me = obs.me
        prot = self.protected_items(obs)
        if me.ll <= 2:
            pick = IT.choose_consumable(me, protected=prot)
            if pick is not None and "critical" in pick[2]:
                return self._use(*pick)
        pick = IT.choose_threat_item(me, obs.threat, prot)
        if pick is not None:
            return self._use(*pick, stat="threat_items")
        return None

    def reveal_action(self, obs) -> Action | None:
        """Read the map items where they show something: the Net Map once
        per world (it reveals all of it), the reusable Diary and Pendulum,
        and the one-shot Static Chew / Bright Bad Idea, wherever enough of
        their disk is still fog."""
        if not self.use_items or obs.underground:
            return None
        me = obs.me
        if not self.throttle.ready("reveal", REVEAL_RETRY_S):
            return None
        for slot, iid, radius in sorted(IT.reveal_items(me), key=lambda x: -x[2]):
            fx = IT.fx_of(iid)
            if radius >= 99:
                if self._whole_map == id(obs.map):
                    continue
                self._whole_map = id(obs.map)
            else:
                if fx["threat"] > 0 and obs.threat + fx["threat"] >= 5:
                    continue        # a tier of threat is not worth a peek
                cells = list(_disk(me.q, me.r, radius))
                fog = sum(1 for qr in cells if obs.map[qr] is None)
                if fog < REVEAL_FOG_SHARE * len(cells):
                    continue
                if not fx["key"] and fog < 0.6 * len(cells):
                    continue        # a one-shot wants a real frontier
            self.throttle.mark("reveal")
            act = self._use(slot, iid, f"lifting the fog: {IT.name(iid)} (r{radius})",
                            stat="reveals")
            if act is not None:
                return act
        return None

    def trade_reply_action(self, obs) -> Action | None:
        """Answer every offer made to us: accept a fair one, decline the
        rest.  Before this existed a rival's offers just lapsed, and
        handleMsg_trade_accept / _decline were never exercised by a bot."""
        if not self.reply_trades or obs.me.in_encounter:
            return None
        me = obs.me
        floors = [self.water_floor, self.food_floor, 0, 0, 0]
        for frm, offer in list(obs.trade_offers_live().items()):
            if not self.throttle.ok(("trade", frm), TRADE_REPLY_RETRY_S):
                continue
            if IT.offer_is_fair(me, offer, floors):
                self.stats["trades_accepted"] += 1
                return Action("trade_accept", frm=frm,
                              why=f"fair offer from {frm}: get {offer['give']} for {offer['want']}")
            self.stats["trades_declined"] += 1
            return Action("trade_decline", frm=frm,
                          why=f"declining {frm}: get {offer['give']} for {offer['want']}")
        return None

    def _spare(self, obs) -> list:
        res = self.token_reserve(obs)
        inv = obs.me.inv
        spare = [max(0, inv[i] - res[i]) for i in range(5)]
        spare[RES_WATER] = 0
        return spare

    def caravan_action(self, obs, max_cost=CARAVAN_SEARCH_COST) -> Action | None:
        """Buy off the caravan shelf: on its hex, car_buy; within `max_cost`
        AP of it and with something worth buying, walk there."""
        if not (self.use_items and self.shop) or obs.underground or obs.me.in_encounter:
            return None
        w = obs.world
        if not w.caravan_active or w.caravan_q < 0:
            return None
        # "caravan" is also held for CARAVAN_BACKOFF_S after a refusal we did
        # not predict (throttle.hold).
        if not self.throttle.ready("caravan", CARAVAN_RETRY_S):
            return None
        me = obs.me
        pick = IT.caravan_pick(me, w.caravan_stock, self._spare(obs))
        if pick is None:
            return None
        item, n, give = pick
        if (me.q, me.r) == (w.caravan_q, w.caravan_r):
            self.throttle.mark("caravan")
            return Action("car_buy", item=item, n=n, give=give,
                          why=f"caravan: {IT.name(item)} for {give}")
        if me.ap < 1:
            return None
        dist, first = dijkstra(obs.map, me.q, me.r, max_cost=max_cost,
                               stop_at=lambda c, q, r: c is not None and is_hatch_terrain(c.terrain))
        cost = dist.get((w.caravan_q, w.caravan_r))
        d = first.get((w.caravan_q, w.caravan_r), -1)
        if cost is None or d not in me.legal_dirs():
            return None
        return Action("move", d=d, why=f"caravan at ({w.caravan_q},{w.caravan_r}) c={cost}: "
                                       f"wants the {IT.name(item)}")

    def craftable_extras(self, obs) -> list:
        """Known recipes we could make right now and want, best first:
        equipment ahead of consumables."""
        me = obs.me
        reserve = self.token_reserve(obs)
        prot = self.protected_items(obs)
        out = []
        for rid, rec in RECIPES.items():
            if rec.get("starter") or not recipe_known(me.known_recipes, rid):
                continue
            if not (IT.craft_wanted(me, rid, self.gear_weights, prot)
                    or self.craft_also_wanted(obs, rid)):
                continue
            if any(me.inv[i] - c < (reserve[i] if i != RES_WATER else self.water_floor)
                   for i, c in enumerate(rec["cost"]) if c):
                continue
            if not self.can_afford(me, rid) or not self.result_fits(me, rid):
                continue
            # Never eat a quest item as a material.
            if any(item in prot for item, _q in rec["mats"]):
                continue
            gear = IT.ITEM_CATEGORY.get(rec["output"]) == "equipment"
            # A recipe the policy wants for its own sake (the quester's
            # workshop: never made yet) goes ahead of one it merely likes the
            # output of -- the two often share a material, and the Field
            # Trauma Patch ate the bandages' rag six times before the first
            # bandage was ever made.
            out.append((0 if self.craft_also_wanted(obs, rid) else 1,
                        0 if gear else 1, -IT.consumable_worth(rec["output"]), rid))
        return [rid for *_k, rid in sorted(out)]

    def craft_extra_action(self, obs, max_cost=CRAFT_EXTRA_COST) -> Action | None:
        """Craft the best extra we can at a Settlement, walking up to
        `max_cost` to one.  craft_at_settlement() owns the cooldown and the
        surface / AP / room checks."""
        if not (self.use_items and self.craft_extras) or obs.underground:
            return None
        if obs.me.in_encounter or self.in_emergency(obs):
            return None
        rids = self.craftable_extras(obs)
        if not rids:
            return None
        rid = rids[0]
        out = RECIPES[rid]["output"]
        act = self.craft_at_settlement(obs, rid, max_cost, "craft",
                                       f"craft {RECIPES[rid]['name']} -> {IT.name(out)}")
        if act is not None and act.kind == "act":
            made = self.stats["crafted_extra"]
            made[rid] = made.get(rid, 0) + 1
        return act
