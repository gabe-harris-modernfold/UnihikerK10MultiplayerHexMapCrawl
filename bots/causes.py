"""Why survivors die -- cause attribution over a recorded run.

`metrics.py` counts deaths and near-deaths.  This answers the next question:
*what killed them*, so the 2-hour tension target can be tuned at the source
rather than by moving a global dial.

## How a cause is recovered

**Protocol 2+ firmware says so.**  `downed` (and the `left` that follows it)
carries `"cause"`, set at the site that took the last LL, in the same names
this module reports; chem storm and Strangle Fog losses arrive as `dmg`
events.  A death with a wire cause is taken at its word (`via == "wire"`).

Everything below is the fallback for recordings from older firmware, where
`EVT_DOWNED` carried a pid and nothing else.  Every LL loss in the firmware
does, however, leave a distinct signature, so each death is matched against
the damage record nearest to it:

| Cause | Firmware site | Wire signature |
|---|---|---|
| thirst / hunger | `dawnUpkeep` -> `applyWStep`/`applyFStep` | `dawn` with a newly-set bit in `wth`/`fth` |
| exposure | `dawnUpkeep` §7.3 | `dawn` with `expd == -1` |
| bad air | `dawnUpkeep`, rest at depth 1 | `dawn` with `air == -1` |
| radiation | `duskCheck` | `dusk` with `out == 0` and `lld < 0` |
| fire | `resolveFireDamage` | `fire_dmg`, `intensity` 2-3 |
| lightning | `maybeIgniteLightning` direct strike | `fire_dmg`, `intensity == 10` (sentinel) |
| creeping doom | `resolveDoomProximity` | `doom_act` with `llLost > 0` |
| encounter hazard | `handleMsg_enc_choice` fail branch | `enc_res` `out == 0`, `penLL < 0` |
| encounter cost | `handleMsg_enc_choice` `ch.costLL` | `enc_res` `out == 1` then a death (see below) |
| action | `handleMsg_act` | `act` with `lld < 0` |
| chem storm | `tickGame` chem hazard | `dmg` `cause == "chem storm"` (protocol 2+; nothing before) |
| strangle fog | `tickGame` fog hazard | `dmg` `cause == "strangle fog"` (protocol 2+; nothing before) |

On older firmware the last two emit no event at all -- the LL simply drops.  They are inferred:
a death with no damage record nearby, while the sampled weather phase was
chem or fog, is attributed to that weather; anything else left over is
reported honestly as `unattributed` rather than folded into a neighbour.

Two ordering details, both found by reading the firmware rather than guessing:

* **The killing `dawn` arrives *after* its own `downed`.**  `dawnUpkeep`
  enqueues `EVT_DOWNED` inside the LL loss loop and `EVT_DAWN` only at the end
  of the function, so the event that explains the death is ~15 ms later on the
  wire than the death itself.  The search window is therefore two-sided.
* **`enc_choice`'s `cost_ll` is invisible.**  A successful choice that spends
  the last LL emits `enc_res` with `out == 1` and no penalty field, so an
  `enc_res` success immediately before a death is read as an encounter cost.

`ev` is broadcast to every client (`downed` is the one unicast), so the same
event appears once per connected bot.  Everything here dedupes first.
"""
import json
from collections import Counter, defaultdict

# Ordered for reporting: the supply grind, then the world hazards, then the
# things the player chose, then what could not be pinned down.
CAUSE_ORDER = [
    "thirst", "hunger", "exposure", "bad air", "radiation",
    "fire", "lightning", "flood", "creeping doom",
    "encounter hazard", "encounter cost", "action",
    "chem storm", "strangle fog", "unattributed",
]

# A damage record can precede its death (hazards enqueue damage then DOWNED)
# or follow it (dawn enqueues DOWNED then the summary event).
WINDOW_BACK = 2.5
WINDOW_FWD = 1.5

# Weather phases that take LL with no event of their own (config.py mirrors
# the firmware's enum).
SILENT_WEATHER = {3: "chem storm", 4: "strangle fog"}


def _dedupe(rows, kinds=None):
    """One copy of each broadcast event, in time order.

    Protocol 2+ stamps every queued event with its sequence number "sq",
    which is exact: one game event, one sq, however many sockets saw it.  (One
    event can produce two messages -- downed and left -- so the key includes
    the kind.)  Older recordings fall back to the payload plus a coarse time
    bucket: every bot records its own copy at its own receive time, and those
    differ by a millisecond or two.
    """
    seen, out = {}, []
    for r in rows:
        if r["ch"] != "rx":
            continue
        d = r["d"]
        if d.get("t") != "ev":
            continue
        k = d.get("k")
        if kinds and k not in kinds:
            continue
        if "sq" in d:
            key = ("sq", d["sq"], k)
            if key in seen:
                continue
            seen[key] = r["ts"]
            out.append((r["ts"], d))
            continue
        key = json.dumps(d, sort_keys=True)
        last = seen.get(key)
        if last is not None and r["ts"] - last < 0.5:
            seen[key] = r["ts"]
            continue
        seen[key] = r["ts"]
        out.append((r["ts"], d))
    out.sort(key=lambda tv: tv[0])
    return out


def _rank(cause):
    """Position in CAUSE_ORDER; a name it does not know sorts last rather than
    raising.  smoke.py checks that the firmware's DC_NAME list is covered."""
    return CAUSE_ORDER.index(cause) if cause in CAUSE_ORDER else len(CAUSE_ORDER)


def _bits(mask):
    return bin(mask & 0xFF).count("1")


class DamageLedger:
    """Every LL loss in the run, attributed and timestamped.

    Built once and used twice: to explain the deaths, and to show what is
    grinding survivors down between them (which is not the same list -- a
    cause can dominate the daily bleed without ever landing the last point).
    """

    def __init__(self, rows):
        self.rows = rows
        self.records = []          # [{ts, pid, causes: {name: amount}, fatal_hint}]
        self._weather = self._weather_timeline()
        self._build()

    # -- inputs ----------------------------------------------------------
    def _weather_timeline(self):
        """(ts, phase) from the sampled broadcast digests."""
        out = [(r["ts"], r["d"].get("wp")) for r in self.rows
               if r["ch"] == "rx_s" and r["d"].get("wp") is not None]
        out.sort()
        return out

    def weather_at(self, ts):
        phase = None
        for t, wp in self._weather:
            if t > ts:
                break
            phase = wp
        return phase

    def _build(self):
        # The F/W threshold masks latch, so a dawn's *new* bits are the loss.
        # Carrying the last reported mask forward (rather than tracking
        # resets by hand) handles both ways a bit can clear on its own: the
        # track rising back over a threshold, which is a heal, and a slot
        # being re-picked after a death, which hands out a fresh survivor at
        # water 6 with the mask zeroed.  Note `rsp` is *resource* respawn --
        # a pile reappearing, no pid -- not a player one; there is no player
        # respawn event on the wire at all.
        fth = defaultdict(int)
        wth = defaultdict(int)

        for ts, d in _dedupe(self.rows):
            k = d.get("k")
            pid = d.get("pid")
            if pid is None:
                continue

            causes = {}
            if k == "dawn":
                new_f = _bits(d.get("fth", 0) & ~fth[pid])
                new_w = _bits(d.get("wth", 0) & ~wth[pid])
                fth[pid] = d.get("fth", 0)
                wth[pid] = d.get("wth", 0)
                exp = 1 if d.get("expd", 0) < 0 else 0
                # Bad air: the TUNNEL_REST_LL_PCT roll a rest at depth 1
                # makes. Unlike expd this is the point that was *owed*, not
                # the one that landed -- it goes through the ordinary loss
                # path with food and water, so the `landed` scaling below is
                # what decides its share. Absent on builds before it existed,
                # which reads as 0, correctly.
                air = 1 if d.get("air", 0) < 0 else 0
                want = {"hunger": new_f, "thirst": new_w,
                        "exposure": exp, "bad air": air}
                total = sum(want.values())
                if not total:
                    continue
                # dll is the *net* change: a rest heal can offset a loss, and
                # LL clamps at 0, so the losses that actually landed can be
                # fewer than the components predict.  Scale to what landed.
                landed = -min(0, d.get("dll", 0))
                if landed == 0:
                    continue
                causes = {c: n * landed / total for c, n in want.items() if n}
            elif k == "dusk":
                if d.get("lld", 0) < 0:
                    causes = {"radiation": -d["lld"]}
            elif k == "fire_dmg":
                # intensity 10 is the lightning sentinel (a direct strike
                # costs 2 LL). Real fire scales: a blaze (3) costs 2, a burn
                # (2) costs 1 -- mirrors fireDamageFor() in world-system.hpp.
                inten = d.get("intensity", 2)
                if inten == 10:
                    causes = {"lightning": 2}
                else:
                    causes = {"fire": 2 if inten >= 3 else 1}
            elif k == "flood_dmg":
                # A flash flood zeroes MP and takes LL. Older builds sent no
                # llLost and cost nothing, so absence means 0, not a default.
                if d.get("llLost", 0) > 0:
                    causes = {"flood": d["llLost"]}
            elif k == "doom_act":
                if d.get("llLost", 0) > 0:
                    causes = {"creeping doom": d["llLost"]}
            elif k == "enc_res":
                if d.get("out") == 0 and d.get("penLL", 0) < 0:
                    causes = {"encounter hazard": -d["penLL"]}
                elif d.get("out") == 1:
                    # cost_ll is not on the wire; only a death right after a
                    # successful node reveals it, so this is a candidate that
                    # counts for nothing unless a death matches it.
                    self.records.append({"ts": ts, "pid": pid,
                                         "causes": {"encounter cost": 0},
                                         "candidate": True, "k": k, "ev": d})
                    continue
            elif k == "act":
                if d.get("lld", 0) < 0:
                    causes = {"action": -d["lld"]}
            elif k == "dmg":
                # Protocol 2+: the hazards that used to take LL silently.
                if d.get("amt", 0) > 0:
                    causes = {d.get("cause", "unattributed"): d["amt"]}

            if causes:
                self.records.append({"ts": ts, "pid": pid, "causes": causes,
                                     "candidate": False, "k": k, "ev": d})

    # -- outputs ---------------------------------------------------------
    def deaths(self):
        """One entry per death, with the cause it was matched to."""
        out = []
        for r in self.rows:
            if r["ch"] != "rx":
                continue
            d = r["d"]
            if d.get("t") != "ev" or d.get("k") != "downed":
                continue
            ts, pid = r["ts"], d.get("pid")
            wire = d.get("cause")
            if wire:
                # Named by the firmware at the site that took the last LL.
                out.append({"ts": ts, "pid": pid, "cause": wire,
                            "mix": {wire: 1.0}, "via": "wire",
                            "weather": self.weather_at(ts)})
            else:
                out.append(self._explain(ts, pid))
        return out

    def _explain(self, ts, pid, back=WINDOW_BACK, fwd=WINDOW_FWD):
        best, best_score = None, None
        for rec in self.records:
            if rec["pid"] != pid:
                continue
            dt = rec["ts"] - ts
            if not (-back <= dt <= fwd):
                continue
            # A record that reports the survivor at LL 0 is the killing blow;
            # prefer it over a merely nearer one.
            ev = rec["ev"]
            terminal = (ev.get("ll") == 0)
            score = (0 if terminal else 1, abs(dt))
            if best_score is None or score < best_score:
                best, best_score = rec, score

        if best is None:
            wp = self.weather_at(ts)
            cause = SILENT_WEATHER.get(wp, "unattributed")
            return {"ts": ts, "pid": pid, "cause": cause, "mix": {cause: 1.0},
                    "via": "weather" if cause in SILENT_WEATHER.values() else None,
                    "weather": wp}

        mix = best["causes"]
        if best.get("candidate"):
            mix = {"encounter cost": 1.0}
        # The primary cause is the largest component; ties break toward the
        # earlier entry in CAUSE_ORDER, which puts the supply grind first.
        primary = max(mix, key=lambda c: (mix[c], -_rank(c)))
        return {"ts": ts, "pid": pid, "cause": primary, "mix": dict(mix),
                "via": best["k"], "weather": self.weather_at(ts)}

    def loss_ledger(self):
        """Total LL lost per cause across the whole run -- the daily grind,
        as opposed to the killing blow.  Fractional where a single dawn had
        more than one component."""
        tot = Counter()
        for rec in self.records:
            if rec.get("candidate"):
                continue
            for c, n in rec["causes"].items():
                tot[c] += n
        return dict(tot)

    def near_death_causes(self, danger_ll=2):
        """What pushes a survivor into the danger band without killing them.

        The trajectory is sampled (one digest per 50 ticks), so all that is
        known is that the drop happened somewhere between two samples.  The
        lookback therefore runs to the previous sample rather than using a
        death's tight window -- a near-miss is located to a few seconds, not
        to the millisecond.
        """
        traj = defaultdict(list)
        for r in self.rows:
            if r["ch"] == "rx_s" and r["arch"] >= 0:
                traj[r["arch"]].append((r["ts"], r["d"]))
        out = []
        for pid, samples in traj.items():
            prev, prev_ts = 9, 0.0
            for ts, d in samples:
                ll = d.get("ll")
                if ll is None:
                    continue
                if 0 < ll <= danger_ll < prev:
                    out.append(self._explain(ts, pid,
                                             back=max(WINDOW_BACK, ts - prev_ts),
                                             fwd=0.5))
                prev, prev_ts = ll, ts
        return out
