#pragma once
// ── ui-display.hpp ──────────────────────────────────────────────────────────
// LovyanGFX display screens, LED flash, audio tone sequences, and score audio alerts.
// Included by Esp32HexMapCrawl.ino before gameplay .hpp files.

// ── RGB888 → RGB565 colour helper ──────────────────────────────────────────
static inline uint16_t c16(uint32_t c) {
  return (uint16_t)(((c & 0xF80000) >> 8) | ((c & 0x00FC00) >> 5) | ((c & 0x0000F8) >> 3));
}

// ── Canvas drawing helpers (replace K10 canvas API) ─────────────────────────
// canvasRect(x1,y1,x2,y2, col, fill): fill=true→fillRect, fill=false→drawRect
static inline void canvasRect(int x1, int y1, int x2, int y2, uint32_t col, bool fill) {
  if (fill) canvas.fillRect(x1, y1, x2 - x1, y2 - y1, c16(col));
  else      canvas.drawRect(x1, y1, x2 - x1, y2 - y1, c16(col));
}
static inline void canvasLine(int x1, int y1, int x2, int y2, uint32_t col) {
  canvas.drawLine(x1, y1, x2, y2, c16(col));
}
// canvasText16: 16px font (setTextSize 2)
static inline void canvasText16(const char* s, int x, int y, uint32_t col) {
  canvas.setTextSize(2);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText8: 8px font (setTextSize 1)
static inline void canvasText8(const char* s, int x, int y, uint32_t col) {
  canvas.setTextSize(1);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText24: 24px font (setTextSize 3)
static inline void canvasText24(const char* s, int x, int y, uint32_t col) {
  canvas.setTextSize(3);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}

// ── Boot splash diagnostic log ─────────────────────────────────────────────
static char    _sLog[14][30];
static uint8_t _sN = 0;
static void splashAdd(const char* msg, uint32_t col = 0) {
  if (_sN == 14) {
    for (int i = 0; i < 13; i++) memcpy(_sLog[i], _sLog[i+1], 30);
    _sN = 13;
  }
  snprintf(_sLog[_sN++], 30, "%s", msg);
  uint32_t textCol = col ? col : 0xD06818;
  canvas.fillScreen(0x0000);
  for (uint8_t i = 0; i < _sN; i++)
    canvasText8(_sLog[i], 4, 4 + i * 10, textCol);
  canvas.pushSprite(0, 0);
}

// ── LED flash (game events → RGB LEDs) ─────────────────────────────────────
// Called from Core 1 (game loop). Sets LED immediately; loop() turns it off.
void ledFlash(uint8_t r, uint8_t g, uint8_t b) {
  g_ledR = r; g_ledG = g; g_ledB = b;
  g_ledEndMs = millis() + 300;
  k10.rgb->write(-1, r, g, b);
}

// ── K10 event log (thread-safe ring buffer) ────────────────────────────────
static void k10LogAdd(const char* text) {
  taskENTER_CRITICAL(&k10LogMux);
  uint8_t idx;
  if (k10LogCount < K10_LOG_SIZE) {
    idx = (k10LogHead + k10LogCount) % K10_LOG_SIZE;
    k10LogCount++;
  } else {
    idx = k10LogHead;
    k10LogHead = (k10LogHead + 1) % K10_LOG_SIZE;
  }
  strlcpy(k10Log[idx].text, text, 34);
  k10Log[idx].ms = millis();
  taskEXIT_CRITICAL(&k10LogMux);
  k10Dirty = true;
}

static TaskHandle_t  s_toneTask = nullptr;
static const char*   s_toneName = nullptr;

static void toneTaskFn(void* arg) {
  // playTone(freq, beat) treats 'beat' as sample count at 8000 Hz, not ms.
  // It also calls i2s_zero_dma_buffer() immediately after writing, which
  // cancels queued audio before the DMA clock has time to play it.
  // Write directly to I2S instead: beat_ms * 8 = samples at 8 kHz.
  // i2s_write(portMAX_DELAY) throttles to DMA playback speed naturally.
  size_t written;
  uint32_t savedRate = i2s_get_clk(I2S_NUM_0);
  i2s_set_sample_rates(I2S_NUM_0, 8000);

  for (const ToneStep* s = (const ToneStep*)arg; s->freq != 0; s++) {
    int ms   = (s->freq < 0) ? -(s->freq) : s->beat;
    int freq = (s->freq < 0) ? 0          : s->freq;
    int n    = ms * 8;
    for (int i = 0; i < n; i++) {
      int16_t v = freq ? (int16_t)(32767.0f * sinf(i * (float)TWO_PI * freq / 8000.0f)) : 0;
      int16_t buf[2] = {v, v};
      i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    }
  }
  // DMA buffer = 3*300 samples @ 8kHz = 112.5 ms max latency; drain before zeroing.
  vTaskDelay(pdMS_TO_TICKS(115));
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_set_sample_rates(I2S_NUM_0, savedRate);
  s_toneTask = nullptr;
  s_toneName = nullptr;
  vTaskDelete(nullptr);
}

static void k10PlaySeq(const ToneStep* seq, const char* name = nullptr) {
  if (s_audioVol == 0) {
    Serial.printf("[TONE]    SKIP (vol=0): %s\n", name ? name : "?");
    return;
  }
  if (s_toneTask != nullptr) {
    Serial.printf("[TONE]    SKIP (busy):  %s\n", name ? name : "?");
    return;
  }
  s_toneName = name;
  Serial.printf("[TONE]    >> %s\n", name ? name : "?");
  xTaskCreate(toneTaskFn, "tone", 4096, (void*)seq, 1, &s_toneTask);
}

// Stringifies the motif/sequence name automatically
#define k10Play(seq) k10PlaySeq(seq, #seq)

// ── Screen 1: player status dashboard ──────────────────────────
static void drawPlayerScreen() {
  struct {
    bool    on;
    char    name[12];
    uint8_t ll, food, water, radiation, statusBits;
    uint8_t archetype;
    int8_t  movesLeft;
  } snap[MAX_PLAYERS];
  uint8_t  snapTC = 0;
  uint16_t snapDay = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  snapTC  = G.threatClock;
  snapDay = G.dayCount;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p          = G.players[i];
    snap[i].on         = p.connected;
    snap[i].ll         = p.ll;
    snap[i].food       = p.food;
    snap[i].water      = p.water;
    snap[i].radiation  = p.radiation;
    snap[i].archetype  = p.archetype;
    snap[i].statusBits = p.statusBits;
    snap[i].movesLeft  = p.movesLeft;
    memcpy(snap[i].name, p.name, 12);
  }
  xSemaphoreGive(G.mutex);

  static const char* ARCH_SHORT[NUM_ARCHETYPES] = {"GUID","QTMR","MEDC","MULE","SCUT","ENDR"};
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_INFO = 0x904030;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_TXT  = 0xC87840;
  static const uint32_t C_DIM  = 0x3A1808;
  static const uint32_t C_COND = 0xD06010;
  static const uint32_t C_OK   = 0xC05810;
  static const uint32_t C_WARN = 0xC87020;
  static const uint32_t C_CRIT = 0xE89018;

  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 27, 0x1E0A00, true);
  canvasText24("WASTELAND", 2, 3, C_HDR);
  canvasLine(0, 28, 239, 28, C_LINE);

  char buf[40];
  snprintf(buf, sizeof(buf), "Day:%-2u TC:%-2u  %luk",
           snapDay, snapTC,
           (unsigned long)(ESP.getFreeHeap() / 1024));
  canvasText8(buf, 2, 32, C_INFO);
  canvasLine(0, 42, 239, 42, C_LINE);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    int y1 = 46 + i * 22;
    int y2 = y1 + 11;

    if (snap[i].on) {
      uint8_t arch = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;
      uint32_t nameCol = (snap[i].statusBits & 0x0F) ? C_COND : C_TXT;
      snprintf(buf, sizeof(buf), "P%d %-4s  %-8.8s", i, ARCH_SHORT[arch], snap[i].name);
      canvasText8(buf, 2, y1, nameCol);

      uint32_t sc;
      if (snap[i].ll <= 2 || snap[i].food <= 1 || snap[i].water <= 1 || snap[i].radiation >= 7)
        sc = C_CRIT;
      else if (snap[i].ll <= 3 || snap[i].food <= 2 || snap[i].water <= 2 ||
               snap[i].radiation >= 4 || (snap[i].statusBits & 0x0F))
        sc = C_WARN;
      else
        sc = C_OK;

      snprintf(buf, sizeof(buf), "   LL:%-2u F:%-2u W:%-2u R:%-2u M:%-2d",
               snap[i].ll, snap[i].food, snap[i].water,
               snap[i].radiation, snap[i].movesLeft);
      canvasText8(buf, 2, y2, sc);
    } else {
      snprintf(buf, sizeof(buf), "P%d ----  (offline)", i);
      canvasText8(buf, 2, y1, C_DIM);
    }
  }

  canvasLine(0, 258, 239, 258, C_LINE);

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "up:%lum%02lus",
           (unsigned long)(up / 60), (unsigned long)(up % 60));
  canvasText8(buf, 2, 262, C_INFO);

  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  snprintf(buf, sizeof(buf), "AP: %d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  canvasText8(buf, 2, 282, 0x5C2C10);
  bool staConn = (staIp[0] != 0);
  uint32_t stColor;
  if (staConn) {
    snprintf(buf, sizeof(buf), "ST: %d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
    stColor = 0x5C2C10;
  } else if (bootWifiPending) {
    snprintf(buf, sizeof(buf), "ST: connecting...");
    stColor = 0x904030;
  } else if (savedSsid[0]) {
    snprintf(buf, sizeof(buf), "ST: saved:\"%.16s\"", savedSsid);
    stColor = 0x904030;
  } else {
    snprintf(buf, sizeof(buf), "ST: no creds");
    stColor = 0x3A1808;
  }
  canvasText8(buf, 2, 300, stColor);
}

// ── Screen 2: event log ────────────────────────────────────────
static void drawEventLogScreen() {
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_INFO = 0x904030;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_DIM  = 0x3A1808;

  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 27, 0x1E0A00, true);
  canvasText24("EVENT LOG", 2, 3, C_HDR);
  canvasLine(0, 28, 239, 28, C_LINE);

  K10LogEntry snap[K10_LOG_SIZE];
  uint8_t snapHead = 0, snapCount = 0;
  taskENTER_CRITICAL(&k10LogMux);
  memcpy(snap, k10Log, sizeof(k10Log));
  snapHead  = k10LogHead;
  snapCount = k10LogCount;
  taskEXIT_CRITICAL(&k10LogMux);

  char buf[50];
  int  y   = 32;
  uint32_t now = millis();
  for (int i = (int)snapCount - 1; i >= 0 && y <= 300; i--) {
    uint8_t  idx = (snapHead + (uint8_t)i) % K10_LOG_SIZE;
    uint32_t age = (now - snap[idx].ms) / 1000;
    uint32_t mm  = age / 60, ss = age % 60;
    snprintf(buf, sizeof(buf), "-%lum%02lus %s",
             (unsigned long)mm, (unsigned long)ss, snap[idx].text);
    uint32_t col = (age < 60) ? C_HDR : C_INFO;
    canvasText8(buf, 2, y, col);
    y += 10;
  }
  if (snapCount == 0)
    canvasText8("No events yet", 2, 50, C_DIM);

  canvasLine(0, 306, 239, 306, C_LINE);
  snprintf(buf, sizeof(buf), "%d events", (int)snapCount);
  canvasText8(buf, 2, 308, C_DIM);
}

// ── Screen 3: resources ────────────────────────────────────────
static void drawResourceScreen() {
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_TXT  = 0xC8A878;
  static const uint32_t C_INFO = 0x904030;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_DIM  = 0x3A1808;

  struct PSnap {
    bool    on;
    char    name[16];
    uint8_t inv[5];
    uint8_t invType[INV_SLOTS_MAX];
    uint8_t invQty[INV_SLOTS_MAX];
    uint8_t invSlots;
  } snap[MAX_PLAYERS];

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p       = G.players[i];
    snap[i].on      = p.connected;
    memcpy(snap[i].name,    p.name,    16);
    memcpy(snap[i].inv,     p.inv,     5);
    memcpy(snap[i].invType, p.invType, INV_SLOTS_MAX);
    memcpy(snap[i].invQty,  p.invQty,  INV_SLOTS_MAX);
    snap[i].invSlots = p.invSlots;
  }
  xSemaphoreGive(G.mutex);

  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 27, 0x1E0A00, true);
  canvasText24("RESOURCES", 2, 3, C_HDR);
  canvasLine(0, 28, 239, 28, C_LINE);

  static const char* RS[5] = {"Wtr","Fod","Ful","Med","Scr"};
  char buf[48];
  int y = 32;
  bool anyOn = false;

  for (int i = 0; i < MAX_PLAYERS && y < 300; i++) {
    if (!snap[i].on) continue;
    anyOn = true;
    snprintf(buf, sizeof(buf), "P%d -- %.11s", i, snap[i].name);
    canvasText8(buf, 2, y, C_HDR);
    y += 10;
    snprintf(buf, sizeof(buf), " %s:%-2u %s:%-2u %s:%-2u %s:%-2u %s:%-2u",
      RS[0], snap[i].inv[0], RS[1], snap[i].inv[1], RS[2], snap[i].inv[2],
      RS[3], snap[i].inv[3], RS[4], snap[i].inv[4]);
    canvasText8(buf, 2, y, C_TXT);
    y += 10;
    for (int s = 0; s < snap[i].invSlots && s < INV_SLOTS_MAX && y < 295; s++) {
      if (!snap[i].invType[s]) continue;
      const ItemDef* def = getItemDef(snap[i].invType[s]);
      snprintf(buf, sizeof(buf), " %-14.14s x%u",
               def ? def->name : "???", snap[i].invQty[s]);
      canvasText8(buf, 2, y, C_INFO);
      y += 10;
    }
    if (y < 295) { canvasLine(4, y+3, 235, y+3, C_LINE); y += 10; }
  }
  if (!anyOn)
    canvasText8("No survivors online", 2, 50, C_DIM);
}

// ── Screen 4: encounter tracking ──────────────────────────────
static void drawEncounterScreen() {
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_TXT  = 0xC8A878;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_DIM  = 0x3A1808;

  uint16_t poiLeft = 0;
  struct ESnap { bool on; char name[16]; uint16_t encCount; } snap[MAX_PLAYERS];

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      if (G.map[r][c].poi) poiLeft++;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    snap[i].on       = G.players[i].connected;
    snap[i].encCount = G.players[i].encCount;
    memcpy(snap[i].name, G.players[i].name, 16);
  }
  xSemaphoreGive(G.mutex);

  // sort connected players by encCount descending
  int order[MAX_PLAYERS], cnt = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (snap[i].on) order[cnt++] = i;
  for (int a = 1; a < cnt; a++) {
    int key = order[a], b = a - 1;
    while (b >= 0 && snap[order[b]].encCount < snap[key].encCount) {
      order[b+1] = order[b]; b--;
    }
    order[b+1] = key;
  }

  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 27, 0x1E0A00, true);
  canvasText24("ENCOUNTERS", 2, 3, C_HDR);
  canvasLine(0, 28, 239, 28, C_LINE);

  char buf[48];
  snprintf(buf, sizeof(buf), "POIs remaining: %u", (unsigned)poiLeft);
  canvasText8(buf, 2, 32, C_HDR);
  canvasLine(0, 42, 239, 42, C_LINE);
  canvasText8("#   NAME         ENCS", 2, 46, C_DIM);

  if (cnt == 0) {
    canvasText8("No survivors online", 2, 80, C_DIM);
  } else {
    int y = 74;
    for (int rank = 0; rank < cnt && y < 310; rank++) {
      int i = order[rank];
      uint32_t col = (rank == 0) ? C_HDR : C_TXT;
      snprintf(buf, sizeof(buf), "%-2d  %-12.12s %u", rank + 1, snap[i].name, snap[i].encCount);
      canvasText8(buf, 2, y, col);
      y += 18;
    }
  }
}

// ── Screen 5: hex map minimap ──────────────────────────────────
static void drawMapScreen() {
  // Terrain fill colours — monochromatic amber, indexed by terrain ID 0–10
  static const uint32_t TERR_COL[11] = {
    0x705840,  // 0  Open Scrub
    0x502010,  // 1  Ash Dunes
    0x281808,  // 2  Rust Forest
    0x403828,  // 3  Marsh
    0xA07828,  // 4  Broken Urban
    0x303828,  // 5  Flooded Ruins
    0xC08028,  // 6  Glass Fields
    0x604838,  // 7  (reserved)
    0x806040,  // 8  (reserved)
    0xE8A828,  // 9  Settlement
    0x080402,  // 10 Nuke Crater
  };

  // ── snapshot ─────────────────────────────────────────────────
  uint8_t terr[MAP_ROWS][MAP_COLS];
  struct { int16_t q, r; bool on; } ps[MAX_PLAYERS];

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      terr[r][c] = G.map[r][c].terrain;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    ps[i].q  = G.players[i].q;
    ps[i].r  = G.players[i].r;
    ps[i].on = G.players[i].connected;
  }
  xSemaphoreGive(G.mutex);

  // ── layout: fill 240×320, no header ──────────────────────────
  // cells 9×16, odd-col y-offset 8 (= CH/2)
  static const int CW =  9;
  static const int CH = 16;
  static const int XS =  9;
  static const int YS = 16;
  static const int OY =  8;
  static const int MX =  7;
  static const int MY =  4;

  // ── draw ─────────────────────────────────────────────────────
  canvas.fillScreen(0x0000);

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int px = MX + q * XS;
      int py = MY + r * YS + (q & 1) * OY;
      uint8_t t = terr[r][q];
      uint32_t col = (t < 11) ? TERR_COL[t] : 0x3A1808;
      // (x1,y1,x2,y2) → fillRect(x1, y1, w, h)
      canvas.fillRect(px, py, CW - 1, CH - 1, c16(col));
    }
  }

  // ── player number markers ─────────────────────────────────────
  canvas.setTextSize(2);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!ps[i].on) continue;
    int q = ps[i].q, r = ps[i].r;
    if (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS) continue;
    int px = MX + q * XS;
    int py = MY + r * YS + (q & 1) * OY;
    char num[2] = { (char)('1' + i), 0 };
    canvas.setTextColor(c16(0xD06818));
    canvas.setCursor(px, py);
    canvas.print(num);
  }

  // ── map border (amber outline) ────────────────────────────────
  int bx1 = MX - 1;
  int by1 = MY - 1;
  int bx2 = MX + (MAP_COLS - 1) * XS + CW;
  int by2 = MY + (MAP_ROWS - 1) * YS + OY + CH;
  canvas.drawRect(bx1, by1, bx2 - bx1, by2 - by1, c16(0xD06818));
}

// ── K10 button B screen switching ─────────────────────────────
// Screens: 1=Players 2=Events 3=Resources 4=Encounters 5=Map
static void checkGestureSwitch() {
  bool btnB = k10.buttonB && k10.buttonB->isPressed();
  if (btnB && !k10BtnBLast) { k10Screen = (k10Screen % 5) + 1; k10Play(MOTIF_SCREEN_CLICK); }
  k10BtnBLast = btnB;
}

// ── Time-of-day LED colour ────────────────────────────────────────────────────
struct TodStop { float pct; uint8_t r, g, b; };
static void todColour(uint32_t dayTick, uint8_t& r, uint8_t& g, uint8_t& b) {
  static const TodStop STOPS[] = {
    { 0.00f, 180,  55,   5 },
    { 0.08f, 200, 130,  15 },
    { 0.30f,  40,  90, 180 },
    { 0.60f, 200, 110,  10 },
    { 0.80f, 160,  25,   5 },
    { 0.92f,  10,   8,  60 },
    { 1.00f,  10,   8,  60 },
  };
  constexpr uint8_t N = sizeof(STOPS) / sizeof(STOPS[0]);
  float t = (float)dayTick / (float)DAY_TICKS;
  if (t >= 1.0f) t = 0.99f;
  for (uint8_t i = 0; i < N - 1; i++) {
    if (t >= STOPS[i].pct && t < STOPS[i+1].pct) {
      float frac = (t - STOPS[i].pct) / (STOPS[i+1].pct - STOPS[i].pct);
      r = (uint8_t)(STOPS[i].r + frac * (STOPS[i+1].r - STOPS[i].r));
      g = (uint8_t)(STOPS[i].g + frac * (STOPS[i+1].g - STOPS[i].g));
      b = (uint8_t)(STOPS[i].b + frac * (STOPS[i+1].b - STOPS[i].b));
      return;
    }
  }
  r = STOPS[N-1].r; g = STOPS[N-1].g; b = STOPS[N-1].b;
}

// ── K10 LED update ───────────────────────────────────────────────────────────
static void updateLEDs() {
  k10.rgb->brightness(s_ledBright);
  uint32_t now = millis();

  uint32_t snapDayTick = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    snapDayTick = G.dayTick;
    xSemaphoreGive(G.mutex);
  }

  uint8_t tr, tg, tb;
  todColour(snapDayTick, tr, tg, tb);

  if (k10LedPulse && now < k10LedPulse) {
    k10.rgb->write(0, k10PulseR, k10PulseG, k10PulseB);
  } else {
    k10LedPulse = 0;
    k10.rgb->write(0, tr, tg, tb);
  }

  k10.rgb->write(1, tr, tg, tb);
  k10.rgb->write(2, tr, tg, tb);
}

// ── K10 audio: score milestones, TC thresholds, resource alerts ─
static void checkScoreAudio() {
  uint32_t teamScore = 0;
  uint8_t  snapTC    = 0;
  bool     crisis    = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (G.players[i].connected) teamScore += G.players[i].score;
    snapTC  = G.threatClock;
    crisis  = G.crisisState;
    xSemaphoreGive(G.mutex);
  }

  if (teamScore / 100 != k10TeamScore / 100) {
    if (teamScore > k10TeamScore) {
      k10Play(SEQ_SCORE_UP);
      k10LedPulse = millis() + 600;
      k10PulseR = 0x00; k10PulseG = 0xC8; k10PulseB = 0x30;
    } else {
      k10Play(MOTIF_ROTTEN_CHORD);
      k10LedPulse = millis() + 600;
      k10PulseR = 0xC8; k10PulseG = 0x10; k10PulseB = 0x10;
    }
  }
  k10TeamScore = teamScore;

  uint8_t tcLvl = (snapTC >= TC_THRESHOLD_D) ? 4 :
                  (snapTC >= TC_THRESHOLD_C) ? 3 :
                  (snapTC >= TC_THRESHOLD_B) ? 2 :
                  (snapTC >= TC_THRESHOLD_A) ? 1 : 0;
  if (tcLvl > k10PrevTCLevel) {
    if (tcLvl == 4 || crisis) k10Play(MOTIF_BUNKER_ALARM); else k10Play(MOTIF_WARNING_GRUNT);
  }
  k10PrevTCLevel = tcLvl;

}
