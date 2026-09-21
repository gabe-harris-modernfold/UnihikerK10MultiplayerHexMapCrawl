// ── Storm Field ────────────────────────────────────────────────────────────
// Local, moving weather. Instead of tinting the whole screen, a squall line
// sweeps across the (toroidal) map over time and only the hex clumps it
// currently covers go dark and get rain/lightning/radioactive pulses. Plain
// globals — no ES module syntax — matching weather-particle-system.js.

const STORM_CLUMP_W = 3;   // clump width, in hexes (mapQ axis)
const STORM_CLUMP_R = 2;   // clump height, in hexes (mapR axis)
const STORM_SLANT   = 0.6; // squall-line tilt — front leans along +mapR

// Per-phase tuning: how wide/dense/dark/fast the front is, and how the hex
// itself is tinted. darkMax is the deepest per-hex darkening alpha reached
// at a clump's core; baseColor is the dark fill, tintColor/tintAlpha lay a
// second, saturated wash on top (storm's neon blue, chem's radioactive
// green) so the hex reads as colored-dark rather than just grey-dark.
// Fog (phase 4) still sweeps in like the other three (same slow travel time
// across the map is intentional) but popIn makes stormIntensityAt() curve
// steeply instead of linearly — a hex snaps to near-full fog as soon as the
// band's edge reaches it, instead of gradually darkening across the whole
// band width. staticBursts (instead of lightning/arcLightning) gates the
// small faint-energy particle flicker in renderWeatherOverlay() —
// deliberately not the big dramatic bolt look.
// Mist (phase 5) is the plain, non-hazardous fog: same popIn sweep as
// Strangle Fog, but no tintColor at all (no black, no green — just an
// off-white color) and darkMax pushed almost to 1 so the hex is fully
// obscured, not just darkened.
// soft/softRGB(/tintRGB) switch drawCellOverlays() to a radial-gradient wash
// instead of the hex-clipped flat fill the other phases use — bleeding past
// each hex's own edge into its neighbors so covered patches blend into one
// fuzzy blob, not a mosaic of crisp hexagon tiles. Both fog phases use it;
// rain/storm/chem keep the flat fill since nobody's asked for those to go
// soft too — its clip bleeds a hair past each hex edge (STORM_WASH_BLEED in
// renderer.js) so the flat path has no per-hex seams either; what stays
// crisp is the outer boundary where the covered patch ends, not the grid
// inside it. tintRGB/softRGB are the same colors as tintColor/baseColor,
// just pre-split into "R,G,B" so the gradient's rgba() strings don't need to
// parse a hex string every frame.
const STORM_PHASE_CFG = {
  1: { speed: 0.00042, halfWidth: 5, density: 0.34, darkMax: 0.22,
       baseColor: '#1C2430', lightning: false },
  2: { speed: 0.00085, halfWidth: 8, density: 0.55, darkMax: 0.74,
       baseColor: '#05070C', tintColor: '#3E5670', tintAlpha: 0.22, lightning: true },
  3: { speed: 0.00055, halfWidth: 9, density: 0.60, darkMax: 0.48,
       baseColor: '#050F08', tintColor: '#22FF66', tintAlpha: 0.24, arcLightning: true },
  4: { speed: 0.00010, halfWidth: 12, density: 0.72, darkMax: 0.8,
       baseColor: '#020302', tintColor: '#173820', tintAlpha: 0.14, staticBursts: true, popIn: true,
       soft: true, softRGB: '2,3,2', tintRGB: '23,56,32' },
  5: { speed: 0.00030, halfWidth: 12, density: 0.75, darkMax: 0.95,
       baseColor: '#F2F2EC', soft: true, softRGB: '242,242,236', popIn: true },
};

// Deterministic 0..1 hash for a clump cell — stable per (cq, cr) so a clump's
// footprint doesn't flicker as the storm front sweeps through it.
function stormClumpHash(cq, cr) {
  let h = (cq * 374761393 + cr * 668265263 + 0x9E3779B9) | 0;
  h = (h ^ (h >>> 13)) * 1274126177;
  h = (h ^ (h >>> 16)) >>> 0;
  return h / 4294967295;
}

// World-space position (in mapQ hexes, wrapped) of the squall line's leading edge.
function stormFrontPos(phase, now) {
  const cfg = STORM_PHASE_CFG[phase] || STORM_PHASE_CFG[1];
  return (now * cfg.speed) % MAP_COLS;
}

// Signed distance (in hexes) from a hex to the front, wrapped across the
// map's torus and slanted so the line isn't perfectly vertical.
function stormFrontDist(mapQ, mapR, phase, now) {
  const front = stormFrontPos(phase, now);
  const eq    = mapQ + mapR * STORM_SLANT;
  let d = eq - front;
  d -= Math.round(d / MAP_COLS) * MAP_COLS;
  return d;
}

// 0..1 storm intensity for one hex. Zero outside the moving band; inside it,
// ramps up as the line approaches (hex "hovers" under the storm), then back
// down as it passes — masked by a per-clump hash so only patches of hexes
// within the band actually light up, not the whole band. Chem storm (phase 3)
// reuses this same mechanic, just with its own speed/width/density and color.
// Fog (phase 4, popIn) reshapes the same 0..1 linear ramp with a steep
// exponent so it stays near-full across most of the band and only tapers
// hard at the outer edge — a "pop" onset instead of a gradual fade, while
// the band's travel time across the map (speed/halfWidth) is untouched.
function stormIntensityAt(mapQ, mapR, phase, now) {
  const cfg = STORM_PHASE_CFG[phase];
  if (!cfg) return 0;
  const ad = Math.abs(stormFrontDist(mapQ, mapR, phase, now));
  if (ad > cfg.halfWidth) return 0;
  const cq = Math.floor(mapQ / STORM_CLUMP_W);
  const cr = Math.floor(mapR / STORM_CLUMP_R);
  if (stormClumpHash(cq, cr) > cfg.density) return 0;
  const lin = 1 - ad / cfg.halfWidth;
  return cfg.popIn ? Math.pow(lin, 0.2) : lin;
}

// ── Lightning ──────────────────────────────────────────────────────────────
// Two flavors sharing one jagged-path builder: a "strike" falls from off the
// top of the screen onto one storm hex (rain storm); an "arc" connects two
// storm hexes to each other, reading as energy jumping across the landscape
// (chem storm).
class LightningSystem {
  constructor() {
    this.bolts = [];
    this.lastStrikeAt = 0;
    this.lastArcAt    = 0;
  }

  // Rolls a chance (once the cooldown has elapsed) to strike one of the given
  // {x, y, intensity} storm-hex anchors from above, favoring the densest core hexes.
  maybeStrike(anchors, now, minGapMs, chance) {
    if (!anchors.length) return;
    if (now - this.lastStrikeAt < minGapMs) return;
    if (Math.random() > chance) return;
    this.lastStrikeAt = now;
    const core = anchors.filter(a => a.intensity > 0.55);
    const pool = core.length ? core : anchors;
    const t = pool[(Math.random() * pool.length) | 0];
    const from = { x: t.x + (Math.random() - 0.5) * 30, y: -30 };
    this.bolts.push(this._build(from, { x: t.x, y: t.y }, '#EAF4FF', '210,230,255', [{ x: t.x, y: t.y }], now));
  }

  // Rolls a chance to arc a bolt directly between two nearby storm hexes —
  // energy connecting across the landscape rather than falling from the sky.
  maybeArc(anchors, now, minGapMs, chance) {
    if (anchors.length < 2) return;
    if (now - this.lastArcAt < minGapMs) return;
    if (Math.random() > chance) return;
    this.lastArcAt = now;
    const core = anchors.filter(a => a.intensity > 0.45);
    const pool = core.length >= 2 ? core : anchors;
    const a = pool[(Math.random() * pool.length) | 0];
    let b = a;
    for (let tries = 0; tries < 6 && b === a; tries++) b = pool[(Math.random() * pool.length) | 0];
    if (b === a) return;
    this.bolts.push(this._build(
      { x: a.x, y: a.y }, { x: b.x, y: b.y },
      '#9CFFB0', '120,255,140', [{ x: a.x, y: a.y }, { x: b.x, y: b.y }], now,
    ));
  }

  // Jagged path from `from` to `to`, offset perpendicular to the line so it
  // works for a near-vertical sky strike or a near-horizontal ground arc alike.
  _build(from, to, strokeColor, glowRGB, glowPoints, now) {
    const segs = 6;
    const dx = to.x - from.x, dy = to.y - from.y;
    const len = Math.hypot(dx, dy) || 1;
    const nx = -dy / len, ny = dx / len; // unit perpendicular
    const points = [];
    for (let i = 0; i <= segs; i++) {
      const t = i / segs;
      const jitter = Math.sin(t * Math.PI) * 16 + 4; // tapers to 0 at both ends
      const off = (i === 0 || i === segs) ? 0 : (Math.random() - 0.5) * jitter;
      points.push({ x: from.x + dx * t + nx * off, y: from.y + dy * t + ny * off });
    }
    let branch = null;
    if (segs > 3 && Math.random() < 0.6) {
      const from2 = points[2 + ((Math.random() * (segs - 3)) | 0)];
      branch = [from2];
      for (let i = 1; i <= 3; i++) {
        const prev = branch[i - 1];
        branch.push({ x: prev.x + (Math.random() - 0.5) * 20 + nx * 10, y: prev.y + (Math.random() - 0.5) * 20 + ny * 10 });
      }
    }
    return { points, branch, glowPoints, strokeColor, glowRGB, bornAt: now, ttl: 260 };
  }

  update(now) {
    this.bolts = this.bolts.filter(b => now - b.bornAt < b.ttl);
  }

  render(ctx, now, hexSz, cssWidth, cssHeight) {
    for (const b of this.bolts) {
      const age = now - b.bornAt;
      const a   = age < 40 ? age / 40 : Math.max(0, 1 - (age - 40) / (b.ttl - 40));
      if (a <= 0.01) continue;

      if (age < 30) {
        ctx.save();
        ctx.globalAlpha = 0.10 * (1 - age / 30);
        ctx.fillStyle   = b.strokeColor;
        ctx.fillRect(0, 0, cssWidth, cssHeight);
        ctx.restore();
      }

      ctx.save();
      ctx.globalAlpha = a;
      ctx.strokeStyle = b.strokeColor;
      ctx.lineWidth   = 2;
      ctx.shadowColor = `rgba(${b.glowRGB},1)`;
      ctx.shadowBlur  = 14;
      this._stroke(ctx, b.points);
      if (b.branch) this._stroke(ctx, b.branch);
      ctx.restore();

      for (const gp of b.glowPoints) {
        ctx.save();
        const g = ctx.createRadialGradient(gp.x, gp.y, 0, gp.x, gp.y, hexSz * 2.2);
        g.addColorStop(0, `rgba(${b.glowRGB},${0.35 * a})`);
        g.addColorStop(1, `rgba(${b.glowRGB},0)`);
        ctx.fillStyle = g;
        ctx.fillRect(gp.x - hexSz * 2.2, gp.y - hexSz * 2.2, hexSz * 4.4, hexSz * 4.4);
        ctx.restore();
      }
    }
  }

  _stroke(ctx, pts) {
    ctx.beginPath();
    ctx.moveTo(pts[0].x, pts[0].y);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i].x, pts[i].y);
    ctx.stroke();
  }
}
