// ── world-entities.js ──────────────────────────────────────────────
// Renders the world-system's mobile NPCs: Caravan and Creeping Doom (see
// docs/world-system-spec.md). State comes from worldState (engine.js),
// populated by _applyWorldState() in network.js from the server's periodic
// "world" sync/state key — sent unfiltered to every client regardless of
// fog of war, same as the caravan already was: both are broadly-telegraphed
// world threats/landmarks, not something hidden until explored (matches
// EVT_DOOM_WARNING's own "sent to all connected players regardless of
// position" framing in the spec).
//
// Position lerps the same way player positions do (lerpPlayerPositions() in
// renderer.js) so travel between world-tick syncs (~15s apart server-side)
// reads as smooth movement instead of a teleport. Kept in its own render-pos
// state (not renderPos[], which is sized/indexed for players) since nothing
// else needs to read the caravan's interpolated position.

const caravanRenderPos = { q: 0, r: 0 };
let caravanRenderPosLive = false;

function lerpCaravanPosition() {
  const c = worldState.caravan;
  if (!c || !c.active) { caravanRenderPosLive = false; return; }
  if (!caravanRenderPosLive) {
    caravanRenderPos.q = c.q;
    caravanRenderPos.r = c.r;
    caravanRenderPosLive = true;
    return;
  }
  let tq = c.q, tr = c.r;
  while (tq - caravanRenderPos.q >  MAP_COLS / 2) tq -= MAP_COLS;
  while (caravanRenderPos.q - tq >  MAP_COLS / 2) tq += MAP_COLS;
  while (tr - caravanRenderPos.r >  MAP_ROWS / 2) tr -= MAP_ROWS;
  while (caravanRenderPos.r - tr >  MAP_ROWS / 2) tr += MAP_ROWS;
  caravanRenderPos.q += (tq - caravanRenderPos.q) * LERP_RATE;
  caravanRenderPos.r += (tr - caravanRenderPos.r) * LERP_RATE;
}

// No dedicated sprite asset (see dev-loop.md's image-cache budget note — the
// PSRAM cache is nearly full) — a plain canvas icon, styled like
// drawCharIcon()'s no-portrait fallback, keeps this from needing one.
function renderCaravan(cam) {
  lerpCaravanPosition();
  if (!worldState.caravan?.active) return;
  const { ox, oy, meRp } = cam;
  const { vq, vr } = closestWrapCoords(caravanRenderPos, meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  const cx = pp.x + ox, cy = pp.y + oy;
  if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) return;
  if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) return;

  const r = Math.max(10, HEX_SZ * 0.28);

  ctx.save();
  ctx.filter = 'blur(3px)';
  ctx.beginPath();
  ctx.ellipse(cx, cy + r * 0.85, r * 0.65, r * 0.18, 0, 0, Math.PI * 2);
  ctx.fillStyle = 'rgba(0,0,0,0.45)';
  ctx.fill();
  ctx.restore();

  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.fillStyle = '#8a6d3b';
  ctx.fill();
  ctx.lineWidth = 2;
  ctx.strokeStyle = '#2a1f10';
  ctx.stroke();

  ctx.font = `${Math.round(r * 1.1)}px sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = '#f0e6d2';
  ctx.fillText('⬢', cx, cy + 1);
}

const doomRenderPos = { q: 0, r: 0 };
let doomRenderPosLive = false;

function lerpDoomPosition() {
  const d = worldState.doom;
  if (!d) { doomRenderPosLive = false; return; }
  if (!doomRenderPosLive) {
    doomRenderPos.q = d.q;
    doomRenderPos.r = d.r;
    doomRenderPosLive = true;
    return;
  }
  let tq = d.q, tr = d.r;
  while (tq - doomRenderPos.q >  MAP_COLS / 2) tq -= MAP_COLS;
  while (doomRenderPos.q - tq >  MAP_COLS / 2) tq += MAP_COLS;
  while (tr - doomRenderPos.r >  MAP_ROWS / 2) tr -= MAP_ROWS;
  while (doomRenderPos.r - tr >  MAP_ROWS / 2) tr += MAP_ROWS;
  doomRenderPos.q += (tq - doomRenderPos.q) * LERP_RATE;
  doomRenderPos.r += (tr - doomRenderPos.r) * LERP_RATE;
}

// Scent-detection radius, mirroring doomDetectionRadius() in world-system.hpp:
// DOOM_BASE_RADIUS at awareness 0, +1 per 25 awareness (6 -> 10). Derived here
// rather than shipped in the world sync because it's a pure function of
// `awareness`, which every state broadcast already carries — a `rad` field
// would be redundant bytes on the K10's hottest message.
function doomDetectionRadius(awareness) {
  return DOOM_BASE_RADIUS + Math.floor(awareness / 25);
}

// Awareness (0-100, raw from the server, never shown as a number per spec)
// drives a pulsing dread aura — bigger, darker and faster-pulsing the closer
// Doom is to locking onto a player, rather than a single fixed-size wash, so
// its threat level actually reads visually. Silhouette core (no sprite asset,
// same image-cache-budget reasoning as the caravan) is deliberately dark and
// blood-tinted, distinct from the caravan's friendly brown circle.
//
// Drawn at every awareness including 0, matching the K10 map screen
// (drawMapScreen() in ui-screens.hpp, which has a dormant colour for
// awareness < 51). An earlier `awareness <= 0` early-out meant a dormant
// Doom was plotted on the device but invisible in the browser — and since
// awareness sits at 0 for most of a run, that was the normal case, not an
// edge case. Only the aura's size/alpha/pulse scale with awareness now.
function renderDoom(cam) {
  lerpDoomPosition();
  const d = worldState.doom;
  if (!d) return;
  const { ox, oy, meRp } = cam;
  const { vq, vr } = closestWrapCoords(doomRenderPos, meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  const cx = pp.x + ox, cy = pp.y + oy;
  const auraR = HEX_SZ * (1.4 + (d.awareness / 100) * 2.2);
  // Scent-detection ring circumradius. The K10 approximates the same disc
  // with an ellipse because its minimap is a rectangular grid (drawMapScreen(),
  // ui-screens.hpp); here the grid is true flat-top axial hexes, so a
  // hexDist <= N disc is an exact regular hexagon in pixel space. One axial
  // step spans SQRT3 * HEX_SZ, and the extra half-step puts the outline
  // outside the boundary hexes instead of through their centres.
  const ringR = SQRT3 * HEX_SZ * (doomDetectionRadius(d.awareness) + 0.5);
  // Cull against whichever is bigger: the ring can still cross the viewport
  // with Doom itself well offscreen, which is exactly when you want to see it.
  const cullR = Math.max(auraR, ringR);
  if (cx < -cullR || cx > cssWidth  + cullR) return;
  if (cy < -cullR || cy > cssHeight + cullR) return;

  const pulseSpeed = 1.2 + (d.awareness / 100) * 2.5;  // faster pulse = more aware
  const pulse = 0.85 + 0.15 * Math.sin(Date.now() / 1000 * pulseSpeed);
  const alpha = (0.12 + (d.awareness / 100) * 0.45) * pulse;

  // Hexagon vertices sit at 30° + k*60°: the six axial step directions
  // ((+1,0), (+1,-1), (0,-1), ...) land there under hexToPixel(), so the ring
  // is rotated a half-facet from the cells it encloses. Dashes crawl inward
  // so a dormant ring still reads as something closing rather than a map
  // annotation.
  ctx.save();
  ctx.beginPath();
  for (let i = 0; i < 6; i++) {
    const a = Math.PI / 6 + Math.PI / 3 * i;
    const vx = cx + ringR * Math.cos(a), vy = cy + ringR * Math.sin(a);
    i === 0 ? ctx.moveTo(vx, vy) : ctx.lineTo(vx, vy);
  }
  ctx.closePath();
  ctx.setLineDash([5, 9]);
  ctx.lineDashOffset = (Date.now() / 1000 * 7) % 14;
  // Dark under-stroke first, same reason drawMapScreen() halos the K10
  // marker: a thin red dash alone disappears against lit terrain, and a
  // dormant ring is exactly the one you most need to notice.
  ctx.lineWidth = 3.5;
  ctx.strokeStyle = `rgba(12,4,16,${0.35 * pulse})`;
  ctx.stroke();
  ctx.lineWidth = 2;
  ctx.strokeStyle = `rgba(190,50,80,${(0.30 + (d.awareness / 100) * 0.45) * pulse})`;
  ctx.stroke();
  ctx.restore();

  const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, auraR);
  g.addColorStop(0,    `rgba(90,0,40,${alpha})`);
  g.addColorStop(0.55, `rgba(40,0,50,${alpha * 0.6})`);
  g.addColorStop(1,    'rgba(20,0,30,0)');
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(cx, cy, auraR, 0, Math.PI * 2);
  ctx.fill();

  const r = Math.max(9, HEX_SZ * 0.24);
  ctx.save();
  ctx.filter = 'blur(2px)';
  ctx.beginPath();
  ctx.ellipse(cx, cy + r * 0.85, r * 0.65, r * 0.18, 0, 0, Math.PI * 2);
  ctx.fillStyle = 'rgba(0,0,0,0.5)';
  ctx.fill();
  ctx.restore();

  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.fillStyle = '#0c0610';
  ctx.fill();
  ctx.lineWidth = 1.5;
  ctx.strokeStyle = `rgba(160,20,60,${0.5 + (d.awareness / 100) * 0.5})`;
  ctx.stroke();

  ctx.font = `${Math.round(r * 1.2)}px sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = `rgba(200,40,80,${0.7 + (d.awareness / 100) * 0.3})`;
  ctx.fillText('☠', cx, cy + 1);
}
