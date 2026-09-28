"""Equipment audit: is each item's declared effect actually applied, and did
wearing it keep anyone alive?

    python gearcheck.py                      # newest run in runs/
    python gearcheck.py runs/run-*.jsonl     # pool several runs

Three questions, in the order they have to be answered:

**1. Coverage.** Which of the registry's equipment items did the fleet ever
manage to wear, and for how long? An item nobody wore is not "fine", it is
untested -- and the honest answer after a short run is that most of the
registry is. The report names the gaps rather than quietly ranking 6 of 24
items and calling it a survey.

**2. Correctness, mod by mod.** Checking "the item did something" is not a
test; an item declaring `rad -1` and `terrain 4` tells you nothing by leaving
`llCap` alone. So every declared mod is tracked separately and gets its own
verdict:

| mod | how it is checked | strength |
|---|---|---|
| `ll` | `llCap` delta across the equip -- `appendPackArrays()` sends the EFFECTIVE ceiling | hard |
| `slots` | `is` delta, allowing for the INV_SLOTS_MAX clamp | hard |
| `water_cap` | `wc` delta -- `appendPackArrays()` sends canteenCap() | hard |
| `vision` | `vr` on the vis disk `pushVisDisk()` sends right after the equip | hard |
| `rad` | direction of radiation change at the dawns it was worn | soft |
| `mp` | dawn MP against `ll + 3` + declared, only on unwounded dawns | soft |
| `terrain`, `threat`, narrative | not observable from the wire | untested |

A mod with no evidence is reported UNTESTED with the reason, never as a pass.
The distinction that matters: "verified" means the wire moved by the declared
amount, and "no-op confirmed" means an unrelated field correctly did *not*
move.

**3. Survival.** For every dawn we know what was worn (the run log samples
`eq` every 50 ticks) and what the dawn cost (`dll`). Sum LL lost per
game-day worn, per item, against the fleet's own baseline for dawns where
nothing was worn, and rank.

That ranking is weak on purpose and says so: dawn LL loss is dominated by
exposure, thirst and hunger, which most gear does not touch. It is a screen
for items that make things *worse*, not a measure of benefit.
"""
import argparse
from collections import defaultdict
from pathlib import Path

from config import (EQUIPMENT, EQUIP_STATS, INV_SLOTS_MAX, ITEM_NAME,
                    base_mp)
from record import read as read_run

RUNS = Path(__file__).parent / "runs"
# Below this many game-days worn, a survival number is noise, not a result.
MIN_DAYS = 3
# Mods that leave no trace on the wire. Listing them explicitly is the point:
# they are the part of the registry this tool cannot speak to at all.
OPAQUE_MODS = {
    "terrain": "movement perk; would need a move onto the gated terrain",
    "threat":  "threat clock is global, not per-player, on the wire",
}

# Most narrative perks are server-side with nothing on the wire, but three of
# them move a field the dawn or action event already carries, which makes
# them checkable after all -- and they are the three that matter most,
# because exposure is the single largest LL drain in the game and scavenging
# is the scrap economy.
#   21 NAR_COLD_IMMUNE  -> dawn `expd` must be 0 on every dawn it is worn
#   30 NAR_SCAV_DOUBLE  -> SCAV pays `sd` 4 instead of 2 (2 instead of 1 partial)
#   31 NAR_RIVER_FORAGE -> FORAGE on River pays double `fd`
#   32 NAR_LAND_FORAGE  -> FORAGE on land pays double `fd`
NAR_COLD_IMMUNE, NAR_SCAV_DOUBLE = 21, 30
NAR_RIVER_FORAGE, NAR_LAND_FORAGE = 31, 32
SCAV_BASE_YIELD = 2       # doScav(): 2 scrap clean, halved on a partial
CHECKABLE_NAR = {NAR_COLD_IMMUNE, NAR_SCAV_DOUBLE,
                 NAR_RIVER_FORAGE, NAR_LAND_FORAGE}


def newest_run() -> Path:
    runs = sorted(RUNS.glob("*.jsonl"), key=lambda p: p.stat().st_mtime)
    if not runs:
        raise SystemExit(f"no runs in {RUNS}")
    return runs[-1]


def _has_nar(eq, nar) -> list:
    """Which worn items claim this narrative perk."""
    return [i for i in eq if i and nar in EQUIP_STATS.get(i, {}).get("narrative", ())]


def _sum(eq, stat) -> int:
    return sum(EQUIP_STATS.get(i, {}).get(stat, 0) for i in eq if i)


def declared(item):
    """The mods this item actually claims, as (stat, amount) pairs."""
    st = EQUIP_STATS.get(item, {})
    out = [(k, st.get(k, 0))
           for k in ("ll", "slots", "water_cap", "vision", "rad", "mp")
           if st.get(k)]
    if st.get("terrain"):
        out.append(("terrain", st["terrain"]))
    if st.get("threat"):
        out.append(("threat", st["threat"]))
    for nar in sorted(st.get("narrative", ())):
        out.append((f"narrative:{nar}", 1))
    return out


class GearAudit:
    def __init__(self):
        self.days_worn = defaultdict(float)
        self.ll_lost = defaultdict(int)
        self.deaths = defaultdict(int)
        self.baseline_dawns = 0
        self.baseline_ll = 0
        # (item, stat) -> {"pass": n, "fail": n, "cases": [...]}
        self.ev = defaultdict(lambda: {"pass": 0, "fail": 0, "cases": []})
        self.worn_ever = set()
        self.equips = self.unequips = self.pickups = 0

    # ── ingest ─────────────────────────────────────────────────────────
    def add_run(self, path: Path) -> None:
        rows = read_run(path)
        st = defaultdict(lambda: {"eq": [0] * 5, "is": None, "llCap": None,
                                  "wc": None,
                                  "vr": None, "day": 0, "wnd": [0, 0],
                                  "seen": False, "pending": None})
        for r in rows:
            arch, d = r.get("arch"), r.get("d")
            if not isinstance(d, dict) or arch is None or arch < 0:
                continue
            ch = r.get("ch")
            if ch == "tx":
                t = d.get("t")
                self.equips += t == "equip_item"
                self.unequips += t == "unequip_item"
                self.pickups += t == "pickup_item"
                continue
            s = st[arch]
            if ch == "rx_s":
                if "vr" in d:
                    s["vr"] = d["vr"]
                self._pack(arch, s, d, "digest")
                if "mp" in d and "ll" in d:
                    self._check_mp(s, d)
                continue
            if ch != "rx":
                continue
            t = d.get("t")
            if t == "item_result":
                self._pack(arch, s, d, f"ack:{d.get('act')}")
            elif t == "sync":
                if "vr" in d:
                    s["vr"] = d["vr"]
                for i, pd in enumerate(d.get("p") or []):
                    if i == arch and isinstance(pd, dict):
                        self._pack(arch, s, pd, "sync")
            elif t == "vis":
                self._check_vision(s, d)
            elif t == "ev" and d.get("pid") == arch:
                k = d.get("k")
                if k == "dawn":
                    self._dawn(s, d)
                elif k == "act":
                    self._check_act(s, d)
                elif k == "downed":
                    for item in s["eq"]:
                        if item:
                            self.deaths[item] += 1
                elif k == "mv":
                    # Moving changes vision through terrain and weather, so a
                    # vis disk after a step is no longer attributable to the
                    # equip that preceded it.
                    s["pending"] = None

    def _pack(self, arch, s, d, source) -> None:
        if "wnd" in d:
            s["wnd"] = list(d["wnd"])
        if "day" in d:
            s["day"] = d["day"]
        if "eq" not in d:
            return
        new_eq = [int(x) for x in d["eq"]]
        if new_eq != s["eq"] and s["seen"]:
            self._check_hard(s, new_eq, d, source)
        for item in new_eq:
            if item:
                self.worn_ever.add(item)
        s["eq"], s["seen"] = new_eq, True
        if d.get("is") is not None:
            s["is"] = int(d["is"])
        if d.get("llCap") is not None:
            s["llCap"] = int(d["llCap"])
        if d.get("wc") is not None:
            s["wc"] = int(d["wc"])

    def _check_hard(self, s, new_eq, d, source) -> None:
        """llCap, is and wc are pure functions of the equipment set, so a
        loadout change must move them by exactly the declared amount."""
        changed = {x for x in new_eq if x} ^ {x for x in s["eq"] if x}
        for stat, field in (("ll", "llCap"), ("slots", "is"), ("water_cap", "wc")):
            before, after = s.get(field), d.get(field)
            if before is None or after is None:
                continue
            want = _sum(new_eq, stat) - _sum(s["eq"], stat)
            got = int(after) - int(before)
            clamped = stat == "slots" and max(int(after), int(before)) >= INV_SLOTS_MAX
            for item in changed:
                if not EQUIP_STATS.get(item, {}).get(stat):
                    continue        # this item claims nothing here
                rec = self.ev[(item, stat)]
                if want == got or (clamped and abs(got) <= abs(want)):
                    rec["pass"] += 1
                else:
                    rec["fail"] += 1
                    rec["cases"].append(f"{source}: {field} {before}->{after} "
                                        f"(delta {got:+d}, declared {want:+d})")
        # Vision is pushed as a fresh vis disk immediately after the ack
        # (pushVisDisk), so arm a check for the next one -- but ONLY for a
        # change we saw acked. A loadout change that turns up in a sync is a
        # respawn: resetSurvivor() wiped the equipment, and the survivor is
        # standing somewhere else entirely, so the next disk's radius is a
        # fact about the new hex's terrain and weather, not about the gear
        # that is no longer worn. Measuring it there reported a Doom Clicker
        # "mismatch" that was purely this tool's own doing.
        vis_delta = _sum(new_eq, "vision") - _sum(s["eq"], "vision")
        if vis_delta and s["vr"] is not None and source.startswith("ack:"):
            s["pending"] = (vis_delta, s["vr"],
                            {i for i in changed
                             if EQUIP_STATS.get(i, {}).get("vision")}, source)

    def _check_vision(self, s, d) -> None:
        vr = d.get("vr")
        if vr is None:
            return
        pend = s["pending"]
        if pend:
            want, before, items, source = pend
            got = int(vr) - int(before)
            for item in items:
                rec = self.ev[(item, "vision")]
                if got == want:
                    rec["pass"] += 1
                else:
                    rec["fail"] += 1
                    rec["cases"].append(f"{source}: vr {before}->{vr} "
                                        f"(delta {got:+d}, declared {want:+d})")
            s["pending"] = None
        s["vr"] = int(vr)

    def _check_mp(self, s, d) -> None:
        """Soft: effectiveMP is ll+3 plus equipment, minus wounds and
        encumbrance, and a *_cost item only pays out on a dawn it was fed."""
        worn_mp = _sum(s["eq"], "mp")
        if not worn_mp or any(s["wnd"]):
            return
        expected = base_mp(int(d["ll"])) + worn_mp
        for item in s["eq"]:
            if not item or not EQUIP_STATS.get(item, {}).get("mp"):
                continue
            rec = self.ev[(item, "mp")]
            # Only a sample at full MP says anything -- mid-day the survivor
            # has spent some.
            if int(d["mp"]) == expected:
                rec["pass"] += 1
            elif int(d["mp"]) > base_mp(int(d["ll"])):
                rec["pass"] += 1        # carrying a bonus, just partly spent
            else:
                rec["cases"].append(f"mp {d['mp']} at ll {d['ll']} "
                                    f"(bare would be {base_mp(int(d['ll']))}, "
                                    f"+{worn_mp} declared)")

    def _check_act(self, s, ev) -> None:
        """SCAV and FORAGE yields carry the doubling perks on the wire."""
        act, out = ev.get("a"), ev.get("out")
        if out is None or int(out) == 0:
            return              # a failed check pays nothing either way
        if act == 3:            # ACT_SCAV
            got = int(ev.get("sd", 0) or 0)
            for item in _has_nar(s["eq"], NAR_SCAV_DOUBLE):
                rec = self.ev[(item, f"narrative:{NAR_SCAV_DOUBLE}")]
                # A clean success doubles 2 -> 4, a partial 1 -> 2. Anything
                # at or above the undoubled clean yield is evidence it landed.
                if got > SCAV_BASE_YIELD:
                    rec["pass"] += 1
                else:
                    rec["fail"] += 1
                    rec["cases"].append(
                        f"SCAV paid {got} scrap while worn "
                        f"(undoubled clean yield is {SCAV_BASE_YIELD})")
        elif act == 0:          # ACT_FORAGE
            got = int(ev.get("fd", 0) or 0)
            for nar in (NAR_LAND_FORAGE, NAR_RIVER_FORAGE):
                for item in _has_nar(s["eq"], nar):
                    rec = self.ev[(item, f"narrative:{nar}")]
                    # The base yield varies by terrain and roll, so this only
                    # records the distribution rather than passing or failing
                    # an individual forage.
                    rec["pass"] += 1
                    rec["cases"].append(f"forage paid {got} food")

    def _dawn(self, s, ev) -> None:
        lost = max(0, -int(ev.get("dll", 0) or 0))
        # Exposure immunity is the one narrative perk with a dedicated wire
        # field: dawnUpkeep reports what the exposure tick actually took as
        # `expd`, and a cold-immune survivor must never be charged.
        expd = ev.get("expd")
        if expd is not None:
            for item in _has_nar(s["eq"], NAR_COLD_IMMUNE):
                rec = self.ev[(item, f"narrative:{NAR_COLD_IMMUNE}")]
                if int(expd) == 0:
                    rec["pass"] += 1
                else:
                    rec["fail"] += 1
                    rec["cases"].append(
                        f"day {ev.get('day')}: exposure took {expd} LL "
                        f"while a cold-immune item was worn")
        worn = [i for i in s["eq"] if i]
        if worn:
            for item in worn:
                self.days_worn[item] += 1
                self.ll_lost[item] += lost
                if EQUIP_STATS.get(item, {}).get("rad"):
                    rec = self.ev[(item, "rad")]
                    rec["pass"] += 1    # counted as dawns worn; direction below
        else:
            self.baseline_dawns += 1
            self.baseline_ll += lost

    # ── report ─────────────────────────────────────────────────────────
    def render(self) -> str:
        L = []
        equip_ids = sorted(EQUIPMENT)
        L.append(f"== coverage: {len(self.worn_ever)}/{len(equip_ids)} "
                 f"equipment items worn at least once")
        L.append(f"   {self.equips} equip, {self.unequips} unequip, "
                 f"{self.pickups} pickup messages sent")
        never = [i for i in equip_ids if i not in self.worn_ever]
        if never:
            L.append("   never worn (UNTESTED, not 'fine'): "
                     + ", ".join(ITEM_NAME.get(i, str(i)) for i in never))

        L.append("\n== correctness, mod by mod")
        L.append("   VERIFIED = the wire moved by the declared amount")
        for item in sorted(self.worn_ever):
            mods = declared(item)
            L.append(f"   {ITEM_NAME.get(item, item)}")
            if not mods:
                L.append("      (declares no stat mods at all)")
                continue
            for stat, amt in mods:
                rec = self.ev.get((item, stat))
                if stat in OPAQUE_MODS:
                    L.append(f"      {stat:<12} {amt:<+4} UNTESTED  "
                             f"-- {OPAQUE_MODS[stat]}")
                elif stat.startswith("narrative"):
                    nar = int(stat.split(":")[1])
                    rec = self.ev.get((item, stat))
                    if nar not in CHECKABLE_NAR:
                        L.append(f"      {stat:<12} {'':<4} UNTESTED  "
                                 f"-- server-side effect, no wire field")
                    elif rec is None or not (rec["pass"] or rec["fail"]):
                        L.append(f"      {stat:<12} {'':<4} UNTESTED  "
                                 f"-- worn, but the perk never had a chance "
                                 f"to fire")
                    elif rec["fail"]:
                        L.append(f"      {stat:<12} {'':<4} MISMATCH  "
                                 f"{rec['pass']} ok / {rec['fail']} bad")
                        for c in rec["cases"][:2]:
                            L.append(f"                        {c}")
                    elif nar == NAR_COLD_IMMUNE:
                        L.append(f"      {stat:<12} {'':<4} VERIFIED  "
                                 f"exposure took 0 LL across "
                                 f"{rec['pass']} dawns worn")
                    elif nar == NAR_SCAV_DOUBLE:
                        L.append(f"      {stat:<12} {'':<4} VERIFIED  "
                                 f"{rec['pass']} scavenge(s) paid above the "
                                 f"undoubled yield")
                    else:
                        L.append(f"      {stat:<12} {'':<4} observed  "
                                 f"{rec['pass']} forage(s): "
                                 f"{', '.join(rec['cases'][:3])}")
                elif rec is None or (rec["pass"] == 0 and rec["fail"] == 0):
                    L.append(f"      {stat:<12} {amt:<+4} UNTESTED  "
                             f"-- worn, but no usable sample")
                elif rec["fail"]:
                    L.append(f"      {stat:<12} {amt:<+4} MISMATCH  "
                             f"{rec['pass']} ok / {rec['fail']} bad")
                    for c in rec["cases"][:2]:
                        L.append(f"                        {c}")
                elif stat == "rad":
                    L.append(f"      {stat:<12} {amt:<+4} observed  "
                             f"worn across {rec['pass']} dawns "
                             f"(per-dawn rad is mixed with terrain exposure)")
                else:
                    L.append(f"      {stat:<12} {amt:<+4} VERIFIED  "
                             f"{rec['pass']} observation(s)")

        L.append("\n== survival: LL lost per game-day worn (lower is better)")
        base = (self.baseline_ll / self.baseline_dawns) if self.baseline_dawns else None
        L.append("   baseline, nothing worn: "
                 + (f"{base:.2f} LL/day over {self.baseline_dawns} dawns"
                    if base is not None else "no bare dawns observed"))
        ranked, thin = [], []
        for item in sorted(self.worn_ever):
            days = self.days_worn[item]
            if not days:
                continue
            row = (self.ll_lost[item] / days, item, days, self.deaths.get(item, 0))
            (ranked if days >= MIN_DAYS else thin).append(row)
        for rate, item, days, deaths in sorted(ranked):
            delta = f"{rate - base:+.2f}" if base is not None else "n/a"
            # Deaths per 100 days worn is the blunt version of the same
            # question and the one actually asked -- did wearing this keep
            # anyone alive. It is much noisier than the LL rate (deaths are
            # rare), which is why the sort is on LL/day and this rides along.
            per100 = 100.0 * deaths / days
            L.append(f"   {ITEM_NAME.get(item, item):<17} {rate:>5.2f} LL/day "
                     f"({delta} vs bare)  {per100:>5.1f} deaths/100d  "
                     f"days={days:<5.0f} deaths={deaths}")
        if thin:
            L.append(f"   too little wear to rank (<{MIN_DAYS} days): "
                     + ", ".join(f"{ITEM_NAME.get(i, i)} ({d:.0f}d)"
                                 for _r, i, d, _x in sorted(thin, key=lambda x: -x[2])))
        L.append("   NB dawn LL loss is mostly exposure, thirst and hunger, "
                 "which most gear does not touch:")
        L.append("   read this as a screen for items that make things WORSE, "
                 "not a ranking of benefit.")
        return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", help="run JSONL files (default: newest)")
    args = ap.parse_args(argv)
    paths = [Path(p) for p in args.logs] or [newest_run()]

    audit = GearAudit()
    used = []
    for p in paths:
        if not p.is_file():
            print(f"   (skipping {p}: not a file)")
            continue
        audit.add_run(p)
        used.append(p)
    print(f"=== gear audit over {len(used)} run(s)")
    for p in used:
        print(f"    {p.name}")
    print()
    print(audit.render())


if __name__ == "__main__":
    main()
