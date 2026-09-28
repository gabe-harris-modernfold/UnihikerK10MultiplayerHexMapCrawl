"""The Understory, checked over the wire (docs/ecology-spec.md).

Seats one probe, waits for the `eco` message and checks its shape against the
spec: the vein grid is two characters per hex over the whole board, the edge
masks are base64 digits, fruiting bodies and daisy patches are on the board,
the `/state` `eco` block agrees with the wire (name, seed, on) and stays
inside the loudness cap.  On the mock (`--bite`) it also plants a bloomed
patch on its own hex with `dbg_eco`, steps off and back, and checks the bite
arrives as a `dmg` event with the `wasteland daisy` cause and costs exactly
one LL.  Run: python eco_check.py [host[:port]] [--bite] [--timeout S]

Every check prints `ok`/`FAIL` and the exit code is the number of failures,
so it slots into a shell loop.  Read-only apart from the `--bite` flag: it
never regens, never pins a seed.
"""
import argparse
import asyncio
import json
import sys
import urllib.request

import config
from findings import FindingLog
from wire import ProbeClient

B64 = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
HEXD = set("0123456789ABCDEFabcdef")
HEXES = config.MAP_COLS * config.MAP_ROWS

fails = 0


def check(label, cond, detail=""):
    global fails
    if cond:
        print(f"  ok   {label}")
    else:
        fails += 1
        print(f"  FAIL {label}" + (f"  ({detail})" if detail else ""))


def fetch_state(host, query=""):
    with urllib.request.urlopen(f"http://{host}/state{query}", timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def on_board(entry, n):
    return (isinstance(entry, list) and len(entry) >= n
            and 0 <= entry[0] < config.MAP_COLS and 0 <= entry[1] < config.MAP_ROWS)


async def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default="localhost:8765")
    ap.add_argument("--bite", action="store_true",
                    help="mock only: plant a bloomed patch with dbg_eco and walk into it")
    ap.add_argument("--timeout", type=float, default=12.0,
                    help="seconds to wait for the first eco message (board tick is 5 s)")
    args = ap.parse_args(argv)

    c = ProbeClient(args.host, FindingLog(), source="eco_check", min_interval=0.2)
    await c.open()
    slot = await c.seat()
    check("seated", slot >= 0, "no free slot")
    if slot < 0:
        await c.close()
        return

    print("wire")
    main_msg = await c.wait_for(lambda m: m.get("t") == "eco" and "v" in m, args.timeout)
    check("eco message arrives", main_msg is not None, f"none within {args.timeout}s")
    scar_msg = next((m for m in c.received if m.get("t") == "eco" and "s" in m), None)
    check("scar message arrives on sync", scar_msg is not None)
    if main_msg is not None:
        v = main_msg.get("v", "")
        check("v is two chars per hex", len(v) == HEXES * 2, f"len {len(v)} vs {HEXES * 2}")
        check("density digits are hex", all(ch in HEXD for ch in v[0::2]))
        check("edge masks are base64 digits", all(ch in B64 for ch in v[1::2]))
        check("name present", isinstance(main_msg.get("n"), str) and len(main_msg["n"]) >= 5)
        check("hue is a byte", isinstance(main_msg.get("h"), int) and 0 <= main_msg["h"] <= 255)
        check("fruit nibble", isinstance(main_msg.get("g"), int) and 0 <= main_msg["g"] <= 15)
        check("wave counter", isinstance(main_msg.get("w"), int) and main_msg["w"] >= 0)
        check("fruiting bodies on the board", all(on_board(e, 4) for e in main_msg.get("f", [])))
        check("daisy patches on the board",
              all(on_board(e, 5) and 1 <= e[2] <= 3 and 1 <= e[3] <= 7
                  for e in main_msg.get("dz", [])))
        check("spore flights on the board", all(on_board(e, 4) and on_board(e[2:], 2)
                                                for e in main_msg.get("sp", [])))
        dens = [int(ch, 16) for ch in v[0::2]]
        visible = sum(1 for d in dens if d >= 4)
        print(f"       tick={main_msg.get('tk')} name={main_msg.get('n')!r} wave={main_msg.get('w')} "
              f"visible hexes={visible} ({100 * visible // HEXES}%) bodies={len(main_msg.get('f', []))} "
              f"daisies={len(main_msg.get('dz', []))}")
    if scar_msg is not None:
        s = scar_msg.get("s", "")
        check("s is one char per hex", len(s) == HEXES, f"len {len(s)}")
        check("scar digits are hex", all(ch in HEXD for ch in s))
    check("observation kept the daisies", isinstance(c.obs.daisies, dict))
    check("observation priced the blooms",
          all(c.obs.map.extra_cost.get(qr) for qr, (st, _n) in c.obs.daisies.items() if st == 3))

    print("/state")
    try:
        st = fetch_state(args.host)
        eco = st.get("eco")
        check("eco block present", isinstance(eco, dict))
        if isinstance(eco, dict):
            check("eco.on", eco.get("on") is True or eco.get("on") == 1)
            if main_msg is not None:
                check("name agrees with the wire", eco.get("name") == main_msg.get("n"),
                      f"{eco.get('name')!r} vs {main_msg.get('n')!r}")
                check("hue agrees with the wire", eco.get("hue") == main_msg.get("h"))
            check("coverage inside the cap", 0 <= (eco.get("coverage") or 0) <= 25,
                  f"coverage {eco.get('coverage')}")
            check("bite switch reported", eco.get("bite") in (0, 1, True, False))
            check("blight switch reported", eco.get("blight") in (0, 1, True, False))
            check("blight count reported", isinstance(eco.get("eaten"), int) and eco.get("eaten") >= 0)
            d = eco.get("daisies") or {}
            check("daisy counts", all(k in d for k in ("seeded", "growing", "bloomed")))
            print(f"       seed={eco.get('seed')} style={eco.get('style')} vector={eco.get('vector')} "
                  f"tick={eco.get('tick')} tickUs={eco.get('tickUs')} colonies={eco.get('colonies')} "
                  f"agents={eco.get('agents')} spores={eco.get('spores')} scarred={eco.get('scarred')} "
                  f"bootsSeen={eco.get('bootsSeen')}")
    except Exception as e:  # noqa: BLE001 -- a probe reports, it does not crash
        check("/state reachable", False, f"{type(e).__name__}: {e}")

    if args.bite:
        print("bite (mock)")
        me = c.obs.me
        q0, r0 = me.q, me.r
        ll0 = me.ll
        await c.request({"t": "dbg_eco", "act": "daisy", "q": q0, "r": r0, "stage": 3, "count": 3})
        grid = await c.wait_for(lambda m: m.get("t") == "eco" and "dz" in m
                                and any(e[0] == q0 and e[1] == r0 and e[2] == 3 for e in m["dz"]), 6.0)
        check("patch is on the wire", grid is not None)
        # Step off along the first legal direction, then straight back.
        dirs = me.legal_dirs() or [0]
        d = dirs[0]
        back = (d + 3) % 6
        await c.request({"t": "m", "d": d})
        await c.wait_for(lambda m: m.get("t") == "ev" and m.get("k") == "mv"
                         and m.get("pid") == c.obs.pid, 3.0)
        dmg = asyncio.create_task(c.wait_for(
            lambda m: m.get("t") == "ev" and m.get("k") == "dmg" and m.get("pid") == c.obs.pid, 4.0))
        await c.request({"t": "m", "d": back})
        ev = await dmg
        check("dmg event on re-entry", ev is not None)
        if ev is not None:
            check("cause is the daisy", ev.get("cause") == "wasteland daisy", repr(ev.get("cause")))
            check("bite costs 1 LL", ev.get("amt") == 1 and ev.get("ll") == ll0 - 1,
                  f"amt={ev.get('amt')} ll={ev.get('ll')} was {ll0}")
        await c.wait_for(lambda m: m.get("t") == "s", 3.0)
        check("broadcast agrees", c.obs.me.ll == ll0 - 1, f"ll {c.obs.me.ll} vs {ll0 - 1}")

    await c.close()
    print(f"{fails} failure(s)")


if __name__ == "__main__":
    asyncio.run(main())
    sys.exit(min(fails, 100))
