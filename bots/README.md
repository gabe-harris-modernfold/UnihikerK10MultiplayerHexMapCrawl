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

A 5-bot / 62-second sprint run against the K10:

| Metric | Idle, 0 clients | 1 client | 5 clients |
|---|---|---|---|
| `maxTickMs` (budget 100) | 95 | 117 | **190** |
| `minHeap` | 43 468 | 43 468 | **10 164** |
| `broadcastPartial` | 1 195 | — | **10 910** (+9 715 in 62 s) |

**The load is fan-out, not inbound traffic.** The bots sent 222 messages in
62 s (3.6/s aggregate) — trivial. The cost is `broadcastState()` serialising
~3.3 KB per client every 100 ms tick: five clients is ~165 KB/s outbound off
one ESP32-S3. So *reducing the send rate barely helps; reducing client count
does*. `broadcastSkips` stayed at 0, so mutex contention is not the problem.

`minHeap` of 10 KB is the number to respect — `docs/dev-loop.md` attributes
the 2026-09-12 HTTP wedge to internal heap starvation at ~46 KB idle. It did
not wedge, but there is not much margin. Power-cycle between serious runs:
`maxTickMs` and `minHeap` are high-water marks that never reset.

**Practical ceiling: 4 bots**, leaving one slot for a browser and one of
headroom. `handleConnect` rejects once `connectedCount + lobbySize >=
MAX_PLAYERS` (6), and a rejected bot gets `{"t":"full"}` and retries forever.

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

## Status

Phase 1 (transport, parser, slot claiming, soak, telemetry) is done and
verified on hardware. Phase 2 — `scoremax`, `contentmax`, `coward`, `rival`
policies and local encounter-JSON loading — is not built yet.
