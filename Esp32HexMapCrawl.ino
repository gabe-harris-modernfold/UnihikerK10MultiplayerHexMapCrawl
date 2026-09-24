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
#include <ESPmDNS.h>          // k10.local (game-server.hpp)
#include <esp_core_dump.h>    // last-crash summary for /state
#include "logging.hpp"
#include "wifi-store.hpp"   // known-network roaming list (NVS "wifinets")

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
// How many survivors the board's own softAP will admit. This is exactly the
// ESP32 core's softAP() default, named here so the "direct uplink" warning the
// client shows (UPLINK_WARNINGS in data/ui-utils.js) quotes a number that is
// actually true, and so raising it later is a one-line change in one place.
static constexpr int AP_MAX_CLIENTS = 4;

// ── WiFi STA connection (background task) ──────────────────────
struct WifiTaskCtx { char ssid[33]; char pass[65]; };
static volatile bool wifiConnecting = false;
static char savedSsid[33] = {0};
static char savedPass[65] = {0};
static bool     bootWifiPending = false;
static uint32_t bootWifiStartMs = 0;
static constexpr uint32_t BOOT_WIFI_TIMEOUT = 12000;

// ── Known-network roaming sweep ────────────────────────────────────────────
// While the board is off every known network, loop() periodically kicks the
// auto-join sweep in network-session.hpp: scan the air, join the strongest
// network in wifi-store.hpp. A scan stalls the softAP for a second or two, so
// repeated misses back off 30s → 60 → 120 → 240 → 300 rather than hammering a
// location where nothing is known. First sweep waits 20s so it can't collide
// with the page load that usually follows boot.
static uint32_t wifiNextSweepMs  = 20000;
static uint32_t wifiSweepBackoff = 30000;
static constexpr uint32_t WIFI_SWEEP_MIN = 30000;
static constexpr uint32_t WIFI_SWEEP_MAX = 300000;
static bool rtcSynced = false;

static bool checkRtcReady() {
  if (rtcSynced) return true;
  struct tm ti;
  if (getLocalTime(&ti, 0) && ti.tm_year > 100) rtcSynced = true;
  return rtcSynced;
}

// ── Bunker tunnel system (tunnels.hpp) ──────────────────────────
// A second, much smaller hex board shared by every player, reached through
// hatch hexes scattered on the surface map (terrain 12/13).  Declared here
// rather than in tunnels.hpp because GameState and SaveHeader below both
// need these sizes before that file is included.
//
// The tunnel board does NOT wrap: wrapQ/wrapR are hardcoded to MAP_COLS/
// MAP_ROWS, so tunnel neighbour math uses tunIn() and an out-of-bounds
// neighbour is simply not a legal move.
static constexpr int      TUN_COLS       = 16;
static constexpr int      TUN_ROWS       = 10;
static constexpr uint8_t  MAX_HATCHES    = 8;
static constexpr uint8_t  TUNNEL_MC      = 2;   // MC of Tunnel Floor (14); mirrors TERRAIN_MC
static constexpr uint8_t  VENT_ASCEND_MP = 2;   // climbing out of a Vent Shaft (13) costs double
// Underground sight. Base is your own hex plus one ring -- no terrain, weather
// or smoke modifiers apply down there. A carried Bile Flare adds 1; a Scout
// adds TUNNEL_VIS_SCOUT on top (less than their +2 on the surface, so the
// light still matters). See playerVisParams() in hex-map.hpp.
static constexpr int      TUNNEL_VIS_BASE  = 1;
static constexpr int      TUNNEL_VIS_SCOUT = 1;
static constexpr uint8_t  ITEM_BILE_FLARE  = 54;  // data/items.cfg — the tunnel light source

// One surface hatch and the shaft cell beneath it.  sq/sr index G.map,
// tq/tr index G.tunnel.  Persisted in SaveHeader, so keep it packed.
struct __attribute__((packed)) BunkerHatch { int16_t sq, sr; uint8_t tq, tr; };

// Filled by generateTunnels() (tunnels.hpp) or restored by tryLoadSave().
// Declared here rather than in tunnels.hpp because ui-display.hpp draws the
// hatches on the LCD minimap and is included well before tunnels.hpp.
// hatchCount may be < MAX_HATCHES when the surface map had nowhere legal to
// put them all.
static BunkerHatch bunkerHatches[MAX_HATCHES];
static uint8_t     hatchCount = 0;

// ── Constants ──────────────────────────────────────────────────
static constexpr int      MAP_COLS      = 75;
static constexpr int      MAP_ROWS      = 57;
static constexpr int      SURVEYED_BYTES = (MAP_ROWS * MAP_COLS + 7) / 8;
static constexpr int      MAX_PLAYERS   = 6;
static constexpr int      VISION_R      = 3;
static constexpr int      NUM_TERRAIN   = 16;
static constexpr uint32_t TICK_MS       = 100;
static constexpr uint8_t  RESPAWN_TICKS = 200;
static constexpr uint32_t MOVE_CD_MS    = 220;
static constexpr uint32_t STATUS_MS     = 30000;

// ── Survivor system constants ───────────────────────────────────
static constexpr int      NUM_ARCHETYPES  = 6;
static constexpr int      NUM_SKILLS      = 5;
static constexpr int      INV_SLOTS_STD   = 8;
static constexpr int      INV_SLOTS_MULE  = 12;
// The hard width of invType[]/invQty[], and therefore the ceiling
// effectiveInvSlots() clamps to.  It must stay above the largest reachable
// base + bonus or slot-granting gear silently does nothing: this was 12, the
// same as INV_SLOTS_MULE, so a Mule wearing a Backpack (+4) or a Hoarder's
// Rig (+4) gained exactly zero slots and no message said why.  Worst case is
// INV_SLOTS_MULE + body(+4) + hand(+1) = 17; 18 leaves a slot of headroom.
// Nothing hardcodes the width any more -- appendPackArrays() in
// inventory_items.hpp emits the JSON arrays -- so this is a single knob.
static constexpr int      INV_SLOTS_MAX   = 18;
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
// Why an action came back AO_BLOCKED.  AO_BLOCKED on its own says "refused,
// no MP spent", which was fine while every reason was something the player
// could already see on their own screen (wrong terrain, no MP).  "Your pack
// is full of scrap" is not one of those, and a silent refusal there reads
// exactly like a dead button -- it livelocked the bot harness for a whole run.
//
// Every AO_BLOCKED path names one of these, so a refusal is never silent. The
// browser only acts on ABW_PACK_FULL; the rest are for bots (the "bw" field on
// the act event, and the nack reply -- see network-reply.hpp).
static constexpr uint8_t ABW_NONE       = 0;
static constexpr uint8_t ABW_PACK_FULL  = 1;  // no token capacity left
static constexpr uint8_t ABW_TERRAIN    = 2;  // this action cannot be done on this hex
static constexpr uint8_t ABW_NO_MP      = 3;
static constexpr uint8_t ABW_NO_RES     = 4;  // missing the resource it consumes (scrap, medicine)
static constexpr uint8_t ABW_NOT_NEEDED = 5;  // nothing to do: no wound to treat, shelter already improved
static constexpr uint8_t ABW_RESTING    = 6;  // already resting
static constexpr uint8_t ABW_ARCHETYPE  = 7;  // this archetype cannot (TREAT outside a Settlement)
static constexpr uint8_t ABW_CRAFT      = 8;  // CRAFT refused; the err reply carries the reason
static constexpr uint8_t ABW_BAD_ACT    = 9;  // unknown action type
// ABW_* as a nack code (network-reply.hpp).
static const char* abwName(uint8_t w) {
  switch (w) {
    case ABW_PACK_FULL:  return "pack_full";
    case ABW_TERRAIN:    return "terrain";
    case ABW_NO_MP:      return "no_mp";
    case ABW_NO_RES:     return "no_res";
    case ABW_NOT_NEEDED: return "not_needed";
    case ABW_RESTING:    return "resting";
    case ABW_ARCHETYPE:  return "archetype";
    case ABW_CRAFT:      return "craft";
    case ABW_BAD_ACT:    return "bad_act";
    default:             return "blocked";
  }
}
static constexpr uint8_t AO_SUCCESS = 1;
static constexpr uint8_t AO_PARTIAL = 2;
static constexpr uint8_t AO_FAIL    = 3;

// River Channel (11) is reachable with the right equipment, so it needs a
// forage DN (this is what the Fishing Pole doubles) and drinkable water.
static const uint8_t TERRAIN_FORAGE_DN[NUM_TERRAIN]  = { 7,0,6,8,0,0,0,0,0,0,0, 6, 0,0,0,0 };
// Tunnel Floor (14) salvages pre-war bunker fittings at DN 7 (tunnels.hpp).
static const uint8_t TERRAIN_SALVAGE_DN[NUM_TERRAIN] = { 0,0,0,0,6,7,8,0,0,0,0, 0, 0,0,7,0 };
// Tunnel Floor (14) has water: seeps and pre-war cisterns.
static const bool    TERRAIN_HAS_WATER[NUM_TERRAIN]  = { 0,0,0,1,0,1,0,0,0,0,0, 1, 0,0,1,0 };
// "Ruins" here means Broken Urban (4) — dense standing structure that blocks
// weather and draws scavengers.  Flooded District (5) is open water-logged
// rubble and is deliberately NOT ruins: it carries the highest chem intensity.
// Tunnels are deliberately NOT ruins: salvaging underground must not raise
// the Threat Clock -- nothing on the surface hears you.
static const bool    TERRAIN_IS_RUINS[NUM_TERRAIN]   = { 0,0,0,0,1,0,0,0,0,0,0, 0, 0,0,0,0 };
static const bool    TERRAIN_IS_RAD[NUM_TERRAIN]     = { 0,1,0,0,0,0,1,0,0,0,1, 0, 0,0,0,0 };

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
static const int8_t  WEATHER_VIS_PENALTY[6]  = { 0, 1, 2, 3, 0, 2 };
static const uint8_t WEATHER_MOVE_PENALTY[6] = { 0, 1, 2, 3, 1, 1 };
// Hexes a survivor may cross in the open during one chem storm before the
// air starts taking LL per step. Enough to reach cover you can see; not
// enough to cross a storm. Reset on every weather change — see movePlayer().
static constexpr uint8_t CHEM_FREE_MOVES = 2;
// Index 0 (Clear) trimmed from {3,7} to {2,5} days — the dominant knob for
// how often weather becomes an incident at all, since every other phase is
// already short-lived (1-3 days) and Clear was the long stretch between them.
static const uint16_t WEATHER_DUR_MIN[6]     = { 2, 1, 1, 1, 3, 1 };
static const uint16_t WEATHER_DUR_MAX[6]     = { 5, 3, 2, 1, 5, 3 };
// Terrain intensity [phase][terrain idx 0-15] — MUST match JS copy exactly
// Terrains: 0=OpenScrub 1=AshDunes 2=RustForest 3=Marsh 4=BrokenUrban
//           5=FloodRuins 6=GlassFields 7=RollingHills 8=Mountain
//           9=Settlement 10=NukeCrater(impassable) 11=RiverChannel(impassable)
//           12=BunkerEntrance 13=VentShaft 14=TunnelFloor 15=TunnelCollapsed
// Fog ("Strangle Fog") is worst in dense/wet terrain that tangles and
// disorients you (Rust Forest, Marsh, Flooded Ruins) and weakest on high dry
// ground where it thins out (Rolling Hills, Mountain) — drives its own
// per-tick MP/LL hazard below, same shape as chem's row but a different feel.
// Mist's row is all-zero: purely cosmetic, no per-tick hazard.
// Columns 12-15 (bunker hatches + tunnel interior) are all-zero: weather does
// not reach underground, and the per-tick hazard loops skip depth>0 players
// outright.  The hatch tiles (12/13) sit on the surface but are sheltered
// mouths, so they take no weather damage either.
static const float WEATHER_INTENSITY[6][NUM_TERRAIN] = {
  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.5f, 0.4f, 0.6f, 0.8f, 0.4f, 0.9f, 0.5f, 0.6f, 0.7f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.7f, 0.6f, 0.7f, 0.9f, 0.5f, 1.0f, 0.8f, 0.9f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.95f,0.85f,0.75f,0.90f,0.6f,0.95f,0.90f,0.90f,0.85f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.45f,0.35f,0.7f, 0.75f,0.25f,0.65f,0.5f, 0.3f, 0.2f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
};


// ── Item system ─────────────────────────────────────────────────
static constexpr uint8_t  MAX_ITEMS  = 128;
static constexpr uint8_t  MAX_GROUND = 32;
// Caravan shelf (world-system.hpp): consumables the trader sells for resource
// tokens. Lives here rather than with the other CARAVAN_* tuning because
// SaveHeader below needs the slot count before world-system.hpp is included.
static constexpr uint8_t  CARAVAN_STOCK_SLOTS = 4;

// ItemDef.passTerrainBits — what an equipped item lets the wearer do.
// Mirrors the `terrain` key documentation in data/items.cfg.
static constexpr uint8_t TERR_PASS_RIVER    = (1 << 0);  // may enter River Channel (11) at MC 2
static constexpr uint8_t TERR_PASS_CLIFF    = (1 << 1);  // Mountain (8) costs CLIFF_MC instead of 4
static constexpr uint8_t TERR_PASS_RAD      = (1 << 2);  // no Endure check on entering Rad terrain
static constexpr uint8_t TERR_PASS_WATER    = (1 << 3);  // any water terrain (Marsh/Flooded District/River) at RAFT_MC
static constexpr uint8_t RIVER_MC           = 2;
static constexpr uint8_t CLIFF_MC           = 2;
static constexpr uint8_t RAFT_MC            = 1;

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
  uint8_t      value;      // items.cfg "value": caravan asking price, in resource tokens of any mix
  uint8_t      tradeable;  // items.cfg "trade": 1 = the caravan may put this on its shelf (consumables only today)
  uint8_t      leavesTracks;  // items.cfg "tracks": 1 = moving with this equipped marks HexCell.tireTrack (the Motorbike today)
};

struct GroundItem {
  int16_t  q, r;
  uint8_t  itemType;
  uint8_t  qty;
};

// ── Crafting ──────────────────────────────────────────────────────────────
// Most recipes are secret — a survivor must discover one via an encounter
// ("recipe" loot entry, see encounter_engine.hpp) before CRAFT will offer it.
// The exception is a recipe flagged `starter = yes` in data/recipes.cfg: that
// is basic wasteland know-how everybody walks in with, so its bit is set at
// spawn (resetSurvivor) and OR'd into every loaded save (tryLoadSave), which
// is what lets a starter recipe be added later without a save migration.
// Crafting itself is an MP-costing, Settlement-only action (see ACT_CRAFT).
static constexpr uint8_t MAX_RECIPES     = 32;
static constexpr uint8_t RECIPE_MAX_MATS = 3;
static constexpr uint8_t TERRAIN_SETTLEMENT = 9;

struct RecipeDef {
  uint8_t id;
  char    name[24];                  // display name, max 23 chars (see data/recipes.cfg header)
  uint8_t outputItem;
  uint8_t outputQty;
  uint8_t matItem[RECIPE_MAX_MATS];  // material ItemDef ids required (0 = unused slot)
  uint8_t matQty[RECIPE_MAX_MATS];
  uint8_t resCost[5];                // water/food/fuel/med/scrap tokens consumed
  uint8_t starter;                   // 1 = known from spawn, no encounter needed
};

// 12-15 are the bunker tunnel system (tunnels.hpp).  12/13 sit on BOTH boards:
// the surface hatch, and the shaft cell directly beneath it on the tunnel
// board.  14/15 are tunnel-board only.  TERRAIN_VIS is unused underground --
// playerVisParams() takes a separate branch at depth 1 (base radius 1, +1 per
// Bile Flare carried, +1 Scout).
static const uint8_t TERRAIN_MC[NUM_TERRAIN]  = { 1, 2, 2, 3, 2, 3, 3, 2, 4, 1, 255, 255, 1, 1, 2, 255 };
static const int8_t  TERRAIN_VIS[NUM_TERRAIN] = { 0, 0, -3, 0, -2, 0, 1, 2, 2, -1, 0, -3, 0, 0, 0, 0 };
static const uint8_t TERRAIN_SV[NUM_TERRAIN]  = { 0, 0,  1, 0,  1,  2, 0, 1, 2, 3, 0, 0, 2, 0, 1, 0 };

// ── Debug label tables ─────────────────────────────────────────
[[maybe_unused]] static const char* T_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes ", "RustForst", "Marsh    ",
  "BrknUrban", "FloodRuin", "GlassFlds", "RolngHill",
  "Mountain ", "Settlment", "NukeCratr", "RiverChnl",
  "BunkerEnt", "VentShaft", "TunnlFlor", "TunnlClpd"
};
static const char* T_SHORT[NUM_TERRAIN] = {
  "Scrub","Dunes","Forst","Marsh","Urban","Flood","Glass","Hills","Mtn  ","Settl","Nukr ","River",
  "Bunkr","Vent ","Tunnl","Clpsd"
};
static const char* TERRAIN_IMG_NAME[NUM_TERRAIN] = {
  "OpenScrub", "AshDunes", "RustForest", "Marsh",
  "BrokenUrban", "FloodedDistrict", "GlassFields",
  "Ridge", "Mountain", "Settlement", "NukeCrater", "RiverChannel",
  "BunkerEntrance", "VentShaft", "TunnelFloor", "TunnelCollapsed"
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
  uint8_t poi;        // 0 = none/looted, non-zero = has encounter
  uint8_t tireTrack;  // 0/1 — caravan has driven through this hex (wire-packed into TT bit 7)
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
  // Hexes crossed in the open during the CURRENT chem storm. Runtime only —
  // same as the caravan's holdTicks/stuckTicks — and reset by
  // updateWeatherPhase() on every phase change, so a reboot mid-storm just
  // gives the survivor their dash back. See movePlayer()'s chem clause.
  uint8_t  chemMoves;

  uint8_t  surveyedMap[SURVEYED_BYTES];

  // ── Bunker tunnels (tunnels.hpp) ──
  // q/r above ALWAYS stay on the surface, pinned to the hatch this player
  // descended through.  That is what lets every surface subsystem (weather,
  // doom, fire, flood, caravan, the LCD minimap) keep indexing G.map[r][q]
  // unchanged -- they just skip players with depth != 0.
  uint8_t  depth;      // 0 = surface, 1 = in the tunnels
  int16_t  tq, tr;     // position on G.tunnel, meaningful only while depth == 1
  uint8_t  hatchIdx;   // index into bunkerHatches[] of the hatch we came down

  uint32_t knownRecipes;  // bit (id-1) per known RecipeDef: starter recipes from
                          // spawn, the rest learned via encounters
};

// ── Tone sequences and motifs ────────────────────────────────────────────────
struct ToneStep { int freq; int beat; };

// Score-up is the only upbeat/positive sound — kept distinct from the dark motifs
static const ToneStep SEQ_SCORE_UP[] = {{220, 400}, {277, 400}, {330, 600}, {0,0}};

#include "tone-motifs.hpp"  // 23 post-apocalyptic motifs (MOTIF_*)

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
  EVT_FLOOD_DAMAGE  = 26,  // player swept off their feet by a flash flood: pid, q, r, amt (sentinel)
  // ── Bunker tunnels (tunnels.hpp) ──
  EVT_TUNNEL_ENTER  = 27,  // player descended: pid, q/r = surface hatch, amt = hatch index
  EVT_TUNNEL_EXIT   = 28,  // player surfaced:  pid, q/r = surface hatch, amt = hatch index
  // ── Creeping Doom taunts (world-system.hpp) ──
  EVT_DOOM_TAUNT    = 29,  // the Doom speaks: pid = who it addresses, amt = tier (0-3), res = line index
  // ── Bunker tunnel taunts (tunnels.hpp) ──
  EVT_TUNNEL_TAUNT  = 30,  // second thoughts about sleeping rough underground:
                           // pid = whose, res = line index. Unicast, unlike the
                           // Doom's — this one is nobody else's business.
  // LL lost to a hazard that has no event of its own (chem storm, Strangle
  // Fog). pid, amt = LL lost, res = DC_* cause. Every other LL loss already
  // rides its own event (dawn, dusk, fire_dmg, flood_dmg, doom_act, enc_res).
  EVT_DAMAGE        = 31
};

// Why a survivor went down: rides EVT_DOWNED (and EVT_DAMAGE) as ev.res, and
// goes on the wire as "cause" in the names bots/causes.py already reports.
enum DownCause : uint8_t {
  DC_UNKNOWN = 0, DC_THIRST, DC_HUNGER, DC_EXPOSURE, DC_BAD_AIR, DC_RADIATION,
  DC_FIRE, DC_LIGHTNING, DC_FLOOD, DC_DOOM, DC_ENC_HAZARD, DC_ENC_COST,
  DC_ACTION, DC_CHEM, DC_FOG, DC_COUNT
};
static const char* const DC_NAME[DC_COUNT] = {
  "unattributed", "thirst", "hunger", "exposure", "bad air", "radiation",
  "fire", "lightning", "flood", "creeping doom", "encounter hazard",
  "encounter cost", "action", "chem storm", "strangle fog"
};
static inline const char* dcName(uint8_t c) { return c < DC_COUNT ? DC_NAME[c] : DC_NAME[0]; }

// Bump when a message's shape changes in a way a client could trip over.
// Rides sync and /state so a bot can refuse a build it was not written for.
//   2: exact-match dispatch, rid/ack/nack, ev "sq", downed "cause", EVT_DAMAGE,
//      "rt" in the tick broadcast, ABW_* codes 2-9.
static constexpr int PROTO_VERSION = 2;

struct GameEvent {
  EvtType  type;
  uint8_t  pid;
  // Stamped by enqEvt() from g_evSeq, including for events the full queue
  // then drops. It never repeats or goes backwards on any one socket, and one
  // game event keeps one seq across every socket that sees it. A gap on one
  // socket is NOT proof of a drop: unicast and vision-culled events (downed,
  // col_fail, fire_spread, flood_washout, tun_taunt) spend a seq too. Drops
  // are counted exactly in g_evtDrops (/state "evtDrops").
  uint32_t seq;
  int16_t  q, r;
  uint8_t  res, amt;
  uint8_t  dawnF, dawnW, dawnLL;
  int8_t   dawnMP, dawnLLDelta;
  uint16_t dawnDay;
  uint8_t  dawnFth, dawnWth;
  int8_t   dawnExpD;
  int8_t   dawnAirD;    // bad air: -1 when a rest underground rolled the LL hit
  uint8_t  dawnWndMin, dawnWndMaj;
  // Bitmask of equipment slots whose daily *_cost could not be paid this dawn
  // (bit 0 = head .. bit 4 = vehicle). Their STAT_MP is dormant for the day.
  // Without this the Motorbike's "+5 MP" simply failed to appear and nothing
  // -- no event, no log, no UI state -- said the fuel had run out.
  uint8_t  dawnUnfuelled;
  uint8_t  actWhy;        // ABW_* — why an AO_BLOCKED action was refused
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
  uint8_t  tradeItem;     // caravan purchase (tradeTo == CARAVAN_PID): item bought; 0 = plain resource swap
  uint8_t  tradeItemQty;
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
  // Which board q/r refer to: 0 = G.map, 1 = G.tunnel. Every event that names
  // a hex needs this now that there are two boards -- without it the client
  // would patch a surface cell with a tunnel coordinate. Serialised as "dp"
  // and omitted when 0, so surface traffic is unchanged on the wire.
  uint8_t  depth;
};

static constexpr uint32_t TRADE_EXPIRE_MS = 30000;

// Player-to-player trades move resource tokens only; typed items only change
// hands at the caravan's shelf (car_buy — see network-msg-trade.hpp).
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
  uint8_t  depth;           // which board hexQ/hexR index: 0 = G.map, 1 = G.tunnel
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
// Indexed by terrain type.  Sized NUM_TERRAIN so the tunnel pool (14) fits --
// index.json only defines a subset; the rest stay count=0 and never fire.
static EncPoolInfo encPools[NUM_TERRAIN];

// POI encounter probability removed — encounters are now pre-placed
// at map generation time (one hex per encounter ID, guaranteed).
// See hex-map.hpp Phase 5.

// ── Loot table cache (parsed from /encounters/loot_tables.json at boot) ───────
// MAX_LOOT_TABLES must be >= the number of top-level tables in loot_tables.json
// (currently 34) — loadLootTables() in boot-assets.hpp silently stops parsing
// once it's full, so a table added past this cap just never loads.  Kept a few
// clear of the real count so adding one is a data edit, not a reflash.
static constexpr int MAX_LOOT_TABLES = 40;
// Likewise the per-table entry cap.  Overflowing THIS one is worse than a
// dropped table: loadLootTables() stops mid-array with `arr` parked on entries
// it never read, and the outer scan then hits `"qty": [` in one of them and
// registers a junk table called "qty".  urban_rare sits at 10.
static constexpr int LOOT_ENTRIES_MAX = 12;
struct LootEntry { uint8_t item; uint8_t qtyMin; uint8_t qtyMax; uint8_t weight; };
struct LootTable  { char name[20]; LootEntry entries[LOOT_ENTRIES_MAX]; uint8_t count; };
static LootTable* lootTables = nullptr;       // [MAX_LOOT_TABLES], PSRAM (allocPsramGlobals)
static uint8_t    lootTableCount = 0;

struct CheckResult { int r1, r2, skillVal, mods, total, dn; bool success; };

struct GameState {
  HexCell  (*map)[MAP_COLS];   // PSRAM: MAP_ROWS rows, allocated by allocPsramGlobals(); G.map[r][q] unchanged
  HexCell  (*tunnel)[TUN_COLS];  // PSRAM: the bunker tunnel board, G.tunnel[r][q] (tunnels.hpp)
  Player*  players;           // PSRAM: [MAX_PLAYERS], allocated by allocPsramGlobals(); G.players[i] unchanged
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
static constexpr size_t MAP_BYTES    = sizeof(HexCell) * MAP_ROWS * MAP_COLS;
static constexpr size_t TUNNEL_BYTES = sizeof(HexCell) * TUN_ROWS * TUN_COLS;

static GameState      G;

// ── SD Save / Load constants + structs ────────────────────────────────────────
static constexpr uint32_t SAVE_MAGIC   = 0xDEADC0DEul;
// v16: HexCell grew a tireTrack byte (see struct above), which changes
// MAP_BYTES — the raw G.map block saveGame()/tryLoadSave() read/write would
// misalign against an older save written with the smaller struct. No
// migration path; a v15 save is ignored and falls through to generateMap(),
// same one-shot-reset precedent as the v9→v10 and v14→v15 bumps.
// v17: the bunker tunnel system (tunnels.hpp). map.bin now carries the whole
// G.tunnel block between the map and the ground-items block, SaveHeader holds
// the hatch pairings, and SavePlayer holds depth/tq/tr. Same one-shot reset —
// a v16 save is ignored and the world (surface AND tunnels) regenerates.
// v18: INV_SLOTS_MAX went 12 -> 18 so slot-granting gear actually grants
// slots on a Mule (see the constant). SavePlayer's invType[]/invQty[] are
// that width, so the players.bin record size changed. Same one-shot reset —
// a v17 save is ignored and survivors respawn.
static constexpr uint8_t  SAVE_VERSION = 18;
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
  // v15: the caravan's consumable shelf (world-system.hpp). Asking prices are
  // not persisted — they come from items.cfg's "value" when serialised, so a
  // cfg edit re-prices the shelf on reboot.
  uint8_t  caravanStockItem[CARAVAN_STOCK_SLOTS];
  uint8_t  caravanStockQty[CARAVAN_STOCK_SLOTS];
  // v17: bunker tunnel hatch pairings (tunnels.hpp). The tunnel board itself
  // is a raw block in map.bin, but the surface<->shaft pairing is derived at
  // generation time and cannot be recovered from the two boards alone.
  BunkerHatch bunkerHatches[MAX_HATCHES];
  uint8_t     hatchCount;
};

struct __attribute__((packed)) SavePlayer {
  char     name[16];
  uint8_t  archetype;
  uint8_t  skills[NUM_SKILLS];
  int16_t  q, r;
  uint8_t  ll, food, water, radiation;
  uint8_t  inv[5];
  uint8_t  invType[INV_SLOTS_MAX];
  uint8_t  invQty[INV_SLOTS_MAX];
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
  uint8_t  depth;         // v17 — 0 surface, 1 tunnels
  int16_t  tq, tr;        // v17 — position on the tunnel board
  uint8_t  hatchIdx;      // v17
};

struct __attribute__((packed)) SaveGroundItem {
  int16_t q, r;
  uint8_t itemType;
  uint8_t qty;
};

static GameEvent*     pendingEvents = nullptr;   // [EVT_QUEUE_SIZE], PSRAM (allocPsramGlobals)
static int            pendingCount  = 0;
static portMUX_TYPE   evtMux        = portMUX_INITIALIZER_UNLOCKED;
static uint32_t       g_evSeq       = 0;   // last seq handed out by enqEvt()
static uint32_t       g_evtDrops    = 0;   // events lost to a full queue (/state)

// ── Boot / crash telemetry (/state "boot") ─────────────────────
// A board that crashed mid-run comes back looking exactly like one that was
// power-cycled on purpose. The reset reason says which; the core dump the
// panic handler leaves in the coredump partition says where.
static constexpr const char* MDNS_HOST = "k10";
static const char* g_resetReason = "UNKNOWN";
struct CrashInfo {
  bool     valid;          // a readable core dump is in flash
  char     task[16];       // task that faulted
  uint32_t pc;             // faulting PC
  uint32_t cause;          // Xtensa EXCCAUSE
  uint32_t vaddr;          // EXCVADDR
  uint8_t  depth;          // backtrace entries kept in bt[]
  uint32_t bt[8];
  char     elf[9];         // first 8 hex chars of the crashing build's ELF SHA
};
static CrashInfo g_crash = {};
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
// Every entry is also stamped with a mark in the chronicle's margin — a small
// 3x2-character pictogram of the thing that happened, set beside the words it
// belongs to. This enum is only the index; the art itself is BOOK_GLYPH in
// ui-screens.hpp. GLY_NONE leaves the gutter empty, which is what a new log
// line gets until someone picks it a mark.
enum K10Glyph : uint8_t {
  GLY_NONE = 0,
  GLY_DAWN, GLY_FORAGE, GLY_WATER, GLY_MEDIC, GLY_SALVAGE, GLY_SHELTER,
  GLY_CRAFT, GLY_SCOUT, GLY_REST, GLY_TRADE, GLY_CARAVAN, GLY_ARRIVE,
  GLY_DEPART, GLY_THRESHOLD, GLY_CLASH, GLY_LIGHT, GLY_HAUL, GLY_WOUND,
  GLY_DEATH, GLY_DOOM, GLY_FIRE, GLY_FLOOD, GLY_QUAKE, GLY_SETTLE,
  GLY_RAIN, GLY_STORM, GLY_CHEM, GLY_FOG,
  GLY_RAD, GLY_MEDAL,
  GLY_COUNT
};
// A few moments are too big for one line of handwriting, so the chronicle
// gives them a plate instead: a framed ASCII block, four rows deep, with the
// mark blown up beside two lines of figures. PLATE_NONE is an ordinary entry.
// The frames and the layout live in ui-screens.hpp (BOOK_PLATE).
enum K10Plate : uint8_t {
  PLATE_NONE = 0, PLATE_WOUND, PLATE_RAD, PLATE_HAUL, PLATE_AWARD, PLATE_COUNT
};
// Commendations. These are not milestones the firmware tracks — each one is
// pinned to a thing the game already announces, so nothing has to be counted
// or saved between reboots. Citation wording comes from the call site.
// Thresholds the plates read against. RAD_CRITICAL matches the auto-fail rung
// in survival_state.hpp, so the dose meter fills exactly as the check bites.
static constexpr uint8_t RAD_CRITICAL = 10;
static constexpr uint8_t HAUL_HEAVY   = 12;   // a haul worth a commendation
enum K10Award : uint8_t {
  AWD_SWEPT = 0,   // cleared an encounter out entire
  AWD_HEAVY,       // carried out a haul worth the walk
  AWD_OFF_SCENT,   // shook the Creeping Doom off the trail
  AWD_LONG_ODDS,   // won a check the numbers said was lost
  AWD_COUNT
};
struct K10LogEntry {
  char     text[48];
  uint32_t ms;
  uint16_t day;
  int8_t   who;
  int8_t   who2;
  uint8_t  tone;
  uint8_t  glyph;   // K10Glyph — the mark drawn in the margin
  uint8_t  plate;   // K10Plate — PLATE_NONE for an ordinary written line
  uint8_t  pv[5];   // plate figures; what they mean is per-kind (BOOK_PLATE)
};
static K10LogEntry* k10Log = nullptr;   // [K10_LOG_SIZE], PSRAM (allocPsramGlobals)
static uint8_t      k10LogHead  = 0;
static uint8_t      k10LogCount = 0;
static uint16_t     k10LogTotal = 0;   // entries ever set down — the page number
static portMUX_TYPE k10LogMux   = portMUX_INITIALIZER_UNLOCKED;

static uint8_t  k10Screen     = 1;
static uint8_t  k10ScreenLast = 255;
static bool     k10BtnBLast   = false;
// Set by checkGestureSwitch() when button B lands on a new screen, consumed
// by the display block below: an ordinary repaint pushes one frame, a switch
// runs the fade/jitter transition instead. Only a button press sets it, so a
// repaint forced by an upload or death takeover ending stays instant.
static bool     k10ScreenXition = false;
static volatile bool k10Dirty = true;  // set whenever game state changes

static uint32_t k10TeamScore   = 0;
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

// ── LED cue state (ui-leds.hpp) ─────────────────────────────────────────────
// One event cue at a time. Set from Core 1 (the game loop) by ledCue() and the
// ledFlash() compatibility shim; rendered and expired by updateLEDs() on the
// display loop. A cue is a colour plus a SHAPE (how it moves over time) and a
// SPAN (which lamps it touches) - see the cue catalogue in ui-leds.hpp.
// Byte-level tearing between the two cores is cosmetically irrelevant on a
// 3-lamp strip, so no lock is taken; g_cueStartMs is written LAST so a
// half-built cue can never render.
static volatile uint8_t  g_cueR = 0, g_cueG = 0, g_cueB = 0;
static volatile uint8_t  g_cueShape = 0, g_cueSpan = 0, g_cueReps = 1, g_cuePrio = 0;
static volatile uint16_t g_cueMs      = 0;   // total cue duration, ms
static volatile uint32_t g_cueStartMs = 0;   // 0 = no cue live
// Perish alarm (ui-leds.hpp): set by ledPerish() from the EVT_DOWNED handler,
// outranks both the event cue and the weather/time-of-day sky.
static volatile uint32_t g_ledPerishEndMs = 0;

// ── Dread snapshot ──────────────────────────────────────────────────────────
// The slow half of the LED story: everything the lamps say about the party's
// condition and about what is hunting it, as against the sky (time of day plus
// weather) they already told. Published once per game tick by publishDread()
// in world-system.hpp - which runs inside the G.mutex tickGame() already holds
// - and consumed lock-free by updateLEDs() at the ~10 Hz display rate.
//
// The indirection exists because ui-display.hpp (and so ui-leds.hpp) is
// included BEFORE world-system.hpp, so the LED code cannot name W.creepingDoom
// or W_hex at all. Publishing a flat byte struct also keeps the 10 Hz lamp
// path off G.mutex entirely: slow data, fast animation.
//
// Every field is pre-normalised to 0-255 "how bad is it" so that ui-leds.hpp
// holds presentation only, and none of the game's own scales (LL 7, food and
// water 6, radiation 10, awareness 100) leak into the lamp code.
struct DreadSnapshot {
  uint8_t tcLevel;      // threat-clock band, 0-4 (TC_THRESHOLD_A..D)
  uint8_t tcWeight;     // 0-255 smooth ramp across the whole clock
  uint8_t doomClose;    // 0 = far or unaware, 255 = standing on someone
  uint8_t doomAware;    // raw Creeping Doom awareness, 0-100
  uint8_t attrition;    // 0-255 party-wide LL shortfall
  uint8_t downed;       // survivors currently at LL 0
  uint8_t hunger;       // 0-255 worst food shortfall in the party
  uint8_t thirst;       // 0-255 worst water shortfall
  uint8_t radLoad;      // 0-255 worst radiation load
  uint8_t woundLoad;    // 0-255 weighted wounds (a major counts double)
  uint8_t fireClose;    // 0-255 nearest fire to anyone standing on the surface
  uint8_t connected;    // connected survivors
  uint8_t under;        // how many of them are down in the tunnels
  bool    allUnder;     // every connected survivor is below ground
  bool    encActive;    // someone is mid-encounter - the sky holds its breath
};
static volatile DreadSnapshot g_dread = {};

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
// 100 -> 160: the four bunker-tunnel placeholder tiles took data/ to 99/100,
// one slot off the cliff.  Past the cap boot-assets.hpp stops caching and the
// route silently serves 204, so art just vanishes with no error -- leave real
// headroom rather than discovering it as a missing tile.  The table is PSRAM
// (allocPsramGlobals), so 60 more slots costs ~4.7 KB of the 8 MB.
static const int MAX_IMG_CACHE = 160;
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

// Bunker tunnel system: the second hex board and everything that happens on
// it. Needs hex-map.hpp (pickVariant, DQ/DR, encPools) and is called back
// into by generateMap() through the forward declaration in hex-map.hpp.
#include "tunnels.hpp"

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
#include "network-reply.hpp"     // rid / ack / nack for the handlers below
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
  G.tunnel      = (HexCell(*)[TUN_COLS])    psramStaticAlloc(TUNNEL_BYTES);
  W_hex         = (HexDynamic(*)[MAP_COLS]) psramStaticAlloc(W_HEX_BYTES);
  pendingEvents = (GameEvent*)              psramStaticAlloc(sizeof(GameEvent) * EVT_QUEUE_SIZE);
  itemRegistry  = (ItemDef*)                psramStaticAlloc(sizeof(ItemDef)   * MAX_ITEMS);
  recipeRegistry= (RecipeDef*)              psramStaticAlloc(sizeof(RecipeDef) * MAX_RECIPES);
  imgCache      = (ImgFile*)                psramStaticAlloc(sizeof(ImgFile)   * MAX_IMG_CACHE);
  webFiles      = (WebFile*)                psramStaticAlloc(sizeof(WebFile)   * MAX_WEB_FILES);
  G.players     = (Player*)                 psramStaticAlloc(sizeof(Player)    * MAX_PLAYERS);
  lootTables    = (LootTable*)              psramStaticAlloc(sizeof(LootTable) * MAX_LOOT_TABLES);
  k10Log        = (K10LogEntry*)            psramStaticAlloc(sizeof(K10LogEntry) * K10_LOG_SIZE);
  g_knownNets   = (KnownNet*)               psramStaticAlloc(sizeof(KnownNet)  * WIFI_MAX_NETS);
  Log.notice("PSRAM globals: map=%u tunnel=%u whex=%u evq=%u items=%u recipes=%u img=%u web=%u "
             "players=%u loot=%u k10log=%u nets=%u B; heap %u->%uKB psram=%uKB",
             (unsigned)MAP_BYTES, (unsigned)TUNNEL_BYTES, (unsigned)W_HEX_BYTES,
             (unsigned)(sizeof(GameEvent) * EVT_QUEUE_SIZE), (unsigned)(sizeof(ItemDef) * MAX_ITEMS),
             (unsigned)(sizeof(RecipeDef) * MAX_RECIPES),
             (unsigned)(sizeof(ImgFile) * MAX_IMG_CACHE), (unsigned)(sizeof(WebFile) * MAX_WEB_FILES),
             (unsigned)(sizeof(Player) * MAX_PLAYERS), (unsigned)(sizeof(LootTable) * MAX_LOOT_TABLES),
             (unsigned)(sizeof(K10LogEntry) * K10_LOG_SIZE), (unsigned)(sizeof(KnownNet) * WIFI_MAX_NETS),
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
    g_resetReason = rs;
  }
  // The dump survives later clean resets, so it describes the LAST crash, not
  // necessarily this boot -- /state pairs it with the reset reason, and "elf"
  // says whether it came from the build that is running now.
  if (esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t cs;
    if (esp_core_dump_get_summary(&cs) == ESP_OK) {
      g_crash.valid = true;
      strlcpy(g_crash.task, cs.exc_task, sizeof(g_crash.task));
      g_crash.pc    = cs.exc_pc;
      g_crash.cause = cs.ex_info.exc_cause;
      g_crash.vaddr = cs.ex_info.exc_vaddr;
      g_crash.depth = (uint8_t)min((int)cs.exc_bt_info.depth, 8);
      for (int i = 0; i < g_crash.depth; i++) g_crash.bt[i] = cs.exc_bt_info.bt[i];
      strlcpy(g_crash.elf, (const char*)cs.app_elf_sha256, sizeof(g_crash.elf));
      Log.warning("last crash: task=%s pc=0x%08lx cause=%lu elf=%s",
                  g_crash.task, (unsigned long)g_crash.pc,
                  (unsigned long)g_crash.cause, g_crash.elf);
    }
  }

  // ── K10 hardware init (buttons, LEDs, audio) ─────────────────
  Log.notice("K10 hw init start");
  k10.begin();
  Log.notice("K10 hw init ok");
  loadK10Prefs();
  Log.notice("K10 prefs loaded: audioVol=%d ledBright=%d", (int)s_audioVol, (int)s_ledBright);

  // ── LovyanGFX display init ────────────────────────────────────
  // k10.begin() turns backlight off (XL9535 P0.0=LOW). Enable it via Wire.
  LOG_VERBOSE("I2C begin SDA=47 SCL=48");
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
  LOG_VERBOSE("XL9535 backlight enabled");
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

  LOG_VERBOSE("Effect table init");
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

  // 18 KB: was 24 KB until drainEvents()'s 6.4 KB snapshot[] moved to PSRAM,
  // so headroom is unchanged. Task stacks come out of internal heap; check
  // the "gameLoop wm: stack_free=" log line before trimming further.
  xTaskCreatePinnedToCore(gameLoopTask, "GameLoop", 18432, NULL, 2, NULL, 1);
  Log.notice("gameLoopTask spawned core=1 prio=2 stack=18KB");
  Log.notice("==== BOOT COMPLETE elapsed=%ums ====", (unsigned)(millis() - _bootT0));
}

void loop() {
  // Headroom matters here: cleanupClients(n) closes _clients.front() -- the
  // OLDEST socket -- whenever count() > n, and count() includes clients that
  // handleConnect has already rejected with {"t":"full"} but which have not
  // finished closing yet. At exactly MAX_PLAYERS, one transient extra
  // connection would evict the longest-seated player rather than the
  // newcomer. The +2 covers that overlap; handleConnect still enforces the
  // real cap of MAX_PLAYERS for anyone trying to join.
  ws.cleanupClients(MAX_PLAYERS + 2);
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
      // First boot after this feature landed: whatever single credential the
      // ESP32 already had in its own NVS becomes entry 0 of the known list.
      wifiStoreRemember(savedSsid, savedPass);
      wifiSweepBackoff = WIFI_SWEEP_MIN;
      k10ScreenLast = 255;  // force title redraw after WiFi splash would have disrupted it
      char buf[88];
      int blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}",
        WiFi.localIP().toString().c_str());
      ws.textAll(buf, (size_t)blen);
      broadcastWifiNets();
      Log.notice("NTP configTime(pool.ntp.org, time.nist.gov) called");
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    } else if (wst == WL_CONNECT_FAILED || wst == WL_NO_SSID_AVAIL ||
               now - bootWifiStartMs > BOOT_WIFI_TIMEOUT) {
      bootWifiPending = false;
      Log.warning("Boot STA FAIL ssid=%s status=%d elapsed=%ums",
                  savedSsid, (int)wst, (unsigned)(now - bootWifiStartMs));
      savedSsid[0] = '\0';
      // The last network didn't answer — we may simply be somewhere else.
      // Let the roaming sweep look for any other known network shortly.
      wifiNextSweepMs = now + 2000;
    }
  }

  // Roaming sweep: off-network but we know some. Skipped while an upload is
  // streaming (a scan would stall the very socket delivering it).
  if (g_knownCount > 0 && !wifiConnecting && !bootWifiPending &&
      (int32_t)(now - wifiNextSweepMs) >= 0 &&
      WiFi.status() != WL_CONNECTED && !UploadUI::isActive()) {
    wifiNextSweepMs = now + wifiSweepBackoff;  // the task refines this on exit
    wifiStartAutoJoin();
  }

  checkGestureSwitch();
  checkScoreAudio();

  bool screenChanged = (k10Screen != k10ScreenLast);
  k10ScreenLast = k10Screen;
  bool uploadActive = UploadUI::isActive();
  // A death outranks everything, including an upload in flight: the screen is
  // given over to the skull for DEATH_HOLD_MS and nothing else paints.
  bool deathActive = DeathUI::isActive();
  // Repaint faster while an upload streams or a death burns, so the byte
  // counter and the flames both animate.
  unsigned long screenInterval = (uploadActive || deathActive) ? 100UL
                                                               : (unsigned long)SCREEN_MS;
  if (screenChanged || k10Dirty || uploadActive || deathActive ||
      (now - lastScreenMs >= screenInterval)) {
    lastScreenMs = now;
    k10Dirty = false;
    if (deathActive) {
      drawDeathScreen();
      canvas.pushSprite(0, 0);
      k10ScreenXition = false;   // a takeover swallows the switch's animation
    } else if (uploadActive) {
      drawUploadScreen();
      canvas.pushSprite(0, 0);
      k10ScreenXition = false;
    } else if (k10ScreenXition) {
      // Renders and pushes every frame of the switch itself (ui-screens.hpp).
      k10ScreenXition = false;
      screenSwitchTransition();
    } else {
      drawActiveScreen();
      canvas.pushSprite(0, 0);
    }
    if (!uploadActive && !deathActive) k10ScreenLast = k10Screen;
    else k10ScreenLast = 255;   // force a repaint when the takeover ends
  }

  // One call owns the whole strip: updateLEDs() runs the perish alarm, the
  // lightning, the event cue, the dread layer and the time-of-day/weather sky
  // in a single pass and expires its own timers, so there is nothing left for
  // the display loop to arbitrate.
  updateLEDs();

  if (now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    LOG_VERBOSE("status: connected=%d tick=%lu freeHeap=%uKB",
                (int)G.connectedCount, (unsigned long)G.tickId,
                (unsigned)(ESP.getFreeHeap() / 1024));
  }
  delay(100);
}
