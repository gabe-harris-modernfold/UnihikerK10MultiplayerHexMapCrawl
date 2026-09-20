# bots/ — autonomous play harness

Python bots that play the game over `/ws` against a real K10, so the balance
can be measured instead of guessed. Purely additive: nothing here touches
firmware or `data/`.

**The full guide is [docs/bot-testing.md](../docs/bot-testing.md)** — protocol
rules, board limits, timing, what the metrics mean, findings and open issues.
Read it before drawing conclusions from a run. This file is just the quick
start.

```bash
python smoke.py                        # 119 offline checks, no board needed
python arena.py --host 192.168.4.234 --bots 5 \
    --policies scoremax,contentmax,coward,rival,scoremax --target 1000
python metrics.py                      # analyse the newest run
```

Deps: Python 3.12 + `websockets`.

## Layout

| File | Role |
|---|---|
| `config.py` | Constants mirrored from the firmware. **No drift detection — update by hand.** |
| `mapdec.py` | Port of `data/map-decoder.js`. Full map + vis-disk decode. |
| `state.py` | Message → `Observation`. Tolerant of unknown/missing keys. |
| `navigate.py` | Dijkstra over the fogged torus using `TERRAIN_MC`. |
| `encounters.py` | Encounter JSON from local disk; ports `computeEncounterDN`. |
| `client.py` | One bot: transport, rate limiting, slot claiming, connection lifecycle. |
| `policy/` | `decide(obs) -> Action`: `drunk`, `scoremax`, `contentmax`, `coward`, `rival`. |
| `arena.py` | Supervisor: reset, spawn, run to target, loop. |
| `telemetry.py` | Polls `/state` for board health. |
| `metrics.py` | Post-run analysis, including the tension-vs-target report. |
| `record.py` | JSONL per run into `runs/` (gitignored). |

## Five things that will silently waste a run

Detail and the rest of the list in [docs/bot-testing.md](../docs/bot-testing.md).

1. **The archetype index IS the player slot**, and a refused `pick` is silent —
   only the unicast `sync` confirms you are seated.
2. **Receiving state is not evidence of having joined.** `broadcastState()` uses
   `ws.textAll()`, so lobby clients get the full broadcast too.
3. **`enc_start` requires `q`/`r`** or it is discarded at the handler's first
   line with no error reply.
4. **Reset is `eraseslot` ×6 then `regen`, verified against `/state`** — `regen`
   alone keeps score, and it drops the control socket after succeeding.
5. **`ev` is broadcast to everyone** — filter by `pid` or every statistic gets
   multiplied by the fleet size.

Plus one operational rule: **never `nohup` a background run.** The wrapper gets
killed and the Python process keeps playing, blocking the next run's reset.

## Status

Built and verified on hardware: transport, slot claiming, parser, pathfinding,
encounter library with the real DN maths, five policies, telemetry, JSONL
recording, and the tension metric.

Open: the firmware seat-loss fix is written but unflashed, and until it lands
the churn contaminates balance numbers. See the doc's "Known issues".
