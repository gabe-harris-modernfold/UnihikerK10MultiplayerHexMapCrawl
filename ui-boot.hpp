#pragma once
// ── ui-boot.hpp ─────────────────────────────────────────────────────────────
// Boot splash diagnostic log and thread-safe K10 event ring buffer.

// ── Boot splash diagnostic log ─────────────────────────────────────────────
static char    _sLog[14][30];
static uint8_t _sN = 0;
static void splashAdd(const char* msg, uint32_t col = 0) {
  if (_sN == 14) {
    for (int i = 0; i < 13; i++) memcpy(_sLog[i], _sLog[i+1], 30);
    _sN = 13;
  }
  snprintf(_sLog[_sN++], 30, "%s", msg);
  uint32_t textCol = col ? col : 0xD06818;
  canvas.fillScreen(0x0000);
  for (uint8_t i = 0; i < _sN; i++)
    canvasText8(_sLog[i], 4, 4 + i * 10, textCol);
  canvas.pushSprite(0, 0);
}

// ── K10 event log (thread-safe ring buffer) ────────────────────────────────
static void k10LogAdd(const char* text) {
  taskENTER_CRITICAL(&k10LogMux);
  uint8_t idx;
  if (k10LogCount < K10_LOG_SIZE) {
    idx = (k10LogHead + k10LogCount) % K10_LOG_SIZE;
    k10LogCount++;
  } else {
    idx = k10LogHead;
    k10LogHead = (k10LogHead + 1) % K10_LOG_SIZE;
  }
  strlcpy(k10Log[idx].text, text, 34);
  k10Log[idx].ms = millis();
  taskEXIT_CRITICAL(&k10LogMux);
  k10Dirty = true;
}
