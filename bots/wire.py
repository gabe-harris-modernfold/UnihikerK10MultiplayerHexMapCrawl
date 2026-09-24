"""The wire layer: request/reply tracking, and a scriptable connection.

Two consumers, one mechanism:

* `BotClient` (client.py) plays a policy.  It fires requests and moves on;
  replies are bookkeeping -- counted, logged, handed to the policy.
* `ProbeClient` (below) runs a script -- the fuzzer, the chaos scenarios.
  It needs the opposite: send one thing, *wait* for the verdict, assert on it.

Both go through `ReplyTracker`, so "every request gets exactly one reply"
(network-reply.hpp) is checked the same way everywhere: an unknown or
repeated rid is a finding, and so is silence from a protocol 2 board.

Layering, bottom up:

    ReplyTracker     rid assignment, reply matching, expiry
    ProbeClient      socket + Observation + WireOracle + ReplyTracker
    fuzz.py / chaos.py   scripts on top of ProbeClient

`connect` is injectable so all of this runs offline against a fake socket
(smoke.py does exactly that).
"""
import asyncio
import json
import random
import time

from findings import FindingLog, ReproBuffer
from oracles import WireOracle
from state import Observation

# A request with no reply after this long is written off as unanswered.
REPLY_TIMEOUT_S = 10.0


class ReplyTracker:
    def __init__(self, oracle: WireOracle | None = None, clock=time.monotonic):
        self.oracle = oracle
        self.clock = clock
        self._next_rid = 1
        self.inflight: dict[int, tuple[str, float, asyncio.Future | None]] = {}
        self.acks = 0
        self.nacks: dict[str, dict[str, int]] = {}   # cmd -> why -> count
        self.unanswered = 0

    def stamp(self, msg: dict, want_future: bool = False):
        """Give `msg` the next rid.  Returns (msg, future-or-None)."""
        rid = self._next_rid
        self._next_rid += 1
        fut = asyncio.get_running_loop().create_future() if want_future else None
        self.inflight[rid] = (msg.get("t", "?"), self.clock(), fut)
        return dict(msg, rid=rid), fut

    def resolve(self, reply: dict):
        """Match one ack/nack.  Returns (cmd, ok, why), or None for a reply to
        a rid we are not waiting on -- which on protocol 2 means the board
        answered something twice, or answered something it was never sent."""
        ok = reply.get("t") == "ack"
        rid = reply.get("rid")
        entry = self.inflight.pop(rid, None)
        why = None if ok else reply.get("why", "?")
        if entry is None:
            if self.oracle is not None:
                self.oracle.flag("reply_unmatched", "major",
                                 "a reply for a rid with nothing in flight "
                                 "(answered twice, or never asked)",
                                 key=(reply.get("cmd"),), reply=reply)
            return None
        cmd, _at, fut = entry
        cmd = reply.get("cmd") or cmd
        if ok:
            self.acks += 1
        else:
            per = self.nacks.setdefault(cmd, {})
            per[why] = per.get(why, 0) + 1
        if fut is not None and not fut.done():
            fut.set_result(reply)
        return cmd, ok, why

    def expire(self, proto: int, timeout: float = REPLY_TIMEOUT_S) -> list:
        """Drop requests nobody answered.  Only a fault once the board has said
        it replies (protocol 2) -- the mock and older firmware never do."""
        now = self.clock()
        stale = [r for r, (_c, at, _f) in self.inflight.items() if now - at > timeout]
        out = []
        for r in stale:
            cmd, _at, fut = self.inflight.pop(r)
            if fut is not None and not fut.done():
                fut.set_result(None)
            if proto >= 2:
                self.unanswered += 1
                out.append((r, cmd))
                if self.oracle is not None:
                    self.oracle.flag("request_unanswered", "major",
                                     "a protocol 2 board never replied to a request",
                                     key=(cmd,), rid=r, cmd=cmd)
        return out

    def clear(self) -> None:
        """A new socket has no replies coming for the old one's requests."""
        for _c, _at, fut in self.inflight.values():
            if fut is not None and not fut.done():
                fut.set_result(None)
        self.inflight.clear()

    def nack_total(self) -> int:
        return sum(sum(w.values()) for w in self.nacks.values())


class ProbeClient:
    """One scripted connection.  Not a player: it seats when told to, sends
    exactly what it is told to, and reports what came back."""

    def __init__(self, host: str, findings: FindingLog, source: str = "probe",
                 recorder=None, min_interval: float = 0.30, rng=None,
                 connect=None):
        self.host = host
        self.findings = findings
        self.source = source
        self.recorder = recorder
        self.rng = rng or random.Random(0)
        self.min_interval = min_interval
        self._connect = connect
        self.obs = Observation()
        self.repro = ReproBuffer()
        self.oracle = WireOracle(findings, source, self.repro)
        self.tracker = ReplyTracker(self.oracle)
        self.ws = None
        self.lobby = None                  # last lobby message
        self.closed = asyncio.Event()
        self.close_info = None
        self.received: list[dict] = []     # every non-tick message, in order
        self._reader = None
        self._waiters: list[tuple] = []
        self._next_send = 0.0
        self.arch = -1

    @property
    def url(self):
        return f"ws://{self.host}/ws"

    # -- lifecycle -----------------------------------------------------------
    async def open(self, timeout: float = 10.0) -> None:
        connect = self._connect
        if connect is None:
            import websockets
            connect = websockets.connect
        self.ws = await connect(self.url, open_timeout=timeout,
                                ping_interval=20, ping_timeout=20, max_size=2 ** 20)
        self.closed.clear()
        self.oracle.new_socket()
        self.tracker.clear()
        self.obs = Observation()
        self.arch = -1
        self.lobby = None
        self._reader = asyncio.create_task(self._read())

    async def close(self) -> None:
        if self.ws is not None:
            try:
                await self.ws.close()
            except Exception:
                pass
        await self._stop_reader()

    async def abort(self) -> None:
        """Drop the TCP connection with no close handshake -- what a phone
        losing signal looks like to the board."""
        tr = getattr(self.ws, "transport", None)
        if tr is not None:
            tr.abort()
        else:
            await self.close()
            return
        await self._stop_reader()

    async def _stop_reader(self) -> None:
        if self._reader is not None:
            self._reader.cancel()
            try:
                await self._reader
            except (asyncio.CancelledError, Exception):
                pass
            self._reader = None
        self.closed.set()
        self.tracker.clear()

    # -- inbound ---------------------------------------------------------------
    async def _read(self) -> None:
        try:
            async for raw in self.ws:
                try:
                    msg = json.loads(raw)
                except (json.JSONDecodeError, TypeError):
                    self._log("rx_bad", {"raw": str(raw)[:200]})
                    continue
                self._dispatch(msg)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            self.close_info = f"{type(e).__name__}: {e}"
        finally:
            code = getattr(self.ws, "close_code", None)
            if self.close_info is None and code is not None:
                self.close_info = f"closed code={code}"
            self.closed.set()

    def _dispatch(self, msg: dict) -> None:
        t = msg.get("t")
        if t in ("ack", "nack"):
            r = self.tracker.resolve(msg)
            if r is not None:
                cmd, ok, why = r
                self.oracle.on_reply(ok, cmd, why, seated=self.seated)
                if not ok:
                    self._log("nack", {"rid": msg.get("rid"), "cmd": cmd, "why": why})
        else:
            if t == "lobby":
                self.lobby = msg
            self.obs.apply(msg)
            self.oracle.on_rx(msg, self.obs)
            if t != "s":
                self.received.append(msg)
                self._log("rx", msg)
        self.oracle.poll()
        for w in list(self._waiters):
            pred, fut = w
            if not fut.done() and pred(msg):
                fut.set_result(msg)
                self._waiters.remove(w)

    async def wait_for(self, pred, timeout: float = 5.0):
        """The next message matching pred, or None on timeout / close."""
        fut = asyncio.get_running_loop().create_future()
        w = (pred, fut)
        self._waiters.append(w)
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            return None
        finally:
            if w in self._waiters:
                self._waiters.remove(w)

    # -- outbound --------------------------------------------------------------
    async def _throttle(self) -> None:
        now = time.monotonic()
        if now < self._next_send:
            await asyncio.sleep(self._next_send - now)
        self._next_send = time.monotonic() + self.min_interval * (
            1.0 + self.rng.uniform(0.0, 0.3))

    async def request(self, msg: dict, timeout: float = 3.0):
        """Send with a rid and wait for its ack/nack.  Returns the reply, or
        None if none came (a protocol 1 board, or a lost reply)."""
        await self._throttle()
        stamped, fut = self.tracker.stamp(msg, want_future=True)
        self.oracle.on_tx(stamped)
        self._log("tx", stamped)
        await self.ws.send(json.dumps(stamped, separators=(",", ":")))
        try:
            return await asyncio.wait_for(asyncio.shield(fut), timeout)
        except asyncio.TimeoutError:
            return None

    async def send_raw(self, data, fragments: int = 0) -> None:
        """Send exactly `data` (str -> text frame, bytes -> binary frame), no
        rid, no tracking.  fragments > 1 splits it across continuation frames,
        which handleMessage never reassembles."""
        await self._throttle()
        self.oracle.on_tx(data if isinstance(data, str) else repr(data))
        self._log("tx_raw", {"data": data if isinstance(data, str) else repr(data),
                             "fragments": fragments})
        if fragments and fragments > 1 and len(data) >= fragments:
            n = len(data) // fragments
            parts = [data[i:i + n] for i in range(0, len(data), n)]
            await self.ws.send(iter(parts))
        else:
            await self.ws.send(data)

    # -- seating ---------------------------------------------------------------
    @property
    def seated(self) -> bool:
        return self.arch >= 0 and self.obs.synced

    async def seat(self, prefer=None, timeout: float = 12.0) -> int:
        """Take a free slot.  Returns the slot, or -1.  Prefers the highest
        free slot by default, so probes stay out of the way of arena bots,
        which fill from 0 upward."""
        if self.lobby is None:
            await self.wait_for(lambda m: m.get("t") in ("lobby", "full"), timeout)
        if self.lobby is None:
            return -1
        avail = list(self.lobby.get("avail", []))
        if not avail:
            return -1
        slot = prefer if prefer in avail else max(avail)
        synced = asyncio.create_task(
            self.wait_for(lambda m: m.get("t") == "sync", timeout))
        reply = await self.request({"t": "pick", "arch": slot}, timeout=timeout)
        if reply is not None and reply.get("t") == "nack":
            synced.cancel()
            try:
                await synced
            except asyncio.CancelledError:
                pass
            return -1
        if await synced is None:
            return -1
        self.arch = self.obs.pid if self.obs.pid >= 0 else slot
        return self.arch

    def _log(self, ch, d) -> None:
        if self.recorder is not None:
            self.recorder.write(ch, self.arch, dict(d, src=self.source)
                                if isinstance(d, dict) else d)
