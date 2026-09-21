"""What the bots actually did with equipment during a run.

`metrics.py` answers "was it a good game". This answers the narrower question
a gear change needs: did anything get found, worn, and did the bonus land.

Reads the newest run JSONL (or one named on the command line) and reports:

  * every equip / pickup a bot attempted, and whether the server accepted it
  * the loadout each bot finished in, per policy
  * the effective pack size and LL ceiling the server reported, so a bonus
    that was granted but never applied shows up as a number that never moved
  * dawns where a fuel-gated item went unpaid (EVT_DAWN "unf")

Run: python gear_report.py [runs/run-*.jsonl]
"""
import collections
import glob
import json
import sys

from config import EQUIPMENT, EQUIP_SLOTS, EQUIP_STATS

SLOT_NAME = ("head", "body", "hand", "feet", "vehicle")


def item_name(item_id, items):
    return items.get(item_id, f"#{item_id}")


def load_item_names():
    """Names straight out of items.cfg so the report reads in English."""
    import os
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "data", "items.cfg")
    out, cur = {}, None
    try:
        with open(path, encoding="utf-8") as fh:
            for raw in fh:
                line = raw.split("#")[0].strip()
                if line == "[item]":
                    cur = {}
                    continue
                if cur is None or "=" not in line:
                    continue
                k, v = (x.strip() for x in line.split("=", 1))
                cur[k] = v
                if "id" in cur and "name" in cur:
                    out[int(cur["id"])] = cur["name"]
    except OSError:
        pass
    return out


def main():
    paths = sys.argv[1:] or sorted(glob.glob("runs/run-*.jsonl"))[-1:]
    if not paths:
        print("no run files found in runs/")
        return 1
    items = load_item_names()

    # arch -> what we know about that bot
    policy = {}
    attempts = collections.defaultdict(list)   # arch -> [kind] (from tx)
    reasons = collections.defaultdict(list)    # arch -> [why]  (from decide)
    acks = collections.defaultdict(list)       # arch -> [item_result dicts]
    loadout = {}                               # arch -> last eq[] seen
    packs = collections.defaultdict(set)       # arch -> {is values seen}
    caps = collections.defaultdict(set)        # arch -> {llCap values seen}
    unfuelled = collections.Counter()          # arch -> dawns with unf != 0
    dawns = collections.Counter()
    mode = set()
    conn_err = collections.Counter()

    for path in paths:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue            # truncated tail on a killed run
                ch, arch, d = rec.get("ch"), rec.get("arch", -1), rec.get("d")
                if ch == "joined" and isinstance(d, dict):
                    policy[arch] = d.get("policy", "?")
                elif ch == "run" and isinstance(d, dict):
                    # The opening "run" record carries bots as a count; only
                    # the closing summary carries the per-bot list.
                    bots = d.get("bots")
                    if isinstance(bots, list):
                        for b in bots:
                            policy.setdefault(b.get("arch"), b.get("policy", "?"))
                    if d.get("mode"):        # only the opening record has it
                        mode.add(d["mode"])
                # `tx` is the authoritative record of what was actually sent;
                # `decide` carries the same action's rationale. Counting both
                # double-counts every equip, so tx counts and decide explains.
                elif ch == "tx" and isinstance(d, dict):
                    if d.get("t") in ("equip_item", "pickup_item"):
                        attempts[arch].append(d["t"])
                elif ch == "decide" and isinstance(d, dict):
                    if d.get("kind") in ("equip_item", "pickup_item"):
                        reasons[arch].append(d.get("why", ""))
                elif ch == "rx" and isinstance(d, dict):
                    t = d.get("t")
                    if t == "item_result":
                        acks[arch].append(d)
                        if isinstance(d.get("eq"), list):
                            loadout[arch] = list(d["eq"])
                        if d.get("is") is not None:
                            packs[arch].add(d["is"])
                        if d.get("llCap") is not None:
                            caps[arch].add(d["llCap"])
                    elif t == "ev" and d.get("k") == "dawn":
                        pid = d.get("pid", arch)
                        dawns[pid] += 1
                        if d.get("unf"):
                            unfuelled[pid] += 1
                elif ch == "conn_err" and isinstance(d, dict):
                    conn_err[str(d.get("err", "?")).split(":")[0]] += 1
                elif ch == "rx_s" and isinstance(d, dict):
                    if isinstance(d.get("eq"), list):
                        loadout[arch] = list(d["eq"])
                    if d.get("is") is not None:
                        packs[arch].add(d["is"])
                    if d.get("llcap") is not None:
                        caps[arch].add(d["llcap"])

    print(f"gear report over {len(paths)} run file(s)"
          + (f"   mode={'/'.join(sorted(m for m in mode if m))}" if mode else "") + "\n")

    everyone = sorted(set(policy) | set(attempts) | set(acks) | set(loadout)
                      | set(reasons))
    if not everyone:
        print("  no bot activity recorded")
        return 0

    total_equips = total_pickups = total_ok = total_refused = 0
    for arch in everyone:
        pol = policy.get(arch, "?")
        att = attempts.get(arch, [])
        eq_n = att.count("equip_item")
        pk_n = att.count("pickup_item")
        ok_n = sum(1 for a in acks.get(arch, [])
                   if a.get("act") in ("equip", "pickup") and a.get("ok"))
        bad_n = sum(1 for a in acks.get(arch, [])
                    if a.get("act") in ("equip", "pickup") and not a.get("ok"))
        total_equips += eq_n
        total_pickups += pk_n
        total_ok += ok_n
        total_refused += bad_n

        worn = loadout.get(arch, [0] * EQUIP_SLOTS)
        wear = ", ".join(f"{SLOT_NAME[i]}={item_name(v, items)}"
                         for i, v in enumerate(worn) if v) or "nothing"
        print(f"  slot {arch}  {pol:<13} equip x{eq_n}  pickup x{pk_n}  "
              f"accepted {ok_n}  refused {bad_n}")
        print(f"            wearing: {wear}")
        if packs.get(arch):
            lo, hi = min(packs[arch]), max(packs[arch])
            print(f"            pack size {lo}..{hi}"
                  + ("   <-- never moved" if lo == hi else "   <-- gear moved it"))
        if caps.get(arch):
            lo, hi = min(caps[arch]), max(caps[arch])
            print(f"            LL ceiling {lo}..{hi}"
                  + ("   <-- never moved" if lo == hi else "   <-- gear moved it"))
        if dawns.get(arch):
            print(f"            dawns {dawns[arch]}, unfuelled {unfuelled.get(arch, 0)}")
        for why in reasons.get(arch, [])[:8]:
            print(f"              - {why}")
        if len(reasons.get(arch, [])) > 8:
            print(f"              ... and {len(reasons[arch]) - 8} more")
        print()

    print(f"  totals: {total_equips} equips, {total_pickups} pickups, "
          f"{total_ok} accepted, {total_refused} refused")
    if conn_err:
        print(f"  connection errors: {dict(conn_err)}")
    if total_equips == 0 and total_pickups == 0:
        print("\n  NOTHING WAS EQUIPPED. Either no equipment dropped (it comes")
        print("  from POI encounter loot, so a run where nobody opens a POI")
        print("  will find none), or gear_action() is not being reached.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
