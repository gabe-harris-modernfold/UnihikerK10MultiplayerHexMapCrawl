// ── Flare Field ──────────────────────────────────────────────────────────────
// The Bright Bad Idea (item 10) going up. Struck at the pawn, it climbs on a
// spitting red star, pops at the top, and then hangs there under a scrap of
// chute, sinking and swinging, for twenty-odd seconds: a hard red light on
// everything around it and a white smoke column standing up off it. Then it
// gutters, goes out, and the dead casing drops. The smoke hangs a while.
//
// The light is the point. The item pops the fog for 2 hexes (items.cfg
// reveal_fog 2), so the pool it throws is about that wide: renderer.js cuts
// it out of the night tint (drawDarkUnderLights, from this.lit) and the pool
// pass below stains what is under it red. Because the flare swings, the pool
// swings too -- the whole lit patch rocks back and forth over the ground.
//
// Everyone sees it. The server broadcasts ev "flare" (EVT_FLARE; the mock
// mirrors it), so every phone puts the same flare in its own sky, and one
// burning off the edge of the screen throws a faint red glow over the tiles
// on the side that faces it. The firer's own phone fires it from item_result
// (network.js _flareOnUse) and ignores the echo.
//
// Below ground there is no sky to fire it into, so it burns in the hand
// instead: the same light, held at the pawn, following them down the corridor.
//
// Built on debris-field.js's frame, deliberately: map space (hexToPixel at
// size 1), altitude drawn as a lift up the screen, every timeline a function
// of age so a throttled tab picks up where the flare really is, and the same
// lumpy smoke puffs (DebrisSprites). Three passes from renderer.js's LAYERS:
//   drawAir   -- smoke, the chute; under the night tint. Also fills this.lit.
//   drawLight -- the pool, the halo, the star, sparks, flashes; over the tint.
// Plain global class -- no ES module syntax.

const ITEM_BRIGHT_BAD_IDEA = 10;

const FLARE_LIFT      = 0.8;     // screen lift per unit of altitude (debris-field.js's DEBRIS_LIFT)
const FLARE_CLIMB_MS  = 1250;    // strike to pop
const FLARE_APEX      = 3.3;     // altitude of the pop -- low enough to stay on a phone screen
const FLARE_FLOOR     = 0.7;     // altitude it has sunk to when it goes out
const FLARE_BURN_MS   = 22000;   // pop to burnout
const FLARE_GUTTER_MS = 1900;    // the sputtering end of the burn
const FLARE_DROP_MS   = 650;     // the dead casing falling the rest of the way
const FLARE_HELD_MS   = 20000;   // in the hand, below ground
const FLARE_SPARK_CAP = 380;
// Strontium red, hottest first. A drip walks down it as it cools.
const FLARE_RAMP = [
  [255, 246, 238],   // 0 the white-hot heart
  [255, 198, 182],
  [255, 124,  98],
  [246,  60,  40],
  [208,  32,  22],
  [150,  16,  12],
  [ 92,  10,   8],   // 6 a dull-red speck about to go dark
];
// Flare smoke is white -- burnt magnesium -- not the near-black of a fire.
const FLARE_SMOKE_RGB = [176, 168, 164];

// ── Sprites ──────────────────────────────────────────────────────────────────
const FlareSprites = (() => {
  let bank = null;
  const canvas = (S) => {
    const c = document.createElement('canvas');
    c.width = c.height = S;
    return c;
  };

  // A spark: a white pinpoint so it reads at 2 px, its band colour round it.
  const ember = ([r, g, b]) => {
    const S = 24, cx = S / 2;
    const c = canvas(S), x = c.getContext('2d');
    const grad = x.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(255,255,255,1)');
    grad.addColorStop(0.22, `rgba(${r},${g},${b},0.95)`);
    grad.addColorStop(0.55, `rgba(${r},${g},${b},0.3)`);
    grad.addColorStop(1,    `rgba(${r},${g},${b},0)`);
    x.fillStyle = grad;
    x.fillRect(0, 0, S, S);
    return c;
  };

  // The halo round the star, and the flashes.
  const glow = () => {
    const S = 128, cx = S / 2;
    const c = canvas(S), x = c.getContext('2d');
    const grad = x.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(255,110,88,0.85)');
    grad.addColorStop(0.25, 'rgba(242,52,34,0.42)');
    grad.addColorStop(0.6,  'rgba(176,22,14,0.14)');
    grad.addColorStop(1,    'rgba(120,10,6,0)');
    x.fillStyle = grad;
    x.fillRect(0, 0, S, S);
    return c;
  };

  // The light on the ground: flat-topped, the falloff kept for the rim, so it
  // reads as a lit patch of country and not a hot spot.
  const pool = () => {
    const S = 128, cx = S / 2;
    const c = canvas(S), x = c.getContext('2d');
    const grad = x.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(255,72,50,0.6)');
    grad.addColorStop(0.45, 'rgba(236,44,30,0.42)');
    grad.addColorStop(0.8,  'rgba(170,22,14,0.12)');
    grad.addColorStop(1,    'rgba(120,10,6,0)');
    x.fillStyle = grad;
    x.fillRect(0, 0, S, S);
    return c;
  };

  return () => bank ?? (bank = { ember: FLARE_RAMP.map(ember), glow: glow(), pool: pool() });
})();

// Value noise, 0..1, smooth: the swing, the flicker, the sputter. Hashed, so
// none of it ever settles into a visible rhythm.
function _flareHash(i) {
  let h = Math.imul(i ^ 0x7f4a7c15, 0x85ebca6b);
  h ^= h >>> 13;
  h = Math.imul(h, 0xc2b2ae35);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}
function _flareNoise(x) {
  const i = Math.floor(x), f = x - i, u = f * f * (3 - 2 * f);
  return _flareHash(i) * (1 - u) + _flareHash(i + 1) * u;
}

class FlareField {
  constructor() {
    this.flares  = [];
    this.sparks  = [];
    this.flashes = [];
    this.wind    = { x: 0, y: 0 };   // integrated drift, world units
    this.last    = 0;
    this._seq    = 0;
    // This frame's light, screen px: { x, y, rad, level }. Filled by drawAir,
    // read by renderer.js's drawDarkUnderLights to cut the dark under it.
    this.lit     = [];
  }

  // ── Intake ────────────────────────────────────────────────────────────────
  // (q, r): the hex it is struck on. held: below ground -- it burns in pid's
  // hand instead of going up.
  fire(q, r, { pid = -1, held = false } = {}, now = Date.now()) {
    const seed = (Math.imul(++this._seq, 7919) + Math.imul(q, 131) + Math.imul(r, 17)) >>> 0;
    const P = hexToPixel(q, r, 1);
    // It climbs leaning downwind and then rides the wind down: the drift for
    // the whole burn is fixed now, so where it is stays a function of age.
    let wx = 0.6, wy = 0.2, ws = 0.4;
    if (typeof Wind !== 'undefined') { wx = Wind.x; wy = Wind.y; ws = Wind.s; }
    const lean = 0.2 + 0.3 * Math.min(1, ws);
    const ride = 0.6 + 1.6 * Math.min(1.2, ws);
    const f = {
      born: now, q, r, P, seed, pid, held,
      A:     { x: P.x + wx * lean, y: P.y + wy * lean },
      drift: { x: wx * ride, y: wy * ride },
      smoke: [], nextSmoke: 0, k: 0,
      struck: false, popped: false, dripAcc: 0, shedAcc: 0,
      until: now + (held ? FLARE_HELD_MS + 6000 : FLARE_CLIMB_MS + FLARE_BURN_MS + 30000),
    };
    this.flares.push(f);
  }

  get active() { return this.flares.length > 0 || this.sparks.length > 0; }

  // ── Where it is ───────────────────────────────────────────────────────────
  // { x, y, h, lv, st, tilt }: world position and altitude, light level 0..1
  // before flicker, stage (0 climbing, 1 burning, 2 the dead casing falling),
  // and the swing (for the chute). null once there is nothing left to draw.
  _head(f, age) {
    if (f.held) return this._heldHead(f, age);
    if (age < FLARE_CLIMB_MS) {
      const u = age / FLARE_CLIMB_MS, e = 1 - Math.pow(1 - u, 2.2);
      return { x: f.P.x + (f.A.x - f.P.x) * e, y: f.P.y + (f.A.y - f.P.y) * e,
               h: 0.45 + (FLARE_APEX - 0.45) * e, lv: 0.45, st: 0, tilt: 0 };
    }
    const tb = age - FLARE_CLIMB_MS;
    if (tb < FLARE_BURN_MS) {
      const t  = tb / FLARE_BURN_MS;
      // The chute rocking on its shrouds: two slow noises, not a pendulum,
      // and it only takes up the swing once the canopy has opened.
      const open = Math.min(1, tb / 1500);
      const sx = (_flareNoise(tb * 0.00045 + (f.seed & 1023)) - 0.5) * 0.9 * open;
      const sy = (_flareNoise(tb * 0.00033 + (f.seed & 1023) + 311) - 0.5) * 0.45 * open;
      let lv = Math.min(1, tb / 260);
      const tg = (tb - (FLARE_BURN_MS - FLARE_GUTTER_MS)) / FLARE_GUTTER_MS;
      if (tg > 0) {
        lv *= Math.pow(1 - tg, 0.6);
        if (_flareNoise(tb * 0.013 + (f.seed & 511)) < tg * 0.75) lv *= 0.18;   // sputters
      }
      return { x: f.A.x + f.drift.x * t + sx, y: f.A.y + f.drift.y * t + sy,
               h: FLARE_APEX - (FLARE_APEX - FLARE_FLOOR) * Math.pow(t, 1.2), lv, st: 1, tilt: sx };
    }
    const td = tb - FLARE_BURN_MS;
    if (td < FLARE_DROP_MS) {
      const u = td / FLARE_DROP_MS;
      return { x: f.A.x + f.drift.x, y: f.A.y + f.drift.y,
               h: FLARE_FLOOR * (1 - u * u), lv: 0, st: 2, tilt: 0 };
    }
    return null;
  }

  // In the hand: at the pawn, wherever they walk with it.
  _heldHead(f, age) {
    if (age >= FLARE_HELD_MS) return null;
    const rp = (typeof renderPos !== 'undefined' && f.pid >= 0) ? renderPos[f.pid] : null;
    const c  = rp ? hexToPixel(rp.q, rp.r, 1) : f.P;
    let lv = Math.min(1, age / 220);
    const tg = (age - (FLARE_HELD_MS - FLARE_GUTTER_MS)) / FLARE_GUTTER_MS;
    if (tg > 0) {
      lv *= Math.pow(1 - tg, 0.6);
      if (_flareNoise(age * 0.013 + (f.seed & 511)) < tg * 0.75) lv *= 0.18;
    }
    return { x: c.x + 0.22, y: c.y - 0.05, h: 0.5, lv, st: 1, tilt: 0 };
  }

  // Whether this flare belongs on the board being drawn. A sky flare is on
  // the surface; a held one is wherever its holder is.
  _visible(f) {
    if (!f.held) return !myDepth;
    if (f.pid === myId) return true;
    const rp = (typeof renderPos !== 'undefined') ? renderPos[f.pid] : null;
    return !!rp && (rp.board === undefined || rp.board === myDepth);
  }

  // A road flare's light is never steady: a fast fizz on a slower breath,
  // with the odd near-miss where it chokes for a moment.
  _flicker(f, now) {
    const s = f.seed & 1023;
    let fl = 0.8 + 0.2 * (0.6 * _flareNoise(now * 0.0105 + s) + 0.4 * _flareNoise(now * 0.043 + s * 3));
    if (_flareNoise(now * 0.0061 + s * 7) > 0.88) fl *= 0.72;
    return fl;
  }

  // ── Per frame ─────────────────────────────────────────────────────────────
  update(now) {
    const dt = this.last ? Math.min(100, Math.max(0, now - this.last)) : 16;
    this.last = now;
    if (!this.active && !this.flashes.length) return;

    // The air its smoke rides, as in debris-field.js: the one wind (wind.js).
    if (typeof Wind !== 'undefined') {
      const k = (0.000015 + Wind.s * 0.00006) * dt;
      this.wind.x += Wind.x * k;
      this.wind.y += Wind.y * k;
    }

    for (const f of this.flares) this._step(f, now, dt);
    this.flares = this.flares.filter(f => now < f.until);

    for (const p of this.sparks) {
      p.x += p.vx * dt; p.y += p.vy * dt; p.h += p.vh * dt;
      const drag = Math.max(0, 1 - p.drag * dt);
      p.vx *= drag; p.vy *= drag;
      p.vh = p.vh * drag - p.grav * dt;
      if (p.h < 0) { p.h = 0; p.vh = 0; }
    }
    this.sparks  = this.sparks.filter(p => now - p.born < p.life);
    this.flashes = this.flashes.filter(fl => now - fl.born < fl.life);
  }

  _step(f, now, dt) {
    const age = now - f.born;

    // The strike: a spray of sparks off the pawn's hand.
    if (!f.struck) {
      f.struck = true;
      const at = this._head(f, 0) ?? { x: f.P.x, y: f.P.y, h: 0.45 };
      this.flashes.push({ f, x: at.x, y: at.y, h: at.h, born: now, R: 0.9, life: 260, screen: 0 });
      for (let k = 0; k < 22; k++) {
        const a = Math.random() * Math.PI * 2, sp = 0.0008 + Math.random() * 0.0022;
        this._spark(at.x, at.y, at.h, Math.cos(a) * sp, Math.sin(a) * sp * 0.7,
          0.0015 + Math.random() * 0.003, now, 260 + Math.random() * 380, 0.05 + Math.random() * 0.05, 0, 0.002, 0.000012);
      }
    }

    // The pop at the top: a flash in the sky and a ring of sparks.
    if (!f.held && !f.popped && age >= FLARE_CLIMB_MS) {
      f.popped = true;
      const at = this._head(f, FLARE_CLIMB_MS);
      if (at) {
        this.flashes.push({ f, x: at.x, y: at.y, h: at.h, born: f.born + FLARE_CLIMB_MS, R: 2.4, life: 650, screen: 0.1 });
        for (let k = 0; k < 40; k++) {
          const a = Math.random() * Math.PI * 2, sp = 0.0015 + Math.random() * 0.005;
          this._spark(at.x, at.y, at.h, Math.cos(a) * sp, Math.sin(a) * sp * 0.8,
            (Math.random() - 0.4) * 0.004, now, 450 + Math.random() * 650, 0.06 + Math.random() * 0.08, 0, 0.0018, 0.000005);
        }
      }
    }

    const hd = this._head(f, age);
    if (hd) {
      if (hd.st === 0) {
        // Climbing: sparks thrown back down its line.
        f.shedAcc += dt * 0.09;
        while (f.shedAcc >= 1) {
          f.shedAcc -= 1;
          this._spark(hd.x, hd.y, hd.h, (Math.random() - 0.5) * 0.0012, (Math.random() - 0.5) * 0.0012,
            -0.0012 - Math.random() * 0.0015, now, 260 + Math.random() * 340, 0.045 + Math.random() * 0.05, 1, 0.0015, 0.000006);
        }
      } else if (hd.st === 1 && hd.lv > 0.25) {
        // Burning: molten drips falling off it and cooling as they go.
        f.dripAcc += dt * 0.05 * hd.lv;
        while (f.dripAcc >= 1) {
          f.dripAcc -= 1;
          this._spark(hd.x + (Math.random() - 0.5) * 0.08, hd.y + (Math.random() - 0.5) * 0.06, hd.h - 0.05,
            (Math.random() - 0.5) * 0.0005, (Math.random() - 0.5) * 0.0004, -0.0004 - Math.random() * 0.0008,
            now, 520 + Math.random() * 640, 0.04 + Math.random() * 0.045, 1 + ((Math.random() * 2) | 0), 0.0008, 0.0000045);
        }
      }
    }

    // Smoke, laid on a schedule by age so a dropped frame still leaves the
    // right puffs behind. The sky flare's positions are a function of age;
    // the held one's follow the pawn, so a long gap just skips ahead.
    const end = f.held ? FLARE_HELD_MS : FLARE_CLIMB_MS + FLARE_BURN_MS;
    if (f.held && age - f.nextSmoke > 1500) f.nextSmoke = age - 1500;
    while (f.nextSmoke <= Math.min(age, end)) {
      const t  = f.nextSmoke;
      const p  = f.held ? hd : this._head(f, t);
      const climb = !f.held && t < FLARE_CLIMB_MS;
      if (p && p.st !== 2) {
        f.smoke.push({
          x: p.x, y: p.y, h: p.h, k: f.k++, born: f.born + t, wx: this.wind.x, wy: this.wind.y,
          w0: f.held ? 0.05 : climb ? 0.08 : 0.09,
          w:  f.held ? 0.32 : climb ? 0.42 : 0.62,
          rise: f.held ? 0.9 : climb ? 0.2 : 1.1,
          life: f.held ? 5200 : climb ? 24000 : 22000,
        });
      }
      f.nextSmoke += f.held ? 140 : climb ? 22 : 115;
    }
    if (f.smoke.length && now - f.smoke[0].born > f.smoke[0].life) {
      f.smoke = f.smoke.filter(n => now - n.born <= n.life);
    }
  }

  _spark(x, y, h, vx, vy, vh, born, life, size, ramp, drag, grav) {
    if (this.sparks.length >= FLARE_SPARK_CAP) return;
    this.sparks.push({ x, y, h, vx, vy, vh, born, life, size, ramp, drag, grav });
  }

  // A smoke node, now: it rises off the heat, drifts on the wind (high air
  // faster), and spreads.
  _node(n, now) {
    const age = now - n.born;
    const h  = n.h + n.rise * (1 - Math.exp(-age / 7000));
    const hf = 0.55 + 0.45 * Math.min(1, h / FLARE_APEX);
    return {
      age, h,
      x: n.x + (this.wind.x - n.wx) * hf,
      y: n.y + (this.wind.y - n.wy) * hf,
      r: n.w0 + (n.w - n.w0) * (1 - Math.exp(-age / 9000)),
    };
  }

  // ── Drawing ───────────────────────────────────────────────────────────────
  drawAir(ctx, cam, hs, W, H) {
    this.lit.length = 0;
    if (!this.flares.length) return;
    const now = Date.now();
    const puffs = typeof DebrisSprites !== 'undefined';
    ctx.save();
    for (const f of this.flares) {
      if (!this._visible(f)) continue;
      const o  = this._frame(f, cam, hs);
      const hd = this._head(f, now - f.born);

      // The light this flare throws, for the dark to be cut under it.
      if (hd && hd.st === 1 && hd.lv > 0.02) {
        const R = f.held ? 2.1 : 3.3 + 0.28 * hd.h;
        this.lit.push({ x: hd.x * hs + o.x, y: hd.y * hs + o.y, rad: R * hs,
                        level: Math.min(1, hd.lv * this._flicker(f, now)) });
      }

      // Smoke. Wide old stretches draw every second or third node, as in
      // debris-field.js: they overlap several times over by then anyway.
      if (puffs) {
        for (const n of f.smoke) {
          const s0 = this._node(n, now);
          if (s0.age < 0) continue;
          const thin = s0.r > 0.45 ? 3 : s0.r > 0.25 ? 2 : 1;
          if (n.k % thin) continue;
          const X = s0.x * hs + o.x, Y = (s0.y - s0.h * FLARE_LIFT) * hs + o.y, s = s0.r * hs;
          if (X < -s || Y < -s || X > W + s || Y > H + s) continue;
          const t   = s0.age / n.life;
          const out = t < 0.5 ? 1 : Math.pow(1 - (t - 0.5) / 0.5, 1.5);
          const a   = Math.min(0.85, 0.66 * Math.min(1, s0.age / 200) * out * Math.pow(n.w0 / s0.r, 0.3) * (1 + 0.28 * (thin - 1)));
          if (a < 0.01) continue;
          const v = n.k % 3;
          const spr = (s0.age - 3000) / 16000 > _flareHash(n.k * 7 + 3)
            ? DebrisSprites.haze(FLARE_SMOKE_RGB, v) : DebrisSprites.puff(FLARE_SMOKE_RGB, v);
          ctx.globalAlpha = a;
          ctx.drawImage(spr, X - s, Y - s, s * 2, s * 2);
        }
      }

      // The chute: a scrap of canopy over the star, leaning against its swing.
      if (hd && !f.held && hd.st === 1) {
        const open = Math.min(1, (now - f.born - FLARE_CLIMB_MS) / 500);
        const X = hd.x * hs + o.x, Y = (hd.y - hd.h * FLARE_LIFT) * hs + o.y;
        const cw = 0.2 * hs * open, ch = 0.11 * hs * open;
        const cx = X - hd.tilt * 0.25 * hs, cy = Y - 0.42 * hs;
        ctx.globalAlpha = 0.85 * open;
        ctx.strokeStyle = 'rgb(74,64,60)';
        ctx.lineWidth = Math.max(0.6, hs * 0.012);
        ctx.beginPath();
        ctx.moveTo(cx - cw, cy); ctx.lineTo(X, Y);
        ctx.moveTo(cx, cy);      ctx.lineTo(X, Y);
        ctx.moveTo(cx + cw, cy); ctx.lineTo(X, Y);
        ctx.stroke();
        ctx.fillStyle = 'rgb(92,82,76)';
        ctx.beginPath();
        ctx.ellipse(cx, cy, cw, ch, -hd.tilt * 0.35, Math.PI, Math.PI * 2);
        ctx.fill();
      }
    }
    ctx.restore();
  }

  // dark: 0 in full day .. 1 at DARK WATCH. A flare at noon is a red star and
  // a pink cast; at night it is the only light there is.
  drawLight(ctx, cam, hs, W, H, dark = 0) {
    if (!this.active && !this.flashes.length) return;
    const bank = FlareSprites();
    const now = Date.now();
    ctx.save();

    for (const f of this.flares) {
      if (!this._visible(f)) continue;
      const hd = this._head(f, now - f.born);
      if (!hd) continue;
      const o  = this._frame(f, cam, hs);
      const fl = this._flicker(f, now);
      const lv = hd.lv * fl;
      const HX = hd.x * hs + o.x, HY = (hd.y - hd.h * FLARE_LIFT) * hs + o.y;
      const GX = hd.x * hs + o.x, GY = hd.y * hs + o.y;

      if (hd.st === 1 && lv > 0.01) {
        const R = (f.held ? 2.1 : 3.3 + 0.28 * hd.h) * hs;
        // Red light drains the other colours out of what it falls on: a
        // multiply that is red in the middle and white (no change) at the rim,
        // then the red added back on top.
        ctx.globalCompositeOperation = 'multiply';
        ctx.globalAlpha = Math.min(1, lv * (0.22 + 0.58 * dark));
        ctx.setTransform(ctx.getTransform().translate(GX, GY).scale(1, 0.75));
        const g = ctx.createRadialGradient(0, 0, 0, 0, 0, R);
        g.addColorStop(0,    'rgb(255,92,74)');
        g.addColorStop(0.55, 'rgb(255,140,120)');
        g.addColorStop(1,    'rgb(255,255,255)');
        ctx.fillStyle = g;
        ctx.fillRect(-R, -R, R * 2, R * 2);
        ctx.restore(); ctx.save();

        ctx.globalCompositeOperation = 'lighter';
        ctx.globalAlpha = lv * (0.28 + 0.34 * dark);
        ctx.drawImage(bank.pool, GX - R, GY - R * 0.75, R * 2, R * 1.5);

        // From further off it is a red glow on the country toward it: one
        // wide, faint wash from the flare's side, so the tiles on the side of
        // the screen that faces it pick up the light and the far side stays
        // dark -- which way to look, without an arrow. It takes over as the
        // pool leaves the view: anyone across the map, or the firer walking
        // away from it. A flare past the edge lights that edge as if it stood just beyond
        // it, so the direction reads from any range; how far it is only sets
        // how faint (down to half, across the whole map).
        const dx = GX - W / 2, dy = GY - H / 2;
        const dd = Math.hypot(dx, dy) || 1;
        const away = Math.max(0, Math.min(1, (Math.hypot(dx, dy / 0.75) - R * 0.5) / R));
        if (away > 0) {
          const half  = Math.hypot(W, H) / 2;
          const L     = Math.min(dd, half * 1.15);
          const ax    = W / 2 + dx / dd * L, ay = H / 2 + dy / dd * L;
          const Rf    = L + half * 0.6;
          const reach = Math.max(0.6, Math.min(1, 1.4 - dd / (hs * 1.732) / 20));
          const gf = ctx.createRadialGradient(ax, ay, 0, ax, ay, Rf);
          gf.addColorStop(0,    'rgba(255,64,44,1)');
          gf.addColorStop(0.35, 'rgba(244,46,30,0.55)');
          gf.addColorStop(0.7,  'rgba(196,28,18,0.15)');
          gf.addColorStop(1,    'rgba(150,18,10,0)');
          ctx.globalAlpha = Math.min(1, lv * away * reach * (0.2 + 0.32 * dark));
          ctx.fillStyle = gf;
          ctx.fillRect(0, 0, W, H);
        }

        // Its own smoke, lit from underneath where it leaves the flare.
        for (let i = f.smoke.length - 1; i >= 0; i--) {
          const n = f.smoke[i], s0 = this._node(n, now);
          if (s0.age > 3200) break;
          if (s0.age < 0 || (n.k & 1)) continue;
          const d = Math.hypot(s0.x - hd.x, s0.h - hd.h);
          if (d > 2.2) continue;
          const X = s0.x * hs + o.x, Y = (s0.y - s0.h * FLARE_LIFT) * hs + o.y, s = s0.r * 1.5 * hs;
          ctx.globalAlpha = 0.4 * lv * (1 - d / 2.2) * (1 - s0.age / 3200);
          ctx.drawImage(bank.glow, X - s, Y - s, s * 2, s * 2);
        }

        // The halo and the star.
        const halo = (f.held ? 1.0 : 1.5) * (0.85 + 0.3 * fl) * hs;
        ctx.globalAlpha = Math.min(1, 0.6 * lv);
        ctx.drawImage(bank.glow, HX - halo, HY - halo, halo * 2, halo * 2);
        const c1 = 0.3 * hs * (0.8 + 0.3 * fl), c0 = 0.14 * hs;
        ctx.globalAlpha = Math.min(1, 0.4 + 0.6 * hd.lv);
        ctx.drawImage(bank.ember[2], HX - c1, HY - c1, c1 * 2, c1 * 2);
        ctx.drawImage(bank.ember[0], HX - c0, HY - c0, c0 * 2, c0 * 2);

        // A red cast over the whole view while it is near enough to see.
        const near = Math.max(0, Math.min(1, 1 - (Math.hypot(GX - W / 2, GY - H / 2) - Math.max(W, H) * 0.45) / (Math.max(W, H) * 0.5)));
        if (near > 0) {
          ctx.globalAlpha = lv * near * (0.025 + 0.07 * dark);
          ctx.fillStyle = 'rgb(255,44,30)';
          ctx.fillRect(0, 0, W, H);
        }
      } else if (hd.st === 0) {
        // Climbing: a hot red star on a glowing thread of fresh smoke.
        ctx.globalCompositeOperation = 'lighter';
        for (let i = f.smoke.length - 1; i >= 0; i--) {
          const n = f.smoke[i], na = now - n.born;
          if (na > 450) break;
          if (na < 0) continue;
          const k = 1 - na / 450;
          const X = n.x * hs + o.x, Y = (n.y - n.h * FLARE_LIFT) * hs + o.y, s = (0.12 + 0.12 * (1 - k)) * hs;
          ctx.globalAlpha = k * k * 0.8;
          ctx.drawImage(bank.ember[3], X - s, Y - s, s * 2, s * 2);
        }
        const s = 0.55 * hs * fl;
        ctx.globalAlpha = 0.85;
        ctx.drawImage(bank.glow, HX - s, HY - s, s * 2, s * 2);
        const c = 0.16 * hs;
        ctx.globalAlpha = 1;
        ctx.drawImage(bank.ember[1], HX - c, HY - c, c * 2, c * 2);
      } else if (hd.st === 2) {
        // The dead casing: a fading red speck dropping out of the sky.
        const u = (now - f.born - FLARE_CLIMB_MS - FLARE_BURN_MS) / FLARE_DROP_MS;
        const c = 0.09 * hs;
        ctx.globalCompositeOperation = 'lighter';
        ctx.globalAlpha = Math.max(0, 0.8 * (1 - u));
        ctx.drawImage(bank.ember[5], HX - c, HY - c, c * 2, c * 2);
      }
    }

    // Flashes: the strike and the pop.
    ctx.globalCompositeOperation = 'lighter';
    for (const fx of this.flashes) {
      if (!this._visible(fx.f)) continue;
      const t = (now - fx.born) / fx.life;
      if (t < 0 || t >= 1) continue;
      const env = t < 0.08 ? t / 0.08 : Math.pow(1 - (t - 0.08) / 0.92, 2);
      const o = this._frame(fx.f, cam, hs);
      const X = fx.x * hs + o.x, Y = (fx.y - fx.h * FLARE_LIFT) * hs + o.y;
      const s = fx.R * (0.7 + 0.3 * t) * hs;
      ctx.globalAlpha = env;
      ctx.drawImage(bank.glow, X - s, Y - s, s * 2, s * 2);
      const c = s * 0.3;
      ctx.drawImage(bank.ember[0], X - c, Y - c, c * 2, c * 2);
      if (fx.screen > 0) {
        // Full strength overhead; from across the map, a third of it.
        const near = Math.max(0.33, Math.min(1, 1 - (Math.hypot(X - W / 2, Y - H / 2) - Math.max(W, H) * 0.5) / (Math.max(W, H) * 0.6)));
        ctx.globalAlpha = fx.screen * env * near * (0.5 + 0.5 * dark);
        ctx.fillStyle = 'rgb(255,120,96)';
        ctx.fillRect(0, 0, W, H);
      }
    }

    // Sparks and drips, walking down the red ramp as they cool.
    for (const p of this.sparks) {
      const t = (now - p.born) / p.life;
      if (t < 0 || t >= 1) continue;
      const P = this._frameXY(p.x, p.y - p.h * FLARE_LIFT, cam, hs);
      const idx = Math.min(FLARE_RAMP.length - 1, (p.ramp + t * 5) | 0);
      const s = p.size * (1 - 0.5 * t) * hs;
      if (P.x < -s || P.y < -s || P.x > W + s || P.y > H + s) continue;
      ctx.globalAlpha = 1 - t;
      ctx.drawImage(bank.ember[idx], P.x - s, P.y - s, s * 2, s * 2);
    }
    ctx.restore();
  }

  // ── Coordinates ───────────────────────────────────────────────────────────
  // As in debris-field.js. A sky flare is pinned to the copy of its hex
  // nearest the camera; a held one lives in renderPos coordinates, which are
  // already the camera's, so the nearest copy of its head stands in for all
  // of its smoke.
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

  _frame(f, cam, hs) {
    if (f.held) {
      const hd = this._heldHead(f, 0) ?? { x: f.P.x, y: f.P.y };
      const P = this._frameXY(hd.x, hd.y, cam, hs);
      return { x: P.x - hd.x * hs, y: P.y - hd.y * hs };
    }
    const n = this._nearest(f.q, f.r, cam.meRp.q, cam.meRp.r);
    const w = hexToPixel(n.q - f.q, n.r - f.r, 1);
    return { x: w.x * hs + cam.ox, y: w.y * hs + cam.oy };
  }

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
