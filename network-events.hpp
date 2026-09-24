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
    case 5: return ok ? K10_SAY("puts something together that mostly works.",
                                "follows the scrawled notes and it holds.",
                                "assembles it exactly the way the notes said.")
                      : K10_SAY("can't make the pieces fit today.",
                                "is missing something the recipe needs.",
                                "gives up on the recipe for now.");
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
  // Condition, for the chronicle's plates: enc_res reports what a hazard cost
  // but not what it left behind, and the plate wants the figure after.
  uint8_t  pll[MAX_PLAYERS] = {0}, prad[MAX_PLAYERS] = {0};

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      conn[i] = G.players[i].connected;
      pq[i]   = G.players[i].q;
      pr[i]   = G.players[i].r;
      wsId[i] = G.players[i].wsClientId;
      pll[i]  = G.players[i].ll;
      prad[i] = G.players[i].radiation;
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
  // PSRAM, not the stack: at ~6 KB this was most of the GameLoop task's stack
  // (see its size in setup()). Only gameLoopTask calls drainEvents(), so one
  // static copy is safe.
  PSRAM_STATIC(GameEvent, snapshot, [EVT_QUEUE_SIZE]);
  int snapCount = 0;
  taskENTER_CRITICAL(&evtMux);
  snapCount = pendingCount;
  if (snapCount > 0) memcpy(snapshot, pendingEvents, snapCount * sizeof(GameEvent));
  pendingCount = 0;
  taskEXIT_CRITICAL(&evtMux);

  if (snapCount > 0) LOG_VERBOSE("drainEvents: %d pending", snapCount);

  // Longest payload is enc_res at ~207 chars worst case; 288 leaves headroom.
  char buf[288];
  for (int i = 0; i < snapCount; i++) {
    GameEvent& ev = snapshot[i];
    int len = 0;
    encStatsNote(ev);   // K10 Encounters screen tallies (ui-screens.hpp)
    switch (ev.type) {

      case EVT_COLLECT:
        Log.notice("EVT col pid=%d q=%d r=%d res=%d amt=%d rem=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt, (int)ev.dawnLL);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"col\",\"pid\":%d,\"q\":%d,\"r\":%d,\"res\":%d,\"amt\":%d,\"rem\":%d,\"dp\":%d}",
          ev.pid, ev.q, ev.r, ev.res, ev.amt, (int)ev.dawnLL, (int)ev.depth);
        ws.textAll(buf, len);
        break;

      case EVT_COLLECT_FAIL:
        Log.notice("EVT col_fail pid=%d q=%d r=%d res=%d reason=%d cap=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt, (int)ev.dawnLL);
        // Sent only to the player who attempted the pickup — others don't need to know.
        // `cap` (dawnLL) is the effective pack size; 0 for the desync reason.
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"col_fail\",\"pid\":%d,\"q\":%d,\"r\":%d,\"res\":%d,\"reason\":%d,\"cap\":%d,\"dp\":%d}",
          ev.pid, ev.q, ev.r, ev.res, ev.amt, (int)ev.dawnLL, (int)ev.depth);
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
        LOG_VERBOSE("EVT rsp q=%d r=%d res=%d amt=%d (broadcast)",
                    (int)ev.q, (int)ev.r, (int)ev.res, (int)ev.amt);
        break;
      }

      case EVT_MOVE:
        LOG_VERBOSE("EVT mv pid=%d ->(%d,%d) rad=%d explo=%d mp=%d trk=%d",
                    (int)ev.pid, (int)ev.q, (int)ev.r,
                    (int)ev.radR, (int)ev.exploD, (int)ev.moveMP, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"mv\",\"pid\":%d,\"q\":%d,\"r\":%d,\"radd\":%d,\"rad\":%d,\"exploD\":%d,\"mp\":%d,\"trk\":%d,\"dp\":%d}",
          ev.pid, ev.q, ev.r, (int)ev.radD, (int)ev.radR, (int)ev.exploD, (int)ev.moveMP, (int)ev.amt, (int)ev.depth);
        ws.textAll(buf, len);
        break;

      // ── Bunker tunnels ──
      // q/r are the SURFACE hatch in both cases (the player's q/r never
      // leave the surface board), so the client can draw a "went below here"
      // marker without needing the tunnel board at all.
      case EVT_TUNNEL_ENTER:
      case EVT_TUNNEL_EXIT: {
        bool down = (ev.type == EVT_TUNNEL_ENTER);
        Log.notice("EVT tun_%s pid=%d hatch=%d at (%d,%d) mp=%d",
                   down ? "in" : "out", (int)ev.pid, (int)ev.amt,
                   (int)ev.q, (int)ev.r, (int)ev.moveMP);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"%s\",\"pid\":%d,\"q\":%d,\"r\":%d,\"hatch\":%d,\"mp\":%d}",
          down ? "tun_in" : "tun_out", ev.pid, ev.q, ev.r, (int)ev.amt, (int)ev.moveMP);
        ws.textAll(buf, len);
        break;
      }

      case EVT_JOINED: {
        Log.notice("EVT join pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"join\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("takes up the road with us.",
                          "arrives out of the haze, still walking.",
                          "falls in with the line of march."),
                  (int8_t)ev.pid, TONE_GOOD, GLY_ARRIVE);
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
                  (int8_t)ev.pid, TONE_PLAIN, GLY_DEPART);
        break;
      }

      case EVT_DAWN: {
        Log.notice("EVT dawn day=%d pid=%d f=%d w=%d ll=%d mp=%d",
                   (int)ev.dawnDay, (int)ev.pid, (int)ev.dawnF,
                   (int)ev.dawnW, (int)ev.dawnLL, (int)ev.dawnMP);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"dawn\",\"pid\":%d,\"day\":%d,"
          "\"f\":%d,\"w\":%d,\"ll\":%d,\"mp\":%d,\"dll\":%d,\"fth\":%d,\"wth\":%d,"
          "\"rad\":%d,\"expd\":%d,\"air\":%d,\"wnd\":[%d,%d],\"unf\":%d}",
          ev.pid, (int)ev.dawnDay,
          (int)ev.dawnF, (int)ev.dawnW, (int)ev.dawnLL,
          (int)ev.dawnMP, (int)ev.dawnLLDelta,
          (int)ev.dawnFth, (int)ev.dawnWth,
          (int)ev.radR, (int)ev.dawnExpD, (int)ev.dawnAirD,
          (int)ev.dawnWndMin, (int)ev.dawnWndMaj,
          (int)ev.dawnUnfuelled);
        ws.textAll(buf, len);
        // Chronicle — only once per day (pid==0 guards double-logging for 6-player dawn)
        if (ev.pid == 0) {
          char lb[48];
          snprintf(lb, sizeof(lb),
                   K10_SAY("Day %d comes up grey over the waste.",
                           "Day %d. Thin light, and we are still here.",
                           "Another sun. Day %d begins."),
                   (int)ev.dawnDay);
          k10LogAdd(lb, -1, TONE_PLAIN, GLY_DAWN);
        }
        // Bad air is per-survivor, not per-day, so it sits outside the pid==0
        // guard -- two sleepers in the tunnels each get their own line. The
        // mark is the chem one: down there it is the same complaint.
        if (ev.dawnAirD < 0) {
          k10LogAdd(K10_SAY("wakes in the dark with burning lungs.",
                            "breathed bunker air all night and paid for it.",
                            "coughs the tunnel up and looks the worse for it."),
                    (int8_t)ev.pid, TONE_ILL, GLY_CHEM);
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
        static const char* ACT_SHORT[8] = {"FORAGE","WATER","TREAT","SCAV","SHELTER","CRAFT","SURVEY","REST"};
        const char* aShort = (ev.actType < 8) ? ACT_SHORT[ev.actType] : "?";
        Log.notice("EVT act pid=%d type=%s(%d) out=%d ll=%d mp=%d fd=%d wd=%d scoreD=%d",
                   (int)ev.pid, aShort, (int)ev.actType, (int)ev.actOut,
                   (int)ev.actNewLL, (int)ev.actNewMP,
                   (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actScoreD);
        // Chronicle — REST has no failure state, so it stays neutral ink.
        // The mark in the margin follows the work, not the outcome — a failed
        // forage is still a forage. Index matches ACT_SHORT above.
        static const uint8_t ACT_GLYPH[8] = {
          GLY_FORAGE, GLY_WATER, GLY_MEDIC, GLY_SALVAGE,
          GLY_SHELTER, GLY_CRAFT, GLY_SCOUT, GLY_REST,
        };
        k10LogAdd(actProse(ev.actType, ev.actOut), (int8_t)ev.pid,
                  (ev.actType == 7) ? TONE_PLAIN : (ev.actOut ? TONE_GOOD : TONE_ILL),
                  (ev.actType < 8) ? ACT_GLYPH[ev.actType] : GLY_NONE);
      }
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"act\",\"pid\":%d,\"a\":%d,\"out\":%d,"
          "\"mp\":%d,\"ll\":%d,\"fd\":%d,\"wd\":%d,\"lld\":%d,"
          "\"dn\":%d,\"tot\":%d,\"radd\":%d,\"rad\":%d,\"cnd\":%d,\"sd\":%d,"
          "\"md\":%d,\"wnd\":[%d,%d],\"scoreD\":%d,\"ar\":%d,\"bw\":%d}",
          ev.pid, (int)ev.actType, (int)ev.actOut,
          (int)ev.actNewMP, (int)ev.actNewLL,
          (int)ev.actFoodD, (int)ev.actWatD, (int)ev.actLLD,
          (int)ev.actDn, (int)ev.actTot,
          (int)ev.radD, (int)ev.radR,
          (int)ev.actCnd, (int)ev.actScrapD,
          (int)ev.actMedD, (int)ev.actWndMin, (int)ev.actWndMaj,
          (int)ev.actScoreD, (int)ev.actRecipe, (int)ev.actWhy);
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
        // 3. Reset slot so it's available for re-pick; move client back to lobby.
        //    Take the name on the way past -- the slot is about to be handed to
        //    whoever picks it up next, and the death screen wants who it was.
        char downedName[16] = {0};
        if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          Player& p = G.players[ev.pid];
          memcpy(downedName, p.name, sizeof(downedName));
          downedName[sizeof(downedName) - 1] = '\0';
          p.connected  = false;
          p.wsClientId = 0;
          p.resting    = false;  // clear stale resting flag on disconnect
          G.connectedCount--;
          xSemaphoreGive(G.mutex);
        }
        if (!downedName[0])
          snprintf(downedName, sizeof(downedName), "Walker %d", (int)ev.pid + 1);
        // The chronicle gets its line, and the panel gets given over to the
        // skull for six seconds -- see ui-death.hpp.
        k10LogAdd(K10_SAY("goes down, and does not get up.",
                          "is finished. The waste keeps them."),
                  (int8_t)ev.pid, TONE_ILL, GLY_DEATH);
        DeathUI::begin(downedName, "does not come out standing.");
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
                  (int8_t)ev.pid, TONE_PLAIN, GLY_TRADE, (int8_t)ev.tradeTo);
        break;
      }

      case EVT_TRADE_RESULT: {
        static const char* TRL[5] = {"?","DONE","DECLINED","EXPIRED","FAILED"};
        Log.notice("EVT trd_res from=%d to=%d result=%s item=%d n=%d",
                   (int)ev.pid, (int)ev.tradeTo,
                   (ev.tradeResult < 5) ? TRL[ev.tradeResult] : "?",
                   (int)ev.tradeItem, (int)ev.tradeItemQty);
        // "item"/"n" only ride along on a caravan purchase (car_buy) — a
        // plain resource swap keeps the original three-field shape.
        char tbuf[128]; int tlen;
        tlen = snprintf(tbuf, sizeof(tbuf),
          "{\"t\":\"ev\",\"k\":\"trd_res\","
          "\"from\":%d,\"to\":%d,\"res\":%d",
          (int)ev.pid, (int)ev.tradeTo, (int)ev.tradeResult);
        if (ev.tradeItem)
          tlen += snprintf(tbuf + tlen, sizeof(tbuf) - tlen,
                           ",\"item\":%d,\"n\":%d", (int)ev.tradeItem, (int)ev.tradeItemQty);
        tlen += snprintf(tbuf + tlen, sizeof(tbuf) - tlen, "}");
        ws.textAll(tbuf, tlen);
        // The caravan isn't a player slot (CARAVAN_PID), so bookName() would
        // write \x01 as "Someone" — give both caravan outcomes their own
        // phrasing instead of the survivor↔survivor lines below.
        if (ev.tradeTo == CARAVAN_PID) {
          char kb[48];
          if (ev.tradeItem) {
            static const char* const BUY_V[] = { "buys", "haggles for", "bargains for" };
            const ItemDef* bd = getItemDef(ev.tradeItem);
            snprintf(kb, sizeof(kb), "%s %s at the caravan.",
                     k10Pick(BUY_V, 3), bd ? bd->name : "goods");
          } else {
            snprintf(kb, sizeof(kb), "%s", K10_SAY("trades supplies with the caravan.",
                                                   "does business with the caravan."));
          }
          k10LogAdd(kb, (int8_t)ev.pid, TONE_GOOD, GLY_CARAVAN);
          break;
        }
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
        k10LogAdd(trProse, (int8_t)ev.pid, trTone, GLY_TRADE, (int8_t)ev.tradeTo);
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
                  (int8_t)ev.pid, TONE_OMEN, GLY_THRESHOLD);

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
          "\"ends\":%d,\"drains\":[%d,%d,%d,%d,%d,%d],\"rec\":%d}",
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
          (int)ev.encDrains[3], (int)ev.encDrains[4], (int)ev.encDrains[5],
          (int)ev.encRecipe);
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
                    (int8_t)ev.pid, TONE_GOOD, GLY_LIGHT);
          k10Play(MOTIF_DARK_DEPART);
        } else if (ev.encEnds) {
          k10LogAdd(K10_SAY("is thrown back into daylight, bleeding.",
                            "comes out the way they went in, worse."),
                    (int8_t)ev.pid, TONE_ILL, GLY_WOUND);
          k10Play(MOTIF_BROKEN_TECH);
        } else {
          k10LogAdd(K10_SAY("takes a hard turn and presses on.",
                            "is hurt by the place and stays in it."),
                    (int8_t)ev.pid, TONE_ILL, GLY_CLASH);
          k10Play(MOTIF_SYSTEM_FAULT);
        }
        {
          // What the room actually cost, set down as plates beside the prose.
          uint8_t pi = (ev.pid < MAX_PLAYERS) ? ev.pid : 0;
          if (ev.encPenWndMin || ev.encPenWndMaj) {
            uint8_t pv[3] = { ev.encPenWndMin, ev.encPenWndMaj, pll[pi] };
            k10LogPlate(PLATE_WOUND, "hurt in the dark", (int8_t)ev.pid,
                        TONE_ILL, pv, 3);
          }
          if (ev.encPenRad > 0) {
            uint8_t pv[3] = { (uint8_t)ev.encPenRad, prad[pi], RAD_CRITICAL };
            k10LogPlate(PLATE_RAD, "", (int8_t)ev.pid, TONE_OMEN, pv, 3);
          }
          if (ev.encOut && ev.encDN >= 8) {
            char cb[48];
            snprintf(cb, sizeof(cb), "DN %u held", (unsigned)ev.encDN);
            uint8_t pv[1] = { AWD_LONG_ODDS };
            k10LogPlate(PLATE_AWARD, cb, (int8_t)ev.pid, TONE_GOOD, pv, 1);
          }
        }
        break;
      }

      case EVT_ENC_BANK: {
        Log.notice("EVT enc_bank pid=%d q=%d r=%d scoreD=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.actScoreD);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"enc_bank\",\"pid\":%d,\"q\":%d,\"r\":%d,"
          "\"loot\":[%d,%d,%d,%d,%d],\"scoreD\":%d,\"recs\":%lu}",
          ev.pid, (int)ev.q, (int)ev.r,
          ev.encLoot[0], ev.encLoot[1], ev.encLoot[2], ev.encLoot[3], ev.encLoot[4],
          (int)ev.actScoreD, (unsigned long)ev.bankedRecipes);
        ws.textAll(buf, len);
        if (ev.actScoreD >= 10 + 3) {  // full clear bonus present
          char lb[48];
          snprintf(lb, sizeof(lb), "clears the place out entire. +%d.", (int)ev.actScoreD);
          k10LogAdd(lb, (int8_t)ev.pid, TONE_GOOD, GLY_HAUL);
          k10Play(MOTIF_WEIRD_ANOMALY);
        } else {
          k10LogAdd(K10_SAY("carries the haul back into the light.",
                            "brings out what the dark was keeping."),
                    (int8_t)ev.pid, TONE_GOOD, GLY_HAUL);
        }
        {
          // The tally gets its own plate, and a big one gets a commendation.
          unsigned all = 0;
          for (uint8_t i = 0; i < 5; i++) all += ev.encLoot[i];
          if (all > 0)
            k10LogPlate(PLATE_HAUL, "", (int8_t)ev.pid, TONE_GOOD, ev.encLoot, 5);
          char cb[48];
          if (ev.actScoreD >= 10 + 3) {
            snprintf(cb, sizeof(cb), "cleared it, +%d", (int)ev.actScoreD);
            uint8_t pv[1] = { AWD_SWEPT };
            k10LogPlate(PLATE_AWARD, cb, (int8_t)ev.pid, TONE_GOOD, pv, 1);
          } else if (all >= HAUL_HEAVY) {
            snprintf(cb, sizeof(cb), "%u carried out", all);
            uint8_t pv[1] = { AWD_HEAVY };
            k10LogPlate(PLATE_AWARD, cb, (int8_t)ev.pid, TONE_GOOD, pv, 1);
          }
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
                    (int8_t)ev.pid, TONE_PLAIN, GLY_DEPART);
        } else if (ev.encOut == ENC_END_DAWN) {
          k10LogAdd(K10_SAY("comes out of it as the sun does.",
                            "is still walking when the light finds them."),
                    (int8_t)ev.pid, TONE_PLAIN, GLY_DAWN);
        } else if (ev.encOut == ENC_END_DOWNED) {
          k10LogAdd(K10_SAY("does not come out standing.",
                            "falls in there, and the dark keeps it."),
                    (int8_t)ev.pid, TONE_ILL, GLY_DEATH);
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
        // Index matches WX[] above; both fogs get the same mark.
        static const uint8_t WX_GLYPH[6] = {
          GLY_DAWN, GLY_RAIN, GLY_STORM, GLY_CHEM, GLY_FOG, GLY_FOG,
        };
        k10LogAdd(WX_PROSE[(ev.q < 6) ? ev.q : 0], -1,
                  (ev.q == 0) ? TONE_GOOD : TONE_OMEN,
                  WX_GLYPH[(ev.q < 6) ? ev.q : 0]);
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
                  (int8_t)ev.pid, TONE_GOOD, GLY_CARAVAN);
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
                  (int8_t)ev.pid, TONE_ILL, GLY_FIRE);
        break;
      }

      case EVT_FIRE_SPREAD: {
        // Vision-culled: a fire's location is map information, and revealing
        // it to a player who hasn't explored that hex would leak outside
        // their fog of war — unlike EVT_FIRE_DAMAGE above (about a specific
        // player who is already there).
        LOG_VERBOSE("EVT fire_spread q=%d r=%d intensity=%d", (int)ev.q, (int)ev.r, (int)ev.amt);
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

      case EVT_FLOOD_WASHOUT: {
        // Vision-culled like EVT_FIRE_SPREAD above — a washed-out hex is map
        // information. amt/"intensity" here is the resulting terrain id, not
        // a flood intensity level: 3 (Marsh — dry ground pushed under by the
        // advancing edge) or 5 (Flooded District — a swamped hex that stayed
        // under long enough to fully drown, see spreadFlood()'s two-stage
        // progression). There's no "receded" sentinel the way fire has amt=0
        // for "just went out" — flood recession is a silent per-tick decay
        // with no broadcast-worthy moment.
        LOG_VERBOSE("EVT flood_washout q=%d r=%d terrain=%d", (int)ev.q, (int)ev.r, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"flood_washout\",\"q\":%d,\"r\":%d,\"intensity\":%d}",
          (int)ev.q, (int)ev.r, (int)ev.amt);
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (!conn[i]) continue;
          if (hexDistWrap(pq[i], pr[i], ev.q, ev.r) > visR[i]) continue;
          AsyncWebSocketClient* cl = ws.client(wsId[i]);
          if (cl) cl->text(buf, len);
        }
        break;
      }

      case EVT_FLOOD_DAMAGE: {
        Log.notice("EVT flood_dmg pid=%d q=%d r=%d llLost=%d",
                   (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.res);
        // `intensity` stays the 10 sentinel the client already keys on;
        // `llLost` is additive and carries what the sweep actually cost, so
        // cause-of-death attribution reads it instead of assuming a constant.
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"flood_dmg\",\"pid\":%d,\"q\":%d,\"r\":%d,"
          "\"intensity\":%d,\"llLost\":%d}",
          (int)ev.pid, (int)ev.q, (int)ev.r, (int)ev.amt, (int)ev.res);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("is swept off their feet by the flash flood.",
                          "loses their footing as the ground gives way.",
                          "goes under for a moment in the rising water."),
                  (int8_t)ev.pid, TONE_ILL, GLY_FLOOD);
        break;
      }

      case EVT_DOOM_WARNING: {
        // Broadcast to everyone regardless of position — Creeping Doom is a
        // world-level threat, not a local one (per the spec). The K10 LED
        // already fired inline in resolveDoomProximity() (world-system.hpp);
        // the audio is not a one-shot any more but the distance-driven
        // ostinato in tickDoomAudio(), so this is just the log + WS notice.
        Log.notice("EVT doom_warn pid=%d", (int)ev.pid);
        len = snprintf(buf, sizeof(buf), "{\"t\":\"ev\",\"k\":\"doom_warn\",\"pid\":%d}", (int)ev.pid);
        ws.textAll(buf, len);
        k10LogAdd(K10_SAY("feels the Doom turn its head.",
                          "goes quiet. Something out there noticed."),
                  (int8_t)ev.pid, TONE_OMEN, GLY_DOOM);
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
                  (int8_t)ev.pid, TONE_OMEN, GLY_DOOM);
        break;
      }

      case EVT_TUNNEL_TAUNT: {
        // res = line index the client reduces mod its own table length
        // (TUNNEL_TAUNTS, data/game-data.js) -- the 30 wordings never go on
        // the wire, so they can be reworded without reflashing.
        //
        // Unicast, which is the one place this differs from the Doom.
        // EVT_DOOM_TAUNT is broadcast because the party watching it single
        // somebody out IS the effect; this is a survivor's own second
        // thoughts about where they bedded down, and in a six-player game
        // with three of them underground, broadcasting would be three
        // streams of somebody else's doubt in everyone's log.
        Log.notice("EVT tun_taunt pid=%d wsId=%u", (int)ev.pid, (unsigned)ev.evWsId);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"tun_taunt\",\"pid\":%d,\"idx\":%d}",
          (int)ev.pid, (int)ev.res);
        if (AsyncWebSocketClient* cl = ws.client(ev.evWsId)) cl->text(buf, len);
        break;
      }

      case EVT_DOOM_TAUNT: {
        // amt = tier (0-3), res = line index the client reduces mod its own
        // table length (DOOM_TAUNTS, data/game-data.js) — the wording never
        // goes on the wire. Broadcast to everyone for the same reason
        // EVT_DOOM_WARNING is: the Doom is a world-level presence, and the
        // rest of the party hearing it single someone out is the point.
        Log.notice("EVT doom_taunt pid=%d tier=%d", (int)ev.pid, (int)ev.amt);
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"doom_taunt\",\"pid\":%d,\"tier\":%d,\"idx\":%d}",
          (int)ev.pid, (int)ev.amt, (int)ev.res);
        ws.textAll(buf, len);
        switch (ev.amt) {
          case 3: {
            k10LogAdd(K10_SAY("is being hunted. It has stopped tracking.",
                              "is all it wants now. No trail, just them."),
                      (int8_t)ev.pid, TONE_OMEN, GLY_DOOM);
            break;
          }
          case 2: {
            k10LogAdd(K10_SAY("works, and the Doom unmakes it behind them.",
                              "cannot keep anything it has decided to rot."),
                      (int8_t)ev.pid, TONE_OMEN, GLY_DOOM);
            break;
          }
          case 1: {
            k10LogAdd(K10_SAY("left too much of themselves on the ground.",
                              "has been noticed. The wind changed."),
                      (int8_t)ev.pid, TONE_OMEN, GLY_DOOM);
            break;
          }
          default: {
            k10LogAdd(K10_SAY("feels the weight behind them lift.",
                              "has slipped it, for now."),
                      (int8_t)ev.pid, TONE_GOOD, GLY_DOOM);
            {
              uint8_t pv[1] = { AWD_OFF_SCENT };
              k10LogPlate(PLATE_AWARD, "Doom lost the trail",
                          (int8_t)ev.pid, TONE_GOOD, pv, 1);
            }
            break;
          }
        }
        break;
      }

      default:
        Log.error("EVT UNKNOWN type=%d pid=%d", (int)ev.type, (int)ev.pid);
        break;
    }
  }
  if (snapCount > 0) LOG_VERBOSE("drainEvents: dispatched=%d", snapCount);
  // pendingCount was already reset to 0 inside the spinlock snapshot above.
}
