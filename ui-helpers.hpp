#pragma once
// ── ui-helpers.hpp ──────────────────────────────────────────────────────────
// RGB888→RGB565 conversion and LovyanGFX canvas drawing abstractions.

static inline uint16_t c16(uint32_t c) {
  return (uint16_t)(((c & 0xF80000) >> 8) | ((c & 0x00FC00) >> 5) | ((c & 0x0000F8) >> 3));
}

// canvasRect(x1,y1,x2,y2, col, fill): fill=true→fillRect, fill=false→drawRect
static inline void canvasRect(int x1, int y1, int x2, int y2, uint32_t col, bool fill) {
  if (fill) canvas.fillRect(x1, y1, x2 - x1, y2 - y1, c16(col));
  else      canvas.drawRect(x1, y1, x2 - x1, y2 - y1, c16(col));
}
static inline void canvasLine(int x1, int y1, int x2, int y2, uint32_t col) {
  canvas.drawLine(x1, y1, x2, y2, c16(col));
}
// ── The font table ──────────────────────────────────────────────────────────
// LovyanGFX ships the same classic faces TFT_eSPI numbers 1/2/4/6/7/8, under
// fonts::FontN — the panel behind `canvas` is a plain ILI9341, so all of them
// are available here:
//
//   Font0   6x8 GLCD monospace   canvasText8 / 16 / 24 scale it 1x / 2x / 3x
//   Font2   16 px proportional   canvasText16p   (~6.4 px average advance)
//   Font4   26 px proportional   canvasText26p   (display sizes only)
//   Font6/7/8  48-75 px, digits only, 7-segment — nothing on these screens is
//              an instrument readout, so they stay unused.
//
// setFont() is sticky on the canvas and setTextSize() does not reset it, so
// every helper below sets *both*. Miss that and the first screen to use a
// proportional face leaves every later canvasText8 rendering in it.

// canvasText16: 16px font (Font0 at setTextSize 2)
static inline void canvasText16(const char* s, int x, int y, uint32_t col) {
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(2);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText8: 8px font (Font0 at setTextSize 1)
static inline void canvasText8(const char* s, int x, int y, uint32_t col) {
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText24: 24px font (Font0 at setTextSize 3)
static inline void canvasText24(const char* s, int x, int y, uint32_t col) {
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(3);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText16p: the 16 px proportional face (TFT_eSPI "Font 2"). Text set in
// it reads at arm's length where the 6x8 hand does not, and costs only ~8%
// more width per character, so a wrapped line holds nearly as much.
static inline void canvasText16p(const char* s, int x, int y, uint32_t col) {
  canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText26p: the 26 px proportional face (TFT_eSPI "Font 4"). A display
// size — mastheads, chapter numerals, dropped capitals.
static inline void canvasText26p(const char* s, int x, int y, uint32_t col) {
  canvas.setFont(&fonts::Font4);
  canvas.setTextSize(1);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// Measured widths for the two proportional faces — the monospace 6 px/char
// arithmetic the screens use for Font0 does not hold for these, so anything
// centred or wrapped in them has to ask. textWidth() multiplies by the current
// text size, hence the setTextSize(1) before each measurement.
static inline int canvasWidth16p(const char* s) {
  canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1);
  return (int)canvas.textWidth(s);
}
static inline int canvasWidth26p(const char* s) {
  canvas.setFont(&fonts::Font4);
  canvas.setTextSize(1);
  return (int)canvas.textWidth(s);
}

// ── Right-aligned and centred setting ───────────────────────────────────────
// The screens are built right-to-left as much as left-to-right, and with a
// proportional face you cannot do that by counting characters. `xr` is the
// column the string ENDS at; `cx` is the column it is centred on. The 6x8
// variants are here too so a mixed row reads the same way all the way across
// instead of switching between two idioms mid-line.
static inline void canvasText8R(const char* s, int xr, int y, uint32_t col) {
  canvasText8(s, xr - (int)strlen(s) * 6, y, col);
}
static inline void canvasText16R(const char* s, int xr, int y, uint32_t col) {
  canvasText16(s, xr - (int)strlen(s) * 12, y, col);
}
static inline void canvasText16pR(const char* s, int xr, int y, uint32_t col) {
  canvasText16p(s, xr - canvasWidth16p(s), y, col);
}
static inline void canvasText26pR(const char* s, int xr, int y, uint32_t col) {
  canvasText26p(s, xr - canvasWidth26p(s), y, col);
}
static inline void canvasText8C(const char* s, int cx, int y, uint32_t col) {
  canvasText8(s, cx - (int)strlen(s) * 3, y, col);
}
static inline void canvasText16pC(const char* s, int cx, int y, uint32_t col) {
  canvasText16p(s, cx - canvasWidth16p(s) / 2, y, col);
}
static inline void canvasText26pC(const char* s, int cx, int y, uint32_t col) {
  canvasText26p(s, cx - canvasWidth26p(s) / 2, y, col);
}

// Copy the widest prefix of `src` that fits `wpx` in the 16 px face into
// `dst`. A proportional face cannot be clipped with "%.10s": "Quartermaster1"
// and "iiiiiiiiiiiiii" are the same number of characters and nowhere near the
// same width, so a character budget either wraps into the next column or
// wastes half of one. Measure instead.
static void canvasFit16p(char* dst, size_t cap, const char* src, int wpx) {
  size_t n = 0;
  int    w = 0;
  char   t[2] = { 0, 0 };
  while (src[n] && n + 1 < cap) {
    t[0] = src[n];
    int cw = canvasWidth16p(t);
    if (w + cw > wpx) break;
    w += cw;
    n++;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}
