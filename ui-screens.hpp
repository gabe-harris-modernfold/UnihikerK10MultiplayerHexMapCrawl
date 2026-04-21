#pragma once
// ── ui-screens.hpp ───────────────────────────────────────────────────────────
// All 5 K10 display screens and button-B screen cycling.

// ── Screen 1: player status dashboard ──────────────────────────
static void drawPlayerScreen() {
  struct {
    bool    on;
    char    name[12];
    uint8_t ll, food, water, radiation;
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
      uint32_t nameCol = C_TXT;
      snprintf(buf, sizeof(buf), "P%d %-4s  %-8.8s", i, ARCH_SHORT[arch], snap[i].name);
      canvasText8(buf, 2, y1, nameCol);

      uint32_t sc;
      if (snap[i].ll <= 2 || snap[i].food <= 1 || snap[i].water <= 1 || snap[i].radiation >= 7)
        sc = C_CRIT;
      else if (snap[i].ll <= 3 || snap[i].food <= 2 || snap[i].water <= 2 ||
               snap[i].radiation >= 4)
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

  uint32_t upSec = millis() / 1000;
  snprintf(buf, sizeof(buf), "up: %lum%02lus",
           (unsigned long)(upSec / 60), (unsigned long)(upSec % 60));
  canvasText8(buf, 2, 262, C_INFO);

  if (checkRtcReady()) {
    time_t nowEpoch  = time(nullptr);
    time_t bootEpoch = nowEpoch - (time_t)upSec;

    struct tm bt; gmtime_r(&bootEpoch, &bt);
    snprintf(buf, sizeof(buf), "boot: %02d:%02d:%02d", bt.tm_hour, bt.tm_min, bt.tm_sec);
    canvasText8(buf, 122, 262, C_INFO);

    struct tm ut; gmtime_r(&nowEpoch, &ut);
    snprintf(buf, sizeof(buf), "UTC: %02d:%02d:%02d", ut.tm_hour, ut.tm_min, ut.tm_sec);
    canvasText8(buf, 2, 275, C_INFO);

    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
    struct tm it; localtime_r(&nowEpoch, &it);
    setenv("TZ", "UTC0", 1); tzset();
    snprintf(buf, sizeof(buf), "IN:  %02d:%02d:%02d", it.tm_hour, it.tm_min, it.tm_sec);
    canvasText8(buf, 122, 275, C_INFO);
  }

  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  snprintf(buf, sizeof(buf), "AP: %d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  canvasText8(buf, 2, 292, 0x5C2C10);
  bool staConn = (staIp[0] != 0);
  uint32_t stColor;
  char stBuf[42];
  if (staConn) {
    snprintf(stBuf, sizeof(stBuf), "ST: %d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
    stColor = 0x5C2C10;
  } else if (bootWifiPending) {
    snprintf(stBuf, sizeof(stBuf), "ST: connecting...");
    stColor = 0x904030;
  } else if (savedSsid[0]) {
    snprintf(stBuf, sizeof(stBuf), "ST: \"%s\"", savedSsid);
    stColor = 0x904030;
  } else {
    snprintf(stBuf, sizeof(stBuf), "ST: no creds");
    stColor = 0x3A1808;
  }
  canvasText8(stBuf, 122, 292, stColor);
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

  // cells 9×16, odd-col y-offset 8 (= CH/2)
  static const int CW =  9;
  static const int CH = 16;
  static const int XS =  9;
  static const int YS = 16;
  static const int OY =  8;
  static const int MX =  7;
  static const int MY =  4;

  canvas.fillScreen(0x0000);

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int px = MX + q * XS;
      int py = MY + r * YS + (q & 1) * OY;
      uint8_t t = terr[r][q];
      uint32_t col = (t < 11) ? TERR_COL[t] : 0x3A1808;
      canvas.fillRect(px, py, CW - 1, CH - 1, c16(col));
    }
  }

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
