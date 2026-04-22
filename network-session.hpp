#pragma once
// ── WebSocket session lifecycle: connect, disconnect, WiFi join task ──────────
// Included from Esp32HexMapCrawl.ino before network-msg-*.hpp files.

static void handleConnect(AsyncWebSocketClient* client) {
  // Cleanup stale connections: p.connected=true but WS client is gone (ungraceful close)
  {
    bool freedAny = false;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& p = G.players[i];
        if (p.connected && !ws.client(p.wsClientId)) {
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
    client->text("{\"t\":\"full\"}");
    return;
  }

  bool added = false;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!lobbyIds[i]) { lobbyIds[i] = client->id(); added = true; break; }
  }
  taskEXIT_CRITICAL(&evtMux);

  if (!added) { client->text("{\"t\":\"full\"}"); return; }

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

static void handleDisconnect(AsyncWebSocketClient* client) {
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

  broadcastLobbyUpdate();
  saveGame();
}

// ── WiFi STA join task (Core 0) ───────────────────────────────────────────────
// Runs on Core 0; attempts STA join while keeping AP alive; broadcasts result.
static void wifiConnectTask(void* param) {
  WifiTaskCtx* ctx = (WifiTaskCtx*)param;


  // Only switch mode if not already in AP+STA — re-calling WiFi.mode() when
  // already in WIFI_AP_STA can reset the WiFi stack and drop the softAP.
  if (WiFi.getMode() != WIFI_MODE_APSTA) {
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  WiFi.begin(ctx->ssid, ctx->pass[0] ? ctx->pass : nullptr);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  char buf[88]; int blen;
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    blen = snprintf(buf, sizeof(buf),
      "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}", ip.c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // Cache in globals so handleConnect can echo creds to new clients.
    // ESP32 already saved these to its own internal NVS when WiFi.begin(ssid,pass)
    // was called above — no separate Preferences write needed.
    strlcpy(savedSsid, ctx->ssid, sizeof(savedSsid));
    strlcpy(savedPass, ctx->pass, sizeof(savedPass));
  } else {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    // Clear in-memory SSID so handleConnect won't send a 'saved' message that
    // would suppress the client's auto-send retry (NVS copy is kept for next boot).
    savedSsid[0] = '\0';
    blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"fail\"}");
  }

  ws.textAll(buf, (size_t)blen);
  free(ctx);
  wifiConnecting = false;
  vTaskDelete(NULL);
}
