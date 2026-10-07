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
        self.pushed = 0          # caches pressed on from, in a trap
        self.last_ci = None      # the choice last sent
        self.sent_at = None      # when it was sent; cleared by its enc_res
        # (node, ci) pairs the board refused outright.  A refusal changes
        # nothing, so the same pick comes straight back; this is what stops it.
        self.refused: set[tuple[str, int]] = set()

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
        self.pending_next = None
        self.sent_at = None

    def summary(self) -> dict:
        return {"biome": self.biome, "id": self.eid,
                "title": self.enc.get("title", ""), "node": self.node_key,
                "rolls": self.rolls, "visited": self.visited,
                "banked": self.banked, "trap": self.trap}


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
