#pragma once
// ── Item message handlers: use, equip, unequip, drop, pickup, loot ───────────

// Shared helper: build item_result JSON into a static buffer and send it.
// The static buffers here are per-function, not shared, so concurrent calls
// from different message types are safe (each function has its own static).

// ── pushVisDisk ───────────────────────────────────────────────────────────
// Send one client a fresh vis disk.  Call after anything that can move that
// survivor's playerVisParams(): a reveal_fog consumable, and equipping or
// unequipping gear carrying the passive +1 vision (Dark Goggles, Glow
// Dentures, Doom Clicker).
//
// The equip path had no push at all.  Server-side visR was right immediately,
// but nothing told the client: putting the goggles on revealed nothing until
// the next step, and taking them off left the client's radius too wide.  The
// message carries "vr", so it corrects the radius in both directions.
// Takes G.mutex itself — call it with the mutex released.
static void pushVisDisk(AsyncWebSocketClient* client, int pid) {
  if (!client || pid < 0) return;
  PSRAM_STATIC(char, visBuf, [1100]);
  int visLen = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int vr; bool mr;
    playerVisParams(pid, &vr, &mr);
    // Depth-aware for the same reason handleMsg_move() is: using a player's
    // q/r while they are below reveals the surface around their hatch.
    uint8_t dep = G.players[pid].depth;
    visLen = buildVisDisk(visBuf, sizeof(visBuf),
                          dep ? G.players[pid].tq : G.players[pid].q,
                          dep ? G.players[pid].tr : G.players[pid].r,
                          vr, mr, nullptr, dep);
    xSemaphoreGive(G.mutex);
  }
  if (visLen > 0) client->text(visBuf);
}

// Common tail of the item handlers' refusal paths. The item functions return
// a bare bool, so "refused" is as specific as it gets without changing them.
static void nackItem(AsyncWebSocketClient* client, bool locked, int mySlot, bool ok) {
  if (!locked)          wsNack(client, "busy");
  else if (mySlot < 0)  wsNack(client, "not_seated");
  else if (!ok)         wsNack(client, "refused");
}

static void handleMsg_use_item(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* sp = strstr(data, "\"slot\"");
  if (!sp) { wsNack(client, "parse"); return; }
  const char* sv = strchr(sp + 6, ':'); if (!sv) { wsNack(client, "parse"); return; }
  int slotIdx = atoi(sv + 1);
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) { wsNack(client, "bad_arg"); return; }
  PSRAM_STATIC(char, ack, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  ack[0] = '\0';  // static buffer: must not leak a previous call's (possibly another player's) ack
  bool ok = false;
  int capturedSlot = -1;
  uint8_t revealParam = 0;
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      Player& pl = G.players[mySlot];
      uint8_t narParam = 0;
      if (slotIdx < INV_SLOTS_MAX && pl.invType[slotIdx]) {
        const ItemDef* preDef = getItemDef(pl.invType[slotIdx]);
        if (preDef) {
          if (preDef->effectId  == EFX_NARRATIVE)  narParam    = preDef->effectParam;
          if (preDef->effectId  == EFX_REVEAL_FOG) revealParam = preDef->effectParam;
          if (preDef->effectId2 == EFX_REVEAL_FOG) revealParam = preDef->effectParam2;
        }
      }
      ok = useItem(mySlot, (uint8_t)slotIdx);
      capturedSlot = mySlot;
      int ap = appendFmt(ack, sizeof(ack), 0,
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"use\",\"slot\":%d,\"pid\":%d,",
        ok?"true":"false", slotIdx, mySlot);
      ap = appendPackArrays(ack, sizeof(ack), ap, mySlot);
      appendFmt(ack, sizeof(ack), ap,
        ",\"inv\":[%d,%d,%d,%d,%d],\"efxp\":%d}",
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4],
        (int)narParam);
    }
    xSemaphoreGive(G.mutex);
  }
  // saveGame and client->text called outside mutex so saveGame can acquire it
  if (ok) requestSave();
  if (ack[0]) client->text(ack);
  // EFX_REVEAL_FOG items: send a fresh vis disk so the client sees newly revealed cells
  if (ok && capturedSlot >= 0 && revealParam >= 2) pushVisDisk(client, capturedSlot);
  nackItem(client, locked, mySlot, ok);
}

static void handleMsg_equip_item(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* sp = strstr(data, "\"slot\"");
  if (!sp) { wsNack(client, "parse"); return; }
  const char* sv = strchr(sp + 6, ':'); if (!sv) { wsNack(client, "parse"); return; }
  int slotIdx = atoi(sv + 1);
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) { wsNack(client, "bad_arg"); return; }
  PSRAM_STATIC(char, ack, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  ack[0] = '\0';  // static buffer: must not leak a previous call's (possibly another player's) ack
  bool ok = false;
  int  capturedSlot = -1;
  bool visChanged   = false;
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      // A swap can change vision through EITHER item, so diff the bonus
      // rather than inspecting the one being put on.
      int visBefore = equipVisionBonus(mySlot);
      ok = equipItem(mySlot, (uint8_t)slotIdx);
      capturedSlot = mySlot;
      visChanged   = ok && (equipVisionBonus(mySlot) != visBefore);
      Player& pl = G.players[mySlot];
      int ap = appendFmt(ack, sizeof(ack), 0,
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"equip\",\"slot\":%d,\"pid\":%d,",
        ok?"true":"false", slotIdx, mySlot);
      ap = appendPackArrays(ack, sizeof(ack), ap, mySlot);
      appendFmt(ack, sizeof(ack), ap, ",\"inv\":[%d,%d,%d,%d,%d]}",
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) requestSave();
  if (ack[0]) client->text(ack);
  if (visChanged) pushVisDisk(client, capturedSlot);
  nackItem(client, locked, mySlot, ok);
}

static void handleMsg_unequip_item(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* ep = strstr(data, "\"eslot\"");
  if (!ep) { wsNack(client, "parse"); return; }
  const char* ev = strchr(ep + 7, ':'); if (!ev) { wsNack(client, "parse"); return; }
  int eslot = atoi(ev + 1);
  if (eslot < 0 || eslot >= EQUIP_SLOTS) { wsNack(client, "bad_arg"); return; }
  PSRAM_STATIC(char, ack, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  ack[0] = '\0';  // static buffer: must not leak a previous call's (possibly another player's) ack
  bool ok = false;
  int  capturedSlot = -1;
  bool visChanged   = false;
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      int visBefore = equipVisionBonus(mySlot);
      ok = unequipItem(mySlot, (uint8_t)eslot);
      capturedSlot = mySlot;
      visChanged   = ok && (equipVisionBonus(mySlot) != visBefore);
      Player& pl = G.players[mySlot];
      int ap = appendFmt(ack, sizeof(ack), 0,
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"unequip\",\"eslot\":%d,\"pid\":%d,",
        ok?"true":"false", eslot, mySlot);
      ap = appendPackArrays(ack, sizeof(ack), ap, mySlot);
      appendFmt(ack, sizeof(ack), ap, ",\"inv\":[%d,%d,%d,%d,%d]}",
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) requestSave();
  if (ack[0]) client->text(ack);
  if (visChanged) pushVisDisk(client, capturedSlot);
  nackItem(client, locked, mySlot, ok);
}

static void handleMsg_drop_item(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* sp = strstr(data, "\"slot\"");
  if (!sp) { wsNack(client, "parse"); return; }
  const char* sv = strchr(sp + 6, ':'); if (!sv) { wsNack(client, "parse"); return; }
  int slotIdx = atoi(sv + 1);
  const char* qp = strstr(data, "\"qty\"");
  // "qty" with no ':' after it used to be atoi(strchr(...) + 1) on a null
  // pointer -- a crash from one malformed message. Same guard as drop_res.
  const char* qv = qp ? strchr(qp + 5, ':') : nullptr;
  int qty = qv ? atoi(qv + 1) : 1;
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX || qty <= 0) { wsNack(client, "bad_arg"); return; }
  PSRAM_STATIC(char, ack, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  bool ok = false;
  int  gq = 0, gr = 0;
  ack[0] = '\0';  // static buffer: must not leak a previous call's (possibly another player's) data
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      ok = dropItem(mySlot, (uint8_t)slotIdx, (uint8_t)qty);
      Player& pl = G.players[mySlot];
      gq = pl.q; gr = pl.r;
      int ap = appendFmt(ack, sizeof(ack), 0,
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"drop\",\"slot\":%d,\"pid\":%d,",
        ok?"true":"false", slotIdx, mySlot);
      ap = appendPackArrays(ack, sizeof(ack), ap, mySlot);
      appendFmt(ack, sizeof(ack), ap, ",\"inv\":[%d,%d,%d,%d,%d]}",
        pl.inv[0],pl.inv[1],pl.inv[2],pl.inv[3],pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) {
    requestSave();
    broadcastGroundUpdate(gq, gr);
  }
  if (ack[0]) client->text(ack);
  nackItem(client, locked, mySlot, ok);
}

// Dump resource tokens (Water/Food/Fuel/Med/Scrap) out of the pack — the
// char sheet's inventory boxes send {"t":"drop_res","res":1-5,"qty":N}.
// dropResource() puts them back on the hex when it can hold them; the "rsp"
// broadcast is what tells every client the pile is there (same event the
// respawn tick uses), so no extra client-side map plumbing is needed.
static void handleMsg_drop_res(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* rp = strstr(data, "\"res\"");
  if (!rp) { wsNack(client, "parse"); return; }
  const char* rv = strchr(rp + 5, ':'); if (!rv) { wsNack(client, "parse"); return; }
  int res = atoi(rv + 1);
  const char* qp = strstr(data, "\"qty\"");
  const char* qv = qp ? strchr(qp + 5, ':') : nullptr;   // "qty" without a ':' would crash strchr()+1
  int qty = qv ? atoi(qv + 1) : 1;
  if (res < 1 || res > 5 || qty <= 0) { wsNack(client, "bad_arg"); return; }
  if (qty > 99) qty = 99;
  PSRAM_STATIC(char, ack, [256]);
  static char upd[96];
  ack[0] = '\0';  // static buffers: must not leak a previous call's (possibly another player's) data
  upd[0] = '\0';
  uint8_t dropped = 0;
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      bool    onGround = false;
      uint8_t rem      = 0;
      dropped = dropResource(mySlot, (uint8_t)res, (uint8_t)qty, &onGround, &rem);
      Player& pl = G.players[mySlot];
      if (dropped && onGround) {
        snprintf(upd, sizeof(upd),
          "{\"t\":\"ev\",\"k\":\"rsp\",\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d}",
          (int)pl.q, (int)pl.r, res, (int)rem);
      }
      snprintf(ack, sizeof(ack),
        "{\"t\":\"res_result\",\"ok\":%s,\"pid\":%d,\"res\":%d,\"qty\":%d,"
        "\"grd\":%d,\"rem\":%d,\"q\":%d,\"r\":%d,\"inv\":[%d,%d,%d,%d,%d],\"sc\":%d}",
        dropped ? "true" : "false", mySlot, res, (int)dropped,
        onGround ? 1 : 0, (int)rem, (int)pl.q, (int)pl.r,
        pl.inv[0], pl.inv[1], pl.inv[2], pl.inv[3], pl.inv[4], (int)pl.score);
    }
    xSemaphoreGive(G.mutex);
  }
  // saveGame outside the mutex so it can acquire it itself (same as drop_item)
  if (dropped) {
    requestSave();
    if (upd[0]) ws.textAll(upd);
  }
  if (ack[0]) client->text(ack);
  nackItem(client, locked, mySlot, dropped != 0);
}

static void handleMsg_pickup_item(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* gp = strstr(data, "\"gslot\"");
  if (!gp) { wsNack(client, "parse"); return; }
  const char* gv = strchr(gp + 7, ':'); if (!gv) { wsNack(client, "parse"); return; }
  int gslot = atoi(gv + 1);
  if (gslot < 0 || gslot >= MAX_GROUND) { wsNack(client, "bad_arg"); return; }
  PSRAM_STATIC(char, ack, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  bool ok = false;
  int  gq = 0, gr = 0;
  ack[0] = '\0';  // static buffer: must not leak a previous call's (possibly another player's) data
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      ok = pickupGroundItem(mySlot, (uint8_t)gslot);
      Player& pl2 = G.players[mySlot];
      gq = pl2.q; gr = pl2.r;
      int ap = appendFmt(ack, sizeof(ack), 0,
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"pickup\",\"gslot\":%d,\"pid\":%d,",
        ok?"true":"false", gslot, mySlot);
      ap = appendPackArrays(ack, sizeof(ack), ap, mySlot);
      appendFmt(ack, sizeof(ack), ap, ",\"inv\":[%d,%d,%d,%d,%d]}",
        pl2.inv[0],pl2.inv[1],pl2.inv[2],pl2.inv[3],pl2.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ok) {
    requestSave();
    broadcastGroundUpdate(gq, gr);   // also drops the grave marker once the hex is bare
  }
  if (ack[0]) client->text(ack);
  nackItem(client, locked, mySlot, ok);
}

// {"t":"loot","res":R} -- take resource tokens from the remains on this hex:
// R = 1-5 for one kind, 0 or absent for everything that fits (lootRemains()).
// The looter gets a loot_result -- `why` is LOOT_* (0 ok, 1 nothing here,
// 2 pack full), `got` what was taken; everyone gets the ground_update, which
// is also how the marker disappears once the hex is bare.
static void handleMsg_loot(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  int res = 0;
  const char* rp = strstr(data, "\"res\"");
  const char* rv = rp ? strchr(rp + 5, ':') : nullptr;
  if (rv) res = atoi(rv + 1);
  if (res < 0 || res > 5) { wsNack(client, "bad_arg"); return; }
  char    ack[192];
  uint8_t got[5] = {0};
  uint8_t out    = LOOT_NONE;
  int     gq = 0, gr = 0;
  ack[0] = '\0';
  bool locked = false, downed = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    downed = mySlot >= 0 && G.players[mySlot].ll == 0;
    if (mySlot >= 0 && G.players[mySlot].connected && !downed) {
      out = lootRemains(mySlot, res, got);
      Player& pl = G.players[mySlot];
      gq = pl.q; gr = pl.r;
      snprintf(ack, sizeof(ack),
        "{\"t\":\"loot_result\",\"ok\":%s,\"why\":%d,\"pid\":%d,"
        "\"got\":[%d,%d,%d,%d,%d],\"inv\":[%d,%d,%d,%d,%d]}",
        out == LOOT_OK ? "true" : "false", (int)out, mySlot,
        got[0], got[1], got[2], got[3], got[4],
        pl.inv[0], pl.inv[1], pl.inv[2], pl.inv[3], pl.inv[4]);
    }
    xSemaphoreGive(G.mutex);
  }
  if (ack[0]) client->text(ack);
  if (out == LOOT_OK) {
    broadcastGroundUpdate(gq, gr);
    requestSave();
  }
  if      (!locked)          wsNack(client, "busy");
  else if (mySlot < 0)       wsNack(client, "not_seated");
  else if (downed)           wsNack(client, "downed");
  else if (out == LOOT_FULL) wsNack(client, "pack_full");
  else if (out != LOOT_OK)   wsNack(client, "not_here");
}
