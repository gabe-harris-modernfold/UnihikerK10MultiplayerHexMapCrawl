#pragma once
// ── Sync and broadcast: outbound state serialization ─────────────────────────
// Included from Esp32HexMapCrawl.ino after network-persistence.hpp.
// Has access to all globals, constants, structs, and functions defined above it.

// ── Lobby: pending clients waiting to pick an archetype ──────────────────────
// Each slot holds a WS client ID (0 = empty). Guarded by evtMux spinlock.
static uint32_t lobbyIds[MAX_PLAYERS] = {0};

// ── broadcastState() telemetry, surfaced via /state (game-server.hpp) ────────
static uint32_t g_broadcastSkips       = 0;  // total ticks skipped: G.mutex busy
static uint32_t g_broadcastSkipsConsec = 0;  // current consecutive-skip streak
static uint32_t g_broadcastPartial     = 0;  // ticks where >=1 client missed the send (queue full)

// ── Skill check broadcast ─────────────────────────────────────────────────────
static void broadcastCheck(int pid, uint8_t skill, CheckResult& r) {
  Log.notice("CHECK pid=%d skill=%d dn=%d r1=%d r2=%d sv=%d mod=%d tot=%d suc=%d",
             pid, (int)skill, (int)r.dn, (int)r.r1, (int)r.r2,
             (int)r.skillVal, (int)r.mods, (int)r.total, r.success ? 1 : 0);
  char buf[128]; int len;
  len = snprintf(buf, sizeof(buf),
    "{\"t\":\"ev\",\"k\":\"chk\",\"pid\":%d,\"sk\":%d,\"dn\":%d,"
    "\"r1\":%d,\"r2\":%d,\"sv\":%d,\"mod\":%d,\"tot\":%d,\"suc\":%d}",
    pid, (int)skill, r.dn,
    r.r1, r.r2, r.skillVal, r.mods, r.total, r.success ? 1 : 0);
  ws.textAll(buf, (size_t)len);
}

// ── Build lobby message for one client ───────────────────────────────────────
// ── Variant counts ───────────────────────────────────────────────────────────
// Emits the bare "N,N,..." body of the "vc" array, one entry per terrain.
// Built as a loop rather than a fixed %d list so widening NUM_TERRAIN (the
// bunker tunnel terrains took it 12 -> 16) can't silently truncate the array
// and leave the client's loadTerrainVariants() short.  Returns chars written.
static int appendVariantCounts(char* buf, size_t cap) {
  int pos = 0;
  for (int t = 0; t < NUM_TERRAIN && pos < (int)cap; t++)
    pos += snprintf(buf + pos, cap - pos, t ? ",%d" : "%d", terrainVariantCount[t]);
  return pos;
}

// {"t":"lobby","avail":[0,1,2,4,5]}  — indices of unconnected archetype slots
static void sendLobbyMsg(AsyncWebSocketClient* client) {
  LOG_VERBOSE("Lobby unicast id=%u", (unsigned)client->id());
  char buf[224]; int pos;   // was 72 — now also carries vc/sv/fa (NUM_TERRAIN entries), worst case ~130B
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
  // Variant counts are static for the boot session (set once by
  // setupVariantCounts() in setup()) — send them on first connect so the
  // client can start preloading hex/shelter/forage-animal art immediately,
  // instead of waiting for sync (which only arrives after picking a
  // character, by which point the boot loading screen has already closed).
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"vc\":[");
  pos += appendVariantCounts(buf + pos, sizeof(buf) - pos);
  int len = snprintf(buf + pos, sizeof(buf) - pos,
    "],\"sv\":[%d,%d],\"fa\":%d}",
    shelterVariantCount[0], shelterVariantCount[1],
    forrageAnimalCount) + pos;
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

  int recipients = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!snapIds[i]) continue;
    AsyncWebSocketClient* cl = ws.client(snapIds[i]);
    if (cl) { cl->text(buf, len); recipients++; }
  }
  LOG_VERBOSE("Lobby broadcast to %d clients", recipients);
}

// Sparse burning-hex list shared by broadcastState()/sendSync() below: writes
// "[q,r,intensity],[q,r,intensity],..." (no brackets/key — caller wraps it in
// "fire":[ ... ]). Returns bytes written, same convention as snprintf. At the
// FIRE_CAP of 20 this is at most ~220 bytes. Must precede both callers below —
// C++ needs the declaration before first use, unlike the forward-declare
// trick hex-map.hpp uses for fireVisionPenalty() (this file has no reverse
// dependency forcing that).
static int appendFireArray(char* buf, size_t cap) {
  int  pos = 0;
  bool first = true;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      if (W_hex[r][q].fire == 0) continue;
      if (!first) buf[pos++] = ',';
      pos += snprintf(buf + pos, cap - pos, "[%d,%d,%d]", q, r, (int)W_hex[r][q].fire);
      first = false;
    }
  }
  return pos;
}

// The caravan's consumable shelf as [[itemId, qty, pricePerUnit], ...] —
// empty slots are skipped so the client only ever sees what it can buy.
// The price rides along (items.cfg "value" via caravanPrice()) rather than
// being mirrored in game-data.js, so the server stays the only pricing
// authority. ≤ CARAVAN_STOCK_SLOTS × ~12 chars.
static int appendCaravanStock(char* buf, size_t cap) {
  int  pos = 0;
  bool first = true;
  for (int s = 0; s < CARAVAN_STOCK_SLOTS; s++) {
    uint8_t id = W.caravan.stockItem[s];
    if (!id || !W.caravan.stockQty[s]) continue;
    if (!first) buf[pos++] = ',';
    pos += snprintf(buf + pos, cap - pos, "[%d,%d,%d]",
                    (int)id, (int)W.caravan.stockQty[s], (int)caravanPrice(getItemDef(id)));
    first = false;
  }
  return pos;
}

// Sparse flooded-hex list, same shape/convention as appendFireArray() above
// (caller wraps it in "flood":[ ... ]). At FLOOD_CAP of 15 this is at most
// ~165 bytes.
static int appendFloodArray(char* buf, size_t cap) {
  int  pos = 0;
  bool first = true;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      if (W_hex[r][q].flood == 0) continue;
      if (!first) buf[pos++] = ',';
      pos += snprintf(buf + pos, cap - pos, "[%d,%d,%d]", q, r, (int)W_hex[r][q].flood);
      first = false;
    }
  }
  return pos;
}

// ── Tunnel sync (unicast) ───────────────────────────────────────────────────
// The whole fogged bunker tunnel board in one message. Sent when a player
// descends, and again from sendSync() when someone reconnects while already
// underground -- without that second case they would come back to an empty
// board with no way to redraw it.
//
// 16x10 at 6 chars/cell is 960 chars, so this fits a small stack buffer; it
// deliberately does NOT reuse sendSync()'s 40 KB PSRAM buffer.
static void sendTunnelSync(AsyncWebSocketClient* client) {
  char buf[1280];
  int pid = findSlot(client->id());
  if (pid < 0) return;
  int visR; bool maskRes;
  int pq, pr;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    Log.warning("sendTunnelSync: G.mutex timeout");
    return;
  }
  playerVisParams(pid, &visR, &maskRes);
  pq = G.players[pid].tq; pr = G.players[pid].tr;
  int pos = snprintf(buf, sizeof(buf),
    "{\"t\":\"tsync\",\"cols\":%d,\"rows\":%d,\"vr\":%d,\"q\":%d,\"r\":%d,\"map\":\"",
    (int)TUN_COLS, (int)TUN_ROWS, visR, pq, pr);
  pos += encodeTunnelFog(buf + pos, (int)sizeof(buf) - pos, pq, pr, visR, maskRes);
  xSemaphoreGive(G.mutex);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "\"}");
  client->text(buf, (size_t)pos);
  Log.notice("tsync pid=%d %dx%d at (%d,%d) vr=%d bytes=%d",
             pid, (int)TUN_COLS, (int)TUN_ROWS, pq, pr, visR, pos);
}

// ── Sync message (unicast to one client on connect) ──────────────────────────
// Buffer: map=4275×6=25650 + header~55 + players~1200 + ground items + margin
static void sendSync(AsyncWebSocketClient* client, int pid) {
  PSRAM_STATIC(char, buf, [40000]);  // 75×57 map fog encoding (6 chars/cell); PSRAM — was 40 KB of internal .bss
  // One-shot (join/regen), never on the hot 100ms tick path, so a few retries
  // here is cheap insurance against leaving a joining player with no map at
  // all — previously a single 20ms miss sent the client nothing.
  bool gotMutex = false;
  for (int attempt = 0; attempt < 3 && !gotMutex; attempt++) {
    gotMutex = (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE);
  }
  if (!gotMutex) {
    Log.error("sendSync pid=%d: G.mutex timeout after 3 attempts", pid);
    client->text("{\"t\":\"err\",\"msg\":\"Sync failed, try reconnecting\"}");
    return;
  }

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
    // Survivor vitals.  "is" and "llCap" are NOT here: appendPackArrays()
    // emits both, and they are the EFFECTIVE values (base + equipment), which
    // is what the client sizes its pack grid and LIFE LEVEL track from.
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"ll\":%d,\"food\":%d,\"water\":%d,\"rad\":%d,"
      "\"arch\":%d,\"fth\":%d,\"wth\":%d,\"mp\":%d,\"wnd\":[%d,%d],\"rt\":%d,",
      p.ll, p.food, p.water, p.radiation,
      p.archetype,
      (int)p.fThreshBelow, (int)p.wThreshBelow, (int)p.movesLeft,
      (int)p.wounds[WOUND_MINOR], (int)p.wounds[WOUND_MAJOR], p.resting ? 1 : 0);
    // Skills array
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"sk\":[%d,%d,%d,%d,%d],",
      p.skills[0], p.skills[1], p.skills[2], p.skills[3], p.skills[4]);
    // Inventory grid + equipment slots + effective pack size / LL ceiling
    pos = appendPackArrays(buf, sizeof(buf), pos, i);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      ",\"kr\":%lu,\"enc\":%d,\"dp\":%d,\"tq\":%d,\"tr\":%d}",
      (unsigned long)p.knownRecipes,
      encounters[i].active ? 1 : 0,
      (int)p.depth, (int)p.tq, (int)p.tr);
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
    "\"world\":{\"caravan\":{\"q\":%d,\"r\":%d,\"active\":%d,\"inv\":[%d,%d,%d,%d,%d],\"stock\":[",
    G.threatClock, G.dayCount, (int)G.weatherPhase,
    (int)W.caravan.q, (int)W.caravan.r, W.caravan.active ? 1 : 0,
    W.caravan.inv[0], W.caravan.inv[1], W.caravan.inv[2], W.caravan.inv[3], W.caravan.inv[4]);
  pos += appendCaravanStock(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "]},\"doom\":{\"q\":%d,\"r\":%d,\"awareness\":%d},\"fire\":[",
    (int)W.creepingDoom.q, (int)W.creepingDoom.r, (int)W.creepingDoom.awareness);
  pos += appendFireArray(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"flood\":[");
  pos += appendFloodArray(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "]},\"vc\":[");
  pos += appendVariantCounts(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "],\"sv\":[%d,%d],\"fa\":%d}",
    shelterVariantCount[0], shelterVariantCount[1],
    forrageAnimalCount);
  // Send BEFORE releasing G.mutex. buf is one static copy, and sendSync runs
  // on two tasks (async_tcp via pick, GameLoop via the EVT_REGEN resync), so
  // releasing first let the other caller overwrite buf before text() had
  // copied it -- a client could get a torn or someone else's sync. G.mutex
  // already serialises the build, so holding it through the copy closes that.
  // Deadlock-free: text() only takes this client's own lock (the library
  // never runs a user callback while holding a client lock), and every
  // G.mutex waiter uses a timeout. Costs no RAM; lengthens the hold by the
  // copy, on a join/regen-only path.
  client->text(buf, (size_t)pos);
  uint32_t tickLog = G.tickId;
  bool below = me.depth;
  xSemaphoreGive(G.mutex);

  Log.notice("SYNC pid=%d tick=%lu mapBytes=%d totalBytes=%d",
             pid, (unsigned long)tickLog, mapLen, pos);
  // Reconnecting while underground: the sync above only carries the surface
  // map, so hand over the tunnel board too or this player comes back to an
  // empty screen with no way to redraw it. Sent after the sync so the client
  // has its player id (and therefore its own depth) first.
  if (below) sendTunnelSync(client);
}

// ── Periodic state broadcast (all clients) ───────────────────────────────────
// Buffer: 6 players × ~385 chars + header/footer ~80 = ~2390; sized at 4096
// to safely accommodate it[INV_SLOTS_MAX]+iq[INV_SLOTS_MAX]+eq[5]+is+llCap
// per player (~195 chars × 6 = 1170) — see appendPackArrays()
// plus the "world" block (caravan incl. its ≤4-entry shelf, doom, sparse
// fire + flood lists — ~250 bytes at the FIRE_CAP of 20; sized up front when
// caravan alone landed).
static void broadcastState() {
  // 3328 covered it[12]+iq[12]; INV_SLOTS_MAX is 18 now and appendPackArrays()
  // also emits "is"/"llCap", which is ~70 B more per player (~420 for six).
  PSRAM_STATIC(char, buf, [4096]);
  // Runs unconditionally every 100ms tick, so keep the retry tight: 2× 8ms
  // (16ms worst case) instead of one 5ms try — enough to ride out the brief
  // holders elsewhere (drainEvents ~5ms, trade-expiry sweep ~2ms/offer)
  // without risking a meaningfully late tick.
  bool gotMutex = false;
  for (int attempt = 0; attempt < 2 && !gotMutex; attempt++) {
    gotMutex = (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(8)) == pdTRUE);
  }
  if (!gotMutex) {
    g_broadcastSkips++;
    g_broadcastSkipsConsec++;
    static uint32_t lastBusyLogMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastBusyLogMs >= 1000) {
      lastBusyLogMs = nowMs;
      LOG_VERBOSE("broadcastState: G.mutex busy (rate-limited) totalSkips=%lu consec=%lu",
                  (unsigned long)g_broadcastSkips, (unsigned long)g_broadcastSkipsConsec);
    }
    return;
  }
  g_broadcastSkipsConsec = 0;

  int pos = snprintf(buf, sizeof(buf),
    "{\"t\":\"s\",\"tk\":%lu,\"p\":[", (unsigned long)G.tickId);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (i) buf[pos++] = ',';
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"q\":%d,\"r\":%d,\"sc\":%d,\"inv\":[%d,%d,%d,%d,%d],\"on\":%d,\"sp\":%d,"
      "\"ll\":%d,\"food\":%d,\"water\":%d,\"rad\":%d,"
      "\"mp\":%d,\"fth\":%d,\"wth\":%d,\"wnd\":[%d,%d],\"vm\":%d,",
      p.q, p.r, p.score,
      p.inv[0], p.inv[1], p.inv[2], p.inv[3], p.inv[4],
      p.connected ? 1 : 0, p.steps,
      p.ll, p.food, p.water, p.radiation,
      (int)p.movesLeft, (int)p.fThreshBelow, (int)p.wThreshBelow,
      (int)p.wounds[WOUND_MINOR], (int)p.wounds[WOUND_MAJOR], (int)computeValidMoves(i));
    pos = appendPackArrays(buf, sizeof(buf), pos, i);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      ",\"enc\":%d,\"dp\":%d,\"tq\":%d,\"tr\":%d}",
      encounters[i].active ? 1 : 0,
      // Bunker tunnels: which board this survivor is on, and where on it.
      // q/r above stay pinned to their entrance hatch while dp is 1, which
      // is what lets the surface keep drawing them at the hatch they went
      // down. ~14 B per player worst case.
      (int)p.depth, (int)p.tq, (int)p.tr);
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "],\"gs\":{\"tc\":%d,\"dc\":%d,\"wp\":%d},"
    "\"world\":{\"caravan\":{\"q\":%d,\"r\":%d,\"active\":%d,\"inv\":[%d,%d,%d,%d,%d],\"stock\":[",
    G.threatClock, G.dayCount, (int)G.weatherPhase,
    (int)W.caravan.q, (int)W.caravan.r, W.caravan.active ? 1 : 0,
    W.caravan.inv[0], W.caravan.inv[1], W.caravan.inv[2], W.caravan.inv[3], W.caravan.inv[4]);
  pos += appendCaravanStock(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "]},\"doom\":{\"q\":%d,\"r\":%d,\"awareness\":%d},\"fire\":[",
    (int)W.creepingDoom.q, (int)W.creepingDoom.r, (int)W.creepingDoom.awareness);
  pos += appendFireArray(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"flood\":[");
  pos += appendFloodArray(buf + pos, sizeof(buf) - pos);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "]}}");
  xSemaphoreGive(G.mutex);
  // setCloseClientOnQueueFull(false) (see handleConnect) means a backlogged
  // client silently misses this tick's send instead of getting force-closed —
  // track how often that happens so a chronically-lagging client is visible.
  AsyncWebSocket::SendStatus st = ws.textAll(buf, (size_t)pos);
  // textAll() returns DISCARDED whenever there are zero WS clients at all
  // (hit==0), same as it would for a real all-clients-backlogged case — guard
  // on count() so an idle/empty server doesn't masquerade as one every tick.
  if (st != AsyncWebSocket::ENQUEUED && ws.count() > 0) {
    g_broadcastPartial++;
    static uint32_t lastPartialLogMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastPartialLogMs >= 1000) {
      lastPartialLogMs = nowMs;
      LOG_VERBOSE("broadcastState: send %s (rate-limited) totalPartial=%lu",
                  st == AsyncWebSocket::DISCARDED ? "DISCARDED" : "PARTIAL",
                  (unsigned long)g_broadcastPartial);
    }
  }
}
