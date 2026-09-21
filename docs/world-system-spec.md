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
| Creeping Doom | Single mobile entity, awareness-driven | World tick |

All three are owned by a single `WorldSystem` struct defined in `world-system.hpp` and ticked from inside `tickGame()` in `Esp32HexMapCrawl.ino`. No virtual dispatch. No inheritance. Fixed entity counts.

---

## Map & Tick Context

```
MAP_COLS = 75, MAP_ROWS = 57 → 4275 hexes (toroidal wrap)
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
    int16_t  wq, wr;        // current waypoint (always a Settlement once active)
    int16_t  pq, pr;        // settlement just departed; -1,-1 = none yet (excluded
                             // from the next pick so it doesn't double back)
    bool     active;
    uint8_t  inv[5];        // resource slots: water/food/fuel/med/scrap (matches Player.inv)
    uint8_t  restockTimer;  // world ticks until inventory refills
    uint8_t  stockItem[CARAVAN_STOCK_SLOTS];  // consumable shelf (see "Shelf" below); 0 = empty slot
    uint8_t  stockQty[CARAVAN_STOCK_SLOTS];
};

struct CreepingDoom {
    int16_t q, r;
    uint8_t awareness;      // 0–100; drives detection radius and behavior
};

struct WorldSystem {
    Caravan     caravan;
    CreepingDoom creepingDoom;
};

static WorldSystem W;         // zero-initialised; activated via wInit()
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
1. tickCreepingDoom()    — read tracks → move → act (may ignite hexes)
2. decayTracks()        — reduce all W_hex[r][q].track by TRACK_DECAY_RATE
3. spreadFire()         — advance fire state (double-buffered to avoid cascade)
4. tickCaravan()        — advance toward waypoint, flee fire/god if adjacent
5. resolveProximity()   — apply per-player effects (fire damage, trade prompt, god effect)
6. enqWorldEvents()     — push resulting GameEvents into the queue
```

Order is load-bearing. Creeping Doom reads tracks **before** decay so single-action heat (Forage = ~4) is still smellable above `SCENT_THRESHOLD` on the next world tick. Fire settles before entities move into or away from it.

---

## Track System

### Writing

When a player spends MP on any action, the server calls:

```cpp
void wOnPlayerAction(int16_t q, int16_t r, uint8_t mpSpent) {
    W_hex[r][q].track = (uint8_t)min(255, (int)W_hex[r][q].track + mpSpent * TRACK_AP_SCALE);
}
```

**Call site 1 — actions:** inside `spendMP(Player& p, int cost)` in `survival_skills.hpp`, after `p.movesLeft` is decremented. Hooking `spendMP` itself (rather than each `do*` handler) captures every action-driven track in one place. Real per-action MP costs in the current code: Forage = 2, Water = 1–3 (variable), Scavenge = 2, Shelter = 1 or 2 (auto-selected), Survey = 1 (0 for Scout). Heavier actions leave hotter tracks.

**Call site 2 — movement:** `movePlayer()` in `survival_state.hpp` deducts the terrain move cost `mc` directly rather than going through `spendMP`, so it calls `wOnPlayerAction(p.q, p.r, mc)` itself right after the deduction. Travelling therefore lays a trail proportional to how hard the ground was to cross: open scrub (1 MP — 2 heat) stays under `SCENT_THRESHOLD`, rough terrain (2–3 MP) does not.

This reverses the original design, where movement was silent and only in-hex actions laid scent. In practice that left the Doom nothing to follow — see the Detection Radius note below — so a player who kept moving was permanently invisible to it.

`ACT_REST` still calls neither hook, so resting remains silent: the intended counter to Doom's attention (see Player Resting as Counter).

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

With `TRACK_AP_SCALE = 2` and `TRACK_DECAY_RATE = 1`: a forage action (MP 2) lays 4 heat, which sits at or above `SCENT_THRESHOLD` (4) for four world ticks (~60 s real time) before going cold. A 3 MP water draw lays 6 heat and stays warm for ~90 s. A 1 MP step lays 2 heat — never smellable on its own. To make heavier actions leave longer trails, raise `TRACK_AP_SCALE`; to make the Doom less sensitive, raise `SCENT_THRESHOLD` or `TRACK_DECAY_RATE`.

`TRACK_DECAY_RATE` was originally 3 and `SCENT_THRESHOLD` 8, which made the system unreachable: no single action in the game costs more than 3 MP, so no single action could ever lay 8 heat, and a 2 MP action's 4 heat was gone in two ticks. `hottestTrackWithin()` therefore almost never cleared the threshold and awareness decayed to 0 and stayed there.

---

## Entity: Caravan

### Purpose

A roaming APC that the player can trade resources with when co-located. Provides a supply pressure valve — scarce goods available at a price, but you have to find it first.

### Waypoint Movement

The caravan holds a single target waypoint, always a Settlement hex. Each world tick it steps one hex toward it using `hexDistWrap`. On arrival it picks the next settlement to head to:

```cpp
void tickCaravan() {
    if (!W.caravan.active) return;

    // Hold for a customer (see Trade Hold), else step toward waypoint
    if (caravanHasCustomer() && W.caravan.holdTicks < CARAVAN_TRADE_HOLD) {
        W.caravan.holdTicks++;
    } else if (W.caravan.q != W.caravan.wq || W.caravan.r != W.caravan.wr) {
        W.caravan.holdTicks = 0;
        moveOneStep(W.caravan.q, W.caravan.r, W.caravan.wq, W.caravan.wr);
    } else {
        W.caravan.holdTicks = 0;
        pickCaravanWaypoint();   // arrived — choose next destination
    }

    // Restock timer — counts down whether the caravan moved or held
    if (W.caravan.restockTimer > 0) W.caravan.restockTimer--;
    else restockCaravan();
}
```

### Trade Hold

The caravan parks while a survivor is standing on its hex, so it cannot roll away mid-purchase: a `car_buy` aimed at a caravan that stepped off one world tick earlier fails the co-location check with `why = 1` ("the caravan has moved on"), which reads as a bug from the shelf screen rather than as world simulation. `caravanHasCustomer()` uses the same filter as the trade prompt — connected, not encounter-locked — plus "not downed", since a downed survivor cannot open the action panel and so has nothing to finish.

The hold is capped at `CARAVAN_TRADE_HOLD = 4` world ticks (~60 s) so a parked player cannot pin the trade route indefinitely; past the cap the caravan resumes its route with the player still standing there. `Caravan.holdTicks` resets the moment the hex is clear (or the cap is hit and it moves), and it is runtime-only — not in `SaveHeader`, so a reboot simply restarts the wait. Mirrored in `mock-server/server.js` (`tickCaravan(connected)` / `caravanHasCustomer(connected)`).

### Waypoint Selection

`pickCaravanWaypoint()` scans the map for Settlement hexes and picks the nearest one by `hexDistWrap`, excluding the hex the caravan is currently standing on. It also excludes `W.caravan.pq/pr` — the settlement it just departed — when another candidate exists, so it doesn't settle into bouncing between two mutually-nearest settlements; that pair becomes `pq/pr` for the *next* pick instead. `pq/pr` starts at the `-1,-1` sentinel (no settlement departed yet) and, like `wq/wr`, is not persisted across a save/load — a reload just gives the caravan a fresh route. If the map has no other settlement to head to at all (degenerate case — map generation guarantees at least 27), it falls back to a random passable hex.

### Tire Tracks

Each successful step in `tickCaravan()` also marks the hex it just entered: `G.map[r][q].tireTrack = 1` (`HexCell.tireTrack`, `Esp32HexMapCrawl.ino`) — the same "mark the hex I just moved onto" idiom player footprints use, but a single shared bit rather than a per-player mask, since it's just "a vehicle drove through here" with no attribution needed. It rides in bit 7 of the wire-encoded `TT` byte (`encodeCell()`, `hex-map.hpp`) — free because terrain only needs 4 bits and bit 6 is already the improved-shelter flag — so the map/vis-disk wire format doesn't grow a byte. Like footprints (and unlike `HexDynamic.track`, the unrelated invisible AP-scent value Creeping Doom follows — see Track System below), tire tracks never fade or decay; they're cleared only by `generateMap()` on a full world regen. Client renders them with `GLYPH.TIRE_TRACK` via `drawTireTracks()` (`data/renderer.js`), the same worn-in-mark idiom as `drawFootprints()`. The mock (`mock-server/server.js`) mirrors this with a `caravanTracks` overlay Set consulted from `ttFor()`.

The caravan isn't the only source: `movePlayer()` (`survival_state.hpp`) sets the same bit when the moving player has any equipped item flagged `tracks = yes` in `items.cfg` — today just the Motorbike (id 26). `hasTireTracks(pid)` (`inventory_items.hpp`) checks every equip slot for the flag, mirroring `hasPassTerrainBit()`'s loop rather than hardcoding `EQUIP_VEHICLE`, so a future non-vehicle item could set it too. The mock's `case 'm'` handler mirrors this by checking `p.eq` against `ITEM_DEFS[itemId]?.tracks` before adding to `caravanTracks`. No fuel gating: the mark fires whenever the item is equipped and the player moves, regardless of whether the day's fuel cost was actually paid (see Restock/dawn-cost sections — `applyDawnItemCosts()` only gates the MP *bonus*, and there's no separate "fuelled today" flag to check at move time).

Unlike the caravan, a player mover's `EVT_MOVE` (`mv`) event isn't broadcast unconditionally by position — it already reaches every client, so the "did this step leave a track" fact just needs to ride along: `ev.amt` (`GameEvent`'s generic per-event-type field, same reuse idiom as fire/flood events) carries it as `trk` in the wire JSON. Client-side, `_evMv()` (`data/network.js`) stamps `gameMap[r][q].tireTrack = 1` on an already-revealed cell when `ev.trk` is set — the same "vis-disk only reaches the mover" reasoning that makes it also stamp footprints there, and the same pattern `_applyWorldState()` uses for the caravan's own position-driven stamp just above it.

### Trade

`resolveProximity()` checks if any connected player shares `(q, r)` with the caravan. If so — **and only on the tick the player first co-locates** (see Proximity Debounce below) — it enqueues `EVT_CARAVAN_TRADE` targeting that player, which the client logs and toasts.

Opening the shelf, though, does **not** wait on that event: it fires on the world tick, up to `WORLD_TICK_INTERVAL` (~15 s) after the step that caused it, which is far too late for "I walked onto the trader". The client edge-triggers the panel itself off the 100 ms state broadcast — which carries both the player's position and the caravan's — in `maybeAutoOpenCaravanTrade()` (`data/ui-panels.js`, called from `_msgState`/`_msgSync` in `data/network.js`). It re-arms when the player leaves the hex, and stands down while the player is downed, encounter-locked, or has another overlay open. `EVT_CARAVAN_TRADE` remains the log/toast notice.

Trade itself is handled by a **dedicated** message handler `handleMsg_caravan_trade` in `network-msg-trade.hpp`. It does **not** reuse the existing `tradeOffers[MAX_PLAYERS]` table or the player-to-player accept/decline flow — that machinery is hard-bounded to `pid < MAX_PLAYERS` (see `network-msg-trade.hpp` and `findSlot()` in `hex-map.hpp`) and shoehorning a pseudo-pid would require a 254-branch in every trade function. Instead the caravan handler:

1. Validates the player is co-located with `W.caravan`.
2. Reads the requested goods from `W.caravan.inv` and the offered goods from `Player.inv`.
3. Mutates both inventories in one atomic block under `G.mutex`.
4. Enqueues `EVT_TRADE_RESULT` with `tradeTo = CARAVAN_PID` for client UI.

Wire format: `{"t":"car_trade","give":[5],"want":[5]}`. No accept/decline round-trip — the caravan transacts immediately.

### Shelf — consumables for resource tokens

Beyond the token swap, the caravan carries a small shelf of consumables it sells for resource tokens (`Caravan.stockItem[CARAVAN_STOCK_SLOTS]` / `stockQty[]`; `CARAVAN_STOCK_SLOTS = 4` lives in the `.ino` because `SaveHeader` needs it, `CARAVAN_STOCK_MAX = 3` units per slot in `world-system.hpp`). What can appear on it is data-driven from `data/items.cfg`: any `category = consumable` item with `trade = yes`. The crafted concoctions (ids 52+) are `trade = no` so a learned recipe stays the only way to get one; key items are `trade = no` by definition.

The asking price per unit is the item's `value` key (floored at 1) — `caravanPrice()`. It is paid in **token-worth, not token count**: `CARAVAN_TOKEN_WORTH[5] = {0, 1, 2, 1, 1}` (water/food/fuel/med/scrap) — food, meds and scrap count 1 each, fuel counts 2, and water is worth nothing and is refused outright (`why = 4`); mirrored in `data/game-data.js` and the mock. Overpaying is accepted (the caravan doesn't make change; the client keeps it to at most one fuel token's rounding). The price is never stored: `appendCaravanStock()` (`network-sync.hpp`) reads it at serialisation time, so editing `items.cfg` re-prices the shelf on the next boot.

Purchase handler: `handleMsg_caravan_buy` (`network-msg-trade.hpp`), wire `{"t":"car_buy","item":ID,"n":QTY,"give":[5]}`. `give[]` is the payment; its sum must cover `price × n` (overpaying is accepted — the client caps its steppers at the exact price). Every check runs before anything is spent — co-located, shelf holds ≥ n, player holds `give[]`, `invRoomFor()` has room for all n — then the tokens move player → caravan `inv`, the shelf slot decrements (item id cleared at 0 so the next restock re-rolls it) and `addItemToInv()` grants the goods. Success emits `EVT_TRADE_RESULT` (`tradeTo = CARAVAN_PID`, `tradeItem`/`tradeItemQty` set, so `trd_res` gains `"item","n"`) for every client's log, plus a targeted `{"t":"item_result","act":"buy",…}` so the buyer's pack and token counts update immediately (typed inventory never rides the state broadcast). Failure replies `{"t":"trade_fail","why":N}` — 1 not co-located / not in stock, 2 can't pay, 3 pack full, 4 tried to pay with water.

Client: with the caravan as target, the TRADE sub-panel shows a CARAVAN STOCK list above the swap steppers (`buildStockList()` in `data/ui-panels.js`); picking a card opens a PAY WITH stepper row pre-filled from whatever the player holds most of. The mock (`mock-server/server.js`) mirrors the shelf, restock and `car_buy`.

### Restock

Every `CARAVAN_RESTOCK_TICKS` world ticks, each `inv` slot refills to a terrain-weighted amount. Caravan never runs fully dry on all slots simultaneously — at least one slot always has stock (clamp in restock logic).

The shelf restocks on the same timer: one occupied slot is rotated out for a fresh roll (so stock still turns over for a party that never buys), empty slots get a new random stockable consumable (`rollCaravanStockItem()`, never a duplicate of what is already on the shelf) with 1–3 units, and every other slot gains one unit up to `CARAVAN_STOCK_MAX`.

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

Ignition sources in v1: Creeping Doom `ACTING` behavior, and future player/event triggers.

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

## Entity: Creeping Doom

### Awareness

`awareness` is the sole state variable driving all behavior. No stored mode enum — behavior is derived at tick time from threshold checks.

| Awareness | Behavior | Detection Radius | Hexes / tick |
|---|---|---|---|
| 0–20 | Random walk; ignores tracks | 6 | 1 |
| 21–50 | Moves toward hottest track in radius | 6–7 | 1 |
| 51–75 | Pathfinds aggressively to hottest track | 8 | 1 |
| 76–99 | Ignores terrain cost; ignites hexes on arrival | 9 | 2 |
| 100 | Locks adjacent to player; effect every world tick | 10 | 3 |

`doomStepsPerTick()` supplies the last column. At one hex per 15 s world tick the Doom needed minutes to cross a single hex, which made even a fully-aware hunt unthreatening to anyone who had simply stopped nearby. Three hexes per tick is still ~1 hex per 5 s against a player's ~1 hex per 0.2–0.9 s (`MOVE_CD_MS * mc`), so running remains a complete answer — it just means a hunting Doom actually arrives.

```cpp
int doomDetectionRadius() {
    return DOOM_BASE_RADIUS + (W.creepingDoom.awareness / 25);  // 6 at 0, 10 at 100
}
```

The base was originally 2. On a 75-column map a random-walking entity that can only smell two hexes will essentially never cross a player's trail, so awareness never left 0 on hardware and none of the `>=51` effects could fire — the Doom was visible on the K10 minimap and completely inert. `DOOM_BASE_RADIUS = 6` gives a dormant Doom a realistic chance of picking up a trail without making it omniscient; awareness still has to climb five consecutive scent ticks (~75 s) before it even warns.

### Tick Logic

The scent read happens **first**, at every awareness level including 100, and the lock-on branch is gated on there actually being a live trail:

```cpp
void tickCreepingDoom() {
    int radius = doomDetectionRadius();
    HexCoord hotspot = hottestTrackWithin(W.creepingDoom.q, W.creepingDoom.r, radius);
    bool hasScent = W_hex[hotspot.r][hotspot.q].track >= SCENT_THRESHOLD;
    int  steps    = doomStepsPerTick();

    // Awareness 100 + a live trail: drop the trail, go for the survivor
    if (W.creepingDoom.awareness >= 100 && hasScent) {
        int tgt = nearestConnectedPlayer(W.creepingDoom.q, W.creepingDoom.r);  // -1 if none
        if (tgt >= 0) {
            for (int s = 0; s < steps; s++)
                stepAdjacentTo(W.creepingDoom.q, W.creepingDoom.r,
                               G.players[tgt].q, G.players[tgt].r);
            wIgnite(W.creepingDoom.q, W.creepingDoom.r, 2);
            return;
        }
    }

    if (hasScent) {
        // Found scent — close in
        for (int s = 0; s < steps; s++)
            stepToward(W.creepingDoom.q, W.creepingDoom.r, hotspot.q, hotspot.r);
        W.creepingDoom.awareness = (uint8_t)min(100, (int)W.creepingDoom.awareness + AWARENESS_GAIN);
    } else {
        // Lost scent — wander and fade
        randomWalk(W.creepingDoom.q, W.creepingDoom.r);
        W.creepingDoom.awareness = (uint8_t)max(0, (int)W.creepingDoom.awareness - AWARENESS_DECAY);
    }

    // High-awareness act: ignite current hex
    if (W.creepingDoom.awareness >= 76)
        wIgnite(W.creepingDoom.q, W.creepingDoom.r, 2);
}
```

`hottestTrackWithin()` is a greedy scan: iterate all hexes within `hexDistWrap <= radius`, return the coord with the highest `W_hex[r][q].track`. No A* — O(MAP_ROWS·MAP_COLS) worst case, fast on ESP32.

### Proximity Effect (awareness ≥ 51, adjacent to player)

Handled in `resolveProximity()`. Effect escalates with awareness:

| Awareness | Effect |
|---|---|
| 51–75 | Emit warning event only (dread; no motif here — audio is the distance-driven ostinato below) |
| 76–99 | Destroy one resource node on the player's hex (set `G.map[r][q].amount = 0`) |
| 100 | LL−1 per world tick; destroy resource node |

### Taunts

`tickDoomTaunts()` gives the Doom a voice, keyed off the same awareness
thresholds every other behaviour tier uses so a taunt always coincides with a
real change in what it is doing to you:

| Tier | Awareness | Meaning |
|---|---|---|
| 1 | 51–75 | it has your scent |
| 2 | 76–99 | it is unmaking what you gather |
| 3 | 100 | it has stopped tracking and started hunting |
| 0 | <51 | it lost you — the release beat |

Tier 0 is only ever sent **after** a higher tier, so a session that never drew
the Doom's attention stays silent rather than opening with "it lost you".

**Firing rule.** A tier change is news and speaks immediately, but never
within `DOOM_TAUNT_MIN_GAP` (2) world ticks of the previous line; otherwise
only the full `DOOM_TAUNT_COOLDOWN` (8 ticks, ~2 min) lets it speak again, and
only while `tier > 0`. The floor matters because awareness gains 12 and decays
5 per tick, so a running battle parks it right on a threshold and flutters
across it — without `MIN_GAP` that taunts every single world tick. A tier
change suppressed by the floor is not lost: `lastTauntTier` is only updated
once the line actually goes out, so the change fires on the next eligible
tick.

**The wording never goes on the wire.** `EVT_DOOM_TAUNT` carries `pid`
(`nearestConnectedPlayer()` — who it addresses), `amt` = tier, and `res` = a
raw random byte. The client reduces that byte modulo its own row length in
`DOOM_TAUNTS` (`data/game-data.js`), so lines can be added or reworded without
touching the firmware and the two sides never have to agree on how many exist.
The K10 keeps its own phrasings in the `EVT_DOOM_TAUNT` handler via `K10_SAY`,
the same way every other event does.

**Presentation.** Broadcast to every client like `EVT_DOOM_WARNING` — the rest
of the party seeing the Doom single someone out is most of the effect — but
only the addressed player gets a toast; everyone else gets the log line.
`showToast(msg, 'doom')` applies a `.toast-doom` class: blood-tinted, italic
and *not* uppercased, so it reads as something in the wasteland talking rather
than the UI reporting. Every other `showToast` caller omits the argument and
is unchanged.

Taunt state (`lastTauntTier`, `tauntCooldown`) lives on `CreepingDoom` but is
**not** persisted — `SaveHeader` still carries only q/r/awareness, so no
`SAVE_VERSION` bump. `tryLoadSave()` re-seeds `lastTauntTier` from the restored
awareness so a reboot mid-hunt doesn't re-announce a tier the player already
heard.

### Audio — the pair

`tickDoomAudio()` gives the Doom the only ostinato in the tone palette: two
notes a minor 2nd apart (`DOOM_LO` 131 Hz / `DOOM_HI` 139 Hz, C3/C#3,
`tone-motifs.hpp`) alternating, where the **repeat rate carries the
information** and the pitch never moves. It is the lowest recurring voice on
the box, so the Doom reads as underneath everything else the K10 says.

Two tuning constraints were found on hardware and are worth not rediscovering:

- **Note-length floor.** A tone needs ~10 cycles before the ear hears pitch
  rather than a click. The first cut accelerated by *shortening notes* to
  50–90 ms, which at the original 98 Hz `DOOM_LO` is 5–9 cycles — it played as
  a burst of clicks. No note in the family now goes below 150 ms (~20 cycles
  at C3), and tempo comes from shrinking the **gaps**, never the notes.
- **Register.** 98/104 Hz (G2/G#2) is at the bottom of what the K10 speaker
  can move; at a low `audioVol` almost none of the fundamental survived.
  Raised a fourth to C3/C#3 for roughly double the output. The tritone drop
  (`DOOM_DROP`) lands on G2 — the old `DOOM_LO`, and `MOTIF_HEAVY_DOOR_DRAG`'s
  opening note, so known-good on this hardware.

The sequencer itself is **not** a suspect: measured on-device,
`i2s_set_sample_rates(I2S_NUM_0, 8000)` takes correctly (`live=8000` against a
vendor default of 16000) and a 1720 ms nominal figure plays in 1726 ms.

Because tempo is the message, the cue is chosen by **distance**, not by
awareness. Awareness is how locked-on the Doom is, which is a different
question from where it is standing, so it only gates whether the Doom makes
any sound at all (the same `>= 51` threshold the proximity effect uses). The
audible range is the scent radius: if it can smell you, you can hear it.

| Band | Condition | Motif | Cadence |
|---|---|---|---|
| 0 | awareness < 51, nobody connected, or `dist > radius` | silent | — |
| 1 | outer half of the scent radius | `MOTIF_DOOM_FAR` | every 2nd world tick (~30 s) |
| 2 | inner half (`dist * 2 <= radius`) | `MOTIF_DOOM_NEAR` | every world tick (~15 s) |
| 3 | `dist <= 2`, or awareness 100 | `MOTIF_DOOM_HUNT` | every world tick |
| — | band falls to 0 after having risen | `MOTIF_DOOM_LOST` | once |

`MOTIF_DOOM_HUNT` ends on a tritone below `DOOM_HI` (G2, 98 Hz) — the floor
dropping out. `MOTIF_DOOM_LOST` is the release: the pair *breaks*, ending on
the low note with its answer missing. Like the tier-0 taunt it only ever fires
after the Doom had actually closed on someone, so a session that never drew
its attention stays silent.

A fully-aware Doom on the far side of the map makes no sound. That is the
point — the sound is proximity, and it has to be possible to outrun it.

**Why it is not a one-shot on the effect.** The old cues fired from
`resolveProximity()` and were gated on `hexDistWrap <= 1`, so the only audio
the Doom ever made arrived at the moment it was already on top of you, and
both motifs were borrowed from unrelated events (`MOTIF_DISTANT_THUD` is the
weather shift, `MOTIF_ROTTEN_CHORD` is score loss). Those calls are gone; at
that range band 3 is sounding every tick anyway, and a one-shot would only
race it and lose — `k10PlaySeq()` drops a cue while another is live, it never
queues. One figure is ~1.0–1.1 s against a 15 s world tick, so even the
tightest band occupies ~7% of the single tone voice.

`lastAudioBand` / `audioPhase` live on `CreepingDoom` alongside the taunt
state and are likewise **not** persisted. `tryLoadSave()` resets both to 0 —
the opposite of the `lastTauntTier` treatment, because seeding the band from
the restored position would let the first world tick after a reboot fire the
release cue for a hunt this boot never played.

---

### Player Resting as Counter

`ACT_REST` sets `p.resting = true` and `p.actUsed = true` but calls neither track hook (not `spendMP`, and not the movement hook either, since resting doesn't move you) — therefore the rest action lays no scent. Track intensity on the resting hex still decays each world tick, so a player's recent hexes fall below `SCENT_THRESHOLD` within a few ticks and Creeping Doom's awareness drops via `AWARENESS_DECAY` — resting is the natural counter to Creeping Doom's attention.

This now holds at **every** tier, including 100. The awareness-100 lock-on originally ran ahead of the scent read and `return`ed unconditionally, so awareness was never touched again once it reached 100: with anyone connected it was a permanent hunt with no escape, directly contradicting this section. Gating the lock on `hasScent` restores it — go still, let the trail go cold, and it loses you and cools off like at any other level. Escaping still costs you: from 100 it takes `100 / AWARENESS_DECAY` = 20 cold ticks to reach 0, and ten of those are still above the 51 effect threshold.

---

## Proximity & Encounter-Lock Rules

`resolveProximity()` is the per-player effect pass. Two cross-cutting rules apply to **every** effect it can produce (fire damage, caravan trade prompt, Creeping Doom warning/act):

1. **Skip players in active encounters.** If `encounters[pid].active` (set by `enc_start` and cleared on bank/abort/dawn), no world effect lands on that player this tick. The encounter system already freezes the player's input loop (`actions_game_loop.hpp` action dispatcher early-returns); world effects must respect that contract or the encounter UI desyncs.

2. **Caravan trade prompt is edge-triggered, not level-triggered.** A small per-player flag `lastCaravanHex[pid]` tracks the (q,r) the player was on the last time they were co-located with the caravan. `EVT_CARAVAN_TRADE` only fires when `(p.q, p.r) == W.caravan` **and** that pair differs from `lastCaravanHex[pid]`. Otherwise a parked player would receive a trade prompt every 15s indefinitely. The flag is cleared on player move, on caravan move, or on disconnect.

Fire damage and god effects are level-triggered (per-tick) by design — being on a burning hex *should* hurt every world tick.

---

## New Event Types

Append to `EvtType` enum starting at 20. The existing enum is `… EVT_ENC_END = 17, EVT_WEATHER = 19` — value **18 is currently unused**; new world events start at 20 to leave 18 reserved for any future encounter-related event:

```cpp
EVT_FIRE_DAMAGE      = 20,   // player took fire damage: pid, q, r, amt (intensity)
EVT_FIRE_SPREAD      = 21,   // hex caught fire: q, r, intensity (broadcast to nearby players)
EVT_CARAVAN_TRADE    = 22,   // caravan trade available: pid (co-located player)
EVT_DOOM_WARNING     = 23,   // creeping doom adjacent, low threshold: pid
EVT_DOOM_ACT         = 24,   // creeping doom destroyed resource / drained LL: pid, q, r, actLLD
EVT_DOOM_TAUNT       = 29,   // the Doom speaks: pid = addressee, amt = tier 0-3, res = line index
```

`EVT_FIRE_SPREAD` and `EVT_DOOM_ACT` use the existing range-filter in `drainEvents()` — only players within vision radius of the affected hex receive them.

`EVT_DOOM_WARNING` is sent to all connected players regardless of position — Creeping Doom is a world-level threat, not a local one.

---

## Network Sync Additions

`broadcastState()` in `network-sync.hpp` sends a full snapshot each tick. Add a `world` key to the existing JSON payload:

```json
"world": {
  "caravan": { "q": 12, "r": 7, "active": true, "inv": [w, f, fu, m, s], "stock": [[itemId, qty, price], ...] },
  "doom":    { "q": 4,  "r": 2, "awareness": 63 },
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
static constexpr uint8_t  TRACK_DECAY_RATE     = 1;     // track intensity lost per world tick
static constexpr uint8_t  SCENT_THRESHOLD      = 4;     // min track intensity Doom will pursue
static constexpr uint8_t  DOOM_BASE_RADIUS     = 6;     // scent radius at awareness 0
static constexpr uint8_t  AWARENESS_GAIN       = 12;    // per world tick when following scent
static constexpr uint8_t  DOOM_TAUNT_COOLDOWN  = 8;     // world ticks between repeat taunts
static constexpr uint8_t  DOOM_TAUNT_MIN_GAP   = 2;     // floor between any two taunts, incl. tier changes
static constexpr uint8_t  AWARENESS_DECAY      = 5;     // per world tick when cold
static constexpr uint8_t  FIRE_SPREAD_CHANCE   = 20;    // percent chance of spread per neighbour
static constexpr uint8_t  FIRE_CAP             = 20;    // max simultaneous burning hexes
static constexpr uint8_t  CARAVAN_RESTOCK_TICKS = 40;  // world ticks between caravan restocks
static constexpr uint8_t  CARAVAN_PID          = 254;  // sentinel for EVT_TRADE_RESULT.tradeTo on caravan trades
static constexpr uint8_t  CARAVAN_STOCK_SLOTS  = 4;    // consumable shelf slots (defined in the .ino — SaveHeader needs it)
static constexpr uint8_t  CARAVAN_STOCK_MAX    = 3;    // max units per shelf slot
```

---

## Memory Budget

| | Bytes |
|---|---|
| `HexDynamic W_hex[57][75]` | 2 × 4275 = **8550 bytes** |
| `static HexDynamic next[57][75]` (spreadFire double-buffer) | **8550 bytes** |
| `WorldSystem W` (Caravan + DeathGod) | ~30 bytes |
| `lastCaravanHex[MAX_PLAYERS]` (proximity debounce) | ~24 bytes |
| **Total added** | **~1.95 KB** |

All in static RAM alongside other globals. Well within ESP32-S3 budget.

---

## v1 Scope

- One caravan, one Creeping Doom, fire system: all active from `wInit()`
- Caravan trade via dedicated `handleMsg_caravan_trade` handler (not the player-to-player trade table)
- Fire cap enforced (no runaway burn)
- Persistence: `W.caravan` (q, r, restockTimer, inv) and `W.creepingDoom` (q, r, awareness) added to `SaveHeader` extension; bumps `SAVE_VERSION` from 9 to 10. **Existing v9 saves will fail the version check in `tryLoadSave()` and be ignored — boot will fall through to `generateMap()`.** This is a deliberate one-shot reset; no migration path is provided. Document in release notes.
- `W_hex` (track + fire) is **not** persisted. Tracks decay in seconds anyway, and fires extinguish on power cycle (parity with player respawn).
- Shelf persistence (`SAVE_VERSION` 15): `caravanStockItem[]` / `caravanStockQty[]` in `SaveHeader`. Prices are not persisted (they come from `items.cfg`), and an id the cfg no longer knows is dropped on load.
- Tire tracks (`SAVE_VERSION` 16): not a `SaveHeader` field — `HexCell` itself grew a `tireTrack` byte, which changes `MAP_BYTES` (the raw `G.map` block `saveGame()`/`tryLoadSave()` read/write). Same one-shot-reset precedent as the other bumps above: a v15 save is ignored, not migrated.