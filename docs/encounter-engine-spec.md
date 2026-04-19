# Encounter Engine Implementation Plan

## Context
The game currently has a "not yet implemented" placeholder for encounters (see `actions_game_loop.hpp:134`). This plan adds a push-your-luck encounter engine triggered at POI hexes, integrating with the existing server-authoritative architecture, item/stat system, and multiplayer mechanics.

**Design decisions:**
- **Hybrid architecture**: Client renders text/UI, server validates rolls and state
- **Solo with assists**: One player runs the encounter; same-hex allies can spend resources to reduce risk
- **Stat-based items**: Equipment modifies player stats that feed into a unified risk formula (no named-item gates)
- **One-time global POIs**: Once any player enters a POI (`enc_start` fires), it's consumed for everyone — regardless of success or failure
- **Per-encounter placeholders**: Each encounter JSON contains its own placeholder dictionary for text resolution
- **First come, first served**: If two players try to enter the same POI, the first claim wins; second receives a specific "claimed" rejection message ("Another survivor is already inside")
- **Resources + typed items**: Encounters can drop both resource quantities and typed items (equipment, consumables, materials)
- **Loot table references**: Encounter JSON references named per-biome loot tables defined in a separate SD file
- **2-4 nodes typical**: Short encounters, 1-3 minutes. Quick push-your-luck decisions
- **Linear encounters**: Main path is a linear push-deeper chain, 2-4 nodes. *(Side rooms deferred to v2 — additive schema change)*
- **Survey reveals POI**: POI hexes are invisible until Surveyed; surveying at half the normal survey range shows an eye icon. POI visibility is global — once any player surveys a POI hex, all players see the eye icon. This matches how shelter and footprints already work in the vis-disk
- **Free entry**: No MP cost to enter an encounter; internal node costs handle resource drain
- **Server-side loot tables**: Server reads `loot_tables.json` at boot (~1-2KB) and resolves typed item drops during `enc_choice` handling. Client receives results in EVT_ENC_RESULT — no client-side loot table cache or rolling needed. Keeps full server authority
- **Random encounter selection**: Server picks randomly from the full pool each time (`random(0, count)`). With 260+ files across biomes, repeats are rare in a single session. *(Used-encounter bitmask tracking deferred to v2 if needed)*
- **Fully locked during encounter**: Player cannot perform any non-encounter actions (move, forage, scavenge, shelter, survey, rest, trade) while in an active encounter
- **Minimal ally view**: Same-hex allies see a "Player X is in an encounter" banner with assist buttons. No spectator narrative view — allies don't fetch the encounter JSON. *(Full spectator read-along deferred to v2)*

---

## 1. Schema: Encounter JSON (on SD card)

Adapted from the spec to use existing stat names and skill indices instead of named items:

```json
{
  "id": "subway_ruins_01",
  "title": "The {{adjective_decay}} Subway",
  "start_node": "entrance",
  "nodes": {
    "entrance": {
      "text": "The stairs descend into {{darkness_desc}}...",
      "can_bank": false,
      "choices": [
        {
          "label": "Wade into the water.",
          "cost": { "fatigue": 1, "ll": 1 },
          "base_risk": 35,
          "skill": 5,
          "success_node": "platform",
          "hazard_id": "water_disease"
        },
        {
          "label": "Search the surface rubble.",
          "cost": { "fatigue": 1 },
          "base_risk": 15,
          "skill": 2,
          "success_node": "platform",
          "hazard_id": "cut_hands"
        }
      ]
    },
    "platform": {
      "text": "You reach the platform...",
      "can_bank": true,
      "loot": [
        { "res": 4, "qty": [1, 3] },
        { "res": 2, "qty": [0, 1] }
      ],
      "loot_table": "urban_common",
      "choices": [
        {
          "label": "Push deeper into the tunnel.",
          "cost": { "fatigue": 2 },
          "base_risk": 55,
          "skill": 2,
          "success_node": "deep_tunnel",
          "hazard_id": "tunnel_collapse"
        }
      ]
    },
    "deep_tunnel": {
      "text": "The tunnel opens into a pre-war maintenance bay...",
      "can_bank": true,
      "loot": [
        { "res": 4, "qty": [2, 5] }
      ],
      "loot_table": "urban_rare",
      "choices": []
    }
  },
  "hazards": {
    "water_disease": {
      "text": "Contaminated water seeps into a wound...",
      "penalty": { "ll": -1, "radiation": 1 },
      "status": 8,
      "ends_encounter": true
    },
    "cut_hands": {
      "text": "Jagged metal slices your palm.",
      "penalty": { "ll": -1 },
      "ends_encounter": false
    },
    "tunnel_collapse": {
      "text": "The ceiling gives way...",
      "penalty": { "ll": -2, "fatigue": 2 },
      "wound": [0, 1],
      "ends_encounter": true
    }
  },
  "placeholders": {
    "adjective_decay": ["Drowned", "Collapsed", "Buried"],
    "darkness_desc": ["pitch blackness", "a foul-smelling dark"]
  }
}
```

### Schema Notes
- **`cost`/`penalty`** use `ll`, `fatigue`, `radiation`, `food`, `water`, `resolve`
- **`loot.res`** uses resource indices 0-4 (Water, Food, Fuel, Medicine, Scrap)
- **`loot_table`** references a named per-biome table in `/encounters/loot_tables.json` for typed item drops (e.g., `urban_common`, `marsh_rare`)
- **`skill`** uses skill indices 0-5 (Navigate, Forage, Scavenge, Treat, Shelter, Endure)
- **`hazard.ends_encounter: false`** allows non-terminal hazards (penalty applied but encounter continues)
- **`side_room: true`** *(v2, not implemented in v1)* — will mark one-shot choices that loop back to the current node. Additive schema field; v1 encounters are purely linear
- **Terminal nodes**: Nodes with empty `choices: []` and `can_bank: true` are encounter endpoints (deepest room)
- **Loot timing**: Node `loot` (resources) and `loot_table` (typed items) are granted after a successful choice that reaches the node, not on arrival. Pending until banked
- **Downed in encounter**: If hazard penalties cause the player to become Downed (LL 0 or wound threshold), the encounter ends immediately, all pending loot is lost, and standard downed rules apply at the POI hex

### Loot Tables (`/encounters/loot_tables.json`)
Separate file on SD card, read and cached by the **server** at boot (~1-2KB). Defines weighted item drop pools per biome. Server resolves loot table rolls during `enc_choice` handling and reports results in EVT_ENC_RESULT:
```json
{
  "urban_common": [
    { "item": 21, "qty": [1, 2], "weight": 40 },
    { "item": 22, "qty": [1, 1], "weight": 30 },
    { "item": 1,  "qty": [1, 1], "weight": 20 },
    { "item": 0,  "qty": [0, 0], "weight": 10 }
  ],
  "urban_rare": [
    { "item": 11, "qty": [1, 1], "weight": 15 },
    { "item": 14, "qty": [1, 1], "weight": 15 },
    { "item": 22, "qty": [1, 3], "weight": 30 },
    { "item": 1,  "qty": [1, 1], "weight": 25 },
    { "item": 0,  "qty": [0, 0], "weight": 15 }
  ],
  "marsh_common": [
    { "item": 23, "qty": [1, 2], "weight": 35 },
    { "item": 1,  "qty": [1, 1], "weight": 30 },
    { "item": 21, "qty": [1, 1], "weight": 25 },
    { "item": 0,  "qty": [0, 0], "weight": 10 }
  ],
  "marsh_rare": [
    { "item": 11, "qty": [1, 1], "weight": 20 },
    { "item": 23, "qty": [1, 3], "weight": 25 },
    { "item": 14, "qty": [1, 1], "weight": 20 },
    { "item": 1,  "qty": [1, 1], "weight": 20 },
    { "item": 0,  "qty": [0, 0], "weight": 15 }
  ]
}
```
- `item: 0` = nothing (empty drop)
- `weight` = relative probability
- **Server resolves** the loot table roll at `enc_choice` time (parsed and cached in memory at boot)
- Each biome has its own `_common` and `_rare` tables for thematic drops
- If typed inventory is full, excess items drop to ground at the player's current position via existing `groundItems[]` system

---

## 2. Server-Side State (Minimal Heap)

### Add to `Esp32HexMapCrawl.ino`

**HexCell** -- add 1 byte (`poi`):
```cpp
struct HexCell {
  uint8_t terrain, resource, amount, respawnTimer, shelter, footprints, variant;
  uint8_t poi;  // 0=none/looted, non-zero=has encounter (boolean flag)
};
```
Memory impact: +475 bytes (19x25).

**ActiveEncounter** -- per-player encounter tracking (~18 bytes x 6 = ~108 bytes):
```cpp
#define ENC_MAX_ITEMS 3   // max typed items pending per encounter
struct ActiveEncounter {
  uint8_t  active;         // bit 0 = in encounter, bit 7 = reachedTerminal (set when success_node has empty choices; checked on enc_bank for +10 full-clear bonus)
  uint8_t  encIdx;         // poi value for hex marking
  uint8_t  hexQ, hexR;
  uint8_t  pendingLoot[5]; // accumulated unbanked resource loot
  uint8_t  pendingItemType[ENC_MAX_ITEMS]; // typed items awaiting bank
  uint8_t  pendingItemQty[ENC_MAX_ITEMS];
  uint8_t  pendingItemCount;               // how many typed items accumulated
  int8_t   assistRisk;     // accumulated risk reduction from assists
  uint8_t  assistUsed;     // bitmask: which allies assisted this node
  // sideRoomUsed removed — v1 encounters are linear only (v2 addition)
};
static ActiveEncounter encounters[MAX_PLAYERS];
```

**Encounter selection**: Server picks randomly from the full pool each time: `random(0, encPoolCount[terrain])`. No used-encounter tracking in v1. With 260+ files across biomes, repeats are rare in a single session. *(Used-encounter bitmask can be added in v2 if repeat avoidance becomes desirable.)*

**POI discovery via Survey**: POI hexes are not visible on the map by default. When any player uses the Survey action, hexes within **half the normal survey range** (rounded down) that have `poi != 0` become globally visible — all players see the eye icon. This is encoded as bit 7 of the DD byte, set directly from `HexCell.poi` (no per-player conditional). Matches how shelter and footprints already work in the vis-disk. *(Per-player POI visibility deferred to v2 if strategic hiding becomes desirable.)*

**Disconnect handling**: If a player disconnects mid-encounter, the `ActiveEncounter` is cleared immediately. Pending loot is lost. The POI is consumed (already set to 0 on `enc_start`).

**New event types**:
```cpp
EVT_ENC_START    = 14,
EVT_ENC_ASSIST   = 15,
EVT_ENC_RESULT   = 16,
EVT_ENC_BANK     = 17,
EVT_ENC_END      = 18
```

**GameEvent** -- add encounter fields (reuse space with union or extend):
```cpp
// encounter fields (active when type is EVT_ENC_*)
uint8_t  encOut;       // 0=fail, 1=success
uint8_t  encSkill;
uint8_t  encDN;
int8_t   encTotal;
uint8_t  encLoot[5];
int8_t   encPenLL, encPenFat, encPenRad;
uint8_t  encStatus;
uint8_t  encEnds;
uint8_t  encAssistRes;  // resource type for assist event
int8_t   encRiskRed;    // risk reduction for assist event
uint8_t  encItemType;   // typed item dropped (server-rolled from loot table)
uint8_t  encItemQty;    // typed item quantity
```

---

## 3. Risk Formula

Uses existing stats and equipment (which modify stats at equip time):

```cpp
static uint8_t computeEncounterDN(int pid, uint8_t baseRisk, uint8_t skill,
                                   int8_t assistMod) {
  // Map base_risk (0-100) to raw DN (2-12) — fits existing 2d6 resolution system
  int effectiveRisk = constrain((int)baseRisk + assistMod, 0, 100);

  // Threat Clock escalation: +5 risk per threshold crossed
  if (G.threatClock >= TC_THRESHOLD_A) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_B) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_C) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_D) effectiveRisk += 5;
  effectiveRisk = constrain(effectiveRisk, 0, 100);

  int rawDN = 2 + (effectiveRisk * 10) / 100;  // DN 2-12 range for 2d6

  Player& p = G.players[pid];
  int bonus = 0;
  if (p.ll > 4)        bonus += (p.ll - 4) / 2;      // health headroom
  if (p.resolve > 2)   bonus += (p.resolve - 2);       // willpower
  if (p.fatigue > 4)   rawDN += (p.fatigue - 4);       // exhaustion penalty
  if (p.radiation > 3) rawDN += (p.radiation - 3) / 2; // sickness penalty

  return (uint8_t)constrain(rawDN - bonus, 2, 12);
}
```

Resolution uses existing `resolveCheck(pid, skill, dn, 0)` which handles wounds/conditions.

---

## 4. Assist Mechanic

Same-hex allies can spend 1 resource (any type) each to reduce risk **before** the active player commits:

| Any Resource | Risk Reduction |
|-------------|---------------|
| 1 unit of any resource (Water, Food, Fuel, Medicine) | -4 |

- Max 1 assist per ally per node (tracked via `assistUsed` bitmask)
- Total assist reduction capped at -12 per node (3 allies x -4)
- On node advance: `assistUsed` and `assistRisk` reset

---

## 5. WebSocket Message Flow

### Client -> Server
| Message | Fields | Purpose |
|---------|--------|---------|
| `enc_start` | `q, r` | Enter POI encounter |
| `enc_assist` | `target, res` | Ally spends resource to help |
| `enc_choice` | `ci, cost_ll, cost_fat, cost_rad, cost_food, cost_water, cost_resolve, base_risk, skill, loot[5], loot_table, can_bank` | Player picks a choice (server rolls loot table) |
| `enc_bank` | -- | Bank accumulated loot, end encounter |
| `enc_abort` | -- | Leave encounter, lose pending loot, increment Threat Clock |

### Server -> Client (via event queue)
| Event | Key Fields | Purpose |
|-------|-----------|---------| 
| `EVT_ENC_START` | `pid, q, r` | Notify same-hex allies: encounter begun. Allies see minimal banner + assist buttons (no enc_path broadcast — spectator narrative deferred to v2) |
| `EVT_ENC_ASSIST` | `pid (ally), target, res, riskRed` | Assist applied |
| `EVT_ENC_RESULT` | `pid, out, skill, dn, total, loot[5], item_type, item_qty, penalties, status, ends` | Choice resolved (includes server-rolled loot table result) |
| `EVT_ENC_BANK` | `pid, q, r, loot[5]` | Loot secured |
| `EVT_ENC_END` | `pid, q, r, reason` | Encounter ended (hazard/abort/dawn/downed) |

**Validation on `enc_choice`**: Server checks player can afford costs, is in active encounter, and is not downed. Server resolves the roll. Client sends cost/risk values from the JSON; server fully trusts the encounter data (SD card is not player-modifiable) but validates affordability. Server does not track the current node key — `can_bank` is sent by the client in `enc_choice` and trusted. Choice-index validity is not server-validated (cheating is out of scope). If more rigor is needed later, add a `uint8_t nodeDepth` to `ActiveEncounter` and define a convention that nodes in the JSON are depth-ordered.

**Rejection on `enc_start`**: If a POI is already claimed by another player, server sends a specific rejection: "Another survivor is already inside." Client displays this message distinctly from generic errors.

**Abort penalty**: Voluntary abort (`enc_abort`) increments the Threat Clock by 1 — you made noise even leaving empty-handed. Dawn-forced abort does NOT increment TC.

### Exact WebSocket JSON Formats

Follows the existing codebase pattern: client messages use `"t"` for type, server events use `{"t":"ev","k":"<shortcode>"}`.

**Client → Server:**
```json
// Enter encounter
{"t":"enc_start","q":5,"r":8}

// Ally assist (spend 1 resource for -4 risk)
{"t":"enc_assist","target":2,"res":3}

// Make a choice (costs/risk from encounter JSON, loot_table name for server to roll)
{"t":"enc_choice","ci":0,"cost_ll":0,"cost_fat":1,"cost_rad":0,"cost_food":0,"cost_water":0,"cost_resolve":0,"base_risk":35,"skill":2,"loot":[0,0,0,1,0],"lt":"urban_common","can_bank":true}

// Bank loot and exit
{"t":"enc_bank"}

// Abort encounter
{"t":"enc_abort"}
```

**Server → Client (via event queue):**
```json
// Encounter started — broadcast to same-hex allies
{"t":"ev","k":"enc_start","pid":2,"q":5,"r":8}

// Assist applied
{"t":"ev","k":"enc_assist","pid":3,"tgt":2,"res":3,"rd":-4}

// Choice resolved (success with loot table item)
{"t":"ev","k":"enc_res","pid":2,"out":1,"skill":2,"dn":7,"tot":9,"loot":[0,0,0,1,0],"it":21,"iq":1,"penLL":0,"penFat":0,"penRad":0,"st":0,"ends":0}

// Choice resolved (hazard, terminal)
{"t":"ev","k":"enc_res","pid":2,"out":0,"skill":2,"dn":9,"tot":6,"loot":[0,0,0,0,0],"it":0,"iq":0,"penLL":-2,"penFat":2,"penRad":0,"st":4,"ends":1}

// Loot banked
{"t":"ev","k":"enc_bank","pid":2,"q":5,"r":8,"loot":[1,0,2,3,0],"scoreD":28}

// Encounter ended (reason: "hazard", "abort", "dawn", "downed", "disconnect")
{"t":"ev","k":"enc_end","pid":2,"q":5,"r":8,"reason":"dawn"}
```

**Note on `loot_table` string parsing:** The `"lt"` field in `enc_choice` is a string (e.g., `"urban_common"`). Server extracts this using `strstr`/`strchr` to find the quoted value, then matches against cached `loot_tables.json` keys. If the field is missing or empty, no loot table roll occurs.

---

## 6. POI Hex Tracking

### Encounter Distribution
Encounters are heavily weighted toward populated/ruins terrain. Hundreds of encounter JSON files exist across biome folders, with urban and settlement hexes having the largest pools.

**POI spawn rates by terrain** (during `generateMap()`):
| Terrain | POI % | Rationale |
|---------|-------|-----------|
| Broken Urban (4) | 25-30% | Most stories — ruins are full of discoverable locations |
| Settlement (9) | 20-25% | Populated areas have narrative density |
| Flooded District (5) | 15-20% | Submerged ruins, drowned buildings |
| Rust Forest (2) | 5-8% | Occasional hidden sites |
| Ash Dunes (1) | 3-5% | Rare buried finds |
| Open Scrub (0) | 2-3% | Very rare — random roadside encounters |
| Ridge (7) | 5% | Cliff dwellings, observation posts |
| Mountain (8) | 5% | Cave systems |
| Marsh (3) | 3-5% | Sunken caches |
| Glass Fields (6) | 2-3% | Pre-war relics |
| Nuke Crater (10) | 0% | Impassable |
| River Channel (11) | 0% | Impassable |

The `poi` byte on HexCell stores a non-zero value as a boolean flag. The actual encounter file is selected randomly at runtime from the terrain's SD card pool (`random(0, encPoolCount[terrain])`).

### Vis-Disk Encoding
Use **bit 7 of the DD byte** to signal POI presence to clients:
```
DD byte: bits 0-5 = footprints, bit 6 = shelter, bit 7 = has POI
```
No encoding format change needed. Bit 7 is set directly from `HexCell.poi` — global visibility, same as shelter/footprints. No per-player conditional in `buildVisDisk()`.

### POI Consumption
The POI is consumed the moment `enc_start` fires: `G.map[r][q].poi = 0`. All players see the POI disappear on next vis update. The POI is consumed on any entry — failing a hazard, getting downed, aborting, or banking all result in the location being used up.

### Looting
On bank: `pendingLoot[5]` added to `inv[5]`, `pendingItemType/Qty` added to typed inventory slots. If typed inventory is full, excess items drop to ground at the player's current position via existing `groundItems[]` system.

---

## 7. SD Card Layout & Serving

```
/encounters/
  index.json               <- terrain-to-count mapping
  loot_tables.json         <- per-biome loot tables (cached by server at boot)
  urban/                   <- Broken Urban (terrain 4) — largest pool
    0.json ... 99.json     <- ~100+ encounter files
  settlement/              <- Settlement (terrain 9)
    0.json ... 49.json     <- ~50+ encounter files
  flooded/                 <- Flooded District (terrain 5)
    0.json ... 39.json
  forest/                  <- Rust Forest (terrain 2)
    0.json ... 19.json
  dunes/                   <- Ash Dunes (terrain 1)
    0.json ... 9.json
  ridge/                   <- Ridge (terrain 7)
    0.json ... 9.json
  mountain/                <- Mountain (terrain 8)
    0.json ... 9.json
  marsh/                   <- Marsh (terrain 3)
    0.json ... 9.json
  glass/                   <- Glass Fields (terrain 6)
    0.json ... 4.json
  scrub/                   <- Open Scrub (terrain 0) — rare roadside encounters
    0.json ... 4.json
```

Files are numbered sequentially (0.json, 1.json, ...) for O(1) random selection — the server picks `random(0, count)` from unused files and serves that file. No directory traversal needed.

**index.json**:
```json
{
  "0": { "count": 5,   "path": "scrub" },
  "1": { "count": 10,  "path": "dunes" },
  "2": { "count": 20,  "path": "forest" },
  "3": { "count": 10,  "path": "marsh" },
  "4": { "count": 100, "path": "urban" },
  "5": { "count": 40,  "path": "flooded" },
  "6": { "count": 5,   "path": "glass" },
  "7": { "count": 10,  "path": "ridge" },
  "8": { "count": 10,  "path": "mountain" },
  "9": { "count": 50,  "path": "settlement" }
}
```

Counts are updated as new encounter files are authored. The server reads this once at boot and caches it in a compact array (~40 bytes).

**HTTP endpoint** (new route in server setup):
```cpp
server.on("/enc", HTTP_GET, [](AsyncWebServerRequest* req) {
  // Param: biome + id, serves /encounters/{biome}/{id}.json
  req->send(SD, path, "application/json");
});
```

On `enc_start`, server picks a random file index for the hex's terrain type and sends the path to the active player's client. Client fetches via HTTP GET.

**Loot tables**: Server reads and caches `/encounters/loot_tables.json` at boot (~1-2KB). No client endpoint needed — server rolls loot table drops during `enc_choice` handling.

---

## 8. Integration with Existing Systems

| System | Interaction |
|--------|------------|
| **Threat Clock** | Starting any encounter increments TC by 1 (all terrain types). Voluntary abort also increments TC by 1. Dawn abort does NOT increment TC. TC thresholds add +5/+10/+15/+20 to base_risk. |
| **Day Cycle** | See Dusk/Dawn details below. |
| **Movement** | Blocked while `encounters[pid].active`. Entering a POI hex does NOT auto-start; player must choose "Enter" action. Entry is free (no MP cost). Block in `movePlayer()` (`survival_state.hpp:252`). |
| **Survey** | Surveying reveals POI hexes within **half normal survey range** (rounded down). Eye icon becomes globally visible once any player surveys the hex. POIs are invisible until surveyed. |
| **Conditions** | Hazard `status` field sets statusBits (bleeding, fever, etc.). Existing condition penalties affect subsequent encounter checks via `resolveCheck()`. Being Downed ends the encounter immediately with all pending loot lost. |
| **Score** | Use existing `addScore(p, ev, pts)` from `survival_skills.hpp:92`. Banking loot: +3 per individual resource unit (`sum(pendingLoot[0..4]) * 3`). Full clear bonus: +10 (awarded when `reachedTerminal` flag is set on `enc_bank`). |
| **Save** | See Save Format details below. |
| **Action lock** | Player is **fully locked** during encounter. Block in `handleAction()` (`actions_game_loop.hpp:254`) — early return with `AO_BLOCKED` for all 6 action types. Block trades in `trade_offer` / `trade_accept` handlers (`network-handlers.hpp:620-747`). Only `enc_*` messages, banking, aborting, and receiving assists are allowed. |
| **Ground items** | On `enc_bank`, if typed inventory is full, call existing `dropItem()` (`inventory_items.hpp:200`). If `groundItems[MAX_GROUND=32]` is also full, item is lost — acceptable edge case. |
| **K10 display** | Add encounter events to Event Log (Screen 1) via `k10LogAdd()` (34-char max, uses `k10LogMux` spinlock). See K10 Log Messages below. |

### Dusk/Dawn Encounter Handling

In `tickGame()` (`actions_game_loop.hpp:29-36`), the day boundary sequence is:
```
G.dayCount++
duskCheck()      // radiation Endure checks — can cause Downed
dawnUpkeep()     // reset daily state, save game
```

**Encounter abort fires before duskCheck.** Insert encounter cleanup between the day-boundary check and `duskCheck()`:
```cpp
// --- Day boundary ---
G.dayCount++;
// Force-abort any active encounters BEFORE dusk radiation checks
for (int i = 0; i < MAX_PLAYERS; i++) {
  if (encounters[i].active) {
    encounters[i] = {};  // clear all pending loot
    enqEvt({ .type = EVT_ENC_END, .pid = i, ... });  // reason = "dawn"
    // No TC increment for dawn abort
  }
}
duskCheck();     // now safe — no player is in an encounter
dawnUpkeep();
```

If dusk radiation causes Downed on a player who was already out of the encounter, standard downed rules apply at the POI hex. The encounter is already over — no interaction.

### Save Format

`SAVE_VERSION` bumps from 4 to 5. HexCell grows from 7 to 8 bytes (adding `poi`). This changes the binary layout of `map.bin`:

**map.bin layout (v5):**
1. `SaveHeader` (12 bytes) — magic, version=5, dayCount, threatClock, sharedFood, sharedWater, pad
2. `G.map[MAP_ROWS][MAP_COLS]` — 19x25x**8** = 3800 bytes (was 3325 at 7 bytes/cell)
3. `SaveGroundItem[MAX_GROUND]` — appended after map (8 bytes each)

**players.bin** — unchanged (SavePlayer struct unaffected).

**ActiveEncounter is NOT saved.** Encounters are transient — disconnecting mid-encounter immediately clears state, pending loot lost, POI consumed (already zeroed on `enc_start`). On load, all `encounters[]` slots start cleared.

**POI state persists** via `HexCell.poi` in the map binary. POIs consumed during gameplay (`poi = 0`) are reflected in saved state. On new game / version mismatch, `generateMap()` places fresh POIs.

**Migration**: Old v4 saves fail the version check and are rejected (fresh start). No automatic conversion.

### Encounter Index Cache

Server reads `index.json` once at boot and stores in a compact array:
```cpp
struct EncPoolInfo {
  uint8_t count;       // number of encounter files for this terrain
  char    path[12];    // folder name (e.g., "urban", "marsh")
};
static EncPoolInfo encPools[10];  // indexed by terrain type (0-9), ~130 bytes
```

### K10 Log Messages

Encounter events added to Event Log (Screen 1) via `k10LogAdd()`:

| Event | K10 Log Text (max 34 chars) |
|-------|----------------------------|
| `EVT_ENC_START` | `"P2 enters encounter"` |
| `EVT_ENC_RESULT` (success) | `"P2 ENC: Scavenge OK"` |
| `EVT_ENC_RESULT` (fail, terminal) | `"P2 ENC: HAZARD! Ejected"` |
| `EVT_ENC_RESULT` (fail, non-terminal) | `"P2 ENC: HAZARD (cont.)"` |
| `EVT_ENC_BANK` | `"P2 banked encounter loot"` |
| `EVT_ENC_BANK` (full clear) | `"P2 FULL CLEAR! +10"` |
| `EVT_ENC_END` (abort) | `"P2 aborted encounter"` |
| `EVT_ENC_END` (dawn) | `"P2 enc ended (dawn)"` |
| `EVT_ENC_END` (downed) | `"P2 DOWNED in encounter"` |
| `EVT_ENC_ASSIST` | `"P3\xE2\x86\x92P2 assist (-4)"` |

---

## 9. Client-Side Changes

### Files to Modify
- **`data/ui.js`** -- Encounter panel UI, choice buttons, risk bar, loot display, bank button, ally assist banner
- **`data/state-manager.js`** -- Handle `enc_*` event types in reducer
- **`data/engine.js`** -- POI hex rendering (read bit 7 of DD byte, draw POI marker — global visibility)

### Text Resolution (client-side only)
```javascript
function resolveText(text, placeholders) {
  return text.replace(/\{\{(\w+)\}\}/g, (_, key) => {
    const opts = placeholders[key];
    return opts ? opts[Math.floor(Math.random() * opts.length)] : key;
  });
}
```
Placeholders are resolved independently per client — different players may see different narrative text variations. This does not affect gameplay.

### Loot Table Resolution (server-side)
Server caches parsed `loot_tables.json` at boot. When `enc_choice` includes a `loot_table` reference, server rolls:
```cpp
// Called during enc_choice handling when node has loot_table
static void rollLootTable(const char* tableName, uint8_t* outItem, uint8_t* outQty) {
  // Look up cached table by name, roll weighted random, set outItem/outQty
  // item=0 means empty drop (no item granted)
}
```
Result is included in EVT_ENC_RESULT (`encItemType`, `encItemQty`). Client just displays what the server reports.

### Encounter UI
- Modal overlay when encounter is active
- Shows node text, choice buttons with cost/risk preview
- Risk bar (green->red) showing **effective risk after all modifiers** (stats, TC, assists) — full transparency
- Accumulated loot display
- Generic "Bank & Exit" button on `can_bank` nodes
- Ally assist section: shows same-hex allies, resource spend buttons

### Ally View (same-hex allies) — v1 Minimal
- Allies on the same hex see a "Player X is in an encounter" banner (no narrative text)
- Assist buttons are interactive (spend 1 resource for -4 risk reduction)
- Banner dismisses when encounter ends (`EVT_ENC_END` or `EVT_ENC_BANK`)
- *(Full spectator read-along with narrative text deferred to v2)*

---

## 10. Annotated Encounter JSON Example & Authoring Guide

### Full Annotated Example

```json
{
  // ── METADATA ──────────────────────────────────────────────────
  // "id": Unique string identifier. Convention: {biome}_{location}_{number}
  //       Used for logging/debugging only. Not referenced by game logic.
  "id": "urban_clinic_01",

  // "title": Display name shown to player after survey reveals the POI.
  //          Supports {{placeholder}} tokens resolved from the "placeholders" dict.
  //          Keep under 40 characters.
  "title": "The {{adjective}} Clinic",

  // "start_node": Key into the "nodes" dict where the encounter begins.
  //               Must match exactly one key in "nodes".
  "start_node": "entrance",

  // ── NODES ─────────────────────────────────────────────────────
  // The encounter is a linear chain of nodes (rooms/areas) the player pushes through.
  // Each node can have:
  //   - One "push deeper" choice (advances to next node in the chain)
  //   - A banking option to secure accumulated loot and exit
  //   (Side rooms deferred to v2 — v1 encounters are purely linear)
  // Typical depth: 2-4 nodes. Deeper = riskier but more rewarding.
  // Loot on a node is granted AFTER a successful choice reaches it, not on arrival.
  "nodes": {

    "entrance": {
      // "text": Narrative description shown when player arrives at this node.
      //         Supports {{placeholder}} tokens. 1-3 sentences recommended.
      //         Written in second person present tense ("You see...", "The air smells of...").
      "text": "The clinic door hangs from one hinge. Inside, {{debris_type}} covers the floor. A {{smell}} hits you immediately.",

      // "can_bank": Whether the player can choose to bank loot and exit at this node.
      //   false = player must push forward or abort (losing all loot)
      //   true  = generic "Bank & Exit" button appears
      // Typically: entrance = false, mid-points and endpoints = true.
      "can_bank": false,

      // "loot": Resource loot granted when a successful choice leads TO this node.
      //   "res": Resource index: 0=Water, 1=Food, 2=Fuel, 3=Medicine, 4=Scrap
      //   "qty": [min, max] — server rolls random quantity in this range (inclusive)
      //   Omit "loot" entirely if arriving at this node grants no resources.
      //   Loot goes into pendingLoot — not banked until player explicitly banks.

      // "loot_table": Reference to a named per-biome loot table in /encounters/loot_tables.json.
      //   Server rolls this table for a possible typed item drop (equipment, consumable, material).
      //   Omit if this node should not drop typed items.
      //   Use "{biome}_common" for mid-tier, "{biome}_rare" for deep/final nodes.

      // "choices": Array of actions the player can take from this node. At least 1 required.
      "choices": [
        {
          // "label": Button text shown to the player. Keep under 50 characters.
          //          Should be an action phrase: "Search the...", "Climb through...", "Push into..."
          "label": "Search the reception desk.",

          // "cost": Resources spent when choosing this option. Deducted immediately on selection.
          //   Valid keys: "ll", "fatigue", "radiation", "food", "water", "resolve"
          //   Values are positive integers (amount consumed).
          //   Server validates the player can afford costs before allowing the choice.
          //   Omit keys that aren't consumed (e.g., if only fatigue, omit ll).
          "cost": { "fatigue": 1 },

          // "base_risk": Failure probability (0-100). This is the raw danger level before modifiers.
          //   0   = guaranteed success (trivial action)
          //   25  = easy (most players pass)
          //   40  = moderate (trained players pass reliably)
          //   55  = risky (even skilled players can fail)
          //   70+ = dangerous (likely failure without good stats/gear/assists)
          //   Modified at runtime by: player stats, equipment, Threat Clock, ally assists.
          //   Final value is converted to a Difficulty Number (DN 2-12) for a 2d6 skill check.
          //   Client UI shows effective risk (after all modifiers) on the risk bar.
          "base_risk": 20,

          // "skill": Which skill index is used for the check roll.
          //   0=Navigate, 1=Forage, 2=Scavenge, 3=Treat, 4=Shelter, 5=Endure
          //   Player's skill level (0=untrained, 1=trained +2, 2=expert +4) modifies the roll.
          //   Choose the skill that narratively fits the action:
          //     - Navigate: finding paths, avoiding traps, reading maps
          //     - Forage: finding food/water, identifying plants
          //     - Scavenge: searching ruins, prying open containers, salvaging
          //     - Treat: medical situations, handling chemicals, biology
          //     - Shelter: structural assessment, barricading, construction
          //     - Endure: physical hardship, resisting hazards, brute force
          "skill": 2,

          // "success_node": Key of the node to advance to on success.
          //   Points to the next node in the linear chain.
          "success_node": "exam_room",

          // "hazard_id": Key into the "hazards" dict. Triggered on a failed skill check.
          //   Every choice MUST have a hazard_id — there's always a consequence for failure.
          "hazard_id": "broken_glass"
        }
      ]
    },

    "exam_room": {
      "text": "The exam room is {{adjective_condition}}. A medicine cabinet stands {{cabinet_state}}.",
      "can_bank": true,
      "loot": [
        { "res": 3, "qty": [1, 2] }
      ],
      "loot_table": "urban_common",
      "choices": [
        {
          "label": "Force open the locked medicine cabinet.",
          "cost": { "fatigue": 2, "ll": 1 },
          "base_risk": 45,
          "skill": 2,
          "success_node": "pharmacy",
          "hazard_id": "needle_trap"
        }
      ]
    },

    "pharmacy": {
      // Terminal node: deepest room. No choices, just loot and bank.
      // Reaching here is the "full clear" — maximum reward + score bonus.
      "text": "Behind the cabinet, a hidden pharmacy. Rows of intact medicine bottles line the shelves.",
      "can_bank": true,
      "loot": [
        { "res": 3, "qty": [2, 4] },
        { "res": 4, "qty": [1, 2] }
      ],
      "loot_table": "urban_rare",
      "choices": []
    }
  },

  // ── HAZARDS ───────────────────────────────────────────────────
  // Each hazard defines what happens when a player fails a choice's skill check.
  // Referenced by "hazard_id" in choices. Every hazard_id used in choices MUST exist here.
  "hazards": {
    "broken_glass": {
      // "text": Narrative shown when the hazard triggers. 1-2 sentences.
      //         Supports {{placeholder}} tokens.
      "text": "Your foot crashes through {{glass_type}}. Pain shoots up your leg.",

      // "penalty": Stat changes applied to the player. Negative values = damage/loss.
      //   Valid keys: "ll", "fatigue", "radiation", "food", "water", "resolve"
      //   Values are signed: negative = harm, positive = gain (rare but possible).
      //   Rule: positive stat penalties apply as stat gains (e.g., +1 ll = heal 1 LL).
      //   Positive resource penalties (food, water) are added to pendingLoot.
      //   If penalties cause player to become Downed, encounter ends immediately, all pending loot lost.
      "penalty": { "ll": -1 },

      // "status": Bitmask of status conditions to apply.
      //   1=ST_WOUNDED, 2=ST_RADSICK, 4=ST_BLEEDING, 8=ST_FEVERED,
      //   16=ST_DOWNED, 32=ST_STABLE, 64=ST_PANICKED
      //   Omit or set to 0 for no status effect.
      //   Multiple conditions: add values (e.g., 4+8=12 for bleeding+fever).
      "status": 4,

      // "wound": [wound_type, count] — adds wounds to the player.
      //   wound_type: 0=minor, 1=major, 2=critical
      //   count: how many wounds of that type to add
      //   Wounds impose cumulative penalties on all future skill checks.
      //   Omit if no wounds.
      "wound": [0, 1],

      // "ends_encounter": Whether this hazard terminates the encounter.
      //   true  = encounter over, all pending loot LOST, player ejected
      //   false = penalty applied but player stays at current node, can continue
      // Non-terminal hazards are useful for early nodes (small punishment, keep going).
      // Terminal hazards should be reserved for deeper-node failures or severe dangers.
      "ends_encounter": false
    },

    "dust_inhalation": {
      "text": "A cloud of toxic dust erupts from the shelves.",
      "penalty": { "fatigue": 1 },
      "ends_encounter": false
    },

    "needle_trap": {
      "text": "A spring-loaded needle jabs into your hand from inside the cabinet lock.",
      "penalty": { "ll": -2, "radiation": 1 },
      "status": 8,
      "wound": [1, 1],
      "ends_encounter": true
    }
  },

  // ── PLACEHOLDERS ──────────────────────────────────────────────
  // Dictionary of token -> word list for narrative variety.
  // The client resolves {{token_name}} in all text fields by picking a random entry.
  // Resolved independently per client (different players may see different text — this is fine).
  //
  // Guidelines for writing placeholders:
  //   - Each list should have 4-8 entries for good variety
  //   - Entries should be grammatically interchangeable in all contexts they appear
  //   - Keep entries short (1-4 words)
  //   - Use sensory details: smells, textures, sounds, light
  "placeholders": {
    "adjective": ["Ransacked", "Abandoned", "Burnt-Out", "Overgrown"],
    "debris_type": ["shattered tile", "fallen ceiling panels", "overturned furniture", "broken medical equipment"],
    "smell": ["the reek of old antiseptic", "a sharp chemical tang", "the sweetness of decay", "stale air thick with dust"],
    "adjective_condition": ["trashed but recognizable", "surprisingly intact", "half-collapsed", "scorched by old fire"],
    "cabinet_state": ["against the far wall, dented but closed", "toppled on its side", "behind an overturned gurney"],
    "glass_type": ["a rotten floor panel", "a hidden skylight", "a pile of medical vials"]
  }
}
```

### Encounter Authoring Prompt

Use the following prompt with an LLM to generate encounter JSON files in bulk:

---

**PROMPT:**

You are writing encounter JSON files for "WASTELAND," a post-apocalyptic survival crawl played on a hex grid. Each encounter is a push-your-luck dungeon dive where a player explores a location node by node, spending resources and risking hazards for loot.

**Target biome:** `{BIOME_NAME}` (terrain index `{TERRAIN_INDEX}`)
**File index:** `{FILE_NUMBER}` (this is `{FILE_NUMBER}.json` in the `{BIOME_FOLDER}/` directory)

**Schema rules:**
- `start_node`: first node key
- Nodes are a linear chain, 2-4 nodes deep. Each node has `text`, `can_bank`, and `choices`
- First node: `can_bank: false`. Mid and final nodes: `can_bank: true`
- Final node has `choices: []` (dead end — player must bank)
- Each choice pushes to the next node in the linear chain (no side rooms in v1)
- Every choice needs: `label`, `cost`, `base_risk`, `skill`, `success_node`, `hazard_id`
- Node `loot` and `loot_table` are granted after a successful choice reaches the node, not on arrival

**Cost keys** (positive integers, amount consumed):
`ll` (life level), `fatigue`, `radiation`, `food`, `water`, `resolve`

**Skill indices** (pick what fits the action):
0=Navigate, 1=Forage, 2=Scavenge, 3=Treat, 4=Shelter, 5=Endure

**base_risk guidelines:**
- 10-20: Easy (early nodes, simple grabs)
- 25-40: Moderate (standard push-deeper choices)
- 45-60: Risky (deep rooms, valuable loot behind danger)
- 65+: Dangerous (final rooms, high-value targets)

**Loot** (on nodes):
- `loot`: array of `{"res": 0-4, "qty": [min, max]}` — resource drops. 0=Water, 1=Food, 2=Fuel, 3=Medicine, 4=Scrap
- `loot_table`: string reference to a per-biome loot table for typed item drops. Use: `"{biome}_common"` for mid-tier, `"{biome}_rare"` for deep/final nodes

**Hazards:**
- `penalty`: stat changes (negative = damage). Keys: ll, fatigue, radiation, food, water, resolve
- `status`: bitmask. 1=wounded, 2=radsick, 4=bleeding, 8=fevered, 16=downed, 64=panicked. 0 or omit for none
- `wound`: `[type, count]` — 0=minor, 1=major, 2=critical. Omit if no wounds
- `ends_encounter`: true = encounter over + pending loot LOST. false = penalty applied, player continues
- Early-node hazards can be non-terminal (ends_encounter: false). Deeper-node hazards should usually be terminal
- If penalties would cause Downed state (LL 0), encounter ends immediately with all pending loot lost

**Placeholders:**
- Use `{{token_name}}` in all text fields for variety
- Define 4-8 entries per token in the `placeholders` dict
- Entries must be grammatically interchangeable
- Focus on sensory details: sights, sounds, smells, textures

**Tone:** Grim, tactile, lived-in. Short punchy sentences. Second person present tense. No humor unless dark/gallows. The world is dangerous but not hopeless — there are things worth finding.

**Generate a complete encounter JSON file. No comments in the output (pure JSON).**
24. **Version migration**: load a v4 save file, verify it's rejected cleanly and a fresh game starts
