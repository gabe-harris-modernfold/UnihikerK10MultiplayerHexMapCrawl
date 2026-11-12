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

// Defined in traps.hpp (needs the encounter pools' trap tiers and the tunnel
// board's hatch table). The very last generation phase: arms traps on both
// boards, skipping POI hexes and hatches.
static void placeTraps();

// ── Group vision bonus ──────────────────────────────────────────
// Survivors watching the same hex together see farther: +1 vision radius per
// other connected player stacked on pid's hex, capped at 3. The cap alone does
// not keep a disk inside the wire buffers -- stacked on top of the terrain,
// Scout and gear bonuses it can reach 12 -- so playerVisParams() also clamps
// the total to VIS_R_MAX.
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
//      Bit 4 is an armed booby trap THIS player knows about (traps.hpp) --
//      per recipient, so pid is the player the cell is being encoded for
//      (-1: nobody's knowledge, e.g. a spectator). Bit 5 stays clear, which
//      is what keeps terrain 15 with every flag set (0xDF) off the 0xFF fog
//      sentinel.
// DD = bits 0-5 footprints, bit 6 any shelter, bit 7 POI present.
// VV = resource type << 4 | image variant (resource masked when maskRes).
static inline bool trapKnownBy(const HexCell& cell, int pid) {
  return pid >= 0 && pid < MAX_PLAYERS && (cell.trap & TRAP_ARMED) && (cell.trap & (1u << pid));
}
static inline void encodeCell(const HexCell& cell, bool maskRes, int pid,
                              uint8_t* tt, uint8_t* dd, uint8_t* vv) {
  *tt = cell.terrain | (cell.shelter >= 2 ? 0x40 : 0x00) | (cell.tireTrack ? 0x80 : 0x00)
      | (trapKnownBy(cell, pid) ? 0x10 : 0x00);
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
  // Last, so a storm still bites into the bonuses rather than the ceiling.
  *outVisR = min(*outVisR, VIS_R_MAX);
}

// ── The sight radius each client was last told ──────────────────
// Sight also changes while a survivor stands still: an ally steps onto or off
// their hex (groupVisionBonus), the last Bile Flare below is burnt or put
// down, smoke reaches their hex. Nothing used to tell the client, so it kept
// drawing the old radius until the next step -- the same staleness the
// equip handlers fixed with pushVisDisk(). Every place that ships a "vr"
// notes it here, and serviceVisRefresh() (network-msg-items.hpp, GameLoop)
// sends a fresh disk to whoever's radius has moved since.
//
// The weather phase rides along because a change of weather is the one change
// that is left for the next step on purpose (see surfaceVis() in the mock): a
// storm rolling in does not blind a survivor who is standing still.
// Written and read under G.mutex -- except forgetVisSent(), which queueSync()
// calls from any task: a lone bool store, and the worst a race can do is one
// refresh too many or one tick late.
static bool    s_visSentKnown[MAX_PLAYERS];
static int8_t  s_visSentR[MAX_PLAYERS];
static uint8_t s_visSentWx[MAX_PLAYERS];
static inline void noteVisSent(int pid, int vr) {
  if (pid < 0 || pid >= MAX_PLAYERS) return;
  s_visSentKnown[pid] = true;
  s_visSentR[pid]     = (int8_t)vr;
  s_visSentWx[pid]    = G.weatherPhase;
}
// A new socket on the seat, or a resync on its way: wait for the sync to set
// the baseline rather than refresh a client that has no board yet.
static inline void forgetVisSent(int pid) {
  if (pid >= 0 && pid < MAX_PLAYERS) s_visSentKnown[pid] = false;
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
// The only free pile on the surface is food: the forage animals renderer.js
// draws on a food hex. Water, fuel, medicine and scrap are never lying about
// on the map -- they come from actions, encounters, trade and what survivors
// drop. The odds are the share of rolls food had in the old five-way table,
// so the animals are exactly as common as they were.
//
// The Tunnel Floor keeps its cistern seeps and bunker scrap: that is the
// tunnel board's own loot, and no forage animal lives down there.
static constexpr uint8_t RES_FOOD = 2;

// Terrain a forage animal can turn up on.
static inline bool terrainGrowsFood(uint8_t t) {
  return t == 0 || t == 2 || t == 3 || t == 9;
}

static uint8_t terrainSpawnRes(uint8_t t, uint32_t rnd) {
  switch (t) {
    case 0: case 9: return (rnd % 5 == 1) ? RES_FOOD : 0;  // Open Scrub, Settlement: 1 roll in 5
    case 2: case 3: return (rnd & 1) ? RES_FOOD : 0;       // Rust Forest, Marsh: 1 in 2
    case 14: return (rnd & 1) ? 1 : 5;  // Tunnel Floor: cistern seeps / bunker scrap
    default: return 0;
  }
}

// What an emptied surface hex's respawn timer is armed to: only a hex that
// lost food grows anything back, so the animals hold steady while a dropped
// pile (or one an older save still has) goes for good once it is taken.
static inline uint8_t respawnTicksFor(uint8_t lostRes) {
  return lostRes == RES_FOOD ? RESPAWN_TICKS : 0;
}

// ── Forest oasis (docs/forest-oasis.md) ────────────────────────
// Five 7-hex groves of Rust Forest a map: a bed in the middle (root hollow or
// den) ringed by dense canopy. Inside one nothing from the world reaches a
// survivor -- weather, fire, lightning, mould, the Doom, Kane, ghosts -- and a
// night's rest heals like a Settlement's. The variant IS the flag: Rust Forest
// cells from 6 on are pinned art (scripts/hex_sheets.py LANDMARKS), so
// pickVariant() never deals 8-12 and a forest hex showing one of them is a
// grove hex. No HexCell field, nothing new on the wire, and the client reads
// the same thing off the variant nibble it already gets.
static constexpr uint8_t OASIS_TERRAIN   = 2;    // Rust Forest
static constexpr uint8_t OASIS_V_HOLLOW  = 8;    // 2_8: root hollow under a fallen tree (centre)
static constexpr uint8_t OASIS_V_DEN     = 9;    // 2_9: a den under the roots (centre)
static constexpr uint8_t OASIS_V_CANOPY  = 10;   // 2_10 .. 2_12: overhead canopy (the ring)
static constexpr uint8_t OASIS_CANOPIES  = 3;
static constexpr uint8_t OASIS_V_LAST    = OASIS_V_CANOPY + OASIS_CANOPIES - 1;
static constexpr uint8_t OASIS_COUNT     = 5;

static inline bool oasisCell(const HexCell& c) {
  return c.terrain == OASIS_TERRAIN && c.variant >= OASIS_V_HOLLOW && c.variant <= OASIS_V_LAST;
}
static inline bool oasisAt(int q, int r) {
  return q >= 0 && q < MAP_COLS && r >= 0 && r < MAP_ROWS && oasisCell(G.map[r][q]);
}
// Under the canopy: on the surface, on a grove hex. q/r stay pinned to the
// hatch while underground, so the depth guard is not optional.
static inline bool inOasis(const Player& p) { return !p.depth && oasisAt(p.q, p.r); }

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

// ── Forest oasis placement (Phase 5.6, and the load path) ─────────────────
// Where the groves stand. Kept for the log only: every rule reads the variant
// off the map (oasisCell), so nothing depends on this.
struct OasisSite { int16_t q, r; };
static OasisSite oases[OASIS_COUNT];
static uint8_t   oasisCount = 0;

// Ground a grove may stand on: the forest itself, or open ground it grows
// over (Open Scrub, Ash Dunes, Rolling Hills). Never water, rock, glass, a
// crater, a town, a city or a hatch. Nothing already on the hex either -- an
// encounter, a trap or a built shelter would be inside the safe ring.
static inline bool oasisGroundOk(const HexCell& c) {
  const uint8_t t = c.terrain;
  if (t != 0 && t != 1 && t != OASIS_TERRAIN && t != 7) return false;
  return !c.poi && !c.trapEnc && !(c.trap & TRAP_ARMED) && !c.shelter;
}

// A grove still standing: a hollow/den centre with all six sides canopy.
static bool oasisIntactAt(int q, int r) {
  const HexCell& c = G.map[r][q];
  if (c.terrain != OASIS_TERRAIN || (c.variant != OASIS_V_HOLLOW && c.variant != OASIS_V_DEN)) return false;
  for (int d = 0; d < 6; d++) {
    const HexCell& n = G.map[wrapR(r + DR[d])][wrapQ(q + DQ[d])];
    if (n.terrain != OASIS_TERRAIN || n.variant < OASIS_V_CANOPY || n.variant > OASIS_V_LAST) return false;
  }
  return true;
}

// Tops the map up to OASIS_COUNT groves. Called at the end of Phase 5.5 on a
// new world, and by tryLoadSave() on a loaded one -- which is how a world
// saved before oases existed gets them, without a SAVE_VERSION bump.
//  1. Count the groves already standing.
//  2. Re-deal every other 8-12 on the forest. An old save dealt 8 and 9 from
//     the pool (Rust Forest had ten), and a stale tiles.json on the card can
//     still say so; a stray one would otherwise be a one-hex oasis.
//  3. Pick the rest one at a time, best score first: deep forest (all seven
//     already forest, more forest round it) against spread (distance to the
//     nearest grove, capped, so far enough is far enough). Centres are never
//     closer than OASIS_MIN_GAP, so two rings never touch. Any of the seven
//     that is not forest yet becomes forest -- a map short of forest still
//     gets its five.
static uint8_t placeForestOases() {   // returns how many it placed
  static constexpr int OASIS_MIN_GAP    = 4;    // centre to centre: one clear hex between rings
  static constexpr int OASIS_SPREAD_CAP = 14;   // past this, distance buys nothing more
  oasisCount = 0;
  for (int r = 0; r < MAP_ROWS && oasisCount < OASIS_COUNT; r++)
    for (int q = 0; q < MAP_COLS && oasisCount < OASIS_COUNT; q++)
      if (oasisIntactAt(q, r)) oases[oasisCount++] = { (int16_t)q, (int16_t)r };
  const uint8_t kept = oasisCount;

  auto inKept = [&](int q, int r) {
    for (uint8_t i = 0; i < oasisCount; i++)
      if (hexDistWrap(q, r, oases[i].q, oases[i].r) <= 1) return true;
    return false;
  };
  const uint8_t pool = min(terrainVariantCount[OASIS_TERRAIN], OASIS_V_HOLLOW);
  int redealt = 0;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int q = 0; q < MAP_COLS; q++)
      if (oasisCell(G.map[r][q]) && !inKept(q, r)) {
        G.map[r][q].variant = pool ? pickVariant(pool, esp_random()) : 0;
        redealt++;
      }

  int grown = 0;
  const uint32_t flip = esp_random();
  while (oasisCount < OASIS_COUNT) {
    int bestScore = -1, bestQ = -1, bestR = -1;
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int q = 0; q < MAP_COLS; q++) {
        int spread = OASIS_SPREAD_CAP;
        for (uint8_t i = 0; i < oasisCount; i++)
          spread = min(spread, hexDistWrap(q, r, oases[i].q, oases[i].r));
        if (spread < OASIS_MIN_GAP) continue;
        if (!oasisGroundOk(G.map[r][q])) continue;
        int forest = (G.map[r][q].terrain == OASIS_TERRAIN);
        bool ok = true;
        for (int d = 0; d < 6 && ok; d++) {
          const HexCell& n = G.map[wrapR(r + DR[d])][wrapQ(q + DQ[d])];
          if (!oasisGroundOk(n)) ok = false;
          else forest += (n.terrain == OASIS_TERRAIN);
        }
        if (!ok) continue;
        int depth = 0;   // forest on the hexes two out
        for (int dq = -2; dq <= 2; dq++)
          for (int dr = -2; dr <= 2; dr++)
            if (abs(dq) + abs(dq + dr) + abs(dr) == 4 &&
                G.map[wrapR(r + dr)][wrapQ(q + dq)].terrain == OASIS_TERRAIN) depth++;
        int score = forest * 6 + depth + spread * 3 + (int)(esp_random() % 4);
        if (score > bestScore) { bestScore = score; bestQ = q; bestR = r; }
      }
    }
    if (bestScore < 0) break;   // nowhere left: no open ground at all
    // The bed alternates hollow / den down the list, from a random start.
    HexCell& centre = G.map[bestR][bestQ];
    if (centre.terrain != OASIS_TERRAIN) { centre.terrain = OASIS_TERRAIN; grown++; }
    centre.variant = ((oasisCount + flip) & 1) ? OASIS_V_DEN : OASIS_V_HOLLOW;
    const uint8_t off = (uint8_t)(esp_random() % OASIS_CANOPIES);
    for (int d = 0; d < 6; d++) {
      HexCell& n = G.map[wrapR(bestR + DR[d])][wrapQ(bestQ + DQ[d])];
      if (n.terrain != OASIS_TERRAIN) { n.terrain = OASIS_TERRAIN; grown++; }
      n.variant = (uint8_t)(OASIS_V_CANOPY + (off + d) % OASIS_CANOPIES);
    }
    oases[oasisCount++] = { (int16_t)bestQ, (int16_t)bestR };
  }
  Log.notice("Oasis: %d groves (%d kept, %d placed), %d hexes grown to forest, %d stray variants re-dealt",
             (int)oasisCount, (int)kept, (int)(oasisCount - kept), grown, redealt);
  for (uint8_t i = 0; i < oasisCount; i++)
    Log.notice("Oasis %d at (%d,%d) %s", (int)i, (int)oases[i].q, (int)oases[i].r,
               G.map[oases[i].r][oases[i].q].variant == OASIS_V_DEN ? "den" : "hollow");
  return (uint8_t)(oasisCount - kept);
}

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
      cell.trap         = 0;
      cell.trapEnc      = 0;
    }
  }
  trailClearAll();   // and which way they went (trails.hpp)

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

  // ── Phase 2.95: Settlement ruins ────────────────────────────────
  // Survivors settle where the salvage is: most settlements sit up against a
  // scrap of old town rather than out in clean scrub. Before this pass only
  // ~7% of settlements touched a ruin (simulated worlds); with it ~70% do,
  // and the urban hexes within 3 of a settlement go from ~0.7 to ~2.7 on
  // average, for ~+40 Broken Urban per world.
  //
  // Each settlement rolls once: an arc of 1-3 CONSECUTIVE ring neighbours --
  // DQ/DR run round the ring in order, so d0..d0+arc-1 is contiguous and the
  // settlement reads as built against one side of a ruin, not haloed by it --
  // then a 0-2 step spill wandering outward from the arc's middle so the ruin
  // trails off into the country instead of stopping at a hard edge.
  //
  // Last of the terrain passes, so the settlement list is final (Phase 2.9
  // adds most of them). Also AFTER Phase 2.6 on purpose: these are not a
  // city, and a fused-glass ring should stay the mark of one. Open ground
  // only, like the lone ruins -- forest, marsh and the rest keep their
  // identity. A settlement already touching urban (Living Ruins, or one that
  // landed against a city edge) is left as it is.
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      if (G.map[r][c].terrain != 9) continue;
      bool touches = false;
      for (int d = 0; d < 6 && !touches; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 4) touches = true;
      if (touches) continue;
      if ((esp_random() % 100) >= 75) continue;             // a quarter stay out in the open

      auto ruin = [](int rr, int cc) {
        uint8_t& t = G.map[rr][cc].terrain;
        if (t == 0 || t == 1 || t == 7) t = 4;              // scrub / dunes / hills
      };
      int d0  = (int)(esp_random() % 6);
      int arc = 1 + (int)(esp_random() % 3);                // 1..3
      for (int k = 0; k < arc; k++) {
        int d = (d0 + k) % 6;
        ruin(wrapR(r + DR[d]), wrapQ(c + DQ[d]));
      }
      int dm    = (d0 + (arc - 1) / 2) % 6;
      int sr    = wrapR(r + DR[dm]);
      int sc    = wrapQ(c + DQ[dm]);
      int spill = (int)(esp_random() % 3);                  // 0..2
      for (int s = 0; s < spill; s++) {
        int d = (dm + 5 + (int)(esp_random() % 3)) % 6;     // dm-1, dm or dm+1: never back toward town
        sr = wrapR(sr + DR[d]);
        sc = wrapQ(sc + DQ[d]);
        ruin(sr, sc);
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
  // Safe before that art exists: a pin with no `poi` entry in tiles.json
  // falls back to `variant % poolLength`, so every pin degrades to plain
  // Broken Urban art. The core tiles go in cells 10-12 of the sheet
  // art/hex-sheets/hexBrokenUrban.png: scripts/hex_sheets.py treats Broken
  // Urban cells from 10 on as landmarks (its LANDMARKS table), so they never
  // join the pool and pickVariant() can't hand out 10 on its own.
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
      // Rotation-based and O(n^2), which is nothing at ~130 urban hexes.
      //
      // Within 2 of a Settlement is not downtown either: Phase 2.95's
      // settlement ruins clump tightly enough to pass the neighbour test, and
      // without this they took ~25% of the deck away from the cities. The arc
      // sits at distance 1 and the first spill step at 2; a second step can
      // reach 3, but out there it is a dead end with one urban neighbour, so
      // radius 2 catches all but ~1 hex a world (sim).
      if (t == 4 && hexCount > 1) {
        int w = 0;
        for (int i = 0; i < hexCount; i++) {
          int     hr = (int)(hexBuf[i] / MAP_COLS);
          int     hq = (int)(hexBuf[i] % MAP_COLS);
          uint8_t un = 0;
          for (int d = 0; d < 6; d++)
            if (G.map[wrapR(hr + DR[d])][wrapQ(hq + DQ[d])].terrain == 4) un++;
          if (un < 2) continue;                    // lone ruin / outskirts
          bool byTown = false;
          for (int dq = -2; dq <= 2 && !byTown; dq++)
            for (int dr = -2; dr <= 2 && !byTown; dr++)
              if (abs(dq + dr) <= 2 &&
                  G.map[wrapR(hr + dr)][wrapQ(hq + dq)].terrain == 9) byTown = true;
          if (byTown) continue;                    // settlement ruins
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
  // 0-9). The client maps terrain 0 + variant 10 to dedicated landmark art
  // (cell 10 of the Open Scrub sheet, tiles.json poi "0_10") instead of a
  // random scrub tile — see terrainTile() in engine.js. Overrides whatever
  // random variant Phase 4 picked.
  //
  // The Perambulator gets the same treatment where it stands on a fixed POI
  // (docs/null-meridian-group.md): scrub/21 -> Open Scrub cell 11, glass/3 ->
  // Glass Fields cell 15 (tiles.json poi "0_11" / "6_15"; scripts/hex_sheets.py
  // LANDMARKS keeps those cells out of the pools, so pickVariant() never deals
  // them). Its third pool, ridge/3, has no pinned tile.
  //
  // Rust Forest (2026-11-04): forest/2 the forester's hut -> cell 6, forest/4
  // the Evacuation Special -> cell 7 (tiles.json poi "2_6" / "2_7"). Cells
  // 8-12 are the forest oasis group, placed by its own phase.
  static const struct { uint8_t terrain, poi, variant; } LANDMARK_PINS[] = {
    { 0, 19, 10 },   // Jack's Chopper
    { 0, 21, 11 },   // the Perambulator on scrub
    { 6,  3, 15 },   // the Perambulator on glass
    { 2,  2,  6 },   // the forester's hut
    { 2,  4,  7 },   // the Evacuation Special
  };
  for (int r2 = 0; r2 < MAP_ROWS; r2++)
    for (int c2 = 0; c2 < MAP_COLS; c2++)
      for (const auto& pin : LANDMARK_PINS)
        if (G.map[r2][c2].terrain == pin.terrain && G.map[r2][c2].poi == pin.poi)
          G.map[r2][c2].variant = pin.variant;

  // ── Phase 5.6: Forest oasis ──────────────────────────────────
  // Five safe groves (docs/forest-oasis.md). After the POIs (Phase 5) and the
  // landmark pins, so a grove never swallows an encounter or pinned art;
  // before the hatches (Phase 6, Open Scrub only, so never on a grove) and
  // the traps (Phase 7, which skips grove hexes).
  placeForestOases();

  // ── Phase 6: bunker tunnels ──────────────────────────────────
  // Last, because it stamps hatch terrain onto finished surface hexes and
  // needs every earlier phase (rivers, craters, settlements) already placed to
  // pick legal, well-spread entrances. Defined in tunnels.hpp.
  generateTunnels();

  // ── Phase 7: booby traps ─────────────────────────────────────
  // After everything that can claim a hex first: POIs (Phase 5, traps never
  // share a hex with one) and the hatches Phase 6 just stamped, on both
  // boards. Defined in traps.hpp; see docs/trap-system-spec.md.
  placeTraps();
}

// ── Map encode: fog masked ─────────────────────────────────────
static const char HEX_CH[] = "0123456789ABCDEF";

// ── Tunnel board encode ────────────────────────────────────────
// Same 3-bytes-per-cell wire format as encodeMapFog(), over the (much smaller)
// bunker tunnel board. No wrap: distance is the plain axial one. At 16x10 the
// whole board is 960 hex chars, so this ships in one go on descend rather than
// dribbling in through vis disks.
//
// A trap pid knows about is sent whatever the distance, same as the surface
// map below: it is the one thing a survivor can know about a hex they cannot
// see, and without it a reconnect would forget it.
static int encodeTunnelFog(char* buf, int cap, int pq, int pr, int visR, bool maskRes, int pid) {
  int pos = 0;
  for (int r = 0; r < TUN_ROWS; r++) {
    for (int c = 0; c < TUN_COLS; c++) {
      uint8_t tt, dd, vv;
      int dq = c - pq, dr = r - pr;
      int dist = (abs(dq) + abs(dq + dr) + abs(dr)) / 2;
      // A lit cell is in sight from anywhere below (tunnels.hpp, "Section lights").
      if (dist <= visR || trapKnownBy(G.tunnel[r][c], pid) || G.tunnelLit[r][c]) {
        encodeCell(G.tunnel[r][c], maskRes, pid, &tt, &dd, &vv);
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

// The tunnel board's open sides (G.tunnelOp, tunnels.hpp), 2 hex chars a
// cell in the same row-major order as encodeTunnelFog() and for the same
// cells: "00" wherever that sent fog. tsync carries it as "op" beside "map".
static int encodeTunnelOps(char* buf, int cap, int pq, int pr, int visR, int pid) {
  int pos = 0;
  for (int r = 0; r < TUN_ROWS; r++) {
    for (int c = 0; c < TUN_COLS; c++) {
      int dq = c - pq, dr = r - pr;
      int dist = (abs(dq) + abs(dq + dr) + abs(dr)) / 2;
      uint8_t op = (dist <= visR || trapKnownBy(G.tunnel[r][c], pid) || G.tunnelLit[r][c]) ? G.tunnelOp[r][c] : 0;
      if (pos + 2 < cap) { buf[pos++] = HEX_CH[op >> 4]; buf[pos++] = HEX_CH[op & 0xF]; }
    }
  }
  buf[pos] = 0;
  return pos;
}

// The tunnel board's lights (G.tunnelLit, tunnels.hpp "Section lights"), 2
// hex chars a cell in the same order again, for every cell: a light is not
// secret, and only a lit cell is ever non-zero -- which encodeTunnelFog()
// sends unfogged anyway. tsync carries it as "lt".
static int encodeTunnelLights(char* buf, int cap) {
  int pos = 0;
  for (int r = 0; r < TUN_ROWS; r++)
    for (int c = 0; c < TUN_COLS; c++) {
      uint8_t lt = G.tunnelLit[r][c];
      if (pos + 2 < cap) { buf[pos++] = HEX_CH[lt >> 4]; buf[pos++] = HEX_CH[lt & 0xF]; }
    }
  buf[pos] = 0;
  return pos;
}

// Known traps ride outside the vision disk too (see encodeTunnelFog): an
// escaped trap stays on that survivor's map across a reconnect, a reload and
// a brand-new browser, and in any weather.
static int encodeMapFog(char* buf, int cap, int pq, int pr, int visR, bool maskRes, int pid) {
  int pos = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t tt, dd, vv;
      if (hexDistWrap(pq, pr, c, r) <= visR || trapKnownBy(G.map[r][c], pid)) {
        encodeCell(G.map[r][c], maskRes, pid, &tt, &dd, &vv);
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

// ── Trail sides on the wire ────────────────────────────────────
// The "trl" string beside a surface disk's cells: one record per walker per
// cell that has one, 7 hex chars apiece -- QQ col, RR row, S slot (0-5 a
// seat, 6 the caravan), BB the trails.hpp byte. Sparse, because nearly every
// cell has none. A record goes out only while the mark it describes is on
// the cell, so a record outliving its bit can never draw a trail.
// Returns chars written; a record that does not fit whole is left off.
static int appendTrailRecs(char* buf, int cap, int q, int r) {
  if (!trailSides) return 0;
  const HexCell& c = G.map[r][q];
  if (!c.footprints && !c.tireTrack) return 0;
  int pos = 0;
  for (int s = 0; s < TRAIL_SLOTS; s++) {
    uint8_t b = trailSides[r][q][s];
    if (b == TRAIL_NONE) continue;
    bool marked = (s == TRAIL_CARAVAN) ? c.tireTrack : (c.footprints & (1 << s));
    if (!marked) continue;
    if (pos + 7 > cap) break;
    buf[pos++] = HEX_CH[q >> 4]; buf[pos++] = HEX_CH[q & 0xF];
    buf[pos++] = HEX_CH[r >> 4]; buf[pos++] = HEX_CH[r & 0xF];
    buf[pos++] = HEX_CH[s];
    buf[pos++] = HEX_CH[b >> 4]; buf[pos++] = HEX_CH[b & 0xF];
  }
  return pos;
}

// Every record in the surface disk of radius visR round (pq, pr) -- the cells
// encodeMapFog() sends unfogged. No terminator written; cap is what may be used.
static int encodeTrailDisk(char* buf, int cap, int pq, int pr, int visR) {
  int pos = 0;
  for (int dr = -visR; dr <= visR; dr++) {
    for (int dq = -visR; dq <= visR; dq++) {
      if (abs(dq) + abs(dr) + abs(dq + dr) > 2 * visR) continue;
      pos += appendTrailRecs(buf + pos, cap - pos, wrapQ(pq + dq), wrapR(pr + dr));
    }
  }
  return pos;
}

// ── Build full vision-disk message ─────────────────────────────
// depth 0 walks G.map with toroidal wrap; depth 1 walks the bunker tunnel
// board, which is a different size and does NOT wrap -- off-board cells are
// simply omitted rather than wrapping round to the far wall. The "dp" field
// tells the client which board to apply the cells to; it is omitted at depth
// 0 so surface traffic is byte-identical to before. Underground an "op"
// string follows the cells: each one's open sides (G.tunnelOp), 2 hex chars
// apiece in the same order, with room reserved for it as the cells go in --
// and after it an "lt" string, each cell's light (G.tunnelLit), the same way.
// Underground the disk also carries every lit cell on the board, wherever it
// is: a lit corridor is in sight from anywhere below (tunnels.hpp).
// On the surface a "trl" string follows instead (appendTrailRecs): the cells
// come first, and the trail records take whatever room is left after them.
// pid is who the disk is for: it decides which traps show (encodeCell).
static int buildVisDisk(char* buf, int cap, int pq, int pr, int visR, bool maskRes,
                        int* outCells = nullptr, uint8_t depth = 0, int pid = -1) {
  int pos   = 0;
  int cells = 0;
  uint8_t ops[TUN_ROWS * TUN_COLS];    // underground only: at most the whole 16x10 board
  uint8_t lts[TUN_ROWS * TUN_COLS];
  // One cell onto the wire. Underground, op and light go into their arrays and
  // room is reserved for both tails: each cell so far plus this one, at 4 hex
  // chars apiece, and the `","op":"` / `","lt":"` that open them.
  auto emit = [&](int cq, int cr) {
    uint8_t tt, dd, vv;
    encodeCell(depth ? G.tunnel[cr][cq] : G.map[cr][cq], maskRes, pid, &tt, &dd, &vv);
    // reserve 2 extra bytes for closing `"}` + snprintf null -- and below,
    // the whole "op"/"lt" tail for every cell so far plus this one, or on the
    // surface the `","trl":"` that opens the trail records
    const int tail = depth ? 20 + 4 * (cells + 1) : 10;
    if (pos + 12 + tail < cap && (!depth || cells < (int)sizeof(ops))) {
      if (depth) { ops[cells] = G.tunnelOp[cr][cq]; lts[cells] = G.tunnelLit[cr][cq]; }
      buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
      buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
      buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
      buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
      buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      cells++;
    }
  };
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
      emit(cq, cr);
    }
  }
  if (depth) {
    // Every lit cell the disk above did not already cover.
    for (int r = 0; r < TUN_ROWS; r++)
      for (int c = 0; c < TUN_COLS; c++) {
        if (!G.tunnelLit[r][c]) continue;
        int dq = c - pq, dr = r - pr;
        if ((abs(dq) + abs(dr) + abs(dq + dr)) / 2 <= visR) continue;
        emit(c, r);
      }
    pos += snprintf(buf + pos, cap - pos, "\",\"op\":\"");
    for (int i = 0; i < cells && pos + 4 < cap; i++) {
      buf[pos++] = HEX_CH[ops[i] >> 4]; buf[pos++] = HEX_CH[ops[i] & 0xF];
    }
    pos += snprintf(buf + pos, cap - pos, "\",\"lt\":\"");
    for (int i = 0; i < cells && pos + 4 < cap; i++) {
      buf[pos++] = HEX_CH[lts[i] >> 4]; buf[pos++] = HEX_CH[lts[i] & 0xF];
    }
  } else {
    pos += snprintf(buf + pos, cap - pos, "\",\"trl\":\"");
    pos += encodeTrailDisk(buf + pos, cap - pos - 3, pq, pr, visR);   // 3: `"}` + null
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  if (outCells) *outCells = cells;
  return pos;
}

// ── Build a survey band: the surface rings past sight ──────────
// Every cell with inner < distance <= outer, as an ev surv: the client draws
// them as a peek beyond its live disk until its next step (network.js
// _evSurv), and keeps the terrain afterwards. SURVEY sends one ring
// (visR, visR + 1); a one-shot reveal_fog item sends (visR, visR + param).
//
// maskRes is the survivor's own (playerVisParams): in Broken Urban the band
// used to go out unmasked, so a survey showed the loot two streets away that
// the street you stand in hides.
static int buildSurveyBand(char* buf, int cap, int pq, int pr, int inner, int outer,
                           bool maskRes, int pid) {
  int pos = snprintf(buf, cap, "{\"t\":\"ev\",\"k\":\"surv\",\"pid\":%d,\"cells\":\"", pid);
  for (int dr = -outer; dr <= outer; dr++) {
    for (int dq = -outer; dq <= outer; dq++) {
      int d = (abs(dq) + abs(dr) + abs(dq + dr)) / 2;
      if (d <= inner || d > outer) continue;
      int cq = wrapQ(pq + dq);
      int cr = wrapR(pr + dr);
      if (pos + 22 < cap) {  // closing `"}` + snprintf null, and the `","trl":"` below
        uint8_t tt, dd, vv;
        encodeCell(G.map[cr][cq], maskRes, pid, &tt, &dd, &vv);
        buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
        buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
        buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      }
    }
  }
  // The band's trail records, as buildVisDisk() sends its disk's.
  pos += snprintf(buf + pos, cap - pos, "\",\"trl\":\"");
  for (int dr = -outer; dr <= outer; dr++) {
    for (int dq = -outer; dq <= outer; dq++) {
      int d = (abs(dq) + abs(dr) + abs(dq + dr)) / 2;
      if (d <= inner || d > outer) continue;
      pos += appendTrailRecs(buf + pos, cap - pos - 3, wrapQ(pq + dq), wrapR(pr + dr));
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  return pos;
}

// SURVEY's ring: one hex beyond visR.
static int buildSurveyDisk(char* buf, int cap, int pq, int pr, int visR, bool maskRes, int pid) {
  return buildSurveyBand(buf, cap, pq, pr, visR, visR + 1, maskRes, pid);
}

// ── The whole surface's terrain, for the Pre-War Net Map (reveal_fog 99) ──
// {"t":"ev","k":"rvl","pid":N,"map":"..."}: 2 hex chars a cell, terrain in
// the high nibble and art variant in the low, row-major like encodeMapFog().
// Terrain and art only: no resources, footprints, shelters or POIs -- a
// pre-war map knows the land, not who is on it. The client files it as
// remembered ground (network.js _evReveal), never as live sight.
// 4275 cells = 8550 chars; cap must hold that plus ~40. Returns 0 if not.
static int buildTerrainReveal(char* buf, int cap, int pid) {
  if (cap < MAP_ROWS * MAP_COLS * 2 + 48) return 0;
  int pos = snprintf(buf, cap, "{\"t\":\"ev\",\"k\":\"rvl\",\"pid\":%d,\"map\":\"", pid);
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      const HexCell& cell = G.map[r][c];
      buf[pos++] = HEX_CH[cell.terrain & 0x0F];
      buf[pos++] = HEX_CH[cell.variant & 0x0F];
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  return pos;
}
