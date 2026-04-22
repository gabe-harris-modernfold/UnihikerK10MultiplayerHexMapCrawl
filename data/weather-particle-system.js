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
  emit(count, phase, w, h) {
    for (let i = 0; i < count; i++) {
      if (this.particles.length >= WEATHER_PARTICLE_HARD_CAP) break;
      this.particles.push(this._spawn(phase, w, h));
    }
  }

  // Advance all particles one frame and cull dead ones.
  update() {
    const FADE = 10;
    const TTL  = 120;
    for (const p of this.particles) {
      p.x += p.dx;
      p.y += p.dy;
      p.age++;
      if      (p.age < FADE)       p.opacity = (p.age / FADE) * p.maxOpacity;
      else if (p.age > TTL - FADE) p.opacity = ((TTL - p.age) / FADE) * p.maxOpacity;
      else                         p.opacity = p.maxOpacity;
      p.dead = p.age >= TTL;
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
      } else {
        ctx.fillStyle = p.color;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.restore();
    }
  }

  _spawn(phase, w, h) {
    if (phase === 1) {
      // Light Rain: gentle diagonal lines from top
      return {
        id: this._nextId++, shape: 'line',
        x: Math.random() * w, y: -10,
        dx: -0.3, dy: 3.5 + Math.random() * 1.5,
        color: 'rgba(120,150,200,0.6)',
        size: 1,
        maxOpacity: 0.5 + Math.random() * 0.3,
        opacity: 0, age: 0, dead: false,
      };
    }
    if (phase === 2) {
      // Storm: heavy angled rain from top
      return {
        id: this._nextId++, shape: 'line',
        x: Math.random() * (w + 100) - 50, y: -10,
        dx: -1.2 - Math.random() * 0.8, dy: 6 + Math.random() * 3,
        color: 'rgba(80,110,160,0.7)',
        size: 1,
        maxOpacity: 0.6 + Math.random() * 0.3,
        opacity: 0, age: 0, dead: false,
      };
    }
    // phase === 3: Chem — greenish circles drifting upward
    return {
      id: this._nextId++, shape: 'circle',
      x: Math.random() * w, y: h + 5,
      dx: (Math.random() - 0.5) * 0.8,
      dy: -(0.5 + Math.random()),
      color: `rgba(${60 + Math.trunc(Math.random() * 40)},${180 + Math.trunc(Math.random() * 60)},${20 + Math.trunc(Math.random() * 30)},0.8)`,
      size: 1.5 + Math.random(),
      maxOpacity: 0.4 + Math.random() * 0.3,
      opacity: 0, age: 0, dead: false,
    };
  }
}
