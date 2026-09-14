/*
 * ESP32-S3 Hex Map Crawl - 6 Player Co-op | Survivor System
 * Post-Apocalyptic World | No Combat | Server-Authoritative Fog of War
 *
 * Core 0 (PRO_CPU): WiFi, HTTP/WebSocket I/O, SD (SPI)
 * Core 1 (APP_CPU): Game loop — resource respawn, state broadcast
 *
 * Libraries: ESPAsyncWebServer, AsyncTCP
 *
 * Hex Grid : Flat-top axial coordinates, 75×57 toroidal wraparound
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
 *              TT = terrain (0x00-0x0B) or 0xFF (fog); bit 6 (0x40) set = improved shelter
 *              DD = bits 0-5: footprint bitmask; bit 6: has shelter; bit 7: has POI
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
 *   boot-assets.hpp       — splash, asset loading, item registry, loot tables
 *   game-server.hpp       — game loop task, WiFi/HTTP/WS setup helpers
 *   survival_skills.hpp   — skill checks, resource economy tracks
 *   inventory_items.hpp   — item effects, equipment, trade, survivor init
 *   survival_state.hpp    — day cycle, movement, resource collection
 *   actions_game_loop.hpp — action handlers (forage, scav, shelter, rest, …)
 *   encounter_engine.hpp  — server-side encounter JSON resolution
 *   network-persistence.hpp — SD save/load
 *   network-sync.hpp      — state serialization and broadcast
 *   network-events.hpp    — event queue drain → JSON → clients
 *   network-session.hpp      — WebSocket connect/disconnect, WiFi join task
 *   network-msg-player.hpp   — pick, move, name, wifi, check, regen, erase, act, settings
 *   network-msg-trade.hpp    — trade offer/accept/decline
 *   network-msg-items.hpp    — use/equip/unequip/drop/pickup item
 *   network-msg-encounter.hpp — enc_start, enc_choice, enc_bank, enc_abort
 *   network-handlers.hpp     — message dispatch and WS event dispatcher
 */

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <FS.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include "logging.hpp"

// ── PSRAM placement helpers ─────────────────────────────────────────────────
// Internal DRAM is the scarce resource on this board. ~210 KB of static .bss
// left only ~45 KB of heap for the Wi-Fi driver + LWIP, and under a burst of
// HTTP traffic their buffer allocations failed and the whole network stack
// wedged until power-cycle (2026-09-12, see docs/dev-loop.md "Diagnosing HTTP
// stalls"). Anything large therefore lives in the 8 MB PSRAM:
//   PSRAM_STATIC(T, name, [dims...])  function-local static array in PSRAM
//       that keeps full array semantics — sizeof(name), name[i][j], decay.
//   allocPsramGlobals()               the big globals (G.map, W_hex, caches,
//       event queue, item registry); called first thing in setup().
static void* psramStaticAlloc(size_t bytes) {
  void* p = ps_calloc(1, bytes);
  if (!p) {
    p = calloc(1, bytes);
    Log.error("PSRAM alloc %u B FAILED — fell back to internal heap", (unsigned)bytes);
  }
  return p;
}
#define PSRAM_STATIC(T, name, dims) \
  static T (&name)dims = *(T(*)dims)psramStaticAlloc(sizeof(T dims))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include "unihiker_k10.h"
#pragma GCC diagnostic pop
// Tell LovyanGFX that LVGL is already included (UNIHIKER K10 pulls in LVGL v8).
// Without this, LovyanGFX includes its own LVGL v9 stubs and the two sets of
// type definitions conflict fatally.
#define M5GFX_USING_REAL_LVGL 1
#include <LovyanGFX.hpp>
#include "lgfx_config.h"

static const char* AP_SSID = "WASTELAND";

// ── WiFi STA connection (background task) ──────────────────────
struct WifiTaskCtx { char ssid[33]; char pass[65]; };
static volatile bool wifiConnecting = false;
static char savedSsid[33] = {0};
static char savedPass[65] = {0};
static bool     bootWifiPending = false;
static uint32_t bootWifiStartMs = 0;
static constexpr uint32_t BOOT_WIFI_TIMEOUT = 12000;
static bool rtcSynced = false;

static bool checkRtcReady() {
  if (rtcSynced) return true;
  struct tm ti;
  if (getLocalTime(&ti, 0) && ti.tm_year > 100) rtcSynced = true;
  return rtcSynced;
}

// ── Constants ──────────────────────────────────────────────────
static constexpr int      MAP_COLS      = 75;
static constexpr int      MAP_ROWS      = 57;
static constexpr int      SURVEYED_BYTES = (MAP_ROWS * MAP_COLS + 7) / 8;
static constexpr int      MAX_PLAYERS   = 6;
static constexpr int      VISION_R      = 3;
static constexpr int      NUM_TERRAIN   = 12;
static constexpr uint32_t TICK_MS       = 100;
static constexpr uint8_t  RESPAWN_TICKS = 200;
static constexpr uint32_t MOVE_CD_MS    = 220;
static constexpr uint32_t STATUS_MS     = 30000;

// ── Survivor system constants ───────────────────────────────────
static constexpr int      NUM_ARCHETYPES  = 6;
static constexpr int      NUM_SKILLS      = 5;
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
static constexpr int SK_SHELTER  = 3;
static constexpr int SK_ENDURE   = 4;

// ── Wound system ────────────────────────────────────────────────
// Player.wounds[] is indexed by these; each tier caps at WOUND_MAX_EACH.
// Minor wounds penalise Endure checks only; major wounds penalise every
// skill check and also cost 1 MP each per day (see effectiveMP).
static constexpr int     WOUND_MINOR    = 0;
static constexpr int     WOUND_MAJOR    = 1;
static constexpr int     NUM_WOUND_TIER = 2;
static constexpr uint8_t WOUND_MAX_EACH = 3;
// Medic treats a major wound in the field at this DN; anyone may treat while
// standing in a Settlement.  Costs 2 MP + 1 Medicine.
static constexpr uint8_t TREAT_DN       = 9;

// ── Action system constants ─────────────────────────────────────
static constexpr uint8_t ACT_FORAGE  = 0;
static constexpr uint8_t ACT_WATER   = 1;
static constexpr uint8_t ACT_TREAT   = 2;
static constexpr uint8_t ACT_SCAV    = 3;
static constexpr uint8_t ACT_SHELTER = 4;
static constexpr uint8_t ACT_CRAFT   = 5;
static constexpr uint8_t ACT_SURVEY  = 6;
static constexpr uint8_t ACT_REST    = 7;

static constexpr uint8_t AO_BLOCKED = 0;
static constexpr uint8_t AO_SUCCESS = 1;
static constexpr uint8_t AO_PARTIAL = 2;
static constexpr uint8_t AO_FAIL    = 3;

// River Channel (11) is reachable with the right equipment, so it needs a
// forage DN (this is what the Fishing Pole doubles) and drinkable water.
static const uint8_t TERRAIN_FORAGE_DN[NUM_TERRAIN]  = { 7,0,6,8,0,0,0,0,0,0,0, 6 };
static const uint8_t TERRAIN_SALVAGE_DN[NUM_TERRAIN] = { 0,0,0,0,6,7,8,0,0,0,0, 0 };
static const bool    TERRAIN_HAS_WATER[NUM_TERRAIN]  = { 0,0,0,1,0,1,0,0,0,0,0, 1 };
// "Ruins" here means Broken Urban (4) — dense standing structure that blocks
// weather and draws scavengers.  Flooded District (5) is open water-logged
// rubble and is deliberately NOT ruins: it carries the highest chem intensity.
static const bool    TERRAIN_IS_RUINS[NUM_TERRAIN]   = { 0,0,0,0,1,0,0,0,0,0,0, 0 };
static const bool    TERRAIN_IS_RAD[NUM_TERRAIN]     = { 0,1,0,0,0,0,1,0,0,0,1, 0 };

// ── Weather system constants ──────────────────────────────────────────────────
static constexpr uint8_t  WEATHER_CLEAR = 0, WEATHER_RAIN = 1, WEATHER_STORM = 2,
                           WEATHER_CHEM  = 3, WEATHER_FOG  = 4, WEATHER_MIST  = 5;
// Weather counter is in game-days; decremented once per dawn (not per tick).
// CLEAR, RAIN, STORM, CHEM, FOG ("Strangle Fog"), MIST (plain "Fog" — see
// WEATHER_PHASE_NAMES for display strings; MIST is the internal name only,
// kept distinct from FOG/"Strangle Fog" so the two are never confused in code).
// Strangle Fog is primarily a sight hazard (vis penalty close to chem's) with
// only a light per-hex move penalty (near rain's) — but see the per-tick
// MP/LL hazard below (search WEATHER_FOG in actions_game_loop.hpp) for the
// "strangle" part: it bleeds MP steadily and, more rarely, LL too. Mist is
// purely cosmetic — a plain, opaque whiteout with a mild vis penalty and no
// per-tick hazard at all, the "everyday" fog as opposed to Strangle Fog's
// dangerous variant.
static const int8_t  WEATHER_VIS_PENALTY[6]  = { 0, 1, 3, 5, 4, 2 };
static const uint8_t WEATHER_MOVE_PENALTY[6] = { 0, 1, 2, 3, 1, 1 };
// Index 0 (Clear) trimmed from {3,7} to {2,5} days — the dominant knob for
// how often weather becomes an incident at all, since every other phase is
// already short-lived (1-3 days) and Clear was the long stretch between them.
static const uint16_t WEATHER_DUR_MIN[6]     = { 2, 1, 1, 1, 1, 1 };
static const uint16_t WEATHER_DUR_MAX[6]     = { 5, 3, 2, 1, 2, 3 };
// Terrain intensity [phase][terrain idx 0-11] — MUST match JS copy exactly
// Terrains: 0=OpenScrub 1=AshDunes 2=RustForest 3=Marsh 4=BrokenUrban
//           5=FloodRuins 6=GlassFields 7=RollingHills 8=Mountain
//           9=Settlement 10=NukeCrater(impassable) 11=RiverChannel(impassable)
// Fog ("Strangle Fog") is worst in dense/wet terrain that tangles and
// disorients you (Rust Forest, Marsh, Flooded Ruins) and weakest on high dry
// ground where it thins out (Rolling Hills, Mountain) — drives its own
// per-tick MP/LL hazard below, same shape as chem's row but a different feel.
// Mist's row is all-zero: purely cosmetic, no per-tick hazard.
static const float WEATHER_INTENSITY[6][12] = {
  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.5f, 0.4f, 0.6f, 0.8f, 0.4f, 0.9f, 0.5f, 0.6f, 0.7f, 0.1f, 0.0f, 0.0f },
  { 0.7f, 0.6f, 0.7f, 0.9f, 0.5f, 1.0f, 0.8f, 0.9f, 1.0f, 0.2f, 0.0f, 0.0f },
  { 0.95f,0.85f,0.75f,0.90f,0.6f,0.95f,0.90f,0.90f,0.85f, 0.1f, 0.0f, 0.0f },
  { 0.45f,0.35f,0.7f, 0.75f,0.25f,0.65f,0.5f, 0.3f, 0.2f, 0.1f, 0.0f, 0.0f },
  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
};


// ── Item system ─────────────────────────────────────────────────
static constexpr uint8_t  MAX_ITEMS  = 128;
static constexpr uint8_t  MAX_GROUND = 32;

// ItemDef.passTerrainBits — what an equipped item lets the wearer do.
// Mirrors the `terrain` key documentation in data/items.cfg.
static constexpr uint8_t TERR_PASS_RIVER    = (1 << 0);  // may enter River Channel (11) at MC 2
static constexpr uint8_t TERR_PASS_CLIFF    = (1 << 1);  // Mountain (8) costs CLIFF_MC instead of 4
static constexpr uint8_t TERR_PASS_RAD      = (1 << 2);  // no Endure check on entering Rad terrain
static constexpr uint8_t RIVER_MC           = 2;
static constexpr uint8_t CLIFF_MC           = 2;

// EFX_NARRATIVE params the server acts on.  Params not listed here are
// client-side only (11 = UI scramble, 12 = reversed keys) — see items.cfg.
static constexpr uint8_t NAR_TELEPORT       = 10;  // teleport to a random surveyed hex
static constexpr uint8_t NAR_FIRE_STARTER   = 20;  // REST upgrades a basic shelter to improved
static constexpr uint8_t NAR_COLD_IMMUNE    = 21;  // no exposure LL loss at dawn
static constexpr uint8_t NAR_LL_CAP_DOWN    = 29;  // permanently lowers the LL ceiling by 1
static constexpr uint8_t NAR_SCAV_DOUBLE    = 30;  // doubles scrap from SCAVENGE
static constexpr uint8_t NAR_RIVER_FORAGE   = 31;  // doubles FORAGE yield on River Channel
static constexpr uint8_t NAR_LAND_FORAGE    = 32;  // doubles FORAGE yield on land hexes

enum StatIdx : uint8_t {
  STAT_LL      = 0,
  STAT_FOOD    = 1,
  STAT_WATER   = 2,
  STAT_RAD     = 3,
  STAT_MP      = 4,
  STAT_SLOTS   = 5,
  STAT_COUNT   = 6
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
  EFX_REVEAL_FOG    = 1,   // param 1 = +1 passive vision while equipped; >=2 = one-shot reveal radius; 99 = whole map
  EFX_NARRATIVE     = 2,   // param = NAR_* (server) or client-only id
  EFX_THREAT_MOD    = 3,   // param = signed delta to the Threat Clock (per use, or per dawn while equipped)
  EFX_CURE_STATUS   = 4,   // param = number of wounds healed (minor first, then major)
  EFX_COUNT
};

struct ItemDef {
  uint8_t      id;
  char         name[16];
  ItemCategory category;
  EquipSlot    equipSlot;
  uint8_t      maxStack;
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

// ── Crafting ──────────────────────────────────────────────────────────────
// Recipes are secret — a survivor must discover one via an encounter
// ("recipe" loot entry, see encounter_engine.hpp) before CRAFT will offer it.
// Crafting itself is an MP-costing, Settlement-only action (see ACT_CRAFT).
static constexpr uint8_t MAX_RECIPES     = 32;
static constexpr uint8_t RECIPE_MAX_MATS = 3;
static constexpr uint8_t TERRAIN_SETTLEMENT = 9;

struct RecipeDef {
  uint8_t id;
  char    name[16];
  uint8_t outputItem;
  uint8_t outputQty;
  uint8_t matItem[RECIPE_MAX_MATS];  // material ItemDef ids required (0 = unused slot)
  uint8_t matQty[RECIPE_MAX_MATS];
  uint8_t resCost[5];                // water/food/fuel/med/scrap tokens consumed
};

static const uint8_t TERRAIN_MC[NUM_TERRAIN]  = { 1, 2, 2, 3, 2, 3, 3, 2, 4, 1, 255, 255 };
static const int8_t  TERRAIN_VIS[NUM_TERRAIN] = { 0, 0, -3, 0, -2, 0, 1, 2, 2, -1, 0, -3 };
static const uint8_t TERRAIN_SV[NUM_TERRAIN]  = { 0, 0,  1, 0,  1,  2, 0, 1, 2, 3, 0, 0 };

// ── Debug label tables ─────────────────────────────────────────
[[maybe_unused]] static const char* T_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes ", "RustForst", "Marsh    ",
  "BrknUrban", "FloodRuin", "GlassFlds", "RolngHill",
  "Mountain ", "Settlment", "NukeCratr", "RiverChnl"
};
static const char* T_SHORT[NUM_TERRAIN] = {
  "Scrub","Dunes","Forst","Marsh","Urban","Flood","Glass","Hills","Mtn  ","Settl","Nukr ","River"
};
static const char* TERRAIN_IMG_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes", "RustForest", "Marsh",
  "BrokenUrban", "FloodedDistrict", "GlassFields",
  "Ridge", "Mountain", "Settlement", "NukeCrater", "RiverChannel"
};
[[maybe_unused]] static const char* VIS_LABEL[6] = { "BLIND", "PENLT", "LOW  ", "STD  ", "HIGH ", "VHIGH" };
[[maybe_unused]] static const char* RES_NAME[6]  = { "None","Water","Food ","Fuel ","Med  ","Scrap" };
[[maybe_unused]] static const char* DIR_NAME[6]  = { "SE","NE","N ","NW","SW","S " };
[[maybe_unused]] static const char* SKILL_NAME[NUM_SKILLS] = {
  "Navigate","Forage  ","Scavenge","Shelter ","Endure  "
};

// ── Survivor archetype tables ───────────────────────────────────
static const char* ARCHETYPE_NAME[NUM_ARCHETYPES] = {
  "Guide", "Quartermaster", "Medic", "Mule", "Scout", "Endurer"
};
static const uint8_t ARCHETYPE_SKILLS[NUM_ARCHETYPES][NUM_SKILLS] = {
  { 2, 1, 0, 1, 1 },
  { 0, 2, 1, 1, 0 },
  { 0, 0, 1, 0, 2 },
  { 0, 1, 2, 1, 1 },
  { 2, 1, 1, 0, 1 },
  { 1, 0, 0, 2, 2 },
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
  uint8_t  radiation;

  uint8_t  archetype;
  uint8_t  skills[NUM_SKILLS];
  uint8_t  invSlots;

  uint8_t  invType[INV_SLOTS_MAX];
  uint8_t  invQty[INV_SLOTS_MAX];

  uint8_t  equip[EQUIP_SLOTS];

  uint8_t  fThreshBelow;
  uint8_t  wThreshBelow;
  int8_t   movesLeft;

  uint8_t  wounds[NUM_WOUND_TIER];  // [0]=minor, [1]=major

  bool     resting;
  bool     radClean;
  uint8_t  llCapPenalty;  // permanent LL-ceiling reduction (Uranium Candy)

  uint8_t  surveyedMap[SURVEYED_BYTES];

  uint32_t knownRecipes;  // bit (id-1) per discovered RecipeDef, learned via encounters
};

// ── Tone sequences and motifs ────────────────────────────────────────────────
struct ToneStep { int freq; int beat; };

// Score-up is the only upbeat/positive sound — kept distinct from the dark motifs
static const ToneStep SEQ_SCORE_UP[] = {{220, 400}, {277, 400}, {330, 600}, {0,0}};

#include "tone-motifs.hpp"  // 19 post-apocalyptic motifs (MOTIF_*)

enum EvtType : uint8_t {
  EVT_COLLECT      = 1,
  EVT_RESPAWN      = 2,
  EVT_MOVE         = 3,
  EVT_JOINED       = 4,
  EVT_LEFT         = 5,
  EVT_DAWN         = 7,
  EVT_ACTION       = 8,
  EVT_DUSK         = 9,
  EVT_DOWNED       = 10,
  EVT_REGEN        = 11,
  EVT_TRADE_OFFER  = 12,
  EVT_TRADE_RESULT = 13,
  EVT_ENC_START    = 14,
  EVT_ENC_RESULT   = 15,
  EVT_ENC_BANK     = 16,
  EVT_ENC_END      = 17,
  EVT_COLLECT_FAIL = 18,
  EVT_WEATHER      = 19,
  EVT_FIRE_DAMAGE  = 20,   // player took fire damage: pid, q, r, amt (intensity) — Phase 2
  EVT_FIRE_SPREAD  = 21,   // hex caught fire: q, r, intensity (vision-culled) — Phase 2
  EVT_CARAVAN_TRADE = 22,  // caravan trade available: pid (co-located player)
  EVT_DOOM_WARNING = 23,   // creeping doom adjacent, low threshold: pid — Phase 3
  EVT_DOOM_ACT     = 24,   // creeping doom destroyed resource / drained LL: pid, q, r — Phase 3
  EVT_FLOOD_WASHOUT = 25,  // hex terrain just changed: q, r, amt=resulting terrain (3=Marsh edge, 5=Flooded District core) (vision-culled)
  EVT_FLOOD_DAMAGE  = 26   // player swept off their feet by a flash flood: pid, q, r, amt (sentinel)
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
  int8_t   dawnExpD;
  uint8_t  dawnWndMin, dawnWndMaj;
  uint8_t  actType;
  uint8_t  actOut;
  uint8_t  actNewLL;
  int8_t   actNewMP;
  int8_t   actFoodD;
  int8_t   actWatD;
  int8_t   actLLD;
  int8_t   actScrapD;
  int8_t   actMedD;
  int16_t  actScoreD;
  uint8_t  actCnd;
  uint8_t  actRecipe;   // recipe id crafted this action (ACT_CRAFT success only)
  uint8_t  actWndMin, actWndMaj;   // wound counts after a TREAT action
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
  int8_t   encPenLL, encPenRad;
  uint8_t  encPenRes[5];  // resources taken by a hazard (0=Wat 1=Fod 2=Ful 3=Med 4=Scr)
  uint8_t  encEnds;
  uint8_t  encPenWndMin, encPenWndMaj;  // wounds inflicted by a hazard
  uint8_t  encItemType;   // typed item granted (node "item" loot entry or loot-table roll)
  uint8_t  encItemQty;
  uint8_t  encItemType2;  // second typed item, when a node grants both
  uint8_t  encItemQty2;
  uint8_t  encRecipe;     // recipe id granted THIS choice (node "recipe" loot entry), for the in-scene toast
  uint32_t bankedRecipes; // enc_bank only: full bitmask of every recipe committed to knownRecipes just now
  uint8_t  encDrains[MAX_PLAYERS]; // per-ally resource drain on failure (auto-assist)
};

static constexpr uint32_t TRADE_EXPIRE_MS = 30000;

// Trades move legacy resource tokens only; typed items are not tradeable.
struct TradeOffer {
  bool     active;
  uint8_t  fromPid;
  uint8_t  toPid;
  uint8_t  give[5];
  uint8_t  want[5];
  uint32_t expiresMs;
};

// ── Encounter engine structs ───────────────────────────────────
#define ENC_MAX_ITEMS  3
#define ENC_KEY_LEN    24
struct ActiveEncounter {
  uint8_t  active;          // bit 0 = in encounter, bit 7 = reachedTerminal
  uint8_t  encIdx;          // encounter file index selected at enc_start
  uint8_t  hexQ, hexR;
  uint8_t  terrain;         // pool the file was drawn from (index into encPools)
  uint8_t  canBank;         // current node's can_bank flag (server-authoritative)
  char     nodeKey[ENC_KEY_LEN];  // current node in the encounter JSON
  uint8_t  pendingLoot[5];  // unbanked resource loot [Water,Food,Fuel,Med,Scrap]
  uint8_t  pendingItemType[ENC_MAX_ITEMS];
  uint8_t  pendingItemQty[ENC_MAX_ITEMS];
  uint8_t  pendingItemCount;
  uint32_t pendingRecipes;  // bitmask (bit id-1) of every recipe learned this scene, granted on enc_bank
};
static ActiveEncounter encounters[MAX_PLAYERS];

// EVT_ENC_END reason codes (carried in GameEvent.encOut; serialised as text
// by drainEvents(), mirrored by ENC_REASON_LABELS in data/network.js).
static constexpr uint8_t ENC_END_HAZARD     = 0;
static constexpr uint8_t ENC_END_ABORT      = 1;
static constexpr uint8_t ENC_END_DAWN       = 2;
static constexpr uint8_t ENC_END_DOWNED     = 3;
static constexpr uint8_t ENC_END_DISCONNECT = 4;
static constexpr uint8_t ENC_END_REGEN      = 5;
static constexpr uint8_t ENC_END_COUNT      = 6;
// Defined in encounter_engine.hpp; called from the game tick, session, and
// regen handlers which are included earlier/later in the chain.
static void endEncounter(int pid, uint8_t reason, bool restorePoi);

struct EncPoolInfo {
  uint8_t count;
  char    path[12];  // e.g. "urban", "marsh"
};
static EncPoolInfo encPools[10];  // indexed by terrain type 0-9

// POI encounter probability removed — encounters are now pre-placed
// at map generation time (one hex per encounter ID, guaranteed).
// See hex-map.hpp Phase 5.

// ── Loot table cache (parsed from /encounters/loot_tables.json at boot) ───────
// MAX_LOOT_TABLES must be >= the number of top-level tables in loot_tables.json
// (currently 34) — loadLootTables() in boot-assets.hpp silently stops parsing
// once it's full, so a table added past this cap just never loads.
static constexpr int MAX_LOOT_TABLES = 34;
struct LootEntry { uint8_t item; uint8_t qtyMin; uint8_t qtyMax; uint8_t weight; };
struct LootTable  { char name[20]; LootEntry entries[8]; uint8_t count; };
static LootTable  lootTables[MAX_LOOT_TABLES];
static uint8_t    lootTableCount = 0;

struct CheckResult { int r1, r2, skillVal, mods, total, dn; bool success; };

struct GameState {
  HexCell  (*map)[MAP_COLS];   // PSRAM: MAP_ROWS rows, allocated by allocPsramGlobals(); G.map[r][q] unchanged
  Player   players[MAX_PLAYERS];
  uint32_t tickId;
  int      connectedCount;
  SemaphoreHandle_t mutex;

  uint8_t  threatClock;

  uint32_t dayTick;
  uint16_t dayCount;

  uint8_t  weatherPhase;    // 0=clear 1=rain 2=storm 3=chem 4=fog(Strangle Fog) 5=mist(Fog)
  uint16_t weatherCounter;  // game-days remaining in current phase (decremented at dawn)
  uint16_t badWeatherTicks; // consecutive game-days in non-CLEAR phases
};

static constexpr int  EVT_QUEUE_SIZE = 64;
// Whole-map byte count — use instead of sizeof(G.map) (which is now a pointer).
static constexpr size_t MAP_BYTES = sizeof(HexCell) * MAP_ROWS * MAP_COLS;

static GameState      G;

// ── SD Save / Load constants + structs ────────────────────────────────────────
static constexpr uint32_t SAVE_MAGIC   = 0xDEADC0DEul;
static constexpr uint8_t  SAVE_VERSION = 14;
static const char         SAVE_DIR[]   = "/save";
static const char         SAVE_MAP_F[] = "/save/map.bin";
static const char         SAVE_PLY_F[] = "/save/players.bin";

struct __attribute__((packed)) SaveHeader {
  uint32_t magic;
  uint8_t  version;
  uint16_t dayCount;
  uint8_t  threatClock;
  uint8_t  weatherPhase;
  uint16_t weatherCounter;
  uint32_t dayTick;          // v12: resume mid-day instead of restarting the day clock
  uint16_t badWeatherTicks;  // v12: bad-weather streak survives a reboot
  // v13: world-system entities (docs/world-system-spec.md). W_hex (fire+track)
  // is deliberately NOT persisted — tracks decay in seconds anyway and fires
  // extinguish on power cycle, same as the spec specifies.
  int16_t  caravanQ, caravanR;
  uint8_t  caravanRestockTimer;
  uint8_t  caravanInv[5];
  uint8_t  caravanActive;
  int16_t  doomQ, doomR;
  uint8_t  doomAwareness;
};

struct __attribute__((packed)) SavePlayer {
  char     name[16];
  uint8_t  archetype;
  uint8_t  skills[NUM_SKILLS];
  int16_t  q, r;
  uint8_t  ll, food, water, radiation;
  uint8_t  inv[5];
  uint8_t  invType[12];
  uint8_t  invQty[12];
  uint8_t  equip[EQUIP_SLOTS];
  uint8_t  invSlots;
  uint16_t score;
  uint16_t steps;
  uint16_t encCount;
  int8_t   movesLeft;
  uint8_t  fThreshBelow;
  uint8_t  wThreshBelow;
  uint8_t  wounds[NUM_WOUND_TIER];
  uint8_t  used;
  uint8_t  radClean;      // v12
  uint8_t  llCapPenalty;  // v12
  uint8_t  surveyedMap[SURVEYED_BYTES];
  uint32_t knownRecipes;  // v14
};

struct __attribute__((packed)) SaveGroundItem {
  int16_t q, r;
  uint8_t itemType;
  uint8_t qty;
};

static GameEvent*     pendingEvents = nullptr;   // [EVT_QUEUE_SIZE], PSRAM (allocPsramGlobals)
static int            pendingCount  = 0;
static portMUX_TYPE   evtMux        = portMUX_INITIALIZER_UNLOCKED;
static TradeOffer     tradeOffers[MAX_PLAYERS];

// ── Item registry ─────────────────────────────────────────────
static ItemDef* itemRegistry = nullptr;          // [MAX_ITEMS], PSRAM (allocPsramGlobals)
static uint8_t  itemCount = 0;

// ── Recipe registry ────────────────────────────────────────────
static RecipeDef* recipeRegistry = nullptr;      // [MAX_RECIPES], PSRAM (allocPsramGlobals)
static uint8_t     recipeCount = 0;

// ── Ground items ──────────────────────────────────────────────
static GroundItem groundItems[MAX_GROUND];
static unsigned long  lastStatusMs  = 0;
static unsigned long  lastScreenMs  = 0;
static constexpr uint32_t SCREEN_MS  = 10000;
static UNIHIKER_K10   k10;
static Music          k10Music;
static LGFX           tft;
static LGFX_Sprite    canvas(&tft);

// ── K10 multi-screen state ─────────────────────────────────────────────────
// The event log is kept as a chronicle, not a log: each entry stores a
// *predicate* ("finds a seep still running.") plus the player it belongs to,
// and the screen prepends the name at draw time. drainEvents() runs without
// G.mutex and must not read Player.name, so the name cannot be baked in at
// log time. `who` < 0 means the text is already a whole sentence (a world
// event). A '\x01' byte inside the text is replaced with who2's name, for
// sentences about two people.
#define K10_LOG_SIZE 16
enum K10Tone : uint8_t { TONE_PLAIN = 0, TONE_GOOD, TONE_ILL, TONE_OMEN, TONE_COUNT };
struct K10LogEntry {
  char     text[48];
  uint32_t ms;
  uint16_t day;
  int8_t   who;
  int8_t   who2;
  uint8_t  tone;
};
static K10LogEntry  k10Log[K10_LOG_SIZE];
static uint8_t      k10LogHead  = 0;
static uint8_t      k10LogCount = 0;
static uint16_t     k10LogTotal = 0;   // entries ever set down — the page number
static portMUX_TYPE k10LogMux   = portMUX_INITIALIZER_UNLOCKED;

static uint8_t  k10Screen     = 1;
static uint8_t  k10ScreenLast = 255;
static bool     k10BtnBLast   = false;
static volatile bool k10Dirty = true;  // set whenever game state changes

static uint32_t k10TeamScore   = 0;
static uint32_t k10LedPulse    = 0;
static uint8_t  k10PulseR = 0, k10PulseG = 0, k10PulseB = 0;
static uint8_t  k10PrevTCLevel = 0;

static uint8_t  s_audioVol   = 5;
static uint8_t  s_ledBright  = 4;
static bool     s_screenFlip = false;

static void loadK10Prefs() {
  Preferences p; p.begin("k10", true);
  s_audioVol   = p.getUChar("vol",    5);
  s_ledBright  = p.getUChar("bright", 4);
  s_screenFlip = p.getBool("flip",    false);
  p.end();
}
static void saveK10Prefs() {
  Preferences p; p.begin("k10", false);
  p.putUChar("vol",    s_audioVol);
  p.putUChar("bright", s_ledBright);
  p.putBool("flip",    s_screenFlip);
  p.end();
}

// ── LED flash state ─────────────────────────────────────────────────────────
static volatile uint8_t  g_ledR = 0, g_ledG = 0, g_ledB = 0;
static volatile uint32_t g_ledEndMs = 0;
// Perish alarm (ui-leds.hpp): set by ledPerish() from the EVT_DOWNED handler,
// outranks both the event flash and the weather/time-of-day sky.
static volatile uint32_t g_ledPerishEndMs = 0;

AsyncWebServer server(80);
AsyncWebSocket  ws("/ws");

// ── PSRAM web-file cache ────────────────────────────────────────────────────
// Populated at boot by loadWebFilesToRAM() (boot-assets.hpp), which scans the
// SD card's /data ROOT: every regular file with a known web extension gets an
// HTTP route at "/<name>" (index.html also answers "/"). A "<name>.gz" sibling
// wins over the plain file and is served with Content-Encoding: gzip.
//
// Adding a new client file therefore needs NO firmware change: drop it in
// data/, list it in data/web-assets.json, run scripts/build_web.ps1 and sync.
// (The old hardcoded WEB_FILES[] table silently 404'd anything it didn't
// know about — that's how world-entities.js / *-field.js went missing.)
struct WebFile { char url[48]; const char* mime; bool gzip;
                 uint8_t* buf; size_t len; char etag[26]; };
static const int MAX_WEB_FILES = 48;
static WebFile*  webFiles = nullptr;             // [MAX_WEB_FILES], PSRAM (allocPsramGlobals)
static int       webFileCount = 0;

// ── PSRAM image cache ──────────────────────────────────────────────────────
struct ImgFile { char name[40]; uint8_t* buf; size_t len; char etag[26]; };
static const int MAX_IMG_CACHE = 100;
static ImgFile*  imgCache = nullptr;             // [MAX_IMG_CACHE], PSRAM (allocPsramGlobals)
static int       imgCacheCount = 0;

// Content-derived ETag (FNV-1a 32 over the bytes + length). Stable across
// reboots so browsers revalidating a cached asset get a cheap 304 instead of
// the whole file — the previous per-boot nonce invalidated every client cache
// on every power cycle.
static void makeEtag(char* out, size_t outLen, const uint8_t* buf, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) { h ^= buf[i]; h *= 16777619u; }
  snprintf(out, outLen, "\"%08lx-%zx\"", (unsigned long)h, len);
}

// ── Split module includes ──────────────────────────────────────
// Order matters: each file depends on declarations above it.
#include "hex-map.hpp"       // hex math, slot mgmt, map gen, vision encoding
#include "boot-assets.hpp"   // asset loading, item registry, loot tables
#include "ui-display.hpp"    // K10 screens, LED, audio, boot splash (needs getItemDef from boot-assets)
#include "usb_drive.h"       // USB MSC mode (needs canvas + canvasXxx from ui-display)

// World system: Caravan/Fire/Creeping Doom (see docs/world-system-spec.md).
// Depends only on hex-map.hpp; ticked from tickGame() in actions_game_loop.hpp.
#include "world-system.hpp"

// Gameplay chain (depend on hex-map + ui-display)
#include "survival_skills.hpp"
#include "inventory_items.hpp"     // depends on survival_skills
#include "survival_state.hpp"      // depends on survival_skills + inventory_items
#include "actions_game_loop.hpp"   // depends on all 3 above
#include "encounter_engine.hpp"    // server-side encounter JSON resolution

// Network layer
#include "network-persistence.hpp"
#include "network-sync.hpp"
#include "network-events.hpp"
#include "network-session.hpp"
#include "network-msg-player.hpp"
#include "network-msg-trade.hpp"
#include "network-msg-items.hpp"
#include "network-msg-encounter.hpp"
#include "network-handlers.hpp"

// Server orchestration — last: calls drainEvents(), broadcastState()
#include "game-server.hpp"

// ── PSRAM-resident globals ─────────────────────────────────────
// Must run before anything touches G.map / W_hex / the caches / the event
// queue / the item registry — i.e. first thing in setup(). Everything here
// used to be internal .bss (≈100 KB); see the PSRAM helpers near the top.
static void allocPsramGlobals() {
  uint32_t heapBefore = ESP.getFreeHeap();
  G.map         = (HexCell(*)[MAP_COLS])    psramStaticAlloc(MAP_BYTES);
  W_hex         = (HexDynamic(*)[MAP_COLS]) psramStaticAlloc(W_HEX_BYTES);
  pendingEvents = (GameEvent*)              psramStaticAlloc(sizeof(GameEvent) * EVT_QUEUE_SIZE);
  itemRegistry  = (ItemDef*)                psramStaticAlloc(sizeof(ItemDef)   * MAX_ITEMS);
  recipeRegistry= (RecipeDef*)              psramStaticAlloc(sizeof(RecipeDef) * MAX_RECIPES);
  imgCache      = (ImgFile*)                psramStaticAlloc(sizeof(ImgFile)   * MAX_IMG_CACHE);
  webFiles      = (WebFile*)                psramStaticAlloc(sizeof(WebFile)   * MAX_WEB_FILES);
  Log.notice("PSRAM globals: map=%u whex=%u evq=%u items=%u recipes=%u img=%u web=%u B; heap %u->%uKB psram=%uKB",
             (unsigned)MAP_BYTES, (unsigned)W_HEX_BYTES,
             (unsigned)(sizeof(GameEvent) * EVT_QUEUE_SIZE), (unsigned)(sizeof(ItemDef) * MAX_ITEMS),
             (unsigned)(sizeof(RecipeDef) * MAX_RECIPES),
             (unsigned)(sizeof(ImgFile) * MAX_IMG_CACHE), (unsigned)(sizeof(WebFile) * MAX_WEB_FILES),
             (unsigned)(heapBefore / 1024), (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
}

// ── Setup ──────────────────────────────────────────────────────
void setup() {
  uint32_t _bootT0 = millis();
  { unsigned long t0 = millis(); while (!Serial && millis()-t0 < 3000) delay(10); }
  delay(200);

  logInit(115200);
  Log.notice("==== BOOT ==== sketch=%uKB freeHeap=%uKB freePSRAM=%uKB",
             (unsigned)(ESP.getSketchSize() / 1024),
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
  allocPsramGlobals();

  // ── Reset reason ─────────────────────────────────────────────
  { esp_reset_reason_t rr = esp_reset_reason();
    const char* rs;
    switch(rr) {
      case ESP_RST_POWERON:  rs = "POWER_ON";  break;
      case ESP_RST_EXT:      rs = "EXT_PIN";   break;
      case ESP_RST_SW:       rs = "SW_RESET";  break;
      case ESP_RST_PANIC:    rs = "PANIC";      break;
      case ESP_RST_INT_WDT:  rs = "INT_WDT";   break;
      case ESP_RST_TASK_WDT: rs = "TASK_WDT";  break;
      case ESP_RST_WDT:      rs = "WDT";        break;
      case ESP_RST_BROWNOUT: rs = "BROWNOUT";  break;
      case ESP_RST_DEEPSLEEP:rs = "DEEPSLEEP"; break;
      default:               rs = "UNKNOWN";   break;
    }
    Log.notice("reset reason=%s", rs);
  }

  // ── K10 hardware init (buttons, LEDs, audio) ─────────────────
  Log.notice("K10 hw init start");
  k10.begin();
  Log.notice("K10 hw init ok");
  loadK10Prefs();
  Log.notice("K10 prefs loaded: audioVol=%d ledBright=%d", (int)s_audioVol, (int)s_ledBright);

  // ── LovyanGFX display init ────────────────────────────────────
  // k10.begin() turns backlight off (XL9535 P0.0=LOW). Enable it via Wire.
  Log.verbose("I2C begin SDA=47 SCL=48");
  Wire.begin(47, 48);  // SDA=47, SCL=48 (K10 I2C bus)
  {
    // Config P0.0 as output
    Wire.beginTransmission(0x20); Wire.write(0x06); Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)0x20, (uint8_t)1); uint8_t cfg0 = Wire.read();
    Wire.beginTransmission(0x20); Wire.write(0x06); Wire.write(cfg0 & ~0x01); Wire.endTransmission();
    // Set P0.0 HIGH = backlight on
    Wire.beginTransmission(0x20); Wire.write(0x02); Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)0x20, (uint8_t)1); uint8_t out0 = Wire.read();
    Wire.beginTransmission(0x20); Wire.write(0x02); Wire.write(out0 | 0x01); Wire.endTransmission();
  }
  Log.verbose("XL9535 backlight enabled");
  tft.init();
  tft.setRotation(s_screenFlip ? 0 : 2);
  Log.notice("Display init ok rotation=%d", s_screenFlip ? 0 : 2);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(240, 320);
  canvas.fillScreen(0x0000);
  canvas.pushSprite(0, 0);
  Log.notice("Sprite 240x320 16bpp in PSRAM");
  splashAdd("Display OK", 0x406030);
  splashAdd("Hold [A] now = USB drive", 0x203060);

  // ── Mutex + game state init ───────────────────────────────────
  G.mutex = xSemaphoreCreateMutex();
  if (!G.mutex) { Log.fatal("Game mutex create FAILED — halting"); for (;;) delay(1000); }
  Log.notice("Game mutex created");

  G.tickId = 0; G.connectedCount = 0;
  G.threatClock = 0;
  G.dayTick = 0; G.dayCount = 0;
  resetWeather();
  memset(groundItems, 0, sizeof(groundItems));
  memset(encounters, 0, sizeof(encounters));
  memset(tradeOffers, 0, sizeof(tradeOffers));

  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    p.connected = false; p.wsClientId = 0;
    p.q = p.r = 0; p.connectMs = 0;
    resetSurvivor(p, (uint8_t)i);
    snprintf(p.name, sizeof(p.name), "%s%d", ARCHETYPE_NAME[i], i);
  }

  // ── SD card mount ─────────────────────────────────────────────
  splashAdd("Mounting SD card...");
  Log.notice("SD mount start");
  if (!SD.begin()) {
    Log.fatal("SD.begin() FAILED — halting");
    splashAdd("SD FAIL - insert card!", 0xC04020);
    for (;;) delay(1000);
  }
  {
    uint64_t tot = SD.totalBytes() / (1024*1024);
    uint64_t use = SD.usedBytes()  / (1024*1024);
    Log.notice("SD mount OK total=%uMB used=%uMB", (unsigned)tot, (unsigned)use);
    char sdBuf[30]; snprintf(sdBuf, 30, "SD %uMB/%uMB used", (unsigned)tot, (unsigned)use);
    splashAdd(sdBuf, 0x406030);
  }
  if (!SD.exists("/data/index.html")) {
    Log.warning("SD MISSING: /data/index.html");
    splashAdd("WARN: no index.html!", 0xC89030);
  } else {
    Log.notice("index.html found");
    splashAdd("index.html OK", 0x60A040);
  }

  Log.notice("Web cache start");
  loadWebFilesToRAM();
  for (int i = 0; i < webFileCount; i++)
    makeEtag(webFiles[i].etag, sizeof(webFiles[i].etag), webFiles[i].buf, webFiles[i].len);
  for (int i = 0; i < imgCacheCount; i++)
    makeEtag(imgCache[i].etag, sizeof(imgCache[i].etag), imgCache[i].buf, imgCache[i].len);
  Log.notice("Web cache complete: %d web files, %d images, freeHeap=%uKB freePSRAM=%uKB",
             (int)webFileCount, (int)imgCacheCount,
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
  { char hb[36]; snprintf(hb, 36, "Web: %ukB in PSRAM", (unsigned)(ESP.getFreeHeap()/1024));
    splashAdd(hb, 0x406030); }

  // ── USB drive mode (hold Button A during SD mount splash) ───────────────────
  if (k10.buttonA && k10.buttonA->isPressed()) {
    Log.notice("ButtonA held — entering USB MSC mode");
    splashAdd("USB DRIVE MODE!", 0x0070C0);
    delay(200);
    enterUSBDriveMode(k10);
    // never returns
  } else {
    Log.notice("ButtonA not held, normal boot");
  }

  setupVariantCounts();

  Log.verbose("Effect table init");
  initEffectTable();
  Log.notice("Items load start");
  splashAdd("Loading items...");
  loadItemRegistry();
  Log.notice("Items loaded: %d", (int)itemCount);
  { char ib[30]; snprintf(ib, 30, "Items: %d loaded", (int)itemCount);
    splashAdd(ib, 0x60A040); }

  Log.notice("Recipes load start");
  splashAdd("Loading recipes...");
  loadRecipeRegistry();
  Log.notice("Recipes loaded: %d", (int)recipeCount);
  { char rb[30]; snprintf(rb, 30, "Recipes: %d loaded", (int)recipeCount);
    splashAdd(rb, 0x60A040); }

  Log.notice("Encounter index load start");
  splashAdd("Loading encounters...");
  loadEncounterIndex();
  loadLootTables();
  Log.notice("Encounters: %d loot tables", (int)lootTableCount);
  { char eb[30]; snprintf(eb, 30, "Enc: %d tables", (int)lootTableCount);
    splashAdd(eb, 0x60A040); }

  splashAdd("Generating map...");
  Log.notice("Save load attempt");
  if (!tryLoadSave()) {
    Log.notice("No save found, generating map");
    generateMap();
    wInit();  // fresh world — tryLoadSave() already called wInit() on its own success path
  } else {
    Log.notice("Save loaded: map+players+world");
  }
  Log.notice("Map ready %dx%d", (int)MAP_COLS, (int)MAP_ROWS);
  { char mb[30]; snprintf(mb, 30, "Map %dx%d ready", MAP_COLS, MAP_ROWS);
    splashAdd(mb, 0x60A040); }

  Log.notice("World system ready: caravan at (%d,%d) doom at (%d,%d)",
             (int)W.caravan.q, (int)W.caravan.r, (int)W.creepingDoom.q, (int)W.creepingDoom.r);

  setupWiFiAndServer();

  xTaskCreatePinnedToCore(gameLoopTask, "GameLoop", 24576, NULL, 2, NULL, 1);
  Log.notice("gameLoopTask spawned core=1 prio=2 stack=24KB");
  Log.notice("==== BOOT COMPLETE elapsed=%ums ====", (unsigned)(millis() - _bootT0));
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
      Log.notice("Boot STA connected ssid=%s ip=%s rssi=%d elapsed=%ums",
                 savedSsid, WiFi.localIP().toString().c_str(),
                 (int)WiFi.RSSI(), (unsigned)(now - bootWifiStartMs));
      k10ScreenLast = 255;  // force title redraw after WiFi splash would have disrupted it
      char buf[88];
      int blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}",
        WiFi.localIP().toString().c_str());
      ws.textAll(buf, (size_t)blen);
      Log.notice("NTP configTime(pool.ntp.org, time.nist.gov) called");
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    } else if (wst == WL_CONNECT_FAILED || wst == WL_NO_SSID_AVAIL ||
               now - bootWifiStartMs > BOOT_WIFI_TIMEOUT) {
      bootWifiPending = false;
      Log.warning("Boot STA FAIL ssid=%s status=%d elapsed=%ums",
                  savedSsid, (int)wst, (unsigned)(now - bootWifiStartMs));
      savedSsid[0] = '\0';
    }
  }

  checkGestureSwitch();
  checkScoreAudio();

  bool screenChanged = (k10Screen != k10ScreenLast);
  k10ScreenLast = k10Screen;
  bool uploadActive = UploadUI::isActive();
  // Repaint faster while an upload is streaming so the bar/byte counter animate.
  unsigned long screenInterval = uploadActive ? 100UL : (unsigned long)SCREEN_MS;
  if (screenChanged || k10Dirty || uploadActive || (now - lastScreenMs >= screenInterval)) {
    lastScreenMs = now;
    k10Dirty = false;
    if (uploadActive) {
      drawUploadScreen();
    } else {
      switch (k10Screen) {
        case 2:  drawEventLogScreen();   break;
        case 3:  drawResourceScreen();   break;
        case 4:  drawEncounterScreen();  break;
        case 5:  drawMapScreen();        break;
        default: drawPlayerScreen();     break;  // case 1
      }
    }
    canvas.pushSprite(0, 0);
    if (!uploadActive) k10ScreenLast = k10Screen; else k10ScreenLast = 255;  // force repaint when leaving upload
  }

  // Perish alarm drives the lamps every tick until it expires (updateLEDs
  // clears g_ledPerishEndMs itself); otherwise hold an event flash for its
  // 300 ms, and fall through to the weather/time-of-day sky the rest of time.
  if (g_ledPerishEndMs) {
    g_ledEndMs = 0;
    updateLEDs();
  } else if (g_ledEndMs && now >= g_ledEndMs) {
    g_ledEndMs = 0;
    updateLEDs();
  } else if (!g_ledEndMs) {
    updateLEDs();
  }

  if (now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    Log.verbose("status: connected=%d tick=%lu freeHeap=%uKB",
                (int)G.connectedCount, (unsigned long)G.tickId,
                (unsigned)(ESP.getFreeHeap() / 1024));
  }
  delay(100);
}
