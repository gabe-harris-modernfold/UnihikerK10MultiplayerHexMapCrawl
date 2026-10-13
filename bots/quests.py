"""Quests: the chains of scenes the content strings together, and a journal
of how far a bot (and the whole fleet) got through each one.

The firmware has no quest system.  There are no quest ids, no per-player
flags and no stage counters -- only Machine Gun Kane's world-shared `stage`
(kane.hpp) and VACANCY's world-shared sign (vacancy.hpp: `st`, `rm`, `g`).
Everything else that reads as a quest is held together by the
pack: a scene pays an item, and a later scene has a `requires_item` choice
that only opens for whoever holds it -- or, for the two oldest chains, only
*says* it needs it.  So "do the sequential quests work" is a question about
three things, and this module checks all three:

1. **Is every step reachable?**  A step lives in an encounter file; the file
   has to be in a pool index.json deals from (`reachable`).
2. **Is the order enforced?**  A step that needs an item has to refuse a
   survivor without it.  Where the content gates it with `requires_item`,
   the board must nack `no_item` (`gate_bypassed` if it does not); where
   the content only implies the item in the choice text, the step goes
   through without it (`quest_ungated`).
3. **Does each step pay what the next one needs?**  An item a scene rolled
   (enc_res `it`) has to be in the pack or on our hex after the bank
   (`banked_item_missing`).

The catalog is derived from the content where the content says enough --
gates from `requires_item`, item payers from `loot`, Kane's chapters from
the `kane` pool, VACANCY's four parts from the `vacancy` pool, recipe
lessons from `"recipe"` loot -- and declared by hand only for the two chains
the content never encodes (Jack's Chopper, the Sticky Note), and for what
VACANCY's parts must pay and need (_VAC_PAYS, _VAC_NEEDS), so the lint
notices if the content stops asking.  smoke.py checks every declared node
and choice still exists.

    python quests.py            # the catalog, and what is wrong with it
"""
import json
import time
from dataclasses import dataclass, field
from pathlib import Path

from config import (ITEM_CATEGORY, ITEM_NAME, KANE_CHAPTERS, RECIPES,
                    VAC_GOT_KEY, VAC_GOT_REGISTER, VAC_ITEM_KEY, VAC_ITEM_REGISTER,
                    VAC_PART_DESK, VAC_PART_KEYS, VAC_PART_OUT, VAC_PART_SIGN,
                    VAC_PARTS, VAC_ST_OPEN, VAC_ST_SAVED)

REPO_ENCOUNTERS = Path(__file__).resolve().parent.parent / "data" / "encounters"

# Seconds after a bank for a rolled item to show up in the pack (item_result)
# or on our hex (the overflow ground_update) before it is called missing.
ITEM_ARRIVAL_S = 8.0
# Bonus the planner puts on a quest goal node, and on an item a quest step
# pays or needs -- big enough to beat any ordinary loot, small next to
# encounters.DOWNED_COST so it never walks into a lethal roll for one.
GOAL_BONUS = 30.0
QUEST_ITEM_BONUS = 25.0


@dataclass(frozen=True)
class Step:
    key: str
    title: str
    files: tuple = ()               # ((biome, eid), ...) any of which holds it
    goal: frozenset = frozenset()   # node keys: arriving at one completes it
    needs: int = 0                  # the item it should take to do this step
    gate: tuple = ()                # ((node, ci), ...) choices that should need it
    gated: bool = False             # the content enforces it (requires_item)
    pays: int = 0                   # the item its goal hands out on the bank
    worn: bool = False              # `needs` counts only while worn
    recipe: int = 0                 # workshop steps
    chapter: int = 0                # Machine Gun Kane's chapters
    vac_part: int = 0               # VACANCY's parts (1-4)
    also: tuple = ()                # further items it needs besides `needs` (Check-Out: both)

    def needed(self) -> tuple:
        """Every item this step needs: `needs`, then `also`."""
        return ((self.needs,) if self.needs else ()) + tuple(self.also)


@dataclass
class Quest:
    key: str
    title: str
    kind: str                       # chain | collect | story | workshop
    steps: list = field(default_factory=list)
    reachable: bool = True
    note: str = ""
    problems: list = field(default_factory=list)    # found while building it (lint)


def _load(root, biome, eid):
    try:
        return json.loads((root / biome / f"{eid}.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _pools(root) -> set:
    try:
        idx = json.loads((root / "index.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return set()
    return {v.get("path") for v in idx.values() if isinstance(v, dict)}


def _terminals(enc) -> frozenset:
    return frozenset(k for k, n in (enc.get("nodes") or {}).items()
                     if not n.get("choices") and not n.get("escape"))


# Two chains the content tells only in prose: nothing in scrub/20 asks for
# the mask, nothing in urban/15 asks for the note.  (node, ci) is the choice
# whose text hands the item over.
_DECLARED = (
    ("jack", "Jack's Chopper", (
        dict(key="jack.accept", title="agree to fetch Jack's mask",
             files=(("scrub", 19),), goal=("quest_accepted",)),
        dict(key="jack.mask", title="find the mask at the gas station",
             files=(("urban", 19),), goal=("mask_found",), pays=33),
        dict(key="jack.deliver", title="hand Jack his mask",
             files=(("scrub", 20),), goal=("mask_delivered",), needs=33,
             gate=(("entry", 0),), pays=26),
    )),
    ("sticky", "The Sticky Note", (
        dict(key="sticky.note", title="take the note off the desk jockey",
             files=(("scrub", 18),), goal=("note_retrieved",), pays=51),
        dict(key="sticky.cult", title="give the Dial-Up Cult its password",
             files=(("urban", 15),), goal=("router_defeated",), needs=51,
             gate=(("entry", 1),), pays=38),
    )),
)

# Files that share requires_item gates and the items that open them: the
# Perambulator stands on three terrains with one graph; the Null Camp is its
# written-but-unplaced sequel.
_COLLECT = (
    ("perambulator", "The Perambulator (Null Meridian)",
     (("scrub", 21), ("glass", 3), ("ridge", 3))),
    ("null_camp", "The Null Camp (Null Meridian, 3 stages)",
     (("meridian/null_camp", 1), ("meridian/null_camp", 2), ("meridian/null_camp", 3))),
)

# VACANCY (vacancy.hpp): what each part should pay on its last node, and
# what Check-Out should ask for.  The board holds the order (the sites open
# only once the town is read, Check-Out only for a pack holding both), so
# this only declares what the content has to keep doing for that to work.
_VAC_TITLES = {VAC_PART_SIGN: "read the town under the sign",
               VAC_PART_DESK: "bank the Guest Register at the Front Desk",
               VAC_PART_KEYS: "bank the Master Key in Housekeeping",
               VAC_PART_OUT: "check the town out"}
_VAC_PAYS = {VAC_PART_DESK: VAC_ITEM_REGISTER, VAC_PART_KEYS: VAC_ITEM_KEY}
_VAC_NEEDS = {VAC_PART_OUT: (VAC_ITEM_REGISTER, VAC_ITEM_KEY)}
# `ev vac` w -> the part it reports done (the hum, a room, the fall: none).
VAC_EV_PART = {"hook": VAC_PART_SIGN, "desk": VAC_PART_DESK,
               "keys": VAC_PART_KEYS, "saved": VAC_PART_OUT}


def build_catalog(root: Path | None = None) -> list[Quest]:
    root = Path(root) if root else REPO_ENCOUNTERS
    pools = _pools(root)
    quests = []

    for qkey, title, steps in _DECLARED:
        q = Quest(qkey, title, "chain")
        for d in steps:
            gated = False
            if d.get("gate"):
                gated = True
                for biome, eid in d["files"]:
                    enc = _load(root, biome, eid) or {}
                    for node, ci in d["gate"]:
                        chs = ((enc.get("nodes") or {}).get(node) or {}).get("choices") or []
                        if ci >= len(chs) or int(chs[ci].get("requires_item", 0) or 0) != d["needs"]:
                            gated = False
            q.steps.append(Step(key=d["key"], title=d["title"], files=tuple(d["files"]),
                                goal=frozenset(d["goal"]), needs=d.get("needs", 0),
                                gate=tuple(d.get("gate", ())), gated=gated,
                                pays=d.get("pays", 0)))
        q.reachable = all(b in pools for s in q.steps for b, _e in s.files)
        quests.append(q)

    for qkey, title, files in _COLLECT:
        q = Quest(qkey, title, "collect")
        q.reachable = all(b in pools for b, _e in files)
        if not q.reachable:
            q.note = "no pool in index.json deals these files"
        # Each step lists only the files it is really in: the Perambulator is
        # one graph on three terrains, the Null Camp three different stages.
        gates, payers = {}, {}
        for biome, eid in files:
            enc = _load(root, biome, eid) or {}
            for nk, node in (enc.get("nodes") or {}).items():
                for row in node.get("loot") or []:
                    if isinstance(row, dict) and row.get("item"):
                        p = payers.setdefault(int(row["item"]), [set(), []])
                        p[0].add(nk)
                        if (biome, eid) not in p[1]:
                            p[1].append((biome, eid))
                for ci, ch in enumerate(node.get("choices") or []):
                    need = int(ch.get("requires_item", 0) or 0)
                    if need:
                        g = gates.setdefault((need, ch.get("success_node", "")), [set(), []])
                        g[0].add((nk, ci))
                        if (biome, eid) not in g[1]:
                            g[1].append((biome, eid))
        wanted = {need for need, _n in gates}
        for iid in sorted(wanted):
            if iid in payers:
                nodes, where = payers[iid]
                q.steps.append(Step(key=f"{qkey}.get{iid}", title=f"take the {ITEM_NAME.get(iid, iid)}",
                                    files=tuple(where), goal=frozenset(nodes), pays=iid))
        shared = {dest for (_n, dest) in gates
                  if sum(1 for (_m, d2) in gates if d2 == dest) > 1}
        for (need, dest), (choices, where) in sorted(gates.items()):
            key = f"{qkey}.{dest}" + (f"+{need}" if dest in shared else "")
            q.steps.append(Step(key=key, title=f"{dest} (needs the {ITEM_NAME.get(need, need)})",
                                files=tuple(where), goal=frozenset({dest}), needs=need,
                                gate=tuple(sorted(choices)), gated=True,
                                worn=ITEM_CATEGORY.get(need) == "equipment"))
        quests.append(q)

    kane = Quest("kane", "Machine Gun Kane", "story")
    kane.reachable = "kane" in pools
    for n in range(1, KANE_CHAPTERS + 1):
        enc = _load(root, "kane", n) or {}
        kane.steps.append(Step(key=f"kane.{n}", title=f"chapter {n}: {enc.get('title', '?')}",
                               files=(("kane", n),), goal=_terminals(enc), chapter=n))
    quests.append(kane)

    # VACANCY: four parts in a pool no terrain deals -- like Kane's, each
    # opens server-side on arrival -- so it is reachable when the pool is in
    # index.json at all.  The goal of each part is its last (non-escape)
    # node; Check-Out's gates are every requires_item choice in it.
    vac = Quest("vacancy", "VACANCY (the sign over the town)", "story")
    vac.reachable = "vacancy" in pools
    if not vac.reachable:
        vac.note = 'no "vacancy" pool in index.json'
    for n in range(1, VAC_PARTS + 1):
        enc = _load(root, "vacancy", n) or {}
        needs = _VAC_NEEDS.get(n, ())
        gates = tuple(sorted(
            (nk, ci) for nk, node in (enc.get("nodes") or {}).items()
            for ci, ch in enumerate(node.get("choices") or [])
            if int(ch.get("requires_item", 0) or 0) in needs))
        asked = {int(((enc.get("nodes") or {})[nk].get("choices") or [])[ci].get("requires_item"))
                 for nk, ci in gates}
        goal = _terminals(enc)
        pays = _VAC_PAYS.get(n, 0)
        paid = {nk for nk, node in (enc.get("nodes") or {}).items()
                for row in node.get("loot") or []
                if isinstance(row, dict) and int(row.get("item", 0) or 0) == pays}
        if pays and not paid & goal:
            vac.problems.append(f"vacancy.{n}: its last node never pays the "
                                f"{ITEM_NAME.get(pays, pays)} (vacOnBank needs it in the haul)")
        if pays and paid - goal:
            vac.problems.append(f"vacancy.{n}: the {ITEM_NAME.get(pays, pays)} is paid before the "
                                f"last node ({', '.join(sorted(paid - goal))}) -- banked there, "
                                f"the part counts without the scene being read")
        vac.steps.append(Step(key=f"vacancy.{n}", title=f"part {n}: {_VAC_TITLES[n]} "
                                                         f"({enc.get('title', '?')})",
                              files=(("vacancy", n),), goal=goal,
                              needs=needs[0] if needs else 0, also=tuple(needs[1:]),
                              gate=gates, gated=bool(needs) and asked == set(needs),
                              pays=pays, vac_part=n))
    quests.append(vac)

    shop = Quest("workshop", "Recipes (learn every one, craft every one)", "workshop")
    shop.reachable = "settlement" in pools
    learn = []
    for path in sorted((root / "settlement").glob("*.json"), key=lambda p: int(p.stem)
                       if p.stem.isdigit() else 0):
        if not path.stem.isdigit():
            continue
        enc = _load(root, "settlement", int(path.stem)) or {}
        for nk, node in (enc.get("nodes") or {}).items():
            for row in node.get("loot") or []:
                if isinstance(row, dict) and row.get("recipe"):
                    learn.append((int(row["recipe"]), int(path.stem), nk))
    for rid, eid, nk in sorted(learn):
        rname = RECIPES.get(rid, {}).get("name", f"recipe {rid}")
        shop.steps.append(Step(key=f"recipe.{rid}.learn", title=f"learn {rname}",
                               files=(("settlement", eid),), goal=frozenset({nk}), recipe=rid))
    for rid, _e, _n in sorted(learn):
        rname = RECIPES.get(rid, {}).get("name", f"recipe {rid}")
        shop.steps.append(Step(key=f"recipe.{rid}.craft", title=f"craft {rname}", recipe=rid))
    quests.append(shop)
    return quests


def lint(catalog) -> list[str]:
    """What is wrong with the quests as authored -- the problems a bot
    would otherwise have to stumble over one run at a time."""
    out = []
    for q in catalog:
        if not q.reachable:
            out.append(f"{q.key}: unreachable -- {q.note or 'a step file is in no pool'}")
        out.extend(q.problems)
        for s in q.steps:
            # A VACANCY gate list is derived from the content, so an empty one
            # is itself the problem: nothing asks for the items any more.
            if s.needs and (s.gate or s.vac_part) and not s.gated:
                names = " and the ".join(ITEM_NAME.get(i, str(i)) for i in s.needed())
                out.append(f"{s.key}: '{s.title}' needs the {names} "
                           f"but no choice asks for it (requires_item missing at "
                           f"{', '.join(f'{n}[{c}]' for n, c in s.gate) or 'every node'})")
    return out


# -- the fleet's ledger ---------------------------------------------------------
class QuestLedger:
    """Every step any bot in this process reached or finished, and every
    ordering problem seen.  One per arena run (module-level LEDGER)."""

    def __init__(self):
        self.steps: dict[str, dict] = {}
        self.problems: list[dict] = []
        self.gates_probed: dict[tuple, str] = {}

    def reset(self):
        self.__init__()

    def mark(self, step_key, status, who, day, **detail):
        cur = self.steps.get(step_key)
        rank = {"opened": 0, "reached": 1, "done": 2}
        if cur is None:
            self.steps[step_key] = cur = {"status": status, "by": who, "day": day,
                                          "n": 0, "t": time.time(), **detail}
        elif rank.get(status, 0) > rank.get(cur["status"], 0):
            cur.update(status=status, by=who, day=day, t=time.time(), **detail)
        if status == "done":
            cur["n"] = cur.get("n", 0) + 1

    def problem(self, check, step_key, who, day, summary):
        self.problems.append({"check": check, "step": step_key, "by": who,
                              "day": day, "summary": summary})

    def to_dict(self) -> dict:
        return {"steps": self.steps, "problems": self.problems,
                "gates_probed": {"/".join(map(str, k)): v for k, v in self.gates_probed.items()}}

    def report(self, catalog) -> list[str]:
        lines = []
        for q in catalog:
            done = sum(1 for s in q.steps if self.steps.get(s.key, {}).get("status") == "done")
            flag = "" if q.reachable else "  [UNREACHABLE]"
            lines.append(f"  {q.title}: {done}/{len(q.steps)} steps{flag}")
            for s in q.steps:
                st = self.steps.get(s.key)
                if st is None:
                    mark = "  .  never found"
                else:
                    mark = {"done": "  ok ", "reached": "  ~  ", "opened": "  o  "}.get(st["status"], "  ?  ")
                    mark += f"{st['status']} by {st['by']} day {st['day']}"
                    if st.get("n", 0) > 1:
                        mark += f" (x{st['n']})"
                lines.append(f"    {s.key:<28} {mark}")
        for p in self.problems:
            lines.append(f"  !! {p['check']}: {p['summary']} ({p['by']}, day {p['day']})")
        return lines


LEDGER = QuestLedger()
_CATALOG = None


def catalog() -> list[Quest]:
    global _CATALOG
    if _CATALOG is None:
        _CATALOG = build_catalog()
    return _CATALOG


# -- one bot's journal ----------------------------------------------------------
def _has(me, iid, worn=False) -> bool:
    """Holding `iid`: worn, or (unless `worn` is demanded) in the pack.  A
    requires_item gate on equipment wants it WORN (playerHasForChoice); a
    chain that only names an item in its text is satisfied by carrying it."""
    if not iid:
        return True
    if iid in (me.equip or []):
        return True
    if worn:
        return False
    return any(t == iid and q > 0 for t, q in zip(me.inv_type, me.inv_qty))


class QuestJournal:
    """How far THIS bot got, the quest-shaped value of loot and nodes for
    its encounter planner, and the sequencing checks.

    Every SurvivorPolicy keeps one, so a quest step any bot stumbles through
    is on the ledger; only a policy with `quest_drive` lets it steer."""

    def __init__(self, quests=None, ledger=None):
        self.quests = quests if quests is not None else catalog()
        self.ledger = ledger if ledger is not None else LEDGER
        self.who = "?"
        self.flag = None            # oracle.flag(check, severity, summary, key=, **detail)
        self.done: dict[str, int] = {}          # step -> day
        self.reached: set[str] = set()
        self.expect: list[tuple] = []           # (item, deadline, q, r, step)
        self._steps = [s for q in self.quests for s in q.steps]
        self._quest_of = {s.key: q for q in self.quests for s in q.steps}
        self._by_file: dict[tuple, list] = {}
        for s in self._steps:
            for f in s.files:
                self._by_file.setdefault(tuple(f), []).append(s)
        self._open_key, self._open = None, []
        # VACANCY's sign as last seen, (town q, stage, got) or None: every
        # caller without the world to hand (the item layer) still needs to
        # know the town fell, or the register is litter worth keeping.
        self._vac = None

    # -- what is still open ---------------------------------------------------
    def is_done(self, step) -> bool:
        return step.key in self.done

    @staticmethod
    def vac_closed(step, vac) -> bool:
        """A VACANCY part the world has closed for everybody: the town read,
        a site banked, or the sign out (saved, fallen, or no town at all)."""
        if not step.vac_part or vac is None:
            return False
        town, stage, got = vac
        if town < 0 or stage >= VAC_ST_SAVED:
            return True
        if step.vac_part == VAC_PART_SIGN:
            return stage >= VAC_ST_OPEN
        if step.vac_part == VAC_PART_DESK:
            return bool(got & VAC_GOT_REGISTER)
        if step.vac_part == VAC_PART_KEYS:
            return bool(got & VAC_GOT_KEY)
        return False

    def open_steps(self, world=None):
        """Every step not yet done (and, given the world, not a Kane chapter
        the board has already read; nor a VACANCY part it has closed, from
        the last world seen).  Asked a dozen times a decide cycle -- the item
        layer, the gear layer and pursuit all want it -- so the answer is
        kept until a step completes or the arc or the sign moves; `done`
        only ever grows, which makes its size a version.  Callers read the
        list, they do not keep or change it."""
        stage = world.kane_stage if world is not None else None
        if world is not None and getattr(world, "vac_seen", False):
            self._vac = (world.vac_q, world.vac_stage, world.vac_got)
        vac = self._vac
        key = (len(self.done), stage, vac)
        if key != self._open_key:
            out = []
            for s in self._steps:
                if self.is_done(s):
                    continue
                if s.chapter and stage is not None and stage >= s.chapter:
                    continue
                if self.vac_closed(s, vac):
                    continue
                out.append(s)
            self._open_key, self._open = key, out
        return self._open

    def steps_in(self, biome, eid):
        return self._by_file.get((biome, int(eid)), [])

    def item_bonus(self, me) -> dict:
        """{item: bonus} for the planner: an item an open step pays that we
        do not hold, or one an open step needs that we do not hold."""
        bonus = {}
        for s in self.open_steps():
            for iid in (s.pays, *s.needed()):
                if iid and not _has(me, iid):
                    bonus[iid] = QUEST_ITEM_BONUS
        return bonus

    def protected_items(self) -> set:
        """Items an open step still needs: never dropped, sold or used up."""
        return {i for s in self.open_steps() for i in s.needed()} | \
               {s.pays for s in self.open_steps() if s.pays and any(
                   s.pays in t.needed() for t in self.open_steps())}

    def node_bonus(self, me):
        """node_bonus(biome, eid, key) for items.Worth: a goal of an open step
        in this very file, when we can take it now."""
        def bonus(biome, eid, key):
            for s in self.steps_in(biome, eid):
                if key in s.goal and not self.is_done(s) and (
                        s.gated is False or all(_has(me, i, s.worn) for i in s.needed())):
                    return GOAL_BONUS
            return 0.0
        return bonus

    def worn_needed(self, me) -> set:
        """Equipment an open gate needs WORN that we carry in the pack."""
        return {s.needs for s in self.open_steps() if s.worn and s.needs
                and s.needs in me.inv_type and s.needs not in me.equip}

    # -- what happened ----------------------------------------------------------
    def _complete(self, step, day, how):
        if step.key in self.done:
            return
        self.done[step.key] = day
        self.ledger.mark(step.key, "done", self.who, day, how=how)
        q = self._quest_of.get(step.key)
        if q is not None and q.kind == "chain":
            before = q.steps[:q.steps.index(step)]
            skipped = [s.key for s in before if s.key not in self.done]
            if skipped:
                msg = f"{step.key} finished before {', '.join(skipped)}"
                self.ledger.problem("quest_out_of_order", step.key, self.who, day, msg)
                self._flag("quest_out_of_order", "minor",
                           f"{q.title}: '{step.title}' was possible before "
                           f"{', '.join(skipped)} -- nothing in the content orders the chain",
                           key=(step.key,), skipped=skipped)

    def _flag(self, check, severity, summary, key=(), **detail):
        if self.flag is not None:
            self.flag(check, severity, summary, key=key, **detail)

    def on_open(self, run, day):
        for s in self.steps_in(run.biome, run.eid):
            if s.key not in self.reached and s.key not in self.done:
                self.ledger.mark(s.key, "opened", self.who, day)

    def on_choice(self, run, ci, me):
        """Before an enc_choice goes out: remember whether a gated (or
        should-be-gated) choice was sent holding the item."""
        chs = run.choices
        if not 0 <= ci < len(chs):
            return
        need = int(chs[ci].get("requires_item", 0) or 0)
        # The content's own gate: equipment counts only worn, like the board.
        worn = bool(need) and ITEM_CATEGORY.get(need) == "equipment"
        if not need:
            for s in self.steps_in(run.biome, run.eid):
                if (run.node_key, ci) in s.gate:
                    need, worn = s.needs, s.worn
        if need:
            run.gates_sent.append((run.node_key, ci, need, _has(me, need, worn)))

    def on_result(self, run, ev, me, day):
        """After run.on_result() folded in one of our own enc_res events."""
        if not ev.get("out"):
            return
        node = run.node_key
        last = run.gates_sent[-1] if run.gates_sent else None
        # Only the choice this roll answered: a probe the board refused
        # earlier in the scene is still the last gate sent, but not this roll.
        if last is not None and (last[0], last[1]) != (getattr(run, "last_from", None), run.last_ci):
            last = None
        for s in self.steps_in(run.biome, run.eid):
            if node not in s.goal:
                continue
            if s.key not in self.reached:
                self.reached.add(s.key)
                self.ledger.mark(s.key, "reached", self.who, day)
            if s.needs and last is not None and last[2] == s.needs and not last[3]:
                where = f"{run.biome}/{run.eid} {last[0]}[{last[1]}]"
                if s.gated:
                    summary = (f"{where} has requires_item {s.needs} and went through for a "
                               f"survivor without the {ITEM_NAME.get(s.needs, s.needs)}")
                    self.ledger.problem("gate_bypassed", s.key, self.who, day, summary)
                    self._flag("gate_bypassed", "critical", summary, key=(run.biome, run.eid, last[0], last[1]))
                else:
                    summary = (f"'{s.title}' ({where}) went through without the "
                               f"{ITEM_NAME.get(s.needs, s.needs)} -- the choice never asks for it")
                    self.ledger.problem("quest_ungated", s.key, self.who, day, summary)
                    self._flag("quest_ungated", "major", summary, key=(s.key,), node=last[0], ci=last[1])
            if not s.pays:
                self._complete(s, day, "reached")

    def on_bank(self, run, me, day):
        """Our enc_bank landed: steps whose goal this scene reached and that
        pay an item are done, and every item the board rolled has to arrive."""
        visited = set(run.visited)
        for s in self.steps_in(run.biome, run.eid):
            if s.pays and visited & s.goal:
                self._complete(s, day, "banked")
        deadline = time.monotonic() + ITEM_ARRIVAL_S
        for iid, _q in run.pending_items:
            self.expect.append((iid, deadline, me.q, me.r, run.biome, run.eid))

    def on_recipes(self, mask, day):
        for s in self._steps:
            if s.recipe and s.key.endswith(".learn") and mask & (1 << (s.recipe - 1)):
                self._complete(s, day, "banked")

    def on_craft(self, rid, day):
        for s in self._steps:
            if s.recipe == rid and s.key.endswith(".craft"):
                self._complete(s, day, "crafted")

    def on_kane(self, stage, mine, day):
        """A kane_arc: chapter `stage` was read, by us if `mine`.  The arc is
        the world's, so a chapter someone else read is closed for us too --
        it goes on the ledger as theirs (their own journal says done)."""
        for s in self._steps:
            if s.chapter != stage:
                continue
            if mine:
                self._complete(s, day, "read")
            elif s.key not in self.ledger.steps:
                self.ledger.mark(s.key, "reached", "another survivor", day)

    def on_vac(self, w, mine, day):
        """An `ev vac`: the board says a part is done -- the town read
        (hook), a site banked (desk, keys) or the town saved.  Like Kane's
        arc it is the world's, so a part somebody else finished is closed for
        us too and goes on the ledger as theirs."""
        part = VAC_EV_PART.get(w)
        if part is None:
            return
        for s in self._steps:
            if s.vac_part != part:
                continue
            if mine:
                self._complete(s, day, "read" if part in (VAC_PART_SIGN, VAC_PART_OUT)
                               else "banked")
            elif s.key not in self.ledger.steps:
                self.ledger.mark(s.key, "reached", "another survivor", day)

    def check_arrivals(self, obs):
        """banked_item_missing: an item enc_res said we won, banked, and
        neither in the pack, worn, nor lying on the hex we banked on."""
        if not self.expect:
            return
        me = obs.me
        now = time.monotonic()
        keep = []
        for iid, deadline, q, r, biome, eid in self.expect:
            if iid in me.inv_type or iid in me.equip or any(
                    gi.get("id") == iid and (gi.get("q"), gi.get("r")) == (q, r)
                    for gi in obs.ground_items):
                continue
            if now < deadline:
                keep.append((iid, deadline, q, r, biome, eid))
                continue
            summary = (f"{biome}/{eid} rolled the {ITEM_NAME.get(iid, iid)} and we banked, "
                       f"but it is not in the pack, worn or on the hex")
            self.ledger.problem("banked_item_missing", f"{biome}/{eid}", self.who, obs.day, summary)
            self._flag("banked_item_missing", "major", summary, key=(biome, eid, iid))
        self.expect = keep


if __name__ == "__main__":
    cat = build_catalog()
    for q in cat:
        print(f"{q.key}: {q.title} [{q.kind}]{'' if q.reachable else '  UNREACHABLE'}")
        for s in q.steps:
            extra = []
            if s.needs:
                names = " + ".join(ITEM_NAME.get(i, str(i)) for i in s.needed())
                extra.append(f"needs {names}{' (worn)' if s.worn else ''}"
                             f"{'' if s.gated else ' -- NOT GATED'}")
            if s.pays:
                extra.append(f"pays {ITEM_NAME.get(s.pays, s.pays)}")
            files = ", ".join(f"{b}/{e}" for b, e in s.files)
            print(f"    {s.key:<28} {s.title}  [{files}] {'; '.join(extra)}")
    problems = lint(cat)
    print(f"\n{len(problems)} problem(s) in the content:")
    for p in problems:
        print("  -", p)
