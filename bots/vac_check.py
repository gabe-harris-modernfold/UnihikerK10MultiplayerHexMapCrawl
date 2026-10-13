"""VACANCY, played by real bots against the mock (docs/vacancy.md).

Seats SurvivorPolicy bots (BotClient, exactly as arena.py runs them) and one
staging probe, then uses the mock's `dbg_vac` to put the sign where the bots
are and watches what they do.  Two parts, both off by flag:

  story   A fresh sign over a hex two steps from the quester (made a
          Settlement for the occasion).  Once a bot reads the town (part 1),
          the Front Desk and Housekeeping are staged two hexes from that bot;
          it has to bank both halves (parts 2 and 3: g == 3), walk back under
          the sign and check the town out (part 4) -- real rolls throughout,
          nothing forced, so a failed check means it comes back for it.
          Passes when the sign ends saved (st 2) with g == 3 and neither half
          is left in anybody's pack.  The dice and the mock's grind for food
          and water can run the eight nights out (Check-Out's last door is
          p ~0.6 and costs 2 LL), so a town that falls is tried again -- a
          fresh sign and fresh survivors -- up to --attempts times.  A sign
          still up when a step times out is not retried: that is a bug.
  fall    A sign counting with one room left and the dawn's check-in run at
          once: `fallen` has to arrive, the sign go out (st 3), and the town's
          hex become Broken Urban (4) -- on the bots' maps, from the event,
          and on the board's own, read back off a fresh vis disk.

Mock only: the staging is `dbg_vac`, and Machine Gun Kane is held still for
the duration (`dbg_kane hold`) so his questions do not pull the bots off the
errand.  The mock runs a day only while somebody is connected and ends it
the moment everyone rests, so the probe rests only once every bot has --
the eight nights are real nights here, a few seconds each, which is why
the bots' retry hold on a part's hex is cut (--retry) along with their
min-LL (--min-ll).  A staged hex where a booby trap goes off is moved: the
trap pre-empts the scene on every arrival (see Sites).  On hardware the
sign is over a random town and a long realtime arena with a quester or a
contentmax in the fleet is the test; metrics.py's "-- vacancy" block reads
it.

    python vac_check.py [host[:port]] [--parts story,fall] [--policies quester]

Every check prints ok/FAIL; the exit code is the number of failures.  One
quester is the default on purpose: with two survivors chasing it, the
register and the key can end up in different packs, and nothing (in the
game or the bots) brings them together again -- Check-Out never opens.
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
from navigate import step_cost
from record import Recorder
from wire import ProbeClient

RUNS_DIR = Path(__file__).resolve().parent / "runs"
REG, KEY = config.VAC_ITEM_REGISTER, config.VAC_ITEM_KEY
# The mock's own surface step table (server.js DIR_DELTA): direction 2 is
# (0, -1) there and 5 is (0, +1), so one undoes the other.  The fall part
# stages the town under the probe, steps off and back on, and so has the
# town at the centre of a vis disk whatever the weather does to the radius.
MOCK_NORTH, MOCK_SOUTH = 2, 5
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


def held(me, iid) -> int:
    return sum(n for t, n in zip(me.inv_type, me.inv_qty) if t == iid and n > 0)


def near(b, avoid=(), k=0):
    """A hex two steps from bot `b` it can walk onto: known, passable, not a
    hatch (stepping on one is the descent), not a Settlement; `k` rotates
    the ring so two sites do not land on the same side."""
    me = b.obs.me
    ring = ring2(me.q, me.r)
    ring = ring[k % 6:] + ring[:k % 6]
    for q, r in ring:
        cell = b.obs.map[(q, r)]
        if (q, r) in avoid or cell is None:
            continue
        if (step_cost(cell) is None or config.is_hatch_terrain(cell.terrain)
                or cell.terrain == config.TERR_SETTLEMENT or cell.trap):
            continue
        return (q, r)
    return next((qr for qr in ring if qr not in avoid), ring[0])


class Stage:
    """The probe's side: dbg_vac and dbg_kane, raw (the mock never answers
    a rid)."""

    def __init__(self, probe):
        self.p = probe
        self.reseats = 0
        self._lock = asyncio.Lock()

    async def ensure_seat(self):
        """dbg_* needs a seated sender, and the probe does not eat: a few
        collapsed mock days and it is dead and unseated, after which the mock
        drops every dbg_vac without a word.  Take a seat again if so.  Also
        what keeps the mock's clock running: with nobody seated it runs no
        days and broadcasts nothing, and a bot that died waits for an `s`
        to notice its seat is gone -- so a dead probe and a dead bot freeze
        the whole check."""
        async with self._lock:
            me = self.p.obs.me
            if me.connected and me.ll > 0:
                return
            if await self.p.seat() >= 0:
                self.reseats += 1
                await asyncio.sleep(0.5)

    async def send(self, t, **kw):
        await self.ensure_seat()
        await self.p.send_raw(json.dumps({"t": t, **kw}, separators=(",", ":")))
        await asyncio.sleep(0.4)

    async def vac(self, **kw):
        await self.send("dbg_vac", **kw)

    @property
    def world(self):
        return self.p.obs.world


async def wait_until(pred, timeout, every=1.0, note=None, note_every=10.0, tick=None):
    t0 = time.monotonic()
    last = t0
    while time.monotonic() - t0 < timeout:
        if pred():
            return True
        if tick:
            await tick()
        if note and time.monotonic() - last >= note_every:
            last = time.monotonic()
            note()
        await asyncio.sleep(every)
    return pred()


class Sites:
    """Where the story's hexes were staged, and moving one that turns out to
    sit on a booby trap.  trapOnArrival runs before vacOnArrival, so an armed
    trap under a site goes off instead of the scene on every arrival until
    somebody works it or springs it -- a placement problem of its own
    (vacSiteOk never asks about traps; docs/bot-testing.md, "VACANCY"), and
    not what this script measures, so the site is moved and the move is
    reported."""

    def __init__(self, st: "Stage", bots):
        self.st, self.bots = st, bots
        self.at = {}                # "town" / "desk" / "keys" -> (q, r)
        self.moved = []
        self._seen = 0

    async def stage(self, what, qr):
        self.at[what] = qr
        await self.st.vac(**{what: [qr[0], qr[1]]})

    async def sweep(self):
        rec = self.st.p.received
        new, self._seen = rec[self._seen:], len(rec)
        for m in new:
            if m.get("k") != "enc_start" or not m.get("trap") or m.get("dp"):
                continue
            qr = (m.get("q"), m.get("r"))
            for what, at in list(self.at.items()):
                if at != qr:
                    continue
                b = next((x for x in self.bots if x.obs.pid == m.get("pid")), self.bots[0])
                await self.st.ensure_seat()
                nqr = near(b, avoid=set(self.at.values()), k=len(self.moved) + 1)
                print(f"       a booby trap went off on the staged {what} {qr}: moving it to {nqr}")
                self.moved.append((what, qr, nqr))
                await self.stage(what, nqr)


def describe(st: Stage, bots):
    w = st.world
    out = (f"st={w.vac_stage} rm={w.vac_rooms} g={w.vac_got} town=({w.vac_q},{w.vac_r}) "
           f"desk=({w.vac_desk_q},{w.vac_desk_r}) keys=({w.vac_keys_q},{w.vac_keys_r})")
    for b in bots:
        me = b.obs.me
        out += (f" | {b.policy.name}@({me.q},{me.r}) ll={me.ll} ap={me.ap} "
                f"reg={held(me, REG)} key={held(me, KEY)} scenes={b.policy.stats['vac_scenes']}")
    return out


async def story_attempt(fleet, st: Stage, timeout: float) -> dict:
    """One sign, staged beside a seeker, played to its end.  Returns what
    happened at each step; part_story judges it."""
    bots = fleet.bots
    res = {"staged": False, "read": False, "banked": False, "holder": None,
           "saved": False, "g": 0, "left": [], "saver": [], "credited": False,
           "rooms": None, "moved": 0, "why": ""}
    seekers = [b for b in bots if b.policy.vac_quest] or bots

    def seeker():
        live = [b for b in seekers if b.joined() and b.obs.me.ll > 0 and not b.obs.underground]
        return max(live, key=lambda b: b.obs.me.ll) if live else None

    def progress():
        print(f"       ... {describe(st, bots)}", flush=True)

    await wait_until(lambda: seeker() is not None, 30)
    # Re-seat a dead probe first: on the mock a pick waits out its whole
    # reply timeout (no ack), and a position read before that is stale by
    # the time the sign goes up.
    await st.ensure_seat()
    b = seeker()
    if b is None:
        res["why"] = "no seeker to stage the sign for"
        return res
    sites = Sites(st, bots)
    sites._seen = len(st.p.received)
    tq, tr = near(b)
    await st.vac(reset=1)
    await sites.stage("town", (tq, tr))
    res["staged"] = await wait_until(lambda: (st.world.vac_q, st.world.vac_r) == (tq, tr)
                                     and st.world.vac_stage == config.VAC_ST_SIGN, 10)
    t0 = time.monotonic()
    await wait_until(lambda: st.world.vac_stage >= config.VAC_ST_OPEN, timeout,
                     note=progress, tick=sites.sweep)
    res["read"] = st.world.vac_stage == config.VAC_ST_OPEN
    if not res["read"]:
        res["why"] = f"part 1 unread: st {st.world.vac_stage} after {time.monotonic() - t0:.0f} s"
        return res
    reader = next((b for b in bots if 1 in b.policy.stats["vac_parts"]), None) or seeker()
    w = st.world
    print(f"       read by {reader.label} in {time.monotonic() - t0:.0f} s, {w.vac_rooms} rooms; "
          f"the board put the desk at ({w.vac_desk_q},{w.vac_desk_r}) and the corridor at "
          f"({w.vac_keys_q},{w.vac_keys_r})")
    town = (w.vac_q, w.vac_r)
    await st.ensure_seat()
    dq, dr = near(reader, avoid={town})
    kq, kr = near(reader, avoid={town, (dq, dr)}, k=3)
    print(f"       staging them beside {reader.label}: desk ({dq},{dr}), corridor ({kq},{kr})")
    await sites.stage("desk", (dq, dr))
    await sites.stage("keys", (kq, kr))
    t0 = time.monotonic()
    await wait_until(lambda: st.world.vac_got == 3 or not st.world.vac_live,
                     2 * timeout, note=progress, tick=sites.sweep)
    res["g"] = st.world.vac_got
    res["banked"] = st.world.vac_got == 3
    if not res["banked"]:
        res["why"] = (f"halves not both banked: g {st.world.vac_got} st {st.world.vac_stage} "
                      f"after {time.monotonic() - t0:.0f} s")
        return res
    print(f"       both banked in {time.monotonic() - t0:.0f} s, {st.world.vac_rooms} rooms left")
    if st.world.vac_live:
        # Still counting: somebody must be holding both, or Check-Out never opens.
        holders = [b for b in bots if held(b.obs.me, REG) and held(b.obs.me, KEY)]
        res["holder"] = bool(holders)
        if not holders:
            res["why"] = "nobody carries both halves: " + str(
                [(b.label, held(b.obs.me, REG), held(b.obs.me, KEY)) for b in bots])
            return res
        t0 = time.monotonic()
        await wait_until(lambda: st.world.vac_stage != config.VAC_ST_OPEN, 2 * timeout,
                         note=progress, tick=sites.sweep)
        print(f"       ... and {time.monotonic() - t0:.0f} s more to the end")
    else:
        res["holder"] = True                    # it checked out before we looked
    w = st.world
    res["saved"], res["rooms"], res["g"] = w.vac_stage == config.VAC_ST_SAVED, w.vac_rooms, w.vac_got
    if not res["saved"]:
        res["why"] = f"not checked out: st {w.vac_stage} rm {w.vac_rooms}"
    await asyncio.sleep(1.5)
    res["left"] = [(b.label, held(b.obs.me, REG), held(b.obs.me, KEY)) for b in bots
                   if held(b.obs.me, REG) or held(b.obs.me, KEY)]
    res["saver"] = [b.label for b in bots if 4 in b.policy.stats["vac_parts"]]
    res["credited"] = bool(res["saver"]) and all(
        b.policy.stats["vac_scenes"] for b in bots if 4 in b.policy.stats["vac_parts"])
    res["moved"] = len(sites.moved)
    return res


async def part_story(fleet, st: Stage, timeout: float, attempts: int):
    """The story with real rolls, up to `attempts` times: on the mock a
    failed last roll at Check-Out, with the mock's grind for food and water
    on top, can run the eight nights out.  Each retry is a fresh sign and a
    fresh fleet (the mock forgets a survivor that disconnects), so no half
    is carried over from the attempt before."""
    print("story")
    res = {}
    for n in range(1, attempts + 1):
        if n > 1:
            print(f"       attempt {n - 1}: {res['why']} -- again, with a fresh sign and "
                  f"fresh survivors")
            await fleet.respawn()
        res = await story_attempt(fleet, st, timeout)
        if res["saved"] or st.world.vac_stage != config.VAC_ST_FALLEN:
            break               # done -- or stuck with the sign still up: worse than the dice
    check("a fresh sign two hexes from the seeker", res["staged"], res["why"])
    check("part 1: a bot walked under the sign and read the town (the count starts)",
          res["read"], res["why"])
    check("parts 2 and 3: the register and the key banked (g == 3)", res["banked"], res["why"])
    check("one survivor carries both halves", bool(res["holder"]), res["why"])
    check(f"part 4: it walked back under the sign and checked the town out (st 2), "
          f"attempt {n} of {attempts}", res["saved"], res["why"])
    check("the sign says both halves were banked (g == 3)", res["g"] == 3, f"g {res['g']}")
    check("both halves left the pack (vac_out)", res["saved"] and not res["left"], str(res["left"]))
    check("the board credited the save to a bot that played Check-Out", res["credited"],
          str(res["saver"]))
    if res["saved"]:
        print(f"       saved by {res['saver']} with {res['rooms']} rooms left on attempt {n}"
              + (f"; {res['moved']} staged site(s) moved off a booby trap" if res["moved"] else ""))


async def part_fall(bots, st: Stage):
    print("fall")
    p = st.p

    def count(pred):
        return sum(1 for m in p.received if pred(m))

    for _try in range(3):
        await st.ensure_seat()
        me_id = p.arch
        n_left = count(lambda m: m.get("k") == "left" and m.get("pid") == me_id)
        # Under the probe's own feet, server-side (`town:1`): the probe's idea
        # of where it stands can be stale on the mock after a re-seat (its
        # sync lists seated players only, which state.py reads by index).  One
        # message: reset, town, the count, one room, the dawn -- the only
        # world object anyone sees after it is the fallen sign.
        n0 = count(lambda m: m.get("k") == "vac" and m.get("w") == "fallen")
        await st.vac(reset=1, town=1, stage=1, rooms=1, dawn=1)
        got = await wait_until(lambda: count(lambda m: m.get("k") == "vac"
                                             and m.get("w") == "fallen") > n0, 10, every=0.5)
        ev = next((m for m in reversed(p.received) if m.get("k") == "vac"
                   and m.get("w") == "fallen"), {})
        tq, tr = ev.get("q", -1), ev.get("r", -1)
        if not got:
            break
        await asyncio.sleep(1.0)
        w = st.world
        sign = (w.vac_stage, w.vac_rooms, w.vac_desk_q, w.vac_keys_q)
        maps = [(b.label, b.obs.map[(tq, tr)]) for b in bots] + [("probe", p.obs.map[(tq, tr)])]
        known = [(lbl, c.terrain) for lbl, c in maps if c is not None]
        # The board's own word: forget the hex, step off it and back on (the
        # mock's own north and south), and read the vis disk of the return --
        # its centre is the town whatever the weather does to the radius.
        n_trap = count(lambda m: m.get("k") == "enc_start" and m.get("trap"))
        p.obs.map.grid[tr][tq] = None
        await p.send_raw(json.dumps({"t": "m", "d": MOCK_NORTH}))
        await asyncio.sleep(0.6)
        vis = asyncio.create_task(p.wait_for(lambda m: m.get("t") == "vis"
                                             and (m.get("q"), m.get("r")) == (tq, tr), 6.0))
        await p.send_raw(json.dumps({"t": "m", "d": MOCK_SOUTH}))
        await vis
        if count(lambda m: m.get("k") == "left" and m.get("pid") == me_id) == n_left:
            break
        print("       (the probe starved between the staging and the read-back: again)")
    check("`fallen` arrives for the staged town", got and tq >= 0, f"{ev}")
    if not got:
        return
    check("the sign is out: st 3, no rooms, no sites",
          sign[0] == config.VAC_ST_FALLEN and sign[1] == 0 and sign[2] < 0 and sign[3] < 0,
          f"st/rm/desk/keys {sign}")
    check("every map that knows the hex has it as Broken Urban (from the event)",
          bool(known) and all(t == config.VAC_FALLEN_TERRAIN for _l, t in known), str(known))
    if count(lambda m: m.get("k") == "enc_start" and m.get("trap")) > n_trap:
        print("       (a booby trap caught the probe stepping off: the vis read-back is skipped)")
        return
    cell = p.obs.map[(tq, tr)]
    check("the board's own vis disk says Broken Urban (4) at the town",
          cell is not None and cell.terrain == config.VAC_FALLEN_TERRAIN,
          f"{cell.terrain if cell is not None else 'not revealed'}")


class Fleet:
    """The seated bots, as arena.py runs them -- and a fresh set on demand:
    the mock deletes a survivor whose socket closes, so a re-seat is a new
    survivor with an empty pack and a policy with an empty journal."""

    def __init__(self, args, rec, findings):
        self.args, self.rec, self.findings = args, rec, findings
        self.glimit = RateLimiter(0.12, random.Random(args.seed), jitter=0.25)
        self.bots, self.tasks, self.gen = [], [], 0

    def spawn(self):
        broker = SlotBroker()
        bots = []
        for i, name in enumerate(self.args.policies.split(",")):
            brng = random.Random(self.args.seed * 1000 + 100 * self.gen + i)
            pol = policy_mod.make(name.strip(), brng)
            # The mock is not the economy (no terrain costs, flat harvests,
            # days that collapse in seconds), and its survivors sit at LL 2-3.
            # vac_min_ll is a judgement about the board's real hazards, so it
            # is relaxed here rather than leaving the sign unread; and the
            # retry hold is measured against the board's five-minute days.
            pol.vac_min_ll = self.args.min_ll
            pol.vac_retry_s = self.args.retry
            bots.append(BotClient(host=self.args.host, preferred_arch=i, policy=pol,
                                  recorder=self.rec, rng=brng,
                                  limiter=RateLimiter(0.30, brng), global_limiter=self.glimit,
                                  broker=broker, findings=self.findings))
        self.gen += 1
        self.bots[:] = bots
        self.tasks = [asyncio.create_task(b.run()) for b in bots]

    async def stop(self):
        for b in self.bots:
            b.stop.set()
        for t in self.tasks:
            t.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)

    async def respawn(self):
        await self.stop()
        await asyncio.sleep(1.0)
        self.spawn()
        await wait_until(lambda: all(b.joined() for b in self.bots), 30)
        await asyncio.sleep(2.0)

    def report(self):
        for b in self.bots:
            c = b.policy.content_score()
            print(f"    {b.label:<28} ll={b.obs.me.ll} scenes={c['vac_scenes']} "
                  f"parts={c['vac_parts']} quest_moves={c['vac_quest_moves']} "
                  f"hums={c['vac_hums']} outcome={c['vac_outcome']} "
                  f"downed={c['downed']} quest_steps={c['quest_steps']}")


async def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default="localhost:8765")
    ap.add_argument("--policies", default="quester")
    ap.add_argument("--parts", default="story,fall")
    ap.add_argument("--timeout", type=float, default=240.0,
                    help="seconds to wait for each step of the story")
    ap.add_argument("--min-ll", type=int, default=1,
                    help="vac_min_ll for the bots: the mock's survivors sit at LL 2-3")
    ap.add_argument("--retry", type=float, default=4.0,
                    help="vac_retry_s for the bots: 20 s is ~7%% of a day on the board, "
                         "but several of the mock's collapsed days")
    ap.add_argument("--attempts", type=int, default=4,
                    help="story attempts (a fresh sign and fresh survivors each) before "
                         "the dice are blamed")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args(argv)
    parts = set(args.parts.split(","))

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = RUNS_DIR / f"vac-{stamp}.jsonl"
    random.seed(args.seed)
    with Recorder(path, {"kind": "vac_check", "host": args.host,
                         "policies": args.policies, "parts": sorted(parts)}) as rec:
        findings = FindingLog(rec)
        fleet = Fleet(args, rec, findings)
        fleet.spawn()
        bots = fleet.bots                       # replaced in place by a respawn
        probe = ProbeClient(args.host, findings, source="vac_check", recorder=rec,
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
            await st.send("dbg_kane", hold=1)      # he stays where he is until the end

            async def rest_probe():
                # The probe never plays, so it must not hold the day open --
                # the mock (like tickGame) starts the next day early only when
                # every seated player rests -- nor end it under the bots. So
                # it rests when they all have.
                while True:
                    await st.ensure_seat()          # see Stage.ensure_seat
                    if all(not b.joined() or b.obs.me.resting or b.obs.me.ap <= 0 for b in bots):
                        await probe.send_raw(json.dumps({"t": "act", "a": config.ACT_REST, "ap": 1}))
                    await asyncio.sleep(2.0)
            rester = asyncio.create_task(rest_probe())
            try:
                await asyncio.sleep(3.0)
                if "story" in parts:
                    await part_story(fleet, st, args.timeout, args.attempts)
                if "fall" in parts:
                    await part_fall(bots, st)
            finally:
                rester.cancel()
            if st.reseats:
                print(f"       (the staging probe re-seated {st.reseats}x)")
            print("bots (the last fleet)")
            fleet.report()
            bad = [f for f in findings.summary() if f["check"].startswith("vac_")]
            check("no VACANCY findings from the oracles", not bad, str(bad))
        finally:
            if st is not None:
                try:
                    await st.send("dbg_kane", hold=0)
                except Exception:  # noqa: BLE001 -- best effort on the way out
                    pass
            await fleet.stop()
            await probe.close()
        print(f"log: {path}")
    print(f"{fails} failure(s)")


if __name__ == "__main__":
    asyncio.run(main())
    sys.exit(min(fails, 100))
