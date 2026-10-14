// ── Debris Field ─────────────────────────────────────────────────────────────
// Space debris coming down. One object enters on a shallow line, breaks up
// high overhead (the double crack the K10 speaker plays), and 1-8 fragments
// streak down to their own impact hexes, each leaving a smoke trail that hangs
// in the upper air and a plume that climbs off the crater and stays there.
//
// The server picks everything -- heading, breakup point, which piece lands
// where, how big it is and when -- and sends it as one 'debris' event (see
// network.js). This class only renders the event it is handed, so every
// client watches the same pieces fall onto the same hexes.
//
// Map space, like bike-dust.js: world units are hex units (hexToPixel at size
// 1), so a trail hangs where it was laid while the camera rides along with
// you, and holds through a zoom. Altitude is a third axis, drawn as a lift up
// the screen -- the tiles are a 3/4 view, where higher means further up.
//
// The sky is not information: trails, fireballs and plumes draw over fogged
// hexes the way the storm wash does, which is also what makes a plume a
// beacon for the crash site. Only the crater itself (the terrain flip
// network.js applies at each landing) stays behind the fog.
//
// Three passes, called from renderer.js's LAYERS:
//   drawGround -- scorch and the shockwave's dust ring, under the pawns
//   drawAir    -- smoke trails, plumes and cold ejecta, under the night tint
//   drawLight  -- fireballs, sparks, flashes and crater glow, over the tint,
//                 because a fireball at night is the brightest thing there is
// Every timeline is a function of the event's age, not a frame count, so a
// throttled or hidden tab picks up exactly where the fall really is.
// Plain global class -- no ES module syntax.

const DEBRIS_LIFT       = 0.8;    // screen lift per unit of altitude (hex units)
const DEBRIS_ENTRY_ALT  = 9.5;    // altitude where the body comes into view
const DEBRIS_BREAK_ALT  = 5.5;    // altitude of the breakup
const DEBRIS_ENTRY_LEN  = 26;     // ground track before the breakup, world units
const DEBRIS_TRAIL_STEP = 0.3;    // world units between smoke nodes along a path
const DEBRIS_MAIN_LIFE  = 95000;  // the parent body's trail, ms
const DEBRIS_SPARK_CAP  = 420;
const DEBRIS_DUST_CAP   = 200;
const DEBRIS_SCORCH_MS  = 60000;
const DEBRIS_RING_MS    = 1000;
// Dark grey with a little brown in it: burnt metal and lofted soil, not the
// near-black of the fire smoke (weather-particle-system.js).
const DEBRIS_SMOKE_RGB  = [86, 77, 68];

// Per size class -- 0 small, 1 medium, 2 large (the main mass, which also gets
// the full crater). Lengths are world units, times ms.
const DEBRIS_CLASS = [
  { head: 0.45, trailW: 0.50, trailLife: 40000, twist: 0.6, flash: 1.1, ring: 1.3, shake: 1.2,
    rocks: 5,  plume: 14, plumeH: 1.5, plumeW: 0.55, plumeLife: 24000, plumeSpawn: 1200, glow: 2500,  scorch: 0.45, flames: 5 },
  { head: 0.70, trailW: 0.70, trailLife: 60000, twist: 0.9, flash: 1.8, ring: 2.1, shake: 3.5,
    rocks: 12, plume: 32, plumeH: 2.5, plumeW: 0.80, plumeLife: 50000, plumeSpawn: 2000, glow: 7000,  scorch: 0.7,  flames: 10 },
  { head: 1.00, trailW: 0.95, trailLife: 80000, twist: 1.2, flash: 2.8, ring: 3.4, shake: 9,
    rocks: 26, plume: 64, plumeH: 3.8, plumeW: 1.15, plumeLife: 85000, plumeSpawn: 3000, glow: 15000, scorch: 1.0,  flames: 20 },
];

// ── Smoke and dust sprites ───────────────────────────────────────────────────
// A lumpy puff, lit on its crowns and darkened toward its base -- the same
// construction as bike-dust.js's, because a self-shaded mound is what reads on
// any ground; a flat smudge of one tone vanishes into it. Baked once per
// colour and shape; drawing is a bare drawImage().
const DebrisSprites = (() => {
  const cache = new Map();
  const clamp = (v) => Math.max(0, Math.min(255, v)) | 0;
  const canvas = (S) => {
    const c = document.createElement('canvas');
    c.width = c.height = S;
    return c;
  };

  function puff(rgb, v) {
    const key = `${rgb}|${v}`;
    let c = cache.get(key);
    if (c) return c;
    const S = 96;
    c = canvas(S);
    const g = c.getContext('2d');
    const [r0, g0, b0] = rgb;
    const tone = (k) => `${clamp(r0 + k)},${clamp(g0 + k)},${clamp(b0 + k * 0.85)}`;
    const hi = tone(30), mid = tone(0), lo = tone(-34);
    let seed = Math.imul(v + 1, 2654435761) >>> 0;
    for (const n of rgb) seed = (Math.imul(seed, 31) + n) >>> 0;
    const rnd = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
    g.filter = 'blur(2.5px)';   // no-op where unsupported
    const lumps = [];
    for (let i = 0; i < 10; i++) {
      const a = rnd() * Math.PI * 2, d = Math.sqrt(rnd()) * 19;
      lumps.push({ x: S / 2 + Math.cos(a) * d * 1.1, y: S / 2 + Math.sin(a) * d * 0.85, r: 11 + rnd() * 13 });
    }
    lumps.sort((p, q) => p.y - q.y);   // lower lumps last: the mound's base sits in front
    for (const L of lumps) {
      const grad = g.createRadialGradient(L.x - L.r * 0.35, L.y - L.r * 0.4, 0, L.x, L.y, L.r);
      grad.addColorStop(0,    `rgba(${hi},0.95)`);
      grad.addColorStop(0.45, `rgba(${mid},0.85)`);
      grad.addColorStop(0.8,  `rgba(${lo},0.5)`);
      grad.addColorStop(1,    `rgba(${lo},0)`);
      g.fillStyle = grad;
      g.beginPath();
      g.arc(L.x, L.y, L.r, 0, Math.PI * 2);
      g.fill();
    }
    g.filter = 'none';
    g.globalCompositeOperation = 'source-atop';
    const base = g.createLinearGradient(0, S * 0.35, 0, S * 0.9);
    base.addColorStop(0, 'rgba(30,24,18,0)');
    base.addColorStop(1, 'rgba(30,24,18,0.42)');
    g.fillStyle = base;
    g.fillRect(0, 0, S, S);
    cache.set(key, c);
    return c;
  }

  // The same puff gone to haze: lumps blurred away, a touch wider. Old trail
  // and old plume both end up here -- smoke that has hung for a minute has no
  // edges left.
  function haze(rgb, v) {
    const key = `${rgb}|${v}|haze`;
    let c = cache.get(key);
    if (c) return c;
    const src = puff(rgb, v), S = src.width;
    c = canvas(S);
    const g = c.getContext('2d');
    g.filter = 'blur(7px)';
    g.drawImage(src, S * 0.06, S * 0.06, S * 0.88, S * 0.88);
    g.filter = 'none';
    cache.set(key, c);
    return c;
  }

  // Scorched ground: a soft dark pool, sootiest in the middle.
  function scorch() {
    let c = cache.get('scorch');
    if (c) return c;
    const S = 96, cx = S / 2;
    c = canvas(S);
    const g = c.getContext('2d');
    const grad = g.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(14,11,9,0.9)');
    grad.addColorStop(0.45, 'rgba(22,17,13,0.6)');
    grad.addColorStop(1,    'rgba(30,24,18,0)');
    g.fillStyle = grad;
    g.fillRect(0, 0, S, S);
    cache.set('scorch', c);
    return c;
  }

  return { puff, haze, scorch };
})();

// Value noise, 0..1, smooth. Drives the twist an old trail picks up from the
// wind shear up there -- a hashed lattice, so no sine ever shows.
function _debrisHash(i) {
  let h = Math.imul(i ^ 0x9e3779b9, 0x85ebca6b);
  h ^= h >>> 13;
  h = Math.imul(h, 0xc2b2ae35);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}
function _debrisNoise(x) {
  const i = Math.floor(x), f = x - i, u = f * f * (3 - 2 * f);
  return _debrisHash(i) * (1 - u) + _debrisHash(i + 1) * u;
}

class DebrisField {
  constructor() {
    this.events = [];
    this.sparks = [];   // airborne light: head sparks, breakup burst, impact flames
    this.rocks  = [];   // ejecta on ballistic arcs
    this.dust   = [];   // ground puffs where ejecta land
    this.shakes = [];   // { born, amp, life, x, y, ent } -- x null = felt everywhere
    this.flashes = [];  // { x, y, h, born, R, life, screen, ent }
    this.wind   = { x: 0, y: 0 };   // integrated drift, world units
    this.last   = 0;
    this._seq   = 0;
  }

  // ── Event intake ──────────────────────────────────────────────────────────
  // ev: { tb, bq, br, hd, f: [{ q, r, s, t }] } -- tb the breakup and t each
  // landing in ms after the event arrived, (bq, br) the hex the body breaks up
  // over, hd its heading in degrees (world space, 0 = east, 90 = south).
  spawnFromEvent(ev, now = Date.now()) {
    if (!ev?.f?.length) return;
    const tb  = ev.tb ?? 1400;
    const B   = this._world(ev.bq, ev.br);
    const ang = (ev.hd ?? 0) * Math.PI / 180;
    const dir = { x: Math.cos(ang), y: Math.sin(ang) };
    const E   = { x: B.x - dir.x * DEBRIS_ENTRY_LEN, y: B.y - dir.y * DEBRIS_ENTRY_LEN };
    const seed = (++this._seq * 7919) >>> 0;
    const ent = { born: now, tb, bq: ev.bq, br: ev.br, B, E, dir, seed, broke: false, frags: [], trails: [] };
    ev.f.forEach((f, i) => {
      // The server wraps every hex onto the board; a piece that landed across
      // the seam from its breakup point comes back next to it here.
      const u   = this._nearest(f.q, f.r, ev.bq, ev.br);
      const P   = this._world(u.q, u.r);
      const s   = Math.max(0, Math.min(2, f.s | 0));
      const d   = Math.hypot(P.x - B.x, P.y - B.y) || 1;
      ent.frags.push({
        i, s, cls: DEBRIS_CLASS[s], P, t: Math.max(tb + 300, f.t | 0),
        perp: { x: -(P.y - B.y) / d, y: (P.x - B.x) / d },
        terrain: gameMap?.[f.r]?.[f.q]?.terrain ?? null,
        landed: false, sparkAcc: 0, puffs: null,
      });
    });
    ent.trails.push(this._trail(ent, -1));
    for (const fr of ent.frags) ent.trails.push(this._trail(ent, fr.i));
    let until = tb + DEBRIS_MAIN_LIFE;
    for (const fr of ent.frags) {
      until = Math.max(until, fr.t + fr.cls.trailLife, fr.t + fr.cls.plumeSpawn + fr.cls.plumeLife, fr.t + DEBRIS_SCORCH_MS);
    }
    ent.until = now + until + 500;
    this.events.push(ent);
  }

  // ── Paths ─────────────────────────────────────────────────────────────────
  // The parent body (-1) comes in on a straight line; a fragment decelerates
  // along the ground and drops ever more steeply, so the last stretch of each
  // streak bends down into its crater instead of skimming in flat.
  _pathAt(ent, idx, u) {
    if (idx < 0) {
      return {
        x: ent.E.x + (ent.B.x - ent.E.x) * u,
        y: ent.E.y + (ent.B.y - ent.E.y) * u,
        h: DEBRIS_ENTRY_ALT + (DEBRIS_BREAK_ALT - DEBRIS_ENTRY_ALT) * u,
      };
    }
    const fr = ent.frags[idx];
    const g  = 1 - Math.pow(1 - u, 1.35);
    return {
      x: ent.B.x + (fr.P.x - ent.B.x) * g,
      y: ent.B.y + (fr.P.y - ent.B.y) * g,
      h: DEBRIS_BREAK_ALT * (1 - Math.pow(u, 1.5)),
    };
  }

  _span(ent, idx) {
    return idx < 0 ? [0, ent.tb] : [ent.tb, ent.frags[idx].t];
  }

  _trail(ent, idx) {
    let len = 0, prev = this._pathAt(ent, idx, 0);
    for (let k = 1; k <= 24; k++) {
      const p = this._pathAt(ent, idx, k / 24);
      len += Math.hypot(p.x - prev.x, p.y - prev.y, p.h - prev.h);
      prev = p;
    }
    const fr = idx < 0 ? null : ent.frags[idx];
    const cls = fr ? fr.cls : DEBRIS_CLASS[2];
    return {
      idx,
      n: Math.max(2, Math.ceil(len / DEBRIS_TRAIL_STEP)),
      emitted: 0,
      nodes: [],
      life:  fr ? cls.trailLife : DEBRIS_MAIN_LIFE,
      w:     fr ? cls.trailW : 1.1,
      w0:    fr ? 0.05 + 0.05 * cls.head : 0.1,
      twist: fr ? cls.twist : 1.6,
      head:  fr ? cls.head : 1.15,
      perp:  fr ? fr.perp : { x: -ent.dir.y, y: ent.dir.x },
      seed:  ent.seed + (idx + 1) * 131,
    };
  }

  // ── Per frame ─────────────────────────────────────────────────────────────
  update(now) {
    const dt = this.last ? Math.min(100, Math.max(0, now - this.last)) : 16;
    this.last = now;
    if (!this.events.length && !this.sparks.length && !this.rocks.length && !this.dust.length) return;

    // One drift for everything up there, on the same wind (wind.js) the fire
    // smoke and the cloud shadows follow -- smoke that all leans one way reads
    // as weather. High up it never quite stops, even on a still day.
    if (typeof Wind !== 'undefined') {
      const k = (0.000015 + Wind.s * 0.00006) * dt;
      this.wind.x += Wind.x * k;
      this.wind.y += Wind.y * k;
    } else {
      this.wind.x += 0.00004 * dt;
      this.wind.y += 0.000012 * dt;
    }

    for (const ent of this.events) this._stepEvent(ent, now, dt);
    this.events = this.events.filter(e => now < e.until);

    for (const p of this.sparks) {
      p.x += p.vx * dt; p.y += p.vy * dt; p.h += p.vh * dt;
      const drag = Math.max(0, 1 - p.drag * dt);
      p.vx *= drag; p.vy *= drag;
      p.vh = p.vh * drag - p.grav * dt;
      if (p.h < 0) { p.h = 0; p.vh = 0; }
    }
    this.sparks = this.sparks.filter(p => now - p.born < p.life);

    for (const r of this.rocks) {
      r.x += r.vx * dt; r.y += r.vy * dt; r.h += r.vh * dt;
      r.vh -= 0.000018 * dt;
      if (r.hot && Math.random() < 0.5) this._spark(r.x, r.y, r.h, 0, 0, 0.0002, now, 180 + Math.random() * 160, 0.05 + r.size * 0.6, 2, 0.002, 0);
      if (r.h <= 0) {
        r.dead = true;
        if (this.dust.length < DEBRIS_DUST_CAP) {
          this.dust.push({ x: r.x, y: r.y, born: now, life: 1400 + Math.random() * 900,
            s0: 0.06, s1: 0.18 + r.size * 2, a: 0.55, rgb: r.rgb, v: (Math.random() * 3) | 0 });
        }
      }
    }
    this.rocks = this.rocks.filter(r => !r.dead);
    this.dust  = this.dust.filter(d => now - d.born < d.life);
    this.shakes  = this.shakes.filter(s => now - s.born < s.life);
    this.flashes = this.flashes.filter(f => now - f.born < f.life);
  }

  _stepEvent(ent, now, dt) {
    const age = now - ent.born;

    // Smoke nodes, laid along each path up to wherever its head has reached.
    // Each node's birth is the moment its head passed that point, so a
    // dropped frame (or a hidden tab) still leaves the right ages behind.
    for (const tr of ent.trails) {
      if (tr.emitted <= tr.n) {
        const [t0, t1] = this._span(ent, tr.idx);
        const u = Math.max(0, Math.min(1, (age - t0) / (t1 - t0)));
        while (tr.emitted <= tr.n && tr.emitted / tr.n <= u) {
          const uk = tr.emitted / tr.n;
          const p  = this._pathAt(ent, tr.idx, uk);
          tr.nodes.push({ x: p.x, y: p.y, h: p.h, k: tr.emitted,
            born: ent.born + t0 + uk * (t1 - t0), wx: this.wind.x, wy: this.wind.y });
          tr.emitted++;
        }
      }
      if (tr.nodes.length && now - tr.nodes[0].born > tr.life) {
        tr.nodes = tr.nodes.filter(n => now - n.born <= tr.life);
      }
    }

    // Sparks shed by every head in flight.
    if (age < ent.tb) this._shed(ent, -1, age / ent.tb, 1.15, now, dt);
    for (const fr of ent.frags) {
      if (age >= ent.tb && age < fr.t) this._shed(ent, fr.i, (age - ent.tb) / (fr.t - ent.tb), fr.cls.head, now, dt);
    }

    // The breakup: a flash up in the sky, a spray of sparks, and the boom --
    // a faint shudder on every screen, wherever you are standing.
    if (!ent.broke && age >= ent.tb) {
      ent.broke = true;
      const at = ent.born + ent.tb;
      this.flashes.push({ x: ent.B.x, y: ent.B.y, h: DEBRIS_BREAK_ALT, born: at, R: 2.6, life: 700, screen: 0.12, ent });
      this.shakes.push({ born: at, amp: 2.4, life: 650, x: null, ent });
      for (let k = 0; k < 46; k++) {
        const a = Math.random() * Math.PI * 2, sp = 0.002 + Math.random() * 0.007;
        this._spark(ent.B.x, ent.B.y, DEBRIS_BREAK_ALT, Math.cos(a) * sp, Math.sin(a) * sp * 0.8,
          (Math.random() - 0.5) * 0.006, now, 500 + Math.random() * 700, 0.07 + Math.random() * 0.1, 1, 0.0018, 0.000004);
      }
    }

    for (const fr of ent.frags) {
      if (!fr.landed && age >= fr.t) this._land(ent, fr, ent.born + fr.t, now);
    }
  }

  // Sparks peeling off a head, thrown back along its line with a little
  // scatter, then falling away and walking down the fire ramp as they cool.
  _shed(ent, idx, u, head, now, dt) {
    const a = this._pathAt(ent, idx, Math.max(0, u - 0.01));
    const b = this._pathAt(ent, idx, Math.min(1, u + 0.01));
    const [t0, t1] = this._span(ent, idx);
    const k = 1 / ((t1 - t0) * 0.02);
    const vx = (b.x - a.x) * k, vy = (b.y - a.y) * k, vh = (b.h - a.h) * k;
    const holder = idx < 0 ? ent : ent.frags[idx];
    holder.sparkAcc = (holder.sparkAcc || 0) + dt * 0.07 * head;
    while (holder.sparkAcc >= 1) {
      holder.sparkAcc -= 1;
      const p = this._pathAt(ent, idx, u);
      const f = 0.25 + Math.random() * 0.3;
      this._spark(p.x, p.y, p.h,
        vx * f + (Math.random() - 0.5) * 0.003, vy * f + (Math.random() - 0.5) * 0.003, vh * f - 0.0008,
        now, 300 + Math.random() * 450, (0.05 + Math.random() * 0.08) * (0.6 + head * 0.5), 1 + ((Math.random() * 2) | 0), 0.0015, 0.000006);
    }
  }

  _spark(x, y, h, vx, vy, vh, born, life, size, ramp, drag, grav, kind = 'ember') {
    if (this.sparks.length >= DEBRIS_SPARK_CAP) return;
    this.sparks.push({ x, y, h, vx, vy, vh, born, life, size, ramp, drag, grav, kind });
  }

  // One piece hits. Everything it sets off is timed from the landing itself.
  _land(ent, fr, at, now) {
    fr.landed = true;
    const { P, cls } = fr;
    this.flashes.push({ x: P.x, y: P.y, h: 0.2, born: at, R: cls.flash, life: 560, screen: fr.s === 2 ? 0.24 : fr.s === 1 ? 0.08 : 0, ent });
    if (cls.shake > 0) this.shakes.push({ born: at, amp: cls.shake, life: 600 + fr.s * 200, x: P.x, y: P.y, ent });

    // The fireball: flame tongues bursting up out of the hole.
    for (let k = 0; k < cls.flames; k++) {
      const a = Math.random() * Math.PI * 2, sp = Math.random() * 0.0012 * (0.6 + cls.head);
      this._spark(P.x + Math.cos(a) * 0.1, P.y + Math.sin(a) * 0.08, 0.05,
        Math.cos(a) * sp, Math.sin(a) * sp * 0.75, 0.0015 + Math.random() * 0.0025 * cls.head,
        now, 450 + Math.random() * 500, (0.22 + Math.random() * 0.3) * (0.5 + cls.head * 0.7), 0, 0.002, 0.000001, 'flame');
    }

    // Ejecta: lumps of ground and wreck thrown out on arcs, some still glowing.
    const ground = this._dustRgb(fr.terrain);
    for (let k = 0; k < cls.rocks; k++) {
      const a = Math.random() * Math.PI * 2, sp = (0.0012 + Math.random() * 0.003) * (0.6 + cls.head * 0.6);
      this.rocks.push({ x: P.x, y: P.y, h: 0.05, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp * 0.8,
        vh: 0.003 + Math.random() * 0.006 * (0.5 + cls.head), hot: Math.random() < 0.45,
        size: 0.025 + Math.random() * 0.045 * (0.6 + cls.head), rgb: ground });
    }

    // The plume. A first quarter of low ground-coloured dust rolls out along
    // the surface; the rest is smoke that climbs, slows, and opens into a cap
    // at the top -- the higher a puff ends up, the wider it spreads and the
    // bigger it grows -- then hangs there and leans off with the wind.
    const puffs = [];
    for (let k = 0; k < cls.plume; k++) {
      const base = k < cls.plume * 0.25;
      const delay = cls.plumeSpawn * Math.pow(k / cls.plume, 1.4);
      const Ht = base ? 0.05 + Math.random() * 0.35 : cls.plumeH * (0.25 + 0.75 * Math.sqrt(Math.random()));
      const hf = Ht / cls.plumeH;
      const a  = Math.random() * Math.PI * 2;
      // Scattered across the column's width, each rising at its own rate:
      // puffs that all climb the same line at the same pace string out into
      // a chain of beads instead of a column.
      puffs.push({
        x: P.x + (Math.random() - 0.5) * cls.plumeW * 0.4, y: P.y + (Math.random() - 0.5) * cls.plumeW * 0.3,
        born: at + delay, Ht, ax: Math.cos(a), ay: Math.sin(a) * 0.75,
        out: base ? cls.plumeW * (0.3 + Math.random() * 0.7) : cls.plumeW * 0.9 * hf * hf * (0.4 + Math.random() * 0.6),
        s1: base ? cls.plumeW * (0.7 + Math.random() * 0.4) : cls.plumeW * (0.6 + 0.6 * hf) * (0.85 + Math.random() * 0.3),
        tr: (700 + 900 * hf) * (0.7 + Math.random() * 0.6),
        life: cls.plumeLife * (base ? 0.35 + Math.random() * 0.15 : 0.7 + Math.random() * 0.3),
        rgb: base ? ground : DEBRIS_SMOKE_RGB, k: (Math.random() * 997) | 0, wx: null, wy: null,
      });
    }
    fr.puffs = puffs;
    fr.landedAt = at;
  }

  _dustRgb(terrain) {
    const c = (typeof BIKE_DUST_COLOR !== 'undefined') ? BIKE_DUST_COLOR[terrain] : null;
    return c ? c.map(v => Math.round(v * 0.82)) : [150, 142, 128];
  }

  // ── Camera ────────────────────────────────────────────────────────────────
  // Strongest shake this frame, in px. The breakup is felt everywhere (the
  // boom); a landing is felt in full within a few hexes and not at all from
  // across the map.
  peakShake(now, meRp) {
    if (!this.shakes.length) return 0;
    const c = hexToPixel(meRp.q, meRp.r, 1);
    let best = 0;
    for (const s of this.shakes) {
      const t = (now - s.born) / s.life;
      if (t < 0 || t >= 1) continue;
      let amp = s.amp * (t < 0.1 ? t / 0.1 : Math.pow(1 - (t - 0.1) / 0.9, 1.6));
      if (s.x !== null) {
        const o = this._wrapOffset(s.ent, meRp);
        const d = Math.hypot(s.x + o.x - c.x, s.y + o.y - c.y);
        amp *= Math.max(0, Math.min(1, 1 - (d - 8) / 22));
      }
      if (amp > best) best = amp;
    }
    return best;
  }

  // ── Drawing ───────────────────────────────────────────────────────────────
  drawGround(ctx, cam, hs, W, H) {
    if (!this.events.length && !this.dust.length) return;
    const now = Date.now();
    const scorch = DebrisSprites.scorch();
    ctx.save();
    for (const ent of this.events) {
      const o = this._frame(ent, cam, hs);
      for (const fr of ent.frags) {
        if (!fr.landed) continue;
        const age = now - fr.landedAt;
        const X = fr.P.x * hs + o.x, Y = fr.P.y * hs + o.y;
        if (X < -hs * 5 || Y < -hs * 5 || X > W + hs * 5 || Y > H + hs * 5) continue;
        // Scorch: the burn round the hole, fading as the ash blows off.
        if (age < DEBRIS_SCORCH_MS) {
          const s = fr.cls.scorch * hs;
          ctx.globalAlpha = 0.65 * Math.pow(1 - age / DEBRIS_SCORCH_MS, 1.3);
          ctx.drawImage(scorch, X - s, Y - s * 0.8, s * 2, s * 1.6);
        }
        // The shockwave: a ring of ground thrown up and racing outward,
        // squashed a little into the 3/4 view.
        if (age < DEBRIS_RING_MS) {
          const u = age / DEBRIS_RING_MS;
          const R = fr.cls.ring * (1 - Math.pow(1 - u, 2.2));
          const n = 14 + fr.s * 8;
          const ground = this._dustRgb(fr.terrain);
          for (let k = 0; k < n; k++) {
            const a = (k + _debrisHash(k + fr.i * 97) * 0.6) / n * Math.PI * 2;
            const rr = R * (0.88 + _debrisHash(k * 3 + 1) * 0.24);
            const s = hs * (0.12 + 0.3 * u) * (0.6 + fr.cls.head * 0.6);
            const x = X + Math.cos(a) * rr * hs, y = Y + Math.sin(a) * rr * hs * 0.75;
            ctx.globalAlpha = 0.75 * Math.pow(1 - u, 1.2);
            ctx.drawImage(DebrisSprites.puff(ground, k % 3), x - s, y - s * 0.6, s * 2, s * 1.2);
          }
          // The pressure front itself, a thin pale line for an instant.
          if (u < 0.35) {
            ctx.globalAlpha = 0.4 * (1 - u / 0.35);
            ctx.strokeStyle = 'rgb(246,232,206)';
            ctx.lineWidth = Math.max(1, hs * 0.03);
            ctx.beginPath();
            ctx.ellipse(X, Y, R * hs, R * hs * 0.75, 0, 0, Math.PI * 2);
            ctx.stroke();
          }
        }
      }
    }
    // Puffs where the ejecta came down.
    for (const d of this.dust) {
      const o = this._frameXY(d.x, d.y, cam, hs);
      const t = (now - d.born) / d.life;
      const s = (d.s0 + (d.s1 - d.s0) * (1 - (1 - t) * (1 - t))) * hs;
      if (o.x < -s || o.y < -s || o.x > W + s || o.y > H + s) continue;
      ctx.globalAlpha = d.a * (1 - t);
      ctx.drawImage(DebrisSprites.puff(d.rgb, d.v), o.x - s, o.y - s * 0.6, s * 2, s * 1.2);
    }
    ctx.restore();
  }

  drawAir(ctx, cam, hs, W, H) {
    if (!this.events.length && !this.rocks.length) return;
    const now = Date.now();
    ctx.save();
    for (const ent of this.events) {
      const o = this._frame(ent, cam, hs);
      for (const tr of ent.trails) this._drawTrail(ctx, tr, o, hs, now, W, H);
      for (const fr of ent.frags) if (fr.puffs) this._drawPlume(ctx, fr, o, hs, now, W, H);
    }
    // Cold ejecta: dark clods in flight.
    ctx.fillStyle = 'rgb(34,28,22)';
    for (const r of this.rocks) {
      if (r.hot) continue;
      const p = this._frameXY(r.x, r.y - r.h * DEBRIS_LIFT, cam, hs);
      ctx.globalAlpha = 0.9;
      ctx.beginPath();
      ctx.arc(p.x, p.y, Math.max(1, r.size * hs), 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();
  }

  // A trail is born thin and nearly opaque, then over the next minute widens,
  // thins out, drifts with the wind, and is bent into slow kinks by the shear
  // -- the way a real persistent train twists after a bright meteor.
  //
  // Cost: once it has spread, neighbouring nodes overlap several times over,
  // so a wide stretch draws every second or third node, a little denser to
  // make up the difference -- the chain stays unbroken because the spacing
  // is still well inside the puffs' width.
  _drawTrail(ctx, tr, o, hs, now, W, H) {
    for (const nd of tr.nodes) {
      const age = now - nd.born;
      if (age < 0) continue;
      const r  = tr.w0 + (tr.w - tr.w0) * (1 - Math.exp(-age / 14000));
      const thin = r > 0.6 ? 3 : r > 0.32 ? 2 : 1;
      if (nd.k % thin) continue;
      const A  = tr.twist * (1 - Math.exp(-age / 22000));
      const n1 = _debrisNoise(nd.k * 0.085 + tr.seed + age * 0.000018) - 0.5;
      const n2 = _debrisNoise(nd.k * 0.21 + tr.seed * 1.7 - age * 0.00003 + 100) - 0.5;
      const off = A * (n1 + 0.6 * n2) * 2;
      const hf = 0.6 + 0.4 * nd.h / DEBRIS_ENTRY_ALT;   // higher air moves faster
      const x  = nd.x + tr.perp.x * off + (this.wind.x - nd.wx) * hf;
      const y  = nd.y + tr.perp.y * off + (this.wind.y - nd.wy) * hf - nd.h * DEBRIS_LIFT;
      const X = x * hs + o.x, Y = y * hs + o.y, s = r * hs;
      if (X < -s || Y < -s || X > W + s || Y > H + s) continue;
      const t   = age / tr.life;
      const out = t < 0.55 ? 1 : Math.pow(1 - (t - 0.55) / 0.45, 1.5);
      const a   = Math.min(0.9, 0.72 * Math.min(1, age / 250) * out * Math.pow(tr.w0 / r, 0.32) * (1 + 0.28 * (thin - 1)));
      if (a < 0.01) continue;
      ctx.globalAlpha = a;
      ctx.drawImage(this._smokeSprite(DEBRIS_SMOKE_RGB, nd.k, age, 4000, 20000), X - s, Y - s, s * 2, s * 2);
    }
  }

  // Lumpy while fresh, haze once it has hung a while. One sprite per puff, not
  // a cross-fade of two (that doubled the fill cost of every old puff); each
  // puff goes over at its own moment, so the change ripples through the cloud
  // instead of popping all at once.
  _smokeSprite(rgb, k, age, t0, span) {
    const v = k % 3;
    return (age - t0) / span > _debrisHash(k * 7 + 3)
      ? DebrisSprites.haze(rgb, v) : DebrisSprites.puff(rgb, v);
  }

  _drawPlume(ctx, fr, o, hs, now, W, H) {
    const cls = fr.cls;
    for (const p of fr.puffs) {
      const age = now - p.born;
      if (age < 0 || age > p.life) continue;
      if (p.wx === null) { p.wx = this.wind.x; p.wy = this.wind.y; }
      const rise = 1 - Math.exp(-age / p.tr);
      const h    = p.Ht * rise;
      const hf   = 0.3 + 0.7 * h / cls.plumeH;
      const sp   = p.out * (1 - Math.exp(-age / 4000));
      const x    = p.x + p.ax * sp + (this.wind.x - p.wx) * hf;
      const y    = p.y + p.ay * sp + (this.wind.y - p.wy) * hf - h * DEBRIS_LIFT;
      const s    = p.s1 * (0.55 + 0.45 * (1 - Math.exp(-age / 2500))) * hs;
      const X = x * hs + o.x, Y = y * hs + o.y;
      if (X < -s || Y < -s || X > W + s || Y > H + s) continue;
      // Dense while it boils up, then thinning as it spreads: the cloud that
      // hangs there for a minute is a veil over the board, not a blackout.
      const t   = age / p.life;
      const out = t < 0.6 ? 1 : Math.pow(1 - (t - 0.6) / 0.4, 1.4);
      const a   = 0.82 * Math.min(1, age / 250) * out * (0.42 + 0.58 * Math.exp(-age / 9000));
      if (a < 0.01) continue;
      ctx.globalAlpha = a;
      ctx.drawImage(this._smokeSprite(p.rgb, p.k, age, 6000, 24000), X - s, Y - s, s * 2, s * 2);
    }
  }

  drawLight(ctx, cam, hs, W, H) {
    if (!this.events.length && !this.sparks.length && !this.flashes.length && !this.rocks.length) return;
    if (typeof FireSprites === 'undefined') return;
    const bank = FireSprites();
    const now = Date.now();
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';

    for (const ent of this.events) {
      const o = this._frame(ent, cam, hs);
      const age = now - ent.born;

      // The hot end of each trail: the freshest nodes still glow from the
      // head that just went through them, which is what draws the streak.
      for (const tr of ent.trails) {
        for (let k = tr.nodes.length - 1; k >= 0; k--) {
          const nd = tr.nodes[k], na = now - nd.born;
          if (na > 800) break;
          if (na < 0 || (nd.k & 1)) continue;   // every other node: the glows overlap anyway
          const f = 1 - na / 800;
          const X = nd.x * hs + o.x, Y = (nd.y - nd.h * DEBRIS_LIFT) * hs + o.y;
          const s = (0.22 + 0.22 * (1 - f)) * tr.head * hs;
          ctx.globalAlpha = f * f * 0.75;
          ctx.drawImage(bank.glow, X - s, Y - s, s * 2, s * 2);
          if (na < 260) {
            const c = 0.12 * tr.head * hs * (1 + na / 260);
            ctx.globalAlpha = 1 - na / 260;
            ctx.drawImage(bank.ember[1], X - c, Y - c, c * 2, c * 2);
          }
        }
      }

      // The heads themselves.
      if (age < ent.tb) this._drawHead(ctx, ent, -1, age / ent.tb, 1.15 * (0.35 + 0.65 * Math.min(1, age / 600)), o, hs, now, bank);
      for (const fr of ent.frags) {
        if (age >= ent.tb && age < fr.t) this._drawHead(ctx, ent, fr.i, (age - ent.tb) / (fr.t - ent.tb), fr.cls.head, o, hs, now, bank);
        // The crater, still glowing: molten glass cooling in the hole.
        if (fr.landed) {
          const ca = now - fr.landedAt;
          if (ca < fr.cls.glow) {
            const X = fr.P.x * hs + o.x, Y = fr.P.y * hs + o.y;
            const fl = 0.85 + 0.15 * _debrisNoise(now * 0.008 + fr.i * 13);
            const s = 0.75 * (0.6 + 0.4 * fr.cls.head) * hs;
            ctx.globalAlpha = 0.9 * Math.pow(1 - ca / fr.cls.glow, 1.5) * fl;
            ctx.drawImage(bank.glow, X - s, Y - s * 0.8, s * 2, s * 1.6);
          }
          // Firelight under the young plume, lighting its belly.
          if (ca < 2500) {
            const X = fr.P.x * hs + o.x, Y = (fr.P.y - 0.5 * DEBRIS_LIFT) * hs + o.y;
            const s = fr.cls.plumeW * 1.4 * hs;
            ctx.globalAlpha = 0.5 * Math.pow(1 - ca / 2500, 2);
            ctx.drawImage(bank.glow, X - s, Y - s, s * 2, s * 2);
          }
        }
      }
    }

    // Flashes: the breakup up in the sky and every landing. The big ones
    // light the whole screen for an instant, if you are close enough to see.
    for (const f of this.flashes) {
      const t = (now - f.born) / f.life;
      if (t < 0 || t >= 1) continue;
      const env = t < 0.07 ? t / 0.07 : Math.pow(1 - (t - 0.07) / 0.93, 2);
      const o = this._frame(f.ent, cam, hs);
      const X = f.x * hs + o.x, Y = (f.y - f.h * DEBRIS_LIFT) * hs + o.y;
      const s = f.R * (0.7 + 0.3 * t) * hs;
      ctx.globalAlpha = env;
      ctx.drawImage(bank.glow, X - s, Y - s, s * 2, s * 2);
      const c = s * 0.35;
      ctx.drawImage(bank.ember[0], X - c, Y - c, c * 2, c * 2);
      if (f.screen > 0) {
        const near = Math.max(0, Math.min(1, 1 - (Math.hypot(X - W / 2, Y - H / 2) - Math.max(W, H) * 0.5) / (Math.max(W, H) * 0.6)));
        if (near > 0) {
          ctx.globalAlpha = f.screen * env * near;
          ctx.fillStyle = 'rgb(255,196,128)';
          ctx.fillRect(0, 0, W, H);
        }
      }
    }

    // Sparks, flame tongues and glowing ejecta.
    for (const p of this.sparks) {
      const t = (now - p.born) / p.life;
      if (t < 0 || t >= 1) continue;
      const P = this._frameXY(p.x, p.y - p.h * DEBRIS_LIFT, cam, hs);
      if (p.kind === 'flame') {
        const idx = Math.min(FIRE_RAMP.length - 1, (p.ramp + t * 6) | 0);
        const s = p.size * (0.6 + 0.8 * Math.sqrt(t)) * hs;
        if (P.x < -s || P.y < -s || P.x > W + s || P.y > H + s) continue;
        ctx.globalAlpha = (t < 0.12 ? t / 0.12 : 1) * (1 - t) * 0.9;
        ctx.drawImage(bank.flame[idx], P.x - s, P.y - s * 1.3, s * 2, s * 2);
      } else {
        const idx = Math.min(FIRE_RAMP.length - 1, (p.ramp + t * 5) | 0);
        const s = p.size * (1 - 0.5 * t) * hs;
        if (P.x < -s || P.y < -s || P.x > W + s || P.y > H + s) continue;
        ctx.globalAlpha = 1 - t;
        ctx.drawImage(bank.ember[idx], P.x - s, P.y - s, s * 2, s * 2);
      }
    }
    for (const r of this.rocks) {
      if (!r.hot) continue;
      const P = this._frameXY(r.x, r.y - r.h * DEBRIS_LIFT, cam, hs);
      const s = Math.max(2, r.size * 2.4 * hs);
      ctx.globalAlpha = 0.95;
      ctx.drawImage(bank.ember[2], P.x - s, P.y - s, s * 2, s * 2);
    }
    ctx.restore();
  }

  // A head: a white-hot core drawn out along its line of flight, inside an
  // orange halo that flickers as the thing sheds pieces of itself.
  _drawHead(ctx, ent, idx, u, head, o, hs, now, bank) {
    const p = this._pathAt(ent, idx, u);
    const q = this._pathAt(ent, idx, Math.min(1, u + 0.02));
    const X = p.x * hs + o.x, Y = (p.y - p.h * DEBRIS_LIFT) * hs + o.y;
    const dx = (q.x - p.x), dy = (q.y - q.h * DEBRIS_LIFT) - (p.y - p.h * DEBRIS_LIFT);
    const ang = Math.atan2(dy, dx);
    const fl = 0.88 + 0.24 * _debrisNoise(now * 0.02 + idx * 7.3);
    const halo = 1.1 * head * hs * fl;
    ctx.globalAlpha = 0.9;
    ctx.drawImage(bank.glow, X - halo, Y - halo, halo * 2, halo * 2);
    ctx.save();
    ctx.translate(X, Y);
    ctx.rotate(ang);
    const c1 = 0.3 * head * hs, c0 = 0.17 * head * hs;
    ctx.globalAlpha = 1;
    ctx.drawImage(bank.ember[1], -c1 * 2.6, -c1, c1 * 3.2, c1 * 2);
    ctx.drawImage(bank.ember[0], -c0 * 1.6, -c0, c0 * 2.4, c0 * 2);
    ctx.restore();
  }

  // ── Coordinates ───────────────────────────────────────────────────────────
  _world(q, r) { return hexToPixel(q, r, 1); }

  // The copy of (q, r) nearest (q0, r0) on the torus.
  _nearest(q, r, q0, r0) {
    let best = { q, r }, bd = Infinity;
    for (let i = -1; i <= 1; i++) {
      for (let j = -1; j <= 1; j++) {
        const d = hexDist(q0, r0, q + i * MAP_COLS, r + j * MAP_ROWS);
        if (d < bd) { bd = d; best = { q: q + i * MAP_COLS, r: r + j * MAP_ROWS }; }
      }
    }
    return best;
  }

  // World offset that puts this event's breakup point on the copy of the
  // board nearest the camera.
  _wrapOffset(ent, meRp) {
    const n = this._nearest(ent.bq, ent.br, meRp.q, meRp.r);
    return hexToPixel(n.q - ent.bq, n.r - ent.br, 1);
  }

  // Screen-space origin (px) for an event's world coordinates this frame.
  _frame(ent, cam, hs) {
    const w = this._wrapOffset(ent, cam.meRp);
    return { x: w.x * hs + cam.ox, y: w.y * hs + cam.oy };
  }

  // For loose particles, which don't remember their event: the copy nearest
  // the camera, judged in world units.
  _frameXY(x, y, cam, hs) {
    const c  = hexToPixel(cam.meRp.q, cam.meRp.r, 1);
    const pw = hexToPixel(MAP_COLS, 0, 1), ph = hexToPixel(0, MAP_ROWS, 1);
    let bx = x, by = y, bd = Infinity;
    for (let i = -1; i <= 1; i++) {
      for (let j = -1; j <= 1; j++) {
        const wx = x + pw.x * i + ph.x * j, wy = y + pw.y * i + ph.y * j;
        const d = (wx - c.x) * (wx - c.x) + (wy - c.y) * (wy - c.y);
        if (d < bd) { bd = d; bx = wx; by = wy; }
      }
    }
    return { x: bx * hs + cam.ox, y: by * hs + cam.oy };
  }
}
