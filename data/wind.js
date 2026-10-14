// ── Wind ───────────────────────────────────────────────────────────────────
// One wind for the whole surface map. Everything that drifts reads it -- fire
// smoke, ash, rain, fog puffs, quake dust, bike dust, debris plumes, spores,
// the daisies, and ground-fx.js's cloud shadows, gust patches and dust devils
// -- so they all lean the same way at the same moment. Smoke that leans one
// way reads as weather; smoke where every puff picks its own way reads as a
// particle system.
//
// It is not just scenery: the board reads the same wind (wind.hpp windNow(),
// the C++ twin of windAt() below). Fire runs downwind, and the Creeping Doom
// smells a trail from further when the wind carries it its way. So the air
// runs off the BOARD's clock: every state tick carries the board's uptime as
// gs.wt, Wind.serverClock() locks onto it, and the smoke a player watches is
// the wind the server is spreading fire with. Until the first tick arrives
// (or against firmware too old to send wt) it falls back to the wall clock.
// Either way it is a function of time, not of frames, so every phone at the
// table sees the same air: the same cloud shadow over the same hex. The air
// mass's position is written first, in closed form (windAirDrift), and the
// wind is its derivative -- a cloud carried by the air and a puff pushed by
// the wind can never disagree about which way the air is going.
//
// Prevailing heading: the way storm fronts travel (storm-field.js advances
// along q + STORM_SLANT*r), so a squall arrives on the wind that carries it.
// Around that it wanders on three slow incommensurate terms, so the heading
// swings tens of degrees over minutes and the speed breathes, never on a beat.
//
// Units: world hexes (1 = HEX_SZ px), screen-oriented, +y down:
//   x = 1.5 q,  y = sqrt3/2 q + sqrt3 r   (renderer.js hexToPixel / HEX_SZ)
// Plain globals, no ES module syntax, like the rest of data/.

const WIND_EPOCH_MS = 1760000000000;  // keeps t in the tens of millions of seconds
const WIND_V        = 0.18;           // mean air speed, hexes per second
// [amplitude (hexes), period (s), phase, axis]: axis 1 swings the heading,
// axis 0 speeds the air up and slows it down along it.
// Roughly +-35 degrees of heading over five minutes, a few degrees of
// shiver on top, and the speed breathing by a third.
const WIND_TERMS = [
  [5.20, 300, 0.7, 1],
  [0.34,  71, 2.3, 1],
  [0.45,  47, 4.1, 0],
];
// How hard it blows, by weatherPhase: clear, rain, storm, chem, strangle fog,
// mist. Fog is still air -- that is why it sits.
const WIND_WEATHER = [0.55, 0.8, 1.0, 0.75, 0.15, 0.1];
// Gust patches ride the air a little faster than the cloud deck does.
const WIND_GUST_RATE = 1.6;
// Gust field: [cells across MAP_COLS, cells down MAP_ROWS, weight]. The cell
// counts have to divide into the map so the field tiles the torus.
const WIND_GUST_OCTAVES = [[25, 19, 0.62], [50, 38, 0.38]];
const WIND_GUST_SEED    = 7331;

function windPrevailing() {
  const s = typeof STORM_SLANT === 'number' ? STORM_SLANT : 0.6;
  const x = 2 / 3 - s / 3, y = s / SQRT3;
  const l = Math.hypot(x, y);
  return { x: x / l, y: y / l };
}

// Where the air that was at the origin at WIND_EPOCH_MS is now, world hexes.
function windAirDrift(t) {
  const e = windPrevailing();
  let a = WIND_V * t, c = 0;
  for (const [amp, per, ph, ax] of WIND_TERMS) {
    const v = amp * Math.sin(t * 2 * Math.PI / per + ph);
    if (ax) c += v; else a += v;
  }
  return { x: e.x * a - e.y * c, y: e.y * a + e.x * c };
}

// Its derivative: the wind, world hexes per second.
function windAirVelocity(t) {
  const e = windPrevailing();
  let a = WIND_V, c = 0;
  for (const [amp, per, ph, ax] of WIND_TERMS) {
    const w = 2 * Math.PI / per;
    const v = amp * w * Math.cos(t * w + ph);
    if (ax) c += v; else a += v;
  }
  return { x: e.x * a - e.y * c, y: e.y * a + e.x * c };
}

// The wind the board plays by at time t (s on its clock) in weatherPhase:
// heading (unit, the way it blows TO) and strength = weather x air speed,
// 1 being a storm at mean speed. No gusts -- those are the client's scenery.
// wind.hpp windNow() is this, line for line; mock-server/server.js loads this
// file to get it. Keep the three in step.
function windAt(t, phase) {
  const v = windAirVelocity(t);
  const sp = Math.hypot(v.x, v.y) || 1e-6;
  const wf = WIND_WEATHER[phase] ?? WIND_WEATHER[0];
  return { x: v.x / sp, y: v.y / sp, s: Math.min(1.5, wf * sp / WIND_V) };
}

// World hexes -> axial (q, r), unwrapped.
function windWorldToAxial(wx, wy) {
  const q = wx / 1.5;
  return { q, r: wy / SQRT3 - q / 2 };
}

// ── Torus value noise ──────────────────────────────────────────────────────
// A lattice of nq x nr cells laid over the whole MAP_COLS x MAP_ROWS map, so
// the field wraps exactly where the map does. Shared by the CPU gust lookup
// below and the textures ground-fx.js bakes from it, which is what keeps a
// daisy's flutter and the gust patch drawn over its hex in step.
function windLatticeHash(i, j, seed) {
  let h = (Math.imul(i, 374761393) + Math.imul(j, 668265263) + Math.imul(seed, 2246822519)) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  h = (h ^ (h >>> 16)) >>> 0;
  return h / 4294967295;
}

function windNoise(q, r, nq, nr, seed) {
  const u = (q / MAP_COLS) * nq, v = (r / MAP_ROWS) * nr;
  const i0 = Math.floor(u), j0 = Math.floor(v);
  const fu = u - i0, fv = v - j0;
  const su = fu * fu * (3 - 2 * fu), sv = fv * fv * (3 - 2 * fv);
  const ia = ((i0 % nq) + nq) % nq, ib = (ia + 1) % nq;
  const ja = ((j0 % nr) + nr) % nr, jb = (ja + 1) % nr;
  const a = windLatticeHash(ia, ja, seed), b = windLatticeHash(ib, ja, seed);
  const c = windLatticeHash(ia, jb, seed), d = windLatticeHash(ib, jb, seed);
  return (a + (b - a) * su) + ((c + (d - c) * su) - (a + (b - a) * su)) * sv;
}

function windFbm(q, r, octaves, seed) {
  let v = 0;
  for (let k = 0; k < octaves.length; k++) {
    const [nq, nr, w] = octaves[k];
    v += w * windNoise(q, r, nq, nr, seed + k * 101);
  }
  return v;
}

function windSmooth(e0, e1, x) {
  const t = Math.max(0, Math.min(1, (x - e0) / (e1 - e0)));
  return t * t * (3 - 2 * t);
}

// Gust patch value for a raw fbm sample -- ground-fx.js bakes the same curve.
function windGustCurve(n) { return windSmooth(0.52, 0.74, n); }

// ── This frame's wind ──────────────────────────────────────────────────────
const Wind = {
  x: 1, y: 0,       // heading, unit vector (the way it blows TO)
  s: 0.5,           // strength, ~0..1.5: weather x air speed x the gust here
  gust: 0,          // gust patch at the camera, 0..1
  calm: 1,          // 0..1, how gusty the hour is (gustAt multiplies by it)
  t: 0,             // seconds on the board's clock (or since WIND_EPOCH_MS, unsynced)
  _wf: null,        // weather factor, eased toward WIND_WEATHER[phase]
  _last: 0,
  _off: null,       // board clock minus Date.now(), ms; null until a tick says

  // The board's uptime (gs.wt, ms) as a tick or /state delivered it. Each
  // sample is late by the trip, so they are smoothed rather than trusted one
  // at a time; a big jump means the board rebooted, so take it whole.
  serverClock(wtMs) {
    if (typeof wtMs !== 'number' || !isFinite(wtMs)) return;
    const off = wtMs - Date.now();
    this._off = this._off === null || Math.abs(off - this._off) > 5000 ? off : this._off + (off - this._off) * 0.05;
  },

  // Once a frame, before anything reads it. camQ/camR: where the camera is
  // (map coords, unwrapped is fine) -- the gust there sets the strength the
  // particle systems feel. Underground there is no wind at all.
  update(nowMs, phase, depth, camQ, camR) {
    this.t = this._off !== null ? (nowMs + this._off) / 1000 : (nowMs - WIND_EPOCH_MS) / 1000;
    const v = windAirVelocity(this.t);
    const sp = Math.hypot(v.x, v.y) || 1e-6;
    this.x = v.x / sp;
    this.y = v.y / sp;
    const target = WIND_WEATHER[phase] ?? WIND_WEATHER[0];
    const dt = this._last ? Math.min(1000, nowMs - this._last) : 0;
    this._last = nowMs;
    // A front changes the wind over a few seconds, not between two frames.
    this._wf = this._wf === null ? target : this._wf + (target - this._wf) * Math.min(1, dt / 3500);
    // Gustiness of the hour: long calm spells and restless ones.
    const t = this.t;
    this.calm = Math.max(0.15, Math.min(1, 0.6 + 0.3 * Math.sin(t * 0.013 + 1.1) + 0.2 * Math.sin(t * 0.047 + 0.4)));
    this.gust = depth ? 0 : this.gustAt(1.5 * camQ, SQRT3 / 2 * camQ + SQRT3 * camR);
    this.s = depth ? 0 : Math.min(1.5, this._wf * (sp / WIND_V) * (0.75 + 0.6 * this.gust));
  },

  // Gust patch 0..1 at a world-hex point: patches of quicker air that run
  // downwind over the ground, the same field ground-fx.js draws on grass and
  // water.
  gustAt(wx, wy) {
    const d = windAirDrift(this.t);
    const a = windWorldToAxial(wx - d.x * WIND_GUST_RATE, wy - d.y * WIND_GUST_RATE);
    return windGustCurve(windFbm(a.q, a.r, WIND_GUST_OCTAVES, WIND_GUST_SEED)) * this.calm;
  },

  // Air drift (times `rate`) as an axial offset folded onto the torus, for
  // pattern transforms: the raw drift is millions of hexes, which a float32
  // pattern matrix would round to whole tiles.
  driftAxial(rate) {
    const d = windAirDrift(this.t);
    const a = windWorldToAxial(d.x * rate, d.y * rate);
    return { q: ((a.q % MAP_COLS) + MAP_COLS) % MAP_COLS, r: ((a.r % MAP_ROWS) + MAP_ROWS) % MAP_ROWS };
  },
};
