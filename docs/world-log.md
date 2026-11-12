# World log — recording a live game

[bots/worldlog.py](../bots/worldlog.py) records whatever is happening on a
board — people on phones, bots, or both — from outside, without picking a
survivor and without changing the firmware. Use it to keep a session for
analysis, or to have a record behind a bug report. The output is one JSONL
file per recording, plus four commands that read it back.

**Status (2026-11-07):** tested on the mock, then on the K10 for one 4.5
minute solo session (protocol 16, days 64→77). Events, deltas and cells all
landed, and the `dawn` events matched the LL in the snapshots poll for
poll. It has not been tested with several players or through a seat yield
on hardware.

---

## For AI coding agents (read first)

- **Record:** `cd bots && python worldlog.py record --host k10.local`.
  Ctrl-C (or `--duration N`) stops it; the output is
  `bots/runs/world-<date>-<time>.jsonl` (gitignored).
- **Read:** `python worldlog.py summary` (newest log), then `events`,
  `track` or `at` (below). Each takes a log path as an optional first
  argument.
- **It is not `record.py`.** `record.py` logs a bot run from each bot's side
  and needs `arena.py`. `worldlog.py` needs nothing playing in particular and
  sees the whole world. The rows have the same shape, so `causes.py` reads
  both.
- **Four silent traps** (details under "Gotchas"):
  1. The spectator socket uses up a seat, so the recorder leaves `/ws` at 4
     seated. Events stop for that stretch; `/state` doesn't.
  2. A death shows up as `left` with a `cause`, never as `downed`.
  3. A missing `sq` number does not prove an event was lost.
  4. On the mock, one seat is always taken by the app's browser pane, and
     `dbg_die` deaths look like players walking away.
- **Deps:** Python 3.12 + `websockets` (the same as the rest of `bots/`).
- **Self-test:** `python smoke.py` includes a `worldlog` section (diff/apply
  round trip, replay, cell decoding). Run it after any change.

---

## Commands

```
python worldlog.py record --host k10.local [--poll 2] [--keyframe 60] [--duration S]
                          [--no-ws] [--hold-seat] [--no-view] [--quiet] [--out FILE]
python worldlog.py summary [log]
python worldlog.py events  [log] [--pid N] [--kind dmg,left,enc_res] [--grep TEXT]
python worldlog.py track   [log] [--pid N]  > vitals.csv
python worldlog.py at      [log] (--ts SECONDS | --day D [--tick T]) [--pid N]
```

| Command | Output |
|---|---|
| `record` | Prints a status line every 30 s (day, seated, polls, events, cells, ws state) and announces reboots, `/state` failures and seat yields. |
| `summary` | Host, how long it ran, protocol, days covered, board health (reboots, min heap, worst tick, `evtDrops` growth), how long it was on `/ws`, map coverage, the survivors seen, deaths with causes, players who walked away, LL lost by cause (`causes.DamageLedger`) and event counts. |
| `events` | One line per event: `+h:mm:ss  dDAY tTICK  pN Arch (name)  kind  key=value ...`. Day/tick come from the last poll *before* the event, so they can be up to `--poll` s old. |
| `track` | CSV, one row per poll per seated survivor (every row for `--pid`): ts, wall clock, day, dayTick, weather, position, depth, LL/cap, food, water, rad, AP, score, steps, resting, encounter, wounds. |
| `at` | The rebuilt `/state` JSON at the first poll at or after the moment asked for (`--pid` prints just that player). The moment it found goes to stderr. |

`record` flags:

| Flag | Default | Effect |
|---|---|---|
| `--host` | `k10.local` | Board address (mDNS works on Windows). The mock is `localhost:8765` (or whatever port `preview_start` gave it). |
| `--poll` | 2 | Seconds between `/state` reads. The observer TV reads it every second; don't go below that. |
| `--keyframe` | 60 | Seconds between full snapshots. Smaller = faster `at`, bigger file. |
| `--no-ws` | off | `/state` only: no events and no lobby seat used. |
| `--hold-seat` | off | Never leave the lobby. Fine for sessions of 4 or fewer, but it shuts out a 5th player. |
| `--no-view` | off | Skip the `?pid=` vision disks (no `cells` rows), for a slightly lighter `/state` read. |

---

## How it works

Two tasks write into one [record.py](../bots/record.py) `Recorder`.

**1. `GET /state` every `--poll` s** ([game-server.hpp:890](../game-server.hpp:890)).
This is read-only and uses no seat. Each response goes through
`strip_state()` (drops `view`, `wt`, and each player's `near`), then:

- On the first poll, after a reboot (`mem.uptimeMs` went backwards), and
  every `--keyframe` s, the whole snapshot is written as `state_key`.
- Otherwise `diff_state(prev, now)` is written as `state_delta`.
  - Dicts recurse by key. Lists whose items are dicts or lists recurse by
    index, as long as the length is unchanged.
  - Anything else is replaced whole, so a changed `invType` is one entry,
    not eighteen.
  - The type is compared too, so a value going from `1` to `true` still
    counts as a change.
- Each poll also asks for one seated survivor's vision disk (`?pid=`,
  taking turns). Cells that are new or changed against an in-memory cache
  go into a `cells` row. Over a session this rebuilds the map wherever
  anyone has looked.
  - Footprints, tire tracks and `trap` are left out of the cells: the first
    two change on every step, and `trap` depends on who is looking.
  - A survivor underground (`dp` 1) is skipped, because the disk is read at
    tunnel coordinates.

The whole map is only ever sent in `sync`, which goes to one player after a
`pick`. Nothing that stays out of a seat can get all of it.

**2. `/ws` as a spectator.** The recorder connects and never sends anything.
Every queued event goes out with `ws.textAll()`
([network-events.hpp:85](../network-events.hpp:85)) to every socket, seated
or in the lobby, stamped with `"sq"`. Every message is written as ch `rx`,
verbatim, except:

- `s` (the 10 Hz tick; `/state` covers it),
- `eco` (~9 KB every 5 s, cosmetic by contract; `/state` has the summary),
- `wifi` (the board's network chatter).

Those three are counted, not written.

---

## File format

Every line has `record.py`'s shape: `{"ts": <seconds since start, monotonic>, "ch": <channel>, "arch": <pid or -1>, "d": <payload>}`.
Wall clock = `started_epoch` (from the first `run` row) + `ts`.

| `ch` | `d` |
|---|---|
| `run` | First row: `{"kind": "worldlog", host, started, started_epoch, poll_s, keyframe_s, ws, hold_seat, view}`. Last row (on a clean stop): `{"end": true, ran_s, pv, last_day, counts, ws_up_s, cells_known, min_heap, max_tick_ms, evt_drops, lines}`. |
| `state_key` | The whole stripped `/state`. |
| `state_delta` | `{"set": {"players.2.ll": 4, "mem.heap": 181220}, "del": ["players.2.encQ"]}`. Paths are dot-joined; a segment that is a number indexes a list. `del` is left out when empty. |
| `cells` | `arch` = whose disk; `d` = `[[q, r, terrain, variant, shelter, resource, amount, poi], ...]`, only the cells that changed. |
| `rx` | A WS message as received (`ev`, `lobby`, `ground_update`, ...). |
| `board_reboot` | `{"reset", "crash", "uptimeMs_before"}`; the next row is a `state_key`. |
| `poll_err` | `{"err", "run"}`: written on the first failure in a row and every 10th after that. |
| `ws_conn` / `ws_close` | Socket lifetime; both carry `last_sq`, `ws_close` also `why` (`peer_closed`, `yield`, `full`, `stopped`, an exception name). |
| `ws_err` / `ws_full` / `ws_yield` / `ws_back` | Connect failure; refused as full (retries in 30 s); stepped out of the lobby; came back. |

A recorder that was killed leaves a half-written last line, which `load()`
skips. There's no closing `run` row either, so `summary` says so and takes
its counts from the rows (`polls ~N`, protocol from the snapshots). Stopping
it from a background task (`TaskStop`) is a kill. Up to one poll of events
can be lost, because `rx` rows are only flushed with the next state row.

### Custom analysis

```python
from worldlog import load, replay
rows = load("runs/world-20261107-153012.jsonl")
for row, state in replay(rows):          # state = /state rebuilt as of this row
    if row["ch"] == "rx" and row["d"].get("k") == "dmg":
        p = state["players"][row["d"]["pid"]]
        print(row["ts"], state["day"], p["name"], p["ll"], row["d"])
```

`replay()` updates one dict in place as it goes. To keep a snapshot,
`copy.deepcopy(state)` it. It yields `state = None` until the first
`state_key`.

Size: about 1 MB an hour of `/state` deltas and keyframes, plus the
events.

---

## Gotchas

**The spectator socket uses up a seat.** `handleConnect` admits a socket
only while `connectedCount + lobbySize < MAX_SEATED`
([network-session.hpp:221](../network-session.hpp:221)). A socket that never
picks stays in the lobby. With the recorder in, the 5th player gets
`{"t":"full"}`. So `_seat_policy()` closes `/ws` when `/state` shows
`MAX_SEATED-1` (4) seated, and reconnects at `MAX_SEATED-2` (3). The `ws_yield` /
`ws_back` rows mark the gap, and `/state` keeps being recorded the whole
time.

**Deaths are `left` events with a `cause`.** `downed` goes only to the
player who died. Everyone else gets the `left` that follows it, which
carries the same `cause` ([network-events.hpp:464](../network-events.hpp:464)).
`summary` counts a `left` with a `cause` key as a death and one without as
walking away. `causes.DamageLedger.deaths()` looks for `downed`, so on a
world log use `loss_ledger()`, as `summary` does, not `deaths()`.

**`sq` gaps are not loss.** Events sent to one player only take `sq`
numbers too: `downed`, `tun_taunt`, `kane_call`, and some `trap`, `vac`,
`lab` and `bonney` messages. The rest of those kinds are broadcast; the
first K10 recording had `vac` countdowns in it. `summary` reports them as "sq unseen". Real loss on the board is
`/state` `evtDrops`; `summary` reports how much it grew, and that count
restarts from zero on a reboot. Messages that never went through the queue
(`chk`, `nm`, `quake`, `settle`, the `left` an `eraseslot` sends) have no
`sq` at all.

**Two clocks.** Events land the moment they arrive; `/state` only every
`--poll` s. A survivor's LL drop shows up in the `dmg` event up to 2 s
before `track` or `at` show it. Don't line up an event with a vitals row
any closer than one poll.

**`?pid=` only works for a seated player.** If a survivor leaves between
two polls, that poll comes back without `view` and no `cells` row is
written. That's harmless.

**`/state` mutex timeout.** The board answers `{"error":"mutex timeout"}`
when it can't take `G.mutex` within 200 ms. That counts as a `poll_err`,
not a snapshot.

**Board load.** It adds one more socket getting every tick broadcast
(~3–4 KB at 10 Hz) and one `/state` read every 2 s (larger with `?pid=` at
a wide vision radius). Don't lower `--poll` under 1 s. Don't use
`--hold-seat` next to `arena.py --bots 5`: the 5th bot gets `full` forever.

**Firmware quirk (open as of 2026-11-07):** a socket refused with `full` is
never closed by the board, and keeps getting every `textAll()` broadcast.
The recorder closes its own socket on `full` and retries 30 s later. Don't
"improve" that by staying connected: it relies on behaviour that should be
fixed, and it keeps loading the board while it's full.

---

## Testing on the mock

- Start it with `preview_start` `{name: "Mock Server"}`. Note the port it
  reports (8765 may be taken by another session's mock).
- **The desktop app's browser pane holds one `/ws` open to the mock, even
  after its tab is closed.** The mock's table is therefore one seat short.
  In a test script, set `worldlog.MAX_SEATED = 4` before building the
  recorder; the yield then happens at 3 seated, which matches.
- The mock ignores `rid`, so `ProbeClient.request()` waits out its timeout
  every time. Drive moves with `send_raw(json.dumps({"t": "m", "d": dir}))`
  and seat with `seat(prefer=N, timeout=4)`.
- The mock's `dbg_die` sends `left` without a `cause`, so its deaths read as
  walking away. The firmware always sends a cause.
- The mock has no `pv` and reports `mem` as zeros, so `summary` shows
  `protocol None` and `min heap 0 KB`. That's expected on the mock.
- Pattern used for the 2026-11-07 check: run `WorldRecorder(...).run()`
  next to a driver coroutine in one `asyncio.gather()`:
  1. seat 1 player, walk 12 steps, `dbg_die`;
  2. seat until the recorder yields, seat one more (it must get in);
  3. close down to 2 seated and check the recorder is back on `/ws`.

## Changing it

- New `/state` fields need no change: the diff handles any JSON. If a new
  field changes on every read (a clock, a counter), drop it in
  `strip_state()` so the deltas stay small.
- Keep WS messages under ch `rx` with `arch` -1. `causes.py` (`_dedupe`)
  and anything else that reads bot runs depend on that.
- Add a new row kind to the table above and to the module docstring, and
  handle it in `cmd_summary` if it should show up there.
- New WS message types are recorded automatically. Add one to `SKIP_WS`
  only if it is high-volume and `/state` already carries it.
