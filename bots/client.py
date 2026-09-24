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

**Replies (protocol 2+).** Every send carries a "rid", and firmware at
PROTO_VERSION 2 answers each with exactly one ack or nack (network-reply.hpp).
A nack names why -- "slot_taken", "no_mp", "in_enc" -- so a refused pick is
retried at once instead of after the sync watchdog, and every refusal lands
in the run log.  Older firmware and the mock ignore "rid" and never reply;
nothing here depends on a reply arriving, and unanswered requests are only
counted once sync has said the board speaks protocol 2.  The bookkeeping is
wire.ReplyTracker, shared with the probes.

**Every bot is a tester.**  Each connection carries a WireOracle
(oracles.py) that checks every tick, event and reply against what the
firmware promises, and reports into the run's FindingLog (findings.py).  A
balance run that trips over a bug says so at the end instead of leaving it
to be inferred from a strange metric.
"""
import asyncio
import json
import random
import time

import websockets

from config import ARCHETYPE_NAME, MAX_PLAYERS
from findings import Finding, FindingLog, ReproBuffer
from oracles import WireOracle
from state import Observation
from wire import ReplyTracker

# How often to re-log a persisting noop reason. A stalled fleet should be
# obvious in the log within seconds, without a line every decide cycle.
NOOP_HEARTBEAT_S = 15.0


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
                 connect_timeout=10.0, sync_timeout=12.0,
                 respawn_after=15.0, findings: FindingLog | None = None):
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
        # Seconds to stay downed before reconnecting to respawn.  Long enough
        # that a dawn heal can revive us in place first.
        self.respawn_after = respawn_after

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
        self.seats_lost = 0
        self.respawns = 0
        self._downed_since = None
        self._last_noop_why = None
        self._last_noop_log = 0.0
        self._noop_streak = 0
        self.stop = asyncio.Event()
        # Connection lifecycle.  Worth tracking explicitly rather than
        # reconstructing from the log: the board can drop a player slot while
        # the socket stays open, so "am I connected" and "do I have a seat"
        # are two different questions and both can fail silently.
        self.connects = 0
        self.disconnects = 0
        self.episodes = []          # one dict per socket lifetime
        self._episode = None
        self._close_reason = None
        self._pending_slot = None
        self._synced_evt = asyncio.Event()
        # Shared with every other bot in the run, so one defect two bots hit
        # is one signature with a count of two.
        self.findings = findings if findings is not None else FindingLog(recorder)
        self.repro = ReproBuffer()
        self.oracle = WireOracle(self.findings, f"bot{preferred_arch}:{policy.name}",
                                 self.repro)
        self.replies = ReplyTracker(self.oracle)

    # Reply counters, read by summary() and arena's report.
    @property
    def acks(self) -> int:
        return self.replies.acks

    @property
    def nacks(self) -> dict:
        return self.replies.nacks

    @property
    def unanswered(self) -> int:
        return self.replies.unanswered

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
        for rid, cmd in self.replies.expire(self.obs.proto):
            self.recorder.write("unanswered", self._log_arch(),
                                {"rid": rid, "cmd": cmd})
        msg, _ = self.replies.stamp(msg)
        self.oracle.on_tx(msg)
        await self.ws.send(json.dumps(msg, separators=(",", ":")))
        self.sent += 1
        self.recorder.write("tx", self._log_arch(), msg)

    async def _on_reply(self, msg: dict) -> None:
        r = self.replies.resolve(msg)
        if r is None:
            return                  # unmatched: the tracker has flagged it
        cmd, ok, why = r
        if not ok:
            self.recorder.write("nack", self._log_arch(),
                                {"rid": msg.get("rid"), "cmd": cmd, "why": why})
        self.oracle.on_reply(ok, cmd, why, seated=self.joined())
        self.policy.on_reply(cmd, ok, why)
        # A refused pick used to be indistinguishable from a slow one, so the
        # watchdog waited out sync_timeout.  Now the board says so: give the
        # slot back and reconnect for a fresh lobby list straight away.
        if not ok and cmd == "pick" and self._pending_slot is not None:
            self.pick_failures += 1
            self._close_reason = f"pick_nack:{why}"
            await self.broker.release(self._pending_slot)
            self._pending_slot = None
            if self.ws is not None:
                await self.ws.close()

    async def run(self) -> None:
        backoff = 1.0
        while not self.stop.is_set():
            try:
                self._episode = {"opened": time.monotonic(), "joined": None,
                                 "arch": None, "closed": None, "reason": None,
                                 "tx": self.sent, "rx": self.received}
                self._close_reason = None
                async with websockets.connect(
                    self.url, open_timeout=self.connect_timeout,
                    ping_interval=20, ping_timeout=20, max_size=2 ** 20,
                ) as ws:
                    self.ws = ws
                    backoff = 1.0
                    self.connects += 1
                    self.oracle.new_socket()
                    self.recorder.write("conn", self._log_arch(),
                                        {"url": self.url, "n": self.connects})
                    await self._session()
                self._close_reason = self._close_reason or "peer_closed"
            except asyncio.CancelledError:
                self._close_reason = "cancelled"
                self._finish_episode()
                raise
            except Exception as e:
                self.errors += 1
                self._close_reason = f"{type(e).__name__}"
                self.recorder.write("conn_err", self._log_arch(),
                                    {"err": f"{type(e).__name__}: {e}"})
            finally:
                self._finish_episode()
                self.ws = None
                await self.broker.release(self._pending_slot)
                await self.broker.release(self.arch)
                self._pending_slot = None
                self.replies.clear()        # a new socket has no replies coming
                self.arch = -1
                self.obs.synced = False
                self._downed_since = None
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
                self._close_reason = "pick_timeout"
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
        if t in ("ack", "nack"):
            await self._on_reply(msg)
            return
        if t == "full":
            self.refused_full += 1
            self._close_reason = "board_full"
            self.recorder.write("full", self._log_arch(), {})
            await self.ws.close()
            return

        if t == "lobby" and self.arch < 0 and self._pending_slot is None:
            avail = msg.get("avail", [])
            slot = await self.broker.claim(avail, self.preferred_arch)
            if slot is None:
                self._close_reason = "no_slot"
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
        self.oracle.on_rx(msg, self.obs)

        # sendSync() only unicasts after a pick succeeds -- that is our ack.
        if not was_synced and self.obs.synced and self._pending_slot is not None:
            self.arch = self.obs.pid if self.obs.pid >= 0 else self._pending_slot
            self.ever_joined = True
            self.seated_arch = self.arch
            if self._episode is not None:
                self._episode["joined"] = time.monotonic()
                self._episode["arch"] = self.arch
            self._pending_slot = None
            self._synced_evt.set()
            # ev messages are broadcast to every client, so the policy needs
            # to know which pid is its own before it starts counting anything.
            self.policy.set_pid(self.arch)
            self.recorder.write("joined", self.arch,
                                {"policy": self.policy.name, "label": self.label})

        if t == "ev":
            self.policy.on_event(msg)

        if t == "s" and self.arch >= 0 and self.obs.synced:
            if await self._check_seat():
                return

        if t != "s":
            self.recorder.write("rx", self._log_arch(), msg)
        elif self.obs.tick % 50 == 0:
            self.recorder.write("rx_s", self._log_arch(), self._digest())

    def _finish_episode(self) -> None:
        """Close out one socket lifetime and record how it ended."""
        ep = self._episode
        if ep is None:
            return
        self._episode = None
        now = time.monotonic()
        ep["closed"] = now
        ep["reason"] = self._close_reason or "unknown"
        ep["arch"] = self.arch if self.arch >= 0 else ep["arch"]
        ep["open_s"] = round(now - ep["opened"], 1)
        # Seated time is what actually matters -- a socket that never got a
        # seat contributed nothing but broadcast load.
        ep["seated_s"] = round(now - ep["joined"], 1) if ep["joined"] else 0.0
        ep["tx"] = self.sent - ep["tx"]
        ep["rx"] = self.received - ep["rx"]
        self.episodes.append(ep)
        if ep["joined"]:
            self.disconnects += 1
        self.recorder.write("disconn", self._log_arch(), ep)

    def connection_report(self) -> dict:
        """Everything about this bot's time on the wire."""
        eps = self.episodes + ([self._episode] if self._episode else [])
        seated = sum(e.get("seated_s") or 0 for e in self.episodes)
        if self._episode and self._episode.get("joined"):
            seated += time.monotonic() - self._episode["joined"]
        reasons = {}
        for e in self.episodes:
            reasons[e.get("reason", "?")] = reasons.get(e.get("reason", "?"), 0) + 1
        return {"connects": self.connects, "disconnects": self.disconnects,
                "episodes": len(eps), "seated_s": round(seated, 1),
                "seats_lost": self.seats_lost, "respawns": self.respawns,
                "pick_failures": self.pick_failures,
                "refused_full": self.refused_full,
                "close_reasons": reasons,
                "currently_seated": self.joined()}

    async def _check_seat(self) -> bool:
        """Watch for two states the socket alone will not tell us about.

        **Seat loss.** The board can drop our player slot while the WebSocket
        stays open -- observed on a realtime run where two bots kept streaming
        state, believing they were seated, while /state showed conn=false and
        their scores frozen. The broadcast's own `on` flag is the only signal,
        and a downed survivor keeps connected=true, so on:0 unambiguously
        means the seat is gone. Reconnecting re-picks it.

        **Death.** handleMsg_pick treats a pick on an LL-0 slot as a respawn:
        fresh survivor, lifetime score and steps carried over. So reconnecting
        is the respawn mechanism, exactly as it is for the browser client.
        Without this a downed bot sits at 0 MP for the rest of the run and the
        fleet quietly hollows out.

        Returns True if the connection was closed and the caller should stop
        processing this message.
        """
        me = self.obs.players[self.arch]

        if not me.connected:
            self.seats_lost += 1
            self._close_reason = "seat_lost"
            # The stale-slot reap unseats a live player without closing the
            # socket (docs/bot-testing.md "Known issues"). It is a firmware
            # defect, not a harness event, so it is a finding.
            self.findings.add(Finding(
                check="seat_lost", severity="major",
                summary="the board unseated a live player; socket stayed open",
                source=self.oracle.source, repro=self.repro.snapshot(),
                detail={"tick": self.obs.tick, "score": me.score}))
            self.recorder.write("seat_lost", self.arch,
                                {"tick": self.obs.tick, "score": me.score})
            await self.ws.close()
            return True

        if me.ll == 0:
            now = time.monotonic()
            if self._downed_since is None:
                self._downed_since = now
                self.recorder.write("downed", self.arch,
                                    {"day": self.obs.day, "score": me.score,
                                     "steps": me.steps})
            elif now - self._downed_since >= self.respawn_after:
                self.respawns += 1
                self._downed_since = None
                self._close_reason = "respawn"
                self.recorder.write("respawn", self.arch,
                                    {"day": self.obs.day, "score": me.score})
                await self.ws.close()
                return True
        else:
            self._downed_since = None
        return False

    def _digest(self) -> dict:
        me = self.obs.me
        return {"tk": self.obs.tick, "day": self.obs.day, "wp": self.obs.weather,
                "sc": me.score, "sp": me.steps, "ll": me.ll, "mp": me.mp,
                "food": me.food, "water": me.water, "rad": me.rad,
                "q": me.q, "r": me.r, "inv": me.inv, "vm": me.valid_moves,
                # Gear state per tick. Without these a run records that a bot
                # *sent* equip_item but not what it was actually wearing at
                # any moment, so "did the bonus apply" had to be inferred from
                # item_result acks scattered through the rx channel.
                "eq": list(me.equip), "is": me.inv_slots, "llcap": me.ll_cap}

    async def _decide_loop(self) -> None:
        while not self.stop.is_set():
            await asyncio.sleep(self.decide_interval)
            self.oracle.poll()
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
                # A noop sends nothing, so it used to leave no trace at all --
                # which made a fully-stalled fleet look identical to dead
                # decision loops when a run froze. Log the reason when it
                # changes, and heartbeat while it persists, without writing a
                # line every 0.35s for a bot that is legitimately idle.
                now = time.monotonic()
                if (action.why != self._last_noop_why
                        or now - self._last_noop_log >= NOOP_HEARTBEAT_S):
                    self.recorder.write("noop", self._log_arch(),
                                        {"why": action.why,
                                         "repeats": self._noop_streak,
                                         "mp": self.obs.me.mp,
                                         "vm": self.obs.me.valid_moves,
                                         "day": self.obs.day})
                    self._last_noop_log = now
                    self._noop_streak = 0
                self._last_noop_why = action.why
                self._noop_streak += 1
                continue
            self._last_noop_why = None
            self._noop_streak = 0
            self.recorder.write("decide", self._log_arch(),
                                {"kind": action.kind, "why": action.why})
            await self._send(msg)

    def joined(self) -> bool:
        """Seated right now.  Use ever_joined for post-run reporting -- the
        teardown path clears arch, so this reads False after a clean stop."""
        return self.arch >= 0 and self.obs.synced

    def summary(self) -> dict:
        me = self.obs.me
        out = {"arch": self.seated_arch, "label": self.label,
               "policy": self.policy.name,
               "joined": self.ever_joined, "score": me.score, "steps": me.steps,
               "ll": me.ll, "day": self.obs.day, "sent": self.sent,
               "received": self.received, "errors": self.errors,
               "refused_full": self.refused_full,
               "pick_failures": self.pick_failures,
               "seats_lost": self.seats_lost, "respawns": self.respawns,
               "acks": self.acks, "nacks": self.nacks,
               "unanswered": self.unanswered,
               "connection": self.connection_report()}
        out["pts_per_step"] = round(me.score / me.steps, 2) if me.steps else None
        # ContentMax is judged on this rather than score, so it has to reach
        # the summary even when the policy is wrapped for sprint mode.
        getter = getattr(self.policy, "content_score", None)
        if callable(getter):
            out["content"] = getter()
        return out
