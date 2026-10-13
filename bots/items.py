"""What a survivor does with the things in its pack.

Until 2026-10-13 no bot ever sent use_item, car_buy, drop_item or a trade
reply, and crafted nothing past the three starter recipes: every Trauma
Patch, ration and wound kit an encounter paid out rode in the pack until the
survivor died with it.  This module is the item knowledge the policies share
-- pure functions of items.cfg / recipes.cfg and a PlayerState, so smoke.py
can check every rule without a board.  The policy layer that sends the
messages (cooldowns, priorities, walking) is policy/itemuse.py.

The rules follow the firmware, not the item text:

* useItem() (inventory_items.hpp) moves the food and water TRACKS, not the
  tokens, one step at a time through applyFStep/applyWStep -- and a step
  back up over a threshold the track fell through gives that LL back.  So a
  ration is worth most when the track is below a threshold it was knocked
  through (fth / wth say which), and nothing at all on a full track.
* LL clamps to llCap and radiation to 0..10; AP is immediate.  Nothing stops
  a wasteful use, so these rules do: nothing is used that would mostly spill.
* Key items with an effect are used any number of times and never spent.
"""
from config import (CARAVAN_PAY_WORTH, EQUIPMENT, ITEM_CATEGORY, ITEM_FX,
                    ITEM_NAME, RECIPES, RES_FOOD, RES_FUEL, RES_MED,
                    RES_SCRAP, RES_WATER, gear_score)

# Track thresholds (survival_skills.hpp): (value, fThreshBelow/wThreshBelow
# bit).  Falling below one costs 1 LL and sets the bit; climbing back over
# it clears the bit and gives the LL back.  Bit 4 is "fell off the floor at
# 1", cleared by any step up to 2.
FOOD_THRESHOLDS = ((4, 1), (2, 2), (2, 4))
WATER_THRESHOLDS = ((5, 1), (3, 2), (2, 4))
TRACK_MAX = 6

# Every recipe input.  A material nothing is made from (the Crater Deed) is
# litter.
MATERIALS = frozenset(item for rec in RECIPES.values() for item, _ in rec["mats"])

# What a resource token is worth to a survivor, before need is counted --
# water first: it is the one that kills (docs/bot-testing.md, cause of death).
TOKEN_WORTH = (3.0, 2.0, 1.0, 2.0, 1.5)    # water food fuel med scrap


def name(iid) -> str:
    return ITEM_NAME.get(iid, f"#{iid}")


def fx_of(iid) -> dict | None:
    return ITEM_FX.get(iid)


def is_useful_fx(fx) -> bool:
    """Does using this do anything a survivor could want?"""
    if not fx:
        return False
    return (fx["ll"] > 0 or fx["food"] > 0 or fx["water"] > 0 or fx["rad"] < 0
            or fx["ap"] > 0 or fx["cure"] > 0 or fx["threat"] < 0 or fx["reveal"] > 0)


def is_junk(iid, protected=()) -> bool:
    """Litter: an item no rule here will ever use, wear or craft with.

    Equipment is never junk (gear_action decides about wearing it), and
    `protected` -- quest items -- never is either, whatever it does: the
    Sticky Note does nothing when used and is the whole point of the Dial-Up
    Cult."""
    if not iid or iid in protected:
        return False
    cat = ITEM_CATEGORY.get(iid)
    if cat == "equipment":
        return False
    if cat == "material":
        return iid not in MATERIALS
    return not is_useful_fx(ITEM_FX.get(iid))


def pack(me):
    """[(slot, item, qty)] for every occupied pack slot."""
    return [(s, t, q) for s, (t, q) in enumerate(zip(me.inv_type, me.inv_qty))
            if t and q > 0]


def held(me, iid) -> int:
    return sum(q for _s, t, q in pack(me) if t == iid)


def free_slots(me) -> int:
    return max(0, me.inv_slots - sum(1 for t in me.inv_type[:me.inv_slots] if t))


def track_regain(track: int, steps: int, bits: int, thresholds) -> int:
    """LL given back by moving a 0-6 track up `steps` (applyFStep's upward
    branch): one per tripped threshold the climb crosses."""
    gain = 0
    for _ in range(max(0, steps)):
        old, track = track, min(TRACK_MAX, track + 1)
        for value, bit in thresholds:
            if old < value <= track and bits & bit:
                bits &= ~bit
                gain += 1
    return gain


def ll_gain(me, fx) -> int:
    """LL this item would add right now: its own ll plus what the food and
    water steps win back, clamped to the ceiling like useItem() clamps it."""
    g = fx["ll"]
    if fx["food"] > 0:
        g += track_regain(me.food, fx["food"], me.f_thresh, FOOD_THRESHOLDS)
    if fx["water"] > 0:
        g += track_regain(me.water, fx["water"], me.w_thresh, WATER_THRESHOLDS)
    return max(0, min(g, me.ll_cap - me.ll))


def _candidates(me, test):
    out = []
    for slot, iid, _q in pack(me):
        fx = ITEM_FX.get(iid)
        if fx and test(iid, fx):
            out.append((slot, iid, fx))
    return out


def choose_consumable(me, want_ap=False, protected=()):
    """(slot, item, why) of the one thing in the pack worth using right now,
    or None.  First match wins, most urgent first.  Never anything that only
    harms (Panic Juice), never the reveal items (reveal_action owns those),
    and never a protected (quest) item."""
    if me.ll <= 0:
        return None

    def ok(iid, fx):
        return iid not in protected and not fx["key"] and fx["reveal"] == 0 \
            and fx["threat"] == 0

    # 1. LL critical: the biggest heal there is, whatever it spills.  The
    #    Uranium Candy lowers the ceiling for good, so only on the last LL.
    if me.ll <= 2:
        heals = [(ll_gain(me, fx), -fx["rad"], s, i) for s, i, fx in _candidates(me, ok)
                 if ll_gain(me, fx) > 0 and (not fx["cap_loss"] or me.ll <= 1)
                 and me.rad + max(0, fx["rad"]) < 9]
        if heals:
            g, _r, s, i = max(heals)
            return s, i, f"LL {me.ll} critical: {name(i)} (+{g})"

    # 2. Wounds: a major one costs an AP every day until it closes.
    minor, major = (list(me.wounds) + [0, 0])[:2]
    if minor + major > 0:
        cures = [(fx["ll"] > me.ll_cap - me.ll, -fx["cure"], s, i)
                 for s, i, fx in _candidates(me, ok) if fx["cure"] > 0]
        if cures:
            _w, _c, s, i = min(cures)
            return s, i, f"wounds {minor}/{major}: {name(i)}"

    # 3. A track below a threshold it fell through: the step back up pays LL.
    regain = []
    for s, i, fx in _candidates(me, ok):
        if fx["cap_loss"] or (fx["rad"] > 0 and me.rad + fx["rad"] > 5):
            continue
        if fx["water"] < 0 and me.water + fx["water"] < 3:
            continue
        if fx["food"] < 0:
            continue
        back = 0
        if fx["food"] > 0:
            back += track_regain(me.food, fx["food"], me.f_thresh, FOOD_THRESHOLDS)
        if fx["water"] > 0:
            back += track_regain(me.water, fx["water"], me.w_thresh, WATER_THRESHOLDS)
        if back:
            spill = max(0, me.food + fx["food"] - TRACK_MAX) + max(0, me.water + fx["water"] - TRACK_MAX)
            regain.append((back, -spill, s, i))
    if regain:
        back, _sp, s, i = max(regain)
        return s, i, f"track below a threshold (F{me.food} W{me.water}): {name(i)} wins back {back} LL"

    # 4. Radiation: 7+ rolls for LL every dusk, 10 always loses it.
    if me.rad >= 5:
        flush = [(fx["rad"], s, i) for s, i, fx in _candidates(me, ok) if fx["rad"] < 0]
        if flush:
            # Big dose: the strongest. Moderate: the weakest that helps.
            r, s, i = min(flush) if me.rad >= 7 else max(flush)
            return s, i, f"radiation {me.rad}: {name(i)} ({r})"

    # 5. A track that will step DOWN at dawn: no tokens left to feed it.
    if me.food < TRACK_MAX and me.inv[RES_FOOD] == 0:
        food = [(-(abs(TRACK_MAX - me.food - fx["food"])), s, i)
                for s, i, fx in _candidates(me, ok)
                if fx["food"] > 0 and fx["food"] <= TRACK_MAX - me.food + 1
                and fx["water"] >= 0 and not fx["cap_loss"]]
        if food:
            _f, s, i = max(food)
            return s, i, f"no food tokens, track {me.food}: {name(i)}"
    if me.water < TRACK_MAX and me.inv[RES_WATER] < 2:
        water = [(-(abs(TRACK_MAX - me.water - fx["water"])), s, i)
                 for s, i, fx in _candidates(me, ok)
                 if fx["water"] > 0 and fx["water"] <= TRACK_MAX - me.water + 1
                 and (fx["ap"] >= 0 or me.ap <= -fx["ap"])]
        if water:
            _w, s, i = max(water)
            return s, i, f"short of water tokens, track {me.water}: {name(i)}"

    # 6. Topping LL up, only when none of it spills over the ceiling.
    if me.ll < me.ll_cap:
        tops = [(fx["ll"], s, i) for s, i, fx in _candidates(me, ok)
                if 0 < fx["ll"] <= me.ll_cap - me.ll and not fx["cap_loss"]
                and me.rad + max(0, fx["rad"]) <= 4
                and fx["food"] >= 0 and fx["water"] >= 0]
        if tops:
            g, s, i = max(tops)
            return s, i, f"LL {me.ll}/{me.ll_cap}: {name(i)} (+{g})"

    # 7. AP, only when the caller has somewhere it must get to.
    if want_ap and me.ap <= 0 and not me.resting:
        boost = [(fx["ap"], s, i) for s, i, fx in _candidates(me, ok) if fx["ap"] > 0
                 and fx["food"] >= 0 and fx["water"] >= 0]
        if boost:
            a, s, i = max(boost)
            return s, i, f"out of AP with somewhere to be: {name(i)} (+{a})"
    return None


def choose_threat_item(me, threat: int, protected=()):
    """(slot, item, why) of a threat reducer that drops the clock below the
    tier it sits on, or None.  computeEncounterDN() adds +5 risk at 5, 10,
    15 and 20 -- so the item is worth using only when it crosses one."""
    if threat < 5:
        return None
    above = threat % 5          # points above the tier boundary
    best = None
    for slot, iid, fx in _candidates(me, lambda i, f: f["threat"] < 0 and not f["key"]
                                     and i not in protected):
        if -fx["threat"] > above and (best is None or fx["threat"] > best[2]["threat"]):
            best = (slot, iid, fx)
    if best is None:
        return None
    slot, iid, fx = best
    return slot, iid, f"threat {threat} -> {threat + fx['threat']}: {name(iid)} drops a tier"


def reveal_items(me):
    """[(slot, item, radius)] for everything that lifts fog on use."""
    return [(s, i, fx["reveal"]) for s, i, fx in _candidates(me, lambda i, f: f["reveal"] > 0)]


def junk_slot(me, protected=()):
    """(slot, item) of the least useful thing to put down, or None."""
    junk = [(s, t) for s, t, _q in pack(me) if is_junk(t, protected)]
    return junk[0] if junk else None


# -- the caravan --------------------------------------------------------------
def pay_for(price: int, spare) -> list | None:
    """give[5] worth at least `price` (CARAVAN_PAY_WORTH) out of `spare`,
    overpaying as little as possible, or None.  Never water: the caravan
    refuses it (why 4).  Overpayment is not refunded."""
    best = None
    for fuel in range(0, spare[RES_FUEL] + 1):
        rest = price - fuel * CARAVAN_PAY_WORTH[RES_FUEL]
        give = [0, 0, fuel, 0, 0]
        for res in (RES_SCRAP, RES_FOOD, RES_MED):    # what a survivor misses least
            take = min(spare[res], max(0, rest))
            give[res] += take
            rest -= take
        if rest > 0:
            continue
        key = (-rest, fuel)             # overpayment first, then fuel kept
        if best is None or key < best[0]:
            best = (key, give)
    return best[1] if best else None


def consumable_worth(iid) -> float:
    """What a consumable is worth to have in the pack, in token-ish units.
    Rough on purpose: it ranks caravan purchases and encounter loot, it does
    not decide when to use anything."""
    fx = ITEM_FX.get(iid)
    if not fx or fx["key"]:
        return 0.0
    w = (2.0 * max(0, fx["ll"]) + 1.0 * max(0, fx["food"]) + 1.3 * max(0, fx["water"])
         + 0.6 * max(0, -fx["rad"]) + 0.8 * max(0, fx["ap"]) + 3.0 * fx["cure"]
         + 0.8 * max(0, -fx["threat"]) + 0.3 * fx["reveal"])
    w -= 1.0 * max(0, fx["rad"]) + 1.0 * max(0, -fx["food"]) + 1.0 * max(0, -fx["water"])
    if fx["cap_loss"]:
        w -= 4.0
    return max(0.0, w)


def caravan_pick(me, stock, spare, want_more=2):
    """(item, n, give) worth buying off the caravan shelf, or None.

    `stock` is world.caravan_stock -- [[id, qty, price], ...] -- and `spare`
    the tokens the policy can do without.  Buys one of whatever it values
    most per token that it holds fewer than `want_more` of, paid as cheaply
    as the change allows.  Needs a free pack slot (the shelf's items stack,
    but not reliably onto a slot we have)."""
    if free_slots(me) <= 0 and not any(held(me, row[0]) for row in stock if row):
        return None
    best = None
    for row in stock or []:
        if not isinstance(row, (list, tuple)) or len(row) < 3:
            continue
        iid, qty, price = int(row[0]), int(row[1]), max(1, int(row[2]))
        if qty <= 0 or held(me, iid) >= want_more:
            continue
        worth = consumable_worth(iid)
        if worth <= 0:
            continue
        give = pay_for(price, spare)
        if give is None:
            continue
        if free_slots(me) <= 0 and not held(me, iid):
            continue
        score = worth / price
        if best is None or score > best[0]:
            best = (score, iid, give)
    if best is None:
        return None
    return best[1], 1, best[2]


# -- trades between survivors -------------------------------------------------
def offer_is_fair(me, offer, floors) -> bool:
    """Take a survivor's trade_offer?  `give` is what they hand us, `want`
    what they ask for.  Yes when we can pay it, it leaves the staples above
    `floors` (inv[] order), and by TOKEN_WORTH we come out at least even."""
    give, want = offer.get("give") or [0] * 5, offer.get("want") or [0] * 5
    if any(me.inv[i] < want[i] for i in range(5)):
        return False
    after = [me.inv[i] - want[i] + give[i] for i in range(5)]
    if any(after[i] < floors[i] and want[i] > 0 for i in range(5)):
        return False
    got = sum(TOKEN_WORTH[i] * give[i] for i in range(5))
    paid = sum(TOKEN_WORTH[i] * want[i] for i in range(5))
    return got > 0 and got >= paid


# -- crafting beyond the starter kit ------------------------------------------
def craft_wanted(me, rid, weights, protected=(), cap=2) -> bool:
    """Is recipe `rid`'s output worth making?  A consumable we hold fewer
    than `cap` of and would ever use, or gear that beats what that slot
    holds and that we do not own already.  The starter recipes (Canteen,
    Backpack, Motorbike) are not asked about here: their own errands own them."""
    rec = RECIPES.get(rid)
    if rec is None or rec.get("starter"):
        return False
    out = rec["output"]
    if ITEM_CATEGORY.get(out) == "equipment":
        eslot = EQUIPMENT.get(out)
        if eslot is None or out in me.equip or held(me, out):
            return False
        return gear_score(out, weights) > gear_score(me.equip[eslot], weights)
    return is_useful_fx(ITEM_FX.get(out)) and held(me, out) < cap and out not in protected


# -- loot worth, for the encounter planner ------------------------------------
class Worth:
    """What a policy thinks loot is worth, for encounters.plan().

    Tokens by TOKEN_WORTH, raised when the pack is short of that staple;
    consumables by consumable_worth(); equipment by how much it beats what
    that slot holds; materials a little; recipes we do not know a lot.
    `bonus` adds per item id (quest items), `node_bonus(biome, eid, key)`
    per node (quest goals, unseen content)."""

    def __init__(self, me, weights=None, floors=(4, 2, 0, 0, 0), bonus=None,
                 node_bonus=None, recipe_value=6.0):
        self.me = me
        self.weights = weights or {}
        self.floors = floors
        self.bonus = bonus or {}
        self.node_bonus = node_bonus
        self.recipe_value = recipe_value

    def token(self, res: int) -> float:
        w = TOKEN_WORTH[res]
        if self.me.inv[res] < self.floors[res]:
            w *= 1.8
        return w

    def item(self, iid: int) -> float:
        if not iid:
            return 0.0
        extra = self.bonus.get(iid, 0.0)
        cat = ITEM_CATEGORY.get(iid)
        if cat == "equipment":
            eslot = EQUIPMENT.get(iid)
            worn = self.me.equip[eslot] if eslot is not None else 0
            gain = gear_score(iid, self.weights) - gear_score(worn, self.weights)
            return extra + 1.0 + max(0.0, gain)
        if cat == "material":
            return extra + (1.5 if iid in MATERIALS else 0.0)
        if cat == "key":
            return extra + 1.0
        return extra + consumable_worth(iid)

    def recipe(self, rid: int) -> float:
        known = self.me.known_recipes & (1 << (rid - 1)) if 1 <= rid <= 32 else 1
        return 0.0 if known else self.recipe_value

    def node(self, biome, eid, key) -> float:
        return self.node_bonus(biome, eid, key) if self.node_bonus else 0.0
