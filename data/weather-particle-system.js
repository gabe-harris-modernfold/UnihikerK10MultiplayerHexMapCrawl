// ── Weather Particle System ───────────────────────────────────────────────────
// Follows ash-particle-system.js structure exactly.
// Plain global class — no ES module syntax.

const WEATHER_PARTICLE_HARD_CAP = 300;
// Fire gets its own budget rather than sharing the weather cap: a fire has to
// keep burning through a storm, and a storm has to keep raining over a fire.
// Sharing one pool meant whichever emitted first that frame starved the other.
const FIRE_PARTICLE_CAP = 480;

// ── Fire sprite bank ─────────────────────────────────────────────────────────
// Flames are drawn as pre-baked sprites, not per-particle canvas gradients:
// createRadialGradient() per particle per frame was the expensive part of the
// old renderer and it only ever bought a round blob. Baking instead lets each
// lick be a real tapered tongue and reduces drawing to a bare drawImage().
//
// FIRE_RAMP is a temperature ramp, hottest first. A lick walks *down* it as it
// ages — the thing an actual flame does, and the main reason the old
// one-colour-for-life licks read as orange confetti.
const FIRE_RAMP = [
  [255, 253, 240],  // 0 white-hot
  [255, 240, 180],
  [255, 212,  96],
  [255, 168,  44],
  [250, 116,  22],
  [224,  70,  14],
  [166,  34,   8],
  [ 88,  16,   6],  // 7 last dull-red gasp before it's gone
];

const FireSprites = (() => {
  let bank = null;

  const canvas = (size) => {
    const c = document.createElement('canvas');
    c.width = c.height = size;
    return c;
  };

  // A flame tongue: round at the base, drawn out to a point at the top, fill
  // hottest at the base. Blurred so the edge reads as gas, not as a decal.
  const flameSprite = ([r, g, b]) => {
    const S = 64, cx = S / 2, bulbY = 44, bulbR = 17, tipY = 3;
    const c = canvas(S), x = c.getContext('2d');
    // Core tops out well under 1: these stack additively, and a fully opaque
    // core meant a dozen overlapping licks clipped to a flat white disc
    // instead of grading white → yellow → orange out from the middle.
    const grad = x.createRadialGradient(cx, bulbY - 6, 0, cx, bulbY - 6, 30);
    grad.addColorStop(0,    `rgba(${r},${g},${b},0.82)`);
    grad.addColorStop(0.35, `rgba(${r},${g},${b},0.62)`);
    grad.addColorStop(0.75, `rgba(${r},${g},${b},0.26)`);
    grad.addColorStop(1,    `rgba(${r},${g},${b},0)`);
    x.filter    = 'blur(3px)'; // no-op where unsupported — sprite just reads crisper
    x.fillStyle = grad;
    x.beginPath();
    x.moveTo(cx - bulbR, bulbY);
    x.bezierCurveTo(cx - bulbR, bulbY - 26, cx - 6, tipY + 11, cx, tipY);
    x.bezierCurveTo(cx + 6, tipY + 11, cx + bulbR, bulbY - 26, cx + bulbR, bulbY);
    x.arc(cx, bulbY, bulbR, 0, Math.PI);
    x.closePath();
    x.fill();
    return c;
  };

  // An airborne spark: white pinpoint core so it stays legible at 2 px, with
  // the band colour bleeding out around it.
  const emberSprite = ([r, g, b]) => {
    const S = 24, cx = S / 2;
    const c = canvas(S), x = c.getContext('2d');
    const grad = x.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(255,255,255,1)');
    grad.addColorStop(0.22, `rgba(${r},${g},${b},0.95)`);
    grad.addColorStop(0.55, `rgba(${r},${g},${b},0.32)`);
    grad.addColorStop(1,    `rgba(${r},${g},${b},0)`);
    x.fillStyle = grad;
    x.fillRect(0, 0, S, S);
    return c;
  };

  // Near-black and dense through most of its radius, not a pale grey wisp:
  // puffs overlap heavily inside a plume, so each one has to hold its value or
  // the stack still reads as haze. Bigger sprite than the flames' so it stays
  // smooth when a puff expands to most of a hex.
  const smokeSprite = () => {
    const S = 96, cx = S / 2;
    const c = canvas(S), x = c.getContext('2d');
    const grad = x.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(14,12,11,0.98)');
    grad.addColorStop(0.42, 'rgba(19,16,15,0.74)');
    grad.addColorStop(0.75, 'rgba(24,21,20,0.30)');
    grad.addColorStop(1,    'rgba(26,23,22,0)');
    x.filter    = 'blur(5px)';
    x.fillStyle = grad;
    x.beginPath();
    x.arc(cx, cx, cx * 0.86, 0, Math.PI * 2);
    x.fill();
    return c;
  };

  // The pool of firelight on the ground under the licks. Drawn once per
  // burning hex per frame, additively, with a flicker — this is what actually
  // says "this hex is on fire" at a glance; the licks are detail on top of it.
  const glowSprite = () => {
    const S = 128, cx = S / 2;
    const c = canvas(S), x = c.getContext('2d');
    const grad = x.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0,    'rgba(255,180,80,0.85)');
    grad.addColorStop(0.28, 'rgba(255,124,36,0.42)');
    grad.addColorStop(0.62, 'rgba(196,58,14,0.15)');
    grad.addColorStop(1,    'rgba(150,30,8,0)');
    x.fillStyle = grad;
    x.fillRect(0, 0, S, S);
    return c;
  };

  // Built on first use rather than at load, so the file stays importable
  // before there is a document to make canvases from.
  return () => bank ?? (bank = {
    flame: FIRE_RAMP.map(flameSprite),
    ember: FIRE_RAMP.map(emberSprite),
    smoke: smokeSprite(),
    glow:  glowSprite(),
  });
})();

// Stable per-hex phase so neighbouring fires never flicker in lockstep — that
// synchrony is the giveaway that it's one global sine and not many fires.
// Shared with renderer.js's ember-bed fill so the hex and its licks pulse
// together (cross-file global; ignore S3800).
function fireHexPhase(q, r) {
  const h = (Math.imul(q | 0, 73856093) ^ Math.imul(r | 0, 19349663)) >>> 0;
  return (h % 6283) / 1000;
}

// ── Fire seats ───────────────────────────────────────────────────────────────
// A burning hex gets several separate seats of fire, not one plume in the
// middle. The map is a top-down view: a hex on fire is an *area* alight, and
// from above that's a scatter of burning spots across the ground — one central
// campfire per tile is the wrong read entirely.
const FIRE_SEAT_COUNT = [0, 2, 3, 5];  // by intensity: smoulder → burning → inferno
const _seatCache = new Map();

function fireSeats(q, r, intensity) {
  const key = `${q}_${r}_${intensity}`;
  const hit = _seatCache.get(key);
  if (hit) return hit;
  // Deterministic per hex, so the spots stay put frame to frame and the glow
  // pass lands on exactly the same spots as the particle pass. Anything
  // random-per-frame here makes the fire crawl around its own tile.
  let s = (Math.imul(q | 0, 73856093) ^ Math.imul(r | 0, 19349663) ^ Math.imul(intensity, 83492791)) >>> 0;
  const rnd = () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296);
  const n = FIRE_SEAT_COUNT[intensity] ?? 3;
  const seats = [];
  for (let k = 0; k < n; k++) {
    // Golden-angle spiral plus jitter: spots land spread over the hex rather
    // than clumping, without a rejection loop. Offsets are in units of the
    // hex's spread, so they hold up at any zoom.
    const ang = k * 2.399963 + rnd() * 0.9;
    const rad = 0.13 + 0.42 * Math.sqrt((k + rnd()) / n);
    seats.push({
      dx: Math.cos(ang) * rad,
      dy: Math.sin(ang) * rad * 0.62,   // squashed: a flat-top hex is wider than tall
      scale: 0.62 + rnd() * 0.55,       // spots differ in size...
      phase: rnd() * Math.PI * 2,       // ...and never flicker in step
    });
  }
  // Bounded: keys are (hex, intensity) and a fire both spreads and changes
  // intensity, so without this the map grows all session for no benefit.
  if (_seatCache.size > 2048) _seatCache.clear();
  _seatCache.set(key, seats);
  return seats;
}

class WeatherParticleSystem {
  constructor() {
    this.particles = [];
    this._nextId = 0;
    this._fireCount = 0;
    this._t = 0; // frame counter; drives all fire flicker/wander
  }

  // Weather's share of the pool, excluding fire (see FIRE_PARTICLE_CAP).
  get _weatherFull() {
    return this.particles.length - this._fireCount >= WEATHER_PARTICLE_HARD_CAP;
  }

  // Emit count new particles for the given weather phase.
  // w, h = canvas pixel dimensions for spawn positioning.
  // anchors = [{x, y, spread}] storm-hex centers to spawn over — with none
  // given, nothing emits (no active storm clump on screen means no weather
  // fx), keeping rain/radioactive pulses confined to the clumps the storm
  // field currently covers instead of the whole canvas.
  emit(count, phase, w, h, anchors) {
    const confined = phase === 1 || phase === 2 || phase === 3;
    if (confined && !anchors?.length) return;
    for (let i = 0; i < count; i++) {
      if (this._weatherFull) break;
      const anchor = confined ? anchors[(Math.random() * anchors.length) | 0] : null;
      this.particles.push(this._spawn(phase, w, h, anchor));
    }
  }

  // Rolls a chance to drop one soft ground-fog puff onto a random storm hex.
  // Called once per frame with a small probability rather than a fixed count —
  // fog puffs live a lot longer than a raindrop, so a per-frame count would
  // pile up fast; a low chance keeps the effect "a little fog", not a haze.
  emitFog(chance, anchors) {
    if (!anchors?.length) return;
    if (this._weatherFull) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnFog(anchor));
  }

  // Rolls a chance to drop one heavy creeping-fog puff onto a random fog-
  // covered hex — bigger, darker and longer-lived than the light ambient fog
  // rain/storm get (emitFog), so it reads as a blanket rolling in rather
  // than a faint accent.
  emitCreepingFog(chance, anchors) {
    if (!anchors?.length) return;
    if (this._weatherFull) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnCreepingFog(anchor));
  }

  // Rolls a chance to flash one faint burst of static-like energy somewhere
  // in the creeping fog — a quick, small flicker, unlike chem's sustained
  // radioactive pulses or the big hex-to-hex lightning arc.
  emitStatic(chance, anchors) {
    if (!anchors?.length) return;
    if (this._weatherFull) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnStatic(anchor));
  }

  // Rolls a chance to kick up one dust mote on a random shaking quake hex.
  emitDust(chance, anchors) {
    if (!anchors?.length) return;
    if (this._weatherFull) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnDust(anchor));
  }

  // ── Fire ───────────────────────────────────────────────────────────────
  // anchors are {x, y, spread, intensity, q, r} from fire-field.js via
  // renderer.js's terrain pass, one per burning hex (not a single random pick
  // like the weather emitters above): a fire has to visibly keep burning on
  // every hex it occupies, not flicker on one at a time.
  //
  // Three emitters, because a fire isn't one kind of thing:
  //   licks  — the body of the flame; several per frame per seat so each spot
  //            burns continuously instead of twinkling
  //   embers — sparks that break free and loft, with a motion trail
  //   smoke  — intensity 2+ only; dark, non-additive, and the reason the
  //            licks under it read as *bright* rather than merely orange
  //
  // Everything is emitted per seat (see fireSeats), so a hex reads as an area
  // alight seen from above rather than one plume at the tile's centre.
  //
  // Each hex is capped independently via _pushFire instead of break-ing out at
  // a shared cap: that break meant that with many hexes burning, whichever
  // came first in the list ate the budget and the rest of the fire silently
  // went out on screen.
  emitFire(anchors) {
    if (!anchors?.length) return;
    // Split the budget across however many hexes are burning, so a big blaze
    // thins every fire evenly instead of the first few hexes eating the cap
    // and the rest going dark. The floor keeps a hex readable as on fire even
    // in a firestorm; it is set low enough that ~24 simultaneous hexes still
    // fit under the cap, since past that point _pushFire starts dropping.
    const share = Math.max(0.20, Math.min(1, FIRE_PARTICLE_CAP / (anchors.length * 78)));
    // Rotate which hex is served first each frame. Beyond the share floor the
    // cap does bind, and _pushFire drops silently — without rotating, the same
    // tail of the list would be starved every frame and those hexes would sit
    // there visibly unlit while the head of the list burned normally.
    for (let n = 0; n < anchors.length; n++) {
      const anchor = anchors[(n + this._t) % anchors.length];
      const i = anchor.intensity;
      const seats = fireSeats(anchor.q ?? anchor.x, anchor.r ?? anchor.y, i);
      for (const seat of seats) {
        // Deliberately sparse per spot: enough overlap for 'lighter' to build
        // a core, but few enough that individual tongues still read. Pile on
        // more and each spot blends into a flat disc of light.
        for (let budget = (0.20 + i * 0.085) * seat.scale * share; budget > 0; budget -= 1) {
          if (Math.random() < Math.min(1, budget)) this._pushFire(this._spawnFlame(anchor, seat));
        }
      }
      // Embers pick one seat at a time — they're an accent over the whole
      // burning tile, not a per-spot fixture, and they live ~3x longer than a
      // lick, so a per-seat rate here turns into a swarm at steady state.
      if (Math.random() < (0.03 + 0.05 * i) * share) {
        this._pushFire(this._spawnEmber(anchor, seats[(Math.random() * seats.length) | 0]));
      }
      // Smoke burns at every intensity, not just 2+: a smoulder throws more of
      // it than a clean inferno does. Emitted as a budget loop rather than one
      // coin flip so a plume actually has body — a single puff per frame never
      // stacks into a column no matter how long it lives.
      for (let budget = (0.03 + 0.03 * i) * share; budget > 0; budget -= 1) {
        if (Math.random() < Math.min(1, budget)) {
          this._pushFire(this._spawnSmoke(anchor, seats[(Math.random() * seats.length) | 0]));
        }
      }
    }
  }

  // Ground-level firelight, drawn straight to the canvas (not a particle)
  // every frame: one faint wash over the whole tile so it reads as burning
  // ground at a glance, plus a tight pool at each seat so the hot spots show
  // through from above. Each pool flickers on its own sum of three
  // incommensurate sines — one clean period, or one phase shared across the
  // tile, immediately reads as an animated overlay rather than as fire.
  // Call before update()/render() so the licks sit on top of it.
  renderFireGlow(ctx, anchors) {
    if (!anchors?.length) return;
    const spr = FireSprites().glow;
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    for (const anchor of anchors) {
      const i  = anchor.intensity;
      const ph = fireHexPhase(anchor.q ?? anchor.x, anchor.r ?? anchor.y);
      // Tile-wide wash, breathing slowly.
      const wash = 0.78 + 0.16 * Math.sin(this._t * 0.055 + ph);
      const wrad = anchor.spread * (0.85 + i * 0.16);
      ctx.globalAlpha = Math.max(0, (0.05 + i * 0.030) * wash);
      ctx.drawImage(spr, anchor.x - wrad, anchor.y - wrad, wrad * 2, wrad * 2);

      for (const seat of fireSeats(anchor.q ?? anchor.x, anchor.r ?? anchor.y, i)) {
        const f = 0.70
          + 0.18 * Math.sin(this._t * 0.085 + seat.phase)
          + 0.09 * Math.sin(this._t * 0.213 + seat.phase * 1.7)
          + 0.05 * Math.sin(this._t * 0.491 + seat.phase * 2.9);
        const sx  = anchor.x + seat.dx * anchor.spread;
        const sy  = anchor.y + seat.dy * anchor.spread + anchor.spread * 0.08;
        const rad = anchor.spread * 0.42 * seat.scale * (0.92 + f * 0.16);
        ctx.globalAlpha = Math.max(0, (0.10 + i * 0.045) * f);
        ctx.drawImage(spr, sx - rad, sy - rad, rad * 2, rad * 2);
        // Tight white-hot core in the spot, only once it is really burning.
        if (i >= 2) {
          const cr = rad * 0.36;
          ctx.globalAlpha = Math.max(0, 0.07 * i * f);
          ctx.drawImage(spr, sx - cr, sy - cr, cr * 2, cr * 2);
        }
      }
    }
    ctx.restore();
  }

  _pushFire(p) {
    if (this._fireCount >= FIRE_PARTICLE_CAP) return;
    this._fireCount++;
    this.particles.push(p);
  }

  // Advance all particles one frame and cull dead ones.
  update() {
    this._t++;
    for (const p of this.particles) {
      if (p.fx) { this._stepFire(p); continue; }

      p.x += p.dx;
      p.y += p.dy;
      p.age++;
      let opacity = p.age < p.fade ? (p.age / p.fade) * p.maxOpacity : p.maxOpacity;

      if (p.maxY != null) {
        // Confined rain: dies at the hex's bottom edge, not on a fixed timer,
        // so the drop never streaks past the hex it's raining on.
        const remain    = p.maxY - p.y;
        const boundFade = p.fade * 4;
        if (remain < boundFade) opacity = Math.min(opacity, Math.max(0, remain / boundFade) * p.maxOpacity);
        p.dead = remain <= 0 || p.age >= p.ttl;
      } else {
        if (p.age > p.ttl - p.fade) opacity = Math.min(opacity, ((p.ttl - p.age) / p.fade) * p.maxOpacity);
        p.dead = p.age >= p.ttl;
      }

      p.opacity = opacity;
    }

    let fire = 0;
    this.particles = this.particles.filter(p => {
      if (p.dead) return false;
      if (p.fx) fire++;
      return true;
    });
    this._fireCount = fire;
  }

  // Fire integration, separate from the generic path above because none of
  // fire's motion is linear: licks accelerate upward while hot and stall as
  // they cool, embers arc under gravity, smoke expands as it climbs.
  _stepFire(p) {
    p.age++;
    const t = p.age / p.ttl;
    if (t >= 1) { p.dead = true; return; }
    p.prevX = p.x; p.prevY = p.y;

    if (p.fx === 'flame') {
      // Buoyancy while hot, then drag takes over: a lick shoots up out of the
      // bed and slows as it cools, instead of tracking at one fixed speed.
      // Drag is heavy on purpose — with light drag the licks reach terminal
      // velocity and keep going, and a fire turns into a row of rockets.
      p.vy -= p.buoy * (1 - t);
      p.vy *= 0.94;
      p.axis += p.drift;
      p.sway += p.swayRate;
      // Lateral wander widens with height — the plume is narrow at the bed
      // and frays at the top, which is most of what reads as turbulence.
      const lateral = Math.sin(p.sway) * p.swayAmp * (0.25 + t * 1.15);
      p.x = p.axis + lateral;
      p.y += p.vy;
      p.lean = Math.max(-0.5, Math.min(0.5, lateral * 0.035));
      // Swell fast off the bed, then taper to a point over the rest of life.
      p.scale = t < 0.25
        ? 0.42 + (t / 0.25) * 0.58
        : Math.pow(1 - (t - 0.25) / 0.75, 0.7) * 0.98 + 0.02;
      p.band = Math.min(FIRE_RAMP.length - 1, (p.heat + t * p.cool) | 0);
      p.opacity = p.maxOpacity * (t < 0.3 ? t / 0.3 : Math.pow(1 - (t - 0.3) / 0.7, 1.15));
      return;
    }

    if (p.fx === 'ember') {
      p.vy += 0.014;        // loses lift and starts to fall back
      p.vy *= 0.996;
      p.x += p.vx + Math.sin(p.age * 0.09 + p.phase) * 0.4;
      p.y += p.vy;
      p.band = Math.min(FIRE_RAMP.length - 1, (p.heat + t * p.cool) | 0);
      // Sparks twinkle as they tumble; a steady dot reads as a UI pip.
      const twinkle = 0.55 + 0.45 * Math.sin(p.age * 0.42 + p.phase);
      p.opacity = p.maxOpacity * twinkle * (t < 0.1 ? t / 0.1 : Math.pow(1 - (t - 0.1) / 0.9, 0.9));
      return;
    }

    // smoke
    p.vy *= 0.992;
    p.vx += p.shear;
    p.x += p.vx;
    p.y += p.vy;
    p.scale *= 1.004;       // a plume opens out as it climbs, but slowly
    p.lean += p.spin;
    // Linear falloff after a quick fade-in, so a puff holds most of its value
    // through mid-life. Any easing above linear here dumps the alpha early and
    // the plume reads as thin haze however many puffs are in it.
    p.opacity = p.maxOpacity * (t < 0.12 ? t / 0.12 : 1 - (t - 0.12) / 0.88);
  }

  render(ctx) {
    // Smoke first, as its own pass: it has to sit *behind* the flames, or a
    // thick plume dulls the fire throwing it. Spawn order alone would put
    // later puffs on top of earlier licks.
    for (const p of this.particles) {
      if (p.fx === 'smoke' && p.opacity > 0.01) this._renderFire(ctx, p);
    }
    for (const p of this.particles) {
      if (p.opacity <= 0.01 || p.fx === 'smoke') continue;
      if (p.fx) { this._renderFire(ctx, p); continue; }
      ctx.save();
      ctx.globalAlpha = p.opacity;
      if (p.shape === 'line') {
        ctx.strokeStyle = p.color;
        ctx.lineWidth   = 1;
        ctx.beginPath();
        ctx.moveTo(p.x, p.y);
        ctx.lineTo(p.x + p.dx * 8, p.y + p.dy * 8);
        ctx.stroke();
      } else if (p.shape === 'fog') {
        const g = ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, p.size);
        g.addColorStop(0, p.color);
        g.addColorStop(1, 'rgba(210,215,225,0)');
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
        ctx.fill();
      } else if (p.shape === 'spark') {
        // Tiny jagged asterisk that flashes in place for a handful of
        // frames — the fog's "little bursts of faint energy", not a bolt.
        ctx.strokeStyle = p.color;
        ctx.lineWidth   = 1;
        for (let i = 0; i < p.rays; i++) {
          const a = p.ang + (i / p.rays) * Math.PI * 2;
          ctx.beginPath();
          ctx.moveTo(p.x, p.y);
          ctx.lineTo(p.x + Math.cos(a) * p.size, p.y + Math.sin(a) * p.size);
          ctx.stroke();
        }
      } else if (p.shape === 'dust') {
        // Soft puff, not a hard-edged dot — a dense-ish core fading smoothly
        // to nothing, like a real cloud of kicked-up dirt.
        const g = ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, p.size);
        g.addColorStop(0,   `rgba(${p.color},0.9)`);
        g.addColorStop(0.4, `rgba(${p.color},0.55)`);
        g.addColorStop(1,   `rgba(${p.color},0)`);
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
        ctx.fill();
      } else {
        ctx.fillStyle = p.color;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.restore();
    }
  }

  // Flames and embers composite with 'lighter' because fire is emissive:
  // overlapping licks have to *sum* toward white, which is what gives a plume
  // a hot core and soft edges. Under the old source-over they just stacked
  // muddy orange on muddy orange. Smoke stays source-over — it occludes.
  _renderFire(ctx, p) {
    const bank = FireSprites();
    ctx.save();
    ctx.globalAlpha = p.opacity;
    ctx.translate(p.x, p.y);

    if (p.fx === 'smoke') {
      const s = p.size * p.scale;
      ctx.rotate(p.lean);
      ctx.drawImage(bank.smoke, -s, -s, s * 2, s * 2);
      ctx.restore();
      return;
    }

    ctx.globalCompositeOperation = 'lighter';

    if (p.fx === 'ember') {
      // Short streak behind the spark so it reads as moving, rather than as a
      // dot teleporting up the screen one frame at a time.
      const [r, g, b] = FIRE_RAMP[p.band];
      ctx.strokeStyle = `rgb(${r},${g},${b})`;
      ctx.lineWidth   = Math.max(0.6, p.size * 0.5);
      ctx.beginPath();
      ctx.moveTo(p.prevX - p.x, p.prevY - p.y);
      ctx.lineTo(0, 0);
      ctx.stroke();
      const s = p.size * 2.6;
      ctx.drawImage(bank.ember[p.band], -s, -s, s * 2, s * 2);
      ctx.restore();
      return;
    }

    // flame
    const w = p.size * p.scale;
    const h = w * p.stretch;
    ctx.rotate(p.lean);
    ctx.drawImage(bank.flame[p.band], -w, -h, w * 2, h * 2);
    ctx.restore();
  }

  _spawn(phase, w, h, anchor) {
    if (phase === 1) {
      // Light Rain: gentle diagonal lines, confined to just above/through
      // the storm hex they spawn over — never streaking past it downscreen.
      const x0 = anchor ? anchor.x + (Math.random() - 0.5) * anchor.spread : Math.random() * w;
      const y0 = anchor ? anchor.y - anchor.spread * 0.8 : -10;
      return {
        id: this._nextId++, shape: 'line',
        x: x0, y: y0,
        dx: -0.3, dy: 3.5 + Math.random() * 1.5,
        color: 'rgba(120,150,200,0.6)',
        size: 1,
        maxOpacity: 0.5 + Math.random() * 0.3,
        opacity: 0, age: 0, dead: false,
        maxY: anchor ? anchor.y + anchor.spread * 0.75 : null,
        ttl: anchor ? 90 : 120, fade: anchor ? 8 : 10,
      };
    }
    if (phase === 2) {
      // Storm: heavier angled rain, same hex-confined fall.
      const x0 = anchor ? anchor.x + (Math.random() - 0.5) * anchor.spread * 1.3 : Math.random() * (w + 100) - 50;
      const y0 = anchor ? anchor.y - anchor.spread * 0.8 : -10;
      return {
        id: this._nextId++, shape: 'line',
        x: x0, y: y0,
        dx: -1.2 - Math.random() * 0.8, dy: 6 + Math.random() * 3,
        color: 'rgba(80,110,160,0.7)',
        size: 1,
        maxOpacity: 0.6 + Math.random() * 0.3,
        opacity: 0, age: 0, dead: false,
        maxY: anchor ? anchor.y + anchor.spread * 0.8 : null,
        ttl: anchor ? 90 : 120, fade: anchor ? 8 : 10,
      };
    }
    // phase === 3: Chem — small radioactive pulses that flicker and jitter
    // in place within their hex (no consistent drift), like a Geiger blip,
    // instead of drifting away across the screen.
    const gx0 = anchor ? anchor.x + (Math.random() - 0.5) * anchor.spread * 1.1 : Math.random() * w;
    const gy0 = anchor ? anchor.y + (Math.random() - 0.5) * anchor.spread * 1.1 : h + 5;
    return {
      id: this._nextId++, shape: 'circle',
      x: gx0, y: gy0,
      dx: (Math.random() - 0.5) * 0.5,
      dy: (Math.random() - 0.5) * 0.5,
      color: `rgba(${60 + Math.trunc(Math.random() * 50)},${215 + Math.trunc(Math.random() * 40)},${70 + Math.trunc(Math.random() * 60)},0.85)`,
      size: 1.2 + Math.random() * 1.6,
      maxOpacity: 0.5 + Math.random() * 0.35,
      opacity: 0, age: 0, dead: false,
      maxY: null,
      ttl: anchor ? 35 + Math.random() * 30 : 120, fade: anchor ? 6 : 10,
    };
  }

  // Soft ground-fog puff drifting slowly around a storm hex — stays put and
  // fades over a few seconds instead of falling like rain (no maxY).
  _spawnFog(anchor) {
    const ang = Math.random() * Math.PI * 2;
    const rad = Math.random() * anchor.spread * 0.4;
    return {
      id: this._nextId++, shape: 'fog',
      x: anchor.x + Math.cos(ang) * rad,
      y: anchor.y + Math.sin(ang) * rad * 0.6,
      dx: (Math.random() - 0.5) * 0.12,
      dy: -0.04 - Math.random() * 0.06,
      color: 'rgba(230,233,238,0.75)',
      size: anchor.spread * (0.5 + Math.random() * 0.3),
      maxOpacity: 0.16 + Math.random() * 0.1,
      opacity: 0, age: 0, dead: false,
      maxY: null,
      ttl: 160 + Math.random() * 90, fade: 45,
    };
  }

  // Heavy creeping-fog puff — blackish with a faint greenish cast, bigger
  // and much longer-lived than the light ground-fog rain/storm spawn via
  // _spawnFog, and drifting slower still, so puffs pile up into a blanket
  // that lingers over the hex instead of a wisp that passes through.
  _spawnCreepingFog(anchor) {
    const ang = Math.random() * Math.PI * 2;
    const rad = Math.random() * anchor.spread * 0.5;
    return {
      id: this._nextId++, shape: 'fog',
      x: anchor.x + Math.cos(ang) * rad,
      y: anchor.y + Math.sin(ang) * rad * 0.6,
      dx: (Math.random() - 0.5) * 0.08,
      dy: -0.02 - Math.random() * 0.05,
      // "rgba(...)" directly (unlike _spawnDust's bare "R,G,B") since this
      // feeds the 'fog' shape's gradient, which wants a full color stop —
      // near-black with G nudged above R/B for a subtle green cast.
      color: `rgba(${3 + Math.trunc(Math.random() * 6)},${14 + Math.trunc(Math.random() * 10)},${8 + Math.trunc(Math.random() * 6)},0.95)`,
      size: anchor.spread * (0.85 + Math.random() * 0.45),
      maxOpacity: 0.4 + Math.random() * 0.2,
      opacity: 0, age: 0, dead: false,
      maxY: null,
      ttl: 220 + Math.random() * 120, fade: 60,
    };
  }

  // Faint, quick flicker of "static electricity" in the fog — a tiny burst
  // of 3-5 rays that flashes for well under a second and vanishes, held in
  // place (no dx/dy) rather than drifting like every other particle here.
  _spawnStatic(anchor) {
    const ang = Math.random() * Math.PI * 2;
    const rad = Math.random() * anchor.spread * 0.7;
    return {
      id: this._nextId++, shape: 'spark',
      x: anchor.x + Math.cos(ang) * rad,
      y: anchor.y + Math.sin(ang) * rad * 0.6,
      dx: 0, dy: 0,
      ang: Math.random() * Math.PI * 2,
      rays: 3 + ((Math.random() * 3) | 0),
      color: `rgba(${110 + Math.trunc(Math.random() * 40)},${225 + Math.trunc(Math.random() * 30)},${140 + Math.trunc(Math.random() * 40)},0.8)`,
      size: 2 + Math.random() * 3,
      maxOpacity: 0.2 + Math.random() * 0.18,
      opacity: 0, age: 0, dead: false,
      maxY: null,
      ttl: 10 + Math.random() * 8, fade: 4,
    };
  }

  // Debris kicked up off a shaking quake hex — drifts outward/upward slowly
  // and hangs, then dissipates over a couple seconds (long ttl + long fade),
  // outlasting the shake itself rather than popping instantly (no maxY: it
  // never falls back, just fades out like settling dust).
  _spawnDust(anchor) {
    const ang   = Math.random() * Math.PI * 2;
    const speed = 0.15 + Math.random() * 0.35;
    return {
      id: this._nextId++, shape: 'dust',
      x: anchor.x + (Math.random() - 0.5) * anchor.spread * 0.8,
      y: anchor.y + (Math.random() - 0.5) * anchor.spread * 0.5,
      dx: Math.cos(ang) * speed,
      dy: Math.sin(ang) * speed - 0.25,
      // "R,G,B" only (no alpha) — render() builds a soft radial gradient from
      // it so the puff fades out at its own edge instead of a hard circle.
      // Grayish grit, not warm brown dirt — channels stay close together.
      color: `${120 + Math.trunc(Math.random() * 30)},${116 + Math.trunc(Math.random() * 30)},${110 + Math.trunc(Math.random() * 28)}`,
      size: 4 + Math.random() * 6,
      maxOpacity: 0.5 + Math.random() * 0.3,
      opacity: 0, age: 0, dead: false,
      maxY: null,
      ttl: 90 + Math.random() * 70, fade: 30,
    };
  }

  // A single flame tongue, rooted at one seat of the hex's fire. Within the
  // seat it still wanders on two incommensurate sines, so the spot breathes in
  // and out instead of being a fixed jet pinned to one pixel.
  // `heat`/`cool` are its start and end positions on FIRE_RAMP — an inferno
  // lick starts white-hot and has further to fall than a smoulder that starts
  // out orange already.
  _spawnFlame(anchor, seat) {
    const i    = anchor.intensity;
    const wob  = Math.sin(this._t * 0.013 + seat.phase) * 0.10
               + Math.sin(this._t * 0.031 + seat.phase * 2.1) * 0.05;
    // Squaring biases toward the seat: a tight body with a few stragglers,
    // rather than a uniform scatter around it.
    const off  = (Math.random() - 0.5) * Math.abs(Math.random() - 0.5) * 2;
    const axis = anchor.x + (seat.dx + wob + off * 0.26) * anchor.spread;
    const base = anchor.y + seat.dy * anchor.spread;
    // Spread of starting temperatures within the one spot, not a single heat
    // for every lick: the cooler ones are bigger and slower and fray at the
    // top, the hot ones stay small and low. That vertical white→orange→red
    // gradient is most of what makes a plume look like a plume — with every
    // lick starting at the same band it just reads as a lamp.
    const cooler = Math.random() * 1.7;
    const heat   = (i >= 3 ? 0.2 : i === 2 ? 1.6 : 2.8) + cooler;
    return {
      id: this._nextId++, fx: 'flame',
      x: axis, y: base + anchor.spread * (0.04 + Math.random() * 0.10),
      prevX: axis, prevY: base,
      axis,
      vy: -(0.25 + Math.random() * 0.25),
      buoy: 0.10 + Math.random() * 0.06 + i * 0.022,
      drift: (Math.random() - 0.5) * 0.16,
      sway: Math.random() * Math.PI * 2,
      swayRate: 0.10 + Math.random() * 0.13,
      swayAmp: anchor.spread * (0.04 + Math.random() * 0.05) * seat.scale,
      lean: 0,
      heat,
      cool: FIRE_RAMP.length - heat - 0.2,
      band: heat | 0,
      // Cooler licks are the fat, ragged ones; the hot core stays tight. Scaled
      // by the seat so the hex has big spots and small ones, not a uniform row.
      size: anchor.spread * (0.13 + Math.random() * 0.06 + cooler * 0.035)
            * (0.80 + i * 0.14) * seat.scale,
      // The sprite is already a tall tongue; stretching it much past 1 on top
      // of that is what turned the licks into streaks.
      stretch: 1.5 + Math.random() * 0.5,
      scale: 0.42,
      maxOpacity: 0.30 + Math.random() * 0.16,
      opacity: 0, age: 0, dead: false,
      ttl: 20 + Math.random() * 14 + i * 3,
    };
  }

  // A spark breaking free of the plume: leaves faster than the licks, cools
  // most of the way down the ramp, and outlives them, so a hot fire always
  // has a few points of light thrown above it.
  _spawnEmber(anchor, seat) {
    const heat = anchor.intensity >= 3 ? 0 : 1;
    const sx = anchor.x + seat.dx * anchor.spread;
    const sy = anchor.y + seat.dy * anchor.spread;
    return {
      id: this._nextId++, fx: 'ember',
      x: sx + (Math.random() - 0.5) * anchor.spread * 0.2,
      y: sy + anchor.spread * 0.05,
      prevX: sx, prevY: sy,
      vx: (Math.random() - 0.5) * 0.5,
      vy: -(1.1 + Math.random() * 1.4),
      phase: Math.random() * Math.PI * 2,
      heat,
      cool: FIRE_RAMP.length - heat - 1.2,
      band: heat,
      size: 0.8 + Math.random() * 0.9,
      maxOpacity: 0.6 + Math.random() * 0.35,
      opacity: 0, age: 0, dead: false,
      ttl: 45 + Math.random() * 40,
    };
  }

  // One slowly-swinging wind direction shared by every plume on the map. Smoke
  // that all leans the same way reads as weather; smoke where each puff picks
  // its own drift reads as a particle system. Two very slow, incommensurate
  // sines so the wind wanders instead of sweeping back and forth on a beat.
  get _wind() {
    return Math.sin(this._t * 0.0006) * 0.9 + Math.sin(this._t * 0.00017) * 0.45;
  }

  // Dark, non-additive plume. Starts just off the flame tips and climbs
  // slowly while expanding, so successive puffs stack into a column rather
  // than scattering. Rendered behind the flames (see render), so it can begin
  // low on the fire without dulling the hot part — and that contrast is what
  // makes the licks underneath read as bright rather than merely orange.
  _spawnSmoke(anchor, seat) {
    const sx   = anchor.x + seat.dx * anchor.spread;
    const sy   = anchor.y + seat.dy * anchor.spread;
    const wind = this._wind;
    return {
      id: this._nextId++, fx: 'smoke',
      x: sx + (Math.random() - 0.5) * anchor.spread * 0.34,
      y: sy - anchor.spread * (0.10 + Math.random() * 0.22),
      prevX: sx, prevY: sy,
      vx: wind * 0.28 + (Math.random() - 0.5) * 0.2,
      // Slow climb: fast-rising puffs string out into a dotted line, slow ones
      // pile up on each other into something with mass.
      vy: -(0.32 + Math.random() * 0.28),
      shear: wind * 0.009 + (Math.random() - 0.5) * 0.003,
      lean: Math.random() * Math.PI * 2,
      spin: (Math.random() - 0.5) * 0.012,
      size: anchor.spread * (0.30 + Math.random() * 0.16) * seat.scale,
      scale: 1,
      maxOpacity: 0.34 + Math.random() * 0.22,
      opacity: 0, age: 0, dead: false,
      ttl: 150 + Math.random() * 110,
    };
  }
}
