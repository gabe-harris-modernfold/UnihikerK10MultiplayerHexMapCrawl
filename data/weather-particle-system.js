// ── Weather Particle System ───────────────────────────────────────────────────
// Follows ash-particle-system.js structure exactly.
// Plain global class — no ES module syntax.

const WEATHER_PARTICLE_HARD_CAP = 300;

class WeatherParticleSystem {
  constructor() {
    this.particles = [];
    this._nextId = 0;
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
      if (this.particles.length >= WEATHER_PARTICLE_HARD_CAP) break;
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
    if (this.particles.length >= WEATHER_PARTICLE_HARD_CAP) return;
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
    if (this.particles.length >= WEATHER_PARTICLE_HARD_CAP) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnCreepingFog(anchor));
  }

  // Rolls a chance to flash one faint burst of static-like energy somewhere
  // in the creeping fog — a quick, small flicker, unlike chem's sustained
  // radioactive pulses or the big hex-to-hex lightning arc.
  emitStatic(chance, anchors) {
    if (!anchors?.length) return;
    if (this.particles.length >= WEATHER_PARTICLE_HARD_CAP) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnStatic(anchor));
  }

  // Rolls a chance to kick up one dust mote on a random shaking quake hex.
  emitDust(chance, anchors) {
    if (!anchors?.length) return;
    if (this.particles.length >= WEATHER_PARTICLE_HARD_CAP) return;
    if (Math.random() > chance) return;
    const anchor = anchors[(Math.random() * anchors.length) | 0];
    this.particles.push(this._spawnDust(anchor));
  }

  // Advance all particles one frame and cull dead ones.
  update() {
    for (const p of this.particles) {
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
    this.particles = this.particles.filter(p => !p.dead);
  }

  render(ctx) {
    for (const p of this.particles) {
      if (p.opacity <= 0.01) continue;
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
}
