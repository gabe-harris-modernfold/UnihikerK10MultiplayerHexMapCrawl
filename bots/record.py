"""Run recorder -- one JSONL file per run.

Every line is {"ts": <seconds since run start>, "ch": <channel>, "arch": <slot
or -1>, "d": <payload>}.  Channels: conn conn_err full slot_busy tx rx rx_s
rx_bad decide policy_err telemetry run.

JSONL rather than a database because runs are append-only, the analysis is
a single pass, and a half-written file from a killed run is still readable up
to the last complete line.
"""
import json
import time
from pathlib import Path


class Recorder:
    def __init__(self, path: Path, meta: dict | None = None):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.t0 = time.monotonic()
        self.lines = 0
        self._fh = self.path.open("w", encoding="utf-8", newline="\n")
        if meta:
            self.write("run", -1, meta)

    def write(self, channel: str, arch: int, payload) -> None:
        rec = {"ts": round(time.monotonic() - self.t0, 3),
               "ch": channel, "arch": arch, "d": payload}
        self._fh.write(json.dumps(rec, separators=(",", ":"), default=str) + "\n")
        self.lines += 1
        # Flush on the rare channels so a wedged board or a Ctrl-C leaves a
        # usable tail; the per-tick firehose (rx_s) is left to buffering.
        if channel not in ("rx", "rx_s", "tx", "decide"):
            self._fh.flush()

    def close(self, summary: dict | None = None) -> None:
        if summary:
            self.write("run", -1, summary)
        self._fh.flush()
        self._fh.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def read(path) -> list:
    """Load a run back for analysis.  Tolerates a truncated final line."""
    out = []
    with Path(path).open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                break   # truncated tail from an interrupted run
    return out
