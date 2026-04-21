// ── Weather particle system ───────────────────────────────────────
const weatherParticles = (typeof WeatherParticleSystem !== 'undefined')
  ? new WeatherParticleSystem() : null;

// ── Smooth animation state ────────────────────────────────────────
const renderPos = Array.from({ length: MAX_PLAYERS }, () => ({ q: 0.0, r: 0.0 }));

// ── Name-tag width cache ──────────────────────────────────────────
// Avoids a ctx.measureText() call every frame per visible player.
// Keyed by player index; invalidated when tag text or font size changes.
const nameWidthCache = new Array(MAX_PLAYERS).fill(null);

// ── Surveyed cells ────────────────────────────────────────────────
// Cells revealed by the SURVEY action beyond normal vision radius.
// Rendered at reduced opacity as scouted-but-not-directly-visible.
// Cleared whenever the local player moves.
const surveyedCells = new Set(); // 'q_r' keys

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

/**
 * Recalculate hex size and canvas resolution based on viewport size.
 * Scales the canvas buffer by devicePixelRatio for crisp rendering on
 * HiDPI / Retina screens while keeping all draw coordinates in CSS pixels.
 */
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
  HEX_SZ = Math.max(HEX_SZ_MIN, Math.min(HEX_SZ_MAX, Math.floor(cssWidth / HEX_SZ_VIEWPORT_DIVISOR)));
}
window.addEventListener('resize', resize);
resize();

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

  ctx.save();
  ctx.globalAlpha  = hasResource ? 0.45 : 0.70;
  ctx.font         = `${sz}px serif`;
  ctx.textAlign    = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle    = '#FFF';
  ctx.fillText(t.icon, cx, cy + offY);
  ctx.restore();
}

/**
 * Draw character/player icon on the hex grid.
 * Includes head, torso, name label, and ground shadow.
 * @param {CanvasRenderingContext2D} ctx - Canvas context
 * @param {number} cx - Center X coordinate
 * @param {number} cy - Center Y coordinate
 * @param {number} hexSz - Hex size in pixels
 * @param {string} color - Character color (CSS color string)
 * @param {string} label - Character label (1-2 chars)
 * @param {boolean} isMe - Whether this is the current player (affects glow)
 * @param {string} nm - Character name for label tooltip
 */
function drawCharIcon(ctx, cx, cy, hexSz, color, label, isMe, nm, arch, sc) {
  const scale     = hexSz * ICON_SIZE_SCALE;
  const r         = Math.max(10, hexSz * 0.28);   // portrait circle radius
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
    const tag      = nm.substring(0, 8).toUpperCase();
    const topY     = portraitCY - r - 4;
    ctx.save();
    ctx.font         = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'bottom';
    ctx.fillStyle    = '#000';
    ctx.letterSpacing = '1px';
    ctx.fillText(tag, cx, topY);
    ctx.restore();
  }

  // Score below circle
  if (sc !== undefined && sc !== null) {
    const scoreStr = String(sc);
    const botY     = portraitCY + r + nameSz + 6;
    ctx.save();
    ctx.font         = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'bottom';
    ctx.fillStyle    = '#000';
    ctx.letterSpacing = '1px';
    ctx.fillText(scoreStr, cx, botY);
    ctx.restore();
  }
}

// ── Time-of-day clock ────────────────────────────────────────────
const TIME_PHASES = [
  { name:'FIRST LIGHT', icon:'☀',      r:255, g:180, b:100, a:0.03 },
  { name:'HIGH WATCH',  icon:'☀',      r:255, g:220, b:150, a:0.01 },
  { name:'NOON BURN',   icon:'☀',      r:255, g:240, b:200, a:0.00 },
  { name:'LOW SUN',     icon:'🌇', r:255, g:160, b: 60, a:0.06 },
  { name:'DUST HOUR',   icon:'🌆', r:210, g: 90, b: 20, a:0.14 },
  { name:'DARK WATCH',  icon:'🌑', r: 15, g:  8, b: 40, a:0.72 },
];
function getTimePhase(mpVal) {
  const _mp = (mpVal !== undefined) ? mpVal : uiMP.val;
  if (myId < 0 || maxMP <= 0) return TIME_PHASES[0];
  const f = Math.max(0, Math.min(1, 1 - _mp / maxMP));
  const scaled = f * (TIME_PHASES.length - 1);
  const i = Math.floor(scaled);
  const t = scaled - i;
  const p0 = TIME_PHASES[Math.min(i, TIME_PHASES.length - 1)];
  const p1 = TIME_PHASES[Math.min(i + 1, TIME_PHASES.length - 1)];
  return {
    icon: p0.icon, name: p0.name,
    r: Math.round(p0.r + t * (p1.r - p0.r)),
    g: Math.round(p0.g + t * (p1.g - p0.g)),
    b: Math.round(p0.b + t * (p1.b - p0.b)),
    a: p0.a + t * (p1.a - p0.a)
  };
}

// ── Ash particle system (Pass 3) ────────────────────────────────
const ashParticles = (typeof AshParticleSystem !== 'undefined') ? new AshParticleSystem({
  maxAlpha:         0.52,   // peak opacity at spawn point
  driftSpeed:       0.04,   // px/ms base speed (randomized ±)
  wobbleAmp:        8,      // px of sine-wave wobble layered on drift
  wobbleFreq:       0.0007, // wobble sine frequency (rad/ms)
  maxDistanceHexes: 3,      // hex-distance at which particle is garbage collected
  countPerTerrain:  2,      // target active particles per ash/mountain hex
  radiusMin:        1.5,    // min gradient radius as multiple of HEX_SZ
  radiusMax:        3.0,    // max gradient radius as multiple of HEX_SZ
}) : null;

// ── Render ──────────────────────────────────────────────────────
function render() {
  ctx.clearRect(0, 0, cssWidth, cssHeight);
  ctx.fillStyle = '#050301';
  ctx.fillRect(0, 0, cssWidth, cssHeight);

  // Lerp all player render positions
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

  // Camera: centre on my smooth position
  const meRp = (myId >= 0) ? renderPos[myId] : { q: 0, r: 0 };
  const cp   = hexToPixel(meRp.q, meRp.r, HEX_SZ);
  const ox   = cssWidth  / 2 - cp.x;
  const oy   = cssHeight / 2 - cp.y;

  // Fog uses the ACTUAL integer position (accurate fog boundary)
  const meAct = (myId >= 0) ? players[myId] : { q: 0, r: 0 };

  const viewQ   = Math.ceil(cssWidth  / (HEX_SZ * 1.5)) + 2;
  const viewR   = Math.ceil(cssHeight / (HEX_SZ * SQRT3)) + 2;
  const centreQ = Math.round(meRp.q);
  const centreR = Math.round(meRp.r);

  // ── Pass 1: Hex fills + terrain icons + resources ─────────────
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
      const visible  = dist <= myVisionR && cell !== null;
      // Surveyed: beyond normal vision but revealed by SURVEY action this turn.
      const surveyed = !visible && cell !== null && surveyedCells.has(`${mapQ}_${mapR}`);

      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      if (visible) {
        ctx.globalAlpha = 1;
        const t = TERRAIN[cell.terrain];
        ctx.fillStyle   = t?.fill || '#2A2010';
      } else if (surveyed) {
        ctx.globalAlpha = 0.7;   // scouted — terrain visible but dimmed
        const t = TERRAIN[cell.terrain];
        ctx.fillStyle   = t?.fill || '#2A2010';
      } else if (dist === myVisionR + 1) {
        ctx.globalAlpha = FOG_INNER_ALPHA;  // inner fog ring — more visible
        ctx.fillStyle   = '#141008';
      } else if (dist === myVisionR + 2) {
        ctx.globalAlpha = 0.92;  // outer fog ring — subtly transparent
        ctx.fillStyle   = '#141008';
      } else {
        ctx.globalAlpha = 1;     // deep fog — fully opaque
        ctx.fillStyle   = '#080402';
      }
      ctx.fill();
      ctx.globalAlpha = 1;

      if (!visible && !surveyed) continue;

      // Surveyed cells draw terrain at reduced opacity; fully visible cells at full opacity.
      ctx.globalAlpha = surveyed ? 0.7 : 1;

      // Terrain hex image (if loaded) — alpha channel handles hex shape, no clipping
      const _tv  = terrainImgVariants[cell.terrain];
      const tImg = (_tv && _tv.length > 0) ? (_tv[cell.variant % _tv.length] || _tv[0]) : null;
      if (tImg && tImg.loaded) {
        const imgSz = HEX_SZ * 2;
        ctx.drawImage(tImg, cx - imgSz / 2, cy - imgSz / 2, imgSz, imgSz);
      }

      // Terrain icon (no resource indicator) — only when no image
      if (!tImg || !tImg.loaded) {
        if (cell.terrain !== 11) drawTerrainIcon(ctx, cx, cy, HEX_SZ, cell.terrain, false);
      }

      // ── River ripples (terrain 11) ────────────────────────────────
      // Animated concentric ellipses using the RAF timestamp for smooth motion.
      if (cell.terrain === 11 && (!tImg || !tImg.loaded)) {
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

      // Reset alpha — footprints/shelter always render at full opacity.
      ctx.globalAlpha = 1;

      // Footprints: dark brown 👣 icon per player that has visited this hex.
      if (cell.footprints > 0) {
        const footprintSize = Math.max(6, Math.round(HEX_SZ * 0.22));
        let footprintCount = 0;
        for (let i = 0; i < 6; i++) if (cell.footprints & (1 << i)) footprintCount++;
        let footprintIdx = 0;
        for (let fpid = 0; fpid < 6; fpid++) {
          if ((cell.footprints & (1 << fpid)) === 0) continue;
          const angle  = (footprintIdx * Math.PI * 2) / Math.max(1, footprintCount);
          const radius = HEX_SZ * FOOTPRINT_RING_RADIUS;
          const fx = cx + Math.cos(angle) * radius;
          const fy = cy + Math.sin(angle) * radius;
          ctx.save();
          ctx.filter       = 'sepia(1) saturate(0.5) brightness(0.35)';
          ctx.globalAlpha  = 0.75;
          ctx.font         = `${footprintSize}px monospace`;
          ctx.textAlign    = 'center';
          ctx.textBaseline = 'middle';
          ctx.fillText('👣', fx, fy);
          ctx.restore();
          footprintIdx++;
        }
      }

      // Food resource — forage animal PNG centred on hex (resource type 2 = food)
      if (cell.resource === 2 && forrageAnimalImgs.length > 0) {
        const v    = (mapQ * 31 + mapR * 17) % forrageAnimalImgs.length;
        const fImg = forrageAnimalImgs[v];
        if (fImg && fImg.loaded) {
          const sz = HEX_SZ * 0.45;
          ctx.drawImage(fImg, cx - sz / 2, cy - sz / 2, sz, sz);
        }
      }


      // Shelter indicator: 1=has shelter — PNG centred on hex, emoji fallback
      if (cell.shelter) {
        const si   = 0;  // basic shelter (bit 6 is now boolean)
        const imgs = shelterImgs[si];
        const v    = imgs.length > 0 ? (mapQ * 31 + mapR * 17) % imgs.length : -1;
        const sImg = v >= 0 ? imgs[v] : null;
        if (sImg && sImg.loaded) {
          const sz = HEX_SZ * 0.9;
          ctx.drawImage(sImg, cx - sz / 2, cy - sz / 2, sz, sz);
        } else {
          ctx.save();
          ctx.fillStyle    = cell.shelter === 2 ? '#7EC8E3' : '#D4A574';
          ctx.font         = `${Math.max(12, Math.round(HEX_SZ * 0.5))}px monospace`;
          ctx.textAlign    = 'right';
          ctx.textBaseline = 'top';
          ctx.fillText(cell.shelter === 2 ? '🏠' : '⛺', cx + HEX_SZ * 0.35, cy - HEX_SZ * 0.35);
          ctx.restore();
        }
      }

      // Weather icon — small 🌧 in upper-left corner of each visible hex
      if (weatherPhase > 0) {
        ctx.save();
        ctx.font         = `${Math.max(8, Math.round(HEX_SZ * 0.28))}px serif`;
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'top';
        ctx.globalAlpha  = 0.75;
        ctx.fillText('🌧', cx - HEX_SZ * 0.48, cy - HEX_SZ * 0.48);
        ctx.restore();
      }
    }
  }

  // ── Pass 1b: Hex grid lines (drawn on top of terrain PNGs) ──────────
  ctx.strokeStyle = 'rgba(50,50,50,0.5)';
  ctx.lineWidth   = 1.0;
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

  // ── POI hex outline (yellow, pulsing, visible cells only) ──
  const _poiAlpha = 0.7 + 0.2 * Math.sin(performance.now() / 400);
  ctx.strokeStyle = `rgba(255,210,0,${_poiAlpha})`;
  ctx.lineWidth   = 3.0;
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
      const cell = gameMap[mapR]?.[mapQ];
      if (!cell?.poi) continue;
      const dist2 = hexDistWrap(meAct.q, meAct.r, mapQ, mapR);
      if (dist2 > myVisionR && !surveyedCells.has(`${mapQ}_${mapR}`)) continue;
      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      ctx.stroke();
    }
  }

  // ── Current hex outline highlight ────────────────────────────────
  if (myId >= 0 && players[myId]?.on) {
    const mePx = hexToPixel(meAct.q, meAct.r, HEX_SZ);
    drawHexPath(ctx, mePx.x + ox, mePx.y + oy, HEX_SZ - 1);
    ctx.globalAlpha = 1;
    ctx.strokeStyle = 'rgba(90,90,90,0.5)';
    ctx.lineWidth   = 1.0;
    ctx.stroke();
  }

  // ── Pass 2: Character icons ────────────────────────────────────
  for (let i = 0; i < MAX_PLAYERS; i++) {
    const p = players[i];
    if (!p.on) continue;

    const rp = renderPos[i];
    let vq = rp.q, vr = rp.r;
    let bestD = Infinity;
    for (let dq2 = -1; dq2 <= 1; dq2++) {
      for (let dr2 = -1; dr2 <= 1; dr2++) {
        const d = hexDist(meRp.q, meRp.r,
                          rp.q + dq2 * MAP_COLS, rp.r + dr2 * MAP_ROWS);
        if (d < bestD) { bestD = d; vq = rp.q + dq2 * MAP_COLS; vr = rp.r + dr2 * MAP_ROWS; }
      }
    }

    const pp  = hexToPixel(vq, vr, HEX_SZ);
    const pcx = pp.x + ox;
    const pcy = pp.y + oy;
    if (pcx < -HEX_SZ * 2 || pcx > cssWidth  + HEX_SZ * 2) continue;
    if (pcy < -HEX_SZ * 2 || pcy > cssHeight + HEX_SZ * 2) continue;

    const _archColor = ARCHETYPE_COLORS[players[i].arch ?? 0] ?? PLAYER_COLORS[i];
    drawCharIcon(ctx, pcx, pcy, HEX_SZ, _archColor, i, i === myId,
                 players[i].nm, players[i].arch ?? 0, players[i].sc ?? 0);
  }

  // ── Pass 2.5: Weather overlay + particles ────────────────────
  if (weatherPhase > 0) {
    const wNow = Date.now();
    // Helper: draw many short individual rain drops falling downward.
    // Each drop is a short 1px line at a slight angle; opacity varies per drop
    // using a golden-angle seed so the variation is distributed evenly, not banded.
    function drawRainDrops(count, dropLen, sinA, color, speedMs) {
      const cosA    = Math.sqrt(Math.max(0, 1 - sinA * sinA));
      const cycle   = cssHeight + dropLen;
      const scrollY = (wNow / speedMs * cycle) % cycle;
      ctx.save();
      ctx.strokeStyle = color;
      ctx.lineWidth   = 1;
      for (let i = 0; i < count; i++) {
        const seed = i * 137.508;                           // golden-angle spread
        const x    = seed % cssWidth;
        const y    = ((seed * 0.618 + scrollY) % cycle) - dropLen;
        ctx.globalAlpha = 0.18 + (i % 13) / 13 * 0.50;    // vary 0.18–0.68 per drop
        ctx.beginPath();
        ctx.moveTo(x,               y);
        ctx.lineTo(x + dropLen * sinA, y + dropLen * cosA);
        ctx.stroke();
      }
      ctx.restore();
    }

    if (weatherPhase === 1) {
      // Light blue-gray wash
      ctx.save(); ctx.globalAlpha = 0.09; ctx.fillStyle = '#667799';
      ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      // 60 drops, 12px long, slight angle, 1.8s to cross the screen
      drawRainDrops(60, 12, 0.18, '#AACCEE', 1800);
      if (weatherParticles) weatherParticles.emit(2, 1, cssWidth, cssHeight);

    } else if (weatherPhase === 2) {
      // Heavier dark overlay
      ctx.save(); ctx.globalAlpha = 0.18; ctx.fillStyle = '#334455';
      ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      // 130 drops, 20px long, steeper angle, 1.0s to cross — heavier & faster
      drawRainDrops(130, 20, 0.28, '#7799BB', 1000);
      // Lightning (3 s cycle: 0–100ms flash, 100–150ms dark, 150–200ms secondary)
      const lc = wNow % 3000;
      if (lc < 100) {
        ctx.save(); ctx.globalAlpha = Math.sin(lc / 100 * Math.PI) * 0.55;
        ctx.fillStyle = '#DDEEFF'; ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      } else if (lc >= 150 && lc < 200) {
        ctx.save(); ctx.globalAlpha = (1 - (lc - 150) / 50) * 0.25;
        ctx.fillStyle = '#BBCCEE'; ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      }
      if (weatherParticles) weatherParticles.emit(4, 2, cssWidth, cssHeight);

    } else {
      // Chem-Storm: sickly green radial gradient
      const cx2 = cssWidth / 2, cy2 = cssHeight / 2;
      const grad = ctx.createRadialGradient(cx2, cy2, 0, cx2, cy2,
        Math.max(cssWidth, cssHeight) * 0.8);
      grad.addColorStop(0,   'rgba(60,120,20,0.18)');
      grad.addColorStop(0.6, 'rgba(30,80,10,0.10)');
      grad.addColorStop(1,   'rgba(0,0,0,0)');
      ctx.save(); ctx.fillStyle = grad;
      ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      // Radiation pulse (4 s cycle)
      const rc = wNow % 4000;
      let pa = 0;
      if      (rc < 100)  pa = (rc / 100)          * 0.12;
      else if (rc < 150)  pa = ((150 - rc) / 50)   * 0.06;
      else if (rc < 220)  pa = ((rc - 150) / 70)   * 0.12;
      else if (rc < 2500) pa = 0;
      else if (rc < 2650) pa = ((rc - 2500) / 150) * 0.20;
      else                pa = ((4000 - rc) / 1350) * 0.20;
      if (pa > 0) {
        ctx.save(); ctx.globalAlpha = pa; ctx.fillStyle = '#44BB22';
        ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      }
      if (weatherParticles) weatherParticles.emit(6, 3, cssWidth, cssHeight);
    }
    if (weatherParticles) { weatherParticles.update(); weatherParticles.render(ctx); }
  }

  // ── Pass 3: Ash particle overlay ─────────────────────────────
  if (ashParticles) {
    ashParticles.update(gameMap, HEX_SZ);
    ashParticles.render(ctx, ox, oy, HEX_SZ);
  }

  // ── Time-of-day tint overlay ──────────────────────────────────
  {
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

  // ── Night linger fade (dawn transition) ──────────────────────
  if (nightFade > 0) {
    ctx.save();
    ctx.globalAlpha = nightFade;
    ctx.fillStyle = 'rgb(15,8,40)';
    ctx.fillRect(0, 0, cssWidth, cssHeight);
    ctx.restore();
    nightFade = Math.max(0, nightFade - NIGHT_FADE_DECAY_RATE); // ~3 s at 60fps
  }

  requestAnimationFrame(render);
}
