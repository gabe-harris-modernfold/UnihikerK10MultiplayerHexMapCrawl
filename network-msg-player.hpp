#pragma once
// ── Player message handlers: pick, move, name, wifi, check, regen, erase, act, settings ──

static void handleMsg_pick(AsyncWebSocketClient* client, char* data, size_t len) {
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
  struct { int16_t q, r; uint8_t terrain; int8_t visLvl; int visR, attempts, connCount;
           char name[12]; } snap = {};

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    Player& p = G.players[arch];
    if (!p.connected) {
      p.connected  = true;
      p.wsClientId = client->id();
      p.connectMs  = millis();

      bool isDowned    = (p.ll == 0);
      bool isReconnect = (p.score > 0 || p.steps > 0) && !isDowned;
      int attempts = 0;
      if (isDowned) {
        // Respawn: player died — preserve lifetime score/steps, reset health+inventory, new spawn
        int32_t  savedScore = p.score;
        uint32_t savedSteps = p.steps;
        do {
          p.q = (int16_t)(esp_random() % MAP_COLS);
          p.r = (int16_t)(esp_random() % MAP_ROWS);
          attempts++;
          uint8_t st = G.map[p.r][p.q].terrain;
          if (TERRAIN_MC[st] != 255 && !TERRAIN_IS_RAD[st]) break;
        } while (TERRAIN_MC[G.map[p.r][p.q].terrain] == 255 && attempts < 50);
        p.lastMoveMs = 0;
        memset(p.inv, 0, sizeof(p.inv));
        p.inv[0] = 2; p.inv[1] = 1; p.inv[2] = 3; p.inv[3] = 3; p.inv[4] = 3;
        if (arch == 1) { p.inv[1] = 2; }
        if (arch == 2) { p.inv[3] = 5; }
        if (arch == 3) { p.inv[1]=2; p.inv[3]=4; p.inv[4]=4; }
        snprintf(p.name, sizeof(p.name), "%s", ARCHETYPE_NAME[arch]);
        p.archetype    = (uint8_t)arch;
        p.ll           = 7;
        p.food         = 6;
        p.water        = 6;
        p.radiation    = 0;
        p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
        memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
        memset(p.invType,     0, sizeof(p.invType));
        memset(p.invQty,      0, sizeof(p.invQty));
        memset(p.equip,       0, sizeof(p.equip));
        memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
        p.fThreshBelow = 0; p.wThreshBelow = 0;
        p.movesLeft    = (int8_t)effectiveMP(arch);
        p.actUsed      = false;
        p.encPenApplied = false;
        p.radClean     = true;
        { int n = (arch == 3) ? 3 : (arch == 1) ? 2 : 1; for (int i=0;i<n;i++) grantRandomStartItem(p); }
        p.score = savedScore;
        p.steps = savedSteps;
      } else if (!isReconnect) {
        // New player: spawn on passable terrain and full init
        do {
          p.q = (int16_t)(esp_random() % MAP_COLS);
          p.r = (int16_t)(esp_random() % MAP_ROWS);
          attempts++;
          uint8_t st = G.map[p.r][p.q].terrain;
          if (TERRAIN_MC[st] != 255 && !TERRAIN_IS_RAD[st]) break;
        } while (TERRAIN_MC[G.map[p.r][p.q].terrain] == 255 && attempts < 50);
        p.lastMoveMs = 0;
        p.score = 0; p.steps = 0;
        memset(p.inv, 0, sizeof(p.inv));
        p.inv[0] = 2; p.inv[1] = 1; p.inv[2] = 3; p.inv[3] = 3; p.inv[4] = 3;
        if (arch == 1) { p.inv[1] = 2; }
        if (arch == 2) { p.inv[3] = 5; }
        if (arch == 3) { p.inv[1]=2; p.inv[3]=4; p.inv[4]=4; }
        snprintf(p.name, sizeof(p.name), "%s", ARCHETYPE_NAME[arch]);
        p.archetype    = (uint8_t)arch;
        p.ll           = 7;
        p.food         = 6;
        p.water        = 6;
        p.radiation    = 0;
        p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
        memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
        memset(p.invType,     0, sizeof(p.invType));
        memset(p.invQty,      0, sizeof(p.invQty));
        memset(p.equip,       0, sizeof(p.equip));
        memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
        p.fThreshBelow = 0; p.wThreshBelow = 0;
        p.movesLeft    = (int8_t)effectiveMP(arch);
        p.actUsed      = false;
        p.encPenApplied = false;
        p.radClean     = true;
        { int n = (arch == 3) ? 3 : (arch == 1) ? 2 : 1; for (int i=0;i<n;i++) grantRandomStartItem(p); }
      } else {
        // Reconnecting player: preserve score, position, inventory — just clear transient state
        p.actUsed      = false;
        p.encPenApplied = false;
      }
      p.resting = false;

      G.connectedCount++;
      G.map[p.r][p.q].footprints |= (1 << arch);

      { GameEvent ev = {}; ev.type = EVT_JOINED; ev.pid = (uint8_t)arch;
        ev.q = p.q; ev.r = p.r; enqEvt(ev); }

      snap.q       = p.q; snap.r = p.r;
      snap.terrain = G.map[p.r][p.q].terrain;
      snap.visLvl  = TERRAIN_VIS[snap.terrain];
      snap.visR    = (snap.visLvl <= -3) ? 0 : (snap.visLvl == -2) ? 1 : (snap.visLvl == -1) ? 2 :
                     (snap.visLvl == 0) ? VISION_R : (snap.visLvl == 1) ? VISION_R+1 : VISION_R+2;
      snap.attempts    = attempts;
      snap.connCount   = G.connectedCount;
      memcpy(snap.name, p.name, 12);
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
  const char* dp = strstr(data, "\"d\""); if (!dp) return;
  const char* dv = strchr(dp + 3, ':');  if (!dv) return;
  int dir = atoi(dv + 1);

  static char visBuf[1100];
  int visLen = 0, visCells = 0;
  int vr = VISION_R; bool mr = false;
  int slot = -1;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      if (encounters[slot].active) {
        xSemaphoreGive(G.mutex);
        client->text("{\"t\":\"err\",\"msg\":\"Cannot move during encounter\"}");
        return;
      }
      movePlayer(slot, dir);
      playerVisParams(slot, &vr, &mr);
      visLen = buildVisDisk(visBuf, sizeof(visBuf),
                            G.players[slot].q, G.players[slot].r, vr, mr, &visCells);
    }
    xSemaphoreGive(G.mutex);
  }
  if (visLen > 0) {
    client->text(visBuf, (size_t)visLen);
  }
}

static void handleMsg_name(AsyncWebSocketClient* client, char* data, size_t len) {
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

static void handleMsg_check(AsyncWebSocketClient* client, char* data, size_t len) {
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
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    SD.remove(SAVE_MAP_F);
    SD.remove(SAVE_PLY_F);
    generateMap();
    G.dayCount = 1; G.dayTick = 0; G.threatClock = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      Player& pl = G.players[i];
      if (!pl.connected) continue;
      for (int tries = 0; tries < 200; tries++) {
        int nq = esp_random() % MAP_COLS;
        int nr = esp_random() % MAP_ROWS;
        if (G.map[nr][nq].terrain == 0) { pl.q = (int16_t)nq; pl.r = (int16_t)nr; break; }
      }
      pl.ll = 7; pl.food = 4; pl.water = 4;
      pl.radiation = 0;
      pl.actUsed = false; pl.resting = false;
      pl.movesLeft = (int8_t)effectiveMP(i);
    }
    xSemaphoreGive(G.mutex);
  }
  { GameEvent ev = {}; ev.type = EVT_REGEN; enqEvt(ev); }
}

static void handleMsg_eraseslot(AsyncWebSocketClient* client, char* data, size_t len) {
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
    // Wipe all persistent fields; setting name[0]='\0' makes saveGame() write sp.used=0
    memset(p.name,        0, sizeof(p.name));
    memset(p.inv,         0, sizeof(p.inv));
    memset(p.invType,     0, sizeof(p.invType));
    memset(p.invQty,      0, sizeof(p.invQty));
    memset(p.equip,       0, sizeof(p.equip));
    memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
    memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
    p.archetype    = (uint8_t)arch;
    p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
    p.ll           = 0;
    p.food         = 0; p.water     = 0;
    p.radiation = 0;
    p.score        = 0; p.steps     = 0;
    p.movesLeft    = 0;
    p.actUsed      = false;
    p.encPenApplied = false;
    p.radClean     = true;
    p.fThreshBelow = 0; p.wThreshBelow = 0;
    p.lastMoveMs   = 0;
    xSemaphoreGive(G.mutex);
  }

  {
    char evBuf[64]; int evLen;
    if (wasConn && evictId) {
      evLen = snprintf(evBuf, sizeof(evBuf),
        "{\"t\":\"ev\",\"k\":\"downed\",\"pid\":%d}", arch);
      for (AsyncWebSocketClient& cl : ws.getClients()) {
        if (cl.id() == evictId) { cl.text(evBuf, evLen); break; }
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
  // Note: "cnd" field (condTgt) removed — Treat action no longer exists
  const char* ap = strstr(data, "\"a\""); if (!ap) return;
  const char* av = strchr(ap + 3, ':');   if (!av) return;
  int actType = atoi(av + 1);
  if (actType < 0 || actType > 7) return;

  int mpParam = 1;
  const char* mpp = strstr(data, "\"mp\"");
  if (mpp) { const char* mpv = strchr(mpp + 4, ':'); if (mpv) mpParam = atoi(mpv + 1); }

  static char survBuf[1100];
  int  survLen = 0;
  int  slot    = -1;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      if (encounters[slot].active) {
        xSemaphoreGive(G.mutex);
        client->text("{\"t\":\"err\",\"msg\":\"Cannot act during encounter\"}");
        return;
      }
      handleAction(slot, (uint8_t)actType, mpParam, 0,
                   survBuf, sizeof(survBuf), &survLen);
    }
    xSemaphoreGive(G.mutex);
  }
  if (survLen > 0)
    client->text(survBuf, (size_t)survLen);
}

static void handleMsg_settings(AsyncWebSocketClient* client, char* data, size_t len) {
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
