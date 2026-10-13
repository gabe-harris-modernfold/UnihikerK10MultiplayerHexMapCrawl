# Bot testing — autonomous play harness

Canonical reference for `bots/`: Python clients that play the game over `/ws`
against a real K10 so balance can be **measured** instead of guessed. Companion
to [dev-loop.md](dev-loop.md), which covers build/flash/deploy.

Purely additive — the harness touches no firmware and no `data/`. Everything it
knows about the game it learns through the same WebSocket a browser uses.

## For AI coding agents (read first, ≤ 1 min)

- **Self-test, no board:** `cd bots && python smoke.py` (639 checks). Run this
  before every live run; it catches a broken parser in 2 s instead of wasting
  10 minutes of hardware time. It also reads the firmware source and fails on
  drift: 51 constants mirrored in `config.py`, the dispatch table, and the
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
- **The budget is Action Points, `ap` on the wire, from protocol 7.** Older
  firmware sent `mp`; against it `me.ap` reads 0 and every bot just rests,
  so `arena.py` refuses a board below `config.MIN_PROTO`. The run logs in
  `runs/` were migrated (2026-10-12), so the analysers read old runs too.
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
- **Booby traps open scenes nobody asked for** (protocol 5) — see "Booby
  traps". A bot that only drives encounters it opened itself will sit in one
  forever; every `SurvivorPolicy` answers them with `decide_trap()`.
- **Machine Gun Kane shoots survivors in the open, and his story opens scenes
  on arrival** (protocol 8) — see "Machine Gun Kane". Every `SurvivorPolicy`
  takes cover when he asks; `contentmax` goes after his five chapters. His
  camps only appear on days that reach dusk, so an all-bot fleet that rests
  early never sees one: put a `sentinel` in a realtime fleet for the story.
- **VACANCY's four parts open on arrival too** (protocol 10) -- see
  "VACANCY". Its desk and corridor count only when BANKED at their last
  node, so a driver that banks early or walks off leaves them open;
  `decide_vac_part()` plays every part to the end. `vac_check.py` stages it
  on the mock.
- **Since 2026-10-13 every survivor uses its items and plays scenes by
  expected value.** See "Items" and "Encounter planning". Earlier numbers
  came from bots that never used a consumable and took the first open
  choice in every scene. Setting `plan_encounters = False` and
  `use_items = False` on a policy gives those baseline arms back.
- **Quests are checked, not just played.** See "Quests". `python quests.py`
  lints the content, `quest_check.py` stages every chain on the mock, and an
  arena with a `quester` in the fleet prints the fleet's quest ledger.
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
| `gearcheck.py` | Equipment audit: declared mods vs the wire, and a survival ranking. |
| `record.py` | JSONL per run into `runs/` (gitignored). |
| `kane_check.py` | Machine Gun Kane on the mock: real bots, staged with `dbg_kane` -- do they take cover when asked, and do they read his story to an ending. |
| `vac_check.py` | VACANCY on the mock: a real quester, staged with `dbg_vac` -- does it read the town, bank both halves, and check the town out; and does the fall turn the town to Broken Urban. |
| `items.py` | Item rules, as pure functions of items.cfg and recipes.cfg: when a consumable is worth using, what is litter, caravan payment, whether a trade is fair, and loot worth for the planner. |
| `policy/itemuse.py` | The item layer every `SurvivorPolicy` mixes in: `use_item`, `drop_item`, `car_buy`, extra crafts and trade replies, each with a cooldown. |
| `quests.py` | The quest catalog (derived from the content where possible), each bot's journal, the fleet's ledger, and the sequencing checks. `python quests.py` lints the content. |
| `quest_check.py` | Stages every quest chain on the mock with `dbg_enc` for a real `quester` to play: in order, out of order, the gates, and the workshop. |

### Policies

| Name | Behaviour |
|---|---|
| `drunk` | Uniform random over legal actions. Soak/fuzz only. |
| `scoremax` | Chases resource piles and unexplored hexes. Opens a POI underfoot but never detours for one. |
| `contentmax` | Seeks POIs specifically. Judged on `content_score()`, not score. |
| `coward` | Refuses all risk, keeps supplies deep, avoids fire/doom/craters. |
| `rival` | Races for contested POIs to deny them; grabs ground items; makes lopsided trade offers. |
| `gearmax` | Hunts encounter loot for equipment and wears everything it finds, good or bad. Feeds `gearcheck.py`. |
| `biker` | **Wants a Motorbike and nothing else.** Crafts a Backpack, gathers the build (7 scrap, 1 fuel, 1 Sparky Bits), crafts the bike at a Settlement, then rides it collecting all the fuel it can carry. See "The Biker" below. |
| `subterranean` | **Subterranean Explorer.** Lives in the bunker tunnels: maps the corridors, works them for water and scrap, sleeps below, surfaces only for food. |
| `tunnelrunner` | **Subterranean Explorer.** Uses the tunnels as transport: dives, walks to the shaft that surfaces furthest away, climbs out and works the fresh ground. |
| `quester` | **Plays every quest to the end, in order, and checks that the order holds.** A `contentmax` whose encounter planner the quest journal steers. It opens POIs on terrains with open steps first, and Jack's Chopper is visible on the map. Quest goals and items outrank any loot; it wears gate gear and never drops quest items. It probes every gated choice once without its item, and crafts every taught recipe once. Its numbers measure coverage, not balance. See "Quests". |
| `sentinel` | The soak bot. **Makes camp** (water, forage, an existing shelter or a Settlement; never radioactive or a hatch), **builds a shelter** there (improved when it has 2 scrap, upgrades later, rebuilds after a quake), harvests in place, and stays awake until 90% of each day has passed, **then rests in the shelter** — so days run ~4.5 of 5 minutes and it heals every night. Realtime only — arena refuses it in sprint mode. |

All non-`drunk` policies share `SurvivorPolicy`, which owns the survival floor
(eat/drink/rest, emergency staple hunting), encounter handling (the planner),
and the item layer (`policy/itemuse.py`). Its decision is a chain of named
layers (`SurvivorPolicy.layers`, top priority first; `LAYER_METHOD` maps a
name to its method), and every decision's `why` starts with the layer that
made it. Subclasses differ mostly in `pursue()`; one that needs a different
chain overrides `layers` by name (the sentinel keeps camp instead of running
errands, the biker sheds junk first) rather than copying it. Every send
cooldown is a key on the policy's `throttle` (`policy/base.py` `Throttle`),
on a clock a test can move.

**The Creeping Doom and the full pack (2026-10-13).** The first 25-minute
quester run on the K10 lost 28 lives, 21 to thirst and 6 to the Doom, and
both were the bot's doing. Thirst: a pack holding 2 fuel, 2 medicine, 2
scrap and 2 food has no slot for water, a pile is refused with the pack
full (`col_fail` reason 2), and the staple hunt still valued the pile at
100 — so a dry bot stepped onto the pile and off it again every AP of every
day (700 refused pickups in one run) and died beside it. Now a pile counts
only while it would fit (`token_room`), `make_room()` puts down the kind it
holds most of beyond `staple_keeps()` (fuel 0, medicine 1, scrap 1, plus
the canteen's scrap while that is unmade; the Biker keeps by its own plan),
and the hunt keeps its target until reached or gone (`_hunt_lock`) so two
sources cannot shuttle it. The Doom: its position rides every `s`
broadcast, but no policy read it. From awareness 76 `state.py` prices the
three hexes around it into `danger_cost` alongside Kane's reach (60 a hex,
past any one target's worth, so no plan ends beside it); the `doom` layer
(right after `item`) steps to the hex that gains the most distance once it
is within `DOOM_FLEE_RADIUS` and keeps going until `DOOM_SAFE_RADIUS` — the
first rerun stopped at the flee radius, the water hunt walked straight
back, and the next world tick's three steps closed the gap; and
`recovery_target()` leaves a grave it is sitting on for later — walking
back to the grave is how it took the same bot twice. The `weather` layer
behind it answers the chem storm (20 of 69 deaths on that rerun, all in the
open): roof the hex with a scrap at once, any shelter being full cover, or
walk to the nearest Settlement, ruin or roof within today's AP. The quester
also sleeps under a shelter down to its last scrap, because exposure (2 LL
a night, never fatal) was pinning it at LL 1–3, resting its days away —
and since scrap is rare (70 roofs in 2,637 dawns on the third run), the
`bed` layer walks a hurt or late survivor to cover (a shelter or SV ≥ 2
terrain, never a hatch) when it is within today's AP, so the rest heal
climbs instead of trading a point a night with the weather. The
run summary prints `pois/100d` (POIs opened per hundred game-days, the
number the quester is judged on) and a `deaths:` line with causes, Doom
hits, flee steps and room drops.

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
food/water in [1, 6], radiation in [0, 10], a downed survivor at 0 AP, no
move mask while unable to move, steps and tick never going backwards, `sq`
never repeating or going backwards on a socket, at most one death per life
(the double-`EVT_DOWNED` seat-count bug), an acked `m` producing an `mv`, an
acked `act` producing its event, a nacked `act`'s event carrying the same
`bw`, `not_seated` never arriving while seated, and on `/state`:
`connected` equal to the seated players, no reboot, no dropped events, heap
above the pre-crash level.

### Probes

All three need protocol 2 (they judge by ack/nack) and a free seat or two.

**First hardware run, 2026-10-12 (firmware aa784f04, protocol 8).**
- `fuzz.py`: 217 of 221 passed. The two `parse:` cases fail as expected.
  `n:long-1400` and `n:long-4000` got no reply at all: `onWsEvent`
  (network-handlers.hpp) handled a text message only when it arrived in one
  piece, so anything that reached it in two chunks -- over a TCP segment, or
  WS-fragmented -- was dropped with no reply. **Fixed 2026-10-13 (firmware
  59e280e4):** `wsDataEvent()` gathers the pieces per client (up to
  `WS_MSG_MAX`, 4096 B, in PSRAM) and dispatches the whole message;
  bigger is still dropped whole, since its rid cannot be trusted. The corpus
  now expects `frame:fragmented` -> `nack:bad_arg` and has `n:long-6000` ->
  `none`. Re-run: 220 of 222, only the two `parse:` cases.
- **A run with silences in threes followed by `fuzz_socket_closed` is the
  Wi-Fi link, not the firmware.** The first fuzz run on 59e280e4 had 8 of
  them in three bursts (each ~9 s: three 3 s silences, then the board's 5 s
  ACK timeout drops the socket); the next run, same firmware, had none. Run
  it again before reading anything into one.
- `chaos.py`: all six pass. `stalled_reader` held heap at 131 -> 129.5 ->
  131.5 KB. `abort_encounter` and `trade_then_leave` skip on map luck (no
  POI in sight, or B out of AP); run them on a fresh world.
- `walk_to()` used to read the survivor's surface position from the `s` tick,
  which lags its own `mv`, so it always walked one hex past the target and
  those two scenarios failed `not_here` / `not_same_hex`. It now steers by the
  `mv`. Policies still have the same lag -- "moves overshoot" under
  "Deadlocks to avoid".

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
{"t":"nack","rid":N,"cmd":"m","why":"no_ap"}    refused -- nothing changed
```

Without `rid` nothing changes, so the browser is unaffected. A failed skill
check is an **ack**: AP was spent and the outcome rides the usual event. A
nack means the request was refused before it did anything.

| `why` | Means |
|---|---|
| `parse`, `bad_arg`, `bad_act`, `unknown_cmd` | malformed, out of range, or no such `t` |
| `not_seated`, `not_in_lobby`, `slot_taken`, `full` | seat state: act before pick, pick twice, pick a taken seat, pick with `MAX_SEATED` already seated |
| `busy` | `G.mutex` timeout — safe to retry |
| `downed`, `in_enc`, `resting`, `underground` | player state forbids it |
| `bad_dir`, `terrain`, `wall`, `no_ap`, `cooldown` | move refusals (`movePlayer` / `moveTunnel`) |
| `pack_full`, `no_res`, `not_needed`, `archetype`, `craft` | action refusals (`ABW_*`, also on the `act` event as `bw`) |
| `not_here`, `no_poi`, `claimed`, `no_pool`, `load_failed`, `no_enc`, `no_choice`, `cannot_bank` | encounter refusals |
| `trap` | `enc_abort` at a booby trap's start and the escape roll failed: still in the scene |
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

Protocol 4 lists only **seated** players in the tick broadcast `s`, each block
carrying its seat as **`id`**. A seat missing from `p` is offline. Never index
`s.p` by position: `p[0]` is whichever seated player has the lowest id.
`state.py` marks unlisted seats disconnected. `sync` still carries all six.

Protocol 5 adds booby traps: **terrain is TT bits 0-3**, and bit 4 is a
per-recipient "a trap you escaped" flag (`mapdec.py` masks `0x0F` and reads
it as `Cell.trap`). `enc_path`, `enc_start` and `enc_res` carry `"trap"`,
`enc_end` gains reason `escaped`, and there is a new `ev trap`. Details in
"Booby traps" below and [trap-system-spec.md](trap-system-spec.md).

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
  `sync` carried the resting flag, so "resting" and "simply out of AP" were
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
- **Protocol 3 adds the `eco` message** ([ecology-spec.md](ecology-spec.md)):
  ~9 KB every 5 s, plus a scar-only variant. `state.py` drops the vein grid
  and keeps only `dz`, the Wasteland Daisy patches — a bloomed one (stage 3)
  bites for 1 LL on entry, reported as a `dmg` event with `cause:
  "wasteland daisy"` (`causes.py` knows it). `navigate.dijkstra` prices
  bloomed and budding patches through `WorldMap.extra_cost`, so a bot walks
  around the flowers when a detour exists. Everything else in the message
  is cosmetic by contract; `eco_check.py` checks the message shape against
  a live board or the mock.

---

## The bunker tunnels

The second board ([tunnel-system-spec.md](tunnel-system-spec.md)) is 16x10,
walled where the surface is a torus, and reached by stepping onto a hatch.
Two policies play it on purpose — the **Subterranean Explorers**,
`subterranean` and `tunnelrunner` — and the other five have to survive
falling into it, because terrain 12/13 costs 1 AP, which makes a hatch the
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
accidental descent costs about 4 AP to undo — step off the shaft, step back
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

**7. Two floor cells side by side are not necessarily joined.** Since
2026-09-27 the board is one-hex corridors with rooms off them, and there are
walls between cells (`G.tunnelOp`, tunnels.hpp "Open sides"). A step through
a closed side is refused as `"wall"` exactly like rock, and silently from the
bot's side. Each tunnel cell's open sides ride `tsync` and `vis` as `op`
(2 hex chars a cell, in cell order); `Cell.op` holds them (-1 = not sent)
and `navigate.walled()` keeps Dijkstra out of them. `vm` is still the
authority for the step you are about to take.

### What the tunnels charge, and what they do not

The numbers the Explorers exist to measure, all from `tunnels.hpp` and
`actions_game_loop.hpp` rather than from the spec's prose:

| | Underground | Surface |
|---|---|---|
| Move cost | 2 AP flat | ~1.6 AP average |
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

`ap_per_surface_hex` is the headline: AP spent underground per surface hex
actually crossed, against ~1.6 over ground. The spec predicts a 44-67% saving
measured over the generator's own corridor paths with the whole network
known; these bots have one ring of vision and learn the pairings by using
them, so the gap between the two figures is the cost of not having the map.

**Surface distances before 2026-10-12 were over-counted.**
`navigate.hex_distance` folded each torus delta to its absolute value before
combining them, so any crossing where q and r moved in opposite directions
came out long -- (22,20) to (19,22) read 5 instead of 3, and from one sample
hex 2816 of the board's 4275 cells were over-counted. It is now the firmware's
`hexDistWrap()` (`hex_dist_wrap`), and `smoke.py` checks it against a
breadth-first walk of the whole torus. What that moves:

- `ap_per_surface_hex` / `ap_per_hex` read too optimistic: diagonal crossings
  counted extra hexes, so the tunnels looked cheaper than they were.
  `metrics.py` recomputes gaps from the raw `tun_in`/`tun_out` events, so
  re-running it on an old log gives the corrected figure; the per-bot
  `transit_hexes` / `longest_transit` stored in an old run's summary stay as
  recorded.
- `tunnelrunner` ranked exits by that gap (and `MIN_WORTHWHILE_GAP` filtered on
  it), so it favoured diagonal exits more than their real distance justified.
- `coward`'s fire and Creeping Doom radii (2 and 4) and `rival`'s contest and
  trade ranges (8 and 6) were effectively smaller toward opposite-sign
  neighbours, so runs before the fix had a coward that walked closer to fire
  and the Doom than intended.

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
  the AP-saving number from one means nothing.** Take that from hardware.
- **`arena.py` cannot drive the mock** — no `/state`, so `fetch_state()` and
  `verify_reset()` both fail. Drive `BotClient` directly for offline work.

---

## The Biker (the Motorbike build)

The Motorbike is a `starter` recipe (`recipes.cfg` id 20): **7 scrap + 1 fuel
+ 1 Sparky Bits**, crafted at a Settlement. It was 8 scrap until 2026-09-29;
the hardware runs below were at 8. There is no "must be wearing a Backpack"
check anywhere. The gate is the pack: tokens count one per slot
(`tokenLoad()` against `effectiveInvSlots()`), and the build's 8 tokens fill a
bare 8-slot pack with no room for food or water. So in practice the order is:
- a Backpack (5 food), a Hoarder's Rig, or a Mule's 12 slots;
- then the parts;
- then the bike.

The bike adds `slots = +2` of its own (saddlebags, fuelled or not). It is
`trade = no`, so it is out of the spawn draw. Jack's Chopper (scrub 19 → 20)
still gives one away.

```bash
python arena.py --host "$HOST" --bots 3 --policies biker,biker,biker --target 0 --max-minutes 15
```

`policy/biker.py` re-reads its phase off the observation every cycle, so a
death or a free bike just moves it to the right phase:

| Phase | When | What it does |
|---|---|---|
| `pack` | the pack cannot hold the build plus a dawn's food and water (< 12 slots) | gathers 5 food, crafts the Backpack, wears it |
| `parts` | the pack is big enough | the Sparky Bits first, from POIs ranked by how much of each terrain's pool can pay them (`encounters.item_drop_terrains()`); then the scrap from piles and salvage; then one fuel. With it all carried, walks to a Settlement (looking up to 120 AP out) and crafts it |
| `ride` | the bike is owned | fills every slot the survival floor does not need with fuel: fuel piles first, fresh ground second |

Things it had to learn on the mock, each of which applies to hardware:
- **The part before the scrap.** Carrying the scrap while hunting a POI leaves
  room for one dawn of water.
- **Floors sized to the room left.** The survival floors shrink to a lean
  2 water + 1 food only while the pack cannot hold the defaults.
- **Junk is shed.** Past `KEEP_*`, tokens are junk: before the bike, fuel past
  the build's one plus a spare, and scrap past the build; always, medicine
  past one. Junk is trimmed at the bank (`enc_bank` `keep[]`), costs a route
  `JUNK_PILE_COST` extra AP to step on (stepping on a pile takes it), and is
  put down with `drop_res` when the pack needs the room. Before this, a
  Quartermaster sat on 11 fuel it could not burn, with no room for the scrap.
- **Medicine goes on a Settlement heal** (TREAT), and spare scrap on a roof
  on the day's last AP.
- **Nothing it sends is re-sent every cycle when it changes nothing:**
  TREAT, `drop_res` and (in `SurvivorPolicy`, for everyone) SHELTER once per
  hex per day.

`arena.py` prints a `bike:` line per Biker: `phase`, `backpacks` built,
`built` / `found` bikes, the day it first owned one, `fuel` / `peak` /
`gained`, and `ride_moves`.

**On the K10 (2026-09-29).** Four 3-Biker realtime runs of 15 minutes each:

```bash
python arena.py --host "$HOST" --bots 3 --policies biker,biker,biker \
    --target 0 --max-minutes 15 --mode realtime --join-stagger 2
```

- **Two finished and two were stopped.** Each stopped run froze on a harness
  bug that is fixed now (see "Scene bookkeeping" under "Deadlocks to
  avoid"). A bot stuck in a scene never rests, so it held every game-day
  open at 5 real minutes.
- **Run 2 (seed 32, 178 game-days).** The Guide built its Backpack on day 4
  and its bike on day 123, the first Motorbike built on hardware. It then
  made 483 riding moves and collected 51 fuel, until a 1-scrap choice it
  could not afford cost it the last 3 minutes. The Quartermaster and Medic
  built Backpacks on days 57 and 68 and never finished the parts.
- **Run 4 (seed 52, 282 game-days).** No bikes. The Quartermaster and Medic
  built Backpacks on days 143 and 145 and died 6 and 4 times. The Guide never
  got one. The only Settlement near it was about 8 AP from water, and the
  lean floors made every dawn a water emergency.

- **Run 5, at 7 scrap (seed 62, 233 game-days).** Clean: no stuck loops,
  and one lost scene recovered by `lost_scene_action()`. The Quartermaster
  built its Backpack on day 142 and its bike on day 155, then made 526
  riding moves and collected 92 fuel (peak 9, 7 in the tank at the end). The
  Guide built a Backpack on day 33, died twice and lost it to a grave. The
  Medic's Backpack came on day 154. Neither built a bike.
- **Run 6, first with the kick-start (2026-09-30, seed 72, 213 game-days).**
  This was firmware f10f5854: a stalled bike starts on the first fuel found
  (`ev refuel`, the `kickstarts=` count on the `bike:` line). No stuck
  loops.
  - The Quartermaster spawned wearing a Hoarder's Rig (12 slots), so it
    skipped the Backpack. It found Sparky Bits on day 3 and built the bike
    on day 15. It then made 1148 riding moves and collected 125 fuel.
  - Its bike stalled at dawn 15 times, and each time started later that day
    for +5 AP.
  - An encounter hazard killed the Quartermaster on day ~205, and the bike
    went to its grave. The other two were heading there when the run ended.
  - The Guide and Medic had Backpacks by days 6 and 1, then carried 7 scrap
    and 2 fuel for about 200 days. Neither ever found Sparky Bits.

So on hardware the bike is genuinely hard: 1 of 6 finished Bikers built one
at 8 scrap, and 2 of 6 at 7. Too few runs to say the easing moved the rate.
In run 6 the bottleneck was Sparky Bits. Before that, it was surviving while
carrying the build (water, above all). About a third of every Biker's
decisions were emergency water walks. The lean 2-water floor is what makes it possible and what kills
them. The recipe cost is the user's call; if it proves too hard, that floor
is the harness knob to look at.

**On the mock (same day).** Three Bikers (Guide, Quartermaster, Mule), 8 real
minutes, 100 game-days:
- all three built one, the two 8-slot survivors after a day-1 Backpack;
- bikes on days 14-16, then 450-535 riding moves and 43-45 fuel collected
  each.

The mock is still not the economy: FORAGE/SCAV are a flat +1, SHELTER builds
nothing, there are no terrain costs. Take the build rate and fuel figures
from hardware. Three mock bugs the bike exposed are fixed in
`mock-server/server.js`:
- `terrainAt()` returned the wire TT byte, tyre-track bit included, so a
  Settlement a bike had crossed refused CRAFT and TREAT;
- pile pickups were capped at a fixed test value of 6, so no Backpack could
  raise the cap and the build was impossible;
- COLLECT WATER gave +1 whatever AP it cost, half of `doWater()`'s rate.

## Auditing the equipment

`gearmax` + `gearcheck.py` exist to answer a question the other policies
cannot: **does each item in the registry actually do what items.cfg says?**

```bash
python arena.py --host "$HOST" --bots 4 --policies gearmax,gearmax,gearmax,gearmax \
    --target 3000 --max-minutes 15
python gearcheck.py                       # newest run
python gearcheck.py runs/run-2026*.jsonl  # pool several
```

**Why a dedicated policy.** Equipment comes from exactly one faucet --
encounter loot, dealt by `data/encounters/loot_tables.json` on a bank -- so
collecting gear means opening POIs, and the survival policies do not detour
for them. `gearmax` also sets `equip_anything`, which is the part that
matters: the shared `gear_action()` refuses anything whose `gear_score()` is
not strictly positive, so a net-negative item would never be worn and its
penalty would never be observed. GearMax wears junk on purpose. That makes
*its* survival numbers unrepresentative and the *items'* numbers possible.

**What gearcheck can and cannot prove.** It reports per declared mod, never
per item, because "the item did something" is not a test -- an item declaring
`rad -1` and `terrain 4` tells you nothing by leaving `llCap` alone:

| mod | evidence | strength |
|---|---|---|
| `ll` | `llCap` delta across the equip (`appendPackArrays` sends the EFFECTIVE ceiling) | hard |
| `slots` | `is` delta, allowing for the INV_SLOTS_MAX clamp | hard |
| `water_cap` | `wc` delta (`canteenCap()`, same emitter) -- the Canteen | hard |
| `vision` | `vr` on the vis disk `pushVisDisk()` sends straight after the equip | hard |
| `ap` | dawn AP against `ll + 3` + declared, unwounded dawns only | soft |
| `rad` | direction of change at the dawns it was worn | soft |
| `terrain`, `threat`, narrative | **nothing on the wire carries them** | untested |

A mod with no evidence is reported UNTESTED with the reason. That is not a
formality: after a 15-minute run most of the registry's *interesting* effects
-- the terrain perks, the narrative ones -- are still unmeasured, and a
report that quietly passed them would be worse than no report.

The audit is itself tested: `smoke.py` feeds it an item whose mod lands, one
whose does not, one applied twice, a clamp case and a vis disk arriving after
a step, and requires it to tell them apart.

**Reading the survival ranking.** LL lost per game-day worn, against the
fleet's own baseline for dawns where nothing was worn. Dawn LL loss is
mostly exposure, thirst and hunger, which most gear does not touch, so this
is a screen for items that make things *worse*, not a ranking of benefit --
and `days` is printed on every row because a two-day sample is noise.

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
rested. It then sits at `ap == 0`, awake, unable to move until dawn — and
because `tickGame()` only ends the day early when **every** connected player is
resting, one bot stuck like that freezes the whole fleet. Five bots sat frozen
at day 16 this way. Retry on a ~4 s cooldown instead.

**Stale POI bits.** A consumed POI makes `enc_start` fail while the cached map
still shows the bit set (`err "Already looted"`, and a `no_poi` nack on
protocol 2). Cap attempts per hex and fall through to movement.

**Scene bookkeeping on a busy board.** On 2026-09-29 the K10 dropped replies
and unicast messages. `broadcastPartial` reached the thousands with only
three bots, and sockets closed with `1002 expected a continuation frame`;
that last one appears in earlier runs too. **It was the WebSocket library**
(ESPAsyncWebServer < 3.12.0 tore and re-sent frames under send pressure,
upstream #453): on 2026-10-12 a 5-bot run on 3.10.3 dropped all five sockets
at once every 30-100 s and delivered one `enc_start` seven times. On 3.12.1
the same run had 0 torn frames, 0 duplicates and 14 connection errors instead
of 139. The rest was the per-client queue of 8 (`build_opt.h`) discarding
acks, events and `kane_call`s behind state ticks during a multi-second Wi-Fi
stall; `broadcastState()` now caps state ticks at 2 queued per client
(firmware aa784f04), and the next run had 0 connection errors and 7
unanswered requests, all in one stall. A `request_unanswered` burst at one
moment is a stall; a steady trickle is worth a look. Four loops came out of it, all
fixed in `SurvivorPolicy` / `EncounterRun` / `BotClient`:

| Symptom | Cause | Fix |
|---|---|---|
| `enc_bank` nacked `no_enc` forever | the scene ended but its `enc_bank`/`enc_end` never arrived | a `no_enc` nack clears `obs.encounter` and the policy's run |
| every `m`/`act` nacked `in_enc` | `enc_path` (a bare `client->text()`) was dropped, so the bot never knew it was in a scene | `lost_scene_action()`: after `LOST_SCENE_GRACE_S`, `enc_abort` on a cooldown |
| `enc_choice` nacked `no_res` forever | the first "open" choice cost more than the bot carried | `encounters.can_pay()` mirrors `canAfford`, and a nacked choice is struck off at that node (`EncounterRun.refused`) |
| a trap choice re-sent every cycle, then `no_choice` | the next pick went out before the last roll's `enc_res` came back | one choice in flight (`EncounterRun.awaiting`, up to `CHOICE_WAIT_S`) |

**Moves overshooting on a slow board** (fixed 2026-10-13). Policies sent a
move every decide cycle, whether or not the last one's `mv` had come back.
Most extras were refused `cooldown` (about 300 a bot per run), but sometimes
both landed: a Biker one hex from a Settlement stepped onto it and then off
it again, and its CRAFT was refused on the wrong hex. `BotClient` now holds
one move in flight (`MOVE_WAIT_S`), released by our own `mv` or the nack;
`moves_held` in the summary counts the steps it kept back.

**The harness's own failures are findings too.** A policy that raises from
`decide()` is `policy_exception` (and after ten in a row its loop slows to
`POLICY_ERR_BACKOFF_S`); a decision loop that dies is `decider_died`, and the
socket is dropped so `run()` starts a fresh pair of tasks instead of leaving
a bot that reads every tick and never acts; a `run()` task that raises is
`bot_task_died`, and `arena.py` starts it again up to `BOT_RESTARTS` times
before ending the run `bot_crashed`. Ctrl-C now writes the run's summary row
and findings totals before it leaves.

**One decision, one search.** `SurvivorPolicy._decide()` is a priority list
whose layers each run their own `navigate` search from the same hex, most of
them to find nothing to walk to -- eight a cycle, 35 ms a cycle for a quester
on a revealed map, on the one thread that reads every socket. `decide()`
opens a memo (`navigate.begin_cycle`) that lives exactly as long as the call,
so the same search is run once; outside a `decide()` nothing is cached.

---

## Timing: how long is a day?

`DAY_TICKS` (3000) × `TICK_MS` (100) = 5 real minutes — **but** `tickGame()`
ends the day the moment every connected player is resting. An all-bot fleet all
rests when out of AP, so days collapse to a few seconds regardless of `--mode`.
That is genuine multiplayer behaviour, not an artefact: a real party would do
the same.

| Mode | Day length | Use for |
|---|---|---|
| `sprint` | ~2.3 s | Soak and stress only |
| `realtime` | ~3–5 s with an all-bot fleet; 5 min if anyone stays awake | Everything else |

**Sprint distorts balance measurement.** A survivor gets `ll + 3` ≈ 10 AP per
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

**Ceiling: 5 bots, which is also the game's cap.** There are six archetype
seats but at most `MAX_SEATED` (5) filled. `handleConnect` rejects once
`connectedCount + lobbySize >= MAX_SEATED`, and a rejected bot gets
`{"t":"full"}` and retries forever, so 5 bots leave no room for a browser
player (the observer TV polls `/state` and takes no seat). Any stale lobby
entry eats into that budget.

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
| scoremax | +AP, +slots | the economy is "keep moving, prefer piles, keep pack space free" |
| contentmax | vision, +AP, −threat | POIs must be seen to be opened, and the Threat Clock is what makes them lethal |
| rival | +AP, vision | denial is a race |
| subterranean | vision, +AP | tunnel sight is 1 hex, and a corridor step costs 2 AP |
| biker | +slots, then +AP | the Backpack is the bike's gate, and carry room is fuel room; it skips the canteen so the Backpack keeps the body slot |

Two properties are asserted in `smoke.py` because breaking either is silent:
the five policies must want **different** loadouts (otherwise the weights are
decoration), and swapping must reach a **fixed point**. The second matters
because `equipItem()` returns the displaced item to the pack — a policy that
ever prefers what it just took off loops forever at one message per cycle and
looks like nothing at all from either end.

A fuel-gated `ap` is discounted (`GATED_AP_DISCOUNT`), not taken at face
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

## Booby traps

Some hexes are trapped ([traps.hpp](../traps.hpp),
[trap-system-spec.md](trap-system-spec.md)): about one in 25 in open country,
one in five downtown, one in twelve of tunnel floor. Stepping onto an armed
one opens an encounter from the `traps` pool **without an `enc_start`** — the
server pushes `enc_path {"biome":"traps","id":N,"trap":1}` after the move's
vis disk, and the move itself is still acked. Every policy meets them, on
either board, whatever it thinks of encounters.

How a scene goes, and what the bot sees:

| The survivor | Wire | The trap |
|---|---|---|
| backs out (Navigate), or leaves a cache with nothing | `enc_end reason:"escaped"` + unicast `ev trap out:"known"` | still armed, and on this bot's map: TT bit 4 from now on, `sync` included |
| banks anything | `enc_bank`, then `ev trap out:"spent"` | gone, for everyone |
| fails any check | `enc_res out:0 trap:1`, `enc_end reason:"hazard"`, `ev trap out:"sprung"` | gone; the hazard hurt, but **never below 1 LL** |
| is cut off by dawn or a disconnect | `enc_end` | re-armed exactly as it was |

`enc_abort` at the start is not a walk-away: it rolls the back-out check, and
a failure nacks `trap` and leaves the bot in the scene.

What the harness does with it:

- **`decide_trap()`** (`policy/survivor.py`): work it (the Scavenge door) when
  the odds are at least `trap_nerve` (0.6) and LL at least `trap_min_ll` (4),
  otherwise back out; from a cache, press on once if the next check is at
  least `trap_push` and LL has a point to spare, else bank; out of an escape
  node, `enc_bank` with nothing — which is also what the browser sends. The
  coward sets `trap_nerve = 2.0` and never works one.
- **Pathing:** a known trap costs `TRAP_KNOWN_COST` (8) extra AP in
  `navigate.step_cost()` — a detour of most of a day is taken first, but a
  trap is never a wall.
- **Causes:** a failed trap check is its own cause, `booby trap`, not
  `encounter hazard`. It can never be the killing blow (the LL floor), so it
  shows up in the loss ledger and near-death causes, never in deaths.
- **Metrics:** `RunReport.traps()` — forced scenes, escaped / disarmed /
  sprung, LL lost, and scenes per bot — printed as `-- booby traps:` after the
  POI block. Trap scenes count in `stats["traps"]`, not
  `encounters_opened`, so POI reach is unchanged by them.

**The question to answer before flashing a density change** is the brush
rate: forced scenes per survivor-day, LL lost to them, and deaths within a
game-day of a sprung trap. The spec's intent is "scares, not deaths".

## Death drops (and why a fall no longer costs the canteen)

A downed survivor now leaves everything it carried on the hex where it fell
(`dropRemains()`; firmware side in the "Death drops" section of
[dev-loop.md](dev-loop.md)): pack and worn gear as ordinary ground piles,
resource tokens in a remains record that sync and `ground_update` carry as
`rm`. It is anyone's for `GROUND_AGE_DAYS` (30). Before this, every death
wiped the bot's crafted canteen — respawn is a fresh survivor — and it had to
scrape together the scrap for another.

What `SurvivorPolicy` does with it:

- **Stood on a grave, any grave, it takes what is there** (`remains_action()`):
  `{"t":"loot"}` for the tokens when some kind has room (the server takes what
  fits, water first), then `pickup_item` for equipment. This runs *ahead of
  every errand*, and it has to: the canteen errand would otherwise walk a
  respawned bot off its own grave to hunt scrap for a new canteen, and the
  walk-back would then bring it straight back.
- **It walks back to its own grave** (`recover_action()`) while the grave holds
  something it can actually lift — tokens with room, equipment with a free
  slot — and will still exist on arrival. Same two-call shape as the canteen
  errand: within today's AP ahead of the survival floor, up to
  `RECOVER_SEARCH_COST` after it. "Own" is the seat's `pid` on the record, or a
  grave recorded from a `why:"fell"` naming our seat (`Observation.my_graves`),
  because a respawn can land in a different seat.
- **The canteen errand stands down** while its canteen lies on a grave it can
  reach — three scrap for a second one would be waste.
- **Graves on a hatch are never walked to.** A fall underground drops on the
  hatch above, and stepping onto a hatch is the descent, so on foot that grave
  cannot be stood on. Loot and pickup are refused at depth 1 anyway.
- `loots` and `recover_moves` ride the content block, and `arena.py` prints
  them at the end of a run.

Two policies had latent loops that graves made live, both fixed:

- **`rival`'s pickup was a placeholder `noop`**, so it froze on the first pile
  it stood on — any pile, since it walks to all of them. It lifts anything
  underfoot now, and only walks to piles while it has a free slot.
- **`gearmax` walked to piles it never lifts** (consumables), then was drawn
  straight back from the next hex. It only targets equipment now, and only
  with a slot free.

Runs from before this are not comparable on anything to do with carrying:
death used to reset the pack, and now it merely relocates it.

## Machine Gun Kane

A rusted jeep that roams the open road and shoots survivors caught in the
open, and a five-chapter story left behind it ([kane.hpp](../kane.hpp),
[machine-gun-kane.md](machine-gun-kane.md)). What the bots see: the `kane`
object in `sync` and every tick's `world` (`q r m st e cq cr gq gr`), and
three events -- `kane_call` (unicast: he has asked *you*; `camp: 1` is the
harmless question at his fire), `kane_fire` (broadcast: a burst, `hits`,
`llLost`, `ll`) and `kane_arc` (a chapter read, `stage`, `ending`).

**The rule that makes him survivable: he always asks first.** Every world tick
(150 game ticks, ~15 s) he looks for anyone within `KANE_RANGE` (3) standing
on open ground -- scrub, dunes or marsh; everything else is cover. The first
tick he sees you he asks and stops; if you are still exposed on the next one
he fires 3 rounds at 35%, at most 2 LL, and then has to ask again. Never
underground, never into a scene, never at night (he sits at his fire).

What every `SurvivorPolicy` does about it (`policy/survivor.py`):

- **Prices his reach.** `Observation._refresh_kane_danger()` fills
  `WorldMap.danger_cost` with the open ground within range of a jeep on the
  road -- `KANE_DANGER_COST` (4) normally, `KANE_CALLED_COST` (30) once he has
  asked us -- and `navigate.dijkstra` adds it, so routes bend around him.
  Unrevealed hexes in reach are priced at half. In the mock runs this alone
  kept the bots out of range whenever they had a second's warning.
- **Takes cover when asked** (`kane_cover_action()`, first after the scene
  checks): the cheapest hex in today's AP that is cover, out of range or a
  hatch, searched with `danger=False` because crossing his reach on the way is
  free -- only where the move ends is checked. No cover in reach and a POI
  underfoot: open it, since he never fires into a scene.
- **Holds cover** (`kane_hold()`): `decide()` is now a wrapper around
  `_decide()`, and while his question is open a move back into the open is
  replaced by a noop. Subclasses override `_decide()` (sentinel) or call
  `super().decide()` (biker, subterranean), so all of them get it.
- **Plays his chapters to the end** (`decide_kane_chapter()`). A chapter
  counts only when its *last* node is reached, so unlike a POI it is never
  banked early -- the default "bank at the first bankable node" would pocket a
  haul and leave the chapter unread for good. It takes the best odds, never a
  choice that leads to an escape node, and at chapter 5's ending tells the
  truth when its odds clear `kane_truth_nerve` (0.5), the lie otherwise. Below
  `kane_min_ll` (3) it walks away; the chapter keeps.
- **Walks to the next chapter** (`kane_quest_action()`): the camp he left at
  dawn (1-3), the house in the glass (4), his own hex while he sits at his
  fire once that is read (5 -- urgent, since the fire goes out two world ticks
  after dawn). `kane_quest_reach` is the AP it will walk (None = what is left
  today); a hex it stood on with no scene (somebody else is reading it, or a
  failed check threw it out) is left for `KANE_QUEST_RETRY_S`.

| policy | dodges | goes after the story |
|---|---|---|
| contentmax | yes | yes, up to 60 AP, and shadows the jeep (outside his reach, within 7 hexes) before the last chapter so the fire is a short walk at dusk |
| scoremax, rival, gearmax, biker | yes | on the way only: within today's AP |
| coward | yes | never -- every chapter is a string of checks |
| sentinel, subterranean, tunnelrunner | yes | never: it would skew what they measure |

**The story needs days that reach dusk.** He camps at dusk (`dayTick` >= 78%
of the day) and leaves a camp when he drives on. `tickGame()` starts a new day
the moment every connected player rests, so an all-bot fleet whose days
collapse to seconds never reaches dusk and he never camps: no camps, no
chapters 1-3, no chapter 5. He still drives and asks all day. For the story,
run realtime with a `sentinel` holding the days open.

**On the wire after the fact:** `causes.py` books `kane_fire` `llLost` as
`machine gun kane` (and the firmware names it on `downed`); `metrics.py`
prints `-- machine gun kane`: questions, how many were dodged (no burst at
that survivor within `KANE_CALL_TTL_S`), bursts and LL, who read which
chapter and how it ended; `arena.py` prints a `kane:` line per bot. The
`WireOracle` flags a burst with no question before it
(`kane_fired_unasked`), a burst outside its caps, and the arc going back a
chapter.

**`kane_check.py` (mock only)** seats real bots and a staging probe and
drives him with `dbg_kane`. `cover` parks the jeep beside a bot on open
ground, has him ask and fires one beat later (`--gap`, 6 s; a real world tick
is 15). `story` leaves each camp at dawn two hexes from the quester, then the
house (staged beside it if the mock put it out of reach) and his fire, with
real rolls. The mock is not the economy -- its survivors sit at LL 2-3, so
the script relaxes `kane_min_ll` -- and the probe re-seats itself, because it
never eats and dies within a few collapsed days. Measured 2026-10-12 on the
mock (contentmax, scoremax, rival): `cover` -- 9 questions, 9 dodged, 0
bursts; `story` -- the contentmax read all five chapters on real rolls (34
quest steps; it found the mock's own house in the glass 6 hexes off) and told
him the truth, with no Kane findings from the oracles. Nothing on hardware yet:
the firmware with Kane in it is not flashed.

## VACANCY

A neon motel sign over one random Settlement, and a four-part story the
whole party shares, like Kane's arc ([vacancy.hpp](../vacancy.hpp),
`data/encounters/vacancy/1-4.json`, protocol 10). What the bots see: the
`vac` object in `sync`, every tick's `world` and top-level in `/state`
(`q r st rm g dq dr kq kr`: the town, the stage -- 0 lit, 1 counting, 2
saved, 3 fallen -- the rooms left, the bits banked -- 1 the Guest Register,
2 the Master Key -- and the two sites while they are open), and one event,
`ev vac` with `w` = hook / desk / keys / room / saved / fallen / lost (pid -1
for the world's: a room, the fall) or `hum` (unicast: you stood under the
sign while it counted without both halves; `has` is what you DO carry).

**Every part opens server-side on arrival, like Kane's chapters** (`enc_path`
and `ev enc_start` carry `"vac": part`; no POI is consumed):

| part | where | counts when |
|---|---|---|
| 1 Vacancy | the town, while `st` 0 | its last node is reached: `st` 1, 8 rooms, and the Front Desk and Housekeeping are placed 4-8 hexes out |
| 2 The Front Desk | `dq dr`, while open | its last node's Guest Register (70) is **banked** -- walking off at the last node leaves the desk open |
| 3 Housekeeping | `kq kr`, while open | the same, for the Master Key (71) |
| 4 Check-Out | the town, only for a pack holding **both** | its `vac_ending` node: `st` 2, both halves leave the pack (`item_result` act `vac_out`), +40 score |

A dawn with somebody connected takes a room; at none the town falls and its
hex becomes Broken Urban (4).

What every `SurvivorPolicy` does about it (`policy/survivor.py`):

- **Walks to the next part** (`vac_quest_action()`): the town while it is
  unread; while it counts, whichever of the two open sites is nearer (not
  one whose half it already holds); the town again once it carries both.
  Mirrors `vacOnArrival()`, so it never walks under the sign without both
  -- that only hums. `vac_quest_reach` is how far it will walk (None = what
  is left today). A hex it stood on with no scene (somebody else is in that
  part, a failed check threw it out, a booby trap went first) is left for
  `vac_retry_s` (20 s). Below `vac_min_ll` (3) it waits.
- **Two layers.** `vacancy_today` -- a part within today's AP, beside
  `craft_today`, ahead of `bed` and the routine top-ups, never in a staple
  emergency -- and `vacancy`, the longer walk, beside `kane`. The first
  exists because `bed` walks a quester at LL 4 or less back to cover after
  every step it takes toward anything (see the caveat below).
- **Plays every part to its LAST node and banks there** (`decide_vac_part()`).
  The default "bank at the first bankable node" would pocket the desk's water
  and leave the register behind; the bank is what counts for parts 2 and 3
  and is harmless for 1 and 4. Best odds among the choices it may take
  (`requires_item` held, cost payable), never one that leads to an escape
  node -- every Check-Out node has that walk-away first. Below `vac_min_ll`
  it walks away; the part keeps.
- **Keeps the halves.** The quest journal protects 70 and 71 while
  Check-Out is open (never litter, never crafted away, picked up off the
  ground), and lets them go once the sign is out.

| policy | goes after it |
|---|---|
| contentmax, quester | yes, up to 60 AP (`vac_quest_reach`); the quester's journal books it as a quest |
| scoremax, rival, gearmax, biker | on the way only: within today's AP |
| coward, sentinel, subterranean, tunnelrunner | never (a sentinel camped on the sign's town still reads part 1: it opens on arrival) |

**On the wire after the fact:** `metrics.py` prints `-- vacancy`: the town
and how it ended, rooms left, the nights counted, who did which part
(hook / desk / keys / saved with pids), the hums, and the scenes opened by
part and by bot; `arena.py` prints a `vac:` line per bot (scenes, the parts
the board credited to it, quest moves, hums, outcome). The `WireOracle`
flags the stage going backwards over one town (`vac_stage_backwards`), the
rooms rising while it counts (`vac_rooms_rose`), Check-Out opening for us
without both halves (`vac_checkout_ungated`, critical), a save from such a
Check-Out (`vac_saved_without_items`, critical), the pair still in the pack
after the save (`vac_items_not_spent`), and a desk or corridor banked with
its item in the haul that never set the bit (`vac_bank_not_counted`).

**`vac_check.py` (mock only)** seats a quester and a staging probe and
drives the sign with `dbg_vac`; Kane is held still for the duration.

    python vac_check.py [host[:port]] [--parts story,fall] [--policies quester] [--attempts 4]

`story` puts a fresh sign two hexes from the quester; once it reads the
town, the desk and the corridor are staged two hexes from it, and it has to
bank both and come back to check out -- real rolls throughout. It passes on
`st` 2 with `g` 3 and both halves out of the pack. A town that falls is tried
again with a fresh sign and a fresh survivor (the mock forgets a survivor
whose socket closes, so no half carries over), up to `--attempts`. `fall`
stages a counting sign under the probe with one room and runs the dawn:
`fallen` must arrive, and the town's hex must read Broken Urban both on the
bots' maps (from the event) and on a fresh vis disk from the board. One
quester is the default on purpose: two survivors can split the halves
between their packs, and nothing then brings them together, so Check-Out
never opens. The mock's survivors sit at LL 2-5 and its days collapse to a
few seconds, so the script relaxes `vac_min_ll` (1) and `vac_retry_s` (4 s);
a staged hex where a booby trap goes off is moved, because the trap
pre-empts the scene (below).

Measured 2026-10-13 on the mock (quester alone, seeds 7-10): all four runs
pass -- 6 story attempts, 4 towns saved with 4-7 rooms left, 2 lost to the
count before the second half was banked, no bot deaths; `fall` and the vis
read-back clean every time; no VACANCY findings from the oracles. Earlier
the same day, before the last-LL rule and the probe keep-alive, 10 of 25
attempts saved the town. The losses are the mock's daily food and water
chores eating most of each night's AP, plus Check-Out's last door (p ~0.58,
2 LL), which killed a bot at LL 2 before `vac_choice` learned never to
risk the last LL. Nothing measured on hardware yet.

**Caveats found on the way.**

- **A booby trap under a site hides it.** `trapOnArrival` runs before
  `vacOnArrival` (survival_state.hpp:714-720), and `vacSiteOk()`
  (vacancy.hpp:87) never asks about traps, so a desk or corridor placed on
  an armed trap opens the trap on every arrival instead; backing out leaves
  it armed. Only working it (a bot needs `trap_min_ll` 4 and odds over
  `trap_nerve`) or springing it frees the site. The mock's `vacPlaceSite`
  has the same gap.
- **`bed` against every walk.** For a contentmax or quester (`rest_below_ll`
  3) the `bed` layer fires at LL 4 or less whatever the AP, and it sits ahead
  of every walking errand: a long walk steps out of cover and `bed` steps
  straight back. One vac_check attempt spent four minutes doing that 25 AP
  from the town. `vacancy_today` sidesteps it within today's AP; Kane's
  story, the `vacancy` long walk and `pursue` do not.
- **The mock's `sync` lists seated players only** (each with its `id`), and
  `state.py` applies a sync's `p` by index, so after a re-seat on the mock a
  client's own slot is stale until the next `s` broadcast. vac_check stages
  by the probe's own hex server-side (`town: 1`) for that reason.

## Items

Until 2026-10-13 no bot ever sent `use_item`, `car_buy`, `drop_item`,
`trade_accept` or `trade_decline`, and none crafted anything beyond the three
starter recipes. Every Trauma Patch, ration and wound kit a scene paid out
stayed in the pack until the survivor died with it. A recipe banked in a scene
also stayed unknown to the bot until it reconnected, because `kr` rides only
on `sync`; the bots now OR in `ev enc_bank` `recs`, as the browser does.
Every `SurvivorPolicy` now mixes in `ItemUseMixin` (`policy/itemuse.py`) on top
of `items.py`:

| when | what | rule (`items.py`) |
|---|---|---|
| any cycle, free | `use_item` | `choose_consumable`, first match wins (see the list below) |
| mid-scene, before a roll | `use_item` | a threat reducer, only when it drops the clock below a tier (5/10/15/20; `computeEncounterDN` adds +5 risk at each); a heal on the last LL |
| where there is fog | `use_item` | the Net Map once per world. The Diary and Pendulum (reusable) and Static Chew when at least 40% of their disk is unseen (60% for a one-shot). Never Bright Bad Idea when its +2 threat would cross a tier |
| pack has no free slot | `drop_item` | litter: Panic Juice, a material no recipe uses, a key item with no effect. Never a quest item an open step still needs |
| a trade offer to us | `trade_accept` / `trade_decline` | accept when `TOKEN_WORTH` says we come out at least even and the staple floors hold; decline the rest. The board needs both survivors still on the same hex, so this runs before anything that walks |
| at or near a Settlement | `act` CRAFT | any learned recipe whose output it would use (fewer than 2 held) or wear (beats what that slot holds). Within 10 AP, spending only tokens above `token_reserve`. Not the starter recipes, which have their own errands |
| caravan within 12 AP | `car_buy` | the consumable worth the most per token among those it holds fewer than 2 of, paid in spare food/med/scrap/fuel with the least overpayment. Water is refused (why 4) |

`choose_consumable` checks these in order and takes the first that applies:

1. LL 2 or below: the biggest heal. The Uranium Candy only on the last LL.
2. Any wound: a wound kit.
3. A food or water item that lifts a track back over a threshold it **fell
   through** (the `fth`/`wth` bits). That LL comes back.
4. Radiation 5 or above.
5. No tokens left to feed a track at dawn.
6. LL that fits under `llCap` with nothing spilled.
7. AP, only when `wants_ap` (out of a staple).

Only the craft and the walks cost AP, so the free actions sit right after the
grave loot in `_decide()`. Every sender has a cooldown: a refusal changes
nothing, and an unguarded rule re-sends the same message every cycle forever.
`EQUIP_RETRY_S` was the first lesson in that.

**What the firmware does with a used item** (`useItem`, inventory_items.hpp):

- **Food and water move the 0-6 track, not the tokens**, one step at a time
  through `applyFStep`/`applyWStep`. A step back over a threshold the track
  fell through returns that LL.
- **LL** clamps to `llCap`, **radiation** to 0..10, and **AP** changes at once.
- **Key items** with an effect are never spent.
- **Nothing is checked**: no encounter, AP or LL check, and nothing stops a
  wasteful use. That is why the rules above refuse to spill.

The `sentinel` takes only the free half (heals, rations, trade replies). Its
job is to hold its camp, so it never walks to a caravan or a Settlement for an
extra. In sprint mode `SprintPolicy` rests the moment AP hits 0, so AP items
are never used there.

`arena.py` prints an `items:` line per bot: uses by name, extra crafts, caravan
buys, trade replies, litter, reveals, threat items, quest steps and gate
probes. `metrics.py` prints `-- items` from the wire: every item message sent,
uses by name, crafts by recipe, and the `item_result` acts the board
confirmed.

## Encounter planning

`SurvivorPolicy` used to take the first open choice until a node was bankable,
then bank. That was deliberate, to measure whether the content was reachable.
It now plays a scene by expected value (`encounters.plan`):

- **A choice** is worth `p * (what the node it leads to pays + the best of
  what follows) + (1 - p) * (its hazard)`. `p` is the 2d6 check from the bot's
  own stats.
- **A failed roll** costs its hazard. A hazard with `ends_encounter` also
  loses everything still pending, so a survivor with a haul in hand banks
  rather than risk it. A roll that could take the last LL is priced at
  `DOWNED_COST`.
- **Banking** is worth what is pending, plus a little at a terminal node for
  the full clear. Pending is read from the `enc_res` events, not the file.
- **`requires_item` choices** are planned only when the survivor may take them
  (it has the item, worn if it is equipment) and can pay for them.
- **Loot** is valued by `items.Worth`:
  - tokens by need;
  - consumables by what they do;
  - equipment by how much it beats that slot (the policy's own gear weights);
  - unknown recipes high;
  - quest items and quest goal nodes very high for a `quest_drive` policy.

  `novelty_bonus` adds worth to nodes the bot has never seen (contentmax 3,
  quester 1).
- **`encounter_risk`** scales every hazard: coward 2.0, contentmax and
  quester 0.8.

Two fixes came with it:

- **Terminal nodes.** At a terminal node not marked `can_bank`, the old rule
  sent `enc_abort`. That gave up the +10 full clear and ticked the threat
  clock, when `enc_bank` is allowed at any terminal node.
- **Mid-scene items.** An item can now be used mid-scene, before a roll.

Traps and Kane's chapters keep their own drivers (`decide_trap`,
`decide_kane_chapter`). Setting `plan_encounters = False` on a policy restores
the old rule as the baseline arm.

## Quests

**The firmware has no quest system.** There are no quest ids, no per-player
flags and no stage counters. The only tracked progress is Machine Gun Kane's
world-shared `stage` and VACANCY's world-shared sign (`st`, `rm`, `g`). Everything else that reads as a quest is held together
by the pack: one scene pays an item, and a later scene has a `requires_item`
choice that only opens for whoever holds it. For the two oldest chains, only
the choice text says so. `quests.py` derives the catalog from the content where
the content says enough, and declares the rest:

| quest | steps | how the order is held |
|---|---|---|
| Jack's Chopper | `scrub/19` agree to fetch the mask, then `urban/19` find the mask (85%), then `scrub/20` hand it over (pays the Motorbike) | **Not at all.** `scrub/20`'s only choice never asks for the mask (`quest_ungated`), and nothing says you met Jack (`quest_out_of_order`) |
| The Sticky Note | `scrub/18` take the note, then `urban/15` present it and fight the Router (pays the Net Map) | **Not at all.** "Present the Sticky Note" has no `requires_item` |
| The Perambulator | `scrub/21`, `glass/3`, `ridge/3` (one graph on three terrains): items 66-69, and four gated shortcuts they open (`in_tune` 66, `remembers` 68, `leaning` 67, `tyre_pulse` 69 worn) | `requires_item`, enforced by the board. A visit pays at most 3 items (`ENC_MAX_ITEMS`) and makes one arrival and one flatbed choice, so three visits cannot open all four gates |
| The Null Camp | `meridian/null_camp/1-3` | **Unreachable.** No pool in `index.json` deals it |
| Machine Gun Kane | chapters 1-5 | the board's own `stage` (see "Machine Gun Kane") |
| VACANCY | `vacancy/1` read the town, `vacancy/2` bank the Guest Register (70), `vacancy/3` bank the Master Key (71), `vacancy/4` Check-Out (needs 70 + 71) | the board's own sign: the sites open only once the town is read, a site counts only on the bank, and Check-Out opens only for a pack holding both -- whose two `requires_item` choices the board enforces too (see "VACANCY") |
| The workshop | `settlement/15-21` teach recipes 1-17, then each one is crafted | the recipe bit (`kr`) |

`python quests.py` prints the catalog and lints it. The three problems in the
table are what it reports today. smoke.py checks that every declared node and
gate still exists.

**Every bot keeps a journal** (`QuestJournal`), so a step any policy stumbles
through reaches the fleet's ledger, `quests.LEDGER`. `arena.py` prints the
ledger and saves it in the run summary, and `metrics.py` reads it back as
`-- quests`. The journal is also an oracle; its findings go through the bot's
`WireOracle`:

| check | severity | means |
|---|---|---|
| `gate_bypassed` | critical | a `requires_item` choice ran for a survivor without the item |
| `quest_ungated` | major | a step that needs an item (per the catalog) went through without it: the content never asks for it |
| `banked_item_missing` | major | `enc_res` said an item was won and the scene was banked, but `ITEM_ARRIVAL_S` later the item is not in the pack, worn, or on the hex |
| `quest_out_of_order` | minor | a chain step finished before an earlier one: nothing orders the chain |

**The quester** (`policy/quester.py`) is the policy that drives these chains:

- **Finds the steps.** It hunts POIs on terrains with open steps (2.5x). It
  walks to Jack's Chopper on sight; the Open Scrub art variant 10 makes it the
  one quest POI the map shows.
- **Plays them.** In a scene, quest goals and quest items rank above any loot.
  It wears gate gear (the Dirty Glove) while a gate needs it, and keeps quest
  items.
- **Probes the gates.** It tries every gate once without its item and expects
  `no_item`. On the mock, which never replies, the expected result is silence
  and no roll.
- **Crafts and reads.** It crafts every taught recipe once fleet-wide, and goes
  after Kane's chapters like a contentmax.

On the board a quest step is found only by opening POIs. Every file in a pool
is placed exactly once (Phase 5), so a fleet that opens every POI meets every
step, in whatever order the map deals them.

**`quest_check.py` (mock)** stages each chain on the quester's own seat with
`dbg_enc`. It forces the rolls with `dbg_force` unless you pass
`--real-rolls`:

    python quest_check.py [localhost:8765] [--parts jack,ungated,sticky,peram,workshop,items]

| part | what it stages |
|---|---|
| `jack` | the chain in order: accept, mask, hand-over, then the Motorbike, with no finding |
| `ungated` | the hand-over again, from a fresh journal and without the mask; the oracle must flag it |
| `sticky` | the Sticky Note chain |
| `peram` | all three Perambulators: items first, then gates, with every gate probed and refused |
| `workshop` | every lesson, then every recipe crafted on a `dbg_settle` Settlement |
| `items` | the heal, wound kit and litter rules |

Kane is covered by `kane_check.py --policies quester,...`, VACANCY by
`vac_check.py`.

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
| per game-day | wasteland daisy | bites on *entry*, so it scales with steps (~7 a day), not with wall time; the patches themselves grow a stage per dawn |
| per game-day | booby trap | fires on *entry* like the daisy; static at worldgen, and never takes the last LL |

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
and its counter-play costs 1 scrap and 1 AP, always succeeds, and pays 4
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
last AP of a day spent on exposed ground, and the two arms are run and pooled
separately. Anything that moves between the arms is policy, not balance.

It is placed ahead of the emergency staple hunt, but only fires at `ap <= 1`
(or when LL is already critical). On the last AP of a day one more step
cannot reach water — most terrain costs 2–4 AP to enter — while the shelter
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
against 3 for a 2-AP FORAGE. **Pile density is the highest-leverage balance
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

**Root cause, restated.** The earlier reading blamed `effectiveAP = ll + 3`
and the scarcity of water terrain. The cause data says the spiral is real but
the *terminal* step is exposure, and the chain is:

1. Water terrain is 4.7% of the map, so W falls to its floor and latches.
2. A latched W track means `restedWell` (`food >= 4 && water >= 3`) is false.
3. `restedWell` is the **only** source of the +1 LL that cancels the −1
   exposure tick, outside a Settlement.
4. So the moment W latches low, every dawn is a guaranteed −1 with no ceiling,
   and `effectiveAP = ll + 3` removes the mobility needed to fix it.

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
  generic act stub, which drains AP and then *adds* a token to `inv[a % 5]`
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
