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

from causes import CAUSE_ORDER, DamageLedger
import findings as findings_mod
from config import ACT_NAME, ARCHETYPE_NAME, RES_NAME, SK_NAME, WEATHER_NAME
from navigate import hex_distance
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
# Causes that tick on a real-time clock rather than once per game-day, so a
# collapsed bot day must not be used to scale them -- see death_causes().
PER_MINUTE_CAUSES = {"fire", "lightning", "flood", "creeping doom",
                     "chem storm", "strangle fog"}


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

    def tunnels(self):
        """Who used the bunker network, and what it bought them.

        Read off the wire rather than the policies, so a surface bot that
        fell down a hatch shows up here too -- terrain 12/13 costs 1 MP,
        which makes a hatch the cheapest hex on the surface map and an
        accidental descent a genuinely common event.

        The headline is `mp_per_hex`: MP spent underground per surface hex
        actually crossed, against a surface average MC of ~1.6.
        docs/tunnel-system-spec.md predicts 44-67% savings from the
        generator's own corridor paths; this is the same number measured
        through one ring of vision and no map of the pairings.
        """
        ins = _dedupe_events(self.rows, "tun_in")
        outs = _dedupe_events(self.rows, "tun_out")
        steps_below = Counter()
        for r in self.rows:
            if r["ch"] != "rx":
                continue
            d = r["d"]
            if d.get("t") == "ev" and d.get("k") == "mv" and d.get("dp"):
                steps_below[d.get("pid")] += 1
        # Pair each descent with the next ascent by the same survivor: the
        # surface gap between the two hatches is what the trip bought.
        pending, trips = {}, []
        for ts, d in sorted(ins + outs, key=lambda x: x[0]):
            pid = d.get("pid")
            if d.get("k") == "tun_in":
                pending[pid] = (ts, d.get("q"), d.get("r"))
            elif pid in pending:
                t0, q0, r0 = pending.pop(pid)
                if q0 is not None and d.get("q") is not None:
                    trips.append({"pid": pid, "s": round(ts - t0, 1),
                                  "gap": hex_distance(q0, r0, d["q"], d["r"])})
        gaps = [t["gap"] for t in trips]
        hexes = sum(gaps)
        # Per-bot tunnel stats ride the policy's content block; only the
        # tunnel policies carry them.
        content = {a: b["content"] for a, b in self.bots.items()
                   if b.get("content") and "descents" in b["content"]}
        below = sum(steps_below.values())
        return {"descents": len(ins), "ascents": len(outs),
                "steps_below": dict(steps_below), "total_steps_below": below,
                "trips": len(trips), "surface_hexes_crossed": hexes,
                "longest_crossing": max(gaps, default=0),
                "mean_crossing": round(hexes / len(gaps), 1) if gaps else None,
                "mp_per_hex": round(below * 2 / hexes, 2) if hexes else None,
                "by_bot": content,
                "users": sorted({d.get("pid") for _t, d in ins})}

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

    def outsiders(self):
        """Players in the run who were not bots from this arena.

        A human opening the browser mid-run is not a neutral observer: they
        take one of the six slots, and because tickGame() only ends a day
        early when *every* connected player is resting, an awake human holds
        every day open for the full DAY_TICKS (5 real minutes) instead of the
        ~3-5s an all-bot fleet collapses to.  That changes game-days per real
        minute by an order of magnitude, which is the denominator under every
        per-game-day rate in this report -- so a contaminated run cannot be
        pooled with clean ones.

        Detected by slot: the arena claims 0..bots-1 via preferred_arch, so a
        join on any higher slot is somebody else.
        """
        n_bots = self.meta.get("bots", 0)
        seen, joins = set(), Counter()
        for r in self.rows:
            if r["ch"] != "rx":
                continue
            d = r["d"]
            if d.get("t") != "ev" or d.get("k") not in ("join", "left"):
                continue
            pid = d.get("pid")
            if pid is None or pid < n_bots:
                continue
            key = (round(r["ts"], 1), d["k"], pid)
            if key in seen:
                continue
            seen.add(key)
            joins[(d["k"], pid)] += 1
        pids = sorted({pid for _k, pid in joins})
        return {"pids": pids, "events": {f"{k}:{p}": n for (k, p), n in joins.items()},
                "clean": not pids}

    def death_causes(self, minutes_per_day=EST_MINUTES_PER_DAY, hours=TARGET_HOURS):
        """What actually kills survivors, and how often each cause would land
        in a 2-hour session.

        Attribution itself lives in causes.py.  What this adds is the
        projection, and it has to be done two different ways because the
        causes run on two different clocks:

        * The supply grind (thirst, hunger, exposure) and the dusk radiation
          check fire **once per game-day**.  A bot fleet collapses days to a
          few seconds, so these are measured per game-day and scaled by how
          many game-days a 2-hour session contains.
        * Fire, lightning, the Creeping Doom and the weather hazards run on
          **real-time clocks** (WORLD_TICK_INTERVAL, and weatherNextGapMs
          pins weather changes to a ~1.1-1.9 real-minute floor regardless of
          how fast game-days fly).  Scaling those per game-day would inflate
          them by the same factor the day collapsed by, so they are measured
          per real minute and scaled to 120.

        This is the one place the harness cannot paper over the collapsed
        day: with days at ~3s the fleet lives ~85 game-days inside 7 real
        minutes, which is ~25x the supply pressure per unit of hazard
        exposure a human would meet.  Both projections are reported so the
        distortion is visible rather than averaged away.
        """
        led = DamageLedger(self.rows)
        deaths = led.deaths()
        nears = led.near_death_causes(danger_ll=NEAR_DEATH_LL)

        traj = self.trajectories()
        days = [d.get("day", 0) for ds in traj.values() for d in ds if d.get("day")]
        maxday = max(days) if days else 0
        bots = len(traj) or 1
        # The real-time denominator only exists in the closing summary, and a
        # run killed mid-flight has no summary -- fall back to the last
        # recorded timestamp so a crashed run still contributes its hazard
        # exposure instead of dividing by zero.
        elapsed_s = self.summary.get("elapsed_s")
        if not elapsed_s:
            elapsed_s = max((r["ts"] for r in self.rows), default=0.0)
        elapsed_min = elapsed_s / 60.0

        session_days = hours * 60.0 / minutes_per_day
        session_min = hours * 60.0

        by_cause = Counter(d["cause"] for d in deaths)
        near_by_cause = Counter(d["cause"] for d in nears)

        proj = {}
        for cause, n in by_cause.items():
            if cause in PER_MINUTE_CAUSES:
                rate = n / bots / elapsed_min if elapsed_min else 0.0
                proj[cause] = {"clock": "real", "per_bot_per_min": round(rate, 4),
                               "projected": round(rate * session_min, 2)}
            else:
                rate = n / bots / maxday if maxday else 0.0
                proj[cause] = {"clock": "game-day", "per_bot_per_day": round(rate, 4),
                               "projected": round(rate * session_days, 2)}

        return {
            "deaths": len(deaths), "near_deaths": len(nears),
            "days": maxday, "bots": bots, "elapsed_min": round(elapsed_min, 1),
            "attributed_pct": round(100.0 * (1 - by_cause.get("unattributed", 0)
                                             / max(1, len(deaths))), 1),
            "by_cause": {c: by_cause[c] for c in CAUSE_ORDER if by_cause[c]},
            "near_by_cause": {c: near_by_cause[c] for c in CAUSE_ORDER
                              if near_by_cause[c]},
            "share": {c: round(by_cause[c] / len(deaths), 3)
                      for c in CAUSE_ORDER if by_cause[c]} if deaths else {},
            "projected": proj,
            "projected_deaths": round(sum(p["projected"] for p in proj.values()), 2),
            # The grind: every LL point lost in the run, by cause.  A cause can
            # dominate this without ever landing a killing blow.
            "ll_lost": {c: round(v, 1) for c, v in
                        sorted(led.loss_ledger().items(), key=lambda kv: -kv[1])},
            # Whether the dangerous weather ever turned up at all.  Without
            # this a zero for chem or fog is unreadable: it could mean the
            # hazard is harmless or that the run never saw one.
            "weather_pct": self.weather_exposure(),
        }

    def weather_exposure(self):
        """Share of sampled time in each weather phase.

        Weather changes are floored at ~1.1-1.9 real minutes by
        weatherNextGapMs, so a short run sees only a handful of phases and
        can easily miss chem and fog entirely.
        """
        seen = Counter()
        for r in self.rows:
            if r["ch"] == "rx_s" and r["d"].get("wp") is not None:
                seen[r["d"]["wp"]] += 1
        tot = sum(seen.values()) or 1
        return {WEATHER_NAME[p] if p < len(WEATHER_NAME) else str(p):
                round(100.0 * n / tot, 1)
                for p, n in sorted(seen.items())}

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
                "tunnels": self.tunnels(),
                "tension": self.tension(), "death_causes": self.death_causes(),
                "outsiders": self.outsiders(),
                "connections": self.connections(), "board": self.board()}

    # --- rendering ------------------------------------------------------
    def render(self):
        m = self.all()
        out = m["outsiders"]
        warn = ([f"    ** NOT A CLEAN RUN: slot(s) {out['pids']} were not this "
                 f"arena's bots — {out['events']}",
                 "       an awake non-bot holds every day open for the full "
                 "5 minutes (tickGame's early-dawn needs ALL players resting),",
                 "       so per-game-day rates from this run are not comparable "
                 "with all-bot runs."]
                if not out["clean"] else [])
        L = [f"=== {m['file']}", *warn,
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

        tu = m["tunnels"]
        if tu["descents"] or tu["ascents"]:
            L.append(f"\n-- bunker tunnels: {tu['descents']} descents, "
                     f"{tu['ascents']} ascents by slot(s) {tu['users']}")
            L.append(f"   {tu['total_steps_below']} steps underground, "
                     f"{tu['trips']} completed crossings covering "
                     f"{tu['surface_hexes_crossed']} surface hexes "
                     f"(longest {tu['longest_crossing']})")
            if tu["mp_per_hex"] is not None:
                verdict = ("cheaper than walking" if tu["mp_per_hex"] < 1.6
                           else "NOT worth it")
                L.append(f"   {tu['mp_per_hex']} MP per surface hex crossed "
                         f"vs ~1.6 over ground -- {verdict}")
                L.append("   (spec predicts 44-67% savings with a known "
                         "network; this is what one ring of vision achieves)")
            for a, c in sorted(tu["by_bot"].items()):
                L.append(f"   {self.label(a):<32} dives={c['descents']} "
                         f"below/above={c['tunnel_steps']}/{c['surface_steps']} "
                         f"shafts={c['shafts_used']} "
                         f"dawnsBelow={c['dawns_below']} "
                         f"slept={c.get('rests_below', 0)} "
                         f"stranded={c.get('stranded_dawns', 0)}")

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

        dc = m["death_causes"]
        L.append(f"\n-- what killed them ({dc['deaths']} deaths, "
                 f"{dc['attributed_pct']}% attributed)")
        for c, n in dc["by_cause"].items():
            share = dc["share"].get(c, 0)
            p = dc["projected"].get(c, {})
            L.append(f"   {c:<17} {n:>4}  {share:>5.0%}  "
                     f"{'#' * round(share * 26):<26} "
                     f"-> {p.get('projected', 0):.2f}/session ({p.get('clock')})")
        if dc["near_by_cause"]:
            L.append(f"   near-misses by cause: {dc['near_by_cause']}")
        if dc["ll_lost"]:
            tot = sum(dc["ll_lost"].values()) or 1
            L.append("   LL lost overall (the grind, not the killing blow): "
                     + ", ".join(f"{c} {v / tot:.0%}"
                                 for c, v in dc["ll_lost"].items()))
        L.append("   weather seen (a hazard absent all run cannot score): "
                 + ", ".join(f"{k} {v}%" for k, v in dc["weather_pct"].items()))

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


def _wilson(k, n, z=1.96):
    """95% CI for a proportion.  Wilson rather than normal-approximation
    because several causes land at 0 or near 1 out of a few hundred deaths,
    where the normal interval runs off the end of [0, 1]."""
    if n == 0:
        return (0.0, 0.0)
    p = k / n
    d = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / d
    half = z * ((p * (1 - p) / n + z * z / (4 * n * n)) ** 0.5) / d
    return (max(0.0, centre - half), min(1.0, centre + half))


def aggregate(reports, hours=TARGET_HOURS, minutes_per_day=EST_MINUTES_PER_DAY):
    """Pool several runs into one answer.

    A single run is one map roll and one set of spawns, and the findings so
    far say the map decides a lot (snowball agreement ~80% by day 5).  So
    every headline number here is pooled across runs, and the per-run spread
    is reported next to it -- a cause share that swings wildly run to run is
    a property of the map, not of the game.
    """
    blobs = [r.all() for r in reports]
    dcs = [b["death_causes"] for b in blobs]
    tns = [b["tension"] for b in blobs]

    total_deaths = sum(d["deaths"] for d in dcs)
    total_near = sum(d["near_deaths"] for d in dcs)
    bot_days = sum(d["bots"] * d["days"] for d in dcs)
    bot_min = sum(d["bots"] * d["elapsed_min"] for d in dcs)
    session_days = hours * 60.0 / minutes_per_day
    session_min = hours * 60.0

    pooled = Counter()
    pooled_near = Counter()
    for d in dcs:
        pooled.update(d["by_cause"])
        pooled_near.update(d["near_by_cause"])

    causes = {}
    for c in CAUSE_ORDER:
        n = pooled.get(c, 0)
        if not n:
            continue
        lo, hi = _wilson(n, total_deaths)
        per_run = [d["by_cause"].get(c, 0) / max(1, d["deaths"]) for d in dcs]
        if c in PER_MINUTE_CAUSES:
            rate = n / bot_min if bot_min else 0.0
            proj, clock = rate * session_min, "real-min"
        else:
            rate = n / bot_days if bot_days else 0.0
            proj, clock = rate * session_days, "game-day"
        causes[c] = {
            "deaths": n, "share": round(n / total_deaths, 4) if total_deaths else 0,
            "ci95": (round(lo, 4), round(hi, 4)),
            "clock": clock, "rate": round(rate, 5),
            "projected_per_session": round(proj, 2),
            "per_run_share_range": (round(min(per_run), 3), round(max(per_run), 3)),
        }

    # Deaths and near-deaths are projected apart on purpose -- see tension().
    # A death is a failure of the target; a near-death is an instance of it.
    death_rate = total_deaths / bot_days if bot_days else 0.0
    near_rate = total_near / bot_days if bot_days else 0.0
    proj_d = death_rate * session_days
    proj_n = near_rate * session_days
    per_run_brushes = [t["session"]["projected_brushes"] for t in tns]

    ll = Counter()
    for b in blobs:
        ll.update(b["death_causes"]["ll_lost"])
    ll_tot = sum(ll.values()) or 1

    dwell = Counter()
    for t in tns:
        for k, v in t["dwell_pct"].items():
            dwell[int(k)] += v / len(tns)

    weather = Counter()
    for d in dcs:
        for k, v in d["weather_pct"].items():
            weather[k] += v / len(dcs)

    return {
        "runs": len(blobs), "files": [b["file"] for b in blobs],
        "total_deaths": total_deaths, "total_near_deaths": total_near,
        "bot_game_days": bot_days, "bot_real_minutes": round(bot_min, 1),
        "attributed_pct": round(100.0 * (1 - pooled.get("unattributed", 0)
                                         / max(1, total_deaths)), 1),
        "causes": causes,
        "near_by_cause": {c: pooled_near[c] for c in CAUSE_ORDER if pooled_near[c]},
        "ll_lost_share": {c: round(v / ll_tot, 3)
                          for c, v in sorted(ll.items(), key=lambda kv: -kv[1])},
        # Share alone hides the thing that matters when comparing arms: two
        # arms can lose LL in exactly the same proportions while one loses
        # far more of it.  Rate is the comparable number.
        "ll_lost_per_bot_day": {c: round(v / bot_days, 4) for c, v in
                                sorted(ll.items(), key=lambda kv: -kv[1])} if bot_days else {},
        "ll_lost_total_per_bot_day": round(ll_tot / bot_days, 4) if bot_days else 0,
        "session": {
            "hours": hours, "minutes_per_day": minutes_per_day,
            "game_days": round(session_days),
            "projected_deaths": round(proj_d, 2),
            "projected_near": round(proj_n, 2),
            "projected_brushes": round(proj_d + proj_n, 2),
            "target_brushes": TARGET_BRUSHES,
            "verdict": "over" if proj_d + proj_n > TARGET_BRUSHES else "within",
            "per_run_brushes": per_run_brushes,
            "per_run_range": (min(per_run_brushes), max(per_run_brushes))
            if per_run_brushes else None,
        },
        "dwell_pct": {k: round(v, 1) for k, v in sorted(dwell.items())},
        "weather_pct": {k: round(v, 1) for k, v in
                        sorted(weather.items(), key=lambda kv: -kv[1])},
    }


def render_aggregate(agg):
    L = [f"=== POOLED ACROSS {agg['runs']} RUNS",
         f"    {agg['total_deaths']} deaths, {agg['total_near_deaths']} near-deaths "
         f"over {agg['bot_game_days']} bot-game-days "
         f"({agg['bot_real_minutes']} bot-minutes of real time)",
         f"    {agg['attributed_pct']}% of deaths attributed to a cause"]

    se = agg["session"]
    L.append(f"\n-- tension vs target ({se['hours']}h ~= {se['game_days']} game-days "
             f"at {se['minutes_per_day']}min/day)")
    L.append(f"   projected per session: {se['projected_deaths']} deaths + "
             f"{se['projected_near']} near = {se['projected_brushes']} brushes "
             f"(target <= {se['target_brushes']})  ** {se['verdict'].upper()} **")
    L.append(f"   per-run spread: {se['per_run_brushes']}")

    L.append(f"\n-- likelihood of death by cause (n={agg['total_deaths']}, "
             f"95% CI)")
    L.append(f"   {'cause':<17} {'n':>5} {'share':>7}  {'95% CI':<16} "
             f"{'per-2h':>7}  clock")
    for c, d in agg["causes"].items():
        lo, hi = d["ci95"]
        L.append(f"   {c:<17} {d['deaths']:>5} {d['share']:>6.1%}  "
                 f"[{lo:>5.1%},{hi:>6.1%}]  "
                 f"{d['projected_per_session']:>7.2f}  {d['clock']}")
        L.append(f"   {'':<17} {'':>5} run-to-run share "
                 f"{d['per_run_share_range'][0]:.0%}-{d['per_run_share_range'][1]:.0%}")

    if agg["near_by_cause"]:
        tot = sum(agg["near_by_cause"].values()) or 1
        L.append("\n-- what pushes them into the danger band (LL<=2, survived)")
        for c, n in agg["near_by_cause"].items():
            L.append(f"   {c:<17} {n:>5} {n / tot:>6.1%}")

    L.append("\n-- the grind: every LL point lost, by cause")
    for c, s in agg["ll_lost_share"].items():
        L.append(f"   {c:<17} {s:>6.1%} {'#' * round(s * 30)}")

    L.append("\n-- hazard exposure: share of time in each weather phase")
    L.append("   (a cause scoring 0 against a phase never seen is untested, "
             "not safe)")
    L.append("   " + ", ".join(f"{k} {v}%" for k, v in agg["weather_pct"].items()))

    L.append("\n-- where LL actually sits (mean of per-run dwell)")
    for ll in range(8):
        pct = agg["dwell_pct"].get(ll, 0.0)
        L.append(f"     LL {ll} {'#' * round(pct / 2):<26} {pct:>5.1f}%")
    return "\n".join(L)


def _rate_ratio(k1, t1, k2, t2, z=1.96):
    """95% CI for the ratio of two Poisson rates (arm2 / arm1).

    Deaths are counts over unequal exposure -- different numbers of runs, of
    bots and of game-days per arm -- so comparing the raw counts or even the
    per-day rates says nothing about whether a difference is real.  The
    standard large-sample interval on log(rate ratio) does, and it is the
    difference between "the shelter arm died less" and "the shelter arm died
    less than chance explains".  An interval that spans 1.0 means it does not.
    """
    if not (k1 and k2 and t1 and t2):
        return None
    import math
    rr = (k2 / t2) / (k1 / t1)
    se = math.sqrt(1.0 / k1 + 1.0 / k2)
    lo = rr * math.exp(-z * se)
    hi = rr * math.exp(z * se)
    return rr, lo, hi


def render_comparison(arms):
    """Two or more pooled arms side by side.

    Built for the shelter experiment, where the question is not "how many
    deaths" but "how many of them survive the survivor knowing about a verb
    it never used".  Anything that moves between arms is policy; anything
    that does not is balance.
    """
    names = list(arms)
    w = max(len(n) for n in names) + 2
    L = ["=== ARM COMPARISON", ""]
    L.append(f"   {'':<18}" + "".join(f"{n:>{w}}" for n in names))

    def row(label, fn):
        L.append(f"   {label:<18}" + "".join(f"{fn(arms[n]):>{w}}" for n in names))

    row("runs", lambda a: a["runs"])
    row("bot-game-days", lambda a: a["bot_game_days"])
    row("deaths", lambda a: a["total_deaths"])
    row("near-deaths", lambda a: a["total_near_deaths"])
    row("deaths/bot-day", lambda a: f"{a['total_deaths'] / max(1, a['bot_game_days']):.4f}")
    row("proj. brushes/2h", lambda a: f"{a['session']['projected_brushes']:.2f}")
    row("verdict", lambda a: a["session"]["verdict"])

    # Against the first arm, which is the baseline by convention.
    if len(names) > 1:
        base = arms[names[0]]
        L.append("")
        L.append(f"   death-rate ratio vs {names[0]} (95% CI; spans 1.0 = "
                 "not distinguishable)")
        for n in names[1:]:
            rr = _rate_ratio(base["total_deaths"], base["bot_game_days"],
                             arms[n]["total_deaths"], arms[n]["bot_game_days"])
            if rr is None:
                L.append(f"   {n:<18} n/a")
                continue
            r, lo, hi = rr
            verdict = "significant" if hi < 1.0 or lo > 1.0 else "inconclusive"
            L.append(f"   {n:<18} {r:.2f}x  [{lo:.2f}, {hi:.2f}]  {verdict}")

    every = [c for c in CAUSE_ORDER
             if any(c in arms[n]["causes"] for n in names)]
    if every:
        L.append("")
        L.append(f"   {'deaths by cause':<18}" + "".join(f"{n:>{w}}" for n in names))
        for c in every:
            L.append(f"   {c:<18}" + "".join(
                f"{arms[n]['causes'].get(c, {}).get('deaths', 0):>{w}}"
                for n in names))

    grind = [c for c in CAUSE_ORDER
             if any(c in arms[n]["ll_lost_share"] for n in names)]
    if grind:
        L.append("")
        L.append("   LL lost per bot-game-day (rate, not share -- two arms can")
        L.append("   bleed in identical proportions and very different amounts)")
        L.append(f"   {'':<18}" + "".join(f"{n:>{w}}" for n in names))
        for c in grind:
            L.append(f"   {c:<18}" + "".join(
                f"{arms[n]['ll_lost_per_bot_day'].get(c, 0):>{w}.3f}"
                for n in names))
        L.append(f"   {'TOTAL':<18}" + "".join(
            f"{arms[n]['ll_lost_total_per_bot_day']:>{w}.3f}" for n in names))
    return "\n".join(L)


def _load_arm(paths, allow_dirty=False):
    """Usable runs only.

    Two things disqualify a run from being pooled: nobody was ever seated
    (no trajectories, so every rate gets a zero numerator over a real
    denominator), and a non-bot joined partway through (which changes how
    many game-days fit in a real minute -- see RunReport.outsiders).
    """
    reports, skipped = [], []
    for p in paths:
        rep = RunReport(p)
        if not rep.bots:
            skipped.append((p, "no bot ever seated"))
            continue
        out = rep.outsiders()
        if not out["clean"] and not allow_dirty:
            skipped.append((p, f"non-bot in slot(s) {out['pids']}"))
            continue
        reports.append(rep)
    for p, why in skipped:
        print(f"   (skipping {Path(p).name}: {why})")
    # Pooled, not skipped: a run that tripped a critical finding may still be
    # informative, but its balance numbers stand on a board that misbehaved.
    suspect = [Path(r.path).name for r in reports
               if any(f.get("severity") == "critical"
                      for f in findings_mod.load([r.path]))]
    if suspect:
        print(f"   (warning: {len(suspect)} pooled run(s) had critical findings -- "
              f"run findings.py on them: {', '.join(suspect)})")
    return reports


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*", help="run JSONL files (default: newest)")
    ap.add_argument("--json", help="also write the full metrics as JSON here")
    ap.add_argument("--aggregate", action="store_true",
                    help="pool the given runs into one report (death causes with "
                         "confidence intervals) instead of printing each")
    ap.add_argument("--per-run", action="store_true",
                    help="with --aggregate, also print each run's own report")
    ap.add_argument("--allow-dirty", action="store_true", dest="allow_dirty",
                    help="pool runs even if a non-bot joined partway through "
                         "(their day length is not comparable — see outsiders())")
    ap.add_argument("--compare", action="append", metavar="LABEL=DIR_OR_GLOB",
                    help="pool each named arm and print them side by side, e.g. "
                         "--compare baseline=runs/armA --compare shelter=runs/armB "
                         "(repeatable)")
    ap.add_argument("--minutes-per-day", type=float, default=EST_MINUTES_PER_DAY,
                    dest="minutes_per_day",
                    help="how long a game-day takes a human; the session "
                         "projection is linear in this")
    a = ap.parse_args(argv)

    # --compare names its own runs, so resolve the default "newest run" only
    # for the modes that actually need it -- otherwise a runs/ directory
    # organised into per-arm subdirectories aborts here with "no runs".
    if not a.compare:
        paths = [Path(p) for p in a.runs]
        if not paths:
            found = sorted(RUNS_DIR.glob("run-*.jsonl"))
            if not found:
                raise SystemExit(f"no runs in {RUNS_DIR}")
            paths = [found[-1]]

    if a.compare:
        arms, blobs = {}, {}
        for spec in a.compare:
            if "=" not in spec:
                raise SystemExit(f"--compare wants LABEL=path, got {spec!r}")
            label, where = spec.split("=", 1)
            p = Path(where)
            found = sorted(p.glob("run-*.jsonl")) if p.is_dir() \
                else sorted(Path().glob(where))
            reps = _load_arm(found, allow_dirty=a.allow_dirty)
            if not reps:
                raise SystemExit(f"arm {label!r}: no usable runs in {where}")
            arms[label] = aggregate(reps, minutes_per_day=a.minutes_per_day)
            blobs[label] = arms[label]
            print(f"\n########## {label}  ({len(reps)} runs)")
            print(render_aggregate(arms[label]))
        print()
        print(render_comparison(arms))
        if a.json:
            Path(a.json).write_text(json.dumps(blobs, indent=2, default=str),
                                    encoding="utf-8")
            print(f"\n-> {a.json}")
        return

    if a.aggregate:
        reports = _load_arm(paths, allow_dirty=a.allow_dirty)
        if a.per_run:
            for rep in reports:
                print(rep.render())
                print()
        if not reports:
            raise SystemExit("no usable runs")
        agg = aggregate(reports, minutes_per_day=a.minutes_per_day)
        print(render_aggregate(agg))
        if a.json:
            Path(a.json).write_text(json.dumps(agg, indent=2, default=str),
                                    encoding="utf-8")
            print(f"\n-> {a.json}")
        return

    blobs = []
    for p in paths:
        rep = RunReport(p)
        print(rep.render())
        rows = findings_mod.load([p])
        if rows:
            print("-- findings (findings.py for the full repro)")
            findings_mod.report(rows)
        print()
        blobs.append(rep.all())
    if a.json:
        Path(a.json).write_text(json.dumps(blobs, indent=2, default=str),
                                encoding="utf-8")
        print(f"-> {a.json}")


if __name__ == "__main__":
    main()
