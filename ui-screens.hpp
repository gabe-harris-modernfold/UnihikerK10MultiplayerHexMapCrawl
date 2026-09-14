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
// Built on the 16 px font (12×16 glyphs → 20 columns) with the 24 px title;
// nothing here uses the 8 px font. Top to bottom: header band with uptime,
// a day / threat / weather strip, six fixed-height survivor cards, then a
// two-line network footer. Every colour is from the existing amber palette.
//
//   y   0-28   header band        "WASTELAND"          uptime (right)
//   y  31-47   strip              DAY n   TC n         weather (right)
//   y  53-274  6 cards × 37 px    [n] ARCH name        MP n / DOWN
//                                 L n  F n  W n  R n   + 3 px bars
//   y 281-316  footer             AP ip                local time (right)
//                                 ST ip / status       free heap (right)

static inline int dashRightX(int chars) { return 238 - chars * 12; }

static void drawPlayerScreen() {
  struct {
    bool    on;
    char    name[12];
    uint8_t ll, food, water, radiation;
    uint8_t archetype;
    int8_t  movesLeft;
  } snap[MAX_PLAYERS];
  uint8_t  snapTC = 0, snapWx = 0;
  uint16_t snapDay = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  snapTC  = G.threatClock;
  snapDay = G.dayCount;
  snapWx  = G.weatherPhase;
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
  // Weather strings: 5 chars max so the strip's right column never collides
  // with the threat clock. Index = G.weatherPhase (see WEATHER_* in the .ino).
  static const char*    WX_NAME[6] = {"CLEAR","RAIN","STORM","CHEM","S.FOG","FOG"};

  static const uint32_t C_HDR   = 0xD06818;
  static const uint32_t C_INFO  = 0x904030;
  static const uint32_t C_LINE  = 0x502010;
  static const uint32_t C_TXT   = 0xC87840;
  static const uint32_t C_DIM   = 0x3A1808;
  static const uint32_t C_BAND  = 0x1E0A00;   // header fill / badge digit
  static const uint32_t C_TRACK = 0x2E1206;   // empty bar track

  static const uint32_t C_OK   = 0xC05810;
  static const uint32_t C_WARN = 0xC87020;
  static const uint32_t C_CRIT = 0xE89018;
  static const uint32_t WX_COL[6] = { C_TXT, C_INFO, C_WARN, C_CRIT, C_WARN, C_INFO };

  char buf[40];
  canvas.fillScreen(0x0000);

  // ── Header band ─────────────────────────────────────────────
  canvasRect(0, 0, 240, 29, C_BAND, true);
  canvasText24("WASTELAND", 4, 3, C_HDR);
  canvasLine(0, 29, 239, 29, C_LINE);

  uint32_t upSec = millis() / 1000;
  uint32_t upMin = upSec / 60, upHr = upMin / 60;
  if      (upHr >= 100) snprintf(buf, sizeof(buf), "%luh", (unsigned long)upHr);
  else if (upHr >= 1)   snprintf(buf, sizeof(buf), "%luh%02lum", (unsigned long)upHr, (unsigned long)(upMin % 60));
  else                  snprintf(buf, sizeof(buf), "%lum", (unsigned long)upMin);
  canvasText16(buf, dashRightX((int)strlen(buf)), 7, C_INFO);

  // ── Day / threat / weather strip ────────────────────────────
  canvasText16("DAY", 2, 31, C_INFO);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snapDay);
  canvasText16(buf, 50, 31, C_TXT);
  canvasText16("TC", 98, 31, C_INFO);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snapTC);
  canvasText16(buf, 134, 31, snapTC >= 15 ? C_CRIT : snapTC >= 8 ? C_WARN : C_TXT);
  uint8_t wx = snapWx < 6 ? snapWx : 0;
  canvasText16(WX_NAME[wx], dashRightX((int)strlen(WX_NAME[wx])), 31, WX_COL[wx]);
  canvasLine(0, 50, 239, 50, C_LINE);

  // ── Survivor cards ──────────────────────────────────────────
  // One stat cell: dim label letter, value in its own status colour, and a
  // 3 px bar underneath whose fill is value/max in the same colour. x is the
  // cell's left edge; four cells at 59 px pitch span the 240 px width.
  auto statCell = [&](int x, int y, const char* lbl, uint8_t v, uint8_t vmax, uint32_t col) {
    canvasText16(lbl, x, y, C_INFO);
    char b[4]; snprintf(b, sizeof(b), "%u", (unsigned)v);
    canvasText16(b, x + 12, y, col);
    canvasRect(x, y + 15, x + 55, y + 18, C_TRACK, true);
    int w = (vmax == 0) ? 0 : (55 * (int)(v < vmax ? v : vmax)) / (int)vmax;
    if (w > 0) canvasRect(x, y + 15, x + w, y + 18, col, true);
  };
  static const int CARD_Y0 = 53, CARD_H = 37, CELL_X[4] = { 2, 61, 120, 179 };

  for (int i = 0; i < MAX_PLAYERS; i++) {
    int  y = CARD_Y0 + i * CARD_H;
    char num[2] = { (char)('1' + i), 0 };

    if (!snap[i].on) {
      canvasRect(2, y + 1, 18, y + 17, C_DIM, false);
      canvasText16(num, 4, y + 1, C_DIM);
      canvasText16("offline", 22, y + 1, C_DIM);
      for (int c = 0; c < 4; c++) canvasRect(CELL_X[c], y + 33, CELL_X[c] + 55, y + 36, C_BAND, true);
      continue;
    }

    // Row A: slot badge, archetype, name, moves.
    uint8_t arch = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;
    canvasRect(2, y + 1, 18, y + 17, C_HDR, true);
    canvasText16(num, 4, y + 1, C_BAND);
    canvasText16(ARCH_SHORT[arch], 22, y + 1, C_INFO);
    snprintf(buf, sizeof(buf), "%.8s", snap[i].name);
    canvasText16(buf, 76, y + 1, C_TXT);
    if (snap[i].ll == 0) {
      canvasText16("DOWN", dashRightX(4), y + 1, C_CRIT);
    } else {
      snprintf(buf, sizeof(buf), "MP %d", (int)snap[i].movesLeft);
      canvasText16(buf, dashRightX((int)strlen(buf)), y + 1, C_TXT);
    }

    // Row B: the four survival stats, each judged on its own thresholds
    // (same cut-offs the old single-colour row used).
    uint32_t cL = snap[i].ll        <= 2 ? C_CRIT : snap[i].ll    <= 3 ? C_WARN : C_OK;
    uint32_t cF = snap[i].food      <= 1 ? C_CRIT : snap[i].food  <= 2 ? C_WARN : C_OK;
    uint32_t cW = snap[i].water     <= 1 ? C_CRIT : snap[i].water <= 2 ? C_WARN : C_OK;
    uint32_t cR = snap[i].radiation >= 7 ? C_CRIT : snap[i].radiation >= 4 ? C_WARN : C_OK;
    statCell(CELL_X[0], y + 18, "L", snap[i].ll,        7,  cL);
    statCell(CELL_X[1], y + 18, "F", snap[i].food,      8,  cF);
    statCell(CELL_X[2], y + 18, "W", snap[i].water,     8,  cW);
    statCell(CELL_X[3], y + 18, "R", snap[i].radiation, 10, cR);
  }

  // ── Footer: network ─────────────────────────────────────────
  canvasLine(0, 277, 239, 277, C_LINE);

  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  canvasText16("AP", 2, 281, C_INFO);
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  canvasText16(buf, 38, 281, C_TXT);

  if (checkRtcReady()) {
    time_t nowEpoch = time(nullptr);
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
    struct tm it; localtime_r(&nowEpoch, &it);
    setenv("TZ", "UTC0", 1); tzset();
    snprintf(buf, sizeof(buf), "%02d:%02d", it.tm_hour, it.tm_min);
    canvasText16(buf, dashRightX(5), 281, C_INFO);
  }

  canvasText16("ST", 2, 301, C_INFO);
  uint32_t stColor;
  if (staIp[0] != 0) {
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
    stColor = C_TXT;
  } else if (bootWifiPending) {
    snprintf(buf, sizeof(buf), "connecting");
    stColor = C_INFO;
  } else if (savedSsid[0]) {
    snprintf(buf, sizeof(buf), "%.12s", savedSsid);
    stColor = C_INFO;
  } else {
    snprintf(buf, sizeof(buf), "no creds");
    stColor = C_DIM;
  }
  canvasText16(buf, 38, 301, stColor);

  snprintf(buf, sizeof(buf), "%luk", (unsigned long)(ESP.getFreeHeap() / 1024));
  canvasText16(buf, dashRightX((int)strlen(buf)), 301, C_DIM);
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
// One survivor per page, rotating through the online players every
// RES_PAGE_MS (the LCD repaints every SCREEN_MS, so a flip lands on the first
// repaint after that). Same 16 px-minimum rule and amber palette as screen 1.
//
//   y   0-28   header band        "RESOURCES"          page n/m (right)
//   y  33-48   who                [n] name             ARCH (right)
//   y  56-101  5 resource tiles   WTR FOD FUL MED SCR  (24 px values)
//   y 109-124  pack heading       PACK used/slots
//   y 129-     pack contents      ▪ item name          xqty   (▪ = equipped)

static constexpr uint32_t RES_PAGE_MS = 8000;

static void drawResourceScreen() {
  static const uint32_t C_HDR   = 0xD06818;
  static const uint32_t C_TXT   = 0xC87840;
  static const uint32_t C_INFO  = 0x904030;
  static const uint32_t C_LINE  = 0x502010;
  static const uint32_t C_DIM   = 0x3A1808;
  static const uint32_t C_BAND  = 0x1E0A00;
  static const uint32_t C_WARN  = 0xC87020;
  static const uint32_t C_CRIT  = 0xE89018;
  static const char* ARCH_SHORT[NUM_ARCHETYPES] = {"GUID","QTMR","MEDC","MULE","SCUT","ENDR"};
  static const char* RS[5] = {"WTR","FOD","FUL","MED","SCR"};

  struct PSnap {
    bool    on;
    uint8_t archetype;
    char    name[16];
    uint8_t inv[5];
    uint8_t invType[INV_SLOTS_MAX];
    uint8_t invQty[INV_SLOTS_MAX];
    uint8_t invSlots;
    uint8_t equip[EQUIP_SLOTS];
  } snap[MAX_PLAYERS];

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p         = G.players[i];
    snap[i].on        = p.connected;
    snap[i].archetype = p.archetype;
    memcpy(snap[i].name,    p.name,    16);
    memcpy(snap[i].inv,     p.inv,     5);
    memcpy(snap[i].invType, p.invType, INV_SLOTS_MAX);
    memcpy(snap[i].invQty,  p.invQty,  INV_SLOTS_MAX);
    memcpy(snap[i].equip,   p.equip,   EQUIP_SLOTS);
    snap[i].invSlots = p.invSlots;
  }
  xSemaphoreGive(G.mutex);

  char buf[40];
  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 29, C_BAND, true);
  canvasText24("RESOURCES", 4, 3, C_HDR);
  canvasLine(0, 29, 239, 29, C_LINE);

  // Which survivor is on the page.
  int online[MAX_PLAYERS], cnt = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (snap[i].on) online[cnt++] = i;
  if (cnt == 0) {
    static const char E[] = "No survivors online";
    canvasText16(E, (240 - (int)(sizeof(E) - 1) * 12) / 2, 150, C_DIM);
    return;
  }
  static uint8_t  page   = 0;
  static uint32_t flipMs = 0;
  uint32_t now = millis();
  if (now - flipMs >= RES_PAGE_MS) { page++; flipMs = now; }
  page %= (uint8_t)cnt;
  const int   pid = online[page];
  const PSnap& P  = snap[pid];

  snprintf(buf, sizeof(buf), "%d/%d", (int)page + 1, cnt);
  canvasText16(buf, dashRightX((int)strlen(buf)), 7, C_INFO);

  // ── Who ─────────────────────────────────────────────────────
  char num[2] = { (char)('1' + pid), 0 };
  canvasRect(2, 33, 18, 49, C_HDR, true);
  canvasText16(num, 4, 33, C_BAND);
  snprintf(buf, sizeof(buf), "%.13s", P.name);
  canvasText16(buf, 22, 33, C_TXT);
  uint8_t arch = P.archetype < NUM_ARCHETYPES ? P.archetype : 0;
  canvasText16(ARCH_SHORT[arch], dashRightX(4), 33, C_INFO);
  canvasLine(0, 52, 239, 52, C_LINE);

  // ── Resource tiles ──────────────────────────────────────────
  // 5 × 46 px tiles at 48 px pitch: 3-letter label on top, 24 px count below.
  // A count of 0 is critical, 1-2 is a warning — the same reading the web
  // client gives these bars.
  for (int k = 0; k < 5; k++) {
    int tx = 2 + k * 48, ty = 56;
    canvasRect(tx, ty, tx + 46, ty + 46, C_BAND, true);
    canvasText16(RS[k], tx + 5, ty + 3, C_INFO);
    uint8_t v = P.inv[k];
    snprintf(buf, sizeof(buf), "%u", (unsigned)v);
    uint32_t vc = (v == 0) ? C_CRIT : (v <= 2) ? C_WARN : C_TXT;
    canvasText24(buf, tx + (46 - (int)strlen(buf) * 18) / 2, ty + 19, vc);
  }
  canvasLine(0, 106, 239, 106, C_LINE);

  // ── Pack ────────────────────────────────────────────────────
  int used = 0;
  for (int s = 0; s < P.invSlots && s < INV_SLOTS_MAX; s++) if (P.invType[s]) used++;
  canvasText16("PACK", 2, 109, C_INFO);
  snprintf(buf, sizeof(buf), "%d/%u", used, (unsigned)P.invSlots);
  canvasText16(buf, 62, 109, C_TXT);

  if (used == 0) {
    canvasText16("pack is empty", 14, 131, C_DIM);
    return;
  }
  static constexpr int ROW_H = 17, ROW_Y0 = 129, ROWS_MAX = 11;
  int row = 0, shown = 0;
  for (int s = 0; s < P.invSlots && s < INV_SLOTS_MAX; s++) {
    if (!P.invType[s]) continue;
    if (row == ROWS_MAX - 1 && used - shown > 1) {
      snprintf(buf, sizeof(buf), "+%d more", used - shown);
      canvasText16(buf, 14, ROW_Y0 + row * ROW_H, C_DIM);
      break;
    }
    int y = ROW_Y0 + row * ROW_H;
    bool equipped = false;
    for (int e = 0; e < EQUIP_SLOTS; e++) if (P.equip[e] && P.equip[e] == P.invType[s]) equipped = true;
    if (equipped) canvasRect(2, y + 5, 8, y + 11, C_HDR, true);
    const ItemDef* def = getItemDef(P.invType[s]);
    snprintf(buf, sizeof(buf), "%.14s", def ? def->name : "???");
    canvasText16(buf, 14, y, equipped ? C_TXT : C_INFO);
    snprintf(buf, sizeof(buf), "x%u", (unsigned)P.invQty[s]);
    canvasText16(buf, dashRightX((int)strlen(buf)), y, C_TXT);
    row++; shown++;
  }
}

// ── Screen 4: encounter tracking ──────────────────────────────
// What the encounter system is doing right now, not just a leaderboard:
// sites left on the map, the party's hit rate, the most recent roll in full
// (who, skill, target number, what they rolled), and per survivor whether
// they are inside an encounter (which biome, how much unbanked loot is at
// stake) or, if not, their own record and how their last one ended.
//
//   y   0-28   header band        "ENCOUNTERS"
//   y  31-47   strip              POI left/total       WON wins/rolls (right)
//   y  54-88   last roll          [n] Skill            WON / LOST (right)
//                                 NEED dn  GOT total
//   y  96-317  6 cards × 37 px    [n] name             ENC count (right)
//                                 IN biome   LOOT n  |  WON w/r   last end
//
// The tallies come from the event stream: drainEvents() in
// network-events.hpp calls encStatsNote() for every event it drains (that is
// the one place every EVT_ENC_* passes through). Counters are session-only —
// they are not saved and start at zero after a reboot.

struct EncStats {
  uint16_t rolls, wins;                 // party-wide
  bool     hasLast;
  uint8_t  lastPid, lastSkill, lastDN;
  int8_t   lastTotal;
  uint8_t  lastOut;                     // 1 = success
  struct {
    uint8_t rolls, wins;
    uint8_t lastEnd;                    // ENC_END_* reason
    bool    hasEnd;
  } p[MAX_PLAYERS];
};
static EncStats     encStats    = {};
static portMUX_TYPE encStatsMux = portMUX_INITIALIZER_UNLOCKED;

// Called once per drained GameEvent, from whichever task runs drainEvents().
static void encStatsNote(const GameEvent& ev) {
  if (ev.type != EVT_ENC_RESULT && ev.type != EVT_ENC_END) return;
  if (ev.pid >= MAX_PLAYERS) return;
  taskENTER_CRITICAL(&encStatsMux);
  if (ev.type == EVT_ENC_RESULT) {
    encStats.rolls++;
    if (ev.encOut) encStats.wins++;
    if (encStats.p[ev.pid].rolls < 255) encStats.p[ev.pid].rolls++;
    if (ev.encOut && encStats.p[ev.pid].wins < 255) encStats.p[ev.pid].wins++;
    encStats.hasLast   = true;
    encStats.lastPid   = ev.pid;
    encStats.lastSkill = ev.encSkill;
    encStats.lastDN    = ev.encDN;
    encStats.lastTotal = ev.encTotal;
    encStats.lastOut   = ev.encOut ? 1 : 0;
  } else {
    encStats.p[ev.pid].lastEnd = ev.encOut;
    encStats.p[ev.pid].hasEnd  = true;
  }
  taskEXIT_CRITICAL(&encStatsMux);
}

static void drawEncounterScreen() {
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_TXT  = 0xC87840;
  static const uint32_t C_INFO = 0x904030;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_DIM  = 0x3A1808;
  static const uint32_t C_BAND = 0x1E0A00;
  static const uint32_t C_OK   = 0xC05810;
  static const uint32_t C_WARN = 0xC87020;
  static const uint32_t C_CRIT = 0xE89018;
  static const char* SKILL_UP[NUM_SKILLS] = {"NAVIGATE","FORAGE","SCAVENGE","SHELTER","ENDURE"};
  static const char* END_NAME[ENC_END_COUNT] = {"hazard","abort","dawn","downed","dropped","regen"};

  struct ESnap {
    bool     on, inEnc;
    char     name[16];
    uint16_t encCount;
    char     biome[12];
    uint8_t  loot;          // unbanked resources + items at stake
  } snap[MAX_PLAYERS];
  uint16_t poiLeft = 0, poiTotal = 0;

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      if (G.map[r][c].poi) poiLeft++;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    snap[i].on       = G.players[i].connected;
    snap[i].encCount = G.players[i].encCount;
    memcpy(snap[i].name, G.players[i].name, 16);
    const ActiveEncounter& e = encounters[i];
    snap[i].inEnc = (e.active & 1) != 0;
    snap[i].loot  = 0;
    snap[i].biome[0] = 0;
    if (snap[i].inEnc) {
      uint8_t t = e.terrain < 10 ? e.terrain : 0;
      strlcpy(snap[i].biome, encPools[t].path, sizeof(snap[i].biome));
      int l = e.pendingItemCount;
      for (int k = 0; k < 5; k++) l += e.pendingLoot[k];
      snap[i].loot = (uint8_t)min(99, l);
    }
  }
  xSemaphoreGive(G.mutex);
  // Every encounter file is placed once at map generation, so the pool sizes
  // are the number of sites the map started with.
  for (int t = 0; t < 10; t++) poiTotal += encPools[t].count;

  EncStats st;
  taskENTER_CRITICAL(&encStatsMux);
  st = encStats;
  taskEXIT_CRITICAL(&encStatsMux);

  char buf[40];
  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 29, C_BAND, true);
  canvasText24("ENCOUNTERS", 4, 3, C_HDR);
  canvasLine(0, 29, 239, 29, C_LINE);

  // ── Strip: sites left, party hit rate ───────────────────────
  canvasText16("POI", 2, 31, C_INFO);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)poiLeft, (unsigned)poiTotal);
  canvasText16(buf, 50, 31, poiLeft == 0 ? C_DIM : C_TXT);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)st.wins, (unsigned)st.rolls);
  int wx = dashRightX((int)strlen(buf));
  canvasText16(buf, wx, 31, st.rolls == 0 ? C_DIM : C_TXT);
  canvasText16("WON", wx - 48, 31, C_INFO);
  canvasLine(0, 50, 239, 50, C_LINE);

  // ── Last roll ───────────────────────────────────────────────
  if (st.hasLast) {
    char num[2] = { (char)('1' + (st.lastPid < MAX_PLAYERS ? st.lastPid : 0)), 0 };
    canvasRect(2, 54, 18, 70, C_HDR, true);
    canvasText16(num, 4, 54, C_BAND);
    canvasText16(SKILL_UP[st.lastSkill < NUM_SKILLS ? st.lastSkill : 0], 22, 54, C_TXT);
    if (st.lastOut) canvasText16("WON",  dashRightX(3), 54, C_OK);
    else            canvasText16("LOST", dashRightX(4), 54, C_CRIT);
    canvasText16("NEED", 22, 72, C_INFO);
    snprintf(buf, sizeof(buf), "%u", (unsigned)st.lastDN);
    canvasText16(buf, 82, 72, C_TXT);
    canvasText16("GOT", 130, 72, C_INFO);
    snprintf(buf, sizeof(buf), "%d", (int)st.lastTotal);
    canvasText16(buf, 178, 72, st.lastOut ? C_OK : C_CRIT);
  } else {
    canvasText16("No rolls yet", 22, 63, C_DIM);
  }
  canvasLine(0, 92, 239, 92, C_LINE);

  // ── Survivor cards ──────────────────────────────────────────
  static const int CARD_Y0 = 96, CARD_H = 37;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    int  y = CARD_Y0 + i * CARD_H;
    char num[2] = { (char)('1' + i), 0 };

    if (!snap[i].on) {
      canvasRect(2, y + 1, 18, y + 17, C_DIM, false);
      canvasText16(num, 4, y + 1, C_DIM);
      canvasText16("offline", 22, y + 1, C_DIM);
      continue;
    }

    // Row A: badge (hot while inside an encounter), name, lifetime count.
    canvasRect(2, y + 1, 18, y + 17, snap[i].inEnc ? C_CRIT : C_HDR, true);
    canvasText16(num, 4, y + 1, C_BAND);
    snprintf(buf, sizeof(buf), "%.10s", snap[i].name);
    canvasText16(buf, 22, y + 1, C_TXT);
    snprintf(buf, sizeof(buf), "ENC %u", (unsigned)snap[i].encCount);
    canvasText16(buf, dashRightX((int)strlen(buf)), y + 1, C_INFO);

    // Row B: live encounter, or the record so far.
    if (snap[i].inEnc) {
      canvasText16("IN", 22, y + 18, C_INFO);
      snprintf(buf, sizeof(buf), "%.8s", snap[i].biome);
      canvasText16(buf, 58, y + 18, C_WARN);
      snprintf(buf, sizeof(buf), "LOOT %u", (unsigned)snap[i].loot);
      canvasText16(buf, dashRightX((int)strlen(buf)), y + 18, snap[i].loot ? C_TXT : C_DIM);
    } else {
      if (st.p[i].rolls == 0) {
        canvasText16("no rolls yet", 22, y + 18, C_DIM);
      } else {
        canvasText16("WON", 22, y + 18, C_INFO);
        snprintf(buf, sizeof(buf), "%u/%u", (unsigned)st.p[i].wins, (unsigned)st.p[i].rolls);
        canvasText16(buf, 70, y + 18, C_TXT);
      }
      if (st.p[i].hasEnd) {
        const char* r = END_NAME[st.p[i].lastEnd < ENC_END_COUNT ? st.p[i].lastEnd : 0];
        uint32_t rc = (st.p[i].lastEnd == ENC_END_DOWNED || st.p[i].lastEnd == ENC_END_HAZARD) ? C_WARN : C_INFO;
        canvasText16(r, dashRightX((int)strlen(r)), y + 18, rc);
      }
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
