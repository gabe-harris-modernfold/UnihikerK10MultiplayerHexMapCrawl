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
// canvasText16: 16px font (setTextSize 2)
static inline void canvasText16(const char* s, int x, int y, uint32_t col) {
  canvas.setTextSize(2);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText8: 8px font (setTextSize 1)
static inline void canvasText8(const char* s, int x, int y, uint32_t col) {
  canvas.setTextSize(1);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
// canvasText24: 24px font (setTextSize 3)
static inline void canvasText24(const char* s, int x, int y, uint32_t col) {
  canvas.setTextSize(3);
  canvas.setTextColor(c16(col));
  canvas.setCursor(x, y);
  canvas.print(s);
}
