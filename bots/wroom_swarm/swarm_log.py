"""Log a wroom_swarm run: the wroom's report port and the K10's own serial log,
side by side, then summarise what the link did.

    python swarm_log.py --minutes 10 --out runs/swarm-baseline
    python swarm_log.py --summarize runs/swarm-baseline

Both ports open with DTR/RTS held low, so neither board resets. The wroom's
counters are zeroed at the start ("zero"), so its last report line is the whole
window. The K10 log is optional (--k10 ""), and it is what says *why*: NETWD
lines every 10 s while anyone is seated, plus every AutoJoin sweep and Wi-Fi
event.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from pathlib import Path

REPORT_S = 10      # the wroom's REPORT_MS

K10_PATTERNS = {
    "netwd_stall": r"NETWD STALL",
    "data_dead": r"NETWD DATA-DEAD",
    "heal": r"NETWD HEAL",
    "mbox_full": r"lwIP mailbox full",
    "ws_error": r"WS ERROR",
    "ws_disconnect": r"WS DISCONNECT",
    "ws_reject_full": r"WS REJECT",
    "autojoin_sweep": r"AutoJoin sweep start",
    "autojoin_scan": r"AutoJoin scan found",
    "sta_disconnected": r"WIFI STA disconnected",
    "ap_client_joined": r"WIFI AP client joined",
    "ap_client_left": r"WIFI AP client left",
    "broadcast_held": r"broadcastState: held",
    "alloc_fail": r"alloc.*fail",
    "watchdog": r"(?i)task watchdog|TASK_WDT",
    "panic": r"Guru Meditation|abort\(\)|Backtrace",
    "boot": r"==== BOOT ====",
}


def open_port(name: str):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = name, 115200, 0.5
    s.dtr = False      # set before open: no auto-reset pulse on either board
    s.rts = False
    s.open()
    return s


def pump(ports: dict, key: str, path: Path, stop: threading.Event) -> None:
    with path.open("a", encoding="utf-8", errors="replace") as f:
        buf = b""
        while not stop.is_set():
            port = ports[key]
            try:
                chunk = port.read(4096)
            except Exception as e:
                # A rebooting K10 drops its USB-CDC port: reopen once it is back.
                f.write(f"{time.time():.3f} !! {key} read error: {e}\n")
                f.flush()
                try:
                    port.close()
                except Exception:
                    pass
                while not stop.is_set():
                    time.sleep(1.0)
                    try:
                        ports[key] = open_port(port.port)
                        f.write(f"{time.time():.3f} !! {key} reopened\n")
                        break
                    except Exception:
                        continue
                continue
            if not chunk:
                continue
            buf += chunk
            *lines, buf = buf.split(b"\n")
            t = time.time()
            for ln in lines:
                f.write(f"{t:.3f} {ln.decode('utf-8', 'replace').rstrip()}\n")
            f.flush()


def record(args) -> Path:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stop = threading.Event()
    ports = {"wroom": open_port(args.wroom)}
    if args.k10:
        ports["k10"] = open_port(args.k10)
    threads = [threading.Thread(target=pump, args=(ports, key, out / f"{key}.log", stop), daemon=True)
               for key in ports]
    for t in threads:
        t.start()
    for cmd in args.cmd:
        ports["wroom"].write((cmd + "\n").encode())
        time.sleep(0.3)
    for cmd in args.k10_cmd:            # e.g. "away": the K10 reboots, the pump reopens its port
        ports["k10"].write((cmd + "\n").encode())
        time.sleep(0.3)
    t0 = time.time()
    zero_at = t0 + args.zero_after if args.zero else None
    if zero_at is not None and args.zero_after <= 0:
        ports["wroom"].write(b"zero\n")
        zero_at = None
    (out / "meta.json").write_text(json.dumps({"start": t0, "minutes": args.minutes, "cmd": args.cmd,
                                               "k10_cmd": args.k10_cmd, "zero_after": args.zero_after,
                                               "note": args.note}, indent=1))
    deadline = t0 + args.minutes * 60
    try:
        while time.time() < deadline:
            time.sleep(1.0)
            if zero_at is not None and time.time() >= zero_at:
                ports["wroom"].write(b"zero\n")
                zero_at = None
    except KeyboardInterrupt:
        pass
    ports["wroom"].write(b"report\n")
    time.sleep(1.5)
    for cmd in args.end_cmd:
        ports["wroom"].write((cmd + "\n").encode())
        time.sleep(0.3)
    stop.set()
    for t in threads:
        t.join(timeout=3)
    for p in ports.values():
        p.close()
    return out


def reports(path: Path) -> list[dict]:
    """Report rows from the last `zero` on (each carries its PC time as _ts)."""
    rows = []
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "cmd=zero" in ln:
            rows = []
            continue
        i = ln.find(" R {")
        if i < 0:
            continue
        try:
            row = json.loads(ln[i + 3:])
        except json.JSONDecodeError:
            continue
        row["_ts"] = float(ln.split(" ", 1)[0])
        rows.append(row)
    return rows


def summarize(out: Path) -> None:
    rows = reports(out / "wroom.log")
    if not rows:
        print("no report lines in", out / "wroom.log")
        return
    first, last = rows[0], rows[-1]
    secs = (last["ms"] - first["ms"]) / 1000 or 1
    print(f"== {out}  ({len(rows)} reports, {secs / 60:.1f} min, pv {last.get('pv')}, "
          f"link ap={last['link']['ap']} cap={last['link']['cap']})")
    w = last["wifi"]
    rssi = [r["wifi"]["rssi"] for r in rows if r["wifi"]["up"]]
    print(f"wroom wifi: drops={w['drops']} down={w['downMs']} ms ch={w['ch']} "
          f"rssi {min(rssi, default=0)}..{max(rssi, default=0)}  heap min {last['minHeap']}")
    hdr = ("seat arch seated  conn drops full resp |  ticks  miss  deliv  >0.5s >2s maxgap |"
           "   tx   ack nack unans lost  rtt  maxrtt  moves")
    print(hdr)
    tot = {}
    for i, s in enumerate(last["seats"]):
        maxgap = max(r["seats"][i]["mg"] for r in rows if i < len(r["seats"]))
        maxrtt = max(r["seats"][i]["rm"] for r in rows if i < len(r["seats"]))
        drops = s["hs"] + s["tf"] + s["cf"] + s["tl"] + s["sk"] + s["sg"] + s["jf"]
        deliv = s["tk"] / (s["tk"] + s["tm"]) * 100 if s["tk"] + s["tm"] else 0
        print(f"{i:>4} {s['a']:>4} {s['sd']:>5}s {s['c']:>5} {drops:>5} {s['fl']:>4} {s['rs']:>4} |"
              f" {s['tk']:>6} {s['tm']:>5} {deliv:>5.1f}% {s['g5']:>5} {s['g2']:>3} {maxgap:>5}ms |"
              f" {s['tx']:>4} {s['ak']:>5} {s['nk']:>4} {s['un']:>5} {s['lo']:>4} {s['rtt']:>4} {maxrtt:>6}ms"
              f" {s['mv']:>6}")
        for k in ("tk", "tm", "g5", "g2", "tx", "ak", "nk", "un", "lo", "mv", "cf", "tl", "sk", "sg", "jf",
                  "hs", "tf", "fl", "rs", "kb"):
            tot[k] = tot.get(k, 0) + s[k]
    print(f"drops by kind: close_frame={tot['cf']} tcp_lost={tot['tl']} stall={tot['sk']} seat_gone={tot['sg']}"
          f" join_fail={tot['jf']} handshake={tot['hs']} tcp_connect={tot['tf']} full={tot['fl']}"
          f" respawns={tot['rs']}")
    print(f"rx {tot['kb'] / 1024:.1f} MB = {tot['kb'] * 8 / secs:.0f} kbit/s into the wroom")
    edges = ["<25", "<50", "<100", "<250", "<500", "<1000", "<2500", ">=2500"]
    print("rtt ms: " + "  ".join(f"{e}:{n}" for e, n in zip(edges, last["rttHist"])))
    print("nacks: " + ", ".join(f"{k}={v}" for k, v in sorted(last["nack"].items(), key=lambda x: -x[1])))
    k = [r["k10"] for r in rows if r["k10"]["ok"]]
    if k:
        kf, kl = k[0], k[-1]
        print(f"K10 /state: polls ok={kl['ok']} fail={kl['fail']} httpMax={kl['httpMax']} ms reboots={kl['reboots']}"
              f" reset={kl['reset']} uptime {kl['uptime'] / 60000:.1f} min")
        print(f"  heap {min(x['heap'] for x in k)}..{max(x['heap'] for x in k)}  minHeap {kl['minHeap']}"
              f"  maxBlock min {min(x['maxBlock'] for x in k)}  maxTickMs {kl['maxTick']}")
        print(f"  broadcastPartial +{kl['partial'] - kf['partial']}  broadcastSkips +{kl['skips'] - kf['skips']}"
              f"  evtDrops +{kl['evtDrops'] - kf['evtDrops']}  connected seen {sorted({x['connected'] for x in k})}"
              f"  day {kf['day']}->{kl['day']}")
    klog = out / "k10.log"
    if klog.exists():
        text = klog.read_text(encoding="utf-8", errors="replace")
        hits = {name: len(re.findall(p, text)) for name, p in K10_PATTERNS.items()}
        print("K10 log: " + ", ".join(f"{k}={v}" for k, v in hits.items() if v))
        apsta = [int(m) for m in re.findall(r"apSta=(\d+)", text)]
        ihe = [int(m) for m in re.findall(r"imin=(\d+)KB", text)]
        if apsta:
            print(f"  softAP stations seen {sorted(set(apsta))}; internal heap low-water {min(ihe, default=0)} KB")
        # The radio's own timeline against the worst tick gap each report saw:
        # a scan that takes the radio off the softAP's channel lines up with a
        # gap on every seat at once.
        t0 = rows[0]["_ts"] - REPORT_S
        radio = [ln for ln in text.splitlines()
                 if re.search(r"WIFI AWAY|Boot STA|AutoJoin (scan|try|no known|connected)|WIFI STA disc", ln)]
        bad = [(r["_ts"], max(s["mg"] for s in r["seats"])) for r in rows]
        bad = [(ts, mg) for ts, mg in bad if mg >= 700]
        if radio or bad:
            print("  timeline (s from window start; radio events and reports with a tick gap >= 700 ms):")
            items = [(float(ln.split(" ", 1)[0]), "radio  " + ln.split("] ", 2)[-1][:90]) for ln in radio]
            items += [(ts, f"gap    worst tick gap in the 10 s before: {mg} ms") for ts, mg in bad]
            for ts, what in sorted(items):
                if ts >= t0:
                    print(f"   {ts - t0:7.1f}  {what}")
    ev = [ln for ln in (out / "wroom.log").read_text(encoding="utf-8", errors="replace").splitlines()
          if " E " in ln and "joined" not in ln and "cmd=" not in ln]
    if ev:
        print(f"wroom events ({len(ev)}):")
        for ln in ev[:25]:
            print("  " + ln)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wroom", default="COM3")
    ap.add_argument("--k10", default="COM5", help='K10 serial port, "" to skip')
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--out", default="runs/swarm-" + time.strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--cmd", action="append", default=[], help="wroom command to send at the start")
    ap.add_argument("--end-cmd", action="append", default=[], help="wroom command to send at the end")
    ap.add_argument("--k10-cmd", action="append", default=[],
                    help='K10 serial console command to send at the start ("away")')
    ap.add_argument("--no-zero", dest="zero", action="store_false")
    ap.add_argument("--zero-after", type=float, default=0,
                    help="seconds before the wroom's counters are zeroed (let a K10 reboot settle first)")
    ap.add_argument("--note", default="")
    ap.add_argument("--summarize", metavar="DIR")
    args = ap.parse_args()
    if args.summarize:
        summarize(Path(args.summarize))
        return
    summarize(record(args))


if __name__ == "__main__":
    sys.exit(main())
