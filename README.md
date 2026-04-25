# WASTELAND A SURVIVAL HEX MAP ROGUE
## ESP32-S3 WebSocket Hex-Crawl Game that is ported to the DfRobot UniHiker K10 ESP32 S3 SBC.

A 6-player cooperative post-apocalyptic hex-crawl survival game running on the **Unihiker K10** microcontroller with embedded web server and real-time WebSocket synchronization. Does not require an internet connection to play. You can play by connecting directly to the wifi network access point "WASTELAND" on your phone or laptop.

---

## Overview

**WASTELAND CRAWL** is a collaborative survival exploration game designed for the **Unihiker K10**, an ESP32-S3 development board with an integrated 2.8" touchscreen display. The game runs a built-in WiFi access point that allows up to 6 players to connect via web browser on their phones or computers, forming a shared party navigating a procedurally generated post-apocalyptic hex grid.

### Platform: Unihiker K10 (ESP32-S3)

The **Unihiker K10** is a dual-core ESP32-S3 microcontroller with:
- **Processor:** ESP32-S3-N16R8 (dual-core Xtensa, 240 MHz)
- **Display:** 2.8" ILI9341 TFT LCD (240×320 pixels, portrait)
- **Connectivity:** WiFi 802.11 b/g/n, Bluetooth 5.0
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


#### Multi-Player Synchronization
- **WebSocket Protocol:** JSON messages broadcast position, actions, and game state changes
- **Low-Latency Updates:** 100ms game tick ensures responsive gameplay across 6 players
- **Conflict Resolution:** Server is authority; all decisions validated server-side
- **Player Names:** Customizable per-session; visible on hex grid and in party roster

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

To minimize bandwidth, hex cells are encoded as byte pairs.
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
- **USB-C power supply** (5V/2A or higher recommended)
- **MicroSD card** — FAT32 formatted, any size (even 1 GB is more than enough)
- **WiFi-enabled device** (phone, tablet, or laptop) for each player

### SD Card — Required

**The game will not boot without an SD card inserted.** If the card is missing or unreadable, the display shows `SD FAIL - insert card!` and halts.

The SD card serves two purposes:

1. **Web assets** — all HTML, CSS, JS, images, and JSON data are loaded from the card into PSRAM at boot and served to connected players' browsers.
2. **Save data** — the game writes `map.bin` and `players.bin` to a `/save/` directory on the card to persist world state between sessions. This directory is created automatically on first run.

#### Preparing the SD Card

Format the card as **FAT32**. Copy the entire `data/` folder from this repository to the **root** of the card, preserving the directory structure exactly:

```
SD card root/
├── data/
│   ├── index.html
│   ├── style.css
│   ├── items.cfg
│   ├── engine.js
│   ├── game-data.js
│   ├── game-config.js
│   ├── state-manager.js
│   ├── animation-manager.js
│   ├── event-handlers.js
│   ├── ui-state.js
│   ├── ui-utils.js
│   ├── ui-hud.js
│   ├── ui-panels.js
│   ├── ui-items.js
│   ├── ui-encounter.js
│   ├── network.js
│   ├── map-decoder.js
│   ├── renderer.js
│   ├── ash-particle-system.js
│   ├── weather-particle-system.js
│   ├── van.js
│   ├── van-ui.js
│   ├── sw.js
│   ├── img/
│   │   ├── hex*.png              (terrain tile variants)
│   │   ├── shelter*.png
│   │   ├── forrage*.png
│   │   ├── wastelandTitle0.png
│   │   ├── survivors/            (archetype portraits + pawns)
│   │   └── items/
│   └── encounters/
│       ├── index.json            (encounter pool manifest — required)
│       ├── loot_tables.json      (loot drop tables — required)
│       ├── dunes/
│       ├── flooded/
│       ├── forest/
│       ├── glass/
│       ├── marsh/
│       ├── mountain/
│       ├── ridge/
│       ├── scrub/
│       ├── settlement/
│       └── urban/
└── save/                         (created automatically by the game)
    ├── map.bin
    └── players.bin
```

Missing `index.html` triggers a boot warning. Missing `encounters/index.json` or `loot_tables.json` disables encounters silently (the game still runs).

#### USB Drive Mode — Easy File Transfer

If you need to update SD card files without removing the card:

1. While the boot splash is showing **"Hold [A] now = USB drive"**, hold **Button A**.
2. The K10 mounts the SD card as a USB mass storage device. The display shows `USB DRIVE MODE`.
3. On your PC, the card appears as a removable drive — copy the `data/` folder normally.
4. Safely eject the drive on your PC, then reboot the K10 (unplug and replug power).

The game does not start while in USB drive mode.

### Installation

1. Install the **UNIHIKER board package** in Arduino IDE (via Boards Manager)
2. Upload `Esp32HexMapCrawl.ino` to the K10 via USB-C
3. Insert a prepared MicroSD card (see above)
4. Once booted, the K10 displays "WASTELAND CRAWL" splash screen
5. Players connect to WiFi `WASTELAND` and open `http://192.168.4.1/`

### Repository File Structure

```
Esp32HexMapCrawl/
├── Esp32HexMapCrawl.ino          # Main sketch
├── boot-assets.hpp               # SD→PSRAM loader, item registry
├── hex-map.hpp                   # Hex math, map generation, fog of war
├── ui-display.hpp                # K10 display, LED, audio, boot splash
├── usb_drive.h                   # USB mass storage mode
├── network-*.hpp                 # WebSocket sync, events, session handling
├── actions_game_loop.hpp         # Action handlers (forage, scavenge, shelter…)
├── survival_state.hpp            # Day cycle, movement, resource collection
├── inventory_items.hpp           # Item effects, equipment, trade
├── data/                         # All files that must be copied to SD card
│   ├── index.html
│   ├── style.css
│   ├── items.cfg
│   ├── *.js                      # Game client modules
│   ├── img/                      # Terrain tiles, survivor art
│   └── encounters/               # Encounter JSON + loot tables
└── README.md
```

---

## Design Philosophy

**WASTELAND CRAWL** prioritizes:
- **Minimal Bandwidth:** Efficient encoding and delta-sync protocols
- **Low Latency:** 100ms tick ensures responsive gameplay across 10 simultaneous players
- **Offline Play:** Self-contained; no internet or external servers required
- **Accessibility:** Simple web interface; no client installation
- **Emergent Gameplay:** Procedural generation and player cooperation create unique stories each session
- 
---

## Credits & License

**Designed for:** Unihiker K10 ESP32-S3 with integrated TFT display
**Game Engine:** ESPAsyncWebServer + Custom WebSocket Protocol
**Theme:** Post-apocalyptic survival exploration
**Players:** Up to 6 simultaneous via WiFi

---

*Navigate the wasteland. Survive together. Every hex tells a story.*
