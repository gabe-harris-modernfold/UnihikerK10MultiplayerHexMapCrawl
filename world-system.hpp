#pragma once
// ── world-system.hpp ──────────────────────────────────────────────────────
// Dynamic world entities that live on the hex map and tick on a slower clock
// than the game tick (WORLD_TICK_INTERVAL game ticks between updates). See
// docs/world-system-spec.md for the full design.
//
// Status: all three phases are implemented below — Caravan, Fire/Tracks, and
// Creeping Doom (the sole consumer of the track/scent system, and the other
// ignition source alongside the lightning-during-a-storm mechanic).
//
// Included from Esp32HexMapCrawl.ino right after hex-map.hpp (needs wrapQ/
// wrapR/hexDistWrap/DQ/DR/TERRAIN_MC) and before actions_game_loop.hpp
// (tickGame() calls tickCaravan()/resolveProximity()).

// ── Tuning ──────────────────────────────────────────────────────────────
static constexpr uint16_t WORLD_TICK_INTERVAL   = 150;  // game ticks between world updates (~15s)
static constexpr uint8_t  CARAVAN_RESTOCK_TICKS = 40;   // world ticks between caravan restocks
static constexpr uint8_t  CARAVAN_PID           = 254;  // sentinel tradeTo for EVT_TRADE_RESULT on caravan trades

static constexpr uint8_t  TRACK_AP_SCALE        = 2;    // heat added = mpSpent * TRACK_AP_SCALE
static constexpr uint8_t  TRACK_DECAY_RATE      = 3;    // track intensity lost per world tick
static constexpr uint8_t  FIRE_SPREAD_CHANCE    = 20;   // % chance a burning hex (intensity >=2) ignites each flammable neighbour per world tick
static constexpr uint8_t  FIRE_CAP              = 20;   // max simultaneously burning hexes
// Not in the original spec: lightning during a storm is an additional
// ignition source alongside Creeping Doom, which can also set fires deliberately.
static constexpr uint8_t  LIGHTNING_IGNITE_CHANCE = 15; // % per world tick while WEATHER_STORM is active

static constexpr uint8_t  SCENT_THRESHOLD = 8;   // min track intensity Doom will pursue
static constexpr uint8_t  AWARENESS_GAIN  = 12;  // per world tick when following scent
static constexpr uint8_t  AWARENESS_DECAY = 5;   // per world tick when cold

struct Caravan {
  int16_t  q, r;
  int16_t  wq, wr;        // current waypoint
  bool     active;
  uint8_t  inv[5];        // water/food/fuel/med/scrap — matches Player.inv
  uint8_t  restockTimer;  // world ticks until inventory refills
};

struct CreepingDoom {
  int16_t q, r;
  uint8_t awareness;  // 0-100; sole state driving all behavior — no mode enum,
                       // behavior is derived at tick time from threshold checks
};

struct WorldSystem {
  Caravan      caravan;
  CreepingDoom creepingDoom;
};
static WorldSystem W;

// Per-player (q,r) the caravan trade prompt last fired at — edge-triggers
// EVT_CARAVAN_TRADE so a parked player isn't re-prompted every world tick.
// -1 sentinel means "never prompted" (0,0 is a valid hex, so it can't double
// as the unset value). Cleared on player move, caravan move, and disconnect.
struct CaravanDebounce { int16_t q, r; };
static CaravanDebounce lastCaravanHex[MAX_PLAYERS];

static void clearCaravanDebounce() {
  for (int i = 0; i < MAX_PLAYERS; i++) { lastCaravanHex[i].q = -1; lastCaravanHex[i].r = -1; }
}

// Random passable hex — mirrors pickSpawnHex()'s fallback loop
// (inventory_items.hpp) minus the radiation preference, which is a player-
// health concern that doesn't apply to world entities.
static void pickPassableHex(int16_t& q, int16_t& r) {
  int attempts = 0;
  do {
    q = (int16_t)(esp_random() % MAP_COLS);
    r = (int16_t)(esp_random() % MAP_ROWS);
  } while (TERRAIN_MC[G.map[r][q].terrain] == 255 && ++attempts < 200);
}

// Steps (q,r) one hex toward (tq,tr), picking whichever of the 6 neighbours
// most reduces hexDistWrap. Skips impassable terrain unless ignoreTerrainCost
// is set (Creeping Doom at awareness >=76 "ignores terrain cost" per spec) —
// no-op (returns false) if boxed in by impassable terrain on every side and
// not ignoring it.
static bool moveOneStep(int16_t& q, int16_t& r, int16_t tq, int16_t tr, bool ignoreTerrainCost = false) {
  int bestDist = hexDistWrap(q, r, tq, tr);
  int bestD    = -1;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(q + DQ[d]);
    int nr = wrapR(r + DR[d]);
    if (!ignoreTerrainCost && TERRAIN_MC[G.map[nr][nq].terrain] == 255) continue;
    int dist = hexDistWrap(nq, nr, tq, tr);
    if (dist < bestDist) { bestDist = dist; bestD = d; }
  }
  if (bestD < 0) return false;
  q = (int16_t)wrapQ(q + DQ[bestD]);
  r = (int16_t)wrapR(r + DR[bestD]);
  return true;
}

// ── Hex-level dynamic state: fire + scent tracks ─────────────────────────
// Parallel to G.map — NOT part of HexCell, which is wire-encoded and shared
// with clients unchanged. Zero-initialised at boot (static global); also
// explicitly cleared in wInit() on map regen (see below).
struct HexDynamic {
  uint8_t fire;   // 0 = none, 1-3 = burning (1=ember, 2=burning, 3=inferno)
  uint8_t track;  // AP footprint intensity; 0-255, decays each world tick
};
// PSRAM-resident (allocated by allocPsramGlobals() in the .ino before setup()
// touches the world). Pointer-to-row keeps W_hex[r][q] indexing unchanged;
// use W_HEX_BYTES instead of W_HEX_BYTES for whole-array copies.
static HexDynamic (*W_hex)[MAP_COLS] = nullptr;
static constexpr size_t W_HEX_BYTES = sizeof(HexDynamic) * MAP_ROWS * MAP_COLS;

// Hexes currently burning (fire>0) — kept as a running count so wIgnite()/
// spreadFire() can enforce FIRE_CAP in O(1) instead of rescanning the grid.
static uint16_t fireCount = 0;

// Flammable: Open Scrub(0), Rust Forest(2), Marsh(3), Broken Urban(4),
// Rolling Hills(7), Settlement(9). Everything else — notably Mountain(8) and
// Glass Fields(6) — is immune: it cannot ignite, cannot hold fire, and fire
// cannot spread onto it from a burning neighbour.
static inline bool isFlammable(uint8_t terrain) {
  return terrain == 0 || terrain == 2 || terrain == 3 || terrain == 4 || terrain == 7 || terrain == 9;
}

// Ignites (q,r) to at least `intensity` if flammable and (for a hex not
// already burning) under FIRE_CAP. No-op on immune terrain or once the cap
// is reached — an already-burning hex can still be raised in intensity by a
// fresh strike/spread since it doesn't consume a new cap slot.
static void wIgnite(int16_t q, int16_t r, uint8_t intensity = 1) {
  if (!isFlammable(G.map[r][q].terrain)) return;
  HexDynamic& cell = W_hex[r][q];
  bool wasUnlit = (cell.fire == 0);
  if (wasUnlit) {
    if (fireCount >= FIRE_CAP) return;
    fireCount++;
  }
  if (intensity > cell.fire) cell.fire = intensity;
  if (wasUnlit) {
    GameEvent ev = {}; ev.type = EVT_FIRE_SPREAD;
    ev.q = q; ev.r = r; ev.amt = cell.fire;
    enqEvt(ev);
  }
}

// Smoke cuts vision independently of weather — declared/forward-referenced
// in hex-map.hpp's playerVisParams() (defined here since it needs W_hex).
// Scales directly with fire intensity (1-3), comparable in magnitude to the
// weather penalties in WEATHER_VIS_PENALTY (STORM=3, CHEM=5).
static int fireVisionPenalty(int q, int r) {
  return (int)W_hex[r][q].fire;
}

static void decayTracks() {
  for (int r = 0; r < MAP_ROWS; r++)
    for (int q = 0; q < MAP_COLS; q++)
      if (W_hex[r][q].track > 0)
        W_hex[r][q].track -= min((uint8_t)TRACK_DECAY_RATE, W_hex[r][q].track);
}

// Single call site: survival_skills.hpp's spendMP(), right after
// p.movesLeft is decremented — captures every action-driven track in one
// place. Movement and ACT_REST don't call spendMP, so stepping through a
// hex or resting leaves no scent; doing things in a hex does.
static void wOnPlayerAction(int16_t q, int16_t r, uint8_t mpSpent) {
  W_hex[r][q].track = (uint8_t)min(255, (int)W_hex[r][q].track + (int)mpSpent * TRACK_AP_SCALE);
}

// Lightning during a storm is the only fire-ignition source until Creeping
// Doom (Phase 3) can set fires deliberately. Struck hex is near a random
// connected player (same "near a connected player" bias maybeTriggerQuake()
// uses) rather than uniformly random on the map, so a strike is actually
// observable instead of landing somewhere nobody will ever see.
static void maybeIgniteLightning() {
  if (G.weatherPhase != WEATHER_STORM) return;
  if ((int)(esp_random() % 100) >= LIGHTNING_IGNITE_CHANCE) return;
  int connectedCount = 0;
  int16_t refQ[MAX_PLAYERS], refR[MAX_PLAYERS];
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (G.players[i].connected) { refQ[connectedCount] = G.players[i].q; refR[connectedCount] = G.players[i].r; connectedCount++; }
  if (connectedCount == 0) return;
  int pick = (int)(esp_random() % connectedCount);
  int16_t q = (int16_t)wrapQ((int)refQ[pick] + (int)(esp_random() % 9) - 4);
  int16_t r = (int16_t)wrapR((int)refR[pick] + (int)(esp_random() % 9) - 4);
  wIgnite(q, r, 2);  // strikes in hot, not just an ember; no-ops on immune terrain

  // Direct strike damage — anyone standing exactly on the struck hex takes
  // it regardless of whether the terrain was flammable (a mountain-top
  // survivor gets hit just as hard as one in dry scrub). Independent of the
  // fire it may have started, which resolveProximity() handles on later
  // ticks once the hex is actually burning.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || encounters[i].active || p.q != q || p.r != r) continue;
    p.ll = (uint8_t)max(0, (int)p.ll - 2);
    ledFlash(255, 255, 140);
    k10Play(MOTIF_BUNKER_ALARM);
    GameEvent ev = {}; ev.type = EVT_FIRE_DAMAGE;
    ev.pid = (uint8_t)i; ev.q = q; ev.r = r;
    ev.amt = 10;  // sentinel, clearly outside fire's 1-3 intensity range: direct lightning strike
    enqEvt(ev);
    if (p.ll == 0) {
      p.movesLeft = 0;
      GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)i;
      dev.evWsId = p.wsClientId; enqEvt(dev);
    }
  }
}

// Fire on (q,r) just went out — a permanent, one-way transition: Ash Dunes(1)
// isn't flammable (see isFlammable()), so a hex only ever burns once before
// it's permanently spent. Caller has already zeroed W_hex[r][q].fire (or is
// about to, via the double-buffer in spreadFire()); this just handles the
// side effects, so the rain-extinguish path below can share it.
static void onFireExtinguished(int q, int r) {
  fireCount--;
  G.map[r][q].terrain = 1;
  // amt=0 always means "just went out" — a hex is never re-broadcast at 0
  // any other time, so the client can key off that to convert its local
  // terrain too (see data/fire-field.js).
  GameEvent ev = {}; ev.type = EVT_FIRE_SPREAD;
  ev.q = (int16_t)q; ev.r = (int16_t)r; ev.amt = 0;
  enqEvt(ev);
}

// Double-buffered so spread within one world tick is simultaneous, not
// cascading (a hex ignited this tick doesn't itself spread again in the same
// pass). Every burning hex decays by 1; any hex at intensity >=2 has a
// chance to ignite each flammable neighbour, capped by FIRE_CAP. Plain rain
// (not the lightning-bearing storm phase) douses every burning hex outright.
static void spreadFire() {
  if (G.weatherPhase == WEATHER_RAIN) {
    for (int r = 0; r < MAP_ROWS; r++)
      for (int q = 0; q < MAP_COLS; q++)
        if (W_hex[r][q].fire > 0) { W_hex[r][q].fire = 0; onFireExtinguished(q, r); }
    return;
  }

  PSRAM_STATIC(HexDynamic, next, [MAP_ROWS][MAP_COLS]);  // static + PSRAM: off the task stack and internal .bss
  memcpy(next, W_hex, W_HEX_BYTES);

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      if (W_hex[r][q].fire == 0) continue;
      if (next[r][q].fire > 0 && --next[r][q].fire == 0) onFireExtinguished(q, r);

      if (W_hex[r][q].fire >= 2) {
        for (int d = 0; d < 6; d++) {
          int nq = wrapQ(q + DQ[d]);
          int nr = wrapR(r + DR[d]);
          if (next[nr][nq].fire > 0) continue;              // already burning this tick
          if (!isFlammable(G.map[nr][nq].terrain)) continue; // immune terrain
          if ((int)(esp_random() % 100) >= FIRE_SPREAD_CHANCE) continue;
          if (fireCount >= FIRE_CAP) continue;
          next[nr][nq].fire = 1;
          fireCount++;
          GameEvent ev = {}; ev.type = EVT_FIRE_SPREAD;
          ev.q = (int16_t)nq; ev.r = (int16_t)nr; ev.amt = 1;
          enqEvt(ev);
        }
      }
    }
  }
  memcpy(W_hex, next, W_HEX_BYTES);
}

// ── Creeping Doom ─────────────────────────────────────────────────────────
// 2 at awareness 0, 6 at awareness 100.
static int doomDetectionRadius() {
  return 2 + (W.creepingDoom.awareness / 25);
}

// Greedy scan of every hex within hexDistWrap <= radius (bounding box first,
// filtered by hexDistWrap — same result as scanning the whole map per the
// spec, just without wasting cycles on hexes no radius could ever include).
// No A* — this is deliberately not pathfinding.
static void hottestTrackWithin(int16_t q, int16_t r, int radius, int16_t* outQ, int16_t* outR) {
  int16_t bestQ = q, bestR = r;
  int     bestTrack = -1;
  for (int dr = -radius; dr <= radius; dr++) {
    for (int dq = -radius; dq <= radius; dq++) {
      int nq = wrapQ((int)q + dq);
      int nr = wrapR((int)r + dr);
      if (hexDistWrap(q, r, nq, nr) > radius) continue;
      int track = W_hex[nr][nq].track;
      if (track > bestTrack) { bestTrack = track; bestQ = (int16_t)nq; bestR = (int16_t)nr; }
    }
  }
  *outQ = bestQ; *outR = bestR;
}

// -1 if nobody connected. Skips encounter-locked players — Doom shouldn't
// home in on someone the encounter system has already frozen out of world
// interactions (matches the general "skip players in active encounters" rule).
static int nearestConnectedPlayer(int16_t q, int16_t r) {
  int best = -1, bestDist = 0x7FFFFFFF;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!G.players[i].connected || encounters[i].active) continue;
    int d = hexDistWrap(q, r, G.players[i].q, G.players[i].r);
    if (d < bestDist) { bestDist = d; best = i; }
  }
  return best;
}

static void randomWalk(int16_t& q, int16_t& r) {
  int d  = (int)(esp_random() % 6);
  int nq = wrapQ(q + DQ[d]);
  int nr = wrapR(r + DR[d]);
  if (TERRAIN_MC[G.map[nr][nq].terrain] == 255) return;  // blocked — stay put this tick
  q = (int16_t)nq; r = (int16_t)nr;
}

// Closes in on (tq,tr) but stops one hex short — "locks adjacent to player",
// not on top of them. No-op once already at distance <=1.
static void stepAdjacentTo(int16_t& q, int16_t& r, int16_t tq, int16_t tr) {
  if (hexDistWrap(q, r, tq, tr) <= 1) return;
  moveOneStep(q, r, tq, tr, /*ignoreTerrainCost=*/true);
}

// Read tracks -> move -> act (may ignite hexes). Must run before
// decayTracks() each world tick — see the spec's tick-order note: single-
// action heat (Forage = ~4) needs to still be smellable above
// SCENT_THRESHOLD on the tick Doom reads it, before that same tick's decay.
static void tickCreepingDoom() {
  if (W.creepingDoom.awareness >= 100) {
    int tgt = nearestConnectedPlayer(W.creepingDoom.q, W.creepingDoom.r);
    if (tgt >= 0) {
      stepAdjacentTo(W.creepingDoom.q, W.creepingDoom.r, G.players[tgt].q, G.players[tgt].r);
      wIgnite(W.creepingDoom.q, W.creepingDoom.r, 2);
      return;
    }
    // No player connected — fall through to the scent path and let awareness decay.
  }

  int radius = doomDetectionRadius();
  int16_t hq, hr;
  hottestTrackWithin(W.creepingDoom.q, W.creepingDoom.r, radius, &hq, &hr);

  if (W_hex[hr][hq].track >= SCENT_THRESHOLD) {
    bool ignoreCost = W.creepingDoom.awareness >= 76;
    moveOneStep(W.creepingDoom.q, W.creepingDoom.r, hq, hr, ignoreCost);
    W.creepingDoom.awareness = (uint8_t)min(100, (int)W.creepingDoom.awareness + AWARENESS_GAIN);
  } else {
    randomWalk(W.creepingDoom.q, W.creepingDoom.r);
    W.creepingDoom.awareness = (uint8_t)max(0, (int)W.creepingDoom.awareness - AWARENESS_DECAY);
  }

  if (W.creepingDoom.awareness >= 76)
    wIgnite(W.creepingDoom.q, W.creepingDoom.r, 2);
}

// Awareness-driven proximity effect, adjacent to Doom only (hexDistWrap<=1) —
// same "adjacent, not co-located" range as stepAdjacentTo's approach.
// Doom already moved to its new hex this tick by the time this runs.
static void resolveDoomProximity() {
  if (W.creepingDoom.awareness < 51) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || encounters[i].active) continue;
    if (hexDistWrap(p.q, p.r, W.creepingDoom.q, W.creepingDoom.r) > 1) continue;

    if (W.creepingDoom.awareness < 76) {
      // 51-75: warning only — dread, audio cue. Sent to every connected
      // player regardless of position: Creeping Doom is a world-level
      // threat, not a local one, per the spec.
      k10Play(MOTIF_DISTANT_THUD);
      GameEvent ev = {}; ev.type = EVT_DOOM_WARNING; ev.pid = (uint8_t)i;
      enqEvt(ev);
      continue;
    }

    // 76-99: destroy one resource node on the player's hex. 100: that, plus
    // LL-1 per world tick (amt carries the LL actually lost, 0 or 1).
    HexCell& cell = G.map[p.r][p.q];
    cell.amount = 0;
    uint8_t llLost = 0;
    if (W.creepingDoom.awareness >= 100 && p.ll > 0) {
      p.ll--;
      llLost = 1;
      if (p.ll == 0) {
        p.movesLeft = 0;
        GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)i;
        dev.evWsId = p.wsClientId; enqEvt(dev);
      }
    }
    ledFlash(140, 0, 160);
    k10Play(MOTIF_ROTTEN_CHORD);
    GameEvent aev = {}; aev.type = EVT_DOOM_ACT; aev.pid = (uint8_t)i;
    aev.q = p.q; aev.r = p.r; aev.amt = llLost;
    enqEvt(aev);
  }
}

// 8-sample search preferring open ground away from fire and Creeping Doom,
// per the spec's scoring formula.
static void pickCaravanWaypoint() {
  int16_t bestQ = 0, bestR = 0;
  int     bestScore = -1;
  bool    found = false;
  for (int i = 0; i < 8; i++) {
    int16_t q = (int16_t)(esp_random() % MAP_COLS);
    int16_t r = (int16_t)(esp_random() % MAP_ROWS);
    if (TERRAIN_MC[G.map[r][q].terrain] == 255) continue;
    int score = (int)W_hex[r][q].fire * 10;
    if (hexDistWrap(q, r, W.creepingDoom.q, W.creepingDoom.r) < 3) score += 50;
    if (!found || score < bestScore) { bestScore = score; bestQ = q; bestR = r; found = true; }
  }
  if (found) { W.caravan.wq = bestQ; W.caravan.wr = bestR; }
  else pickPassableHex(W.caravan.wq, W.caravan.wr);  // all 8 samples impassable — fall back
}

static void restockCaravan() {
  W.caravan.restockTimer = CARAVAN_RESTOCK_TICKS;
  // Terrain-weighted restock amounts are an unspecified v2 tuning knob (see
  // spec); v1 refills every slot by a flat random amount so the caravan is
  // never left fully dry after a restock.
  for (int i = 0; i < 5; i++)
    W.caravan.inv[i] = (uint8_t)min(99, (int)W.caravan.inv[i] + 4 + (int)(esp_random() % 5));
}

static void tickCaravan() {
  if (!W.caravan.active) return;

  if (W.caravan.q != W.caravan.wq || W.caravan.r != W.caravan.wr) {
    if (moveOneStep(W.caravan.q, W.caravan.r, W.caravan.wq, W.caravan.wr))
      clearCaravanDebounce();
  } else {
    pickCaravanWaypoint();
  }

  if (W.caravan.restockTimer > 0) W.caravan.restockTimer--;
  else restockCaravan();
}

// Fire damage — level-triggered (every tick standing in it hurts). Must run
// BEFORE spreadFire()'s decay this same tick: a hex lightning just set to
// intensity 2 would otherwise already be decayed to 1 (< the damage
// threshold) by the time this checked it, so a single-tick spike would never
// land a hit. Intensity 1 (ember) is a warning only, matching the spec's
// escalation table.
static void resolveFireDamage() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || encounters[i].active) continue;
    uint8_t fireHere = W_hex[p.r][p.q].fire;
    if (fireHere < 2 || p.ll == 0) continue;
    p.ll--;
    if (fireHere == 3) p.radiation = (uint8_t)min(255, (int)p.radiation + 1);
    ledFlash(255, 60, 0);
    k10Play(MOTIF_WARNING_GRUNT);
    GameEvent fev = {};
    fev.type = EVT_FIRE_DAMAGE;
    fev.pid  = (uint8_t)i;
    fev.q = p.q; fev.r = p.r; fev.amt = fireHere;
    enqEvt(fev);
    if (p.ll == 0) {
      p.movesLeft = 0;
      GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)i;
      dev.evWsId = p.wsClientId; enqEvt(dev);
    }
  }
}

// Caravan trade prompt, called once per world tick after the caravan moves.
// Edge-triggered (see lastCaravanHex comment above). Doom warnings fold into
// this same function in Phase 3.
static void resolveCaravanProximity() {
  if (!W.caravan.active) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || encounters[i].active) continue;
    if (p.q != W.caravan.q || p.r != W.caravan.r) continue;
    if (lastCaravanHex[i].q == W.caravan.q && lastCaravanHex[i].r == W.caravan.r) continue;
    lastCaravanHex[i].q = W.caravan.q;
    lastCaravanHex[i].r = W.caravan.r;
    GameEvent ev = {};
    ev.type = EVT_CARAVAN_TRADE;
    ev.pid  = (uint8_t)i;
    enqEvt(ev);
  }
}

// Zero-clears world state and places the caravan on a random passable hex.
// Call sites: setup() (boot) and the EVT_REGEN handler (map regen) — see
// docs/world-system-spec.md. Caller holds G.mutex.
static void wInit() {
  memset(&W, 0, sizeof(W));
  memset(W_hex, 0, W_HEX_BYTES);
  fireCount = 0;
  clearCaravanDebounce();
  pickPassableHex(W.creepingDoom.q, W.creepingDoom.r);  // placed first: pickCaravanWaypoint scores against it
  W.creepingDoom.awareness = 0;
  pickPassableHex(W.caravan.q, W.caravan.r);
  pickCaravanWaypoint();
  W.caravan.active = true;
  restockCaravan();
}
