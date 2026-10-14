// ── vacancy.js ─────────────────────────────────────────────────────────────
// VACANCY on the map (vacancy.hpp, docs/vacancy.md): the motel sign over one
// town, the front desk standing in the open, the corridor in a ruin, the
// houses that have checked in, and what the sign says when it is full.
//
// State is worldState.vac (the server's "vac" object, merged in by
// _applyWorldState in network.js): q r (the town, -1 = no town on this
// board), st (0 lit / 1 counting / 2 saved / 3 fallen), rm (nights left), g
// (banked: 1 the register, 2 the key), dq dr (the front desk) and kq kr (the
// corridor), each -1 unless it is still open.
//
// Everyone is told where the sign is. It is drawn once its hex is on your map
// -- and at night from VAC_SEE_NIGHT hexes off whatever the fog, because it
// is neon and nothing else out here is lit. The desk and the corridor are
// places: drawn once their hex is known. Whatever holds the next step gets
// the quests' yellow outline (vacQuestAt, renderer.js isPOIRenderable).
//
// Two layers: 'vacancy' under the pawns (the pole, the dead tubes, the desk,
// the doorway) and 'vacancy_light' over the night tint (lit neon, the lamp,
// the strip light, the windows of the houses that turned) -- light is light,
// the reasoning kane_light and debris_light use.
//
// The desk, the corridor and the fallen town wear the user's painted landmark
// tiles (vacTileKey, swapped in by engine.js terrainTile); the desk and the
// doorway below are drawn in code only until that sheet has loaded, and the
// lamp and the strip light stay code-drawn either way, so they can light up
// at night. The sign is all code. It is a wreck on a leaning pole off the
// town's upper right: NO (dark unless full) + VACANCY in single-stroke pink
// tubing, half of it broken or stuttering, and a backlit letter board hanging
// off one chain under it with the count. It mends as the town fills (see
// "The sign" below): the fuller the town, the steadier the sign.

const VAC_ROOMS      = 8;    // = vacancy.hpp VAC_ROOMS: the lit windows are VAC_ROOMS - rm
const VAC_SEE_NIGHT  = 10;   // hexes the lit sign carries at night, fog or not
const VAC_ITEM_REG   = 70;   // Guest Register (items.cfg)
const VAC_ITEM_KEY   = 71;   // Master Key
const VAC_PINK       = '255,62,150';
const VAC_RED        = '255,40,40';

let _vacTownSeen = '';       // the town we last told the log about (a new board = a new sign)
let _vacShake = null;        // this frame's quake on the sign (vacQuakeShake), shared by both passes
let _vacSiteJit = { d: [0, 0], k: [0, 0] };   // and on the desk and the corridor

// Lost to a quake or a falling sky before the count ran out: the sign went
// out with the town. Dead glass, like a saved one -- never the red of a full town.
const vacDark = (v) => v.st === 2 || (v.st === 3 && v.rm > 0);
const vacFull = (v) => v.st === 3 && v.rm === 0;

// A quake shakes the sign like everything else. The whole-view camera shake
// comes free through vacScreen (cam.ox/oy); the fault line's per-hex jitter
// is the same one renderHexTerrain gives the ground under it; and on top of
// both a tall pole on shaking ground whips, the box rattles on its last
// bracket and the board swings on its one chain -- a little even off the
// fault line, since the whole view is trembling. Computed once a frame in the
// base pass and reused by the light pass, so the glass and the glow never
// come apart.
function vacQuakeShake(v, now) {
  if (typeof quakeField === 'undefined' || !quakeField || myDepth) return null;
  const near = quakeField.intensityAt(v.q, v.r);
  const env = Math.max(near, 0.35 * quakeField.peakEnvelope());
  if (env <= 0.01) return null;
  const mag = near * HEX_SZ * 0.1;
  return {
    env,
    jx: (Math.random() - 0.5) * mag, jy: (Math.random() - 0.5) * mag,
    lean: env * (0.08 * Math.sin(now / 37) + 0.05 * Math.sin(now / 91 + 1.3)),
    sag: env * 0.06 * Math.sin(now / 29 + 0.7),
    mq: env * (0.34 * Math.sin(now / 150 + 2.1) + 0.08 * Math.sin(now / 47)),
  };
}
function vacJit(q, r) {
  if (typeof quakeField === 'undefined' || !quakeField || myDepth || q < 0) return [0, 0];
  const m = quakeField.intensityAt(q, r) * HEX_SZ * 0.1;
  return m ? [(Math.random() - 0.5) * m, (Math.random() - 0.5) * m] : [0, 0];
}

function vacState() { return worldState.vac || null; }

// The landmark tiles VACANCY's places wear (tiles.json `poi`, from the user's
// quest sheets; scripts/hex_sheets.py LANDMARKS). questTileKey() in engine.js
// asks here, by position. Only while a site is open: a banked desk is gone,
// carpet and all. The town wears its tile only once it is full -- a town lost
// to a quake was levelled, not checked in.
const VAC_TILE = {
  desk: '0_12', deskDunes: '1_15',     // the desk on scrub / on ash dunes
  hall: '4_13', hallWet: '5_15',       // the corridor in a ruin / in floodwater
  town: '4_14',                        // the fallen town, every door a motel door
};
// Where each tile's own light is painted, in HEX_SZ from the hex centre, so
// the lamp's glow and the strip light land on it (measured off the cells).
const VAC_TILE_LIGHT = {
  '0_12': [-0.24, -0.44], '1_15': [-0.03, -0.59],
  '4_13': [0.02, -0.26],  '5_15': [-0.04, -0.34],
};
function vacTileKey(q, r) {
  const v = vacState();
  if (!v || v.q < 0) return null;
  const t = gameMap[r]?.[q]?.terrain;
  if (v.dq === q && v.dr === r) return t === 1 ? VAC_TILE.deskDunes : VAC_TILE.desk;
  if (v.kq === q && v.kr === r) return t === 5 || t === 3 ? VAC_TILE.hallWet : VAC_TILE.hall;
  if (v.q === q && v.r === r && vacFull(v)) return VAC_TILE.town;
  return null;
}
// The painted light's offset for the site at (q, r), or null when the tile
// isn't up (then the code-drawn desk / doorway is, with its own light).
function vacTileLight(q, r) {
  const key = vacTileKey(q, r);
  return questTileDrawn(key) ? VAC_TILE_LIGHT[key] || null : null;
}
function vacMe() { return myId >= 0 ? players[myId] : null; }
function vacCarries(id) {
  const p = vacMe();
  return !!p && (p.it ?? []).some((t, i) => t === id && (p.iq?.[i] ?? 0) > 0);
}
function vacNight() {
  return typeof getTimePhase === 'function' ? Math.min(1, getTimePhase(displayAP).a / 0.6) : 0;
}
function vacInSight(q, r) {
  const me = vacMe();
  if (!me) return true;
  return hexDistWrap(me.q, me.r, q, r) <= (myVisionR ?? 3);
}
const vacKnown = (q, r) => !!gameMap[r]?.[q] || vacInSight(q, r);
// The sign: known, or lit and close enough to see across the dark.
function vacSignSeen(v) {
  if (v.q < 0) return false;
  if (vacKnown(v.q, v.r)) return true;
  const me = vacMe();
  return v.st <= 1 && !!me && vacNight() > 0.35 && hexDistWrap(me.q, me.r, v.q, v.r) <= VAC_SEE_NIGHT;
}

// Is (q, r) where the next step waits -- step on it and a scene opens
// (vacOnArrival in vacancy.hpp)? Same sight rule as a POI.
//   the sign not yet read:  the town
//   counting:               the desk and the corridor while they are open,
//                           and the town once you are carrying both halves
function vacQuestAt(q, r) {
  const v = vacState();
  if (!v || myDepth || v.q < 0 || v.st > 1) return false;
  if (v.st === 0) return v.q === q && v.r === r;
  if (v.dq === q && v.dr === r) return true;
  if (v.kq === q && v.kr === r) return true;
  return v.q === q && v.r === r && vacCarries(VAC_ITEM_REG) && vacCarries(VAC_ITEM_KEY);
}

// A compass word and a distance from us to (q, r), across the seam if shorter.
function vacBearing(q, r) {
  const me = vacMe();
  if (!me) return null;
  const { vq, vr } = closestWrapCoords({ q, r }, { q: me.q, r: me.r });
  const a = hexToPixel(vq, vr, 1), b = hexToPixel(me.q, me.r, 1);
  const ang = Math.atan2(-(a.y - b.y), a.x - b.x);
  const DIRS = ['east', 'north-east', 'north', 'north-west', 'west', 'south-west', 'south', 'south-east'];
  const i = ((Math.round(ang / (Math.PI / 4)) % 8) + 8) % 8;
  return { dir: DIRS[i], d: hexDistWrap(me.q, me.r, q, r) };
}

// Called from _applyWorldState: a new town under a new sign gets one line in
// the log, and which way it lies -- the only way anyone hears of it before
// they see it.
function vacApplyWorld() {
  const v = vacState();
  if (!v || v.q < 0) return;
  const key = `${v.q}_${v.r}`;
  if (key === _vacTownSeen || !vacMe()) return;
  _vacTownSeen = key;
  if (v.st > 1) return;
  const b = vacBearing(v.q, v.r);
  if (!b) return;
  addLog(`<span class="log-mv">⌂ Somewhere to the ${b.dir}, ${b.d} hexes off, a sign has come on over a town. It says VACANCY.</span>`);
}

function vacScreen(cam, q, r) {
  const { vq, vr } = closestWrapCoords({ q, r }, cam.meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  return { x: pp.x + cam.ox, y: pp.y + cam.oy };
}
const vacOnScreen = (p, m) => p.x > -m && p.x < cssWidth + m && p.y > -m && p.y < cssHeight + m;

// Stable per-hex noise, so a flicker or a window is the same every frame.
function vacHash(a, b) {
  let h = (Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul(b | 0, 0x165667b1)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
  return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
}

// A rounded rectangle path (CanvasRenderingContext2D.roundRect is too new for
// some of the phones that play this).
function vacRR(x, y, w, h, r) {
  r = Math.min(r, w / 2, h / 2);
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// ── The 'vacancy' layer (under the pawns) ───────────────────────────────────
function renderVacancy(cam) {
  const v = vacState();
  if (!v || v.q < 0) return;
  const now = performance.now();
  const s = HEX_SZ;
  _vacShake = vacQuakeShake(v, now);
  _vacSiteJit = { d: vacJit(v.dq, v.dr), k: vacJit(v.kq, v.kr) };
  // Drawn in code only until their landmark tiles are up (vacTileKey).
  if (v.dq >= 0 && vacKnown(v.dq, v.dr) && !vacTileLight(v.dq, v.dr)) {
    const p = vacScreen(cam, v.dq, v.dr);
    if (vacOnScreen(p, s * 2)) drawVacDesk(p.x + _vacSiteJit.d[0], p.y + _vacSiteJit.d[1], s);
  }
  if (v.kq >= 0 && vacKnown(v.kq, v.kr) && !vacTileLight(v.kq, v.kr)) {
    const p = vacScreen(cam, v.kq, v.kr);
    if (vacOnScreen(p, s * 2)) drawVacDoorway(p.x + _vacSiteJit.k[0], p.y + _vacSiteJit.k[1], s, now);
  }
  if (!vacSignSeen(v)) return;
  const p = vacScreen(cam, v.q, v.r);
  if (!vacOnScreen(p, s * 3)) return;
  drawVacSignBody(p.x, p.y, s, v);
}

// ── The 'vacancy_light' layer (over the night tint) ─────────────────────────
function renderVacancyLight(cam) {
  const v = vacState();
  if (!v || v.q < 0) return;
  const now = performance.now();
  const s = HEX_SZ, night = vacNight();
  if (v.dq >= 0 && vacKnown(v.dq, v.dr)) {
    const p = vacScreen(cam, v.dq, v.dr);
    if (vacOnScreen(p, s * 2)) drawVacLamp(p.x + _vacSiteJit.d[0], p.y + _vacSiteJit.d[1], s, night, now, vacTileLight(v.dq, v.dr));
  }
  if (v.kq >= 0 && vacKnown(v.kq, v.kr)) {
    const p = vacScreen(cam, v.kq, v.kr);
    if (vacOnScreen(p, s * 2)) drawVacStrip(p.x + _vacSiteJit.k[0], p.y + _vacSiteJit.k[1], s, night, now, vacTileLight(v.kq, v.kr));
  }
  if (!vacSignSeen(v) || vacDark(v)) return;   // saved or lost: the tubes are dead for good
  const p = vacScreen(cam, v.q, v.r);
  if (!vacOnScreen(p, s * 3)) return;
  if (v.st === 1) drawVacWindows(p.x, p.y, s, v, night, now);
  drawVacSignLight(p.x, p.y, s, v, night, now);
}

// ── The sign ────────────────────────────────────────────────────────────────
// A 1960s roadside motel sign sixty years on. The pole leans, the cabinet has
// slumped off a rusted-through bracket, the turquoise paint is down to a few
// flakes on rust, somebody has put rounds through it, and a cable hangs out of
// its belly to the dirt and spits. The tubes are single-stroke neon drawn in
// code (VAC_GLYPHS), and each has a health: some are snapped and dead, one is
// dying and only catches near its electrode, a few stutter in bursts, and the
// generator it runs off browns the whole thing out now and then.
//
// It mends as the town fills. Every house that checks in heals a tube: the
// snapped glass goes whole, the stutter steadies, the brown-outs stop -- and
// the night the town is full every tube burns steady red, NO VACANCY, in
// perfect working order. Saved, it is dead glass for good.

const VAC_DEAD = 0, VAC_DYING = 1, VAC_FLAKY = 2, VAC_OK = 3;
const VAC_DIM_RGB = '255,118,104';   // an underdriven tube: dimmer, and it goes orange

function vacArcPts(cx, cy, rx, ry, a0, a1, n) {
  const pts = [];
  for (let i = 0; i <= n; i++) {
    const a = (a0 + (a1 - a0) * i / n) * Math.PI / 180;
    pts.push([cx + Math.cos(a) * rx, cy + Math.sin(a) * ry]);
  }
  return pts;
}
// Single-stroke tube letters in a unit cell: x 0..w, y 0..1 (down).
const VAC_GLYPHS = {
  V: { w: 0.6,  s: [[[0, 0], [0.3, 1], [0.6, 0]]] },
  A: { w: 0.6,  s: [[[0, 1], [0.3, 0], [0.6, 1]], [[0.13, 0.64], [0.47, 0.64]]] },
  C: { w: 0.62, s: [vacArcPts(0.34, 0.5, 0.31, 0.5, -48, -312, 14)] },
  N: { w: 0.56, s: [[[0, 1], [0, 0], [0.56, 1], [0.56, 0]]] },
  Y: { w: 0.6,  s: [[[0, 0], [0.3, 0.48], [0.6, 0]], [[0.3, 0.48], [0.3, 1]]] },
  O: { w: 0.6,  s: [vacArcPts(0.3, 0.5, 0.3, 0.5, -90, 270, 20)] },
};
const vacStrokeLen = (st) => st.reduce((a, p, j) => (j ? a + Math.hypot(p[0] - st[j - 1][0], p[1] - st[j - 1][1]) : 0), 0);
for (const g of Object.values(VAC_GLYPHS)) g.len = g.s.reduce((a, st) => a + vacStrokeLen(st), 0);

// The sign's frame and this town's quirks, from the town hex's centre (x, y).
// Pole-local coordinates: origin at the foot of the pole, after the lean.
function vacSignFrame(x, y, s, v) {
  const seed = ((v.q * 73856093) ^ (v.r * 19349663)) | 0;
  const hs = (k) => vacHash(seed, k);
  const D = Math.PI / 180;
  const F = {
    seed, hs, s,
    bx: x + s * 0.3, by: y + s * 0.24,
    lean: -(2 + 3 * hs(1)) * D,          // the pole leans
    sag: (3 + 4 * hs(2)) * D,            // the cabinet slumped off its right bracket
    mqSag: -(7 + 7 * hs(3)) * D,         // the marquee hangs off its right chain
    poleH: s * 0.88, pw: Math.max(3, s * 0.06),
    W: s * 1.36, H: s * 0.36, Hl: s * 0.19,
    mW: s * 0.76, mH: s * 0.17,
  };
  // Which tubes are the bad ones: a fixed shuffle per town, mended in order.
  F.rank = [0, 1, 2, 3, 4, 5, 6].sort((a, b) => hs(20 + a) - hs(20 + b));
  // Sixty years of replacement glass from whatever was to hand: every tube
  // its own brightness, and a couple of them redder than the rest.
  F.age = [...Array(9)].map((_, i) => 0.62 + 0.38 * hs(150 + i));
  F.hue = [...Array(9)].map((_, i) => (hs(160 + i) < 0.28 ? '255,84,120' : null));
  // A quake under it (vacQuakeShake): the hex jitters, the pole whips, the
  // box rattles, the board swings.
  const k = _vacShake;
  if (k) { F.bx += k.jx; F.by += k.jy; F.lean += k.lean; F.sag += k.sag; F.mqSag += k.mq; }
  // Where the sparks come from (in s units, pole-local): the cable's frayed
  // end, and the rusted-through bracket under the cabinet's low corner.
  F.cableEnd = [0.5, -0.07];
  F.breakPt = [(F.W / 2) * Math.cos(F.sag) / s, (-F.poleH + (F.W / 2) * Math.sin(F.sag)) / s];
  return F;
}

// Where every letter sits on the cabinet (cabinet-local: x across the pole,
// y up from the cabinet's foot, so the face is y in [-H, 0]).
function vacLetters(F) {
  const hN = F.Hl * 0.6, gap = F.s * 0.07, trk = 0.22;
  const adv = (ch, h) => (VAC_GLYPHS[ch].w + trk) * h;
  const noW = adv('N', hN) + VAC_GLYPHS.O.w * hN;
  const vacW = [...'VACANCY'].reduce((a, c) => a + adv(c, F.Hl), 0) - trk * F.Hl;
  let ox = -(noW + gap + vacW) / 2;
  const mid = -F.H / 2, out = [];
  for (const ch of 'NO') { out.push({ ch, ox, top: mid - hN / 2, h: hN, no: true }); ox += adv(ch, hN); }
  ox += gap - trk * hN;
  for (const ch of 'VACANCY') { out.push({ ch, ox, top: mid - F.Hl / 2, h: F.Hl, no: false }); ox += adv(ch, F.Hl); }
  return out;
}

// A VACANCY tube's health (i = 2..8) with `fill` of the town checked in.
function vacHealth(F, i, fill, st, rm) {
  if (st === 2 || (st === 3 && rm > 0)) return VAC_DEAD;   // saved or lost: dead glass
  if (st === 3) return VAC_OK;          // full: every tube perfect
  const score = F.rank.indexOf(i - 2) / 7, k = 1 - fill;
  if (score < 0.22 * k) return VAC_DEAD;
  if (score < 0.36 * k) return VAC_DYING;
  if (score < 0.75 * k) return VAC_FLAKY;
  return VAC_OK;
}

// How lit a tube is right now, 0..1. Bursts sit in per-tube time windows, so
// two bad letters never stutter in step.
function vacLevel(state, i, now, seed) {
  if (state === VAC_DEAD) return 0;
  const h = (a, b) => vacHash(a, (seed + b) | 0);
  if (state === VAC_OK) return 0.9 + 0.1 * h(Math.floor(now / 80) + i * 977, 3);
  const flaky = state === VAC_FLAKY;
  const P = flaky ? 2300 + 1100 * h(i, 5) : 1700 + 900 * h(i, 7);
  const tt = now + i * 613, w = Math.floor(tt / P), off = tt - w * P, key = w * 13 + i;
  const start = P * (0.15 + 0.5 * h(key, 9));
  const dur = flaky ? 160 + 700 * h(key, 11) : 90 + 420 * h(key, 13);
  const burst = off > start && off < start + dur && h(key, 15) < (flaky ? 0.6 : 0.55);
  const r = h(Math.floor(now / 45) + i * 31, 17);
  if (flaky) return burst ? (r < 0.45 ? 0 : r < 0.62 ? 0.3 : 1) : 0.93;
  return burst ? (r < 0.4 ? 0 : 0.3 + 0.35 * r) : 0;   // dying: dark, then it catches, dim
}

// The generator sputters: the whole sign drops to a fraction for a moment.
// Never once the town is full.
function vacBrown(now, seed, k) {
  if (k <= 0) return 1;
  const P = 3900, w = Math.floor(now / P), off = now - w * P;
  const h = (b) => vacHash(w, (seed + b) | 0);
  if (h(51) > 0.35 * k + 0.05) return 1;
  const start = P * (0.1 + 0.6 * h(53)), dur = 140 + 420 * h(57);
  if (off < start || off > start + dur) return 1;
  return 0.18 + 0.45 * vacHash(Math.floor(now / 55), (seed + 59) | 0);
}

// Trace a letter into the current path, up to `frac` of its tube length (a
// dying tube only lights the stretch nearest its electrode).
function vacTrace(L, frac = 1) {
  const g = VAC_GLYPHS[L.ch];
  let budget = frac >= 1 ? Infinity : g.len * frac;
  ctx.beginPath();
  for (const st of g.s) {
    if (budget <= 0) break;
    let [px, py] = st[0];
    ctx.moveTo(L.ox + px * L.h, L.top + py * L.h);
    for (let j = 1; j < st.length; j++) {
      const [qx, qy] = st[j], seg = Math.hypot(qx - px, qy - py);
      if (seg >= budget) {
        const t = budget / seg;
        ctx.lineTo(L.ox + (px + (qx - px) * t) * L.h, L.top + (py + (qy - py) * t) * L.h);
        budget = 0;
        break;
      }
      ctx.lineTo(L.ox + qx * L.h, L.top + qy * L.h);
      budget -= seg; px = qx; py = qy;
    }
  }
}

// Unlit glass: a pale, dusty tube (coated pink, or red for the NO) with a
// dark seat under it and a sheen down one side -- readable in daylight, the
// way a dead sign still says what it used to.
function vacGlass(h, no) {
  ctx.lineCap = 'round'; ctx.lineJoin = 'round';
  ctx.lineWidth = Math.max(2, h * 0.17);
  ctx.strokeStyle = 'rgba(12,6,4,0.55)';
  ctx.stroke();
  ctx.lineWidth = Math.max(1.2, h * 0.1);
  ctx.strokeStyle = no ? 'rgba(176,112,104,0.9)' : 'rgba(184,138,150,0.9)';
  ctx.stroke();
  ctx.lineWidth = Math.max(0.6, h * 0.035);
  ctx.strokeStyle = 'rgba(240,226,220,0.45)';
  ctx.stroke();
}

// A lit tube: additive glow in three widths, then the glass, then the hot
// core -- which is broken up along its length (`seed`), where sixty years of
// grime and a failing coat leave the tube dull in patches.
function vacNeon(h, rgb, lvl, glowK, seed) {
  ctx.lineCap = 'round'; ctx.lineJoin = 'round';
  ctx.globalCompositeOperation = 'lighter';
  ctx.lineWidth = h * 0.62; ctx.strokeStyle = `rgba(${rgb},${0.06 * lvl * glowK})`; ctx.stroke();
  ctx.lineWidth = h * 0.34; ctx.strokeStyle = `rgba(${rgb},${0.12 * lvl * glowK})`; ctx.stroke();
  ctx.lineWidth = h * 0.18; ctx.strokeStyle = `rgba(${rgb},${0.4 * lvl})`; ctx.stroke();
  ctx.globalCompositeOperation = 'source-over';
  ctx.lineWidth = Math.max(1.2, h * 0.1); ctx.strokeStyle = `rgba(${rgb},${Math.min(1, 1.2 * lvl)})`; ctx.stroke();
  const d = (k) => h * (0.12 + 0.5 * vacHash(seed, k));
  ctx.setLineDash([d(1) * 1.6, d(2) * 0.35, d(3), d(4) * 0.25, d(5) * 2, d(6) * 0.3]);
  ctx.lineWidth = Math.max(0.6, h * 0.04); ctx.strokeStyle = `rgba(255,236,244,${0.9 * lvl})`; ctx.stroke();
  ctx.setLineDash([]);
}

// Blackened ends: an old tube goes dark for a finger's width at each electrode.
function vacBurntEnds(L, lvl) {
  ctx.fillStyle = `rgba(20,8,10,${0.75 * Math.min(1, lvl + 0.3)})`;
  for (const st of VAC_GLYPHS[L.ch].s) for (const p of [st[0], st[st.length - 1]]) {
    ctx.beginPath(); ctx.arc(L.ox + p[0] * L.h, L.top + p[1] * L.h, Math.max(1, L.h * 0.07), 0, Math.PI * 2); ctx.fill();
  }
}

// Split a stroke (letter-local px points) at a fraction of its length.
function vacSplit(pts, t) {
  const total = vacStrokeLen(pts) * t;
  let acc = 0;
  for (let j = 1; j < pts.length; j++) {
    const a = pts[j - 1], b = pts[j], seg = Math.hypot(b[0] - a[0], b[1] - a[1]);
    if (acc + seg >= total) {
      const u = (total - acc) / seg, cut = [a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u];
      return [[...pts.slice(0, j), cut], [cut, ...pts.slice(j)], cut];
    }
    acc += seg;
  }
  return [pts, [pts[pts.length - 1]], pts[pts.length - 1]];
}
function vacPoly(pts) { ctx.beginPath(); pts.forEach(([x, y], j) => (j ? ctx.lineTo(x, y) : ctx.moveTo(x, y))); }

// A snapped tube: its longest stroke broken, the far half hanging off the
// break, a few shards of glass on the cabinet face under it.
function vacSnapped(L, F, i) {
  const g = VAC_GLYPHS[L.ch];
  const px = (st) => st.map(([x, y]) => [L.ox + x * L.h, L.top + y * L.h]);
  let longest = 0;
  g.s.forEach((st, j) => { if (vacStrokeLen(st) > vacStrokeLen(g.s[longest])) longest = j; });
  g.s.forEach((st, j) => {
    if (j !== longest) { vacPoly(px(st)); vacGlass(L.h, L.no); return; }
    const [a, b, cut] = vacSplit(px(st), 0.35 + 0.3 * F.hs(60 + i));
    vacPoly(a); vacGlass(L.h, L.no);
    ctx.save();
    ctx.translate(cut[0], cut[1]);
    ctx.rotate((F.hs(70 + i) < 0.5 ? -1 : 1) * (0.35 + 0.4 * F.hs(75 + i)));
    ctx.translate(-cut[0], -cut[1] + L.h * 0.06);
    vacPoly(b); vacGlass(L.h, L.no);
    ctx.restore();
    ctx.fillStyle = 'rgba(200,170,185,0.35)';
    for (let k = 0; k < 3; k++) {
      const sx = cut[0] + (F.hs(80 + i * 3 + k) - 0.5) * L.h * 0.5, sy = cut[1] + L.h * (0.15 + 0.3 * F.hs(90 + i * 3 + k));
      ctx.fillRect(sx, sy, Math.max(1, L.h * 0.05), Math.max(1, L.h * 0.03));
    }
  });
  // its electrode caps
  ctx.fillStyle = '#110906';
  for (const st of g.s) for (const p of [st[0], st[st.length - 1]]) {
    ctx.beginPath(); ctx.arc(L.ox + p[0] * L.h, L.top + p[1] * L.h, Math.max(1, L.h * 0.055), 0, Math.PI * 2); ctx.fill();
  }
}

// ── Base pass: pole, cable, marquee board, cabinet, dead glass ─────────────
function drawVacSignBody(x, y, s, v) {
  const F = vacSignFrame(x, y, s, v);
  const { hs, poleH, pw, W, H } = F;
  const fill = vacFull(v) ? 1 : v.st === 1 ? 1 - v.rm / VAC_ROOMS : 0;
  ctx.save();
  // its shadow, rubble round the foot, and a guy wire to a stake
  ctx.fillStyle = 'rgba(0,0,0,0.32)';
  ctx.beginPath(); ctx.ellipse(F.bx + s * 0.16, F.by + s * 0.01, s * 0.26, s * 0.05, 0, 0, Math.PI * 2); ctx.fill();
  ctx.strokeStyle = 'rgba(24,18,12,0.85)'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(F.bx - s * 0.02, F.by - poleH * 0.55); ctx.lineTo(F.bx - s * 0.42, F.by + s * 0.03); ctx.stroke();
  ctx.fillStyle = '#2a2018'; ctx.fillRect(F.bx - s * 0.44, F.by + s * 0.0, s * 0.03, s * 0.05);
  for (let k = 0; k < 4; k++) {
    ctx.fillStyle = k % 2 ? '#3d3730' : '#4a423a';
    ctx.beginPath();
    ctx.ellipse(F.bx + (hs(40 + k) - 0.5) * s * 0.3, F.by + s * (0.01 + 0.03 * hs(44 + k)), s * (0.04 + 0.03 * hs(48 + k)), s * 0.025, 0, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.translate(F.bx, F.by);
  ctx.rotate(F.lean);
  // the pole: rusted round section, a wrap of black tape where it split
  const pg = ctx.createLinearGradient(-pw / 2, 0, pw / 2, 0);
  pg.addColorStop(0, '#24170e'); pg.addColorStop(0.35, '#6b4630'); pg.addColorStop(1, '#1a1009');
  ctx.fillStyle = pg; ctx.fillRect(-pw / 2, -poleH, pw, poleH);
  ctx.strokeStyle = 'rgba(8,5,3,0.9)'; ctx.lineWidth = 1; ctx.strokeRect(-pw / 2, -poleH, pw, poleH);
  ctx.fillStyle = 'rgba(150,70,25,0.45)';
  ctx.fillRect(-pw / 2, -poleH * (0.25 + 0.2 * hs(5)), pw, s * 0.06);
  ctx.fillStyle = 'rgba(18,16,14,0.95)';
  ctx.fillRect(-pw / 2 - 1, -poleH * (0.5 + 0.12 * hs(6)), pw + 2, s * 0.045);
  // the cable out of its belly, down to the dirt, frayed at the end
  const cx0 = W * 0.36 * Math.cos(F.sag), cy0 = -poleH + W * 0.36 * Math.sin(F.sag);
  const ex = F.cableEnd[0] * s, ey = F.cableEnd[1] * s;
  ctx.strokeStyle = '#0b0907'; ctx.lineWidth = Math.max(1.2, s * 0.016);
  ctx.beginPath(); ctx.moveTo(cx0, cy0); ctx.quadraticCurveTo(ex + s * 0.14, -poleH * 0.35, ex, ey); ctx.stroke();
  ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(190,120,60,0.8)';
  ctx.beginPath(); ctx.moveTo(ex, ey); ctx.lineTo(ex - s * 0.025, ey + s * 0.02); ctx.moveTo(ex, ey); ctx.lineTo(ex + s * 0.02, ey + s * 0.025); ctx.stroke();
  drawVacMarqueeBoard(F);
  // the cabinet, slumped on top of the pole
  ctx.save();
  ctx.translate(0, -poleH);
  ctx.rotate(F.sag);
  vacCabinet(F);
  const letters = vacLetters(F);
  letters.forEach((L, i) => {
    const dead = vacDark(v) ? F.rank.indexOf(i - 2) >= 0 && F.rank.indexOf(i - 2) < 3
                            : !L.no && vacHealth(F, i, fill, v.st, v.rm) === VAC_DEAD;
    if (dead) { vacSnapped(L, F, i); return; }
    vacTrace(L); vacGlass(L.h, L.no);
    ctx.fillStyle = '#110906';
    for (const st of VAC_GLYPHS[L.ch].s) for (const p of [st[0], st[st.length - 1]]) {
      ctx.beginPath(); ctx.arc(L.ox + p[0] * L.h, L.top + p[1] * L.h, Math.max(1, L.h * 0.055), 0, Math.PI * 2); ctx.fill();
    }
  });
  ctx.restore();
  ctx.restore();
}

// The cabinet in cabinet-local space: sheet steel gone to rust, a few flakes
// of the old turquoise, rust tears under the rivets, its top-left corner torn
// off down to the angle iron, and bullet holes.
function vacCabinet(F) {
  const { W, H, s, hs } = F;
  const L = -W / 2, T = -H, R = W / 2;
  const n1 = s * (0.1 + 0.06 * hs(70)), n2 = s * (0.08 + 0.05 * hs(71));
  const body = new Path2D();
  body.moveTo(L + n1, T); body.lineTo(R, T); body.lineTo(R, 0); body.lineTo(L, 0); body.lineTo(L, T + n2);
  body.lineTo(L + n1 * 0.35, T + n2 * 0.8); body.lineTo(L + n1 * 0.5, T + n2 * 0.35); body.lineTo(L + n1 * 0.8, T + n2 * 0.5);
  body.closePath();
  // the angle iron that is left where the sheet tore away
  ctx.strokeStyle = '#1c130c'; ctx.lineWidth = Math.max(1, s * 0.018);
  ctx.beginPath(); ctx.moveTo(L + n1, T); ctx.lineTo(L, T); ctx.lineTo(L, T + n2); ctx.stroke();
  const g = ctx.createLinearGradient(0, T, 0, 0);
  g.addColorStop(0, '#4d2f1c'); g.addColorStop(0.55, '#33200f'); g.addColorStop(1, '#22150b');
  ctx.fillStyle = g; ctx.fill(body);
  ctx.save();
  ctx.clip(body);
  ctx.fillStyle = 'rgba(56,138,128,0.3)';
  for (let k = 0; k < 6; k++) {
    const bx = L + W * hs(80 + k), by = T + H * hs(90 + k), br = s * (0.025 + 0.045 * hs(100 + k));
    ctx.beginPath();
    for (let j = 0; j < 7; j++) {
      const a = j / 7 * Math.PI * 2, rr = br * (0.55 + 0.45 * hs(300 + k * 7 + j));
      j ? ctx.lineTo(bx + Math.cos(a) * rr, by + Math.sin(a) * rr * 0.7) : ctx.moveTo(bx + Math.cos(a) * rr, by + Math.sin(a) * rr * 0.7);
    }
    ctx.fill();
  }
  ctx.strokeStyle = 'rgba(165,74,24,0.45)'; ctx.lineWidth = Math.max(1, s * 0.012);
  for (let k = 0; k < 8; k++) {
    const rx = L + W * hs(110 + k);
    ctx.beginPath(); ctx.moveTo(rx, T + H * 0.1); ctx.lineTo(rx + s * 0.004, T + H * (0.3 + 0.65 * hs(120 + k))); ctx.stroke();
  }
  ctx.restore();
  ctx.strokeStyle = '#0d0704'; ctx.lineWidth = Math.max(1.2, s * 0.025); ctx.stroke(body);
  ctx.strokeStyle = 'rgba(225,140,80,0.22)'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(L + n1 + 2, T + 1.5); ctx.lineTo(R - 2, T + 1.5); ctx.stroke();
  // rivets along the top and the foot
  for (let k = 0; k < 9; k++) {
    const rx = L + W * (k + 0.5) / 9;
    for (const ry of [T + s * 0.025, -s * 0.025]) {
      if (rx < L + n1 && ry < T + n2) continue;   // torn away
      ctx.fillStyle = '#120a06'; ctx.beginPath(); ctx.arc(rx, ry, Math.max(0.8, s * 0.008), 0, Math.PI * 2); ctx.fill();
    }
  }
  // somebody used it for target practice
  for (let k = 0; k < 4; k++) {
    const hx = L + W * (0.12 + 0.78 * hs(130 + k)), hy = T + H * (0.15 + 0.7 * hs(140 + k)), hr = Math.max(1, s * 0.011);
    ctx.strokeStyle = 'rgba(150,70,25,0.5)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(hx, hy); ctx.lineTo(hx, hy + s * 0.04); ctx.stroke();
    ctx.fillStyle = '#040201'; ctx.beginPath(); ctx.arc(hx, hy, hr, 0, Math.PI * 2); ctx.fill();
    ctx.strokeStyle = 'rgba(205,130,80,0.55)'; ctx.beginPath(); ctx.arc(hx, hy, hr + 0.8, 0, Math.PI * 2); ctx.stroke();
  }
}

// The marquee: a letter board on a T-bracket under the cabinet. Its left
// chain has snapped, so it hangs off the right one.
function vacMarqueeFrame(F) {
  const { s, mW, mH, poleH } = F;
  const yA = -poleH + s * 0.05, px = mW / 2 - s * 0.03, py = yA + s * 0.05;
  return { yA, px, py, x0: -mW + s * 0.05, x1: s * 0.03, h: mH };
}
function drawVacMarqueeBoard(F) {
  const { s, mW } = F, M = vacMarqueeFrame(F);
  ctx.strokeStyle = '#1a120b'; ctx.lineWidth = Math.max(1.2, s * 0.02);
  ctx.beginPath(); ctx.moveTo(-mW / 2, M.yA); ctx.lineTo(mW / 2, M.yA); ctx.stroke();
  ctx.lineWidth = 1; ctx.strokeStyle = '#2b241c';
  ctx.beginPath(); ctx.moveTo(M.px, M.yA); ctx.lineTo(M.px, M.py); ctx.stroke();                        // the chain that held
  ctx.beginPath(); ctx.moveTo(-mW / 2 + s * 0.03, M.yA); ctx.lineTo(-mW / 2 + s * 0.04, M.yA + s * 0.03); ctx.stroke();   // the one that went
  ctx.save();
  ctx.translate(M.px, M.py); ctx.rotate(F.mqSag);
  ctx.fillStyle = '#23201b'; ctx.strokeStyle = '#3e2414'; ctx.lineWidth = Math.max(1, s * 0.018);
  ctx.beginPath(); vacRR(M.x0, 0, M.x1 - M.x0, M.h, s * 0.02); ctx.fill(); ctx.stroke();
  ctx.restore();
}

// ── Light pass: the glow, the marquee's backlight, the live tubes, sparks ──
let _vacSparks = [], _vacSparkWin = -1, _vacSparkT = 0, _vacBrownWas = 1, _vacQuakeWin = -1;

function drawVacSignLight(x, y, s, v, night, now) {
  const F = vacSignFrame(x, y, s, v);
  const full = vacFull(v);
  const left = full ? 0 : (v.st === 1 ? v.rm : VAC_ROOMS);
  const fill = 1 - left / VAC_ROOMS;
  const rgb = full ? VAC_RED : VAC_PINK;
  let brown = full ? 1 : vacBrown(now, F.seed, 1 - fill);
  // A quake jolts the generator: the power stutters hard while it shakes. The
  // full sign does not so much as blink -- it rides the quake out steady red.
  if (!full && _vacShake && vacHash(Math.floor(now / 40), F.seed + 71) < 0.6 * _vacShake.env) brown = Math.min(brown, 0.12);
  const glowK = 0.55 + 0.9 * night;
  const letters = vacLetters(F);
  // every tube's level now, before anything is drawn -- the glow follows them
  const lv = letters.map((L, i) => {
    if (L.no) {
      if (full) return { lvl: vacLevel(VAC_OK, i, now, F.seed), rgb: VAC_RED, frac: 1 };
      // the NO is dark until the last two rooms; then it starts to catch
      const catchP = left === 1 ? 0.1 : left === 2 ? 0.03 : 0;
      return { lvl: vacHash(Math.floor(now / 70) + i * 7, F.seed + 41) < catchP ? 0.55 : 0, rgb: VAC_RED, frac: 1 };
    }
    const st = vacHealth(F, i, fill, v.st, v.rm);
    const dying = st === VAC_DYING;
    // the full sign is perfect: every tube matched, none of them aged
    const age = full ? 1 : F.age[i], hue = full ? null : F.hue[i];
    return { lvl: vacLevel(st, i, now, F.seed) * brown * age, rgb: dying ? VAC_DIM_RGB : (hue || rgb),
             frac: dying ? 0.3 + 0.35 * vacHash(Math.floor(now / 1500) + i, F.seed + 43) : 1 };
  });
  const mean = lv.slice(2).reduce((a, o) => a + o.lvl, 0) / 7;

  ctx.save();
  ctx.translate(F.bx, F.by);
  ctx.rotate(F.lean);
  // the air round the cabinet and the pool at its foot follow what is lit
  const a = (0.05 + 0.2 * night) * (0.45 + 0.7 * mean);
  const cy = -F.poleH - F.H / 2;
  const g = ctx.createRadialGradient(0, cy, 0, 0, cy, s * (0.85 + 0.35 * night));
  g.addColorStop(0, `rgba(${rgb},${a})`); g.addColorStop(1, `rgba(${rgb},0)`);
  ctx.fillStyle = g; ctx.fillRect(-s * 1.8, cy - s * 1.2, s * 3.6, s * 2.6);
  const gg = ctx.createRadialGradient(0, 0, 0, 0, 0, s * 0.8);
  gg.addColorStop(0, `rgba(${rgb},${a * 0.7})`); gg.addColorStop(1, `rgba(${rgb},0)`);
  ctx.fillStyle = gg; ctx.fillRect(-s, -s * 0.5, s * 2, s);

  drawVacMarqueeLight(F, v, left, full, night, now, brown);

  ctx.save();
  ctx.translate(0, -F.poleH);
  ctx.rotate(F.sag);
  letters.forEach((L, i) => {
    const o = lv[i];
    if (o.lvl <= 0.01) return;
    vacTrace(L, o.frac);
    vacNeon(L.h, o.rgb, o.lvl, glowK, F.seed + i * 101);
    if (!full) vacBurntEnds(L, o.lvl);
  });
  ctx.restore();

  vacSparks(F, now, brown, !full);
  ctx.restore();
}

function drawVacMarqueeLight(F, v, left, full, night, now, brown) {
  const { s } = F, M = vacMarqueeFrame(F);
  ctx.save();
  ctx.translate(M.px, M.py); ctx.rotate(F.mqSag);
  // the backlight: one of its two tubes is dead, so the left third is dim
  const w = M.x1 - M.x0, a = (0.8 + 0.12 * night) * brown;
  const g = ctx.createLinearGradient(M.x0, 0, M.x1, 0);
  g.addColorStop(0, `rgba(120,110,92,${0.3 * a})`);
  g.addColorStop(0.3, `rgba(175,160,130,${0.42 * a})`);
  g.addColorStop(0.42, `rgba(250,236,205,${a})`);
  g.addColorStop(1, `rgba(250,236,205,${0.95 * a})`);
  ctx.fillStyle = g;
  ctx.beginPath(); vacRR(M.x0 + 1, 1, w - 2, M.h - 2, s * 0.018); ctx.fill();
  // slot-in letters, none of them quite straight; one hangs off a single slot
  const msg = full ? 'FULL' : v.st === 0 ? 'WELCOME' : `ROOMS ${left}`;
  const cw = (w * 0.84) / msg.length;
  ctx.font = `bold ${Math.round(M.h * 0.66)}px Consolas,ui-monospace,monospace`;
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.fillStyle = '#17110b';
  [...msg].forEach((c, j) => {
    if (c === ' ') return;
    const hang = j === 1 && !full;
    const rot = hang ? 0.55 : (F.hs(200 + j) - 0.5) * 0.24;
    ctx.save();
    ctx.translate(M.x0 + w * 0.08 + cw * (j + 0.5), M.h * 0.5 + (F.hs(210 + j) - 0.5) * M.h * 0.14 + (hang ? M.h * 0.08 : 0));
    ctx.rotate(rot);
    ctx.fillText(c, 0, 0);
    ctx.restore();
  });
  ctx.restore();
}

// Sparks off the cable's frayed end now and then, and off the rusted bracket
// when the generator sputters. Kept in s units in the sign's own frame, so a
// zoom or a pan carries them along.
function vacSparks(F, now, brown, live) {
  const dt = Math.min(0.05, Math.max(0, (now - _vacSparkT) / 1000));
  _vacSparkT = now;
  const spawn = (pt, n) => {
    for (let k = 0; k < n; k++) {
      _vacSparks.push({ x: pt[0], y: pt[1], vx: (Math.random() - 0.5) * 1.6, vy: -0.3 - Math.random() * 0.9,
                       t0: now, life: 250 + Math.random() * 420 });
    }
    if (_vacSparks.length > 60) _vacSparks.splice(0, _vacSparks.length - 60);
  };
  if (live) {
    const w = Math.floor(now / 1400);
    if (w !== _vacSparkWin) {
      _vacSparkWin = w;
      if (vacHash(w, F.seed + 61) < 0.33) spawn(F.cableEnd, 4 + Math.floor(vacHash(w, F.seed + 62) * 5));
    }
    if (brown < 1 && _vacBrownWas >= 1) spawn(F.breakPt, 6);
    // shaken: the bracket and the cable spit every few beats
    const qk = _vacShake ? _vacShake.env : 0;
    if (qk > 0.25) {
      const qw = Math.floor(now / 160);
      if (qw !== _vacQuakeWin) {
        _vacQuakeWin = qw;
        if (vacHash(qw, F.seed + 63) < 0.7 * qk) spawn(vacHash(qw, F.seed + 64) < 0.5 ? F.breakPt : F.cableEnd, 3 + Math.floor(5 * qk));
      }
    }
  }
  _vacBrownWas = brown;
  if (!_vacSparks.length) return;
  const s = F.s;
  ctx.globalCompositeOperation = 'lighter';
  ctx.lineCap = 'round';
  ctx.lineWidth = Math.max(1, s * 0.012);
  for (let i = _vacSparks.length - 1; i >= 0; i--) {
    const p = _vacSparks[i], age = (now - p.t0) / p.life;
    if (age >= 1) { _vacSparks.splice(i, 1); continue; }
    p.vy += 2.6 * dt; p.x += p.vx * dt; p.y += p.vy * dt;
    ctx.strokeStyle = `rgba(255,${(215 - 120 * age) | 0},${(130 - 110 * age) | 0},${1 - age})`;
    ctx.beginPath();
    ctx.moveTo((p.x - p.vx * 0.03) * s, (p.y - p.vy * 0.03) * s);
    ctx.lineTo(p.x * s, p.y * s);
    ctx.stroke();
  }
  ctx.globalCompositeOperation = 'source-over';
}

// The houses that have turned: a warm window apiece, scattered over the town
// hex at places fixed by the hex, each with somebody sat on the end of a bed.
function drawVacWindows(x, y, s, v, night, now) {
  const n = Math.max(0, VAC_ROOMS - v.rm);
  if (!n) return;
  ctx.save();
  for (let i = 0; i < n; i++) {
    const ang = vacHash(v.q * 31 + i, v.r * 17) * Math.PI * 2;
    const rad = s * (0.18 + 0.42 * vacHash(i, v.q + v.r * 101));
    const wx = x - s * 0.12 + Math.cos(ang) * rad, wy = y + s * 0.08 + Math.sin(ang) * rad * 0.6;
    const ww = s * 0.07, wh = s * 0.08;
    const a = 0.55 + 0.4 * night;
    ctx.shadowColor = 'rgba(255,200,90,0.9)'; ctx.shadowBlur = s * 0.12 * (0.3 + night);
    ctx.fillStyle = `rgba(255,214,120,${a})`;
    ctx.fillRect(wx - ww / 2, wy - wh / 2, ww, wh);
    ctx.shadowBlur = 0;
    // the one inside, facing out
    ctx.fillStyle = `rgba(40,24,10,${0.6 * a})`;
    ctx.beginPath(); ctx.arc(wx, wy - wh * 0.08, ww * 0.18, 0, Math.PI * 2); ctx.fill();
    ctx.fillRect(wx - ww * 0.2, wy + wh * 0.08, ww * 0.4, wh * 0.42);
  }
  ctx.restore();
}

// ── The front desk: a counter on a square of carpet, out in the open ───────
function drawVacDesk(x, y, s) {
  ctx.save();
  const cy = y + s * 0.1;
  // the carpet: burgundy, gold diamonds, edges cut clean
  const cw = s * 0.42, cd = s * 0.2;
  ctx.beginPath();
  ctx.moveTo(x - cw, cy); ctx.lineTo(x, cy - cd); ctx.lineTo(x + cw, cy); ctx.lineTo(x, cy + cd); ctx.closePath();
  ctx.fillStyle = '#5c1626'; ctx.fill();
  ctx.save(); ctx.clip();
  ctx.strokeStyle = 'rgba(214,170,70,0.45)'; ctx.lineWidth = 1;
  for (let k = -4; k <= 4; k++) {
    const o = k * s * 0.09;
    ctx.beginPath(); ctx.moveTo(x - cw + o, cy - cd); ctx.lineTo(x + o + cw, cy + cd); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x - cw + o, cy + cd); ctx.lineTo(x + o + cw, cy - cd); ctx.stroke();
  }
  ctx.restore();
  // the STAFF ONLY door behind the counter, with the waste showing through it
  const dx = x + s * 0.1, dw = s * 0.13, dh = s * 0.3, dy = cy - s * 0.06 - dh;
  ctx.fillStyle = 'rgba(0,0,0,0.25)'; ctx.fillRect(dx, dy, dw, dh);
  ctx.strokeStyle = '#2c2420'; ctx.lineWidth = Math.max(1.5, s * 0.03); ctx.strokeRect(dx, dy, dw, dh);
  // the counter: a laminate front and a lighter top
  const kw = s * 0.27, kh = s * 0.11, ky = cy - s * 0.04;
  ctx.fillStyle = '#7a5a38'; ctx.fillRect(x - kw, ky, kw * 2, kh);
  ctx.fillStyle = '#a8865a';
  ctx.beginPath(); ctx.moveTo(x - kw, ky); ctx.lineTo(x - kw + s * 0.05, ky - s * 0.05); ctx.lineTo(x + kw + s * 0.05, ky - s * 0.05); ctx.lineTo(x + kw, ky); ctx.closePath(); ctx.fill();
  ctx.strokeStyle = '#2a1c10'; ctx.lineWidth = 1; ctx.strokeRect(x - kw, ky, kw * 2, kh);
  ctx.strokeStyle = 'rgba(60,40,20,0.6)';
  for (let i = 1; i < 4; i++) { ctx.beginPath(); ctx.moveTo(x - kw + i * kw / 2, ky + 1); ctx.lineTo(x - kw + i * kw / 2, ky + kh - 1); ctx.stroke(); }
  // the bell
  ctx.fillStyle = '#d8b450';
  ctx.beginPath(); ctx.arc(x + kw * 0.45, ky - s * 0.035, s * 0.03, Math.PI, 0); ctx.fill();
  // the lamp's stand and fringed shade (the light comes in drawVacLamp)
  const lx = x - kw * 0.6, ly = ky - s * 0.03;
  ctx.strokeStyle = '#3a2a1a'; ctx.lineWidth = Math.max(1, s * 0.02);
  ctx.beginPath(); ctx.moveTo(lx, ly); ctx.lineTo(lx, ly - s * 0.12); ctx.stroke();
  ctx.fillStyle = '#c8a070';
  ctx.beginPath(); ctx.moveTo(lx - s * 0.05, ly - s * 0.1); ctx.lineTo(lx + s * 0.05, ly - s * 0.1); ctx.lineTo(lx + s * 0.035, ly - s * 0.17); ctx.lineTo(lx - s * 0.035, ly - s * 0.17); ctx.closePath(); ctx.fill();
  ctx.restore();
}

// at: the lamp painted on the landmark tile (VAC_TILE_LIGHT), or null for the
// code-drawn desk's own lamp. A painted lamp keeps its shade; it only gets
// the glow, and a hot core once it is dark enough to need one.
function drawVacLamp(x, y, s, night, now, at) {
  const kw = s * 0.27, ky = y + s * 0.06;
  const lx = at ? x + at[0] * s : x - kw * 0.6, ly = at ? y + at[1] * s : ky - s * 0.16;
  const breathe = 0.85 + 0.15 * Math.sin(now / 1300);
  ctx.save();
  const g = ctx.createRadialGradient(lx, ly, 0, lx, ly, s * (0.35 + 0.35 * night));
  g.addColorStop(0, `rgba(255,214,150,${(0.25 + 0.45 * night) * breathe})`);
  g.addColorStop(1, 'rgba(255,214,150,0)');
  ctx.fillStyle = g;
  ctx.fillRect(lx - s, ly - s, s * 2, s * 2);
  if (at) {
    const c = ctx.createRadialGradient(lx, ly, 0, lx, ly, s * 0.07);
    c.addColorStop(0, `rgba(255,240,200,${0.75 * night * breathe})`);
    c.addColorStop(1, 'rgba(255,240,200,0)');
    ctx.fillStyle = c;
    ctx.fillRect(lx - s * 0.07, ly - s * 0.07, s * 0.14, s * 0.14);
  } else {
    ctx.fillStyle = `rgba(255,236,190,${0.7 + 0.3 * night})`;
    ctx.beginPath(); ctx.moveTo(lx - s * 0.05, ly + s * 0.06); ctx.lineTo(lx + s * 0.05, ly + s * 0.06); ctx.lineTo(lx + s * 0.035, ly - s * 0.01); ctx.lineTo(lx - s * 0.035, ly - s * 0.01); ctx.closePath(); ctx.fill();
  }
  ctx.restore();
}

// ── Housekeeping: glass doors in a ruin, and a corridor too long for it ────
function drawVacDoorway(x, y, s, now) {
  ctx.save();
  const w = s * 0.3, h = s * 0.4, L = x - w / 2, T = y - h * 0.62;
  // the corridor inside, running away to a point far behind the wall
  const vx = x + s * 0.01, vy = T + h * 0.42;
  ctx.fillStyle = '#2a0f18';  // carpet the colour of old wine
  ctx.beginPath(); ctx.moveTo(L, T + h); ctx.lineTo(L + w, T + h); ctx.lineTo(vx + 1, vy); ctx.lineTo(vx - 1, vy); ctx.closePath(); ctx.fill();
  ctx.fillStyle = '#3b3a2e';  // walls
  ctx.beginPath(); ctx.moveTo(L, T); ctx.lineTo(vx - 1, vy - 1); ctx.lineTo(vx - 1, vy); ctx.lineTo(L, T + h); ctx.closePath(); ctx.fill();
  ctx.beginPath(); ctx.moveTo(L + w, T); ctx.lineTo(vx + 1, vy - 1); ctx.lineTo(vx + 1, vy); ctx.lineTo(L + w, T + h); ctx.closePath(); ctx.fill();
  ctx.fillStyle = '#4a4a40';  // ceiling
  ctx.beginPath(); ctx.moveTo(L, T); ctx.lineTo(L + w, T); ctx.lineTo(vx + 1, vy - 1); ctx.lineTo(vx - 1, vy - 1); ctx.closePath(); ctx.fill();
  // doors down both sides, smaller and smaller, never ending
  ctx.fillStyle = '#2f8f8a';
  for (let i = 0; i < 6; i++) {
    const f = Math.pow(0.62, i), f2 = Math.pow(0.62, i + 0.55);
    for (const side of [-1, 1]) {
      const ex = side < 0 ? L : L + w;
      const ax = vx + (ex - vx) * f, bx = vx + (ex - vx) * f2;
      const ayT = vy + (T - vy) * f * 0.75, ayB = vy + (T + h - vy) * f;
      const byT = vy + (T - vy) * f2 * 0.75, byB = vy + (T + h - vy) * f2;
      ctx.beginPath(); ctx.moveTo(ax, ayT); ctx.lineTo(bx, byT); ctx.lineTo(bx, byB); ctx.lineTo(ax, ayB); ctx.closePath(); ctx.fill();
    }
  }
  // the frame and the two glass doors standing open
  ctx.strokeStyle = '#1a1814'; ctx.lineWidth = Math.max(1.5, s * 0.035);
  ctx.strokeRect(L, T, w, h);
  ctx.strokeStyle = 'rgba(190,220,215,0.45)'; ctx.lineWidth = Math.max(1, s * 0.015);
  ctx.beginPath(); ctx.moveTo(L, T); ctx.lineTo(L - s * 0.06, T + s * 0.03); ctx.lineTo(L - s * 0.06, T + h + s * 0.02); ctx.lineTo(L, T + h); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(L + w, T); ctx.lineTo(L + w + s * 0.06, T + s * 0.03); ctx.lineTo(L + w + s * 0.06, T + h + s * 0.02); ctx.lineTo(L + w, T + h); ctx.stroke();
  // WELCOME, on the mat
  ctx.fillStyle = '#3c3428'; ctx.fillRect(x - w * 0.4, T + h + s * 0.01, w * 0.8, s * 0.05);
  ctx.restore();
}

// at: the corridor painted on the landmark tile (VAC_TILE_LIGHT), or null for
// the code-drawn doorway. The painted corridor already has its strip light;
// it only gets the spill, stuttering the same way.
function drawVacStrip(x, y, s, night, now, at) {
  const w = s * 0.3, h = s * 0.4, T = y - h * 0.62;
  // the strip light stutters the way they do
  const on = vacHash(Math.floor(now / 110), 3) > 0.06;
  if (!on) return;
  ctx.save();
  const gx = at ? x + at[0] * s : x, gy = at ? y + at[1] * s : T + h * 0.7;
  const g = ctx.createRadialGradient(gx, gy, 0, gx, gy, s * (0.35 + 0.3 * night));
  g.addColorStop(0, `rgba(215,255,232,${0.18 + 0.4 * night})`);
  g.addColorStop(1, 'rgba(215,255,232,0)');
  ctx.fillStyle = g;
  ctx.fillRect(gx - s, gy - s, s * 2, s * 2);
  if (at) { ctx.restore(); return; }
  ctx.strokeStyle = `rgba(230,255,240,${0.75 + 0.25 * night})`;
  ctx.lineWidth = Math.max(1, s * 0.02);
  ctx.beginPath(); ctx.moveTo(x - w * 0.3, T + h * 0.08); ctx.lineTo(x + w * 0.3, T + h * 0.08); ctx.stroke();
  ctx.restore();
}

// ── Events (network.js dispatch) ───────────────────────────────────────────
// ev vac: w = hook | desk | keys | room | saved | fallen | lost | hum.
// The hum is unicast (has = what we DO carry); the rest are the world's.
function _evVac(ev) {
  const mine = ev.pid === myId;
  const who = mine ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const rm = ev.rm | 0;
  const nights = `${rm} night${rm === 1 ? '' : 's'}`;
  switch (ev.w) {
    case 'hook': {
      addLog(`<span class="log-col">⌂ ${escHtml(who)} heard the town under the sign out. It is counting: ${nights} before it is full.</span>`);
      addLog('<span class="log-mv">⌂ Bring back the guest register from a front desk standing in the open, and the master key from a corridor in a ruin.</span>');
      showBanner('VACANCY', `${rm} rooms left`);
      break;
    }
    case 'desk':
      addLog(`<span class="log-col">⌂ ${escHtml(who)} took the guest register off the front desk.</span>`);
      if (!mine) showToast('⌂ Somebody has the register. The desk is gone.', 'vac');
      break;
    case 'keys':
      addLog(`<span class="log-col">⌂ ${escHtml(who)} came back out of the corridor with the master key.</span>`);
      if (!mine) showToast('⌂ Somebody has the master key. The corridor is shut.', 'vac');
      break;
    case 'room':
      addLog(`<span class="log-check-fail">⌂ Another house checked in overnight. ${rm} room${rm === 1 ? '' : 's'} left.</span>`);
      showToast(rm <= 2 ? `The NO is warming up. ${rm} left.` : `Another window lit. ${rm} rooms left.`, 'vac');
      break;
    case 'saved':
      addLog(`<span class="log-col">⌂ ${escHtml(who)} checked the whole town out. The sign is dark.</span>`);
      showBanner('THE SIGN IS DARK', 'every chair has somebody on it');
      break;
    case 'fallen':
      // The town checked in: its hex is a dead motel now (Broken Urban).
      if (gameMap[ev.r]?.[ev.q]) gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], terrain: 4, poi: 0 };
      addLog('<span class="log-check-fail">⌂ NO VACANCY. The town under the sign checked in, every last one of them.</span>');
      showBanner('NO VACANCY', 'the town is full');
      break;
    case 'lost':
      addLog('<span class="log-check-fail">⌂ The town under the sign is gone, and the sign went with it.</span>');
      break;
    case 'hum': {
      const has = ev.has | 0;
      const want = has === 1 ? 'You have the register. It wants the master key too.'
                 : has === 2 ? 'You have the master key. It wants the register too.'
                 : 'It wants the register off the front desk, and the master key from the corridor.';
      addLog(`<span class="log-mv">⌂ The sign hums at you. ${want}</span>`);
      showToast(`The sign hums. ${want}`, 'vac');
      break;
    }
  }
  if (mine) updateSidebar();
}
