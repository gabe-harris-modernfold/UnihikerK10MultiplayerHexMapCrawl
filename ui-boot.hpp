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

// ── The chronicle (thread-safe ring buffer) ────────────────────────────────
// `text` is a predicate when `who` names a player ("goes hungry for the
// trying.") and a whole sentence when who is -1. See K10LogEntry for the
// '\x01' second-name placeholder. Safe from either core.
static void k10LogAdd(const char* text, int8_t who = -1,
                      uint8_t tone = TONE_PLAIN, int8_t who2 = -1) {
  uint16_t day = G.dayCount;   // read outside the spinlock
  uint32_t now = millis();
  taskENTER_CRITICAL(&k10LogMux);
  uint8_t idx;
  if (k10LogCount < K10_LOG_SIZE) {
    idx = (k10LogHead + k10LogCount) % K10_LOG_SIZE;
    k10LogCount++;
  } else {
    idx = k10LogHead;
    k10LogHead = (k10LogHead + 1) % K10_LOG_SIZE;
  }
  strlcpy(k10Log[idx].text, text, sizeof(k10Log[idx].text));
  k10Log[idx].ms   = now;
  k10Log[idx].day  = day;
  k10Log[idx].who  = who;
  k10Log[idx].who2 = who2;
  k10Log[idx].tone = tone;
  if (k10LogTotal < 0xFFFF) k10LogTotal++;
  taskEXIT_CRITICAL(&k10LogMux);
  k10Dirty = true;
}

// ── Chronicle phrasing ─────────────────────────────────────────────────────
// K10_SAY picks one of several wordings for the same event so two forages in
// a row don't read like a stuck record. A rolling counter rather than random:
// deterministic, and free.
static uint8_t _k10Turn = 0;
static inline const char* k10Pick(const char* const* v, uint8_t n) {
  return v[_k10Turn++ % n];
}
#define K10_SAY(...) ({ static const char* const _v[] = { __VA_ARGS__ }; \
                        k10Pick(_v, (uint8_t)(sizeof(_v) / sizeof(_v[0]))); })
