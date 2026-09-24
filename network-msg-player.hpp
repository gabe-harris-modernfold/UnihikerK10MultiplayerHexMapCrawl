#pragma once
// ── Player message handlers: pick, move, name, wifi, check, regen, erase, act, settings ──

static void handleMsg_pick(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* ap = strstr(data, "\"arch\""); if (!ap) return;
  const char* av = strchr(ap + 6, ':');      if (!av) return;
  int arch = atoi(av + 1);
  if (arch < 0 || arch >= NUM_ARCHETYPES) return;

  bool inLobby = false;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (lobbyIds[i] == client->id()) { inLobby = true; break; }
  }
  taskEXIT_CRITICAL(&evtMux);
  if (!inLobby) {
    sendLobbyMsg(client);
    return;
  }

  bool assigned = false;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    Player& p = G.players[arch];
    if (!p.connected) {
      p.connected  = true;
      p.wsClientId = client->id();
      p.connectMs  = millis();
      // Seed the liveness clock now: a slot with lastWsAliveMs == 0 is treated
      // as never-seen and so is immediately reapable, which would leave a
      // freshly-picked player one unlucky ws.client() look from being unseated
      // before gameLoop's first refreshWsLiveness() ever ran.
      lastWsAliveMs[arch] = p.connectMs;

      // Three ways into a slot:
      //   downed    — LL 0: a fresh survivor, but lifetime score/steps carry over
      //   reconnect — a live survivor (moved or scored) picks up where they left off
      //   new       — an untouched slot: full init, score reset
      bool isDowned    = (p.ll == 0);
      bool isReconnect = (p.score > 0 || p.steps > 0) && !isDowned;
      if (!isReconnect) {
        uint16_t savedScore = isDowned ? p.score : 0;
        uint16_t savedSteps = isDowned ? p.steps : 0;
        resetSurvivor(p, (uint8_t)arch);
        pickSpawnNearPlayer(p, (uint8_t)arch);
        snprintf(p.name, sizeof(p.name), "%s", ARCHETYPE_NAME[arch]);
        int n = (arch == 3) ? 3 : (arch == 1) ? 2 : 1;
        for (int i = 0; i < n; i++) grantRandomStartItem(p);
        p.score = savedScore;
        p.steps = savedSteps;
      }
      // Reconnecting players keep score, position, inventory and wounds;
      // only the resting flag is transient.
      p.resting = false;

      G.connectedCount++;
      G.map[p.r][p.q].footprints |= (1 << arch);

      { GameEvent ev = {}; ev.type = EVT_JOINED; ev.pid = (uint8_t)arch;
        ev.q = p.q; ev.r = p.r; enqEvt(ev); }
      Log.notice("PICK arch=%d name=%s q=%d r=%d %s connected=%d",
                 arch, p.name, (int)p.q, (int)p.r,
                 isDowned ? "respawn" : (isReconnect ? "reconnect" : "new"),
                 (int)G.connectedCount);
      assigned = true;
    }
    xSemaphoreGive(G.mutex);
  }

  if (assigned) {
    taskENTER_CRITICAL(&evtMux);
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (lobbyIds[i] == client->id()) { lobbyIds[i] = 0; break; }
    taskEXIT_CRITICAL(&evtMux);


    char buf[48];
    snprintf(buf, sizeof(buf), "{\"t\":\"asgn\",\"id\":%d}", arch);
    client->text(buf);
    sendSync(client, arch);
    broadcastLobbyUpdate();
  } else {
    sendLobbyMsg(client);
  }
}

static void handleMsg_move(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* dp = strstr(data, "\"d\""); if (!dp) return;
  const char* dv = strchr(dp + 3, ':');  if (!dv) return;
  int dir = atoi(dv + 1);

  PSRAM_STATIC(char, visBuf, [1100]);
  int visLen = 0, visCells = 0;
  int vr = VISION_R; bool mr = false;
  int slot = -1;
  uint8_t depBefore = 0, depAfter = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      if (encounters[slot].active) {
        xSemaphoreGive(G.mutex);
        client->text("{\"t\":\"err\",\"msg\":\"Cannot move during encounter\"}");
        return;
      }
      depBefore = G.players[slot].depth;
      movePlayer(slot, dir);          // may cross boards -- see tunnelStepDown/Up
      depAfter  = G.players[slot].depth;
      playerVisParams(slot, &vr, &mr);
      // Underground the disk must be built from tq/tr against G.tunnel. p.q/p.r
      // stay pinned to the hatch the player descended through -- the invariant
      // at the top of tunnels.hpp -- so building from them here shipped a
      // *surface* disk around that hatch and revealed nothing below. The tunnel
      // board therefore stayed fogged past the one ring tsync sent on descend,
      // and every step after that landed on a cell the client had never seen,
      // which applyHexFill() paints flat black: a move onto "no hex".
      visLen = buildVisDisk(visBuf, sizeof(visBuf),
                            depAfter ? G.players[slot].tq : G.players[slot].q,
                            depAfter ? G.players[slot].tr : G.players[slot].r,
                            vr, mr, &visCells, depAfter);
    }
    xSemaphoreGive(G.mutex);
  }
  // That step went through a hatch and we are now below. The client has never
  // seen the tunnel board, so hand over the whole fogged thing before the vis
  // disk that indexes into it -- sendTunnelSync() takes G.mutex itself, hence
  // out here. Surfacing needs no equivalent: the client already has G.map.
  if (slot >= 0 && depAfter == 1 && depBefore == 0) sendTunnelSync(client);
  if (visLen > 0) {
    client->text(visBuf, (size_t)visLen);
  }
  if (slot >= 0 && depAfter != depBefore) k10Play(MOTIF_SEWER_ECHO);
}

static void handleMsg_name(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* np = strstr(data, "\"name\""); if (!np) return;
  const char* nv = strchr(np + 6, '"');      if (!nv) return; nv++;
  const char* ne = strchr(nv, '"');          if (!ne) return;

  char oldName[12] = {0};
  char newName[12] = {0};
  int  slot = -1;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      memcpy(oldName, G.players[slot].name, 12);
      int nl = (int)(ne - nv); if (nl > 11) nl = 11;
      strncpy(G.players[slot].name, nv, nl);
      G.players[slot].name[nl] = 0;
      sanitizeName(G.players[slot].name, 11);
      memcpy(newName, G.players[slot].name, 12);
    }
    xSemaphoreGive(G.mutex);
  }
  if (slot >= 0) {
    char nameBuf[56];
    int  nameLen = snprintf(nameBuf, sizeof(nameBuf),
      "{\"t\":\"ev\",\"k\":\"nm\",\"pid\":%d,\"nm\":\"%s\"}", slot, newName);
    ws.textAll(nameBuf, (size_t)nameLen);
  }
}

static void handleMsg_wifi(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* sp = strstr(data, "\"ssid\""); if (!sp) return;
  const char* sv = strchr(sp + 6, '"');      if (!sv) return; sv++;
  const char* se = strchr(sv, '"');          if (!se) return;

  const char* pp = strstr(data, "\"pass\"");
  const char* pv = pp ? strchr(pp + 6, '"') : nullptr;
  if (pv) pv++;
  const char* pe = pv ? strchr(pv, '"') : nullptr;

  if (wifiConnecting || bootWifiPending) {
    const char* busy = "{\"t\":\"wifi\",\"status\":\"busy\"}";
    client->text(busy, strlen(busy));
    return;
  }

  WifiTaskCtx* ctx = (WifiTaskCtx*)malloc(sizeof(WifiTaskCtx));
  if (!ctx) return;

  int sl = (int)(se - sv); if (sl > 32) sl = 32;
  strncpy(ctx->ssid, sv, sl); ctx->ssid[sl] = 0;

  if (pv && pe) {
    int pl = (int)(pe - pv); if (pl > 64) pl = 64;
    strncpy(ctx->pass, pv, pl); ctx->pass[pl] = 0;
  } else {
    ctx->pass[0] = 0;
  }

  wifiConnecting = true;
  xTaskCreatePinnedToCore(wifiConnectTask, "wifiConn", 4096, ctx, 1, NULL, 0);
}

// {"t":"wifi_forget","ssid":"..."} — drop a network from the roaming list.
// An active connection to that network is left alone; forgetting is about
// which networks the board goes looking for next time.
static void handleMsg_wifi_forget(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* sp = strstr(data, "\"ssid\""); if (!sp) return;
  const char* sv = strchr(sp + 6, '"');      if (!sv) return; sv++;
  const char* se = strchr(sv, '"');          if (!se) return;

  char ssid[33];
  int sl = (int)(se - sv); if (sl > 32) sl = 32;
  strncpy(ssid, sv, sl); ssid[sl] = 0;

  if (!wifiStoreForget(ssid)) {
    LOG_VERBOSE("wifi_forget: ssid=%s not in store", ssid);
    return;
  }
  // Tell clients to drop their cached copy too, otherwise the next reconnect
  // would auto-send those credentials and re-add the network we just dropped.
  char fb[96];
  int fl = snprintf(fb, sizeof(fb), "{\"t\":\"wifi\",\"status\":\"forgot\",\"ssid\":\"%s\"}", ssid);
  ws.textAll(fb, (size_t)fl);
  broadcastWifiNets();
}

static void handleMsg_check(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* skp = strstr(data, "\"sk\""); if (!skp) return;
  const char* skv = strchr(skp + 4, ':');   if (!skv) return;
  int sk = atoi(skv + 1);
  if (sk < 0 || sk >= NUM_SKILLS) return;

  const char* dnp = strstr(data, "\"dn\""); if (!dnp) return;
  const char* dnv = strchr(dnp + 4, ':');   if (!dnv) return;
  int dn = atoi(dnv + 1);
  if (dn < 2) dn = 2; if (dn > 14) dn = 14;

  int bonus = 0;
  const char* bonp = strstr(data, "\"bon\"");
  if (bonp) {
    const char* bonv = strchr(bonp + 5, ':');
    if (bonv) { bonus = atoi(bonv + 1); if (bonus < 0) bonus = 0; if (bonus > 2) bonus = 2; }
  }

  CheckResult res = {};
  int slot = -1;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      res = resolveCheck(slot, (uint8_t)sk, (uint8_t)dn, (uint8_t)bonus);
    }
    xSemaphoreGive(G.mutex);
  }
  if (slot >= 0) {
    broadcastCheck(slot, (uint8_t)sk, res);
  }
}

static void handleMsg_regen(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    Log.notice("Regen: removing %s and %s", SAVE_MAP_F, SAVE_PLY_F);
    SD.remove(SAVE_MAP_F);
    SD.remove(SAVE_PLY_F);
    // A new world: nothing from the old one may leak through.
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (encounters[i].active) endEncounter(i, ENC_END_REGEN, /*restorePoi=*/false);
    memset(tradeOffers, 0, sizeof(tradeOffers));
    memset(groundItems, 0, sizeof(groundItems));
    generateMap();
    wInit();  // re-place world entities — stale coords may now be impassable (e.g. a new Nuke Crater)
    G.dayCount = 1; G.dayTick = 0; G.threatClock = 0;
    resetWeather();
    for (int i = 0; i < MAX_PLAYERS; i++) {
      Player& pl = G.players[i];
      if (!pl.connected) continue;
      // Connected survivors start the new world fresh on Open Scrub, keeping
      // only name, score, and steps.
      resetSurvivor(pl, pl.archetype);
      for (int tries = 0; tries < 200; tries++) {
        int nq = esp_random() % MAP_COLS;
        int nr = esp_random() % MAP_ROWS;
        if (G.map[nr][nq].terrain == 0) { pl.q = (int16_t)nq; pl.r = (int16_t)nr; break; }
      }
      G.map[pl.r][pl.q].footprints |= (1 << i);
    }
    xSemaphoreGive(G.mutex);
  }
  { GameEvent ev = {}; ev.type = EVT_REGEN; enqEvt(ev); }
}

static void handleMsg_eraseslot(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* ap = strstr(data, "\"arch\""); if (!ap) return;
  const char* av = strchr(ap + 6, ':');      if (!av) return;
  int arch = atoi(av + 1);
  if (arch < 0 || arch >= NUM_ARCHETYPES) return;

  uint32_t evictId  = 0;
  bool     wasConn  = false;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    Player& p = G.players[arch];
    if (p.connected) {
      evictId  = p.wsClientId;
      wasConn  = true;
      p.connected  = false;
      p.wsClientId = 0;
      p.resting    = false;
      G.connectedCount--;
    }
    if (encounters[arch].active) endEncounter(arch, ENC_END_DISCONNECT, /*restorePoi=*/true);
    // Wipe all persistent fields; setting name[0]='\0' makes saveGame() write
    // sp.used=0, and LL 0 makes the next pick take the fresh-survivor path.
    resetSurvivor(p, (uint8_t)arch);
    memset(p.name, 0, sizeof(p.name));
    memset(p.inv,  0, sizeof(p.inv));
    p.ll = 0; p.food = 0; p.water = 0;
    p.score = 0; p.steps = 0; p.encCount = 0;
    p.movesLeft = 0;
    xSemaphoreGive(G.mutex);
  }

  {
    char evBuf[64]; int evLen;
    if (wasConn && evictId) {
      evLen = snprintf(evBuf, sizeof(evBuf),
        "{\"t\":\"ev\",\"k\":\"downed\",\"pid\":%d}", arch);
      if (AsyncWebSocketClient* cl = ws.client(evictId)) {
        cl->text(evBuf, evLen);
      }
      taskENTER_CRITICAL(&evtMux);
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!lobbyIds[i]) { lobbyIds[i] = evictId; break; }
      }
      taskEXIT_CRITICAL(&evtMux);
    }
    evLen = snprintf(evBuf, sizeof(evBuf),
      "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", arch);
    ws.textAll(evBuf, evLen);
  }

  broadcastLobbyUpdate();
  saveGame();
}

static void handleMsg_act(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* ap = strstr(data, "\"a\""); if (!ap) return;
  const char* av = strchr(ap + 3, ':');   if (!av) return;
  int actType = atoi(av + 1);
  if (actType < 0 || actType > 7) return;

  int mpParam = 1;
  const char* mpp = strstr(data, "\"mp\"");
  if (mpp) { const char* mpv = strchr(mpp + 4, ':'); if (mpv) mpParam = atoi(mpv + 1); }

  int recipeId = 0;  // ACT_CRAFT only — which known recipe to craft
  const char* rp = strstr(data, "\"r\"");
  if (rp) { const char* rv = strchr(rp + 3, ':'); if (rv) recipeId = atoi(rv + 1); }
  if (recipeId < 0 || recipeId > 255) recipeId = 0;

  PSRAM_STATIC(char, survBuf, [1100]);
  int  survLen = 0;
  int  slot    = -1;
  bool actOk   = false;
  SettleResult settleResult = {};
  // CRAFT mutates invType[]/invQty[]/knownRecipes, which — like use_item/
  // equip_item — are private state never carried by the broadcastState()
  // tick or the 'ev'/'act' broadcast below, so a successful craft needs its
  // own targeted snapshot back to the crafting client (and a saveGame(),
  // same as use_item/equip_item/drop_item do for the same kind of mutation).
  PSRAM_STATIC(char, craftAck, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  craftAck[0] = '\0';
  const char* craftWhy = nullptr;  // why ACT_CRAFT was refused (static string), toasted back below

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      if (encounters[slot].active) {
        xSemaphoreGive(G.mutex);
        client->text("{\"t\":\"err\",\"msg\":\"Cannot act during encounter\"}");
        return;
      }
      actOk = handleAction(slot, (uint8_t)actType, mpParam, (uint8_t)recipeId,
                            survBuf, sizeof(survBuf), &survLen, settleResult, &craftWhy);
      if (actType == ACT_CRAFT && actOk) {
        Player& pl = G.players[slot];
        int ap = appendFmt(craftAck, sizeof(craftAck), 0,
          "{\"t\":\"item_result\",\"ok\":true,\"act\":\"craft\",\"pid\":%d,\"recipe\":%d,",
          slot, recipeId);
        ap = appendPackArrays(craftAck, sizeof(craftAck), ap, slot);
        appendFmt(craftAck, sizeof(craftAck), ap,
          ",\"inv\":[%d,%d,%d,%d,%d],\"kr\":%lu}",
          pl.inv[0], pl.inv[1], pl.inv[2], pl.inv[3], pl.inv[4],
          (unsigned long)pl.knownRecipes);
      }
    }
    xSemaphoreGive(G.mutex);
  }
  // saveGame outside the mutex (it acquires it itself) — same pattern as
  // handleMsg_use_item, and for the same reason: a craft just changed
  // invType[]/invQty[]/knownRecipes.
  if (actType == ACT_CRAFT && actOk) saveGame();
  if (survLen > 0)
    client->text(survBuf, (size_t)survLen);
  if (craftAck[0])
    client->text(craftAck);
  // A refused craft still broadcasts its AO_BLOCKED 'act' event like every
  // other action, but that only says "blocked" — tell the crafting client
  // why (same strings the mock sends) so the tap doesn't just vanish.
  if (actType == ACT_CRAFT && !actOk && craftWhy) {
    static char errBuf[96];
    snprintf(errBuf, sizeof(errBuf), "{\"t\":\"err\",\"msg\":\"%s\"}", craftWhy);
    client->text(errBuf);
  }
  if (settleResult.fired)
    broadcastSettle(settleResult);
}

static void handleMsg_settings(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* avp = strstr(data, "\"audioVol\"");
  if (avp) { const char* avv = strchr(avp + 10, ':'); if (avv) {
    int v = atoi(avv + 1);
    if (v >= 0 && v <= 9) s_audioVol = (uint8_t)v;
  }}
  const char* lbp = strstr(data, "\"ledBright\"");
  if (lbp) { const char* lbv = strchr(lbp + 11, ':'); if (lbv) {
    int b = atoi(lbv + 1);
    if (b >= 0 && b <= 9) s_ledBright = (uint8_t)b;
  }}
  const char* sfp = strstr(data, "\"screenFlip\"");
  if (sfp) { const char* sfv = strchr(sfp + 12, ':'); if (sfv) {
    while (*sfv == ':' || *sfv == ' ') sfv++;
    bool flip = (strncmp(sfv, "true", 4) == 0);
    if (flip != s_screenFlip) {
      s_screenFlip = flip;
      tft.setRotation(s_screenFlip ? 0 : 2);
    }
  }}
  saveK10Prefs();
}
