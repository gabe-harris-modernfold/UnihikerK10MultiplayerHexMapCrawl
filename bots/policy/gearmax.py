"""GearMax -- collects and wears as much equipment as it can find.

Built to answer three questions the other policies cannot, because none of
them accumulates enough gear to ask:

1. **How much equipment can a survivor actually get hold of?**
2. **Does each item's declared benefit or penalty actually land?**
   `bots/gearcheck.py` audits that from this policy's run log, comparing the
   wire's effective `llCap` / `is` / `mp` against what items.cfg promises.
3. **Which items keep a survivor alive?**  Same tool, ranking items by LL
   lost per game-day worn.

## Where equipment comes from

There is exactly one faucet: **encounter loot**.  `data/encounters/loot_tables.json`
deals item ids on a bank, and `grantItemOrDrop()` puts anything that will not
fit the pack on the ground instead.  Ground items are therefore a *secondary*
source -- they only exist because somebody's pack was full -- and crafting
needs a recipe learned from, again, an encounter.

So a gear bot is a POI bot with different priorities: it opens every scene it
can reach and banks, where ContentMax opens scenes to *see* them.  The
difference in weighting is that GearMax will walk a long way for a POI and
will not detour for a resource pile it does not need.

## Wearing junk on purpose

`equip_anything` is what separates this from every other policy.  The shared
`gear_action()` refuses a swap whose `gear_score()` is not strictly positive,
which is right for a bot that wants to survive and wrong for one that exists
to measure whether penalties are applied: an item with a net-negative score
would never be worn, so its penalty would never be observed.  This one fills
an empty slot with whatever it is carrying, good or bad, and only applies the
score comparison when something is already worn.

That makes its own survival numbers unrepresentative -- it is deliberately
wearing things a sane player would leave in the pack -- which is the point.
The *items* are being measured here, not the policy.
"""
from config import EQUIPMENT, RES_FOOD, RES_WATER, TERR_SETTLEMENT
from navigate import best_target, frontier_bonus
from .base import Action
from .survivor import SurvivorPolicy, ends_journey

POI_VALUE = 90          # the only real source of items; dominates everything
GROUND_ITEM_VALUE = 70  # loot that did not fit somebody's pack, or a grave's gear
GEAR_GROUND_BONUS = 40  # ...and it is equipment (the only ground piles targeted)
SETTLEMENT_VALUE = 12   # CRAFT needs one, and a recipe learned from a POI
PILE_VALUE = 8          # tokens matter only as far as staying alive
STAPLE_BONUS = 14
NEW_HEX_VALUE = 2
FRONTIER_WEIGHT = 1.0
SEARCH_COST = 55        # a POI is worth a long walk


class GearMaxPolicy(SurvivorPolicy):
    """Hunts encounters for loot, wears everything it finds."""

    name = "gearmax"
    engage_encounters = True
    # Push deeper than a cautious bot: the loot is at the bankable node, and
    # a scene abandoned early pays nothing at all.
    min_success = 0.30
    bank_greed = 0.40
    rest_below_ll = 3
    # Keep the pack roomier than usual. invType[] is capped at
    # effectiveInvSlots too, and grantItemOrDrop() dumps anything that will
    # not fit on the ground -- a full pack at the wrong moment turns a drop
    # into litter the bot then has to walk back for.
    pack_headroom = 3
    # Wear it even if it is bad for us. See the module docstring.
    equip_anything = True
    # Near-flat weights: this policy is not trying to find the *best* loadout,
    # it is trying to sample as many items as possible for long enough to
    # measure them. Steeper weights would make it re-swap toward the same few
    # favourites and leave most of the registry untested.
    gear_weights = {
        "ll": 1.0, "rad": 0.5, "mp": 0.5, "slots": 0.5,
        "vision": 0.5, "threat": 0.25, "terrain": 0.25, "water_cap": 0.5,
        "nar": {},
    }

    def pursue(self, obs) -> Action:
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        # Ground items are keyed by hex, and there are few of them, so this
        # is a dict lookup rather than a scan inside the value function.
        # Only piles gear_action() will actually lift -- equipment, with a
        # free slot to put it in.  A consumable pile used to be worth the
        # walk too, so the bot arrived, took nothing, left, and was drawn
        # straight back from the next hex; harmless while ground items were
        # rare overflow, a loop once every fall started littering its grave.
        loot = {}
        if self.pack_has_slot(me):
            for gi in obs.ground_items:
                q, r, iid = gi.get("q"), gi.get("r"), gi.get("id")
                if q is None or iid not in EQUIPMENT:
                    continue
                loot[(q, r)] = GROUND_ITEM_VALUE + GEAR_GROUND_BONUS

        need_water = me.inv[RES_WATER] < 4
        need_food = me.inv[RES_FOOD] < 2
        full = self.pack_full(obs)

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = loot.get((q, r), 0.0)
            if cell is None:
                v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
            else:
                if cell.poi:
                    v += POI_VALUE
                if cell.terrain == TERR_SETTLEMENT:
                    v += SETTLEMENT_VALUE
                if cell.resource and not full:
                    v += PILE_VALUE
                    if need_water and cell.resource == RES_WATER + 1:
                        v += STAPLE_BONUS
                    if need_food and cell.resource == RES_FOOD + 1:
                        v += STAPLE_BONUS
                if not cell.visited_by(obs.pid):
                    v += NEW_HEX_VALUE
                v += FRONTIER_WEIGHT * frontier_bonus(obs.board, q, r)
            return None if v <= 0 else v / cost

        # stop_at keeps the route off hatches: a step onto one crosses boards
        # as the last act of the move, and there is no loot underground --
        # generateTunnels() deals POIs from the "14" pool, which is empty.
        target = best_target(obs.board, *obs.pos(), value,
                             max_cost=SEARCH_COST, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d,
                          why=f"-> ({q},{r}) cost={cost} v={val:.1f}")
        return Action("move", d=self.rng.choice(legal), why="fallback step")
