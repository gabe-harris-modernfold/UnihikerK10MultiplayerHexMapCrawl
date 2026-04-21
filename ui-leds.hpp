#pragma once
// ── ui-leds.hpp ─────────────────────────────────────────────────────────────
// LED flash for game events, time-of-day colour gradient, and periodic LED sync.

// ── LED flash (game events → RGB LEDs) ─────────────────────────────────────
// Called from Core 1 (game loop). Sets LED immediately; loop() turns it off.
void ledFlash(uint8_t r, uint8_t g, uint8_t b) {
  g_ledR = r; g_ledG = g; g_ledB = b;
  g_ledEndMs = millis() + 300;
  k10.rgb->write(-1, r, g, b);
}

// ── Time-of-day LED colour ────────────────────────────────────────────────────
struct TodStop { float pct; uint8_t r, g, b; };
static void todColour(uint32_t dayTick, uint8_t& r, uint8_t& g, uint8_t& b) {
  static const TodStop STOPS[] = {
    { 0.00f, 180,  55,   5 },
    { 0.08f, 200, 130,  15 },
    { 0.30f,  40,  90, 180 },
    { 0.60f, 200, 110,  10 },
    { 0.80f, 160,  25,   5 },
    { 0.92f,  10,   8,  60 },
    { 1.00f,  10,   8,  60 },
  };
  constexpr uint8_t N = sizeof(STOPS) / sizeof(STOPS[0]);
  float t = (float)dayTick / (float)DAY_TICKS;
  if (t >= 1.0f) t = 0.99f;
  for (uint8_t i = 0; i < N - 1; i++) {
    if (t >= STOPS[i].pct && t < STOPS[i+1].pct) {
      float frac = (t - STOPS[i].pct) / (STOPS[i+1].pct - STOPS[i].pct);
      r = (uint8_t)(STOPS[i].r + frac * (STOPS[i+1].r - STOPS[i].r));
      g = (uint8_t)(STOPS[i].g + frac * (STOPS[i+1].g - STOPS[i].g));
      b = (uint8_t)(STOPS[i].b + frac * (STOPS[i+1].b - STOPS[i].b));
      return;
    }
  }
  r = STOPS[N-1].r; g = STOPS[N-1].g; b = STOPS[N-1].b;
}

// ── K10 LED update ───────────────────────────────────────────────────────────
static void updateLEDs() {
  k10.rgb->brightness(s_ledBright);
  uint32_t now = millis();

  uint32_t snapDayTick = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    snapDayTick = G.dayTick;
    xSemaphoreGive(G.mutex);
  }

  uint8_t tr, tg, tb;
  todColour(snapDayTick, tr, tg, tb);

  if (k10LedPulse && now < k10LedPulse) {
    k10.rgb->write(0, k10PulseR, k10PulseG, k10PulseB);
  } else {
    k10LedPulse = 0;
    k10.rgb->write(0, tr, tg, tb);
  }

  k10.rgb->write(1, tr, tg, tb);
  k10.rgb->write(2, tr, tg, tb);
}
