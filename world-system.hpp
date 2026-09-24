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
static constexpr uint8_t  CARAVAN_STOCK_MAX     = 3;    // max units of one consumable on the shelf (CARAVAN_STOCK_SLOTS lives in the .ino — SaveHeader needs it)
static constexpr uint8_t  CARAVAN_STUCK_LIMIT   = 6;    // world ticks of no progress toward the waypoint before the caravan gives up and re-routes (river divide / mountain wedge)
static constexpr uint8_t  CARAVAN_TRADE_HOLD    = 4;    // world ticks the caravan waits while a survivor shares its hex (~60s) before rolling on regardless

static constexpr uint8_t  TRACK_AP_SCALE        = 2;    // heat added = mpSpent * TRACK_AP_SCALE
static constexpr uint8_t  TRACK_DECAY_RATE      = 1;    // track intensity lost per world tick (a 2 MP action stays smellable ~4 ticks / 60 s)
static constexpr uint8_t  FIRE_SPREAD_CHANCE    = 20;   // % chance a burning hex (intensity >=2) ignites each flammable neighbour per world tick
static constexpr uint8_t  FIRE_CAP              = 20;   // max simultaneously burning hexes
// Not in the original spec: lightning during a storm is an additional
// ignition source alongside Creeping Doom, which can also set fires deliberately.
static constexpr uint8_t  LIGHTNING_IGNITE_CHANCE = 17; // % per world tick while WEATHER_STORM is active (was 15: +15% relative)

// Flash flood: another storm-gated hazard alongside lightning, but hits
// water terrain instead of flammable terrain and washes out neighbours
// rather than burning them.
static constexpr uint8_t  FLASH_FLOOD_CHANCE  = 12;  // % per world tick while WEATHER_STORM is active
static constexpr uint8_t  FLOOD_SPREAD_CHANCE = 20;  // % chance a flooded hex (intensity >=2) washes out each eligible neighbour per world tick
static constexpr uint8_t  FLOOD_CAP           = 15;  // max simultaneously flooded hexes
static constexpr uint8_t  FLOOD_MAX_INTENSITY = 3;
// LL cost of being caught on a hex the instant it washes out. Deliberately
// 1 rather than lightning's 2: the flood also zeroes movesLeft, and with
// effectiveMP = ll + 3 the stranding is the harsher half of the hit — MP is
// how you reach water. 1 + a wiped turn lands near lightning's 2 for total
// severity while being far less likely to kill outright, which is the shape
// the tension measurements say this game is short of (see
// docs/bot-testing.md: deaths:near-misses measured 1:1, target ~1:4).
static constexpr uint8_t  FLOOD_LL_DAMAGE     = 1;
static constexpr uint8_t  FLOOD_SEARCH_RADIUS = 4;   // jitter box (± radius) searched for a water hex near the reference player

static constexpr uint8_t  SCENT_THRESHOLD = 4;   // min track intensity Doom will pursue (one 2 MP action, or one 2 MP move, clears it)
static constexpr uint8_t  DOOM_BASE_RADIUS = 6;  // scent radius at awareness 0 (see doomDetectionRadius)
static constexpr uint8_t  AWARENESS_GAIN  = 12;  // per world tick when following scent
static constexpr uint8_t  AWARENESS_DECAY = 5;   // per world tick when cold
// Taunts fire on every awareness-tier change, and then keep needling at this
// cadence while the Doom is still aware of anyone. 8 world ticks is ~2 min:
// often enough to feel stalked, rare enough that it never fights the toast
// stack for attention with the action feedback the player actually needs.
static constexpr uint8_t  DOOM_TAUNT_COOLDOWN = 8;  // world ticks between repeat taunts
// Floor between ANY two taunts, including ones a tier change would otherwise
// let jump the queue. Awareness sitting on a threshold flutters across it
// (gain 12 / decay 5 per tick), and without this a running battle on the
// 76 or 100 boundary taunts every world tick.
static constexpr uint8_t  DOOM_TAUNT_MIN_GAP  = 2;  // world ticks

struct Caravan {
  int16_t  q, r;
  int16_t  wq, wr;        // current waypoint (always a Settlement once active)
  int16_t  pq, pr;        // settlement just departed; -1,-1 = none yet.
                           // Excluded from the next pick alongside the current
                           // hex so two mutually-nearest settlements don't
                           // leave the caravan bouncing between just those two.
  bool     active;
  uint8_t  inv[5];        // water/food/fuel/med/scrap — matches Player.inv
  uint8_t  restockTimer;  // world ticks until inventory refills
  uint8_t  holdTicks;     // consecutive world ticks spent waiting on a co-located
                          // trader (tickCaravan). Runtime only — a reboot just
                          // restarts the wait, so it stays out of SaveHeader.
  uint8_t  stuckTicks;    // consecutive world ticks with no progress toward the
                          // waypoint. The river divide can leave a settlement on
                          // the far bank that greedy stepping can never reach, so
                          // past CARAVAN_STUCK_LIMIT the route is re-picked.
                          // Runtime only, same as holdTicks.
  // Consumable shelf — what the trader sells for resource tokens (car_buy).
  // Parallel arrays, stockItem 0 = empty slot. The asking price isn't stored:
  // caravanPrice() reads items.cfg's "value", so the cfg stays the one place
  // to tune prices (and a reboot re-prices whatever is already on the shelf).
  uint8_t  stockItem[CARAVAN_STOCK_SLOTS];
  uint8_t  stockQty[CARAVAN_STOCK_SLOTS];
};

struct CreepingDoom {
  int16_t q, r;
  uint8_t awareness;  // 0-100; sole state driving all behavior — no mode enum,
                       // behavior is derived at tick time from threshold checks
  // Taunt bookkeeping. Not persisted (only q/r/awareness go in SaveHeader);
  // lastTauntTier is re-seeded from the restored awareness on load so a
  // reboot mid-hunt doesn't fire a spurious "it noticed you" on tick one.
  uint8_t lastTauntTier;
  uint8_t tauntCooldown;
  // Audio bookkeeping, same deal — derived state, never persisted.
  // lastAudioBand is reset to 0 on load so a reboot can't fire the release
  // cue for a hunt the player never heard start.
  uint8_t lastAudioBand;   // 0 silent / 1 far / 2 near / 3 hunt
  uint8_t audioPhase;      // world ticks inside the current band, for cadence
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
  int sideD    = -1;
  uint32_t sideN = 0;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(q + DQ[d]);
    int nr = wrapR(r + DR[d]);
    if (!ignoreTerrainCost && TERRAIN_MC[G.map[nr][nq].terrain] == 255) continue;
    int dist = hexDistWrap(nq, nr, tq, tr);
    if (dist < bestDist) { bestDist = dist; bestD = d; }
    // Equal-distance sidestep, reservoir-sampled so no direction is favoured.
    // Only used when nothing strictly improves: it lets a walker slide along
    // an impassable wall (river bank, mountain range) instead of pinning
    // against it. Callers that care about making progress must watch the
    // distance themselves — see tickCaravan's stuckTicks.
    else if (dist == bestDist && bestD < 0 && (esp_random() % (++sideN)) == 0) sideD = d;
  }
  if (bestD < 0) bestD = sideD;
  if (bestD < 0) return false;
  q = (int16_t)wrapQ(q + DQ[bestD]);
  r = (int16_t)wrapR(r + DR[bestD]);
  return true;
}

// ── Hex-level dynamic state: fire + scent tracks + flood ─────────────────
// Parallel to G.map — NOT part of HexCell, which is wire-encoded and shared
// with clients unchanged. Zero-initialised at boot (static global); also
// explicitly cleared in wInit() on map regen (see below).
struct HexDynamic {
  uint8_t fire;   // 0 = none, 1-3 = burning (1=ember, 2=burning, 3=inferno)
  uint8_t track;  // AP footprint intensity; 0-255, decays each world tick
  uint8_t flood;  // 0 = none, 1-3 = flash-flood intensity, rises while storming, recedes after
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

// Hexes currently flooded (flood>0) — mirrors fireCount, so
// maybeTriggerFlashFlood()/spreadFlood() can enforce FLOOD_CAP in O(1).
static uint16_t floodCount = 0;

// Washout-eligible: Open Scrub(0) and Rolling Hills(7) only — the two dry
// terrains a flash flood can permanently drown into Flooded District(5).
// Existing water terrain (River Channel(11), Flooded District(5) itself)
// is never a washout target, just a flood source.
static inline bool isWashoutEligible(uint8_t terrain) {
  return terrain == 0 || terrain == 7;
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
// weather penalties in WEATHER_VIS_PENALTY (STORM=2, CHEM=3).
static int fireVisionPenalty(int q, int r) {
  return (int)W_hex[r][q].fire;
}

// Flash flood cuts move speed, not vision — additive alongside
// WEATHER_MOVE_PENALTY in survival_state.hpp's move-cost calc. Forward-
// declared in hex-map.hpp next to fireVisionPenalty for the same reason.
static int floodMovePenalty(int q, int r) {
  return (int)W_hex[r][q].flood;
}

static void decayTracks() {
  for (int r = 0; r < MAP_ROWS; r++)
    for (int q = 0; q < MAP_COLS; q++)
      if (W_hex[r][q].track > 0)
        W_hex[r][q].track -= min((uint8_t)TRACK_DECAY_RATE, W_hex[r][q].track);
}

// Call sites: survival_skills.hpp's spendMP(), right after p.movesLeft is
// decremented (captures every action-driven track in one place), and
// movePlayer() in survival_state.hpp, which deducts the terrain move cost
// directly rather than going through spendMP. Travelling therefore lays a
// trail proportional to how hard the ground was to cross — open scrub
// (1 MP -> 2 heat) stays under SCENT_THRESHOLD, rough terrain doesn't.
// ACT_REST still calls neither, so resting stays silent — the spec's
// intended counter to Doom's attention.
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
    if (G.players[i].connected && !G.players[i].depth) { refQ[connectedCount] = G.players[i].q; refR[connectedCount] = G.players[i].r; connectedCount++; }
  if (connectedCount == 0) return;
  // Where the bolt lands, relative to the survivor it picked.
  //
  // This used to be a bare uniform jitter box, which made "does lightning
  // hit anyone" an accident of geometry rather than a number anyone could
  // tune: a direct strike needs the jittered hex to BE the player's hex, so
  // +/-4 (81 cells) meant 1-in-81 and +/-2 (25 cells) 1-in-25. Measured, the
  // first gave ~0.1 direct hits per 2-hour session and the second ~0.4, and
  // LIGHTNING_IGNITE_CHANCE barely moved either -- the box dominated it.
  //
  // So the direct hit is now its own explicit probability, and the jitter
  // only decides where the *near* misses land (and therefore where fires
  // start, which is the other half of what lightning is for).
  static constexpr uint8_t LIGHTNING_DIRECT_CHANCE = 30;  // % of strikes that land ON the survivor
  static constexpr int     LIGHTNING_JITTER        = 2;
  static constexpr int     LIGHTNING_SPAN          = LIGHTNING_JITTER * 2 + 1;
  int pick = (int)(esp_random() % connectedCount);
  int16_t q, r;
  if ((int)(esp_random() % 100) < LIGHTNING_DIRECT_CHANCE) {
    q = refQ[pick];
    r = refR[pick];
  } else {
    q = (int16_t)wrapQ((int)refQ[pick] + (int)(esp_random() % LIGHTNING_SPAN) - LIGHTNING_JITTER);
    r = (int16_t)wrapR((int)refR[pick] + (int)(esp_random() % LIGHTNING_SPAN) - LIGHTNING_JITTER);
  }
  wIgnite(q, r, 2);  // strikes in hot, not just an ember; no-ops on immune terrain

  // Direct strike damage — anyone standing exactly on the struck hex takes
  // it regardless of whether the terrain was flammable (a mountain-top
  // survivor gets hit just as hard as one in dry scrub). Independent of the
  // fire it may have started, which resolveProximity() handles on later
  // ticks once the hex is actually burning.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || encounters[i].active || p.depth || p.q != q || p.r != r) continue;
    // Already downed and waiting for the slot reset: every other damage site
    // guards this (resolveFireDamage, resolveDoomProximity, duskCheck,
    // dawnUpkeep) and lightning did not. `p.ll` clamps at 0, so the strike
    // itself was harmless -- but `if (p.ll == 0)` below then queued a SECOND
    // EVT_DOWNED for a player already being downed, and that handler does
    // `G.connectedCount--` and appends to lobbyIds. Two of them corrupts the
    // seat count, which handleConnect uses to decide when the board is full.
    if (p.ll == 0) continue;
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
      dev.res = DC_LIGHTNING;
      dev.evWsId = p.wsClientId; enqEvt(dev);
    }
  }
}

// Flash flood: the other storm-gated hazard alongside lightning. Rather than
// striking directly at a jittered point (lightning's approach), it needs to
// find actual water terrain to start from, so it searches a small jittered
// box around a random connected player and seeds the first water hex
// (TERRAIN_HAS_WATER) it finds. No-op that tick if none is found nearby —
// a storm doesn't always happen to be over a river.
static void maybeTriggerFlashFlood() {
  if (G.weatherPhase != WEATHER_STORM) return;
  if ((int)(esp_random() % 100) >= FLASH_FLOOD_CHANCE) return;
  int connectedCount = 0;
  int16_t refQ[MAX_PLAYERS], refR[MAX_PLAYERS];
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (G.players[i].connected && !G.players[i].depth) { refQ[connectedCount] = G.players[i].q; refR[connectedCount] = G.players[i].r; connectedCount++; }
  if (connectedCount == 0) return;
  int pick = (int)(esp_random() % connectedCount);
  int span = FLOOD_SEARCH_RADIUS * 2 + 1;
  for (int i = 0; i < span * span; i++) {
    int dq = (int)(esp_random() % span) - FLOOD_SEARCH_RADIUS;
    int dr = (int)(esp_random() % span) - FLOOD_SEARCH_RADIUS;
    int q = wrapQ((int)refQ[pick] + dq);
    int r = wrapR((int)refR[pick] + dr);
    if (!TERRAIN_HAS_WATER[G.map[r][q].terrain]) continue;
    HexDynamic& cell = W_hex[r][q];
    if (cell.flood == 0) {
      if (floodCount >= FLOOD_CAP) return;
      floodCount++;
    }
    cell.flood = max(cell.flood, (uint8_t)2);
    return;
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

// Double-buffered like spreadFire(), but the branches are inverted: while
// the storm that caused it continues, every flooded hex rises in intensity
// and (once >=2) can wash out an eligible dry neighbour — permanent,
// one-way, just like fire's burnout, except two-staged instead of one pop:
// dry Scrub(0)/Hills(7) floods into Marsh(3) first (a swampy edge), and a
// Marsh hex that stays at max intensity long enough fully drowns into
// Flooded District(5). The newly-flooded hex keeps carrying flood intensity
// afterward instead of self-extinguishing, so the "core" of the flood keeps
// advancing behind its own edge. Once the storm passes, floods simply
// recede (decay by 1/tick, no spread) rather than being doused outright —
// there's no single-tick "rain washes away a flood" moment the way plain
// rain douses fire.
static void spreadFlood() {
  if (G.weatherPhase != WEATHER_STORM) {
    for (int r = 0; r < MAP_ROWS; r++)
      for (int q = 0; q < MAP_COLS; q++)
        if (W_hex[r][q].flood > 0 && --W_hex[r][q].flood == 0) floodCount--;
    return;
  }

  PSRAM_STATIC(HexDynamic, nextFlood, [MAP_ROWS][MAP_COLS]);
  memcpy(nextFlood, W_hex, W_HEX_BYTES);

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      if (W_hex[r][q].flood == 0) continue;
      if (nextFlood[r][q].flood < FLOOD_MAX_INTENSITY && ++nextFlood[r][q].flood == FLOOD_MAX_INTENSITY
          && G.map[r][q].terrain == 3) {
        // Second stage: a swamped edge hex that's stayed underwater long
        // enough (reached max intensity) fully drowns into Flooded
        // District — the flood's "core" advances behind its own edge
        // instead of the whole washed area popping straight to open water.
        G.map[r][q].terrain = 5;
        GameEvent sev = {}; sev.type = EVT_FLOOD_WASHOUT;
        sev.q = (int16_t)q; sev.r = (int16_t)r; sev.amt = 5;
        enqEvt(sev);
      }

      if (W_hex[r][q].flood >= 2) {
        for (int d = 0; d < 6; d++) {
          int nq = wrapQ(q + DQ[d]);
          int nr = wrapR(r + DR[d]);
          if (nextFlood[nr][nq].flood > 0) continue;                 // already flooded this tick
          if (!isWashoutEligible(G.map[nr][nq].terrain)) continue;   // not a washout-eligible terrain
          if ((int)(esp_random() % 100) >= FLOOD_SPREAD_CHANCE) continue;
          if (floodCount >= FLOOD_CAP) continue;

          // First stage: dry ground pushed by the advancing flood front
          // becomes Marsh — a swampy edge, not open water outright (see
          // isWashoutEligible(): only ever dry Scrub(0)/Hills(7) reach this
          // branch, since a hex already at Marsh/water carries flood>0 and
          // is skipped by the check above).
          G.map[nr][nq].terrain = 3;
          nextFlood[nr][nq].flood = 1;
          floodCount++;
          GameEvent ev = {}; ev.type = EVT_FLOOD_WASHOUT;
          ev.q = (int16_t)nq; ev.r = (int16_t)nr; ev.amt = 3;
          enqEvt(ev);

          // Anyone standing on the hex the instant it washes out is swept —
          // independent of the water damage/movement penalty the flood
          // leaves behind, same "hit the player who happened to be there"
          // idiom as maybeIgniteLightning()'s direct strike.
          for (int i = 0; i < MAX_PLAYERS; i++) {
            Player& p = G.players[i];
            if (!p.connected || encounters[i].active || p.depth || p.q != nq || p.r != nr) continue;
            if (p.ll == 0) continue;   // already down, slot reset pending — same guard as resolveFireDamage()
            p.ll = (uint8_t)max(0, (int)p.ll - (int)FLOOD_LL_DAMAGE);
            p.movesLeft = 0;
            ledFlash(80, 160, 255);
            k10Play(MOTIF_SEWER_ECHO);
            GameEvent dev = {}; dev.type = EVT_FLOOD_DAMAGE;
            dev.pid = (uint8_t)i; dev.q = (int16_t)nq; dev.r = (int16_t)nr;
            dev.amt = 10;  // sentinel, outside flood's 1-3 intensity range: swept off your feet
            dev.res = FLOOD_LL_DAMAGE;  // LL actually lost — `res` is unused for flood events
            enqEvt(dev);
            // Damage event first, then the downing — the same order every
            // other hazard uses, and the order causes.py matches against.
            if (p.ll == 0) {
              GameEvent ddev = {}; ddev.type = EVT_DOWNED; ddev.pid = (uint8_t)i;
              ddev.res = DC_FLOOD;
              ddev.evWsId = p.wsClientId; enqEvt(ddev);
            }
          }
        }
      }
    }
  }
  memcpy(W_hex, nextFlood, W_HEX_BYTES);
}

  // Underground survivors are out of the world system's reach: their q/r is
  // still their entrance hatch, so without a depth check fire, lightning,
  // floods, the Doom and the caravan would all act on someone who is not
  // actually standing there.
// ── Creeping Doom ─────────────────────────────────────────────────────────
// DOOM_BASE_RADIUS at awareness 0, +4 at awareness 100 (6 -> 10).
// The original base of 2 made the dormant Doom effectively blind: on a
// 75-column map a random-walking entity that can only smell 2 hexes will
// essentially never cross a player's trail, so awareness never left 0 and
// none of the >=51 effects could ever fire on hardware.
static int doomDetectionRadius() {
  return DOOM_BASE_RADIUS + (W.creepingDoom.awareness / 25);
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
    if (!G.players[i].connected || encounters[i].active || G.players[i].depth) continue;
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

// Hexes covered per world tick. One is the "creeping" baseline; the upper
// tiers key off the same 76/100 thresholds every other behaviour does. Even
// at 3 this is ~1 hex per 5 s against a player's ~1 hex per 0.2-0.9 s
// (MOVE_CD_MS * mc), so running is always a valid answer — what it buys is
// that a hunting Doom actually closes ground on someone who has stopped,
// instead of needing minutes per hex to reach them.
static int doomStepsPerTick() {
  if (W.creepingDoom.awareness >= 100) return 3;
  if (W.creepingDoom.awareness >=  76) return 2;
  return 1;
}

// Read tracks -> move -> act (may ignite hexes). Must run before
// decayTracks() each world tick — see the spec's tick-order note: single-
// action heat (Forage = ~4) needs to still be smellable above
// SCENT_THRESHOLD on the tick Doom reads it, before that same tick's decay.
//
// The scent read happens FIRST, including at awareness 100. The lock-on
// branch used to run ahead of it and `return` unconditionally, which meant
// awareness was never touched again once it hit 100: with anyone connected
// it was a permanent hunt with no way out, contradicting the spec's "resting
// is the natural counter to Creeping Doom's attention". Gating the lock on
// live scent restores that counter at every tier — go still, let your tracks
// go cold, and it loses you and cools off like it does at any other level.
static void tickCreepingDoom() {
  int radius = doomDetectionRadius();
  int16_t hq, hr;
  hottestTrackWithin(W.creepingDoom.q, W.creepingDoom.r, radius, &hq, &hr);
  bool hasScent = W_hex[hr][hq].track >= SCENT_THRESHOLD;
  int  steps    = doomStepsPerTick();

  // Awareness 100 + a live trail: forget the trail and go for the survivor
  // themselves. Awareness is already clamped at 100, so no gain is applied.
  if (W.creepingDoom.awareness >= 100 && hasScent) {
    int tgt = nearestConnectedPlayer(W.creepingDoom.q, W.creepingDoom.r);
    if (tgt >= 0) {
      for (int s = 0; s < steps; s++)
        stepAdjacentTo(W.creepingDoom.q, W.creepingDoom.r, G.players[tgt].q, G.players[tgt].r);
      wIgnite(W.creepingDoom.q, W.creepingDoom.r, 2);
      return;
    }
    // Nobody targetable — fall through to the scent path.
  }

  if (hasScent) {
    bool ignoreCost = W.creepingDoom.awareness >= 76;
    for (int s = 0; s < steps; s++)
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
    if (!p.connected || encounters[i].active || p.depth) continue;
    if (hexDistWrap(p.q, p.r, W.creepingDoom.q, W.creepingDoom.r) > 1) continue;

    if (W.creepingDoom.awareness < 76) {
      // 51-75: warning only — dread, no mechanical effect. Sent to every
      // connected player regardless of position: Creeping Doom is a
      // world-level threat, not a local one, per the spec.
      // No motif here: the Doom's audio is the distance-driven ostinato in
      // tickDoomAudio(), which is already sounding (and at its tightest)
      // by the time anything is adjacent enough to reach this branch.
      GameEvent ev = {}; ev.type = EVT_DOOM_WARNING; ev.pid = (uint8_t)i;
      enqEvt(ev);
      continue;
    }

    // 76-99: destroy one resource node on the player's hex. 100: that, plus
    // LL-1 per world tick (amt carries the LL actually lost, 0 or 1).
    // Mirrors collectResource()'s own drained-to-zero branch (clear
    // `resource` + arm `respawnTimer`) — leaving `resource` non-zero would
    // never satisfy the dawn respawn loop's `resource==0` gate, so the node
    // would stay dead forever instead of eventually respawning. No-op if
    // the hex had nothing to destroy.
    HexCell& cell = G.map[p.r][p.q];
    if (cell.resource != 0) {
      cell.resource     = 0;
      cell.amount       = 0;
      cell.respawnTimer = RESPAWN_TICKS;
    }
    uint8_t llLost = 0;
    if (W.creepingDoom.awareness >= 100 && p.ll > 0) {
      p.ll--;
      llLost = 1;
      if (p.ll == 0) {
        p.movesLeft = 0;
        GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)i;
        dev.res = DC_DOOM;
        dev.evWsId = p.wsClientId; enqEvt(dev);
      }
    }
    ledFlash(140, 0, 160);
    // Audio deliberately omitted — see the warn branch above. At this range
    // tickDoomAudio() is playing MOTIF_DOOM_HUNT every world tick, and a
    // one-shot here would only race it and lose (k10PlaySeq drops a cue
    // while another is live).
    GameEvent aev = {}; aev.type = EVT_DOOM_ACT; aev.pid = (uint8_t)i;
    aev.q = p.q; aev.r = p.r; aev.amt = llLost;
    enqEvt(aev);
  }
}

// ── Doom taunts ───────────────────────────────────────────────────────────
// The Doom's voice. Keyed off the same awareness thresholds every other
// behaviour tier uses, so a taunt always coincides with a real change in what
// it is doing to you rather than being decorative noise:
//   1 = 51-75  it has your scent (warn tier)
//   2 = 76-99  it is unmaking what you gather (destroy tier)
//   3 = 100    it has stopped tracking and started hunting
//   0 = <51    it lost you — the release beat, only ever sent after a higher
//              tier, so a session that never drew its attention stays silent
static uint8_t doomTauntTier() {
  if (W.creepingDoom.awareness >= 100) return 3;
  if (W.creepingDoom.awareness >=  76) return 2;
  if (W.creepingDoom.awareness >=  51) return 1;
  return 0;
}

// Only the tier and a line index go on the wire — the wording itself lives in
// DOOM_TAUNTS (data/game-data.js) and in the K10_SAY lists below. The index is
// a raw byte the client reduces modulo its own table length, so the two sides
// never have to agree on how many lines a tier has.
//
// Runs after resolveDoomProximity() so the taunt lands *after* the mechanical
// event it comments on (you read "your supplies rot", then the voice).
static void tickDoomTaunts() {
  uint8_t tier = doomTauntTier();
  bool    changed = (tier != W.creepingDoom.lastTauntTier);

  if (W.creepingDoom.tauntCooldown > 0) W.creepingDoom.tauntCooldown--;
  uint8_t sinceLast = (uint8_t)(DOOM_TAUNT_COOLDOWN - W.creepingDoom.tauntCooldown);

  // A tier change is news and speaks early, but never inside MIN_GAP of the
  // last line. Otherwise only the full cooldown lets it speak again, and only
  // while it is actually aware of someone.
  bool speak = changed ? (sinceLast >= DOOM_TAUNT_MIN_GAP)
                       : (tier > 0 && W.creepingDoom.tauntCooldown == 0);
  if (!speak) return;

  // Falling to 0 without ever having risen is not a "it lost you" moment.
  if (tier == 0 && W.creepingDoom.lastTauntTier == 0) return;

  int tgt = nearestConnectedPlayer(W.creepingDoom.q, W.creepingDoom.r);
  W.creepingDoom.lastTauntTier = tier;
  if (tgt < 0) return;  // nobody to speak to; tier is still recorded
  W.creepingDoom.tauntCooldown = DOOM_TAUNT_COOLDOWN;

  GameEvent ev = {};
  ev.type = EVT_DOOM_TAUNT;
  ev.pid  = (uint8_t)tgt;
  ev.amt  = tier;
  ev.res  = (uint8_t)(esp_random() & 0xFF);  // client reduces mod its table length
  enqEvt(ev);
}

// ── Doom audio ────────────────────────────────────────────────────────────
// The Doom's ostinato (MOTIF_DOOM_*, tone-motifs.hpp): two notes a minor 2nd
// apart whose *repeat rate* is the signal. Because tempo is the message, the
// band is chosen by how close the Doom actually is, not by awareness —
// awareness is how locked-on it is, which is a different question from where
// it is standing. Awareness only decides whether it makes any sound at all,
// at the same 51 threshold resolveDoomProximity() uses for its first effect.
//
// Scent radius is the audible range, so the cue arrives on exactly the terms
// the entity works on: if it can smell you, you can hear it. That also means
// a fully-aware Doom on the far side of the map is silent, which is correct —
// the sound is proximity, and it should be possible to outrun it.
static uint8_t doomAudioBand() {
  if (W.creepingDoom.awareness < 51) return 0;
  int tgt = nearestConnectedPlayer(W.creepingDoom.q, W.creepingDoom.r);
  if (tgt < 0) return 0;

  int dist   = hexDistWrap(W.creepingDoom.q, W.creepingDoom.r,
                           G.players[tgt].q,  G.players[tgt].r);
  int radius = doomDetectionRadius();
  if (dist > radius) return 0;                              // out of its nose, out of earshot
  if (dist <= 2 || W.creepingDoom.awareness >= 100) return 3;
  return (dist * 2 <= radius) ? 2 : 1;                      // inner half / outer half
}

// Runs last in the Doom's slice of the world tick, after the taunt, so the
// order a player experiences is: effect, then voice, then the sound of it
// still being there. One figure is ~1.0-1.1 s against a 15 s world tick, so
// even at the tightest band this occupies ~7% of the single tone voice and
// rarely swallows another cue (k10PlaySeq drops, never queues).
static void tickDoomAudio() {
  uint8_t band = doomAudioBand();
  uint8_t prev = W.creepingDoom.lastAudioBand;
  W.creepingDoom.lastAudioBand = band;

  if (band == 0) {
    W.creepingDoom.audioPhase = 0;
    // The pair breaks. Only ever after it had actually closed on someone, so
    // a session that never drew its attention stays silent — same rule as
    // the tier-0 taunt.
    if (prev > 0) k10Play(MOTIF_DOOM_LOST);
    return;
  }

  // Odd phases only for the far band: ~30 s between figures out at the edge
  // of its range, every 15 s once it is inside half. Incremented before the
  // test so entering a band always sounds immediately rather than on the
  // tick after.
  W.creepingDoom.audioPhase++;
  if (band == 1 && (W.creepingDoom.audioPhase & 1) == 0) return;

  if      (band == 3) k10Play(MOTIF_DOOM_HUNT);
  else if (band == 2) k10Play(MOTIF_DOOM_NEAR);
  else                k10Play(MOTIF_DOOM_FAR);
}

// Nearest Settlement to the caravan's current position — a trade route, not
// a wander. Excludes the hex it's standing on (its current stop) and, when
// another candidate exists, the settlement it just left (W.caravan.pq/pr):
// a plain nearest-neighbour walk would otherwise settle into bouncing
// between two mutually-nearest settlements forever instead of working its
// way across the map. Falls back to a random passable hex only if the map
// has no other settlement to head to at all.
static void pickCaravanWaypoint() {
  int16_t curQ = W.caravan.q, curR = W.caravan.r;
  int16_t prevQ = W.caravan.pq, prevR = W.caravan.pr;

  int16_t bestQ = 0, bestR = 0; int bestDist = 0; bool found = false;    // excludes current + previous
  int16_t altQ  = 0, altR  = 0; int altDist  = 0; bool altFound = false; // excludes current only

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      if (G.map[r][c].terrain != TERRAIN_SETTLEMENT) continue;
      if (c == curQ && r == curR) continue;
      int dist = hexDistWrap(curQ, curR, c, r);
      if (!altFound || dist < altDist) { altDist = dist; altQ = (int16_t)c; altR = (int16_t)r; altFound = true; }
      if (c == prevQ && r == prevR) continue;
      if (!found || dist < bestDist) { bestDist = dist; bestQ = (int16_t)c; bestR = (int16_t)r; found = true; }
    }
  }

  if (found || altFound) {
    W.caravan.pq = curQ; W.caravan.pr = curR;
    W.caravan.wq = found ? bestQ : altQ;
    W.caravan.wr = found ? bestR : altR;
  } else {
    pickPassableHex(W.caravan.wq, W.caravan.wr);  // no settlement anywhere on the map — degenerate fallback
  }
}

// ── Caravan shelf ──────────────────────────────────────────────────────────
// Asking price per unit, in token-worth: items.cfg "value", floored at 1 so
// a mis-keyed item can never be free.
static uint8_t caravanPrice(const ItemDef* def) {
  return (def && def->value) ? def->value : 1;
}

// What one token of each resource is worth to the caravan when a survivor
// pays for shelf goods (water/food/fuel/med/scrap). Water is worth nothing
// out here and is refused outright (see handleMsg_caravan_buy); fuel counts
// double. Mirror of CARAVAN_TOKEN_WORTH in data/game-data.js and the mock.
static const uint8_t CARAVAN_TOKEN_WORTH[5] = { 0, 1, 2, 1, 1 };

// Worth of a payment vector to the caravan.
static int caravanPaymentValue(const uint8_t give[5]) {
  int v = 0;
  for (int i = 0; i < 5; i++) v += (int)give[i] * (int)CARAVAN_TOKEN_WORTH[i];
  return v;
}

// True if the caravan may put this item on its shelf: a consumable flagged
// trade=yes in items.cfg. Crafted concoctions are trade=no so a learned
// recipe stays the only way to get one; key items are trade=no by definition.
static bool caravanCanStock(const ItemDef& def) {
  return def.id != 0 && def.category == ITEM_CONSUMABLE && def.tradeable != 0;
}

// A random stockable consumable not already on the shelf — 0 if items.cfg
// has none, in which case the shelf just stays empty. Reservoir-sampled
// straight off the registry so no MAX_ITEMS scratch array is needed.
static uint8_t rollCaravanStockItem() {
  uint8_t pick = 0;
  int     seen = 0;
  for (int i = 0; i < (int)itemCount; i++) {
    const ItemDef& d = itemRegistry[i];
    if (!caravanCanStock(d)) continue;
    bool onShelf = false;
    for (int s = 0; s < CARAVAN_STOCK_SLOTS; s++)
      if (W.caravan.stockItem[s] == d.id) { onShelf = true; break; }
    if (onShelf) continue;
    seen++;
    if ((int)(esp_random() % (uint32_t)seen) == 0) pick = d.id;
  }
  return pick;
}

// Shelf slot holding at least one of itemId, or -1.
static int caravanStockSlot(uint8_t itemId) {
  if (!itemId) return -1;
  for (int s = 0; s < CARAVAN_STOCK_SLOTS; s++)
    if (W.caravan.stockItem[s] == itemId && W.caravan.stockQty[s]) return s;
  return -1;
}

static void restockCaravan() {
  W.caravan.restockTimer = CARAVAN_RESTOCK_TICKS;
  // Terrain-weighted restock amounts are an unspecified v2 tuning knob (see
  // spec); v1 refills every slot by a flat random amount so the caravan is
  // never left fully dry after a restock.
  for (int i = 0; i < 5; i++)
    W.caravan.inv[i] = (uint8_t)min(99, (int)W.caravan.inv[i] + 4 + (int)(esp_random() % 5));
  // Shelf: one occupied slot is swapped for something new every restock so
  // the stock still rotates for a party that never buys; empty slots get a
  // fresh roll; everything else gains a unit up to CARAVAN_STOCK_MAX.
  int rotate = (int)(esp_random() % CARAVAN_STOCK_SLOTS);
  if (W.caravan.stockQty[rotate]) { W.caravan.stockItem[rotate] = 0; W.caravan.stockQty[rotate] = 0; }
  for (int s = 0; s < CARAVAN_STOCK_SLOTS; s++) {
    if (!W.caravan.stockQty[s]) {
      W.caravan.stockItem[s] = rollCaravanStockItem();
      W.caravan.stockQty[s]  = W.caravan.stockItem[s] ? (uint8_t)(1 + esp_random() % CARAVAN_STOCK_MAX) : 0;
    } else if (W.caravan.stockQty[s] < CARAVAN_STOCK_MAX) {
      W.caravan.stockQty[s]++;
    }
  }
}

// Is anyone standing in the shop? Same filter resolveCaravanProximity() uses to
// decide who gets the trade prompt, plus a downed survivor not counting — they
// cannot open the action panel, so there is nothing to wait for.
static bool caravanHasCustomer() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player& p = G.players[i];
    if (!p.connected || encounters[i].active || p.depth || p.ll == 0) continue;
    if (p.q == W.caravan.q && p.r == W.caravan.r) return true;
  }
  return false;
}

static void tickCaravan() {
  if (!W.caravan.active) return;

  // Wait while someone is trading instead of rolling off mid-purchase: a
  // car_buy aimed at a caravan that just stepped away fails with "the caravan
  // has moved on" (handleMsg_caravan_buy), which reads as a bug from the shelf
  // screen. Capped at CARAVAN_TRADE_HOLD so a camper cannot pin the trade route
  // indefinitely; the count resets as soon as the hex is clear again.
  if (caravanHasCustomer() && W.caravan.holdTicks < CARAVAN_TRADE_HOLD) {
    W.caravan.holdTicks++;
  } else if (W.caravan.q != W.caravan.wq || W.caravan.r != W.caravan.wr) {
    W.caravan.holdTicks = 0;
    // Track progress, not merely movement: moveOneStep can sidestep along an
    // obstacle forever without ever closing on the waypoint. A settlement on
    // the far side of the river is simply unreachable by greedy stepping, so
    // after CARAVAN_STUCK_LIMIT fruitless ticks the route is abandoned rather
    // than parking the trader against the bank for the rest of the world's life.
    int before = hexDistWrap(W.caravan.q, W.caravan.r, W.caravan.wq, W.caravan.wr);
    if (moveOneStep(W.caravan.q, W.caravan.r, W.caravan.wq, W.caravan.wr)) {
      clearCaravanDebounce();
      // Tire tracks — same "mark the hex I just entered" idiom as player
      // footprints (survival_state.hpp), but one shared bit instead of a
      // per-player mask since there's only one caravan.
      G.map[W.caravan.r][W.caravan.q].tireTrack = 1;
    }
    int after = hexDistWrap(W.caravan.q, W.caravan.r, W.caravan.wq, W.caravan.wr);
    if (after < before) {
      W.caravan.stuckTicks = 0;
    } else if (++W.caravan.stuckTicks >= CARAVAN_STUCK_LIMIT) {
      W.caravan.stuckTicks = 0;
      pickCaravanWaypoint();
    }
  } else {
    W.caravan.holdTicks = 0;
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
//
// Damage scales with intensity rather than being flat, so the ember →
// burn → blaze ladder the escalation table describes is something you feel
// and not just something the tile art says: 2 (burning) costs 1, 3 (blaze)
// costs 2 and irradiates. Standing in a full blaze is now as bad per tick as
// a direct lightning strike, which is the intent — it is the one hazard you
// can always see coming and walk out of.
static inline uint8_t fireDamageFor(uint8_t intensity) {
  return (intensity >= 3) ? 2 : 1;
}

static void resolveFireDamage() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || encounters[i].active || p.depth) continue;
    uint8_t fireHere = W_hex[p.r][p.q].fire;
    if (fireHere < 2 || p.ll == 0) continue;
    p.ll = (uint8_t)max(0, (int)p.ll - (int)fireDamageFor(fireHere));
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
      dev.res = DC_FIRE;
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
    if (!p.connected || encounters[i].active || p.depth) continue;
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
  floodCount = 0;
  clearCaravanDebounce();
  pickPassableHex(W.creepingDoom.q, W.creepingDoom.r);
  W.creepingDoom.awareness = 0;
  pickPassableHex(W.caravan.q, W.caravan.r);
  W.caravan.pq = -1; W.caravan.pr = -1;  // sentinel: no settlement departed yet (0,0 is a valid hex)
  pickCaravanWaypoint();
  W.caravan.active = true;
  restockCaravan();
}

// ── Minimap markers ────────────────────────────────────────────────────────
// Declared in ui-screens.hpp (included before this file) so drawMapScreen()
// can plot the Caravan and Creeping Doom. Caller holds G.mutex.
static void snapshotWorldMarkers(WorldMarkers& out) {
  out.caravanQ      = W.caravan.q;
  out.caravanR      = W.caravan.r;
  out.caravanActive = W.caravan.active;
  out.doomQ         = W.creepingDoom.q;
  out.doomR         = W.creepingDoom.r;
  out.doomAwareness = W.creepingDoom.awareness;
  out.doomRadius    = (int16_t)doomDetectionRadius();
}
