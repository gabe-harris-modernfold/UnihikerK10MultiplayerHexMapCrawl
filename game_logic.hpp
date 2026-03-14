#pragma once
// ── Game logic: skill checks, resource economy, movement, actions ─────────────
// Included from Esp32HexMapCrawl.ino after all types, constants, and globals.
// Include order: game_logic.hpp first, then network.hpp, then ino tail functions.

// broadcastCheck is defined in network.hpp (included after this file).
static void broadcastCheck(int pid, uint8_t skill, CheckResult& r, bool pushed);
// ledFlash is defined in Esp32HexMapCrawl.ino (Core 1 safe — sets LED + endMs timer).
void ledFlash(uint8_t r, uint8_t g, uint8_t b);

// ── §6.2 Radiation status sync ───────────────────────────────────────────────
// Sync ST_RADSICK to current radiation level (R ≥ 4 = Rad-Sick).
static void updateRadStatus(Player& p) {
  if (p.radiation >= 4) p.statusBits |=  ST_RADSICK;
  else                  p.statusBits &= ~ST_RADSICK;
}

// ── Skill check resolution ────────────────────────────────────────────────────
// Call while holding G.mutex (reads player skills/status).
// bonus = item bonus from client (0–2). Condition penalties applied automatically.
static CheckResult resolveCheck(int pid, uint8_t skill, uint8_t dn, uint8_t bonus) {
  uint32_t rnd  = esp_random();
  CheckResult r;
  r.r1       = 1 + (int)(rnd         % 6);
  r.r2       = 1 + (int)((rnd >> 8)  % 6);
  r.dn       = (int)dn;
  r.skillVal = (int)G.players[pid].skills[skill < NUM_SKILLS ? skill : 0];

  // Net modifier: item bonus + condition penalties (§3.1 Step 4)
  int cm = (int)bonus;
  uint8_t sb = G.players[pid].statusBits;
  if (sb & ST_RADSICK)  cm -= 1;                           // Rad-Sick: −1 all
  if (sb & ST_FEVERED)  cm -= 1;                           // Fevered:  −1 all
  if (G.players[pid].wounds[1] > 0)                cm -= 1; // Major wound: −1 all
  if (skill == SK_ENDURE && G.players[pid].wounds[0] > 0) cm -= 1; // Minor: −1 Endure only
  r.mods   = cm;
  r.total  = r.r1 + r.r2 + r.skillVal + r.mods;
  r.success = (r.total >= r.dn);
  return r;
}

// ── §4 Resource economy helpers ───────────────────────────────────────────────

// Move F track by dir (+1 or -1).  Checks threshold crossings and accumulates
// llDelta (+1 = restore LL, -1 = lose LL).  F clamps [1,6].
// F thresholds: box 4 (fThreshBelow bit0), box 2 (fThreshBelow bit1).
static void applyFStep(Player& p, int dir, int& llDelta) {
  uint8_t oldF = p.food;
  if (dir > 0) {
    if (p.food < 6) p.food++;
    // Upward crossings → restore LL if threshold was previously tripped
    if (oldF < 4 && p.food >= 4 && (p.fThreshBelow & 1)) { p.fThreshBelow &= ~1; llDelta++; }
    if (oldF < 2 && p.food >= 2 && (p.fThreshBelow & 2)) { p.fThreshBelow &= ~2; llDelta++; }
  } else {
    if (p.food > 1) p.food--;
    // Downward crossings → LL loss on first crossing below each threshold
    if (oldF >= 4 && p.food < 4 && !(p.fThreshBelow & 1)) { p.fThreshBelow |= 1; llDelta--; }
    if (oldF >= 2 && p.food < 2 && !(p.fThreshBelow & 2)) { p.fThreshBelow |= 2; llDelta--; }
  }
}

// Move W track by dir (+1 or -1).  Checks threshold crossings.  W clamps [1,6].
// W thresholds: box 5 (bit0), box 3 (bit1), box 1 (bit2 — fires when W==1 and dir==-1).
static void applyWStep(Player& p, int dir, int& llDelta) {
  uint8_t oldW = p.water;
  if (dir > 0) {
    if (p.water < 6) p.water++;
    if (oldW < 5 && p.water >= 5 && (p.wThreshBelow & 1)) { p.wThreshBelow &= ~1; llDelta++; }
    if (oldW < 3 && p.water >= 3 && (p.wThreshBelow & 2)) { p.wThreshBelow &= ~2; llDelta++; }
    // bit2 (floor penalty): clear when W rises from 1 → 2; oldW<1 was dead code since W floors at 1
    if (oldW < 2 && p.water >= 2 && (p.wThreshBelow & 4)) { p.wThreshBelow &= ~4; llDelta++; }
  } else {
    if (p.water > 1) {
      p.water--;
      if (oldW >= 5 && p.water < 5 && !(p.wThreshBelow & 1)) { p.wThreshBelow |= 1; llDelta--; }
      if (oldW >= 3 && p.water < 3 && !(p.wThreshBelow & 2)) { p.wThreshBelow |= 2; llDelta--; }
    } else {
      // W is already at floor 1; "crossing below box 1" — only fires once
      if (!(p.wThreshBelow & 4)) { p.wThreshBelow |= 4; llDelta--; }
    }
  }
}

// Effective Movement Points = LL − major-wound penalty (max 2) − encumbrance penalty.
// Called while holding G.mutex.
static int effectiveMP(int pid) {
  Player& p  = G.players[pid];
  int     mp = (int)p.ll;
  mp -= min((int)p.wounds[1], 2);          // major wounds: −1 each, max −2
  int used = 0;
  for (int k = 0; k < 5; k++) used += (int)p.inv[k];
  if (used > (int)p.invSlots) mp--;        // encumbrance
  return max(0, mp);
}

// ── §6.2 End-of-day radiation Endure check ───────────────────────────────────
// End-of-day Endure check for R ≥ 7 (§6.2 Dusk Check).
// Called from tickGame() just before dawnUpkeep() each day transition.
// Mutates player state and enqueues EVT_DUSK.  Must hold G.mutex.
static void duskCheck() {
  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    Player& p = G.players[pid];
    if (!p.connected || p.radiation < 7) continue;

    GameEvent ev = {};
    ev.type  = EVT_DUSK;
    ev.pid   = (uint8_t)pid;
    ev.radR  = p.radiation;
    ev.radD  = 0;

    if (p.radiation >= 10) {
      // R 10: auto-fail, do not roll (§6.2)
      ev.actDn  = 8;
      ev.actTot = 0;
      ev.actOut = AO_FAIL;
      Serial.printf("[DUSK]    P%d \"%s\" R:10 auto-FAIL Endure\n", pid, p.name);
    } else {
      CheckResult cr = resolveCheck(pid, SK_ENDURE, 8, 0);
      ev.actDn  = 8;
      ev.actTot = (int8_t)cr.total;
      ev.actOut = cr.success ? AO_SUCCESS : AO_FAIL;
      Serial.printf("[DUSK]    P%d \"%s\" R:%d Endure DN8=%d → %s\n",
        pid, p.name, p.radiation, cr.total, cr.success ? "PASS" : "FAIL");
    }

    if (ev.actOut == AO_FAIL) {
      p.wounds[1]++;                  // +1 Major Wound
      if (p.ll > 0) { p.ll--; ledFlash(255, 0, 0); }  // red = LL lost
      if (p.ll == 0) p.statusBits |= ST_DOWNED;
      ev.actLLD   = -1;
      ev.actNewLL = p.ll;
    } else {
      ev.actLLD   = 0;
      ev.actNewLL = p.ll;
    }
    enqEvt(ev);
  }
}

// ── Dawn upkeep for all connected players (§4.1, §4.2, §4.5) ────────────────
// Called from tickGame() while holding G.mutex.
static void dawnUpkeep() {
  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    Player& p = G.players[pid];
    if (!p.connected) continue;

    int llDelta = 0;

    // ── Clean-zone R recovery (§6.2): no rad hex entered all day → R−1 ─────
    if (p.radClean && p.radiation > 0) {
      p.radiation--;
      updateRadStatus(p);
      Serial.printf("[DAWN]    P%d \"%s\" clean zone R→%d\n", pid, p.name, p.radiation);
    }
    p.radClean = true;  // reset for new day

    // ── Food (§4.1): consume 1 token; F track +1; else F track -1 ─────────
    if (p.inv[1] > 0) {
      p.inv[1]--;
      applyFStep(p, +1, llDelta);
    } else {
      applyFStep(p, -1, llDelta);
    }

    // ── Water (§4.2): consume 2 tokens ───────────────────────────────────────
    {
      int have = (int)p.inv[0];
      int use  = min(have, 2);
      int miss = 2 - use;
      p.inv[0] -= (uint8_t)use;
      for (int i = 0; i < use;  i++) applyWStep(p, +1, llDelta);
      for (int i = 0; i < miss; i++) applyWStep(p, -1, llDelta);
    }

    // ── Apply LL delta (§4.5): losses first (F→W order), then gains ────────
    // Losses were accumulated first in llDelta (both F and W steps above).
    // Apply each step individually so we stop at 0 immediately.
    uint8_t prevLL = p.ll;  // snapshot before changes to compute actual delta
    if (llDelta < 0) {
      for (int i = 0; i < -llDelta; i++) {
        if (p.ll > 0) p.ll--;
        if (p.ll == 0) { p.statusBits |= ST_DOWNED; break; }
      }
    } else if (llDelta > 0) {
      p.ll = (uint8_t)min((int)p.ll + llDelta, 6);
    }
    int8_t actualDelta = (int8_t)((int)p.ll - (int)prevLL); // true LL change (clamped)
    if (actualDelta < 0) ledFlash(255, 0, 0);  // red = Life Level lost

    // ── Reset daily move budget and action flags ────────────────────────────
    p.movesLeft    = (int8_t)effectiveMP(pid);
    p.actUsed      = false;
    p.encPenApplied = false;
    p.resting      = false;

    Serial.printf("[DAWN]    P%d \"%s\" Day:%d | F:%d W:%d LL:%d%+d | R:%d | MP:%d | inv F:%d W:%d\n",
      pid, p.name, (int)G.dayCount,
      (int)p.food, (int)p.water, (int)p.ll, (int)actualDelta,
      (int)p.radiation, (int)p.movesLeft, p.inv[1], p.inv[0]);

    // Enqueue event for broadcast (includes threshold bitmasks for client rendering)
    GameEvent ev = {};
    ev.type        = EVT_DAWN;
    ev.pid         = (uint8_t)pid;
    ev.dawnF       = p.food;
    ev.dawnW       = p.water;
    ev.dawnLL      = p.ll;
    ev.dawnMP      = p.movesLeft;
    ev.dawnLLDelta = actualDelta;
    ev.dawnDay     = G.dayCount;
    ev.dawnFth     = p.fThreshBelow;
    ev.dawnWth     = p.wThreshBelow;
    ev.radR        = p.radiation;
    ev.dawnFat     = p.fatigue;
    enqEvt(ev);
  }
}

// ── Resource collection ───────────────────────────────────────────────────────
static void collectResource(int pid, int q, int r) {
  HexCell& cell = G.map[r][q];
  if (cell.resource == 0 || cell.amount == 0) return;
  Player&  p    = G.players[pid];
  uint8_t  idx  = cell.resource - 1;
  uint8_t  gain = cell.amount;

  // Enforce total-carry cap (invSlots) — count all items across all types
  int totalInv = 0;
  for (int k = 0; k < 5; k++) totalInv += (int)p.inv[k];
  if (totalInv >= (int)p.invSlots) {
    Serial.printf("[COLLECT] P%d \"%s\" (%2d,%2d) FULL (%d/%d) — %s skipped\n",
      pid, p.name, q, r, totalInv, (int)p.invSlots, RES_NAME[cell.resource]);
    return;  // inventory full, cannot collect
  }
  // Collect only as many as there is room for
  int room = (int)p.invSlots - totalInv;
  gain = (uint8_t)min((int)gain, room);
  if (gain == 0) return;

  p.inv[idx] = (uint8_t)min((int)p.inv[idx] + gain, 99);
  p.score   += gain * 10;
  { GameEvent ev = {}; ev.type = EVT_COLLECT; ev.pid = (uint8_t)pid;
    ev.q = (int16_t)q; ev.r = (int16_t)r; ev.res = cell.resource; ev.amt = gain;
    enqEvt(ev); }

  Serial.printf("[COLLECT] P%d \"%s\" (%2d,%2d) +%dx%s | inv W:%d F:%d Fu:%d M:%d S:%d | score:%d\n",
    pid, p.name, q, r, gain, RES_NAME[cell.resource],
    p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4], p.score);

  cell.amount = 0; cell.resource = 0;
  cell.respawnTimer = RESPAWN_TICKS;

  // ── Encumbrance check (§5): if inv > invSlots, deduct 1 MP once per day ─
  int totalInvAfter = 0;
  for (int k = 0; k < 5; k++) totalInvAfter += (int)p.inv[k];
  if (totalInvAfter > (int)p.invSlots && !p.encPenApplied) {
    p.encPenApplied = true;
    if (p.movesLeft > 0) p.movesLeft--;
    Serial.printf("[ENC]     P%d \"%s\" inv %d/%d — encumbrance −1 MP → mp:%d\n",
      pid, p.name, totalInvAfter, (int)p.invSlots, (int)p.movesLeft);
  }
}

// ── Player move ───────────────────────────────────────────────────────────────
static void movePlayer(int pid, int dir) {
  if (dir < 0 || dir > 5) return;
  Player& p  = G.players[pid];
  if (p.resting) {
    Serial.printf("[BLOCKED] P%d \"%s\" resting — waiting for other survivors to finish\n", pid, p.name);
    return;
  }
  int     nq = wrapQ(p.q + DQ[dir]);
  int     nr = wrapR(p.r + DR[dir]);

  uint8_t destTerrain = G.map[nr][nq].terrain;
  uint8_t mc          = TERRAIN_MC[destTerrain];
  if (mc == 255) {
    Serial.printf("[BLOCKED] P%d \"%s\" dir:%s → (%2d,%2d) %s (impassable)\n",
      pid, p.name, DIR_NAME[dir], nq, nr, T_NAME[destTerrain]);
    return;
  }

  // ── MP budget check (§4.5 hard daily cap) ──────────────────────────────
  if (p.movesLeft == 0) {
    Serial.printf("[EXHAUST] P%d \"%s\" → (%2d,%2d) MP=0 (LL:%d) — wait for Dawn\n",
      pid, p.name, nq, nr, (int)p.ll);
    return;
  }

  uint32_t cd  = (uint32_t)MOVE_CD_MS * mc;
  uint32_t now = millis();
  if (now - p.lastMoveMs < cd) return;  // cooldown — silent, normal behaviour
  p.lastMoveMs = now;

  int16_t oldQ = p.q, oldR = p.r;
  p.q = (int16_t)nq;
  p.r = (int16_t)nr;
  p.steps++;

  // Mark footprint at new hex (visible to all players)
  G.map[p.r][p.q].footprints |= (1 << pid);

  // Compute vision params at new position for the debug line
  int8_t  visLvl  = (destTerrain < NUM_TERRAIN) ? TERRAIN_VIS[destTerrain] : 0;
  int     effVisR = (visLvl <= -3) ? 0 : (visLvl == -2) ? 1 : (visLvl == -1) ? 2 : (visLvl == 0) ? VISION_R : (visLvl == 1) ? VISION_R + 1 : VISION_R + 2;
  // Deduct movement cost from daily MP budget
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - (int)mc);

  Serial.printf("[MOVE]    P%d \"%s\" (%2d,%2d)→(%2d,%2d) %s [%s] visR:%d%s cd:%lums mp:%d\n",
    pid, p.name, oldQ, oldR, p.q, p.r,
    T_NAME[destTerrain], VIS_LABEL[visLvl + 3],
    effVisR, (visLvl == -2) ? " mask" : "     ",
    (unsigned long)cd, (int)p.movesLeft);

  // ── Radiation entry check (§6.2): Rad-tagged terrain → Endure DN6 or +1 R ──
  int8_t radGain = 0;
  if (destTerrain < NUM_TERRAIN && TERRAIN_IS_RAD[destTerrain]) {
    p.radClean = false;   // day is no longer clean
    if (p.radiation < 10) {
      CheckResult cr = resolveCheck(pid, SK_ENDURE, 6, 0);
      if (!cr.success) {
        p.radiation++;
        radGain = 1;
        updateRadStatus(p);
        ledFlash(0, 255, 0);  // green = radiation gained
        Serial.printf("[RAD]     P%d \"%s\" +1 R (now %d) Endure DN6=%d FAIL\n",
          pid, p.name, p.radiation, cr.total);
      } else {
        Serial.printf("[RAD]     P%d \"%s\" R:%d Endure DN6=%d PASS\n",
          pid, p.name, p.radiation, cr.total);
      }
    }
  }
  { GameEvent ev = {}; ev.type = EVT_MOVE; ev.pid = (uint8_t)pid;
    ev.q = p.q; ev.r = p.r;
    ev.radD = radGain; ev.radR = p.radiation;
    enqEvt(ev); }
  collectResource(pid, p.q, p.r);
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

  if (G.dayTick >= DAY_TICKS || (connCount > 0 && allResting)) {
    G.dayTick = 0;
    G.dayCount++;
    Serial.printf("[DUSK]    ──── Dusk (end of Day %d) ────\n", (int)G.dayCount - 1);
    duskCheck();    // end-of-day radiation Endure checks (R ≥ 7); enqueues EVT_DUSK
    Serial.printf("[DAWN]    ──── Day %d begins ────\n", (int)G.dayCount);
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
  xSemaphoreGive(G.mutex);
}

// ── §5 Action handler ─────────────────────────────────────────────────────────
// Call while holding G.mutex.  Enqueues EVT_ACTION.
// survBuf/survLen: optional out-param for SURVEY response (send to client directly).
static void handleAction(int pid, uint8_t actType, int mpParam, int condTgt,
                         char* survBuf, int survCap, int* survLen) {
  Player& p    = G.players[pid];
  uint8_t terr = (p.r < MAP_ROWS && p.q < MAP_COLS) ? G.map[p.r][p.q].terrain : 0;
  if (terr >= NUM_TERRAIN) terr = 0;

  GameEvent ev = {};
  ev.type    = EVT_ACTION;
  ev.pid     = (uint8_t)pid;
  ev.actType = actType;
  ev.actOut  = AO_BLOCKED;  // default; overwritten on successful dispatch
  if (survLen) *survLen = 0;

  switch (actType) {

    // ──────────────────────────────────────────────────────────────
    case ACT_FORAGE: {
      uint8_t dn = TERRAIN_FORAGE_DN[terr];
      if (!dn || p.actUsed || p.movesLeft < 2) break;
      p.actUsed   = true;
      p.movesLeft = (int8_t)max(0, (int)p.movesLeft - 2);
      CheckResult cr = resolveCheck(pid, SK_FORAGE, dn, 0);
      ev.actDn  = dn; ev.actTot = (int8_t)cr.total;
      broadcastCheck(pid, SK_FORAGE, cr, false);
      if (cr.total >= (int)dn) {
        p.inv[1]     = (uint8_t)min((int)p.inv[1] + 1, 99);
        ev.actFoodD  = 1;
        ev.actOut    = AO_SUCCESS;
      } else if (cr.total >= (int)dn - 1) {
        p.inv[1]     = (uint8_t)min((int)p.inv[1] + 1, 99);
        ev.actFoodD  = 1;
        if (p.fatigue < 8) p.fatigue++;
        ev.actOut    = AO_PARTIAL;
      } else {
        if (p.fatigue < 8) p.fatigue++;
        ev.actOut    = AO_FAIL;
      }
      Serial.printf("[FORAGE]  P%d \"%s\" @ %s | DN%d tot:%d → %s | inv food:%d fat:%d\n",
        pid, p.name, T_NAME[terr], dn, cr.total,
        ev.actOut == AO_SUCCESS ? "SUCCESS" : ev.actOut == AO_PARTIAL ? "PARTIAL" : "FAIL",
        p.inv[1], p.fatigue);
      break;
    }

    // ──────────────────────────────────────────────────────────────
    case ACT_WATER: {
      if (!TERRAIN_HAS_WATER[terr] || p.actUsed || p.movesLeft < 1) break;
      int spend = max(1, min(3, min(mpParam, (int)p.movesLeft)));
      p.actUsed   = true;
      p.movesLeft = (int8_t)max(0, (int)p.movesLeft - spend);
      p.inv[0]    = (uint8_t)min((int)p.inv[0] + spend, 99);
      // TODO §6.5: contaminated water — pending token system
      ev.actWatD  = (int8_t)spend;
      ev.actOut   = AO_SUCCESS;
      Serial.printf("[WATER]   P%d \"%s\" @ %s | +%d water | inv water:%d mp→%d\n",
        pid, p.name, T_NAME[terr], spend, p.inv[0], p.movesLeft);
      break;
    }

    // ──────────────────────────────────────────────────────────────
    case ACT_SCAV: {
      uint8_t dn = TERRAIN_SALVAGE_DN[terr];
      if (!dn || p.actUsed || p.movesLeft < 2) break;
      p.actUsed   = true;
      p.movesLeft = (int8_t)max(0, (int)p.movesLeft - 2);
      // Ruins → advance Threat Clock
      if (TERRAIN_IS_RUINS[terr] && G.threatClock < 20) {
        G.threatClock++;
        Serial.printf("[TC]      Threat Clock → %d (Scavenge in Ruins)\n", G.threatClock);
      }
      CheckResult cr = resolveCheck(pid, SK_SCAVENGE, dn, 0);
      ev.actDn = dn; ev.actTot = (int8_t)cr.total;
      broadcastCheck(pid, SK_SCAVENGE, cr, false);
      if (cr.total >= (int)dn) {
        // Success: award 1 Scrap (placeholder for proper Scavenge deck)
        p.inv[4]      = (uint8_t)min((int)p.inv[4] + 1, 99);
        ev.actScrapD  = 1;
        ev.actOut     = AO_SUCCESS;
      } else if (cr.total >= (int)dn - 1) {
        // Partial: item + Encounter (Encounter not yet implemented)
        p.inv[4]      = (uint8_t)min((int)p.inv[4] + 1, 99);
        ev.actScrapD  = 1;
        ev.actOut     = AO_PARTIAL;
      } else {
        ev.actOut = AO_FAIL;
      }
      Serial.printf("[SCAV]    P%d \"%s\" @ %s | DN%d tot:%d → %s | scrap:%d\n",
        pid, p.name, T_NAME[terr], dn, cr.total,
        ev.actOut == AO_SUCCESS ? "SUCCESS" : ev.actOut == AO_PARTIAL ? "PARTIAL" : "FAIL",
        p.inv[4]);
      break;
    }

    // ──────────────────────────────────────────────────────────────
    case ACT_SHELTER: {
      // Requires at least 1 scrap and 1 MP
      if (p.inv[4] == 0) {
        Serial.printf("[SHELTER] ✗ %s (P%d) no scrap\n", p.name, pid);
        break;  // sends AO_BLOCKED to client so UI can respond
      }
      // Auto-select type: 2+ scrap = improved (2 MP), 1 scrap = lean-to (1 MP)
      uint8_t shelterType = (p.inv[4] >= 2) ? 2 : 1;
      uint8_t mpCost      = shelterType;  // basic=1 MP, improved=2 MP
      if (p.actUsed || p.movesLeft < (int8_t)mpCost) break;
      p.actUsed    = true;
      p.movesLeft  = (int8_t)max(0, (int)p.movesLeft - mpCost);
      p.inv[4]     = (uint8_t)max(0, (int)p.inv[4] - shelterType);
      G.map[p.r][p.q].shelter = shelterType;
      ev.actOut    = AO_SUCCESS;
      ev.actCnd    = shelterType;           // 1=lean-to, 2=improved (reuses cnd field)
      ev.actScrapD = -(int8_t)shelterType;  // scrap spent: -1 lean-to, -2 improved
      Serial.printf("[SHELTER] ✓ %s (P%d) built %s shelter @ (%d,%d) | -%d MP | scrap→%d\n",
        p.name, pid, shelterType == 2 ? "improved" : "lean-to",
        p.q, p.r, mpCost, (int)p.inv[4]);
      break;
    }

    // ──────────────────────────────────────────────────────────────
    case ACT_TREAT: {
      if (p.actUsed || p.movesLeft < 2) break;
      // Settlement (terrain 9) reduces all Treat DNs by 2 (§7.3A)
      uint8_t hexTerr    = G.map[p.r][p.q].terrain < NUM_TERRAIN ? G.map[p.r][p.q].terrain : 0;
      bool    inSettl    = (hexTerr == 9);
      uint8_t dn         = 7;
      bool    canTreat   = false;
      ev.actCnd          = (uint8_t)condTgt;
      switch (condTgt) {
        case TC_MINOR:   if (p.wounds[0] > 0)              { dn = 7;  canTreat = true; } break;
        case TC_BLEED:   if (p.statusBits & ST_BLEEDING)   { dn = 7;  canTreat = true; } break;
        case TC_FEVER:   if (p.statusBits & ST_FEVERED)    { dn = 9;  canTreat = true; } break;
        case TC_MAJOR:   break;  // TODO: requires Med Kit — blocked until item tracking
        case TC_RAD:     if (p.radiation > 0)              { dn = 7;  canTreat = true; } break;
        // Grievous Wound: Settlement-only; DN 10 base → 8 with Settlement bonus (§7.3A)
        // TODO: requires Adv Med Kit — blocked until item tracking
        case TC_GRIEVOUS:if (p.wounds[2] > 0 && inSettl)  { dn = 10; canTreat = true; } break;
        default:
          Serial.printf("[TREAT]   P%d invalid condTgt=%d — ignored\n", pid, (int)condTgt);
          return;
      }
      if (!canTreat) break;
      // Apply Settlement bonus after determining base DN
      if (inSettl) dn = (uint8_t)max(4, (int)dn - 2);
      p.actUsed   = true;
      p.movesLeft = (int8_t)max(0, (int)p.movesLeft - 2);
      CheckResult cr = resolveCheck(pid, SK_TREAT, dn, 0);
      ev.actDn = dn; ev.actTot = (int8_t)cr.total;
      broadcastCheck(pid, SK_TREAT, cr, false);
      if (cr.success) {
        switch (condTgt) {
          case TC_MINOR:   if (p.wounds[0] > 0) p.wounds[0]--; break;
          case TC_BLEED:   p.statusBits &= ~ST_BLEEDING; break;
          case TC_FEVER:   p.statusBits &= ~ST_FEVERED;  break;
          case TC_RAD: {
            int8_t prevR = (int8_t)p.radiation;
            p.radiation  = (uint8_t)max(0, (int)p.radiation - 2);  // Anti-Rad: −2 R
            ev.radD      = (int8_t)(p.radiation - prevR);
            ev.radR      = p.radiation;
            updateRadStatus(p);
            break;
          }
          case TC_GRIEVOUS: {  // §7.3A: remove 1 Grievous Wound + restore 1 LL
            if (p.wounds[2] > 0) {
              p.wounds[2]--;
              if (p.ll < 6) { p.ll++; ev.actLLD = 1; }
            }
            break;
          }
          default: break;  // unreachable — caught in condition switch above
        }
        ev.actOut = AO_SUCCESS;
      } else {
        if (condTgt == TC_BLEED && p.fatigue < 8) p.fatigue++;  // bleed fail → fatigue
        ev.actOut = AO_FAIL;
      }
      Serial.printf("[TREAT]   P%d \"%s\" cnd:%d%s DN%d tot:%d → %s | wd:%d/%d/%d sb:0x%02X rad:%d\n",
        pid, p.name, condTgt, inSettl ? "(Settl)" : "", dn, cr.total,
        ev.actOut == AO_SUCCESS ? "SUCCESS" : "FAIL",
        p.wounds[0], p.wounds[1], p.wounds[2], p.statusBits, p.radiation);
      break;
    }

    // ──────────────────────────────────────────────────────────────
    case ACT_SURVEY: {
      if (p.actUsed || p.movesLeft < 1) break;
      p.actUsed   = true;
      p.movesLeft = (int8_t)max(0, (int)p.movesLeft - 1);
      ev.actOut   = AO_SUCCESS;
      // Build ring of hexes at distance visR+1 and return to caller for direct send
      if (survBuf && survLen) {
        int visR; bool mr;
        playerVisParams(pid, &visR, &mr);
        *survLen = buildSurveyDisk(survBuf, survCap, p.q, p.r, visR);
      }
      Serial.printf("[SURVEY]  P%d \"%s\" @ (%d,%d) surveyed extended ring\n",
        pid, p.name, p.q, p.r);
      break;
    }

    // ──────────────────────────────────────────────────────────────
    case ACT_REST: {
      if (p.resting) break;  // already resting; prevent duplicate REST commands
      p.actUsed = true;
      p.resting = true;  // mark as resting; if all players rest, day ends early
      uint8_t hexShelt = G.map[p.r][p.q].shelter;  // 0=none, 1=lean-to, 2=improved
      // Camp bonus: any other player in the same hex
      int campCount = 0;
      for (int k = 0; k < MAX_PLAYERS; k++)
        if (k != pid && G.players[k].connected &&
            G.players[k].q == p.q && G.players[k].r == p.r) campCount++;
      // Fatigue reduction: base 2 | +1 camp | +1 lean-to | +2 improved shelter
      int fatRed = 2;
      if (campCount >= 1)  fatRed += 1;
      if (hexShelt == 1)   fatRed += 1;
      else if (hexShelt == 2) fatRed += 2;
      uint8_t prevFat = p.fatigue;
      p.fatigue    = (uint8_t)max(0, (int)p.fatigue - fatRed);
      // LL restore: Food≥4 AND Water≥3 AND fatigue low enough
      // Improved shelter relaxes the fatigue threshold (fat<6 instead of fat<4)
      bool llOk  = (p.food >= 4 && p.water >= 3);
      bool fatOk = (p.fatigue < 4) || (hexShelt == 2 && p.fatigue < 6);
      if (llOk && fatOk && p.ll < 6) {
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
      Serial.printf("[REST]    P%d \"%s\" fat %d→%d (red:%d shelt:%d camp:%d) | F:%d W:%d LL:%d%s res:%d (waiting for dawn)\n",
        pid, p.name, prevFat, p.fatigue, fatRed, hexShelt, campCount,
        p.food, p.water, p.ll, ev.actLLD ? " +1LL" : "", p.resolve);
      break;
    }

    default:
      break;  // actOut remains AO_BLOCKED
  }

  // Snapshot post-action state into event
  ev.actNewLL  = p.ll;
  ev.actNewFat = p.fatigue;
  ev.actNewMP  = p.movesLeft;
  enqEvt(ev);
}
