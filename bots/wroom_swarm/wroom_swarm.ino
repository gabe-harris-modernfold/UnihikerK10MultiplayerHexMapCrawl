// wroom_swarm -- fill every K10 seat from one ESP32-WROOM-32E ("the wroom")
// joined to the K10's own softAP, play a slow random walk on each seat, and
// measure what direct-connect play does to the link. No router anywhere in the
// path: the wroom talks to 192.168.47.1 over WASTELAND exactly as a phone that
// joined the board's hotspot would.
//
// Build/flash (ESP32 core 3.x, not build.ps1 -- that one is the K10's):
//   arduino-cli compile --fqbn esp32:esp32:esp32 --build-path <dir> bots/wroom_swarm
//   arduino-cli upload  --fqbn esp32:esp32:esp32 --input-dir <dir> -p COM3
//
// Serial, 115200: an "R {...}" report line every REPORT_MS, "E ..." lines for
// anything that went wrong, and the commands in handleSerial(). swarm_log.py
// logs this port and the K10's together and summarises a run.
//
// The protocol rules are docs/bot-testing.md's: the archetype index is the
// seat, a pick is confirmed only by the unicast sync, every rid gets exactly
// one ack/nack, and a death moves the socket to the lobby (reconnect = respawn).

#include <WiFi.h>
#include <esp_wifi.h>

static const char* AP_SSID = "WASTELAND";
static const IPAddress K10_IP(192, 168, 47, 1);
static constexpr uint16_t K10_PORT = 80;

static constexpr int      MAX_SEATS      = 6;      // archetypes on the K10 (MAX_PLAYERS)
static constexpr int      DEFAULT_SEATS  = 5;      // MAX_SEATED: the table is full at 5
static constexpr uint32_t PER_SEAT_MS    = 300;    // bots/ --min-interval
static constexpr uint32_t GLOBAL_MS      = 120;    // bots/ --global-interval
static constexpr uint32_t MOVE_WAIT_MS   = 2000;   // one move in flight, as BotClient
static constexpr uint32_t REST_COOL_MS   = 4000;   // REST is retried, never latched
static constexpr uint32_t ABORT_COOL_MS  = 3000;
static constexpr uint32_t RID_TIMEOUT_MS = 10000;  // bots/ "unanswered"
static constexpr uint32_t STALL_MS       = 30000;  // no bytes at all: drop the socket
static constexpr uint32_t JOIN_WAIT_MS   = 10000;  // lobby or sync never came
static constexpr uint32_t SEAT_GONE_MS   = 3000;   // our block missing from `s` this long
static constexpr uint32_t REPORT_MS      = 10000;
static constexpr uint32_t STATE_POLL_MS  = 10000;
static constexpr size_t   RX_CAP         = 12 * 1024;   // an `s` tick is ~3-4 KB
static constexpr int      RID_SLOTS      = 16;
static constexpr int      RTT_BUCKETS    = 8;
static const uint32_t     RTT_EDGE[RTT_BUCKETS - 1] = {25, 50, 100, 250, 500, 1000, 2500};

// ── Per-seat counters ────────────────────────────────────────────────────────
// Cumulative unless marked "interval" (reset by every report).
struct Stats {
  uint32_t connects, hsFail, tcpFail, full, closeFrm, tcpLost, stalls, seatGone, joinFail;
  uint32_t respawns, syncs, rx, ticks, tickMiss, tickBack, gap500, gap2s, tx;
  uint32_t acks, nacks, unanswered, lostOnDrop, moves, trunc, maxMsg;
  uint64_t rxBytes, rttSum;
  uint32_t rttN, seatedMs;
  uint32_t maxGapI, rttMaxI;                  // interval
};

enum SeatSt : uint8_t { S_OFF, S_LOBBY, S_PICKING, S_PLAY };

struct Seat {
  WiFiClient tcp;
  SeatSt st = S_OFF;
  int arch = -1, want = -1;
  uint32_t nextTry = 0, backoff = 1000, stT = 0, lastRx = 0, joinMs = 0;
  uint32_t lastTk = 0, lastTickMs = 0, goneSince = 0, fellAt = 0;
  bool haveTk = false;
  // WebSocket frame parser
  uint8_t ph = 0, hn = 0, hneed = 2, op = 0, msgOp = 0, mk[4];
  uint8_t hb[14];
  bool fin = false, masked = false, trunc = false;
  uint64_t plen = 0, pgot = 0, msgBytes = 0;
  char* buf = nullptr;
  size_t bn = 0;
  uint8_t cb[128];
  uint8_t cn = 0;
  // what the last tick said about us
  int ap = 0, vm = 0, rt = 0, enc = 0, dp = 0, ll = 1, lastDir = -1;
  uint32_t downSince = 0;
  uint32_t lastSend = 0, moveSent = 0, restSent = 0, abortSent = 0;
  // requests in flight
  uint32_t ridId[RID_SLOTS], ridT[RID_SLOTS];
  char ridCmd[RID_SLOTS][10];
  Stats s;
};

static Seat     seats[MAX_SEATS];
static int      g_seats   = DEFAULT_SEATS;
static bool     g_run     = true;
static uint32_t g_rid     = 1;
static uint32_t g_lastSend = 0;
static long     g_pv = -1, g_linkAp = -1, g_linkCap = -1;
static uint32_t g_rttHist[RTT_BUCKETS];
static uint32_t g_wifiDrops = 0, g_wifiReason = 0, g_wifiUpMs = 0, g_wifiDownMs = 0, g_wifiDownTotal = 0;
static uint32_t g_minHeap = UINT32_MAX;
static uint8_t  g_rxTmp[2048];

struct Why { char w[14]; uint32_t n; };
static Why g_why[16];
static int g_whyN = 0;

// ── K10 /state, polled from its own task so a slow HTTP reply never stalls the
// sockets. Every HTTP request is a fresh TCP connection on the K10 (no
// keep-alive), so this is also what a page load costs it.
struct K10State {
  uint32_t ok, fail, lastMs, maxMs, reboots;
  long heap, minHeap, maxBlock, maxTick, partial, skips, connected, evtDrops, uptime, pv, day;
  char reset[16];
};
static K10State g_k10 = {};
static portMUX_TYPE g_k10Mux = portMUX_INITIALIZER_UNLOCKED;
static char g_stateBuf[12 * 1024];

static long jnum(const char* b, const char* key, long dflt = -1) {
  const char* p = strstr(b, key);
  return p ? strtol(p + strlen(key), nullptr, 10) : dflt;
}

static bool jstr(const char* b, const char* key, char* out, size_t cap) {
  const char* p = strstr(b, key);
  size_t n = 0;
  if (p)
    for (p += strlen(key); *p && *p != '"' && n + 1 < cap; ) out[n++] = *p++;
  out[n] = 0;
  return p != nullptr;
}

static void countWhy(const char* why) {
  for (int i = 0; i < g_whyN; i++)
    if (!strcmp(g_why[i].w, why)) { g_why[i].n++; return; }
  if (g_whyN < (int)(sizeof(g_why) / sizeof(g_why[0]))) {
    strlcpy(g_why[g_whyN].w, why, sizeof(g_why[0].w));
    g_why[g_whyN++].n = 1;
  }
}

static void stateTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(STATE_POLL_MS));
    if (WiFi.status() != WL_CONNECTED) continue;
    WiFiClient c;
    uint32_t t0 = millis();
    size_t n = 0;
    bool ok = c.connect(K10_IP, K10_PORT, 3000);
    if (ok) {
      c.print("GET /state HTTP/1.1\r\nHost: 192.168.47.1\r\nConnection: close\r\n\r\n");
      while (millis() - t0 < 5000 && n < sizeof(g_stateBuf) - 1) {
        int a = c.available();
        if (a > 0) n += c.read((uint8_t*)g_stateBuf + n, min((size_t)a, sizeof(g_stateBuf) - 1 - n));
        else if (!c.connected()) break;
        else vTaskDelay(pdMS_TO_TICKS(5));
      }
      c.stop();
    }
    g_stateBuf[n] = 0;
    uint32_t ms = millis() - t0;
    const char* body = strstr(g_stateBuf, "\r\n\r\n");
    ok = ok && body && strstr(body, "\"mem\"");
    portENTER_CRITICAL(&g_k10Mux);
    if (!ok) {
      g_k10.fail++;
    } else {
      long up = jnum(body, "\"uptimeMs\":");
      if (g_k10.ok && up >= 0 && up < g_k10.uptime) g_k10.reboots++;
      g_k10.ok++;
      g_k10.lastMs = ms;
      if (ms > g_k10.maxMs) g_k10.maxMs = ms;
      g_k10.uptime    = up;
      g_k10.heap      = jnum(body, "\"heap\":");
      g_k10.minHeap   = jnum(body, "\"minHeap\":");
      g_k10.maxBlock  = jnum(body, "\"maxBlock\":");
      g_k10.maxTick   = jnum(body, "\"maxTickMs\":");
      g_k10.partial   = jnum(body, "\"broadcastPartial\":");
      g_k10.skips     = jnum(body, "\"broadcastSkips\":");
      g_k10.connected = jnum(body, "\"connected\":");
      g_k10.evtDrops  = jnum(body, "\"evtDrops\":");
      g_k10.pv        = jnum(body, "\"pv\":");
      g_k10.day       = jnum(body, "\"day\":");
      jstr(body, "\"reset\":\"", g_k10.reset, sizeof(g_k10.reset));
    }
    portEXIT_CRITICAL(&g_k10Mux);
  }
}

// ── WebSocket client (RFC 6455, just enough of it) ───────────────────────────
static bool sendFrame(Seat& z, uint8_t opcode, const uint8_t* p, size_t n) {
  uint8_t f[300];
  if (n > sizeof(f) - 8) return false;
  size_t h = 0;
  f[h++] = 0x80 | opcode;
  if (n < 126) f[h++] = 0x80 | (uint8_t)n;
  else { f[h++] = 0x80 | 126; f[h++] = n >> 8; f[h++] = n & 0xFF; }
  uint32_t m = esp_random();
  uint8_t* mk = f + h;
  memcpy(mk, &m, 4); h += 4;
  for (size_t i = 0; i < n; i++) f[h + i] = p[i] ^ mk[i & 3];
  return z.tcp.write(f, h + n) == h + n;
}

static void dropSeat(int i, const char* why, bool fault);

// Every request carries a rid, so the K10 owes it exactly one ack or nack.
static void request(int i, const char* cmd, const char* fmt, ...) {
  Seat& z = seats[i];
  char body[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(body, sizeof(body) - 24, fmt, ap);
  va_end(ap);
  uint32_t rid = g_rid++;
  n += snprintf(body + n - 1, sizeof(body) - n, ",\"rid\":%lu}", (unsigned long)rid) - 1;
  for (int k = 0; k < RID_SLOTS; k++) {
    if (z.ridId[k]) continue;
    z.ridId[k] = rid; z.ridT[k] = millis();
    strlcpy(z.ridCmd[k], cmd, sizeof(z.ridCmd[k]));
    break;
  }
  if (!sendFrame(z, 0x1, (const uint8_t*)body, n)) { dropSeat(i, "send_fail", true); return; }
  z.s.tx++;
  z.lastSend = g_lastSend = millis();
}

static void pickSeat(int i, const char* b) {
  Seat& z = seats[i];
  const char* a = strstr(b, "\"avail\":[");
  if (!a) return;
  bool avail[MAX_SEATS] = {};
  for (a += 9; *a && *a != ']'; a++)
    if (*a >= '0' && *a < '0' + MAX_SEATS) avail[*a - '0'] = true;
  for (int k = 0; k < MAX_SEATS; k++)   // never two of ours on one archetype
    if (k != i && seats[k].st != S_OFF && seats[k].arch >= 0) avail[seats[k].arch] = false;
  int pick = (z.want >= 0 && avail[z.want]) ? z.want : -1;
  for (int k = 0; pick < 0 && k < MAX_SEATS; k++) if (avail[k]) pick = k;
  if (pick < 0) { dropSeat(i, "no_slot", true); return; }
  z.arch = pick;
  z.st = S_PICKING; z.stT = millis();
  request(i, "pick", "{\"t\":\"pick\",\"arch\":%d}", pick);
}

static void onTick(int i, const char* b, uint32_t now) {
  Seat& z = seats[i];
  uint32_t tk = (uint32_t)jnum(b, "\"tk\":", 0);
  if (z.haveTk) {
    if (tk > z.lastTk + 1) z.s.tickMiss += tk - z.lastTk - 1;
    else if (tk <= z.lastTk) z.s.tickBack++;
    uint32_t gap = now - z.lastTickMs;
    if (gap > z.s.maxGapI) z.s.maxGapI = gap;
    if (gap > 500) z.s.gap500++;
    if (gap > 2000) z.s.gap2s++;
  }
  z.haveTk = true; z.lastTk = tk; z.lastTickMs = now;
  z.s.ticks++;
  if (z.st != S_PLAY) return;
  // Protocol 4: only seated survivors are listed, each tagged with its seat.
  char pat[24];
  snprintf(pat, sizeof(pat), "{\"id\":%d,\"q\":", z.arch);
  const char* me = strstr(b, pat);
  if (!me) {
    if (z.fellAt) { z.s.respawns++; dropSeat(i, "respawn", false); return; }
    if (!z.goneSince) z.goneSince = now;
    else if (now - z.goneSince > SEAT_GONE_MS) { z.s.seatGone++; dropSeat(i, "seat_gone", true); }
    return;
  }
  z.goneSince = 0;
  z.ap  = jnum(me, "\"ap\":", 0);
  z.vm  = jnum(me, "\"vm\":", 0);
  z.rt  = jnum(me, "\"rt\":", 0);
  z.enc = jnum(me, "\"enc\":", 0);
  z.dp  = jnum(me, "\"dp\":", 0);
  z.ll  = jnum(me, "\"ll\":", 1);
  if (z.fellAt && now - z.fellAt > 15000) z.fellAt = 0;   // a dawn heal revived us in place
  // A downed survivor keeps its seat at LL 0 and every move is nacked. As in
  // BotClient: wait long enough for a dawn heal, then re-pick (= respawn).
  if (z.ll > 0) z.downSince = 0;
  else if (!z.downSince) z.downSince = now;
  else if (now - z.downSince > 15000) { z.s.respawns++; dropSeat(i, "respawn", false); }
}

static void onReply(int i, const char* b, bool ok) {
  Seat& z = seats[i];
  uint32_t rid = (uint32_t)jnum(b, "\"rid\":", 0);
  char cmd[10], why[14] = "";
  jstr(b, "\"cmd\":\"", cmd, sizeof(cmd));
  if (ok) z.s.acks++;
  else { z.s.nacks++; jstr(b, "\"why\":\"", why, sizeof(why)); countWhy(why); }
  for (int k = 0; k < RID_SLOTS; k++) {
    if (z.ridId[k] != rid) continue;
    uint32_t rtt = millis() - z.ridT[k];
    z.ridId[k] = 0;
    z.s.rttN++; z.s.rttSum += rtt;
    if (rtt > z.s.rttMaxI) z.s.rttMaxI = rtt;
    int bkt = 0;
    while (bkt < RTT_BUCKETS - 1 && rtt >= RTT_EDGE[bkt]) bkt++;
    g_rttHist[bkt]++;
    break;
  }
  if (!strcmp(cmd, "m") && !ok) z.moveSent = 0;
  if (!strcmp(cmd, "pick") && !ok) {
    z.s.joinFail++;
    char why2[24];
    snprintf(why2, sizeof(why2), "pick_nack_%s", why);
    dropSeat(i, why2, true);
  }
}

static void onMessage(int i) {
  Seat& z = seats[i];
  uint32_t now = millis();
  z.s.rx++;
  z.s.rxBytes += z.msgBytes;
  if (z.msgBytes > z.s.maxMsg) z.s.maxMsg = z.msgBytes;
  if (z.trunc) z.s.trunc++;
  if (z.msgOp != 0x1) return;
  const char* b = z.buf;
  char t[12];
  jstr(b, "\"t\":\"", t, sizeof(t));
  if (!strcmp(t, "s")) { onTick(i, b, now); return; }
  if (!strcmp(t, "ack"))  { onReply(i, b, true); return; }
  if (!strcmp(t, "nack")) { onReply(i, b, false); return; }
  if (!strcmp(t, "ev")) {
    char k[12];
    jstr(b, "\"k\":\"", k, sizeof(k));
    if (jnum(b, "\"pid\":", -9) != z.arch || z.st != S_PLAY) return;
    if (!strcmp(k, "mv")) { z.moveSent = 0; z.s.moves++; }
    else if (!strcmp(k, "downed")) {
      // A death moves this socket to the lobby (EVT_DOWNED). Respawn the way
      // the browser does: re-pick on the same socket when the lobby update
      // that follows arrives, so a seat the K10 failed to free shows up here
      // as a refused pick rather than being hidden behind a reconnect.
      z.s.respawns++;
      z.s.seatedMs += now - z.joinMs;
      z.want = z.arch; z.arch = -1;
      z.st = S_LOBBY; z.stT = now;
      z.fellAt = 0; z.downSince = 0; z.moveSent = 0;
      Serial.printf("E %lu seat=%d downed, re-picking arch=%d\n", (unsigned long)now, i, z.want);
    }
    return;
  }
  if (!strcmp(t, "lobby")) { if (z.st == S_LOBBY) pickSeat(i, b); return; }
  if (!strcmp(t, "sync")) {
    if (z.st != S_PICKING) return;
    long id = jnum(b, "\"id\":");
    if (id >= 0) z.arch = (int)id;
    g_pv = jnum(b, "\"pv\":");
    z.want = z.arch;
    z.st = S_PLAY; z.joinMs = now; z.s.syncs++;
    z.fellAt = 0; z.goneSince = 0; z.backoff = 1000;
    Serial.printf("E %lu seat=%d joined arch=%d sync=%luB\n", (unsigned long)now, i, z.arch,
                  (unsigned long)z.msgBytes);
    return;
  }
  if (!strcmp(t, "full")) { z.s.full++; dropSeat(i, "full", true); return; }
  if (!strcmp(t, "wifi")) {
    char st[8];
    jstr(b, "\"status\":\"", st, sizeof(st));
    if (!strcmp(st, "link")) { g_linkAp = jnum(b, "\"ap\":"); g_linkCap = jnum(b, "\"cap\":"); }
  }
}

static void endFrame(int i) {
  Seat& z = seats[i];
  z.ph = 0;
  if (z.op >= 0x8) {
    if (z.op == 0x9) sendFrame(z, 0xA, z.cb, z.cn);       // ping -> pong, same payload
    else if (z.op == 0x8) {
      int code = z.cn >= 2 ? (z.cb[0] << 8) | z.cb[1] : 0;
      char why[24];
      snprintf(why, sizeof(why), "close_%d", code);
      z.s.closeFrm++;
      dropSeat(i, why, true);
    }
    return;
  }
  if (!z.fin) return;
  z.buf[z.bn] = 0;
  onMessage(i);
  z.bn = 0; z.msgBytes = 0; z.trunc = false;
}

static void startPayload(int i) {
  Seat& z = seats[i];
  z.fin = z.hb[0] & 0x80;
  z.op = z.hb[0] & 0x0F;
  z.masked = z.hb[1] & 0x80;
  uint8_t l7 = z.hb[1] & 0x7F, h = 2;
  if (l7 == 126) { z.plen = (z.hb[2] << 8) | z.hb[3]; h = 4; }
  else if (l7 == 127) { z.plen = 0; for (int k = 0; k < 8; k++) z.plen = (z.plen << 8) | z.hb[2 + k]; h = 10; }
  else z.plen = l7;
  if (z.masked) memcpy(z.mk, z.hb + h, 4);
  z.pgot = 0; z.hn = 0; z.hneed = 2; z.ph = 1;
  if (z.op >= 0x8) z.cn = 0;
  else if (z.op != 0x0) { z.msgOp = z.op; z.bn = 0; z.msgBytes = 0; z.trunc = false; }
  if (z.plen == 0) endFrame(i);
}

static void feed(int i, const uint8_t* d, size_t n) {
  Seat& z = seats[i];
  size_t k = 0;
  while (k < n && z.st != S_OFF) {
    if (z.ph == 0) {
      z.hb[z.hn++] = d[k++];
      if (z.hn == 2) {
        uint8_t l7 = z.hb[1] & 0x7F;
        z.hneed = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + ((z.hb[1] & 0x80) ? 4 : 0);
      }
      if (z.hn >= 2 && z.hn == z.hneed) startPayload(i);
      continue;
    }
    size_t take = (size_t)min((uint64_t)(n - k), z.plen - z.pgot);
    for (size_t j = 0; j < take; j++) {
      uint8_t c = d[k + j];
      if (z.masked) c ^= z.mk[(z.pgot + j) & 3];
      if (z.op >= 0x8) { if (z.cn < sizeof(z.cb)) z.cb[z.cn++] = c; }
      else if (z.bn < RX_CAP - 1) z.buf[z.bn++] = (char)c;
      else z.trunc = true;
    }
    if (z.op < 0x8) z.msgBytes += take;
    k += take; z.pgot += take;
    if (z.pgot == z.plen) endFrame(i);
  }
}

// ── Seat lifecycle ───────────────────────────────────────────────────────────
static void dropSeat(int i, const char* why, bool fault) {
  Seat& z = seats[i];
  if (z.st == S_OFF) return;
  uint32_t now = millis();
  int pending = 0;
  for (int k = 0; k < RID_SLOTS; k++) if (z.ridId[k]) { pending++; z.ridId[k] = 0; }
  z.s.lostOnDrop += pending;
  if (z.st == S_PLAY) z.s.seatedMs += now - z.joinMs;
  Serial.printf("E %lu seat=%d arch=%d drop=%s st=%d pending=%d seated=%lus\n", (unsigned long)now, i,
                z.arch, why, (int)z.st, pending,
                (unsigned long)(z.st == S_PLAY ? (now - z.joinMs) / 1000 : 0));
  z.tcp.stop();
  z.st = S_OFF;
  if (z.arch >= 0) z.want = z.arch;
  z.arch = -1;
  z.haveTk = false; z.fellAt = 0; z.goneSince = 0; z.moveSent = 0; z.downSince = 0; z.ll = 1;
  z.ph = 0; z.hn = 0; z.hneed = 2; z.bn = 0;
  z.nextTry = now + (fault ? z.backoff : 1000);
  if (fault) z.backoff = min<uint32_t>(z.backoff * 2, 15000);
}

static bool openSeat(int i) {
  Seat& z = seats[i];
  z.s.connects++;
  if (!z.tcp.connect(K10_IP, K10_PORT, 3000)) {
    z.s.tcpFail++;
    z.nextTry = millis() + z.backoff;
    z.backoff = min<uint32_t>(z.backoff * 2, 15000);
    Serial.printf("E %lu seat=%d tcp_connect_fail\n", (unsigned long)millis(), i);
    return false;
  }
  z.tcp.setNoDelay(true);
  uint8_t key[16];
  for (int k = 0; k < 16; k += 4) { uint32_t r = esp_random(); memcpy(key + k, &r, 4); }
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char k64[25];
  int o = 0;
  for (int k = 0; k < 15; k += 3) {
    uint32_t v = (key[k] << 16) | (key[k + 1] << 8) | key[k + 2];
    k64[o++] = B64[(v >> 18) & 63]; k64[o++] = B64[(v >> 12) & 63];
    k64[o++] = B64[(v >> 6) & 63];  k64[o++] = B64[v & 63];
  }
  uint32_t v = key[15] << 16;
  k64[o++] = B64[(v >> 18) & 63]; k64[o++] = B64[(v >> 12) & 63]; k64[o++] = '='; k64[o++] = '=';
  k64[o] = 0;
  z.tcp.printf("GET /ws HTTP/1.1\r\nHost: 192.168.47.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", k64);
  // Headers byte by byte: the K10 sends its first frames straight after the
  // 101, often in the same segment, and they must stay in the socket.
  char hdr[256];
  size_t hn = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 4000) {
    if (z.tcp.available()) {
      int c = z.tcp.read();
      if (hn < sizeof(hdr) - 1) hdr[hn++] = (char)c;
      if (hn >= 4 && !memcmp(hdr + hn - 4, "\r\n\r\n", 4)) break;
    } else if (!z.tcp.connected()) break;
    else delay(1);
  }
  hdr[hn] = 0;
  if (!strstr(hdr, " 101")) {
    z.s.hsFail++;
    z.tcp.stop();
    z.nextTry = millis() + z.backoff;
    z.backoff = min<uint32_t>(z.backoff * 2, 15000);
    Serial.printf("E %lu seat=%d ws_handshake_fail after=%lums\n", (unsigned long)millis(), i,
                  (unsigned long)(millis() - t0));
    return false;
  }
  z.st = S_LOBBY; z.stT = z.lastRx = millis();
  z.ph = 0; z.hn = 0; z.hneed = 2; z.bn = 0; z.msgBytes = 0; z.trunc = false;
  return true;
}

// millis(), not the loop's `now`: another seat may have sent since it was read,
// and an unsigned now - g_lastSend would then wrap and skip the global pacing.
static void decide(int i) {
  Seat& z = seats[i];
  uint32_t now = millis();
  if (now - z.lastSend < PER_SEAT_MS || now - g_lastSend < GLOBAL_MS || !z.haveTk) return;
  if (z.fellAt || z.ll == 0) return;
  if (z.enc) {   // a booby trap, Kane or VACANCY opened a scene on arrival
    if (now - z.abortSent > ABORT_COOL_MS) { z.abortSent = now; request(i, "enc_abort", "{\"t\":\"enc_abort\"}"); }
    return;
  }
  if (z.rt) return;
  if (z.moveSent && now - z.moveSent < MOVE_WAIT_MS) return;
  if (z.ap > 0 && z.vm) {
    int d = z.lastDir;
    if (d < 0 || !(z.vm & (1 << d)) || esp_random() % 100 < 35) {
      int legal[6], n = 0;
      for (int k = 0; k < 6; k++) if (z.vm & (1 << k)) legal[n++] = k;
      d = legal[esp_random() % n];
    }
    z.lastDir = d; z.moveSent = now;
    request(i, "m", "{\"t\":\"m\",\"d\":%d}", d);
    return;
  }
  if (now - z.restSent > REST_COOL_MS) { z.restSent = now; request(i, "act", "{\"t\":\"act\",\"a\":7,\"ap\":1}"); }
}

static void service(int i) {
  Seat& z = seats[i];
  if (z.st == S_OFF) return;
  for (int guard = 0; guard < 16 && z.st != S_OFF; guard++) {
    int a = z.tcp.available();
    if (a <= 0) break;
    int r = z.tcp.read(g_rxTmp, min((size_t)a, sizeof(g_rxTmp)));
    if (r <= 0) break;
    z.lastRx = millis();
    feed(i, g_rxTmp, (size_t)r);
  }
  if (z.st == S_OFF) return;
  // Read after the messages above were handled: they stamp stT and ridT from
  // millis(), and an earlier `now` made those differences wrap -- a pick
  // counted unanswered, a re-pick timed out the instant it was sent.
  uint32_t now = millis();
  if (!z.tcp.connected()) { z.s.tcpLost++; dropSeat(i, "tcp_lost", true); return; }
  if (now - z.lastRx > STALL_MS) { z.s.stalls++; dropSeat(i, "stall", true); return; }
  if ((z.st == S_LOBBY || z.st == S_PICKING) && now - z.stT > JOIN_WAIT_MS) {
    z.s.joinFail++; dropSeat(i, z.st == S_LOBBY ? "no_lobby" : "pick_timeout", true); return;
  }
  for (int k = 0; k < RID_SLOTS; k++) {
    if (z.ridId[k] && now - z.ridT[k] > RID_TIMEOUT_MS) {
      z.s.unanswered++;
      Serial.printf("E %lu seat=%d unanswered cmd=%s rid=%lu\n", (unsigned long)now, i, z.ridCmd[k],
                    (unsigned long)z.ridId[k]);
      if (!strcmp(z.ridCmd[k], "m")) z.moveSent = 0;
      z.ridId[k] = 0;
    }
  }
  if (z.st == S_PLAY) decide(i);
}

// ── Report ───────────────────────────────────────────────────────────────────
static void report() {
  static char out[4096];
  uint32_t now = millis();
  int n = 0;
  wifi_ap_record_t ap = {};
  bool assoc = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  uint32_t heap = ESP.getFreeHeap();
  if (heap < g_minHeap) g_minHeap = heap;
  n += snprintf(out + n, sizeof(out) - n,
    "R {\"ms\":%lu,\"run\":%d,\"wifi\":{\"up\":%d,\"rssi\":%d,\"ch\":%d,\"drops\":%lu,\"reason\":%lu,\"downMs\":%lu},"
    "\"heap\":%lu,\"minHeap\":%lu,\"pv\":%ld,\"link\":{\"ap\":%ld,\"cap\":%ld},\"seats\":[",
    (unsigned long)now, g_run ? 1 : 0, assoc ? 1 : 0, assoc ? ap.rssi : 0, assoc ? ap.primary : 0,
    (unsigned long)g_wifiDrops, (unsigned long)g_wifiReason,
    (unsigned long)(g_wifiDownTotal + (g_wifiDownMs ? now - g_wifiDownMs : 0)),
    (unsigned long)heap, (unsigned long)g_minHeap, g_pv, g_linkAp, g_linkCap);
  for (int i = 0; i < g_seats; i++) {
    Seat& z = seats[i];
    Stats& s = z.s;
    uint32_t seated = s.seatedMs + (z.st == S_PLAY ? now - z.joinMs : 0);
    n += snprintf(out + n, sizeof(out) - n,
      "%s{\"a\":%d,\"st\":%d,\"c\":%lu,\"hs\":%lu,\"tf\":%lu,\"fl\":%lu,\"cf\":%lu,\"tl\":%lu,\"sk\":%lu,"
      "\"sg\":%lu,\"jf\":%lu,\"rs\":%lu,\"sy\":%lu,\"rx\":%lu,\"kb\":%lu,\"tk\":%lu,\"tm\":%lu,\"tb\":%lu,"
      "\"g5\":%lu,\"g2\":%lu,\"mg\":%lu,\"tx\":%lu,\"ak\":%lu,\"nk\":%lu,\"un\":%lu,\"lo\":%lu,"
      "\"rtt\":%lu,\"rm\":%lu,\"mv\":%lu,\"tr\":%lu,\"mm\":%lu,\"sd\":%lu,\"ap\":%d,\"dp\":%d}",
      i ? "," : "", z.arch, (int)z.st, (unsigned long)s.connects, (unsigned long)s.hsFail,
      (unsigned long)s.tcpFail, (unsigned long)s.full, (unsigned long)s.closeFrm, (unsigned long)s.tcpLost,
      (unsigned long)s.stalls, (unsigned long)s.seatGone, (unsigned long)s.joinFail, (unsigned long)s.respawns,
      (unsigned long)s.syncs, (unsigned long)s.rx, (unsigned long)(s.rxBytes / 1024), (unsigned long)s.ticks,
      (unsigned long)s.tickMiss, (unsigned long)s.tickBack, (unsigned long)s.gap500, (unsigned long)s.gap2s,
      (unsigned long)s.maxGapI, (unsigned long)s.tx, (unsigned long)s.acks, (unsigned long)s.nacks,
      (unsigned long)s.unanswered, (unsigned long)s.lostOnDrop,
      (unsigned long)(s.rttN ? s.rttSum / s.rttN : 0), (unsigned long)s.rttMaxI, (unsigned long)s.moves,
      (unsigned long)s.trunc, (unsigned long)s.maxMsg, (unsigned long)(seated / 1000), z.ap, z.dp);
    s.maxGapI = 0; s.rttMaxI = 0;
  }
  n += snprintf(out + n, sizeof(out) - n, "],\"rttHist\":[");
  for (int b = 0; b < RTT_BUCKETS; b++)
    n += snprintf(out + n, sizeof(out) - n, "%s%lu", b ? "," : "", (unsigned long)g_rttHist[b]);
  n += snprintf(out + n, sizeof(out) - n, "],\"nack\":{");
  for (int w = 0; w < g_whyN; w++)
    n += snprintf(out + n, sizeof(out) - n, "%s\"%s\":%lu", w ? "," : "", g_why[w].w, (unsigned long)g_why[w].n);
  K10State k;
  portENTER_CRITICAL(&g_k10Mux);
  k = g_k10;
  portEXIT_CRITICAL(&g_k10Mux);
  n += snprintf(out + n, sizeof(out) - n,
    "},\"k10\":{\"ok\":%lu,\"fail\":%lu,\"httpMs\":%lu,\"httpMax\":%lu,\"reboots\":%lu,\"heap\":%ld,"
    "\"minHeap\":%ld,\"maxBlock\":%ld,\"maxTick\":%ld,\"partial\":%ld,\"skips\":%ld,\"connected\":%ld,"
    "\"evtDrops\":%ld,\"uptime\":%ld,\"pv\":%ld,\"day\":%ld,\"reset\":\"%s\"}}",
    (unsigned long)k.ok, (unsigned long)k.fail, (unsigned long)k.lastMs, (unsigned long)k.maxMs,
    (unsigned long)k.reboots, k.heap, k.minHeap, k.maxBlock, k.maxTick, k.partial, k.skips, k.connected,
    k.evtDrops, k.uptime, k.pv, k.day, k.reset);
  Serial.println(out);
}

// Commands, one per line:
//   go | stop          start / close every seat (stop is graceful, no fault)
//   seats N            how many seats to fill (1-6; the K10 seats at most 5)
//   sleep 0|1          Wi-Fi modem sleep: 1 behaves like a phone in power save
//   raw S <json>       send text on seat S's socket as is
//   report | zero      report now / clear every counter
static void handleSerial() {
  static char line[256];
  static size_t ln = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (ln < sizeof(line) - 1) line[ln++] = c; continue; }
    line[ln] = 0; ln = 0;
    if (!strcmp(line, "go")) g_run = true;
    else if (!strcmp(line, "stop")) { g_run = false; for (int i = 0; i < MAX_SEATS; i++) dropSeat(i, "stop", false); }
    else if (!strncmp(line, "seats ", 6)) g_seats = constrain(atoi(line + 6), 1, MAX_SEATS);
    else if (!strncmp(line, "sleep ", 6)) WiFi.setSleep(atoi(line + 6) != 0);
    else if (!strncmp(line, "raw ", 4)) {
      int i = atoi(line + 4);
      const char* j = strchr(line + 4, ' ');
      if (i >= 0 && i < MAX_SEATS && j && seats[i].st != S_OFF) sendFrame(seats[i], 0x1, (const uint8_t*)j + 1, strlen(j + 1));
    }
    else if (!strcmp(line, "report")) report();
    else if (!strcmp(line, "zero")) {
      for (int i = 0; i < MAX_SEATS; i++) { seats[i].s = Stats{}; if (seats[i].st == S_PLAY) seats[i].joinMs = millis(); }
      memset(g_rttHist, 0, sizeof(g_rttHist)); g_whyN = 0;
      g_wifiDrops = 0; g_wifiDownTotal = 0; g_minHeap = UINT32_MAX;
    }
    else continue;
    Serial.printf("E %lu cmd=%s\n", (unsigned long)millis(), line);
  }
}

static void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  uint32_t now = millis();
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    if (!g_wifiDownMs) { g_wifiDownMs = now; g_wifiDrops++; }
    g_wifiReason = info.wifi_sta_disconnected.reason;
    Serial.printf("E %lu wifi_down reason=%u\n", (unsigned long)now, (unsigned)g_wifiReason);
  } else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    if (g_wifiDownMs) { g_wifiDownTotal += now - g_wifiDownMs; g_wifiDownMs = 0; }
    g_wifiUpMs = now;
    Serial.printf("E %lu wifi_up ip=%s ch=%d\n", (unsigned long)now,
                  IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str(), WiFi.channel());
  }
}

void setup() {
  // A report line is ~2.5 KB: through the default 128-byte TX buffer at 115200
  // baud, println blocked the loop ~240 ms and every seat logged it as a gap.
  Serial.setTxBufferSize(8192);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nE 0 wroom_swarm boot seats=%d mac=%s\n", g_seats, WiFi.macAddress().c_str());
  for (int i = 0; i < MAX_SEATS; i++) {
    seats[i].buf = (char*)malloc(RX_CAP);
    seats[i].want = i;
    seats[i].nextTry = 3000 + i * 1500;
  }
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(AP_SSID);
  xTaskCreatePinnedToCore(stateTask, "k10state", 6144, nullptr, 1, nullptr, 0);
}

void loop() {
  handleSerial();
  uint32_t now = millis();
  for (int i = 0; i < MAX_SEATS; i++) {
    if (i >= g_seats && seats[i].st != S_OFF) dropSeat(i, "seats_reduced", false);
    service(i);
  }
  // Open one seat at a time, and only once nobody of ours is mid-join: each
  // pick costs the K10 a ~30 KB sync, and a join burst is what starved its
  // internal heap on 2026-09-24.
  if (g_run && WiFi.status() == WL_CONNECTED) {
    bool joining = false;
    for (int i = 0; i < g_seats; i++) joining |= (seats[i].st == S_LOBBY || seats[i].st == S_PICKING);
    for (int i = 0; i < g_seats && !joining; i++) {
      if (seats[i].st != S_OFF || (int32_t)(now - seats[i].nextTry) < 0) continue;
      openSeat(i);
      break;
    }
  }
  static uint32_t lastReport = 0;
  if (now - lastReport >= REPORT_MS) { lastReport = now; report(); }
  delay(1);
}
