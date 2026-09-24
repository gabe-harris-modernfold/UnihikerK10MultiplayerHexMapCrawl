#pragma once
// ── ui-boot.hpp ─────────────────────────────────────────────────────────────
// Boot splash diagnostic log and thread-safe K10 event ring buffer.

// ── Boot splash diagnostic log ─────────────────────────────────────────────
// Once the SD card is up, the splash is the title art with only the newest
// line in a dark strip along the bottom. Before that (or if the art is missing,
// or splashFail() has fired) it is the full scrolling text log. Every line is
// kept in _sLog either way, so falling back to the log loses no history.
// Boot-only, so the log lives in PSRAM rather than internal .bss.
static const char* SPLASH_ART_PATH = "/data/img/wastelandTitle0.png";
static const int   SPLASH_STRIP_H  = 14;
static char       (*_sLog)[30] = nullptr;   // [14][30], allocated on first use
static uint8_t      _sN = 0;
static uint32_t     _sCol[14];
static LGFX_Sprite  _sArt;             // decoded title art, PSRAM; no buffer = text log
static bool         _sUsbHint = false; // "[A] USB" tag in the strip until the button check

static void splashRender() {
  if (_sArt.getBuffer()) {
    _sArt.pushSprite(&canvas, 0, 0);
    int y = canvas.height() - SPLASH_STRIP_H;
    canvas.fillRectAlpha(0, y, canvas.width(), SPLASH_STRIP_H, 180, 0x000000u);
    // The per-line colours are tuned for a black screen and several are too
    // dim to read over the art, so the strip uses one bone colour.
    if (_sN) canvasText8(_sLog[_sN - 1], 4, y + 3, 0xE8D8B0);
    if (_sUsbHint) canvasText8R("[A] USB", canvas.width() - 4, y + 3, 0x40A0E0);
  } else {
    canvas.fillScreen(0x0000);
    for (uint8_t i = 0; i < _sN; i++)
      canvasText8(_sLog[i], 4, 4 + i * 10, _sCol[i]);
  }
  canvas.pushSprite(0, 0);
}

static void splashAdd(const char* msg, uint32_t col = 0) {
  if (!_sLog) _sLog = (char(*)[30])psramStaticAlloc(14 * 30);
  if (_sN == 14) {
    for (int i = 0; i < 13; i++) { memcpy(_sLog[i], _sLog[i+1], 30); _sCol[i] = _sCol[i+1]; }
    _sN = 13;
  }
  _sCol[_sN] = col ? col : 0xD06818;
  snprintf(_sLog[_sN++], 30, "%s", msg);
  splashRender();
}

static void splashUsbHint(bool on) { _sUsbHint = on; }

// Decode the title art once into its own sprite; splashRender() copies it
// under the strip on every line. Needs SD mounted. Failure just keeps the log.
static void splashLoadArt() {
  _sArt.setPsram(true);
  _sArt.setColorDepth(16);
  if (!_sArt.createSprite(canvas.width(), canvas.height())) {
    Log.error("Splash art sprite alloc FAIL"); return;
  }
  uint32_t t0 = millis();
  if (!SD.exists(SPLASH_ART_PATH) || !_sArt.drawPngFile(SD, SPLASH_ART_PATH, 0, 0)) {
    Log.warning("Splash art missing or undecodable: %s", SPLASH_ART_PATH);
    _sArt.deleteSprite(); return;
  }
  Log.notice("Splash art %s decoded in %ums", SPLASH_ART_PATH, (unsigned)(millis() - t0));
}

// Boot is over (or failed): give the 150 KB back.
static void splashFreeArt() { _sArt.deleteSprite(); }

// Something is wrong enough that the reader needs the whole story: drop the
// art and show the full log from here on.
static void splashFail(const char* msg, uint32_t col = 0xC04020) {
  splashFreeArt();
  splashAdd(msg, col);
}

// ── The chronicle (thread-safe ring buffer) ────────────────────────────────
// `text` is a predicate when `who` names a player ("goes hungry for the
// trying.") and a whole sentence when who is -1. See K10LogEntry for the
// '\x01' second-name placeholder. `glyph` is the
// K10Glyph stamped in the margin beside the words. Safe from either core.
static void k10LogAddEx(const char* text, int8_t who, uint8_t tone,
                        uint8_t glyph, int8_t who2,
                        uint8_t plate, const uint8_t* pv, uint8_t nv) {
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
  k10Log[idx].glyph = glyph;
  k10Log[idx].plate = plate;
  memset(k10Log[idx].pv, 0, sizeof(k10Log[idx].pv));
  if (pv && nv) memcpy(k10Log[idx].pv, pv,
                       (nv < sizeof(k10Log[idx].pv)) ? nv : sizeof(k10Log[idx].pv));
  if (k10LogTotal < 0xFFFF) k10LogTotal++;
  taskEXIT_CRITICAL(&k10LogMux);
  k10Dirty = true;
}

// A line of handwriting — the common case.
static inline void k10LogAdd(const char* text, int8_t who = -1,
                             uint8_t tone = TONE_PLAIN, uint8_t glyph = GLY_NONE,
                             int8_t who2 = -1) {
  k10LogAddEx(text, who, tone, glyph, who2, PLATE_NONE, nullptr, 0);
}

// A plate: `text` is the citation or cause that goes on the block, and pv[]
// carries the figures the layout for this kind reads (see BOOK_PLATE).
static inline void k10LogPlate(uint8_t plate, const char* text, int8_t who,
                               uint8_t tone, const uint8_t* pv, uint8_t nv) {
  k10LogAddEx(text, who, tone, GLY_NONE, -1, plate, pv, nv);
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
