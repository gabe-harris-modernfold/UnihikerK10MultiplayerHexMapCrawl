// ── Ash Particle System ──────────────────────────────────────────
// Terrain types that produce ash cloud particles
const ASH_TERRAIN_TYPES = new Set([1, 6, 8]); // 1=Ash Dunes, 6=Glass Fields, 8=Mountain

// Max total particles regardless of terrain count (perf guard)
const ASH_PARTICLE_HARD_CAP = 60;

// Particle gradient colours: warm grey ash fading to transparent
const ASH_COLOR_INNER  = '125,120,115';  // centre of particle puff
const ASH_COLOR_MID    = '115,110,105';  // mid-gradient stop
const ASH_COLOR_OUTER  = '105,100,100';  // outer edge (always transparent)

class AshParticleSystem {
  constructor(cfg) {
    this.maxAlpha         = cfg.maxAlpha         ?? 0.45;
    this.fadeInMs         = cfg.fadeInMs         ?? 2800; // ms to reach full opacity
    this.driftSpeed       = cfg.driftSpeed       ?? 0.04;
    this.wobbleAmp        = cfg.wobbleAmp        ?? 8;
    this.wobbleFreq       = cfg.wobbleFreq       ?? 0.0007;
    this.maxDistanceHexes = cfg.maxDistanceHexes ?? 3;
    this.countPerTerrain  = cfg.countPerTerrain  ?? 2;
    this.radiusMin        = cfg.radiusMin        ?? 1.5;
    this.radiusMax        = cfg.radiusMax        ?? 3;
    this.particles = [];
    this._nextId = 0;
  }

  update(gameMap, hexSz) {
    const now = Date.now();
    const maxDistPx = this.maxDistanceHexes * hexSz * SQRT3;

    // Advance each particle and mark dead ones
    for (const p of this.particles) {
      const dt = now - p.prevNow;
      p.prevNow = now;
      p.driftOffsetX += p.driftVX * dt;
      p.driftOffsetY += p.driftVY * dt;
      const dist = Math.hypot(p.driftOffsetX, p.driftOffsetY);
      const fadeIn = Math.min(1, (now - p.spawnTime) / this.fadeInMs);
      p.currentAlpha = p.maxAlpha * fadeIn * Math.max(0, 1 - dist / maxDistPx);
      p.dead = dist >= maxDistPx;
    }

    // Remove dead particles
    this.particles = this.particles.filter(p => !p.dead);

    // Spawn replacements around qualifying terrain hexes
    if (!gameMap || this.particles.length >= ASH_PARTICLE_HARD_CAP) return;
    for (let r = 0; r < MAP_ROWS; r++) {
      for (let q = 0; q < MAP_COLS; q++) {
        const cell = gameMap[r][q];
        if (!cell || !ASH_TERRAIN_TYPES.has(cell.terrain)) continue;
        const have = this._countForOrigin(q, r);
        const need = this.countPerTerrain - have;
        for (let i = 0; i < need; i++) {
          if (this.particles.length >= ASH_PARTICLE_HARD_CAP) return;
          this.particles.push(this._spawnNear(q, r, hexSz, now));
        }
      }
    }
  }

  render(ctx, ox, oy, hexSz) {
    const now = Date.now();
    for (const p of this.particles) {
      if (p.currentAlpha <= 0.01) continue;

      // World-space base = origin hex center (no camera offset yet)
      const bx = hexSz * 1.5 * p.originQ;
      const by = hexSz * (SQRT3 / 2 * p.originQ + SQRT3 * p.originR);

      // Sine wobble layered on top of linear drift
      const wx = Math.sin(now * p.wobbleFreq + p.wobblePhaseX) * p.wobbleAmp;
      const wy = Math.sin(now * p.wobbleFreq * 0.83 + p.wobblePhaseY) * p.wobbleAmp;

      const sx = bx + ox + p.driftOffsetX + wx;
      const sy = by + oy + p.driftOffsetY + wy;

      const a  = p.currentAlpha;
      const a2 = (a * 0.5).toFixed(3);
      const as = a.toFixed(3);

      ctx.save();
      const grad = ctx.createRadialGradient(sx, sy, 0, sx, sy, p.radius);
      grad.addColorStop(0,   `rgba(${ASH_COLOR_INNER},${as})`);
      grad.addColorStop(0.55, `rgba(${ASH_COLOR_MID},${a2})`);
      grad.addColorStop(1,   `rgba(${ASH_COLOR_OUTER},0)`);
      ctx.fillStyle = grad;
      ctx.beginPath();
      ctx.arc(sx, sy, p.radius, 0, Math.PI * 2);
      ctx.fill();
      ctx.restore();
    }
  }

  _spawnNear(originQ, originR, hexSz, now) {
    // Random outward direction
    const angle = Math.random() * Math.PI * 2;
    const speed = this.driftSpeed * (0.5 + Math.random());

    // Small random initial offset so particles don't all pop from exact center
    const spawnDist = hexSz * 0.4 * Math.random();
    const spawnAngle = Math.random() * Math.PI * 2;

    return {
      id:           this._nextId++,
      originQ,
      originR,
      driftOffsetX: Math.cos(spawnAngle) * spawnDist,
      driftOffsetY: Math.sin(spawnAngle) * spawnDist,
      driftVX:      Math.cos(angle) * speed,
      driftVY:      Math.sin(angle) * speed,
      spawnTime:    now,
      prevNow:      now,
      wobblePhaseX: Math.random() * Math.PI * 2,
      wobblePhaseY: Math.random() * Math.PI * 2,
      wobbleAmp:    this.wobbleAmp * (0.5 + Math.random()),
      wobbleFreq:   this.wobbleFreq * (0.7 + Math.random() * 0.6),
      radius:       hexSz * (this.radiusMin + Math.random() * (this.radiusMax - this.radiusMin)),
      maxAlpha:     this.maxAlpha * (0.6 + Math.random() * 0.4),
      currentAlpha: 0,
      dead:         false,
    };
  }

  _countForOrigin(q, r) {
    let count = 0;
    for (const p of this.particles) {
      if (p.originQ === q && p.originR === r) count++;
    }
    return count;
  }
}
