#pragma once
// ── tunnels.hpp — the bunker tunnel system ──────────────────────────────────
// A second, much smaller hex board (TUN_COLS x TUN_ROWS) shared by every
// player, reached through hatch hexes scattered on the surface map.  Moving a
// few hexes underground can replace twenty on the surface — but it costs 2 MP
// a step, the air is bad, and the light only reaches so far.
//
// Included from Esp32HexMapCrawl.ino after world-system.hpp.  The sizes,
// BunkerHatch and the G.tunnel pointer live in the .ino because GameState and
// SaveHeader both need them before this file is parsed.
//
// ── The load-bearing invariant ──
// Player.q/r ALWAYS index G.map.  While a player is underground their q/r stay
// pinned to the hatch they descended through, and G.tunnel is indexed by
// tq/tr instead.  That is what lets weather, the world system (doom, fire,
// flood, caravan), vision, actions, the LCD minimap and broadcastState keep
// reading G.map[p.r][p.q] unchanged — they just skip players with depth != 0.
// Breaking this turns every one of those into an out-of-bounds read.
//
// Terrain ids (see TERRAIN_MC / TERRAIN_IMG_NAME in the .ino):
//   12 Bunker Entrance  — on BOTH boards: the surface hatch and its shaft cell
//   13 Vent Shaft       — likewise, but costs VENT_ASCEND_MP to climb out of
//   14 Tunnel Floor     — the walkable underground; waters and salvages
//                         (corridors and rooms -- which sides of a cell are
//                         open is G.tunnelOp, see "Open sides" below)
//   15 Collapsed Tunnel — solid rock and cave-ins; impassable, permanent

// bunkerHatches[] / hatchCount live in the .ino (ui-display.hpp needs them for
// the LCD minimap, and it is included long before this file).
//
// Minimum surface distance between two hatches.  Crossing the map is the whole
// point, so hatches that land near each other are worthless.  Relaxed on
// repeated failure rather than abandoned — see placeSurfaceHatches().
static constexpr int HATCH_MIN_DIST = 14;

// Bad air.  A bunker corridor is the one place on the map the weather cannot
// reach -- dawnUpkeep() switches exposure off outright at depth -- but the air
// down there is dead still, and a whole night breathing it costs a point of LL
// this often.  Sleeping underground is a trade, not a free hotel: a guaranteed
// 2 LL of exposure out in the open against a 30% chance of 1 down here.
// Only a *rest* rolls it; walking a corridor through costs nothing but MP.
static constexpr int TUNNEL_REST_LL_PCT = 30;

// ── The voice in the corridor ───────────────────────────────────────────────
// Bad air is the mechanical price of bedding down below; this is the other
// one. A survivor camped in a bare tunnel gets needled about it -- dry,
// unhelpful second-guessing of a decision that is, mechanically, usually
// fine. That gap is the joke: the tunnels are genuinely good shelter and the
// game will not stop implying you have made a mistake.
//
// Same shape as the Doom's taunts (tickDoomTaunts, world-system.hpp) and for
// the same reasons: a per-player cooldown so it cannot spam the toast stack,
// and only a line INDEX on the wire -- the 30 wordings live in TUNNEL_TAUNTS
// (data/game-data.js) and the client reduces the index modulo its own table,
// so lines can be added or reworded without reflashing. Nothing goes in the
// K10 chronicle: that book is for things that happened, and this is a mood.
//
// 8 world ticks matches DOOM_TAUNT_COOLDOWN -- ~2 real minutes, so roughly
// two or three lines per game-day spent below, and ~10 days underground
// before the table can repeat itself.
static constexpr uint8_t TUNNEL_TAUNT_COOLDOWN = 8;

// World ticks this player has spent camped in a bare corridor. Runtime only,
// deliberately not in Player or SaveHeader -- same call as the caravan's
// lastCaravanHex[] and the Doom's tauntCooldown.
//
// It counts UP rather than down so that zero means "not long enough yet".
// That makes silence the default at a cold boot, a restored save and a
// reconnect alike, with no init call to forget at any of those three sites --
// the down-counting version needed one and would have spoken on world tick
// one to anyone restored at depth 1.
static uint8_t tunnelTauntTicks[MAX_PLAYERS];

// ── Tunnel board bounds ─────────────────────────────────────────────────────
// The tunnel board does not wrap.  wrapQ/wrapR are hardcoded to MAP_COLS/
// MAP_ROWS, so every tunnel neighbour lookup goes through this instead, and an
// out-of-bounds neighbour is simply not a legal move.
static inline bool tunIn(int q, int r) {
  return q >= 0 && q < TUN_COLS && r >= 0 && r < TUN_ROWS;
}

// Non-wrapping axial hex distance, tunnel board only.  Same formula as
// hexDistWrap() without the nine-way wrap search.
static inline int tunDist(int q1, int r1, int q2, int r2) {
  int dq = q2 - q1, dr = r2 - r1;
  return (abs(dq) + abs(dq + dr) + abs(dr)) / 2;
}

static inline bool isHatchTerrain(uint8_t t)  { return t == 12 || t == 13; }
static inline bool isTunnelTerrain(uint8_t t) { return t >= 12 && t <= 15; }

// ── Open sides: the walls between cells ─────────────────────────────────────
// The board is corridors one hex wide with rooms hanging off them, and a
// corridor is not open to every floor cell it happens to touch -- two
// corridors can run side by side with rock between them. The art depends on
// it too: the overhead corridor pieces only join up when their openings are
// the real ones. So which sides of a cell are open is state of its own,
// G.tunnelOp[r][q], one byte a cell:
//   bits 0-5  side d is open (DQ/DR order). A floor-to-floor link is set on
//             both cells. A corridor that ends in a cave-in opens toward the
//             collapsed cell on its own side only: Collapsed Tunnel is MC 255,
//             so that opening is drawn and never walkable.
//   bit 6     TOP_ART_HI -- bit 4 of the cell's art index. The low nibble is
//             HexCell.variant, which is 4 bits on the wire; a room index can
//             pass 15.
//   bit 7     TOP_ROOM -- a room: a dead end drawn from the tunRoom sheet, not
//             a corridor piece picked by shape.
// A tunnel move needs the side open AND the terrain enterable (moveTunnel,
// computeValidMoves). The client gets the byte as "op" next to every tunnel
// cell it is sent (encodeTunnelOps / buildVisDisk in hex-map.hpp) and turns a
// corridor piece to fit the open sides.
static constexpr uint8_t TOP_OPEN   = 0x3F;
static constexpr uint8_t TOP_ART_HI = 0x40;
static constexpr uint8_t TOP_ROOM   = 0x80;

static inline bool tunOpen(int q, int r, int d) {
  return (G.tunnelOp[r][q] >> d) & 1;
}

// How much tunnel art the client has, from tiles.json "tunnelCounts"
// (setupVariantCounts() in game-server.hpp): rooms, entrance interiors, vent
// interiors, cave-ins. generateTunnels() deals art indices from these so a
// world shows as many different rooms as it has rooms. Zeros -- a card
// without the tunnel sheets -- deal index 0 everywhere, and the client has no
// tunnel art to draw either.
enum { TUN_ART_ROOM, TUN_ART_ENTRANCE, TUN_ART_VENT, TUN_ART_CAVE, TUN_ART_KINDS };
static uint8_t tunnelArtCount[TUN_ART_KINDS] = {};

// Which hatch sits on this surface hex, or -1.  Linear over at most 8 entries.
static int hatchAtSurface(int q, int r) {
  for (int i = 0; i < hatchCount; i++)
    if (bunkerHatches[i].sq == q && bunkerHatches[i].sr == r) return i;
  return -1;
}

// Which hatch shaft sits on this tunnel hex, or -1.
static int hatchAtShaft(int q, int r) {
  for (int i = 0; i < hatchCount; i++)
    if (bunkerHatches[i].tq == q && bunkerHatches[i].tr == r) return i;
  return -1;
}

// ── Forced surfacing ─────────────────────────────────────────────
// Put pid back on the surface at the hatch they descended through, and tell
// every client. Used for the downed sweep in tickGame(): a survivor who goes
// down in the tunnels has movesLeft zeroed and no legal action while the bad
// air keeps ticking, so they would die with no agency and no way out.
// Caller holds G.mutex.
static void surfacePlayer(int pid) {
  Player& p = G.players[pid];
  if (!p.depth) return;
  uint8_t h = (p.hatchIdx < hatchCount) ? p.hatchIdx : 0;
  if (hatchCount > 0) { p.q = bunkerHatches[h].sq; p.r = bunkerHatches[h].sr; }
  p.depth = 0;
  GameEvent ev = {}; ev.type = EVT_TUNNEL_EXIT; ev.pid = (uint8_t)pid;
  ev.q = p.q; ev.r = p.r; ev.amt = h; ev.moveMP = p.movesLeft;
  enqEvt(ev);
  Log.notice("surfacePlayer pid=%d -> hatch=%u (%d,%d)", pid, (unsigned)h, (int)p.q, (int)p.r);
}

// ── The voice in the corridor, per world tick ───────────────────────────────
// Called from tickGame()'s world-tick slice. Caller holds G.mutex.
//
// The condition is "underground on a hex with no shelter", which today is
// every underground hex: doShelter() is refused at depth 1, so a tunnel
// cell's shelter byte is always 0. The check is written out anyway rather
// than folded into `p.depth` -- if underground shelters ever land, the one
// thing that should silence this voice is having built something, and a
// reader should not have to know the action table to see that.
static void tickTunnelTaunts() {
  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    Player& p = G.players[pid];

    // Back on the surface, downed, or mid-encounter: rearm and stay quiet.
    // The encounter guard matters -- a scene has its own prose and the last
    // thing it needs is the corridor talking over it.
    if (!p.connected || !p.depth || p.ll == 0 || encounters[pid].active) {
      tunnelTauntTicks[pid] = 0;
      continue;
    }
    if (!tunIn(p.tq, p.tr)) continue;                 // mid-transition; say nothing
    if (G.tunnel[p.tr][p.tq].shelter > 0) {           // they built something down here
      tunnelTauntTicks[pid] = 0;
      continue;
    }
    if (++tunnelTauntTicks[pid] < TUNNEL_TAUNT_COOLDOWN) continue;
    tunnelTauntTicks[pid] = 0;

    GameEvent ev = {};
    ev.type    = EVT_TUNNEL_TAUNT;
    ev.pid     = (uint8_t)pid;
    ev.evWsId  = p.wsClientId;                        // unicast: their doubt, not the party's
    ev.res     = (uint8_t)(esp_random() & 0xFF);      // client reduces mod its table length
    enqEvt(ev);
  }
}

// ── Crossing between the boards ─────────────────────────────────────────────
// There is no DESCEND/ASCEND control: stepping onto a Bunker Entrance (12) or
// Vent Shaft (13) IS the transition.  movePlayer() and moveTunnel() call these
// as the last thing a step does, which is what keeps it from looping -- you
// arrive on the paired cell of the OTHER board by transition, not by a move,
// so nothing re-triggers until you deliberately step onto a hatch again.
//
// Both charge their cost clamped at 0 rather than refusing when it cannot be
// afforded.  The step has already landed by the time we get here, so a refusal
// would strand the player on the hatch with no feedback; and on the way up,
// being unable to climb out means dying in the bad air with no agency (see
// surfacePlayer() above).  You can always cross -- you may just arrive spent.

// Climbing out of a Vent Shaft (13) costs double.  Shared with the reconnect
// path, so it stays a named function rather than an inline max().
static void tunnelAscendCost(Player& p, uint8_t shaftTerrain) {
  int cost = (shaftTerrain == 13) ? (int)VENT_ASCEND_MP : 1;
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - cost);
}

// Just stepped onto a surface hex -- is it a hatch?  Caller holds G.mutex.
// Returns true when the player is now underground, which is the caller's cue
// to hand the client the tunnel board (sendTunnelSync).
static bool tunnelStepDown(int pid) {
  Player& p = G.players[pid];
  if (p.depth) return false;
  if (!isHatchTerrain(G.map[p.r][p.q].terrain)) return false;
  int h = hatchAtSurface(p.q, p.r);
  if (h < 0) return false;                      // terrain says hatch, table disagrees
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - 1);
  p.depth     = 1;
  p.hatchIdx  = (uint8_t)h;
  p.tq        = (int16_t)bunkerHatches[h].tq;
  p.tr        = (int16_t)bunkerHatches[h].tr;
  G.tunnel[p.tr][p.tq].footprints |= (1 << pid);
  // A full cooldown of quiet before the corridor starts second-guessing the
  // decision -- arriving and being needled in the same breath reads as a
  // refusal to descend rather than a mood.
  tunnelTauntTicks[pid] = 0;
  GameEvent ev = {}; ev.type = EVT_TUNNEL_ENTER; ev.pid = (uint8_t)pid;
  ev.q = p.q; ev.r = p.r; ev.amt = (uint8_t)h; ev.moveMP = p.movesLeft;
  enqEvt(ev);
  Log.notice("tun step-down pid=%d hatch=%d (%d,%d)->(%d,%d) mp=%d",
             pid, h, (int)p.q, (int)p.r, (int)p.tq, (int)p.tr, (int)p.movesLeft);
  return true;
}

// Just stepped onto a tunnel hex -- is it a shaft?  Caller holds G.mutex.
// q/r already point at the paired surface hatch for whichever shaft this is,
// which is why you can surface somewhere other than where you went down.
static bool tunnelStepUp(int pid) {
  Player& p = G.players[pid];
  if (!p.depth) return false;
  int h = hatchAtShaft(p.tq, p.tr);
  if (h < 0) return false;
  tunnelAscendCost(p, G.tunnel[p.tr][p.tq].terrain);
  p.depth    = 0;
  p.hatchIdx = (uint8_t)h;
  p.q        = bunkerHatches[h].sq;
  p.r        = bunkerHatches[h].sr;
  G.map[p.r][p.q].footprints |= (1 << pid);
  GameEvent ev = {}; ev.type = EVT_TUNNEL_EXIT; ev.pid = (uint8_t)pid;
  ev.q = p.q; ev.r = p.r; ev.amt = (uint8_t)h; ev.moveMP = p.movesLeft;
  enqEvt(ev);
  Log.notice("tun step-up pid=%d hatch=%d -> (%d,%d) mp=%d",
             pid, h, (int)p.q, (int)p.r, (int)p.movesLeft);
  // Climbing out is entering a surface hex: a bloomed daisy patch on the
  // hatch bites (ecology.hpp). After the exit event, so the client is back
  // on the surface board before the damage arrives.
  ecoBiteCheck(pid);
  return true;
}

// ── Surface hatch placement ─────────────────────────────────────────────────
// Modelled on the MIN_SETTLE pass in hex-map.hpp: rejection-sample Open Scrub
// hexes, refusing anything adjacent to Mountain/Settlement/Crater/River so a
// hatch is never walled in by impassable terrain.  Additionally enforces a
// minimum spacing, and because that spacing IS the feature, it relaxes rather
// than abandoning: successive rounds at smaller distances before settling for
// however many were placed.
static void placeSurfaceHatches() {
  hatchCount = 0;
  memset(bunkerHatches, 0, sizeof(bunkerHatches));

  for (int minDist = HATCH_MIN_DIST; minDist >= 4 && hatchCount < MAX_HATCHES; minDist -= 5) {
    for (uint16_t attempt = 0; hatchCount < MAX_HATCHES && attempt < 2500; attempt++) {
      int r = (int)(esp_random() % MAP_ROWS);
      int c = (int)(esp_random() % MAP_COLS);
      if (G.map[r][c].terrain != 0) continue;           // Open Scrub only
      bool ok = true;
      for (int d = 0; d < 6 && ok; d++) {
        uint8_t nt = G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain;
        if (nt == 8 || nt == 9 || nt == 10 || nt == 11) ok = false;
      }
      if (!ok) continue;
      for (int i = 0; i < hatchCount && ok; i++)
        if (hexDistWrap(c, r, bunkerHatches[i].sq, bunkerHatches[i].sr) < minDist) ok = false;
      if (!ok) continue;
      bunkerHatches[hatchCount].sq = (int16_t)c;
      bunkerHatches[hatchCount].sr = (int16_t)r;
      hatchCount++;
    }
  }

  // Sort left-to-right by column so the shafts — also laid out left-to-right —
  // pair up geographically.  Without this the network is unlearnable: a player
  // has to be able to build a mental model of which shaft surfaces where.
  for (int i = 1; i < hatchCount; i++) {
    BunkerHatch key = bunkerHatches[i];
    int j = i - 1;
    while (j >= 0 && bunkerHatches[j].sq > key.sq) { bunkerHatches[j + 1] = bunkerHatches[j]; j--; }
    bunkerHatches[j + 1] = key;
  }
}

// ── Corridor shapes ─────────────────────────────────────────────────────────
// The set of sides a corridor cell has open decides which art piece draws it.
// The client turns and mirrors a piece onto the cell, so what matters is the
// shape up to rotation and reflection: canonical form is the smallest mask
// over the six rotations of the mask and of its mirror image. The corridor
// art (tunCorridor.json) has pieces for exactly these, and the generator
// steers toward them.
static uint8_t tunRotate(uint8_t m, int k) {
  uint8_t out = 0;
  for (int d = 0; d < 6; d++)
    if ((m >> d) & 1) out |= (uint8_t)(1 << ((d + k) % 6));
  return out;
}

// Reflection across the screen's vertical axis: direction d -> 4 - d.
static uint8_t tunMirror(uint8_t m) {
  uint8_t out = 0;
  for (int d = 0; d < 6; d++)
    if ((m >> d) & 1) out |= (uint8_t)(1 << ((10 - d) % 6));
  return out;
}

static uint8_t tunCanon(uint8_t m) {
  m &= TOP_OPEN;
  uint8_t best = 0xFF;
  for (int mir = 0; mir < 2; mir++) {
    uint8_t mm = mir ? tunMirror(m) : m;
    for (int k = 0; k < 6; k++) best = min(best, tunRotate(mm, k));
  }
  return best;
}

static constexpr uint8_t TUN_SHAPE_DEAD     = 0b000001;
static constexpr uint8_t TUN_SHAPE_SHARP    = 0b000011;   // 60 degree bend
static constexpr uint8_t TUN_SHAPE_GENTLE   = 0b000101;   // 120 degree bend
static constexpr uint8_t TUN_SHAPE_STRAIGHT = 0b001001;
static constexpr uint8_t TUN_SHAPE_TEE      = 0b001011;
static constexpr uint8_t TUN_SHAPE_WYE      = 0b010101;
static constexpr uint8_t TUN_SHAPE_CROSS    = 0b011011;   // two straights, crossing at 60

static bool tunShapeDrawn(uint8_t m) {
  switch (tunCanon(m)) {
    case TUN_SHAPE_DEAD: case TUN_SHAPE_SHARP: case TUN_SHAPE_GENTLE:
    case TUN_SHAPE_STRAIGHT: case TUN_SHAPE_TEE: case TUN_SHAPE_WYE:
    case TUN_SHAPE_CROSS:
      return true;
  }
  return false;
}

// What leaving a cell with open sides m costs the carving walk. A shape the
// art has no piece for is a last resort -- the client draws it as a chamber,
// which reads, but not as corridor. A T is fine but there are only two T
// pieces against six straights and six crossings, so the walk leans away
// from them. Everything else is free.
static int tunShapeCost(uint8_t m) {
  if (!tunShapeDrawn(m)) return 40;
  return tunCanon(m) == TUN_SHAPE_TEE ? 20 : 0;
}

static void tunLink(int q, int r, int d) {
  G.tunnelOp[r][q] |= (uint8_t)(1 << d);
  G.tunnelOp[r + DR[d]][q + DQ[d]] |= (uint8_t)(1 << ((d + 3) % 6));
}

// ── Corridor carving ────────────────────────────────────────────────────────
// Walk from (q1,r1) to (q2,r2), carving Tunnel Floor and joining each step to
// the last. Every step is scored and the cheapest taken:
//   - progress toward the target, 30 a hex;
//   - no turn sharper than 60 degrees (a hairpin would open two neighbouring
//     sides of one cell, the bend the art draws worst);
//   - what the step does to the shape of the cell it leaves and the cell it
//     joins (tunShapeCost);
//   - reusing a link that already exists is free, so walks share corridors;
//   - 25 for every earlier visit this walk made to the cell -- without it a
//     walk whose way on is all costly shapes ping-pongs along corridor it
//     already dug, for free, until the step budget runs out (1.1% of worlds
//     had a shaft cut off that way, measured over 2000 mock boards);
//   - a little noise, so corridors wander instead of running dead straight.
// shapely=false drops the shape and turn terms: the connectivity pass's
// last-resort reroute, which only has to get there.
// Bounded: a stuck walk gives up, and the connectivity pass reroutes.
//
// Shaft cells are walls to this walk.  Stepping onto one climbs out
// (tunnelStepUp), so a corridor that ran through a shaft would eject anyone
// merely passing by -- see the junction pass in generateTunnels().
static void carveCorridor(int q1, int r1, int q2, int r2, bool shapely = true) {
  int q = q1, r = r1, head = -1;
  uint8_t visits[TUN_ROWS][TUN_COLS] = {};
  for (int step = 0; step < TUN_COLS * TUN_ROWS * 3; step++) {
    if (visits[r][q] < 255) visits[r][q]++;
    if (q == q2 && r == r2) return;
    int bestD = -1, bestScore = 0x7FFFFFFF;
    for (int d = 0; d < 6; d++) {
      int nq = q + DQ[d], nr = r + DR[d];
      if (!tunIn(nq, nr)) continue;
      if (hatchAtShaft(nq, nr) >= 0) continue;     // never route through a shaft
      int score = 25 * (int)visits[nr][nq];
      if (shapely && tunOpen(q, r, d)) {
        score -= 5;
      } else if (shapely) {
        if (head >= 0 && d != head && d != (head + 1) % 6 && d != (head + 5) % 6) score += 60;
        score += tunShapeCost(G.tunnelOp[r][q] | (uint8_t)(1 << d));
        if (G.tunnel[nr][nq].terrain == 14)
          score += tunShapeCost(G.tunnelOp[nr][nq] | (uint8_t)(1 << ((d + 3) % 6)));
      }
      score -= 30 * (tunDist(q, r, q2, r2) - tunDist(nq, nr, q2, r2));
      score += (int)(esp_random() % 22);
      if (score < bestScore) { bestScore = score; bestD = d; }
    }
    if (bestD < 0) return;                         // boxed in; should not happen
    int nq = q + DQ[bestD], nr = r + DR[bestD];
    if (G.tunnel[nr][nq].terrain == 15) G.tunnel[nr][nq].terrain = 14;
    tunLink(q, r, bestD);
    q = nq; r = nr; head = bestD;
  }
}

// ── Connectivity ────────────────────────────────────────────────────────────
// Flood fill from (q,r) through open sides, marking `seen`.  160 cells, so
// cheap.
//
// blockShafts treats every shaft as solid as well.  That is the "can you get
// there without being thrown out of the tunnels on the way" question, which is
// what generateTunnels() checks its junctions against.
// Keyed on bunkerHatches[] rather than the terrain id: generateTunnels() runs
// this before the 12/13 stamping pass, when a shaft cell is still plain floor.
static bool tunBlocked(int q, int r, bool blockShafts) {
  if (G.tunnel[r][q].terrain == 15) return true;
  return blockShafts && hatchAtShaft(q, r) >= 0;
}

static void tunnelFloodFill(int q, int r, bool seen[TUN_ROWS][TUN_COLS],
                            bool blockShafts = false) {
  if (!tunIn(q, r) || seen[r][q] || tunBlocked(q, r, blockShafts)) return;
  // Explicit stack: recursion over 160 cells would be fine on the main task but
  // this also runs from the world tick, which has a smaller stack budget.
  struct { int8_t q, r; } stack[TUN_ROWS * TUN_COLS];
  int sp = 0;
  stack[sp++] = { (int8_t)q, (int8_t)r };
  seen[r][q] = true;
  while (sp > 0) {
    int cq = stack[--sp].q, cr = stack[sp].r;
    for (int d = 0; d < 6; d++) {
      if (!tunOpen(cq, cr, d)) continue;           // a wall, whatever is behind it
      int nq = cq + DQ[d], nr = cr + DR[d];
      if (!tunIn(nq, nr) || seen[nr][nq] || tunBlocked(nq, nr, blockShafts)) continue;
      seen[nr][nq] = true;
      stack[sp++] = { (int8_t)nq, (int8_t)nr };
    }
  }
}

// ── Rooms and cave-ins ──────────────────────────────────────────────────────
// Both hang off a corridor cell into the rock ABOVE it -- N, NE or NW. The
// room art is drawn at an angle with its back walls at the top, so a room
// entered from below is entered through its open front; the same goes for
// the cave-in art, which shows the tunnel carrying on into rubble.
static constexpr int TUN_UP[3] = { 2, 1, 3 };      // N, NE, NW
static constexpr int TUNNEL_ROOMS    = 12;
static constexpr int TUNNEL_CAVE_INS = 3;

// A corridor cell a room or cave-in may hang off: carved floor that is not
// itself a room. Shaft junctions qualify -- they are ordinary corridor.
static bool tunCorridorCell(int q, int r) {
  return G.tunnel[r][q].terrain == 14 && !(G.tunnelOp[r][q] & TOP_ROOM);
}

// One room at a time, each at the best spot left. Where a room goes changes
// the shape of the corridor cell it hangs off, and the art has six crossings
// and one Y against two Ts -- so a room that turns a T into a crossing, or a
// bend into a Y, beats one that turns a straight into a T.
static int placeRooms(int want, uint8_t roomQ[], uint8_t roomR[]) {
  int placed = 0;
  while (placed < want) {
    int bestScore = 0x7FFFFFFF, bq = -1, br = -1, bd = -1;
    for (int r = 0; r < TUN_ROWS; r++)
      for (int q = 0; q < TUN_COLS; q++) {
        if (!tunCorridorCell(q, r)) continue;
        for (int k = 0; k < 3; k++) {
          int d = TUN_UP[k], nq = q + DQ[d], nr = r + DR[d];
          if (!tunIn(nq, nr) || G.tunnel[nr][nq].terrain != 15) continue;
          uint8_t c = tunCanon(G.tunnelOp[r][q] | (uint8_t)(1 << d));
          int pref = (c == TUN_SHAPE_CROSS) ? 0 : (c == TUN_SHAPE_WYE) ? 10 : (c == TUN_SHAPE_TEE) ? 30 : -1;
          if (pref < 0) continue;
          int score = pref + (int)(esp_random() % 25);
          if (score < bestScore) { bestScore = score; bq = q; br = r; bd = d; }
        }
      }
    if (bd < 0) break;                             // nowhere left that draws
    int nq = bq + DQ[bd], nr = br + DR[bd];
    G.tunnel[nr][nq].terrain = 14;
    tunLink(bq, br, bd);
    G.tunnelOp[nr][nq] |= TOP_ROOM;
    roomQ[placed] = (uint8_t)nq; roomR[placed] = (uint8_t)nr;
    placed++;
  }
  return placed;
}

// A corridor that runs on into a cave-in: an opening on the corridor cell's
// side only, toward a collapsed cell drawn as rubble (variant 1+ on terrain
// 15; 0 is plain rock). Nothing about the collapse is walkable.
static int placeCaveIns(int want) {
  uint8_t cells[TUN_ROWS * TUN_COLS][2];
  int n = 0;
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++)
      if (tunCorridorCell(q, r)) { cells[n][0] = (uint8_t)q; cells[n][1] = (uint8_t)r; n++; }
  for (int i = n - 1; i > 0; i--) {                // Fisher-Yates
    int j = (int)(esp_random() % (uint32_t)(i + 1));
    uint8_t t0 = cells[i][0], t1 = cells[i][1];
    cells[i][0] = cells[j][0]; cells[i][1] = cells[j][1];
    cells[j][0] = t0;          cells[j][1] = t1;
  }
  const int kinds = max(1, (int)tunnelArtCount[TUN_ART_CAVE]);
  int placed = 0;
  for (int i = 0; i < n && placed < want; i++) {
    int q = cells[i][0], r = cells[i][1];
    for (int k = 0; k < 3; k++) {
      int d = TUN_UP[k], nq = q + DQ[d], nr = r + DR[d];
      if (!tunIn(nq, nr) || G.tunnel[nr][nq].terrain != 15 || G.tunnel[nr][nq].variant) continue;
      if (!tunShapeDrawn(G.tunnelOp[r][q] | (uint8_t)(1 << d))) continue;
      G.tunnelOp[r][q] |= (uint8_t)(1 << d);
      G.tunnel[nr][nq].variant = (uint8_t)(1 + placed % kinds);
      placed++;
      break;
    }
  }
  return placed;
}

// ── Tunnel board generation ─────────────────────────────────────────────────
// Called from the end of generateMap() (forward-declared in hex-map.hpp), so
// the surface map is already final and hatches can be placed against it.
static void generateTunnels() {
  // Surface first: however many hatches we manage to place is how many shafts
  // the tunnel board needs.  The other order can orphan a shaft with no exit.
  placeSurfaceHatches();

  // Solid rock everywhere, every side shut, then carve.
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++) {
      G.tunnel[r][q] = HexCell{ 15, 0, 0, 0, 0, 0, 0, 0, 0 };
      G.tunnelOp[r][q] = 0;
    }

  if (hatchCount == 0) {
    Log.warning("generateTunnels: no surface hatch could be placed — tunnels sealed");
    return;
  }

  // Shafts spread left-to-right, one per column band, so shaft i lines up with
  // surface hatch i (both sorted by column).  Two shafts never touch: each is
  // a dead end drawn as a room of its own, and side by side they would read as
  // one.
  for (int i = 0; i < hatchCount; i++) {
    int band0 = (i * TUN_COLS) / hatchCount;
    int band1 = ((i + 1) * TUN_COLS) / hatchCount;
    if (band1 <= band0) band1 = band0 + 1;
    int tq = 0, tr = 0;
    for (int attempt = 0; attempt < 50; attempt++) {
      tq = band0 + (int)(esp_random() % (uint32_t)(band1 - band0));
      tr = 1 + (int)(esp_random() % (uint32_t)(TUN_ROWS - 2));   // off the top/bottom edge
      if (tq >= TUN_COLS) tq = TUN_COLS - 1;
      bool apart = true;
      for (int j = 0; j < i && apart; j++)
        if (tunDist(tq, tr, bunkerHatches[j].tq, bunkerHatches[j].tr) < 2) apart = false;
      if (apart) break;
    }
    bunkerHatches[i].tq = (uint8_t)tq;
    bunkerHatches[i].tr = (uint8_t)tr;
    G.tunnel[tr][tq].terrain = 14;   // carved now, stamped 12/13 below
  }

  // ── Junctions ──
  // A shaft must not be load-bearing.  Stepping onto one climbs straight out
  // (tunnelStepUp), so a corridor chained shaft-to-shaft ejects anyone walking
  // from shaft i-1 to shaft i+1 — the network collapses into "hop to the next
  // shaft and get spat out".  Each shaft instead gets a junction: one adjacent
  // floor cell that IS on the chain, leaving the shaft hanging off the network
  // as a one-hex spur you only step onto when you mean to leave.  The junction
  // goes below the shaft where it can (S, SE, SW), for the same reason rooms
  // hang above their corridor: the shaft art is a room seen from the front.
  static constexpr int BELOW[3] = { 5, 0, 4 };
  uint8_t junQ[MAX_HATCHES], junR[MAX_HATCHES];
  for (int i = 0; i < hatchCount; i++) {
    const int hq = bunkerHatches[i].tq, hr = bunkerHatches[i].tr;
    int pick[6], n = 0;
    for (int k = 0; k < 3; k++) {
      int nq = hq + DQ[BELOW[k]], nr = hr + DR[BELOW[k]];
      if (tunIn(nq, nr) && hatchAtShaft(nq, nr) < 0) pick[n++] = BELOW[k];
    }
    for (int d = 0; d < 6 && n == 0; d++) {       // nothing below: any side will do
      int nq = hq + DQ[d], nr = hr + DR[d];
      if (tunIn(nq, nr) && hatchAtShaft(nq, nr) < 0) pick[n++] = d;
    }
    if (n == 0) {                       // walled in by the board edge and peers
      junQ[i] = (uint8_t)hq; junR[i] = (uint8_t)hr;
      continue;
    }
    int d = pick[esp_random() % (uint32_t)n];
    junQ[i] = (uint8_t)(hq + DQ[d]);
    junR[i] = (uint8_t)(hr + DR[d]);
    G.tunnel[junR[i]][junQ[i]].terrain = 14;      // the spur off the shaft
    tunLink(hq, hr, d);
  }

  // Chain every junction to the next, then add a couple of long loops.  The
  // loops are what make a dead end survivable rather than fatal — with no
  // alternate route, the network is one long corridor.
  for (int i = 0; i + 1 < hatchCount; i++)
    carveCorridor(junQ[i], junR[i], junQ[i + 1], junR[i + 1]);
  if (hatchCount >= 4) {
    carveCorridor(junQ[0], junR[0], junQ[hatchCount / 2],   junR[hatchCount / 2]);
    carveCorridor(junQ[1], junR[1], junQ[hatchCount - 1],   junR[hatchCount - 1]);
  }

  // Prove it rather than assume it.  carveCorridor() will not route through a
  // shaft, and it walks through open sides only, but a walk can still give up
  // short of its target.  Flood fill with every shaft solid: any junction that
  // does not come back is cut off, so carve it a second way in and check again.
  bool shaftFree = false;
  for (int pass = 0; pass <= MAX_HATCHES && !shaftFree; pass++) {
    bool seen[TUN_ROWS][TUN_COLS] = {};
    tunnelFloodFill(junQ[0], junR[0], seen, true);
    int orphan = -1;
    for (int i = 1; i < hatchCount && orphan < 0; i++)
      if (!seen[junR[i]][junQ[i]]) orphan = i;
    if (orphan < 0) { shaftFree = true; break; }
    if (pass == MAX_HATCHES) break;               // last pass was verify-only
    Log.notice("generateTunnels: junction %d cut off — rerouting", orphan);
    // The first reroute keeps to the art's shapes; after that, just get there.
    carveCorridor(junQ[0], junR[0], junQ[orphan], junR[orphan], pass == 0);
  }
  if (!shaftFree)
    Log.warning("generateTunnels: could not connect every junction without crossing a shaft");

  // Stamp the paired terrain on BOTH boards.  Alternating 12/13 gives the two
  // entrance types even representation; a surface hex and its shaft always
  // share an id so the art reads as the same landmark from either side.
  for (int i = 0; i < hatchCount; i++) {
    uint8_t t = (i & 1) ? 13 : 12;
    HexCell& s = G.map[bunkerHatches[i].sr][bunkerHatches[i].sq];
    s.terrain = t; s.resource = 0; s.amount = 0; s.poi = 0;
    G.tunnel[bunkerHatches[i].tr][bunkerHatches[i].tq].terrain = t;
  }

  // Rooms, then cave-ins -- after the stamping, so a shaft is 12/13 and never
  // mistaken for rock a room could be dug into.
  uint8_t roomQ[TUNNEL_ROOMS], roomR[TUNNEL_ROOMS];
  const int rooms = placeRooms(TUNNEL_ROOMS, roomQ, roomR);
  const int caves = placeCaveIns(TUNNEL_CAVE_INS);

  // Resources: Water from cistern seeps, Scrap from bunker fittings.  Same 19%
  // rate and 1-3 pile as the surface Phase 3 pass — that loop only walks
  // G.map, so the tunnel board needs its own.  Rooms are floor too.
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++) {
      HexCell& cell = G.tunnel[r][q];
      if (cell.terrain != 14) continue;
      uint32_t rnd = esp_random();
      if (((rnd >> 8) & 0xFF) % 100 < 19) {
        cell.resource = terrainSpawnRes(14, (rnd >> 16) & 0xFF);
        if (cell.resource > 0) cell.amount = 1 + ((rnd >> 24) & 0xFF) % 3;
      }
    }

  // Encounter POIs, same shuffle-and-deal as the surface Phase 5: every file in
  // the tunnel pool is placed at least once.  encPools[14].count is 0 until
  // data/encounters/index.json defines a "14" entry, in which case this is a
  // no-op — that content lands in a later step.
  if (encPools[14].count > 0) {
    uint8_t cells[TUN_ROWS * TUN_COLS][2];
    int n = 0;
    for (int r = 0; r < TUN_ROWS; r++)
      for (int q = 0; q < TUN_COLS; q++)
        if (G.tunnel[r][q].terrain == 14) { cells[n][0] = (uint8_t)q; cells[n][1] = (uint8_t)r; n++; }
    for (int i = n - 1; i > 0; i--) {                    // Fisher-Yates
      int j = (int)(esp_random() % (uint32_t)(i + 1));
      uint8_t t0 = cells[i][0], t1 = cells[i][1];
      cells[i][0] = cells[j][0]; cells[i][1] = cells[j][1];
      cells[j][0] = t0;          cells[j][1] = t1;
    }
    int place = min(n, (int)encPools[14].count);
    for (int i = 0; i < place; i++)
      G.tunnel[cells[i][1]][cells[i][0]].poi = (uint8_t)(i + 1);
  }

  // ── Art ──
  // Rooms are dealt from a shuffled deck of every room the art has, so a
  // world shows as many different rooms as it has rooms and never repeats one
  // until the deck runs out. Shafts go round their own interiors from a
  // random start. Corridor cells get a free 4-bit variant: the client picks
  // among the pieces that fit their shape with it. Collapsed cells keep 0
  // (plain rock) unless placeCaveIns() made them rubble.
  {
    const int nRoom = (int)tunnelArtCount[TUN_ART_ROOM];
    uint8_t deck[32];
    const int deckN = min(nRoom, 32);
    for (int i = 0; i < deckN; i++) deck[i] = (uint8_t)i;
    for (int i = deckN - 1; i > 0; i--) {
      int j = (int)(esp_random() % (uint32_t)(i + 1));
      uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int k = 0; k < rooms; k++) {
      uint8_t idx = deckN ? deck[k % deckN] : 0;
      G.tunnel[roomR[k]][roomQ[k]].variant = idx & 0x0F;
      if (idx & 0x10) G.tunnelOp[roomR[k]][roomQ[k]] |= TOP_ART_HI;
    }
    for (int r = 0; r < TUN_ROWS; r++)
      for (int q = 0; q < TUN_COLS; q++)
        if (tunCorridorCell(q, r)) G.tunnel[r][q].variant = (uint8_t)(esp_random() & 0x0F);
    const int nEnt = max(1, (int)tunnelArtCount[TUN_ART_ENTRANCE]);
    const int nVent = max(1, (int)tunnelArtCount[TUN_ART_VENT]);
    int nextEnt = (int)(esp_random() % (uint32_t)nEnt), nextVent = (int)(esp_random() % (uint32_t)nVent);
    for (int i = 0; i < hatchCount; i++) {
      HexCell& c = G.tunnel[bunkerHatches[i].tr][bunkerHatches[i].tq];
      c.variant = (uint8_t)((c.terrain == 13 ? nextVent++ % nVent : nextEnt++ % nEnt) & 0x0F);
    }
  }

  int floorCells = 0, artless = 0;
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++) {
      if (G.tunnel[r][q].terrain != 14) continue;
      floorCells++;
      if (tunCorridorCell(q, r) && !tunShapeDrawn(G.tunnelOp[r][q])) artless++;
    }
  Log.notice("Tunnels ready %dx%d hatches=%d floor=%d rooms=%d caves=%d artless=%d shaftFreePaths=%s",
             (int)TUN_COLS, (int)TUN_ROWS, (int)hatchCount, floorCells, rooms, caves, artless,
             shaftFree ? "ok" : "FAILED");
}
