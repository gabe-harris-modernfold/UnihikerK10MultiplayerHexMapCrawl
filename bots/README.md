# bots/ — autonomous play harness

Python bots that play the game over `/ws` against a real K10, so the balance
can be measured instead of guessed. Purely additive: nothing here touches
firmware or `data/`.

```bash
python smoke.py                                    # offline self-test, no board
python arena.py --host 192.168.4.234 --bots 4      # run an arena
python arena.py --host 192.168.4.234 --bots 1 --no-reset --max-minutes 1
```

## Layout

| File | Role |
|---|---|
| `config.py` | Constants mirrored from the firmware. **No drift detection — update by hand.** |
| `mapdec.py` | Port of `data/map-decoder.js`. Full map + vis-disk decode. |
| `state.py` | Message → `Observation`. Deliberately tolerant of unknown/missing keys. |
| `client.py` | One bot: transport, rate limiting, slot claiming, decide loop. |
| `policy/` | `decide(obs) -> Action`. Swappable; `drunk` is the soak policy. |
| `arena.py` | Supervisor: reset, spawn, run to score target, loop. |
| `telemetry.py` | Polls `/state` for board health alongside the game log. |
| `record.py` | JSONL per run, into `runs/`. |

## Board limits (measured 2026-09-19, firmware at commit e4c3517+)

**Read `/state` numbers only against a known uptime.** `maxTickMs` and
`minHeap` are high-water marks that never reset, so a board that has been up
for hours reports the worst moment it ever had, not its current health. An
earlier revision of this file quoted "95 ms idle with 0 clients" as a
baseline; that was a stale high-water mark from a prior browser session. A
freshly booted board idles at **`maxTickMs` 3 and `minHeap` ~185 KB**.
Power-cycle before any run whose numbers you intend to trust.

From a clean boot, four bots over ~10 minutes:

| Metric | Fresh boot, 0 clients | 4 bots, sprint |
|---|---|---|
| `maxTickMs` (budget 100) | 3 | 305 → 805 |
| `minHeap` | 184 964 | 53 084 |
| `broadcastPartial` | 0 | 0 |
| `broadcastSkips` | 0 | 0 |

The 805 ms spike is almost certainly `saveGame()`: `tickGame()` calls it on
every dawn, and sprint mode produces a dawn every ~2.3 s, so an SD write that
would normally happen once per 5 real minutes runs constantly. Worth knowing
for normal play too — a dawn save can stall the game loop for most of a
second.

`broadcastPartial` and `broadcastSkips` both stayed at 0 across these runs,
so with 4 clients the fan-out is keeping up. Inbound traffic is negligible
either way: the bots send a few messages per second at most.

**Practical ceiling: 4 bots.** `handleConnect` rejects once
`connectedCount + lobbySize >= MAX_PLAYERS` (6), and a rejected bot gets
`{"t":"full"}` and retries forever — so 5 bots plus a browser does not fit,
and any stale slot eats into the budget.

## Sprint mode distorts balance measurement — use it as a stress test only

Sprint reaches ~2.3 s per game-day (a ~130× speedup), which is excellent for
soak testing and useless for tuning. A survivor gets `ll + 3` ≈ 10 MP per day,
enough for six or seven moves, but at a 2.3 s day and a shared send budget
each bot only gets four or five *messages* per day. The bots therefore play
every day with roughly a third of their action budget, which makes survival
look far harder than it is and starves the economy of the moves that drive it.

Use `--mode realtime` for anything you intend to draw a balance conclusion
from. At 5 real minutes per game-day a 30-day run is ~2.5 hours, which is
what the auto-loop is for.

## Gotchas the firmware imposes

- **The archetype index IS the player slot** (`handleMsg_pick` does
  `Player& p = G.players[arch]`). Two bots must never claim one slot —
  `SlotBroker` coordinates that.
- **A refused pick is silent.** No nack, and a lobby client still receives
  every `broadcastState()` because it uses `ws.textAll()`. Receiving state is
  *not* evidence of having joined. The only ack is the unicast `sync`; the
  pick watchdog waits for it and reconnects if it never comes.
- **`regen` keeps score and steps** on connected players, so the reset is
  `eraseslot` ×6 (which zeroes them) *then* `regen`. Run termination is
  score-based, so a stale score would end the next run instantly.
- **`regen` deletes both SD save files.** Use `--no-reset` to leave a world
  alone.
- **Encounters gate movement.** The server refuses `m` and `act` while one is
  open; policies must answer with `enc_choice`/`enc_bank`/`enc_abort`.
- **Sprint mode** exploits `tickGame()` ending the day the moment every
  connected player is resting. Measured ~4.3 s/game-day vs 300 s — a ~70×
  speedup. But `weatherNextGapMs` pins real weather changes to a ~1.1–1.9
  real-minute floor, so sprint runs barely exercise weather. Use
  `--mode realtime` for weather, fire, flood, caravan and Creeping Doom.

## Scoring model (measured, day-11 board state)

Score ≈ **5 × steps**, consistent across archetypes with very different
skills. Walking is the game:

| Source | Points |
|---|---|
| Step onto a resource pile (`collectResource`, automatic in `movePlayer`) | **10/token**, piles are 1–3 |
| Explore a new hex | 1 |
| Found a settlement | 20 |
| Improved / basic shelter | 8 / 4 |
| Bank encounter loot | 3/token (+10 full clear) |
| FORAGE success / partial | 3 / 1 |

48.5% of hexes (2074 of 4275) hold a pile, so roughly every other step pays
10–30 points, against 3 for a 2-MP FORAGE. Pile density is the single
highest-leverage balance lever.

## Gameplay findings so far

**The thirst spiral is real and sharp.** Water is the binding constraint:
`ACT_WATER` needs Marsh, Flooded or River terrain (~4.7% of the map once the
raft-gated River is excluded), so most water comes from piles. A survivor that
lets water tokens reach zero goes: track falls → LL falls → MP is `ll + 3`, so
mobility falls → cannot reach water → dead. Measured on hardware: tokens hit 0
on day 2, dead on day 7. Resting through a shortage makes it strictly worse,
because dawn drinks 2 water whether you moved or not. Policies now hunt water
as an override (`staple_hunt`), which roughly tripled survival distance.

**Encounters are barely reachable.** Across every run so far, ContentMax has
opened **zero** POIs — with 105 POIs on 4275 hexes (2.5%) and fog limiting
vision, a bot simply never walks onto one by chance. Whether that holds in
realtime mode with a full MP budget is the open question, but if it does, the
authored content is effectively unreachable rather than merely underpaid.

## Status

Phase 1 (transport, parser, slot claiming, soak, telemetry) and Phase 2
(`scoremax`, `contentmax`, `coward`, `rival`, local encounter JSON,
pathfinding) are built and verified on hardware — 113 offline checks in
`smoke.py`.

Not done: a realtime-mode run long enough to draw balance conclusions from,
and `metrics.py` (score spread, snowball correlation, POI race outcomes,
check-margin histogram) over the recorded JSONL.
