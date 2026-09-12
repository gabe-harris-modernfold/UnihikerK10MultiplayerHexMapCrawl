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
      // One outstanding offer per sender: block a second offer (even to a
      // different target) until the first resolves/declines/expires, so it
      // can't be silently overwritten out from under its original recipient.
      bool dup = false;
      for (int ti = 0; ti < MAX_PLAYERS; ti++) {
        if (!tradeOffers[ti].active) continue;
        if (ti == fromSlot ||
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
    if (mySlot < 0) { xSemaphoreGive(G.mutex); return; }  // not a seated player — nothing to accept with
    if (encounters[mySlot].active || encounters[fromPid].active) {
      xSemaphoreGive(G.mutex);
      client->text("{\"t\":\"err\",\"msg\":\"Cannot trade during encounter\"}");
      return;
    }
    // No offer actually addressed to me from this sender — ignore rather than
    // broadcast a misleading result for an offer that never existed.
    if (!tradeOffers[fromPid].active || tradeOffers[fromPid].toPid != (uint8_t)mySlot) {
      xSemaphoreGive(G.mutex);
      return;
    }
    GameEvent tev = {};
    tev.type    = EVT_TRADE_RESULT;
    tev.pid     = (uint8_t)fromPid;
    tev.tradeTo = (uint8_t)mySlot;
    if (millis() < tradeOffers[fromPid].expiresMs &&
        samehex(fromPid, mySlot) &&
        hasResources(fromPid, tradeOffers[fromPid].give) &&
        hasResources(mySlot,  tradeOffers[fromPid].want)) {
      executeTrade(fromPid, mySlot, tradeOffers[fromPid]);
      tev.tradeResult = 1;
    } else {
      // Offer existed but conditions no longer hold (moved off-hex, resources
      // spent, expired) — distinct from an explicit decline (see trade_decline).
      tev.tradeResult = 4;
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

// ── Caravan trade (dedicated — not the player-to-player tradeOffers table) ──
// One-shot: no offer/accept round-trip. The caravan side of the transaction
// is world-system.hpp's W.caravan, not another Player slot, so this can't
// reuse handleMsg_trade_accept's pid<MAX_PLAYERS-bounded machinery — see
// docs/world-system-spec.md's rationale for a dedicated handler.
static void handleMsg_caravan_trade(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
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

  bool valid = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int slot = findSlot(client->id());
    if (slot >= 0 && !encounters[slot].active && W.caravan.active &&
        G.players[slot].q == W.caravan.q && G.players[slot].r == W.caravan.r &&
        hasResources(slot, give)) {
      bool caravanHas = true;
      for (int i = 0; i < 5; i++) if (W.caravan.inv[i] < want[i]) { caravanHas = false; break; }
      if (caravanHas) {
        Player& p = G.players[slot];
        for (int i = 0; i < 5; i++) {
          p.inv[i]         = (uint8_t)min((int)p.inv[i] - give[i] + want[i], 99);
          W.caravan.inv[i] = (uint8_t)min((int)W.caravan.inv[i] - want[i] + give[i], 99);
        }
        GameEvent tev = {};
        tev.type        = EVT_TRADE_RESULT;
        tev.pid         = (uint8_t)slot;
        tev.tradeTo     = CARAVAN_PID;
        tev.tradeResult = 1;
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
