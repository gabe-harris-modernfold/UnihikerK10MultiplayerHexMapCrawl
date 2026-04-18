#pragma once
// ── Sync and broadcast: outbound state serialization ─────────────────────────
// Included from Esp32HexMapCrawl.ino after network-persistence.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

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
  static char buf[6400];  // expanded: +equip[5] per player + ground items list
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
      "\"inv\":[%d,%d,%d,%d,%d],\"sp\":%d,",
      i, p.connected ? 1 : 0, p.q, p.r, p.score, p.name,
      p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4],
      p.steps);
    // Survivor vitals
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
    // Inventory grid + equipment slots
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
      "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
      "\"eq\":[%d,%d,%d,%d,%d],\"enc\":%d}",
      p.invType[0],  p.invType[1],  p.invType[2],  p.invType[3],
      p.invType[4],  p.invType[5],  p.invType[6],  p.invType[7],
      p.invType[8],  p.invType[9],  p.invType[10], p.invType[11],
      p.invQty[0],   p.invQty[1],   p.invQty[2],   p.invQty[3],
      p.invQty[4],   p.invQty[5],   p.invQty[6],   p.invQty[7],
      p.invQty[8],   p.invQty[9],   p.invQty[10],  p.invQty[11],
      p.equip[0], p.equip[1], p.equip[2], p.equip[3], p.equip[4],
      encounters[i].active ? 1 : 0);
  }
  // Ground items visible to this player
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"gi\":[");
  bool firstGi = true;
  for (int g = 0; g < MAX_GROUND; g++) {
    if (!groundItems[g].itemType) continue;
    if (!firstGi) buf[pos++] = ',';
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"g\":%d,\"q\":%d,\"r\":%d,\"id\":%d,\"n\":%d}",
      g, groundItems[g].q, groundItems[g].r,
      groundItems[g].itemType, groundItems[g].qty);
    firstGi = false;
  }
  // Shared game-state object + variant counts
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "],\"gs\":{\"tc\":%d,\"dc\":%d,\"wp\":%d},"
    "\"vc\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
    "\"sv\":[%d,%d],"
    "\"fa\":%d}",
    G.threatClock, G.dayCount, (int)G.weatherPhase,
    terrainVariantCount[0],  terrainVariantCount[1],  terrainVariantCount[2],
    terrainVariantCount[3],  terrainVariantCount[4],  terrainVariantCount[5],
    terrainVariantCount[6],  terrainVariantCount[7],  terrainVariantCount[8],
    terrainVariantCount[9],  terrainVariantCount[10], terrainVariantCount[11],
    shelterVariantCount[0], shelterVariantCount[1],
    forrageAnimalCount);
  xSemaphoreGive(G.mutex);

  Serial.printf("[SYNC]    →P%d | visR:%d mask:%c | mapStr:%dB | total:%d/6400B\n",
    pid, visR, maskRes ? 'Y' : 'N', mapLen, pos);

  client->text(buf, (size_t)pos);
}

// ── Periodic state broadcast (all clients) ───────────────────────────────────
// Buffer: 6 players × ~310 chars + header/footer ~80 = ~1940; sized at 2500
// to safely accommodate it[12]+iq[12]+eq[5] per player (~125 chars × 6 = 750).
static void broadcastState() {
  static char buf[2500];
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

  int pos = snprintf(buf, sizeof(buf),
    "{\"t\":\"s\",\"tk\":%lu,\"p\":[", (unsigned long)G.tickId);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (i) buf[pos++] = ',';
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"q\":%d,\"r\":%d,\"sc\":%d,\"inv\":[%d,%d,%d,%d,%d],\"on\":%d,\"sp\":%d,"
      "\"ll\":%d,\"food\":%d,\"water\":%d,\"fat\":%d,\"rad\":%d,\"res\":%d,"
      "\"sb\":%d,\"mp\":%d,\"fth\":%d,\"wth\":%d,\"au\":%d,\"vm\":%d,"
      "\"wd\":[%d,%d,%d],"
      "\"it\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
      "\"iq\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
      "\"eq\":[%d,%d,%d,%d,%d],\"enc\":%d}",
      p.q, p.r, p.score,
      p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4],
      p.connected ? 1 : 0, p.steps,
      p.ll, p.food, p.water, p.fatigue, p.radiation, p.resolve,
      p.statusBits, (int)p.movesLeft, (int)p.fThreshBelow, (int)p.wThreshBelow,
      p.actUsed ? 1 : 0, (int)computeValidMoves(i),
      p.wounds[0], p.wounds[1], p.wounds[2],
      p.invType[0],  p.invType[1],  p.invType[2],  p.invType[3],
      p.invType[4],  p.invType[5],  p.invType[6],  p.invType[7],
      p.invType[8],  p.invType[9],  p.invType[10], p.invType[11],
      p.invQty[0],   p.invQty[1],   p.invQty[2],   p.invQty[3],
      p.invQty[4],   p.invQty[5],   p.invQty[6],   p.invQty[7],
      p.invQty[8],   p.invQty[9],   p.invQty[10],  p.invQty[11],
      p.equip[0], p.equip[1], p.equip[2], p.equip[3], p.equip[4],
      encounters[i].active ? 1 : 0);
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "],\"gs\":{\"tc\":%d,\"dc\":%d,\"wp\":%d}}",
    G.threatClock, G.dayCount, (int)G.weatherPhase);
  xSemaphoreGive(G.mutex);
  ws.textAll(buf, (size_t)pos);
}
