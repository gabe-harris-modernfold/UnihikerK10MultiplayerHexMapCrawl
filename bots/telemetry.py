"""Board health poller.

The soak test is not just "does it crash" -- it is a measurable question.
A baseline /state read with zero clients connected showed:

    maxTickMs 95   (against a 100 ms tick budget)
    minHeap   43468 bytes
    broadcastPartial 1195
    assetReqRejects  10

maxTickMs at 95/100 means the game loop was already nearly saturating its own
period before any bot connected, and broadcastState() serialises up to ~3.3 KB
per client per tick.  So the numbers to watch as bots attach are maxTickMs
(pins at 100 = overloaded), broadcastSkips (mutex contention), broadcastPartial
(client send queues filling) and minHeap (the HTTP-wedge precursor documented
in docs/dev-loop.md).

This polls over plain HTTP, which is a separate path from the WebSockets, so
it keeps reporting even when the WS side is struggling.
"""
import asyncio
import json
import urllib.request

WATCH_KEYS = ("heap", "minHeap", "maxBlock", "psram", "maxTickMs",
              "broadcastSkips", "broadcastSkipsConsec", "broadcastPartial",
              "assetReqActive", "assetReqRejects", "uptimeMs")


def fetch_state(host: str, timeout: float = 5.0) -> dict:
    with urllib.request.urlopen(f"http://{host}/state", timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def digest(state: dict) -> dict:
    """Pull the health numbers plus a little game context out of /state."""
    mem = state.get("mem", {})
    out = {k: mem.get(k) for k in WATCH_KEYS}
    out["day"] = state.get("day")
    out["tickId"] = state.get("tickId")
    out["weather"] = state.get("weather")
    out["connected"] = state.get("connected")
    out["evtQueue"] = state.get("evtQueue")
    scores = [p.get("score", 0) for p in state.get("players", []) if p.get("conn")]
    out["scores"] = scores
    return out


class TelemetryPoller:
    """Polls /state on an interval and records a digest.  Raises nothing --
    a failed poll is itself a signal and gets recorded as such."""

    def __init__(self, host: str, recorder, interval: float = 5.0):
        self.host = host
        self.recorder = recorder
        self.interval = interval
        self.stop = asyncio.Event()
        self.last: dict | None = None
        self.worst_tick = 0
        self.min_heap = None
        self.failures = 0

    async def run(self) -> None:
        while not self.stop.is_set():
            try:
                state = await asyncio.to_thread(fetch_state, self.host)
                d = digest(state)
                self.last = d
                if d.get("maxTickMs"):
                    self.worst_tick = max(self.worst_tick, d["maxTickMs"])
                if d.get("minHeap") is not None:
                    self.min_heap = (d["minHeap"] if self.min_heap is None
                                     else min(self.min_heap, d["minHeap"]))
                self.recorder.write("telemetry", -1, d)
            except Exception as e:
                self.failures += 1
                self.recorder.write("telemetry", -1,
                                    {"err": f"{type(e).__name__}: {e}"})
            try:
                await asyncio.wait_for(self.stop.wait(), timeout=self.interval)
            except asyncio.TimeoutError:
                pass

    def summary(self) -> dict:
        return {"worst_maxTickMs": self.worst_tick, "min_heap": self.min_heap,
                "poll_failures": self.failures, "last": self.last}
