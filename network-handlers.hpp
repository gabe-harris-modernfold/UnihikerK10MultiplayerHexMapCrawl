#pragma once
// ── WebSocket handlers: session lifecycle and message dispatch ────────────────
// Included from Esp32HexMapCrawl.ino after network-sync.hpp + network-events.hpp.
// Has access to all globals, constants, structs, and all prior functions.

// ── WebSocket connect handler ─────────────────────────────────────────────────
// Adds the client to the lobby and sends available archetypes for selection.
// Does NOT assign a slot — that happens when the client sends a "pick" message.
static void handleConnect(AsyncWebSocketClient* client) {
  // Cleanup stale connections: p.connected=true but WS client is gone (ungraceful close)
  {
    bool freedAny = false;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& p = G.players[i];
        if (p.connected && !ws.client(p.wsClientId)) {
          Serial.printf("[CONNECT] Freeing stale slot %d (wsId %lu gone)\n",
                         i, (unsigned long)p.wsClientId);
          p.connected  = false;
          p.wsClientId = 0;
          if (G.connectedCount > 0) G.connectedCount--;
          freedAny = true;
        }
      }
      xSemaphoreGive(G.mutex);
    }
    if (freedAny) broadcastLobbyUpdate();
  }

  // Count how many slots are occupied (connected) and how many are pending in lobby
  int connectedCount = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    connectedCount = G.connectedCount;
    xSemaphoreGive(G.mutex);
  }
  int lobbySize = 0;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) if (lobbyIds[i]) lobbySize++;
  taskEXIT_CRITICAL(&evtMux);

  // Reject if no slots remain for this client
  if (connectedCount + lobbySize >= MAX_PLAYERS) {
    Serial.printf("[LOBBY]   FULL — client:#%lu rejected (%d connected, %d in lobby)\n",
      (unsigned long)client->id(), connectedCount, lobbySize);
    client->text("{\"t\":\"full\"}");
    return;
  }

  // Add to lobby
  bool added = false;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!lobbyIds[i]) { lobbyIds[i] = client->id(); added = true; break; }
  }
  taskEXIT_CRITICAL(&evtMux);

  if (!added) { client->text("{\"t\":\"full\"}"); return; }

  Serial.printf("[LOBBY]   Client #%lu in character selection (%d connected, %d→%d in lobby)\n",
    (unsigned long)client->id(), connectedCount, lobbySize, lobbySize + 1);
  // If we have saved WiFi credentials, echo them to this client so its
  // localStorage (and the Settings inputs) stay in sync across devices/reboots.
  if (savedSsid[0]) {
    char credBuf[160];
    int credLen = snprintf(credBuf, sizeof(credBuf),
      "{\"t\":\"wifi\",\"status\":\"saved\",\"ssid\":\"%s\",\"pass\":\"%s\"}",
      savedSsid, savedPass);
    client->text(credBuf, (size_t)credLen);
  }
  sendLobbyMsg(client);
}

// ── WebSocket disconnect handler ──────────────────────────────────────────────
static void handleDisconnect(AsyncWebSocketClient* client) {
  // Check if this client was in the lobby (not yet assigned a slot)
  bool wasInLobby = false;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (lobbyIds[i] == client->id()) {
      lobbyIds[i] = 0;
      wasInLobby = true;
      break;
    }
  }
  taskEXIT_CRITICAL(&evtMux);
  if (wasInLobby) {
    Serial.printf("[LOBBY]   Client #%lu left lobby\n", (unsigned long)client->id());
    return;
  }

  // Snapshot before clearing slot
  int      slot    = -1;
  char     name[12] = {0};
  uint16_t steps   = 0, score = 0;
  uint8_t  ll = 0, food = 0, water = 0, fatigue = 0, rad = 0, sb = 0;
  uint32_t connMs  = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      Player& p = G.players[slot];
      memcpy(name, p.name, 12);
      steps   = p.steps;   score   = p.score; connMs = p.connectMs;
      ll      = p.ll;      food    = p.food;  water  = p.water;
      fatigue = p.fatigue; rad     = p.radiation; sb = p.statusBits;
      p.connected  = false;
      p.wsClientId = 0;
      p.resting    = false;  // clear stale resting flag on disconnect
      G.connectedCount--;
      // Clear active encounter on disconnect (POI already consumed at enc_start)
      if (encounters[slot].active) {
        uint8_t hq = encounters[slot].hexQ, hr = encounters[slot].hexR;
        encounters[slot] = {};
        GameEvent eev = {}; eev.type = EVT_ENC_END; eev.pid = (uint8_t)slot;
        eev.q = (int16_t)hq; eev.r = (int16_t)hr; eev.encOut = 4;  // reason: disconnect
        enqEvt(eev);
      }
      { GameEvent ev = {}; ev.type = EVT_LEFT; ev.pid = (uint8_t)slot; enqEvt(ev); }
    }
    xSemaphoreGive(G.mutex);
  }
  if (slot < 0) return;

  uint32_t sessSec = (millis() - connMs) / 1000;
  Serial.printf("[DISCONN] Slot:%d \"%s\" | steps:%d score:%d | LL:%d F:%d W:%d T:%d R:%d sb:0x%02X | session:%lum%02lus | players now:%d/%d\n",
    slot, name, steps, score, ll, food, water, fatigue, rad, sb,
    (unsigned long)(sessSec / 60), (unsigned long)(sessSec % 60),
    G.connectedCount, MAX_PLAYERS);

  // Notify lobby clients that this archetype slot is now free
  broadcastLobbyUpdate();
  // Persist game state so progress survives reconnects
  saveGame();
}

// ── WiFi STA join task (Core 0) ───────────────────────────────────────────────
// Runs on Core 0; attempts STA join while keeping AP alive; broadcasts result.
static void wifiConnectTask(void* param) {
  WifiTaskCtx* ctx = (WifiTaskCtx*)param;

  Serial.printf("[WIFI]    Task start — ssid:\"%s\" pass_len:%d current_mode:%d\n",
    ctx->ssid, (int)strlen(ctx->pass), (int)WiFi.getMode());

  // Only switch mode if not already in AP+STA — re-calling WiFi.mode() when
  // already in WIFI_AP_STA can reset the WiFi stack and drop the softAP.
  if (WiFi.getMode() != WIFI_MODE_APSTA) {
    Serial.println("[WIFI]    Switching to WIFI_AP_STA");
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(100));  // let mode switch settle
  }

  Serial.printf("[WIFI]    Calling WiFi.begin(\"%s\")...\n", ctx->ssid);
  WiFi.begin(ctx->ssid, ctx->pass[0] ? ctx->pass : nullptr);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.printf("[WIFI]    status=%d  elapsed=%lums\n",
      (int)WiFi.status(), (unsigned long)(millis() - t0));
  }

  char buf[88]; int blen;
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    blen = snprintf(buf, sizeof(buf),
      "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}", ip.c_str());
    Serial.printf("[WIFI]    Connected! STA IP: %s  AP IP: %s\n",
      ip.c_str(), WiFi.softAPIP().toString().c_str());
    // Cache in globals so handleConnect can echo creds to new clients.
    // ESP32 already saved these to its own internal NVS when WiFi.begin(ssid,pass)
    // was called above — no separate Preferences write needed.
    strlcpy(savedSsid, ctx->ssid, sizeof(savedSsid));
    strlcpy(savedPass, ctx->pass, sizeof(savedPass));
  } else {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);   // fall back to AP-only
    WiFi.softAP(AP_SSID); // re-assert AP (mode switch may reset it)
    // Clear in-memory SSID so handleConnect won't send a 'saved' message that
    // would suppress the client's auto-send retry (NVS copy is kept for next boot).
    savedSsid[0] = '\0';
    blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"fail\"}");
    Serial.printf("[WIFI]    Failed (timeout) — back to AP mode  IP: %s\n",
      WiFi.softAPIP().toString().c_str());
  }

  ws.textAll(buf, (size_t)blen);
  free(ctx);
  wifiConnecting = false;
  vTaskDelete(NULL);
}

// ── WebSocket message handler ─────────────────────────────────────────────────
static void handleMessage(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* tp = strstr(data, "\"t\""); if (!tp) return;
  const char* tv = strchr(tp + 3, '"');   if (!tv) return; tv++;
  const char* te = strchr(tv, '"');       if (!te) return;

  if (strncmp(tv, "pick", (size_t)(te - tv)) == 0) {
    // Archetype pick: {"t":"pick","arch":N}
    const char* ap = strstr(data, "\"arch\""); if (!ap) return;
    const char* av = strchr(ap + 6, ':');      if (!av) return;
    int arch = atoi(av + 1);
    if (arch < 0 || arch >= NUM_ARCHETYPES) return;

    // Verify client is in the lobby
    bool inLobby = false;
    taskENTER_CRITICAL(&evtMux);
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (lobbyIds[i] == client->id()) { inLobby = true; break; }
    }
    taskEXIT_CRITICAL(&evtMux);
    if (!inLobby) {
      sendLobbyMsg(client);  // clear client's pending state; never silently drop a pick
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

        // Reconnect path: player has prior progress — restore without wiping score/position
        // isDowned also catches ll==0 with no ST_DOWNED (old saves written before the persistence
        // fix; or any edge-case where ll reached 0 before ST_DOWNED was flushed to the save).
        // A live survivor never legitimately has ll==0, so this is always a respawn trigger.
        bool isDowned    = !!(p.statusBits & ST_DOWNED) || (p.ll == 0);
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
          // Starting resources (all archetypes)
          p.inv[0] = 2;  // water tokens
          p.inv[1] = 1;  // food tokens
          p.inv[2] = 3;  // fuel
          p.inv[3] = 3;  // medicine
          p.inv[4] = 3;  // scrap
          if (arch == 1) { p.inv[1] = 2; }           // Quartermaster: extra food
          if (arch == 2) { p.inv[3] = 5; }           // Medic: extra medicine (base 3 +2)
          if (arch == 3) { p.inv[1]=2; p.inv[3]=4; p.inv[4]=4; }  // Mule
          snprintf(p.name, sizeof(p.name), "%s", ARCHETYPE_NAME[arch]);
          p.archetype    = (uint8_t)arch;
          p.ll           = 7;
          p.food         = 6;
          p.water        = 6;
          p.fatigue      = 0;
          p.radiation    = 0;
          p.resolve      = 3;
          p.statusBits   = 0;  // clears ST_DOWNED
          p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
          memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
          memset(p.wounds,      0, sizeof(p.wounds));
          memset(p.invType,     0, sizeof(p.invType));
          memset(p.invQty,      0, sizeof(p.invQty));
          memset(p.equip,       0, sizeof(p.equip));
          memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
          p.fThreshBelow = 0; p.wThreshBelow = 0;
          p.movesLeft    = (int8_t)effectiveMP(arch);
          p.actUsed      = false;
          p.encPenApplied = false;
          p.radClean     = true;
          // Grant starting item(s) — Mule:3, Quartermaster:2, others:1
          { int n = (arch == 3) ? 3 : (arch == 1) ? 2 : 1; for (int i=0;i<n;i++) grantRandomStartItem(p); }
          p.score = savedScore;  // restore lifetime score
          p.steps = savedSteps;
          Serial.printf("[RESPAWN] Slot:%d (%s) score:%d — rising from the ash\n",
            arch, ARCHETYPE_NAME[arch], (int)p.score);
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
          // Starting resources (all archetypes)
          p.inv[0] = 2;  // water tokens
          p.inv[1] = 1;  // food tokens
          p.inv[2] = 3;  // fuel
          p.inv[3] = 3;  // medicine
          p.inv[4] = 3;  // scrap
          if (arch == 1) { p.inv[1] = 2; }           // Quartermaster: extra food
          if (arch == 2) { p.inv[3] = 5; }           // Medic: extra medicine (base 3 +2)
          if (arch == 3) { p.inv[1]=2; p.inv[3]=4; p.inv[4]=4; }  // Mule
          snprintf(p.name, sizeof(p.name), "%s", ARCHETYPE_NAME[arch]);
          // Survivor initialisation
          p.archetype    = (uint8_t)arch;
          p.ll           = 7;
          p.food         = 6;
          p.water        = 6;
          p.fatigue      = 0;
          p.radiation    = 0;
          p.resolve      = 3;
          p.statusBits   = 0;
          p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
          memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
          memset(p.wounds,      0, sizeof(p.wounds));
          memset(p.invType,     0, sizeof(p.invType));
          memset(p.invQty,      0, sizeof(p.invQty));
          memset(p.equip,       0, sizeof(p.equip));
          memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
          p.fThreshBelow = 0; p.wThreshBelow = 0;
          p.movesLeft    = (int8_t)effectiveMP(arch);
          p.actUsed      = false;
          p.encPenApplied = false;
          p.radClean     = true;
          // Grant starting item(s) — Mule:3, Quartermaster:2, others:1
          { int n = (arch == 3) ? 3 : (arch == 1) ? 2 : 1; for (int i=0;i<n;i++) grantRandomStartItem(p); }
        } else {
          // Reconnecting player: preserve score, position, inventory — just clear transient state
          Serial.printf("[RECONN]  Slot:%d (%s) score:%d steps:%d pos:(%d,%d) — restored\n",
            arch, ARCHETYPE_NAME[arch], (int)p.score, (int)p.steps, (int)p.q, (int)p.r);
          p.actUsed      = false;
          p.encPenApplied = false;
        }
        p.resting = false;  // always clear stale resting flag on (re)connect

        G.connectedCount++;

        // Mark starting hex with player footprint
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
      // Remove from lobby
      taskENTER_CRITICAL(&evtMux);
      for (int i = 0; i < MAX_PLAYERS; i++)
        if (lobbyIds[i] == client->id()) { lobbyIds[i] = 0; break; }
      taskEXIT_CRITICAL(&evtMux);

      Serial.printf("[CONNECT] Slot:%d (%s) client:#%lu | spawn:(%2d,%2d) %s [%s] visR:%d attempts:%d\n",
        arch, ARCHETYPE_NAME[arch], (unsigned long)client->id(),
        snap.q, snap.r, T_NAME[snap.terrain], VIS_LABEL[snap.visLvl + 3], snap.visR, snap.attempts);
      Serial.printf("[CONNECT]   name:\"%s\" | players now:%d/%d\n",
        snap.name, snap.connCount, MAX_PLAYERS);

      char buf[48];
      snprintf(buf, sizeof(buf), "{\"t\":\"asgn\",\"id\":%d}", arch);
      client->text(buf);
      sendSync(client, arch);
      broadcastLobbyUpdate();  // tell remaining lobby clients this arch is now taken
    } else {
      // Archetype was already taken (race condition); send refreshed lobby list
      sendLobbyMsg(client);
    }

  } else if (strncmp(tv, "m", (size_t)(te - tv)) == 0) {
    // Move: {"t":"m","d":0-5}
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
          goto move_done;
        }
        movePlayer(slot, dir);          // logs [MOVE] or [BLOCKED]
        playerVisParams(slot, &vr, &mr);
        visLen = buildVisDisk(visBuf, sizeof(visBuf),
                              G.players[slot].q, G.players[slot].r, vr, mr, &visCells);
      }
      xSemaphoreGive(G.mutex);
    }
    if (visLen > 0) {
      Serial.printf("[VIS]     →P%d visR:%d mask:%c cells:%d msgLen:%d/1100B\n",
        slot, vr, mr ? 'Y' : 'N', visCells, visLen);
      client->text(visBuf, (size_t)visLen);
    }
    move_done:;

  } else if (strncmp(tv, "n", (size_t)(te - tv)) == 0) {
    // Name: {"t":"n","name":"X"}
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
      Serial.printf("[NAME]    Slot:%d \"%s\" → \"%s\"\n", slot, oldName, newName);
      char nameBuf[56];
      int  nameLen = snprintf(nameBuf, sizeof(nameBuf),
        "{\"t\":\"ev\",\"k\":\"nm\",\"pid\":%d,\"nm\":\"%s\"}", slot, newName);
      ws.textAll(nameBuf, (size_t)nameLen);
    }

  } else if (strncmp(tv, "wifi", (size_t)(te - tv)) == 0) {
    // WiFi join: {"t":"wifi","ssid":"X","pass":"Y"}
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

  } else if (strncmp(tv, "check", (size_t)(te - tv)) == 0) {
    // Skill check: {"t":"check","sk":N,"dn":N,"bon":N}
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
      Serial.printf("[CHECK]   P%d %s DN%d | %d+%d sk:%d mod:%d = %d | %s\n",
        slot, SKILL_NAME[sk], dn,
        res.r1, res.r2, res.skillVal, res.mods, res.total,
        res.success ? "SUCCESS" : "FAIL");
      broadcastCheck(slot, (uint8_t)sk, res);
    }

  } else if (strncmp(tv, "regen", (size_t)(te - tv)) == 0) {
    // Regen: {"t":"regen"} — regenerate map, scatter all players
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      SD.remove(SAVE_MAP_F);
      SD.remove(SAVE_PLY_F);
      generateMap();
      G.dayCount = 1; G.dayTick = 0; G.threatClock = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& pl = G.players[i];
        if (!pl.connected) continue;
        // Scatter to random open scrub hex
        for (int tries = 0; tries < 200; tries++) {
          int nq = esp_random() % MAP_COLS;
          int nr = esp_random() % MAP_ROWS;
          if (G.map[nr][nq].terrain == 0) { pl.q = (int16_t)nq; pl.r = (int16_t)nr; break; }
        }
        pl.ll = 7; pl.food = 4; pl.water = 4;
        pl.fatigue = 0; pl.radiation = 0; pl.resolve = 3;
        pl.actUsed = false; pl.resting = false;
        pl.movesLeft = (int8_t)effectiveMP(i);
      }
      xSemaphoreGive(G.mutex);
    }
    { GameEvent ev = {}; ev.type = EVT_REGEN; enqEvt(ev); }
    Serial.println("[REGEN]   Map regenerated — all players scattered");

  } else if (strncmp(tv, "eraseslot", (size_t)(te - tv)) == 0) {
    // Erase slot: {"t":"eraseslot","arch":N}
    // Wipes all persistent data for the given archetype slot and saves to SD.
    // If the slot is currently connected the player is evicted back to the lobby.
    const char* ap = strstr(data, "\"arch\""); if (!ap) return;
    const char* av = strchr(ap + 6, ':');      if (!av) return;
    int arch = atoi(av + 1);
    if (arch < 0 || arch >= NUM_ARCHETYPES) return;

    uint32_t evictId  = 0;
    bool     wasConn  = false;

    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      Player& p = G.players[arch];
      // If this slot is live, note the client ID so we can evict them below
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
      memset(p.wounds,      0, sizeof(p.wounds));
      memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
      memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
      p.archetype    = (uint8_t)arch;
      p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
      p.ll           = 0;
      p.food         = 0; p.water     = 0;
      p.fatigue      = 0; p.radiation = 0; p.resolve = 0;
      p.score        = 0; p.steps     = 0;
      p.statusBits   = 0;
      p.movesLeft    = 0;
      p.actUsed      = false;
      p.encPenApplied = false;
      p.radClean     = true;
      p.fThreshBelow = 0; p.wThreshBelow = 0;
      p.lastMoveMs   = 0;
      xSemaphoreGive(G.mutex);
    }

    // Notify evicted client (if any) with a downed event so they see the death
    // redirect and land on char select; then return them to the lobby list
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
      // Broadcast left so all clients remove the slot's player icon
      evLen = snprintf(evBuf, sizeof(evBuf),
        "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", arch);
      ws.textAll(evBuf, evLen);
    }

    broadcastLobbyUpdate();
    saveGame();
    Serial.printf("[ERASE]   Slot %d (%s) wiped\n", arch, ARCHETYPE_NAME[arch]);

  } else if (strncmp(tv, "act", (size_t)(te - tv)) == 0) {
    // Action: {"t":"act","a":N[,"mp":N]}
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
          goto act_done;
        }
        handleAction(slot, (uint8_t)actType, mpParam, 0,
                     survBuf, sizeof(survBuf), &survLen);
      }
      xSemaphoreGive(G.mutex);
    }
    if (survLen > 0)
      client->text(survBuf, (size_t)survLen);
    act_done:;

  } else if (strncmp(tv, "trade_offer", (size_t)(te - tv)) == 0) {
    // Trade offer: {"t":"trade_offer","to":N,"give":[w,f,fu,m,s],"want":[w,f,fu,m,s]}
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

    // Validate totals non-zero
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
        // Check no duplicate active offer between this pair
        bool dup = false;
        for (int ti = 0; ti < MAX_PLAYERS; ti++) {
          if (!tradeOffers[ti].active) continue;
          if ((ti == fromSlot && tradeOffers[ti].toPid == (uint8_t)toPid) ||
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
          Serial.printf("[TRADE]   P%d → P%d offer queued\n", fromSlot, toPid);
        }
      }
      xSemaphoreGive(G.mutex);
    }
    if (!valid) {
      char fb[48];
      int fl = snprintf(fb, sizeof(fb), "{\"t\":\"trade_fail\"}");
      client->text(fb, (size_t)fl);
    }

  } else if (strncmp(tv, "trade_accept", (size_t)(te - tv)) == 0) {
    // Accept trade: {"t":"trade_accept","from":N}
    const char* fp = strstr(data, "\"from\""); if (!fp) return;
    const char* fv = strchr(fp + 6, ':');      if (!fv) return;
    int fromPid = atoi(fv + 1);
    if (fromPid < 0 || fromPid >= MAX_PLAYERS) return;

    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int mySlot = findSlot(client->id());
      if (mySlot >= 0 && (encounters[mySlot].active || encounters[fromPid].active)) {
        xSemaphoreGive(G.mutex);
        client->text("{\"t\":\"err\",\"msg\":\"Cannot trade during encounter\"}");
        return;
      }
      GameEvent tev = {};
      tev.type    = EVT_TRADE_RESULT;
      tev.pid     = (uint8_t)fromPid;
      if (mySlot >= 0 &&
          tradeOffers[fromPid].active &&
          tradeOffers[fromPid].toPid == (uint8_t)mySlot &&
          millis() < tradeOffers[fromPid].expiresMs &&
          samehex(fromPid, mySlot) &&
          hasResources(fromPid, tradeOffers[fromPid].give) &&
          hasResources(mySlot,  tradeOffers[fromPid].want)) {
        executeTrade(fromPid, mySlot, tradeOffers[fromPid]);
        tev.tradeTo     = (uint8_t)mySlot;
        tev.tradeResult = 1; // accepted
        Serial.printf("[TRADE]   P%d ↔ P%d ACCEPTED\n", fromPid, mySlot);
      } else if (mySlot >= 0) {
        tev.tradeTo     = (uint8_t)mySlot;
        tev.tradeResult = 2; // declined/invalid
        Serial.printf("[TRADE]   P%d → P%d accept FAILED (validation)\n", fromPid, mySlot);
      }
      tradeOffers[fromPid].active = false;
      enqEvt(tev);
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "trade_decline", (size_t)(te - tv)) == 0) {
    // Decline trade: {"t":"trade_decline","from":N}
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
        tev.tradeResult = 2; // declined
        enqEvt(tev);
        Serial.printf("[TRADE]   P%d → P%d DECLINED\n", fromPid, mySlot);
      }
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "use_item", (size_t)(te - tv)) == 0) {
    // Use a consumable: {"t":"use_item","slot":N}
    const char* sp = strstr(data, "\"slot\"");
    if (!sp) return;
    const char* sv2 = strchr(sp + 6, ':'); if (!sv2) return;
    int slotIdx = atoi(sv2 + 1);
    if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) return;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int mySlot = findSlot(client->id());
      if (mySlot >= 0 && G.players[mySlot].connected) {
        // Capture narrative effect param before item is consumed
        Player& pl = G.players[mySlot];
        uint8_t narParam = 0;
        if (slotIdx < INV_SLOTS_MAX && pl.invType[slotIdx]) {
          const ItemDef* preDef = getItemDef(pl.invType[slotIdx]);
          if (preDef && preDef->effectId == EFX_NARRATIVE) narParam = preDef->effectParam;
        }
        bool ok = useItem(mySlot, (uint8_t)slotIdx);
        if (ok) saveGame();
        static char ack[320];
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
        client->text(ack);
      }
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "equip_item", (size_t)(te - tv)) == 0) {
    // Equip an item: {"t":"equip_item","slot":N}
    const char* sp = strstr(data, "\"slot\"");
    if (!sp) return;
    const char* sv2 = strchr(sp + 6, ':'); if (!sv2) return;
    int slotIdx = atoi(sv2 + 1);
    if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) return;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int mySlot = findSlot(client->id());
      if (mySlot >= 0 && G.players[mySlot].connected) {
        bool ok = equipItem(mySlot, (uint8_t)slotIdx);
        if (ok) saveGame();
        static char ack[256];
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
        client->text(ack);
      }
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "unequip_item", (size_t)(te - tv)) == 0) {
    // Unequip from equipment slot: {"t":"unequip_item","eslot":N}  (0=HEAD..4=VEHICLE)
    const char* ep = strstr(data, "\"eslot\"");
    if (!ep) return;
    const char* ev2 = strchr(ep + 7, ':'); if (!ev2) return;
    int eslot = atoi(ev2 + 1);
    if (eslot < 0 || eslot >= EQUIP_SLOTS) return;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int mySlot = findSlot(client->id());
      if (mySlot >= 0 && G.players[mySlot].connected) {
        bool ok = unequipItem(mySlot, (uint8_t)eslot);
        if (ok) saveGame();
        static char ack[256];
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
        client->text(ack);
      }
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "drop_item", (size_t)(te - tv)) == 0) {
    // Drop item at current hex: {"t":"drop_item","slot":N,"qty":K}
    const char* sp = strstr(data, "\"slot\"");
    if (!sp) return;
    const char* sv2 = strchr(sp + 6, ':'); if (!sv2) return;
    int slotIdx = atoi(sv2 + 1);
    const char* qp = strstr(data, "\"qty\"");
    int qty = qp ? atoi(strchr(qp + 5, ':') + 1) : 1;
    if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX || qty <= 0) return;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int mySlot = findSlot(client->id());
      if (mySlot >= 0 && G.players[mySlot].connected) {
        bool ok = dropItem(mySlot, (uint8_t)slotIdx, (uint8_t)qty);
        Player& p = G.players[mySlot];
        if (ok) {
          saveGame();
          // Broadcast ground item update with full gi[] list to all clients
          static char upd[1280];
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
          ws.textAll(upd);
        }
        static char ack[256];
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
        client->text(ack);
      }
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "pickup_item", (size_t)(te - tv)) == 0) {
    // Pick up a ground item: {"t":"pickup_item","gslot":N}
    const char* gp = strstr(data, "\"gslot\"");
    if (!gp) return;
    const char* gv = strchr(gp + 7, ':'); if (!gv) return;
    int gslot = atoi(gv + 1);
    if (gslot < 0 || gslot >= MAX_GROUND) return;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int mySlot = findSlot(client->id());
      if (mySlot >= 0 && G.players[mySlot].connected) {
        bool ok = pickupGroundItem(mySlot, (uint8_t)gslot);
        Player& p2 = G.players[mySlot];
        if (ok) {
          saveGame();
          // Broadcast ground item update with full gi[] list to all clients
          static char upd[1280];
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
          ws.textAll(upd);
        }
        static char ack[256];
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
        client->text(ack);
      }
      xSemaphoreGive(G.mutex);
    }

  } else if (strncmp(tv, "settings", (size_t)(te - tv)) == 0) {
    // Settings: {"t":"settings","audioVol":N,"ledBright":N}
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
    saveK10Prefs();

  // ── Encounter: enter POI ─────────────────────────────────────────────────────
  } else if (strncmp(tv, "enc_start", (size_t)(te - tv)) == 0) {
    // {"t":"enc_start","q":5,"r":8}
    const char* qp = strstr(data, "\"q\""); if (!qp) return;
    const char* qv = strchr(qp + 3, ':');  if (!qv) return;
    const char* rp = strstr(data, "\"r\""); if (!rp) return;
    const char* rv = strchr(rp + 3, ':');  if (!rv) return;
    int hq = atoi(qv + 1), hr = atoi(rv + 1);
    if (hq < 0 || hq >= MAP_COLS || hr < 0 || hr >= MAP_ROWS) return;

    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int pid = findSlot(client->id());
      if (pid >= 0 && !encounters[pid].active && !(G.players[pid].statusBits & ST_DOWNED)) {
        Player& p   = G.players[pid];
        HexCell& cell = G.map[hr][hq];
        if ((int)p.q != hq || (int)p.r != hr) {
          Serial.printf("[ENC] REJECT 'Not at hex': player at (%d,%d), tried (%d,%d)\n", p.q, p.r, hq, hr);
          client->text("{\"t\":\"err\",\"msg\":\"Not at that hex\"}");
        } else if (cell.poi == 0) {
          Serial.printf("[ENC] REJECT 'Already looted': cell.poi=0 at (%d,%d)\n", hq, hr);
          client->text("{\"t\":\"err\",\"msg\":\"Already looted\"}");
        } else {
          // Check if another player is already inside at this hex
          bool claimed = false;
          for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pid || !encounters[i].active) continue;
            if (encounters[i].hexQ == (uint8_t)hq && encounters[i].hexR == (uint8_t)hr) {
              claimed = true; break;
            }
          }
          if (claimed) {
            client->text("{\"t\":\"err\",\"msg\":\"Another survivor is already inside\"}");
          } else {
            uint8_t terrain = cell.terrain;
            if (terrain >= 10 || encPools[terrain].count == 0) {
              Serial.printf("[ENC] REJECT 'No encounters here': terrain=%d (>=10:%s), pool count=%d, path='%s'\n",
                terrain,
                terrain >= 10 ? "YES" : "no",
                terrain < 10 ? (int)encPools[terrain].count : -1,
                terrain < 10 ? encPools[terrain].path : "N/A");
              client->text("{\"t\":\"err\",\"msg\":\"No encounters here\"}");
            } else {
              uint8_t idx = cell.poi;  // poi stores the pre-placed encounter ID (1..N)
              cell.poi = 0;  // consume POI immediately
              if (G.threatClock < 20) G.threatClock++;
              encounters[pid].active           = 1;
              encounters[pid].encIdx           = idx;
              encounters[pid].hexQ             = (uint8_t)hq;
              encounters[pid].hexR             = (uint8_t)hr;
              memset(encounters[pid].pendingLoot,     0, 5);
              memset(encounters[pid].pendingItemType, 0, ENC_MAX_ITEMS);
              memset(encounters[pid].pendingItemQty,  0, ENC_MAX_ITEMS);
              encounters[pid].pendingItemCount = 0;
              encounters[pid].assistRisk       = 0;
              encounters[pid].assistUsed       = 0;
              // Send encounter path directly to active player
              char pathBuf[72];
              int pathLen = snprintf(pathBuf, sizeof(pathBuf),
                "{\"t\":\"enc_path\",\"biome\":\"%s\",\"id\":%d}",
                encPools[terrain].path, (int)idx);
              client->text(pathBuf, (size_t)pathLen);
              Serial.printf("[ENC] Serving -> /data/encounters/%s/%d.json\n", encPools[terrain].path, (int)idx);
              // Broadcast EVT_ENC_START to allies
              GameEvent ev = {};
              ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
              ev.q = (int16_t)hq; ev.r = (int16_t)hr;
              enqEvt(ev);
              Serial.printf("[ENC] P%d enters %s/%d at (%d,%d)\n",
                pid, encPools[terrain].path, (int)idx, hq, hr);
            }
          }
        }
      }
      xSemaphoreGive(G.mutex);
    }

  // ── Encounter: make a choice ─────────────────────────────────────────────────
  } else if (strncmp(tv, "enc_choice", (size_t)(te - tv)) == 0) {
    // {"t":"enc_choice","ci":0,"cost_ll":0,"cost_fat":1,"cost_rad":0,"cost_food":0,
    //  "cost_water":0,"cost_resolve":0,"base_risk":35,"skill":2,
    //  "loot":[0,0,0,1,0],"lt":"urban_common","can_bank":true,
    //  "is_terminal":false,
    //  "haz_ll":0,"haz_fat":0,"haz_rad":0,"haz_st":0,"haz_wt":0,"haz_wc":0,"haz_ends":0}
    #define ENC_JP(field, key) const char* field##_p = strstr(data, "\"" key "\""); \
      int field = 0; if (field##_p) { const char* vp = strchr(field##_p + strlen("\"" key "\""), ':'); if (vp) field = atoi(vp + 1); }
    ENC_JP(costLL,  "cost_ll")   ENC_JP(costFat,  "cost_fat")
    ENC_JP(costRad, "cost_rad")  ENC_JP(costFood, "cost_food")
    ENC_JP(costWat, "cost_water") ENC_JP(costRes, "cost_resolve")
    ENC_JP(costScrap, "cost_scrap")
    ENC_JP(baseRisk,"base_risk") ENC_JP(skill,    "skill")
    ENC_JP(isTerm,  "is_terminal")
    ENC_JP(hazLL,   "haz_ll")   ENC_JP(hazFat, "haz_fat")
    ENC_JP(hazRad,  "haz_rad")  ENC_JP(hazSt,  "haz_st")
    ENC_JP(hazWt,   "haz_wt")   ENC_JP(hazWc,  "haz_wc")
    ENC_JP(hazEnds, "haz_ends") ENC_JP(hazLoseCon, "haz_lose_consumable")
    #undef ENC_JP
    // Parse loot array — indices map directly to p.inv[]: 0=Water 1=Food 2=Fuel 3=Medicine 4=Scrap.
    // NOTE: encounter JSON uses 0-based res indices; map cell.resource uses 1-based. Different spaces.
    uint8_t lootArr[5] = {0};
    const char* lp = strstr(data, "\"loot\"");
    if (lp) { const char* lb = strchr(lp + 6, '['); if (lb) { lb++;
      for (int i = 0; i < 5; i++) {
        while (*lb == ' ') lb++;
        lootArr[i] = (uint8_t)constrain(atoi(lb), 0, 99);
        const char* nx = strchr(lb, i < 4 ? ',' : ']'); if (!nx) break; lb = nx + 1;
      }
    }}
    // Parse loot_table string
    char ltName[20] = {0};
    const char* ltp = strstr(data, "\"lt\"");
    if (ltp) { const char* lts = strchr(ltp + 4, '"'); if (lts) { lts++;
      const char* lte = strchr(lts + 1, '"');
      if (lte) { int ln = min((int)(lte - lts), 19); strncpy(ltName, lts, ln); ltName[ln] = 0; }
    }}

    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int pid = findSlot(client->id());
      if (pid >= 0 && encounters[pid].active && !(G.players[pid].statusBits & ST_DOWNED)) {
        Player& p = G.players[pid];
        ActiveEncounter& enc = encounters[pid];
        // Validate affordability
        bool canAfford = (p.ll >= costLL) && (p.fatigue + costFat <= 8) &&
                         (p.radiation + costRad <= 10) &&
                         (p.inv[1] >= costFood) && (p.inv[0] >= costWat) &&
                         (p.resolve >= costRes) && (p.inv[4] >= costScrap);
        if (!canAfford) {
          client->text("{\"t\":\"err\",\"msg\":\"Cannot afford cost\"}");
          xSemaphoreGive(G.mutex); return;
        }
        // Deduct costs
        if (costLL)   { p.ll = (uint8_t)max(0, (int)p.ll - costLL); if (p.ll == 0) { p.statusBits |= ST_DOWNED; p.movesLeft = 0; } }
        p.fatigue  = (uint8_t)constrain((int)p.fatigue  + costFat, 0, 8);
        p.radiation= (uint8_t)constrain((int)p.radiation+ costRad, 0, 10);
        p.inv[1]   = (uint8_t)max(0, (int)p.inv[1] - costFood);
        p.inv[0]   = (uint8_t)max(0, (int)p.inv[0] - costWat);
        p.resolve  = (uint8_t)max(0, (int)p.resolve - costRes);
        p.inv[4]   = (uint8_t)max(0, (int)p.inv[4] - costScrap);
        updateRadStatus(p);
        // Compute DN and roll
        uint8_t dn = computeEncounterDN(pid, (uint8_t)constrain(baseRisk, 0, 100),
                                        (uint8_t)constrain(skill, 0, 5), enc.assistRisk);
        CheckResult cr = resolveCheck(pid, (uint8_t)constrain(skill, 0, 5), dn, 0);
        // Build result event
        GameEvent ev = {};
        ev.type      = EVT_ENC_RESULT;
        ev.pid       = (uint8_t)pid;
        ev.encSkill  = (uint8_t)constrain(skill, 0, 5);
        ev.encDN     = dn;
        ev.encTotal  = (int8_t)cr.total;
        ev.encOut    = cr.success ? 1 : 0;
        bool encounterEnded = false;
        if (cr.success) {
          // Accumulate loot
          for (int i = 0; i < 5; i++) {
            enc.pendingLoot[i] = (uint8_t)min(99, (int)enc.pendingLoot[i] + (int)lootArr[i]);
            ev.encLoot[i] = lootArr[i];
          }
          // Roll loot table item if present
          if (ltName[0]) {
            uint8_t itm = 0, qty = 0;
            rollLootTable(ltName, &itm, &qty);
            if (itm && qty && enc.pendingItemCount < ENC_MAX_ITEMS) {
              enc.pendingItemType[enc.pendingItemCount] = itm;
              enc.pendingItemQty[enc.pendingItemCount]  = qty;
              enc.pendingItemCount++;
              ev.encItemType = itm; ev.encItemQty = qty;
            }
          }
          // Mark terminal node
          if (isTerm) enc.active |= (1 << 7);
        } else {
          // Apply hazard penalties
          ev.encPenLL  = (int8_t)hazLL;
          ev.encPenFat = (int8_t)hazFat;
          ev.encPenRad = (int8_t)hazRad;
          ev.encStatus = (uint8_t)hazSt;
          ev.encEnds   = (uint8_t)(hazEnds ? 1 : 0);
          if (hazLL > 0) {
            Serial.printf("[ENC] WARNING: P%d haz_ll=%d is positive — LL penalties must be negative in JSON. Ignored.\n", pid, hazLL);
          } else if (hazLL < 0) {
            int newLL = (int)p.ll + hazLL;
            p.ll = (uint8_t)max(0, newLL);
            if (p.ll == 0) { p.statusBits |= ST_DOWNED; p.movesLeft = 0; }
            if (p.ll == 0) ledFlash(255, 0, 0);
          }
          p.fatigue  = (uint8_t)constrain((int)p.fatigue  + hazFat, 0, 8);
          p.radiation= (uint8_t)constrain((int)p.radiation+ hazRad, 0, 10);
          if (hazRad) updateRadStatus(p);
          if (hazSt)  p.statusBits |= (uint8_t)hazSt;
          if (hazLoseCon) {
            // Remove a random consumable from the player's item slots
            int slots[INV_SLOTS_MAX]; int slotCount = 0;
            for (int s = 0; s < (int)p.invSlots; s++) {
              if (p.invType[s] != 0) {
                const ItemDef* def = getItemDef(p.invType[s]);
                if (def && def->category == 0) slots[slotCount++] = s; // category 0 = consumable
              }
            }
            if (slotCount > 0) {
              int target = slots[random(0, slotCount)];
              Serial.printf("[ENC] P%d haz_lose_consumable: dropped item:%d from slot %d\n",
                pid, (int)p.invType[target], target);
              p.invType[target] = 0;
              p.invQty[target]  = 0;
            }
          }
          if (hazWc > 0 && hazWt < 3) {
            p.wounds[hazWt] = (uint8_t)min(10, (int)p.wounds[hazWt] + hazWc);
            if (p.wounds[hazWt]) p.statusBits |= ST_WOUNDED;
          }
          bool downed = (p.statusBits & ST_DOWNED) != 0;
          encounterEnded = downed || (hazEnds != 0);
          // Auto-drain co-located allies: major hazard (LL loss or wound) costs 2, minor costs 1
          bool isMajor = (hazLL < 0 || hazWc > 0);
          int drainAmt = isMajor ? 2 : 1;
          for (int ally = 0; ally < MAX_PLAYERS; ally++) {
            if (ally == pid || !G.players[ally].connected) continue;
            if ((int)G.players[ally].q != (int)p.q || (int)G.players[ally].r != (int)p.r) continue;
            int drained = 0;
            for (int ri = 0; ri < 5 && drained < drainAmt; ri++) {
              if (G.players[ally].inv[ri] > 0) { G.players[ally].inv[ri]--; drained++; }
            }
            ev.encDrains[ally] = (uint8_t)drained;
          }
        }
        // Reset per-node assist state
        enc.assistRisk = 0; enc.assistUsed = 0;
        // Result must be broadcast before EVT_ENC_END so allies see the outcome before the encounter clears.
        enqEvt(ev);
        if (encounterEnded) {
          // End encounter — loot lost
          if (p.statusBits & ST_DOWNED) {
            GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
            enqEvt(devt);
          }
          uint8_t hq = enc.hexQ, hr2 = enc.hexR;
          enc = {};
          // Always send EVT_ENC_END so allies can clear the banner regardless of downed state
          { GameEvent eev = {}; eev.type = EVT_ENC_END; eev.pid = (uint8_t)pid;
            eev.q = (int16_t)hq; eev.r = (int16_t)hr2;
            eev.encOut = (G.players[pid].statusBits & ST_DOWNED) ? 3 : 0; // 3=downed, 0=hazard
            enqEvt(eev); }
        }
        Serial.printf("[ENC] P%d choice: DN%d roll%d %s%s\n",
          pid, dn, cr.total, cr.success ? "OK" : "FAIL", encounterEnded ? " (end)" : "");
      }
      xSemaphoreGive(G.mutex);
    }

  // ── Encounter: bank loot and exit ────────────────────────────────────────────
  } else if (strncmp(tv, "enc_bank", (size_t)(te - tv)) == 0) {
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int pid = findSlot(client->id());
      if (pid >= 0 && encounters[pid].active) {
        Player& p = G.players[pid];
        ActiveEncounter& enc = encounters[pid];
        bool fullClear = (enc.active & (1 << 7)) != 0;
        // Transfer resource loot
        int totalRes = 0;
        for (int i = 0; i < 5; i++) {
          p.inv[i] = (uint8_t)min(99, (int)p.inv[i] + (int)enc.pendingLoot[i]);
          totalRes += enc.pendingLoot[i];
        }
        // Transfer typed items — overflow goes to ground
        for (int j = 0; j < enc.pendingItemCount; j++) {
          uint8_t itemId = enc.pendingItemType[j];
          uint8_t qty    = enc.pendingItemQty[j];
          if (!itemId || !qty) continue;
          bool placed = false;
          for (int s = 0; s < p.invSlots && !placed; s++) {
            if (p.invType[s] == itemId) {
              const ItemDef* def = getItemDef(itemId);
              uint8_t cap = def ? def->maxStack : 1;
              if (p.invQty[s] < cap) {
                uint8_t space = cap - p.invQty[s];
                uint8_t add   = min(qty, space);
                p.invQty[s]  += add;
                qty           -= add;
                placed = (qty == 0);
              }
            }
          }
          if (!placed) {
            for (int s = 0; s < p.invSlots && !placed; s++) {
              if (!p.invType[s]) {
                p.invType[s] = itemId; p.invQty[s] = qty; placed = true;
              }
            }
          }
          if (!placed) {
            // Inventory full — drop to ground
            int gslot = -1;
            for (int g = 0; g < MAX_GROUND; g++) {
              if (groundItems[g].itemType == itemId && groundItems[g].q == p.q && groundItems[g].r == p.r)
                { gslot = g; break; }
            }
            if (gslot < 0) {
              for (int g = 0; g < MAX_GROUND; g++) {
                if (!groundItems[g].itemType) { gslot = g; break; }
              }
            }
            if (gslot >= 0) {
              groundItems[gslot].q = p.q; groundItems[gslot].r = p.r;
              groundItems[gslot].itemType = itemId;
              groundItems[gslot].qty = (uint8_t)min(255, (int)groundItems[gslot].qty + (int)qty);
            } else {
              Serial.printf("[ENC] P%d bank item:%d x%d LOST — MAX_GROUND full\n", pid, (int)itemId, (int)qty);
            }
          }
        }
        // Score: +3 per resource unit banked, +10 full clear
        int scoreGain = totalRes * 3 + (fullClear ? 10 : 0);
        GameEvent ev = {};
        ev.type = EVT_ENC_BANK; ev.pid = (uint8_t)pid;
        ev.q = (int16_t)enc.hexQ; ev.r = (int16_t)enc.hexR;
        memcpy(ev.encLoot, enc.pendingLoot, 5);
        addScore(p, ev, scoreGain);
        p.encCount++;
        enqEvt(ev);
        Serial.printf("[ENC] P%d banked (res:%d score+%d%s)\n",
          pid, totalRes, scoreGain, fullClear ? " FULLCLEAR" : "");
        enc = {};
      }
      xSemaphoreGive(G.mutex);
    }

  // ── Encounter: voluntary abort ────────────────────────────────────────────────
  } else if (strncmp(tv, "enc_abort", (size_t)(te - tv)) == 0) {
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int pid = findSlot(client->id());
      if (pid >= 0 && encounters[pid].active) {
        uint8_t hq = encounters[pid].hexQ, hr2 = encounters[pid].hexR;
        encounters[pid] = {};
        if (G.threatClock < 20) G.threatClock++;  // TC +1 for voluntary abort
        GameEvent ev = {}; ev.type = EVT_ENC_END; ev.pid = (uint8_t)pid;
        ev.q = (int16_t)hq; ev.r = (int16_t)hr2; ev.encOut = 1;  // reason: abort
        enqEvt(ev);
        Serial.printf("[ENC] P%d aborted (TC now %d)\n", pid, (int)G.threatClock);
      }
      xSemaphoreGive(G.mutex);
    }

  }
}

// ── WebSocket event dispatcher ────────────────────────────────────────────────
static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:    handleConnect(client); break;
    case WS_EVT_DISCONNECT: handleDisconnect(client); break;
    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        handleMessage(client, (char*)data, len);
      }
      break;
    }
    default: break;
  }
}
