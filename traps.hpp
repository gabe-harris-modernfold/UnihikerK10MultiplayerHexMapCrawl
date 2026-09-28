#pragma once
// ── traps.hpp — booby traps ─────────────────────────────────────────────────
// Design: docs/trap-system-spec.md. Included from Esp32HexMapCrawl.ino right
// after encounter_engine.hpp, whose file loader and start-node reader a trap
// scene reuses unchanged -- a trap IS an encounter, from the traps pool
// (ENC_POOL_TRAP), with three things the engine could not do on its own:
//
//  1. You did not choose to be here. Every other scene opens on enc_start; a
//     trap opens from inside movePlayer()/moveTunnel() (trapOnArrival), and
//     handleMsg_move() pushes the enc_path the client never asked for.
//  2. The hex remembers. HexCell.trap: bit 7 armed, bits 0-5 who knows.
//  3. Knowledge is asymmetric. Only a survivor who got back out of one can
//     see it: encodeCell() sets the wire bit for that player alone.
//
// Outcomes, and nothing else changes a trap after worldgen:
//   escaped / left with nothing  -> still armed, now on that survivor's map
//   banked anything from a cache -> spent (taking the bait disarmed it)
//   any check failed             -> sprung: spent, and it hurt
// No re-arming, no dynamic placement. The Doom and the caravan walk straight
// over them -- traps are player-triggered only.

// ── Density ─────────────────────────────────────────────────────────────────
// Per mille, rolled once per eligible hex at generation. Agreed values
// (spec Q1, "density reading"): city core 18%, outskirts and lone ruins 8%,
// everywhere else 4%. The last band dominates the count -- open country is
// most of the map -- so crossing wild ground costs about one forced scene per
// 25 hexes and a city is a gauntlet. Measure with the bot harness
// (docs/bot-testing.md) before flashing a change here.
static constexpr uint16_t TRAP_PERMILLE_CORE     = 180;  // Broken Urban with 5+ urban neighbours
static constexpr uint16_t TRAP_PERMILLE_OUTSKIRT = 80;   // any other Broken Urban hex
static constexpr uint16_t TRAP_PERMILLE_OPEN     = 40;   // every other trappable surface hex
static constexpr uint16_t TRAP_PERMILLE_TUNNEL   = 80;   // bunker corridor floor -- someone rigged the door

enum : uint8_t { TRAP_BAND_OPEN = 0, TRAP_BAND_OUTSKIRT, TRAP_BAND_CORE, TRAP_BAND_TUNNEL };

// Surface terrain that can carry a trap. Settlements are the one safe ground
// in the game. Crater (10), River (11) and Collapsed Tunnel (15) are MC 255:
// a trap nobody can walk onto is a trap nobody springs. Bunker Entrance and
// Vent Shaft (12/13) are board transitions -- arriving on one moves you to
// the other board before anything could fire.
static inline bool trapTerrainOk(uint8_t t) {
  return t < NUM_TERRAIN && t != 9 && t != 10 && t != 11 && t != 12 && t != 13 && t != 15;
}

// ── Tier by richness ────────────────────────────────────────────────────────
// "Fat hexes get nasty traps, thin ones get cheap ones": the trap guards the
// hex's own bait, so a harder trap sits where the haul was always going to
// be better and no second loot source is invented. Richness is the resource
// pile lying on the hex at generation (0-3) plus where it is -- city core +3,
// outskirts and bunker +2, open country 0 -- and it weights the tier roll
// rather than fixing it, or with open ground holding nearly every trap the
// top tier would appear twice a world:
//
//   richness   0      1      2      3      4      5+
//   cheap     70     55     40     25     10      0
//   mid       25     30     35     40     45     50
//   top        5     15     25     35     45     55
//
// Simulated over the band mix above: roughly 60% cheap / 30% mid / 10% top,
// with the city scenes almost all mid and top.
static uint8_t trapRollTier(uint8_t band, uint8_t amount) {
  int rich = (int)min((uint8_t)3, amount);
  if (band == TRAP_BAND_CORE) rich += 3;
  else if (band != TRAP_BAND_OPEN) rich += 2;
  if (rich > 5) rich = 5;
  const int wCheap = max(0, 70 - 15 * rich);
  const int wMid   = 25 + 5 * rich;
  const int wTop   = 5 + 10 * rich;
  int roll = (int)(esp_random() % (uint32_t)(wCheap + wMid + wTop));
  if (roll < wCheap) return 0;
  return (roll < wCheap + wMid) ? 1 : 2;
}

// Deal file ids within a tier from a shuffled deck, reshuffled each pass, so
// every file in a tier turns up about equally often instead of whichever the
// dice favoured -- with ~100 cheap traps a world over six files, repetition is
// inevitable, clumping is not. Falls back to the nearest non-empty tier.
struct TrapDeck { uint8_t ids[64]; uint8_t n, next; };

static void trapDeckShuffle(TrapDeck& d) {
  for (int i = d.n - 1; i > 0; i--) {
    int j = (int)(esp_random() % (uint32_t)(i + 1));
    uint8_t t = d.ids[i]; d.ids[i] = d.ids[j]; d.ids[j] = t;
  }
  d.next = 0;
}

static uint8_t trapDeal(TrapDeck* decks, uint8_t tier) {
  static const int8_t FALLBACK[TRAP_TIERS][TRAP_TIERS] = { {0, 1, 2}, {1, 0, 2}, {2, 1, 0} };
  for (int k = 0; k < TRAP_TIERS; k++) {
    TrapDeck& d = decks[FALLBACK[tier][k]];
    if (!d.n) continue;
    if (d.next >= d.n) trapDeckShuffle(d);
    return d.ids[d.next++];
  }
  return 0;
}

// ── Placement (worldgen Phase 7) ────────────────────────────────────────────
// Called last in generateMap() (forward-declared in hex-map.hpp), after
// Phase 5 has dealt the POIs -- traps and POIs never share a hex, which keeps
// the icon unambiguous and never stacks two scenes on one tile -- and after
// generateTunnels(), which stamps hatch terrain onto finished surface hexes
// and carves the board below.
static void placeTraps() {
  for (int r = 0; r < MAP_ROWS; r++)
    for (int q = 0; q < MAP_COLS; q++) { G.map[r][q].trap = 0; G.map[r][q].trapEnc = 0; }
  for (int r = 0; r < TUN_ROWS; r++)
    for (int q = 0; q < TUN_COLS; q++) { G.tunnel[r][q].trap = 0; G.tunnel[r][q].trapEnc = 0; }

  if (encPools[ENC_POOL_TRAP].count == 0) {
    Log.warning("placeTraps: no traps pool in index.json -- the world has no traps");
    return;
  }

  TrapDeck decks[TRAP_TIERS] = {};
  for (int t = 0; t < TRAP_TIERS; t++) {
    for (int id = trapTierLo[t]; id <= trapTierHi[t] && decks[t].n < sizeof(decks[t].ids); id++)
      decks[t].ids[decks[t].n++] = (uint8_t)id;
    trapDeckShuffle(decks[t]);
  }

  int nBand[4] = {}, nTier[TRAP_TIERS] = {};
  auto arm = [&](HexCell& cell, uint8_t band) {
    uint8_t tier = trapRollTier(band, cell.resource ? cell.amount : 0);
    uint8_t id   = trapDeal(decks, tier);
    if (!id) return;
    cell.trap    = TRAP_ARMED;
    cell.trapEnc = id;
    nBand[band]++;
    for (int t = 0; t < TRAP_TIERS; t++)
      if (id >= trapTierLo[t] && id <= trapTierHi[t]) { nTier[t]++; break; }
  };

  // Surface. "City core" is the same test Phase 4 pins downtown art with and
  // Phase 5 partitions urban encounters by: an urban hex ringed by 5+ more.
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      HexCell& cell = G.map[r][q];
      if (!trapTerrainOk(cell.terrain) || cell.poi) continue;
      uint8_t band = TRAP_BAND_OPEN;
      if (cell.terrain == 4) {
        uint8_t un = 0;
        for (int d = 0; d < 6; d++)
          if (G.map[wrapR(r + DR[d])][wrapQ(q + DQ[d])].terrain == 4) un++;
        band = (un >= 5) ? TRAP_BAND_CORE : TRAP_BAND_OUTSKIRT;
      }
      static const uint16_t PERMILLE[3] = { TRAP_PERMILLE_OPEN, TRAP_PERMILLE_OUTSKIRT, TRAP_PERMILLE_CORE };
      if ((esp_random() % 1000) < PERMILLE[band]) arm(cell, band);
    }
  }

  // Tunnels: corridor floor only, and never a shaft's junction -- that cell
  // is the one every descent steps onto next, so a trap there would be a
  // toll on the whole network rather than a thing you might walk into.
  for (int r = 0; r < TUN_ROWS; r++) {
    for (int q = 0; q < TUN_COLS; q++) {
      HexCell& cell = G.tunnel[r][q];
      if (cell.terrain != 14 || cell.poi) continue;
      bool junction = false;
      for (int d = 0; d < 6 && !junction; d++) {
        int nq = q + DQ[d], nr = r + DR[d];
        if (tunIn(nq, nr) && hatchAtShaft(nq, nr) >= 0) junction = true;
      }
      if (junction) continue;
      if ((esp_random() % 1000) < TRAP_PERMILLE_TUNNEL) arm(cell, TRAP_BAND_TUNNEL);
    }
  }

  Log.notice("Traps placed: surface core=%d outskirts=%d open=%d, tunnels=%d; tiers cheap=%d mid=%d top=%d",
             nBand[TRAP_BAND_CORE], nBand[TRAP_BAND_OUTSKIRT], nBand[TRAP_BAND_OPEN],
             nBand[TRAP_BAND_TUNNEL], nTier[0], nTier[1], nTier[2]);
}

// ── Forced entry ────────────────────────────────────────────────────────────
// The last act of a step (movePlayer / moveTunnel), after the EVT_MOVE and
// after collectResource() -- you grab the bottle on your way into the punji
// pit, and that ordering is the point. The client is sent enc_path by
// handleMsg_move(), which sees the scene it did not ask for open under it.
//
// Arriving with 0 MP is fine (no trap choice costs MP). Arriving by a board
// transition cannot fire one: tunnelStepDown/Up land you on a shaft or a
// hatch, and placeTraps() never arms either. Two survivors on one hex: the
// armed bit is held clear while a scene runs, so the second walks in safely
// -- the first is in there with their hands in it.
static bool trapOnArrival(int pid) {
  Player& p = G.players[pid];
  if (p.ll == 0 || encounters[pid].active) return false;
  const bool below = (p.depth != 0);
  const int  q = below ? p.tq : p.q;
  const int  r = below ? p.tr : p.r;
  if (below ? !tunIn(q, r) : (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS)) return false;
  HexCell& cell = below ? G.tunnel[r][q] : G.map[r][q];
  if (!(cell.trap & TRAP_ARMED) || !cell.trapEnc) return false;

  const char* json = encLoadFile(ENC_POOL_TRAP, cell.trapEnc);
  if (!json) {
    // Stays armed: the next arrival tries again. Nothing on the wire, so the
    // survivor simply walked over it.
    Log.warning("trap pid=%d (%d,%d) dp=%d id=%d: file will not load", pid, q, r, (int)below, (int)cell.trapEnc);
    return false;
  }
  cell.trap &= (uint8_t)~TRAP_ARMED;   // held while the scene runs; trapSettle() decides the rest

  ActiveEncounter& enc = encounters[pid];
  enc = {};
  enc.active  = 1;
  enc.trap    = 1;
  enc.encIdx  = cell.trapEnc;
  enc.hexQ    = (uint8_t)q;
  enc.hexR    = (uint8_t)r;
  enc.depth   = below ? 1 : 0;
  enc.terrain = ENC_POOL_TRAP;
  encEnterStartNode(json, enc);
  // No threat-clock tick, unlike enc_start: nobody chose to walk in here,
  // and at this density a tick per trap would max the clock in a week.

  GameEvent ev = {};
  ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
  ev.q = (int16_t)q; ev.r = (int16_t)r; ev.depth = enc.depth;
  ev.amt = 1;   // EVT_ENC_START-specific: 1 = a trap fired, not a door opened
  enqEvt(ev);
  ledFlash(255, 60, 0);
  Log.notice("trap SPRING pid=%d (%d,%d) dp=%d id=%d", pid, q, r, (int)below, (int)cell.trapEnc);
  return true;
}

// ── Settling a scene ────────────────────────────────────────────────────────
// See the declaration in the .ino for the outcomes. Caller holds G.mutex and
// has not cleared encounters[pid] yet.
static void trapSettle(int pid, uint8_t outcome) {
  const ActiveEncounter& enc = encounters[pid];
  if (!enc.active || !enc.trap) return;
  const bool below = enc.depth != 0;
  const int  q = enc.hexQ, r = enc.hexR;
  if (below ? !tunIn(q, r) : (q >= MAP_COLS || r >= MAP_ROWS)) return;
  HexCell& cell = below ? G.tunnel[r][q] : G.map[r][q];
  if (!cell.trapEnc) return;   // the hex was wiped under the scene (a settlement took root)

  if (outcome == TRAP_SETTLE_REARM) {
    cell.trap |= TRAP_ARMED;   // exactly as it was; nothing to tell anyone
    return;
  }
  if (outcome == TRAP_OUT_KNOWN) {
    // Still armed, and now this survivor has it on their map -- for good:
    // the bit rides map.bin, and encodeCell() sends it whatever their vision
    // (the fog problem in the spec). Nobody else learns a thing, including
    // a teammate watching from the next hex.
    cell.trap |= (uint8_t)(TRAP_ARMED | (1u << pid));
  } else {
    cell.trap = 0;
    cell.trapEnc = 0;
  }
  GameEvent ev = {};
  ev.type = EVT_TRAP; ev.pid = (uint8_t)pid;
  ev.q = (int16_t)q; ev.r = (int16_t)r; ev.depth = below ? 1 : 0;
  ev.amt = outcome;
  if (outcome == TRAP_OUT_KNOWN) ev.evWsId = G.players[pid].wsClientId;   // theirs alone
  enqEvt(ev);
  Log.notice("trap %s pid=%d (%d,%d) dp=%d",
             outcome == TRAP_OUT_KNOWN ? "ESCAPED" : outcome == TRAP_OUT_SPENT ? "DISARMED" : "SPRUNG",
             pid, q, r, (int)below);
}
