# Bot testing — autonomous play harness

Canonical reference for `bots/`: Python clients that play the game over `/ws`
against a real K10 so balance can be **measured** instead of guessed. Companion
to [dev-loop.md](dev-loop.md), which covers build/flash/deploy.

Purely additive — the harness touches no firmware and no `data/`. Everything it
knows about the game it learns through the same WebSocket a browser uses.

## For AI coding agents (read first, ≤ 1 min)

- **Self-test, no board:** `cd bots && python smoke.py` (119 checks). Run this
  before every live run; it catches a broken parser in 2 s instead of wasting
  10 minutes of hardware time.
- **Run an arena:** `python arena.py --host 192.168.4.234 --bots 5
  --policies scoremax,contentmax,coward,rival,scoremax --target 1000`
- **Analyse:** `python metrics.py` (newest run) or `python metrics.py runs/run-*.jsonl`
- **Deps:** Python 3.12 + `websockets`. Nothing else.
- **Power-cycle the board before any run whose numbers you intend to trust.**
  `maxTickMs` and `minHeap` in `/state` are high-water marks that never reset.
- **Five protocol rules will silently waste a run if you get them wrong** —
  see "Protocol rules" below. Every one of them cost a wasted run to find.
- **Never `nohup` a background run.** The wrapper gets killed and the Python
  process survives, keeps playing, and blocks the next run's reset.

---

## Layout

| File | Role |
|---|---|
| `config.py` | Constants mirrored from the firmware. **No drift detection — update by hand.** |
| `mapdec.py` | Port of `data/map-decoder.js`. Full map + vis-disk decode. |
| `state.py` | Message → `Observation`. Deliberately tolerant of unknown/missing keys. |
| `navigate.py` | Dijkstra over the fogged torus using `TERRAIN_MC`. |
| `encounters.py` | Encounter JSON from local disk; ports `computeEncounterDN` + the 2d6 table. |
| `client.py` | One bot: transport, rate limiting, slot claiming, connection lifecycle. |
| `policy/` | `decide(obs) -> Action`. Swappable. |
| `arena.py` | Supervisor: reset, spawn, run to target, loop. |
| `telemetry.py` | Polls `/state` for board health alongside the game log. |
| `metrics.py` | Post-run analysis. |
| `record.py` | JSONL per run into `runs/` (gitignored). |

### Policies

| Name | Behaviour |
|---|---|
| `drunk` | Uniform random over legal actions. Soak/fuzz only. |
| `scoremax` | Chases resource piles and unexplored hexes. Opens a POI underfoot but never detours for one. |
| `contentmax` | Seeks POIs specifically. Judged on `content_score()`, not score. |
| `coward` | Refuses all risk, keeps supplies deep, avoids fire/doom/craters. |
| `rival` | Races for contested POIs to deny them; grabs ground items; makes lopsided trade offers. |

All non-`drunk` policies share `SurvivorPolicy`, which owns the survival floor
(eat/drink/rest, emergency staple hunting) and encounter handling. Subclasses
differ only in `pursue()`.

### Useful flags

```
--bots N            1-6. Leave a slot free if you want to watch in a browser.
--policies a,b,c    cycled across slots
--target N          score that ends the run
--mode sprint|realtime
--runs N            auto-loop with a cap
--seed N            whole run is reproducible from this
--no-reset          keep the current world (skips eraseslot+regen)
--max-minutes N     safety timeout
--min-interval      per-bot seconds between sends (default 0.30)
--global-interval   arena-wide seconds between any two sends (default 0.12)
```

---

## Protocol rules

Each of these is silent when violated — no error, no nack, just nothing
happening. All were found the expensive way.

**1. The archetype index IS the player slot.** `handleMsg_pick` does
`Player& p = G.players[arch]` and returns silently if that slot is already
connected. `SlotBroker` stops two bots claiming one slot.

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

- **No `rt` in the periodic broadcast.** Only `sync` carries the resting flag,
  so "resting" and "simply out of MP" are indistinguishable from `s` alone.
  REST is therefore retried on a cooldown, never latched — see "Deadlocks".
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

**Stale POI bits.** A consumed POI makes `enc_start` a silent no-op while the
cached map still shows the bit set. Cap attempts per hex and fall through to
movement.

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

For true 5-minute days (weather, fire, flood, Creeping Doom all tick on their
own clocks) you would need a policy that deliberately stays awake. Not built.

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

---

## Findings so far

Measured on hardware, 5 bots, day 85, target 1000.

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

**Tension is over budget and the wrong shape.** 6.1 projected brushes per
2-hour session against a target of 4, a 1:1.1 death-to-near-miss ratio (should
be nearer 1:4), 54% of time at full health, and **LL 4 occupied only 2.4% of
the time** — the wounded-but-coping band is a transit state, not a place
characters live.

Root cause: `effectiveMP = ll + 3`. Losing health costs mobility, and mobility
is how you reach water — which is the actual killer (bots dry 41–54% of the
time; water terrain is 4.7% of the map once the raft-gated River is excluded).
The spiral is self-accelerating, which is why brushes convert to deaths instead
of recoveries.

---

## Known issues

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
- `config.py` mirrors firmware constants by hand. Nothing detects drift.
