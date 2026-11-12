#pragma once
// ── Item message handlers: use, equip, unequip, drop, pickup, loot ───────────

// Shared helper: build item_result JSON into a static buffer and send it.
// The static buffers here are per-function, not shared, so concurrent calls
// from different message types are safe (each function has its own static).

// ── pushVisDisk ───────────────────────────────────────────────────────────
// Send one client a fresh vis disk.  Call after anything that can move that
// survivor's playerVisParams(): equipping or unequipping gear carrying the
// passive +1 vision (Dark Goggles, Glow Dentures, Doom Clicker), a tunnel
// section coming on. (A one-shot reveal_fog item does not move the radius --
// it sends its own band, buildItemReveal() below.) async_tcp only.
//
// The equip path had no push at all.  Server-side visR was right immediately,
// but nothing told the client: putting the goggles on revealed nothing until
// the next step, and taking them off left the client's radius too wide.  The
// message carries "vr", so it corrects the radius in both directions.
// Takes G.mutex itself — call it with the mutex released.
static void pushVisDisk(AsyncWebSocketClient* client, int pid) {
  if (!client || pid < 0) return;
  // 1600: an underground disk at the widest light (radius 6, clipped to the
  // 16x10 board) is ~110 cells at 12 chars with its "op" tail. 3200 for the
  // surface: its "trl" trail records ride after the cells and take what is left.
  PSRAM_STATIC(char, visBuf, [3200]);
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
                          vr, mr, nullptr, dep, pid);
    noteVisSent(pid, vr);
    xSemaphoreGive(G.mutex);
  }
  if (visLen > 0) client->text(visBuf);
}

// ── serviceVisRefresh ─────────────────────────────────────────────────────
// GameLoop, every tick, without G.mutex. Sends a fresh vis disk to any
// survivor whose sight radius has moved since their client was last told
// (noteVisSent(), hex-map.hpp) while they stood still: an ally stepping onto
// or off their hex, the last Bile Flare below burnt or dropped, smoke on their
// hex. A change of weather is left for their next step, as it always was.
//
// Its own buffer, not pushVisDisk()'s: that one belongs to the async_tcp
// task, and a PSRAM_STATIC is one shared copy (dev-loop.md). One survivor a
// tick at most -- a second one waits 100 ms -- so the buffer is filled under
// G.mutex and sent after it, with the client looked up by id (ws.client()
// takes AsyncWebSocket's own lock, which must not nest inside G.mutex).
static void serviceVisRefresh() {
  static uint8_t next = 0;   // round robin, so one busy seat cannot starve the rest
  PSRAM_STATIC(char, refBuf, [3200]);
  int      len  = 0;
  uint32_t wsId = 0;
  int      who  = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(2)) != pdTRUE) return;
  for (int n = 0; n < MAX_PLAYERS && who < 0; n++) {
    int i = (next + n) % MAX_PLAYERS;
    const Player& p = G.players[i];
    if (!p.connected || !s_visSentKnown[i]) continue;
    if (s_visSentWx[i] != G.weatherPhase) continue;   // weather: the next step
    int vr; bool mr;
    playerVisParams(i, &vr, &mr);
    if (vr == s_visSentR[i]) continue;
    len = buildVisDisk(refBuf, sizeof(refBuf),
                       p.depth ? p.tq : p.q, p.depth ? p.tr : p.r,
                       vr, mr, nullptr, p.depth, i);
    noteVisSent(i, vr);
    wsId = p.wsClientId;
    who  = i;
    Log.notice("VIS refresh pid=%d vr=%d dp=%d", i, vr, (int)p.depth);
  }
  xSemaphoreGive(G.mutex);
  if (who < 0) return;
  next = (uint8_t)((who + 1) % MAX_PLAYERS);
  if (len > 0)
    if (AsyncWebSocketClient* cl = ws.client(wsId)) cl->text(refBuf, (size_t)len);
}

// ── The reveal a one-shot reveal_fog item does on use ────────────────────
// Filled under G.mutex by handleMsg_use_item, sent after it. 2..98: the rings
// from just past the reader's sight out to sight + param (capped at
// REVEAL_R_MAX), as an ev surv band -- the same peek SURVEY gives, so the
// client already draws it and keeps the ground. 99: the whole map's terrain
// as an ev rvl. Surface only; useItem() refuses a pure reveal below, and the
// flare's reveal half does nothing there.
//
// The param is counted from the edge of sight, not from the survivor: base
// sight is 3 and a Scout's 5, so a radius-2 or -3 reveal measured from the
// hex underfoot showed nothing at all on open ground. The flare's own line in
// items.cfg always said "2 extra hexes".
static int buildItemReveal(char* buf, int cap, int pid, uint8_t param) {
  const Player& p = G.players[pid];
  if (p.depth || param < 2) return 0;
  if (param == REVEAL_WHOLE_MAP) return buildTerrainReveal(buf, cap, pid);
  int vr; bool mr;
  playerVisParams(pid, &vr, &mr);
  int outer = min(vr + (int)param, REVEAL_R_MAX);
  if (outer <= vr) return 0;
  return buildSurveyBand(buf, cap, p.q, p.r, vr, outer, mr, pid);
}

// The equip/unequip ack's own fields. "uf" is unfuelledMask() as of this
// swap, so a bike put on with a dry tank shows stalled from the first frame
// instead of riding for one tick and then fading. "why" (refusals only) is
// equipItem()/unequipItem()'s s_itemWhy, so the client can say what is in
// the way rather than ignore the tap. Must hold G.mutex.
static int appendEquipTail(char* buf, size_t cap, int pos, int pid, bool ok) {
  pos = appendFmt(buf, cap, pos, ",\"uf\":%d", (int)unfuelledMask(pid));
  if (!ok) pos = appendFmt(buf, cap, pos, ",\"why\":\"%s\"", s_itemWhy);
  return pos;
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
  // A reveal's cells (buildItemReveal): the whole-map read is 8.6 KB, a band
  // at REVEAL_R_MAX up to ~5.5 KB plus its trail records. async_tcp only.
  PSRAM_STATIC(char, rvlBuf, [8800]);
  int rvlLen = 0;
  bool ok = false;
  uint8_t revealParam = 0;
  bool locked = false; int mySlot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    locked = true;
    mySlot = findSlot(client->id());
    if (mySlot >= 0 && G.players[mySlot].connected) {
      Player& pl = G.players[mySlot];
      uint8_t narParam = 0;
      const uint8_t usedType = pl.invType[slotIdx];   // the slot may be empty after the use
      if (slotIdx < INV_SLOTS_MAX && pl.invType[slotIdx]) {
        const ItemDef* preDef = getItemDef(pl.invType[slotIdx]);
        if (preDef) {
          if (preDef->effectId  == EFX_NARRATIVE)  narParam    = preDef->effectParam;
          revealParam = itemRevealParam(preDef);
        }
      }
      ok = useItem(mySlot, (uint8_t)slotIdx);
      // After the use, so a Super Juice or anything else that moved this
      // survivor's sight is already in the radius the band starts from.
      if (ok && revealParam) {
        rvlLen = buildItemReveal(rvlBuf, sizeof(rvlBuf), mySlot, revealParam);
        Log.notice("REVEAL pid=%d param=%d bytes=%d", mySlot, (int)revealParam, rvlLen);
      }
      // A flare is everyone's business: every phone draws it (or its glow).
      // The firer's own client fires it from this ack, so it ignores its own.
      if (ok && usedType == ITEM_BRIGHT_BAD_IDEA) {
        GameEvent fev = {}; fev.type = EVT_FLARE; fev.pid = (uint8_t)mySlot;
        fev.q = pl.q; fev.r = pl.r; fev.amt = pl.depth;
        enqEvt(fev);
      }
      int ap = appendFmt(ack, sizeof(ack), 0,
        "{\"t\":\"item_result\",\"ok\":%s,\"act\":\"use\",\"slot\":%d,\"pid\":%d,",
        ok?"true":"false", slotIdx, mySlot);
      ap = appendPackArrays(ack, sizeof(ack), ap, mySlot);
      if (!ok && *s_useWhy) ap = appendFmt(ack, sizeof(ack), ap, ",\"why\":\"%s\"", s_useWhy);
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
  // After the ack, so the pack is settled before the ground opens up. Over
  // 4 KB, so the copy text() makes comes out of PSRAM (dev-loop.md "Heap").
  if (rvlLen > 0) client->text(rvlBuf, (size_t)rvlLen);
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
      ap = appendEquipTail(ack, sizeof(ack), ap, mySlot, ok);
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
      ap = appendEquipTail(ack, sizeof(ack), ap, mySlot, ok);
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
      { int16_t bq, br; boardPos(pl, &bq, &br); gq = bq; gr = br; }   // the tunnel cell, below
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
// dropResource() puts them back on the hex when it can hold them -- the tunnel
// cell, below; the "rsp" broadcast is what tells every client the pile is
// there (same event the respawn tick uses, plus "dp" for the board).
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
      // The cell it went on, on the board it went on: "dp":1 tells clients to
      // patch tunnelMap rather than the surface hex of the hatch overhead.
      int16_t bq, br;
      const uint8_t bd = boardPos(pl, &bq, &br);
      if (dropped && onGround) {
        snprintf(upd, sizeof(upd),
          "{\"t\":\"ev\",\"k\":\"rsp\",\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d,\"dp\":%d}",
          (int)bq, (int)br, res, (int)rem, (int)bd);
      }
      snprintf(ack, sizeof(ack),
        "{\"t\":\"res_result\",\"ok\":%s,\"pid\":%d,\"res\":%d,\"qty\":%d,"
        "\"grd\":%d,\"rem\":%d,\"q\":%d,\"r\":%d,\"dp\":%d,\"inv\":[%d,%d,%d,%d,%d],\"sc\":%d}",
        dropped ? "true" : "false", mySlot, res, (int)dropped,
        onGround ? 1 : 0, (int)rem, (int)bq, (int)br, (int)bd,
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
      { int16_t bq, br; boardPos(pl2, &bq, &br); gq = bq; gr = br; }
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
