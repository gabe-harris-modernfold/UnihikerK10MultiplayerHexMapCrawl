#pragma once
// ── ecology.hpp — The Understory ────────────────────────────────────────────
// An algorithmic slime mould that lives only in PSRAM, plus the Wasteland
// Daisy. Design: docs/ecology-spec.md. Status: phases 1-5 of that spec.
//
// Every boot rolls a genome (or takes the pinned NVS seed), a species
// germinates from the scars the last one left in /save/scar.bin, and it grows
// in waves: forage -> fruiting bodies -> burst -> spores -> a larger wave. It
// feeds on the game's own in-memory data (footprints, tracks, terrain, fire,
// flood, the caravan's ruts, the weather, the board's vital signs) through
// the transforms in section 2 and never writes anything the game reads.
//
// Cosmetic, with two exceptions, both in section 7: a bloomed Wasteland Daisy
// bites for 1 LL (ecoBiteCheck), and the mould's blight -- a surface hex with
// visible growth on it holds no resource and grows none back (ecoBlightMap;
// the GameLoop's respawn pass does the eating). Each is behind its own NVS
// switch, eco/bite and eco/blight (default 1). With both off, or ECO_ENABLE=0
// in build_opt.h, the game is identical to one without this file.
//
// Layers, bottom up (spec "Architecture"):
//   1. TRANSFORMS   snapshot of G.map/W_hex -> per-hex food/barrier (ecoHex)
//   2. PHYSARUM     agents on a trail grid at 4x hex resolution
//   3. LIFECYCLE    colonies: spore -> germinate -> forage -> fruit -> burst
//   4. MEMBRANE     the `eco` wire message, /state, the LCD tint, the bloom
//                   bitset the GameLoop bites from, the density it blights from
//
// Concurrency: everything here belongs to the Eco task (core 1, prio 1, below
// GameLoop). Each tick it takes G.mutex once to copy its inputs, then works on
// private buffers. The only things other tasks touch are the PUBLISHED
// buffers, each double-buffered and swapped with a single index write:
// the wire message (sendSync), the packed scars (saveGame), the bloom bitset
// (movePlayer et al.), the per-hex density (drawMapScreen, the respawn pass's
// blight) and the hurt ring
// (written by the damage sites under G.mutex, drained under G.mutex).
//
// Included from Esp32HexMapCrawl.ino after tunnels.hpp: needs G/W_hex/DQ/DR/
// wrapQ/wrapR/hexDistWrap/enqEvt (hex-map.hpp, world-system.hpp), ledFlash/
// k10Play/k10LogAdd (ui-display.hpp) and `ws`. The hooks the earlier files
// call (ecoNoteHurt, ecoBiteCheck, ecoOnIgnite, ecoPublishedDensity,
// ecoLcdTint) are forward-declared in the .ino.

#ifndef ECO_ENABLE
#define ECO_ENABLE 1
#endif

#include <esp_heap_caps.h>

static const char SAVE_SCAR_F[] = "/save/scar.bin";

#if ECO_ENABLE

// ── 0. Sizes and tuning ──────────────────────────────────────────────────────
static constexpr uint32_t ECO_TICK_MS        = 5000;
static constexpr int      ECO_SUB            = 4;                      // sub-cells per hex side
static constexpr int      ECO_W              = MAP_COLS * ECO_SUB;     // 300
static constexpr int      ECO_H              = MAP_ROWS * ECO_SUB;     // 228
static constexpr int      ECO_XW             = ECO_W * 64;             // Q6 wrap
static constexpr int      ECO_YW             = ECO_H * 64;
static constexpr int      ECO_TILE           = 16;
static constexpr int      ECO_TX             = (ECO_W + ECO_TILE - 1) / ECO_TILE;   // 19
static constexpr int      ECO_TY             = (ECO_H + ECO_TILE - 1) / ECO_TILE;   // 15
static constexpr int      ECO_HEXES          = MAP_ROWS * MAP_COLS;    // 4275
static constexpr int      ECO_BLOOM_BYTES    = (ECO_HEXES + 7) / 8;    // 535
static constexpr int      ECO_SCAR_NIBBLES   = (ECO_HEXES + 1) / 2;    // 2138
static constexpr int      ECO_MAX_AGENTS     = 4096;
static constexpr int      ECO_MAX_COLONIES   = 16;
static constexpr int      ECO_MAX_BODIES     = 12;
static constexpr int      ECO_MAX_SPORES     = 32;
static constexpr int      ECO_MAX_DAISIES    = 48;
static constexpr int      ECO_MAX_FLIGHTS    = 24;
static constexpr int      ECO_HURT_RING      = 16;
static constexpr int      ECO_WIRE_CAP       = 16384;
static constexpr int      ECO_SCARMSG_CAP    = ECO_HEXES + 96;
static constexpr int      ECO_COVERAGE_CAP   = 25;     // % of land hexes with a visible vein: germination waits above this
static constexpr uint8_t  ECO_VISIBLE_DENS   = 4;      // density at or above which a hex "carries a vein"
// Trail units per density step: (max + 3*mean)/4 over the hex's 16 sub-cells,
// divided by this, capped at 15. 12 needed a mean trail of ~48 before a hex
// counted as veined and 4096 agents covered 3% of the land (mock harness,
// 2026-09-27); 6 puts a single well-run vein at the visible threshold.
static constexpr int      ECO_DENS_DIV       = 6;
static constexpr uint8_t  ECO_AGE_SCAR_TICKS = 60;     // ticks (5 min) of visible vein in one day earns a vein scar
static constexpr uint16_t ECO_FIRST_CAP      = 256;    // wave 1 agent cap per colony
static constexpr uint16_t ECO_COLONY_CAP_MAX = 1024;
static constexpr uint8_t  ECO_EDGE_T         = 20;     // mean trail in a shared border strip that joins two hexes
static constexpr uint8_t  ECO_TILE_T         = 6;      // a tile with no cell above this and no agent goes dormant
static constexpr uint8_t  ECO_FOOD_MIN       = 32;     // food at or above this feeds an agent
static constexpr uint8_t  ECO_SPAWN_ENERGY   = 200;
static constexpr int      ECO_SPORE_KEEPOUT  = 3;      // hexes: no spore this close to a connected survivor
static constexpr int      ECO_PLAYER_MAX     = MAX_PLAYERS;

// Dispersal vectors (genome `vector`)
static constexpr uint8_t ECO_VEC_WIND = 0, ECO_VEC_FOOT = 1, ECO_VEC_TIRE = 2, ECO_VEC_ASH = 3;
// Barrier levels (EcoHex.barrier)
static constexpr uint8_t ECO_B_HOSTILE = 160;   // agents avoid, may still enter
static constexpr uint8_t ECO_B_FLOOD   = 200;   // agents are pushed out
static constexpr uint8_t ECO_B_LETHAL  = 255;   // water, active fire: agents die
// Agent flags
static constexpr uint8_t ECO_AF_RETURN  = 0x02;
static constexpr uint8_t ECO_AF_SETTLED = 0x04;
// Colony stages
static constexpr uint8_t ECO_ST_FREE = 0, ECO_ST_GERM = 1, ECO_ST_FORAGE = 2,
                         ECO_ST_FORM = 3, ECO_ST_MATURE = 4;

// ── 1. Data ──────────────────────────────────────────────────────────────────
struct EcoHex {        // 4 bytes per hex, PSRAM
  uint8_t food;        // attractant, 0-255 (transforms)
  uint8_t barrier;     // 0 open .. ECO_B_LETHAL
  uint8_t age;         // ticks this hex has carried a visible vein since dawn (saturates)
  uint8_t scar;        // 0-15, from scar.bin; see "Scars"
};
struct EcoAgent {      // 8 bytes
  uint16_t x, y;       // sub-cell position, Q6
  uint8_t  heading;    // 0-255 = 0-360 deg
  uint8_t  colony;
  uint8_t  energy;
  uint8_t  flags;
};
struct EcoColony {
  uint8_t  stage, generation, nSites, seed;
  uint16_t timer, waveCap, agentCount;
  uint32_t stageTick, lastFoodTick;
  int16_t  originQ, originR;
  int16_t  siteQ[3], siteR[3];
};
struct EcoBody {       // fruiting body
  uint8_t used, q, r, colony, stage, timer, seed;
};
struct EcoSpore {
  uint8_t  used, generation;
  int16_t  q, r;
  uint16_t timer, waveCap;
};
struct EcoDaisy {
  uint8_t  used, q, r, stage, dawns, count, seed;
  uint32_t seededTick;
};
struct EcoFlight { int16_t fq, fr, tq, tr; };

struct EcoStyle { uint8_t sa, ra, dep; uint16_t sd, ss, dk; };
// Physarum parameter sets (after Jones 2010; angles in 0-255 units, sd and
// ss in Q6 sub-cells, dk = 4096*keep/9 for the 3x3 mean decay). The first
// eight are the distinct looks; 8-15 are the same looks with one knob moved.
static const EcoStyle ECO_STYLES[16] = {
  { 16, 32,  8,  9*64, 64, 418 },   // 0 cables: thick, sparse, decisive
  { 24, 48,  4,  5*64, 64, 400 },   // 1 lace: fine, short-sighted, quick to fade
  { 32, 24,  6,  7*64, 51, 410 },   // 2 reticulum: loops and cross-links
  { 12, 20, 10, 12*64, 77, 425 },   // 3 highways: long straight runs
  { 40, 60,  5,  6*64, 64, 405 },   // 4 wandering: broad sweeps, changeable
  { 20, 40,  6,  4*64, 45, 395 },   // 5 tight mesh: dense, slow
  { 10, 16,  9, 14*64, 83, 428 },   // 6 cords: fast, far-sensing, few branches
  { 28, 36,  4,  8*64, 70, 390 },   // 7 frayed: lots of tips, little persistence
  { 16, 32,  8,  9*64, 64, 400 },   // 8 cables, faster decay
  { 24, 48,  6,  5*64, 64, 400 },   // 9 lace, heavier deposit
  { 32, 24,  6,  7*64, 64, 410 },   // 10 reticulum, longer step
  { 18, 20, 10, 12*64, 77, 425 },   // 11 highways, wider sensing
  { 40, 60,  5,  6*64, 64, 415 },   // 12 wandering, slower decay
  { 20, 40,  6,  6*64, 45, 395 },   // 13 tight mesh, farther sensing
  { 10, 16,  7, 14*64, 83, 428 },   // 14 cords, lighter deposit
  { 28, 28,  4,  8*64, 70, 390 },   // 15 frayed, gentler turns
};

struct EcoSpecies {
  uint32_t seed;
  uint8_t  genome[16];
  uint8_t  style, tempo, vector, cycle, growth, fruit, scarLove, hue;
  int8_t   shyness;
  uint8_t  affinity[NUM_TERRAIN];   // 0 hostile, 1 neutral, 2 liked, 3 loved
  // decoded Physarum parameters
  uint8_t  sa, ra, dep;
  uint16_t sd, ss, dk;
  // decoded cycle
  uint16_t dormancy, germTicks, forageTicks, starveTicks, formTicks, holdTicks;
  uint16_t capMulQ8;      // wave cap multiplier x256 (1.4-1.8)
  uint8_t  sporesPerBody; // 2-5
  char     name[28];
};

// Species-name syllables. The colour word follows the hue byte so the name
// and the veins agree (hue-ordered, 16 steps round the wheel).
static const char* const ECO_WORD_A[32] = {
  "ASH","BONE","RUST","GLASS","SALT","TAR","IRON","MILK","SOOT","LIME","CHALK","BLOOD",
  "WAX","SILT","EMBER","DUST","MOTH","VEIL","MARROW","HOLLOW","BRINE","COAL","THORN","SPORE",
  "GRAVE","WIRE","LEAD","HUSK","STATIC","TALLOW","CINDER","FROST"
};
static const char* const ECO_WORD_B[32] = {
  "VESSEL","LATTICE","CROWN","FINGER","THREAD","ORGAN","TONGUE","CANDLE","LANTERN","ROOT",
  "WEB","CUP","CHOIR","MOUTH","EYE","HAND","BELL","KNOT","SHROUD","COMB","CRADLE","LOOM",
  "SPINE","WING","BLOOM","CHAIN","MASK","SEAM","HALO","ANCHOR","QUILL","GATE"
};
static const char* const ECO_COLOUR[16] = {
  "CRIMSON","RUST","AMBER","OCHRE","SAFFRON","LIME","JADE","VERDIGRIS",
  "TEAL","CERULEAN","SLATE","INDIGO","VIOLET","MAUVE","ROSE","CARMINE"
};

// PSRAM (ecoAllocPsram)
static uint8_t*    ecoTrail    = nullptr;   // [ECO_H][ECO_W]
static uint8_t*    ecoTrailB   = nullptr;   // diffusion scratch
static EcoAgent*   ecoAgents   = nullptr;   // [ECO_MAX_AGENTS]
static EcoHex    (*ecoHex)[MAP_COLS]    = nullptr;
static HexCell   (*ecoSnapMap)[MAP_COLS] = nullptr;   // input snapshot of G.map
static HexDynamic(*ecoSnapDyn)[MAP_COLS] = nullptr;   // input snapshot of W_hex
static char*       ecoWire[2]  = { nullptr, nullptr };
static char*       ecoScarMsg[2] = { nullptr, nullptr };
static uint8_t*    ecoPubDens[2] = { nullptr, nullptr };  // per hex: density (low nibble) | 0x40 bloom | 0x80 mature body
static uint8_t*    ecoHexDens  = nullptr;   // [ECO_HEXES] 0-15
static uint8_t*    ecoHexMask  = nullptr;   // [ECO_HEXES] 6-bit edge mask
static uint8_t*    ecoVisited  = nullptr;   // [ECO_MAX_COLONIES][ECO_BLOOM_BYTES]
static uint16_t*   ecoHexCount = nullptr;   // [ECO_HEXES] scratch for fruit-site picking
static uint8_t*    ecoScarPack[2] = { nullptr, nullptr };  // packed nibbles for scar.bin
static int16_t     ecoSin[256];             // Q10

// Small state (internal .bss, < 3 KB)
static EcoSpecies  ecoSp = {};
static EcoColony   ecoColonies[ECO_MAX_COLONIES] = {};
static EcoBody     ecoBodies[ECO_MAX_BODIES] = {};
static EcoSpore    ecoSpores[ECO_MAX_SPORES] = {};
static EcoDaisy    ecoDaisies[ECO_MAX_DAISIES] = {};
static EcoFlight   ecoFlights[ECO_MAX_FLIGHTS];
static uint8_t     ecoNFlights = 0;
static uint8_t     ecoTileAgent[ECO_TY][ECO_TX];
static uint8_t     ecoTileTrail[ECO_TY][ECO_TX];
static uint8_t     ecoBloom[2][ECO_BLOOM_BYTES] = {};
static int         ecoAgentCount = 0;
static uint32_t    ecoTickN = 0;
static uint32_t    ecoRng = 0x9E3779B9u;
static uint16_t    ecoWave = 0;
static uint8_t     ecoCoveragePct = 0;
static uint32_t    ecoBootsSeen = 0;
static uint16_t    ecoLastDay = 0xFFFF;
static bool        ecoBiteOn = true;
static bool        ecoBlightOn = true;
static uint32_t    ecoBlightEaten = 0;      // resources the blight has eaten since boot (GameLoop writes, /state reads)
static bool        ecoGenesisDone = false;
static uint32_t    ecoLastTickUs = 0;
static uint32_t    ecoScarredHexes = 0;

// Published indices (one pointer-sized write each; readers copy the index once)
static volatile uint8_t  ecoPubWire = 0, ecoPubScar = 0, ecoPubBloomIdx = 0, ecoPubDensIdx = 0, ecoPubPackIdx = 0;
static int               ecoWireLen[2] = { 0, 0 }, ecoScarLen[2] = { 0, 0 };
static volatile bool     ecoWireReady = false;    // false until the task has published once (and after a regen)
static volatile bool     ecoScarDirty = false;    // saveGame() writes scar.bin when set
static volatile bool     ecoRegenReq  = false;
static bool              ecoScarMsgStale = true;  // the scar wire message needs rebuilding
static bool              ecoScarMsgSend  = false; // ...and broadcasting this tick

// Hurt ring: written by the damage sites under G.mutex, drained under G.mutex.
static struct { uint8_t q, r; } ecoHurt[ECO_HURT_RING];
static uint8_t ecoHurtHead = 0, ecoHurtCount = 0;

// Snapshot extras
static uint8_t ecoSnapWeather = 0;
static uint16_t ecoSnapDay = 0;
static struct { int16_t q, r; } ecoSnapPlayers[ECO_PLAYER_MAX];
static int ecoSnapNPlayers = 0;

// /state telemetry
struct EcoStats {
  uint32_t tick; uint16_t wave; uint8_t colonies; uint16_t agents; uint8_t coverage;
  uint8_t fruiting; uint16_t scarred; uint8_t dSeeded, dGrowing, dBloomed; uint16_t spores; uint32_t tickUs;
};
static volatile EcoStats ecoStats = {};

// ── Helpers ──────────────────────────────────────────────────────────────────
static inline uint32_t ecoRand() {
  uint32_t x = ecoRng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; ecoRng = x ? x : 0x2545F491u; return x;
}
static inline int ecoRandN(int n) { return n > 0 ? (int)(ecoRand() % (uint32_t)n) : 0; }
static inline int ecoHexIdx(int q, int r) { return r * MAP_COLS + q; }
static inline bool ecoIsWater(uint8_t t) { return t == 5 || t == 11; }
static inline int ecoPopcount8(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static inline int16_t ecoCos(uint8_t h) { return ecoSin[(uint8_t)(h + 64)]; }
static inline uint8_t ecoLandTerrain(int q, int r) {
  uint8_t t = ecoSnapMap[r][q].terrain; return t < NUM_TERRAIN ? t : 0;
}
static inline bool ecoNearPlayer(int q, int r, int keepout) {
  for (int i = 0; i < ecoSnapNPlayers; i++)
    if (hexDistWrap(q, r, ecoSnapPlayers[i].q, ecoSnapPlayers[i].r) <= keepout) return true;
  return false;
}
static uint32_t ecoCrc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}
// Hue byte -> RGB (0xRRGGBB) at the given saturation/value, for the LCD tint.
static uint32_t ecoHsv(uint8_t hue, uint8_t sat, uint8_t val) {
  int h = hue * 6;                 // 0..1530
  int region = h >> 8, rem = h & 255;
  int p = val * (255 - sat) / 255;
  int q = val * (255 - sat * rem / 255) / 255;
  int t = val * (255 - sat * (255 - rem) / 255) / 255;
  int r, g, b;
  switch (region) {
    case 0:  r = val; g = t;   b = p;   break;
    case 1:  r = q;   g = val; b = p;   break;
    case 2:  r = p;   g = val; b = t;   break;
    case 3:  r = p;   g = q;   b = val; break;
    case 4:  r = t;   g = p;   b = val; break;
    default: r = val; g = p;   b = q;   break;
  }
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ── Allocation (called from allocPsramGlobals) ───────────────────────────────
static void ecoAllocPsram() {
  ecoTrail    = (uint8_t*)psramStaticAlloc((size_t)ECO_H * ECO_W);
  ecoTrailB   = (uint8_t*)psramStaticAlloc((size_t)ECO_H * ECO_W);
  ecoAgents   = (EcoAgent*)psramStaticAlloc(sizeof(EcoAgent) * ECO_MAX_AGENTS);
  ecoHex      = (EcoHex(*)[MAP_COLS])psramStaticAlloc(sizeof(EcoHex) * ECO_HEXES);
  ecoSnapMap  = (HexCell(*)[MAP_COLS])psramStaticAlloc(MAP_BYTES);
  ecoSnapDyn  = (HexDynamic(*)[MAP_COLS])psramStaticAlloc(W_HEX_BYTES);
  for (int i = 0; i < 2; i++) {
    ecoWire[i]     = (char*)psramStaticAlloc(ECO_WIRE_CAP);
    ecoScarMsg[i]  = (char*)psramStaticAlloc(ECO_SCARMSG_CAP);
    ecoPubDens[i]  = (uint8_t*)psramStaticAlloc(ECO_HEXES);
    ecoScarPack[i] = (uint8_t*)psramStaticAlloc(ECO_SCAR_NIBBLES);
  }
  ecoHexDens  = (uint8_t*)psramStaticAlloc(ECO_HEXES);
  ecoHexMask  = (uint8_t*)psramStaticAlloc(ECO_HEXES);
  ecoVisited  = (uint8_t*)psramStaticAlloc((size_t)ECO_MAX_COLONIES * ECO_BLOOM_BYTES);
  ecoHexCount = (uint16_t*)psramStaticAlloc(sizeof(uint16_t) * ECO_HEXES);
  for (int i = 0; i < 256; i++) ecoSin[i] = (int16_t)lroundf(sinf((float)i * 6.2831853f / 256.0f) * 1024.0f);
  Log.notice("eco: PSRAM trail=%u agents=%u hex=%u snap=%u wire=%u B",
             (unsigned)(2 * ECO_H * ECO_W), (unsigned)(sizeof(EcoAgent) * ECO_MAX_AGENTS),
             (unsigned)(sizeof(EcoHex) * ECO_HEXES), (unsigned)(MAP_BYTES + W_HEX_BYTES),
             (unsigned)(2 * ECO_WIRE_CAP));
}

// ── NVS: eco/seed (pinned genome seed, 0 = roll), eco/bite, eco/blight ──────
static uint32_t ecoPrefSeed() {
  Preferences p; p.begin("eco", true);
  uint32_t s = p.getUInt("seed", 0);
  ecoBiteOn   = p.getUChar("bite", 1) != 0;
  ecoBlightOn = p.getUChar("blight", 1) != 0;
  p.end();
  return s;
}
// seed / bite / blight < 0 leave that key alone. The seed applies at the next
// genesis (boot or regen); the two switches apply at once.
static void ecoSetPrefs(long seed, int bite, int blight) {
  Preferences p; p.begin("eco", false);
  if (seed >= 0) p.putUInt("seed", (uint32_t)seed);
  if (bite >= 0) { p.putUChar("bite", bite ? 1 : 0); ecoBiteOn = bite != 0; }
  if (blight >= 0) { p.putUChar("blight", blight ? 1 : 0); ecoBlightOn = blight != 0; }
  p.end();
  Log.notice("eco prefs: seed=%u bite=%d blight=%d", (unsigned)(seed >= 0 ? seed : 0),
             (int)ecoBiteOn, (int)ecoBlightOn);
}

// ── 2. Genome ────────────────────────────────────────────────────────────────
static void ecoDecodeGenome(uint32_t seed) {
  EcoSpecies& s = ecoSp;
  memset(&s, 0, sizeof(s));
  s.seed = seed;
  uint32_t x = seed ? seed : 0xA511E9B3u;
  for (int i = 0; i < 16; i++) {   // 16 bytes, expanded from the 32-bit seed so a pinned seed replays
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s.genome[i] = (uint8_t)(x >> ((i & 3) * 8));
  }
  const uint8_t* g = s.genome;
  s.style    = g[0] & 15;
  s.tempo    = g[0] >> 4;
  uint8_t jitter = g[1];
  uint32_t aff = (uint32_t)g[2] | ((uint32_t)g[3] << 8) | ((uint32_t)g[4] << 16) | ((uint32_t)g[5] << 24);
  for (int t = 0; t < NUM_TERRAIN; t++) s.affinity[t] = (uint8_t)((aff >> (t * 2)) & 3);
  s.affinity[5] = 0; s.affinity[11] = 0;         // water is forced hostile (and lethal below)
  s.affinity[10] = (s.affinity[10] == 3) ? 2 : s.affinity[10];   // the crater is never loved
  s.vector   = g[6] & 3;
  s.cycle    = g[6] >> 4;
  s.growth   = g[7] & 15;
  s.fruit    = g[7] >> 4;
  s.shyness  = (int8_t)((int)(g[8] & 15) - 8);   // -8 drawn to fresh tracks .. +7 steers away
  s.scarLove = g[8] >> 4;
  s.hue      = g[9];

  const EcoStyle& st = ECO_STYLES[s.style];
  int sa = (int)st.sa + ((jitter & 3) - 1) * 2;
  int sd = (int)st.sd + (((jitter >> 2) & 3) - 1) * 64;
  int ss = (int)st.ss + (((jitter >> 4) & 3) - 1) * 6;
  int dp = (int)st.dep + (((jitter >> 6) & 3) - 1);
  s.sa  = (uint8_t)constrain(sa, 6, 60);
  s.ra  = st.ra;
  s.sd  = (uint16_t)constrain(sd, 3 * 64, 16 * 64);
  s.ss  = (uint16_t)constrain(ss, 32, 96);
  s.dep = (uint8_t)constrain(dp, 2, 14);
  s.dk  = st.dk;

  // Cycle timings in eco ticks (5 s): dormancy 1-5 min, forage 15-35 min,
  // starvation patience 2-5 min, fruit forming ~1-2 min, mature hold 2-4 min.
  int c = s.cycle;
  s.dormancy    = (uint16_t)(12 + c * 3);
  s.germTicks   = 12;
  s.forageTicks = (uint16_t)(180 + c * 16);
  s.starveTicks = (uint16_t)(24 + c * 4);
  s.formTicks   = (uint16_t)(12 + c);
  s.holdTicks   = (uint16_t)(24 + c * 2);
  s.capMulQ8    = (uint16_t)(358 + (s.growth * 103) / 15);   // 1.4 .. 1.8 (x256)
  s.sporesPerBody = (uint8_t)(2 + (s.growth >> 2));           // 2..5

  snprintf(s.name, sizeof(s.name), "%s-%s %s",
           ECO_WORD_A[g[10] & 31], ECO_WORD_B[g[11] & 31], ECO_COLOUR[s.hue >> 4]);
}

static const char* ecoName() { return ecoSp.name[0] ? ecoSp.name : "nothing yet"; }
static uint32_t ecoLcdTint(uint32_t base, uint8_t dens) {
  // Blend a terrain colour toward the species hue by density (0-15): slight.
  uint32_t hue = ecoHsv(ecoSp.hue, 200, 255);
  int w = (int)dens * 10;   // 0..150 of 255
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int b = (int)((base >> sh) & 255), h = (int)((hue >> sh) & 255);
    out |= (uint32_t)((b * (255 - w) + h * w) / 255) << sh;
  }
  return out;
}
static uint32_t ecoAccentRgb() { return ecoHsv((uint8_t)(ecoSp.hue + 28), 160, 255); }

// ── 3. Scars: /save/scar.bin ─────────────────────────────────────────────────
// uint32 magic 'SCAR' | uint8 version 1 | uint8 cols, rows | uint32 bootsSeen |
// uint8 scar[rows*cols/2] packed nibbles row-major | uint32 crc32 over all above.
static constexpr uint32_t ECO_SCAR_MAGIC = 0x52414353u;   // "SCAR"
struct __attribute__((packed)) EcoScarHdr { uint32_t magic; uint8_t version, cols, rows; uint32_t bootsSeen; };

static void ecoPackScars(uint8_t* out) {
  memset(out, 0, ECO_SCAR_NIBBLES);
  ecoScarredHexes = 0;
  for (int i = 0; i < ECO_HEXES; i++) {
    uint8_t v = ecoHex[i / MAP_COLS][i % MAP_COLS].scar & 15;
    if (v) ecoScarredHexes++;
    out[i >> 1] |= (i & 1) ? (uint8_t)(v << 4) : v;
  }
}
// Eco task only: repack, publish, and flag the save.
static void ecoPublishScars() {
  uint8_t next = (uint8_t)(ecoPubPackIdx ^ 1);
  ecoPackScars(ecoScarPack[next]);
  ecoPubPackIdx = next;
  ecoScarDirty    = true;
  ecoScarMsgStale = true;
}
// Called inside saveGame()'s G.mutex hold: one SD write on the serialised save path.
static void ecoSaveScarsIfDirty() {
  if (!ecoScarDirty || !ecoScarPack[0]) return;
  ecoScarDirty = false;
  const uint8_t* pack = ecoScarPack[ecoPubPackIdx];
  File f = SD.open(SAVE_SCAR_F, FILE_WRITE);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_SCAR_F); return; }
  EcoScarHdr h = { ECO_SCAR_MAGIC, 1, (uint8_t)MAP_COLS, (uint8_t)MAP_ROWS, ecoBootsSeen };
  uint8_t tmp[sizeof(h) + ECO_SCAR_NIBBLES];   // header then nibbles; the crc covers both
  memcpy(tmp, &h, sizeof(h)); memcpy(tmp + sizeof(h), pack, ECO_SCAR_NIBBLES);
  uint32_t crc = ecoCrc32(tmp, sizeof(tmp));
  size_t n = f.write(tmp, sizeof(tmp));
  n += f.write((const uint8_t*)&crc, 4);
  f.close();
  Log.notice("SD WRITE: %s bytes=%u scarred=%u", SAVE_SCAR_F, (unsigned)n, (unsigned)ecoScarredHexes);
}
// Boot, after tryLoadSave(): missing file, bad magic/version/size or CRC -> no scars, no error.
static void ecoLoadScars() {
  for (int r = 0; r < MAP_ROWS; r++) for (int q = 0; q < MAP_COLS; q++) ecoHex[r][q].scar = 0;
  ecoBootsSeen = 0;
  if (!SD.exists(SAVE_SCAR_F)) { Log.notice("No %s: fresh ground", SAVE_SCAR_F); return; }
  File f = SD.open(SAVE_SCAR_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_SCAR_F); return; }
  uint8_t tmp[sizeof(EcoScarHdr) + ECO_SCAR_NIBBLES];
  uint32_t crc = 0;
  bool ok = f.read(tmp, sizeof(tmp)) == sizeof(tmp) && f.read((uint8_t*)&crc, 4) == 4;
  f.close();
  EcoScarHdr h; memcpy(&h, tmp, sizeof(h));
  if (!ok || h.magic != ECO_SCAR_MAGIC || h.version != 1 || h.cols != MAP_COLS || h.rows != MAP_ROWS ||
      crc != ecoCrc32(tmp, sizeof(tmp))) {
    Log.warning("%s unreadable or stale - ignoring", SAVE_SCAR_F);
    return;
  }
  const uint8_t* pack = tmp + sizeof(h);
  int n = 0;
  for (int i = 0; i < ECO_HEXES; i++) {
    uint8_t v = (i & 1) ? (pack[i >> 1] >> 4) : (pack[i >> 1] & 15);
    ecoHex[i / MAP_COLS][i % MAP_COLS].scar = v;
    if (v) n++;
  }
  ecoBootsSeen = h.bootsSeen;
  Log.notice("SD READ: %s scarred=%d bootsSeen=%u", SAVE_SCAR_F, n, (unsigned)ecoBootsSeen);
}
// New land (no save at boot, or a regen): the scars belonged to the old land.
// The file goes wherever the caller already holds the SD (setup(), or the
// regen handler under G.mutex); the RAM copy is the Eco task's to clear.
static void ecoDiscardScarFile() {
  if (SD.exists(SAVE_SCAR_F)) { SD.remove(SAVE_SCAR_F); Log.notice("Removed %s", SAVE_SCAR_F); }
}
static void ecoClearScarsRam() {
  if (ecoHex) for (int r = 0; r < MAP_ROWS; r++) for (int q = 0; q < MAP_COLS; q++) ecoHex[r][q].scar = 0;
  ecoBootsSeen = 0;
}

// ── 4. Transforms (layer 1) ──────────────────────────────────────────────────
static void ecoWipeBlock(int q, int r, bool halve) {
  for (int y = r * ECO_SUB; y < (r + 1) * ECO_SUB; y++) {
    uint8_t* row = ecoTrail + (size_t)y * ECO_W + q * ECO_SUB;
    for (int x = 0; x < ECO_SUB; x++) row[x] = halve ? (uint8_t)(row[x] >> 1) : 0;
  }
}
static void ecoTransforms() {
  const int shy = ecoSp.shyness;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      const HexCell&    c = ecoSnapMap[r][q];
      const HexDynamic& d = ecoSnapDyn[r][q];
      EcoHex& e = ecoHex[r][q];
      uint8_t t = c.terrain < NUM_TERRAIN ? c.terrain : 0;
      int food = 0, barrier = 0;
      if (ecoIsWater(t)) {
        barrier = ECO_B_LETHAL;                                  // T2: water
      } else {
        switch (ecoSp.affinity[t]) {                             // T2: substrate
          case 0:  barrier = ECO_B_HOSTILE; break;
          case 1:  food = 8;  break;
          case 2:  food = 40; break;
          default: food = 80; break;
        }
        food += ecoPopcount8(c.footprints) * 12;                 // T1: footfall (persistent)
        if (d.track) {                                           // T1: fresh tracks, signed by shyness
          if (shy <= 0) food += (int)d.track * (1 - shy) / 8;
          else          food -= (int)d.track * shy / 8;
        }
        if (c.tireTrack) food += 16;                             // T5: ruts
        if (ecoSp.vector == ECO_VEC_ASH && t == 1) food += 40;   // T3: ash is liked substrate
        food += (int)ecoSp.scarLove * (int)e.scar / 2;           // T8: scar memory
        if (d.flood) { barrier = ECO_B_FLOOD; ecoWipeBlock(q, r, true); }   // T4: wash
      }
      if (d.fire) { barrier = ECO_B_LETHAL; ecoWipeBlock(q, r, false); }    // T3: burn
      e.food    = (uint8_t)constrain(food, 0, 255);
      e.barrier = (uint8_t)barrier;
    }
  }
}
// T7: vital signs. More clients / less internal heap -> faster, +-25%.
static int ecoSubsteps() {
  int base = 4 + (int)ecoSp.tempo / 2;                          // 4..11
  int heapKB = (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  int heapFrac = constrain((heapKB - 60) * 100 / 120, 0, 100);   // 60 KB -> 0, 180 KB -> 100
  int clients = constrain(ecoSnapNPlayers * 100 / MAX_SEATED, 0, 100);
  int rate = 100 + (clients * 25) / 100 - (heapFrac * 25) / 100;  // 75..125
  return max(1, (base * rate + 50) / 100);
}

// ── 5. Physarum (layer 2) ────────────────────────────────────────────────────
static inline void ecoMarkTile(int x, int y) {
  int tx = x / ECO_TILE, ty = y / ECO_TILE;
  for (int dy = -1; dy <= 1; dy++) {
    int yy = ty + dy; if (yy < 0) yy += ECO_TY; else if (yy >= ECO_TY) yy -= ECO_TY;
    for (int dx = -1; dx <= 1; dx++) {
      int xx = tx + dx; if (xx < 0) xx += ECO_TX; else if (xx >= ECO_TX) xx -= ECO_TX;
      ecoTileAgent[yy][xx] = 1;
    }
  }
}
static inline int ecoSense(int x, int y, uint8_t h) {
  int px = x + (((int)ecoSp.sd * ecoCos(h)) >> 10);
  int py = y + (((int)ecoSp.sd * ecoSin[h]) >> 10);
  if (px < 0) px += ECO_XW; else if (px >= ECO_XW) px -= ECO_XW;
  if (py < 0) py += ECO_YW; else if (py >= ECO_YW) py -= ECO_YW;
  int sx = px >> 6, sy = py >> 6;
  const EcoHex& e = ecoHex[sy >> 2][sx >> 2];
  return (int)ecoTrail[(size_t)sy * ECO_W + sx] + (int)e.food - (int)e.barrier;
}
static inline void ecoDeposit(int x, int y, int amt) {
  uint8_t* c = ecoTrail + (size_t)(y >> 6) * ECO_W + (x >> 6);
  int v = (int)*c + amt; *c = (uint8_t)(v > 255 ? 255 : v);
}
static void ecoKillAgent(int i) {
  EcoAgent& a = ecoAgents[i];
  if (a.colony < ECO_MAX_COLONIES && ecoColonies[a.colony].agentCount) ecoColonies[a.colony].agentCount--;
  ecoAgents[i] = ecoAgents[ecoAgentCount - 1];
  ecoAgentCount--;
}
static bool ecoSpawnAgent(uint8_t colony, int x, int y, uint8_t heading, uint8_t energy, uint8_t flags) {
  if (ecoAgentCount >= ECO_MAX_AGENTS) return false;
  EcoAgent& a = ecoAgents[ecoAgentCount++];
  a.x = (uint16_t)x; a.y = (uint16_t)y; a.heading = heading; a.colony = colony; a.energy = energy; a.flags = flags;
  ecoColonies[colony].agentCount++;
  return true;
}
// Turn `h` toward the wrapped delta (dx, dy) by `by`. Cross product sign says which way.
static inline uint8_t ecoTurnToward(uint8_t h, int dx, int dy, uint8_t by) {
  int cross = dx * (int)ecoSin[h] - dy * (int)ecoCos(h);
  return cross > 0 ? (uint8_t)(h - by) : (uint8_t)(h + by);
}
static inline void ecoWrapDelta(int& dx, int& dy) {
  if (dx >  ECO_XW / 2) dx -= ECO_XW; else if (dx < -ECO_XW / 2) dx += ECO_XW;
  if (dy >  ECO_YW / 2) dy -= ECO_YW; else if (dy < -ECO_YW / 2) dy += ECO_YW;
}

static void ecoAgentStep() {
  const uint8_t sa = ecoSp.sa, ra = ecoSp.ra, dep = ecoSp.dep;
  const int ss = ecoSp.ss;
  for (int i = 0; i < ecoAgentCount; ) {
    EcoAgent& a = ecoAgents[i];
    EcoColony& col = ecoColonies[a.colony];
    int x = a.x, y = a.y;
    const EcoHex& here = ecoHex[y >> 8][x >> 8];
    if (here.barrier == ECO_B_LETHAL || col.stage == ECO_ST_FREE) { ecoKillAgent(i); continue; }
    uint8_t h = a.heading;
    if (here.barrier == ECO_B_FLOOD) { h = (uint8_t)(h + 128); a.energy = (uint8_t)(a.energy > 2 ? a.energy - 2 : 0); }

    bool settled = false;
    if (a.flags & ECO_AF_RETURN) {
      // Fruit stage: drain toward the colony's nearest fruiting site.
      int best = 0x7FFFFFFF, bdx = 0, bdy = 0;
      for (int s = 0; s < col.nSites; s++) {
        int dx = col.siteQ[s] * 256 + 128 - x, dy = col.siteR[s] * 256 + 128 - y;
        ecoWrapDelta(dx, dy);
        int d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; bdx = dx; bdy = dy; }
      }
      if (col.nSites && best <= (3 * 64) * (3 * 64)) {
        settled = true;
        a.flags |= ECO_AF_SETTLED;
        ecoDeposit(x, y, dep * 2);
        ecoMarkTile(x >> 6, y >> 6);
        i++;
        continue;
      }
      if (col.nSites && (ecoRand() & 1)) h = ecoTurnToward(h, bdx, bdy, ra);
      // Tips far from any body wither: the lace withdraws.
      int drain = (best > (24 * 64) * (24 * 64)) ? 8 : 1;
      if (a.energy <= drain) { ecoKillAgent(i); continue; }
      a.energy = (uint8_t)(a.energy - drain);
    }
    if (!settled) {
      int vC = ecoSense(x, y, h), vL = ecoSense(x, y, (uint8_t)(h - sa)), vR = ecoSense(x, y, (uint8_t)(h + sa));
      if (vC >= vL && vC >= vR) { /* straight on */ }
      else if (vL > vR)          h = (uint8_t)(h - ra);
      else if (vR > vL)          h = (uint8_t)(h + ra);
      else                       h = (ecoRand() & 1) ? (uint8_t)(h + ra) : (uint8_t)(h - ra);
    }
    int nx = x + ((ss * ecoCos(h)) >> 10);
    int ny = y + ((ss * ecoSin[h]) >> 10);
    if (nx < 0) nx += ECO_XW; else if (nx >= ECO_XW) nx -= ECO_XW;
    if (ny < 0) ny += ECO_YW; else if (ny >= ECO_YW) ny -= ECO_YW;
    const EcoHex& there = ecoHex[ny >> 8][nx >> 8];
    if (there.barrier == ECO_B_LETHAL) {
      // Water or fire ahead: turn back with a random swing rather than step
      // in. Standing in one (the fire spread over the vein, the flood rose
      // under it) is what kills, at the top of the loop. Jones's agents do
      // not enter blocked cells either; dying on contact emptied whole
      // colonies into the rivers within a dozen ticks (mock, 2026-09-27).
      h = (uint8_t)(h + 128 + ecoRandN(65) - 32);
    } else if (there.barrier >= ECO_B_HOSTILE && (ecoRand() & 1)) {
      h = (uint8_t)(h + 128);           // turn back rather than enter
    } else {
      x = nx; y = ny;
    }
    a.x = (uint16_t)x; a.y = (uint16_t)y; a.heading = h;
    ecoDeposit(x, y, dep);
    ecoMarkTile(x >> 6, y >> 6);

    // Feeding, and (forage only) budding at a full tip.
    const EcoHex& e2 = ecoHex[y >> 8][x >> 8];
    if (e2.food >= ECO_FOOD_MIN) {
      int en = (int)a.energy + e2.food / 8;
      a.energy = (uint8_t)(en > 255 ? 255 : en);
      int hi = ecoHexIdx(x >> 8, y >> 8);
      uint8_t* vis = ecoVisited + (size_t)a.colony * ECO_BLOOM_BYTES;
      if (!((vis[hi >> 3] >> (hi & 7)) & 1)) { vis[hi >> 3] |= (uint8_t)(1 << (hi & 7)); col.lastFoodTick = ecoTickN; }
    } else if (a.energy) {
      a.energy--;
    }
    if (col.stage == ECO_ST_FORAGE && a.energy >= ECO_SPAWN_ENERGY &&
        col.agentCount < col.waveCap && ecoAgentCount < ECO_MAX_AGENTS) {
      uint8_t half = (uint8_t)(a.energy / 2);
      a.energy = half;
      uint8_t ch = (uint8_t)(h + (int)(ecoRand() % 64) - 32);
      ecoSpawnAgent(a.colony, x, y, ch, half, 0);
    }
    i++;
  }
}

// 3x3 mean, decayed, on active tiles only; a tile with no agents and no
// trail left above ECO_TILE_T is zeroed and goes dormant.
static void ecoDiffuse() {
  const int dk = ecoSp.dk;
  for (int ty = 0; ty < ECO_TY; ty++) {
    for (int tx = 0; tx < ECO_TX; tx++) {
      if (!ecoTileAgent[ty][tx] && !ecoTileTrail[ty][tx]) continue;
      int x0 = tx * ECO_TILE, y0 = ty * ECO_TILE;
      int x1 = min(x0 + ECO_TILE, ECO_W), y1 = min(y0 + ECO_TILE, ECO_H);
      int tileMax = 0;
      for (int y = y0; y < y1; y++) {
        int ym = y ? y - 1 : ECO_H - 1, yp = (y + 1 == ECO_H) ? 0 : y + 1;
        const uint8_t* rm = ecoTrail + (size_t)ym * ECO_W;
        const uint8_t* r0 = ecoTrail + (size_t)y  * ECO_W;
        const uint8_t* rp = ecoTrail + (size_t)yp * ECO_W;
        uint8_t* out = ecoTrailB + (size_t)y * ECO_W;
        for (int x = x0; x < x1; x++) {
          int xm = x ? x - 1 : ECO_W - 1, xp = (x + 1 == ECO_W) ? 0 : x + 1;
          int sum = rm[xm] + rm[x] + rm[xp] + r0[xm] + r0[x] + r0[xp] + rp[xm] + rp[x] + rp[xp];
          int v = (sum * dk) >> 12;
          out[x] = (uint8_t)v;
          if (v > tileMax) tileMax = v;
        }
      }
      bool keep = ecoTileAgent[ty][tx] || tileMax >= ECO_TILE_T;
      for (int y = y0; y < y1; y++) {
        uint8_t* dst = ecoTrail + (size_t)y * ECO_W + x0;
        if (keep) memcpy(dst, ecoTrailB + (size_t)y * ECO_W + x0, x1 - x0);
        else      memset(dst, 0, x1 - x0);
      }
      ecoTileTrail[ty][tx] = keep ? 1 : 0;
    }
  }
  memset(ecoTileAgent, 0, sizeof(ecoTileAgent));
}

// ── 6. Lifecycle (layer 3) ───────────────────────────────────────────────────
static int ecoFreeColony() { for (int i = 0; i < ECO_MAX_COLONIES; i++) if (ecoColonies[i].stage == ECO_ST_FREE) return i; return -1; }
static int ecoFreeBody()   { for (int i = 0; i < ECO_MAX_BODIES;   i++) if (!ecoBodies[i].used) return i; return -1; }
static int ecoFreeSpore()  { for (int i = 0; i < ECO_MAX_SPORES;   i++) if (!ecoSpores[i].used) return i; return -1; }

static bool ecoSporeOk(int q, int r) {
  uint8_t t = ecoLandTerrain(q, r);
  if (ecoIsWater(t)) return false;
  if (ecoSnapDyn[r][q].fire) return false;
  if (ecoNearPlayer(q, r, ECO_SPORE_KEEPOUT)) return false;
  return true;
}
static bool ecoPlaceSpore(int q, int r, uint8_t generation, uint16_t waveCap) {
  int s = ecoFreeSpore();
  if (s < 0 || !ecoSporeOk(q, r)) return false;
  EcoSpore& sp = ecoSpores[s];
  sp.used = 1; sp.q = (int16_t)q; sp.r = (int16_t)r; sp.generation = generation; sp.waveCap = waveCap;
  int d = ecoSp.dormancy;
  sp.timer = (uint16_t)max(6, d + ecoRandN(d / 2 + 1) - d / 4);
  return true;
}
static uint8_t ecoWindDir() {
  // The squall line walks +q (data/storm-field.js): mostly east, a little
  // north-east or south-east. In calm weather the spores just drift.
  if (ecoSnapWeather == WEATHER_RAIN || ecoSnapWeather == WEATHER_STORM || ecoSnapWeather == WEATHER_CHEM) {
    int r = ecoRandN(10);
    return r < 6 ? 0 : (r < 8 ? 1 : 5);
  }
  return (uint8_t)ecoRandN(6);
}
static void ecoNoteFlight(int fq, int fr, int tq, int tr) {
  if (ecoNFlights >= ECO_MAX_FLIGHTS) return;
  ecoFlights[ecoNFlights++] = { (int16_t)fq, (int16_t)fr, (int16_t)tq, (int16_t)tr };
}
static bool ecoDisperseWind(int q, int r, uint8_t gen, uint16_t cap) {
  for (int attempt = 0; attempt < 4; attempt++) {
    int d = ecoWindDir(), dist = 4 + ecoRandN(7);
    int tq = wrapQ(q + DQ[d] * dist + ecoRandN(3) - 1), tr = wrapR(r + DR[d] * dist + ecoRandN(3) - 1);
    if (ecoPlaceSpore(tq, tr, gen, cap)) { ecoNoteFlight(q, r, tq, tr); return true; }
  }
  return false;
}
// Candidates within `radius` (and at least `minDist` away) that `pred` likes,
// reservoir-sampled with `weight` so the strongest few win.
static bool ecoDisperseTo(int q, int r, uint8_t gen, uint16_t cap, int minDist, int radius, int kind) {
  int bestQ = -1, bestR = -1, bestW = 0; uint32_t seen = 0;
  for (int dr = -radius; dr <= radius; dr++) {
    for (int dq = -radius; dq <= radius; dq++) {
      int cq = wrapQ(q + dq), cr = wrapR(r + dr);
      int dist = hexDistWrap(q, r, cq, cr);
      if (dist < minDist || dist > radius) continue;
      const HexCell& c = ecoSnapMap[cr][cq];
      int w = 0;
      if (kind == ECO_VEC_FOOT) w = ecoPopcount8(c.footprints) * 4 + (ecoSnapDyn[cr][cq].track ? 2 : 0);
      else if (kind == ECO_VEC_TIRE) w = c.tireTrack ? 4 : 0;
      else if (kind == ECO_VEC_ASH)  w = (c.terrain == 1) ? 4 : 0;
      if (!w) continue;
      w += ecoRandN(3);
      if (w > bestW) { bestW = w; bestQ = cq; bestR = cr; seen = 1; }
      else if (w == bestW && ecoRandN((int)++seen) == 0) { bestQ = cq; bestR = cr; }
    }
  }
  if (bestQ >= 0 && ecoPlaceSpore(bestQ, bestR, gen, cap)) { ecoNoteFlight(q, r, bestQ, bestR); return true; }
  return false;
}
static void ecoDisperse(int q, int r, uint8_t gen, uint16_t cap) {
  int placed = 0;
  for (int k = 0; k < ecoSp.sporesPerBody; k++) {
    bool ok = false;
    switch (ecoSp.vector) {
      case ECO_VEC_FOOT: ok = ecoDisperseTo(q, r, gen, cap, 3, 8, ECO_VEC_FOOT); break;
      case ECO_VEC_TIRE: ok = ecoDisperseTo(q, r, gen, cap, 3, 12, ECO_VEC_TIRE); break;
      case ECO_VEC_ASH:  ok = ecoDisperseTo(q, r, gen, cap, 1, 10, ECO_VEC_ASH); break;
      default: break;
    }
    if (!ok) ok = ecoDisperseWind(q, r, gen, cap);
    if (ok) placed++;
  }
  // A wave must not end here. If the vector and the wind both found nothing
  // (a body hemmed in by water, or by survivors -- ECO_SPORE_KEEPOUT), try
  // anywhere within reach, then anywhere at all.
  for (int tries = 0; tries < 60 && !placed; tries++) {
    int radius = tries < 40 ? 12 : MAP_COLS;
    int tq = wrapQ(q + ecoRandN(radius * 2 + 1) - radius), tr = wrapR(r + ecoRandN(radius * 2 + 1) - radius);
    if (ecoPlaceSpore(tq, tr, gen, cap)) { ecoNoteFlight(q, r, tq, tr); placed++; }
  }
}

static void ecoGerminate(EcoSpore& sp) {
  int ci = ecoFreeColony();
  if (ci < 0) return;
  EcoColony& c = ecoColonies[ci];
  memset(&c, 0, sizeof(c));
  c.stage = ECO_ST_GERM; c.generation = sp.generation; c.waveCap = sp.waveCap;
  c.stageTick = ecoTickN; c.lastFoodTick = ecoTickN; c.originQ = sp.q; c.originR = sp.r;
  c.seed = (uint8_t)ecoRand();
  memset(ecoVisited + (size_t)ci * ECO_BLOOM_BYTES, 0, ECO_BLOOM_BYTES);
  int n = 32 + ecoRandN(33);
  int cx = sp.q * 256 + 128, cy = sp.r * 256 + 128;
  for (int k = 0; k < n; k++) {
    int x = cx + ecoRandN(129) - 64, y = cy + ecoRandN(129) - 64;
    if (x < 0) x += ECO_XW; else if (x >= ECO_XW) x -= ECO_XW;
    if (y < 0) y += ECO_YW; else if (y >= ECO_YW) y -= ECO_YW;
    if (!ecoSpawnAgent((uint8_t)ci, x, y, (uint8_t)ecoRand(), 128, 0)) break;
  }
  if ((int)sp.generation + 1 > ecoWave) ecoWave = (uint16_t)(sp.generation + 1);
  sp.used = 0;
  Log.notice("eco germinate colony=%d gen=%u cap=%u at (%d,%d) agents=%d",
             ci, (unsigned)c.generation, (unsigned)c.waveCap, (int)sp.q, (int)sp.r, n);
}

// Densest hexes of this colony on liked ground, at least 3 apart, 1-3 of them.
static void ecoEnterFruit(int ci) {
  EcoColony& c = ecoColonies[ci];
  memset(ecoHexCount, 0, sizeof(uint16_t) * ECO_HEXES);
  for (int i = 0; i < ecoAgentCount; i++) {
    const EcoAgent& a = ecoAgents[i];
    if (a.colony != ci) continue;
    int hi = ecoHexIdx(a.x >> 8, a.y >> 8);
    if (ecoHexCount[hi] < 0xFFFF) ecoHexCount[hi]++;
  }
  int want = min(3, 1 + (int)c.agentCount / 200);
  c.nSites = 0;
  for (int s = 0; s < want; s++) {
    int bestI = -1; long bestScore = 0;
    for (int hi = 0; hi < ECO_HEXES; hi++) {
      if (!ecoHexCount[hi]) continue;
      int q = hi % MAP_COLS, r = hi / MAP_COLS;
      const EcoHex& e = ecoHex[r][q];
      if (e.barrier >= ECO_B_HOSTILE) continue;
      uint8_t t = ecoLandTerrain(q, r);
      long score = (long)ecoHexCount[hi] * (2 + (long)ecoSp.affinity[t]) * (16 + (long)ecoHexDens[hi]);
      bool far = true;
      for (int k = 0; k < c.nSites; k++) if (hexDistWrap(q, r, c.siteQ[k], c.siteR[k]) < 3) { far = false; break; }
      if (!far) continue;
      if (score > bestScore) { bestScore = score; bestI = hi; }
    }
    if (bestI < 0) break;
    c.siteQ[c.nSites] = (int16_t)(bestI % MAP_COLS); c.siteR[c.nSites] = (int16_t)(bestI / MAP_COLS); c.nSites++;
  }
  if (!c.nSites) { c.siteQ[0] = c.originQ; c.siteR[0] = c.originR; c.nSites = 1; }
  for (int s = 0; s < c.nSites; s++) {
    int b = ecoFreeBody();
    if (b < 0) break;
    EcoBody& body = ecoBodies[b];
    body.used = 1; body.q = (uint8_t)c.siteQ[s]; body.r = (uint8_t)c.siteR[s]; body.colony = (uint8_t)ci;
    body.stage = 1; body.timer = 0; body.seed = (uint8_t)ecoRand();
  }
  for (int i = 0; i < ecoAgentCount; i++) if (ecoAgents[i].colony == ci) ecoAgents[i].flags |= ECO_AF_RETURN;
  c.stage = ECO_ST_FORM; c.timer = ecoSp.formTicks; c.stageTick = ecoTickN;
  Log.notice("eco fruit colony=%d sites=%d first=(%d,%d) agents=%u", ci, (int)c.nSites,
             (int)c.siteQ[0], (int)c.siteR[0], (unsigned)c.agentCount);
}

static void ecoBurst(int ci) {
  EcoColony& c = ecoColonies[ci];
  uint16_t cap = (uint16_t)min((long)ECO_COLONY_CAP_MAX, ((long)c.waveCap * ecoSp.capMulQ8) >> 8);
  uint8_t gen = (uint8_t)min(250, (int)c.generation + 1);
  int bodies = 0;
  for (int b = 0; b < ECO_MAX_BODIES; b++) {
    EcoBody& body = ecoBodies[b];
    if (!body.used || body.colony != ci) continue;
    body.stage = 3;                                   // bursting: one message, then gone
    bodies++;
    EcoHex& e = ecoHex[body.r][body.q];
    e.scar = (uint8_t)min(15, (int)e.scar + 3);       // fruiting scar
    ecoDisperse(body.q, body.r, gen, cap);
  }
  if (!bodies) ecoDisperse(c.siteQ[0], c.siteR[0], gen, cap);   // no body slot was free: the spores still fly
  for (int i = 0; i < ecoAgentCount; ) { if (ecoAgents[i].colony == ci) ecoKillAgent(i); else i++; }
  Log.notice("eco burst colony=%d gen=%u bodies=%d nextCap=%u", ci, (unsigned)c.generation, bodies, (unsigned)cap);
  memset(&c, 0, sizeof(c));   // stage FREE
  ecoPublishScars();
}

static void ecoLifecycle() {
  // Bodies that burst last tick were in last tick's message; they go now.
  for (int b = 0; b < ECO_MAX_BODIES; b++) if (ecoBodies[b].used && ecoBodies[b].stage == 3) ecoBodies[b].used = 0;
  // Spores: dormancy, then germination when the caps allow.
  int alive = 0;
  for (int s = 0; s < ECO_MAX_SPORES; s++) {
    EcoSpore& sp = ecoSpores[s];
    if (!sp.used) continue;
    if (ecoSnapDyn[sp.r][sp.q].fire || ecoIsWater(ecoLandTerrain(sp.q, sp.r))) { sp.used = 0; continue; }
    alive++;
    if (sp.timer) { sp.timer--; continue; }
    if (ecoAgentCount + 64 > ECO_MAX_AGENTS || ecoCoveragePct >= ECO_COVERAGE_CAP || ecoFreeColony() < 0) continue;
    ecoGerminate(sp);
  }
  ecoStats.spores = (uint16_t)alive;
  // Colonies
  for (int ci = 0; ci < ECO_MAX_COLONIES; ci++) {
    EcoColony& c = ecoColonies[ci];
    if (c.stage == ECO_ST_FREE) continue;
    uint32_t inStage = ecoTickN - c.stageTick;
    switch (c.stage) {
      case ECO_ST_GERM:
        if (inStage >= ecoSp.germTicks) { c.stage = ECO_ST_FORAGE; c.stageTick = ecoTickN; c.lastFoodTick = ecoTickN; }
        break;
      case ECO_ST_FORAGE:
        if (c.agentCount == 0) { memset(&c, 0, sizeof(c)); break; }   // starved to nothing
        if (c.agentCount >= c.waveCap || ecoTickN - c.lastFoodTick >= ecoSp.starveTicks || inStage >= ecoSp.forageTicks)
          ecoEnterFruit(ci);
        break;
      case ECO_ST_FORM:
        if (c.timer) c.timer--;
        else {
          c.stage = ECO_ST_MATURE; c.timer = ecoSp.holdTicks; c.stageTick = ecoTickN;
          for (int b = 0; b < ECO_MAX_BODIES; b++) if (ecoBodies[b].used && ecoBodies[b].colony == ci) ecoBodies[b].stage = 2;
        }
        break;
      case ECO_ST_MATURE:
        if (c.timer) c.timer--;
        else ecoBurst(ci);
        break;
      default: break;
    }
  }
}

// ── 7. The Wasteland Daisy ───────────────────────────────────────────────────
// Only injuries plant seeds (spec table). One switch, so moving a cause is a
// one-line change.
static bool ecoIsInjury(uint8_t cause) {
  switch (cause) {
    case DC_FIRE: case DC_LIGHTNING: case DC_FLOOD: case DC_DOOM:
    case DC_ENC_HAZARD: case DC_CHEM: case DC_DAISY:
      return true;
    default:
      return false;
  }
}
// Called by every injury-class damage site, under G.mutex, right where it sets
// the DownCause. Surface only. The ring is the game's one write into the
// Understory and carries nothing back.
static void ecoNoteHurt(int pid, uint8_t cause) {
  if (pid < 0 || pid >= MAX_PLAYERS || !ecoIsInjury(cause)) return;
  const Player& p = G.players[pid];
  if (p.depth) return;
  if (p.q < 0 || p.q >= MAP_COLS || p.r < 0 || p.r >= MAP_ROWS) return;
  uint8_t idx = (uint8_t)((ecoHurtHead + ecoHurtCount) % ECO_HURT_RING);
  if (ecoHurtCount == ECO_HURT_RING) { idx = ecoHurtHead; ecoHurtHead = (uint8_t)((ecoHurtHead + 1) % ECO_HURT_RING); }
  else ecoHurtCount++;
  ecoHurt[idx].q = (uint8_t)p.q; ecoHurt[idx].r = (uint8_t)p.r;
}
static int ecoDaisyAt(int q, int r) {
  for (int i = 0; i < ECO_MAX_DAISIES; i++)
    if (ecoDaisies[i].used && ecoDaisies[i].q == q && ecoDaisies[i].r == r) return i;
  return -1;
}
static void ecoDaisySeed(int q, int r) {
  int i = ecoDaisyAt(q, r);
  if (i >= 0) { if (ecoDaisies[i].count < 7) ecoDaisies[i].count++; return; }
  int slot = -1;
  for (int k = 0; k < ECO_MAX_DAISIES; k++) if (!ecoDaisies[k].used) { slot = k; break; }
  if (slot < 0) {
    // Full: the youngest patch nobody can see yet gives way. Blooms never do.
    uint32_t newest = 0;
    for (int k = 0; k < ECO_MAX_DAISIES; k++)
      if (ecoDaisies[k].stage == 0 && ecoDaisies[k].seededTick >= newest) { newest = ecoDaisies[k].seededTick; slot = k; }
    if (slot < 0) return;
  }
  EcoDaisy& d = ecoDaisies[slot];
  d.used = 1; d.q = (uint8_t)q; d.r = (uint8_t)r; d.stage = 0; d.dawns = 0; d.count = 1;
  d.seed = (uint8_t)ecoRand(); d.seededTick = ecoTickN;
}
static void ecoDaisyTick(const uint8_t* hurtQ, const uint8_t* hurtR, int nHurt, bool dawn) {
  for (int i = 0; i < ECO_MAX_DAISIES; i++) {
    EcoDaisy& d = ecoDaisies[i];
    if (!d.used) continue;
    if (ecoSnapDyn[d.r][d.q].fire) { d.used = 0; continue; }   // fire burns them away, seed included
    if (dawn) { if (d.dawns < 255) d.dawns++; d.stage = (uint8_t)min(3, (int)d.dawns); }
  }
  for (int i = 0; i < nHurt; i++) ecoDaisySeed(hurtQ[i], hurtR[i]);
  // Publish the bloom bitset the GameLoop bites from.
  uint8_t next = (uint8_t)(ecoPubBloomIdx ^ 1);
  memset(ecoBloom[next], 0, ECO_BLOOM_BYTES);
  int seeded = 0, growing = 0, bloomed = 0;
  for (int i = 0; i < ECO_MAX_DAISIES; i++) {
    const EcoDaisy& d = ecoDaisies[i];
    if (!d.used) continue;
    if (d.stage == 0) seeded++; else if (d.stage < 3) growing++; else {
      bloomed++;
      int hi = ecoHexIdx(d.q, d.r);
      ecoBloom[next][hi >> 3] |= (uint8_t)(1 << (hi & 7));
    }
  }
  ecoPubBloomIdx = next;
  ecoStats.dSeeded = (uint8_t)seeded; ecoStats.dGrowing = (uint8_t)growing; ecoStats.dBloomed = (uint8_t)bloomed;
}
// Fire clears the GameLoop's copy of the bit the moment the hex ignites, so
// "I burned them, now I walk through" is exact rather than a tick late.
static void ecoOnIgnite(int16_t q, int16_t r) {
  if (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS) return;
  int hi = ecoHexIdx(q, r);
  ecoBloom[ecoPubBloomIdx][hi >> 3] &= (uint8_t)~(1 << (hi & 7));
}
static inline bool ecoBloomAt(int q, int r) {
  int hi = ecoHexIdx(q, r);
  return (ecoBloom[ecoPubBloomIdx][hi >> 3] >> (hi & 7)) & 1;
}
// The bite. Called under G.mutex wherever a survivor ENTERS a surface hex:
// a step, surfacing from the tunnels, a respawn, a teleport. Once per entry.
// Reads only the published bloom bitset, so the patch array stays private.
static bool ecoBiteCheck(int pid) {
  if (!ecoBiteOn || pid < 0 || pid >= MAX_PLAYERS) return false;
  Player& p = G.players[pid];
  if (!p.connected || p.depth || p.ll == 0) return false;
  if (p.q < 0 || p.q >= MAP_COLS || p.r < 0 || p.r >= MAP_ROWS) return false;
  if (!ecoBloomAt(p.q, p.r)) return false;
  p.ll--;
  ledFlash(210, 235, 120);
  k10Play(MOTIF_MUTANT_BREATH);
  { GameEvent dmg = {}; dmg.type = EVT_DAMAGE; dmg.pid = (uint8_t)pid;
    dmg.amt = 1; dmg.res = DC_DAISY; dmg.actNewLL = p.ll; dmg.q = p.q; dmg.r = p.r; enqEvt(dmg); }
  ecoNoteHurt(pid, DC_DAISY);   // a bite is an injury: the patch that bit you thickens
  if (p.ll == 0) {
    p.movesLeft = 0;
    GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)pid;
    dev.res = DC_DAISY; dev.evWsId = p.wsClientId; enqEvt(dev);
  }
  return true;
}

// The blight: a surface hex with any mould the client draws on it (density 1
// or more, or a mature fruiting body -- not a daisy) holds no resource and
// grows none back while the mould stays. The GameLoop's respawn pass
// (tickGame, actions_game_loop.hpp) does the eating under G.mutex from the
// published density, so the Eco task still writes nothing the game reads.
// The mould is surface-only and so is this: nothing here touches G.tunnel.
// nullptr = no blight: the switch is off, or nothing is published yet or
// since a regen (the old world's density must not eat the new world's piles).
static const uint8_t* ecoBlightMap() {
  if (!ecoBlightOn || !ecoWireReady) return nullptr;
  return ecoPubDens[ecoPubDensIdx];
}
static inline bool ecoBlightAt(const uint8_t* bm, int q, int r) {
  return bm && q >= 0 && q < MAP_COLS && r >= 0 && r < MAP_ROWS && (bm[ecoHexIdx(q, r)] & 0x8F);
}
static void ecoNoteEaten() { ecoBlightEaten++; }

// ── 8. Per-hex derivation: density, edge masks, coverage, age, scars ─────────
static inline int ecoStripMean(int x0, int y0, int w, int h) {
  int sum = 0;
  for (int y = y0; y < y0 + h; y++) {
    const uint8_t* row = ecoTrail + (size_t)(y % ECO_H) * ECO_W;
    for (int x = x0; x < x0 + w; x++) sum += row[x % ECO_W];
  }
  return sum / (w * h);
}
static void ecoDerive(bool dawn) {
  int land = 0, veined = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int mx = 0, sum = 0;
      for (int y = r * ECO_SUB; y < (r + 1) * ECO_SUB; y++) {
        const uint8_t* row = ecoTrail + (size_t)y * ECO_W + q * ECO_SUB;
        for (int x = 0; x < ECO_SUB; x++) { sum += row[x]; if (row[x] > mx) mx = row[x]; }
      }
      int mean = sum / (ECO_SUB * ECO_SUB);
      int dens = ((mx + 3 * mean) / 4) / ECO_DENS_DIV;
      if (dens > 15) dens = 15;
      int hi = ecoHexIdx(q, r);
      ecoHexDens[hi] = (uint8_t)dens;
      EcoHex& e = ecoHex[r][q];
      if (!ecoIsWater(ecoLandTerrain(q, r))) {
        land++;
        if (dens >= ECO_VISIBLE_DENS) { veined++; if (e.age < 255) e.age++; }
      }
    }
  }
  ecoCoveragePct = (uint8_t)(land ? veined * 100 / land : 0);
  // Edge masks: bits 0-2 (E, NE, N) from the shared border strips, bits 3-5
  // copied from the neighbours' 0-2 so both sides of an edge always agree.
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int hi = ecoHexIdx(q, r);
      uint8_t m = 0;
      if (ecoHexDens[hi]) {
        int x0 = q * ECO_SUB, y0 = r * ECO_SUB;
        // d0: east neighbour (q+1, r)
        { int nq = wrapQ(q + 1);
          if (ecoHexDens[ecoHexIdx(nq, r)] &&
              (ecoStripMean(x0 + 3, y0, 1, 4) + ecoStripMean(nq * ECO_SUB, y0, 1, 4)) / 2 >= ECO_EDGE_T) m |= 1; }
        // d1: north-east neighbour (q+1, r-1): the two corner 2x2 blocks
        { int nq = wrapQ(q + 1), nr = wrapR(r - 1);
          if (ecoHexDens[ecoHexIdx(nq, nr)] &&
              (ecoStripMean(x0 + 2, y0, 2, 2) + ecoStripMean(nq * ECO_SUB, nr * ECO_SUB + 2, 2, 2)) / 2 >= ECO_EDGE_T) m |= 2; }
        // d2: north neighbour (q, r-1)
        { int nr = wrapR(r - 1);
          if (ecoHexDens[ecoHexIdx(q, nr)] &&
              (ecoStripMean(x0, y0, 4, 1) + ecoStripMean(x0, nr * ECO_SUB + 3, 4, 1)) / 2 >= ECO_EDGE_T) m |= 4; }
      }
      ecoHexMask[hi] = m;
    }
  }
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int hi = ecoHexIdx(q, r);
      uint8_t m = ecoHexMask[hi];
      if (ecoHexMask[ecoHexIdx(wrapQ(q - 1), r)] & 1)               m |= 8;    // west's east
      if (ecoHexMask[ecoHexIdx(wrapQ(q - 1), wrapR(r + 1))] & 2)    m |= 16;   // south-west's north-east
      if (ecoHexMask[ecoHexIdx(q, wrapR(r + 1))] & 4)               m |= 32;   // south's north
      ecoHexMask[hi] = m;
    }
  }
  // Dawn: every hex that carried a vein long enough today takes a scar.
  if (dawn) {
    bool changed = false;
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int q = 0; q < MAP_COLS; q++) {
        EcoHex& e = ecoHex[r][q];
        if (e.age >= ECO_AGE_SCAR_TICKS && e.scar < 15) { e.scar++; changed = true; }
        e.age = 0;
      }
    }
    if (changed) ecoPublishScars();
  }
  // LCD density
  uint8_t next = (uint8_t)(ecoPubDensIdx ^ 1);
  uint8_t* pd = ecoPubDens[next];
  for (int hi = 0; hi < ECO_HEXES; hi++) pd[hi] = ecoHexDens[hi];
  for (int b = 0; b < ECO_MAX_BODIES; b++)
    if (ecoBodies[b].used && ecoBodies[b].stage == 2) pd[ecoHexIdx(ecoBodies[b].q, ecoBodies[b].r)] |= 0x80;
  for (int i = 0; i < ECO_MAX_DAISIES; i++)
    if (ecoDaisies[i].used && ecoDaisies[i].stage == 3) pd[ecoHexIdx(ecoDaisies[i].q, ecoDaisies[i].r)] |= 0x40;
  ecoPubDensIdx = next;
}
static const uint8_t* ecoPublishedDensity() { return ecoPubDens[ecoPubDensIdx]; }

// ── 9. Wire encode (layer 4) ─────────────────────────────────────────────────
static const char ECO_B64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char ECO_HEXC[17] = "0123456789ABCDEF";
static void ecoEncode() {
  uint8_t next = (uint8_t)(ecoPubWire ^ 1);
  char* b = ecoWire[next];
  int cap = ECO_WIRE_CAP, pos = 0;
  pos += snprintf(b + pos, cap - pos, "{\"t\":\"eco\",\"tk\":%lu,\"n\":\"%s\",\"h\":%u,\"g\":%u,\"w\":%u,\"v\":\"",
                  (unsigned long)ecoTickN, ecoSp.name, (unsigned)ecoSp.hue, (unsigned)ecoSp.fruit, (unsigned)ecoWave);
  for (int hi = 0; hi < ECO_HEXES && pos + 2 < cap - 400; hi++) {
    b[pos++] = ECO_HEXC[ecoHexDens[hi] & 15];
    b[pos++] = ECO_B64[ecoHexMask[hi] & 63];
  }
  pos += snprintf(b + pos, cap - pos, "\",\"f\":[");
  bool first = true;
  int fruiting = 0;
  for (int i = 0; i < ECO_MAX_BODIES; i++) {
    const EcoBody& bd = ecoBodies[i];
    if (!bd.used) continue;
    fruiting++;
    pos += snprintf(b + pos, cap - pos, "%s[%d,%d,%d,%d]", first ? "" : ",", (int)bd.q, (int)bd.r, (int)bd.stage, (int)bd.seed);
    first = false;
  }
  pos += snprintf(b + pos, cap - pos, "],\"dz\":[");
  first = true;
  for (int i = 0; i < ECO_MAX_DAISIES; i++) {
    const EcoDaisy& d = ecoDaisies[i];
    if (!d.used || d.stage == 0) continue;   // seeded patches draw nothing
    pos += snprintf(b + pos, cap - pos, "%s[%d,%d,%d,%d,%d]", first ? "" : ",", (int)d.q, (int)d.r, (int)d.stage, (int)d.count, (int)d.seed);
    first = false;
  }
  pos += snprintf(b + pos, cap - pos, "],\"sp\":[");
  for (int i = 0; i < ecoNFlights; i++)
    pos += snprintf(b + pos, cap - pos, "%s[%d,%d,%d,%d]", i ? "," : "", (int)ecoFlights[i].fq, (int)ecoFlights[i].fr,
                    (int)ecoFlights[i].tq, (int)ecoFlights[i].tr);
  pos += snprintf(b + pos, cap - pos, "]}");
  ecoWireLen[next] = pos;
  ecoPubWire = next;
  // Scars: a separate small message, sent on join and whenever they change.
  // Rebuilt only when stale (genesis, a scar accrued) -- it is 4.3 KB.
  if (ecoScarMsgStale) {
    uint8_t sn = (uint8_t)(ecoPubScar ^ 1);
    char* s = ecoScarMsg[sn];
    int sp = snprintf(s, ECO_SCARMSG_CAP, "{\"t\":\"eco\",\"tk\":%lu,\"s\":\"", (unsigned long)ecoTickN);
    for (int hi = 0; hi < ECO_HEXES; hi++) s[sp++] = ECO_HEXC[ecoHex[hi / MAP_COLS][hi % MAP_COLS].scar & 15];
    sp += snprintf(s + sp, ECO_SCARMSG_CAP - sp, "\"}");
    ecoScarLen[sn] = sp;
    ecoPubScar = sn;
    ecoScarMsgStale = false;
    ecoScarMsgSend  = true;
  }
  ecoWireReady = true;
  ecoStats.fruiting = (uint8_t)fruiting;
}
// sendSync: the published buffers, read through the swapped index. Never races the encoder.
static void ecoSendPublished(AsyncWebSocketClient* client) {
  if (!ecoWireReady || !client) return;
  uint8_t si = ecoPubScar, wi = ecoPubWire;
  if (ecoScarLen[si]) client->text(ecoScarMsg[si], (size_t)ecoScarLen[si]);
  if (ecoWireLen[wi]) client->text(ecoWire[wi], (size_t)ecoWireLen[wi]);
}
static void ecoStateJson(String& j) {
  EcoStats st; memcpy(&st, (const void*)&ecoStats, sizeof(st));
  j += ",\"eco\":{\"on\":true,\"seed\":";      j += (uint32_t)ecoSp.seed;
  j += ",\"name\":\"";                          j += ecoSp.name; j += "\"";
  j += ",\"style\":";                           j += (int)ecoSp.style;
  j += ",\"vector\":";                          j += (int)ecoSp.vector;
  j += ",\"hue\":";                             j += (int)ecoSp.hue;
  j += ",\"tick\":";                            j += (uint32_t)st.tick;
  j += ",\"tickUs\":";                          j += (uint32_t)st.tickUs;
  j += ",\"wave\":";                            j += (int)st.wave;
  j += ",\"colonies\":";                        j += (int)st.colonies;
  j += ",\"agents\":";                          j += (int)st.agents;
  j += ",\"spores\":";                          j += (int)st.spores;
  j += ",\"coverage\":";                        j += (int)st.coverage;
  j += ",\"fruiting\":";                        j += (int)st.fruiting;
  j += ",\"scarred\":";                         j += (int)st.scarred;
  j += ",\"bootsSeen\":";                       j += (uint32_t)ecoBootsSeen;
  j += ",\"daisies\":{\"seeded\":";             j += (int)st.dSeeded;
  j += ",\"growing\":";                         j += (int)st.dGrowing;
  j += ",\"bloomed\":";                         j += (int)st.dBloomed; j += "}";
  j += ",\"bite\":";                            j += ecoBiteOn ? 1 : 0;
  j += ",\"blight\":";                          j += ecoBlightOn ? 1 : 0;
  j += ",\"eaten\":";                           j += (uint32_t)ecoBlightEaten;
  j += "}";
}

// ── 10. Genesis, regen, the task ─────────────────────────────────────────────
static void ecoResetLiving() {
  ecoAgentCount = 0;
  memset(ecoColonies, 0, sizeof(ecoColonies));
  memset(ecoBodies, 0, sizeof(ecoBodies));
  memset(ecoSpores, 0, sizeof(ecoSpores));
  memset(ecoDaisies, 0, sizeof(ecoDaisies));
  memset(ecoTileAgent, 0, sizeof(ecoTileAgent));
  memset(ecoTileTrail, 0, sizeof(ecoTileTrail));
  memset(ecoTrail, 0, (size_t)ECO_H * ECO_W);
  memset(ecoHexDens, 0, ECO_HEXES);
  memset(ecoHexMask, 0, ECO_HEXES);
  memset(ecoBloom, 0, sizeof(ecoBloom));
  for (int r = 0; r < MAP_ROWS; r++) for (int q = 0; q < MAP_COLS; q++) { ecoHex[r][q].age = 0; ecoHex[r][q].food = 0; ecoHex[r][q].barrier = 0; }
  ecoNFlights = 0; ecoWave = 0; ecoCoveragePct = 0; ecoHurtHead = ecoHurtCount = 0;
}
// Weighted toward old fruiting scars, then vein scars, then preferred terrain;
// never on water, never near a connected survivor. Caller has the snapshot.
static void ecoPlaceGenesisSpores() {
  int want = 2 + ecoRandN(3);
  for (int k = 0; k < want; k++) {
    long total = 0; int pickQ = -1, pickR = -1;
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int q = 0; q < MAP_COLS; q++) {
        uint8_t t = ecoLandTerrain(q, r);
        if (ecoIsWater(t) || ecoNearPlayer(q, r, ECO_SPORE_KEEPOUT)) continue;
        const EcoHex& e = ecoHex[r][q];
        long w = 0;
        if (e.scar >= 3) w = 8 + (long)e.scar * (1 + ecoSp.scarLove);
        else if (e.scar)  w = 2 + (long)e.scar * (1 + ecoSp.scarLove) / 2;
        uint8_t aff = ecoSp.affinity[t];
        if (aff == 3) w += 3; else if (aff == 2) w += 1;
        if (!w) continue;
        // no spore stacked on another spore's hex
        bool taken = false;
        for (int s = 0; s < ECO_MAX_SPORES; s++) if (ecoSpores[s].used && ecoSpores[s].q == q && ecoSpores[s].r == r) { taken = true; break; }
        if (taken) continue;
        total += w;
        if ((long)(ecoRand() % (uint32_t)total) < w) { pickQ = q; pickR = r; }
      }
    }
    if (pickQ < 0) {   // nothing preferred anywhere: any dry hex
      for (int tries = 0; tries < 200 && pickQ < 0; tries++) {
        int q = ecoRandN(MAP_COLS), r = ecoRandN(MAP_ROWS);
        if (ecoSporeOk(q, r)) { pickQ = q; pickR = r; }
      }
      if (pickQ < 0) break;
    }
    ecoPlaceSpore(pickQ, pickR, 0, ECO_FIRST_CAP);
    Log.notice("eco spore %d at (%d,%d) scar=%u", k, pickQ, pickR, (unsigned)ecoHex[pickR][pickQ].scar);
  }
}
// Snapshot the inputs (caller does NOT hold G.mutex).
static bool ecoSnapshot(uint8_t* hurtQ, uint8_t* hurtR, int* nHurt) {
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  memcpy(ecoSnapMap, G.map, MAP_BYTES);
  memcpy(ecoSnapDyn, W_hex, W_HEX_BYTES);
  ecoSnapWeather = G.weatherPhase;
  ecoSnapDay = G.dayCount;
  ecoSnapNPlayers = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player& p = G.players[i];
    if (!p.connected || p.depth) continue;
    ecoSnapPlayers[ecoSnapNPlayers].q = p.q; ecoSnapPlayers[ecoSnapNPlayers].r = p.r; ecoSnapNPlayers++;
  }
  *nHurt = 0;
  while (ecoHurtCount) {
    hurtQ[*nHurt] = ecoHurt[ecoHurtHead].q; hurtR[*nHurt] = ecoHurt[ecoHurtHead].r; (*nHurt)++;
    ecoHurtHead = (uint8_t)((ecoHurtHead + 1) % ECO_HURT_RING); ecoHurtCount--;
  }
  xSemaphoreGive(G.mutex);
  return true;
}
// Genesis: roll (or take) the seed, decode, seed the ecology's own PRNG --
// nothing after this calls esp_random() -- and place the first spores.
// Runs from setup() once the radio is up (esp_random() is only a true RNG
// then), so the splash can carry the name; and again from the task on regen.
static void ecoGenesis() {
  if (!ecoTrail) return;
  uint32_t pinned = ecoPrefSeed();
  uint32_t seed = pinned ? pinned : esp_random();
  if (!seed) seed = 1;
  ecoDecodeGenome(seed);
  ecoRng = seed ^ 0x9E3779B9u; ecoRand();
  ecoResetLiving();
  ecoBootsSeen++;
  uint8_t hq[ECO_HURT_RING], hr[ECO_HURT_RING]; int nh = 0;
  if (!ecoSnapshot(hq, hr, &nh)) { memcpy(ecoSnapMap, G.map, MAP_BYTES); memcpy(ecoSnapDyn, W_hex, W_HEX_BYTES); }
  ecoPlaceGenesisSpores();
  ecoLastDay = ecoSnapDay;
  ecoGenesisDone = true;
  ecoPublishScars();                // the first wire message carries the scars
  ecoScarDirty = false;             // nothing new to write yet; bootsSeen rides the next save
  Log.notice("eco genesis seed=%u name=%s style=%u vector=%u tempo=%u cycle=%u growth=%u shy=%d hue=%u pinned=%d boots=%u",
             (unsigned)seed, ecoSp.name, (unsigned)ecoSp.style, (unsigned)ecoSp.vector, (unsigned)ecoSp.tempo,
             (unsigned)ecoSp.cycle, (unsigned)ecoSp.growth, (int)ecoSp.shyness, (unsigned)ecoSp.hue,
             pinned ? 1 : 0, (unsigned)ecoBootsSeen);
  { char lb[48]; snprintf(lb, sizeof(lb), "Taken root: %s", ecoSp.name);
    k10LogAdd(lb, -1, TONE_OMEN, GLY_NONE); }
}
// A world regen (network-msg-player.hpp): the scars belong to the old land.
// The task re-runs genesis on its next tick so buffer ownership never moves.
static void ecoRequestRegen() { ecoWireReady = false; ecoRegenReq = true; }

static void ecoTick() {
  uint32_t t0 = micros();
  if (ecoRegenReq) { ecoRegenReq = false; ecoClearScarsRam(); ecoGenesis(); }
  uint8_t hq[ECO_HURT_RING], hr[ECO_HURT_RING]; int nh = 0;
  if (!ecoSnapshot(hq, hr, &nh)) { LOG_VERBOSE("eco tick skipped: G.mutex busy"); return; }
  ecoTickN++;
  ecoNFlights = 0;
  ecoScarMsgSend = false;
  bool dawn = (ecoLastDay != 0xFFFF && ecoSnapDay != ecoLastDay);
  ecoLastDay = ecoSnapDay;

  ecoTransforms();
  ecoDaisyTick(hq, hr, nh, dawn);
  ecoLifecycle();
  int sub = ecoSubsteps();
  for (int s = 0; s < sub; s++) {
    ecoAgentStep();
    ecoDiffuse();
    taskYIELD();
  }
  ecoDerive(dawn);
  ecoEncode();

  int colonies = 0;
  for (int i = 0; i < ECO_MAX_COLONIES; i++) if (ecoColonies[i].stage != ECO_ST_FREE) colonies++;
  ecoStats.tick = ecoTickN; ecoStats.wave = ecoWave; ecoStats.colonies = (uint8_t)colonies;
  ecoStats.agents = (uint16_t)ecoAgentCount; ecoStats.coverage = ecoCoveragePct; ecoStats.scarred = (uint16_t)ecoScarredHexes;
  ecoLastTickUs = micros() - t0;
  ecoStats.tickUs = ecoLastTickUs;

  if (ws.count() > 0) {
    uint8_t wi = ecoPubWire;
    if (ecoScarMsgSend) { uint8_t si = ecoPubScar; ws.textAll(ecoScarMsg[si], (size_t)ecoScarLen[si]); }
    ws.textAll(ecoWire[wi], (size_t)ecoWireLen[wi]);
  }
  LOG_VERBOSE("eco tick us=%u sub=%d agents=%d colonies=%d cov=%u wave=%u bodies=%u daisies=%u/%u/%u bytes=%d",
              (unsigned)ecoLastTickUs, sub, ecoAgentCount, colonies, (unsigned)ecoCoveragePct, (unsigned)ecoWave,
              (unsigned)ecoStats.fruiting, (unsigned)ecoStats.dSeeded, (unsigned)ecoStats.dGrowing,
              (unsigned)ecoStats.dBloomed, ecoWireLen[ecoPubWire]);
}

static void ecoTask(void* param) {
  (void)param;
  Log.notice("ecoTask running core=%d prio=%d", (int)xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(ECO_TICK_MS));
    if (!ecoGenesisDone) continue;
    ecoTick();
  }
}
static void ecoStartTask() {
  if (!ecoTrail) { Log.error("eco: no PSRAM buffers, task not started"); return; }
  xTaskCreatePinnedToCore(ecoTask, "Eco", 8192, NULL, 1, NULL, 1);
  Log.notice("ecoTask spawned core=1 prio=1 stack=8KB tick=%ums", (unsigned)ECO_TICK_MS);
}

#else  // ECO_ENABLE == 0: every hook is a no-op and the game is exactly as it was.

static void ecoAllocPsram() {}
static void ecoNoteHurt(int, uint8_t) {}
static bool ecoBiteCheck(int) { return false; }
static void ecoOnIgnite(int16_t, int16_t) {}
static void ecoRequestRegen() {}
static void ecoSendPublished(AsyncWebSocketClient*) {}
static void ecoSaveScarsIfDirty() {}
static void ecoLoadScars() {}
static void ecoDiscardScarFile() {}
static void ecoGenesis() {}
static void ecoStartTask() {}
static void ecoStateJson(String& j) { j += ",\"eco\":{\"on\":false}"; }
static const char* ecoName() { return "(disabled)"; }
static const uint8_t* ecoPublishedDensity() { return nullptr; }
static const uint8_t* ecoBlightMap() { return nullptr; }
static inline bool ecoBlightAt(const uint8_t*, int, int) { return false; }
static void ecoNoteEaten() {}
static uint32_t ecoLcdTint(uint32_t base, uint8_t) { return base; }
static uint32_t ecoAccentRgb() { return 0xFFFFFF; }
static void ecoSetPrefs(long, int, int) {}

#endif
