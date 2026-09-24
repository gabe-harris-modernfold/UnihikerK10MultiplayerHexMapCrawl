#pragma once
// ── Encounter message handlers: enc_start, enc_choice, enc_bank, enc_abort ───
// The server is authoritative: the client sends only which hex it is on, which
// choice index it picked, and — on enc_bank — how much of the haul it wants to
// keep.  That last one is a request, clamped against pendingLoot[] here; the
// loot itself is still rolled server-side.  See encounter_engine.hpp.

static void handleMsg_enc_start(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  // The q/r check is the one docs/bot-testing.md calls out: {"t":"enc_start"}
  // alone used to be discarded here with no reply at all.
  const char* qp = strstr(data, "\"q\""); if (!qp) { wsNack(client, "parse"); return; }
  const char* qv = strchr(qp + 3, ':');  if (!qv) { wsNack(client, "parse"); return; }
  const char* rp = strstr(data, "\"r\""); if (!rp) { wsNack(client, "parse"); return; }
  const char* rv = strchr(rp + 3, ':');  if (!rv) { wsNack(client, "parse"); return; }
  int hq = atoi(qv + 1), hr = atoi(rv + 1);
  // Bounds are checked against whichever board the player is on, below --
  // the tunnel board is 16x10, not 75x57. Reject obvious garbage here.
  if (hq < 0 || hr < 0) { wsNack(client, "bad_arg"); return; }

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"mutex_timeout\"}");
    wsNack(client, "busy");
    return;
  }
  int pid = findSlot(client->id());
  if (pid < 0) {
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"no_slot\"}");
    wsNack(client, "not_seated");
  } else if (encounters[pid].active) {
    client->text("{\"t\":\"err\",\"msg\":\"Already in an encounter — abort first\"}");
    wsNack(client, "in_enc");
  } else if (G.players[pid].ll == 0) {
    client->text("{\"t\":\"err\",\"msg\":\"Cannot enter — you are downed\"}");
    wsNack(client, "downed");
  } else {
    Player&  p    = G.players[pid];
    bool     below = (p.depth != 0);
    bool     inBounds = below ? tunIn(hq, hr)
                              : (hq < MAP_COLS && hr < MAP_ROWS);
    if (!inBounds) {
      client->text("{\"t\":\"err\",\"msg\":\"No such hex\"}");
      wsNack(client, "bad_arg");
      xSemaphoreGive(G.mutex);
      return;
    }
    HexCell& cell = below ? G.tunnel[hr][hq] : G.map[hr][hq];
    bool claimed = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (i == pid || !encounters[i].active) continue;
      // Depth too: a tunnel hex and a surface hex can share coordinates.
      if (G.players[i].depth != p.depth) continue;
      if (encounters[i].hexQ == (uint8_t)hq && encounters[i].hexR == (uint8_t)hr) { claimed = true; break; }
    }
    uint8_t terrain = cell.terrain;
    int myQ = below ? (int)p.tq : (int)p.q;
    int myR = below ? (int)p.tr : (int)p.r;
    if (myQ != hq || myR != hr) {
      client->text("{\"t\":\"err\",\"msg\":\"Not at that hex\"}");
      wsNack(client, "not_here");
    } else if (cell.poi == 0) {
      client->text("{\"t\":\"err\",\"msg\":\"Already looted\"}");
      wsNack(client, "no_poi");
    } else if (claimed) {
      client->text("{\"t\":\"err\",\"msg\":\"Another survivor is already inside\"}");
      wsNack(client, "claimed");
    } else if (terrain >= NUM_TERRAIN || encPools[terrain].count == 0) {
      client->text("{\"t\":\"err\",\"msg\":\"No encounters here\"}");
      wsNack(client, "no_pool");
    } else {
      uint8_t idx = cell.poi;
      const char* json = encLoadFile(terrain, idx);
      if (!json) {
        client->text("{\"t\":\"err\",\"msg\":\"The way in is blocked\"}");
        wsNack(client, "load_failed");
      } else {
        cell.poi = 0;  // consume POI; restored if the encounter ends involuntarily
        if (G.threatClock < 20) G.threatClock++;
        ActiveEncounter& enc = encounters[pid];
        enc = {};
        enc.active  = 1;
        enc.encIdx  = idx;
        enc.hexQ    = (uint8_t)hq;
        enc.hexR    = (uint8_t)hr;
        enc.depth   = below ? 1 : 0;   // which board hexQ/hexR index
        enc.terrain = terrain;
        encEnterStartNode(json, enc);
        char pathBuf[72];
        int pathLen = snprintf(pathBuf, sizeof(pathBuf),
          "{\"t\":\"enc_path\",\"biome\":\"%s\",\"id\":%d}", encPools[terrain].path, (int)idx);
        client->text(pathBuf, (size_t)pathLen);
        GameEvent ev = {};
        ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
        ev.q = (int16_t)hq; ev.r = (int16_t)hr;
        enqEvt(ev);
      }
    }
  }
  xSemaphoreGive(G.mutex);
}

// Place a typed item into the player's pack via addItemToInv() (stacks first,
// then empty slots — the same rule pickups and crafting use); overflow goes
// to the ground at the player's hex.  Caller holds G.mutex.
static void grantItemOrDrop(Player& p, uint8_t itemId, uint8_t qty) {
  if (!itemId || !qty) return;
  qty = (uint8_t)(qty - addItemToInv(p, itemId, qty));
  if (!qty) return;
  // Pack full — drop the remainder where the player stands
  int gslot = -1;
  for (int g = 0; g < MAX_GROUND; g++) {
    if (groundItems[g].itemType == itemId && groundItems[g].q == p.q && groundItems[g].r == p.r) { gslot = g; break; }
  }
  if (gslot < 0) for (int g = 0; g < MAX_GROUND; g++) if (!groundItems[g].itemType) { gslot = g; break; }
  if (gslot < 0) { Log.warning("grantItemOrDrop: ground full, item %d x%d lost", (int)itemId, (int)qty); return; }
  groundItems[gslot].q = p.q; groundItems[gslot].r = p.r;
  groundItems[gslot].itemType = itemId;
  groundItems[gslot].qty = (uint8_t)min(255, (int)groundItems[gslot].qty + (int)qty);
}

static void handleMsg_enc_choice(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  // {"t":"enc_choice","ci":N}
  const char* cp = strstr(data, "\"ci\""); if (!cp) { wsNack(client, "parse"); return; }
  const char* cv = strchr(cp + 4, ':');   if (!cv) { wsNack(client, "parse"); return; }
  int ci = atoi(cv + 1);
  if (ci < 0 || ci > 15) { wsNack(client, "bad_arg"); return; }

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) { wsNack(client, "busy"); return; }
  int pid = findSlot(client->id());
  if (pid < 0 || !encounters[pid].active || G.players[pid].ll == 0) {
    xSemaphoreGive(G.mutex);
    wsNack(client, pid < 0 ? "not_seated" : !encounters[pid].active ? "no_enc" : "downed");
    return;
  }
  Player&          p   = G.players[pid];
  ActiveEncounter& enc = encounters[pid];

  const char* json = encLoadFile(enc.terrain, enc.encIdx);
  EncChoice ch;
  if (!json || !encResolveChoice(json, enc.nodeKey, ci, ch)) {
    Log.warning("enc_choice pid=%d node=%s ci=%d: not found", pid, enc.nodeKey, ci);
    client->text("{\"t\":\"err\",\"msg\":\"That choice is not open to you\"}");
    wsNack(client, "no_choice");
    xSemaphoreGive(G.mutex); return;
  }

  bool canAfford = (p.ll >= ch.costLL) && (p.radiation + ch.costRad <= 10) &&
                   (p.inv[1] >= ch.costFood) && (p.inv[0] >= ch.costWat) &&
                   (p.inv[4] >= ch.costScrap) && (p.inv[3] >= ch.costMed);
  if (!canAfford) {
    client->text("{\"t\":\"err\",\"msg\":\"Cannot afford cost\"}");
    wsNack(client, "no_res");
    xSemaphoreGive(G.mutex); return;
  }
  // Deduct costs (a negative cost is a gain; LL gains respect the ceiling)
  if (ch.costLL) { p.ll = (uint8_t)constrain((int)p.ll - ch.costLL, 0, (int)effectiveMaxLL(pid)); if (p.ll == 0) p.movesLeft = 0; }
  p.radiation = (uint8_t)constrain((int)p.radiation + ch.costRad, 0, 10);
  p.inv[1]    = (uint8_t)constrain((int)p.inv[1] - ch.costFood,  0, 99);
  p.inv[0]    = (uint8_t)constrain((int)p.inv[0] - ch.costWat,   0, 99);
  p.inv[4]    = (uint8_t)constrain((int)p.inv[4] - ch.costScrap, 0, 99);
  p.inv[3]    = (uint8_t)constrain((int)p.inv[3] - ch.costMed,   0, 99);
  const bool costKilled = (p.ll == 0);   // the price of the choice took the last LL

  uint8_t dn = computeEncounterDN(pid, (uint8_t)ch.baseRisk, ch.skill);
  CheckResult cr = resolveCheck(pid, ch.skill, dn, 0);
  GameEvent ev = {};
  ev.type      = EVT_ENC_RESULT;
  ev.pid       = (uint8_t)pid;
  ev.encSkill  = ch.skill;
  ev.encDN     = dn;
  ev.encTotal  = (int8_t)cr.total;
  ev.encOut    = cr.success ? 1 : 0;
  bool encounterEnded = false;
  if (cr.success) {
    for (int i = 0; i < 5; i++) {
      enc.pendingLoot[i] = (uint8_t)min(99, (int)enc.pendingLoot[i] + (int)ch.loot[i]);
      ev.encLoot[i] = ch.loot[i];
    }
    // Typed items: explicit "item" loot entries first, then the loot-table roll.
    uint8_t itm[3] = { ch.itemType[0], ch.itemType[1], 0 };
    uint8_t qty[3] = { ch.itemQty[0],  ch.itemQty[1],  0 };
    if (ch.lootTable[0]) rollLootTable(ch.lootTable, &itm[2], &qty[2]);
    int shown = 0;
    for (int k = 0; k < 3; k++) {
      if (!itm[k] || !qty[k] || enc.pendingItemCount >= ENC_MAX_ITEMS) continue;
      enc.pendingItemType[enc.pendingItemCount] = itm[k];
      enc.pendingItemQty[enc.pendingItemCount]  = qty[k];
      enc.pendingItemCount++;
      if (shown == 0)      { ev.encItemType  = itm[k]; ev.encItemQty  = qty[k]; }
      else if (shown == 1) { ev.encItemType2 = itm[k]; ev.encItemQty2 = qty[k]; }
      shown++;
    }
    // A recipe is a one-time knowledge grant, pending like the loot above
    // until the player banks — see handleMsg_enc_bank. OR'd into a bitmask
    // (not overwritten) since a single scene can walk through several nodes,
    // each granting a different recipe, before ever banking.
    // Bound the id before the shift (1u << 32+ is UB) — content is hand-authored
    // and should never exceed MAX_RECIPES, but this is parsed from a data file.
    if (ch.recipeId && ch.recipeId <= MAX_RECIPES) {
      enc.pendingRecipes |= (1u << (ch.recipeId - 1));
      ev.encRecipe = ch.recipeId;
    }
    // Advance to the destination node
    strncpy(enc.nodeKey, ch.nextKey, ENC_KEY_LEN - 1); enc.nodeKey[ENC_KEY_LEN - 1] = 0;
    enc.canBank = ch.nextCanBank ? 1 : 0;
    if (ch.nextTerminal) enc.active |= (1 << 7);
    // cost_ll can take the last point of LL even on a success.
    if (p.ll == 0) encounterEnded = true;
  } else {
    ev.encPenLL  = (int8_t)ch.hazLL;
    ev.encPenRad = (int8_t)ch.hazRad;
    ev.encEnds   = ch.hazEnds ? 1 : 0;
    if (ch.hazLL > 0) {
      p.ll = (uint8_t)min((int)p.ll + ch.hazLL, (int)effectiveMaxLL(pid));
    } else if (ch.hazLL < 0) {
      p.ll = (uint8_t)max(0, (int)p.ll + ch.hazLL);
      if (p.ll == 0) { p.movesLeft = 0; ledFlash(255, 0, 0); }
    }
    p.radiation = (uint8_t)constrain((int)p.radiation + ch.hazRad, 0, 10);
    for (int i = 0; i < 5; i++) {
      int take = min((int)ch.hazRes[i], (int)p.inv[i]);
      p.inv[i] = (uint8_t)(p.inv[i] - take);
      ev.encPenRes[i] = (uint8_t)take;
    }
    ev.encPenWndMin = addWound(p, WOUND_MINOR, ch.hazWMin);
    ev.encPenWndMaj = addWound(p, WOUND_MAJOR, ch.hazWMaj);
    // A major wound costs MP for the rest of the day, not just from dawn.
    if (ev.encPenWndMaj) p.movesLeft = (int8_t)max(0, (int)p.movesLeft - (int)ev.encPenWndMaj);
    encounterEnded = (p.ll == 0) || ch.hazEnds;
    // Auto-drain co-located allies: major hazard (LL loss) costs 2, minor costs 1
    int drainAmt = (ch.hazLL < 0) ? 2 : 1;
    for (int ally = 0; ally < MAX_PLAYERS; ally++) {
      if (ally == pid || !G.players[ally].connected) continue;
      if ((int)G.players[ally].q != (int)p.q || (int)G.players[ally].r != (int)p.r) continue;
      int drained = 0;
      for (int ri = 0; ri < 5 && drained < drainAmt; ri++) {
        if (G.players[ally].inv[ri] > 0) { G.players[ally].inv[ri]--; drained++; }
      }
      ev.encDrains[ally] = (uint8_t)drained;
    }
  }
  // Result must be broadcast before EVT_ENC_END so allies see the outcome before the encounter clears.
  enqEvt(ev);
  if (encounterEnded) {
    if (p.ll == 0) {
      GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
      devt.res = (costKilled || cr.success) ? DC_ENC_COST : DC_ENC_HAZARD;
      enqEvt(devt);
    }
    endEncounter(pid, (p.ll == 0) ? ENC_END_DOWNED : ENC_END_HAZARD, /*restorePoi=*/false);
  }
  xSemaphoreGive(G.mutex);
}

static void handleMsg_enc_bank(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  // Built inside the mutex below (if any items were pending) and sent after
  // it's released — same static-ack pattern as handleMsg_act's craft path.
  PSRAM_STATIC(char, itemAck, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  itemAck[0] = '\0';
  // Optional trim: {"t":"enc_bank","keep":[w,f,fu,m,s]} — how much of each
  // resource the player chose to take on the haul tray's steppers.  An absent
  // or malformed "keep" means "bank everything", which is what an older client
  // sends, so the default is wide open; every entry is clamped against
  // pendingLoot[] below, so this can only ever take less than was won.
  // Same array idiom as car_buy's "give" (network-msg-trade.hpp).
  uint8_t keep[5] = { 99, 99, 99, 99, 99 };
  const char* kp = strstr(data, "\"keep\"");
  if (kp) {
    const char* kb = strchr(kp + 6, '[');
    if (kb) {
      kb++;
      for (int i = 0; i < 5; i++) {
        while (*kb == ' ') kb++;
        keep[i] = (uint8_t)constrain(atoi(kb), 0, 99);
        const char* nx = strchr(kb, i < 4 ? ',' : ']');
        if (!nx) break;
        kb = nx + 1;
      }
    }
  }
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) { wsNack(client, "busy"); return; }
  bool banked = false;
  int pid = findSlot(client->id());
  if (pid < 0)                         wsNack(client, "not_seated");
  else if (!encounters[pid].active)    wsNack(client, "no_enc");
  if (pid >= 0 && encounters[pid].active) {
    Player& p = G.players[pid];
    ActiveEncounter& enc = encounters[pid];
    bool fullClear = (enc.active & (1 << 7)) != 0;
    if (!fullClear && !enc.canBank) {
      client->text("{\"t\":\"err\",\"msg\":\"You can't carry loot out from here\"}");
      wsNack(client, "cannot_bank");
    } else {
      int totalRes = 0;
      for (int i = 0; i < 5; i++) {
        // Take the smaller of what was rolled and what was asked for.  Writing
        // the result back into pendingLoot keeps the EVT_ENC_BANK memcpy below
        // honest: the event must report what was actually banked, or the
        // client's _evEncBank would add the untrimmed amount to its inv[].
        // What's left over is simply left behind — no ground drop, no refund.
        int take = min((int)enc.pendingLoot[i], (int)keep[i]);
        p.inv[i] = (uint8_t)min(99, (int)p.inv[i] + take);
        enc.pendingLoot[i] = (uint8_t)take;
        totalRes += take;
      }
      for (int j = 0; j < enc.pendingItemCount; j++)
        grantItemOrDrop(p, enc.pendingItemType[j], enc.pendingItemQty[j]);
      p.knownRecipes |= enc.pendingRecipes;
      // grantItemOrDrop() just mutated invType[]/invQty[], which — like
      // use_item/equip_item/craft — is private state never carried by the
      // broadcastState() tick or the 'ev' broadcast below. Without this
      // targeted snapshot the banked item sits in the player's save-state
      // but never reaches their on-screen pack until the next full sync.
      if (enc.pendingItemCount) {
        int ap = appendFmt(itemAck, sizeof(itemAck), 0,
          "{\"t\":\"item_result\",\"ok\":true,\"act\":\"enc_bank\",\"pid\":%d,", pid);
        ap = appendPackArrays(itemAck, sizeof(itemAck), ap, pid);
        appendFmt(itemAck, sizeof(itemAck), ap, "}");
      }
      int scoreGain = totalRes * 3 + (fullClear ? 10 : 0);
      GameEvent ev = {};
      ev.type = EVT_ENC_BANK; ev.pid = (uint8_t)pid;
      ev.q = (int16_t)enc.hexQ; ev.r = (int16_t)enc.hexR;
      memcpy(ev.encLoot, enc.pendingLoot, 5);
      ev.bankedRecipes = enc.pendingRecipes;
      addScore(p, ev, scoreGain);
      p.encCount++;
      enqEvt(ev);
      enc = {};
      banked = true;
    }
  }
  xSemaphoreGive(G.mutex);
  // Persist outside the mutex (saveGame takes it itself) — same pattern as
  // handleMsg_act's craft path. The POI was consumed when the scene opened,
  // so any save between then and now (another survivor's craft/use_item, a
  // disconnect) already holds the emptied hex; without a save here a reboot
  // would keep that empty hex yet forget the loot, items and — worst of all —
  // the one-time recipes that were just banked from it.
  if (banked) saveGame();
  if (itemAck[0]) client->text(itemAck);
}

static void handleMsg_enc_abort(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) { wsNack(client, "busy"); return; }
  int pid = findSlot(client->id());
  if (pid >= 0 && encounters[pid].active) {
    if (G.threatClock < 20) G.threatClock++;
    endEncounter(pid, ENC_END_ABORT, /*restorePoi=*/false);  // walking away closes the place for good
  } else {
    wsNack(client, pid < 0 ? "not_seated" : "no_enc");
  }
  xSemaphoreGive(G.mutex);
}
