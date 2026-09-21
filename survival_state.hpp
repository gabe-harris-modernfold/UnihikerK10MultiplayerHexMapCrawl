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
    // Depth too, same as every other co-location test (see the cross-board
    // leak list in docs/tunnel-system-spec.md): q/r stay pinned to the hatch
    // underground, so without this a survivor stood on the hatch and one far
    // below it would share a "camp".
    if (G.players[i].q != me.q || G.players[i].r != me.r) continue;
    if (G.players[i].depth != me.depth) continue;
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

    // ── Equipment radiation delta (Glow Suit rad=-1, Lead Snuggie rad=-5) ────
    // items.cfg documents `rad` as signed and "Equipment: applied each dawn",
    // so honour the sign.  This used to be guarded `< 0`, which quietly made
    // irradiating gear a no-op -- a key that parsed cleanly and did nothing.
    for (int s = 0; s < EQUIP_SLOTS; s++) {
      if (!p.equip[s]) continue;
      const ItemDef* def = getItemDef(p.equip[s]);
      if (def && def->statMods[STAT_RAD]) {
        p.radiation = (uint8_t)constrain((int)p.radiation + (int)def->statMods[STAT_RAD], 0, 10);
      }
    }

    // ── Settlement flag: food/water consumption exemption removed — a
    //    Settlement still guarantees the rest-recovery heal below regardless
    //    of supply levels, same as it always has.
    bool inSettlement = (G.map[p.r][p.q].terrain == 9);

    // ── Quartermaster trait: in a Camp (2+ survivors sharing a hex, one of
    //    them the Quartermaster) every 2 tokens consumed restores 1 extra
    //    track step.  Water consumes 2/day → +1 every day.  Food consumes
    //    1/day → +1 on alternate days, which is the same 2-for-1 rate.
    bool qmCamp = inQuartermasterCamp(pid);

    // ── Food (§4.1): consume 1 token; F track +1; else F track -1 ─────────
    if (p.inv[1] > 0) {
      p.inv[1]--;
      applyFStep(p, +1, llDelta);
      if (qmCamp && (G.dayCount & 1)) applyFStep(p, +1, llDelta);
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
      if (qmCamp && use >= 2) applyWStep(p, +1, llDelta);
    }

    // ── Exposure (§7.3): no built shelter + terrain SV < 2 → LL−EXPOSURE_BITE
    //
    // The bite is 2, not 1, and that is only safe because exposure can no
    // longer land the killing blow (see the apply block below). That floor is
    // what turns this from a difficulty knob into a *pacing* knob: cranking it
    // buys more entries into the danger band without buying more deaths,
    // which is exactly the shortfall measured on v5 — the deaths-to-near-miss
    // ratio had reached 1:3.3 against a 1:4 target, but the brush count was
    // 1.84 against a budget of 4 and survivors sat at full health 39% of the
    // time. They were not dipping often enough.
    //
    // Cover is still a complete answer, so this widens the gap between a
    // player who builds and one who does not rather than taxing everyone.
    static constexpr int EXPOSURE_BITE = 2;
    int8_t expDelta = 0;
    {
      uint8_t terr   = G.map[p.r][p.q].terrain < NUM_TERRAIN ? G.map[p.r][p.q].terrain : 0;
      uint8_t sv     = TERRAIN_SV[terr];
      uint8_t shelt  = G.map[p.r][p.q].shelter;
      // Underground there is no weather to be exposed to: a corridor is cover
      // by construction, and the world system already skips depth != 0 for
      // fire, flood, the chem storm and the fog. This is a correctness fix as
      // much as a rule -- q/r stay pinned to the hatch while depth != 0 (see
      // tunnels.hpp), so the tick was reading the *surface* hex and a survivor
      // sheltering under a Bunker Entrance (SV 2) came out covered while one
      // under a Vent Shaft (SV 0) took the full bite. Same tunnel, same
      // weather, opposite answer, settled by which hole they happened to use.
      // What the tunnels charge instead is bad air, below, and only if you
      // sleep down there.
      bool    covered = (shelt > 0) || (sv >= 2) || (p.depth != 0)
                     || (p.archetype == 5)                       // Endurer needs no shelter
                     || hasNarrativeParam(pid, NAR_COLD_IMMUNE); // Bear Skin Cape
      if (!covered) {
        llDelta  -= EXPOSURE_BITE;
        expDelta  = (int8_t)-EXPOSURE_BITE;
      }
    }

    // ── Shelter protection: resting in shelter suppresses all LL losses ────
    // Surface only. G.map[p.r][p.q] is the hatch a descended survivor came in
    // through, so without the depth guard a shelter pitched on that surface
    // hex would cancel a bad-air roll happening a long way underneath it.
    if (p.resting && !p.depth && G.map[p.r][p.q].shelter > 0 && llDelta < 0) {
      llDelta  = 0;
      expDelta = 0;
    }

    // ── Bad air (§tunnels): sleeping underground is a gamble ──────────────
    // The tunnels trade one risk for another. Down there the weather cannot
    // reach you -- exposure is off entirely, worth a guaranteed 2 LL a night
    // on 96% of the map -- but a sealed bunker's air is dead still, and a
    // night spent breathing it costs a point TUNNEL_REST_LL_PCT of the time.
    //
    // Unlike exposure this one CAN land the killing blow, and the asymmetry is
    // deliberate. Exposure is floored at LL 1 because it is a silent tick on
    // nearly every hex with nothing to react to and nothing to remember; bad
    // air is a roll the survivor opted into by bedding down below, and it
    // rides the dawn event as "air" so it reads as something that happened.
    // Floor this one too and the tunnels become the single place on the map
    // where a survivor at LL 1 cannot be killed by attrition -- exactly the
    // absorbing state the rest-recovery note above exists to keep out, only
    // now with a door on it. Dying down here is not a soft-lock either:
    // tickGame()'s downed sweep hands them back to the surface through
    // surfacePlayer() (tunnels.hpp).
    int8_t airDelta = 0;
    if (p.resting && p.depth && (int)(esp_random() % 100) < TUNNEL_REST_LL_PCT) {
      llDelta  -= 1;
      airDelta  = -1;
    }

    // ── Rest recovery: resting with adequate supplies → +1 LL, and a minor
    //    wound knits closed.  W floors at 1, so the water gate must be >1.
    //    Settlements always qualify regardless of supply levels — beds and
    //    hot meals heal on their own, same guarantee a well-stocked
    //    player-built shelter gives.
    //
    //    The gate was F>=4 && W>=3, which is the single reason brushes with
    //    death turned into deaths instead of close calls. The thing that
    //    drives a survivor into the danger band is running short — and this
    //    demanded they be *well* stocked to climb back out, so the moment
    //    they needed healing was exactly the moment they could not have it.
    //    Measured across 8 post-flash runs: deaths to near-misses 1:0.9,
    //    against a design target nearer 1:4, and LL 1-2 occupied 6.1% of the
    //    time while LL 6-7 took 59.5%. Characters were fine or doomed.
    //
    //    F>=2 && W>=2 still asks for a real margin above the floor — it is
    //    not free, and a survivor scraping the bottom of both tracks still
    //    gets nothing — but it is reachable from inside the hole.
    //    The heal is +2 rather than +1 while badly hurt (LL <= 2), and that
    //    is load-bearing, not generosity. With a flat +1 the arithmetic at
    //    the bottom is exactly zero: rest gives +1, the dawn exposure tick
    //    takes 1, and a survivor at LL 1 can never climb out. Once exposure
    //    stopped being able to kill (see the apply block below) that turned
    //    into an absorbing state — measured, LL 1 went to **37.2% of all
    //    time**, characters pinned at death's door indefinitely. That is not
    //    tension, it is the same flatness as permanent full health with worse
    //    lighting, and it collapsed the brush count to 1.44 per session
    //    because the metric counts *entries* into the danger band and a
    //    survivor parked there never makes another one.
    //
    //    +2 at the bottom nets +1 against exposure, so a wounded survivor
    //    climbs out over a couple of days, wanders back into trouble, and
    //    dips again. The oscillation is the point: brushes come from
    //    repeatedly entering the band, not from living in it.
    //    The wounded heal is expressed as EXPOSURE_BITE + 1 rather than a
    //    literal, and that coupling is the whole point: the climb-out only
    //    works while rest beats the dawn tick by exactly one. A flat +1
    //    against a bite of 1 nets zero and pinned survivors at LL 1 for 37%
    //    of all time (the v4 measurement). Raising the bite to 2 without
    //    raising this re-creates the same trap silently -- it was caught by a
    //    smoke test rather than by a run, which is the only reason it is not
    //    in this build. Tie them together and the property survives retuning.
    bool restedWell = p.resting && ((p.food >= 2 && p.water >= 2) || inSettlement);
    if (restedWell && p.ll < (uint8_t)effectiveMaxLL(pid)) {
      llDelta += (p.ll <= 2) ? (EXPOSURE_BITE + 1) : 1;
    }
    if (restedWell) healWound(p, WOUND_MINOR);

    // ── Apply LL delta (§4.5): losses first (F→W order), then gains ────────
    // Losses were accumulated first in llDelta (food, water, and exposure above).
    // Apply each step individually so we stop at 0 immediately.
    //
    // Exposure is applied LAST and separately, because **exposure never lands
    // the killing blow**. It wounds down to LL 1 and stops.
    //
    // This is the fix for the shape of the tension curve rather than its
    // size. Deaths to near-misses measured 1:1.2 against a design target
    // nearer 1:4 -- characters were dying instead of having close calls. The
    // arithmetic points at exactly one cause: of 54 deaths across 4 runs, 33
    // were exposure, and converting those to survivals gives 1:4.6. No other
    // cause moves it past 1:1.8.
    //
    // It is also the right cause to pick on its own merits. Exposure is a
    // silent -1 every dawn on 96% of the map with no event attached -- there
    // is nothing to react to and nothing to remember. Everything else that
    // kills is something that *happened*: a thirst crisis, a hazard, a
    // lightning strike, a choice in an encounter. Letting the attrition tick
    // grind you to the brink while reserving the last point for an actual
    // event is the arc the design asks for.
    //
    // A survivor pinned at LL 1 by exposure is not safe -- effectiveMP still
    // sags, and now literally anything else finishes them.
    uint8_t prevLL = p.ll;  // snapshot before changes to compute actual delta
    int  expLoss    = -expDelta;            // 0 or 1; exposure's own contribution
    int  otherDelta = llDelta - expDelta;   // food/water losses plus the rest heal
    bool downed     = false;

    if (otherDelta < 0) {
      for (int i = 0; i < -otherDelta; i++) {
        if (p.ll > 0) p.ll--;
        if (p.ll == 0) { downed = true; break; }
      }
    } else if (otherDelta > 0) {
      p.ll = (uint8_t)min((int)p.ll + otherDelta, (int)effectiveMaxLL(pid));
    }

    if (!downed && expLoss > 0) {
      // Take up to expLoss points, but never the last one. Report what
      // actually landed rather than what was owed, so the dawn event's expd
      // stays an honest record -- causes.py attributes deaths from it.
      int landed = 0;
      for (int i = 0; i < expLoss && p.ll > 1; i++) { p.ll--; landed++; }
      expDelta = (int8_t)-landed;
    } else if (downed) {
      expDelta = 0;                // food/water got there first
    }

    if (downed) {
      GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
      enqEvt(devt);
    }
    int8_t actualDelta = (int8_t)((int)p.ll - (int)prevLL); // true LL change (clamped)
    if (actualDelta < 0) { ledFlash(255, 0, 0); k10Play(MOTIF_GROSS_SLUDGE); }  // red = LL lost

    // ── Reset daily move budget and action flags ────────────────────────────
    p.movesLeft    = (p.ll == 0) ? 0 : (int8_t)effectiveMP(pid);  // downed: no moves
    p.resting      = false;
    // Apply equipped item operating costs (fuel-gated MP bonuses added here).
    // Returns the slots that could not pay, so EVT_DAWN can report it.
    uint8_t unfuelled = applyDawnItemCosts(pid);


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
    ev.dawnAirD    = airDelta;
    ev.dawnWndMin  = p.wounds[WOUND_MINOR];
    ev.dawnWndMaj  = p.wounds[WOUND_MAJOR];
    ev.dawnUnfuelled = unfuelled;
    enqEvt(ev);
  }
}

// ── Resource collection ───────────────────────────────────────────────────────
// Failure reason codes (sent in EVT_COLLECT_FAIL.amt as a reason byte)
static constexpr uint8_t COL_FAIL_DESYNC   = 1; // server has no resource here (client/server out of sync)
static constexpr uint8_t COL_FAIL_INV_FULL = 2; // inventory at cap

// depth picks the board: 0 = G.map, 1 = G.tunnel (bunker tunnels). It also
// rides the EVT_COLLECT / EVT_COLLECT_FAIL events so the client patches the
// cell on the board the player is actually standing on.
static void collectResource(int pid, int q, int r, uint8_t depth = 0) {
  HexCell& cell = depth ? G.tunnel[r][q] : G.map[r][q];
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
    ev.depth = depth;
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
    ev.q = (int16_t)q; ev.r = (int16_t)r; ev.depth = depth;
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
  // Underground the board is a different size and does NOT wrap, so an
  // off-board neighbour is simply not a legal move. Collapsed Tunnel (15) is
  // MC 255, so canEnterTerrain() refuses it for free and the client greys the
  // button without any tunnel-specific client code.
  if (p.depth) {
    for (int d = 0; d < 6; d++) {
      int nq = p.tq + DQ[d], nr = p.tr + DR[d];
      if (!tunIn(nq, nr)) continue;
      if (canEnterTerrain(pid, G.tunnel[nr][nq].terrain, nullptr)) mask |= (1 << d);
    }
    return mask;
  }
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(p.q + DQ[d]);
    int nr = wrapR(p.r + DR[d]);
    if (canEnterTerrain(pid, G.map[nr][nq].terrain, nullptr)) mask |= (1 << d);
  }
  return mask;
}

// -- Tunnel move -------------------------------------------------------------
// Sibling of movePlayer() rather than a parameterisation of it: underground the
// rules genuinely differ. No weather penalty (WEATHER_INTENSITY is zero down
// there anyway), no flood penalty, no tire tracks (nothing drives a bunker
// corridor), no radiation check (TERRAIN_IS_RAD[14] is 0). What it keeps is
// everything the client depends on -- canEnterTerrain(), the MOVE_CD_MS * mc
// cooldown, the MP deduction, footprints, first-visit score and the EVT_MOVE
// shape -- so the existing D-pad / keyboard / swipe handlers work unchanged.
//
// p.q/p.r are NOT touched: they stay pinned to the hatch this player descended
// through. See the invariant note at the top of tunnels.hpp.
static void moveTunnel(int pid, int dir) {
  if (dir < 0 || dir > 5) return;
  Player& p = G.players[pid];
  if (p.ll == 0) return;               // downed -- the downed path surfaces them
  if (encounters[pid].active) return;  // locked during an active encounter
  if (p.resting) return;

  int nq = p.tq + DQ[dir];
  int nr = p.tr + DR[dir];
  if (!tunIn(nq, nr)) return;          // the tunnel board does not wrap: that is a wall

  uint8_t destTerrain = G.tunnel[nr][nq].terrain;
  uint8_t mc          = 0;
  if (!canEnterTerrain(pid, destTerrain, &mc)) return;  // Collapsed Tunnel (15) is MC 255

  // Guide trait (archetype 0), same as the surface: a companion moving onto the
  // hex a Guide occupies follows their line and pays MC-1. Compares depth as
  // well as position -- a Guide standing on the hatch overhead is not down here.
  if (p.archetype != 0) {
    for (int g = 0; g < MAX_PLAYERS; g++) {
      if (g == pid || !G.players[g].connected) continue;
      if (G.players[g].archetype != 0 || G.players[g].ll == 0) continue;
      if (G.players[g].depth != 1) continue;
      if ((int)G.players[g].tq == nq && (int)G.players[g].tr == nr) {
        mc = (uint8_t)max(1, (int)mc - 1);
        break;
      }
    }
  }

  if (p.movesLeft == 0) return;

  uint32_t cd  = (uint32_t)MOVE_CD_MS * mc;
  uint32_t now = millis();
  if (now - p.lastMoveMs < cd) return;   // cooldown -- silent, same as the surface
  p.lastMoveMs = now;

  p.tq = (int16_t)nq;
  p.tr = (int16_t)nr;
  p.steps++;

  HexCell& cell = G.tunnel[p.tr][p.tq];
  bool firstVisit = !(cell.footprints & (1 << pid));
  if (firstVisit) p.score = (uint16_t)min((int)p.score + 1, 65535);
  int8_t exploGain = firstVisit ? 1 : 0;
  cell.footprints |= (1 << pid);

  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - (int)mc);

  { GameEvent ev = {}; ev.type = EVT_MOVE; ev.pid = (uint8_t)pid;
    ev.q = p.tq; ev.r = p.tr; ev.depth = 1;
    ev.radD = 0; ev.radR = p.radiation;
    ev.exploD = exploGain;
    ev.moveMP = p.movesLeft;
    ev.amt = 0;                        // no tire tracks underground
    enqEvt(ev); }
  collectResource(pid, p.tq, p.tr, 1);
  // Landing on a shaft climbs out (tunnels.hpp).  After the pickup, so the
  // last thing you grab on the way past still lands in the pack.
  tunnelStepUp(pid);
}

// ── Player move ───────────────────────────────────────────────────────────────
static void movePlayer(int pid, int dir) {
  if (dir < 0 || dir > 5) return;
  Player& p  = G.players[pid];
  if (p.depth) { moveTunnel(pid, dir); return; }  // underground: different board, different rules
  if (p.ll == 0) return;  // downed — waiting for slot reset
  if (encounters[pid].active)  return;  // locked during active encounter
  if (p.resting) return;
  int     nq = wrapQ(p.q + DQ[dir]);
  int     nr = wrapR(p.r + DR[dir]);

  uint8_t destTerrain = G.map[nr][nq].terrain;
  uint8_t mc          = 0;
  if (!canEnterTerrain(pid, destTerrain, &mc)) return;

  // ── Weather movement penalty ─────────────────────────────────────────────
  mc = (uint8_t)min(255, (int)mc + (int)WEATHER_MOVE_PENALTY[G.weatherPhase] + floodMovePenalty(nq, nr));

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

  // Vehicle tracks — same shared "someone drove through here" mark the
  // caravan leaves in tickCaravan() (world-system.hpp), triggered here by
  // whichever equipped item is flagged items.cfg "tracks" (the Motorbike).
  // Carried on the EVT_MOVE event below (ev.amt) so every client — not just
  // this player's own vis-disk — can stamp it onto an already-revealed hex,
  // the same reason footprints ride that event instead of waiting on vision.
  bool laidTrack = hasTireTracks(pid);
  if (laidTrack) G.map[p.r][p.q].tireTrack = 1;

  // Deduct movement cost from daily MP budget
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - (int)mc);
  wOnPlayerAction(p.q, p.r, (uint8_t)mc);  // travelling lays a scent track too (world-system-spec.md)

  // ── Chem storm travel ─────────────────────────────────────────────────────
  // In a chem storm you get CHEM_FREE_MOVES hexes to reach cover. Every hex
  // after that, still in the open, costs 1 LL — so crossing a storm on foot
  // is not a plan, it is a death. Standing still is free; the per-tick hazard
  // in tickGame() handles simply being caught out.
  //
  // The move that ARRIVES under cover is always free, however many you have
  // already spent: the dash that saves you is never the one that kills you.
  // Cover means the same thing here as it does to the per-tick hazard — any
  // built shelter, a Settlement, or Broken Urban.
  if (G.weatherPhase == WEATHER_CHEM) {
    bool destCovered = (G.map[p.r][p.q].shelter >= 1)
                    || destTerrain == 9
                    || TERRAIN_IS_RUINS[destTerrain];
    if (!destCovered) {
      if (p.chemMoves < 255) p.chemMoves++;
      if (p.chemMoves > CHEM_FREE_MOVES && p.ll > 0) {
        p.ll--;
        ledFlash(0, 100, 0);
        k10Play(MOTIF_ACID_DRIP);
        if (p.ll == 0) {
          p.movesLeft = 0;
          GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)pid;
          dev.evWsId = p.wsClientId; enqEvt(dev);
        }
      }
    }
  }

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
    ev.amt = laidTrack ? 1 : 0;  // EVT_MOVE-specific reuse of the generic amt field: 1 = this step laid a tire track
    enqEvt(ev); }
  collectResource(pid, p.q, p.r);
  // Landing on a Bunker Entrance / Vent Shaft drops you into the tunnels
  // (tunnels.hpp).  Last, so the surface EVT_MOVE reaches clients before the
  // EVT_TUNNEL_ENTER that follows it.
  tunnelStepDown(pid);
}
