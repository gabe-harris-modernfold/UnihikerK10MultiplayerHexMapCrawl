/*
 * ESP32-S3 Hex Map Crawl - 6 Player Co-op | Wayfarer System
 * Post-Apocalyptic World | No Combat | Server-Authoritative Fog of War
 *
 * Core 0 (PRO_CPU): WiFi, HTTP/WebSocket I/O, SD (SPI)
 * Core 1 (APP_CPU): Game loop — resource respawn, state broadcast
 *
 * Libraries: ESPAsyncWebServer, AsyncTCP
 *
 * Hex Grid : Flat-top axial coordinates, 25×19 toroidal wraparound
 * Fog      : Each player sees cells within their effective vision radius.
 *            encodeMapFog() sends 0xFF (terrain byte) for invisible cells.
 *            On every move the server sends a full fresh vision-disk ("vis").
 *            Respawn events sent only to players currently within range.
 *
 * Map gen  : Phase 1 = independent weighted fill (T_BASE%).
 *            Phase 2 = SMOOTH_PASSES cellular passes; each cell re-rolls with
 *                      weight = T_BASE[t] * (100 + TERRAIN_CLUMP[t] * neighbourCount[t]).
 *                      High CLUMP% → that terrain clusters into organic blobs/lines.
 *            Phase 3 = resource placement (terrain-specific loot tables).
 *
 * Encoding : 2 bytes / cell (4 hex chars):
 *              Byte 1 = terrain (0x00-0x0A) or 0xFF (fog)
 *              Byte 2 = (resource<<2)|(amount&3)|(shelter<<5), or 0x00 if masked/fogged
 *
 * Vis-disk : {"t":"vis","vr":N,"q":QQ,"r":RR,"cells":"QQRRTTDD..."}
 *              QQ=col, RR=row, TT=terrain, DD=data  (2 hex chars each)
 *
 * 11 Terrain types (index 0-10):
 *   0 Open Scrub    MC=1  SV=0  vis=STANDARD
 *   1 Ash Dunes     MC=2  SV=0  vis=STANDARD
 *   2 Rust Forest   MC=2  SV=1  vis=PENALTY (resources masked)
 *   3 Marsh         MC=3  SV=0  vis=STANDARD
 *   4 Broken Urban  MC=2  SV=1  vis=PENALTY
 *   5 Flooded Ruins MC=3  SV=2  vis=STANDARD
 *   6 Glass Fields  MC=3  SV=0  vis=HIGH
 *   7 Rolling Hills MC=2  SV=1  vis=VHIGH
 *   8 Mountain      MC=4  SV=2  vis=STANDARD
 *   9 Settlement    MC=1  SV=3  vis=STANDARD
 *  10 Nuke Crater   MC=∞  SV=0  vis=STANDARD (impassable)
 *
 * WiFi: AP mode, SSID "WASTELAND", IP 192.168.4.1
 *
 * ── Serial debug output key ──────────────────────────────────────
 *   [SETUP]   Startup milestones and config summary
 *   [MAP]     Terrain and resource distribution after generation
 *   [CONNECT] New player joined: slot, position, terrain, vis params
 *   [DISCONN] Player left: name, steps, score, session duration
 *   [MOVE]    Every successful hex move: terrain, vis, cooldown applied
 *   [BLOCKED] Movement rejected: Nuke Crater (impassable)
 *   [COLLECT] Resource picked up: type, amount, new inventory, score
 *   [RESPAWN] Resource regenerated on map cell
 *   [NAME]    Player renamed their call sign
 *   [SYNC]    Initial sync message size sent to a client
 *   [VIS]     Vision-disk size and params sent after each move
 *   [STATUS]  Periodic full player table + map resource summary (30 s)
 *   [HEAP]    Free heap warning if below 100 KB
 *
 * ── File structure ───────────────────────────────────────────────
 *   Esp32HexMapCrawl.ino  — includes, constants, structs, globals,
 *                           hex math, slot mgmt, map gen, encoding,
 *                           printStatus, setup, loop
 *   game_logic.h          — skill checks, resource economy, movement,
 *                           dawn/dusk, actions
 *   network.h             — sync, broadcast, event drain, WS handlers
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include <FS.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include "unihiker_k10.h"
#include "usb_drive.h"

static const char* AP_SSID = "WASTELAND";

// ── WiFi STA connection (background task) ──────────────────────
struct WifiTaskCtx { char ssid[33]; char pass[65]; };
static volatile bool wifiConnecting = false;  // guard against concurrent attempts
// SSID/pass of last successful STA join — populated from WiFi.SSID()/psk() on
// connect; used by handleConnect to echo creds back to new clients.
static char savedSsid[33] = {0};
static char savedPass[65] = {0};
// Boot-time async STA connect state (monitored in loop()).
static bool     bootWifiPending = false;
static uint32_t bootWifiStartMs = 0;
static constexpr uint32_t BOOT_WIFI_TIMEOUT = 12000;

// ── Constants ──────────────────────────────────────────────────
static constexpr int      MAP_COLS      = 25;
static constexpr int      MAP_ROWS      = 19;
static constexpr int      MAX_PLAYERS   = 6;    // one per Wayfarer archetype
static constexpr int      VISION_R      = 3;    // base hex vision radius
static constexpr int      NUM_TERRAIN   = 11;
static constexpr uint32_t TICK_MS       = 100;  // 10 ticks/sec
static constexpr uint8_t  RESPAWN_TICKS = 200;  // 20 s resource respawn
static constexpr uint32_t MOVE_CD_MS    = 220;  // base move cooldown (ms)
static constexpr uint32_t STATUS_MS     = 30000; // player status table interval

// ── Wayfarer system constants ───────────────────────────────────
static constexpr int      NUM_ARCHETYPES  = 6;
static constexpr int      NUM_SKILLS      = 6;
static constexpr int      INV_SLOTS_STD   = 8;   // standard inventory capacity
static constexpr int      INV_SLOTS_MULE  = 12;  // Mule archetype capacity
static constexpr int      INV_SLOTS_MAX   = 12;  // array dimension
static constexpr uint32_t DAY_TICKS       = 3000; // 1 game-day = 5 min at 100 ms/tick
static constexpr uint8_t  TC_THRESHOLD_A  = 5;   // Threat Clock: Encounter card
static constexpr uint8_t  TC_THRESHOLD_B  = 9;   // Hazard DN +1 + Encounter card
static constexpr uint8_t  TC_THRESHOLD_C  = 13;  // Encounter roll threshold -1
static constexpr uint8_t  TC_THRESHOLD_D  = 17;  // CRISIS STATE

// Skill indices
static constexpr int SK_NAVIGATE = 0;
static constexpr int SK_FORAGE   = 1;
static constexpr int SK_SCAVENGE = 2;
static constexpr int SK_TREAT    = 3;
static constexpr int SK_SHELTER  = 4;
static constexpr int SK_ENDURE   = 5;

// ── Action system constants (§5 Turn/Action rules) ─────────────
static constexpr uint8_t ACT_FORAGE  = 0;  // Any hex with Forage tag; cost 2 MP
static constexpr uint8_t ACT_WATER   = 1;  // Any hex with Water tag;  cost 1-3 MP
static constexpr uint8_t ACT_SCAV    = 3;  // Salvage/Ruins hex;       cost 2 MP
static constexpr uint8_t ACT_SHELTER = 4;  // Any hex;                 cost 3 MP
static constexpr uint8_t ACT_TREAT   = 5;  // Any hex;                 cost 2 MP
static constexpr uint8_t ACT_SURVEY  = 6;  // Any hex;                 cost 1 MP
static constexpr uint8_t ACT_REST    = 7;  // Any hex;                 cost 0 MP (action slot)

// Action outcomes
static constexpr uint8_t AO_BLOCKED = 0;   // not available / insufficient MP / already acted
static constexpr uint8_t AO_SUCCESS = 1;   // full success
static constexpr uint8_t AO_PARTIAL = 2;   // partial success (total = DN-1)
static constexpr uint8_t AO_FAIL    = 3;   // failure

// Treat condition targets
static constexpr uint8_t TC_MINOR  = 0;
static constexpr uint8_t TC_BLEED  = 1;
static constexpr uint8_t TC_FEVER  = 2;
static constexpr uint8_t TC_MAJOR  = 3;  // requires Med Kit
static constexpr uint8_t TC_RAD     = 4;  // requires Anti-Rad
static constexpr uint8_t TC_GRIEVOUS= 5;  // Grievous Wound — Settlement only; Adv Med Kit req (§7.3A)

// Forage DN per terrain (0 = not available); mirrors SK_DN[1][] in client
static const uint8_t TERRAIN_FORAGE_DN[NUM_TERRAIN]  = { 7,0,6,8,0,0,0,0,0,0,0 };  // [0]=Open Scrub hunt DN7, [2]=Rust Forest DN6, [3]=Marsh DN8
// Salvage DN per terrain (0 = not available)
static const uint8_t TERRAIN_SALVAGE_DN[NUM_TERRAIN] = { 0,0,0,0,6,0,8,0,0,0,0 };
// Hex has Water tag (Marsh, Flooded Ruins)
static const bool    TERRAIN_HAS_WATER[NUM_TERRAIN]  = { 0,0,0,1,0,1,0,0,0,0,0 };
// Ruins: Scavenge here advances the Threat Clock
static const bool    TERRAIN_IS_RUINS[NUM_TERRAIN]   = { 0,0,0,0,1,0,0,0,0,0,0 };
// Rad-tagged terrain: entering triggers Endure roll vs +1 R (§6.2)
static const bool    TERRAIN_IS_RAD[NUM_TERRAIN]     = { 0,1,0,0,0,0,1,0,0,0,0 };
//                                                         0 1 2 3 4 5 6 7 8 9 10
//                                                         AshDunes(1), GlassFields(6)

// Status condition bitmask positions
static constexpr uint8_t ST_WOUNDED  = (1 << 0);
static constexpr uint8_t ST_RADSICK  = (1 << 1);
static constexpr uint8_t ST_BLEEDING = (1 << 2);
static constexpr uint8_t ST_FEVERED  = (1 << 3);
static constexpr uint8_t ST_DOWNED   = (1 << 4);
static constexpr uint8_t ST_STABLE   = (1 << 5);
static constexpr uint8_t ST_PANICKED = (1 << 6);

// Movement Cost per terrain (255 = impassable)
// Resource: 0=None 1=Water 2=Food 3=Fuel 4=Medicine 5=Scrap
static const uint8_t TERRAIN_MC[NUM_TERRAIN]  = { 1, 2, 2, 3, 2, 3, 3, 2, 4, 1, 255 };

// Visibility level: +2=VHIGH (radius 5), +1=HIGH (radius 4), 0=STD (radius 3),
//                   -1=LOW (radius 2), -2=PENALTY (radius 1, resources masked), -3=BLIND (radius 0)
static const int8_t  TERRAIN_VIS[NUM_TERRAIN] = { 0, 0, -3, 0, -2, 0, 1, 2, 2, -1, 0 };
//                                                 0  1   2  3   4  5  6  7  8   9  10
//  Rust Forest(2)=-3: dense canopy, zero visibility beyond standing hex
//  Flooded Ruins(5)=0: cleared ruins, standard visibility (murky but navigable)
//  Broken Urban(4)=-2: rubble and ruin, radius 1 only (resources masked)
//  Settlement(9)=-1: walled/enclosed, restricted sightlines (radius 2)
//  Rolling Hills(7)=+2, Mountain(8)=+2: elevated vantage, exceptional sightlines (radius 5)

// Shelter Value (§7.3): natural protection of terrain (0–3)
// 0=no cover, 1=some cover, 2=good shelter, 3=full cover
static const uint8_t TERRAIN_SV[NUM_TERRAIN]  = { 0, 0,  1, 0,  1,  2, 0, 1, 2, 3, 0 };
//                                                  0  1   2  3   4   5  6  7  8  9  10
//  Flooded Ruins(5) = 2: standing buildings provide good shelter (§7.3)

// ── Debug label tables ─────────────────────────────────────────
// Short names (≤10 chars) for serial output alignment
static const char* T_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes ", "RustForst", "Marsh    ",
  "BrknUrban", "FloodRuin", "GlassFlds", "RolngHill",
  "Mountain ", "Settlment", "NukeCratr"
};
static const char* T_SHORT[NUM_TERRAIN] = {
  "Scrub","Dunes","Forst","Marsh","Urban","Ruins","Glass","Hills","Mtn  ","Settl","Nukr "
};
// vis level label (index = TERRAIN_VIS[t]+3 → 0..5 for range -3..+2)
static const char* TERRAIN_IMG_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes", "RustForest", "Marsh",
  "BrokenUrban", "FloodedDistrict", "GlassFields",
  "Ridge", "Mountain", "Settlement", "NukeCrater"
};
static const char* VIS_LABEL[6] = { "BLIND", "PENLT", "LOW  ", "STD  ", "HIGH ", "VHIGH" };
static const char* RES_NAME[6]  = { "None","Water","Food ","Fuel ","Med  ","Scrap" };
static const char* DIR_NAME[6]  = { "SE","NE","N ","NW","SW","S " };
static const char* SKILL_NAME[NUM_SKILLS] = {
  "Navigate","Forage  ","Scavenge","Treat   ","Shelter ","Endure  "
};
// ── Wayfarer archetype tables ───────────────────────────────────
// Slot 0=Guide 1=Quartermaster 2=Medic 3=Mule 4=Scout 5=Endurer
static const char* ARCHETYPE_NAME[NUM_ARCHETYPES] = {
  "Guide", "Quartermaster", "Medic", "Mule", "Scout", "Endurer"
};

// Skills[arch][skill]: Navigate,Forage,Scavenge,Treat,Shelter,Endure
static const uint8_t ARCHETYPE_SKILLS[NUM_ARCHETYPES][NUM_SKILLS] = {
  { 2, 1, 0, 0, 1, 1 },   // Guide
  { 0, 2, 1, 1, 1, 0 },   // Quartermaster
  { 0, 0, 1, 2, 0, 2 },   // Medic
  { 0, 1, 2, 0, 1, 1 },   // Mule
  { 2, 1, 1, 0, 0, 1 },   // Scout
  { 1, 0, 0, 1, 2, 2 },   // Endurer
};

// Inventory slot count per archetype
static const uint8_t ARCHETYPE_INV_SLOTS[NUM_ARCHETYPES] = {
  INV_SLOTS_STD,  // Guide
  INV_SLOTS_STD,  // Quartermaster
  INV_SLOTS_STD,  // Medic
  INV_SLOTS_MULE, // Mule — 12 slots
  INV_SLOTS_STD,  // Scout
  INV_SLOTS_STD,  // Endurer
};

// ── Flat-top axial hex directions ─────────────────────────────
// 0:SE  1:NE  2:N  3:NW  4:SW  5:S
static const int8_t DQ[6] = {  1,  1,  0, -1, -1,  0 };
static const int8_t DR[6] = {  0, -1, -1,  0,  1,  1 };

// ── Data structures ────────────────────────────────────────────
struct HexCell {
  uint8_t terrain;
  uint8_t resource;
  uint8_t amount;
  uint8_t respawnTimer;
  uint8_t shelter;      // 0=none, 1=shelter (1 scrap), 2=improved shelter (2 scrap)
  uint8_t footprints;   // bitmask: players who have visited (bit 0-5 for P0-P5)
  uint8_t variant;     // image variant index assigned at map gen
};

struct Player {
  // ── Position / session ────────────────────────────────────────
  int16_t  q, r;
  bool     connected;
  uint32_t wsClientId;
  char     name[12];
  uint32_t lastMoveMs;
  uint32_t connectMs;

  // ── Legacy resource counters (used by collectResource / HUD) ──
  uint8_t  inv[5];      // [water, food, fuel, medicine, scrap]
  uint16_t score;
  uint16_t steps;

  // ── Wayfarer survival tracks ──────────────────────────────────
  uint8_t  ll;          // Life Level  0–6   (start: 6)
  uint8_t  food;        // F track     0–6   (start: 6)
  uint8_t  water;       // W track     0–6   (start: 6)
  uint8_t  fatigue;     // T track     0–8   (start: 0)
  uint8_t  radiation;   // R track     0–10  (start: 0)
  uint8_t  resolve;     // Resolve     0–5   (start: 3, §4.4)

  // ── Archetype & skills ────────────────────────────────────────
  uint8_t  archetype;               // 0–5 (index into ARCHETYPE_*)
  uint8_t  skills[NUM_SKILLS];      // [Nav,For,Scav,Treat,Shel,End] 0–2
  uint8_t  invSlots;                // 8 std, 12 for Mule

  // ── Status & wounds ──────────────────────────────────────────
  uint8_t  statusBits;              // bitmask, see ST_* constants
  uint8_t  wounds[3];               // [minor, major, grievous]

  // ── Wayfarer inventory grid (replaces inv[] over future phases) ─
  uint8_t  invType[INV_SLOTS_MAX];  // item type per slot (0 = empty)
  uint8_t  invQty[INV_SLOTS_MAX];   // quantity per slot

  // ── Legacy stats (kept for debug; may evolve in later phases) ─
  uint8_t  stamina;     // 0–100
  uint8_t  perception;  // 1–5
  uint8_t  strength;    // 1–5

  // ── Skill check / Push state ────────────────────────────────
  uint8_t  chkSk;       // skill index of last check (SK_* constant)
  uint8_t  chkDn;       // DN of last check
  uint8_t  chkBonus;    // item bonus applied to last check (for Push reuse)
  uint8_t  chkPushable; // 1 = last check failed and fatigue < 6 → Push eligible

  // ── Dawn / resource-economy tracking (§4) ───────────────────
  uint8_t  fThreshBelow;  // bitmask: bit0=F crossed below 4, bit1=F crossed below 2
  uint8_t  wThreshBelow;  // bitmask: bit0=W crossed below 5, bit1=W crossed below 3, bit2=W attempted below 1
  int8_t   movesLeft;     // MP budget remaining this day; reset to effectiveMP at each Dawn

  // ── Action tracking (§5 Turn rules) ─────────────────────────
  bool     actUsed;       // one action per day; cleared at dawn
  bool     encPenApplied; // encumbrance -1 MP penalty applied this day; cleared at dawn
  bool     resting;       // player rested this day; cleared at dawn; if all rest → day ends early
  bool     radClean;    // no rad hex entered this day; true → R−1 at next dawn (clean zone)
};

enum EvtType : uint8_t {
  EVT_COLLECT = 1,
  EVT_RESPAWN = 2,
  EVT_MOVE    = 3,
  EVT_JOINED  = 4,
  EVT_LEFT    = 5,
  EVT_NAME    = 6,
  EVT_DAWN    = 7,
  EVT_ACTION  = 8, // action result broadcast
  EVT_DUSK    = 9, // end-of-day radiation Endure check (§6.2)
  EVT_DOWNED  = 10 // player LL reached 0 — reset slot, return to char select
};

struct GameEvent {
  EvtType  type;
  uint8_t  pid;
  int16_t  q, r;
  uint8_t  res, amt;
  // Dawn-specific payload (unused by other event types):
  uint8_t  dawnF, dawnW, dawnLL;
  int8_t   dawnMP, dawnLLDelta;
  uint16_t dawnDay;
  uint8_t  dawnFth, dawnWth;   // threshold bitmasks after upkeep
  uint8_t  dawnFat;            // fatigue after dawn
  // EVT_ACTION payload:
  uint8_t  actType;     // ACT_*
  uint8_t  actOut;      // AO_*
  uint8_t  actNewLL;    // LL value after action
  uint8_t  actNewFat;   // fatigue value after action
  int8_t   actNewMP;    // movesLeft after action
  int8_t   actFoodD;    // food token delta (inv[1])
  int8_t   actWatD;     // water token delta (inv[0])
  int8_t   actLLD;      // LL delta
  int8_t   actResD;     // Resolve delta (REST in good shelter, §7.3)
  int8_t   actScrapD;   // scrap delta: +1 gained (SCAV), -1/-2 spent (SHELTER)
  int16_t  actScoreD;   // score delta awarded for this action
  uint8_t  actCnd;      // TREAT condition target (TC_*, 0 for non-Treat actions)
  uint32_t evWsId;      // WS client ID for targeted sends (EVT_DOWNED)
  uint8_t  actDn;       // check DN (0 = no check)
  int8_t   actTot;      // check total signed (0 = no check)
  // Radiation payload (EVT_MOVE rad gain, EVT_DUSK, EVT_DAWN clean reduction):
  int8_t   radD;        // R change (+1 gain / -1 or -2 reduction)
  uint8_t  radR;        // R value after change
};

// Must be declared here (above function forward-declarations generated by Arduino IDE)
// so that resolveCheck() and broadcastCheck() prototypes compile correctly.
struct CheckResult { int r1, r2, skillVal, mods, total, dn; bool success; };

struct GameState {
  HexCell  map[MAP_ROWS][MAP_COLS];
  Player   players[MAX_PLAYERS];
  uint32_t tickId;
  int      connectedCount;
  SemaphoreHandle_t mutex;

  // ── Threat Clock (shared, all modes) ─────────────────────────
  uint8_t  threatClock;   // 0–20
  uint8_t  tcTriggered;   // bitmask: bit0=T5 bit1=T9 bit2=T13 bit3=T17
  bool     crisisState;   // true once TC reaches 17

  // ── Shared resource pools ─────────────────────────────────────
  uint8_t  sharedFood;    // 0–30 tokens in common pool
  uint8_t  sharedWater;   // 0–30 tokens in common pool

  // ── Day/Night cycle ───────────────────────────────────────────
  uint32_t dayTick;       // ticks elapsed in current day (resets each day)
  uint16_t dayCount;      // total days elapsed
};

static constexpr int  EVT_QUEUE_SIZE = 64;

static GameState      G;
static GameEvent      pendingEvents[EVT_QUEUE_SIZE];
static int            pendingCount  = 0;
static portMUX_TYPE   evtMux        = portMUX_INITIALIZER_UNLOCKED; // guards pendingEvents/pendingCount
static unsigned long  lastStatusMs  = 0;
static unsigned long  lastScreenMs  = 0;
static constexpr uint32_t SCREEN_MS  = 2000;  // screen refresh interval
static UNIHIKER_K10   k10;

// ── LED flash (game events → RGB LEDs) ─────────────────────────────────────
static volatile uint8_t  g_ledR = 0, g_ledG = 0, g_ledB = 0;
static volatile uint32_t g_ledEndMs = 0;

// Called from Core 1 (game loop). Sets LED immediately; loop() turns it off.
void ledFlash(uint8_t r, uint8_t g, uint8_t b) {
  g_ledR = r; g_ledG = g; g_ledB = b;
  g_ledEndMs = millis() + 300;
  k10.rgb->write(-1, r, g, b);
}

AsyncWebServer server(80);
AsyncWebSocket  ws("/ws");

// ── PSRAM web-file cache ────────────────────────────────────────────────────
// All static assets are loaded from SD into PSRAM once at boot.
// HTTP serving reads only from RAM — no SD bus contention, no intermittent 404s.
struct WebFile { const char* url; const char* mime; const char* sdName;
                 uint8_t* buf; size_t len; };
static WebFile WEB_FILES[] = {
  { "/",            "text/html",       "index.html",   nullptr, 0 },
  { "/engine.js",   "text/javascript", "engine.js",    nullptr, 0 },
  { "/game-data.js","text/javascript", "game-data.js", nullptr, 0 },
  { "/style.css",   "text/css",        "style.css",    nullptr, 0 },
  { "/ui.js",       "text/javascript", "ui.js",        nullptr, 0 },
  { "/van-ui.js",   "text/javascript", "van-ui.js",    nullptr, 0 },
  { "/van.js",      "text/javascript", "van.js",       nullptr, 0 },
};
static const int WEB_FILE_COUNT = (int)(sizeof(WEB_FILES)/sizeof(WEB_FILES[0]));

// ── PSRAM image cache (hex tile PNGs, populated alongside web files at boot) ──
struct ImgFile { char name[40]; uint8_t* buf; size_t len; };
static const int MAX_IMG_CACHE = 64;
static ImgFile   imgCache[MAX_IMG_CACHE];
static int       imgCacheCount = 0;

// ── Hex math helpers ───────────────────────────────────────────
static inline int wrapQ(int q) { return ((q % MAP_COLS) + MAP_COLS) % MAP_COLS; }
static inline int wrapR(int r) { return ((r % MAP_ROWS) + MAP_ROWS) % MAP_ROWS; }

static int hexDistWrap(int q1, int r1, int q2, int r2) {
  int best = 0x7FFFFFFF;
  for (int dq = -1; dq <= 1; dq++) {
    for (int dr = -1; dr <= 1; dr++) {
      int aq   = q2 + dq * MAP_COLS - q1;
      int ar   = r2 + dr * MAP_ROWS - r1;
      int dist = (abs(aq) + abs(aq + ar) + abs(ar)) / 2;
      if (dist < best) best = dist;
    }
  }
  return best;
}

// ── Effective vision parameters for a player ──────────────────
// Call while holding G.mutex (reads map terrain at player position).
static void playerVisParams(int pid, int* outVisR, bool* outMaskRes) {
  uint8_t t = G.map[G.players[pid].r][G.players[pid].q].terrain;
  if (t >= NUM_TERRAIN) t = 0;
  int8_t vl = TERRAIN_VIS[t];
  if      (vl <= -3) { *outVisR = 0;            *outMaskRes = false; } // BLIND: standing hex only
  else if (vl == -2) { *outVisR = 1;            *outMaskRes = true;  } // PENALTY: 1 hex, resources masked
  else if (vl == -1) { *outVisR = 2;            *outMaskRes = false; } // LOW: 2 hex radius
  else if (vl ==  0) { *outVisR = VISION_R;     *outMaskRes = false; } // STD
  else if (vl ==  1) { *outVisR = VISION_R + 1; *outMaskRes = false; } // HIGH
  else               { *outVisR = VISION_R + 2; *outMaskRes = false; } // VHIGH (Rolling Hills, Mountain)
}

// ── Slot management ────────────────────────────────────────────
static int findSlot(uint32_t id) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (G.players[i].connected && G.players[i].wsClientId == id) return i;
  return -1;
}
static void enqEvt(GameEvent ev) {
  taskENTER_CRITICAL(&evtMux);
  if (pendingCount < EVT_QUEUE_SIZE) pendingEvents[pendingCount++] = ev;
  taskEXIT_CRITICAL(&evtMux);
}

// ── Terrain resource spawn helper ──────────────────────────────
// Returns the resource type (1-5) that spawns on terrain t, seeded by rnd.
// Returns 0 for terrain that never spawns resources (Nuke Crater).
// Used by both generateMap (Phase 3) and tickGame (respawn).
static uint8_t terrainSpawnRes(uint8_t t, uint32_t rnd) {
  switch (t) {
    case 0: return 1 + rnd % 5;
    case 1: return (rnd & 1) ? 3 : 5;
    case 2: return (rnd & 1) ? 2 : 5;
    case 3: return (rnd & 1) ? 1 : 2;
    case 4: return (rnd & 1) ? 5 : 4;
    case 5: return 1;
    case 6: return 5;
    case 7: return (rnd & 1) ? 3 : 5;
    case 8: return (rnd & 1) ? 5 : 4;
    case 9: return 1 + rnd % 5;
    default: return 0;
  }
}

// ── Map generation ─────────────────────────────────────────────
// Phase 1 : independent base fill using target weights.
// Phase 2 : SMOOTH_PASSES cellular smoothing passes.
//           Each cell re-rolls with weight = baseWeight[t] * (100 + CLUMP[t] * neighbourCount[t]).
//           CLUMP is a 0-100 percentage — 0 = no pull, 100 = strong pull.
//           With clump=80 and all 6 neighbours matching: weight multiplies by 5.8×.
// Phase 3 : resource placement (unchanged).
//
// Target base weights (sum = 100):
//   Open Scrub=41, Ash Dunes=15, Rust Forest=12, Marsh=8,
//   Broken Urban=5, Flooded Ruins=5, Glass Fields=0 (Phase 2.6 only), Rolling Hills=7,
//   Mountain=4, Settlement=1, Nuke Crater=2
//   Note: Glass Fields weight is 0 — they are placed exclusively by the
//   Phase 2.6 pass adjacent to qualifying Broken Urban clusters (≥2 BU).
// ── Image variant counts (filled from SD (SPI) scan before generateMap) ──
static uint8_t terrainVariantCount[NUM_TERRAIN] = {};  // 0 = no variants found

// Rank-linear weighted pick: variant 0 has weight n, variant n-1 has weight 1.
static uint8_t pickVariant(uint8_t n, uint32_t rnd) {
  if (n <= 1) return 0;
  uint32_t total = (uint32_t)n * (n + 1) / 2;
  uint32_t r = rnd % total;
  for (uint8_t i = 0; i < n; i++) {
    uint32_t w = n - i;
    if (r < w) return i;
    r -= w;
  }
  return 0;
}

static const uint8_t T_BASE[NUM_TERRAIN]  = { 41, 15, 12,  8,  5,  5,  0,  7,  4,  1,  2 };

// Clump % per terrain — how strongly each type pulls adjacent cells to match it.
// 0 = purely random, 100 = maximum neighbourhood pull.
static const uint8_t TERRAIN_CLUMP[NUM_TERRAIN] = {
  35,  // 0 Open Scrub   — light background scatter
  65,  // 1 Ash Dunes    — dune belts
  70,  // 2 Rust Forest  — forest patches
  70,  // 3 Marsh        — marshes spread
  60,  // 4 Broken Urban — ruined district clusters
  75,  // 5 Flooded Ruins — ruined district spreads wide
  60,  // 6 Glass Fields — glass plains cluster
  70,  // 7 Rolling Hills — hill ranges
  80,  // 8 Mountain     — mountain ranges
  15,  // 9 Settlement   — rare, isolated
  85,  // 10 Nuke Crater — crater scars cluster
};

static constexpr uint8_t SMOOTH_PASSES = 3;

static void generateMap() {
  // ── Build cumulative thresholds for Phase 1 ─────────────────
  uint8_t T_THRESH[NUM_TERRAIN];
  T_THRESH[0] = T_BASE[0];
  for (int t = 1; t < NUM_TERRAIN; t++)
    T_THRESH[t] = T_THRESH[t - 1] + T_BASE[t];

  // ── Phase 1: independent base fill ───────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      uint8_t  rv   = esp_random() % 100;
      uint8_t  t    = NUM_TERRAIN - 1;
      for (uint8_t i = 0; i < NUM_TERRAIN - 1; i++)
        if (rv < T_THRESH[i]) { t = i; break; }
      cell.terrain      = t;
      cell.resource     = 0;
      cell.amount       = 0;
      cell.respawnTimer = 0;
    }
  }

  // ── Phase 2: clump smoothing passes ──────────────────────────
  // Scratch buffer holds new terrain choices; written all at once per pass
  // to avoid in-pass ordering bias.
  static uint8_t scratch[MAP_ROWS][MAP_COLS];

  for (int pass = 0; pass < SMOOTH_PASSES; pass++) {
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        // Count matching neighbours per terrain type
        uint8_t nCount[NUM_TERRAIN] = {0};
        for (int d = 0; d < 6; d++)
          nCount[G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain]++;

        // Weighted draw: weight[t] = T_BASE[t] * (100 + CLUMP[t] * nCount[t])
        uint32_t weights[NUM_TERRAIN];
        uint32_t total = 0;
        for (int t = 0; t < NUM_TERRAIN; t++) {
          weights[t] = (uint32_t)T_BASE[t] * (100u + (uint32_t)TERRAIN_CLUMP[t] * nCount[t]);
          total     += weights[t];
        }
        uint32_t pick = esp_random() % total;
        uint8_t  chosen = NUM_TERRAIN - 1;
        uint32_t cum    = 0;
        for (int t = 0; t < NUM_TERRAIN - 1; t++) {
          cum += weights[t];
          if (pick < cum) { chosen = (uint8_t)t; break; }
        }
        scratch[r][c] = chosen;
      }
    }
    // Commit pass
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.5: mountain-rolling hills transition pass ────────
  // Any non-mountain cell that neighbours at least one mountain hex is
  // converted to Rolling Hills with a probability that rises with mountain-neighbour
  // count.  This produces a natural rolling hills ring around every mountain cluster.
  //   1 mountain neighbour → 40 %
  //   2 mountain neighbours → 70 %
  //   3+ mountain neighbours → 95 % (capped)
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      scratch[r][c] = G.map[r][c].terrain;

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      if (G.map[r][c].terrain == 8) continue;   // mountains stay mountains
      uint8_t mNeigh = 0;
      for (int d = 0; d < 6; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 8) mNeigh++;
      if (mNeigh == 0) continue;
      uint32_t v    = (uint32_t)mNeigh * 40u;
      uint8_t  prob = (v >= 95u) ? 95u : (uint8_t)v;
      if ((esp_random() % 100) < prob)
        scratch[r][c] = 7;   // → Rolling Hills
    }
  }
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      G.map[r][c].terrain = scratch[r][c];

  // ── Phase 2.6: Glass Fields placement ────────────────────────────────────
  // Two sources:
  //   A) Nuke Crater ring — any non-crater, non-mountain hex adjacent to a
  //      Nuke Crater (10) converts to Glass Fields at 80 %.  No cluster
  //      requirement; a single crater hex is enough (blast radius logic).
  //   B) Broken Urban fringe — hex adjacent to a BU cluster (≥ 2 BU tiles)
  //      converts at 55 %.  A lone BU tile produces no Glass Fields.
  //   • BU (4), Mountain (8), and Nuke Crater (10) hexes are never converted.
  //   • Crater-ring check runs first and short-circuits the BU check.
  // Step 1: strip any stray Glass Fields left by clump smoothing.
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      if (G.map[r][c].terrain == 6) G.map[r][c].terrain = 0; // → Open Scrub

  // Step 2: place Glass Fields.
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      scratch[r][c] = G.map[r][c].terrain;

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t t = G.map[r][c].terrain;
      if (t == 4 || t == 8 || t == 10) continue; // BU / Mountain / Crater: never convert

      // ── A) Nuke Crater ring (80 %) ──────────────────────────────
      bool nearCrater = false;
      for (int d = 0; d < 6 && !nearCrater; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 10) nearCrater = true;
      if (nearCrater) {
        if ((esp_random() % 100) < 80) scratch[r][c] = 6;
        continue; // crater ring takes priority — skip BU check
      }

      // ── B) Broken Urban fringe (55 %, cluster ≥ 2) ──────────────
      bool nearQualBU = false;
      for (int d = 0; d < 6 && !nearQualBU; d++) {
        int nr = wrapR(r + DR[d]);
        int nc = wrapQ(c + DQ[d]);
        if (G.map[nr][nc].terrain != 4) continue;
        uint8_t buOfBU = 0;
        for (int d2 = 0; d2 < 6; d2++)
          if (G.map[wrapR(nr + DR[d2])][wrapQ(nc + DQ[d2])].terrain == 4) buOfBU++;
        if (buOfBU >= 1) nearQualBU = true;
      }
      if (!nearQualBU) continue;
      if ((esp_random() % 100) < 55) scratch[r][c] = 6;
    }
  }
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      G.map[r][c].terrain = scratch[r][c];

  // ── Phase 3: resource placement ───────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      uint8_t  t    = cell.terrain;
      if (t == 10) continue;  // Nuke Crater: impassable, no resources

      uint32_t rnd = esp_random();
      uint8_t  r2  = (rnd >>  8) & 0xFF;
      uint8_t  r3  = (rnd >> 16) & 0xFF;
      uint8_t  r4  = (rnd >> 24) & 0xFF;
      if (r2 % 100 < 35) {
        cell.resource = terrainSpawnRes(t, r3);
        if (cell.resource > 0)
          cell.amount = 1 + r4 % 3;
      }
    }
  }

  // ── Phase 4: assign image variant per cell ───────────────────────
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c2 = 0; c2 < MAP_COLS; c2++) {
      HexCell& cell = G.map[r][c2];
      uint8_t  n    = terrainVariantCount[cell.terrain];
      cell.variant  = (n > 0) ? pickVariant(n, esp_random()) : 0;
    }

  // ── Post-generation map stats ────────────────────────────────
  uint16_t tCount[NUM_TERRAIN] = {0};
  uint16_t rCount[6]           = {0};
  uint16_t totalRes            = 0;

  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      tCount[cell.terrain]++;
      if (cell.resource > 0 && cell.resource < 6) { rCount[cell.resource]++; totalRes++; }
    }

  Serial.printf("[MAP] Generated %dx%d = %d cells | clump passes:%d\n",
    MAP_COLS, MAP_ROWS, MAP_COLS * MAP_ROWS, SMOOTH_PASSES);
  Serial.print ("[MAP] Terrain  : ");
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf("%s:%d ", T_SHORT[t], tCount[t]);
  Serial.println();
  Serial.print ("[MAP] Base wt% : ");
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf("%s:%-2d ", T_SHORT[t], T_BASE[t]);
  Serial.println();
  Serial.print ("[MAP] Clump %  : ");
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf("%s:%-2d ", T_SHORT[t], TERRAIN_CLUMP[t]);
  Serial.println();
  Serial.printf("[MAP] Resources: Water:%d Food:%d Fuel:%d Med:%d Scrap:%d | total:%d (%.1f%%)\n",
    rCount[1], rCount[2], rCount[3], rCount[4], rCount[5],
    totalRes, (float)totalRes * 100.0f / (MAP_COLS * MAP_ROWS));
}

// ── Map encode: fog masked ─────────────────────────────────────
// 2 bytes / cell (4 hex chars):
//   TT = terrain byte (0x00-0x0A) or 0xFF if fogged
//   DD = (resource<<2)|(amount&3)|(shelter<<5), or 0x00 if fogged or maskRes
static const char HEX_CH[] = "0123456789ABCDEF";

static int encodeMapFog(char* buf, int cap, int pq, int pr, int visR, bool maskRes) {
  int pos = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t tt, dd;
      uint8_t vv;
      if (hexDistWrap(pq, pr, c, r) <= visR) {
        HexCell& cell = G.map[r][c];
        tt = cell.terrain;
        // Data byte: bits 0-5 = footprints (which players visited), bit 6 = shelter
        dd = (cell.footprints & 0x3F) | (cell.shelter << 6);
        vv = cell.variant;
      } else {
        tt = 0xFF; dd = 0x00; vv = 0x00;
      }
      if (pos + 6 < cap) {
        buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      }
    }
  }
  buf[pos] = 0;
  return pos;
}

// ── Build full vision-disk message ─────────────────────────────
// Format: {"t":"vis","vr":N,"q":QQ,"r":RR,"cells":"QQRRTTDD..."}
// Each cell: QQ=col, RR=row, TT=terrain, DD=data (2 hex chars each = 8 total)
// Max cells: visR=6 → 127 cells × 8 + header ≈ 1064 chars (within 1100 buf)
// maskRes: when true (PENALTY terrain), sends DD=0 — terrain visible, no resource info
// Returns total message length AND writes cell count to *outCells for debug.
static int buildVisDisk(char* buf, int cap, int pq, int pr, int visR, bool maskRes, int* outCells = nullptr) {
  int pos   = 0;
  int cells = 0;
  pos += snprintf(buf, cap, "{\"t\":\"vis\",\"vr\":%d,\"q\":%d,\"r\":%d,\"cells\":\"", visR, pq, pr);
  for (int dr = -visR; dr <= visR; dr++) {
    for (int dq = -visR; dq <= visR; dq++) {
      int s = -(dq + dr);
      if (abs(dq) + abs(dr) + abs(s) > 2 * visR) continue;
      int      cq   = wrapQ(pq + dq);
      int      cr   = wrapR(pr + dr);
      HexCell& cell = G.map[cr][cq];
      uint8_t  tt   = cell.terrain;
      // Data byte: bits 0-5 = footprints (which players visited), bit 6 = shelter
      uint8_t  dd   = (cell.footprints & 0x3F) | (cell.shelter << 6);
      uint8_t  vv   = cell.variant;
      if (pos + 10 < cap) {
        buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
        buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
        buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
        cells++;
      }
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  if (outCells) *outCells = cells;
  return pos;
}

// ── Build survey-ring vision disk (one hex beyond visR) ────────
// Sends terrain data for all hexes at exactly distance visR+1 around (pq,pr).
// Format matches applyVisDisk on the client: "QQRRTTDD..." (8 hex chars/cell).
// Returns message length written into buf.
static int buildSurveyDisk(char* buf, int cap, int pq, int pr, int visR) {
  int pos = snprintf(buf, cap, "{\"t\":\"ev\",\"k\":\"surv\",\"cells\":\"");
  int ring = visR + 1;
  for (int dr = -ring; dr <= ring; dr++) {
    for (int dq = -ring; dq <= ring; dq++) {
      int s = -(dq + dr);
      if ((abs(dq) + abs(dr) + abs(s)) / 2 != ring) continue;
      int cq = wrapQ(pq + dq);
      int cr = wrapR(pr + dr);
      if (pos + 10 < cap) {
        HexCell& cell = G.map[cr][cq];
        uint8_t dd = (cell.footprints & 0x3F) | (cell.shelter << 6);
        uint8_t vv = cell.variant;
        buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
        buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
        buf[pos++] = HEX_CH[cell.terrain >> 4]; buf[pos++] = HEX_CH[cell.terrain & 0xF];
        // Data byte: bits 0-5 = footprints (which players visited), bit 6 = shelter
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      }
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  return pos;
}


// ── K10 screen: server status dashboard ────────────────────────
// Redraws portrait 240×320 TFT every SCREEN_MS with player stats.
static void drawStatusScreen() {
  struct {
    bool    on;
    char    name[12];
    uint8_t ll, food, water, radiation, statusBits;
    uint8_t archetype;
    int8_t  movesLeft;
  } snap[MAX_PLAYERS];
  uint8_t  snapTC = 0, snapSF = 0, snapSW = 0;
  uint16_t snapDay = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  snapTC  = G.threatClock;
  snapDay = G.dayCount;
  snapSF  = G.sharedFood;
  snapSW  = G.sharedWater;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p          = G.players[i];
    snap[i].on         = p.connected;
    snap[i].ll         = p.ll;
    snap[i].food       = p.food;
    snap[i].water      = p.water;
    snap[i].radiation  = p.radiation;
    snap[i].archetype  = p.archetype;
    snap[i].statusBits = p.statusBits;
    snap[i].movesLeft  = p.movesLeft;
    memcpy(snap[i].name, p.name, 12);
  }
  xSemaphoreGive(G.mutex);

  static const char* ARCH_SHORT[NUM_ARCHETYPES] = {"GUID","QTMR","MEDC","MULE","SCUT","ENDR"};
  // Colors: RGB888 uint32_t  (same palette as web UI)
  static const uint32_t C_HDR  = 0xC89030; // amber  — header title
  static const uint32_t C_INFO = 0x907050; // dim    — info / footer text
  static const uint32_t C_LINE = 0x503830; // dim    — divider lines
  static const uint32_t C_TXT  = 0xC8A878; // amber  — connected player name
  static const uint32_t C_DIM  = 0x403020; // dim    — offline slot
  static const uint32_t C_COND = 0xD07020; // orange — status condition active
  static const uint32_t C_OK   = 0x88C040; // green  — healthy stats
  static const uint32_t C_WARN = 0xC8A030; // yellow — warning stats
  static const uint32_t C_CRIT = 0xC84830; // red    — critical stats

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);

  // ── Header band (y 0–27) ──────────────────────────────────────
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E1000, 0x1E1000, true);
  k10.canvas->canvasText("WASTELAND", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);

  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  // ── Info bar (y 32) ───────────────────────────────────────────
  char buf[40];
  snprintf(buf, sizeof(buf), "Day:%-2u TC:%-2u  F:%-2u W:%-2u  %luk",
           snapDay, snapTC, snapSF, snapSW,
           (unsigned long)(ESP.getFreeHeap() / 1024));
  k10.canvas->canvasText(buf, 2, 32, C_INFO, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 50, 239, 50, C_LINE);

  // ── Player rows (y 54–257, 34 px per slot) ───────────────────
  for (int i = 0; i < MAX_PLAYERS; i++) {
    int y1 = 54 + i * 34;   // name line
    int y2 = y1 + 17;        // stats line

    if (snap[i].on) {
      uint8_t arch = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;

      // Name line — orange if any status condition, amber otherwise
      uint32_t nameCol = (snap[i].statusBits & 0x0F) ? C_COND : C_TXT;
      snprintf(buf, sizeof(buf), "P%d %-4s  %-8.8s", i, ARCH_SHORT[arch], snap[i].name);
      k10.canvas->canvasText(buf, 2, y1, nameCol, Canvas::eCNAndENFont16, 50, false);

      // Stats line — color driven by worst stat
      uint32_t sc;
      if (snap[i].ll <= 2 || snap[i].food <= 1 || snap[i].water <= 1 || snap[i].radiation >= 7)
        sc = C_CRIT;
      else if (snap[i].ll <= 3 || snap[i].food <= 2 || snap[i].water <= 2 ||
               snap[i].radiation >= 4 || (snap[i].statusBits & 0x0F))
        sc = C_WARN;
      else
        sc = C_OK;

      snprintf(buf, sizeof(buf), "   LL:%-2u F:%-2u W:%-2u R:%-2u M:%-2d",
               snap[i].ll, snap[i].food, snap[i].water,
               snap[i].radiation, snap[i].movesLeft);
      k10.canvas->canvasText(buf, 2, y2, sc, Canvas::eCNAndENFont16, 50, false);
    } else {
      snprintf(buf, sizeof(buf), "P%d ----  (offline)", i);
      k10.canvas->canvasText(buf, 2, y1, C_DIM, Canvas::eCNAndENFont16, 50, false);
    }
  }

  // ── Footer ────────────────────────────────────────────────────
  k10.canvas->canvasLine(0, 258, 239, 258, C_LINE);

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "SF:%-2u SW:%-2u  up:%lum%02lus",
           snapSF, snapSW, (unsigned long)(up / 60), (unsigned long)(up % 60));
  k10.canvas->canvasText(buf, 2, 262, C_INFO, Canvas::eCNAndENFont16, 50, false);

  // IP lines — AP (softAP) and STA (connected router), y=282 + y=300
  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  snprintf(buf, sizeof(buf), "AP: %d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  k10.canvas->canvasText(buf, 2, 282, 0x406030, Canvas::eCNAndENFont16, 50, false);
  bool staConn = (staIp[0] != 0);
  uint32_t stColor;
  if (staConn) {
    snprintf(buf, sizeof(buf), "ST: %d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
    stColor = 0x406030;
  } else if (bootWifiPending) {
    snprintf(buf, sizeof(buf), "ST: connecting...");
    stColor = 0x4080C0;
  } else if (savedSsid[0]) {
    snprintf(buf, sizeof(buf), "ST: saved:\"%.16s\"", savedSsid);
    stColor = 0x605030;
  } else {
    snprintf(buf, sizeof(buf), "ST: no creds");
    stColor = 0x302820;
  }
  k10.canvas->canvasText(buf, 2, 300, stColor, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->updateCanvas();
}

// ── Periodic player status table ───────────────────────────────
// Printed from loop() every STATUS_MS. Takes the mutex briefly.
static void printStatus() {
  // Snapshot under mutex
  struct Snap {
    bool     on; int16_t q, r; uint16_t score, steps;
    uint8_t  terrain, stamina;
    char     name[12]; uint32_t connectMs;
    // Wayfarer fields
    uint8_t  ll, food, water, fatigue, radiation, resolve;
    uint8_t  archetype, statusBits;
    uint8_t  skills[NUM_SKILLS];
  } snap[MAX_PLAYERS];
  uint32_t tick = 0;
  uint8_t  snapTC = 0, snapSF = 0, snapSW = 0;
  uint16_t snapDay = 0;
  uint32_t nowMs = millis();

  uint16_t mapRes[6] = {0}; // resource counts by type on map

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  tick    = G.tickId;
  snapTC  = G.threatClock;
  snapDay = G.dayCount;
  snapSF  = G.sharedFood;
  snapSW  = G.sharedWater;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p       = G.players[i];
    snap[i].on      = p.connected;
    snap[i].q       = p.q; snap[i].r = p.r;
    snap[i].score   = p.score; snap[i].steps = p.steps;
    snap[i].terrain = G.map[p.r][p.q].terrain;
    snap[i].stamina = p.stamina;
    snap[i].connectMs = p.connectMs;
    memcpy(snap[i].name, p.name, 12);
    snap[i].ll        = p.ll;
    snap[i].food      = p.food;
    snap[i].water     = p.water;
    snap[i].fatigue   = p.fatigue;
    snap[i].radiation = p.radiation;
    snap[i].resolve   = p.resolve;
    snap[i].archetype = p.archetype;
    snap[i].statusBits = p.statusBits;
    memcpy(snap[i].skills, p.skills, NUM_SKILLS);
  }
  // Count live resources on map
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t res = G.map[r][c].resource;
      if (res > 0 && res < 6) mapRes[res]++;
    }
  xSemaphoreGive(G.mutex);

  uint32_t upSec = nowMs / 1000;
  Serial.printf("\n[STATUS] tick:%lu | heap:%lu | uptime:%lum%02lus | players:%d/%d | Day:%d TC:%d/20 | Shared F:%d W:%d\n",
    (unsigned long)tick,
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)(upSec / 60), (unsigned long)(upSec % 60),
    G.connectedCount, MAX_PLAYERS,
    snapDay, snapTC, snapSF, snapSW);

  // Column header — Wayfarer fields
  Serial.println("[STATUS]  SL Arch         Name       Pos      Terrain    vR  LL  F  W  T   R Res Sb  Steps Score  Sk:Na Fo Sc Tr Sh En");
  Serial.println("[STATUS]  -- ------------ ---------- -------- ---------- -- --- -- -- -- --- -- --- ----- ----- --------------------------------");

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!snap[i].on) continue;
    uint8_t  t      = snap[i].terrain < NUM_TERRAIN ? snap[i].terrain : 0;
    int8_t   vl     = TERRAIN_VIS[t];
    int      effVR  = (vl <= -3) ? 0 : (vl == -2) ? 1 : (vl == -1) ? 2 : (vl == 0) ? VISION_R : (vl == 1) ? VISION_R + 1 : VISION_R + 2;
    uint32_t sessMs = nowMs - snap[i].connectMs;
    uint32_t sessSec = sessMs / 1000;
    uint8_t  arch   = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;

    Serial.printf("[STATUS]  P%d %-12s %-10s (%2d,%2d)  %-10s %2d %3d %2d %2d %2d %3d %2d %3d %5d %5d  %2d %2d %2d %2d %2d %2d  [%lum%02lus]\n",
      i, ARCHETYPE_NAME[arch], snap[i].name, snap[i].q, snap[i].r,
      T_NAME[t], effVR,
      snap[i].ll, snap[i].food, snap[i].water, snap[i].fatigue, snap[i].radiation,
      snap[i].resolve, snap[i].statusBits,
      snap[i].steps, snap[i].score,
      snap[i].skills[SK_NAVIGATE], snap[i].skills[SK_FORAGE],
      snap[i].skills[SK_SCAVENGE], snap[i].skills[SK_TREAT],
      snap[i].skills[SK_SHELTER],  snap[i].skills[SK_ENDURE],
      (unsigned long)(sessSec / 60), (unsigned long)(sessSec % 60));
  }
  if (G.connectedCount == 0)
    Serial.println("[STATUS]  (no players connected)");

  Serial.printf("[STATUS] Map live resources: Water:%d Food:%d Fuel:%d Med:%d Scrap:%d | total:%d\n\n",
    mapRes[1], mapRes[2], mapRes[3], mapRes[4], mapRes[5],
    mapRes[1]+mapRes[2]+mapRes[3]+mapRes[4]+mapRes[5]);
}

// ── Game logic and networking ──────────────────────────────────
#include "game_logic.hpp"
#include "network.hpp"

// ── Game loop task (Core 1) ────────────────────────────────────
static void gameLoopTask(void* param) {
  Serial.printf("[SETUP] Game loop on Core %d\n", xPortGetCoreID());
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TICK_MS));
    tickGame();
    drainEvents();
    broadcastState();
  }
}

// ── Boot splash diagnostic log ─────────────────────────────────────────────
// Scrolling 14-line log; each entry 22 px tall → fills y=4..290 of the 320 px screen.
static char     _sLog[14][30];
static uint32_t _sCol[14];
static uint8_t  _sN = 0;
static void splashAdd(const char* msg, uint32_t col = 0x806040) {
  if (_sN == 14) {
    for (int i = 0; i < 13; i++) { memcpy(_sLog[i], _sLog[i+1], 30); _sCol[i] = _sCol[i+1]; }
    _sN = 13;
  }
  snprintf(_sLog[_sN], 30, "%s", msg);
  _sCol[_sN++] = col;
  k10.canvas->canvasRectangle(0, 0, 240, 320, 0x000000, 0x000000, true);
  for (uint8_t i = 0; i < _sN; i++)
    k10.canvas->canvasText(_sLog[i], 4, 4 + i*22,
                           _sCol[i], Canvas::eCNAndENFont16, 50, false);
  k10.canvas->updateCanvas();
}

// ── Boot-time SD→PSRAM loader ─────────────────────────────────
static void loadWebFilesToRAM() {
  // Single-pass walk of /data. SD.open(fullPath) is unreliable on this config;
  // SD.open("/data") + openNextFile() is the only method confirmed to work.
  Serial.println("[WEB] Loading web assets into PSRAM...");
  File dir = SD.open("/data");
  if (!dir) { Serial.println("[WEB] ERROR: cannot open /data"); return; }
  File f = dir.openNextFile();
  while (f) {
    String fname = String(f.name());
    if (f.isDirectory() && fname.equalsIgnoreCase("img")) {
      // Walk into the img subdirectory by calling openNextFile() on its handle.
      File imgFile = f.openNextFile();
      while (imgFile && imgCacheCount < MAX_IMG_CACHE) {
        if (!imgFile.isDirectory()) {
          size_t sz = imgFile.size();
          uint8_t* buf = (uint8_t*)ps_malloc(sz);
          if (buf) {
            imgFile.read(buf, sz);
            strncpy(imgCache[imgCacheCount].name, imgFile.name(), 39);
            imgCache[imgCacheCount].name[39] = 0;
            imgCache[imgCacheCount].buf = buf;
            imgCache[imgCacheCount].len = sz;
            imgCacheCount++;
            Serial.printf("[WEB]   img/%-22s %u bytes -> PSRAM\n", imgFile.name(), (unsigned)sz);
          } else {
            Serial.printf("[WEB]   img/%s: ps_malloc(%u) FAILED\n", imgFile.name(), (unsigned)sz);
          }
        }
        imgFile.close();
        imgFile = f.openNextFile();
      }
    } else if (!f.isDirectory()) {
      // Regular file — match against the web asset list.
      for (int i = 0; i < WEB_FILE_COUNT; i++) {
        if (fname.equalsIgnoreCase(WEB_FILES[i].sdName)) {
          size_t sz = f.size();
          uint8_t* buf = (uint8_t*)ps_malloc(sz);
          if (buf) {
            size_t got = f.read(buf, sz);
            WEB_FILES[i].buf = buf;
            WEB_FILES[i].len = got;
            Serial.printf("[WEB]   %-16s %u bytes -> PSRAM\n", WEB_FILES[i].sdName, (unsigned)got);
          } else {
            Serial.printf("[WEB]   %-16s ps_malloc(%u) FAILED\n", WEB_FILES[i].sdName, (unsigned)sz);
          }
          break;
        }
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  for (int i = 0; i < WEB_FILE_COUNT; i++)
    if (!WEB_FILES[i].buf)
      Serial.printf("[WEB]   WARNING: %s not cached!\n", WEB_FILES[i].sdName);
  Serial.printf("[WEB]   %d image(s) cached.\n", imgCacheCount);
  Serial.printf("[WEB] Done. Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

// ── Setup ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // USB-CDC: wait up to 3 s for the host serial monitor to connect.
  { unsigned long t0 = millis(); while (!Serial && millis()-t0 < 3000) delay(10); }
  delay(200);
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║   ESP32-S3  WASTELAND HEX CRAWL  v3.0       ║");
  Serial.println("║   11-Terrain | FOW | 6-Wayfarer Co-op        ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.printf("[SETUP] Free heap at boot: %lu bytes\n", (unsigned long)ESP.getFreeHeap());

  // ── K10 screen init ───────────────────────────────────────────
  k10.begin();
  k10.initScreen(2, 0);   // portrait 240×320, no camera
  k10.creatCanvas();
  k10.setScreenBackground(0x000000);
  splashAdd("Display OK", 0x406030);
  splashAdd("Hold [A] now = USB drive", 0x203060);
  Serial.printf("[SETUP] Map: %dx%d=%d cells | VISION_R:%d | MOVE_CD:%lums | RESPAWN:%ds\n",
    MAP_COLS, MAP_ROWS, MAP_COLS * MAP_ROWS,
    VISION_R, (unsigned long)MOVE_CD_MS, (int)RESPAWN_TICKS / 10);

  // Print terrain system reference table
  Serial.println("[SETUP] Terrain MC/SV/VIS table:");
  Serial.println("[SETUP]   #  Name       MC  SV  Vis     Resources");
  for (int t = 0; t < NUM_TERRAIN; t++) {
    const char* visTxt = (TERRAIN_VIS[t] >= 2)   ? "+2 VHIGH "
                       : (TERRAIN_VIS[t] == 1)  ? "+1 HIGH  "
                       : (TERRAIN_VIS[t] == 0)  ? "standard "
                       : (TERRAIN_VIS[t] == -1) ? "LOW r2   "
                       : (TERRAIN_VIS[t] == -2) ? "PENALTY  "
                       :                          "BLIND    ";
    char mcBuf[4] = {(char)('0'+TERRAIN_MC[t]), 0};
    const char* mcStr = (TERRAIN_MC[t] == 255) ? "∞" : mcBuf;
    Serial.printf("[SETUP]   %2d %-10s %-3s %-3d %-9s ",
      t, T_NAME[t], mcStr,
      TERRAIN_SV[t], visTxt);
    // Print which resources can appear here
    switch (t) {
      case 0:  Serial.print("Any"); break;
      case 1:  Serial.print("Fuel/Scrap"); break;
      case 2:  Serial.print("Food/Scrap"); break;
      case 3:  Serial.print("Water/Food"); break;
      case 4:  Serial.print("Scrap/Med"); break;
      case 5:  Serial.print("Water"); break;
      case 6:  Serial.print("Scrap"); break;
      case 7:  Serial.print("Fuel/Scrap"); break;
      case 8:  Serial.print("Scrap/Med"); break;
      case 9:  Serial.print("Any"); break;
      case 10: Serial.print("None (impassable)"); break;
    }
    Serial.println();
  }

  G.mutex = xSemaphoreCreateMutex();
  if (!G.mutex) { Serial.println("[ERROR] Mutex creation failed!"); for (;;) delay(1000); }

  G.tickId = 0; G.connectedCount = 0;
  // Wayfarer shared state
  G.threatClock = 0; G.tcTriggered = 0; G.crisisState = false;
  G.sharedFood  = 30; G.sharedWater = 30;
  G.dayTick = 0; G.dayCount = 0;

  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    p.connected = false; p.wsClientId = 0;
    p.q = p.r = 0; p.score = 0; p.lastMoveMs = 0; p.steps = 0;
    p.connectMs = 0; p.stamina = 100; p.perception = 2; p.strength = 2;
    memset(p.inv, 0, sizeof(p.inv));
    // Wayfarer defaults
    p.archetype    = (uint8_t)i;
    p.ll = 6; p.food = 6; p.water = 6;
    p.fatigue = 0; p.radiation = 0; p.resolve = 3;  // §4.4: start at 3, max 5
    p.statusBits = 0; p.invSlots = ARCHETYPE_INV_SLOTS[i];
    memcpy(p.skills, ARCHETYPE_SKILLS[i], NUM_SKILLS);
    memset(p.wounds,  0, sizeof(p.wounds));
    memset(p.invType, 0, sizeof(p.invType));
    memset(p.invQty,  0, sizeof(p.invQty));
    p.chkSk = 0; p.chkDn = 7; p.chkBonus = 0; p.chkPushable = 0;
    p.fThreshBelow = 0; p.wThreshBelow = 0;
    p.actUsed = false; p.encPenApplied = false; p.radClean = true;
    p.movesLeft = (int8_t)effectiveMP(i);  // consistent with handleConnect
    snprintf(p.name, sizeof(p.name), "%s%d", ARCHETYPE_NAME[i], i);
  }

  // Print archetype assignment table
  Serial.println("[SETUP] Wayfarer archetype assignments:");
  Serial.println("[SETUP]   Slot  Archetype      InvSlots  Skills[Nav For Scav Treat Shel End]");
  for (int i = 0; i < NUM_ARCHETYPES; i++) {
    const uint8_t* sk = ARCHETYPE_SKILLS[i];
    Serial.printf("[SETUP]   P%d    %-13s  %-8d  [%d   %d   %d    %d     %d    %d]\n",
      i, ARCHETYPE_NAME[i], ARCHETYPE_INV_SLOTS[i],
      sk[0], sk[1], sk[2], sk[3], sk[4], sk[5]);
  }

  splashAdd("Mounting SD card...");
  if (!SD.begin()) {
    splashAdd("SD FAIL - insert card!", 0xC04020);
    Serial.println("[ERROR] SD card mount failed! Insert card and reboot.");
    for (;;) delay(1000);
  }
  { // SD stats
    uint64_t tot = SD.totalBytes() / (1024*1024);
    uint64_t use = SD.usedBytes()  / (1024*1024);
    char sdBuf[30]; snprintf(sdBuf, 30, "SD %uMB/%uMB used", (unsigned)tot, (unsigned)use);
    splashAdd(sdBuf, 0x406030);
    Serial.println("[SETUP] SD card mounted OK");
    Serial.printf("[SETUP] SD: total=%lluMB used=%lluMB\n", tot, use);
  }
  if (!SD.exists("/data/index.html")) {
    splashAdd("WARN: no index.html!", 0xC89030);
    Serial.println("[WARN]  /index.html missing on SD card root!");
  } else {
    splashAdd("index.html OK", 0x60A040);
  }
  loadWebFilesToRAM();
  { char hb[36]; snprintf(hb, 36, "Web: %ukB in PSRAM", (unsigned)(ESP.getFreeHeap()/1024));
    splashAdd(hb, 0x406030); }

  // ── USB drive mode (hold Button A during SD mount splash) ───────────────────
  if (k10.buttonA && k10.buttonA->isPressed()) {
    splashAdd("USB DRIVE MODE!", 0x0070C0);
    delay(200);  // debounce
    enterUSBDriveMode(k10);
    // never returns
  }

  // Derive variant counts from imgCache (built at boot — no SD.exists() needed).
  Serial.print("[SETUP] Variants:");
  for (int i = 0; i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    for (int t = 0; t < NUM_TERRAIN; t++) {
      String pfx = String("hex") + TERRAIN_IMG_NAME[t];
      if (fname.startsWith(pfx) && fname.endsWith(".png")) {
        String numStr = fname.substring(pfx.length(), fname.length() - 4);
        if (numStr.length() > 0) {
          bool dig = true;
          for (int k = 0; k < (int)numStr.length(); k++)
            if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
          if (dig) {
            int idx = numStr.toInt();
            if (idx + 1 > (int)terrainVariantCount[t])
              terrainVariantCount[t] = (uint8_t)min(idx + 1, 255);
          }
        }
        break;
      }
    }
  }
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf(" %s:%d", T_SHORT[t], terrainVariantCount[t]);
  Serial.println();

  splashAdd("Generating map...");
  Serial.println("[SETUP] Generating map...");
  generateMap();  // logs [MAP] stats
  { char mb[30]; snprintf(mb, 30, "Map %dx%d ready", MAP_COLS, MAP_ROWS);
    splashAdd(mb, 0x60A040); }

  splashAdd("Starting WiFi...");
  // ESP32 stores WiFi credentials internally whenever WiFi.begin(ssid,pass) is
  // called. On boot, read that stored config via esp_wifi_get_config to decide
  // whether to attempt a STA join. No Preferences/NVS code needed.
  WiFi.mode(WIFI_AP_STA);   // AP+STA always; AP keeps running during STA join
  WiFi.softAP(AP_SSID);
  // Check for stored STA credentials in ESP32's internal WiFi config.
  { wifi_config_t staCfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &staCfg) == ESP_OK && staCfg.sta.ssid[0]) {
      strlcpy(savedSsid, (char*)staCfg.sta.ssid, sizeof(savedSsid));
      // Note: password not copied here for safety; WiFi.begin() uses it internally
      splashAdd("Joining saved WiFi...", 0x4080C0);
      Serial.printf("[WIFI] Stored creds found for \"%s\" — connecting\n", savedSsid);
      WiFi.begin();   // uses ESP32's stored credentials, no ssid/pass args needed
      bootWifiPending = true;
      bootWifiStartMs = millis();
    } else {
      Serial.println("[WIFI] No stored STA creds — AP only");
    }
  }
  { char wb[30]; snprintf(wb, 30, "AP: %s", WiFi.softAPIP().toString().c_str());
    splashAdd(wb, 0x60A040); }
  Serial.printf("[SETUP] WiFi AP: \"%s\"  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent); ws.enable(true); server.addHandler(&ws);

  // Static web assets — served from PSRAM (loaded at boot, no SD access at runtime).
  for (int i = 0; i < WEB_FILE_COUNT; i++) {
    server.on(WEB_FILES[i].url, HTTP_GET, [i](AsyncWebServerRequest* req) {
      if (WEB_FILES[i].buf) {
        req->send(200, WEB_FILES[i].mime, WEB_FILES[i].buf, WEB_FILES[i].len);
      } else {
        req->send(503, "text/plain", "Web file not cached - check SD & reboot");
      }
    });
  }
  // Suppress favicon noise.
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) { req->send(204); });
  // Captive-portal redirects.
  auto toGame = [](AsyncWebServerRequest* req) { req->redirect("/"); };
  server.on("/generate_204",              HTTP_GET, toGame);
  server.on("/gen_204",                   HTTP_GET, toGame);
  server.on("/hotspot-detect.html",       HTTP_GET, toGame);
  server.on("/library/test/success.html", HTTP_GET, toGame);
  server.on("/ncsi.txt",                  HTTP_GET, toGame);
  server.on("/connecttest.txt",           HTTP_GET, toGame);
  server.on("/fwlink",                    HTTP_GET, toGame);
  // ── /state — full game-state JSON endpoint (server-side view) ────
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    String j;
    j.reserve(6144);
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {

      // ── Global fields ──────────────────────────────────────────────
      j += "{\"day\":";         j += G.dayCount;
      j += ",\"dayTick\":";     j += G.dayTick;
      j += ",\"tickId\":";      j += G.tickId;
      j += ",\"tc\":";          j += G.threatClock;
      j += ",\"crisis\":";      j += G.crisisState  ? "true" : "false";
      j += ",\"connected\":";   j += G.connectedCount;
      j += ",\"sharedFood\":";  j += G.sharedFood;
      j += ",\"sharedWater\":"; j += G.sharedWater;
      j += ",\"evtQueue\":";    j += pendingCount;   // approximate; guarded by evtMux not mutex

      // ── Map summary ────────────────────────────────────────────────
      int shelters = 0, impShelters = 0;
      int resCnt[6] = {0,0,0,0,0,0};
      int terrCnt[NUM_TERRAIN] = {};
      for (int row = 0; row < MAP_ROWS; row++) {
        for (int col = 0; col < MAP_COLS; col++) {
          const HexCell& c = G.map[row][col];
          if (c.shelter == 1) shelters++;
          else if (c.shelter == 2) impShelters++;
          if (c.resource > 0 && c.resource < 6) resCnt[c.resource] += c.amount;
          if (c.terrain < NUM_TERRAIN) terrCnt[c.terrain]++;
        }
      }
      j += ",\"map\":{\"cells\":"; j += (MAP_ROWS * MAP_COLS);
      j += ",\"shelters\":";    j += shelters;
      j += ",\"impShelters\":"; j += impShelters;
      j += ",\"res\":{\"water\":";  j += resCnt[1];
      j += ",\"food\":";  j += resCnt[2];
      j += ",\"fuel\":";  j += resCnt[3];
      j += ",\"med\":";   j += resCnt[4];
      j += ",\"scrap\":"; j += resCnt[5];
      j += "},\"terrain\":[";
      for (int t = 0; t < NUM_TERRAIN; t++) {
        if (t) j += ",";
        j += "{\"id\":"; j += t;
        j += ",\"name\":\""; j += T_SHORT[t]; j += "\"";
        j += ",\"count\":"; j += terrCnt[t];
        j += "}";
      }
      j += "]}";

      // ── Players ────────────────────────────────────────────────────
      j += ",\"players\":[";
      for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player& p = G.players[i];
        if (i) j += ",";
        j += "{";
        // Identity
        j += "\"pid\":";          j += i;
        j += ",\"conn\":";        j += p.connected ? "true" : "false";
        j += ",\"wsClientId\":";  j += p.wsClientId;
        j += ",\"connectMs\":";   j += p.connectMs;
        j += ",\"lastMoveMs\":";  j += p.lastMoveMs;
        j += ",\"name\":\"";     j += p.name; j += "\"";
        j += ",\"arch\":";        j += p.archetype;
        j += ",\"archName\":\""; j += (p.archetype < NUM_ARCHETYPES ? ARCHETYPE_NAME[p.archetype] : "?"); j += "\"";
        j += ",\"invSlots\":";    j += p.invSlots;
        // Position
        j += ",\"q\":";           j += p.q;
        j += ",\"r\":";           j += p.r;
        // Survival tracks
        j += ",\"ll\":";          j += p.ll;
        j += ",\"food\":";        j += p.food;
        j += ",\"water\":";       j += p.water;
        j += ",\"fatigue\":";     j += p.fatigue;
        j += ",\"rad\":";         j += p.radiation;
        j += ",\"resolve\":";     j += p.resolve;
        // Status & turn state
        j += ",\"sb\":";          j += p.statusBits;
        j += ",\"mp\":";          j += p.movesLeft;
        j += ",\"actUsed\":";     j += p.actUsed       ? "true" : "false";
        j += ",\"encPenApplied\":"; j += p.encPenApplied ? "true" : "false";
        j += ",\"resting\":";     j += p.resting       ? "true" : "false";
        j += ",\"radClean\":";    j += p.radClean      ? "true" : "false";
        j += ",\"fThreshBelow\":"; j += p.fThreshBelow;
        j += ",\"wThreshBelow\":"; j += p.wThreshBelow;
        // Wounds [minor, major, grievous]
        j += ",\"wounds\":[";
        j += p.wounds[0]; j += ","; j += p.wounds[1]; j += ","; j += p.wounds[2];
        j += "]";
        // Skills [Navigate,Forage,Scavenge,Treat,Shelter,Endure]
        j += ",\"skills\":[";
        for (int s = 0; s < NUM_SKILLS; s++) { if (s) j += ","; j += p.skills[s]; }
        j += "]";
        // Inventory quick-totals [water,food,fuel,med,scrap]
        j += ",\"inv\":[";
        for (int s = 0; s < 5; s++) { if (s) j += ","; j += p.inv[s]; }
        j += "]";
        // Full inventory grid
        j += ",\"invType\":[";
        for (int s = 0; s < INV_SLOTS_MAX; s++) { if (s) j += ","; j += p.invType[s]; }
        j += "]";
        j += ",\"invQty\":[";
        for (int s = 0; s < INV_SLOTS_MAX; s++) { if (s) j += ","; j += p.invQty[s]; }
        j += "]";
        // Skill check / push state
        j += ",\"chkSk\":";       j += p.chkSk;
        j += ",\"chkDn\":";       j += p.chkDn;
        j += ",\"chkBonus\":";    j += p.chkBonus;
        j += ",\"chkPushable\":"; j += p.chkPushable;
        // Legacy stats
        j += ",\"stamina\":";     j += p.stamina;
        j += ",\"perception\":";  j += p.perception;
        j += ",\"strength\":";    j += p.strength;
        // Score / steps
        j += ",\"score\":";       j += p.score;
        j += ",\"steps\":";       j += p.steps;
        j += "}";
      }
      j += "]";
      j += "}";  // close root object
      xSemaphoreGive(G.mutex);
    } else {
      j = "{\"error\":\"mutex timeout\"}";
    }
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", j);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
  });


  // ── /view — visible cells for a player (AI agent / accessibility endpoint) ──
  server.on("/view", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("pid")) {
      req->send(400, "application/json", "{\"error\":\"missing pid\"}");
      return;
    }
    int pid = req->getParam("pid")->value().toInt();
    if (pid < 0 || pid >= MAX_PLAYERS) {
      req->send(400, "application/json", "{\"error\":\"invalid pid\"}");
      return;
    }
    static const char* TNAME_FULL[NUM_TERRAIN] = {
      "Open Scrub","Ash Dunes","Rust Forest","Marsh","Broken Urban",
      "Flooded Ruins","Glass Fields","Rolling Hills","Mountain","Settlement","Nuke Crater"
    };
    static const char* RES_NAME[6] = {"none","water","food","fuel","medicine","scrap"};
    String j;
    j.reserve(4096);
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      const Player& p = G.players[pid];
      if (!p.connected) {
        xSemaphoreGive(G.mutex);
        req->send(400, "application/json", "{\"error\":\"player not connected\"}");
        return;
      }
      int visR; bool mr;
      playerVisParams(pid, &visR, &mr);
      j += "{\"pid\":"; j += pid;
      j += ",\"name\":\""; j += p.name; j += "\"";
      j += ",\"q\":"; j += p.q;
      j += ",\"r\":"; j += p.r;
      j += ",\"visR\":"; j += visR;
      j += ",\"cells\":[";
      bool first = true;
      for (int dr = -visR; dr <= visR; dr++) {
        for (int dq = -visR; dq <= visR; dq++) {
          int s = -(dq + dr);
          if (abs(dq) + abs(dr) + abs(s) > 2 * visR) continue;
          int cq = wrapQ(p.q + dq);
          int cr = wrapR(p.r + dr);
          const HexCell& cell = G.map[cr][cq];
          uint8_t tt = cell.terrain < NUM_TERRAIN ? cell.terrain : 0;
          uint8_t res = cell.resource < 6 ? cell.resource : 0;
          if (!first) j += ",";
          first = false;
          j += "{\"q\":"; j += cq;
          j += ",\"r\":"; j += cr;
          j += ",\"dq\":"; j += dq;
          j += ",\"dr\":"; j += dr;
          j += ",\"terrain\":"; j += tt;
          j += ",\"terrainName\":\""; j += TNAME_FULL[tt]; j += "\"";
          j += ",\"shelter\":"; j += cell.shelter;
          j += ",\"resource\":"; j += res;
          j += ",\"resourceName\":\""; j += RES_NAME[res]; j += "\"";
          j += ",\"amount\":"; j += cell.amount;
          j += ",\"footprints\":"; j += cell.footprints;
          j += "}";
        }
      }
      j += "]}";
      xSemaphoreGive(G.mutex);
    } else {
      j = "{\"error\":\"mutex timeout\"}";
    }
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", j);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
  });

  // All text assets (JS/CSS/HTML) are in PSRAM above.
  // Only /img/*.png requests reach here; serve them from the imgCache in PSRAM.
  server.onNotFound([](AsyncWebServerRequest* req) {
    String url = req->url();
    if (url.startsWith("/img/")) {
      String filename = url.substring(5);  // strip "/img/"
      for (int i = 0; i < imgCacheCount; i++) {
        if (filename.equalsIgnoreCase(imgCache[i].name)) {
          AsyncWebServerResponse* resp = req->beginResponse(
              200, "image/png", imgCache[i].buf, imgCache[i].len);
          resp->addHeader("Cache-Control", "public, max-age=86400");
          req->send(resp);
          return;
        }
      }
    }
    req->send(404, "text/plain", "Not found: " + url);
  });
  server.begin();
  { char hb[30]; snprintf(hb, 30, "Heap: %ukB free", (unsigned)(ESP.getFreeHeap()/1024));
    splashAdd("HTTP+WS ready", 0x60A040);
    splashAdd(hb, 0x406030); }
  Serial.println("[SETUP] HTTP+WS server started");
  Serial.printf("[SETUP] Free heap after init: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.println("[SETUP] ─── Ready. Connect to \"WASTELAND\" → http://192.168.4.1 ───\n");

  xTaskCreatePinnedToCore(gameLoopTask, "GameLoop", 8192, NULL, 2, NULL, 1);
}

void loop() {
  ws.cleanupClients(MAX_PLAYERS);
  unsigned long now = millis();

  // Monitor async boot-time STA connect.
  if (bootWifiPending) {
    wl_status_t wst = WiFi.status();
    if (wst == WL_CONNECTED) {
      bootWifiPending = false;
      // Populate savedSsid/savedPass from WiFi library (now connected, SSID() works).
      strlcpy(savedSsid, WiFi.SSID().c_str(), sizeof(savedSsid));
      strlcpy(savedPass, WiFi.psk().c_str(),  sizeof(savedPass));
      char msg[48]; snprintf(msg, sizeof(msg), "WiFi: %s", WiFi.localIP().toString().c_str());
      splashAdd(msg, 0x40C080);
      Serial.printf("[WIFI] Boot connect OK — STA IP: %s\n", WiFi.localIP().toString().c_str());
      char buf[88];
      int blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}",
        WiFi.localIP().toString().c_str());
      ws.textAll(buf, (size_t)blen);
    } else if (wst == WL_CONNECT_FAILED || wst == WL_NO_SSID_AVAIL ||
               now - bootWifiStartMs > BOOT_WIFI_TIMEOUT) {
      bootWifiPending = false;
      savedSsid[0] = '\0';  // clear so handleConnect doesn't suppress client retry
      Serial.printf("[WIFI] Boot connect failed (status=%d)\n", (int)wst);
      // Stay in AP_STA mode — AP is still running; STA just didn't connect.
    }
  }

  if (now - lastScreenMs >= SCREEN_MS) {
    lastScreenMs = now;
    drawStatusScreen();
  }

  // Turn off LED flash when duration expires
  if (g_ledEndMs && now >= g_ledEndMs) {
    k10.rgb->write(-1, 0, 0, 0);
    g_ledEndMs = 0;
  }

  if (now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    printStatus();  // full player table + map resource summary

    uint32_t heap = ESP.getFreeHeap();
    if (heap < 100000)
      Serial.printf("[HEAP]  *** WARNING: low heap %lu bytes — consider reboot ***\n",
        (unsigned long)heap);
  }
  delay(100);
}
