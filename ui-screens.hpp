#pragma once
// ── ui-screens.hpp ───────────────────────────────────────────────────────────
// All 5 K10 display screens and button-B screen cycling.

// ── World entity markers for the minimap ─────────────────────────────────────
// The map screen plots the Caravan and Creeping Doom alongside the players, but
// world-system.hpp (which owns `W`) is included *after* this file — see the
// include block in Esp32HexMapCrawl.ino. Declare the accessor here and define
// it there; drawMapScreen() only ever calls it at runtime, so the prototype is
// all this translation unit needs. Caller must hold G.mutex: W is protected by
// it exactly like G.map/G.players (see the world-tick block in
// actions_game_loop.hpp).
struct WorldMarkers {
  int16_t caravanQ, caravanR;
  bool    caravanActive;
  int16_t doomQ, doomR;
  uint8_t doomAwareness;   // 0-100
  int16_t doomRadius;      // hexes of scent detection at the current awareness
};
static void snapshotWorldMarkers(WorldMarkers& out);

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

// ── Screen 2: the chronicle ────────────────────────────────────
// The event log, rendered as a hand-kept book rather than a console: a ruled
// left margin, day headings struck across the page, entries written top-down
// in the order they happened, and the ink fading the further back you read.
// The pen nib sits at the end of the last line — this is a book still being
// written.

static constexpr uint8_t BOOK_COLS   = 37;   // 6px glyphs inside the margin
static constexpr int     BOOK_LINE_H = 9;
static constexpr int     BOOK_X      = 14;
static constexpr int     BOOK_TOP    = 37;
static constexpr int     BOOK_BOT    = 298;
static constexpr int     BOOK_GAP    = 3;    // breath between entries
static constexpr int     BOOK_HEAD_H = 12;   // a day heading's block height
static constexpr uint8_t BOOK_WRAP_MAX = 4;

// Ink by [tone][age tier]: fresh at the nib, drying as it goes up the page.
static const uint32_t BOOK_INK[TONE_COUNT][4] = {
  { 0xE8CFA6, 0xB89C74, 0x8A7150, 0x604A31 },  // plain — sepia
  { 0xF2D278, 0xC0A458, 0x8C7740, 0x60522D },  // good  — gold
  { 0xE28450, 0xB4603A, 0x82452A, 0x5A311D },  // ill   — rust
  { 0xB0A6C8, 0x8A83A0, 0x635F76, 0x454153 },  // omen  — cold
};

// Name to write for a player: their own if they have one, else a stand-in.
static const char* bookName(int8_t pid, const char names[][16]) {
  static char fallback[12];
  if (pid < 0 || pid >= MAX_PLAYERS) return "Someone";
  if (names[pid][0]) return names[pid];
  snprintf(fallback, sizeof(fallback), "Walker %d", (int)pid + 1);
  return fallback;
}

// Expand one entry into the whole sentence the page shows.
static void bookSentence(const K10LogEntry& e, const char names[][16],
                         char* out, size_t cap) {
  size_t n = 0;
  if (e.who >= 0) {
    const char* nm = bookName(e.who, names);
    while (*nm && n + 2 < cap) out[n++] = *nm++;
    if (n + 2 < cap) out[n++] = ' ';
  }
  for (const char* p = e.text; *p && n + 1 < cap; p++) {
    if (*p == '\x01') {
      const char* nm = bookName(e.who2, names);
      while (*nm && n + 1 < cap) out[n++] = *nm++;
    } else {
      out[n++] = *p;
    }
  }
  out[n] = '\0';
}

// Break `s` onto word-wrapped lines of at most BOOK_COLS characters.
// Returns the number of lines written (>= 1 for non-empty input).
static uint8_t bookWrap(const char* s, char out[][BOOK_COLS + 1], uint8_t maxLines) {
  uint8_t n = 0;
  while (*s && n < maxLines) {
    while (*s == ' ') s++;
    if (!*s) break;
    size_t len = strlen(s);
    if (len <= BOOK_COLS) { strlcpy(out[n++], s, BOOK_COLS + 1); break; }
    uint8_t cut = BOOK_COLS;
    while (cut > 0 && s[cut] != ' ') cut--;
    if (cut == 0) cut = BOOK_COLS;             // one unbroken word — hard break
    memcpy(out[n], s, cut);
    out[n][cut] = '\0';
    n++;
    s += cut;
  }
  return n;
}

// "———  Day 4  ———" struck across the page.
static void bookDayRule(int y, uint16_t day, uint32_t col, uint32_t rule) {
  char d[16];
  snprintf(d, sizeof(d), "Day %u", (unsigned)day);
  int w  = (int)strlen(d) * 6;
  int x0 = 120 - w / 2;
  canvasText8(d, x0, y + 2, col);
  canvasLine(BOOK_X, y + 5, x0 - 6, y + 5, rule);
  canvasLine(x0 + w + 5, y + 5, 226, y + 5, rule);
}

static void drawEventLogScreen() {
  static const uint32_t C_TITLE = 0xD06818;
  static const uint32_t C_SUB   = 0x6A4020;
  static const uint32_t C_RULE  = 0x502010;
  static const uint32_t C_MARG  = 0x2E1206;
  static const uint32_t C_DIM   = 0x3A1808;

  K10LogEntry snap[K10_LOG_SIZE];
  uint8_t  snapHead = 0, snapCount = 0;
  uint16_t total    = 0;
  taskENTER_CRITICAL(&k10LogMux);
  memcpy(snap, k10Log, sizeof(k10Log));
  snapHead  = k10LogHead;
  snapCount = k10LogCount;
  total     = k10LogTotal;
  taskEXIT_CRITICAL(&k10LogMux);

  char     names[MAX_PLAYERS][16] = {};
  uint16_t today = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) memcpy(names[i], G.players[i].name, 16);
    today = G.dayCount;
    xSemaphoreGive(G.mutex);
  }

  canvas.fillScreen(0x0000);

  // ── Title page furniture ────────────────────────────────────
  static const char TITLE[] = "The Chronicle";
  canvasText16(TITLE, (240 - (int)(sizeof(TITLE) - 1) * 12) / 2, 3, C_TITLE);
  char sub[40];
  snprintf(sub, sizeof(sub), "kept by hand, day %u", (unsigned)today);
  canvasText8(sub, (240 - (int)strlen(sub) * 6) / 2, 22, C_SUB);
  canvasLine(6, 31, 233, 31, C_RULE);
  canvasLine(6, 33, 233, 33, C_MARG);

  if (snapCount == 0) {
    static const char E1[] = "The page is still blank.";
    static const char E2[] = "Nothing yet worth setting down.";
    canvasText8(E1, (240 - (int)(sizeof(E1) - 1) * 6) / 2, 150, C_SUB);
    canvasText8(E2, (240 - (int)(sizeof(E2) - 1) * 6) / 2, 163, C_DIM);
  } else {
    canvasLine(9, BOOK_TOP, 9, BOOK_BOT, C_MARG);   // the ruled margin

    char sent[96];
    char wrap[BOOK_WRAP_MAX][BOOK_COLS + 1];

    // Measure backwards from the newest entry to find the oldest one that
    // still fits, so the page always ends at the pen.
    const int budget = BOOK_BOT - BOOK_TOP;
    int first = (int)snapCount - 1, used = 0;
    for (int i = (int)snapCount - 1; i >= 0; i--) {
      uint8_t idx = (snapHead + (uint8_t)i) % K10_LOG_SIZE;
      bookSentence(snap[idx], names, sent, sizeof(sent));
      int cost = bookWrap(sent, wrap, BOOK_WRAP_MAX) * BOOK_LINE_H + BOOK_GAP;
      uint8_t prev = (snapHead + (uint8_t)(i - 1)) % K10_LOG_SIZE;
      bool ownHeading = (i == 0) || (snap[prev].day != snap[idx].day);
      if (ownHeading) cost += BOOK_HEAD_H;
      // Whichever entry ends up at the top of the page is given a heading
      // whether or not its day differs, so reserve one here too — otherwise
      // the newest entry gets pushed off the foot of the page.
      int fits = used + cost + (ownHeading ? 0 : BOOK_HEAD_H);
      if (fits > budget && i != (int)snapCount - 1) break;
      used  += cost;
      first  = i;
    }

    // Write the page top-down, oldest first — the way a book fills.
    int y = BOOK_TOP;
    for (int i = first; i < (int)snapCount; i++) {
      uint8_t idx  = (snapHead + (uint8_t)i) % K10_LOG_SIZE;
      uint8_t prev = (snapHead + (uint8_t)(i - 1)) % K10_LOG_SIZE;
      uint8_t age  = (uint8_t)(snapCount - 1 - i);
      uint8_t tier = (age == 0) ? 0 : (age <= 2) ? 1 : (age <= 5) ? 2 : 3;
      uint8_t tone = (snap[idx].tone < TONE_COUNT) ? snap[idx].tone : TONE_PLAIN;
      uint32_t ink = BOOK_INK[tone][tier];

      if (i == first || snap[prev].day != snap[idx].day) {
        if (y + BOOK_HEAD_H > BOOK_BOT) break;
        bookDayRule(y, snap[idx].day, C_SUB, C_MARG);
        y += BOOK_HEAD_H;
      }

      bookSentence(snap[idx], names, sent, sizeof(sent));
      uint8_t lines = bookWrap(sent, wrap, BOOK_WRAP_MAX);
      for (uint8_t l = 0; l < lines; l++) {
        if (y + BOOK_LINE_H > BOOK_BOT) break;
        canvasText8(wrap[l], BOOK_X, y, ink);
        // The nib rests where the last word ended.
        if (i == (int)snapCount - 1 && l == lines - 1) {
          int nx = BOOK_X + (int)strlen(wrap[l]) * 6 + 2;
          if (nx < 232) canvasRect(nx, y, nx + 2, y + 8, ink, true);
        }
        y += BOOK_LINE_H;
      }
      y += BOOK_GAP;
    }
  }

  // ── Page number ─────────────────────────────────────────────
  canvasLine(6, 302, 233, 302, C_MARG);
  char pg[16];
  snprintf(pg, sizeof(pg), "- %u -", (unsigned)total);
  canvasText8(pg, (240 - (int)strlen(pg) * 6) / 2, 307, C_SUB);
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

  PSRAM_STATIC(uint8_t, terr, [MAP_ROWS][MAP_COLS]);
  struct { int16_t q, r; bool on; } ps[MAX_PLAYERS];
  WorldMarkers wm = {};

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      terr[r][c] = G.map[r][c].terrain;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    ps[i].q  = G.players[i].q;
    ps[i].r  = G.players[i].r;
    ps[i].on = G.players[i].connected;
  }
  snapshotWorldMarkers(wm);   // same critical section — W lives under G.mutex too
  xSemaphoreGive(G.mutex);

  // shrink-to-fit: derive cell size from screen + map dims (240×320 LCD)
  static constexpr int MAP_PX_W = 234;
  static constexpr int MAP_PX_H = 304;
  static constexpr int XS = MAP_PX_W / MAP_COLS;
  static constexpr int YS = MAP_PX_H / (MAP_ROWS + 1);   // +1 row of slack for odd-col offset
  static constexpr int CW = XS;
  static constexpr int CH = YS;
  static constexpr int OY = YS / 2;
  static constexpr int MX = (240 - MAP_COLS * XS) / 2;
  static constexpr int MY = 6;

  canvas.fillScreen(0x0000);

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      int px = MX + q * XS;
      int py = MY + r * YS + (q & 1) * OY;
      uint8_t t = terr[r][q];
      uint32_t col = (t < 11) ? TERR_COL[t] : 0x3A1808;
      // at small cell sizes, drop the 1-px gap so cells read as a continuous map
      int rectW = (CW > 2) ? CW - 1 : CW;
      int rectH = (CH > 2) ? CH - 1 : CH;
      canvas.fillRect(px, py, rectW, rectH, c16(col));
    }
  }

  // ── World entities ───────────────────────────────────────────────────────
  // Drawn under the players so a survivor standing on the trader's hex (or in
  // the Doom's jaws) still reads as a player marker.
  //
  // Cell→pixel for a marker, hoisted so all three markers agree.
  auto cellPx = [&](int q, int r, int& px, int& py) {
    px = MX + q * XS;
    py = MY + r * YS + (q & 1) * OY;
  };
  bool doomOnMap = (wm.doomQ >= 0 && wm.doomQ < MAP_COLS && wm.doomR >= 0 && wm.doomR < MAP_ROWS);

  // Scent-detection ring: the radius inside which Doom can pick up tracks
  // (doomDetectionRadius() = 2 + awareness/25). Drawn as an ellipse because the
  // offset-column layout has different x and y pitches — it approximates the
  // hex disc closely enough to read as "this is how far it can smell you".
  // No wrap-around copy: the canvas clips at the edges, which is honest enough.
  if (doomOnMap && wm.doomRadius > 0) {
    int dpx, dpy; cellPx(wm.doomQ, wm.doomR, dpx, dpy);
    canvas.drawEllipse(dpx + CW / 2, dpy + CH / 2,
                       wm.doomRadius * XS, wm.doomRadius * YS, c16(0x803010));
  }

  // Creeping Doom. Colour tracks the awareness thresholds the behaviour itself
  // keys off (see tickCreepingDoom/resolveDoomProximity in world-system.hpp):
  //   <51 dormant · 51-75 warns nearby survivors · 76-99 ignores terrain and
  //   sets fires · 100 hunts the nearest survivor outright.
  if (doomOnMap) {
    uint32_t dcol = (wm.doomAwareness >= 100) ? 0xFF3000
                  : (wm.doomAwareness >=  76) ? 0xFF4008
                  : (wm.doomAwareness >=  51) ? 0xD03810
                                              : 0x903018;
    int dpx, dpy; cellPx(wm.doomQ, wm.doomR, dpx, dpy);
    // A dark halo one pixel out keeps the marker legible over bright terrain
    // (Glass Fields / Settlement are near the same value as the hot reds).
    canvas.drawRect(dpx - 1, dpy - 1, CW + 2, CH + 2, c16(0x180800));
    canvas.fillRect(dpx, dpy, CW, CH, c16(dcol));
    // Hunting: a white core so "it is coming for someone" is unmistakable.
    if (wm.doomAwareness >= 100)
      canvas.fillRect(dpx + CW / 2, dpy + CH / 2, 1, 1, c16(0xFFFFFF));
  }

  // Caravan (the trader). Teal reads as "not terrain, not danger" against the
  // amber map and the Doom's reds.
  bool caravanOnMap = wm.caravanActive &&
                      wm.caravanQ >= 0 && wm.caravanQ < MAP_COLS &&
                      wm.caravanR >= 0 && wm.caravanR < MAP_ROWS;
  if (caravanOnMap) {
    int cpx, cpy; cellPx(wm.caravanQ, wm.caravanR, cpx, cpy);
    canvas.drawRect(cpx - 1, cpy - 1, CW + 2, CH + 2, c16(0x041810));
    canvas.fillRect(cpx, cpy, CW, CH, c16(0x30E0A0));
  }

  // Player marker: at small cell sizes a digit no longer fits; draw a bright filled
  // square sized to the cell. Fall back to a digit when cells are big enough.
  bool bigCells = (CW >= 8 && CH >= 12);
  if (bigCells) canvas.setTextSize(2);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!ps[i].on) continue;
    int q = ps[i].q, r = ps[i].r;
    if (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS) continue;
    int px = MX + q * XS;
    int py = MY + r * YS + (q & 1) * OY;
    if (bigCells) {
      char num[2] = { (char)('1' + i), 0 };
      canvas.setTextColor(c16(0xD06818));
      canvas.setCursor(px, py);
      canvas.print(num);
    } else {
      // Same dark halo the world markers get: 0xFFD060 sits right on top of
      // Settlement (0xE8A828) and Glass Fields (0xC08028) in the terrain
      // palette, so an un-outlined player square disappears over those.
      canvas.drawRect(px - 1, py - 1, CW + 2, CH + 2, c16(0x180C00));
      canvas.fillRect(px, py, CW, CH, c16(0xFFD060));
    }
  }

  int bx1 = MX - 1;
  int by1 = MY - 1;
  int bx2 = MX + (MAP_COLS - 1) * XS + CW;
  int by2 = MY + (MAP_ROWS - 1) * YS + OY + CH;
  canvas.drawRect(bx1, by1, bx2 - bx1, by2 - by1, c16(0xD06818));

  // ── Legend ───────────────────────────────────────────────────────────────
  // At 3×5-pixel cells the three marker colours are the only thing telling the
  // entities apart, so name them. Sits in the ~26 px left below the map border.
  // 8px font = 6 px per character, which is what the x-advances below assume.
  {
    int ly = by2 + 6;
    int lx = MX;
    auto legend = [&](uint32_t col, const char* label, int chars) {
      canvas.fillRect(lx, ly + 1, 4, 6, c16(col));
      canvasText8(label, lx + 6, ly, 0xC0A080);
      lx += 6 + chars * 6 + 6;
    };
    legend(0xFFD060, "YOU", 3);
    legend(caravanOnMap ? 0x30E0A0 : 0x1A4438, "TRADE", 5);
    legend(0xFF4008, "DOOM", 4);
    // Awareness is the number that decides whether the Doom is wandering or
    // hunting — worth a readout next to its swatch.
    char aw[10];
    snprintf(aw, sizeof(aw), "%d%%", (int)wm.doomAwareness);
    canvasText8(aw, lx, ly, wm.doomAwareness >= 76 ? 0xFF4008 : 0xC0A080);
  }
}

// ── K10 button B screen switching ─────────────────────────────
// Screens: 1=Players 2=Events 3=Resources 4=Encounters 5=Map
static void checkGestureSwitch() {
  bool btnB = k10.buttonB && k10.buttonB->isPressed();
  if (btnB && !k10BtnBLast) { k10Screen = (k10Screen % 5) + 1; k10Play(MOTIF_SCREEN_CLICK); }
  k10BtnBLast = btnB;
}
