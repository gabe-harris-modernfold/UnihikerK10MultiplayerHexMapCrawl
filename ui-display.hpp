#pragma once
// ── ui-display.hpp ──────────────────────────────────────────────────────────
// K10 display screens, LED flash, audio tone sequences, and score audio alerts.
// Included by Esp32HexMapCrawl.ino before gameplay .hpp files.

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

static TaskHandle_t s_toneTask = nullptr;

static void toneTaskFn(void* arg) {
  for (const ToneStep* s = (const ToneStep*)arg; s->freq != 0; s++) {
    if (s->freq < 0)
      vTaskDelay(pdMS_TO_TICKS(-(s->freq)));
    else
      k10Music.playTone(s->freq, s->beat);
  }
  s_toneTask = nullptr;
  vTaskDelete(nullptr);
}

static void k10PlaySeq(const ToneStep* seq) {
  if (s_audioVol > 0 && s_toneTask == nullptr)
    xTaskCreate(toneTaskFn, "tone", 2048, (void*)seq, 1, &s_toneTask);
}

// ── K10 screen 1: player status dashboard ──────────────────────
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

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);

  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E0A00, 0x1E0A00, true);
  k10.canvas->canvasText("WASTELAND", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[P]", 192, 10, 0x502010, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  char buf[40];
  snprintf(buf, sizeof(buf), "Day:%-2u TC:%-2u  %luk",
           snapDay, snapTC,
           (unsigned long)(ESP.getFreeHeap() / 1024));
  k10.canvas->canvasText(buf, 2, 32, C_INFO, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 50, 239, 50, C_LINE);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    int y1 = 54 + i * 34;
    int y2 = y1 + 17;

    if (snap[i].on) {
      uint8_t arch = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;

      uint32_t nameCol = (snap[i].statusBits & 0x0F) ? C_COND : C_TXT;
      snprintf(buf, sizeof(buf), "P%d %-4s  %-8.8s", i, ARCH_SHORT[arch], snap[i].name);
      k10.canvas->canvasText(buf, 2, y1, nameCol, Canvas::eCNAndENFont16, 50, false);

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
      k10.canvas->canvasText(buf, 2, y2, sc, Canvas::eCNAndENFont16, 50, false);
    } else {
      snprintf(buf, sizeof(buf), "P%d ----  (offline)", i);
      k10.canvas->canvasText(buf, 2, y1, C_DIM, Canvas::eCNAndENFont16, 50, false);
    }
  }

  k10.canvas->canvasLine(0, 258, 239, 258, C_LINE);

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "up:%lum%02lus",
           (unsigned long)(up / 60), (unsigned long)(up % 60));
  k10.canvas->canvasText(buf, 2, 262, C_INFO, Canvas::eCNAndENFont16, 50, false);

  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  snprintf(buf, sizeof(buf), "AP: %d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  k10.canvas->canvasText(buf, 2, 282, 0x5C2C10, Canvas::eCNAndENFont16, 50, false);
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
  k10.canvas->canvasText(buf, 2, 300, stColor, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->updateCanvas();
}

// ── K10 screen 0: title ────────────────────────────────────────
static void drawTitleScreen() {
  k10.canvas->canvasClear();
  k10.canvas->canvasDrawImage(0, 0, String("S:/data/img/wastelandTitle0.png"));
  k10.canvas->updateCanvas();
}

// ── K10 screen 2: event log ────────────────────────────────────
static void drawEventLogScreen() {
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_INFO = 0x904030;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_DIM  = 0x3A1808;

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E0A00, 0x1E0A00, true);
  k10.canvas->canvasText("EVENT LOG", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[E]", 192, 10, 0x502010, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

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
    k10.canvas->canvasText(buf, 2, y, col, Canvas::eCNAndENFont16, 50, false);
    y += 18;
  }
  if (snapCount == 0)
    k10.canvas->canvasText("No events yet", 2, 50, C_DIM, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 306, 239, 306, C_LINE);
  snprintf(buf, sizeof(buf), "%d events", (int)snapCount);
  k10.canvas->canvasText(buf, 2, 308, C_DIM, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->updateCanvas();
}

// ── K10 screen 3: resources ────────────────────────────────────
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

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E0A00, 0x1E0A00, true);
  k10.canvas->canvasText("RESOURCES", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[R]", 192, 10, 0x502010, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  static const char* RS[5] = {"Wtr","Fod","Ful","Med","Scr"};
  char buf[48];
  int y = 32;
  bool anyOn = false;

  for (int i = 0; i < MAX_PLAYERS && y < 300; i++) {
    if (!snap[i].on) continue;
    anyOn = true;
    snprintf(buf, sizeof(buf), "P%d -- %.11s", i, snap[i].name);
    k10.canvas->canvasText(buf, 2, y, C_HDR, Canvas::eCNAndENFont16, 50, false);
    y += 18;
    snprintf(buf, sizeof(buf), " %s:%-2u %s:%-2u %s:%-2u %s:%-2u %s:%-2u",
      RS[0], snap[i].inv[0], RS[1], snap[i].inv[1], RS[2], snap[i].inv[2],
      RS[3], snap[i].inv[3], RS[4], snap[i].inv[4]);
    k10.canvas->canvasText(buf, 2, y, C_TXT, Canvas::eCNAndENFont16, 50, false);
    y += 18;
    for (int s = 0; s < snap[i].invSlots && s < INV_SLOTS_MAX && y < 295; s++) {
      if (!snap[i].invType[s]) continue;
      const ItemDef* def = getItemDef(snap[i].invType[s]);
      snprintf(buf, sizeof(buf), " %-14.14s x%u",
               def ? def->name : "???", snap[i].invQty[s]);
      k10.canvas->canvasText(buf, 2, y, C_INFO, Canvas::eCNAndENFont16, 50, false);
      y += 18;
    }
    if (y < 295) { k10.canvas->canvasLine(4, y+3, 235, y+3, C_LINE); y += 10; }
  }
  if (!anyOn)
    k10.canvas->canvasText("No survivors online", 2, 50, C_DIM, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->updateCanvas();
}

// ── K10 screen 4: encounter tracking ──────────────────────────
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

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E0A00, 0x1E0A00, true);
  k10.canvas->canvasText("ENCOUNTERS", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[X]", 192, 10, 0x502010, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  char buf[48];
  snprintf(buf, sizeof(buf), "POIs remaining: %u", (unsigned)poiLeft);
  k10.canvas->canvasText(buf, 2, 32, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasLine(0, 52, 239, 52, C_LINE);

  k10.canvas->canvasText("#   NAME         ENCS", 2, 56, C_DIM, Canvas::eCNAndENFont16, 50, false);

  if (cnt == 0) {
    k10.canvas->canvasText("No survivors online", 2, 80, C_DIM, Canvas::eCNAndENFont16, 50, false);
  } else {
    int y = 74;
    for (int rank = 0; rank < cnt && y < 310; rank++) {
      int i = order[rank];
      uint32_t col = (rank == 0) ? C_HDR : C_TXT;
      snprintf(buf, sizeof(buf), "%-2d  %-12.12s %u", rank + 1, snap[i].name, snap[i].encCount);
      k10.canvas->canvasText(buf, 2, y, col, Canvas::eCNAndENFont16, 50, false);
      y += 18;
    }
  }
  k10.canvas->updateCanvas();
}

// ── K10 screen 5: hex map minimap ─────────────────────────────
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
  // width:  7 + 24*9 + 9 = 232 px  → 7px left, 8px right
  // height: 4 + 18*16 + 8 + 16 = 316 px → 4px top/bottom
  static const int CW =  9;
  static const int CH = 16;
  static const int XS =  9;
  static const int YS = 16;
  static const int OY =  8;
  static const int MX =  7;
  static const int MY =  4;

  // ── draw ─────────────────────────────────────────────────────
  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int px = MX + q * XS;
      int py = MY + r * YS + (q & 1) * OY;
      uint8_t t = terr[r][q];
      uint32_t col = (t < 11) ? TERR_COL[t] : 0x3A1808;
      k10.canvas->canvasRectangle(px, py, px + CW - 1, py + CH - 1, col, col, true);
    }
  }

  // ── player number markers: white text over terrain, no background rect ─
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!ps[i].on) continue;
    int q = ps[i].q, r = ps[i].r;
    if (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS) continue;
    int px = MX + q * XS;
    int py = MY + r * YS + (q & 1) * OY;
    char num[2] = { (char)('1' + i), 0 };
    k10.canvas->canvasText(num, px, py, 0xD06818, Canvas::eCNAndENFont16, 50, false);
  }

  // ── map border ───────────────────────────────────────────────
  k10.canvas->canvasRectangle(MX-1, MY-1,
                               MX + (MAP_COLS-1)*XS + CW,
                               MY + (MAP_ROWS-1)*YS + OY + CH,
                               0xD06818, 0x000000, false);

  k10.canvas->updateCanvas();
}

// ── K10 button B screen switching ─────────────────────────────
// Screens: 0=Title 1=Players 2=Events 3=Resources 4=Encounters
static void checkGestureSwitch() {
  bool btnB = k10.buttonB && k10.buttonB->isPressed();
  if (btnB && !k10BtnBLast) k10Screen = (k10Screen + 1) % 6;
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
      k10PlaySeq(SEQ_SCORE_UP);
      k10LedPulse = millis() + 600;
      k10PulseR = 0x00; k10PulseG = 0xC8; k10PulseB = 0x30;
    } else {
      k10PlaySeq(SEQ_SCORE_DOWN);
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
    k10PlaySeq((tcLvl == 4 || crisis) ? SEQ_CRISIS : SEQ_ALERT);
  }
  k10PrevTCLevel = tcLvl;

}
