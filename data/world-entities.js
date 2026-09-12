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

// Awareness (0-100, raw from the server, never shown as a number per spec)
// drives a pulsing dread aura — bigger, darker and faster-pulsing the closer
// Doom is to locking onto a player, rather than a single fixed-size wash, so
// its threat level actually reads visually. Silhouette core (no sprite asset,
// same image-cache-budget reasoning as the caravan) is deliberately dark and
// blood-tinted, distinct from the caravan's friendly brown circle.
function renderDoom(cam) {
  lerpDoomPosition();
  const d = worldState.doom;
  if (!d || d.awareness <= 0) return;
  const { ox, oy, meRp } = cam;
  const { vq, vr } = closestWrapCoords(doomRenderPos, meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  const cx = pp.x + ox, cy = pp.y + oy;
  const auraR = HEX_SZ * (1.4 + (d.awareness / 100) * 2.2);
  if (cx < -auraR || cx > cssWidth  + auraR) return;
  if (cy < -auraR || cy > cssHeight + auraR) return;

  const pulseSpeed = 1.2 + (d.awareness / 100) * 2.5;  // faster pulse = more aware
  const pulse = 0.85 + 0.15 * Math.sin(Date.now() / 1000 * pulseSpeed);
  const alpha = (0.12 + (d.awareness / 100) * 0.45) * pulse;

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
