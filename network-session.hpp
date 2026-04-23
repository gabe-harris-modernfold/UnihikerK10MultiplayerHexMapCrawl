#pragma once
// ── WebSocket session lifecycle: connect, disconnect, WiFi join task ──────────
// Included from Esp32HexMapCrawl.ino before network-msg-*.hpp files.

static void handleConnect(AsyncWebSocketClient* client) {
  Log.notice("WS CONNECT id=%u ip=%s",
             (unsigned)client->id(), client->remoteIP().toString().c_str());

  // Cleanup stale connections: p.connected=true but WS client is gone (ungraceful close)
  {
    bool freedAny = false;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& p = G.players[i];
        if (p.connected && !ws.client(p.wsClientId)) {
          Log.warning("Reaped stale player slot=%d oldWsId=%u (ungraceful close)",
                      i, (unsigned)p.wsClientId);
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

  int connectedCount = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    connectedCount = G.connectedCount;
    xSemaphoreGive(G.mutex);
  }
  int lobbySize = 0;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) if (lobbyIds[i]) lobbySize++;
  taskEXIT_CRITICAL(&evtMux);

  if (connectedCount + lobbySize >= MAX_PLAYERS) {
    Log.warning("WS REJECT id=%u reason=full connected=%d lobby=%d",
                (unsigned)client->id(), connectedCount, lobbySize);
    client->text("{\"t\":\"full\"}");
    return;
  }

  bool added = false;
  int addedSlot = -1;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!lobbyIds[i]) { lobbyIds[i] = client->id(); added = true; addedSlot = i; break; }
  }
  taskEXIT_CRITICAL(&evtMux);

  if (!added) {
    Log.error("Lobby add failed id=%u — no free slot despite capacity check",
              (unsigned)client->id());
    client->text("{\"t\":\"full\"}"); return;
  }
  Log.notice("Lobby add: id=%u slot=%d connected=%d lobby=%d",
             (unsigned)client->id(), addedSlot, connectedCount, lobbySize + 1);

  // If we have saved WiFi credentials, echo them to this client so its
  // localStorage (and the Settings inputs) stay in sync across devices/reboots.
  if (savedSsid[0]) {
    Log.verbose("WS echo wifi creds to id=%u ssid=%s", (unsigned)client->id(), savedSsid);
    char credBuf[160];
    int credLen = snprintf(credBuf, sizeof(credBuf),
      "{\"t\":\"wifi\",\"status\":\"saved\",\"ssid\":\"%s\",\"pass\":\"%s\"}",
      savedSsid, savedPass);
    client->text(credBuf, (size_t)credLen);
  }
  Log.verbose("WS send lobby msg id=%u", (unsigned)client->id());
  sendLobbyMsg(client);
}

static void handleDisconnect(AsyncWebSocketClient* client) {
  Log.notice("WS DISCONNECT id=%u", (unsigned)client->id());
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
    Log.notice("Lobby remove: id=%u (was in lobby, not in game)", (unsigned)client->id());
    return;
  }

  int      slot    = -1;
  char     name[12] = {0};
  uint16_t steps   = 0, score = 0;
  uint8_t  ll = 0, food = 0, water = 0, rad = 0;
  uint32_t connMs  = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    slot = findSlot(client->id());
    if (slot >= 0) {
      Player& p = G.players[slot];
      memcpy(name, p.name, 12);
      steps   = p.steps;   score   = p.score; connMs = p.connectMs;
      ll      = p.ll;      food    = p.food;  water  = p.water;
      rad     = p.radiation;
      p.connected  = false;
      p.wsClientId = 0;
      p.resting    = false;
      G.connectedCount--;
      // Clear active encounter on disconnect (POI already consumed at enc_start)
      if (encounters[slot].active) {
        uint8_t hq = encounters[slot].hexQ, hr = encounters[slot].hexR;
        Log.warning("Encounter ended by disconnect slot=%d q=%u r=%u", slot, hq, hr);
        encounters[slot] = {};
        GameEvent eev = {}; eev.type = EVT_ENC_END; eev.pid = (uint8_t)slot;
        eev.q = (int16_t)hq; eev.r = (int16_t)hr; eev.encOut = 4;  // reason: disconnect
        enqEvt(eev);
      }
      { GameEvent ev = {}; ev.type = EVT_LEFT; ev.pid = (uint8_t)slot; enqEvt(ev);
        Log.verbose("Enq EVT_LEFT pid=%d", slot); }
    }
    xSemaphoreGive(G.mutex);
  }
  if (slot < 0) {
    Log.warning("Disconnect id=%u not found in any slot or lobby",
                (unsigned)client->id());
    return;
  }

  uint32_t sessSec = (millis() - connMs) / 1000;
  Log.notice("Player LEFT slot=%d name=%s score=%u steps=%u ll=%u food=%u water=%u rad=%u sessionSec=%u",
             slot, name, (unsigned)score, (unsigned)steps,
             (unsigned)ll, (unsigned)food, (unsigned)water, (unsigned)rad,
             (unsigned)sessSec);

  broadcastLobbyUpdate();
  Log.notice("Auto-save triggered by disconnect slot=%d", slot);
  saveGame();
}

// ── WiFi STA join task (Core 0) ───────────────────────────────────────────────
// Runs on Core 0; attempts STA join while keeping AP alive; broadcasts result.
static void wifiConnectTask(void* param) {
  WifiTaskCtx* ctx = (WifiTaskCtx*)param;

  Log.notice("STA task start ssid=%s hasPass=%d", ctx->ssid, ctx->pass[0] ? 1 : 0);

  // Only switch mode if not already in AP+STA — re-calling WiFi.mode() when
  // already in WIFI_AP_STA can reset the WiFi stack and drop the softAP.
  if (WiFi.getMode() != WIFI_MODE_APSTA) {
    Log.verbose("Switching mode -> WIFI_AP_STA");
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  Log.notice("STA begin ssid=%s", ctx->ssid);
  WiFi.begin(ctx->ssid, ctx->pass[0] ? ctx->pass : nullptr);

  unsigned long t0 = millis();
  wl_status_t lastStatus = (wl_status_t)255;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    wl_status_t cur = WiFi.status();
    if (cur != lastStatus) {
      Log.verbose("STA status=%d elapsed=%lums", (int)cur, (unsigned long)(millis() - t0));
      lastStatus = cur;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  char buf[88]; int blen;
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Log.notice("STA connected ssid=%s ip=%s rssi=%d elapsed=%lums",
               ctx->ssid, ip.c_str(), (int)WiFi.RSSI(),
               (unsigned long)(millis() - t0));
    blen = snprintf(buf, sizeof(buf),
      "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}", ip.c_str());
    Log.notice("NTP configTime(pool.ntp.org, time.nist.gov) called");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // Cache in globals so handleConnect can echo creds to new clients.
    // ESP32 already saved these to its own internal NVS when WiFi.begin(ssid,pass)
    // was called above — no separate Preferences write needed.
    strlcpy(savedSsid, ctx->ssid, sizeof(savedSsid));
    strlcpy(savedPass, ctx->pass, sizeof(savedPass));
  } else {
    Log.warning("STA FAIL ssid=%s elapsed=%lums reverting to AP-only",
                ctx->ssid, (unsigned long)(millis() - t0));
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    // Clear in-memory SSID so handleConnect won't send a 'saved' message that
    // would suppress the client's auto-send retry (NVS copy is kept for next boot).
    savedSsid[0] = '\0';
    blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"fail\"}");
  }

  Log.verbose("WS broadcast wifi result: %s", buf);
  ws.textAll(buf, (size_t)blen);
  free(ctx);
  wifiConnecting = false;
  Log.notice("STA task exit");
  vTaskDelete(NULL);
}
