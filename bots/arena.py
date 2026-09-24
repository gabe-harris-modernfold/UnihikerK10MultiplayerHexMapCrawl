"""Arena supervisor -- spins up N bots against one K10 and runs them to a
score target, then resets and does it again.

Reset sequence (order matters):

  1. every bot disconnects, so handleDisconnect() clears p.connected and
     G.connectedCount drops back to 0
  2. one control connection opens -- it needs a free lobby slot, and
     handleConnect() rejects once connectedCount + lobbySize >= MAX_PLAYERS,
     which is exactly why this runs with the bots down rather than as a
     seventh client
  3. eraseslot for each of the 6 slots.  regen alone is not enough: it keeps
     score and steps on connected players ("keeping only name, score, and
     steps"), and the run terminates on score, so a stale 988 would end run 2
     instantly.  eraseslot zeroes score/steps/encCount and wipes the survivor.
  4. regen -- deletes both SD save files, regenerates the map, resets
     day/threat/weather
  5. bots reconnect and pick

Usage:
    python arena.py --host 192.168.4.234 --bots 5 --policies drunk --runs 1
"""
import argparse
import asyncio
import json
import random
import time
from datetime import datetime
from pathlib import Path

import websockets

import policy as policy_mod
from client import BotClient, RateLimiter, SlotBroker
from config import MAX_PLAYERS, ARCHETYPE_NAME
from findings import FindingLog
from policy.base import Action, Policy
from policy.survivor import REST_RETRY_S
from record import Recorder
from telemetry import TelemetryPoller, fetch_state

RUNS_DIR = Path(__file__).parent / "runs"

# How long the whole fleet may be unseated before the run is written off.
# Generous on purpose: the seat-loss bug unseats everyone at once and the
# clients re-pick on their own, so a short fuse here scraps healthy runs.
NO_SEAT_GRACE = 45.0


class SprintPolicy(Policy):
    """Wraps any policy to enable sprint mode.

    tickGame() ends the day immediately when every connected player is
    resting (`connCount > 0 && allResting`), so a board with only bots on it
    can fast-forward: a day collapses from 5 real minutes to the next tick.
    This forces REST the moment a survivor is out of MP and lets the wrapped
    policy play normally the rest of the time.

    Caveat: weatherNextGapMs pins real weather changes to a ~1.1-1.9 real
    minute floor no matter how fast game-days fly, so sprint runs exercise
    the economy and survival loop but barely touch weather.

    REST at depth 1 used to be refused, which meant one survivor underground
    held the whole fleet's day open -- tickGame() needs *every* connected
    player resting. That is no longer true: a bot down a hole can sleep like
    any other, so sprint mode no longer depends on the tunnel policies timing
    their way back to the surface. What a night below costs them now is the
    TUNNEL_REST_LL_PCT bad-air roll (config.py), not the fleet's wall clock.
    """

    def __init__(self, inner: Policy):
        super().__init__(inner.rng)
        self.inner = inner
        self.name = f"sprint:{inner.name}"
        self._rested_day = None
        self._last_rest_sent = 0.0

    def decide(self, obs) -> Action:
        me = obs.me
        # Cooldown, not a once-per-day latch.  Unguarded re-sending produced
        # 2054 dead messages in an early run, but latching per day is worse:
        # a REST decided just before a reconnect never reaches the board while
        # the bot still thinks it rested, leaving it at mp == 0 and awake --
        # and one awake player stops tickGame() ending the day early for the
        # whole fleet.  See SurvivorPolicy.rest_once for the full account.
        if (obs.encounter is None and me.connected and me.ll > 0
                and me.mp <= 0 and not me.resting
                and (self._rested_day != obs.day
                     or time.monotonic() - self._last_rest_sent >= REST_RETRY_S)):
            from config import ACT_REST
            self._rested_day = obs.day
            self._last_rest_sent = time.monotonic()
            return Action("act", a=ACT_REST, why="sprint: out of MP")
        return self.inner.decide(obs)

    def on_event(self, ev):
        self.inner.on_event(ev)

    def on_reply(self, cmd, ok, why):
        self.inner.on_reply(cmd, ok, why)

    def set_pid(self, pid):
        self.pid = pid
        self.inner.set_pid(pid)

    def content_score(self):
        getter = getattr(self.inner, "content_score", None)
        return getter() if callable(getter) else None


async def verify_reset(host: str, recorder, attempts: int = 8,
                       delay: float = 2.0) -> bool:
    """Confirm the reset actually landed, by reading /state rather than by
    trusting the WebSocket round-trip.

    A fresh world is day <= 2 with every slot at score 0 and steps 0.  This is
    the authoritative check: the control socket routinely dies mid-regen (see
    reset_world), so its survival says nothing about whether regen ran.
    """
    for i in range(attempts):
        try:
            st = await asyncio.to_thread(fetch_state, host)
            day = st.get("day", 99)
            players = st.get("players", [])
            dirty = [(p.get("pid"), p.get("score"), p.get("steps")) for p in players
                     if p.get("score") or p.get("steps")]
            if day <= 2 and not dirty:
                recorder.write("reset", -1, {"verified": True, "day": day,
                                             "attempt": i + 1})
                return True
            recorder.write("reset", -1, {"verified": False, "day": day,
                                         "dirty": dirty, "attempt": i + 1})
        except Exception as e:
            recorder.write("reset", -1, {"probe_err": f"{type(e).__name__}: {e}",
                                         "attempt": i + 1})
        await asyncio.sleep(delay)
    return False


async def reset_world(host: str, recorder, timeout: float = 20.0) -> bool:
    """Wipe every slot and regenerate the map.  Destructive by design --
    deletes both SD save files.

    Verified against /state, not against the socket.  regen runs generateMap()
    and wInit() while holding G.mutex, which blocks the AsyncTCP task long
    enough that the control connection is routinely dropped with
    ConnectionClosedError *after* the command has been accepted.  Treating
    that as a failure made a successful reset look like a failed one -- and
    the run then started on a dirty board, which against a score target means
    it can finish before it begins.
    """
    url = f"ws://{host}/ws"
    sent_regen = False
    try:
        async with websockets.connect(url, open_timeout=timeout,
                                      ping_interval=None, max_size=2 ** 20) as ws:
            # Wait for the lobby greeting so we know we were not rejected.
            try:
                async with asyncio.timeout(timeout):
                    async for raw in ws:
                        m = json.loads(raw)
                        if m.get("t") == "full":
                            recorder.write("reset", -1, {"err": "board full"})
                            return False
                        if m.get("t") == "lobby":
                            break
            except (asyncio.TimeoutError, json.JSONDecodeError):
                pass
            for arch in range(MAX_PLAYERS):
                await ws.send(json.dumps({"t": "eraseslot", "arch": arch}))
                await asyncio.sleep(0.25)
            await asyncio.sleep(0.5)
            await ws.send(json.dumps({"t": "regen"}))
            sent_regen = True
            await asyncio.sleep(3.0)
    except Exception as e:
        if not sent_regen:
            recorder.write("reset", -1, {"err": f"{type(e).__name__}: {e}"})
            return False
        recorder.write("reset", -1,
                       {"note": "socket dropped after regen was sent (expected)",
                        "err": f"{type(e).__name__}"})
    return await verify_reset(host, recorder)


async def run_once(args, run_idx: int, seed: int) -> dict:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = RUNS_DIR / f"run-{stamp}-{run_idx:02d}.jsonl"
    meta = {"run": run_idx, "seed": seed, "host": args.host,
            "bots": args.bots, "policies": args.policies, "mode": args.mode,
            "target": args.target, "shelter": bool(getattr(args, "shelter", False)),
            "started": datetime.now().isoformat(timespec="seconds")}

    rng = random.Random(seed)
    with Recorder(path, meta) as rec:
        print(f"\n=== run {run_idx}  seed={seed}  -> {path.name}")

        if not args.no_reset:
            print("  reset: eraseslot x6 + regen ...", end="", flush=True)
            ok = await reset_world(args.host, rec)
            print(" verified" if ok else " FAILED")
            if not ok:
                # Against a score target a dirty board can end the run
                # instantly, so this is not something to continue past.
                print("  aborting: board not clean (use --no-reset to override)")
                rec.write("run", -1, {"reason": "reset_failed"})
                return {"run": run_idx, "seed": seed, "reason": "reset_failed",
                        "bots": [], "board": {}, "log": str(path)}

        global_limiter = RateLimiter(args.global_interval, rng, jitter=0.25)
        broker = SlotBroker()
        # One log for the whole run: every bot's WireOracle and the /state
        # poller report into it, so a defect two bots hit is counted once.
        findings = FindingLog(rec)
        names = args.policies.split(",")
        bots = []
        for i in range(args.bots):
            pname = names[i % len(names)].strip()
            bot_rng = random.Random(seed * 1000 + i)
            pol = policy_mod.make(pname, bot_rng)
            # Set on the instance, not the class, so a mixed fleet is possible
            # and so the flag does not leak between runs in a --runs loop.
            if getattr(args, "shelter", False) and hasattr(pol, "shelter_when_exposed"):
                pol.shelter_when_exposed = True
            if args.mode == "sprint":
                pol = SprintPolicy(pol)
            bots.append(BotClient(
                host=args.host, preferred_arch=i, policy=pol, recorder=rec,
                rng=bot_rng, limiter=RateLimiter(args.min_interval, bot_rng),
                global_limiter=global_limiter, broker=broker,
                decide_interval=args.decide_interval, findings=findings,
            ))

        tele = TelemetryPoller(args.host, rec, interval=args.telemetry_interval,
                               findings=findings)
        tasks = [asyncio.create_task(b.run()) for b in bots]
        tasks.append(asyncio.create_task(tele.run()))

        reason, t_start = "unknown", time.monotonic()
        deadline = t_start + args.max_minutes * 60
        # "Nobody is seated" has two completely different meanings and this
        # is the only place that can tell them apart.  A refused pick is
        # silent, so a fleet that never got in looks exactly like a quiet
        # game -- but the unflashed seat-loss bug also unseats the whole
        # fleet at once mid-run, and those bots re-pick within seconds.
        # Timing from the last moment anyone was seated covers both: a fleet
        # that never joined still bails at NO_SEAT_GRACE, while a live run
        # gets the same grace to recover instead of being scrapped.  Before
        # this, 2 of 4 runs in a batch died at day 1 and day 6.
        last_seated = t_start
        try:
            while True:
                await asyncio.sleep(2.0)
                joined = [b for b in bots if b.joined()]
                scores = [b.obs.me.score for b in joined]
                alive = [b for b in joined if b.obs.me.ll > 0]
                now = time.monotonic()
                elapsed = now - t_start
                if joined:
                    last_seated = now
                # --target 0 runs on the clock alone (soak.py).
                if args.target and max(scores, default=0) >= args.target:
                    reason = "target"
                    break
                if now > deadline:
                    reason = "timeout"
                    break
                if now - last_seated > NO_SEAT_GRACE:
                    reason = "no_join" if last_seated == t_start else "seats_lost"
                    break
                if joined and not alive and elapsed > 60:
                    reason = "all_downed"
                    break
                if int(elapsed) % 30 < 2:
                    day = joined[0].obs.day if joined else 0
                    print(f"  t={int(elapsed):4}s day={day:3} seated={len(joined)}/{len(bots)} "
                          f"scores={scores} tick={tele.worst_tick} heap={tele.min_heap}")
        except KeyboardInterrupt:
            reason = "interrupted"
        finally:
            for b in bots:
                b.stop.set()
            tele.stop.set()
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

        summary = {
            "run": run_idx, "seed": seed, "reason": reason,
            "elapsed_s": round(time.monotonic() - t_start, 1),
            "day": bots[0].obs.day if bots else 0,
            "bots": [b.summary() for b in bots],
            "board": tele.summary(),
            "findings": findings.summary(),
            "log": str(path),
        }
        findings.flush()
        rec.write("run", -1, summary)
        print(f"  end: {reason} after {summary['elapsed_s']}s, day {summary['day']}")
        for b in summary["bots"]:
            seat = "" if b["joined"] else f" NOT SEATED (pickfail={b['pick_failures']})"
            pps = b.get("pts_per_step")
            print(f"    {b['label']:<30} score={b['score']:<5} steps={b['steps']:<4} "
                  f"pts/step={pps if pps is not None else '-':<5} ll={b['ll']} "
                  f"tx={b['sent']} err={b['errors']}{seat}")
            if b.get("acks") or b.get("nacks"):
                # Refusals by command, most common reason first. A policy
                # that keeps asking for something the board keeps refusing
                # shows up here long before it shows up in the score.
                parts = []
                for cmd, whys in sorted(b["nacks"].items()):
                    top = sorted(whys.items(), key=lambda kv: -kv[1])
                    parts.append(f"{cmd}(" + ",".join(f"{w}:{n}" for w, n in top) + ")")
                print(f"       replies: ack={b['acks']} "
                      f"nack={sum(sum(w.values()) for w in b['nacks'].values())} "
                      f"unanswered={b['unanswered']} {' '.join(parts)}")
            c = b.get("content")
            if c:
                print(f"       content: opened={c['encounters_opened']} "
                      f"banked={c['encounters_banked']} aborted={c['encounters_aborted']} "
                      f"nodes={c['nodes_seen']} rolls={c['rolls_won']}/{c['rolls']} "
                      f"recipes={c['recipes']} downed={c['downed']}")
            camp = (c or {}).get("camp")
            if camp:
                # The Sentinel's camp: did it get one, cover it, and sleep in it?
                print(f"       camp: at={camp['at']} sites={camp['camps']} "
                      f"built={camp['shelters']} upgraded={camp['upgrades']} "
                      f"rests={camp['rests']} (in camp {camp['rests_in_camp']})")
            # Only the tunnel policies carry these. mp_per_surface_hex is the
            # headline: underground MP spent per surface hex actually crossed,
            # against a surface average MC of ~1.6.
            if c and "descents" in c:
                saving = c.get("mp_per_surface_hex")
                print(f"       tunnels: dives={c['descents']}/{c['ascents']} "
                      f"steps={c['tunnel_steps']} below/above="
                      f"{c['tunnel_steps']}/{c['surface_steps']} "
                      f"shafts={c['shafts_used']}/{c['hatches_known']} "
                      f"crossed={c['transit_hexes']}hx "
                      f"mp/hex={saving if saving is not None else '-'} "
                      f"dawnsBelow={c['dawns_below']} "
                      f"(bunker={c['bunker_dawns']} vent={c['vent_dawns']})")
                # dawnsBelow is now a bad-air exposure count, not an LL one:
                # the split by hatch type only prices the climb out (1 MP
                # against 2) since dawnUpkeep stopped reading the surface hex.
        bd = summary["board"]
        print(f"    board: worst maxTickMs={bd['worst_maxTickMs']} "
              f"minHeap={bd['min_heap']} pollFail={bd['poll_failures']} "
              f"evtDrops={bd['evt_drops'] if bd['evt_drops'] is not None else '-'}")
        for rb in bd["reboots"]:
            cr = rb.get("crash") or {}
            print(f"    board REBOOTED mid-run: reset={rb.get('reset')} "
                  f"after {rb['uptimeMs_before'] / 1000:.0f}s up"
                  + (f", crash task={cr.get('task')} pc={cr.get('pc')}" if cr else ""))
        findings.print_summary()
        return summary


async def main_async(args):
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    try:
        st = await asyncio.to_thread(fetch_state, args.host)
        print(f"board {args.host}: day {st.get('day')} connected {st.get('connected')} "
              f"maxTickMs {st.get('mem', {}).get('maxTickMs')} "
              f"minHeap {st.get('mem', {}).get('minHeap')}")
    except Exception as e:
        raise SystemExit(f"cannot reach board at {args.host}: {e}")

    summaries = []
    for i in range(1, args.runs + 1):
        summaries.append(await run_once(args, i, args.seed + i))
        if i < args.runs:
            await asyncio.sleep(5.0)

    out = RUNS_DIR / f"summary-{datetime.now().strftime('%Y%m%d-%H%M%S')}.json"
    out.write_text(json.dumps(summaries, indent=2, default=str), encoding="utf-8")
    print(f"\n{len(summaries)} run(s) -> {out}")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.4.234",
                   help="K10 address, or localhost:8765 for the mock")
    p.add_argument("--bots", type=int, default=5,
                   help="1-6; 5 leaves a slot free so you can watch in a browser")
    p.add_argument("--policies", default="drunk",
                   help="comma-separated, cycled across slots (e.g. drunk,drunk)")
    p.add_argument("--target", type=int, default=1000,
                   help="score that ends a run; 0 = run on --max-minutes alone")
    p.add_argument("--runs", type=int, default=1)
    p.add_argument("--mode", choices=("sprint", "realtime"), default="sprint")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--min-interval", type=float, default=0.30,
                   dest="min_interval", help="per-bot minimum seconds between sends")
    p.add_argument("--global-interval", type=float, default=0.12,
                   dest="global_interval",
                   help="arena-wide minimum seconds between any two sends")
    p.add_argument("--decide-interval", type=float, default=0.35,
                   dest="decide_interval")
    p.add_argument("--telemetry-interval", type=float, default=5.0,
                   dest="telemetry_interval")
    p.add_argument("--max-minutes", type=float, default=30.0, dest="max_minutes",
                   help="safety timeout per run")
    p.add_argument("--no-reset", action="store_true",
                   help="skip eraseslot+regen (keeps the current world/save)")
    p.add_argument("--shelter", action="store_true",
                   help="let the survivor policies build a basic shelter before "
                        "sleeping on exposed ground (1 scrap, 1 MP). The other "
                        "arm of the exposure experiment -- see death_causes in "
                        "metrics.py")
    args = p.parse_args(argv)
    if not 1 <= args.bots <= MAX_PLAYERS:
        p.error(f"--bots must be 1..{MAX_PLAYERS}")
    if args.mode == "sprint" and "sentinel" in args.policies.split(","):
        # SprintPolicy forces REST the moment MP runs out, which collapses the
        # day the Sentinel exists to hold open.
        p.error("sentinel holds days open and rests only late in each one; "
                "run it with --mode realtime (or use soak.py)")
    return args


if __name__ == "__main__":
    a = parse_args()
    try:
        asyncio.run(main_async(a))
    except KeyboardInterrupt:
        print("\ninterrupted")
