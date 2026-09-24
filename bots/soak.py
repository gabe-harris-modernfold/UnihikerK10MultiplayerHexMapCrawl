"""Soak: a long realtime run with the world clock actually ticking.

An all-bot fleet rests the moment it runs out of MP, tickGame() ends the day
as soon as everyone is resting, and so a 5-minute day collapses to seconds.
The supply economy is measured fine that way; everything on a real clock --
weather (weatherNextGapMs), fire, flood, Creeping Doom (WORLD_TICK_INTERVAL)
-- is barely sampled.  A soak puts a Sentinel (policy/sentinel.py) in the
fleet: it makes camp, builds a shelter, stays awake for 90% of each day and
only then rests in its shelter, so days run ~4.5 of their 5 minutes and it
survives the nights.  Then the board runs for as long as you give it.

It is also the long-haul health test: memory, tick time and event loss over
an hour are a different question from over seven minutes.

    python soak.py --host k10.local --minutes 60
    python soak.py --host k10.local --minutes 120 --policies sentinel,coward
    python soak.py --report runs/run-...jsonl       # re-read an old soak

The run itself is an ordinary arena run (score target off, realtime), so its
log works with metrics.py and findings.py as well as the report below.
"""
import argparse
import asyncio
import sys
from collections import Counter
from pathlib import Path

import arena
from causes import _dedupe
from config import DAY_TICKS, TICK_MS, WEATHER_NAME
from findings import Finding, FindingLog
from record import Recorder, read as read_run

FULL_DAY_MIN = DAY_TICKS * TICK_MS / 60000.0          # 5.0
# Days shorter than this mean the Sentinel was not holding them open.
SHORT_DAY_MIN = FULL_DAY_MIN * 0.8
# A free-heap trend worse than this over a long enough window is a leak.
LEAK_BYTES_PER_MIN = -300
LEAK_MIN_WINDOW_MIN = 20


def slope_per_min(points) -> float | None:
    """Least-squares slope of (seconds, value) points, per minute."""
    if len(points) < 3:
        return None
    n = len(points)
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mx) ** 2 for p in points)
    if sxx == 0:
        return None
    sxy = sum((p[0] - mx) * (p[1] - my) for p in points)
    return sxy / sxx * 60.0


def soak_report(rows, findings: FindingLog | None = None, out=None) -> dict:
    """What a soak is for, read back out of its log."""
    out = out or sys.stdout
    tele = [(r["ts"], r["d"]) for r in rows if r["ch"] == "telemetry" and "err" not in r["d"]]
    span_s = (rows[-1]["ts"] - rows[0]["ts"]) if rows else 0.0
    minutes = span_s / 60.0

    # -- days: from dawn events, deduped across bots ---------------------------
    dawns = {}
    for ts, d in _dedupe(rows, {"dawn"}):
        day = d.get("day")
        if isinstance(day, int) and day not in dawns:
            dawns[day] = ts
    days = sorted(dawns.items())
    lengths = [(b[1] - a[1]) / 60.0 for a, b in zip(days, days[1:]) if b[0] == a[0] + 1]
    mean_day = sum(lengths) / len(lengths) if lengths else None
    short = sum(1 for x in lengths if x < SHORT_DAY_MIN)

    # -- weather ---------------------------------------------------------------
    phases = Counter(r["d"].get("wp") for r in rows
                     if r["ch"] == "rx_s" and r["d"].get("wp") is not None)
    changes = sum(1 for _ts, _d in _dedupe(rows, {"weather"}))

    # -- real-clock hazards, per real minute -------------------------------------
    hz = Counter()
    for _ts, d in _dedupe(rows, {"fire_dmg", "flood_dmg", "doom_act", "dmg"}):
        k = d.get("k")
        if k == "fire_dmg":
            hz["lightning" if d.get("intensity") == 10 else "fire"] += 1
        elif k == "flood_dmg":
            hz["flood"] += 1
        elif k == "doom_act" and d.get("llLost", 0) > 0:
            hz["creeping doom"] += 1
        elif k == "dmg":
            hz[d.get("cause", "?")] += 1

    # -- board health over time ----------------------------------------------------
    heap_pts = [(ts, d["heap"]) for ts, d in tele if isinstance(d.get("heap"), int)]
    heap_slope = slope_per_min(heap_pts)
    heap_window = (heap_pts[-1][0] - heap_pts[0][0]) / 60.0 if len(heap_pts) > 1 else 0
    min_heap = min((d["minHeap"] for _ts, d in tele if isinstance(d.get("minHeap"), int)),
                   default=None)
    max_tick = max((d["maxTickMs"] for _ts, d in tele if isinstance(d.get("maxTickMs"), int)),
                   default=None)
    drops = max((d["evtDrops"] for _ts, d in tele if isinstance(d.get("evtDrops"), int)),
                default=None)
    reboots = [r["d"] for r in rows if r["ch"] == "board_reboot"]

    rep = {"minutes": round(minutes, 1), "days": len(days),
           "mean_day_min": round(mean_day, 2) if mean_day else None,
           "short_days": short, "weather_changes": changes,
           "weather_time": {WEATHER_NAME[w] if isinstance(w, int) and w < len(WEATHER_NAME)
                            else str(w): n for w, n in phases.items()},
           "hazards": dict(hz),
           "hazards_per_min": {k: round(v / minutes, 3) for k, v in hz.items()} if minutes else {},
           "heap_slope_b_per_min": round(heap_slope, 1) if heap_slope is not None else None,
           "heap_window_min": round(heap_window, 1), "min_heap": min_heap,
           "max_tick_ms": max_tick, "evt_drops": drops, "reboots": len(reboots)}

    print(f"  soak: {rep['minutes']} min, {rep['days']} day(s), "
          f"mean day {rep['mean_day_min']} min (full = {FULL_DAY_MIN:.0f}), "
          f"{short} short", file=out)
    total = sum(phases.values()) or 1
    wt = ", ".join(f"{k} {100 * v / total:.0f}%" for k, v in rep["weather_time"].items())
    print(f"  weather: {changes} change(s); time in phase: {wt or '-'}", file=out)
    print(f"  hazards: " + (", ".join(f"{k} {v} ({rep['hazards_per_min'].get(k, 0)}/min)"
                                      for k, v in hz.most_common()) or "none"), file=out)
    print(f"  board: heap trend {rep['heap_slope_b_per_min']} B/min over "
          f"{rep['heap_window_min']} min, minHeap {min_heap}, maxTick {max_tick} ms, "
          f"evtDrops {drops}, reboots {len(reboots)}", file=out)

    if findings is not None:
        if lengths and short > len(lengths) / 2:
            findings.add(Finding(
                check="soak_days_collapsed", severity="minor",
                summary="most days ended early: the Sentinel was not holding them open",
                source="soak", detail={"mean_day_min": rep["mean_day_min"],
                                       "short": short, "days": len(lengths)}))
        if (heap_slope is not None and heap_window >= LEAK_MIN_WINDOW_MIN
                and heap_slope < LEAK_BYTES_PER_MIN):
            findings.add(Finding(
                check="heap_trend", severity="major",
                summary=f"free heap falling {heap_slope:.0f} B/min over "
                        f"{heap_window:.0f} min -- a leak",
                source="soak", detail={"slope": heap_slope, "window_min": heap_window,
                                       "min_heap": min_heap}))
    return rep


async def main_async(a) -> int:
    argv = ["--host", a.host, "--bots", str(a.bots), "--policies", a.policies,
            "--mode", "realtime", "--target", "0", "--runs", "1",
            "--max-minutes", str(a.minutes), "--seed", str(a.seed),
            "--telemetry-interval", str(a.telemetry_interval)]
    if a.no_reset:
        argv.append("--no-reset")
    args = arena.parse_args(argv)
    if "sentinel" not in args.policies.split(","):
        print("  note: no sentinel in the fleet -- days will collapse whenever "
              "everyone rests, which defeats the point of a soak")
    arena.RUNS_DIR.mkdir(parents=True, exist_ok=True)
    summary = await arena.run_once(args, 1, a.seed)
    log = summary.get("log")
    if not log or summary.get("reason") == "reset_failed":
        return 2
    rows = read_run(log)
    # Append the soak's own verdicts to the same log so findings.py sees them.
    with Recorder(Path(log).with_suffix(".soak.jsonl"), {"kind": "soak", "log": log}) as rec:
        fl = FindingLog(rec)
        rep = soak_report(rows, fl)
        rec.write("soak", -1, rep)
        fl.print_summary()
        fl.flush()
    return 1 if (summary.get("findings") or len(fl)) else 0


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="k10.local")
    p.add_argument("--minutes", type=float, default=60.0)
    p.add_argument("--bots", type=int, default=3)
    p.add_argument("--policies", default="sentinel,scoremax,coward")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--telemetry-interval", type=float, default=15.0,
                   dest="telemetry_interval")
    p.add_argument("--no-reset", action="store_true")
    p.add_argument("--report", metavar="LOG",
                   help="skip the run; print the soak report for an existing log")
    return p.parse_args(argv)


if __name__ == "__main__":
    a = parse_args()
    if a.report:
        soak_report(read_run(a.report))
        sys.exit(0)
    sys.exit(asyncio.run(main_async(a)))
