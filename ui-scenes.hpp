#pragma once
// ── ui-scenes.hpp ────────────────────────────────────────────────────────────
// The K10 screens as pictures. Each one is a comic panel the table can read
// from across the room: the story is drawn, the few numbers that matter ride
// along as pictograms, and the jokes are the wasteland's own -- dry, a little
// grim, never cute.
//
// These draw straight into the canvas's pixel buffer with the primitives in
// ui-fx.hpp (spans, strokes, halftone, the two screen faces), in its sixteen
// amber rungs, so a scene and a cut-in over it are one palette and one hand.
// The one thing on them that is not amber is the Creeping Doom's green gas.
// They animate: the board repaints a scene screen every scenePeriod() ms
// instead of every SCREEN_MS, and the compositor goes on doing everything it
// always did on top.
//
// Like ui-fx.hpp this header is portable -- it compiles under FX_NATIVE, and
// scripts/fxsim renders every scene to GIF from this exact code. Nothing here
// touches G, the network or the panel: the board fills a SceneSnap under
// G.mutex (sceneSnapFill in ui-screens.hpp) and hands it in.
//
// The diagnostics that survived: the join address (the one thing a new player
// actually needs off this screen) and the LAN address, in the footer. Heap,
// uptime and the rest went to the serial log, where they always were too.

static const int SC_SEATS = 6;

struct SceneSurv {
  bool     on;
  char     name[16];
  uint8_t  arch;                 // 0 Guide 1 Quartermaster 2 Medic 3 Mule 4 Scout 5 Endurer
  uint8_t  ll, llCap, food, water, rad;
  int8_t   ap;
  uint8_t  depth;                // 0 on the surface
  bool     inEnc;
  uint16_t score;
  int16_t  q, r;                 // on the surface map (pinned to their hatch while below)
  uint8_t  res[5];               // water food fuel meds scrap
  uint8_t  packUsed, packCap;    // cap is the effective one, gear bonuses in
  uint16_t encCount;             // encounters ever entered
  uint8_t  encRolls, encWins;    // this boot
  uint8_t  lastEnd;              // how the last one ended (ENC_END_*)
  bool     hasEnd;
  char     biome[12];            // while inside: which pool
  uint8_t  loot;                 // while inside: what is at stake
};
// One chronicle entry, already written out as the sentence the page shows
// (names filled in by the board -- see sceneLogFill in ui-screens.hpp).
struct SceneLog {
  char     text[100];
  uint8_t  glyph;                // SC_GLY_*, the entry's margin mark
  uint8_t  tone;                 // SC_TONE_*
  uint16_t day;
  int8_t   who, who2;            // seats, or -1
  uint16_t serial;               // which entry this is, ever: the page number
};
// Mirrors of the board's K10Glyph and K10Tone (Esp32HexMapCrawl.ino), same
// order; ui-screens.hpp static_asserts the two lists agree.
enum : uint8_t {
  SC_GLY_NONE = 0,
  SC_GLY_DAWN, SC_GLY_FORAGE, SC_GLY_WATER, SC_GLY_MEDIC, SC_GLY_SALVAGE, SC_GLY_SHELTER,
  SC_GLY_CRAFT, SC_GLY_SCOUT, SC_GLY_REST, SC_GLY_TRADE, SC_GLY_CARAVAN, SC_GLY_ARRIVE,
  SC_GLY_DEPART, SC_GLY_THRESHOLD, SC_GLY_CLASH, SC_GLY_LIGHT, SC_GLY_HAUL, SC_GLY_WOUND,
  SC_GLY_DEATH, SC_GLY_DOOM, SC_GLY_FIRE, SC_GLY_FLOOD, SC_GLY_QUAKE, SC_GLY_SETTLE,
  SC_GLY_RAIN, SC_GLY_STORM, SC_GLY_CHEM, SC_GLY_FOG,
  SC_GLY_RAD, SC_GLY_MEDAL,
  SC_GLY_TRIP, SC_GLY_GUNFIRE,
  SC_GLY_ROUGH, SC_GLY_ROUGH_TREE, SC_GLY_ROUGH_CHALK,
  SC_GLY_COUNT
};
enum : uint8_t { SC_TONE_PLAIN = 0, SC_TONE_GOOD, SC_TONE_ILL, SC_TONE_OMEN };

struct SceneSnap {
  uint16_t  day;
  uint16_t  dayMil;              // how far through the day, 0-999; 0 is dawn
  uint8_t   weather;             // 0 clear 1 rain 2 storm 3 chem 4 smog 5 fog
  uint8_t   tcWeight;            // the threat clock, 0-255 across its whole range
  uint8_t   threat;              // the clock's own number
  uint8_t   doomAware;           // 0-100
  uint8_t   doomClose;           // 0 far/unaware .. 255 on top of someone
  SceneSurv s[SC_SEATS];
  char      ap[16];              // the hotspot address
  char      lan[20];             // the LAN address, or what the Wi-Fi is doing
  bool      lanUp;
  SceneLog  log[3];              // the last three chronicle entries, oldest first
  uint8_t   logN;
  uint16_t  logTotal;            // entries ever set down
  // screen 3: whose stash is on the page, and what is in their pack
  int8_t    page;
  char      item[18][22];
  uint8_t   itemQty[18];
  uint8_t   itemN;
  // screen 4: the rolls, this boot
  uint16_t  poiLeft, poiTotal, rolls, wins;
  bool      hasRoll, rollWon;
  int8_t    rollWho;
  uint8_t   rollSkill, rollDN;
  int8_t    rollTotal;
  // screen 5: the map (terr/eco point at buffers the board owns)
  const uint8_t* terr;
  const uint8_t* eco;
  int16_t   cols, rows;
  int16_t   caravanQ, caravanR, doomQ, doomR;
  bool      caravanOn;
  int16_t   kaneQ, kaneR;   // Machine Gun Kane's jeep (kane.hpp)
  uint8_t   kaneMode;       // 0 driving, 1 at his fire, 2 gone (parked for good)
  int16_t   vacQ, vacR;     // VACANCY's town under the sign (vacancy.hpp)
  uint8_t   vacStage;       // 0 lit, 1 counting, 2 saved (dark), 3 fallen (NO VACANCY)
  uint8_t   hatchN;
  int16_t   hatchQ[8], hatchR[8];
  uint32_t  wasteFault;     // 0: the scope came up this visit; else how it is broken (scWasteBreak)
  uint32_t  wasteSince;     // when button B landed on it
};

// ── 1. Small hands ───────────────────────────────────────────────────────────
static inline void scPx(int x, int y, uint16_t c) {
  if ((unsigned)x >= (unsigned)FX_W || (unsigned)y >= (unsigned)FX_H) return;
  if (fxColT && (y < fxColT[x] || y >= fxColB[x])) return;     // a panel's clip holds these too
  fxDst[y * FX_W + x] = c;
}
// A span set in halftone between two rungs: `lv` 0-15, its fraction deciding
// how many dots of the rung above are printed. This is how every gradient in
// the scenes is made -- a comic had no other way to shade.
static void scTone(int y, int x0, int x1, float lv) {
  if ((unsigned)y >= (unsigned)FX_H) return;
  if (lv < 0) lv = 0;
  if (lv > 14.99f) lv = 14.99f;
  int lo = (int)lv, fr = (int)((lv - lo) * 256.0f);
  if (x0 < 0) x0 = 0;
  if (x1 > FX_W) x1 = FX_W;
  if (fxColT) {
    while (x0 < x1 && (y < fxColT[x0] || y >= fxColB[x0])) x0++;
    while (x1 > x0 && (y < fxColT[x1 - 1] || y >= fxColB[x1 - 1])) x1--;
  }
  const uint8_t* dots = FX_DOT + (y & 7) * 8;
  uint16_t a = FX_PAL[lo], b = FX_PAL[lo + 1];
  uint16_t* row = fxDst + y * FX_W;
  for (int x = x0; x < x1; x++) row[x] = ((int)dots[x & 7] < fr) ? b : a;
}
static inline bool scDot(int x, int y, float f) {       // a halftone test, f 0-1
  return (int)FX_DOT[(y & 7) * 8 + (x & 7)] < (int)(f * 256.0f);
}
// The glcd hand with a hard ink shadow under it, for type set over a picture.
static int scType(const char* s, int x, int y, int sc, uint8_t col) {
  fxGlcd(s, x + 1, y + 1, sc, FX_PAL[FXP_INK]);
  return fxGlcd(s, x, y, sc, FX_PAL[col]);
}
static inline int scTypeW(const char* s, int sc) { return (int)strlen(s) * 6 * sc; }
// The widest prefix of `s` that fits `w` px in Font2.
static void scFitF2(char* out, size_t cap, const char* s, int w) {
  size_t n = 0;
  int    x = 0;
  while (s[n] && n + 1 < cap) {
    int cw = fxF2W(fxF2C(s[n]));
    if (x + cw > w) break;
    x += cw;
    n++;
  }
  memcpy(out, s, n);
  out[n] = 0;
}
// Column clip for a panel: nothing drawn with fxPut/fxSpan leaves rows y0..y1.
static int16_t scClipT[FX_W], scClipB[FX_W];
static void scClip(int y0, int y1) {
  for (int x = 0; x < FX_W; x++) { scClipT[x] = (int16_t)y0; scClipB[x] = (int16_t)y1; }
  fxColT = scClipT; fxColB = scClipB; fxColY0 = y0; fxColY1 = y1;
}
static void scUnclip() { fxColT = fxColB = nullptr; fxColY0 = 0; fxColY1 = FX_H; }
static void scReset(uint16_t* buf) {
  fxDst = buf;
  fxHalfN = 0; fxStip = 0;
  scUnclip();
}

// A speech balloon: white-hot, inked, a tail down to whoever said it.
static void scBalloon(const char* s, int cx, int by, int tailX, int tailY) {
  int w = scTypeW(s, 1) + 8, h = 13, x0 = cx - w / 2, y0 = by - h;
  if (x0 < 1) x0 = 1;
  if (x0 + w > FX_W - 1) x0 = FX_W - 1 - w;
  fxTri((float)fxMaxI(x0 + 3, fxMinI(x0 + w - 8, tailX - 3)), (float)by - 1,
        (float)fxMaxI(x0 + 8, fxMinI(x0 + w - 3, tailX + 3)), (float)by - 1,
        (float)tailX, (float)tailY, FX_PAL[FXP_INK]);
  fxRect(x0 - 1, y0 - 1, x0 + w + 1, by + 1, FX_PAL[FXP_INK]);
  fxRect(x0, y0, x0 + w, by, FX_PAL[FXP_GLOW]);
  fxTri((float)fxMaxI(x0 + 4, fxMinI(x0 + w - 7, tailX - 2)), (float)by - 1,
        (float)fxMaxI(x0 + 7, fxMinI(x0 + w - 4, tailX + 2)), (float)by - 1,
        (float)tailX, (float)tailY - 2, FX_PAL[FXP_GLOW]);
  fxGlcd(s, x0 + 4, y0 + 3, 1, FX_PAL[FXP_INK]);
}

// A narration box, top-left of a panel, typed in as it arrives. Lines are
// split on '|'. The comic's caption box: the voice that is not anybody.
static void scNarration(const char* s, int x, int y, uint32_t since) {
  char lines[3][34];
  int  n = 0, len = 0, wMax = 0;
  lines[0][0] = 0;
  for (const char* p = s; *p && n < 3; p++) {
    if (*p == '|') { lines[n][len] = 0; n++; len = 0; if (n < 3) lines[n][0] = 0; continue; }
    if (len < 33) lines[n][len++] = *p;
  }
  if (n < 3) { lines[n][len] = 0; n++; }
  for (int i = 0; i < n; i++) wMax = fxMaxI(wMax, (int)strlen(lines[i]));
  int w = wMax * 6 + 9, h = n * 10 + 5;
  fxRect(x + 2, y + 2, x + w + 2, y + h + 2, FX_PAL[FXP_INK]);        // drop shadow
  fxRect(x - 1, y - 1, x + w + 1, y + h + 1, FX_PAL[FXP_INK]);
  fxRect(x, y, x + w, y + h, FX_PAL[FXP_CRIT]);
  int shown = (int)(since / 28);
  for (int i = 0; i < n; i++) {
    char t[34];
    int  k = fxMinI((int)strlen(lines[i]), fxMaxI(0, shown));
    memcpy(t, lines[i], k); t[k] = 0;
    fxGlcd(t, x + 5, y + 4 + i * 10, 1, FX_PAL[FXP_INK]);
    shown -= (int)strlen(lines[i]);
  }
}

// ── 2. Pictograms ────────────────────────────────────────────────────────────
// Stats as things. Tiny pixel marks, 5-7 px, drawn off rows of bits so they
// stay crisp at the size a heart has to be to fit ten on a line.
static void scBits(const uint8_t* rows, int h, int w, int x, int y, uint16_t col) {
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++)
      if (rows[r] & (0x80 >> c)) scPx(x + c, y + r, col);
}
static const uint8_t SC_HEART[5]  = { 0x50, 0xF8, 0xF8, 0x70, 0x20 };
static const uint8_t SC_HEARTO[5] = { 0x50, 0xA8, 0x88, 0x50, 0x20 };
static const uint8_t SC_CAN[7]    = { 0x70, 0xF8, 0x88, 0xF8, 0xF8, 0x88, 0x70 };   // a tin, ribbed
static const uint8_t SC_DROP[7]   = { 0x20, 0x20, 0x70, 0x70, 0xF8, 0xF8, 0x70 };
static const uint8_t SC_TREF[7]   = { 0x44, 0xEE, 0xEE, 0x10, 0x00, 0x38, 0x38 };   // the trefoil
static const uint8_t SC_SKULL[7]  = { 0x70, 0xF8, 0xA8, 0xF8, 0x70, 0x50, 0x00 };
// The Creeping Doom on the map, where the gas is too small to hold a face
// (fxGasSkull gives up under R 7): a skull set by hand, seven by seven.
static const uint8_t SC_DOOM_MARK[7] = { 0x7C, 0xFE, 0x92, 0x92, 0xEE, 0x7C, 0x54 };

// ── 3. The road: sky, weather, the ruins going by ────────────────────────────
// Keyframes of the sky over the day, top of the sky and the horizon, in rungs.
// t = 0 is dawn, the way the day clock counts it (ui-leds.hpp's TOD_STOPS).
struct ScSkyKey { float t, top, hor; };
static const ScSkyKey SC_SKY[] = {
  { 0.00f, 2.0f, 11.0f }, { 0.08f, 4.2f, 10.2f }, { 0.30f, 5.4f, 9.4f }, { 0.55f, 4.4f, 10.4f },
  { 0.66f, 2.2f, 12.2f }, { 0.74f, 1.0f, 7.5f },  { 0.80f, 0.3f, 3.2f }, { 0.90f, 0.1f, 2.2f },
  { 0.96f, 1.0f, 6.0f },  { 1.00f, 2.0f, 11.0f },
};
static void scSkyAt(float t, float* top, float* hor) {
  int n = (int)(sizeof(SC_SKY) / sizeof(SC_SKY[0]));
  for (int i = 0; i < n - 1; i++)
    if (t <= SC_SKY[i + 1].t) {
      float u = (t - SC_SKY[i].t) / (SC_SKY[i + 1].t - SC_SKY[i].t);
      u = fxSmooth(fxClampF(u, 0, 1));
      *top = fxLerp(SC_SKY[i].top, SC_SKY[i + 1].top, u);
      *hor = fxLerp(SC_SKY[i].hor, SC_SKY[i + 1].hor, u);
      return;
    }
  *top = SC_SKY[0].top; *hor = SC_SKY[0].hor;
}

// The road screen's geometry.
static const int RD_Y0 = 20;       // under the masthead
static const int RD_HOR = 130;     // where the ruins stand
static const int RD_NEAR = 142;    // where the near wreckage stands
static const int RD_FOOT = 161;    // where the party walks
static const int RD_Y1 = 170;      // the bottom of the panel

enum : uint8_t { SC_WX_CLEAR = 0, SC_WX_RAIN, SC_WX_STORM, SC_WX_CHEM, SC_WX_SMOG, SC_WX_FOG };

// Ruins on the skyline: blocks of hashed width and height with tops bitten
// and sheared, gutted windows the sky shows through, the odd steel skeleton
// with only its floors left, a tower with its spire, one leaning on its
// neighbour. Two of these ride at different speeds -- the far one paler, the
// way distance is in haze -- and they overlap, the way a city does. Once in a
// long while, at night, one window is lit. Nobody lives there.
static inline void scDither(int x, int y, float lv) {
  if ((unsigned)x >= (unsigned)FX_W || (unsigned)y >= (unsigned)FX_H) return;
  if (fxColT && (y < fxColT[x] || y >= fxColB[x])) return;
  lv = fxClampF(lv, 0, 14.99f);
  int lo = (int)lv;
  fxDst[y * FX_W + x] = FX_PAL[scDot(x, y, lv - lo) ? lo + 1 : lo];
}
static void scRuins(float scroll, int baseY, float lv, bool night, uint32_t now, uint32_t seed,
                    int cell, int hMin, int hMax) {
  int k0 = (int)floorf((scroll - 40) / cell), k1 = (int)floorf((scroll + FX_W + 4) / cell);
  for (int k = k0; k <= k1; k++) {
    uint32_t h = fxHash2((uint32_t)k, seed);
    if (fxU(h) > 0.86f) continue;
    int   w  = 9 + (int)(h % 18);
    int   x0 = k * cell + (int)((h >> 5) % (uint32_t)(cell / 2)) - (int)floorf(scroll);
    int   kind = (int)((h >> 12) % 9);
    float H = hMin + (hMax - hMin) * (0.35f + 0.65f * fxNoise1(k * 0.37f, seed)) * (0.6f + 0.4f * fxU(h >> 3));
    if (kind == 7) H *= 0.45f;                                // a stump, mostly rubble now
    float lean = kind == 5 ? ((h >> 22) & 1 ? 0.2f : -0.2f) : 0.0f;
    float amp  = kind == 6 ? 0.0f : 3.0f + (float)((h >> 16) % 11);
    int   bite = ((h >> 20) & 3) == 0 ? (int)((h >> 23) % 3) + 1 : 0;
    for (int lx = 0; lx < w; lx++) {
      float tn  = fxNoise1((k * 37 + lx) * 0.55f, seed + 1);
      int   top = baseY - (int)H + (int)(tn * amp);
      if (kind == 6) {                                         // setbacks, and a spire
        if (lx < w / 4 || lx >= w - w / 4) top += (int)(H * 0.28f);
        if (lx == w / 2) top -= 10;
      }
      if (bite) top += fxMaxI(0, lx - w / 3) * bite / 2;       // a slab sheared off
      for (int y = fxMaxI(top, RD_Y0); y < baseY; y++) {
        int fy = baseY - y, x = x0 + lx + (int)(lean * fy);
        bool hole = false;
        if (kind == 4) {                                       // the steel skeleton
          hole = !(lx % 5 == 0 || lx == w - 1 || fy % 6 == 0 || (y < top + 2 && lx % 5 < 2));
        } else if (lx > 0 && lx < w - 1 && y > top + 2 && fy > 3 && lx % 3 == 1 && fy % 5 >= 2 && fy % 5 <= 3) {
          uint32_t wh = fxHash2((uint32_t)(k * 131 + lx), (uint32_t)(fy / 5));
          hole = (wh & 3) != 0;
          if (hole && night && (wh & 255) < 3 && ((wh >> 9) + now / 2300) % 5) {
            scPx(x, y, FX_PAL[FXP_HOT]);
            continue;
          }
        }
        if (!hole) scDither(x, y, lv);
      }
    }
    if (kind == 4 && ((h >> 25) & 1)) {                        // a girder hanging off it
      float gx = (float)(x0 + w), gy = baseY - H + 6;
      fxStroke(gx - 2, gy, gx + 9, gy + 12, 1.4f, 1.2f, FX_PAL[(int)fxClampF(lv, 0, 14)]);
    }
  }
}

// The billboards the old world left out here. Their copy is the joke.
static const char* const SC_ADS[][2] = {
  { "RAD-COLA",  "IT GLOWS!" },   { "SUNNY",     "ACRES 2 MI" },  { "VACANCY",   "(FOREVER)" },
  { "YOU ARE",   "HERE. SORRY" }, { "BUY BONDS", "WIN A ROOF" },  { "JESUS IS",  "LATE" },
  { "SAFE ZONE", "NOT HERE" },    { "DRINK MORE","WATER HA" },    { "LAWYERS",   "0 LEFT" },
  { "OPEN 24H",  "NOT OPEN" },
};
static const int SC_AD_N = (int)(sizeof(SC_ADS) / sizeof(SC_ADS[0]));

// A dead tree: a trunk that forks and forks again, every branch a tapering
// brush stroke, lit along one edge.
static void scTree(float x, float y, uint32_t seed, float len, float w, int depth, uint16_t rim) {
  FxRng R; R.s = seed;
  struct Br { float x, y, a, len, w; int d; } st[24];
  int sp = 0;
  st[sp++] = { x, y, -1.5708f + R.sgn() * 0.12f, len, w, 0 };
  while (sp) {
    Br b = st[--sp];
    float ex = b.x + cosf(b.a) * b.len, ey = b.y + sinf(b.a) * b.len;
    fxStroke(b.x - 0.8f, b.y - 0.8f, ex - 0.8f, ey - 0.8f, b.w + 0.8f, b.w * 0.6f + 0.6f, rim);
    fxStroke(b.x, b.y, ex, ey, b.w, b.w * 0.6f, FX_PAL[FXP_INK]);
    if (b.d < depth && sp < 21) {
      int kids = (b.d == 0) ? 2 : 1 + (int)(R.next() & 1);
      for (int c = 0; c < kids; c++)
        st[sp++] = { ex, ey, b.a + (c ? -1 : 1) * (0.35f + R.u() * 0.5f), b.len * (0.55f + R.u() * 0.2f),
                     b.w * 0.6f, b.d + 1 };
    }
  }
}

// The wreckage in the middle distance, one piece per 70 px of road: a mound,
// a dead tree, a leaning pylon, a burnt-out car, or a billboard.
static void scWreck(float scroll, uint8_t rimLv, bool night) {
  const int CELL = 70;
  const uint16_t ink = FX_PAL[FXP_INK], rim = FX_PAL[rimLv], soot = FX_PAL[FXP_SOOT];
  int k0 = (int)floorf((scroll - 90) / CELL), k1 = (int)floorf((scroll + FX_W + 90) / CELL);
  for (int k = k0; k <= k1; k++) {
    uint32_t h = fxHash2((uint32_t)k, 0xB0B5u);
    float bx = k * CELL + (h % 30) - scroll, by = (float)RD_NEAR + (int)((h >> 5) % 4);
    int kind = (int)((h >> 9) % 9);
    FxRng R; R.s = h;
    if (kind <= 1) {                                         // a heap of rubble
      float px[9], py[9];
      float w = 22 + R.u() * 26, hh = 6 + R.u() * 9;
      for (int i = 0; i < 9; i++) {
        float u = i / 8.0f;
        px[i] = bx - w * 0.5f + w * u;
        py[i] = by - (i == 0 || i == 8 ? 0 : hh * (1 - (2 * u - 1) * (2 * u - 1)) * (0.7f + 0.5f * R.u()));
      }
      for (int i = 0; i < 8; i++) {                          // convex pieces, lit on top
        float qx[4] = { px[i], px[i + 1], px[i + 1], px[i] }, qy[4] = { py[i] - 1, py[i + 1] - 1, by, by };
        fxPoly(qx, qy, 4, rim);
        float rx[4] = { px[i], px[i + 1], px[i + 1], px[i] }, ry[4] = { py[i] + 0.6f, py[i + 1] + 0.6f, by, by };
        fxPoly(rx, ry, 4, soot);
      }
      if (R.u() < 0.5f) fxStroke(bx - 4, by - hh * 0.7f, bx + 6, by - hh - 5, 1.6f, 1.2f, ink);   // rebar
    } else if (kind == 2 || kind == 3) {                     // a dead tree
      scTree(bx, by, h, 12 + R.u() * 8, 3.4f, 3, rim);
    } else if (kind == 4 || kind == 5) {                     // a pylon, leaning
      float lean = R.sgn() * 0.18f, H = 34 + R.u() * 10;
      float tx = bx + lean * H, ty = by - H;
      float lx0 = bx - 6, rx0 = bx + 6;
      for (int pass = 0; pass < 2; pass++) {
        uint16_t c = pass ? ink : rim;
        float o = pass ? 0.0f : -0.8f, wv = pass ? 1.4f : 2.2f;
        fxStroke(lx0 + o, by, tx - 1.5f + o, ty + o, wv, wv * 0.8f, c);
        fxStroke(rx0 + o, by, tx + 1.5f + o, ty + o, wv, wv * 0.8f, c);
        fxStroke(tx - 9 + o, ty + 5 + o, tx + 9 + o, ty + 5 + o, wv, wv, c);   // the crossarm
      }
      for (int i = 1; i < 5; i++) {                          // the bracing, a zigzag
        float u0 = i / 5.0f, u1 = (i + 1) / 5.0f;
        float ax = fxLerp(lx0, tx - 1.5f, u0), ay = fxLerp(by, ty, u0);
        float cx = fxLerp(rx0, tx + 1.5f, u1), cy = fxLerp(by, ty, u1);
        fxLine((int)ax, (int)ay, (int)cx, (int)cy, ink);
      }
      // A cable dropped off one arm, down into the dirt.
      fxLine((int)(tx + 9), (int)(ty + 5), (int)(tx + 16), (int)(by - 2), soot);
    } else if (kind == 6 || kind == 7) {                     // a burnt-out car
      bool flipped = (h >> 20) & 1;
      float w = 30, x0 = bx - w * 0.5f;
      if (!flipped) {
        // A sedan, one wheel gone so it sits nose-down. Built in its own
        // frame and tipped about the back wheel.
        bool noWheel = (h & 4) == 0;
        float tip = noWheel ? 0.09f : 0.0f, ca = cosf(tip), sa = sinf(tip);
        float rxp = x0 + 6, ryp = by - 1;
        auto P = [&](float u, float v, float* X, float* Y) {   // u right, v up, from the back wheel
          *X = rxp + ca * u + sa * v; *Y = ryp - (ca * v - sa * u);
        };
        static const float BODY[8][2] = { { -6, 2 }, { -5, 7 }, { 3, 8 }, { 7, 13 }, { 17, 13 }, { 21, 8 }, { 27, 6 }, { 27, 2 } };
        static const float GLASS[2][4][2] = { { { 4.5f, 8.5f }, { 7.5f, 12 }, { 11.5f, 12 }, { 11.5f, 8.5f } },
                                              { { 13, 8.5f }, { 13, 12 }, { 16.5f, 12 }, { 19.5f, 8.5f } } };
        float bx8[8], by8[8];
        for (int i = 0; i < 8; i++) P(BODY[i][0], BODY[i][1], &bx8[i], &by8[i]);
        for (int i = 0; i < 8; i++) { bx8[i] -= 0.8f; by8[i] -= 0.8f; }
        fxPoly(bx8, by8, 8, rim);                                 // the light along its roof
        for (int i = 0; i < 8; i++) P(BODY[i][0], BODY[i][1], &bx8[i], &by8[i]);
        fxPoly(bx8, by8, 8, ink);
        for (int g2 = 0; g2 < 2; g2++) {                         // no glass left, just the dark
          float gx[4], gy[4];
          for (int i = 0; i < 4; i++) P(GLASS[g2][i][0], GLASS[g2][i][1], &gx[i], &gy[i]);
          fxPoly(gx, gy, 4, FX_PAL[rimLv > 2 ? rimLv - 2 : 0]);
        }
        float wx2, wy2;
        P(0, 1.5f, &wx2, &wy2);  fxDisc(wx2, wy2, 3.3f, ink); fxDisc(wx2, wy2, 1.1f, FX_PAL[FXP_LINE]);
        P(21, 1.5f, &wx2, &wy2);
        if (!noWheel) { fxDisc(wx2, wy2, 3.3f, ink); fxDisc(wx2, wy2, 1.1f, FX_PAL[FXP_LINE]); }
        else fxRect((int)wx2 - 2, (int)wy2 - 1, (int)wx2 + 2, (int)by, ink);   // up on a brick
      } else {                                               // roof down, wheels up
        float rx[6] = { x0, x0 + w, x0 + w - 3, x0 + 24, x0 + 6, x0 + 3 };
        float ry[6] = { by - 8, by - 8, by - 1, by, by, by - 1 };
        fxPoly(rx, ry, 6, ink);
        fxLine((int)x0, (int)by - 9, (int)(x0 + w), (int)by - 9, rim);
        fxDisc(x0 + 7, by - 11, 3.0f, ink);
        fxDisc(x0 + 23, by - 11, 3.0f, ink);
      }
    } else {                                                 // a billboard
      int ad = (int)((uint32_t)(k * 7 + 3) % (uint32_t)SC_AD_N);
      int bw = 64, bh = 22, x0 = (int)bx - bw / 2, y0 = (int)by - 44;
      fxStroke(bx - 20, by, bx - 20, (float)y0 + bh, 2.2f, 2.0f, ink);
      fxStroke(bx + 20, by, bx + 17, (float)y0 + bh + 3, 2.2f, 2.0f, ink);   // one post gave
      uint16_t face = night ? FX_PAL[FXP_LINE] : FX_PAL[FXP_HDR];
      fxRect(x0 - 1, y0 - 1, x0 + bw + 1, y0 + bh + 1, ink);
      fxRect(x0, y0, x0 + bw, y0 + bh, face);
      fxTri((float)(x0 + bw - 12), (float)(y0 - 1), (float)(x0 + bw + 1), (float)(y0 - 1),
            (float)(x0 + bw + 1), (float)(y0 + 10), ink);  // a corner torn off
      for (int i = 0; i < 7; i++) {                          // weathered: flecks of the old paint gone
        uint32_t fh = fxHash2((uint32_t)i, h);
        scPx(x0 + 2 + (int)(fh % (bw - 4)), y0 + 2 + (int)((fh >> 8) % (bh - 4)), FX_PAL[FXP_RUST]);
      }
      uint16_t tc = FX_PAL[FXP_INK];
      const char* a = SC_ADS[ad][0], *b = SC_ADS[ad][1];
      fxGlcd(a, x0 + (bw - scTypeW(a, 1)) / 2, y0 + 3, 1, tc);
      fxGlcd(b, x0 + (bw - scTypeW(b, 1)) / 2, y0 + 12, 1, tc);
    }
  }
}

// The Creeping Doom over the skyline: a skull of sick green gas, hanging
// bigger and higher the closer it is, its gas going up off it like smoke off a
// fire. How well the gas holds the face is how much it cares, on the same
// tiers the web map draws (DoomGas, data/world-entities.js): a scowl once it
// has a scent, embers for eyes once it is unmaking things, the jaw going at
// 100. It looks toward the party -- they are always to its left.
static void scDoomAt(float cx, float base, float s, uint8_t aware, uint32_t now) {
  const float t = now * 0.001f;
  const float R = 6.0f + s * 26.0f;
  const float cy = base - R * 1.25f - s * 18.0f + sinf(t * 1.4f) * R * 0.08f;
  const float coh = aware >= 100 ? 1.0f : aware >= 76 ? 0.86f : aware >= 51 ? 0.70f + 0.04f * sinf(t * 1.3f) : 0.45f;
  const float jaw = aware >= 100 ? 0.26f + 0.08f * fabsf(sinf(t * 7.5f))
                  : aware >= 51 ? 0.03f + 0.05f * (0.5f + 0.5f * sinf(t * 1.6f)) : 0.0f;
  const float ember = aware >= 100 ? 1.0f : aware >= 76 ? 0.6f : 0.0f;
  const float len = aware >= 100 ? 3.0f : aware >= 76 ? 2.5f : 1.9f;
  const float bx = 0.42f, by = -0.91f;              // up, and off downwind
  fxGasPlume(cx, cy, R, bx, by, len, coh, t, 404);
  fxGasSkull(cx, cy, R, coh, -0.45f, jaw, ember, bx, by, t, 404);
}

static void scDoom(uint8_t close, uint8_t aware, uint32_t now) {
  float s = close / 255.0f;
  if (s < 0.05f) return;
  scDoomAt(262.0f - s * 78.0f, (float)RD_HOR + 2, s, aware, now);
}

// Rain, clipped to the panel. Chem rain falls fatter and hisses where it lands.
static void scRain(uint32_t now, int n, bool chem, bool heavy) {
  for (int i = 0; i < n; i++) {
    uint32_t h = fxHash2((uint32_t)i, 0xDA1Eu);
    float spd = (heavy ? 0.36f : 0.26f) + 0.2f * fxU(fxHash(h + 1));
    float len = chem ? 3.0f + 3.0f * fxU(fxHash(h + 2)) : 6.0f + 12.0f * fxU(fxHash(h + 2));
    float span = (float)(RD_Y1 - RD_Y0) + 30;
    float y = RD_Y0 - 20 + fmodf(fxU(h) * span + now * spd, span);
    float x = fxU(fxHash(h + 3)) * 280.0f - 20.0f - (y - RD_Y0) * 0.25f;
    uint8_t c = chem ? ((h & 3) == 0 ? FXP_GLOW : FXP_BLOOD) : ((h & 3) == 0 ? FXP_HDR : ((h & 3) == 1 ? FXP_OK : FXP_RUST));
    fxLine((int)x, (int)y, (int)(x - len * 0.25f), (int)(y + len), FX_PAL[c]);
    if (chem && (h & 7) == 0) {                             // where it lands, it smokes
      float sx = fxU(fxHash(h + 7)) * FX_W, ph = fmodf(now * 0.002f + fxU(h >> 3), 1.0f);
      fxDisc(sx, RD_FOOT + 2 - ph * 9, 1.0f + ph * 1.6f, FX_PAL[ph < 0.5f ? FXP_LINE : FXP_TRACK]);
    }
  }
}

// Fog in the low ground, drifting: value noise thresholded into halftone.
static void scFog(uint32_t now, float lv, float thick, int y0 = 66, int y1 = RD_Y1) {
  // The noise grid lives in PSRAM: 6.7 KB is a lot of internal RAM on a
  // board whose internal heap has starved the network before.
  static float (*N)[62] = nullptr;
  static bool tried = false;
  if (!tried) { tried = true; N = (float(*)[62])fxAlloc(sizeof(float) * 27 * 62); }
  if (!N) return;
  float t = now * 0.00004f;
  for (int j = 0; j < 27; j++)
    for (int i = 0; i < 62; i++)
      N[j][i] = fxNoise2(i * 0.11f + t * 3.0f, j * 0.28f + t, 919) * 0.7f +
                fxNoise2(i * 0.31f - t * 5.0f, j * 0.6f, 311) * 0.3f;
  if (y1 - y0 > 104) y0 = y1 - 104;                        // what the noise grid covers
  for (int y = y0; y < y1; y++) {
    float low = fxClampF((y - y0) / 70.0f, 0, 1);
    int j = (y - y0) >> 2;
    uint16_t* row = fxDst + y * FX_W;
    uint16_t c = FX_PAL[(int)lv], c2 = FX_PAL[(int)lv + 1];
    float fj = ((y - y0) & 3) * 0.25f;
    for (int x = 0; x < FX_W; x++) {
      int   i = x >> 2;
      float fi = (x & 3) * 0.25f;
      float n0 = N[j][i] + (N[j][i + 1] - N[j][i]) * fi, n1 = N[j + 1][i] + (N[j + 1][i + 1] - N[j + 1][i]) * fi;
      float n = n0 + (n1 - n0) * fj;
      float d = fxClampF((n - 0.42f + low * 0.25f) * 2.4f, 0, 1) * thick;
      if (scDot(x, y, d)) row[x] = scDot(x + 3, y + 1, d * 0.6f) ? c2 : c;
    }
  }
}

// A bolt, now and then, from the cloud to the skyline.
static bool scBoltNow(uint32_t now, uint32_t* seed) {
  uint32_t ep = now / 3700, off = fxHash(ep * 17 + 5) % 2600;
  uint32_t in = now % 3700;
  *seed = fxHash(ep);
  return in >= off && in < off + 170;
}
static void scBolt(uint32_t seed) {
  FxRng R; R.s = seed;
  float x = 30 + R.u() * 180, y = (float)RD_Y0;
  while (y < RD_HOR - 8) {
    float nx = x + R.sgn() * 9, ny = y + 5 + R.u() * 8;
    fxStroke(x, y, nx, ny, 3.0f, 2.4f, FX_PAL[FXP_GLOW]);
    fxLine((int)x, (int)y, (int)nx, (int)ny, FX_PAL[FXP_WHITE]);
    if (R.u() < 0.18f) fxLine((int)nx, (int)ny, (int)(nx + R.sgn() * 14), (int)(ny + 10), FX_PAL[FXP_HOT]);
    x = nx; y = ny;
  }
}

// A find: two thin diamonds crossed, turning over.
static void scSparkleSmall(float x, float y, uint32_t now) {
  float r = 3.0f + (now ? 1.6f * fxNoise1(now * 0.006f, 17) : 1.0f);
  fxTri(x - r, y, x, y - 0.9f, x, y + 0.9f, FX_PAL[FXP_WHITE]);
  fxTri(x + r, y, x, y - 0.9f, x, y + 0.9f, FX_PAL[FXP_WHITE]);
  fxTri(x, y - r * 1.3f, x - 0.9f, y, x + 0.9f, y, FX_PAL[FXP_WHITE]);
  fxTri(x, y + r * 1.3f, x - 0.9f, y, x + 0.9f, y, FX_PAL[FXP_WHITE]);
}

// ── 4. The party ─────────────────────────────────────────────────────────────
// Everyone is a silhouette the way an eighties cover did them: solid ink
// against the sky, a hard rim of light on the side it comes from, and only
// the few things that catch it picked out over the black -- a spur, a lens,
// the coal on the end of a cheroot. The cut of each one says who they are
// before anything else can, and the cut is the frontier gone to seed:
//
//   Guide          drifter's hat, a duster to the knee, the rifle on the shoulder
//   Quartermaster  a derby, a paunch, the crate, spectacles
//   Medic          a flat parson's hat, a bandanna over the face, the bag with the cross
//   Mule           a slouch hat under a pack that has a pack: pan, shovel, lantern
//   Scout          flying cap and goggles, a scarf the wind has hold of, the radio whip
//   Endurer        a crest, the gas mask, a poncho with spikes through it, a sawn-off, chain
//
// A figure is a skeleton first -- hip, spine, head, and two-bone legs and arms
// solved to where the feet and hands have to be -- and the gear hangs off it
// in the frame of whatever it is fixed to, so a hat stays on a head bent over
// the dirt or lying in it. There is one wind for everybody (scFigT, the frame
// clock sceneDraw sets): coat tails and scarves stream away from the way they
// face.
enum : uint8_t { SCP_WALK = 0, SCP_SIT, SCP_LIE, SCP_STAND, SCP_REACH, SCP_CROUCH, SCP_HOLD, SCP_SLUMP };
struct ScFig {
  float   x, foot;
  float   dir;          // +1 faces right
  uint8_t arch, pose, seat;
  float   ph;           // the walk's phase, radians
  uint8_t rim;          // rung of the light catching it
  float   lx, ly;       // unit toward that light
  float   k;            // scale: 1 is a figure 28 px tall, not counting the hat
  // Optional -- zero when an initialiser leaves them out:
  float   shadow;       // a long shadow on the ground, its lean in px per px of height (0: none)
  float   handU, handV; // where the near hand has to be, in figure units (handV 0: the pose decides)
  float   look;         // the head's tilt, radians, up positive (0: the pose decides)
};
static uint32_t scFigT = 0;                  // the wind's clock
static float    scHandX = 0, scHandY = 0;    // the last figure's near hand, on screen, for its props

// Two bones from (au,av) to the target (bu,bv), the joint bent to the `bend`
// side of the line (+1 ahead of it for a knee, -1 behind for an elbow). A
// target out of reach is pulled in to the end of the straightened limb.
static void scIK(float au, float av, float& bu, float& bv, float l1, float l2, float bend, float* ju, float* jv) {
  float du = bu - au, dv = bv - av, L = sqrtf(du * du + dv * dv);
  if (L < 0.01f) { *ju = au; *jv = av - l1; return; }
  float ux = du / L, uy = dv / L;
  if (L > l1 + l2 - 0.02f) { L = l1 + l2 - 0.02f; bu = au + ux * L; bv = av + uy * L; }
  float ca = fxClampF((l1 * l1 + L * L - l2 * l2) / (2.0f * l1 * L), -1.0f, 1.0f), sa = sqrtf(1.0f - ca * ca) * bend;
  *ju = au + (ux * ca - uy * sa) * l1;
  *jv = av + (uy * ca + ux * sa) * l1;
}

// pass 0: the rim (fat, pushed toward the light); 1: the ink; 2: the details
// over the ink; 3: the shadow on the ground.
static void scFigureDraw(const ScFig& F, int pass) {
  const float d = F.dir, k = F.k;
  const bool  shade = pass == 3;
  // The rim pass is drawn fat, so the sky's light wraps the whole silhouette;
  // a fire's light only catches the side facing it, or a figure by the fire
  // reads as a hollow outline.
  const bool  fire = F.rim >= FXP_FLAME;
  const float g = pass == 0 ? (fire ? 0.4f : 1.3f) : 0.0f;
  const float lo = fire ? 1.6f : 1.0f;
  const float ox = pass == 0 ? F.lx * lo : 0.0f, oy = pass == 0 ? F.ly * lo : 0.0f;
  const float shx = shade ? F.shadow : 0.0f, shy = shade ? -0.09f : 1.0f, wk = shade ? 0.55f : 1.0f;
  const uint16_t c = FX_PAL[pass == 0 ? F.rim : (shade ? FXP_SOOT : FXP_INK)];
  auto PX = [&](float u, float v) { return F.x + d * u * k + v * k * shx + ox; };
  auto PY = [&](float u, float v) { return F.foot - v * k * shy + oy; };
  auto S = [&](float u0, float v0, float u1, float v1, float w0, float w1) {
    fxStroke(PX(u0, v0), PY(u0, v0), PX(u1, v1), PY(u1, v1), w0 * k * wk + g, w1 * k * wk + g, c);
  };
  auto D = [&](float u, float v, float r) { fxDisc(PX(u, v), PY(u, v), r * k * wk + g * 0.6f, c); };
  auto Pg = [&](const float* us, const float* vs, int n) {  // a polygon in figure space, grown for the rim
    float px[8], py[8], mx = 0, my = 0;
    for (int i = 0; i < n; i++) { px[i] = PX(us[i], vs[i]); py[i] = PY(us[i], vs[i]); mx += px[i]; my += py[i]; }
    mx /= n; my /= n;
    for (int i = 0; g > 0 && i < n; i++) {
      float dx = px[i] - mx, dy = py[i] - my, l = sqrtf(dx * dx + dy * dy);
      if (l > 0.01f) { px[i] += dx / l * g * 0.6f; py[i] += dy / l * g * 0.6f; }
    }
    fxPoly(px, py, n, c);
  };
  auto WB = [&](float u0, float v0, float u1, float v1) {   // a box, square to the ground
    float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
    Pg(us, vs, 4);
  };
  auto Px = [&](float u, float v, uint8_t col) { scPx((int)floorf(PX(u, v)), (int)floorf(PY(u, v)), FX_PAL[col]); };

  // -- the skeleton, in figure space: u ahead, v up from the ground --
  const uint8_t a = F.arch < 6 ? F.arch : 0, pose = F.pose;
  const bool mule = a == 3, bulky = a == 5;
  const bool walk = pose == SCP_WALK;
  const bool upright = walk || pose == SCP_STAND || pose == SCP_REACH || pose == SCP_HOLD || pose == SCP_SLUMP;
  const bool carry = walk || pose == SCP_STAND;             // hands free for what they carry
  const float LT = 6.6f, LS = 6.6f, UA = 5.0f, FA = 4.8f, HR = 2.5f;
  const float gust = fxNoise1(scFigT * 0.0017f + F.x * 0.011f, 91);
  const float flut = sinf(scFigT * 0.013f + F.x * 0.21f);
  const float ph = walk ? F.ph : 0.0f;
  float hu = 0, hv = 13.4f, su = 0, sv = 0, hdA = 0;
  float fU[2], fV[2], bA[2] = { 0, 0 }, hU[2], hV[2];       // ankles, boot angles, hands: [0] far side, [1] near
  switch (pose) {
    case SCP_WALK:
      hv = 13.0f + 0.65f * fabsf(cosf(ph));                  // highest over the planted foot
      su = mule ? 2.4f : 1.0f; sv = hv + 8.2f;
      for (int s = 0; s < 2; s++) {
        float la = ph + (s ? 0.0f : 3.14159f), sw = sinf(la), cw = cosf(la);
        fU[s] = sw * 4.4f;
        fV[s] = 1.1f + fmaxf(0.0f, cw) * 1.9f;               // lifted on the swing through
        bA[s] = (sw < 0 ? 0.55f : 0.25f) * sw * sw * sw;     // toe down pushing off, up at the heel strike
        hU[s] = su - 0.5f - sw * 3.3f;                       // the arms swing against the legs
        hV[s] = sv - 9.3f + fabsf(sw) * 1.1f;
      }
      hdA = mule ? -0.2f : -0.05f;
      break;
    case SCP_CROUCH:
      hu = 0.2f; hv = 6.6f; su = 4.8f; sv = 13.4f; hdA = -0.55f;
      fU[0] = -5.4f; fV[0] = 2.7f; bA[0] = -1.3f;            // the back knee in the dirt, the toe dug in
      fU[1] = 5.2f;  fV[1] = 1.1f;
      hU[0] = 6.6f;  hV[0] = 7.2f;                           // a hand on the knee
      hU[1] = 10.4f; hV[1] = 1.6f;                           // the other into whatever it is
      break;
    case SCP_SIT:
      hu = 0; hv = 2.6f; su = 1.2f; sv = 10.9f; hdA = -0.2f;
      fU[0] = 7.4f; fU[1] = 8.8f; fV[0] = fV[1] = 1.1f;
      hU[0] = 8.8f; hV[0] = 9.2f; hU[1] = 10.0f; hV[1] = 10.2f;   // out to the warmth
      break;
    case SCP_LIE:
      hu = 0; hv = 2.3f; su = -8.3f; sv = 2.6f; hdA = 1.5708f;
      fU[0] = 13.0f; fV[0] = 1.2f; bA[0] = 1.35f;            // toes to the sky
      fU[1] = 11.4f; fV[1] = 2.0f; bA[1] = 1.2f;
      hU[0] = -6.2f; hV[0] = 0.7f;
      hU[1] = -2.6f; hV[1] = 8.2f;                           // one arm up. still with us
      break;
    case SCP_SLUMP:                                          // it didn't go their way: knees gone soft, head hung
      hv = 12.4f; su = mule ? 2.8f : 1.9f; sv = hv + 7.9f; hdA = -0.75f;
      fU[0] = -1.8f; fU[1] = 2.2f; fV[0] = fV[1] = 1.1f;
      hU[0] = su + 0.4f; hV[0] = sv - 9.6f;
      hU[1] = su + 1.8f; hV[1] = sv - 9.4f;
      break;
    default:                                                 // on their feet and planted: a gunfighter's stance
      hv = 13.0f;
      su = mule ? 1.8f : (pose == SCP_REACH ? -0.5f : 0.25f); sv = hv + 8.3f;
      fU[0] = -2.3f; fU[1] = 2.7f; fV[0] = fV[1] = 1.1f;
      hU[0] = su - 1.4f; hV[0] = sv - 9.4f;
      hU[1] = su + 1.7f; hV[1] = sv - 9.0f;                  // the near hand hangs by the hip, where the iron would be
      if (pose == SCP_REACH) { hU[0] = su - 1.6f; hV[0] = sv + 8.6f; hU[1] = su + 3.4f; hV[1] = sv + 8.4f; hdA = 0.3f; }
      if (pose == SCP_HOLD)  { hU[1] = su + 4.6f; hV[1] = sv - 3.8f; }
      break;
  }
  if (F.look != 0) hdA = F.look;
  // The spine's frame (x ahead of the chest, y up the spine from the hip) and
  // the head's (x the way the face looks, y out of the crown).
  float tuU = su - hu, tuV = sv - hv;
  const float SP = sqrtf(tuU * tuU + tuV * tuV);
  tuU /= SP; tuV /= SP;
  const float taU = tuV, taV = -tuU;
  auto TU = [&](float x, float y) { return hu + taU * x + tuU * y; };
  auto TV = [&](float x, float y) { return hv + taV * x + tuV * y; };
  const float hcU = su + tuU * (0.8f + HR) + taU * 0.5f, hcV = sv + tuV * (0.8f + HR) + taV * 0.5f;
  const float haU = cosf(hdA), haV = sinf(hdA);
  struct Frm { float ou, ov, au, av, pu, pv; };
  const Frm hf = { hcU, hcV, haU, haV, -haV, haU };
  auto FU = [](const Frm& f, float x, float y) { return f.ou + f.au * x + f.pu * y; };
  auto FV = [](const Frm& f, float x, float y) { return f.ov + f.av * x + f.pv * y; };
  auto FS = [&](const Frm& f, float x0, float y0, float x1, float y1, float w0, float w1) {
    S(FU(f, x0, y0), FV(f, x0, y0), FU(f, x1, y1), FV(f, x1, y1), w0, w1);
  };
  auto FD = [&](const Frm& f, float x, float y, float r) { D(FU(f, x, y), FV(f, x, y), r); };
  auto FP = [&](const Frm& f, const float* xs, const float* ys, int n) {
    float us[8], vs[8];
    for (int i = 0; i < n; i++) { us[i] = FU(f, xs[i], ys[i]); vs[i] = FV(f, xs[i], ys[i]); }
    Pg(us, vs, n);
  };
  auto FB = [&](const Frm& f, float x0, float y0, float x1, float y1) {
    float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    FP(f, xs, ys, 4);
  };
  const Frm tf = { hu, hv, taU, taV, tuU, tuV };

  // What each of them does with their hands, where it isn't just the pose.
  if (a == 0 && carry)              { hU[1] = TU(2.8f, SP - 2.8f); hV[1] = TV(2.8f, SP - 2.8f); }   // the rifle on the shoulder
  if (a == 0 && pose == SCP_STAND)  { hU[0] = hu - 1.9f; hV[0] = hv + 1.2f; }                        // and a fist on the hip
  if (a == 1 && carry)              { hU[1] = TU(7.0f, SP - 6.2f); hV[1] = TV(7.0f, SP - 6.2f);
                                      hU[0] = TU(5.0f, SP - 3.8f); hV[0] = TV(5.0f, SP - 3.8f); }   // round the crate
  if (a == 3 && carry)              { hU[1] = TU(1.8f, SP - 3.6f); hV[1] = TV(1.8f, SP - 3.6f);
                                      hU[0] = TU(1.0f, SP - 2.6f); hV[0] = TV(1.0f, SP - 2.6f); }   // thumbs in the straps
  if (a == 4 && pose == SCP_STAND)  { hU[1] = FU(hf, HR + 1.4f, -0.2f); hV[1] = FV(hf, HR + 1.4f, -0.2f); }   // the glasses up
  if (a == 5 && pose == SCP_STAND)  { hU[0] = hu - 1.6f; hV[0] = hv + 1.0f; }
  if (F.handV != 0)                 { hU[1] = F.handU; hV[1] = F.handV; }
  float kU[2], kV[2], eU[2], eV[2], rU[2], rV[2];
  for (int s = 0; s < 2; s++) {
    scIK(hu, hv, fU[s], fV[s], LT, LS, 1.0f, &kU[s], &kV[s]);
    rU[s] = TU(s ? 0.3f : -0.5f, SP - 0.9f); rV[s] = TV(s ? 0.3f : -0.5f, SP - 0.9f);
    scIK(rU[s], rV[s], hU[s], hV[s], UA, FA, -1.0f, &eU[s], &eV[s]);
  }
  if (pass == 1) { scHandX = PX(hU[1], hV[1]); scHandY = PY(hU[1], hV[1]); }
  const int bagHand = (walk || pose == SCP_STAND) ? 1 : (pose == SCP_HOLD ? 0 : -1);

  if (pass != 2) {
    // -- the coat, under everything: to the knee, open down the front, the tails taken by the wind --
    if (a == 0 || a == 2) {
      const float flap = a == 0 ? 1.0f : 0.5f, drop = a == 0 ? 8.6f : 7.0f;
      float tail = ((walk ? 1.5f : 0.4f) + gust * 2.2f) * flap;
      float lift = upright ? ((walk ? 0.9f : 0.3f) + flut * 0.5f) * flap : 0.0f;
      float us[6] = { TU(-2.2f, SP - 0.5f), TU(2.2f, SP - 0.7f), TU(2.5f, 0.2f), kU[1] + 0.9f,
                      TU(-3.8f - tail, -drop), TU(-2.9f - tail * 0.4f, -drop * 0.35f) };
      float vs[6] = { TV(-2.2f, SP - 0.5f), TV(2.2f, SP - 0.7f), TV(2.5f, 0.2f), fmaxf(0.5f, kV[1] - 1.4f),
                      fmaxf(0.5f, TV(-3.8f - tail, -drop) + lift), fmaxf(0.5f, TV(-2.9f - tail * 0.4f, -drop * 0.35f)) };
      Pg(us, vs, 6);
    }
    // -- legs and boots: a heel on every one of them --
    for (int s = 0; s < 2; s++) {
      S(hu, hv, kU[s], kV[s], bulky ? 3.4f : 3.0f, 2.5f);
      S(kU[s], kV[s], fU[s], fV[s], 2.5f, 1.9f);
      float bc = cosf(bA[s]), bs = sinf(bA[s]), pdu = bs, pdv = -bc;   // along the boot, and down through its sole
      S(fU[s] - bc * 0.8f + pdu * 0.3f, fV[s] - bs * 0.8f + pdv * 0.3f,
        fU[s] + bc * 2.7f + pdu * 0.5f, fV[s] + bs * 2.7f + pdv * 0.5f, bulky ? 2.6f : 2.2f, 1.2f);
      D(fU[s] - bc * 0.7f + pdu * 0.7f, fV[s] - bs * 0.7f + pdv * 0.7f, 0.85f);
    }
    // -- the body: a waist, a chest, a neck, a head that faces somewhere --
    const float waist = bulky ? 5.0f : (a == 1 ? 4.8f : 3.4f), chest = bulky ? 6.8f : (mule ? 6.0f : 5.8f);
    FS(tf, 0, 0, 0, SP - 0.9f, waist, chest);
    FD(tf, 0, SP - 1.0f, chest * 0.42f);
    D(hu, hv, waist * 0.52f);
    S(TU(0.3f, SP - 0.4f), TV(0.3f, SP - 0.4f), FU(hf, -0.2f, -HR * 0.7f), FV(hf, -0.2f, -HR * 0.7f), 2.1f, 1.8f);
    D(hcU, hcV, HR);
    FD(hf, 1.1f, -1.1f, 1.5f);                               // the jaw
    // -- arms --
    for (int s = 0; s < 2; s++) {
      S(rU[s], rV[s], eU[s], eV[s], bulky ? 2.8f : 2.4f, 2.1f);
      S(eU[s], eV[s], hU[s], hV[s], 2.1f, 1.7f);
      D(hU[s], hV[s], 1.05f);
    }

    // -- what each of them carries --
    if (a == 0) {                                            // THE GUIDE
      if (upright) {                                         // the bandanna's knot, flying
        float us[3] = { TU(-0.6f, SP + 0.4f), TU(-0.3f, SP - 1.0f), TU(-0.6f, SP - 0.3f) - 3.2f - gust * 1.8f };
        float vs[3] = { TV(-0.6f, SP + 0.4f), TV(-0.3f, SP - 1.0f), TV(-0.6f, SP - 0.3f) + flut * 0.5f };
        Pg(us, vs, 3);
      }
      if (carry) {                                           // the rifle, on the shoulder
        FS(tf, 3.9f, SP - 3.9f, 1.2f, SP - 1.3f, 2.3f, 1.7f);
        FS(tf, 1.2f, SP - 1.3f, -6.6f, SP + 6.2f, 1.25f, 0.95f);
      } else if (pose == SCP_LIE) {
        S(-4.0f, 0.7f, 6.0f, 0.7f, 1.2f, 1.0f);              // dropped beside
      } else {                                               // slung across the back
        FS(tf, -2.9f, 1.6f, -2.2f, 4.8f, 2.2f, 1.7f);
        FS(tf, -2.2f, 4.8f, -0.4f, SP + 5.2f, 1.2f, 0.95f);
      }
    } else if (a == 1) {                                     // THE QUARTERMASTER
      if (carry)                  FB(tf, 1.6f, SP - 8.6f, 9.2f, SP - 1.2f);   // the crate
      else if (pose != SCP_LIE)   WB(-9.8f, 0.0f, -3.8f, 5.8f);              // set down behind
    } else if (a == 2) {                                     // THE MEDIC: the bag, hung off a hand or set down
      if (bagHand >= 0) {
        float bu = hU[bagHand], bv = hV[bagHand];
        WB(bu - 2.1f, bv - 3.8f, bu + 2.3f, bv - 0.6f);
      } else if (pose != SCP_LIE) {
        WB(-8.0f, 0.0f, -3.4f, 3.6f);
      }
    } else if (a == 3) {                                     // THE MULE
      if (pose == SCP_SIT) {                                 // the pack, set down at last
        WB(-15.6f, 0.0f, -6.6f, 16.0f);
        S(-15.0f, 17.6f, -7.2f, 17.6f, 3.6f, 3.6f);
      } else if (pose == SCP_LIE) {                          // still wearing it
        WB(-6.0f, 3.0f, 4.0f, 9.0f);
        S(-5.4f, 10.4f, 3.4f, 10.4f, 3.2f, 3.2f);
      } else {
        const float kx[6] = { -1.6f, -9.2f, -10.4f, -10.2f, -8.4f, -1.8f };   // the pack, sagging where it's full
        const float ky[6] = { -1.2f, -0.6f, 3.0f, SP + 4.2f, SP + 6.6f, SP + 6.6f };
        FP(tf, kx, ky, 6);
        FS(tf, -9.4f, SP + 8.1f, -2.4f, SP + 8.3f, 3.9f, 3.9f);   // the bedroll on it
        FS(tf, -8.4f, SP + 4.6f, -5.6f, SP + 11.4f, 1.0f, 0.9f);  // a spade, blade to the sky
        const float bx[5] = { -6.7f, -4.4f, -3.2f, -3.5f, -5.9f }, by[5] = { SP + 10.6f, SP + 9.7f, SP + 13.2f, SP + 14.9f, SP + 14.4f };
        FP(tf, bx, by, 5);
        float sw = walk ? sinf(ph * 2.0f) * 0.5f : 0.0f;     // the skillet, swinging off its hook
        float pu = TU(-10.4f, 5.6f), pv = TV(-10.4f, 5.6f);
        S(pu, pv, pu + sinf(sw) * 2.0f, pv - cosf(sw) * 2.0f, 0.9f, 0.9f);
        D(pu + sinf(sw) * 3.8f, pv - cosf(sw) * 3.8f, 2.2f);
        float lu = TU(-2.8f, -1.6f) + sinf(sw) * 1.2f, lv = TV(-2.8f, -1.6f) - 1.9f;   // the lantern
        S(TU(-2.8f, -1.6f), TV(-2.8f, -1.6f), lu, lv + 0.8f, 0.7f, 0.7f);
        WB(lu - 0.9f, lv - 1.1f, lu + 0.9f, lv + 1.0f);
      }
    } else if (a == 4) {                                     // THE SCOUT
      if (pose != SCP_LIE) {
        FB(tf, -3.9f, 2.2f, -1.2f, SP - 0.9f);               // the set, on the back
        FS(tf, -3.2f, SP - 1.2f, -6.8f, SP + 16.4f, 1.1f, 0.7f);   // its whip
      }
      float bu = TU(-0.4f, SP + 0.4f), bv = TV(-0.4f, SP + 0.4f), pu = bu, pv = bv;
      for (int i = 1; i <= 6; i++) {                         // the scarf
        float nu, nv;
        if (upright) {
          nu = bu - i * 2.3f * (0.75f + 0.5f * gust);
          nv = bv - i * 0.4f + sinf(scFigT * 0.017f + F.x * 0.1f - i * 1.3f) * 0.3f * i;
        } else {
          nu = bu - i * 0.7f; nv = fmaxf(0.6f, bv - i * 1.4f);
        }
        S(pu, pv, nu, nv, 3.2f - i * 0.25f, 3.0f - i * 0.25f);
        pu = nu; pv = nv;
      }
    } else {                                                 // THE ENDURER
      // The poncho: from the hood to the hips, the back of it on the wind.
      float tail = ((walk ? 1.0f : 0.3f) + gust * 1.6f), lift = upright ? flut * 0.4f : 0.0f;
      const float px[7] = { -1.6f, 1.4f, 3.8f, 4.4f, 0.6f, -4.8f - tail, -3.6f };
      const float py[7] = { SP + 0.8f, SP + 0.6f, SP - 1.2f, SP - 6.6f, SP - 7.6f, SP - 6.6f + lift, SP - 1.0f };
      FP(tf, px, py, 7);
      float pu = TU(-1.4f, SP - 0.9f), pv = TV(-1.4f, SP - 0.9f);   // spikes, bolted through it at the shoulder
      for (int i = 0; i < 3; i++) {
        float an = 2.0f + i * 0.45f, cu = cosf(an), cv = sinf(an);
        float du = taU * cu + tuU * cv, dv = taV * cu + tuV * cv, nu = -dv, nv = du;
        float ln = i == 1 ? 6.4f : 5.4f;
        float us[3] = { pu + du * 1.8f + nu * 0.8f, pu + du * 1.8f - nu * 0.8f, pu + du * ln };
        float vs[3] = { pv + dv * 1.8f + nv * 0.8f, pv + dv * 1.8f - nv * 0.8f, pv + dv * ln };
        Pg(us, vs, 3);
      }
      if (carry) {                                           // the sawn-off
        S(hU[1] - 1.1f, hV[1] + 0.5f, hU[1] + 0.2f, hV[1] - 0.2f, 2.0f, 1.8f);
        S(hU[1], hV[1], hU[1] + 3.4f, hV[1] - 2.3f, 1.5f, 1.3f);
      }
    }

    // -- what they wear on their heads --
    if (a == 0) {
      // The Guide's hat. Lying down it has come off, and sits over the face.
      Frm hat = hf;
      if (pose == SCP_LIE) { hat.ou = FU(hf, HR - 0.4f, 0); hat.ov = FV(hf, HR - 0.4f, 0); hat.au = -hf.pu; hat.av = -hf.pv; hat.pu = hf.au; hat.pv = hf.av; }
      const float cx[5] = { -2.4f, -2.0f, 0.0f, 2.2f, 2.6f }, cy[5] = { 1.5f, 4.5f, 4.0f, 4.4f, 1.3f };
      FP(hat, cx, cy, 5);
      FS(hat, -5.0f, 1.5f, 5.3f, 1.1f, 1.3f, 1.0f);           // the brim
      FS(hat, -5.0f, 1.5f, -5.7f, 2.3f, 1.0f, 0.8f);          // and its curl
      FS(hat, 5.3f, 1.1f, 5.9f, 1.8f, 1.0f, 0.8f);
    } else if (a == 1) {                                     // a derby
      FD(hf, -0.1f, 2.3f, 2.65f);
      FS(hf, -3.7f, 1.4f, 3.9f, 1.3f, 1.1f, 1.0f);
      FS(hf, -3.7f, 1.4f, -4.1f, 2.0f, 0.9f, 0.8f);
      FS(hf, 3.9f, 1.3f, 4.3f, 1.9f, 0.9f, 0.8f);
    } else if (a == 2) {                                     // the parson's hat, and a bandanna for a mask
      FS(hf, -5.1f, 1.4f, 5.2f, 1.4f, 1.1f, 1.0f);
      FB(hf, -2.0f, 1.4f, 2.1f, 3.3f);
      const float mx[3] = { -0.4f, 3.3f, 1.3f }, my[3] = { 0.0f, -0.5f, -4.0f };
      FP(hf, mx, my, 3);
    } else if (a == 3) {                                     // a slouch hat, gone soft years ago
      FS(hf, -4.2f, 0.9f, 0.2f, 1.7f, 1.3f, 1.2f);
      FS(hf, 0.2f, 1.7f, 4.6f, 0.5f, 1.2f, 1.0f);
      FD(hf, -0.2f, 2.2f, 2.2f);
    } else if (a == 4) {                                     // flying cap, the flap, the goggles up
      FD(hf, -0.3f, 0.5f, HR + 0.45f);
      FS(hf, -0.9f, -0.3f, -1.1f, -2.6f, 1.2f, 1.0f);
      FS(hf, 0.6f, 1.7f, 2.9f, 1.6f, 1.5f, 1.3f);
      if (a == 4 && pose == SCP_STAND) FB(hf, HR - 0.1f, -0.8f, HR + 2.6f, 0.9f);   // field glasses
    } else {                                                 // the crest, and the mask
      for (int i = 0; i < 4; i++) {                          // a mohawk, cut with a knife
        float a0 = 1.2f + i * 0.42f, a1 = a0 + 0.42f, r = HR + 0.5f;
        FS(hf, cosf(a0) * r, sinf(a0) * r, cosf(a1) * r, sinf(a1) * r, 1.6f, 1.5f);
        float am = a0 + 0.21f, tr = HR + 2.0f + (i == 1 ? 0.5f : 0.0f);
        const float tx[3] = { cosf(a0) * r, cosf(a1) * r, cosf(am - 0.1f) * tr }, ty[3] = { sinf(a0) * r, sinf(a1) * r, sinf(am - 0.1f) * tr };
        FP(hf, tx, ty, 3);
      }
      FS(hf, 0.8f, -0.6f, 3.3f, -1.9f, 3.1f, 2.5f);          // the snout
      FD(hf, 3.6f, -2.3f, 1.55f);                            // its filter
      S(FU(hf, 2.6f, -2.6f), FV(hf, 2.6f, -2.6f), TU(2.9f, SP - 3.4f), TV(2.9f, SP - 3.4f), 1.0f, 1.0f);   // and the hose
    }
    return;
  }

  // -- pass 2: what catches the light --
  if (a == 0) {
    if (pose != SCP_LIE) {
      for (int s = 0; s < 2; s++) {                          // the spurs
        float bc = cosf(bA[s]), bs = sinf(bA[s]);
        Px(fU[s] - bc * 1.5f, fV[s] - bs * 1.5f - 0.2f, FXP_GLOW);
      }
      float eu = FU(hf, HR + 0.9f, -1.0f), ev = FV(hf, HR + 0.9f, -1.0f);   // the cheroot, and its smoke
      Px(eu, ev, ((scFigT / 230 + F.seat) % 5) ? FXP_HOT : FXP_GLOW);
      for (int i = 1; i <= 3; i++) {
        if (((scFigT / 180) + i) % 4 == 0) continue;
        Px(eu - i * (0.9f + gust * 0.8f), ev + i * 1.3f + sinf(scFigT * 0.006f + i) * 0.4f, i < 3 ? FXP_LINE : FXP_TRACK);
      }
    }
  } else if (a == 1) {
    if (pose != SCP_LIE) Px(FU(hf, 1.5f, 0.3f), FV(hf, 1.5f, 0.3f), FXP_WHITE);   // the spectacles
    if (carry) {                                             // the crate's planks, and its stencil
      fxLine((int)PX(TU(1.6f, SP - 5.6f), TV(1.6f, SP - 5.6f)), (int)PY(TU(1.6f, SP - 5.6f), TV(1.6f, SP - 5.6f)),
             (int)PX(TU(9.2f, SP - 5.6f), TV(9.2f, SP - 5.6f)), (int)PY(TU(9.2f, SP - 5.6f), TV(9.2f, SP - 5.6f)), FX_PAL[FXP_RUST]);
      int sx = (int)floorf(PX(TU(5.4f, SP - 3.0f), TV(5.4f, SP - 3.0f))), sy = (int)floorf(PY(TU(5.4f, SP - 3.0f), TV(5.4f, SP - 3.0f)));
      fxLine(sx - 1, sy - 1, sx + 1, sy + 1, FX_PAL[FXP_BRICK]);
      fxLine(sx - 1, sy + 1, sx + 1, sy - 1, FX_PAL[FXP_BRICK]);
    }
  } else if (a == 2) {                                       // the cross on the bag
    float bu, bv;
    if (bagHand >= 0) { bu = hU[bagHand] + 0.1f; bv = hV[bagHand] - 2.2f; }
    else if (pose != SCP_LIE) { bu = -5.7f; bv = 1.8f; }
    else bu = bv = -99;
    if (bv > -50) {
      int x = (int)floorf(PX(bu, bv)), y = (int)floorf(PY(bu, bv));
      uint16_t w = FX_PAL[FXP_WHITE];
      scPx(x, y, w); scPx(x - 1, y, w); scPx(x + 1, y, w); scPx(x, y - 1, w); scPx(x, y + 1, w);
    }
  } else if (a == 3) {
    if (pose != SCP_SIT && pose != SCP_LIE) {                // the lantern's glass
      float sw = walk ? sinf(ph * 2.0f) * 0.5f : 0.0f;
      float lu = TU(-2.8f, -1.6f) + sinf(sw) * 1.2f, lv = TV(-2.8f, -1.6f) - 1.9f;
      Px(lu, lv, ((scFigT / 150) % 7) ? FXP_HOT : FXP_GLOW);
    }
  } else if (a == 4) {
    if (pose != SCP_LIE) {
      Px(FU(hf, 1.4f, 1.7f), FV(hf, 1.4f, 1.7f), FXP_GLOW);  // goggles
      Px(FU(hf, 2.4f, 1.6f), FV(hf, 2.4f, 1.6f), FXP_GLOW);
      if (pose == SCP_STAND) Px(FU(hf, HR + 2.5f, 0.1f), FV(hf, HR + 2.5f, 0.1f), FXP_WHITE);   // a lens
      if ((scFigT / 420) & 1) Px(TU(-2.5f, SP - 2.2f), TV(-2.5f, SP - 2.2f), FXP_CRIT);        // the set's lamp
      // the pennant on the whip, streaming
      float wu = TU(-6.8f, SP + 16.4f), wv = TV(-6.8f, SP + 16.4f), bu0 = TU(-6.0f, SP + 12.8f), bv0 = TV(-6.0f, SP + 12.8f);
      float fl = fxNoise1(scFigT * 0.004f + F.x * 0.1f, 3) * 2.0f;
      fxTri(PX(wu, wv), PY(wu, wv), PX(bu0, bv0), PY(bu0, bv0),
            PX(wu - 5.0f - fl, wv - 1.6f + fl * 0.3f), PY(wu - 5.0f - fl, wv - 1.6f + fl * 0.3f), FX_PAL[FXP_HOT]);
    }
  } else {
    if (pose != SCP_LIE) {                                   // the mask's eye: amber by day, red when the fire's up
      uint8_t ec = fire || F.rim < FXP_BRICK ? FXP_CRIT : FXP_GLOW;
      Px(FU(hf, 1.4f, 0.5f), FV(hf, 1.4f, 0.5f), ec);
      Px(FU(hf, 1.9f, 0.5f), FV(hf, 1.9f, 0.5f), ec);
    }
    for (int i = 0; i < 4; i++)                              // a chain, looped at the hem
      Px(TU(-3.0f + i * 1.7f, SP - 6.2f - (i & 1) * 0.6f), TV(-3.0f + i * 1.7f, SP - 6.2f - (i & 1) * 0.6f), FXP_RUST);
  }
  if (pose != SCP_LIE && F.seat < SC_SEATS) {                // whose they are
    static const float HAT[6] = { 4.5f, 4.9f, 3.3f, 3.9f, 3.5f, 4.6f };
    float top = FV(hf, 0, HAT[a]);
    if (mule && pose != SCP_SIT) top = fmaxf(top, TV(-6.0f, SP + 10.0f));
    top = fmaxf(top, fmaxf(hV[0], hV[1]) + 1.0f);
    char n[2] = { (char)('1' + F.seat), 0 };
    scType(n, (int)PX(hcU, 0) - 2, (int)PY(0, top) - 9, 1, FXP_HOT);
  }
}
static void scFigure(const ScFig& F, int pass) {
  if (pass == 0 && F.shadow != 0) scFigureDraw(F, 3);
  scFigureDraw(F, pass);
}

// A hatch in the road with someone gone down it, and their note to the rest.
static void scHatch(float x, int seat) {
  float y = (float)RD_FOOT;
  fxBlob(x, y - 1, 1, 0, 8.5f, 2.6f, 11, FX_PAL[FXP_INK]);
  fxBlob(x + 7, y - 6, 0.5f, -0.87f, 7.5f, 1.6f, 12, FX_PAL[FXP_LINE]);   // the lid, propped
  fxLine((int)x - 3, (int)y - 1, (int)x - 3, (int)y - 7, FX_PAL[FXP_RUST]);
  fxLine((int)x + 3, (int)y - 1, (int)x + 3, (int)y - 7, FX_PAL[FXP_RUST]);
  fxLine((int)x - 3, (int)y - 4, (int)x + 3, (int)y - 4, FX_PAL[FXP_RUST]);
  char n[2] = { (char)('1' + seat), 0 };
  scType(n, (int)x - 2, (int)y - 18, 1, FXP_HOT);
  scBalloon("BRB", (int)x + 14, (int)y - 20, (int)x + 5, (int)y - 9);
}

// The camp fire: tongues that lick on noise, a pool of light on the dirt, the
// logs, and sparks going up to join the stars. The glow is wide on purpose:
// the ring round the fire has to sit black against it, or at night they are
// ink on ink and only their rims show, like a drawing nobody filled in.
static void scFireGlow(float x, float y, uint32_t now) {
  float fl = 0.85f + 0.15f * fxNoise1(now * 0.012f, 8);
  for (int yy = (int)y - 54; yy < RD_Y1; yy++) {
    if (yy < RD_Y0) continue;
    float dy = (yy - y) / (yy < y ? 50.0f : 9.0f);
    for (int xx = (int)x - 88; xx < (int)x + 88; xx++) {
      if ((unsigned)xx >= (unsigned)FX_W) continue;
      float dx = (xx - x) / 88.0f, r = dx * dx + dy * dy;
      if (r >= 1) continue;
      float k = (1 - r) * fl;
      uint16_t& p = fxDst[yy * FX_W + xx];
      int lum = fxLum(p) >> 4;
      int lv = fxMinI(12, lum + (int)(k * 6.0f));
      if (scDot(xx, yy, k * 1.2f)) lv = fxMinI(13, lv + 1);
      if (lv > lum) p = FX_PAL[lv];
    }
  }
}
static void scFire(float x, float y, uint32_t now, float k = 1.0f) {
  fxStroke(x - 9 * k, y + 1, x + 8 * k, y - 2 * k, 3.0f * k, 2.6f * k, FX_PAL[FXP_INK]);
  fxStroke(x - 8 * k, y - 2 * k, x + 9 * k, y + 1, 3.0f * k, 2.6f * k, FX_PAL[FXP_INK]);
  fxLine((int)(x - 8 * k), (int)(y - 3 * k), (int)(x + 7 * k), (int)(y - 4 * k), FX_PAL[FXP_RUST]);
  float t = now * 0.009f;
  static const uint8_t COL[3] = { FXP_FLAME, FXP_HOT, FXP_GLOW };
  for (int layer = 0; layer < 3; layer++) {
    float sc = (1.0f - layer * 0.3f) * k;
    for (int i = 0; i < 5; i++) {
      float u = (i - 2) / 2.0f;
      float h = (9 + 9 * fxNoise1(t + i * 1.7f + layer, 41)) * sc * (1.0f - fabsf(u) * 0.35f);
      float sway = (fxNoise1(t * 1.3f + i, 43) - 0.5f) * 6.0f;
      float bx = x + u * 6.0f * sc;   // sc carries k
      fxTri(bx - 3.2f * sc, y - 1, bx + 3.2f * sc, y - 1, bx + sway, y - 1 - h, FX_PAL[COL[layer]]);
    }
  }
  for (int i = 0; i < 9; i++) {
    uint32_t h = fxHash(i * 7 + 1);
    float ph = fmodf(now * (0.0006f + 0.0004f * fxU(h)) + fxU(h >> 4), 1.0f);
    float ex = x + (fxNoise1(ph * 3 + i, 47) - 0.5f) * 26 * k, ey = y - 8 * k - ph * 60 * k;
    scPx((int)ex, (int)ey, FX_PAL[ph < 0.4f ? FXP_GLOW : (ph < 0.75f ? FXP_FLAME : FXP_RUST)]);
  }
}

// A tumbleweed, for when there is nobody on the road at all.
static void scTumbleweed(uint32_t now) {
  float p = fmodf(now * 0.045f, 380.0f) - 60.0f;
  float bounce = fabsf(sinf(p * 0.07f)) * 10.0f;
  float x = p, y = RD_FOOT - 10 - bounce, rot = p * 0.09f;
  fxBlob(x - 2, RD_FOOT + 1.0f, 1, 0, 9.0f - bounce * 0.4f, 1.6f, 5, FX_PAL[FXP_SOOT]);   // its shadow keeps up
  FxRng R; R.s = 77;
  for (int i = 0; i < 34; i++) {
    float a0 = rot + R.u() * 6.28f, a1 = a0 + 1.0f + R.u() * 2.2f, r0 = 3 + R.u() * 6.5f, r1 = 4 + R.u() * 5.5f;
    fxLine((int)(x + cosf(a0) * r0), (int)(y + sinf(a0) * r0), (int)(x + cosf(a1) * r1), (int)(y + sinf(a1) * r1),
           FX_PAL[i % 3 == 0 ? FXP_BRICK : (i & 1 ? FXP_RUST : FXP_EMBER)]);
  }
}

// ── 5. What the wasteland has to say about it ────────────────────────────────
// The narration box. Lines are '|'-split, at most 30 characters each. The
// voice is the one off the back of a paperback western found in a burnt-out
// car: laconic, a little grim, and it has seen this all before -- the law is
// gone, the horses were eaten, the gasoline is a rumour, and nobody has told
// the road.
#define SC_NN(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const char* const SC_NAR_CALM[] = {
  "MEANWHILE, OUT PAST THE|LAST OF THE LAW...",
  "THE PARTY RIDES OUT.|ON FOOT. WE ATE THE HORSES.",
  "NO LAW WEST OF THE CRATER.|NO WEST, EITHER. JUST DUST.",
  "DUST IN THE BOOTS.|DUST IN THE BEANS.",
  "THE ROAD WAS HERE FIRST.|IT PLANS TO BE HERE LAST.",
  "THE GASOLINE IS GONE.|THE GRUDGES ARE NOT.",
  "EVERY MILE OF THIS ROAD|HAS A NAME. AND A GRAVE.",
  "NOBODY HAS DIED IN|SEVERAL WHOLE MINUTES.",
  "SOMEWHERE, A SETTLEMENT.|PROBABLY.",
};
static const char* const SC_NAR_NIGHT[] = {
  "NIGHT. THE FIRE IS ON|OUR SIDE. MOSTLY.",
  "CAMP. SOMEBODY HAS TO|TAKE FIRST WATCH.",
  "THE DARK MAKES NOISES.|WE MAKE THEM BACK.",
  "BEANS AGAIN. NOBODY ASKS|WHAT KIND OF BEANS.",
  "SOMEBODY TELLS THE ONE|ABOUT THE LAST SHERIFF.",
  "COYOTES. OR WORSE.|PROBABLY WORSE.",
};
static const char* const SC_NAR_DUSK[] = { "THE SUN CLOCKS OFF.|THE WASTE CLOCKS ON.",
                                           "SUNDOWN. EVERYTHING WITH|TEETH GETS UP FOR WORK." };
static const char* const SC_NAR_DAWN[] = { "ANOTHER DAWN. NOBODY IS|MORE SURPRISED THAN US.",
                                           "SUN'S UP. SHAKE OUT YOUR|BOOTS FOR SCORPIONS." };
static const char* const SC_NAR_WX[6][2] = {
  { nullptr, nullptr },
  { "RAIN. FREE WATER, FOR|ONCE. PROBABLY.", "EVERYTHING IS WET.|EVEN THE DRY STUFF." },
  { "THE SKY HAS OPINIONS|TODAY. LOUD ONES.", "LIGHTNING: THE SKY'S|WAY OF COUNTING US." },
  { "RAIN: NOW WITH ADDED|CHEMISTRY.", "DO NOT DRINK|THE WEATHER." },
  { "THE AIR HAS TEXTURE.|DO NOT CHEW IT.", "SMOG. BREATHE LESS." },
  { "VISIBILITY: YOUR OWN|FEET, ON A GOOD DAY.", "FOG. SOMETHING IN IT|IS ALSO LOST." },
};
static const char* const SC_NAR_DOOM[]   = { "SOMETHING ON THE HORIZON|HAS TAKEN AN INTEREST.",
                                             "DON'T LOOK BACK.|IT'S STILL THERE.",
                                             "IT RIDES BEHIND US. IT|DOESN'T NEED A HORSE." };
static const char* const SC_NAR_HUNT[]   = { "IT HAS STOPPED|PRETENDING. RUN.",
                                             "THIS WASTE AIN'T BIG|ENOUGH FOR THE BOTH OF US." };
static const char* const SC_NAR_CLOCK[]  = { "THE CLOCK SAYS LATE.|THE CLOCK IS NEVER WRONG.",
                                             "CLOSE TO MIDNIGHT. SOMEONE|WILL HAVE TO DRAW FIRST." };
static const char* const SC_NAR_HUNGRY[] = { "RATIONS: A FOND|MEMORY.",
                                             "WE COULD EAT A HORSE.|WE DID. THAT WAS MONDAY." };
static const char* const SC_NAR_DRY[]    = { "WATER: THEORETICAL.",
                                             "THE CANTEEN RATTLES.|CANTEENS SHOULDN'T RATTLE." };
static const char* const SC_NAR_DOWN[]   = { "SOMEONE IS HORIZONTAL.|WE DRAG. IT'S TRADITION.",
                                             "NOBODY GETS LEFT FOR|THE BUZZARDS. NOT TODAY." };
static const char* const SC_NAR_BELOW[]  = { "SOMEONE WENT BELOW.|THE HOLE SAYS NOTHING.",
                                             "DOWN THE OLD MINE. THE|CANARY DIDN'T COME BACK." };
static const char* const SC_NAR_EMPTY[]  = { "THE ROAD IS EMPTY.|THE ROAD CAN WAIT.",
                                             "NOBODY OUT HERE. JUST|THE WIND, PRACTISING.",
                                             "THE TUMBLEWEED HAS THE|ROAD TO ITSELF. EARNED IT." };

// Per survivor, the one line the wasteland would say about them right now.
static const char* const SC_QUIP_ARCH[6][2] = {
  { "knows a shortcut. allegedly.",    "smoking the last cheroot on earth." },
  { "counting. always counting.",      "has a list. you're on it." },
  { "sawbones. emphasis on the saw.",  "bedside manner: a stick." },
  { "carries everything. complains never.", "the pack has a pack. and a skillet." },
  { "saw something. won't say what.",  "first in. first out. usually." },
  { "fine. it's fine. it's all fine.", "the mask stays on. don't ask." },
};
static const char* scQuip(const SceneSurv& v, uint32_t now) {
  uint32_t alt = (now / 17000 + v.arch) & 1;
  if (v.depth)              return alt ? "gone below. back soon. probably." : "down a hole. by choice.";
  if (v.ll == 0)            return alt ? "horizontal. being dragged." : "having a long lie-down.";
  if (v.inEnc)              return alt ? "poking something. it pokes back." : "busy. do not disturb the loot.";
  if (v.ll <= 2)            return alt ? "held together by tape and spite." : "leaking. slightly.";
  if (v.water <= 1)         return alt ? "drinking own optimism." : "dry as the jokes.";
  if (v.food <= 1)          return v.arch == 3 ? "eyeing the pack straps. hungrily." : "eyeing the Mule. the Mule knows.";
  if (v.rad >= 7)           return alt ? "glows. saves on torches." : "can read in the dark. by self.";
  if (v.rad >= 4)           return "faintly luminous.";
  if (v.ap == 0)         return "out of legs for today.";
  return SC_QUIP_ARCH[v.arch < 6 ? v.arch : 0][alt];
}

// ── 6. Screen 1: THE ROAD ────────────────────────────────────────────────────
// The party on the road. The sky is the clock -- dawn, a high white day,
// golden hour, the night they sit it out round a fire -- and the weather, and
// the Creeping Doom rises on the skyline as it gets closer. Under the panel
// is the party's ledger: hearts for life, a tin for food, a drop for water,
// the trefoil for rads, and one line on how each of them is getting on.
//
//   y   0-19   masthead      DAY 12  weather         the doomsday clock
//   y  20-169  the panel     sky, ruins, wreckage, the road, the party
//   y 172-296  the ledger    5 rows x 24: name, hearts, tin, drop, trefoil, quip
//   y 298-319  footer        the join address, the LAN address
static const char* const SC_WX_NAME[6] = { "CLEAR", "RAIN", "STORM", "CHEM RAIN", "SMOG", "FOG" };

static struct { uint32_t last, walked; const char* nar; uint32_t narT0; } scRoadSt = { 0, 0, nullptr, 0 };

// The threat clock, as a doomsday clock: a quarter to midnight when nothing is
// hunting, closing on twelve as the clock climbs. The time beside it is the
// stat; you do not need the number to know it is late.
static void scDoomsday(int cx, int cy, uint8_t w, uint32_t now) {
  int mins = 15 - (w * 15 + 127) / 255;                     // minutes to midnight
  float frac = mins / 60.0f;
  float a = -1.5708f - frac * FX_TAU;
  bool late = mins <= 3;
  fxDisc((float)cx, (float)cy, 8.6f, FX_PAL[FXP_LINE]);
  fxDisc((float)cx, (float)cy, 7.4f, FX_PAL[FXP_INK]);
  // the last quarter of the face, the part that matters, shaded in
  for (int y = cy - 8; y <= cy; y++)
    for (int x = cx - 8; x <= cx; x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      if (dx * dx + dy * dy < 44.0f && scDot(x, y, 0.45f)) scPx(x, y, FX_PAL[FXP_BLOOD]);
    }
  scPx(cx, cy - 7, FX_PAL[FXP_GLOW]); scPx(cx, cy - 6, FX_PAL[FXP_GLOW]);
  uint8_t hc = late && ((now / 350) & 1) ? FXP_WHITE : FXP_CRIT;
  fxStroke((float)cx, (float)cy, cx + cosf(a) * 6.4f, cy + sinf(a) * 6.4f, 1.6f, 1.0f, FX_PAL[hc]);
  fxStroke((float)cx, (float)cy, cx - 3.6f, (float)cy, 1.8f, 1.4f, FX_PAL[FXP_HDR]);   // the hour hand, on 11
  char t[8];
  snprintf(t, sizeof(t), "11:%02d", 60 - mins);
  scType(t, cx - 12 - scTypeW(t, 1), cy - 3, 1, late ? FXP_CRIT : FXP_HDR);
}

static void scRoadDraw(const SceneSnap& S, uint32_t now) {
  uint32_t dt = scRoadSt.last ? now - scRoadSt.last : 0;
  if (dt > 500) dt = 500;
  scRoadSt.last = now;
  const float t = S.dayMil / 1000.0f;
  const bool night = t >= 0.80f && t < 0.965f, dusk = t >= 0.66f && t < 0.80f;
  const uint8_t wx = S.weather < 6 ? S.weather : 0;

  int seated[SC_SEATS], nOn = 0, nWalk = 0;
  for (int i = 0; i < SC_SEATS; i++)
    if (S.s[i].on) {
      seated[nOn++] = i;
      if (!S.s[i].depth && S.s[i].ll) nWalk++;
    }
  const bool walking = nWalk > 0 && !night;
  if (walking) scRoadSt.walked += dt;
  const float scroll = (float)(scRoadSt.walked % 4000000u) * 0.028f;

  // -- the sky --
  float top, hor;
  scSkyAt(t, &top, &hor);
  if (wx == SC_WX_RAIN)  { top -= 1.4f; hor -= 2.5f; }
  if (wx == SC_WX_STORM) { top -= 2.4f; hor -= 4.0f; }
  if (wx == SC_WX_CHEM)  { top -= 1.2f; hor -= 1.5f; }
  if (wx == SC_WX_SMOG || wx == SC_WX_FOG) { float m = (top + hor) * 0.5f; top = fxLerp(top, m, 0.7f); hor = fxLerp(hor, m, 0.7f); }
  uint32_t boltSeed = 0;
  bool bolt = wx == SC_WX_STORM && scBoltNow(now, &boltSeed);
  if (bolt) { top += 5; hor += 3; }
  for (int y = RD_Y0; y < RD_HOR; y++) {
    float u = (float)(y - RD_Y0) / (RD_HOR - RD_Y0);
    scTone(y, 0, FX_W, fxLerp(top, hor, u * u * 0.8f + u * 0.2f));
  }
  // stars, when there is a night to see them through
  if (top < 1.2f && wx != SC_WX_STORM && wx != SC_WX_SMOG && wx != SC_WX_FOG)
    for (int i = 0; i < 46; i++) {
      uint32_t h = fxHash2((uint32_t)i, 0x57A2u);
      int x = (int)(h % FX_W), y = RD_Y0 + 2 + (int)((h >> 9) % 70);
      uint32_t tw = fxHash2((uint32_t)i, now / 450);
      if ((tw & 7) == 0) continue;
      scPx(x, y, FX_PAL[(h >> 20) % 5 == 0 ? FXP_GLOW : ((tw & 3) ? FXP_LINE : FXP_RUST)]);
    }
  // cloud, for weather that has some
  if (wx == SC_WX_RAIN || wx == SC_WX_STORM || wx == SC_WX_CHEM) {
    float cv = wx == SC_WX_STORM ? 0.9f : 0.65f, ct = now * 0.00002f;
    for (int y = RD_Y0; y < RD_Y0 + 46; y++)
      for (int x = 0; x < FX_W; x++) {
        float n = fxNoise2(x * 0.035f + ct * 20, y * 0.09f, 233) * 0.7f + fxNoise2(x * 0.1f + ct * 35, y * 0.2f, 239) * 0.3f;
        float fall = 1.0f - (y - RD_Y0) / 46.0f;
        float d = fxClampF((n - 0.52f + fall * 0.35f) * 3.0f, 0, 1) * cv;
        if (scDot(x, y, d)) fxDst[y * FX_W + x] = FX_PAL[bolt ? FXP_RUST : (d > 0.7f ? FXP_SOOT : FXP_BAND)];
      }
  }
  // the sun, or the moon, on their way over
  float sunX = -100, sunY = 0;
  if (t < 0.745f && wx != SC_WX_STORM) {
    float u = t / 0.745f;
    sunX = 12 + u * 216;
    sunY = RD_HOR - 4 - 4.0f * u * (1 - u) * 90.0f;
    float low = 1.0f - fxClampF(4.0f * u * (1 - u) * 1.6f, 0, 1);
    float haze = (wx == SC_WX_CLEAR) ? 1.0f : 0.5f;
    for (int y = (int)sunY - 20; y < (int)sunY + 20; y++)
      for (int x = (int)sunX - 20; x < (int)sunX + 20; x++) {
        float dx = x + 0.5f - sunX, dy = y + 0.5f - sunY, r = sqrtf(dx * dx + dy * dy);
        if (r > 19 || y < RD_Y0 || y >= RD_HOR) continue;
        if (scDot(x, y, (1 - r / 19) * 0.8f * haze)) scPx(x, y, FX_PAL[FXP_HOT]);
      }
    fxDisc(sunX, sunY, 8.5f + low * 3, FX_PAL[low > 0.5f ? FXP_HOT : FXP_GLOW]);
    if (haze > 0.9f) fxDisc(sunX - 1, sunY - 1, 5.0f + low * 2, FX_PAL[FXP_WHITE]);
    // the haze streaks it through, low down
    if (low > 0.4f)
      for (int k = 0; k < 3; k++) {
        int yy = (int)sunY - 4 + k * 5;
        fxRect((int)sunX - 22 + k * 4, yy, (int)sunX + 18 - k * 3, yy + 1, FX_PAL[(int)fxClampF(hor, 0, 14)]);
      }
  } else if (t >= 0.77f && wx != SC_WX_STORM && wx != SC_WX_SMOG) {
    float u = (t - 0.77f) / 0.23f, mx = 30 + u * 180, my = RD_HOR - 20 - 4.0f * u * (1 - u) * 70.0f;
    for (int y = (int)my - 8; y <= (int)my + 8; y++)
      for (int x = (int)mx - 8; x <= (int)mx + 8; x++) {
        float ax = x + 0.5f - mx, ay = y + 0.5f - my, bx = ax - 3.2f, by = ay + 1.2f;
        if (ax * ax + ay * ay < 42 && bx * bx + by * by > 36) scPx(x, y, FX_PAL[FXP_GLOW]);
      }
  }
  if (bolt) scBolt(boltSeed);

  // -- the skyline, the Doom, the ground --
  const bool lit = night || (dusk && t > 0.74f);
  scRuins(scroll * 0.10f, RD_HOR - 2, fxClampF(hor - 2.4f, 1.2f, 9.0f), lit, now, 0x5C1A7Eu, 15, 8, 36);
  scRuins(scroll * 0.24f, RD_HOR + 3, fxClampF(hor - 4.8f, 0.5f, 6.0f), lit, now, 0x77E1D0u, 24, 16, 58);
  if (S.doomClose) scDoom(S.doomClose, S.doomAware, now);
  float gTop = fxClampF(hor - 5.0f, 1.0f, 6.0f);
  for (int y = RD_HOR; y < RD_Y1; y++) {
    float u = (float)(y - RD_HOR) / (RD_Y1 - RD_HOR);
    scTone(y, 0, FX_W, fxLerp(gTop, night ? 0.4f : 1.2f, u));
  }
  // Never darker than rust: in a storm the sky goes nearly black, and a party
  // inked against it with no light on them is not there at all.
  uint8_t rimLv = (uint8_t)fxClampF(hor - 1.0f, 7.0f, 11.0f);
  scWreck(scroll * 0.5f, rimLv, night);
  // the road: a paler strip with its centre line still stubbornly painted
  for (int y = RD_FOOT - 8; y < RD_FOOT + 6; y++) {
    float u = (float)(y - (RD_FOOT - 8)) / 14.0f;
    scTone(y, 0, FX_W, fxLerp(gTop, night ? 0.4f : 1.2f, 0.5f) + 0.9f - fabsf(u - 0.5f) * 1.2f);
  }
  {
    int off = (int)fmodf(scroll, 26.0f);
    for (int x = -off; x < FX_W; x += 26) {
      uint32_t h = fxHash2((uint32_t)((x + (int)scroll) / 26), 5);
      if ((h & 7) == 0) continue;                              // worn away
      fxRect(x, RD_FOOT - 1, x + 10, RD_FOOT, FX_PAL[night ? FXP_TRACK : FXP_LINE]);
    }
    for (int i = 0; i < 30; i++) {                             // grit, rushing by
      uint32_t h = fxHash2((uint32_t)i, 0x6A1u);
      float x = fmodf(fxU(h) * 400.0f - scroll, 400.0f);
      if (x < 0) x += 400;
      int y = RD_Y1 - 1 - (int)((h >> 9) % 9);
      scPx((int)x - 80, y, FX_PAL[FXP_TRACK]);
    }
  }

  // Fog lies between the ruins and the road: it swallows the city, and the
  // party walks in front of it, which is the only reason they can be seen.
  if (wx == SC_WX_FOG)  scFog(now, 5.0f, 0.9f);
  if (wx == SC_WX_SMOG) scFog(now, 3.0f, 1.0f);

  // -- the party --
  if (nOn == 0) {
    scTumbleweed(now);
  } else if (night) {
    const float fx = 120, fy = (float)RD_FOOT;
    scFireGlow(fx, fy, now);
    static const float CAMP[5] = { -32, 32, -60, 60, -84 };
    int k = 0, kd = 0;
    for (int n = 0; n < nOn; n++) {
      const SceneSurv& v = S.s[seated[n]];
      if (v.depth) { scHatch(206 - 22 * kd++, seated[n]); continue; }
      float x = fx + CAMP[k < 5 ? k : 4];
      float dir = x < fx ? 1.0f : -1.0f;
      ScFig F = { x, (float)RD_FOOT + (k & 2 ? 2.0f : 0.0f), dir, v.arch, (uint8_t)(v.ll ? SCP_SIT : SCP_LIE),
                  (uint8_t)seated[n], 0, FXP_FLAME, dir, -0.4f, 1.3f };
      if (!v.ll) F.x = x - dir * 4;
      for (int p = 0; p < 3; p++) scFigure(F, p);
      k++;
    }
    scFire(fx, fy, now);
  } else {
    // Walking right, the leader at the front, a body length and a bit apart.
    float lx = sunX > -50 ? (sunX < 120 ? -0.7f : 0.7f) : 0.0f;
    // A low sun throws them long down the road, the way every western ends.
    // Only under a sun you can see; the lean is away from it.
    float shadowLean = 0;
    if (sunX > -50 && (wx == SC_WX_CLEAR || wx == SC_WX_RAIN)) {
      float elev = fmaxf(0.0f, (float)RD_HOR - 4.0f - sunY);
      shadowLean = fminf(2.4f, 26.0f / (elev + 10.0f)) * (sunX < 120 ? 1.0f : -1.0f);
      if (fabsf(shadowLean) < 0.35f) shadowLean = 0;
    }
    float x = 212;
    int kd = 0;
    for (int n = 0; n < nOn; n++) {
      const SceneSurv& v = S.s[seated[n]];
      if (v.depth) { scHatch(26 + 24 * kd++, seated[n]); continue; }
      if (v.ll == 0) {
        // Downed: dragged along on a rope by whoever is in front.
        ScFig F = { x + 6, (float)RD_FOOT, -1.0f, v.arch, SCP_LIE, (uint8_t)seated[n], 0, rimLv, lx, -0.7f, 1.3f };
        for (int p = 0; p < 3; p++) scFigure(F, p);
        fxLine((int)x - 11, RD_FOOT - 2, (int)x + 30, RD_FOOT - 17, FX_PAL[FXP_RUST]);   // the rope
        x -= 46;
        continue;
      }
      float ph = scRoadSt.walked * 0.0105f + seated[n] * 1.9f;
      ScFig F = { x, (float)RD_FOOT, 1.0f, v.arch, (uint8_t)(v.inEnc || !walking ? SCP_STAND : SCP_WALK),
                  (uint8_t)seated[n], ph, rimLv, lx, -0.7f, 1.35f };
      F.shadow = shadowLean;
      for (int p = 0; p < 3; p++) scFigure(F, p);
      if (v.inEnc) scBalloon("?!", (int)x + 18, RD_FOOT - 50, (int)x + 8, RD_FOOT - 40);
      x -= (v.arch == 3 ? 50 : 42);
    }
  }

  // -- weather over everything in the panel --
  scClip(RD_Y0, RD_Y1);
  if (wx == SC_WX_RAIN)  scRain(now, 60, false, false);
  if (wx == SC_WX_STORM) scRain(now, 110, false, true);
  if (wx == SC_WX_CHEM)  scRain(now, 50, true, false);
  scUnclip();

  // -- the narration --
  {
    const char* pool[16];
    int n = 0;
    auto add = [&](const char* const* a, int k) { for (int i = 0; i < k && n < 16; i++) pool[n++] = a[i]; };
    bool hunt = S.doomAware >= 100 && S.doomClose > 60, doom = S.doomClose > 120;
    bool hungry = false, dry = false, down = false, below = false;
    for (int i = 0; i < nOn; i++) {
      const SceneSurv& v = S.s[seated[i]];
      hungry |= v.food <= 1; dry |= v.water <= 1; down |= v.ll == 0; below |= v.depth != 0;
    }
    if (nOn == 0) add(SC_NAR_EMPTY, SC_NN(SC_NAR_EMPTY));
    else {
      if (hunt) add(SC_NAR_HUNT, SC_NN(SC_NAR_HUNT));
      else if (doom) add(SC_NAR_DOOM, SC_NN(SC_NAR_DOOM));
      if (down) add(SC_NAR_DOWN, SC_NN(SC_NAR_DOWN));
      if (S.tcWeight > 200) add(SC_NAR_CLOCK, SC_NN(SC_NAR_CLOCK));
      if (dry) add(SC_NAR_DRY, SC_NN(SC_NAR_DRY));
      if (hungry) add(SC_NAR_HUNGRY, SC_NN(SC_NAR_HUNGRY));
      if (below) add(SC_NAR_BELOW, SC_NN(SC_NAR_BELOW));
      if (SC_NAR_WX[wx][0]) add(SC_NAR_WX[wx], 2);
      if (night) add(SC_NAR_NIGHT, SC_NN(SC_NAR_NIGHT));
      else if (dusk) add(SC_NAR_DUSK, SC_NN(SC_NAR_DUSK));
      else if (t < 0.07f) add(SC_NAR_DAWN, SC_NN(SC_NAR_DAWN));
      if (n < 3) add(SC_NAR_CALM, SC_NN(SC_NAR_CALM));
    }
    const char* pick = pool[(now / 9000) % (uint32_t)n];
    if (pick != scRoadSt.nar) { scRoadSt.nar = pick; scRoadSt.narT0 = now; }
    scNarration(pick, 5, RD_Y0 + 5, now - scRoadSt.narT0);
  }

  // -- the masthead --
  fxRect(0, 0, FX_W, RD_Y0, FX_PAL[FXP_INK]);
  fxRect(0, RD_Y0 - 1, FX_W, RD_Y0, FX_PAL[FXP_LINE]);
  char buf[40];
  snprintf(buf, sizeof(buf), "DAY %u", (unsigned)S.day);
  int w = scType(buf, 4, 2, 2, FXP_HDR);
  scType(SC_WX_NAME[wx], 4 + w + 6, 7, 1, wx == SC_WX_CLEAR ? FXP_RUST : FXP_CRIT);
  scDoomsday(228, 9, S.tcWeight, now);
  // the ground's edge, ragged, where the panel stops
  for (int x = 0; x < FX_W; x++) {
    int e = (int)(fxHash2((uint32_t)x, 3) % 3);
    for (int y = RD_Y1 - e; y < RD_Y1 + 2; y++) scPx(x, y, FX_PAL[FXP_INK]);
  }

  // -- the ledger --
  fxRect(0, RD_Y1 + 2, FX_W, FX_H, FX_PAL[FXP_INK]);
  if (nOn == 0) {
    static const char L1[] = "NOBODY ON THE ROAD";
    fxF2(L1, (FX_W - fxF2Width(L1)) / 2, 190, FX_PAL[FXP_HDR]);
    scType("THE WASTELAND IS NOW", 60, 216, 1, FXP_RUST);
    scType("ACCEPTING APPLICANTS.", 57, 226, 1, FXP_RUST);
    // On the house network the LAN address is the one to type; the hotspot
    // only takes the headline when there is no LAN to send anyone to.
    const char* how = S.lanUp ? "ON THE HOUSE WI-FI, OPEN" : "JOIN THE HOTSPOT, THEN OPEN";
    scType(how, (FX_W - scTypeW(how, 1)) / 2, 250, 1, FXP_LINE);
    // plain http:// so nobody expects a padlock
    char url[28];
    snprintf(url, sizeof(url), "http://%s", S.lanUp ? S.lan : S.ap);
    fxF2(url, (FX_W - fxF2Width(url)) / 2, 262, FX_PAL[FXP_CRIT]);
    scType("PLAIN HTTP - NOT HTTPS", 54, 282, 1, FXP_RUST);
  } else {
    for (int n = 0; n < nOn && n < 5; n++) {
      const SceneSurv& v = S.s[seated[n]];
      int y = RD_Y1 + 5 + n * 25;
      // the seat badge
      fxRect(2, y + 2, 14, y + 14, FX_PAL[v.ll ? FXP_HDR : FXP_LINE]);
      char num[2] = { (char)('1' + seated[n]), 0 };
      fxGlcd(num, 6, y + 5, 1, FX_PAL[FXP_INK]);
      // the gauges, right to left: rads, water, food, then the hearts
      int gx = 238;
      auto gauge = [&](const uint8_t* icon, int iw, uint8_t val, bool bad, bool worse) {
        char b[4];
        snprintf(b, sizeof(b), "%u", (unsigned)val);
        gx -= scTypeW(b, 1);
        uint8_t col = worse ? (((now / 300) & 1) ? FXP_WHITE : FXP_CRIT) : (bad ? FXP_CRIT : FXP_HDR);
        fxGlcd(b, gx, y + 4, 1, FX_PAL[col]);
        gx -= iw + 2;
        scBits(icon, 7, iw, gx, y + 4, FX_PAL[worse || bad ? FXP_CRIT : FXP_RUST]);
        gx -= 5;
      };
      gauge(SC_TREF, 7, v.rad, v.rad >= 4, v.rad >= 7);
      gauge(SC_DROP, 5, v.water, v.water <= 2, v.water <= 1);
      gauge(SC_CAN, 5, v.food, v.food <= 2, v.food <= 1);
      int cap = fxMinI(10, fxMaxI(v.llCap, v.ll));
      int hx = gx - cap * 6;
      bool frail = v.ll <= 2;
      // A weak heart beats: frail ones pulse, twice a second.
      bool beat = frail && (now % 800) < 140;
      for (int k = 0; k < cap; k++) {
        if (k < v.ll) scBits(SC_HEART, 5, 5, hx + k * 6, y + 5, FX_PAL[beat ? FXP_WHITE : (frail ? FXP_CRIT : FXP_FLAME)]);
        else          scBits(SC_HEARTO, 5, 5, hx + k * 6, y + 5, FX_PAL[FXP_LINE]);
      }
      if (v.ll == 0) scBits(SC_SKULL, 6, 5, hx - 8, y + 4, FX_PAL[FXP_CRIT]);
      char nm[16];
      scFitF2(nm, sizeof(nm), v.name[0] ? v.name : "Walker", hx - 22 - (v.ll ? 0 : 8));
      fxF2(nm, 18, y, FX_PAL[v.ll ? FXP_CRIT : FXP_RUST]);
      fxGlcd(scQuip(v, now), 18, y + 16, 1, FX_PAL[FXP_BRICK]);
    }
    if (nOn > 5) scType("+ MORE, SOMEWHERE", 120, RD_Y1 + 5 + 5 * 25, 1, FXP_LINE);
  }

  // -- the footer: all that is left of the diagnostics --
  fxRect(0, 299, FX_W, 300, FX_PAL[FXP_TRACK]);
  snprintf(buf, sizeof(buf), "JOIN %s", S.ap);
  fxGlcd(buf, 3, 306, 1, FX_PAL[FXP_LINE]);
  snprintf(buf, sizeof(buf), "LAN %s", S.lan);
  fxGlcd(buf, 237 - scTypeW(buf, 1), 306, 1, FX_PAL[S.lanUp ? FXP_LINE : FXP_DIM]);
}

// ── 7. Screen 6: THE ADMIRED ─────────────────────────────────────────────────
// The score ladder as a boneyard on a hill. Everyone on the board is dead;
// the party is ranked in among them. The one you are climbing toward gets the
// big headstone in front, their name and score cut into it and their
// epitaph under that; beside it the party's leader stands at an open grave
// with a shovel and a sign -- the plot is reserved, and the sign says how far
// they still have to go. Under the hill the ladder runs down the page, and at
// the foot of it the last one you passed, their epitaph going by on a ticker.
//
//   y   0-19   masthead    THE ADMIRED                  OUT AT 10000
//   y  20-191  the panel   moon, crows, the hill, the stone, the grave
//   y 194-293  the ladder  9 rows: rank, score, cross or badge, name
//   y 296-319  the wake    LAST PASSED, and their epitaph going by
//
// This table is a MIRROR of ADMIRED in data/game-data.js, which draws the
// same board in the browser. The LCD has no JS and the browser has no flash
// access, so the rows genuinely live twice: change one and change the other
// or the two boards disagree about who you just passed. The wording rules are
// in the game-data.js comment -- dry, lower case, one line each, and every
// name on it is dead.
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

static const int BY_Y0 = 20, BY_GROUND = 186, BY_Y1 = 192;

// Word-wrap into glcd lines of at most `cols` characters, upper-cased: it is
// cut into stone, and stone-cutters charged by the letter, not the case.
static int scWrapCols(const char* s, int cols, char out[][40], int maxLines, bool upper) {
  int n = 0;
  if (cols > 39) cols = 39;
  while (*s && n < maxLines) {
    while (*s == ' ') s++;
    int len = (int)strlen(s);
    if (len == 0) break;
    int cut = len <= cols ? len : cols;
    if (len > cols) { while (cut > 0 && s[cut] != ' ') cut--; if (cut == 0) cut = cols; }
    if (n == maxLines - 1 && len > cut) {                  // the chisel ran out of stone
      cut = fxMinI(len, cols - 3);
      while (cut > 0 && s[cut] != ' ') cut--;
      if (cut == 0) cut = cols - 3;
      for (int i = 0; i < cut; i++) out[n][i] = upper ? (char)toupper((unsigned char)s[i]) : s[i];
      strcpy(out[n] + cut, "...");
      return n + 1;
    }
    for (int i = 0; i < cut; i++) out[n][i] = upper ? (char)toupper((unsigned char)s[i]) : s[i];
    out[n][cut] = 0;
    n++;
    s += cut;
  }
  return n;
}

// A headstone: an arch whose rim is hand-cut (it wanders), leaning a little
// the way they all do eventually, shaded moonlit-left to shadowed-right in
// halftone, mottled, with a crack and lichen at the foot.
struct ScStone { float x, base, w, h, lean; uint32_t seed; };
static void scStoneShape(const ScStone& S, int y, float* xl, float* xr) {
  float top = S.base - S.h, r = S.w * 0.5f, v = y + 0.5f - top;
  float hw = r;
  if (v < r) { float q = 1.0f - (r - v) / r; hw = r * sqrtf(fmaxf(0.0f, q * (2.0f - q))); }
  hw *= 1.0f + 0.05f * (fxNoise1(y * 0.3f, S.seed) - 0.5f);
  float cx = S.x + S.lean * (S.base - y);
  *xl = cx - hw; *xr = cx + hw;
}
static void scStone(const ScStone& S, float lvHi, float lvLo) {
  int y0 = (int)(S.base - S.h), y1 = (int)S.base;
  for (int y = y0 - 1; y < y1; y++) {
    float xl, xr;
    scStoneShape(S, y, &xl, &xr);
    if (xr - xl < 0.5f) continue;
    fxSpan(y, (int)floorf(xl) - 1, (int)ceilf(xr) + 1, FX_PAL[FXP_INK]);
  }
  for (int y = y0; y < y1; y++) {
    float xl, xr;
    scStoneShape(S, y, &xl, &xr);
    xl += 0.6f; xr -= 0.6f;
    for (int x = (int)ceilf(xl); x < (int)floorf(xr); x++) {
      float u = (x - xl) / fmaxf(1.0f, xr - xl);
      float m = fxNoise2(x * 0.18f, y * 0.18f, S.seed + 7) - 0.5f;
      float lv = fxLerp(lvHi, lvLo, u) + m * 1.6f;
      if (y > y1 - 6 && fxHash2((uint32_t)(x * 7 + y), S.seed) % 5 == 0) lv -= 2.0f;   // lichen
      scDither(x, y, lv);
    }
  }
  // the crack, from the rim down
  FxRng R; R.s = S.seed;
  float cx = S.x + S.w * (0.15f + 0.3f * R.u()), cy = S.base - S.h + 2;
  for (int i = 0; i < 6; i++) {
    float nx = cx + R.sgn() * 3.0f, ny = cy + 3 + R.u() * 5;
    fxLine((int)cx, (int)cy, (int)nx, (int)ny, FX_PAL[FXP_SOOT]);
    cx = nx; cy = ny;
  }
}
// Type cut into stone: the dark of the cut, with the lip of it catching the
// moon one pixel up and left. Follows the stone's lean.
static void scCarve(const ScStone& S, const char* t, int cy, int sc, bool f2) {
  float cx = S.x + S.lean * (S.base - cy);
  int w = f2 ? fxF2Width(t) : scTypeW(t, sc);
  int x = (int)(cx - w * 0.5f);
  if (f2) {
    fxF2(t, x - 1, cy - 1, FX_PAL[FXP_OK]);
    fxF2(t, x, cy, FX_PAL[FXP_SOOT]);
  } else {
    fxGlcd(t, x - 1, cy - 1, sc, FX_PAL[FXP_OK]);
    fxGlcd(t, x, cy, sc, FX_PAL[FXP_SOOT]);
  }
}

// A crow. Hops, looks about, and once in a while flies off and comes back.
// Black on a night sky is nothing, so the moon catches its back (`rim`
// pass first, a pixel toward the light, then the ink).
static void scCrowSit(float x, float y, float dir, uint32_t now, uint32_t seed, float k) {
  uint32_t beat = now / 700 + seed;
  float hop = (fxHash(beat) % 9 == 0) ? -2.0f * k : 0.0f;
  float look = (fxHash(beat / 3 + 11) & 1) ? 1.0f : -1.0f;
  fxLine((int)(x - k), (int)(y - 2 * k + hop), (int)(x - k), (int)y, FX_PAL[FXP_INK]);   // legs
  fxLine((int)(x + k), (int)(y - 2 * k + hop), (int)(x + k), (int)y, FX_PAL[FXP_INK]);
  y += hop - 2 * k;
  for (int pass = 0; pass < 2; pass++) {
    float o = pass ? 0.0f : 1.0f;
    uint16_t c = FX_PAL[pass ? FXP_INK : FXP_BRICK];
    fxBlob(x + o, y - 4 * k - o, 1, 0, 4.2f * k, 2.8f * k, seed, c);                 // body
    fxTri(x - dir * 3 * k + o, y - 5 * k - o, x - dir * 9 * k + o, y - 2 * k - o, x - dir * 3 * k + o, y - 2 * k - o, c);
    fxDisc(x + dir * 3.5f * k + o, y - 7.5f * k - o, 2.2f * k, c);                   // head
    float bx = x + dir * (3.5f + look * 2.4f) * k + o;
    fxTri(bx, y - 8.2f * k - o, bx, y - 6.8f * k - o, bx + look * dir * 3.2f * k, y - 7.3f * k - o, c);   // beak
  }
  scPx((int)(x + dir * 4.0f * k), (int)(y - 8.2f * k), FX_PAL[FXP_HOT]);           // an eye on you
}
static void scCrowFly(float x, float y, uint32_t now) {
  float up = (fxNoise1(now * 0.02f, 3) - 0.5f) * 8.0f;
  for (int pass = 0; pass < 2; pass++) {
    float o = pass ? 0.0f : 1.0f;
    uint16_t c = FX_PAL[pass ? FXP_INK : FXP_RUST];
    fxBlob(x + o, y - o, 1, 0, 3.4f, 1.8f, 5, c);
    fxStroke(x - 1 + o, y - o, x - 10 + o, y + up - o, 2.4f, 0.8f, c);
    fxStroke(x + 1 + o, y - o, x + 10 + o, y + up - o, 2.4f, 0.8f, c);
    fxDisc(x + 3.5f + o, y - 1 - o, 1.6f, c);
  }
}

// The iron fence along the brow of the hill, some of it bent, some of it gone.
static void scFence(float x0, float x1, float (*hill)(float), uint16_t col) {
  for (float x = x0; x < x1; x += 5) {
    uint32_t h = fxHash((uint32_t)(x * 13));
    if (h % 7 == 0) continue;
    float y = hill(x), bend = (h % 11 == 0) ? 3.0f : 0.0f;
    fxLine((int)x, (int)y, (int)(x + bend), (int)y - 13, col);
    fxTri(x + bend - 1.5f, y - 13, x + bend + 1.5f, y - 13, x + bend, y - 17, col);
  }
  for (int r = 0; r < 2; r++)
    for (float x = x0; x < x1; x += 2) scPx((int)x, (int)hill(x) - 4 - r * 7, col);
}
static float scHillY(float x) {
  return 118.0f + x * 0.16f + 9.0f * (fxNoise1(x * 0.02f, 61) - 0.5f);
}

static struct { const char* nar; uint32_t narT0; } scBoneSt = { nullptr, 0 };

// The still layer, kept. Most of the boneyard does not move -- the hill, the
// stones, the sign, the ladder -- so it is drawn once into a PSRAM copy and
// only redrawn when what it shows changes; each frame is that copy plus the
// things that do move (cloud over the moon, the crows, the mist, the ticker).
// If the copy will not allocate, everything is simply drawn every frame.
static uint16_t* scStill    = nullptr;
static uint32_t  scStillKey = 0;
static bool      scStillTried = false;

static void scAdmiredDraw(const SceneSnap& S, uint32_t now) {
  uint16_t* const out = fxDst;

  // -- who is where: the dead and the living in one ranked list --
  struct Row { uint16_t sc; const char* nm; const char* ln; int8_t pid; };
  Row rows[ADMIRED_COUNT + SC_SEATS];
  int n = 0;
  for (int i = 0; i < ADMIRED_COUNT; i++) rows[n++] = { ADMIRED[i].sc, ADMIRED[i].nm, ADMIRED[i].ln, (int8_t)-1 };
  for (int i = 0; i < SC_SEATS; i++)
    if (S.s[i].on) rows[n++] = { S.s[i].score, S.s[i].name[0] ? S.s[i].name : "Walker", nullptr, (int8_t)i };
  for (int i = 1; i < n; i++) {               // insertion sort; a tie puts the living above the dead
    Row k = rows[i];
    int j = i - 1;
    while (j >= 0 && (rows[j].sc < k.sc || (rows[j].sc == k.sc && rows[j].pid < 0 && k.pid >= 0))) {
      rows[j + 1] = rows[j];
      j--;
    }
    rows[j + 1] = k;
  }
  int best = -1;
  for (int i = 0; i < n; i++) if (rows[i].pid >= 0) { best = i; break; }
  uint16_t bestSc = best >= 0 ? rows[best].sc : 0;
  const AdmiredRow* next = nullptr;
  const AdmiredRow* past = nullptr;
  for (int i = ADMIRED_COUNT - 1; i >= 0; i--) if (ADMIRED[i].sc > bestSc) { next = &ADMIRED[i]; break; }
  for (int i = 0; i < ADMIRED_COUNT; i++) if (ADMIRED[i].sc <= bestSc) { past = &ADMIRED[i]; break; }
  int lead = best >= 0 ? rows[best].pid : -1;
  const int LY0 = 196, LH = 11, LROWS = 9;
  int start = best >= 0 ? best - 3 : 0;
  if (start > n - LROWS) start = n - LROWS;
  if (start < 0) start = 0;
  int below = 0;
  for (int i = start + LROWS; i < n; i++) if (rows[i].pid >= 0) below++;
  const uint8_t signHead = (uint8_t)((now / 11000) % 3);

  // Everything the still layer shows, folded into one key.
  uint32_t key = 0x6A09E667u ^ signHead;
  for (int i = 0; i < n; i++) key = fxHash2(key, (uint32_t)rows[i].sc * 8u + (uint32_t)(rows[i].pid + 1));
  for (int i = 0; i < SC_SEATS; i++)
    if (S.s[i].on) for (const char* p = S.s[i].name; *p; p++) key = fxHash2(key, (uint8_t)*p);
  if (lead >= 0) key = fxHash2(key, S.s[lead].arch);

  if (!scStillTried) { scStillTried = true; scStill = (uint16_t*)fxAlloc((size_t)FX_W * FX_H * 2); }
  const bool cached = scStill && scStillKey == key && key != 0;
  float hillY[FX_W];
  for (int x = 0; x < FX_W; x++) hillY[x] = scHillY((float)x);
  const float mx = 186, my = 58;
  ScStone big = { 64, (float)BY_GROUND, 104, 118, -0.05f, 0xA11CEu };

  if (!cached) {
    if (scStill) fxDst = scStill;
    // -- night on the hill --
    for (int y = BY_Y0; y < BY_Y1; y++) scTone(y, 0, FX_W, 0.5f + 3.0f * (float)(y - BY_Y0) / (BY_Y1 - BY_Y0));
    // the moon, huge
    for (int y = (int)my - 34; y < (int)my + 34; y++)
      for (int x = (int)mx - 34; x < (int)mx + 34; x++) {
        float dx = x + 0.5f - mx, dy = y + 0.5f - my, r = sqrtf(dx * dx + dy * dy);
        if (r < 21) {
          float m = fxNoise2(x * 0.16f, y * 0.16f, 91);       // the seas
          scDither(x, y, 12.2f - dx * 0.06f - (m > 0.58f ? 2.4f : 0.0f) - (r > 18 ? 1.0f : 0.0f));
        } else if (r < 33 && scDot(x, y, (1 - (r - 21) / 12) * 0.5f)) scPx(x, y, FX_PAL[FXP_EMBER]);
      }
    // -- the hill: back rows of stones, a fence, a dead tree --
    for (int y = BY_Y0; y < BY_Y1; y++)
      for (int x = 0; x < FX_W; x++)
        if (y >= hillY[x]) scDither(x, y, 2.3f + (y - hillY[x]) * 0.012f);
    for (int x = 0; x < FX_W; x++) {                       // the brow catches the moon
      scPx(x, (int)hillY[x], FX_PAL[x > 110 ? FXP_RUST : FXP_EMBER]);
    }
    scTree(26, hillY[26] + 2, 0x7EE, 16, 4.0f, 4, FX_PAL[FXP_RUST]);
    scFence(44, 240, scHillY, FX_PAL[FXP_INK]);
    for (int x = 44; x < 240; x += 5) {
      int y = (int)hillY[x] - 13;
      scPx(x + 1, y, FX_PAL[FXP_EMBER]);                   // moonlight on the spear tips
    }
    for (int i = 0; i < 9; i++) {                          // the ones higher up the board
      uint32_t h = fxHash2((uint32_t)i, 0x5707u);
      float x = 50 + i * 21 + (h % 9), y = scHillY(x) + 6 + (h >> 8) % 8;
      float sz = 0.55f + 0.2f * fxU(h >> 3) + (y - 125) * 0.012f;
      if ((h >> 16) % 4 == 0) {                            // a cross
        fxStroke(x, y, x + 0.8f, y - 15 * sz, 2.4f * sz + 0.6f, 2.0f * sz + 0.6f, FX_PAL[FXP_RUST]);
        fxStroke(x - 5 * sz, y - 10 * sz, x + 5 * sz, y - 10.5f * sz, 2.2f * sz + 0.6f, 2.2f * sz + 0.6f, FX_PAL[FXP_RUST]);
      } else {
        ScStone st = { x, y, 12 * sz, 16 * sz, fxS(h >> 5) * 0.12f, h };
        scStone(st, 5.6f, 3.0f);
      }
    }
    // the near ground
    for (int x = 0; x < FX_W; x++) {
      float e = 164 + 5.0f * fxNoise1(x * 0.05f, 13) - x * 0.02f;
      for (int y = (int)e; y < BY_Y1; y++) scDither(x, y, 3.0f - (y - e) * 0.05f);
    }

    // -- the stone you are climbing toward --
    scStone(big, 7.4f, 3.6f);
    char line[40];
    int y = (int)(big.base - big.h) + 12;
    if (next) {
      scCarve(big, "HERE LIES", y, 1, false);
      char nm[20];
      scFitF2(nm, sizeof(nm), next->nm, 92);
      scCarve(big, nm, y + 11, 1, true);
      snprintf(line, sizeof(line), "%u", (unsigned)next->sc);
      scCarve(big, line, y + 30, 2, false);
      char ep[6][40];
      int ln = scWrapCols(next->ln, 15, ep, 5, true);
      for (int i = 0; i < ln; i++) scCarve(big, ep[i], y + 52 + i * 9, 1, false);
    } else {
      scCarve(big, "NOBODY", y + 14, 2, false);
      scCarve(big, "ABOVE.", y + 34, 2, false);
      scCarve(big, "WALK OUT.", y + 60, 1, false);
    }
    for (int i = 0; i < 5; i++) {                          // stalks of whatever grows on graves
      float fx0 = big.x - 40 + i * 19 + (fxHash(i) % 7);
      fxLine((int)fx0, BY_GROUND + 1, (int)fx0 + 1, BY_GROUND - 5, FX_PAL[FXP_EMBER]);
    }

    // -- the plot, reserved --
    float px0 = 140, px1 = 196, py = BY_GROUND - 2;
    float hx[7] = { 196, 204, 214, 226, 236, 240, 196 }, hy[7] = { py, py - 8, py - 13, py - 11, py - 4, py, py };
    fxPoly(hx, hy, 7, FX_PAL[FXP_LINE]);
    for (int i = 0; i < 22; i++) {
      uint32_t h = fxHash(i * 31 + 3);
      scPx(198 + (int)(h % 38), (int)py - 1 - (int)((h >> 8) % 9), FX_PAL[(h >> 20) & 1 ? FXP_EMBER : FXP_TRACK]);
    }
    float qx[4] = { px0, px1, px1 - 5, px0 + 5 }, qy[4] = { py - 3, py - 3, py + 5, py + 5 };
    fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
    fxLine((int)px0, (int)py - 3, (int)px1, (int)py - 3, FX_PAL[FXP_RUST]);
    // The sign: a board on a stake, hand-painted.
    const int sx0 = 142, sy0 = 112, sw = 60, sh = 38;
    fxStroke(sx0 + sw * 0.5f, (float)sy0 + sh, sx0 + sw * 0.5f + 1, py - 1, 3.0f, 2.6f, FX_PAL[FXP_INK]);
    fxLine(sx0 + sw / 2 + 2, sy0 + sh, sx0 + sw / 2 + 3, (int)py - 1, FX_PAL[FXP_EMBER]);
    fxRect(sx0 - 1, sy0 - 1, sx0 + sw + 1, sy0 + sh + 1, FX_PAL[FXP_INK]);
    for (int yy = sy0; yy < sy0 + sh; yy++)
      for (int x = sx0; x < sx0 + sw; x++)
        scDither(x, yy, 6.6f + 1.4f * (fxNoise2(x * 0.05f, yy * 0.5f, 4) - 0.5f) - ((yy - sy0) % 13 == 12 ? 2 : 0));
    char a[24], b[24], c[24];
    if (lead >= 0) {
      static const char* const HEAD[3] = { "RESERVED", "PLOT HELD", "NOT YET" };
      snprintf(a, sizeof(a), "%s", HEAD[signHead]);
      scFitF2(b, sizeof(b), rows[best].nm, sw - 6);
      if (next) snprintf(c, sizeof(c), "%u TO GO", (unsigned)(next->sc - bestSc));
      else      snprintf(c, sizeof(c), "DUE OUT");
    } else {
      snprintf(a, sizeof(a), "PLOT");
      snprintf(b, sizeof(b), "AVAILABLE");
      snprintf(c, sizeof(c), "APPLY WITHIN");
    }
    fxGlcd(a, sx0 + (sw - scTypeW(a, 1)) / 2, sy0 + 3, 1, FX_PAL[FXP_INK]);
    if (lead >= 0) fxF2(b, sx0 + (sw - fxF2Width(b)) / 2, sy0 + 11, FX_PAL[FXP_INK]);
    else           fxGlcd(b, sx0 + (sw - scTypeW(b, 1)) / 2, sy0 + 15, 1, FX_PAL[FXP_INK]);
    fxGlcd(c, sx0 + (sw - scTypeW(c, 1)) / 2, sy0 + 28, 1, FX_PAL[FXP_INK]);
    // The leader, by the grave, leaning on the spade and not reading the sign.
    float spX = lead >= 0 ? 212.0f : 213.0f;
    fxStroke(spX + 2, BY_GROUND - 13, spX - 2, BY_GROUND - 47, 2.6f, 2.4f, FX_PAL[FXP_RUST]);
    fxStroke(spX + 2, BY_GROUND - 13, spX - 2, BY_GROUND - 47, 1.6f, 1.4f, FX_PAL[FXP_INK]);
    float bl[4] = { spX - 1, spX + 6, spX + 5, spX - 2 };
    float bly[4] = { BY_GROUND - 18.0f, BY_GROUND - 18.0f, BY_GROUND - 8.0f, BY_GROUND - 8.0f };
    fxPoly(bl, bly, 4, FX_PAL[FXP_INK]);
    fxLine((int)spX - 1, BY_GROUND - 18, (int)spX + 6, BY_GROUND - 18, FX_PAL[FXP_HDR]);
    if (lead >= 0) {
      const SceneSurv& v = S.s[lead];
      // The hand goes where the spade's handle is, a little over halfway up it.
      ScFig F = { 222, (float)BY_GROUND - 12, -1.0f, v.arch, SCP_HOLD, (uint8_t)lead, 0, FXP_BRICK, 0.7f, -0.7f, 1.35f };
      F.handU = (222 - (spX - 0.2f)) / 1.35f;
      F.handV = ((BY_GROUND - 12) - (BY_GROUND - 32.0f)) / 1.35f;
      for (int p = 0; p < 3; p++) scFigure(F, p);
    }

    // -- the masthead --
    fxRect(0, 0, FX_W, BY_Y0, FX_PAL[FXP_INK]);
    fxRect(0, BY_Y0 - 1, FX_W, BY_Y0, FX_PAL[FXP_LINE]);
    scType("THE ADMIRED", 4, 2, 2, FXP_HDR);
    scType("OUT AT 10000", 237 - scTypeW("OUT AT 10000", 1), 7, 1, FXP_RUST);
    for (int x = 0; x < FX_W; x++) {
      int e = (int)(fxHash2((uint32_t)x, 5) % 3);
      for (int yy = BY_Y1 - e; yy < BY_Y1 + 2; yy++) scPx(x, yy, FX_PAL[FXP_INK]);
    }

    // -- the ladder --
    fxRect(0, BY_Y1 + 2, FX_W, FX_H, FX_PAL[FXP_INK]);
    static const uint8_t CROSS[7] = { 0x20, 0x20, 0xF8, 0x20, 0x20, 0x20, 0x20 };
    char buf[48];
    for (int k = 0; k < LROWS && start + k < n; k++) {
      const Row& r = rows[start + k];
      int ry = LY0 + k * LH;
      bool lv = r.pid >= 0;
      if (lv) {
        fxRect(0, ry - 2, FX_W, ry + 9, FX_PAL[FXP_BAND]);
        fxRect(0, ry - 2, 2, ry + 9, FX_PAL[FXP_HDR]);
      }
      snprintf(buf, sizeof(buf), "#%d", start + k + 1);
      fxGlcd(buf, 5, ry, 1, FX_PAL[lv ? FXP_HDR : FXP_LINE]);
      snprintf(buf, sizeof(buf), "%u", (unsigned)r.sc);
      fxGlcd(buf, 70 - scTypeW(buf, 1), ry, 1, FX_PAL[lv ? FXP_CRIT : FXP_BRICK]);
      if (lv) {
        fxRect(76, ry - 1, 84, ry + 8, FX_PAL[FXP_HDR]);
        char nb[2] = { (char)('1' + r.pid), 0 };
        fxGlcd(nb, 78, ry, 1, FX_PAL[FXP_INK]);
      } else {
        scBits(CROSS, 7, 5, 78, ry, FX_PAL[FXP_LINE]);
      }
      char nm[24];
      snprintf(nm, sizeof(nm), "%.23s", r.nm);
      if (lv) for (char* q = nm; *q; q++) *q = (char)toupper((unsigned char)*q);
      fxGlcd(nm, 90, ry, 1, FX_PAL[lv ? FXP_GLOW : FXP_RUST]);
    }
    // -- the wake: who you stepped over last --
    fxRect(0, 295, FX_W, 296, FX_PAL[FXP_TRACK]);
    char tail[16] = "";
    if (below) snprintf(tail, sizeof(tail), "+%d BELOW", below);
    int room = 237 - (tail[0] ? scTypeW(tail, 1) + 6 : 0);
    if (past) snprintf(buf, sizeof(buf), "LAST PASSED %s %u", past->nm, (unsigned)past->sc);
    else      snprintf(buf, sizeof(buf), "LAST PASSED nobody yet");
    buf[fxMaxI(0, fxMinI((int)strlen(buf), (room - 3) / 6))] = 0;
    fxGlcd(buf, 3, 300, 1, FX_PAL[FXP_BRICK]);
    if (tail[0]) fxGlcd(tail, 237 - scTypeW(tail, 1), 300, 1, FX_PAL[FXP_HDR]);
    if (!past) fxGlcd("the board starts at 120. that is KEV.", 3, 310, 1, FX_PAL[FXP_LINE]);

    scStillKey = scStill ? key : 0;
    fxDst = out;
  }
  if (scStill) memcpy(out, scStill, (size_t)FX_W * FX_H * 2);

  // ── what moves ──
  // stars, only where the sky shows
  for (int i = 0; i < 40; i++) {
    uint32_t h = fxHash2((uint32_t)i, 0xB0E5u);
    int x = (int)(h % FX_W), y = BY_Y0 + 2 + (int)((h >> 9) % 90);
    if (y >= hillY[x] - 2 || fxLum(out[y * FX_W + x]) > 60) continue;
    if ((fxHash2((uint32_t)i, now / 500) & 7) == 0) continue;
    out[y * FX_W + x] = FX_PAL[(h >> 20) % 6 == 0 ? FXP_HOT : FXP_LINE];
  }
  // cloud going over the moon
  {
    float ct = now * 0.00006f;
    for (int y = 28; y < 94; y++)
      for (int x = 124; x < FX_W; x++) {
        if (y >= hillY[x] - 1) continue;
        float c = fxNoise2(x * 0.03f - ct * 30, y * 0.12f, 191) * 0.75f + fxNoise2(x * 0.09f - ct * 50, y * 0.3f, 193) * 0.25f;
        float band = 1.0f - fabsf(y - 62) / 32.0f;
        float d = fxClampF((c - 0.55f) * 3.2f, 0, 1) * band;
        if (scDot(x, y, d)) out[y * FX_W + x] = FX_PAL[d > 0.6f ? FXP_INK : FXP_SOOT];
      }
  }
  {
    uint32_t cyc = now % 21000;                            // a crow crossing the moon, now and then
    if (cyc < 5200) scCrowFly(250.0f - cyc * 0.055f, 44.0f + 8.0f * fxNoise1(cyc * 0.001f, 9), now);
  }
  {
    // Perched on the rim of the stone, wherever the rim is at that column.
    const float cx = big.x + 20;
    int ty = (int)(big.base - big.h);
    for (; ty < (int)big.base; ty++) { float xl, xr; scStoneShape(big, ty, &xl, &xr); if (cx >= xl && cx <= xr) break; }
    scCrowSit(cx, (float)ty, -1.0f, now, 7, 1.5f);
  }
  scFog(now, 3.0f, 0.5f, 150, BY_Y1);
  // the living breathe; the dead hold still
  for (int k = 0; k < LROWS && start + k < n; k++)
    if (rows[start + k].pid >= 0 && ((now / 500) & 1)) fxGlcd("<", 232, LY0 + k * LH, 1, FX_PAL[FXP_HDR]);
  if (past) {                                              // the epitaph, going by like the news
    char tick[160];
    snprintf(tick, sizeof(tick), "%s   ***   ", past->ln);
    int tl = (int)strlen(tick), off = (int)((now / 45) % (uint32_t)(tl * 6));
    fxRect(0, 309, FX_W, 318, FX_PAL[FXP_INK]);
    for (int r = 0; r < 3; r++) fxGlcd(tick, 3 - off + r * tl * 6, 310, 1, FX_PAL[FXP_LINE]);
  }
}

// ── 8. Screen 2: THE CHRONICLE ───────────────────────────────────────────────
// The chronicle as a comic page: the last three things that happened, each a
// drawn panel. The two before are small panels side by side, printed and
// gone still; the newest is the big panel under them, and it is live -- the
// fire burns, the rain comes down, the zeds drift off the sleeper -- and when
// something new happens it slams in and the others move up the page.
//
//   y   0-19   masthead    THE CHRONICLE                 DAY 12
//   y  21-132  two panels  the one before last, the last-but-one
//   y 136-305  the panel   the newest, a caption box over it
//   y 307-319  folio       the page number, i.e. how much has happened
//
// The picture is chosen by the entry's margin mark (K10Glyph -- mirrored as
// SC_GLY_* here, and ui-screens.hpp asserts the two agree); the words are the
// chronicle's own sentence, lettered in capitals as a caption.

// A panel's stage: its rectangle, where the ground is, the figures' scale,
// and whether it is the live one (a still one is frozen at `now`).
struct ScStage {
  int      x0, y0, x1, y1;
  float    gy, cx, k;
  uint8_t  tone, rim;
  uint32_t seed, now;
  bool     live, night;
  uint32_t t0;          // when the live one landed: a picture that tells a story starts it here
};
static void scClipRect(int x0, int y0, int x1, int y1) {
  for (int x = 0; x < FX_W; x++) {
    bool in = x >= x0 && x < x1;
    scClipT[x] = (int16_t)(in ? y0 : 0);
    scClipB[x] = (int16_t)(in ? y1 : 0);
  }
  fxColT = scClipT; fxColB = scClipB; fxColY0 = y0; fxColY1 = y1;
}

// SFX in a starburst: a spiky balloon fanned out of triangles, and the word.
static void scBang(const char* w, float cx, float cy, float r, uint32_t seed, int sc) {
  const int N = 14;
  float px[N], py[N];
  FxRng R; R.s = seed;
  r = fmaxf(r, scTypeW(w, sc) * 0.42f + 3.0f * sc);          // the balloon is made to fit the noise
  for (int i = 0; i < N; i++) {
    float a = i * (FX_TAU / N) + R.sgn() * 0.12f, rr = (i & 1) ? r * (0.55f + 0.1f * R.u()) : r * (0.95f + 0.3f * R.u());
    px[i] = cx + cosf(a) * rr * 1.35f; py[i] = cy + sinf(a) * rr;
  }
  for (int pass = 0; pass < 2; pass++) {
    float g = pass ? 1.0f : 1.22f;
    uint16_t c = FX_PAL[pass ? FXP_GLOW : FXP_INK];
    for (int i = 0; i < N; i++) {
      int j = (i + 1) % N;
      fxTri(cx, cy, cx + (px[i] - cx) * g, cy + (py[i] - cy) * g, cx + (px[j] - cx) * g, cy + (py[j] - cy) * g, c);
    }
  }
  int tw = scTypeW(w, sc);
  fxGlcd(w, (int)(cx - tw * 0.5f) + 1, (int)(cy - 4 * sc) + 1, sc, FX_PAL[FXP_HDR]);
  fxGlcd(w, (int)(cx - tw * 0.5f), (int)(cy - 4 * sc), sc, FX_PAL[FXP_INK]);
}

// An umbrella over someone: a scalloped dome on a stick. `wrong` turns it
// inside out (the storm got it); `holes` eats it (the rain is chemistry).
static void scUmbrella(float hx, float hy, float r, uint8_t wrong, bool holes, uint32_t now) {
  fxStroke(hx, hy, hx, hy - r * 1.1f, 1.4f, 1.2f, FX_PAL[FXP_INK]);
  float top = hy - r * 1.1f;
  if (wrong) {
    float fl = (fxNoise1(now * 0.01f, 5) - 0.5f) * r * 0.4f;
    float xs[3] = { hx - r * 0.9f + fl, hx, hx + r * 0.9f + fl }, ys[3] = { top - r * 1.0f, top, top - r * 0.9f };
    fxTri(xs[0], ys[0], xs[1], ys[1], xs[1] + fl * 0.3f, top - r * 0.35f, FX_PAL[FXP_INK]);
    fxTri(xs[2], ys[2], xs[1], ys[1], xs[1] + fl * 0.3f, top - r * 0.35f, FX_PAL[FXP_INK]);
    fxLine((int)xs[0], (int)ys[0], (int)hx, (int)top, FX_PAL[FXP_RUST]);
    fxLine((int)xs[2], (int)ys[2], (int)hx, (int)top, FX_PAL[FXP_RUST]);
    return;
  }
  const int N = 12;
  float px[N + 2], py[N + 2];
  for (int i = 0; i <= N; i++) {
    float a = 3.14159f + i * (3.14159f / N);
    px[i] = hx + cosf(a) * r; py[i] = top + sinf(a) * r * 0.62f;
  }
  for (int i = 0; i < N; i++) fxTri(hx, top, px[i], py[i], px[i + 1], py[i + 1], FX_PAL[FXP_INK]);
  for (int i = 0; i < 4; i++) {                             // the scallops along the rim
    float u0 = hx - r + i * r * 0.5f;
    fxDisc(u0 + r * 0.25f, top, r * 0.25f, FX_PAL[FXP_INK]);
  }
  fxLine((int)(hx - r + 1), (int)top - 1, (int)(hx - 2), (int)(top - r * 0.55f), FX_PAL[FXP_RUST]);   // lit rib
  if (holes)
    for (int i = 0; i < 7; i++) {
      uint32_t h = fxHash(i * 13 + 1);
      float x = hx - r * 0.7f + fxU(h) * r * 1.4f, y = top - r * 0.45f * fxU(h >> 8);
      fxDisc(x, y, 1.2f, FX_PAL[FXP_LINE]);
      float ph = fmodf(now * 0.0012f + fxU(h >> 4), 1.0f);  // and it smokes
      fxDisc(x + ph * 3, y - 3 - ph * 12, 0.8f + ph * 1.8f, FX_PAL[ph < 0.5f ? FXP_RUST : FXP_LINE]);
    }
}

// A heap of rubble, lit along its top.
static void scHeap(float cx, float by, float w, float h, uint32_t seed, uint8_t rimLv) {
  FxRng R; R.s = seed;
  float px[9], py[9];
  for (int i = 0; i < 9; i++) {
    float u = i / 8.0f;
    px[i] = cx - w * 0.5f + w * u;
    py[i] = by - (i == 0 || i == 8 ? 0 : h * (1 - (2 * u - 1) * (2 * u - 1)) * (0.7f + 0.5f * R.u()));
  }
  for (int i = 0; i < 8; i++) {
    float qx[4] = { px[i], px[i + 1], px[i + 1], px[i] }, qy[4] = { py[i] - 1, py[i + 1] - 1, by, by };
    fxPoly(qx, qy, 4, FX_PAL[rimLv]);
    float rx[4] = { px[i], px[i + 1], px[i + 1], px[i] }, ry[4] = { py[i] + 0.6f, py[i + 1] + 0.6f, by, by };
    fxPoly(rx, ry, 4, FX_PAL[FXP_SOOT]);
  }
}

// A lump of concrete turned `a`: six corners knocked about, lit along the
// edge toward the light, and a stub of rebar out of the big ones.
static void scLump(float x, float y, float a, float s, uint32_t seed, uint8_t rimLv) {
  FxRng R; R.s = seed;
  float px[6], py[6], qx[6], qy[6];
  const float ca = cosf(a), sa = sinf(a);
  for (int i = 0; i < 6; i++) {
    float b = i * (FX_TAU / 6) + R.sgn() * 0.3f, rr = s * (0.7f + 0.4f * R.u());
    float lu = cosf(b) * rr * 1.3f, lv = sinf(b) * rr;
    px[i] = x + lu * ca - lv * sa; py[i] = y + lu * sa + lv * ca;
    qx[i] = px[i] - 0.8f; qy[i] = py[i] - 1.0f;
  }
  fxPoly(qx, qy, 6, FX_PAL[rimLv]);
  fxPoly(px, py, 6, FX_PAL[FXP_INK]);
  if (s > 3.0f)
    fxStroke(x + cosf(a - 2.2f) * s * 0.6f, y + sinf(a - 2.2f) * s * 0.6f,
             x + cosf(a - 2.2f) * s * 1.9f, y + sinf(a - 2.2f) * s * 1.9f, 1.0f, 0.8f, FX_PAL[FXP_RUST]);
}

// The page takes a knock when the live panel's picture lands one -- the first
// time through only, or a picture that loops would rattle the screen forever.
// `lt` is ms since the panel landed.
static uint32_t scJoltMark = 0;
static void scJolt(const ScStage& P, uint32_t lt, uint32_t at, float amt) {
  if (!P.live || lt < at || lt > at + 500 || scJoltMark == P.t0 + at + 1) return;
  scJoltMark = P.t0 + at + 1;
  fxImpact(amt);
}

// ── Manga marks ──
// What a body is feeling, said the way a manga page says it: focus lines,
// speed lines, the startle, the sweat drop, gloom hung over a head, a sparkle
// on a find, stars round a knocked head, an afterimage, the impact frame. All
// in the house ink and amber, sized off k so a mark reads the same in either
// size of panel.

// Roughly where a figure's head is (scFigureDraw's skeleton, solved per pose).
static void scHeadAt(const ScFig& F, float* x, float* y) {
  float u = 0.85f, v = 24.6f;
  switch (F.pose) {
    case SCP_WALK:   u = 1.9f;   v = 24.8f; break;
    case SCP_CROUCH: u = 7.1f;   v = 15.9f; break;
    case SCP_SIT:    u = 2.2f;   v = 14.1f; break;
    case SCP_SLUMP:  u = 3.2f;   v = 23.4f; break;
    case SCP_LIE:    u = -11.6f; v = 3.2f;  break;
  }
  *x = F.x + F.dir * u * F.k; *y = F.foot - v * F.k;
}
// Put a figure's near hand at (x, y) on the screen.
static inline void scHandTo(ScFig& F, float x, float y) {
  F.handU = (x - F.x) / (F.dir * F.k);
  F.handV = fmaxf(0.05f, (F.foot - y) / F.k);
}
// Focus lines on a panel: fxFocusLines' hashed wedges, cut to the panel's
// reach and re-dealt at the cel rate, in to a ragged clear zone round (x, y).
static void scFocus(const ScStage& P, float x, float y, float r, uint8_t col, float wMax, int n = 28) {
  const uint32_t boil = P.live ? P.now / 90 : 0;
  const float far = (float)(P.x1 - P.x0) * 1.2f;
  for (int i = 0; i < n; i++) {
    uint32_t h = fxHash2((uint32_t)i, P.seed + 71), hb = fxHash2((uint32_t)i, (P.seed + 71) ^ boil);
    if (fxU(h ^ 0x5bd1e995U) < 0.2f) continue;
    float a = ((float)i + fxS(hb) * 0.45f) * FX_TAU / (float)n, ca = cosf(a), sa = sinf(a);
    float r0 = r * (0.8f + 0.5f * fxU(fxHash(h + 1)) + 0.12f * fxS(hb >> 3));
    float w = 0.6f + wMax * fxU(fxHash(h + 2)) * fxU(fxHash(h + 3));
    fxTri(x + ca * r0, y + sa * r0 * 0.85f, x + ca * far - sa * w, y + sa * far + ca * w,
          x + ca * far + sa * w, y + sa * far - ca * w, FX_PAL[col]);
  }
}
// Speed lines trailing whatever is going along (dx, dy) at (x, y): tapered
// streaks, staggered, crawling at the cel rate.
static void scSpeed(const ScStage& P, float x, float y, float dx, float dy, float len, float spread, int n, uint8_t col) {
  const float d = fmaxf(0.001f, sqrtf(dx * dx + dy * dy)), ux = dx / d, uy = dy / d, nx = -uy, ny = ux;
  const uint32_t boil = P.live ? P.now / 70 : 0;
  for (int i = 0; i < n; i++) {
    uint32_t h = fxHash2((uint32_t)i, P.seed + 73), hb = fxHash2((uint32_t)i, boil + 5);
    float off = fxS(h) * spread, back = (0.1f + 0.5f * fxU(hb)) * len, l = len * (0.4f + 0.6f * fxU(h >> 8));
    float x0 = x - ux * back + nx * off, y0 = y - uy * back + ny * off;
    fxStroke(x0, y0, x0 - ux * l, y0 - uy * l, 0.5f + P.k * (0.4f + 0.6f * fxU(h >> 16)), 0.3f, FX_PAL[col]);
  }
}
// A five-point star.
static void scStar(float x, float y, float r, uint16_t col) {
  for (int i = 0; i < 5; i++) {
    float a = -1.5708f + i * (FX_TAU / 5);
    for (int sd = -1; sd <= 1; sd += 2)
      fxTri(x, y, x + cosf(a + sd * 0.63f) * r * 0.42f, y + sinf(a + sd * 0.63f) * r * 0.42f,
            x + cosf(a) * r, y + sinf(a) * r, col);
  }
}
// The startle: a "!" by the head, popped in big and settling, and shock
// ticks off the crown. `age` ms since it went up.
static void scStartle(float hx, float hy, float k, uint32_t age) {
  const float s = k * (age < 140 ? 1.4f : 1.0f), x = hx + 6 * k, y = hy - 7 * k;
  fxStroke(x, y - 7 * s, x, y - 2.6f * s, 2.6f * s, 1.4f * s, FX_PAL[FXP_INK]);
  fxDisc(x, y, 1.4f * s, FX_PAL[FXP_INK]);
  fxStroke(x, y - 7 * s, x, y - 2.6f * s, 1.6f * s, 0.8f * s, FX_PAL[FXP_GLOW]);
  fxDisc(x, y, 0.85f * s, FX_PAL[FXP_GLOW]);
  for (int i = 0; i < 3; i++) {
    float a = -2.7f + i * 0.5f, r0 = 4.6f * k, r1 = (age < 140 ? 8.5f : 7.0f) * k;
    fxStroke(hx + cosf(a) * r0, hy + sinf(a) * r0, hx + cosf(a) * r1, hy + sinf(a) * r1, 1.1f * k, 0.4f, FX_PAL[FXP_GLOW]);
  }
}
// The sweat drop: a bead with a point, sliding down beside a head.
static void scSweat(float hx, float hy, float k, uint32_t age, float side) {
  const float x = hx + side * 3.6f * k, y = hy - 2.5f * k + fminf(age * 0.004f, 3.0f) * k;
  fxDisc(x, y, 1.5f * k, FX_PAL[FXP_INK]);
  fxTri(x - 1.35f * k, y - 0.5f * k, x + 1.35f * k, y - 0.5f * k, x + side * 0.3f * k, y - 3.8f * k, FX_PAL[FXP_INK]);
  fxDisc(x, y, 0.95f * k, FX_PAL[FXP_GLOW]);
  fxTri(x - 0.8f * k, y - 0.4f * k, x + 0.8f * k, y - 0.4f * k, x + side * 0.25f * k, y - 2.9f * k, FX_PAL[FXP_GLOW]);
  scPx((int)(x - 0.4f * k), (int)(y - 0.3f * k), FX_PAL[FXP_WHITE]);
}
// Gloom: wavering vertical lines let down over a head that has just been
// let down itself. Ink with a rust edge, so they read over any sky.
static void scGloom(float hx, float hy, float k, uint32_t age, uint32_t now) {
  const float grow = fxClampF(age / 500.0f, 0.0f, 1.0f);
  for (int i = 0; i < 7; i++) {
    uint32_t h = fxHash2((uint32_t)i, 0x6100u);
    float x = hx + (i - 3) * 1.8f * k, top = hy - 15 * k, len = (7 + 8 * fxU(h)) * k * grow;
    for (int y = (int)top; y < (int)(top + len); y++) {
      int lx = (int)(x + sinf(y * 0.45f + now * 0.005f + i * 1.3f) * 0.6f);
      scPx(lx + 1, y, FX_PAL[FXP_RUST]);
      scPx(lx, y, FX_PAL[FXP_INK]);
    }
  }
}
// A sparkle that twinkles: fxSparkle's crossed diamonds, breathing.
static void scKira(float x, float y, float r, uint32_t now, uint32_t ph) {
  fxSparkle(x, y, r * (0.5f + 0.5f * sinf(now * 0.011f + ph * 1.9f)));
}
// Stars going round a knocked head.
static void scDizzy(float hx, float hy, float k, uint32_t now) {
  for (int i = 0; i < 3; i++) {
    float a = now * 0.008f + i * (FX_TAU / 3), s = sinf(a);
    scStar(hx + cosf(a) * 6.5f * k, hy - 5 * k + s * 1.8f * k, (s > 0 ? 2.2f : 1.5f) * k, FX_PAL[s > 0 ? FXP_GLOW : FXP_HDR]);
  }
}
// An afterimage, where a fast figure just was: its silhouette, flat, in `col`.
static void scGhost(ScFig F, float dx, float dy, uint8_t col) {
  F.x += dx; F.foot += dy; F.rim = col; F.lx = F.ly = 0;
  scFigureDraw(F, 0);
}
// An impact frame: the panel blown out flat for a beat, ink focus lines
// driving in at what was hit. Drawn before the figures, which stay ink.
static void scImpactFrame(const ScStage& P, float x, float y) {
  for (int yy = P.y0; yy < P.y1; yy++) scTone(yy, P.x0, P.x1, 13.5f);
  scFocus(P, x, y, 11 * P.k, FXP_INK, 2.6f * P.k, 36);
}
// A plus, rising off whoever was just patched up.
static void scPlus(float x, float y, float k) {
  fxRect((int)(x - 2.2f * k), (int)(y - 0.9f * k), (int)(x + 2.2f * k) + 1, (int)(y + 0.9f * k) + 1, FX_PAL[FXP_INK]);
  fxRect((int)(x - 0.9f * k), (int)(y - 2.2f * k), (int)(x + 0.9f * k) + 1, (int)(y + 2.2f * k) + 1, FX_PAL[FXP_INK]);
  fxRect((int)(x - 1.6f * k), (int)(y - 0.4f * k), (int)(x + 1.6f * k) + 1, (int)(y + 0.4f * k) + 1, FX_PAL[FXP_GLOW]);
  fxRect((int)(x - 0.4f * k), (int)(y - 1.6f * k), (int)(x + 0.4f * k) + 1, (int)(y + 1.6f * k) + 1, FX_PAL[FXP_GLOW]);
}

// The Creeping Doom, placed: `s` 0-1 is how much of it there is.
static void scDoomAt(float cx, float base, float s, uint8_t aware, uint32_t now);

// The panels that tell a story, each its own function: inlined into one
// scPanelArt() they made a function the Xtensa GCC cannot allocate (an ICE
// in postreload), so they stay out of line.
#if defined(_MSC_VER)
#define SC_NOINLINE __declspec(noinline)
#else
#define SC_NOINLINE __attribute__((noinline))
#endif

static SC_NOINLINE void scArtClash(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float L = (float)P.x0, R = (float)P.x1, T = (float)P.y0, G = P.gy, W = (float)(P.x1 - P.x0), k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // The place itself, hurting them. Indoors, under a slab with a hole
    // punched in it and a shaft of daylight standing in the dust: in they
    // come out of the dark doorway, stop in the light, look up -- and a
    // lump of the ceiling picks its moment. WHAM. Down on a knee, an arm
    // over the head, the bits still coming down; then up, and on, out past
    // the pillar. And the room waits for the next one.
    const uint32_t CYC = 4800, LOOK = 1100, CRACK = 1250, DROP = 1500, HIT = 1800, RISE = 3000, GONE = 4500;
    const uint32_t lt = t - P.t0, u = P.live ? lt % CYC : HIT + 90;
    const float hx = cx + 4 * k, slab = G - 50 * k, fx = hx - 1.5f * k;   // the hole, the slab's underside, where they stop
    const float lx0 = hx - 3.5f * k, ly0 = slab + 1.2f * k, lyH = G - 32 * k;   // the lump: where it hangs, where it lands
    // the room: dark, the shaft standing in it, a pool where the light lands
    for (int y = (int)T; y < (int)G; y++) scTone(y, (int)L, (int)R, 1.1f + 0.6f * (y - T) / (G - T));
    for (int y = (int)slab; y < (int)G; y++) {
      float v = (y - slab) / (G - slab);
      scTone(y, (int)(hx - 6 * k - 3 * k * v), (int)(hx + 5 * k + 8 * k * v), 5.7f - 1.5f * v);
    }
    for (int y = (int)G; y < P.y1; y++) {
      float v = (y - G) / fmaxf(1.0f, P.y1 - G);
      float hw = 12 * k * sqrtf(fmaxf(0.0f, 1.0f - v * v * 0.85f));
      scTone(y, (int)L, (int)R, 2.4f - v);
      scTone(y, (int)(hx + 2 * k - hw), (int)(hx + 2 * k + hw), 4.8f - 1.8f * v);
    }
    fxRect((int)(L + 3 * k), (int)(G - 31 * k), (int)(L + 13 * k), (int)G, FX_PAL[FXP_INK]);   // the way in
    fxLine((int)(L + 13 * k), (int)(G - 31 * k), (int)(L + 13 * k), (int)G - 1, FX_PAL[FXP_TRACK]);
    fxLine((int)(L + 3 * k), (int)(G - 31 * k), (int)(L + 13 * k), (int)(G - 31 * k), FX_PAL[FXP_TRACK]);
    scHeap(hx + 1 * k, G, 24 * k, 4 * k, P.seed, FXP_RUST);   // what came down before
    for (int i = 0; i < 9; i++) {                          // the dust, standing in the light
      uint32_t h = fxHash2((uint32_t)i, P.seed + 3);
      float v = fmodf(fxU(h) + (P.live ? t * 0.00011f : 0.3f) * (0.6f + 0.6f * fxU(h >> 4)), 1.0f);
      float y = slab + v * (G - slab), x = hx - 5 * k + fxU(h >> 8) * (10 + 7 * v) * k + sinf(v * 9 + i) * k;
      scPx((int)x, (int)y, FX_PAL[(h & 3) ? FXP_RUST : FXP_HDR]);
    }
    // the slab overhead, the hole through it, the sky through that
    fxRect((int)L, (int)T, (int)R, (int)slab, FX_PAL[FXP_INK]);
    for (int y = (int)T; y < (int)slab; y++) {
      float v = (y - T) / fmaxf(1.0f, slab - T), hw = (4.2f + 1.8f * v + 1.4f * fxNoise1(y * 0.3f, P.seed)) * k;
      scTone(y, (int)(hx - hw), (int)(hx + hw * 0.9f), 10.6f - 2.5f * v);
      scPx((int)(hx - hw) - 1, y, FX_PAL[FXP_HOT]);
    }
    fxLine((int)L, (int)slab, (int)(hx - 6 * k), (int)slab, FX_PAL[FXP_DIM]);   // its edge, what light gets to it
    fxLine((int)(hx + 5 * k), (int)slab, (int)R, (int)slab, FX_PAL[FXP_DIM]);
    for (int i = 0; i < 10; i++) {                         // its broken underside
      uint32_t h = fxHash2((uint32_t)i, P.seed + 9);
      float x = L + (i + fxU(h)) * W / 10;
      if (fabsf(x - hx) < 7 * k) continue;
      fxTri(x - 3 * k, slab - 1, x + 3 * k, slab - 1, x + fxS(h >> 4) * 2 * k, slab + (1 + 2.5f * fxU(h >> 8)) * k, FX_PAL[FXP_INK]);
    }
    fxStroke(hx + 4.5f * k, slab - 3 * k, hx + 1.5f * k, slab + 2 * k, 0.9f * k, 0.7f * k, FX_PAL[FXP_RUST]);   // rebar, hung in the hole
    fxStroke(hx - 5.5f * k, slab - 5 * k, hx - 3 * k, slab - 2 * k, 0.9f * k, 0.7f * k, FX_PAL[FXP_RUST]);
    // the lump: hanging off the lip, grit first, a crack; then down
    if (u < DROP || u >= GONE) {
      scLump(lx0, ly0, 0.2f, 3.6f * k, P.seed + 17, FXP_RUST);
      if (u >= CRACK && u < DROP && ((u / 140) & 1) == 0)
        fxLine((int)(lx0 - 4 * k), (int)(slab + 0.5f * k), (int)(lx0 + 4 * k), (int)(slab - 0.5f * k), FX_PAL[FXP_HOT]);
      if (u >= LOOK - 400 && u < DROP)
        for (int i = 0; i < 3; i++) {
          float v = fmodf((u - (LOOK - 400)) * 0.0021f + i / 3.0f, 1.0f);
          scPx((int)(lx0 + (i - 1) * 1.5f * k), (int)(ly0 + 3 * k + v * (G - ly0 - 3 * k)), FX_PAL[FXP_HDR]);
        }
    } else if (u < HIT) {
      float e = (u - DROP) / (float)(HIT - DROP), y = fxLerp(ly0, lyH, e * e), x = fxLerp(lx0, fx + 0.5f * k, e);
      for (int i = -1; i <= 1; i++)                        // how fast
        fxLine((int)(x + i * 2.2f * k), (int)(y - 5 * k), (int)(x + i * 2.2f * k), (int)(y - (5 + 12 * e) * k), FX_PAL[FXP_LINE]);
      scLump(x, y, 0.2f + 1.4f * e, 3.6f * k, P.seed + 17, FXP_RUST);
    }
    if (P.live && u >= HIT && u < HIT + 150) scImpactFrame(P, fx + 0.5f * k, lyH);   // the blow, as a cel
    // them
    if (u < GONE) {
      ScFig F = { fx, G, 1, arch, SCP_STAND, seat, 0, FXP_HDR, 0.25f, -0.97f, k };   // lit from overhead
      if (u < LOOK) { F.x = fxLerp(L + 9 * k, fx, u / (float)LOOK); F.pose = SCP_WALK; F.ph = u * 0.016f; }
      else if (u < HIT) F.look = 0.75f * fxClampF((u - LOOK) / 210.0f, 0, 1);
      else if (u < RISE) { F.pose = SCP_CROUCH; F.handU = 7.2f; F.handV = 19.8f; }   // an arm over the head
      else { F.pose = SCP_WALK; F.x = fxLerp(fx, R + 7 * k, (u - RISE) / (float)(GONE - RISE)); F.ph = (u - RISE) * 0.015f; }
      if (fabsf(F.x - hx) > 11 * k) F.rim = FXP_RUST;      // out of the shaft, only the edge of it
      for (int p = 0; p < 3; p++) scFigure(F, p);
      if (u >= HIT + 350 && u < RISE) {                    // and stars going round
        float qx, qy;
        scHeadAt(F, &qx, &qy);
        scDizzy(qx, qy, k, P.live ? t : 500);
      }
    }
    // the bits, and the dust off them
    if (u >= HIT && u < GONE) {
      const float dt = (float)(u - HIT), gk = 0.00042f * k, ix = fx + 0.5f * k;
      for (int i = 0; i < 4; i++) {
        uint32_t h = fxHash2((uint32_t)i, P.seed + 21);
        float vx = ((i & 1) ? 1.0f : -1.0f) * (0.01f + 0.028f * fxU(h)) * k, vy = -(0.008f + 0.03f * fxU(h >> 8)) * k;
        float s = (1.3f + 0.9f * fxU(h >> 16)) * k, fl = G - s * 0.6f;
        float tl = (-vy + sqrtf(vy * vy + 2 * gk * (fl - lyH))) / gk, tt = fminf(dt, tl);
        scLump(ix + vx * tt, lyH + vy * tt + 0.5f * gk * tt * tt, i * 1.3f + tt * 0.011f, s, h, FXP_RUST);
      }
      if (dt < 260)                                        // the blow itself
        for (int i = 0; i < 8; i++) {
          float a = i * (FX_TAU / 8) + 0.3f, r0 = (4 + dt * 0.02f) * k, r1 = r0 + 4 * k * (1 - dt / 260);
          fxLine((int)(ix + cosf(a) * r0), (int)(lyH + sinf(a) * r0), (int)(ix + cosf(a) * r1), (int)(lyH + sinf(a) * r1), FX_PAL[FXP_WHITE]);
        }
      fxDustDraw(ix, lyH, dt, 13 * k, P.seed + 5, 900.0f);
      fxDustDraw(hx, G - 1, dt - 380, 18 * k, P.seed + 6, 1100.0f);
    }
    fxRect((int)(R - 12 * k), (int)T, (int)(R - 5 * k), P.y1, FX_PAL[FXP_INK]);   // a pillar to go out past
    fxLine((int)(R - 12 * k), (int)slab, (int)(R - 12 * k), (int)G + 2, FX_PAL[FXP_TRACK]);
    if (u >= HIT && u < HIT + 1000) scBang("WHAM!", cx + 30 * k, T + (G - T) * 0.4f, 10 * k, P.seed, P.k > 1.3f ? 2 : 1);
    scJolt(P, lt, HIT, 0.22f);
}

static SC_NOINLINE void scArtTrip(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float L = (float)P.x0, R = (float)P.x1, T = (float)P.y0, G = P.gy, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // A tripwire, told small (ui-fx.hpp 13b is the close-up, and plays over
    // this as it lands, so this one has no knock of its own). A wire across
    // the path at the ankle; over the path, out on a gallows arm, a deadfall
    // -- a log, planks, a drum, lashed and hung. The boot finds the wire,
    // the pegs lean in, it parts -- SNAP -- they go over forward and it all
    // comes down where they were. A look back at it, up, gone; and the wire
    // is strung again for whoever is next.
    const uint32_t CYC = 5600, TOUCH = 1150, SNAP = 1450, DROP = 1500, KNEE = 1720, LAND = 1820,
                   BACK = 2500, RISE = 3300, GONE = 4700;
    const uint32_t u = P.live ? (t - P.t0) % CYC : SNAP + 170;
    const bool armed = u < SNAP || u >= GONE;
    const float wx = cx - 6 * k, wy = G - 3 * k, wa = wx - 6 * k, wb = wx + 6 * k;   // the wire, and its pegs
    const float gx = cx - 30 * k, armY = G - 54 * k, hx = cx - 4 * k;   // the gallows, and the drop under its arm
    const float xt = wx - 4.2f * k, xk = cx + 10 * k;      // where the shin meets the wire; where they land
    const float push = u < TOUCH || u >= GONE ? 0.0f : fxCubicOut(fxClampF((u - TOUCH) / (float)(SNAP - TOUCH), 0, 1));
    const float tipX = xt + (5 + 1.6f * push) * k, tipY = wy - 0.9f * push * k;
    if (P.live && u >= TOUCH + 80 && u < SNAP) scFocus(P, tipX, tipY, 9 * k, FXP_INK, 1.6f * k);   // the strain, as a page holds it
    // the gallows: a post, an arm out over the path, a brace
    fxStroke(gx, G + 1, gx, armY - 2 * k, 2.6f * k, 2.1f * k, FX_PAL[FXP_INK]);
    fxStroke(gx - 1 * k, armY, hx + 5 * k, armY + 0.6f * k, 2.1f * k, 1.6f * k, FX_PAL[FXP_INK]);
    fxStroke(gx, armY + 11 * k, gx + 9 * k, armY + 0.5f * k, 1.3f * k, 1.1f * k, FX_PAL[FXP_INK]);
    fxLine((int)(gx - 1.4f * k), (int)G, (int)(gx - 1.4f * k), (int)(armY - 1 * k), FX_PAL[P.rim]);
    fxLine((int)(gx - 1 * k), (int)(armY - 1.1f * k), (int)(hx + 5 * k), (int)(armY - 0.5f * k), FX_PAL[P.rim]);
    if (armed) {                                           // the line from the wire, up the post, out to the knot
      fxLine((int)wa, (int)wy, (int)(gx + 1.6f * k), (int)G - 1, FX_PAL[FXP_LINE]);
      fxLine((int)(gx + 1.6f * k), (int)G - 1, (int)(gx + 1.6f * k), (int)(armY + 1.4f * k), FX_PAL[FXP_LINE]);
      fxLine((int)(gx + 1.6f * k), (int)(armY + 1.4f * k), (int)hx, (int)(armY + 1.4f * k), FX_PAL[FXP_LINE]);
    }
    // the deadfall: hung, falling, or down and gone to pieces
    const float hang = armY + 16 * k;                      // its foot, while it hangs: over the Mule's pack
    if (!armed && u >= DROP) {                             // the cut end of its rope, swinging
      float a = (float)(u - DROP), sw = sinf(a * 0.011f) * 0.6f * expf(-a * 0.0011f);
      fxStroke(hx, armY + 1 * k, hx + sinf(sw) * 5 * k, armY + (1 + cosf(sw) * 5) * k, 0.9f * k, 0.7f * k, FX_PAL[FXP_INK]);
    } else {
      fxStroke(hx, armY + 1 * k, hx, hang - 12 * k, 0.9f * k, 0.9f * k, FX_PAL[FXP_INK]);
    }
    if (!armed && u >= LAND) {
      fxStroke(hx - 10 * k, G - 1.7f * k, hx + 8 * k, G - 1.6f * k, 3.4f * k, 3.2f * k, FX_PAL[FXP_INK]);   // the log
      fxLine((int)(hx - 10 * k), (int)(G - 3.4f * k), (int)(hx + 8 * k), (int)(G - 3.3f * k), FX_PAL[P.rim]);
      fxDisc(hx + 8 * k, G - 1.7f * k, 1.6f * k, FX_PAL[FXP_BRICK]);
      fxStroke(hx - 3 * k, G - 1, hx + 2 * k, G - 12 * k, 1.5f * k, 1.4f * k, FX_PAL[FXP_INK]);   // a plank, stood up in the dirt
      fxLine((int)(hx - 3.8f * k), (int)G - 2, (int)(hx + 1.2f * k), (int)(G - 12 * k), FX_PAL[P.rim]);
      fxStroke(hx - 12 * k, G - 4 * k, hx + 3 * k, G - 2.6f * k, 1.5f * k, 1.4f * k, FX_PAL[FXP_INK]);
      fxRect((int)(hx - 18 * k), (int)(G - 6 * k), (int)(hx - 10 * k), (int)G, FX_PAL[FXP_INK]);   // the drum, rolled off
      fxLine((int)(hx - 18 * k), (int)(G - 6 * k), (int)(hx - 10 * k), (int)(G - 6 * k), FX_PAL[P.rim]);
      fxLine((int)(hx - 15 * k), (int)(G - 6 * k), (int)(hx - 15 * k), (int)G - 1, FX_PAL[FXP_RUST]);
    } else {
      float b = hang;
      if (!armed && u >= DROP) {
        float e = (u - DROP) / (float)(LAND - DROP);
        b = hang + (G - hang) * e * e;
        for (int i = -1; i <= 1; i++)                      // how fast
          fxLine((int)(hx + i * 5 * k), (int)(b - 14 * k), (int)(hx + i * 5 * k), (int)(b - (14 + 14 * e) * k), FX_PAL[FXP_LINE]);
      }
      fxRect((int)(hx - 3 * k), (int)(b - 13 * k), (int)(hx + 3 * k), (int)(b - 6 * k), FX_PAL[FXP_INK]);   // a drum
      fxLine((int)(hx - 3 * k), (int)(b - 13 * k), (int)(hx + 3 * k), (int)(b - 13 * k), FX_PAL[P.rim]);
      fxLine((int)(hx - 3 * k), (int)(b - 10 * k), (int)(hx + 3 * k), (int)(b - 10 * k), FX_PAL[FXP_RUST]);
      fxStroke(hx - 8 * k, b - 9 * k, hx + 7 * k, b - 6 * k, 1.5f * k, 1.5f * k, FX_PAL[FXP_INK]);       // planks
      fxStroke(hx - 7 * k, b - 4.5f * k, hx + 8 * k, b - 10.5f * k, 1.5f * k, 1.5f * k, FX_PAL[FXP_INK]);
      fxStroke(hx - 9 * k, b - 2.6f * k, hx + 9 * k, b - 3.2f * k, 3.4f * k, 3.2f * k, FX_PAL[FXP_INK]); // the log
      fxLine((int)(hx - 9 * k), (int)(b - 4.3f * k), (int)(hx + 9 * k), (int)(b - 4.9f * k), FX_PAL[P.rim]);
      fxDisc(hx + 9 * k, b - 3.2f * k, 1.6f * k, FX_PAL[FXP_BRICK]);
      fxLine((int)(hx - 4 * k), (int)(b - 13 * k), (int)(hx - 4 * k), (int)(b - 1 * k), FX_PAL[FXP_RUST]);   // the lashing
      fxLine((int)(hx + 5 * k), (int)(b - 12 * k), (int)(hx + 5 * k), (int)(b - 1 * k), FX_PAL[FXP_RUST]);
    }
    for (int s = 0; s < 2; s++) {                          // the pegs, leaning in as it takes the strain
      float x = s ? wb : wa, lean = (s ? -0.8f : 0.8f) * push * k;
      fxStroke(x, G + 1, x + lean, wy - 1.2f * k, 1.3f * k, 1.0f * k, FX_PAL[FXP_INK]);
      scPx((int)(x + lean), (int)(wy - 1.2f * k), FX_PAL[FXP_GLOW]);
    }
    // them
    if (u < GONE) {
      ScFig F = { xt, G, 1, arch, SCP_WALK, seat, 1.5708f, P.rim, -0.7f, -0.7f, k };
      if (u < TOUCH) { F.x = fxLerp(L + 6 * k, xt, u / (float)TOUCH); F.ph = 1.5708f - (TOUCH - u) * 0.016f; }
      else if (u < SNAP) F.x = xt + 1.6f * k * push;     // stopped mid-stride, leaning into it
      else if (u < KNEE) { F.pose = SCP_SLUMP; F.x = fxLerp(xt + 1.6f * k, xk - 2 * k, (u - SNAP) / (float)(KNEE - SNAP)); }
      else if (u < RISE) {
        F.pose = SCP_CROUCH; F.x = xk;
        if (u >= BACK) { F.dir = -1; F.lx = 0.7f; }        // and a look back at it
      } else { F.x = fxLerp(xk, R + 8 * k, (u - RISE) / (float)(GONE - RISE)); F.ph = (u - RISE) * 0.016f; }
      if (u >= SNAP && u < KNEE) scGhost(F, -3 * k, 0, FXP_DIM);   // going over, fast
      for (int p = 0; p < 3; p++) scFigure(F, p);
      if (u >= SNAP && u < SNAP + 600) {
        float qx, qy;
        scHeadAt(F, &qx, &qy);
        scStartle(qx, qy, k, u - SNAP);
      }
    }
    // the wire, over the shin: taut and glinting, bowed, or parted
    if (u < TOUCH || u >= GONE) {
      fxLine((int)wa, (int)wy, (int)wb, (int)wy, FX_PAL[FXP_GLOW]);
      float g = fmodf((P.live ? t : 0) * 0.0007f, 1.6f);
      if (g < 1) fxLine((int)fxLerp(wa, wb, g), (int)wy, (int)fxLerp(wa, wb, g) + 1, (int)wy, FX_PAL[FXP_WHITE]);
    } else if (u < SNAP) {
      fxLine((int)wa, (int)wy, (int)tipX, (int)tipY, FX_PAL[FXP_WHITE]);
      fxLine((int)tipX, (int)tipY, (int)wb, (int)wy, FX_PAL[FXP_WHITE]);
      for (int i = 0; i < 4; i++) {                        // the strain, jumping
        uint32_t h = fxHash2((uint32_t)i, u / 70);
        float a = FX_TAU * fxU(h), r0 = (1.5f + fxU(h >> 8)) * k;
        fxLine((int)(tipX + cosf(a) * r0), (int)(tipY + sinf(a) * r0), (int)(tipX + cosf(a) * (r0 + 2 * k)), (int)(tipY + sinf(a) * (r0 + 2 * k)), FX_PAL[FXP_WHITE]);
      }
    } else {
      float a = (float)(u - SNAP), w = expf(-a / 160.0f);
      for (int s = 0; s < 2; s++) {                        // the halves whip back, ring, and hang
        float p0 = s ? wb : wa, d = s ? -1.0f : 1.0f;
        float len = (2.0f + 4.0f * w * fabsf(cosf(a * 0.035f))) * k;
        float ex = p0 + d * len, ey = wy + (1 - w) * 2.2f * k;
        float mx = (p0 + ex) * 0.5f, my = (wy + ey) * 0.5f + sinf(a * 0.05f + s) * w * 1.5f * k;
        fxLine((int)p0, (int)wy, (int)mx, (int)my, FX_PAL[FXP_GLOW]);
        fxLine((int)mx, (int)my, (int)ex, (int)ey, FX_PAL[FXP_GLOW]);
      }
      if (a < 200) {                                       // where it went
        for (int i = 0; i < 7; i++) {
          float an = i * (FX_TAU / 7) + 0.4f, r0 = (1.5f + a * 0.02f) * k, r1 = r0 + 3.5f * k * (1 - a / 200);
          fxLine((int)(tipX + cosf(an) * r0), (int)(tipY + sinf(an) * r0), (int)(tipX + cosf(an) * r1), (int)(tipY + sinf(an) * r1), FX_PAL[FXP_WHITE]);
        }
        fxDisc(tipX, tipY, 1.4f * k * (1 - a / 200), FX_PAL[FXP_WHITE]);
      }
    }
    if (!armed && u >= LAND) fxDustDraw(hx, G - 1, (float)(u - LAND), 17 * k, P.seed + 3, 1000.0f);
    if (!armed && u < SNAP + 1100) scBang("SNAP!", cx + 32 * k, T + (G - T) * 0.42f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);
}

static SC_NOINLINE void scArtGunfire(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float L = (float)P.x0, R = (float)P.x1, T = (float)P.y0, G = P.gy, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // Machine Gun Kane (docs/machine-gun-kane.md): the old M151 idling, the
    // gun bolted to its windshield frame and a cable run from its trigger to
    // the wheel, and him at the wheel, grinning. A burst walks its spurts up
    // the dirt to them, and the brass goes up off the gun in a fountain --
    // which is why it is everywhere. A hit (an ILL line) knocks them to a
    // knee, and they get up again; a miss (OMEN) is at night, in his lights,
    // with them already down and the rest of the burst going over.
    const bool hit = P.tone != SC_TONE_OMEN;
    const uint32_t CYC = 4400, SHOT0 = 300, GAP = 100, FLY = 170, HITN = 6, SHOTS = hit ? 10 : 13;
    const uint32_t HIT = SHOT0 + HITN * GAP + FLY, KNEE = HIT + 380, RISE = 3500, SHOT1 = SHOT0 + SHOTS * GAP;
    const uint32_t lt = t - P.t0, u = P.live ? lt % CYC : (hit ? HIT + 70 : SHOT0 + 9 * GAP + 60);
    const bool firing = u >= SHOT0 && u < SHOT1;
    const float jx = cx + 30 * k, figX = cx - 28 * k;
    const float bY = G - (firing ? fxU(fxHash2(u / 50, P.seed)) * 0.6f * k : (float)((u / 120) & 1) * 0.3f * k);   // the tub, shaking
    const float mx = jx - 18.8f * k, my = bY - 22 * k;     // the muzzle
    const float hlx = jx - 15.8f * k, hly = bY - 8.5f * k; // a headlight
    if (hit && P.live && u >= HIT && u < HIT + 150) scImpactFrame(P, figX + 1.5f * k, G - 18 * k);   // the one that finds them
    for (int i = 0; i < 14; i++) {                         // the brass, everywhere
      uint32_t h = fxHash2((uint32_t)i, P.seed + 31);
      int x = (int)(cx - 14 * k + fxU(h) * (R - cx + 12 * k)), y = (int)(G + fxU(h >> 8) * 2.5f * k);
      bool glint = P.live && fxHash2((uint32_t)i, t / 380) % 7 == 0;
      scPx(x, y, FX_PAL[glint ? FXP_WHITE : FXP_HDR]);
      if (k > 1.3f) scPx(x + 1, y, FX_PAL[FXP_RUST]);
    }
    for (int i = 0; i < 3; i++) {                          // the exhaust
      float p = fmodf((P.live ? t * 0.0011f : 0.4f) + i / 3.0f, 1.0f);
      fxDisc(jx + (16 + p * 9) * k, bY - (4 + p * 6) * k, (1 + p * 2.4f) * k, FX_PAL[p < 0.5f ? FXP_LINE : FXP_DIM]);
    }
    if (P.night)                                           // his lights, down the road at them
      for (int y = (int)(hly - (hlx - L) * 0.12f); y < P.y1; y++) {
        float dy = y - hly, xr = hlx - fabsf(dy) / (dy < 0 ? 0.12f : 0.22f);
        for (int x = (int)L; x < (int)xr; x++)
          if (scDot(x, y, 0.12f + 0.4f * (x - L) / fmaxf(1.0f, hlx - L))) scPx(x, y, FX_PAL[FXP_EMBER]);
      }
    // the jeep: the spare, the tub and the hood, the wheels
    fxDisc(jx + 15.2f * k, bY - 8.6f * k, 3.4f * k, FX_PAL[FXP_INK]);
    float qx[5] = { jx - 16 * k, jx - 15.6f * k, jx - 4 * k, jx + 15 * k, jx + 15 * k };
    float qy[5] = { bY - 4.6f * k, bY - 10 * k, bY - 12.6f * k, bY - 12.6f * k, bY - 4.6f * k };
    fxPoly(qx, qy, 5, FX_PAL[FXP_INK]);
    fxLine((int)(jx - 15.6f * k), (int)(bY - 10.6f * k), (int)(jx - 4 * k), (int)(bY - 13.2f * k), FX_PAL[P.rim]);
    fxLine((int)(jx - 4 * k), (int)(bY - 13.2f * k), (int)(jx + 15 * k), (int)(bY - 13.2f * k), FX_PAL[P.rim]);
    fxLine((int)(jx - 16.4f * k), (int)(bY - 10 * k), (int)(jx - 16.4f * k), (int)(bY - 5 * k), FX_PAL[P.rim]);
    if (k > 1.3f) {                                        // the white star, mostly gone
      const float sx = jx - 9 * k, sy = bY - 8 * k;
      for (int i = 0; i < 5; i++) {
        float a = -1.5708f + i * (FX_TAU / 5);
        for (int sd = -1; sd <= 1; sd += 2)
          fxTri(sx, sy, sx + cosf(a + sd * 0.63f) * 1.0f * k, sy + sinf(a + sd * 0.63f) * 1.0f * k,
                sx + cosf(a) * 2.6f * k, sy + sinf(a) * 2.6f * k, FX_PAL[FXP_DIM]);
      }
    }
    for (int s = 0; s < 2; s++) {
      float wx2 = jx + (s ? 9 : -10) * k;
      fxDisc(wx2, G - 4.3f * k, 4.4f * k, FX_PAL[FXP_INK]);
      fxDisc(wx2, G - 4.3f * k, 1.5f * k, FX_PAL[FXP_LINE]);
      fxDisc(wx2, G - 4.3f * k, 0.6f * k, FX_PAL[FXP_INK]);
      for (int i = 0; i < 5; i++) {                        // the tread, catching the light
        float a = 3.6f + i * 0.25f;
        scPx((int)(wx2 + cosf(a) * 4.6f * k), (int)(G - 4.3f * k + sinf(a) * 4.6f * k), FX_PAL[P.rim]);
      }
    }
    fxDisc(hlx, hly, 1.1f * k, FX_PAL[P.night ? FXP_WHITE : FXP_GLOW]);
    // the windshield frame, the gun bolted to it, the cable to the wheel
    fxStroke(jx - 3.6f * k, bY - 12.6f * k, jx - 4.6f * k, bY - 20.6f * k, 1.1f * k, 1.0f * k, FX_PAL[FXP_INK]);
    fxLine((int)(jx - 2.8f * k), (int)(bY - 13.4f * k), (int)(jx - 3.6f * k), (int)(bY - 19.8f * k), FX_PAL[FXP_LINE]);
    fxRect((int)(jx - 7.4f * k), (int)(bY - 23.2f * k), (int)(jx - 1.6f * k), (int)(bY - 20.6f * k), FX_PAL[FXP_INK]);
    fxStroke(jx - 7 * k, bY - 22 * k, mx + 0.6f * k, my, 1.1f * k, 0.9f * k, FX_PAL[FXP_INK]);
    fxRect((int)mx, (int)(my - 0.8f * k), (int)(mx + 1.4f * k), (int)(my + 0.8f * k), FX_PAL[FXP_INK]);
    fxLine((int)(jx - 7.4f * k), (int)(bY - 23.6f * k), (int)(jx - 1.6f * k), (int)(bY - 23.6f * k), FX_PAL[P.rim]);
    fxLine((int)mx, (int)(my - 0.9f * k), (int)(jx - 7.4f * k), (int)(bY - 22.9f * k), FX_PAL[P.rim]);
    fxRect((int)(jx - 5.4f * k), (int)(bY - 20.6f * k), (int)(jx - 2.6f * k), (int)(bY - 18.4f * k), FX_PAL[FXP_INK]);   // the ammo can
    fxLine((int)(jx - 2 * k), (int)(bY - 20.6f * k), (int)(jx - 1.2f * k), (int)(bY - 15.6f * k), FX_PAL[FXP_RUST]);
    // and him: a field cap, both hands on the wheel, and the grin that no longer closes
    fxStroke(jx + 2.6f * k, bY - 12.6f * k, jx + 1.8f * k, bY - 19.6f * k, 4.2f * k, 3.6f * k, FX_PAL[FXP_INK]);
    fxDisc(jx + 1.0f * k - 0.7f, bY - 22.6f * k - 0.7f, 2.4f * k + 0.8f, FX_PAL[P.rim]);
    fxDisc(jx + 1.0f * k, bY - 22.6f * k, 2.4f * k, FX_PAL[FXP_INK]);
    fxRect((int)(jx - 0.6f * k), (int)(bY - 26 * k), (int)(jx + 3 * k), (int)(bY - 23.8f * k), FX_PAL[FXP_INK]);
    fxStroke(jx + 0.2f * k, bY - 24 * k, jx - 2.6f * k, bY - 23.4f * k, 0.8f * k, 0.6f * k, FX_PAL[FXP_INK]);
    fxStroke(jx + 1.4f * k, bY - 18.6f * k, jx - 1.4f * k, bY - 15.4f * k, 1.5f * k, 1.2f * k, FX_PAL[FXP_INK]);
    fxStroke(jx - 2.8f * k, bY - 17.6f * k, jx - 0.2f * k, bY - 13.8f * k, 0.8f * k, 0.8f * k, FX_PAL[FXP_INK]);
    scPx((int)(jx - 0.4f * k), (int)(bY - 23.1f * k), FX_PAL[FXP_CRIT]);
    fxLine((int)(jx - 1.1f * k), (int)(bY - 21.5f * k), (int)(jx + 0.5f * k), (int)(bY - 21.2f * k), FX_PAL[FXP_WHITE]);
    // them
    {
      ScFig F = { figX, G, 1, arch, SCP_STAND, seat, 0, P.rim, -0.7f, -0.7f, k };
      if (!hit) {                                          // down already, in his lights
        F.pose = SCP_CROUCH; F.x = figX + 2 * k; F.handU = 7.2f; F.handV = 19.8f;
        F.rim = FXP_HDR; F.lx = 0.92f; F.ly = -0.4f;
      } else if (u >= HIT && u < KNEE) {
        F.pose = SCP_SLUMP; F.x = figX - 1.6f * k * fxCubicOut((u - HIT) / (float)(KNEE - HIT));
      } else if (u >= KNEE && u < RISE) {
        F.pose = SCP_CROUCH; F.x = figX - 1.6f * k;
      } else if (u >= RISE) {                              // up, and squared up to it again
        F.pose = u < RISE + 260 ? SCP_SLUMP : SCP_STAND;
        F.x = figX - 1.6f * k * (1 - fxClampF((u - RISE) / 700.0f, 0, 1));
      }
      for (int p = 0; p < 3; p++) scFigure(F, p);
      float qx, qy;
      scHeadAt(F, &qx, &qy);
      const uint32_t OVER = SHOT0 + 6 * GAP + 100;         // the first round over their head
      if (!hit && u >= OVER && u < OVER + 600) scStartle(qx, qy, k, u - OVER);
      if (!hit && u >= OVER) scSweat(qx, qy, k, u - OVER, -1);
      if (hit && u >= RISE + 260) scSweat(qx, qy, k, u - RISE - 260, -1);
    }
    // the burst: where each round goes, its tracer, and the dirt it throws up
    for (uint32_t i = 0; i < SHOTS; i++) {
      const uint32_t f = SHOT0 + i * GAP;
      if (u < f) break;
      const uint32_t h = fxHash2(i, P.seed + 13);
      float tx, ty = G + 1;
      bool ground = true;
      if (hit && i == HITN)     { tx = figX + 1.5f * k; ty = G - 18 * k; ground = false; }   // the one that finds them
      else if (hit && i > HITN) tx = figX + (6 + 3.0f * (i - HITN)) * k + fxS(h) * k;      // then the dirt in front of them
      else if (!hit && i >= 6)  { tx = L - 8 * k; ty = G - (22 + 5 * fxU(h)) * k; ground = false; }   // over, close
      else tx = fxLerp(cx + 8 * k, figX + (hit ? 7 : 13) * k, i / (hit ? (float)HITN : 5.0f)) + fxS(h) * 1.5f * k;
      const float age = (float)(u - f);
      if (age < FLY) {
        float q = age / FLY, x = fxLerp(mx, tx, q), y = fxLerp(my, ty, q);
        float dx = mx - tx, dy = my - ty, d = fmaxf(1.0f, sqrtf(dx * dx + dy * dy)), tl = fminf(13 * k, d * q);
        fxStroke(x + dx / d * tl, y + dy / d * tl, x, y, 0.5f, 0.6f + 0.7f * k, FX_PAL[FXP_HOT]);
        fxLine((int)(x + dx / d * tl * 0.4f), (int)(y + dy / d * tl * 0.4f), (int)x, (int)y, FX_PAL[FXP_WHITE]);
      } else if (ground && age < FLY + 340) {
        const float a2 = (age - FLY) / 340.0f, e = fxCubicOut(a2);
        for (int j = 0; j < 4; j++) {
          const uint32_t hj = fxHash2((uint32_t)j, h);
          const float an = -1.5708f + (j - 1.5f) * 0.42f + fxS(hj) * 0.15f, ln = (3.5f + 7 * fxU(hj >> 8)) * k * e;
          fxStroke(tx + cosf(an) * ln * 0.35f, G + sinf(an) * ln * 0.35f, tx + cosf(an) * ln, G + sinf(an) * ln + a2 * a2 * 6 * k,
                   0.5f + 1.4f * k * (1 - a2), 0.5f, FX_PAL[(j & 1) ? FXP_LINE : FXP_TRACK]);
        }
        if (a2 < 0.55f) fxBlob(tx, G - 1.6f * k * e, 1, 0, (1.8f + 3.4f * e) * k, (1.1f + 1.8f * e) * k, h, FX_PAL[FXP_DIM]);
      }
    }
    if (hit && u >= HIT && u < HIT + 460) {                // where it found them
      const float a = (float)(u - HIT), bx = figX + 1.5f * k, by = G - 18 * k;
      if (a < 120)
        for (int i = 0; i < 6; i++) {
          float an = i * (FX_TAU / 6) + 0.5f;
          fxLine((int)(bx + cosf(an) * 1.5f * k), (int)(by + sinf(an) * 1.5f * k), (int)(bx + cosf(an) * 4.5f * k), (int)(by + sinf(an) * 4.5f * k), FX_PAL[FXP_WHITE]);
        }
      for (int i = 0; i < 6; i++) {
        uint32_t h = fxHash2((uint32_t)i, P.seed + 41);
        float vx = -(0.012f + 0.03f * fxU(h)) * k, vy = (fxS(h >> 8) * 0.02f - 0.008f) * k;
        fxDisc(bx + vx * a, fminf(G - 0.5f, by + vy * a + 0.00025f * k * a * a), (0.5f + 0.35f * fxU(h >> 16)) * k, FX_PAL[FXP_BLOOD]);
      }
    }
    if (hit && u >= KNEE && u < RISE)                      // and it runs
      for (int i = 0; i < 2; i++) {
        float ph2 = fmodf((u - KNEE) * 0.0011f + i * 0.5f, 1.0f);
        fxDisc(figX + 1.4f * k + i * 2, G - 9 * k + ph2 * 9 * k, 1.2f, FX_PAL[FXP_BLOOD]);
      }
    if (firing && (!P.live || fxHash2(u / 70, P.seed) % 5 != 0)) {   // the flash, most frames of the burst
      for (int i = 0; i < 5; i++) {
        const float a = 3.14159f + (i - 2) * 0.5f + fxS(fxHash2((uint32_t)i, u / 70)) * 0.12f, r = (i == 2 ? 7.0f : 3.8f) * k;
        fxTri(mx, my - 0.9f * k, mx, my + 0.9f * k, mx + cosf(a) * r, my + sinf(a) * r, FX_PAL[FXP_GLOW]);
      }
      fxDisc(mx - 0.6f * k, my, 1.7f * k, FX_PAL[FXP_WHITE]);
    }
    if (u >= SHOT0 && u < SHOT1 + 300)                           // and the brass going up off it
      for (int i = 0; i < 5; i++) {
        float p = fmodf((u - SHOT0) * 0.0032f + i / 5.0f, 1.0f);
        const float x0 = jx - 2.4f * k, y0 = bY - 22 * k;
        const float x = x0 + p * 11 * k, y = y0 - 32 * k * p * (1 - p) + (G - y0) * p * p;
        scPx((int)x, (int)y, FX_PAL[FXP_HDR]);
        scPx((int)x + ((int)(p * 9) & 1), (int)y - 1, FX_PAL[FXP_GLOW]);
      }
    if (u >= SHOT0 && u < SHOT1 + 250)
      scBang("BRRRAT", jx - 2 * k, T + (G - T) * (P.k > 1.3f ? 0.33f : 0.26f), 9 * k, P.seed, P.k > 1.3f ? 2 : 1);   // over the gun
    scJolt(P, lt, hit ? HIT : SHOT0, 0.18f);
}

static SC_NOINLINE void scArtForage(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float L = (float)P.x0, R = (float)P.x1, T = (float)P.y0, G = P.gy, W = (float)(P.x1 - P.x0), k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // Foraging dead ground: down on a knee, digging, the dirt going back over
    // the shoulder. A good line (GOOD) pulls up a root worth having -- SHLUK,
    // the startle -- and holds it up to the light with the page closing in
    // on it, sparkling. A bad one (ILL) turns a stone over on nothing: the
    // dots, the sweat drop, the gloom, a tumbleweed going by, and the belly
    // having its say.
    const bool ok = P.tone != SC_TONE_ILL;
    const uint32_t CYC = 5200, DUG = 1700, HELD = DUG + 300, SHOW = HELD + 1400, FLIPPED = DUG + 400;
    const uint32_t u = P.live ? (t - P.t0) % CYC : (ok ? HELD + 400 : FLIPPED + 1100);
    const float fx = cx - 10 * k, dx0 = fx + 10.4f * k;    // where they kneel; where they dig
    const float sway = P.live ? sinf(t * 0.004f) : 0.3f;
    for (int i = 0; i < 6; i++) {                          // the scrub, what there is of it
      uint32_t h = fxHash2((uint32_t)i, P.seed + 2);
      float x = L + 6 * k + fxU(h) * (W - 12 * k), hh = (3 + 4 * fxU(h >> 8)) * k;
      if (fabsf(x - dx0) < 7 * k) continue;
      for (int j = -1; j <= 1; j++)
        fxLine((int)x, (int)G, (int)(x + j * 1.5f * k + sway * k), (int)(G - hh * (j ? 0.7f : 1.0f)), FX_PAL[FXP_INK]);
    }
    const float hole = fminf(1.0f, u / (float)DUG);
    fxBlob(dx0, G + 0.5f, 1, 0, (1.5f + 3 * hole) * k, (0.6f + 0.8f * hole) * k, P.seed, FX_PAL[FXP_INK]);
    if (u < DUG)                                           // the dirt, going back over the shoulder
      for (int i = 0; i < 3; i++) {
        float p = fmodf(u / 330.0f + i / 3.0f, 1.0f);
        float x = dx0 - p * (15 + 5 * i) * k, y = G - 2 - sinf(p * 3.14159f) * (8 + 3 * i) * k;
        fxDisc(x, y, (0.9f + 0.4f * i) * k, FX_PAL[FXP_INK]);
        scPx((int)(x - 0.5f * k), (int)(y - 0.6f * k), FX_PAL[FXP_RUST]);
      }
    ScFig F = { fx, G, 1, arch, SCP_CROUCH, seat, 0, P.rim, -0.7f, -0.7f, k };
    if (u < DUG) {                                         // dig, dig
      float pump = fabsf(sinf(u * 0.0095f));
      F.handU = 10.4f - 1.2f * pump; F.handV = 1.4f + 4.4f * pump;
    }
    float hx, hy;
    if (ok) {
      const float rx = fx + 3 * k + 8.5f * k, ry = G - 24 * k;   // held up, out in front of the face
      if (u >= DUG && u < HELD) {
        float e = fxCubicOut((u - DUG) / (float)(HELD - DUG));
        F.handU = 10.4f - 3.4f * e; F.handV = 1.4f + 17 * e;
      } else if (u >= HELD && u < SHOW) {
        F.pose = SCP_STAND; F.x = fx + 3 * k; F.handU = 8.5f; F.handV = 24.0f; F.look = 0.25f;
        scFocus(P, rx, ry + 4 * k, 12 * k, FXP_INK, 2.2f * k);   // and the page closes in on it
      } else if (u >= SHOW) {
        F.pose = SCP_STAND; F.x = fx + 3 * k; F.handU = 8.0f; F.handV = 17.0f; F.look = -0.25f;
      }
      for (int p = 0; p < 3; p++) scFigure(F, p);
      scHeadAt(F, &hx, &hy);
      if (u >= DUG) {                                      // the root, hung from the hand
        const float x = scHandX, y = scHandY;
        for (int j = -1; j <= 1; j++)                      // its greens
          fxStroke(x, y, x + j * 2.6f * k, y - (3.5f + (j == 0 ? 2 : 0)) * k, 1.1f * k, 0.4f * k, FX_PAL[FXP_INK]);
        fxBlob(x - 0.8f, y + 4.2f * k - 1.0f, 0.2f, 0.98f, 4.2f * k, 2.5f * k, P.seed + 9, FX_PAL[P.rim]);
        fxBlob(x, y + 4.2f * k, 0.2f, 0.98f, 4.0f * k, 2.3f * k, P.seed + 9, FX_PAL[FXP_INK]);
        scPx((int)(x - 1 * k), (int)(y + 2.6f * k), FX_PAL[FXP_HDR]);
        for (int j = 0; j < 3; j++)                        // and its roots
          fxLine((int)(x + (j - 1) * 0.8f * k), (int)(y + 7.8f * k), (int)(x + (j - 1) * 1.8f * k), (int)(y + (10 + j) * k), FX_PAL[FXP_INK]);
        if (u < HELD + 500)                                // still shedding its dirt
          for (int j = 0; j < 3; j++) {
            float p = fmodf((u - DUG) * 0.003f + j / 3.0f, 1.0f);
            scPx((int)(x - 2 * k + j * k), (int)(y + 6 * k + p * (G - y - 6 * k)), FX_PAL[FXP_INK]);
          }
        if (u < HELD) scSpeed(P, x, y + 5 * k, 0, -1, 10 * k, 2.5f * k, 5, FXP_LINE);
        else
          for (int j = 0; j < 3; j++) scKira(x + (j - 1) * 6 * k, y + (j == 1 ? -4.0f : 3.0f) * k, 4.2f * k, P.live ? t : 300, j);
      }
      if (u >= DUG && u < DUG + 600) scStartle(hx, hy, k, u - DUG);
      if (u >= DUG && u < DUG + 800) scBang("SHLUK!", cx + 30 * k, T + (G - T) * 0.38f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);
    } else {
      // the stone: turned over in the hand, then left lying
      float sx = dx0 + 1 * k, sy = G - 1.2f * k, sa = 0;
      if (u >= DUG) {
        float e = fminf(1.0f, (u - DUG) / (float)(FLIPPED - DUG));
        sx = fxLerp(dx0 + 1 * k, dx0 + 5 * k, e); sy = G - 1.2f * k - sinf(e * 3.14159f) * 5 * k; sa = e * 3.14159f;
        if (u < FLIPPED) scHandTo(F, sx - 1.5f * k, sy - 1 * k);
        fxBlob(dx0 + 1 * k, G, 1, 0, 2.6f * k, 0.8f * k, P.seed + 4, FX_PAL[FXP_SOOT]);   // where it lay: nothing
        if (u >= FLIPPED && u < FLIPPED + 1100)            // nothing but a beetle, leaving
          scPx((int)(dx0 + 1 * k + (u - FLIPPED) * 0.012f * k), (int)G, FX_PAL[FXP_HDR]);
      }
      fxBlob(sx - 0.7f, sy - 0.6f * k - 0.9f, cosf(sa), sinf(sa), 3.0f * k, 1.9f * k, P.seed + 6, FX_PAL[P.rim]);
      fxBlob(sx, sy - 0.6f * k, cosf(sa), sinf(sa), 2.9f * k, 1.8f * k, P.seed + 6, FX_PAL[FXP_INK]);
      if (u >= FLIPPED + 1200) F.look = -0.85f;            // and the head goes down
      for (int p = 0; p < 3; p++) scFigure(F, p);
      scHeadAt(F, &hx, &hy);
      if (u >= FLIPPED + 200 && u < FLIPPED + 1500)
        scBalloon("...", (int)(hx + 12 * k), (int)(hy - 5 * k), (int)(hx + 4 * k), (int)(hy - 2 * k));   // off to the side, clear of the gloom
      if (u >= FLIPPED + 500) scSweat(hx, hy, k, u - FLIPPED - 500, -1);
      if (u >= FLIPPED + 1200) scGloom(hx, hy, k, u - FLIPPED - 1200, t);
      if (u >= FLIPPED + 900 && u < FLIPPED + 2900) {      // and the wind brings a tumbleweed by
        float e = (u - FLIPPED - 900) / 2000.0f, x = fxLerp(R + 6 * k, L - 8 * k, e), r = 3.4f * k;
        float y = G - r - fabsf(sinf(e * 14)) * 4 * k, a = -e * 30;
        for (int j = 0; j < 9; j++) {
          float a0 = a + j * 0.7f, a1 = a0 + 2.1f;
          fxLine((int)(x + cosf(a0) * r), (int)(y + sinf(a0) * r), (int)(x + cosf(a1) * r * 0.8f), (int)(y + sinf(a1) * r * 0.8f), FX_PAL[FXP_INK]);
        }
        scSpeed(P, x + r, y, -1, 0, 6 * k, r * 0.8f, 3, FXP_LINE);
      }
      if (u >= FLIPPED + 2000 && u < FLIPPED + 3000) {     // the belly has its say
        int jx = P.live ? (int)(fxS(fxHash(t / 70)) * 1.5f) : 0;
        scType("GRMBL", (int)(fx + 11 * k) + jx, (int)(G - 14 * k), 1, FXP_GLOW);
      }
    }
}

static SC_NOINLINE void scArtWater(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float T = (float)P.y0, G = P.gy, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // At a hand pump, working the handle, the pail under the spout. A good
    // line (GOOD) has it cough and then come -- GUSH -- into the pail till
    // it brims, a shine on it and a wipe of the brow. A bad one (ILL) coughs
    // dust -- pff -- and one drop, plip, into the empty pail; then the
    // handle worked harder and harder for nothing, the dots, the gloom.
    const bool ok = P.tone != SC_TONE_ILL;
    const uint32_t CYC = 5200, COME = 1500, FULL = COME + 1800, PLIP = 1900, FRANTIC = 2300, QUIT = 3000;
    const uint32_t u = P.live ? (t - P.t0) % CYC : (ok ? COME + 900 : QUIT + 200);
    const float px = cx + 10 * k, pvx = px - 2.2f * k, pvy = G - 21 * k;   // the pump; its handle's pivot
    const float sx = px + 7.5f * k, sy = G - 12 * k;       // the spout's mouth; the pail stands under it
    const float fx = cx - 12 * k;                          // where they stand to work it
    // the handle: steady strokes, then (a bad line) frantic ones, then let go
    float sw;
    if (ok)               sw = u < FULL ? sinf(u * (FX_TAU / 520.0f)) : 0.9f;
    else if (u < FRANTIC) sw = sinf(u * (FX_TAU / 520.0f));
    else if (u < QUIT)    sw = sinf(u * (FX_TAU / 260.0f));
    else                  sw = 0.9f + 0.1f * expf(-(float)(u - QUIT) * 0.004f) * sinf((u - QUIT) * 0.03f);   // let go, it rocks and settles
    const float la = 0.38f * sw, gx = pvx - cosf(la) * 13 * k, gy = pvy + sinf(la) * 13 * k;   // the grip
    const float lvl = ok ? fxClampF((u - COME) / (float)(FULL - COME), 0.0f, 1.0f) : 0.0f;
    // the pump: a pad, the barrel and its cap, the spout
    fxRect((int)(px - 5 * k), (int)(G - 1.5f * k), (int)(px + 5 * k), (int)G + 1, FX_PAL[FXP_INK]);
    fxRect((int)(px - 2 * k), (int)(G - 20 * k), (int)(px + 2 * k), (int)(G - 1.5f * k), FX_PAL[FXP_INK]);
    fxRect((int)(px - 2.7f * k), (int)(G - 21.6f * k), (int)(px + 2.7f * k), (int)(G - 19.6f * k), FX_PAL[FXP_INK]);
    fxStroke(px + 1.5f * k, G - 15 * k, px + 7 * k, G - 14 * k, 1.8f * k, 1.4f * k, FX_PAL[FXP_INK]);
    fxStroke(px + 7 * k, G - 14 * k, sx, sy, 1.4f * k, 1.3f * k, FX_PAL[FXP_INK]);
    fxLine((int)(px - 2.6f * k), (int)(G - 19.6f * k), (int)(px - 2.6f * k), (int)(G - 2 * k), FX_PAL[P.rim]);
    fxLine((int)(px - 2.7f * k), (int)(G - 22.2f * k), (int)(px + 2.7f * k), (int)(G - 22.2f * k), FX_PAL[P.rim]);
    fxLine((int)(px + 1.5f * k), (int)(G - 15.9f * k), (int)(px + 7 * k), (int)(G - 14.9f * k), FX_PAL[P.rim]);
    // the pail, and what is in it
    {
      const float bx = sx, by = G - 2.5f * k;
      float qx[4] = { bx - 3 * k, bx + 3 * k, bx + 2.3f * k, bx - 2.3f * k }, qy[4] = { by - 2.5f * k, by - 2.5f * k, by + 2.5f * k, by + 2.5f * k };
      fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
      fxLine((int)(bx - 3 * k), (int)(by - 2.6f * k), (int)(bx + 3 * k), (int)(by - 2.6f * k), FX_PAL[P.rim]);
      fxLine((int)(bx - 2.7f * k), (int)(by + 0.3f * k), (int)(bx + 2.7f * k), (int)(by + 0.3f * k), FX_PAL[FXP_RUST]);
      fxLine((int)(bx - 3 * k), (int)(by - 2.5f * k), (int)bx, (int)(by - 5.5f * k), FX_PAL[FXP_INK]);   // its bail
      fxLine((int)(bx + 3 * k), (int)(by - 2.5f * k), (int)bx, (int)(by - 5.5f * k), FX_PAL[FXP_INK]);
      if (lvl > 0) {                                       // the water, coming up it
        float yl = by + 2.2f * k - lvl * 4.6f * k, hw = fxLerp(2.0f, 2.8f, lvl) * k;
        fxLine((int)(bx - hw), (int)yl, (int)(bx + hw), (int)yl, FX_PAL[FXP_GLOW]);
        scPx((int)(bx - hw * 0.4f), (int)yl, FX_PAL[FXP_WHITE]);
        if (lvl >= 1.0f) fxLine((int)(bx - hw), (int)yl - 1, (int)(bx + hw), (int)yl - 1, FX_PAL[FXP_HDR]);
      }
    }
    // the handle, over its pivot
    fxStroke(gx, gy, pvx + cosf(la) * 2.6f * k, pvy - sinf(la) * 2.6f * k, 1.2f * k, 1.0f * k, FX_PAL[FXP_INK]);
    fxDisc(pvx, pvy, 0.9f * k, FX_PAL[FXP_INK]);
    scPx((int)pvx, (int)pvy, FX_PAL[FXP_LINE]);
    // them
    ScFig F = { fx, G, 1, arch, SCP_STAND, seat, 0, P.rim, -0.7f, -0.7f, k };
    const bool working = ok ? u < FULL : u < QUIT;
    if (working) scHandTo(F, gx + 0.5f * k, gy);
    if (!ok && u >= QUIT + 600) { F.pose = SCP_SLUMP; F.look = -0.85f; }
    float hx, hy;
    scHeadAt(F, &hx, &hy);
    if (ok && u >= FULL + 200 && u < FULL + 1100) scHandTo(F, hx + 1.5f * k, hy - 0.4f * k);   // the brow
    for (int p = 0; p < 3; p++) scFigure(F, p);
    if (working && sw < -0.8f) scType("kii", (int)(pvx - 3 * k), (int)(pvy - 9 * k), 1, FXP_HDR);    // it squeals at the top
    if (ok) {
      if (u >= COME - 350 && u < COME) fxDustDraw(sx, sy, (float)(u - COME + 350), 3 * k, P.seed + 7, 350.0f);   // a cough first
      if (u >= COME && u < FULL + 250) {                   // then it comes
        const float yl = G - 0.3f * k - lvl * 4.6f * k, wob = P.live ? sinf(t * 0.03f) * 0.5f * k : 0;
        fxStroke(sx, sy, sx + wob, yl, 2.2f * k, 1.4f * k, FX_PAL[FXP_GLOW]);
        fxLine((int)sx, (int)sy, (int)(sx + wob), (int)yl, FX_PAL[FXP_WHITE]);
        for (int i = 0; i < 4; i++) {                      // splashing out of the pail
          float p = fmodf((P.live ? t : 0) * 0.005f + i / 4.0f, 1.0f), d = (i & 1) ? 1.0f : -1.0f;
          fxDisc(sx + d * (1 + 3 * p) * k, yl - sinf(p * 3.14159f) * 3.5f * k, 0.6f * k, FX_PAL[FXP_WHITE]);
        }
        scSpeed(P, sx, sy + 4 * k, 0, 1, 5 * k, 1.5f * k, 3, FXP_WHITE);
      }
      if (u >= COME && u < COME + 800) scBang("GUSH!", cx + 32 * k, T + (G - T) * 0.36f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);
      if (u >= FULL - 300)
        for (int j = 0; j < 2; j++) scKira(sx + (j ? 3.5f : -3.0f) * k, G - (7 + 3 * j) * k, 3.4f * k, P.live ? t : 300, j);
      if (u >= FULL + 200 && u < FULL + 1100) {            // phew
        float p = (u - FULL - 200) / 900.0f;
        fxDisc(hx - 3 * k - p * 5 * k, hy - 2 * k - sinf(p * 3.14159f) * 4 * k + p * 4 * k, 0.8f * k, FX_PAL[FXP_GLOW]);
      }
    } else {
      if (u >= COME && u < COME + 700) {                   // it coughs dust
        fxDustDraw(sx, sy, (float)(u - COME), 6 * k, P.seed + 7, 700.0f);
        scType("pff", (int)(sx + 3 * k), (int)(sy - 8 * k), 1, FXP_GLOW);
      }
      if (u >= PLIP - 200 && u < PLIP + 200) {             // and one drop
        float e = fmaxf(0.0f, (u - PLIP + 200) / 400.0f - 0.3f) / 0.7f;
        fxDisc(sx, fxLerp(sy + 0.8f * k, G - 0.6f * k, e * e), 0.7f * k, FX_PAL[FXP_GLOW]);
      }
      if (u >= PLIP + 200 && u < PLIP + 900) scType("plip", (int)(sx + 4 * k), (int)(G - 9 * k), 1, FXP_GLOW);
      if (u >= FRANTIC && u < QUIT) {                      // harder, harder
        for (int i = 0; i < 3; i++) {
          uint32_t h = fxHash2((uint32_t)i, u / 80);
          float a = FX_TAU * fxU(h);
          fxLine((int)(gx + cosf(a) * 2.5f * k), (int)(gy + sinf(a) * 2.5f * k), (int)(gx + cosf(a) * 4.5f * k), (int)(gy + sinf(a) * 4.5f * k), FX_PAL[FXP_WHITE]);
        }
        scSweat(hx, hy, k, u - FRANTIC, -1);
      }
      if (u >= QUIT && u < QUIT + 1300)
        scBalloon("...", (int)(hx + 12 * k), (int)(hy - 5 * k), (int)(hx + 4 * k), (int)(hy - 2 * k));   // off to the side, clear of the gloom
      if (u >= QUIT + 600) scGloom(hx, hy, k, u - QUIT - 600, t);
    }
}

static SC_NOINLINE void scArtMedic(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float G = P.gy, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // Patching up: the roll going round and round, the white closing over
    // the red. A good line (GOOD) ties it off with a shine and a plus going
    // up, and a wipe of the brow; a bad one (ILL) runs out of roll -- the
    // core goes bouncing off -- and the red comes back through, hands
    // pressing, sweat flying, the gloom. On someone else (who2) the patient
    // lies there beside them; on themselves, they sit and do their own leg.
    const bool ok = P.tone != SC_TONE_ILL, self = seat2 == 255;
    const uint32_t CYC = 5000, WRAPPED = 2000, TIED = WRAPPED + 300;
    const uint32_t u = P.live ? (t - P.t0) % CYC : (ok ? TIED + 300 : WRAPPED + 800);
    const float mx = self ? cx - 8 * k : cx - 10 * k;      // the one doing it
    float wx, wy, ddx, ddy;                                // the wound, and which way the limb runs through it
    if (self) { wx = mx + 7 * k; wy = G - 3.9f * k; ddx = 0.548f; ddy = 0.837f; }
    else      { wx = mx + 6 * k; wy = G - 3 * k;    ddx = 1.0f;   ddy = 0.0f;   }
    const float nx = -ddy, ny = ddx;                       // across it
    float bx = cx - 26 * k;                                // the kit, lid up
    fxRect((int)bx - 1, (int)(G - 7 * k) - 1, (int)(bx + 9 * k) + 1, (int)G + 1, FX_PAL[FXP_INK]);
    fxRect((int)bx, (int)(G - 7 * k), (int)(bx + 9 * k), (int)G, FX_PAL[FXP_GLOW]);
    int kmx = (int)(bx + 4.5f * k), kmy = (int)(G - 3.5f * k);
    fxRect(kmx - 1, kmy - 3, kmx + 1, kmy + 3, FX_PAL[FXP_BLOOD]);
    fxRect(kmx - 3, kmy - 1, kmx + 3, kmy + 1, FX_PAL[FXP_BLOOD]);
    if (!self) {
      ScFig D = { mx + 10 * k, G, -1, arch2, SCP_LIE, seat2, 0, P.rim, 0.7f, -0.7f, k };
      for (int p = 0; p < 3; p++) scFigure(D, p);
    }
    ScFig F = { mx, G, 1, arch, self ? SCP_SIT : SCP_CROUCH, seat, 0, P.rim, -0.7f, -0.7f, k };
    const float th = fminf((float)u, (float)WRAPPED) * 0.013f;   // the roll, going round
    float rx = wx + nx * cosf(th) * 2.4f * k + ddx * sinf(th) * 0.6f * k, ry = wy + ny * cosf(th) * 2.4f * k + ddy * sinf(th) * 0.6f * k;
    if (u < WRAPPED) scHandTo(F, rx, ry);
    else if (ok && u < TIED) { float e = (u - WRAPPED) / 300.0f; scHandTo(F, wx + nx * 3.2f * k * sinf(e * 3.14159f), wy + ny * 3.2f * k * sinf(e * 3.14159f)); }
    else if (!ok && u < WRAPPED + 1600) {                  // pressing on it, hard and fast
      float pr = P.live ? fabsf(sinf(u * 0.017f)) : 0.5f;
      scHandTo(F, wx + nx * (0.6f + pr) * k, wy + ny * (0.6f + pr) * k);
    }
    float hx, hy;
    scHeadAt(F, &hx, &hy);
    if (ok && u >= TIED + 600 && u < TIED + 1500) scHandTo(F, hx + 1.5f * k, hy - 0.5f * k);   // the brow
    if (!ok && u >= WRAPPED + 1600) F.look = -0.85f;
    for (int p = 0; p < 3; p++) scFigure(F, p);
    // the wound, and the white going over it, band by band
    const int bands = (int)fminf(6.0f, u / 300.0f);
    fxDisc(wx, wy, 1.4f * k, FX_PAL[FXP_BLOOD]);
    for (int i = 0; i < bands; i++) {
      float ox = wx + ddx * (i - 2.5f) * 0.75f * k, oy = wy + ddy * (i - 2.5f) * 0.75f * k;
      fxStroke(ox - nx * 1.5f * k, oy - ny * 1.5f * k, ox + nx * 1.5f * k, oy + ny * 1.5f * k, 1.1f * k, 1.1f * k, FX_PAL[FXP_WHITE]);
      if (!ok && u >= WRAPPED + 300 && i < (int)((u - WRAPPED - 300) / 160))   // and the red back through it
        fxDisc(ox + nx * 0.3f * k, oy + ny * 0.3f * k, 0.7f * k, FX_PAL[FXP_BLOOD]);
    }
    if (u < WRAPPED) {                                     // the roll, and its tail to the wrap
      fxLine((int)rx, (int)ry, (int)(wx + ddx * (bands - 2.5f) * 0.75f * k), (int)(wy + ddy * (bands - 2.5f) * 0.75f * k), FX_PAL[FXP_WHITE]);
      fxDisc(rx, ry, 1.4f * k, FX_PAL[FXP_INK]);
      fxDisc(rx, ry, 1.0f * k, FX_PAL[sinf(th) > 0 ? FXP_WHITE : FXP_LINE]);
    }
    if (ok) {
      if (u >= WRAPPED) {
        fxDisc(wx + ddx * 3 * k, wy + ddy * 3 * k - 0.6f * k, 0.7f * k, FX_PAL[FXP_WHITE]);   // the knot
        fxLine((int)(wx + ddx * 3 * k), (int)(wy + ddy * 3 * k), (int)(wx + ddx * 4.4f * k + nx * k), (int)(wy + ddy * 4.4f * k + ny * k), FX_PAL[FXP_WHITE]);
      }
      if (u >= WRAPPED + 100 && u < TIED + 1200) scKira(wx + nx * 2 * k, wy + ny * 2 * k - 2 * k, 3.6f * k, P.live ? t : 300, 1);
      if (u >= TIED)                                       // and a plus going up off them
        for (int i = 0; i < 2; i++) {
          float a = (float)(u - TIED) - i * 450.0f;
          if (a < 0 || a > 1400) continue;
          float ox = self ? hx - 5 * k : mx + 15 * k, oy = self ? hy - 4 * k : G - 8 * k;
          fxStip = (uint8_t)fxClampF((a - 900) / 500.0f * 255, 0, 255);
          scPlus(ox + i * 5 * k + sinf(a * 0.006f) * k, oy - a * 0.012f * k, k * 1.6f);
          fxStip = 0;
        }
      if (u >= TIED + 600 && u < TIED + 1500) {            // phew
        float p = (u - TIED - 600) / 900.0f;
        fxDisc(hx - 3 * k - p * 5 * k, hy - 2 * k - sinf(p * 3.14159f) * 4 * k + p * 4 * k, 0.8f * k, FX_PAL[FXP_GLOW]);
      }
    } else {
      if (u >= WRAPPED && u < WRAPPED + 1100) {            // the roll's run out: its core, off and away
        float a = (float)(u - WRAPPED), x = wx + 3 * k + a * 0.02f * k, y = G - 1.2f * k - fabsf(sinf(a * 0.012f)) * 3 * k * (1 - a / 1100);
        fxDisc(x, y, 1.2f * k, FX_PAL[FXP_INK]);
        fxDisc(x, y, 0.7f * k, FX_PAL[FXP_LINE]);
      }
      if (u >= WRAPPED && u < WRAPPED + 600) scStartle(hx, hy, k, u - WRAPPED);
      if (u >= WRAPPED + 300 && u < WRAPPED + 1600) {
        for (int i = 0; i < 3; i++) {                      // the hands going, going
          uint32_t h = fxHash2((uint32_t)i, u / 90);
          float a = FX_TAU * fxU(h);
          fxLine((int)(scHandX + cosf(a) * 2.2f * k), (int)(scHandY + sinf(a) * 2.2f * k), (int)(scHandX + cosf(a) * 4 * k), (int)(scHandY + sinf(a) * 4 * k), FX_PAL[FXP_WHITE]);
        }
        for (int s = -1; s <= 1; s += 2) {                 // the sweat, flying off
          float p = fmodf((u - WRAPPED) * 0.0025f + (s > 0 ? 0.5f : 0), 1.0f);
          scSweat(hx + s * p * 5 * k, hy - 3 * k - sinf(p * 3.14159f) * 4 * k + p * 5 * k, k * 0.8f, 0, (float)s);
        }
      }
      if (u >= WRAPPED + 400)                              // and it drips
        for (int i = 0; i < 2; i++) {
          float p = fmodf((u - WRAPPED) * 0.0012f + i * 0.5f, 1.0f);
          fxDisc(wx + i * k, wy + 1.4f * k + p * (G - wy), 0.6f * k, FX_PAL[FXP_BLOOD]);
        }
      if (u >= WRAPPED + 1600) scGloom(hx, hy, k, u - WRAPPED - 1600, t);
    }
}

static SC_NOINLINE void scArtSalvage(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float G = P.gy, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // Into the wreck: a car on its roof in a heap of everything else. Down
    // in it to the elbows, a cloud of dust and the junk going back over the
    // shoulder -- a tyre, a can, a boot, a hubcap, a spring, a pipe. A good
    // line (GOOD) comes up with a cache -- the startle, held up with the page
    // closing in, then the lid up and the light coming out of it. A bad one
    // (ILL) comes up with one bolt, cuts a hand on it -- OW -- and throws it
    // after the rest, and hangs its head.
    const bool ok = P.tone != SC_TONE_ILL;
    const uint32_t CYC = 5400, PULL = 1900, OPEN = PULL + 1600;
    const uint32_t u = P.live ? (t - P.t0) % CYC : (ok ? PULL + 500 : PULL + 600);
    const float hx0 = cx + 12 * k, dx0 = hx0 - 14 * k, fx = dx0 - 8 * k;   // the heap; where they dig; where they kneel
    // the car, on its roof, and the heap it is in
    {
      float qx[4] = { hx0 - 4 * k, hx0 + 22 * k, hx0 + 22.5f * k, hx0 - 4 * k };
      float qy[4] = { G - 14 * k, G - 16 * k, G - 10 * k, G - 8.5f * k };
      fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
      fxLine((int)(hx0 - 4 * k), (int)(G - 14.6f * k), (int)(hx0 + 22 * k), (int)(G - 16.6f * k), FX_PAL[P.rim]);
      for (int s = 0; s < 2; s++) {                         // wheels to the sky
        float wx2 = hx0 + (s ? 17 : 1) * k, wy2 = G - (s ? 17.6f : 16.2f) * k;
        float spin = P.live ? t * 0.002f : 0;
        fxDisc(wx2, wy2, 3 * k, FX_PAL[FXP_INK]);
        fxDisc(wx2, wy2, 1.1f * k, FX_PAL[FXP_LINE]);
        scPx((int)(wx2 + cosf(spin + s) * 2.2f * k), (int)(wy2 + sinf(spin + s) * 2.2f * k), FX_PAL[P.rim]);
      }
      scHeap(hx0, G, 46 * k, 13 * k, P.seed, P.rim);
      fxStroke(hx0 + 6 * k, G - 10 * k, hx0 + 9 * k, G - 18 * k, 1.0f * k, 0.8f * k, FX_PAL[FXP_INK]);   // rebar
    }
    // the junk, going back over the shoulder, and where it lands
    auto junk = [&](int kind, float x, float y, float a) {
      const float ca = cosf(a), sa = sinf(a);
      switch (kind % 6) {
        case 0: fxDisc(x, y, 2.4f * k, FX_PAL[FXP_INK]); fxDisc(x, y, 1.0f * k, FX_PAL[FXP_LINE]); break;   // a tyre
        case 1: fxStroke(x - ca * 1.7f * k, y - sa * 1.7f * k, x + ca * 1.7f * k, y + sa * 1.7f * k, 2.4f * k, 2.4f * k, FX_PAL[FXP_INK]);
                scPx((int)x, (int)y, FX_PAL[FXP_RUST]); break;                                                  // a can
        case 2: fxStroke(x, y, x - sa * 3 * k, y + ca * 3 * k, 1.4f * k, 1.4f * k, FX_PAL[FXP_INK]);              // a boot
                fxStroke(x - sa * 3 * k, y + ca * 3 * k, x - sa * 3 * k + ca * 2.4f * k, y + ca * 3 * k + sa * 2.4f * k, 1.5f * k, 1.2f * k, FX_PAL[FXP_INK]); break;
        case 3: fxBlob(x, y, ca, sa, 2.3f * k, 0.9f * k, 5, FX_PAL[FXP_HDR]); scPx((int)x, (int)y, FX_PAL[FXP_WHITE]); break;   // a hubcap
        case 4: for (int i = 0; i < 4; i++) {                                                                  // a spring
                  // +k, -k, +k...: the sign from integer bits, because a select between two float
                  // constants here gave the Xtensa GCC an internal compiler error (postreload)
                  const float z = (float)(((i & 1) << 1) - 1) * k;
                  fxLine((int)(x + ca * (i - 2) * k - sa * z), (int)(y + sa * (i - 2) * k + ca * z),
                         (int)(x + ca * (i - 1) * k + sa * z), (int)(y + sa * (i - 1) * k - ca * z), FX_PAL[FXP_LINE]);
                }
                break;
        default: fxStroke(x - ca * 2.6f * k, y - sa * 2.6f * k, x + ca * 2.6f * k, y + sa * 2.6f * k, 1.0f * k, 1.0f * k, FX_PAL[FXP_INK]); break;   // a pipe
      }
    };
    for (int i = 0; i < 6; i++) {
      const uint32_t f = 150 + i * 290;
      if (u < f) break;
      const float a = (float)(u - f), e = fminf(1.0f, a / 440.0f);
      const float lx = fx - (6 + 3.5f * i) * k, ly = G - 1.2f * k;
      junk(i, fxLerp(dx0, lx, e), fxLerp(G - 7 * k, ly, e) - sinf(e * 3.14159f) * 20 * k, e < 1 ? a * 0.02f : 1.3f * i);
    }
    // them
    ScFig F = { fx, G, 1, arch, SCP_CROUCH, seat, 0, P.rim, -0.7f, -0.7f, k };
    if (u < PULL) { float pump = fabsf(sinf(u * 0.011f)); scHandTo(F, dx0 - 2 * k + pump * 2 * k, G - 4.5f * k + pump * 1.5f * k); }
    float hx, hy;
    if (ok) {
      const float cxh = fx + 2 * k + (arch == 3 ? 5.2f : 2.9f) * k, cyh = G - 29.6f * k;   // held up: REACH's near hand
      if (u >= PULL && u < OPEN) {
        F.pose = SCP_REACH; F.x = fx + 2 * k;
        if (u >= PULL + 150) scFocus(P, cxh, cyh + 1 * k, 13 * k, FXP_INK, 1.8f * k);
      } else if (u >= OPEN) { F.pose = SCP_STAND; F.x = fx + 2 * k; F.handU = 6.0f; F.handV = 14.0f; F.look = -0.3f; }
      for (int p = 0; p < 3; p++) scFigure(F, p);
      scHeadAt(F, &hx, &hy);
      if (u >= PULL) {                                     // the cache: an ammo can, held up
        const float x = scHandX, y = scHandY - (u < OPEN ? 1.5f * k : 0);
        fxRect((int)(x - 3.4f * k), (int)(y - 2.4f * k), (int)(x + 3.4f * k), (int)(y + 2.4f * k), FX_PAL[FXP_INK]);
        fxLine((int)(x - 3.4f * k), (int)(y - 2.6f * k), (int)(x + 3.4f * k), (int)(y - 2.6f * k), FX_PAL[P.rim]);
        fxLine((int)(x - 1.2f * k), (int)(y - 3.4f * k), (int)(x + 1.2f * k), (int)(y - 3.4f * k), FX_PAL[FXP_INK]);   // its handle
        fxLine((int)(x - 2.4f * k), (int)y, (int)(x + 2.4f * k), (int)y, FX_PAL[FXP_HDR]);   // its stencil, what's left
        if (u < OPEN)
          for (int j = 0; j < 3; j++) scKira(x + (j - 1) * 6 * k, y - (j == 1 ? 6.0f : 2.0f) * k, 3.4f * k, P.live ? t : 300, j);
        else {                                             // the lid up, and the light coming out of it
          fxStroke(x - 3.4f * k, y - 2.6f * k, x - 4.4f * k, y - 8 * k, 0.9f * k, 0.9f * k, FX_PAL[FXP_INK]);
          for (int j = 0; j < 5; j++) {
            float fl = P.live ? 0.8f + 0.2f * sinf(t * 0.02f + j) : 1.0f, a = -1.5708f + (j - 2) * 0.3f;
            fxTri(x - 2.6f * k + j * 1.3f * k, y - 2.6f * k, x - 2.6f * k + j * 1.3f * k + 0.6f * k, y - 2.6f * k,
                  x + cosf(a) * 13 * k * fl, y - 2.6f * k + sinf(a) * 13 * k * fl, FX_PAL[(j & 1) ? FXP_HDR : FXP_GLOW]);
          }
          for (int j = 0; j < 3; j++) {
            float p = fmodf((P.live ? t : 0) * 0.0008f + j / 3.0f, 1.0f);
            scKira(x + (j - 1) * 4 * k, y - 4 * k - p * 12 * k, 2.4f * k * (1 - p), P.live ? t : 300, j);
          }
        }
      }
      if (u >= PULL && u < PULL + 600) scStartle(hx, hy, k, u - PULL);
    } else {
      const uint32_t OW = PULL + 400, TOSS = PULL + 1000;
      if (u >= PULL) {
        F.pose = u >= TOSS + 300 ? SCP_SLUMP : SCP_STAND; F.x = fx + 2 * k;
        float hx2, hy2;
        scHeadAt(F, &hx2, &hy2);
        float jit = (u >= OW && u < OW + 500 && P.live) ? fxS(fxHash(u / 60)) * 1.2f * k : 0;
        if (u < TOSS) scHandTo(F, hx2 + 5 * k + jit, hy2 + 1.5f * k);
        else if (u < TOSS + 300) { float e = (u - TOSS) / 300.0f; scHandTo(F, hx2 + (5 - 9 * e) * k, hy2 - 3 * k * sinf(e * 3.14159f)); }
        else { F.handU = 4.0f; F.handV = 12.5f; }
      }
      if (u >= TOSS + 300) F.look = -0.85f;
      for (int p = 0; p < 3; p++) scFigure(F, p);
      scHeadAt(F, &hx, &hy);
      if (u >= PULL && u < TOSS) {                         // one bolt
        fxRect((int)(scHandX - 0.5f * k), (int)(scHandY - 2.4f * k), (int)(scHandX + 0.5f * k) + 1, (int)scHandY, FX_PAL[FXP_LINE]);
        fxRect((int)(scHandX - 1.1f * k), (int)(scHandY - 2.8f * k), (int)(scHandX + 1.1f * k) + 1, (int)(scHandY - 2.0f * k), FX_PAL[FXP_LINE]);
        if (u < PULL + 300) scKira(scHandX + 2 * k, scHandY - 4 * k, 2.6f * k, P.live ? t : 300, 0);
      }
      if (u >= TOSS && u < TOSS + 700) {                   // and after the rest of it
        float e = (u - TOSS) / 700.0f;
        float x = fxLerp(scHandX, fx - 14 * k, e), y = fxLerp(scHandY, G - 1, e) - sinf(e * 3.14159f) * 10 * k;
        fxRect((int)(x - 0.5f * k), (int)(y - 1.2f * k), (int)(x + 0.5f * k) + 1, (int)(y + 1.2f * k), FX_PAL[FXP_LINE]);
      }
      if (u >= OW && u < OW + 600) {                       // OW
        for (int i = 0; i < 3; i++) {
          float a = -2.4f + i * 0.8f;
          fxLine((int)(scHandX + cosf(a) * 2.5f * k), (int)(scHandY + sinf(a) * 2.5f * k), (int)(scHandX + cosf(a) * 5 * k), (int)(scHandY + sinf(a) * 5 * k), FX_PAL[FXP_WHITE]);
        }
        scBang("OW!", scHandX + 12 * k, scHandY - 6 * k, 7 * k, P.seed, P.k > 1.3f ? 2 : 1);
      }
      if (u >= OW)                                         // and the hand drips
        for (int i = 0; i < 2; i++) {
          float p = fmodf((u - OW) * 0.0014f + i * 0.5f, 1.0f);
          fxDisc(scHandX, scHandY + 1 * k + p * (G - scHandY - 1 * k), 0.55f * k, FX_PAL[FXP_BLOOD]);
        }
      if (u >= OW + 300) scSweat(hx, hy, k, u - OW - 300, -1);
      if (u >= TOSS + 300) scGloom(hx, hy, k, u - TOSS - 300, t);
    }
    if (u < PULL) {                                        // the dig cloud, over the arms, boiling: a manga fight cloud, inked
      for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 6; i++) {
          uint32_t h = fxHash2((uint32_t)i, P.live ? u / 100 : 3);
          float g = pass ? 0.0f : 0.9f * k;
          fxBlob(dx0 + 1 * k + fxS(h) * 5 * k, G - 6 * k + fxS(h >> 8) * 3 * k, 1, 0, (3.2f + 2.4f * fxU(h >> 16)) * k + g,
                 (2.6f + 1.6f * fxU(h >> 4)) * k + g, h, FX_PAL[pass ? ((i & 1) ? FXP_HDR : FXP_GLOW) : FXP_INK]);
        }
      for (int i = 0; i < 4; i++) {                        // and what flies out of it
        uint32_t h = fxHash2((uint32_t)i, P.live ? u / 100 + 7 : 5);
        float an = FX_TAU * fxU(h >> 12);
        fxStroke(dx0 + cosf(an) * 6 * k, G - 5 * k + sinf(an) * 4.5f * k, dx0 + cosf(an) * 9 * k, G - 5 * k + sinf(an) * 7 * k, 1.0f * k, 0.3f, FX_PAL[FXP_INK]);
      }
    }
}

static SC_NOINLINE void scArtShelter(const ScStage& P, uint8_t arch, uint8_t arch2, uint8_t seat, uint8_t seat2) {
  const float L = (float)P.x0, R = (float)P.x1, T = (float)P.y0, G = P.gy, W = (float)(P.x1 - P.x0), k = P.k, cx = P.cx;
  const uint32_t t = P.now;
    // A lean-to, put up against the wind -- and the wind is in every frame
    // of it. Driving the short pole, then the sheet carried over from the
    // tall one and pulled down tight: FWAP. A good line (GOOD) goes under it
    // and sits, a fire going, the wind going over the top and none of it
    // getting in. A bad one (ILL) has the gust take it -- WHOOSH -- the
    // sheet gone up and away over the page, the tall pole down after it,
    // and them reaching for it, and then just standing there.
    const bool ok = P.tone != SC_TONE_ILL;
    const uint32_t CYC = 5400, DRIVEN = 1300, TAUT = 1900, UNDER = TAUT + 800;
    const uint32_t u = P.live ? (t - P.t0) % CYC : (ok ? UNDER + 500 : TAUT + 400);
    const float ax = cx - 4 * k, bx = cx + 24 * k, aTop = G - 26 * k, bTop = G - 12 * k;   // the poles
    const bool gone = !ok && u >= TAUT;
    const float tt = P.live ? (float)t : 1500.0f;
    // the pole that goes over, and where the sheet is
    float aAng = 0;
    if (gone && u >= TAUT + 200) aAng = -1.35f * fxCubicIn(fminf(1.0f, (u - TAUT - 200) / 350.0f));
    const float aTx = ax + sinf(aAng) * 26 * k, aTy = G - cosf(aAng) * 26 * k;
    auto inShelter = [&](float x, float y) {
      if (gone || u < TAUT || x < ax || x > bx) return false;
      return y > fxLerp(aTop, bTop, (x - ax) / (bx - ax));
    };
    // the wind, in every frame: streaks and grit going left, none of it under the sheet
    for (int i = 0; i < 14; i++) {
      uint32_t h = fxHash2((uint32_t)i, P.seed + 61);
      float span = W + 40 * k, x = R + 20 * k - fmodf(fxU(h) * span + tt * (0.12f + 0.08f * fxU(h >> 4)) * k, span);
      float y = T + (0.3f + 0.68f * fxU(h >> 8)) * (G - T), len = (6 + 10 * fxU(h >> 12)) * k;
      if (inShelter(x, y) || inShelter(x + len, y)) continue;
      fxStroke(x, y, x + len, y, 0.4f + 0.4f * k, 0.3f, FX_PAL[ok ? ((h & 3) ? FXP_SOOT : FXP_DIM) : ((h & 3) ? FXP_HDR : FXP_RUST)]);
    }
    for (int i = 0; i < 10; i++) {
      uint32_t h = fxHash2((uint32_t)i, P.seed + 63);
      float span = W + 20 * k, x = R + 10 * k - fmodf(fxU(h) * span + tt * 0.3f * k, span), y = G - (0.5f + 3 * fxU(h >> 8)) * k;
      if (!inShelter(x, y)) scPx((int)x, (int)y, FX_PAL[ok ? FXP_INK : FXP_HDR]);
    }
    if (gone && u < TAUT + 700) scSpeed(P, cx - 8 * k, G - 20 * k, -1, 0, 30 * k, 18 * k, 16, FXP_WHITE);   // the gust
    // the poles
    fxStroke(ax, G, aTx, aTy, 1.8f * k, 1.6f * k, FX_PAL[FXP_INK]);
    fxStroke(bx, G, bx, bTop, 1.8f * k, 1.6f * k, FX_PAL[FXP_INK]);
    fxLine((int)(ax - 1 * k), (int)G, (int)(aTx - 1 * k), (int)aTy, FX_PAL[P.rim]);
    if (gone && u >= TAUT + 550) fxDustDraw(aTx, G - 1, (float)(u - TAUT - 550), 9 * k, P.seed + 5, 700.0f);
    // the sheet: bunched at the foot of the tall pole, carried, tight, or flying
    float sway = (fxNoise1(tt * 0.004f, 6) - 0.5f) * 3 * k;
    float fx2 = bx, fy2 = bTop;                            // its free end
    if (u < DRIVEN) {
      fxBlob(ax + 3 * k, G - 1.5f * k, 1, 0, 4 * k, 1.8f * k, P.seed + 3, FX_PAL[FXP_INK]);
      fxLine((int)(ax), (int)(G - 3.2f * k), (int)(ax + 6 * k), (int)(G - 2.8f * k), FX_PAL[P.rim]);
    } else if (!gone) {
      if (u < TAUT) { float e = (u - DRIVEN) / (float)(TAUT - DRIVEN); fx2 = fxLerp(ax + 4 * k, bx + 2 * k, e); fy2 = G - 20 * k + (bTop - G + 20 * k) * e; sway *= 2.5f; }
      else sway *= (u < TAUT + 200) ? 0.3f : 0.4f;
      float mxs = (ax + fx2) * 0.5f, mys = (aTop + fy2) * 0.5f + 1.5f * k + sway;
      float qx[4] = { ax - 4 * k, mxs, mxs, ax - 4 * k }, qy[4] = { aTop - 1 * k, mys - 1.2f * k, mys + 1.2f * k, aTop + 2 * k };
      fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
      float rx2[4] = { mxs, fx2 + 3 * k, fx2 + 3 * k, mxs }, ry2[4] = { mys - 1.2f * k, fy2 - 0.4f * k, fy2 + 1.6f * k, mys + 1.2f * k };
      fxPoly(rx2, ry2, 4, FX_PAL[FXP_INK]);
      fxLine((int)(ax - 4 * k), (int)(aTop - 1.6f * k), (int)mxs, (int)(mys - 1.8f * k), FX_PAL[P.rim]);
      fxLine((int)mxs, (int)(mys - 1.8f * k), (int)(fx2 + 3 * k), (int)(fy2 - 1.0f * k), FX_PAL[P.rim]);
      if (ok && u >= TAUT)                                 // the wind, going over the top of it
        for (int i = 0; i < 3; i++) {
          float p = fmodf(tt * 0.0016f + i / 3.0f, 1.0f), x = fxLerp(bx + 8 * k, ax - 6 * k, p), y = fxLerp(bTop - 2 * k, aTop - 3 * k, p) - (i - 1) * 2 * k;
          fxStroke(x, y, x + 7 * k, y + 7 * k * (bTop - aTop) / (bx - ax), 0.9f * k, 0.3f, FX_PAL[FXP_LINE]);
        }
    } else if (u < TAUT + 1100) {                          // gone, tumbling up and away over the page
      float e = (u - TAUT) / 1100.0f, s = 1 - 0.7f * e, a = e * 7.0f;
      float x = fxLerp(cx + 10 * k, L - 6 * k, e), y = fxLerp(G - 20 * k, T + 6 * k, fxCubicOut(e));
      float qx[4], qy[4];
      for (int i = 0; i < 4; i++) {
        float ca2 = cosf(a + i * 1.5708f), sa2 = sinf(a + i * 1.5708f) * 0.45f;
        qx[i] = x + ca2 * 12 * k * s; qy[i] = y + sa2 * 12 * k * s;
      }
      scSpeed(P, x + 8 * k * s, y, -1, -0.4f, 14 * k, 5 * k * s, 6, FXP_LINE);
      fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
      fxLine((int)qx[0], (int)qy[0], (int)qx[1], (int)qy[1], FX_PAL[P.rim]);
    } else {                                               // a speck, still going
      float e = fminf(1.0f, (u - TAUT - 1100) / 2400.0f), x = L + (5 + 4 * e) * k, y = T + (5 - 2 * e) * k + sinf(tt * 0.01f) * k;
      fxLine((int)(x - 1.5f * k), (int)(y - 0.5f * k), (int)(x + 1.5f * k), (int)(y + 0.5f * k), FX_PAL[FXP_INK]);
    }
    // the fire they get, under it
    if (ok && u >= UNDER) scFire(cx + 2 * k, G - 1, P.live ? t : 1500, k * 0.45f);
    // them
    ScFig F = { bx - 7.5f * k, G, 1, arch, SCP_STAND, seat, 0, P.rim, -0.7f, -0.7f, k };
    float hx, hy;
    if (u < DRIVEN) {                                      // driving the short pole
      float p = (u % 420) / 420.0f, raise = p < 0.75f ? fxSmooth(p / 0.75f) : 1 - fxCubicIn((p - 0.75f) / 0.25f);
      scHandTo(F, fxLerp(bx - 1.5f * k, F.x + 2.5f * k, raise), fxLerp(bTop - 1.5f * k, G - 26 * k, raise));
    } else if (u < TAUT) {                                 // carrying the sheet over
      F.pose = SCP_WALK; F.x = fxLerp(ax + 2 * k, bx + 2 * k, (u - DRIVEN) / (float)(TAUT - DRIVEN)); F.ph = u * 0.016f;
      scHandTo(F, fx2 + 1 * k, fy2 + 0.5f * k);
    } else if (ok) {
      if (u < TAUT + 400) { F.x = bx + 3 * k; F.dir = -1; F.lx = 0.7f; scHandTo(F, bx + 0.5f * k, bTop); }
      else if (u < UNDER) { F.pose = SCP_WALK; F.dir = -1; F.lx = 0.7f; F.x = fxLerp(bx + 3 * k, cx + 12 * k, (u - TAUT - 400) / 400.0f); F.ph = u * 0.016f; }
      else { F.pose = SCP_SIT; F.dir = -1; F.lx = -0.95f; F.ly = -0.3f; F.x = cx + 12 * k; F.rim = FXP_FLAME; }   // by the fire, lit by it
    } else {
      F.x = bx + 3 * k; F.dir = -1; F.lx = 0.7f;
      if (u < TAUT + 1000) F.pose = SCP_REACH;            // after it
      else if (u >= TAUT + 1400) F.look = -0.8f;
    }
    if (u >= DRIVEN && u < TAUT) scGhost(F, -2.5f * k, 0, FXP_DIM);
    for (int p = 0; p < 3; p++) scFigure(F, p);
    scHeadAt(F, &hx, &hy);
    if (u < DRIVEN) {                                      // the hammer, and what it says
      float p = (u % 420) / 420.0f, ex = scHandX + 2.5f * k, ey = scHandY - 1.5f * k;
      fxStroke(scHandX, scHandY, ex, ey, 0.9f * k, 0.8f * k, FX_PAL[FXP_INK]);
      fxRect((int)(ex - 1.5f * k), (int)(ey - 1.2f * k), (int)(ex + 1.5f * k), (int)(ey + 1.2f * k), FX_PAL[FXP_INK]);
      if (p < 0.2f) scType("TOK", (int)(bx + 3 * k), (int)(bTop - 6 * k), 1, FXP_GLOW);
    }
    if (u >= TAUT && u < TAUT + 700) {
      if (ok) scBang("FWAP!", cx - 22 * k, T + (G - T) * 0.32f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);   // clear of them
      else    scBang("WHOOSH", cx - 24 * k, T + (G - T) * 0.55f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);
    }
    if (ok && u >= TAUT && u < TAUT + 800) scKira((ax + bx) * 0.5f, (aTop + bTop) * 0.5f - 2 * k, 3.6f * k, P.live ? t : 300, 0);
    if (ok && u >= UNDER + 400) {                          // ahh
      float p = fmodf((u - UNDER - 400) * 0.0006f, 1.0f);
      fxStip = (uint8_t)(p * 230);
      fxDisc(hx - (3 + p * 4) * k, hy - (1 + p * 6) * k, (0.8f + p * 2) * k, FX_PAL[FXP_LINE]);
      fxStip = 0;
    }
    if (gone && u < TAUT + 600) scStartle(hx, hy, k, u - TAUT);
    if (gone && u >= TAUT + 1000) scSweat(hx, hy, k, u - TAUT - 1000, 1);
    if (gone && u >= TAUT + 1400) scGloom(hx, hy, k, u - TAUT - 1400, t);
}

// ── Sleeping rough ──
// Three nights with no roof, each told from lights-out to the morning after,
// and then laughed at. They are drawn on a stage like every panel here, so
// the book's picture (SC_GLY_ROUGH*) and the cut scene cued with it (section
// 12b, FXK_ROUGH) are one drawing at two sizes. `u` is ms into the story; the
// last frame holds once it is told. `cut` says the cut scene is doing the
// laughing, bigger, so the panel's own HA's stay out of its way.
//
//   THE GROUND  flat in the dirt with a rock for a pillow, shivering, while
//               two vultures circle. At first light one comes down to see,
//               and pecks a boot. The boot sits up. The bird can wait.
//   THE TREE    along a branch too thin for it, in the Rust Forest. A crow
//               lands on the end. The branch has views on that, and a moment
//               later so does the rest of the tree.
//   THE CHALK   out on the open ground, so still all night that by morning
//               somebody has drawn round them, tagged a toe, left a flower
//               and put up a cross. Then the body sits up and reads the tag.
enum : uint32_t { SCR_DAWN = 1800, SCR_LIGHT = 2500, SCR_MOCK = 4000, SCR_CYC = 5900 };
static const uint32_t SCR_STILL[3] = { 4200, 3900, 3800 };   // the beat a still panel keeps
static void scMoon(float cx, float cy, float r, float ph, float low, uint32_t now);   // section 9

// The set: the night going over to a hard morning, `dawn` 0-1. The broken
// moon goes down the left, the sun comes up the right, and the stars go out
// one at a time.
static void scRoughSet(const ScStage& P, float dawn) {
  const float W = (float)(P.x1 - P.x0), k = P.k, sky = P.gy - P.y0;
  const float top = fxLerp(0.5f, 2.8f, dawn), hor = fxLerp(3.6f, 12.0f, dawn);
  for (int y = P.y0; y < (int)P.gy; y++) {
    const float v = (float)(y - P.y0) / fmaxf(1.0f, sky);
    scTone(y, P.x0, P.x1, fxLerp(top, hor, v * v * 0.8f + v * 0.2f));
  }
  const int nStars = (P.x1 - P.x0) / 4;
  for (int i = 0; i < nStars; i++) {
    const uint32_t h = fxHash2((uint32_t)i, P.seed + 17);
    if (fxU(fxHash(h)) < dawn * 1.5f) continue;
    if (P.live && (fxHash2((uint32_t)i, P.now / 450) & 7) == 0) continue;
    const int x = P.x0 + (int)(h % (uint32_t)(P.x1 - P.x0));
    const int y = P.y0 + 2 + (int)((h >> 9) % (uint32_t)fxMaxI(1, (int)(sky * 0.6f)));
    scPx(x, y, FX_PAL[(h >> 20) % 5 == 0 ? FXP_GLOW : FXP_LINE]);
  }
  if (dawn < 0.75f) scMoon(P.x0 + W * 0.3f, P.y0 + sky * (0.34f + 0.3f * dawn), 6.0f * k, 0.42f, dawn, P.now);
  if (dawn > 0.05f) {
    const float r = 11.0f * k, sx = P.x0 + W * 0.82f, sy = P.gy + r * 0.7f - (dawn - 0.05f) * r * 1.4f;
    const float hr = r * 2.4f;                               // its glow on the sky, in dots, coming up with it
    for (int y = (int)(sy - hr); y < (int)fminf(P.gy, sy + hr); y++)
      for (int x = (int)(sx - hr); x < (int)(sx + hr); x++) {
        const float ddx = x + 0.5f - sx, ddy = y + 0.5f - sy, d2 = (ddx * ddx + ddy * ddy) / (r * r);
        if (d2 <= 1.0f || d2 >= 5.76f) continue;
        if (scDot(x, y, (2.4f - sqrtf(d2)) * 0.43f * dawn)) scPx(x, y, FX_PAL[FXP_HOT]);
      }
    fxDisc(sx, sy, r, FX_PAL[FXP_GLOW]);
    fxDisc(sx - 1, sy - 1, r * 0.65f, FX_PAL[FXP_WHITE]);
  }
  scRuins((float)(P.seed % 3000), (int)P.gy, fxClampF(hor - 3.0f, 0.8f, 8.0f), dawn < 0.5f, P.now, P.seed,
          fxMaxI(12, (int)(16 * k)), (int)(4 * k), (int)fminf(sky * 0.3f, 14.0f * k));
  for (int y = (int)P.gy; y < P.y1; y++) {
    const float v = (float)(y - (int)P.gy) / fmaxf(1.0f, (float)(P.y1 - (int)P.gy));
    scTone(y, P.x0, P.x1, fxLerp(fxLerp(1.6f, 4.5f, dawn), 0.6f, v));
  }
}

// Lettering with an ink shadow under it, so it reads on any sky.
static void scSay(const char* s, float x, float y, int sc, uint8_t col) {
  scType(s, (int)x + 1, (int)y + 1, sc, FXP_INK);
  scType(s, (int)x, (int)y, sc, col);
}
// The z's going up off somebody asleep.
static void scZzz(float x, float y, float k, uint32_t now) {
  const int big = k > 2.5f ? 3 : (k > 1.6f ? 2 : 1);
  for (int i = 0; i < 3; i++) {
    const float ph = fmodf(now * 0.0005f + i / 3.0f, 1.0f);
    const char z[2] = { ph > 0.55f ? 'Z' : 'z', 0 };
    scSay(z, x + ph * 8 * k + sinf(ph * 9.0f + i) * 1.4f * k, y - ph * 14 * k, ph > 0.5f ? big : fxMaxI(1, big - 1),
          ph > 0.8f ? FXP_LINE : FXP_GLOW);
  }
}
// The nose bubble, the way a manga page says fast asleep: it swells on the
// breath in and goes down on the breath out. `pop` is ms since it burst
// (negative: it has not).
static void scNoseBubble(float x, float y, float k, uint32_t now, int32_t pop) {
  if (pop >= 0) {
    if (pop > 260) return;
    const float p = pop / 260.0f;
    for (int i = 0; i < 7; i++) {
      const float a = i * (FX_TAU / 7) + 0.4f, r = (1.5f + 5.0f * fxCubicOut(p)) * k;
      fxDisc(x + cosf(a) * r, y - 2.5f * k + sinf(a) * r, (1.0f - p) * 0.8f * k, FX_PAL[FXP_GLOW]);
    }
    if (pop < 170) scSay("POP", x + 3 * k, y - 9 * k, k > 2.5f ? 2 : 1, FXP_WHITE);
    return;
  }
  const float br = 0.5f + 0.5f * sinf(now * 0.0042f);
  const float r = (0.7f + 2.4f * br) * k, bx = x + r * 0.5f, by = y - r * 0.9f;   // off the nose, up and forward
  fxDisc(bx, by, r + fmaxf(0.8f, 0.35f * k), FX_PAL[FXP_INK]);
  fxDisc(bx, by, r, FX_PAL[FXP_LINE]);
  fxDisc(bx + r * 0.15f, by + r * 0.15f, r * 0.72f, FX_PAL[FXP_DIM]);
  fxDisc(bx - r * 0.38f, by - r * 0.38f, fmaxf(0.6f, r * 0.2f), FX_PAL[FXP_WHITE]);
}
// Cold: jagged shiver ticks either side of a body, flipping every few frames.
static void scShiver(float xl, float xr, float y, float k, uint32_t now) {
  const int jit = (int)((now / 90) & 1);
  for (int s = 0; s < 2; s++) {
    const float x = s ? xr : xl, dir = (float)(s * 2 - 1);
    for (int j = 0; j < 2; j++) {
      const float bx = x + dir * (1.5f + j * 1.8f) * k, by = y - (2.0f + j * 1.6f) * k;
      const float o = (float)(((jit ^ j) << 1) - 1) * 0.7f * k;
      fxStroke(bx, by - 2.0f * k, bx + o, by - 1.0f * k, 0.5f * k, 0.4f * k, FX_PAL[FXP_GLOW]);
      fxStroke(bx + o, by - 1.0f * k, bx - o, by, 0.5f * k, 0.4f * k, FX_PAL[FXP_GLOW]);
      fxStroke(bx - o, by, bx, by + 1.0f * k, 0.5f * k, 0.3f * k, FX_PAL[FXP_GLOW]);
    }
  }
}
// The anger mark on a temple: four arcs back to back, throbbing.
static void scVein(float x, float y, float k, uint32_t now) {
  const float s = k * (1.0f + 0.18f * sinf(now * 0.02f));
  for (int pass = 0; pass < 2; pass++) {
    const uint16_t c = FX_PAL[pass ? FXP_CRIT : FXP_INK];
    const float w = pass ? 0.6f * s : 1.3f * s;
    for (int q = 0; q < 4; q++) {
      const float qa = q * 1.5708f + 0.785f, ax = x + cosf(qa) * 2.3f * s, ay = y + sinf(qa) * 2.3f * s;
      float px = 0, py = 0;
      for (int i = 0; i <= 4; i++) {                         // each arc bows in toward the middle
        const float a = qa + 3.14159f + (i - 2) * 0.42f, ex = ax + cosf(a) * 1.9f * s, ey = ay + sinf(a) * 1.9f * s;
        if (i) fxStroke(px, py, ex, ey, w, w, c);
        px = ex; py = ey;
      }
    }
  }
}
// The panel's own laugh track, once the story is told: HA's popping up round
// whoever it was about. The cut scene laughs bigger (section 12b).
static void scHaHa(const ScStage& P, uint32_t u) {
  if (u < SCR_MOCK) return;
  const int sc = P.k > 1.6f ? 2 : 1;
  const float top = P.y0 + (P.gy - P.y0) * 0.38f, span = (P.gy - top) - 10.0f * sc;
  for (int i = 0; i < 3; i++) {
    if (u < SCR_MOCK + (uint32_t)i * 170) continue;
    const uint32_t h = fxHash2((uint32_t)i + 3u * ((u - SCR_MOCK) / 600u), P.seed + 29);
    const float x = P.x0 + 4 + ((float)i + 0.1f + 0.5f * fxU(h)) * (float)(P.x1 - P.x0 - 8 - 18 * sc) / 3.0f;   // a third each
    const float y = top + fxU(h >> 8) * span - (float)(((u + i * 97) / 90) & 1) * sc;   // shaking with it
    scSay((h >> 16) & 1 ? "HA!" : "HA", x, y, sc, (h >> 17) & 1 ? FXP_GLOW : FXP_HOT);
  }
}

// A vulture side on, feet at (x, y), facing `dir`, `k` px to the unit: a
// hunched body, the pale ruff that says what it is from across a room, a
// bald head on a long neck. `wing` 0 folded .. 1 spread, beating with `beat`
// (radians); `peck` 0-1 takes the head down to the dirt ahead of it.
static void scVulture(float x, float y, float dir, float k, float wing, float beat, float peck, uint8_t rim) {
  const float d = dir;
  auto X = [&](float u) { return x + d * u * k; };
  auto Y = [&](float v) { return y - v * k; };
  const float hu = fxLerp(4.4f, 8.2f, peck), hv = fxLerp(14.0f, 2.4f, peck);   // the head
  const float lift = fxLerp(-0.35f, 1.25f, 0.5f + 0.5f * sinf(beat));          // the wings, up from flat
  const float ba = -0.45f * d;                                                  // hunched into it
  auto wingAt = [&](float su, float sv, float ang, float span, float o, float g, uint16_t c) {
    const float wu = -cosf(ang), wv = sinf(ang), pu = -sinf(ang), pv = -cosf(ang);   // out along it; back across it
    const float tu = su + wu * span, tv = sv + wv * span;
    float qx[4] = { X(su), X(tu), X(tu + pu * 2.6f), X(su + pu * 4.6f) };
    float qy[4] = { Y(sv), Y(tv), Y(tv + pv * 2.6f), Y(sv + pv * 4.6f) };
    for (int i = 0; i < 4; i++) { qx[i] += o; qy[i] += o; }
    fxPoly(qx, qy, 4, c);
    for (int f = 0; f < 4; f++) {                            // the primaries, spread like fingers
      const float fu = tu + pu * (0.3f + f * 0.72f), fv = tv + pv * (0.3f + f * 0.72f);
      fxStroke(X(fu) + o, Y(fv) + o, X(fu + wu * 2.6f - pu * 0.35f * f) + o, Y(fv + wv * 2.6f - pv * 0.35f * f) + o,
               0.9f * k + g, 0.4f * k + g, c);
    }
  };
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.8f, g = pass ? 0.0f : 1.1f;   // the rim: fat, up toward the light
    const uint16_t c = FX_PAL[pass ? FXP_INK : rim];
    if (wing > 0.02f) wingAt(1.4f, 11.2f, lift + 0.2f, 11.0f * wing, o, g, c);   // the far one
    for (int s = 0; s < 2; s++) {                            // legs, and the toes splayed in the dirt
      const float lu = s ? 1.1f : -0.9f;
      fxStroke(X(lu * 0.7f) + o, Y(6.0f) + o, X(lu), Y(0.3f), 1.2f * k + g, 0.7f * k + g, c);
      fxStroke(X(lu) - d * 1.3f * k, Y(0.2f), X(lu) + d * 1.9f * k, Y(0.2f), 0.7f * k + g, 0.5f * k + g, c);
    }
    fxBlob(X(-0.6f) + o, Y(8.4f) + o, cosf(ba), sinf(ba), 5.6f * k + g, 3.9f * k + g, 7, c);
    fxTri(X(-5.2f) + o, Y(7.6f) + o, X(-9.6f) + o, Y(3.6f) + o, X(-5.6f) + o, Y(5.0f) + o, c);   // the tail
    fxStroke(X(2.0f) + o, Y(11.2f) + o, X(hu) + o, Y(hv) + o, 1.4f * k + g, 1.0f * k + g, c);  // the neck
    fxDisc(X(hu) + o, Y(hv) + o, 1.55f * k + g, c);
    const float pa = -peck * 1.1f, bu = cosf(pa), bv = sinf(pa);                                // the beak, hooked
    fxTri(X(hu + 0.5f) + o, Y(hv + 0.8f) + o, X(hu + 0.5f) + o, Y(hv - 0.8f) + o,
          X(hu + bu * 3.6f) + o, Y(hv + bv * 3.6f - 0.3f) + o, c);
    if (wing > 0.02f) wingAt(0.4f, 10.6f, lift, 11.0f * wing, o, g, c);                       // the near one
  }
  // the ruff, pale, and the bald head coming up out of it
  for (int i = 0; i < 10; i++) {
    const float a = i * (FX_TAU / 10) + 0.3f, ca = cosf(a), sa = sinf(a);
    fxTri(X(2.0f + ca * 1.0f), Y(11.0f + sa * 1.0f), X(2.0f + ca * 1.0f - sa * 0.9f), Y(11.0f + sa * 1.0f + ca * 0.9f),
          X(2.0f + ca * 2.7f), Y(11.0f + sa * 2.7f), FX_PAL[FXP_LINE]);
  }
  fxStroke(X(2.6f), Y(11.6f), X(hu), Y(hv), 1.0f * k, 0.8f * k, FX_PAL[FXP_INK]);
  fxDisc(X(hu), Y(hv), 1.1f * k, FX_PAL[FXP_DIM]);
  scPx((int)X(hu + 0.5f), (int)Y(hv + 0.4f), FX_PAL[FXP_HOT]);                                 // an eye on you
  if (wing > 0.02f) {                                                                           // the near wing's edge lit
    const float ang = lift, tu = 0.4f - cosf(ang) * 11.0f * wing, tv = 10.6f + sinf(ang) * 11.0f * wing;
    fxLine((int)X(0.4f), (int)Y(10.6f) - 1, (int)X(tu), (int)Y(tv) - 1, FX_PAL[rim]);
  }
}
// One of them aloft, small against the sky: long fingered wings, banking.
static void scVultureSoar(float x, float y, float k, float tilt, uint8_t rim) {
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.6f, g = pass ? 0.0f : 0.8f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : rim];
    for (int s = 0; s < 2; s++) {
      const float sd = (float)(s * 2 - 1), tx = x + sd * 10.0f * k, ty = y - 1.4f * k - sd * tilt * k;
      fxStroke(x + o, y + o, tx + o, ty + o, 3.0f * k + g, 1.8f * k + g, c);
      for (int f = 0; f < 3; f++)                            // the fingers at the tips
        fxStroke(tx + o, ty + o, tx + sd * 2.4f * k + o, ty + (f - 1) * 0.9f * k + o, 0.6f * k + g, 0.4f * k + g, c);
    }
    fxBlob(x + o, y + o, 1, 0, 2.2f * k + g, 1.2f * k + g, 3, c);
    fxTri(x - 0.9f * k + o, y + 0.6f * k + o, x + 0.9f * k + o, y + 0.6f * k + o, x + o, y + 3.2f * k + o, c);   // the tail
  }
}
// A crow on the wing, at any size (scCrowFly is one far off).
static void scCrowFlap(float x, float y, float k, uint32_t now, float dir) {
  const float up = sinf(now * 0.03f) * 3.5f * k;
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.7f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : FXP_RUST];
    fxBlob(x + o, y + o, 1, 0, 3.0f * k, 1.6f * k, 5, c);
    fxStroke(x - 0.8f * k + o, y + o, x - 8.0f * k + o, y - up + o, 2.2f * k, 0.6f * k, c);
    fxStroke(x + 0.8f * k + o, y + o, x + 8.0f * k + o, y - up + o, 2.2f * k, 0.6f * k, c);
    fxDisc(x + dir * 3.2f * k + o, y - 0.9f * k + o, 1.4f * k, c);
    fxTri(x + dir * 4.2f * k + o, y - 1.4f * k + o, x + dir * 4.2f * k + o, y - 0.4f * k + o, x + dir * 6.4f * k + o, y - 0.9f * k + o, c);
  }
}
// A crow sat on something at (x, y), facing `dir`. Laughing (`caw`), it
// throws its head back and opens its beak wide on every caw.
static void scCrowCaw(float x, float y, float dir, float k, uint32_t now, bool caw) {
  const int open = caw ? (int)((now / 130) & 1) : 0;
  const float back = 0.7f * open, bob = (float)open * 0.6f * k;
  const float hx = x + dir * (3.4f - back) * k, hy = y - (7.6f + back) * k - bob;
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.8f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : FXP_BRICK];
    fxLine((int)(x - k), (int)(y - 2 * k), (int)(x - k), (int)y, c);
    fxLine((int)(x + k), (int)(y - 2 * k), (int)(x + k), (int)y, c);
    fxBlob(x + o, y - 4.2f * k - bob + o, 1, 0, 4.2f * k, 2.8f * k, 11, c);
    fxTri(x - dir * 3 * k + o, y - 5 * k - bob + o, x - dir * 9 * k + o, y - 2.4f * k + o, x - dir * 3 * k + o, y - 2.2f * k - bob + o, c);
    fxDisc(hx + o, hy + o, 2.2f * k, c);
    const float bx = hx + dir * 1.6f * k, lift = 1.1f * k * open;                 // the beak: two halves, apart
    fxTri(bx + o, hy - 0.8f * k + o, bx + o, hy + 0.2f * k + o, bx + dir * 3.4f * k + o, hy - 0.4f * k - lift + o, c);
    fxTri(bx + o, hy + 0.2f * k + o, bx + o, hy + 0.9f * k + o, bx + dir * 3.0f * k + o, hy + 0.5f * k + lift + o, c);
  }
  scPx((int)(hx + dir * 0.6f * k), (int)(hy - 0.6f * k), FX_PAL[FXP_HOT]);
}

// THE GROUND.
static SC_NOINLINE void scRoughGround(const ScStage& P, uint8_t arch, uint8_t seat, uint32_t u, bool cut,
                                      float* hx, float* hy) {
  const float G = P.gy, k = P.k, cx = P.cx, T = (float)P.y0;
  const uint32_t t = P.now;
  const uint32_t LAND = SCR_LIGHT, HOP = LAND + 200, LOOK = LAND + 550, PECK = LAND + 650, POP = LAND + 800;
  const uint32_t UP = LAND + 900, SHOO = LAND + 950, WAIT = LAND + 1300;
  const float dawn = fxClampF(((float)u - SCR_DAWN) / (float)(SCR_LIGHT - SCR_DAWN), 0.0f, 1.0f);
  scRoughSet(P, dawn);
  const uint8_t rim = (uint8_t)(FXP_BRICK + (int)(dawn * 2.0f));  // moonlight, then the morning
  ScFig F = { cx - 2 * k, G + 1 * k, 1, arch, SCP_LIE, seat, 0, rim, fxLerp(-0.7f, 0.7f, dawn), -0.7f, k };
  float px, py;                                              // where the head lay all night
  scHeadAt(F, &px, &py);
  scLump(px - 0.4f * k, G + 0.8f * k, 0.25f, 2.5f * k, P.seed + 3, rim);   // the pillow
  // The two that circled all night. At first light one of them comes down.
  const float ccx = cx + 4 * k, ccy = T + (G - T) * 0.3f, ct = (float)(P.live ? t : 2000u);
  for (int i = 0; i < 2; i++) {
    if (i == 0 && u >= SCR_DAWN) continue;
    const float a = ct * 0.0011f + i * 3.14159f;
    scVultureSoar(ccx + cosf(a) * 17 * k, ccy + sinf(a) * 4 * k, 0.42f * k, cosf(a) * 0.8f, rim);
  }
  const float kv = 0.74f * k, vy0 = G + 1.5f * k;
  float vx = cx + 24 * k, vy = vy0, vk = kv, wing = 0, beat = 0, peck = 0;
  if (u >= SCR_DAWN && u < LAND) {                           // gliding down to it, wings out
    const float p = (float)(u - SCR_DAWN) / (float)(LAND - SCR_DAWN), a0 = (ct - (float)(u - SCR_DAWN)) * 0.0011f;
    vx = fxLerp(ccx + cosf(a0) * 17 * k, cx + 24 * k, fxSmooth(p));
    vy = fxLerp(ccy + sinf(a0) * 4 * k, vy0, p * p);
    vk = fxLerp(0.38f * k, kv, p);
    wing = 1; beat = u * 0.018f;
  } else if (u >= LAND && u < HOP) {                         // down, and folding up
    wing = 1.0f - (float)(u - LAND) / (float)(HOP - LAND); beat = 1.2f;
  } else if (u >= HOP && u < LOOK) {                         // two hops in, for a closer look
    const float p = (float)(u - HOP) / (float)(LOOK - HOP), q = fmodf(p * 2.0f, 1.0f);
    vx = fxLerp(cx + 24 * k, cx + 17.5f * k, (floorf(p * 2.0f) + fxSmooth(q)) * 0.5f);
    vy -= sinf(q * 3.14159f) * 2.4f * k;
  } else if (u >= LOOK && u < SHOO) {
    vx = cx + 17.5f * k;
    if (u < PECK) peck = 0.25f;
    else if (u < PECK + 120) peck = 1.0f;
    else peck = fmaxf(0.0f, 1.0f - (float)(u - PECK - 120) / 150.0f);
  } else if (u >= SHOO) {                                    // and back off, as if that were the plan
    const float p = fminf(1.0f, (float)(u - SHOO) / 220.0f);
    vx = fxLerp(cx + 17.5f * k, cx + 25 * k, fxCubicOut(p));
    vy -= sinf(p * 3.14159f) * 3.0f * k;
    if (u < WAIT) { wing = 0.75f * (1.0f - fxClampF((float)(u - SHOO - 150) / (float)(WAIT - SHOO - 150), 0.0f, 1.0f)); beat = 0.9f; }
  }
  if (u >= LAND && u < LAND + 700) fxDustDraw(cx + 24 * k, vy0, (float)(u - LAND), 8 * k, P.seed + 7, 700.0f);
  if (u >= SCR_DAWN) scVulture(vx, vy, -1, vk, wing, beat, peck, rim);
  // Them.
  if (u >= UP) F.pose = SCP_SIT;
  else if (u < SCR_LIGHT) F.x += (float)((int)((t / 70) & 1) * 2 - 1) * 0.3f * k;   // shivering
  for (int p = 0; p < 3; p++) scFigure(F, p);
  scHeadAt(F, hx, hy);
  if (u < SCR_LIGHT) scShiver(F.x - 15 * k, F.x + 14 * k, F.foot, k, t);
  scNoseBubble(px + 0.6f * k, py - 2.4f * k, k, t, u < POP ? -1 : (int32_t)(u - POP));
  if (u < POP) scZzz(px + 3 * k, py - 6 * k, k, t);
  if (u >= PECK && u < PECK + 250)                           // up over the bird, clear of the boot
    scBang("PECK!", vx + 2 * k, vy - 30 * vk, 6 * k, P.seed + 1, k > 1.3f ? 2 : 1);
  scJolt(P, u, PECK, 0.15f);
  if (u >= POP + 50 && u < UP + 500) scStartle(*hx, *hy, k, u - POP - 50);
  if (u >= UP + 100) scSweat(*hx, *hy, k, u - UP - 100, -1);
  if (u >= WAIT + 100) scGloom(*hx, *hy, k, u - WAIT - 100, t);
  if (u >= SHOO - 30 && u < SHOO + 450) scStartle(vx - 4.4f * vk, vy - 14 * vk, vk * 1.3f, u - SHOO + 30);
  if (u >= WAIT + 150 && k > 1.5f)
    scBalloon("I'LL WAIT.", (int)(vx + 2 * k), (int)(vy - 19 * vk), (int)(vx - 3 * vk), (int)(vy - 16 * vk));
  if (!cut) scHaHa(P, u);
}

// THE TREE.
static SC_NOINLINE void scRoughTree(const ScStage& P, uint8_t arch, uint8_t seat, uint32_t u, bool cut,
                                    float* hx, float* hy) {
  const float G = P.gy, k = P.k, cx = P.cx, T = (float)P.y0, R = (float)P.x1;
  const uint32_t t = P.now;
  const uint32_t INCOMING = SCR_DAWN + 150, PERCH = SCR_LIGHT - 200, CREAK = SCR_LIGHT - 50, SNAP = SCR_LIGHT + 200;
  const uint32_t WHAM = SNAP + 240, BONK = WHAM + 280, STUB = SNAP + 800;
  const float kc = 0.8f * k;                                 // the crow
  const float dawn = fxClampF(((float)u - SCR_DAWN) / (float)(SCR_LIGHT - SCR_DAWN), 0.0f, 1.0f);
  scRoughSet(P, dawn);
  const uint8_t rim = (uint8_t)(FXP_BRICK + (int)(dawn * 2.0f));
  const int sc = k > 1.3f ? 2 : 1;
  if ((u >= SNAP && u < SNAP + 70) || (u >= WHAM && u < WHAM + 70)) scImpactFrame(P, cx, G - 12 * k);
  const float tx = cx - 15 * k;                              // the trunk
  const float bx0 = tx + 1.4f * k, by0 = G - 24 * k;         // where the branch leaves it
  float sag = (1.4f + 0.7f * sinf(t * 0.0042f)) * k;         // it breathes with them
  if (u >= PERCH) sag += 2.6f * k * fxCubicOut(fminf(1.0f, (float)(u - PERCH) / 200.0f));   // and then the crow
  if (u >= CREAK && u < SNAP) sag += (float)((u / 60) & 1) * 0.7f * k;                        // giving, in jerks
  const float bx1 = cx + 22 * k, by1 = G - 22 * k + sag;
  // The trunk, its roots, what is left of the crown, and the bit of it that
  // is about to join them.
  const float twx = tx + 1.9f * k, twy = G - 31.5f * k;
  const bool twigOn = u < WHAM + 80;
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.9f, g = pass ? 0.0f : 1.2f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : rim];
    fxStroke(tx + o, G + o, tx + 1.4f * k + o, G - 31 * k + o, 4.8f * k + g, 2.6f * k + g, c);
    fxStroke(tx + o, G - 1 * k + o, tx - 5 * k + o, G + 0.6f * k + o, 2.0f * k + g, 0.8f * k + g, c);
    fxStroke(tx + o, G - 1 * k + o, tx + 4.5f * k + o, G + 0.8f * k + o, 1.8f * k + g, 0.7f * k + g, c);
    if (twigOn) fxStroke(twx + o, twy + o, twx + 5 * k + o, twy - 4 * k + o, 1.3f * k + g, 0.6f * k + g, c);
  }
  scTree(tx + 1.4f * k, G - 30 * k, P.seed + 9, 8.5f * k, 2.4f * k, 3, FX_PAL[rim]);
  fxDisc(tx + 0.4f * k, G - 13 * k, 1.1f * k, FX_PAL[FXP_SOOT]);   // a knot-hole
  // The branch: on the tree until it isn't, then down with them.
  const float ff = u < SNAP ? 0.0f : fminf(1.0f, (float)(u - SNAP) / (float)(WHAM - SNAP)), fall = ff * ff;
  float ax = bx0, ay = by0, ex = bx1, ey = by1;
  if (u >= SNAP) {
    ax = fxLerp(bx0 + 2.5f * k, bx0 + 4 * k, fall); ay = fxLerp(by0, G - 0.6f * k, fall);
    ex = fxLerp(bx1, bx1 + 2 * k, fall);             ey = fxLerp(by1 + 2 * k, G + 0.6f * k, fall);
  }
  const float mx = (ax + ex) * 0.5f, my = (ay + ey) * 0.5f + (u < SNAP ? sag * 0.5f : 0.0f);
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.8f, g = pass ? 0.0f : 1.1f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : rim];
    fxStroke(ax + o, ay + o, mx + o, my + o, 3.0f * k + g, 2.2f * k + g, c);
    fxStroke(mx + o, my + o, ex + o, ey + o, 2.2f * k + g, 1.0f * k + g, c);
    fxStroke(mx + 3 * k + o, my - 0.3f * k + o, mx + 6 * k + o, my - 4 * k + o, 0.9f * k + g, 0.5f * k + g, c);
    if (u >= SNAP) fxStroke(bx0 + o, by0 + o, bx0 + 2.4f * k + o, by0 + 0.3f * k + o, 3.0f * k + g, 2.6f * k + g, c);   // the stub
  }
  if (u >= SNAP)                                             // splintered pale where it went
    for (int i = 0; i < 3; i++)
      fxLine((int)(bx0 + 2.4f * k), (int)(by0 - 1.0f * k + i * k), (int)(bx0 + 3.6f * k), (int)(by0 - 1.4f * k + i * 1.4f * k), FX_PAL[FXP_GLOW]);
  // Them, along it, an arm hanging down; then not along it.
  const float fx0 = cx + 4 * k, lieY = my - 1.2f * k;
  ScFig F = { fx0, lieY, 1, arch, SCP_LIE, seat, 0, rim, fxLerp(-0.7f, 0.7f, dawn), -0.7f, k };
  if (u < SNAP)       { F.handU = -3.0f; F.handV = -5.0f; F.x += (float)((int)((t / 70) & 1) * 2 - 1) * 0.25f * k * (u < SCR_LIGHT); }
  else if (u < WHAM)  { F.foot = fxLerp((by0 + by1 + sag) * 0.5f - 1.2f * k, G + 1 * k, fall); F.handU = -4.0f; F.handV = 9.0f; }   // arms up, going down
  else                F.foot = G + 1 * k;
  float px, py;
  scHeadAt(F, &px, &py);
  if (u >= SNAP && u < WHAM) {
    scGhost(F, 0, -9 * k * ff, FXP_DIM);
    scGhost(F, 0, -4.5f * k * ff, FXP_LINE);
    scSpeed(P, F.x - 2 * k, F.foot - 3 * k, 0, 1, 22 * k, 14 * k, 12, FXP_WHITE);
  }
  for (int p = 0; p < 3; p++) scFigure(F, p);
  scHeadAt(F, hx, hy);
  // The crow: in off the right, onto the end of the branch -- the last straw
  // -- up when it goes, round, and onto the stub to have a good laugh.
  if (u >= INCOMING && u < PERCH) {
    const float p = (float)(u - INCOMING) / (float)(PERCH - INCOMING);
    scCrowFlap(fxLerp(R + 6 * k, ex - 2 * k, fxSmooth(p)), fxLerp(T + (G - T) * 0.25f, ey - 4 * k, p), kc, t, -1);
  } else if (u >= PERCH && u < SNAP) {
    scCrowCaw(ex - 2.5f * k, ey - 0.9f * k, -1, kc, t, false);
  } else if (u >= SNAP && u < STUB) {
    const float p = (float)(u - SNAP) / (float)(STUB - SNAP);
    scCrowFlap(fxLerp(bx1 - 2 * k, bx0 + 2.4f * k, fxSmooth(p)), fxLerp(by1 - 3 * k, by0 - 1.5f * k, p) - sinf(p * 3.14159f) * 10 * k,
               kc, t, -1);
  } else if (u >= STUB) {
    const bool caw = u >= STUB + 150;
    scCrowCaw(bx0 + 2.2f * k, by0 - 1.4f * k, 1, kc, t, caw);
    if (caw && ((t / 260) & 1)) scSay("KAW", bx0 + (4 + ((t / 520) & 1) * 3) * k, by0 - 12 * k, sc, FXP_GLOW);
  }
  // The bit of the crown, following them down onto the head, and off it.
  if (!twigOn) {
    float qx, qy, a;
    const float hgx = px, hgy = G + 1 * k - 3.2f * k;        // the head, on the ground
    if (u < BONK) {
      const float p = (float)(u - WHAM - 80) / (float)(BONK - WHAM - 80);
      qx = fxLerp(twx + 2.5f * k, hgx, p); qy = fxLerp(twy - 2 * k, hgy - 2.5f * k, p * p); a = -0.7f + p * 5.0f;
    } else {
      const float p = fminf(1.0f, (float)(u - BONK) / 260.0f);
      qx = fxLerp(hgx, hgx - 7 * k, p); qy = fxLerp(hgy - 2.5f * k, G + 0.4f * k, p) - sinf(p * 3.14159f) * 5 * k; a = 4.3f + p * 1.9f;
    }
    const float ca = cosf(a) * 3 * k, sa = sinf(a) * 3 * k;
    fxStroke(qx - ca - 0.8f, qy - sa - 0.8f, qx + ca - 0.8f, qy + sa - 0.8f, 1.3f * k + 1.1f, 0.6f * k + 1.1f, FX_PAL[rim]);
    fxStroke(qx - ca, qy - sa, qx + ca, qy + sa, 1.3f * k, 0.6f * k, FX_PAL[FXP_INK]);
  }
  // What it all sounds like.
  if (u < CREAK) {
    scNoseBubble(px + 0.6f * k, py - 2.4f * k, k, t, -1);
    scZzz(px + 3 * k, py - 6 * k, k, t);
    if ((u >= 600 && u < 1000) || (u >= 1250 && u < 1650)) scSay("creak", bx0 + 2 * k, by0 - 7 * k, 1, FXP_LINE);
  } else if (u < SNAP) {
    scNoseBubble(px + 0.6f * k, py - 2.4f * k, k, t, (int32_t)(u - CREAK));
    for (int i = 0; i < 3; i++)                              // the crack, running
      fxLine((int)(bx0 + 1 * k), (int)(by0 - 0.5f * k + i * 0.6f * k), (int)(bx0 + (2.2f + i * 0.5f) * k), (int)(by0 - (1.6f - i * 1.2f) * k), FX_PAL[FXP_WHITE]);
    scSay("crk", bx0 + 3 * k, by0 - 8 * k, sc, FXP_GLOW);
    if (u >= CREAK + 60) scStartle(*hx, *hy, k, u - CREAK - 60);
    if (u >= CREAK + 120) scSweat(*hx, *hy, k, u - CREAK - 120, 1);
  }
  if (u >= SNAP && u < SNAP + 240) scBang("SNAP!", bx0 + 15 * k, by0 - 23 * k, 8 * k, P.seed, sc);   // all clear of the fall
  if (u >= WHAM && u < WHAM + 300) scBang("WHAM!", F.x + 15 * k, G - 9 * k, 8 * k, P.seed + 2, sc);
  if (u >= WHAM) fxDustDraw(F.x, G + 1 * k, (float)(u - WHAM), 14 * k, P.seed + 4, 700.0f);
  if (u >= BONK && u < BONK + 300) scBang("BONK", *hx - 5 * k, *hy - 23 * k, 6 * k, P.seed + 3, sc);
  if (u >= BONK) scDizzy(*hx, *hy, k, t);
  scJolt(P, u, SNAP, 0.25f);
  scJolt(P, u, WHAM, 0.5f);
  scJolt(P, u, BONK, 0.2f);
  if (!cut) scHaHa(P, u);
}

// The chalk outline, round somebody lying with their hip at x on the ground
// line g, seen low across the dirt so its plan is squashed: a head, arms
// flung out, legs apart, the way the police drew them. Chalk goes on dry.
static void scChalk(float x, float g, float k, uint32_t seed) {
  static const float CU[22] = { -9.0f, -6.5f, -5.0f, -3.0f, -3.6f, 2.0f, 13.8f, 15.6f, 5.0f, 15.6f, 14.0f,
                                2.0f, -3.6f, -2.6f, -4.6f, -6.5f, -9.0f, -11.0f, -14.8f, -16.2f, -14.8f, -11.0f };
  static const float CW[22] = { 1.9f, 3.0f, 8.4f, 8.0f, 2.8f, 3.0f, 6.0f, 4.4f, 0.4f, -3.8f, -5.6f,
                                -3.0f, -2.8f, -8.2f, -8.8f, -3.0f, -1.9f, -3.6f, -3.0f, 0.0f, 3.0f, 3.6f };
  float ou[96], ov[96];
  const int m = fxCurve(CU, CW, 22, 4, ou, ov, 96);
  const uint8_t stip0 = fxStip;
  const uint32_t seed0 = fxStipSeed;
  for (int pass = 0; pass < 2; pass++) {
    fxStip = (uint8_t)(pass ? 150 : 40);
    fxStipSeed = seed + pass * 7;
    const float dx = pass * 0.35f * k, dy = pass * 0.25f * k, w = fmaxf(1.2f, 0.65f * k);
    for (int i = 0; i < m; i++) {
      const int j = (i + 1) % m;
      fxStroke(x + ou[i] * k + dx, g + ov[i] * 0.55f * k + dy, x + ou[j] * k + dx, g + ov[j] * 0.55f * k + dy, w, w, FX_PAL[FXP_WHITE]);
    }
  }
  fxStip = stip0;
  fxStipSeed = seed0;
}
// The cross at the head of it, on its mound: RIP across the bar where the
// glass is big enough to letter it.
static void scGraveCross(float x, float g, float k, uint8_t rim) {
  const bool rip = k > 2.4f;
  const float th = rip ? 10.0f : 2.4f * k;
  fxBlob(x, g + 0.2f * k, 1, 0, 4.4f * k, 1.4f * k, 19, FX_PAL[FXP_TRACK]);
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -0.9f, gr = pass ? 0.0f : 1.2f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : rim];
    fxStroke(x + o, g + o, x + 0.3f * k + o, g - 13 * k + o, 1.6f * k + gr, 1.3f * k + gr, c);
    fxStroke(x - 4.2f * k + o, g - 9.7f * k + o, x + 4.4f * k + o, g - 9.3f * k + o, th + gr, th + gr, c);
  }
  if (rip) fxGlcd("RIP", (int)(x - 8), (int)(g - 9.5f * k - 3), 1, FX_PAL[FXP_GLOW]);
}
// A toe tag, on a string from the big toe at (x, y).
static void scToeTag(float x, float y, float k, uint32_t now) {
  const float tx = x + 1.6f * k + sinf(now * 0.003f) * 0.5f * k, ty = y + 2.2f * k;
  fxLine((int)x, (int)y, (int)tx, (int)ty, FX_PAL[FXP_GLOW]);
  const int x0 = (int)tx, y0 = (int)ty, x1 = (int)(tx + 2.8f * k) + 1, y1 = (int)(ty + 1.7f * k) + 1;
  fxRect(x0 - 1, y0 - 1, x1 + 1, y1 + 1, FX_PAL[FXP_INK]);
  fxRect(x0, y0, x1, y1, FX_PAL[FXP_GLOW]);
  if (k > 1.6f) fxLine(x0 + 2, (y0 + y1) / 2, x1 - 2, (y0 + y1) / 2 - 1, FX_PAL[FXP_INK]);   // a name, scrawled
}
// A flower somebody left, already wilting: standing at (x, y), or laid flat.
static void scWiltFlower(float x, float y, float k, bool flat) {
  const float hx = flat ? x + 5 * k : x + 2.2f * k, hy = flat ? y - 0.5f * k : y - 3.4f * k;
  if (flat) fxStroke(x, y, hx, hy, 0.7f * k, 0.5f * k, FX_PAL[FXP_RUST]);
  else {
    fxStroke(x, y, x + 0.5f * k, y - 4.2f * k, 0.7f * k, 0.5f * k, FX_PAL[FXP_RUST]);
    fxStroke(x + 0.5f * k, y - 4.2f * k, hx, hy, 0.5f * k, 0.4f * k, FX_PAL[FXP_RUST]);   // gone over at the neck
  }
  for (int i = 0; i < 4; i++) {
    const float a = i * 1.5708f + 0.4f;
    fxDisc(hx + cosf(a) * 0.9f * k, hy + sinf(a) * 0.9f * k, 0.75f * k, FX_PAL[FXP_HOT]);
  }
  fxDisc(hx, hy, 0.5f * k, FX_PAL[FXP_INK]);
}
// The tape across the front of it all: DO NOT CROSS, sagging.
static void scTape(const ScStage& P, float y, float k, uint32_t now) {
  const bool words = k > 2.4f;
  const float th = words ? 10.0f : 2.2f * k, half = (P.x1 - P.x0) * 0.5f, mid = P.x0 + half;
  auto at = [&](float x) {
    const float q = (x - mid) / half;
    return y + 2.2f * k * (1.0f - q * q) + sinf(now * 0.004f + x * 0.05f) * 0.4f * k;
  };
  for (int x = P.x0; x < P.x1; x += 2) {
    const float yc = at(x + 1.0f);
    fxRect(x, (int)(yc - th * 0.5f) - 1, x + 2, (int)(yc + th * 0.5f) + 1, FX_PAL[FXP_INK]);
    fxRect(x, (int)(yc - th * 0.5f), x + 2, (int)(yc + th * 0.5f), FX_PAL[FXP_HOT]);
  }
  static const char TXT[] = "DO NOT CROSS  ";
  for (int x = P.x0 + 2, i = 0; x < P.x1 - 4; x += words ? 6 : (int)(4 * k), i++) {
    const float yc = at(x + 2.5f);
    if (words) {
      const char ch[2] = { TXT[i % 14], 0 };
      fxGlcd(ch, x, (int)(yc - 3.5f), 1, FX_PAL[FXP_INK]);
    } else if (i & 1) {
      fxRect(x, (int)(yc - th * 0.5f), x + (int)(1.6f * k), (int)(yc + th * 0.5f), FX_PAL[FXP_INK]);
    }
  }
}

// THE CHALK.
static SC_NOINLINE void scRoughChalk(const ScStage& P, uint8_t arch, uint8_t seat, uint32_t u, bool cut,
                                     float* hx, float* hy) {
  const float G = P.gy, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
  const uint32_t BLINK = SCR_LIGHT + 300, JOLT = BLINK + 150, SIT = BLINK + 260, READ = BLINK + 500, CHEAP = BLINK + 950;
  const bool morning = u >= SCR_DAWN;                        // no dawn here: a cut to the morning after
  scRoughSet(P, morning ? 1.0f : 0.0f);
  if (morning && u < SCR_DAWN + 70) scImpactFrame(P, cx, G - 8 * k);
  const uint8_t rim = morning ? FXP_HDR : FXP_BRICK;
  ScFig F = { cx + 3 * k, G + 2 * k, 1, arch, SCP_LIE, seat, 0, rim, morning ? 0.7f : -0.7f, -0.7f, k };
  float px, py;
  scHeadAt(F, &px, &py);
  if (morning) {
    scChalk(F.x, F.foot, k, P.seed);
    scGraveCross(F.x - 21 * k, G, k, rim);
  }
  if (u >= SIT) F.pose = SCP_SIT;
  else if (!morning) F.x += (float)((int)((t / 70) & 1) * 2 - 1) * 0.3f * k;   // shivering
  if (u >= READ) F.look = -0.6f;                             // down at the tag on their toe
  for (int p = 0; p < 3; p++) scFigure(F, p);
  scHeadAt(F, hx, hy);
  if (morning) {
    const bool sat = u >= SIT;
    scToeTag(F.x + (sat ? 11.3f : 12.4f) * k, F.foot - (sat ? 1.6f : 4.4f) * k, k, t);
    if (sat) scWiltFlower(F.x - 6 * k, G + 2.4f * k, k, true);   // off the chest when they came up
    else     scWiltFlower(F.x - 5 * k, F.foot - 5.2f * k, k, false);
  }
  if (!morning) {
    scShiver(F.x - 15 * k, F.x + 14 * k, F.foot, k, t);
    scZzz(px + 3 * k, py - 6 * k, k, t);
    for (int i = 0; i < 4; i++) scKira(F.x + (-10 + i * 7) * k, F.foot - (3 + (i & 1) * 2) * k, 1.6f * k, t, i);   // frost on them
  }
  scNoseBubble(px + 0.6f * k, py - 2.4f * k, k, t, u < BLINK ? -1 : (int32_t)(u - BLINK));
  if (u >= BLINK && u < SIT) fxDisc(px + 0.7f * k, py - 1.4f * k, fmaxf(0.8f, 0.6f * k), FX_PAL[FXP_WHITE]);   // an eye, open
  if (u >= JOLT && u < JOLT + 600) scStartle(*hx, *hy, k, u - JOLT);
  scJolt(P, u, JOLT, 0.15f);
  if (u >= READ + 150) scSweat(*hx, *hy, k, u - READ - 150, -1);
  if (u >= CHEAP && k > 1.5f) scBalloon("NOT DEAD. JUST CHEAP.", (int)(*hx - 10 * k), (int)(*hy - 8 * k), (int)(*hx - 1 * k), (int)(*hy - 4 * k));
  if (morning) scTape(P, G + 5.5f * k, k, t);
  if (!cut) scHaHa(P, u);
}

// One of the three, by number: 0 the ground, 1 the tree, 2 the chalk. Puts
// where the sleeper's head is now in (hx, hy), for whatever is laughing.
static void scArtRough(const ScStage& P, uint8_t v, uint8_t arch, uint8_t seat, uint32_t u, bool cut,
                       float* hx, float* hy) {
  if (v == 1)      scRoughTree(P, arch, seat, u, cut, hx, hy);
  else if (v == 2) scRoughChalk(P, arch, seat, u, cut, hx, hy);
  else             scRoughGround(P, arch, seat, u, cut, hx, hy);
}

// One panel's picture. Everything is placed off the stage, so the same event
// reads at either size.
static SC_NOINLINE void scPanelArt(const ScStage& P, uint8_t glyph, uint8_t arch, uint8_t arch2, int8_t who, int8_t who2) {
  const float L = (float)P.x0, R = (float)P.x1, T = (float)P.y0, G = P.gy, W = R - L, k = P.k, cx = P.cx;
  const uint32_t t = P.now;
  const uint8_t seat = (uint8_t)(who >= 0 && who < SC_SEATS ? who : 255);
  const uint8_t seat2 = (uint8_t)(who2 >= 0 && who2 < SC_SEATS ? who2 : 255);
  const float ph = P.live ? t * 0.0105f : 1.1f;
  auto fig = [&](float x, uint8_t a, uint8_t pose, float dir, float phase, uint8_t st, float kk) {
    ScFig F = { x, G, dir, a, pose, st, phase, P.rim, dir > 0 ? -0.7f : 0.7f, -0.7f, kk };
    for (int p = 0; p < 3; p++) scFigure(F, p);
  };
  switch (glyph) {
    case SC_GLY_DAWN: {                                      // up comes the sun; someone is still here to see it
      fxSunburst(cx + W * 0.18f, G, 13, P.seed, P.live ? t * 0.00012f : 0.3f, FX_PAL[FXP_HOT]);
      fxDisc(cx + W * 0.18f, G, 14 * k, FX_PAL[FXP_GLOW]);
      fxDisc(cx + W * 0.18f - 2, G - 2, 9 * k, FX_PAL[FXP_WHITE]);
      fxRect((int)L, (int)G, (int)R, (int)G + 2, FX_PAL[FXP_INK]);
      fig(cx - W * 0.2f, arch, SCP_REACH, 1, 0, seat, k);    // a stretch. or a surrender.
      break;
    }
    case SC_GLY_FORAGE: scArtForage(P, arch, arch2, seat, seat2); break;
    case SC_GLY_WATER: scArtWater(P, arch, arch2, seat, seat2); break;
    case SC_GLY_MEDIC: scArtMedic(P, arch, arch2, seat, seat2); break;
    case SC_GLY_SALVAGE: scArtSalvage(P, arch, arch2, seat, seat2); break;
    case SC_GLY_SHELTER: scArtShelter(P, arch, arch2, seat, seat2); break;
    case SC_GLY_CRAFT: {
      float bx = cx + 6 * k;                                 // the bench
      fxRect((int)(bx - 14 * k), (int)(G - 11 * k), (int)(bx + 16 * k), (int)(G - 8 * k), FX_PAL[FXP_INK]);
      fxRect((int)(bx - 12 * k), (int)(G - 8 * k), (int)(bx - 10 * k), (int)G, FX_PAL[FXP_INK]);
      fxRect((int)(bx + 12 * k), (int)(G - 8 * k), (int)(bx + 14 * k), (int)G, FX_PAL[FXP_INK]);
      fxLine((int)(bx - 14 * k), (int)(G - 12 * k), (int)(bx + 16 * k), (int)(G - 12 * k), FX_PAL[P.rim]);
      fxRect((int)(bx - 2 * k), (int)(G - 16 * k), (int)(bx + 8 * k), (int)(G - 11 * k), FX_PAL[FXP_LINE]);   // the thing
      float swing = P.live ? fxClampF(sinf(t * 0.009f) * 1.6f, -1, 1) : 1.0f;
      fig(cx - 18 * k, arch, SCP_REACH, 1, 0, seat, k);
      float hx = scHandX, hy = scHandY;                      // the hammer, coming down
      float ex = bx, ey = G - 16 * k;
      float mx = fxLerp(hx, ex, (swing + 1) * 0.5f), my = fxLerp(hy, ey - 6 * k, (swing + 1) * 0.5f);
      fxStroke(hx, hy, mx, my, 1.6f, 1.4f, FX_PAL[FXP_INK]);
      fxRect((int)(mx - 3 * k), (int)(my - 2 * k), (int)(mx + 3 * k), (int)(my + 2 * k), FX_PAL[FXP_INK]);
      if (swing > 0.7f) {
        for (int i = 0; i < 6; i++) {
          uint32_t h = fxHash(i + (P.live ? t / 300 : 1) * 17);
          float a2 = -2.6f + fxU(h) * 2.0f, rr = (4 + 8 * fxU(h >> 8)) * k;
          fxLine((int)bx, (int)(G - 16 * k), (int)(bx + cosf(a2) * rr), (int)(G - 16 * k + sinf(a2) * rr), FX_PAL[FXP_GLOW]);
        }
        scBang("CLANG", bx + 12 * k, T + (G - T) * 0.35f, 9 * k, P.seed, 1);
      }
      break;
    }
    case SC_GLY_SCOUT: {
      scHeap(cx + 4 * k, G, 70 * k, 20 * k, P.seed ^ 5, P.rim);   // a rise to stand on
      ScFig F = { cx - 2 * k, G - 17 * k, 1, arch, SCP_STAND, seat, 0, P.rim, -0.7f, -0.7f, k };
      for (int p = 0; p < 3; p++) scFigure(F, p);
      float fx0 = cx + 12 * k, fy0 = G - 16 * k;             // the flag, on what was blank
      fxStroke(fx0, fy0, fx0, fy0 - 26 * k, 1.4f, 1.2f, FX_PAL[FXP_INK]);
      float fl = P.live ? fxNoise1(t * 0.006f, 7) * 4 : 2;
      fxTri(fx0, fy0 - 26 * k, fx0, fy0 - 18 * k, fx0 + 11 * k + fl, fy0 - 23 * k + fl * 0.3f, FX_PAL[FXP_HOT]);
      break;
    }
    case SC_GLY_REST: {
      fxBlob(cx, G - 2, 1, 0, 19 * k, 3.5f * k, 11, FX_PAL[FXP_LINE]);   // the bedroll
      ScFig D = { cx + 2 * k, G - 3, -1, arch, SCP_LIE, seat, 0, P.rim, 0.7f, -0.7f, k };
      for (int p = 0; p < 3; p++) scFigure(D, p);
      for (int i = 0; i < 3; i++) {
        float ph2 = fmodf((P.live ? t * 0.0005f : 0.3f) + i / 3.0f, 1.0f);
        char z[2] = { 'z', 0 };
        scType(z, (int)(cx - 14 * k + ph2 * 16 * k + i * 2), (int)(G - 12 * k - ph2 * 24 * k), ph2 > 0.6f ? 2 : 1, FXP_GLOW);
      }
      break;
    }
    case SC_GLY_TRADE: {
      fig(cx - 20 * k, arch, SCP_STAND, 1, 0, seat, k);
      fig(cx + 20 * k, arch2, SCP_STAND, -1, 0, seat2, k);
      float bob = P.live ? sinf(t * 0.004f) * 2 : 0;
      fxRect((int)(cx - 5 * k), (int)(G - 17 * k + bob), (int)(cx + 5 * k), (int)(G - 10 * k + bob), FX_PAL[FXP_INK]);
      fxRect((int)(cx - 4 * k), (int)(G - 16 * k + bob), (int)(cx + 4 * k), (int)(G - 11 * k + bob), FX_PAL[FXP_HDR]);
      scType("-->", (int)(cx - 9), (int)(G - 30 * k), 1, FXP_GLOW);
      scType("<--", (int)(cx - 9), (int)(G - 30 * k) + 9, 1, FXP_CRIT);
      break;
    }
    case SC_GLY_CARAVAN: {
      float drive = P.live ? fmodf(t * 0.012f, 14.0f) - 7 : 0;
      float x = cx + drive * k, y = G - 2;                   // the lead vehicle: an APC, more or less
      float bx[6] = { x - 26 * k, x + 18 * k, x + 26 * k, x + 26 * k, x - 26 * k, x - 28 * k };
      float by[6] = { y - 18 * k, y - 18 * k, y - 11 * k, y - 4 * k, y - 4 * k, y - 11 * k };
      fxPoly(bx, by, 6, FX_PAL[FXP_INK]);
      fxLine((int)(x - 26 * k), (int)(y - 19 * k), (int)(x + 18 * k), (int)(y - 19 * k), FX_PAL[P.rim]);
      fxRect((int)(x - 8 * k), (int)(y - 24 * k), (int)(x + 6 * k), (int)(y - 18 * k), FX_PAL[FXP_INK]);   // turret
      fxStroke(x + 6 * k, y - 22 * k, x + 20 * k, y - 25 * k, 1.8f, 1.4f, FX_PAL[FXP_INK]);
      for (int i = 0; i < 4; i++) { fxDisc(x - 18 * k + i * 12 * k, y - 3, 3.6f * k, FX_PAL[FXP_INK]); fxDisc(x - 18 * k + i * 12 * k, y - 3, 1.2f * k, FX_PAL[FXP_LINE]); }
      fxStroke(x - 20 * k, y - 18 * k, x - 20 * k, y - 32 * k, 1.0f, 1.0f, FX_PAL[FXP_INK]);
      fxTri(x - 20 * k, y - 32 * k, x - 20 * k, y - 27 * k, x - 12 * k, y - 30 * k, FX_PAL[FXP_HOT]);   // a pennant
      for (int i = 0; i < 5; i++) {                          // dust
        float ph2 = fmodf((P.live ? t * 0.001f : 0.4f) + i / 5.0f, 1.0f);
        fxDisc(x - 30 * k - ph2 * 22 * k, y - 4 - ph2 * 7 * k, (2 + ph2 * 5) * k, FX_PAL[ph2 < 0.5f ? FXP_RUST : FXP_EMBER]);
      }
      break;
    }
    case SC_GLY_ARRIVE: {
      float in = P.live ? fmodf(t * 0.009f, 40.0f) : 20.0f;
      fig(R - 14 * k - in * k, arch, SCP_WALK, -1, ph, seat, k);
      fig(cx - 26 * k, arch2 < 6 ? arch2 : 0, SCP_REACH, 1, 0, 255, k);   // somebody waves. probably at them.
      break;
    }
    case SC_GLY_DEPART: {
      float out = P.live ? fmodf(t * 0.004f, 1.0f) : 0.5f;
      float kk = k * (0.9f - out * 0.55f);                   // getting smaller as they go
      fig(cx + (10 + out * 30) * k, arch, SCP_WALK, 1, ph, seat, kk);
      for (int i = 0; i < 6; i++) scPx((int)(cx - 20 * k + i * 6 * k), (int)G + 1 + (i & 1), FX_PAL[FXP_LINE]);   // footprints
      break;
    }
    case SC_GLY_THRESHOLD: {
      for (int y = (int)(T + 4); y < (int)G; y++) scTone(y, (int)(cx - 4 * k), (int)R, 3.6f + 0.8f * fxNoise1(y * 0.2f, 3));   // a wall
      for (int i = 0; i < 8; i++) {                          // its bricks, some of them
        uint32_t h = fxHash(i + P.seed);
        int bx0 = (int)(cx + (h % 60) * k), by0 = (int)(T + 8 + ((h >> 8) % 40));
        fxRect(bx0, by0, bx0 + (int)(7 * k), by0 + 3, FX_PAL[FXP_LINE]);
      }
      float dx0 = cx + 8 * k, dx1 = cx + 26 * k, dtop = G - 34 * k;
      fxRect((int)dx0, (int)dtop, (int)dx1, (int)G, FX_PAL[FXP_INK]);
      fxDisc((dx0 + dx1) * 0.5f, dtop, (dx1 - dx0) * 0.5f, FX_PAL[FXP_INK]);
      float open = P.live ? (((t / 1300) % 4) != 0 ? 1.0f : 0.0f) : 1.0f;
      if (open > 0) {                                        // something in there has eyes
        scPx((int)((dx0 + dx1) * 0.5f) - 3, (int)(dtop + 8 * k), FX_PAL[FXP_CRIT]);
        scPx((int)((dx0 + dx1) * 0.5f) + 2, (int)(dtop + 8 * k), FX_PAL[FXP_CRIT]);
      }
      fig(cx - 8 * k, arch, SCP_STAND, 1, 0, seat, k);
      scBang("CREAK", cx - 26 * k, T + (G - T) * 0.35f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_CLASH: scArtClash(P, arch, arch2, seat, seat2); break;
    case SC_GLY_LIGHT: {
      fxRect((int)L, (int)T, (int)R, (int)G, FX_PAL[FXP_INK]);
      float dx = cx + 14 * k;                                // a panel, open, and the light behind it
      fxHalfSet(0, dx, G - 16 * k, -1, -0.45f, -1);
      fxHalfSet(1, dx, G - 16 * k, -1, 0.45f, 1);
      fxHalfN = 2;
      fxSunburst(dx, G - 16 * k, 17, P.seed, P.live ? t * 0.0002f : 0, FX_PAL[FXP_HOT]);
      fxHalfN = 0;
      fxRect((int)dx - 2, (int)(G - 30 * k), (int)(dx + 8 * k), (int)G, FX_PAL[FXP_WHITE]);
      fig(cx - 6 * k, arch, SCP_STAND, 1, 0, seat, k);
      scBang("CLICK", cx - 28 * k, T + (G - T) * 0.3f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_HAUL: {
      float walk = P.live ? fmodf(t * 0.01f, 30.0f) - 15 : 0;
      float x = cx + walk * k;
      fig(x, arch, SCP_WALK, 1, ph, seat, k);
      fxBlob(x - 8 * k, G - 27 * k, 0.9f, 0.3f, 11 * k, 9 * k, 77, FX_PAL[FXP_INK]);   // the sack. heavy.
      fxBlob(x - 9 * k, G - 28 * k, 0.9f, 0.3f, 9 * k, 7 * k, 77, FX_PAL[FXP_LINE]);
      fxLine((int)(x - 3 * k), (int)(G - 36 * k), (int)(x - 1 * k), (int)(G - 32 * k), FX_PAL[FXP_INK]);
      for (int i = 0; i < 3; i++) {                          // and it leaks tins
        float ph2 = fmodf((P.live ? t * 0.0015f : 0.6f) + i / 3.0f, 1.0f);
        fxRect((int)(x - 18 * k - i * 3), (int)(G - 22 * k + ph2 * 20 * k), (int)(x - 15 * k - i * 3), (int)(G - 19 * k + ph2 * 20 * k), FX_PAL[FXP_HDR]);
      }
      if (P.k > 1.3f) scBang("HNNGH", cx + 28 * k, T + (G - T) * 0.3f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_WOUND: {
      fig(cx - 4 * k, arch, SCP_CROUCH, 1, 0, seat, k);
      for (int i = 0; i < 4; i++) {                          // it runs
        float ph2 = fmodf((P.live ? t * 0.0011f : 0.5f) + i / 4.0f, 1.0f);
        fxDisc(cx + 5 * k + (i & 1) * 2, G - 8 * k + ph2 * 8 * k, 1.3f, FX_PAL[FXP_BLOOD]);
      }
      fxBlob(cx + 8 * k, G + 1, 1, 0, 8 * k, 1.8f, 13, FX_PAL[FXP_BLOOD]);
      scBang("OOF", cx + 22 * k, T + (G - T) * 0.35f, 8 * k, P.seed, P.k > 1.3f ? 2 : 1);
      break;
    }
    case SC_GLY_DEATH: {
      fxBlob(cx, G, 1, 0, 24 * k, 7 * k, 21, FX_PAL[FXP_TRACK]);   // the mound
      fxBlob(cx, G + 1, 1, 0, 23 * k, 5 * k, 21, FX_PAL[FXP_SOOT]);
      // the cross, with the moon along its edge -- ink alone is nothing at night
      fxStroke(cx - 1, G - 4 * k - 1, cx, G - 30 * k - 1, 3.0f * k + 1.4f, 2.6f * k + 1.4f, FX_PAL[FXP_BRICK]);
      fxStroke(cx - 9 * k - 1, G - 21 * k - 1, cx + 9 * k - 1, G - 22 * k - 1, 2.8f * k + 1.4f, 2.8f * k + 1.4f, FX_PAL[FXP_BRICK]);
      fxStroke(cx, G - 4 * k, cx + 1, G - 30 * k, 3.0f * k, 2.6f * k, FX_PAL[FXP_INK]);
      fxStroke(cx - 9 * k, G - 21 * k, cx + 9 * k, G - 22 * k, 2.8f * k, 2.8f * k, FX_PAL[FXP_INK]);
      fxLine((int)(cx - 1), (int)(G - 30 * k), (int)(cx - 1), (int)(G - 5 * k), FX_PAL[P.rim]);
      scCrowSit(cx + 8 * k, G - 22 * k - 1.5f * k, -1, P.live ? t : 0, 3, k);
      break;
    }
    case SC_GLY_DOOM: {
      scDoomAt(cx + W * 0.2f, G, fminf(1.0f, 0.55f * k), 90, P.live ? t : 4000);
      fig(cx - W * 0.3f, arch, SCP_STAND, -1, 0, seat, k * 0.8f);   // not looking. that is worse.
      break;
    }
    case SC_GLY_FIRE: {
      for (int i = 0; i < 3; i++) scFire(cx + 10 * k + (i - 1) * 13 * k, G - 1, (P.live ? t : 999) + i * 777, k * 0.8f);
      fig(cx - 26 * k, arch, SCP_WALK, -1, P.live ? t * 0.02f : 1.1f, seat, k);   // running
      scBang("FWOOSH", cx + 14 * k, T + (G - T) * 0.3f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);
      break;
    }
    case SC_GLY_FLOOD: {
      float rise = P.live ? sinf(t * 0.0015f) * 2 : 0;
      ScFig F = { cx + 4 * k, G - 11 * k, 1, arch, SCP_STAND, seat, 0, P.rim, -0.7f, -0.7f, k };
      float x0 = cx - 14 * k;                                // on the roof of a car, which is also going under
      fxRect((int)x0, (int)(G - 11 * k), (int)(x0 + 34 * k), (int)(G - 3 * k), FX_PAL[FXP_INK]);
      for (int p = 0; p < 3; p++) scFigure(F, p);
      for (int y = (int)(G - 6 * k + rise); y < P.y1; y++)
        for (int x = P.x0; x < P.x1; x++) {
          float wv = sinf(x * 0.25f + (P.live ? t * 0.006f : 0)) * 1.2f;
          if (y > G - 6 * k + rise + wv) scDither(x, y, 6.0f + ((y + (int)wv) % 5 == 0 ? 3 : 0));
        }
      scBang("SPLOSH", cx - 24 * k, T + (G - T) * 0.3f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_QUAKE: {
      float jolt = P.live ? (fxNoise1(t * 0.03f, 8) - 0.5f) * 3 : 0;
      float gx[6] = { cx - 4 * k, cx + 4 * k, cx + 1 * k, cx + 7 * k, cx - 2 * k, cx - 8 * k };
      float gyv[6] = { G, G, G + 8 * k, G + 20 * k, G + 12 * k, G + 6 * k };
      for (int i = 0; i < 6; i++) { gx[i] += (i < 3 ? -1.5f : 1.5f); }
      fxPoly(gx, gyv, 6, FX_PAL[FXP_HDR]);                   // the lip of it, catching the light
      for (int i = 0; i < 6; i++) { gx[i] -= (i < 3 ? -1.5f : 1.5f); }
      fxPoly(gx, gyv, 6, FX_PAL[FXP_INK]);                  // the ground opens
      fxLine((int)(cx - 30 * k), (int)G, (int)(cx - 4 * k), (int)G, FX_PAL[P.rim]);
      ScFig F = { cx - 14 * k + jolt, G - 2, 1, arch, SCP_WALK, seat, 2.2f, P.rim, -0.7f, -0.7f, k };
      for (int p = 0; p < 3; p++) scFigure(F, p);
      for (int i = 0; i < 5; i++) {
        uint32_t h = fxHash(i * 3 + 1);
        fxDisc(cx + (fxS(h) * 20) * k, G - (4 + fxU(h >> 8) * 22) * k + jolt, (1 + fxU(h >> 16) * 2) * k, FX_PAL[FXP_LINE]);
      }
      scBang("KRRAK", cx + 20 * k, T + (G - T) * 0.32f, 9 * k, P.seed, P.k > 1.3f ? 2 : 1);
      break;
    }
    case SC_GLY_SETTLE: {
      for (int i = 0; i < 4; i++) {                          // shacks, and they stay
        float sx = cx - 40 * k + i * 22 * k, sh = (14 + (fxHash(i + P.seed) % 8)) * k;
        fxRect((int)(sx - 9 * k), (int)(G - sh), (int)(sx + 9 * k), (int)G, FX_PAL[FXP_INK]);
        fxTri(sx - 11 * k, G - sh, sx + 11 * k, G - sh, sx + (i & 1 ? 3 : -3) * k, G - sh - 8 * k, FX_PAL[FXP_INK]);
        fxLine((int)(sx - 11 * k), (int)(G - sh), (int)(sx + (i & 1 ? 3 : -3) * k), (int)(G - sh - 8 * k), FX_PAL[P.rim]);
        if ((i & 1) == 0) fxRect((int)(sx - 3 * k), (int)(G - sh * 0.6f), (int)(sx + 1 * k), (int)(G - sh * 0.35f), FX_PAL[FXP_HOT]);
        if (i == 2)
          for (int j = 0; j < 4; j++) {
            float ph2 = fmodf((P.live ? t * 0.0007f : 0.3f) + j / 4.0f, 1.0f);
            fxDisc(sx + 4 * k + ph2 * 6 * k, G - sh - 10 * k - ph2 * 18 * k, (1.5f + ph2 * 3) * k, FX_PAL[FXP_LINE]);
          }
      }
      break;
    }
    case SC_GLY_RAIN: case SC_GLY_STORM: case SC_GLY_CHEM: {
      bool chem = glyph == SC_GLY_CHEM, storm = glyph == SC_GLY_STORM;
      uint32_t tt = P.live ? t : 2500;
      for (int i = 0; i < (int)(W * 0.35f); i++) {           // rain, inside the panel
        uint32_t h = fxHash2((uint32_t)i, P.seed);
        float span = G - T + 20, y = T - 10 + fmodf(fxU(h) * span + tt * (storm ? 0.3f : 0.2f), span);
        float x = L + fxU(h >> 5) * (W + 20) - (y - T) * 0.25f, len = chem ? 3 : 7 + 5 * fxU(h >> 9);
        fxLine((int)x, (int)y, (int)(x - len * 0.25f), (int)(y + len), FX_PAL[chem ? ((h & 3) ? FXP_BLOOD : FXP_GLOW) : ((h & 1) ? FXP_OK : FXP_RUST)]);
      }
      if (storm && (!P.live || (t % 2600) < 180)) {
        FxRng Rr; Rr.s = P.seed;
        float bx = cx + 20 * k, by = T;
        while (by < G - 8) { float nx = bx + Rr.sgn() * 7 * k, ny = by + (5 + Rr.u() * 6) * k; fxStroke(bx, by, nx, ny, 2.6f, 2.0f, FX_PAL[FXP_WHITE]); bx = nx; by = ny; }
      }
      fig(cx - 8 * k, arch, SCP_HOLD, 1, 0, seat, k);
      scUmbrella(scHandX, scHandY, 12 * k, storm ? 1 : 0, chem, tt);
      if (storm)     scBang("KRAKOOM", cx + 20 * k, T + (G - T) * 0.35f, 10 * k, P.seed, 1);
      else if (chem) scBang("TSSS", cx + 24 * k, T + (G - T) * 0.35f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_FOG: {
      fig(cx - 4 * k, arch, SCP_STAND, 1, 0, seat, k);
      for (int y = (int)(T + (G - T) * 0.35f); y < P.y1; y++)
        for (int x = P.x0; x < P.x1; x++) {
          float n = fxNoise2(x * 0.05f + (P.live ? t * 0.0003f : 0), y * 0.12f, 71);
          float d = fxClampF((n - 0.25f) * 1.8f, 0, 0.92f);
          if (scDot(x, y, d)) scDither(x, y, 5.2f);
        }
      scBalloon("HELLO?", (int)(cx + 4 * k), (int)(G - 36 * k), (int)(cx + 2 * k), (int)(G - 26 * k));
      break;
    }
    case SC_GLY_RAD: {
      float sx = cx + 16 * k, sy = G - 30 * k;               // the sign, on its post
      fxStroke(sx, G, sx, sy, 2.0f, 2.0f, FX_PAL[FXP_INK]);
      fxDisc(sx, sy, 10 * k, FX_PAL[FXP_INK]);
      fxDisc(sx, sy, 9 * k, FX_PAL[FXP_CRIT]);
      for (int i = 0; i < 3; i++) {                          // the trefoil
        float a = -1.5708f + i * (FX_TAU / 3);
        fxTri(sx, sy, sx + cosf(a - 0.5f) * 8 * k, sy + sinf(a - 0.5f) * 8 * k, sx + cosf(a + 0.5f) * 8 * k, sy + sinf(a + 0.5f) * 8 * k, FX_PAL[FXP_INK]);
      }
      fxDisc(sx, sy, 2.4f * k, FX_PAL[FXP_CRIT]);
      fxDisc(sx, sy, 1.5f * k, FX_PAL[FXP_INK]);
      float gx = cx - 16 * k;                                // and someone who has started to glow
      for (int y = (int)(G - 40 * k); y < (int)G + 2; y++)
        for (int x = (int)(gx - 14 * k); x < (int)(gx + 14 * k); x++) {
          float dx = (x - gx) / (14 * k), dy = (y - (G - 16 * k)) / (22 * k), r = dx * dx + dy * dy;
          float pulse = P.live ? 0.4f + 0.25f * sinf(t * 0.005f) : 0.5f;
          if (r < 1 && scDot(x, y, (1 - r) * pulse)) scPx(x, y, FX_PAL[FXP_FLAME]);
        }
      ScFig F = { gx, G, 1, arch, SCP_STAND, seat, 0, FXP_GLOW, -0.7f, -0.7f, k };
      for (int p = 0; p < 3; p++) scFigure(F, p);
      scBang("TIK TIK", cx + 14 * k, T + (G - T) * 0.22f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_MEDAL: {
      fxSunburst(cx, G - 20 * k, 11, P.seed, P.live ? t * 0.00025f : 0.2f, FX_PAL[FXP_RUST]);
      fig(cx, arch, SCP_REACH, 1, 0, seat, k);
      if (k > 1.3f) fxMedallion(cx + 1.5f * k, G - 13 * k, P.live ? sinf(t * 0.003f) * 0.2f : 0.1f);
      else { fxDisc(cx + 1.5f * k, G - 13 * k, 3.4f, FX_PAL[FXP_INK]); fxDisc(cx + 1.5f * k, G - 13 * k, 2.4f, FX_PAL[FXP_CRIT]); }
      scBang("TA-DA", cx + 26 * k, T + (G - T) * 0.3f, 8 * k, P.seed, 1);
      break;
    }
    case SC_GLY_TRIP: scArtTrip(P, arch, arch2, seat, seat2); break;
    case SC_GLY_GUNFIRE: scArtGunfire(P, arch, arch2, seat, seat2); break;
    case SC_GLY_ROUGH: case SC_GLY_ROUGH_TREE: case SC_GLY_ROUGH_CHALK: {
      const uint8_t v = (uint8_t)(glyph - SC_GLY_ROUGH);
      float hx, hy;
      scArtRough(P, v, arch, seat, P.live ? (t - P.t0) % SCR_CYC : SCR_STILL[v], false, &hx, &hy);
      break;
    }
    default: {                                               // on the road, as ever
      fig(cx, arch, SCP_WALK, 1, ph, seat, k);
      break;
    }
  }
}

// The chart overhead: the waste's own zodiac, twelve signs strung on a belt
// round the sky, on a chart that wraps the way the map does -- the map is a
// torus, so is the sky over it. Once round the map east-west is once round
// the belt, so which signs are overhead says how far east you stand: walk
// east and the next one climbs in from the right. North-south tilts the belt
// higher or lower and slides the field stars behind it. A neighbour a hex
// away sees nearly the same sky; somebody across the map sees other signs.
// No names, no numbers -- the clusters, the faintest join inside each, and a
// scatter of field stars. For whoever learns to steer by them: the can, the
// jackal, the wheel, the crow, the skull, the bottle, the key, the scorpion,
// the lantern, the snake, the crown, the twins.
struct ScSky {
  float x, y;                                    // the chart point at the panel's top-left
  bool  moon;                                    // the broken moon is up (scMoon), at mx,my
  float mx, my, phase, low;                      // phase 0 new .. 0.5 full; low 1 on the horizon
};
static const int SC_CHART_W = 960, SC_CHART_H = 340;
struct ScAster { uint8_t n; int8_t x[7], y[7]; uint8_t mag[7]; const char* path; };   // path: star digits, ' ' lifts the pen
static const ScAster SC_ASTER[12] = {
  { 5, {  -9,   7,  10,  -8,  -3 },           {  -7,  -9,   8,  10, -15 },           { 1, 2, 1, 1, 0 },       "401230" },
  { 7, { -16, -11,  -8,   6,  14,  -6,   8 }, {  -6,  -9,  -3,  -2,  -8,   8,   9 }, { 2, 1, 0, 1, 0, 0, 0 }, "01234 25 36" },
  { 7, {  11,   5,  -6, -10,  -5,   6,   0 }, {   1,  -9,  -8,   0,   9,   9,   0 }, { 1, 0, 1, 0, 1, 0, 2 }, "0123450" },
  { 6, { -18,  -9,   0,   9,  17,   0 },      {  -6,   0,   4,  -1,  -8,  11 },      { 0, 1, 2, 1, 0, 1 },    "01234 25" },
  { 6, {  -7,   7, -10,  -4,   4,  10 },      {  -4,  -4,   6,  11,  11,   6 },      { 2, 2, 0, 1, 1, 0 },    "2345" },
  { 6, {   0,   1,  -6,  -6,   6,   6 },      { -15,  -8,  -2,  12,  12,  -2 },      { 0, 1, 1, 0, 1, 2 },    "0154321" },
  { 7, { -16, -12,  -6,   5,  14,  14,   9 }, {  -3,   3,   0,   0,   1,   7,   6 }, { 1, 0, 2, 0, 1, 0, 0 }, "0120 234 45 36" },
  { 7, { -16, -12,  -6,   1,   8,  13,  11 }, {  -6,  -1,   2,   4,   2,  -3,  -9 }, { 1, 0, 1, 2, 0, 1, 1 }, "0123456" },
  { 6, {   0,  -6,   6,  -7,   7,   0 },      { -14,  -6,  -6,   8,   8,   1 },      { 0, 1, 1, 0, 0, 2 },    "013420 12" },
  { 7, { -18, -11,  -4,   3,  10,  16,  20 }, {   4,  -3,   3,  -4,   2,  -5,  -2 }, { 1, 0, 1, 0, 1, 2, 0 }, "0123456" },
  { 7, { -12, -10,  -5,   0,   5,  10,  12 }, {   6,  -6,   1,  -9,   1,  -6,   6 }, { 0, 1, 0, 2, 0, 1, 0 }, "0123456" },
  { 6, {  -3,   3,  -5,   5,  -7,   7 },      { -10, -10,   0,   0,  10,  10 },      { 2, 2, 1, 1, 0, 0 },    "024 135 23" },
};
static inline float scWrapF(float v, float m) { v = fmodf(v, m); return v < 0 ? v + m : v; }
// Where sign i sits on the belt (x on the chart) and how far off its line
// (y, px): evenly round, each nudged so the spacing never looks ruled.
static void scAsterAt(int i, float* x, float* y) {
  uint32_t h = fxHash2((uint32_t)i, 0x57A2u);
  *x = i * (SC_CHART_W / 12.0f) + SC_CHART_W / 24.0f + fxS(h) * 10.0f;
  *y = fxS(fxHash(h)) * 5.0f;
}
// The broken moon: a bite out of its crown where something hit it, the
// rubble from it still drifting off the break, a crack run back across the
// face. Lit for its phase the northern way -- waxing on the right, waning on
// the left -- with the dark of it a shade above the sky, so the broken shape
// reads even as a sliver; a halo as full as it is; ruddy when it is low.
static void scMoon(float cx, float cy, float r, float ph, float low, uint32_t now) {
  const float th = ph * FX_TAU, lx = sinf(th), lz = -cosf(th);     // where the sun is, seen from it
  const float full = (1.0f - cosf(th)) * 0.5f;                      // how much of it is lit
  const float BX = 0.48f, BY = -0.82f, BITE_R = 0.4f;                   // the bite out of its crown, in radii (BR, CR: board macros)
  const uint8_t LIT = low > 0.55f ? FXP_FLAME : FXP_GLOW, HI = low > 0.55f ? FXP_HOT : FXP_WHITE;
  const int x0 = (int)floorf(cx - r * 2), x1 = (int)ceilf(cx + r * 2), y0 = (int)floorf(cy - r * 2), y1 = (int)ceilf(cy + r * 2);
  for (int y = y0; y <= y1; y++)                                    // the halo, thrown on the sky round it
    for (int x = x0; x <= x1; x++) {
      float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) / r;
      if (d > 1.05f && d < 1.7f && scDot(x, y, (1.7f - d) * 0.2f * full)) scPx(x, y, FX_PAL[FXP_EMBER]);
    }
  static const float CRATER[3][3] = { { -0.32f, 0.18f, 0.24f }, { 0.12f, 0.48f, 0.16f }, { -0.52f, -0.28f, 0.13f } };   // craters
  for (int y = (int)floorf(cy - r - 1); y <= (int)ceilf(cy + r + 1); y++)
    for (int x = (int)floorf(cx - r - 1); x <= (int)ceilf(cx + r + 1); x++) {
      float u = (x + 0.5f - cx) / r, v = (y + 0.5f - cy) / r, d2 = u * u + v * v;
      if (d2 > 1.0f) continue;
      float bu = u - BX, bv = v - BY, bd = sqrtf(bu * bu + bv * bv);
      float jag = BITE_R * (0.8f + 0.4f * fxNoise1(atan2f(bv, bu) * 2.4f + 3.0f, 77));
      if (bd < jag) continue;                                       // gone: the sky through it
      float z = sqrtf(1.0f - d2), lit = u * lx + z * lz;
      uint8_t c;
      if (bd < jag + 0.17f) c = lit > 0 ? FXP_EMBER : FXP_RUST;     // the broken edge, its insides showing
      else if (lit > 0.06f) {
        c = lit > 0.42f && d2 < 0.72f ? HI : LIT;
        for (int k = 0; k < 3; k++) {
          float cu = u - CRATER[k][0], cv = v - CRATER[k][1];
          if (cu * cu + cv * cv < CRATER[k][2] * CRATER[k][2]) c = c == HI ? LIT : FXP_HDR;
        }
      } else if (lit > -0.05f) c = scDot(x, y, 0.5f) ? FXP_HDR : FXP_DIM;   // the terminator, soft
      else c = FXP_DIM;                                              // the dark of it
      // the crack: from the break back down across the face
      float pu = u - (BX - 0.1f), pv = v - (BY + 0.4f);
      float t1 = fxClampF((pu * -0.4f + pv * 0.82f) / (0.16f + 0.6724f), 0.0f, 1.0f);
      float ex = pu + 0.4f * t1, ey = pv - 0.82f * t1;
      if (ex * ex + ey * ey < 0.0085f && lit > -0.05f) c = FXP_BRICK;
      scPx(x, y, FX_PAL[c]);
    }
  const float dx = 0.505f, dy = -0.863f;                            // the rubble, drifting off the break
  for (int k = 0; k < 5; k++) {
    uint32_t h = fxHash2((uint32_t)k, 0xB0C3u);
    float dist = 1.08f + k * 0.36f + 0.1f * sinf(now * 0.0007f + k * 1.7f), side = fxS(h) * 0.4f;
    int fx = (int)lroundf(cx + r * (dx * dist - dy * side)), fy = (int)lroundf(cy + r * (dy * dist + dx * side));
    uint16_t c = FX_PAL[full > 0.2f ? (k & 1 ? LIT : FXP_HDR) : FXP_LINE];
    scPx(fx, fy, c);
    if (k < 2) { scPx(fx + 1, fy, c); scPx(fx, fy + 1, FX_PAL[FXP_DIM]); scPx(fx + 1, fy + 1, FX_PAL[FXP_DIM]); }
  }
}

// The chart laid over the sky scPanelSet has just graded (top..hor), before
// the ruins go up in front of it. Stars wash out as the sky brightens toward
// the horizon, the way they do.
static void scStars(const ScStage& P, const ScSky& sky, float top, float hor) {
  const int w = P.x1 - P.x0, h = (int)P.gy - P.y0;
  auto skyLv = [&](int y) -> float {
    float u = (float)(y - P.y0) / fmaxf(1.0f, P.gy - P.y0);
    return fxLerp(top, hor, u * u * 0.8f + u * 0.2f);
  };
  auto star = [&](int x, int y, float lv, bool rel) {         // rel: lv is above the sky, not a rung
    if (x < P.x0 || x >= P.x1 || y <= P.y0 || y >= (int)P.gy - 2) return;
    float s = skyLv(y), vis = fxClampF((7.0f - s) / 4.5f, 0.0f, 1.0f);
    if (vis > 0) scDither(x, y, rel ? s + lv * vis : fxLerp(s, lv, vis));
  };
  const int CELL = 10, NC = SC_CHART_W / CELL, NR = SC_CHART_H / CELL;
  const int c0 = (int)floorf(sky.x / CELL), r0 = (int)floorf(sky.y / CELL);
  const float belt = h * 0.42f + h * 0.1f * sinf(sky.y * (FX_TAU / SC_CHART_H));   // how high it rides, by how far north
  for (int cr = r0; cr <= r0 + h / CELL + 1; cr++)            // the field: one cell in five holds a star, dim
    for (int cc = c0; cc <= c0 + w / CELL + 1; cc++) {
      uint32_t hh = fxHash2((uint32_t)(((cc % NC) + NC) % NC), (uint32_t)(((cr % NR) + NR) % NR) * 977u + 0x5EEDu);
      if (hh % 5) continue;
      if (P.live && (fxHash2(hh, P.now / 450) & 15) == 0) continue;
      int x = P.x0 + (int)floorf(cc * CELL + (float)((hh >> 8) % CELL) - sky.x);
      int y = P.y0 + (int)floorf(cr * CELL + (float)((hh >> 16) % CELL) - sky.y);
      star(x, y, (hh >> 24) % 8 == 0 ? 8.6f : 4.6f + ((hh >> 27) & 3) * 0.7f, false);
    }
  // a star of the chart: the brighter, the bigger -- a cross of glow round the leads
  auto lamp = [&](int x, int y, uint8_t mag, bool tw) {
    float c = mag == 2 ? 15.0f : mag == 1 ? 13.4f : 11.8f;
    if (tw) c -= 2.4f;
    star(x, y, c, false);
    if (mag == 0 || (mag == 1 && tw)) return;
    float a = mag == 2 ? c - 3.6f : c - 5.6f;
    star(x - 1, y, a, false); star(x + 1, y, a, false); star(x, y - 1, a, false); star(x, y + 1, a, false);
  };
  for (int i = 0; i < 12; i++) {
    const ScAster& A = SC_ASTER[i];
    float ax, ay;
    scAsterAt(i, &ax, &ay);
    float dx = scWrapF(ax - sky.x + 40.0f, (float)SC_CHART_W) - 40.0f;   // the nearest lap of it
    if (dx > w + 40) continue;
    float dy = belt + h * 0.08f * sinf(ax * (2.0f * FX_TAU / SC_CHART_W)) + ay;   // the belt winds twice round
    int sx[7], sy[7];
    for (int k = 0; k < A.n; k++) {                           // drawn a quarter over the table's size
      sx[k] = P.x0 + (int)floorf(dx + A.x[k] * 1.25f);
      sy[k] = P.y0 + (int)floorf(dy + A.y[k] * 1.25f);
    }
    int from = -1;
    for (const char* p = A.path;; p++) {                      // the joins, dotted, kept clear of the stars
      if (*p >= '0' && *p <= '9') {
        int to = *p - '0';
        if (from >= 0) {
          int n = fxMaxI(abs(sx[to] - sx[from]), abs(sy[to] - sy[from]));
          for (int s = 3; s <= n - 3; s += 2)
            star(sx[from] + (sx[to] - sx[from]) * s / n, sy[from] + (sy[to] - sy[from]) * s / n, 3.6f, true);
        }
        from = to;
      } else from = -1;
      if (!*p) break;
    }
    for (int k = 0; k < A.n; k++)
      lamp(sx[k], sy[k], A.mag[k], P.live && fxHash2((uint32_t)(i * 8 + k), P.now / 380) % 11 == 0);
  }
  if (sky.moon) scMoon(sky.mx, sky.my, 7.5f, sky.phase, sky.low, P.now);
}

// The sky a panel is set under: its tone decides the hour, its weather the
// rest -- and, handed the chart point somebody stands under, its stars are
// that place's (scStars) instead of a scatter.
static void scPanelSet(ScStage& P, uint8_t glyph, const ScSky* sky = nullptr) {
  float top = 3.4f, hor = 8.6f;
  switch (P.tone) {
    case SC_TONE_GOOD: top = 4.4f; hor = 11.0f; break;
    case SC_TONE_ILL:  top = 1.2f; hor = 7.4f;  break;
    case SC_TONE_OMEN: top = 0.3f; hor = 2.9f;  break;
  }
  if (glyph == SC_GLY_DAWN)                            { top = 2.0f; hor = 11.5f; }
  if (glyph == SC_GLY_REST || glyph == SC_GLY_DEATH)   { top = 0.2f; hor = 2.6f; }
  if (glyph == SC_GLY_RAIN || glyph == SC_GLY_CHEM)    { top = 1.8f; hor = 5.2f; }
  if (glyph == SC_GLY_STORM)                           { top = 0.6f; hor = 3.2f; }
  if (glyph == SC_GLY_FOG)                             { top = 5.0f; hor = 6.4f; }
  P.night = top < 1.0f;
  P.rim = (uint8_t)fxClampF(hor - 1.0f, 7.0f, 11.0f);
  if (sky) top = fminf(top, 0.4f);                           // a sky with a chart in it goes dark enough to read
  for (int y = P.y0; y < (int)P.gy; y++) {
    float u = (float)(y - P.y0) / fmaxf(1.0f, P.gy - P.y0);
    scTone(y, P.x0, P.x1, fxLerp(top, hor, u * u * 0.8f + u * 0.2f));
  }
  if (sky)
    scStars(P, *sky, top, hor);
  else if (P.night)
    for (int i = 0; i < (P.x1 - P.x0) / 5; i++) {
      uint32_t h = fxHash2((uint32_t)i, P.seed);
      int x = P.x0 + (int)(h % (uint32_t)(P.x1 - P.x0)), y = P.y0 + 2 + (int)((h >> 9) % (uint32_t)fxMaxI(1, (int)(P.gy - P.y0) / 2));
      if (P.live && (fxHash2((uint32_t)i, P.now / 450) & 7) == 0) continue;
      scPx(x, y, FX_PAL[(h >> 20) % 5 == 0 ? FXP_GLOW : FXP_LINE]);
    }
  if (glyph != SC_GLY_LIGHT && glyph != SC_GLY_CLASH)       // both of those are indoors
    scRuins((float)(P.seed % 3000), (int)P.gy, fxClampF(hor - 3.0f, 0.8f, 8.0f), P.night, P.now, P.seed,
            fxMaxI(12, (int)(16 * P.k)), (int)(5 * P.k), (int)((P.gy - P.y0) * 0.42f));
  for (int y = (int)P.gy; y < P.y1; y++) {
    float u = (float)(y - (int)P.gy) / fmaxf(1.0f, (float)(P.y1 - (int)P.gy));
    scTone(y, P.x0, P.x1, fxLerp(fxClampF(hor - 5.0f, 1.0f, 5.0f), 0.6f, u));
  }
}

static struct { uint16_t serial; uint32_t t0; bool have; } scChronSt = { 0, 0, false };

// A caption box, lettered in capitals, `lines` rows of `cols` characters.
static void scCaption(const char* s, int x, int y, int cols, int lines, int typed) {
  char ln[6][40];
  int n = scWrapCols(s, cols, ln, lines, true), w = 0;
  for (int i = 0; i < n; i++) w = fxMaxI(w, (int)strlen(ln[i]));
  int bw = w * 6 + 8, bh = n * 9 + 5;
  fxRect(x + 2, y + 2, x + bw + 2, y + bh + 2, FX_PAL[FXP_INK]);
  fxRect(x - 1, y - 1, x + bw + 1, y + bh + 1, FX_PAL[FXP_INK]);
  fxRect(x, y, x + bw, y + bh, FX_PAL[FXP_CRIT]);
  for (int i = 0; i < n; i++) {
    char t[40];
    int k = fxMinI((int)strlen(ln[i]), fxMaxI(0, typed));
    memcpy(t, ln[i], k); t[k] = 0;
    fxGlcd(t, x + 4, y + 3 + i * 9, 1, FX_PAL[FXP_INK]);
    typed -= (int)strlen(ln[i]);
  }
}

// One whole panel: the set, the picture, the day tab, the caption, the border.
static void scPanel(const SceneSnap& S, const SceneLog* e, int x0, int y0, int x1, int y1, bool big,
                    uint32_t now, float wipe) {
  ScStage P;
  P.x0 = x0; P.y0 = y0; P.x1 = x1; P.y1 = y1;
  P.k    = big ? 2.1f : 1.05f;
  P.gy   = big ? (float)(y1 - 16) : (float)(y1 - 44);
  P.cx   = (x0 + x1) * 0.5f;
  P.tone = e ? e->tone : SC_TONE_PLAIN;
  P.seed = e ? fxHash(e->serial * 2654435761u + 7) : 99;
  P.live = big;
  P.now  = big ? now : 3000 + (P.seed % 4000);
  P.t0   = big ? scChronSt.t0 : 0;
  uint8_t glyph = e ? e->glyph : SC_GLY_NONE;
  int cx1 = x1;
  if (wipe < 1.0f) cx1 = x0 + (int)((x1 - x0) * wipe);      // the new panel prints in from the left
  fxRect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, FX_PAL[FXP_INK]);
  scClipRect(x0, y0, cx1, y1);
  if (!e) {
    for (int y = y0; y < y1; y++) scTone(y, x0, cx1, 1.4f);
    fxGlcd("...", (x0 + x1) / 2 - 9, (y0 + y1) / 2 - 4, 1, FX_PAL[FXP_LINE]);
    scUnclip();
    return;
  }
  scPanelSet(P, glyph);
  uint8_t arch = (e->who >= 0 && e->who < SC_SEATS) ? S.s[e->who].arch : (uint8_t)(P.seed % 6);
  uint8_t arch2 = (e->who2 >= 0 && e->who2 < SC_SEATS) ? S.s[e->who2].arch : (uint8_t)((P.seed >> 8) % 6);
  scPanelArt(P, glyph, arch, arch2, e->who, e->who2);
  // the day, on a tab in the corner
  char d[12];
  snprintf(d, sizeof(d), "DAY %u", (unsigned)e->day);
  int dw = scTypeW(d, 1);
  fxRect(x1 - dw - 7, y0, x1, y0 + 11, FX_PAL[FXP_INK]);
  fxGlcd(d, x1 - dw - 3, y0 + 2, 1, FX_PAL[FXP_HDR]);
  if (big) {
    int typed = (int)((now - scChronSt.t0) / 30);
    scCaption(e->text, x0 + 5, y0 + 5, 27, 3, typed);
  } else {
    fxRect(x0, y1 - 42, x1, y1, FX_PAL[FXP_INK]);
    fxRect(x0, y1 - 42, x1, y1 - 41, FX_PAL[FXP_HDR]);
    char ln[6][40];
    int n = scWrapCols(e->text, 18, ln, 4, true);
    for (int i = 0; i < n; i++) fxGlcd(ln[i], x0 + 4, y1 - 38 + i * 9, 1, FX_PAL[FXP_CRIT]);
  }
  scUnclip();
  if (!big)                                                  // a still panel has gone to print: a shade quieter
    for (int y = y0; y < y1; y++) {
      uint16_t* row = fxDst + y * FX_W;
      for (int x = x0; x < x1; x++) if (row[x]) row[x] = fxDim(row[x], 205);
    }
}

static void scChronicleDraw(const SceneSnap& S, uint32_t now) {
  uint16_t* const out = fxDst;
  const SceneLog* newest = S.logN ? &S.log[S.logN - 1] : nullptr;
  const SceneLog* prev1  = S.logN >= 2 ? &S.log[S.logN - 2] : nullptr;
  const SceneLog* prev2  = S.logN >= 3 ? &S.log[S.logN - 3] : nullptr;
  if (newest && (!scChronSt.have || newest->serial != scChronSt.serial)) {
    if (scChronSt.have) fxImpact(0.32f);                     // it lands
    scChronSt.serial = newest->serial;
    scChronSt.t0 = now;
    scChronSt.have = true;
  }

  // The page and the two still panels are the still layer; only the newest moves.
  uint32_t key = 0xC4201C1Eu ^ (uint32_t)S.logN;
  for (int i = 0; i < S.logN; i++) {
    key = fxHash2(key, S.log[i].serial);
    for (const char* p = S.log[i].text; *p; p++) key = fxHash2(key, (uint8_t)*p);
  }
  key = fxHash2(key, S.day);
  for (int i = 0; i < SC_SEATS; i++) key = fxHash2(key, S.s[i].arch);
  if (!scStillTried) { scStillTried = true; scStill = (uint16_t*)fxAlloc((size_t)FX_W * FX_H * 2); }
  if (!scStill || scStillKey != key) {
    if (scStill) fxDst = scStill;
    for (int y = 0; y < FX_H; y++)                           // the page: newsprint, in dots
      for (int x = 0; x < FX_W; x++) fxDst[y * FX_W + x] = FX_PAL[scDot(x, y, 0.35f) ? FXP_TRACK : FXP_BAND];
    fxRect(0, 0, FX_W, 19, FX_PAL[FXP_INK]);
    fxRect(0, 18, FX_W, 19, FX_PAL[FXP_LINE]);
    scType("THE CHRONICLE", 4, 2, 2, FXP_HDR);
    char b[16];
    snprintf(b, sizeof(b), "DAY %u", (unsigned)S.day);
    scType(b, 237 - scTypeW(b, 1), 7, 1, FXP_CRIT);
    scPanel(S, prev2, 4, 23, 116, 131, false, now, 1.0f);
    scPanel(S, prev1, 124, 23, 236, 131, false, now, 1.0f);
    fxRect(0, 308, FX_W, FX_H, FX_PAL[FXP_INK]);
    snprintf(b, sizeof(b), "fol. %u", (unsigned)S.logTotal);
    fxGlcd(b, 237 - scTypeW(b, 1), 310, 1, FX_PAL[FXP_LINE]);
    fxGlcd("kept by hand, drawn badly", 3, 310, 1, FX_PAL[FXP_DIM]);
    scStillKey = scStill ? key : 0;
    fxDst = out;
  }
  if (scStill) memcpy(out, scStill, (size_t)FX_W * FX_H * 2);

  if (!newest) {
    // Nothing yet. The blank panel says so.
    fxRect(2, 134, 238, 306, FX_PAL[FXP_INK]);
    scClipRect(4, 136, 236, 304);
    ScStage P = { 4, 136, 236, 304, 288, 120, 1.55f, SC_TONE_PLAIN, 9, 0x1234, now, true, false };
    scPanelSet(P, SC_GLY_NONE);
    ScFig F = { 120, 288, 1, 0, SCP_SIT, 255, 0, P.rim, -0.7f, -0.7f, 1.55f };
    for (int p = 0; p < 3; p++) scFigure(F, p);
    scCaption("THE PAGE IS STILL BLANK. NOTHING HAS HAPPENED YET. OUT HERE, THAT IS A GOOD DAY.", 9, 141, 30, 3, (int)(now / 30));
    scUnclip();
    return;
  }
  float wipe = fxClampF((now - scChronSt.t0) / 420.0f, 0.0f, 1.0f);
  scPanel(S, newest, 4, 136, 236, 304, true, now, fxCubicOut(wipe));
}

// ── 9. Screen 3: THE STASH ───────────────────────────────────────────────────
// One survivor at a time, turning over every eight seconds: everything they
// own laid out on a tarp the way you do before you decide what to leave
// behind. The five resources are piles you can count -- bottles, tins, cans,
// kits, a heap of scrap -- with the number on a tag under each. The pack's
// slots are a row of boxes, and what is in it is on the manifest.
//
//   y   0-19   masthead    THE STASH                         o o * o  (pages)
//   y  20-161  the tarp    the survivor sorting it, the five piles and tags
//   y 163-181  who         [n] name, what they are, pack slots as boxes
//   y 184-297  manifest    two columns: item, xqty
//   y 300-319  the remark
static const char* const SC_ARCH_NAME[6] = { "GUIDE", "QUARTERMASTER", "MEDIC", "MULE", "SCOUT", "ENDURER" };
static const char* const SC_RES_NAME[5]  = { "WATER", "FOOD", "FUEL", "MEDS", "SCRAP" };

static void scBottle(float x, float y) {                     // base centre
  fxRect((int)x - 3, (int)y - 11, (int)x + 3, (int)y, FX_PAL[FXP_INK]);
  fxRect((int)x - 2, (int)y - 10, (int)x + 2, (int)y - 1, FX_PAL[FXP_OK]);
  fxRect((int)x - 1, (int)y - 14, (int)x + 1, (int)y - 10, FX_PAL[FXP_INK]);
  scPx((int)x - 1, (int)y - 9, FX_PAL[FXP_WHITE]);
  fxLine((int)x - 1, (int)y - 8, (int)x - 1, (int)y - 3, FX_PAL[FXP_GLOW]);
  fxRect((int)x - 2, (int)y - 5, (int)x + 2, (int)y - 4, FX_PAL[FXP_EMBER]);   // what is left in it
}
static void scTin(float x, float y) {
  fxRect((int)x - 4, (int)y - 7, (int)x + 4, (int)y, FX_PAL[FXP_INK]);
  fxRect((int)x - 3, (int)y - 6, (int)x + 3, (int)y - 1, FX_PAL[FXP_RUST]);
  fxRect((int)x - 3, (int)y - 5, (int)x + 3, (int)y - 3, FX_PAL[FXP_HDR]);    // the label, mostly
  fxLine((int)x - 3, (int)y - 6, (int)x + 2, (int)y - 6, FX_PAL[FXP_GLOW]);
}
static void scJerry(float x, float y) {
  fxRect((int)x - 5, (int)y - 13, (int)x + 5, (int)y, FX_PAL[FXP_INK]);
  fxRect((int)x - 4, (int)y - 12, (int)x + 4, (int)y - 1, FX_PAL[FXP_BRICK]);
  fxLine((int)x - 3, (int)y - 11, (int)x + 3, (int)y - 2, FX_PAL[FXP_RUST]);
  fxLine((int)x + 3, (int)y - 11, (int)x - 3, (int)y - 2, FX_PAL[FXP_RUST]);
  fxRect((int)x - 3, (int)y - 16, (int)x + 1, (int)y - 13, FX_PAL[FXP_INK]);  // the handle
  fxRect((int)x + 2, (int)y - 15, (int)x + 4, (int)y - 13, FX_PAL[FXP_INK]);  // the spout
  fxLine((int)x - 4, (int)y - 12, (int)x - 4, (int)y - 2, FX_PAL[FXP_HOT]);
}
static void scKit(float x, float y) {
  fxRect((int)x - 5, (int)y - 7, (int)x + 5, (int)y, FX_PAL[FXP_INK]);
  fxRect((int)x - 4, (int)y - 6, (int)x + 4, (int)y - 1, FX_PAL[FXP_GLOW]);
  fxRect((int)x - 1, (int)y - 6, (int)x + 1, (int)y - 1, FX_PAL[FXP_BLOOD]);
  fxRect((int)x - 3, (int)y - 4, (int)x + 3, (int)y - 3, FX_PAL[FXP_BLOOD]);
}
// The scrap pile's pieces, centred where they sit: lit from up and left,
// bright enough to read on the tarp the way the bottles and kits do.
static void scCog(float x, float y, float r, float turn) {    // a gear: teeth, a hub, the hole through it
  const int T = r > 4.0f ? 8 : 6;
  for (int pass = 0; pass < 2; pass++) {                      // inked a pixel wider first
    const uint16_t c = FX_PAL[pass ? FXP_BRICK : FXP_INK];
    const float g = pass ? 0.0f : 1.0f;
    fxDisc(x, y, r - 1.0f + g, c);
    for (int i = 0; i < T; i++) {
      float a = turn + i * (FX_TAU / T);
      fxDisc(x + cosf(a) * (r - 0.4f), y + sinf(a) * (r - 0.4f), 1.05f + g * 0.75f, c);
    }
  }
  fxDisc(x - 0.6f, y - 0.6f, r * 0.62f, FX_PAL[FXP_HDR]);     // the face
  fxDisc(x - 1.2f, y - 1.2f, r * 0.3f, FX_PAL[FXP_GLOW]);
  fxDisc(x, y, fmaxf(0.75f, r * 0.24f), FX_PAL[FXP_INK]);     // the hole
}
static void scTyre(float x, float y, float r) {               // a bald one stood on end, the rim still in it
  fxDisc(x, y, r + 1.0f, FX_PAL[FXP_INK]);
  fxDisc(x, y, r, FX_PAL[FXP_DIM]);
  for (int i = 0; i < 14; i++) {                              // what is left of the tread
    float a = i * (FX_TAU / 14);
    if (i & 1) scPx((int)lroundf(x + cosf(a) * (r - 0.4f)), (int)lroundf(y + sinf(a) * (r - 0.4f)), FX_PAL[FXP_SOOT]);
  }
  for (int i = 0; i < 5; i++) {                               // the light along its shoulder
    float a = 3.4f + i * 0.28f;
    scPx((int)lroundf(x + cosf(a) * (r - 1.2f)), (int)lroundf(y + sinf(a) * (r - 1.2f)), FX_PAL[FXP_EMBER]);
  }
  fxDisc(x, y, r * 0.5f + 0.9f, FX_PAL[FXP_INK]);
  fxDisc(x, y, r * 0.5f, FX_PAL[FXP_OK]);                     // the rim
  fxDisc(x - 0.7f, y - 0.7f, r * 0.22f, FX_PAL[FXP_GLOW]);
  fxDisc(x, y, 0.9f, FX_PAL[FXP_INK]);
}
static void scSheet(int x0, int y0, int x1, int y1, int w) {   // corrugated iron leant up, bottom (x0,y0) top (x1,y1)
  float ox[4] = { x0 - 1.0f, x0 + w + 1.0f, x1 + w + 1.0f, x1 - 1.0f }, oy[4] = { y0 + 0.5f, y0 + 0.5f, y1 - 1.5f, y1 - 1.5f };
  fxPoly(ox, oy, 4, FX_PAL[FXP_INK]);
  for (int i = 0; i < w; i++)                                 // the ridges, one lit in three
    fxLine(x0 + i, y0 - 1, x1 + i, y1, FX_PAL[i % 3 == 0 ? FXP_GLOW : i % 3 == 1 ? FXP_HDR : FXP_RUST]);
  fxLine(x1, y1 - 1, x1 + w - 1, y1 - 1, FX_PAL[FXP_WHITE]);
  fxRect((x0 + x1) / 2 + w / 2 - 1, (y0 + y1) / 2, (x0 + x1) / 2 + w / 2 + 1, (y0 + y1) / 2 + 2, FX_PAL[FXP_INK]);   // rusted through
}
static void scPipe(float ax, float ay, float bx, float by, float cx, float cy) {   // a pipe with one bend in it
  fxStroke(ax, ay, bx, by, 4.4f, 4.4f, FX_PAL[FXP_INK]);
  fxStroke(bx, by, cx, cy, 4.4f, 4.4f, FX_PAL[FXP_INK]);
  fxDisc(bx, by, 2.2f, FX_PAL[FXP_INK]);
  fxStroke(ax, ay, bx, by, 2.4f, 2.4f, FX_PAL[FXP_OK]);
  fxStroke(bx, by, cx, cy, 2.4f, 2.4f, FX_PAL[FXP_OK]);
  fxDisc(bx, by, 1.2f, FX_PAL[FXP_OK]);
  fxLine((int)ax, (int)ay - 1, (int)bx - 1, (int)by - 1, FX_PAL[FXP_GLOW]);
  fxLine((int)cx - 1, (int)cy + 1, (int)bx - 1, (int)by - 2, FX_PAL[FXP_GLOW]);
  fxRect((int)cx - 1, (int)cy - 1, (int)cx + 2, (int)cy + 1, FX_PAL[FXP_INK]);   // the open end
}

static struct { int8_t page; uint32_t t0; float skyX, skyY; uint32_t skyT; bool skyHave; } scStashSt = { -1, 0, 0, 0, 0, false };

// A name cut to `room` characters the way a clerk would: in capitals, and if
// it does not fit, stopped short with a full stop rather than lopped mid-word.
static void scAbbrev(char* out, size_t cap, const char* s, int room) {
  int n = (int)strlen(s);
  room = fxMaxI(2, fxMinI(room, (int)cap - 1));
  if (n <= room) {
    memcpy(out, s, n); out[n] = 0;
  } else {
    int k = room - 1;
    while (k > 1 && s[k - 1] == ' ') k--;
    memcpy(out, s, k); out[k++] = '.'; out[k] = 0;
  }
  for (char* p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
}

// The others, steered by: every survivor has a sign of their own -- a tight
// knot of bright stars with no lines in it that never twinkles, which is how
// it differs from the zodiac -- hung in the sky over wherever they stand, and
// your own sits at the zenith. East is right and north is up, at the chart's
// own scale (a hex east is SC_CHART_W/cols px, a row north SC_CHART_H/rows; a
// step along q is half a row south as well, as on the map). Anyone past the
// rim of what you can see hangs low on the rim along the line to them, dimmer,
// the way a far sign sits on the horizon. Nothing says whose sign is whose:
// you are always the one overhead on your own page, and the rest is for
// whoever is paying attention. Placed from the gliding sky, so they slide
// with the stars when the page turns.
struct ScSeatSign { uint8_t n; int8_t x[5], y[5]; };
static const ScSeatSign SC_SEAT_SIGN[SC_SEATS] = {
  { 3, {  0, -5,  5 },         { -5,  4,  4 } },             // the tripod
  { 3, { -6,  0,  6 },         {  4,  0, -4 } },             // the belt
  { 4, {  0, -5,  0,  5 },     { -6,  0,  6,  0 } },         // the diamond
  { 4, { -5, -5,  0,  6 },     { -5,  1,  5,  4 } },         // the hook
  { 5, {  0,  0,  0, -6,  6 }, { -6,  0,  6,  0,  0 } },     // the cross
  { 3, { -3,  3,  0 },         { -2, -2,  5 } },             // the pair and its drop
};
static const float SC_ZENITH_X = 120, SC_ZENITH_Y = 68;   // clear of the caption above and the moon's arc below
// boxR/boxB: the narration box's far corner -- a sign that would hide under
// it is hung just below it instead.
static void scStashWanderers(const SceneSnap& S, int pid, const ScSky& sky, int cols, int rows, int boxR, int boxB) {
  const float sq = (float)SC_CHART_W / cols, sr = (float)SC_CHART_H / rows;
  const float L = 10, R = 229, T = 28, B = 108;              // the rim, a sign's width in
  struct Mark { float px, py; uint8_t seat; bool off, below, side; };   // side: pinned to the left or right rim
  Mark m[SC_SEATS];
  int n = 0;
  for (int i = 0; i < SC_SEATS; i++) {
    if (!S.s[i].on) continue;
    const SceneSurv& o = S.s[i];
    Mark& k = m[n++];
    k.seat = (uint8_t)i; k.below = o.depth > 0; k.off = false; k.side = false;
    if (i == pid) { k.px = SC_ZENITH_X; k.py = SC_ZENITH_Y; continue; }
    float X = scWrapF(o.q * sq - sky.x - SC_ZENITH_X + SC_CHART_W * 0.5f, (float)SC_CHART_W) - SC_CHART_W * 0.5f;
    float Y = scWrapF(o.r * sr - sky.y - (SC_ZENITH_Y - 20) + SC_CHART_H * 0.5f, (float)SC_CHART_H) - SC_CHART_H * 0.5f
              + X / sq * 0.5f * sr;
    k.px = SC_ZENITH_X + X; k.py = SC_ZENITH_Y + Y;
    k.off = !(k.px >= L && k.px <= R && k.py >= T && k.py <= B);
    if (k.off) {                                             // pulled in to the rim along the line to them
      float t = 1.0f, tx = 1.0f;
      if (k.px < L) tx = fminf(tx, (L - SC_ZENITH_X) / X);
      if (k.px > R) tx = fminf(tx, (R - SC_ZENITH_X) / X);
      if (k.py < T) t = fminf(t, (T - SC_ZENITH_Y) / Y);
      if (k.py > B) t = fminf(t, (B - SC_ZENITH_Y) / Y);
      k.side = tx < t;
      t = fminf(t, tx);
      k.px = SC_ZENITH_X + X * t; k.py = SC_ZENITH_Y + Y * t;
    }
  }
  // Signs that would land on one another -- a party on one hex, or two off the
  // same way past the rim -- are hung side by side instead, along the rim for
  // the far ones.
  bool used[SC_SEATS] = {};
  for (int a = 0; a < n; a++) {
    if (used[a]) continue;
    int g[SC_SEATS], gn = 0;
    for (int b = a; b < n; b++)
      if (!used[b] && m[b].off == m[a].off && fabsf(m[b].px - m[a].px) < 17 && fabsf(m[b].py - m[a].py) < 17) { used[b] = true; g[gn++] = b; }
    if (gn < 2) continue;
    const bool vert = m[a].off && m[a].side;
    const float cx = m[a].px, cy = m[a].py;
    for (int j = 0; j < gn; j++) {
      float o = (j - (gn - 1) * 0.5f) * 18.0f;
      m[g[j]].px = vert ? cx : fxClampF(cx + o, L, R);
      m[g[j]].py = vert ? fxClampF(cy + o, T, B) : cy;
    }
  }
  for (int a = 0; a < n; a++) {
    Mark& k = m[a];
    if (k.px < boxR + 7 && k.py < boxB + 7) k.py = fminf(B, (float)boxB + 7);   // out from under the caption
    const ScSeatSign& G = SC_SEAT_SIGN[k.seat];
    const uint8_t lv = (uint8_t)(k.off + k.below);           // 0 overhead, 1 far or below, 2 both
    const uint16_t c = FX_PAL[lv == 0 ? FXP_WHITE : lv == 1 ? FXP_CRIT : FXP_HDR];
    const uint16_t arm = FX_PAL[lv == 0 ? FXP_GLOW : lv == 1 ? FXP_FLAME : FXP_RUST];
    for (int pass = 0; pass < 2; pass++)                     // inked round first, so a sign cuts through the zodiac behind it
      for (int s = 0; s < G.n; s++) {
        int x = (int)lroundf(k.px) + G.x[s], y = (int)lroundf(k.py) + G.y[s];
        if (x < 1 || x > FX_W - 2 || y < 23 || y > 114) continue;
        if (!pass) { fxDisc((float)x, (float)y, 2.7f, FX_PAL[FXP_INK]); continue; }
        scPx(x - 1, y, arm); scPx(x + 1, y, arm); scPx(x, y - 1, arm); scPx(x, y + 1, arm);
        scPx(x, y, c);
      }
  }
}

static void scStashDraw(const SceneSnap& S, uint32_t now) {
  int seated[SC_SEATS], nOn = 0;
  for (int i = 0; i < SC_SEATS; i++) if (S.s[i].on) seated[nOn++] = i;
  const int pid = (S.page >= 0 && S.page < SC_SEATS && S.s[S.page].on) ? S.page : -1;
  if (pid != scStashSt.page) { scStashSt.page = (int8_t)pid; scStashSt.t0 = now; }
  const uint32_t since = now - scStashSt.t0;

  // -- the stars over wherever they stand (scStars), their zenith at
  //    SC_ZENITH_X/Y. When the page turns the chart slides across to the next
  //    survivor's sky, the short way round --
  const int cols = S.cols > 0 ? S.cols : 75, rows = S.rows > 0 ? S.rows : 57;
  if (pid >= 0) {
    const float tx = scWrapF(S.s[pid].q * (float)SC_CHART_W / cols - SC_ZENITH_X, (float)SC_CHART_W);
    const float ty = scWrapF(S.s[pid].r * (float)SC_CHART_H / rows - (SC_ZENITH_Y - 20), (float)SC_CHART_H);
    if (!scStashSt.skyHave) { scStashSt.skyX = tx; scStashSt.skyY = ty; scStashSt.skyHave = true; }
    const float k  = 1.0f - expf(-(float)(now - scStashSt.skyT) / 260.0f);
    const float dx = scWrapF(tx - scStashSt.skyX + SC_CHART_W * 0.5f, (float)SC_CHART_W) - SC_CHART_W * 0.5f;
    const float dy = scWrapF(ty - scStashSt.skyY + SC_CHART_H * 0.5f, (float)SC_CHART_H) - SC_CHART_H * 0.5f;
    scStashSt.skyX = scWrapF(scStashSt.skyX + dx * k, (float)SC_CHART_W);
    scStashSt.skyY = scWrapF(scStashSt.skyY + dy * k, (float)SC_CHART_H);
  }
  scStashSt.skyT = now;
  ScSky sky = { scStashSt.skyX, scStashSt.skyY };
  // -- the broken moon (scMoon). Thirteen of its months to the year, so the
  //    year's wrap is seamless; up for one night's length, later each night
  //    the way ours is: full rises at nightfall and sets at dawn, the first
  //    quarter is high at nightfall and gone by midnight, the last quarter
  //    rises at midnight, a crescent only shows near dusk or dawn. It goes
  //    the northern way, facing south: up out of the left behind the ruins,
  //    low across, down on the right --
  {
    const float t = S.dayMil / 1000.0f, NIGHTFALL = 0.745f;          // where the road's sun goes down
    const float ph = scWrapF(((S.day ? S.day - 1 : 0) + t) / (365.0f / 13.0f) + 0.15f, 1.0f);
    const float u = (t - NIGHTFALL) / (1.0f - NIGHTFALL) + 1.0f - 2.0f * ph;   // 0 rising .. 1 setting
    if (t >= NIGHTFALL && u >= 0.0f && u <= 1.0f) {
      const float arc = sinf(u * 3.14159265f);
      sky.moon = true; sky.phase = ph; sky.low = 1.0f - arc;
      sky.mx = -10.0f + u * 260.0f; sky.my = 122.0f - 34.0f * arc;
    }
  }

  // -- the set: dusk over the ruins, the tarp in the dirt --
  ScStage P = { 0, 20, FX_W, 162, 150, 120, 1.35f, SC_TONE_ILL, 9, 0x57A5u + (uint32_t)(pid + 1) * 977u, now, true, false };
  scPanelSet(P, SC_GLY_NONE, scStashSt.skyHave ? &sky : nullptr);
  for (int y = 118; y < 162; y++) {
    float u = (y - 118) / 44.0f, xl = 64 - u * 20, xr = 236 + u * 8;
    for (int x = (int)xl; x < (int)xr && x < FX_W; x++) {
      float crease = fabsf(fxNoise1(x * 0.08f + y * 0.02f, 91) - 0.5f) < 0.03f ? -1.4f : 0.0f;
      scDither(x, y, 4.6f + u * 1.4f + crease + 0.6f * (fxNoise2(x * 0.1f, y * 0.2f, 7) - 0.5f));
    }
    scPx((int)xl, y, FX_PAL[FXP_INK]);
  }
  fxLine(64, 118, 236, 118, FX_PAL[FXP_HDR]);                 // the far edge catches the light

  if (pid < 0) {
    ScFig F = { 34, 150, 1, 3, SCP_SIT, 255, 0, P.rim, -0.7f, -0.7f, 1.35f };
    for (int p = 0; p < 3; p++) scFigure(F, p);
    scNarration("NOBODY'S STUFF. JUST|A TARP. FINE TARP.|NO SALOON TAKES TARPS.", 5, 25, since);
  } else {
    const SceneSurv& v = S.s[pid];
    // A while into the page they pick something up out of the dirt, hold it
    // up to what light there is, and decide about it out loud.
    float ins = 0;
    if (since > 1800 && since < 4400)
      ins = fxSmooth(fxClampF((since - 1800) / 350.0f, 0, 1)) * fxSmooth(fxClampF((4400 - (float)since) / 350.0f, 0, 1));
    ScFig F = { 30, 150, 1, v.arch, SCP_CROUCH, (uint8_t)pid, 0, P.rim, -0.7f, -0.7f, 1.45f };
    if (ins > 0) { F.handU = fxLerp(10.4f, 11.0f, ins); F.handV = fxLerp(1.6f, 16.8f, ins); F.look = fxLerp(-0.55f, 0.08f, ins); }
    for (int p = 0; p < 3; p++) scFigure(F, p);
    if (ins > 0.3f) scTin(scHandX, scHandY + 3);
    if (ins > 0.95f) {
      static const char* const VERDICT[] = { "KEEP.", "HMM.", "MINE.", "EDIBLE?", "TRASH.", "LUCKY.", "SMELLS OK." };
      scBalloon(VERDICT[fxHash((uint32_t)pid * 131u + (uint32_t)v.packUsed * 7u + (uint32_t)(now / 8000)) % SC_NN(VERDICT)],
                76, 102, (int)scHandX + 5, (int)scHandY - 6);
    }

    // -- the piles; they drop onto the tarp when the page turns --
    static const float PX[5] = { 82, 118, 154, 188, 220 };
    const float base = 136;
    for (int r = 0; r < 5; r++) {
      float drop = fxClampF((since - r * 70) / 260.0f, 0, 1);
      float yo = -(1.0f - fxBackOut(drop)) * 40.0f;
      if (drop <= 0) continue;
      float x = PX[r], y = base + yo;
      int c = v.res[r];
      switch (r) {
        case 0:                                              // bottles, two rows, the back row first
          for (int i = 3; i < fxMinI(c, 6); i++) scBottle(x - 4 + (i - 3) * 8, y - 4);
          for (int i = 0; i < fxMinI(c, 3); i++) scBottle(x - 8 + i * 8, y);
          break;
        case 1: {                                            // a pyramid of tins, 3-2-1
          static const float TX[6] = { -9, 0, 9, -4.5f, 4.5f, 0 }, TY[6] = { 0, 0, 0, -7, -7, -14 };
          for (int i = 0; i < fxMinI(c, 6); i++) scTin(x + TX[i], y + TY[i]);
          if (c > 0) {                                       // and the fly that found them
            float a = now * 0.011f;
            scPx((int)(x + cosf(a) * 7 + sinf(a * 2.3f) * 3), (int)(y - 20 + sinf(a * 1.7f) * 4), FX_PAL[FXP_INK]);
          }
          break;
        }
        case 2: for (int i = 0; i < fxMinI(c, 3); i++) scJerry(x - 8 + i * 8, y - (i & 1) * 2); break;
        case 3: for (int i = 0; i < fxMinI(c, 4); i++) scKit(x + (i & 1) * 3 - 1, y - i * 7); break;
        case 4: if (c > 0) {                                 // the junk itself, a piece more for each one held
          const int n = fxMinI(c, 12);
          const float ox = n == 1 ? -7.0f : 0.0f;            // one cog alone sits in the middle
          const int sw = 8 + fxMinI(n, 6) * 2;
          for (int k = 0; k < 2; k++) fxSpan((int)y - k, (int)x - sw + k * 3, (int)x + sw - k * 3, FX_PAL[FXP_TRACK]);   // its shadow
          if (n >= 3) scSheet((int)x - 19, (int)y - 1, (int)x - 13, (int)y - 23, 7);
          if (n >= 4) scPipe(x + 2, y - 4, x + 13, y - 4, x + 14, y - 19);
          if (n >= 2) scTyre(x - 7, y - 8, 6.5f);
          scCog(x + 6 + ox, y - 7, 5.6f, 0.2f);
          if (n >= 5) scCog(x - 1, y - 17, 3.6f, 0.5f);
          if (n >= 6)                                        // a spring, sprung
            for (int i = 0; i < 4; i++) {
              int sx = (int)x + 4 + i * 2, sy = (int)y - 15;
              fxLine(sx + 1, sy + 1, sx + 2, sy - 4, FX_PAL[FXP_INK]);
              fxLine(sx, sy, sx + 1, sy - 5, FX_PAL[FXP_GLOW]);
            }
          static const int8_t NUT[6][2] = { { -13, -1 }, { 12, -1 }, { -1, -1 }, { 17, -1 }, { -18, -1 }, { 7, -14 } };
          for (int i = 0; i < n - 6; i++) {                  // and the nuts and bolts that came with it all
            int bx = (int)x + NUT[i][0], by = (int)y + NUT[i][1];
            fxRect(bx - 2, by - 3, bx + 2, by + 1, FX_PAL[FXP_INK]);
            fxRect(bx - 1, by - 2, bx + 1, by, FX_PAL[i & 1 ? FXP_HDR : FXP_OK]);
            scPx(bx - 1, by - 2, FX_PAL[FXP_GLOW]);
          }
          if ((now / 110) % 41 == 0) {                       // something in it catches the light
            int gx = (int)(x + 6 + ox - 2.5f), gy = (int)(y - 9.5f);
            scPx(gx, gy, FX_PAL[FXP_WHITE]);
            scPx(gx - 1, gy, FX_PAL[FXP_GLOW]); scPx(gx + 1, gy, FX_PAL[FXP_GLOW]);
            scPx(gx, gy - 1, FX_PAL[FXP_GLOW]); scPx(gx, gy + 1, FX_PAL[FXP_GLOW]);
          }
        } break;
      }
      if (c == 0) {                                          // nothing: the chalk outline of where it would go
        for (int i = 0; i < 12; i++) scPx((int)(x - 8 + i * 1.4f), (int)y - (i % 3 == 0 ? 1 : 0), FX_PAL[FXP_LINE]);
      }
      int more = c - (r == 0 ? 6 : r == 1 ? 6 : r == 2 ? 3 : r == 3 ? 4 : 99);
      if (more > 0) { char m[6]; snprintf(m, sizeof(m), "+%d", more); scType(m, (int)x + 7, (int)y - 20, 1, FXP_GLOW); }
      // the tag
      char n[4];
      snprintf(n, sizeof(n), "%u", (unsigned)c);
      bool bad = c == 0, low = c <= 2;
      uint8_t col = bad ? (((now / 300) & 1) ? FXP_WHITE : FXP_CRIT) : low ? FXP_CRIT : FXP_GLOW;
      int tw = scTypeW(n, 2);
      scType(n, (int)x - tw / 2, 137, 2, col);
      scType(SC_RES_NAME[r], (int)x - scTypeW(SC_RES_NAME[r], 1) / 2, 152, 1, bad ? FXP_CRIT : FXP_HDR);
    }
    const char* nar =
        v.res[0] == 0 ? "NO WATER. THIS IS|NOT A DRILL." :
        v.res[1] == 0 ? "NO FOOD. THE TARP|IS STARTING TO LOOK TASTY." :
        v.packUsed >= v.packCap ? "PACK FULL. SOMETHING|HAS TO GO. NOT YOU." :
        v.res[4] >= 8 ? "RICH IN SCRAP. SCRAP|IS NOT WATER." :
        v.res[2] >= 3 ? "GASOLINE. PEOPLE HAVE|KILLED FOR LESS. RECENTLY." :
        v.packUsed == 0 ? "TRAVELLING LIGHT.|TOO LIGHT." :
                           "EVERYTHING THEY OWN,|LAID OUT TO JUDGE.";
    scNarration(nar, 5, 25, since);
    {
      int nl = 1, ll = 0, lm = 0;                              // where scNarration put its box
      for (const char* c = nar; *c; c++) { if (*c == '|') { nl++; ll = 0; } else lm = fxMaxI(lm, ++ll); }
      scStashWanderers(S, pid, sky, cols, rows, 5 + lm * 6 + 12, 25 + nl * 10 + 8);
    }
  }

  // -- the masthead, with a dot per survivor --
  fxRect(0, 0, FX_W, 20, FX_PAL[FXP_INK]);
  fxRect(0, 19, FX_W, 20, FX_PAL[FXP_LINE]);
  scType("THE STASH", 4, 2, 2, FXP_HDR);
  for (int i = 0; i < nOn; i++) {
    int x = 234 - (nOn - 1 - i) * 9;
    bool me = seated[i] == pid;
    fxDisc((float)x, 10, me ? 3.2f : 2.0f, FX_PAL[me ? FXP_CRIT : FXP_LINE]);
  }
  for (int x = 0; x < FX_W; x++) for (int y = 161 - (int)(fxHash2((uint32_t)x, 9) % 3); y < 164; y++) scPx(x, y, FX_PAL[FXP_INK]);
  fxRect(0, 163, FX_W, FX_H, FX_PAL[FXP_INK]);
  if (pid < 0) {
    static const char L1[] = "NOBODY ONLINE";
    fxF2(L1, (FX_W - fxF2Width(L1)) / 2, 220, FX_PAL[FXP_HDR]);
    scType("NOTHING TO COUNT. RESTFUL.", 42, 244, 1, FXP_RUST);
    return;
  }
  const SceneSurv& v = S.s[pid];

  // -- who, and the pack as a row of slots --
  fxRect(2, 166, 14, 178, FX_PAL[FXP_HDR]);
  char num[2] = { (char)('1' + pid), 0 };
  fxGlcd(num, 6, 169, 1, FX_PAL[FXP_INK]);
  int cap = fxMinI(18, v.packCap), sx = 238 - cap * 6;
  for (int i = 0; i < cap; i++) {
    int x = sx + i * 6;
    bool on = i < v.packUsed;
    fxRect(x, 168, x + 5, 176, FX_PAL[on ? (v.packUsed >= v.packCap ? FXP_CRIT : FXP_HDR) : FXP_TRACK]);
  }
  char nm[16];
  const char* an = SC_ARCH_NAME[v.arch < 6 ? v.arch : 0];
  scFitF2(nm, sizeof(nm), v.name[0] ? v.name : "Walker", sx - 22 - 4);
  fxF2(nm, 18, 164, FX_PAL[FXP_CRIT]);
  int nw = fxF2Width(nm);
  if (18 + nw + 6 + scTypeW(an, 1) < sx - 4) fxGlcd(an, 18 + nw + 6, 170, 1, FX_PAL[FXP_BRICK]);

  // -- the manifest: a tractor-feed printout clipped to a board. Bars of two
  //    shades so the eye can follow a row across, holes down both edges, and
  //    a count only where there is more than one of a thing --
  const int PX0 = 7, PY0 = 189, PX1 = 233, PY1 = 297, ROW0 = 205;
  fxRect(2, 183, 238, 300, FX_PAL[FXP_INK]);                  // the board, the grain running down it
  for (int y = 184; y < 299; y++)
    for (int x = 3; x < 237; x++) scDither(x, y, 2.7f + 1.2f * (fxNoise2(x * 0.45f, y * 0.035f, 41) - 0.5f));
  fxLine(3, 184, 236, 184, FX_PAL[FXP_LINE]);
  fxRect(PX0 + 2, PY0 + 2, PX1 + 2, PY1 + 1, FX_PAL[FXP_INK]);  // the sheet's shadow
  for (int y = PY0; y < PY1; y++) {
    bool bar = y >= ROW0 && ((y - ROW0) / 10) & 1;
    fxSpan(y, PX0, PX1, FX_PAL[FXP_HDR]);
    if (bar) fxSpan(y, PX0 + 7, PX1 - 7, FX_PAL[FXP_OK]);
  }
  for (int x = PX0; x < PX1; x += 4) fxRect(x + 2, PY1 - 1, x + 4, PY1, FX_PAL[FXP_INK]);   // torn off at the fold
  for (int y = PY0 + 4; y + 3 < PY1; y += 10)                 // the sprocket holes, and the perforations
    for (int s = 0; s < 2; s++) {
      int hx = s ? PX1 - 5 : PX0 + 2;
      fxRect(hx, y, hx + 3, y + 3, FX_PAL[FXP_TRACK]);
      fxLine(hx, y, hx + 2, y, FX_PAL[FXP_SOOT]);
    }
  for (int y = PY0 + 1; y < PY1 - 1; y += 2) { scPx(PX0 + 6, y, FX_PAL[FXP_BRICK]); scPx(PX1 - 7, y, FX_PAL[FXP_BRICK]); }
  for (int dy = -5; dy <= 5; dy++)                             // somebody's thumb, where you hold it
    for (int dx = -5; dx <= 5; dx++) {
      float d = sqrtf(dx * dx * 1.3f + dy * dy * 0.8f);
      if (d > 5.6f || fabsf(d * 0.62f - roundf(d * 0.62f)) > 0.2f || fxHash2((uint32_t)(dx * 31 + dy), 0x7Bu) % 5 == 0) continue;
      scPx(81 + dx, 195 + dy, FX_PAL[FXP_OK]);
    }
  // the clip: a pressed-brass clamp, the hanging slot in its crown
  {
    const float ox[6] = { 93, 147, 147, 137, 103, 93 }, oy[6] = { 200, 200, 192, 181, 181, 192 };
    const float ix[6] = { 94, 146, 146, 136, 104, 94 }, iy[6] = { 199, 199, 192, 182, 182, 192 };
    fxPoly(ox, oy, 6, FX_PAL[FXP_INK]);
    fxPoly(ix, iy, 6, FX_PAL[FXP_FLAME]);
    fxLine(105, 182, 135, 182, FX_PAL[FXP_GLOW]);
    fxLine(95, 192, 103, 183, FX_PAL[FXP_HOT]);
    fxLine(137, 183, 145, 191, FX_PAL[FXP_BRICK]);
    fxLine(95, 193, 145, 193, FX_PAL[FXP_HOT]);              // the fold where it bites the paper
    fxLine(95, 198, 145, 198, FX_PAL[FXP_BRICK]);
    fxRect(112, 185, 128, 189, FX_PAL[FXP_INK]);
    fxLine(113, 189, 127, 189, FX_PAL[FXP_RUST]);
    for (int s = 0; s < 2; s++) {
      int rx = s ? 140 : 100;
      fxDisc((float)rx, 196, 1.5f, FX_PAL[FXP_INK]);
      scPx(rx - 1, 195, FX_PAL[FXP_GLOW]);
    }
  }
  // the head of the form
  fxGlcd("MANIFEST", 15, 192, 1, FX_PAL[FXP_INK]);
  fxGlcd("MANIFEST", 16, 192, 1, FX_PAL[FXP_INK]);              // struck twice, the way a heading was
  snprintf(nm, sizeof(nm), "%u/%u", (unsigned)v.packUsed, (unsigned)v.packCap);
  const int cw = scTypeW(nm, 1);
  fxGlcd(nm, 226 - cw, 192, 1, FX_PAL[v.packUsed >= v.packCap ? FXP_BLOOD : FXP_INK]);
  fxGlcd("PACK", 226 - cw - 30, 192, 1, FX_PAL[FXP_RUST]);
  fxLine(15, 201, 225, 201, FX_PAL[FXP_INK]);
  for (int x = 15; x <= 225; x += 2) scPx(x, 203, FX_PAL[FXP_BRICK]);
  int shown = (int)(since / 70);                               // printed up a line at a time when the page turns
  if (S.itemN == 0) {
    static const char E1[] = "NOTHING. CONTENTS MAY", E2[] = "HAVE SETTLED ELSEWHERE.";
    fxGlcd(E1, 120 - scTypeW(E1, 1) / 2, 226, 1, FX_PAL[FXP_INK]);
    fxGlcd(E2, 120 - scTypeW(E2, 1) / 2, 236, 1, FX_PAL[FXP_INK]);
  } else {
    for (int y = ROW0; y < ROW0 + 90; y += 2) scPx(120, y, FX_PAL[FXP_BRICK]);
  }
  for (int i = 0; i < S.itemN && i < 18 && i < shown; i++) {
    const int x0 = i < 9 ? 15 : 124, x1 = i < 9 ? 116 : 225, y = ROW0 + 1 + (i % 9) * 10;
    char q[6] = "";
    if (S.itemQty[i] > 1) snprintf(q, sizeof(q), "x%u", (unsigned)S.itemQty[i]);
    const int qw = q[0] ? scTypeW(q, 1) : 0;
    char it[22];
    scAbbrev(it, sizeof(it), S.item[i], (x1 - x0 + 1 - (qw ? qw + 4 : 0)) / 6);
    const int tw = fxGlcd(it, x0, y, 1, FX_PAL[FXP_INK]);
    if (qw) {
      for (int x = x0 + tw + 1; x < x1 - qw - 2; x += 3) scPx(x, y + 6, FX_PAL[FXP_RUST]);   // the leader
      fxGlcd(q, x1 - qw + 1, y, 1, FX_PAL[FXP_BLOOD]);
    }
  }
  // -- the remark --
  static const char* const REM[] = {
    "CHECKED TWICE. STILL NOT ENOUGH.", "EVERY ITEM HAS A STORY. MOSTLY SAD.",
    "WEIGHT LIMIT: WHATEVER THE KNEES SAY.", "NOTHING HERE IS RETURNABLE.",
    "WHAT YOU CAN'T CARRY, YOU BURY.", "LOOTED FAIR AND SQUARE. MOSTLY.",
  };
  fxGlcd(REM[(now / 8000) % SC_NN(REM)], 3, 306, 1, FX_PAL[FXP_LINE]);
}

// ── 10. Screen 4: THE ODDS ───────────────────────────────────────────────────
// The encounter rolls as a fairground strength-tester someone dragged out of
// the ruins: the survivor swings, the puck flies up the pole to what they
// rolled, and it rings the bell hung at what they needed -- or doesn't, and
// falls back down. Chalk tallies keep the party's score on the boards; under
// them, each survivor's record, and a doorway for anyone still inside one.
//
//   y   0-19   masthead    THE ODDS                          SITES 34/80
//   y  20-177  the booth   caption, verdict stamp, the pole, the bulbs
//   y 180-199  the tally   WON |||| ||   LOST x x x
//   y 202-301  who         5 rows: name, then INSIDE <biome> or their record
//   y 304-319  the remark
static const char* const SC_SKILL[5] = { "NAVIGATE", "FORAGE", "SCAVENGE", "SHELTER", "ENDURE" };
static const char* const SC_END[7]   = { "BITTEN", "LEGGED IT", "SUNRISE", "CARRIED OUT", "SIGNAL LOST", "UNMADE", "EMPTY-HANDED" };

static struct { uint16_t rolls; uint32_t t0; bool have; } scOddsSt = { 0, 0, false };

static void scOddsDraw(const SceneSnap& S, uint32_t now) {
  if (!scOddsSt.have || S.rolls != scOddsSt.rolls) {
    scOddsSt.t0 = scOddsSt.have ? now : now - 100000;         // the first look finds it already rung
    scOddsSt.rolls = S.rolls;
    scOddsSt.have = true;
  }
  const uint32_t t = now - scOddsSt.t0;

  ScStage P = { 0, 20, FX_W, 178, 170, 120, 1.4f, SC_TONE_ILL, 9, 0x0DD5u, now, true, false };
  scPanelSet(P, SC_GLY_NONE);

  // -- the pole --
  const int DN = S.hasRoll ? S.rollDN : 8, TOT = S.hasRoll ? S.rollTotal : 0;
  const int M  = fxMaxI(12, fxMaxI(DN, TOT) + 2);
  const float px = 190, y0 = 166, y1 = 58;
  auto yv = [&](float v) { return y0 - (y0 - y1) * v / M; };
  fxRect((int)px - 4, (int)y1 - 4, (int)px + 4, (int)y0 + 2, FX_PAL[FXP_INK]);
  fxRect((int)px - 3, (int)y1 - 3, (int)px + 3, (int)y0 + 1, FX_PAL[FXP_SOOT]);
  fxLine((int)px - 3, (int)y1 - 3, (int)px - 3, (int)y0, FX_PAL[FXP_RUST]);
  for (int v = 0; v <= M; v++) {
    int y = (int)yv((float)v);
    fxLine((int)px + 4, y, (int)px + (v % 5 == 0 ? 10 : 7), y, FX_PAL[FXP_LINE]);
    if (v % 5 == 0 && v) { char b[4]; snprintf(b, sizeof(b), "%d", v); fxGlcd(b, (int)px + 12, y - 3, 1, FX_PAL[FXP_LINE]); }
  }
  fxRect((int)px - 14, (int)y0 + 1, (int)px + 14, (int)y0 + 5, FX_PAL[FXP_INK]);   // the plate
  fxRect((int)px - 12, (int)y0 + 1, (int)px + 12, (int)y0 + 2, FX_PAL[FXP_HDR]);
  // the bell, hung at what they needed
  const float by = yv((float)DN);
  const bool won = S.hasRoll && S.rollWon;
  float ring = (won && t > 1150 && t < 2400) ? sinf(t * 0.08f) * 2.5f * (1 - (t - 1150) / 1250.0f) : 0;
  fxLine((int)px - 4, (int)by, (int)px - 12, (int)by, FX_PAL[FXP_INK]);
  float bx = px - 16 + ring;
  fxTri(bx - 6, by + 5, bx + 6, by + 5, bx, by - 5, FX_PAL[FXP_INK]);
  fxTri(bx - 5, by + 4, bx + 5, by + 4, bx, by - 4, FX_PAL[won && t > 1150 && t < 1600 ? FXP_WHITE : FXP_CRIT]);
  fxDisc(bx, by + 5, 1.4f, FX_PAL[FXP_INK]);
  char need[16];
  snprintf(need, sizeof(need), "NEED %d", DN);
  scType(need, (int)bx - 8 - scTypeW(need, 1), (int)by - 3, 1, FXP_CRIT);

  // -- the swing and the flight --
  float puck = 0;                                             // in pole units
  if (S.hasRoll) {
    float peak = (float)TOT;
    if (t < 500) puck = 0;
    else if (t < 1150) puck = peak * fxCubicOut((t - 500) / 650.0f);
    else if (won) puck = peak;
    else if (t < 1900) puck = peak * (1 - fxCubicIn((t - 1150) / 750.0f));
    else puck = 0;
    // the peg it reached stays, so the table can read it after
    if (t >= 1150) {
      float py = yv(peak);
      fxRect((int)px + 3, (int)py - 1, (int)px + 8, (int)py + 1, FX_PAL[won ? FXP_GLOW : FXP_BLOOD]);
      char b[12];
      snprintf(b, sizeof(b), "%d", TOT);
      int bw = scTypeW(b, 1);
      fxRect((int)px + 9, (int)py - 5, (int)px + 14 + bw, (int)py + 5, FX_PAL[FXP_INK]);   // over the scale
      fxGlcd(b, (int)px + 12, (int)py - 3, 1, FX_PAL[won ? FXP_GLOW : FXP_CRIT]);
    }
  }
  {
    float py = yv(puck);
    fxRect((int)px - 5, (int)py - 4, (int)px + 5, (int)py + 1, FX_PAL[FXP_INK]);
    fxRect((int)px - 4, (int)py - 3, (int)px + 4, (int)py, FX_PAL[FXP_GLOW]);
  }
  {
    int who = S.hasRoll && S.rollWho >= 0 && S.rollWho < SC_SEATS ? S.rollWho : -1;
    uint8_t arch = who >= 0 ? S.s[who].arch : 5;
    // The swing, then how they take it: the mallet held up high, or dragged.
    bool windup = S.hasRoll && t < 450, after = S.hasRoll && t >= 1150;
    uint8_t pose = windup ? SCP_REACH : after ? (won ? SCP_REACH : SCP_SLUMP) : SCP_HOLD;
    ScFig F = { 150, 170, 1, arch, pose, (uint8_t)(who >= 0 ? who : 255), 0, P.rim, -0.7f, -0.7f, 1.4f };
    if (pose == SCP_HOLD)  { F.handU = 6.6f; F.handV = 13.2f; }   // both hands down on the handle
    if (pose == SCP_SLUMP) { F.handU = 4.4f; F.handV = 9.2f; }
    for (int p = 0; p < 3; p++) scFigure(F, p);
    float hx = scHandX, hy = scHandY, mx, my;
    if (windup)      { mx = 147.0f; my = 110.0f; }             // over the head
    else if (!after) { mx = 176.0f; my = 166.0f; }             // down on the plate
    else if (won)    { mx = hx - 2.0f; my = hy - 16.0f; }      // held up, like a trophy
    else             { mx = hx + 15.0f; my = 167.0f; }         // its head in the dirt
    fxStroke(hx, hy, mx, my, 2.0f, 1.8f, FX_PAL[FXP_INK]);
    fxRect((int)mx - 5, (int)my - 4, (int)mx + 5, (int)my + 3, FX_PAL[FXP_INK]);
    fxLine((int)mx - 5, (int)my - 4, (int)mx + 4, (int)my - 4, FX_PAL[FXP_HDR]);
    if (S.hasRoll && t >= 450 && t < 700) scBang("WHACK", 150, 150, 8, 3, 1);
    // and what they have to say about it, each in their own way
    if (after && t < 3600) {
      static const char* const WIN[6]  = { "YEE-HAW!", "PAYDAY!", "AMEN!", "HOO-EE!", "TOLD YA.", "...GOOD." };
      static const char* const LOSE[6] = { "DAGNABBIT", "RIGGED.", "LORD.", "MY BACK.", "IT'S BENT.", "..." };
      scBalloon((won ? WIN : LOSE)[arch < 6 ? arch : 0], 110, 120, 144, 128);
    }
  }
  // -- the marquee, bulbs chasing --
  const int mx0 = 126, mx1 = 236, my0 = 24, my1 = 44;
  fxRect(mx0 - 1, my0 - 1, mx1 + 1, my1 + 1, FX_PAL[FXP_INK]);
  fxRect(mx0, my0, mx1, my1, FX_PAL[FXP_BLOOD]);
  fxGlcd("TEST YOUR", mx0 + (mx1 - mx0 - scTypeW("TEST YOUR", 1)) / 2, my0 + 3, 1, FX_PAL[FXP_GLOW]);
  fxGlcd("LUCK", mx0 + (mx1 - mx0 - scTypeW("LUCK", 1)) / 2, my0 + 11, 1, FX_PAL[FXP_GLOW]);
  {
    int k = 0, chase = (int)(now / 160);
    for (int x = mx0 + 2; x < mx1; x += 6, k++) {
      bool lit = ((k + chase) % 3) == 0 && (fxHash(k * 13) % 7);   // one of them is dead. of course.
      scPx(x, my0 - 3, FX_PAL[lit ? FXP_WHITE : FXP_EMBER]);
      scPx(x, my1 + 2, FX_PAL[((k + chase + 1) % 3) == 0 ? FXP_WHITE : FXP_EMBER]);
    }
  }
  fxStroke(mx0 + 30, my1, mx0 + 30, 58, 1.2f, 1.2f, FX_PAL[FXP_INK]);    // it hangs off the pole on wire
  fxLine(mx1 - 10, my1, (int)px, (int)y1 - 4, FX_PAL[FXP_INK]);

  // -- what happened, in words --
  char cap[64];
  if (S.hasRoll) {
    int who = S.rollWho >= 0 && S.rollWho < SC_SEATS ? S.rollWho : 0;
    // Steps up; then, once the puck has come down, how it went.
    const char* nm = S.s[who].name[0] ? S.s[who].name : "SOMEONE";
    if (t < 1150) snprintf(cap, sizeof(cap), "%s|STEPS UP TO %s.", nm, SC_SKILL[S.rollSkill < 5 ? S.rollSkill : 0]);
    else if (S.rollWon) snprintf(cap, sizeof(cap), "%s|RINGS THE BELL.", nm);
    else snprintf(cap, sizeof(cap), "%s|COMES UP SHORT.", nm);
    for (char* p = cap; *p; p++) *p = (char)toupper((unsigned char)*p);
  } else {
    snprintf(cap, sizeof(cap), "NOBODY HAS TRIED|THEIR LUCK YET.");
  }
  scNarration(cap, 5, 25, t);
  if (S.hasRoll && t >= 1150) {
    // the verdict, stamped
    const char* v = won ? "WON" : "LOST";
    int w = scTypeW(v, 3) + 10, x = 14, y = 70;
    float slam = fxClampF((t - 1150) / 180.0f, 0, 1);
    if (slam > 0) {
      fxRect(x - 2, y - 2, x + w + 2, y + 28, FX_PAL[FXP_INK]);
      fxRect(x, y, x + w, y + 26, FX_PAL[won ? FXP_GLOW : FXP_BLOOD]);
      fxRect(x + 2, y + 2, x + w - 2, y + 24, FX_PAL[FXP_INK]);
      fxGlcd(v, x + 5, y + 2, 3, FX_PAL[won ? FXP_GLOW : FXP_CRIT]);
      char b[32];
      snprintf(b, sizeof(b), "NEEDED %d", DN);
      scType(b, x, y + 34, 1, FXP_HDR);
      snprintf(b, sizeof(b), "ROLLED %d", TOT);
      scType(b, x, y + 44, 1, won ? FXP_GLOW : FXP_CRIT);
    }
    if (won && t < 2200) scBang("DING!", bx - 26, by - 14, 9, 17, 1);
    if (!won && t >= 1850 && t < 2500) scBang("THUNK", px + 22, y0 - 22, 8, 19, 1);   // off the figure, by the pole's foot
  }

  // -- the masthead --
  fxRect(0, 0, FX_W, 20, FX_PAL[FXP_INK]);
  fxRect(0, 19, FX_W, 20, FX_PAL[FXP_LINE]);
  scType("THE ODDS", 4, 2, 2, FXP_HDR);
  char b[40];
  snprintf(b, sizeof(b), "SITES %u/%u", (unsigned)S.poiLeft, (unsigned)S.poiTotal);
  scType(b, 237 - scTypeW(b, 1), 7, 1, S.poiLeft ? FXP_CRIT : FXP_LINE);
  for (int x = 0; x < FX_W; x++) for (int y = 177 - (int)(fxHash2((uint32_t)x, 11) % 3); y < 180; y++) scPx(x, y, FX_PAL[FXP_INK]);
  fxRect(0, 179, FX_W, FX_H, FX_PAL[FXP_INK]);

  // -- the tally, chalked --
  {
    int x = 4;
    fxGlcd("WON", x, 186, 1, FX_PAL[FXP_HDR]);
    x += 22;
    int w = fxMinI(S.wins, 25);
    for (int i = 0; i < w; i++) {
      int g = i / 5, k = i % 5, gx = x + g * 14;
      uint32_t h = fxHash(i + 5);
      if (k < 4) fxLine(gx + k * 3, 184 + (int)(h & 1), gx + k * 3 + (int)((h >> 1) & 1), 193, FX_PAL[FXP_GLOW]);
      else       fxLine(gx - 2, 191, gx + 11, 185, FX_PAL[FXP_GLOW]);
    }
    int lx = x + ((w + 4) / 5) * 14 + 6;
    int l = fxMinI(S.rolls - S.wins, 12);
    fxGlcd("LOST", lx, 186, 1, FX_PAL[FXP_BRICK]);
    for (int i = 0; i < l; i++) {
      int cx = lx + 28 + i * 7;
      if (cx > 206) break;
      fxLine(cx - 2, 185, cx + 2, 192, FX_PAL[FXP_BLOOD]);
      fxLine(cx + 2, 185, cx - 2, 192, FX_PAL[FXP_BLOOD]);
    }
    snprintf(b, sizeof(b), "%u/%u", (unsigned)S.wins, (unsigned)S.rolls);
    fxGlcd(b, 237 - scTypeW(b, 1), 186, 1, FX_PAL[S.rolls ? FXP_CRIT : FXP_LINE]);
    fxRect(0, 198, FX_W, 199, FX_PAL[FXP_TRACK]);
  }

  // -- who --
  int row = 0;
  for (int i = 0; i < SC_SEATS && row < 5; i++) {
    const SceneSurv& v = S.s[i];
    if (!v.on) continue;
    int y = 202 + row * 20;
    row++;
    fxRect(2, y + 2, 14, y + 14, FX_PAL[v.inEnc ? FXP_CRIT : FXP_HDR]);
    char nb[2] = { (char)('1' + i), 0 };
    fxGlcd(nb, 6, y + 5, 1, FX_PAL[FXP_INK]);
    char nm[16];
    scFitF2(nm, sizeof(nm), v.name[0] ? v.name : "Walker", 80);
    fxF2(nm, 18, y, FX_PAL[FXP_CRIT]);
    if (v.inEnc) {
      // a doorway, and them half into it
      int dx = 104;
      fxRect(dx, y + 1, dx + 11, y + 16, FX_PAL[FXP_LINE]);
      fxRect(dx + 2, y + 3, dx + 9, y + 16, FX_PAL[FXP_INK]);
      if ((now / 900 + i) % 5) { scPx(dx + 4, y + 7, FX_PAL[FXP_CRIT]); scPx(dx + 7, y + 7, FX_PAL[FXP_CRIT]); }
      char bi[16];
      snprintf(bi, sizeof(bi), "%s", v.biome);
      for (char* p = bi; *p; p++) *p = (char)toupper((unsigned char)*p);
      snprintf(b, sizeof(b), "IN %s", bi);
      fxGlcd(b, 120, y + 5, 1, FX_PAL[FXP_GLOW]);
      snprintf(b, sizeof(b), "LOOT %u", (unsigned)v.loot);
      fxGlcd(b, 237 - scTypeW(b, 1), y + 5, 1, FX_PAL[v.loot ? FXP_HOT : FXP_LINE]);
    } else if (v.encRolls == 0) {
      fxGlcd(v.encCount ? "no rolls this boot" : "hasn't dared yet", 104, y + 5, 1, FX_PAL[FXP_LINE]);
    } else {
      snprintf(b, sizeof(b), "WON %u/%u", (unsigned)v.encWins, (unsigned)v.encRolls);
      fxGlcd(b, 104, y + 5, 1, FX_PAL[FXP_HDR]);
      if (v.hasEnd) {
        const char* e = SC_END[v.lastEnd < 7 ? v.lastEnd : 0];
        bool bad = v.lastEnd == 0 || v.lastEnd == 3;
        fxGlcd(e, 237 - scTypeW(e, 1), y + 5, 1, FX_PAL[bad ? FXP_CRIT : FXP_BRICK]);
      }
    }
  }
  if (row == 0) scType("NOBODY AT THE BOOTH.", 60, 240, 1, FXP_RUST);
  static const char* const REM[] = {
    "THE HOUSE ALWAYS WINS. IT IS ALSO DEAD.", "ODDS ARE ODDS. EVENS ARE RARER.",
    "NO REFUNDS. NO RULES. NO HOUSE.", "EVERY SITE IS ONLY OPENED ONCE.",
    "THE DEALER'S DEAD. DEAL YOURSELF IN.", "STEP RIGHT UP. STEP RIGHT BACK.",
  };
  fxGlcd(REM[(now / 9000) % SC_NN(REM)], 3, 308, 1, FX_PAL[FXP_LINE]);
}

// ── 11. Screen 5: THE WASTE ──────────────────────────────────────────────────
// The whole ring, on the terminal's old radar. Terrain in halftone -- glass
// bright, craters black, forest a dotted dark -- a sweep going round the
// party with a phosphor trail behind it, and the blips: the party numbered,
// the caravan's wagon, the hatches, and the Creeping Doom as a mass that
// breathes, with the dotted ring it can smell you inside. Under it, the
// Doom's mood, in words.
//
//   y   0-19   masthead    THE WASTE                          DAY 12
//   y  21-252  the scope   the map, 3 x 4 px a hex
//   y 256-318  the key     blips named, the Doom's mood meter, a remark
// Rungs per terrain, kept low so the blips are the brightest things on the
// scope; the landmarks (settlements, bunkers, the glass) are the exceptions.
static const float SC_TERR_LV[16] = {
  3.6f, 2.4f, 1.3f, 2.1f, 4.6f, 1.7f, 6.6f, 3.0f, 3.6f, 9.4f, 0.3f, 1.2f, 8.5f, 4.4f, 1.5f, 0.8f,
};
static uint8_t* scAng = nullptr;                              // angle of every scope pixel from the sweep's centre (PSRAM)
static int16_t  scAngCx = -1000, scAngCy = -1000;

static void scWasteDraw(const SceneSnap& S, uint32_t now) {
  uint16_t* const out = fxDst;
  const int cols = S.cols > 0 ? S.cols : 1, rows = S.rows > 0 ? S.rows : 1;
  const int XS = fxMaxI(1, (FX_W - 8) / cols), YS = fxMaxI(1, fxMinI(4, 228 / rows));
  const int MX = (FX_W - cols * XS) / 2, MY = 22, OY = YS / 2;
  const int SY1 = MY + rows * YS + OY;
  auto cell = [&](int q, int r, int* x, int* y) { *x = MX + q * XS; *y = MY + r * YS + (q & 1) * OY; };

  // -- the still layer: the terrain --
  uint32_t key = 0x3A9E5u ^ (uint32_t)cols * 131u ^ S.day;
  if (S.terr) for (int i = 0; i < cols * rows; i++) key = key * 31u + S.terr[i];
  if (S.eco)  for (int i = 0; i < cols * rows; i++) key = key * 31u + S.eco[i];
  for (int i = 0; i < S.hatchN; i++) key = fxHash2(key, (uint32_t)(S.hatchQ[i] * 1000 + S.hatchR[i]));
  if (!scStillTried) { scStillTried = true; scStill = (uint16_t*)fxAlloc((size_t)FX_W * FX_H * 2); }
  if (!scStill || scStillKey != key) {
    if (scStill) fxDst = scStill;
    for (int i = 0; i < FX_W * FX_H; i++) fxDst[i] = 0;
    fxRect(MX - 3, MY - 2, MX + cols * XS + 3, SY1 + 2, FX_PAL[FXP_LINE]);   // the bezel
    fxRect(MX - 2, MY - 1, MX + cols * XS + 2, SY1 + 1, FX_PAL[FXP_INK]);
    if (S.terr)
      for (int r = 0; r < rows; r++)
        for (int q = 0; q < cols; q++) {
          int t = S.terr[r * cols + q] & 15, x0, y0;
          cell(q, r, &x0, &y0);
          uint8_t ed = S.eco ? S.eco[r * cols + q] : 0;
          float lv = SC_TERR_LV[t] - ((ed & 15) >= 4 ? 1.2f : 0.0f);
          for (int y = y0; y < y0 + YS; y++)
            for (int x = x0; x < x0 + XS; x++) {
              float l = lv;
              if (t == 2 && ((x + y) & 1)) l -= 1.0f;          // forest: dark, and dotted
              if (t == 11 && ((x + y * 2) % 5 == 0)) l += 3.0f;  // river: a glint
              if (t == 4 && (x % 3 == 0 || y % 4 == 0)) l -= 1.4f;   // the old streets
              scDither(x, y, l);
            }
          if (ed & 0x80) scPx(x0 + XS / 2, y0 + YS / 2, FX_PAL[FXP_BLOOD]);   // the mould, fruiting
        }
    for (int i = 0; i < S.hatchN; i++) {
      int x, y;
      cell(S.hatchQ[i], S.hatchR[i], &x, &y);
      fxRect(x - 2, y - 2, x + XS + 2, y + YS + 2, FX_PAL[FXP_INK]);
      fxRect(x - 1, y - 1, x + XS + 1, y + YS + 1, FX_PAL[FXP_GLOW]);
      fxRect(x, y, x + XS, y + YS, FX_PAL[FXP_INK]);
    }
    // the masthead and the key's fixed words
    fxRect(0, 0, FX_W, 20, FX_PAL[FXP_INK]);
    fxRect(0, 19, FX_W, 20, FX_PAL[FXP_LINE]);
    scType("THE WASTE", 4, 2, 2, FXP_HDR);
    char b[16];
    snprintf(b, sizeof(b), "DAY %u", (unsigned)S.day);
    scType(b, 237 - scTypeW(b, 1), 7, 1, FXP_CRIT);
    scStillKey = scStill ? key : 0;
    fxDst = out;
  }
  if (scStill) memcpy(out, scStill, (size_t)FX_W * FX_H * 2);

  // -- the sweep, round the middle of the party --
  float scx = 0, scy = 0;
  int nOn = 0;
  for (int i = 0; i < SC_SEATS; i++)
    if (S.s[i].on) { int x, y; cell(S.s[i].q, S.s[i].r, &x, &y); scx += x; scy += y; nOn++; }
  if (nOn) { scx /= nOn; scy /= nOn; } else { scx = FX_W * 0.5f; scy = (MY + SY1) * 0.5f; }
  if (!scAng) scAng = (uint8_t*)fxAlloc((size_t)FX_W * FX_H);
  if (scAng && (fabsf(scx - scAngCx) > 1 || fabsf(scy - scAngCy) > 1)) {
    scAngCx = (int16_t)scx; scAngCy = (int16_t)scy;
    for (int y = MY; y < SY1; y++)
      for (int x = MX; x < MX + cols * XS; x++)
        scAng[y * FX_W + x] = (uint8_t)((atan2f(y - scAngCy + 0.5f, x - scAngCx + 0.5f) + 3.14159265f) * (256.0f / FX_TAU));
  }
  const uint8_t sw = (uint8_t)((now % 4200) * 256 / 4200);
  if (scAng)
    for (int y = MY; y < SY1; y++) {
      uint16_t* row = out + y * FX_W;
      const uint8_t* ar = scAng + y * FX_W;
      for (int x = MX; x < MX + cols * XS; x++) {
        uint8_t d = (uint8_t)(sw - ar[x]);
        if (d < 2) row[x] = fxBoost(row[x] ? row[x] : FX_PAL[FXP_LINE], 520);
        else if (d < 44) row[x] = row[x] ? fxBoost(row[x], 256 + (44 - d) * 5) : FX_PAL[d < 20 ? FXP_TRACK : FXP_BAND];
      }
    }
  auto swept = [&](float x, float y) -> float {              // 1 just after the sweep crosses, fading
    float a = (atan2f(y - scAngCy, x - scAngCx) + 3.14159265f) * (256.0f / FX_TAU);
    uint8_t d = (uint8_t)(sw - (uint8_t)a);
    return d < 64 ? 1.0f - d / 64.0f : 0.0f;
  };

  // -- the Doom: a small skull of its gas, a wisp going up off it, and ember
  // eyes once it is unmaking things. No ring: the web map dropped its scent
  // ring too, and the mood meter below says how much it cares. --
  bool doomOn = S.doomQ >= 0 && S.doomQ < cols && S.doomR >= 0 && S.doomR < rows;
  if (doomOn) {
    int dx, dy;
    cell(S.doomQ, S.doomR, &dx, &dy);
    const float t = now * 0.001f, cx = dx + XS * 0.5f, cy = dy + YS * 0.5f - 1.0f;
    fxGasPlume(cx, cy - 2.0f, 4.0f, 0.35f, -0.94f, 2.4f, 0.8f, t, 77);
    // Inked round, the cranium catching the light, and the sockets lit once
    // it is unmaking things -- guttering now and then.
    const int mx = (int)cx - 3, my = (int)(cy + sinf(t * 1.4f) * 0.7f) - 4;
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 7; c++)
        if (SC_DOOM_MARK[r] & (0x80 >> c)) fxRect(mx + c - 1, my + r - 1, mx + c + 2, my + r + 2, FX_PAL[FXP_INK]);
    scBits(SC_DOOM_MARK, 2, 7, mx, my, FX_PAL[FXP_GAS_L]);
    scBits(SC_DOOM_MARK + 2, 5, 7, mx, my + 2, FX_PAL[FXP_GAS]);
    if (S.doomAware >= 76) {
      const bool hot = (now / 140) % 7 != 0;
      for (int sd = 0; sd < 2; sd++) {
        const int sx = mx + 1 + sd * 3;
        fxRect(sx, my + 2, sx + 2, my + 4, FX_PAL[FXP_FLAME]);
        scPx(sx + 1 - sd, my + 2, FX_PAL[hot ? FXP_GLOW : FXP_HOT]);
      }
    }
  }
  // -- the caravan's wagon --
  if (S.caravanOn && S.caravanQ >= 0 && S.caravanQ < cols && S.caravanR >= 0 && S.caravanR < rows) {
    int x, y;
    cell(S.caravanQ, S.caravanR, &x, &y);
    float g = swept((float)x, (float)y);
    fxRect(x - 3, y - 2, x + 5, y + 3, FX_PAL[FXP_INK]);
    fxRect(x - 2, y - 1, x + 4, y + 1, FX_PAL[g > 0.5f ? FXP_WHITE : FXP_HOT]);
    scPx(x - 1, y + 2, FX_PAL[FXP_HOT]); scPx(x + 3, y + 2, FX_PAL[FXP_HOT]);
  }
  // -- Kane's jeep: a rust-red box with the gun over the hood; a glow beside it at his fire --
  if (S.kaneQ >= 0 && S.kaneQ < cols && S.kaneR >= 0 && S.kaneR < rows) {
    int x, y;
    cell(S.kaneQ, S.kaneR, &x, &y);
    const uint16_t body = FX_PAL[S.kaneMode == 2 ? FXP_INK : FXP_BLOOD];
    fxRect(x - 3, y - 2, x + 4, y + 3, FX_PAL[FXP_INK]);
    fxRect(x - 2, y - 1, x + 3, y + 2, body);
    fxRect(x - 5, y - 3, x - 1, y - 2, FX_PAL[FXP_INK]);   // the barrel
    if (S.kaneMode == 1 && (now / 300) % 2) scPx(x + 5, y + 1, FX_PAL[FXP_GLOW]);
  }
  // -- VACANCY's sign: a box on a pole over the town. Lit, it stutters like a
  //    tube about to go; fallen, it holds a steady red (NO VACANCY); saved,
  //    there is nothing left to draw. --
  if (S.vacStage != 2 && S.vacQ >= 0 && S.vacQ < cols && S.vacR >= 0 && S.vacR < rows) {
    int x, y;
    cell(S.vacQ, S.vacR, &x, &y);
    const bool fell = (S.vacStage == 3);
    const bool lit  = fell || ((now / 90) % 23) != 0;
    fxRect(x + 1, y - 6, x + 2, y + 1, FX_PAL[FXP_INK]);    // the pole
    fxRect(x - 3, y - 10, x + 6, y - 5, FX_PAL[FXP_INK]);   // the box
    fxRect(x - 2, y - 9, x + 5, y - 6, FX_PAL[!lit ? FXP_SOOT : fell ? FXP_BLOOD : FXP_CRIT]);
  }
  // -- the party --
  for (int i = 0; i < SC_SEATS; i++) {
    const SceneSurv& v = S.s[i];
    if (!v.on || v.q < 0 || v.q >= cols || v.r < 0 || v.r >= rows) continue;
    int x, y;
    cell(v.q, v.r, &x, &y);
    float g = swept((float)x, (float)y);
    uint16_t c = FX_PAL[g > 0.6f ? FXP_WHITE : (g > 0.2f ? FXP_GLOW : FXP_HOT)];
    fxRect(x - 3, y - 3, x + XS + 3, y + YS + 3, FX_PAL[FXP_INK]);
    fxRect(x - 2, y - 2, x + XS + 2, y + YS + 2, c);
    if (v.depth) fxRect(x, y, x + XS, y + YS, FX_PAL[FXP_INK]);   // below: a hollow blip
    // the seat number on a tab of its own, so terrain never swallows it
    char nb[3] = { (char)('1' + i), v.depth ? 'v' : 0, 0 };
    int tw = scTypeW(nb, 1), lx = x + XS + 4, ly = y - 9;
    if (lx + tw > FX_W - 4) lx = x - tw - 5;
    if (ly < MY) ly = y + YS + 3;
    fxRect(lx - 1, ly - 1, lx + tw + 1, ly + 8, FX_PAL[FXP_INK]);
    fxGlcd(nb, lx, ly, 1, FX_PAL[g > 0.4f ? FXP_WHITE : FXP_GLOW]);
  }

  // -- the key --
  const int ky = 257;
  {
    int x = 4;
    fxRect(x, ky + 1, x + 5, ky + 6, FX_PAL[FXP_CRIT]); fxGlcd("US", x + 8, ky, 1, FX_PAL[FXP_HDR]); x += 28;
    fxRect(x, ky + 1, x + 5, ky + 6, FX_PAL[FXP_CRIT]); fxRect(x + 1, ky + 2, x + 4, ky + 5, FX_PAL[FXP_INK]);
    fxGlcd("BELOW", x + 8, ky, 1, FX_PAL[FXP_HDR]); x += 46;
    fxRect(x, ky + 2, x + 7, ky + 5, FX_PAL[FXP_HOT]); fxGlcd(S.caravanOn ? "TRADER" : "TRADER?", x + 10, ky, 1, FX_PAL[FXP_HDR]); x += 58;
    fxRect(x, ky + 1, x + 6, ky + 7, FX_PAL[FXP_GLOW]); fxRect(x + 1, ky + 2, x + 5, ky + 6, FX_PAL[FXP_INK]);
    fxGlcd("HATCH", x + 9, ky, 1, FX_PAL[FXP_HDR]); x += 44;
    scBits(SC_SKULL, 6, 5, x + 1, ky + 1, FX_PAL[FXP_GAS]); fxGlcd("IT", x + 9, ky, 1, FX_PAL[FXP_HDR]);
  }
  {
    static const char* const MOOD[5] = { "NAPPING", "SNIFFING", "INTERESTED", "PECKISH", "HUNTING" };
    int a = S.doomAware, m = a >= 100 ? 4 : a >= 76 ? 3 : a >= 51 ? 2 : a >= 25 ? 1 : 0;
    int y = ky + 14;
    fxGlcd("DOOM MOOD", 4, y + 2, 1, FX_PAL[FXP_BRICK]);
    for (int i = 0; i < 5; i++) {
      bool on = i <= m;
      bool pulse = on && i == m && m >= 3 && ((now / 250) & 1);
      scBits(SC_SKULL, 6, 5, 64 + i * 9, y + 2, FX_PAL[pulse ? FXP_GAS_L : on ? FXP_GAS : FXP_TRACK]);
    }
    fxF2(MOOD[m], 112, y - 2, FX_PAL[m >= 3 ? FXP_CRIT : FXP_HDR]);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", a);
    fxGlcd(b, 237 - scTypeW(b, 1), y + 2, 1, FX_PAL[FXP_LINE]);
  }
  static const char* const REM[] = {
    "YOU ARE HERE. SORRY ABOUT THAT.", "MAP NOT TO SCALE. NEITHER ARE WE.",
    "HERE BE EVERYTHING, MOSTLY TEETH.", "THE EDGES JOIN UP. NOBODY LEAVES.",
    "IF IT BLINKS, IT IS PROBABLY US.", "UNCLAIMED TERRITORY. FOR A REASON.",
  };
  const char* rem = (!S.caravanOn && (now / 9000) % 3 == 0) ? "THE TRADER IS ELSEWHERE, AS USUAL." : REM[(now / 9000) % SC_NN(REM)];
  fxRect(0, 297, FX_W, 298, FX_PAL[FXP_TRACK]);
  fxGlcd(rem, 3, 304, 1, FX_PAL[FXP_LINE]);
}

// ── 11b. THE WASTE, broken ───────────────────────────────────────────────────
// The old radar only comes up about a third of the times button B lands on
// it. The rest, the board hands in a seed (S.wasteFault, rolled once a visit
// in ui-screens.hpp) and the scope is broken its own way for that whole
// visit: one big fault -- the picture rolls, collapses to a line, shears off
// its sync, fills with junk out of bad video RAM, loses half the tube, or
// gives up and dumps its memory -- with a few small ones on top: the sweep
// stuck and twitching, a field of lines dropped, the phosphor smearing,
// columns stuck, the lettering rotting, snow. The masthead and the key mostly
// keep working; it is the scope that has gone. The remark line owns up.
//
// All of it is done to the finished frame, so scWasteDraw never knows; the
// stuck sweep is only scWasteDraw handed a clock that has stopped.
enum : uint8_t { SCW_ROLL = 0, SCW_COLLAPSE, SCW_SHEAR, SCW_VRAM, SCW_HALF, SCW_DUMP, SCW_MAJOR_N };
enum : uint8_t { SCW_STALL = 1, SCW_SCAN = 2, SCW_SMEAR = 4, SCW_COLS = 8, SCW_TYPE = 16, SCW_SNOW = 32 };
static const int SCW_Y0 = 20, SCW_Y1 = 255;     // the scope's rows, bezel and all
static const int SCW_H  = SCW_Y1 - SCW_Y0;

static inline uint8_t scWasteMajor(uint32_t f) { return (uint8_t)(fxHash2(f, 101) % SCW_MAJOR_N); }
static uint8_t scWasteMinor(uint32_t f) {
  const uint32_t h = fxHash2(f, 202), g = fxHash(h);
  uint8_t m = 0;
  if ((h & 255) < 100)        m |= SCW_STALL;
  if (((h >> 8) & 255) < 70)  m |= SCW_SCAN;
  if (((h >> 16) & 255) < 80) m |= SCW_SMEAR;
  if (((h >> 24) & 255) < 80) m |= SCW_COLS;
  if ((g & 255) < 150)        m |= SCW_TYPE;
  if (((g >> 8) & 255) < 120) m |= SCW_SNOW;
  return m;
}
// The clock scWasteDraw is handed: the real one, or -- the sweep stuck -- the
// moment it broke, twitching between three nearly-the-same frames.
static inline uint32_t scWasteClock(const SceneSnap& S, uint32_t now) {
  if (!S.wasteFault || !(scWasteMinor(S.wasteFault) & SCW_STALL)) return now;
  return S.wasteSince + (fxHash2(now / 280, S.wasteFault) % 3) * 110;
}

// Junk out of bad character RAM: the glcd hand's own pictographs, its shades
// and box pieces, some punctuation -- what a terminal shows when its memory
// lies to it.
static const uint8_t SCW_JUNK[] = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x18, 0x19, 0x1E, 0x1F,
  '#', '%', '&', '@', '$', '*', '?', '<', '>', '/',
  0xB0, 0xB1, 0xB2, 0xBA, 0xBB, 0xBC, 0xC4, 0xC5, 0xCD, 0xCE, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xFE,
};
// One 6 x 8 cell of it, inked over whatever was there; now and then in
// inverse video.
static void scwJunk(int x, int y, uint32_t h) {
  static const uint8_t COL[4] = { FXP_LINE, FXP_HDR, FXP_CRIT, FXP_DIM };
  const char g[2] = { (char)SCW_JUNK[(h >> 4) % sizeof(SCW_JUNK)], 0 };
  const bool inv = (h & 7) == 0;
  fxRect(x, y, x + 6, y + 8, FX_PAL[inv ? FXP_HDR : FXP_INK]);
  fxGlcd(g, x, y, 1, FX_PAL[inv ? FXP_INK : COL[(h >> 12) & 3]]);
}
static inline uint16_t* scwRow(int y) { return fxDst + y * FX_W; }
static void scwSwapRows(int a, int b) {
  uint16_t tmp[FX_W];
  memcpy(tmp, scwRow(a), sizeof(tmp));
  memcpy(scwRow(a), scwRow(b), sizeof(tmp));
  memcpy(scwRow(b), tmp, sizeof(tmp));
}
static void scwReverseRows(int y0, int y1) { for (y1--; y0 < y1; y0++, y1--) scwSwapRows(y0, y1); }
// Turn the rows [y0, y1) over by k, wrapping: what was at y0 lands at y0 + k.
static void scwRollRows(int y0, int y1, int k) {
  const int n = y1 - y0;
  k %= n;
  if (k < 0) k += n;
  if (!k) return;
  scwReverseRows(y0, y1); scwReverseRows(y0, y0 + k); scwReverseRows(y0 + k, y1);
}
// And a row along by k, wrapping: what was at x = 0 lands at x = k.
static void scwShiftRow(uint16_t* row, int k) {
  k %= FX_W;
  if (k < 0) k += FX_W;
  if (!k) return;
  uint16_t tmp[FX_W];
  memcpy(tmp, row + FX_W - k, (size_t)k * 2);
  memmove(row + k, row, (size_t)(FX_W - k) * 2);
  memcpy(row, tmp, (size_t)k * 2);
}
static void scwDimRow(int y, uint32_t k) {
  uint16_t* row = scwRow(y);
  for (int x = 0; x < FX_W; x++) row[x] = fxDim(row[x], k);
}

// Vertical hold gone: the picture rolls, catches for a beat, slips again,
// and the blanking between one frame and the next goes by as a black bar.
// A soft hum bar drifts up through it the other way.
static SC_NOINLINE void scwRoll(uint32_t t, uint32_t f) {
  const uint32_t cyc = 2600 + (f >> 20) % 1400, slip = cyc * 3 / 4, n = t / cyc, u = t % cyc;
  const float v = 0.05f + 0.05f * fxU(fxHash2(f, 7));            // px a ms while it slips
  int k = (int)((float)(n * slip + (u < slip ? u : slip)) * v);
  if (u >= slip) k += (int)(fxHash2(t / 140, f) % 3) - 1;        // caught, but not firmly
  k %= SCW_H;
  scwRollRows(SCW_Y0, SCW_Y1, k);
  for (int i = 1; i <= 9; i++) {                                   // the blanking, over the seam
    int y = SCW_Y0 + k - i;
    if (y < SCW_Y0) y += SCW_H;
    uint16_t* row = scwRow(y);
    const uint16_t c = i == 4 ? FX_PAL[FXP_SOOT] : FX_PAL[FXP_INK];
    for (int x = 0; x < FX_W; x++) row[x] = c;
  }
  const int hy = SCW_Y1 - 1 - (int)((t / 46) % SCW_H);            // the hum bar, going up
  for (int i = 0; i < 44; i++) {
    int y = hy + i;
    if (y >= SCW_Y1) y -= SCW_H;
    scwDimRow(y, 256 - (uint32_t)(120.0f * sinf(i * (3.14159265f / 44))));
  }
}

// The vertical deflection gone: the whole picture squeezed into a few lines
// of raster, white-hot because all of the beam is there, breathing with the
// supply and now and then heaving open as if it might come back. The rest of
// the scope is the picture burnt into the glass, barely.
static SC_NOINLINE void scwCollapse(uint32_t t, uint32_t f) {
  float h = 4.0f + 9.0f * fxNoise1(t * 0.0016f, f);
  const uint32_t b = t / 900, u = t % 900;
  if (fxHash2(b, f + 3) % 4 == 0 && u < 220) h += 70.0f * (1.0f - u / 220.0f);
  const int hh = fxMaxI(1, fxMinI(SCW_H / 2, (int)h));
  const int c = (SCW_Y0 + SCW_Y1) / 2 + (int)(fxS(fxHash2(f, 9)) * 40.0f);
  const int top = fxMaxI(SCW_Y0, c - hh / 2), bot = fxMinI(SCW_Y1, top + hh);
  const float k = (float)SCW_H / (float)(bot - top);
  // Squeezed in place, outward from the row that stays put, so nothing is
  // read after it has been written over.
  auto src = [&](int y) { return fxMinI(SCW_Y1 - 1, SCW_Y0 + (int)((y - top) * k)); };
  const int ys = fxMaxI(top, fxMinI(bot - 1, (int)((top * k - SCW_Y0) / (k - 1.0f))));
  for (int y = ys; y >= top; y--) { int s = src(y); if (s != y) memcpy(scwRow(y), scwRow(s), FX_W * 2); }
  for (int y = ys + 1; y < bot; y++) { int s = src(y); if (s != y) memcpy(scwRow(y), scwRow(s), FX_W * 2); }
  const int core = (top + bot) / 2;
  for (int y = SCW_Y0; y < SCW_Y1; y++) {
    uint16_t* row = scwRow(y);
    if (y < top || y >= bot) { for (int x = 0; x < FX_W; x++) row[x] = fxDim(row[x], 30); continue; }
    for (int x = 0; x < FX_W; x++) row[x] = row[x] ? fxBoost(row[x], 380) : FX_PAL[FXP_EMBER];
  }
  // The beam itself, white-hot down the middle, and its bloom on the glass
  // either side -- uneven along its length, the way a tired tube burns.
  static const uint8_t GLOW[6] = { FXP_WHITE, FXP_GLOW, FXP_HOT, FXP_FLAME, FXP_RUST, FXP_TRACK };
  for (int x = 0; x < FX_W; x++) {
    const int reach = (int)(fxNoise1(x * 0.07f + t * 0.004f, f + 21) * 5.0f) + 2;
    for (int d = -reach; d <= reach; d++) {
      const int y = core + d, r = fxMinI(5, d < 0 ? -d : d) + (reach < 4 ? 1 : 0);
      if (y < SCW_Y0 || y >= SCW_Y1) continue;
      uint16_t& p = fxDst[y * FX_W + x];
      const uint16_t c = FX_PAL[GLOW[fxMinI(5, r)]];
      if (fxLum(p) < fxLum(c)) p = c;
    }
  }
}

// Horizontal sync gone: every line starts a little late, so the picture
// leans over into diagonal bands with the blanking showing black between
// them. Now and then it nearly catches and only stands crooked, sliding.
static SC_NOINLINE void scwShear(uint32_t t, uint32_t f) {
  const float dir   = (float)((int)((f >> 1) & 2) - 1);
  const float slope = fmaxf(0.0f, fxNoise1(t * 0.0006f, f + 5) - 0.22f) * 3.4f * dir;
  const float drift = t * 0.05f;
  for (int y = SCW_Y0; y < SCW_Y1; y++) {
    const int k = (int)(drift + (y - SCW_Y0) * slope);
    uint16_t* row = scwRow(y);
    scwShiftRow(row, k);
    int bx = k % FX_W;
    if (bx < 0) bx += FX_W;
    for (int i = 1; i <= 26; i++) row[(bx - i + FX_W) % FX_W] = FX_PAL[FXP_INK];   // where the line should have started
    row[(bx - 27 + FX_W) % FX_W] = FX_PAL[FXP_DIM];
  }
}

// The scope's character RAM gone bad: an address line stuck, so whole strips
// of the picture turn up twice; a page stuck on one glyph, or on nothing;
// runs of junk across the rows, a third of them churning while you watch.
static SC_NOINLINE void scwVram(uint32_t t, uint32_t f) {
  const int GY = SCW_Y0 + 2, NC = FX_W / 6, NR = (SCW_Y1 - GY) / 8;
  FxRng R; R.s = f ^ 0x0BADC0DEu;
  for (int i = R.range(1, 3); i > 0; i--) {                       // strips doubled
    const int a = R.range(0, NR - 2), b = R.range(0, NR - 2), n = R.range(1, 2);
    if (a != b)
      for (int y = 0; y < n * 8; y++) memcpy(scwRow(GY + b * 8 + y), scwRow(GY + a * 8 + y), FX_W * 2);
  }
  for (int p = R.range(1, 2); p > 0; p--) {                       // a page stuck
    const int cw = R.range(4, 14), ch = R.range(2, 7), cx = R.range(0, NC - cw), ry = R.range(0, NR - ch);
    const uint32_t h = R.next() | 1;
    const bool dead = (h & 6) == 0;
    for (int r = 0; r < ch; r++)
      for (int c = 0; c < cw; c++) {
        const int x = (cx + c) * 6, y = GY + (ry + r) * 8;
        if (dead) fxRect(x, y, x + 6, y + 8, FX_PAL[FXP_INK]);
        else scwJunk(x, y, h);                                     // the same cell, over and over
      }
  }
  for (int i = R.range(10, 22); i > 0; i--) {                     // runs of junk
    const int ry = R.range(0, NR - 1), cx = R.range(0, NC - 2), len = R.range(2, 18);
    const uint32_t rs = R.next(), tick = rs % 3 == 0 ? t / 140 : 0;
    for (int j = 0; j < len && cx + j < NC; j++) {
      const uint32_t h = fxHash2(rs + (uint32_t)j, tick);
      if (h % 5 == 0) continue;                                    // the picture, through a gap
      scwJunk((cx + j) * 6, GY + ry * 8, h);
    }
  }
}

// Half the tube dark: a dead sector with a fizzing edge and the odd spark
// across it, and the half still going a little weak and unsteady.
static SC_NOINLINE void scwHalf(uint32_t t, uint32_t f) {
  FxRng R; R.s = f ^ 0x5EC7u;
  const int side = R.range(0, 3);                                 // dead: 0 left 1 right 2 top 3 bottom
  const int pct = R.range(35, 60), lead = (side & 1) ? 100 - pct : pct;
  const bool vert = side < 2;
  const int base = vert ? FX_W * lead / 100 : SCW_Y0 + SCW_H * lead / 100;
  const int len = vert ? SCW_H : FX_W;
  const uint32_t fr = t / 140, weak = 170 + fxHash2(fr, f) % 70;
  int16_t ex[FX_W];                                               // the edge, along it
  for (int i = 0; i < len; i++)
    ex[i] = (int16_t)(base + (int)(fxNoise1(i * 0.12f, f) * 10.0f) - 5 + (int)(fxHash2((uint32_t)i, fr ^ f) % 3) - 1);
  for (int y = SCW_Y0; y < SCW_Y1; y++) {
    uint16_t* row = scwRow(y);
    for (int x = 0; x < FX_W; x++) {
      const int e = vert ? ex[y - SCW_Y0] : ex[x], p = vert ? x : y;
      if ((side & 1) ? p >= e : p < e) row[x] = FX_PAL[FXP_INK];
      else row[x] = fxDim(row[x], weak);
    }
  }
  for (int i = 0; i < len; i += 2) {                              // the edge fizzing
    const uint32_t h = fxHash2((uint32_t)i * 31u, fr + f);
    if (h % 3) continue;
    const int e = ex[i] + (int)((h >> 8) % 7) - 3;
    const uint16_t c = FX_PAL[(h >> 16) & 1 ? FXP_HDR : FXP_LINE];
    if (vert) scPx(e, SCW_Y0 + i, c); else scPx(i, e, c);
  }
  if (fxHash2(fr, f + 1) % 3) return;
  FxRng K; K.s = fxHash2(fr, f + 2);                              // a spark across the dark
  for (int n = K.range(1, 3); n > 0; n--) {
    const int L = K.range(6, 50);
    if (vert) {
      const int y = K.range(SCW_Y0, SCW_Y1 - 1), e = ex[y - SCW_Y0];
      const int x0 = side == 0 ? K.range(0, fxMaxI(0, e - 12)) : K.range(fxMinI(FX_W - 1, e + 6), FX_W - 1);
      const int x1 = side == 0 ? fxMinI(x0 + L, e - 4) : fxMinI(x0 + L, FX_W);
      for (int x = x0; x < x1; x++) scPx(x, y, FX_PAL[x == x0 ? FXP_WHITE : FXP_HDR]);
    } else {
      const int x0 = K.range(0, FX_W - 8);
      const int y = side == 2 ? K.range(SCW_Y0, fxMaxI(SCW_Y0, base - 10)) : K.range(fxMinI(SCW_Y1 - 1, base + 10), SCW_Y1 - 1);
      for (int x = x0; x < x0 + L && x < FX_W; x++) scPx(x, y, FX_PAL[x == x0 ? FXP_WHITE : FXP_HDR]);
    }
  }
}

// The scope gave up partway down and is dumping its memory instead: the map's
// own bytes, eight to a line, scrolling up under a fault header, the terrain
// codes showing as whatever glyphs they happen to be.
static SC_NOINLINE void scwDump(const SceneSnap& S, uint32_t t, uint32_t f) {
  const int y0 = 112 + (int)(fxHash2(f, 11) % 80);
  for (int x = 0; x < FX_W; x++) {                                // a torn edge
    const int e = y0 - (int)(fxNoise1(x * 0.09f, f + 13) * 9.0f);
    for (int y = e; y < y0 + 10; y++) fxDst[y * FX_W + x] = FX_PAL[FXP_INK];
  }
  for (int y = y0 + 10; y < SCW_Y1; y++) fxRect(0, y, FX_W, y + 1, FX_PAL[FXP_INK]);
  char b[40];
  snprintf(b, sizeof(b), "?SCOPE FAULT %02X  CORE DUMP", (unsigned)(fxHash2(f, 12) & 0x3F));
  fxGlcd(b, 3, y0 + 2, 1, FX_PAL[FXP_CRIT]);
  const int n = S.terr ? S.cols * S.rows : 0;
  const uint32_t first = t / 220;
  for (int i = 0, y = y0 + 14; y + 8 <= SCW_Y1 - 2; i++, y += 9) {
    const uint32_t ln = first + (uint32_t)i, at = (ln * 8u) % (uint32_t)(n > 0 ? n : 4096);
    snprintf(b, sizeof(b), "%04X", (unsigned)((0x3F00u + at) & 0xFFFFu));
    int x = 3 + fxGlcd(b, 3, y, 1, FX_PAL[FXP_EMBER]);
    uint8_t v[8];
    for (int k = 0; k < 8; k++) {
      const uint32_t h = fxHash2(ln * 8u + (uint32_t)k, f);
      v[k] = n > 0 ? S.terr[(at + (uint32_t)k) % (uint32_t)n] : (uint8_t)h;
      if (h % 11 == 0) v[k] ^= (uint8_t)(h >> 8);                 // and some of it is wrong
      snprintf(b, sizeof(b), " %02X", (unsigned)v[k]);
      x += fxGlcd(b, x, y, 1, FX_PAL[FXP_OK]);
    }
    x += 6;
    for (int k = 0; k < 8; k++) {
      const char g[2] = { (char)(v[k] ? v[k] : '.'), 0 };
      x += fxGlcd(g, x, y, 1, FX_PAL[FXP_HDR]);
    }
  }
}

// -- the small faults, on top --
static void scwScan(uint32_t f) {                    // a field dropped: every other line faint
  for (int y = SCW_Y0 + (int)((f >> 9) & 1); y < SCW_Y1; y += 2) scwDimRow(y, 70);
}
static void scwSmear() {                             // the phosphor hanging on, smeared rightward
  for (int y = SCW_Y0; y < SCW_Y1; y++) {
    uint16_t* row = scwRow(y);
    uint16_t trail = 0;
    uint32_t tl = 0;
    for (int x = 0; x < FX_W; x++) {
      const uint32_t l = fxLum(row[x]);
      if (l + 8 < tl) row[x] = trail;
      else { trail = row[x]; tl = l; }
      trail = fxDim(trail, 212);
      tl = (tl * 212u) >> 8;
    }
  }
}
static void scwCols(uint32_t t, uint32_t f) {        // a column or two stuck dead, or stuck on
  FxRng R; R.s = f ^ 0xC011u;
  for (int i = R.range(1, 3); i > 0; i--) {
    const int x0 = R.range(6, FX_W - 8), w = R.range(1, 2), kind = R.range(0, 2);
    for (int y = SCW_Y0; y < SCW_Y1; y++) {
      uint16_t c = FX_PAL[FXP_INK];
      if (kind) c = FX_PAL[(fxHash2((uint32_t)(y / 3), (kind == 2 ? t / 140 : 0) + f) & 7) == 0 ? FXP_HDR : FXP_DIM];
      for (int x = x0; x < x0 + w; x++) fxDst[y * FX_W + x] = c;
    }
  }
}
static void scwSnow(uint32_t t, uint32_t f) {        // snow, and a streak or two
  FxRng R; R.s = fxHash2(t / 70, f);
  for (int n = 140 + R.range(0, 240); n > 0; n--) {
    const int x = R.range(0, FX_W - 1), y = R.range(SCW_Y0, SCW_Y1 - 1);
    fxDst[y * FX_W + x] = FX_PAL[R.range(0, 5) == 0 ? FXP_WHITE : FXP_LINE];
  }
  for (int n = R.range(0, 2); n > 0; n--) {
    const int y = R.range(SCW_Y0, SCW_Y1 - 1), x0 = R.range(0, FX_W - 12), len = R.range(10, 90);
    const uint16_t c = FX_PAL[R.range(0, 1) ? FXP_HDR : FXP_INK];
    for (int x = x0; x < x0 + len && x < FX_W; x++) fxDst[y * FX_W + x] = c;
  }
}
static void scwType(uint32_t t, uint32_t f) {        // the lettering round the scope, rotting
  FxRng R; R.s = f ^ 0x7E77u;
  for (int i = R.range(3, 8); i > 0; i--) {
    const int x = R.range(0, FX_W / 6 - 1) * 6;
    const int y = R.range(0, 2) == 0 ? 2 + R.range(0, 1) * 8 : 257 + R.range(0, 4) * 8;
    const uint32_t s = R.next(), h = fxHash2(s, (s & 1) ? t / 280 : 0);
    if ((s & 1) && h % 4 == 0) continue;               // the right letter, for a moment
    scwJunk(x, y, h);
  }
}

// What the terminal says about it, typed out on the remark line, the cursor
// waiting after it.
static const char* const SCW_SAYS[SCW_MAJOR_N][2] = {
  { "VERTICAL HOLD: GONE. LIKE EVERYONE.",  "V-HOLD LOST. THE MAP IS IN THERE." },
  { "YOKE FAULT. THE WORLD IS FLAT NOW.",   "DEFLECTION OUT. ONE LINE LEFT." },
  { "NO H-SYNC. EVERYTHING LEANS.",         "SYNC LOST. TRY SQUINTING." },
  { "PARITY ERROR AT C3F0. AND C3F1. AND",  "VIDEO RAM: MOSTLY OPINIONS NOW." },
  { "SECTOR 7 NOT RESPONDING. NOR IS 8.",   "HALF A MAP. THE WORSE HALF." },
  { "?SCOPE FAULT -- DUMPING CORE",         "CORE DUMPED. IT WAS ALL WE HAD." },
};
static const char* const SCW_ANY[] = {
  "HAVE YOU TRIED HITTING IT?", "WARRANTY VOID SINCE THE BOMBS.", "SIGNAL LOST. SO ARE YOU.",
  "?REDO FROM START", "THE MAP IS FINE. THE SCREEN IS NOT.", "NO CARRIER", "PRESS B. COME BACK. PRAY.",
};
static void scwOwnUp(uint32_t t, uint32_t now, uint32_t f, uint8_t big) {
  const uint32_t h = fxHash2(f, 303);
  const char* s = (h & 1) ? SCW_SAYS[big][(h >> 1) & 1] : SCW_ANY[(h >> 2) % SC_NN(SCW_ANY)];
  const int len = (int)strlen(s), n = fxMinI(len, (int)(t / 45));
  char b[48];
  snprintf(b, sizeof(b), "%.*s", n, s);
  fxRect(0, 299, FX_W, FX_H, FX_PAL[FXP_INK]);
  const int w = fxGlcd(b, 3, 304, 1, FX_PAL[FXP_CRIT]);
  if (n < len || ((now / 380) & 1)) fxRect(3 + w, 304, 3 + w + 5, 311, FX_PAL[FXP_CRIT]);   // the cursor
}
// Now and then the whole glass takes a knock with it (the compositor's tear).
static void scwKick(uint32_t t, uint32_t now, uint32_t f) {
  static uint32_t last = 0;
  const uint32_t b = t / 1300, key = b ^ f;
  if (key == last) return;
  last = key;
  const uint32_t h = fxHash2(b, f + 17);
  if (h % 3 == 0) fxTearBurst(now, (uint8_t)(1 + (h >> 8) % 2), 8 + (int)((h >> 12) % 14), 180);
}

static void scWasteBreak(const SceneSnap& S, uint32_t now) {
  const uint32_t f = S.wasteFault, t = now - S.wasteSince;
  const uint8_t big = scWasteMajor(f), small = scWasteMinor(f);
  if (small & SCW_SMEAR) scwSmear();               // in the phosphor, so under whatever the timing does
  switch (big) {
    case SCW_ROLL:     scwRoll(t, f); break;
    case SCW_COLLAPSE: scwCollapse(t, f); break;
    case SCW_SHEAR:    scwShear(t, f); break;
    case SCW_VRAM:     scwVram(t, f); break;
    case SCW_HALF:     scwHalf(t, f); break;
    default:           scwDump(S, t, f); break;
  }
  if (small & SCW_SCAN) scwScan(f);
  if (small & SCW_COLS) scwCols(t, f);
  if (small & SCW_SNOW) scwSnow(t, f);
  if (small & SCW_TYPE) scwType(t, f);
  scwOwnUp(t, now, f, big);
  scwKick(t, now, f);
}

// ── 12. Takeover: THE LAUGH ──────────────────────────────────────────────────
// When someone goes down the LCD stops being a screen for six seconds and is
// the Creeping Doom, laughing at it. The gas pulls itself into the skull while
// the knock lands, then it laughs to one of doom-laugh.h's scores -- the score
// the speaker is laughing to, counted from the same moment, so the jaw drops
// on every HA: head thrown back on the big one, rocking with the rest, the
// embers flaring, the glass jolting. Under it, on the dirt, whoever it was,
// its gas pooling round them; around it, the noise it is making, lettered in
// its own green. At the end it lets go of the face. The name and the
// chronicle's line sit under the ground.
//
// The board fills a SceneDeath (ui-death.hpp) and calls this every frame of
// the takeover; scripts/fxsim renders it as death0..2.
#include "doom-laugh.h"

struct SceneDeath {
  char    who[16];          // the fallen
  char    line[48];         // the chronicle's line for them, a predicate
  uint8_t seat;             // their archetype: the body on the ground
  uint8_t laugh;            // which of doom-laugh.h's
};
static const int      SCD_GROUND = 258;        // the dirt
static const float    SCD_R      = 54.0f;      // the cranium, px
static const uint32_t SCD_LETGO  = 4900;       // ms in: it lets go of the face

// The comic's caption for it -- the voice that is not anybody -- one per laugh.
static const char* const SCD_QUIP[DOOM_LAUGH_N] = {
  "SOMETHING OUT THERE|FINDS THIS HILARIOUS.",
  "NEVER ONE FOR|A EULOGY.",
  "IT HAS BEEN WAITING|ALL DAY FOR THAT.",
};

// The noise syllable i makes, as lettered. A said line says its words
// (DOOM_LAUGH_SAYS); a laugh spells itself -- MWA, HA, HO, HEH -- shouted and
// stretched on the loudest, with a bang on a long loud one, in lower case as
// it runs out of breath.
static void scLaughWord(uint8_t v, int i, char* out, size_t cap) {
  const uint8_t vi = v % DOOM_LAUGH_N;
  if (DOOM_LAUGH_SAYS[vi]) { snprintf(out, cap, "%s", DOOM_LAUGH_SAYS[vi][i]); return; }
  const DoomLaughSyl& s = DOOM_LAUGH[vi].s[i];
  const bool hum = s.cons == 'm', top = s.amp >= 250, lng = s.len > 300;
  const char vw = s.vowel == 'o' ? 'O' : (s.vowel == 'e' ? 'E' : 'A');
  char b[16];
  int n = 0;
  b[n++] = hum ? 'M' : 'H';
  if (hum) b[n++] = 'W';
  for (int k = 0; k < (top || (lng && s.amp < 140) ? 3 : 1); k++) b[n++] = vw;
  if (vw == 'E' && !lng) b[n++] = 'H';
  if (top || (lng && !hum && s.amp >= 200)) b[n++] = '!';
  b[n] = 0;
  if (s.amp < 140) {
    for (int k = 0; k < n; k++) b[k] = (char)tolower((unsigned char)b[k]);
    if (lng) { b[n++] = '.'; b[n++] = '.'; b[n++] = '.'; b[n] = 0; }
  }
  snprintf(out, cap, "%s", b);
}

// The noise in a balloon: the chronicle's starburst (scBang), but the Doom's
// -- its gas green inside an ink rim, the word in ink with a green shadow.
// `pop` > 1 while it slams in: the balloon overshoots and the word lands a
// size up first.
static void scLaughBang(const char* w, float cx, float cy, int sc, float pop, uint32_t seed) {
  const int N = 12;
  float px[N], py[N];
  FxRng R; R.s = seed;
  const float rx = scTypeW(w, sc) * 0.5f + 5.0f * sc, ry = 9.0f * sc;
  for (int i = 0; i < N; i++) {
    float a = i * (FX_TAU / N) + R.sgn() * 0.14f, rr = (i & 1) ? 0.62f + 0.1f * R.u() : 1.0f + 0.28f * R.u();
    px[i] = cx + cosf(a) * rx * rr * pop; py[i] = cy + sinf(a) * ry * rr * pop;
  }
  for (int pass = 0; pass < 2; pass++) {
    const float g = pass ? 1.0f : 1.14f;
    const uint16_t c = FX_PAL[pass ? FXP_GAS_L : FXP_INK];
    for (int i = 0; i < N; i++) {
      int j = (i + 1) % N;
      fxTri(cx, cy, cx + (px[i] - cx) * g, cy + (py[i] - cy) * g, cx + (px[j] - cx) * g, cy + (py[j] - cy) * g, c);
    }
  }
  const int ws = pop > 1.12f ? sc + 1 : sc, tw = scTypeW(w, ws);
  const int x = (int)(cx - tw * 0.5f), y = (int)(cy - 3.5f * ws);
  fxGlcd(w, x + 1 + ws / 2, y + 1 + ws / 2, ws, FX_PAL[FXP_GAS]);
  fxGlcd(w, x, y, ws, FX_PAL[FXP_INK]);
}

static void scDeathDraw(const SceneDeath& D, uint32_t now, uint32_t since, uint16_t* buf) {
  scReset(buf);
  memset(buf, 0, FX_W * FX_H * sizeof(uint16_t));     // FXP_INK is 0
  scFigT = now;
  const uint8_t  v    = D.laugh % DOOM_LAUGH_N;
  const DoomLaugh& L  = DOOM_LAUGH[v];
  const float    t    = now * 0.001f;
  const int32_t  ls   = (int32_t)since - (int32_t)DOOM_LAUGH_DELAY_MS;   // into the laugh
  const uint32_t seed = 666u + v;

  // How well the gas holds the face: it pulls together while the knock lands,
  // and lets go again at the end.
  const float form = fxSmooth(fxClampF(((float)since - 120.0f) / 600.0f, 0.0f, 1.0f));
  const float let  = fxSmooth(fxClampF(((float)since - (float)SCD_LETGO) / 1000.0f, 0.0f, 1.0f));
  float coh = (0.2f + 0.8f * form) * (1.0f - 0.8f * let);

  // The laugh: the jaw on the score, the head with it.
  int syl = -1;
  const float open = ls >= 0 ? doomLaughOpen(v, ls, &syl) : 0.0f;
  float jaw = 0.04f, turn = 0.0f, lift = 0.0f, jolt = 0.0f, ember = 0.6f * form;
  if (syl >= 0) {
    const DoomLaughSyl& s = L.s[syl];
    const float u = fxClampF((float)(ls - (int32_t)s.at) / (float)s.len, 0.0f, 1.0f);
    const float side = (syl & 1) ? 1.0f : -1.0f;
    jaw   = 0.04f + 0.82f * open;
    turn  = side * 0.22f * sinf(3.14159f * u) * (s.amp / 255.0f);      // rocking with it
    jolt  = side * 3.0f * open;
    lift  = SCD_R * open * (s.amp >= 250 ? 0.20f : 0.10f);             // the big one: head back
    ember = 0.55f + 0.45f * open;
  } else if (ls >= 0) {
    jaw = 0.12f;                                                        // after: the grin it keeps
    ember = ls > (int32_t)doomLaughMs(v) ? 0.7f : 0.55f;
  }
  jaw   = fxLerp(jaw, 0.30f, let);                                      // and the jaw falls slack
  ember *= 1.0f - let;

  // A jolt down the glass on every syllable, hardest on the loudest.
  static int32_t lastSince = -1, lastSyl = -1;
  if ((int32_t)since < lastSince) lastSyl = -1;
  lastSince = (int32_t)since;
  if (syl >= 0 && syl != lastSyl) { lastSyl = syl; fxImpact(0.08f + 0.3f * fxCubicIn(L.s[syl].amp / 255.0f)); }

  const float cx = 120.0f + jolt, cy = 124.0f - lift + sinf(t * 2.1f) * 2.5f;

  // 1. The gas it is made of, going up off it.
  fxGasPlume(cx, cy, SCD_R, 0.12f * sinf(t * 0.7f), -1.0f, 2.3f, coh, t, seed, 1.0f + 0.3f * let);

  // 2. The dirt, and whoever it was on it, in a pool of its gas.
  for (int y = SCD_GROUND - 4; y < SCD_GROUND + 9; y++) scTone(y, 0, FX_W, y < SCD_GROUND - 2 ? 2.4f : 1.4f);
  for (int x = 0; x < FX_W; x += 2) {
    float n = fxNoise1(x * 0.07f, 31) * 2.0f;
    scPx(x, SCD_GROUND - 4 - (int)n, FX_PAL[FXP_TRACK]);
  }
  for (int y = SCD_GROUND - 12; y < SCD_GROUND + 7; y++)
    for (int x = 28; x < 212; x++) {
      float dx = fabsf(x - 120.0f) / 92.0f, dy = fabsf(y - (SCD_GROUND - 2.0f)) / 10.0f;
      float d = (1.0f - dx) * (1.0f - dx) * (1.0f - fminf(1.0f, dy)) * 0.75f * form * (1.0f - 0.5f * let);
      d *= 0.7f + 0.6f * fxNoise2(x * 0.09f + t * 0.4f, y * 0.2f, 47);
      if (d > 0.02f && scDot(x, y, d)) scPx(x, y, FX_PAL[scDot(x + 3, y + 5, d * 0.6f) ? FXP_GAS : FXP_GAS_D]);
    }
  // Rimmed in the dirt's own dull amber, not the gas: scFigureDraw takes any
  // rung from FXP_FLAME up for firelight and lights a sliver of one side, and
  // a black body on black dirt is no body at all.
  ScFig F = { 112.0f, (float)SCD_GROUND, 1.0f, (uint8_t)(D.seat % SC_SEATS), SCP_LIE, D.seat, 0.0f,
              FXP_BRICK, 0.0f, -1.0f, 1.75f };
  for (int q = 0; q < 3; q++) scFigure(F, q);

  // 3. Its gas reaching down to them out of the jaw, then the face itself.
  fxGasPlume(cx - 2.0f, cy + SCD_R * 0.95f, 22.0f, -0.08f, 1.0f, 2.4f, 0.8f * coh, t, seed + 7, 0.75f * form);
  fxGasSkull(cx, cy, SCD_R, coh, turn, jaw, ember, 0.0f, -1.0f, t, seed);

  // 4. The noise, a balloon a syllable, slamming in on its side of the head
  // (the loudest dead centre over it), gone again a beat after.
  int shown = 0;
  for (int i = 0; i < L.n && ls >= 0; i++) {
    const DoomLaughSyl& s = L.s[i];
    if (s.amp < 100) continue;
    const int k = shown++;
    const int32_t age = ls - (int32_t)s.at, life = (int32_t)s.len + (s.amp >= 250 ? 800 : 520);
    if (age < 0 || age >= life || (int32_t)since >= (int32_t)SCD_LETGO + 400) continue;
    char w[16];
    scLaughWord(v, i, w, sizeof(w));
    const bool top = s.amp >= 250;
    const int  sc  = top ? 4 : (s.amp >= 180 ? 3 : 2);
    const uint32_t h = fxHash2((uint32_t)i, seed);
    float bx, by;
    if (top) { bx = 120.0f; by = 46.0f; }
    else {
      const float side = (k & 1) ? 1.0f : -1.0f;
      bx = 120.0f + side * (82.0f + fxU(h) * 10.0f);
      // under the shout's balloon, over the caption box
      by = 104.0f + (float)(((k >> 1) * 31) % 64) + fxU(h >> 8) * 8.0f;
    }
    const float half = scTypeW(w, sc) * 0.5f + 7.0f * sc;
    bx = fxClampF(bx, half, FX_W - half);
    const float pop = age < 110 ? 1.0f + 0.5f * (1.0f - fxBackOut(age / 110.0f)) + 0.12f : 1.0f;
    fxStip = (uint8_t)(255.0f * fxClampF((float)(age - (life - 200)) / 200.0f, 0.0f, 1.0f) * 0.9f);
    fxStipSeed = h;
    scLaughBang(w, bx + ((now / 80 + i) & 1 ? 0.6f : -0.6f), by, sc, pop, h);
    fxStip = 0;
  }

  // 5. The caption box, once it is properly going.
  if (since > 1400) scNarration(SCD_QUIP[v], 6, SCD_GROUND - 46, since - 1400);

  // 6. Who, and the chronicle's line, under the dirt.
  char nm[16];
  int i = 0;
  for (; D.who[i] && i < 15; i++) nm[i] = (char)toupper((unsigned char)D.who[i]);
  nm[i] = 0;
  const int nw = fxF2Width(nm), nx = (FX_W - nw) / 2;
  fxF2(nm, nx + 1, SCD_GROUND + 14, FX_PAL[FXP_INK]);
  fxF2(nm, nx, SCD_GROUND + 13, FX_PAL[FXP_GLOW]);
  // a line of type, broken at a space if it will not fit on one
  const char* ln = D.line;
  const int maxC = (FX_W - 12) / 6, len = (int)strlen(ln);
  int cut = len;
  if (len > maxC) { cut = maxC; while (cut > 0 && ln[cut] != ' ') cut--; if (cut <= 0) cut = maxC; }
  char l1[48], l2[48];
  snprintf(l1, sizeof(l1), "%.*s", cut, ln);
  snprintf(l2, sizeof(l2), "%s", ln[cut] == ' ' ? ln + cut + 1 : ln + cut);
  scType(l1, (FX_W - scTypeW(l1, 1)) / 2, SCD_GROUND + 35, 1, FXP_HDR);
  if (l2[0]) scType(l2, (FX_W - scTypeW(l2, 1)) / 2, SCD_GROUND + 46, 1, FXP_HDR);
  scReset(buf);
}

// ── 12b. Cut scene: SLEEPING ROUGH ───────────────────────────────────────────
// Somebody slept with no roof over them (network-events.hpp picks one a
// dawn), and the waste would like a word. One of the three stories in
// section 8 plays at the size of the glass, inside a cartoon's frame: it
// irises in on the sleeper, and once the story has landed the world points
// and laughs -- hands in off every edge of the glass, the laugh slammed
// across the top, a vein going on the victim's temple. The review is
// stamped on it, the chronicle's line comes up underneath, and the iris
// closes on their face. That's all, folks. FX_STYLE's holdMs is 6800.
enum : uint16_t { FXR_IN = 380, FXR_STAMP = SCR_MOCK + 450, FXR_CAP = SCR_MOCK + 700, FXR_OUT = 6200, FXR_HOLD = 6600,
                  FXR_SHUT = 6720 };
struct FxRough { uint8_t v, arch, seat, stamped; uint32_t seed; };
static FxRough fxRough;
static const char* const FXR_OPEN[3] = {
  "NIGHT. NO ROOF.|THE GROUND IT IS, THEN.",
  "NIGHT. NO ROOF.|UP A TREE. WHAT COULD|POSSIBLY GO WRONG.",
  "NIGHT. NO ROOF.|FLAT OUT IN THE OPEN.|NOT A MUSCLE MOVES.",
};
static const char* const FXR_REVIEW[3][2] = {
  { "DIRT MOTEL", "ZERO STARS" }, { "BRANCH OFFICE", "CLOSED" }, { "FUNERAL", "POSTPONED" },
};

static void fxRoughBegin(FxCut& C, const FxCueReq& q) {
  fxRough.v       = (uint8_t)((q.arg & 15) % 3);
  fxRough.arch    = (uint8_t)(((q.arg >> 4) & 15) % 6);
  fxRough.seat    = (uint8_t)(q.who >= 0 && q.who < SC_SEATS ? q.who : 255);
  fxRough.seed    = fxHash(C.seed ^ 0x5EE9u);
  fxRough.stamped = 0;
}

// A hand in off the edge of the glass from (ox, oy), pointing at (tx, ty): a
// sleeve, its cuff, a fist, the finger. The crowd a page has no room for,
// drawn the way manga draws it -- just the arms.
static void scPointer(float ox, float oy, float tx, float ty, float k, uint8_t rim) {
  const float dx = tx - ox, dy = ty - oy, L = fmaxf(1.0f, sqrtf(dx * dx + dy * dy));
  const float ux = dx / L, uy = dy / L;
  const float up = (float)((ux >= 0) * 2 - 1);               // the thumb's side: whichever way is up
  const float nx = uy * up, ny = -ux * up;
  const float fist = 3.2f * k, fing = 6.5f * k;
  const float fx = tx - ux * (fing + fist * 0.6f), fy = ty - uy * (fing + fist * 0.6f);
  const float wx = fx - ux * fist * 1.3f, wy = fy - uy * fist * 1.3f;   // the wrist
  for (int pass = 0; pass < 2; pass++) {
    const float o = pass ? 0.0f : -1.0f, g = pass ? 0.0f : 1.4f;
    const uint16_t c = FX_PAL[pass ? FXP_INK : rim];
    fxStroke(ox + o, oy + o, wx + o, wy + o, 6.4f * k + g, 5.2f * k + g, c);
    fxStroke(wx - ux * 1.6f * k + o, wy - uy * 1.6f * k + o, wx + o, wy + o, 6.8f * k + g, 6.8f * k + g, c);
    fxBlob(fx + o, fy + o, ux, uy, fist + g, fist * 0.82f + g, 41, c);
    for (int i = 0; i < 3; i++)                              // the fingers curled under
      fxDisc(fx + ux * (i - 0.6f) * 1.2f * k - nx * fist * 0.75f + o, fy + uy * (i - 0.6f) * 1.2f * k - ny * fist * 0.75f + o,
             1.15f * k + g * 0.6f, c);
    fxStroke(fx + ux * fist * 0.5f + o, fy + uy * fist * 0.5f + o, tx + o, ty + o, 1.9f * k + g, 1.5f * k + g, c);
    fxDisc(tx + o, ty + o, 0.75f * k + g * 0.5f, c);
    fxStroke(fx + nx * fist * 0.6f + o, fy + ny * fist * 0.6f + o, fx + ux * 2.0f * k + nx * fist * 1.05f + o,
             fy + uy * 2.0f * k + ny * fist * 1.05f + o, 1.6f * k + g, 1.2f * k + g, c);   // the thumb, cocked
  }
  const float cx0 = wx - ux * 0.8f * k, cy0 = wy - uy * 0.8f * k;   // the cuff's seam, and a nail catching the light
  fxLine((int)(cx0 + nx * 3.2f * k), (int)(cy0 + ny * 3.2f * k), (int)(cx0 - nx * 3.2f * k), (int)(cy0 - ny * 3.2f * k), FX_PAL[rim]);
  scPx((int)(tx + nx * 0.4f * k), (int)(ty + ny * 0.4f * k), FX_PAL[FXP_GLOW]);
}

// The cartoon's iris: everything outside a circle round (cx, cy) gone to ink.
static void scIris(float cx, float cy, float r) {
  const uint16_t ink = FX_PAL[FXP_INK];
  for (int y = 0; y < FX_H; y++) {
    const float dy = y + 0.5f - cy, h = r * r - dy * dy;
    if (h <= 0) { fxSpan(y, 0, FX_W, ink); continue; }
    const float w = sqrtf(h);
    fxSpan(y, 0, (int)ceilf(cx - w - 0.5f), ink);
    fxSpan(y, (int)ceilf(cx + w - 0.5f), FX_W, ink);
  }
  if (r > 3.0f && r < 400.0f) {                              // its edge catches a little light
    const int n = fxMinI(900, (int)(r * 3.0f));
    for (int i = 0; i < n; i++) {
      const float a = i * FX_TAU / n;
      fxPut((int)(cx + cosf(a) * (r - 0.5f)), (int)(cy + sinf(a) * (r - 0.5f)), FX_PAL[FXP_RUST]);
    }
  }
}

// The review: a card, turned `rot` and struck at `scl`, with the stamp on
// it (fxStampDraw) and five stars underneath, none of them filled in.
static void scReview(const char* l1, const char* l2, float cx, float cy, float rot, float scl, uint32_t seed) {
  const int n = fxMaxI((int)strlen(l1), (int)strlen(l2));
  const float W = n * 11 + 30.0f, H = 84.0f, c = cosf(rot), s = sinf(rot);
  auto at = [&](float u, float v, float* x, float* y) { *x = cx + (u * c - v * s) * scl; *y = cy + (u * s + v * c) * scl; };
  static const float CU[4] = { -1, 1, 1, -1 }, CV[4] = { -1, -1, 1, 1 };
  float qx[4], qy[4];
  for (int pass = 0; pass < 3; pass++) {                     // its shadow, its edge, the card
    const float m = pass == 2 ? 0.0f : 3.0f, off = pass == 0 ? 5.0f : 0.0f;
    for (int i = 0; i < 4; i++) { at(CU[i] * (W * 0.5f + m), CV[i] * (H * 0.5f + m), &qx[i], &qy[i]); qx[i] += off; qy[i] += off; }
    fxPoly(qx, qy, 4, FX_PAL[pass == 2 ? FXP_GLOW : FXP_INK]);
  }
  float sx, sy;
  at(0, -12, &sx, &sy);
  fxStampDraw(l1, l2, sx, sy, rot, scl, seed, true);
  for (int i = 0; i < 5; i++) {
    at((i - 2) * 16.0f, 27.0f, &sx, &sy);
    scStar(sx, sy, 6.5f * scl, FX_PAL[FXP_INK]);
    scStar(sx, sy, 3.6f * scl, FX_PAL[FXP_GLOW]);
  }
}

static void fxRoughFrame(FxCut& C, uint32_t t, uint32_t now, float shx, float shy) {
  FxRough& Rg = fxRough;
  const uint8_t v = Rg.v;
  if (t >= FXR_SHUT) { fxRect(0, 0, FX_W, FX_H, FX_PAL[FXP_INK]); return; }   // the iris is shut
  ScStage P;
  P.x0 = 0; P.y0 = 0; P.x1 = FX_W; P.y1 = FX_H;
  P.k = 2.9f; P.gy = 226.0f + shy; P.cx = 120.0f + shx;
  P.tone = SC_TONE_ILL; P.rim = FXP_HDR; P.seed = Rg.seed; P.now = now; P.live = true; P.night = false;
  P.t0 = C.t0;
  scFigT = now;
  float hx, hy;
  scArtRough(P, v, Rg.arch, Rg.seat, t < SCR_CYC ? t : SCR_CYC - 1, true, &hx, &hy);
  // What the narrator makes of it; for the chalk, the cut to the morning.
  if (t >= 150 && t < SCR_DAWN + (v == 2 ? 0 : 500)) scNarration(FXR_OPEN[v], 8, 8, (t - 150) * 2);   // typed at a clip
  if (v == 2 && t >= SCR_DAWN + 60 && t < SCR_DAWN + 1500) scNarration("THE NEXT MORNING...", 8, 8, (t - SCR_DAWN - 60) * 2);
  // And then everybody else's: three hands in off the edges, one after
  // another, jabbing; a HA off each; the laugh across the top.
  if (t >= SCR_MOCK) {
    const float m = (float)(t - SCR_MOCK);
    const float ORG[3][2] = { { -40.0f, hy + 6.0f }, { 280.0f, hy - 70.0f }, { hx + 40.0f, (float)FX_H + 30.0f } };
    for (int i = 0; i < 3; i++) {
      const float a = fxClampF((m - i * 110.0f) / 220.0f, 0.0f, 1.0f);
      if (a <= 0) continue;
      const float ox = ORG[i][0], oy = ORG[i][1], ddx = hx - ox, ddy = hy - oy;
      const float L = sqrtf(ddx * ddx + ddy * ddy), ux = ddx / L, uy = ddy / L;
      const float jab = 5.0f * fmaxf(0.0f, sinf(m * 0.028f + i * 2.1f));
      const float reach = (L - 26.0f - 6.0f * i) * fxBackOut(a) + jab;
      const float tx = ox + ux * reach, ty = oy + uy * reach;
      scPointer(ox - ux * 60.0f, oy - uy * 60.0f, tx, ty, 3.0f, FXP_HDR);
      if (a >= 1.0f)
        scSay(i == 1 ? "HA!" : "HA", tx - ux * 44.0f - 10.0f, ty - uy * 44.0f - 26.0f - (float)(((t / 110 + i) & 1) * 2), 2, FXP_GLOW);
    }
    if (t >= SCR_MOCK + 300) scVein(hx + 4.5f * P.k, hy - 4.2f * P.k, P.k * 0.9f, now);
    if (C.hasWord) fxWordSlam(C.word, (int32_t)(t - SCR_MOCK - 80), 120.0f + shx, 34.0f + shy + sinf(m * 0.025f) * 3.0f,
                              -0.07f, 55, t / 80);
  }
  // The review, stamped on it.
  if (t >= FXR_STAMP) {
    const float p = fxClampF((float)(t - FXR_STAMP) / 110.0f, 0.0f, 1.0f);
    if (p >= 1.0f && !Rg.stamped) { Rg.stamped = 1; fxImpact(0.35f); }
    scReview(FXR_REVIEW[v][0], FXR_REVIEW[v][1], 120.0f + shx, 98.0f + shy, -0.12f, fxLerp(2.4f, 1.0f, fxCubicIn(p)), Rg.seed);
  }
  // The chronicle's line, in a box at the foot of the glass.
  if (C.capN && t >= FXR_CAP) {
    const float p = fxClampF((float)(t - FXR_CAP) / 150.0f, 0.0f, 1.0f);
    const int bw = C.capW + 14, bh = C.capN * 16 + 8;
    const int x = (int)(8 + shx), y = (int)(FX_H - 10 - bh + shy + (1.0f - fxBackOut(p)) * 24.0f);
    fxRect(x + 3, y + 3, x + bw + 3, y + bh + 3, FX_PAL[FXP_INK]);
    fxRect(x, y, x + bw, y + bh, FX_PAL[FXP_INK]);
    fxRect(x + 1, y + 1, x + bw - 1, y + 2, FX_PAL[FXP_EMBER]);
    fxRect(x + 1, y + bh - 2, x + bw - 1, y + bh - 1, FX_PAL[FXP_EMBER]);
    for (int i = 0; i < C.capN; i++) fxF2(C.cap[i], x + 7, y + 4 + i * 16, FX_PAL[FXP_OK]);
  }
  // The iris: open on the sleeper, and at the end shut on them.
  if (t < FXR_IN) scIris(hx, hy, 2.0f + 420.0f * fxCubicOut((float)t / FXR_IN));
  else if (t >= FXR_OUT && t < FXR_HOLD) scIris(hx, hy, fxLerp(420.0f, 30.0f, fxSmooth((float)(t - FXR_OUT) / (float)(FXR_HOLD - FXR_OUT))));
  else if (t >= FXR_HOLD) scIris(hx, hy, t < FXR_HOLD + 120 ? 30.0f : fxLerp(30.0f, 0.0f, (float)(t - FXR_HOLD - 120) / (float)(FXR_SHUT - FXR_HOLD - 120)));
}

// ── 13. Dispatch ──────────────────────────────────────────────────────────────
// Which screens are scenes (the rest still draw through ui-screens.hpp), and
// how often each wants a new frame.
static inline bool sceneOwns(uint8_t screen) { return screen >= 1 && screen <= 6; }
static inline uint16_t scenePeriod(uint8_t screen) { (void)screen; return 140; }
static void sceneDraw(uint8_t screen, const SceneSnap& S, uint32_t now, uint16_t* buf) {
  scReset(buf);
  scFigT = now;
  switch (screen) {
    case 2:  scChronicleDraw(S, now); break;
    case 3:  scStashDraw(S, now); break;
    case 4:  scOddsDraw(S, now); break;
    case 5:
      scWasteDraw(S, scWasteClock(S, now));
      if (S.wasteFault) scWasteBreak(S, now);
      break;
    case 6:  scAdmiredDraw(S, now); break;
    default: scRoadDraw(S, now); break;
  }
  scReset(buf);
}
