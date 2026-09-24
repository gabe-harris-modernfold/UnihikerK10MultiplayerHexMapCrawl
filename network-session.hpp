#pragma once
// ── WebSocket session lifecycle: connect, disconnect, WiFi join task ──────────
// Included from Esp32HexMapCrawl.ino before network-msg-*.hpp files.

// Tell every client which networks the board will auto-join, and which one it
// is on right now. Passwords stay on the board — this list is SSIDs only.
static void broadcastWifiNets() {
  char buf[700];
  int len = wifiStoreNetsJson(buf, sizeof(buf), savedSsid);
  ws.textAll(buf, (size_t)len);
}

// ── WS liveness tracking ─────────────────────────────────────────────────────
// ws.client(id) only returns a client whose status() is exactly WS_CONNECTED,
// so a healthy-but-momentarily-invisible client reads as gone.  handleConnect
// used to reap on that single observation, which let one client connecting
// evict a different, live player — and because the reap clears p.connected
// *without* closing the socket, the evicted player kept receiving the full
// broadcast while believing it was still in the game.  Its score simply
// stopped changing.
//
// That is self-sustaining: the evicted client eventually reconnects, which
// runs the reap again, which can evict another live player.  Measured with
// instrumented bot clients: 5 silent seat losses in a 90-second run with only
// 4 players connected.
//
// Fix: remember when each slot was last positively seen and only reap one that
// has been unverifiable for the whole grace window.  Graceful closes are
// unaffected — handleDisconnect still frees those immediately — so this only
// delays cleanup of genuinely ungraceful drops, by a few seconds, once.
static uint32_t lastWsAliveMs[MAX_PLAYERS] = {0};
static constexpr uint32_t WS_REAP_GRACE_MS = 8000;

// Called every tick from gameLoop(). Snapshots under G.mutex, then does the
// ws.client() lookups after releasing it: ws.client() takes AsyncWebSocket's
// own recursive_mutex, and taking that while holding G.mutex would introduce
// a second lock order against the AsyncTCP task.
static void refreshWsLiveness() {
  uint32_t ids[MAX_PLAYERS]  = {0};
  bool     conn[MAX_PLAYERS] = {false};
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    conn[i] = G.players[i].connected;
    ids[i]  = G.players[i].wsClientId;
  }
  xSemaphoreGive(G.mutex);

  uint32_t now = millis();
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (conn[i] && ids[i] && ws.client(ids[i])) lastWsAliveMs[i] = now;
  }
}

static void handleConnect(AsyncWebSocketClient* client) {
  Log.notice("WS CONNECT id=%u ip=%s",
             (unsigned)client->id(), client->remoteIP().toString().c_str());

  // Idle-ping keepalive (defense-in-depth alongside AsyncClient's own 5s ack
  // timeout) and: don't force-close a client whose send queue fills —
  // broadcastState() re-sends full state every 100ms, so a superseded queued
  // message is safe to drop; closing the connection over it is not.
  // The queue is capped at 8 (WS_MAX_QUEUED_MESSAGES in build_opt.h, down
  // from the library's 32): each queued tick is a ~3-4 KB buffer, and under
  // 4 KB malloc stays on the internal heap, so one stalled client could pin
  // ~100 KB of it -- enough to starve LWIP (docs/dev-loop.md "HTTP wedge").
  client->keepAlivePeriod(15);
  client->setCloseClientOnQueueFull(false);

  // Cleanup stale connections: p.connected=true but WS client is gone (ungraceful close)
  {
    bool freedAny = false;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      uint32_t nowMs = millis();
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& p = G.players[i];
        if (!p.connected || ws.client(p.wsClientId)) continue;
        // Invisible right now — but was it invisible long enough to be real?
        // See WS_REAP_GRACE_MS above: reaping on one bad look is what let a
        // connecting client silently unseat a live player.
        uint32_t seen = lastWsAliveMs[i];
        if (seen != 0 && (nowMs - seen) < WS_REAP_GRACE_MS) {
          LOG_VERBOSE("Reap skipped slot=%d wsId=%u — seen %lums ago (grace %lums)",
                      i, (unsigned)p.wsClientId,
                      (unsigned long)(nowMs - seen),
                      (unsigned long)WS_REAP_GRACE_MS);
          continue;
        }
        Log.warning("Reaped stale player slot=%d oldWsId=%u (ungraceful close, "
                    "unseen for %lums)", i, (unsigned)p.wsClientId,
                    (unsigned long)(seen ? nowMs - seen : 0));
        p.connected  = false;
        p.wsClientId = 0;
        lastWsAliveMs[i] = 0;
        if (G.connectedCount > 0) G.connectedCount--;
        freedAny = true;
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
  // Prune lobby entries whose socket is gone before counting them.
  //
  // lobbyIds is only cleared by handleDisconnect (by id) and by a successful
  // pick, but TWO paths *add* an id back to it: EVT_DOWNED moves a downed
  // player's client to the lobby, and handleMsg_pick's evict path does the
  // same to whoever it displaced. If either client's socket is already gone
  // -- which is the normal case, since ungraceful closes are exactly what
  // produces downed/evicted players -- handleDisconnect has already run and
  // found nothing to clear, so that id sits in lobbyIds forever.
  //
  // Each leaked entry permanently costs one seat, because the capacity gate
  // below is `connectedCount + lobbySize >= MAX_PLAYERS`. Measured on a
  // freshly rebooted board: a 5-bot arena produced 92 "full" rejections in
  // 180s with only 3-4 bots ever seated, and the locked-out client's retry
  // storm then drove 9 seat losses among the ones that had got in. That is
  // the "ceiling: 5 bots" in docs/bot-testing.md -- it was never a real
  // ceiling, just this leak.
  //
  // ws.client() takes AsyncWebSocket's own lock, so it must not be called
  // inside the critical section: snapshot, look up, then clear. Same pattern
  // as the stale-slot reap above.
  uint32_t lobbySnapshot[MAX_PLAYERS];
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) lobbySnapshot[i] = lobbyIds[i];
  taskEXIT_CRITICAL(&evtMux);

  bool lobbyPruned = false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!lobbySnapshot[i] || lobbySnapshot[i] == client->id()) continue;
    if (ws.client(lobbySnapshot[i])) continue;          // still a live socket
    taskENTER_CRITICAL(&evtMux);
    if (lobbyIds[i] == lobbySnapshot[i]) { lobbyIds[i] = 0; lobbyPruned = true; }
    taskEXIT_CRITICAL(&evtMux);
    Log.warning("Lobby prune: slot=%d staleWsId=%u (socket gone)",
                i, (unsigned)lobbySnapshot[i]);
  }

  int lobbySize = 0;
  taskENTER_CRITICAL(&evtMux);
  for (int i = 0; i < MAX_PLAYERS; i++) if (lobbyIds[i]) lobbySize++;
  taskEXIT_CRITICAL(&evtMux);
  if (lobbyPruned) broadcastLobbyUpdate();

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
    LOG_VERBOSE("WS echo wifi creds to id=%u ssid=%s", (unsigned)client->id(), savedSsid);
    char credBuf[160];
    int credLen = snprintf(credBuf, sizeof(credBuf),
      "{\"t\":\"wifi\",\"status\":\"saved\",\"ssid\":\"%s\",\"pass\":\"%s\"}",
      savedSsid, savedPass);
    client->text(credBuf, (size_t)credLen);
  }
  {
    char netBuf[700];
    int netLen = wifiStoreNetsJson(netBuf, sizeof(netBuf), savedSsid);
    client->text(netBuf, (size_t)netLen);
  }
  // ── Link mode ──────────────────────────────────────────────────────────────
  // One radio serves both interfaces. A client that came in over our own softAP
  // shares airtime with beaconing, DHCP, association management and the roaming
  // sweep (wifiAutoJoinTask), on top of the 10 Hz broadcastState() feed - which
  // players feel as stutter and reasonably blame on the game. Say it first, in
  // the game's own voice: see showUplinkWarning() in data/ui-utils.js.
  {
    IPAddress rip = client->remoteIP(), apip = WiFi.softAPIP();
    bool viaAp = (rip[0] == apip[0] && rip[1] == apip[1] && rip[2] == apip[2]);
    // If we ALSO hold a station link, hand over that address - "go join the
    // real network instead" is only actionable if we say where to go.
    String staIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
    Log.notice("WS link id=%u via=%s staIp=%s", (unsigned)client->id(),
               viaAp ? "softAP" : "STA", staIp.length() ? staIp.c_str() : "-");
    char lb[128];
    int llen = snprintf(lb, sizeof(lb),
      "{\"t\":\"wifi\",\"status\":\"link\",\"ap\":%d,\"cap\":%d,\"ip\":\"%s\"}",
      viaAp ? 1 : 0, AP_MAX_CLIENTS, staIp.c_str());
    client->text(lb, (size_t)llen);
  }

  LOG_VERBOSE("WS send lobby msg id=%u", (unsigned)client->id());
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
      // Clear active encounter on disconnect.  Involuntary, so the POI goes
      // back on the hex; unbanked loot is forfeit.
      if (encounters[slot].active) {
        Log.warning("Encounter ended by disconnect slot=%d q=%u r=%u",
                    slot, encounters[slot].hexQ, encounters[slot].hexR);
        endEncounter(slot, ENC_END_DISCONNECT, /*restorePoi=*/true);
      }
      // Kill any outstanding offer this slot made. Slots are reused by
      // archetype (see handleMsg_pick) — without this, a stale offer could
      // still be armed once a new, unrelated player inherits the slot.
      tradeOffers[slot].active = false;
      lastCaravanHex[slot].q = -1; lastCaravanHex[slot].r = -1;  // same reuse hazard as tradeOffers above
      { GameEvent ev = {}; ev.type = EVT_LEFT; ev.pid = (uint8_t)slot; enqEvt(ev);
        LOG_VERBOSE("Enq EVT_LEFT pid=%d", slot); }
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
    LOG_VERBOSE("Switching mode -> WIFI_AP_STA");
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
      LOG_VERBOSE("STA status=%d elapsed=%lums", (int)cur, (unsigned long)(millis() - t0));
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
    // was called above — but that slot holds exactly one network, so also add
    // it to the roaming list that survives moving to another location.
    strlcpy(savedSsid, ctx->ssid, sizeof(savedSsid));
    strlcpy(savedPass, ctx->pass, sizeof(savedPass));
    wifiStoreRemember(ctx->ssid, ctx->pass);
    wifiSweepBackoff = WIFI_SWEEP_MIN;
  } else {
    Log.warning("STA FAIL ssid=%s elapsed=%lums reverting to AP-only",
                ctx->ssid, (unsigned long)(millis() - t0));
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, nullptr, 1, 0, AP_MAX_CLIENTS);
    // Clear in-memory SSID so handleConnect won't send a 'saved' message that
    // would suppress the client's auto-send retry (NVS copy is kept for next boot).
    savedSsid[0] = '\0';
    blen = snprintf(buf, sizeof(buf), "{\"t\":\"wifi\",\"status\":\"fail\"}");
  }

  LOG_VERBOSE("WS broadcast wifi result: %s", buf);
  ws.textAll(buf, (size_t)blen);
  broadcastWifiNets();
  free(ctx);
  wifiConnecting = false;
  Log.notice("STA task exit");
  vTaskDelete(NULL);
}

// ── Known-network auto-join sweep (Core 0) ───────────────────────────────────
// What makes the board portable: enter a network's password once, and on any
// later visit it finds that network on its own. Scans the air, ranks every
// known SSID that answered by RSSI, and tries them strongest-first while the
// softAP stays up so players never lose the game.
static void wifiAutoJoinTask(void* param) {
  (void)param;
  Log.notice("AutoJoin sweep start known=%d", (int)g_knownCount);

  if (WiFi.getMode() != WIFI_MODE_APSTA) {
    LOG_VERBOSE("AutoJoin switching mode -> WIFI_AP_STA");
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  uint32_t scanT0 = millis();
  int found = WiFi.scanNetworks(false /*async*/, false /*showHidden*/);
  Log.notice("AutoJoin scan found=%d elapsed=%lums", found,
             (unsigned long)(millis() - scanT0));

  // Rank the known networks that actually answered, strongest signal first.
  int cand[WIFI_MAX_NETS], candRssi[WIFI_MAX_NETS], nCand = 0;
  for (int i = 0; i < found; i++) {
    int k = wifiStoreFind(WiFi.SSID(i).c_str());
    if (k < 0) continue;
    int rssi = (int)WiFi.RSSI(i);
    int dup = -1;
    for (int j = 0; j < nCand; j++) if (cand[j] == k) { dup = j; break; }
    if (dup >= 0) { if (rssi > candRssi[dup]) candRssi[dup] = rssi; continue; }
    if (nCand >= WIFI_MAX_NETS) continue;
    cand[nCand] = k; candRssi[nCand] = rssi; nCand++;
  }
  WiFi.scanDelete();
  for (int i = 1; i < nCand; i++) {
    int ck = cand[i], cr = candRssi[i], j = i - 1;
    while (j >= 0 && candRssi[j] < cr) { cand[j+1] = cand[j]; candRssi[j+1] = candRssi[j]; j--; }
    cand[j+1] = ck; candRssi[j+1] = cr;
  }
  Log.notice("AutoJoin candidates=%d", nCand);

  bool ok = false;
  char joinedSsid[33] = {0}, joinedPass[65] = {0};
  for (int c = 0; c < nCand && !ok; c++) {
    strlcpy(joinedSsid, g_knownNets[cand[c]].ssid, sizeof(joinedSsid));
    strlcpy(joinedPass, g_knownNets[cand[c]].pass, sizeof(joinedPass));
    Log.notice("AutoJoin try ssid=%s rssi=%d", joinedSsid, candRssi[c]);
    WiFi.begin(joinedSsid, joinedPass[0] ? joinedPass : nullptr);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000)
      vTaskDelay(pdMS_TO_TICKS(400));
    ok = (WiFi.status() == WL_CONNECTED);
    if (!ok) {
      Log.warning("AutoJoin ssid=%s failed status=%d", joinedSsid, (int)WiFi.status());
      WiFi.disconnect(false);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

  if (ok) {
    String ip = WiFi.localIP().toString();
    Log.notice("AutoJoin connected ssid=%s ip=%s rssi=%d", joinedSsid,
               ip.c_str(), (int)WiFi.RSSI());
    strlcpy(savedSsid, joinedSsid, sizeof(savedSsid));
    strlcpy(savedPass, joinedPass, sizeof(savedPass));
    wifiStoreRemember(joinedSsid, joinedPass);   // freshest network to the front
    wifiSweepBackoff = WIFI_SWEEP_MIN;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    char buf[88];
    int blen = snprintf(buf, sizeof(buf),
      "{\"t\":\"wifi\",\"status\":\"ok\",\"ip\":\"%s\"}", ip.c_str());
    ws.textAll(buf, (size_t)blen);
    broadcastWifiNets();
  } else {
    // Nothing known is in range. Stay in AP+STA (the softAP never dropped) and
    // try again later, less and less often.
    WiFi.disconnect(false);
    savedSsid[0] = '\0';
    if (wifiSweepBackoff < WIFI_SWEEP_MAX) {
      wifiSweepBackoff *= 2;
      if (wifiSweepBackoff > WIFI_SWEEP_MAX) wifiSweepBackoff = WIFI_SWEEP_MAX;
    }
    wifiNextSweepMs = millis() + wifiSweepBackoff;
    Log.notice("AutoJoin no known network in range, next sweep in %lus",
               (unsigned long)(wifiSweepBackoff / 1000));
  }

  wifiConnecting = false;
  Log.notice("AutoJoin sweep exit");
  vTaskDelete(NULL);
}

// Kick a sweep if one isn't already running. Returns true if a task started.
static bool wifiStartAutoJoin() {
  if (g_knownCount == 0 || wifiConnecting || bootWifiPending) return false;
  wifiConnecting = true;
  if (xTaskCreatePinnedToCore(wifiAutoJoinTask, "wifiSweep", 6144, NULL, 1, NULL, 0) != pdPASS) {
    Log.error("AutoJoin task spawn failed");
    wifiConnecting = false;
    return false;
  }
  return true;
}
