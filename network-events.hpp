#pragma once
// ── Event queue drain: Core-1 async broadcast interface ──────────────────────
// Included from Esp32HexMapCrawl.ino after network-persistence.hpp.
// Has access to all globals, constants, structs, and functions defined above it.
// NOTE: enqEvt() and the event queue globals (pendingEvents[], pendingCount,
//       evtMux) remain in Esp32HexMapCrawl.ino — all game_logic files depend on
//       them and are included before this file.

// ── Chronicle phrasing for the eight action types ────────────────────────────
// Index matches ev.actType (5 is unused). Each returns a predicate — the
// screen supplies the name in front of it. Keep these under ~46 chars so they
// fit K10LogEntry::text.
static const char* actProse(uint8_t type, bool ok) {
  switch (type) {
    case 0: return ok ? K10_SAY("coaxes a meal out of dead ground.",
                                "finds what the birds missed.",
                                "brings back roots, and they are enough.")
                      : K10_SAY("turns over stones and finds only stones.",
                                "comes back with dirt and nothing else.",
                                "goes hungry for the trying.");
    case 1: return ok ? K10_SAY("draws water clean enough to keep.",
                                "finds a seep still running.",
                                "fills the cans and does not hurry back.")
                      : K10_SAY("tastes the water and spits it out.",
                                "finds the well dry to the stone.",
                                "comes back with the cans light.");
    case 2: return ok ? K10_SAY("binds the wound and it holds.",
                                "stitches what can be stitched.",
                                "cleans the rot out before it spreads.")
                      : K10_SAY("does what can be done. It is not much.",
                                "runs out of clean cloth.",
                                "cannot stop the bleeding for long.");
    case 3: return ok ? K10_SAY("pries something useful from the wreck.",
                                "strips the ruin down to its good bones.",
                                "finds a cache nobody else did.")
                      : K10_SAY("finds the ruin picked clean already.",
                                "cuts a hand on rusted nothing.",
                                "comes out of the wreck empty.");
    case 4: return ok ? K10_SAY("raises a roof against the night.",
                                "makes a place out of the wind.",
                                "builds it low and builds it to last.")
                      : K10_SAY("loses the frame to the wind.",
                                "builds it twice and it falls twice.",
                                "gives up on the shelter before dark.");
    case 6: return ok ? K10_SAY("reads the land and marks the map.",
                                "takes the high ground and looks long.",
                                "puts a name to what was blank.")
                      : K10_SAY("climbs high and sees only haze.",
                                "loses the horizon to dust.",
                                "maps nothing worth keeping.");
    case 7: return     K10_SAY("sleeps, badly, and wakes anyway.",
                               "rests. The dark passes over.",
                               "lies down and lets the day go.");
    default: return ok ? "does the work and it comes good."
                       : "does the work and it comes to nothing.";
  }
}

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

  // Longest payload is enc_res at ~207 chars worst case; 288 leaves headroom.
  char buf[288];
  for (int i = 0; i < snapCount; i++) {
    GameEvent& ev = snapshot[i];
    int len = 0;
    switch (ev.type) {

      case EVT_COLLECT:
        Log.notice("EVT col pid=%d q=%d r=%d res=%d amt=%d rem=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt, (int)ev.dawnLL);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"col\",\"pid\":%d,\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d,\"rem\":%d}",
          ev.pid, ev.q, ev.r, ev.res, ev.amt, (int)ev.dawnLL);
        ws.textAll(buf, len);
        break;

      case EVT_COLLECT_FAIL:
        Log.notice("EVT col_fail pid=%d q=%d r=%d res=%d reason=%d cap=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt, (int)ev.dawnLL);
        // Sent only to the player who attempted the pickup — others don't need to know.
        // `cap` (dawnLL) is the effective pack size; 0 for the desync reason.
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"col_fail\",\"pid\":%d,\"q\":%d,\"r\":%d,\"res\":%d,\"reason\":%d,\"cap\":%d}",
          ev.pid, ev.q, ev.r, ev.res, ev.amt, (int)ev.dawnLL);
        if (ev.pid < MAX_PLAYERS && conn[ev.pid]) {
          AsyncWebSocketClient* cl = ws.client(wsId[ev.pid]);
          if (cl) cl->text(buf, len);
        }
        break;

      case EVT_RESPAWN: {
        // Broadcast to all clients (no vision cull) so out-of-range players
        // clear their stale `collectedCells` Set entry — without this the
        // hex's icon never returns when they walk back into vision.
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"rsp\",\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d}",
          ev.q, ev.r, ev.res, ev.amt);
        ws.textAll(buf, len);
        Log.verbose("EVT rsp q=%d r=%d res=%d amt=%d (broadcast)",
                    (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt);
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
        k10LogAdd(K10_SAY("takes up the road with us.",
                          "arrives out of the haze, still walking.",
                          "falls in with the line of march."),
                  (int8_t)ev.pid, TONE_GOOD);
        k10Play(MOTIF_SEWER_ECHO);
        break;
      }

      case EVT_LEFT: {
        Log.notice("EVT left pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("walks out and does not look back.",
                          "is gone before the fire burns down.",
                          "leaves an empty place at the watch."),
                  (int8_t)ev.pid, TONE_PLAIN);
        break;
      }

      case EVT_DAWN: {
        Log.notice("EVT dawn day=%d pid=%d f=%d w=%d ll=%d mp=%d",
                   (int)ev.dawnDay, (int)ev.pid, (int)ev.dawnF,
                   (int)ev.dawnW, (int)ev.dawnLL, (int)ev.dawnMP);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"dawn\",\"pid\":%d,\"day\":%d,"
          "\"f\":%d,\"w\":%d,\"ll\":%d,\"mp\":%d,\"dll\":%d,\"fth\":%d,\"wth\":%d,"
          "\"rad\":%d,\"expd\":%d,\"wnd\":[%d,%d]}",
          ev.pid, (int)ev.dawnDay,
          (int)ev.dawnF, (int)ev.dawnW, (int)ev.dawnLL,
          (int)ev.dawnMP, (int)ev.dawnLLDelta,
          (int)ev.dawnFth, (int)ev.dawnWth,
          (int)ev.radR, (int)ev.dawnExpD,
          (int)ev.dawnWndMin, (int)ev.dawnWndMaj);
        ws.textAll(buf, len);
        // Chronicle — only once per day (pid==0 guards double-logging for 6-player dawn)
        if (ev.pid == 0) {
          char lb[48];
          snprintf(lb, sizeof(lb),
                   K10_SAY("Day %d comes up grey over the waste.",
                           "Day %d. Thin light, and we are still here.",
                           "Another sun. Day %d begins."),
                   (int)ev.dawnDay);
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
        static const char* ACT_SHORT[8] = {"FORAGE","WATER","TREAT","SCAV","SHELTER","?","SURVEY","REST"};
        const char* aShort = (ev.actType < 8) ? ACT_SHORT[ev.actType] : "?";
        Log.notice("EVT act pid=%d type=%s(%d) out=%d ll=%d mp=%d fd=%d wd=%d scoreD=%d",
                   (int)ev.pid, aShort, (int)ev.actType, (int)ev.actOut,
                   (int)ev.actNewLL, (int)ev.actNewMP,
                   (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actScoreD);
        // Chronicle — REST has no failure state, so it stays neutral ink.
        k10LogAdd(actProse(ev.actType, ev.actOut), (int8_t)ev.pid,
                  (ev.actType == 7) ? TONE_PLAIN : (ev.actOut ? TONE_GOOD : TONE_ILL));
      }
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"act\",\"pid\":%d,\"a\":%d,\"out\":%d,"
          "\"mp\":%d,\"ll\":%d,\"fd\":%d,\"wd\":%d,\"lld\":%d,"
          "\"dn\":%d,\"tot\":%d,\"radd\":%d,\"rad\":%d,\"cnd\":%d,\"sd\":%d,"
          "\"md\":%d,\"wnd\":[%d,%d],\"scoreD\":%d}",
          ev.pid, (int)ev.actType, (int)ev.actOut,
          (int)ev.actNewMP, (int)ev.actNewLL,
          (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actLLD,
          (int)ev.actDn, (int)ev.actTot,
          (int)ev.radD, (int)ev.radR,
          (int)ev.actCnd, (int)ev.actScrapD,
          (int)ev.actMedD, (int)ev.actWndMin, (int)ev.actWndMaj,
          (int)ev.actScoreD);
        ws.textAll(buf, len);
        break;

      case EVT_DOWNED: {
        Log.warning("EVT downed pid=%d wsId=%u lobby-moved",
                    (int)ev.pid, (unsigned)ev.evWsId);
        // 1. Send targeted "downed" message to the player's client
        {
          len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"downed\",\"pid\":%d}", (int)ev.pid);
          if (AsyncWebSocketClient* cl = ws.client(ev.evWsId)) {
            cl->text(buf, len);
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
        ledPerish();  // red heartbeat across all 3 lamps — see ui-leds.hpp
        break;
      }

      case EVT_REGEN: {
        // Regen requested — generateMap() already called on Core 1 before enqueue
        // Broadcast regen event then send full syncs to all connected clients
        len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"regen\"}");
        ws.textAll(buf, len);
        k10Play(MOTIF_POWER_DOWN);
        int synced = 0;
        for (const auto& cl : ws.getClients()) {
          uint32_t cid = cl.id();
          int slot = findSlot(cid);
          if (slot >= 0) {
            if (AsyncWebSocketClient* mut = ws.client(cid)) { sendSync(mut, slot); synced++; }
          }
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
        k10LogAdd(K10_SAY("holds out a bargain to \x01.",
                          "names a price to \x01.",
                          "offers \x01 a trade and waits."),
                  (int8_t)ev.pid, TONE_PLAIN, (int8_t)ev.tradeTo);
        break;
      }

      case EVT_TRADE_RESULT: {
        static const char* TRL[5] = {"?","DONE","DECLINED","EXPIRED","FAILED"};
        Log.notice("EVT trd_res from=%d to=%d result=%s",
                   (int)ev.pid, (int)ev.tradeTo,
                   (ev.tradeResult < 5) ? TRL[ev.tradeResult] : "?");
        char tbuf[96]; int tlen;
        tlen = snprintf(tbuf, sizeof(tbuf),
          "{\"t\":\"ev\",\"k\":\"trd_res\","
          "\"from\":%d,\"to\":%d,\"res\":%d}",
          (int)ev.pid, (int)ev.tradeTo, (int)ev.tradeResult);
        ws.textAll(tbuf, tlen);
        const char* trProse;
        uint8_t     trTone;
        switch (ev.tradeResult) {
          case 1:  trProse = K10_SAY("and \x01 strike a bargain.",
                                     "and \x01 shake on it.");
                   trTone  = TONE_GOOD; break;
          case 2:  trProse = K10_SAY("is turned down flat by \x01.",
                                     "asks. \x01 says no.");
                   trTone  = TONE_PLAIN; break;
          case 3:  trProse = K10_SAY("waits on \x01. Nothing comes.",
                                     "lets the offer to \x01 go cold.");
                   trTone  = TONE_PLAIN; break;
          default: trProse = K10_SAY("and \x01 cannot make it work.",
                                     "and \x01 walk away from it.");
                   trTone  = TONE_ILL; break;
        }
        k10LogAdd(trProse, (int8_t)ev.pid, trTone, (int8_t)ev.tradeTo);
        break;
      }

      case EVT_ENC_START:
        Log.notice("EVT enc_start pid=%d q=%d r=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_start\",\"pid\":%d,\"q\":%d,\"r\":%d}",
          ev.pid, (int)ev.q, (int)ev.r);
         
        ws.textAll(buf, len);
         
        k10LogAdd(K10_SAY("steps off the map and into the dark.",
                          "goes in where the light stops.",
                          "crosses the threshold alone."),
                  (int8_t)ev.pid, TONE_OMEN);

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
          "\"it\":%d,\"iq\":%d,\"it2\":%d,\"iq2\":%d,\"penLL\":%d,\"penRad\":%d,"
          "\"penRes\":[%d,%d,%d,%d,%d],\"penWnd\":[%d,%d],"
          "\"ends\":%d,\"drains\":[%d,%d,%d,%d,%d,%d]}",
          ev.pid, (int)ev.encOut, (int)ev.encSkill,
          (int)ev.encDN, (int)ev.encTotal,
          ev.encLoot[0], ev.encLoot[1], ev.encLoot[2], ev.encLoot[3], ev.encLoot[4],
          (int)ev.encItemType, (int)ev.encItemQty,
          (int)ev.encItemType2, (int)ev.encItemQty2,
          (int)ev.encPenLL, (int)ev.encPenRad,
          ev.encPenRes[0], ev.encPenRes[1], ev.encPenRes[2], ev.encPenRes[3], ev.encPenRes[4],
          (int)ev.encPenWndMin, (int)ev.encPenWndMaj,
          (int)ev.encEnds,
          (int)ev.encDrains[0], (int)ev.encDrains[1], (int)ev.encDrains[2],
          (int)ev.encDrains[3], (int)ev.encDrains[4], (int)ev.encDrains[5]);
        ws.textAll(buf, len);
        // Success reads through the skill that carried it.
        static const char* ENC_WON[5] = {
          "picks the safe line through.",
          "reads the ground right and lives on it.",
          "gets the panel open and the dark gives.",
          "finds cover before it matters.",
          "takes it and keeps standing.",
        };
        if (ev.encOut) {
          k10LogAdd(ENC_WON[(ev.encSkill < 5) ? ev.encSkill : 0],
                    (int8_t)ev.pid, TONE_GOOD);
          k10Play(MOTIF_DARK_DEPART);
        } else if (ev.encEnds) {
          k10LogAdd(K10_SAY("is thrown back into daylight, bleeding.",
                            "comes out the way they went in, worse."),
                    (int8_t)ev.pid, TONE_ILL);
          k10Play(MOTIF_BROKEN_TECH);
        } else {
          k10LogAdd(K10_SAY("takes a hard turn and presses on.",
                            "is hurt by the place and stays in it."),
                    (int8_t)ev.pid, TONE_ILL);
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
          char lb[48];
          snprintf(lb, sizeof(lb), "clears the place out entire. +%d.", (int)ev.actScoreD);
          k10LogAdd(lb, (int8_t)ev.pid, TONE_GOOD);
          k10Play(MOTIF_WEIRD_ANOMALY);
        } else {
          k10LogAdd(K10_SAY("carries the haul back into the light.",
                            "brings out what the dark was keeping."),
                    (int8_t)ev.pid, TONE_GOOD);
        }
        break;
      }

      case EVT_ENC_END: {
        static const char* REASON[ENC_END_COUNT] = {"hazard","abort","dawn","downed","disconnect","regen"};
        const char* reason = (ev.encOut < ENC_END_COUNT) ? REASON[ev.encOut] : "?";
        if (ev.encOut == ENC_END_DOWNED || ev.encOut == ENC_END_DISCONNECT)
          Log.warning("EVT enc_end pid=%d reason=%s", (int)ev.pid, reason);
        else
          Log.notice("EVT enc_end pid=%d reason=%s", (int)ev.pid, reason);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_end\",\"pid\":%d,\"q\":%d,\"r\":%d,\"reason\":\"%s\"}",
          ev.pid, (int)ev.q, (int)ev.r, reason);
        ws.textAll(buf, len);
        if (ev.encOut == ENC_END_ABORT) {
          k10LogAdd(K10_SAY("turns back before the dark takes more.",
                            "leaves it unfinished, and lives."),
                    (int8_t)ev.pid, TONE_PLAIN);
        } else if (ev.encOut == ENC_END_DAWN) {
          k10LogAdd(K10_SAY("comes out of it as the sun does.",
                            "is still walking when the light finds them."),
                    (int8_t)ev.pid, TONE_PLAIN);
        } else if (ev.encOut == ENC_END_DOWNED) {
          k10LogAdd(K10_SAY("does not come out standing.",
                            "falls in there, and the dark keeps it."),
                    (int8_t)ev.pid, TONE_ILL);
        }
        break;
      }

      case EVT_WEATHER: {
        static const char* WX[6] = {"CLEAR","RAIN","STORM","CHEM","STRANGLE FOG","FOG"};
        Log.notice("EVT weather phase=%s ticks=%d",
                   (ev.q < 6) ? WX[ev.q] : "?", (int)ev.r);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"weather\",\"phase\":%d,\"ticks\":%d}",
          (int)ev.q, (int)ev.r);
        ws.textAll(buf, len);
        // Weather is written as the world's own line — no name in front of it.
        static const char* WX_PROSE[6] = {
          "The sky clears. Small mercy, and brief.",
          "Rain comes in thin and cold.",
          "A storm walks in off the flats.",
          "The rain turns wrong. Chem burn.",
          "Strangle fog settles in the low ground.",
          "Fog closes the world to arm's length.",
        };
        k10LogAdd(WX_PROSE[(ev.q < 6) ? ev.q : 0], -1,
                  (ev.q == 0) ? TONE_GOOD : TONE_OMEN);
        if (ev.q == WEATHER_STORM) k10Play(MOTIF_MUTANT_BREATH); else k10Play(MOTIF_DISTANT_THUD);
        // Announce the new phase on the lamps in its own signature colour; the
        // ambient sky picks the phase up on the next updateLEDs() tick anyway.
        { uint8_t wr, wg, wb; weatherFlashColour((uint8_t)ev.q, wr, wg, wb);
          ledFlash(wr, wg, wb); }
        break;
      }
      case EVT_CARAVAN_TRADE: {
        Log.notice("EVT car_avail pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"car_avail\",\"pid\":%d}", (int)ev.pid);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("meets a caravan on the road.",
                          "falls in with traders for an hour."),
                  (int8_t)ev.pid, TONE_GOOD);
        break;
      }

      case EVT_FIRE_DAMAGE: {
        Log.notice("EVT fire_dmg pid=%d q=%d r=%d intensity=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"fire_dmg\",\"pid\":%d,\"q\":%d,\"r\":%d,\"intensity\":%d}",
          (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.amt);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("is caught in the burn.",
                          "walks into fire and wears it out.",
                          "comes through the flames marked."),
                  (int8_t)ev.pid, TONE_ILL);
        break;
      }

      case EVT_FIRE_SPREAD: {
        // Vision-culled: a fire's location is map information, and revealing
        // it to a player who hasn't explored that hex would leak outside
        // their fog of war — unlike EVT_FIRE_DAMAGE above (about a specific
        // player who is already there).
        Log.verbose("EVT fire_spread q=%d r=%d intensity=%d", (int)ev.q, (int)ev.r, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"fire_spread\",\"q\":%d,\"r\":%d,\"intensity\":%d}",
          (int)ev.q, (int)ev.r, (int)ev.amt);
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (!conn[i]) continue;
          if (hexDistWrap(pq[i], pr[i], ev.q, ev.r) > visR[i]) continue;
          AsyncWebSocketClient* cl = ws.client(wsId[i]);
          if (cl) cl->text(buf, len);
        }
        break;
      }

      case EVT_DOOM_WARNING: {
        // Broadcast to everyone regardless of position — Creeping Doom is a
        // world-level threat, not a local one (per the spec). K10 LED/motif
        // already fired inline in resolveDoomProximity() (world-system.hpp)
        // at the moment of detection, so this is just the log + WS notice.
        Log.notice("EVT doom_warn pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"doom_warn\",\"pid\":%d}", (int)ev.pid);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("feels the Doom turn its head.",
                          "goes quiet. Something out there noticed."),
                  (int8_t)ev.pid, TONE_OMEN);
        break;
      }

      case EVT_DOOM_ACT: {
        // amt = LL actually lost this act (0 or 1) — resource destruction
        // always happens at this awareness tier, LL loss only at 100.
        Log.notice("EVT doom_act pid=%d q=%d r=%d llLost=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"doom_act\",\"pid\":%d,\"q\":%d,\"r\":%d,\"llLost\":%d}",
          (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.amt);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("loses something to the Doom.",
                          "pays the Doom what it came for."),
                  (int8_t)ev.pid, TONE_OMEN);
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
