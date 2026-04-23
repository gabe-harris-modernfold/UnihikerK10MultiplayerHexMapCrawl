#pragma once
// ── Trade message handlers: offer, accept, decline ───────────────────────────

static void handleMsg_trade_offer(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* top = strstr(data, "\"to\""); if (!top) return;
  const char* tov = strchr(top + 4, ':');  if (!tov) return;
  int toPid = atoi(tov + 1);

  uint8_t give[5] = {0}, want[5] = {0};
  const char* gp = strstr(data, "\"give\"");
  if (gp) { const char* gb = strchr(gp + 6, '['); if (gb) { gb++;
    for (int i = 0; i < 5; i++) {
      while (*gb == ' ') gb++;
      give[i] = (uint8_t)constrain(atoi(gb), 0, 99);
      const char* nx = strchr(gb, i < 4 ? ',' : ']'); if (!nx) break; gb = nx + 1;
    }
  }}
  const char* wp = strstr(data, "\"want\"");
  if (wp) { const char* wb = strchr(wp + 6, '['); if (wb) { wb++;
    for (int i = 0; i < 5; i++) {
      while (*wb == ' ') wb++;
      want[i] = (uint8_t)constrain(atoi(wb), 0, 99);
      const char* nx = strchr(wb, i < 4 ? ',' : ']'); if (!nx) break; wb = nx + 1;
    }
  }}

  int total = 0;
  for (int i = 0; i < 5; i++) total += give[i] + want[i];
  if (total == 0) return;
  if (toPid < 0 || toPid >= MAX_PLAYERS) return;

  bool valid = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int fromSlot = findSlot(client->id());
    if (fromSlot >= 0 && (encounters[fromSlot].active || encounters[toPid].active)) {
      xSemaphoreGive(G.mutex);
      client->text("{\"t\":\"err\",\"msg\":\"Cannot trade during encounter\"}");
      return;
    }
    if (fromSlot >= 0 && fromSlot != toPid &&
        G.players[toPid].connected &&
        samehex(fromSlot, toPid) &&
        hasResources(fromSlot, give)) {
      bool dup = false;
      for (int ti = 0; ti < MAX_PLAYERS; ti++) {
        if (!tradeOffers[ti].active) continue;
        if ((ti == fromSlot && tradeOffers[ti].toPid == (uint8_t)toPid) ||
            (tradeOffers[ti].fromPid == (uint8_t)toPid && tradeOffers[ti].toPid == (uint8_t)fromSlot)) {
          dup = true; break;
        }
      }
      if (!dup) {
        tradeOffers[fromSlot].active    = true;
        tradeOffers[fromSlot].fromPid   = (uint8_t)fromSlot;
        tradeOffers[fromSlot].toPid     = (uint8_t)toPid;
        memcpy(tradeOffers[fromSlot].give, give, 5);
        memcpy(tradeOffers[fromSlot].want, want, 5);
        tradeOffers[fromSlot].expiresMs = millis() + TRADE_EXPIRE_MS;
        GameEvent tev = {};
        tev.type    = EVT_TRADE_OFFER;
        tev.pid     = (uint8_t)fromSlot;
        tev.tradeTo = (uint8_t)toPid;
        memcpy(tev.tradeGive, give, 5);
        memcpy(tev.tradeWant, want, 5);
        enqEvt(tev);
        valid = true;
      }
    }
    xSemaphoreGive(G.mutex);
  }
  if (!valid) {
    char fb[48];
    int fl = snprintf(fb, sizeof(fb), "{\"t\":\"trade_fail\"}");
    client->text(fb, (size_t)fl);
  }
}

static void handleMsg_trade_accept(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* fp = strstr(data, "\"from\""); if (!fp) return;
  const char* fv = strchr(fp + 6, ':');      if (!fv) return;
  int fromPid = atoi(fv + 1);
  if (fromPid < 0 || fromPid >= MAX_PLAYERS) return;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 && (encounters[mySlot].active || encounters[fromPid].active)) {
      xSemaphoreGive(G.mutex);
      client->text("{\"t\":\"err\",\"msg\":\"Cannot trade during encounter\"}");
      return;
    }
    GameEvent tev = {};
    tev.type    = EVT_TRADE_RESULT;
    tev.pid     = (uint8_t)fromPid;
    if (mySlot >= 0 &&
        tradeOffers[fromPid].active &&
        tradeOffers[fromPid].toPid == (uint8_t)mySlot &&
        millis() < tradeOffers[fromPid].expiresMs &&
        samehex(fromPid, mySlot) &&
        hasResources(fromPid, tradeOffers[fromPid].give) &&
        hasResources(mySlot,  tradeOffers[fromPid].want)) {
      executeTrade(fromPid, mySlot, tradeOffers[fromPid]);
      tev.tradeTo     = (uint8_t)mySlot;
      tev.tradeResult = 1;
    } else if (mySlot >= 0) {
      tev.tradeTo     = (uint8_t)mySlot;
      tev.tradeResult = 2;
    }
    tradeOffers[fromPid].active = false;
    enqEvt(tev);
    xSemaphoreGive(G.mutex);
  }
}

static void handleMsg_trade_decline(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* fp = strstr(data, "\"from\""); if (!fp) return;
  const char* fv = strchr(fp + 6, ':');      if (!fv) return;
  int fromPid = atoi(fv + 1);
  if (fromPid < 0 || fromPid >= MAX_PLAYERS) return;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 &&
        tradeOffers[fromPid].active &&
        tradeOffers[fromPid].toPid == (uint8_t)mySlot) {
      tradeOffers[fromPid].active = false;
      GameEvent tev = {};
      tev.type        = EVT_TRADE_RESULT;
      tev.pid         = (uint8_t)fromPid;
      tev.tradeTo     = (uint8_t)mySlot;
      tev.tradeResult = 2;
      enqEvt(tev);
    }
    xSemaphoreGive(G.mutex);
  }
}
