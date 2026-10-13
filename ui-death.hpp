#pragma once
// -- ui-death.hpp ------------------------------------------------------------
// The death takeover: when a survivor goes down, the LCD drops whatever it was
// showing and gives the whole 240x320 to the Creeping Doom, laughing at it.
// The picture is scDeathDraw (ui-scenes.hpp, section 12); the laugh it moves
// its jaw to is doom-laugh.h, the same score the speaker laughs to
// (sndStory(SS_DOWNED)) and the TV's skull laughs to (data/observer-fx.js).
//
// Same division of labour as ui-upload.hpp -- drainEvents() (Core 1) only
// writes state here, never the canvas; the LCD refresh path in the main loop
// checks DeathUI::isActive() and calls drawDeathScreen() every FRAME_MS, so
// the jaw is open when the syllable is heard.

namespace DeathUI {
  static volatile bool     _active = false;
  static volatile uint32_t _sinceMs = 0;
  static char              _who[16]  = {0};
  static char              _line[48] = {0};
  static uint8_t           _seat = 0, _laugh = 0;

  static constexpr uint32_t DEATH_HOLD_MS = 6000;
  static constexpr uint32_t FRAME_MS      = 66;    // a laugh is ~6 syllables a second

  // Starts the takeover and says which laugh it is: the caller hands the same
  // one to the speaker and to the TV. They take turns from wherever this boot
  // happened to start, so two deaths running never laugh the same way.
  inline uint8_t begin(const char* who, const char* line, uint8_t seat) {
    static uint8_t next = 255;
    if (next == 255) next = (uint8_t)(esp_random() % DOOM_LAUGH_N);
    strlcpy(_who,  who  ? who  : "Someone", sizeof(_who));
    strlcpy(_line, line ? line : "",        sizeof(_line));
    _seat  = seat;
    _laugh = next;
    next   = (uint8_t)((next + 1) % DOOM_LAUGH_N);
    _sinceMs = millis();
    _active  = true;
    return _laugh;
  }

  inline bool isActive() {
    if (!_active) return false;
    if (millis() - _sinceMs > DEATH_HOLD_MS) { _active = false; return false; }
    return true;
  }
}

static void drawDeathScreen() {
  SceneDeath D;
  memset(&D, 0, sizeof(D));
  memcpy(D.who, DeathUI::_who, sizeof(D.who));
  memcpy(D.line, DeathUI::_line, sizeof(D.line));
  D.seat  = DeathUI::_seat;
  D.laugh = DeathUI::_laugh;
  uint32_t now = millis();
  scDeathDraw(D, now, now - DeathUI::_sinceMs, (uint16_t*)canvas.getBuffer());
}
