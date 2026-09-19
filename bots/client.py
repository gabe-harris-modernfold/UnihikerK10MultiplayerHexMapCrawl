"""One bot: a WebSocket client that drives a single survivor slot.

Two things here are load-bearing and were both learned the hard way.

**Throttling.** handleMessage() in network-handlers.hpp dispatches every
inbound message synchronously on the AsyncTCP task with no rate limiting of
any kind, and /state showed maxTickMs at 95 against a 100 ms budget with zero
clients connected.  Every send therefore goes through two limiters: a per-bot
minimum interval and an arena-wide one shared by all bots.

**Slot claiming.** The archetype index IS the player slot -- handleMsg_pick
does `Player& p = G.players[arch]` and silently returns if that slot is
already connected.  There is no nack.  So a bot cannot simply assume its
index: it must read the lobby `avail` list, coordinate with its siblings
through a SlotBroker so two bots never claim the same slot, and confirm the
claim landed by waiting for the sync that only a successful pick produces.
A blind pick looks exactly like a successful one until you notice tx=1.

Note broadcastState() uses ws.textAll(), so even a client stuck in the lobby
receives the full per-tick broadcast.  Receiving state is NOT evidence of
having joined.
"""
import asyncio
import json
import random
import time

import websockets

from config import ARCHETYPE_NAME, MAX_PLAYERS
from state import Observation


class RateLimiter:
    """Minimum spacing between sends, with jitter so N bots do not
    synchronise into a periodic burst."""

    def __init__(self, min_interval: float, rng: random.Random, jitter: float = 0.4):
        self.min_interval = min_interval
        self.jitter = jitter
        self.rng = rng
        self._next_at = 0.0
        self._lock = asyncio.Lock()

    async def acquire(self) -> None:
        async with self._lock:
            now = time.monotonic()
            if now < self._next_at:
                await asyncio.sleep(self._next_at - now)
            gap = self.min_interval * (1.0 + self.rng.uniform(0.0, self.jitter))
            self._next_at = time.monotonic() + gap


class SlotBroker:
    """Hands out archetype slots so two bots never pick the same one.

    `avail` comes from the server's lobby message and reflects slots with
    p.connected == false.  A slot held by someone else -- a human watching in
    a browser, or a session the firmware has not reaped yet -- is simply not
    offered, and we never try to evict it.
    """

    def __init__(self):
        self._claimed: set[int] = set()
        self._lock = asyncio.Lock()

    async def claim(self, avail, preferred: int) -> int | None:
        async with self._lock:
            free = [a for a in avail if a not in self._claimed]
            if not free:
                return None
            slot = preferred if preferred in free else min(free)
            self._claimed.add(slot)
            return slot

    async def release(self, slot: int) -> None:
        if slot is None or slot < 0:
            return
        async with self._lock:
            self._claimed.discard(slot)


class BotClient:
    def __init__(self, host, preferred_arch, policy, recorder, rng,
                 limiter, global_limiter, broker, decide_interval=0.35,
                 connect_timeout=10.0, sync_timeout=12.0):
        self.host = host
        self.preferred_arch = preferred_arch
        self.arch = -1                  # assigned once a pick is confirmed
        self.policy = policy
        self.recorder = recorder
        self.rng = rng
        self.limiter = limiter
        self.global_limiter = global_limiter
        self.broker = broker
        self.decide_interval = decide_interval
        self.connect_timeout = connect_timeout
        self.sync_timeout = sync_timeout

        self.obs = Observation()
        self.ws = None
        self.sent = 0
        self.received = 0
        self.errors = 0
        self.refused_full = 0
        self.pick_failures = 0
        # arch is cleared whenever the connection drops (including at
        # shutdown), so these two remember what actually happened for the
        # end-of-run summary.
        self.ever_joined = False
        self.seated_arch = -1
        self.stop = asyncio.Event()
        self._pending_slot = None
        self._synced_evt = asyncio.Event()

    @property
    def label(self):
        a = self.arch if self.arch >= 0 else self.seated_arch
        if a < 0:
            a = self.preferred_arch
        who = ARCHETYPE_NAME[a] if 0 <= a < MAX_PLAYERS else "unassigned"
        tag = "" if self.ever_joined else "?"
        return f"{a}{tag}:{who}:{self.policy.name}"

    @property
    def url(self):
        return f"ws://{self.host}/ws"

    def _log_arch(self):
        return self.arch if self.arch >= 0 else self.preferred_arch

    async def _send(self, msg: dict) -> None:
        await self.global_limiter.acquire()
        await self.limiter.acquire()
        if self.ws is None:
            return
        await self.ws.send(json.dumps(msg, separators=(",", ":")))
        self.sent += 1
        self.recorder.write("tx", self._log_arch(), msg)

    async def run(self) -> None:
        backoff = 1.0
        while not self.stop.is_set():
            try:
                async with websockets.connect(
                    self.url, open_timeout=self.connect_timeout,
                    ping_interval=20, ping_timeout=20, max_size=2 ** 20,
                ) as ws:
                    self.ws = ws
                    backoff = 1.0
                    self.recorder.write("conn", self._log_arch(), {"url": self.url})
                    await self._session()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self.errors += 1
                self.recorder.write("conn_err", self._log_arch(),
                                    {"err": f"{type(e).__name__}: {e}"})
            finally:
                self.ws = None
                await self.broker.release(self._pending_slot)
                await self.broker.release(self.arch)
                self._pending_slot = None
                self.arch = -1
                self.obs.synced = False
                self._synced_evt.clear()
            if self.stop.is_set():
                break
            await asyncio.sleep(backoff + self.rng.uniform(0, 0.5))
            backoff = min(backoff * 2, 15.0)

    async def _session(self) -> None:
        decider = asyncio.create_task(self._decide_loop())
        watchdog = asyncio.create_task(self._pick_watchdog())
        try:
            async for raw in self.ws:
                if self.stop.is_set():
                    break
                self.received += 1
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    self.recorder.write("rx_bad", self._log_arch(), {"raw": raw[:200]})
                    continue
                await self._on_message(msg)
        finally:
            for t in (decider, watchdog):
                t.cancel()
                try:
                    await t
                except asyncio.CancelledError:
                    pass

    async def _pick_watchdog(self) -> None:
        """A refused pick is silent -- no nack, and the lobby client keeps
        receiving broadcasts as if nothing were wrong.  The only evidence of
        success is the sync that sendSync() unicasts after a pick lands.  If
        it does not arrive, drop the connection so run() reconnects and tries
        a different slot."""
        while not self.stop.is_set():
            await asyncio.sleep(0.5)
            if self._pending_slot is None:
                continue
            try:
                await asyncio.wait_for(self._synced_evt.wait(), self.sync_timeout)
            except asyncio.TimeoutError:
                self.pick_failures += 1
                self.recorder.write("pick_timeout", self._log_arch(),
                                    {"slot": self._pending_slot})
                await self.broker.release(self._pending_slot)
                self._pending_slot = None
                if self.ws is not None:
                    await self.ws.close()
                return
            return

    async def _on_message(self, msg: dict) -> None:
        t = msg.get("t")
        if t == "full":
            self.refused_full += 1
            self.recorder.write("full", self._log_arch(), {})
            await self.ws.close()
            return

        if t == "lobby" and self.arch < 0 and self._pending_slot is None:
            avail = msg.get("avail", [])
            slot = await self.broker.claim(avail, self.preferred_arch)
            if slot is None:
                self.recorder.write("no_slot", self._log_arch(), {"avail": avail})
                await asyncio.sleep(2.0)
                await self.ws.close()
                return
            self._pending_slot = slot
            self.recorder.write("claim", slot,
                                {"avail": avail, "preferred": self.preferred_arch})
            await self._send({"t": "pick", "arch": slot})
            return

        was_synced = self.obs.synced
        self.obs.apply(msg)

        # sendSync() only unicasts after a pick succeeds -- that is our ack.
        if not was_synced and self.obs.synced and self._pending_slot is not None:
            self.arch = self.obs.pid if self.obs.pid >= 0 else self._pending_slot
            self.ever_joined = True
            self.seated_arch = self.arch
            self._pending_slot = None
            self._synced_evt.set()
            self.recorder.write("joined", self.arch,
                                {"policy": self.policy.name, "label": self.label})

        if t == "ev":
            self.policy.on_event(msg)
        if t != "s":
            self.recorder.write("rx", self._log_arch(), msg)
        elif self.obs.tick % 50 == 0:
            self.recorder.write("rx_s", self._log_arch(), self._digest())

    def _digest(self) -> dict:
        me = self.obs.me
        return {"tk": self.obs.tick, "day": self.obs.day, "wp": self.obs.weather,
                "sc": me.score, "sp": me.steps, "ll": me.ll, "mp": me.mp,
                "food": me.food, "water": me.water, "rad": me.rad,
                "q": me.q, "r": me.r, "inv": me.inv, "vm": me.valid_moves}

    async def _decide_loop(self) -> None:
        while not self.stop.is_set():
            await asyncio.sleep(self.decide_interval)
            # arch < 0 means the pick has not been confirmed; acting now would
            # just be shouting into the lobby.
            if self.arch < 0 or not self.obs.synced or self.ws is None:
                continue
            try:
                action = self.policy.decide(self.obs)
            except Exception as e:
                self.errors += 1
                self.recorder.write("policy_err", self._log_arch(),
                                    {"err": f"{type(e).__name__}: {e}"})
                continue
            msg = action.to_msg()
            if msg is None:
                continue
            self.recorder.write("decide", self._log_arch(),
                                {"kind": action.kind, "why": action.why})
            await self._send(msg)

    def joined(self) -> bool:
        """Seated right now.  Use ever_joined for post-run reporting -- the
        teardown path clears arch, so this reads False after a clean stop."""
        return self.arch >= 0 and self.obs.synced

    def summary(self) -> dict:
        me = self.obs.me
        return {"arch": self.seated_arch, "label": self.label,
                "policy": self.policy.name,
                "joined": self.ever_joined, "score": me.score, "steps": me.steps,
                "ll": me.ll, "day": self.obs.day, "sent": self.sent,
                "received": self.received, "errors": self.errors,
                "refused_full": self.refused_full,
                "pick_failures": self.pick_failures}
