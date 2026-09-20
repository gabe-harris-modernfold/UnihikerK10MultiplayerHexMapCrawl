"""Post-run analysis over the recorded JSONL.

    python metrics.py                     # newest run in runs/
    python metrics.py runs/run-*.jsonl    # specific runs
    python metrics.py --json out.json

What it measures and why -- these are chosen to say something about whether
the game is *entertaining*, which win rate does not:

  score spread       identical policies finishing far apart means spawn and
                     map roll decide the game, not play
  snowball           day-N standings vs final standings; high agreement means
                     the game is settled before it gets interesting
  check margins      every roll is already on the wire (broadcastCheck emits
                     dn/tot/sv/mod). A fat cluster near margin 0 is tension;
                     everything clearing by +5 means no one was ever at risk
  action mix         if one verb is most of the messages, the others are
                     decoration
  resource slack     if the pack is never tight, the player is never choosing
  crises             a shortage you always recover from is not a crisis; one
                     you never recover from is a death spiral. ~60-75% is the
                     interesting band
  POI reach          how many of the map's POIs anyone actually got to

Event caveat that shapes all of this: `ev` messages are broadcast to every
client via ws.textAll(), so the same roll appears once per connected bot in
the log.  Everything below dedupes on (ts, pid) before counting.
"""
import argparse
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path

from config import ACT_NAME, ARCHETYPE_NAME, RES_NAME, SK_NAME
from record import read as read_run

RUNS_DIR = Path(__file__).parent / "runs"
WATER, FOOD = 0, 1

# Design target (stated 2026-09-19): a good tension arc across two hours of
# play, with a character coming close to dying at most four times in that
# window.
TARGET_HOURS = 2.0
TARGET_BRUSHES = 4
NEAR_DEATH_LL = 2
# A day ends the moment every connected player rests, so its real length is
# set by how long a player takes to spend ~10 MP (six or seven moves), not by
# DAY_TICKS. 3 real minutes per day is the working estimate for a human at a
# deliberate pace; DAY_TICKS caps it at 5 and a brisk player is nearer 2.
EST_MINUTES_PER_DAY = 3.0


def _dedupe_events(rows, kind):
    """One copy of each broadcast event, keyed on (timestamp, pid)."""
    seen, out = set(), []
    for r in rows:
        if r["ch"] != "rx":
            continue
        d = r["d"]
        if d.get("t") != "ev" or d.get("k") != kind:
            continue
        key = (r["ts"], d.get("pid"), d.get("tot"), d.get("dn"))
        if key in seen:
            continue
        seen.add(key)
        out.append((r["ts"], d))
    return out


class RunReport:
    def __init__(self, path):
        self.path = Path(path)
        self.rows = read_run(path)
        meta = [r["d"] for r in self.rows if r["ch"] == "run"]
        self.meta = meta[0] if meta else {}
        self.summary = meta[-1] if len(meta) > 1 else {}
        self.bots = {b["arch"]: b for b in self.summary.get("bots", [])
                     if b.get("arch", -1) >= 0}

    # --- per-bot trajectories -------------------------------------------
    def trajectories(self):
        """arch -> [digest, ...] in time order, from the sampled broadcast."""
        traj = defaultdict(list)
        for r in self.rows:
            if r["ch"] == "rx_s":
                traj[r["arch"]].append(r["d"])
        return traj

    def label(self, arch):
        b = self.bots.get(arch)
        if b:
            return b["label"]
        return f"{arch}:{ARCHETYPE_NAME[arch]}" if 0 <= arch < 6 else str(arch)

    # --- the metrics ----------------------------------------------------
    def score_spread(self):
        scores = {a: b["score"] for a, b in self.bots.items()}
        if len(scores) < 2:
            return {"scores": scores}
        vals = list(scores.values())
        return {"scores": scores, "min": min(vals), "max": max(vals),
                "spread": max(vals) - min(vals),
                "mean": round(statistics.mean(vals), 1),
                "stdev": round(statistics.pstdev(vals), 1),
                "ratio": round(max(vals) / max(1, min(vals)), 2)}

    def snowball(self, early_day=5):
        """Standings at `early_day` against final standings.

        Reported as the fraction of bot pairs ordered the same way early as at
        the end. 1.0 means the finish was fully decided by early_day.
        """
        traj = self.trajectories()
        early = {}
        for arch, ds in traj.items():
            for d in ds:
                if d.get("day", 0) >= early_day:
                    early[arch] = d.get("sc", 0)
                    break
        final = {a: b["score"] for a, b in self.bots.items()}
        common = sorted(set(early) & set(final))
        if len(common) < 2:
            return {"early_day": early_day, "pairs": 0, "agreement": None,
                     "early": early}
        agree = total = 0
        for i, a in enumerate(common):
            for b in common[i + 1:]:
                if early[a] == early[b] or final[a] == final[b]:
                    continue
                total += 1
                if (early[a] > early[b]) == (final[a] > final[b]):
                    agree += 1
        return {"early_day": early_day, "pairs": total,
                "agreement": round(agree / total, 2) if total else None,
                "early": early, "final": final}

    def check_margins(self):
        """Histogram of (total - DN) over every skill check on the wire."""
        rolls = _dedupe_events(self.rows, "chk")
        margins, by_skill, per_bot = [], defaultdict(list), defaultdict(list)
        for _ts, d in rolls:
            if "tot" not in d or "dn" not in d:
                continue
            m = d["tot"] - d["dn"]
            margins.append(m)
            sk = d.get("sk")
            if sk is not None:
                by_skill[sk].append(m)
            per_bot[d.get("pid")].append(m)
        if not margins:
            return {"rolls": 0}
        hist = Counter(max(-6, min(6, m)) for m in margins)
        near = sum(1 for m in margins if -1 <= m <= 1)
        return {"rolls": len(margins),
                "won": sum(1 for m in margins if m >= 0),
                "win_rate": round(sum(1 for m in margins if m >= 0) / len(margins), 3),
                "mean_margin": round(statistics.mean(margins), 2),
                "near_miss_rate": round(near / len(margins), 3),
                "histogram": {k: hist[k] for k in sorted(hist)},
                "by_skill": {SK_NAME[s]: len(v) for s, v in sorted(by_skill.items())
                             if 0 <= s < len(SK_NAME)},
                "per_bot": {p: len(v) for p, v in sorted(per_bot.items())
                            if p is not None}}

    def action_mix(self):
        """What the bots actually sent, by arch."""
        mix = defaultdict(Counter)
        for r in self.rows:
            if r["ch"] != "tx":
                continue
            d, t = r["d"], r["d"].get("t")
            if t == "act":
                a = d.get("a", -1)
                name = ACT_NAME[a] if 0 <= a < len(ACT_NAME) else f"act{a}"
            elif t == "m":
                name = "MOVE"
            elif t == "pick":
                continue
            else:
                name = t.upper()
            mix[r["arch"]][name] += 1
        overall = Counter()
        for c in mix.values():
            overall.update(c)
        return {"overall": dict(overall.most_common()),
                "by_bot": {a: dict(c.most_common()) for a, c in sorted(mix.items())}}

    def resource_slack(self):
        """How tight supplies ever got -- if never tight, nothing was at stake."""
        out = {}
        for arch, ds in self.trajectories().items():
            if not ds:
                continue
            water = [d["inv"][WATER] for d in ds if "inv" in d]
            food = [d["inv"][FOOD] for d in ds if "inv" in d]
            carried = [sum(d["inv"]) for d in ds if "inv" in d]
            if not water:
                continue
            out[arch] = {
                "min_water": min(water), "min_food": min(food),
                "dry_samples": round(sum(1 for w in water if w == 0) / len(water), 3),
                "starving_samples": round(sum(1 for f in food if f == 0) / len(food), 3),
                "max_carried": max(carried),
                "min_ll": min(d.get("ll", 9) for d in ds),
            }
        return out

    def crises(self, threshold=1):
        """A crisis starts when a staple hits `threshold` and ends if it
        recovers above it.  Recovery rate is the interesting number."""
        out = {}
        for arch, ds in self.trajectories().items():
            started = recovered = 0
            in_crisis = False
            for d in ds:
                if "inv" not in d:
                    continue
                low = d["inv"][WATER] <= threshold or d["inv"][FOOD] <= threshold
                if low and not in_crisis:
                    in_crisis = True
                    started += 1
                elif not low and in_crisis:
                    in_crisis = False
                    recovered += 1
            out[arch] = {"crises": started, "recovered": recovered,
                         "recovery_rate": round(recovered / started, 2) if started else None,
                         "ended_in_crisis": in_crisis}
        return out

    def poi_reach(self):
        """POIs opened/banked/aborted.  POIs are consumed permanently, so
        every one opened is one no other survivor can ever have."""
        opened = _dedupe_events(self.rows, "enc_start")
        banked = _dedupe_events(self.rows, "enc_bank")
        ended = _dedupe_events(self.rows, "enc_end")
        by_pid = Counter(d.get("pid") for _t, d in opened)
        content = {a: b.get("content") for a, b in self.bots.items() if b.get("content")}
        return {"opened": len(opened), "banked": len(banked), "ended": len(ended),
                "opened_by_bot": dict(sorted(by_pid.items(),
                                             key=lambda kv: (kv[0] is None, kv[0]))),
                "content_scores": content}

    def deaths(self):
        out = {}
        for _ts, d in _dedupe_events(self.rows, "downed"):
            out.setdefault(d.get("pid"), 0)
            out[d["pid"]] += 1
        days = {}
        for arch, ds in self.trajectories().items():
            dead = [d for d in ds if d.get("ll") == 0]
            days[arch] = dead[0].get("day") if dead else None
        return {"downed_events": out, "first_death_day": days}

    def tension(self, minutes_per_day=EST_MINUTES_PER_DAY, hours=TARGET_HOURS):
        """Measure against the stated design target: a good tension arc across
        two hours of play, with a character coming close to dying at most four
        times in that window.

        Three separate questions, because they fail independently:

        **Rate** -- how often does a survivor brush with death?  Counted per
        game-day and projected onto a session, since a run is not two hours
        long.  Deaths and near-deaths are counted apart on purpose: "came
        close to dying" means it survived, and a death is a failure of the
        target, not an instance of it.

        **Dwell** -- where does LL actually sit?  Time at full health is time
        with no tension at all; time in the wounded middle band is where the
        interesting play is.  A bimodal distribution with a hole in the middle
        means characters are either fine or already doomed.

        **Arc** -- are the brushes spread evenly, or do they build?  A flat
        distribution is not an arc, however many brushes it contains.
        """
        traj = self.trajectories()
        days = [d.get("day", 0) for ds in traj.values() for d in ds if d.get("day")]
        maxday = max(days) if days else 0
        bots = len(traj) or 1

        deaths = defaultdict(list)
        for ts, d in _dedupe_events(self.rows, "downed"):
            deaths[d.get("pid")].append(ts)
        n_deaths = sum(len(v) for v in deaths.values())

        near = 0
        dwell = Counter()
        for ds in traj.values():
            prev = 9
            for d in ds:
                ll = d.get("ll")
                if ll is None:
                    continue
                dwell[ll] += 1
                # An entry into the danger band that is not itself a death.
                if ll <= NEAR_DEATH_LL and prev > NEAR_DEATH_LL and ll > 0:
                    near += 1
                prev = ll
        samples = sum(dwell.values()) or 1

        per_day_death = n_deaths / bots / maxday if maxday else 0.0
        per_day_near = near / bots / maxday if maxday else 0.0
        session_days = hours * 60.0 / minutes_per_day
        proj_death = per_day_death * session_days
        proj_near = per_day_near * session_days

        # Arc: deaths by decile of the run.
        stamps = sorted({(r["ts"], r["d"]["day"]) for r in self.rows
                         if r["ch"] == "rx_s" and r["d"].get("day")})

        def day_at(ts):
            best = 0
            for t, dd in stamps:
                if t > ts:
                    break
                best = dd
            return best

        arc = Counter()
        for pid, times in deaths.items():
            for ts in times:
                if maxday:
                    arc[min(9, int(day_at(ts) / maxday * 10))] += 1
        early = sum(arc[i] for i in range(5))
        late = sum(arc[i] for i in range(5, 10))

        return {
            "days": maxday, "bots": bots,
            "deaths": n_deaths, "near_deaths": near,
            "per_bot_per_day": {"deaths": round(per_day_death, 4),
                                "near": round(per_day_near, 4)},
            "session": {"hours": hours, "minutes_per_day": minutes_per_day,
                        "game_days": round(session_days),
                        "projected_deaths": round(proj_death, 1),
                        "projected_near": round(proj_near, 1),
                        "projected_brushes": round(proj_death + proj_near, 1),
                        "target_brushes": TARGET_BRUSHES,
                        "verdict": ("over" if proj_death + proj_near > TARGET_BRUSHES
                                    else "within")},
            "dwell_pct": {ll: round(100.0 * dwell.get(ll, 0) / samples, 1)
                          for ll in range(8)},
            "danger_pct": round(100.0 * sum(dwell.get(i, 0) for i in range(3)) / samples, 1),
            "full_health_pct": round(100.0 * sum(dwell.get(i, 0) for i in (6, 7)) / samples, 1),
            "arc_deciles": [arc[i] for i in range(10)],
            "arc_early_vs_late": [early, late],
            # Deaths should be the rare tail of near-misses, not half of them.
            "death_to_near_ratio": round(n_deaths / near, 2) if near else None,
        }

    def connections(self):
        """Connection lifecycle per bot.

        Tracked explicitly because "connected" and "seated" are two different
        states here and both fail quietly.  A refused pick leaves a client in
        the lobby still receiving every broadcast, and the board can drop a
        player slot while the socket stays open -- observed on a realtime run
        where two bots streamed state for minutes with frozen scores.
        `seated_s` is the only number that reflects time actually spent
        playing.
        """
        ev = Counter()
        for r in self.rows:
            if r["ch"] in ("conn", "disconn", "seat_lost", "respawn",
                           "pick_timeout", "full", "no_slot", "conn_err",
                           "slot_busy", "claim", "joined", "downed"):
                ev[r["ch"]] += 1
        per_bot = {a: b.get("connection") for a, b in self.bots.items()
                   if b.get("connection")}
        gaps = defaultdict(list)
        for r in self.rows:
            if r["ch"] == "disconn":
                gaps[r["arch"]].append(r["d"])
        elapsed = self.summary.get("elapsed_s") or 0
        for a, rep in per_bot.items():
            if elapsed:
                rep["seated_pct"] = round(100.0 * rep.get("seated_s", 0) / elapsed, 1)
            eps = gaps.get(a, [])
            if eps:
                rep["mean_episode_s"] = round(
                    statistics.mean(e.get("open_s", 0) for e in eps), 1)
                rep["shortest_episode_s"] = min(e.get("open_s", 0) for e in eps)
        return {"events": dict(ev), "per_bot": per_bot,
                "run_elapsed_s": elapsed}

    def board(self):
        tel = [r["d"] for r in self.rows
               if r["ch"] == "telemetry" and "err" not in r["d"]]
        if not tel:
            return {}
        ticks = [t["maxTickMs"] for t in tel if t.get("maxTickMs") is not None]
        heaps = [t["minHeap"] for t in tel if t.get("minHeap") is not None]
        return {"samples": len(tel),
                "maxTickMs": max(ticks) if ticks else None,
                "minHeap": min(heaps) if heaps else None,
                "broadcastPartial": max((t.get("broadcastPartial") or 0) for t in tel),
                "broadcastSkips": max((t.get("broadcastSkips") or 0) for t in tel),
                "days": max((t.get("day") or 0) for t in tel),
                "poll_failures": sum(1 for r in self.rows
                                     if r["ch"] == "telemetry" and "err" in r["d"])}

    def all(self):
        return {"file": self.path.name, "meta": self.meta,
                "reason": self.summary.get("reason"),
                "elapsed_s": self.summary.get("elapsed_s"),
                "day": self.summary.get("day"),
                "score_spread": self.score_spread(), "snowball": self.snowball(),
                "check_margins": self.check_margins(), "action_mix": self.action_mix(),
                "resource_slack": self.resource_slack(), "crises": self.crises(),
                "poi_reach": self.poi_reach(), "deaths": self.deaths(),
                "tension": self.tension(),
                "connections": self.connections(), "board": self.board()}

    # --- rendering ------------------------------------------------------
    def render(self):
        m = self.all()
        L = [f"=== {m['file']}",
             f"    mode={m['meta'].get('mode')} target={m['meta'].get('target')} "
             f"seed={m['meta'].get('seed')} end={m['reason']} "
             f"after {m['elapsed_s']}s, day {m['day']}"]

        ss = m["score_spread"]
        L.append("\n-- standings")
        for a, sc in sorted(ss["scores"].items(), key=lambda kv: -kv[1]):
            b = self.bots[a]
            died = m["deaths"]["first_death_day"].get(a)
            L.append(f"   {self.label(a):<32} {sc:>6}  steps={b['steps']:<5} "
                     f"pts/step={b.get('pts_per_step')}"
                     + (f"  DIED day {died}" if died else ""))
        if "spread" in ss:
            L.append(f"   spread={ss['spread']} stdev={ss['stdev']} "
                     f"max/min={ss['ratio']}x")

        sn = m["snowball"]
        if sn.get("agreement") is not None:
            L.append(f"\n-- snowball: day-{sn['early_day']} order matches the finish "
                     f"in {sn['agreement']:.0%} of pairs ({sn['pairs']} pairs)")
            L.append("   1.0 = decided early and nothing changed; ~0.5 = still open")

        cm = m["check_margins"]
        L.append(f"\n-- skill checks: {cm.get('rolls', 0)} rolls")
        if cm.get("rolls"):
            L.append(f"   win rate {cm['win_rate']:.0%}  mean margin {cm['mean_margin']:+.2f}  "
                     f"near-misses (|margin|<=1) {cm['near_miss_rate']:.0%}")
            width = max(cm["histogram"].values())
            for k in sorted(cm["histogram"]):
                n = cm["histogram"][k]
                bar = "#" * max(1, round(28 * n / width))
                tag = "<=-6" if k == -6 else (">=+6" if k == 6 else f"{k:+d}")
                L.append(f"   {tag:>4} {bar} {n}")
            L.append(f"   by skill: {cm['by_skill']}")

        am = m["action_mix"]
        total = sum(am["overall"].values()) or 1
        L.append("\n-- action mix (what was actually sent)")
        for name, n in am["overall"].items():
            L.append(f"   {name:<12} {n:>6}  {n / total:.0%}")

        L.append("\n-- supplies and crises")
        for a, s in sorted(m["resource_slack"].items()):
            c = m["crises"].get(a, {})
            rr = c.get("recovery_rate")
            L.append(f"   {self.label(a):<32} min water={s['min_water']} "
                     f"food={s['min_food']} ll={s['min_ll']}  "
                     f"dry {s['dry_samples']:.0%} of the time  "
                     f"crises={c.get('crises', 0)} recovered="
                     f"{'n/a' if rr is None else f'{rr:.0%}'}")

        pr = m["poi_reach"]
        L.append(f"\n-- POIs: {pr['opened']} opened, {pr['banked']} banked, "
                 f"{pr['ended']} ended")
        if pr["opened_by_bot"]:
            L.append(f"   by bot: {pr['opened_by_bot']}")
        for a, c in sorted(pr["content_scores"].items()):
            L.append(f"   {self.label(a):<32} nodes={c['nodes_seen']} "
                     f"rolls={c['rolls_won']}/{c['rolls']} recipes={c['recipes']} "
                     f"aborted={c['encounters_aborted']}")

        tn = m["tension"]
        se = tn["session"]
        L.append(f"\n-- tension vs target ({se['hours']}h session "
                 f"~= {se['game_days']} game-days at {se['minutes_per_day']}min/day)")
        L.append(f"   per bot per game-day: {tn['per_bot_per_day']['deaths']} deaths, "
                 f"{tn['per_bot_per_day']['near']} near-deaths")
        L.append(f"   projected per session: {se['projected_deaths']} deaths + "
                 f"{se['projected_near']} near = {se['projected_brushes']} brushes "
                 f"(target <= {se['target_brushes']})  ** {se['verdict'].upper()} **")
        L.append(f"   deaths:near-misses = 1:{round(1 / tn['death_to_near_ratio'], 1)}"
                 if tn["death_to_near_ratio"] else "   deaths:near-misses = n/a")
        L.append("   time spent at each LL (tension lives in the middle):")
        for ll in range(8):
            pct = tn["dwell_pct"].get(ll, 0.0)
            L.append(f"     LL {ll} {'#' * round(pct / 2):<26} {pct:>5.1f}%")
        L.append(f"   danger (LL<=2) {tn['danger_pct']}%   "
                 f"full health (LL>=6) {tn['full_health_pct']}%")
        a = tn["arc_deciles"]
        L.append(f"   arc, deaths by decile: {a}")
        L.append(f"   first half {tn['arc_early_vs_late'][0]} vs "
                 f"second half {tn['arc_early_vs_late'][1]} "
                 f"(a rising arc wants the second half higher)")

        cn = m["connections"]
        L.append("\n-- connections (seated != connected; both fail quietly)")
        L.append(f"   events: {cn['events']}")
        for a, rep in sorted(cn["per_bot"].items()):
            L.append(f"   {self.label(a):<32} seated {rep.get('seated_s', 0)}s "
                     f"({rep.get('seated_pct', '?')}% of run)  "
                     f"connects={rep['connects']} drops={rep['disconnects']} "
                     f"seatsLost={rep['seats_lost']} respawns={rep['respawns']} "
                     f"pickFail={rep['pick_failures']} full={rep['refused_full']}")
            if rep.get("close_reasons"):
                L.append(f"       closed by: {rep['close_reasons']}"
                         + (f"  mean episode {rep['mean_episode_s']}s"
                            if rep.get("mean_episode_s") else ""))

        bd = m["board"]
        if bd:
            L.append(f"\n-- board: worst maxTickMs={bd['maxTickMs']} "
                     f"minHeap={bd['minHeap']} bcastPartial={bd['broadcastPartial']} "
                     f"bcastSkips={bd['broadcastSkips']} pollFail={bd['poll_failures']}")
        return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*", help="run JSONL files (default: newest)")
    ap.add_argument("--json", help="also write the full metrics as JSON here")
    a = ap.parse_args(argv)

    paths = [Path(p) for p in a.runs]
    if not paths:
        found = sorted(RUNS_DIR.glob("run-*.jsonl"))
        if not found:
            raise SystemExit(f"no runs in {RUNS_DIR}")
        paths = [found[-1]]

    blobs = []
    for p in paths:
        rep = RunReport(p)
        print(rep.render())
        print()
        blobs.append(rep.all())
    if a.json:
        Path(a.json).write_text(json.dumps(blobs, indent=2, default=str),
                                encoding="utf-8")
        print(f"-> {a.json}")


if __name__ == "__main__":
    main()
