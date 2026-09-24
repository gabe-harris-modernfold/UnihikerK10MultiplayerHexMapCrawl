#pragma once
// ── ui-screens.hpp ───────────────────────────────────────────────────────────
// All 6 K10 display screens and button-B screen cycling.

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

// Defined in inventory_items.hpp, which is included *after* this file: pack
// size in effect right now = archetype base + every equipped item's "slots"
// bonus (Hoarder's Rig, Backpack, Knife-Wrench), capped at INV_SLOTS_MAX.
// The resource screen's PACK readout has to use this, not Player::invSlots,
// or the bonus slots and everything sitting in them go missing from the LCD.
// Caller must hold G.mutex.
static uint8_t effectiveInvSlots(const Player& p);
// Same deal: the survivor's LL ceiling, 7 plus every equipped item's "ll"
// bonus minus any permanent penalty.  The dashboard's LIFE bar is drawn
// against it rather than a literal 7.
static uint8_t effectiveMaxLL(int pid);

// -- Screen 1: player status dashboard --------------------------
// Three faces, and which one a thing is set in is what it IS, not decoration
// (the table is in the font block in ui-helpers.hpp):
//
//   Font4  26 px  the masthead, and the two numbers that describe the whole
//                 party at once -- the day, and the threat clock. Nothing
//                 per-survivor is ever this large, so the eye finds the party
//                 state without reading a word.
//   Font2  16 px  everything a person actually reads: names, stat values,
//                 the weather, the addresses.
//   Font0   6x8   every label. At 6 px a label costs a third of what it did
//                 on the 12x16 grid, which is why nothing on this screen is
//                 abbreviated any more: the labels that used to read TC, MP,
//                 L/F/W/R, AP and ST are now THREAT, MOVES, LIFE/FOOD/WATER/
//                 RADS, HOTSPOT and NETWORK, and the archetype is its real
//                 name out of ARCHETYPE_NAME instead of a four-letter code.
//                 Small type buys words; it is not a consolation prize.
//
//   y   0-29   header band    "WASTELAND" 26 px         uptime 16 px (right)
//   y  31-58   strip          DAY 12  THREAT 4 (26 px)  weather 16 px (right)
//   y  60-281  6 cards x 37   [n] name    archetype     MOVES n / DOWN
//                             LIFE n  FOOD n  WATER n  RADS n  + 3 px bars
//   y 285-319  footer         HOTSPOT ip                local time (right)
//                             NETWORK ip / status       free heap (right)

// The column every screen aligns its right-hand edge to. Proportional faces
// cannot be positioned by counting characters, so the canvasText*R helpers in
// ui-helpers.hpp take this as the column the string ENDS at.
static constexpr int DASH_R = 238;

static void drawPlayerScreen() {
  struct {
    bool    on;
    char    name[12];
    uint8_t ll, food, water, radiation;
    uint8_t llCap;                     // effectiveMaxLL(), not a literal 7
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
    snap[i].llCap      = effectiveMaxLL(i);   // base 7 + equipment - penalties
    snap[i].food       = p.food;
    snap[i].water      = p.water;
    snap[i].radiation  = p.radiation;
    snap[i].archetype  = p.archetype;
    snap[i].movesLeft  = p.movesLeft;
    memcpy(snap[i].name, p.name, 12);
  }
  xSemaphoreGive(G.mutex);

  // Weather names are set in the 16 px face and right-aligned by measurement,
  // so the old 5-character budget that forced "S.FOG" is gone.
  static const char* WX_NAME[6] = {"CLEAR","RAIN","STORM","CHEM RAIN","SMOG","FOG"};

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

  // -- Header band ---------------------------------------------
  canvasRect(0, 0, 240, 29, C_BAND, true);
  canvasText26p("WASTELAND", 4, 1, C_HDR);

  uint32_t upSec = millis() / 1000;
  uint32_t upMin = upSec / 60, upHr = upMin / 60;
  if      (upHr >= 100) snprintf(buf, sizeof(buf), "%luh", (unsigned long)upHr);
  else if (upHr >= 1)   snprintf(buf, sizeof(buf), "%luh%02lum", (unsigned long)upHr, (unsigned long)(upMin % 60));
  else                  snprintf(buf, sizeof(buf), "%lum", (unsigned long)upMin);
  canvasText16pR(buf, DASH_R, 7, C_INFO);
  canvasLine(0, 29, 239, 29, C_LINE);

  // -- Day / threat / weather strip ----------------------------
  // The only two 26 px numerals on the screen. Their labels sit beside them
  // at mid-height rather than above: the strip is 28 px and the numerals want
  // all of it.
  canvasText8("DAY", 2, 40, C_INFO);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snapDay);
  canvasText26p(buf, 24, 31, C_TXT);
  canvasText8("THREAT", 76, 40, C_INFO);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snapTC);
  canvasText26p(buf, 116, 31, snapTC >= 15 ? C_CRIT : snapTC >= 8 ? C_WARN : C_TXT);
  uint8_t wx = snapWx < 6 ? snapWx : 0;
  canvasText16pR(WX_NAME[wx], DASH_R, 36, WX_COL[wx]);
  canvasLine(0, 58, 239, 58, C_LINE);

  // -- Survivor cards ------------------------------------------
  // One stat cell: the stat spelled out at 6x8, the value at 16 px in its own
  // status colour, and a 3 px bar underneath filled value/max in that colour.
  // The value sits at a fixed offset rather than after the label, so all four
  // numbers line up down the row whatever the labels are.
  auto statCell = [&](int x, int y, const char* lbl, uint8_t v, uint8_t vmax, uint32_t col) {
    canvasText8(lbl, x, y + 4, C_INFO);
    char b[4]; snprintf(b, sizeof(b), "%u", (unsigned)v);
    canvasText16p(b, x + 36, y, col);
    // y+15..y+17, so row y+18 is blank and the bar does not run straight into
    // the next card's badge 1 px later.
    canvasRect(x, y + 15, x + 55, y + 18, C_TRACK, true);
    int w = (vmax == 0) ? 0 : (55 * (int)(v < vmax ? v : vmax)) / (int)vmax;
    if (w > 0) canvasRect(x, y + 15, x + w, y + 18, col, true);
  };
  static const int CARD_Y0 = 60, CARD_H = 37, CELL_X[4] = { 2, 61, 120, 179 };

  for (int i = 0; i < MAX_PLAYERS; i++) {
    int  y = CARD_Y0 + i * CARD_H;
    char num[2] = { (char)('1' + i), 0 };

    if (!snap[i].on) {
      canvasRect(2, y, 18, y + 16, C_DIM, false);
      canvasText16pC(num, 10, y, C_DIM);
      canvasText16p("offline", 24, y, C_DIM);
      for (int c = 0; c < 4; c++) canvasRect(CELL_X[c], y + 33, CELL_X[c] + 55, y + 36, C_BAND, true);
      continue;
    }

    // Row A: slot badge, name, archetype, moves. The name is a person so it
    // is 16 px; the archetype is a role so it is 6x8 -- but it is the whole
    // word, because at 6 px "Quartermaster" costs 78 px and there is room.
    //
    // Laid out right to left, because everything on the right is fixed and
    // the name is what has to give. `nameR` walks in past the moves block and
    // then past the archetype, and the name takes whatever is left of the row
    // -- measured, since the default names are literally the archetype with a
    // digit on the end and "Quartermaster1" is twice the width of "Mule1".
    uint8_t arch = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;
    canvasRect(2, y, 18, y + 16, C_HDR, true);
    canvasText16pC(num, 10, y, C_BAND);

    int nameR;
    if (snap[i].ll == 0) {
      canvasText16pR("DOWN", DASH_R, y, C_CRIT);
      nameR = DASH_R - canvasWidth16p("DOWN") - 8;
    } else {
      snprintf(buf, sizeof(buf), "%d", (int)snap[i].movesLeft);
      canvasText16pR(buf, DASH_R, y, C_TXT);
      int lx = DASH_R - canvasWidth16p(buf) - 5;
      canvasText8R("MOVES", lx, y + 4, C_INFO);
      nameR = lx - 5 * 6 - 8;
    }
    const char* an = ARCHETYPE_NAME[arch];
    canvasText8R(an, nameR, y + 4, C_INFO);
    nameR -= (int)strlen(an) * 6 + 10;

    char nm[13];
    snprintf(nm, sizeof(nm), "%.12s", snap[i].name);
    canvasFit16p(buf, sizeof(buf), nm, nameR - 22);
    canvasText16p(buf, 22, y, C_TXT);

    // Row B: the four survival stats, each judged on its own thresholds.
    uint32_t cL = snap[i].ll        <= 2 ? C_CRIT : snap[i].ll    <= 3 ? C_WARN : C_OK;
    // LIFE is drawn against the survivor's OWN ceiling, not a literal 7:
    // +LL equipment (Dent Absorber, Bear Skin Cape) raises it and Uranium
    // Candy lowers it, and a fixed 7 pinned the bar full for anyone wearing
    // armour -- the bonus was real and completely invisible.
    uint32_t cF = snap[i].food      <= 1 ? C_CRIT : snap[i].food  <= 2 ? C_WARN : C_OK;
    uint32_t cW = snap[i].water     <= 1 ? C_CRIT : snap[i].water <= 2 ? C_WARN : C_OK;
    uint32_t cR = snap[i].radiation >= 7 ? C_CRIT : snap[i].radiation >= 4 ? C_WARN : C_OK;
    statCell(CELL_X[0], y + 18, "LIFE",  snap[i].ll,  snap[i].llCap,  cL);
    statCell(CELL_X[1], y + 18, "FOOD",  snap[i].food,      8,  cF);
    statCell(CELL_X[2], y + 18, "WATER", snap[i].water,     8,  cW);
    statCell(CELL_X[3], y + 18, "RADS",  snap[i].radiation, 10, cR);
  }

  // -- Footer: network -----------------------------------------
  // Addresses get typed into a phone, so they are 16 px; what they are is a
  // 6x8 word. The old footer set label and address at the same size, and the
  // label read as loudly as the thing it labelled.
  canvasLine(0, 283, 239, 283, C_LINE);

  IPAddress apIp  = WiFi.softAPIP();
  IPAddress staIp = WiFi.localIP();
  canvasText8("HOTSPOT", 2, 289, C_INFO);
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  canvasText16p(buf, 48, 285, C_TXT);

  if (checkRtcReady()) {
    time_t nowEpoch = time(nullptr);
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
    struct tm it; localtime_r(&nowEpoch, &it);
    setenv("TZ", "UTC0", 1); tzset();
    snprintf(buf, sizeof(buf), "%02d:%02d", it.tm_hour, it.tm_min);
    canvasText16pR(buf, DASH_R, 285, C_INFO);
  }

  canvasText8("NETWORK", 2, 307, C_INFO);
  uint32_t stColor;
  if (staIp[0] != 0) {
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
    stColor = C_TXT;
  } else if (bootWifiPending) {
    snprintf(buf, sizeof(buf), "connecting");
    stColor = C_INFO;
  } else if (savedSsid[0]) {
    snprintf(buf, sizeof(buf), "%.14s", savedSsid);
    stColor = C_INFO;
  } else {
    snprintf(buf, sizeof(buf), "no credentials");
    stColor = C_DIM;
  }
  canvasText16p(buf, 48, 303, stColor);

  snprintf(buf, sizeof(buf), "%luk", (unsigned long)(ESP.getFreeHeap() / 1024));
  canvasText8R(buf, DASH_R, 307, C_DIM);
}

// ── Screen 2: the chronicle ────────────────────────────────────
// The event log, rendered as a hand-kept book rather than a console: a ruled
// left margin, a chapter rule struck across the page for each day, entries
// written top-down in the order they happened, and the ink fading the further
// back you read. The pen nib sits at the end of the last line — this is a book
// still being written.
//
// Three faces are set on one page, and which one an entry gets is its *age*,
// not a style choice (BOOK_FACE_AGE). The panel is an ILI9341 and LovyanGFX
// ships the whole classic TFT_eSPI table, so all of them are just fonts::FontN
// — see the font block in ui-helpers.hpp:
//
//   Font4  26 px  the masthead, and the dropped capital opening the page
//   Font2  16 px  the freshest entries, the open chapter's numeral, the folio
//   Font0   6x8   everything that has receded: older entries, the margin
//                 marks, the plates, the small labels
//
// so the page reads large at the pen and shrinks as it goes back — the type
// scale *is* the clock.
//
// Between the ruled margin and the words runs a gutter of marks: each entry
// carries a small 3x2-character pictogram of the thing that happened, set on
// the same two baselines as its first two lines so the mark and the sentence
// read as one block. See BOOK_GLYPH below and K10Glyph in the .ino.
//
// The entry at the head of the page is illuminated instead of marked: its
// first letter is lifted out of the sentence and set in the 26 px face inside
// a gilded box with a vine let down into the margin, and the lines beside it
// are ruled short so the text steps around the letter. That runaround is the
// `narrow` count in bookWrapPx(), and it is why the fitting pass has to
// measure the head of the page *as illuminated* (bookLayout's `illum`) — a
// capital added after the fact would push the newest line off the foot.

static constexpr int     BOOK_LINE_H = 9;    // the 6x8 hand's line pitch
static constexpr int     BOOK_LINE_P = 17;   // the 16 px face's line pitch
static constexpr int     BOOK_MARK_X = 12;   // the margin mark: 3 chars wide
static constexpr int     BOOK_X      = 34;   // BOOK_MARK_X + 3*6 + 4 of air
static constexpr int     BOOK_R      = 232;  // right edge of the text column
static constexpr int     BOOK_TOP    = 36;
static constexpr int     BOOK_BOT    = 297;
static constexpr int     BOOK_GAP    = 3;    // breath between entries
static constexpr int     BOOK_HEAD_H = 12;   // a closed chapter's rule
static constexpr int     BOOK_HEAD_P = 20;   // the open chapter's rule
static constexpr uint8_t BOOK_WRAP_MAX = 4;
static constexpr uint8_t BOOK_LINE_MAX = 48; // widest line either face holds
static constexpr uint8_t BOOK_FACE_AGE = 2;  // this fresh and it is set 16 px

// The illuminated initial's cell. It stands in the margin and bites into the
// text column — that overlap is what gives the runaround something to step
// around, and it is why the capital replaces the entry's gutter mark rather
// than sitting beside it.
static constexpr int BOOK_CAP_X  = 10;
static constexpr int BOOK_CAP_W  = 34;
static constexpr int BOOK_CAP_H  = 30;
static constexpr int BOOK_CAP_TX = BOOK_CAP_X + BOOK_CAP_W + 6;   // short rule

// Ink by [tone][age tier]: fresh at the nib, drying as it goes up the page.
//
// Every lead colour (tier 0) is an exact constant off the resource screen, so
// the chronicle sits in the one amber palette the rest of the UI uses rather
// than the sepia-and-cold-violet set it had. The three faded tiers are that
// lead scaled 0.72 / 0.52 / 0.38 per channel, which drops the brightness
// without moving the hue -- fading inside the palette instead of out of it.
//
// The four leads sit on roughly the same brightness rung and differ by hue, so
// that brightness is left free to mean one thing only: age. Get that wrong and
// a fresh omen line comes out darker than a stale plain one, and the fade
// stops reading as time.
//
//   plain  C_TXT   0xC87840   soft amber, the neutral hand
//   good   C_CRIT  0xE89018   the brightest rung: the page lights up
//   ill    C_HDR   0xD06818   hot orange, it went hard
//   omen   0xC05640          C_INFO brick, lifted to the same rung -- C_INFO
//                            itself is a label colour and sits a stop too low
//                            to carry a whole sentence
static const uint32_t BOOK_INK[TONE_COUNT][4] = {
  { 0xC87840, 0x90562E, 0x683E21, 0x4C2E18 },  // plain
  { 0xE89018, 0xA76811, 0x794B0C, 0x583709 },  // good
  { 0xD06818, 0x964B11, 0x6C360C, 0x4F2809 },  // ill
  { 0xC05640, 0x8A3E2E, 0x632C21, 0x492018 },  // omen
};

// The marks in the margin. Two rows of at most three characters, drawn on the
// entry's first two baselines (BOOK_LINE_H apart), in the entry's own ink
// pulled one tier back so the words stay in front of the picture. Indexed by
// K10Glyph; an empty row draws nothing.
static const char* const BOOK_GLYPH[GLY_COUNT][2] = {
  { "",     ""     },  // NONE      nothing set down for this one
  { "\\|/", "-o-"  },  // DAWN      the sun, and a sky you can see through
  { "\\./", "_|_"  },  // FORAGE    something green out of dead ground
  { " , ",  "~~~"  },  // WATER     a drop, and water to take it
  { "[+]",  "\\_/" },  // MEDIC     the kit
  { "_/|",  "|_|"  },  // SALVAGE   a fallen beam over what it was holding up
  { "/^\\", "|_|"  },  // SHELTER   a roof raised against the night
  { "==,",  " ||"  },  // CRAFT     the hammer
  { "|=,",  "|__"  },  // SCOUT     a flag planted on what was blank
  { "zZ ",  "___"  },  // REST      the bedroll
  { "-->",  "<--"  },  // TRADE     goods going both ways
  { "[=]",  "o-o"  },  // CARAVAN   the wagon
  { " o ",  "/|\\" },  // ARRIVE    someone standing with us
  { " o ",  " |\\" },  // DEPART    someone walking off
  { ",-,",  "|#|"  },  // THRESHOLD the doorway, and the dark behind it
  { "/!\\", "---"  },  // CLASH     it went hard
  { "\\*/", " | "  },  // LIGHT     the dark gives
  { "._.",  "[=]"  },  // HAUL      the crate comes back out
  { " v ",  "'''"  },  // WOUND     a gash, and what runs from it
  { " + ",  "/_\\" },  // DEATH     a marker on a mound
  { ",-.",  "(x)"  },  // DOOM      the Creeping Doom turns its head
  { "}^{",  "/_\\" },  // FIRE      flame standing in its own pit
  { ",~.",  "~~~"  },  // FLOOD     water where none should be
  { "_\\_", ",|,"  },  // QUAKE     the ground splits
  { "^^^",  "|||"  },  // SETTLE    roofs, plural, and they stay
  { "___",  "'''"  },  // RAIN      thin and cold
  { "___",  ",/,"  },  // STORM     the bolt in it
  { "(x)",  "'''"  },  // CHEM      the rain turned wrong
  { "~-~",  "-~-"  },  // FOG       the world closed to arm's length
  { "\\o/", "/o\\" },  // RAD       the trefoil, turning
  { "\\ /", "(*)"  },  // MEDAL     a disc on a ribbon
};

// ── The two writing faces ────────────────────────────────────────────────
// `face` is 1 for the 16 px proportional hand and 0 for the 6x8 one. Only
// these four helpers know the difference; everything above them works in
// pixels, because a proportional face has no such thing as a column.
static inline int bookFaceH(uint8_t face) {
  return face ? BOOK_LINE_P : BOOK_LINE_H;
}
static inline int bookCharW(uint8_t face, char c) {
  if (!face) return 6;
  char t[2] = { c, '\0' };
  return canvasWidth16p(t);
}
static inline int bookTextW(uint8_t face, const char* s) {
  return face ? canvasWidth16p(s) : (int)strlen(s) * 6;
}
static inline void bookDraw(uint8_t face, const char* s, int x, int y, uint32_t col) {
  if (face) canvasText16p(s, x, y, col);
  else      canvasText8(s, x, y, col);
}

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

// Break `s` onto word-wrapped lines that fit the page, measured in pixels.
// A dropped capital rules the first `narrow` lines short (`wNarrow`); every
// line past it runs the full `wWide`. Returns the number of lines written.
static uint8_t bookWrapPx(const char* s, char out[][BOOK_LINE_MAX + 1],
                          uint8_t maxLines, uint8_t face,
                          int wNarrow, uint8_t narrow, int wWide) {
  uint8_t n = 0;
  while (*s && n < maxLines) {
    while (*s == ' ') s++;
    if (!*s) break;
    const int budget = (n < narrow) ? wNarrow : wWide;
    int    w    = 0;
    size_t take = 0;
    while (s[take] && take < BOOK_LINE_MAX) {
      int cw = bookCharW(face, s[take]);
      if (w + cw > budget) break;
      w += cw;
      take++;
    }
    if (!s[take]) {                            // the tail fits - last line
      memcpy(out[n], s, take);
      out[n][take] = '\0';
      n++;
      break;
    }
    size_t cut = take;
    while (cut > 0 && s[cut] != ' ') cut--;
    if (cut == 0) cut = take ? take : 1;       // one unbroken word, hard break
    memcpy(out[n], s, cut);
    out[n][cut] = '\0';
    n++;
    s += cut;
  }
  return n;
}

// One entry, measured. The pass that decides how far back the page reaches and
// the pass that writes it both go through this, so the two can never disagree
// about where the next entry starts.
struct BookLay {
  uint8_t face;                             // 0 = the 6x8 hand, 1 = the 16 px
  uint8_t lines;
  char    cap;                              // the illuminated initial, or 0
  int     h;                                // height the whole block needs
  char    txt[BOOK_WRAP_MAX][BOOK_LINE_MAX + 1];
};

static void bookLayout(const K10LogEntry& e, const char names[][16],
                       uint8_t age, bool illum, BookLay& L) {
  char sent[96];
  bookSentence(e, names, sent, sizeof(sent));

  // Age picks the face; the head of the page is always given the large hand,
  // because that is where the capital is, and a 26 px initial beside 6x8
  // script reads as a mistake rather than a flourish.
  L.face = (age <= BOOK_FACE_AGE || illum) ? 1 : 0;
  L.cap  = '\0';
  const char* body = sent;
  if (illum) {
    // The initial is lifted out of the sentence and set in the margin; the
    // words carry on from the letter after it, the way a scribe would.
    for (const char* p = sent; *p; p++) {
      if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
        L.cap = (*p >= 'a') ? (char)(*p - 32) : *p;
        body  = p + 1;
        break;
      }
    }
  }

  const int lh     = bookFaceH(L.face);
  const uint8_t nr = L.cap ? (uint8_t)((BOOK_CAP_H + lh - 1) / lh) : 0;
  L.lines = bookWrapPx(body, L.txt, BOOK_WRAP_MAX, L.face,
                       BOOK_R - BOOK_CAP_TX, nr, BOOK_R - BOOK_X);
  if (L.lines == 0) { L.txt[0][0] = '\0'; L.lines = 1; }

  L.h = L.lines * lh;
  // A mark is always given both its rows, even when the sentence only fills
  // one - otherwise the second row is written over whatever comes next. The
  // capital reserves its own cell the same way, plus a little leaf under it so
  // the vine in bookDrawCap() always has somewhere to go.
  if (L.cap) { if (L.h < BOOK_CAP_H + 12) L.h = BOOK_CAP_H + 12; }
  else if (e.glyph != GLY_NONE && L.h < 2 * BOOK_LINE_H) L.h = 2 * BOOK_LINE_H;
}

// ── Plates ───────────────────────────────────────────────────────────────
// Some moments do not fit a line of handwriting, so they get a block pressed
// into the page instead: a framed ASCII panel 36 cells wide (216 px, flush
// with the marks' gutter), four rows deep, with this kind's mark blown up on
// the left and two lines of figures beside it.
//
//   +-- H A U L -----------------------+
//   | ._.  WTR 2  FOD 4  FUL 1         |
//   | [=]  MED 0  SCR 6      all 13    |
//   +----------------------------------+
//
// The rules and sides are drawn a tier back from the figures, the same way a
// margin mark sits behind its sentence.
static constexpr int     BOOK_PLATE_W    = 36;   // cells across, 216 px
static constexpr uint8_t BOOK_PLATE_ROWS = 4;    // rule, two bodies, rule
static constexpr uint8_t BOOK_PLATE_CONT = 27;   // chars of figures per row
static constexpr int     BOOK_PLATE_GX   = BOOK_MARK_X + 2 * 6;   // mark column
static constexpr int     BOOK_PLATE_TX   = BOOK_MARK_X + 7 * 6;   // figures
static constexpr int     BOOK_PLATE_RX   = BOOK_MARK_X + (BOOK_PLATE_W - 1) * 6;

struct BookPlateDef { const char* title; uint8_t glyph; char rule; };
static const BookPlateDef BOOK_PLATE[PLATE_COUNT] = {
  { nullptr,     GLY_NONE,  '-' },   // NONE  — never drawn
  { "WOUNDS",    GLY_WOUND, '-' },   // pv: minor, major, LL now
  { "RADIATION", GLY_RAD,   '-' },   // pv: dose, level now, level max
  { "HAUL",      GLY_HAUL,  '-' },   // pv: the five resources carried out
  { "COMMENDED", GLY_MEDAL, '=' },   // pv: K10Award; text is the citation
};

// The commendations, in K10Award order. The citation under each one is
// written at the call site, because only the call site knows the numbers.
static const char* const BOOK_AWARD[AWD_COUNT] = {
  "Swept the Place",
  "Heavy Load",
  "Off the Scent",
  "Long Odds",
};

// "+-- H A U L ------+", the title letter-spaced into the rule. A null title
// gives the plain closing rule.
static void bookPlateRule(char* out, const char* title, char ch) {
  int n = 0;
  out[n++] = '+'; out[n++] = ch; out[n++] = ch;
  if (title && *title) {
    out[n++] = ' ';
    for (const char* p = title; *p && n < BOOK_PLATE_W - 2; p++) {
      out[n++] = *p;
      if (p[1] && n < BOOK_PLATE_W - 2) out[n++] = ' ';
    }
    out[n++] = ' ';
  }
  while (n < BOOK_PLATE_W - 1) out[n++] = ch;
  out[n++] = '+';
  out[n]   = '\0';
}

// "[####......]" — a dose read off at a glance.
static void bookPlateMeter(char* out, size_t cap, uint8_t v, uint8_t vmax) {
  const uint8_t CELLS = 10;
  uint8_t on = 0;
  if (vmax > 0) {
    uint16_t f = ((uint16_t)v * CELLS + vmax - 1) / vmax;
    on = (f > CELLS) ? CELLS : (uint8_t)f;
  }
  size_t n = 0;
  if (cap) out[n++] = '[';
  for (uint8_t i = 0; i < CELLS && n + 2 < cap; i++) out[n++] = (i < on) ? '#' : '.';
  if (n + 1 < cap) out[n++] = ']';
  out[n] = '\0';
}

// One framed row: the sides in the frame's ink, the mark and figures in the
// entry's own.
static void bookPlateRow(int y, const char* mark, const char* text,
                         uint32_t ink, uint32_t frame) {
  canvasText8("|", BOOK_MARK_X,  y, frame);
  canvasText8("|", BOOK_PLATE_RX, y, frame);
  if (mark && *mark) canvasText8(mark, BOOK_PLATE_GX, y, ink);
  if (text && *text) {
    char t[BOOK_PLATE_CONT + 1];
    strlcpy(t, text, sizeof(t));
    canvasText8(t, BOOK_PLATE_TX, y, ink);
  }
}

// Fill a plate's two figure lines from pv[], per kind.
static void bookPlateFigures(const K10LogEntry& e, const char names[][16],
                             char* a, char* b, size_t cap) {
  const char* who = (e.who >= 0) ? bookName(e.who, names) : "The party";
  a[0] = b[0] = '\0';
  switch (e.plate) {
    case PLATE_WOUND:
      if (e.text[0]) snprintf(a, cap, "%s, %s", who, e.text);
      else           snprintf(a, cap, "%s", who);
      snprintf(b, cap, "minor %u  major %u   LL %u",
               (unsigned)e.pv[0], (unsigned)e.pv[1], (unsigned)e.pv[2]);
      break;
    case PLATE_RAD: {
      char bar[16];
      bookPlateMeter(bar, sizeof(bar), e.pv[1], e.pv[2]);
      snprintf(a, cap, "%s takes %u rad%s", who,
               (unsigned)e.pv[0], (e.pv[0] == 1) ? "" : "s");
      snprintf(b, cap, "%s %u of %u", bar, (unsigned)e.pv[1], (unsigned)e.pv[2]);
      break;
    }
    case PLATE_HAUL: {
      unsigned all = 0;
      for (uint8_t i = 0; i < 5; i++) all += e.pv[i];
      snprintf(a, cap, "WTR %u  FOD %u  FUL %u",
               (unsigned)e.pv[0], (unsigned)e.pv[1], (unsigned)e.pv[2]);
      snprintf(b, cap, "MED %u  SCR %u      all %u",
               (unsigned)e.pv[3], (unsigned)e.pv[4], all);
      break;
    }
    case PLATE_AWARD:
      snprintf(a, cap, "%s", (e.pv[0] < AWD_COUNT) ? BOOK_AWARD[e.pv[0]] : "Noted");
      if (e.text[0]) snprintf(b, cap, "%s, %s", who, e.text);
      else           snprintf(b, cap, "%s", who);
      break;
    default:
      break;
  }
}

static void bookDrawPlate(const K10LogEntry& e, const char names[][16],
                          int y, uint32_t ink, uint32_t frame) {
  const BookPlateDef& d = BOOK_PLATE[e.plate];
  char line[BOOK_PLATE_W + 1], a[64], b[64];
  bookPlateRule(line, d.title, d.rule);
  canvasText8(line, BOOK_MARK_X, y, frame);
  bookPlateFigures(e, names, a, b, sizeof(a));
  bookPlateRow(y + BOOK_LINE_H,     BOOK_GLYPH[d.glyph][0], a, ink, frame);
  bookPlateRow(y + 2 * BOOK_LINE_H, BOOK_GLYPH[d.glyph][1], b, ink, frame);
  bookPlateRule(line, nullptr, d.rule);
  canvasText8(line, BOOK_MARK_X, y + 3 * BOOK_LINE_H, frame);
}

// A small diamond, the way a scribe stopped a ruled line.
static void bookDiamond(int x, int y, uint32_t col) {
  canvasLine(x - 2, y,     x,     y - 2, col);
  canvasLine(x,     y - 2, x + 2, y,     col);
  canvasLine(x + 2, y,     x,     y + 2, col);
  canvasLine(x,     y + 2, x - 2, y,     col);
}

// The chapter rule struck across the page. The day still being written gets
// the 16 px numeral, a letter-spaced label and diamonds stopping its rules;
// every day behind it keeps the small hand, so however many days a page spans
// there is exactly one loud heading on it.
static void bookDayRule(int y, uint16_t day, bool open,
                        uint32_t col, uint32_t rule, uint32_t accent) {
  char d[16];
  if (!open) {
    snprintf(d, sizeof(d), "Day %u", (unsigned)day);
    int w  = (int)strlen(d) * 6;
    int x0 = 120 - w / 2;
    canvasText8(d, x0, y + 2, col);
    canvasLine(BOOK_X, y + 5, x0 - 6, y + 5, rule);
    canvasLine(x0 + w + 5, y + 5, BOOK_R - 6, y + 5, rule);
    return;
  }
  static const char LBL[] = "D A Y";
  snprintf(d, sizeof(d), "%u", (unsigned)day);
  const int lw    = (int)(sizeof(LBL) - 1) * 6;
  const int total = lw + 7 + canvasWidth16p(d);
  const int x0    = 120 - total / 2;
  const int ry    = y + 9;
  canvasText8(LBL, x0, y + 6, col);
  canvasText16p(d, x0 + lw + 7, y + 1, accent);
  canvasLine(BOOK_X + 6, ry, x0 - 8, ry, rule);
  canvasLine(x0 + total + 7, ry, BOOK_R - 6, ry, rule);
  bookDiamond(BOOK_X + 2, ry, rule);
  bookDiamond(BOOK_R - 2, ry, rule);
}

// The illuminated initial: a ruled box with a lighter inner rule and gilded
// corners, the letter set in the 26 px face inside it, and a vine let down
// into the margin for as far as this entry's own block runs. Gold does not
// fade the way ink does, so the capital is always struck in the brightest gold
// on the palette, however far back the head of the page has receded.
static void bookDrawCap(char c, int y, int h, uint32_t ink,
                        uint32_t frame, uint32_t leaf) {
  const int x0 = BOOK_CAP_X, x1 = BOOK_CAP_X + BOOK_CAP_W;
  canvasRect(x0, y, x1, y + BOOK_CAP_H, frame, false);
  canvasRect(x0 + 2, y + 2, x1 - 2, y + BOOK_CAP_H - 2, leaf, false);
  for (uint8_t i = 0; i < 4; i++) {          // gold where the leaf catches
    int cx = (i & 1) ? x1 - 1 : x0;
    int cy = (i & 2) ? y + BOOK_CAP_H - 1 : y;
    canvasLine(cx, cy, cx + ((i & 1) ? -3 : 3), cy, ink);
    canvasLine(cx, cy, cx, cy + ((i & 2) ? -3 : 3), ink);
  }
  char s[2] = { c, 0 };
  canvasText26p(s, x0 + (BOOK_CAP_W - canvasWidth26p(s)) / 2, y + 2, ink);

  // A tailpiece under the capital: two rules narrowing to the same diamond the
  // chapter rules are stopped with, so the page keeps one vocabulary of
  // ornament. It is drawn only inside this entry's own block, so it can never
  // grow into the next one's gutter mark -- bookLayout() floors an illuminated
  // block at BOOK_CAP_H + 12 to keep the room for it.
  const int cx = x0 + BOOK_CAP_W / 2;
  const int vy = y + BOOK_CAP_H + 2;
  if (y + h - vy < 9) return;
  canvasLine(cx - 8, vy, cx + 8, vy, leaf);
  canvasLine(cx - 4, vy + 3, cx + 4, vy + 3, leaf);
  bookDiamond(cx, vy + 7, leaf);
}

static void drawEventLogScreen() {
  // The resource screen palette, name for name -- the page furniture is not a
  // second colour scheme, it is the same eight amber rungs used for a book:
  // C_BAND is the leaf, C_LINE the heavy rules, C_DIM the ruling and the fine
  // print, C_INFO the labels, C_HDR the masthead, C_CRIT the gilding.
  static const uint32_t C_HDR   = 0xD06818;
  static const uint32_t C_INFO  = 0x904030;
  static const uint32_t C_LINE  = 0x502010;
  static const uint32_t C_DIM   = 0x3A1808;
  static const uint32_t C_BAND  = 0x1E0A00;
  static const uint32_t C_CRIT  = 0xE89018;

  K10LogEntry snap[K10_LOG_SIZE];
  uint8_t  snapHead = 0, snapCount = 0;
  uint16_t total    = 0;
  taskENTER_CRITICAL(&k10LogMux);
  memcpy(snap, k10Log, sizeof(K10LogEntry) * K10_LOG_SIZE);
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

  // -- Masthead ------------------------------------------------
  // Title in the 26 px face, the day standing off to its right in the 16 px
  // one under a letter-spaced 6x8 label: all three sizes are on the screen
  // before a single entry has been written.
  canvasText26p("The Chronicle", 5, 1, C_HDR);
  char dbuf[12];
  snprintf(dbuf, sizeof(dbuf), "%u", (unsigned)today);
  canvasText8("d a y", BOOK_R - 5 * 6, 2, C_INFO);
  canvasText16p(dbuf, BOOK_R - canvasWidth16p(dbuf), 12, C_CRIT);
  canvasLine(6, 30, 233, 30, C_LINE);
  canvasLine(6, 32, 233, 32, C_DIM);

  canvasRect(6, BOOK_TOP - 3, 234, BOOK_BOT + 3, C_BAND, true);   // the leaf
  canvasLine(9, BOOK_TOP - 3, 9, BOOK_BOT + 2, C_DIM);            // the rule
  int y = BOOK_TOP;                                               // the pen

  if (snapCount == 0) {
    static const char E1[] = "The page is still blank.";
    static const char E2[] = "Nothing yet worth setting down.";
    canvasText16p(E1, (240 - canvasWidth16p(E1)) / 2, 142, C_INFO);
    canvasText8(E2, (240 - (int)(sizeof(E2) - 1) * 6) / 2, 164, C_LINE);
  } else {
    // The day at the pen. Its chapter gets the loud heading; the days behind
    // it get the quiet one.
    const uint16_t openDay =
        snap[(snapHead + (uint8_t)(snapCount - 1)) % K10_LOG_SIZE].day;
    BookLay L;

    // Measure backwards from the newest entry to find the oldest one that
    // still fits, so the page always ends at the pen. Two things about the
    // head of the page are only known once it *is* the head, and both cost
    // room, so both are carried as a running surcharge rather than discovered
    // after the fact -- add either one late and it pushes the pen off the foot:
    //
    //   capX   the topmost written entry is the illuminated one, and a capital
    //          floors its block at the height of its own cell. Scanning
    //          backwards, "topmost written" is simply the newest written entry
    //          reached so far, so the surcharge moves with it and the entry it
    //          left keeps only its plain height.
    //   headX  the head carries a chapter rule whether or not its day differs
    //          from the one above it -- which there isn't one of.
    const int budget = BOOK_BOT - BOOK_TOP;
    int first = (int)snapCount - 1, used = 0, capX = 0;
    for (int i = (int)snapCount - 1; i >= 0; i--) {
      uint8_t idx  = (snapHead + (uint8_t)i) % K10_LOG_SIZE;
      uint8_t prev = (snapHead + (uint8_t)(i - 1)) % K10_LOG_SIZE;
      uint8_t age  = (uint8_t)(snapCount - 1 - i);
      const bool plate = (snap[idx].plate != PLATE_NONE &&
                          snap[idx].plate < PLATE_COUNT);
      int hPlain = BOOK_PLATE_ROWS * BOOK_LINE_H, newCapX = capX;
      if (!plate) {
        bookLayout(snap[idx], names, age, false, L); hPlain = L.h;
        bookLayout(snap[idx], names, age, true,  L); newCapX = L.h - hPlain;
      }
      const bool ownHeading = (i == 0) || (snap[prev].day != snap[idx].day);
      const int  head  = (snap[idx].day == openDay) ? BOOK_HEAD_P : BOOK_HEAD_H;
      const int  cost  = hPlain + BOOK_GAP + (ownHeading ? head : 0);
      const int  headX = ownHeading ? 0 : head;
      if (i != (int)snapCount - 1 && used + cost + newCapX + headX > budget) break;
      used += cost;
      capX  = newCapX;
      first = i;
    }

    // The illuminated initial goes on the first *written* entry of the page: a
    // plate is already a framed block pressed into the leaf and gilding one
    // would be two decorations fighting over the same corner.
    int illum = -1;
    for (int i = first; i < (int)snapCount; i++) {
      uint8_t idx = (snapHead + (uint8_t)i) % K10_LOG_SIZE;
      if (snap[idx].plate == PLATE_NONE || snap[idx].plate >= PLATE_COUNT) {
        illum = i;
        break;
      }
    }

    // Write the page top-down, oldest first -- the way a book fills.
    for (int i = first; i < (int)snapCount; i++) {
      uint8_t idx  = (snapHead + (uint8_t)i) % K10_LOG_SIZE;
      uint8_t prev = (snapHead + (uint8_t)(i - 1)) % K10_LOG_SIZE;
      uint8_t age  = (uint8_t)(snapCount - 1 - i);
      uint8_t tier = (age == 0) ? 0 : (age <= 2) ? 1 : (age <= 5) ? 2 : 3;
      uint8_t tone = (snap[idx].tone < TONE_COUNT) ? snap[idx].tone : TONE_PLAIN;
      uint32_t ink   = BOOK_INK[tone][tier];
      uint32_t faded = BOOK_INK[tone][(tier < 3) ? tier + 1 : 3];

      if (i == first || snap[prev].day != snap[idx].day) {
        const bool open = (snap[idx].day == openDay);
        const int  hh   = open ? BOOK_HEAD_P : BOOK_HEAD_H;
        if (y + hh > BOOK_BOT) break;
        bookDayRule(y, snap[idx].day, open, C_INFO, C_DIM, C_CRIT);
        y += hh;
      }

      // A plate is pressed in whole, or not at all -- never half off the foot.
      if (snap[idx].plate != PLATE_NONE && snap[idx].plate < PLATE_COUNT) {
        if (y + BOOK_PLATE_ROWS * BOOK_LINE_H > BOOK_BOT) break;
        bookDrawPlate(snap[idx], names, y, ink, faded);
        y += BOOK_PLATE_ROWS * BOOK_LINE_H + BOOK_GAP;
        continue;
      }

      bookLayout(snap[idx], names, age, i == illum, L);
      const int lh = bookFaceH(L.face);
      if (y + lh > BOOK_BOT) break;

      if (L.cap) {
        bookDrawCap(L.cap, y, L.h, C_CRIT, BOOK_INK[tone][1], BOOK_INK[tone][2]);
      } else if (snap[idx].glyph != GLY_NONE && snap[idx].glyph < GLY_COUNT) {
        // The mark in the gutter, a tier back from the words it belongs to.
        for (uint8_t g = 0; g < 2; g++) {
          int gy = y + g * BOOK_LINE_H;
          if (BOOK_GLYPH[snap[idx].glyph][g][0] && gy + BOOK_LINE_H <= BOOK_BOT)
            canvasText8(BOOK_GLYPH[snap[idx].glyph][g], BOOK_MARK_X, gy, faded);
        }
      }

      // Lines beside the capital are ruled short so the script steps around
      // it; everything past the capital runs the full measure.
      const uint8_t nr = L.cap ? (uint8_t)((BOOK_CAP_H + lh - 1) / lh) : 0;
      for (uint8_t l = 0; l < L.lines; l++) {
        const int ly = y + l * lh;
        if (ly + lh > BOOK_BOT) break;
        const int lx = (l < nr) ? BOOK_CAP_TX : BOOK_X;
        bookDraw(L.face, L.txt[l], lx, ly, ink);
        // The newest entry is written twice, a pixel apart: the pen is still
        // on it and the ink has not had time to thin.
        if (age == 0) bookDraw(L.face, L.txt[l], lx + 1, ly, ink);
        // The nib rests where the last word ended.
        if (i == (int)snapCount - 1 && l == L.lines - 1) {
          const int nx = lx + bookTextW(L.face, L.txt[l]) + 4;
          if (nx < BOOK_R) canvasRect(nx, ly + 1, nx + 2, ly + lh - 3, ink, true);
        }
      }
      y += L.h + BOOK_GAP;
    }
  }

  // Whatever leaf is left under the pen is ruled and empty, the way a scribe
  // ruled the whole page before writing a word on it.
  for (int ry = y + 5; ry + 4 <= BOOK_BOT; ry += BOOK_LINE_P)
    canvasLine(BOOK_X, ry, BOOK_R - 8, ry, C_DIM);

  // -- Folio ---------------------------------------------------
  canvasLine(6, 301, 233, 301, C_LINE);
  canvasText8("kept by hand", 6, 308, C_DIM);
  char pg[16];
  snprintf(pg, sizeof(pg), "%u", (unsigned)total);
  const int pw = canvasWidth16p(pg);
  canvasText8("fol.", BOOK_R - pw - 28, 308, C_DIM);
  canvasText16p(pg, BOOK_R - pw, 303, C_INFO);
}

// -- Screen 3: resources ----------------------------------------
// One survivor per page, rotating through the online players every
// RES_PAGE_MS (the LCD repaints every SCREEN_MS, so a flip lands on the first
// repaint after that). Same three-face system as screen 1, with the five
// counts taking the 26 px face: they are the entire point of the screen, so
// they are the largest thing on it and everything else annotates them.
//
//   y   0-29   header band     "RESOURCES" 26 px       page n/m 16 px (right)
//   y  33-49   who             [n] name 16 px          archetype 6x8 (right)
//   y  56-102  5 tiles         WATER FOOD FUEL ...     counts 26 px
//   y 109-125  pack heading    PACK used/slots         + a 3 px capacity bar
//   y 129-     pack contents   name 16 px              xqty 16 px (right)
//
// Two things the small face bought here. The tile labels are words now
// (WATER, FOOD, FUEL, SCRAP) instead of WTR/FOD/FUL/SCR -- a 46 px tile holds
// seven 6 px characters. And the pack list fits roughly 28 characters in the
// width the 12x16 grid fit 14, so item names stopped being cut mid-word.

static constexpr uint32_t RES_PAGE_MS = 8000;

static void drawResourceScreen() {
  static const uint32_t C_HDR   = 0xD06818;
  static const uint32_t C_TXT   = 0xC87840;
  static const uint32_t C_INFO  = 0x904030;
  static const uint32_t C_LINE  = 0x502010;
  static const uint32_t C_DIM   = 0x3A1808;
  static const uint32_t C_BAND  = 0x1E0A00;
  static const uint32_t C_TRACK = 0x2E1206;   // same empty track as screen 1
  static const uint32_t C_WARN  = 0xC87020;
  static const uint32_t C_CRIT  = 0xE89018;
  // MEDS rather than MEDICINE: eight characters is 48 px and the tile is 46.
  // It is still a word, which WTR/FOD/FUL/MED/SCR were not.
  static const char* RS[5] = {"WATER","FOOD","FUEL","MEDS","SCRAP"};

  struct PSnap {
    bool    on;
    uint8_t archetype;
    char    name[16];
    uint8_t inv[5];
    uint8_t invType[INV_SLOTS_MAX];
    uint8_t invQty[INV_SLOTS_MAX];
    uint8_t invSlots;                  // effectiveInvSlots(), not the archetype base
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
    snap[i].invSlots = effectiveInvSlots(p);   // base + equipped "slots" bonuses
  }
  xSemaphoreGive(G.mutex);

  char buf[40];
  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 29, C_BAND, true);
  canvasText26p("RESOURCES", 4, 1, C_HDR);
  canvasLine(0, 29, 239, 29, C_LINE);

  // Which survivor is on the page.
  int online[MAX_PLAYERS], cnt = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (snap[i].on) online[cnt++] = i;
  if (cnt == 0) {
    canvasText16pC("No survivors online", 120, 150, C_DIM);
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
  canvasText16pR(buf, DASH_R, 7, C_INFO);

  // -- Who -----------------------------------------------------
  // Archetype right-aligned at 6x8 as its real name; the survivor name takes
  // whatever is left, clipped by measurement (see canvasFit16p).
  char num[2] = { (char)('1' + pid), 0 };
  canvasRect(2, 33, 18, 49, C_HDR, true);
  canvasText16pC(num, 10, 33, C_BAND);
  uint8_t arch = P.archetype < NUM_ARCHETYPES ? P.archetype : 0;
  const char* an = ARCHETYPE_NAME[arch];
  canvasText8R(an, DASH_R, 37, C_INFO);
  char nm[17];
  snprintf(nm, sizeof(nm), "%.16s", P.name);
  canvasFit16p(buf, sizeof(buf), nm, DASH_R - (int)strlen(an) * 6 - 8 - 22);
  canvasText16p(buf, 22, 33, C_TXT);
  canvasLine(0, 52, 239, 52, C_LINE);

  // -- Resource tiles ------------------------------------------
  // 5 x 46 px tiles at 48 px pitch: the 6x8 word on top, the 26 px count
  // centred under it. A count of 0 is critical, 1-2 is a warning -- the same
  // reading the web client gives these bars.
  for (int k = 0; k < 5; k++) {
    int tx = 2 + k * 48, ty = 56;
    canvasRect(tx, ty, tx + 46, ty + 46, C_BAND, true);
    canvasText8C(RS[k], tx + 23, ty + 4, C_INFO);
    uint8_t v = P.inv[k];
    snprintf(buf, sizeof(buf), "%u", (unsigned)v);
    uint32_t vc = (v == 0) ? C_CRIT : (v <= 2) ? C_WARN : C_TXT;
    canvasText26pC(buf, tx + 23, ty + 15, vc);
  }
  canvasLine(0, 106, 239, 106, C_LINE);

  // -- Pack ----------------------------------------------------
  int used = 0;
  for (int s = 0; s < P.invSlots && s < INV_SLOTS_MAX; s++) if (P.invType[s]) used++;
  canvasText8("PACK", 2, 113, C_INFO);
  snprintf(buf, sizeof(buf), "%d/%u", used, (unsigned)P.invSlots);
  canvasText16p(buf, 32, 109, C_TXT);
  // The same 3 px bar idiom the dashboard uses for stats, so "how full is it"
  // reads the same way on both screens.
  const int bx0 = 96;
  canvasRect(bx0, 117, DASH_R, 120, C_TRACK, true);
  int bw = (P.invSlots == 0) ? 0 : ((DASH_R - bx0) * used) / (int)P.invSlots;
  if (bw > 0) canvasRect(bx0, 117, bx0 + bw, 120, used >= P.invSlots ? C_WARN : C_HDR, true);

  if (used == 0) {
    canvasText16p("pack is empty", 14, 131, C_DIM);
    return;
  }
  static constexpr int ROW_H = 17, ROW_Y0 = 129, ROWS_MAX = 11;
  int row = 0, shown = 0;
  for (int s = 0; s < P.invSlots && s < INV_SLOTS_MAX; s++) {
    if (!P.invType[s]) continue;
    if (row == ROWS_MAX - 1 && used - shown > 1) {
      snprintf(buf, sizeof(buf), "%d more", used - shown);
      canvasText16p(buf, 14, ROW_Y0 + row * ROW_H, C_DIM);
      break;
    }
    int y = ROW_Y0 + row * ROW_H;
    // No "equipped" marker here: equipItem() clears the pack slot on its way
    // to equip[], so an equipped item is never in invType[].  The test this
    // replaces compared item IDs, which could only ever fire on a spare
    // DUPLICATE of worn gear -- and then flagged the wrong copy.
    const ItemDef* def = getItemDef(P.invType[s]);
    char qty[8];
    snprintf(qty, sizeof(qty), "x%u", (unsigned)P.invQty[s]);
    canvasText16pR(qty, DASH_R, y, C_TXT);
    canvasFit16p(buf, sizeof(buf), def ? def->name : "unknown",
                 DASH_R - canvasWidth16p(qty) - 8 - 14);
    canvasText16p(buf, 14, y, C_INFO);
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
//   y   0-29   header band   "ENCOUNTERS" 26 px
//   y  31-49   strip         SITES left/total      WON wins/rolls (right)
//   y  54-88   last roll     [n] skill 16 px       WON / LOST 26 px (right)
//                            NEEDED n   ROLLED n
//   y  96-317  6 cards x 37  [n] name 16 px        ENTERED count (right)
//                            INSIDE biome  CARRYING n  |  WON w/r  last end
//
// Same three faces as screens 1 and 3. The verdict gets the 26 px face on its
// own, because it is the one thing here that is a yes or a no -- everything
// else is a count you have to read. And with labels at 6 px there was room to
// stop abbreviating: POI became SITES, ENC became ENTERED, NEED/GOT became
// NEEDED/ROLLED, and IN/LOOT became INSIDE/CARRYING.
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
  static const char* END_NAME[ENC_END_COUNT] = {"hazard","abandoned","dawn","downed","dropped","reset"};

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
  canvasText26p("ENCOUNTERS", 4, 1, C_HDR);
  canvasLine(0, 29, 239, 29, C_LINE);

  // -- Strip: sites left, party hit rate -----------------------
  canvasText8("SITES", 2, 35, C_INFO);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)poiLeft, (unsigned)poiTotal);
  canvasText16p(buf, 38, 31, poiLeft == 0 ? C_DIM : C_TXT);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)st.wins, (unsigned)st.rolls);
  canvasText16pR(buf, DASH_R, 31, st.rolls == 0 ? C_DIM : C_TXT);
  canvasText8R("WON", DASH_R - canvasWidth16p(buf) - 6, 35, C_INFO);
  canvasLine(0, 50, 239, 50, C_LINE);

  // -- Last roll -----------------------------------------------
  // The verdict is 26 px and right-aligned across both rows: it is the only
  // yes-or-no on the screen, and it should read from across the table without
  // reading the numbers that produced it.
  if (st.hasLast) {
    char num[2] = { (char)('1' + (st.lastPid < MAX_PLAYERS ? st.lastPid : 0)), 0 };
    canvasRect(2, 54, 18, 70, C_HDR, true);
    canvasText16pC(num, 10, 54, C_BAND);
    canvasText16p(SKILL_UP[st.lastSkill < NUM_SKILLS ? st.lastSkill : 0], 24, 54, C_TXT);
    canvasText8("NEEDED", 24, 76, C_INFO);
    snprintf(buf, sizeof(buf), "%u", (unsigned)st.lastDN);
    canvasText16p(buf, 64, 72, C_TXT);
    canvasText8("ROLLED", 92, 76, C_INFO);
    snprintf(buf, sizeof(buf), "%d", (int)st.lastTotal);
    canvasText16p(buf, 132, 72, st.lastOut ? C_OK : C_CRIT);
    if (st.lastOut) canvasText26pR("WON",  DASH_R, 57, C_OK);
    else            canvasText26pR("LOST", DASH_R, 57, C_CRIT);
  } else {
    canvasText16p("No rolls yet", 24, 63, C_DIM);
  }
  canvasLine(0, 92, 239, 92, C_LINE);

  // -- Survivor cards ------------------------------------------
  static const int CARD_Y0 = 96, CARD_H = 37;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    int  y = CARD_Y0 + i * CARD_H;
    char num[2] = { (char)('1' + i), 0 };

    if (!snap[i].on) {
      canvasRect(2, y, 18, y + 16, C_DIM, false);
      canvasText16pC(num, 10, y, C_DIM);
      canvasText16p("offline", 24, y, C_DIM);
      continue;
    }

    // Row A: badge (hot while inside an encounter), name, lifetime count.
    // Right to left again -- the count and its label are fixed, the name
    // takes the rest and is clipped by measurement.
    canvasRect(2, y, 18, y + 16, snap[i].inEnc ? C_CRIT : C_HDR, true);
    canvasText16pC(num, 10, y, C_BAND);
    snprintf(buf, sizeof(buf), "%u", (unsigned)snap[i].encCount);
    canvasText16pR(buf, DASH_R, y, C_INFO);
    int lx = DASH_R - canvasWidth16p(buf) - 5;
    canvasText8R("ENTERED", lx, y + 4, C_INFO);
    char nm[17];
    snprintf(nm, sizeof(nm), "%.16s", snap[i].name);
    canvasFit16p(buf, sizeof(buf), nm, lx - 7 * 6 - 8 - 24);
    canvasText16p(buf, 24, y, C_TXT);

    // Row B: live encounter, or the record so far.
    if (snap[i].inEnc) {
      canvasText8("INSIDE", 24, y + 22, C_INFO);
      snprintf(buf, sizeof(buf), "%u", (unsigned)snap[i].loot);
      canvasText16pR(buf, DASH_R, y + 18, snap[i].loot ? C_TXT : C_DIM);
      int bx = DASH_R - canvasWidth16p(buf) - 5;
      canvasText8R("CARRYING", bx, y + 22, C_INFO);
      char bi[13];
      snprintf(bi, sizeof(bi), "%.12s", snap[i].biome);
      canvasFit16p(buf, sizeof(buf), bi, bx - 8 * 6 - 8 - 64);
      canvasText16p(buf, 64, y + 18, C_WARN);
    } else {
      if (st.p[i].rolls == 0) {
        canvasText16p("no rolls yet", 24, y + 18, C_DIM);
      } else {
        canvasText8("WON", 24, y + 22, C_INFO);
        snprintf(buf, sizeof(buf), "%u/%u", (unsigned)st.p[i].wins, (unsigned)st.p[i].rolls);
        canvasText16p(buf, 48, y + 18, C_TXT);
      }
      if (st.p[i].hasEnd) {
        const char* r = END_NAME[st.p[i].lastEnd < ENC_END_COUNT ? st.p[i].lastEnd : 0];
        uint32_t rc = (st.p[i].lastEnd == ENC_END_DOWNED || st.p[i].lastEnd == ENC_END_HAZARD) ? C_WARN : C_INFO;
        canvasText16pR(r, DASH_R, y + 18, rc);
        canvasText8R("ENDED", DASH_R - canvasWidth16p(r) - 6, y + 22, C_DIM);
      }
    }
  }
}

// ── Screen 5: hex map minimap ──────────────────────────────────
static void drawMapScreen() {
  // Terrain fill colours — monochromatic amber, indexed by terrain ID 0–10
  static const uint32_t TERR_COL[NUM_TERRAIN] = {
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
    0x102838,  // 11 River Channel    (was missing: t>=11 fell back to a flat brown)
    0xB89038,  // 12 Bunker Entrance  -- bright, it is a landmark
    0x886028,  // 13 Vent Shaft
    0x241C14,  // 14 Tunnel Floor     -- tunnel board only, never drawn here
    0x0C0906,  // 15 Collapsed Tunnel -- ditto
  };

  PSRAM_STATIC(uint8_t, terr, [MAP_ROWS][MAP_COLS]);
  struct { int16_t q, r; bool on; uint8_t depth; } ps[MAX_PLAYERS];
  WorldMarkers wm = {};

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      terr[r][c] = G.map[r][c].terrain;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    ps[i].q  = G.players[i].q;
    ps[i].r  = G.players[i].r;
    ps[i].on = G.players[i].connected;
    // Underground survivors keep q/r pinned to the hatch they went down, so
    // they still plot correctly here — they just need to read differently.
    ps[i].depth = G.players[i].depth;
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
      uint32_t col = (t < NUM_TERRAIN) ? TERR_COL[t] : 0x3A1808;
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

  // Bunker hatches. Drawn under the players so a survivor standing on one
  // still reads as a player marker. Violet-blue is the one hue not already
  // spoken for by terrain (amber), Doom (red) or the caravan (teal).
  for (int i = 0; i < hatchCount; i++) {
    int hq = bunkerHatches[i].sq, hr = bunkerHatches[i].sr;
    if (hq < 0 || hq >= MAP_COLS || hr < 0 || hr >= MAP_ROWS) continue;
    int hpx, hpy; cellPx(hq, hr, hpx, hpy);
    canvas.drawRect(hpx - 1, hpy - 1, CW + 2, CH + 2, c16(0x08040C));
    canvas.fillRect(hpx, hpy, CW, CH, c16(0x8878E0));
  }

  // Player marker: at small cell sizes a digit no longer fits; draw a bright filled
  // square sized to the cell. Fall back to a digit when cells are big enough.
  bool bigCells = (CW >= 8 && CH >= 12);
  // This is the one place that drives the canvas text engine directly, so it
  // has to name the font as well as the size: the chronicle leaves a
  // proportional face selected and setTextSize() does not reset it, which
  // would render these digits 26 px tall over the map.
  if (bigCells) { canvas.setFont(&fonts::Font0); canvas.setTextSize(2); }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!ps[i].on) continue;
    int q = ps[i].q, r = ps[i].r;
    if (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS) continue;
    int px = MX + q * XS;
    int py = MY + r * YS + (q & 1) * OY;
    // Underground: same hex (they are standing on their entrance hatch as far
    // as the surface is concerned), but drawn in the hatch violet rather than
    // player amber so "they went below here" is readable at a glance.
    bool below = (ps[i].depth != 0);
    if (bigCells) {
      char num[2] = { (char)('1' + i), 0 };
      canvas.setTextColor(c16(below ? 0xA090F0 : 0xD06818));
      canvas.setCursor(px, py);
      canvas.print(num);
    } else {
      // Same dark halo the world markers get: 0xFFD060 sits right on top of
      // Settlement (0xE8A828) and Glass Fields (0xC08028) in the terrain
      // palette, so an un-outlined player square disappears over those.
      canvas.drawRect(px - 1, py - 1, CW + 2, CH + 2, c16(0x180C00));
      canvas.fillRect(px, py, CW, CH, c16(below ? 0xA090F0 : 0xFFD060));
    }
  }

  int bx1 = MX - 1;
  int by1 = MY - 1;
  int bx2 = MX + (MAP_COLS - 1) * XS + CW;
  int by2 = MY + (MAP_ROWS - 1) * YS + OY + CH;
  canvas.drawRect(bx1, by1, bx2 - bx1, by2 - by1, c16(0xD06818));

  // ── Legend ───────────────────────────────────────────────────────────────
  // At 3×5-pixel cells the marker colours are the only thing telling the
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
    // Four swatches + the awareness readout come to ~170 of the 234 px the
    // map border spans, so this still fits on one line.
    legend(0xFFD060, "YOU", 3);
    legend(hatchCount ? 0x8878E0 : 0x201C38, "HATCH", 5);
    legend(caravanOnMap ? 0x30E0A0 : 0x1A4438, "TRADE", 5);
    legend(0xFF4008, "DOOM", 4);
    // Awareness is the number that decides whether the Doom is wandering or
    // hunting — worth a readout next to its swatch.
    char aw[10];
    snprintf(aw, sizeof(aw), "%d%%", (int)wm.doomAwareness);
    canvasText8(aw, lx, ly, wm.doomAwareness >= 76 ? 0xFF4008 : 0xC0A080);
  }
}

// ── Screen 6: the board of the admired ─────────────────────────
// The score ladder, on the table where the whole group can see it.
// ADMIRED_WIN (10000) is the way out; everything under it is the wasteland's
// own leaderboard, and the party is ranked INTO that list rather than beside
// it — the point of the screen is which corpses you are currently between.
//
//   y   0-29   header band   "THE ADMIRED" 26 px
//   y  31-49   strip         OUT AT 10000        BEST <party high> (right)
//   y  54-108  the chase     NEXT ABOVE, their name 16 px, their score 26 px
//                            right, the gap under it, citation in two 6 px
//                            lines
//   y 114-267  the ladder    9 merged rows x 17 px — rank, score, badge,
//                            name. A living row is lit and carries the
//                            numbered badge; the dead carry neither.
//   y 274-317  the wake      LAST PASSED — the highest name already under
//                            the party, with its citation
//
// So the ladder is framed by the dead in both directions: the one being
// climbed towards at the top, the one most recently stepped over at the
// bottom. Both move on their own as the score does; nothing here needs a
// timer or a page flip.
//
// This table is a MIRROR of ADMIRED in data/game-data.js, which draws the
// same board in the browser. The LCD has no JS and the browser has no flash
// access, so the rows genuinely live twice: change one and change the other
// or the two boards disagree about who you just passed. The wording rules
// are in the game-data.js comment — dry, lower case, one line each, and
// every name on it is dead.
struct AdmiredRow { uint16_t sc; const char* nm; const char* ln; };
static constexpr uint16_t ADMIRED_WIN = 10000;
static const AdmiredRow ADMIRED[] = {
  { 10000, "SAINT ABEL",      "walked out at ten thousand. nobody has come back to say what out looks like." },
  {  9100, "THE CARTOGRAPHER","mapped every hex on the ring. died on the one he started from." },
  {  8300, "MOTHER GRILLE",   "fed nine hundred strangers. ate last, the one time it mattered." },
  {  7400, "QUIET KORO",      "built two rafts and gave away the one that floated." },
  {  6600, "TEETH",           "won every fight out here. lost the argument about the water." },
  {  5900, "DELPH",           "surveyed the whole north ridge and never once went down into it." },
  {  5200, "OLD PELL",        "ninety-one days. spent the last four looking for his glasses." },
  {  4700, "HANNA VOSS",      "carried the medicine four days to a town that had already finished." },
  {  4300, "THE COURIER",     "delivered every package. the last one was addressed to her." },
  {  4000, "BRACE MULDOON",   "traded his rifle for a roof and was proved right for six weeks." },
  {  3800, "SISTER ANNEX",    "preached that the wasteland provides. it provided." },
  {  3650, "LOW TOM",         "died rich in scrap. scrap is not water." },
  {  3500, "VERA ASH",        "found three settlements. none of them were looking for her." },
  {  3400, "THE ACCOUNTANT",  "kept a ledger of everything he was owed. we buried it with him." },
  {  3300, "GIL MARROW",      "reached the caravan carrying nothing the caravan would take." },
  {  3200, "PIP ENSLEY",      "starved two hexes from a forage ground she had already found." },
  {  3100, "DOC HALVERS",     "treated everyone. kept his own wounds for later." },
  {  3000, "THE AVERAGE MAN", "got exactly this far, like almost all of you. admired for the punctuality." },
  {  2900, "RUTH KANE",       "famous for surviving a storm she chose to walk into." },
  {  2800, "HOLLIS PEMM",     "slept forty nights underground and died of the one night out." },
  {  2700, "THE TWINS",       "shared everything. the ration, the shelter, the fever." },
  {  2600, "MAGGS",           "lost the map on day six and kept walking with great confidence." },
  {  2500, "CUT-RATE ELIAS",  "sold his shelter for three days of food and ate it in one night." },
  {  2400, "NELLA BRUNE",     "survived the rads, the flood and the dogs. the dawn got her." },
  {  2300, "BOSS RIKE",       "ran a settlement for a season. the settlement ran out." },
  {  2200, "WENDEL FRAY",     "crossed the glass for a rumour and brought the rumour back intact." },
  {  2100, "THE GLEANER",     "picked over eleven hundred hexes and never put up a roof." },
  {  2000, "ODESSA PIKE",     "went down the hatch to get out of the rain." },
  {  1800, "CARTER ILL",      "knew the water was bad. was very thirsty." },
  {  1600, "SMALL AGNES",     "traded away the coat. it was warm out, and then it was not." },
  {  1400, "THE OPTIMIST",    "was right about the weather and wrong about everything else." },
  {  1200, "JODIE SAWN",      "reached the settlement, then kept going to see what else there was." },
  {   900, "FENN",            "admired for the speed. not for the direction." },
  {   600, "THE VOLUNTEER",   "went into the crater first so nobody else had to. nobody else was going to." },
  {   350, "TILLY MOSS",      "died on day two. every story about her is from day one." },
  {   120, "KEV",             "stepped off the ridge on the first morning. still on the board, somehow." },
};
static constexpr int ADMIRED_COUNT = (int)(sizeof(ADMIRED) / sizeof(ADMIRED[0]));

// A citation set in the 6 px hand across two lines. 39 characters is what
// the 234 px of ladder holds at the 6 px pitch; the break is taken at the
// last space that fits, and a citation long enough to need a third line is
// cut with an ellipsis rather than run under the panel edge. No citation in
// the table needs one today — this only has to hold if someone writes a
// longer row later.
static constexpr int ADM_WRAP = 39;
static void admiredWrap(const char* s, char* a, char* b) {
  a[0] = b[0] = 0;
  size_t n = strlen(s);
  if (n <= (size_t)ADM_WRAP) { strlcpy(a, s, ADM_WRAP + 1); return; }
  int cut = ADM_WRAP;
  while (cut > 0 && s[cut] != ' ') cut--;
  if (cut == 0) cut = ADM_WRAP;           // one unbroken word: hard break
  memcpy(a, s, cut);
  a[cut] = 0;
  const char* rest = s + cut + (s[cut] == ' ' ? 1 : 0);
  strlcpy(b, rest, ADM_WRAP + 1);
  if (strlen(rest) > (size_t)ADM_WRAP) { b[ADM_WRAP - 2] = '.'; b[ADM_WRAP - 1] = '.'; }
}

static void drawAdmiredScreen() {
  static const uint32_t C_HDR  = 0xD06818;
  static const uint32_t C_TXT  = 0xC87840;
  static const uint32_t C_INFO = 0x904030;
  static const uint32_t C_LINE = 0x502010;
  static const uint32_t C_DIM  = 0x3A1808;
  static const uint32_t C_BAND = 0x1E0A00;
  static const uint32_t C_LIVE = 0xE8A828;   // the living, who are not admired yet
  static const uint32_t C_ROW  = 0x2A1204;   // the lit band behind a living row

  struct { bool on; uint16_t sc; char name[17]; } snap[MAX_PLAYERS];
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    snap[i].on = G.players[i].connected;
    snap[i].sc = G.players[i].score;
    memcpy(snap[i].name, G.players[i].name, 16);
    snap[i].name[16] = 0;
  }
  xSemaphoreGive(G.mutex);

  // The dead and the living in one ranked list. A tie puts the living above
  // the dead: you are not admired yet, but you are here.
  struct Row { uint16_t sc; const char* nm; const char* ln; int8_t pid; };
  Row rows[ADMIRED_COUNT + MAX_PLAYERS];
  int n = 0;
  for (int i = 0; i < ADMIRED_COUNT; i++)
    rows[n++] = { ADMIRED[i].sc, ADMIRED[i].nm, ADMIRED[i].ln, (int8_t)-1 };
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (snap[i].on) rows[n++] = { snap[i].sc, snap[i].name, nullptr, (int8_t)i };
  for (int i = 1; i < n; i++) {            // insertion sort — 42 rows worst case
    Row k = rows[i];
    int  j = i - 1;
    while (j >= 0 && (rows[j].sc < k.sc ||
                      (rows[j].sc == k.sc && rows[j].pid < 0 && k.pid >= 0))) {
      rows[j + 1] = rows[j];
      j--;
    }
    rows[j + 1] = k;
  }

  int      best   = -1;                    // first living row = the party high
  for (int i = 0; i < n; i++) if (rows[i].pid >= 0) { best = i; break; }
  uint16_t bestSc = (best >= 0) ? rows[best].sc : 0;

  // ADMIRED is descending, so the last row still above the party is the
  // nearest one, and the first row at or below it is the one most recently
  // stepped over. Both are the dead only: another survivor being ahead of
  // you is a different screen's business.
  const AdmiredRow* next = nullptr;
  const AdmiredRow* past = nullptr;
  for (int i = ADMIRED_COUNT - 1; i >= 0; i--) if (ADMIRED[i].sc >  bestSc) { next = &ADMIRED[i]; break; }
  for (int i = 0; i < ADMIRED_COUNT;   i++)    if (ADMIRED[i].sc <= bestSc) { past = &ADMIRED[i]; break; }

  char buf[48], l1[ADM_WRAP + 1], l2[ADM_WRAP + 1], nm[32];
  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 29, C_BAND, true);
  canvasText26p("THE ADMIRED", 4, 1, C_HDR);
  canvasLine(0, 29, 239, 29, C_LINE);

  // -- Strip: the win line, and the party's high-water mark ----
  canvasText8("OUT AT", 2, 35, C_INFO);
  snprintf(buf, sizeof(buf), "%u", (unsigned)ADMIRED_WIN);
  canvasText16p(buf, 44, 31, C_TXT);
  snprintf(buf, sizeof(buf), "%u", (unsigned)bestSc);
  canvasText16pR(buf, DASH_R, 31, best >= 0 ? C_LIVE : C_DIM);
  canvasText8R("BEST", DASH_R - canvasWidth16p(buf) - 6, 35, C_INFO);
  canvasLine(0, 50, 239, 50, C_LINE);

  // -- The chase -----------------------------------------------
  // The score is 26 px because it is the only number on this screen anyone
  // is actually playing against.
  if (next) {
    snprintf(buf, sizeof(buf), "%u", (unsigned)next->sc);
    canvasText26pR(buf, DASH_R, 52, C_HDR);
    int sx = DASH_R - canvasWidth26p(buf) - 8;
    canvasText8("NEXT ABOVE", 2, 56, C_INFO);
    canvasFit16p(nm, sizeof(nm), next->nm, sx - 2);
    canvasText16p(nm, 2, 66, C_TXT);
    snprintf(buf, sizeof(buf), "%u TO GO", (unsigned)(next->sc - bestSc));
    canvasText8R(buf, DASH_R, 84, C_INFO);
    admiredWrap(next->ln, l1, l2);
    canvasText8(l1, 2,  92, C_INFO);
    canvasText8(l2, 2, 101, C_INFO);
  } else {
    canvasText8("NOTHING ABOVE", 2, 56, C_INFO);
    canvasText16p("THE WAY OUT", 2, 66, C_LIVE);
    canvasText8("ten thousand, and nobody to pass.", 2, 92, C_INFO);
  }
  canvasLine(0, 110, 239, 110, C_LINE);

  // -- The ladder ----------------------------------------------
  // Anchored three rows above the party's leader, so what is still in reach
  // is on screen with the rest of the party under it.
  static const int LAD_Y0 = 114, LAD_H = 17, LAD_ROWS = 9;
  int start = (best >= 0) ? best - 3 : 0;
  if (start > n - LAD_ROWS) start = n - LAD_ROWS;
  if (start < 0) start = 0;
  for (int k = 0; k < LAD_ROWS && start + k < n; k++) {
    const Row& r  = rows[start + k];
    int        y  = LAD_Y0 + k * LAD_H;
    bool       lv = r.pid >= 0;
    if (lv) canvasRect(0, y - 1, 239, y + 15, C_ROW, true);
    snprintf(buf, sizeof(buf), "#%d", start + k + 1);
    canvasText8(buf, 2, y + 4, lv ? C_TXT : C_DIM);
    snprintf(buf, sizeof(buf), "%u", (unsigned)r.sc);
    canvasText16pR(buf, 78, y, lv ? C_LIVE : C_TXT);
    if (lv) {
      char num[2] = { (char)('1' + r.pid), 0 };
      canvasRect(84, y, 98, y + 15, C_HDR, true);
      canvasText16pC(num, 91, y, C_BAND);
    }
    canvasFit16p(nm, sizeof(nm), r.nm, DASH_R - 102);
    canvasText16p(nm, 102, y, lv ? C_LIVE : C_TXT);
  }
  canvasLine(0, 269, 239, 269, C_LINE);

  // -- The wake ------------------------------------------------
  // The window is anchored on the party's leader, so nobody living is ever
  // off the top of it -- but a straggler can be well off the bottom, and a
  // board that silently left them out would be lying about the party.
  int below = 0;
  for (int i = start + LAD_ROWS; i < n; i++) if (rows[i].pid >= 0) below++;
  canvasText8("LAST PASSED", 2, 274, C_INFO);
  if (below) {
    snprintf(buf, sizeof(buf), "%d MORE SURVIVOR%s BELOW", below, below > 1 ? "S" : "");
    canvasText8R(buf, DASH_R, 274, C_DIM);
  }
  if (past) {
    snprintf(buf, sizeof(buf), "%u", (unsigned)past->sc);
    canvasText16pR(buf, DASH_R, 282, C_DIM);
    canvasFit16p(nm, sizeof(nm), past->nm, DASH_R - canvasWidth16p(buf) - 10);
    canvasText16p(nm, 2, 282, C_TXT);
    admiredWrap(past->ln, l1, l2);
    canvasText8(l1, 2, 300, C_INFO);
    canvasText8(l2, 2, 309, C_INFO);
  } else {
    canvasText16p("nobody yet", 2, 282, C_DIM);
    canvasText8("the board starts at 120. that is KEV.", 2, 300, C_DIM);
    canvasText8("he fell off a ridge on day one.", 2, 309, C_DIM);
  }
}

// ── Screen dispatch ────────────────────────────────────────────────────────
// Renders whichever screen button B has landed on into the canvas. Does not
// push — the caller decides whether the frame goes out clean or as one step
// of the switch transition below.
static void drawActiveScreen() {
  switch (k10Screen) {
    case 2:  drawEventLogScreen();   break;
    case 3:  drawResourceScreen();   break;
    case 4:  drawEncounterScreen();  break;
    case 5:  drawMapScreen();        break;
    case 6:  drawAdmiredScreen();    break;
    default: drawPlayerScreen();     break;  // case 1
  }
}

// ── Screen switch transition: fade out, jitter, fade in ────────────────────
// The panel backlight isn't on a PWM we can borrow mid-loop, so the fade is
// done on the sprite instead: fillRectAlpha() blends the whole canvas toward
// black before each push. That works cumulatively on the way *out* — the
// outgoing screen is still sitting in the canvas from the last push, so no
// redraw is needed — but not on the way in: brightening an already-blended
// buffer is lossy, so each fade-in step re-renders the new screen and blends
// that down to the step's darkness. The step counts are deliberately small.
// A full-screen alpha blend is a per-pixel read-modify-write over a 240×320
// PSRAM sprite, and that, not the delays, is what this costs.

// Push the canvas with a per-band horizontal offset so the frame lands torn
// rather than square — the switch reads as a tube dropout rather than a
// clean crossfade. `mag` is the worst-case offset in pixels; 0 pushes clean.
// The bands are clipped on the *display*, not the sprite, so the sprite is
// never disturbed and the next fade step still has a whole screen to blend.
// Whatever a band's offset uncovers is blacked out, or it would keep showing
// a sliver of the frame before it.
static void pushCanvasJittered(int mag) {
  if (mag <= 0) { canvas.pushSprite(0, 0); return; }
  const int BANDS = 5;
  const int BH    = 320 / BANDS;
  for (int b = 0; b < BANDS; b++) {
    int y  = b * BH;
    int h  = (b == BANDS - 1) ? 320 - y : BH;
    int dx = (int)random(-mag, mag + 1);
    tft.setClipRect(0, y, 240, h);
    canvas.pushSprite(&tft, dx, 0);
    tft.clearClipRect();
    if      (dx > 0) tft.fillRect(0, y, dx, h, 0);
    else if (dx < 0) tft.fillRect(240 + dx, y, -dx, h, 0);
  }
}

static void screenSwitchTransition() {
  // Out: cumulative blends toward black, the jitter widening as it goes.
  for (int i = 0; i < 3; i++) {
    canvas.fillRectAlpha(0, 0, 240, 320, 120, (uint16_t)0);
    pushCanvasJittered(1 + i * 2);
  }
  tft.fillScreen(0);
  delay(30);   // the beat the switch hangs on

  // In: re-render per step, blended down to that step's darkness, settling
  // to a clean unjittered push on the last one.
  static const uint8_t IN_ALPHA[3]  = {170, 80, 0};
  static const int8_t  IN_JITTER[3] = {4, 2, 0};
  for (int i = 0; i < 3; i++) {
    drawActiveScreen();
    if (IN_ALPHA[i]) canvas.fillRectAlpha(0, 0, 240, 320, IN_ALPHA[i], (uint16_t)0);
    pushCanvasJittered(IN_JITTER[i]);
  }
}

// ── K10 button B screen switching ──────────────────────────────────────────
// Screens: 1=Players 2=Events 3=Resources 4=Encounters 5=Map 6=Admired
static void checkGestureSwitch() {
  bool btnB = k10.buttonB && k10.buttonB->isPressed();
  if (btnB && !k10BtnBLast) {
    k10Screen = (k10Screen % 6) + 1;
    k10ScreenXition = true;   // consumed by the display block in the .ino loop
    k10Play(MOTIF_SCREEN_CLICK);
  }
  k10BtnBLast = btnB;
}
