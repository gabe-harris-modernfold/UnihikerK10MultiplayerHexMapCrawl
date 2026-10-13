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
        self.write_errors = 0           # lines lost to a failed write
        self.first_error = None
        self._fh = self.path.open("w", encoding="utf-8", newline="\n")
        if meta:
            self.write("run", -1, meta)

    def write(self, channel: str, arch: int, payload) -> None:
        rec = {"ts": round(time.monotonic() - self.t0, 3),
               "ch": channel, "arch": arch, "d": payload}
        # Never raises.  Every bot writes from inside its own except/finally,
        # so one failed write (a momentarily full disk, ENOSPC) used to kill
        # every bot's task at once while the arena kept printing their last
        # state as if they were still seated.
        try:
            self._fh.write(json.dumps(rec, separators=(",", ":"), default=str) + "\n")
            self.lines += 1
            # Flush on the rare channels so a wedged board or a Ctrl-C leaves a
            # usable tail; the per-tick firehose (rx_s) is left to buffering.
            if channel not in ("rx", "rx_s", "tx", "decide"):
                self._fh.flush()
        except (OSError, ValueError) as e:
            self.write_errors += 1
            if self.first_error is None:
                self.first_error = f"{type(e).__name__}: {e}"
                print(f"  recorder: write failed ({self.first_error}); "
                      f"the run continues, lines are being lost", flush=True)

    def close(self, summary: dict | None = None) -> None:
        if self.write_errors:
            self.write("recorder", -1, {"write_errors": self.write_errors,
                                        "first_error": self.first_error})
        if summary:
            self.write("run", -1, summary)
        try:
            self._fh.flush()
            self._fh.close()
        except OSError as e:
            print(f"  recorder: close failed ({type(e).__name__}: {e})", flush=True)

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
