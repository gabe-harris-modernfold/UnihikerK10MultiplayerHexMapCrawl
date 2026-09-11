#pragma once
// ── Encounter message handlers: enc_start, enc_choice, enc_bank, enc_abort ───
// The server is authoritative: the client sends only which hex it is on and
// which choice index it picked.  See encounter_engine.hpp.

static void handleMsg_enc_start(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* qp = strstr(data, "\"q\""); if (!qp) return;
  const char* qv = strchr(qp + 3, ':');  if (!qv) return;
  const char* rp = strstr(data, "\"r\""); if (!rp) return;
  const char* rv = strchr(rp + 3, ':');  if (!rv) return;
  int hq = atoi(qv + 1), hr = atoi(rv + 1);
  if (hq < 0 || hq >= MAP_COLS || hr < 0 || hr >= MAP_ROWS) return;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"mutex_timeout\"}");
    return;
  }
  int pid = findSlot(client->id());
  if (pid < 0) {
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"no_slot\"}");
  } else if (encounters[pid].active) {
    client->text("{\"t\":\"err\",\"msg\":\"Already in an encounter — abort first\"}");
  } else if (G.players[pid].ll == 0) {
    client->text("{\"t\":\"err\",\"msg\":\"Cannot enter — you are downed\"}");
  } else {
    Player&  p    = G.players[pid];
    HexCell& cell = G.map[hr][hq];
    bool claimed = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (i == pid || !encounters[i].active) continue;
      if (encounters[i].hexQ == (uint8_t)hq && encounters[i].hexR == (uint8_t)hr) { claimed = true; break; }
    }
    uint8_t terrain = cell.terrain;
    if ((int)p.q != hq || (int)p.r != hr) {
      client->text("{\"t\":\"err\",\"msg\":\"Not at that hex\"}");
    } else if (cell.poi == 0) {
      client->text("{\"t\":\"err\",\"msg\":\"Already looted\"}");
    } else if (claimed) {
      client->text("{\"t\":\"err\",\"msg\":\"Another survivor is already inside\"}");
    } else if (terrain >= 10 || encPools[terrain].count == 0) {
      client->text("{\"t\":\"err\",\"msg\":\"No encounters here\"}");
    } else {
      uint8_t idx = cell.poi;
      const char* json = encLoadFile(terrain, idx);
      if (!json) {
        client->text("{\"t\":\"err\",\"msg\":\"The way in is blocked\"}");
      } else {
        cell.poi = 0;  // consume POI; restored if the encounter ends involuntarily
        if (G.threatClock < 20) G.threatClock++;
        ActiveEncounter& enc = encounters[pid];
        enc = {};
        enc.active  = 1;
        enc.encIdx  = idx;
        enc.hexQ    = (uint8_t)hq;
        enc.hexR    = (uint8_t)hr;
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

// Place a typed item into the player's pack (stack first, then a free slot);
// overflow goes to the ground at the player's hex.  Caller holds G.mutex.
static void grantItemOrDrop(Player& p, uint8_t itemId, uint8_t qty) {
  if (!itemId || !qty) return;
  const ItemDef* def = getItemDef(itemId);
  uint8_t cap   = def ? def->maxStack : 1;
  uint8_t slots = effectiveInvSlots(p);
  for (int s = 0; s < slots && qty; s++) {
    if (p.invType[s] != itemId || p.invQty[s] >= cap) continue;
    uint8_t add = min(qty, (uint8_t)(cap - p.invQty[s]));
    p.invQty[s] += add; qty -= add;
  }
  for (int s = 0; s < slots && qty; s++) {
    if (p.invType[s]) continue;
    uint8_t add = min(qty, cap);
    p.invType[s] = itemId; p.invQty[s] = add; qty -= add;
  }
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
  const char* cp = strstr(data, "\"ci\""); if (!cp) return;
  const char* cv = strchr(cp + 4, ':');   if (!cv) return;
  int ci = atoi(cv + 1);
  if (ci < 0 || ci > 15) return;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  int pid = findSlot(client->id());
  if (pid < 0 || !encounters[pid].active || G.players[pid].ll == 0) { xSemaphoreGive(G.mutex); return; }
  Player&          p   = G.players[pid];
  ActiveEncounter& enc = encounters[pid];

  const char* json = encLoadFile(enc.terrain, enc.encIdx);
  EncChoice ch;
  if (!json || !encResolveChoice(json, enc.nodeKey, ci, ch)) {
    Log.warning("enc_choice pid=%d node=%s ci=%d: not found", pid, enc.nodeKey, ci);
    client->text("{\"t\":\"err\",\"msg\":\"That choice is not open to you\"}");
    xSemaphoreGive(G.mutex); return;
  }

  bool canAfford = (p.ll >= ch.costLL) && (p.radiation + ch.costRad <= 10) &&
                   (p.inv[1] >= ch.costFood) && (p.inv[0] >= ch.costWat) &&
                   (p.inv[4] >= ch.costScrap) && (p.inv[3] >= ch.costMed);
  if (!canAfford) {
    client->text("{\"t\":\"err\",\"msg\":\"Cannot afford cost\"}");
    xSemaphoreGive(G.mutex); return;
  }
  // Deduct costs (a negative cost is a gain; LL gains respect the ceiling)
  if (ch.costLL) { p.ll = (uint8_t)constrain((int)p.ll - ch.costLL, 0, (int)effectiveMaxLL(pid)); if (p.ll == 0) p.movesLeft = 0; }
  p.radiation = (uint8_t)constrain((int)p.radiation + ch.costRad, 0, 10);
  p.inv[1]    = (uint8_t)constrain((int)p.inv[1] - ch.costFood,  0, 99);
  p.inv[0]    = (uint8_t)constrain((int)p.inv[0] - ch.costWat,   0, 99);
  p.inv[4]    = (uint8_t)constrain((int)p.inv[4] - ch.costScrap, 0, 99);
  p.inv[3]    = (uint8_t)constrain((int)p.inv[3] - ch.costMed,   0, 99);

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
      enqEvt(devt);
    }
    endEncounter(pid, (p.ll == 0) ? ENC_END_DOWNED : ENC_END_HAZARD, /*restorePoi=*/false);
  }
  xSemaphoreGive(G.mutex);
}

static void handleMsg_enc_bank(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  int pid = findSlot(client->id());
  if (pid >= 0 && encounters[pid].active) {
    Player& p = G.players[pid];
    ActiveEncounter& enc = encounters[pid];
    bool fullClear = (enc.active & (1 << 7)) != 0;
    if (!fullClear && !enc.canBank) {
      client->text("{\"t\":\"err\",\"msg\":\"You can't carry loot out from here\"}");
    } else {
      int totalRes = 0;
      for (int i = 0; i < 5; i++) {
        p.inv[i] = (uint8_t)min(99, (int)p.inv[i] + (int)enc.pendingLoot[i]);
        totalRes += enc.pendingLoot[i];
      }
      for (int j = 0; j < enc.pendingItemCount; j++)
        grantItemOrDrop(p, enc.pendingItemType[j], enc.pendingItemQty[j]);
      int scoreGain = totalRes * 3 + (fullClear ? 10 : 0);
      GameEvent ev = {};
      ev.type = EVT_ENC_BANK; ev.pid = (uint8_t)pid;
      ev.q = (int16_t)enc.hexQ; ev.r = (int16_t)enc.hexR;
      memcpy(ev.encLoot, enc.pendingLoot, 5);
      addScore(p, ev, scoreGain);
      p.encCount++;
      enqEvt(ev);
      enc = {};
    }
  }
  xSemaphoreGive(G.mutex);
}

static void handleMsg_enc_abort(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  int pid = findSlot(client->id());
  if (pid >= 0 && encounters[pid].active) {
    if (G.threatClock < 20) G.threatClock++;
    endEncounter(pid, ENC_END_ABORT, /*restorePoi=*/false);  // walking away closes the place for good
  }
  xSemaphoreGive(G.mutex);
}
