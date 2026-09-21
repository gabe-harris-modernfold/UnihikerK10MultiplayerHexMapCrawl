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

// ── Corridor carving ────────────────────────────────────────────────────────
// Greedy walk from (q1,r1) to (q2,r2), carving Tunnel Floor as it goes.  Takes
// whichever of the six directions shrinks the distance most, but a random legal
// step ~25% of the time so corridors bend instead of running dead straight.
// Bounded: a stuck walk gives up rather than spinning.
//
// Shaft cells are walls to this walk.  Stepping onto one climbs out
// (tunnelStepUp), so a corridor that ran through a shaft would eject anyone
// merely passing by -- see the junction pass in generateTunnels().
static void carveCorridor(int q1, int r1, int q2, int r2) {
  int q = q1, r = r1;
  for (int step = 0; step < TUN_COLS * TUN_ROWS * 2; step++) {
    if (q == q2 && r == r2) return;
    int bestD = -1, bestDist = 0x7FFFFFFF;
    int legal[6], legalN = 0;
    for (int d = 0; d < 6; d++) {
      int nq = q + DQ[d], nr = r + DR[d];
      if (!tunIn(nq, nr)) continue;
      if (hatchAtShaft(nq, nr) >= 0) continue;     // never route through a shaft
      legal[legalN++] = d;
      int dist = tunDist(nq, nr, q2, r2);
      if (dist < bestDist) { bestDist = dist; bestD = d; }
    }
    if (legalN == 0) return;                       // boxed in; should not happen
    int d = ((esp_random() % 100) < 25) ? legal[esp_random() % legalN] : bestD;
    if (d < 0) d = legal[esp_random() % legalN];
    q += DQ[d]; r += DR[d];
    if (G.tunnel[r][q].terrain == 15) G.tunnel[r][q].terrain = 14;
  }
}

// ── Connectivity ────────────────────────────────────────────────────────────
// Flood fill from (q,r) over passable tunnel terrain, marking `seen`.  Used by
// the cave-in guard: a collapse that would cut a shaft (or a player) off from
// the rest of the network is refused outright.  160 cells, so cheap.
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
      int nq = cq + DQ[d], nr = cr + DR[d];
      if (!tunIn(nq, nr) || seen[nr][nq] || tunBlocked(nq, nr, blockShafts)) continue;
      seen[nr][nq] = true;
      stack[sp++] = { (int8_t)nq, (int8_t)nr };
    }
  }
}

// ── Tunnel board generation ─────────────────────────────────────────────────
// Called from the end of generateMap() (forward-declared in hex-map.hpp), so
// the surface map is already final and hatches can be placed against it.
static void generateTunnels() {
  // Surface first: however many hatches we manage to place is how many shafts
  // the tunnel board needs.  The other order can orphan a shaft with no exit.
  placeSurfaceHatches();

  // Solid rock everywhere, then carve.
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++)
      G.tunnel[r][q] = HexCell{ 15, 0, 0, 0, 0, 0, 0, 0, 0 };

  if (hatchCount == 0) {
    Log.warning("generateTunnels: no surface hatch could be placed — tunnels sealed");
    return;
  }

  // Shafts spread left-to-right, one per column band, so shaft i lines up with
  // surface hatch i (both sorted by column).
  for (int i = 0; i < hatchCount; i++) {
    int band0 = (i * TUN_COLS) / hatchCount;
    int band1 = ((i + 1) * TUN_COLS) / hatchCount;
    if (band1 <= band0) band1 = band0 + 1;
    int tq = band0 + (int)(esp_random() % (uint32_t)(band1 - band0));
    int tr = 1 + (int)(esp_random() % (uint32_t)(TUN_ROWS - 2));   // off the top/bottom edge
    if (tq >= TUN_COLS) tq = TUN_COLS - 1;
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
  // as a one-hex spur you only step onto when you mean to leave.
  uint8_t junQ[MAX_HATCHES], junR[MAX_HATCHES];
  for (int i = 0; i < hatchCount; i++) {
    int pick[6], n = 0;
    for (int d = 0; d < 6; d++) {
      int nq = bunkerHatches[i].tq + DQ[d], nr = bunkerHatches[i].tr + DR[d];
      if (!tunIn(nq, nr) || hatchAtShaft(nq, nr) >= 0) continue;
      pick[n++] = d;
    }
    if (n == 0) {                       // walled in by the board edge and peers
      junQ[i] = (uint8_t)bunkerHatches[i].tq; junR[i] = (uint8_t)bunkerHatches[i].tr;
      continue;
    }
    int d = pick[esp_random() % (uint32_t)n];
    junQ[i] = (uint8_t)(bunkerHatches[i].tq + DQ[d]);
    junR[i] = (uint8_t)(bunkerHatches[i].tr + DR[d]);
    G.tunnel[junR[i]][junQ[i]].terrain = 14;      // the spur off the shaft
  }

  // Chain every junction to the next, then add a couple of long loops.  The
  // loops are what make "a cave-in means backtrack" survivable rather than
  // fatal — with no alternate route, one collapse cuts the network in half.
  for (int i = 0; i + 1 < hatchCount; i++)
    carveCorridor(junQ[i], junR[i], junQ[i + 1], junR[i + 1]);
  if (hatchCount >= 4) {
    carveCorridor(junQ[0], junR[0], junQ[hatchCount / 2],   junR[hatchCount / 2]);
    carveCorridor(junQ[1], junR[1], junQ[hatchCount - 1],   junR[hatchCount - 1]);
  }

  // Prove it rather than assume it.  carveCorridor() will not route through a
  // shaft, but a corridor can still run alongside one and leave a junction
  // reachable only by cutting the corner across it.  Flood fill with every
  // shaft solid: any junction that does not come back was connected *through*
  // a shaft, so carve it a second way in and check again.
  bool shaftFree = false;
  for (int pass = 0; pass <= MAX_HATCHES && !shaftFree; pass++) {
    bool seen[TUN_ROWS][TUN_COLS] = {};
    tunnelFloodFill(junQ[0], junR[0], seen, true);
    int orphan = -1;
    for (int i = 1; i < hatchCount && orphan < 0; i++)
      if (!seen[junR[i]][junQ[i]]) orphan = i;
    if (orphan < 0) { shaftFree = true; break; }
    if (pass == MAX_HATCHES) break;               // last pass was verify-only
    Log.notice("generateTunnels: junction %d only reachable through a shaft — rerouting", orphan);
    carveCorridor(junQ[0], junR[0], junQ[orphan], junR[orphan]);
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

  // Resources: Water from cistern seeps, Scrap from bunker fittings.  Same 19%
  // rate and 1-3 pile as the surface Phase 3 pass — that loop only walks
  // G.map, so the tunnel board needs its own.
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

  // Image variant per cell, same rank-quadratic pick as the surface.
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++) {
      uint8_t n = terrainVariantCount[G.tunnel[r][q].terrain];
      G.tunnel[r][q].variant = (n > 1) ? pickVariant(n, esp_random()) : 0;
    }

  int floorCells = 0;
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++)
      if (G.tunnel[r][q].terrain == 14) floorCells++;
  Log.notice("Tunnels ready %dx%d hatches=%d floor=%d shaftFreePaths=%s",
             (int)TUN_COLS, (int)TUN_ROWS, (int)hatchCount, floorCells,
             shaftFree ? "ok" : "FAILED");
}
