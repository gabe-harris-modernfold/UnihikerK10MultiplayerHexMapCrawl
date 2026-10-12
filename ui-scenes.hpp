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
  int16_t   caravanQ, caravanR, doomQ, doomR, doomRadius;
  bool      caravanOn;
  uint8_t   hatchN;
  int16_t   hatchQ[8], hatchR[8];
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

// The Creeping Doom on the horizon: a mound of dark with arms that do not
// hang still, rising higher the closer it is. Once it cares, it has eyes.
static void scDoomAt(float cx, float base, float s, uint8_t aware, uint32_t now) {
  float H = 10 + s * 78;
  float tt = now * 0.00055f;
  // the mass
  fxBlob(cx, base, 1, 0, 26 + s * 26, H * 0.42f, 404, FX_PAL[FXP_BLOOD]);
  fxBlob(cx + 1, base + 1, 1, 0, 25 + s * 25, H * 0.40f, 404, FX_PAL[FXP_INK]);
  // the arms, tapering, wandering on value noise so they sway rather than wave
  for (int i = 0; i < 9; i++) {
    float u = (i - 4) / 4.0f, x = cx + u * (18 + s * 22), y = base - H * 0.25f;
    float a = -1.5708f + u * 0.5f, w = 5.5f + s * 2;
    float len = H * (0.42f + 0.35f * fxU(fxHash(i + 1)));
    for (int seg = 0; seg < 6; seg++) {
      a += (fxNoise1(tt + i * 3.1f + seg * 0.7f, 71) - 0.5f) * 0.9f;
      float nx = x + cosf(a) * len / 6, ny = y + sinf(a) * len / 6, nw = w * 0.72f;
      fxStroke(x - 0.8f, y - 0.8f, nx - 0.8f, ny - 0.8f, w + 1.2f, nw + 1.0f, FX_PAL[FXP_BLOOD]);
      fxStroke(x, y, nx, ny, w, nw, FX_PAL[FXP_INK]);
      x = nx; y = ny; w = nw;
    }
  }
  if (aware >= 76 && s > 0.3f) {
    float open = aware >= 100 ? 1.0f : 0.35f + 0.25f * fxNoise1(tt * 3, 5);
    // Blinks, now and then. It is not in a hurry.
    if (((now / 180) % 37) == 0) open = 0.0f;
    fxEyePair(cx - 4, base - H * 0.55f, 9 + s * 5, 2.2f + s * 1.5f, 6, open, 0.5f * fxS(fxHash(now / 1500)),
              33, FXP_GLOW, FXP_CRIT, FXP_INK);
  }
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
    scType("JOIN THE HOTSPOT, THEN OPEN", 39, 250, 1, FXP_LINE);
    // plain http:// so nobody expects a padlock
    char url[28];
    snprintf(url, sizeof(url), "http://%s", S.ap);
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

// The Creeping Doom, placed: `s` 0-1 is how much of it there is.
static void scDoomAt(float cx, float base, float s, uint8_t aware, uint32_t now);

// One panel's picture. Everything is placed off the stage, so the same event
// reads at either size.
static void scPanelArt(const ScStage& P, uint8_t glyph, uint8_t arch, uint8_t arch2, int8_t who, int8_t who2) {
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
    case SC_GLY_FORAGE: {
      fig(cx - 6 * k, arch, SCP_CROUCH, 1, 0, seat, k);
      float px = cx + 12 * k;
      float sway = P.live ? (fxNoise1(t * 0.002f, 4) - 0.5f) * 2 : 0;
      fxStroke(px, G, px + sway, G - 12 * k, 1.6f, 1.0f, FX_PAL[FXP_INK]);
      fxBlob(px + sway - 3 * k, G - 9 * k, 0.8f, -0.6f, 3.4f * k, 1.6f * k, 3, FX_PAL[FXP_HDR]);
      fxBlob(px + sway + 3 * k, G - 12 * k, 0.8f, 0.6f, 3.4f * k, 1.6f * k, 4, FX_PAL[FXP_CRIT]);
      scSparkleSmall(px + 9 * k, G - 17 * k, P.live ? t : 0);
      break;
    }
    case SC_GLY_WATER: {
      float px = cx + 8 * k;
      fxBlob(px, G + 1, 1, 0, 16 * k, 2.6f * k, 9, FX_PAL[FXP_GLOW]);                  // the seep
      fxLine((int)(px - 10 * k), (int)G + 1, (int)(px + 7 * k), (int)G + 1, FX_PAL[FXP_WHITE]);
      fxStroke(px + 10 * k, G - 2, px + 10 * k, G - 16 * k, 3.4f, 3.0f, FX_PAL[FXP_INK]);   // a pipe out of nowhere
      fxStroke(px + 10 * k, G - 16 * k, px + 4 * k, G - 16 * k, 3.4f, 3.0f, FX_PAL[FXP_INK]);
      for (int i = 0; i < 3; i++) {
        float ph2 = fmodf((P.live ? t * 0.0016f : 0.2f) + i / 3.0f, 1.0f);
        fxDisc(px + 4 * k, G - 14 * k + ph2 * 13 * k, 1.3f, FX_PAL[FXP_GLOW]);
      }
      fig(cx - 14 * k, arch, SCP_CROUCH, 1, 0, seat, k);
      break;
    }
    case SC_GLY_MEDIC: {
      ScFig D = { cx + 10 * k, G, -1, arch2, SCP_LIE, seat2, 0, P.rim, 0.7f, -0.7f, k };
      for (int p = 0; p < 3; p++) scFigure(D, p);
      fig(cx - 14 * k, arch, SCP_CROUCH, 1, 0, seat, k);
      float bx = cx - 26 * k;                                // the kit, lid up
      fxRect((int)bx - 1, (int)(G - 7 * k) - 1, (int)(bx + 9 * k) + 1, (int)G + 1, FX_PAL[FXP_INK]);
      fxRect((int)bx, (int)(G - 7 * k), (int)(bx + 9 * k), (int)G, FX_PAL[FXP_GLOW]);
      int mx = (int)(bx + 4.5f * k), my = (int)(G - 3.5f * k);
      fxRect(mx - 1, my - 3, mx + 1, my + 3, FX_PAL[FXP_BLOOD]);
      fxRect(mx - 3, my - 1, mx + 3, my + 1, FX_PAL[FXP_BLOOD]);
      break;
    }
    case SC_GLY_SALVAGE: {
      scHeap(cx + 10 * k, G, 46 * k, 16 * k, P.seed, P.rim);
      fxStroke(cx + 2 * k, G - 11 * k, cx + 14 * k, G - 19 * k, 1.8f, 1.4f, FX_PAL[FXP_INK]);   // rebar
      float pop = P.live ? fabsf(sinf(t * 0.004f)) * 5 * k : 3 * k;
      float bx = cx + 12 * k, by = G - 16 * k - pop;          // something comes up out of it
      fxRect((int)(bx - 5 * k), (int)(by - 6 * k), (int)(bx + 5 * k), (int)by, FX_PAL[FXP_INK]);
      fxRect((int)(bx - 4 * k), (int)(by - 5 * k), (int)(bx + 4 * k), (int)by - 1, FX_PAL[FXP_HDR]);
      fxLine((int)(bx - 4 * k), (int)(by - 3 * k), (int)(bx + 4 * k), (int)(by - 3 * k), FX_PAL[FXP_RUST]);
      scSparkleSmall(bx + 8 * k, by - 7 * k, P.live ? t : 0);
      fig(cx - 18 * k, arch, SCP_CROUCH, 1, 0, seat, k);
      break;
    }
    case SC_GLY_SHELTER: {
      float a = cx - 2 * k, b = cx + 30 * k;                  // a lean-to: two poles, a sheet
      fxStroke(a, G, a, G - 26 * k, 1.8f, 1.6f, FX_PAL[FXP_INK]);
      fxStroke(b, G, b, G - 12 * k, 1.8f, 1.6f, FX_PAL[FXP_INK]);
      float sag = P.live ? (fxNoise1(t * 0.003f, 6) - 0.5f) * 2.5f : 0;
      float qx[4] = { a - 4 * k, b + 5 * k, b + 5 * k, a - 4 * k }, qy[4] = { G - 27 * k, G - 11 * k + sag, G - 9 * k + sag, G - 24 * k };
      fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
      fxLine((int)(a - 4 * k), (int)(G - 28 * k), (int)(b + 5 * k), (int)(G - 12 * k + sag), FX_PAL[P.rim]);
      fig(cx - 20 * k, arch, SCP_STAND, 1, 0, seat, k);
      break;
    }
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
    case SC_GLY_CLASH: {
      fig(cx - 16 * k, arch, SCP_REACH, 1, 0, seat, k);
      float lunge = P.live ? fabsf(sinf(t * 0.006f)) * 4 * k : 2 * k;
      float mx = cx + 18 * k - lunge, my = G - 12 * k;       // it: all mouth
      fxBlob(mx, my, 1, 0, 13 * k, 11 * k, 55, FX_PAL[FXP_INK]);
      fxBlob(mx - 1, my - 1, 1, 0, 12 * k, 10 * k, 55, FX_PAL[FXP_SOOT]);
      for (int i = 0; i < 5; i++) {
        float tx = mx - 10 * k + i * 3.2f * k;
        fxTri(tx, my - 2, tx + 1.6f * k, my - 2, tx + 0.8f * k, my + 3 * k, FX_PAL[FXP_GLOW]);
      }
      scPx((int)(mx - 4 * k), (int)(my - 6 * k), FX_PAL[FXP_CRIT]);
      scPx((int)(mx + 3 * k), (int)(my - 6 * k), FX_PAL[FXP_CRIT]);
      fxStroke(cx - 12 * k, G - 31 * k, cx + 2 * k, G - 22 * k, 2.0f, 1.8f, FX_PAL[FXP_INK]);   // the pipe
      scBang("WHAM!", cx + 2 * k, T + (G - T) * 0.32f, 10 * k, P.seed, P.k > 1.3f ? 2 : 1);
      break;
    }
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
    default: {                                               // on the road, as ever
      fig(cx, arch, SCP_WALK, 1, ph, seat, k);
      break;
    }
  }
}

// The sky a panel is set under: its tone decides the hour, its weather the rest.
static void scPanelSet(ScStage& P, uint8_t glyph) {
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
  for (int y = P.y0; y < (int)P.gy; y++) {
    float u = (float)(y - P.y0) / fmaxf(1.0f, P.gy - P.y0);
    scTone(y, P.x0, P.x1, fxLerp(top, hor, u * u * 0.8f + u * 0.2f));
  }
  if (P.night)
    for (int i = 0; i < (P.x1 - P.x0) / 5; i++) {
      uint32_t h = fxHash2((uint32_t)i, P.seed);
      int x = P.x0 + (int)(h % (uint32_t)(P.x1 - P.x0)), y = P.y0 + 2 + (int)((h >> 9) % (uint32_t)fxMaxI(1, (int)(P.gy - P.y0) / 2));
      if (P.live && (fxHash2((uint32_t)i, P.now / 450) & 7) == 0) continue;
      scPx(x, y, FX_PAL[(h >> 20) % 5 == 0 ? FXP_GLOW : FXP_LINE]);
    }
  if (glyph != SC_GLY_LIGHT)
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

static struct { int8_t page; uint32_t t0; } scStashSt = { -1, 0 };

static void scStashDraw(const SceneSnap& S, uint32_t now) {
  int seated[SC_SEATS], nOn = 0;
  for (int i = 0; i < SC_SEATS; i++) if (S.s[i].on) seated[nOn++] = i;
  const int pid = (S.page >= 0 && S.page < SC_SEATS && S.s[S.page].on) ? S.page : -1;
  if (pid != scStashSt.page) { scStashSt.page = (int8_t)pid; scStashSt.t0 = now; }
  const uint32_t since = now - scStashSt.t0;

  // -- the set: dusk over the ruins, the tarp in the dirt --
  ScStage P = { 0, 20, FX_W, 162, 150, 120, 1.35f, SC_TONE_ILL, 9, 0x57A5u + (uint32_t)(pid + 1) * 977u, now, true, false };
  scPanelSet(P, SC_GLY_NONE);
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
        case 4: if (c > 0) {
          float w = 14 + fxMinI(c, 12) * 1.6f, h = 4 + fxMinI(c, 12) * 1.1f;
          scHeap(x, y, w, h, 0x5C4Au + (uint32_t)c, FXP_HDR);
          for (int i = 0; i < fxMinI(c, 8); i++) {           // a bolt, a spring, a something
            uint32_t hh = fxHash(i * 7 + 3);
            float bx = x + fxS(hh) * w * 0.3f, by = y - 2 - fxU(hh >> 8) * h * 0.7f;
            scPx((int)bx, (int)by, FX_PAL[(hh >> 20) & 1 ? FXP_GLOW : FXP_OK]);
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

  // -- the manifest: a clipboard of what is in the pack --
  for (int y = 186; y < 297; y++)
    for (int x = 6; x < 234; x++)
      scDither(x, y, 7.4f + 0.7f * (fxNoise2(x * 0.07f, y * 0.07f, 29) - 0.5f) + ((y - 200) % 10 == 9 && y > 200 ? -1.6f : 0));
  fxRect(6, 186, 234, 187, FX_PAL[FXP_GLOW]);
  fxRect(96, 182, 144, 192, FX_PAL[FXP_INK]);                 // the clip
  fxRect(98, 183, 142, 185, FX_PAL[FXP_HDR]);
  fxGlcd("MANIFEST", 12, 191, 1, FX_PAL[FXP_INK]);
  snprintf(nm, sizeof(nm), "%u/%u", (unsigned)v.packUsed, (unsigned)v.packCap);
  fxGlcd(nm, 228 - scTypeW(nm, 1), 191, 1, FX_PAL[FXP_INK]);
  int shown = (int)(since / 70);                               // written up a line at a time when the page turns
  if (S.itemN == 0) {
    fxGlcd("NOTHING. CONTENTS MAY", 12, 214, 1, FX_PAL[FXP_INK]);
    fxGlcd("HAVE SETTLED ELSEWHERE.", 12, 224, 1, FX_PAL[FXP_INK]);
  }
  for (int i = 0; i < S.itemN && i < 18 && i < shown; i++) {
    int col = i / 9, row = i % 9;
    int x = 12 + col * 112, y = 202 + row * 10;
    char q[6];
    snprintf(q, sizeof(q), "x%u", (unsigned)S.itemQty[i]);
    char it[20];
    int room = (104 - scTypeW(q, 1) - 4) / 6;
    snprintf(it, sizeof(it), "%.*s", fxMaxI(1, fxMinI(19, room)), S.item[i]);
    for (char* p = it; *p; p++) *p = (char)toupper((unsigned char)*p);
    fxGlcd(it, x, y, 1, FX_PAL[FXP_INK]);
    fxGlcd(q, x + 104 - scTypeW(q, 1), y, 1, FX_PAL[FXP_BLOOD]);
  }
  fxDisc(226, 290, 4, FX_PAL[FXP_EMBER]);                      // somebody's thumbprint
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

  // -- the Doom: its ring, then it --
  bool doomOn = S.doomQ >= 0 && S.doomQ < cols && S.doomR >= 0 && S.doomR < rows;
  if (doomOn) {
    int dx, dy;
    cell(S.doomQ, S.doomR, &dx, &dy);
    float cx = dx + XS * 0.5f, cy = dy + YS * 0.5f;
    if (S.doomRadius > 0) {
      float rx = S.doomRadius * XS, ry = S.doomRadius * YS, spin = now * 0.0004f;
      int n = fxMaxI(16, (int)(rx * 0.9f));
      for (int i = 0; i < n; i++) {
        float a = spin + i * (FX_TAU / n), wob = 1.0f + 0.06f * (fxNoise1(i * 0.7f + now * 0.001f, 3) - 0.5f);
        int ex = (int)(cx + cosf(a) * rx * wob), ey = (int)(cy + sinf(a) * ry * wob);
        if (i & 1) continue;                                   // dashes, not a line: it is a guess
        fxRect(ex - 1, ey - 1, ex + 2, ey + 2, FX_PAL[FXP_INK]);
        fxRect(ex, ey, ex + 1, ey + 1, FX_PAL[FXP_CRIT]);
      }
    }
    float br = 3.5f + 1.2f * sinf(now * 0.004f) + S.doomAware * 0.02f;
    fxBlob(cx, cy, 1, 0, br + 1.5f, br + 1.0f, 404, FX_PAL[FXP_BLOOD]);
    fxBlob(cx, cy, 1, 0, br, br * 0.8f, 404, FX_PAL[FXP_INK]);
    if (S.doomAware >= 76) { scPx((int)cx - 1, (int)cy - 1, FX_PAL[FXP_GLOW]); scPx((int)cx + 1, (int)cy - 1, FX_PAL[FXP_GLOW]); }
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
    fxBlob((float)x + 3, ky + 4.0f, 1, 0, 3.5f, 3, 404, FX_PAL[FXP_BLOOD]); fxGlcd("IT", x + 9, ky, 1, FX_PAL[FXP_HDR]);
  }
  {
    static const char* const MOOD[5] = { "NAPPING", "SNIFFING", "INTERESTED", "PECKISH", "HUNTING" };
    int a = S.doomAware, m = a >= 100 ? 4 : a >= 76 ? 3 : a >= 51 ? 2 : a >= 25 ? 1 : 0;
    int y = ky + 14;
    fxGlcd("DOOM MOOD", 4, y + 2, 1, FX_PAL[FXP_BRICK]);
    for (int i = 0; i < 5; i++) {
      bool on = i <= m;
      bool pulse = on && i == m && m >= 3 && ((now / 250) & 1);
      scBits(SC_SKULL, 6, 5, 64 + i * 9, y + 2, FX_PAL[pulse ? FXP_WHITE : on ? FXP_CRIT : FXP_TRACK]);
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

// ── 12. Dispatch ──────────────────────────────────────────────────────────────
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
    case 5:  scWasteDraw(S, now); break;
    case 6:  scAdmiredDraw(S, now); break;
    default: scRoadDraw(S, now); break;
  }
  scReset(buf);
}
