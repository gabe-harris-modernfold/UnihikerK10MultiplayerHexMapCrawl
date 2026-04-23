#pragma once
// ── Action handlers and game tick ─────────────────────────────────────────────
// Included from Esp32HexMapCrawl.ino after survival_state.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

// ── Weather phase state machine ───────────────────────────────────────────────
// Called once per dawn (not per tick) so resting through days advances weather normally.
static void updateWeatherPhase() {
  // Accumulate bad-weather streak in game-days
  if (G.weatherPhase != WEATHER_CLEAR) G.badWeatherTicks++;

  if (G.weatherCounter > 0) { G.weatherCounter--; return; }

  static constexpr uint16_t BAD_WEATHER_CAP = 6; // max 6 game-days of bad weather
  uint8_t next;
  if (G.badWeatherTicks >= BAD_WEATHER_CAP) {
    next = WEATHER_CLEAR;  // force clear after 3-day bad-weather streak
  } else {
    uint32_t roll = esp_random() % 100;
    next = G.weatherPhase;
    switch (G.weatherPhase) {
      case WEATHER_CLEAR: next = (roll < 70) ? WEATHER_RAIN  : WEATHER_STORM; break;
      case WEATHER_RAIN:  next = (roll < 50) ? WEATHER_STORM : WEATHER_CLEAR; break;
      case WEATHER_STORM:
        next = (roll < 30) ? WEATHER_CHEM : (roll < 65) ? WEATHER_RAIN : WEATHER_CLEAR; break;
      case WEATHER_CHEM:  next = (roll < 20) ? WEATHER_CLEAR : (roll < 60) ? WEATHER_STORM : WEATHER_RAIN; break;
    }
  }

  if (next == WEATHER_CLEAR) G.badWeatherTicks = 0;

  G.weatherCounter = WEATHER_DUR_MIN[next] +
    (uint16_t)(esp_random() % (WEATHER_DUR_MAX[next] - WEATHER_DUR_MIN[next] + 1));
  G.weatherPhase = next;
  GameEvent ev = {}; ev.type = EVT_WEATHER;
  ev.q = (int16_t)next; ev.r = (int16_t)G.weatherCounter;
  enqEvt(ev);
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
    // Force-abort any active encounters before dusk (no TC increment for dawn abort)
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!encounters[i].active) continue;
      uint8_t hq = encounters[i].hexQ;
      uint8_t hr = encounters[i].hexR;
      encounters[i] = {};
      GameEvent eev = {}; eev.type = EVT_ENC_END; eev.pid = (uint8_t)i;
      eev.q = (int16_t)hq; eev.r = (int16_t)hr;
      eev.encOut = 2;  // reason: dawn
      enqEvt(eev);
    }
    duskCheck();    // end-of-day radiation Endure checks (R ≥ 7); enqueues EVT_DUSK
    updateWeatherPhase();  // advance weather once per game-day
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
  if (G.weatherPhase == WEATHER_CHEM) {
    for (int pid = 0; pid < MAX_PLAYERS; pid++) {
      Player& p = G.players[pid];
      if (!p.connected || (p.ll == 0)) continue;
      uint8_t t = G.map[p.r][p.q].terrain;
      if (t >= NUM_TERRAIN) t = 0;
      // Settlement (t==9) and Ruins terrain: fully immune to all weather hazards
      if (t == 9 || TERRAIN_IS_RUINS[t]) continue;
      // Improved shelter (level 2): immune to all weather; basic shelter (level 1): no immunity
      if (G.map[p.r][p.q].shelter >= 2) continue;
      float prob = WEATHER_INTENSITY[WEATHER_CHEM][t] * 0.016f;
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

  xSemaphoreGive(G.mutex);
  if (dawnOccurred) saveGame();  // save outside mutex — SD writes are slow
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
  if (cr.total >= (int)dn) {
    uint8_t yield = (terr == 0 || terr == 2) ? 3 : 2;  // Open Scrub + Rust Forest → 3 food, others → 2
    // Compound Bow (id:29) doubles food yield on land hexes
    if (p.equip[EQUIP_HAND - 1] == 29) yield = (uint8_t)min((int)yield * 2, 99);
    // Fishing Pole (id:28) doubles yield on River Channel (terr==11)
    if (p.equip[EQUIP_HAND - 1] == 28 && terr == 11) yield = (uint8_t)min((int)yield * 2, 99);
    p.inv[1]    = (uint8_t)min((int)p.inv[1] + yield, 99);
    ev.actFoodD = (int8_t)yield;
    addScore(p, ev, 3);
    ev.actOut   = AO_SUCCESS;
  } else if (cr.total >= (int)dn - 1) {
    p.inv[1]    = (uint8_t)min((int)p.inv[1] + 2, 99);
    ev.actFoodD = 2;
    addScore(p, ev, 1);
    ev.actOut   = AO_PARTIAL;
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
  if (cr.total >= (int)dn) {
    uint8_t scrapYield = 2;
    // Portable Forge (id:27) doubles scrap yield on scavenge
    if (p.equip[EQUIP_HAND - 1] == 27) scrapYield = 4;
    p.inv[4]     = (uint8_t)min((int)p.inv[4] + scrapYield, 99);
    ev.actScrapD = scrapYield;
    addScore(p, ev, 5);
    ev.actOut    = AO_SUCCESS;
  } else if (cr.total >= (int)dn - 1) {
    // Partial: item + Encounter (Encounter not yet implemented)
    uint8_t scrapYield = 2;
    if (p.equip[EQUIP_HAND - 1] == 27) scrapYield = 4;
    p.inv[4]     = (uint8_t)min((int)p.inv[4] + scrapYield, 99);
    ev.actScrapD = scrapYield;
    addScore(p, ev, 2);
    ev.actOut    = AO_PARTIAL;
  } else {
    ev.actOut = AO_FAIL;
  }
}

static void doShelter(int pid, GameEvent& ev) {
  Player& p = G.players[pid];
  if (p.inv[4] == 0) {
    return;  // ev.actOut stays AO_BLOCKED — UI responds accordingly
  }
  // Auto-select type: 2+ scrap + 2+ MP = improved shelter (2 MP), otherwise basic shelter (1 MP)
  uint8_t shelterType = (p.inv[4] >= 2 && p.movesLeft >= 2) ? 2 : 1;
  uint8_t mpCost      = shelterType;
  if (p.movesLeft < (int8_t)mpCost) return;
  spendMP(p, mpCost);
  p.inv[4]                 = (uint8_t)max(0, (int)p.inv[4] - shelterType);
  G.map[p.r][p.q].shelter  = shelterType;
  addScore(p, ev, (shelterType == 2) ? 8 : 4);
  ev.actOut    = AO_SUCCESS;
  ev.actCnd    = shelterType;           // 1=basic, 2=improved (reuses cnd field)
  ev.actScrapD = -(int8_t)shelterType;
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

static void doRest(int pid, uint8_t terr, GameEvent& ev) {
  Player& p = G.players[pid];
  if (p.resting) return;  // already resting; prevent duplicate REST commands
  p.actUsed = true;
  p.resting = true;  // mark as resting; if all players rest, day ends early
  [[maybe_unused]] uint8_t hexShelt = G.map[p.r][p.q].shelter;  // 0=none, 1=shelter, 2=improved shelter
  ev.actOut = AO_SUCCESS;
  [[maybe_unused]] uint32_t ticksLeft = (G.dayTick < DAY_TICKS) ? (DAY_TICKS - G.dayTick) : 0;
  // Count how many connected players are now resting (including this one)
  int restCount = 0, totalConn = 0;
  for (int k = 0; k < MAX_PLAYERS; k++) {
    if (G.players[k].connected) { totalConn++; if (G.players[k].resting) restCount++; }
  }
}

// ── §5 Action dispatcher ──────────────────────────────────────────────────────
// Call while holding G.mutex.  Enqueues EVT_ACTION.
// survBuf/survLen: optional out-param for SURVEY response (send to client directly).
// condTgt: unused (retained for call-site compatibility; Treat action removed).
static void handleAction(int pid, uint8_t actType, int mpParam, int condTgt,
                         char* survBuf, int survCap, int* survLen) {
  (void)condTgt;
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
    case ACT_SCAV:    doScav   (pid, terr, ev);                      break;
    case ACT_SHELTER: doShelter(pid, ev);                            break;
    case ACT_SURVEY:  doSurvey (pid, ev, survBuf, survCap, survLen); break;
    case ACT_REST:    doRest   (pid, terr, ev);                      break;
    default: break;
  }

  ev.actNewLL  = p.ll;
  ev.actNewMP  = p.movesLeft;
  enqEvt(ev);
}
