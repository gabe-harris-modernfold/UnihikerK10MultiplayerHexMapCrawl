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
    } else {
      CheckResult cr = resolveCheck(pid, SK_ENDURE, 8, 0);
      ev.actDn  = 8;
      ev.actTot = (int8_t)cr.total;
      ev.actOut = cr.success ? AO_SUCCESS : AO_FAIL;
    }

    if (ev.actOut == AO_FAIL) {
      if (p.ll > 0) { p.ll--; ledFlash(255, 0, 0); k10Play(MOTIF_GROSS_SLUDGE); }  // red = LL lost
      if (p.ll == 0) {
        p.movesLeft = 0;  // zero MP immediately — prevents phantom moves if dawn fires before slot reset
        GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
        enqEvt(devt);
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
    }
    p.radClean = true;  // reset for new day

    // ── Equipment radiation reduction (e.g. Hazmat Suit: rad=-1 per dawn) ────
    for (int s = 0; s < EQUIP_SLOTS; s++) {
      if (!p.equip[s]) continue;
      const ItemDef* def = getItemDef(p.equip[s]);
      if (def && def->statMods[STAT_RAD] < 0) {
        p.radiation = (uint8_t)max(0, (int)p.radiation + (int)def->statMods[STAT_RAD]);
      }
    }

    // ── Settlement rest: no food or water consumed (§settlement rule) ────────
    bool inSettlement = (G.map[p.r][p.q].terrain == 9);
    if (inSettlement) {
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
      }
    }

    // ── Shelter protection: resting in shelter suppresses all LL losses ────
    if (p.resting && G.map[p.r][p.q].shelter > 0 && llDelta < 0) {
      llDelta  = 0;
      expDelta = 0;
    }

    // ── Rest recovery: resting with adequate supplies → +1 LL ───────────
    if (p.resting && p.food >= 4 && p.water >= 3 && p.ll < (uint8_t)effectiveMaxLL(pid))
      llDelta++;

    // ── Apply LL delta (§4.5): losses first (F→W order), then gains ────────
    // Losses were accumulated first in llDelta (food, water, and exposure above).
    // Apply each step individually so we stop at 0 immediately.
    uint8_t prevLL = p.ll;  // snapshot before changes to compute actual delta
    if (llDelta < 0) {
      for (int i = 0; i < -llDelta; i++) {
        if (p.archetype == 5 && (esp_random() % 4 == 0)) {  // Endurer: 25% chance to resist
          continue;
        }
        if (p.ll > 0) p.ll--;
        if (p.ll == 0) {
          GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
          enqEvt(devt);
          break;
        }
      }
    } else if (llDelta > 0) {
      p.ll = (uint8_t)min((int)p.ll + llDelta, (int)effectiveMaxLL(pid));
    }
    int8_t actualDelta = (int8_t)((int)p.ll - (int)prevLL); // true LL change (clamped)
    if (actualDelta < 0) { ledFlash(255, 0, 0); k10Play(MOTIF_GROSS_SLUDGE); }  // red = LL lost

    // ── Reset daily move budget and action flags ────────────────────────────
    p.movesLeft    = (p.ll == 0) ? 0 : (int8_t)effectiveMP(pid);  // downed: no moves
    p.actUsed      = false;
    p.encPenApplied = false;
    p.resting      = false;
    // Apply equipped item operating costs (fuel-gated MP bonuses added here)
    applyDawnItemCosts(pid);


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


  cell.amount = 0; cell.resource = 0;
  cell.respawnTimer = RESPAWN_TICKS;

  // ── Encumbrance check (§5): if inv > invSlots, deduct 1 MP once per day ─
  int totalInvAfter = 0;
  for (int k = 0; k < 5; k++) totalInvAfter += (int)p.inv[k];
  if (totalInvAfter > (int)p.invSlots && !p.encPenApplied) {
    p.encPenApplied = true;
    if (p.movesLeft > 0) p.movesLeft--;
  }
}

// ── Valid move bitmask ────────────────────────────────────────────────────────
// Returns a 6-bit mask (bit N = direction N is passable and player can move).
// Used by broadcastState() to let the client gray out blocked direction buttons.
// Fog (0xFF) is treated as impassable — movement into fog is denied until revealed.
static uint8_t computeValidMoves(int pid) {
  Player& p = G.players[pid];
  if (!p.connected) return 0;
  if ((p.ll == 0) || p.resting || p.movesLeft == 0) return 0;
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
  if (p.ll == 0) return;  // downed — waiting for slot reset
  if (encounters[pid].active)  return;  // locked during active encounter
  if (p.resting) {
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
      return;
    }
    // Terrain unlocked by equipment — use a default MC of 2 for river traversal
    mc = 2;
  }

  // ── Weather movement penalty ─────────────────────────────────────────────
  mc = (uint8_t)min(255, (int)mc + (int)WEATHER_MOVE_PENALTY[G.weatherPhase]);

  // ── MP budget check (§4.5 hard daily cap) ──────────────────────────────
  if (p.movesLeft == 0) {
    return;
  }

  uint32_t cd  = (uint32_t)MOVE_CD_MS * mc;
  uint32_t now = millis();
  if (now - p.lastMoveMs < cd) return;  // cooldown — silent, normal behaviour
  p.lastMoveMs = now;

  [[maybe_unused]] int16_t oldQ = p.q, oldR = p.r;
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
  [[maybe_unused]] int     effVisR = (visLvl <= -3) ? 0 : (visLvl == -2) ? 1 : (visLvl == -1) ? 2 : (visLvl == 0) ? VISION_R : (visLvl == 1) ? VISION_R + 1 : VISION_R + 2;
  // Deduct movement cost from daily MP budget
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - (int)mc);


  // ── Radiation entry check (§6.2): Rad-tagged terrain → Endure DN6 or +1 R ──
  int8_t radGain = 0;
  if (destTerrain < NUM_TERRAIN && TERRAIN_IS_RAD[destTerrain]) {
    p.radClean = false;   // day is no longer clean
    if (p.radiation < 10) {
      CheckResult cr = resolveCheck(pid, SK_ENDURE, 6, 0);
      if (!cr.success) {
        p.radiation++;
        radGain = 1;
        ledFlash(0, 255, 0); k10Play(MOTIF_GEIGER);  // green + geiger ticks
      } else {
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
