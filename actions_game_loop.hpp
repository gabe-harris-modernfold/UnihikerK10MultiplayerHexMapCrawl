#pragma once
// ── Action handlers and game tick ─────────────────────────────────────────────
// Included from Esp32HexMapCrawl.ino after survival_state.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

// ── Weather phase state machine ───────────────────────────────────────────────
// Called once per dawn (not per tick) so resting through days advances weather normally.
// Real-time floor: a day normally takes DAY_TICKS (5 real minutes), but it
// can end early the moment every connected player is resting — so a group
// spamming REST back-to-back could otherwise collapse days to seconds and
// cycle weather absurdly fast. lastWeatherChangeMs/weatherNextGapMs enforce
// a ~2-3 real-minute floor between actual phase changes regardless of how
// fast in-game days are flying by; the day-counter mechanic above still
// governs everything else (bad-weather streak, duration-in-days) untouched.
// Not persisted across reboot/load — same as lastQuakeMs — a slightly-early
// first change right after a fresh boot is a harmless edge case.
static uint32_t lastWeatherChangeMs = 0;
static uint32_t weatherNextGapMs    = 120000;
static void updateWeatherPhase() {
  // Accumulate bad-weather streak in game-days
  if (G.weatherPhase != WEATHER_CLEAR) G.badWeatherTicks++;

  if (G.weatherCounter > 0) { G.weatherCounter--; return; }

  if (lastWeatherChangeMs != 0 && millis() - lastWeatherChangeMs < weatherNextGapMs) {
    G.weatherCounter = 1;  // day-counter says "change now" — try again next dawn instead
    return;
  }

  static constexpr uint16_t BAD_WEATHER_CAP = 6; // max 6 game-days of bad weather
  uint8_t next;
  if (G.badWeatherTicks >= BAD_WEATHER_CAP) {
    next = WEATHER_CLEAR;  // force clear once the bad-weather streak hits the cap
  } else {
    uint32_t roll = esp_random() % 100;
    next = G.weatherPhase;
    switch (G.weatherPhase) {
      // Plain mist is the common, everyday fog — rolls in out of clear
      // skies or after rain, usually just burns off, but can occasionally
      // thicken into the dangerous Strangle Fog. Strangle Fog itself only
      // arrives directly from clear skies or as a storm/chem cloud breaking
      // up — otherwise it mostly just resolves back to clear or rain.
      case WEATHER_CLEAR: next = (roll < 50) ? WEATHER_RAIN  : (roll < 70) ? WEATHER_STORM : (roll < 90) ? WEATHER_MIST : WEATHER_FOG; break;
      case WEATHER_RAIN:  next = (roll < 40) ? WEATHER_STORM : (roll < 75) ? WEATHER_CLEAR : WEATHER_MIST; break;
      case WEATHER_STORM:
        next = (roll < 30) ? WEATHER_CHEM : (roll < 55) ? WEATHER_RAIN : (roll < 80) ? WEATHER_CLEAR : WEATHER_FOG; break;
      case WEATHER_CHEM:
        next = (roll < 20) ? WEATHER_CLEAR : (roll < 45) ? WEATHER_STORM : (roll < 70) ? WEATHER_RAIN : WEATHER_FOG; break;
      case WEATHER_FOG:
        next = (roll < 50) ? WEATHER_CLEAR : (roll < 75) ? WEATHER_RAIN : WEATHER_STORM; break;
      case WEATHER_MIST:
        next = (roll < 60) ? WEATHER_CLEAR : (roll < 85) ? WEATHER_RAIN : WEATHER_FOG; break;
    }
  }

  if (next == WEATHER_CLEAR) G.badWeatherTicks = 0;

  G.weatherCounter = WEATHER_DUR_MIN[next] +
    (uint16_t)(esp_random() % (WEATHER_DUR_MAX[next] - WEATHER_DUR_MIN[next] + 1));
  G.weatherPhase      = next;
  lastWeatherChangeMs = millis();
  weatherNextGapMs    = 120000 + (esp_random() % 60001);  // 2-3 real minutes until the next one
  GameEvent ev = {}; ev.type = EVT_WEATHER;
  ev.q = (int16_t)next; ev.r = (int16_t)G.weatherCounter;
  enqEvt(ev);
}

// ── Quakes ─────────────────────────────────────────────────────────────────
// Occasional earthquake: ruptures a straight line of QUAKE_MIN_LEN..MAX_LEN
// hexes near a connected player, destroys any shelter caught on it, and
// levels any Settlement on the line to Open Scrub (terrain 9 -> 0; see
// TERRAIN_IMG_NAME / TERRAIN_NAME index order above). Mirrors mock-server/
// server.js's triggerQuake()/maybeTriggerQuake() closely enough that the
// wire message ("t":"ev","k":"quake",...) is byte-identical between the mock
// and the firmware. Built directly here rather than through GameEvent/
// enqEvt/drainEvents (the same way sendSync() hand-rolls its own JSON)
// because a variable-length cell list doesn't fit the fixed-size GameEvent
// struct without bloating every queued event for every other type.
static constexpr uint8_t  QUAKE_MIN_LEN     = 7;
static constexpr uint8_t  QUAKE_MAX_LEN     = 10;
// Was a 45s floor + 2%/tick roll — recurred roughly every 45-55 real seconds
// on average (near-instant near mountains), far too often for a dramatic,
// destructive one-off. Now a 3 min floor + 1%/tick roll averages out to
// roughly once every 3-3.5 real minutes (near mountains: ~3.05 min).
static constexpr uint32_t QUAKE_MIN_GAP_MS  = 180000;
static constexpr uint8_t  QUAKE_TRIGGER_PCT = 1;  // rolled once per tick (100 ms) once the gap has elapsed
static uint32_t lastQuakeMs = 0;

static inline bool isMountainous(int q, int r) {
  uint8_t t = G.map[r][q].terrain;
  return t == 7 || t == 8;  // Rolling Hills, Mountain
}

// Samples a few candidate start points around (refQ, refR) and prefers one
// that lands on Hills/Mountain — fault lines are drawn to real rough country
// instead of landing uniformly at random. Mirrors mock-server/server.js's
// pickQuakeOrigin().
static void pickQuakeOrigin(int16_t refQ, int16_t refR, int* outQ, int* outR) {
  int fallbackQ = 0, fallbackR = 0;
  for (uint8_t i = 0; i < 6; i++) {
    int q = wrapQ((int)refQ + (int)(esp_random() % 17) - 8);
    int r = wrapR((int)refR + (int)(esp_random() % 13) - 6);
    if (i == 0) { fallbackQ = q; fallbackR = r; }
    if (isMountainous(q, r)) { *outQ = q; *outR = r; return; }
  }
  *outQ = fallbackQ; *outR = fallbackR;
}

struct QuakeResult {
  bool    fired;
  uint8_t len;
  int16_t cellQ[QUAKE_MAX_LEN], cellR[QUAKE_MAX_LEN];
  uint8_t destroyedCount;
  int16_t destQ[QUAKE_MAX_LEN], destR[QUAKE_MAX_LEN];
  uint8_t convertedCount;
  int16_t convQ[QUAKE_MAX_LEN], convR[QUAKE_MAX_LEN];
};

// Walks a straight fault line from (refQ, refR) ± a random offset in a random
// hex direction, clearing G.map[...].shelter and leveling any Settlement to
// Open Scrub on anything it crosses. Call while holding G.mutex — out.fired's
// JSON is broadcast by the caller after releasing it (see tickGame()).
static void triggerQuake(int16_t refQ, int16_t refR, QuakeResult& out) {
  uint8_t len = QUAKE_MIN_LEN + (uint8_t)(esp_random() % (QUAKE_MAX_LEN - QUAKE_MIN_LEN + 1));
  uint8_t dir = (uint8_t)(esp_random() % 6);
  int q, r;
  pickQuakeOrigin(refQ, refR, &q, &r);

  out.len = len;
  out.destroyedCount = 0;
  out.convertedCount = 0;
  for (uint8_t i = 0; i < len; i++) {
    int cq = wrapQ(q), cr = wrapR(r);
    out.cellQ[i] = (int16_t)cq;
    out.cellR[i] = (int16_t)cr;
    HexCell& cell = G.map[cr][cq];
    if (cell.shelter) {
      cell.shelter = 0;
      out.destQ[out.destroyedCount] = (int16_t)cq;
      out.destR[out.destroyedCount] = (int16_t)cr;
      out.destroyedCount++;
    }
    if (cell.terrain == 9) {  // Settlement -> Open Scrub
      cell.terrain = 0;
      out.convQ[out.convertedCount] = (int16_t)cq;
      out.convR[out.convertedCount] = (int16_t)cr;
      out.convertedCount++;
    }
    q += DQ[dir]; r += DR[dir];
  }
  out.fired = true;
  lastQuakeMs = millis();
}

// Rolls a chance (once the cooldown has elapsed) to rupture a fault line near
// a connected player. Call once per tick while holding G.mutex. Standing near
// Hills/Mountain makes it several times more likely — real fault country, not
// a uniform roll anywhere on the map — mirroring the mock's maybeTriggerQuake().
static void maybeTriggerQuake(QuakeResult& out) {
  if (millis() - lastQuakeMs < QUAKE_MIN_GAP_MS) return;
  uint8_t candidates[MAX_PLAYERS], n = 0;
  int8_t  nearMountainIdx = -1;
  for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
    if (!G.players[i].connected) continue;
    candidates[n++] = i;
    if (nearMountainIdx < 0 && isMountainous(G.players[i].q, G.players[i].r)) nearMountainIdx = (int8_t)i;
  }
  if (n == 0) return;
  uint8_t chancePct = (nearMountainIdx >= 0) ? (uint8_t)(QUAKE_TRIGGER_PCT * 3) : QUAKE_TRIGGER_PCT;
  if ((esp_random() % 100) >= chancePct) return;
  Player& ref = (nearMountainIdx >= 0) ? G.players[nearMountainIdx] : G.players[candidates[esp_random() % n]];
  triggerQuake(ref.q, ref.r, out);
}

// Builds and broadcasts the quake's JSON. Call OUTSIDE G.mutex (does WS I/O),
// mirroring how saveGame() is deferred until after tickGame() releases it.
static void broadcastQuake(const QuakeResult& q) {
  char buf[768];
  int len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"quake\",\"cells\":[");
  for (uint8_t i = 0; i < q.len; i++)
    len += snprintf(buf + len, sizeof(buf) - len, "%s{\"q\":%d,\"r\":%d}",
                     i ? "," : "", (int)q.cellQ[i], (int)q.cellR[i]);
  len += snprintf(buf + len, sizeof(buf) - len, "],\"destroyed\":[");
  for (uint8_t i = 0; i < q.destroyedCount; i++)
    len += snprintf(buf + len, sizeof(buf) - len, "%s{\"q\":%d,\"r\":%d}",
                     i ? "," : "", (int)q.destQ[i], (int)q.destR[i]);
  len += snprintf(buf + len, sizeof(buf) - len, "],\"converted\":[");
  for (uint8_t i = 0; i < q.convertedCount; i++)
    len += snprintf(buf + len, sizeof(buf) - len, "%s{\"q\":%d,\"r\":%d}",
                     i ? "," : "", (int)q.convQ[i], (int)q.convR[i]);
  len += snprintf(buf + len, sizeof(buf) - len, "]}");
  ws.textAll(buf, len);
  Log.notice("EVT quake len=%d destroyed=%d converted=%d start=(%d,%d)",
             (int)q.len, (int)q.destroyedCount, (int)q.convertedCount, (int)q.cellQ[0], (int)q.cellR[0]);
  char lb[34];
  if (q.convertedCount > 0)
    snprintf(lb, sizeof(lb), "Quake leveled %d settlement%s", (int)q.convertedCount, q.convertedCount > 1 ? "s" : "");
  else if (q.destroyedCount > 0)
    snprintf(lb, sizeof(lb), "Quake destroyed %d shelter%s", (int)q.destroyedCount, q.destroyedCount > 1 ? "s" : "");
  else
    snprintf(lb, sizeof(lb), "Earthquake!");
  k10LogAdd(lb);
  k10Play(MOTIF_DISTANT_THUD);
}

// ── Game tick (Core 1) ────────────────────────────────────────────────────────
static void tickGame() {
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  G.tickId++;

  // ── Day cycle: advance dayTick; trigger Dawn when full day elapses ──────
  G.dayTick++;

  // ── Early dusk: if all connected players are resting, end day immediately ──
  bool allResting = false;
  int connCount = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (G.players[i].connected) {
      connCount++;
      if (!G.players[i].resting) {
        allResting = false;
        break;
      }
      allResting = true;
    }
  }

  bool dawnOccurred = false;
  if (G.dayTick >= DAY_TICKS || (connCount > 0 && allResting)) {
    G.dayTick = 0;
    G.dayCount++;
    dawnOccurred = true;
    // Force-abort any active encounters at dawn.  Involuntary, so no TC
    // increment and the POI goes back on the hex for another visit.
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!encounters[i].active) continue;
      endEncounter(i, ENC_END_DAWN, /*restorePoi=*/true);
    }
    duskCheck();    // end-of-day radiation Endure checks (R ≥ 7); enqueues EVT_DUSK
    updateWeatherPhase();  // advance weather once per game-day
    // Threat decays 1/day.  Without this the clock only ever climbs and every
    // encounter is permanently pinned at the +20 risk ceiling.
    if (G.threatClock > 0) G.threatClock--;
    dawnUpkeep();   // modifies player state, enqueues EVT_DAWN per connected player
    // Note: shelters are now permanent and persist across days
  }

  // ── Resource respawn ────────────────────────────────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      if (cell.resource == 0 && cell.respawnTimer > 0) {
        if (--cell.respawnTimer == 0) {
          uint32_t rnd  = esp_random();
          cell.resource = terrainSpawnRes(cell.terrain, rnd);
          if (cell.resource == 0) { cell.respawnTimer = 0; continue; }
          cell.amount   = 1 + (uint8_t)(rnd % 3);
          GameEvent rev = {};
          rev.type = EVT_RESPAWN; rev.q = (int16_t)c; rev.r = (int16_t)r;
          rev.res  = cell.resource; rev.amt = cell.amount;
          enqEvt(rev);
        }
      }
    }
  }

  // ── Chem-storm per-tick hazard ────────────────────────────────────────────
  // CHEM_TICK_RATE is tuned so a survivor standing in the open on the worst
  // terrain (intensity 1.0) loses about 1 LL per real minute (600 ticks), i.e.
  // roughly 5 LL over a full 300-second day — dangerous, but survivable long
  // enough to reach cover.  Basic shelter halves the rate; improved shelter,
  // Settlement, and Broken Urban are immune.
  if (G.weatherPhase == WEATHER_CHEM) {
    static constexpr float CHEM_TICK_RATE = 1.0f / 600.0f;
    for (int pid = 0; pid < MAX_PLAYERS; pid++) {
      Player& p = G.players[pid];
      if (!p.connected || (p.ll == 0)) continue;
      uint8_t t = G.map[p.r][p.q].terrain;
      if (t >= NUM_TERRAIN) t = 0;
      if (t == 9 || TERRAIN_IS_RUINS[t]) continue;
      uint8_t shelter = G.map[p.r][p.q].shelter;
      if (shelter >= 2) continue;
      float prob = WEATHER_INTENSITY[WEATHER_CHEM][t] * CHEM_TICK_RATE;
      if (shelter == 1) prob *= 0.5f;
      if (esp_random() < (uint32_t)(prob * 0xFFFFFFFFul)) {
        if (p.ll > 0) { p.ll--; ledFlash(0, 100, 0); k10Play(MOTIF_ACID_DRIP); }
        if (p.ll == 0) {
          p.movesLeft = 0;
          GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)pid;
          dev.evWsId = p.wsClientId; enqEvt(dev);
        }
      }
    }
  }

  // ── Strangle Fog per-tick hazard ──────────────────────────────────────────
  // Fog costs you two different ways: MP bleeds away steadily (turned around,
  // fighting through it — no LED/sound cue, it'd be constant noise at this
  // cadence) while LL loss is far rarer, a real but occasional risk of
  // getting properly lost rather than a steady bleed like chem's.
  // FOG_MP_TICK_RATE ~= 1 MP lost per 30s of full exposure on the worst
  // terrain — enough to strand a lingering survivor well before a 300s day
  // is out. FOG_LL_TICK_RATE ~= 1 LL per 3 real minutes at worst, rarely more
  // than 1 LL across a whole day spent in it. Same shelter/terrain immunity
  // shape as chem: basic shelter halves both, improved shelter/Settlement/
  // Broken Urban are immune.
  if (G.weatherPhase == WEATHER_FOG) {
    static constexpr float FOG_MP_TICK_RATE = 1.0f / 300.0f;
    static constexpr float FOG_LL_TICK_RATE = 1.0f / 1800.0f;
    for (int pid = 0; pid < MAX_PLAYERS; pid++) {
      Player& p = G.players[pid];
      if (!p.connected || (p.ll == 0)) continue;
      uint8_t t = G.map[p.r][p.q].terrain;
      if (t >= NUM_TERRAIN) t = 0;
      if (t == 9 || TERRAIN_IS_RUINS[t]) continue;
      uint8_t shelter = G.map[p.r][p.q].shelter;
      if (shelter >= 2) continue;
      float intensity = WEATHER_INTENSITY[WEATHER_FOG][t];
      float mpProb = intensity * FOG_MP_TICK_RATE;
      float llProb = intensity * FOG_LL_TICK_RATE;
      if (shelter == 1) { mpProb *= 0.5f; llProb *= 0.5f; }
      if (p.movesLeft > 0 && esp_random() < (uint32_t)(mpProb * 0xFFFFFFFFul)) {
        p.movesLeft--;
      }
      if (esp_random() < (uint32_t)(llProb * 0xFFFFFFFFul)) {
        if (p.ll > 0) { p.ll--; ledFlash(30, 55, 40); k10Play(MOTIF_CREEPING_RUST); }
        if (p.ll == 0) {
          p.movesLeft = 0;
          GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)pid;
          dev.evWsId = p.wsClientId; enqEvt(dev);
        }
      }
    }
  }

  // ── World system (Caravan/Fire/Creeping Doom) ─────────────────────────────
  // Ticks on its own slower clock — see docs/world-system-spec.md. Still
  // inside G.mutex: entity state (W/W_hex) is read/written the same as
  // G.map/G.players. Order is load-bearing:
  //  - Doom reads tracks BEFORE decayTracks() so single-action heat is still
  //    smellable this tick (spec's tick-order note).
  //  - Fire damage is resolved against THIS tick's fresh intensity before
  //    spreadFire() decays it — otherwise a hex lightning/Doom just set to 2
  //    would already read as 1 (below the damage threshold) by the time
  //    anyone checked it.
  //  - Caravan trade prompt runs last, after the caravan has actually moved
  //    to its new hex.
  if (G.tickId % WORLD_TICK_INTERVAL == 0) {
    tickCreepingDoom();
    decayTracks();
    maybeIgniteLightning();
    resolveFireDamage();
    resolveDoomProximity();
    spreadFire();
    tickCaravan();
    resolveCaravanProximity();
  }

  // ── Quakes ───────────────────────────────────────────────────────────────
  // Checked every tick (like the mock), not gated behind dawnOccurred — an
  // earthquake is an independent hazard, not tied to the weather/day cycle.
  QuakeResult quakeResult = {};
  maybeTriggerQuake(quakeResult);

  xSemaphoreGive(G.mutex);
  if (dawnOccurred) saveGame();  // save outside mutex — SD writes are slow
  if (quakeResult.fired) broadcastQuake(quakeResult);  // WS I/O — outside mutex too
}

// ── §5 Per-action handlers ────────────────────────────────────────────────────
// Each function is called while holding G.mutex.  Writes result into ev.

static void doForage(int pid, uint8_t terr, GameEvent& ev) {
  Player& p  = G.players[pid];
  uint8_t dn = TERRAIN_FORAGE_DN[terr];
  if (!dn || p.movesLeft < 2) return;
  spendMP(p, 2);
  CheckResult cr = resolveCheck(pid, SK_FORAGE, dn, 0);
  ev.actDn  = dn; ev.actTot = (int8_t)cr.total;
  broadcastCheck(pid, SK_FORAGE, cr);
  if (cr.total >= (int)dn - 1) {
    bool partial = (cr.total < (int)dn);
    // Open Scrub + Rust Forest are the rich land hexes → 3 food, others → 2.
    // Keep data/game-data.js TERRAIN desc text in step with these numbers.
    uint8_t yield = (terr == 0 || terr == 2) ? 3 : 2;
    // Compound Bow doubles food yield on land hexes (not the river).
    if (terr != 11 && hasNarrativeParam(pid, NAR_LAND_FORAGE))  yield = (uint8_t)min((int)yield * 2, 99);
    // Fishing Pole doubles yield on River Channel.
    if (terr == 11 && hasNarrativeParam(pid, NAR_RIVER_FORAGE)) yield = (uint8_t)min((int)yield * 2, 99);
    // A partial always comes in strictly under a clean success.
    if (partial) yield = (uint8_t)max(1, (int)yield - 1);
    p.inv[1]    = (uint8_t)min((int)p.inv[1] + yield, 99);
    ev.actFoodD = (int8_t)yield;
    addScore(p, ev, partial ? 1 : 3);
    ev.actOut   = partial ? AO_PARTIAL : AO_SUCCESS;
  } else {
    ev.actOut = AO_FAIL;
  }
}

static void doWater(int pid, uint8_t terr, int mpParam, GameEvent& ev) {
  Player& p = G.players[pid];
  if (!TERRAIN_HAS_WATER[terr] || p.movesLeft < 1) return;
  int spend = max(1, min(3, min(mpParam, (int)p.movesLeft)));
  spendMP(p, spend);
  p.inv[0]     = (uint8_t)min((int)p.inv[0] + spend, 99);
  ev.actWatD   = (int8_t)spend;
  addScore(p, ev, spend);
  ev.actOut    = AO_SUCCESS;
}

static void doScav(int pid, uint8_t terr, GameEvent& ev) {
  Player& p  = G.players[pid];
  uint8_t dn = TERRAIN_SALVAGE_DN[terr];
  if (!dn || p.movesLeft < 2) return;
  spendMP(p, 2);
  if (TERRAIN_IS_RUINS[terr] && G.threatClock < 20) {
    G.threatClock++;
  }
  CheckResult cr = resolveCheck(pid, SK_SCAVENGE, dn, 0);
  ev.actDn = dn; ev.actTot = (int8_t)cr.total;
  broadcastCheck(pid, SK_SCAVENGE, cr);
  if (cr.total >= (int)dn - 1) {
    bool    partial    = (cr.total < (int)dn);
    uint8_t scrapYield = 2;
    // Portable Forge doubles scrap yield on scavenge
    if (hasNarrativeParam(pid, NAR_SCAV_DOUBLE)) scrapYield = 4;
    // A partial always comes in strictly under a clean success.
    if (partial) scrapYield = (uint8_t)max(1, (int)scrapYield / 2);
    p.inv[4]     = (uint8_t)min((int)p.inv[4] + scrapYield, 99);
    ev.actScrapD = scrapYield;
    addScore(p, ev, partial ? 2 : 5);
    ev.actOut    = partial ? AO_PARTIAL : AO_SUCCESS;
  } else {
    ev.actOut = AO_FAIL;
  }
}

// Treat a major wound.  The Medic (archetype 2) may do this anywhere — that is
// the archetype's trait; everyone else must be standing in a Settlement.
// Costs 2 MP + 1 Medicine, and rolls Endure vs TREAT_DN.  On a partial the
// major wound is downgraded to a minor one rather than cleared outright.
static void doTreat(int pid, uint8_t terr, GameEvent& ev) {
  Player& p = G.players[pid];
  bool isMedic      = (p.archetype == 2);
  bool inSettlement = (terr == 9);
  if (!isMedic && !inSettlement) return;   // AO_BLOCKED
  if (p.wounds[WOUND_MAJOR] == 0)    return;
  if (p.inv[3] == 0 || p.movesLeft < 2) return;
  spendMP(p, 2);
  p.inv[3]--;
  ev.actMedD = -1;
  CheckResult cr = resolveCheck(pid, SK_ENDURE, TREAT_DN, 0);
  ev.actDn = TREAT_DN; ev.actTot = (int8_t)cr.total;
  broadcastCheck(pid, SK_ENDURE, cr);
  if (cr.success) {
    healWound(p, WOUND_MAJOR);
    addScore(p, ev, 6);
    ev.actOut = AO_SUCCESS;
  } else if (cr.total >= (int)TREAT_DN - 1) {
    // Partial: the major wound becomes a minor one.
    healWound(p, WOUND_MAJOR);
    addWound(p, WOUND_MINOR, 1);
    addScore(p, ev, 2);
    ev.actOut = AO_PARTIAL;
  } else {
    ev.actOut = AO_FAIL;
  }
  ev.actWndMin = p.wounds[WOUND_MINOR];
  ev.actWndMaj = p.wounds[WOUND_MAJOR];
}

// A settlement can bloom out of collaborative shelter-building — the reverse
// of triggerQuake() leveling a Settlement back to Open Scrub. Two triggers,
// both only checked the moment a shelter finishes as *improved*:
//   (a) 3+ connected players share the hex being built on
//   (b) the new improved shelter completes a triangle of 3 mutually-adjacent
//       improved shelters (a hex and two of its neighbours that are also
//       neighbours of each other)
// Either way every shelter involved is cleared and exactly one of the
// involved hexes becomes Settlement (terrain 9), chosen at random for (b).
// A multi-cell change doesn't fit the fixed-size GameEvent/enqEvt path any
// more than the quake's fault line does, so broadcastSettle() (below) hand-
// rolls its own JSON the same way broadcastQuake() does.
static constexpr int16_t SETTLEMENT_FOUND_BONUS = 20;

struct SettleResult {
  bool    fired;
  uint8_t removedCount;
  int16_t remQ[3], remR[3];   // hexes whose shelter was cleared
  int16_t settleQ, settleR;   // the hex that became Settlement
};

static int countConnectedPlayersOn(int16_t q, int16_t r) {
  int n = 0;
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (G.players[i].connected && G.players[i].q == q && G.players[i].r == r) n++;
  return n;
}

// Tests the two triangles hex (q,r) takes part in against each of its six
// neighbour pairs (d, d+1) — three mutually-adjacent hexes only ever meet at
// such a pair, so this is the complete check, not a heuristic.
static bool findShelterTriangle(int16_t q, int16_t r, int16_t triQ[3], int16_t triR[3]) {
  for (int d = 0; d < 6; d++) {
    int q1 = wrapQ(q + DQ[d]),           r1 = wrapR(r + DR[d]);
    int q2 = wrapQ(q + DQ[(d + 1) % 6]), r2 = wrapR(r + DR[(d + 1) % 6]);
    if (G.map[r1][q1].shelter >= 2 && G.map[r2][q2].shelter >= 2) {
      triQ[0] = q;            triR[0] = r;
      triQ[1] = (int16_t)q1;  triR[1] = (int16_t)r1;
      triQ[2] = (int16_t)q2;  triR[2] = (int16_t)r2;
      return true;
    }
  }
  return false;
}

// Build or upgrade the shelter on the current hex.
//   empty hex : 2+ scrap and 2+ MP → improved (2 scrap, 2 MP, +8);
//               otherwise basic (1 scrap, 1 MP, +4)
//   basic here: upgrade to improved only (2 scrap, 2 MP, +8)
//   improved  : nothing to build — blocked
// A hex is never downgraded and a finished shelter never pays out twice.
// Keep data/ui-panels.js getShelterDesc() in step with these rules.
// settle.fired tells the caller whether to broadcastSettle() — see
// SettleResult above for why this can't just ride on ev.
static void doShelter(int pid, GameEvent& ev, SettleResult& settle) {
  Player&  p       = G.players[pid];
  uint8_t  current = G.map[p.r][p.q].shelter;
  settle.fired = false;
  if (current >= 2) return;                       // AO_BLOCKED: already improved
  if (p.inv[4] == 0) return;                      // AO_BLOCKED: no scrap
  uint8_t shelterType = (p.inv[4] >= 2 && p.movesLeft >= 2) ? 2 : 1;
  if (current == 1 && shelterType < 2) return;    // AO_BLOCKED: can't afford the upgrade
  uint8_t mpCost = shelterType;
  if (p.movesLeft < (int8_t)mpCost) return;
  spendMP(p, mpCost);
  p.inv[4]                 = (uint8_t)max(0, (int)p.inv[4] - shelterType);
  G.map[p.r][p.q].shelter  = shelterType;
  int scoreD = (shelterType == 2) ? 8 : 4;

  if (shelterType == 2) {
    int16_t q = p.q, r = p.r;
    if (countConnectedPlayersOn(q, r) >= 3) {
      G.map[r][q].shelter  = 0;
      G.map[r][q].terrain  = 9;
      settle.fired         = true;
      settle.removedCount  = 1;
      settle.remQ[0] = q; settle.remR[0] = r;
      settle.settleQ = q; settle.settleR = r;
      scoreD += SETTLEMENT_FOUND_BONUS;
    } else {
      int16_t triQ[3], triR[3];
      if (findShelterTriangle(q, r, triQ, triR)) {
        uint8_t pick = (uint8_t)(esp_random() % 3);
        for (uint8_t i = 0; i < 3; i++) G.map[triR[i]][triQ[i]].shelter = 0;
        G.map[triR[pick]][triQ[pick]].terrain = 9;
        settle.fired        = true;
        settle.removedCount = 3;
        for (uint8_t i = 0; i < 3; i++) { settle.remQ[i] = triQ[i]; settle.remR[i] = triR[i]; }
        settle.settleQ = triQ[pick]; settle.settleR = triR[pick];
        scoreD += SETTLEMENT_FOUND_BONUS;
      }
    }
  }

  addScore(p, ev, scoreD);
  ev.actOut    = AO_SUCCESS;
  ev.actCnd    = G.map[p.r][p.q].shelter;  // final truth: may have been cleared above,
                                            // which also keeps this from racing broadcastSettle()
  ev.actScrapD = -(int8_t)shelterType;
}

// Builds and broadcasts a newly-formed settlement's JSON, mirroring
// broadcastQuake(). Call OUTSIDE G.mutex (does WS I/O). Reuses the shelter/
// terrain field meanings the client already knows from the 'quake' handler.
static void broadcastSettle(const SettleResult& s) {
  char buf[256];
  int len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"settle\",\"removed\":[");
  for (uint8_t i = 0; i < s.removedCount; i++)
    len += snprintf(buf + len, sizeof(buf) - len, "%s{\"q\":%d,\"r\":%d}",
                     i ? "," : "", (int)s.remQ[i], (int)s.remR[i]);
  len += snprintf(buf + len, sizeof(buf) - len, "],\"q\":%d,\"r\":%d}",
                   (int)s.settleQ, (int)s.settleR);
  ws.textAll(buf, len);
  Log.notice("EVT settle removed=%d settlement=(%d,%d)",
             (int)s.removedCount, (int)s.settleQ, (int)s.settleR);
  k10LogAdd("A settlement rises!");
}

static void doSurvey(int pid, GameEvent& ev, char* survBuf, int survCap, int* survLen) {
  Player& p      = G.players[pid];
  bool    isScout = (p.archetype == 4);  // Scout: Survey costs 0 MP
  if (p.resting) return;
  if (!isScout && p.movesLeft < 1) return;
  if (!isScout) {
    spendMP(p, 1);
  }
  // Per-hex cap: +2 pts first survey only; repeats still reveal terrain
  int  hexIdx        = (int)p.r * MAP_COLS + (int)p.q;
  bool alreadySurveyed = (p.surveyedMap[hexIdx / 8] >> (hexIdx % 8)) & 1;
  if (!alreadySurveyed) {
    p.surveyedMap[hexIdx / 8] |= (uint8_t)(1 << (hexIdx % 8));
    addScore(p, ev, 2);
  } else {
    ev.actScoreD = 0;  // already surveyed — no pts, still reveals terrain
  }
  ev.actOut = AO_SUCCESS;
  if (survBuf && survLen) {
    int visR; bool mr;
    playerVisParams(pid, &visR, &mr);
    *survLen = buildSurveyDisk(survBuf, survCap, p.q, p.r, visR, pid);
  }
}

// Settle in for the night.  With a Fire Starter equipped, a basic shelter on
// this hex is banked up to an improved one for free (the actCnd field carries
// the new level so the client can redraw the hex).
static void doRest(int pid, GameEvent& ev) {
  Player& p = G.players[pid];
  if (p.resting) return;  // already resting; prevent duplicate REST commands
  p.resting = true;  // mark as resting; if all players rest, day ends early
  HexCell& cell = G.map[p.r][p.q];
  if (cell.shelter == 1 && hasNarrativeParam(pid, NAR_FIRE_STARTER)) {
    cell.shelter = 2;
    ev.actCnd    = 2;
  }
  ev.actOut = AO_SUCCESS;
}

// ── §5 Action dispatcher ──────────────────────────────────────────────────────
// Call while holding G.mutex.  Enqueues EVT_ACTION.
// survBuf/survLen: optional out-param for SURVEY response (send to client directly).
// settleOut: out-param for a settlement founded by this action (only ACT_SHELTER
// can set it — caller zero-inits and broadcastSettle()s after releasing G.mutex).
static void handleAction(int pid, uint8_t actType, int mpParam,
                         char* survBuf, int survCap, int* survLen,
                         SettleResult& settleOut) {
  Player& p    = G.players[pid];
  uint8_t terr = (p.r < MAP_ROWS && p.q < MAP_COLS) ? G.map[p.r][p.q].terrain : 0;
  if (terr >= NUM_TERRAIN) terr = 0;
  if (p.ll == 0) return;  // downed — no actions until respawn
  if (encounters[pid].active)  return;  // locked during active encounter

  GameEvent ev = {};
  ev.type    = EVT_ACTION;
  ev.pid     = (uint8_t)pid;
  ev.actType = actType;
  ev.actOut  = AO_BLOCKED;
  if (survLen) *survLen = 0;

  switch (actType) {
    case ACT_FORAGE:  doForage (pid, terr, ev);                      break;
    case ACT_WATER:   doWater  (pid, terr, mpParam, ev);             break;
    case ACT_TREAT:   doTreat  (pid, terr, ev);                      break;
    case ACT_SCAV:    doScav   (pid, terr, ev);                      break;
    case ACT_SHELTER: doShelter(pid, ev, settleOut);                 break;
    case ACT_SURVEY:  doSurvey (pid, ev, survBuf, survCap, survLen); break;
    case ACT_REST:    doRest   (pid, ev);                            break;
    default: break;
  }

  ev.actNewLL  = p.ll;
  ev.actNewMP  = p.movesLeft;
  enqEvt(ev);
}
