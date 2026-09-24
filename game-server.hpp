#pragma once
#include <errno.h>
// ── game-server.hpp ─────────────────────────────────────────────────────────
// Game loop task (Core 1) and server setup helpers extracted from setup().
// Included LAST, after all gameplay and network .hpp files.

#include "lwip/tcpip.h"
#include "esp_wifi.h"

// ── Network watchdog ───────────────────────────────────────────
// 2026-09-24: when a 5-bot run ended, the board stopped answering ARP, HTTP
// and WS for over 15 minutes while the game loop, LCD and audio carried on,
// and WiFi.status() still said connected. The task watchdog (10 s, core 0's
// idle task) never fired, so nothing was spinning: a network task was
// blocked. gameLoopTask calls netWatchdog() every 5 s. It posts a no-op
// through lwIP's own task (tcpip_try_callback) and, when one is still
// unanswered 5 s later, logs every task's state plus the driver's own view of
// the association and the internal heap -- what is stuck, and on what.
static volatile uint32_t g_tcpipPongMs  = 0;   // set by netProbeCb, in the lwIP task
static uint32_t          g_tcpipPingMs  = 0;   // when the outstanding probe went in; 0 = none
static uint32_t          g_tcpipRttMs   = 0;   // last answered probe's round trip
static uint32_t          g_netStallT0   = 0;   // first unanswered probe of the current stall; 0 = none
static uint32_t          g_netDumpMs    = 0;
static uint32_t          g_netBeatMs    = 0;
static uint32_t          g_netMboxFull  = 0;

static void netProbeCb(void*) { g_tcpipPongMs = millis(); }

static const char* netTaskState(eTaskState s) {
  switch (s) {
    case eRunning:   return "RUN";
    case eReady:     return "READY";
    case eBlocked:   return "BLOCKED";
    case eSuspended: return "SUSP";
    case eDeleted:   return "DEL";
    default:         return "?";
  }
}

// The second failure (2026-09-24 20:37): lwIP answered every probe and the
// driver stayed associated, yet no data moved for five minutes until a beacon
// timeout (reason 200) dropped the link and a rejoin brought it all back. The
// internal heap's largest block had fallen 91 -> 18 KB just before. What
// follows is for telling that apart next time: allocation failures, heap
// fragmentation (internal and DMA-capable, which the driver's buffers need),
// each seated socket's queue and TCP send space, and a "data dead" snapshot
// when players are seated but no WS event has arrived for 20 s.

// Every failed allocation, from any task. Counters only: logging from inside
// an allocator failure could itself allocate. netLogHeap() reports them.
static volatile uint32_t g_allocFailN = 0, g_allocFailIntN = 0;
static volatile uint32_t g_allocFailLastSize = 0, g_allocFailLastCaps = 0, g_allocFailLastMs = 0;
static volatile uint32_t g_allocFailMinSize = 0xFFFFFFFFu, g_allocFailMaxSize = 0;
static void netAllocFailed(size_t size, uint32_t caps, const char* fn) {
  (void)fn;   // always the heap API's own name, so it says nothing
  g_allocFailN++;
  if (caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) g_allocFailIntN++;
  g_allocFailLastSize = (uint32_t)size; g_allocFailLastCaps = caps; g_allocFailLastMs = millis();
  if (size < g_allocFailMinSize) g_allocFailMinSize = (uint32_t)size;
  if (size > g_allocFailMaxSize) g_allocFailMaxSize = (uint32_t)size;
}

static uint32_t g_wsEvtNPrev = 0, g_wsEvtPrevMs = 0;

// Internal and DMA-capable heap in full: a fragmenting heap shows as the free
// block count climbing while `largest` falls and `free` holds.
static void netLogHeap(const char* tag) {
  multi_heap_info_t in = {}, dm = {};
  heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&dm, MALLOC_CAP_DMA);
  uint32_t now = millis();
  Log.notice("NETWD %s heap int free=%u largest=%u min=%u blocks=%u free_blocks=%u | "
             "dma free=%u largest=%u | allocFail=%u int=%u last=%uB caps=0x%x %us ago size=%u..%u",
             tag, (unsigned)in.total_free_bytes, (unsigned)in.largest_free_block,
             (unsigned)in.minimum_free_bytes, (unsigned)in.total_blocks, (unsigned)in.free_blocks,
             (unsigned)dm.total_free_bytes, (unsigned)dm.largest_free_block,
             (unsigned)g_allocFailN, (unsigned)g_allocFailIntN, (unsigned)g_allocFailLastSize,
             (unsigned)g_allocFailLastCaps,
             (unsigned)(g_allocFailN ? (now - g_allocFailLastMs) / 1000 : 0),
             (unsigned)(g_allocFailN ? g_allocFailMinSize : 0), (unsigned)g_allocFailMaxSize);
}

// Each seated socket: messages queued in AsyncWebSocket, and the TCP send
// buffer space left. A socket whose space sits at 0 while its queue is full
// is one whose peer is not ACKing -- the data path, not the game.
// Read without G.mutex, the way refreshWsLiveness() looks clients up: it is
// only ever printed.
static void netLogClients() {
  char line[200]; int n = 0;
  for (int i = 0; i < MAX_PLAYERS && n < (int)sizeof(line) - 40; i++) {
    const Player& p = G.players[i];
    if (!p.connected) continue;
    AsyncWebSocketClient* c = ws.client(p.wsClientId);
    AsyncClient* tc = c ? c->client() : nullptr;
    if (!c) { n += snprintf(line + n, sizeof(line) - n, " s%d:gone", i); continue; }
    n += snprintf(line + n, sizeof(line) - n, " s%d:q%u%s/sp%u", i, (unsigned)c->queueLen(),
                  c->queueIsFull() ? "F" : "", tc ? (unsigned)tc->space() : 0u);
  }
  if (n) Log.notice("NETWD   sockets%s", line);
}

// One line on the link: the driver's association (not WiFi.status(), which
// only moves when the event task delivers an event), softAP stations (a
// station dozing in power save makes the driver hold frames for it), WS event
// rate, and how long since async_tcp last delivered anything.
static void netLogLink(const char* tag, uint32_t now) {
  wifi_ap_record_t ap = {};
  bool assoc = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
  uint32_t dt = now - g_wsEvtPrevMs;
  unsigned rate = (g_wsEvtPrevMs && dt) ? (unsigned)((g_wsEvtN - g_wsEvtNPrev) * 1000u / dt) : 0u;
  g_wsEvtNPrev = g_wsEvtN; g_wsEvtPrevMs = now;
  Log.notice("NETWD %s tcpip rtt=%ums assoc=%d rssi=%d ch=%d wifiStatus=%d apSta=%d wsIdle=%us wsEv/s=%u seated=%d "
             "iheap=%uKB ilargest=%uKB imin=%uKB mboxFull=%u",
             tag, (unsigned)g_tcpipRttMs, assoc ? 1 : 0, assoc ? (int)ap.rssi : 0,
             assoc ? (int)ap.primary : 0, (int)WiFi.status(), (int)WiFi.softAPgetStationNum(),
             (unsigned)((now - g_lastWsEvtMs) / 1000), rate, (int)G.connectedCount,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)g_netMboxFull);
}

// Set from the Wi-Fi event task; the game loop takes the snapshot, since by
// then the sockets are still in the table for one more pass.
static const char* volatile g_netSnapReq = nullptr;

// The tasks that matter, by name. uxTaskGetSystemState() would list them all,
// but the core's prebuilt FreeRTOS is linked without the trace facility.
// tiT = lwIP, wifi = the driver, sys_evt / arduino_events = the event loops
// WiFi.status() depends on.
static void netDumpTasks() {
  static const char* const NAMES[] = { "tiT", "wifi", "async_tcp", "sys_evt", "arduino_events",
                                       "esp_timer", "snd", "GameLoop", "loopTask", "wifiSweep",
                                       "wifiConn" };
  for (const char* nm : NAMES) {
    TaskHandle_t h = xTaskGetHandle(nm);
    if (!h) continue;
    BaseType_t core = xTaskGetAffinity(h);
    Log.warning("NETWD   task=%s state=%s prio=%u core=%d stack_free=%u",
                nm, netTaskState(eTaskGetState(h)), (unsigned)uxTaskPriorityGet(h),
                core == tskNO_AFFINITY ? -1 : (int)core,
                (unsigned)uxTaskGetStackHighWaterMark(h));
  }
}

static void netWatchdog(uint32_t now) {
  bool pending = g_tcpipPingMs && (int32_t)(g_tcpipPongMs - g_tcpipPingMs) < 0;
  if (pending) {
    // Still no answer after a whole period. One probe at a time: stacking
    // more on a stuck mbox only fills it.
    if (!g_netStallT0) g_netStallT0 = g_tcpipPingMs;
    if (!g_netDumpMs || now - g_netDumpMs >= 30000) {
      g_netDumpMs = now;
      Log.warning("NETWD STALL: lwIP task has not run a probe for %ums", (unsigned)(now - g_tcpipPingMs));
      netLogLink("stall", now);
      netDumpTasks();
    }
    return;
  }
  if (g_tcpipPingMs) g_tcpipRttMs = g_tcpipPongMs - g_tcpipPingMs;
  if (g_netStallT0) {
    Log.warning("NETWD recovered: lwIP task was stuck for %ums", (unsigned)(g_tcpipPongMs - g_netStallT0));
    g_netStallT0 = 0; g_netDumpMs = 0;
  }
  // Players seated, but nothing inbound for 20 s: the data path is dead while
  // lwIP is not. Inbound means DATA / PING / PONG -- a DISCONNECT is lwIP
  // timing a dead socket out, and counting it once reported a 19-minute wedge
  // as "resumed". Bots send several messages a second; a browser player idle
  // that long also trips the log (not the heal -- see below).
  static uint32_t deadT0 = 0, deadDumpMs = 0, lastHealMs = 0, healN = 0;
  // Signed: async_tcp can stamp g_lastWsDataMs just after `now` was read.
  int32_t quietMs = (int32_t)(now - g_lastWsDataMs);
  bool dataDead = G.connectedCount > 0 && quietMs >= 20000;
  if (dataDead) {
    if (!deadT0) deadT0 = g_lastWsDataMs;
    if (!deadDumpMs || now - deadDumpMs >= 30000) {
      deadDumpMs = now;
      Log.warning("NETWD DATA-DEAD: nothing inbound for %us with %d seated, lwIP answering",
                  (unsigned)((now - g_lastWsDataMs) / 1000), (int)G.connectedCount);
      netLogLink("dead", now);
      netLogHeap("dead");
      netLogClients();
      netDumpTasks();
    }
    // The heal. That state latches: memory recovered and every player gone,
    // it stayed dead for 50 min on 2026-09-24, and only a reassociation (a
    // beacon timeout, the first time) brought it back. So after 30 s, if
    // every seated socket is jammed -- queue full or no TCP send space, i.e.
    // nothing is being ACKed, which an idle browser never looks like --
    // drop and rejoin the AP. At most every 2 minutes.
    if (quietMs >= 30000 && (!lastHealMs || now - lastHealMs >= 120000)) {
      int seated = 0, jammed = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!G.players[i].connected) continue;
        AsyncWebSocketClient* c = ws.client(G.players[i].wsClientId);
        if (!c) continue;
        seated++;
        AsyncClient* tc = c->client();
        if (c->queueIsFull() || (tc && tc->space() < 64)) jammed++;
      }
      if (seated > 0 && jammed == seated) {
        lastHealMs = now; healN++;
        Log.warning("NETWD HEAL #%u: all %d seated sockets jammed, nothing inbound for %us -- reassociating Wi-Fi",
                    (unsigned)healN, seated, (unsigned)((now - g_lastWsDataMs) / 1000));
        wifiNextSweepMs = now + 30000;   // keep the roaming sweep off this reconnect
        WiFi.reconnect();
      }
    }
  } else if (deadT0) {
    Log.warning("NETWD data resumed after %us (heals so far %u)",
                (unsigned)((now - deadT0) / 1000), (unsigned)healN);
    deadT0 = 0; deadDumpMs = 0;
  }
  if (const char* why = g_netSnapReq) {
    g_netSnapReq = nullptr;
    netLogLink(why, now);
    netLogHeap(why);
    netLogClients();
    netDumpTasks();
  }
  // Heartbeat: every 60 s, every 10 s while anyone is seated -- the slide
  // toward a wedge is what the last run did not record.
  uint32_t beat = G.connectedCount > 0 ? 10000 : 60000;
  if (now - g_netBeatMs >= beat) {
    g_netBeatMs = now;
    netLogLink("ok", now);
    netLogHeap("ok");
    if (G.connectedCount > 0) netLogClients();
  }
  g_tcpipPingMs = now ? now : 1;
  if (tcpip_try_callback(netProbeCb, nullptr) != ERR_OK) {
    // The lwIP mailbox is full: the task is not draining it. Counted, and
    // treated as a probe that never got answered.
    g_netMboxFull++;
    Log.warning("NETWD lwIP mailbox full (x%u)", (unsigned)g_netMboxFull);
  }
}

// Wi-Fi events. None were logged before, so a dropped association left no
// trace in the serial log. Runs on the Arduino event task.
static void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Log.notice("WIFI STA associated ch=%d", (int)info.wifi_sta_connected.channel); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Log.warning("WIFI STA disconnected reason=%d", (int)info.wifi_sta_disconnected.reason);
      netLogHeap("wifi-disc");            // the heap as the link went, before sockets close
      g_netSnapReq = "wifi-disc";         // sockets and tasks from the game loop
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Log.notice("WIFI STA got ip=%s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str()); break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Log.warning("WIFI STA lost ip"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Log.notice("WIFI AP client joined aid=%d", (int)info.wifi_ap_staconnected.aid); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Log.notice("WIFI AP client left aid=%d", (int)info.wifi_ap_stadisconnected.aid); break;
    default: break;
  }
}

// ── Game loop task (Core 1) ────────────────────────────────────
static uint32_t g_maxTickMs = 0;  // worst-case tickGame+drainEvents+broadcastState time, surfaced via /state

static void gameLoopTask(void* param) {
  Log.notice("gameLoopTask running core=%d prio=%d stack_free=%u",
             (int)xPortGetCoreID(), (int)uxTaskPriorityGet(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t lastWatermarkMs = 0;
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TICK_MS));
    uint32_t loopMs = millis();
    if (loopMs - lastWatermarkMs >= 5000) {
      lastWatermarkMs = loopMs;
      LOG_VERBOSE("gameLoop wm: stack_free=%u heap=%uKB psram=%uKB tickId=%lu connected=%d",
                  (unsigned)uxTaskGetStackHighWaterMark(NULL),
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024),
                  (unsigned long)G.tickId, (int)G.connectedCount);
      netWatchdog(loopMs);
    }
    uint32_t t0tick = millis();
    tickGame();
    drainEvents();
    // ── Trade offer expiry sweep ──────────────────────────────────
    {
      uint32_t nowMs = millis();
      for (int ti = 0; ti < MAX_PLAYERS; ti++) {
        if (tradeOffers[ti].active && nowMs >= tradeOffers[ti].expiresMs) {
          if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            if (tradeOffers[ti].active && nowMs >= tradeOffers[ti].expiresMs) {
              Log.notice("Trade expired: from=%d to=%d",
                         (int)tradeOffers[ti].fromPid, (int)tradeOffers[ti].toPid);
              GameEvent tev = {};
              tev.type        = EVT_TRADE_RESULT;
              tev.pid         = tradeOffers[ti].fromPid;
              tev.tradeTo     = tradeOffers[ti].toPid;
              tev.tradeResult = 3; // expired
              tradeOffers[ti].active = false;
              enqEvt(tev);
            }
            xSemaphoreGive(G.mutex);
          }
        }
      }
    }
    broadcastState();
    // Refresh per-slot WS liveness so handleConnect's stale-slot reap only
    // fires on a slot that has been unverifiable for the whole grace window,
    // not on one bad ws.client() look (see network-session.hpp).
    refreshWsLiveness();
    uint32_t tickDurMs = millis() - t0tick;
    if (tickDurMs > g_maxTickMs) g_maxTickMs = tickDurMs;
    // Saves the WS handlers asked for (network-persistence.hpp), after the
    // tick is timed so maxTickMs keeps meaning the tick.
    serviceSaveRequest();
  }
}

// ── Derive image variant counts from PSRAM imgCache ────────────
// Called from setup() after loadWebFilesToRAM(). Populates terrainVariantCount[],
// shelterVariantCount[], and forrageAnimalCount by scanning cached filenames.
static void setupVariantCounts() {
  Log.notice("Variant scan start: imgCacheCount=%d", (int)imgCacheCount);
  // /img/tiles.json (scripts/tilegen/build_tiles.py) packs every terrain tile
  // into a couple of atlas pages and carries the pool sizes as one
  // "counts":[...] array. When it is on the card it wins; the per-file
  // hex<Name><N>.png scan below is the fallback for a card without it.
  bool fromAtlas = false;
  for (int i = 0; i < imgCacheCount && !fromAtlas; i++) {
    if (strcmp(imgCache[i].name, "tiles.json") != 0) continue;
    const char* s = (const char*)imgCache[i].buf;
    const size_t n = imgCache[i].len;
    static const char KEY[] = "\"counts\":[";
    const size_t kl = sizeof(KEY) - 1;
    for (size_t p = 0; p + kl <= n && !fromAtlas; p++) {
      if (memcmp(s + p, KEY, kl) != 0) continue;
      size_t q = p + kl;
      for (int t = 0; t < NUM_TERRAIN && q < n; t++) {
        int v = 0;
        while (q < n && s[q] >= '0' && s[q] <= '9') { v = v * 10 + (s[q] - '0'); q++; }
        terrainVariantCount[t] = (uint8_t)min(v, 16);   // cell.variant is 4 bits on the wire
        while (q < n && (s[q] == ',' || s[q] == ' ')) q++;
        if (q >= n || s[q] == ']') break;
      }
      fromAtlas = true;
    }
  }
  if (fromAtlas) Log.notice("Variant counts: terrain pools from tiles.json (atlas)");
  for (int i = 0; !fromAtlas && i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    for (int t = 0; t < NUM_TERRAIN; t++) {
      String pfx = String("hex") + TERRAIN_IMG_NAME[t];
      if (fname.startsWith(pfx) && fname.endsWith(".png")) {
        String numStr = fname.substring(pfx.length(), fname.length() - 4);
        if (numStr.length() > 0) {
          bool dig = true;
          for (int k = 0; k < (int)numStr.length(); k++)
            if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
          if (dig) {
            int idx = numStr.toInt();
            if (idx + 1 > (int)terrainVariantCount[t])
              terrainVariantCount[t] = (uint8_t)min(idx + 1, 255);
          }
        }
        break;
      }
    }
  }
  const char* SHELTER_PFX[2] = { "shelterBasic", "shelterImproved" };
  for (int i = 0; i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    for (int s = 0; s < 2; s++) {
      String pfx = String(SHELTER_PFX[s]);
      if (fname.startsWith(pfx) && fname.endsWith(".png")) {
        String numStr = fname.substring(pfx.length(), fname.length() - 4);
        if (numStr.length() > 0) {
          bool dig = true;
          for (int k = 0; k < (int)numStr.length(); k++)
            if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
          if (dig) {
            int idx = numStr.toInt();
            if (idx + 1 > (int)shelterVariantCount[s])
              shelterVariantCount[s] = (uint8_t)min(idx + 1, 255);
          }
        }
        break;
      }
    }
  }
  for (int i = 0; i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    if (fname.startsWith("forrageAnimal") && fname.endsWith(".png")) {
      String numStr = fname.substring(13, fname.length() - 4);
      if (numStr.length() > 0) {
        bool dig = true;
        for (int k = 0; k < (int)numStr.length(); k++)
          if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
        if (dig) {
          int idx = numStr.toInt();
          if (idx + 1 > (int)forrageAnimalCount)
            forrageAnimalCount = (uint8_t)min(idx + 1, 255);
        }
      }
    }
  }
  Log.notice("Variant counts: terrain=[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d] shelter=[%d,%d] forrageAnimal=%d",
             (int)terrainVariantCount[0],(int)terrainVariantCount[1],(int)terrainVariantCount[2],
             (int)terrainVariantCount[3],(int)terrainVariantCount[4],(int)terrainVariantCount[5],
             (int)terrainVariantCount[6],(int)terrainVariantCount[7],(int)terrainVariantCount[8],
             (int)terrainVariantCount[9],(int)terrainVariantCount[10],(int)terrainVariantCount[11],
             (int)shelterVariantCount[0],(int)shelterVariantCount[1],(int)forrageAnimalCount);
}

// ── Cache-Control policy per asset ──────────────────────────────
// Entry points that must always revalidate: the page itself, the service
// worker, and the asset manifests the loader reads first. Everything else is
// requested by the client as "<url>?v=<manifest version>", so it can be
// immutable — a new build changes the query string, not the cache policy.
static const char* cacheControlFor(const char* url, const char* mime) {
  if (strcmp(mime, "text/html") == 0)               return "no-cache";
  if (strcmp(url, "/sw.js") == 0)                    return "no-cache";
  if (strcmp(url, "/assets.json") == 0)              return "no-cache";
  if (strcmp(url, "/web-assets.json") == 0)          return "no-cache";
  return "public, max-age=31536000, immutable";
}

// ── HTTP asset admission control ────────────────────────────────────────────
// Bounds concurrent in-flight sendWebFile()/img requests. Both routes serve
// already-in-PSRAM bytes with no blocking I/O, so the constraint isn't per-
// request time — it's aggregate concurrent-connection load on the single
// async_tcp task (see docs/dev-loop.md "Web asset pipeline"). Measured: 5-6
// simultaneous full page loads (each tab's own 2-concurrent asset queue, with
// zero cross-tab coordination) took the whole WiFi stack down in ~15-20s even
// with heap healthy (170KB+ free) — this caps aggregate concurrency instead.
//
// async_tcp-task-only: increment (here) and decrement (onDisconnect below)
// both run exclusively on the single AsyncTCP dispatch task — never from
// gameLoopTask/Core 1 — so a plain int needs no lock/atomic/volatile. If that
// ever changes, add evtMux protection the same way lobbyIds[] has it.
static const int MAX_CONCURRENT_ASSET_REQS = 4;  // conservative default; tune from telemetry
static int      g_activeAssetReqs = 0;   // current in-flight requests
static uint32_t g_assetReqRejects = 0;   // lifetime 429 count, surfaced via /state

// Call as the FIRST statement in a gated handler. Sends 429 and returns false
// if over the cap; otherwise increments, registers the release, returns true.
static bool admitAssetRequest(AsyncWebServerRequest* req) {
  if (g_activeAssetReqs >= MAX_CONCURRENT_ASSET_REQS) {
    g_assetReqRejects++;
    static uint32_t lastRejectLogMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastRejectLogMs >= 1000) {
      lastRejectLogMs = nowMs;
      Log.warning("HTTP 429 %s active=%d cap=%d totalRejects=%lu (rate-limited) heap=%uKB",
                  req->url().c_str(), g_activeAssetReqs, MAX_CONCURRENT_ASSET_REQS,
                  (unsigned long)g_assetReqRejects, (unsigned)(ESP.getFreeHeap() / 1024));
    }
    AsyncWebServerResponse* r = req->beginResponse(429, "text/plain", "Busy");
    r->addHeader("Retry-After", "1");
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
    return false;
  }
  g_activeAssetReqs++;
  req->onDisconnect([]() {                              // no captures — req may be mid-teardown
    if (g_activeAssetReqs > 0) g_activeAssetReqs--;     // defensive floor, same pattern as
  });                                                     // G.connectedCount-- in network-session.hpp
  return true;
}

// Serve webFiles[i] (PSRAM) for one request: 304 on ETag match, else 200 with
// the cached bytes. Runs on the async_tcp task — keep it allocation-light.
static void sendWebFile(AsyncWebServerRequest* req, int i) {
  if (!admitAssetRequest(req)) return;
  const WebFile& wf = webFiles[i];
  const char* cc = cacheControlFor(wf.url, wf.mime);
  if (req->hasHeader("If-None-Match") &&
      req->getHeader("If-None-Match")->value() == wf.etag) {
    LOG_VERBOSE("HTTP 304 %s", wf.url);
    AsyncWebServerResponse* r = req->beginResponse(304);
    r->addHeader("ETag", wf.etag);
    r->addHeader("Cache-Control", cc);
    req->send(r);
    return;
  }
  LOG_VERBOSE("HTTP GET %s -> %s %u B%s heap=%uKB",
              wf.url, wf.mime, (unsigned)wf.len, wf.gzip ? " gz" : "",
              (unsigned)(ESP.getFreeHeap() / 1024));
  AsyncWebServerResponse* resp = req->beginResponse(200, wf.mime, wf.buf, wf.len);
  resp->addHeader("ETag", wf.etag);
  resp->addHeader("Cache-Control", cc);
  if (wf.gzip) {
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Vary", "Accept-Encoding");
  }
  req->send(resp);
}

// ── /upload body writer (shared by the multipart and raw-body callbacks) ───
// Runs on the async_tcp task. One upload at a time (sync scripts are
// sequential); state is reset on index == 0. The request handler reads
// g_uploadOk / g_uploadTotal to answer 200 or 500 — before this, a failed
// SD open still returned "OK".
static File     g_uploadFile;
static size_t   g_uploadTotal = 0;
static bool     g_uploadOk    = false;
static String   g_uploadDest;
static char     g_uploadLastErr[120] = {0};   // surfaced in /state mem.lastUploadErr
static uint32_t g_uploadResumes = 0;          // successful mid-file recoveries (telemetry)

// Write one body chunk, recovering from short writes. Observed on the K10:
// f_write fails at offsets 16 KB apart (12288, 28672, 45056, ...) — the card
// goes busy at a physical block boundary and the SPI transaction times out.
// FatFs then latches the error on the handle, so every later write fails and
// the file is left truncated at a cluster boundary. Recovery: close, let the
// card settle, reopen for append, confirm the on-disk size is exactly what we
// have accounted for, then continue with the unwritten remainder.
//
// Every chunk is flushed and its on-disk size confirmed before we account for
// it (g_uploadTotal only ever holds *verified* bytes). fwrite() is buffered,
// so without the flush the error surfaces a few KB late and the bytes that
// were still in the stdio buffer are gone — the first version of this resume
// saw disk=12288 vs accounted=15796 and had nothing to rewrite them from. With
// per-chunk verification the only bytes at risk are the current chunk's, and
// we still hold those.
static bool uploadWriteChunk(uint8_t* data, size_t len) {
  const size_t chunkStart = g_uploadTotal;            // verified bytes before this chunk
  for (int attempt = 0; attempt < 5; attempt++) {
    size_t done = g_uploadTotal - chunkStart;         // bytes of this chunk already on disk
    size_t w = 0;
    if (g_uploadFile) {
      w = g_uploadFile.write(data + done, len - done);
      g_uploadFile.flush();
    }
    size_t onDisk = g_uploadFile ? g_uploadFile.size() : 0;
    if (onDisk >= chunkStart + len) {
      g_uploadTotal = chunkStart + len;
      if (attempt) g_uploadResumes++;
      return true;
    }
    int e = errno;
    snprintf(g_uploadLastErr, sizeof(g_uploadLastErr),
             "%s: short write %u/%u at %u (disk=%u) errno=%d try=%d heap=%uKB",
             g_uploadDest.c_str(), (unsigned)w, (unsigned)(len - done), (unsigned)g_uploadTotal,
             (unsigned)onDisk, e, attempt + 1, (unsigned)(ESP.getFreeHeap() / 1024));
    Log.warning("UPLOAD %s", g_uploadLastErr);
    g_uploadFile.close();
    delay(25 * (attempt + 1));                        // let the card finish its busy cycle
    g_uploadFile = SD.open(g_uploadDest.c_str(), FILE_APPEND);
    if (!g_uploadFile) { Log.error("UPLOAD reopen FAIL %s", g_uploadDest.c_str()); return false; }
    onDisk = g_uploadFile.size();
    if (onDisk < chunkStart || onDisk > chunkStart + len) {   // verified bytes vanished / FS confused
      Log.error("UPLOAD resume mismatch %s disk=%u chunk=%u..%u", g_uploadDest.c_str(),
                (unsigned)onDisk, (unsigned)chunkStart, (unsigned)(chunkStart + len));
      return false;
    }
    g_uploadTotal = onDisk;                           // continue from what actually landed
  }
  return false;
}

static void uploadChunk(AsyncWebServerRequest* request, const String& filename,
                        size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    String dest = request->hasParam("dest")
                  ? request->getParam("dest")->value()
                  : "/data/" + (filename.length() ? filename : String("upload.bin"));
    if (!dest.startsWith("/")) dest = "/" + dest;
    if (dest.indexOf("..") >= 0) {                       // keep writes under the SD root
      Log.error("UPLOAD rejected dest=%s", dest.c_str());
      g_uploadOk = false; return;
    }
    g_uploadDest  = dest;
    g_uploadTotal = 0;
    if (g_uploadFile) g_uploadFile.close();
    // Create any missing parent directories -- SD.open(FILE_WRITE) will not,
    // so a sync that introduces a new folder (e.g. encounters/traps/) used to
    // 500 on every file in it.
    for (int s = dest.indexOf("/", 1); s > 0; s = dest.indexOf("/", s + 1)) {
      String dir = dest.substring(0, s);
      if (!SD.exists(dir.c_str())) SD.mkdir(dir.c_str());
    }
    g_uploadFile = SD.open(dest.c_str(), FILE_WRITE);
    g_uploadOk   = (bool)g_uploadFile;
    if (g_uploadOk) Log.notice("UPLOAD start dest=%s", dest.c_str());
    else            Log.error("SD WRITE FAIL: %s", dest.c_str());
    UploadUI::begin(dest.c_str());
  }
  if (g_uploadOk && len) {
    if (!uploadWriteChunk(data, len)) {
      g_uploadOk = false;
      Log.error("SD WRITE FAIL: %s after %u bytes — %s",
                g_uploadDest.c_str(), (unsigned)g_uploadTotal, g_uploadLastErr);
    }
    UploadUI::chunk(g_uploadTotal);
    yield();
  }
  if (final) {
    if (g_uploadFile) {
      g_uploadFile.flush();
      g_uploadFile.close();
      Log.notice("UPLOAD complete dest=%s total=%u ok=%d",
                 g_uploadDest.c_str(), (unsigned)g_uploadTotal, (int)g_uploadOk);
    }
    UploadUI::end(g_uploadOk);
  }
}

// ── WiFi, HTTP routes, and WebSocket setup ──────────────────────
// Extracted from setup() to keep that function concise.
static void setupWiFiAndServer() {
  Log.notice("Starting WiFi/HTTP/WS setup");
  splashAdd("Starting WiFi...");
  heap_caps_register_failed_alloc_callback(netAllocFailed);   // counted, reported by NETWD
  {
    // Proof that setup()'s heap_caps_malloc_extmem_enable() took: where a
    // plain 2 KB malloc lands now (it used to be internal).
    void* t = malloc(2048);
    Log.notice("malloc routing: 2 KB -> %s, internal free=%u largest=%u",
               t && esp_ptr_external_ram(t) ? "PSRAM" : "internal",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    free(t);
  }
  WiFi.onEvent(onWifiEvent);     // before WiFi.mode(), so the first association is logged too
  WiFi.setHostname(MDNS_HOST);   // DHCP hostname; must precede WiFi.mode()
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, nullptr, 1, 0, AP_MAX_CLIENTS);
  Log.notice("AP start SSID=%s maxClients=%d", AP_SSID, AP_MAX_CLIENTS);
  // http://k10.local/ -- the DHCP lease moves on every reboot, and a crash
  // mid-run used to mean a subnet sweep to find the board again. The
  // responder picks up each interface (softAP now, STA once it joins, and the
  // AP-only fallback) from the IP events itself.
  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Log.notice("mDNS responder up host=%s.local", MDNS_HOST);
  } else {
    Log.warning("mDNS responder failed to start");
  }
  wifiStoreLoad();
  {
    // Fast path first: the ESP32's own one-slot credential is whatever network
    // we last joined, so at home this connects in a few seconds with no scan.
    // If it doesn't answer (we're somewhere else), loop()'s roaming sweep takes
    // over and hunts for any other network in wifi-store.hpp.
    wifi_config_t staCfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &staCfg) == ESP_OK && staCfg.sta.ssid[0]) {
      strlcpy(savedSsid, (char*)staCfg.sta.ssid, sizeof(savedSsid));
      Log.notice("Saved STA creds found ssid=%s -> joining", savedSsid);
      splashAdd("Joining saved WiFi...", 0x4080C0);
      WiFi.begin();
      Log.notice("STA connect attempt (saved creds)");
      bootWifiPending = true;
      bootWifiStartMs = millis();
    } else if (g_knownCount > 0) {
      Log.notice("No STA creds in NVS but %d known network(s) -> sweeping",
                 (int)g_knownCount);
      splashAdd("Scanning for known WiFi...", 0x4080C0);
      wifiNextSweepMs = millis();   // loop() kicks the sweep on its next pass
    } else {
      LOG_VERBOSE("No saved STA creds");
    }
  }
  { char wb[30]; snprintf(wb, 30, "AP: %s", WiFi.softAPIP().toString().c_str());
    splashAdd(wb, 0x60A040);
    Log.notice("AP IP=%s mac=%s", WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str()); }

  ws.onEvent(onWsEvent); ws.enable(true); server.addHandler(&ws);
  Log.notice("WS handler registered path=/ws");

  // Static web assets served from PSRAM — one route per discovered file.
  // Query strings (the loader's "?v=<version>") are ignored by path matching.
  for (int i = 0; i < webFileCount; i++) {
    server.on(webFiles[i].url, HTTP_GET, [i](AsyncWebServerRequest* req) { sendWebFile(req, i); });
  }
  {
    int idx = findWebFile("/index.html");
    if (idx >= 0) {
      server.on("/", HTTP_GET, [idx](AsyncWebServerRequest* req) { sendWebFile(req, idx); });
    } else {
      Log.error("HTTP: no /index.html cached — '/' will 503 until SD has data/index.html");
      server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(503, "text/plain", "index.html not cached - check SD & reboot");
      });
    }
  }
  Log.notice("HTTP static routes: %d files", webFileCount);
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
    LOG_VERBOSE("HTTP 204 /favicon.ico");
    req->send(204);
  });

  // Captive-portal redirects
  auto toGame = [](AsyncWebServerRequest* req) {
    LOG_VERBOSE("HTTP redirect %s -> /", req->url().c_str());
    req->redirect("/");
  };
  server.on("/generate_204",              HTTP_GET, toGame);
  server.on("/gen_204",                   HTTP_GET, toGame);
  server.on("/hotspot-detect.html",       HTTP_GET, toGame);
  server.on("/library/test/success.html", HTTP_GET, toGame);
  server.on("/ncsi.txt",                  HTTP_GET, toGame);
  server.on("/connecttest.txt",           HTTP_GET, toGame);
  server.on("/fwlink",                    HTTP_GET, toGame);

  // /state — full game-state JSON endpoint
  // The speaker measurement: the audio task plays a fixed 25 s sequence of
  // exact digital levels (snd-engine.hpp sndCalBlock) for a microphone in
  // front of the board. scripts/sndsim/k10measure.py triggers it and records.
  // Read-only as far as the game goes; it only borrows the speaker.
  // ?fmt=N first reinstalls I2S in format N (ui-audio.hpp SND_FMT) for an A/B;
  // ?seq=1 plays real content through the mix instead of test tones; ?rec=1
  // records the board's own mics meanwhile, for GET /sndrec.wav.
  server.on("/sndtest", HTTP_GET, [](AsyncWebServerRequest* req) {
    int fmt = req->hasParam("fmt") ? req->getParam("fmt")->value().toInt() : -1;
    int seq = req->hasParam("seq") ? req->getParam("seq")->value().toInt() : 0;
    bool rec = req->hasParam("rec");
    if (fmt >= 0 && fmt < (int)SND_FMT_N) sndFmtReq = (uint8_t)(fmt + 1);
    if (seq < 0 || seq > 2) seq = 0;
    if (rec) sndRecReq = (seq == 1) ? 22.0f : (seq == 2) ? 11.5f : 26.5f;
    sndStory(SS_CALIBRATE, (uint8_t)seq);
    Log.notice("HTTP /sndtest fmt=%d seq=%d rec=%d", fmt, seq, (int)rec);
    req->send(200, "text/plain", seq == 1 ? "sndtest: content sequence (21 s)\n"
                               : seq == 2 ? "sndtest: engine tones, then the narrator (10.5 s)\n"
                                          : "sndtest: tone sequence (25 s)\n");
  });
  // The last capture, as a 16 kHz stereo WAV straight out of PSRAM.
  server.on("/sndrec.wav", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (sndRecState != 2 || !sndRecBuf) {
      char m[80];
      snprintf(m, sizeof(m), "not ready: state=%u frames=%u\n", (unsigned)sndRecState, (unsigned)sndRecFrames);
      req->send(409, "text/plain", m);
      return;
    }
    req->send(req->beginResponse(200, "audio/wav", (const uint8_t*)sndRecBuf, 44 + sndRecFrames * 4));
  });
  // Live sound knobs (ui-audio.hpp SND_KNOB, which the sound desk data/sound.html
  // drives): /snddbg?key=value[&key=value...]; &save=1 stores the mix on the
  // board, &reset=1 restores the compiled one. Replies with every knob's value.
  // amp=0/1 drives eAmp_Gain (debug only: 1 mutes the speaker).
  server.on("/snddbg", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("reset")) sndKnobReset();
    for (int k = 0; k < SNDK_COUNT; k++)
      if (req->hasParam(SND_KNOB[k].key)) sndKnobSet(k, req->getParam(SND_KNOB[k].key)->value().toFloat());
    if (req->hasParam("amp")) sndAmpReq = (int8_t)(req->getParam("amp")->value().toInt() ? 1 : 0);
    if (req->hasParam("save")) sndKnobSave();
    req->send(200, "application/json", sndKnobJson());
  });
  // The sound desk's catalogue: knob ranges and defaults, and every effect,
  // vocabulary line, speaking style, music style and story beat by name.
  server.on("/sndinfo", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", sndInfoJson());
  });
  // Sound desk playback, through the same thread-safe queue as the game:
  // sfx=N | say=N[&style=S] | story=K[&a=A&b=B] | seq=1|2 | stop=1.
  // Nothing plays while the volume is 0 (the muted audio task drops cues).
  server.on("/sndplay", HTTP_GET, [](AsyncWebServerRequest* req) {
    auto num = [req](const char* k, int def) { return req->hasParam(k) ? (int)req->getParam(k)->value().toInt() : def; };
    if (req->hasParam("stop")) sndStory(SS_STOP);
    if (req->hasParam("sfx")) {
      int n = num("sfx", 0);
      if (n > SFX_NONE && n < SFX_COUNT) sndStory(SS_SFX, (uint8_t)n);
    }
    if (req->hasParam("say")) {
      int n = num("say", 0), s = num("style", SAY_NARRATOR);
      if (n >= 0 && n < VOC_COUNT && s >= 0 && s < SAY_STYLE_COUNT) sndStory(SS_SAY, (uint8_t)n, (uint8_t)s);
    }
    if (req->hasParam("story")) {
      int k = num("story", 0);
      if (k > SS_NONE && k < SS_COUNT && k != SS_CALIBRATE) sndStory((uint8_t)k, (uint8_t)num("a", 0), (uint8_t)num("b", 0));
    }
    if (req->hasParam("seq")) {
      int s = num("seq", 1);
      if (s == 1 || s == 2) sndStory(SS_CALIBRATE, (uint8_t)s);
    }
    req->send(200, "application/json", s_audioVol == 0 ? "{\"ok\":true,\"muted\":true}" : "{\"ok\":true,\"muted\":false}");
  });

  server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    static const char* TNAME_FULL[NUM_TERRAIN] = {
      "Open Scrub","Ash Dunes","Rust Forest","Marsh","Broken Urban",
      "Flooded Ruins","Glass Fields","Rolling Hills","Mountain","Settlement","Nuke Crater","River Channel",
      "Bunker Entrance","Vent Shaft","Tunnel Floor","Tunnel Collapsed"
    };
    static const char* RES_NAME_L[6] = {"none","water","food","fuel","medicine","scrap"};
    LOG_VERBOSE("HTTP /state pid=%s",
                req->hasParam("pid") ? req->getParam("pid")->value().c_str() : "-");
    String j;
    j.reserve(10240);
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      j += "{\"day\":";         j += G.dayCount;
      j += ",\"dayTick\":";     j += G.dayTick;
      j += ",\"tickId\":";      j += G.tickId;
      j += ",\"tc\":";          j += G.threatClock;
      j += ",\"weather\":";     j += G.weatherPhase;
      j += ",\"connected\":";   j += G.connectedCount;
      j += ",\"evtQueue\":";    j += pendingCount;
      // Event loss: evSeq is the last "sq" handed out, evtDrops how many of
      // those never made it onto the wire because the queue was full.
      j += ",\"evSeq\":";       j += g_evSeq;
      j += ",\"evtDrops\":";    j += g_evtDrops;
      j += ",\"pv\":";          j += PROTO_VERSION;
      // Why the board last booted, and the last crash it recorded (if any).
      // reset PANIC / *WDT with a small uptimeMs = it fell over mid-run.
      j += ",\"boot\":{\"reset\":\""; j += g_resetReason;
      j += "\",\"host\":\"";            j += MDNS_HOST; j += ".local\"";
      if (g_crash.valid) {
        char cb[200];
        int cn = snprintf(cb, sizeof(cb),
          ",\"crash\":{\"task\":\"%s\",\"pc\":\"0x%08lx\",\"cause\":%lu,"
          "\"vaddr\":\"0x%08lx\",\"elf\":\"%s\",\"bt\":[",
          g_crash.task, (unsigned long)g_crash.pc, (unsigned long)g_crash.cause,
          (unsigned long)g_crash.vaddr, g_crash.elf);
        for (int i = 0; i < g_crash.depth && cn < (int)sizeof(cb) - 16; i++)
          cn += snprintf(cb + cn, sizeof(cb) - cn, "%s\"0x%08lx\"",
                         i ? "," : "", (unsigned long)g_crash.bt[i]);
        j += cb; j += "]}";
      }
      j += "}";
      // Memory telemetry — lets a browser poll heap trend without a serial
      // monitor (see docs/dev-loop.md "Diagnosing HTTP stalls").
      j += ",\"mem\":{\"heap\":";    j += (uint32_t)ESP.getFreeHeap();
      j += ",\"minHeap\":";          j += (uint32_t)ESP.getMinFreeHeap();
      j += ",\"maxBlock\":";         j += (uint32_t)ESP.getMaxAllocHeap();
      j += ",\"psram\":";            j += (uint32_t)ESP.getFreePsram();
      j += ",\"uptimeMs\":";         j += (uint32_t)millis();
      j += ",\"uploadResumes\":";    j += g_uploadResumes;
      j += ",\"lastUploadErr\":\"";  j += g_uploadLastErr; j += "\"";
      // WS resilience telemetry — see docs/dev-loop.md "Diagnosing HTTP stalls"
      // sibling section for the WS equivalent. All near-zero in normal play.
      j += ",\"maxTickMs\":";        j += g_maxTickMs;
      j += ",\"broadcastSkips\":";   j += g_broadcastSkips;
      j += ",\"broadcastSkipsConsec\":"; j += g_broadcastSkipsConsec;
      j += ",\"broadcastPartial\":"; j += g_broadcastPartial;
      j += ",\"assetReqActive\":";   j += g_activeAssetReqs;
      j += ",\"assetReqRejects\":";  j += g_assetReqRejects;
      if (req->hasParam("sd")) {     // opt-in: f_getfree can take a while on big cards
        uint64_t tot = SD.totalBytes(), used = SD.usedBytes();
        j += ",\"sdTotal\":";        j += (uint32_t)(tot / 1024);
        j += ",\"sdUsedKB\":";       j += (uint32_t)(used / 1024);
        j += ",\"sdFreeKB\":";       j += (uint32_t)((tot - used) / 1024);
      }
      j += "}";
      {
        struct tm ti;
        bool ok = (getLocalTime(&ti, 0) && ti.tm_year > 100);
        j += ",\"rtc\":{\"synced\":"; j += ok ? "true" : "false";
        if (ok) {
          char ts[20]; strftime(ts, sizeof(ts), "%F %T", &ti);
          j += ",\"utc\":\""; j += ts; j += "\"";
          j += ",\"epoch\":"; j += (uint32_t)mktime(&ti);
        }
        j += "}";
      }

      int shelters = 0, impShelters = 0, poiCount = 0;
      int resCnt[6] = {0,0,0,0,0,0};
      int terrCnt[NUM_TERRAIN] = {};
      for (int row = 0; row < MAP_ROWS; row++) {
        for (int col = 0; col < MAP_COLS; col++) {
          const HexCell& c = G.map[row][col];
          if (c.shelter == 1) shelters++;
          else if (c.shelter == 2) impShelters++;
          if (c.resource > 0 && c.resource < 6) resCnt[c.resource] += c.amount;
          if (c.terrain < NUM_TERRAIN) terrCnt[c.terrain]++;
          if (c.poi) poiCount++;
        }
      }
      j += ",\"map\":{\"cells\":"; j += (MAP_ROWS * MAP_COLS);
      j += ",\"shelters\":";    j += shelters;
      j += ",\"impShelters\":"; j += impShelters;
      j += ",\"pois\":";        j += poiCount;
      j += ",\"res\":{\"water\":";  j += resCnt[1];
      j += ",\"food\":";  j += resCnt[2];
      j += ",\"fuel\":";  j += resCnt[3];
      j += ",\"med\":";   j += resCnt[4];
      j += ",\"scrap\":"; j += resCnt[5];
      j += "},\"terrain\":[";
      for (int t = 0; t < NUM_TERRAIN; t++) {
        if (t) j += ",";
        j += "{\"id\":"; j += t;
        j += ",\"name\":\""; j += T_SHORT[t]; j += "\"";
        j += ",\"count\":"; j += terrCnt[t];
        j += "}";
      }
      j += "]}";
      // Per-terrain art variant counts -- the same "vc" array sendLobbyMsg()
      // and syncMsg() put on the socket. /state had no equivalent, so an
      // HTTP-only client could not tell how many hex<Name><N>.png files exist
      // and had no safe way to wrap a variant it does not have art for.
      j += ",\"vc\":[";
      for (int t = 0; t < NUM_TERRAIN; t++) { if (t) j += ","; j += terrainVariantCount[t]; }
      j += "],\"sv\":[";
      for (int sIdx = 0; sIdx < 2; sIdx++) { if (sIdx) j += ","; j += shelterVariantCount[sIdx]; }
      j += "]";

      if (req->hasParam("pid")) {
        int vpid = req->getParam("pid")->value().toInt();
        if (vpid >= 0 && vpid < MAX_PLAYERS && G.players[vpid].connected) {
          const Player& vp = G.players[vpid];
          int visR; bool mr;
          playerVisParams(vpid, &visR, &mr);
          j += ",\"view\":{\"pid\":"; j += vpid;
          j += ",\"name\":\""; j += vp.name; j += "\"";
          j += ",\"q\":"; j += vp.q;
          j += ",\"r\":"; j += vp.r;
          j += ",\"visR\":"; j += visR;
          j += ",\"cells\":[";
          bool first = true;
          for (int dr = -visR; dr <= visR; dr++) {
            for (int dq = -visR; dq <= visR; dq++) {
              int s = -(dq + dr);
              if (abs(dq) + abs(dr) + abs(s) > 2 * visR) continue;
              int cq = wrapQ(vp.q + dq);
              int cr = wrapR(vp.r + dr);
              const HexCell& cell = G.map[cr][cq];
              uint8_t tt  = cell.terrain  < NUM_TERRAIN ? cell.terrain  : 0;
              uint8_t res = cell.resource < 6           ? cell.resource : 0;
              if (!first) j += ",";
              first = false;
              j += "{\"q\":"; j += cq;
              j += ",\"r\":"; j += cr;
              j += ",\"dq\":"; j += dq;
              j += ",\"dr\":"; j += dr;
              j += ",\"terrain\":"; j += tt;
              j += ",\"terrainName\":\""; j += TNAME_FULL[tt]; j += "\"";
              j += ",\"shelter\":"; j += cell.shelter;
              j += ",\"resource\":"; j += res;
              j += ",\"resourceName\":\""; j += RES_NAME_L[res]; j += "\"";
              j += ",\"amount\":"; j += cell.amount;
              j += ",\"footprints\":"; j += cell.footprints;
              j += ",\"tireTrack\":";  j += cell.tireTrack ? "true" : "false";
              // Which of /img/hex<Name><N>.png this hex is wearing. The game
              // client gets this in the packed map encoding; a spectator on
              // /state got nothing and had to draw flat colours. Also carries
              // the pinned landmark variants (hex-map.hpp Phase 5.5), which
              // is how POI_ART finds its one fixed image.
              j += ",\"variant\":";    j += cell.variant;
              j += ",\"poi\":";        j += cell.poi ? "true" : "false";
              j += "}";
            }
          }
          j += "]}";
        }
      }

      j += ",\"players\":[";
      for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player& p = G.players[i];
        if (i) j += ",";
        j += "{";
        j += "\"pid\":";          j += i;
        j += ",\"conn\":";        j += p.connected ? "true" : "false";
        j += ",\"wsClientId\":";  j += p.wsClientId;
        j += ",\"connectMs\":";   j += p.connectMs;
        j += ",\"lastMoveMs\":";  j += p.lastMoveMs;
        j += ",\"name\":\"";     j += p.name; j += "\"";
        j += ",\"arch\":";        j += p.archetype;
        j += ",\"archName\":\""; j += (p.archetype < NUM_ARCHETYPES ? ARCHETYPE_NAME[p.archetype] : "?"); j += "\"";
        j += ",\"invSlots\":";    j += p.invSlots;              // archetype base
        j += ",\"invSlotsEff\":"; j += effectiveInvSlots(p);    // base + equipment "slots"
        j += ",\"equip\":[";
        for (int s = 0; s < EQUIP_SLOTS; s++) { if (s) j += ","; j += p.equip[s]; }
        j += "]";
        j += ",\"q\":";           j += p.q;
        j += ",\"r\":";           j += p.r;
        j += ",\"ll\":";          j += p.ll;
        j += ",\"food\":";        j += p.food;
        j += ",\"water\":";       j += p.water;
        j += ",\"rad\":";         j += p.radiation;
        j += ",\"mp\":";          j += p.movesLeft;
        j += ",\"wounds\":[";     j += p.wounds[WOUND_MINOR]; j += ",";
                                   j += p.wounds[WOUND_MAJOR]; j += "]";
        j += ",\"resting\":";     j += p.resting       ? "true" : "false";
        j += ",\"radClean\":";    j += p.radClean      ? "true" : "false";
        j += ",\"fThreshBelow\":"; j += p.fThreshBelow;
        j += ",\"wThreshBelow\":"; j += p.wThreshBelow;
        j += ",\"skills\":[";
        for (int s = 0; s < NUM_SKILLS; s++) { if (s) j += ","; j += p.skills[s]; }
        j += "]";
        j += ",\"inv\":[";
        for (int s = 0; s < 5; s++) { if (s) j += ","; j += p.inv[s]; }
        j += "]";
        j += ",\"invType\":[";
        for (int s = 0; s < INV_SLOTS_MAX; s++) { if (s) j += ","; j += p.invType[s]; }
        j += "]";
        j += ",\"invQty\":[";
        for (int s = 0; s < INV_SLOTS_MAX; s++) { if (s) j += ","; j += p.invQty[s]; }
        j += "]";
        j += ",\"score\":";       j += p.score;
        j += ",\"steps\":";       j += p.steps;
        j += ",\"dp\":";          j += p.depth;   // 1 = in the tunnels (observer-fx.js, without the fx ring)
        j += ",\"llCap\":";       j += effectiveMaxLL(i);
        j += ",\"encActive\":";   j += encounters[i].active ? "true" : "false";
        if (encounters[i].active) {
          j += ",\"encQ\":";      j += encounters[i].hexQ;
          j += ",\"encR\":";      j += encounters[i].hexR;
          j += ",\"encNode\":\""; j += encounters[i].nodeKey; j += "\"";
          // Which scene this is, as the /enc route addresses it:
          // GET /enc?biome=<encBiome>&id=<encId>.  encNode alone says where in
          // a file the player stands but not which file, so without these two
          // a spectator can see that someone is in an encounter and nothing
          // about what it is -- no title, no prose, no choices, no hazards.
          // See docs/observer-screen-spec.md.
          {
            uint8_t et = encounters[i].terrain;
            j += ",\"encId\":";     j += encounters[i].encIdx;
            j += ",\"encBiome\":\"";
            if (et < NUM_TERRAIN) j += encPools[et].path;
            j += "\"";
          }
          j += ",\"encCanBank\":"; j += encounters[i].canBank ? "true" : "false";
          j += ",\"encLoot\":[";
          for (int s = 0; s < 5; s++) { if (s) j += ","; j += encounters[i].pendingLoot[s]; }
          j += "]";
        }
        j += "}";
      }
      j += "]";
      // The panels the LCD cut in, and how close the Doom is, so the observer
      // screen can cut in on the same beats (ui-fx.hpp, data/observer-fx.js).
      fxCueLogJson(j);
      j += "}";
      xSemaphoreGive(G.mutex);
    } else {
      Log.warning("HTTP /state mutex timeout");
      j = "{\"error\":\"mutex timeout\"}";
    }
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", j);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
  });

  // /enc?biome=X&id=Y — serve encounter JSON from SD card
  server.on("/enc", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("biome") || !req->hasParam("id")) {
      Log.warning("HTTP /enc 400 reason=missing");
      req->send(400, "text/plain", "Missing biome or id"); return;
    }
    String biome = req->getParam("biome")->value();
    String id    = req->getParam("id")->value();
    Log.notice("HTTP /enc biome=%s id=%s", biome.c_str(), id.c_str());
    // Validate: only allow alphanumeric + underscore in both params (prevent path traversal)
    for (unsigned i = 0; i < biome.length(); i++) {
      char c = biome[i];
      if (!isAlphaNumeric(c) && c != '_') {
        Log.warning("HTTP /enc 400 reason=invalid_biome=%s", biome.c_str());
        req->send(400, "text/plain", "Invalid biome"); return;
      }
    }
    for (unsigned i = 0; i < id.length(); i++) {
      char c = id[i];
      if (!isDigit(c)) {
        Log.warning("HTTP /enc 400 reason=invalid_id=%s", id.c_str());
        req->send(400, "text/plain", "Invalid id"); return;
      }
    }
    char path[56];
    snprintf(path, sizeof(path), "/data/encounters/%s/%s.json", biome.c_str(), id.c_str());
    uint32_t t0 = millis();
    // One f.read() into a PSRAM block. f.readString() grew a String one byte
    // at a time -- thousands of reallocs, every one under 4 KB landing on the
    // internal heap, all while holding G.mutex. The response owns the block
    // (shared_ptr captured by the filler) and frees it once sent.
    std::shared_ptr<uint8_t> body;
    size_t bodyLen = 0;
    bool found = false;
    bool mutexOk = (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    if (mutexOk) {
      uint32_t tsd = millis();
      bool exists = SD.exists(path);
      if (exists) {
        File f = SD.open(path, FILE_READ);
        if (f) {
          size_t fsz = f.size();
          uint8_t* blk = (uint8_t*)ps_malloc(fsz ? fsz : 1);
          if (blk) {
            body.reset(blk, free);
            bodyLen = f.read(blk, fsz);
            found = true;
          } else {
            Log.error("HTTP /enc: ps_malloc(%u) failed", (unsigned)fsz);
          }
          f.close();
          Log.notice("SD READ: %s size=%u took=%ums",
                     path, (unsigned)fsz, (unsigned)(millis() - tsd));
        } else {
          Log.error("SD OPEN FAIL: %s", path);
        }
      } else {
        Log.warning("SD MISSING: %s (via /enc)", path);
      }
      xSemaphoreGive(G.mutex);
    } else {
      Log.warning("HTTP /enc 503: mutex timeout path=%s", path);
      req->send(503, "text/plain", "Server busy"); return;
    }
    if (!found) {
      Log.warning("HTTP /enc 404 path=%s", path);
      req->send(404, "text/plain", "Encounter not found"); return;
    }
    LOG_VERBOSE("HTTP /enc 200 path=%s len=%u took=%ums",
                path, (unsigned)bodyLen, (unsigned)(millis() - t0));
    req->send(req->beginResponse("application/json", bodyLen,
        [body, bodyLen](uint8_t* out, size_t maxLen, size_t index) -> size_t {
          size_t n = (index < bodyLen) ? bodyLen - index : 0;
          if (n > maxLen) n = maxLen;
          memcpy(out, body.get() + index, n);
          return n;
        }));
  });

  // /img/*.png served from PSRAM imgCache
  server.onNotFound([](AsyncWebServerRequest* req) {
    String url = req->url();
    LOG_VERBOSE("HTTP 404-check %s", url.c_str());
    if (url.startsWith("/img/")) {
      if (!admitAssetRequest(req)) return;
      String filename = url.substring(5);
      for (int i = 0; i < imgCacheCount; i++) {
        if (filename.equalsIgnoreCase(imgCache[i].name)) {
          String mimeType = "image/png";
          if (filename.endsWith(".jpg") || filename.endsWith(".jpeg") ||
              filename.endsWith(".JPG") || filename.endsWith(".JPEG"))
            mimeType = "image/jpeg";
          else if (filename.endsWith(".webp"))
            mimeType = "image/webp";
          else if (filename.endsWith(".json"))
            mimeType = "application/json";
          // tiles.json names the current atlas build, so it revalidates
          // (cheap: 304 on the ETag); everything else is requested
          // versioned or never changes, and stays pinned for a year.
          const char* cc = filename.endsWith(".json") ? "no-cache" : "public, max-age=31536000, immutable";
          if (req->hasHeader("If-None-Match") &&
              req->getHeader("If-None-Match")->value() == imgCache[i].etag) {
            LOG_VERBOSE("HTTP 304 /img/%s", filename.c_str());
            AsyncWebServerResponse* r = req->beginResponse(304);
            r->addHeader("ETag", imgCache[i].etag);
            r->addHeader("Cache-Control", cc);
            req->send(r);
            return;
          }
          LOG_VERBOSE("HTTP /img/ hit %s (%u B) heap=%uKB", filename.c_str(),
                      (unsigned)imgCache[i].len, (unsigned)(ESP.getFreeHeap() / 1024));
          AsyncWebServerResponse* resp = req->beginResponse(
              200, mimeType, imgCache[i].buf, imgCache[i].len);
          resp->addHeader("ETag", imgCache[i].etag);
          resp->addHeader("Cache-Control", cc);
          req->send(resp);
          return;
        }
      }
      Log.warning("IMG MISSING: %s (served 204)", filename.c_str());
      req->send(204); return;
    }
    Log.warning("HTTP 404 %s", url.c_str());
    req->send(404, "text/plain", "Not found: " + url);
  });

  // POST /upload?dest=/data/<path>
  // Two body shapes reach us and ESPAsyncWebServer dispatches them to
  // DIFFERENT callbacks:
  //   * raw body (sync_data.ps1 / .sh: Content-Type application/octet-stream,
  //     curl --data-binary)            -> onBody   (arg 5 below)
  //   * multipart/form-data file part   -> onUpload (arg 4 below)
  // Until 2026-09-12 only onUpload was registered, so every sync_data run got
  // "OK" while nothing was written to the SD card. Both now feed one writer.
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      bool ok = g_uploadOk;
      Log.notice("UPLOAD request dest=%s ok=%d bytes=%u",
                 request->hasParam("dest") ? request->getParam("dest")->value().c_str() : "-",
                 (int)ok, (unsigned)g_uploadTotal);
      // Body is "OK <bytes written>" so sync_data.ps1 can verify the count
      // against the local file size — a short write is never silently "OK".
      char body[48];
      if (ok) snprintf(body, sizeof(body), "OK %u", (unsigned)g_uploadTotal);
      else    snprintf(body, sizeof(body), "SD write failed after %u", (unsigned)g_uploadTotal);
      request->send(ok ? 200 : 500, "text/plain", body);
    },
    [](AsyncWebServerRequest* request, const String& filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      uploadChunk(request, filename, index, data, len, final);
    },
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len,
       size_t index, size_t total) {
      uploadChunk(request, String(), index, data, len, index + len >= total);
    }
  );

  server.begin();
  Log.notice("HTTP server listening port=80 heap=%uKB",
             (unsigned)(ESP.getFreeHeap() / 1024));
  { char hb[30]; snprintf(hb, 30, "Heap: %ukB free", (unsigned)(ESP.getFreeHeap()/1024));
    splashAdd("HTTP+WS ready", 0x60A040);
    splashAdd(hb, 0x406030); }
}
