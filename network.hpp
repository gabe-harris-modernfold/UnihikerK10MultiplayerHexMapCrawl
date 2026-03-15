#pragma once
// ── Networking: sync, broadcast, event drain, WS handlers ────────────────────
// Included from Esp32HexMapCrawl.ino after game_logic.hpp.
// Has access to all globals, constants, and game_logic functions.

// ── Lobby: pending clients waiting to pick an archetype ──────────────────────
// Each slot holds a WS client ID (0 = empty). Guarded by evtMux spinlock.
static uint32_t lobbyIds[MAX_PLAYERS] = {0};

// ── Skill check broadcast ─────────────────────────────────────────────────────
static void broadcastCheck(int pid, uint8_t skill, CheckResult& r) {
  char buf[128]; int len;
  len = snprintf(buf, sizeof(buf),
    "{\"t\":\"ev\",\"k\":\"chk\",\"pid\":%d,\"sk\":%d,\"dn\":%d,"
    "\"r1\":%d,\"r2\":%d,\"sv\":%d,\"mod\":%d,\"tot\":%d,\"suc\":%d}",
    pid, (int)skill, r.dn,
    r.r1, r.r2, r.skillVal, r.mods, r.total, r.success ? 1 : 0);
  ws.textAll(buf, (size_t)len);
}

// ── Build lobby message for one client ───────────────────────────────────────
// {"t":"lobby","avail":[0,1,2,4,5]}  — indices of unconnected archetype slots
static void sendLobbyMsg(AsyncWebSocketClient* client) {
  char buf[72]; int pos;
  pos = snprintf(buf, sizeof(buf), "{\"t\":\"lobby\",\"avail\":[");
  bool first = true;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!G.players[i].connected) {
        if (!first) buf[pos++] = ',';
        buf[pos++] = '0' + i;
        first = false;
      }
    }
    xSemaphoreGive(G.mutex);
  }
  int len = snprintf(buf + pos, sizeof(buf) - pos, "]}") + pos;
  client->text(buf, len);
}

// ── Broadcast updated lobby to all lobby clients ─────────────────────────────
// Called after a pick succeeds, so remaining lobby clients update their UI.
static void broadcastLobbyUpdate() {
  char buf[72]; int pos;
  pos = snprintf(buf, sizeof(buf), "{\"t\":\"lobby\",\"avail\":[");
  bool first = true;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!G.players[i].connected) {
        if (!first) buf[pos++] = ',';
        buf[pos++] = '0' + i;
        first = false;
      }
    }
    xSemaphoreGive(G.mutex);
  }
  int len = snprintf(buf + pos, sizeof(buf) - pos, "]}") + pos;

  // Snapshot IDs outside the spinlock, then send
  uint32_t snapIds[MAX_PLAYERS] = {0};
  taskENTER_CRITICAL(&evtMux);
  memcpy(snapIds, lobbyIds, sizeof(snapIds));
  taskEXIT_CRITICAL(&evtMux);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!snapIds[i]) continue;
    AsyncWebSocketClient* cl = ws.client(snapIds[i]);
    if (cl) cl->text(buf, len);
  }
}

// ── Sync message (unicast to one client on connect) ──────────────────────────
// Buffer: map=475×4=1900 + header~55 + players~1200 + footer = ~3160 chars
static void sendSync(AsyncWebSocketClient* client, int pid) {
  static char buf[5500];  // expanded for full Wayfarer payload
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

  Player& me = G.players[pid];
  int visR; bool maskRes;
  playerVisParams(pid, &visR, &maskRes);

  int pos = snprintf(buf, sizeof(buf),
    "{\"t\":\"sync\",\"id\":%d,\"tk\":%lu,\"vr\":%d,\"map\":\"",
    pid, (unsigned long)G.tickId, visR);
  int mapStart = pos;
  pos += encodeMapFog(buf + pos, (int)sizeof(buf) - pos, me.q, me.r, visR, maskRes);
  int mapLen = pos - mapStart;
  pos += snprintf(buf + pos, sizeof(buf) - pos, "\",\"p\":[");

  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (i) buf[pos++] = ',';
    // Basic / legacy fields
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"id\":%d,\"on\":%d,\"q\":%d,\"r\":%d,\"sc\":%d,\"nm\":\"%s\","
      "\"inv\":[%d,%d,%d,%d,%d],\"st\":%d,\"sp\":%d,",
      i, p.connected ? 1 : 0, p.q, p.r, p.score, p.name,
      p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4],
      p.stamina, p.steps);
    // Wayfarer vitals
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"ll\":%d,\"food\":%d,\"water\":%d,\"fat\":%d,\"rad\":%d,\"res\":%d,"
      "\"arch\":%d,\"sb\":%d,\"is\":%d,\"fth\":%d,\"wth\":%d,\"mp\":%d,\"au\":%d,\"rt\":%d,",
      p.ll, p.food, p.water, p.fatigue, p.radiation, p.resolve,
      p.archetype, p.statusBits, p.invSlots,
      (int)p.fThreshBelow, (int)p.wThreshBelow, (int)p.movesLeft,
      p.actUsed ? 1 : 0, p.resting ? 1 : 0);
    // Skills array
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"sk\":[%d,%d,%d,%d,%d,%d],",
      p.skills[0], p.skills[1], p.skills[2], p.skills[3], p.skills[4], p.skills[5]);
    // Wounds array
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"wd\":[%d,%d,%d],",
      p.wounds[0], p.wounds[1], p.wounds[2]);
    // Inventory grid
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
      "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
      p.invType[0],  p.invType[1],  p.invType[2],  p.invType[3],
      p.invType[4],  p.invType[5],  p.invType[6],  p.invType[7],
      p.invType[8],  p.invType[9],  p.invType[10], p.invType[11],
      p.invQty[0],   p.invQty[1],   p.invQty[2],   p.invQty[3],
      p.invQty[4],   p.invQty[5],   p.invQty[6],   p.invQty[7],
      p.invQty[8],   p.invQty[9],   p.invQty[10],  p.invQty[11]);
  }
  // Shared game-state object + variant counts
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "],\"gs\":{\"tc\":%d,\"dc\":%d,\"sf\":%d,\"sw\":%d},"
    "\"vc\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
    G.threatClock, G.dayCount, G.sharedFood, G.sharedWater,
    terrainVariantCount[0], terrainVariantCount[1], terrainVariantCount[2],
    terrainVariantCount[3], terrainVariantCount[4], terrainVariantCount[5],
    terrainVariantCount[6], terrainVariantCount[7], terrainVariantCount[8],
    terrainVariantCount[9], terrainVariantCount[10]);
  xSemaphoreGive(G.mutex);

  Serial.printf("[SYNC]    →P%d | visR:%d mask:%c | mapStr:%dB | total:%d/5500B\n",
    pid, visR, maskRes ? 'Y' : 'N', mapLen, pos);

  client->text(buf, (size_t)pos);
}

// ── Periodic state broadcast (all clients) ───────────────────────────────────
static void broadcastState() {
  static char buf[1500];  // 6 players × ~175 chars + header/footer ~80
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

  int pos = snprintf(buf, sizeof(buf),
    "{\"t\":\"s\",\"tk\":%lu,\"p\":[", (unsigned long)G.tickId);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (i) buf[pos++] = ',';
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"q\":%d,\"r\":%d,\"sc\":%d,\"inv\":[%d,%d,%d,%d,%d],\"on\":%d,\"sp\":%d,"
      "\"ll\":%d,\"food\":%d,\"water\":%d,\"fat\":%d,\"rad\":%d,\"res\":%d,"
      "\"sb\":%d,\"mp\":%d,\"fth\":%d,\"wth\":%d,\"au\":%d}",
      p.q, p.r, p.score,
      p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4],
      p.connected ? 1 : 0, p.steps,
      p.ll, p.food, p.water, p.fatigue, p.radiation, p.resolve,
      p.statusBits, (int)p.movesLeft, (int)p.fThreshBelow, (int)p.wThreshBelow,
      p.actUsed ? 1 : 0);
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "],\"gs\":{\"tc\":%d,\"dc\":%d,\"sf\":%d,\"sw\":%d}}",
    G.threatClock, G.dayCount, G.sharedFood, G.sharedWater);
  xSemaphoreGive(G.mutex);
  ws.textAll(buf, (size_t)pos);
}

// ── SD Save / Load ───────────────────────────────────────────────────────────
void saveGame() {
  if (SD.cardType() != CARD_NONE && xSemaphoreTake(G.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (!SD.exists(SAVE_DIR)) SD.mkdir(SAVE_DIR);
    // Map file
    File f = SD.open(SAVE_MAP_F, FILE_WRITE);
    if (f) {
      SaveHeader hdr = { SAVE_MAGIC, SAVE_VERSION, G.dayCount,
                         G.threatClock, G.sharedFood, G.sharedWater, 0 };
      f.write((uint8_t*)&hdr, sizeof(hdr));
      f.write((uint8_t*)G.map, sizeof(G.map));
      f.close();
    }
    // Players file
    File p = SD.open(SAVE_PLY_F, FILE_WRITE);
    if (p) {
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& pl = G.players[i];
        SavePlayer sp = {};
        memcpy(sp.name, pl.name, 12);
        sp.archetype = pl.archetype;
        memcpy(sp.skills, pl.skills, 6);
        sp.q = pl.q; sp.r = pl.r;
        sp.ll = pl.ll; sp.food = pl.food; sp.water = pl.water;
        sp.fatigue = pl.fatigue; sp.radiation = pl.radiation; sp.resolve = pl.resolve;
        memcpy(sp.inv, pl.inv, 5);
        memcpy(sp.invType, pl.invType, 12);
        memcpy(sp.invQty, pl.invQty, 12);
        sp.invSlots = pl.invSlots;
        memcpy(sp.wounds, pl.wounds, 3);
        sp.statusBits = pl.statusBits & (uint8_t)~ST_DOWNED;
        sp.score = pl.score; sp.steps = pl.steps;
        memcpy(sp.surveyedMap, pl.surveyedMap, sizeof(sp.surveyedMap));
        sp.used = (pl.name[0] != '\0') ? 1 : 0;
        p.write((uint8_t*)&sp, sizeof(sp));
      }
      p.close();
    }
    xSemaphoreGive(G.mutex);
    Serial.printf("[SAVE] Day %d saved to SD\n", (int)G.dayCount);
  }
}

bool tryLoadSave() {
  if (!SD.exists(SAVE_MAP_F)) return false;
  File f = SD.open(SAVE_MAP_F, FILE_READ);
  if (!f) return false;
  SaveHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }
  if (hdr.magic != SAVE_MAGIC || hdr.version != SAVE_VERSION) { f.close(); return false; }
  if (f.read((uint8_t*)G.map, sizeof(G.map)) != sizeof(G.map)) { f.close(); return false; }
  f.close();
  G.dayCount    = hdr.dayCount;
  G.threatClock = hdr.threatClock;
  G.sharedFood  = hdr.sharedFood;
  G.sharedWater = hdr.sharedWater;
  File p = SD.open(SAVE_PLY_F, FILE_READ);
  if (p) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      SavePlayer sp;
      if (p.read((uint8_t*)&sp, sizeof(sp)) != sizeof(sp)) break;
      if (!sp.used) continue;
      Player& pl = G.players[i];
      memcpy(pl.name, sp.name, 12);
      pl.archetype = sp.archetype;
      memcpy(pl.skills, sp.skills, 6);
      pl.q = sp.q; pl.r = sp.r;
      pl.ll = sp.ll; pl.food = sp.food; pl.water = sp.water;
      pl.fatigue = sp.fatigue; pl.radiation = sp.radiation; pl.resolve = sp.resolve;
      memcpy(pl.inv, sp.inv, 5);
      memcpy(pl.invType, sp.invType, 12);
      memcpy(pl.invQty, sp.invQty, 12);
      pl.invSlots = sp.invSlots;
      memcpy(pl.wounds, sp.wounds, 3);
      pl.statusBits = sp.statusBits;
      pl.score = sp.score; pl.steps = sp.steps;
      memcpy(pl.surveyedMap, sp.surveyedMap, sizeof(sp.surveyedMap));
      pl.connected = false; pl.wsClientId = 0;
    }
    p.close();
  }
  Serial.printf("[SAVE] Loaded Day %d from SD\n", (int)G.dayCount);
  return true;
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

      case EVT_JOINED:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"join\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        break;

      case EVT_LEFT:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"left\",\"pid\":%d}", ev.pid);
        ws.textAll(buf, len);
        break;

      case EVT_DAWN:
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
        break;

      case EVT_DUSK:
        len = snprintf(buf, sizeof(buf),
          "{\"t\":\"ev\",\"k\":\"dusk\",\"pid\":%d,\"out\":%d,"
          "\"dn\":%d,\"tot\":%d,\"ll\":%d,\"lld\":%d,\"rad\":%d}",
          ev.pid, (int)ev.actOut,
          (int)ev.actDn, (int)ev.actTot,
          (int)ev.actNewLL, (int)ev.actLLD, (int)ev.radR);
        ws.textAll(buf, len);
        break;

      case EVT_ACTION:
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

      case EVT_NAME: break;   // broadcast is handled inline in handleMessage ("nm" event)
    }
  }
  // pendingCount was already reset to 0 inside the spinlock snapshot above.
}

// ── WebSocket connect handler ─────────────────────────────────────────────────
// Adds the client to the lobby and sends available archetypes for selection.
// Does NOT assign a slot — that happens when the client sends a "pick" message.
static void handleConnect(AsyncWebSocketClient* client) {
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
    if (arch < 0 || arch >= MAX_PLAYERS) return;

    // Verify client is in the lobby
    bool inLobby = false;
    taskENTER_CRITICAL(&evtMux);
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (lobbyIds[i] == client->id()) { inLobby = true; break; }
    }
    taskEXIT_CRITICAL(&evtMux);
    if (!inLobby) return;

    bool assigned = false;
    struct { int16_t q, r; uint8_t terrain; int8_t visLvl; int visR, attempts, connCount;
             uint8_t perc, str; char name[12]; } snap = {};

    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      Player& p = G.players[arch];
      if (!p.connected) {
        p.connected  = true;
        p.wsClientId = client->id();
        p.connectMs  = millis();

        // Spawn on passable terrain
        int attempts = 0;
        do {
          p.q = (int16_t)(esp_random() % MAP_COLS);
          p.r = (int16_t)(esp_random() % MAP_ROWS);
          attempts++;
        } while (TERRAIN_MC[G.map[p.r][p.q].terrain] == 255 && attempts < 50);
        p.lastMoveMs = 0;
        p.score = 0; p.steps = 0;
        memset(p.inv, 0, sizeof(p.inv));
        snprintf(p.name, sizeof(p.name), "%s", ARCHETYPE_NAME[arch]);

        // Wayfarer initialisation
        p.archetype    = (uint8_t)arch;
        p.ll           = 6;
        p.food         = 6;
        p.water        = 6;
        p.fatigue      = 0;
        p.radiation    = 0;
        p.resolve      = 3;
        p.statusBits   = 0;
        p.invSlots     = ARCHETYPE_INV_SLOTS[arch];
        memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);
        memset(p.wounds,  0, sizeof(p.wounds));
        memset(p.invType, 0, sizeof(p.invType));
        memset(p.invQty,  0, sizeof(p.invQty));
        p.chkSk = 0; p.chkDn = 7; p.chkBonus = 0;
        p.fThreshBelow = 0; p.wThreshBelow = 0;
        p.movesLeft    = (int8_t)effectiveMP(arch);
        p.actUsed      = false;
        p.encPenApplied = false;
        p.radClean     = true;

        p.stamina    = 100;
        uint32_t rnd = esp_random();
        p.perception = 1 + (uint8_t)(rnd % 3);
        p.strength   = 1 + (uint8_t)((rnd >> 8) % 3);
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
        snap.perc        = p.perception; snap.str = p.strength;
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
      Serial.printf("[CONNECT]   name:\"%s\" | perc:%d str:%d | players now:%d/%d\n",
        snap.name, snap.perc, snap.str, snap.connCount, MAX_PLAYERS);

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
        Player& p    = G.players[slot];
        p.chkSk      = (uint8_t)sk;
        p.chkDn      = (uint8_t)dn;
        p.chkBonus   = (uint8_t)bonus;
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
        pl.ll = 6; pl.food = 4; pl.water = 4;
        pl.fatigue = 0; pl.radiation = 0; pl.resolve = 3;
        pl.actUsed = false; pl.resting = false;
        pl.movesLeft = (int8_t)effectiveMP(i);
      }
      xSemaphoreGive(G.mutex);
    }
    { GameEvent ev = {}; ev.type = EVT_REGEN; enqEvt(ev); }
    Serial.println("[REGEN]   Map regenerated — all players scattered");

  } else if (strncmp(tv, "act", (size_t)(te - tv)) == 0) {
    // Action: {"t":"act","a":N[,"mp":N,"cnd":N]}
    const char* ap = strstr(data, "\"a\""); if (!ap) return;
    const char* av = strchr(ap + 3, ':');   if (!av) return;
    int actType = atoi(av + 1);
    if (actType < 0 || actType > 7) return;

    int mpParam = 1;
    const char* mpp = strstr(data, "\"mp\"");
    if (mpp) { const char* mpv = strchr(mpp + 4, ':'); if (mpv) mpParam = atoi(mpv + 1); }

    int condTgt = 0;
    const char* cndp = strstr(data, "\"cnd\"");
    if (cndp) { const char* cndv = strchr(cndp + 5, ':'); if (cndv) condTgt = atoi(cndv + 1); }

    static char survBuf[1100];
    int  survLen = 0;
    int  slot    = -1;

    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      slot = findSlot(client->id());
      if (slot >= 0)
        handleAction(slot, (uint8_t)actType, mpParam, condTgt,
                     survBuf, sizeof(survBuf), &survLen);
      xSemaphoreGive(G.mutex);
    }
    if (survLen > 0)
      client->text(survBuf, (size_t)survLen);
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
