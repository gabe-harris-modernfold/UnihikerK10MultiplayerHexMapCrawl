"""Protocol fuzzer: malformed, prefixed, oversized and out-of-order messages.

Asserts two things about every case in fuzz_cases.py:

  1. the board survives it -- /state keeps answering and uptime keeps rising
  2. the board answers it exactly as the handler says it should: one reply,
     the right verdict, the right reason (network-reply.hpp)

Each failure is a finding (findings.py) carrying the case, what came back,
and the last messages sent.  A board that stops answering ends the run with a
critical finding naming the cases that preceded it.

    python fuzz.py --host k10.local                  # the whole corpus
    python fuzz.py --host k10.local --only prefix:   # one family
    python fuzz.py --host k10.local --burst 20       # + an unthrottled burst

Refuses to run unless /state reports protocol 2: on older firmware a
one-letter command wipes the world (fuzz_cases.py SAFETY).  Takes one free
seat, the highest, and leaves the world as it found it apart from the
probe's own name.
"""
import argparse
import asyncio
import json
import sys
import time
from datetime import datetime
from pathlib import Path

from findings import Finding, FindingLog
from fuzz_cases import BURST, CASES, Case, judge
from oracles import StateOracle
from record import Recorder
from telemetry import fetch_state
from wire import ProbeClient

RUNS_DIR = Path(__file__).parent / "runs"
REPLY_WAIT_S = 3.0          # how long a case waits for its verdict
SILENCE_WAIT_S = 1.0        # how long an "expect none" case listens
HEALTH_EVERY = 10           # cases between /state health checks


class BoardDown(Exception):
    pass


async def health(host: str, oracle: StateOracle, tries: int = 3) -> dict:
    """Read /state through the StateOracle; BoardDown if it will not answer."""
    last = None
    for i in range(tries):
        try:
            st = await asyncio.to_thread(fetch_state, host, 5.0)
            oracle.check(st)
            return st
        except Exception as e:
            last = e
            await asyncio.sleep(2.0 * (i + 1))
    raise BoardDown(f"/state unreachable after {tries} tries: {last}")


def render(case: Case, obs):
    p = case.payload(obs) if callable(case.payload) else case.payload
    return p


async def run_case(client: ProbeClient, case: Case) -> tuple[str | None, object, object]:
    """Send one case.  Returns (failure-or-None, what was sent, the reply)."""
    payload = render(case, client.obs)
    if payload is None:
        return None, None, "skipped"
    reply = None
    if isinstance(payload, dict):
        reply = await client.request(payload, timeout=REPLY_WAIT_S)
        sent = payload
    else:
        wait = SILENCE_WAIT_S if case.expect == "none" else REPLY_WAIT_S
        rid = None
        fut = None
        if isinstance(payload, str) and "@RID@" in payload:
            stamped, fut = client.tracker.stamp({"t": "?"}, want_future=True)
            rid = stamped["rid"]
            payload = payload.replace("@RID@", str(rid))
        elif case.rid_as_parsed is not None:
            # The board will answer with the rid strtoul makes of the mangled
            # one; register that so the reply is matched, not flagged.
            rid = case.rid_as_parsed
            fut = asyncio.get_running_loop().create_future()
            client.tracker.inflight[rid] = ("?", time.monotonic(), fut)
        sent = payload
        await client.send_raw(payload, fragments=case.fragments)
        if fut is not None:
            try:
                reply = await asyncio.wait_for(asyncio.shield(fut), wait)
            except asyncio.TimeoutError:
                reply = None
            # Whatever happened, this rid is done: do not let the tracker
            # later count an intentionally silent case as unanswered.
            client.tracker.inflight.pop(rid, None)
        else:
            await asyncio.sleep(wait)
    return judge(case.expect, reply), sent, reply


async def ensure_open(client: ProbeClient, seated: bool, findings: FindingLog,
                      after: str) -> None:
    """Reopen (and reseat) if the last case cost us the socket.  A board that
    closes a socket over one malformed message is itself a finding."""
    if client.ws is not None and not client.closed.is_set():
        if not seated or client.seated:
            return
    if client.ws is not None and client.closed.is_set():
        findings.add(Finding(check="fuzz_socket_closed", severity="major",
                             summary="the board closed the socket after a case",
                             source=client.source, key=(after,),
                             detail={"after": after, "info": client.close_info},
                             repro=client.repro.snapshot()))
        await client.close()
    if client.ws is None or client.closed.is_set():
        await client.open()
        await client.wait_for(lambda m: m.get("t") in ("lobby", "full"), 10.0)
    if seated and not client.seated:
        if await client.seat() < 0:
            raise BoardDown("could not take a seat")


async def main_async(a) -> int:
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = RUNS_DIR / f"fuzz-{stamp}.jsonl"
    with Recorder(path, {"kind": "fuzz", "host": a.host, "only": a.only,
                         "burst": a.burst}) as rec:
        findings = FindingLog(rec)
        state_oracle = StateOracle(findings, source="fuzz:state")
        try:
            st = await health(a.host, state_oracle)
        except BoardDown as e:
            raise SystemExit(f"cannot reach {a.host}: {e}")
        if st.get("pv", 1) < 2 and not a.force_old:
            raise SystemExit(
                f"{a.host} reports protocol {st.get('pv', 1)}: on firmware before "
                "protocol 2 a one-letter command runs eraseslot/regen. Flash the "
                "current firmware first (or --force-old if the world is expendable).")
        free = [p for p in st.get("players", []) if not p.get("conn")]
        if not free:
            raise SystemExit("no free seat on the board")
        print(f"fuzz {a.host}: pv={st.get('pv')} up={st['mem'].get('uptimeMs', 0)//1000}s "
              f"-> {path.name}")

        cases = [c for c in CASES if not a.only or c.name.startswith(a.only)]
        cases.sort(key=lambda c: c.seated)     # lobby-phase cases first
        client = ProbeClient(a.host, findings, source="fuzz", recorder=rec,
                             min_interval=a.min_interval)
        await client.open()
        await client.wait_for(lambda m: m.get("t") in ("lobby", "full"), 10.0)
        failed = passed = skipped = 0
        last_name = "(start)"
        try:
            for i, case in enumerate(cases):
                await ensure_open(client, case.seated, findings, last_name)
                if case.seated and not client.seated:
                    if await client.seat() < 0:
                        raise BoardDown("could not take a seat")
                why, sent, reply = await run_case(client, case)
                last_name = case.name
                if reply == "skipped":
                    skipped += 1
                    continue
                if why is None:
                    passed += 1
                else:
                    failed += 1
                    findings.add(Finding(
                        check=f"fuzz:{case.name}", severity="major", summary=why,
                        source="fuzz", key=(),
                        detail={"sent": sent if isinstance(sent, (dict, str)) else repr(sent),
                                "expect": case.expect, "reply": reply, "note": case.note},
                        repro=client.repro.snapshot()))
                    if a.verbose:
                        print(f"  FAIL {case.name}: {why}")
                if (i + 1) % HEALTH_EVERY == 0:
                    await health(a.host, state_oracle)
            if a.burst:
                await ensure_open(client, True, findings, last_name)
                replies = await burst(client, a.burst)
                got = sum(1 for r in replies if r is not None)
                if got != a.burst:
                    findings.add(Finding(
                        check="fuzz:burst", severity="major",
                        summary=f"{a.burst} unthrottled requests, {got} replies",
                        source="fuzz", detail={"sent": a.burst, "replied": got}))
            await health(a.host, state_oracle)
        except BoardDown as e:
            findings.add(Finding(check="board_unreachable", severity="critical",
                                 summary=str(e), source="fuzz",
                                 detail={"last_case": last_name},
                                 repro=client.repro.snapshot()))
        finally:
            await client.close()

        print(f"  {passed} passed, {failed} failed, {skipped} skipped "
              f"of {len(cases)} case(s)")
        findings.print_summary()
        findings.flush()
        return 1 if len(findings) else 0


async def burst(client: ProbeClient, n: int) -> list:
    """n requests with the throttle off, then collect the verdicts."""
    saved, client.min_interval = client.min_interval, 0.0
    try:
        futs = []
        for _ in range(n):
            stamped, fut = client.tracker.stamp(dict(BURST.payload), want_future=True)
            await client.ws.send(json.dumps(stamped, separators=(",", ":")))
            futs.append(fut)
        out = []
        for f in futs:
            try:
                out.append(await asyncio.wait_for(asyncio.shield(f), REPLY_WAIT_S * 2))
            except asyncio.TimeoutError:
                out.append(None)
        return out
    finally:
        client.min_interval = saved


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="k10.local")
    p.add_argument("--only", default="", help="run only cases whose name starts with this")
    p.add_argument("--burst", type=int, default=0,
                   help="finish with N unthrottled requests (stress; off by default)")
    p.add_argument("--min-interval", type=float, default=0.30, dest="min_interval")
    p.add_argument("--force-old", action="store_true", dest="force_old",
                   help="run against a pre-protocol-2 board (may wipe the world)")
    p.add_argument("-v", "--verbose", action="store_true")
    return p.parse_args(argv)


if __name__ == "__main__":
    sys.exit(asyncio.run(main_async(parse_args())))
