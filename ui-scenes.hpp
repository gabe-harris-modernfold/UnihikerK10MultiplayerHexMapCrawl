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
  int8_t   moves;
  uint8_t  depth;                // 0 on the surface
  bool     inEnc;
  uint16_t score;
};
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
};

// ── 1. Small hands ───────────────────────────────────────────────────────────
static inline void scPx(int x, int y, uint16_t c) {
  if ((unsigned)x < (unsigned)FX_W && (unsigned)y < (unsigned)FX_H) fxDst[y * FX_W + x] = c;
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
static void scDoom(uint8_t close, uint8_t aware, uint32_t now) {
  float s = close / 255.0f;
  if (s < 0.05f) return;
  float cx = 262.0f - s * 78.0f, base = (float)RD_HOR + 2, H = 10 + s * 78;
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

// ── 4. The party ─────────────────────────────────────────────────────────────
// Everyone is a silhouette -- black ink against the sky, a rim of light on the
// side the light is coming from -- and everyone carries what they are: the
// Guide's staff and hat, the Quartermaster's crate, the Medic's bag, the
// Mule's absurd pack, the Scout's radio whip, the Endurer's gas mask.
enum : uint8_t { SCP_WALK = 0, SCP_SIT, SCP_LIE, SCP_STAND };
struct ScFig {
  float   x, foot;
  float   dir;          // +1 faces right
  uint8_t arch, pose, seat;
  float   ph;           // the walk's phase, radians
  uint8_t rim;          // rung of the light catching it
  float   lx, ly;       // unit toward that light
  float   k;            // scale: 1 is a figure 26 px tall
};
static void scFigure(const ScFig& F, int pass) {
  const float d = F.dir;
  // The rim pass is drawn fat, so the sky's light wraps the whole silhouette;
  // a fire's light only catches the side facing it, or a figure by the fire
  // reads as a hollow outline.
  const bool  fire = F.rim >= FXP_FLAME;
  const float g = pass == 0 ? (fire ? 0.4f : 1.3f) : 0.0f;
  const float lo = fire ? 1.6f : 1.0f;
  const float ox = pass == 0 ? F.lx * lo : 0.0f, oy = pass == 0 ? F.ly * lo : 0.0f;
  const uint16_t c = pass == 0 ? FX_PAL[F.rim] : FX_PAL[FXP_INK];
  const float k = F.k;
  auto X = [&](float u) { return F.x + d * u * k + ox; };
  auto Y = [&](float v) { return F.foot - v * k + oy; };
  auto S = [&](float u0, float v0, float u1, float v1, float w0, float w1) {
    fxStroke(X(u0), Y(v0), X(u1), Y(v1), w0 * k + g, w1 * k + g, c);
  };
  auto D = [&](float u, float v, float r) { fxDisc(X(u), Y(v), r * k + g * 0.6f, c); };
  auto B = [&](float u0, float v0, float u1, float v1) {    // a box, in figure space
    float px[4] = { X(u0) - g * 0.5f * d, X(u1) + g * 0.5f * d, X(u1) + g * 0.5f * d, X(u0) - g * 0.5f * d };
    float py[4] = { Y(v1) - g * 0.5f, Y(v1) - g * 0.5f, Y(v0) + g * 0.5f, Y(v0) + g * 0.5f };
    fxPoly(px, py, 4, c);
  };
  const uint8_t a = F.arch;
  const bool mule = a == 3, bulky = a == 5;
  const float tw = bulky ? 6.2f : 4.8f;
  if (pass == 2) {                                           // the details, over the ink
    if (a == 2 && F.pose != SCP_LIE) {                       // the Medic's cross
      float u = F.pose == SCP_SIT ? -4.0f : -3.0f, v = F.pose == SCP_SIT ? 6.0f : 10.5f;
      scPx((int)X(u), (int)Y(v), FX_PAL[FXP_WHITE]);
      scPx((int)X(u), (int)Y(v) - 1, FX_PAL[FXP_WHITE]);
      scPx((int)X(u), (int)Y(v) + 1, FX_PAL[FXP_WHITE]);
      scPx((int)X(u) - 1, (int)Y(v), FX_PAL[FXP_WHITE]);
      scPx((int)X(u) + 1, (int)Y(v), FX_PAL[FXP_WHITE]);
    }
    if (a == 1 && F.pose == SCP_WALK) {                      // planks on the crate
      fxLine((int)X(2.5f), (int)Y(16), (int)X(8), (int)Y(16), FX_PAL[FXP_RUST]);
    }
    if (a == 5 && F.pose != SCP_LIE) {                       // the mask's eye, catching light
      float hv = F.pose == SCP_SIT ? 16.5f : 24.0f;
      scPx((int)X(1.5f), (int)Y(hv), FX_PAL[FXP_GLOW]);
    }
    if (F.pose != SCP_LIE) {                                 // whose they are
      char n[2] = { (char)('1' + F.seat), 0 };
      float hv = F.pose == SCP_SIT ? 25.0f : (mule ? 38.0f : 33.0f);
      scType(n, (int)(F.x - 2), (int)Y(hv) - 6, 1, FXP_HOT);
    }
    return;
  }

  if (F.pose == SCP_WALK || F.pose == SCP_STAND) {
    float ph = F.pose == SCP_STAND ? 0.0f : F.ph;
    float bob = fabsf(sinf(ph)) * 1.1f, lean = mule ? 2.0f : 0.8f;
    float hipV = 11 + bob, shV = 19 + bob, hdV = 23.5f + bob;
    // the far leg and arm first, the near ones over the body
    for (int side = -1; side <= 1; side += 2) {
      float sw = sinf(ph) * side;
      float fu = sw * 5.0f, fv = fmaxf(0.0f, cosf(ph + (side > 0 ? 0.0f : 3.14159f))) * 2.2f;
      if (F.pose == SCP_STAND) { fu = side * 2.0f; fv = 0; }
      float ku = fu * 0.45f + 1.6f, kv = 5.6f + fv * 0.5f;
      S(0, hipV, ku, kv, 2.8f, 2.4f);
      S(ku, kv, fu, fv, 2.4f, 2.0f);
      S(fu - 0.5f, fv + 0.4f, fu + 2.2f, fv + 0.4f, 1.8f, 1.6f);   // the boot
    }
    if (mule) {                                              // the pack, then the pack on the pack
      B(-10.5f, 8, -1.5f, 29);
      S(-9.5f, 30.5f, -2.5f, 30.5f, 3.6f, 3.6f);             // bedroll
      D(-11.5f, 13, 2.2f);                                   // a pan, swinging
    }
    if (a == 2) B(-5.5f, 8.5f, -1.0f, 13);                   // the bag
    if (a == 4) {                                            // the whip
      S(-2, 19 + bob, -5, 39, 1.1f, 0.8f);
      if (pass == 1) {
        float fl = fxNoise1(ph * 0.5f + F.x * 0.1f, 3) * 2.0f;
        fxTri(X(-5), Y(39), X(-5), Y(35), X(-10 - fl), Y(37.5f + fl * 0.3f), FX_PAL[FXP_HOT]);
      }
    }
    S(0, hipV, lean, shV, tw, tw - 0.6f);                    // the body
    D(lean + 0.6f, hdV, 3.2f);                               // the head
    if (bulky) D(lean + 2.6f, hdV - 1.2f, 1.7f);             // the mask's snout
    if (a == 0) {                                            // hat and staff
      S(lean - 4.8f, hdV + 2.6f, lean + 5.0f, hdV + 2.6f, 1.8f, 1.8f);
      B(lean - 2.0f, hdV + 2.6f, lean + 2.6f, hdV + 5.0f);
      float sw = sinf(ph) * 1.5f;
      S(6 + sw, 0, 7 + sw, 30, 1.4f, 1.2f);
      S(lean, shV - 1, 6 + sw, 14, 2.0f, 1.8f);
    } else if (a == 1) {                                     // the crate, both arms round it
      B(2.0f, 12.5f, 8.5f, 19);
      S(lean, shV - 1, 5, 15, 2.0f, 1.8f);
    } else {
      for (int side = -1; side <= 1; side += 2) {
        float hu = -sinf(ph) * side * 4.0f + (a == 4 && side > 0 ? 3 : 0);
        S(lean, shV - 1, hu * 0.5f + lean * 0.6f, shV - 5.5f, 2.2f, 2.0f);
        S(hu * 0.5f + lean * 0.6f, shV - 5.5f, hu, shV - 10.5f, 2.0f, 1.8f);
      }
    }
  } else if (F.pose == SCP_SIT) {
    if (mule) { B(-15, 0, -6, 17); S(-14, 18.5f, -7, 18.5f, 3.4f, 3.4f); }   // the pack, set down at last
    S(0, 4, 6, 6, 3.0f, 2.6f);                               // thigh
    S(6, 6, 8.5f, 0, 2.6f, 2.2f);                            // shin
    S(8, 0.3f, 10.5f, 0.3f, 1.8f, 1.6f);
    S(0, 4, 2.2f, 13, tw, tw - 0.6f);
    D(3.0f, 16.5f, 3.1f);
    if (bulky) D(5.0f, 15.5f, 1.6f);
    if (a == 0) { S(-2, 19.2f, 8, 19.2f, 1.8f, 1.8f); B(1, 19.2f, 5, 21.5f); }
    if (a == 2) B(-5, 2, -1, 6);
    S(2.2f, 12, 7, 10, 2.1f, 1.9f);                          // arms out to the warmth
    S(7, 10, 10.5f, 10.5f, 1.9f, 1.7f);
  } else {                                                   // down, flat out
    S(-8, 2.2f, 7, 2.2f, 4.6f, 4.2f);
    D(-10.6f, 2.8f, 3.0f);
    S(7, 2.0f, 13, 1.2f, 2.6f, 2.2f);
    S(7, 2.6f, 12.5f, 3.4f, 2.4f, 2.2f);
    S(-4, 3.5f, -1, 7.5f, 1.8f, 1.6f);                       // one arm up. still with us
    if (mule) B(-2, 4.5f, 8, 11);                            // still wearing it
  }
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
// logs, and sparks going up to join the stars.
static void scFireGlow(float x, float y, uint32_t now) {
  float fl = 0.85f + 0.15f * fxNoise1(now * 0.012f, 8);
  for (int yy = (int)y - 46; yy < RD_Y1; yy++) {
    if (yy < RD_Y0) continue;
    float dy = (yy - y) / (yy < y ? 42.0f : 9.0f);
    for (int xx = (int)x - 60; xx < (int)x + 60; xx++) {
      if ((unsigned)xx >= (unsigned)FX_W) continue;
      float dx = (xx - x) / 60.0f, r = dx * dx + dy * dy;
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
static void scFire(float x, float y, uint32_t now) {
  fxStroke(x - 9, y + 1, x + 8, y - 2, 3.0f, 2.6f, FX_PAL[FXP_INK]);
  fxStroke(x - 8, y - 2, x + 9, y + 1, 3.0f, 2.6f, FX_PAL[FXP_INK]);
  fxLine((int)x - 8, (int)y - 3, (int)x + 7, (int)y - 4, FX_PAL[FXP_RUST]);
  float t = now * 0.009f;
  static const uint8_t COL[3] = { FXP_FLAME, FXP_HOT, FXP_GLOW };
  for (int layer = 0; layer < 3; layer++) {
    float sc = 1.0f - layer * 0.3f;
    for (int i = 0; i < 5; i++) {
      float u = (i - 2) / 2.0f;
      float h = (9 + 9 * fxNoise1(t + i * 1.7f + layer, 41)) * sc * (1.0f - fabsf(u) * 0.35f);
      float sway = (fxNoise1(t * 1.3f + i, 43) - 0.5f) * 6.0f;
      float bx = x + u * 6.0f * sc;
      fxTri(bx - 3.2f * sc, y - 1, bx + 3.2f * sc, y - 1, bx + sway, y - 1 - h, FX_PAL[COL[layer]]);
    }
  }
  for (int i = 0; i < 9; i++) {
    uint32_t h = fxHash(i * 7 + 1);
    float ph = fmodf(now * (0.0006f + 0.0004f * fxU(h)) + fxU(h >> 4), 1.0f);
    float ex = x + (fxNoise1(ph * 3 + i, 47) - 0.5f) * 26, ey = y - 8 - ph * 60;
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
// The narration box. Lines are '|'-split, at most 30 characters each.
static const char* const SC_NAR_CALM[] = {
  "MEANWHILE, IN THE WASTE...",
  "THE PARTY WALKS.|THE WASTE WATCHES.",
  "ANOTHER GLORIOUS DAY|IN THE POST-EVERYTHING.",
  "NOBODY HAS DIED IN|SEVERAL WHOLE MINUTES.",
  "THE SCENIC ROUTE. EVERY|ROUTE IS SCENIC NOW.",
  "PROGRESS: MEASURABLE.|DIRECTION: DEBATED.",
  "SOMEWHERE, A SETTLEMENT.|PROBABLY.",
};
static const char* const SC_NAR_NIGHT[] = {
  "NIGHT. THE FIRE IS ON|OUR SIDE. MOSTLY.",
  "CAMP. SOMEBODY HAS TO|TAKE FIRST WATCH.",
  "THE DARK MAKES NOISES.|WE MAKE THEM BACK.",
};
static const char* const SC_NAR_DUSK[] = { "THE SUN CLOCKS OFF.|THE WASTE CLOCKS ON." };
static const char* const SC_NAR_DAWN[] = { "ANOTHER DAWN. NOBODY IS|MORE SURPRISED THAN US." };
static const char* const SC_NAR_WX[6][2] = {
  { nullptr, nullptr },
  { "RAIN. FREE WATER, FOR|ONCE. PROBABLY.", "EVERYTHING IS WET.|EVEN THE DRY STUFF." },
  { "THE SKY HAS OPINIONS|TODAY. LOUD ONES.", "LIGHTNING: THE SKY'S|WAY OF COUNTING US." },
  { "RAIN: NOW WITH ADDED|CHEMISTRY.", "DO NOT DRINK|THE WEATHER." },
  { "THE AIR HAS TEXTURE.|DO NOT CHEW IT.", "SMOG. BREATHE LESS." },
  { "VISIBILITY: YOUR OWN|FEET, ON A GOOD DAY.", "FOG. SOMETHING IN IT|IS ALSO LOST." },
};
static const char* const SC_NAR_DOOM[]   = { "SOMETHING ON THE HORIZON|HAS TAKEN AN INTEREST.",
                                             "DON'T LOOK BACK.|IT'S STILL THERE." };
static const char* const SC_NAR_HUNT[]   = { "IT HAS STOPPED|PRETENDING. RUN." };
static const char* const SC_NAR_CLOCK[]  = { "THE CLOCK SAYS LATE.|THE CLOCK IS NEVER WRONG." };
static const char* const SC_NAR_HUNGRY[] = { "RATIONS: A FOND|MEMORY." };
static const char* const SC_NAR_DRY[]    = { "WATER: THEORETICAL." };
static const char* const SC_NAR_DOWN[]   = { "SOMEONE IS HORIZONTAL.|WE DRAG. IT'S TRADITION." };
static const char* const SC_NAR_BELOW[]  = { "SOMEONE WENT BELOW.|THE HOLE SAYS NOTHING." };
static const char* const SC_NAR_EMPTY[]  = { "THE ROAD IS EMPTY.|THE ROAD CAN WAIT.",
                                             "NOBODY OUT HERE. JUST|THE WIND, PRACTISING." };

// Per survivor, the one line the wasteland would say about them right now.
static const char* const SC_QUIP_ARCH[6][2] = {
  { "knows a shortcut. allegedly.",    "reads the stars. and lies." },
  { "counting. always counting.",      "has a list. you're on it." },
  { "has seen worse. was worse.",      "bedside manner: a stick." },
  { "carries everything. complains never.", "the pack has a pack." },
  { "saw something. won't say what.",  "first in. first out. usually." },
  { "fine. it's fine. it's all fine.", "has stopped feeling the feet." },
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
  if (v.moves == 0)         return "out of legs for today.";
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
    static const float CAMP[5] = { -38, 38, -74, 74, -104 };
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
    if (nOn == 0) add(SC_NAR_EMPTY, 2);
    else {
      if (hunt) add(SC_NAR_HUNT, 1);
      else if (doom) add(SC_NAR_DOOM, 2);
      if (down) add(SC_NAR_DOWN, 1);
      if (S.tcWeight > 200) add(SC_NAR_CLOCK, 1);
      if (dry) add(SC_NAR_DRY, 1);
      if (hungry) add(SC_NAR_HUNGRY, 1);
      if (below) add(SC_NAR_BELOW, 1);
      if (SC_NAR_WX[wx][0]) add(SC_NAR_WX[wx], 2);
      if (night) add(SC_NAR_NIGHT, 3);
      else if (dusk) add(SC_NAR_DUSK, 1);
      else if (t < 0.07f) add(SC_NAR_DAWN, 1);
      if (n < 3) add(SC_NAR_CALM, 7);
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
    fxF2(S.ap, (FX_W - fxF2Width(S.ap)) / 2, 262, FX_PAL[FXP_CRIT]);
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
    float spX = lead >= 0 ? 205.0f : 213.0f;
    fxStroke(spX + 2, BY_GROUND - 13, spX - 2, BY_GROUND - 47, 2.6f, 2.4f, FX_PAL[FXP_RUST]);
    fxStroke(spX + 2, BY_GROUND - 13, spX - 2, BY_GROUND - 47, 1.6f, 1.4f, FX_PAL[FXP_INK]);
    float bl[4] = { spX - 1, spX + 6, spX + 5, spX - 2 };
    float bly[4] = { BY_GROUND - 18.0f, BY_GROUND - 18.0f, BY_GROUND - 8.0f, BY_GROUND - 8.0f };
    fxPoly(bl, bly, 4, FX_PAL[FXP_INK]);
    fxLine((int)spX - 1, BY_GROUND - 18, (int)spX + 6, BY_GROUND - 18, FX_PAL[FXP_HDR]);
    if (lead >= 0) {
      const SceneSurv& v = S.s[lead];
      ScFig F = { 222, (float)BY_GROUND - 12, -1.0f, v.arch, SCP_STAND, (uint8_t)lead, 0, FXP_BRICK, 0.7f, -0.7f, 1.35f };
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

// ── 8. Dispatch ──────────────────────────────────────────────────────────────
// Which screens are scenes (the rest still draw through ui-screens.hpp), and
// how often each wants a new frame.
static inline bool sceneOwns(uint8_t screen) { return screen == 1 || screen == 6; }
static inline uint16_t scenePeriod(uint8_t screen) { (void)screen; return 140; }
static void sceneDraw(uint8_t screen, const SceneSnap& S, uint32_t now, uint16_t* buf) {
  scReset(buf);
  switch (screen) {
    case 6:  scAdmiredDraw(S, now); break;
    default: scRoadDraw(S, now); break;
  }
  scReset(buf);
}
