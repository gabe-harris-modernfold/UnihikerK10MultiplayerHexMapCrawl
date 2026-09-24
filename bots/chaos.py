"""Chaos scenarios: the things real players do that no policy ever does.

Phones lose signal mid-encounter.  Two people tap the same survivor at once.
A tab gets backgrounded and stops reading its socket.  Someone leaves while
their trade offer is still on the table.  Each scenario here does one of
those on purpose and checks what the board promised about it:

    reconnect_storm    N connect/drop cycles, half of them abrupt, then:
                       /state seat count still adds up, and a seat can be taken
    seat_race          two clients pick one slot at once: exactly one ack,
                       exactly one nack:slot_taken
    abort_request      a request then an immediate TCP drop; the board lives
                       and the next client is served
    abort_encounter    walk onto a POI, open it, drop the connection; the
                       survivor must come back out of the encounter and the
                       POI must be put back (handleDisconnect restorePoi)
    trade_then_leave   A offers B a trade and vanishes; B's accept must be
                       refused, and nobody's inventory may change
    stalled_reader     a WebSocket that never reads while broadcasts pile up
                       (a backgrounded tab); heap must hold and recover

Every scenario is followed by a /state health check through StateOracle, so
a scenario that "passes" but leaves the seat count wrong still fails.

    python chaos.py --host k10.local
    python chaos.py --host k10.local --only seat_race,stalled_reader

Needs protocol 2 (replies) and up to two free seats; scenarios that cannot
get what they need are skipped, not failed.
"""
import argparse
import asyncio
import base64
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

from findings import Finding, FindingLog
from navigate import dijkstra
from oracles import StateOracle
from record import Recorder
from telemetry import fetch_state
from wire import ProbeClient

RUNS_DIR = Path(__file__).parent / "runs"


class Skip(Exception):
    pass


class Ctx:
    def __init__(self, host, findings, recorder, state_oracle):
        self.host = host
        self.findings = findings
        self.recorder = recorder
        self.state_oracle = state_oracle
        self.n = 0

    def client(self, tag: str) -> ProbeClient:
        self.n += 1
        return ProbeClient(self.host, self.findings, source=f"chaos:{tag}",
                           recorder=self.recorder)

    def flag(self, check, severity, summary, repro=None, **detail):
        self.findings.add(Finding(check=check, severity=severity, summary=summary,
                                  source="chaos", detail=detail, repro=repro or []))

    async def state(self) -> dict:
        st = await asyncio.to_thread(fetch_state, self.host, 5.0)
        self.state_oracle.check(st)
        return st

    async def free_seats(self) -> int:
        st = await self.state()
        return sum(1 for p in st.get("players", []) if not p.get("conn"))


async def opened(ctx: Ctx, tag: str) -> ProbeClient:
    c = ctx.client(tag)
    await c.open()
    m = await c.wait_for(lambda m: m.get("t") in ("lobby", "full"), 10.0)
    if m is None or m.get("t") == "full":
        await c.close()
        raise Skip("board full")
    return c


async def seated(ctx: Ctx, tag: str, prefer=None) -> ProbeClient:
    c = await opened(ctx, tag)
    if await c.seat(prefer) < 0:
        await c.close()
        raise Skip("could not take a seat")
    return c


async def walk_to(c: ProbeClient, q: int, r: int, budget_s: float = 90.0) -> bool:
    """Step toward (q, r) on the surface until there or out of time / MP."""
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline:
        me = c.obs.me
        if me.depth:
            return False            # fell down a hatch: not this scenario's job
        if (me.q, me.r) == (q, r):
            return True
        _dist, first = dijkstra(c.obs.map, me.q, me.r, max_cost=80)
        d = first.get((q, r), -1)
        if d < 0:
            return False
        reply = await c.request({"t": "m", "d": d})
        if reply is None:
            return False
        if reply.get("t") == "nack":
            why = reply.get("why")
            if why == "cooldown":
                await asyncio.sleep(0.6)
                continue
            return False            # no_mp, terrain, resting, in_enc: give up
        await c.wait_for(lambda m: m.get("t") == "ev" and m.get("k") == "mv"
                         and m.get("pid") == c.obs.pid, 2.0)
    return False


# -- scenarios -----------------------------------------------------------------

async def reconnect_storm(ctx: Ctx, cycles: int = 12) -> str:
    for i in range(cycles):
        c = ctx.client(f"storm{i}")
        try:
            await c.open()
            await c.wait_for(lambda m: m.get("t") in ("lobby", "full"), 5.0)
        except Exception as e:
            ctx.flag("storm_connect_failed", "major",
                     "the board refused a connection during a reconnect storm",
                     cycle=i, err=f"{type(e).__name__}: {e}")
            continue
        if i % 2:
            await c.abort()
        else:
            await c.close()
        await asyncio.sleep(0.3)
    await asyncio.sleep(3.0)        # let handleDisconnect / the reap settle
    await ctx.state()               # StateOracle checks the seat count
    try:
        c = await seated(ctx, "after-storm")
    except Skip as e:
        if str(e) == "board full":
            ctx.flag("storm_left_board_full", "critical",
                     "after the storm the board reports full with seats free")
        raise
    await c.close()
    return "pass"


async def seat_race(ctx: Ctx) -> str:
    if await ctx.free_seats() < 1:
        raise Skip("no free seat")
    a, b = await opened(ctx, "raceA"), await opened(ctx, "raceB")
    try:
        slot = max(a.lobby.get("avail", []) or [-1])
        if slot < 0:
            raise Skip("no slot in lobby")
        ra, rb = await asyncio.gather(a.request({"t": "pick", "arch": slot}, 5.0),
                                      b.request({"t": "pick", "arch": slot}, 5.0))
        verdicts = sorted((r or {}).get("t", "silence") +
                          (":" + r["why"] if r and r.get("why") else "")
                          for r in (ra, rb))
        if verdicts != ["ack", "nack:slot_taken"]:
            ctx.flag("seat_race", "critical" if verdicts == ["ack", "ack"] else "major",
                     "two simultaneous picks of one slot did not resolve to one winner",
                     verdicts=verdicts, slot=slot)
            return "fail"
        return "pass"
    finally:
        await a.close()
        await b.close()


async def abort_request(ctx: Ctx) -> str:
    c = await seated(ctx, "abortreq")
    slot = c.arch
    stamped, _ = c.tracker.stamp({"t": "m", "d": 0})
    await c.ws.send(json.dumps(stamped))
    await c.abort()
    await asyncio.sleep(2.0)
    c2 = await seated(ctx, "abortreq2", prefer=slot)
    reply = await c2.request({"t": "check", "sk": 9, "dn": 5})
    await c2.close()
    if reply is None:
        ctx.flag("unserved_after_abort", "major",
                 "a client after an aborted one got no reply")
        return "fail"
    return "pass"


async def abort_encounter(ctx: Ctx) -> str:
    c = await seated(ctx, "enc")
    slot = c.arch
    try:
        me = c.obs.me
        # Nearest known POI on the surface.
        dist, _first = dijkstra(c.obs.map, me.q, me.r, max_cost=40)
        pois = sorted((cost, q, r) for (q, r), cost in dist.items()
                      if (cell := c.obs.map[(q, r)]) is not None and cell.poi)
        if not pois:
            raise Skip("no known POI within reach")
        _cost, q, r = pois[0]
        if not await walk_to(c, q, r):
            raise Skip("could not walk to the POI (MP, terrain or time)")
        reply = await c.request({"t": "enc_start", "q": q, "r": r}, 5.0)
        if reply is None or reply.get("t") != "ack":
            raise Skip(f"enc_start refused: {reply}")
        await c.abort()
    except Skip:
        await c.close()
        raise
    await asyncio.sleep(2.0)
    c2 = await seated(ctx, "enc2", prefer=slot)
    try:
        if c2.arch != slot:
            raise Skip("slot was taken before the probe could return")
        tick = await c2.wait_for(lambda m: m.get("t") == "s", 3.0)
        me = (tick or {}).get("p", [{}] * 6)[slot] if tick else {}
        ok = True
        if me.get("enc"):
            ok = False
            ctx.flag("encounter_survived_disconnect", "major",
                     "an encounter stayed open across an abrupt disconnect",
                     slot=slot, q=q, r=r)
        cell = c2.obs.map[(q, r)]
        if cell is not None and not cell.poi:
            ok = False
            ctx.flag("poi_not_restored", "major",
                     "an involuntarily ended encounter did not put its POI back",
                     slot=slot, q=q, r=r)
        reply = await c2.request({"t": "m", "d": 0})
        if reply and reply.get("why") == "in_enc":
            ok = False
            ctx.flag("locked_in_encounter", "critical",
                     "a returning survivor cannot move: still in the encounter",
                     slot=slot)
        return "pass" if ok else "fail"
    finally:
        await c2.close()


async def trade_then_leave(ctx: Ctx) -> str:
    if await ctx.free_seats() < 2:
        raise Skip("needs two free seats")
    a = await seated(ctx, "tradeA")
    b = None
    try:
        b = await seated(ctx, "tradeB")
        await asyncio.sleep(0.5)
        pa = a.obs.players[a.arch]
        if not await walk_to(b, pa.q, pa.r, budget_s=60):
            raise Skip("B could not reach A's hex")
        mine = b.obs.me.inv
        # Scrap, then medicine, then fuel: dawn consumes food and water (and
        # equipment can burn fuel), so asking for those would let a dawn
        # landing mid-scenario look like resources moving on a refused trade.
        res = next((i for i in (4, 3, 2) if mine[i] > 0), None)
        if res is None:
            raise Skip("B has no scrap, medicine or fuel to be asked for")
        want = [0] * 5
        want[res] = 1
        before = list(b.obs.me.inv)
        offer = await a.request({"t": "trade_offer", "to": b.arch,
                                 "give": [0] * 5, "want": want})
        if offer is None or offer.get("t") != "ack":
            raise Skip(f"offer refused: {offer}")
        await b.wait_for(lambda m: m.get("t") == "ev" and m.get("k") == "trd_off", 3.0)
        await a.abort()
        await asyncio.sleep(2.0)
        reply = await b.request({"t": "trade_accept", "from": a.arch})
        await b.wait_for(lambda m: m.get("t") == "s", 2.0)
        after = list(b.obs.me.inv)
        ok = True
        if reply is None or reply.get("t") != "nack":
            ok = False
            ctx.flag("trade_with_departed_player", "major",
                     "an offer from a player who had left was accepted",
                     reply=reply, before=before, after=after)
        elif after[res] != before[res]:
            ok = False
            ctx.flag("trade_refused_but_resources_moved", "critical",
                     "a refused trade still changed the inventory",
                     res=res, before=before, after=after)
        return "pass" if ok else "fail"
    finally:
        await a.close()
        if b is not None:
            await b.close()


async def stalled_reader(ctx: Ctx, hold_s: float = 20.0) -> str:
    """A raw WebSocket upgrade that then never reads a byte."""
    host, _, port = ctx.host.partition(":")
    port = int(port or 80)
    before = await ctx.state()
    reader, writer = await asyncio.open_connection(host, port)
    key = base64.b64encode(os.urandom(16)).decode()
    writer.write((f"GET /ws HTTP/1.1\r\nHost: {ctx.host}\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                  "Sec-WebSocket-Version: 13\r\n\r\n").encode())
    await writer.drain()
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10.0)
    if b" 101 " not in head.split(b"\r\n", 1)[0]:
        writer.close()
        raise Skip(f"upgrade refused: {head[:40]!r}")
    lowest = (before.get("mem") or {}).get("heap")
    t_end = time.monotonic() + hold_s
    while time.monotonic() < t_end:          # never read `reader`
        await asyncio.sleep(2.0)
        st = await ctx.state()
        h = (st.get("mem") or {}).get("heap")
        if isinstance(h, int) and (lowest is None or h < lowest):
            lowest = h
    writer.transport.abort()
    await asyncio.sleep(5.0)
    after = await ctx.state()
    h0 = (before.get("mem") or {}).get("heap")
    h1 = (after.get("mem") or {}).get("heap")
    ctx.recorder.write("chaos_heap", -1, {"before": h0, "lowest": lowest, "after": h1})
    if isinstance(h0, int) and isinstance(h1, int) and h1 < h0 - 16 * 1024:
        ctx.flag("heap_not_recovered", "major",
                 "free heap did not recover after a stalled client went away",
                 before=h0, lowest=lowest, after=h1)
        return "fail"
    return "pass"


SCENARIOS = {
    "reconnect_storm": reconnect_storm,
    "seat_race": seat_race,
    "abort_request": abort_request,
    "abort_encounter": abort_encounter,
    "trade_then_leave": trade_then_leave,
    "stalled_reader": stalled_reader,
}


async def main_async(a) -> int:
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    path = RUNS_DIR / f"chaos-{datetime.now().strftime('%Y%m%d-%H%M%S')}.jsonl"
    names = [n.strip() for n in a.only.split(",")] if a.only else list(SCENARIOS)
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        raise SystemExit(f"unknown scenario(s) {unknown}; have {list(SCENARIOS)}")
    with Recorder(path, {"kind": "chaos", "host": a.host, "scenarios": names}) as rec:
        findings = FindingLog(rec)
        ctx = Ctx(a.host, findings, rec, StateOracle(findings, source="chaos:state"))
        try:
            st = await ctx.state()
        except Exception as e:
            raise SystemExit(f"cannot reach {a.host}: {e}")
        if st.get("pv", 1) < 2:
            raise SystemExit(f"{a.host} reports protocol {st.get('pv', 1)}; chaos "
                             "scenarios judge by ack/nack and need protocol 2")
        print(f"chaos {a.host}: pv={st.get('pv')} -> {path.name}")
        results = {}
        for name in names:
            t0 = time.monotonic()
            try:
                results[name] = await SCENARIOS[name](ctx)
            except Skip as e:
                results[name] = f"skip ({e})"
            except Exception as e:
                results[name] = f"error ({type(e).__name__}: {e})"
                ctx.flag("scenario_error", "major", f"{name} raised", scenario=name,
                         err=f"{type(e).__name__}: {e}")
            try:
                await ctx.state()
            except Exception as e:
                ctx.flag("board_unreachable", "critical",
                         f"/state stopped answering after {name}", err=str(e))
                results[name] += " -- BOARD DOWN"
                print(f"  {name:<18} {results[name]}")
                break
            rec.write("scenario", -1, {"name": name, "result": results[name],
                                       "s": round(time.monotonic() - t0, 1)})
            print(f"  {name:<18} {results[name]}")
            await asyncio.sleep(1.0)
        findings.print_summary()
        findings.flush()
        return 1 if len(findings) else 0


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="k10.local")
    p.add_argument("--only", default="", help="comma-separated scenario names")
    return p.parse_args(argv)


if __name__ == "__main__":
    sys.exit(asyncio.run(main_async(parse_args())))
