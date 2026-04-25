# Dynamic World Entity Layer — Design Spec

**File:** `docs/world-system-spec.md`
**Status:** Design / Pre-implementation
**Companion file:** `world-system.hpp` (to be created)

---

## Overview

Three dynamic world features share a common need: stateful things that live on the hex map, update on a slower clock than the game tick, and interact with the player and each other.

| Entity | Shape | Clock |
|---|---|---|
| Caravan | Single mobile entity, 1 per world | World tick |
| Fire | Hex-owned intensity, up to ~20 hexes | World tick |
| Death God | Single mobile entity, awareness-driven | World tick |

All three are owned by a single `WorldSystem` struct defined in `world-system.hpp` and ticked from inside `tickGame()` in `Esp32HexMapCrawl.ino`. No virtual dispatch. No inheritance. Fixed entity counts.

---

## Map & Tick Context

```
MAP_COLS = 25, MAP_ROWS = 19 → 475 hexes (toroidal wrap)
TICK_MS  = 100 ms
DAY_TICKS = 3000 ticks  (5 minutes real time per game-day)

WORLD_TICK_INTERVAL = 150  // ticks between world system updates (~15 s real time, ~1/20th day)
```

World tick fires when `G.tickId % WORLD_TICK_INTERVAL == 0` inside `tickGame()`. All world state mutates during this window while `G.mutex` is already held.

---

## HexDynamic — Parallel State Array

Fire and track intensity are hex-level state. They do **not** go into `HexCell` — that struct is wire-encoded and shared with clients unchanged. Instead, a parallel flat array holds dynamic-only state:

```cpp
struct HexDynamic {
    uint8_t fire;   // 0 = none, 1–3 = burning (1=ember, 2=burning, 3=inferno)
    uint8_t track;  // AP footprint intensity; 0–255, decays each world tick
};

static HexDynamic W_hex[MAP_ROWS][MAP_COLS];  // zero-initialised at boot
```

`W_hex[r][q]` mirrors `G.map[r][q]`. Access pattern is identical — never allocated separately from the static globals.

**Flammable terrains:** Open Scrub (0), Rust Forest (2), Broken Urban (4), Rolling Hills (7), Settlement (9). All others are immune.

---

## WorldSystem Struct

```cpp
struct Caravan {
    int16_t  q, r;
    int16_t  wq, wr;        // current waypoint
    bool     active;
    uint8_t  inv[5];        // resource slots: water/food/fuel/med/scrap (matches Player.inv)
    uint8_t  restockTimer;  // world ticks until inventory refills
};

struct DeathGod {
    int16_t q, r;
    uint8_t awareness;      // 0–100; drives detection radius and behavior
};

struct WorldSystem {
    Caravan  caravan;
    DeathGod deathGod;
};

static WorldSystem W;       // zero-initialised; activated via wInit()
```

`W` is a single global, parallel to `G` (the `GameState`). Both are held by `G.mutex` during the world tick.

---

## Initialisation & Lifecycle

`wInit()` zero-clears `W_hex`, places the caravan and death god at randomly-chosen passable hexes, and sets `W.caravan.active = true`. Call sites:

- **Boot:** in `setup()` after `tryLoadSave()` / `generateMap()` and before `gameLoopTask` is spawned.
- **Map regen:** in the `EVT_REGEN` handler path on Core 1, after the new map is generated. Without this, entities sit on stale or impassable coords (e.g. a freshly-rolled Nuke Crater).

`wInit()` is also the only place the caravan's `active` flag is flipped on; without the call the caravan never moves.

---

## Tick Order

Called once per world tick, with `G.mutex` held:

```
1. tickDeathGod()       — read tracks → move → act (may ignite hexes)
2. decayTracks()        — reduce all W_hex[r][q].track by TRACK_DECAY_RATE
3. spreadFire()         — advance fire state (double-buffered to avoid cascade)
4. tickCaravan()        — advance toward waypoint, flee fire/god if adjacent
5. resolveProximity()   — apply per-player effects (fire damage, trade prompt, god effect)
6. enqWorldEvents()     — push resulting GameEvents into the queue
```

Order is load-bearing. The god reads tracks **before** decay so single-action heat (Forage = ~4) is still smellable above `SCENT_THRESHOLD` on the next world tick. Fire settles before entities move into or away from it.

---

## Track System

### Writing

When a player spends MP on any action, the server calls:

```cpp
void wOnPlayerAction(int16_t q, int16_t r, uint8_t mpSpent) {
    W_hex[r][q].track = (uint8_t)min(255, (int)W_hex[r][q].track + mpSpent * TRACK_AP_SCALE);
}
```

**Single call site:** inside `spendMP(Player& p, int cost)` in `survival_skills.hpp`, after `p.movesLeft` is decremented. Hooking `spendMP` itself (rather than each `do*` handler) captures every action-driven track in one place. Real per-action MP costs in the current code: Forage = 2, Water = 1–3 (variable), Scavenge = 2, Shelter = 1 or 2 (auto-selected), Survey = 1 (0 for Scout). Heavier actions leave hotter tracks.

Movement does not lay tracks: the move handler in `survival_state.hpp` decrements `p.movesLeft` directly without going through `spendMP`. Stepping through a hex leaves no scent; doing things in a hex does. `ACT_REST` also does not call `spendMP`, so resting is silent.

### Decay

Each world tick:

```cpp
void decayTracks() {
    for (int r = 0; r < MAP_ROWS; r++)
        for (int q = 0; q < MAP_COLS; q++)
            if (W_hex[r][q].track > 0)
                W_hex[r][q].track -= min((uint8_t)TRACK_DECAY_RATE, W_hex[r][q].track);
}
```

With `TRACK_AP_SCALE = 2` and `TRACK_DECAY_RATE = 3`: a forage action (MP 2) lays 4 heat — readable on the next world tick (god reads before decay), then cold one tick later (~30 s real time). An improved shelter build (MP 2 + a follow-up scrap deposit) lays ~4–8 heat — fades within 1–3 world ticks. A scavenge in ruins (MP 2) lays 4 heat. To make heavier actions leave longer trails, raise `TRACK_AP_SCALE`; to make the god less sensitive, raise `SCENT_THRESHOLD`. The numbers in the Constants table are starting points to tune in playtest.

---

## Entity: Caravan

### Purpose

A roaming APC that the player can trade resources with when co-located. Provides a supply pressure valve — scarce goods available at a price, but you have to find it first.

### Waypoint Movement

The caravan holds a single target waypoint. Each world tick it steps one hex toward it using `hexDistWrap`. On arrival it picks a new waypoint biased toward unexplored or low-fire hexes:

```cpp
void tickCaravan() {
    if (!W.caravan.active) return;

    // Step toward waypoint
    if (W.caravan.q != W.caravan.wq || W.caravan.r != W.caravan.wr) {
        moveOneStep(W.caravan.q, W.caravan.r, W.caravan.wq, W.caravan.wr);
    } else {
        pickCaravanWaypoint();   // arrived — choose next destination
    }

    // Restock timer
    if (W.caravan.restockTimer > 0) W.caravan.restockTimer--;
    else restockCaravan();
}
```

### Waypoint Bias

`pickCaravanWaypoint()` samples 8 random hexes and picks the candidate with the lowest combined score of: `fire_intensity * 10 + hexDistWrap(to death god) < 3 ? 50 : 0`. Caravan prefers open ground away from the god and away from fire.

### Trade

`resolveProximity()` checks if any connected player shares `(q, r)` with the caravan. If so — **and only on the tick the player first co-locates** (see Proximity Debounce below) — it enqueues `EVT_CARAVAN_TRADE` targeting that player. The client opens the trade panel.

Trade itself is handled by a **dedicated** message handler `handleMsg_caravan_trade` in `network-msg-trade.hpp`. It does **not** reuse the existing `tradeOffers[MAX_PLAYERS]` table or the player-to-player accept/decline flow — that machinery is hard-bounded to `pid < MAX_PLAYERS` (see `network-msg-trade.hpp` and `findSlot()` in `hex-map.hpp`) and shoehorning a pseudo-pid would require a 254-branch in every trade function. Instead the caravan handler:

1. Validates the player is co-located with `W.caravan`.
2. Reads the requested goods from `W.caravan.inv` and the offered goods from `Player.inv`.
3. Mutates both inventories in one atomic block under `G.mutex`.
4. Enqueues `EVT_TRADE_RESULT` with `tradeTo = CARAVAN_PID` for client UI.

Wire format: `{"t":"car_trade","give":[5],"want":[5]}`. No accept/decline round-trip — the caravan transacts immediately.

### Restock

Every `CARAVAN_RESTOCK_TICKS` world ticks, each `inv` slot refills to a terrain-weighted amount. Caravan never runs fully dry on all slots simultaneously — at least one slot always has stock (clamp in restock logic).

---

## Entity: Fire

### Ignition Sources

Any system can ignite a hex by calling:

```cpp
void wIgnite(int16_t q, int16_t r, uint8_t intensity = 1) {
    if (!isFlammable(G.map[r][q].terrain)) return;
    W_hex[r][q].fire = max(W_hex[r][q].fire, intensity);
}
```

Ignition sources in v1: Death God `ACTING` behavior, and future player/event triggers.

### Spread and Decay

Double-buffered so spread is simultaneous, not cascade:

```cpp
void spreadFire() {
    static HexDynamic next[MAP_ROWS][MAP_COLS];   // static to keep stack watermark stable
    memcpy(next, W_hex, sizeof(W_hex));

    for (int r = 0; r < MAP_ROWS; r++) {
        for (int q = 0; q < MAP_COLS; q++) {
            if (W_hex[r][q].fire == 0) continue;
            if (next[r][q].fire > 0) next[r][q].fire--;   // decay toward 0

            if (W_hex[r][q].fire >= 2) {
                for (int d = 0; d < 6; d++) {
                    int nq = wrapQ(q + DQ[d]);
                    int nr = wrapR(r + DR[d]);
                    if (isFlammable(G.map[nr][nq].terrain) && (random(100) < FIRE_SPREAD_CHANCE))
                        next[nr][nq].fire = max(next[nr][nq].fire, (uint8_t)1);
                }
            }
        }
    }
    memcpy(W_hex, next, sizeof(W_hex));
}
```

`FIRE_SPREAD_CHANCE` = 20 (20%). At intensity 3, fire is aggressive — each burning hex has a 20% chance of igniting each of its six neighbours per world tick.

### Fire Damage

In `resolveProximity()`, each connected player on a burning hex loses LL:

```
fire intensity 1 → no damage (smoke, warning only)
fire intensity 2 → LL−1 per world tick
fire intensity 3 → LL−1 per world tick + radiation +1
```

Emits `EVT_FIRE_DAMAGE` with `pid`, `q`, `r`, `intensity`. Client plays an audio sting and shows a damage toast.

---

## Entity: Death God

### Awareness

`awareness` is the sole state variable driving all behavior. No stored mode enum — behavior is derived at tick time from threshold checks.

| Awareness | Behavior | Detection Radius |
|---|---|---|
| 0–20 | Random walk; ignores tracks | 2 |
| 21–50 | Moves toward hottest track in radius | 3 |
| 51–75 | Pathfinds aggressively to hottest track | 4 |
| 76–99 | Ignores terrain cost; ignites hexes on arrival | 5 |
| 100 | Locks adjacent to player; effect every world tick | 6 |

```cpp
int godDetectionRadius() {
    return 2 + (W.deathGod.awareness / 25);  // 2 at 0, 6 at 100
}
```

### Tick Logic

```cpp
void tickDeathGod() {
    // Awareness == 100: lock to nearest connected player (overrides scent path)
    if (W.deathGod.awareness >= 100) {
        int tgt = nearestConnectedPlayer(W.deathGod.q, W.deathGod.r);  // -1 if none
        if (tgt >= 0) {
            stepAdjacentTo(W.deathGod.q, W.deathGod.r,
                           G.players[tgt].q, G.players[tgt].r);
            wIgnite(W.deathGod.q, W.deathGod.r, 2);
            return;
        }
        // No player connected — fall through to scent path and let awareness decay
    }

    int radius = godDetectionRadius();
    HexCoord hotspot = hottestTrackWithin(W.deathGod.q, W.deathGod.r, radius);

    if (W_hex[hotspot.r][hotspot.q].track >= SCENT_THRESHOLD) {
        // Found scent — close in
        stepToward(W.deathGod.q, W.deathGod.r, hotspot.q, hotspot.r);
        W.deathGod.awareness = (uint8_t)min(100, (int)W.deathGod.awareness + AWARENESS_GAIN);
    } else {
        // Lost scent — wander and fade
        randomWalk(W.deathGod.q, W.deathGod.r);
        W.deathGod.awareness = (uint8_t)max(0, (int)W.deathGod.awareness - AWARENESS_DECAY);
    }

    // High-awareness act: ignite current hex
    if (W.deathGod.awareness >= 76)
        wIgnite(W.deathGod.q, W.deathGod.r, 2);
}
```

`hottestTrackWithin()` is a greedy scan: iterate all hexes within `hexDistWrap <= radius`, return the coord with the highest `W_hex[r][q].track`. No A* — O(475) worst case, fast on ESP32.

### Proximity Effect (awareness ≥ 51, adjacent to player)

Handled in `resolveProximity()`. Effect escalates with awareness:

| Awareness | Effect |
|---|---|
| 51–75 | Emit warning event only (dread, audio cue) |
| 76–99 | Destroy one resource node on the player's hex (set `G.map[r][q].amount = 0`) |
| 100 | LL−1 per world tick; destroy resource node |

### Player Resting as Counter

`ACT_REST` sets `p.resting = true` and `p.actUsed = true` but does **not** call `spendMP` — therefore the rest action lays no track via the `spendMP` hook. Track intensity on the resting hex still decays each world tick. After 2–3 consecutive days of rest, all of a player's recent hexes decay below `SCENT_THRESHOLD` and god awareness drops via `AWARENESS_DECAY` — resting is the natural counter to the god's attention.

---

## Proximity & Encounter-Lock Rules

`resolveProximity()` is the per-player effect pass. Two cross-cutting rules apply to **every** effect it can produce (fire damage, caravan trade prompt, god warning/act):

1. **Skip players in active encounters.** If `encounters[pid].active` (set by `enc_start` and cleared on bank/abort/dawn), no world effect lands on that player this tick. The encounter system already freezes the player's input loop (`actions_game_loop.hpp` action dispatcher early-returns); world effects must respect that contract or the encounter UI desyncs.

2. **Caravan trade prompt is edge-triggered, not level-triggered.** A small per-player flag `lastCaravanHex[pid]` tracks the (q,r) the player was on the last time they were co-located with the caravan. `EVT_CARAVAN_TRADE` only fires when `(p.q, p.r) == W.caravan` **and** that pair differs from `lastCaravanHex[pid]`. Otherwise a parked player would receive a trade prompt every 15s indefinitely. The flag is cleared on player move, on caravan move, or on disconnect.

Fire damage and god effects are level-triggered (per-tick) by design — being on a burning hex *should* hurt every world tick.

---

## New Event Types

Append to `EvtType` enum starting at 20. The existing enum is `… EVT_ENC_END = 17, EVT_WEATHER = 19` — value **18 is currently unused**; new world events start at 20 to leave 18 reserved for any future encounter-related event:

```cpp
EVT_FIRE_DAMAGE   = 20,   // player took fire damage: pid, q, r, amt (intensity)
EVT_FIRE_SPREAD   = 21,   // hex caught fire: q, r, intensity (broadcast to nearby players)
EVT_CARAVAN_TRADE = 22,   // caravan trade available: pid (co-located player)
EVT_GOD_WARNING   = 23,   // death god adjacent, low threshold: pid
EVT_GOD_ACT       = 24,   // death god destroyed resource / drained LL: pid, q, r, actLLD
```

`EVT_FIRE_SPREAD` and `EVT_GOD_ACT` use the existing range-filter in `drainEvents()` — only players within vision radius of the affected hex receive them.

`EVT_GOD_WARNING` is sent to all connected players regardless of position — the god is a world-level threat, not a local one.

---

## Network Sync Additions

`broadcastState()` in `network-sync.hpp` sends a full snapshot each tick. Add a `world` key to the existing JSON payload:

```json
"world": {
  "caravan": { "q": 12, "r": 7, "active": true },
  "god":     { "q": 4,  "r": 2, "awareness": 63 },
  "fire":    [[q, r, intensity], ...]
}
```

`fire` is a sparse array — only hexes with `W_hex[r][q].fire > 0`. At the budgeted cap of 20 burning hexes, this adds at most ~120 bytes to the broadcast payload.

`awareness` is sent as a raw 0–100 value. The client maps this to a visual aura intensity — no number is shown to the player.

Tracks are **not** sent to clients — the trail is server-internal state.

**Buffer sizing:** the existing `static char buf[2500]` in `broadcastState()` carries ~1940 bytes today (6 players × ~310 + footer). The world block adds ~200 bytes (caravan + god + 20 fire entries). Bump the buffer to **3072** to give comfortable headroom; alternatively, guard the world-block append with an `snprintf` length check and skip it if remaining capacity < 256. Without one of these, an overflow would silently truncate JSON mid-payload.

---

## Constants / Tuning Table

```cpp
// ── World system tuning ────────────────────────────────────────
static constexpr uint8_t  WORLD_TICK_INTERVAL  = 150;   // game ticks between world updates
static constexpr uint8_t  TRACK_AP_SCALE       = 2;     // heat added = apSpent * TRACK_AP_SCALE
static constexpr uint8_t  TRACK_DECAY_RATE     = 3;     // track intensity lost per world tick
static constexpr uint8_t  SCENT_THRESHOLD      = 8;     // min track intensity god will pursue
static constexpr uint8_t  AWARENESS_GAIN       = 12;    // per world tick when following scent
static constexpr uint8_t  AWARENESS_DECAY      = 5;     // per world tick when cold
static constexpr uint8_t  FIRE_SPREAD_CHANCE   = 20;    // percent chance of spread per neighbour
static constexpr uint8_t  FIRE_CAP             = 20;    // max simultaneous burning hexes
static constexpr uint8_t  CARAVAN_RESTOCK_TICKS = 40;  // world ticks between caravan restocks
static constexpr uint8_t  CARAVAN_PID          = 254;  // sentinel for EVT_TRADE_RESULT.tradeTo on caravan trades
```

---

## Memory Budget

| | Bytes |
|---|---|
| `HexDynamic W_hex[19][25]` | 2 × 475 = **950 bytes** |
| `static HexDynamic next[19][25]` (spreadFire double-buffer) | **950 bytes** |
| `WorldSystem W` (Caravan + DeathGod) | ~30 bytes |
| `lastCaravanHex[MAX_PLAYERS]` (proximity debounce) | ~24 bytes |
| **Total added** | **~1.95 KB** |

All in static RAM alongside other globals. Well within ESP32-S3 budget.

---

## v1 Scope

- One caravan, one death god, fire system: all active from `wInit()`
- Caravan trade via dedicated `handleMsg_caravan_trade` handler (not the player-to-player trade table)
- Fire cap enforced (no runaway burn)
- Persistence: `W.caravan` (q, r, restockTimer, inv) and `W.deathGod` (q, r, awareness) added to `SaveHeader` extension; bumps `SAVE_VERSION` from 9 to 10. **Existing v9 saves will fail the version check in `tryLoadSave()` and be ignored — boot will fall through to `generateMap()`.** This is a deliberate one-shot reset; no migration path is provided. Document in release notes.
- `W_hex` (track + fire) is **not** persisted. Tracks decay in seconds anyway, and fires extinguish on power cycle (parity with player respawn).