#pragma once
// ── Action handlers and game tick ─────────────────────────────────────────────
// Included from Esp32HexMapCrawl.ino after survival_state.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

// ── Weather phase state machine ───────────────────────────────────────────────
// Called each tick while holding G.mutex.
static void updateWeatherPhase() {
  if (G.tickId % WEATHER_TICK_DIVIDER != 0) return;  // advance only every N ticks

  // Accumulate bad-weather streak (non-CLEAR ticks)
  if (G.weatherPhase != WEATHER_CLEAR) G.badWeatherTicks++;

  if (G.weatherCounter > 0) { G.weatherCounter--; return; }

  static constexpr uint16_t BAD_WEATHER_CAP = 180; // 3 days × 60 weather-ticks/day
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
  Serial.printf("[WEATHER] Phase→%d counter→%d badTicks→%d\n",
                (int)next, (int)G.weatherCounter, (int)G.badWeatherTicks);
}

// ── Game tick (Core 1) ────────────────────────────────────────────────────────
static void tickGame() {
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  G.tickId++;
  updateWeatherPhase();

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
    bool earlyDawn = (connCount > 0 && allResting && G.dayTick < DAY_TICKS);
    uint32_t savedTick = G.dayTick;
    G.dayTick = 0;
    G.dayCount++;
    dawnOccurred = true;
    if (earlyDawn) {
      Serial.printf("[DUSK]    ──── Day %d ends EARLY (all %d resting) | tick %lu/%lu (%.0f%%, %lus saved) ────\n",
        (int)G.dayCount - 1, connCount,
        (unsigned long)savedTick, (unsigned long)DAY_TICKS,
        100.0f * savedTick / DAY_TICKS,
        (unsigned long)((DAY_TICKS - savedTick) * TICK_MS / 1000));
    } else {
      Serial.printf("[DUSK]    ──── Day %d ends (natural) | tick %lu/%lu ────\n",
        (int)G.dayCount - 1, (unsigned long)savedTick, (unsigned long)DAY_TICKS);
    }
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
      Serial.printf("[ENC]     P%d encounter force-aborted (dawn)\n", i);
    }
    duskCheck();    // end-of-day radiation Endure checks (R ≥ 7); enqueues EVT_DUSK
    Serial.printf("[DAWN]    ──── Day %d begins | cycle %lus/day (%lu ticks @ %lums) ────\n",
      (int)G.dayCount,
      (unsigned long)(DAY_TICKS * TICK_MS / 1000),
      (unsigned long)DAY_TICKS, (unsigned long)TICK_MS);
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
          Serial.printf("[RESPAWN] (%2d,%2d) %s | %dx%s | tick:%lu\n",
            c, r, T_NAME[cell.terrain],
            cell.amount, RES_NAME[cell.resource],
            (unsigned long)G.tickId);
        }
      }
    }
  }

  // ── Chem-storm per-tick hazard ────────────────────────────────────────────
  if (G.weatherPhase == WEATHER_CHEM) {
    for (int pid = 0; pid < MAX_PLAYERS; pid++) {
      Player& p = G.players[pid];
      if (!p.connected || (p.statusBits & ST_DOWNED)) continue;
      uint8_t t = G.map[p.r][p.q].terrain;
      if (t >= NUM_TERRAIN) t = 0;
      // Settlement (t==9) and Ruins terrain: fully immune to all weather hazards
      if (t == 9 || TERRAIN_IS_RUINS[t]) continue;
      // Improved shelter (level 2): immune to all weather; basic shelter (level 1): no immunity
      if (G.map[p.r][p.q].shelter >= 2) continue;
      float prob = WEATHER_INTENSITY[WEATHER_CHEM][t] * 0.016f;
      if (esp_random() < (uint32_t)(prob * 0xFFFFFFFFul)) {
        if (p.ll > 0) { p.ll--; ledFlash(0, 100, 0); }
        if (p.ll == 0) {
          p.statusBits |= ST_DOWNED; p.movesLeft = 0;
          GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)pid;
          dev.evWsId = p.wsClientId; enqEvt(dev);
        }
        Serial.printf("[CHEM]    P%d \"%s\" chem tick LL→%d\n", pid, p.name, (int)p.ll);
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
    if (p.fatigue < 8) p.fatigue++;
    addScore(p, ev, 1);
    ev.actOut   = AO_PARTIAL;
  } else {
    if (p.fatigue < 8) p.fatigue++;
    if (terr == 2 && p.wounds[0] < 10) {  // Rust Forest fail → Minor wound
      p.wounds[0]++;
      Serial.printf("[WOUND]   P%d \"%s\" Minor wound from Rust Forest (wounds[0]=%d)\n", pid, p.name, p.wounds[0]);
    }
    ev.actOut = AO_FAIL;
  }
  Serial.printf("[FORAGE]  P%d \"%s\" @ %s | DN%d tot:%d → %s | inv food:%d fat:%d\n",
    pid, p.name, T_NAME[terr], dn, cr.total,
    ev.actOut == AO_SUCCESS ? "SUCCESS" : ev.actOut == AO_PARTIAL ? "PARTIAL" : "FAIL",
    p.inv[1], p.fatigue);
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
  Serial.printf("[WATER]   P%d \"%s\" @ %s | +%d water | inv water:%d mp→%d\n",
    pid, p.name, T_NAME[terr], spend, p.inv[0], p.movesLeft);
}

static void doScav(int pid, uint8_t terr, GameEvent& ev) {
  Player& p  = G.players[pid];
  uint8_t dn = TERRAIN_SALVAGE_DN[terr];
  if (!dn || p.movesLeft < 2) return;
  spendMP(p, 2);
  if (TERRAIN_IS_RUINS[terr] && G.threatClock < 20) {
    G.threatClock++;
    Serial.printf("[TC]      Threat Clock → %d (Scavenge in Ruins)\n", G.threatClock);
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
    if (terr == 6 && !(p.statusBits & ST_BLEEDING)) {  // Glass Fields fail → Bleeding
      p.statusBits |= ST_BLEEDING;
      Serial.printf("[WOUND]   P%d \"%s\" Bleeding from Glass Fields shards\n", pid, p.name);
    }
    ev.actOut = AO_FAIL;
  }
  Serial.printf("[SCAV]    P%d \"%s\" @ %s | DN%d tot:%d → %s | scrap:%d\n",
    pid, p.name, T_NAME[terr], dn, cr.total,
    ev.actOut == AO_SUCCESS ? "SUCCESS" : ev.actOut == AO_PARTIAL ? "PARTIAL" : "FAIL",
    p.inv[4]);
}

static void doShelter(int pid, GameEvent& ev) {
  Player& p = G.players[pid];
  if (p.inv[4] == 0) {
    Serial.printf("[SHELTER] ✗ %s (P%d) no scrap\n", p.name, pid);
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
  Serial.printf("[SHELTER] ✓ %s (P%d) built %s shelter @ (%d,%d) | -%d MP | scrap→%d\n",
    p.name, pid, shelterType == 2 ? "improved shelter" : "shelter",
    p.q, p.r, mpCost, (int)p.inv[4]);
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
  Serial.printf("[SURVEY]  P%d \"%s\" @ (%d,%d) surveyed extended ring\n",
    pid, p.name, p.q, p.r);
}

static void doRest(int pid, uint8_t terr, GameEvent& ev) {
  Player& p = G.players[pid];
  if (p.resting) return;  // already resting; prevent duplicate REST commands
  p.actUsed = true;
  p.resting = true;  // mark as resting; if all players rest, day ends early
  uint8_t hexShelt = G.map[p.r][p.q].shelter;  // 0=none, 1=shelter, 2=improved shelter
  // Marsh exposure: resting without shelter risks infection → Fever
  if (terr == 3 && hexShelt == 0 && !(p.statusBits & ST_FEVERED)) {
    p.statusBits |= ST_FEVERED;
    Serial.printf("[WOUND]   P%d \"%s\" Fever from sleeping in Marsh (no shelter)\n", pid, p.name);
  }
  // Camp bonus: any other player in the same hex
  int campCount = 0;
  for (int k = 0; k < MAX_PLAYERS; k++)
    if (k != pid && G.players[k].connected &&
        G.players[k].q == p.q && G.players[k].r == p.r) campCount++;
  // Fatigue reduction: base 2 | +1 camp | +1 shelter | +2 improved shelter
  int fatRed = 2;
  if (campCount >= 1)     fatRed += 1;
  if (hexShelt == 1)      fatRed += 1;
  else if (hexShelt == 2) fatRed += 2;
  uint8_t prevFat = p.fatigue;
  p.fatigue = (uint8_t)max(0, (int)p.fatigue - fatRed);
  // LL restore: Food≥4 AND Water≥3 AND fatigue low enough
  // Improved shelter relaxes the fatigue threshold (fat<6 instead of fat<4)
  bool llOk  = (p.food >= 4 && p.water >= 3);
  bool fatOk = (p.fatigue < 4) || (hexShelt == 2 && p.fatigue < 6);
  if (llOk && fatOk && p.ll < (uint8_t)effectiveMaxLL(pid)) {
    p.ll++;
    ev.actLLD = 1;
  }
  // Resolve gain (§7.3): SV≥3 | SV≥2+any shelter | SV≥1+improved shelter
  {
    uint8_t rHexT = G.map[p.r][p.q].terrain < NUM_TERRAIN ? G.map[p.r][p.q].terrain : 0;
    uint8_t sv    = TERRAIN_SV[rHexT];
    bool goodShelter = (sv >= 3) || (sv >= 2 && hexShelt > 0) || (sv >= 1 && hexShelt == 2);
    if (goodShelter && p.resolve < 5) {
      p.resolve++;
      ev.actResD = 1;
      Serial.printf("[REST]    P%d \"%s\" +1 Resolve (SV%d shelt:%d) → %d\n",
        pid, p.name, sv, hexShelt, p.resolve);
    }
  }
  ev.actOut = AO_SUCCESS;
  uint32_t ticksLeft = (G.dayTick < DAY_TICKS) ? (DAY_TICKS - G.dayTick) : 0;
  // Count how many connected players are now resting (including this one)
  int restCount = 0, totalConn = 0;
  for (int k = 0; k < MAX_PLAYERS; k++) {
    if (G.players[k].connected) { totalConn++; if (G.players[k].resting) restCount++; }
  }
  Serial.printf("[REST]    P%d \"%s\" fat %d→%d (red:%d shelt:%d camp:%d) | F:%d W:%d LL:%d%s res:%d"
                " | tick %lu/%lu ~%lus remain | resting %d/%d\n",
    pid, p.name, prevFat, p.fatigue, fatRed, hexShelt, campCount,
    p.food, p.water, p.ll, ev.actLLD ? " +1LL" : "", p.resolve,
    (unsigned long)G.dayTick, (unsigned long)DAY_TICKS,
    (unsigned long)(ticksLeft * TICK_MS / 1000),
    restCount, totalConn);
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
  if (p.statusBits & ST_DOWNED) return;  // downed — no actions until respawn
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
  ev.actNewFat = p.fatigue;
  ev.actNewMP  = p.movesLeft;
  enqEvt(ev);
}
