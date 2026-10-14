#pragma once
// ── wind.hpp ──────────────────────────────────────────────────────────────
// The one wind, the board's half. data/wind.js draws it -- smoke, rain, ash,
// cloud shadows, dust devils all lean with it -- and this is the same closed
// form, term for term (windAt() there), evaluated on the board's own clock.
// Every state tick carries that clock as gs.wt (network-sync.hpp) and /state
// as "wt" (game-server.hpp), and the clients lock their wind to it, so the
// smoke a player watches is the wind the board is playing by:
//
//   * fire runs downwind      (world-system.hpp spreadFire / windSpreadFactor)
//   * scent carries downwind  (world-system.hpp hottestTrackWithin / windScentDist)
//
// millis() rather than NTP time: the board only has a wall clock on a network
// with internet, and none in AP mode. Uptime wraps after ~49 days, which
// turns the wind once. Nothing here is saved -- a reboot just picks the wind
// up somewhere else.
//
// Units: world hexes, screen-oriented, +y down -- x = 1.5 q,
// y = sqrt3/2 q + sqrt3 r -- so a heading means the same thing here as on
// the screen. Included from the .ino ahead of world-system.hpp.

static constexpr double WIND_V = 0.18;   // mean air speed, hexes/s (wind.js WIND_V)
struct WindTerm { double amp, per, ph; uint8_t axis; };   // axis 1 swings the heading, 0 its speed
static constexpr WindTerm WIND_TERMS[3] = {   // wind.js WIND_TERMS
  { 5.20, 300.0, 0.7, 1 },
  { 0.34,  71.0, 2.3, 1 },
  { 0.45,  47.0, 4.1, 0 },
};
// How hard it blows by weatherPhase: clear, rain, storm, chem, strangle fog,
// mist (wind.js WIND_WEATHER). Fog is still air.
static constexpr float WIND_WEATHER[6] = { 0.55f, 0.8f, 1.0f, 0.75f, 0.15f, 0.1f };
static constexpr double WIND_STORM_SLANT = 0.6;   // data/storm-field.js STORM_SLANT: the prevailing heading

struct WindNow {
  float x, y;   // heading, unit vector (the way it blows TO)
  float s;      // strength: weather x air speed, 1 = a storm at mean speed, <= 1.5
};

// The wind right now, in the current weather. Doubles: t is up to ~4e6 s.
static WindNow windNow() {
  const double t  = millis() / 1000.0;
  double ex = 2.0 / 3.0 - WIND_STORM_SLANT / 3.0, ey = WIND_STORM_SLANT / sqrt(3.0);
  const double el = sqrt(ex * ex + ey * ey);
  ex /= el; ey /= el;
  double a = WIND_V, c = 0;
  for (const WindTerm& k : WIND_TERMS) {
    const double w = 2.0 * M_PI / k.per;
    const double v = k.amp * w * cos(t * w + k.ph);
    if (k.axis) c += v; else a += v;
  }
  const double vx = ex * a - ey * c, vy = ey * a + ex * c;
  const double sp = fmax(1e-6, sqrt(vx * vx + vy * vy));
  const uint8_t ph = G.weatherPhase < 6 ? G.weatherPhase : 0;
  WindNow n;
  n.x = (float)(vx / sp);
  n.y = (float)(vy / sp);
  n.s = (float)fmin(1.5, WIND_WEATHER[ph] * sp / WIND_V);
  return n;
}

// How much the wind is blowing along an axial step (dq, dr): +1 straight
// down it, -1 straight into it, 0 across it.
static inline float windAlong(const WindNow& w, int dq, int dr) {
  const float x = 1.5f * dq, y = 0.8660254f * dq + 1.7320508f * dr;
  const float l = sqrtf(x * x + y * y);
  return l > 0 ? (x * w.x + y * w.y) / l : 0.0f;
}
