#pragma once
// ── Item message handlers: use, equip, unequip, drop, pickup ─────────────────

// Shared helper: build item_result JSON into a static buffer and send it.
// The static buffers here are per-function, not shared, so concurrent calls
// from different message types are safe (each function has its own static).

static void handleMsg_use_item(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* sp = strstr(data, "\"slot\"");
  if (!sp) return;
  const char* sv = strchr(sp + 6, ':'); if (!sv) return;
  int slotIdx = atoi(sv + 1);
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) return;
  static char ack[320];
  bool ok = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      Player& pl = G.players[mySlot];
      uint8_t narParam = 0;
      if (slotIdx < INV_SLOTS_MAX && pl.invType[slotIdx]) {
        const ItemDef* preDef = getItemDef(pl.invType[slotIdx]);
        if (preDef && preDef->effectId == EFX_NARRATIVE) narParam = preDef->effectParam;
      }
      ok = useItem(mySlot, (uint8_t)slotIdx);
      snprintf(ack, sizeof(ack),
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"use\",\"slot\":%d,\"pid\":%d,"
        "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"eq\":[%d,%d,%d,%d,%d],\"inv\":[%d,%d,%d,%d,%d],\"efxp\":%d}",
        ok?"true":"false", slotIdx, mySlot,
        pl.invType[0],pl.invType[1],pl.invType[2],pl.invType[3],
        pl.invType[4],pl.invType[5],pl.invType[6],pl.invType[7],
        pl.invType[8],pl.invType[9],pl.invType[10],pl.invType[11],
        pl.invQty[0],pl.invQty[1],pl.invQty[2],pl.invQty[3],
        pl.invQty[4],pl.invQty[5],pl.invQty[6],pl.invQty[7],
        pl.invQty[8],pl.invQty[9],pl.invQty[10],pl.invQty[11],
        pl.equip[0],pl.equip[1],pl.equip[2],pl.equip[3],pl.equip[4],
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4],
        (int)narParam);
    }
    xSemaphoreGive(G.mutex);
  }
  // saveGame and client->text called outside mutex so saveGame can acquire it
  if (ok) saveGame();
  if (ack[0]) client->text(ack);
}

static void handleMsg_equip_item(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* sp = strstr(data, "\"slot\"");
  if (!sp) return;
  const char* sv = strchr(sp + 6, ':'); if (!sv) return;
  int slotIdx = atoi(sv + 1);
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) return;
  static char ack[256];
  bool ok = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      ok = equipItem(mySlot, (uint8_t)slotIdx);
      Player& pl = G.players[mySlot];
      snprintf(ack, sizeof(ack),
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"equip\",\"slot\":%d,\"pid\":%d,"
        "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"eq\":[%d,%d,%d,%d,%d],\"inv\":[%d,%d,%d,%d,%d]}",
        ok?"true":"false", slotIdx, mySlot,
        pl.invType[0],pl.invType[1],pl.invType[2],pl.invType[3],
        pl.invType[4],pl.invType[5],pl.invType[6],pl.invType[7],
        pl.invType[8],pl.invType[9],pl.invType[10],pl.invType[11],
        pl.invQty[0],pl.invQty[1],pl.invQty[2],pl.invQty[3],
        pl.invQty[4],pl.invQty[5],pl.invQty[6],pl.invQty[7],
        pl.invQty[8],pl.invQty[9],pl.invQty[10],pl.invQty[11],
        pl.equip[0],pl.equip[1],pl.equip[2],pl.equip[3],pl.equip[4],
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) saveGame();
  if (ack[0]) client->text(ack);
}

static void handleMsg_unequip_item(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* ep = strstr(data, "\"eslot\"");
  if (!ep) return;
  const char* ev = strchr(ep + 7, ':'); if (!ev) return;
  int eslot = atoi(ev + 1);
  if (eslot < 0 || eslot >= EQUIP_SLOTS) return;
  static char ack[256];
  bool ok = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      ok = unequipItem(mySlot, (uint8_t)eslot);
      Player& pl = G.players[mySlot];
      snprintf(ack, sizeof(ack),
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"unequip\",\"eslot\":%d,\"pid\":%d,"
        "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"eq\":[%d,%d,%d,%d,%d],\"inv\":[%d,%d,%d,%d,%d]}",
        ok?"true":"false", eslot, mySlot,
        pl.invType[0],pl.invType[1],pl.invType[2],pl.invType[3],
        pl.invType[4],pl.invType[5],pl.invType[6],pl.invType[7],
        pl.invType[8],pl.invType[9],pl.invType[10],pl.invType[11],
        pl.invQty[0],pl.invQty[1],pl.invQty[2],pl.invQty[3],
        pl.invQty[4],pl.invQty[5],pl.invQty[6],pl.invQty[7],
        pl.invQty[8],pl.invQty[9],pl.invQty[10],pl.invQty[11],
        pl.equip[0],pl.equip[1],pl.equip[2],pl.equip[3],pl.equip[4],
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) saveGame();
  if (ack[0]) client->text(ack);
}

static void handleMsg_drop_item(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* sp = strstr(data, "\"slot\"");
  if (!sp) return;
  const char* sv = strchr(sp + 6, ':'); if (!sv) return;
  int slotIdx = atoi(sv + 1);
  const char* qp = strstr(data, "\"qty\"");
  int qty = qp ? atoi(strchr(qp + 5, ':') + 1) : 1;
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX || qty <= 0) return;
  static char ack[256];
  static char upd[1280];
  bool ok = false;
  upd[0] = '\0';
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      ok = dropItem(mySlot, (uint8_t)slotIdx, (uint8_t)qty);
      Player& p = G.players[mySlot];
      if (ok) {
        int upos = snprintf(upd, sizeof(upd),
          "{\"t\":\"ground_update\",\"q\":%d,\"r\":%d,\"gi\":[",
          (int)p.q, (int)p.r);
        bool firstGi = true;
        for (int g = 0; g < MAX_GROUND; g++) {
          if (!groundItems[g].itemType) continue;
          if (!firstGi) upd[upos++] = ',';
          upos += snprintf(upd + upos, sizeof(upd) - upos,
            "{\"g\":%d,\"q\":%d,\"r\":%d,\"id\":%d,\"n\":%d}",
            g, groundItems[g].q, groundItems[g].r,
            groundItems[g].itemType, groundItems[g].qty);
          firstGi = false;
        }
        snprintf(upd + upos, sizeof(upd) - upos, "]}");
      }
      Player& pl = p;
      snprintf(ack, sizeof(ack),
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"drop\",\"slot\":%d,\"pid\":%d,"
        "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"eq\":[%d,%d,%d,%d,%d],\"inv\":[%d,%d,%d,%d,%d]}",
        ok?"true":"false", slotIdx, mySlot,
        pl.invType[0],pl.invType[1],pl.invType[2],pl.invType[3],
        pl.invType[4],pl.invType[5],pl.invType[6],pl.invType[7],
        pl.invType[8],pl.invType[9],pl.invType[10],pl.invType[11],
        pl.invQty[0],pl.invQty[1],pl.invQty[2],pl.invQty[3],
        pl.invQty[4],pl.invQty[5],pl.invQty[6],pl.invQty[7],
        pl.invQty[8],pl.invQty[9],pl.invQty[10],pl.invQty[11],
        pl.equip[0],pl.equip[1],pl.equip[2],pl.equip[3],pl.equip[4],
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) {
    saveGame();
    if (upd[0]) ws.textAll(upd);
  }
  if (ack[0]) client->text(ack);
}

static void handleMsg_pickup_item(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* gp = strstr(data, "\"gslot\"");
  if (!gp) return;
  const char* gv = strchr(gp + 7, ':'); if (!gv) return;
  int gslot = atoi(gv + 1);
  if (gslot < 0 || gslot >= MAX_GROUND) return;
  static char ack[256];
  static char upd[1280];
  bool ok = false;
  upd[0] = '\0';
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      ok = pickupGroundItem(mySlot, (uint8_t)gslot);
      Player& p2 = G.players[mySlot];
      if (ok) {
        int upos = snprintf(upd, sizeof(upd),
          "{\"t\":\"ground_update\",\"q\":%d,\"r\":%d,\"gi\":[",
          (int)p2.q, (int)p2.r);
        bool firstGi = true;
        for (int g = 0; g < MAX_GROUND; g++) {
          if (!groundItems[g].itemType) continue;
          if (!firstGi) upd[upos++] = ',';
          upos += snprintf(upd + upos, sizeof(upd) - upos,
            "{\"g\":%d,\"q\":%d,\"r\":%d,\"id\":%d,\"n\":%d}",
            g, groundItems[g].q, groundItems[g].r,
            groundItems[g].itemType, groundItems[g].qty);
          firstGi = false;
        }
        snprintf(upd + upos, sizeof(upd) - upos, "]}");
      }
      Player& pl2 = p2;
      snprintf(ack, sizeof(ack),
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"pickup\",\"gslot\":%d,\"pid\":%d,"
        "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"eq\":[%d,%d,%d,%d,%d],\"inv\":[%d,%d,%d,%d,%d]}",
        ok?"true":"false", gslot, mySlot,
        pl2.invType[0],pl2.invType[1],pl2.invType[2],pl2.invType[3],
        pl2.invType[4],pl2.invType[5],pl2.invType[6],pl2.invType[7],
        pl2.invType[8],pl2.invType[9],pl2.invType[10],pl2.invType[11],
        pl2.invQty[0],pl2.invQty[1],pl2.invQty[2],pl2.invQty[3],
        pl2.invQty[4],pl2.invQty[5],pl2.invQty[6],pl2.invQty[7],
        pl2.invQty[8],pl2.invQty[9],pl2.invQty[10],pl2.invQty[11],
        pl2.equip[0],pl2.equip[1],pl2.equip[2],pl2.equip[3],pl2.equip[4],
        pl2.inv[0],pl2.inv[1],pl2.inv[2],pl2.inv[3],pl2.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) {
    saveGame();
    if (upd[0]) ws.textAll(upd);
  }
  if (ack[0]) client->text(ack);
}
