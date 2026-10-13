"""Machine Gun Kane, played by real bots against the mock (docs/machine-gun-kane.md).

Seats a few SurvivorPolicy bots (BotClient, exactly as arena.py runs them) and
one staging probe, then uses the mock's `dbg_kane` to put Kane where the bots
are and watches what they do.  Two parts, both off by flag:

  cover   Park the jeep beside a bot standing on open ground, let him
          ask (a forced world tick), give the bots one beat, then force the
          next tick: anyone still in the open is shot at.  The bots are meant
          to be somewhere else by then.  Repeated --trials times.
  story   His five chapters: a camp left at dawn two hexes from the quester
          (1-3), the house in the glass (4, the mock's own placement, staged
          beside the quester only if it is out of reach), and his fire once
          that is read (5).  Real rolls -- nothing is forced -- so a failed
          check means the bot comes back for it.

Mock only: the staging is `dbg_kane` and the call -> burst gap is set by this
script (--gap, the real one is a 15 s world tick).  On hardware Kane drives
and asks on his own, and a long realtime arena run with a contentmax in the
fleet is the test; metrics.py's "-- machine gun kane" block reads it.

    python kane_check.py [host[:port]] [--parts cover,story] [--trials 4]

Every check prints ok/FAIL; the exit code is the number of failures.
"""
import argparse
import asyncio
import json
import random
import sys
import time
from datetime import datetime
from pathlib import Path

import config
import policy as policy_mod
from client import BotClient, RateLimiter, SlotBroker
from findings import FindingLog
from navigate import hex_dist_wrap
from record import Recorder
from wire import ProbeClient

RUNS_DIR = Path(__file__).resolve().parent / "runs"
fails = 0


def check(label, cond, detail=""):
    global fails
    if cond:
        print(f"  ok   {label}")
    else:
        fails += 1
        print(f"  FAIL {label}" + (f"  ({detail})" if detail else ""))


def ring2(q, r):
    """The six hexes two steps from (q, r) along a straight line."""
    return [((q + 2 * dq) % config.MAP_COLS, (r + 2 * dr) % config.MAP_ROWS)
            for dq, dr in zip(config.DQ, config.DR)]


def far_from(bots):
    """Somewhere at least 15 hexes from every bot: where he goes between acts."""
    for _ in range(500):
        q, r = random.randrange(config.MAP_COLS), random.randrange(config.MAP_ROWS)
        if all(hex_dist_wrap(q, r, b.obs.me.q, b.obs.me.r) >= 15 for b in bots):
            return [q, r]
    return [0, 0]


class Stage:
    """The probe's side: dbg_kane, raw (the mock never answers a rid)."""

    def __init__(self, probe):
        self.p = probe
        self.reseats = 0

    async def ensure_seat(self):
        """dbg_kane needs a seated sender, and the probe does not eat: a few
        collapsed mock days and it is dead and unseated, after which the mock
        drops every dbg_kane without a word. Take a seat again if so."""
        me = self.p.obs.me
        if me.connected and me.ll > 0:
            return
        if await self.p.seat() >= 0:
            self.reseats += 1
            await asyncio.sleep(0.5)

    async def kane(self, **kw):
        await self.ensure_seat()
        await self.p.send_raw(json.dumps({"t": "dbg_kane", **kw}, separators=(",", ":")))
        await asyncio.sleep(0.4)

    @property
    def world(self):
        return self.p.obs.world


async def wait_until(pred, timeout, every=1.0, note=None):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if pred():
            return True
        if note:
            note()
        await asyncio.sleep(every)
    return pred()


def stat_sum(bots, key):
    return sum(b.policy.stats.get(key, 0) or 0 for b in bots)


async def part_cover(bots, st: Stage, trials: int, gap: float):
    print("cover")
    # A fresh jeep and an unread arc: a retired one is ignored by the bots
    # (and rightly), but `at` would put him back on the road regardless.
    await st.kane(hold=1, reset=1)
    await asyncio.sleep(1.0)
    asked = shot = 0
    for t in range(trials):
        # A bot out in the open, not in a scene, able to move: the one to ask.
        def pick():
            for b in bots:
                me, here = b.obs.me, b.obs.here()
                if (b.joined() and me.ll > 0 and me.ap > 0 and not b.obs.underground
                        and b.obs.encounter is None and here is not None
                        and config.kane_open_ground(here.terrain)):
                    return b
            return None
        ok = await wait_until(lambda: pick() is not None, 60)
        target = pick()
        if not ok or target is None:
            check(f"trial {t + 1}: a bot on open ground to ask", False, "none in 60 s")
            continue
        me = target.obs.me
        # Right beside it: the mock's terrain runs in one-hex stripes, so a
        # bot that takes one step in the half second before his tick has
        # usually left the open already -- a trial with no question is a
        # non-event, not a dodge.
        kq, kr = config.neighbor(me.q, me.r, t % 6)
        c0, b0 = stat_sum(bots, "kane_calls"), stat_sum(bots, "kane_bursts")
        # Park and ask in one message: given a second's warning the bots walk
        # out of his reach before he has a turn (the danger cost), which is
        # the proactive half and not what this part measures. Pinned, so a
        # mock night does not send him to his fire instead.
        await st.kane(hold=1, pin=1, at=[kq, kr], tick=1)   # he asks
        await asyncio.sleep(gap)                    # the bots' window
        await st.kane(tick=1)                       # the burst, at whoever is still out
        await asyncio.sleep(1.5)
        calls = stat_sum(bots, "kane_calls") - c0
        bursts = stat_sum(bots, "kane_bursts") - b0
        asked += calls
        shot += bursts
        print(f"       trial {t + 1}: jeep at ({kq},{kr}) next to {target.label} "
              f"({me.q},{me.r}) -- asked {calls}, shot at {bursts}")
        await st.kane(at=far_from(bots))            # drive him off
        await asyncio.sleep(2.0)
    check("he asked someone", asked > 0, f"{asked} questions")
    check("nobody asked stayed in the open for the burst", shot == 0,
          f"{shot} of {asked} were shot at")
    return asked, shot


async def part_story(bots, st: Stage, timeout: float):
    print("story")
    questers = [b for b in bots if getattr(b.policy, "kane_shadow", False)] or bots

    def quester():
        live = [b for b in questers if b.joined() and b.obs.me.ll > 0 and not b.obs.underground]
        return max(live, key=lambda b: b.obs.me.ll) if live else None

    def progress():
        w = st.world
        print(f"       ... stage {w.kane_stage} camp=({w.kane_camp_q},{w.kane_camp_r}) "
              f"site=({w.kane_site_q},{w.kane_site_r}) mode={w.kane_mode} "
              f"scenes={stat_sum(bots, 'kane_scenes')}", flush=True)

    await st.kane(hold=1, reset=1)
    await asyncio.sleep(1.0)
    for ch in (1, 2, 3):
        b = quester()
        if b is None:
            check(f"chapter {ch}: a quester to stage it for", False)
            return
        me = b.obs.me
        cq, cr = ring2(me.q, me.r)[ch % 6]
        # The real flow: he sits at a fire there, and at dawn drives on,
        # leaving the camp behind him with the chapter in it.
        await st.kane(at=[cq, cr], mode=config.KANE_CAMP)
        await asyncio.sleep(2.0)
        await st.kane(pin=0)
        left = False
        for _ in range(60):                         # ~2 min of ticks covers a mock night
            await st.kane(tick=1)
            if st.world.kane_camp_q >= 0:
                left = True
                break
            await asyncio.sleep(1.6)
        check(f"chapter {ch}: his camp is left at dawn",
              left and (st.world.kane_camp_q, st.world.kane_camp_r) == (cq, cr),
              f"camp=({st.world.kane_camp_q},{st.world.kane_camp_r}) want ({cq},{cr})")
        await st.kane(at=far_from(bots))
        read = await wait_until(lambda: st.world.kane_stage >= ch, timeout, every=10.0,
                                note=progress)
        check(f"chapter {ch}: a bot walked to the camp and read it", read,
              f"stage {st.world.kane_stage}")
        if not read:
            return
    # Chapter 4: the house in the glass, where the mock put it.
    await asyncio.sleep(1.5)
    w = st.world
    b = quester()
    dist = hex_dist_wrap(w.kane_site_q, w.kane_site_r, b.obs.me.q, b.obs.me.r) if b else 99
    print(f"       the house is at ({w.kane_site_q},{w.kane_site_r}), {dist} hexes from {b.label if b else '-'}")
    if dist > 12 and b is not None:
        me = b.obs.me
        sq, sr = ring2(me.q, me.r)[4]
        print(f"       out of easy reach on the mock: staging it at ({sq},{sr})")
        await st.kane(site=[sq, sr])
    read = await wait_until(lambda: st.world.kane_stage >= 4, timeout, every=10.0, note=progress)
    check("chapter 4: a bot found the house in the glass and read it", read,
          f"stage {st.world.kane_stage}")
    if not read:
        return
    # Chapter 5: his fire, next to the quester.
    b = quester()
    me = b.obs.me
    fq, fr = ring2(me.q, me.r)[1]
    await st.kane(at=[fq, fr], mode=config.KANE_CAMP)
    done = await wait_until(lambda: st.world.kane_ending != 0, timeout, every=10.0, note=progress)
    w = st.world
    check("chapter 5: a bot walked into his firelight and ended it", done,
          f"stage {w.kane_stage} ending {w.kane_ending}")
    if done:
        print(f"       ending: {'the truth' if w.kane_ending == 1 else 'the lie'} -- the jeep is retired")


async def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default="localhost:8765")
    ap.add_argument("--policies", default="contentmax,scoremax,rival")
    ap.add_argument("--parts", default="cover,story")
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--gap", type=float, default=6.0,
                    help="seconds between his question and the burst (a world tick is 15)")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="seconds to wait for each chapter to be read")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args(argv)
    parts = set(args.parts.split(","))

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = RUNS_DIR / f"kane-{stamp}.jsonl"
    rng = random.Random(args.seed)
    random.seed(args.seed)
    with Recorder(path, {"kind": "kane_check", "host": args.host,
                         "policies": args.policies, "parts": sorted(parts)}) as rec:
        findings = FindingLog(rec)
        broker = SlotBroker()
        glimit = RateLimiter(0.12, rng, jitter=0.25)
        bots = []
        for i, name in enumerate(args.policies.split(",")):
            brng = random.Random(args.seed * 1000 + i)
            pol = policy_mod.make(name.strip(), brng)
            # The mock is not the economy (no terrain costs, flat harvests,
            # days that collapse in seconds), and its survivors sit at LL 2-3.
            # kane_min_ll is a judgement about the board's real hazards, so it
            # is relaxed here rather than leaving the story unread.
            pol.kane_min_ll = 1
            # The mock's VACANCY sign is vac_check.py's, and its errand sits
            # ahead of Kane's in the chain.
            pol.vac_quest = False
            bots.append(BotClient(host=args.host, preferred_arch=i,
                                  policy=pol,
                                  recorder=rec, rng=brng,
                                  limiter=RateLimiter(0.30, brng), global_limiter=glimit,
                                  broker=broker, findings=findings))
        tasks = [asyncio.create_task(b.run()) for b in bots]
        probe = ProbeClient(args.host, findings, source="kane_check", recorder=rec,
                            min_interval=0.2)
        st = None
        try:
            seated = await wait_until(lambda: all(b.joined() for b in bots), 30)
            check("bots seated", seated, str([b.label for b in bots]))
            await probe.open()
            slot = await probe.seat()
            check("staging probe seated", slot >= 0)
            if slot < 0 or not seated:
                return
            st = Stage(probe)

            async def rest_probe():
                # The probe never plays, so it must not hold the day open --
                # the mock (like tickGame) starts the next day early only when
                # every seated player rests -- but resting all the time makes
                # the days fly whenever the bots rest too, and the survival
                # grind with them. So it rests when they all have.
                while True:
                    if all(not b.joined() or b.obs.me.resting or b.obs.me.ap <= 0 for b in bots):
                        await probe.send_raw(json.dumps({"t": "act", "a": config.ACT_REST, "ap": 1}))
                    await asyncio.sleep(2.0)
            rester = asyncio.create_task(rest_probe())
            try:
                await asyncio.sleep(3.0)
                if "cover" in parts:
                    await part_cover(bots, st, args.trials, args.gap)
                if "story" in parts:
                    await part_story(bots, st, args.timeout)
            finally:
                rester.cancel()
            if st.reseats:
                print(f"       (the staging probe re-seated {st.reseats}x)")
            print("bots")
            for b in bots:
                c = b.policy.content_score()
                print(f"    {b.label:<28} ll={b.obs.me.ll} asked={c['kane_calls']} "
                      f"dodged={c['kane_dodged']} shot={c['kane_bursts']} "
                      f"ll_lost={c['kane_ll_lost']} scenes={c['kane_scenes']} "
                      f"read={c['kane_chapters']} quest_moves={c['kane_quest_moves']} "
                      f"shadow={c['kane_shadow_moves']}"
                      + (f" ending={c['kane_ending']}" if c.get('kane_ending') else ""))
            bad = [f for f in findings.summary() if f["check"].startswith("kane_")]
            check("no Kane findings from the oracles", not bad, str(bad))
        finally:
            if st is not None:
                try:
                    await st.kane(hold=0)
                except Exception:  # noqa: BLE001 -- best effort on the way out
                    pass
            for b in bots:
                b.stop.set()
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await probe.close()
        print(f"log: {path}")
    print(f"{fails} failure(s)")


if __name__ == "__main__":
    asyncio.run(main())
    sys.exit(min(fails, 100))
