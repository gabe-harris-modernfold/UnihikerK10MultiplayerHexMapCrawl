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
from pathlib import Path

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
    dn = 2 + (risk * 10) // 100
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

    @property
    def node(self) -> dict:
        return (self.enc.get("nodes") or {}).get(self.node_key, {})

    @property
    def choices(self) -> list:
        return self.node.get("choices") or []

    def can_bank(self) -> bool:
        return bool(self.node.get("can_bank"))

    def loot_here(self) -> list:
        return self.node.get("loot") or []

    def choose(self, ci: int) -> None:
        """Record that we sent choice ci, so on success we know where we land."""
        self.rolls += 1
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

    def summary(self) -> dict:
        return {"biome": self.biome, "id": self.eid,
                "title": self.enc.get("title", ""), "node": self.node_key,
                "rolls": self.rolls, "visited": self.visited,
                "banked": self.banked}
