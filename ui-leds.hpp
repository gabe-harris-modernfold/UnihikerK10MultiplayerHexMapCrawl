#pragma once
// ── ui-leds.hpp ─────────────────────────────────────────────────────────────
// The K10's three RGB LEDs as a miniature sky.
//
// The three lamps are NOT three copies of one colour — each has a distinct job,
// so the strip reads left-to-right as a slice of the horizon:
//
//   LED 0 — SUN      the time-of-day gradient run hot, gated by a sun-elevation
//                    envelope. Below the horizon it decays to a faint moon
//                    ember instead of going black. Also the lamp the party's
//                    own condition rides on: hunger browns it out, radiation
//                    turns it sick and makes it stutter.
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
// Under all three sits the DREAD layer (applyDread below), fed by the
// g_dread snapshot the game tick publishes: the threat clock, the Creeping
// Doom's distance, hunger, thirst, radiation, wounds, fire in the near field,
// and whether anyone is still down. None of it gets a lamp of its own. The
// landscape stays the landscape and every reading arrives as a DISTORTION of
// it — light drained, colour pulled, the breath made ragged, a pulse crossing
// the strip. It is still the sky; it is a sky seen through a worse day.
//
// The strip is rendered in layers, each one composited over the last:
//   1. perish alarm  (ledPerish — a survivor hit LL 0; red heartbeat, 2.6 s)
//                    outranks and replaces everything below.
//   2. lightning     (stormBolt — whites out the strip for a tick)
//   3. event cue     (ledCue — shaped, spanned, and MIXED over the sky so the
//                    world bleeds back through as the cue decays)
//   4. dread         (the party and what is hunting it, as distortion)
//   5. weather + time-of-day  (the landscape, animated at the ~10 Hz loop rate)
//
// Underground (every connected survivor in the tunnels) layer 5 is not
// modulated but REPLACED: there is no sky down there, just three dim lamps
// guttering out of phase. Surfacing brings the sky back in one tick, and that
// snap is the point.

// ── Small fixed-point colour helpers (t / s are 0-255) ──────────────────────
static inline uint8_t mixU8(uint8_t a, uint8_t b, uint8_t t) {
  return (uint8_t)(((uint16_t)a * (255 - t) + (uint16_t)b * t) / 255);
}
static inline uint8_t scaleU8(uint8_t v, uint8_t s) {
  return (uint8_t)(((uint16_t)v * s) / 255);
}
static inline uint8_t satU8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
static inline uint8_t lumaU8(uint8_t r, uint8_t g, uint8_t b) {
  return (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 151 + (uint16_t)b * 28) >> 8);
}
// Cheap integer hash. Everything that has to look random on a 100 ms tick --
// flicker, the geiger stutter, a lamp guttering underground -- derives from
// this rather than esp_random(), so one tick renders identically across all
// three lamps' passes and the strip never tears mid-frame.
static inline uint8_t hash8(uint32_t x) {
  x *= 2654435761u; x ^= x >> 13; x *= 1274126177u; x ^= x >> 16;
  return (uint8_t)(x & 0xFFu);
}
// Drain colour toward the lamp's own brightness. amt 0 = untouched, 255 = grey.
static inline void desatU8(uint8_t amt, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t y = lumaU8(r, g, b);
  r = mixU8(r, y, amt); g = mixU8(g, y, amt); b = mixU8(b, y, amt);
}
// Pull a lamp toward a target colour by amt.
static inline void pullU8(uint8_t tr, uint8_t tg, uint8_t tb, uint8_t amt,
                          uint8_t& r, uint8_t& g, uint8_t& b) {
  r = mixU8(r, tr, amt); g = mixU8(g, tg, amt); b = mixU8(b, tb, amt);
}
// Take light without touching hue. keep 255 = untouched, 0 = dark.
static inline void dimU8(uint8_t keep, uint8_t& r, uint8_t& g, uint8_t& b) {
  r = scaleU8(r, keep); g = scaleU8(g, keep); b = scaleU8(b, keep);
}

// ── Event cues (game events -> RGB LEDs) ────────────────────────────────────
// A cue is a colour plus a SHAPE (how it moves through time) and a SPAN (which
// lamps it touches). Crucially it COMPOSITES over the time-of-day/weather sky
// rather than replacing it, so the world bleeds back through as the cue decays
// and an event reads as happening *in* the sky instead of blanking it. The old
// ledFlash() did the opposite -- write(-1, ...) across all three lamps -- which
// threw away the sun/sky/horizon geography for its whole 300 ms.
//
// Shapes:
//   CUE_STAB     hard on, short tail             a hit landing
//   CUE_BLINK    square on/off, reps cycles      a signal, a threshold crossed
//   CUE_PULSE    smooth cosine, reps cycles      a throb, a double thump
//   CUE_FLICKER  random lit/dark ticks, decaying lightning, fire, bad air
//   CUE_SWELL    rise, hold, fall away           something gathering
//   CUE_CREEP    a bump travelling lamp to lamp  something arriving or leaving
//   CUE_BLOOM    out of the middle lamp          something spreading
//
// Spans: ALL / SUN / SKY / HORIZON pick lamps outright; INWARD and OUTWARD
// weight the three as a gradient, and also give CUE_CREEP its direction
// (OUTWARD = sun to horizon, anything else = horizon to sun).
//
// Everything is quantised to the ~10 Hz display loop, so STAB and BLINK are
// crisp, FLICKER is a per-tick coin flip (the same constraint BOLT_PATTERNS
// works around below), and the multi-second shapes come out perfectly smooth.
enum : uint8_t {
  CUE_STAB = 0, CUE_BLINK, CUE_PULSE, CUE_FLICKER, CUE_SWELL, CUE_CREEP, CUE_BLOOM,
};
enum : uint8_t {
  SPAN_ALL = 0, SPAN_SUN, SPAN_SKY, SPAN_HORIZON, SPAN_INWARD, SPAN_OUTWARD,
};
// A new cue wins only if it is at least as urgent as the one still running, so
// a caravan cannot stomp on the tail of a fire hit.
enum : uint8_t { CUEP_INFO = 1, CUEP_HURT = 2, CUEP_ALARM = 3 };

// Called from Core 1 (the game loop). Only records the cue; updateLEDs() on
// the display loop renders and expires it.
void ledCue(uint8_t r, uint8_t g, uint8_t b, uint8_t shape, uint8_t span,
            uint16_t ms, uint8_t prio, uint8_t reps) {
  uint32_t now = millis();
  if (g_cueStartMs && prio < g_cuePrio && (uint32_t)(now - g_cueStartMs) < g_cueMs) return;
  g_cueR = r; g_cueG = g; g_cueB = b;
  g_cueShape = shape; g_cueSpan = span;
  g_cueReps  = reps ? reps : 1;
  g_cuePrio  = prio;
  g_cueMs    = ms ? ms : 300;
  g_cueStartMs = now ? now : 1;   // written last; 0 is the "no cue" sentinel
}

// Compatibility shim: a plain stab across the whole strip, which is what every
// ledFlash() call site used to get. New code should pick a shape and a span.
void ledFlash(uint8_t r, uint8_t g, uint8_t b) {
  ledCue(r, g, b, CUE_STAB, SPAN_ALL, 300, CUEP_HURT, 1);
}

// How much of a lamp a span claims.
static uint8_t cueSpanWeight(uint8_t span, uint8_t lamp) {
  switch (span) {
    case SPAN_SUN:     return lamp == 0 ? 255 : 0;
    case SPAN_SKY:     return lamp == 1 ? 255 : 0;
    case SPAN_HORIZON: return lamp == 2 ? 255 : 0;
    case SPAN_INWARD:  return (uint8_t)(90 + 82 * lamp);    //  90/172/254, horizon hottest
    case SPAN_OUTWARD: return (uint8_t)(254 - 82 * lamp);   // 254/172/90,  sun hottest
    default:           return 255;
  }
}

// The shape envelope for one lamp. prog is 0-255 through the cue's duration.
static uint8_t cueShapeEnv(uint8_t shape, uint8_t reps, uint8_t prog,
                           uint8_t lamp, uint8_t span, uint32_t now) {
  switch (shape) {
    case CUE_BLINK: {
      uint16_t slots = (uint16_t)reps * 2;
      uint16_t slot  = (uint16_t)prog * slots / 256u;
      return (slot & 1) ? 0 : 255;
    }
    case CUE_PULSE: {
      float ph = (float)prog / 256.0f * (float)reps;
      ph -= (float)(int)ph;
      float k = 0.5f - 0.5f * cosf(6.2831853f * ph);
      // Fade the train out so the last beat is the softest one.
      return (uint8_t)(k * 255.0f * (1.0f - (float)prog / 320.0f));
    }
    case CUE_FLICKER: {
      uint8_t h   = hash8((now / 100u) * 2654435761u + (uint32_t)lamp * 40503u);
      uint8_t amp = (uint8_t)(255 - prog);
      return (h & 1) ? amp : (uint8_t)(amp / 5);
    }
    case CUE_SWELL: {
      if (prog <  90) return (uint8_t)((uint16_t)prog * 255u / 90u);
      if (prog < 150) return 255;
      return (uint8_t)(255 - (uint16_t)(prog - 150) * 255u / 106u);
    }
    case CUE_CREEP: {
      float pos = (float)prog / 255.0f * 2.0f;             // 0 -> 2 across the strip
      float me  = (span == SPAN_OUTWARD) ? (float)lamp : (float)(2 - lamp);
      float d   = fabsf(pos - me);
      if (d >= 1.0f) return 0;
      return (uint8_t)((1.0f - d) * 255.0f);
    }
    case CUE_BLOOM: {
      float radius = (float)prog / 255.0f * 2.0f;
      if (fabsf((float)lamp - 1.0f) > radius) return 0;
      return (uint8_t)(255 - (uint16_t)prog * 200u / 256u);
    }
    default: {  // CUE_STAB
      if (prog < 100) return 255;
      return (uint8_t)(255 - (uint16_t)(prog - 100) * 255u / 156u);
    }
  }
}

// True while a cue is live; fills prog with its 0-255 progress, and expires it.
static bool cueLive(uint32_t now, uint8_t& prog) {
  if (!g_cueStartMs) return false;
  uint16_t dur = g_cueMs ? g_cueMs : 1;
  uint32_t el  = now - g_cueStartMs;
  if (el >= dur) { g_cueStartMs = 0; g_cuePrio = 0; return false; }
  prog = (uint8_t)(el * 255u / dur);
  return true;
}

static void cueApply(uint32_t now, uint8_t prog, uint8_t lamp,
                     uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t env = cueShapeEnv(g_cueShape, g_cueReps, prog, lamp, g_cueSpan, now);
  // CREEP and BLOOM decide their own placement, so a span would double-dip.
  if (g_cueShape != CUE_CREEP && g_cueShape != CUE_BLOOM)
    env = scaleU8(env, cueSpanWeight(g_cueSpan, lamp));
  if (!env) return;
  r = mixU8(r, g_cueR, env); g = mixU8(g, g_cueG, env); b = mixU8(b, g_cueB, env);
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
  g_cueStartMs = 0; g_cuePrio = 0;  // drop any live cue — the alarm owns the lamps now
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


// ── Dread: the party, and what is hunting it ────────────────────────────────
// g_dread is published once per game tick by publishDread() in
// actions_game_loop.hpp (inside the G.mutex the tick already holds) and read
// lock-free here at ~10 Hz: slow data, fast animation. Every field is already
// normalised to 0-255 "how bad is it", so this file stays pure presentation.
//
// The copy goes through memcpy because an implicitly-declared copy constructor
// cannot bind to a volatile object. Byte-level tearing against the publisher
// is possible and cosmetically irrelevant: the worst case is one 100 ms frame
// blending two adjacent game ticks.
static DreadSnapshot dreadRead() {
  DreadSnapshot d;
  memcpy(&d, (const void*)&g_dread, sizeof(d));
  return d;
}

// The clock the weather breath runs on. Two dread readings bend it:
//   encActive  someone is mid-encounter, so the sky holds its breath -- frozen
//              at one phase until the encounter resolves.
//   woundLoad  a hurt party breathes raggedly, so the clock jitters per tick
//              and the smooth cosine of wxBreath() comes out uneven.
static uint32_t dreadBreathClock(uint32_t now, const DreadSnapshot& d) {
  if (d.encActive) return 0;
  if (!d.woundLoad) return now;
  uint8_t j = hash8(now / 100u);
  return now + (uint32_t)((uint16_t)j * d.woundLoad / 255u) * 2u;
}

// Every connected survivor is in the tunnels. Three dim lamps guttering out of
// phase with each other, and no sky at all -- that absence IS the read.
static void undergroundLamp(uint32_t now, uint8_t lamp,
                            uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t lvl = (uint8_t)(120 + (hash8(now / 200u + (uint32_t)lamp * 7717u) >> 2));
  r = scaleU8(190, lvl); g = scaleU8(108, lvl); b = scaleU8(38, lvl);
}

// Apply every dread reading to one lamp. lamp 0 = sun, 1 = sky, 2 = horizon.
// Order matters: the tints go on first and the light losses last, so attrition
// takes a bite out of the dread colours too rather than being painted over.
static void applyDread(const DreadSnapshot& d, uint32_t now, uint8_t lamp,
                       uint8_t& r, uint8_t& g, uint8_t& b) {
  // 1. Threat clock -- the strip loses conviction. Colour drains first, then
  //    the horizon takes a blood tint: the world going wrong from the edges
  //    in. Only at the top bands does it reach the sky lamp itself.
  if (d.tcWeight) {
    desatU8((uint8_t)(d.tcWeight / 3), r, g, b);
    if (lamp == 2)                        pullU8(90, 15, 15, (uint8_t)(d.tcWeight / 2), r, g, b);
    else if (lamp == 1 && d.tcLevel >= 3) pullU8(70, 22, 28, (uint8_t)(d.tcWeight / 4), r, g, b);
  }

  // 2. Hunger is the body's own fire going out, so it is the SUN that browns
  //    and dims -- not the sky. Thirst is the opposite reading: dehydration
  //    bleaches rather than darkens, so it washes the sky lamp pale.
  if (lamp == 0 && d.hunger) {
    pullU8(80, 50, 22, (uint8_t)(d.hunger / 2), r, g, b);
    dimU8((uint8_t)(255 - d.hunger / 3), r, g, b);
  }
  if (lamp == 1 && d.thirst) {
    desatU8((uint8_t)(d.thirst / 2), r, g, b);
    pullU8(205, 198, 170, (uint8_t)(d.thirst / 3), r, g, b);
  }

  // 3. Radiation -- a sick yellow-green on the sun lamp, with a geiger stutter
  //    over it: random 100 ms dropouts whose frequency climbs with the load,
  //    so a hot survivor makes the lamp visibly unwell rather than just tinted.
  if (lamp == 0 && d.radLoad) {
    pullU8(155, 200, 45, (uint8_t)(d.radLoad / 2), r, g, b);
    if (hash8(now / 100u + 7u) < (uint8_t)(d.radLoad / 3)) dimU8(70, r, g, b);
  }

  // 4. Fire in the near field -- an ember glow pushed into the horizon lamp,
  //    flickering on the 100 ms tick. Distinct from the CUE_FLICKER a fire HIT
  //    fires: this is the standing glow of a fire you are near but not in.
  if (lamp == 2 && d.fireClose) {
    uint8_t amt = scaleU8((uint8_t)(d.fireClose / 2),
                          (uint8_t)(170 + (hash8(now / 100u + 19u) >> 1)));
    pullU8(255, 95, 20, amt, r, g, b);
  }

  // 5. Creeping Doom sonar. The PERIOD is the whole message: ~2.4 s when it is
  //    aware but far, down to ~0.5 s when it is on top of someone -- the
  //    classic sonar speeding up, which you feel before you can name it.
  //    Cubed so it reads as a discrete ping rather than a sine, and it enters
  //    from the horizon, reaching the sky and finally the sun as it closes.
  if (d.doomClose) {
    uint16_t period = (uint16_t)(2400 - (uint16_t)d.doomClose * 1900 / 255);
    float ph = (float)(now % period) / (float)period;
    float k  = 0.5f - 0.5f * cosf(6.2831853f * ph);
    k = k * k * k;
    uint8_t reach = (lamp == 2) ? 255 : (lamp == 1) ? 150 : 70;
    pullU8(150, 40, 200,
           scaleU8(scaleU8((uint8_t)(k * 255.0f), d.doomClose), reach), r, g, b);
  }

  // 6. A survivor still down -- a slow, low red heartbeat under everything.
  //    The 2.6 s perish alarm says "someone just went down"; this says "and
  //    they are still down", and it does not stop until they are back up.
  if (d.downed) {
    float ph = (float)(now % 1500u) / 1500.0f;
    float k  = 0.5f - 0.5f * cosf(6.2831853f * ph);
    k *= k;
    uint8_t amt = (uint8_t)(k * (float)min(150, 55 + 32 * (int)d.downed));
    pullU8(190, 20, 20, amt, r, g, b);
  }

  // 7. Party attrition -- a strip that has simply lost light. Last, so it bites
  //    into every dread tint above as well as the sky underneath.
  if (d.attrition) dimU8((uint8_t)(255 - scaleU8(d.attrition, 90)), r, g, b);
}

// ── K10 LED update ──────────────────────────────────────────────────────────
static void updateLEDs() {
  uint32_t now = millis();

  // 1. Perish alarm outranks everything, including the user's dim preference -
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

  DreadSnapshot d = dreadRead();

  float t = (float)snapDayTick / (float)DAY_TICKS;
  if (t >= 1.0f) t = 0.99f;

  // 2. Lightning whites out the whole strip for a tick. stormBolt() is called
  //    unconditionally so its schedule keeps advancing while the party is
  //    underground, otherwise surfacing mid-storm fires a stale bolt at once.
  bool bolt = stormBolt(snapWeather, now);
  if (bolt && !d.allUnder) {
    k10.rgb->write(-1, 235, 240, 255);
    return;
  }

  uint8_t lr[3], lg[3], lb[3];

  if (d.allUnder) {
    for (uint8_t i = 0; i < 3; i++) undergroundLamp(now, i, lr[i], lg[i], lb[i]);
  } else {
    const WxLed& w  = WX_LED[snapWeather];
    uint32_t     bc = dreadBreathClock(now, d);
    uint8_t breathMid = wxBreath(w, bc, false, 255);   // sky
    uint8_t breathSun = wxBreath(w, bc, false, 128);   // sun resists the dip
    uint8_t breathLow = wxBreath(w, bc, true,  255);   // horizon, half a cycle behind

    // LED 0 - SUN
    todColourAt(t, lr[0], lg[0], lb[0]);
    uint8_t elev = sunElevation(t);
    // Run hot in proportion to elevation - red hardest, green less, blue never,
    // so midday reads as a warm lamp rather than a brighter copy of the sky.
    lr[0] = satU8(lr[0] + scaleU8(lr[0], (uint8_t)(elev / 3)));
    lg[0] = satU8(lg[0] + scaleU8(lg[0], (uint8_t)(elev / 5)));
    // Below the horizon it decays into a faint moon ember instead of going
    // dark. The hand-off is deliberately much sharper than the elevation ramp:
    // a low sun at dawn is still the brightest thing in the sky, so anything
    // above elev 64 is fully lit and only a genuinely setting sun fades.
    uint8_t lit = (elev >= 64) ? 255 : (uint8_t)(elev * 4);
    lr[0] = mixU8(22, lr[0], lit);
    lg[0] = mixU8(26, lg[0], lit);
    lb[0] = mixU8(52, lb[0], lit);
    applyWeather(snapWeather, 150, breathSun, lr[0], lg[0], lb[0]);

    // LED 1 - SKY (samples slightly ahead of now)
    todColourAt(t + 0.03f, lr[1], lg[1], lb[1]);
    applyWeather(snapWeather, 218, breathMid, lr[1], lg[1], lb[1]);

    // LED 2 - HORIZON (samples slightly behind, dimmer, hit hardest)
    todColourAt(t - 0.03f, lr[2], lg[2], lb[2]);
    dimU8(215, lr[2], lg[2], lb[2]);
    applyWeather(snapWeather, 255, breathLow, lr[2], lg[2], lb[2]);

    // 3. Dread distorts the landscape rather than replacing it.
    for (uint8_t i = 0; i < 3; i++) applyDread(d, now, i, lr[i], lg[i], lb[i]);
  }

  // 4. The event cue composites on top, so the sky bleeds back through as it
  //    decays instead of the event blanking the strip for its whole duration.
  uint8_t prog = 0;
  if (cueLive(now, prog))
    for (uint8_t i = 0; i < 3; i++) cueApply(now, prog, i, lr[i], lg[i], lb[i]);

  for (uint8_t i = 0; i < 3; i++) k10.rgb->write(i, lr[i], lg[i], lb[i]);
}
