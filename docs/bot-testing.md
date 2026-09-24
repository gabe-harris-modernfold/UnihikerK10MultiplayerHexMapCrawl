# Bot testing — autonomous play harness

Canonical reference for `bots/`: Python clients that play the game over `/ws`
against a real K10 so balance can be **measured** instead of guessed. Companion
to [dev-loop.md](dev-loop.md), which covers build/flash/deploy.

Purely additive — the harness touches no firmware and no `data/`. Everything it
knows about the game it learns through the same WebSocket a browser uses.

## For AI coding agents (read first, ≤ 1 min)

- **Self-test, no board:** `cd bots && python smoke.py` (359 checks). Run this
  before every live run; it catches a broken parser in 2 s instead of wasting
  10 minutes of hardware time. It also reads the firmware source and fails on
  drift: 36 constants mirrored in `config.py`, the dispatch table, and the
  cause-of-death names.
- **Every bot is a tester now.** Each connection carries a `WireOracle` and
  the `/state` poller a `StateOracle` (`oracles.py`); anything the firmware
  promises and breaks becomes a *finding* (`findings.py`), printed at the end
  of every run and readable later with `python findings.py <log>`. See
  "Finding problems" below.
- **Three probes look for problems on purpose:** `fuzz.py` (malformed and
  out-of-order messages), `chaos.py` (disconnects, races, stalled sockets),
  `soak.py` (long realtime runs with a `sentinel` that camps, shelters, and
  rests only late in each day so the days run nearly full length). All need
  protocol 2 firmware and refuse to run without it.
- **Protocol 2 firmware answers every request** that carries a `rid` with one
  `ack` or `nack` — see "Replies" below. The client sends `rid` on everything;
  the per-bot `replies:` line at the end of a run is the first place to look
  when a policy is stuck.
- **Run an arena:** `python arena.py --host 192.168.4.234 --bots 5
  --policies scoremax,contentmax,coward,rival,scoremax --target 1000`
- **Analyse:** `python metrics.py` (newest run) or `python metrics.py runs/run-*.jsonl`
- **Pool runs:** `python metrics.py --aggregate runs/run-2026*.jsonl` — one run
  is one map roll, and the map decides enough that single-run numbers mislead.
- **Deps:** Python 3.12 + `websockets`. Nothing else.
- **Power-cycle the board before any run whose numbers you intend to trust.**
  `maxTickMs` and `minHeap` in `/state` are high-water marks that never reset.
- **Never raise the send rate above the defaults.** 2.5x the inbound rate
  starved the board's heap and crashed it mid-run — see "Useful flags".
- **Five protocol rules will silently waste a run if you get them wrong** —
  see "Protocol rules" below. Every one of them cost a wasted run to find.
- **The bunker tunnels are a second board with six more of them** — see "The
  bunker tunnels". They apply to any policy, not just the two that go down
  there deliberately: a hatch is the cheapest hex on the surface map, so bots
  fall in.
- **Never `nohup` a background run, and never pipe `arena.py` into
  `head`/`tail`.** Both orphan the Python process: it survives, keeps
  playing, and every later `verify_reset()` then sees a dirty board and
  fails. See "Orphaned runs" below — this cost 22 wasted runs in one sitting.

---

## Layout

| File | Role |
|---|---|
| `config.py` | Constants mirrored from the firmware. Updated by hand; `smoke.py` fails if any of 36 of them drift from the firmware source. |
| `mapdec.py` | Port of `data/map-decoder.js`. Full map + vis-disk decode. |
| `state.py` | Message → `Observation`. Deliberately tolerant of unknown/missing keys. |
| `navigate.py` | Dijkstra over the fogged torus using `TERRAIN_MC`. |
| `encounters.py` | Encounter JSON from local disk; ports `computeEncounterDN` + the 2d6 table. |
| `client.py` | One bot: transport, rate limiting, slot claiming, connection lifecycle. Carries a `WireOracle`. |
| `wire.py` | `ReplyTracker` (rid / ack / nack bookkeeping, shared) and `ProbeClient` (a scriptable connection: `request()`, `send_raw()`, `wait_for()`, `abort()`). |
| `oracles.py` | The invariants every connection and every `/state` read is checked against. |
| `findings.py` | The one output format for "found a problem"; also the CLI that reads them back. |
| `fuzz.py`, `fuzz_cases.py` | Protocol fuzzer and its corpus (data, checked offline by `smoke.py`). |
| `chaos.py` | Scenarios real players cause and policies never do. |
| `soak.py` | Long realtime arena preset + the soak report (day length, weather, hazards per minute, heap trend). |
| `policy/` | `decide(obs) -> Action`. Swappable. `subterranean.py` also owns the shared tunnel machinery. |
| `arena.py` | Supervisor: reset, spawn, run to target, loop. |
| `telemetry.py` | Polls `/state` for board health alongside the game log. |
| `causes.py` | Attributes every LL loss, and every death, to a cause. |
| `metrics.py` | Post-run analysis; `--aggregate` pools runs. |
| `record.py` | JSONL per run into `runs/` (gitignored). |

### Policies

| Name | Behaviour |
|---|---|
| `drunk` | Uniform random over legal actions. Soak/fuzz only. |
| `scoremax` | Chases resource piles and unexplored hexes. Opens a POI underfoot but never detours for one. |
| `contentmax` | Seeks POIs specifically. Judged on `content_score()`, not score. |
| `coward` | Refuses all risk, keeps supplies deep, avoids fire/doom/craters. |
| `rival` | Races for contested POIs to deny them; grabs ground items; makes lopsided trade offers. |
| `subterranean` | **Subterranean Explorer.** Lives in the bunker tunnels: maps the corridors, works them for water and scrap, sleeps below, surfaces only for food. |
| `tunnelrunner` | **Subterranean Explorer.** Uses the tunnels as transport: dives, walks to the shaft that surfaces furthest away, climbs out and works the fresh ground. |
| `sentinel` | The soak bot. **Makes camp** (water, forage, an existing shelter or a Settlement; never radioactive or a hatch), **builds a shelter** there (improved when it has 2 scrap, upgrades later, rebuilds after a quake), harvests in place, and stays awake until 90% of each day has passed, **then rests in the shelter** — so days run ~4.5 of 5 minutes and it heals every night. Realtime only — arena refuses it in sprint mode. |

All non-`drunk` policies share `SurvivorPolicy`, which owns the survival floor
(eat/drink/rest, emergency staple hunting) and encounter handling. Subclasses
differ only in `pursue()`.

The two Subterranean Explorers share `TunnelPolicy` (`policy/subterranean.py`)
on top of that, which owns the dive/surface cycle and the cross-board
bookkeeping.  They are the only policies that read the tunnel board on
purpose — see "The bunker tunnels" below, which also covers what happens to
the other five when they fall down a hatch.

### Useful flags

```
--bots N            1-6. Leave a slot free if you want to watch in a browser.
--policies a,b,c    cycled across slots
--target N          score that ends the run; 0 = run on --max-minutes alone
--mode sprint|realtime
--runs N            auto-loop with a cap
--seed N            whole run is reproducible from this
--no-reset          keep the current world (skips eraseslot+regen)
--max-minutes N     safety timeout
--min-interval      per-bot seconds between sends (default 0.30)
--global-interval   arena-wide seconds between any two sends (default 0.12)
--shelter           let survivor policies build a basic shelter before sleeping
                    on exposed ground — the other arm of the exposure experiment
```

**Do not raise the send rate.** `--min-interval 0.20 --global-interval 0.08`
(2.5x the default inbound rate) was tried once on a 5-bot realtime run: the
board's `minHeap` fell 47 KB → 27 KB → 21 KB over seven minutes and it then
crashed and rebooted mid-run, coming back on a fresh DHCP lease at a different
IP. The doc's "inbound traffic is cheap next to `broadcastState()` fan-out"
holds for message *processing*, not for the AsyncTCP buffers behind it. The
defaults are the measured-safe rates; if a run is starved of actions, take a
bot out rather than sending faster.

**The board's address moves.** It takes a DHCP lease, so a crash-reboot can
land it on a new IP and every `--host 192.168.4.234` in this doc goes stale.
Protocol 2 firmware answers mDNS as **`k10.local`** (`--host k10.local`), and
`/state` → `boot.reset` says whether the last restart was a crash. Without
mDNS on the client machine, a TCP-connect sweep of port 80 across the subnet
still finds it in about a minute.

---

## Finding problems

Balance runs answer "is the game fun"; this answers "is the game *right*". It
is layered so every kind of run gets it:

```
ReplyTracker (wire.py)          rid -> exactly one ack/nack; unmatched or silent = finding
WireOracle   (oracles.py)       every tick, event and reply, per connection
StateOracle  (oracles.py)       every /state read (telemetry, fuzz, chaos)
FindingLog   (findings.py)      one per run, shared by every bot: signature -> count + repro
  BotClient -> arena.py / soak.py         balance and soak runs: bugs found in passing
  ProbeClient -> fuzz.py / chaos.py       probes: bugs looked for on purpose
```

A finding has a `check` (stable id), a severity (critical / major / minor), a
signature (check + key, e.g. the pid), a count, and the last messages that
connection sent. Only the first three occurrences of a signature are written
in full; the run-end `findings` row has the totals.

```bash
python findings.py                       # newest log in runs/
python findings.py runs/fuzz-*.jsonl runs/chaos-*.jsonl
```

`metrics.py` prints a run's findings under its report, and `--aggregate`
warns when a pooled run had a critical one — a reboot or a corrupted seat
count mid-run means its balance numbers stand on a board that misbehaved.

**What the oracles check** (all derived from the firmware source; the full
list with file references is `oracles.py`'s docstring): LL within `llCap`,
food/water in [1, 6], radiation in [0, 10], a downed survivor at 0 MP, no
move mask while unable to move, steps and tick never going backwards, `sq`
never repeating or going backwards on a socket, at most one death per life
(the double-`EVT_DOWNED` seat-count bug), an acked `m` producing an `mv`, an
acked `act` producing its event, a nacked `act`'s event carrying the same
`bw`, `not_seated` never arriving while seated, and on `/state`:
`connected` equal to the seated players, no reboot, no dropped events, heap
above the pre-crash level.

### Probes

All three need protocol 2 (they judge by ack/nack) and a free seat or two.
**None has been run on hardware yet.**

**`fuzz.py`** sends the 217-case corpus in `fuzz_cases.py` — every prefix of
every command, framing (not JSON, binary, fragmented, empty), rid mangling,
out-of-range and missing fields for every handler, out-of-order encounter
and trade messages, oversized names — and checks each verdict and that the
board survives. It **refuses a protocol 1 board**, where `{"t":"e"}` runs
`eraseslot`. The corpus never contains a well-formed `regen`/`eraseslot`, a
`wifi` with an `ssid` (the board would try to join it and drop off the LAN),
or a drop of anything the probe carries; `smoke.py` asserts that. Two
`parse:` cases (`nested-t`, `nested-field`) expect what a standard JSON
parser would do and are expected to fail on current firmware: `handleMessage` and every handler take the
first occurrence of a key anywhere in the raw text, nested or not. `--burst
N` adds an unthrottled burst; off by default for the reason under "Useful
flags".

```bash
python fuzz.py --host k10.local
python fuzz.py --host k10.local --only enc_ -v
```

**`chaos.py`** runs `reconnect_storm`, `seat_race`, `abort_request`,
`abort_encounter`, `trade_then_leave` and `stalled_reader` (a raw WebSocket
that never reads — a backgrounded tab), each followed by a `/state` check.
Scenarios that cannot get what they need (a reachable POI, two free seats)
are skipped, not failed.

**`soak.py`** is an arena preset: realtime, `--target 0` (time only),
`sentinel,scoremax,coward` (the sentinel camps under a shelter and rests
late; its `camp:` line at the end says whether it managed), then a report of
day length, weather phases,
real-clock hazards per real minute and a least-squares heap trend (a steady
decline over 20+ minutes is a `heap_trend` finding).

```bash
python chaos.py --host k10.local
python soak.py --host k10.local --minutes 60
```

## Replies (protocol 2)

Firmware at `PROTO_VERSION` 2 (`sync` → `pv`, `/state` → `pv`) answers every
message that carries `"rid":N` with exactly one reply
([network-reply.hpp](../network-reply.hpp)):

```
{"t":"ack","rid":N,"cmd":"m"}                   applied
{"t":"nack","rid":N,"cmd":"m","why":"no_mp"}    refused -- nothing changed
```

Without `rid` nothing changes, so the browser is unaffected. A failed skill
check is an **ack**: MP was spent and the outcome rides the usual event. A
nack means the request was refused before it did anything.

| `why` | Means |
|---|---|
| `parse`, `bad_arg`, `bad_act`, `unknown_cmd` | malformed, out of range, or no such `t` |
| `not_seated`, `not_in_lobby`, `slot_taken` | seat state: act before pick, pick twice, pick a taken slot |
| `busy` | `G.mutex` timeout — safe to retry |
| `downed`, `in_enc`, `resting`, `underground` | player state forbids it |
| `bad_dir`, `terrain`, `wall`, `no_mp`, `cooldown` | move refusals (`movePlayer` / `moveTunnel`) |
| `pack_full`, `no_res`, `not_needed`, `archetype`, `craft` | action refusals (`ABW_*`, also on the `act` event as `bw`) |
| `not_here`, `no_poi`, `claimed`, `no_pool`, `load_failed`, `no_enc`, `no_choice`, `cannot_bank` | encounter refusals |
| `self`, `no_target`, `not_same_hex`, `dup_offer`, `no_offer`, `stale_offer`, `empty`, `no_caravan`, `caravan_short`, `water` | trade refusals |
| `refused` | an item function said no; it returns a bare bool, so this is as specific as it gets |

`client.py` counts them per command and reason, writes each nack as a `nack`
row, hands them to `Policy.on_reply()`, and closes and re-picks at once on a
nacked `pick` rather than waiting out the sync watchdog. A request unanswered
after 10 s is counted as `unanswered` — but only once `sync` has said the
board speaks protocol 2, since the mock and older firmware never reply.

Protocol 2 also:

- stamps every `ev` built from the event queue with **`sq`**, its sequence
  number. One game event, one `sq`, however many bots saw it — `causes.py`
  dedupes on it exactly — and on any one socket it only ever rises. A gap is
  **not** by itself a drop: unicast and vision-culled events (`downed`,
  `col_fail`, `fire_spread`, `flood_washout`, `tun_taunt`) spend a number
  too. Drops are counted exactly in `/state` → `evtDrops`.
- carries **`rt`** (resting) in every tick broadcast.
- names the **`cause`** on `downed` and on the `left` that follows a death.
- sends **`dmg`** (`pid`, `amt`, `cause`, `ll`) for the chem storm and
  Strangle Fog losses that used to emit nothing.

## Protocol rules

On firmware before protocol 2 each of these is silent when violated — no
error, no nack, just nothing happening. All were found the expensive way.
Protocol 2 nacks every one of them, but the rules themselves still hold.

**1. The archetype index IS the player slot.** `handleMsg_pick` does
`Player& p = G.players[arch]` and refuses if that slot is already connected
(it re-sends `lobby`; protocol 2 also nacks `slot_taken`). `SlotBroker` stops
two bots claiming one slot.

**2. Receiving state is not evidence of having joined.** `broadcastState()`
uses `ws.textAll()`, so a client stuck in the lobby receives the full per-tick
broadcast. The *only* ack for a successful pick is the unicast `sync`. A pick
watchdog waits for it and reconnects into a different slot if it never comes.

**3. `enc_start` requires `q` and `r`.** The handler opens with

```c
const char* qp = strstr(data, "\"q\""); if (!qp) return;
```

so `{"t":"enc_start"}` is discarded at the first line with no error reply. This
cost *every* POI attempt across the first several runs and was misread as the
POIs being unreachable behind fog. The coordinates are cross-checked against the
player's own position (`"Not at that hex"`), so they must be where you actually
are.

**4. The reset is two steps, and is verified against `/state`.** `regen` keeps
score and steps on connected players, so `eraseslot` ×6 must come first.
`regen` then runs `generateMap()` + `wInit()` under `G.mutex`, which blocks the
AsyncTCP task long enough that the control socket is routinely dropped *after*
the command was accepted — so socket survival says nothing. `verify_reset()`
polls `/state` until day ≤ 2 with every slot at score 0, and the run aborts if
that never happens. Termination is score-based, so a dirty board can end a run
before it begins.

**5. `ev` messages are broadcast to everyone.** Every roll appears once per
connected client. Policies filter on their own `pid`; `metrics.py` dedupes on
`(ts, pid)`. Without this, a 5-bot run multiplies every statistic by 5.

**Also worth knowing:**

- **`rt` is in the periodic broadcast from protocol 2 on.** Before that only
  `sync` carried the resting flag, so "resting" and "simply out of MP" were
  indistinguishable from `s` alone. REST is still retried on a cooldown, never
  latched — see "Deadlocks" — and a refused REST now comes back as a nack.
- **The board can drop a player slot while the socket stays open.** The
  broadcast's own `on` flag is the only signal. The client watches it and
  reconnects. See "Known issues".
- **Re-picking a downed slot is the respawn path** — fresh survivor, lifetime
  score and steps carried over, exactly as for the browser client.
- **Encounters gate movement.** `m` and `act` are both refused while one is
  open; answer with `enc_choice` / `enc_bank` / `enc_abort`.
- **`vm` is a free legal-direction bitmask** (`computeValidMoves`), and it
  already accounts for equipment unlocks like the raft. Use it for the step you
  are about to take; use `navigate.py` to choose which step that should be.
- **There is no inbound rate limiting anywhere** in `handleMessage`. Bots must
  self-throttle. In practice the load is `broadcastState()` fan-out rather than
  inbound traffic, so client *count* matters far more than send rate.

---

## The bunker tunnels

The second board ([tunnel-system-spec.md](tunnel-system-spec.md)) is 16x10,
walled where the surface is a torus, and reached by stepping onto a hatch.
Two policies play it on purpose — the **Subterranean Explorers**,
`subterranean` and `tunnelrunner` — and the other five have to survive
falling into it, because terrain 12/13 costs 1 MP, which makes a hatch the
cheapest hex on the surface map and a pile-chasing pathfinder walks onto one
eventually.

Run them against a surface control, or there is nothing to price the
underground against:

```bash
python arena.py --host "$HOST" --bots 5 \
    --policies subterranean,subterranean,tunnelrunner,scoremax,scoremax \
    --target 1000
```

`metrics.py` grows a `-- bunker tunnels` block whenever a run contains a
descent, including accidental ones.

### Six ways the second board breaks surface assumptions

Each of these was a real defect in the harness before it was a rule.

**1. `p.q`/`p.r` are not where the survivor is.** While `depth` is 1 they stay
pinned to the hatch they came down — that invariant is what lets weather, the
world system and the LCD keep indexing `G.map` unchanged. So
`obs.map[(me.q, me.r)]` underground returns a real, plausible surface cell 30
metres overhead, and every decision made from it is confident nonsense. Use
`obs.board`, `obs.pos()` and `obs.here()`, which switch boards for you.

**2. The tunnel board does not wrap.** `wrapQ`/`wrapR` are hardcoded to the
surface dimensions. `WorldMap(wraps=False)` makes an off-board coordinate read
as `None`, and `in_bounds()` is what separates "rock beyond the wall" from
"on the board but still fogged" — the pathfinder must never enter the first
and very much wants to enter the second.

**3. A hatch is entered, never traversed.** The crossing happens as the last
act of the step that lands on it, so a route *through* one does not exist: it
ends there, on the other board, somewhere the caller did not choose. Every
search on either board passes `stop_at=ends_journey`. Without it the
pathfinder plots a tidy line straight through a hatch and the survivor is
ejected mid-journey.

**4. You cannot ascend from the shaft you are standing on.** You arrived by
transition, not by a step, and only a fresh step onto one crosses. So an
accidental descent costs about 4 MP to undo — step off the shaft, step back
on — not 1.

**5. The hatch pairings are never sent.** Which surface hatch comes out at
which shaft is decided in `generateTunnels()` and rides the save header. A
client can only infer it by watching a `tun_in` / `tun_out` event, which names
the hatch index and the *surface* hex, and pairing that against where the
survivor was underground. `Observation.hatches` accumulates it — including
from other players' events, so a fleet maps the network faster than one bot
can.

**6. `tsync` arrives on every descent, fogged.** Applied naively it wipes the
corridor map built on the last trip down. `WorldMap.load_full(merge=True)`
keeps what was already known.

### What the tunnels charge, and what they do not

The numbers the Explorers exist to measure, all from `tunnels.hpp` and
`actions_game_loop.hpp` rather than from the spec's prose:

| | Underground | Surface |
|---|---|---|
| Move cost | 2 MP flat | ~1.6 MP average |
| Sleeping out | 30% roll for 1 LL (`TUNNEL_REST_LL_PCT`) | 2 LL, certain, on 96% of terrain |
| Food | none — `TERRAIN_FORAGE_DN[14]` is 0 | forageable on 4 terrains |
| Water | every corridor cell | 4.7% of hexes |
| Scrap | salvages at DN 7, **no threat clock** | ruins salvage, +1 threat |
| Refused actions | SHELTER, CRAFT, SURVEY | — |

Two of those invert the obvious play and both caught this harness out:

- **A corridor is cover.** `dawnUpkeep()` treats `depth != 0` as covered, so
  sleeping below trades a guaranteed 2 LL bite for a 30% chance of 1. Being
  hurt is a reason to go *down*, not to stay up — the first cut of
  `provisioned()` refused to dive below LL 3 and spent an entire run too
  battered to ever reach the one place that would have let it heal.
- **Water is not a reason to stay up.** Requiring a full canteen to enter the
  wettest place on the map kept the bots on dry scrub for 25 game-days.

Bad air is not floored at LL 1 the way exposure is, so it *can* land the
killing blow. Both policies therefore rest below only when the rest-heal will
fire (F >= 2 and W >= 2) and surface when it will not.

### Reading the metrics

`mp_per_surface_hex` is the headline: MP spent underground per surface hex
actually crossed, against ~1.6 over ground. The spec predicts a 44-67% saving
measured over the generator's own corridor paths with the whole network
known; these bots have one ring of vision and learn the pairings by using
them, so the gap between the two figures is the cost of not having the map.

`dawns_below` / `rests_below` / `below_dawn_ll` are the other half of the
ledger — read them against a surface bot's exposure losses in
`-- what killed them`. `stranded_dawns` counts dives that wanted out and were
still down there at dawn.

### Caveats

- **There are no tunnel encounters yet.** `generateTunnels()` deals POIs from
  the `"14"` pool, and `data/encounters/index.json` has no `"14"` entry, so
  `encPools[14].count` is 0 and nothing is placed. The policies handle a
  tunnel POI already (`enc_start` carries `tq`/`tr`, which
  `handleMsg_enc_start` validates board-aware); it simply never fires.
- **The mock is a protocol stand-in, not an economy.** `mock-server/` speaks
  the whole tunnel protocol — `tsync`, `dp` vis disks, `tun_in`/`tun_out`,
  bad air — which is enough to develop the policies offline, and they were.
  Its `DIR_DELTA` is its own approximate geometry (no `(+-1, 0)` step), it
  charges no terrain cost on the surface, and its actions succeed regardless
  of terrain. **Dive counts and crossings from a mock run prove the mechanism;
  the MP-saving number from one means nothing.** Take that from hardware.
- **`arena.py` cannot drive the mock** — no `/state`, so `fetch_state()` and
  `verify_reset()` both fail. Drive `BotClient` directly for offline work.

---

## Orphaned runs

The single most expensive failure mode found so far, because it looks like a
board problem rather than a harness one.

`python arena.py ... | grep ... | tail -2` **orphans the arena.** `tail` exits
as soon as its input closes or it has what it needs, `arena.py` takes the
SIGPIPE and keeps running, and the shell moves on to the next command. The
orphan's five bots stay connected and *keep scoring*. Every subsequent run
then fails at `verify_reset()`, because that polls `/state` until every slot
reads day ≤ 2 and score 0 — and the orphan is busily making that untrue. The
reported symptom is `reset: eraseslot x6 + regen ... FAILED` on every run,
forever, which reads exactly like a wedged board.

Two tells that distinguish it from a real board fault:

- the `reset` record's `dirty` list shows scores **climbing between
  attempts** (`[[1,36,10],[2,36,13]]` then `[[1,44,12],[2,37,17]]`) — nothing
  but a live player does that;
- `mv` on the previous run's JSONL fails with **"Device or resource busy"**,
  because the orphan still holds the file open.

Do this instead — redirect to a file, then grep the file:

```bash
python arena.py --host "$HOST" ... > "$LOGDIR/$seed.log" 2>&1
grep -E "end: |reset:" "$LOGDIR/$seed.log" | tail -2
```

and check for strays before each run:

```bash
powershell -NoProfile -c "(Get-Process python -EA SilentlyContinue|Measure-Object).Count"
```

**An orphan also contaminates the runs it does not break.** Its bots reconnect
into whatever slots are free, so they show up as joins on slots the new arena
never claimed — which `RunReport.outsiders()` flags exactly as it flags a
human who opened the browser. Both are "not this arena's bots", and neither
can be pooled.

---

## Deadlocks to avoid

Two loops that a naive policy falls into, both observed live.

**REST spam vs the REST latch.** Re-sending REST every cycle is a server-side
no-op (`doRest` returns early when already resting) but burns a message each
time — 2054 dead sends in one early run. Latching it once per game-day is
*worse*: the decision is latched before the send clears the rate limiter, so a
REST decided just before a reconnect never lands while the bot believes it
rested. It then sits at `mp == 0`, awake, unable to move until dawn — and
because `tickGame()` only ends the day early when **every** connected player is
resting, one bot stuck like that freezes the whole fleet. Five bots sat frozen
at day 16 this way. Retry on a ~4 s cooldown instead.

**Stale POI bits.** A consumed POI makes `enc_start` fail while the cached map
still shows the bit set (`err "Already looted"`, and a `no_poi` nack on
protocol 2). Cap attempts per hex and fall through to movement.

---

## Timing: how long is a day?

`DAY_TICKS` (3000) × `TICK_MS` (100) = 5 real minutes — **but** `tickGame()`
ends the day the moment every connected player is resting. An all-bot fleet all
rests when out of MP, so days collapse to a few seconds regardless of `--mode`.
That is genuine multiplayer behaviour, not an artefact: a real party would do
the same.

| Mode | Day length | Use for |
|---|---|---|
| `sprint` | ~2.3 s | Soak and stress only |
| `realtime` | ~3–5 s with an all-bot fleet; 5 min if anyone stays awake | Everything else |

**Sprint distorts balance measurement.** A survivor gets `ll + 3` ≈ 10 MP per
day, enough for six or seven moves, but at a 2.3 s day with a shared send
budget each bot only gets four or five *messages* per day. The bots then play
every day on a third of their action budget, which makes survival look far
harder than it is. Use sprint to find breaking points, not balance numbers.

For near-5-minute days (weather, fire, flood, Creeping Doom all tick on their
own clocks) put a `sentinel` in the fleet. `tickGame()` starts a new day at
`DAY_TICKS` *or* when every connected player is resting, so one player awake
holds the day open — and a player who never rests never heals, so the
sentinel stays awake for 90% of each day (timed off the tick id since the
dawn it last saw) and then rests in its shelter. `soak.py` does exactly that
and reports whether the days really ran long.

**The bots now see shelters change without moving.** An action gets no vis
disk, so until this the local map said "no shelter" after a SHELTER until the
next step, and never learned about quakes or new settlements at all.
`state.py` now applies the `act` event's `cnd` (the hex's shelter level
afterwards — what the browser uses), and the `quake` and `settle` events.

---

## Board limits

Read `/state` only against a known uptime — `maxTickMs` and `minHeap` are
high-water marks that never reset, so a long-lived board reports the worst
moment it ever had.

| Metric | Fresh boot, 0 clients | 5 bots playing |
|---|---|---|
| `maxTickMs` (budget 100) | 3 | 149–405 |
| `minHeap` | ~185 KB | ~47–110 KB |
| `broadcastPartial` | 0 | 1–95 |
| `broadcastSkips` | 0 | 0 |

`broadcastSkips` staying at 0 means mutex contention is not a factor. The
occasional large `maxTickMs` spike is `saveGame()` — `tickGame()` saves on
every dawn, and with collapsed days that is an SD write every few seconds.

**Ceiling: 5 bots.** `handleConnect` rejects once
`connectedCount + lobbySize >= MAX_PLAYERS` (6), and a rejected bot gets
`{"t":"full"}` and retries forever. Any stale slot eats into that budget.

---

## Equipment (and why older numbers are not comparable)

`SurvivorPolicy.gear_action()` picks up equipment underfoot and wears the best
thing it is carrying for each slot. "Best" is per policy: every policy carries
a `gear_weights` table and `config.gear_score()` scores an item out of
items.cfg against it, so the bots want different gear for the reasons they
already play differently.

| policy | wants | why |
|---|---|---|
| coward | exposure immunity, +LL, rad | its whole plan is not dying; exposure is the biggest measured LL drain |
| scoremax | +MP, +slots | the economy is "keep moving, prefer piles, keep pack space free" |
| contentmax | vision, +MP, −threat | POIs must be seen to be opened, and the Threat Clock is what makes them lethal |
| rival | +MP, vision | denial is a race |
| subterranean | vision, +MP | tunnel sight is 1 hex, and a corridor step costs 2 MP |

Two properties are asserted in `smoke.py` because breaking either is silent:
the five policies must want **different** loadouts (otherwise the weights are
decoration), and swapping must reach a **fixed point**. The second matters
because `equipItem()` returns the displaced item to the pack — a policy that
ever prefers what it just took off loops forever at one message per cycle and
looks like nothing at all from either end.

A fuel-gated `mp` is discounted (`GATED_MP_DISCOUNT`), not taken at face
value: the bots run their fuel down and the bonus vanishes on a dry dawn.

`drunk` does not inherit `SurvivorPolicy`, so it never equips anything — that
is the point of it as a chaos baseline, not an oversight.

**Every balance number recorded before this existed was measured on a survivor
wearing nothing.** The bots parsed `eq[]` out of every state message and never
sent a single `equip_item`, so no run ever exercised the +LL ceiling, the
vision bonus, carry slots, or a fuel-gated vehicle. Treat the tension-arc
figures from those runs as a no-equipment baseline, not as the game's balance.

Two things changed underneath at the same time, both of which move the numbers:

- 12 of the 24 equipment items had no drop table or recipe entry and could not
  be obtained at all. They are in the `*_rare` tables now, so a run that opens
  POIs will actually find gear.
- `INV_SLOTS_MAX` went 12 -> 18. It used to equal the Mule's base, so slot
  gear did nothing for that archetype, and FORAGE/WATER/SCAV ignored the pack
  size entirely. They honour it now (`tokenRoomFor`), which puts a real ceiling
  on hoarding and makes a Backpack worth wearing.

`config.EQUIPMENT` is parsed from `data/items.cfg` at import rather than
hardcoded, so adding an item to the registry needs no change here.

## What gets measured

`metrics.py` reports the things that speak to whether the game is *entertaining*,
which win rate does not.

| Metric | Reads as a problem when |
|---|---|
| **Tension vs target** | see below — this is the headline |
| Score spread | identical policies finishing far apart = spawn/map roll decides it |
| Snowball | day-5 order predicting the finish = decided before it gets interesting |
| Check margins | everything clearing by +5 = nobody was ever at risk |
| Action mix | one verb dominating = the others are decoration |
| Resource slack | pack never tight = the player is never choosing |
| Crisis recovery | 100% = not a crisis; 0% = death spiral. ~60–75% is the band |
| POI reach | content nobody arrives at |
| Connections | `seated_s` is the only number reflecting time actually playing |
| **Death causes** | one cause carrying most deaths = one dial, not a system |

### The design target

**A good tension arc across 2 hours of play, with a character coming close to
dying at most 4 times in that window.**

Three things are tracked separately because they fail independently:

- **Rate** — deaths and near-deaths per bot per game-day, projected onto a
  session. Counted apart on purpose: *coming close to dying* means surviving,
  so a death is a failure of the target rather than an instance of it. The
  projection needs a day-length estimate (`EST_MINUTES_PER_DAY`, default 3);
  it is a parameter, not a constant of the game.
- **Dwell** — where LL actually sits. Time at full health is time with no
  tension. A bimodal distribution with a hole in the middle means characters
  are either fine or already doomed.
- **Arc** — deaths by decile, first half vs second. A flat distribution is not
  an arc however many brushes it contains.

### Cause of death

`causes.py` answers *what killed them*, which is the question the rate alone
cannot: 6 brushes against a target of 4 is one number, but "5 of them were
exposure" and "they were spread across five systems" call for completely
different fixes.

**Protocol 2 puts the cause on the wire.** Every `EVT_DOWNED` site sets a
`DC_*` code (`Esp32HexMapCrawl.ino`), and `downed` / `left` carry it as
`cause` in the names below; those deaths report `via == "wire"`. The dawn
death is named for the last loss applied — bad air, then thirst, then hunger —
since exposure is floored and never takes the last point.

On older recordings nothing on the wire says cause of death — `EVT_DOWNED`
carries a pid and nothing else. Every LL loss does leave a distinct signature
though, so each death is matched to the damage record nearest it. The table of
signatures is in `causes.py`'s docstring. Three things about the inference are
worth knowing before reading a number off an old report:

- **The killing `dawn` arrives *after* its own `downed`.** `dawnUpkeep`
  enqueues `EVT_DOWNED` inside the LL-loss loop and `EVT_DAWN` only at the end
  of the function, so the event that explains a death is ~15 ms *later* on the
  wire than the death. The match window is two-sided for this reason.
- **Two causes emit nothing at all.** The chem-storm and Strangle-Fog per-tick
  hazards just decrement LL. They are inferred from the sampled weather phase,
  and anything still unexplained is reported as `unattributed` rather than
  folded into a neighbour — watch that number, it is the honesty check on
  everything above it.
- **`enc_choice`'s `cost_ll` is invisible.** A *successful* choice that spends
  the last LL emits `enc_res` with `out: 1` and no penalty field, so a success
  immediately before a death is read as an encounter cost.

Causes run on two different clocks and must not share a projection:

| Clock | Causes | Why |
|---|---|---|
| per game-day | thirst, hunger, exposure, radiation | fire once per dawn/dusk |
| per real minute | fire, lightning, Creeping Doom, chem, fog | `WORLD_TICK_INTERVAL`, and `weatherNextGapMs` floors weather changes at ~1.1–1.9 real minutes no matter how fast game-days fly |

A bot fleet collapses days to a few seconds, so it lives ~85 game-days inside
7 real minutes — roughly 25x the supply pressure per unit of hazard exposure
that a human meets. Scaling the hazards per game-day would inflate them by
that same factor, so `death_causes()` scales them per real minute instead and
prints both clocks. **The corollary is that hazard causes are badly
under-sampled**: an hour of pooled bot time is half a session's worth of
hazard exposure, so their confidence intervals stay wide however many runs
you pool. Supply causes are the opposite — hundreds of bot-game-days accrue
in minutes.

### Pooling runs

One run is one map roll and one set of spawns, and the map decides a lot
(snowball agreement ~80% by day 5). Two runs on the same settings have come
in at 6.1 and 0.6 projected brushes. So headline numbers come from
`--aggregate`, which pools across runs and prints Wilson 95% intervals on
each cause share plus the per-run spread next to it:

```bash
python metrics.py --aggregate runs/run-2026*.jsonl
```

Two sets of runs go side by side with `--compare`, which pools each arm and
then prints the differences — which is how the shelter experiment below is
read:

```bash
python metrics.py --compare baseline=runs/armA --compare shelter=runs/armB
```

### The exposure confound

Exposure (`dawnUpkeep` §7.3: −1 LL per dawn wherever terrain SV < 2 and
nothing is built) covers **96% of the map** — only Flooded, Mountain,
Settlement and Bunker are SV ≥ 2. It is the largest single LL drain measured,
and its counter-play costs 1 scrap and 1 MP, always succeeds, and pays 4
points.

And it does more than cancel exposure. `dawnUpkeep`'s next clause —
*"resting in shelter suppresses all LL losses"* — zeroes `llDelta` outright
when a resting survivor is under any shelter, so a basic shelter is immunity
to the **entire** dawn, thirst threshold breaks included. Both measured
killers, for one scrap, with no check to fail, plus 4 points.

No policy used it. `SHELTER` was one of the five never-used verbs, while bots
walked around carrying 5–6 scrap. Measuring exposure against a fleet with no
answer to it measures the bot's blind spot, not the game's balance — so
`--shelter` turns on a survival-floor step that builds a basic shelter on the
last MP of a day spent on exposed ground, and the two arms are run and pooled
separately. Anything that moves between the arms is policy, not balance.

It is placed ahead of the emergency staple hunt, but only fires at `mp <= 1`
(or when LL is already critical). On the last MP of a day one more step
cannot reach water — most terrain costs 2–4 MP to enter — while the shelter
cancels the whole night, so building strictly dominates walking *there*. The
hunt still outranks it on every earlier move.

**Measured.** 4 shelter runs (905 bot-game-days, 26 deaths, SHELTER sent 131
times) against the 5-run baseline (1795 bot-game-days, 94 deaths):

| | baseline | shelter |
|---|---|---|
| deaths per bot-game-day | 0.0524 | **0.0287** |
| projected brushes / 2 h | 4.26 | **2.34** |
| verdict vs target | **over** | **within** |
| exposure deaths | 53 | 9 |
| thirst deaths | 39 | 16 |
| LL lost per bot-day, total | 0.589 | **0.397** |

Death-rate ratio **0.55x [0.36, 0.85]** — the interval clears 1.0, so this is
a real effect and not run-to-run noise.

**The game already meets its own tension target as soon as the survivor uses
one verb.** One scrap a night moves it from 4.26 brushes to 2.34 and flips
the verdict. So the baseline numbers are a *floor on harshness for a player
who never shelters*, not a measurement of the game's balance — and the
honest reading of "tension is over budget" is that it is over budget **only
for a player who has not discovered SHELTER**.

Two details worth keeping:

- **The shelter cuts thirst deaths too** (LL lost to thirst 0.208 → 0.121,
  −42%), which is `dawnUpkeep`'s all-losses suppression working exactly as
  written, not a side effect of fewer exposure ticks.
- **The remaining deaths invert.** With shelter, thirst becomes the leading
  cause (61.5%) and exposure drops to 34.6%. Tuning past this point is a
  water-economy problem, not an exposure one.

---

## Findings so far

Measured on hardware, 5 bots, realtime, target 1000. The tension and
cause-of-death numbers are pooled over 5 runs; the rest are from a single
day-85 run and are marked where that matters.

**Score ≈ 5 × steps.** Consistent across archetypes with very different skills.
Walking is the game:

| Source | Points |
|---|---|
| Step onto a resource pile (`collectResource`, automatic inside `movePlayer`) | **10/token**, piles are 1–3 |
| Explore a new hex | 1 |
| Found a settlement | 20 |
| Improved / basic shelter | 8 / 4 |
| Bank encounter loot | 3/token (+10 full clear) |
| FORAGE success / partial | 3 / 1 |

48.5% of hexes hold a pile, so roughly every other step pays 10–30 points
against 3 for a 2-MP FORAGE. **Pile density is the highest-leverage balance
lever in the game.**

**Engaging with content costs about half your score.** The bot that opened the
most encounters (9, 11 nodes) finished last at 500; a bot that opened none
finished at 847, and the winner opened 2 and scored 1008.

**The game is largely decided by day 5** — snowball agreement 80% over 10 pairs.

**Skill checks are well-shaped.** 200 rolls, 70% win rate, mean margin +0.70,
**44% landing within 1 of the DN**. The healthiest number in the dataset.

**Tension is over budget and the wrong shape.** Pooled over 5 realtime 5-bot
runs — **94 deaths, 97 near-deaths, 1795 bot-game-days, 100% attributed**:

- **4.26 projected brushes** per 2-hour session against a target of 4. Over,
  but only just, and the per-run spread was 2.7–6.1 — one run cannot tell you
  which side of the line the game is on.
- **1:1.03 deaths to near-misses**, against a target nearer 1:4. This is the
  sharpest failure in the dataset and it is not the same problem as the rate:
  characters are not having close calls, they are dying. A brush that kills is
  not a brush.
- **56.9% of time at full health**, and the wounded-but-coping band is thin
  (LL 4 = 6.7%). Tension lives in the middle and the middle is mostly empty.

### Cause of death

| Cause | Deaths | Share | 95% CI | Clock |
|---|---|---|---|---|
| exposure | 53 | **56.4%** | 46.3–66.0% | game-day |
| thirst | 39 | **41.5%** | 32.0–51.6% | game-day |
| fire | 1 | 1.1% | 0.2–5.8% | real-min |
| creeping doom | 1 | 1.1% | 0.2–5.8% | real-min |

Everything else scored **zero**: hunger, radiation, encounter hazards,
encounter costs, lightning, flood, chem storm, Strangle Fog. Two survival
systems are the whole game and the other eight are decoration.

**Thirst frightens, exposure finishes.** The two causes do different jobs, and
the split is the most useful thing in the dataset:

| | thirst | exposure |
|---|---|---|
| deaths | 41.5% | 56.4% |
| **near-misses** | **73.2%** | 23.7% |
| total LL lost | 35.3% | 61.6% |

Thirst is what drives a survivor into the danger band; exposure is what takes
the last point once they are there. That is because the water thresholds
**latch** — `wThreshBelow` bit 2 fires once when W hits the floor and never
again — so thirst is a one-off drop of a few LL, while exposure is −1 every
single dawn, forever, on 96% of the map.

**Root cause, restated.** The earlier reading blamed `effectiveMP = ll + 3`
and the scarcity of water terrain. The cause data says the spiral is real but
the *terminal* step is exposure, and the chain is:

1. Water terrain is 4.7% of the map, so W falls to its floor and latches.
2. A latched W track means `restedWell` (`food >= 4 && water >= 3`) is false.
3. `restedWell` is the **only** source of the +1 LL that cancels the −1
   exposure tick, outside a Settlement.
4. So the moment W latches low, every dawn is a guaranteed −1 with no ceiling,
   and `effectiveMP = ll + 3` removes the mobility needed to fix it.

The dial is not exposure's damage. It is that recovery is gated on the one
resource the map is stingiest with.

### Tuning pass, 2026-09-20 — acting on the above

Five changes landed off the back of these measurements. All compile clean
(22% flash / 22% static RAM) and are mirrored across firmware, mock, client
and the bot analyser. **None of them has been measured yet** — the numbers
below are models, not results, and the whole point of the harness is that
they now get re-run.

| Change | Where | Effect (modelled) |
|---|---|---|
| Flash flood costs 1 LL | `world-system.hpp` | it could not kill at all before — no LL line existed |
| Fire scales with intensity | `fireDamageFor()` | blaze (3) = 2 LL, burn (2) = 1; was flat 1 |
| Hunger floor penalty | `applyFStep` | food had 2 thresholds to water's 3; starving at the floor was free |
| Lightning 15 → 17%, jitter ±4 → ±2 | `maybeIgniteLightning` | 0.10 → 0.38 direct hits per 2 h session |
| Encounter DN `2 + r*10/100` → `5 + r*7/100` | `computeEncounterDN` | library failure 9.3% → 25.9%, 0.28 → 0.74 LL/choice |

**Two of these were reach problems, not damage problems.** Lightning's strike
jitter was a uniform ±4 box — 81 cells — so a direct hit needed the jittered
hex to *be* the player's hex: a 1-in-81 event, ~0.1 per session. Raising
`LIGHTNING_IGNITE_CHANCE` moves that to 0.116; the box is what matters. Fire
was limited by the same box, since that is where fire gets ignited. Tune the
span before the damage for either of them.

**And the encounter one was a formula problem, not a content problem.** It is
worth being precise about this because the first read was wrong: the bots'
7% encounter-failure rate looked like policy risk-aversion
(`min_success = 0.45`), but modelling all 334 authored choices gives 9.3% —
the filter almost never binds. `dn = 2 + risk*10/100` compressed the authored
0–100 range into DN 2–12 when only DN 5–10 is contestable against 2d6+skill,
so the library's median `base_risk` of 30 became DN 5, a 92% pass. The
penalties were never weak: 277 of 320 hazards cost LL, mean −3.2. They just
almost never fired.

**Still open:** `computeEncounterDN`'s LL bonus is `(ll-4)/2 when ll > 4`, so
checks get *easier* at full health and *harder* once wounded. That is a
death-spiral amplifier and it points the same way as the 1:1 death-to-near-miss
ratio. Left alone in this pass, deliberately — worth revisiting after the
re-measure.

**Bug found while validating lightning:** `maybeIgniteLightning()` was the one
damage site missing the `p.ll == 0` guard that `resolveFireDamage`,
`resolveDoomProximity`, `duskCheck` and `dawnUpkeep` all have. A strike on a
player already downed and awaiting their slot reset queued a *second*
`EVT_DOWNED`, whose handler does `G.connectedCount--` and appends to
`lobbyIds` — so it corrupts the seat count `handleConnect` uses to decide the
board is full. Plausibly a contributor to the seat-loss churn below. Fixed in
firmware and mock.

### Untested, not safe

`weather_pct` exists to stop a zero being misread. Across every run measured:
**chem storm never occurred once**, and Strangle Fog held 2.7% of the time.
Both are per-tick LL hazards on real clocks. They score zero deaths because
they barely happened, not because they are harmless — an all-bot fleet
collapses game-days to a few seconds while `weatherNextGapMs` floors weather
changes at ~1.1–1.9 real minutes, so a 7-minute run sees about five weather
phases where a 2-hour session would see 60–100. Fire and Creeping Doom each
landed exactly one kill in 152 bot-minutes, which projects to ~0.8 per session
each with a CI far too wide to act on.

**Measuring the real-clock hazards properly needs a policy that deliberately
stays awake**, so days run their full 5 minutes. That now exists — the
`sentinel` policy, run by `soak.py` — but **has not been run on hardware
yet**, so the figures above are still the under-sampled ones.

---

## Known issues

- **The board falls over under a sustained 5-bot load.** Four times in one
  session: twice it crashed and rebooted onto a fresh DHCP lease mid-run
  (once with `minHeap` visibly bleeding 47 KB → 27 KB → 21 KB beforehand),
  and twice it dropped off the network entirely — `/state` timing out, no
  ICMP, nothing answering on port 80 anywhere on the subnet. Runs at the
  default send rates survive 5–10 minutes reliably; **batches of four do
  not**. Run one at a time, check `/state` between runs, and stop the loop
  rather than burning the remaining seeds against a dead host.

  **You do not need physical access to recover it.** Pulse DTR/RTS on COM5 to
  force the ESP32 reset — same sequence as the boot-log capture in
  dev-loop.md — and it comes back with a clean heap, which is also the
  power-cycled state this doc asks for before a run you intend to trust:

  ```powershell
  $p = New-Object System.IO.Ports.SerialPort COM5,115200,None,8,one
  $p.Open()
  $p.DtrEnable=$false; $p.RtsEnable=$true;  Start-Sleep -Milliseconds 150
  $p.RtsEnable=$false; $p.DtrEnable=$true;  Start-Sleep -Milliseconds 100
  $p.DtrEnable=$false
  # read for ~45 s, then grep the capture for the address:
  #   Boot STA connected ssid=cattails ip=192.168.4.239 rssi=-54
  $p.Close()
  ```

  Take the host from that boot line rather than from this doc — the lease
  moves every time it reboots.
- **The mock cannot stand in for the shelter arm.** `mock-server/server.js`
  mirrors `dawnUpkeep()` closely, exposure and shelter-protects-rest
  included, but it has no `doShelter()`: `ACT_SHELTER` falls through to the
  generic act stub, which drains MP and then *adds* a token to `inv[a % 5]`
  — slot 4, scrap. So SHELTER on the mock pays you a scrap instead of
  costing one and never sets the cell's shelter bit. Any balance number for
  the shelter arm has to come from hardware.
- **Firmware seat-loss fix is written but uncommitted and unflashed.**
  `ws.client(id)` only matches `status() == WS_CONNECTED`, and the stale-slot
  reap in `handleConnect` clears `p.connected` *without* closing the socket, so
  one client connecting can silently unseat a live player — who keeps receiving
  broadcasts with a frozen score. 47 seat losses in one 7-minute 5-bot run. A
  grace-window fix (`lastWsAliveMs` + `WS_REAP_GRACE_MS`) plus
  `cleanupClients(MAX_PLAYERS + 2)` is in the working tree and compiles clean in
  isolation (22% flash / 22% static RAM), but the shared tree does not build.
  The client works around it by watching its own `on` flag and reconnecting.
- **Balance numbers are contaminated by that churn** until it lands. Some
  deaths are plausibly harness-induced.
- **Five of eight action verbs are never used** — SHELTER, SCAV, SURVEY, CRAFT,
  TREAT. Partly a policy limitation, but no score-driven policy has had a
  reason to want them either.
- **No recipes learned** across 12 banked encounters.
- `config.py` mirrors firmware constants by hand. `smoke.py` now checks the
  dispatch table and the cause names against the firmware source, but the
  numeric constants are still unchecked.
- **Fixed in protocol 2, found while engineering for bots:** the dispatcher
  matched any *prefix* of a command name (`{"t":"e"}` ran `eraseslot`,
  `{"t":"r"}` ran `regen`); `drop_item` crashed on `"qty"` with no `:`;
  `regen` announced a new world even when it had not made one; the event queue
  dropped overflow with no count; and every connecting client was sent the
  saved Wi-Fi password in plain text. A harmful consumable could also leave a
  survivor at LL 0 without ever being downed.
