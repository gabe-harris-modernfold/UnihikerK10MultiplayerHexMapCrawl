#pragma once
// ── Encounter message handlers: enc_start, enc_choice, enc_bank, enc_abort ───

static void handleMsg_enc_start(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* qp = strstr(data, "\"q\""); if (!qp) return;
  const char* qv = strchr(qp + 3, ':');  if (!qv) return;
  const char* rp = strstr(data, "\"r\""); if (!rp) return;
  const char* rv = strchr(rp + 3, ':');  if (!rv) return;
  int hq = atoi(qv + 1), hr = atoi(rv + 1);
  if (hq < 0 || hq >= MAP_COLS || hr < 0 || hr >= MAP_ROWS) return;

  Serial.printf("[ENC] enc_start recv q=%d r=%d  heap=%u  core=%d\n",
    hq, hr, (unsigned)ESP.getFreeHeap(), xPortGetCoreID());
  Serial.flush();
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int pid = findSlot(client->id());
    if (pid >= 0 && !encounters[pid].active && !(G.players[pid].ll == 0)) {
      Player& p     = G.players[pid];
      HexCell& cell = G.map[hr][hq];
      if ((int)p.q != hq || (int)p.r != hr) {
        Serial.printf("[ENC] REJECT 'Not at hex': player at (%d,%d), tried (%d,%d)\n", p.q, p.r, hq, hr);
        client->text("{\"t\":\"err\",\"msg\":\"Not at that hex\"}");
      } else if (cell.poi == 0) {
        Serial.printf("[ENC] REJECT 'Already looted': cell.poi=0 at (%d,%d)\n", hq, hr);
        client->text("{\"t\":\"err\",\"msg\":\"Already looted\"}");
      } else {
        bool claimed = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (i == pid || !encounters[i].active) continue;
          if (encounters[i].hexQ == (uint8_t)hq && encounters[i].hexR == (uint8_t)hr) {
            claimed = true; break;
          }
        }
        if (claimed) {
          client->text("{\"t\":\"err\",\"msg\":\"Another survivor is already inside\"}");
        } else {
          uint8_t terrain = cell.terrain;
          if (terrain >= 10 || encPools[terrain].count == 0) {
            Serial.printf("[ENC] REJECT 'No encounters here': terrain=%d (>=10:%s), pool count=%d, path='%s'\n",
              terrain,
              terrain >= 10 ? "YES" : "no",
              terrain < 10 ? (int)encPools[terrain].count : -1,
              terrain < 10 ? encPools[terrain].path : "N/A");
            client->text("{\"t\":\"err\",\"msg\":\"No encounters here\"}");
          } else {
            uint8_t idx = cell.poi;
            cell.poi = 0;  // consume POI immediately
            if (G.threatClock < 20) G.threatClock++;
            encounters[pid].active           = 1;
            encounters[pid].encIdx           = idx;
            encounters[pid].hexQ             = (uint8_t)hq;
            encounters[pid].hexR             = (uint8_t)hr;
            memset(encounters[pid].pendingLoot,     0, 5);
            memset(encounters[pid].pendingItemType, 0, ENC_MAX_ITEMS);
            memset(encounters[pid].pendingItemQty,  0, ENC_MAX_ITEMS);
            encounters[pid].pendingItemCount = 0;
            char pathBuf[72];
            int pathLen = snprintf(pathBuf, sizeof(pathBuf),
              "{\"t\":\"enc_path\",\"biome\":\"%s\",\"id\":%d}",
              encPools[terrain].path, (int)idx);
            client->text(pathBuf, (size_t)pathLen);
            Serial.printf("[ENC] P%d enters %s/%d at (%d,%d) — enc_path sent\n",
              pid, encPools[terrain].path, (int)idx, hq, hr);
            Serial.flush();
            GameEvent ev = {};
            ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
            ev.q = (int16_t)hq; ev.r = (int16_t)hr;
            enqEvt(ev);
          }
        }
      }
    } else if (pid < 0) {
      Serial.printf("[ENC] enc_start WARN: no slot for client %u\n", client->id());
    }
    xSemaphoreGive(G.mutex);
    Serial.println("[ENC] enc_start mutex released"); Serial.flush();
  } else {
    Serial.printf("[ENC] enc_start MUTEX TIMEOUT (5ms) — game loop busy  heap=%u\n",
      (unsigned)ESP.getFreeHeap());
    Serial.flush();
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"mutex_timeout\"}");
  }
}

static void handleMsg_enc_choice(AsyncWebSocketClient* client, char* data, size_t len) {
  // {"t":"enc_choice","ci":0,"cost_ll":0,"cost_rad":0,"cost_food":0,
  //  "cost_water":0,"cost_resolve":0,"base_risk":35,"skill":2,
  //  "loot":[0,0,0,1,0],"lt":"urban_common","can_bank":true,
  //  "is_terminal":false,
  //  "haz_ll":0,"haz_rad":0,"haz_st":0,"haz_ends":0}
  #define ENC_JP(field, key) const char* field##_p = strstr(data, "\"" key "\""); \
    int field = 0; if (field##_p) { const char* vp = strchr(field##_p + strlen("\"" key "\""), ':'); if (vp) field = atoi(vp + 1); }
  ENC_JP(costLL,  "cost_ll")
  ENC_JP(costRad, "cost_rad")  ENC_JP(costFood, "cost_food")
  ENC_JP(costWat, "cost_water")
  ENC_JP(costScrap, "cost_scrap")
  ENC_JP(baseRisk,"base_risk") ENC_JP(skill,    "skill")
  ENC_JP(isTerm,  "is_terminal")
  ENC_JP(hazLL,   "haz_ll")
  ENC_JP(hazRad,  "haz_rad")
  ENC_JP(hazEnds, "haz_ends") ENC_JP(hazLoseCon, "haz_lose_consumable")
  #undef ENC_JP

  // Parse loot array — indices map directly to p.inv[]: 0=Water 1=Food 2=Fuel 3=Medicine 4=Scrap.
  // NOTE: encounter JSON uses 0-based res indices; map cell.resource uses 1-based. Different spaces.
  uint8_t lootArr[5] = {0};
  const char* lp = strstr(data, "\"loot\"");
  if (lp) { const char* lb = strchr(lp + 6, '['); if (lb) { lb++;
    for (int i = 0; i < 5; i++) {
      while (*lb == ' ') lb++;
      lootArr[i] = (uint8_t)constrain(atoi(lb), 0, 99);
      const char* nx = strchr(lb, i < 4 ? ',' : ']'); if (!nx) break; lb = nx + 1;
    }
  }}
  char ltName[20] = {0};
  const char* ltp = strstr(data, "\"lt\"");
  if (ltp) { const char* lts = strchr(ltp + 4, '"'); if (lts) { lts++;
    const char* lte = strchr(lts + 1, '"');
    if (lte) { int ln = min((int)(lte - lts), 19); strncpy(ltName, lts, ln); ltName[ln] = 0; }
  }}

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int pid = findSlot(client->id());
    if (pid >= 0 && encounters[pid].active && !(G.players[pid].ll == 0)) {
      Player& p = G.players[pid];
      ActiveEncounter& enc = encounters[pid];
      bool canAfford = (p.ll >= costLL) &&
                       (p.radiation + costRad <= 10) &&
                       (p.inv[1] >= costFood) && (p.inv[0] >= costWat) &&
                       (p.inv[4] >= costScrap);
      if (!canAfford) {
        client->text("{\"t\":\"err\",\"msg\":\"Cannot afford cost\"}");
        xSemaphoreGive(G.mutex); return;
      }
      // Deduct costs
      if (costLL)   { p.ll = (uint8_t)max(0, (int)p.ll - costLL); if (p.ll == 0) p.movesLeft = 0; }
      p.radiation= (uint8_t)constrain((int)p.radiation+ costRad, 0, 10);
      p.inv[1]   = (uint8_t)max(0, (int)p.inv[1] - costFood);
      p.inv[0]   = (uint8_t)max(0, (int)p.inv[0] - costWat);
      p.inv[4]   = (uint8_t)max(0, (int)p.inv[4] - costScrap);
      uint8_t dn = computeEncounterDN(pid, (uint8_t)constrain(baseRisk, 0, 100),
                                      (uint8_t)constrain(skill, 0, 5));
      CheckResult cr = resolveCheck(pid, (uint8_t)constrain(skill, 0, 5), dn, 0);
      GameEvent ev = {};
      ev.type      = EVT_ENC_RESULT;
      ev.pid       = (uint8_t)pid;
      ev.encSkill  = (uint8_t)constrain(skill, 0, 5);
      ev.encDN     = dn;
      ev.encTotal  = (int8_t)cr.total;
      ev.encOut    = cr.success ? 1 : 0;
      bool encounterEnded = false;
      if (cr.success) {
        for (int i = 0; i < 5; i++) {
          enc.pendingLoot[i] = (uint8_t)min(99, (int)enc.pendingLoot[i] + (int)lootArr[i]);
          ev.encLoot[i] = lootArr[i];
        }
        if (ltName[0]) {
          uint8_t itm = 0, qty = 0;
          rollLootTable(ltName, &itm, &qty);
          if (itm && qty && enc.pendingItemCount < ENC_MAX_ITEMS) {
            enc.pendingItemType[enc.pendingItemCount] = itm;
            enc.pendingItemQty[enc.pendingItemCount]  = qty;
            enc.pendingItemCount++;
            ev.encItemType = itm; ev.encItemQty = qty;
          }
        }
        if (isTerm) enc.active |= (1 << 7);
      } else {
        ev.encPenLL  = (int8_t)hazLL;
        ev.encPenRad = (int8_t)hazRad;
        ev.encEnds   = (uint8_t)(hazEnds ? 1 : 0);
        if (hazLL > 0) {
          Serial.printf("[ENC] WARNING: P%d haz_ll=%d is positive — LL penalties must be negative in JSON. Ignored.\n", pid, hazLL);
        } else if (hazLL < 0) {
          int newLL = (int)p.ll + hazLL;
          p.ll = (uint8_t)max(0, newLL);
          if (p.ll == 0) { p.movesLeft = 0; ledFlash(255, 0, 0); }
        }
        p.radiation= (uint8_t)constrain((int)p.radiation+ hazRad, 0, 10);
        if (hazLoseCon) {
          int slots[INV_SLOTS_MAX]; int slotCount = 0;
          for (int s = 0; s < (int)p.invSlots; s++) {
            if (p.invType[s] != 0) {
              const ItemDef* def = getItemDef(p.invType[s]);
              if (def && def->category == 0) slots[slotCount++] = s;
            }
          }
          if (slotCount > 0) {
            int target = slots[random(0, slotCount)];
            Serial.printf("[ENC] P%d haz_lose_consumable: dropped item:%d from slot %d\n",
              pid, (int)p.invType[target], target);
            p.invType[target] = 0;
            p.invQty[target]  = 0;
          }
        }
        bool downed = (p.ll == 0);
        encounterEnded = downed || (hazEnds != 0);
        // Auto-drain co-located allies: major hazard (LL loss) costs 2, minor costs 1
        bool isMajor = (hazLL < 0);
        int drainAmt = isMajor ? 2 : 1;
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
        uint8_t hq = enc.hexQ, hr2 = enc.hexR;
        enc = {};
        { GameEvent eev = {}; eev.type = EVT_ENC_END; eev.pid = (uint8_t)pid;
          eev.q = (int16_t)hq; eev.r = (int16_t)hr2;
          eev.encOut = (G.players[pid].ll == 0) ? 3 : 0;
          enqEvt(eev); }
      }
      Serial.printf("[ENC] P%d choice: DN%d roll%d %s%s\n",
        pid, dn, cr.total, cr.success ? "OK" : "FAIL", encounterEnded ? " (end)" : "");
    }
    xSemaphoreGive(G.mutex);
  }
}

static void handleMsg_enc_bank(AsyncWebSocketClient* client, char* data, size_t len) {
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int pid = findSlot(client->id());
    if (pid >= 0 && encounters[pid].active) {
      Player& p = G.players[pid];
      ActiveEncounter& enc = encounters[pid];
      bool fullClear = (enc.active & (1 << 7)) != 0;
      int totalRes = 0;
      for (int i = 0; i < 5; i++) {
        p.inv[i] = (uint8_t)min(99, (int)p.inv[i] + (int)enc.pendingLoot[i]);
        totalRes += enc.pendingLoot[i];
      }
      for (int j = 0; j < enc.pendingItemCount; j++) {
        uint8_t itemId = enc.pendingItemType[j];
        uint8_t qty    = enc.pendingItemQty[j];
        if (!itemId || !qty) continue;
        bool placed = false;
        for (int s = 0; s < p.invSlots && !placed; s++) {
          if (p.invType[s] == itemId) {
            const ItemDef* def = getItemDef(itemId);
            uint8_t cap = def ? def->maxStack : 1;
            if (p.invQty[s] < cap) {
              uint8_t space = cap - p.invQty[s];
              uint8_t add   = min(qty, space);
              p.invQty[s]  += add;
              qty           -= add;
              placed = (qty == 0);
            }
          }
        }
        if (!placed) {
          for (int s = 0; s < p.invSlots && !placed; s++) {
            if (!p.invType[s]) {
              p.invType[s] = itemId; p.invQty[s] = qty; placed = true;
            }
          }
        }
        if (!placed) {
          // Inventory full — drop to ground
          int gslot = -1;
          for (int g = 0; g < MAX_GROUND; g++) {
            if (groundItems[g].itemType == itemId && groundItems[g].q == p.q && groundItems[g].r == p.r)
              { gslot = g; break; }
          }
          if (gslot < 0) {
            for (int g = 0; g < MAX_GROUND; g++) {
              if (!groundItems[g].itemType) { gslot = g; break; }
            }
          }
          if (gslot >= 0) {
            groundItems[gslot].q = p.q; groundItems[gslot].r = p.r;
            groundItems[gslot].itemType = itemId;
            groundItems[gslot].qty = (uint8_t)min(255, (int)groundItems[gslot].qty + (int)qty);
          } else {
            Serial.printf("[ENC] P%d bank item:%d x%d LOST — MAX_GROUND full\n", pid, (int)itemId, (int)qty);
          }
        }
      }
      int scoreGain = totalRes * 3 + (fullClear ? 10 : 0);
      GameEvent ev = {};
      ev.type = EVT_ENC_BANK; ev.pid = (uint8_t)pid;
      ev.q = (int16_t)enc.hexQ; ev.r = (int16_t)enc.hexR;
      memcpy(ev.encLoot, enc.pendingLoot, 5);
      addScore(p, ev, scoreGain);
      p.encCount++;
      enqEvt(ev);
      Serial.printf("[ENC] P%d banked (res:%d score+%d%s)\n",
        pid, totalRes, scoreGain, fullClear ? " FULLCLEAR" : "");
      enc = {};
    }
    xSemaphoreGive(G.mutex);
  }
}

static void handleMsg_enc_abort(AsyncWebSocketClient* client, char* data, size_t len) {
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int pid = findSlot(client->id());
    if (pid >= 0 && encounters[pid].active) {
      uint8_t hq = encounters[pid].hexQ, hr2 = encounters[pid].hexR;
      encounters[pid] = {};
      if (G.threatClock < 20) G.threatClock++;
      GameEvent ev = {}; ev.type = EVT_ENC_END; ev.pid = (uint8_t)pid;
      ev.q = (int16_t)hq; ev.r = (int16_t)hr2; ev.encOut = 1;  // reason: abort
      enqEvt(ev);
      Serial.printf("[ENC] P%d aborted (TC now %d)\n", pid, (int)G.threatClock);
    }
    xSemaphoreGive(G.mutex);
  }
}
