#pragma once
// ── ui-fx.hpp ────────────────────────────────────────────────────────────────
// The LCD as a salvaged amber terminal that an 80s comic book has got into.
//
// Every screen in ui-screens.hpp still renders into `canvas` exactly as it
// did, and nothing in this file draws a screen. What it owns is the step
// between that render and the glass: canvas is composed, a row at a time, into
// a second sprite (fxOut) and fxOut is what gets pushed. Composing is where the
// terminal gets to misbehave -- shake, tear, roll, ghost, sink into screentone
// -- and where the comic gets to cut in over the top of it.
//
// Four rules the whole file keeps:
//
//   1. Everything is calculated, and nothing may look it. There is no sine
//      wave, circle or even spacing anywhere a person can see one. Shapes come
//      out of integer hashes, value noise and power-law sizes; strokes taper
//      the way a brush does; spacing jitters the way a hand does. The one
//      regular structure is the halftone screen, and that reads as print, not
//      maths, because it is how a comic was printed. (The commendation's
//      guilloche is the other, for the same reason: it is how a banknote or
//      a medal certificate was engraved.)
//   2. One hue. An amber phosphor only has a brightness, so the whole comic
//      vocabulary -- ink, screentone, SFX lettering, caption boxes -- is set in
//      the sixteen rungs of FX_PAL, black to white-hot. It is how the single-ink
//      pages of a manga weekly were printed, and it keeps every cut-in inside
//      the palette the screens already use.
//   3. The terminal fails in proportion to the party's dread. g_dread (the
//      snapshot the LEDs read) is folded into one `madness` number, and that
//      alone decides how often the picture tears, whether the hum bar rolls,
//      whether rows shiver, and -- near the top -- whether something flashes a
//      word at you for two frames or opens its eyes in the dark.
//   4. Big moments are comic panels cut into the terminal. A quake, a strike,
//      the Doom turning its head, a new day: a slanted band slams across the
//      screen trailing speed lines, SFX lettering stamps in a letter at a time,
//      a caption box drops under it, and a blade cuts it back out.
//
// Cost. The content screens render exactly as often as before (k10Dirty or
// SCREEN_MS). A frame here is one row pass (~307 KB of PSRAM traffic), the
// overlays, and the ~31 ms SPI push -- ~20 fps flat out, and it only runs that
// fast while something is moving. A calm screen with nothing happening pushes
// nothing at all, exactly as before.
//
// This header compiles on its own under FX_NATIVE: scripts/fxsim/fxsim.cpp
// supplies the handful of hooks in section 0 (fonts, lock, alloc, clock,
// names) and `python scripts/fxsim/fxsim.py` renders the animation to GIF from
// this exact code with MSVC -- see docs/dev-loop.md. Everything that touches
// the board lives in the last section. data/observer-fx.js is the same engine
// for the TV.

// ── 0. Portability ───────────────────────────────────────────────────────────
#ifndef FX_NATIVE
typedef lgfx::GFXfont  FxGfxFont;
typedef lgfx::GFXglyph FxGfxGlyph;
static inline const uint8_t* fxGlcdTable()        { return fonts::Font0.chartbl; }
static inline const uint8_t* fxF2Glyph(uint8_t c) {
  return ((const uint8_t* const*)fonts::Font2.void_chartbl)[c - 32];
}
static inline uint8_t          fxF2W(uint8_t c)     { return fonts::Font2.widthtbl[c - 32]; }
static inline const FxGfxFont& fxSfxFont()          { return fonts::FreeSansBoldOblique24pt7b; }
static portMUX_TYPE fxMux = portMUX_INITIALIZER_UNLOCKED;
#define FX_LOCK()   taskENTER_CRITICAL(&fxMux)
#define FX_UNLOCK() taskEXIT_CRITICAL(&fxMux)
static inline void* fxAlloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM); }
#endif

static const int   FX_W   = 240;
static const int   FX_H   = 320;
static const float FX_TAU = 6.2831853f;

static inline int   fxMinI(int a, int b)     { return a < b ? a : b; }
static inline int   fxMaxI(int a, int b)     { return a > b ? a : b; }
static inline float fxClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float fxLerp(float a, float b, float t)     { return a + (b - a) * t; }
static inline float fxCubicOut(float t) { t = 1.0f - t; return 1.0f - t * t * t; }
static inline float fxCubicIn(float t)  { return t * t * t; }
static inline float fxBackOut(float t)  { t -= 1.0f; return 1.0f + t * t * (2.7f * t + 1.7f); }

// The name of whoever a cue is about, and the clock. Defined with the board
// glue (the name takes G.mutex) or by the desktop harness.
static void     fxNameOf(int8_t who, char* out, size_t cap);
static uint32_t fxNowMs();

// ── 1. The phosphor ──────────────────────────────────────────────────────────
// Sixteen rungs of one amber, black to white-hot, monotonic in brightness. The
// anchors are the screens' own constants (C_BAND, C_TRACK, C_DIM, C_LINE, C_OK,
// C_HDR, C_CRIT), so a cut-in and the dashboard under it are one palette.
// BLOOD is the single step off the ramp: redder and darker, for drips.
enum : uint8_t {
  FXP_INK = 0, FXP_SOOT, FXP_BAND, FXP_TRACK, FXP_DIM, FXP_LINE, FXP_EMBER,
  FXP_RUST, FXP_BRICK, FXP_OK, FXP_HDR, FXP_FLAME, FXP_CRIT, FXP_HOT, FXP_GLOW,
  FXP_WHITE, FXP_BLOOD, FXP_COUNT
};
static const uint32_t FX_PAL_RGB[FXP_COUNT] = {
  0x000000, 0x0C0400, 0x1E0A00, 0x2E1206, 0x3A1808, 0x502010, 0x6E2C0E, 0x8C3A12,
  0xA84A14, 0xC05810, 0xD06818, 0xE07C1C, 0xE89018, 0xF8A830, 0xFFC860, 0xFFF0C8,
  0x6A1004,
};
static uint16_t FX_PAL[FXP_COUNT];    // byte-swapped RGB565, the sprite's own format
static uint8_t  FX_DOT[64];           // the halftone screen, see fxInitTables()
static bool     fxTablesReady = false;
// A ghost is the old picture a few rungs down the ramp, looked up rather than
// scaled: scaling a white-hot pixel toward black passes through grey, and a
// grey ghost reads as a different tube.
static const uint8_t FX_GHOST[16] = { 0, 0, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7 };

static inline uint16_t fxSwap(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

// Brightness of a sprite pixel, 0-255.
static inline uint8_t fxLum(uint16_t sw) {
  uint16_t c = fxSwap(sw);
  uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return (uint8_t)((r * 616 + g * 604 + b * 224) >> 8);
}
// Take light off a sprite pixel without moving its hue. k 0-256.
static inline uint16_t fxDim(uint16_t sw, uint32_t k) {
  uint16_t c = fxSwap(sw);
  uint32_t r = (((c >> 11) & 31) * k) >> 8, g = (((c >> 5) & 63) * k) >> 8, b = ((c & 31) * k) >> 8;
  return fxSwap((uint16_t)((r << 11) | (g << 5) | b));
}
// And add some, saturating: an overdriven phosphor.
static inline uint16_t fxBoost(uint16_t sw, uint32_t k) {
  uint16_t c = fxSwap(sw);
  uint32_t r = (((c >> 11) & 31) * k) >> 8, g = (((c >> 5) & 63) * k) >> 8, b = ((c & 31) * k) >> 8;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return fxSwap((uint16_t)((r << 11) | (g << 5) | b));
}

// ── 2. Hash and noise ────────────────────────────────────────────────────────
// Everything that has to look accidental comes from here, never from a
// trigonometric pattern. Integer hash (lowbias32) for scatter; value noise with
// a smoothstep for anything that has to wander rather than flicker.
static inline uint32_t fxHash(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
static inline uint32_t fxHash2(uint32_t a, uint32_t b) { return fxHash(a * 0x9E3779B1U + fxHash(b)); }
static inline float fxU(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }   // [0,1)
static inline float fxS(uint32_t h) { return fxU(h) * 2.0f - 1.0f; }                      // [-1,1)
static inline float fxSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

static float fxNoise1(float x, uint32_t seed) {
  float   fl = floorf(x);
  int32_t i  = (int32_t)fl;
  float   a  = fxU(fxHash2((uint32_t)i, seed)), b = fxU(fxHash2((uint32_t)(i + 1), seed));
  return a + (b - a) * fxSmooth(x - fl);
}
static float fxNoise2(float x, float y, uint32_t seed) {
  float   fx = floorf(x), fy = floorf(y);
  int32_t ix = (int32_t)fx, iy = (int32_t)fy;
  float   tx = fxSmooth(x - fx), ty = fxSmooth(y - fy);
  uint32_t s0 = fxHash2((uint32_t)iy, seed), s1 = fxHash2((uint32_t)(iy + 1), seed);
  float a = fxU(fxHash2((uint32_t)ix, s0)), b = fxU(fxHash2((uint32_t)(ix + 1), s0));
  float c = fxU(fxHash2((uint32_t)ix, s1)), d = fxU(fxHash2((uint32_t)(ix + 1), s1));
  float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
  return ab + (cd - ab) * ty;
}
// A deterministic stream, for building a shape out of a seed.
struct FxRng {
  uint32_t s;
  uint32_t next()             { s = s * 1664525U + 1013904223U; return fxHash(s); }
  float    u()                { return fxU(next()); }
  float    sgn()              { return fxS(next()); }
  int      range(int a, int b) { return a + (int)(next() % (uint32_t)(b - a + 1)); }
};

// ── 3. Surface, spans and clipping ───────────────────────────────────────────
// Every primitive below draws horizontal spans into fxDst, and every span goes
// through the same three clips: up to two half-planes (a band's leading edge,
// the two halves of a blade cut), an optional per-column top/bottom (the
// ragged edge of a band), and an optional stipple that drops pixels on a
// grain hash (how a mark dissolves instead of blinking out).
static uint16_t*      fxDst  = nullptr;
static const int16_t* fxColT = nullptr;
static const int16_t* fxColB = nullptr;
static int            fxColY0 = 0, fxColY1 = 320;   // rows the column clip can pass at all
struct FxHalf { float a, b, c; };                // keep a*x + b*y + c >= 0
static FxHalf   fxHalf[2];
static uint8_t  fxHalfN    = 0;
static uint8_t  fxStip     = 0;
static uint32_t fxStipSeed = 0;

static inline bool fxHalfOk(int x, int y) {
  for (uint8_t i = 0; i < fxHalfN; i++)
    if (fxHalf[i].a * (x + 0.5f) + fxHalf[i].b * (y + 0.5f) + fxHalf[i].c < 0) return false;
  return true;
}
// The line through (x0,y0) heading (dx,dy); keep the side on its left (+1) or right (-1).
static void fxHalfSet(uint8_t i, float x0, float y0, float dx, float dy, float side) {
  fxHalf[i].a = -dy * side;
  fxHalf[i].b =  dx * side;
  fxHalf[i].c = -(fxHalf[i].a * x0 + fxHalf[i].b * y0);
}

static void fxSpan(int y, int x0, int x1, uint16_t col) {
  if ((unsigned)y >= (unsigned)FX_H) return;
  if (x0 < 0) x0 = 0;
  if (x1 > FX_W) x1 = FX_W;
  if (x0 >= x1) return;
  const float yc = y + 0.5f;
  for (uint8_t i = 0; i < fxHalfN; i++) {
    const FxHalf& h = fxHalf[i];
    float r = h.b * yc + h.c;
    if (h.a > 1e-6f)       { int lo = (int)ceilf(-r / h.a - 0.5f);       if (lo > x0) x0 = lo; }
    else if (h.a < -1e-6f) { int hi = (int)floorf(-r / h.a - 0.5f) + 1;  if (hi < x1) x1 = hi; }
    else if (r < 0) return;
    if (x0 >= x1) return;
  }
  uint16_t* row = fxDst + y * FX_W;
  if (!fxColT && !fxStip) { for (int x = x0; x < x1; x++) row[x] = col; return; }
  for (int x = x0; x < x1; x++) {
    if (fxColT && (y < fxColT[x] || y >= fxColB[x])) continue;
    if (fxStip && (fxHash2((uint32_t)(x * 7 + y * 1031), fxStipSeed) & 255) < fxStip) continue;
    row[x] = col;
  }
}
static inline void fxPut(int x, int y, uint16_t col) {
  if ((unsigned)x >= (unsigned)FX_W || (unsigned)y >= (unsigned)FX_H) return;
  if (!fxHalfN && !fxColT && !fxStip) { fxDst[y * FX_W + x] = col; return; }
  if (fxHalfN && !fxHalfOk(x, y)) return;
  if (fxColT && (y < fxColT[x] || y >= fxColB[x])) return;
  if (fxStip && (fxHash2((uint32_t)(x * 7 + y * 1031), fxStipSeed) & 255) < fxStip) return;
  fxDst[y * FX_W + x] = col;
}

// ── 4. Shapes ────────────────────────────────────────────────────────────────
static void fxTri(float ax, float ay, float bx, float by, float cx, float cy, uint16_t col) {
  float t;
  if (by < ay) { t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
  if (cy < ay) { t = ax; ax = cx; cx = t; t = ay; ay = cy; cy = t; }
  if (cy < by) { t = bx; bx = cx; cx = t; t = by; by = cy; cy = t; }
  int y0 = (int)ceilf(ay - 0.5f), y1 = (int)ceilf(cy - 0.5f);
  int lo = fxColT ? fxColY0 : 0, hi = fxColT ? fxColY1 : FX_H;
  if (y0 < lo) y0 = lo;
  if (y1 > hi) y1 = hi;
  if (y0 >= y1) return;
  // Edge slopes once per triangle; every row is then a multiply, not a divide.
  float sAC = (cx - ax) / (cy - ay);
  float sAB = (by > ay) ? (bx - ax) / (by - ay) : 0.0f;
  float sBC = (cy > by) ? (cx - bx) / (cy - by) : 0.0f;
  for (int y = y0; y < y1; y++) {
    float yc = y + 0.5f;
    float xl = ax + sAC * (yc - ay);
    float xr = (yc < by) ? ax + sAB * (yc - ay) : bx + sBC * (yc - by);
    if (xl > xr) { t = xl; xl = xr; xr = t; }
    fxSpan(y, (int)ceilf(xl - 0.5f), (int)ceilf(xr - 0.5f), col);
  }
}
// Convex polygon.
static void fxPoly(const float* px, const float* py, int n, uint16_t col) {
  float ymin = py[0], ymax = py[0];
  for (int i = 1; i < n; i++) { if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
  int y0 = fxMaxI(0, (int)ceilf(ymin - 0.5f)), y1 = fxMinI(FX_H, (int)ceilf(ymax - 0.5f));
  for (int y = y0; y < y1; y++) {
    float yc = y + 0.5f, xl = 1e9f, xr = -1e9f;
    for (int i = 0; i < n; i++) {
      int j = (i + 1 == n) ? 0 : i + 1;
      float ya = py[i], yb = py[j];
      if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
        float x = px[i] + (yc - ya) * (px[j] - px[i]) / (yb - ya);
        if (x < xl) xl = x;
        if (x > xr) xr = x;
      }
    }
    if (xl <= xr) fxSpan(y, (int)ceilf(xl - 0.5f), (int)ceilf(xr - 0.5f), col);
  }
}
static void fxLine(int x0, int y0, int x1, int y1, uint16_t col) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 1200; guard++) {
    fxPut(x0, y0, col);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
static void fxDisc(float cx, float cy, float r, uint16_t col) {
  if (r < 0.7f) { fxPut((int)floorf(cx), (int)floorf(cy), col); return; }
  int y0 = (int)ceilf(cy - r - 0.5f), y1 = (int)ceilf(cy + r - 0.5f);
  for (int y = y0; y < y1; y++) {
    float dy = y + 0.5f - cy, h = r * r - dy * dy;
    if (h < 0) continue;
    float w = sqrtf(h);
    fxSpan(y, (int)ceilf(cx - w - 0.5f), (int)ceilf(cx + w - 0.5f), col);
  }
}
// A brush stroke: thick where it starts, thinning to w1 where it lifts.
static void fxStroke(float x0, float y0, float x1, float y1, float w0, float w1, uint16_t col) {
  float dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy);
  if (w0 < 1.3f && w1 < 1.3f) { fxLine((int)x0, (int)y0, (int)x1, (int)y1, col); return; }
  if (L < 0.01f) { fxDisc(x0, y0, w0 * 0.5f, col); return; }
  float nx = -dy / L * 0.5f, ny = dx / L * 0.5f;
  float xs[4] = { x0 + nx * w0, x1 + nx * w1, x1 - nx * w1, x0 - nx * w0 };
  float ys[4] = { y0 + ny * w0, y1 + ny * w1, y1 - ny * w1, y0 - ny * w0 };
  fxPoly(xs, ys, 4, col);
}
static void fxRect(int x0, int y0, int x1, int y1, uint16_t col) {
  for (int y = y0; y < y1; y++) fxSpan(y, x0, x1, col);
}

// ── 5. Type ──────────────────────────────────────────────────────────────────
// Two of the screens' own faces, rendered straight from LovyanGFX's tables so
// the harness and the board draw the same pixels: the 5x7 GLCD hand at any
// integer scale (the terminal's voice) and the 16 px proportional Font2 (the
// caption boxes). The SFX lettering is section 6.
static int fxGlcd(const char* s, int x, int y, int sc, uint16_t col) {
  const uint8_t* T = fxGlcdTable();
  int x0 = x;
  for (; *s; s++) {
    const uint8_t* g = T + (uint8_t)*s * 5;
    for (int cx = 0; cx < 5; cx++) {
      uint8_t bits = g[cx];
      for (int ry = 0; ry < 8; ry++)
        if (bits & (1 << ry)) fxRect(x + cx * sc, y + ry * sc, x + cx * sc + sc, y + ry * sc + sc, col);
    }
    x += 6 * sc;
  }
  return x - x0;
}
static inline uint8_t fxF2C(char ch) { uint8_t c = (uint8_t)ch; return (c < 32 || c > 127) ? 32 : c; }
static int fxF2Width(const char* s) {
  int w = 0;
  for (; *s; s++) w += fxF2W(fxF2C(*s));
  return w;
}
static int fxF2(const char* s, int x, int y, uint16_t col) {
  int x0 = x;
  for (; *s; s++) {
    uint8_t c = fxF2C(*s);
    int w = fxF2W(c), bpr = (w + 6) >> 3;
    const uint8_t* d = fxF2Glyph(c);
    for (int r = 0; r < 16; r++)
      for (int j = 0; j < w - 1; j++)
        if (d[r * bpr + (j >> 3)] & (0x80 >> (j & 7))) fxPut(x + j, y + r, col);
    x += w;
  }
  return x - x0;
}
// Word-wrap into Font2 lines of at most `wpx`, upper-casing on the way: a
// caption box is hand-lettered, and comic lettering is capitals.
static const int FX_CAP_CH = 44;
static uint8_t fxWrap(const char* s, int wpx, char out[][FX_CAP_CH], uint8_t maxLines) {
  uint8_t n = 0;
  while (*s && n < maxLines) {
    while (*s == ' ') s++;
    if (!*s) break;
    int w = 0, take = 0;
    while (s[take] && take < FX_CAP_CH - 1) {
      int cw = fxF2W(fxF2C((char)toupper((unsigned char)s[take])));
      if (w + cw > wpx) break;
      w += cw;
      take++;
    }
    int cut = take;
    if (s[take]) { while (cut > 0 && s[cut] != ' ') cut--; if (cut == 0) cut = take ? take : 1; }
    for (int i = 0; i < cut; i++) out[n][i] = (char)toupper((unsigned char)s[i]);
    out[n][cut] = 0;
    n++;
    s += cut;
  }
  return n;
}

// ── 6. SFX lettering ─────────────────────────────────────────────────────────
// Comic sound effects are drawn, not typeset: fat letters that crowd until
// their outlines weld, a block extrude, a sheen falling into dots, every
// letter at its own tilt and baseline. The source is a bold oblique GFX face;
// each glyph becomes a signed distance field (two-pass chamfer), and the SDF
// is what gets styled -- outline, extrude and a ragged ink edge are all just
// thresholds on the same distance, and a ragged edge is that distance with
// value noise added. The result is baked into a small palette-index "sticker"
// per letter, which is what gets slammed about every frame.
struct FxInk { uint8_t top, bot, line, ext, extLine; };
static const FxInk FX_INK_HOT   = { FXP_WHITE, FXP_HOT,   FXP_INK,   FXP_EMBER, FXP_INK };
static const FxInk FX_INK_BLOOD = { FXP_GLOW,  FXP_BLOOD, FXP_INK,   FXP_SOOT,  FXP_INK };
static const FxInk FX_INK_COLD  = { FXP_GLOW,  FXP_OK,    FXP_INK,   FXP_RUST,  FXP_INK };

static const int FX_MAX_LETTERS = 14;
static const int FX_SDF_PAD     = 7;
static const int FX_SDF_MAX     = 72 * 72;
static const uint32_t FX_POOL   = 96 * 1024;

struct FxLetter {
  uint8_t* px;              // palette index + 1 per pixel; 0 = clear
  int16_t  w, h;
  float    pivX, pivY;      // glyph centre inside the sticker
  float    x, y;            // that centre relative to the word's, screen px
  float    rot;             // its own tilt at rest
  uint8_t  nDrip;
  float    dripX[2], dripY[2];
  uint8_t  dripLen[2];
};
struct FxWord { FxLetter L[FX_MAX_LETTERS]; uint8_t n; float w, h; };

static int16_t* fxSdfA = nullptr;   // FX_SDF_MAX each, PSRAM
static int16_t* fxSdfB = nullptr;
static int      fxSdfW = 0, fxSdfH = 0;
static uint8_t* fxPool = nullptr;   // FX_POOL, PSRAM
static uint32_t fxPoolUsed = 0;

static void fxChamfer(int16_t* d, int W, int H) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int i = y * W + x, v = d[i];
      if (x > 0 && d[i - 1] + 3 < v) v = d[i - 1] + 3;
      if (y > 0) {
        if (d[i - W] + 3 < v) v = d[i - W] + 3;
        if (x > 0     && d[i - W - 1] + 4 < v) v = d[i - W - 1] + 4;
        if (x < W - 1 && d[i - W + 1] + 4 < v) v = d[i - W + 1] + 4;
      }
      d[i] = (int16_t)v;
    }
  for (int y = H - 1; y >= 0; y--)
    for (int x = W - 1; x >= 0; x--) {
      int i = y * W + x, v = d[i];
      if (x < W - 1 && d[i + 1] + 3 < v) v = d[i + 1] + 3;
      if (y < H - 1) {
        if (d[i + W] + 3 < v) v = d[i + W] + 3;
        if (x < W - 1 && d[i + W + 1] + 4 < v) v = d[i + W + 1] + 4;
        if (x > 0     && d[i + W - 1] + 4 < v) v = d[i + W - 1] + 4;
      }
      d[i] = (int16_t)v;
    }
}

// Signed distance to the glyph edge, 1/16 px, positive outside, into fxSdfA.
static bool fxSdfBuild(const FxGfxFont& f, const FxGfxGlyph& g) {
  int W = g.width + 2 * FX_SDF_PAD, H = g.height + 2 * FX_SDF_PAD;
  if (!fxSdfA || W * H > FX_SDF_MAX) return false;
  const int16_t BIG = 30000;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int  gx = x - FX_SDF_PAD, gy = y - FX_SDF_PAD;
      bool in = false;
      if (gx >= 0 && gy >= 0 && gx < g.width && gy < g.height) {
        uint32_t bit = (uint32_t)gy * g.width + gx;
        in = (f.bitmap[g.bitmapOffset + (bit >> 3)] >> (7 - (bit & 7))) & 1;
      }
      fxSdfA[y * W + x] = in ? 0 : BIG;
      fxSdfB[y * W + x] = in ? BIG : 0;
    }
  fxChamfer(fxSdfA, W, H);
  fxChamfer(fxSdfB, W, H);
  for (int i = 0; i < W * H; i++) {
    float d = (fxSdfA[i] > 0) ? (fxSdfA[i] / 3.0f - 0.5f) : -(fxSdfB[i] / 3.0f - 0.5f);
    fxSdfA[i] = (int16_t)(d * 16.0f);
  }
  fxSdfW = W;
  fxSdfH = H;
  return true;
}
static inline float fxSdfCell(int x, int y) {
  if (x < 0 || y < 0 || x >= fxSdfW || y >= fxSdfH) return 8.0f;
  return fxSdfA[y * fxSdfW + x] * (1.0f / 16.0f);
}
static float fxSdfAt(float u, float v) {
  u -= 0.5f; v -= 0.5f;
  float fu = floorf(u), fv = floorf(v);
  int   i  = (int)fu, j = (int)fv;
  float tu = u - fu, tv = v - fv;
  float a = fxSdfCell(i, j), b = fxSdfCell(i + 1, j);
  float c = fxSdfCell(i, j + 1), d = fxSdfCell(i + 1, j + 1);
  return (a + (b - a) * tu) * (1 - tv) + (c + (d - c) * tu) * tv;
}

// Bake the glyph now in fxSdfA into a sticker at scale S. `ext` is the depth
// of the block extrude in screen px; it runs down and to the right, the way the
// light falls on every SFX in the title art.
static bool fxSticker(FxLetter& L, const FxGfxGlyph& g, float S, const FxInk& ink,
                      int ext, uint32_t seed) {
  const float OL = 2.4f + S * 0.6f;               // the ink outline, screen px
  const float EX = 0.60f, EY = 0.80f;             // extrude heading (unit)
  int w = (int)ceilf(fxSdfW * S) + ext + 1, h = (int)ceilf(fxSdfH * S) + ext + 1;
  if (!fxPool || fxPoolUsed + (uint32_t)(w * h) > FX_POOL) return false;
  L.px = fxPool + fxPoolUsed;
  fxPoolUsed += (uint32_t)(w * h);
  L.w = (int16_t)w;
  L.h = (int16_t)h;
  L.pivX = (FX_SDF_PAD + g.width  * 0.5f) * S;
  L.pivY = (FX_SDF_PAD + g.height * 0.5f) * S;
  const float inv = 1.0f / S, gTop = (float)FX_SDF_PAD, gH = (float)(g.height ? g.height : 1);
  for (int py = 0; py < h; py++) {
    for (int px = 0; px < w; px++) {
      float u = (px + 0.5f) * inv, v = (py + 0.5f) * inv;
      float rag = (fxNoise2(u * 0.33f, v * 0.33f, seed) - 0.5f) * 1.5f;
      float d = (fxSdfAt(u, v) + rag) * S;
      uint8_t idx = 0;
      if (d < 0) {
        // Sheen: solid white-hot across the top, falling into dots of the
        // second ink toward the foot -- printed shading, not a gradient.
        float t = (v - gTop) / gH;
        int tone = (int)((t - 0.28f) * 1.7f * 256.0f);
        idx = (uint8_t)(((int)FX_DOT[(py & 7) * 8 + (px & 7)] < tone ? ink.bot : ink.top) + 1);
      } else if (d < OL) {
        idx = (uint8_t)(ink.line + 1);
      } else if (d < OL + ext * 1.4f + 2.0f) {
        float m = 1e9f;
        for (int k = 1; k <= ext; k++) {
          float dk = (fxSdfAt(u - k * EX * inv, v - k * EY * inv) + rag) * S;
          if (dk < m) m = dk;
        }
        if (m < OL - 1.6f)  idx = (uint8_t)(ink.ext + 1);
        else if (m < OL)    idx = (uint8_t)(ink.extLine + 1);
      }
      L.px[py * w + px] = idx;
    }
  }
  return true;
}

// Where ink would run off this letter if it were wet: the lowest points of its
// strokes, two of them, far enough apart to read as separate runs.
static void fxFindDrips(FxLetter& L, const FxGfxGlyph& g, float S, uint32_t seed) {
  float bx[24], by[24];
  int   n = 0;
  for (int gx = 1; gx < g.width - 1 && n < 24; gx += 2)
    for (int gy = g.height - 1; gy >= g.height / 2; gy--)
      if (fxSdfAt(FX_SDF_PAD + gx + 0.5f, FX_SDF_PAD + gy + 0.5f) < -0.6f) {
        bx[n] = (float)gx; by[n] = (float)gy; n++;
        break;
      }
  L.nDrip = 0;
  for (int pick = 0; pick < 2 && n > 0; pick++) {
    int best = -1;
    float bestScore = -1e9f;
    for (int i = 0; i < n; i++) {
      bool nearOld = false;
      for (int j = 0; j < L.nDrip; j++)
        if (fabsf((FX_SDF_PAD + bx[i] + 0.5f) * S - L.dripX[j]) < 7.0f * S) nearOld = true;
      if (nearOld) continue;
      float score = by[i] + fxU(fxHash2((uint32_t)i, seed)) * 6.0f;
      if (score > bestScore) { bestScore = score; best = i; }
    }
    if (best < 0) break;
    L.dripX[L.nDrip]   = (FX_SDF_PAD + bx[best] + 0.5f) * S;
    L.dripY[L.nDrip]   = (FX_SDF_PAD + by[best] + 0.5f) * S;
    L.dripLen[L.nDrip] = (uint8_t)(7 + fxHash2((uint32_t)best, seed + 3) % 18);
    L.nDrip++;
  }
}

// Lay `s` out to fit a boxW x boxH box, centred on (0,0). `cresc` grows the
// letters along the word, the way a noise swells as it is read.
static bool fxBuildWord(FxWord& W, const char* s, float boxW, float boxH, float sizeMul,
                        const FxInk& ink, bool cresc, bool drips, uint32_t seed) {
  const FxGfxFont& f = fxSfxFont();
  W.n = 0;
  fxPoolUsed = 0;
  // Each letter's size relative to the word first -- the crescendo and a
  // little hand wobble -- so the fit below measures the word as it will
  // actually be drawn, not as the font would set it.
  float rel[FX_MAX_LETTERS];
  uint8_t cs[FX_MAX_LETTERS];
  int n = 0;
  FxRng R; R.s = seed;
  float adv = 0, relMax = 0;
  for (const char* p = s; *p && n < FX_MAX_LETTERS; p++, n++) {
    uint8_t c = (uint8_t)*p;
    if (c < f.first || c > f.last) c = ' ';
    cs[n] = c;
    rel[n] = 0.95f + 0.10f * R.u();
    if (relMax < rel[n]) relMax = rel[n];
  }
  if (n == 0) return false;
  if (cresc && n > 1)
    for (int i = 0; i < n; i++) { rel[i] *= 0.84f + 0.32f * (float)i / (float)(n - 1); if (relMax < rel[i]) relMax = rel[i]; }
  const float track = -3.5f;
  for (int i = 0; i < n; i++) adv += f.glyph[cs[i] - f.first].xAdvance * rel[i];
  const float capH  = (float)f.glyph['H' - f.first].height;
  float total = adv + track * (n - 1);
  // sizeMul lets a style ask for a bigger noise, but never wider than the box.
  float S = fminf(boxW / total, boxH / (capH * relMax) * sizeMul);
  S = fxClampF(S, 0.5f, 2.6f);
  W.w = total * S;
  W.h = capH * S;
  float cx = -W.w * 0.5f;
  for (int i = 0; i < n; i++) {
    uint8_t c = cs[i];
    const FxGfxGlyph& g = f.glyph[c - f.first];
    float ls = S * rel[i];
    float tilt = R.sgn() * 0.11f, bob = R.sgn() * 2.8f * S;
    if (c != ' ' && g.width && g.height && fxSdfBuild(f, g)) {
      FxLetter& L = W.L[W.n];
      if (fxSticker(L, g, ls, ink, (int)(3.0f + ls * 2.2f), seed + (uint32_t)i * 977U)) {
        // Letters stand on one baseline whatever their size, so a swelling
        // word grows upward, the way hand-lettered noise does.
        L.x = cx + (g.xOffset + g.width * 0.5f) * ls;
        L.y = W.h * 0.5f + (g.yOffset + g.height * 0.5f) * ls + bob;
        L.rot = tilt;
        L.nDrip = 0;
        if (drips) fxFindDrips(L, g, ls, seed + (uint32_t)i);
        W.n++;
      }
    }
    cx += g.xAdvance * ls + track * S;
  }
  return W.n > 0;
}

// One sticker, at its centre (cx,cy), scaled and turned. Nearest sampling on
// purpose: at rest the sticker is 1:1 and crisp, and a letter only ever
// grows while it is slamming in, when motion hides the stepping.
static void fxLetterDraw(const FxLetter& L, float cx, float cy, float scl, float rot) {
  float c = cosf(rot), s = sinf(rot), ic = c / scl, is = s / scl;
  float rad = 0.5f * sqrtf((float)(L.w * L.w + L.h * L.h)) * scl + 2.0f;
  int x0 = fxMaxI(0, (int)floorf(cx - rad)), x1 = fxMinI(FX_W, (int)ceilf(cx + rad));
  int y0 = fxMaxI(0, (int)floorf(cy - rad)), y1 = fxMinI(FX_H, (int)ceilf(cy + rad));
  for (int y = y0; y < y1; y++) {
    float dy = y + 0.5f - cy, dx = x0 + 0.5f - cx;
    float u = ic * dx + is * dy + L.pivX, v = -is * dx + ic * dy + L.pivY;
    for (int x = x0; x < x1; x++, u += ic, v -= is) {
      if (u < 0 || v < 0) continue;
      int iu = (int)u, iv = (int)v;
      if (iu >= L.w || iv >= L.h) continue;
      uint8_t idx = L.px[iv * L.w + iu];
      if (idx) fxPut(x, y, FX_PAL[idx - 1]);
    }
  }
}
// Runs of wet ink under a letter, `grow` 0-1 of their full length. They fall
// straight down whatever the letter's tilt, because gravity does.
static void fxDripDraw(const FxLetter& L, float cx, float cy, float scl, float rot, float grow) {
  float c = cosf(rot), s = sinf(rot);
  for (int i = 0; i < L.nDrip; i++) {
    float sx = L.dripX[i] - L.pivX, sy = L.dripY[i] - L.pivY;
    float X = cx + (c * sx - s * sy) * scl, Y = cy + (s * sx + c * sy) * scl;
    float len = L.dripLen[i] * grow * scl, w = 3.4f * scl;
    fxStroke(X, Y - 2, X, Y + len, w + 2.4f, w * 0.5f + 2.4f, FX_PAL[FXP_INK]);
    fxDisc(X, Y + len, w * 0.62f + 1.2f, FX_PAL[FXP_INK]);
    fxStroke(X, Y - 2, X, Y + len, w, w * 0.5f, FX_PAL[FXP_BLOOD]);
    fxDisc(X, Y + len, w * 0.62f, FX_PAL[FXP_BLOOD]);
  }
}

// ── 7. Ink ───────────────────────────────────────────────────────────────────
// Focus lines (the manga concentration lines): thin wedges pointing at a
// subject, each one a triangle whose apex stops at a ragged clear zone. Their
// angles, lengths and weights are all hashed, and a fifth of them are simply
// skipped, so the density has the rhythm of a hand rather than a protractor.
// `boil` re-deals the jitter; changing it every ~80 ms makes the lines crawl
// the way an animated cel's do.
static void fxFocusLines(float cx, float cy, float rx, float ry, int n, uint32_t seed,
                         uint32_t boil, uint16_t col, float wMax) {
  for (int k = 0; k < n; k++) {
    uint32_t h = fxHash2((uint32_t)k, seed), hb = fxHash2((uint32_t)k, seed ^ boil);
    if (fxU(h ^ 0x5bd1e995U) < 0.2f) continue;
    float a  = ((float)k + fxS(hb) * 0.45f) * FX_TAU / (float)n;
    float r0 = 0.72f + 0.55f * fxU(fxHash(h + 1)) + 0.12f * fxS(hb >> 3);
    float ca = cosf(a), sa = sinf(a);
    float ax = cx + ca * rx * r0, ay = cy + sa * ry * r0;
    float w  = 0.8f + wMax * fxU(fxHash(h + 2)) * fxU(fxHash(h + 3));
    float ox = cx + ca * 420.0f, oy = cy + sa * 420.0f;
    fxTri(ax, ay, ox - sa * w, oy + ca * w, ox + sa * w, oy - ca * w, col);
  }
}
// Speed lines running along a heading: lens-shaped streaks, thick in the
// middle, gone at both ends.
static void fxSpeedLines(float cx, float cy, float halfH, float dirX, float dirY, int n,
                         uint32_t seed, float scroll, uint16_t colA, uint16_t colB) {
  float nx = -dirY, ny = dirX;
  for (int k = 0; k < n; k++) {
    uint32_t h = fxHash2((uint32_t)k, seed);
    float off = fxS(h) * halfH;
    float len = 30.0f + 130.0f * fxU(fxHash(h + 1)) * fxU(fxHash(h + 5));
    float at  = fmodf(fxU(fxHash(h + 2)) * 520.0f - scroll * (0.6f + fxU(fxHash(h + 4))), 520.0f);
    if (at < 0) at += 520.0f;
    at -= 260.0f;
    float w = 0.9f + 2.6f * fxU(fxHash(h + 3)) * fxU(fxHash(h + 6));
    float x0 = cx + dirX * at + nx * off, y0 = cy + dirY * at + ny * off;
    float x1 = x0 + dirX * len, y1 = y0 + dirY * len, xm = (x0 + x1) * 0.5f, ym = (y0 + y1) * 0.5f;
    uint16_t col = (h & 7) == 0 ? colB : colA;
    fxTri(x0, y0, xm + nx * w * 0.5f, ym + ny * w * 0.5f, xm - nx * w * 0.5f, ym - ny * w * 0.5f, col);
    fxTri(x1, y1, xm + nx * w * 0.5f, ym + ny * w * 0.5f, xm - nx * w * 0.5f, ym - ny * w * 0.5f, col);
  }
}
// Kirby krackle: energy drawn as clots of black dots that follow a wandering
// walk, their sizes on a power law so a few are fat and most are specks.
static void fxKrackle(float cx, float cy, float spread, int clusters, uint32_t seed, uint16_t col) {
  FxRng R; R.s = seed;
  for (int c = 0; c < clusters; c++) {
    float x = cx + R.sgn() * spread, y = cy + R.sgn() * spread * 0.35f, a = R.u() * FX_TAU;
    int steps = R.range(7, 13);
    for (int i = 0; i < steps; i++) {
      float u = R.u(), r = 1.0f + u * u * u * 7.5f;
      if (R.u() < 0.25f) {
        fxDisc(x, y, r + 1.3f, col);
        fxDisc(x, y, r * 0.5f, FX_PAL[FXP_GLOW]);
      } else {
        fxDisc(x, y, r, col);
      }
      a += R.sgn() * 1.1f;
      float st = 4.0f + r * 1.6f + R.u() * 5.0f;
      x += cosf(a) * st;
      y += sinf(a) * st * 0.6f;
    }
  }
}
// A rising sun broken into rays of irregular width, radiating from under the
// panel -- the dawn chapter card.
static void fxSunburst(float cx, float cy, int n, uint32_t seed, float turn, uint16_t col) {
  float a = turn;
  for (int k = 0; k < n; k++) {
    uint32_t h = fxHash2((uint32_t)k, seed);
    float span = FX_TAU / n * (0.55f + 0.9f * fxU(h));
    float w    = span * (0.35f + 0.25f * fxU(fxHash(h + 1)));
    float r = 420.0f;
    fxTri(cx, cy, cx + cosf(a) * r, cy + sinf(a) * r, cx + cosf(a + w) * r, cy + sinf(a + w) * r, col);
    a += span;
  }
}
// Rain in streaks, slanted, falling at a few speeds.
static void fxRain(uint32_t now, int n, uint32_t seed, float shx, float shy) {
  for (int i = 0; i < n; i++) {
    uint32_t h = fxHash2((uint32_t)i, seed);
    float spd = 0.30f + 0.35f * fxU(fxHash(h + 1));
    float len = 7.0f + 18.0f * fxU(fxHash(h + 2));
    float y = fmodf(fxU(h) * 380.0f + now * spd, 380.0f) - 30.0f;
    float x = fxU(fxHash(h + 3)) * 300.0f - 30.0f - y * 0.28f;
    uint8_t c = (h & 3) == 0 ? FXP_HDR : ((h & 3) == 1 ? FXP_OK : FXP_RUST);
    fxLine((int)(x + shx), (int)(y + shy), (int)(x + shx - len * 0.28f), (int)(y + shy + len), FX_PAL[c]);
  }
}

// ── 8. Set pieces ────────────────────────────────────────────────────────────
// Cracks: the glass (or the ground -- the terminal cannot tell) splits from an
// impact point. Arms walk outward with a jittered heading, thinning as they
// go, forking now and then; they are grown from the centre in a quarter of a
// second and dissolve on a grain hash rather than fading, so they go the way
// a crack does in a flipbook.
struct FxSeg { int16_t x0, y0, x1, y1; uint8_t w10; uint16_t d; };
static const int FX_CRK_MAX = 110;
static FxSeg*   fxCrk   = nullptr;          // FX_CRK_MAX, PSRAM
static uint8_t  fxCrkN  = 0;
static uint16_t fxCrkLen = 0;
static uint32_t fxCrkT0 = 0;
static bool     fxCrkOn = false;
static float    fxCrkX = 0, fxCrkY = 0;

static void fxCracksAdd(float x0, float y0, float x1, float y1, float w, uint16_t d) {
  if (fxCrkN >= FX_CRK_MAX) return;
  FxSeg s = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (uint8_t)(w * 10.0f), d };
  fxCrk[fxCrkN++] = s;
}
static void fxCracksGen(float ox, float oy, uint32_t seed) {
  if (!fxCrk) return;
  struct Br { float x, y, a, w, left; uint16_t d; };
  Br st[24];
  float armA[10];
  int sp = 0;
  FxRng R; R.s = seed;
  fxCrkN = 0; fxCrkLen = 0; fxCrkX = ox; fxCrkY = oy;
  int arms = R.range(6, 9);
  float a0 = R.u() * FX_TAU;
  for (int i = 0; i < arms && sp < 24; i++) {
    armA[i] = a0 + ((float)i + R.sgn() * 0.3f) * FX_TAU / arms;
    Br b = { ox, oy, armA[i], 2.8f + R.u() * 1.4f, 80.0f + R.u() * 150.0f, 0 };
    st[sp++] = b;
  }
  // Two rings of short cracks bridging neighbouring arms: the spiderweb that
  // says "glass" rather than "ground".
  for (int ring = 0; ring < 2; ring++) {
    float r = ring ? 40.0f + R.u() * 16.0f : 16.0f + R.u() * 8.0f;
    for (int i = 0; i < arms; i++) {
      if (R.u() < 0.22f) continue;
      float aa = armA[i], ab = armA[(i + 1) % arms] + (i + 1 == arms ? FX_TAU : 0.0f);
      float r0 = r * (0.85f + 0.3f * R.u()), r1 = r * (0.85f + 0.3f * R.u());
      float xa = ox + cosf(aa) * r0, ya = oy + sinf(aa) * r0;
      float xb = ox + cosf(ab) * r1, yb = oy + sinf(ab) * r1;
      float am = (aa + ab) * 0.5f, rm = r * (1.05f + 0.15f * R.u());
      float xm = ox + cosf(am) * rm, ym = oy + sinf(am) * rm;
      fxCracksAdd(xa, ya, xm, ym, 1.4f, (uint16_t)r);
      fxCracksAdd(xm, ym, xb, yb, 1.2f, (uint16_t)r);
    }
  }
  while (sp > 0 && fxCrkN < FX_CRK_MAX) {
    Br b = st[--sp];
    while (b.left > 0 && fxCrkN < FX_CRK_MAX) {
      float step = 5.0f + R.u() * 7.0f;
      b.a += R.sgn() * 0.22f;
      float nx = b.x + cosf(b.a) * step, ny = b.y + sinf(b.a) * step;
      fxCracksAdd(b.x, b.y, nx, ny, b.w, b.d);
      b.d = (uint16_t)(b.d + step);
      b.x = nx; b.y = ny; b.left -= step;
      b.w = fmaxf(1.0f, b.w * 0.93f);
      if (R.u() < 0.12f && sp < 24) {
        Br f = { b.x, b.y, b.a + R.sgn() * 0.65f, b.w * 0.7f, b.left * 0.5f, b.d };
        st[sp++] = f;
      }
      if (b.x < -10 || b.x > FX_W + 10 || b.y < -10 || b.y > FX_H + 10) break;
    }
    if (b.d > fxCrkLen) fxCrkLen = b.d;
  }
}
static void fxCracksDraw(uint32_t now, float shx, float shy) {
  uint32_t t = now - fxCrkT0;
  if (t > 3000) { fxCrkOn = false; return; }
  float grow = (t < 240) ? fxCubicOut(t / 240.0f) * fxCrkLen : (float)fxCrkLen;
  if (t > 1900) { fxStip = (uint8_t)fxMinI(255, (int)((t - 1900) * 255 / 1100)); fxStipSeed = fxCrkT0; }
  // A fracture on a screen is bright -- light catching the break -- with the
  // dark of the split either side of it. Halo pass first, then the cores.
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < fxCrkN; i++) {
      const FxSeg& s = fxCrk[i];
      if (s.d > grow) continue;
      float w = s.w10 / 10.0f;
      float x0 = s.x0 + shx, y0 = s.y0 + shy, x1 = s.x1 + shx, y1 = s.y1 + shy;
      if (pass == 0) fxStroke(x0, y0, x1, y1, w + 2.2f, w * 0.93f + 2.2f, FX_PAL[FXP_INK]);
      else if (w > 2.2f) fxStroke(x0, y0, x1, y1, w * 0.5f, w * 0.46f, FX_PAL[FXP_WHITE]);
      else fxLine((int)x0, (int)y0, (int)x1, (int)y1, FX_PAL[(i & 3) ? FXP_GLOW : FXP_WHITE]);
    }
  // The impact itself, a burst of chips round the point.
  FxRng R; R.s = fxCrkT0;
  for (int k = 0; k < 9; k++) {
    float a = R.u() * FX_TAU, r0 = 2.0f + R.u() * 3.0f, r1 = 7.0f + R.u() * 9.0f;
    fxStroke(fxCrkX + shx + cosf(a) * r0, fxCrkY + shy + sinf(a) * r0,
             fxCrkX + shx + cosf(a) * r1, fxCrkY + shy + sinf(a) * r1, 3.0f, 0.8f, FX_PAL[FXP_WHITE]);
  }
  fxDisc(fxCrkX + shx, fxCrkY + shy, 3.5f, FX_PAL[FXP_INK]);
  fxStip = 0;
}

// Lightning: midpoint displacement, five subdivisions, a couple of forks.
// Regrown from a new seed on every flicker, so no two strikes of one bolt are
// the same shape -- which is how lightning actually reads on film.
static const int FX_BOLT_MAX = 72;
static int16_t  fxBoltX[FX_BOLT_MAX], fxBoltY[FX_BOLT_MAX];
static uint8_t  fxBoltLen[4], fxBoltPaths = 0;
static uint32_t fxBoltT0 = 0;
static bool     fxBoltOn = false;
static float    fxBoltSx, fxBoltSy, fxBoltEx, fxBoltEy;

static int fxBoltPath(int at, float x0, float y0, float x1, float y1, int levels, float rough, FxRng& R) {
  float xs[33], ys[33];
  int n = 2;
  xs[0] = x0; ys[0] = y0; xs[1] = x1; ys[1] = y1;
  for (int l = 0; l < levels && n < 33; l++) {
    float tx[33], ty[33];
    int m = 0;
    for (int i = 0; i + 1 < n && m + 2 < 33; i++) {
      float dx = xs[i + 1] - xs[i], dy = ys[i + 1] - ys[i], L = sqrtf(dx * dx + dy * dy);
      tx[m] = xs[i]; ty[m] = ys[i]; m++;
      float off = R.sgn() * L * rough;
      tx[m] = (xs[i] + xs[i + 1]) * 0.5f - dy / (L + 0.001f) * off;
      ty[m] = (ys[i] + ys[i + 1]) * 0.5f + dx / (L + 0.001f) * off;
      m++;
    }
    tx[m] = xs[n - 1]; ty[m] = ys[n - 1]; m++;
    for (int i = 0; i < m; i++) { xs[i] = tx[i]; ys[i] = ty[i]; }
    n = m;
  }
  int k = 0;
  for (int i = 0; i < n && at + k < FX_BOLT_MAX; i++, k++) {
    fxBoltX[at + k] = (int16_t)xs[i];
    fxBoltY[at + k] = (int16_t)ys[i];
  }
  return k;
}
static void fxBoltGen(uint32_t seed) {
  FxRng R; R.s = seed;
  int at = 0;
  fxBoltPaths = 0;
  int n = fxBoltPath(at, fxBoltSx, fxBoltSy, fxBoltEx, fxBoltEy, 5, 0.22f, R);
  fxBoltLen[fxBoltPaths++] = (uint8_t)n;
  int mainAt = at;
  at += n;
  for (int b = 0; b < 2 && at < FX_BOLT_MAX - 10; b++) {
    int i = R.range(n / 5, n * 3 / 5);
    float bx = fxBoltX[mainAt + i], by = fxBoltY[mainAt + i];
    float ex = bx + R.sgn() * 60.0f, ey = by + 30.0f + R.u() * 60.0f;
    int m = fxBoltPath(at, bx, by, ex, ey, 3, 0.28f, R);
    fxBoltLen[fxBoltPaths++] = (uint8_t)m;
    at += m;
  }
}
static void fxBoltDraw(float shx, float shy) {
  int at = 0;
  for (int p = 0; p < fxBoltPaths; p++) {
    float wm = (p == 0) ? 1.0f : 0.55f;
    for (int pass = 0; pass < 3; pass++) {
      float    w   = (pass == 0 ? 6.0f : pass == 1 ? 3.0f : 1.2f) * wm;
      uint16_t col = FX_PAL[pass == 0 ? FXP_FLAME : pass == 1 ? FXP_GLOW : FXP_WHITE];
      for (int i = 0; i + 1 < fxBoltLen[p]; i++) {
        float taper = 1.0f - 0.6f * (float)i / fxBoltLen[p];
        fxStroke(fxBoltX[at + i] + shx, fxBoltY[at + i] + shy, fxBoltX[at + i + 1] + shx,
                 fxBoltY[at + i + 1] + shy, w * taper, w * taper, col);
      }
    }
    at += fxBoltLen[p];
  }
}

// Fire, the terminal's way: the old demo-scene flame (heat rises a cell,
// loses a random amount and drifts a column) on a 40x16 grid of 6x8 cells, set
// in the same density glyphs the death screen's ASCII plume burns in.
static const int FX_FIRE_C = 40, FX_FIRE_R = 20;
static uint8_t  (*fxHeat)[FX_FIRE_C] = nullptr;   // [FX_FIRE_R], PSRAM
static uint32_t fxFireUntil = 0, fxFireLast = 0, fxFireSeed = 0;
static bool     fxFireOn = false;

static void fxFireStep(uint32_t now) {
  bool feed = (int32_t)(fxFireUntil - now) > 0;
  FxRng R; R.s = fxFireSeed ^ (now / 50);
  for (int c = 0; c < FX_FIRE_C; c++) {
    uint8_t& b = fxHeat[FX_FIRE_R - 1][c];
    b = feed ? (uint8_t)(170 + R.range(0, 85)) : (uint8_t)(b * 5 / 8);
  }
  for (int r = 0; r < FX_FIRE_R - 1; r++)
    for (int c = 0; c < FX_FIRE_C; c++) {
      int sc = c + R.range(-1, 1);
      sc = sc < 0 ? 0 : (sc >= FX_FIRE_C ? FX_FIRE_C - 1 : sc);
      int v = fxHeat[r + 1][sc] - R.range(0, 30);
      fxHeat[r][c] = (uint8_t)(v < 0 ? 0 : v);
    }
  bool any = false;
  for (int c = 0; c < FX_FIRE_C && !any; c++) any = fxHeat[FX_FIRE_R - 2][c] > 8;
  if (!feed && !any) fxFireOn = false;
}
static void fxFireDraw(float shx, float shy) {
  static const char RAMP[] = ".,:;+*%#@";
  const int y0 = FX_H - FX_FIRE_R * 8;
  for (int r = 0; r < FX_FIRE_R; r++)
    for (int c = 0; c < FX_FIRE_C; c++) {
      int h = fxHeat[r][c];
      if (h < 24) continue;
      int x = c * 6 + (int)shx, y = y0 + r * 8 + (int)shy;
      if (h > 70) fxRect(x, y, x + 6, y + 8, FX_PAL[h > 150 ? FXP_EMBER : FXP_SOOT]);
      char g[2] = { RAMP[fxMinI(8, (h - 24) / 26)], 0 };
      fxGlcd(g, x, y, 1, FX_PAL[fxMinI(FXP_WHITE, FXP_DIM + h * 12 / 256)]);
    }
}

// Splatter: an inkblot and its spray. The blot is a star-shaped polygon whose
// radius is value noise around the rim; the spray flies out along a few rays
// with sizes on a power law, the bigger drops dragging a tail.
struct FxBlob { int16_t x, y; uint8_t r; uint8_t kind; uint32_t seed; };
static const int FX_SPLAT_MAX = 26;
static FxBlob   fxSplat[FX_SPLAT_MAX];
static uint8_t  fxSplatN = 0;
static uint32_t fxSplatT0 = 0;
static bool     fxSplatOn = false;

static void fxSplatGen(uint32_t seed) {
  FxRng R; R.s = seed;
  fxSplatN = 0;
  int blots = R.range(1, 2);
  for (int b = 0; b < blots; b++) {
    float cx = 40 + R.u() * 160, cy = 60 + R.u() * 200, r = 12 + R.u() * 12;
    FxBlob B = { (int16_t)cx, (int16_t)cy, (uint8_t)r, 0, R.next() };
    fxSplat[fxSplatN++] = B;
    int rays = R.range(4, 7);
    for (int k = 0; k < rays && fxSplatN < FX_SPLAT_MAX; k++) {
      float a = R.u() * FX_TAU;
      int drops = R.range(2, 4);
      for (int d = 0; d < drops && fxSplatN < FX_SPLAT_MAX; d++) {
        float dist = r * (1.3f + d * 0.7f + R.u() * 0.6f), u = R.u();
        FxBlob D = { (int16_t)(cx + cosf(a) * dist), (int16_t)(cy + sinf(a) * dist),
                     (uint8_t)(1 + u * u * r * 0.4f), (uint8_t)(d == 0 ? 2 : 1), (uint32_t)(a * 1000) };
        fxSplat[fxSplatN++] = D;
      }
    }
  }
}
static void fxSplatDraw(uint32_t now, float shx, float shy) {
  uint32_t t = now - fxSplatT0;
  if (t > 2600) { fxSplatOn = false; return; }
  float grow = t < 90 ? fxCubicOut(t / 90.0f) : 1.0f;
  if (t > 1500) { fxStip = (uint8_t)fxMinI(255, (int)((t - 1500) * 255 / 1100)); fxStipSeed = fxSplatT0 + 7; }
  uint16_t ink = FX_PAL[FXP_INK], blood = FX_PAL[FXP_BLOOD];
  for (int i = 0; i < fxSplatN; i++) {
    const FxBlob& B = fxSplat[i];
    float x = B.x + shx, y = B.y + shy, r = B.r * grow;
    if (B.kind == 0) {
      float px[25], py[25];
      for (int k = 0; k < 24; k++) {
        float a = k * FX_TAU / 24.0f;
        float rr = r * (0.72f + 0.55f * fxNoise1(k * 0.9f, B.seed));
        px[k] = x + cosf(a) * rr; py[k] = y + sinf(a) * rr;
      }
      for (int k = 0; k < 24; k++) {
        int j = (k + 1) % 24;
        fxTri(x, y, px[k] + (px[k] - x) * 0.12f, py[k] + (py[k] - y) * 0.12f,
              px[j] + (px[j] - x) * 0.12f, py[j] + (py[j] - y) * 0.12f, ink);
      }
      for (int k = 0; k < 24; k++) {
        int j = (k + 1) % 24;
        fxTri(x, y, px[k], py[k], px[j], py[j], blood);
      }
    } else {
      fxDisc(x, y, r + 1.0f, ink);
      fxDisc(x, y, r, blood);
      if (B.kind == 2) {
        float a = B.seed / 1000.0f;
        fxStroke(x, y, x - cosf(a) * r * 3.5f, y - sinf(a) * r * 3.5f, r * 1.6f, 0.6f, blood);
      }
    }
  }
  fxStip = 0;
}

// The eyes. Two almonds whose lids are noise-bent curves, pinched at the
// inner corners (it is angry), a vertical slit for a pupil, a heavy brow and
// crow's-feet at the outer corners -- the eyes a villain gets in the one panel
// before the reveal. `open` 0-1 lifts the lids; `look` slides the pupils.
static void fxEye(float cx, float cy, float w, float h, float open, float look, float dir,
                  uint32_t seed, uint8_t fillA, uint8_t fillB, uint8_t brow) {
  float hw = w * 0.5f;
  int x0 = (int)floorf(cx - hw - 3), x1 = (int)ceilf(cx + hw + 3);
  if (open < 0.08f) {
    fxStroke(cx - hw, cy + h * 0.05f, cx + hw, cy + h * 0.25f * dir, 2.8f, 1.2f, FX_PAL[brow]);
  } else {
    for (int pass = 0; pass < 2; pass++) {
      float grow = pass == 0 ? 2.2f : 0.0f;
      for (int x = x0; x < x1; x++) {
        float t = (x + 0.5f - cx) / (hw + grow);
        if (t <= -1.0f || t >= 1.0f) continue;
        float tt = t * dir, q = 1.0f - t * t;
        float wob = 1.0f + 0.10f * (fxNoise1(x * 0.21f, seed) - 0.5f);
        float yu = cy - h * open * powf(q, 0.62f) * wob + tt * h * 0.42f - grow;
        float yl = cy + h * open * 0.52f * powf(q, 0.95f) + tt * h * 0.18f + grow;
        for (int y = (int)ceilf(yu - 0.5f); y < (int)ceilf(yl - 0.5f); y++) {
          uint16_t col;
          if (pass == 0) col = FX_PAL[FXP_INK];
          else {
            float r = fabsf(t) + fabsf(y + 0.5f - cy) / (h + 0.01f) * 0.6f;
            int tone = (int)(r * 300.0f) - 60;
            col = FX_PAL[(int)FX_DOT[(y & 7) * 8 + (x & 7)] < tone ? fillB : fillA];
          }
          fxPut(x, y, col);
        }
      }
    }
    // Slit pupil, and one hard highlight: it is looking at you.
    float px = cx + look * hw * 0.35f, pr = h * open * 0.78f;
    for (int y = (int)(cy - pr); y < (int)(cy + pr); y++) {
      float q = 1.0f - ((y + 0.5f - cy) / pr) * ((y + 0.5f - cy) / pr);
      if (q <= 0) continue;
      float pw = fmaxf(0.8f, w * 0.035f * sqrtf(q));
      fxSpan(y, (int)(px - pw), (int)(px + pw + 1), FX_PAL[FXP_INK]);
    }
    fxDisc(px - w * 0.09f, cy - pr * 0.45f, fmaxf(1.0f, w * 0.03f), FX_PAL[FXP_WHITE]);
  }
  // The brow comes down toward the nose; the creases fan off the outer corner.
  fxStroke(cx - hw * 1.05f * dir, cy - h * 1.35f, cx + hw * 0.85f * dir, cy - h * 0.55f,
           2.0f, 1.0f + w * 0.09f, FX_PAL[brow]);
  FxRng R; R.s = seed;
  for (int k = 0; k < 3; k++) {
    float a = (-0.35f + k * 0.35f + R.sgn() * 0.1f), sx = cx - hw * 1.08f * dir;
    fxStroke(sx, cy + k * 2.0f - 2.0f, sx - cosf(a) * w * 0.28f * dir, cy + sinf(a) * w * 0.28f,
             1.6f, 0.6f, FX_PAL[brow]);
  }
}
static void fxEyePair(float cx, float cy, float w, float h, float gap, float open, float look,
                      uint32_t seed, uint8_t fillA, uint8_t fillB, uint8_t brow) {
  fxEye(cx - gap * 0.5f - w * 0.5f, cy, w, h, open, look,  1.0f, seed,     fillA, fillB, brow);
  fxEye(cx + gap * 0.5f + w * 0.5f, cy, w, h, open, look, -1.0f, seed + 9, fillA, fillB, brow);
}

// The nest's cast. Section 12 stages them; these only draw. All of it is on
// the glass rather than in the picture -- black ink, with the phosphor behind
// catching its edges -- and every creature is a rig, not a sprite: a spider
// stands on eight jointed legs that plant and step, and a fly flies a path it
// only decided a moment ago.

// A jointed leg, hip to foot. The knee comes out of the law of cosines, bent
// to whichever side (bx,by) points -- the way a leg arches out from its body --
// and the shin bows the same way at a second joint, so a leg is an arch, not
// a bent stick. A foot out of reach just gets the leg straight at it.
struct FxLegPose { float hx, hy, kx, ky, mx, my, fx, fy; };
static FxLegPose fxLegSolve(float hx, float hy, float fx, float fy, float a, float b,
                            float bx, float by) {
  FxLegPose P = { hx, hy, hx, hy, fx, fy, fx, fy };
  float dx = fx - hx, dy = fy - hy, d = sqrtf(dx * dx + dy * dy);
  if (d < 0.01f) return P;
  float ux = dx / d, uy = dy / d, nx = -uy, ny = ux;
  if (nx * bx + ny * by < 0) { nx = -nx; ny = -ny; }
  float bow = 0.0f;
  if (d >= a + b) {
    P.kx = hx + ux * a;       P.ky = hy + uy * a;
    P.fx = hx + ux * (a + b); P.fy = hy + uy * (a + b);
  } else {
    float along = (a * a - b * b + d * d) / (2.0f * d);
    float h = sqrtf(fmaxf(0.0f, a * a - along * along));
    P.kx = hx + ux * along + nx * h;
    P.ky = hy + uy * along + ny * h;
    bow = 0.07f;
  }
  float sx = P.fx - P.kx, sy = P.fy - P.ky, sl = sqrtf(sx * sx + sy * sy);
  P.mx = P.kx + sx * 0.55f + nx * sl * bow;
  P.my = P.ky + sy * 0.55f + ny * sl * bow;
  return P;
}
// Three passes, which a creature runs over every leg and then its body before
// the next: 0 the edges, 1 the ink, 2 the shine. A thick leg is black chitin --
// a faint edge all round, a lit one on the side the light comes from (above,
// to the left, as on every SFX), and a hard specular stripe down each segment
// -- because black with an edge on both sides reads, on a dark screen, as a
// hollow tube. A thin one is too fine to be black at all: all there is of it
// is where the light catches it. `hair` (a length; 0 for none) sets bristles
// on the edge pass, raked toward the foot, their roots under the ink.
static void fxLegDraw(const FxLegPose& P, float w, int pass, uint8_t rim, float hair, uint32_t seed) {
  const float xs[4] = { P.hx, P.kx, P.mx, P.fx }, ys[4] = { P.hy, P.ky, P.my, P.fy };
  const float ws[4] = { w, w * 0.8f, w * 0.55f, fmaxf(0.7f, w * 0.25f) };
  if (w < 2.0f) {
    if (pass == 1)
      for (int i = 0; i < 3; i++) fxStroke(xs[i], ys[i], xs[i + 1], ys[i + 1], ws[i], ws[i + 1], FX_PAL[rim]);
    return;
  }
  for (int i = 0; i < 3; i++) {
    float dx = xs[i + 1] - xs[i], dy = ys[i + 1] - ys[i], L = sqrtf(dx * dx + dy * dy);
    if (pass == 0) {
      fxStroke(xs[i], ys[i], xs[i + 1], ys[i + 1], ws[i] + 1.6f, ws[i + 1] + 1.6f, FX_PAL[rim - 2]);
      fxStroke(xs[i] - 1.1f, ys[i] - 1.1f, xs[i + 1] - 1.1f, ys[i + 1] - 1.1f, ws[i] + 0.4f, ws[i + 1] + 0.4f,
               FX_PAL[rim]);
      if (hair <= 0.0f || L < 5.0f) continue;
      float ux = dx / L, uy = dy / L;
      int n = (int)(L / 4.0f);
      for (int k = 1; k < n; k++) {
        uint32_t h = fxHash2((uint32_t)(i * 64 + k), seed);
        float at   = ((float)k + fxS(h) * 0.35f) / (float)n;
        float half = fxLerp(ws[i], ws[i + 1], at) * 0.5f;
        float sd   = (k & 1) ? 1.0f : -1.0f;
        float px = xs[i] + dx * at - uy * half * sd, py = ys[i] + dy * at + ux * half * sd;
        float len = hair * (0.5f + fxU(fxHash(h + 1)));
        float rx = ux * 0.6f - uy * 0.8f * sd, ry = uy * 0.6f + ux * 0.8f * sd;
        fxLine((int)px, (int)py, (int)(px + rx * len), (int)(py + ry * len), FX_PAL[(h & 3) ? rim - 1 : rim + 2]);
      }
    } else if (pass == 1) {
      fxStroke(xs[i], ys[i], xs[i + 1], ys[i + 1], ws[i], ws[i + 1], FX_PAL[FXP_INK]);
      if (i < 2) fxDisc(xs[i + 1], ys[i + 1], ws[i + 1] * 0.5f, FX_PAL[FXP_INK]);
    } else if (w >= 3.0f && L >= 4.0f) {
      float nx = -dy / L, ny = dx / L;
      if (nx + ny > 0) { nx = -nx; ny = -ny; }              // toward the light
      float o = (ws[i] + ws[i + 1]) * 0.22f;
      fxLine((int)(xs[i] + dx * 0.2f + nx * o), (int)(ys[i] + dy * 0.2f + ny * o),
             (int)(xs[i] + dx * 0.75f + nx * o), (int)(ys[i] + dy * 0.75f + ny * o), FX_PAL[rim + 3]);
    }
  }
}
// A body part: an ellipse whose rim wanders a little, turned to (c,s).
static void fxBlob(float cx, float cy, float c, float s, float ra, float rb, uint32_t seed, uint16_t col) {
  float px[14], py[14];
  for (int k = 0; k < 14; k++) {
    float a = k * (FX_TAU / 14.0f), wob = 1.0f + 0.12f * (fxNoise1(k * 0.9f, seed) - 0.5f);
    float ex = cosf(a) * ra * wob, ey = sinf(a) * rb * wob;
    px[k] = cx + c * ex - s * ey;
    py[k] = cy + s * ex + c * ey;
  }
  fxPoly(px, py, 14, col);
}
// The same passes for a body part: pass 0 its edges, pass 1 its ink.
static void fxBlobLit(float cx, float cy, float c, float s, float ra, float rb, uint32_t seed, int pass,
                      uint8_t rim) {
  if (pass == 0) {
    fxBlob(cx, cy, c, s, ra + 1.2f, rb + 1.2f, seed, FX_PAL[rim - 2]);
    fxBlob(cx - 1.1f, cy - 1.1f, c, s, ra + 0.3f, rb + 0.3f, seed, FX_PAL[rim]);
  } else if (pass == 1) {
    fxBlob(cx, cy, c, s, ra, rb, seed, FX_PAL[FXP_INK]);
  }
}

// A spider from above, in its own frame: x forward, y to its right, `sc` px
// to the unit. It walks a tetrapod gait clocked by the distance it has
// covered -- L1 R2 L3 R4 swing while the other four stand -- so its feet stay
// planted on the glass while the body passes over them, and stop when it does.
struct FxSpider {
  bool     on;
  float    x, y, head;   // body centre; heading, 0 faces +x
  float    sc;           // 1 = a spider the size of a thumbnail
  float    span;         // leg length against the body; 0 is 1
  float    walk;         // px walked: the gait's clock
  float    curl;         // 0-1, legs drawn in (dropping on a thread)
  float    reach;        // 0-1, forelegs up and out at whatever is in front
  bool     fangs;
  uint32_t seed;
};
static const float FX_SP_HIP[4][2]  = { { 3.6f, 1.4f }, { 2.6f, 2.1f }, { 1.4f, 2.4f }, { 0.2f, 2.0f } };
static const float FX_SP_FOOT[4][2] = { { 14.0f, 8.0f }, { 6.5f, 13.5f }, { -3.5f, 13.8f }, { -11.5f, 9.5f } };

static void fxSpiderLegs(const FxSpider& S, uint32_t now, FxLegPose P[8]) {
  const float c = cosf(S.head), s = sinf(S.head), k = S.sc, sp = S.span > 0 ? S.span : 1.0f;
  const float L = 14.0f * sp, SW = 0.36f, A = (1.0f - SW) * L * 0.5f;    // the stride, body units
  for (int i = 0; i < 8; i++) {
    const int   j  = i >> 1;
    const float sd = (i & 1) ? 1.0f : -1.0f;
    const float hx = FX_SP_HIP[j][0], hy = FX_SP_HIP[j][1] * sd;
    float fx = hx + (FX_SP_FOOT[j][0] - hx) * sp, fy = hy + (FX_SP_FOOT[j][1] * sd - hy) * sp;
    const float cx = fx - hx, cy = fy - hy, chord = sqrtf(cx * cx + cy * cy);
    // Two sets of four, half a stride apart, each leg a hair off its set.
    float u = S.walk / (L * k) + (float)((j + (i & 1)) & 1) * 0.5f + fxU(fxHash2((uint32_t)i, S.seed)) * 0.06f;
    u -= floorf(u);
    float lift = 0.0f;
    if (u < SW) { float p = u / SW; fx += -A + 2.0f * A * fxSmooth(p); lift = 4.0f * p * (1.0f - p); }
    else        fx += A - 2.0f * A * (u - SW) / (1.0f - SW);
    // A leg with nothing to do lifts now and then, feels forward, sets down.
    float tw = fxClampF((fxNoise1(now * 0.0055f + i * 3.7f, S.seed) - 0.7f) * 5.0f, 0.0f, 1.0f);
    if (tw > lift) lift = tw;
    fx += cx / chord * tw * 2.5f;
    fy += cy / chord * tw * 2.5f;
    // The forelegs come up and forward, spread for what is in front of it.
    if (j == 0) { fx += S.reach * 7.0f * sp; fy += sd * S.reach * 2.5f; lift = fmaxf(lift, S.reach * 0.5f); }
    if (j == 1) fx += S.reach * 3.0f * sp;
    // Drawn in, the feet fold up under the body.
    fx = fxLerp(fx, hx + cx * 0.3f + 1.0f, S.curl);
    fy = fxLerp(fy, hy + cy * 0.3f, S.curl);
    // A lifted foot reads shorter from above.
    fx = hx + (fx - hx) * (1.0f - 0.2f * lift);
    fy = hy + (fy - hy) * (1.0f - 0.2f * lift);
    P[i] = fxLegSolve(S.x + (c * hx - s * hy) * k, S.y + (s * hx + c * hy) * k,
                      S.x + (c * fx - s * fy) * k, S.y + (s * fx + c * fy) * k,
                      chord * 0.66f * k, chord * 0.62f * k, -s * sd, c * sd);
  }
}
// One step toward (tx,ty), `d` px off: turning no faster than `rate` rad/ms,
// wandering a little on noise while it is still far, slowing into a hard
// turn -- and turning on the spot when `spd` is 0. The legs step for the
// turn as well as the walk.
static void fxSpiderStep(FxSpider& S, float tx, float ty, float d, float spd, float rate, float t, float dt) {
  float want = atan2f(ty - S.y, tx - S.x) + (fxNoise1(t * 0.004f, S.seed) - 0.5f) * 0.9f * fxClampF(d / 40.0f, 0.0f, 1.0f);
  float da = want - S.head;
  while (da >  FX_TAU * 0.5f) da -= FX_TAU;
  while (da < -FX_TAU * 0.5f) da += FX_TAU;
  float turn = fxClampF(da, -rate * dt, rate * dt);
  S.head += turn;
  float go = fminf(d, spd * dt) * fmaxf(0.15f, cosf(da));
  S.x += cosf(S.head) * go;
  S.y += sinf(S.head) * go;
  S.walk += go + fabsf(turn) * 5.0f * S.sc;
}
// `rim` is the rung the light catches its edge in. The big one also gets
// bristles, all eight eyes, pedipalps that will not keep still, fangs when
// it means it, and the hourglass.
static void fxSpiderDraw(const FxSpider& S, uint32_t now, float shx, float shy, uint8_t rim, bool big) {
  if (!S.on) return;
  FxSpider T = S;
  T.x += shx; T.y += shy;
  FxLegPose P[10];
  fxSpiderLegs(T, now, P);
  const float c = cosf(S.head), s = sinf(S.head), k = S.sc;
  const float w = (big ? 1.3f : 1.0f) * k;
  int nl = 8;
  if (big)
    for (int sd = -1; sd <= 1; sd += 2) {
      float tw = fxNoise1(now * 0.008f + sd * 5.0f, S.seed + 3) - 0.5f;
      float hx = 5.0f, hy = 0.9f * sd, fx = 7.6f + tw * 1.6f, fy = (1.6f + tw) * sd;
      P[nl++] = fxLegSolve(T.x + (c * hx - s * hy) * k, T.y + (s * hx + c * hy) * k,
                           T.x + (c * fx - s * fy) * k, T.y + (s * fx + c * fy) * k,
                           1.7f * k, 1.5f * k, -s * sd, c * sd);
    }
  const float ax = T.x - c * 5.6f * k, ay = T.y - s * 5.6f * k;   // abdomen
  const float qx = T.x + c * 2.2f * k, qy = T.y + s * 2.2f * k;   // head and thorax
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < nl; i++)
      fxLegDraw(P[i], i < 8 ? w : w * 0.55f, pass, rim, (big && i < 8) ? 1.1f * k : 0.0f,
                S.seed + (uint32_t)i * 131U);
    fxBlobLit(ax, ay, c, s, 5.8f * k, 4.4f * k, S.seed, pass, rim);
    fxBlobLit(qx, qy, c, s, 3.4f * k, 2.9f * k, S.seed + 1, pass, rim);
  }
  if (big) {
    // The hourglass, in the one ink that is not the phosphor's.
    float lx = -s * 1.7f * k, ly = c * 1.7f * k, fx = c * 2.6f * k, fy = s * 2.6f * k;
    fxTri(ax, ay, ax + fx + lx, ay + fy + ly, ax + fx - lx, ay + fy - ly, FX_PAL[FXP_BLOOD]);
    fxTri(ax, ay, ax - fx + lx, ay - fy + ly, ax - fx - lx, ay - fy - ly, FX_PAL[FXP_BLOOD]);
  }
  // Where the light, from above and to the left, catches the abdomen.
  fxDisc(ax - 1.8f * k, ay - 1.6f * k, fmaxf(0.7f, 0.5f * k), FX_PAL[big ? FXP_HDR : FXP_OK]);
  if (big) fxDisc(ax - 2.0f * k, ay - 1.8f * k, 0.25f * k, FX_PAL[FXP_WHITE]);
  // Eyes: two glints on a small one, all eight on the big one.
  static const float EYE[4][3] = { { 5.0f, 0.75f, 0.55f }, { 4.5f, 1.75f, 0.42f },
                                   { 4.0f, 0.70f, 0.36f }, { 3.6f, 2.00f, 0.42f } };
  for (int e = 0; e < (big ? 4 : 1); e++)
    for (int sd = -1; sd <= 1; sd += 2) {
      float ex = EYE[e][0], ey = EYE[e][1] * sd;
      float X = T.x + (c * ex - s * ey) * k, Y = T.y + (s * ex + c * ey) * k;
      if (!big) { fxPut((int)X, (int)Y, FX_PAL[FXP_GLOW]); continue; }
      fxDisc(X, Y, EYE[e][2] * k, FX_PAL[FXP_CRIT]);
      fxPut((int)(X - 0.4f), (int)(Y - 0.4f), FX_PAL[FXP_WHITE]);
    }
  if (big && S.fangs)
    for (int sd = -1; sd <= 1; sd += 2) {
      float x0 = 5.6f, y0 = 0.6f * sd, x1 = 7.3f, y1 = 0.15f * sd;
      fxStroke(T.x + (c * x0 - s * y0) * k, T.y + (s * x0 + c * y0) * k,
               T.x + (c * x1 - s * y1) * k, T.y + (s * x1 + c * y1) * k, 0.55f * k, 0.25f * k, FX_PAL[FXP_GLOW]);
    }
}

// A fly from above, `k` px to the unit. mode 0 in the air (the wings a blur
// of two strokes at once), 1 set down (wings folded, forelegs working at each
// other), 2 stuck (in the air, going nowhere, frantic about it).
static void fxFlyDraw(float x, float y, float head, float k, uint8_t mode, uint32_t frame, uint32_t now) {
  const float c = cosf(head), s = sinf(head);
  for (int pass = 0; pass < 2; pass++) {
    fxBlobLit(x - c * 2.3f * k, y - s * 2.3f * k, c, s, 2.7f * k, 2.0f * k, 7, pass, FXP_BRICK);
    fxBlobLit(x + c * 0.6f * k, y + s * 0.6f * k, c, s, 1.9f * k, 1.9f * k, 8, pass, FXP_BRICK);
    fxBlobLit(x + c * 3.1f * k, y + s * 3.1f * k, c, s, 1.45f * k, 1.45f * k, 9, pass, FXP_BRICK);
  }
  // The eyes, which on a fly are most of the head, and red.
  for (int sd = -1; sd <= 1; sd += 2)
    fxDisc(x + (c * 3.3f - s * 1.05f * sd) * k, y + (s * 3.3f + c * 1.05f * sd) * k, 0.9f * k, FX_PAL[FXP_BLOOD]);
  if (mode == 1) {
    static const float LEG[3][4] = { { 3.9f, 0.8f, 5.8f, 0.0f }, { 0.9f, 1.4f, 1.7f, 3.2f }, { -0.1f, 1.5f, -1.6f, 3.2f } };
    const float rub = ((now / 70) & 1) ? 0.2f : 0.8f;
    for (int sd = -1; sd <= 1; sd += 2) {
      for (int l = 0; l < 3; l++) {
        float x0 = LEG[l][0], y0 = LEG[l][1] * sd, x1 = LEG[l][2], y1 = (l ? LEG[l][3] : rub) * sd;
        fxLine((int)(x + (c * x0 - s * y0) * k), (int)(y + (s * x0 + c * y0) * k),
               (int)(x + (c * x1 - s * y1) * k), (int)(y + (s * x1 + c * y1) * k), FX_PAL[l ? FXP_RUST : FXP_OK]);
      }
      float dxb = -0.987f, dyb = 0.16f * sd;
      float bx = 0.2f + dxb * 3.0f, by = 0.5f * sd + dyb * 3.0f;
      fxStip = 90; fxStipSeed = 5;
      fxBlob(x + (c * bx - s * by) * k, y + (s * bx + c * by) * k, c * dxb - s * dyb, s * dxb + c * dyb,
             3.1f * k, 1.1f * k, 11, FX_PAL[FXP_OK]);
      fxStip = 0;
    }
    return;
  }
  // In the air the wings are two strokes at once: this frame's nearly there,
  // the other nearly gone.
  const int fresh = (int)(frame & 1);
  for (int pz = 0; pz < 2; pz++) {
    float phi = mode == 2 ? (pz ? 1.25f : 0.3f) : (pz ? 1.05f : 0.42f);
    fxStip = (uint8_t)(pz == fresh ? 70 : 180);
    fxStipSeed = frame * 31U + (uint32_t)pz;
    for (int sd = -1; sd <= 1; sd += 2) {
      float dxb = -cosf(phi), dyb = sinf(phi) * sd;
      float bx = 0.3f + dxb * 3.1f, by = 0.6f * sd + dyb * 3.1f;
      fxBlob(x + (c * bx - s * by) * k, y + (s * bx + c * by) * k, c * dxb - s * dyb, s * dxb + c * dyb,
             3.2f * k, 1.25f * k, 13, FX_PAL[FXP_GLOW]);
    }
  }
  fxStip = 0;
}
// The comic's way to letter a buzz: a small z, shaken loose.
static void fxZed(float x, float y, float z, uint32_t seed, uint16_t col) {
  FxRng R; R.s = seed;
  float j = z * 0.2f;
  float x0 = x - z * 0.5f  + R.sgn() * j, y0 = y - z * 0.5f  + R.sgn() * j;
  float x1 = x + z * 0.5f  + R.sgn() * j, y1 = y - z * 0.55f + R.sgn() * j;
  float x2 = x - z * 0.45f + R.sgn() * j, y2 = y + z * 0.5f  + R.sgn() * j;
  float x3 = x + z * 0.55f + R.sgn() * j, y3 = y + z * 0.45f + R.sgn() * j;
  float w = z > 6.0f ? 1.8f : 1.0f;
  fxStroke(x0, y0, x1, y1, w, w * 0.8f, col);
  fxStroke(x1, y1, x2, y2, w * 0.9f, w * 0.7f, col);
  fxStroke(x2, y2, x3, y3, w * 0.8f, w, col);
}
// Silk wrapped round and round something that was a fly.
static void fxBundleDraw(float x, float y, float head, float k) {
  const float c = cosf(head), s = sinf(head);
  fxBlob(x, y, c, s, 3.6f * k + 1.0f, 2.3f * k + 1.0f, 17, FX_PAL[FXP_INK]);
  fxBlob(x, y, c, s, 3.6f * k, 2.3f * k, 17, FX_PAL[FXP_GLOW]);
  for (int q = -1; q <= 1; q++) {
    float x0 = q * 1.7f * k - 0.9f * k, y0 = -2.3f * k, x1 = q * 1.7f * k + 0.9f * k, y1 = 2.3f * k;
    fxLine((int)(x + c * x0 - s * y0), (int)(y + s * x0 + c * y0),
           (int)(x + c * x1 - s * y1), (int)(y + s * x1 + c * y1), FX_PAL[FXP_RUST]);
  }
}

// A cobweb in a corner of the glass: radials fanned from the corner, rings
// that sag between them toward it the way silk hangs, a few strands broken,
// a few hanging loose, and dew on some that has caught the light. It spins
// itself while it is watched: the radials shoot out, then the rings are laid
// from the outside in, back and forth, a strand at a time.
static const int FX_WEB_RAD = 7, FX_WEB_RING = 6;
static const uint16_t FX_WEB_RADMS = 60;     // between radials
static const uint16_t FX_WEB_RINGT = 500;    // the first ring, after the first radial
static const uint16_t FX_WEB_SEGMS = 38;     // each strand of a ring
struct FxWeb {
  float    hx, hy;                                    // the hub, just off the corner
  float    ax[FX_WEB_RAD], ay[FX_WEB_RAD], len[FX_WEB_RAD];
  float    r[FX_WEB_RING][FX_WEB_RAD];                // ring k (0 outermost) across radial i
  uint32_t seed;
};
static void fxWebGen(FxWeb& W, float hx, float hy, float a0, float a1, float reach, uint32_t seed) {
  FxRng R; R.s = seed;
  W.hx = hx; W.hy = hy; W.seed = seed;
  for (int i = 0; i < FX_WEB_RAD; i++) {
    float a = fxLerp(a0, a1, ((float)i + 0.5f + R.sgn() * 0.3f) / FX_WEB_RAD);
    W.ax[i] = cosf(a); W.ay[i] = sinf(a);
    W.len[i] = reach * (0.8f + 0.25f * R.u());
  }
  for (int k = 0; k < FX_WEB_RING; k++)
    for (int i = 0; i < FX_WEB_RAD; i++) W.r[k][i] = W.len[i] * (0.9f - 0.135f * k + R.sgn() * 0.035f);
}
// Ring k's strand from radial i to i+1, `u` of the way along: a quadratic
// through a midpoint that hangs toward the hub. (px,py) are the crossings,
// already shaken.
static void fxWebAt(const FxWeb& W, float ax, float ay, float bx, float by, int k, int i, float u,
                    float* x, float* y) {
  float mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f, tx = W.hx - mx, ty = W.hy - my;
  float tl = sqrtf(tx * tx + ty * ty) + 0.001f, L = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
  float sag = (0.10f + 0.12f * fxU(fxHash2((uint32_t)(k * 16 + i), W.seed + 1))) * L * 2.0f;
  float cx = mx + tx / tl * sag, cy = my + ty / tl * sag, iu = 1.0f - u;
  *x = iu * iu * ax + 2.0f * u * iu * cx + u * u * bx;
  *y = iu * iu * ay + 2.0f * u * iu * cy + u * u * by;
}
// `t` ms since the spinning began. It trembles `amp` px round (vx,vy).
static void fxWebDraw(const FxWeb& W, float t, float vx, float vy, float amp, uint32_t frame,
                      float shx, float shy) {
  if (t <= 0) return;
  // Every crossing once, shaken; the last row is the radials' tips.
  float px[FX_WEB_RING + 1][FX_WEB_RAD], py[FX_WEB_RING + 1][FX_WEB_RAD];
  for (int k = 0; k <= FX_WEB_RING; k++)
    for (int i = 0; i < FX_WEB_RAD; i++) {
      float r = k < FX_WEB_RING ? W.r[k][i] : W.len[i];
      float x = W.hx + W.ax[i] * r, y = W.hy + W.ay[i] * r;
      if (amp > 0) {
        float f = 1.0f - sqrtf((x - vx) * (x - vx) + (y - vy) * (y - vy)) / 80.0f;
        if (f > 0) {
          uint32_t h = fxHash2((uint32_t)(k * 16 + i), frame);
          x += fxS(h) * amp * f;
          y += fxS(fxHash(h + 1)) * amp * f;
        }
      }
      px[k][i] = x + shx; py[k][i] = y + shy;
    }
  const float hx = W.hx + shx, hy = W.hy + shy;
  const uint16_t silk = FX_PAL[FXP_EMBER], lit = FX_PAL[FXP_RUST], dew = FX_PAL[FXP_GLOW];
  // The radials: out from the hub through every ring they carry.
  for (int i = 0; i < FX_WEB_RAD; i++) {
    float g = fxClampF((t - i * FX_WEB_RADMS) / 150.0f, 0.0f, 1.0f);
    if (g <= 0) continue;
    float want = W.len[i] * fxCubicOut(g), done = 0, lx = hx, ly = hy;
    for (int k = FX_WEB_RING - 1; k >= -1 && done < want; k--) {
      int row = k < 0 ? FX_WEB_RING : k;
      float nx = px[row][i], ny = py[row][i];
      float L = sqrtf((nx - lx) * (nx - lx) + (ny - ly) * (ny - ly));
      if (done + L > want) { float f = (want - done) / L; nx = lx + (nx - lx) * f; ny = ly + (ny - ly) * f; }
      fxLine((int)lx, (int)ly, (int)nx, (int)ny, (i & 1) ? silk : lit);
      done += L; lx = nx; ly = ny;
    }
  }
  // Two long anchor lines to the glass's edges, and a few loose strands.
  float ga = fxClampF((t - 150.0f) / 200.0f, 0.0f, 1.0f);
  if (ga > 0) {
    const int e = FX_WEB_RAD - 1;
    float tx0 = px[FX_WEB_RING][0], ty0 = py[FX_WEB_RING][0], tx1 = px[FX_WEB_RING][e], ty1 = py[FX_WEB_RING][e];
    fxLine((int)tx0, (int)ty0, (int)(tx0 + (FX_W + 1 + shx - tx0) * ga), (int)(ty0 + 34.0f * ga), silk);
    fxLine((int)tx1, (int)ty1, (int)(tx1 - 38.0f * ga), (int)(ty1 + (-1.0f + shy - ty1) * ga), silk);
  }
  // The rings: outside in, back and forth, laid one strand at a time.
  int q = 0;
  for (int k = 0; k < FX_WEB_RING; k++)
    for (int s = 0; s < FX_WEB_RAD - 1; s++, q++) {
      int i = (k & 1) ? FX_WEB_RAD - 2 - s : s;
      float u = fxClampF((t - FX_WEB_RINGT - q * FX_WEB_SEGMS) / (float)FX_WEB_SEGMS, 0.0f, 1.0f);
      if (u <= 0) continue;
      uint32_t h = fxHash2((uint32_t)(k * 16 + i), W.seed);
      if (h % 13 == 0) continue;                               // broken
      float ax = px[k][i], ay = py[k][i], bx = px[k][i + 1], by = py[k][i + 1];
      if (k & 1) { float tt = ax; ax = bx; bx = tt; tt = ay; ay = by; by = tt; }
      float lx = ax, ly = ay;
      for (int n = 1; n <= 5; n++) {
        float x, y;
        fxWebAt(W, ax - shx, ay - shy, bx - shx, by - shy, k, i, u * n / 5.0f, &x, &y);
        fxLine((int)lx, (int)ly, (int)(x + shx), (int)(y + shy), (h & 7) == 1 ? lit : silk);
        lx = x + shx; ly = y + shy;
      }
      float dw = 0.25f + 0.5f * fxU(fxHash(h + 3));
      if ((h >> 4) % 3 == 0 && u > dw) {
        float x, y;
        fxWebAt(W, ax - shx, ay - shy, bx - shx, by - shy, k, i, dw, &x, &y);
        fxPut((int)(x + shx), (int)(y + shy), dew);
      }
      if (k < 2 && (h >> 9) % 4 == 0 && u >= 1.0f) {
        // Loose: a strand that let go at one end and hangs, and sways.
        float x, y;
        fxWebAt(W, ax - shx, ay - shy, bx - shx, by - shy, k, i, 0.5f, &x, &y);
        float hang = 7.0f + 10.0f * fxU(fxHash(h + 5)), sway = (fxNoise1(t * 0.0012f, h) - 0.5f) * 5.0f;
        fxLine((int)(x + shx), (int)(y + shy), (int)(x + shx + sway), (int)(y + shy + hang), silk);
      }
    }
}

// ── 9. The cue catalogue ─────────────────────────────────────────────────────
// A cue is a moment worth a panel. The game side only ever names one and says
// who it is about; everything about how it looks lives in FX_STYLE.
enum FxKind : uint8_t {
  FXK_NONE = 0,
  FXK_QUAKE,      // the earth heaves
  FXK_STRIKE,     // lightning hits a survivor
  FXK_STORM,      // storm walks in
  FXK_CHEM,       // chem rain
  FXK_FOG,        // strangle fog / mist
  FXK_WEATHER,    // rain, clearing: a caption, not a panel
  FXK_DOOM_EYE,   // the Doom turns its head
  FXK_DOOM_HUNT,  // it is hunting someone now
  FXK_DOOM_ACT,   // it took something
  FXK_DOOM_LOST,  // it lost the trail
  FXK_DOWNED,     // someone went down (the skull takes it from here)
  FXK_DAWN,       // a new day: the chapter card
  FXK_JOIN,       // new blood
  FXK_THREAT,     // the clock crossed a band
  FXK_LONGODDS,   // won a check the numbers said was lost
  FXK_THROWN,     // thrown back out of an encounter
  FXK_CLEARED,    // an encounter cleared out entire
  FXK_FIRE,       // caught in the burn
  FXK_FLOOD,      // swept by the flood
  FXK_BELOW,      // someone went down a hatch into the tunnels: the nest
  FXK_CRAFTED,    // someone made something: the Committee's commendation
  FXK_CRAWL,      // someone came back up, and one of the small ones came with them
  FXK_COUNT
};
// Band fills. The last two are not bands at all but cut scenes: the nest
// (section 12) and the commendation (section 13).
enum : uint8_t { FXF_FOCUS = 0, FXF_SPEED, FXF_HAZARD, FXF_KRACKLE, FXF_RAIN, FXF_SUN, FXF_EYES, FXF_DARK,
                 FXF_NEST, FXF_MEDAL };
// Impact flashes: a white-out frame, a negative frame, or both in that order.
enum : uint8_t { FXFL_NONE = 0, FXFL_WHITE = 1, FXFL_NEG = 2, FXFL_BOTH = 3 };
// Extras.
enum : uint16_t {
  FXE_CRACKS = 1 << 0, FXE_BOLT = 1 << 1, FXE_FIRE = 1 << 2, FXE_RAIN = 1 << 3,
  FXE_SPLAT = 1 << 4, FXE_VROLL = 1 << 5, FXE_TEAR = 1 << 6, FXE_DRIPS = 1 << 7,
  FXE_CRESC = 1 << 8, FXE_NOBAND = 1 << 9, FXE_BGLINES = 1 << 10, FXE_BLOOD = 1 << 11,
};
struct FxStyleDef {
  const char* sfx;      // the sound; nullptr = a caption panel
  uint8_t  fill;
  uint8_t  prio;        // 1 aside, 2 event, 3 catastrophe
  uint8_t  trauma;      // shake at impact, 0-255
  uint8_t  flash;
  uint16_t extras;
  uint16_t holdMs;      // band on screen, before the blade
  uint32_t coolMs;      // this kind cannot cut in again this soon
  uint8_t  size;        // SFX size, % of the fit
};
static const FxStyleDef FX_STYLE[FXK_COUNT] = {
  /* NONE      */ { nullptr,    FXF_DARK,    0,   0, FXFL_NONE, 0, 0, 0, 100 },
  /* QUAKE     */ { "KRRAKK",   FXF_FOCUS,   3, 255, FXFL_BOTH,
                    FXE_CRACKS | FXE_VROLL | FXE_TEAR | FXE_CRESC | FXE_BGLINES, 1800, 4000, 112 },
  /* STRIKE    */ { "KA-THOOM", FXF_FOCUS,   3, 230, FXFL_BOTH, FXE_BOLT | FXE_CRESC | FXE_BGLINES, 1600, 3000, 100 },
  /* STORM     */ { "KRAKOOM",  FXF_RAIN,    2, 150, FXFL_WHITE, FXE_BOLT | FXE_RAIN | FXE_CRESC, 1600, 8000, 100 },
  /* CHEM      */ { "HSSSSSS",  FXF_KRACKLE, 2,  80, FXFL_NONE, FXE_DRIPS, 1600, 8000, 100 },
  /* FOG       */ { nullptr,    FXF_DARK,    1,   0, FXFL_NONE, 0, 1900, 8000, 100 },
  /* WEATHER   */ { nullptr,    FXF_SPEED,   1,  30, FXFL_NONE, 0, 1500, 8000, 100 },
  /* DOOM_EYE  */ { nullptr,    FXF_EYES,    3,  70, FXFL_NONE, FXE_TEAR, 2200, 40000, 100 },
  /* DOOM_HUNT */ { "RUN",      FXF_EYES,    3, 210, FXFL_NEG,
                    FXE_VROLL | FXE_TEAR | FXE_DRIPS | FXE_BLOOD | FXE_BGLINES, 2000, 30000, 150 },
  /* DOOM_ACT  */ { "SKRITCH",  FXF_FOCUS,   3, 170, FXFL_NEG, FXE_SPLAT | FXE_DRIPS | FXE_BLOOD, 1500, 30000, 100 },
  /* DOOM_LOST */ { nullptr,    FXF_SPEED,   1,  20, FXFL_NONE, 0, 1600, 20000, 100 },
  /* DOWNED    */ { nullptr,    FXF_DARK,    3, 255, FXFL_BOTH, FXE_NOBAND | FXE_VROLL | FXE_TEAR, 600, 0, 100 },
  /* DAWN      */ { "DAY",      FXF_SUN,     2,  60, FXFL_NONE, 0, 1800, 20000, 105 },
  /* JOIN      */ { "NEW BLOOD", FXF_SPEED,  1,  90, FXFL_WHITE, FXE_CRESC, 1500, 3000, 100 },
  /* THREAT    */ { "THREAT",   FXF_HAZARD,  2, 130, FXFL_NONE, FXE_TEAR, 1700, 5000, 100 },
  /* LONGODDS  */ { "LONG ODDS", FXF_SUN,    2, 110, FXFL_WHITE, FXE_CRESC, 1600, 4000, 100 },
  /* THROWN    */ { "WHAM",     FXF_FOCUS,   2, 200, FXFL_NEG, FXE_SPLAT | FXE_BGLINES, 1500, 4000, 118 },
  /* CLEARED   */ { "CLEARED",  FXF_SUN,     2,  90, FXFL_WHITE, FXE_CRESC, 1600, 4000, 100 },
  /* FIRE      */ { "FWOOSH",   FXF_FOCUS,   2, 130, FXFL_WHITE, FXE_FIRE | FXE_CRESC, 1700, 25000, 104 },
  /* FLOOD     */ { "SPLOOSH",  FXF_RAIN,    2, 150, FXFL_NONE, FXE_RAIN | FXE_DRIPS, 1600, 25000, 100 },
  // Seven seconds is a lot of screen to hand over, and survivors go up and
  // down the hatches all game: only a descent three minutes clear of the last
  // one gets the nest.
  /* BELOW     */ { "KRUNCH",   FXF_NEST,    2,   0, FXFL_NONE, FXE_BLOOD | FXE_DRIPS, 6800, 180000, 100 },
  // The one cue whose `cap` is not a sentence: it is what was made (the
  // recipe's name), which the certificate cites. The word is the stamp's.
  /* CRAFTED   */ { "THUNK",    FXF_MEDAL,   2,   0, FXFL_NONE, 0, 5900, 120000, 100 },
  // Not a panel either: one small spider let loose on the glass, which walks
  // over whatever else is on it (fxCrawlFrame).
  /* CRAWL     */ { nullptr,    FXF_DARK,    1,   0, FXFL_NONE, FXE_NOBAND, 0, 60000, 100 },
};
// A cut scene rather than a band: it holds the panel for seconds, draws no
// band, and takes the whole screen down into the dark behind it.
static inline bool fxIsScene(uint8_t kind) { return FX_STYLE[kind].fill >= FXF_NEST; }

// Requests from the game side. drainEvents() runs on the game task while the
// LCD composes on loop(); the queue is the only thing the two share and it is
// held under a spinlock for a copy, never across any drawing.
struct FxCueReq {
  uint8_t  kind;
  int8_t   who;
  uint16_t arg;
  uint32_t atMs;
  char     sfx[16];
  char     cap[56];
};
static FxCueReq fxQ[4];
static uint8_t  fxQn = 0;

// Every panel the board cues is also written here, whatever the LCD's own FX
// level, so anything else watching can cut in on the same beats: /state
// carries it as `fx` (fxCueLogJson below) and data/observer-fx.js plays it on
// the TV. `n` is a sequence number that only ever goes up (it wraps at 65536,
// and the reader compares modulo that); the kind goes out as a name so the
// two sides never have to agree on an enum's order. MIRRORED-IN
// data/observer-fx.js (STYLE).
static const char* const FX_KIND_NAME[FXK_COUNT] = {
  "", "QUAKE", "STRIKE", "STORM", "CHEM", "FOG", "WEATHER", "DOOM_EYE", "DOOM_HUNT",
  "DOOM_ACT", "DOOM_LOST", "DOWNED", "DAWN", "JOIN", "THREAT", "LONGODDS", "THROWN",
  "CLEARED", "FIRE", "FLOOD", "BELOW", "CRAFTED", "CRAWL",
};
struct FxLogEnt { uint16_t n; uint8_t kind; int8_t who; uint16_t arg; char cap[56]; };
static const int FX_LOG_N = 6;
static FxLogEnt fxLog[FX_LOG_N];
static uint16_t fxLogSeq = 0;

// Queue a panel. Safe from any task. `cap` is the chronicle's own sentence (a
// predicate when `who` names a survivor); `sfx` overrides the style's word.
static void fxCue(uint8_t kind, int8_t who = -1, const char* cap = nullptr,
                  const char* sfx = nullptr, uint16_t arg = 0) {
  if (kind == FXK_NONE || kind >= FXK_COUNT) return;
  FxCueReq r;
  memset(&r, 0, sizeof(r));
  r.kind = kind; r.who = who; r.arg = arg; r.atMs = fxNowMs();
  if (cap) strncpy(r.cap, cap, sizeof(r.cap) - 1);
  if (sfx) strncpy(r.sfx, sfx, sizeof(r.sfx) - 1);
  FX_LOCK();
  fxLogSeq++;
  if (!fxLogSeq) fxLogSeq = 1;                     // 0 means "never written"
  FxLogEnt& lg = fxLog[fxLogSeq % FX_LOG_N];
  lg.n = fxLogSeq; lg.kind = kind; lg.who = who; lg.arg = arg;
  memcpy(lg.cap, r.cap, sizeof(lg.cap));
  if (fxQn < 4) fxQ[fxQn++] = r;
  else {
    // Full: the least urgent request makes room, if it is less urgent than this.
    int lo = 0;
    for (int i = 1; i < 4; i++) if (FX_STYLE[fxQ[i].kind].prio < FX_STYLE[fxQ[lo].kind].prio) lo = i;
    if (FX_STYLE[fxQ[lo].kind].prio <= FX_STYLE[kind].prio) fxQ[lo] = r;
  }
  FX_UNLOCK();
}

// ── 10. Engine state ─────────────────────────────────────────────────────────
struct FxDread {                  // the parts of g_dread this file reads, 0-255
  uint8_t tc, doomClose, doomAware, attrition, hunger, thirst, rad, wounds, fire;
  uint8_t downed, connected;
};
enum : uint8_t { FXD_CALM = 0, FXD_DOOM, FXD_HUNGER, FXD_THIRST, FXD_RAD, FXD_BLOOD, FXD_CLOCK };

struct FxCut {
  bool     on;
  uint8_t  kind;
  uint32_t t0, seed;
  float    yc, h, k;              // the band: centre row, height, slope
  FxWord   word;
  bool     hasWord;
  char     cap[3][FX_CAP_CH];
  uint8_t  capN;
  int16_t  capW;
  char     name[16];
  uint16_t hit;                   // letters whose slam has already landed
  bool     bladeHit;
};

static struct FxState {
  uint8_t  level;                 // 0 off, 1 restrained, 2 madness
  uint32_t lastMs;
  uint32_t frame;
  float    trauma;
  uint8_t  flashQ[3], flashN;     // impact frames still to show
  uint8_t  madness, madTarget, cause;
  bool     doomNear, lowLife;
  uint32_t nextGlitch, glitchUntil;
  uint8_t  glitchKind;
  uint32_t nextSub, subAt;
  uint8_t  subFrames;
  bool     subReady;
  uint32_t nextEyes, eyesT0;
  int16_t  eyesX, eyesY;
  bool     eyesOn;
  uint32_t nextBeat;
  uint8_t  beatStep;
  uint32_t vrollT0;
  uint16_t vrollDur;
  uint8_t  vrollKind;             // 0 slip, 1 full roll
  struct { int16_t y0, h, dx; uint8_t curve; } tear[3];
  uint8_t  tearN;
  uint32_t tearUntil;
  uint32_t lastKind[FXK_COUNT];
  bool     takeover;              // a full-screen takeover (the skull) owns the panel
  uint32_t toastUntil;
  // Screen switch: the old frame is cut along a line and the halves slide off.
  bool     sw;
  uint32_t swT0;
  float    swCx, swCy, swDx, swDy;
  // Content repaint: rows that changed are reprinted rather than snapped.
  struct { int16_t y0, y1; uint32_t t0; uint8_t style; } rep[4];
  uint8_t  repN;
  bool     haveHash;
  uint32_t boltAmbT0;
  bool     boltAmb;
  bool     lastFlash;             // the frame on the glass is a flash: nothing ghosts from it
  uint32_t cutEnd;                // when the last panel left
} FX;

static FxCut     fxCut;
static FxWord*   fxSubWord = nullptr; // the subliminal phrase, built ahead of its frame (PSRAM)
static uint32_t* fxRowHash = nullptr; // FX_H, PSRAM
static uint16_t  fxL[FX_W];           // the row being composed (internal RAM)
static int16_t   fxBT[FX_W], fxBB[FX_W];
static uint8_t   (*fxTone)[30] = nullptr;   // [40] halftone shadow, one tone per 8x8 cell (PSRAM)
static float*    fxInvH = nullptr;          // [FX_W] a band's 2/height per column (PSRAM)

// Add shake. Trauma is squared on the way to pixels, so small knocks stay
// small and a big one is violent.
static void fxImpact(float amount) { FX.trauma = fxClampF(FX.trauma + amount, 0.0f, 1.0f); }
static void fxFlash(uint8_t kind) {
  if (kind & FXFL_WHITE) { if (FX.flashN < 3) FX.flashQ[FX.flashN++] = FXFL_WHITE; }
  if (kind & FXFL_NEG)   { if (FX.flashN < 3) FX.flashQ[FX.flashN++] = FXFL_NEG; }
}
static void fxVroll(uint32_t now, uint8_t kind, uint16_t dur) {
  FX.vrollT0 = now; FX.vrollKind = kind; FX.vrollDur = dur;
}
static void fxTearBurst(uint32_t now, uint8_t n, int mag, uint16_t dur) {
  FxRng R; R.s = fxHash(now * 31U + FX.frame);
  FX.tearN = (uint8_t)fxMinI(3, n);
  for (int i = 0; i < FX.tearN; i++) {
    FX.tear[i].y0 = (int16_t)R.range(0, FX_H - 20);
    FX.tear[i].h  = (int16_t)R.range(6, 44);
    FX.tear[i].dx = (int16_t)(R.sgn() * mag);
    FX.tear[i].curve = (uint8_t)(R.next() & 1);
  }
  FX.tearUntil = now + dur;
}

static void fxSetDread(const FxDread& d) {
  int m = 0, cause = FXD_CALM, v;
  v = d.tc * 55 / 100;                                       if (v > m) { m = v; cause = FXD_CLOCK; }
  v = d.doomAware >= 51 ? d.doomClose : d.doomClose * 3 / 10; if (v > m) { m = v; cause = FXD_DOOM; }
  v = d.attrition * 8 / 10;                                  if (v > m) { m = v; cause = FXD_BLOOD; }
  v = d.hunger * 6 / 10;                                     if (v > m) { m = v; cause = FXD_HUNGER; }
  v = d.thirst * 6 / 10;                                     if (v > m) { m = v; cause = FXD_THIRST; }
  v = d.rad * 7 / 10;                                        if (v > m) { m = v; cause = FXD_RAD; }
  v = d.wounds * 6 / 10;                                     if (v > m) { m = v; cause = FXD_BLOOD; }
  v = d.fire / 2;                                            if (v > m) { m = v; }
  if (d.downed) m = fxMaxI(m, 180);
  if (!d.connected) m = 0;
  FX.madTarget = (uint8_t)fxMinI(255, m);
  FX.cause     = (uint8_t)cause;
  FX.doomNear  = d.connected && d.doomAware >= 51 && d.doomClose >= 110;
  FX.lowLife   = d.connected && (d.attrition >= 150 || d.downed);
}

// ── 11. Cut-ins ──────────────────────────────────────────────────────────────
static void fxNestBegin(FxCut& C);                                        // section 12
static void fxNestFrame(FxCut& C, uint32_t t, uint32_t now, float shx, float shy);
static void fxMedalBegin(FxCut& C, const FxCueReq& q);                    // section 13
static void fxMedalFrame(FxCut& C, uint32_t t, uint32_t now, float shx, float shy);
static void fxCrawlBegin(uint32_t seed, uint32_t now);                    // section 12

static void fxCutBegin(const FxCueReq& q, uint32_t now) {
  const FxStyleDef& st = FX_STYLE[q.kind];
  FxCut& C = fxCut;
  C.on = true; C.kind = q.kind; C.t0 = now; C.hit = 0; C.bladeHit = false;
  // The panel's letters take the sticker pool; a whisper waiting in it is
  // gone, and the next one gets built again when its time comes.
  FX.subReady = false;
  FX.subFrames = 0;
  C.seed = fxHash(now * 2654435761U ^ ((uint32_t)q.kind << 24) ^ (uint32_t)q.who);
  FxRng R; R.s = C.seed;
  C.yc = 150.0f + R.sgn() * 22.0f;
  C.h  = (st.sfx || q.sfx[0]) ? 94.0f : 58.0f;
  if (q.kind == FXK_DOOM_EYE) C.h = 70.0f;
  C.k  = (R.u() < 0.5f ? -1.0f : 1.0f) * (0.12f + 0.07f * R.u());

  C.name[0] = 0;
  if (q.who >= 0) fxNameOf(q.who, C.name, sizeof(C.name));

  // The word. A few kinds build it out of the cue itself.
  char sfx[20];
  sfx[0] = 0;
  if (q.sfx[0])                      strncpy(sfx, q.sfx, sizeof(sfx) - 1), sfx[sizeof(sfx) - 1] = 0;
  else if (q.kind == FXK_DAWN)       snprintf(sfx, sizeof(sfx), "DAY %u", (unsigned)q.arg);
  else if (q.kind == FXK_THREAT)     snprintf(sfx, sizeof(sfx), q.arg >= 4 ? "TOO LATE" : "THREAT %u", (unsigned)q.arg);
  else if (q.kind == FXK_JOIN && C.name[0]) {
    int i = 0;
    for (; C.name[i] && i < 10; i++) sfx[i] = (char)toupper((unsigned char)C.name[i]);
    sfx[i] = 0;
  }
  else if (st.sfx)                   strncpy(sfx, st.sfx, sizeof(sfx) - 1), sfx[sizeof(sfx) - 1] = 0;
  C.hasWord = false;
  const bool nest = st.fill == FXF_NEST, medal = st.fill == FXF_MEDAL;
  if (sfx[0]) {
    const FxInk& ink = (st.extras & FXE_BLOOD) ? FX_INK_BLOOD : (st.fill == FXF_RAIN ? FX_INK_COLD : FX_INK_HOT);
    bool drips = (st.extras & FXE_DRIPS) != 0 || (q.kind == FXK_THREAT && q.arg >= 4);
    // A scene's word is one noise in it, not the panel's headline.
    C.hasWord = fxBuildWord(C.word, sfx, nest ? 150.0f : (medal ? 110.0f : 200.0f),
                            nest ? 40.0f : (medal ? 30.0f : C.h * 0.66f),
                            st.size / 100.0f, ink, (st.extras & FXE_CRESC) != 0, drips, C.seed);
  }

  // The caption: the chronicle's sentence, with the name in front of it.
  char full[80];
  if (q.cap[0]) {
    if (q.who >= 0 && C.name[0]) snprintf(full, sizeof(full), "%s %s", C.name, q.cap);
    else                         snprintf(full, sizeof(full), "%s", q.cap);
    C.capN = fxWrap(full, nest ? 132 : 190, C.cap, 3);   // the nest's sits in a corner
    C.capW = 0;
    for (int i = 0; i < C.capN; i++) C.capW = (int16_t)fxMaxI(C.capW, fxF2Width(C.cap[i]));
  } else {
    C.capN = 0;
    C.capW = 0;
  }

  // Impact.
  fxImpact(st.trauma / 255.0f);
  fxFlash(st.flash);
  if (st.extras & FXE_VROLL) fxVroll(now, q.kind == FXK_DOWNED || q.kind == FXK_QUAKE ? 1 : 0, 420);
  if (st.extras & FXE_TEAR)  fxTearBurst(now, 3, 26, 260);
  if (st.extras & FXE_CRACKS) {
    fxCracksGen(40.0f + R.u() * 160.0f, 50.0f + R.u() * 60.0f, C.seed + 11);
    fxCrkT0 = now; fxCrkOn = true;
  }
  if (st.extras & FXE_BOLT) {
    fxBoltSx = 30.0f + R.u() * 180.0f; fxBoltSy = -10.0f;
    fxBoltEx = 40.0f + R.u() * 160.0f; fxBoltEy = q.kind == FXK_STRIKE ? C.yc + 30.0f : 60.0f + R.u() * 60.0f;
    fxBoltGen(C.seed + 21);
    fxBoltT0 = now; fxBoltOn = true;
  }
  if ((st.extras & FXE_FIRE) && fxHeat) {
    fxFireSeed = C.seed; fxFireUntil = now + st.holdMs + 300; fxFireOn = true;
    memset(fxHeat, 0, FX_FIRE_R * FX_FIRE_C);
  }
  if (st.extras & FXE_SPLAT) { fxSplatGen(C.seed + 31); fxSplatT0 = now; fxSplatOn = true; }
  if (st.extras & FXE_NOBAND) C.on = false;
  if (nest)  fxNestBegin(C);
  if (medal) fxMedalBegin(C, q);
  if (q.kind == FXK_CRAWL) fxCrawlBegin(C.seed, now);
  FX.lastKind[q.kind] = now ? now : 1;
}

static uint32_t fxCutEnd(const FxCut& C) { return FX_STYLE[C.kind].holdMs + 330; }

// Band edge rows at column x, for a band slid by (ox, oy).
static void fxBandCols(const FxCut& C, float ox, float oy) {
  for (int x = 0; x < FX_W; x++) {
    float xs = x - ox;
    float ym = C.yc + oy + C.k * (xs - 120.0f);
    float nt = (fxNoise1(xs * 0.065f, C.seed) - 0.5f) * 6.0f;
    float nb = (fxNoise1(xs * 0.065f, C.seed + 101) - 0.5f) * 6.0f;
    fxBT[x] = (int16_t)floorf(ym - C.h * 0.5f + nt);
    fxBB[x] = (int16_t)floorf(ym + C.h * 0.5f + nb);
  }
  int y0 = 9999, y1 = -9999;
  for (int x = 0; x < FX_W; x++) { y0 = fxMinI(y0, fxBT[x]); y1 = fxMaxI(y1, fxBB[x]); }
  fxColY0 = fxMaxI(0, y0);
  fxColY1 = fxMinI(FX_H, y1);
}

// The panel body: an ink border, a white-hot keyline, and the fill printed as
// a flat plus screentone that thickens toward the edges.
static void fxBandBody(const FxCut& C, uint32_t t) {
  static const uint8_t BASE[8]  = { FXP_HDR,  FXP_CRIT, FXP_HOT,  FXP_HOT,   FXP_DIM,  FXP_HOT,   FXP_SOOT, FXP_BAND };
  static const uint8_t SHADE[8] = { FXP_RUST, FXP_BRICK, FXP_INK, FXP_FLAME, FXP_SOOT, FXP_FLAME, FXP_INK,  FXP_SOOT };
  const uint8_t f = FX_STYLE[C.kind].fill;
  const uint16_t base = FX_PAL[BASE[f]], shade = FX_PAL[SHADE[f]];
  const uint16_t ink = FX_PAL[FXP_INK], key = FX_PAL[(f == FXF_EYES || f == FXF_DARK) ? FXP_EMBER : FXP_WHITE];
  const int B = 4;
  int ymin = 9999, ymax = -9999;
  for (int x = 0; x < FX_W; x++) { ymin = fxMinI(ymin, fxBT[x]); ymax = fxMaxI(ymax, fxBB[x]); }
  int scroll = (int)(t / 18);
  // The grain is per 4x4 cell, so it is hashed once per cell row, not per pixel.
  static uint8_t grain[FX_W / 4];
  float* invH = fxInvH;
  for (int x = 0; x < FX_W; x++) invH[x] = 2.0f / (float)fxMaxI(1, fxBB[x] - fxBT[x]);
  int grainRow = -1;
  for (int y = fxMaxI(0, ymin - B); y < fxMinI(FX_H, ymax + B); y++) {
    uint16_t* row = fxDst + y * FX_W;
    const uint8_t* dots = FX_DOT + (y & 7) * 8;
    if ((y >> 2) != grainRow) {
      grainRow = y >> 2;
      for (int i = 0; i < FX_W / 4; i++) grain[i] = (uint8_t)(fxHash2((uint32_t)i, (uint32_t)grainRow + C.seed) & 31);
    }
    for (int x = 0; x < FX_W; x++) {
      int top = fxBT[x], bot = fxBB[x];
      if (y < top - B || y >= bot + B) continue;
      if (fxHalfN && !fxHalfOk(x, y)) continue;
      uint16_t col;
      if (y < top || y >= bot)             col = ink;
      else if (y == top + 1 || y == bot - 2) col = key;
      else if (f == FXF_HAZARD) {
        // Hazard stripes, chipped: the boundary wanders a pixel or two and
        // the black shows through in worn patches.
        int s = x + (y - top) + scroll + (int)(fxHash2((uint32_t)((x + y) >> 3), C.seed) & 3);
        bool blk = ((s / 14) & 1) != 0;
        if (!blk && (fxHash2((uint32_t)(x >> 2) * 131U + (uint32_t)(y >> 2), C.seed) & 31) == 0) blk = true;
        col = blk ? ink : base;
      } else {
        float e = fabsf((float)(y - top) * invH[x] - 1.0f);
        int tone = (int)(e * e * 210.0f) + (int)grain[x >> 2] - 16;
        col = ((int)dots[x & 7] < tone) ? shade : base;
      }
      row[x] = col;
    }
  }
}

// SFX: a letter at a time, each one slamming in at two and a half times its
// size, overshooting, and knocking the screen when it lands. `tw` is ms since
// the first letter was due; the word rides a slope `k` about (cx, cy).
static void fxWordSlam(const FxWord& Wd, int32_t tw, float cx, float cy, float k, uint32_t step, uint32_t boil) {
  for (int i = 0; i < Wd.n; i++) {
    const FxLetter& L = Wd.L[i];
    int32_t tau = tw - (int32_t)(i * step);
    if (tau < 0) continue;
    float scl, rot = L.rot;
    if (tau < 90) {
      float p = tau / 90.0f;
      scl = fxLerp(2.5f, 0.9f, powf(p, 1.6f));
      rot += (1.0f - p) * ((i & 1) ? 0.34f : -0.34f);
    } else if (tau < 170) {
      scl = fxLerp(0.9f, 1.0f, fxCubicOut((tau - 90) / 80.0f));
    } else {
      scl = 1.0f;
    }
    if (tau >= 90 && !(fxCut.hit & (1u << i))) {
      fxCut.hit |= (uint16_t)(1u << i);
      fxImpact(i + 1 == Wd.n ? 0.36f : 0.12f);
    }
    float bx = 0, by = 0;
    if (tau >= 170) {
      uint32_t hb = fxHash2((uint32_t)i, boil);
      bx = (float)((int)(hb & 3) - 1) * 0.7f;
      by = (float)((int)((hb >> 2) & 3) - 1) * 0.7f;
    }
    float lx = cx + L.x + bx, ly = cy + L.y + by + k * L.x;
    fxLetterDraw(L, lx, ly, scl, rot);
    if (L.nDrip && tau > 200) fxDripDraw(L, lx, ly, scl, rot, fxClampF((tau - 200) / 900.0f, 0.0f, 1.0f));
  }
}

// Everything that sits on the band, at slide offset (ox, oy).
static void fxBandDraw(const FxCut& C, uint32_t t, float ox, float oy) {
  const FxStyleDef& st = FX_STYLE[C.kind];
  const float cx = 120.0f + ox, cy = C.yc + oy;
  fxBandCols(C, ox, oy);
  fxBandBody(C, t);
  fxColT = fxBT; fxColB = fxBB;
  uint32_t boil = t / 80;
  float ang = atanf(C.k), dxl = cosf(ang), dyl = sinf(ang);
  switch (st.fill) {
    case FXF_FOCUS:
      fxFocusLines(cx, cy, 104.0f, 30.0f, 64, C.seed, boil, FX_PAL[FXP_INK], 4.5f);
      break;
    case FXF_SPEED:
      fxSpeedLines(cx, cy, C.h * 0.5f, dxl, dyl, 34, C.seed, (float)t * 0.9f, FX_PAL[FXP_INK], FX_PAL[FXP_WHITE]);
      break;
    case FXF_KRACKLE:
      fxKrackle(cx, cy, 110.0f, 5, C.seed ^ (boil / 2), FX_PAL[FXP_INK]);
      break;
    case FXF_RAIN:
      fxSpeedLines(cx, cy, 160.0f, 0.27f, 0.96f, 46, C.seed, (float)t * 1.4f, FX_PAL[FXP_RUST], FX_PAL[FXP_OK]);
      break;
    case FXF_SUN:
      fxSunburst(cx, cy + C.h * 0.9f, 22, C.seed, -FX_TAU * 0.5f + t * 0.00012f, FX_PAL[FXP_GLOW]);
      break;
    case FXF_EYES: {
      // The eye strip: open over a quarter second, a look to one side, one
      // blink, and shut again before the blade.
      float open = fxClampF((t - 60.0f) / 260.0f, 0.0f, 1.0f);
      uint32_t hold = st.holdMs;
      if (t > 1150 && t < 1290) open *= fabsf((float)t - 1220.0f) / 70.0f;
      if (t + 240 > hold) open *= fxClampF((hold - (float)t) / 240.0f, 0.0f, 1.0f);
      float look = (fxNoise1(t * 0.0021f, C.seed) - 0.5f) * 2.2f;
      fxFocusLines(cx, cy, 128.0f, 26.0f, 44, C.seed, boil, FX_PAL[FXP_TRACK], 2.5f);
      fxEyePair(cx, cy + 4.0f, 68.0f, 17.0f, 22.0f, open, look, C.seed, FXP_GLOW, FXP_CRIT, FXP_EMBER);
      break;
    }
    default:
      break;
  }

  if (C.hasWord) fxWordSlam(C.word, (int32_t)t - 120, cx, cy, C.k, 55, boil);
  fxColT = fxColB = nullptr;

  // A caption-only panel sets its sentence in the band itself.
  float capY;
  if (!C.hasWord && C.kind != FXK_DOOM_EYE && C.capN) {
    int total = C.capN * 16;
    capY = cy - total * 0.5f;
    for (int i = 0; i < C.capN; i++) {
      int w = fxF2Width(C.cap[i]);
      int lx = (int)(cx - w * 0.5f + C.k * 0), ly = (int)(capY + i * 16);
      fxF2(C.cap[i], lx + 1, ly + 1, FX_PAL[FXP_INK]);
      fxF2(C.cap[i], lx, ly, FX_PAL[FXP_WHITE]);
    }
    return;
  }

  // The name plate, pinned over the band's top-left.
  if (C.name[0] && C.kind != FXK_JOIN) {
    char nm[18];
    int i = 0;
    for (; C.name[i] && i < 15; i++) nm[i] = (char)toupper((unsigned char)C.name[i]);
    nm[i] = 0;
    int w = fxF2Width(nm) + 12, x = (int)(12 + ox);
    int y = (int)(C.yc + oy + C.k * (12 + 0 - 120.0f) - C.h * 0.5f - 13);
    fxRect(x + 3, y + 3, x + w + 3, y + 21, FX_PAL[FXP_INK]);
    fxRect(x, y, x + w, y + 18, FX_PAL[FXP_INK]);
    fxRect(x + 1, y + 1, x + w - 1, y + 2, FX_PAL[FXP_HOT]);
    fxF2(nm, x + 6, y + 2, FX_PAL[FXP_GLOW]);
  }

  // The caption box: the title art's own -- hot amber, black rule, black
  // capitals, a hard shadow -- dropped in under the band once the word is in.
  if (C.capN) {
    uint32_t tCap = 120 + (C.hasWord ? C.word.n * 55 : 0) + 110;
    if (t >= tCap || C.kind == FXK_DOOM_EYE) {
      float p = C.kind == FXK_DOOM_EYE ? fxClampF((t - 380.0f) / 150.0f, 0.0f, 1.0f)
                                       : fxClampF((t - tCap) / 150.0f, 0.0f, 1.0f);
      if (p <= 0) return;
      int bw = C.capW + 14, bh = C.capN * 16 + 8;
      int x  = (int)(FX_W - 8 - bw + ox);
      float midX = x + bw * 0.5f - ox - 120.0f;
      float bandBot = C.yc + oy + C.k * midX + C.h * 0.5f;
      // Tucked under the band's edge -- unless the word has burst out of the
      // panel, in which case under the word.
      float top = bandBot - 10.0f;
      if (C.hasWord) top = fmaxf(top, C.yc + oy + C.k * midX + C.word.h * 0.62f + 6.0f);
      int y  = (int)(top - (1.0f - fxBackOut(p)) * 22.0f);
      fxRect(x + 4, y + 4, x + bw + 4, y + bh + 4, FX_PAL[FXP_INK]);
      fxRect(x, y, x + bw, y + bh, FX_PAL[FXP_INK]);
      fxRect(x + 2, y + 2, x + bw - 2, y + bh - 2, FX_PAL[FXP_HOT]);
      for (int i = 0; i < C.capN; i++) fxF2(C.cap[i], x + 7, y + 4 + i * 16, FX_PAL[FXP_INK]);
    }
  }
}

// Run the panel for this frame. In: the band slides in from the left behind a
// slanted leading edge and overshoots. Out: a blade flashes across it and the
// two halves slide apart along the cut.
static void fxCutFrame(uint32_t now, float shx, float shy) {
  FxCut& C = fxCut;
  if (!C.on) return;
  const FxStyleDef& st = FX_STYLE[C.kind];
  uint32_t t = now - C.t0;
  const uint32_t IN = 170, BLADE = st.holdMs, SPLIT = st.holdMs + 60, END = fxCutEnd(C);
  if (t >= END) { C.on = false; FX.cutEnd = now ? now : 1; return; }
  if (st.fill == FXF_NEST)  { fxNestFrame(C, t, now, shx, shy); return; }
  if (st.fill == FXF_MEDAL) { fxMedalFrame(C, t, now, shx, shy); return; }

  // Speed lines across the whole screen behind a big hit, for its first beat.
  if ((st.extras & FXE_BGLINES) && t < 700)
    fxFocusLines(120.0f + shx, C.yc + shy, 118.0f, 46.0f, 52, C.seed + 5, t / 80,
                 FX_PAL[t < 350 ? FXP_LINE : FXP_DIM], 3.0f);

  if (t < SPLIT) {
    float ox = shx, oy = shy;
    if (t < IN) {
      float p = t / (float)IN;
      ox += -300.0f * (1.0f - fxBackOut(p));
      // The leading edge is cut on a slant, not square.
      float ex = 270.0f + ox;
      fxHalfSet(0, ex, C.yc, -0.35f, 1.0f, 1.0f);
      fxHalfN = 1;
      // Motion lines streaming off the back of it.
      fxSpeedLines(ex - 150.0f, C.yc + oy, C.h * 0.6f, 1.0f, C.k, 18, C.seed + 3, (float)t * 2.0f,
                   FX_PAL[FXP_GLOW], FX_PAL[FXP_WHITE]);
    }
    fxBandDraw(C, t, ox, oy);
    fxHalfN = 0;
    if (t >= BLADE) {
      // The blade: a white-hot slash across the band, and a knock.
      if (!C.bladeHit) { C.bladeHit = true; fxImpact(0.22f); }
      float bx = 150.0f + shx, by = C.yc + shy;
      fxStroke(bx - 70.0f, by + 90.0f, bx + 70.0f, by - 90.0f, 1.0f, 6.0f, FX_PAL[FXP_WHITE]);
      fxStroke(bx - 58.0f, by + 74.0f, bx + 58.0f, by - 74.0f, 0.8f, 2.0f, FX_PAL[FXP_GLOW]);
    }
  } else {
    float p = (t - SPLIT) / (float)(END - SPLIT), d = fxCubicIn(p) * 300.0f + p * 12.0f;
    float ang = atanf(C.k), ux = cosf(ang), uy = sinf(ang);
    float bx = 150.0f + shx, by = C.yc + shy, lx = 140.0f, ly = -180.0f;   // the cut, heading up-right
    float L = sqrtf(lx * lx + ly * ly);
    lx /= L; ly /= L;
    // The half behind the cut goes back the way the band came, the half in
    // front carries on; each drops away from the cut a little as it goes. The
    // clip travels with its half -- it is the cut edge, not a window.
    float na = -ly, nb = lx;                       // unit normal, toward the front half
    float oxA = -ux * d - na * p * 9.0f, oyA = -uy * d - nb * p * 9.0f;
    float oxB =  ux * d + na * p * 9.0f, oyB =  uy * d + nb * p * 9.0f;
    fxHalfN = 1;
    fxHalfSet(0, bx + oxA, by + oyA, lx, ly, -1.0f);
    fxBandDraw(C, t, oxA + shx, oyA + shy);
    fxHalfSet(0, bx + oxB, by + oyB, lx, ly, 1.0f);
    fxBandDraw(C, t, oxB + shx, oyB + shy);
    fxHalfN = 0;
  }
}

// ── 12. The nest ─────────────────────────────────────────────────────────────
// Someone has gone down a hatch into the bunker tunnels. Not a panel: a cut
// scene, seven seconds of it, played on the glass while the screen behind
// sinks into the dark from its edges. A fly buzzes about the dashboard
// trailing the dotted path a comic gives a fly, and sets down on a line to
// rub its hands. In the top corner a web spins itself. Something too big for
// the screen reaches up over its bottom edge, feels about the glass and goes
// back down. Small spiders come out of the sides, stopping dead and starting
// again the way they do. The fly finds the web, and everything on the glass
// goes still -- and the web's owner drops out of the top on its thread,
// unfolds, lifts its forelegs, and takes it. Then they are all gone, and down
// in the black at the bottom of the screen eight eyes open, and go out one
// at a time.
//
// It keeps the panel (fxCut) all seven seconds, so nothing cuts in over it
// and its word keeps the sticker pool -- except a catastrophe, which is let
// in at once and simply ends it (the queue in fxCompose).
enum : uint16_t {             // the beats, ms into the scene; FX_STYLE's holdMs is 6800
  FXN_WEB     = 300,          // the web starts to spin itself
  FXN_REACH   = 1100,         // the legs come up over the bottom edge
  FXN_CRAWL   = 2250,         // the first small one comes out
  FXN_CAUGHT  = 3800,         // the web takes the fly
  FXN_DROP    = 4300,         // its owner drops
  FXN_STRIKE  = 5250,         // and takes it
  FXN_SCATTER = 5350,         // everything else bolts
  FXN_CLIMB   = 5800,         // back up the thread
  FXN_EYES    = 6100,         // the eyes
  FXN_FADE    = 6200,         // the web and the words dissolve
};
static const int   FXN_STOPS = 12;
static const float FXN_FLY   = 1.4f;      // the fly's scale

struct FxNest {
  FxWeb    web;
  float    wx[FXN_STOPS], wy[FXN_STOPS];  // the fly's stops,
  uint16_t wt[FXN_STOPS];                 // when it reaches each,
  uint8_t  wn, wland;                     // how many, and the one it sets down on
  float    flyHead;
  float    stuckX, stuckY;                // where the web takes it
  FxSpider sk[3];                         // the small ones:
  float    rx[3][3], ry[3][3];            // their stops,
  uint16_t rf[3][3];                      // how long each freezes at each,
  uint16_t skIn[3];                       // when each comes out,
  float    exX[3], exY[3];                // and where each bolts to
  uint8_t  ri[3], rFrozen[3];
  uint32_t rUntil[3];
  FxSpider big;
  float    bigX, bigHang, bigBite;        // its thread; where it hangs; where it bites
  float    gHx[3], gHy[3], gFx[3], gFy[3], gSd[3];   // under the glass: hips, feet, knee side
  float    eyeX, eyeY, wordX, wordY;
  uint32_t lastT;
  bool     struck;
};
static FxNest* fxNest = nullptr;          // PSRAM

// Where the fly is at scene time t, and what it is doing: 0 flying, 1 set
// down, 2 stuck, 3 taken. Closed-form, so its dotted trail is only the same
// question asked of the last half second.
static uint8_t fxFlyAt(const FxNest& N, float t, float* x, float* y) {
  const int last = N.wn - 1;
  if (t <= N.wt[0]) { *x = N.wx[0]; *y = N.wy[0]; return 0; }
  if (t >= N.wt[last]) {
    // Stuck: flying as hard as it can, and going nowhere.
    if (t >= FXN_STRIKE) { *x = N.stuckX; *y = N.stuckY; return 3; }
    uint32_t h = fxHash2((uint32_t)(t * (1.0f / 45.0f)), N.web.seed + 77U);
    *x = N.stuckX + fxS(h) * 1.6f;
    *y = N.stuckY + fxS(fxHash(h + 1)) * 1.6f;
    return 2;
  }
  int i = 0;
  while (i + 1 < last && t >= N.wt[i + 1]) i++;
  const float t0 = N.wt[i], t1 = N.wt[i + 1];
  const float dx = N.wx[i + 1] - N.wx[i], dy = N.wy[i + 1] - N.wy[i];
  const float dart = fminf((t1 - t0) * 0.55f, 70.0f + sqrtf(dx * dx + dy * dy) * 1.1f);
  const float go = t1 - dart;                       // when it sets off
  if (t >= go) {
    // A dart, bowed to one side, fast away and fast to a stop.
    float p = (t - go) / dart, e = fxSmooth(p);
    float bow = fxS(fxHash2((uint32_t)i, N.web.seed + 5U)) * 1.2f * p * (1.0f - p);
    *x = N.wx[i] + dx * e - dy * bow;
    *y = N.wy[i] + dy * e + dx * bow;
    return 0;
  }
  if (i == N.wland) { *x = N.wx[i]; *y = N.wy[i]; return 1; }
  // Hovering, and it cannot keep still. The wander eases in and out so a
  // dart leaves from where it hangs.
  float amp = 6.0f * fxClampF((t - t0) / 90.0f, 0.0f, 1.0f) * fxClampF((go - t) / 90.0f, 0.0f, 1.0f);
  uint32_t sd = N.web.seed + 11U * (uint32_t)i;
  *x = N.wx[i] + (fxNoise1(t * 0.012f, sd) - 0.5f) * 2.0f * amp;
  *y = N.wy[i] + (fxNoise1(t * 0.012f, sd + 3) - 0.5f) * 2.0f * amp;
  return 0;
}

static void fxNestBegin(FxCut& C) {
  if (!fxNest) { C.on = false; return; }
  FxNest& N = *fxNest;
  memset(&N, 0, sizeof(N));
  FxRng R; R.s = C.seed ^ 0x5B1D3A7U;
  // The web, in the top right corner, fanned from straight down to straight
  // left; it takes the fly on its second ring, low on the fan, on a strand
  // that is not broken.
  fxWebGen(N.web, FX_W + 2.0f, -2.0f, FX_TAU * 0.25f + 0.12f, FX_TAU * 0.5f - 0.10f, 160.0f, R.next());
  {
    const FxWeb& W = N.web;
    int i = 1 + (int)(R.next() % 2);
    if (fxHash2((uint32_t)(16 + i), W.seed) % 13 == 0) i = 3 - i;
    float ax = W.hx + W.ax[i] * W.r[1][i], ay = W.hy + W.ay[i] * W.r[1][i];
    float bx = W.hx + W.ax[i + 1] * W.r[1][i + 1], by = W.hy + W.ay[i + 1] * W.r[1][i + 1];
    fxWebAt(W, ax, ay, bx, by, 1, i, 0.35f + 0.3f * R.u(), &N.stuckX, &N.stuckY);
  }
  // The fly: in from the left, a few darts about the screen well clear of
  // the corner, one stop set down on a line, then the approach and the web.
  float x = -14.0f, y = 140.0f + R.u() * 100.0f;
  uint32_t at = 120;
  N.wx[0] = x; N.wy[0] = y; N.wt[0] = (uint16_t)at; N.wn = 1;
  N.wland = (uint8_t)(3 + R.next() % 2);
  for (int s = 1; s <= 7; s++) {
    float nx = 0, ny = 0;
    for (int tries = 0; tries < 16; tries++) {
      nx = 26.0f + R.u() * 170.0f;
      ny = 96.0f + R.u() * 196.0f;
      float d  = sqrtf((nx - x) * (nx - x) + (ny - y) * (ny - y));
      float hd = sqrtf((nx - N.web.hx) * (nx - N.web.hx) + (ny - N.web.hy) * (ny - N.web.hy));
      if (d > 38.0f && d < 130.0f && hd > 180.0f) break;
    }
    at += (s == N.wland + 1) ? 650u + (uint32_t)R.range(0, 200) : 240u + (uint32_t)R.range(0, 220);
    N.wx[s] = nx; N.wy[s] = ny; N.wt[s] = (uint16_t)at; N.wn++;
    x = nx; y = ny;
  }
  at += 300;
  N.wx[N.wn] = N.stuckX - 55.0f - R.u() * 20.0f; N.wy[N.wn] = N.stuckY + 48.0f + R.u() * 25.0f;
  N.wt[N.wn++] = (uint16_t)at;
  at += 260;
  N.wx[N.wn] = N.stuckX; N.wy[N.wn] = N.stuckY; N.wt[N.wn++] = (uint16_t)at;
  // Fit the flight to the beat: the web takes it at FXN_CAUGHT.
  const float fit = (float)(FXN_CAUGHT - N.wt[0]) / (float)(at - N.wt[0]);
  for (int s = 1; s < N.wn; s++) N.wt[s] = (uint16_t)(N.wt[0] + (N.wt[s] - N.wt[0]) * fit);
  // The small ones, out of the left, the bottom and the right, each stop a
  // little nearer whatever is going on up in the corner.
  for (int i = 0; i < 3; i++) {
    FxSpider& S = N.sk[i];
    S.seed = R.next();
    S.sc = 1.15f + 0.4f * R.u();
    float ex, ey, hd;
    if (i == 0)      { ex = -26.0f;                ey = 170.0f + R.u() * 90.0f; hd = 0.0f; }
    else if (i == 1) { ex = 90.0f + R.u() * 90.0f; ey = FX_H + 26.0f;          hd = -FX_TAU * 0.25f; }
    else             { ex = FX_W + 26.0f;          ey = 190.0f + R.u() * 80.0f; hd = FX_TAU * 0.5f; }
    S.x = ex; S.y = ey; S.head = hd + R.sgn() * 0.3f;
    N.exX[i] = ex + (i == 1 ? R.sgn() * 50.0f : 0.0f);
    N.exY[i] = ey + (i == 1 ? 0.0f : R.sgn() * 50.0f);
    for (int s = 0; s < 3; s++) {
      float f = 0.22f + 0.2f * s;
      N.rx[i][s] = fxClampF(fxLerp(ex, N.stuckX - 30.0f, f) + R.sgn() * 26.0f, 16.0f, FX_W - 16.0f);
      N.ry[i][s] = fxClampF(fxLerp(ey, N.stuckY + 80.0f, f) + R.sgn() * 22.0f, 120.0f, FX_H - 22.0f);
      N.rf[i][s] = (uint16_t)(260 + R.range(0, 480));
    }
    N.skIn[i] = (uint16_t)(FXN_CRAWL + i * 380 + R.range(0, 120));
  }
  // The owner, which hangs head down above the web and bites with its jaws
  // on the fly.
  N.big.seed = R.next();
  N.big.sc = 2.4f;
  N.big.span = 1.35f;
  N.bigX = N.stuckX + R.sgn() * 3.0f;
  N.bigBite = N.stuckY - 6.4f * N.big.sc;
  N.bigHang = N.bigBite - 26.0f;
  // The thing under the glass: three legs, their hips below the bottom edge.
  static const float GH[3][2] = { { 22.0f, 352.0f }, { 60.0f, 366.0f }, { 106.0f, 356.0f } };
  static const float GF[3][2] = { { 12.0f, 262.0f }, { 58.0f, 244.0f }, { 132.0f, 264.0f } };
  const float gx = R.sgn() * 16.0f;
  for (int j = 0; j < 3; j++) {
    N.gHx[j] = GH[j][0] + gx;                    N.gHy[j] = GH[j][1];
    N.gFx[j] = GF[j][0] + gx + R.sgn() * 8.0f;   N.gFy[j] = GF[j][1] + R.sgn() * 8.0f;
    N.gSd[j] = j < 2 ? -1.0f : 1.0f;
  }
  N.eyeX = 62.0f + gx; N.eyeY = 302.0f;
  N.wordX = fxClampF(N.stuckX - 26.0f, 82.0f, 158.0f);
  N.wordY = N.stuckY + 52.0f;
}

// One small one's next step. Out, then stop to stop -- a dash, dead still, a
// dash -- until the web shakes; then nothing moves at all until it is time
// to run.
static void fxNestCrawl(FxNest& N, int i, float t, float dt) {
  FxSpider& S = N.sk[i];
  if (t < N.skIn[i]) return;
  const bool bolt = t >= FXN_SCATTER + i * 60.0f;
  float tx, ty, spd;
  if (bolt) {
    if (!S.on) return;
    tx = N.exX[i]; ty = N.exY[i]; spd = 0.30f;
  } else {
    S.on = true;
    if (t >= FXN_CAUGHT + 80.0f || N.ri[i] >= 3) return;
    if (N.rFrozen[i]) {
      if (t < N.rUntil[i]) return;
      N.rFrozen[i] = 0;
      if (++N.ri[i] >= 3) return;
    }
    tx = N.rx[i][N.ri[i]]; ty = N.ry[i][N.ri[i]];
    spd = 0.15f + 0.07f * fxU(S.seed);
  }
  float dx = tx - S.x, dy = ty - S.y, d = sqrtf(dx * dx + dy * dy);
  if (!bolt && d < 3.0f) { N.rFrozen[i] = 1; N.rUntil[i] = (uint32_t)t + N.rf[i][N.ri[i]]; return; }
  fxSpiderStep(S, tx, ty, d, spd, bolt ? 0.022f : 0.011f, t, dt);   // it turns on the spot to run
  if (bolt && (S.x < -50 || S.x > FX_W + 50 || S.y < -50 || S.y > FX_H + 50)) S.on = false;
}

// The owner, closed-form: dropped on its thread, bouncing once on the silk,
// hanging and turning slowly on it; legs tucked for the fall and opening
// once it hangs; forelegs coming up for the fly; the bite; then reeled back
// up with what it caught.
static void fxNestBig(FxNest& N, float t) {
  FxSpider& B = N.big;
  B.on = t >= FXN_DROP && t < FXN_CLIMB + 900.0f;
  if (!B.on) return;
  B.head = FX_TAU * 0.25f + (fxNoise1(t * 0.0011f, B.seed) - 0.5f) * 0.7f;
  B.x = N.bigX + (fxNoise1(t * 0.0009f, B.seed + 1) - 0.5f) * 4.0f;
  if (t < FXN_DROP + 340.0f)   B.y = fxLerp(-80.0f, N.bigHang, fxBackOut((t - FXN_DROP) / 340.0f));
  else if (t < FXN_STRIKE)     B.y = N.bigHang;
  else if (t < FXN_STRIKE + 70.0f) B.y = fxLerp(N.bigHang, N.bigBite, fxCubicOut((t - FXN_STRIKE) / 70.0f));
  else if (t < FXN_CLIMB)      B.y = N.bigBite;
  else B.y = fxLerp(N.bigBite, -110.0f, fxSmooth(fxClampF((t - FXN_CLIMB) / 520.0f, 0.0f, 1.0f)));
  B.curl  = 1.0f - fxSmooth(fxClampF((t - FXN_DROP - 320.0f) / 380.0f, 0.0f, 1.0f));
  B.reach = fxClampF((t - (FXN_STRIKE - 520.0f)) / 520.0f, 0.0f, 1.0f);
  B.fangs = t > FXN_STRIKE - 260.0f;
  if (t >= FXN_STRIKE) { B.curl = t >= FXN_CLIMB ? 0.65f : 0.5f; B.reach = 0.3f; }
}

// The thing under the glass: three legs up over the bottom edge, each too
// long for the screen. They come up, feel about -- a foot lifting and coming
// down a little further on -- and slide back down.
static void fxNestGiant(const FxNest& N, float t, float shx, float shy) {
  FxLegPose P[3];
  int n = 0;
  for (int j = 0; j < 3; j++) {
    float tj = t - (FXN_REACH + j * 120.0f);
    if (tj <= 0 || tj >= 1950.0f) continue;
    float out = tj < 450.0f ? fxCubicOut(tj / 450.0f)
              : tj > 1500.0f ? 1.0f - fxCubicIn((tj - 1500.0f) / 450.0f) : 1.0f;
    uint32_t sd = N.web.seed + 31U * (uint32_t)j;
    float hx = N.gHx[j], hy = N.gHy[j];
    float fx = N.gFx[j] + (fxNoise1(t * 0.0021f, sd) - 0.5f) * 16.0f;
    float fy = N.gFy[j] + (fxNoise1(t * 0.0021f, sd + 1) - 0.5f) * 12.0f;
    float tap = fxClampF((fxNoise1(t * 0.006f, sd + 2) - 0.66f) * 6.0f, 0.0f, 1.0f);
    float f = (0.18f + 0.82f * out) * (1.0f - 0.1f * tap);
    fx = hx + (fx - hx) * f;
    fy = hy + (fy - hy) * f;
    float d0 = sqrtf((N.gFx[j] - hx) * (N.gFx[j] - hx) + (N.gFy[j] - hy) * (N.gFy[j] - hy));
    P[n++] = fxLegSolve(hx + shx, hy + shy, fx + shx, fy + shy, d0 * 0.58f, d0 * 0.53f, N.gSd[j], 0.0f);
  }
  for (int pass = 0; pass < 3; pass++)
    for (int i = 0; i < n; i++) fxLegDraw(P[i], 7.5f, pass, FXP_RUST, 3.2f, N.web.seed + 77U * (uint32_t)i);
}

// Eight eyes opening in the black it came up out of: the big pair first, the
// rest after in no order, then out again one at a time.
static void fxNestEyes(const FxNest& N, float t, float end, float shx, float shy) {
  const float te = t - FXN_EYES;
  if (te <= 0) return;
  static const float EO[8][3] = { { -3.4f,  0.0f, 2.6f  }, { 3.4f,  0.0f, 2.6f  },
                                  { -9.0f, -1.8f, 1.6f  }, { 9.0f, -1.8f, 1.6f  },
                                  { -2.8f, -5.6f, 1.25f }, { 2.8f, -5.6f, 1.25f },
                                  { -10.0f, -7.0f, 1.35f }, { 10.0f, -7.0f, 1.35f } };
  const float k = 1.35f, cx = N.eyeX + shx, cy = N.eyeY + shy;
  // A ragged pool of black first, so they look out of the dark, not off the UI.
  float pool = fxClampF(te / 200.0f, 0.0f, 1.0f) * fxClampF((end - t) / 250.0f, 0.0f, 1.0f);
  for (int y = (int)(cy - 21.0f); y < (int)(cy + 15.0f); y++) {
    float q = (y + 0.5f - (cy - 3.0f)) / 18.0f;
    float w = 30.0f * sqrtf(fmaxf(0.0f, 1.0f - q * q)) * pool * (0.85f + 0.3f * fxNoise1(y * 0.4f, N.web.seed + 9));
    fxSpan(y, (int)(cx - w), (int)(cx + w), FX_PAL[FXP_INK]);
  }
  for (int e = 0; e < 8; e++) {
    uint32_t h = fxHash2((uint32_t)e, N.web.seed + 13U);
    float on  = e < 2 ? 0.0f : 140.0f + (float)(h % 260);
    float off = end - 520.0f + (float)((h >> 9) % 300) + (e < 2 ? 260.0f : 0.0f);
    float r = EO[e][2] * k * fxClampF((te - on) / 110.0f, 0.0f, 1.0f) * fxClampF((off - t) / 90.0f, 0.0f, 1.0f);
    if (r <= 0.2f) continue;
    float x = cx + EO[e][0] * k, y = cy + EO[e][1] * k;
    fxDisc(x, y, r + 1.2f, FX_PAL[FXP_INK]);
    fxDisc(x, y, r, FX_PAL[FXP_CRIT]);
    fxDisc(x - r * 0.3f, y - r * 0.35f, fmaxf(0.6f, r * 0.42f), FX_PAL[FXP_GLOW]);
    fxPut((int)(x - r * 0.35f), (int)(y - r * 0.4f), FX_PAL[FXP_WHITE]);
  }
}

static void fxNestFrame(FxCut& C, uint32_t t, uint32_t now, float shx, float shy) {
  FxNest& N = *fxNest;
  const float tf = (float)t, end = (float)fxCutEnd(C);
  float dt = t > N.lastT ? (float)(t - N.lastT) : 0.0f;
  if (dt > 100.0f) dt = 100.0f;
  N.lastT = t;
  for (int i = 0; i < 3; i++) fxNestCrawl(N, i, tf, dt);
  fxNestBig(N, tf);
  // The bite: a frame in the negative, a lurch, and the picture tears.
  if (t >= FXN_STRIKE && !N.struck) {
    N.struck = true;
    fxImpact(0.55f);
    fxFlash(FXFL_NEG);
    fxTearBurst(now, 2, 18, 220);
  }
  const uint8_t fade = t > FXN_FADE ? (uint8_t)fxMinI(255, (int)((t - FXN_FADE) * 255 / 700)) : 0;

  float fX, fY;
  const uint8_t fm = fxFlyAt(N, tf, &fX, &fY);
  // The web trembles while the fly fights it, and recoils from the bite.
  float amp = fm == 2 ? 2.0f : 0.0f;
  if (t >= FXN_STRIKE && t < FXN_STRIKE + 450) amp = 3.5f * (1.0f - (tf - FXN_STRIKE) / 450.0f);
  if (fade) { fxStip = fade; fxStipSeed = C.seed + 1; }
  fxWebDraw(N.web, tf - FXN_WEB, N.stuckX, N.stuckY, amp, FX.frame, shx, shy);
  fxStip = 0;

  fxNestGiant(N, tf, shx, shy);
  for (int i = 0; i < 3; i++) fxSpiderDraw(N.sk[i], now, shx, shy, FXP_RUST, false);

  if (fm <= 2) {
    float pX, pY;
    fxFlyAt(N, fmaxf(0.0f, tf - 30.0f), &pX, &pY);
    if (fabsf(fX - pX) + fabsf(fY - pY) > 0.8f) N.flyHead = atan2f(fY - pY, fX - pX);
    if (fm == 2) N.flyHead += fxS(fxHash2(FX.frame, C.seed + 3)) * 0.35f;
    // The comic's dotted flight path: the last half second of where it has
    // been, a dot every few px of the way, the oldest dimmest.
    if (fm == 0) {
      static const uint8_t AGE[4] = { FXP_OK, FXP_RUST, FXP_EMBER, FXP_LINE };
      float lx = fX, ly = fY, acc = 0.0f;
      for (int k = 1; k <= 56; k++) {
        float tt = tf - k * 11.0f, qx, qy;
        if (tt < 0 || fxFlyAt(N, tt, &qx, &qy) != 0) break;
        acc += sqrtf((qx - lx) * (qx - lx) + (qy - ly) * (qy - ly));
        lx = qx; ly = qy;
        if (acc < 5.0f) continue;
        acc = 0.0f;
        fxPut((int)(qx + shx), (int)(qy + shy), FX_PAL[AGE[fxMinI(3, k / 14)]]);
      }
    }
    float jx = 0, jy = 0;
    if (fm == 0) { uint32_t h = fxHash2(FX.frame, C.seed + 5); jx = fxS(h) * 0.6f; jy = fxS(fxHash(h + 1)) * 0.6f; }
    fxFlyDraw(fX + jx + shx, fY + jy + shy, N.flyHead, FXN_FLY, fm, FX.frame, now);
    // Its buzz: two small z's in the air, three big ones in the web.
    if (fm != 1) {
      const int nz = fm == 2 ? 3 : 2;
      const uint32_t boil = (uint32_t)(tf / (fm == 2 ? 50.0f : 90.0f));
      for (int z = 0; z < nz; z++) {
        uint32_t h = fxHash2((uint32_t)z, boil + C.seed);
        float a  = -FX_TAU * 0.25f + ((float)z - (nz - 1) * 0.5f) * 0.9f + fxS(h) * 0.3f;
        float rr = (fm == 2 ? 13.0f : 10.0f) + z * 3.0f + fxU(fxHash(h + 1)) * 3.0f;
        float sz = fm == 2 ? 6.0f + 2.5f * fxU(fxHash(h + 2)) : 3.5f + 1.2f * z;
        float zx = fX + cosf(a) * rr + shx, zy = fY + sinf(a) * rr + shy;
        fxZed(zx + 1.0f, zy + 1.0f, sz, h, FX_PAL[FXP_INK]);
        fxZed(zx, zy, sz, h, FX_PAL[fm == 2 ? FXP_HOT : FXP_OK]);
      }
    }
  }

  // The owner: its thread, then what it is holding, then it.
  const FxSpider& B = N.big;
  if (B.on) {
    const float c = cosf(B.head), s = sinf(B.head), k = B.sc;
    fxLine((int)(N.bigX + shx), -1, (int)(B.x + shx - c * 11.2f * k), (int)(B.y + shy - s * 11.2f * k),
           FX_PAL[FXP_RUST]);
    if (fm == 3) fxBundleDraw(B.x + shx + c * 8.4f * k, B.y + shy + s * 8.4f * k, B.head, FXN_FLY);
    fxSpiderDraw(B, now, shx, shy, FXP_BRICK, true);
  }

  // Its word, slammed in on the bite, dripping.
  if (C.hasWord && t >= FXN_STRIKE) {
    if (fade) { fxStip = fade; fxStipSeed = C.seed + 2; }
    fxWordSlam(C.word, (int32_t)(t - FXN_STRIKE) - 30, N.wordX + shx, N.wordY + shy, 0.0f, 38, t / 80);
    fxStip = 0;
  }
  // The caption: one quiet box, in the corner the web is not in.
  if (C.capN && t >= 250) {
    float p = fxClampF((tf - 250.0f) / 150.0f, 0.0f, 1.0f);
    int bw = C.capW + 14, bh = C.capN * 16 + 8;
    int x = (int)(8 + shx), y = (int)(8 + shy - (1.0f - fxBackOut(p)) * 22.0f);
    if (fade) { fxStip = fade; fxStipSeed = C.seed + 4; }
    fxRect(x + 3, y + 3, x + bw + 3, y + bh + 3, FX_PAL[FXP_INK]);
    fxRect(x, y, x + bw, y + bh, FX_PAL[FXP_INK]);
    fxRect(x + 1, y + 1, x + bw - 1, y + 2, FX_PAL[FXP_EMBER]);
    fxRect(x + 1, y + bh - 2, x + bw - 1, y + bh - 1, FX_PAL[FXP_EMBER]);
    for (int i = 0; i < C.capN; i++) fxF2(C.cap[i], x + 7, y + 4 + i * 16, FX_PAL[FXP_OK]);
    fxStip = 0;
  }
  fxNestEyes(N, tf, end, shx, shy);
}

// One that got out. Someone climbs back up a hatch, and one of the small ones
// from the nest comes up with them: in over one edge of the glass, across it
// stop by stop, dead still at each -- and at the middle stop it turns round
// to face whoever is holding the thing, lifts its forelegs at them, thinks
// better of it, and goes on off the far edge. No caption, no word, no dark
// behind it; it walks over whatever is on the screen, panels included, and
// takes no turn in the queue.
struct FxCrawl {
  bool     on, frozen;
  uint32_t t0, lastT, until;   // scene ms
  FxSpider S;
  float    rx[4], ry[4];       // its stops; the last one is off the far edge
  uint16_t rf[4];              // how long it stays at each
  uint8_t  ri;                 // the stop it is making for
};
static FxCrawl fxCrawl;
static const uint8_t FXC_LOOK = 1;   // the stop it looks round at you from

static void fxCrawlBegin(uint32_t seed, uint32_t now) {
  FxCrawl& K = fxCrawl;
  memset(&K, 0, sizeof(K));
  FxRng R; R.s = seed ^ 0xC4A1154BU;
  K.on = true;
  K.t0 = now;
  K.S.on = true;
  K.S.seed = R.next();
  K.S.sc = 1.75f;
  float ax, ay, bx, by;                // in at one edge, out at the far one
  switch (R.range(0, 3)) {
    case 0:  ax = -30.0f;       ay = 70.0f + R.u() * 180.0f; bx = FX_W + 30.0f; by = 70.0f + R.u() * 180.0f; break;
    case 1:  ax = FX_W + 30.0f; ay = 70.0f + R.u() * 180.0f; bx = -30.0f;       by = 70.0f + R.u() * 180.0f; break;
    case 2:  ax = 40.0f + R.u() * 160.0f; ay = -30.0f;       bx = 40.0f + R.u() * 160.0f; by = FX_H + 30.0f; break;
    default: ax = 40.0f + R.u() * 160.0f; ay = FX_H + 30.0f; bx = 40.0f + R.u() * 160.0f; by = -30.0f;       break;
  }
  K.S.x = ax; K.S.y = ay;
  K.S.head = atan2f(by - ay, bx - ax);
  for (int s = 0; s < 3; s++) {
    K.rx[s] = fxClampF(fxLerp(ax, bx, 0.27f + 0.23f * s) + R.sgn() * 28.0f, 28.0f, FX_W - 28.0f);
    K.ry[s] = fxClampF(fxLerp(ay, by, 0.27f + 0.23f * s) + R.sgn() * 28.0f, 28.0f, FX_H - 28.0f);
    K.rf[s] = (uint16_t)(320 + R.range(0, 420));
  }
  K.rf[FXC_LOOK] = 1150;
  K.rx[3] = bx; K.ry[3] = by;
}

static void fxCrawlFrame(uint32_t now, float shx, float shy) {
  FxCrawl& K = fxCrawl;
  FxSpider& S = K.S;
  const uint32_t tm = now - K.t0;
  const float t = (float)tm, dt = fminf(100.0f, (float)(tm - K.lastT));
  K.lastT = tm;
  if (tm > 9000) { K.on = false; return; }             // it has outstayed any walk it could take
  S.reach = 0.0f;
  if (K.frozen) {
    if (tm >= K.until) { K.frozen = false; K.ri++; }
    else if (K.ri == FXC_LOOK) {
      // Round to face out of the bottom of the glass, forelegs up, and back.
      float lt = t - (float)(K.until - K.rf[FXC_LOOK]);
      fxSpiderStep(S, S.x, S.y + 100.0f, 100.0f, 0.0f, 0.006f, t, dt);
      S.reach = 0.85f * fxClampF((lt - 280.0f) / 220.0f, 0.0f, 1.0f)
                      * fxClampF(((float)K.rf[FXC_LOOK] - lt - 150.0f) / 200.0f, 0.0f, 1.0f);
    }
  }
  if (!K.frozen) {
    if (K.ri > 3) { K.on = false; return; }
    float dx = K.rx[K.ri] - S.x, dy = K.ry[K.ri] - S.y, d = sqrtf(dx * dx + dy * dy);
    if (K.ri < 3 && d < 3.0f) { K.frozen = true; K.until = tm + K.rf[K.ri]; }
    else fxSpiderStep(S, K.rx[K.ri], K.ry[K.ri], d, 0.14f, 0.011f, t, dt);
    if (K.ri == 3 && (S.x < -45 || S.x > FX_W + 45 || S.y < -45 || S.y > FX_H + 45)) { K.on = false; return; }
  }
  fxSpiderDraw(S, now, shx, shy, FXP_RUST, false);
}

// ── 13. The commendation ─────────────────────────────────────────────────────
// Someone made something at a settlement. The Committee has been informed,
// and is moved to issue a decoration.
//
// It is engraved onto the glass the way banknotes, share certificates and
// service awards were engraved: by machine, on a rose engine. So this is the
// one place besides the halftone screen where the maths is allowed to show --
// a lathe does not hide that it turns, and its curves read as money and
// medals, not as geometry. It plots itself in with the stylus still hot at
// its tip: a guilloche frame traced strand by strand, rosettes turned in the
// corners, the letterhead typed, the banner, an engraved sunburst, the order
// star dropped on its ribbon and left swinging, laurels, the citation typed
// out -- the survivor, the thing they made, the Committee's view of it -- a
// signature nobody could read, and then the rubber stamp comes down. A blade
// cuts it away, as it does every panel.
enum : uint16_t {             // the beats, ms into the scene; FX_STYLE's holdMs is 5900
  FXM_LAND   = 280,           // the certificate lands
  FXM_FRAME  = 260,           // the frame starts to plot
  FXM_HEAD   = 560,           // the letterhead types
  FXM_BANNER = 980,
  FXM_SUN    = 1250,
  FXM_MEDAL  = 1450,          // the star drops on its ribbon
  FXM_LAUREL = 1750,
  FXM_TYPE   = 2300,          // the citation
  FXM_SIGN   = 4150,
  FXM_STAMP  = 4700,
};
static const int FXM_ROSE = 40;          // a corner rosette's cell, px
static const int FXM_TYPEMS = 14;        // a keystroke
struct FxMedal {
  uint8_t rose[FXM_ROSE * FXM_ROSE];     // when the lathe first cut each pixel, 1-255; 0 never
  char    to[28];                        // AWARDED TO ...
  char    what[24];                      // the thing, in capitals
  uint8_t first, joke, stamp;            // which opening, which remark, which stamp
  bool    roseReady, stamped;
};
static FxMedal* fxMedal = nullptr;       // PSRAM
static uint8_t  fxMedalTurn = 0;         // rolls, so two in a row do not say the same thing

static const char* const FXM_FIRST[4] = {
  "FOR CONSPICUOUS INGENUITY", "FOR INGENUITY UNDER DURESS",
  "FOR SERVICE BEYOND ALL SENSE", "FOR MAKING DO, CONSPICUOUSLY",
};
static const char* const FXM_JOKE[10][2] = {
  { "FROM PARTS THAT WERE NOT SPARE.", nullptr },
  { "NOBODY ASK WHAT IT USED TO BE.", nullptr },
  { "FINGERS REMAINING: MOST.", nullptr },
  { "THE COMMITTEE IS AS SURPRISED", "AS YOU ARE." },
  { "IT WORKS. DO NOT ASK HOW.", nullptr },
  { "AWARDED IN LIEU OF RATIONS.", nullptr },
  { "THE MEDAL IS NOT EDIBLE.", "WE CHECKED." },
  { "WITH ONLY MINOR SCREAMING.", nullptr },
  { "THE LAST OWNER OF THE PARTS", "HAS NO FURTHER NEED OF THEM." },
  { "MORALE HAS IMPROVED BY ONE.", nullptr },
};
static const char* const FXM_STAMPS[4][2] = {
  { "PENDING", "SURVIVAL" }, { "VOID IF", "DECEASED" }, { "DO NOT", "EAT" }, { "NOT FOR", "RESALE" },
};

// The rose engine, run once: four turns of a seven-lobed rose, each set a
// fraction round from the last so their lobes weave, then a ring round the
// lot. Every pixel keeps the moment the cutter first reached it (the low
// seven bits) and which turn cut it (the top bit), so drawing the rosette up
// to any moment is the lathe caught mid-cut, and alternate turns show as the
// two strands of the weave.
static void fxRoseGen(uint8_t* m) {
  memset(m, 0, FXM_ROSE * FXM_ROSE);
  const float c = FXM_ROSE * 0.5f;
  const int L = 4, N = 900;
  for (int l = 0; l <= L; l++)
    for (int i = 0; i < N; i++) {
      float th = i * (FX_TAU / N);
      float r = l < L ? 10.2f + 5.6f * cosf(7.0f * (th + l * (FX_TAU / (7.0f * L)))) : 18.4f;
      int x = (int)floorf(c + cosf(th) * r), y = (int)floorf(c + sinf(th) * r);
      if ((unsigned)x >= (unsigned)FXM_ROSE || (unsigned)y >= (unsigned)FXM_ROSE) continue;
      uint8_t& p = m[y * FXM_ROSE + x];
      if (!p) p = (uint8_t)((1 + (l * N + i) * 126 / ((L + 1) * N)) | ((l & 1) << 7));
    }
}
// Cut to `prog` (0-127); the last few moments of the cut are still hot.
static void fxRoseDraw(const uint8_t* m, int x0, int y0, int prog) {
  for (int y = 0; y < FXM_ROSE; y++)
    for (int x = 0; x < FXM_ROSE; x++) {
      int v = m[y * FXM_ROSE + x], at = v & 127;
      if (!v || at > prog) continue;
      uint8_t col = (prog < 127 && prog - at < 5) ? FXP_GLOW : ((v & 128) ? FXP_RUST : FXP_OK);
      fxPut(x0 + x, y0 + y, FX_PAL[col]);
    }
}
// A guilloche run: three strands a third of a turn apart crossing and
// recrossing along a straight stretch of the frame, drawn to `upto` px. `s0`
// carries the phase round the corners.
static void fxGuillocheRun(float x0, float y0, float ux, float uy, float len, float upto, float s0) {
  static const uint8_t COL[3] = { FXP_RUST, FXP_BRICK, FXP_OK };
  const float nx = -uy, ny = ux, end = fminf(len, upto);
  if (end <= 0) return;
  for (int k = 0; k < 3; k++) {
    float px = 0, py = 0;
    for (float s = 0; s <= end; s += 2.0f) {
      float o = 3.6f * sinf((s + s0) * (FX_TAU / 12.0f) + k * (FX_TAU / 3.0f));
      float x = x0 + ux * s + nx * o, y = y0 + uy * s + ny * o;
      if (s > 0) fxLine((int)px, (int)py, (int)x, (int)y, FX_PAL[COL[k]]);
      px = x; py = y;
    }
  }
}
// Typewriter: the first `n` characters of `s`, centred as if it were all
// there, `adv` px a letter.
static void fxTyped(const char* s, int cx, int y, int n, int adv, uint16_t col) {
  int len = (int)strlen(s), x = cx - len * adv / 2;
  char g[2] = { 0, 0 };
  for (int i = 0; i < len && i < n; i++) { g[0] = s[i]; fxGlcd(g, x + i * adv, y, 1, col); }
}
static inline void fxRot(float cx, float cy, float c, float s, float x, float y, float* X, float* Y) {
  *X = cx + c * x - s * y;
  *Y = cy + s * x + c * y;
}

// The order star: sixteen rays, long and short, each cut into two facets --
// the one that faces the light (above and left) white-hot, the other burnt
// -- the way a breast star's cut metal throws light.
static void fxOrderStar(float cx, float cy, float rot) {
  for (int pass = 0; pass < 2; pass++)
    for (int order = 0; order < 2; order++)
      for (int i = 1 - order; i < 16; i += 2) {
        float a = rot + i * (FX_TAU / 16.0f), len = (i & 1) ? 21.0f : 30.0f, half = (i & 1) ? 4.4f : 5.6f;
        float ux = cosf(a), uy = sinf(a), nx = -uy, ny = ux;
        float g = pass ? 0.0f : 1.5f;
        float tx = cx + ux * (len + g), ty = cy + uy * (len + g);
        float lx = cx + nx * (half + g), ly = cy + ny * (half + g);
        float rx = cx - nx * (half + g), ry = cy - ny * (half + g);
        if (!pass) {
          fxTri(cx, cy, tx, ty, lx, ly, FX_PAL[FXP_INK]);
          fxTri(cx, cy, tx, ty, rx, ry, FX_PAL[FXP_INK]);
          continue;
        }
        float lit = -(nx + ny) * 0.707f;       // the +n facet against the light
        uint8_t fa = lit > 0.25f ? FXP_GLOW : (lit < -0.25f ? FXP_BRICK : FXP_HDR);
        uint8_t fb = lit > 0.25f ? FXP_BRICK : (lit < -0.25f ? FXP_GLOW : FXP_HDR);
        fxTri(cx, cy, tx, ty, lx, ly, FX_PAL[fa]);
        fxTri(cx, cy, tx, ty, rx, ry, FX_PAL[fb]);
      }
}
// The medallion: an engraved rose and a ring, two crossed wrenches, and the
// Committee's own skull on top of them.
static void fxMedallion(float cx, float cy, float rot) {
  const float c = cosf(rot), s = sinf(rot);
  fxDisc(cx, cy, 15.5f, FX_PAL[FXP_INK]);
  fxDisc(cx, cy, 14.0f, FX_PAL[FXP_CRIT]);
  float px = 0, py = 0;
  for (int i = 0; i <= 66; i++) {
    float th = i * (FX_TAU / 66.0f), r = 9.2f + 2.2f * cosf(11.0f * th);
    float x = cx + cosf(th + rot) * r, y = cy + sinf(th + rot) * r;
    if (i) fxLine((int)px, (int)py, (int)x, (int)y, FX_PAL[FXP_HOT]);
    px = x; py = y;
  }
  for (int i = 0; i <= 48; i++) {
    float th = i * (FX_TAU / 48.0f), x = cx + cosf(th) * 12.6f, y = cy + sinf(th) * 12.6f;
    if (i) fxLine((int)px, (int)py, (int)x, (int)y, FX_PAL[FXP_GLOW]);
    px = x; py = y;
  }
  for (int sd = -1; sd <= 1; sd += 2) {
    float ax, ay, bx, by;
    fxRot(cx, cy, c, s, -8.0f * sd, -8.0f, &ax, &ay);
    fxRot(cx, cy, c, s,  8.0f * sd,  8.0f, &bx, &by);
    fxStroke(ax, ay, bx, by, 3.4f, 3.4f, FX_PAL[FXP_INK]);
    fxStroke(ax, ay, bx, by, 2.0f, 2.0f, FX_PAL[FXP_GLOW]);
    for (int e = 0; e < 2; e++) {
      float hx = e ? bx : ax, hy = e ? by : ay, ox = (e ? 1.0f : -1.0f) * sd, oy = e ? 1.0f : -1.0f;
      float nx, ny;
      fxRot(0, 0, c, s, ox * 1.3f, oy * 1.3f, &nx, &ny);
      fxDisc(hx, hy, 3.2f, FX_PAL[FXP_INK]);
      fxDisc(hx, hy, 2.3f, FX_PAL[FXP_GLOW]);
      fxDisc(hx + nx, hy + ny, 1.2f, FX_PAL[FXP_CRIT]);    // the jaw of the wrench
    }
  }
  float X, Y, qx[4], qy[4];
  fxRot(cx, cy, c, s, 0.0f, -1.6f, &X, &Y);
  fxBlob(X, Y, c, s, 6.2f, 5.7f, 3, FX_PAL[FXP_INK]);
  fxBlob(X, Y, c, s, 5.0f, 4.6f, 3, FX_PAL[FXP_WHITE]);
  static const float JAW[4][2] = { { -3.0f, 1.6f }, { 3.0f, 1.6f }, { 2.6f, 5.0f }, { -2.6f, 5.0f } };
  for (int i = 0; i < 4; i++) fxRot(cx, cy, c, s, JAW[i][0], JAW[i][1], &qx[i], &qy[i]);
  fxPoly(qx, qy, 4, FX_PAL[FXP_WHITE]);
  for (int sd = -1; sd <= 1; sd += 2) {
    fxRot(cx, cy, c, s, 2.1f * sd, -1.4f, &X, &Y);
    fxDisc(X, Y, 1.6f, FX_PAL[FXP_INK]);
  }
  float nx0, ny0, nx1, ny1, nx2, ny2;
  fxRot(cx, cy, c, s, 0.0f, 0.4f, &nx0, &ny0);
  fxRot(cx, cy, c, s, -0.9f, 1.9f, &nx1, &ny1);
  fxRot(cx, cy, c, s, 0.9f, 1.9f, &nx2, &ny2);
  fxTri(nx0, ny0, nx1, ny1, nx2, ny2, FX_PAL[FXP_INK]);
  for (int k = -1; k <= 1; k++) {
    fxRot(cx, cy, c, s, k * 1.3f, 2.8f, &nx0, &ny0);
    fxRot(cx, cy, c, s, k * 1.3f, 4.8f, &nx1, &ny1);
    fxLine((int)nx0, (int)ny0, (int)nx1, (int)ny1, FX_PAL[FXP_INK]);
  }
}
// A comic sparkle: two thin diamonds crossed, for something that shines.
static void fxSparkle(float x, float y, float r) {
  if (r < 0.6f) return;
  fxTri(x - r, y, x + r, y, x, y - r * 0.18f, FX_PAL[FXP_WHITE]);
  fxTri(x - r, y, x + r, y, x, y + r * 0.18f, FX_PAL[FXP_WHITE]);
  fxTri(x, y - r, x, y + r, x - r * 0.18f, y, FX_PAL[FXP_WHITE]);
  fxTri(x, y - r, x, y + r, x + r * 0.18f, y, FX_PAL[FXP_WHITE]);
}
// A laurel branch round an arc from a0 to a1: leaf pairs raked along the
// stem, shrinking toward the tip, each sprouting when the growth reaches it.
static void fxLaurel(float cx, float cy, float R, float a0, float a1, float grow) {
  if (grow <= 0) return;
  const int N = 9;
  float px = cx + cosf(a0) * R, py = cy + sinf(a0) * R;
  for (int i = 1; i <= 24 && i <= (int)(grow * 24.0f); i++) {
    float a = a0 + (a1 - a0) * i / 24.0f, x = cx + cosf(a) * R, y = cy + sinf(a) * R;
    fxStroke(px, py, x, y, 2.2f, 1.6f, FX_PAL[FXP_RUST]);
    px = x; py = y;
  }
  const float dir = a1 > a0 ? 1.0f : -1.0f;
  for (int i = 0; i < N; i++) {
    float u = (i + 0.6f) / N, sprout = fxClampF((grow - u) * 7.0f, 0.0f, 1.0f);
    if (sprout <= 0) break;
    float a = a0 + (a1 - a0) * u, x = cx + cosf(a) * R, y = cy + sinf(a) * R;
    float ta = a + dir * FX_TAU * 0.25f, len = fxLerp(9.0f, 5.0f, u) * sprout, wid = len * 0.36f;
    for (int sd = -1; sd <= 1; sd += 2) {
      float la = ta + sd * 0.62f, lx = cosf(la), ly = sinf(la), nx = -ly, ny = lx;
      float bx = x + cosf(a) * sd * 1.2f, by = y + sinf(a) * sd * 1.2f;   // either side of the stem
      float xs[6] = { bx, bx + lx * len * 0.35f + nx * wid, bx + lx * len * 0.75f + nx * wid * 0.8f,
                      bx + lx * len, bx + lx * len * 0.75f - nx * wid * 0.8f, bx + lx * len * 0.35f - nx * wid };
      float ys[6] = { by, by + ly * len * 0.35f + ny * wid, by + ly * len * 0.75f + ny * wid * 0.8f,
                      by + ly * len, by + ly * len * 0.75f - ny * wid * 0.8f, by + ly * len * 0.35f - ny * wid };
      fxPoly(xs, ys, 6, FX_PAL[FXP_OK]);
      fxLine((int)bx, (int)by, (int)(bx + lx * len * 0.8f), (int)(by + ly * len * 0.8f), FX_PAL[FXP_RUST]);
    }
  }
}
// The Committee's hand. A pen runs right, one stroke a letter, every letter
// leaving and rejoining the line so it all runs on joined up. What each
// letter is gets dealt from a hash: a small loop, a tall one, a hump that
// never doubles back, a tail below the line -- and first a great looped
// capital. It leans, and ends in a flourish back under the whole name.
static void fxSignature(float x0, float y0, float w, float prog, uint32_t seed) {
  enum { CAP, SMALL, TALL, HUMP, TAIL };
  static const float HT[5] = { 13.0f, 3.6f, 9.5f, 4.2f, -7.0f };   // height (negative: below)
  static const float BK[5] = { 3.8f, 2.0f, 1.6f, 0.0f, 1.8f };     // how far it doubles back
  static const float WD[5] = { 1.9f, 1.0f, 0.9f, 1.3f, 1.0f };     // width, in letters
  const int LET = 8;
  uint8_t kind[LET];
  float at[LET + 1];
  at[0] = 0.0f;
  for (int k = 0; k < LET; k++) {
    uint32_t h = fxHash2((uint32_t)k, seed) % 20;
    kind[k] = k == 0 ? CAP : (h < 8 ? SMALL : (h < 12 ? TALL : (h < 17 ? HUMP : TAIL)));
    at[k + 1] = at[k] + WD[kind[k]];
  }
  const int N = 180;
  float px = x0, py = y0;
  for (int i = 1; i <= (int)(prog * N); i++) {
    float u = i / (float)N, x, y;
    if (u < 0.84f) {
      float pos = u / 0.84f * at[LET];
      int k = 0;
      while (k + 1 < LET && pos >= at[k + 1]) k++;
      float f = (pos - at[k]) / WD[kind[k]], lift = 0.5f * (1.0f - cosf(f * FX_TAU));
      y = y0 - HT[kind[k]] * lift;
      x = x0 + pos / at[LET] * w - BK[kind[k]] * sinf(f * FX_TAU) + (y0 - y) * 0.36f;
    } else {
      float v = (u - 0.84f) / 0.16f;
      x = x0 + w * (1.04f - v * 1.12f);
      y = y0 + 3.5f + 10.0f * v * (1.0f - v) - v * 2.5f;
    }
    fxLine((int)px, (int)py, (int)x, (int)y, FX_PAL[FXP_GLOW]);
    px = x; py = y;
  }
}
// The rubber stamp, turned `rot` and struck at `scl`: a double rule and two
// lines of block capitals, found by mapping each screen pixel back onto the
// stamp's face, so a turned stamp has no holes. The pad was not evenly inked:
// value noise leaves it dry in patches.
static void fxStampDraw(const char* l1, const char* l2, float cx, float cy, float rot, float scl,
                        uint32_t seed, bool wet) {
  const int ADV = 11;
  const int n1 = (int)strlen(l1), n2 = (int)strlen(l2);
  const float W = fxMaxI(n1, n2) * ADV + 16.0f, H = 48.0f;
  const float c = cosf(rot), s = sinf(rot), rad = 0.5f * sqrtf(W * W + H * H) * scl + 2.0f;
  const uint8_t* T = fxGlcdTable();
  const uint16_t col = FX_PAL[FXP_BLOOD], hot = FX_PAL[FXP_CRIT];
  for (int y = (int)(cy - rad); y < (int)(cy + rad); y++)
    for (int x = (int)(cx - rad); x < (int)(cx + rad); x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      float u = (c * dx + s * dy) / scl + W * 0.5f, v = (-s * dx + c * dy) / scl + H * 0.5f;
      if (u < 0 || v < 0 || u >= W || v >= H) continue;
      float e = fminf(fminf(u, W - u), fminf(v, H - v));
      bool ink = e < 2.4f || (e >= 4.6f && e < 5.8f);
      if (!ink && e >= 5.8f) {
        const bool second = v >= H * 0.5f;
        const char* L = second ? l2 : l1;
        const int n = second ? n2 : n1;
        float lx = (W - n * ADV) * 0.5f + 1.0f, ly = second ? 26.0f : 8.0f;
        int ci = (int)floorf((u - lx) / ADV);
        if (ci >= 0 && ci < n) {
          int gx = (int)((u - lx - ci * ADV) * 0.5f), gy = (int)((v - ly) * 0.5f);
          if (gx >= 0 && gx < 5 && gy >= 0 && gy < 8) ink = (T[(uint8_t)L[ci] * 5 + gx] >> gy) & 1;
        }
      }
      if (!ink) continue;
      if (!wet && fxNoise2(u * 0.3f, v * 0.3f, seed) < 0.24f) continue;
      fxPut(x, y, fxNoise2(u * 0.45f, v * 0.45f, seed + 9) > 0.68f ? hot : col);   // pressed hardest
    }
}

static void fxMedalBegin(FxCut& C, const FxCueReq& q) {
  if (!fxMedal) { C.on = false; return; }
  FxMedal& M = *fxMedal;
  if (!M.roseReady) { fxRoseGen(M.rose); M.roseReady = true; }
  char nm[16];
  int i = 0;
  for (; C.name[i] && i < 15; i++) nm[i] = (char)toupper((unsigned char)C.name[i]);
  nm[i] = 0;
  snprintf(M.to, sizeof(M.to), "AWARDED TO %s", nm[0] ? nm : "PERSONS UNKNOWN");
  for (i = 0; q.cap[i] && i < (int)sizeof(M.what) - 1; i++) M.what[i] = (char)toupper((unsigned char)q.cap[i]);
  M.what[i] = 0;
  if (!M.what[0]) snprintf(M.what, sizeof(M.what), "SOMETHING");
  const uint8_t turn = fxMedalTurn++;
  M.first = (uint8_t)(turn % 4);
  M.joke  = (uint8_t)(turn % 10);
  M.stamp = (uint8_t)((turn + turn / 4) % 4);
  M.stamped = false;
}

// The certificate at time t, slid by (ox, oy) -- the exit draws it twice.
static void fxMedalDraw(FxCut& C, uint32_t t, float ox, float oy) {
  FxMedal& M = *fxMedal;
  const float tf = (float)t;
  oy -= 330.0f * (1.0f - fxBackOut(fxClampF(tf / FXM_LAND, 0.0f, 1.0f)));
  const float cx = 120.0f + ox;
  // The sheet, and its two plain rules.
  fxRect((int)(ox + 3), (int)(oy + 3), (int)(ox + 237), (int)(oy + 317), FX_PAL[FXP_INK]);
  for (int r = 0; r < 2; r++) {
    int a = r ? 21 : 4, x0 = (int)ox + a, y0 = (int)oy + a, x1 = (int)ox + 240 - a, y1 = (int)oy + 320 - a;
    uint16_t col = FX_PAL[FXP_RUST];
    fxRect(x0, y0, x1, y0 + 1, col); fxRect(x0, y1 - 1, x1, y1, col);
    fxRect(x0, y0, x0 + 1, y1, col); fxRect(x1 - 1, y0, x1, y1, col);
  }
  // The frame plots itself clockwise from the top left, the cutter's tip hot.
  const float bp = (tf - FXM_FRAME) * 0.66f;
  if (bp > 0) {
    fxGuillocheRun(ox + 40.0f,  oy + 12.5f,  1.0f,  0.0f, 160.0f, bp,          0.0f);
    fxGuillocheRun(ox + 227.5f, oy + 40.0f,  0.0f,  1.0f, 240.0f, bp - 160.0f, 160.0f);
    fxGuillocheRun(ox + 200.0f, oy + 307.5f, -1.0f, 0.0f, 160.0f, bp - 400.0f, 400.0f);
    fxGuillocheRun(ox + 12.5f,  oy + 280.0f, 0.0f, -1.0f, 240.0f, bp - 560.0f, 560.0f);
    if (bp < 800.0f) {
      float s = bp, x, y;
      if (s < 160)      { x = ox + 40 + s;          y = oy + 12.5f; }
      else if (s < 400) { x = ox + 227.5f;          y = oy + 40 + (s - 160); }
      else if (s < 560) { x = ox + 200 - (s - 400); y = oy + 307.5f; }
      else              { x = ox + 12.5f;           y = oy + 280 - (s - 560); }
      fxDisc(x, y, 1.6f, FX_PAL[FXP_WHITE]);
    }
  }
  const int rp = (int)fxClampF((tf - FXM_FRAME - 100.0f) * (127.0f / 1000.0f), 0.0f, 127.0f);
  if (rp > 0) {
    int L = (int)ox + 1, R = (int)ox + 199, T = (int)oy + 1, B = (int)oy + 279;
    fxRoseDraw(M.rose, L, T, rp); fxRoseDraw(M.rose, R, T, rp);
    fxRoseDraw(M.rose, L, B, rp); fxRoseDraw(M.rose, R, B, rp);
  }
  // The letterhead.
  if (tf > FXM_HEAD) {
    static const char* const H1 = "THE COMMITTEE FOR", *const H2 = "DECORATIONS & DISPOSALS";
    int n = (int)((tf - FXM_HEAD) / 20.0f);
    fxTyped(H1, (int)cx, (int)(oy + 27), n, 7, FX_PAL[FXP_OK]);
    fxTyped(H2, (int)cx, (int)(oy + 37), n - (int)strlen(H1), 7, FX_PAL[FXP_OK]);
  }
  // Sunburst: engraved rays over the medal, fanned round its top.
  const float my = oy + 128.0f;
  const float sun = fxClampF((tf - FXM_SUN) / 450.0f, 0.0f, 1.0f);
  if (sun > 0)
    for (int i = 0; i < 72; i++) {
      float a = i * (FX_TAU / 72.0f), sa = sinf(a);
      if (sa > 0.3f) continue;
      float r0 = 37.0f, r1 = r0 + ((i & 1) ? 16.0f : 26.0f) * fxCubicOut(sun);
      fxLine((int)(cx + cosf(a) * r0), (int)(my + sa * r0), (int)(cx + cosf(a) * r1), (int)(my + sa * r1),
             FX_PAL[(i & 3) ? FXP_LINE : FXP_EMBER]);
    }
  // Laurels round the foot of it.
  const float gl = fxClampF((tf - FXM_LAUREL) / 600.0f, 0.0f, 1.0f);
  fxLaurel(cx, my + 6.0f, 41.0f, FX_TAU * 0.29f, FX_TAU * 0.58f, gl);
  fxLaurel(cx, my + 6.0f, 41.0f, FX_TAU * 0.21f, FX_TAU * -0.08f, gl);
  // The star, dropped on its ribbon from behind the banner, left swinging.
  const float tm = tf - FXM_MEDAL;
  if (tm > 0) {
    const float px = cx, py = oy + 70.0f;
    const float drop = fxBackOut(fxClampF(tm / 300.0f, 0.0f, 1.0f));
    const float sw = 0.34f * expf(-tm / 650.0f) * cosf(tm * 0.0115f);
    const float ux = sinf(sw), uy = cosf(sw), nx = uy, ny = -ux;
    const float Lr = 30.0f * drop, mx = px + ux * (Lr + 28.0f), mmy = py + uy * (Lr + 28.0f);
    const float rx = px + ux * Lr, ry = py + uy * Lr;
    // The ribbon: its stripes, and the one in the colour of what it costs.
    static const float    SX[8] = { -12.0f, -10.0f, -6.0f, -3.0f, 3.0f, 6.0f, 10.0f, 12.0f };
    static const uint8_t  SC[7] = { FXP_EMBER, FXP_HDR, FXP_BLOOD, FXP_GLOW, FXP_BLOOD, FXP_HDR, FXP_EMBER };
    float qx[4] = { px + nx * -13.5f, px + nx * 13.5f, rx + nx * 13.5f, rx + nx * -13.5f };
    float qy[4] = { py + ny * -13.5f, py + ny * 13.5f, ry + ny * 13.5f, ry + ny * -13.5f };
    fxPoly(qx, qy, 4, FX_PAL[FXP_INK]);
    for (int k = 0; k < 7; k++) {
      float sx[4] = { px + nx * SX[k], px + nx * SX[k + 1], rx + nx * SX[k + 1], rx + nx * SX[k] };
      float sy[4] = { py + ny * SX[k], py + ny * SX[k + 1], ry + ny * SX[k + 1], ry + ny * SX[k] };
      fxPoly(sx, sy, 4, FX_PAL[SC[k]]);
    }
    fxDisc(rx, ry + 1.0f, 3.6f, FX_PAL[FXP_INK]);
    fxDisc(rx, ry + 1.0f, 2.6f, FX_PAL[FXP_HOT]);
    fxOrderStar(mx, mmy, -sw);
    fxMedallion(mx, mmy, -sw);
    // Now and then the light finds a point of it.
    if (tm > 500.0f) {
      uint32_t slot = (uint32_t)(tm / 380.0f), h = fxHash2(slot, C.seed + 7);
      float p = (tm - slot * 380.0f) / 220.0f;
      if (p < 1.0f) {
        float a = -sw + (float)((h % 8) * 2) * (FX_TAU / 16.0f);
        fxSparkle(mx + cosf(a) * 27.0f, mmy + sinf(a) * 27.0f, 6.0f * (1.0f - fabsf(2.0f * p - 1.0f)));
      }
    }
  }
  // The banner, unrolling from its middle.
  const float ban = fxClampF((tf - FXM_BANNER) / 260.0f, 0.0f, 1.0f);
  if (ban > 0) {
    const float by = oy + 59.0f, hw = 81.0f * fxCubicOut(ban);
    if (ban >= 1.0f)
      for (int sd = -1; sd <= 1; sd += 2) {
        // The tails, folded behind, with a V cut out of each.
        float e = cx + sd * hw;
        fxRect((int)fminf(e, e + sd * 18.0f), (int)(by - 5), (int)fmaxf(e, e + sd * 18.0f), (int)(by + 16),
               FX_PAL[FXP_FLAME]);
        fxTri(e + sd * 19.0f, by - 6, e + sd * 19.0f, by + 17, e + sd * 11.0f, by + 5.5f, FX_PAL[FXP_INK]);
        fxTri(e, by + 11, e, by + 17, e - sd * 6.0f, by + 11, FX_PAL[FXP_BRICK]);
      }
    fxRect((int)(cx - hw), (int)(by - 11), (int)(cx + hw), (int)(by + 11), FX_PAL[FXP_INK]);
    fxRect((int)(cx - hw + 2), (int)(by - 9), (int)(cx + hw - 2), (int)(by + 9), FX_PAL[FXP_HOT]);
    fxRect((int)(cx - hw + 2), (int)(by - 9), (int)(cx + hw - 2), (int)(by - 8), FX_PAL[FXP_WHITE]);
    static const char* const TITLE = "COMMENDATION";
    const int tw = 12 * 12;
    for (int k = 0; k < 12; k++) {
      int x = (int)cx - tw / 2 + k * 12;
      if (x < cx - hw + 2 || x + 10 > cx + hw - 2) continue;
      char g[2] = { TITLE[k], 0 };
      fxGlcd(g, x, (int)(by - 7), 2, FX_PAL[FXP_INK]);
    }
  }
  // The citation, typed.
  if (tf > FXM_TYPE) {
    int n = (int)((tf - FXM_TYPE) / FXM_TYPEMS);
    const char* const* J = FXM_JOKE[M.joke];
    const char* L[5] = { M.to, FXM_FIRST[M.first], "IN THE MANUFACTURE OF ONE (1)", J[0], J[1] };
    static const int LY[5] = { 180, 191, 201, 229, 239 };
    int caretX = -1, caretY = 0;
    for (int k = 0; k < 5 && n > 0; k++) {
      if (k == 3) {
        // What it was, set bigger, under the line that promised it.
        int len = (int)strlen(M.what), w = fxF2Width(M.what), m = fxMinI(n, len);
        char part[24];
        memcpy(part, M.what, (size_t)m); part[m] = 0;
        fxF2(part, (int)(cx - w * 0.5f), (int)(oy + 210), FX_PAL[FXP_GLOW]);
        if (n <= len) { caretX = (int)(cx - w * 0.5f) + fxF2Width(part); caretY = (int)(oy + 219); }
        n -= len;
        if (n <= 0) break;
      }
      if (!L[k]) continue;
      int len = (int)strlen(L[k]);
      fxTyped(L[k], (int)cx, (int)(oy + LY[k]), n, 6, FX_PAL[k == 0 ? FXP_GLOW : FXP_HDR]);
      if (n <= len) { caretX = (int)cx - len * 3 + n * 6; caretY = (int)(oy + LY[k]); }
      n -= len;
    }
    if (caretX >= 0 && ((t / 120) & 1)) fxGlcd("_", caretX, caretY, 1, FX_PAL[FXP_GLOW]);
  }
  // Signed, for the Committee.
  if (tf > FXM_SIGN) {
    fxSignature(ox + 50.0f, oy + 274.0f, 66.0f, fxClampF((tf - FXM_SIGN) / 450.0f, 0.0f, 1.0f), C.seed + 3);
    fxRect((int)(ox + 44), (int)(oy + 283), (int)(ox + 126), (int)(oy + 284), FX_PAL[FXP_RUST]);
    fxGlcd("FOR THE COMMITTEE", (int)(ox + 44), (int)(oy + 287), 1, FX_PAL[FXP_RUST]);
  }
  // And then the stamp.
  if (tf > FXM_STAMP) {
    float ts = tf - FXM_STAMP, scl = ts < 90.0f ? fxLerp(1.55f, 1.0f, fxCubicOut(ts / 90.0f)) : 1.0f;
    const char* const* S = FXM_STAMPS[M.stamp];
    fxStampDraw(S[0], S[1], ox + 168.0f, oy + 262.0f, -0.22f, scl, C.seed + 5, ts < 140.0f);
    if (C.hasWord) {
      uint8_t fade = ts > 650.0f ? (uint8_t)fxMinI(255, (int)((ts - 650.0f) * 255.0f / 300.0f)) : 0;
      if (fade < 255) {
        if (fade) { fxStip = fade; fxStipSeed = C.seed + 6; }
        fxWordSlam(C.word, (int32_t)ts - 60, ox + 76.0f, oy + 262.0f, 0.0f, 40, t / 80);
        fxStip = 0;
      }
    }
  }
}

// The whole of it, and the blade that takes it away: the same cut and the
// same two halves sliding apart along it that end every panel.
static void fxMedalFrame(FxCut& C, uint32_t t, uint32_t now, float shx, float shy) {
  FxMedal& M = *fxMedal;
  const FxStyleDef& st = FX_STYLE[C.kind];
  const uint32_t BLADE = st.holdMs, SPLIT = st.holdMs + 60, END = fxCutEnd(C);
  if (t >= FXM_STAMP + 90 && !M.stamped) {
    M.stamped = true;
    fxImpact(0.45f);
    fxTearBurst(now, 1, 10, 140);
  }
  if (t < SPLIT) {
    fxMedalDraw(C, t, shx, shy);
    if (t >= BLADE) {
      if (!C.bladeHit) { C.bladeHit = true; fxImpact(0.22f); }
      float bx = 150.0f + shx, by = 160.0f + shy;
      fxStroke(bx - 90.0f, by + 116.0f, bx + 90.0f, by - 116.0f, 1.0f, 7.0f, FX_PAL[FXP_WHITE]);
      fxStroke(bx - 74.0f, by + 95.0f, bx + 74.0f, by - 95.0f, 0.8f, 2.4f, FX_PAL[FXP_GLOW]);
    }
    return;
  }
  float p = (t - SPLIT) / (float)(END - SPLIT), d = fxCubicIn(p) * 300.0f + p * 12.0f;
  float bx = 150.0f + shx, by = 160.0f + shy, lx = 180.0f, ly = -232.0f, L = sqrtf(lx * lx + ly * ly);
  lx /= L; ly /= L;
  const float na = -ly, nb = lx;
  float oxA = -d - na * p * 9.0f, oyA = -nb * p * 9.0f, oxB = d + na * p * 9.0f, oyB = nb * p * 9.0f;
  fxHalfN = 1;
  fxHalfSet(0, bx + oxA, by + oyA, lx, ly, -1.0f);
  fxMedalDraw(C, t, oxA + shx, oyA + shy);
  fxHalfSet(0, bx + oxB, by + oyB, lx, ly, 1.0f);
  fxMedalDraw(C, t, oxB + shx, oyB + shy);
  fxHalfN = 0;
}

// ── 14. Madness ──────────────────────────────────────────────────────────────
// The ambient layer. None of it is scripted; it is scheduled off `madness`
// with every interval jittered, so it never falls into a beat you could tap.
static const char* const FX_WHISPER[7][3] = {
  { "KEEP WALKING",  "WE SEE YOU",   "STAY"      },   // calm
  { "IT KNOWS YOU",  "IT'S CLOSER",  "DON'T TURN" },  // the Doom
  { "EAT",           "HUNGRY",       "FEED IT"   },   // hunger
  { "WATER",         "SO DRY",       "DRINK IT"  },   // thirst
  { "YOUR TEETH HUM", "GLOWING",     "IT'S IN YOU" }, // radiation
  { "LIE DOWN",      "BLEEDING",     "SO TIRED"  },   // wounds, attrition
  { "TOO LATE",      "RUN",          "THE CLOCK" },   // the threat clock
};

static uint32_t fxJitter(uint32_t base, uint32_t seed) {
  return (uint32_t)(base * (0.6f + 0.8f * fxU(fxHash(seed))));
}

static void fxAmbient(uint32_t now) {
  const uint8_t m = FX.madness;
  const bool full = FX.level >= 2;
  // Glitches: tears, slips, dropouts, a dim beat. Rare when calm, constant
  // at the top.
  if ((int32_t)(now - FX.nextGlitch) >= 0) {
    uint32_t h = fxHash(now ^ 0xA511E9B3U);
    uint8_t kind = (uint8_t)(h % 5);
    if (m < 60 && kind == 1) kind = 0;
    FX.glitchKind = kind;
    uint16_t dur = (uint16_t)(60 + (h >> 8) % 160);
    FX.glitchUntil = now + dur;
    if (kind == 0) fxTearBurst(now, 1 + (m > 120) + (m > 200), 6 + m / 10, dur);
    if (kind == 1) fxVroll(now, 0, dur + 120);
    FX.nextGlitch = now + fxJitter((uint32_t)fxMaxI(1700, 16000 - m * 56), h);
  }
  // The subliminal: two frames, a word, in the negative.
  if (full && m >= 170) {
    if (!FX.subReady && (int32_t)(now - FX.nextSub) >= -700 && !fxCut.on) {
      uint32_t h = fxHash(now ^ 0x51AB1E5U);
      const char* w = FX_WHISPER[FX.cause < 7 ? FX.cause : 0][h % 3];
      FX.subReady = fxSubWord && fxBuildWord(*fxSubWord, w, 190.0f, 70.0f, 1.0f, FX_INK_BLOOD, false, true, h);
      FX.subAt = FX.nextSub;
    }
    if (FX.subReady && (int32_t)(now - FX.subAt) >= 0 && !fxCut.on) {
      FX.subFrames = 2;
      FX.subReady  = false;
      FX.nextSub   = now + fxJitter((uint32_t)fxMaxI(5000, 14000 - (m - 170) * 100), now);
    }
  } else {
    if ((int32_t)(now - FX.nextSub) > 0) FX.nextSub = now + 4000;
    FX.subReady = false;
  }
  // The eyes, when the Doom is near and knows it.
  if (full && FX.doomNear && !FX.eyesOn && !fxCut.on && (int32_t)(now - FX.nextEyes) >= 0) {
    FX.eyesOn = true; FX.eyesT0 = now;
    FX.nextEyes = now + fxJitter(22000, now + 5);
  }
  // The heart, when someone is nearly gone: lub, dub.
  if (FX.lowLife && (int32_t)(now - FX.nextBeat) >= 0) {
    fxImpact(FX.beatStep == 0 ? 0.30f : 0.20f);
    FX.nextBeat = now + (FX.beatStep == 0 ? 150 : 820);
    FX.beatStep ^= 1;
  }
}

// Put the eyes where the screen is darkest -- they should be looking out of
// the black, not printed over a number.
static void fxPlaceEyes(const uint16_t* src) {
  int bestX = 120, bestY = 160;
  uint32_t best = 0xFFFFFFFFU;
  for (int k = 0; k < 10; k++) {
    uint32_t h = fxHash(FX.eyesT0 + k * 977);
    int cx = 50 + (int)(h % 140), cy = 50 + (int)((h >> 12) % 220);
    uint32_t lit = 0;
    for (int y = cy - 12; y < cy + 12; y += 3)
      for (int x = cx - 46; x < cx + 46; x += 3)
        if (src[y * FX_W + x]) lit++;
    if (lit < best) { best = lit; bestX = cx; bestY = cy; }
  }
  FX.eyesX = (int16_t)bestX; FX.eyesY = (int16_t)bestY;
}
static void fxEyesFrame(const uint16_t* src, uint32_t now, float shx, float shy) {
  uint32_t t = now - FX.eyesT0;
  if (t == 0 || !FX.eyesX) fxPlaceEyes(src);
  if (t > 2100) { FX.eyesOn = false; FX.eyesX = 0; return; }
  float open = fxClampF(t / 450.0f, 0.0f, 1.0f);
  if (t > 1100 && t < 1240) open *= fabsf((float)t - 1170.0f) / 70.0f;
  if (t > 1700) open *= fxClampF((2100.0f - t) / 400.0f, 0.0f, 1.0f);
  float look = (fxNoise1(t * 0.003f, FX.eyesT0) - 0.5f) * 2.0f;
  float cx = FX.eyesX + shx, cy = FX.eyesY + shy;
  // A pool of black behind them first, so they sit in the dark, not on the UI.
  for (int y = (int)cy - 16; y < (int)cy + 16; y++) {
    float q = (y + 0.5f - cy) / 16.0f;
    float w = 54.0f * sqrtf(fmaxf(0.0f, 1.0f - q * q)) * fxClampF(open * 3.0f, 0.0f, 1.0f);
    fxSpan(y, (int)(cx - w), (int)(cx + w), FX_PAL[FXP_INK]);
  }
  fxEyePair(cx, cy, 34.0f, 8.5f, 16.0f, open, look, FX.eyesT0, FXP_HOT, FXP_BRICK, FXP_RUST);
}

// ── 15. Switching and reprinting ─────────────────────────────────────────────
// Button B: the old screen is cut along a slanted line and the halves slide
// apart along it, the new one underneath. Needs the old frame, which is why
// fxPrev exists at all.
static void fxBeginSwitch(uint32_t now) {
  FxRng R; R.s = fxHash(now);
  FX.sw = true; FX.swT0 = now;
  FX.swCx = 110.0f + R.sgn() * 30.0f;
  FX.swCy = 160.0f + R.sgn() * 40.0f;
  float a = -1.05f + R.sgn() * 0.25f;          // steep, up to the right
  FX.swDx = cosf(a); FX.swDy = sinf(a);
  fxImpact(0.18f);
}
static const uint32_t FX_SW_MS = 300;

// Row-diff the new content against the last one and mark what changed for a
// reprint: small changes tick, mid-size bands slide in from the right like a
// line being retyped, big ones are swept down by the beam.
static void fxContentChanged(const uint16_t* src, uint32_t now, bool switched) {
  if (!fxRowHash) return;
  bool had = FX.haveHash;
  int y0 = -1, last = -100;
  uint8_t n = 0;
  for (int y = 0; y <= FX_H; y++) {
    bool ch = false;
    if (y < FX_H) {
      const uint32_t* w = (const uint32_t*)(src + y * FX_W);
      uint32_t h = 2166136261U;
      for (int i = 0; i < FX_W / 2; i++) h = (h ^ w[i]) * 16777619U;
      ch = had && fxRowHash[y] != h;
      fxRowHash[y] = h;
    }
    if (ch) { if (y0 < 0) y0 = y; last = y; }
    // A band closes after five quiet rows -- two glyphs can share a row
    // exactly, and a changed number should not reprint as three slivers.
    if (y0 >= 0 && (y == FX_H || y - last > 5)) {
      if (!switched && n < 4) {
        int h = last + 1 - y0;
        FX.rep[n].y0 = (int16_t)y0; FX.rep[n].y1 = (int16_t)(last + 1);
        FX.rep[n].t0 = now;
        FX.rep[n].style = (uint8_t)(h <= 10 ? 0 : (h <= 70 ? 1 : 2));
        n++;
      }
      y0 = -1;
    }
  }
  FX.haveHash = true;
  if (!switched && n) FX.repN = n;
}

// ── 16. The compositor ───────────────────────────────────────────────────────
static inline void fxFetch(const uint16_t* srcRow, int dx, uint16_t* L) {
  if (dx == 0)            { memcpy(L, srcRow, FX_W * 2); return; }
  if (dx >= FX_W || dx <= -FX_W) { memset(L, 0, FX_W * 2); return; }
  if (dx > 0) { memset(L, 0, dx * 2); memcpy(L + dx, srcRow, (FX_W - dx) * 2); }
  else        { memcpy(L, srcRow - dx, (FX_W + dx) * 2); memset(L + FX_W + dx, 0, -dx * 2); }
}

// The switch cut, one row: each old half is looked up at its displaced
// position and only kept if that pixel was on its own side of the cut.
static void fxSwitchRow(const uint16_t* src, const uint16_t* prev, int y, float p, uint16_t* L) {
  memcpy(L, src + y * FX_W, FX_W * 2);
  if (p >= 1.0f || !prev) return;
  float d = p < 0.18f ? 0.0f : fxCubicIn((p - 0.18f) / 0.82f) * 340.0f;
  float s = p * 10.0f;
  float nx = -FX.swDy, ny = FX.swDx;
  for (int half = 0; half < 2; half++) {
    float sg = half ? 1.0f : -1.0f;
    float ox = sg * (FX.swDx * d + nx * s), oy = sg * (FX.swDy * d + ny * s);
    int sy = (int)floorf(y - oy + 0.5f);
    if (sy < 0 || sy >= FX_H) continue;
    const uint16_t* pr = prev + sy * FX_W;
    for (int x = 0; x < FX_W; x++) {
      float sx = x - ox;
      int ix = (int)floorf(sx + 0.5f);
      if (ix < 0 || ix >= FX_W) continue;
      float side = (sx - FX.swCx) * nx + (sy - FX.swCy) * ny;
      if (half == 0 ? side >= 0 : side < 0) continue;
      L[x] = (fabsf(side) < 1.6f && d > 0) ? FX_PAL[FXP_WHITE] : pr[ix];
    }
  }
}

static bool fxAnimating(uint32_t now);

// Compose one frame of `src` (the screen as rendered) into `out`. `prev` is
// the frame a switch is cutting away from.
static void fxCompose(const uint16_t* src, const uint16_t* prev, uint16_t* out, uint32_t now) {
  if (!fxTablesReady) return;
  uint32_t dt = FX.lastMs ? now - FX.lastMs : 0;
  if (dt > 200) dt = 200;
  FX.lastMs = now;
  FX.frame++;
  fxDst = out;

  // Madness drifts toward its target rather than jumping to it.
  {
    // RESTRAINED stops short of the hum bar (64): rare glitches, no continuous
    // animation, so the screen is still until something happens.
    int cap = FX.level >= 2 ? 255 : (FX.level == 1 ? 60 : 0);
    int tgt = fxMinI(FX.madTarget, cap), m = FX.madness;
    int step = fxMaxI(1, (int)(dt / 40));
    m += (tgt > m) ? fxMinI(step, tgt - m) : -fxMinI(step, m - tgt);
    FX.madness = (uint8_t)m;
  }
  if (FX.level > 0) fxAmbient(now);

  // Pull the next panel off the queue. A panel already running is never
  // interrupted -- what arrives meanwhile waits its turn, and goes stale after
  // eight seconds -- except by a death, which takes the whole screen.
  {
    FxCueReq q[4];
    uint8_t  n = 0;
    FX_LOCK();
    // A panel just ended: give the screen under it a moment to be read before
    // the next one, unless the next one is a catastrophe.
    // A cut scene is the one thing a catastrophe may cut short: seconds of
    // spiders are not worth missing the quake.
    bool wait = fxCut.on || (FX.cutEnd && now - FX.cutEnd < 900);
    for (int i = 0; i < fxQn; i++)
      if (fxQ[i].kind == FXK_DOWNED ||
          (FX_STYLE[fxQ[i].kind].prio >= 3 && (!fxCut.on || fxIsScene(fxCut.kind)))) wait = false;
    if (!wait) { n = fxQn; memcpy(q, fxQ, sizeof(FxCueReq) * n); fxQn = 0; }
    FX_UNLOCK();
    int pick = -1;
    for (int i = 0; i < n; i++) {
      if (now - q[i].atMs > 8000) continue;
      if (FX.takeover && q[i].kind != FXK_DOWNED) continue;
      const FxStyleDef& st = FX_STYLE[q[i].kind];
      if (FX.lastKind[q[i].kind] && st.coolMs && now - FX.lastKind[q[i].kind] < st.coolMs) continue;
      if (pick < 0 || st.prio > FX_STYLE[q[pick].kind].prio || q[i].kind == FXK_DOWNED) pick = i;
    }
    if (pick >= 0 && FX.level > 0) fxCutBegin(q[pick], now);
  }
  if (FX.takeover && fxCut.on && fxCut.kind != FXK_DOWNED) fxCut.on = false;
  if (FX.takeover) fxCrawl.on = false;              // not over the skull

  // Camera: trauma squared, through value noise, so it lurches instead of
  // buzzing.
  FX.trauma = fmaxf(0.0f, FX.trauma - dt * 0.0016f);
  float s2 = FX.trauma * FX.trauma;
  float nt = now * 0.021f;
  int   dx = (int)lroundf(11.0f * s2 * (fxNoise1(nt, 11) * 2.0f - 1.0f));
  int   dy = (int)lroundf( 8.0f * s2 * (fxNoise1(nt, 23) * 2.0f - 1.0f));
  float shear = 0.05f * s2 * (fxNoise1(nt * 0.7f, 37) * 2.0f - 1.0f);

  // Vertical hold.
  int  vOff = 0;
  bool vOn  = false;
  if (FX.vrollDur && now - FX.vrollT0 < FX.vrollDur) {
    float p = (now - FX.vrollT0) / (float)FX.vrollDur;
    vOn = true;
    if (FX.vrollKind == 1) vOff = (int)(fxCubicOut(p) * (FX_H + 16));
    else                   vOff = (int)(sinf(p * 3.14159f) * (18.0f + FX.madness / 6.0f));
  }

  // Screen switch.
  float swP = -1.0f;
  if (FX.sw) {
    swP = (now - FX.swT0) / (float)FX_SW_MS;
    if (swP >= 1.0f) { FX.sw = false; swP = -1.0f; fxImpact(0.16f); }
  }

  // Halftone shadow: the screen sinks into screentone behind a panel.
  int shadow = 0;
  if (fxCut.on) {
    uint32_t t = now - fxCut.t0, end = fxCutEnd(fxCut);
    float a = fxClampF(t / 140.0f, 0.0f, 1.0f) * fxClampF((end - (float)t) / 260.0f, 0.0f, 1.0f);
    // Fog closes in slower and thicker than a panel's shadow.
    if (fxCut.kind == FXK_FOG) a = fxClampF(t / 700.0f, 0.0f, 1.0f) * fxClampF((end - (float)t) / 500.0f, 0.0f, 1.0f) * 1.55f;
    // A cut scene takes its time about it, and the edges go first.
    if (fxIsScene(fxCut.kind)) a = fxClampF(t / 900.0f, 0.0f, 1.0f) * fxClampF((end - (float)t) / 800.0f, 0.0f, 1.0f) * 1.3f;
    shadow = (int)(a * 150.0f);
  }
  if (shadow) {
    uint32_t drift = now / 90;
    const bool scene = fxIsScene(fxCut.kind);
    for (int cy = 0; cy < 40; cy++)
      for (int cx = 0; cx < 30; cx++) {
        float sc = fxCut.kind == FXK_FOG ? 0.14f : 0.31f;
        float n = fxNoise2(cx * sc + drift * 0.02f, cy * sc, 77);
        int t;
        if (scene) {
          float vx = (cx - 14.5f) / 15.0f, vy = (cy - 19.5f) / 20.0f;
          t = (int)(shadow * (0.3f + 0.5f * n + 0.55f * (vx * vx + vy * vy)));
        } else {
          float bandD = fabsf(cy * 8.0f + 4.0f - fxCut.yc) / 160.0f;
          t = (int)(shadow * (0.45f + 0.9f * n) * (1.25f - bandD * 0.6f));
        }
        fxTone[cy][cx] = (uint8_t)fxMinI(255, fxMaxI(0, t));
      }
  }

  // Ambient: the hum bar, the shiver.
  const uint8_t m = FX.madness;
  int   rollY = -1000, rollDepth = 0;
  if (m >= 64 && FX.level > 0) {
    rollY = FX_H + 60 - (int)((now / 22) % (FX_H + 120));
    rollDepth = 22 + (m - 64) / 3;
  }
  int  shiverP  = (m >= 110) ? (m - 100) / 3 : 0;          // /256 per row pair
  bool glitchOn = (int32_t)(FX.glitchUntil - now) > 0;
  bool dropout  = glitchOn && FX.glitchKind == 2;
  int  dimAll   = (glitchOn && FX.glitchKind == 3) ? 170 : 256;
  bool tearOn   = FX.tearN && (int32_t)(FX.tearUntil - now) > 0;
  bool ghost    = (FX.trauma > 0.12f || vOn) && !FX.lastFlash;
  FX.lastFlash  = false;

  // Reprint bands still running.
  // A tick is over in a frame or two; only real reprints keep the loop awake.
  bool repOn = false;
  for (int i = 0; i < FX.repN; i++)
    if (now - FX.rep[i].t0 < (FX.rep[i].style == 0 ? 100u : 260u)) repOn = true;
  if (!repOn) FX.repN = 0;

  // ── the row pass ──
  for (int y = 0; y < FX_H; y++) {
    uint16_t* o = out + y * FX_W;
    int rowDx = dx + (int)lroundf(shear * (y - 160));
    if (tearOn)
      for (int i = 0; i < FX.tearN; i++) {
        int ty = y - FX.tear[i].y0;
        if (ty >= 0 && ty < FX.tear[i].h) {
          float f = FX.tear[i].curve ? (float)ty / FX.tear[i].h : 1.0f;
          rowDx += (int)(FX.tear[i].dx * f * f);
        }
      }
    if (shiverP && (int)(fxHash2((uint32_t)(y >> 1), FX.frame) & 255) < shiverP)
      rowDx += (fxHash2((uint32_t)y, FX.frame + 3) & 1) ? (m > 200 ? 2 : 1) : -1;

    // Reprint: this row's band may still be arriving.
    int  repKeep = 0;
    bool repPop  = false;
    for (int i = 0; i < FX.repN; i++) {
      if (y < FX.rep[i].y0 || y >= FX.rep[i].y1) continue;
      uint32_t t = now - FX.rep[i].t0;
      if (t >= 260) continue;
      if (FX.rep[i].style == 0) { if (t < 90) { rowDx += 2; repPop = true; } }
      else if (FX.rep[i].style == 1) { rowDx += (int)((1.0f - fxCubicOut(t / 220.0f)) * 150.0f); repPop = t < 120; }
      else {
        int beam = FX.rep[i].y0 + (int)((FX.rep[i].y1 - FX.rep[i].y0) * fxClampF(t / 240.0f, 0.0f, 1.0f));
        if (y > beam) repKeep = 1;
        else if (y == beam) repKeep = 2;
      }
    }
    if (repKeep == 1 && !FX.sw && !vOn && dx == 0) continue;   // still the old line

    if (swP >= 0) {
      fxSwitchRow(src, prev, y, swP, fxL);
    } else {
      int sy = y - dy;
      bool blank = false;
      if (vOn) {
        int r = sy + vOff;
        const int PERIOD = FX_H + 16;
        r %= PERIOD;
        if (r < 0) r += PERIOD;
        if (r >= FX_H) blank = true;
        sy = r;
      }
      if (blank || sy < 0 || sy >= FX_H) memset(fxL, 0, FX_W * 2);
      else fxFetch(src + sy * FX_W, rowDx, fxL);
    }
    if (repKeep == 2) for (int x = 0; x < FX_W; x++) fxL[x] = FX_PAL[FXP_GLOW];

    // Gain: the hum bar, a dim beat, a freshly reprinted line glowing hot.
    int g = dimAll;
    if (rollDepth) {
      int d = y - rollY;
      if (d > -40 && d < 40) {
        float f = 1.0f - fabsf(d / 40.0f);
        g = g * (256 - (int)(rollDepth * fxSmooth(f))) >> 8;
      }
    }
    if (dropout && (y & 1)) g = 0;
    if (repPop) { for (int x = 0; x < FX_W; x++) if (fxL[x]) fxL[x] = fxBoost(fxL[x], 330); }
    else if (g == 0) memset(fxL, 0, FX_W * 2);
    else if (g < 256) { for (int x = 0; x < FX_W; x++) if (fxL[x]) fxL[x] = fxDim(fxL[x], (uint32_t)g); }

    // Screentone. On black it prints faint dots; on a lit pixel it cuts one.
    if (shadow) {
      const uint8_t* dots = FX_DOT + (y & 7) * 8;
      const uint8_t* tr   = fxTone[y >> 3];
      const uint16_t lit  = FX_PAL[FXP_BAND];
      for (int x = 0; x < FX_W; x++) {
        bool in = dots[x & 7] < tr[x >> 3];
        if (fxL[x]) fxL[x] = in ? 0 : fxDim(fxL[x], 118);
        else if (in) fxL[x] = lit;
      }
    }
    // Phosphor persistence while the picture is moving: where the new frame
    // is black, the old one lingers a moment.
    if (ghost) for (int x = 0; x < FX_W; x++) if (!fxL[x] && o[x]) fxL[x] = FX_PAL[FX_GHOST[fxLum(o[x]) >> 4]];
    memcpy(o, fxL, FX_W * 2);
  }

  // ── overlays, back to front ──
  float shx = (float)dx, shy = (float)dy;
  fxHalfN = 0; fxColT = fxColB = nullptr; fxStip = 0;
  if (fxCut.on && (FX_STYLE[fxCut.kind].extras & FXE_RAIN)) fxRain(now, 70, fxCut.seed, shx, shy);
  if (fxSplatOn) fxSplatDraw(now, shx, shy);
  if (fxFireOn) {
    if (now - fxFireLast >= 50) { fxFireLast = now; fxFireStep(now); }
    fxFireDraw(shx, shy);
  }
  if (fxBoltOn) {
    uint32_t t = now - fxBoltT0;
    // Three flickers, each one regrown.
    int f = t < 60 ? 0 : (t >= 120 && t < 170) ? 1 : (t >= 260 && t < 300) ? 2 : -1;
    if (f >= 0) { fxBoltGen(fxCut.seed + 21 + f); fxBoltDraw(shx, shy); }
    if (t > 320) fxBoltOn = false;
  }
  fxCutFrame(now, shx, shy);
  // The glass is in front of everything, panel included.
  if (fxCrkOn) fxCracksDraw(now, shx, shy);
  if (FX.eyesOn) fxEyesFrame(src, now, shx, shy);
  if (fxCrawl.on) fxCrawlFrame(now, shx, shy);      // on the glass, over the lot

  // A switch opens as a blade drawn across the old frame, a beat before it
  // parts.
  if (swP >= 0 && swP < 0.2f) {
    for (int dir = -1; dir <= 1; dir += 2)
      fxStroke(FX.swCx, FX.swCy, FX.swCx + dir * FX.swDx * 420.0f, FX.swCy + dir * FX.swDy * 420.0f,
               5.0f, 0.8f, FX_PAL[FXP_WHITE]);
  }
  // Datamosh: a few blocks of the picture turn up where they do not belong.
  if (glitchOn && FX.glitchKind == 4) {
    FxRng R; R.s = fxHash(FX.frame * 977U);
    for (int b = 0; b < 3; b++) {
      int w = R.range(16, 56), h = R.range(4, 14);
      int sx = R.range(0, FX_W - w), sy = R.range(0, FX_H - h);
      int tx = fxMaxI(0, fxMinI(FX_W - w, sx + R.range(-34, 34)));
      int ty = fxMaxI(0, fxMinI(FX_H - h, sy + R.range(-10, 10)));
      for (int r = 0; r < h; r++) {
        memcpy(fxL, out + (sy + r) * FX_W + sx, w * 2);
        memcpy(out + (ty + r) * FX_W + tx, fxL, w * 2);
      }
    }
  }

  // Static: specks and dropout streaks, in proportion.
  if (m >= 90 && FX.level > 0) {
    int n = (m - 80) / 5;
    for (int i = 0; i < n; i++) {
      uint32_t h = fxHash2((uint32_t)i, FX.frame * 7919U);
      int x = (int)(h % FX_W), y = (int)((h >> 9) % FX_H);
      fxPut(x, y, FX_PAL[FXP_LINE + (h >> 20) % 10]);
    }
    if ((fxHash(FX.frame) & 7) == 0) {
      uint32_t h = fxHash(FX.frame * 31U);
      int y = (int)(h % FX_H), x = (int)((h >> 9) % 200), w = 10 + (int)((h >> 17) % 90);
      fxSpan(y, x, x + w, FX_PAL[(h >> 28) & 1 ? FXP_GLOW : FXP_INK]);
    }
  }

  // The subliminal frame: the screen in negative, a word written on it.
  if (FX.subFrames) {
    FX.subFrames--;
    FX.lastFlash = true;
    for (int i = 0; i < FX_W * FX_H; i++) out[i] = FX_PAL[15 - (fxLum(out[i]) >> 4)];
    uint32_t h = fxHash(now);
    float cx = 120.0f + fxS(h) * 8.0f, cy = 90.0f + fxU(h >> 3) * 140.0f, rot = fxS(h >> 7) * 0.10f;
    for (int i = 0; i < fxSubWord->n; i++) {
      const FxLetter& L = fxSubWord->L[i];
      fxLetterDraw(L, cx + L.x, cy + L.y, 1.0f, L.rot + rot);
      fxDripDraw(L, cx + L.x, cy + L.y, 1.0f, L.rot + rot, 0.8f);
    }
  }

  // Impact frames: white-out, then the negative.
  if (FX.flashN) {
    uint8_t k = FX.flashQ[0];
    for (int i = 1; i < FX.flashN; i++) FX.flashQ[i - 1] = FX.flashQ[i];
    FX.flashN--;
    FX.lastFlash = true;
    if (k == FXFL_WHITE) {
      for (int i = 0; i < FX_W * FX_H; i++) out[i] = FX_PAL[fxLum(out[i]) > 40 ? FXP_WHITE : FXP_GLOW];
    } else {
      for (int i = 0; i < FX_W * FX_H; i++) out[i] = FX_PAL[15 - (fxLum(out[i]) >> 4)];
    }
  }
  // Ambient lightning from the lamps' own bolt: the room lights up for a tick.
  if (FX.boltAmb) {
    FX.boltAmb = false;
    FX.lastFlash = true;
    for (int i = 0; i < FX_W * FX_H; i++) out[i] = out[i] ? fxBoost(out[i], 400) : FX_PAL[FXP_TRACK];
  }

  // The FX-level toast (button A).
  if ((int32_t)(FX.toastUntil - now) > 0) {
    static const char* const LV[3] = { "OFF", "RESTRAINED", "MADNESS" };
    char b[32];
    snprintf(b, sizeof(b), "LCD FX  %s", LV[FX.level < 3 ? FX.level : 2]);
    int w = (int)strlen(b) * 12 + 16;
    int x = (FX_W - w) / 2;
    fxRect(x, 132, x + w, 160, FX_PAL[FXP_INK]);
    fxRect(x + 1, 133, x + w - 1, 134, FX_PAL[FXP_HOT]);
    fxRect(x + 1, 157, x + w - 1, 158, FX_PAL[FXP_HOT]);
    fxGlcd(b, x + 8, 138, 2, FX_PAL[FXP_GLOW]);
  }
}

// ── 17. Pacing ───────────────────────────────────────────────────────────────
static bool fxAnimating(uint32_t now) {
  if (FX.level == 0) return (int32_t)(FX.toastUntil - now) > 0;
  if (fxCut.on || FX.sw || FX.trauma > 0.01f || FX.flashN || FX.subFrames || FX.boltAmb) return true;
  if (fxCrkOn || fxBoltOn || fxFireOn || fxSplatOn || FX.eyesOn || FX.repN || fxCrawl.on) return true;
  if ((FX.vrollDur && now - FX.vrollT0 < FX.vrollDur) || (int32_t)(FX.glitchUntil - now) > 0) return true;
  if ((int32_t)(FX.tearUntil - now) > 0 || (int32_t)(FX.toastUntil - now) > 0) return true;
  if (FX.madness >= 64) return true;          // the hum bar never stops rolling
  {
    // Madness only moves on a frame, so keep stepping while it is still
    // drifting toward what the dread says -- a few seconds, not a minute of
    // content refreshes.
    int cap = FX.level >= 2 ? 255 : 60, tgt = FX.madTarget < cap ? FX.madTarget : cap;
    if (abs((int)FX.madness - tgt) > 2) return true;
  }
  bool queued;
  FX_LOCK(); queued = fxQn > 0; FX_UNLOCK();
  if (queued) return true;
  // Something is scheduled to happen shortly: wake for it.
  if ((int32_t)(FX.nextGlitch - now) <= 0) return true;
  if (FX.subReady && (int32_t)(FX.subAt - now) <= 0) return true;
  return false;
}
// Milliseconds the loop should wait before the next frame.
static uint16_t fxFramePeriod(uint32_t now) {
  if (fxCut.on || FX.sw || FX.trauma > 0.05f || FX.flashN || fxBoltOn || FX.repN) return 40;
  if (fxCrkOn || fxFireOn || fxSplatOn || FX.eyesOn || FX.subFrames || fxCrawl.on) return 50;
  if ((FX.vrollDur && now - FX.vrollT0 < FX.vrollDur) || (int32_t)(FX.glitchUntil - now) > 0) return 45;
  return 90;
}

static void fxInitTables() {
  for (int i = 0; i < FXP_COUNT; i++) FX_PAL[i] = fxSwap(c16(FX_PAL_RGB[i]));
  // The halftone screen: each cell of an 8x8 tile ranked by its distance to the
  // nearest dot centre of a 45-degree lattice (pitch 5.7 px). A pixel is inked
  // when its rank is under the tone, so coverage rises linearly with tone and
  // the dots grow, touch, and invert the way a real screen does.
  static const float LX[5] = { 0, 8, 0, 8, 4 }, LY[5] = { 0, 0, 8, 8, 4 };
  float d[64];
  for (int i = 0; i < 64; i++) {
    float x = (i & 7) + 0.5f, y = (i >> 3) + 0.5f, best = 1e9f;
    for (int k = 0; k < 5; k++) {
      float ex = x - LX[k], ey = y - LY[k], dd = ex * ex + ey * ey;
      if (dd < best) best = dd;
    }
    d[i] = best + i * 1e-4f;
  }
  for (int i = 0; i < 64; i++) {
    int r = 0;
    for (int j = 0; j < 64; j++) if (d[j] < d[i]) r++;
    FX_DOT[i] = (uint8_t)(r * 4);
  }
  memset(&FX, 0, sizeof(FX));
  memset(&fxCut, 0, sizeof(fxCut));
  memset(&fxCrawl, 0, sizeof(fxCrawl));
  FX.level = 2;
  FX.nextGlitch = 4000;
  FX.nextSub = 12000;
  FX.nextEyes = 6000;
  fxTablesReady = true;
}
static bool fxAllocPools() {
  if (!fxPool)    fxPool    = (uint8_t*)fxAlloc(FX_POOL);
  if (!fxSdfA)    fxSdfA    = (int16_t*)fxAlloc(FX_SDF_MAX * 2);
  if (!fxSdfB)    fxSdfB    = (int16_t*)fxAlloc(FX_SDF_MAX * 2);
  if (!fxCrk)     fxCrk     = (FxSeg*)fxAlloc(sizeof(FxSeg) * FX_CRK_MAX);
  if (!fxRowHash) fxRowHash = (uint32_t*)fxAlloc(sizeof(uint32_t) * FX_H);
  if (!fxHeat)    fxHeat    = (uint8_t(*)[FX_FIRE_C])fxAlloc(FX_FIRE_R * FX_FIRE_C);
  if (!fxSubWord) fxSubWord = (FxWord*)fxAlloc(sizeof(FxWord));
  if (!fxTone)    fxTone    = (uint8_t(*)[30])fxAlloc(40 * 30);
  if (!fxInvH)    fxInvH    = (float*)fxAlloc(sizeof(float) * FX_W);
  if (!fxNest)    fxNest    = (FxNest*)fxAlloc(sizeof(FxNest));
  if (!fxMedal)   fxMedal   = (FxMedal*)fxAlloc(sizeof(FxMedal));
  return fxPool && fxSdfA && fxSdfB && fxCrk && fxRowHash && fxHeat && fxSubWord && fxTone && fxInvH &&
         fxNest && fxMedal;
}

// ── 18. The board ────────────────────────────────────────────────────────────
// Everything above is portable. This is where it meets the K10: the two extra
// sprites, the present call the loop makes instead of canvas.pushSprite(), the
// dread feed. Button A's handler (checkFxButton) sits with Button B's in
// ui-screens.hpp, because it needs the audio helpers included after this.
//
// Button A (unused once boot is past the USB-drive check) cycles the level:
// MADNESS -> RESTRAINED -> OFF -> MADNESS, persisted with the other K10 prefs
// ("fx" in the "k10" namespace). RESTRAINED keeps every panel and the shake
// but caps the ambient layer at a mild flicker, with no whispers and no eyes;
// OFF is the plain screens and the old tube-dropout switch, exactly as before
// this file existed.
#ifndef FX_NATIVE
static LGFX_Sprite fxOutSpr(&tft);     // what the glass shows
static LGFX_Sprite fxPrevSpr(&tft);    // the frame a screen switch cuts away from
static bool        fxReady    = false;
static bool        fxBtnALast = false;

static uint32_t fxNowMs() { return millis(); }

static void fxNameOf(int8_t who, char* out, size_t cap) {
  out[0] = 0;
  if (who < 0 || who >= MAX_PLAYERS) return;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    strlcpy(out, G.players[who].name, cap);
    xSemaphoreGive(G.mutex);
  }
  if (!out[0]) snprintf(out, cap, "Walker %d", (int)who + 1);
}

// Called once from setup(), after the canvas exists. ~430 KB of PSRAM: the two
// sprites, the sticker pool and the distance-field scratch. If any of it will
// not allocate the level is forced to OFF and the screens push as they always
// did.
static void fxBegin(uint8_t level) {
  fxInitTables();
  bool ok = fxAllocPools();
  fxOutSpr.setPsram(true);  fxOutSpr.setColorDepth(16);
  fxPrevSpr.setPsram(true); fxPrevSpr.setColorDepth(16);
  ok = ok && fxOutSpr.createSprite(FX_W, FX_H) && fxPrevSpr.createSprite(FX_W, FX_H);
  fxReady  = ok;
  FX.level = ok ? (level > 2 ? 2 : level) : 0;
  FX.nextGlitch = millis() + 6000;
  Log.notice("LCD FX %s level=%d psram=%uKB", ok ? "ready" : "ALLOC FAIL",
             (int)FX.level, (unsigned)(ESP.getFreePsram() / 1024));
}

static inline bool fxLive() { return fxReady && FX.level > 0; }

static void fxFeedDread() {
  FxDread d;
  d.tc        = g_dread.tcWeight;
  d.doomClose = g_dread.doomClose;
  d.doomAware = g_dread.doomAware;
  d.attrition = g_dread.attrition;
  d.hunger    = g_dread.hunger;
  d.thirst    = g_dread.thirst;
  d.rad       = g_dread.radLoad;
  d.wounds    = g_dread.woundLoad;
  d.fire      = g_dread.fireClose;
  d.downed    = g_dread.downed;
  d.connected = g_dread.connected;
  fxSetDread(d);
}

// The loop's one way onto the glass. `takeover`: the skull owns the screen,
// so no panel may cut in over it (its own knock still lands).
static void fxPresent(uint32_t now, bool takeover) {
  if (!fxReady || (FX.level == 0 && (int32_t)(FX.toastUntil - now) <= 0)) {
    canvas.pushSprite(0, 0);
    return;
  }
  FX.takeover = takeover;
  fxFeedDread();
  uint32_t t0 = micros();
  fxCompose((const uint16_t*)canvas.getBuffer(), (const uint16_t*)fxPrevSpr.getBuffer(),
            (uint16_t*)fxOutSpr.getBuffer(), now);
  uint32_t t1 = micros();
  fxOutSpr.pushSprite(0, 0);
  uint32_t t2 = micros();
  // What a frame actually costs on the board, every 30 s while anything moved:
  // the numbers the frame periods in fxFramePeriod() were guessed from.
  static uint32_t nF = 0, sumC = 0, sumP = 0, maxC = 0, lastLog = 0, nSlow = 0, lastSlow = 0;
  nF++; sumC += t1 - t0; sumP += t2 - t1;
  if (t1 - t0 > maxC) maxC = t1 - t0;
  // One frame at a time, when it is slow. These are wall-clock times and the
  // loop task runs at priority 1, so a slow frame is either the compositor
  // doing too much or the loop being preempted -- `seated` and the heap say
  // which it lines up with. At most one line a second.
  if (t2 - t0 >= 150000) {
    nSlow++;
    if (now - lastSlow >= 1000) {
      lastSlow = now;
      Log.warning("LCD FX slow frame: compose=%uus push=%uus madness=%u level=%u seated=%u iheap=%uKB",
                  (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)FX.madness,
                  (unsigned)FX.level, (unsigned)g_dread.connected,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
  }
  if (now - lastLog >= 30000) {
    // busy: the share of the window the loop spent composing and pushing.
    uint32_t winMs = lastLog ? now - lastLog : 30000;
    Log.notice("LCD FX: %u frames/30s compose avg=%uus max=%uus push avg=%uus busy=%u%% slow=%u madness=%u level=%u",
               (unsigned)nF, (unsigned)(sumC / nF), (unsigned)maxC, (unsigned)(sumP / nF),
               (unsigned)((sumC + sumP) / 10 / winMs), (unsigned)nSlow,
               (unsigned)FX.madness, (unsigned)FX.level);
    nF = sumC = sumP = maxC = nSlow = 0;
    lastLog = now;
  }
}

// Button B landed on a new screen. Keep the frame that is on the glass right
// now -- the next present cuts it away to reveal the new one.
static void fxSwitchScreens(uint32_t now) {
  memcpy(fxPrevSpr.getBuffer(), fxOutSpr.getBuffer(), (size_t)FX_W * FX_H * 2);
  fxBeginSwitch(now);
}

// A lit tick of the lamps' storm bolt (ui-leds.hpp): the room lights up.
static inline void fxAmbientBolt() { if (fxLive() && !fxCut.on) FX.boltAmb = true; }

// The cue ring and the Doom's proximity, appended to the /state JSON for the
// observer screen (game-server.hpp). Captions are the chronicle's own ASCII
// lines; quotes and backslashes are escaped anyway.
static void fxCueLogJson(String& j) {
  FxLogEnt snap[FX_LOG_N];
  uint16_t seq;
  FX_LOCK();
  memcpy(snap, fxLog, sizeof(snap));
  seq = fxLogSeq;
  FX_UNLOCK();
  j += ",\"fx\":{\"seq\":"; j += (unsigned)seq; j += ",\"q\":[";
  bool first = true;
  for (int i = 0; i < FX_LOG_N; i++) {
    const FxLogEnt& e = snap[i];
    if (!e.n || e.kind == FXK_NONE || e.kind >= FXK_COUNT) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"n\":"; j += (unsigned)e.n;
    j += ",\"k\":\""; j += FX_KIND_NAME[e.kind];
    j += "\",\"w\":"; j += (int)e.who;
    j += ",\"a\":"; j += (unsigned)e.arg;
    j += ",\"c\":\"";
    for (const char* p = e.cap; *p && p < e.cap + sizeof(e.cap); p++) {
      if (*p == '"' || *p == '\\') j += '\\';
      if ((uint8_t)*p >= 32) j += *p;
    }
    j += "\"}";
  }
  j += "]},\"doom\":{\"aw\":"; j += (unsigned)g_dread.doomAware;
  j += ",\"cl\":";               j += (unsigned)g_dread.doomClose;
  j += "}";
}

// Whether the loop should put a frame on the glass this pass. Reads the dread
// first -- a flat byte struct, lock-free -- so a party getting worse is noticed
// within one loop pass rather than at the next content repaint.
static bool fxWantsFrame(uint32_t now) {
  if (!fxLive()) return fxReady && (int32_t)(FX.toastUntil - now) > 0;
  fxFeedDread();
  return fxAnimating(now);
}

// How long the loop may sleep: a frame's worth while something moves, the old
// 100 ms when nothing does.
// While anyone is seated the LCD gets at most 10 frames a second. During the
// 2026-09-24 bot run it went from ~10 frames/30 s to 250+, the loop spending
// most of core 1 on the panel just as the network wedged. The table is still
// worth watching at 10; the players' sockets are worth more.
static constexpr uint32_t FX_SEATED_MIN_PERIOD_MS = 100;
static uint32_t fxLoopDelay(uint32_t now, uint32_t frameStart) {
  if (!fxLive() || !fxAnimating(now)) return 100;
  uint32_t per = fxFramePeriod(now), spent = millis() - frameStart;
  if (g_dread.connected && per < FX_SEATED_MIN_PERIOD_MS) per = FX_SEATED_MIN_PERIOD_MS;
  return (spent + 5 >= per) ? 5 : per - spent;
}

#endif
