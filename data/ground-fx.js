// ── Ground FX ──────────────────────────────────────────────────────────────
// The ground between the events: what makes a painted tile read as a place
// with weather on it. Atmosphere only -- nothing here is information and
// nothing is simulated by the server -- and surface only. renderer.js feeds
// it per hex during the terrain pass (collect / noteRemembered) and calls
// three draws. observer.js drives the same three on the TV, onto a
// transparent canvas laid over its still hex disk (see opts.source).
//
//   drawGround  over the terrain, under the grid
//     water   Marsh, Flooded District, River Channel: the painted water sways
//             (row displacement -- the old wavy-water trick), gust patches
//             darken it as they run over it (cat's paws) and the sun glitters
//             on it, thicker where the gusts roughen it. Masked to the water
//             in the art, not to the hex: _analyze() keys it off the tile.
//     heat    Ash Dunes, Glass Fields, Nuke Crater: shimmer rising off the
//             ground around NOON BURN.
//     grass   Open Scrub: the gust patches as a pale sheen -- grass laid
//             flat by the wind.
//     glints  Glass Fields: the shards flash in the sun, most at LOW SUN; the
//             ones that flash shift as you walk past.
//   drawDevils  over the pawns: faint, occasional dust devils over scrub and
//             dunes on clear, sunny, breezy days, riding the wind.
//   drawClouds  over the pawns, under the night tint: cloud shadows drifting
//             with the air in fair weather.
//
// Wind and gusts come from wind.js, so the shadows, the sheen, the paws, the
// devils and every particle system agree on where the air is going. Light
// comes from the survivor's own clock (renderer.js sunState()): none of the
// sun's work happens at night. Plain globals, no ES module syntax.

const GFX_WATER = new Set([3, 5, 11]);              // Marsh, Flooded District, River Channel
const GFX_HEAT  = { 1: 0.85, 6: 1, 10: 0.9 };   // terrain -> shimmer weight
// Dust devils kick up the ground they cross -- scrub tan, dune ash -- a few
// shades darker than the ground, or the haze melts into the pale art. Each
// clump of it is [in its own shade, body, in the sun].
const GFX_DEVIL_PAL = { 0: ['112,94,66', '170,146,106', '236,224,194'], 1: ['92,89,84', '148,144,136', '236,234,228'] };
const GFX_DEVIL_INK = { 0: 'rgb(96,80,56)', 1: 'rgb(78,76,72)' };    // its stipple of sand
const GFX_DEVIL_SHADE = ['28,22,14', '28,22,14', '28,22,14'];       // its shadow
// How much sun gets through, by weatherPhase (clear, rain, storm, chem,
// strangle fog, mist): the glitter and the glints.
const GFX_SUN_WEATHER   = [1, 0.4, 0.12, 0.2, 0, 0.1];
// Heat shimmer wants clear, still-ish air and a high sun.
const GFX_HEAT_WEATHER  = [1, 0.15, 0, 0, 0, 0];
// Broken cloud: fair weather has the most distinct shadows; under a front the
// whole sky is cloud and there is nothing left to cast one.
const GFX_CLOUD_WEATHER = [1, 0.55, 0.25, 0.3, 0, 0];
const GFX_CLOUD_OCTAVES = [[12, 9, 0.5], [25, 19, 0.32], [75, 57, 0.18]];
const GFX_CLOUD_SEED    = 4242;
const GFX_CLOUD_RGB     = [6, 8, 16];
const GFX_CLOUD_ALPHA   = 0.24;
const GFX_TEX_K         = 4;      // texels per hex in the baked torus textures
const GFX_SAMPLE        = 80;     // px, the raster a tile is analysed at
const GFX_SHIMMER_EVERY = 3;      // frames between rebuilds of a still shimmer layer
// Dust devils: per known scrub/dune hex, the chance one spins up in a slot,
// at full noon heat. ~60 such hexes on screen -> one every minute or so.
const GFX_DEVIL_SLOT  = 40;       // s
const GFX_DEVIL_P     = 0.012;
const GFX_DEVIL_ALPHA  = 0.95;    // clump density; the column stays see-through
const GFX_DEVIL_GRIT   = 0.8;     // the stipple of sand climbing it
const GFX_DEVIL_TWIST  = 5;       // radians its dust bands wind through, foot to top
const GFX_SPARKLE_ALPHA = 0.35;   // peak opacity of a water glitter or a glass glint (the user's call)
// Water glitter and glass glints are an occasional flash, not a steady fizz
// (also the user's call): every point's cycle runs 30% slower, and only this
// share of its cycles actually flash -- which ones is a hash of (point,
// cycle), so a point flashes irregularly instead of on a metronome.
const GFX_SPARKLE_SLOW = 1 / 0.7; // period multiplier
const GFX_SPARKLE_FIRE = 0.35;    // chance a given cycle flashes

// Six corners of a flat-top hex onto the current path (no beginPath, so many
// hexes can share one path / one clip).
function gfxHexTo(g, cx, cy, size) {
  for (let i = 0; i < 6; i++) {
    const a = Math.PI / 3 * i;
    const x = cx + size * Math.cos(a), y = cy + size * Math.sin(a);
    if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
  }
  g.closePath();
}

class GroundFx {
  // opts.source: a canvas holding the ground, for a page that draws these
  // effects on a transparent canvas laid OVER its own (the observer TV's
  // static hex disk) instead of onto the board itself. The shimmer then reads
  // the ground from it rather than from the canvas being drawn on.
  // opts.shimmerEvery: frames between shimmer rebuilds (GFX_SHIMMER_EVERY).
  constructor(opts = {}) {
    this.source = opts.source || null;
    this.shimmerEvery = opts.shimmerEvery || GFX_SHIMMER_EVERY;
    this.water = []; this.hot = []; this.glass = []; this.scrub = []; this.devilHexes = [];
    this._info   = new Map();    // tile key -> { mask, wpts, gpts } (_analyze)
    this._budget = 0;            // tile analyses still allowed this frame
    this._tex    = null;         // baked torus textures, false if unsupported
    this._pats   = new WeakMap();// context -> { name: CanvasPattern }
    this._puffs  = new Map();    // 'rgb|shape' -> dust clump sprite (_dust)
    this._layers = {};           // cached shimmer layers (_shimmer)
    this._frame  = 0;
  }

  beginFrame() {
    this.water.length = 0; this.hot.length = 0; this.glass.length = 0;
    this.scrub.length = 0; this.devilHexes.length = 0;
    this._budget = 2;            // a hitch-free trickle when a lot of new art scrolls in
    this._frame++;
  }

  // A hex in sight, after its art and overlays. ghost: the sight-fade veil
  // alpha it got (0 = none) -- effects at the edge of sight fade with it.
  collect(cx, cy, cell, q, r, tile, ghost) {
    const t = cell.terrain, w = 1 - (ghost || 0);
    if (w <= 0.05) return;
    if (tile && GFX_WATER.has(t)) this.water.push({ cx, cy, q, r, tile, w });
    const heat = GFX_HEAT[t];
    if (heat) this.hot.push({ cx, cy, q, r, k: heat * w });
    if (tile && t === 6) this.glass.push({ cx, cy, q, r, tile, w });
    if (t === 0) this.scrub.push({ cx, cy, w });
    if (GFX_DEVIL_PAL[t]) this.devilHexes.push({ cx, cy, q, r, t });
  }

  // Remembered ground: nothing changes on it that you can see, but a dust
  // devil is weather, and you can watch one cross the flats you walked.
  noteRemembered(cx, cy, terrain, q, r) {
    if (GFX_DEVIL_PAL[terrain]) this.devilHexes.push({ cx, cy, q, r, t: terrain });
  }

  // ── Layers ───────────────────────────────────────────────────────────────
  drawGround(ctx, cam, now, sz, w, h, sun, phase) {
    this._waterPass(ctx, cam, now, sz, w, sun, phase);
    this._heatPass(ctx, cam, now, sz, w, sun, phase);
    this._grassPass(ctx, cam, sz, sun);
    this._glintPass(ctx, cam, now, sz, sun, phase);
  }

  drawClouds(ctx, cam, sz, w, h, sun, phase) {
    const a = GFX_CLOUD_ALPHA * sun.day * (GFX_CLOUD_WEATHER[phase] ?? 0);
    if (a < 0.01 || !this._textures()) return;
    const p = this._pattern(ctx, 'cloud');
    p.setTransform(this._patMatrix(cam, sz, 1));
    ctx.save();
    ctx.globalAlpha = a;
    ctx.fillStyle = p;
    ctx.fillRect(0, 0, w, h);
    ctx.restore();
  }

  // A dust devil is a hash of (hex, time slot): every client that knows the
  // ground sees the same one at the same time, riding the same air.
  drawDevils(ctx, cam, now, sz, w, h, sun, phase) {
    if (phase !== 0 || sun.day < 0.3 || !this.devilHexes.length) return;
    if (Wind.s < 0.12 || Wind.s > 1.25) return;   // dead calm, or blowing too hard to spin
    const heat = sun.day * (0.35 + 0.65 * sun.noon);
    const t = Wind.t;
    const slot = Math.floor(t / GFX_DEVIL_SLOT);
    const margin = sz * 3;
    ctx.save();
    for (const hx of this.devilHexes) {
      if (hx.cx < -margin || hx.cx > w + margin || hx.cy < -margin || hx.cy > h + margin) continue;
      for (let s = slot - 1; s <= slot; s++) {     // one born late last slot may still be up
        if (windLatticeHash(hx.q * 131 + s, hx.r * 71 - s, 555) > GFX_DEVIL_P * heat) continue;
        const h1 = windLatticeHash(hx.q, hx.r * 3 + s, 556);
        const h2 = windLatticeHash(hx.q + s, hx.r, 557);
        const t0 = (s + h1 * 0.6) * GFX_DEVIL_SLOT;
        const life = 14 + h2 * 12;
        const age = t - t0;
        if (age >= 0 && age <= life) this._devil(ctx, hx, t0, age, life, sz, h1, h2);
      }
    }
    ctx.restore();
  }

  // ── Water ────────────────────────────────────────────────────────────────
  _waterPass(ctx, cam, now, sz, w, sun, phase) {
    if (!this.water.length) return;
    const items = [];
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const it of this.water) {
      const a = this._analysis(it.tile);
      if (!a?.mask) continue;
      const b = this._box(it.tile, it.cx, it.cy, sz);
      items.push({ it, a, b });
      x0 = Math.min(x0, b.x); y0 = Math.min(y0, b.y);
      x1 = Math.max(x1, b.x + b.w); y1 = Math.max(y1, b.y + b.h);
    }
    if (!items.length) return;
    const R = this._devRect(ctx, x0, y0, x1, y1, w);
    if (!R) return;

    // 1. Sway: two slow swells crossing, more of it when the wind is up.
    // 2. Cat's paws: the gust patches darkening the water they roughen.
    const t = now / 1000;
    const amp = Math.max(0.5, sz / 56) * (0.5 + 0.5 * Math.min(1, Wind.s));
    const pa = 0.3 * Math.min(1, Wind.s * 1.2);
    this._shimmer('water', 0, ctx, cam, R, items.length, (g) => {
      this._rows(g, R, cam, sz, 3,
        (yw) => amp * (0.6 * Math.sin(yw * 26 + t * 1.7) + 0.4 * Math.sin(yw * 47 - t * 2.6 + 1.3)));
      if (pa > 0.02 && this._textures()) {
        g.setTransform(R.dpr, 0, 0, R.dpr, -R.X0, -R.Y0);
        g.globalCompositeOperation = 'source-atop';
        const p = this._pattern(g, 'dark');
        p.setTransform(this._patMatrix(cam, sz, WIND_GUST_RATE));
        g.globalAlpha = pa;
        g.fillStyle = p;
        g.fillRect(x0, y0, x1 - x0, y1 - y0);
      }
    }, (mg) => {
      for (const { it, a, b } of items) {
        mg.globalAlpha = it.w;
        mg.drawImage(a.mask, b.x, b.y, b.w, b.h);
      }
    });

    // 3. Glitter: points of sun on the water, more where it is ruffled.
    const base = sun.day * (GFX_SUN_WEATHER[phase] ?? 0) * (0.45 + 0.35 * sun.low + 0.2 * sun.noon);
    if (base < 0.03) return;
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    for (const { it, a, b } of items) {
      const k = base * it.w * (1 - this._storm(it.q, it.r, now, phase));
      if (k < 0.03) continue;
      const gust = Wind.gustAt(1.5 * it.q, SQRT3 / 2 * it.q + SQRT3 * it.r);
      const dens = 0.2 + 0.8 * Math.min(1, gust + 0.3 * Wind.s);
      for (let i = 0; i < a.wpts.length; i++) {
        const hs = windLatticeHash(it.q * 64 + i, it.r, 31);
        if (hs > dens) continue;
        const per = (0.7 + windLatticeHash(i, it.q * 5 + it.r, 32) * 1.3) * GFX_SPARKLE_SLOW;
        const cyc = t / per + windLatticeHash(it.r * 9 + i, it.q, 33);
        const ph = cyc % 1;
        if (ph > 0.14) continue;
        if (windLatticeHash(it.q * 64 + i, it.r * 977 + Math.floor(cyc), 34) > GFX_SPARKLE_FIRE) continue;
        const e = Math.sin(Math.PI * ph / 0.14);
        const p = a.wpts[i];
        const drift = ph * sz * 0.25;    // carried a hair downwind while it lives
        this._sparkle(ctx, b.x + p[0] * b.w + Wind.x * drift, b.y + p[1] * b.h + Wind.y * drift,
                      sz * (0.035 + 0.03 * hs) * e, k * e, 0.35);
      }
    }
    ctx.restore();
  }

  // ── Heat ─────────────────────────────────────────────────────────────────
  _heatPass(ctx, cam, now, sz, w, sun, phase) {
    const g = sun.noon * sun.day * (GFX_HEAT_WEATHER[phase] ?? 0);
    if (g < 0.03 || !this.hot.length) return;
    const items = [];
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const it of this.hot) {
      const k = it.k * g * (1 - this._storm(it.q, it.r, now, phase));
      if (k < 0.03) continue;
      items.push({ x: it.cx, y: it.cy, k });
      x0 = Math.min(x0, it.cx - sz); y0 = Math.min(y0, it.cy - sz);
      x1 = Math.max(x1, it.cx + sz); y1 = Math.max(y1, it.cy + sz);
    }
    if (!items.length) return;
    const R = this._devRect(ctx, x0, y0, x1, y1, w);
    if (!R) return;
    // Fine, quick ripples climbing up the screen: hot air rising.
    const t = now / 1000;
    const amp = Math.max(0.45, 0.75 * sz / 56);
    this._shimmer('heat', 1, ctx, cam, R, items.length, (sg) => {
      this._rows(sg, R, cam, sz, 2,
        (yw) => amp * (0.65 * Math.sin(yw * 44 + t * 7) + 0.35 * Math.sin(yw * 71 + t * 10.3 + 2.1)));
    }, (mg) => {
      mg.fillStyle = '#fff';
      for (const it of items) {
        mg.globalAlpha = Math.min(1, it.k);
        mg.beginPath();
        gfxHexTo(mg, it.x, it.y, sz);
        mg.fill();
      }
    });
  }

  // ── Grass ────────────────────────────────────────────────────────────────
  _grassPass(ctx, cam, sz, sun) {
    if (!this.scrub.length) return;
    const a = 0.2 * sun.day * Math.min(1, Wind.s * 1.3);
    if (a < 0.01 || !this._textures()) return;
    const p = this._pattern(ctx, 'light');
    p.setTransform(this._patMatrix(cam, sz, WIND_GUST_RATE));
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    ctx.save();
    ctx.beginPath();
    for (const s of this.scrub) {
      gfxHexTo(ctx, s.cx, s.cy, sz);
      x0 = Math.min(x0, s.cx - sz); y0 = Math.min(y0, s.cy - sz);
      x1 = Math.max(x1, s.cx + sz); y1 = Math.max(y1, s.cy + sz);
    }
    ctx.clip();
    // Over a separate overlay there is no ground under the pen to blend
    // with, so the sheen is laid on plainly, and lighter.
    ctx.globalCompositeOperation = this.source ? 'source-over' : 'soft-light';
    ctx.globalAlpha = this.source ? a * 0.45 : a;
    ctx.fillStyle = p;
    ctx.fillRect(x0, y0, x1 - x0, y1 - y0);
    ctx.restore();
  }

  // ── Glass ────────────────────────────────────────────────────────────────
  _glintPass(ctx, cam, now, sz, sun, phase) {
    if (!this.glass.length) return;
    const base = sun.day * (GFX_SUN_WEATHER[phase] ?? 0) * (0.3 + 0.7 * Math.max(sun.low, 0.55 * sun.noon));
    if (base < 0.03) return;
    const t = now / 1000;
    // Which shard catches the sun depends on where you stand: walk past a
    // glass field and the flashes move across it.
    const view = (cam.meRp.q * 0.9 + cam.meRp.r * 1.3);
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    for (const it of this.glass) {
      const a = this._analysis(it.tile);
      if (!a) continue;
      const k = base * it.w * (1 - this._storm(it.q, it.r, now, phase));
      if (k < 0.03) continue;
      const b = this._box(it.tile, it.cx, it.cy, sz);
      for (let i = 0; i < a.gpts.length; i++) {
        const hs  = windLatticeHash(it.q * 16 + i, it.r, 97);
        const per = (2.6 + hs * 3.4) * GFX_SPARKLE_SLOW;
        const cyc = t / per + windLatticeHash(i, it.q * 7 + it.r, 13) + view * (0.12 + 0.2 * hs);
        let ph = cyc % 1;
        if (ph < 0) ph += 1;
        if (ph > 0.07) continue;
        if (windLatticeHash(it.q * 16 + i, it.r * 977 + Math.floor(cyc), 98) > GFX_SPARKLE_FIRE) continue;
        const e = Math.sin(Math.PI * ph / 0.07);
        const p = a.gpts[i];
        this._sparkle(ctx, b.x + p[0] * b.w, b.y + p[1] * b.h, sz * (0.09 + 0.07 * hs) * e, k * e, 0.85);
      }
    }
    ctx.restore();
  }

  // A comic sparkle: two tapered lozenges and a hot core. tall: the vertical
  // ray's length relative to the horizontal one (water glitter is flatter).
  // a: 0..1, scaled to GFX_SPARKLE_ALPHA. Rays and core are one path, filled
  // once, so the middle where they cross isn't brighter than the cap.
  _sparkle(ctx, x, y, L, a, tall) {
    if (a < 0.02 || L < 0.6) return;
    const wd = Math.max(0.5, L * 0.12);
    ctx.globalAlpha = GFX_SPARKLE_ALPHA * Math.min(1, a);
    ctx.fillStyle = 'rgb(255,244,220)';
    ctx.beginPath();
    ctx.moveTo(x - L, y); ctx.lineTo(x, y - wd); ctx.lineTo(x + L, y); ctx.lineTo(x, y + wd); ctx.closePath();
    const V = L * tall;
    ctx.moveTo(x, y - V); ctx.lineTo(x + wd, y); ctx.lineTo(x, y + V); ctx.lineTo(x - wd, y); ctx.closePath();
    const cr = Math.max(0.6, L * 0.2);
    ctx.moveTo(x + cr, y);
    ctx.arc(x, y, cr, 0, Math.PI * 2);
    ctx.fill();
  }

  // ── Dust devil ───────────────────────────────────────────────────────────
  // A column of dust, not a drawing of one: no outline anywhere. Ragged,
  // sun-modelled clumps of grit (_dust) are whirled up round a leaning axis
  // over a faint body of bigger ones, packed at the foot and thinning as they
  // climb into a haze that shears off downwind. Most ride three bands wound
  // round the funnel, which is what makes it read as a whirl. A ring of grit
  // is thrown out along the ground, a stipple of sand climbs the bands (the
  // dotted lines the tile art shades its dunes with), and its shadow falls on
  // the ground. It rides the air (windAirDrift between its birth and now) with
  // a lazy meander across it.
  _devil(ctx, hx, t0, age, life, sz, h1, h2) {
    const d0 = windAirDrift(t0), d1 = windAirDrift(t0 + age);
    const ride = 0.8;
    const mx = Math.sin(age * 0.83 + h2 * 9) * 0.22 + Math.sin(age * 2.1 + h1 * 5) * 0.07;
    const x = hx.cx + ((d1.x - d0.x) * ride + (h1 - 0.5) * 0.8 - Wind.y * mx) * sz;
    const y = hx.cy + ((d1.y - d0.y) * ride + (h2 - 0.5) * 0.6 + Wind.x * mx) * sz;
    const env = Math.max(0, Math.min(1, age / 3, (life - age) / 3.5));
    if (env <= 0.01) return;
    const H = sz * (0.85 + 0.5 * h2) * (0.55 + 0.45 * env);   // spins up tall, sags as it dies
    const lean = (Wind.x * 0.24 + Math.sin(age * 1.3 + h1 * 4) * 0.07) * sz;
    const spin = age * (5 + h1 * 2.5);
    const pal = GFX_DEVIL_PAL[hx.t];
    const fr = (v) => v - Math.floor(v);
    const seed = (h1 * 65536) | 0;
    const rnd = (i, k) => windLatticeHash(i, k, seed);       // independent per particle and per use
    const axis = (f) => x + lean * f * f + Math.sin(age * 1.7 + f * 3 + h1 * 6) * sz * 0.05 * f;
    const half = (f) => sz * (0.045 + 0.34 * Math.pow(f, 1.2)) * (1 + 0.2 * Math.sin(age * 2.3 + f * 7 + h2 * 5));
    const shear = (f) => f * f * sz * 0.15;      // the top bleeds away downwind
    const band = (f, k, jit) => spin * (1.2 - 0.55 * f) + (k % 3) * 2.094 + f * GFX_DEVIL_TWIST + jit;
    const blot = (img, px, py, r, sx, a) => {
      if (a < 0.01) return;
      ctx.globalAlpha = a;
      ctx.drawImage(img, px - r * sx, py - r, r * 2 * sx, r * 2);
    };

    // Its shadow, thrown down-right off the sun the tiles are lit from: from
    // above, that dark smear is most of what gives a dust devil away.
    for (let i = 0; i < 5; i++) {
      const f = i / 4;
      blot(this._dust(GFX_DEVIL_SHADE, i & 3), x + (H * 0.55 + lean * 0.4) * f, y + H * 0.22 * f,
           sz * (0.07 + 0.09 * f), 1.8, 0.3 * env * (1 - 0.5 * f));
    }

    // Grit thrown out along the ground round the foot, spreading and settling.
    for (let j = 0; j < 10; j++) {
      const u = fr(rnd(j, 1) + age * (0.45 + 0.1 * rnd(j, 2)));
      const ang = spin * 0.55 + rnd(j, 3) * 6.28 + u * 1.4;
      const rad = sz * (0.06 + 0.32 * u);
      blot(this._dust(pal, j & 3), x + Math.cos(ang) * rad, y + Math.sin(ang) * rad * 0.34 - u * sz * 0.04,
           sz * (0.07 + 0.09 * u), 1.7, 0.55 * GFX_DEVIL_ALPHA * env * windSmooth(0, 0.12, u) * Math.pow(1 - u, 1.6));
    }

    // Its body: a few big, faint clumps stacked up the axis, so the grit
    // in front reads as one column of dust rather than a scatter.
    for (let k = 0; k < 7; k++) {
      const f = (k + 0.5) / 7;
      blot(this._dust(pal, (k + seed) & 3), axis(f) + Wind.x * shear(f), y - H * f + Wind.y * shear(f) * 0.5,
           half(f) * 1.1 + sz * 0.04, 0.85, 0.5 * GFX_DEVIL_ALPHA * env * (1 - f * f));
    }

    // The column: each clump climbs on its own clock and is recycled at the
    // foot, faded out at both ends so nothing pops; one in four fills the
    // core instead of riding a band. The far side is drawn first, fainter:
    // it shows through the dust in front.
    const clumps = [];
    for (let i = 0; i < 52; i++) {
      const f = fr(rnd(i, 11) + age * (0.26 + 0.14 * rnd(i, 12)));
      const core = i % 4 === 3;
      const ang = core ? spin + rnd(i, 13) * 6.28 : band(f, i, (rnd(i, 13) - 0.5) * 0.8);
      const orbit = half(f) * (core ? 0.35 * rnd(i, 14) : 0.75 + 0.3 * rnd(i, 14));
      const front = Math.sin(ang);
      clumps.push({
        x: axis(f) + Math.cos(ang) * orbit + Wind.x * shear(f),
        y: y - H * f + front * orbit * 0.3 + Wind.y * shear(f) * 0.5,
        r: sz * (0.05 + 0.18 * f) * (0.75 + 0.5 * rnd(i, 15)),
        a: GFX_DEVIL_ALPHA * env * windSmooth(0, 0.07, f) * (1 - Math.pow(f, 1.8)) * (0.7 + 0.3 * rnd(i, 16)) *
           (0.6 + 0.4 * (front + 1) / 2),
        front, v: i & 3,
      });
    }
    clumps.sort((a, b) => a.front - b.front);
    for (const c of clumps) blot(this._dust(pal, c.v), c.x, c.y, c.r, 1, c.a);

    // Stipple: grains of sand climbing the bands, on the near side only.
    ctx.fillStyle = GFX_DEVIL_INK[hx.t];
    ctx.beginPath();
    let lastA = -1;
    for (let k = 0; k < 66; k++) {
      const f = fr(rnd(k, 21) + age * (0.3 + 0.1 * rnd(k, 22)));
      const ang = band(f, k, (rnd(k, 23) - 0.5) * 0.35);
      const front = Math.sin(ang);
      if (front <= 0) continue;
      // Alpha in steps of 0.1, so neighbouring grains share one fill.
      const a = Math.round(10 * GFX_DEVIL_GRIT * env * windSmooth(0, 0.06, f) * Math.pow(1 - f, 1.1) * front ** 0.7) / 10;
      if (a < 0.05) continue;
      if (a !== lastA) {
        if (lastA > 0) ctx.fill();
        ctx.beginPath();
        ctx.globalAlpha = lastA = a;
      }
      const rr = half(f) * (0.85 + 0.3 * rnd(k, 24));
      const px = axis(f) + Math.cos(ang) * rr + Wind.x * shear(f) * 0.8;
      const py = y - H * f + front * rr * 0.3 + Wind.y * shear(f) * 0.4;
      const dr = Math.max(0.6, sz * 0.011 * (0.6 + 0.9 * rnd(k, 25)));
      ctx.moveTo(px + dr, py);
      ctx.arc(px, py, dr, 0, Math.PI * 2);
    }
    if (lastA > 0) ctx.fill();
  }

  // A clump of dust, baked once per palette and shape (v 0..3): a lumpy ball
  // lit from the upper left like the tile art -- pal is [shade, body, sun] --
  // its edge torn by value noise and its body clotted and grainy, so a heap of
  // them reads as billowing dust rather than as discs.
  _dust(pal, v) {
    const key = pal.join('|') + '|' + v;
    let c = this._puffs.get(key);
    if (c) return c;
    const N = 48, seed = 900 + v * 37;
    c = document.createElement('canvas');
    c.width = c.height = N;
    const g = c.getContext('2d');
    const img = g.createImageData(N, N), px = img.data;
    const [D, M, L] = pal.map((p) => p.split(',').map(Number));
    const noise = (x, y, cells, s) => {
      const u = x / N * cells, w = y / N * cells;
      const i = Math.floor(u), j = Math.floor(w);
      const fu = windSmooth(0, 1, u - i), fw = windSmooth(0, 1, w - j);
      const a = windLatticeHash(i, j, s), b = windLatticeHash(i + 1, j, s);
      const cc = windLatticeHash(i, j + 1, s), d = windLatticeHash(i + 1, j + 1, s);
      return a + (b - a) * fu + (cc - a + (a - b - cc + d) * fu) * fw;
    };
    for (let y = 0; y < N; y++) {
      for (let x = 0; x < N; x++) {
        const n1 = noise(x, y, 3, seed), n2 = noise(x, y, 7, seed + 1), n3 = noise(x, y, 16, seed + 2);
        const n = 0.55 * n1 + 0.3 * n2 + 0.15 * n3;
        const dx = (x + 0.5) / N * 2 - 1, dy = (y + 0.5) / N * 2 - 1;
        const d = Math.hypot(dx, dy) + (n - 0.5) * 0.6;
        let a = 1 - windSmooth(0.05, 0.95, d);
        a *= 0.6 + 0.65 * (0.6 * n2 + 0.4 * n3);
        // Light: a ball's normal, made lumpy by the noise, against a sun high
        // in the upper left. lt -1 (shade) .. 1 (full sun).
        const nz = Math.sqrt(Math.max(0, 1 - dx * dx - dy * dy));
        const lt = Math.max(-1, Math.min(1, (-0.5 * dx - 0.6 * dy + 0.62 * nz) * 1.2 - 0.45 + (n2 - 0.5) * 0.9));
        const k = lt < 0 ? -lt : lt, A = lt < 0 ? D : L;
        const sp = windLatticeHash(x, y, seed + 3);
        const grain = (1 + (n - 0.5) * 0.25 + (sp - 0.5) * 0.3) * (sp > 0.93 ? 0.7 : 1);   // grit, and a speck of darker grit
        const o = (y * N + x) * 4;
        for (let ch = 0; ch < 3; ch++) px[o + ch] = Math.min(255, (M[ch] + (A[ch] - M[ch]) * k * 0.85) * grain);
        px[o + 3] = Math.max(0, Math.min(255, a * 255));
      }
    }
    g.putImageData(img, 0, 0);
    this._puffs.set(key, c);
    return c;
  }

  // ── Plumbing ─────────────────────────────────────────────────────────────
  _storm(q, r, now, phase) {
    return phase > 0 && typeof stormIntensityAt === 'function' ? stormIntensityAt(q, r, phase, now) : 0;
  }

  // Tiles come in three shapes: the game's atlas cell { page, sx, sy } (cell
  // and anchor from engine.js tileAtlas), the observer's { img, sx, sy, m }
  // (m = the tiles.json manifest), and a per-file tile { img }.
  // Screen box a tile is drawn into (renderer drawTerrainTile, observer renderDisk).
  _box(tile, cx, cy, sz) {
    if (tile.m) {
      const m = tile.m, k = sz / m.radius;
      return { x: cx - m.anchor[0] * k, y: cy - m.anchor[1] * k, w: m.cell[0] * k, h: m.cell[1] * k };
    }
    return tile.img ? { x: cx - sz, y: cy - sz, w: sz * 2, h: sz * 2 } : tileBox(cx, cy, sz);
  }

  _analysis(tile) {
    const src = tile.page || tile.img;
    if (!src) return null;
    const key = `${src.src}|${tile.sx | 0}|${tile.sy | 0}`;
    let a = this._info.get(key);
    if (a || this._budget <= 0) return a || null;
    this._budget--;
    a = this._analyze(tile);
    this._info.set(key, a);
    return a;
  }

  // Read a tile's art once: where its water is (an alpha mask, plus points on
  // it for the glitter) and where its bright crystal edges are (glint points).
  // Water in this art is the cool, green-leaning paint -- marsh teal, flood
  // olive, river slate -- with ink, mud, reeds, rust and concrete all warmer
  // or greyer; foam counts at half.
  _analyze(tile) {
    const N = GFX_SAMPLE;
    const empty = { mask: null, wpts: [], gpts: [] };
    const c = document.createElement('canvas');
    c.width = c.height = N;
    const g = c.getContext('2d', { willReadFrequently: true });
    if (tile.m) g.drawImage(tile.img, tile.sx, tile.sy, tile.m.cell[0], tile.m.cell[1], 0, 0, N, N);
    else if (tile.img) g.drawImage(tile.img, 0, 0, N, N);
    else g.drawImage(tile.page, tile.sx, tile.sy, tileAtlas.cell[0], tileAtlas.cell[1], 0, 0, N, N);
    let px;
    try { px = g.getImageData(0, 0, N, N).data; } catch { return empty; }

    const m = document.createElement('canvas');
    m.width = m.height = N;
    const mg = m.getContext('2d');
    const md = mg.createImageData(N, N);
    const lum = new Float32Array(N * N);
    const wpts = [];
    let water = 0;
    for (let i = 0; i < N * N; i++) {
      const r = px[i * 4], gg = px[i * 4 + 1], b = px[i * 4 + 2], al = px[i * 4 + 3];
      const l = 0.3 * r + 0.59 * gg + 0.11 * b;
      lum[i] = l;
      let s = 0;
      if (l >= 30 && al > 0) {
        const gr = gg - r, yb = (r + gg) / 2 - b;
        s = Math.max(0, Math.min(1, (gr - 2) / 8)) * Math.max(0, Math.min(1, (16 - yb) / 10));
        if (l > 190) s *= 0.5;
        s *= al / 255;
      }
      md.data[i * 4] = md.data[i * 4 + 1] = md.data[i * 4 + 2] = 255;
      md.data[i * 4 + 3] = (s * 255) | 0;
      water += s;
      if (s > 0.75 && wpts.length < 48 && windLatticeHash(i, 7, 41) < 0.07) wpts.push([(i % N + 0.5) / N, ((i / N | 0) + 0.5) / N]);
    }
    mg.putImageData(md, 0, 0);

    // Glints: pale pixels clearly brighter than their surroundings (a 7x7
    // box mean off an integral image), well inside the hex, spaced apart.
    const S = new Float64Array((N + 1) * (N + 1));
    for (let y = 0; y < N; y++) {
      let row = 0;
      for (let x = 0; x < N; x++) {
        row += lum[y * N + x];
        S[(y + 1) * (N + 1) + x + 1] = S[y * (N + 1) + x + 1] + row;
      }
    }
    const R = 3, cand = [];
    for (let y = R; y < N - R; y++) {
      for (let x = R; x < N - R; x++) {
        const i = y * N + x;
        const l = lum[i];
        if (l <= 200) continue;
        if (px[((y - R) * N + x) * 4 + 3] < 250 || px[((y + R) * N + x) * 4 + 3] < 250 ||
            px[(y * N + x - R) * 4 + 3] < 250 || px[(y * N + x + R) * 4 + 3] < 250) continue;
        const sum = S[(y + R + 1) * (N + 1) + x + R + 1] - S[(y - R) * (N + 1) + x + R + 1]
                  - S[(y + R + 1) * (N + 1) + x - R] + S[(y - R) * (N + 1) + x - R];
        const mean = sum / ((2 * R + 1) * (2 * R + 1));
        if (l - mean > 18) cand.push([l - mean + l * 0.1, x, y]);
      }
    }
    cand.sort((a, b) => b[0] - a[0]);
    const gpts = [], minD = (N * 0.085) ** 2;
    for (const [, x, y] of cand) {
      if (gpts.every(([gx, gy]) => (gx * N - x) ** 2 + (gy * N - y) ** 2 > minD)) gpts.push([(x + 0.5) / N, (y + 0.5) / N]);
      if (gpts.length >= 12) break;
    }
    return { mask: water > N * N * 0.01 ? m : null, wpts, gpts };
  }

  // Device-pixel rectangle on ctx's canvas for a css-pixel box, clamped.
  _devRect(ctx, x0, y0, x1, y1, w) {
    const cv = ctx.canvas, dpr = cv.width / w;
    const X0 = Math.max(0, Math.floor(x0 * dpr)), Y0 = Math.max(0, Math.floor(y0 * dpr));
    const X1 = Math.min(cv.width, Math.ceil(x1 * dpr)), Y1 = Math.min(cv.height, Math.ceil(y1 * dpr));
    if (X1 - X0 < 2 || Y1 - Y0 < 2) return null;
    return { X0, Y0, W: X1 - X0, H: Y1 - Y0, dpr };
  }

  // A cleared offscreen context at least W x H, reused frame to frame.
  _scratch(name, W, H) {
    let c = this[name];
    if (!c) c = this[name] = document.createElement('canvas');
    if (c.width < W || c.height < H) { c.width = Math.max(c.width, W); c.height = Math.max(c.height, H); }
    const g = c.getContext('2d');
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.globalAlpha = 1;
    g.globalCompositeOperation = 'source-over';
    g.clearRect(0, 0, W, H);
    return g;
  }

  // One cached shimmer layer: the board under region R redrawn displaced by
  // paint(g) and cut to the mask that mask(g) draws (css coordinates), laid
  // over the board every frame but rebuilt only every `shimmerEvery` frames
  // -- at once when the camera, the zoom or the region moves. Those are
  // full-region copies, the expensive part on a phone; the ripples are slow
  // enough that 20 rebuilds a second can't be told from 60. `slot` staggers
  // the passes so they don't rebuild on the same frame.
  _shimmer(name, slot, ctx, cam, R, count, paint, mask) {
    const key = `${R.X0},${R.Y0},${R.W},${R.H},${cam.ox.toFixed(2)},${cam.oy.toFixed(2)},${count}`;
    let L = this._layers[name];
    if (!L) L = this._layers[name] = { key: '', c: document.createElement('canvas') };
    if (L.key !== key || (this._frame + slot) % this.shimmerEvery === 0) {
      const c = L.c;
      if (c.width < R.W || c.height < R.H) { c.width = Math.max(c.width, R.W); c.height = Math.max(c.height, R.H); }
      const g = c.getContext('2d');
      g.setTransform(1, 0, 0, 1, 0, 0);
      g.globalAlpha = 1;
      g.globalCompositeOperation = 'source-over';
      g.clearRect(0, 0, R.W, R.H);
      // The ground to displace: a still source canvas is read in place; the
      // live board is copied first, since it is the canvas being drawn on.
      if (this.source) {
        this._rowSrc = this.source; this._rowX = R.X0; this._rowY = R.Y0;
      } else {
        this._scratch('_snapC', R.W, R.H).drawImage(ctx.canvas, R.X0, R.Y0, R.W, R.H, 0, 0, R.W, R.H);
        this._rowSrc = this._snapC; this._rowX = 0; this._rowY = 0;
      }
      paint(g);
      const mg = this._scratch('_maskC', R.W, R.H);
      mg.setTransform(R.dpr, 0, 0, R.dpr, -R.X0, -R.Y0);
      mask(mg);
      g.setTransform(1, 0, 0, 1, 0, 0);
      g.globalAlpha = 1;
      g.globalCompositeOperation = 'destination-in';
      g.drawImage(this._maskC, 0, 0, R.W, R.H, 0, 0, R.W, R.H);
      g.globalCompositeOperation = 'source-over';
      L.key = key;
    }
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(L.c, 0, 0, R.W, R.H, R.X0, R.Y0, R.W, R.H);
    ctx.restore();
  }

  // Row displacement from the ground (_shimmer picks it) into g: thin rows,
  // each shifted sideways by offAt(the row's ground y, in hexes). Rows are
  // keyed to the ground, not the screen, so the ripples stay put on the map
  // while the camera glides.
  _rows(g, R, cam, sz, rowCss, offAt) {
    const src = this._rowSrc, sx = this._rowX, sy = this._rowY;
    const step = Math.max(Math.round(rowCss * R.dpr), Math.ceil(R.H / 240), 1);
    for (let y = 0; y < R.H; y += step) {
      const hh = Math.min(step, R.H - y);
      const yw = ((R.Y0 + y + hh / 2) / R.dpr - cam.oy) / sz;
      g.drawImage(src, sx, sy + y, R.W, hh, offAt(yw) * R.dpr, y, R.W, hh);
    }
  }

  // Torus textures in axial space: one texel grid laid over the whole map,
  // so a 'repeat' pattern wraps exactly where the map does. cloud: shadow
  // coverage; light/dark: the gust field (wind.js) as a pale or a dark wash.
  _textures() {
    if (this._tex !== null) return this._tex;
    if (typeof DOMMatrix === 'undefined' || typeof CanvasPattern === 'undefined' ||
        !CanvasPattern.prototype.setTransform) { this._tex = false; return false; }
    const K = GFX_TEX_K, W = MAP_COLS * K, H = MAP_ROWS * K;
    const mk = () => { const c = document.createElement('canvas'); c.width = W; c.height = H; return c; };
    const cloud = mk(), light = mk(), dark = mk();
    const ci = new ImageData(W, H), li = new ImageData(W, H), di = new ImageData(W, H);
    for (let v = 0; v < H; v++) {
      for (let u = 0; u < W; u++) {
        const q = (u + 0.5) / K, r = (v + 0.5) / K, o = (v * W + u) * 4;
        const ca = windSmooth(0.5, 0.66, windFbm(q, r, GFX_CLOUD_OCTAVES, GFX_CLOUD_SEED));
        const ga = windGustCurve(windFbm(q, r, WIND_GUST_OCTAVES, WIND_GUST_SEED));
        ci.data[o] = GFX_CLOUD_RGB[0]; ci.data[o + 1] = GFX_CLOUD_RGB[1]; ci.data[o + 2] = GFX_CLOUD_RGB[2];
        ci.data[o + 3] = (ca * 255) | 0;
        li.data[o] = 255; li.data[o + 1] = 248; li.data[o + 2] = 228; li.data[o + 3] = (ga * 255) | 0;
        di.data[o] = 4;   di.data[o + 1] = 12;  di.data[o + 2] = 14;  di.data[o + 3] = (ga * 255) | 0;
      }
    }
    cloud.getContext('2d').putImageData(ci, 0, 0);
    light.getContext('2d').putImageData(li, 0, 0);
    dark.getContext('2d').putImageData(di, 0, 0);
    this._tex = { cloud, light, dark };
    return this._tex;
  }

  _pattern(g, name) {
    let p = this._pats.get(g);
    if (!p) { p = {}; this._pats.set(g, p); }
    return p[name] || (p[name] = g.createPattern(this._tex[name], 'repeat'));
  }

  // Texel (u, v) -> axial (u/K + drift) -> screen, for the camera this frame.
  // rate: how fast this layer rides the air (1 = the cloud deck).
  _patMatrix(cam, sz, rate) {
    const d = Wind.driftAxial(rate), S = sz / GFX_TEX_K;
    return new DOMMatrix([1.5 * S, SQRT3 / 2 * S, 0, SQRT3 * S,
                          cam.ox + sz * 1.5 * d.q, cam.oy + sz * (SQRT3 / 2 * d.q + SQRT3 * d.r)]);
  }
}
