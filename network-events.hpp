#pragma once
// ── Event queue drain: Core-1 async broadcast interface ──────────────────────
// Included from Esp32HexMapCrawl.ino after network-persistence.hpp.
// Has access to all globals, constants, structs, and functions defined above it.
// NOTE: enqEvt() and the event queue globals (pendingEvents[], pendingCount,
//       evtMux) remain in Esp32HexMapCrawl.ino — all game_logic files depend on
//       them and are included before this file.

// ── Drain event queue ─────────────────────────────────────────────────────────
static void drainEvents() {
  int16_t  pq[MAX_PLAYERS], pr[MAX_PLAYERS];
  bool     conn[MAX_PLAYERS];
  uint32_t wsId[MAX_PLAYERS];
  int      visR[MAX_PLAYERS];

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      conn[i] = G.players[i].connected;
      pq[i]   = G.players[i].q;
      pr[i]   = G.players[i].r;
      wsId[i] = G.players[i].wsClientId;
      if (conn[i]) {
        bool mr;
        playerVisParams(i, &visR[i], &mr);
      } else {
        visR[i] = 0;
      }
    }
    xSemaphoreGive(G.mutex);
  }

  // Atomically snapshot the event queue so Core-0 connect/disconnect handlers
  // can enqueue safely while we drain on Core-1 without a race on pendingCount.
  GameEvent snapshot[EVT_QUEUE_SIZE];
  int snapCount = 0;
  taskENTER_CRITICAL(&evtMux);
  snapCount = pendingCount;
  if (snapCount > 0) memcpy(snapshot, pendingEvents, snapCount * sizeof(GameEvent));
  pendingCount = 0;
  taskEXIT_CRITICAL(&evtMux);

  char buf[160];
  for (int i = 0; i < snapCount; i++) {
    GameEvent& ev = snapshot[i];
    int len = 0;
    switch (ev.type) {

      case EVT_COLLECT:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"col\",\"pid\":%d,\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d}",
          ev.pid, ev.q, ev.r, ev.res, ev.amt);
        ws.textAll(buf, len);
        break;

      case EVT_RESPAWN:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"rsp\",\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d}",
          ev.q, ev.r, ev.res, ev.amt);
        for (int pid = 0; pid < MAX_PLAYERS; pid++) {
          if (!conn[pid]) continue;
          if (hexDistWrap(pq[pid], pr[pid], ev.q, ev.r) > visR[pid]) continue;
          AsyncWebSocketClient* cl = ws.client(wsId[pid]);
          if (cl) cl->text(buf, len);
        }
        break;

      case EVT_MOVE:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"mv\",\"pid\":%d,\"q\":%d,\"r\":%d,\"radd\":%d,\"rad\":%d,\"exploD\":%d,\"mp\":%d}",
          ev.pid, ev.q, ev.r, (int)ev.radD, (int)ev.radR, (int)ev.exploD, (int)ev.moveMP);
        ws.textAll(buf, len);
        break;

      case EVT_JOINED: {
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"join\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d joined", (int)ev.pid);
          k10LogAdd(lb); }
        k10PlaySeq(SEQ_JOIN);
        break;
      }

      case EVT_LEFT: {
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d left", (int)ev.pid);
          k10LogAdd(lb); }
        break;
      }

      case EVT_DAWN: {
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"dawn\",\"pid\":%d,\"day\":%d,"
          "\"f\":%d,\"w\":%d,\"ll\":%d,\"mp\":%d,\"dll\":%d,\"fth\":%d,\"wth\":%d,"
          "\"rad\":%d,\"fat\":%d,\"expd\":%d}",
          ev.pid, (int)ev.dawnDay,
          (int)ev.dawnF, (int)ev.dawnW, (int)ev.dawnLL,
          (int)ev.dawnMP, (int)ev.dawnLLDelta,
          (int)ev.dawnFth, (int)ev.dawnWth,
          (int)ev.radR, (int)ev.dawnFat, (int)ev.dawnExpD);
        ws.textAll(buf, len);
        // K10 event log — only once per day (pid==0 guards double-logging for 6-player dawn)
        if (ev.pid == 0) {
          char lb[34]; snprintf(lb, sizeof(lb), "Day %d dawn", (int)ev.dawnDay);
          k10LogAdd(lb);
        }
        break;
      }

      case EVT_DUSK:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"dusk\",\"pid\":%d,\"out\":%d,"
          "\"dn\":%d,\"tot\":%d,\"ll\":%d,\"lld\":%d,\"rad\":%d}",
          ev.pid, (int)ev.actOut,
          (int)ev.actDn, (int)ev.actTot,
          (int)ev.actNewLL, (int)ev.actLLD, (int)ev.radR);
        ws.textAll(buf, len);
        break;

      case EVT_ACTION: {
        // K10 event log — brief action summary
        static const char* ACT_SHORT[8] = {"FORAGE","WATER","?","SCAV","SHELTER","?","SURVEY","REST"};
        const char* aShort = (ev.actType < 8) ? ACT_SHORT[ev.actType] : "?";
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d: %s%s",
            (int)ev.pid, aShort, ev.actOut ? " OK" : " FAIL");
          k10LogAdd(lb); }
      }
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"act\",\"pid\":%d,\"a\":%d,\"out\":%d,"
          "\"mp\":%d,\"ll\":%d,\"fat\":%d,\"fd\":%d,\"wd\":%d,\"lld\":%d,"
          "\"dn\":%d,\"tot\":%d,\"radd\":%d,\"rad\":%d,\"cnd\":%d,\"resd\":%d,\"sd\":%d,\"scoreD\":%d}",
          ev.pid, (int)ev.actType, (int)ev.actOut,
          (int)ev.actNewMP, (int)ev.actNewLL, (int)ev.actNewFat,
          (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actLLD,
          (int)ev.actDn, (int)ev.actTot,
          (int)ev.radD, (int)ev.radR,
          (int)ev.actCnd, (int)ev.actResD, (int)ev.actScrapD, (int)ev.actScoreD);
        ws.textAll(buf, len);
        break;

      case EVT_DOWNED: {
        // 1. Send targeted "downed" message to the player's client
        {
          len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"downed\",\"pid\":%d}", (int)ev.pid);
          for (AsyncWebSocketClient& cl : ws.getClients()) {
            if (cl.id() == ev.evWsId) { cl.text(buf, len); break; }
          }
        }
        // 2. Broadcast EVT_LEFT so all clients remove the player icon
        len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", (int)ev.pid);
        ws.textAll(buf, len);
        // 3. Reset slot so it's available for re-pick; move client back to lobby
        if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          Player& p = G.players[ev.pid];
          p.connected  = false;
          p.wsClientId = 0;
          p.resting    = false;  // clear stale resting flag on disconnect
          G.connectedCount--;
          xSemaphoreGive(G.mutex);
        }
        taskENTER_CRITICAL(&evtMux);
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (!lobbyIds[i]) { lobbyIds[i] = ev.evWsId; break; }
        }
        taskEXIT_CRITICAL(&evtMux);
        // 4. Tell all lobby clients (including the downed player) archetypes now available
        broadcastLobbyUpdate();
        Serial.printf("[DOWNED]  Slot %d reset — client %lu returned to lobby\n",
          (int)ev.pid, (unsigned long)ev.evWsId);
        break;
      }

      case EVT_REGEN: {
        // Regen requested — generateMap() already called on Core 1 before enqueue
        // Broadcast regen event then send full syncs to all connected clients
        len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"regen\"}");
        ws.textAll(buf, len);
        for (AsyncWebSocketClient& cl : ws.getClients()) {
          int slot = findSlot(cl.id());
          if (slot >= 0) sendSync(&cl, slot);
        }
        break;
      }

      case EVT_TRADE_OFFER: {
        char tbuf[192]; int tlen;
        tlen = snprintf(tbuf, sizeof(tbuf),
          "{\"t\":\"ev\",\"k\":\"trd_off\","
          "\"from\":%d,\"to\":%d,"
          "\"give\":[%d,%d,%d,%d,%d],"
          "\"want\":[%d,%d,%d,%d,%d]}",
          (int)ev.pid, (int)ev.tradeTo,
          ev.tradeGive[0], ev.tradeGive[1], ev.tradeGive[2],
          ev.tradeGive[3], ev.tradeGive[4],
          ev.tradeWant[0], ev.tradeWant[1], ev.tradeWant[2],
          ev.tradeWant[3], ev.tradeWant[4]);
        ws.textAll(tbuf, tlen);
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d\xE2\x86\x92P%d offer", (int)ev.pid, (int)ev.tradeTo);
          k10LogAdd(lb); }
        break;
      }

      case EVT_TRADE_RESULT: {
        char tbuf[96]; int tlen;
        tlen = snprintf(tbuf, sizeof(tbuf),
          "{\"t\":\"ev\",\"k\":\"trd_res\","
          "\"from\":%d,\"to\":%d,\"res\":%d}",
          (int)ev.pid, (int)ev.tradeTo, (int)ev.tradeResult);
        ws.textAll(tbuf, tlen);
        static const char* TR_LABEL[4] = {"?", "DONE", "DECLINED", "EXPIRED"};
        const char* rl = (ev.tradeResult < 4) ? TR_LABEL[ev.tradeResult] : "?";
        { char lb[40]; snprintf(lb, sizeof(lb), "Trade P%d\xE2\x86\x94P%d %s",
            (int)ev.pid, (int)ev.tradeTo, rl);
          k10LogAdd(lb); }
        break;
      }

      case EVT_NAME:
        break; // name changes broadcast via broadcastState(); no dedicated event message needed
    }
  }
  // pendingCount was already reset to 0 inside the spinlock snapshot above.
}
