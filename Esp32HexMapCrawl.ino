/*
 * ESP32-S3 Hex Map Crawl - 6 Player Co-op | Survivor System
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
 * Encoding : 3 bytes / cell (6 hex chars):
 *              TT = terrain (0x00-0x0B) or 0xFF (fog)
 *              DD = bits 0-5: footprint bitmask; bit 6: shelter level
 *              VV = high nibble: resource type (0-5); low nibble: terrain variant (0-15)
 *
 * Vis-disk : {"t":"vis","vr":N,"q":QQ,"r":RR,"cells":"QQRRTTDDVV..."}
 *              QQ=col, RR=row, TT=terrain, DD=data, VV=variant (2 hex chars each = 10 total/cell)
 *
 * 12 Terrain types (index 0-11):
 *   0 Open Scrub    MC=1  SV=0  vis=STANDARD
 *   1 Ash Dunes     MC=2  SV=0  vis=STANDARD
 *   2 Rust Forest   MC=2  SV=1  vis=BLIND (resources masked)
 *   3 Marsh         MC=3  SV=0  vis=STANDARD
 *   4 Broken Urban  MC=2  SV=1  vis=PENALTY
 *   5 Flooded Ruins MC=3  SV=2  vis=STANDARD
 *   6 Glass Fields  MC=3  SV=0  vis=HIGH
 *   7 Rolling Hills MC=2  SV=1  vis=VHIGH
 *   8 Mountain      MC=4  SV=2  vis=STANDARD
 *   9 Settlement    MC=1  SV=3  vis=STANDARD
 *  10 Nuke Crater   MC=∞  SV=0  vis=STANDARD (impassable)
 *  11 River Channel MC=2  SV=0  vis=STANDARD (path-placed only)
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
 *                           setup(), loop()
 *   hex-map.hpp           — hex math, slot mgmt, map gen, vision encoding
 *   ui-display.hpp        — K10 screens, LED, audio
 *   boot-assets.hpp       — splash, asset loading, item registry, printStatus
 *   game-server.hpp       — game loop task, WiFi/HTTP/WS setup helpers
 *   survival_skills.hpp   — skill checks, resource economy tracks
 *   inventory_items.hpp   — item effects, equipment, trade
 *   survival_state.hpp    — day cycle, movement, resource collection
 *   actions_game_loop.hpp — action handlers (forage, scav, shelter, rest, …)
 *   network-persistence.hpp — SD save/load
 *   network-sync.hpp      — state serialization and broadcast
 *   network-events.hpp    — event queue drain → JSON → clients
 *   network-handlers.hpp  — WebSocket session lifecycle and message dispatch
 */

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <FS.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include "unihiker_k10.h"
#pragma GCC diagnostic pop
#include "usb_drive.h"

static const char* AP_SSID = "WASTELAND";

// ── WiFi STA connection (background task) ──────────────────────
struct WifiTaskCtx { char ssid[33]; char pass[65]; };
static volatile bool wifiConnecting = false;
static char savedSsid[33] = {0};
static char savedPass[65] = {0};
static bool     bootWifiPending = false;
static uint32_t bootWifiStartMs = 0;
static constexpr uint32_t BOOT_WIFI_TIMEOUT = 12000;

// ── Constants ──────────────────────────────────────────────────
static constexpr int      MAP_COLS      = 25;
static constexpr int      MAP_ROWS      = 19;
static constexpr int      MAX_PLAYERS   = 6;
static constexpr int      VISION_R      = 3;
static constexpr int      NUM_TERRAIN   = 12;
static constexpr uint32_t TICK_MS       = 100;
static constexpr uint8_t  RESPAWN_TICKS = 200;
static constexpr uint32_t MOVE_CD_MS    = 220;
static constexpr uint32_t STATUS_MS     = 30000;

// ── Survivor system constants ───────────────────────────────────
static constexpr int      NUM_ARCHETYPES  = 6;
static constexpr int      NUM_SKILLS      = 6;
static constexpr int      INV_SLOTS_STD   = 8;
static constexpr int      INV_SLOTS_MULE  = 12;
static constexpr int      INV_SLOTS_MAX   = 12;
static constexpr uint32_t DAY_TICKS       = 3000;
static constexpr uint8_t  TC_THRESHOLD_A  = 5;
static constexpr uint8_t  TC_THRESHOLD_B  = 9;
static constexpr uint8_t  TC_THRESHOLD_C  = 13;
static constexpr uint8_t  TC_THRESHOLD_D  = 17;

// Skill indices
static constexpr int SK_NAVIGATE = 0;
static constexpr int SK_FORAGE   = 1;
static constexpr int SK_SCAVENGE = 2;
static constexpr int SK_TREAT    = 3;
static constexpr int SK_SHELTER  = 4;
static constexpr int SK_ENDURE   = 5;

// ── Action system constants ─────────────────────────────────────
static constexpr uint8_t ACT_FORAGE  = 0;
static constexpr uint8_t ACT_WATER   = 1;
static constexpr uint8_t ACT_SCAV    = 3;
static constexpr uint8_t ACT_SHELTER = 4;
static constexpr uint8_t ACT_SURVEY  = 6;
static constexpr uint8_t ACT_REST    = 7;

static constexpr uint8_t AO_BLOCKED = 0;
static constexpr uint8_t AO_SUCCESS = 1;
static constexpr uint8_t AO_PARTIAL = 2;
static constexpr uint8_t AO_FAIL    = 3;

static const uint8_t TERRAIN_FORAGE_DN[NUM_TERRAIN]  = { 7,0,6,8,0,0,0,0,0,0,0, 0 };
static const uint8_t TERRAIN_SALVAGE_DN[NUM_TERRAIN] = { 0,0,0,0,6,7,8,0,0,0,0, 0 };
static const bool    TERRAIN_HAS_WATER[NUM_TERRAIN]  = { 0,0,0,1,0,1,0,0,0,0,0, 0 };
static const bool    TERRAIN_IS_RUINS[NUM_TERRAIN]   = { 0,0,0,0,1,0,0,0,0,0,0, 0 };
static const bool    TERRAIN_IS_RAD[NUM_TERRAIN]     = { 0,1,0,0,0,0,1,0,0,0,0, 0 };

// ── Weather system constants ──────────────────────────────────────────────────
static constexpr uint8_t  WEATHER_CLEAR = 0, WEATHER_RAIN = 1,
                           WEATHER_STORM = 2, WEATHER_CHEM  = 3;
// Weather counter decrements only once every WEATHER_TICK_DIVIDER game ticks.
// At TICK_MS=100 (10 ticks/sec), divider=50 → 5 sec/weather-tick:
//   Clear 60-100 weather-ticks = 5-8.3 min | Chem 15-30 = 1.25-2.5 min
static constexpr uint32_t WEATHER_TICK_DIVIDER = 50;
static const int8_t  WEATHER_VIS_PENALTY[4]  = { 0, 1, 3, 5 };
static const uint8_t WEATHER_MOVE_PENALTY[4] = { 0, 1, 2, 3 };
static const uint16_t WEATHER_DUR_MIN[4]     = { 180, 30, 20, 15 };
static const uint16_t WEATHER_DUR_MAX[4]     = { 300, 50, 40, 30 };
// Terrain intensity [phase][terrain idx 0-11] — MUST match JS copy exactly
// Terrains: 0=OpenScrub 1=AshDunes 2=RustForest 3=Marsh 4=BrokenUrban
//           5=FloodRuins 6=GlassFields 7=RollingHills 8=Mountain
//           9=Settlement 10=NukeCrater(impassable) 11=RiverChannel(impassable)
static const float WEATHER_INTENSITY[4][12] = {
  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.5f, 0.4f, 0.6f, 0.8f, 0.4f, 0.9f, 0.5f, 0.6f, 0.7f, 0.1f, 0.0f, 0.0f },
  { 0.7f, 0.6f, 0.7f, 0.9f, 0.5f, 1.0f, 0.8f, 0.9f, 1.0f, 0.2f, 0.0f, 0.0f },
  { 0.95f,0.85f,0.75f,0.90f,0.6f,0.95f,0.90f,0.90f,0.85f, 0.1f, 0.0f, 0.0f },
};

// Status condition bitmask positions
static constexpr uint8_t ST_WOUNDED  = (1 << 0);
static constexpr uint8_t ST_RADSICK  = (1 << 1);
static constexpr uint8_t ST_BLEEDING = (1 << 2);
static constexpr uint8_t ST_FEVERED  = (1 << 3);
static constexpr uint8_t ST_DOWNED   = (1 << 4);
static constexpr uint8_t ST_STABLE   = (1 << 5);
static constexpr uint8_t ST_PANICKED = (1 << 6);
static constexpr uint8_t ST_STINK   = (1 << 7);   // Applied by methane/sewage encounters; blocks stealth for 2 turns

// ── Item system ─────────────────────────────────────────────────
static constexpr uint8_t  MAX_ITEMS  = 128;
static constexpr uint8_t  MAX_GROUND = 32;

static constexpr uint8_t TERR_BIT_NUKECRATR = (1 << 4);
static constexpr uint8_t TERR_BIT_RIVER     = (1 << 3);
static constexpr uint8_t TERR_PASS_RIVER    = (1 << 0);
static constexpr uint8_t TERR_PASS_NUKE     = (1 << 1);

enum StatIdx : uint8_t {
  STAT_LL      = 0,
  STAT_FOOD    = 1,
  STAT_WATER   = 2,
  STAT_FATIGUE = 3,
  STAT_RAD     = 4,
  STAT_RESOLVE = 5,
  STAT_MP      = 6,
  STAT_SLOTS   = 7,
  STAT_COUNT   = 8
};

enum ItemCategory : uint8_t {
  ITEM_CONSUMABLE = 0,
  ITEM_EQUIPMENT  = 1,
  ITEM_MATERIAL   = 2,
  ITEM_KEY        = 3
};

enum EquipSlot : uint8_t {
  EQUIP_NONE    = 0,
  EQUIP_HEAD    = 1,
  EQUIP_BODY    = 2,
  EQUIP_HAND    = 3,
  EQUIP_FEET    = 4,
  EQUIP_VEHICLE = 5
};
static constexpr uint8_t EQUIP_SLOTS = 5;

enum EffectId : uint8_t {
  EFX_NONE          = 0,
  EFX_UNLOCK_ACTION = 1,
  EFX_REVEAL_FOG    = 2,
  EFX_NARRATIVE     = 3,
  EFX_THREAT_MOD    = 4,
  EFX_CURE_STATUS   = 5
};

struct ItemDef {
  uint8_t      id;
  char         name[16];
  ItemCategory category;
  EquipSlot    equipSlot;
  uint8_t      maxStack;
  bool         tradeable;
  uint8_t      tradeValue;
  int8_t       statMods[STAT_COUNT];
  EffectId     effectId;
  uint8_t      effectParam;
  EffectId     effectId2;
  uint8_t      effectParam2;
  uint8_t      opCost[5];
  uint8_t      passTerrainBits;
};

struct GroundItem {
  int16_t  q, r;
  uint8_t  itemType;
  uint8_t  qty;
};

static const uint8_t TERRAIN_MC[NUM_TERRAIN]  = { 1, 2, 2, 3, 2, 3, 3, 2, 4, 1, 255, 255 };
static const int8_t  TERRAIN_VIS[NUM_TERRAIN] = { 0, 0, -3, 0, -2, 0, 1, 2, 2, -1, 0, -3 };
static const uint8_t TERRAIN_SV[NUM_TERRAIN]  = { 0, 0,  1, 0,  1,  2, 0, 1, 2, 3, 0, 0 };

// ── Debug label tables ─────────────────────────────────────────
static const char* T_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes ", "RustForst", "Marsh    ",
  "BrknUrban", "FloodRuin", "GlassFlds", "RolngHill",
  "Mountain ", "Settlment", "NukeCratr", "RiverChnl"
};
static const char* T_SHORT[NUM_TERRAIN] = {
  "Scrub","Dunes","Forst","Marsh","Urban","Ruins","Glass","Hills","Mtn  ","Settl","Nukr ","River"
};
static const char* TERRAIN_IMG_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes", "RustForest", "Marsh",
  "BrokenUrban", "FloodedDistrict", "GlassFields",
  "Ridge", "Mountain", "Settlement", "NukeCrater", "RiverChannel"
};
static const char* VIS_LABEL[6] = { "BLIND", "PENLT", "LOW  ", "STD  ", "HIGH ", "VHIGH" };
static const char* RES_NAME[6]  = { "None","Water","Food ","Fuel ","Med  ","Scrap" };
static const char* DIR_NAME[6]  = { "SE","NE","N ","NW","SW","S " };
static const char* SKILL_NAME[NUM_SKILLS] = {
  "Navigate","Forage  ","Scavenge","Treat   ","Shelter ","Endure  "
};

// ── Survivor archetype tables ───────────────────────────────────
static const char* ARCHETYPE_NAME[NUM_ARCHETYPES] = {
  "Guide", "Quartermaster", "Medic", "Mule", "Scout", "Endurer"
};
static const uint8_t ARCHETYPE_SKILLS[NUM_ARCHETYPES][NUM_SKILLS] = {
  { 2, 1, 0, 0, 1, 1 },
  { 0, 2, 1, 1, 1, 0 },
  { 0, 0, 1, 2, 0, 2 },
  { 0, 1, 2, 0, 1, 1 },
  { 2, 1, 1, 0, 0, 1 },
  { 1, 0, 0, 1, 2, 2 },
};
static const uint8_t ARCHETYPE_INV_SLOTS[NUM_ARCHETYPES] = {
  INV_SLOTS_STD, INV_SLOTS_STD, INV_SLOTS_STD,
  INV_SLOTS_MULE, INV_SLOTS_STD, INV_SLOTS_STD,
};

// ── Flat-top axial hex directions ─────────────────────────────
static const int8_t DQ[6] = {  1,  1,  0, -1, -1,  0 };
static const int8_t DR[6] = {  0, -1, -1,  0,  1,  1 };

// ── Data structures ────────────────────────────────────────────
struct HexCell {
  uint8_t terrain;
  uint8_t resource;
  uint8_t amount;
  uint8_t respawnTimer;
  uint8_t shelter;
  uint8_t footprints;
  uint8_t variant;
  uint8_t poi;  // 0 = none/looted, non-zero = has encounter
};

struct Player {
  int16_t  q, r;
  bool     connected;
  uint32_t wsClientId;
  char     name[16];
  uint32_t lastMoveMs;
  uint32_t connectMs;

  uint8_t  inv[5];
  uint16_t score;
  uint16_t steps;
  uint16_t encCount;

  uint8_t  ll;
  uint8_t  food;
  uint8_t  water;
  uint8_t  fatigue;
  uint8_t  radiation;
  uint8_t  resolve;

  uint8_t  archetype;
  uint8_t  skills[NUM_SKILLS];
  uint8_t  invSlots;

  uint8_t  statusBits;
  uint8_t  wounds[3];

  uint8_t  invType[INV_SLOTS_MAX];
  uint8_t  invQty[INV_SLOTS_MAX];

  uint8_t  equip[EQUIP_SLOTS];

  uint8_t  fThreshBelow;
  uint8_t  wThreshBelow;
  int8_t   movesLeft;

  bool     actUsed;
  bool     encPenApplied;
  bool     resting;
  bool     radClean;

  uint8_t  surveyedMap[60];
};

// ── Custom tone sequences ────────────────────────────────────────────────────
struct ToneStep { int freq; int beat; };

static const ToneStep SEQ_DAMAGE[]     = {{1300, 900},  {0,0}};
static const ToneStep SEQ_SCORE_UP[]   = {{220, 2000}, {277, 2000}, {330, 2000}, {0,0}};
static const ToneStep SEQ_SCORE_DOWN[] = {{330, 2000}, {277, 2000}, {220, 2000}, {0,0}};
static const ToneStep SEQ_ALERT[]      = {{196, 2000}, {220, 2000}, {247, 2000}, {0,0}};
static const ToneStep SEQ_CRISIS[]     = {{174, 2000}, {196, 2000}, {174, 2000}, {0,0}};
static const ToneStep SEQ_RESOURCE[]   = {{185, 3000}, {185, 3000}, {185, 3000}, {0,0}};
static const ToneStep SEQ_JOIN[]       = {{220, 2000}, {262, 2000}, {330, 2000}, {0,0}};

static const ToneStep SEQ_GEIGER[] = {
  {800, 400}, {-430, 0},
  {800, 400}, {-370, 0},
  {800, 400}, {-310, 0},
  {800, 400}, {-250, 0},
  {800, 400}, {-190, 0},
  {800, 400},
  {0, 0}
};

enum EvtType : uint8_t {
  EVT_COLLECT      = 1,
  EVT_RESPAWN      = 2,
  EVT_MOVE         = 3,
  EVT_JOINED       = 4,
  EVT_LEFT         = 5,
  EVT_NAME         = 6,
  EVT_DAWN         = 7,
  EVT_ACTION       = 8,
  EVT_DUSK         = 9,
  EVT_DOWNED       = 10,
  EVT_REGEN        = 11,
  EVT_TRADE_OFFER  = 12,
  EVT_TRADE_RESULT = 13,
  EVT_ENC_START    = 14,
  EVT_ENC_ASSIST   = 15,
  EVT_ENC_RESULT   = 16,
  EVT_ENC_BANK     = 17,
  EVT_ENC_END      = 18,
  EVT_WEATHER      = 19
};

struct GameEvent {
  EvtType  type;
  uint8_t  pid;
  int16_t  q, r;
  uint8_t  res, amt;
  uint8_t  dawnF, dawnW, dawnLL;
  int8_t   dawnMP, dawnLLDelta;
  uint16_t dawnDay;
  uint8_t  dawnFth, dawnWth;
  uint8_t  dawnFat;
  int8_t   dawnExpD;
  uint8_t  actType;
  uint8_t  actOut;
  uint8_t  actNewLL;
  uint8_t  actNewFat;
  int8_t   actNewMP;
  int8_t   actFoodD;
  int8_t   actWatD;
  int8_t   actLLD;
  int8_t   actResD;
  int8_t   actScrapD;
  int16_t  actScoreD;
  uint8_t  actCnd;
  uint32_t evWsId;
  uint8_t  actDn;
  int8_t   actTot;
  int8_t   radD;
  uint8_t  radR;
  int8_t   exploD;
  int8_t   moveMP;
  uint8_t  tradeTo;
  uint8_t  tradeGive[5];
  uint8_t  tradeWant[5];
  uint8_t  tradeResult;
  // ── Encounter fields (active when type is EVT_ENC_*) ───────────
  uint8_t  encOut;       // 0=fail/reason-code, 1=success
  uint8_t  encSkill;
  uint8_t  encDN;
  int8_t   encTotal;
  uint8_t  encLoot[5];
  int8_t   encPenLL, encPenFat, encPenRad;
  uint8_t  encStatus;
  uint8_t  encEnds;
  uint8_t  encAssistRes;  // resource type for assist event
  int8_t   encRiskRed;    // risk reduction for assist event
  uint8_t  encItemType;   // typed item dropped (loot table roll)
  uint8_t  encItemQty;
  uint8_t  encDrains[MAX_PLAYERS]; // per-ally resource drain on failure (auto-assist)
};

static constexpr uint32_t TRADE_EXPIRE_MS = 30000;

struct TradeOffer {
  bool     active;
  uint8_t  fromPid;
  uint8_t  toPid;
  uint8_t  give[5];
  uint8_t  want[5];
  uint8_t  giveSlots[4];
  uint8_t  wantItemType[4];
  uint8_t  wantItemQty[4];
  uint32_t expiresMs;
};

// ── Encounter engine structs ───────────────────────────────────
#define ENC_MAX_ITEMS 3
struct ActiveEncounter {
  uint8_t  active;          // bit 0 = in encounter, bit 7 = reachedTerminal
  uint8_t  encIdx;          // encounter file index selected at enc_start
  uint8_t  hexQ, hexR;
  uint8_t  pendingLoot[5];  // unbanked resource loot [Water,Food,Fuel,Med,Scrap]
  uint8_t  pendingItemType[ENC_MAX_ITEMS];
  uint8_t  pendingItemQty[ENC_MAX_ITEMS];
  uint8_t  pendingItemCount;
  int8_t   assistRisk;  // accumulated risk reduction from assists (≥ -12)
  uint8_t  assistUsed;  // bitmask: which ally pids assisted this node
};
static ActiveEncounter encounters[MAX_PLAYERS];

struct EncPoolInfo {
  uint8_t count;
  char    path[12];  // e.g. "urban", "marsh"
};
static EncPoolInfo encPools[10];  // indexed by terrain type 0-9

// POI encounter probability removed — encounters are now pre-placed
// at map generation time (one hex per encounter ID, guaranteed).
// See hex-map.hpp Phase 5.

// ── Loot table cache (parsed from /encounters/loot_tables.json at boot) ───────
struct LootEntry { uint8_t item; uint8_t qtyMin; uint8_t qtyMax; uint8_t weight; };
struct LootTable  { char name[20]; LootEntry entries[8]; uint8_t count; };
static LootTable  lootTables[20];
static uint8_t    lootTableCount = 0;

struct CheckResult { int r1, r2, skillVal, mods, total, dn; bool success; };

struct GameState {
  HexCell  map[MAP_ROWS][MAP_COLS];
  Player   players[MAX_PLAYERS];
  uint32_t tickId;
  int      connectedCount;
  SemaphoreHandle_t mutex;

  uint8_t  threatClock;
  bool     crisisState;

  uint32_t dayTick;
  uint16_t dayCount;

  uint8_t  weatherPhase;    // 0=clear 1=rain 2=storm 3=chem
  uint16_t weatherCounter;  // ticks remaining in current phase
  uint16_t badWeatherTicks; // consecutive weather-ticks in non-CLEAR phases
};

static constexpr int  EVT_QUEUE_SIZE = 64;

static GameState      G;

// ── SD Save / Load constants + structs ────────────────────────────────────────
static constexpr uint32_t SAVE_MAGIC   = 0xDEADC0DEul;
static constexpr uint8_t  SAVE_VERSION = 7;
static const char         SAVE_DIR[]   = "/save";
static const char         SAVE_MAP_F[] = "/save/map.bin";
static const char         SAVE_PLY_F[] = "/save/players.bin";

struct __attribute__((packed)) SaveHeader {
  uint32_t magic;
  uint8_t  version;
  uint16_t dayCount;
  uint8_t  threatClock;
  uint8_t  weatherPhase;    // was: pad
  uint16_t weatherCounter;  // new (+2 bytes); total header: 11 bytes
};

struct __attribute__((packed)) SavePlayer {
  char     name[16];
  uint8_t  archetype;
  uint8_t  skills[6];
  int16_t  q, r;
  uint8_t  ll, food, water, fatigue, radiation, resolve;
  uint8_t  inv[5];
  uint8_t  invType[12];
  uint8_t  invQty[12];
  uint8_t  equip[EQUIP_SLOTS];
  uint8_t  invSlots;
  uint8_t  wounds[3];
  uint8_t  statusBits;
  uint16_t score;
  uint16_t steps;
  uint8_t  used;
  uint8_t  surveyedMap[60];
};

struct __attribute__((packed)) SaveGroundItem {
  int16_t q, r;
  uint8_t itemType;
  uint8_t qty;
};

static GameEvent      pendingEvents[EVT_QUEUE_SIZE];
static int            pendingCount  = 0;
static portMUX_TYPE   evtMux        = portMUX_INITIALIZER_UNLOCKED;
static TradeOffer     tradeOffers[MAX_PLAYERS];

// ── Item registry ─────────────────────────────────────────────
static ItemDef  itemRegistry[MAX_ITEMS];
static uint8_t  itemCount = 0;

// ── Ground items ──────────────────────────────────────────────
static GroundItem groundItems[MAX_GROUND];
static unsigned long  lastStatusMs  = 0;
static unsigned long  lastScreenMs  = 0;
static constexpr uint32_t SCREEN_MS  = 10000;
static UNIHIKER_K10   k10;
static Music          k10Music;

// ── K10 multi-screen state ─────────────────────────────────────────────────
#define K10_LOG_SIZE 15
struct K10LogEntry { char text[34]; uint32_t ms; };
static K10LogEntry  k10Log[K10_LOG_SIZE];
static uint8_t      k10LogHead  = 0;
static uint8_t      k10LogCount = 0;
static portMUX_TYPE k10LogMux   = portMUX_INITIALIZER_UNLOCKED;

static uint8_t  k10Screen     = 0;
static uint8_t  k10ScreenLast = 255;
static bool     k10BtnBLast   = false;
static volatile bool k10Dirty = true;  // set whenever game state changes

static uint32_t k10TeamScore   = 0;
static uint32_t k10LedPulse    = 0;
static uint8_t  k10PulseR = 0, k10PulseG = 0, k10PulseB = 0;
static uint8_t  k10PrevTCLevel = 0;

static uint8_t  s_audioVol   = 5;
static uint8_t  s_ledBright  = 4;

static void loadK10Prefs() {
  Preferences p; p.begin("k10", true);
  s_audioVol  = p.getUChar("vol",    5);
  s_ledBright = p.getUChar("bright", 4);
  p.end();
}
static void saveK10Prefs() {
  Preferences p; p.begin("k10", false);
  p.putUChar("vol",    s_audioVol);
  p.putUChar("bright", s_ledBright);
  p.end();
}

// ── LED flash state ─────────────────────────────────────────────────────────
static volatile uint8_t  g_ledR = 0, g_ledG = 0, g_ledB = 0;
static volatile uint32_t g_ledEndMs = 0;

AsyncWebServer server(80);
AsyncWebSocket  ws("/ws");

// ── PSRAM web-file cache ────────────────────────────────────────────────────
struct WebFile { const char* url; const char* mime; const char* sdName;
                 uint8_t* buf; size_t len; char etag[26]; };
static WebFile WEB_FILES[] = {
  { "/",                           "text/html",       "index.html",                nullptr, 0 },
  { "/engine.js",                  "text/javascript", "engine.js",                 nullptr, 0 },
  { "/ash-particle-system.js",     "text/javascript", "ash-particle-system.js",    nullptr, 0 },
  { "/weather-particle-system.js", "text/javascript", "weather-particle-system.js",nullptr, 0 },
  { "/game-data.js",               "text/javascript", "game-data.js",              nullptr, 0 },
  { "/game-config.js",             "text/javascript", "game-config.js",            nullptr, 0 },
  { "/state-manager.js",           "text/javascript", "state-manager.js",          nullptr, 0 },
  { "/animation-manager.js",       "text/javascript", "animation-manager.js",      nullptr, 0 },
  { "/event-handlers.js",          "text/javascript", "event-handlers.js",         nullptr, 0 },
  { "/style.css",                  "text/css",        "style.css",                 nullptr, 0 },
  { "/ui.js",                      "text/javascript", "ui.js",                     nullptr, 0 },
  { "/van-ui.js",                  "text/javascript", "van-ui.js",                 nullptr, 0 },
  { "/van.js",                     "text/javascript", "van.js",                    nullptr, 0 },
  { "/sw.js",                      "text/javascript", "sw.js",                     nullptr, 0 },
};
static const int WEB_FILE_COUNT = (int)(sizeof(WEB_FILES)/sizeof(WEB_FILES[0]));

// ── PSRAM image cache ──────────────────────────────────────────────────────
struct ImgFile { char name[40]; uint8_t* buf; size_t len; char etag[26]; };
static const int MAX_IMG_CACHE = 100;
static ImgFile   imgCache[MAX_IMG_CACHE];
static int       imgCacheCount = 0;
static uint32_t  g_bootNonce   = 0;

// ── Split module includes ──────────────────────────────────────
// Order matters: each file depends on declarations above it.
#include "hex-map.hpp"       // hex math, slot mgmt, map gen, vision encoding
#include "boot-assets.hpp"   // splash, asset loading, item registry, printStatus
#include "ui-display.hpp"    // K10 screens, LED, audio

// Gameplay chain (depend on hex-map + ui-display)
#include "survival_skills.hpp"
#include "inventory_items.hpp"     // depends on survival_skills
#include "survival_state.hpp"      // depends on survival_skills + inventory_items
#include "actions_game_loop.hpp"   // depends on all 3 above

// Network layer
#include "network-persistence.hpp"
#include "network-sync.hpp"
#include "network-events.hpp"
#include "network-handlers.hpp"

// Server orchestration — last: calls drainEvents(), broadcastState()
#include "game-server.hpp"

// ── Setup ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  { unsigned long t0 = millis(); while (!Serial && millis()-t0 < 3000) delay(10); }
  delay(200);
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║   ESP32-S3  WASTELAND HEX CRAWL  v3.0       ║");
  Serial.println("║   11-Terrain | FOW | 6-Survivor Co-op        ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.printf("[SETUP] Free heap at boot: %lu bytes\n", (unsigned long)ESP.getFreeHeap());

  // ── K10 screen init ───────────────────────────────────────────
  k10.begin();
  loadK10Prefs();
  k10.initScreen(2, 0);
  k10.creatCanvas();
  k10.setScreenBackground(0x000000);
  splashAdd("Display OK", 0x406030);
  splashAdd("Hold [A] now = USB drive", 0x203060);
  Serial.printf("[SETUP] Map: %dx%d=%d cells | VISION_R:%d | MOVE_CD:%lums | RESPAWN:%ds\n",
    MAP_COLS, MAP_ROWS, MAP_COLS * MAP_ROWS,
    VISION_R, (unsigned long)MOVE_CD_MS, (int)RESPAWN_TICKS / 10);

  // Print terrain reference table
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
      t, T_NAME[t], mcStr, TERRAIN_SV[t], visTxt);
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

  // ── Mutex + game state init ───────────────────────────────────
  G.mutex = xSemaphoreCreateMutex();
  if (!G.mutex) { Serial.println("[ERROR] Mutex creation failed!"); for (;;) delay(1000); }

  G.tickId = 0; G.connectedCount = 0;
  G.threatClock = 0; G.crisisState = false;
  G.dayTick = 0; G.dayCount = 0;
  G.weatherPhase = WEATHER_CLEAR; G.weatherCounter = 80; G.badWeatherTicks = 0;
  memset(groundItems, 0, sizeof(groundItems));

  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    p.connected = false; p.wsClientId = 0;
    p.q = p.r = 0; p.score = 0; p.lastMoveMs = 0; p.steps = 0;
    p.connectMs = 0;
    memset(p.inv, 0, sizeof(p.inv));
    p.inv[0] = 2; p.inv[1] = 1;
    if (i == 1) { p.inv[1] = 2; }
    if (i == 2) { p.inv[3] = 2; }
    if (i == 3) { p.inv[1] = 2; p.inv[3] = 1; p.inv[4] = 1; }
    memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
    p.archetype    = (uint8_t)i;
    p.ll = 7; p.food = 6; p.water = 6;
    p.fatigue = 0; p.radiation = 0; p.resolve = 3;
    p.statusBits = 0; p.invSlots = ARCHETYPE_INV_SLOTS[i];
    memcpy(p.skills, ARCHETYPE_SKILLS[i], NUM_SKILLS);
    memset(p.wounds,  0, sizeof(p.wounds));
    memset(p.invType, 0, sizeof(p.invType));
    memset(p.invQty,  0, sizeof(p.invQty));
    memset(p.equip,   0, sizeof(p.equip));
    p.fThreshBelow = 0; p.wThreshBelow = 0;
    p.encPenApplied = false; p.radClean = true;
    p.movesLeft = (int8_t)effectiveMP(i);
    snprintf(p.name, sizeof(p.name), "%s%d", ARCHETYPE_NAME[i], i);
  }

  Serial.println("[SETUP] Survivor archetype assignments:");
  Serial.println("[SETUP]   Slot  Archetype      InvSlots  Skills[Nav For Scav Treat Shel End]");
  for (int i = 0; i < NUM_ARCHETYPES; i++) {
    const uint8_t* sk = ARCHETYPE_SKILLS[i];
    Serial.printf("[SETUP]   P%d    %-13s  %-8d  [%d   %d   %d    %d     %d    %d]\n",
      i, ARCHETYPE_NAME[i], ARCHETYPE_INV_SLOTS[i],
      sk[0], sk[1], sk[2], sk[3], sk[4], sk[5]);
  }

  // ── SD card mount ─────────────────────────────────────────────
  splashAdd("Mounting SD card...");
  if (!SD.begin()) {
    splashAdd("SD FAIL - insert card!", 0xC04020);
    Serial.println("[ERROR] SD card mount failed! Insert card and reboot.");
    for (;;) delay(1000);
  }
  {
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
  g_bootNonce = esp_random();
  for (int i = 0; i < WEB_FILE_COUNT; i++)
    snprintf(WEB_FILES[i].etag, sizeof(WEB_FILES[i].etag), "\"%08lx-%zx\"", (unsigned long)g_bootNonce, WEB_FILES[i].len);
  for (int i = 0; i < imgCacheCount; i++)
    snprintf(imgCache[i].etag, sizeof(imgCache[i].etag), "\"%08lx-%zx\"", (unsigned long)g_bootNonce, imgCache[i].len);
  { char hb[36]; snprintf(hb, 36, "Web: %ukB in PSRAM", (unsigned)(ESP.getFreeHeap()/1024));
    splashAdd(hb, 0x406030); }

  // ── USB drive mode (hold Button A during SD mount splash) ───────────────────
  if (k10.buttonA && k10.buttonA->isPressed()) {
    splashAdd("USB DRIVE MODE!", 0x0070C0);
    delay(200);
    enterUSBDriveMode(k10);
    // never returns
  }

  setupVariantCounts();

  initEffectTable();
  splashAdd("Loading items...");
  loadItemRegistry();
  { char ib[30]; snprintf(ib, 30, "Items: %d loaded", (int)itemCount);
    splashAdd(ib, 0x60A040); }

  splashAdd("Loading encounters...");
  loadEncounterIndex();
  loadLootTables();
  { char eb[30]; snprintf(eb, 30, "Enc: %d tables", (int)lootTableCount);
    splashAdd(eb, 0x60A040); }

  splashAdd("Generating map...");
  Serial.println("[SETUP] Loading or generating map...");
  if (!tryLoadSave()) {
    generateMap();
    Serial.println("[SAVE] No save found - fresh map generated");
  }
  { char mb[30]; snprintf(mb, 30, "Map %dx%d ready", MAP_COLS, MAP_ROWS);
    splashAdd(mb, 0x60A040); }

  setupWiFiAndServer();

  xTaskCreatePinnedToCore(gameLoopTask, "GameLoop", 8192, NULL, 2, NULL, 1);
}

void loop() {
  ws.cleanupClients(MAX_PLAYERS);
  unsigned long now = millis();

  // Monitor async boot-time STA connect
  if (bootWifiPending) {
    wl_status_t wst = WiFi.status();
    if (wst == WL_CONNECTED) {
      bootWifiPending = false;
      strlcpy(savedSsid, WiFi.SSID().c_str(), sizeof(savedSsid));
      strlcpy(savedPass, WiFi.psk().c_str(),  sizeof(savedPass));
      Serial.printf("[WIFI] Boot connect OK — STA IP: %s\n", WiFi.localIP().toString().c_str());
      k10ScreenLast = 255;  // force title redraw after WiFi splash would have disrupted it
      char buf[88];
      int blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}",
        WiFi.localIP().toString().c_str());
      ws.textAll(buf, (size_t)blen);
    } else if (wst == WL_CONNECT_FAILED || wst == WL_NO_SSID_AVAIL ||
               now - bootWifiStartMs > BOOT_WIFI_TIMEOUT) {
      bootWifiPending = false;
      savedSsid[0] = '\0';
      Serial.printf("[WIFI] Boot connect failed (status=%d)\n", (int)wst);
    }
  }

  checkGestureSwitch();
  checkScoreAudio();

  bool screenChanged = (k10Screen != k10ScreenLast);
  k10ScreenLast = k10Screen;
  if (screenChanged || k10Dirty || (k10Screen != 0 && now - lastScreenMs >= SCREEN_MS)) {
    lastScreenMs = now;
    k10Dirty = false;
    switch (k10Screen) {
      case 0:  if (screenChanged) drawTitleScreen(); break;
      case 2:  drawEventLogScreen();   break;
      case 3:  drawResourceScreen();   break;
      case 4:  drawEncounterScreen();  break;
      case 5:  drawMapScreen();        break;
      default: drawPlayerScreen();     break;  // case 1
    }
  }

  if (g_ledEndMs && now >= g_ledEndMs) {
    g_ledEndMs = 0;
    updateLEDs();
  } else if (!g_ledEndMs) {
    updateLEDs();
  }

  if (now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    printStatus();

    uint32_t heap = ESP.getFreeHeap();
    if (heap < 100000)
      Serial.printf("[HEAP]  *** WARNING: low heap %lu bytes — consider reboot ***\n",
        (unsigned long)heap);
  }
  delay(100);
}
