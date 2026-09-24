#pragma once
// ── hex-map.hpp ─────────────────────────────────────────────────────────────
// Hex math helpers, slot management, map generation, and vision encoding.
// Included by Esp32HexMapCrawl.ino before gameplay .hpp files.

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

// Defined in inventory_items.hpp (needs the item registry, which is loaded
// after this file).  Sums +1 per equipped item with EFX_REVEAL_FOG param 1.
static int equipVisionBonus(int pid);

// Defined in world-system.hpp (needs W_hex, which is declared after this
// file). Smoke from a burning hex the player is standing on cuts vision
// independently of the weather phase — see docs/world-system-spec.md.
static int fireVisionPenalty(int q, int r);

// Defined in world-system.hpp, same reason as fireVisionPenalty above.
// Flash-flood intensity adds to move cost instead of cutting vision — see
// survival_state.hpp's move-cost calc.
static int floodMovePenalty(int q, int r);

// Defined in inventory_items.hpp (needs the item registry, like
// equipVisionBonus above). True when the player is CARRYING at least one Bile
// Flare -- the tunnel light source. Carried, not equipped: the flare is
// slot=none, so it can only ever sit in invType[].
static bool hasTunnelLight(int pid);

// Defined in tunnels.hpp (needs G.tunnel and bunkerHatches, both declared
// after this file). Called from the tail of generateMap() below: it places the
// surface hatches against the finished map, then carves the tunnel board.
static void generateTunnels();

// ── Group vision bonus ──────────────────────────────────────────
// Survivors watching the same hex together see farther: +1 vision radius per
// other connected player stacked on pid's hex, capped so a full party stack
// stays within the wire-buffer budget buildVisDisk() callers allocate for.
static constexpr int GROUP_VISION_CAP = 3;
static int groupVisionBonus(int pid) {
  int stacked = 0;
  const Player& me = G.players[pid];
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i == pid || !G.players[i].connected) continue;
    // Depth first. Underground survivors keep q/r pinned to the hatch they
    // descended through, so without this check a player standing ON a hatch
    // would gain a phantom +1 per teammate somewhere far below.
    if (G.players[i].depth != me.depth) continue;
    if (me.depth) {
      if (G.players[i].tq == me.tq && G.players[i].tr == me.tr) stacked++;
    } else if (G.players[i].q == me.q && G.players[i].r == me.r) {
      stacked++;
    }
  }
  return min(stacked, GROUP_VISION_CAP);
}

// ── Wire encoding of one cell (shared by full map, vis disk, survey ring) ──
// TT = terrain, with bit 6 set when the hex holds an improved (level 2)
//      shelter, bit 7 set when the caravan has driven through (tireTrack).
//      Fog is 0xFF and is tested for equality before decoding. Terrain
//      itself only needs bits 0-3 (NUM_TERRAIN=16), so bits 6/7 are free
//      for these flags without widening the byte.
// DD = bits 0-5 footprints, bit 6 any shelter, bit 7 POI present.
// VV = resource type << 4 | image variant (resource masked when maskRes).
static inline void encodeCell(const HexCell& cell, bool maskRes,
                              uint8_t* tt, uint8_t* dd, uint8_t* vv) {
  *tt = cell.terrain | (cell.shelter >= 2 ? 0x40 : 0x00) | (cell.tireTrack ? 0x80 : 0x00);
  *dd = (cell.footprints & 0x3F) | ((cell.shelter ? 1 : 0) << 6) | (cell.poi ? 0x80 : 0x00);
  *vv = (maskRes ? 0 : (cell.resource << 4)) | (cell.variant & 0x0F);
}

// ── Effective vision parameters for a player ──────────────────
// Call while holding G.mutex (reads map terrain at player position).
static void playerVisParams(int pid, int* outVisR, bool* outMaskRes) {
  // ── Underground ────────────────────────────────────────────────────────
  // A separate formula, not the surface one with modifiers: terrain vision,
  // weather and smoke are all meaningless in a bunker corridor. You see your
  // own hex and one ring; a carried light adds one; a Scout adds one more
  // (the surface Scout bonus is +2 -- down here it is deliberately tighter).
  // The flare check lives in inventory_items.hpp and scans invType[], not
  // equip[]: a Bile Flare is slot=none and can only ever be carried.
  if (G.players[pid].depth) {
    int vr = TUNNEL_VIS_BASE;
    vr += hasTunnelLight(pid) ? 1 : 0;
    if (G.players[pid].archetype == 4) vr += TUNNEL_VIS_SCOUT;
    vr += groupVisionBonus(pid);
    *outVisR    = max(0, vr);
    *outMaskRes = false;
    return;
  }
  uint8_t t = G.map[G.players[pid].r][G.players[pid].q].terrain;
  if (t >= NUM_TERRAIN) t = 0;
  int8_t vl = TERRAIN_VIS[t];
  if      (vl <= -3) { *outVisR = 0;            *outMaskRes = false; }
  else if (vl == -2) { *outVisR = 1;            *outMaskRes = true;  }
  else if (vl == -1) { *outVisR = 2;            *outMaskRes = false; }
  else if (vl ==  0) { *outVisR = VISION_R;     *outMaskRes = false; }
  else if (vl ==  1) { *outVisR = VISION_R + 1; *outMaskRes = false; }
  else               { *outVisR = VISION_R + 2; *outMaskRes = false; }
  if (G.players[pid].archetype == 4) *outVisR += 2;  // Scout: +2 vision radius
  *outVisR += equipVisionBonus(pid);  // EFX_REVEAL_FOG param 1 on equipped items
  *outVisR += groupVisionBonus(pid);  // allies stacked on the same hex
  // ── Weather visibility penalty (applied after Scout and equipment bonuses) ──
  *outVisR = max(0, *outVisR - (int)WEATHER_VIS_PENALTY[G.weatherPhase]);
  // ── Fire smoke (independent of weather) ─────────────────────────────────
  *outVisR = max(0, *outVisR - fireVisionPenalty(G.players[pid].q, G.players[pid].r));
}

// ── Slot management ────────────────────────────────────────────
static int findSlot(uint32_t id) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (G.players[i].connected && G.players[i].wsClientId == id) return i;
  return -1;
}
// Every event gets the next seq whether or not it fits; the ones that do not
// fit are counted in g_evtDrops, since a gap in "sq" alone can also be an
// event this socket was simply not sent (see GameEvent::seq).
static void enqEvt(GameEvent ev) {
  taskENTER_CRITICAL(&evtMux);
  ev.seq = ++g_evSeq;
  if (pendingCount < EVT_QUEUE_SIZE) pendingEvents[pendingCount++] = ev;
  else                               g_evtDrops++;
  taskEXIT_CRITICAL(&evtMux);
}

// ── Terrain resource spawn helper ──────────────────────────────
// Water is the binding survival constraint.  ACT_WATER needs Marsh, Flooded
// or River terrain — about 4.7% of the map once the raft-gated River is
// excluded — so nearly all of it has to come from piles, and bot runs
// measured survivors carrying no water 41-54% of the time.
//
// This upgrades a small share of non-water rolls to water, which raises the
// water pile count ~15% without touching pile density or any other resource's
// absolute count.  Calibrated against the shipped spawn table weighted by a
// real generated map: water is 18.4% of spawns (predicted 18.38%, measured
// 18.47% on hardware), and 34/1000 of the remainder takes that to 21.2%.
static constexpr uint16_t WATER_BONUS_PERMILLE = 34;

static uint8_t terrainSpawnResBase(uint8_t t, uint32_t rnd) {
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
    case 14: return (rnd & 1) ? 1 : 5;  // Tunnel Floor: cistern seeps / bunker scrap
    default: return 0;
  }
}

static uint8_t terrainSpawnRes(uint8_t t, uint32_t rnd) {
  uint8_t res = terrainSpawnResBase(t, rnd);
  // Fresh entropy rather than more bits of `rnd`: Phase 3 passes only 8 bits
  // here, and the base table already consumes the low ones via `% 5` / `& 1`,
  // so a derived roll would correlate with the very resource it is meant to
  // replace.
  if (res != 0 && res != 1 && (esp_random() % 1000) < WATER_BONUS_PERMILLE)
    return 1;
  return res;
}

// ── Map generation data ────────────────────────────────────────
// Image variant counts (filled from SD scan before generateMap)
static uint8_t terrainVariantCount[NUM_TERRAIN] = {};  // 0 = no variants found
static uint8_t shelterVariantCount[2]           = {};  // [0]=basic, [1]=improved
static uint8_t forrageAnimalCount               = 0;   // forrageAnimal<N>.png

// Rank-quadratic weighted pick: variant 0 has weight (n)^2, variant n-1 has weight 1.
static uint8_t pickVariant(uint8_t n, uint32_t rnd) {
  if (n <= 1) return 0;
  uint32_t total = 0;
  for (uint8_t i = 0; i < n; i++) { uint32_t w = (uint32_t)(n - i) * (n - i); total += w; }
  uint32_t r = rnd % total;
  for (uint8_t i = 0; i < n; i++) {
    uint32_t w = (uint32_t)(n - i) * (n - i);
    if (r < w) return i;
    r -= w;
  }
  return 0;
}

// 12-15 are 0: the bunker tunnel terrains are never drawn by the terrain
// lottery, they are stamped explicitly by generateTunnels() -- same as the
// already-zero Flooded Ruins(5) and Glass Fields(6), which later phases place.
//
// Broken Urban(4) is 0 for the same reason: cities are stamped as clusters by
// Phase 2.55, not rolled per-hex. It used to sit at 2 and the result was ~33
// scattered single hexes -- see the note on TERRAIN_CLUMP[4] for why the
// smoothing passes could never let them clump. Its 2 points went to Open
// Scrub (66 -> 68) to keep this table summing to exactly 100: Phase 1 falls
// through to NUM_TERRAIN-1 (Collapsed Tunnel!) for any roll above the total.
static const uint8_t T_BASE[NUM_TERRAIN]  = { 68,  8, 12,  3,  0,  0,  0,  3,  2,  1,  3,  0,  0,  0,  0,  0 };

// Clump % per terrain
// NOTE: Open Scrub's base rate (66%) already lets it win most contested
// smoothing cells on frequency alone — it doesn't need a high clump value
// to survive the way a rare terrain like Rust Forest (12%, clump 75) does.
// This value mainly controls Scrub's own internal texture: too low and its
// interior looks noisy/speckled, too high and it starts steamrolling minor
// terrains at their boundaries too. 30 was picked to firm up grassland into
// broad fields while staying well under Marsh/Hills/Dunes (40-45).
static const uint8_t TERRAIN_CLUMP[NUM_TERRAIN] = {
  30,  // 0 Open Scrub
  40,  // 1 Ash Dunes
  75,  // 2 Rust Forest
  45,  // 3 Marsh
  35,  // 4 Broken Urban  -- inert: T_BASE[4] is 0, so its weight is always 0.
       //   Kept as documentation of why cities are stamped instead of rolled:
       //   weight = T_BASE * (100 + CLUMP * n), so urban ringed by six urban
       //   neighbours scored 2*310 = 620 against Open Scrub's *floor* of
       //   66*100 = 6600. A hex surrounded by city still picked scrub 10:1 --
       //   Broken Urban was mathematically incapable of clumping.
  50,  // 5 Flooded Ruins
  35,  // 6 Glass Fields
  40,  // 7 Rolling Hills
  55,  // 8 Mountain
  10,  // 9 Settlement
  55,  // 10 Nuke Crater
  95,  // 11 River Channel
   0,  // 12 Bunker Entrance  -- placed by generateTunnels(), never clumped
   0,  // 13 Vent Shaft       -- ditto
   0,  // 14 Tunnel Floor     -- tunnel board only
   0,  // 15 Collapsed Tunnel -- tunnel board only
};

static constexpr uint8_t SMOOTH_PASSES = 3;

// ── City records ───────────────────────────────────────────────────────────
// Filled by Phase 2.55. Kept only for the post-generation log line -- every
// later phase re-derives "is this downtown?" from the map itself by counting
// urban neighbours, so nothing depends on these surviving.
enum : uint8_t { CITY_RUINS = 0, CITY_GROUND_ZERO, CITY_DROWNED, CITY_LIVING };
static constexpr uint8_t MAX_CITIES     = 8;
static constexpr uint8_t MAX_CITY_CELLS = 22;
struct CityInfo { uint8_t q, r, size, arch; };
static CityInfo cities[MAX_CITIES];
static uint8_t  cityCount = 0;

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
      cell.shelter      = 0;
      cell.footprints   = 0;
      cell.tireTrack    = 0;
    }
  }

  // ── Phase 1.5: Forest Belt Pre-seeding ──────────────────────────
  {
    static const uint8_t AXIS_A[3] = { 0, 1, 2 };
    static const uint8_t AXIS_B[3] = { 3, 4, 5 };
    int numBelts = 36 + (int)(esp_random() % 9);
    for (int b = 0; b < numBelts; b++) {
      int bRow    = (int)(esp_random() % MAP_ROWS);
      int bCol    = (int)(esp_random() % MAP_COLS);
      int ax      = (int)(esp_random() % 3);
      int halfLen = 4 + (int)(esp_random() % 3);
      for (int side = 0; side < 2; side++) {
        int dir = (side == 0) ? AXIS_A[ax] : AXIS_B[ax];
        int cr = bRow, cc = bCol;
        for (int step = 0; step <= halfLen; step++) {
          uint8_t t = G.map[cr][cc].terrain;
          if (t != 8 && t != 10) G.map[cr][cc].terrain = 2;
          cr = wrapR(cr + DR[dir]);
          cc = wrapQ(cc + DQ[dir]);
        }
      }
    }
  }

  // ── Phase 2: clump smoothing passes ──────────────────────────
  PSRAM_STATIC(uint8_t, scratch, [MAP_ROWS][MAP_COLS]);

  for (int pass = 0; pass < SMOOTH_PASSES; pass++) {
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        uint8_t nCount[NUM_TERRAIN] = {0};
        for (int d = 0; d < 6; d++)
          nCount[G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain]++;

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
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.5: mountain-rolling hills transition pass ────────
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      scratch[r][c] = G.map[r][c].terrain;

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      if (G.map[r][c].terrain == 8) continue;
      uint8_t mNeigh = 0;
      for (int d = 0; d < 6; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 8) mNeigh++;
      if (mNeigh == 0) continue;
      uint32_t v    = (uint32_t)mNeigh * 40u;
      uint8_t  prob = (v >= 95u) ? 95u : (uint8_t)v;
      if ((esp_random() % 100) < prob)
        scratch[r][c] = 7;
    }
  }
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      G.map[r][c].terrain = scratch[r][c];

  // ── Phase 2.55: City stamp ─────────────────────────────
  // Broken Urban is placed as whole cities here rather than rolled per-hex in
  // Phase 1. Position in the pipeline is load-bearing in both directions:
  //
  //   AFTER the smoothing passes (Phase 2) -- they would shred a city. Urban
  //   loses every contested cell to Open Scrub roughly 10:1 even when ringed
  //   by six urban neighbours (see TERRAIN_CLUMP[4]). Nothing softens these
  //   footprints afterwards, which is why growth below is a ragged accretion
  //   walk rather than a hex disk -- a disk would read as obviously stamped.
  //
  //   BEFORE Phase 2.6 -- the Glass Fields halo only fires next to an urban
  //   hex that ITSELF has an urban neighbour. With scattered singletons that
  //   condition was never true and the pass was dead code; real clusters ring
  //   every city in fused glass for free. Note it skips terrain 4 outright,
  //   so a Ground Zero crater never glasses its own city.
  //
  //   BEFORE Phase 2.7/2.82 -- the river paints straight through a city (only
  //   mountain and crater stop it), and 2.82 then floods urban river banks at
  //   70% against 20% for everything else. A channel through downtown turns
  //   it into a drowned quarter on its own. That is intended: the crossing
  //   needs the raft, and the arc-overlap seal that makes the river a real
  //   divide must not be punched open just to spare a city.
  {
    cityCount = 0;
    const uint8_t MIN_SEP = 8;                              // cities are separate places
    uint8_t  want = 5 + (uint8_t)(esp_random() % 3);        // 5..7
    uint16_t foot[MAX_CITY_CELLS];

    for (uint16_t attempt = 0; cityCount < want && cityCount < MAX_CITIES && attempt < 900; attempt++) {
      int     cr = (int)(esp_random() % MAP_ROWS);
      int     cq = (int)(esp_random() % MAP_COLS);
      uint8_t st = G.map[cr][cq].terrain;
      if (st == 8 || st == 9 || st == 10) continue;         // no mountain/settlement/crater seeds

      bool clear = true;
      for (uint8_t i = 0; i < cityCount && clear; i++)
        if (hexDistWrap(cq, cr, (int)cities[i].q, (int)cities[i].r) < MIN_SEP) clear = false;
      if (!clear) continue;

      // Size: the first city is always a real one so every world has somewhere
      // worth mounting an expedition to; the rest skew small.
      uint8_t target;
      if (cityCount == 0) target = 14 + (uint8_t)(esp_random() % 9);   // 14..22
      else {
        uint8_t sz = (uint8_t)(esp_random() % 100);
        if      (sz < 50) target =  4 + (uint8_t)(esp_random() % 4);   // township 4..7
        else if (sz < 85) target =  8 + (uint8_t)(esp_random() % 6);   // town     8..13
        else              target = 14 + (uint8_t)(esp_random() % 9);   // city    14..22
      }
      if (target > MAX_CITY_CELLS) target = MAX_CITY_CELLS;

      uint8_t arch;
      uint8_t ar = (uint8_t)(esp_random() % 100);
      if      (ar < 45) arch = CITY_RUINS;
      else if (ar < 75) arch = CITY_GROUND_ZERO;
      else if (ar < 90) arch = CITY_DROWNED;
      else              arch = CITY_LIVING;
      // A crater needs a city around it to read as ground zero rather than as
      // a crater with debris; a five-hex township has no ring to spare.
      if (arch == CITY_GROUND_ZERO && target < 8) arch = CITY_RUINS;

      // ── Eden accretion ──────────────────────────────────
      // Grow by repeatedly picking a random cell already in the footprint and
      // a random direction off it. Produces the lobed, irregular outline a
      // ruin wants. O(size^2) membership scan is fine at size <= 22.
      uint8_t n = 0;
      foot[n++] = (uint16_t)(cr * MAP_COLS + cq);
      for (uint16_t grow = 0; n < target && grow < 500; grow++) {
        uint16_t src2 = foot[esp_random() % n];
        int      sr2  = (int)(src2 / MAP_COLS);
        int      sq2  = (int)(src2 % MAP_COLS);
        int      d    = (int)(esp_random() % 6);
        int      nr   = wrapR(sr2 + DR[d]);
        int      nq   = wrapQ(sq2 + DQ[d]);
        uint8_t  nt   = G.map[nr][nq].terrain;
        if (nt == 8 || nt == 9) continue;                   // never eat mountain or a settlement
        uint16_t key = (uint16_t)(nr * MAP_COLS + nq);
        bool dup = false;
        for (uint8_t i = 0; i < n && !dup; i++) if (foot[i] == key) dup = true;
        if (dup) continue;
        foot[n++] = key;
      }

      for (uint8_t i = 0; i < n; i++)
        G.map[foot[i] / MAP_COLS][foot[i] % MAP_COLS].terrain = 4;

      if (arch == CITY_GROUND_ZERO) {
        // Force the full ring before stamping, so accretion's raggedness can't
        // leave ground zero open to the countryside. The crater is MC 255:
        // downtown becomes a wall you walk around, never through.
        for (int d = 0; d < 6; d++) {
          int     nr = wrapR(cr + DR[d]);
          int     nq = wrapQ(cq + DQ[d]);
          uint8_t nt = G.map[nr][nq].terrain;
          if (nt == 8 || nt == 9) continue;
          G.map[nr][nq].terrain = 4;
        }
        G.map[cr][cq].terrain = 10;
      } else if (arch == CITY_DROWNED) {
        // A sunken quarter out on the fringe -- burst mains and collapsed
        // basements, not a riverbank. The core stays dry and salvageable.
        uint8_t wet = 2 + (uint8_t)(esp_random() % 3);
        for (uint8_t i = 0; i < n && wet; i++) {
          int fr = (int)(foot[i] / MAP_COLS);
          int fq = (int)(foot[i] % MAP_COLS);
          if (hexDistWrap(fq, fr, cq, cr) < 2) continue;
          G.map[fr][fq].terrain = 5;
          wet--;
        }
      } else if (arch == CITY_LIVING) {
        // Survivors squatting at the edge of the ruins. Open Scrub only, so we
        // never trade away another terrain's feature for it.
        for (uint8_t i = 0; i < n; i++) {
          int fr = (int)(foot[i] / MAP_COLS);
          int fq = (int)(foot[i] % MAP_COLS);
          int d  = (int)(esp_random() % 6);
          int nr = wrapR(fr + DR[d]);
          int nq = wrapQ(fq + DQ[d]);
          if (G.map[nr][nq].terrain != 0) continue;
          G.map[nr][nq].terrain = 9;
          break;
        }
      }

      // Outskirts: loose rubble on the approach so a city fades in instead of
      // starting at a hard edge. These land as isolated hexes with no urban
      // neighbour of their own, so Phase 2.6 will NOT glass-ring them -- the
      // fused-glass halo stays a reliable marker of a real city.
      uint8_t scatter = 1 + (uint8_t)(esp_random() % 4);
      for (uint8_t s = 0; s < scatter; s++) {
        int rr = cr, rq = cq;
        int steps = 2 + (int)(esp_random() % 3);
        for (int k = 0; k < steps; k++) {
          int d = (int)(esp_random() % 6);
          rr = wrapR(rr + DR[d]);
          rq = wrapQ(rq + DQ[d]);
        }
        uint8_t ot = G.map[rr][rq].terrain;
        if (ot == 4 || ot == 8 || ot == 9 || ot == 10) continue;
        G.map[rr][rq].terrain = 4;
      }

      cities[cityCount].q    = (uint8_t)cq;
      cities[cityCount].r    = (uint8_t)cr;
      cities[cityCount].size = n;
      cities[cityCount].arch = arch;
      cityCount++;
    }

    // Lone ruins: true map-wide scatter with no city attached -- a collapsed
    // overpass in open country. Placed after smoothing, so exactly as many
    // survive as we ask for. Open ground only; they must not carve up forest,
    // marsh or anything else with its own identity.
    uint8_t lone = 10 + (uint8_t)(esp_random() % 7);        // 10..16
    for (uint16_t attempt = 0; lone && attempt < 600; attempt++) {
      int     r2 = (int)(esp_random() % MAP_ROWS);
      int     c2 = (int)(esp_random() % MAP_COLS);
      uint8_t t2 = G.map[r2][c2].terrain;
      if (t2 != 0 && t2 != 1 && t2 != 7) continue;          // scrub / dunes / hills
      G.map[r2][c2].terrain = 4;
      lone--;
    }

    static const char* CITY_ARCH_NAME[4] = { "ruins", "groundzero", "drowned", "living" };
    for (uint8_t i = 0; i < cityCount; i++)
      Log.notice("City %u: (%d,%d) size=%u %s", (unsigned)i, (int)cities[i].q, (int)cities[i].r,
                 (unsigned)cities[i].size, CITY_ARCH_NAME[cities[i].arch]);
  }

  // ── Phase 2.6: Glass Fields placement ────────────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      if (G.map[r][c].terrain == 6) G.map[r][c].terrain = 0;

  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      scratch[r][c] = G.map[r][c].terrain;

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t t = G.map[r][c].terrain;
      // 5 and 9 joined this list with Phase 2.55. A city's Drowned Quarter
      // and its fringe Settlement both sit adjacent to qualifying urban hexes,
      // so without them the halo below would fuse away the better part of both
      // archetypes at 55% -- a Living Ruins city would lose its survivors
      // about half the time. Neither should glass on its own terms either:
      // standing water does not fuse, and a settlement is scarce, guaranteed
      // terrain carrying 21 encounters. This also closes the pre-existing case
      // of a lottery crater glassing a settlement that happened to spawn beside
      // it (the Phase 2.9 minimum pass already refuses such crater sites).
      if (t == 4 || t == 5 || t == 8 || t == 9 || t == 10) continue;

      bool nearCrater = false;
      for (int d = 0; d < 6 && !nearCrater; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 10) nearCrater = true;
      if (nearCrater) {
        if ((esp_random() % 100) < 80) scratch[r][c] = 6;
        continue;
      }

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

  // ── Phase 2.7: River Channel placement ───────────────────────────
  // One continuous snaking river, usually 2 hexes wide, with 1-wide tendril
  // creeks running off it. The axis is a coin flip, so a world reads as
  // either an east/west river or a north/south one.
  //
  // The channel is stored as ONE CONTIGUOUS ARC per step — cross-coordinates
  // A[i] .. A[i]+W[i]-1, kept unwrapped and wrapped only at paint time.
  // Whether the channel actually stops a unit rests entirely on consecutive
  // arcs overlapping:
  //
  //     A[i+1] <= B[i]   &&   B[i+1] >= A[i] - 1
  //
  // Width is NOT what seals the channel: a straight 1-wide line seals, while
  // a sloppy 3-wide one leaks — a unit steps diagonally between two arcs that
  // only touch at a corner. 2 wide simply buys room to meander; the clamp
  // below is what keeps the channel unbroken.
  //
  // This is a wall, not a partition. The map is a torus, and a single closed
  // curve never separates a torus, so a survivor without river gear can
  // always detour the long way round the world rather than cross. That is
  // deliberate: the raft buys the shortcut, not the only route.
  {
    const uint8_t T_RIVER = 11;
    const int MEANDER_MAX = 6;    // corridor half-width the channel wanders in
    const int CLOSE_STEPS = 12;   // steps spent steering back to close the loop

    const bool vertical = (esp_random() & 1);
    const int  AXIS_N   = vertical ? MAP_ROWS : MAP_COLS;  // steps around the loop
    const int  CROSS_N  = vertical ? MAP_COLS : MAP_ROWS;  // span the arc lives in

    // Function-local, not stack: generateMap() runs on the websocket task
    // during a regen and this table is not worth the stack risk.
    static int16_t rA[MAP_COLS];
    static uint8_t rW[MAP_COLS];

    const int base = (int)(esp_random() % CROSS_N);

    int meander = 0, mDir = 0, mRun = 0;
    int w = 2, wRun = 0, sideW = 0;
    int prevA = 0, prevB = 0;

    for (int i = 0; i < AXIS_N; i++) {
      // Width persists in runs so the channel keeps a consistent bank instead
      // of shimmering between 1 and 3 every step.
      if (--wRun <= 0) {
        uint32_t v = esp_random() % 100;
        w = (v < 62) ? 2 : (v < 88) ? 3 : 1;
        wRun = 4 + (int)(esp_random() % 7);
        if ((esp_random() % 100) < 25) sideW ^= 1;
      }

      // Meander: bounded random walk held on one heading for several steps at
      // a time, which reads as lazy sweeping bends rather than jitter. Driven
      // back to 0 over the last CLOSE_STEPS so the loop closes on its start.
      int tail = AXIS_N - 1 - i;
      if (tail <= CLOSE_STEPS) {
        mDir = (meander > 0) ? -1 : (meander < 0) ? 1 : 0;
      } else if (--mRun <= 0) {
        uint32_t v = esp_random() % 100;
        mDir = (v < 45) ? -1 : (v < 90) ? 1 : 0;
        mRun = 4 + (int)(esp_random() % 9);
      }
      meander += mDir;
      if (meander >  MEANDER_MAX) { meander =  MEANDER_MAX; mDir = -1; }
      if (meander < -MEANDER_MAX) { meander = -MEANDER_MAX; mDir =  1; }

      int a = base + meander;
      if (sideW) a -= (w - 1);          // grow the extra column on the low side

      if (i > 0) {
        // ── HARD INVARIANT — channel integrity. Never remove. ──
        if (a > prevB)             a = prevB;
        if (a + w - 1 < prevA - 1) a = prevA - w;
      }
      rA[i] = (int16_t)a;
      rW[i] = (uint8_t)w;
      prevA = a;
      prevB = a + w - 1;
    }

    // The axis wraps, so the last arc must also seal against arc 0.
    {
      int a0 = rA[0], b0 = a0 + rW[0] - 1;
      int a  = rA[AXIS_N - 1], w2 = rW[AXIS_N - 1];
      if (a0 > a + w2 - 1) w2 = a0 - a + 1;
      if (b0 < a - 1)      { w2 = a - b0; a = b0 + 1; }
      rA[AXIS_N - 1] = (int16_t)a;
      rW[AXIS_N - 1] = (uint8_t)w2;
    }

    // Paint. Mountain(8) IS overwritten: TERRAIN_MC[8] is 4, i.e. walkable, so
    // leaving mountains standing would break the channel wherever it meets a
    // range. Crater(10) is MC 255 and blocks on its own, so it may stay.
    for (int i = 0; i < AXIS_N; i++)
      for (int k = 0; k < rW[i]; k++) {
        int x  = rA[i] + k;
        int rr = vertical ? i : wrapR(x);
        int cc = vertical ? wrapQ(x) : i;
        if (G.map[rr][cc].terrain != 10) G.map[rr][cc].terrain = T_RIVER;
      }

    // Tendrils: 1-wide dead-end creeks off the channel. They stop on contact
    // with existing water, because a tendril that loops back onto the trunk
    // pinches off a bay no one can ever reach.
    {
      int numTend = 5 + (int)(esp_random() % 4);
      for (int t = 0; t < numTend; t++) {
        int i   = (int)(esp_random() % AXIS_N);
        int off = (esp_random() & 1) ? -1 : rW[i];
        int tq, tr;
        if (vertical) { tr = i;                 tq = wrapQ(rA[i] + off); }
        else          { tr = wrapR(rA[i] + off); tq = i;                 }
        int dir = (int)(esp_random() % 6);
        int len = 3 + (int)(esp_random() % 7);
        for (int s = 0; s < len; s++) {
          uint8_t tt = G.map[tr][tq].terrain;
          if (s > 0 && tt == T_RIVER) break;
          if (tt != 8 && tt != 10) G.map[tr][tq].terrain = T_RIVER;
          if ((esp_random() % 100) < 30)
            dir = (dir + ((esp_random() & 1) ? 1 : 5)) % 6;
          tq = wrapQ(tq + DQ[dir]);
          tr = wrapR(tr + DR[dir]);
        }
      }
    }
  }

  // ── Phase 2.75: Standing ponds ───────────────────────────────────
  // A single channel is only ~150 hexes, against ~800 under the old braided
  // generator, so water would otherwise exist along one line only. These
  // isolated pockets keep fishing, the water action and flash-flood triggering
  // (maybeTriggerFlashFlood searches a small box around a player for a
  // TERRAIN_HAS_WATER hex) spread across the whole map. They run before Phases
  // 2.8/2.82 so they seed forest fringe and flooded ruins the same way the
  // channel does.
  {
    int nPonds = 25 + (int)(esp_random() % 16);
    for (int p = 0, attempt = 0; p < nPonds && attempt < 1500; attempt++) {
      int r = (int)(esp_random() % MAP_ROWS);
      int c = (int)(esp_random() % MAP_COLS);
      uint8_t t = G.map[r][c].terrain;
      if (t != 0 && t != 1 && t != 3 && t != 7) continue;

      // Keep clear of the trunk — a pond hugging the bank is just more river.
      bool nearRiver = false;
      for (int dr = -3; dr <= 3 && !nearRiver; dr++)
        for (int dc = -3; dc <= 3 && !nearRiver; dc++)
          if (G.map[wrapR(r + dr)][wrapQ(c + dc)].terrain == 11) nearRiver = true;
      if (nearRiver) continue;

      uint8_t pt = ((esp_random() % 100) < 45) ? 11 : 3;
      G.map[r][c].terrain = pt;
      int extra = (int)(esp_random() % 3);        // 1..3 hexes
      for (int e = 0; e < extra; e++) {
        int d  = (int)(esp_random() % 6);
        int nr = wrapR(r + DR[d]);
        int nc = wrapQ(c + DQ[d]);
        uint8_t nt = G.map[nr][nc].terrain;
        if (nt == 0 || nt == 1 || nt == 3 || nt == 7) G.map[nr][nc].terrain = pt;
      }
      p++;
    }
  }

  // ── Phase 2.8: Riverine Forest Fringe ───────────────────────────
  // Only organic ground grows a forest fringe: Open Scrub, Marsh, Rolling
  // Hills. Ash Dunes/Glass Fields are excluded — arid or glassed ground
  // doesn't green up just because water is nearby. Broken Urban is also
  // excluded so it stays fully governed by the dedicated flooded-ruins
  // transition below (Phase 2.82) instead of partially pre-empted here.
  {
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        scratch[r][c] = G.map[r][c].terrain;

    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        if (G.map[r][c].terrain != 11) continue;
        for (int d = 0; d < 6; d++) {
          int nr = wrapR(r + DR[d]);
          int nc = wrapQ(c + DQ[d]);
          uint8_t t = G.map[nr][nc].terrain;
          if (t != 0 && t != 3 && t != 7) continue;
          if ((esp_random() % 100) < 50) scratch[nr][nc] = 2;
        }
      }
    }
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.82: Flooded Ruins seeded along rivers ───────────────
  // Flooded Ruins (5) are placed only on non-river hexes that border
  // a River (11). Urban neighbors flood eagerly; other passable
  // terrain floods occasionally. Mountains, craters, settlements,
  // and existing rivers never flood.
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t t = G.map[r][c].terrain;
      if (t == 8 || t == 9 || t == 10 || t == 11) continue;
      bool nearRiver = false;
      for (int d = 0; d < 6 && !nearRiver; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 11) nearRiver = true;
      if (!nearRiver) continue;
      uint8_t prob = (t == 4) ? 70 : 20;
      if ((esp_random() % 100) < prob) G.map[r][c].terrain = 5;
    }
  }

  // ── Phase 2.85: Rust Forest de-speckle ──────────────────────────
  // Isolated single-hex forests are rare: convert lone forest hexes
  // to their dominant non-forest neighbor (or scrub) most of the time.
  {
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        scratch[r][c] = G.map[r][c].terrain;

    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        if (G.map[r][c].terrain != 2) continue;
        uint8_t nCount[NUM_TERRAIN] = {0};
        for (int d = 0; d < 6; d++)
          nCount[G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain]++;
        if (nCount[2] > 0) continue;
        if ((esp_random() % 100) >= 90) continue;
        uint8_t best = 0;
        uint8_t bestN = 0;
        for (int t = 0; t < NUM_TERRAIN; t++) {
          if (t == 2 || t == 8 || t == 9 || t == 10 || t == 11) continue;
          if (nCount[t] > bestN) { bestN = nCount[t]; best = (uint8_t)t; }
        }
        scratch[r][c] = best;
      }
    }
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.86: Grassland consolidation ─────────────────────────
  // A lone Ash Dunes/Marsh/Rolling Hills hex with no same-terrain neighbor,
  // sitting almost entirely inside Open Scrub, reads as single-hex noise
  // rather than a feature. Fold it back into Scrub so grassland reads as
  // broad fields. Terrain with its own identity (forest — already
  // de-speckled above, urban, mountain, river, etc.) is untouched.
  {
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        scratch[r][c] = G.map[r][c].terrain;

    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        uint8_t t = G.map[r][c].terrain;
        if (t != 1 && t != 3 && t != 7) continue;
        uint8_t nCount[NUM_TERRAIN] = {0};
        for (int d = 0; d < 6; d++)
          nCount[G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain]++;
        if (nCount[t] > 0) continue;   // has company of its own kind, leave it
        if (nCount[0] < 5) continue;   // not surrounded by scrub
        scratch[r][c] = 0;
      }
    }
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.9: Guaranteed minimums for Settlement and Nuke Crater ───────────
  {
    const uint8_t MIN_SETTLE = 27;
    const uint8_t MIN_CRATER = 18;

    uint8_t nSettle = 0, nCrater = 0;
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++) {
        uint8_t t = G.map[r][c].terrain;
        if (t == 9)  nSettle++;
        if (t == 10) nCrater++;
      }

    for (uint16_t attempt = 0; nSettle < MIN_SETTLE && attempt < 1800; attempt++) {
      int r = (int)(esp_random() % MAP_ROWS);
      int c = (int)(esp_random() % MAP_COLS);
      if (G.map[r][c].terrain != 0) continue;
      bool ok = true;
      for (int d = 0; d < 6 && ok; d++) {
        uint8_t nt = G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain;
        if (nt == 8 || nt == 9 || nt == 10 || nt == 11) ok = false;
      }
      if (!ok) continue;
      G.map[r][c].terrain = 9;
      nSettle++;
    }

    for (uint16_t attempt = 0; nCrater < MIN_CRATER && attempt < 1800; attempt++) {
      int r = (int)(esp_random() % MAP_ROWS);
      int c = (int)(esp_random() % MAP_COLS);
      if (G.map[r][c].terrain != 0) continue;
      bool ok = true;
      for (int d = 0; d < 6 && ok; d++) {
        uint8_t nt = G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain;
        if (nt == 9 || nt == 10) ok = false;
      }
      if (!ok) continue;
      G.map[r][c].terrain = 10;
      nCrater++;
      for (int d = 0; d < 6; d++) {
        int nr = wrapR(r + DR[d]);
        int nc = wrapQ(c + DQ[d]);
        uint8_t nt = G.map[nr][nc].terrain;
        // River(11) must stay in this list: a crater dropped beside the
        // channel would otherwise glass its bank into Glass Fields (MC 3) and
        // punch a walkable hole straight through the divide.
        if (nt == 4 || nt == 8 || nt == 10 || nt == 11) continue;
        if ((esp_random() % 100) < 80) G.map[nr][nc].terrain = 6;
      }
    }
  }

  // ── Phase 3: resource placement ───────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      uint8_t  t    = cell.terrain;
      if (t == 10 || t == 11) continue;

      uint32_t rnd = esp_random();
      uint8_t  r2  = (rnd >>  8) & 0xFF;
      uint8_t  r3  = (rnd >> 16) & 0xFF;
      uint8_t  r4  = (rnd >> 24) & 0xFF;
      if (r2 % 100 < 19) {
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

  // Downtown art pin: an urban hex with 5+ urban neighbours is the dense core
  // of a real city, not fringe rubble or a lone ruin. Pin it to a sentinel
  // variant past the end of Broken Urban's counted pool (hexBrokenUrban0-9)
  // so the client can map it to dedicated standing-tower art through POI_ART
  // in engine.js, the same trick Phase 5.5 uses for Jack's Chopper.
  //
  // Three sentinels, not one: a simulated 200 worlds pins a median of 11 core
  // hexes, and they cluster inside the same few cities, so a single tile would
  // visibly tile against itself down one street.
  //
  // Safe before that art exists: poiArtFor() returns undefined for an unknown
  // key and the renderer falls back to `variant % poolLength`, so every pin
  // degrades to plain hexBrokenUrban art. It stops being safe the moment a
  // hexBrokenUrban10.png is added -- terrainVariantCount[4] would become 11
  // and pickVariant() could hand out 10 on its own, so the real core tiles
  // must NOT use that filename. See data/img/HEX_TILE_PROMPTS.md.
  static constexpr uint8_t CITY_CORE_V0    = 10;   // POI_ART keys 4_10 .. 4_12
  static constexpr uint8_t CITY_CORE_TILES = 3;    // ceiling is 16: variant is 4 bits on the wire
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c2 = 0; c2 < MAP_COLS; c2++) {
      if (G.map[r][c2].terrain != 4) continue;
      uint8_t un = 0;
      for (int d = 0; d < 6; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c2 + DQ[d])].terrain == 4) un++;
      if (un >= 5)
        G.map[r][c2].variant = CITY_CORE_V0 + (uint8_t)(esp_random() % CITY_CORE_TILES);
    }

  // ── Phase 5: Guaranteed encounter pre-placement ──────────────
  // poi stores the specific encounter ID (1..N) for each hex.
  // Every encounter file is placed at least once on a shuffled hex
  // of the matching terrain.  If the terrain has fewer hexes than
  // encounter files, IDs cycle (highest IDs take priority — lower
  // ones are overwritten and will never appear naturally).
  for (int r2 = 0; r2 < MAP_ROWS; r2++)
    for (int c2 = 0; c2 < MAP_COLS; c2++)
      G.map[r2][c2].poi = 0;

  {
    PSRAM_STATIC(uint16_t, hexBuf, [MAP_ROWS * MAP_COLS]);  // scratch; PSRAM, off both stack and internal .bss
    for (int t = 0; t <= 9; t++) {
      if (encPools[t].count == 0) continue;
      uint8_t n = encPools[t].count;
      // Collect passable hexes of this terrain
      int hexCount = 0;
      for (int r3 = 0; r3 < MAP_ROWS; r3++)
        for (int c3 = 0; c3 < MAP_COLS; c3++)
          if (G.map[r3][c3].terrain == (uint8_t)t)
            hexBuf[hexCount++] = (uint16_t)(r3 * MAP_COLS + c3);
      if (hexCount == 0) {
        continue;
      }
      // Fisher-Yates shuffle
      for (int i = hexCount - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint16_t tmp = hexBuf[i]; hexBuf[i] = hexBuf[j]; hexBuf[j] = tmp;
      }
      // Broken Urban only: stable-partition downtown to the front, so the 24
      // urban encounters land inside cities rather than on the lone ruins and
      // outskirts rubble that Phase 2.55 scatters across open country. The
      // shuffle above already randomised order, so this keeps the choice of
      // WHICH downtown hex random while fixing which KIND of hex wins.
      // Rotation-based and O(n^2), which is nothing at ~90 urban hexes.
      if (t == 4 && hexCount > 1) {
        int w = 0;
        for (int i = 0; i < hexCount; i++) {
          int     hr = (int)(hexBuf[i] / MAP_COLS);
          int     hq = (int)(hexBuf[i] % MAP_COLS);
          uint8_t un = 0;
          for (int d = 0; d < 6; d++)
            if (G.map[wrapR(hr + DR[d])][wrapQ(hq + DQ[d])].terrain == 4) un++;
          if (un < 2) continue;                    // lone ruin / outskirts
          uint16_t v = hexBuf[i];
          for (int k = i; k > w; k--) hexBuf[k] = hexBuf[k - 1];
          hexBuf[w++] = v;
        }
      }
      // Assign encounter IDs 1..n to shuffled hexes (wrap if hexes < n)
      for (int i = 0; i < (int)n; i++) {
        uint16_t hIdx = hexBuf[i % hexCount];
        G.map[hIdx / MAP_COLS][hIdx % MAP_COLS].poi = (uint8_t)(i + 1);
      }
    }
  }

  // ── Phase 5.5: Landmark art pin ──────────────────────────────
  // Jack's Chopper (scrub/19.json) is a named landmark, not empty scrub —
  // force its hex's variant to 10, a sentinel reserved for point-of-interest
  // art rather than a real scrub variant (Open Scrub's counted variants are
  // 0-9). The client maps terrain 0 + variant 10 to a dedicated named image
  // (poi_jacks_chopper.png) instead of a random hexOpenScrub<N>.png — see
  // POI_ART in engine.js. Overrides whatever random variant Phase 4 picked.
  for (int r2 = 0; r2 < MAP_ROWS; r2++)
    for (int c2 = 0; c2 < MAP_COLS; c2++)
      if (G.map[r2][c2].terrain == 0 && G.map[r2][c2].poi == 19)
        G.map[r2][c2].variant = 10;

  // ── Phase 6: bunker tunnels ──────────────────────────────────
  // Last, because it stamps hatch terrain onto finished surface hexes and
  // needs every earlier phase (rivers, craters, settlements) already placed to
  // pick legal, well-spread entrances. Defined in tunnels.hpp.
  generateTunnels();
}

// ── Map encode: fog masked ─────────────────────────────────────
static const char HEX_CH[] = "0123456789ABCDEF";

// ── Tunnel board encode ────────────────────────────────────────
// Same 3-bytes-per-cell wire format as encodeMapFog(), over the (much smaller)
// bunker tunnel board. No wrap: distance is the plain axial one. At 16x10 the
// whole board is 960 hex chars, so this ships in one go on descend rather than
// dribbling in through vis disks.
static int encodeTunnelFog(char* buf, int cap, int pq, int pr, int visR, bool maskRes) {
  int pos = 0;
  for (int r = 0; r < TUN_ROWS; r++) {
    for (int c = 0; c < TUN_COLS; c++) {
      uint8_t tt, dd, vv;
      int dq = c - pq, dr = r - pr;
      int dist = (abs(dq) + abs(dq + dr) + abs(dr)) / 2;
      if (dist <= visR) {
        encodeCell(G.tunnel[r][c], maskRes, &tt, &dd, &vv);
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

static int encodeMapFog(char* buf, int cap, int pq, int pr, int visR, bool maskRes) {
  int pos = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t tt, dd, vv;
      if (hexDistWrap(pq, pr, c, r) <= visR) {
        encodeCell(G.map[r][c], maskRes, &tt, &dd, &vv);
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
// depth 0 walks G.map with toroidal wrap; depth 1 walks the bunker tunnel
// board, which is a different size and does NOT wrap -- off-board cells are
// simply omitted rather than wrapping round to the far wall. The "dp" field
// tells the client which board to apply the cells to; it is omitted at depth
// 0 so surface traffic is byte-identical to before.
static int buildVisDisk(char* buf, int cap, int pq, int pr, int visR, bool maskRes,
                        int* outCells = nullptr, uint8_t depth = 0) {
  int pos   = 0;
  int cells = 0;
  if (depth)
    pos += snprintf(buf, cap, "{\"t\":\"vis\",\"dp\":1,\"vr\":%d,\"q\":%d,\"r\":%d,\"cells\":\"", visR, pq, pr);
  else
    pos += snprintf(buf, cap, "{\"t\":\"vis\",\"vr\":%d,\"q\":%d,\"r\":%d,\"cells\":\"", visR, pq, pr);
  for (int dr = -visR; dr <= visR; dr++) {
    for (int dq = -visR; dq <= visR; dq++) {
      int s = -(dq + dr);
      if (abs(dq) + abs(dr) + abs(s) > 2 * visR) continue;
      int cq, cr;
      if (depth) {
        cq = pq + dq; cr = pr + dr;
        // Walled, not toroidal. Spelled out rather than calling tunIn() --
        // that lives in tunnels.hpp, which is included after this file.
        if (cq < 0 || cq >= TUN_COLS || cr < 0 || cr >= TUN_ROWS) continue;
      } else {
        cq = wrapQ(pq + dq); cr = wrapR(pr + dr);
      }
      uint8_t tt, dd, vv;
      encodeCell(depth ? G.tunnel[cr][cq] : G.map[cr][cq], maskRes, &tt, &dd, &vv);
      if (pos + 12 < cap) {  // reserve 2 extra bytes for closing `"}` + snprintf null
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
static int buildSurveyDisk(char* buf, int cap, int pq, int pr, int visR, int pid) {
  int pos = snprintf(buf, cap, "{\"t\":\"ev\",\"k\":\"surv\",\"pid\":%d,\"cells\":\"", pid);
  int ring = visR + 1;
  for (int dr = -ring; dr <= ring; dr++) {
    for (int dq = -ring; dq <= ring; dq++) {
      int s = -(dq + dr);
      if ((abs(dq) + abs(dr) + abs(s)) / 2 != ring) continue;
      int cq = wrapQ(pq + dq);
      int cr = wrapR(pr + dr);
      if (pos + 12 < cap) {  // reserve 2 extra bytes for closing `"}` + snprintf null
        uint8_t tt, dd, vv;
        encodeCell(G.map[cr][cq], false, &tt, &dd, &vv);
        buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
        buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
        buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      }
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  return pos;
}
