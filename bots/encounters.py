"""Encounter content, loaded from the local repo rather than the board.

The server tells us only which file we are in:
    {"t":"enc_path","biome":"<dir>","id":<n>}
and everything after that is resolved server-side from its own copy on the SD
card.  The client's copy exists purely to decide *which* choice index to send.

We read data/encounters/ off local disk instead of GET /enc?biome=..&id=..
because the content is identical, static, and every avoided request is one
less TCP connection through AsyncTCP -- docs/dev-loop.md is explicit that
request bursts are what wedge the HTTP server.

Node transitions mirror data/ui-encounter.js: on success you advance to the
chosen branch's `success_node`; a failure applies the hazard and `ends` in
the enc_res event says whether the scene is over.
"""
import json
import time
from pathlib import Path

from config import ITEM_CATEGORY

REPO_ENCOUNTERS = Path(__file__).resolve().parent.parent / "data" / "encounters"

# Skill ids are the firmware's 5-skill enum (0 NAVIGATE .. 4 ENDURE).  The
# files were migrated off a 6-skill enum in Sept 2026; id 5 must not reappear.
MAX_SKILL_ID = 4

# computeEncounterDN thresholds (encounter_engine.hpp): the threat clock adds
# +5 effective risk at each one.
TC_THRESHOLDS = (5, 10, 15, 20)

# 2d6 probability of rolling >= n, for n in 2..12.
_2D6_AT_LEAST = {2: 36, 3: 35, 4: 33, 5: 30, 6: 26, 7: 21,
                 8: 15, 9: 10, 10: 6, 11: 3, 12: 1}


def p_at_least(n: int) -> float:
    """P(2d6 >= n)."""
    if n <= 2:
        return 1.0
    if n > 12:
        return 0.0
    return _2D6_AT_LEAST[n] / 36.0


def compute_dn(base_risk: int, threat: int, ll: int, rad: int) -> int:
    """Port of computeEncounterDN().  The skill argument is unused there too."""
    risk = min(int(base_risk), 100)
    for t in TC_THRESHOLDS:
        if threat >= t:
            risk += 5
    risk = max(0, min(risk, 100))
    # Mirrors computeEncounterDN() -- see boot-assets.hpp for why this is
    # 5 + risk*7/100 rather than the old 2 + risk*10/100.
    dn = 5 + (risk * 7) // 100
    if rad > 3:
        dn += (rad - 3) // 2
    bonus = (ll - 4) // 2 if ll > 4 else 0
    return max(2, min(dn - bonus, 12))


def success_chance(choice: dict, obs) -> float:
    """Odds this choice's check passes, from the bot's own stats.

    The roll is 2d6 + skill value; success needs total >= DN.  Equipment and
    situational mods are not modelled, so this is a floor, not an oracle.
    """
    me = obs.me
    dn = compute_dn(choice.get("base_risk", 50), obs.threat, me.ll, me.rad)
    skill_id = choice.get("skill", 0)
    sv = me.skills[skill_id] if 0 <= skill_id <= MAX_SKILL_ID and me.skills else 0
    return p_at_least(dn - sv)


def has_for_choice(choice: dict, me) -> bool:
    """May `me` pick this choice?  A choice with "requires_item": N is only
    open to a survivor who has item N -- WORN if it is equipment, anywhere in
    the pack otherwise.  Port of playerHasForChoice() (inventory_items.hpp).
    The server refuses anyone else, and the browser hides it from them."""
    iid = int(choice.get("requires_item", 0) or 0)
    if not iid:
        return True
    cat = ITEM_CATEGORY.get(iid)
    if cat is None or me is None:
        return False
    if cat == "equipment":
        return iid in (me.equip or [])
    return any(t == iid and q > 0 for t, q in zip(me.inv_type or [], me.inv_qty or []))


def can_pay(choice: dict, me) -> bool:
    """Can `me` afford this choice's "cost"?  Port of canAfford in
    handleEncChoice() (network-msg-encounter.hpp): LL, radiation headroom to
    10, and the four tokens it charges -- food, water, scrap, medicine.  The
    board nacks anything else "no_res" and changes nothing, so a policy that
    picks it without asking waits forever: a Biker spent the last 3 minutes of
    a K10 run re-sending a 1-scrap option it could not pay for, 477 times."""
    cost = choice.get("cost") or {}
    if not cost or me is None:
        return True
    inv = list(getattr(me, "inv", None) or [0] * 5)
    n = lambda k: int(cost.get(k, 0) or 0)
    return (me.ll >= n("ll") and getattr(me, "rad", 0) + n("radiation") <= 10
            and inv[1] >= n("food") and inv[0] >= n("water")
            and inv[4] >= n("scrap") and inv[3] >= n("med"))


class EncounterLibrary:
    """All encounter JSON, indexed by the (biome_path, id) the server names."""

    def __init__(self, root: Path | None = None):
        self.root = Path(root) if root else REPO_ENCOUNTERS
        self._cache: dict[tuple[str, int], dict] = {}
        self.index: dict = {}
        self.loaded = 0
        self.failed: list[str] = []
        self._load_index()

    def _load_index(self) -> None:
        idx = self.root / "index.json"
        if idx.is_file():
            try:
                self.index = json.loads(idx.read_text(encoding="utf-8"))
            except json.JSONDecodeError as e:
                self.failed.append(f"index.json: {e}")

    def load_all(self) -> int:
        """Eagerly parse every encounter so a malformed file is found up front
        rather than mid-run.  Returns the count loaded."""
        for entry in sorted(self.root.glob("*/*.json")):
            biome = entry.parent.name
            try:
                eid = int(entry.stem)
            except ValueError:
                continue
            try:
                self._cache[(biome, eid)] = json.loads(entry.read_text(encoding="utf-8"))
                self.loaded += 1
            except (json.JSONDecodeError, OSError) as e:
                self.failed.append(f"{biome}/{entry.name}: {e}")
        return self.loaded

    def get(self, biome: str, eid: int) -> dict | None:
        key = (biome, int(eid))
        if key in self._cache:
            return self._cache[key]
        path = self.root / biome / f"{eid}.json"
        if not path.is_file():
            return None
        try:
            enc = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as e:
            self.failed.append(f"{biome}/{eid}.json: {e}")
            return None
        self._cache[key] = enc
        return enc

    def node_count(self) -> int:
        return sum(len(e.get("nodes", {})) for e in self._cache.values())


class EncounterRun:
    """Tracks one open encounter: which file, which node, what we have seen.

    The server never names the current node -- it only sends enc_path once and
    then enc_res per roll -- so the node pointer is maintained here exactly as
    the browser client does it.
    """

    def __init__(self, library: EncounterLibrary, biome: str, eid: int):
        self.biome = biome
        self.eid = eid
        self.enc = library.get(biome, eid) or {}
        self.node_key = self.enc.get("start_node", "")
        self.visited: list[str] = []
        self.rolls = 0
        self.pending_next = None
        self.banked = False
        # A booby trap (enc_path "trap": 1, traps.hpp): opened by a step, not
        # chosen, and it cannot be walked away from at the start.
        self.trap = False
        # One of Machine Gun Kane's chapters (enc_path "kane": n, kane.hpp):
        # the chapter number, 0 for anything else. Also opened by arriving,
        # and read only once its last node is reached.
        self.kane = 0
        # One of VACANCY's parts (enc_path "vac": n, vacancy.hpp): 1 the town,
        # 2 the Front Desk, 3 Housekeeping, 4 Check-Out; 0 for anything else.
        # Opened by arriving too; 2 and 3 count only once BANKED at the end.
        self.vac = 0
        self.pushed = 0          # caches pressed on from, in a trap
        self.last_ci = None      # the choice last sent
        self.last_from = None    # ...and the node it was sent from
        self.sent_at = None      # when it was sent; cleared by its enc_res
        # (node, ci) pairs the board refused outright.  A refusal changes
        # nothing, so the same pick comes straight back; this is what stops it.
        self.refused: set[tuple[str, int]] = set()
        # What has been won so far and waits for the bank (encRunChoice adds
        # each success's loot to pendingLoot/Items/Recipes; a hazard that ends
        # the scene loses the lot).  From the enc_res events themselves, so it
        # is what the board rolled, not what the file says it might.
        self.pending_tokens = [0] * 5
        self.pending_items: list[tuple[int, int]] = []
        self.pending_recipes: set[int] = set()
        # (node, ci, item) for every requires_item choice this scene took,
        # and whether we held the item when we sent it (the quest oracle).
        self.gates_sent: list[tuple[str, int, int, bool]] = []

    @property
    def node(self) -> dict:
        return (self.enc.get("nodes") or {}).get(self.node_key, {})

    @property
    def choices(self) -> list:
        return self.node.get("choices") or []

    def can_bank(self) -> bool:
        return bool(self.node.get("can_bank"))

    @property
    def at_escape(self) -> bool:
        """On a node flagged "escape": a way out, not the bottom of the place."""
        return bool(self.node.get("escape"))

    def _leads_to_escape(self, ch: dict) -> bool:
        nxt = (self.enc.get("nodes") or {}).get(ch.get("success_node", ""), {})
        return bool(nxt.get("escape"))

    def awaiting(self, wait: float) -> bool:
        """A choice is in flight and its enc_res has not come back yet (for
        at most `wait` seconds -- after that it was lost, and the next pick
        finds out where the board really is).  Sending another meanwhile
        answers a node the board has already left: on a busy K10 the result
        took seconds, and a trap was re-worked every cycle into no_choice."""
        return self.sent_at is not None and time.monotonic() - self.sent_at < wait

    def is_refused(self, ci) -> bool:
        return ci is not None and (self.node_key, ci) in self.refused

    def escape_choice(self):
        """Index of the choice that backs out of a trap, or None."""
        for i, ch in enumerate(self.choices):
            if self._leads_to_escape(ch):
                return i
        return None

    def work_choice(self):
        """Index of the first choice that does not back out, or None."""
        for i, ch in enumerate(self.choices):
            if not self._leads_to_escape(ch):
                return i
        return None

    def first_open_choice(self, me):
        """Index of the first choice `me` is allowed to pick and can pay for,
        and that the board has not already refused, or None."""
        for i, ch in enumerate(self.choices):
            if ((self.node_key, i) not in self.refused
                    and has_for_choice(ch, me) and can_pay(ch, me)):
                return i
        return None

    def refuse(self) -> None:
        """The board refused the choice last sent: never offer it again here."""
        if self.last_ci is not None:
            self.refused.add((self.node_key, self.last_ci))
        self.pending_next = None
        self.sent_at = None

    def loot_here(self) -> list:
        return self.node.get("loot") or []

    def choose(self, ci: int) -> None:
        """Record that we sent choice ci, so on success we know where we land."""
        self.rolls += 1
        self.last_ci = ci
        self.last_from = self.node_key
        self.sent_at = time.monotonic()
        chs = self.choices
        self.pending_next = chs[ci].get("success_node") if 0 <= ci < len(chs) else None

    def on_result(self, ev: dict) -> None:
        """Fold in an enc_res event.  Advance only on success, exactly as
        ui-encounter.js does; a failure leaves us where we are (or ends the
        scene, which the caller sees via ev['ends'])."""
        if ev.get("out") and self.pending_next:
            self.node_key = self.pending_next
            self.visited.append(self.node_key)
        if ev.get("out"):
            for i, n in enumerate((ev.get("loot") or [])[:5]):
                self.pending_tokens[i] += max(0, int(n or 0))
            for tk, qk in (("it", "iq"), ("it2", "iq2")):
                if ev.get(tk):
                    self.pending_items.append((int(ev[tk]), int(ev.get(qk) or 1)))
            if ev.get("rec"):
                self.pending_recipes.add(int(ev["rec"]))
        self.pending_next = None
        self.sent_at = None

    def summary(self) -> dict:
        return {"biome": self.biome, "id": self.eid,
                "title": self.enc.get("title", ""), "node": self.node_key,
                "rolls": self.rolls, "visited": self.visited,
                "banked": self.banked, "trap": self.trap, "kane": self.kane,
                "vac": self.vac}


# -- planning a scene ----------------------------------------------------------
# A full clear (banking at a terminal node) pays SCORE_ENC_FULL_CLEAR on top.
CLEAR_BONUS = 2.0
# What going down costs, in the same loot units: a fresh survivor, the pack on
# the ground, and the walk back for it.
DOWNED_COST = 60.0
# Walking away from a scene that cannot be banked: the threat clock ticks and
# the POI is gone for good.
ABORT_COST = 1.0
PLAN_DEPTH = 6

_LOOT_TABLES = None


def loot_tables(root: Path | None = None) -> dict:
    global _LOOT_TABLES
    if _LOOT_TABLES is None or root is not None:
        try:
            p = (Path(root) if root else REPO_ENCOUNTERS) / "loot_tables.json"
            _LOOT_TABLES = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            _LOOT_TABLES = {}
    return _LOOT_TABLES


def _mean(q) -> float:
    if isinstance(q, (list, tuple)) and q:
        return (float(q[0]) + float(q[-1])) / 2.0
    try:
        return float(q)
    except (TypeError, ValueError):
        return 1.0


def node_gain(enc: dict, key: str, worth, biome=None, eid=None) -> float:
    """Expected worth of arriving at node `key`: its literal loot rows, its
    loot table's weighted roll, any recipe, and the policy's own bonus for
    the node itself (quest goals, unseen content)."""
    node = (enc.get("nodes") or {}).get(key) or {}
    v = 0.0
    for row in node.get("loot") or []:
        if not isinstance(row, dict):
            continue
        if "res" in row and 0 <= int(row["res"]) < 5:
            v += worth.token(int(row["res"])) * _mean(row.get("qty", 1))
        elif row.get("item"):
            v += worth.item(int(row["item"])) * max(1.0, _mean(row.get("qty", 1)))
        elif row.get("recipe"):
            v += worth.recipe(int(row["recipe"]))
    table = loot_tables().get(node.get("loot_table") or "")
    if isinstance(table, list) and table:
        tot = sum(max(0, int(r.get("weight", 0) or 0)) for r in table if isinstance(r, dict))
        if tot > 0:
            v += sum(max(0, int(r.get("weight", 0) or 0)) / tot
                     * worth.item(int(r.get("item", 0) or 0)) * max(1.0, _mean(r.get("qty", 1)))
                     for r in table if isinstance(r, dict) and r.get("item"))
    v += worth.node(biome, eid, key)
    return v


def hazard_cost(enc: dict, ch: dict, me, risk: float = 1.0) -> tuple[float, bool]:
    """(cost, ends) of failing choice `ch`: the hazard's LL, radiation,
    wounds and tokens priced against the survivor, and whether it ends the
    scene (and so loses everything still pending)."""
    hz = (enc.get("hazards") or {}).get(ch.get("hazard_id") or "") or {}
    pen = hz.get("penalty") or {}
    ends = bool(hz.get("ends_encounter"))
    cost = 0.0
    ll_loss = -int(pen.get("ll", 0) or 0)
    if ll_loss > 0:
        left = me.ll - ll_loss
        if left <= 0:
            cost += DOWNED_COST
        else:
            # A point of LL is worth more the fewer are left: AP is ll + 3 and
            # the last ones are the difference between walking home and not.
            cost += ll_loss * (3.0 + max(0, 4 - left) * 2.0)
    cost += 0.7 * max(0, int(pen.get("radiation", 0) or 0)) * (2.0 if me.rad >= 5 else 1.0)
    w = hz.get("wound") or [0, 0]
    if isinstance(w, (list, tuple)):
        cost += 1.5 * (w[0] if len(w) > 0 else 0) + 4.0 * (w[1] if len(w) > 1 else 0)
    for i, k in enumerate(("water", "food", "fuel", "med", "scrap")):
        cost += 1.0 * max(0, int(pen.get(k, 0) or 0)) * (2.0 if i < 2 else 1.0)
    return cost * risk, ends


def pending_worth(run, worth) -> float:
    v = sum(worth.token(i) * n for i, n in enumerate(run.pending_tokens))
    v += sum(worth.item(i) * q for i, q in run.pending_items)
    v += sum(worth.recipe(r) for r in run.pending_recipes)
    return v


def plan(run, obs, worth, risk: float = 1.0, min_p: float = 0.0, depth: int = PLAN_DEPTH):
    """The best thing to do at the current node: ("bank", None, value),
    ("choice", ci, value) or ("abort", None, value).

    Expected value over the node graph: a choice is worth p * (what the node
    it leads to holds, plus the best of what follows) + (1 - p) * (its
    hazard, and everything pending if the hazard ends the scene).  Banking
    is worth what is pending (plus the clear bonus at a terminal node).  A
    gated choice is only considered when we may take it (has_for_choice), and
    one the board refused here never again.  Cycles (a choice back to an
    earlier node) are cut, and the search stops after `depth` nodes -- what
    lies beyond is counted as bankable as it stands.

    `min_p` drops choices whose odds are below it, unless nothing else is
    left.  `risk` scales every hazard (a coward prices them higher)."""
    enc = run.enc or {}
    nodes = enc.get("nodes") or {}
    me = obs.me
    biome, eid = run.biome, run.eid
    memo = {}

    def options(key, pending, d, path):
        node = nodes.get(key) or {}
        chs = node.get("choices") or []
        terminal = not chs
        bankable = bool(node.get("can_bank")) or terminal
        out = []
        if bankable:
            bonus = CLEAR_BONUS if terminal and not node.get("escape") else 0.0
            out.append(("bank", None, pending + bonus))
        if terminal or d <= 0:
            if not bankable:
                out.append(("bank", None, pending))   # depth cut: count it as is
            return out
        here = []
        for ci, ch in enumerate(chs):
            if key == run.node_key and run.is_refused(ci):
                continue
            if not has_for_choice(ch, me) or not can_pay(ch, me):
                continue
            nxt = ch.get("success_node", "")
            if nxt not in nodes or nxt in path:
                continue
            p = success_chance(ch, obs)
            gain = node_gain(enc, nxt, worth, biome, eid)
            succ = best(nxt, pending + gain, d - 1, path | {nxt})
            cost, ends = hazard_cost(enc, ch, me, risk)
            stay = pending if bankable else 0.5 * pending
            fail = -cost if ends else -cost + stay
            here.append((p, ("choice", ci, p * succ + (1 - p) * fail)))
        if min_p > 0 and any(p >= min_p for p, _ in here):
            here = [h for h in here if h[0] >= min_p]
        out.extend(o for _p, o in here)
        if not bankable:
            out.append(("abort", None, -ABORT_COST))
        return out

    def best(key, pending, d, path):
        mk = (key, round(pending, 2), d)
        if mk not in memo:
            memo[mk] = max(o[2] for o in options(key, pending, d, path))
        return memo[mk]

    pend = pending_worth(run, worth)
    opts = options(run.node_key, pend, depth, frozenset({run.node_key}))
    if not opts:
        return ("abort", None, -ABORT_COST)
    # Ties go to the choice (it reads more of the scene), then to the bank.
    rank = {"choice": 2, "bank": 1, "abort": 0}
    return max(opts, key=lambda o: (round(o[2], 3), rank[o[0]]))


def item_drop_terrains(item_id: int, root: Path | None = None) -> dict[int, float]:
    """{terrain index: share of that terrain's POI pool that can pay item_id}.

    A POI's scene is dealt from the pool index.json names for the terrain it
    stands on, and an item reaches the pack on a bank -- through a node's
    "loot_table" (data/encounters/loot_tables.json) or a literal "item" in
    its "loot".  A scene counts if any of its nodes can pay it; terrains
    whose pool never can are left out.  The Biker ranks POIs by this when it
    is hunting the Sparky Bits for its bike: those come out of urban, glass,
    mountain and settlement scenes and never out of the marsh.
    """
    root = Path(root) if root else REPO_ENCOUNTERS
    try:
        index = json.loads((root / "index.json").read_text(encoding="utf-8"))
        tables = json.loads((root / "loot_tables.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    paying = {name for name, rows in tables.items() if isinstance(rows, list)
              and any(isinstance(r, dict) and r.get("item") == item_id for r in rows)}

    def pays(node) -> bool:
        if isinstance(node, dict):
            if node.get("loot_table") in paying:
                return True
            for row in node.get("loot") or []:
                if isinstance(row, dict) and row.get("item") == item_id:
                    return True
            return any(pays(v) for v in node.values())
        if isinstance(node, list):
            return any(pays(v) for v in node)
        return False

    out = {}
    for key, pool in index.items():
        if not key.isdigit() or not isinstance(pool, dict):
            continue
        seen = hits = 0
        for eid in range(1, int(pool.get("count", 0)) + 1):
            try:
                enc = json.loads((root / pool["path"] / f"{eid}.json").read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError, KeyError):
                continue
            seen += 1
            hits += pays(enc.get("nodes", {}))
        if hits:
            out[int(key)] = hits / seen
    return out
