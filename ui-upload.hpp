#pragma once
// ── ui-upload.hpp ────────────────────────────────────────────────────────────
// On-device "FILE UPLOAD" status screen. Driven by the /upload handler in
// game-server.hpp; rendered by the main loop's LCD refresh path.
//
// The handler (AsyncTCP task) only writes state here — it never touches the
// canvas. The LCD refresh task in the main loop checks UploadUI::isActive()
// and calls drawUploadScreen() when true. Active window auto-clears 1500 ms
// after the last chunk, so back-to-back files don't flicker the gameplay UI.

namespace UploadUI {
  static volatile bool     _active   = false;
  static volatile uint32_t _lastMs   = 0;
  static volatile size_t   _bytes    = 0;
  static volatile bool     _flashEnd = false;
  static volatile bool     _flashOk  = true;
  static char              _dest[64] = {0};

  inline void begin(const char* dest) {
    _bytes    = 0;
    _flashEnd = false;
    _flashOk  = true;
    if (dest) strlcpy(_dest, dest, sizeof(_dest));
    else      _dest[0] = '\0';
    _lastMs = millis();
    _active = true;
  }

  inline void chunk(size_t totalSoFar) {
    _bytes  = totalSoFar;
    _lastMs = millis();
    _active = true;
  }

  inline void end(bool ok) {
    _flashEnd = true;
    _flashOk  = ok;
    _lastMs   = millis();
  }

  // True while an upload is in flight, or for ~1.5 s after the last chunk so
  // the OK/FAIL flash is visible. The LCD refresh path calls this every tick.
  inline bool isActive() {
    if (!_active) return false;
    if (millis() - _lastMs > 1500) { _active = false; return false; }
    return true;
  }
}

static void drawUploadScreen() {
  // Banner colour: teal while streaming, green/red on the post-final flash.
  uint32_t banner = 0x0070C0;
  bool     flash  = UploadUI::_flashEnd;
  if (flash) banner = UploadUI::_flashOk ? 0x208040 : 0xA02018;

  canvas.fillScreen(0x0000);
  canvasRect(0, 0, 240, 36, banner, true);
  canvasText24("FILE UPLOAD", 12, 6, 0xFFFFFF);
  canvasLine(0, 38, 239, 38, 0x004080);

  canvasText16("Destination", 8, 50, 0x60C0E0);
  // Two-line wrap for long paths (e.g. /data/img/survivors/foo.jpg).
  const char* d = UploadUI::_dest;
  size_t dlen = strlen(d);
  if (dlen <= 30) {
    canvasText8(d, 8, 72, 0xC8E0F0);
  } else {
    char l1[31]; strlcpy(l1, d, 31);
    canvasText8(l1,        8, 72, 0xC8E0F0);
    canvasText8(d + 30,    8, 84, 0xC8E0F0);
  }

  char buf[40];
  if (UploadUI::_bytes >= 1024)
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(UploadUI::_bytes / 1024));
  else
    snprintf(buf, sizeof(buf), "%u bytes", (unsigned)UploadUI::_bytes);
  canvasText16(buf, 8, 104, 0xFFC040);

  // Indeterminate scrolling progress bar (no Content-Length on chunked POST).
  canvasRect(8, 140, 232, 162, 0x202830, true);
  uint32_t t = millis();
  int barW = 30;
  int span = 232 - 8 - barW;
  int barX = 8 + (int)((t / 12) % (uint32_t)span);
  canvasRect(barX, 142, barX + barW, 160, 0x00C0FF, true);

  canvasLine(0, 180, 239, 180, 0x202830);
  canvasText8("Live HTTP /upload POST",   8, 188, 0x808090);
  canvasText8("(scripts/sync_data.ps1)",  8, 200, 0x606070);
  canvasText8("Hold A at boot = USB-MSC", 8, 220, 0x606070);

  if (flash) {
    canvasText24(UploadUI::_flashOk ? "OK"   : "FAIL",
                 UploadUI::_flashOk ? 92     : 80,
                 252,
                 UploadUI::_flashOk ? 0x60FF60 : 0xFF6060);
  }
}
