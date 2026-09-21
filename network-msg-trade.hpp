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

// ── Caravan purchase: resource tokens → a consumable off the shelf ────────
// {"t":"car_buy","item":ID,"n":QTY,"give":[5]}. give[] is the payment; its
// worth (caravanPaymentValue: food/med/scrap 1 each, fuel 2, water refused)
// must cover caravanPrice(item) × n. Overpaying is accepted — the caravan
// doesn't make change; the client keeps it to at most one fuel token's
// rounding. One-shot like car_trade. Everything is checked before
// anything is spent — co-location, shelf has ≥ n, the player holds give[],
// invRoomFor() has room for all n (same all-or-nothing rule as applyRecipe()).
// Success: EVT_TRADE_RESULT with tradeTo=CARAVAN_PID + tradeItem for every
// client's log, plus a targeted item_result (act:"buy") so the buyer's pack
// and token counts update at once — invType[]/invQty[] never ride the state
// broadcast, same reason the craft ack exists (network-msg-player.hpp).
// Failure: {"t":"trade_fail","why":N} — 1 not co-located / not in stock,
// 2 can't pay, 3 pack full, 4 tried to pay with water.
static void handleMsg_caravan_buy(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* ip = strstr(data, "\"item\""); if (!ip) return;
  const char* iv = strchr(ip + 6, ':');      if (!iv) return;
  int itemId = atoi(iv + 1);
  if (itemId <= 0 || itemId > 254) return;
  int n = 1;
  const char* np = strstr(data, "\"n\"");
  if (np) { const char* nv = strchr(np + 3, ':'); if (nv) n = atoi(nv + 1); }
  if (n < 1 || n > (int)CARAVAN_STOCK_MAX) return;
  uint8_t give[5] = {0};
  const char* gp = strstr(data, "\"give\"");
  if (gp) { const char* gb = strchr(gp + 6, '['); if (gb) { gb++;
    for (int i = 0; i < 5; i++) {
      while (*gb == ' ') gb++;
      give[i] = (uint8_t)constrain(atoi(gb), 0, 99);
      const char* nx = strchr(gb, i < 4 ? ',' : ']'); if (!nx) break; gb = nx + 1;
    }
  }}
  int paid = caravanPaymentValue(give);

  static char ack[512];   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  ack[0] = '\0';  // static buffer: must not leak a previous call's (possibly another player's) ack
  int why = 1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int slot = findSlot(client->id());
    const ItemDef* def = getItemDef((uint8_t)itemId);
    int ss = caravanStockSlot((uint8_t)itemId);
    if (slot >= 0 && !encounters[slot].active && W.caravan.active &&
        G.players[slot].q == W.caravan.q && G.players[slot].r == W.caravan.r &&
        def && ss >= 0 && (int)W.caravan.stockQty[ss] >= n) {
      Player& p = G.players[slot];
      int price = (int)caravanPrice(def) * n;
      if (give[0])                                                                          why = 4;  // water: worthless to the caravan — refused, not silently ignored
      else if (paid < price || !hasResources(slot, give))                                  why = 2;
      else if (invRoomFor(p.invType, p.invQty, effectiveInvSlots(p), (uint8_t)itemId) < n) why = 3;
      else {
        for (int i = 0; i < 5; i++) {
          p.inv[i]         = (uint8_t)(p.inv[i] - give[i]);
          W.caravan.inv[i] = (uint8_t)min((int)W.caravan.inv[i] + give[i], 99);
        }
        W.caravan.stockQty[ss] = (uint8_t)(W.caravan.stockQty[ss] - n);
        if (!W.caravan.stockQty[ss]) W.caravan.stockItem[ss] = 0;  // free the slot for the next restock roll
        addItemToInv(p, (uint8_t)itemId, (uint8_t)n);
        GameEvent tev = {};
        tev.type         = EVT_TRADE_RESULT;
        tev.pid          = (uint8_t)slot;
        tev.tradeTo      = CARAVAN_PID;
        tev.tradeResult  = 1;
        tev.tradeItem    = (uint8_t)itemId;
        tev.tradeItemQty = (uint8_t)n;
        memcpy(tev.tradeGive, give, 5);
        enqEvt(tev);
        int ap = appendFmt(ack, sizeof(ack), 0,
          "{\"t\":\"item_result\",\"ok\":true,\"act\":\"buy\",\"pid\":%d,\"item\":%d,\"n\":%d,",
          slot, itemId, n);
        ap = appendPackArrays(ack, sizeof(ack), ap, slot);
        appendFmt(ack, sizeof(ack), ap, ",\"inv\":[%d,%d,%d,%d,%d]}",
          p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4]);
        why = 0;
      }
    }
    xSemaphoreGive(G.mutex);
  }
  if (why == 0) {
    saveGame();  // outside the mutex — it takes G.mutex itself (same as craft/use_item)
    if (ack[0]) client->text(ack);
  } else {
    char fb[48];
    int fl = snprintf(fb, sizeof(fb), "{\"t\":\"trade_fail\",\"why\":%d}", why);
    client->text(fb, (size_t)fl);
  }
}
