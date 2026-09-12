#pragma once
// ── Survival state: day cycle, resource collection, movement ─────────────────
// Included from Esp32HexMapCrawl.ino after inventory_items.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

// ── Camp detection (Quartermaster trait) ─────────────────────────────────────
// A Camp is 2+ connected survivors sharing a hex.  Returns true if `pid` is in
// a Camp that includes a connected Quartermaster (archetype 1).
// Call while holding G.mutex.
static bool inQuartermasterCamp(int pid) {
  const Player& me = G.players[pid];
  if (!me.connected) return false;
  bool hasQM = (me.archetype == 1);
  int  count = 1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i == pid || !G.players[i].connected) continue;
    if (G.players[i].q != me.q || G.players[i].r != me.r) continue;
    count++;
    if (G.players[i].archetype == 1) hasQM = true;
  }
  return hasQM && count >= 2;
}

// ── §6.2 Dusk radiation check (all players) ──────────────────────────────────
// Called from tickGame() while holding G.mutex.  Survivors already at LL 0
// are skipped: their EVT_DOWNED is queued and the slot is about to be reset.
static void duskCheck() {
  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    Player& p = G.players[pid];
    if (!p.connected || p.ll == 0 || p.radiation < 7) continue;

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
// Called from tickGame() while holding G.mutex.  Downed survivors (LL 0) are
// skipped — duskCheck() or an encounter already queued their EVT_DOWNED, and
// running upkeep on them would either queue a second one or revive them.
static void dawnUpkeep() {
  for (int pid = 0; pid < MAX_PLAYERS; pid++) {
    Player& p = G.players[pid];
    if (!p.connected || p.ll == 0) continue;

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

    // ── Quartermaster trait: in a Camp (2+ survivors sharing a hex, one of
    //    them the Quartermaster) every 2 tokens consumed restores 1 extra
    //    track step.  Water consumes 2/day → +1 every day.  Food consumes
    //    1/day → +1 on alternate days, which is the same 2-for-1 rate.
    bool qmCamp = !inSettlement && inQuartermasterCamp(pid);

    // ── Food (§4.1): consume 1 token; F track +1; else F track -1 ─────────
    if (!inSettlement) {
      if (p.inv[1] > 0) {
        p.inv[1]--;
        applyFStep(p, +1, llDelta);
        if (qmCamp && (G.dayCount & 1)) applyFStep(p, +1, llDelta);
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
      if (qmCamp && use >= 2) applyWStep(p, +1, llDelta);
    }

    // ── Exposure (§7.3): no built shelter + terrain SV < 2 → LL−1 ──────────
    int8_t expDelta = 0;
    {
      uint8_t terr   = G.map[p.r][p.q].terrain < NUM_TERRAIN ? G.map[p.r][p.q].terrain : 0;
      uint8_t sv     = TERRAIN_SV[terr];
      uint8_t shelt  = G.map[p.r][p.q].shelter;
      bool    covered = (shelt > 0) || (sv >= 2)
                     || (p.archetype == 5)                       // Endurer needs no shelter
                     || hasNarrativeParam(pid, NAR_COLD_IMMUNE); // Bear Skin Cape
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

    // ── Rest recovery: resting with adequate supplies → +1 LL, and a minor
    //    wound knits closed.  W floors at 1, so the water gate must be >1.
    bool restedWell = p.resting && p.food >= 4 && p.water >= 3;
    if (restedWell && p.ll < (uint8_t)effectiveMaxLL(pid)) llDelta++;
    if (restedWell) healWound(p, WOUND_MINOR);

    // ── Apply LL delta (§4.5): losses first (F→W order), then gains ────────
    // Losses were accumulated first in llDelta (food, water, and exposure above).
    // Apply each step individually so we stop at 0 immediately.
    uint8_t prevLL = p.ll;  // snapshot before changes to compute actual delta
    if (llDelta < 0) {
      for (int i = 0; i < -llDelta; i++) {
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
    ev.dawnWndMin  = p.wounds[WOUND_MINOR];
    ev.dawnWndMaj  = p.wounds[WOUND_MAJOR];
    enqEvt(ev);
  }
}

// ── Resource collection ───────────────────────────────────────────────────────
// Failure reason codes (sent in EVT_COLLECT_FAIL.amt as a reason byte)
static constexpr uint8_t COL_FAIL_DESYNC   = 1; // server has no resource here (client/server out of sync)
static constexpr uint8_t COL_FAIL_INV_FULL = 2; // inventory at cap

static void collectResource(int pid, int q, int r) {
  HexCell& cell = G.map[r][q];
  if (cell.resource == 0 || cell.amount == 0) {
    Log.notice("col SKIP desync pid=%d q=%d r=%d cellRes=%d cellAmt=%d",
               pid, q, r, (int)cell.resource, (int)cell.amount);
    return;
  }
  Player&  p    = G.players[pid];
  uint8_t  idx  = cell.resource - 1;
  uint8_t  gain = cell.amount;

  // Enforce total-carry cap — count all tokens across all types against the
  // pack size in effect (archetype base + equipment slot bonuses)
  int totalInv = 0;
  for (int k = 0; k < 5; k++) totalInv += (int)p.inv[k];
  int cap = (int)effectiveInvSlots(p);
  if (totalInv >= cap) {
    Log.notice("col SKIP inv-full pid=%d q=%d r=%d res=%d totalInv=%d/%d",
               pid, q, r, (int)cell.resource, totalInv, cap);
    GameEvent ev = {}; ev.type = EVT_COLLECT_FAIL; ev.pid = (uint8_t)pid;
    ev.q = (int16_t)q; ev.r = (int16_t)r; ev.res = cell.resource; ev.amt = COL_FAIL_INV_FULL;
    // Reuse dawnLL (unused for collect events, same trick as EVT_COLLECT's
    // `remaining`) to carry the effective pack size, so the client can say
    // "pack full (8/8)" without guessing at equipment slot bonuses.
    ev.dawnLL = (uint8_t)cap;
    enqEvt(ev);
    return;  // cell stays untouched — icon correctly remains visible
  }
  // Collect only as many as there is room for
  int room = cap - totalInv;
  gain = (uint8_t)min((int)gain, room);
  if (gain == 0) {
    Log.notice("col SKIP no-room pid=%d q=%d r=%d", pid, q, r);
    return;
  }

  p.inv[idx] = (uint8_t)min((int)p.inv[idx] + gain, 99);
  p.score   += gain * 10;

  // Partial-pickup leak fix: only zero the cell when we drained it. If we
  // took less than was there (inv room < pile size), leave the remainder
  // on the hex so it isn't deleted from the world.
  uint8_t remaining = (uint8_t)((int)cell.amount - (int)gain);
  cell.amount = remaining;
  if (remaining == 0) {
    cell.resource     = 0;
    cell.respawnTimer = RESPAWN_TICKS;
  }

  { GameEvent ev = {}; ev.type = EVT_COLLECT; ev.pid = (uint8_t)pid;
    ev.q = (int16_t)q; ev.r = (int16_t)r;
    ev.res = (uint8_t)(idx + 1);   // original resource type (1-5)
    ev.amt = gain;
    // Reuse dawnLL to carry post-pickup remaining amount (0 = hex now empty).
    // dawnLL is unused for collect events; lets the client update gameMap.amount
    // for partial pickups without a second event.
    ev.dawnLL = remaining;
    enqEvt(ev); }
  // Note: gain is clamped to remaining pack room above, so collection can
  // never push a survivor over the cap.  Actions, encounter loot, and trades
  // can; effectiveMP() charges the encumbrance penalty at the next dawn.
}

// ── Voluntary resource drop ────────────────────────────────────
// The carry cap counts resource tokens, so a survivor with a full pack needs a
// way to dump them — this is it (WS "drop_res", char-sheet inventory boxes).
// Tokens land back on the hex when it can hold them (empty, or already holding
// the same resource) so the pile can be picked up again later; a hex already
// holding a *different* resource can't take them and they are lost to the dust.
// Either way the 10 pts/token collectResource() granted comes back off —
// without that, drop→collect on the same hex is an infinite score pump.
// Mirrored in mock-server/server.js (dropResource).
// Returns tokens removed from the pack (0 = nothing happened). *outOnGround is
// true when they landed on the hex, *outRem is the hex pile after the drop.
static uint8_t dropResource(int pid, uint8_t res, uint8_t qty,
                            bool* outOnGround, uint8_t* outRem) {
  if (outOnGround) *outOnGround = false;
  if (outRem)      *outRem      = 0;
  if (res < 1 || res > 5 || qty == 0) return 0;
  Player& p   = G.players[pid];
  uint8_t idx = (uint8_t)(res - 1);
  uint8_t take = (uint8_t)min((int)qty, (int)p.inv[idx]);
  if (take == 0) {
    Log.notice("drop_res SKIP empty pid=%d res=%d qty=%d", pid, (int)res, (int)qty);
    return 0;
  }
  p.inv[idx] = (uint8_t)(p.inv[idx] - take);
  uint16_t refund = (uint16_t)take * 10;
  p.score = (p.score > refund) ? (uint16_t)(p.score - refund) : 0;

  HexCell& cell = G.map[p.r][p.q];
  bool onGround = (cell.resource == 0 || cell.resource == res);
  if (onGround) {
    int pile = (cell.resource == res ? (int)cell.amount : 0) + (int)take;
    cell.resource     = res;
    cell.amount       = (uint8_t)min(pile, 99);
    cell.respawnTimer = 0;   // occupied hexes never respawn — don't leave one armed
    if (outRem) *outRem = cell.amount;
  }
  if (outOnGround) *outOnGround = onGround;
  Log.notice("drop_res OK pid=%d q=%d r=%d res=%d qty=%d ground=%d pile=%d sc=%d",
             pid, (int)p.q, (int)p.r, (int)res, (int)take,
             onGround ? 1 : 0, (int)cell.amount, (int)p.score);
  return take;
}

// ── Valid move bitmask ────────────────────────────────────────────────────────
// Returns a 6-bit mask (bit N = direction N is passable and player can move).
// Used by broadcastState() to let the client gray out blocked direction buttons.
// Shares canEnterTerrain() with movePlayer() so equipment unlocks (river gear,
// climbing gear) show up as enabled buttons.
static uint8_t computeValidMoves(int pid) {
  Player& p = G.players[pid];
  if (!p.connected) return 0;
  if ((p.ll == 0) || p.resting || p.movesLeft == 0) return 0;
  uint8_t mask = 0;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(p.q + DQ[d]);
    int nr = wrapR(p.r + DR[d]);
    if (canEnterTerrain(pid, G.map[nr][nq].terrain, nullptr)) mask |= (1 << d);
  }
  return mask;
}

// ── Player move ───────────────────────────────────────────────────────────────
static void movePlayer(int pid, int dir) {
  if (dir < 0 || dir > 5) return;
  Player& p  = G.players[pid];
  if (p.ll == 0) return;  // downed — waiting for slot reset
  if (encounters[pid].active)  return;  // locked during active encounter
  if (p.resting) return;
  int     nq = wrapQ(p.q + DQ[dir]);
  int     nr = wrapR(p.r + DR[dir]);

  uint8_t destTerrain = G.map[nr][nq].terrain;
  uint8_t mc          = 0;
  if (!canEnterTerrain(pid, destTerrain, &mc)) return;

  // ── Weather movement penalty ─────────────────────────────────────────────
  mc = (uint8_t)min(255, (int)mc + (int)WEATHER_MOVE_PENALTY[G.weatherPhase]);

  // ── Guide trait (archetype 0): a companion moving into a hex the Guide is
  //    standing on follows their line and pays MC−1 (min 1).
  if (p.archetype != 0) {
    for (int g = 0; g < MAX_PLAYERS; g++) {
      if (g == pid || !G.players[g].connected) continue;
      if (G.players[g].archetype != 0) continue;
      if (G.players[g].ll == 0) continue;
      if ((int)G.players[g].q == nq && (int)G.players[g].r == nr) {
        mc = (uint8_t)max(1, (int)mc - 1);
        break;
      }
    }
  }

  // ── MP budget check (§4.5 hard daily cap) ──────────────────────────────
  if (p.movesLeft == 0) {
    return;
  }

  uint32_t cd  = (uint32_t)MOVE_CD_MS * mc;
  uint32_t now = millis();
  if (now - p.lastMoveMs < cd) return;  // cooldown — silent, normal behaviour
  p.lastMoveMs = now;

  p.q = (int16_t)nq;
  p.r = (int16_t)nr;
  p.steps++;
  lastCaravanHex[pid].q = -1; lastCaravanHex[pid].r = -1;  // moved — re-arm the caravan trade prompt

  // Exploration bonus: +1 score first time this player visits this hex
  bool firstVisit = !(G.map[p.r][p.q].footprints & (1 << pid));
  if (firstVisit) {
    p.score = (uint16_t)min((int)p.score + 1, 65535);
  }
  int8_t exploGain = firstVisit ? 1 : 0;

  // Mark footprint at new hex (visible to all players)
  G.map[p.r][p.q].footprints |= (1 << pid);

  // Deduct movement cost from daily MP budget
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - (int)mc);

  // ── Radiation entry check (§6.2): Rad-tagged terrain → Endure DN6 or +1 R ──
  // Sealed gear (TERR_PASS_RAD: Glow Suit, Wheeze Filter) skips the check and
  // keeps the day "clean" for the dawn R−1 recovery.
  int8_t radGain = 0;
  if (TERRAIN_IS_RAD[destTerrain] && !hasPassTerrainBit(pid, TERR_PASS_RAD)) {
    p.radClean = false;   // day is no longer clean
    if (p.radiation < 10) {
      CheckResult cr = resolveCheck(pid, SK_ENDURE, 6, 0);
      if (!cr.success) {
        p.radiation++;
        radGain = 1;
        ledFlash(0, 255, 0); k10Play(MOTIF_GEIGER);  // green + geiger ticks
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
