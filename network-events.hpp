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
  } else {
    Log.warning("drainEvents: G.mutex timeout - using stale player snapshot");
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

  if (snapCount > 0) Log.verbose("drainEvents: %d pending", snapCount);

  char buf[256];
  for (int i = 0; i < snapCount; i++) {
    GameEvent& ev = snapshot[i];
    int len = 0;
    switch (ev.type) {

      case EVT_COLLECT:
        Log.notice("EVT col pid=%d q=%d r=%d res=%d amt=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"col\",\"pid\":%d,\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d}",
          ev.pid, ev.q, ev.r, ev.res, ev.amt);
        ws.textAll(buf, len);
        break;

      case EVT_RESPAWN: {
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"rsp\",\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d}",
          ev.q, ev.r, ev.res, ev.amt);
        int rcpt = 0;
        for (int pid = 0; pid < MAX_PLAYERS; pid++) {
          if (!conn[pid]) continue;
          if (hexDistWrap(pq[pid], pr[pid], ev.q, ev.r) > visR[pid]) continue;
          AsyncWebSocketClient* cl = ws.client(wsId[pid]);
          if (cl) { cl->text(buf, len); rcpt++; }
        }
        Log.verbose("EVT rsp q=%d r=%d res=%d amt=%d recipients=%d",
                    (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt, rcpt);
        break;
      }

      case EVT_MOVE:
        Log.verbose("EVT mv pid=%d ->(%d,%d) rad=%d explo=%d mp=%d",
                    (int)ev.pid, (int)ev.q, (int)ev.r,
                    (int)ev.radR, (int)ev.exploD, (int)ev.moveMP);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"mv\",\"pid\":%d,\"q\":%d,\"r\":%d,\"radd\":%d,\"rad\":%d,\"exploD\":%d,\"mp\":%d}",
          ev.pid, ev.q, ev.r, (int)ev.radD, (int)ev.radR, (int)ev.exploD, (int)ev.moveMP);
        ws.textAll(buf, len);
        break;

      case EVT_JOINED: {
        Log.notice("EVT join pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"join\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d joined", (int)ev.pid);
          k10LogAdd(lb); }
        k10Play(MOTIF_SEWER_ECHO);
        break;
      }

      case EVT_LEFT: {
        Log.notice("EVT left pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d left", (int)ev.pid);
          k10LogAdd(lb); }
        break;
      }

      case EVT_DAWN: {
        Log.notice("EVT dawn day=%d pid=%d f=%d w=%d ll=%d mp=%d",
                   (int)ev.dawnDay, (int)ev.pid, (int)ev.dawnF,
                   (int)ev.dawnW, (int)ev.dawnLL, (int)ev.dawnMP);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"dawn\",\"pid\":%d,\"day\":%d,"
          "\"f\":%d,\"w\":%d,\"ll\":%d,\"mp\":%d,\"dll\":%d,\"fth\":%d,\"wth\":%d,"
          "\"rad\":%d,\"expd\":%d}",
          ev.pid, (int)ev.dawnDay,
          (int)ev.dawnF, (int)ev.dawnW, (int)ev.dawnLL,
          (int)ev.dawnMP, (int)ev.dawnLLDelta,
          (int)ev.dawnFth, (int)ev.dawnWth,
          (int)ev.radR, (int)ev.dawnExpD);
        ws.textAll(buf, len);
        // K10 event log — only once per day (pid==0 guards double-logging for 6-player dawn)
        if (ev.pid == 0) {
          char lb[34]; snprintf(lb, sizeof(lb), "Day %d dawn", (int)ev.dawnDay);
          k10LogAdd(lb);
        }
        break;
      }

      case EVT_DUSK:
        Log.notice("EVT dusk pid=%d out=%d dn=%d tot=%d ll=%d",
                   (int)ev.pid, (int)ev.actOut, (int)ev.actDn,
                   (int)ev.actTot, (int)ev.actNewLL);
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
        Log.notice("EVT act pid=%d type=%s(%d) out=%d ll=%d mp=%d fd=%d wd=%d scoreD=%d",
                   (int)ev.pid, aShort, (int)ev.actType, (int)ev.actOut,
                   (int)ev.actNewLL, (int)ev.actNewMP,
                   (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actScoreD);
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d: %s%s",
            (int)ev.pid, aShort, ev.actOut ? " OK" : " FAIL");
          k10LogAdd(lb); }
      }
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"act\",\"pid\":%d,\"a\":%d,\"out\":%d,"
          "\"mp\":%d,\"ll\":%d,\"fd\":%d,\"wd\":%d,\"lld\":%d,"
          "\"dn\":%d,\"tot\":%d,\"radd\":%d,\"rad\":%d,\"cnd\":%d,\"sd\":%d,\"scoreD\":%d}",
          ev.pid, (int)ev.actType, (int)ev.actOut,
          (int)ev.actNewMP, (int)ev.actNewLL,
          (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actLLD,
          (int)ev.actDn, (int)ev.actTot,
          (int)ev.radD, (int)ev.radR,
          (int)ev.actCnd, (int)ev.actScrapD, (int)ev.actScoreD);
        ws.textAll(buf, len);
        break;

      case EVT_DOWNED: {
        Log.warning("EVT downed pid=%d wsId=%u lobby-moved",
                    (int)ev.pid, (unsigned)ev.evWsId);
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
        k10Play(MOTIF_DEAD_BATTERY);
        break;
      }

      case EVT_REGEN: {
        // Regen requested — generateMap() already called on Core 1 before enqueue
        // Broadcast regen event then send full syncs to all connected clients
        len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"regen\"}");
        ws.textAll(buf, len);
        k10Play(MOTIF_POWER_DOWN);
        int synced = 0;
        for (AsyncWebSocketClient& cl : ws.getClients()) {
          int slot = findSlot(cl.id());
          if (slot >= 0) { sendSync(&cl, slot); synced++; }
        }
        Log.notice("EVT regen - broadcasting full sync to %d clients", synced);
        break;
      }

      case EVT_TRADE_OFFER: {
        Log.notice("EVT trd_off from=%d to=%d give=[%d,%d,%d,%d,%d] want=[%d,%d,%d,%d,%d]",
                   (int)ev.pid, (int)ev.tradeTo,
                   ev.tradeGive[0], ev.tradeGive[1], ev.tradeGive[2],
                   ev.tradeGive[3], ev.tradeGive[4],
                   ev.tradeWant[0], ev.tradeWant[1], ev.tradeWant[2],
                   ev.tradeWant[3], ev.tradeWant[4]);
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
        static const char* TRL[4] = {"?","DONE","DECLINED","EXPIRED"};
        Log.notice("EVT trd_res from=%d to=%d result=%s",
                   (int)ev.pid, (int)ev.tradeTo,
                   (ev.tradeResult < 4) ? TRL[ev.tradeResult] : "?");
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
        Log.verbose("EVT name pid=%d (piggybacks on state broadcast)", (int)ev.pid);
        break; // name changes broadcast via broadcastState(); no dedicated event message needed

      case EVT_ENC_START:
        Log.notice("EVT enc_start pid=%d q=%d r=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_start\",\"pid\":%d,\"q\":%d,\"r\":%d}",
          ev.pid, (int)ev.q, (int)ev.r);
         
        ws.textAll(buf, len);
         
        { char lb[34]; snprintf(lb, sizeof(lb), "P%d enters encounter", (int)ev.pid);
          k10LogAdd(lb); }
         
        k10Play(MOTIF_DARK_ENTRY);
         
        break;

      case EVT_ENC_RESULT: {
        {
          static const char* SK[5] = {"NAV","FORAGE","SCAV","SHELT","ENDURE"};
          Log.notice("EVT enc_res pid=%d skill=%s out=%d dn=%d total=%d ends=%d penLL=%d penRad=%d",
                     (int)ev.pid, (ev.encSkill < 5) ? SK[ev.encSkill] : "?",
                     (int)ev.encOut, (int)ev.encDN, (int)ev.encTotal,
                     (int)ev.encEnds, (int)ev.encPenLL, (int)ev.encPenRad);
        }
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_res\",\"pid\":%d,\"out\":%d,\"skill\":%d,"
          "\"dn\":%d,\"tot\":%d,\"loot\":[%d,%d,%d,%d,%d],"
          "\"it\":%d,\"iq\":%d,\"penLL\":%d,\"penRad\":%d,"
          "\"ends\":%d,\"drains\":[%d,%d,%d,%d,%d,%d]}",
          ev.pid, (int)ev.encOut, (int)ev.encSkill,
          (int)ev.encDN, (int)ev.encTotal,
          ev.encLoot[0], ev.encLoot[1], ev.encLoot[2], ev.encLoot[3], ev.encLoot[4],
          (int)ev.encItemType, (int)ev.encItemQty,
          (int)ev.encPenLL, (int)ev.encPenRad,
          (int)ev.encEnds,
          (int)ev.encDrains[0], (int)ev.encDrains[1], (int)ev.encDrains[2],
          (int)ev.encDrains[3], (int)ev.encDrains[4], (int)ev.encDrains[5]);
        ws.textAll(buf, len);
        static const char* SK_SHORT[5] = {"NAV","FORAGE","SCAV","SHELT","ENDURE"};
        const char* sks = (ev.encSkill < 5) ? SK_SHORT[ev.encSkill] : "?";
        if (ev.encOut) {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d ENC: %s OK", (int)ev.pid, sks);
          k10LogAdd(lb);
          k10Play(MOTIF_DARK_DEPART);
        } else if (ev.encEnds) {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d ENC: HAZARD! Ejected", (int)ev.pid);
          k10LogAdd(lb);
          k10Play(MOTIF_BROKEN_TECH);
        } else {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d ENC: HAZARD (cont.)", (int)ev.pid);
          k10LogAdd(lb);
          k10Play(MOTIF_SYSTEM_FAULT);
        }
        break;
      }

      case EVT_ENC_BANK: {
        Log.notice("EVT enc_bank pid=%d q=%d r=%d scoreD=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.actScoreD);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_bank\",\"pid\":%d,\"q\":%d,\"r\":%d,"
          "\"loot\":[%d,%d,%d,%d,%d],\"scoreD\":%d}",
          ev.pid, (int)ev.q, (int)ev.r,
          ev.encLoot[0], ev.encLoot[1], ev.encLoot[2], ev.encLoot[3], ev.encLoot[4],
          (int)ev.actScoreD);
        ws.textAll(buf, len);
        if (ev.actScoreD >= 10 + 3) {  // full clear bonus present
          char lb[34]; snprintf(lb, sizeof(lb), "P%d FULL CLEAR! +%d", (int)ev.pid, (int)ev.actScoreD);
          k10LogAdd(lb);
          k10Play(MOTIF_WEIRD_ANOMALY);
        } else {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d banked enc loot", (int)ev.pid);
          k10LogAdd(lb);
        }
        break;
      }

      case EVT_ENC_END: {
        static const char* REASON[] = {"hazard","abort","dawn","downed","disconnect"};
        const char* reason = (ev.encOut < 5) ? REASON[ev.encOut] : "?";
        if (ev.encOut == 3 || ev.encOut == 4)
          Log.warning("EVT enc_end pid=%d reason=%s", (int)ev.pid, reason);
        else
          Log.notice("EVT enc_end pid=%d reason=%s", (int)ev.pid, reason);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_end\",\"pid\":%d,\"q\":%d,\"r\":%d,\"reason\":\"%s\"}",
          ev.pid, (int)ev.q, (int)ev.r, reason);
        ws.textAll(buf, len);
        if (ev.encOut == 1) {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d aborted encounter", (int)ev.pid);
          k10LogAdd(lb);
        } else if (ev.encOut == 2) {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d enc ended (dawn)", (int)ev.pid);
          k10LogAdd(lb);
        } else if (ev.encOut == 3) {
          char lb[34]; snprintf(lb, sizeof(lb), "P%d DOWNED in encounter", (int)ev.pid);
          k10LogAdd(lb);
        }
        break;
      }

      case EVT_WEATHER: {
        static const char* WX[4] = {"CLEAR","RAIN","STORM","CHEM"};
        Log.notice("EVT weather phase=%s ticks=%d",
                   (ev.q < 4) ? WX[ev.q] : "?", (int)ev.r);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"weather\",\"phase\":%d,\"ticks\":%d}",
          (int)ev.q, (int)ev.r);
        ws.textAll(buf, len);
        char lb[34]; snprintf(lb, sizeof(lb), "Weather: %s", (ev.q < 4) ? WX[ev.q] : "?");
        k10LogAdd(lb);
        if (ev.q == WEATHER_STORM) k10Play(MOTIF_MUTANT_BREATH); else k10Play(MOTIF_DISTANT_THUD);
        break;
      }
      default:
        Log.error("EVT UNKNOWN type=%d pid=%d", (int)ev.type, (int)ev.pid);
        break;
    }
  }
  if (snapCount > 0) Log.verbose("drainEvents: dispatched=%d", snapCount);
  // pendingCount was already reset to 0 inside the spinlock snapshot above.
}
