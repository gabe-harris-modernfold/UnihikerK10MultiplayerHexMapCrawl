# WASTELAND CRAWL
## ESP32-S3 WebSocket Hex-Crawl Game

A 10-player cooperative post-apocalyptic hex-crawl survival game running on the **Unihiker K10** microcontroller with embedded web server and real-time WebSocket synchronization.

---

## Overview

**WASTELAND CRAWL** is a collaborative survival exploration game designed for the **Unihiker K10**, an ESP32-S3 development board with an integrated 2.8" touchscreen display. The game runs a built-in WiFi access point that allows up to 10 players to connect via web browser on their phones or computers, forming a shared party navigating a procedurally generated post-apocalyptic hex grid.

### Platform: Unihiker K10 (ESP32-S3)

The **Unihiker K10** is a dual-core ESP32-S3 microcontroller with:
- **Processor:** ESP32-S3-N16R8 (dual-core Xtensa, 240 MHz)
- **Display:** 2.8" ILI9341 TFT LCD (240×320 pixels, portrait)
- **Connectivity:** WiFi 802.11 b/g/n, Bluetooth 5.0
- **Built-in Gesture Recognition:** Capacitive touch with multi-touch support
- **Arduino Ecosystem:** Full compatibility via UNIHIKER board package

The K10's dual-core architecture enables:
- **Core 0:** WiFi stack, HTTP requests, WebSocket messaging (handled by ESPAsyncWebServer)
- **Core 1:** Game logic loop (100ms tick), hex grid calculations, visibility checks, state management

This separation allows real-time network communication without blocking game logic.

### Architecture: Embedded Web Server

The game uses **ESPAsyncWebServer** to:
1. **Host an Access Point** at `WASTELAND` (192.168.4.1) with no authentication
2. **Serve a Single-Page Application (SPA):** HTML/CSS/JavaScript interface served from SPIFFS
3. **Manage WebSocket Connections:** Bidirectional JSON-based protocol for game state synchronization
4. **Handle Static Assets:** Stylesheet, game client code, and UI elements

Players connect via WiFi and open a browser to `http://192.168.4.1/` to join the game. Each connected player receives real-time updates about the hex grid, other players, and game events through WebSocket messages.

---

## The Game

### What is WASTELAND CRAWL?

**WASTELAND CRAWL** is a **tactical survival exploration game** set in a post-nuclear wasteland. A party of survivors navigates an endless procedurally generated hex grid, managing resources (food, water), avoiding hazards (radiation, terrain penalties), and discovering artifacts while cooperating to stay alive.

**Key Themes:**
- **Survival:** Food and water are limited; players must forage, collect water, and ration resources
- **Exploration:** Each hex reveals new terrain types with different visibility ranges, dangers, and loot
- **Radiation:** Some terrains are radioactive; prolonged exposure causes sickness and stat penalties
- **Cooperation:** All players share the same game state; decisions affect the entire party
- **Emergent Storytelling:** Random events, terrain hazards, and resource scarcity create tense moments

### Core Mechanics

#### Hex Grid
- **Size:** 25×19 toroidal grid (wraps around edges)
- **Terrain System:** 11 distinct post-apocalyptic terrain types, each with unique properties:
  - **Open Scrub, Ash Dunes, Marsh, Flooded Ruins, River Channel:** Standard visibility & traversal
  - **Rust Forest:** Obscures vision (BLIND terrain)
  - **Broken Urban:** Difficult to traverse (penalties)
  - **Glass Fields, Rolling Hills, Mountain:** Extended visibility, rare resources
  - **Settlement:** Reduced visibility but scavenging opportunity

Each hex contains:
- **Terrain type** (affects visibility range, traversal difficulty)
- **Resources:** Food, water, ammunition, medicine (randomized per terrain)
- **Radiation levels** (on Ash Dunes and Glass Fields)
- **Built shelters:** Persistent player-built structures for refuge

#### Resource Management (Daily Cycle)
Every **5 minutes (in-game day)**, players must manage:
- **Food (F):** Consumed daily; players lose resolve (LL) if depleted
- **Water (W):** Consumed daily; players lose resolve if depleted
- **Resolve (LL):** Physical/mental stamina; 10 total → 0 causes incapacitation
- **Wounds:** Reduce available resolve; accumulated from hazards or failures

**Action Economy:** Each player gets **3 action points (MP)** per day. Actions cost MP:
- **FORAGE** (1 MP): Gather food from terrain
- **COLLECT WATER** (1 MP): Gather water (variable amount via stepper)
- **SCAVENGE** (1 MP): Search hex for random valuable artifacts
- **BUILD SHELTER** (2 MP): Create permanent shelter on current hex (restored on rest)
- **TREAT** (1 MP): Heal wounds or recover from radiation
- **SURVEY** (1 MP): Scan surrounding hexes for terrain & resources
- **REST** (1 MP): Restore resolve; if all players rest, day ends immediately

#### Radiation System
- **Rad-Tagged Terrains:** Ash Dunes and Glass Fields emit radiation
- **Radiation Exposure (R):** Increases each turn spent on rad terrain; decreases naturally at dawn
- **Radiation Sickness:** At R ≥ 4, player becomes RAD-SICK (visual/mechanical debuff)
- **Dusk Check:** At day's end, if R ≥ 7, player must pass **Endure DN8** check or suffer a major wound
- **Decontamination:** Rest in non-rad areas to naturally reduce R; TREAT action reduces R by 2

#### Visibility & Exploration
- **Visibility Range (VR):** Depends on terrain; ranges from 0 hexes (BLIND terrain) to 2+ hexes (high ground)
- **Client-Side Caching:** Each player maintains a local visibility cache; server syncs only changed cells
- **Shared View:** All players see the same revealed hexes (collaborative mapping)

#### Multi-Player Synchronization
- **WebSocket Protocol:** JSON messages broadcast position, actions, and game state changes
- **Low-Latency Updates:** 100ms game tick ensures responsive gameplay across 10 players
- **Conflict Resolution:** Server is authority; all decisions validated server-side
- **Player Names & Colors:** Customizable per-session; visible on hex grid and in party roster

---

## How to Play

### Starting the Game

1. **Power on the K10** and wait for the bootscreen (displays "WASTELAND CRAWL")
2. **Activate WiFi AP:** Once booted, the K10 broadcasts WiFi network `WASTELAND` (no password)
3. **Connect Players:**
   - On phone/laptop, connect to WiFi network `WASTELAND`
   - Open browser and navigate to `http://192.168.4.1/`
   - Enter your survivor name and click **ENTER WASTELAND**
4. **K10 Display:** Shows a status dashboard with all connected players, their stats, and game time

### Playing a Turn

Each **day cycle (5 minutes)** plays out as follows:

#### Morning (Dusk Check Resolution)
- If radiation R ≥ 7, affected players resolve an **Endure DN8** check
- Failures cause major wounds and resolve loss
- Radiation naturally decays if player rested in non-rad zones

#### Action Phase
Each player secretly chooses one action (in parallel):
1. **FORAGE:** Gather food from current terrain type
2. **COLLECT WATER:** Gather water (select amount with stepper)
3. **SCAVENGE:** Roll for random artifacts (ammo, medicine, tools)
4. **BUILD SHELTER:** Spend 2 MP to create a permanent shelter (visible as 🏕 emoji)
5. **TREAT:** Heal wounds or reduce radiation (costs medicine/resolve)
6. **SURVEY:** Peek at hexes one ring beyond visibility
7. **REST:** Restore 2 resolve; if all players rest, day ends immediately

#### Movement Phase
After taking an action, use remaining movement points (MP) to navigate:
- **Standard hex:** 1 hex costs 1 MP
- **Difficult terrain:** Costs extra MP (penalty terrain)
- **Encumbered:** Carrying heavy items costs extra MP
- **Wounded/Sick:** Wounds and radiation reduce available MP

#### Resource Consumption
At **dusk**, food and water are consumed:
- Lose 1 food and 1 water per player
- If below threshold, lose 1 resolve per player below threshold
- Zero food + zero water = immediate resolve loss

#### Night Phase
- **Radiation decay:** Reduce R by 1 if in non-rad terrain
- **Heal resting:** Resting players restore 2 resolve
- **Night events:** Random encounters or hazards (rare)

#### Dawn (Next Day)
- **Day counter increments**
- **All MP and action flags reset**
- **Radiation sickness check:** At R ≥ 4, player flagged as RAD-SICK (affects morale on K10 display)

### Winning Conditions

**WASTELAND CRAWL** is a **sandbox survival game** with no fixed end state. Goals are emergent:

- **Survival:** Keep all party members above 0 resolve
- **Exploration:** Map the entire hex grid
- **Scavenging:** Collect rare artifacts and powerful items
- **Building:** Establish shelters across the map for future parties
- **Cooperation:** Coordinate movement and action choices for maximum efficiency

### Losing Conditions

- **Party Wipe:** If all players are incapacitated (resolve = 0), the run ends
- **Abandonment:** If a player disconnects and doesn't rejoin before day's end, their character may be left behind

---

## Technical Details

### Networking

- **AP Mode:** K10 runs as WiFi access point (no internet required)
- **Protocol:** WebSocket (WS) with JSON payloads
- **Message Types:**
  - `sync`: Full game state (hex grid, players, resources)
  - `s`: Incremental state update (single player)
  - `ev`: Game event (action taken, movement, resource change)
  - `vis`: Visibility disk update (hex cell changes)
  - `asgn`: Assign player to party slot
  - `full`: Request full state resync

### Data Encoding

To minimize bandwidth, hex cells are encoded as 2-byte pairs:
- **Byte 0:** Terrain type (0–10)
- **Byte 1:** Resource amount, shelter flag, contamination

This allows a 25×19 grid to be transmitted in ~1 KB.

### Dual-Core Execution

- **Core 0 (WiFi):** Handles WebSocket, HTTP, and network I/O (priority 0)
- **Core 1 (Game):** Runs 100ms game tick, hex calculations, and player logic (priority 2)
- **Gesture Recognizer:** Background task (priority 5) handles K10 touchscreen

No blocking calls between cores; game logic never stalls network communication.

---

## Getting Started

### Hardware Requirements
- **Unihiker K10** development board
- **USB-C power supply** (5V/2A or higher)
- **WiFi-enabled device** (phone, tablet, or laptop) for each player

### Installation

1. Install the **UNIHIKER board package** in Arduino IDE (via Boards Manager)
2. Upload `Esp32HexMapCrawl.ino` to the K10 via USB-C
3. Once booted, the K10 displays "WASTELAND CRAWL" splash screen
4. Players connect to WiFi `WASTELAND` and open `http://192.168.4.1/`

### File Structure

```
Esp32HexMapCrawl/
├── Esp32HexMapCrawl.ino          # Main sketch
├── data/
│   ├── index.html                # SPA HTML
│   ├── style.css                 # Themed stylesheet
│   └── (game client JavaScript)
└── README.md                      # This file
```

---

## Design Philosophy

**WASTELAND CRAWL** prioritizes:
- **Minimal Bandwidth:** Efficient encoding and delta-sync protocols
- **Low Latency:** 100ms tick ensures responsive gameplay across 10 simultaneous players
- **Offline Play:** Self-contained; no internet or external servers required
- **Accessibility:** Simple web interface; no client installation
- **Emergent Gameplay:** Procedural generation and player cooperation create unique stories each session

---

## Credits & License

**Designed for:** Unihiker K10 ESP32-S3 with integrated TFT display
**Game Engine:** ESPAsyncWebServer + Custom WebSocket Protocol
**Theme:** Post-apocalyptic survival exploration
**Players:** Up to 10 simultaneous via WiFi

---

*Navigate the wasteland. Survive together. Every hex tells a story.*
