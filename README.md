# WASTELAND CRAWL
## ESP32-S3 WebSocket Hex-Crawl Game

A 6-player cooperative post-apocalyptic hex-crawl survival game running on the **Unihiker K10** microcontroller with embedded web server and real-time WebSocket synchronization. Does not require an internet connection to play. You can play by connecting directly to the wifi network "WASTELAND" on your phone or laptop.

---

## Overview

**WASTELAND CRAWL** is a collaborative survival exploration game designed for the **Unihiker K10**, an ESP32-S3 development board with an integrated 2.8" touchscreen display. The game runs a built-in WiFi access point that allows up to 6 players to connect via web browser on their phones or computers, forming a shared party navigating a procedurally generated post-apocalyptic hex grid.

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
- **Resolve (LL):** Physical/mental stamina; max 6 → 0 causes incapacitation
- **Wounds:** Reduce available movement points; accumulated from hazards or failures

**Movement Points (MP):** Derived from `max(2, LL - wounds - encumbrance)`. Even at worst condition, players retain minimum 2 MP to stay mobile.

**Action Economy:** Each player gets **1 action per day** (once used, no more actions that day). Available actions:
- **FORAGE** (2 MP, check needed): Gather food from terrain (+1 to +2 food, +3 pts)
- **COLLECT WATER** (1 MP, no check): Gather water by choice (1–3 tokens, +1–3 pts per token)
- **SCAVENGE** (2 MP, check needed): Search ruins for artifacts (+5 pts success, +2 partial)
- **BUILD SHELTER** (1–2 MP, no check): Create permanent shelter on hex (1–2 MP depending on scrap count; +4–8 pts; each hex surveys once per player)
- **TREAT** (2 MP, check needed): Heal wounds or reduce radiation (+3 pts on success)
- **SURVEY** (1 MP Scout-free): Scan hexes one ring beyond vision (+2 pts first survey per hex, +0 repeats; each hex per player capped at 1 survey)
- **REST** (0 MP, no action slot): Restore fatigue and resolve; if all players rest, day ends immediately

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
- **Exploration Bonus:** First player to visit a hex earns +1 score (reward for discovery)
- **SURVEY Mechanic:** Peek at hexes beyond normal vision; each hex can be surveyed once per player for +2 pts

#### Multi-Player Synchronization
- **WebSocket Protocol:** JSON messages broadcast position, actions, and game state changes
- **Low-Latency Updates:** 100ms game tick ensures responsive gameplay across 6 players
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
   - Enter your survivor name and choose your archetype (Guide, Quartermaster, Medic, Mule, Scout, or Endurer)
   - Click **ENTER WASTELAND**
4. **Starting Resources:**
   - **All survivors:** 1 food token, 2 water tokens
   - **Mule archetype only:** 2 food, 2 water, 1 medicine, 1 scrap (higher carrying capacity to reflect quartermaster role)
   - These limited starting resources create pressure to forage/collect immediately
5. **K10 Display:** Shows a status dashboard with all connected players, their stats, and game time

### Playing a Turn

Each **day cycle (5 minutes)** plays out as follows:

#### Morning (Dusk Check Resolution)
- If radiation R ≥ 7, affected players resolve an **Endure DN8** check
- Failures cause major wounds and resolve loss
- Radiation naturally decays if player rested in non-rad zones

#### Action Phase
Each player may take **one action** (or skip) from:
1. **FORAGE** (2 MP): Roll Forage skill; success gives +1–2 food, +3 pts
2. **COLLECT WATER** (1 MP): No roll; choose how much to gather (1–3 tokens, +1–3 pts)
3. **SCAVENGE** (2 MP): Roll Scavenge; success gives scrap/tools, +5 pts
4. **BUILD SHELTER** (1–2 MP): No roll; spend scrap to build shelter (visible as 🏕 or 🏠), +4–8 pts
5. **TREAT** (2 MP): Roll Treat; success heals wounds/radiation, +3 pts
6. **SURVEY** (1 MP, free for Scout): Reveal hexes at range; +2 pts on first survey per hex
7. **REST** (0 MP, always available): Restore fatigue; if all players rest, day ends early

#### Movement Phase
After taking an action (or choosing none), use remaining movement points (MP) to navigate:
- **Standard hex:** 1 MP per hex
- **First visit bonus:** +1 score when entering a new hex (automatic)
- **Difficult terrain:** Costs extra MP (1–2 depending on terrain)
- **Encumbered:** Carrying over max items costs 1 extra MP (applied once per day)
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

### Scoring & Winning Conditions

**WASTELAND CRAWL** is a **sandbox survival game** with emergent scoring. Your party's **score** is earned through:

- **Exploration:** +1 pt per new hex discovered (first visit only)
- **Foraging:** +1–3 pts depending on roll quality
- **Scavenging:** +2–5 pts depending on success
- **Shelter Building:** +4 pts (basic shelter), +8 pts (improved shelter)
- **Treatment:** +3 pts per successful condition healed
- **Survey Reconnaissance:** +2 pts per hex surveyed (first survey only; repeats yield no points)
- **Water Collection:** +1 pt per water token collected

**Goals are emergent:**
- **Survival:** Keep all party members above 0 resolve
- **Exploration:** Map the entire hex grid for maximum discovery bonuses
- **Scavenging:** Collect rare artifacts and build a well-stocked depot
- **Building:** Establish shelter network across the map as checkpoints
- **Cooperation:** Coordinate movement and resource sharing for maximum efficiency
- **High Score:** Maximize total party score through exploration + successful actions

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
