// ── Weather particle system ───────────────────────────────────────
const weatherParticles = (typeof WeatherParticleSystem === 'undefined')
  ? null : new WeatherParticleSystem();
const lightningSystem = (typeof LightningSystem === 'undefined')
  ? null : new LightningSystem();
const quakeField = (typeof QuakeField === 'undefined')
  ? null : new QuakeField();
const fireField = (typeof FireField === 'undefined')
  ? null : new FireField();

// Storm hexes ({x, y, spread, intensity} in screen space) collected during
// this frame's terrain pass — consumed by renderWeatherOverlay() afterward
// so rain/lightning only ever appear over the clumps the storm field
// actually darkened, never the whole screen.
let stormyHexesThisFrame = [];
// Quake fault-line pixel positions collected the same way: quakeId -> array
// of {x, y} indexed by the cell's order along the line (gaps where a cell
// isn't currently on screen), consumed by renderQuakeOverlay() afterward.
let quakePixelsThisFrame = new Map();
// Burning-hex pixel anchors ({x, y, spread, intensity}) collected during
// this frame's terrain pass — consumed by emitFire() afterward, same
// handoff pattern as stormyHexesThisFrame.
let fireHexesThisFrame = [];
// Pixel positions of hexes the quake leveled from Settlement to Open Scrub —
// these get an extra, heavier burst of dust on top of the regular fault-line
// dust to sell "a settlement just got flattened" rather than an ordinary shake.
let quakeConvertedPixelsThisFrame = [];
let weatherNow = Date.now();

const LIGHTNING_MIN_GAP_MS   = 3200;
const LIGHTNING_STRIKE_CHANCE = 0.02; // rolled once per eligible frame after the gap elapses
const ARC_MIN_GAP_MS   = 4200;
const ARC_STRIKE_CHANCE = 0.02; // chem storm's hex-to-hex arc, same cadence idea as the sky strike
const QUAKE_DUST_CHANCE = 0.28; // rolled twice per frame per active quake
const QUAKE_CONVERTED_DUST_CHANCE = 0.8; // rolled several times per frame per leveled-settlement hex

// ── Smooth animation state ────────────────────────────────────────
const renderPos = Array.from({ length: MAX_PLAYERS }, () => ({ q: 0, r: 0 }));

// ── Name-tag width cache ──────────────────────────────────────────
// Avoids a ctx.measureText() call every frame per visible player.
// Keyed by player index; invalidated when tag text or font size changes.
const nameWidthCache = new Array(MAX_PLAYERS).fill(null);

// ── Surveyed cells ────────────────────────────────────────────────
// Cells revealed by the SURVEY action beyond normal vision radius.
// Rendered at reduced opacity as scouted-but-not-directly-visible.
// Cleared whenever the local player moves.
const surveyedCells = new Set(); // 'q_r' keys — populated from network.js (cross-file; ignore S4158)
/* global displayMP, nightFade, fireHexPhase */

// Stable 1..N permutation keyed by (q,r) — non-positional reference number.
const hexLabel = (() => {
  const N = MAP_COLS * MAP_ROWS;
  const arr = new Array(N);
  for (let i = 0; i < N; i++) arr[i] = i + 1;
  let seed = 0x9E3779B1;
  const rand = () => {
    seed = (seed * 1664525 + 1013904223) >>> 0;
    return seed / 0x100000000;
  };
  for (let i = N - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [arr[i], arr[j]] = [arr[j], arr[i]];
  }
  return arr;
})();

// ── Hex math (flat-top) ─────────────────────────────────────────
function hexToPixel(q, r, size) {
  return { x: size * 1.5 * q, y: size * (SQRT3 / 2 * q + SQRT3 * r) };
}

function hexDist(q1, r1, q2, r2) {
  const dq = q2 - q1, dr = r2 - r1;
  return (Math.abs(dq) + Math.abs(dq + dr) + Math.abs(dr)) / 2;
}
function hexDistWrap(q1, r1, q2, r2) {
  let min = Infinity;
  for (let dq = -1; dq <= 1; dq++)
    for (let dr = -1; dr <= 1; dr++)
      min = Math.min(min, hexDist(q1, r1, q2 + dq * MAP_COLS, r2 + dr * MAP_ROWS));
  return min;
}

function drawHexPath(ctx, cx, cy, size) {
  ctx.beginPath();
  for (let i = 0; i < 6; i++) {
    const a = Math.PI / 3 * i;
    i === 0 ? ctx.moveTo(cx + size * Math.cos(a), cy + size * Math.sin(a))
            : ctx.lineTo(cx + size * Math.cos(a), cy + size * Math.sin(a));
  }
  ctx.closePath();
}

// ── Canvas setup ─────────────────────────────────────────────────
const canvas = document.getElementById('hexCanvas');
const ctx    = canvas.getContext('2d');
let HEX_SZ    = 56;
// CSS-pixel dimensions used throughout the render loop.
// canvas.width/height hold physical pixels (= cssWidth/Height × devicePixelRatio).
let cssWidth  = 0;
let cssHeight = 0;

const ZOOM_STEP_PX  = 4;
const ZOOM_STEP_MAX = 6;
let   zoomStep      = 0;

function resize() {
  const wrap = document.getElementById('canvas-wrap');
  const dpr  = window.devicePixelRatio || 1;
  cssWidth   = wrap.clientWidth;
  cssHeight  = wrap.clientHeight;
  canvas.width  = cssWidth  * dpr;
  canvas.height = cssHeight * dpr;
  canvas.style.width  = cssWidth  + 'px';
  canvas.style.height = cssHeight + 'px';
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);  // reset any prior transform then scale
  const base = Math.max(HEX_SZ_MIN, Math.min(HEX_SZ_MAX, Math.floor(cssWidth / HEX_SZ_VIEWPORT_DIVISOR)));
  HEX_SZ = Math.max(HEX_SZ_MIN - ZOOM_STEP_PX * ZOOM_STEP_MAX,
                    Math.min(HEX_SZ_MAX + ZOOM_STEP_PX * ZOOM_STEP_MAX,
                             base + zoomStep * ZOOM_STEP_PX));
}
window.addEventListener('resize', resize);
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  zoomStep = Math.max(-ZOOM_STEP_MAX, Math.min(ZOOM_STEP_MAX,
    zoomStep + (e.deltaY > 0 ? -1 : 1)));
  resize();
}, { passive: false });
resize();

// ── Pixel glyphs ───────────────────────────────────────────────────
// Tinted copies of one 16px cell from the glyph strip, keyed `${idx}|${color}`.
const _glyphTints = new Map();
function _glyphTile(idx, color) {
  const key = idx + '|' + color;
  let c = _glyphTints.get(key);
  if (c) return c;
  c = document.createElement('canvas');
  c.width = c.height = GLYPH_CELL;
  const g = c.getContext('2d');
  g.drawImage(glyphImg, idx * GLYPH_CELL, 0, GLYPH_CELL, GLYPH_CELL, 0, 0, GLYPH_CELL, GLYPH_CELL);
  g.globalCompositeOperation = 'source-in';   // recolour, keep alpha
  g.fillStyle = color;
  g.fillRect(0, 0, GLYPH_CELL, GLYPH_CELL);
  _glyphTints.set(key, c);
  return c;
}

/**
 * Draw a glyph from the sprite strip with its top-left at (x, y).
 * Nearest-neighbour when enlarging (keeps the pixel look); bilinear when
 * shrinking below 16px so no rows drop out. No-op until the strip loads.
 */
function drawGlyph(ctx, idx, x, y, size, color = '#FFF', alpha = 1) {
  if (idx == null || idx < 0 || !glyphImg.loaded) return;
  const px = Math.max(1, Math.round(size));
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.imageSmoothingEnabled = px < GLYPH_CELL;
  ctx.drawImage(_glyphTile(idx, color), Math.round(x), Math.round(y), px, px);
  ctx.restore();
}

/**
 * Draw terrain icon centered in a hex cell.
 * @param {CanvasRenderingContext2D} ctx - Canvas context
 * @param {number} cx - Center X coordinate
 * @param {number} cy - Center Y coordinate
 * @param {number} hexSz - Hex size in pixels
 * @param {number} terrainIdx - Terrain type index
 * @param {boolean} hasResource - Whether cell has a resource (affects positioning/opacity)
 */
function drawTerrainIcon(ctx, cx, cy, hexSz, terrainIdx, hasResource) {
  const t = TERRAIN[terrainIdx];
  if (!t) return;
  const sz   = Math.max(10, hexSz * ICON_SIZE_SCALE);
  const offY = hasResource ? -hexSz * 0.42 : 0;
  // Glyph strip index 0..11 == terrain index
  drawGlyph(ctx, terrainIdx, cx - sz / 2, cy + offY - sz / 2, sz, '#FFF', hasResource ? 0.45 : 0.7);
}

/**
 * Draw resource icon centered on a hex cell.
 * Used for Water, Fuel, Medicine, Scrap (Food uses forage-animal PNG instead).
 */
function drawResourceIcon(ctx, cx, cy, hexSz, resourceType) {
  const idx = RES_GLYPH[resourceType] ?? -1;
  if (idx < 0) return;
  const sz = Math.max(10, hexSz * ICON_SIZE_SCALE);
  drawGlyph(ctx, idx, cx - sz / 2, cy - sz / 2, sz, '#FFF', 0.3);
}

/**
 * Draw character/player icon on the hex grid.
 * Includes head, torso, name label, and ground shadow.
 * @param {CanvasRenderingContext2D} ctx - Canvas context
 * @param {number} cx - Center X coordinate
 * @param {number} cy - Center Y coordinate
 * @param {number} hexSz - Hex size in pixels
 * @param {object} opts - { color, label, isMe, nm, arch, sc }
 */
function drawCharIcon(ctx, cx, cy, hexSz, { color, label, isMe, nm, arch, sc } = {}) {
  const scale      = hexSz * ICON_SIZE_SCALE;
  const r          = Math.max(10, hexSz * 0.28);   // portrait circle radius
  const portraitCY = cy - scale * 0.15;            // circle center, slightly above hex centre

  // Ground shadow — fuzzy ellipse beneath the circle
  ctx.save();
  ctx.filter = 'blur(3px)';
  ctx.beginPath();
  ctx.ellipse(cx, cy + r * 0.85, r * 0.65, r * 0.18, 0, 0, Math.PI * 2);
  ctx.fillStyle = 'rgba(0,0,0,0.45)';
  ctx.fill();
  ctx.restore();

  // Portrait circle (clipped image, or fallback solid colour + number)
  const img = pawnImgs[arch];
  if (img?.loaded) {
    ctx.save();
    ctx.beginPath();
    ctx.arc(cx, portraitCY, r, 0, Math.PI * 2);
    ctx.clip();
    ctx.drawImage(img, cx - r, portraitCY - r, r * 2, r * 2);
    ctx.restore();
  } else {
    ctx.beginPath();
    ctx.arc(cx, portraitCY, r, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
    ctx.save();
    ctx.fillStyle    = '#000';
    ctx.font         = `bold ${Math.max(7, Math.round(r * 0.9))}px monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, cx, portraitCY + 0.5);
    ctx.restore();
  }

  // Coloured outline — archetype ring (all players)
  const outlineW = Math.max(1.5, r * 0.09);
  ctx.beginPath();
  ctx.arc(cx, portraitCY, r, 0, Math.PI * 2);
  ctx.strokeStyle = color;
  ctx.lineWidth   = outlineW;
  ctx.stroke();

  // Current player: brighter/thicker second ring just outside
  if (isMe) {
    ctx.beginPath();
    ctx.arc(cx, portraitCY, r + outlineW + 1, 0, Math.PI * 2);
    ctx.strokeStyle = color;
    ctx.lineWidth   = Math.max(2, outlineW * 0.7);
    ctx.globalAlpha = 0.7;
    ctx.stroke();
    ctx.globalAlpha = 1;
  }

  const nameSz = Math.max(8, Math.round(hexSz * 0.21));
  // Call sign above circle
  if (nm) {
    const tag  = nm.substring(0, 8).toUpperCase();
    const topY = portraitCY - r - 4;
    ctx.save();
    ctx.font          = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign     = 'center';
    ctx.textBaseline  = 'bottom';
    ctx.fillStyle     = '#000';
    ctx.letterSpacing = '1px';
    ctx.fillText(tag, cx, topY);
    ctx.restore();
  }

  // Score below circle
  if (sc !== undefined && sc !== null) {
    const scoreStr = String(sc);
    const botY     = portraitCY + r + nameSz + 6;
    ctx.save();
    ctx.font          = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign     = 'center';
    ctx.textBaseline  = 'bottom';
    ctx.fillStyle     = '#000';
    ctx.letterSpacing = '1px';
    ctx.fillText(scoreStr, cx, botY);
    ctx.restore();
  }
}

// ── Time-of-day clock ────────────────────────────────────────────
const TIME_PHASES = [
  { name:'FIRST LIGHT', icon:'☀',      r:255, g:180, b:100, a:0.03 },
  { name:'HIGH WATCH',  icon:'☀',      r:255, g:220, b:150, a:0.01 },
  { name:'NOON BURN',   icon:'☀',      r:255, g:240, b:200, a:0 },
  { name:'LOW SUN',     icon:'🌇', r:255, g:160, b: 60, a:0.06 },
  { name:'DUST HOUR',   icon:'🌆', r:210, g: 90, b: 20, a:0.14 },
  { name:'DARK WATCH',  icon:'🌑', r: 15, g:  8, b: 40, a:0.72 },
];
function getTimePhase(mpVal) {
  const _mp = mpVal === undefined ? uiMP.val : mpVal;
  if (myId >= 0 && maxMP > 0) {
    const f      = Math.max(0, Math.min(1, 1 - _mp / maxMP));
    const scaled = f * (TIME_PHASES.length - 1);
    const i      = Math.floor(scaled);
    const t      = scaled - i;
    const p0     = TIME_PHASES[Math.min(i, TIME_PHASES.length - 1)];
    const p1     = TIME_PHASES[Math.min(i + 1, TIME_PHASES.length - 1)];
    return {
      icon: p0.icon, name: p0.name,
      r: Math.round(p0.r + t * (p1.r - p0.r)),
      g: Math.round(p0.g + t * (p1.g - p0.g)),
      b: Math.round(p0.b + t * (p1.b - p0.b)),
      a: p0.a + t * (p1.a - p0.a)
    };
  }
  return TIME_PHASES[0];
}

// ── Ash particle system (Pass 3) ────────────────────────────────
const ashParticles = (typeof AshParticleSystem === 'undefined')
  ? null : new AshParticleSystem({
    maxAlpha:         0.52,
    driftSpeed:       0.04,
    wobbleAmp:        8,
    wobbleFreq:       0.0007,
    maxDistanceHexes: 3,
    countPerTerrain:  2,
    radiusMin:        1.5,
    radiusMax:        3,
  });

// ── Render helpers ──────────────────────────────────────────────

function lerpPlayerPositions() {
  for (let i = 0; i < MAX_PLAYERS; i++) {
    const p = players[i];
    if (!p.on) { renderPos[i].q = p.q; renderPos[i].r = p.r; continue; }
    let tq = p.q, tr = p.r;
    while (tq - renderPos[i].q >  MAP_COLS / 2) tq -= MAP_COLS;
    while (renderPos[i].q - tq >  MAP_COLS / 2) tq += MAP_COLS;
    while (tr - renderPos[i].r >  MAP_ROWS / 2) tr -= MAP_ROWS;
    while (renderPos[i].r - tr >  MAP_ROWS / 2) tr += MAP_ROWS;
    renderPos[i].q += (tq - renderPos[i].q) * LERP_RATE;
    renderPos[i].r += (tr - renderPos[i].r) * LERP_RATE;
  }
}

function buildCamera() {
  const meRp  = myId >= 0 ? renderPos[myId] : { q: 0, r: 0 };
  const meAct = myId >= 0 ? players[myId]   : { q: 0, r: 0 };
  const cp    = hexToPixel(meRp.q, meRp.r, HEX_SZ);
  // Whole-view quake shake — applied to the shared camera offset so every
  // layer (terrain, grid, characters, ...) trembles together, not just the
  // fault-line hexes themselves.
  const shake = quakeField ? quakeField.peakEnvelope() : 0;
  const shakeMag = shake * 7;
  const shakeX = shake > 0 ? (Math.random() - 0.5) * shakeMag : 0;
  const shakeY = shake > 0 ? (Math.random() - 0.5) * shakeMag : 0;
  return {
    meRp,
    meAct,
    ox:      cssWidth  / 2 - cp.x + shakeX,
    oy:      cssHeight / 2 - cp.y + shakeY,
    centreQ: Math.round(meRp.q),
    centreR: Math.round(meRp.r),
    viewQ:   Math.ceil(cssWidth  / (HEX_SZ * 1.5)) + 2,
    viewR:   Math.ceil(cssHeight / (HEX_SZ * SQRT3)) + 2,
  };
}

function drawRiverRipples(cx, cy) {
  const rt = performance.now() / 1000;
  ctx.save();
  ctx.lineWidth = 1;
  for (let w = 0; w < RIVER_RIPPLE_COUNT; w++) {
    const phase = (rt * RIVER_RIPPLE_SPEED + w * RIVER_RIPPLE_SPACING) % 1;
    const scale = 0.25 + phase * 0.55;
    const alpha = 0.18 * (1 - phase);
    ctx.strokeStyle = `rgba(30,70,90,${alpha.toFixed(3)})`;
    ctx.beginPath();
    ctx.ellipse(cx, cy, HEX_SZ * scale * RIVER_RIPPLE_W_SCALE, HEX_SZ * scale * RIVER_RIPPLE_H_SCALE, 0, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.restore();
}

function drawFootprints(cx, cy, cell) {
  if (cell.footprints <= 0) return;
  const footprintSize  = Math.max(6, Math.round(HEX_SZ * 0.22));
  let footprintCount   = 0;
  for (let i = 0; i < 6; i++) if (cell.footprints & (1 << i)) footprintCount++;
  let footprintIdx = 0;
  for (let fpid = 0; fpid < 6; fpid++) {
    if ((cell.footprints & (1 << fpid)) === 0) continue;
    const angle  = (footprintIdx * Math.PI * 2) / Math.max(1, footprintCount);
    const radius = HEX_SZ * FOOTPRINT_RING_RADIUS;
    const fx = cx + Math.cos(angle) * radius;
    const fy = cy + Math.sin(angle) * radius;
    // Worn-in tracks: black boot sole
    drawGlyph(ctx, GLYPH.FOOTPRINT, fx - footprintSize / 2, fy - footprintSize / 2,
              footprintSize, '#000000', 0.75);
    footprintIdx++;
  }
}

function drawShelterIcon(cx, cy, cell, mapQ, mapR) {
  const imgs = shelterImgs[0] ?? [];   // mock server never sends shelter variants
  const v    = imgs.length > 0 ? (mapQ * 31 + mapR * 17) % imgs.length : -1;
  const sImg = v >= 0 ? imgs[v] : null;
  if (sImg?.loaded) {
    const sz = HEX_SZ * 0.9;
    ctx.drawImage(sImg, cx - sz / 2, cy - sz / 2, sz, sz);
  } else {
    // Upper-right corner of the hex; improved shelter = hut in steel blue, basic = tan tent
    const sz = Math.max(12, Math.round(HEX_SZ * 0.5));
    drawGlyph(ctx, cell.shelter === 2 ? GLYPH.HUT : GLYPH.TENT,
              cx + HEX_SZ * 0.35 - sz, cy - HEX_SZ * 0.35, sz,
              cell.shelter === 2 ? '#7EC8E3' : '#D4A574');
  }
}

function drawCellOverlays(cx, cy, cell, mapQ, mapR) {
  if (cell.resource > 0) {
    if (cell.resource === 2 && forrageAnimalImgs.length > 0) {
      const v    = (mapQ * 31 + mapR * 17) % forrageAnimalImgs.length;
      const fImg = forrageAnimalImgs[v];
      if (fImg?.loaded) {
        const sz = HEX_SZ * 0.45;
        ctx.drawImage(fImg, cx - sz / 2, cy - sz / 2, sz, sz);
      } else {
        drawResourceIcon(ctx, cx, cy, HEX_SZ, cell.resource);
      }
    } else {
      drawResourceIcon(ctx, cx, cy, HEX_SZ, cell.resource);
    }
  }

  if (cell.shelter) drawShelterIcon(cx, cy, cell, mapQ, mapR);

  // Storm clump darkening — only hexes the moving squall line currently
  // covers go dark; deeper into a clump's core (and worse the phase) means
  // darker. Untouched hexes stay fully lit even while it's raining nearby.
  // Rain storm gets a neon-blue tint on top of the dark fill; chem storm
  // (phase 3) reuses the exact same clump mechanic with a radioactive-green
  // tint instead, fog (phase 4) reuses it again with its own slow travel
  // speed and a steep "pop" onset curve (see popIn in STORM_PHASE_CFG), and
  // mist (phase 5) reuses it once more with no tintColor at all and darkMax
  // near 1 — but mist alone renders as a soft radial wash (see cfg.soft
  // below) instead of a hex-clipped flat fill, so covered patches blend into
  // one fuzzy blob instead of a mosaic of crisp hexagon tiles.
  if (weatherPhase === 1 || weatherPhase === 2 || weatherPhase === 3 || weatherPhase === 4 || weatherPhase === 5) {
    const intensity = stormIntensityAt(mapQ, mapR, weatherPhase, weatherNow);
    if (intensity > 0.04) {
      const cfg = STORM_PHASE_CFG[weatherPhase];
      if (cfg.soft) {
        // Radius extends well past this hex's own edge so neighboring
        // covered hexes' gradients overlap and merge — no hard hexagon
        // boundary anywhere, unlike the clip+fillRect path below. A second
        // wash (tintRGB/tintAlpha) layers the same way as the flat path's
        // tintColor, for phases like Strangle Fog that need both a base
        // wash and a colored one, not just a single flat color like mist's.
        const r = HEX_SZ * 1.9;
        const drawWash = (rgb, alpha) => {
          const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, r);
          g.addColorStop(0,    `rgba(${rgb},${alpha})`);
          g.addColorStop(0.55, `rgba(${rgb},${alpha * 0.75})`);
          g.addColorStop(1,    `rgba(${rgb},0)`);
          ctx.fillStyle = g;
          ctx.beginPath();
          ctx.arc(cx, cy, r, 0, Math.PI * 2);
          ctx.fill();
        };
        ctx.save();
        drawWash(cfg.softRGB, intensity * cfg.darkMax);
        if (cfg.tintRGB) drawWash(cfg.tintRGB, intensity * cfg.tintAlpha);
        ctx.restore();
      } else {
        ctx.save();
        drawHexPath(ctx, cx, cy, HEX_SZ - 1);
        ctx.clip();
        ctx.globalAlpha = intensity * cfg.darkMax;
        ctx.fillStyle   = cfg.baseColor;
        ctx.fillRect(cx - HEX_SZ, cy - HEX_SZ, HEX_SZ * 2, HEX_SZ * 2);
        if (cfg.tintColor) {
          ctx.globalAlpha = intensity * cfg.tintAlpha;
          ctx.fillStyle   = cfg.tintColor;
          ctx.fillRect(cx - HEX_SZ, cy - HEX_SZ, HEX_SZ * 2, HEX_SZ * 2);
        }
        ctx.restore();
      }
      // Only rain/storm get the little raindrop glyph — chem gets its
      // radioactive pulses instead and fog/mist get their own particle
      // treatment (or none, for mist), neither of which reads as rain.
      if ((weatherPhase === 1 || weatherPhase === 2) && intensity > 0.3) {
        drawGlyph(ctx, GLYPH.RAIN, cx - HEX_SZ * 0.48, cy - HEX_SZ * 0.48,
                  Math.max(8, Math.round(HEX_SZ * 0.28)), '#FFF', 0.55 + 0.3 * intensity);
      }
      stormyHexesThisFrame.push({ x: cx, y: cy, spread: HEX_SZ, intensity });
    }
  }

  // Fire — layered on top of weather darkening so it stays visible even
  // during a storm. Two passes, in this order for a reason:
  //   1. CHAR: burning ground is *dark*. This used to be a flat orange wash,
  //      which brightened the whole hex uniformly and flattened it; darkening
  //      first is what gives the additive firelight above something to read
  //      against, and it also pre-stains the hex toward the Ash Dunes it
  //      becomes when the fire burns out (see world-system.hpp).
  //   2. EMBER BED: additive and flickering, so the hex breathes instead of
  //      sitting at one fixed alpha. Phase is per-hex (fireHexPhase, shared
  //      with the particle system) so a fire spreading across several hexes
  //      doesn't pulse in lockstep — that synchrony reads as one animated
  //      overlay rather than as several separate fires.
  // The fire itself is the flame/ember/smoke particles emitFire() spawns over
  // the anchor collected below — several separate seats scattered across the
  // hex, not one plume at its centre, since this is a top-down view of an area
  // alight (see fireSeats in weather-particle-system.js).
  if (fireField) {
    const fireIntensity = fireField.intensityAt(mapQ, mapR);
    if (fireIntensity > 0) {
      const BED_FILL = ['', '#C8400E', '#FF7A1E', '#FFC864'];
      const ph = fireHexPhase(mapQ, mapR);
      const ft = weatherNow * 0.001;
      // Three incommensurate rates (~0.8 / 2 / 4.7 Hz) — a real fire's light
      // is not periodic, and a single sine is immediately readable as fake.
      const flick = 0.70
        + 0.18 * Math.sin(ft * 5.1  + ph)
        + 0.09 * Math.sin(ft * 12.8 + ph * 1.7)
        + 0.05 * Math.sin(ft * 29.5 + ph * 2.9);
      ctx.save();
      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      ctx.clip();
      ctx.globalAlpha = 0.26 + fireIntensity * 0.07;
      ctx.fillStyle   = '#140A04';
      ctx.fillRect(cx - HEX_SZ, cy - HEX_SZ, HEX_SZ * 2, HEX_SZ * 2);
      ctx.globalCompositeOperation = 'lighter';
      ctx.globalAlpha = Math.max(0, (0.13 + fireIntensity * 0.07) * flick);
      ctx.fillStyle   = BED_FILL[fireIntensity] ?? '#FF7A1E';
      ctx.fillRect(cx - HEX_SZ, cy - HEX_SZ, HEX_SZ * 2, HEX_SZ * 2);
      ctx.restore();
      fireHexesThisFrame.push({ x: cx, y: cy, spread: HEX_SZ, intensity: fireIntensity, q: mapQ, r: mapR });
    }
  }
}

function applyHexFill(cell, dist, visible, surveyed, vr) {
  if (visible || surveyed) {
    ctx.globalAlpha = surveyed ? 0.7 : 1;
    ctx.fillStyle   = TERRAIN[cell.terrain]?.fill || '#2A2010';
  } else if (dist === vr + 1) {
    ctx.globalAlpha = FOG_INNER_ALPHA;
    ctx.fillStyle   = '#141008';
  } else if (dist === vr + 2) {
    ctx.globalAlpha = 0.92;
    ctx.fillStyle   = '#141008';
  } else {
    ctx.globalAlpha = 1;
    ctx.fillStyle   = '#080402';
  }
}

function renderHexContent(cx, cy, cell, mapQ, mapR, surveyed) {
  ctx.globalAlpha = surveyed ? 0.7 : 1;
  const _tv  = terrainImgVariants[cell.terrain];
  const tImg = poiArtFor(cell.terrain, cell.variant) ||
               (_tv?.length > 0 ? (_tv[cell.variant % _tv.length] || _tv[0]) : null);
  if (tImg?.loaded) {
    const imgSz = HEX_SZ * 2;
    ctx.drawImage(tImg, cx - imgSz / 2, cy - imgSz / 2, imgSz, imgSz);
  } else {
    if (cell.terrain !== 11) drawTerrainIcon(ctx, cx, cy, HEX_SZ, cell.terrain, cell.resource > 0);
    if (cell.terrain === 11) drawRiverRipples(cx, cy);
  }
  ctx.globalAlpha = 1;
  drawFootprints(cx, cy, cell);
  drawCellOverlays(cx, cy, cell, mapQ, mapR);
}

// ── Pass 1: Hex fills + terrain icons + resources ─────────────────
function renderHexTerrain(cam) {
  const { ox, oy, centreQ, centreR, viewQ, viewR, meAct } = cam;
  const effectiveVR = getEffectiveVR();
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq   = centreQ + dq;
      const vr   = centreR + dr;
      const mapQ = ((vq % MAP_COLS) + MAP_COLS) % MAP_COLS;
      const mapR = ((vr % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;

      const px = hexToPixel(vq, vr, HEX_SZ);
      const cx = px.x + ox;
      const cy = px.y + oy;

      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;

      const dist     = hexDistWrap(meAct.q, meAct.r, mapQ, mapR);
      const cell     = gameMap[mapR][mapQ];
      const visible  = dist <= effectiveVR && cell !== null;
      const surveyed = !visible && cell !== null && surveyedCells.has(`${mapQ}_${mapR}`); // NOSONAR S4158 — populated in network.js

      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      applyHexFill(cell, dist, visible, surveyed, effectiveVR);
      ctx.fill();
      ctx.globalAlpha = 1;

      if (visible || surveyed) {
        let ccx = cx, ccy = cy;
        const qinfo = quakeField?.cellInfo(mapQ, mapR);
        if (qinfo) {
          let pts = quakePixelsThisFrame.get(qinfo.id);
          if (!pts) { pts = []; quakePixelsThisFrame.set(qinfo.id, pts); }
          pts[qinfo.order] = { x: cx, y: cy }; // stable, unjittered — crack path anchor
          if (qinfo.converted) quakeConvertedPixelsThisFrame.push({ x: cx, y: cy });
          const mag = qinfo.env * HEX_SZ * 0.1;
          ccx += (Math.random() - 0.5) * mag;
          ccy += (Math.random() - 0.5) * mag;
        }
        renderHexContent(ccx, ccy, cell, mapQ, mapR, surveyed);
      }
    }
  }
}

// ── Pass 1b: Hex grid lines (drawn on top of terrain PNGs) ────────
function renderGridLines(cam) {
  const { ox, oy, centreQ, centreR, viewQ, viewR } = cam;
  ctx.strokeStyle = 'rgba(50,50,50,0.5)';
  ctx.lineWidth   = 1;
  ctx.globalAlpha = 1;
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq = centreQ + dq;
      const vr = centreR + dr;
      const px = hexToPixel(vq, vr, HEX_SZ);
      const cx = px.x + ox;
      const cy = px.y + oy;
      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;
      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      ctx.stroke();
    }
  }
}

function isPOIRenderable(mapQ, mapR, meAct) {
  const cell = gameMap[mapR]?.[mapQ];
  if (!cell?.poi) return false;
  const dist = hexDistWrap(meAct.q, meAct.r, mapQ, mapR);
  const effectiveVR = getEffectiveVR();
  return dist <= effectiveVR || surveyedCells.has(`${mapQ}_${mapR}`); // NOSONAR S4158
}

// ── Pass 1c: Faint per-hex integer label (top-inside-border) ─────
function renderHexLabels(cam) {
  const { ox, oy, centreQ, centreR, viewQ, viewR } = cam;
  const fontPx = Math.max(8, Math.round(HEX_SZ * 0.18));
  ctx.save();
  ctx.font         = `${fontPx}px sans-serif`;
  ctx.textAlign    = 'center';
  ctx.textBaseline = 'top';
  ctx.fillStyle    = 'rgba(160,160,160,0.5)';
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq   = centreQ + dq;
      const vr   = centreR + dr;
      const mapQ = ((vq % MAP_COLS) + MAP_COLS) % MAP_COLS;
      const mapR = ((vr % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
      const px = hexToPixel(vq, vr, HEX_SZ);
      const cx = px.x + ox;
      const cy = px.y + oy;
      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;
      const ty = cy - HEX_SZ * 0.78;
      ctx.fillText(hexLabel[mapR * MAP_COLS + mapQ], cx, ty);
    }
  }
  ctx.restore();
}

// ── POI hex outline (yellow, pulsing, visible cells only) ──────────
function renderPOIOutlines(cam) {
  const { ox, oy, centreQ, centreR, viewQ, viewR, meAct } = cam;
  const _poiAlpha = 0.7 + 0.2 * Math.sin(performance.now() / 400);
  ctx.strokeStyle = `rgba(255,210,0,${_poiAlpha})`;
  ctx.lineWidth   = 3;
  ctx.globalAlpha = 1;
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq   = centreQ + dq;
      const vr   = centreR + dr;
      const mapQ = ((vq % MAP_COLS) + MAP_COLS) % MAP_COLS;
      const mapR = ((vr % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
      const px   = hexToPixel(vq, vr, HEX_SZ);
      const cx   = px.x + ox;
      const cy   = px.y + oy;
      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;
      if (!isPOIRenderable(mapQ, mapR, meAct)) continue;
      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      ctx.stroke();
    }
  }
}

// ── Current hex outline highlight ─────────────────────────────────
function renderCurrentHex(cam) {
  if (myId < 0 || !players[myId]?.on) return;
  const { ox, oy, meAct } = cam;
  const mePx = hexToPixel(meAct.q, meAct.r, HEX_SZ);
  drawHexPath(ctx, mePx.x + ox, mePx.y + oy, HEX_SZ - 1);
  ctx.globalAlpha = 1;
  ctx.strokeStyle = 'rgba(90,90,90,0.5)';
  ctx.lineWidth   = 1;
  ctx.stroke();
}

function closestWrapCoords(rp, meRp) {
  let vq = rp.q, vr = rp.r, bestD = Infinity;
  for (let dq2 = -1; dq2 <= 1; dq2++) {
    for (let dr2 = -1; dr2 <= 1; dr2++) {
      const d = hexDist(meRp.q, meRp.r, rp.q + dq2 * MAP_COLS, rp.r + dr2 * MAP_ROWS);
      if (d < bestD) { bestD = d; vq = rp.q + dq2 * MAP_COLS; vr = rp.r + dr2 * MAP_ROWS; }
    }
  }
  return { vq, vr };
}

// ── Pass 2: Character icons ────────────────────────────────────────
function renderCharacters(cam) {
  const { ox, oy, meRp } = cam;

  // Group active players by their snapped hex key so co-located players
  // can be spread into a small cluster instead of stacking on one point.
  const hexGroups = new Map(); // "q_r" -> [playerIndex, ...]
  for (let i = 0; i < MAX_PLAYERS; i++) {
    if (!players[i].on) continue;
    const { vq, vr } = closestWrapCoords(renderPos[i], meRp);
    const key = `${Math.round(vq)}_${Math.round(vr)}`;
    if (!hexGroups.has(key)) hexGroups.set(key, []);
    hexGroups.get(key).push(i);
  }

  for (let i = 0; i < MAX_PLAYERS; i++) {
    const p = players[i];
    if (!p.on) continue;

    const { vq, vr } = closestWrapCoords(renderPos[i], meRp);
    const pp  = hexToPixel(vq, vr, HEX_SZ);
    const pcx = pp.x + ox;
    const pcy = pp.y + oy;
    if (pcx < -HEX_SZ * 2 || pcx > cssWidth  + HEX_SZ * 2) continue;
    if (pcy < -HEX_SZ * 2 || pcy > cssHeight + HEX_SZ * 2) continue;

    // Cluster offset: solo player stays centred; 2+ players spread in a ring.
    const key     = `${Math.round(vq)}_${Math.round(vr)}`;
    const group   = hexGroups.get(key);
    const n       = group.length;
    const idx     = group.indexOf(i);
    const clusterR = HEX_SZ * 0.38;  // ring radius as fraction of hex size
    const angle   = (idx / n) * Math.PI * 2; // right-first (horizontal)
    const offX    = n > 1 ? Math.cos(angle) * clusterR : 0;
    const offY    = n > 1 ? Math.sin(angle) * clusterR : 0;

    const _archColor = ARCHETYPE_COLORS[p.arch ?? 0] ?? PLAYER_COLORS[i];
    drawCharIcon(ctx, pcx + offX, pcy + offY, HEX_SZ, {
      color: _archColor,
      label: i,
      isMe:  i === myId,
      nm:    p.nm,
      arch:  p.arch ?? 0,
      sc:    p.sc   ?? 0,
    });
  }
}

// ── Pass 2.5: Weather overlay + particles ─────────────────────────
// Hex darkening for every phase already happened per-hex during the terrain
// pass (drawCellOverlays) — this just spawns rain/fog/radioactive pulses over
// the storm hexes it found, and occasionally fires lightning: a sky-strike
// for rain storms, a hex-to-hex arc for chem storms, or a little faint
// static-energy flicker for creeping fog. Mist (phase 5) gets none of this —
// just the flat opaque hex fill from drawCellOverlays(), no particles at all.
function renderWeatherOverlay() {
  if (weatherPhase <= 0) return;

  const cfg     = STORM_PHASE_CFG[weatherPhase];
  const anchors = stormyHexesThisFrame;
  if (weatherParticles) {
    if (weatherPhase === 4) {
      // Fog has its own particle look entirely (heavy drifting puffs + rare
      // faint sparks) rather than the rain-line/radioactive-circle emit()
      // used by rain/storm/chem.
      weatherParticles.emitCreepingFog(0.16, anchors);
      if (cfg.staticBursts) weatherParticles.emitStatic(0.03, anchors);
    } else if (weatherPhase !== 5) {
      const count = weatherPhase === 1 ? 2 : weatherPhase === 2 ? 4 : 5;
      weatherParticles.emit(count, weatherPhase, cssWidth, cssHeight, anchors);
      if (weatherPhase === 1 || weatherPhase === 2) {
        weatherParticles.emitFog(weatherPhase === 1 ? 0.06 : 0.1, anchors);
      }
    }
  }
  if (cfg.lightning && lightningSystem) {
    lightningSystem.maybeStrike(anchors, weatherNow, LIGHTNING_MIN_GAP_MS, LIGHTNING_STRIKE_CHANCE);
  }
  if (cfg.arcLightning && lightningSystem) {
    lightningSystem.maybeArc(anchors, weatherNow, ARC_MIN_GAP_MS, ARC_STRIKE_CHANCE);
  }

  if (lightningSystem) {
    lightningSystem.update(weatherNow);
    lightningSystem.render(ctx, weatherNow, HEX_SZ, cssWidth, cssHeight);
  }
}

// ── Pass 2.5: Quake overlay — fault-line dust ─────────────────────
// Per-hex shake jitter and the whole-view camera shake already happened
// earlier (renderHexTerrain / buildCamera); this just kicks up dust along
// the fault line. Individual dust motes outlive the quake itself (long ttl
// in _spawnDust) so the cloud keeps drifting and dissipating for a couple
// seconds after the shaking has already stopped.
// NOTE: weatherParticles.update()/render() are NOT called from here or from
// renderWeatherOverlay() — a quake can happen in clear weather, so ticking
// the shared particle pool must not depend on weatherPhase being active.
// See the 'weather_particles' layer below.
function renderQuakeOverlay() {
  if (!quakeField || !quakeField.quakes.length || !weatherParticles) return;
  for (const quake of quakeField.quakes) {
    if (quake.env <= 0.02) continue;
    const pts = quakePixelsThisFrame.get(quake.id);
    if (!pts) continue;
    const anchors = pts.filter(Boolean).map(p => ({ x: p.x, y: p.y, spread: HEX_SZ }));
    for (let i = 0; i < 2; i++) weatherParticles.emitDust(QUAKE_DUST_CHANCE * quake.env, anchors);
  }
  // Extra, heavier dust specifically over hexes the quake leveled from
  // Settlement to Open Scrub — on top of the regular fault-line dust above.
  if (quakeConvertedPixelsThisFrame.length) {
    const convertedAnchors = quakeConvertedPixelsThisFrame.map(p => ({ x: p.x, y: p.y, spread: HEX_SZ }));
    for (let i = 0; i < 6; i++) weatherParticles.emitDust(QUAKE_CONVERTED_DUST_CHANCE, convertedAnchors);
  }
}

// ── Time-of-day tint overlay ──────────────────────────────────────
function renderTimeOfDay() {
  // Lerp displayMP: fast toward uiMP.val normally; slow drift to 0 while resting.
  // Exception: if nightFade > 0 a dawn event just fired — don't lerp toward 0 (BUG-13).
  // nightFade > 0 means dawn already happened; trust displayMP (snapped at dawn) not resting flag.
  const stillResting = uiResting.val && nightFade <= 0;
  const mpTarget = stillResting ? 0 : uiMP.val;
  const lerpRate = stillResting ? RESTING_LERP_RATE : MOVING_LERP_RATE;
  displayMP += (mpTarget - displayMP) * lerpRate;
  const phase = getTimePhase(displayMP);
  ctx.save();
  ctx.globalAlpha = phase.a;
  ctx.fillStyle   = `rgb(${phase.r},${phase.g},${phase.b})`;
  ctx.fillRect(0, 0, cssWidth, cssHeight);
  ctx.restore();
}

// ── Night linger fade (dawn transition) ──────────────────────────
function renderNightFade() {
  if (nightFade <= 0) return;
  ctx.save();
  ctx.globalAlpha = nightFade;
  ctx.fillStyle = 'rgb(15,8,40)';
  ctx.fillRect(0, 0, cssWidth, cssHeight);
  ctx.restore();
  nightFade = Math.max(0, nightFade - NIGHT_FADE_DECAY_RATE); // ~3 s at 60fps
}

// ── Render layers (drawn in order, bottom-to-top) ────────────────
// To disable a layer temporarily: add  enabled: false.
// To add a new layer: push a { name, draw } entry at the right position.
const LAYERS = [
  { name: 'terrain',      draw: (cam) => renderHexTerrain(cam) },
  { name: 'grid',         draw: (cam) => renderGridLines(cam) },
  { name: 'hex_labels',   draw: (cam) => renderHexLabels(cam) },
  { name: 'poi_outlines', draw: (cam) => renderPOIOutlines(cam) },
  { name: 'current_hex',  draw: (cam) => renderCurrentHex(cam) },
  { name: 'characters',   draw: (cam) => renderCharacters(cam) },
  { name: 'caravan',      draw: (cam) => renderCaravan(cam) },
  { name: 'doom',         draw: (cam) => renderDoom(cam) },
  { name: 'weather',      draw: (_)   => renderWeatherOverlay() },
  { name: 'quake',        draw: (_)   => renderQuakeOverlay() },
  // Ticks unconditionally — shared by weather (gated above) and quake dust
  // (not weather-gated), so it must run regardless of weatherPhase.
  // Fire particles emit here too (not weather-gated — a fire burns
  // regardless of weatherPhase) so they share one update/render pass.
  // renderFireGlow() runs first so the firelight pool lands *under* the licks;
  // it deliberately spills over characters standing on the hex, which lights
  // them by the fire rather than leaving them flatly lit inside it.
  { name: 'weather_particles', draw: (_) => { if (weatherParticles) { weatherParticles.renderFireGlow(ctx, fireHexesThisFrame); weatherParticles.emitFire(fireHexesThisFrame); weatherParticles.update(); weatherParticles.render(ctx); } } },
  { name: 'ash',          draw: (cam) => { if (ashParticles) { ashParticles.update(gameMap, HEX_SZ); ashParticles.render(ctx, cam.ox, cam.oy, HEX_SZ); } } },
  { name: 'time_of_day',  draw: (_)   => renderTimeOfDay() },
  { name: 'night_fade',   draw: (_)   => renderNightFade() },
];

// ── Render ──────────────────────────────────────────────────────
function render() {
  ctx.clearRect(0, 0, cssWidth, cssHeight);
  ctx.fillStyle = '#050301';
  ctx.fillRect(0, 0, cssWidth, cssHeight);

  weatherNow = Date.now();
  stormyHexesThisFrame = [];
  quakePixelsThisFrame = new Map();
  quakeConvertedPixelsThisFrame = [];
  fireHexesThisFrame = [];
  if (quakeField) quakeField.update(weatherNow);

  lerpPlayerPositions();
  const cam = buildCamera();

  for (const layer of LAYERS) {
    if (layer.enabled !== false) layer.draw(cam);
  }

  requestAnimationFrame(render);
}
