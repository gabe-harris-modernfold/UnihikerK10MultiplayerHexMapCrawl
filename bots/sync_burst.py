"""Join-burst probe: N sockets pick at the same instant, and the K10's memory
is read from /state around it.

    python sync_burst.py --host 192.168.4.135 --rounds 3

Every pick is answered with a ~32 KB `sync` plus the ecology message. Several
at once -- survivors dying at the same dawn and re-picking together, or phones
rejoining after a Wi-Fi blip -- is the burst that took the board's internal
heap to 19 KB on 2026-10-23 (docs/bot-testing.md, "The wroom swarm").

`minHeap` in /state is a low-water mark since boot, so reboot the board first
for a clean number, and run this with nobody else seated (it needs the seats).
Prints pick -> sync latency per socket, and the board's heap before and after.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time
import urllib.request

import websockets


def state(host: str) -> dict:
    with urllib.request.urlopen(f"http://{host}/state", timeout=8) as r:
        return json.loads(r.read())


def mem_line(tag: str, st: dict) -> str:
    m = st.get("mem", {})
    return (f"{tag:7} heap={m.get('heap')} minHeap={m.get('minHeap')} maxBlock={m.get('maxBlock')} "
            f"psram={m.get('psram')} psramMax={m.get('psramMax', '-')} uptime={m.get('uptimeMs', 0) // 1000}s")


async def recv_until(ws, want: str, timeout: float):
    end = time.monotonic() + timeout
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return None
        try:
            raw = await asyncio.wait_for(ws.recv(), left)
        except (asyncio.TimeoutError, websockets.ConnectionClosed):
            return None
        if isinstance(raw, bytes):
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if msg.get("t") == want:
            return msg
        if msg.get("t") == "full":
            return msg


async def one_round(host: str, n: int, hold: float) -> list:
    socks = [await websockets.connect(f"ws://{host}/ws", max_size=None, open_timeout=8) for _ in range(n)]
    avail = []
    for ws in socks:
        lobby = await recv_until(ws, "lobby", 8)
        if not lobby or lobby.get("t") != "lobby":
            raise SystemExit(f"no lobby (got {lobby}) -- is someone else seated?")
        avail = lobby.get("avail", [])
    if len(avail) < n:
        raise SystemExit(f"only {len(avail)} free seats: {avail}")

    async def pick(ws, arch, rid):
        t0 = time.monotonic()
        await ws.send(json.dumps({"t": "pick", "arch": arch, "rid": rid}))
        msg = await recv_until(ws, "sync", 15)
        return arch, (time.monotonic() - t0) * 1000 if msg and msg.get("t") == "sync" else None

    results = await asyncio.gather(*(pick(ws, a, 9000 + i) for i, (ws, a) in enumerate(zip(socks, avail))))
    await asyncio.sleep(hold)
    for ws in socks:
        await ws.close()
    return results


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.4.135")
    ap.add_argument("--n", type=int, default=5)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--hold", type=float, default=3.0, help="seconds seated before closing")
    args = ap.parse_args()
    print(mem_line("before", state(args.host)))
    for r in range(args.rounds):
        res = await one_round(args.host, args.n, args.hold)
        lat = ", ".join(f"arch{a}:{'-' if ms is None else f'{ms:.0f}ms'}" for a, ms in res)
        print(f"round {r + 1}: pick->sync {lat}")
        await asyncio.sleep(4)   # let the board release the seats
        print(mem_line("after", state(args.host)))


if __name__ == "__main__":
    asyncio.run(main())
