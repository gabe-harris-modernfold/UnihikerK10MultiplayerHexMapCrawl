"""World log -- record a live game for analysis and debugging.

record.py logs a bot run, but only while bots play and only from each bot's
side of the wire.  This records the WORLD: whoever is playing -- people on
phones, bots, both -- from outside, without picking a survivor.

    python worldlog.py record --host k10.local         # Ctrl-C to stop
    python worldlog.py summary [log]                   # newest runs/world-*.jsonl
    python worldlog.py events  [log] --pid 2 --kind dmg,left
    python worldlog.py track   [log] --pid 2 > p2.csv  # vitals at every poll
    python worldlog.py at      [log] --day 12 --tick 300 [--pid 2]

## Two sources

**GET /state every --poll seconds.**  Read-only and seat-free; the observer
TV lives on it.  Each poll is diffed against the last: the whole snapshot
("state_key") on the first poll, after a reboot and every --keyframe
seconds, and a {"set", "del"} delta ("state_delta") in between.  Each poll
also asks for one seated survivor's vision disk (?pid=, taking turns), and
the cells that are new or changed go into a "cells" row -- over a session
that rebuilds the map wherever anyone has looked.  The whole map is only
ever in `sync`, which is unicast after a pick, so nothing that stays out of
a seat can have it.

**/ws as a spectator.**  Every `ev` goes out with ws.textAll() to every
socket, seated or in the lobby (bot-testing.md, protocol rule 5), stamped
with its "sq".  The recorder connects, never picks, and keeps every message
except the 10 Hz `s` tick (/state covers it), `eco` (~9 KB every 5 s and
cosmetic by contract; /state carries its summary) and `wifi`.  They are
written as ch "rx", the shape of a bot run, so causes.py reads them.

Three things about that socket matter:

* **It costs a seat.**  handleConnect admits a socket while
  connected + lobby < MAX_SEATED (network-session.hpp), and a socket that
  never picks stays in the lobby.  With the recorder in, a fifth player is
  refused with {"t":"full"}.  So by default it steps out when /state shows
  MAX_SEATED-1 seated and comes back at MAX_SEATED-2; "ws_yield" and
  "ws_back" bound the gap, and /state is recorded throughout.  --hold-seat
  keeps it in regardless.
* **`downed` is unicast** to the one who fell.  Everyone else gets the
  `left` that follows it, with the same "cause".  Here a death is a `left`
  with a cause; a `left` without one is someone walking away.
* **An sq that never arrives is not proof of loss.**  Unicast events
  (tun_taunt, kane_call, trap, ...) take sq numbers too.  The summary counts
  them as "unseen"; real loss on the board is /state's `evtDrops`.

## Rows (record.py's {"ts", "ch", "arch", "d"}, ts = seconds since start)

  run           meta first (host, wall clock, settings), the summary last
  state_key     the whole /state minus "view", "wt" and each player's "near"
  state_delta   {"set": {"players.2.ll": 4, ...}, "del": ["players.2.encQ"]}
  cells         arch = pid, d = [[q, r, terrain, variant, shelter, res, amount, poi], ...]
  rx            a WS message (ev, lobby, item_result, ...), verbatim
  board_reboot  uptimeMs went backwards: {"reset", "crash", "uptimeMs_before"}
  poll_err      a failed /state (logged on the first of a run and every 10th)
  ws_conn / ws_close / ws_err / ws_full / ws_yield / ws_back
"""
import argparse
import asyncio
import copy
import csv
import datetime
import json
import sys
import time
import urllib.request
from collections import Counter
from pathlib import Path

import websockets

from config import ARCHETYPE_NAME, MAP_COLS, MAP_ROWS, MAX_PLAYERS, MAX_SEATED, WEATHER_NAME
from record import Recorder

RUNS = Path(__file__).resolve().parent / "runs"

# WS message types not written: the per-tick broadcast, the vein grid, and
# the board's Wi-Fi chatter.  Counted, so the summary shows they arrived.
SKIP_WS = frozenset(("s", "eco", "wifi"))
STATUS_EVERY_S = 30.0
FULL_RETRY_S = 30.0


# ── /state snapshots: strip, diff, apply ─────────────────────────────────────

def fetch_state(host: str, pid=None, timeout: float = 5.0) -> dict:
    url = f"http://{host}/state" + (f"?pid={pid}" if pid is not None else "")
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def strip_state(state: dict):
    """(snapshot to log, the ?pid= view or None).  `wt` is millis() and
    changes on every read; `near` is 19 hexes per survivor that move with
    them, and the cells rows already carry the ground."""
    s = dict(state)
    view = s.pop("view", None)
    s.pop("wt", None)
    players = s.get("players")
    if isinstance(players, list):
        s["players"] = [{k: v for k, v in p.items() if k != "near"}
                        if isinstance(p, dict) else p for p in players]
    return s, view


def _join(path: str, key) -> str:
    return f"{path}.{key}" if path else str(key)


def _diff(a, b, path, sets, dels):
    if isinstance(a, dict) and isinstance(b, dict):
        if any("." in str(k) for k in (*a, *b)):
            if a != b:                       # a key a path cannot address
                sets[path] = b
            return
        for k, v in b.items():
            if k in a:
                _diff(a[k], v, _join(path, k), sets, dels)
            else:
                sets[_join(path, k)] = v
        dels.extend(_join(path, k) for k in a if k not in b)
        return
    if (isinstance(a, list) and isinstance(b, list) and len(a) == len(b)
            and any(isinstance(x, (dict, list)) for x in b)):
        for i, (x, y) in enumerate(zip(a, b)):
            _diff(x, y, _join(path, i), sets, dels)
        return
    # type() as well: True == 1 in Python, and the wire has both.
    if a != b or type(a) is not type(b):
        sets[path] = b


def diff_state(old: dict, new: dict):
    """(sets, dels) turning `old` into `new`.  Dicts recurse by key, lists of
    containers by index when the length holds; anything else is replaced
    whole, so a changed invType is one entry, not eighteen."""
    sets, dels = {}, []
    _diff(old, new, "", sets, dels)
    return sets, dels


def _walk(obj, seg):
    return obj[int(seg)] if isinstance(obj, list) else obj[seg]


def apply_delta(state: dict, sets: dict, dels=()) -> dict:
    """diff_state() backwards.  Mutates and returns `state`; values are
    copied in, so the rows it came from stay as they were read."""
    for path, v in sets.items():
        if path == "":
            state = copy.deepcopy(v)
            continue
        segs = path.split(".")
        cur = state
        for seg in segs[:-1]:
            cur = _walk(cur, seg)
        last = segs[-1]
        if isinstance(cur, list):
            cur[int(last)] = copy.deepcopy(v)
        else:
            cur[last] = copy.deepcopy(v)
    for path in dels:
        segs = path.split(".")
        cur = state
        for seg in segs[:-1]:
            cur = _walk(cur, seg)
        if isinstance(cur, dict):
            cur.pop(segs[-1], None)
    return state


def view_cells(view: dict) -> dict:
    """{(q, r): (terrain, variant, shelter, resource, amount, poi)} from a
    /state ?pid= disk.  Footprints and tire tracks are left out (every step
    changes them, and `mv` events already say who went where), and so is
    `trap`, which is per-viewer knowledge and would flip between viewers."""
    out = {}
    for c in view.get("cells", ()):
        try:
            out[(c["q"], c["r"])] = (c.get("terrain", 0), c.get("variant", 0),
                                     c.get("shelter", 0), c.get("resource", 0),
                                     c.get("amount", 0), 1 if c.get("poi") else 0)
        except (KeyError, TypeError):
            continue
    return out


def seated_pids(state: dict) -> list:
    return [p.get("pid", i) for i, p in enumerate(state.get("players") or ())
            if isinstance(p, dict) and p.get("conn")]


# ── The recorder ─────────────────────────────────────────────────────────────

class WorldRecorder:
    def __init__(self, host, out, poll=2.0, keyframe=60.0, use_ws=True,
                 hold_seat=False, view=True, duration=None, quiet=False):
        self.host = host
        self.out = Path(out)
        self.poll = poll
        self.keyframe = keyframe
        self.use_ws = use_ws
        self.hold_seat = hold_seat
        self.view = view
        self.duration = duration
        self.quiet = quiet
        self.t0 = time.monotonic()
        self.rec = Recorder(self.out, {
            "kind": "worldlog", "host": host,
            "started": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
            "started_epoch": round(time.time(), 3),
            "poll_s": poll, "keyframe_s": keyframe, "ws": use_ws,
            "hold_seat": hold_seat, "view": view})
        self.stop = asyncio.Event()
        self.first_poll = asyncio.Event()
        self.may_connect = asyncio.Event()
        self.may_connect.set()
        self.n = Counter()              # everything countable, for the summary
        self.prev = None                # last logged (stripped) snapshot
        self.prev_up = None
        self.last_key = None
        self.cells = {}                 # (q, r) -> view_cells() tuple
        self.view_turn = 0
        self.seated = []
        self.day = None
        self.pv = None
        self.poll_err_run = 0
        self.ws = None
        self.ws_up_since = None
        self.ws_up_s = 0.0
        self.yielded = False
        self.last_sq = None
        self.min_heap = None
        self.max_tick = 0
        self.evt_drops = [None, None]   # first, last

    def _say(self, text):
        if not self.quiet:
            el = int(time.monotonic() - self.t0)
            print(f"[{el // 3600:02d}:{el // 60 % 60:02d}:{el % 60:02d}] {text}", flush=True)

    # -- /state ----------------------------------------------------------
    def _next_view_pid(self):
        if not self.view or not self.seated:
            return None
        pid = self.seated[self.view_turn % len(self.seated)]
        self.view_turn += 1
        return pid

    async def poll_loop(self):
        while not self.stop.is_set():
            started = time.monotonic()
            pid = self._next_view_pid()
            try:
                st = await asyncio.to_thread(fetch_state, self.host, pid)
                if "error" in st and "players" not in st:
                    raise RuntimeError(st["error"])         # mutex timeout
            except Exception as e:
                self.n["poll_errs"] += 1
                self.poll_err_run += 1
                if self.poll_err_run == 1 or self.poll_err_run % 10 == 0:
                    self.rec.write("poll_err", -1, {"err": f"{type(e).__name__}: {e}",
                                                    "run": self.poll_err_run})
                    self._say(f"/state failed ({type(e).__name__}: {e}), x{self.poll_err_run}")
            else:
                if self.poll_err_run >= 10:
                    self._say(f"/state answering again after {self.poll_err_run} failures")
                self.poll_err_run = 0
                self._on_state(st, pid)
                self.first_poll.set()
            wait = max(0.05, self.poll - (time.monotonic() - started))
            try:
                await asyncio.wait_for(self.stop.wait(), timeout=wait)
            except asyncio.TimeoutError:
                pass

    def _on_state(self, st, pid):
        self.n["polls"] += 1
        s, view = strip_state(st)
        mem = s.get("mem") or {}
        up = mem.get("uptimeMs")
        reboot = (self.prev_up is not None and up is not None and up < self.prev_up)
        if reboot:
            boot = s.get("boot") or {}
            rb = {"reset": boot.get("reset"), "crash": boot.get("crash"),
                  "uptimeMs_before": self.prev_up}
            self.n["reboots"] += 1
            self.rec.write("board_reboot", -1, rb)
            self._say(f"BOARD REBOOTED (reset {rb['reset']})")
            self.evt_drops[0] = None        # a high-water count, reset by the boot
        self.prev_up = up
        now = time.monotonic()
        if self.prev is None or reboot or now - self.last_key >= self.keyframe:
            self.rec.write("state_key", -1, s)
            self.last_key = now
            self.n["keyframes"] += 1
        else:
            sets, dels = diff_state(self.prev, s)
            if sets or dels:
                d = {"set": sets}
                if dels:
                    d["del"] = dels
                self.rec.write("state_delta", -1, d)
                self.n["deltas"] += 1
        self.prev = s
        self.seated = seated_pids(s)
        self.day = s.get("day", self.day)
        self.pv = s.get("pv", self.pv)
        if mem.get("minHeap") is not None:
            self.min_heap = mem["minHeap"] if self.min_heap is None else min(self.min_heap, mem["minHeap"])
        self.max_tick = max(self.max_tick, mem.get("maxTickMs") or 0)
        if s.get("evtDrops") is not None:
            if self.evt_drops[0] is None:
                self.evt_drops[0] = s["evtDrops"]
            self.evt_drops[1] = s["evtDrops"]
        if view and pid is not None and view.get("pid") == pid:
            self._on_view(s, view, pid)
        self._seat_policy()

    def _on_view(self, s, view, pid):
        # The disk is read off the surface map at the player's q/r; below
        # ground those are tunnel coordinates, so it would paint the wrong hexes.
        players = s.get("players") or []
        p = players[pid] if 0 <= pid < len(players) else {}
        if p.get("dp"):
            return
        changed = [[q, r, *c] for (q, r), c in view_cells(view).items()
                   if self.cells.get((q, r)) != c]
        for q, r, *c in changed:
            self.cells[(q, r)] = tuple(c)
        if changed:
            self.rec.write("cells", pid, changed)
            self.n["cells_rows"] += 1

    def _seat_policy(self):
        if not self.use_ws or self.hold_seat:
            return
        k = len(self.seated)
        if not self.yielded and k >= MAX_SEATED - 1:
            self.yielded = True
            self.may_connect.clear()
            self.n["yields"] += 1
            self.rec.write("ws_yield", -1, {"seated": k})
            self._say(f"{k} seated: leaving the lobby so a {MAX_SEATED}th can join "
                      "(/state still recorded; --hold-seat to stay)")
            if self.ws is not None:
                asyncio.ensure_future(self.ws.close())
        elif self.yielded and k <= MAX_SEATED - 2:
            self.yielded = False
            self.rec.write("ws_back", -1, {"seated": k})
            self._say(f"{k} seated: back on /ws")
            self.may_connect.set()

    # -- /ws -------------------------------------------------------------
    async def ws_loop(self):
        await self.first_poll.wait()
        backoff = 1.0
        url = f"ws://{self.host}/ws"
        while not self.stop.is_set():
            await self.may_connect.wait()
            full = False
            reason = "peer_closed"
            try:
                async with websockets.connect(url, open_timeout=10, ping_interval=20,
                                              ping_timeout=20, max_size=2 ** 20) as ws:
                    self.ws = ws
                    self.ws_up_since = time.monotonic()
                    self.n["ws_connects"] += 1
                    self.rec.write("ws_conn", -1, {"n": self.n["ws_connects"],
                                                   "last_sq": self.last_sq})
                    if self.n["ws_connects"] == 1:
                        self._say(f"on /ws as a spectator ({url})")
                    async for raw in ws:
                        if self.yielded:            # stepped out mid-handshake
                            break
                        if self._on_ws(raw):
                            full = True
                            reason = "full"
                            break
                    if self.yielded:
                        reason = "yield"
                    backoff = 1.0
            except asyncio.CancelledError:
                reason = "stopped"
                raise
            except Exception as e:
                reason = f"{type(e).__name__}"
                self.n["ws_errs"] += 1
                self.rec.write("ws_err", -1, {"err": f"{type(e).__name__}: {e}"})
            finally:
                if self.ws_up_since is not None:
                    self.ws_up_s += time.monotonic() - self.ws_up_since
                    self.ws_up_since = None
                    self.rec.write("ws_close", -1, {"why": reason, "last_sq": self.last_sq})
                self.ws = None
            if self.stop.is_set():
                break
            if full:
                self.n["ws_full"] += 1
                self.rec.write("ws_full", -1, {"seated": len(self.seated)})
                self._say(f"/ws refused (board full); retrying in {FULL_RETRY_S:.0f} s")
                delay = FULL_RETRY_S
            elif reason == "yield":
                continue
            else:
                delay = backoff
                backoff = min(backoff * 2, 15.0)
            try:
                await asyncio.wait_for(self.stop.wait(), timeout=delay)
            except asyncio.TimeoutError:
                pass

    def _on_ws(self, raw) -> bool:
        """Log one message; True when the board said `full`."""
        try:
            msg = json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            self.n["rx_bad"] += 1
            self.rec.write("rx_bad", -1, {"raw": str(raw)[:200]})
            return False
        if not isinstance(msg, dict):
            return False
        t = msg.get("t")
        if t == "full":
            return True
        self.n[f"t:{t}"] += 1
        if t in SKIP_WS:
            return False
        if t == "ev":
            sq = msg.get("sq")
            if isinstance(sq, int):
                if self.last_sq is None or sq > self.last_sq:
                    if self.last_sq is not None and self.ws_up_since is not None:
                        self.n["sq_unseen"] += sq - self.last_sq - 1
                    self.last_sq = sq
                elif self.last_sq - sq > 1000:           # the board rebooted
                    self.last_sq = sq
        self.rec.write("rx", -1, msg)
        return False

    # -- running ---------------------------------------------------------
    async def status_loop(self):
        while not self.stop.is_set():
            try:
                await asyncio.wait_for(self.stop.wait(), timeout=STATUS_EVERY_S)
            except asyncio.TimeoutError:
                ws = "off" if not self.use_ws else (
                    "yielded" if self.yielded else ("on" if self.ws is not None else "down"))
                self._say(f"day {self.day}  seated {len(self.seated)}  polls {self.n['polls']}"
                          f"  events {self.n['t:ev']}  cells {len(self.cells)}  ws {ws}")

    async def _timer(self):
        try:
            await asyncio.wait_for(self.stop.wait(), timeout=self.duration)
        except asyncio.TimeoutError:
            self.stop.set()

    async def run(self):
        self._say(f"recording {self.host} -> {self.out}")
        tasks = [asyncio.create_task(self.poll_loop()),
                 asyncio.create_task(self.status_loop())]
        if self.use_ws:
            tasks.append(asyncio.create_task(self.ws_loop()))
        if self.duration:
            tasks.append(asyncio.create_task(self._timer()))
        try:
            await self.stop.wait()
        finally:
            self.stop.set()
            if self.ws is not None:
                try:
                    await asyncio.wait_for(self.ws.close(), timeout=2)
                except Exception:
                    pass
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            self.rec.close(self.summary())

    def summary(self) -> dict:
        n = dict(self.n)
        d0, d1 = self.evt_drops
        return {"end": True, "ran_s": round(time.monotonic() - self.t0, 1),
                "pv": self.pv, "last_day": self.day, "counts": n,
                "ws_up_s": round(self.ws_up_s, 1), "cells_known": len(self.cells),
                "min_heap": self.min_heap, "max_tick_ms": self.max_tick,
                "evt_drops": (d1 - d0) if d0 is not None and d1 is not None else None,
                "lines": self.rec.lines}


# ── Reading a log back ───────────────────────────────────────────────────────

def newest_log() -> Path:
    logs = sorted(RUNS.glob("world-*.jsonl"), key=lambda p: p.stat().st_mtime)
    if not logs:
        sys.exit(f"no world logs in {RUNS}; record one with: python worldlog.py record --host <board>")
    return logs[-1]


def load(path) -> list:
    """Every complete row.  A killed recorder leaves a half line at the end;
    it is skipped, not fatal."""
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def replay(rows):
    """Yield (row, state) for every row, `state` being the /state rebuilt as
    of that row (None before the first keyframe).  The same dict is updated
    in place as the log goes on: copy it to keep one."""
    state = None
    for r in rows:
        ch = r.get("ch")
        if ch == "state_key":
            state = copy.deepcopy(r["d"])
        elif ch == "state_delta" and state is not None:
            state = apply_delta(state, r["d"].get("set", {}), r["d"].get("del", ()))
        yield r, state


def meta_of(rows) -> dict:
    for r in rows:
        if r.get("ch") == "run" and r["d"].get("kind") == "worldlog":
            return r["d"]
    return {}


def clock(ts: float) -> str:
    ts = int(ts)
    return f"+{ts // 3600:d}:{ts // 60 % 60:02d}:{ts % 60:02d}"


def label(p: dict, pid) -> str:
    arch = p.get("archName") or (ARCHETYPE_NAME[pid] if 0 <= pid < len(ARCHETYPE_NAME) else "?")
    name = p.get("name")
    return f"p{pid} {arch}" + (f" ({name})" if name and name != arch else "")


def who(state, pid) -> str:
    if not isinstance(pid, int) or pid < 0:
        return "-"
    players = (state or {}).get("players") or []
    p = players[pid] if pid < len(players) and isinstance(players[pid], dict) else {}
    return label(p, pid)


def when(state) -> str:
    if not state:
        return "d? t?"
    return f"d{state.get('day', '?')} t{state.get('dayTick', '?')}"


def _fmt_ev(d) -> str:
    rest = {k: v for k, v in d.items() if k not in ("t", "k", "pid", "sq")}
    return "  ".join(f"{k}={json.dumps(v, separators=(',', ':')) if isinstance(v, (dict, list)) else v}"
                     for k, v in rest.items())


def cmd_summary(path, rows):
    meta = meta_of(rows)
    end = next((r["d"] for r in reversed(rows) if r.get("ch") == "run" and r["d"].get("end")), {})
    ran = rows[-1]["ts"] if rows else 0
    first_day = last_day = None
    seen = {}                           # pid -> last player block while seated
    heap = tick = None
    drops = []
    deaths, quits, kinds = [], Counter(), Counter()
    ws_up, ws_open, yields, fulls, reboots, poll_errs = 0.0, None, 0, 0, [], 0
    cells = set()
    state_rows, pv = 0, None            # fallbacks for a log with no closing row
    for r, st in replay(rows):
        ch = r.get("ch")
        if ch in ("state_key", "state_delta") and st:
            state_rows += 1
            pv = st.get("pv", pv)
            day = st.get("day")
            if day is not None:
                first_day = day if first_day is None else first_day
                last_day = day
            for p in st.get("players") or ():
                if isinstance(p, dict) and p.get("conn"):
                    seen[p.get("pid")] = dict(p)
            mem = st.get("mem") or {}
            if mem.get("minHeap") is not None:
                heap = mem["minHeap"] if heap is None else min(heap, mem["minHeap"])
            tick = max(tick or 0, mem.get("maxTickMs") or 0)
            if st.get("evtDrops") is not None:
                drops.append(st["evtDrops"])
        elif ch == "rx" and r["d"].get("t") == "ev":
            d = r["d"]
            kinds[d.get("k")] += 1
            if d.get("k") == "left":
                # EVT_DOWNED's left always carries "cause" (dcName()); a quit's
                # never does.  The mock's dbg_die leaves it off, so reads as a quit.
                if "cause" in d:
                    deaths.append((r["ts"], when(st), who(st, d.get("pid")), d["cause"] or "?"))
                else:
                    quits[who(st, d.get("pid"))] += 1
        elif ch == "cells":
            cells.update((c[0], c[1]) for c in r["d"])
        elif ch == "ws_conn":
            ws_open = r["ts"]
        elif ch == "ws_close" and ws_open is not None:
            ws_up += r["ts"] - ws_open
            ws_open = None
        elif ch == "ws_yield":
            yields += 1
        elif ch == "ws_full":
            fulls += 1
        elif ch == "board_reboot":
            reboots.append((r["ts"], r["d"].get("reset")))
        elif ch == "poll_err":
            poll_errs = max(poll_errs, r["d"].get("run", 1))
    if ws_open is not None:
        ws_up += ran - ws_open

    print(f"world log  {path}")
    print(f"  host {meta.get('host', '?')}   started {meta.get('started', '?')}   "
          f"ran {clock(ran)}   protocol {end.get('pv', pv) if end else pv}")
    if not end:
        print("  (no closing row: the recorder was killed, so counts come from the rows;"
              " the last second or two may be missing)")
    counts = end.get("counts", {})
    polls = counts["polls"] if "polls" in counts else f"~{state_rows}"
    failed = counts.get("poll_errs", poll_errs)
    print(f"  polls {polls} every {meta.get('poll_s', '?')} s "
          f"({failed} failed)   days {first_day} -> {last_day}")
    print(f"  board  reboots {len(reboots)}   min heap "
          f"{'?' if heap is None else f'{heap // 1024} KB'}   worst tick {'?' if tick is None else tick} ms   "
          f"evtDrops +{(drops[-1] - drops[0]) if len(drops) > 1 and not reboots else '?'}")
    for ts, why in reboots:
        print(f"    {clock(ts)} reboot, reset {why}")
    if meta.get("ws"):
        unseen = counts.get("sq_unseen")
        print(f"  ws     on {clock(ws_up)} of {clock(ran)}   yields {yields}   refused {fulls}"
              + (f"   sq unseen {unseen} (unicast events + any loss)" if unseen is not None else ""))
    else:
        print("  ws     off (--no-ws): no events, /state only")
    print(f"  map    {len(cells)} of {MAP_COLS * MAP_ROWS} surface cells seen")
    if seen:
        print("  survivors")
        for pid in sorted(seen, key=lambda x: (x is None, x)):
            p = seen[pid]
            print(f"    {label(p, pid if isinstance(pid, int) else -1):34s} "
                  f"score {p.get('score', '?'):>4}  steps {p.get('steps', '?'):>4}  "
                  f"last LL {p.get('ll', '?')}/{p.get('llCap', '?')}")
    if deaths:
        print(f"  deaths ({len(deaths)})")
        for ts, w, name, cause in deaths:
            print(f"    {clock(ts)}  {w:12s} {name:34s} {cause}")
    if quits:
        print("  walked away  " + ", ".join(f"{k} x{v}" for k, v in quits.items()))
    try:
        from causes import DamageLedger
        ledger = DamageLedger(rows).loss_ledger()
    except Exception as e:                                  # analysis is best effort
        ledger = {"(causes.py failed)": str(e)}
    if ledger:
        print("  LL lost by cause  " + ", ".join(
            f"{c} {v:g}" if isinstance(v, (int, float)) else f"{c} {v}"
            for c, v in sorted(ledger.items(), key=lambda cv: -cv[1] if isinstance(cv[1], (int, float)) else 0)))
    if kinds:
        print("  events  " + ", ".join(f"{k} {n}" for k, n in kinds.most_common()))


def cmd_events(rows, pid=None, kinds=None, grep=None):
    for r, st in replay(rows):
        if r.get("ch") != "rx" or r["d"].get("t") != "ev":
            continue
        d = r["d"]
        if pid is not None and d.get("pid") != pid:
            continue
        if kinds and d.get("k") not in kinds:
            continue
        line = f"{clock(r['ts'])}  {when(st):12s} {who(st, d.get('pid')):28s} {d.get('k', '?'):12s} {_fmt_ev(d)}"
        if grep and grep.lower() not in line.lower():
            continue
        print(line)


TRACK_COLS = ("ts", "wall", "day", "dayTick", "weather", "pid", "name", "arch", "conn",
              "q", "r", "dp", "ll", "llCap", "food", "water", "rad", "ap", "score",
              "steps", "resting", "encActive", "wound_minor", "wound_major")


def cmd_track(rows, pid=None, out=sys.stdout):
    meta = meta_of(rows)
    t0 = meta.get("started_epoch")
    w = csv.writer(out, lineterminator="\n")
    w.writerow(TRACK_COLS)
    for r, st in replay(rows):
        if r.get("ch") not in ("state_key", "state_delta") or not st:
            continue
        wp = st.get("weather")
        wname = WEATHER_NAME[wp] if isinstance(wp, int) and 0 <= wp < len(WEATHER_NAME) else wp
        wall = (datetime.datetime.fromtimestamp(t0 + r["ts"]).isoformat(timespec="seconds")
                if t0 else "")
        for p in st.get("players") or ():
            if not isinstance(p, dict):
                continue
            if pid is None and not p.get("conn"):
                continue
            if pid is not None and p.get("pid") != pid:
                continue
            wounds = p.get("wounds") or [0, 0]
            w.writerow([r["ts"], wall, st.get("day"), st.get("dayTick"), wname,
                        p.get("pid"), p.get("name"), p.get("archName"), int(bool(p.get("conn"))),
                        p.get("q"), p.get("r"), p.get("dp"), p.get("ll"), p.get("llCap"),
                        p.get("food"), p.get("water"), p.get("rad"), p.get("ap"),
                        p.get("score"), p.get("steps"), int(bool(p.get("resting"))),
                        int(bool(p.get("encActive"))), wounds[0], wounds[1]])


def cmd_at(rows, ts=None, day=None, tick=0, pid=None):
    """The rebuilt /state at the first poll at or after the moment asked for."""
    found = None
    for r, st in replay(rows):
        if r.get("ch") not in ("state_key", "state_delta") or not st:
            continue
        if ts is not None:
            hit = r["ts"] >= ts
        else:
            d = st.get("day")
            hit = d is not None and (d > day or (d == day and (st.get("dayTick") or 0) >= tick))
        if hit:
            found = (r["ts"], copy.deepcopy(st))
            break
    if found is None:
        sys.exit("the log never reaches that moment")
    at_ts, st = found
    print(f"# {clock(at_ts)}  {when(st)}", file=sys.stderr)
    if pid is not None:
        players = st.get("players") or []
        st = players[pid] if 0 <= pid < len(players) else {}
    print(json.dumps(st, indent=1))


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    rp = sub.add_parser("record", help="record a live board until Ctrl-C")
    rp.add_argument("--host", default="k10.local")
    rp.add_argument("--out", help="default runs/world-<date>-<time>.jsonl")
    rp.add_argument("--poll", type=float, default=2.0, help="seconds between /state reads")
    rp.add_argument("--keyframe", type=float, default=60.0, help="seconds between full snapshots")
    rp.add_argument("--duration", type=float, help="stop after this many seconds")
    rp.add_argument("--no-ws", action="store_true", help="/state only: no events, no seat used")
    rp.add_argument("--hold-seat", action="store_true",
                    help=f"never leave the lobby, even when it keeps a {MAX_SEATED}th player out")
    rp.add_argument("--no-view", action="store_true", help="skip the ?pid= vision disks (no cells rows)")
    rp.add_argument("--quiet", action="store_true")

    for name, hlp in (("summary", "what happened, in one screen"),
                      ("events", "every event, with day/tick and who"),
                      ("track", "CSV of survivor vitals at every poll"),
                      ("at", "the rebuilt /state at a moment")):
        p = sub.add_parser(name, help=hlp)
        p.add_argument("log", nargs="?", help="default: the newest runs/world-*.jsonl")
        if name in ("events", "track", "at"):
            p.add_argument("--pid", type=int)
        if name == "events":
            p.add_argument("--kind", help="comma-separated event kinds, e.g. dmg,left,enc_res")
            p.add_argument("--grep", help="only lines containing this text")
        if name == "at":
            g = p.add_mutually_exclusive_group(required=True)
            g.add_argument("--ts", type=float, help="seconds since the recording started")
            g.add_argument("--day", type=int)
            p.add_argument("--tick", type=int, default=0, help="dayTick within --day")

    a = ap.parse_args(argv)
    if a.cmd == "record":
        out = a.out or RUNS / f"world-{datetime.datetime.now():%Y%m%d-%H%M%S}.jsonl"
        wr = WorldRecorder(a.host, out, poll=a.poll, keyframe=a.keyframe,
                           use_ws=not a.no_ws, hold_seat=a.hold_seat, view=not a.no_view,
                           duration=a.duration, quiet=a.quiet)
        try:
            asyncio.run(wr.run())
        except KeyboardInterrupt:
            pass
        print(f"wrote {wr.rec.lines} rows to {out}")
        return 0

    path = Path(a.log) if a.log else newest_log()
    rows = load(path)
    if a.cmd == "summary":
        cmd_summary(path, rows)
    elif a.cmd == "events":
        cmd_events(rows, pid=a.pid, kinds=set(a.kind.split(",")) if a.kind else None, grep=a.grep)
    elif a.cmd == "track":
        cmd_track(rows, pid=a.pid)
    elif a.cmd == "at":
        cmd_at(rows, ts=a.ts, day=a.day, tick=a.tick, pid=a.pid)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:             # `... | more` closed early
        pass
