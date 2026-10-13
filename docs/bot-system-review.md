# Bot system review — 2026-10-13

Scope: `bots/` (17.7k lines of Python: the client, the wire layer, the
observation model, the pathfinder, `SurvivorPolicy` and its twelve policies,
the arena, the oracles and findings, the probes and the analysis tools).
Method: read every runtime module, profiled one decide cycle per policy on a
fully revealed map, and ran `smoke.py` and `quest_check.py` before and after
each change. Nothing here is committed.

Two columns of status: **done** means it is in the working tree and `smoke.py`
passes (712 checks); **proposed** means it is a design call for you.

## What is sound

- The wire layer is honest about the board's silent failure modes and says so
  in the code: slot claiming through the lobby, the pick watchdog, the seat
  loss detection, reply tracking with expiry, every refusal logged.
- Every connection is a tester (`WireOracle`), every run ends with findings,
  and a finding carries its repro. That is the right shape and it has paid
  for itself repeatedly.
- Policies are pure decision functions over an `Observation` the client owns;
  `smoke.py` can drive any of them against a canned observation.
- `Observation.apply` is tolerant of additive firmware changes, and the two
  boards (surface torus, walled tunnel grid) are kept apart on purpose.

## Performance

### P1. `decide()` runs on the socket thread, and it was expensive — done

Every bot's `decide()` runs synchronously on the one asyncio thread that also
reads every socket. Measured before this review (fully revealed map, the
state a Net Map or a long sprint run produces):

| policy | ms per decide, before | after |
|---|---|---|
| scoremax | 9.4 | 4.8 |
| contentmax | 19.1 | 10.5 |
| quester | 34.7 | 20.5 |
| rival | 22.3 | 12.3 |
| coward | 5.7 | 2.9 |
| `dijkstra` alone, max_cost 60 | 18.0 | 11.1 |
| `dijkstra` alone, max_cost 40 | 8.4 | 5.7 |

Five questers at three cycles a second was ~0.5 s of CPU per second of wall
clock, all of it blocking the readers: ticks, acks and `enc_res` events
queued behind it, which is the shape of the "choice re-sent before its roll
came back" loops the doc already records.

Three causes, three fixes:

1. **`SurvivorPolicy._decide()` ran up to eight searches a cycle** from the
   same hex over the same map (the grave twice, the canteen errand twice, the
   staple hunt, the caravan, Kane, pursuit), most of them to find nothing.
   `navigate` now memoises searches for exactly the length of one `decide()`
   (`begin_cycle` / `end_cycle` in `SurvivorPolicy.decide`); outside a decide
   nothing is cached, so tests that mutate a map between searches are
   unaffected.
2. **The inner loop of `dijkstra` went through `WorldMap.__getitem__`,
   `in_bounds`, `step_cost` and `getattr` for every neighbour** — a million
   calls per 20 cycles. It now indexes the grid directly and reads the terrain
   cost from a tuple. `frontier_bonus` got the same treatment.
3. **`QuestJournal.open_steps()` was called 14 times a cycle** (the item layer,
   the gear layer, pursuit). It is cached on `(len(done), kane_stage)`;
   `done` only grows, so its size is a version.

### P2. What is left in a cycle — proposed

Half of what remains is `best_target` scoring 3,600 reachable cells with a
`value()` closure that calls `frontier_bonus` for each. Three options, in
order of value:

- **Plan persistence.** Keep the chosen target between cycles and re-use the
  `first` direction while the target is still valid (unchanged cell, still
  reachable, position moved toward it). Pursuit would then search once per
  step rather than once per 0.35 s cycle — a 3-10x cut for the policies that
  walk, with no change to what they choose.
- **Incremental frontier counts** in `WorldMap`: maintain a per-cell count of
  fogged neighbours, decremented in `apply_vis` / `load_full`, so scoring
  reads an int instead of six lookups.
- **A narrower routine search** (max_cost 25) with the wide one (60) every
  Nth cycle or on arrival.

### P3. The run log records every broadcast five times — proposed

Each bot writes every non-tick message it receives to `rx`, and `ev` is
broadcast to every client, so a 5-bot run holds every event five times and
`metrics.py` / `causes.py` dedupe on `(ts, pid)` before counting. A `world`
channel written once per run by the lowest seated bot would cut logs by
roughly 60% (`runs/` is 403 MB over 240 runs) and remove the dedupe pass.
It touches every reader, so it is a deliberate change, not a quick one.

### P4. Startup — fine

`config.py` 13 ms, the shared encounter library 150 ms, the quest catalog
7 ms, per process. Nothing to do.

## Resiliency

### R1. A bad `Action` killed the decision loop silently — done

`Action.to_msg()` validates (an `enc_start` without `q`/`r`, a `car_buy`
paying water) and raises, and it ran *outside* the `try` in
`BotClient._decide_loop`. One bad action ended the decider task; the socket
kept reading ticks, so the bot looked seated and alive in every report and
never acted again until its socket happened to drop. Now: `to_msg()` is inside
the `try`; the reader checks `decider.done()` and drops the socket so `run()`
starts a fresh pair of tasks; a decider that died by exception is a
`decider_died` finding.

### R2. Policy exceptions were a log line nobody reads — done

A policy raising every cycle wrote `policy_err` every 0.35 s and nothing
else. It is now a `policy_exception` finding (signature: policy name and
exception type, with the repro buffer), and after ten in a row the loop slows
to `POLICY_ERR_BACKOFF_S`.

### R3. Ctrl-C lost the run summary — done

Ctrl-C reaches a task as `CancelledError`, not `KeyboardInterrupt`, so the
`except KeyboardInterrupt` in `run_once` never fired and the cancellation
escaped past the summary: no `run` row, no findings totals, and `metrics.py`
read the run as crashed. `run_once` now books `reason = "interrupted"`,
prints and writes everything, then re-raises; `main_async` writes the batch
summary for the runs that finished.

### R4. One dead bot task ended the whole run — done

`run()` catches everything the socket can throw, so a task that still dies
is a harness defect; `arena.py` now records it as a `bot_task_died` finding
and starts the bot again (its state survives, as it does across reconnects),
up to `BOT_RESTARTS` times before `bot_crashed`.

### R5. Replies were only expired on send — done

`ReplyTracker.expire()` ran inside `_send()`, so a quiet bot (resting,
holding cover from Kane) carried unanswered requests unexpired for as long
as it was quiet, and `unanswered` under-counted. The decide loop expires them
every cycle as well.

### R6. Moves overshot on a slow board — done

The doc listed this as still open. `BotClient` now holds one move in flight
(`MOVE_WAIT_S`, released by our own `mv` or the nack) and counts what it
held back as `moves_held` in the summary. Expect the ~300 `cooldown` nacks a
bot a run to fall to near zero.

### R7. Reconnect storms and ping timeouts — proposed

- `--join-stagger` spaces the *first* joins, but a seat-loss event unseats
  every bot at once and they reconnect together, each costing the board a
  full `sendSync()` — the same burst the stagger exists to prevent. Apply the
  stagger (or a jittered backoff keyed on the number of bots reconnecting) to
  reconnects as well.
- `ping_timeout=20` on a board that stalls for seconds under `regen` and
  dawn saves turns a stall into a disconnect and a re-pick. Worth measuring
  on the K10 before changing; a longer timeout trades a slower detection of a
  real hang for fewer self-inflicted reconnects.

### R8. Constant drift — proposed

`config.py` mirrors firmware constants by hand and says so. `smoke.py`
already reads `DC_NAME` and the `CMD_IS` dispatch table out of the firmware
source; extend that to `MAP_COLS/ROWS`, `TERRAIN_MC`, the `ACT_*` ids,
`MAX_SEATED` and `PROTO_VERSION`, which are the ones whose drift would
silently corrupt every path and every metric.

## Architecture

### A1. `SurvivorPolicy` is a 1,700-line priority list — done

(Implemented later the same day: `SurvivorPolicy.layers` + `LAYER_METHOD`,
`run_layers()` prefixing every `why` with the deciding layer; the sentinel's
chain is `("climb_out", "asleep", "gear", "trade_reply", "item", "bedtime",
"hold_camp", "survival", "camp_errand")` and the biker's is `("shed_junk",)
+ SurvivorPolicy.layers`. smoke.py checks that every layer name resolves on
every registered policy. 750 checks.)

`_decide()` is a hand-ordered chain of ~17 layers, each `act = self.x(obs);
if act: return act`. The ordering *is* the design and it is well commented,
but every subclass that needs a different order re-implements the preamble:
`SentinelPolicy._decide` copies the first six layers, `BikerPolicy.decide`
wraps and overrides three methods, the tunnel policies override `decide` and
`pursue`. Proposal: make the chain data —

```python
layers = ("remains", "trade_reply", "item", "recover_today", "craft_today",
          "survival", "kane_urgent", "gear", "reveal", "recover", "craft",
          "craft_extra", "caravan", "kane", "poi", "climb_out", "pursue")
```

— resolved to bound methods once, so a subclass reorders, removes or inserts
by name, and every decision's `why` can be prefixed with the layer that made
it. The chain's behaviour does not change; what changes is that the sentinel
and the biker stop carrying private copies of it.

### A2. Fifteen hand-rolled cooldowns on `time.monotonic()` — done

(Implemented later the same day: `policy/base.py` `Throttle` with `ok`,
`ready`/`mark`, `hold`/`held`, `clear`, on `Policy.clock`; every cooldown in
survivor.py, itemuse.py, sentinel.py and biker.py is a key on
`self.throttle`. `rest_once` and `EncounterRun.sent_at` were left as they
were. smoke.py drives it with a fake clock.)

`_equip_sent`, `_pickup_sent`, `_loot_sent`, `_use_sent`, `_drop_sent`,
`_trade_sent`, `_car_sent`, `_reveal_sent`, `_craft_sent`, `_shelter_sent`,
`_rested_day`, `_kane_block`, `_lost_abort`, `_probe`, `EncounterRun.sent_at`:
83 `time.monotonic()` sites across the policies, each a slightly different
shape of "do not re-send this key for N seconds". `ReplyTracker` and
`WireOracle` take an injectable clock; the policies do not, so none of these
cooldowns can be exercised in `smoke.py` without sleeping. One `Throttle`
class (`ok(key, period)`), with the clock on the policy, replaces all of them
and makes every cooldown testable.

### A3. Module-level singletons — proposed

`quests.LEDGER`, `quests.catalog()`, `policy._SHARED_LIBRARY`,
`encounters._LOOT_TABLES`, and now `navigate._memo` are process globals.
They are reset per run by convention (`QUEST_LEDGER.reset()` in `run_once`),
which means two arenas in one process is impossible and every test that
touches the ledger has to remember to reset it. A `RunContext` (library,
catalog, ledger, findings, recorder) passed to `policy.make()` and
`BotClient` would replace the convention with a scope.

### A4. `Observation.apply` — proposed

350 lines of `if t == ... elif k == ...`. A handler table
(`{"sync": self._on_sync, "s": self._on_tick, ...}` and the same for event
kinds) reads better and makes "which messages do we handle" greppable. No
behaviour change; low priority.

### A5. `smoke.py` is a 3,400-line script with its own `check()` — proposed

It has excellent fixtures (`surface_obs`, `underground_obs`, `kane_obs`,
`item_obs`, `_FakeWS`) buried in one file that runs top to bottom, so one
failure early can mask later sections and nothing runs in CI. Splitting it
into `bots/tests/test_*.py` under pytest, with the fixtures in `conftest.py`,
keeps every check and gains selection, isolation and a runner. Mechanical but
large; do it in one sitting.

### A6. Smaller

- `stats` is a plain dict that four mixins add keys to; a `Counter` with
  `content_score()` flattening the sets would stop the `KeyError` class of
  bug when a mixin's `_init` is skipped.
- `run_once` mixes supervision with 100 lines of report printing; the
  printing belongs next to `metrics.py`'s renderers.
- `PlayerState` declared `inv_slots` twice and `_digest()` wrote `eq`/`is`
  twice (with `llcap` and `llCap`) — fixed.

## The quest check (mock), while here

`quest_check.py` reported 3 failures on its first full run. All three were
the harness, not the content:

| check | cause | fix |
|---|---|---|
| "which it reads (the whole map)" | the bot read the Net Map on the first cycle after the bank, before the script took its baseline | baseline taken before the scene |
| "crafted every one of them" | the mock sends no `ev act a=5` for a craft (the firmware sends `ar`), so `journal.on_craft` never ran | `SurvivorPolicy._book_craft` reads the craft off the `item_result` ack (`recipe`), which both boards send |
| "a major wound: it used the Anti-Rot Kit" | the kit was used inside `give()`'s settling sleep, before the baseline | baseline taken before the items land |

The content findings stand: `jack.deliver` ungated (the hand-over never asks
for the mask) and out of order, the Perambulator's gates refuse without the
item (8 probes, all refused), the Null Camp unreachable.

## After the first K10 run (2026-10-13): the quester's two killers — done

The 25-minute quester run (`bots/runs/questers-k10-20261013.txt`) was the
"before" measurement: 28 deaths, 21 thirst and 6 Creeping Doom, and POIs
opened at 20 in 317 days. Both killers were bot defects, not balance:

- **A full pack starved the hunt.** With 2 fuel / 2 med / 2 scrap / 2 food
  the pack had no slot for water; a pile is refused when full, and
  `staple_hunt` still valued it at 100, so the bot shuttled on and off the
  same pile every AP (700 refused pickups). Fixed: piles count only with
  room, `make_room()` drops the spare kind beyond `staple_keeps()`, and the
  hunt locks its target (`_hunt_lock`).
- **Nobody read the Doom.** Its position rides every `s` broadcast; no
  policy used it, and one bot at LL 1 walked back onto its hex for twenty
  world ticks running, then walked back to its own grave beside it. Fixed:
  `state.py` prices the ground around it into `danger_cost` from awareness
  76, a `doom` layer steps away within `DOOM_FLEE_RADIUS`, and
  `recovery_target` skips a grave beside it.
- The quester now shelters down to its last scrap (exposure pinned it at
  LL 1–3), and the summary prints `pois/100d` and a `deaths:` line.
- **Second rerun** (`questers-k10-20261013b.txt`): POIs 20 → 47 and score
  up 2×, but deaths 28 → 69. Two new shapes in the log: the flee stopped at
  its radius and the hunt walked back into the Doom (fixed: hysteresis to
  `DOOM_SAFE_RADIUS`, danger radius 3 at cost 60), and the chem storm
  killed 20 in the open (fixed: a `weather` layer roofs the hex or walks to
  cover). Third run: `questers-k10-20261013c.txt`.
- **Third run**: deaths 69 → 10, water solved (canteens crafted, 2,414 of
  2,637 dawns at water 6), no Doom contact, no chem storm that run. But
  2,226 dawns at LL 2–3 and 2,399 exposed: the rest heal (+1, +3 under LL
  3) against exposure (−2) is a band the bot never leaves, and it rests
  half its days in it, so POIs fell to 25 in 531 days. Fixed: a `bed`
  layer walks a hurt or late survivor to cover within today's AP. Fourth
  run: `questers-k10-20261013d.txt`.
- **Fourth run** (the "after"): 332 days, 19 deaths (5.7/100 days vs 8.8
  before; 13 thirst, 3 chem, 0 Doom), 34 POIs (10.2/100 days vs 6.3),
  13.4 steps a day vs 8.2, dawns at LL ≤ 3 down from 80% to 24%, exposed
  dawns from 64% to 46%, critical-LL rests 665 → 129. Thirst is what is
  left, and it is the game's intended pressure. Caveat: another session's
  5-bot `drunk` arena contended for seats 1 and 4 for about 80 s of this
  run (t ≈ 100–180 s); no regen happened, the day count is continuous.

## Validation

- `python smoke.py`: 712 checks passed, before and after; 801 after the
  K10 follow-up above.
- `quest_check.py` on the mock: re-run after the fixes (result in the session
  report).
- Profile: `scratchpad/prof_bots.py` (fully revealed map, one hex, 30 cycles
  per policy), numbers above.
- Not run on the K10. The client changes (one move in flight, the decider
  watchdog) change the wire behaviour and should get one arena run before
  the next balance batch is trusted.
