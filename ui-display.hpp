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
  uint8_t  snapTC = 0, snapSF = 0, snapSW = 0;
  uint16_t snapDay = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  snapTC  = G.threatClock;
  snapDay = G.dayCount;
  snapSF  = G.sharedFood;
  snapSW  = G.sharedWater;
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
  static const uint32_t C_HDR  = 0xC89030;
  static const uint32_t C_INFO = 0x907050;
  static const uint32_t C_LINE = 0x503830;
  static const uint32_t C_TXT  = 0xC8A878;
  static const uint32_t C_DIM  = 0x403020;
  static const uint32_t C_COND = 0xD07020;
  static const uint32_t C_OK   = 0x88C040;
  static const uint32_t C_WARN = 0xC8A030;
  static const uint32_t C_CRIT = 0xC84830;

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);

  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E1000, 0x1E1000, true);
  k10.canvas->canvasText("WASTELAND", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[P]", 192, 10, 0x504030, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  char buf[40];
  snprintf(buf, sizeof(buf), "Day:%-2u TC:%-2u  F:%-2u W:%-2u  %luk",
           snapDay, snapTC, snapSF, snapSW,
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
  snprintf(buf, sizeof(buf), "SF:%-2u SW:%-2u  up:%lum%02lus",
           snapSF, snapSW, (unsigned long)(up / 60), (unsigned long)(up % 60));
  k10.canvas->canvasText(buf, 2, 262, C_INFO, Canvas::eCNAndENFont16, 50, false);

  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  snprintf(buf, sizeof(buf), "AP: %d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  k10.canvas->canvasText(buf, 2, 282, 0x406030, Canvas::eCNAndENFont16, 50, false);
  bool staConn = (staIp[0] != 0);
  uint32_t stColor;
  if (staConn) {
    snprintf(buf, sizeof(buf), "ST: %d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
    stColor = 0x406030;
  } else if (bootWifiPending) {
    snprintf(buf, sizeof(buf), "ST: connecting...");
    stColor = 0x4080C0;
  } else if (savedSsid[0]) {
    snprintf(buf, sizeof(buf), "ST: saved:\"%.16s\"", savedSsid);
    stColor = 0x605030;
  } else {
    snprintf(buf, sizeof(buf), "ST: no creds");
    stColor = 0x302820;
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
  static const uint32_t C_HDR  = 0xC89030;
  static const uint32_t C_INFO = 0x907050;
  static const uint32_t C_LINE = 0x503830;
  static const uint32_t C_DIM  = 0x403020;

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E1000, 0x1E1000, true);
  k10.canvas->canvasText("EVENT LOG", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[E]", 192, 10, 0x504030, Canvas::eCNAndENFont16, 50, false);
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

// ── K10 screen 2: threat clock ─────────────────────────────────
static void drawThreatScreen() {
  static const uint32_t C_HDR  = 0xC89030;
  static const uint32_t C_INFO = 0x907050;
  static const uint32_t C_LINE = 0x503830;
  static const uint32_t C_DIM  = 0x403020;

  uint8_t snapTC   = 0;
  bool    crisis   = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    snapTC = G.threatClock; crisis = G.crisisState;
    xSemaphoreGive(G.mutex);
  }

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E1000, 0x1E1000, true);
  k10.canvas->canvasText("THREAT CLOCK", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[T]", 192, 10, 0x504030, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  char buf[48];
  snprintf(buf, sizeof(buf), "TC: %d / 20", (int)snapTC);
  k10.canvas->canvasText(buf, 2, 32, C_HDR, Canvas::eCNAndENFont24, 50, false);

  int barW = (int)((snapTC * 230L) / 20);
  uint32_t barCol = (snapTC >= TC_THRESHOLD_D) ? 0xC84830 :
                    (snapTC >= TC_THRESHOLD_C) ? 0xC06020 :
                    (snapTC >= TC_THRESHOLD_A) ? 0xC89030 : 0x88C040;
  k10.canvas->canvasRectangle(4, 60, 234, 78, C_LINE, C_DIM, true);
  if (barW > 0) k10.canvas->canvasRectangle(4, 60, 4 + barW, 78, barCol, barCol, true);
  const uint8_t TC_TICKS[4] = { TC_THRESHOLD_A, TC_THRESHOLD_B, TC_THRESHOLD_C, TC_THRESHOLD_D };
  for (int t = 0; t < 4; t++) {
    int tx = 4 + (int)((TC_TICKS[t] * 230L) / 20);
    k10.canvas->canvasLine(tx, 57, tx, 81, 0x808080);
  }

  const char* nextLabel = nullptr;
  int         nextIn    = 0;
  if      (snapTC < TC_THRESHOLD_A) { nextLabel = "TC5: Encounter"; nextIn = TC_THRESHOLD_A - snapTC; }
  else if (snapTC < TC_THRESHOLD_B) { nextLabel = "TC9: Hazard+1";  nextIn = TC_THRESHOLD_B - snapTC; }
  else if (snapTC < TC_THRESHOLD_C) { nextLabel = "TC13: Roll-1";   nextIn = TC_THRESHOLD_C - snapTC; }
  else if (snapTC < TC_THRESHOLD_D) { nextLabel = "TC17: CRISIS";   nextIn = TC_THRESHOLD_D - snapTC; }
  else                               { nextLabel = "CRISIS ACTIVE";  nextIn = 0; }
  if (nextIn > 0)
    snprintf(buf, sizeof(buf), "Next: %s (in %d)", nextLabel, nextIn);
  else
    snprintf(buf, sizeof(buf), "%s", nextLabel);
  k10.canvas->canvasText(buf, 2, 84, C_INFO, Canvas::eCNAndENFont16, 50, false);

  if (crisis || snapTC >= TC_THRESHOLD_D) {
    k10.canvas->canvasRectangle(0, 106, 240, 130, 0xC84830, 0xC84830, true);
    k10.canvas->canvasText("! CRISIS STATE !", 6, 109, 0xFFFFFF, Canvas::eCNAndENFont24, 50, false);
  }

  k10.canvas->canvasLine(0, 134, 239, 134, C_LINE);
  k10.canvas->canvasText("Ruins scavenge: TC+1", 2, 138, C_DIM, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("TC5 Enc TC9 Hzd+1", 2, 156, C_DIM, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("TC13 Roll-1 TC17 CRISIS", 2, 174, C_DIM, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->updateCanvas();
}

// ── K10 screen 3: resources ────────────────────────────────────
static void drawResourceScreen() {
  static const uint32_t C_HDR  = 0xC89030;
  static const uint32_t C_INFO = 0x907050;
  static const uint32_t C_LINE = 0x503830;
  static const uint32_t C_DIM  = 0x403020;
  static const uint32_t C_OK   = 0x88C040;
  static const uint32_t C_WARN = 0xC8A030;
  static const uint32_t C_CRIT = 0xC84830;

  uint8_t snapSF = 0, snapSW = 0;
  int     conn   = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    snapSF = G.sharedFood; snapSW = G.sharedWater;
    for (int i = 0; i < MAX_PLAYERS; i++) if (G.players[i].connected) conn++;
    xSemaphoreGive(G.mutex);
  }

  k10.canvas->canvasClear();
  k10.canvas->canvasSetLineWidth(1);
  k10.canvas->canvasRectangle(0, 0, 240, 27, 0x1E1000, 0x1E1000, true);
  k10.canvas->canvasText("RESOURCES", 2, 3, C_HDR, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasText("[R]", 192, 10, 0x504030, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(0, 28, 239, 28, C_LINE);

  char buf[48];
  int fPerDay = max(conn, 1);
  int fDays   = snapSF / fPerDay;
  uint32_t fCol   = (snapSF <= 2) ? C_CRIT : (snapSF <= 5) ? C_WARN : C_OK;
  uint32_t fTlCol = (fDays  <= 2) ? C_CRIT : (fDays  <= 5) ? C_WARN : C_OK;

  k10.canvas->canvasText("FOOD (shared)", 2, 32, C_INFO, Canvas::eCNAndENFont16, 50, false);
  snprintf(buf, sizeof(buf), "SF: %d tokens", (int)snapSF);
  k10.canvas->canvasText(buf, 2, 50, fCol, Canvas::eCNAndENFont24, 50, false);
  int fBarW = (int)(min((int)snapSF, 30) * 230 / 30);
  k10.canvas->canvasRectangle(4, 78, 234, 92, C_LINE, C_DIM, true);
  if (fBarW > 0) k10.canvas->canvasRectangle(4, 78, 4 + fBarW, 92, fCol, fCol, true);
  snprintf(buf, sizeof(buf), "-%d/day  ~%d days", fPerDay, fDays);
  k10.canvas->canvasText(buf, 2, 96, fTlCol, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 115, 239, 115, C_LINE);

  int wPerDay = max(conn * 2, 1);
  int wDays   = snapSW / wPerDay;
  uint32_t wCol   = (snapSW <= 2) ? C_CRIT : (snapSW <= 5) ? C_WARN : C_OK;
  uint32_t wTlCol = (wDays  <= 2) ? C_CRIT : (wDays  <= 5) ? C_WARN : C_OK;

  k10.canvas->canvasText("WATER (shared)", 2, 119, C_INFO, Canvas::eCNAndENFont16, 50, false);
  snprintf(buf, sizeof(buf), "SW: %d tokens", (int)snapSW);
  k10.canvas->canvasText(buf, 2, 137, wCol, Canvas::eCNAndENFont24, 50, false);
  int wBarW = (int)(min((int)snapSW, 30) * 230 / 30);
  k10.canvas->canvasRectangle(4, 165, 234, 179, C_LINE, C_DIM, true);
  if (wBarW > 0) k10.canvas->canvasRectangle(4, 165, 4 + wBarW, 179, wCol, wCol, true);
  snprintf(buf, sizeof(buf), "-%d/day  ~%d days", wPerDay, wDays);
  k10.canvas->canvasText(buf, 2, 183, wTlCol, Canvas::eCNAndENFont16, 50, false);

  k10.canvas->canvasLine(0, 202, 239, 202, C_LINE);
  snprintf(buf, sizeof(buf), "Players online: %d", conn);
  k10.canvas->canvasText(buf, 2, 206, C_DIM, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->updateCanvas();
}

// ── K10 gesture + button B screen switching ────────────────────
static void checkGestureSwitch() {
  uint8_t g = GestureNone;
  if      (k10.isGesture(TiltForward)) g = TiltForward;
  else if (k10.isGesture(TiltRight))   g = TiltRight;
  else if (k10.isGesture(TiltBack))    g = TiltBack;
  else if (k10.isGesture(ScreenUp))    g = ScreenUp;

  if (g != GestureNone) {
    if (g == k10GestLast) { if (k10GestCnt < 2) k10GestCnt++; }
    else                  { k10GestLast = g; k10GestCnt = 1; }
    if (k10GestCnt >= 2) {
      if      (g == TiltForward) k10Screen = 2;
      else if (g == TiltRight)   k10Screen = 3;
      else if (g == TiltBack)    k10Screen = 4;
      else if (g == ScreenUp)    k10Screen = 1;
    }
  } else {
    k10GestCnt  = 0;
    k10GestLast = GestureNone;
  }

  bool btnB = k10.buttonB && k10.buttonB->isPressed();
  if (btnB && !k10BtnBLast) k10Screen = (k10Screen + 1) % 5;
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
  uint8_t  minRes    = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (G.players[i].connected) teamScore += G.players[i].score;
    snapTC  = G.threatClock;
    crisis  = G.crisisState;
    minRes  = min(G.sharedFood, G.sharedWater);
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

  bool resCrit = (minRes <= 2);
  if (resCrit && !k10ResCritPrev) k10PlaySeq(SEQ_RESOURCE);
  k10ResCritPrev = resCrit;
}
