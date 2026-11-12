// ── ghosts.js ──────────────────────────────────────────────────────────────
// Wasteland ghosts on the map (ghosts.hpp): one rises where a survivor fell,
// drifts toward the living, and rides whoever it catches -- a token off their
// pack every couple of minutes and a third of everything they gain -- until
// they walk into a Settlement. Seven dawns on it fades and drops its hoard.
//
// State is worldState.ghosts (the server's "ghosts" array, merged in by
// _applyWorldState in network.js): [slot, q, r, haunt, state, hoard, src,
// name] per live ghost. haunt -1 = drifting; state 0 drifting / 1 rising /
// 2 shunning (just shaken off at a town). Events: ev "ghost", w = rise /
// latch / letgo / drain / tithe / fade (_evGhost below).
//
// Everyone is told where they are. A drifting ghost is drawn only inside your
// own sight, like Kane's jeep; a riding one wherever its survivor's pawn is,
// looming behind the portrait with its hands over the rim
// (renderCharacters calls ghostBehindPawn / ghostOverPawn round each pawn).
// Drawn with math, no sprite: a hooded shroud of black smoke that barely
// holds its shape, shedding mist that streams off with the wind; a black
// skull looks out of the hood, two cold coals in its sockets in the colour of
// whoever it was.

const GHOST_R       = 0.15;   // drifting: hood radius in HEX_SZ (the Doom's skull is 0.2)
const GHOST_RIDE_K  = 0.6;    // riding: hood radius as a fraction of the portrait's
const GHOST_GLIDE   = 0.05;   // per-frame lerp to a new hex: a third of a pawn's -- it drifts
const GHOST_RISE_MS = 2400;   // getting up off the body
const GHOST_GRAB_MS = 650;    // swooping from its hex onto the survivor it caught
// How much of it is there: the smoke, the mist and the skull (and its shadow
// on a face) are drawn at this share; the eyes are light and keep their own.
// Halved at the user's call, 2026-10-23, then taken down to 0.3 the same day.
const GHOST_BODY_ALPHA = 0.3;

// Slot-indexed: the live ghosts, and what the client remembers of each (the
// name outlives the slot, since a fade's event drains after the slot clears).
let   ghostLive  = [];               // [{ g, q, r, h, s, hd, src, name }]
const ghostMeta  = [];               // slot -> { name, src, rq, rr, seen, riseAt, grabAt, sx, sy, sR }
let   ghostOnMe  = -1;               // slot riding us, -1 none

function ghostMetaFor(g) {
  return ghostMeta[g] || (ghostMeta[g] = { name: '', src: 0, rq: 0, rr: 0, seen: false, riseAt: 0, grabAt: 0, sx: 0, sy: 0, sR: 0 });
}
function ghostWho(slot, src) {
  const m = ghostMeta[slot];
  const nm = m?.name || players[src ?? m?.src]?.nm;
  return nm ? `${nm}’s ghost` : 'A ghost';
}
const ghostRes = (res) => (RES_NAMES[res] || 'token').toLowerCase();

// ── World state ─────────────────────────────────────────────────────────────
function ghostApplyWorld() {
  const arr = Array.isArray(worldState.ghosts) ? worldState.ghosts : [];
  ghostLive = arr.map(([g, q, r, h, s, hd, src, name]) => ({ g, q, r, h, s, hd, src, name }));
  let mine = -1;
  for (const gh of ghostLive) {
    const m = ghostMetaFor(gh.g);
    if (m.name !== gh.name) {
      // A new ghost in this slot: start it where it is, rising if it is.
      m.seen = false;
      m.riseAt = gh.s === 1 ? performance.now() : 0;
    }
    m.name = gh.name; m.src = gh.src;
    if (!m.seen) { m.rq = gh.q; m.rr = gh.r; m.seen = true; }
    if (gh.h >= 0 && gh.h === myId) mine = gh.g;
  }
  if (mine !== ghostOnMe) { ghostOnMe = mine; ghostUpdateHud(); }
}

function ghostUpdateHud() {
  const el = document.getElementById('hud-haunt');
  if (!el) return;
  const on = ghostOnMe >= 0;
  el.style.display = on ? 'inline' : 'none';
  if (on) {
    el.textContent = '☠ HAUNTED';
    el.title = `${ghostWho(ghostOnMe)} rides you: a token every few minutes and a third of what you gain. Walk into a Settlement to shake it off.`;
  }
  document.body.classList.toggle('haunted', on);
}

function ghostInSight(q, r) {
  if (myId < 0 || !players[myId]?.on) return true;
  const me = players[myId];
  return hexDistWrap(me.q, me.r, q, r) <= (myVisionR ?? 3);
}

function lerpGhost(m, q, r) {
  let tq = q, tr = r;
  while (tq - m.rq >  MAP_COLS / 2) tq -= MAP_COLS;
  while (m.rq - tq >  MAP_COLS / 2) tq += MAP_COLS;
  while (tr - m.rr >  MAP_ROWS / 2) tr -= MAP_ROWS;
  while (m.rr - tr >  MAP_ROWS / 2) tr += MAP_ROWS;
  const dq = (tq - m.rq) * GHOST_GLIDE, dr = (tr - m.rr) * GHOST_GLIDE;
  m.rq += dq; m.rr += dr;
  return hexToPixel(dq, dr, 1);
}

// ── The figure ──────────────────────────────────────────────────────────────
const GhostWisp = (() => {
  const PERM = new Uint8Array(512);
  {
    const p = Array.from({ length: 256 }, (_, i) => i);
    let s = 0x1F3D5B79;
    for (let i = 255; i > 0; i--) {
      s ^= s << 13; s ^= s >>> 17; s ^= s << 5; s >>>= 0;
      const j = s % (i + 1), tmp = p[i]; p[i] = p[j]; p[j] = tmp;
    }
    for (let i = 0; i < 512; i++) PERM[i] = p[i & 255];
  }
  function vn(x, y) {
    const xf = Math.floor(x), yf = Math.floor(y);
    const xi = xf & 255, yi = yf & 255;
    let fx = x - xf, fy = y - yf;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    const p0 = PERM[xi], p1 = PERM[xi + 1];
    const a = PERM[p0 + yi], b = PERM[p1 + yi], c = PERM[p0 + yi + 1], d = PERM[p1 + yi + 1];
    const top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return (top + (bot - top) * fy) * (1 / 255);
  }
  const fbm2 = (x, y) => vn(x, y) * 0.62 + vn(x * 2.03 + 17.1, y * 2.03 - 9.7) * 0.38;
  const clamp01 = v => v < 0 ? 0 : v > 1 ? 1 : v;
  const ss = (e0, e1, x) => { const t = clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); };
  const smin = (a, b, k) => { const h = Math.max(k - Math.abs(a - b), 0) / k; return Math.min(a, b) - h * h * k * 0.25; };

  // A tapering capsule from (ax, ay) to (bx, by), radius ra at a and rb at b.
  // Returns the distance; ARM_T is how far along it the nearest point sits.
  let ARM_T = 0;
  function sdArm(x, y, ax, ay, bx, by, ra, rb) {
    const vx = bx - ax, vy = by - ay;
    const t = clamp01(((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy));
    ARM_T = t;
    return Math.hypot(x - ax - vx * t, y - ay - vy * t) - (ra + (rb - ra) * t);
  }

  const sdBox = (x, y, hw, hh, rr) => {
    const dx = Math.abs(x) - hw + rr, dy = Math.abs(y) - hh + rr;
    return Math.hypot(Math.max(dx, 0), Math.max(dy, 0)) + Math.min(Math.max(dx, dy), 0) - rr;
  };
  const sdEll = (dx, dy, rx, ry, a) => {
    const ca = Math.cos(a), sa = Math.sin(a);
    const u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
    return (Math.hypot(u / rx, v / ry) - 1) * Math.min(rx, ry);
  };

  // The skull in the hood. Its own units (SKULL_S of R), y down, origin
  // mid-face, centred SKULL_Y below mid-hood. Returns the bone's distance and
  // leaves the holes' (sockets, nose, the gaps in the teeth) in SK_CAV.
  // jaw 0..~0.2 drops the lower jaw: it moans, slowly.
  const SKULL_S = 0.9, SKULL_Y = 0.14;
  let SK_CAV = 0;
  function skull(u, v, jaw) {
    const cran  = Math.hypot(u * 1.02, v + 0.2) - 0.64;
    const face  = sdBox(u, v - 0.26, 0.44, 0.2, 0.15);
    const cheek = Math.min(Math.hypot(u + 0.42, v - 0.2), Math.hypot(u - 0.42, v - 0.2)) - 0.12;
    const jy    = 0.66 + jaw * 0.3;
    const taper = 1 - 0.3 * clamp01((v - jy + 0.06) / 0.2);
    const jawd  = sdBox(u / taper, v - jy, 0.31, 0.13, 0.1) * taper;
    const bone  = smin(smin(smin(cran, face, 0.2), cheek, 0.1), jawd, 0.08);
    // Sockets tilted inner-end-down: it scowls.
    const sL = sdEll(u + 0.25, v - 0.07, 0.19, 0.16, 0.35);
    const sR = sdEll(u - 0.25, v - 0.07, 0.19, 0.16, -0.35);
    const nv = v - 0.24;
    const nose = Math.max(Math.abs(u) - (0.012 + 0.08 * nv / 0.15), -nv, nv - 0.15);
    const my = 0.535 + jaw * 0.15;
    let cav = Math.min(sL, sR, nose, sdBox(u, v - my, 0.27, 0.012 + jaw * 0.15, 0.005));
    if (Math.abs(u) < 0.27) {
      const up = v > 0.45 && v < 0.52, lo = v > 0.55 + jaw * 0.3 && v < 0.61 + jaw * 0.3;
      if (up || lo) {
        const f = ((u + 0.3) / 0.09) % 1;
        cav = Math.min(cav, Math.abs(f - 0.5) * 0.09 - 0.008);   // a slit between each tooth
      }
    }
    SK_CAV = cav;
    return bone;
  }

  // The skull in its own field, sharper than the smoke round it: obsidian,
  // black bone whose edges catch a cold sheen from the upper left -- the
  // dome, the brow, the cheekbones, the rims of its holes, the teeth -- so a
  // black skull still reads against the black inside of the hood. Box in
  // skull units; pxR = device px per R.
  let SKF = null;
  const SKX0 = -0.8, SKX1 = 0.8, SKY0 = -0.95, SKY1 = 1.0;
  function skullField(t, pxR, seed, jaw) {
    const ppu = pxR * SKULL_S;
    const W = Math.max(8, Math.min(72, Math.ceil((SKX1 - SKX0) * ppu)));
    const H = Math.max(9, Math.min(88, Math.ceil((SKY1 - SKY0) * ppu)));
    if (!SKF || SKF.W !== W || SKF.H !== H) SKF = field(W, H);
    const d = SKF.img.data;
    const soft = Math.max(0.02, 0.9 / ppu), e = 0.02;
    const LX = -0.55, LY = -0.83;   // toward the light
    let k = 0;
    for (let j = 0; j < H; j++) {
      const v = SKY0 + (j + 0.5) / H * (SKY1 - SKY0);
      for (let i = 0; i < W; i++, k += 4) {
        // The smoke breathes through it, a little.
        const u = SKX0 + (i + 0.5) / W * (SKX1 - SKX0) + (vn(v * 4 + t * 0.5 + seed, t * 0.3) - 0.5) * 0.03;
        const bone = skull(u, v, jaw), cav = SK_CAV;
        if (bone > soft) { d[k + 3] = 0; continue; }
        const a = ss(soft, -soft, bone), hole = ss(soft, -soft, cav);
        // Bevel: the shape is bone minus its holes; its outward gradient
        // facing the light is the sheen, just inside every edge.
        const s0 = Math.max(bone, -cav);
        const sx = Math.max(skull(u + e, v, jaw), -SK_CAV);
        const sy = Math.max(skull(u, v + e, jaw), -SK_CAV);
        let gx = (sx - s0) / e, gy = (sy - s0) / e;
        const gl = Math.hypot(gx, gy) || 1; gx /= gl; gy /= gl;
        const sheen = clamp01(gx * LX + gy * LY) * ss(-0.13, -0.005, s0);
        const gloss = ss(0.42, 0.0, Math.hypot(u + 0.2, v + 0.42)) * 0.45;
        const lit = Math.min(1, sheen * 0.95 + gloss) * (1 - hole);
        d[k]     = (17 + 62 * lit) * (1 - hole);
        d[k + 1] = (18 + 68 * lit) * (1 - hole);
        d[k + 2] = (23 + 82 * lit) * (1 - hole);
        d[k + 3] = a * 0.97 * 255;
      }
    }
    SKF.g.putImageData(SKF.img, 0, 0);
    return SKF;
  }

  // The shroud's centre line: it leans the way it goes and its tail swings.
  const spine = (y, P, t) => P.lean * y * 0.22 + P.sway * Math.pow(clamp01((y - 0.2) / 3.8), 1.6) * Math.sin(t * 1.5 - y * 1.25);
  // Half-width down the robe: narrow shoulders by y 1.2, a rag of a tail by 4.
  const halfW = (y) => 0.82 + 0.26 * ss(0.2, 1.2, y) - 1.0 * ss(1.3, 4.0, y);

  // Units R (hood radius), y down, origin mid-hood. Returns { d, face, armT }:
  // the figure's distance, the hood hollow's, and how far down an arm the
  // nearest point is (armsOnly: the forearms-and-claws pass over a portrait).
  const OUT = { d: 0, face: 0, armT: 0 };
  function figure(x, y, P, t, armsOnly) {
    const c09 = spine(0.9, P, t);
    let arm = 1e9, armT = 0;
    for (let k = 0; k < 2; k++) {
      const sx = (k ? 0.7 : -0.7) + c09, sy = 0.95;
      const h  = P.hands[k];
      let da = sdArm(x, y, sx, sy, h[0], h[1], 0.25, 0.1);
      let ta = ARM_T;
      if (armsOnly && ta < 0.3) da = 1e9;
      // Three long fingers off the hand, splayed round the way the arm runs.
      const ax = h[0] - sx, ay = h[1] - sy, al = Math.hypot(ax, ay) || 1;
      const ux = ax / al, uy = ay / al;
      for (let f = -1; f <= 1; f++) {
        const a = f * 0.42, ca = Math.cos(a), sa = Math.sin(a);
        const fx = h[0] + (ux * ca - uy * sa) * 0.5, fy = h[1] + (ux * sa + uy * ca) * 0.5;
        const df = sdArm(x, y, h[0], h[1], fx, fy, 0.09, 0.025);
        if (df < da) { da = df; ta = 1; }
      }
      if (da < arm) { arm = da; armT = ta; }
    }
    OUT.armT = armT;
    if (armsOnly) { OUT.d = arm; OUT.face = 1e9; return OUT; }
    const hx = x - P.lean * 0.1;
    // The hood: round, with a slack peak that droops toward where it is going.
    const hood = Math.hypot(hx, (y + 0.04) / 1.08) - 1;
    const peak = Math.hypot((hx + P.lean * 0.35) * 1.6, y + 0.85) - 0.55;
    let d = smin(hood, peak, 0.35);
    if (y > 0.1) {
      const c = spine(y, P, t);
      let robe = (Math.abs(x - c) - halfW(y)) * 0.85;
      robe = Math.max(robe, y - 4.1);
      d = smin(d, robe, 0.5);
    }
    d = smin(d, arm, 0.22);
    OUT.d = d;
    // The shadowed inside of the hood the skull looks out of.
    OUT.face = Math.hypot((hx - P.lean * 0.16) / 0.8, (y - 0.14) / 0.96) - 1;
    return OUT;
  }

  function field(W, H) {
    const cv = document.createElement('canvas');
    cv.width = W; cv.height = H;
    const g = cv.getContext('2d');
    return { cv, g, img: g.createImageData(W, H), W, H };
  }
  let F = null, M = null;
  // The box the figure lives in, in R: x -2.6..2.6, y -1.9..4.6.
  const X0 = -2.6, X1 = 2.6, Y0 = -1.9, Y1 = 4.6;
  // The mist's box, wider: it sheds well past the robe.
  const MX0 = -3.8, MX1 = 3.8, MY0 = -3.4, MY1 = 5.6;

  // The shroud: black smoke that only just holds a shape. Solid enough in the
  // hood for the eyes to sit in, thinning down the robe and tearing apart at
  // the hem and the fingers.
  function paint(P, t, ppr, armsOnly, seed) {
    const W = Math.max(16, Math.min(80, Math.ceil((X1 - X0) * ppr)));
    const H = Math.max(20, Math.min(96, Math.ceil((Y1 - Y0) * ppr)));
    if (!F || F.W !== W || F.H !== H) F = field(W, H);
    const d = F.img.data;
    const soft = Math.max(0.16, 1.6 / ppr);
    let k = 0;
    for (let j = 0; j < H; j++) {
      const y0 = Y0 + (j + 0.5) / H * (Y1 - Y0);
      for (let i = 0; i < W; i++, k += 4) {
        const x0 = X0 + (i + 0.5) / W * (X1 - X0);
        // Drifting smoke: warp the plane by slow noise, harder toward the hem.
        const hemW = 0.14 + 0.30 * ss(0.8, 3.6, y0);
        const wx = (fbm2(x0 * 1.3 + seed + t * 0.23, y0 * 1.1 - t * 0.41) - 0.5) * 2 * hemW;
        const wy = (fbm2(x0 * 1.3 - 7.7 - t * 0.19, y0 * 1.1 + seed + 3.1 - t * 0.33) - 0.5) * 2 * hemW * 0.6;
        const x = x0 + wx, y = y0 + wy;
        const f = figure(x, y, P, t, armsOnly);
        if (f.d > 0.4) { d[k + 3] = 0; continue; }
        // The edge boils: its own noise eats in and out of the outline.
        const boil = (fbm2(x * 3.1 - t * 0.6 + seed, y * 3.1 + t * 0.45) - 0.5) * 0.34;
        let a = ss(soft, -soft, f.d + boil);
        // The hem and the fingers tear into strands.
        const rg = 1 - Math.abs(2 * fbm2(x * 2.4 + seed, y * 1.3 - t * 0.9) - 1);
        const tear = armsOnly ? ss(0.7, 1, f.armT) * 0.7 : ss(1.2, 4.0, y);
        a *= 1 - tear + tear * ss(0.45, 0.82, rg + 0.15 * (1 - tear));
        // Thin where the smoke thins: the hood holds, the robe barely does.
        const veil = 0.35 + 0.65 * ss(0.28, 0.72, fbm2(x * 2.1 - t * 0.12 + seed, y * 2.1 + t * 0.27));
        const holdK = ss(2.2, 0.4, y);   // 1 in the hood .. 0 by the knees
        const alpha = a * (holdK * 0.92 + (1 - holdK) * veil * 0.85);
        if (alpha < 0.004) { d[k + 3] = 0; continue; }
        const inHood = !armsOnly ? ss(0.06, -0.06, f.face) * ss(0.3, -0.2, f.d) : 0;
        // Charcoal, its folds catching a little cold light; the hood's inside
        // a true black for the skull (skullField) to catch its light against.
        const lum = clamp01(0.45 + (fbm2(x * 3.3 + t * 0.2, y * 3.3 - t * 0.35) - 0.5) * 1.4 - Math.max(0, f.d) * 1.2);
        let rr = 12 + 44 * lum, gg = 13 + 45 * lum, bb = 17 + 52 * lum;
        rr += (2 - rr) * inHood; gg += (2 - gg) * inHood; bb += (4 - bb) * inHood;
        d[k] = rr; d[k + 1] = gg; d[k + 2] = bb;
        d[k + 3] = Math.min(1, alpha + inHood * 0.3 * a) * 255;
      }
    }
    F.g.putImageData(F.img, 0, 0);
    return F;
  }

  // The mist it sheds: a low-res smoke field round the robe that streams off
  // the way the wind and its own drift take it, torn into strands like the
  // Doom's gas (DoomGas.smokeField). Dark: smoke on pale ground, a deeper
  // dark on dark ground. (bx, by): unit vector the mist streams along.
  function mist(P, t, mpr, seed, bx, by) {
    const W = Math.max(12, Math.min(64, Math.ceil((MX1 - MX0) * mpr)));
    const H = Math.max(14, Math.min(80, Math.ceil((MY1 - MY0) * mpr)));
    if (!M || M.W !== W || M.H !== H) M = field(W, H);
    const d = M.img.data;
    let k = 0;
    for (let j = 0; j < H; j++) {
      const y = MY0 + (j + 0.5) / H * (MY1 - MY0);
      for (let i = 0; i < W; i++, k += 4) {
        const x = MX0 + (i + 0.5) / W * (MX1 - MX0);
        // Round the robe, then the stream off the shoulders.
        const ex = (x - P.lean * 0.3) / 1.8, ey = (y - 1.5) / 3.0;
        let env = Math.exp(-(ex * ex + ey * ey) * 1.5);
        const sx = x, sy = y - 0.9;
        const s = sx * bx + sy * by, dp = -sx * by + sy * bx;
        if (s > 0) { const w = 0.75 + 0.3 * s; env = Math.max(env, Math.exp(-(dp * dp) / (w * w) - s / 1.9) * 0.85); }
        // Faded out well inside the box, so no edge of it ever shows.
        env *= ss(0, 1.4, x - MX0) * ss(0, 1.4, MX1 - x) * ss(0, 1.6, y - MY0) * ss(0, 1.2, MY1 - y);
        if (env < 0.02) { d[k + 3] = 0; continue; }
        const ax = x - bx * t * 0.55, ay = y - by * t * 0.55;
        const n  = fbm2(ax * 1.15 + seed, ay * 1.15 - t * 0.12);
        const rg = 1 - Math.abs(2 * fbm2(ax * 2.2 - 5.3 + seed, ay * 2.2 + 2.2) - 1);
        const body   = env * ss(0.34, 0.74, n + env * 0.3);
        const strand = env * ss(0.70, 0.93, rg) * 0.8;
        const dens = Math.min(1, body * 0.7 + strand);
        const lit  = ss(0.2, 0.9, strand + body * 0.2);
        d[k]     = 16 + 30 * lit;
        d[k + 1] = 17 + 32 * lit;
        d[k + 2] = 21 + 38 * lit;
        d[k + 3] = dens * 0.62 * 255;
      }
    }
    M.g.putImageData(M.img, 0, 0);
    return M;
  }

  // cx, cy: mid-hood on screen; R: hood radius px. P: { lean, sway, hands,
  // eye, eyeK, alpha, rise } -- hands in R from mid-hood, eye a #rrggbb.
  function draw(ctx, cx, cy, R, P, now, dpr, armsOnly = false, seed = 0) {
    const t = now / 1000;
    const ppr = Math.max(4, Math.min(14, R * Math.min(dpr, 2)));
    const rise = P.rise ?? 1;
    ctx.save();
    ctx.globalAlpha = P.alpha * rise * GHOST_BODY_ALPHA;
    const prevSmooth = ctx.imageSmoothingEnabled;
    ctx.imageSmoothingEnabled = true;
    // Rising: it stands up off the ground, the hem first.
    const sy = 0.25 + 0.75 * rise;
    const groundY = cy + Y1 * R * 0.85;
    ctx.translate(cx, groundY);
    ctx.scale(1, sy);
    ctx.translate(-cx, -groundY);
    if (!armsOnly) {
      // A pool of dark round it, the light going out where it hangs.
      const gR = R * 3.0, gy = cy + R * 1.4;
      const gg = ctx.createRadialGradient(cx, gy, 0, cx, gy, gR);
      gg.addColorStop(0, 'rgba(0,0,0,0.30)');
      gg.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = gg;
      ctx.fillRect(cx - gR, gy - gR, gR * 2, gR * 2);
      // The mist streams back off the way it goes, up, and with the wind.
      const wx = typeof Wind !== 'undefined' ? Wind.x * Wind.s : 0;
      const wy = typeof Wind !== 'undefined' ? Wind.y * Wind.s : 0;
      let bx = -P.lean * 0.7 + wx * 0.9, by = -0.45 + wy * 0.6;
      const bl = Math.hypot(bx, by) || 1; bx /= bl; by /= bl;
      const mf = mist(P, t, Math.max(3, Math.min(8, ppr / 1.8)), seed, bx, by);
      ctx.drawImage(mf.cv, cx + MX0 * R, cy + MY0 * R, (MX1 - MX0) * R, (MY1 - MY0) * R);
    }
    const fld = paint(P, t, ppr, armsOnly, seed);
    ctx.drawImage(fld.cv, cx + X0 * R, cy + Y0 * R, (X1 - X0) * R, (Y1 - Y0) * R);
    if (!armsOnly) {
      // The skull looking out of the hood, sharper than the smoke.
      const jaw = 0.05 + 0.05 * Math.sin(t * 1.3 + seed);
      const sk = skullField(t, Math.min(48, R * Math.min(dpr, 2)), seed, jaw);
      const S = SKULL_S * R;
      ctx.drawImage(sk.cv, cx + P.lean * 0.2 * R + SKX0 * S, cy + SKULL_Y * R + SKY0 * S,
                    (SKX1 - SKX0) * S, (SKY1 - SKY0) * S);
    }
    ctx.imageSmoothingEnabled = prevSmooth;
    if (!armsOnly) {
      // The eyes: two cold coals in the colour of who it was -- on a black
      // ghost, the thing you see first.
      // In the skull's sockets (skull(): +-0.25, 0.07 of SKULL_S).
      const ex = cx + P.lean * 0.2 * R, ey = cy + (SKULL_Y + 0.07 * SKULL_S) * R;
      const fl = 0.75 + 0.25 * vn(t * 5 + seed, 2.7);
      ctx.globalCompositeOperation = 'lighter';
      for (let e = 0; e < 2; e++) {
        const x = ex + (e ? 0.25 : -0.25) * SKULL_S * R, y = ey;
        const er = R * 0.24;
        const g = ctx.createRadialGradient(x, y, 0, x, y, er);
        g.addColorStop(0, `${P.eye}ee`);
        g.addColorStop(0.3, `${P.eye}66`);
        g.addColorStop(1, `${P.eye}00`);
        ctx.globalAlpha = P.alpha * rise * fl * P.eyeK;
        ctx.fillStyle = g;
        ctx.fillRect(x - er, y - er, er * 2, er * 2);
        ctx.beginPath();
        ctx.arc(x, y, Math.max(0.7, R * 0.05), 0, Math.PI * 2);
        ctx.fillStyle = '#f2f6ff';
        ctx.fill();
      }
    }
    ctx.restore();
  }
  return { draw, vn };
})();

// The survivor's colour, gone a little cold: the eyes keep most of it, so on
// a black ghost you can tell whose it is.
function ghostEye(src) {
  const hex = ARCHETYPE_COLORS[src] || '#c8d8e8';
  const n = parseInt(hex.slice(1), 16);
  const mix = (c) => Math.round(c * 0.7 + 225 * 0.3).toString(16).padStart(2, '0');
  return `#${mix((n >> 16) & 255)}${mix((n >> 8) & 255)}${mix(n & 255)}`;
}

// The 'ghosts' layer: drifting ghosts, under the pawns -- a survivor who walks
// through one stays in front of it. Riding ones are drawn round their pawn.
function renderGhosts(cam) {
  if (!ghostLive.length) return;
  const now = performance.now(), dpr = window.devicePixelRatio || 1;
  for (const gh of ghostLive) {
    if (gh.h >= 0) continue;
    const m = ghostMetaFor(gh.g);
    const v = lerpGhost(m, gh.q, gh.r);
    if (!ghostInSight(gh.q, gh.r)) continue;
    const { vq, vr } = closestWrapCoords({ q: m.rq, r: m.rr }, cam.meRp);
    const pp = hexToPixel(vq, vr, HEX_SZ);
    const R = Math.max(6, HEX_SZ * GHOST_R);
    const bob = Math.sin(now / 1000 * 1.1 + gh.g * 1.7) * 0.12;
    const cx = pp.x + cam.ox, cy = pp.y + cam.oy - R * (1.9 + bob);
    if (cx < -R * 6 || cx > cssWidth + R * 6 || cy < -R * 6 || cy > cssHeight + R * 8) continue;
    m.sx = cx; m.sy = cy; m.sR = R;
    const vl = Math.hypot(v.x, v.y);
    const lean = vl > 1e-4 ? Math.max(-1, Math.min(1, v.x / vl)) * Math.min(1, vl * 40) : 0;
    const wx = typeof Wind !== 'undefined' ? Wind.x * Wind.s : 0;
    const rise = m.riseAt ? Math.min(1, (now - m.riseAt) / GHOST_RISE_MS) : 1;
    GhostWisp.draw(ctx, cx, cy, R, {
      lean, sway: 0.35 + 0.25 * Math.abs(wx) - lean * 0.2,
      // Arms out from the robe, a little forward of it: reaching.
      hands: [[-1.35 + lean * 0.5, 2.0], [1.35 + lean * 0.5, 2.0]],
      eye: ghostEye(gh.src), eyeK: gh.s === 2 ? 0.35 : 1,
      alpha: gh.s === 2 ? 0.42 : gh.s === 1 ? 0.6 : 0.78, rise,
    }, now, dpr, false, gh.g * 13.7);
  }
}

// Riding: the hood over the survivor's shoulder, the robe down behind the
// portrait, both hands on its rim. px/py/pr: the portrait circle on screen
// (renderCharacters hands us last frame's offset before the pawn, and this
// frame's exact circle after it).
function ghostRidePose(gh, px, py, pr, now) {
  const m = ghostMetaFor(gh.g);
  const R = pr * GHOST_RIDE_K;
  const bob = Math.sin(now / 1000 * 1.3 + gh.g) * 0.05;
  let cx = px - pr * 0.62, cy = py - pr * (1.2 + bob);
  // One hand gripping the near rim, the other arm laid over the top of them.
  const hands = [[(-pr * 0.92 - (cx - px)) / R, (py + pr * 0.05 - cy) / R],
                 [( pr * 0.55 - (cx - px)) / R, (py - pr * 0.55 - cy) / R]];
  // Swooping in from where it hung when it caught them.
  if (m.grabAt && now - m.grabAt < GHOST_GRAB_MS && m.sR) {
    const k = (now - m.grabAt) / GHOST_GRAB_MS, e = k * k * (3 - 2 * k);
    cx = m.sx + (cx - m.sx) * e; cy = m.sy + (cy - m.sy) * e;
    return { cx, cy, R: m.sR + (R - m.sR) * e, hands };
  }
  return { cx, cy, R, hands };
}

function ghostRiding(pid) {
  for (const gh of ghostLive) if (gh.h === pid) return gh;
  return null;
}

function ghostBehindPawn(pid, px, py, pr) {
  const gh = ghostRiding(pid);
  if (!gh) return;
  const now = performance.now();
  const { cx, cy, R, hands } = ghostRidePose(gh, px, py, pr, now);
  GhostWisp.draw(ctx, cx, cy, R, {
    lean: 0.35, sway: 0.25, hands, eye: ghostEye(gh.src), eyeK: 1, alpha: 0.82, rise: 1,
  }, now, window.devicePixelRatio || 1, false, gh.g * 13.7);
}

function ghostOverPawn(pid, px, py, pr) {
  const gh = ghostRiding(pid);
  if (!gh) return;
  const now = performance.now();
  const { cx, cy, R, hands } = ghostRidePose(gh, px, py, pr, now);
  const m = ghostMetaFor(gh.g);
  // Where it is drawn, for the swoop off again when it lets go.
  m.sx = cx; m.sy = cy; m.sR = R;
  m.rq = players[pid]?.q ?? m.rq; m.rr = players[pid]?.r ?? m.rr;
  if (renderPos[pid]) { m.rq = renderPos[pid].q; m.rr = renderPos[pid].r; }
  // Its shadow on their face, heaviest on the side it hangs from: what still
  // reads when the ghost is a few pixels.
  ctx.save();
  ctx.beginPath();
  ctx.arc(px, py, pr, 0, Math.PI * 2);
  ctx.clip();
  const wash = ctx.createRadialGradient(px - pr * 0.6, py - pr * 0.7, 0, px, py, pr * 1.3);
  wash.addColorStop(0, `rgba(0,0,0,${(0.55 + 0.08 * Math.sin(now / 1000 * 1.3)) * GHOST_BODY_ALPHA})`);
  wash.addColorStop(1, `rgba(0,0,0,${0.12 * GHOST_BODY_ALPHA})`);
  ctx.fillStyle = wash;
  ctx.fillRect(px - pr, py - pr, pr * 2, pr * 2);
  ctx.restore();
  GhostWisp.draw(ctx, cx, cy, R, {
    lean: 0.35, sway: 0.25, hands, eye: '#ffffff', eyeK: 0, alpha: 0.9, rise: 1,
  }, now, window.devicePixelRatio || 1, true, gh.g * 13.7);
}

// ── Events (network.js dispatch) ───────────────────────────────────────────
function _evGhost(ev) {
  const mine = ev.pid >= 0 && ev.pid === myId;
  const whoP = ev.pid >= 0 ? (mine ? 'you' : (players[ev.pid]?.nm || `P${ev.pid}`)) : '';
  const m = ghostMetaFor(ev.g);
  if (ev.w === 'rise') {
    m.name = ev.name || m.name; m.src = ev.src;
    m.rq = ev.q; m.rr = ev.r; m.seen = true; m.riseAt = performance.now();
  }
  const who = ghostWho(ev.g, ev.src);
  const res = ghostRes(ev.res);
  switch (ev.w) {
    case 'rise':
      addLog(`<span class="log-check-fail">☠ ${escHtml(who)} gets up where they fell.</span>`);
      if (ghostInSight(ev.q, ev.r)) showToast(`${who} gets up off the ground. Keep away from it.`, 'ghost');
      break;
    case 'latch':
      m.grabAt = performance.now();
      addLog(`<span class="log-check-fail">☠ ${escHtml(who)} has hold of ${escHtml(whoP)}.</span>`);
      if (mine) {
        showBanner('HAUNTED', `${who} rides you — walk into a Settlement to shake it off`);
        showToast(`${who} has hold of you. It will feed on your pack and take a third of all you gain.`, 'ghost');
      }
      break;
    case 'letgo':
      m.grabAt = 0;
      addLog(`<span class="log-col">☠ ${escHtml(who)} lets go of ${escHtml(whoP)} at the edge of town.</span>`);
      if (mine) showToast(`${who} stops at the edge of town. It cannot follow you in.`, 'ghost');
      break;
    case 'drain':
      addLog(`<span class="log-check-fail">☠ ${escHtml(who)} takes 1 ${escHtml(res)} from ${escHtml(whoP)}.</span>`);
      if (mine) {
        const me = players[myId], i = (ev.res | 0) - 1;
        if (me && i >= 0 && i < 5 && me.inv[i] > 0) { me.inv[i]--; updateSidebar(); }
        showToast(`${who} takes 1 ${res} from your pack.`, 'ghost');
      }
      break;
    case 'tithe':
      addLog(`<span class="log-check-fail">☠ ${escHtml(who)} takes its share: ${ev.n | 0} ${escHtml(res)}.</span>`);
      if (mine) showToast(`${who} takes its share: ${ev.n | 0} ${res}.`, 'ghost');
      break;
    case 'fade': {
      const n = ev.n | 0;
      addLog(`<span class="log-col">☠ ${escHtml(who)} thins to nothing${n ? `, and drops the ${n} it stole` : ''}.</span>`);
      if (mine) showToast(n ? `${who} lets go and thins to nothing. What it took lies at your feet.` : `${who} lets go and thins to nothing.`, 'ghost');
      else if (n && ghostInSight(ev.q, ev.r)) showToast(`${who} thins to nothing. What it stole lies where it was.`, 'ghost');
      break;
    }
  }
}
