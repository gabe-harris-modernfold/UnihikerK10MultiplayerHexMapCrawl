# Encounter Engine Implementation Plan

## Context
The game currently has a "not yet implemented" placeholder for encounters (see `actions_game_loop.hpp:134`). This plan adds a push-your-luck encounter engine triggered at POI hexes, integrating with the existing server-authoritative architecture, item/stat system, and multiplayer mechanics.

**Design decisions:**
- **Hybrid architecture**: Client renders text/UI, server validates rolls and state
- **Solo with assists**: One player runs the encounter; same-hex allies can spend resources to reduce risk
- **Stat-based items**: Equipment modifies player stats that feed into a unified risk formula (no named-item gates)
- **One-time global POIs**: Once any player completes a POI (bank OR fail), it's consumed for everyone
- **Per-encounter placeholders**: Each encounter JSON contains its own placeholder dictionary for text resolution
- **First come, first served**: If two players try to enter the same POI, the first claim wins; second is rejected
- **Resources + typed items**: Encounters can drop both resource quantities and typed items (equipment, consumables, materials)
- **Loot table references**: Encounter JSON references named loot tables defined in a separate SD file
- **2-4 nodes typical**: Short encounters, 1-3 minutes. Quick push-your-luck decisions
- **Linear with optional side rooms**: Main path is linear push-deeper, but some nodes offer side choices that return to the same depth
- **Survey reveals POI**: POI hexes are invisible until Surveyed; surveying shows an eye icon on hexes that contain encounters
- **Free entry**: No MP cost to enter an encounter; internal node costs handle resource drain
- **Server-side loot tables**: Server loads `loot_tables.json` at boot for authoritative item drops

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
          "success_node": "entrance",
          "hazard_id": "cut_hands",
          "side_room": true
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
- **`loot_table`** references a named table in `/encounters/loot_tables.json` for typed item drops
- **`skill`** uses skill indices 0-5 (Navigate, Forage, Scavenge, Treat, Shelter, Endure)
- **`side_room: true`** marks a choice whose `success_node` loops back to the current node (same depth). Optional quick-grab with risk, doesn't advance deeper
- **`hazard.ends_encounter: false`** allows non-terminal hazards (penalty applied but encounter continues)
- **Terminal nodes**: Nodes with empty `choices: []` and `can_bank: true` are encounter endpoints (deepest room)

### Loot Tables (`/encounters/loot_tables.json`)
Separate file on SD card, loaded by client at encounter start. Defines weighted item drop pools:
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
  ]
}
```
- `item: 0` = nothing (empty drop)
- `weight` = relative probability
- **Server resolves** the loot table roll (not client) to prevent cheating
- Loot tables loaded into memory at boot (~1-2KB)
- Rolled item is included in the `EVT_ENC_RESULT` event

---

## 2. Server-Side State (Minimal Heap)

### Add to `Esp32HexMapCrawl.ino`

**HexCell** -- add 1 byte (`poi`):
```cpp
struct HexCell {
  uint8_t terrain, resource, amount, respawnTimer, shelter, footprints, variant;
  uint8_t poi;  // 0=none/looted, 1-255=encounter pool index
};
```
Memory impact: +475 bytes (19x25).

**ActiveEncounter** -- per-player encounter tracking (~24 bytes x 6 = 144 bytes):
```cpp
#define ENC_MAX_ITEMS 3   // max typed items pending per encounter
struct ActiveEncounter {
  uint8_t  active;
  uint8_t  encIdx;         // poi value for hex marking
  uint8_t  hexQ, hexR;
  uint8_t  pendingLoot[5]; // accumulated unbanked resource loot
  uint8_t  pendingItemType[ENC_MAX_ITEMS]; // typed items awaiting bank
  uint8_t  pendingItemQty[ENC_MAX_ITEMS];
  uint8_t  pendingItemCount;               // how many typed items accumulated
  int8_t   assistRisk;     // accumulated risk reduction from assists
  uint8_t  assistUsed;     // bitmask: which allies assisted this node
};
static ActiveEncounter encounters[MAX_PLAYERS];
```

**POI discovery via Survey**: POI hexes are not visible on the map by default. When a player uses the Survey action, any hexes within survey range that have `poi != 0` are flagged with an eye icon on the client. This uses the existing Survey vis-update flow — the DD byte's bit 7 (has POI) is only sent for hexes the player has surveyed. Unsurveyed POI hexes appear as normal terrain.

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
```

---

## 3. Risk Formula

Uses existing stats and equipment (which modify stats at equip time):

```cpp
static uint8_t computeEncounterDN(int pid, uint8_t baseRisk, uint8_t skill,
                                   int8_t assistMod) {
  // Map base_risk (0-100) to raw DN (2-14)
  int effectiveRisk = constrain((int)baseRisk + assistMod, 0, 100);

  // Threat Clock escalation: +5 risk per threshold crossed
  if (G.threatClock >= TC_THRESHOLD_A) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_B) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_C) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_D) effectiveRisk += 5;
  effectiveRisk = constrain(effectiveRisk, 0, 100);

  int rawDN = 2 + (effectiveRisk * 12) / 100;

  Player& p = G.players[pid];
  int bonus = 0;
  if (p.ll > 4)        bonus += (p.ll - 4) / 2;      // health headroom
  if (p.resolve > 2)   bonus += (p.resolve - 2);       // willpower
  if (p.fatigue > 4)   rawDN += (p.fatigue - 4);       // exhaustion penalty
  if (p.radiation > 3) rawDN += (p.radiation - 3) / 2; // sickness penalty

  return (uint8_t)constrain(rawDN - bonus, 2, 14);
}
```

Resolution uses existing `resolveCheck(pid, skill, dn, 0)` which handles wounds/conditions.

---

## 4. Assist Mechanic

Same-hex allies can spend 1 resource each to reduce risk **before** the active player commits:

| Resource | Risk Reduction |
|----------|---------------|
| Medicine (3) | -5 |
| Fuel (2) | -5 |
| Food (1) | -3 |
| Water (0) | -3 |

- Max 1 assist per ally per node (tracked via `assistUsed` bitmask)
- Total assist reduction capped at -15 per node
- Assist resets when active player advances to a new node

---

## 5. WebSocket Message Flow

### Client -> Server
| Message | Fields | Purpose |
|---------|--------|---------|
| `enc_start` | `q, r` | Enter POI encounter |
| `enc_assist` | `target, res` | Ally spends resource to help |
| `enc_choice` | `ci, cost_ll, cost_fat, cost_rad, cost_food, cost_water, base_risk, skill, loot[5], can_bank` | Player picks a choice |
| `enc_bank` | -- | Bank accumulated loot, end encounter |
| `enc_abort` | -- | Leave encounter, lose pending loot |

### Server -> Client (via event queue)
| Event | Key Fields | Purpose |
|-------|-----------|---------|
| `EVT_ENC_START` | `pid, q, r` | Notify all: encounter begun |
| `EVT_ENC_ASSIST` | `pid (ally), target, res, riskRed` | Assist applied |
| `EVT_ENC_RESULT` | `pid, out, skill, dn, total, loot[5], penalties, status, ends` | Choice resolved |
| `EVT_ENC_BANK` | `pid, q, r, loot[5]` | Loot secured |
| `EVT_ENC_END` | `pid, q, r, reason` | Encounter ended (hazard/abort/dawn) |

**Validation on `enc_choice`**: Server checks player can afford costs, is in active encounter, and is not downed. Server resolves the roll. Client sends cost/risk values from the JSON; server trusts the encounter data but validates affordability.

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

The `poi` byte on HexCell stores a non-zero value (1-255) as a flag. The actual encounter file is selected randomly at runtime from the terrain's SD card pool — not baked into the map. This means the same POI hex yields a different encounter each game.

### Vis-Disk Encoding
Use **bit 7 of the DD byte** to signal POI presence to clients:
```
DD byte: bits 0-5 = footprints, bit 6 = shelter, bit 7 = has POI
```
No encoding format change needed.

### Looting
When encounter completes (bank OR hazard/fail), `G.map[r][q].poi = 0`. All players see the POI disappear on next vis update. The POI is consumed on any completion -- failing a hazard still "uses up" the location.

On bank: `pendingLoot[5]` added to `inv[5]`, `pendingItemType/Qty` added to typed inventory slots. If typed inventory is full, excess items drop to ground via existing `groundItems[]` system.

---

## 7. SD Card Layout & Serving

```
/encounters/
  index.json               <- terrain-to-count mapping
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

Files are numbered sequentially (0.json, 1.json, ...) for O(1) random selection — the server picks `random(0, count)` and serves that file. No directory traversal needed.

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

On `enc_start`, server picks a random file index for the hex's terrain type, sends the path to the client. Client fetches via HTTP GET.

---

## 8. Integration with Existing Systems

| System | Interaction |
|--------|------------|
| **Threat Clock** | Starting a ruins encounter increments TC by 1. TC thresholds add +5/+10/+15/+20 to base_risk. |
| **Day Cycle** | Dawn during active encounter -> forced abort (pending loot lost). Players cannot REST while in encounter. |
| **Movement** | Blocked while `encounters[pid].active`. Entering a POI hex does NOT auto-start; player must choose "Enter" action. Entry is free (no MP cost). |
| **Survey** | Surveying reveals POI hexes within range (eye icon). POIs are invisible until surveyed. Uses existing survey vis-update flow. |
| **Conditions** | Hazard `status` field sets statusBits (bleeding, fever, etc.). Existing condition penalties affect subsequent encounter checks via `resolveCheck()`. |
| **Score** | Banking loot: +10 per total resource unit. Full clear bonus: +25. |
| **Save** | `SAVE_VERSION` bumps to 5. `poi` byte persists in map binary. Active encounters are transient -- disconnecting mid-encounter loses pending loot. |

---

## 9. Client-Side Changes

### Files to Modify
- **`data/ui.js`** -- Encounter panel UI, choice buttons, risk bar, loot display, bank button, assist controls
- **`data/state-manager.js`** -- Handle `enc_*` event types in reducer
- **`data/engine.js`** -- POI hex rendering (read bit 7 of DD byte, draw POI marker)

### Text Resolution (client-side only)
```javascript
function resolveText(text, placeholders) {
  return text.replace(/\{\{(\w+)\}\}/g, (_, key) => {
    const opts = placeholders[key];
    return opts ? opts[Math.floor(Math.random() * opts.length)] : key;
  });
}
```

### Encounter UI
- Modal overlay when encounter is active
- Shows node text, choice buttons with cost/risk preview
- Risk bar (green->red based on effective risk)
- Accumulated loot display
- "Bank & Exit" button on `can_bank` nodes
- Ally assist section: shows same-hex allies, resource spend buttons

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
  //   - Zero or more "side room" choices (loop back to same node for quick grabs)
  //   - A banking option to secure accumulated loot and exit
  // Typical depth: 2-4 nodes. Deeper = riskier but more rewarding.
  "nodes": {

    "entrance": {
      // "text": Narrative description shown when player arrives at this node.
      //         Supports {{placeholder}} tokens. 1-3 sentences recommended.
      //         Written in second person present tense ("You see...", "The air smells of...").
      "text": "The clinic door hangs from one hinge. Inside, {{debris_type}} covers the floor. A {{smell}} hits you immediately.",

      // "can_bank": Whether the player can choose to bank loot and exit at this node.
      //   false = player must push forward or abort (losing all loot)
      //   true  = "Bank & Exit" button appears
      // Typically: entrance = false, mid-points and endpoints = true.
      "can_bank": false,

      // "loot": Resource loot gained automatically upon reaching this node (before choosing).
      //   "res": Resource index: 0=Water, 1=Food, 2=Fuel, 3=Medicine, 4=Scrap
      //   "qty": [min, max] — server rolls random quantity in this range (inclusive)
      //   Omit "loot" entirely if arriving at this node grants no resources.
      //   Loot goes into pendingLoot — not banked until player explicitly banks.

      // "loot_table": Reference to a named loot table in /encounters/loot_tables.json.
      //   Server rolls this table for a possible typed item drop (equipment, consumable, material).
      //   Omit if this node should not drop typed items.
      //   Resolved server-side. Result included in EVT_ENC_RESULT event.

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
          //   Final value is converted to a Difficulty Number (DN 2-14) for a 2d10 skill check.
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
          //   For push-deeper choices: points to the next node in the chain.
          //   For side_room choices: points back to the CURRENT node (same key as parent).
          "success_node": "exam_room",

          // "hazard_id": Key into the "hazards" dict. Triggered on a failed skill check.
          //   Every choice MUST have a hazard_id — there's always a consequence for failure.
          "hazard_id": "broken_glass"
        },
        {
          // Side room: a quick optional grab that loops back to the same node.
          // Lower risk, smaller reward. Does not advance depth.
          "label": "Grab supplies from the waiting room shelves.",
          "cost": { "fatigue": 1 },
          "base_risk": 10,
          "skill": 2,
          "success_node": "entrance",
          "hazard_id": "dust_inhalation",

          // "side_room": true marks this choice as a side grab.
          //   success_node MUST point back to the current node.
          //   The client uses this flag to display the choice differently (e.g., dimmer, indented).
          //   Optional field — omit for main-path choices.
          "side_room": true
        }
      ]
    },

    "exam_room": {
      "text": "The exam room is {{adjective_condition}}. A medicine cabinet stands {{cabinet_state}}.",
      "can_bank": true,
      "bank_text": "Retreat through the entrance with your haul.",
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
      // Reaching here is the "full clear" — maximum reward.
      "text": "Behind the cabinet, a hidden pharmacy. Rows of intact medicine bottles line the shelves.",
      "can_bank": true,
      "bank_text": "Carefully pack everything and leave the clinic.",
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
      // Non-terminal hazards are great for side rooms (small punishment, keep going).
      // Terminal hazards should be reserved for main-path failures or severe dangers.
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
  // Resolved once per encounter instance (same word used throughout one playthrough).
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

You are writing encounter JSON files for "Ashways," a post-apocalyptic survival crawl played on a hex grid. Each encounter is a push-your-luck dungeon dive where a player explores a location node by node, spending resources and risking hazards for loot.

**Target biome:** `{BIOME_NAME}` (terrain index `{TERRAIN_INDEX}`)
**File index:** `{FILE_NUMBER}` (this is `{FILE_NUMBER}.json` in the `{BIOME_FOLDER}/` directory)

**Schema rules:**
- `start_node`: first node key
- Nodes are a linear chain, 2-4 nodes deep. Each node has `text`, `can_bank`, and `choices`
- First node: `can_bank: false`. Mid and final nodes: `can_bank: true`
- Final node has `choices: []` (dead end — player must bank)
- Each main-path choice pushes to the next node. Optional `side_room: true` choices loop back to the current node
- Every choice needs: `label`, `cost`, `base_risk`, `skill`, `success_node`, `hazard_id`

**Cost keys** (positive integers, amount consumed):
`ll` (life level), `fatigue`, `radiation`, `food`, `water`, `resolve`

**Skill indices** (pick what fits the action):
0=Navigate, 1=Forage, 2=Scavenge, 3=Treat, 4=Shelter, 5=Endure

**base_risk guidelines:**
- 10-20: Easy (side rooms, simple grabs)
- 25-40: Moderate (standard push-deeper choices)
- 45-60: Risky (deep rooms, valuable loot behind danger)
- 65+: Dangerous (final rooms, high-value targets)

**Loot** (on nodes):
- `loot`: array of `{"res": 0-4, "qty": [min, max]}` — resource drops. 0=Water, 1=Food, 2=Fuel, 3=Medicine, 4=Scrap
- `loot_table`: string reference to a loot table for typed item drops. Use: `"{biome}_common"` for mid-tier, `"{biome}_rare"` for deep/final nodes

**Hazards:**
- `penalty`: stat changes (negative = damage). Keys: ll, fatigue, radiation, food, water, resolve
- `status`: bitmask. 1=wounded, 2=radsick, 4=bleeding, 8=fevered, 16=downed, 64=panicked. 0 or omit for none
- `wound`: `[type, count]` — 0=minor, 1=major, 2=critical. Omit if no wounds
- `ends_encounter`: true = encounter over + pending loot LOST. false = penalty applied, player continues
- Side room hazards should be non-terminal (ends_encounter: false). Main-path hazards at deeper nodes should usually be terminal

**Placeholders:**
- Use `{{token_name}}` in all text fields for variety
- Define 4-8 entries per token in the `placeholders` dict
- Entries must be grammatically interchangeable
- Focus on sensory details: sights, sounds, smells, textures

**Tone:** Grim, tactile, lived-in. Short punchy sentences. Second person present tense. No humor unless dark/gallows. The world is dangerous but not hopeless — there are things worth finding.

**Generate a complete encounter JSON file. No comments in the output (pure JSON).**

---

---

## 11. Implementation Phases

### Phase 1: Server Foundation
- Add `poi` to `HexCell`, bump save version
- Add `ActiveEncounter` struct
- Add `EVT_ENC_*` event types and GameEvent fields
- `computeEncounterDN()` function
- POI placement in `generateMap()`

### Phase 2: Server Handlers
- Load encounter index from SD at boot
- HTTP `/enc` route for JSON serving
- WebSocket handlers: `enc_start`, `enc_choice`, `enc_bank`, `enc_abort`, `enc_assist`
- Event serialization for encounter events
- Blocking checks (movement, actions, dawn abort)

### Phase 3: Client
- POI hex rendering (bit 7 of DD)
- Encounter JSON fetch + text resolution
- Encounter UI panel
- Assist UI
- Event handlers for `enc_*` messages

### Phase 4: Content
- 3-5 starter encounter JSON files across terrain types
- `index.json` encounter pool
- Placeholder word lists in encounter files

---

## Critical Files
| File | Changes |
|------|---------|
| `Esp32HexMapCrawl.ino` | HexCell.poi, ActiveEncounter, EVT_ENC_*, GameEvent fields, SAVE_VERSION=5 |
| `actions_game_loop.hpp` | Encounter blocking checks, dawn abort |
| `network-handlers.hpp` | WebSocket enc_* message handlers |
| `network-events.hpp` | EVT_ENC_* serialization |
| `hex-map.hpp` | POI placement in generateMap(), vis-disk bit 7 |
| `data/ui.js` | Encounter panel, assist UI |
| `data/state-manager.js` | enc_* event reducer cases |
| `data/engine.js` | POI hex rendering |

## Verification
1. Add 1 test encounter JSON to SD card, verify HTTP `/enc` serves it
2. Generate map, confirm POI hexes appear (check via serial debug or vis-disk)
3. Move onto POI hex, start encounter, verify `enc_start` event broadcasts
4. Make choices, verify server resolves rolls and sends `enc_result` events
5. Bank loot, verify `inv[]` updated and `poi` zeroed on hex
6. Test assist: second player on same hex spends resource, verify risk reduction
7. Test dawn abort: start encounter, let dawn fire, verify forced end
8. Test hazard: fail a roll, verify penalties applied and loot lost
