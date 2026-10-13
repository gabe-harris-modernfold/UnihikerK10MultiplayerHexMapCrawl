// ── kane-jeep.js ───────────────────────────────────────────────────────────
// Machine Gun Kane on the map (kane.hpp, docs/machine-gun-kane.md): the rusted
// M151, the camp he leaves at dawn, the house in the glass, his question, and
// the burst that follows it.
//
// State is worldState.kane (the server's "kane" object, merged in by
// _applyWorldState in network.js): q r, m (0 driving / 1 at his fire / 2
// gone), f (-1 west as painted / +1 east, mirrored), st (chapters read), e
// (ending), cq cr (the live camp, -1 none), gq gr (the house in the glass,
// only while it is the next thing to find).
//
// Everyone is told where he is; the jeep is drawn only inside your own sight
// (myVisionR), the way the design asks -- you hear about Kane before you see
// him. His camp, the house and a retired jeep stay drawn once their hex is on
// your map: they are places, not a threat on the move.
//
// Sticker cells (tiles.json "kane", hex_sheets.py KANE): 0 driving, 1 at his
// fire, 2 retired, 3 the camp he leaves. All are 1.5*HEX_SZ squares like the
// caravan's, painted with the front toward the lower left. The effects (the
// tracers, the muzzle flash, the brass, his fire, his dust) are drawn here.
// The dust is the Motorbike's (bike-dust.js); his fire is its own small
// flicker, drawn over the night tint like the debris fireballs (see
// renderKaneLight). Where the next chapter waits gets the quests' yellow hex
// outline (kaneQuestAt, renderer.js isPOIRenderable).

const KANE_SPRITE_HEX = 1.5;   // sticker edge in HEX_SZ, = KANE in hex_sheets.py
const KANE_ASK_MS     = 15000; // one world tick: how long the answer has, = WORLD_TICK_INTERVAL
// Points on the painted jeep, in HEX_SZ from the sticker's centre, facing west
// (x is mirrored when he heads east). Measured off the cut cells.
const KANE_MUZZLE = { x: -0.25, y: -0.21 };   // the barrel's tip, over the hood
const KANE_TYRE   = { x:  0.35, y:  0.06 };   // the rear tyre on the ground
const KANE_FIRE   = { x:  0.28, y:  0.32 };   // the stone ring in the camped cell
// How fast the sticker glides to a new hex: half the pawns' LERP_RATE, so a
// step reads as the jeep driving there rather than jumping (user, 2026-10-12).
// A literal, not LERP_RATE / 2: this file loads before engine.js defines it.
const KANE_GLIDE  = 0.13;

function kaneSprite(cell) {
  const at = tileAtlas.kane?.[cell];
  const page = at && tileAtlas.pages[at[0]];
  return page?.loaded ? { page, sx: at[1], sy: at[2], s: tileAtlas.kaneCell?.[0] || 384 } : null;
}

const kaneRenderPos = { q: 0, r: 0 };
let kaneRenderLive = false;
let kaneLastStep = { dq: 0, dr: 0 };
let _lastKaneHex = null;
// His question, put to us: until when the "?" shows (0 = none).
let kaneAskUntil = 0;
// Tracers, flashes, brass and hits in flight, in map coordinates.
const kaneFx = [];

// ── Heading ─────────────────────────────────────────────────────────────────
// The jeep turns to face where it is going BEFORE it moves, and swings its
// gun -- bolted to the windshield, pointing forward -- onto whoever it is
// asking before it fires (the user, 2026-10-12). The sticker is one 3/4
// painting whose nose points at 175 deg (screen angle, y down: just about due
// west); mirrored, 5 deg. A heading is drawn as that mirror (sx: 1 as painted,
// -1 mirrored) plus a tilt of the whole sticker toward the way it is pointed,
// clamped so it never stands on its tail. A turn that changes sides flips
// through edge-on (scaleX through 0), which reads as swinging round.
const KANE_NOSE     = 175 * Math.PI / 180;
const KANE_TILT_MAX = 50 * Math.PI / 180;
const KANE_TURN_MS  = 520;   // an about-face; smaller turns take proportionally less
const kaneHeading = { sx: 1, tilt: 0 };   // what is drawn this frame
let kaneTurn = null;                       // { from, to, t0, ms } while turning
let kaneTarget = null;                     // the unwrapped hex he is gliding to
let kaneHoldUntil = 0;                     // firing: no new turn or glide before this

function kaneHeadingFor(dx, dy, prevSx) {
  const sx = Math.abs(dx) < 1e-3 ? (prevSx < 0 ? -1 : 1) : (dx < 0 ? 1 : -1);
  const base = sx === 1 ? KANE_NOSE : Math.PI - KANE_NOSE;
  let t = Math.atan2(dy, dx) - base;
  while (t >  Math.PI) t -= 2 * Math.PI;
  while (t < -Math.PI) t += 2 * Math.PI;
  return { sx, tilt: Math.max(-KANE_TILT_MAX, Math.min(KANE_TILT_MAX, t)) };
}

// Start turning toward a screen-space direction; returns how long it takes.
function kaneTurnTo(dx, dy) {
  if (kaneTurn) { kaneHeading.sx = kaneTurn.to.sx; kaneHeading.tilt = kaneTurn.to.tilt; kaneTurn = null; }
  const to = kaneHeadingFor(dx, dy, kaneHeading.sx);
  const d = Math.abs(to.tilt - kaneHeading.tilt) + (to.sx !== kaneHeading.sx ? Math.PI : 0);
  if (d < 0.05) return 0;
  const ms = Math.max(160, KANE_TURN_MS * Math.min(1, d / Math.PI));
  kaneTurn = { from: { sx: kaneHeading.sx, tilt: kaneHeading.tilt }, to, t0: performance.now(), ms };
  return ms;
}

// Swing the jeep (and the gun) onto a map position: the survivor he is
// asking, or shooting. Returns the turn's length, so a burst can wait for it.
function kaneFaceToward(pos) {
  if (!kaneRenderLive || !pos) return 0;
  const v = hexToPixel(pos.q - kaneRenderPos.q, pos.r - kaneRenderPos.r, 1);
  if (Math.hypot(v.x, v.y) < 0.05) return 0;
  return kaneTurnTo(v.x, v.y);
}

// A sticker-local point (HEX_SZ, measured on the painting as it faces west)
// to a screen offset under the current mirror and tilt.
function kaneLocal(off, hd) {
  const x = off.x * hd.sx * HEX_SZ, y = off.y * HEX_SZ;
  const c = Math.cos(hd.tilt), s = Math.sin(hd.tilt);
  return { x: x * c - y * s, y: x * s + y * c };
}

// ── His ruts: behind him, never ahead ───────────────────────────────────────
// The server stamps the ruts on the hex he moves to the moment he moves
// (kaneStep in kane.hpp), but the sticker first turns and then glides there.
// So the ruts of a step still being driven are drawn only as far as the jeep
// has got (TrailField.drawHex asks kaneTrackCut): they come out from under
// it, never run on ahead of it (the user, 2026-10-12).
const kaneTrackMoves = [];   // [{ aq, ar, bq, br, d }]: steps the sticker has not finished

// null, or how far to draw the ruts on (q, r): the jeep's position in that
// hex's units (HEX_SZ 1, from its centre), and which ruts there are the steps
// being driven -- o: the side a step leaves this hex by, i: the side one
// enters it by (-1 none).
function kaneTrackCut(q, r) {
  if (!kaneTrackMoves.length || !kaneRenderLive) return null;
  let i = -1, o = -1;
  for (const m of kaneTrackMoves) {
    if (m.aq === q && m.ar === r) o = m.d;
    if (m.bq === q && m.br === r) i = (m.d + 3) % 6;
  }
  if (i < 0 && o < 0) return null;
  let dq = kaneRenderPos.q - q, dr = kaneRenderPos.r - r;
  dq -= Math.round(dq / MAP_COLS) * MAP_COLS;
  dr -= Math.round(dr / MAP_ROWS) * MAP_ROWS;
  const p = hexToPixel(dq, dr, 1);
  // Quantised: the rut geometry is cached per cut.
  return { x: Math.round(p.x * 32) / 32, y: Math.round(p.y * 32) / 32, i, o };
}

function lerpKanePosition() {
  const k = worldState.kane;
  if (!k) { kaneRenderLive = false; return; }
  if (!kaneRenderLive) {
    kaneRenderPos.q = k.q; kaneRenderPos.r = k.r; kaneRenderLive = true;
    kaneHeading.sx = k.f > 0 ? -1 : 1; kaneHeading.tilt = 0; kaneTarget = { q: k.q, r: k.r };
    return;
  }
  let tq = k.q, tr = k.r;
  while (tq - kaneRenderPos.q >  MAP_COLS / 2) tq -= MAP_COLS;
  while (kaneRenderPos.q - tq >  MAP_COLS / 2) tq += MAP_COLS;
  while (tr - kaneRenderPos.r >  MAP_ROWS / 2) tr -= MAP_ROWS;
  while (kaneRenderPos.r - tr >  MAP_ROWS / 2) tr += MAP_ROWS;
  kaneLastStep = { dq: 0, dr: 0 };
  // 1. A turn in progress plays out first.
  if (kaneTurn) {
    const u = Math.min(1, (performance.now() - kaneTurn.t0) / kaneTurn.ms);
    const e = u * u * (3 - 2 * u);
    kaneHeading.tilt = kaneTurn.from.tilt + (kaneTurn.to.tilt - kaneTurn.from.tilt) * e;
    kaneHeading.sx = kaneTurn.from.sx !== kaneTurn.to.sx ? kaneTurn.from.sx * Math.cos(Math.PI * e) : kaneTurn.to.sx;
    if (u >= 1) { kaneHeading.sx = kaneTurn.to.sx; kaneHeading.tilt = kaneTurn.to.tilt; kaneTurn = null; }
    return;
  }
  // 2. Firing: hold still until the burst is done.
  if (performance.now() < kaneHoldUntil) return;
  // 3. A new hex to drive to: point the jeep at it first (next frames), then go.
  if (!kaneTarget || kaneTarget.q !== tq || kaneTarget.r !== tr) {
    kaneTarget = { q: tq, r: tr };
    const v = hexToPixel(tq - kaneRenderPos.q, tr - kaneRenderPos.r, 1);
    if (Math.hypot(v.x, v.y) > 0.05 && kaneTurnTo(v.x, v.y)) return;
  }
  // Parked (at his fire, or for good): settle level.
  if (k.m !== 0) kaneHeading.tilt *= 0.9;
  const dq = (tq - kaneRenderPos.q) * KANE_GLIDE, dr = (tr - kaneRenderPos.r) * KANE_GLIDE;
  kaneRenderPos.q += dq;
  kaneRenderPos.r += dr;
  kaneLastStep = { dq, dr };
  // There: every step he was driving is laid in full.
  if (kaneTrackMoves.length && Math.abs(tq - kaneRenderPos.q) < 0.01 && Math.abs(tr - kaneRenderPos.r) < 0.01) {
    kaneTrackMoves.length = 0;
  }
}

// Called by _applyWorldState: his ruts are the caravan's bit (kane.hpp), so
// stamp them on an already-revealed hex the same way, without waiting for a
// vis disk to cover it.
function kaneApplyWorld() {
  const k = worldState.kane;
  if (!k || k.m === 2) { _lastKaneHex = k ? { q: k.q, r: k.r } : null; return; }
  if (_lastKaneHex && (k.q !== _lastKaneHex.q || k.r !== _lastKaneHex.r)) {
    if (gameMap[k.r]?.[k.q]) gameMap[k.r][k.q] = { ...gameMap[k.r][k.q], tireTrack: 1 };
    trailField?.step('v', 0, k.q, k.r, _lastKaneHex.q, _lastKaneHex.r);
    // Not driven yet: kaneTrackCut holds these ruts back until the sticker is there.
    const d = _trailSideTo(_lastKaneHex.q, _lastKaneHex.r, k.q, k.r);
    if (d >= 0) {
      kaneTrackMoves.push({ aq: _lastKaneHex.q, ar: _lastKaneHex.r, bq: k.q, br: k.r, d });
      if (kaneTrackMoves.length > 4) kaneTrackMoves.shift();
    }
  }
  _lastKaneHex = { q: k.q, r: k.r };
}

function kaneInSight(q, r) {
  if (myId < 0 || !players[myId]) return true;
  const me = players[myId];
  return hexDistWrap(me.q, me.r, q, r) <= (myVisionR ?? 3);
}
const kaneKnown = (q, r) => !!gameMap[r]?.[q] || kaneInSight(q, r);

// Is (q, r) where his next chapter waits -- step on it and it opens
// (kaneOnArrival in kane.hpp)? Those hexes get the quests' yellow outline
// (the user, 2026-10-12; renderer.js isPOIRenderable, same sight rule):
//   chapters 1-3: the camp he left at dawn (cq cr, only sent while one is live)
//   chapter 4:    the house in the glass (gq gr, only sent while it is next)
//   chapter 5:    him, at his fire at night, once chapter 4 is read
function kaneQuestAt(q, r) {
  const k = worldState.kane;
  if (!k || myDepth || k.e) return false;
  if (k.cq === q && k.cr === r) return true;
  if (k.gq === q && k.gr === r) return true;
  return k.st === 4 && k.m === 1 && k.q === q && k.r === r;
}

// Screen point of a map position (fractional q/r), wrapped to the copy
// nearest us.
function kaneScreen(cam, pos) {
  const { vq, vr } = closestWrapCoords(pos, cam.meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  return { x: pp.x + cam.ox, y: pp.y + cam.oy };
}
const kaneOnScreen = (p, m) => p.x > -m && p.x < cssWidth + m && p.y > -m && p.y < cssHeight + m;

// hd: { sx, tilt } -- mirror (1 as painted, -1 mirrored, anything between
// mid-turn) and tilt in radians. The camp marker passes { sx: 1, tilt: 0 }.
function kaneDrawCell(cell, x, y, hd) {
  const sp = kaneSprite(cell);
  const sz = HEX_SZ * KANE_SPRITE_HEX;
  if (!sp) {
    // No sheet on this board: a rust-red badge with a gun over it.
    const r = Math.max(9, HEX_SZ * 0.24);
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(hd.tilt);
    ctx.scale(hd.sx, 1);
    ctx.fillStyle = cell === 3 ? '#5a4a36' : '#7a3b1c';
    ctx.strokeStyle = '#1a0e06'; ctx.lineWidth = 2;
    ctx.fillRect(-r, -r * 0.55, r * 2, r * 1.1); ctx.strokeRect(-r, -r * 0.55, r * 2, r * 1.1);
    if (cell !== 3) { ctx.beginPath(); ctx.moveTo(-r * 0.2, -r * 0.6); ctx.lineTo(-r * 1.5, -r * 0.95); ctx.stroke(); }
    ctx.restore();
    return;
  }
  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(hd.tilt);
  ctx.scale(hd.sx, 1);
  drawAtlasCell(ctx, sp.page, sp.sx, sp.sy, sp.s, sp.s, -sz / 2, -sz / 2, sz, sz);
  ctx.restore();
}

// The 'kane' layer: under the pawns, like the caravan.
function renderKane(cam) {
  lerpKanePosition();
  const k = worldState.kane;
  if (!k) return;
  const now = performance.now();

  // The camp he left, holding the next chapter.
  if (k.cq >= 0 && kaneKnown(k.cq, k.cr)) {
    const p = kaneScreen(cam, { q: k.cq, r: k.cr });
    if (kaneOnScreen(p, HEX_SZ * 2)) kaneDrawCell(3, p.x, p.y, { sx: 1, tilt: 0 });
  }
  // The house in the glass: there is no sticker for it -- it is inside the
  // glass and his ring of tracks is the landmark. A low pulse over the hex
  // and a doorway with somebody standing in it.
  if (k.gq >= 0 && kaneKnown(k.gq, k.gr)) {
    const p = kaneScreen(cam, { q: k.gq, r: k.gr });
    if (kaneOnScreen(p, HEX_SZ * 2)) drawKaneHouse(p.x, p.y, now);
  }

  if (!kaneRenderLive) return;
  const jp = kaneScreen(cam, kaneRenderPos);
  if (!kaneOnScreen(jp, HEX_SZ * 2)) return;
  const gone = k.m === 2;
  if (gone ? !kaneKnown(k.q, k.r) : !kaneInSight(k.q, k.r)) return;
  // His question: a "?" over the jeep, for as long as the answer has.
  if (kaneAskUntil > now && k.m === 0) drawKaneAsk(jp.x, jp.y, now);

  kaneDrawCell(gone ? 2 : k.m === 1 ? 1 : 0, jp.x, jp.y, kaneHeading);

  // His fire is drawn in the 'kane_light' layer, over the night tint.
  if (k.m === 0 && bikeDust) {
    // Driving: the Motorbike's dust off his rear tyre, a bigger cloud.
    const v = hexToPixel(kaneLastStep.dq, kaneLastStep.dr, 1);
    const ty0 = kaneLocal(KANE_TYRE, kaneHeading);
    const tx = jp.x + ty0.x, ty = jp.y + ty0.y;
    const cell = gameMap[k.r]?.[k.q];
    bikeDust.kick('kane', (tx - cam.ox) / HEX_SZ, (ty - cam.oy) / HEX_SZ, v.x, v.y, cell ? cell.terrain : null, 1.3);
  }
}

// The 'kane_light' layer, over the time-of-day tint: his fire is a light, and
// he only lights it after dusk, so it has to be the brightest thing out there.
// Drawn here rather than as an anchor in the fire particle system -- that one
// sizes its smoke by the fire's spread but rises it in fixed px, so a
// campfire-sized anchor strung its smoke out into thin dark rods.
function renderKaneLight(cam) {
  const k = worldState.kane;
  if (!k || k.m !== 1 || !kaneRenderLive || !kaneInSight(k.q, k.r)) return;
  const jp = kaneScreen(cam, kaneRenderPos);
  if (!kaneOnScreen(jp, HEX_SZ * 2)) return;
  const fo = kaneLocal(KANE_FIRE, kaneHeading);
  drawKaneCampfire(jp.x + fo.x, jp.y + fo.y, performance.now());
}

function drawKaneCampfire(x, y, now) {
  const s = HEX_SZ, t = now / 1000;
  const fl = 0.8 + 0.12 * Math.sin(t * 11.3) + 0.08 * Math.sin(t * 17.9 + 1.3);
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  // The pool of light on the ground and the jeep beside it.
  const gr = s * 0.75;
  const g = ctx.createRadialGradient(x, y, 0, x, y, gr);
  g.addColorStop(0, `rgba(255,150,60,${0.32 * fl})`);
  g.addColorStop(0.4, `rgba(220,90,30,${0.12 * fl})`);
  g.addColorStop(1, 'rgba(200,70,20,0)');
  ctx.fillStyle = g;
  ctx.fillRect(x - gr, y - gr, gr * 2, gr * 2);
  // Three tongues, each its own flicker, leaning together in the draught.
  const lean = 0.25 * Math.sin(t * 1.7);
  for (let i = 0; i < 3; i++) {
    const ph = i * 2.1;
    const h = s * (0.13 + 0.05 * Math.sin(t * (9 + i * 3.7) + ph) + (i === 1 ? 0.04 : 0));
    const w = s * 0.045;
    const bx = x + (i - 1) * s * 0.035, by = y;
    const tx = bx + lean * h * 0.6 + Math.sin(t * 13 + ph) * s * 0.012, ty = by - h;
    const fg = ctx.createLinearGradient(bx, by, tx, ty);
    fg.addColorStop(0, 'rgba(255,235,160,0.95)');
    fg.addColorStop(0.45, 'rgba(255,150,40,0.85)');
    fg.addColorStop(1, 'rgba(200,50,10,0)');
    ctx.fillStyle = fg;
    ctx.beginPath();
    ctx.moveTo(bx - w, by);
    ctx.quadraticCurveTo(bx - w * 0.9, by - h * 0.55, tx, ty);
    ctx.quadraticCurveTo(bx + w * 0.9, by - h * 0.55, bx + w, by);
    ctx.closePath();
    ctx.fill();
  }
  // Embers going up and winking out.
  for (let e = 0; e < 4; e++) {
    const life = ((t * 0.6 + e * 0.29) % 1);
    const ex = x + Math.sin(e * 3.1 + t * 2) * s * 0.06 * (1 + life);
    const ey = y - s * (0.08 + life * 0.45);
    ctx.fillStyle = `rgba(255,${170 - 80 * life},60,${(1 - life) * 0.9})`;
    ctx.fillRect(ex - 1, ey - 1, 2, 2);
  }
  ctx.restore();
}

// A "?" over his head while he waits for an answer. No range ring and no
// countdown (the user, 2026-10-12): the toast and the log say what to do.
function drawKaneAsk(x, y, now) {
  const pulse = 0.5 + 0.5 * Math.sin(now / 160);
  ctx.save();
  const by = y - HEX_SZ * 0.95, br = Math.max(8, HEX_SZ * 0.2);
  ctx.beginPath();
  ctx.arc(x, by, br, 0, Math.PI * 2);
  ctx.fillStyle = 'rgba(20,10,4,0.85)';
  ctx.fill();
  ctx.lineWidth = 2;
  ctx.strokeStyle = `rgba(240,150,60,${0.6 + 0.4 * pulse})`;
  ctx.stroke();
  ctx.font = `bold ${Math.round(br * 1.35)}px monospace`;
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.fillStyle = '#f2b45a';
  ctx.fillText('?', x, by + 1);
  ctx.restore();
}

// Chapter 4's marker. Glass-green, still: a door frame and a small figure
// with one hand up, lit from inside.
function drawKaneHouse(x, y, now) {
  const s = HEX_SZ;
  const glow = 0.55 + 0.25 * Math.sin(now / 900);
  ctx.save();
  const g = ctx.createRadialGradient(x, y, 0, x, y, s * 0.8);
  g.addColorStop(0, `rgba(150,220,190,${0.22 * glow})`);
  g.addColorStop(1, 'rgba(150,220,190,0)');
  ctx.fillStyle = g;
  ctx.fillRect(x - s, y - s, s * 2, s * 2);
  const w = s * 0.26, h = s * 0.42, top = y - h * 0.62;
  ctx.fillStyle = 'rgba(12,22,18,0.85)';
  ctx.fillRect(x - w / 2, top, w, h);
  ctx.lineWidth = Math.max(1.5, s * 0.03);
  ctx.strokeStyle = `rgba(170,235,205,${0.7 + 0.3 * glow})`;
  ctx.strokeRect(x - w / 2, top, w, h);
  // her: a head, a body, one arm up.
  ctx.fillStyle = `rgba(190,245,220,${0.75 + 0.25 * glow})`;
  ctx.beginPath(); ctx.arc(x, top + h * 0.24, w * 0.14, 0, Math.PI * 2); ctx.fill();
  ctx.fillRect(x - w * 0.12, top + h * 0.34, w * 0.24, h * 0.5);
  ctx.save();
  ctx.translate(x + w * 0.1, top + h * 0.38);
  ctx.rotate(-0.7);
  ctx.fillRect(0, -w * 0.05, w * 0.32, w * 0.1);
  ctx.restore();
  ctx.restore();
}

// ── The burst ───────────────────────────────────────────────────────────────
// kane_fire: three rounds 110 ms apart, each a tracer from the muzzle to the
// target. A hit ends in a spark at the pawn, a miss throws up dirt off to the
// side. Every round flashes the muzzle and kicks a case out of the breech.
// Everything is stored in map coordinates (q, r plus HEX_SZ offsets), so the
// camera can move under it.
function kaneQueueBurst(ev) {
  const now = performance.now();
  const from = kaneRenderLive ? { q: kaneRenderPos.q, r: kaneRenderPos.r } : { q: ev.q, r: ev.r };
  const tgt = renderPos[ev.pid] ? { q: renderPos[ev.pid].q, r: renderPos[ev.pid].r } : null;
  if (!tgt) return;
  // The gun points where the jeep points: swing onto the target first, then
  // fire, and hold still until the last round has landed -- the move he makes
  // in the same world tick arrives right behind this event and would otherwise
  // turn him away mid-burst.
  const turnMs = kaneFaceToward(tgt);
  const hd = kaneTurn ? { ...kaneTurn.to } : { sx: kaneHeading.sx, tilt: kaneHeading.tilt };
  const rounds = 3, hits = ev.hits | 0;
  const t0 = now + turnMs;
  kaneHoldUntil = t0 + rounds * 110 + 260;
  for (let i = 0; i < rounds; i++) {
    const hit = i < hits;
    const ang = Math.random() * Math.PI * 2, miss = 0.35 + Math.random() * 0.35;
    kaneFx.push({
      kind: 'round', t0: t0 + i * 110, ttl: 420, from, hd, tgt,
      end: hit ? { x: (Math.random() - 0.5) * 0.12, y: -0.12 } : { x: Math.cos(ang) * miss, y: Math.sin(ang) * miss * 0.6 },
      hit,
      // the case: out of the jeep's right side, up and over, onto the ground
      bx: (0.15 + Math.random() * 0.25) * -Math.sign(hd.sx || 1), by: -(0.25 + Math.random() * 0.2),
    });
  }
  if (kaneFx.length > 60) kaneFx.splice(0, kaneFx.length - 60);
}

// The 'kane_fx' layer: over the pawns.
function renderKaneFx(cam) {
  if (!kaneFx.length) return;
  const now = performance.now();
  ctx.save();
  for (let i = kaneFx.length - 1; i >= 0; i--) {
    const f = kaneFx[i];
    const t = now - f.t0;
    if (t < 0) continue;
    if (t > f.ttl + 700) { kaneFx.splice(i, 1); continue; }
    const jp = kaneScreen(cam, f.from), tp = kaneScreen(cam, f.tgt);
    const mo = kaneLocal(KANE_MUZZLE, f.hd), mx = jp.x + mo.x, my = jp.y + mo.y;
    const ex = tp.x + f.end.x * HEX_SZ, ey = tp.y + f.end.y * HEX_SZ;
    if (!kaneOnScreen(jp, HEX_SZ * 6) && !kaneOnScreen(tp, HEX_SZ * 6)) continue;

    // Muzzle flash: the first 70 ms of each round.
    if (t < 70) {
      const a = 1 - t / 70, r = HEX_SZ * (0.18 + 0.1 * Math.random());
      ctx.globalCompositeOperation = 'lighter';
      const g = ctx.createRadialGradient(mx, my, 0, mx, my, r * 2.2);
      g.addColorStop(0, `rgba(255,240,200,${0.95 * a})`);
      g.addColorStop(0.3, `rgba(255,170,60,${0.6 * a})`);
      g.addColorStop(1, 'rgba(255,90,20,0)');
      ctx.fillStyle = g;
      ctx.fillRect(mx - r * 2.2, my - r * 2.2, r * 4.4, r * 4.4);
      ctx.globalCompositeOperation = 'source-over';
    }
    // The tracer: a bright head along the line with a short tail behind it.
    const fly = 140;
    if (t < fly + 90) {
      const h = Math.min(1, t / fly), tl = Math.max(0, (t - 60) / fly);
      const hx = mx + (ex - mx) * h, hy = my + (ey - my) * h;
      const tx = mx + (ex - mx) * Math.min(h, tl), ty = my + (ey - my) * Math.min(h, tl);
      ctx.globalCompositeOperation = 'lighter';
      ctx.lineCap = 'round';
      ctx.strokeStyle = 'rgba(255,170,70,0.55)';
      ctx.lineWidth = Math.max(2, HEX_SZ * 0.05);
      ctx.beginPath(); ctx.moveTo(tx, ty); ctx.lineTo(hx, hy); ctx.stroke();
      ctx.strokeStyle = 'rgba(255,245,210,0.95)';
      ctx.lineWidth = Math.max(1, HEX_SZ * 0.02);
      ctx.beginPath(); ctx.moveTo(tx + (hx - tx) * 0.5, ty + (hy - ty) * 0.5); ctx.lineTo(hx, hy); ctx.stroke();
      ctx.globalCompositeOperation = 'source-over';
    }
    // Where it landed: a red spark on a hit, a spray of dirt on a miss.
    const land = t - fly;
    if (land > 0 && land < 320) {
      const a = 1 - land / 320;
      if (f.hit) {
        ctx.globalCompositeOperation = 'lighter';
        for (let s = 0; s < 5; s++) {
          const an = s * 1.256 + f.t0, d = HEX_SZ * (0.06 + 0.22 * (land / 320));
          ctx.fillStyle = `rgba(255,${80 + 60 * a},40,${a})`;
          ctx.fillRect(ex + Math.cos(an) * d - 1.5, ey + Math.sin(an) * d - 1.5, 3, 3);
        }
        ctx.globalCompositeOperation = 'source-over';
      } else {
        for (let s = 0; s < 6; s++) {
          const an = -Math.PI / 2 + (s - 2.5) * 0.35, d = HEX_SZ * 0.18 * (land / 320);
          ctx.fillStyle = `rgba(150,128,96,${0.75 * a})`;
          ctx.beginPath();
          ctx.arc(ex + Math.cos(an) * d, ey + Math.sin(an) * d * 1.4, Math.max(1.5, HEX_SZ * 0.03), 0, Math.PI * 2);
          ctx.fill();
        }
      }
    }
    // The spent case: up out of the breech and down beside the jeep, glinting.
    if (t < 700) {
      const u = t / 700;
      const bx = mx + HEX_SZ * f.bx * u * 2.2, by = my + HEX_SZ * (f.by * 4 * u * (1 - u) + 0.45 * u * u);
      ctx.fillStyle = (Math.floor(t / 60) % 2) ? '#f6d27a' : '#b8862e';
      ctx.fillRect(bx - 1.5, by - 1, 3, 2);
    }
  }
  ctx.restore();
}

// ── Events (network.js dispatch) ───────────────────────────────────────────
// kane_call: unicast, so it is always about us. camp 0 = on the road (a burst
// follows in 15 s if we are still in the open), 1 = at his fire, where he
// does not shoot and there is nothing to tell him yet.
function _evKaneCall(ev) {
  const camp = !!ev.camp;
  const line = camp ? kaneCampLine(ev.idx) : kaneRoadLine(ev.idx);
  addLog(`<span class="log-check-fail">⌖ Kane: “${escHtml(line)}”</span>`);
  showToast(line, 'kane');
  if (!camp) {
    // He stops and points the jeep -- and the gun bolted to it -- at us.
    if (renderPos[myId]) kaneFaceToward({ q: renderPos[myId].q, r: renderPos[myId].r });
    kaneAskUntil = performance.now() + KANE_ASK_MS;
    addLog('<span class="log-mv">⌖ He is waiting for an answer. Get off the open ground or out of range — trees, ruins, a ridge, a town, the glass.</span>');
  }
}

function _evKaneFire(ev) {
  kaneAskUntil = 0;
  kaneQueueBurst(ev);
  const p = players[ev.pid];
  if (p && typeof ev.ll === 'number') p.ll = ev.ll;
  const mine = ev.pid === myId;
  const who = mine ? 'You' : (p?.nm || `P${ev.pid}`);
  if (ev.llLost > 0) {
    addLog(`<span class="log-check-fail">⌖ Kane’s burst catches ${escHtml(mine ? 'you' : who)} in the open (−${ev.llLost} LL)</span>`);
    if (mine) showToast(ev.ll > 0 ? '⌖ The burst finds you. Get to cover.' : '⌖ The burst finds you.');
  } else {
    addLog(`<span class="log-mv">⌖ Kane fires on ${escHtml(mine ? 'you' : who)} and misses</span>`);
    if (mine) showToast('⌖ Rounds kick up the dirt around you. He will ask again.');
  }
  if (mine) updateSidebar();
}

// kane_arc: the story moved. Everyone hears it -- it is the world's.
const KANE_ARC_LOG = [
  '',
  'read Kane’s board, and the catalog. NOT HER.',
  'read the tag on a child’s coat. KANE, AGE 9.',
  'read Kane’s rules off an ammo-can lid. A ring of tracks has been found round the glass.',
  'went into the glass, to the woman at the door. He knows. Find his fire, at night.',
];
function _evKaneArc(ev) {
  const mine = ev.pid === myId;
  const who = mine ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  if (ev.ending === 'truth') {
    addLog(`<span class="log-col">⌖ ${escHtml(who)} told Kane the truth. He walked into the glass.</span>`);
    showBanner('THE JEEP IS QUIET', 'Kane walked into the glass');
  } else if (ev.ending === 'lie') {
    addLog(`<span class="log-col">⌖ ${escHtml(who)} told Kane she was waiting at the Seam. He went.</span>`);
    showBanner('THE JEEP WENT WEST', 'nobody goes through the Seam');
  } else {
    addLog(`<span class="log-col">⌖ ${escHtml(who)} ${KANE_ARC_LOG[ev.stage | 0] || 'found more of Kane’s story.'}</span>`);
    if (!mine && ev.stage === 3) showToast('⌖ Somebody read Kane’s rules. There is a ring of tracks round the glass.');
  }
}
