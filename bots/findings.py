"""Findings -- the one output format for "a bot found a problem".

Balance runs produce metrics; the fuzzer, the chaos scenarios, the soak and
the invariant oracles every bot now carries produce *findings*.  One shape
for all of them, so a run of any kind ends with the same answer to the same
question: what is wrong, how bad, and how do I make it happen again.

A finding is keyed by its **signature** -- `check` plus whatever detail
distinguishes one instance of the defect from another (the pid, the command,
the fuzz case).  The first few occurrences of a signature are written to the
run log in full, with the last messages this connection sent as a repro;
after that only the count grows, so a check that fires every tick does not
drown the log.

    python findings.py                       # newest run in runs/
    python findings.py runs/fuzz-*.jsonl     # any set of logs
"""
import argparse
import json
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

SEVERITIES = ("critical", "major", "minor")
# How many occurrences of one signature are written out in full.
WRITE_FIRST = 3
# How many outbound messages a connection keeps for a repro.
REPRO_DEPTH = 12


@dataclass
class Finding:
    check: str                 # stable id, e.g. "ll_over_cap", "fuzz:m-no-d"
    severity: str              # critical | major | minor
    summary: str               # one line a person can read
    source: str = ""           # which bot / probe saw it
    detail: dict = field(default_factory=dict)
    repro: list = field(default_factory=list)   # last messages sent, oldest first
    key: tuple = ()            # extra signature parts beyond `check`

    @property
    def signature(self) -> tuple:
        return (self.check,) + tuple(self.key)


class FindingLog:
    """Collects findings for one run.  Shared by every bot in that run, so a
    defect two bots both trip over is one signature with a count of two."""

    def __init__(self, recorder=None):
        self.recorder = recorder
        self.counts: dict[tuple, int] = {}
        self.first: dict[tuple, Finding] = {}
        self.first_ts: dict[tuple, float] = {}

    def add(self, f: Finding) -> None:
        if f.severity not in SEVERITIES:
            raise ValueError(f"bad severity {f.severity!r}")
        sig = f.signature
        n = self.counts.get(sig, 0) + 1
        self.counts[sig] = n
        if n == 1:
            self.first[sig] = f
            self.first_ts[sig] = time.monotonic()
        if n <= WRITE_FIRST and self.recorder is not None:
            self.recorder.write("finding", -1, {
                "check": f.check, "severity": f.severity, "summary": f.summary,
                "source": f.source, "detail": f.detail, "repro": f.repro,
                "key": list(f.key), "n": n})

    def __len__(self):
        return len(self.counts)

    def worst(self) -> str | None:
        for sev in SEVERITIES:
            if any(f.severity == sev for f in self.first.values()):
                return sev
        return None

    def summary(self) -> list[dict]:
        """One row per signature, most severe first, then most frequent."""
        rows = [{"check": f.check, "severity": f.severity, "summary": f.summary,
                 "key": list(f.key), "count": self.counts[sig], "source": f.source}
                for sig, f in self.first.items()]
        rows.sort(key=lambda r: (SEVERITIES.index(r["severity"]), -r["count"]))
        return rows

    def flush(self) -> None:
        """Write the full per-signature counts -- the log itself only keeps
        the first WRITE_FIRST occurrences of each.  Call once, at run end."""
        if self.recorder is not None:
            self.recorder.write("findings", -1, {"rows": self.summary()})

    def print_summary(self, out=None, indent="    ") -> None:
        out = out or sys.stdout
        rows = self.summary()
        if not rows:
            print(f"{indent}findings: none", file=out)
            return
        print(f"{indent}findings: {len(rows)} signature(s), worst={self.worst()}",
              file=out)
        for r in rows:
            k = f" {r['key']}" if r["key"] else ""
            print(f"{indent}  [{r['severity']:<8}] x{r['count']:<4} {r['check']}{k}"
                  f" -- {r['summary']}", file=out)


class ReproBuffer:
    """The last few messages one connection sent -- attached to every finding
    that connection raises, because "what did we just do" is most of a repro."""

    def __init__(self, depth: int = REPRO_DEPTH):
        self._buf = deque(maxlen=depth)

    def add(self, msg) -> None:
        self._buf.append(msg if isinstance(msg, (dict, str)) else repr(msg))

    def snapshot(self) -> list:
        return list(self._buf)


# -- CLI -------------------------------------------------------------------

def load(paths) -> list[dict]:
    """Every finding row, with `n` raised to the run-end total where the log
    has one (a run killed mid-way has only the per-occurrence rows)."""
    out = []
    for p in paths:
        rows, totals = [], {}
        with open(p, encoding="utf-8") as fh:
            for line in fh:
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if rec.get("ch") == "finding":
                    d = dict(rec["d"])
                    d["log"] = Path(p).name
                    rows.append(d)
                elif rec.get("ch") == "findings":
                    for r in rec["d"].get("rows", []):
                        totals[(r["check"],) + tuple(r.get("key") or ())] = r["count"]
        for d in rows:
            sig = (d["check"],) + tuple(d.get("key") or ())
            if sig in totals:
                d["n"] = max(d.get("n", 1), totals[sig])
        out.extend(rows)
    return out


def report(rows: list[dict], out=None) -> int:
    """Group across logs by signature.  Returns the number of signatures."""
    out = out or sys.stdout
    groups: dict[tuple, dict] = {}
    for d in rows:
        sig = (d["check"],) + tuple(d.get("key") or ())
        g = groups.setdefault(sig, {"first": d, "per_log": {}, "logs": set()})
        # Only the first WRITE_FIRST occurrences reach the log, each carrying
        # its running count, so the highest n per log is that log's total.
        g["per_log"][d["log"]] = max(g["per_log"].get(d["log"], 0), d.get("n", 1))
        g["logs"].add(d["log"])
    for g in groups.values():
        g["count"] = sum(g["per_log"].values())
    order = sorted(groups.items(), key=lambda kv: (
        SEVERITIES.index(kv[1]["first"]["severity"]), -kv[1]["count"]))
    if not order:
        print("no findings", file=out)
        return 0
    for sig, g in order:
        f = g["first"]
        print(f"[{f['severity']:<8}] {sig[0]}{' ' + str(list(sig[1:])) if sig[1:] else ''}",
              file=out)
        print(f"    {f['summary']}", file=out)
        print(f"    seen {g['count']}x across {len(g['logs'])} log(s); "
              f"first in {sorted(g['logs'])[0]} from {f.get('source') or '?'}", file=out)
        if f.get("detail"):
            print(f"    detail: {json.dumps(f['detail'], default=str)[:300]}", file=out)
        if f.get("repro"):
            print("    repro (last sent, oldest first):", file=out)
            for m in f["repro"][-6:]:
                print(f"      {json.dumps(m, default=str)[:200]}", file=out)
    return len(order)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    a = ap.parse_args(argv)
    paths = a.logs
    if not paths:
        runs = sorted((Path(__file__).parent / "runs").glob("*.jsonl"),
                      key=lambda p: p.stat().st_mtime)
        if not runs:
            raise SystemExit("no logs in runs/")
        paths = [runs[-1]]
    n = report(load(paths))
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
