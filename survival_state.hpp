#pragma once
// ── Survival state: day cycle, resource collection, movement ─────────────────
// Included from Esp32HexMapCrawl.ino after inventory_items.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

// ── §6.2 Dusk radiation check (all players) ──────────────────────────────────
// Called from tickGame() while holding G.mutex.
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
      if (p.ll > 0) { p.ll--; ledFlash(255, 0, 0); k10PlaySeq(SEQ_DAMAGE); }  // red = LL lost
      if (p.ll == 0) {
        p.statusBits |= ST_DOWNED;
        p.movesLeft = 0;  // zero MP immediately — prevents phantom moves if dawn fires before slot reset
        GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
        enqEvt(devt);
        Serial.printf("[DOWNED]  P%d \"%s\" LL=0 (dusk wound) — slot queued for reset\n", pid, p.name);
      }
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

    // ── Equipment radiation reduction (e.g. Hazmat Suit: rad=-1 per dawn) ────
    for (int s = 0; s < EQUIP_SLOTS; s++) {
      if (!p.equip[s]) continue;
      const ItemDef* def = getItemDef(p.equip[s]);
      if (def && def->statMods[STAT_RAD] < 0) {
        p.radiation = (uint8_t)max(0, (int)p.radiation + (int)def->statMods[STAT_RAD]);
        updateRadStatus(p);
        Serial.printf("[ITEM]  P%d \"%s\" dawn rad mod %+d → R:%d\n",
          pid, def->name, (int)def->statMods[STAT_RAD], (int)p.radiation);
      }
    }

    // ── Settlement rest: no food or water consumed (§settlement rule) ────────
    bool inSettlement = (G.map[p.r][p.q].terrain == 9);
    if (inSettlement) {
      Serial.printf("[DAWN]    P%d \"%s\" in Settlement — food/water upkeep skipped\n", pid, p.name);
    }

    // ── Food (§4.1): consume 1 token; F track +1; else F track -1 ─────────
    if (!inSettlement) {
      if (p.inv[1] > 0) {
        p.inv[1]--;
        applyFStep(p, +1, llDelta);
      } else {
        applyFStep(p, -1, llDelta);
      }
    }

    // ── Water (§4.2): consume 2 tokens ───────────────────────────────────────
    if (!inSettlement) {
      int have = (int)p.inv[0];
      int use  = min(have, 2);
      int miss = 2 - use;
      p.inv[0] -= (uint8_t)use;
      for (int i = 0; i < use;  i++) applyWStep(p, +1, llDelta);
      for (int i = 0; i < miss; i++) applyWStep(p, -1, llDelta);
    }

    // ── Exposure (§7.3): no built shelter + terrain SV < 2 → LL−1 ──────────
    int8_t expDelta = 0;
    {
      uint8_t terr   = G.map[p.r][p.q].terrain < NUM_TERRAIN ? G.map[p.r][p.q].terrain : 0;
      uint8_t sv     = TERRAIN_SV[terr];
      uint8_t shelt  = G.map[p.r][p.q].shelter;
      bool    covered = (shelt > 0) || (sv >= 2);
      if (!covered) {
        llDelta--;
        expDelta = -1;
        Serial.printf("[DAWN]    P%d \"%s\" exposed (SV%d shelt:%d) −1 LL\n", pid, p.name, sv, shelt);
      }
    }

    // ── Shelter protection: resting in shelter suppresses all LL losses ────
    if (p.resting && G.map[p.r][p.q].shelter > 0 && llDelta < 0) {
      Serial.printf("[DAWN]    P%d \"%s\" shelter protection — %d LL loss(es) suppressed\n", pid, p.name, -llDelta);
      llDelta  = 0;
      expDelta = 0;
    }

    // ── Apply LL delta (§4.5): losses first (F→W order), then gains ────────
    // Losses were accumulated first in llDelta (food, water, and exposure above).
    // Apply each step individually so we stop at 0 immediately.
    uint8_t prevLL = p.ll;  // snapshot before changes to compute actual delta
    if (llDelta < 0) {
      for (int i = 0; i < -llDelta; i++) {
        if (p.archetype == 5 && (esp_random() % 4 == 0)) {  // Endurer: 25% chance to resist
          Serial.printf("[DAWN]    P%d \"%s\" Endurer resists LL loss\n", pid, p.name);
          continue;
        }
        if (p.ll > 0) p.ll--;
        if (p.ll == 0) {
          p.statusBits |= ST_DOWNED;
          GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
          enqEvt(devt);
          Serial.printf("[DOWNED]  P%d \"%s\" LL=0 (dawn upkeep) — slot queued for reset\n", pid, p.name);
          break;
        }
      }
    } else if (llDelta > 0) {
      p.ll = (uint8_t)min((int)p.ll + llDelta, (int)effectiveMaxLL(pid));
    }
    int8_t actualDelta = (int8_t)((int)p.ll - (int)prevLL); // true LL change (clamped)
    if (actualDelta < 0) { ledFlash(255, 0, 0); k10PlaySeq(SEQ_DAMAGE); }  // red = LL lost

    // ── Reset daily move budget and action flags ────────────────────────────
    p.movesLeft    = (p.statusBits & ST_DOWNED) ? 0 : (int8_t)effectiveMP(pid);  // downed: no moves
    p.actUsed      = false;
    p.encPenApplied = false;
    p.resting      = false;
    // Apply equipped item operating costs (fuel-gated MP bonuses added here)
    applyDawnItemCosts(pid);

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
    ev.dawnExpD    = expDelta;
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

// ── Valid move bitmask ────────────────────────────────────────────────────────
// Returns a 6-bit mask (bit N = direction N is passable and player can move).
// Used by broadcastState() to let the client gray out blocked direction buttons.
// Fog (0xFF) is treated as impassable — movement into fog is denied until revealed.
static uint8_t computeValidMoves(int pid) {
  Player& p = G.players[pid];
  if (!p.connected) return 0;
  if ((p.statusBits & ST_DOWNED) || p.resting || p.movesLeft == 0) return 0;
  uint8_t mask = 0;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(p.q + DQ[d]);
    int nr = wrapR(p.r + DR[d]);
    uint8_t t = G.map[nr][nq].terrain;
    // BUG-10 fix: removed `t == 0xFF ||` — fog is not passable
    if (TERRAIN_MC[t] != 255) mask |= (1 << d);
  }
  return mask;
}

// ── Player move ───────────────────────────────────────────────────────────────
static void movePlayer(int pid, int dir) {
  if (dir < 0 || dir > 5) return;
  Player& p  = G.players[pid];
  if (p.statusBits & ST_DOWNED) return;  // downed — waiting for slot reset
  if (encounters[pid].active)  return;  // locked during active encounter
  if (p.resting) {
    Serial.printf("[BLOCKED] P%d \"%s\" resting — waiting for other survivors to finish\n", pid, p.name);
    return;
  }
  int     nq = wrapQ(p.q + DQ[dir]);
  int     nr = wrapR(p.r + DR[dir]);

  uint8_t destTerrain = G.map[nr][nq].terrain;
  uint8_t mc          = TERRAIN_MC[destTerrain];
  if (mc == 255) {
    // Check if an equipped item unlocks this terrain type
    // River Channel (terrain 11) → TERR_PASS_RIVER (bit 0)
    uint8_t neededBit = 0;
    if (destTerrain == 11) neededBit = TERR_PASS_RIVER;
    // Add future terrain unlocks here (e.g. Nuke Crater → TERR_PASS_NUKE)
    if (!neededBit || !hasPassTerrainBit(pid, neededBit)) {
      Serial.printf("[BLOCKED] P%d \"%s\" dir:%s → (%2d,%2d) %s (impassable)\n",
        pid, p.name, DIR_NAME[dir], nq, nr, T_NAME[destTerrain]);
      return;
    }
    // Terrain unlocked by equipment — use a default MC of 2 for river traversal
    mc = 2;
    Serial.printf("[ITEM]  P%d terrain unlock: %s traversal\n", pid, T_NAME[destTerrain]);
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

  // Exploration bonus: +1 score first time this player visits this hex
  bool firstVisit = !(G.map[p.r][p.q].footprints & (1 << pid));
  if (firstVisit) {
    p.score = (uint16_t)min((int)p.score + 1, 65535);
  }
  int8_t exploGain = firstVisit ? 1 : 0;

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
        ledFlash(0, 255, 0); k10PlaySeq(SEQ_GEIGER);  // green + geiger ticks
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
    ev.exploD = exploGain;
    ev.moveMP = p.movesLeft;
    enqEvt(ev); }
  collectResource(pid, p.q, p.r);
}
