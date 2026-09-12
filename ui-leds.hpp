#pragma once
// ── ui-leds.hpp ─────────────────────────────────────────────────────────────
// The K10's three RGB LEDs as a miniature sky.
//
// The three lamps are NOT three copies of one colour — each has a distinct job,
// so the strip reads left-to-right as a slice of the horizon:
//
//   LED 0 — SUN      the time-of-day gradient run hot, gated by a sun-elevation
//                    envelope. Below the horizon it decays to a faint moon
//                    ember instead of going black. Also carries the score
//                    pulse (k10LedPulse) from checkScoreAudio().
//   LED 1 — SKY      the ambient reference: the same gradient sampled slightly
//                    AHEAD of now, with a slow breath so it is never static.
//   LED 2 — HORIZON  the gradient sampled slightly BEHIND now, dimmer, and the
//                    lamp the weather bites hardest. Its breath runs in
//                    antiphase with the sky so the strip visibly moves.
//
// Over all three sits the weather (WX_LED below): a colour the sky is pulled
// toward, a light loss, and a per-phase animation — rain shimmers, storm
// throws lightning, chem pulses acid, Strangle Fog suffocates, mist sits flat.
//
// Priority, highest first:
//   1. perish alarm  (ledPerish — a survivor hit LL 0; red heartbeat, 2.6 s)
//   2. event flash   (ledFlash — 300 ms, held by loop() via g_ledEndMs)
//   3. score pulse   (k10LedPulse — LED 0 only)
//   4. weather + time-of-day  (the default, animated at the ~10 Hz loop rate)

// ── Small fixed-point colour helpers (t / s are 0-255) ──────────────────────
static inline uint8_t mixU8(uint8_t a, uint8_t b, uint8_t t) {
  return (uint8_t)(((uint16_t)a * (255 - t) + (uint16_t)b * t) / 255);
}
static inline uint8_t scaleU8(uint8_t v, uint8_t s) {
  return (uint8_t)(((uint16_t)v * s) / 255);
}
static inline uint8_t satU8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// ── LED flash (game events → RGB LEDs) ─────────────────────────────────────
// Called from Core 1 (game loop). Sets LED immediately; loop() turns it off.
void ledFlash(uint8_t r, uint8_t g, uint8_t b) {
  g_ledR = r; g_ledG = g; g_ledB = b;
  g_ledEndMs = millis() + 300;
  k10.rgb->write(-1, r, g, b);
}

// ── Perish alarm ────────────────────────────────────────────────────────────
// A survivor has dropped to LL 0. Runs a hard red heartbeat across all three
// lamps for PERISH_MS, outranking weather, time-of-day and any event flash.
// Fired from the single EVT_DOWNED funnel in network-events.hpp, so every
// death path (dusk, dawn upkeep, encounter, fire, chem, Strangle Fog) shows it.
static constexpr uint32_t PERISH_MS      = 2600;  // total alarm duration
static constexpr uint32_t PERISH_BEAT_MS = 700;   // one heartbeat cycle

void ledPerish() {
  g_ledPerishEndMs = millis() + PERISH_MS;
  g_ledEndMs = 0;  // drop any routine flash — the alarm owns the lamps now
}

// Fills r/g/b with the current beat and returns true while the alarm is live.
// The pattern is quantised to the 100 ms loop tick: two lit ticks, a gap, one
// dim echo, then dark — a double-thump that reads clearly as an alarm.
static bool perishColour(uint32_t now, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!g_ledPerishEndMs) return false;
  if (now >= g_ledPerishEndMs) { g_ledPerishEndMs = 0; return false; }
  uint32_t elapsed = PERISH_MS - (g_ledPerishEndMs - now);
  uint8_t  step    = (uint8_t)((elapsed % PERISH_BEAT_MS) / 100u);
  if      (step <= 1) { r = 255; g = 0; b = 0; }   // thump
  else if (step == 3) { r = 110; g = 0; b = 0; }   // echo
  else                { r = 0;   g = 0; b = 0; }   // dark
  return true;
}

// ── Time-of-day gradient ─────────────────────────────────────────────────────
// Sampled by fraction-of-day. NOTE t=0.0 is DAWN, not midnight: tickGame()
// zeroes G.dayTick at the day rollover and fires dawnUpkeep() on the same tick
// (actions_game_loop.hpp), so the day runs dawn → night → next dawn and the
// first stop has to be sunrise or the lamps show night while the board is
// announcing "Day N dawn". Denser than a simple dawn/day/dusk ramp so the slow
// crawl through morning and golden hour is visible on a lamp that only
// changes every 100 ms.
struct TodStop { float pct; uint8_t r, g, b; };
static const TodStop TOD_STOPS[] = {
  { 0.00f, 205,  85,  25 },  // DAWN — the exact tick dawnUpkeep() fires
  { 0.06f, 235, 160,  60 },  // morning gold
  { 0.18f, 150, 180, 210 },  // the warmth burns off into haze
  { 0.32f,  90, 140, 215 },  // high day blue
  { 0.44f, 120, 165, 220 },  // early afternoon
  { 0.56f, 225, 150,  45 },  // golden hour
  { 0.66f, 225,  85,  15 },  // sunset orange
  { 0.74f, 150,  30,  30 },  // last red on the rim
  { 0.80f,  60,  20,  70 },  // twilight violet — duskCheck() territory
  { 0.88f,  18,  12,  62 },  // night indigo — the darkest the sky gets
  { 0.94f,  60,  22,  50 },  // the rim starts to turn again
  { 1.00f, 205,  85,  25 },  // must equal the 0.00 stop, or the rollover
};                           // shows as a visible jump on the lamps
static constexpr uint8_t TOD_N = sizeof(TOD_STOPS) / sizeof(TOD_STOPS[0]);

static inline float wrap01(float t) {
  while (t < 0.0f)  t += 1.0f;
  while (t >= 1.0f) t -= 1.0f;
  return t;
}

static void todColourAt(float t, uint8_t& r, uint8_t& g, uint8_t& b) {
  t = wrap01(t);
  for (uint8_t i = 0; i < TOD_N - 1; i++) {
    if (t >= TOD_STOPS[i].pct && t < TOD_STOPS[i+1].pct) {
      float frac = (t - TOD_STOPS[i].pct) / (TOD_STOPS[i+1].pct - TOD_STOPS[i].pct);
      r = (uint8_t)(TOD_STOPS[i].r + frac * (TOD_STOPS[i+1].r - TOD_STOPS[i].r));
      g = (uint8_t)(TOD_STOPS[i].g + frac * (TOD_STOPS[i+1].g - TOD_STOPS[i].g));
      b = (uint8_t)(TOD_STOPS[i].b + frac * (TOD_STOPS[i+1].b - TOD_STOPS[i].b));
      return;
    }
  }
  r = TOD_STOPS[TOD_N-1].r; g = TOD_STOPS[TOD_N-1].g; b = TOD_STOPS[TOD_N-1].b;
}

// How high the sun sits, 0 (below the horizon) to 255 (full day). Drives the
// sun lamp's heat and its decay into the moon ember after dusk. Phased to
// match TOD_STOPS above: t=0 is dawn, so the sun starts already on the rim
// (60, not 0) rather than ramping up from nothing.
static uint8_t sunElevation(float t) {
  t = wrap01(t);
  if (t > 0.78f) return 0;                                        // below the horizon
  if (t < 0.08f) return (uint8_t)(60.0f + 195.0f * (t / 0.08f));  // clearing the rim
  if (t > 0.62f) return (uint8_t)(255.0f * (0.78f - t) / 0.16f);  // going down
  return 255;
}

// ── Weather signature ────────────────────────────────────────────────────────
// One row per WEATHER_* phase, indexed by G.weatherPhase.
//   r/g/b     the colour the sky is pulled toward
//   tint      how hard it pulls (0 = leave the sky alone, 255 = replace it)
//   dim       overall light left after the weather eats it (255 = no loss)
//   periodMs  animation period, 0 = flat
//   depth     how far the animation dips the light at its trough
// Note this is not one "severity" axis. STORM and Strangle FOG take the light
// (dim 140/130); CHEM and MIST keep most of it (215/205) and instead replace
// its colour outright (tint 205/180) — an acid glare and a flat whiteout are
// bright, not dark, which is also why both carry a heavy WEATHER_VIS_PENALTY.
// MIST is the one phase with no animation at all, so the three lamps collapse
// to near-identical grey: the strip going uniform IS the mist read.
struct WxLed { uint8_t r, g, b, tint, dim; uint16_t periodMs; uint8_t depth; };
static const WxLed WX_LED[6] = {
  /* CLEAR */ {   0,   0,   0,   0, 255, 9000,  22 },  // untinted; a slow calm shimmer only
  /* RAIN  */ {  70,  95, 130, 115, 190, 1600,  55 },  // cool slate, steady drizzle shimmer
  /* STORM */ {  45,  55,  95, 165, 140, 2400,  90 },  // bruised blue-grey + lightning (below)
  /* CHEM  */ { 150, 200,  40, 205, 215, 1100, 105 },  // sickly acid green, fast nauseous pulse
  /* FOG   */ {  95, 115,  95, 215, 130, 4000, 140 },  // Strangle Fog: murk that nearly closes
  /* MIST  */ { 185, 195, 200, 180, 205,    0,   0 },  // plain whiteout — deliberately dead flat
};

// Breath envelope for a phase: 255 = full light, lower = into the dip.
// depthScale trims the dip per-lamp; antiphase offsets the horizon half a cycle
// behind the sky so the two lamps visibly trade places.
static uint8_t wxBreath(const WxLed& w, uint32_t now, bool antiphase, uint8_t depthScale) {
  if (!w.periodMs || !w.depth) return 255;
  float ph = (float)(now % w.periodMs) / (float)w.periodMs;
  if (antiphase) ph = wrap01(ph + 0.5f);
  float k = 0.5f - 0.5f * cosf(6.2831853f * ph);           // 0 → 1 → 0, smooth
  return (uint8_t)(255 - (uint8_t)((float)w.depth * (depthScale / 255.0f) * k));
}

// Pull a lamp toward the weather and take its light. strength trims the tint
// per-lamp (the sun resists, the horizon takes it worst); breath is the
// envelope from wxBreath().
static void applyWeather(uint8_t phase, uint8_t strength, uint8_t breath,
                         uint8_t& r, uint8_t& g, uint8_t& b) {
  if (phase >= 6) return;
  const WxLed& w = WX_LED[phase];
  if (w.tint) {
    uint16_t pull = (uint16_t)w.tint * strength / 255;
    if (pull > 255) pull = 255;
    r = mixU8(r, w.r, (uint8_t)pull);
    g = mixU8(g, w.g, (uint8_t)pull);
    b = mixU8(b, w.b, (uint8_t)pull);
  }
  uint8_t lvl = scaleU8(w.dim, breath);
  r = scaleU8(r, lvl); g = scaleU8(g, lvl); b = scaleU8(b, lvl);
}

// Lightning, STORM only. The loop runs at ~10 Hz so a bolt is a pattern of lit
// / dark 100 ms ticks rather than a real sub-frame strobe — consumed LSB-first,
// which gives strike / gap / strike doubles that read as a proper flicker.
static const uint8_t BOLT_PATTERNS[4] = { 0b00000001, 0b00000101, 0b00001011, 0b00010101 };
static uint8_t  s_boltTicks   = 0;   // ticks left in the current bolt
static uint8_t  s_boltBits    = 0;   // remaining lit/dark ticks, LSB first
static uint32_t s_nextBoltMs  = 0;

static bool stormBolt(uint8_t phase, uint32_t now) {
  if (phase != WEATHER_STORM) { s_boltTicks = 0; s_nextBoltMs = 0; return false; }
  if (!s_boltTicks && now >= s_nextBoltMs) {
    s_boltBits   = BOLT_PATTERNS[esp_random() % 4];
    s_boltTicks  = 6;
    s_nextBoltMs = now + 1800 + (esp_random() % 4200);   // next front in 1.8-6 s
  }
  if (!s_boltTicks) return false;
  bool lit = (s_boltBits & 1) != 0;
  s_boltBits >>= 1;
  s_boltTicks--;
  return lit;
}

// Colour used to announce a weather change (EVT_WEATHER → ledFlash). The
// WX_LED tints are ambient-strength, so normalise the phase colour to full
// brightness; CLEAR has no tint at all and gets a clean sky blue instead.
static void weatherFlashColour(uint8_t phase, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (phase >= 6 || phase == WEATHER_CLEAR) { r = 120; g = 175; b = 235; return; }
  const WxLed& w = WX_LED[phase];
  uint8_t peak = max(w.r, max(w.g, w.b));
  if (!peak) { r = w.r; g = w.g; b = w.b; return; }
  r = (uint8_t)((uint16_t)w.r * 255 / peak);
  g = (uint8_t)((uint16_t)w.g * 255 / peak);
  b = (uint8_t)((uint16_t)w.b * 255 / peak);
}

// ── K10 LED update ───────────────────────────────────────────────────────────
static void updateLEDs() {
  uint32_t now = millis();

  // 1. Perish alarm outranks everything, including the user's dim preference —
  //    bump brightness so the heartbeat still reads on a dark-set board.
  uint8_t pr, pg, pb;
  if (perishColour(now, pr, pg, pb)) {
    k10.rgb->brightness((uint8_t)min(9, (int)s_ledBright + 3));
    k10.rgb->write(-1, pr, pg, pb);
    return;
  }

  k10.rgb->brightness(s_ledBright);

  uint32_t snapDayTick = 0;
  uint8_t  snapWeather = WEATHER_CLEAR;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    snapDayTick = G.dayTick;
    snapWeather = G.weatherPhase;
    xSemaphoreGive(G.mutex);
  }
  if (snapWeather >= 6) snapWeather = WEATHER_CLEAR;

  float t = (float)snapDayTick / (float)DAY_TICKS;
  if (t >= 1.0f) t = 0.99f;

  // 2. Lightning whites out the whole strip for a tick.
  if (stormBolt(snapWeather, now)) {
    k10.rgb->write(-1, 235, 240, 255);
    return;
  }

  const WxLed& w = WX_LED[snapWeather];
  uint8_t breathMid = wxBreath(w, now, false, 255);   // sky
  uint8_t breathSun = wxBreath(w, now, false, 128);   // sun resists the dip
  uint8_t breathLow = wxBreath(w, now, true,  255);   // horizon, half a cycle behind

  // ── LED 0 — SUN ────────────────────────────────────────────────────────────
  uint8_t sr, sg, sb;
  todColourAt(t, sr, sg, sb);
  uint8_t elev = sunElevation(t);
  // Run hot in proportion to elevation — red hardest, green less, blue never,
  // so midday reads as a warm lamp rather than a brighter copy of the sky.
  sr = satU8(sr + scaleU8(sr, (uint8_t)(elev / 3)));
  sg = satU8(sg + scaleU8(sg, (uint8_t)(elev / 5)));
  // Below the horizon it decays into a faint moon ember instead of going dark.
  // The hand-off is deliberately much sharper than the elevation ramp: a low
  // sun at dawn is still the brightest thing in the sky, so anything above
  // elev 64 is fully lit and only a genuinely setting sun fades to the ember.
  uint8_t lit = (elev >= 64) ? 255 : (uint8_t)(elev * 4);
  sr = mixU8(22, sr, lit); sg = mixU8(26, sg, lit); sb = mixU8(52, sb, lit);
  applyWeather(snapWeather, 150, breathSun, sr, sg, sb);

  // ── LED 1 — SKY (samples slightly ahead of now) ─────────────────────────────
  uint8_t kr, kg, kb;
  todColourAt(t + 0.03f, kr, kg, kb);
  applyWeather(snapWeather, 218, breathMid, kr, kg, kb);

  // ── LED 2 — HORIZON (samples slightly behind, dimmer, hit hardest) ──────────
  uint8_t hr, hg, hb;
  todColourAt(t - 0.03f, hr, hg, hb);
  hr = scaleU8(hr, 215); hg = scaleU8(hg, 215); hb = scaleU8(hb, 215);
  applyWeather(snapWeather, 255, breathLow, hr, hg, hb);

  // 3. Score pulse still owns LED 0 while it runs.
  if (k10LedPulse && now < k10LedPulse) {
    k10.rgb->write(0, k10PulseR, k10PulseG, k10PulseB);
  } else {
    k10LedPulse = 0;
    k10.rgb->write(0, sr, sg, sb);
  }
  k10.rgb->write(1, kr, kg, kb);
  k10.rgb->write(2, hr, hg, hb);
}
