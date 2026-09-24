# bots/ — autonomous play harness

Python bots that play the game over `/ws` against a real K10, so the balance
can be measured instead of guessed. Purely additive: nothing here touches
firmware or `data/`.

**The full guide is [docs/bot-testing.md](../docs/bot-testing.md)** — protocol
rules, board limits, timing, what the metrics mean, findings and open issues.
Read it before drawing conclusions from a run. This file is just the quick
start.

```bash
python smoke.py                        # 359 offline checks, no board needed
python arena.py --host 192.168.4.234 --bots 5 \
    --policies scoremax,contentmax,coward,rival,scoremax --target 1000

# The Subterranean Explorers, against a surface control bot. Anything that
# reads the tunnel board belongs in the same run as something that does not,
# or there is nothing to compare the cost of going underground against.
python arena.py --host 192.168.4.234 --bots 5 \
    --policies subterranean,subterranean,tunnelrunner,scoremax,scoremax \
    --target 1000
python metrics.py                      # analyse the newest run
python metrics.py --aggregate runs/run-2026*.jsonl   # pool several runs

# Looking for problems on purpose (protocol 2 firmware only):
python fuzz.py  --host k10.local       # malformed / out-of-order messages
python chaos.py --host k10.local       # disconnects, races, stalled sockets
python soak.py  --host k10.local --minutes 60   # realtime, days held open
python findings.py                     # what the newest log found
```

Every bot, balance runs included, checks what it sees against invariants in
`oracles.py` and reports breaches as findings -- see "Finding problems" in the
doc.

Do not raise `--min-interval` / `--global-interval` above the defaults: 2.5x
the inbound rate starved the board's heap and crashed it mid-run. The board
also takes a DHCP lease, so its address moves after a reboot.

Deps: Python 3.12 + `websockets`.

## Layout

| File | Role |
|---|---|
| `config.py` | Constants mirrored from the firmware. Updated by hand; `smoke.py` catches drift. |
| `mapdec.py` | Port of `data/map-decoder.js`. Full map + vis-disk decode. |
| `state.py` | Message → `Observation`. Tolerant of unknown/missing keys. |
| `navigate.py` | Dijkstra over the fogged torus using `TERRAIN_MC`. |
| `encounters.py` | Encounter JSON from local disk; ports `computeEncounterDN`. |
| `client.py` | One bot: transport, rate limiting, slot claiming, connection lifecycle. |
| `wire.py` | Reply tracking (shared) and `ProbeClient`, the scriptable connection. |
| `oracles.py` | Invariants checked on every connection and every `/state` read. |
| `findings.py` | Finding format + CLI. |
| `fuzz.py`, `chaos.py`, `soak.py` | The three probes. `fuzz_cases.py` is the fuzz corpus. |
| `policy/` | `decide(obs) -> Action`: `drunk`, `scoremax`, `contentmax`, `coward`, `rival`, the two Subterranean Explorers `subterranean` / `tunnelrunner`, and `sentinel` (soak only: camps under a shelter, rests late in each day). |
| `arena.py` | Supervisor: reset, spawn, run to target, loop. |
| `telemetry.py` | Polls `/state` for board health. |
| `causes.py` | Attributes every LL loss, and every death, to a cause. |
| `metrics.py` | Post-run analysis: tension vs target, death causes, `--aggregate`. |
| `record.py` | JSONL per run into `runs/` (gitignored). |

## Five things that will silently waste a run

Detail and the rest of the list in [docs/bot-testing.md](../docs/bot-testing.md).
On protocol 2 firmware (`sync` carries `pv: 2`) every one of these that is a
refusal comes back as a `nack` with a reason, because the client sends a `rid`
on every message -- see "Replies" in the doc. The rules still hold; they are
just no longer silent.

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

And, for anything touching the bunker tunnels: **a survivor underground is not
where `p.q`/`p.r` says they are** — those stay pinned to the hatch, so reading
`obs.map[(me.q, me.r)]` down there describes a hex 30 metres overhead. Go
through `obs.board` / `obs.pos()` / `obs.here()`. Five more like it in the
doc's "The bunker tunnels".

## Status

Built and verified on hardware: transport, slot claiming, parser, pathfinding,
encounter library with the real DN maths, five policies, telemetry, JSONL
recording, and the tension metric.

**Not yet on hardware: the two Subterranean Explorers** (`subterranean`,
`tunnelrunner`) and the tunnel-board plumbing behind them. Verified offline
against `smoke.py` and against `mock-server/`, which speaks the whole tunnel
protocol — repeated dives, crossings and climb-outs, with the metrics coming
through. The mock's hex geometry and economy are its own, so treat the
MP-saving figures from it as a working mechanism, not a measurement.

Open: the firmware seat-loss fix is written but unflashed, and until it lands
the churn contaminates balance numbers. See the doc's "Known issues".
