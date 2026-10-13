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
// Which way the convoy points: -1 west as painted, +1 east (mirrored). Screen
// x is 1.5*q on this flat-top grid, so the sign of the q still to travel is
// the heading; a leg straight north or south keeps the last one.
let caravanFacing = -1;

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
  if (Math.abs(tq - caravanRenderPos.q) > 0.25) caravanFacing = Math.sign(tq - caravanRenderPos.q);
  caravanRenderPos.q += (tq - caravanRenderPos.q) * LERP_RATE;
  caravanRenderPos.r += (tr - caravanRenderPos.r) * LERP_RATE;
}

// The convoy sticker (caravanSprite(), engine.js) -- the user's APC leading a
// cargo truck, cut out like the shelters and drawn over the terrain the same
// way. A board without the caravan sheet gets the plain canvas badge below,
// styled like drawCharIcon()'s no-portrait fallback.
const CARAVAN_SPRITE_HEX = 1.5;   // sticker edge in HEX_SZ, = CARAVAN in hex_sheets.py
function renderCaravan(cam) {
  lerpCaravanPosition();
  if (!worldState.caravan?.active) return;
  const { ox, oy, meRp } = cam;
  const { vq, vr } = closestWrapCoords(caravanRenderPos, meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  const cx = pp.x + ox, cy = pp.y + oy;
  if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) return;
  if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) return;

  const sp = caravanSprite();
  if (sp) {
    const sz = HEX_SZ * CARAVAN_SPRITE_HEX;
    ctx.save();
    ctx.translate(cx, cy);
    if (caravanFacing > 0) ctx.scale(-1, 1);
    drawAtlasCell(ctx, sp.page, sp.sx, sp.sy, sp.s, sp.s, -sz / 2, -sz / 2, sz, sz);
    ctx.restore();
    return;
  }

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
// Which way it last went, in screen space, and how lately (k): the skull keeps
// facing its last heading and its gas keeps streaming off the back, easing to
// a lean while it hangs between world ticks -- a step lerps in a quarter of a
// second and then nothing moves for 15. k 1 just moved .. 0.5 hanging; 0
// until it has been seen moving at all, which leaves the gas going straight up.
const doomHeading = { x: 0, y: 0, k: 0 };

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
  const dq = (tq - doomRenderPos.q) * LERP_RATE, dr = (tr - doomRenderPos.r) * LERP_RATE;
  const v = hexToPixel(dq, dr, 1), vl = Math.hypot(v.x, v.y);
  if (vl > 0.004) { doomHeading.x = v.x / vl; doomHeading.y = v.y / vl; doomHeading.k = 1; }
  else if (doomHeading.k > 0.5) doomHeading.k = Math.max(0.5, doomHeading.k * 0.985);
  doomRenderPos.q += dq;
  doomRenderPos.r += dr;
}

// The Creeping Doom is a skull of sick green gas floating over its hex. Drawn
// with math, no sprite: two density fields recomputed every frame --
//   smoke  low-res and wide: the cloud it sits in and the tail it streams
//   skull  high-res and tight: the skull, shaped out of that gas
// and awareness is how well the gas holds the face, on the same tiers its
// behaviour changes at (world-system.hpp): under 51 a smear that nearly finds
// a face every few seconds and loses it; 51 a scowl that turns where it goes;
// 76 embers for eyes (it is setting fires by then, wIgnite); 100 the jaw going
// and the tail a hex long. The K10 draws the same monster in its own hand
// (fxGasSkull, ui-fx.hpp). Units are R, the cranium radius.
const DoomGas = (() => {
  const PERM = new Uint8Array(512);
  {
    const p = Array.from({ length: 256 }, (_, i) => i);
    let s = 0x2545F491;
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
  const fbm3 = (x, y) => vn(x, y) * 0.5 + vn(x * 2.03 + 17.1, y * 2.03 - 9.7) * 0.3 + vn(x * 4.11 - 31.7, y * 4.11 + 5.3) * 0.2;
  const clamp01 = v => v < 0 ? 0 : v > 1 ? 1 : v;
  const ss = (e0, e1, x) => { const t = clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); };
  const smin = (a, b, k) => { const h = Math.max(k - Math.abs(a - b), 0) / k; return Math.min(a, b) - h * h * k * 0.25; };
  function sdBox(x, y, hw, hh, rr) {
    const dx = Math.abs(x) - hw + rr, dy = Math.abs(y) - hh + rr;
    return Math.hypot(Math.max(dx, 0), Math.max(dy, 0)) + Math.min(Math.max(dx, dy), 0) - rr;
  }
  function sdEllRot(dx, dy, rx, ry, a) {
    const ca = Math.cos(a), sa = Math.sin(a);
    const u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
    return (Math.hypot(u / rx, v / ry) - 1) * Math.min(rx, ry);
  }

  // The skull, y down, origin mid-face. Returns the bone's signed distance and
  // leaves the cavities' (sockets, nose, mouth, teeth) in CAV. `turn` -1..1
  // slides the face toward where it is looking; `jaw` 0..~0.35 drops the jaw.
  let CAV = 0;
  function skull(x, y, turn, jaw, teeth) {
    const fx = x - turn * 0.13;
    const cran = Math.hypot((x + turn * 0.06) * 1.06, y + 0.20) - 0.68;
    const face = sdBox(fx, y - 0.30, 0.48, 0.22, 0.16);
    const cheek = Math.min(Math.hypot(fx + 0.46, y - 0.20), Math.hypot(fx - 0.46, y - 0.20)) - 0.13;
    const jy = 0.70 + jaw * 0.34;
    const taper = 1 - 0.30 * clamp01((y - jy + 0.05) / 0.2);
    const jawd = sdBox(fx / taper, y - jy, 0.36, 0.15, 0.12) * taper;
    const bone = smin(smin(smin(cran, face, 0.22), cheek, 0.12), jawd, 0.08);

    // Sockets tilted inner-end-down: a scowl, not a stare.
    const sL = sdEllRot(fx + 0.29, y - 0.10, 0.22, 0.17, 0.35);
    const sR = sdEllRot(fx - 0.29, y - 0.10, 0.22, 0.17, -0.35);
    const ny = y - 0.25;
    const nose = Math.max(Math.abs(fx) - (0.012 + 0.09 * ny / 0.17), -ny, ny - 0.17);
    const my = 0.535 + jaw * 0.17;
    const mouth = sdBox(fx, y - my, 0.30 - jaw * 0.05, 0.016 + jaw * 0.17, 0.015);
    let cav = Math.min(sL, sR, nose, mouth);
    if (teeth && Math.abs(fx) < 0.28) {
      const up = y > 0.45 && y < 0.52, lo = y > 0.55 + jaw * 0.34 && y < 0.61 + jaw * 0.34;
      if (up || lo) {
        const f = ((fx + 0.3) / 0.1) % 1;
        cav = Math.min(cav, Math.abs(f - 0.5) - 0.38);  // a thin slit every tooth
      }
    }
    CAV = cav;
    return bone;
  }

  // How well the gas holds the skull. Dormant it is a smear that nearly forms
  // a face every few seconds and loses it again.
  function coherence(aw, t) {
    if (aw >= 100) return 1;
    if (aw >= 76) return 0.86;
    if (aw >= 51) return 0.70 + 0.04 * Math.sin(t * 1.3);
    const ph = (t / 5.5) % 1;
    const pk = Math.pow(Math.max(0, Math.sin(ph * Math.PI)), 6);
    return 0.12 + (0.40 + (aw / 50) * 0.1) * pk;
  }

  function field(N) {
    const cv = document.createElement('canvas');
    cv.width = cv.height = N;
    const g = cv.getContext('2d');
    return { cv, g, img: g.createImageData(N, N), N };
  }
  let SK = null, SM = null;
  const sized = (f, N) => (f && f.N === N) ? f : field(N);

  function smokeField(F, ext, t, bx, by, L, cloudR, c, flow) {
    const d = F.img.data, N = F.N;
    const trailK = 0.55 + 0.4 * c, cloudK = 0.35 + 0.6 * (1 - c);
    let k = 0;
    for (let j = 0; j < N; j++) {
      const y = (j + 0.5) / N * 2 * ext - ext;
      for (let i = 0; i < N; i++, k += 4) {
        const x = (i + 0.5) / N * 2 * ext - ext;
        const r2 = x * x + y * y;
        const s = x * bx + y * by, dp = -x * by + y * bx;
        let env;
        if (s > 0) { const w = 0.8 + 0.22 * s; env = Math.exp(-(dp * dp) / (w * w) - s / L); }
        else env = Math.exp(-r2 / 0.72);
        env = Math.max(env * trailK, Math.exp(-r2 / (cloudR * cloudR)) * cloudK);
        env *= ss(ext, ext * 0.7, Math.sqrt(r2));
        if (env < 0.01) { d[k + 3] = 0; continue; }
        const ax = x - bx * t * flow, ay = y - by * t * flow;
        const n = fbm3(ax * 1.45 + 3.7, ay * 1.45 - 1.9);
        // Ridged noise for the strands: gas that tears, not fog that fades.
        const rg = 1 - Math.abs(2 * fbm2(ax * 2.3 - 8.1, ay * 2.3 + 4.4) - 1);
        const body = env * ss(0.30, 0.70, n + env * 0.35);
        const strand = env * ss(0.72, 0.92, rg) * 0.9;
        const dens = Math.min(1, body * 0.75 + strand);
        const lit = ss(0.10, 0.80, strand * 1.2 + body * 0.3);
        d[k]     = 30 + 104 * lit;
        d[k + 1] = 40 + 128 * lit;
        d[k + 2] = 10 + 40 * lit;
        d[k + 3] = dens * 0.80 * 255;
      }
    }
    F.g.putImageData(F.img, 0, 0);
  }

  function skullField(F, ext, t, c, turn, tilt, jaw, lumK, bx, by) {
    const d = F.img.data, N = F.N;
    const ppr = N / (2 * ext);
    const teeth = ppr >= 20;
    const A = 0.05 + (1 - c) * 0.55;
    const softB = Math.max(0.04 + (1 - c) * 0.2, 0.9 / ppr), softC = Math.max(0.025, 0.8 / ppr);
    const thr = (1 - c) * 0.62;
    const cavK = clamp01((c - 0.15) / 0.5) * 0.92;
    const boneK = 0.35 + 0.6 * c;
    const ct = Math.cos(tilt), st = Math.sin(tilt);
    let k = 0;
    for (let j = 0; j < N; j++) {
      const y0 = (j + 0.5) / N * 2 * ext - ext;
      for (let i = 0; i < N; i++, k += 4) {
        const x0 = (i + 0.5) / N * 2 * ext - ext;
        const r = Math.hypot(x0, y0);
        if (r > ext * 0.98) { d[k + 3] = 0; continue; }
        const x = x0 * ct + y0 * st, y = -x0 * st + y0 * ct;
        const wn = fbm2(x * 1.7 + 11.3 + t * 0.21, y * 1.7 - t * 0.37);
        let wx = A * (wn - 0.5) * 2;
        let wy = A * (fbm2(x * 1.7 - 7.9 - t * 0.17, y * 1.7 + 3.1 - t * 0.29) - 0.5) * 2;
        // The side facing the tail gets pulled toward it: the skull sheds
        // gas off its back edge instead of sitting in it like a sticker.
        const lee = Math.max(0, x * bx + y * by);
        wx -= bx * lee * wn * 0.45; wy -= by * lee * wn * 0.45;
        const sdb = skull(x + wx, y + wy, turn, jaw, teeth);
        const win = ss(ext * 0.98, ext * 0.8, r);
        let bone = ss(softB, -softB, sdb) * win;
        if (bone > 0.002) {
          // Ragged at the rim, solid in the middle.
          const rim = ss(-0.22, 0.0, sdb) * 0.22;
          bone *= ss(thr + rim - 0.12, thr + rim + 0.12, fbm3(x * 2.4 + t * 0.13, y * 2.4 - t * 0.31));
        }
        // A dark lip round the bone so it holds against pale or green ground.
        const ah = ss(0.13, 0.01, sdb) * (0.25 + 0.4 * c) * win;
        const ac = ss(softC, -softC, CAV) * cavK * win;
        const swirl = fbm2(x * 2.6 + t * 0.35, y * 2.6 - t * 0.5);
        const ab = bone * boneK * (0.62 + 0.38 * ss(0.32, 0.68, swirl));
        const a1 = ab + ah * (1 - ab);
        const a = ac + a1 * (1 - ac);
        if (a < 0.003) { d[k + 3] = 0; continue; }
        let lum = 0.40 + (fbm2(x * 3.1 - t * 0.2, y * 3.1 + t * 0.45) - 0.5) * 1.8 + (swirl - 0.5) * 0.6 + Math.min(0.3, Math.max(0, -sdb) * 0.8);
        lum = clamp01(lum * lumK);
        const wB = ab * (1 - ac) / a, wH = ah * (1 - ab) * (1 - ac) / a, wC = ac / a;
        d[k]     = (96 + 110 * lum) * wB + 20 * wH + 10 * wC;
        d[k + 1] = (122 + 106 * lum) * wB + 28 * wH + 14 * wC;
        d[k + 2] = (36 + 76 * lum) * wB + 8 * wH + 5 * wC;
        d[k + 3] = a * 255;
      }
    }
    F.g.putImageData(F.img, 0, 0);
  }

  // cx,cy: the hex centre on screen. R: cranium radius in CSS px. vx,vy: the
  // screen-space heading, length 0 (still) .. 1 (full travel). dpr: device px
  // per CSS px, so the skull field resolves at the screen's real density.
  function draw(ctx, cx, cy, R, aw, vx, vy, now, dpr) {
    const t = now / 1000;
    const c = coherence(aw, t);
    const sp = Math.hypot(vx, vy), m = Math.min(1, sp);
    const hx = sp > 1e-3 ? vx / sp : 0, hy = sp > 1e-3 ? vy / sp : 0;
    // The tail streams behind it and rises: still, the gas just goes up.
    let bx = -hx * m, by = -hy * m - 0.55;
    const bl = Math.hypot(bx, by); bx /= bl; by /= bl;
    const turn = hx * m * 0.9, tilt = hx * m * 0.14;
    const jaw = aw >= 100 ? 0.26 + 0.08 * Math.abs(Math.sin(t * 7.5))
              : aw >= 51 ? 0.03 + 0.05 * (0.5 + 0.5 * Math.sin(t * 1.6)) : 0;
    const L = aw >= 100 ? 4.0 : aw >= 76 ? 3.2 : aw >= 51 ? 2.4 : 1.4;
    const lumK = 0.9 + 0.1 * Math.sin(t * (1.2 + 2.5 * aw / 100));
    const bob = Math.sin(t * 1.4) * 0.10;
    const sy = cy - R * (0.30 + bob);

    // Ground shadow, smaller and fainter as it bobs up.
    ctx.save();
    ctx.translate(cx, cy + R * 0.62);
    ctx.scale(1, 0.27);
    const shR = R * 0.85 * (1 - bob * 0.8);
    const sg = ctx.createRadialGradient(0, 0, 0, 0, 0, shR);
    sg.addColorStop(0, 'rgba(8,12,4,0.32)');
    sg.addColorStop(1, 'rgba(8,12,4,0)');
    ctx.fillStyle = sg;
    ctx.fillRect(-shR, -shR, shR * 2, shR * 2);
    ctx.restore();

    // A faint sick light on whatever is under it.
    const gR = R * 2.2;
    const gg = ctx.createRadialGradient(cx, sy, 0, cx, sy, gR);
    gg.addColorStop(0, `rgba(150,190,50,${0.10 + 0.12 * aw / 100})`);
    gg.addColorStop(1, 'rgba(150,190,50,0)');
    ctx.fillStyle = gg;
    ctx.fillRect(cx - gR, sy - gR, gR * 2, gR * 2);

    const prevSmooth = ctx.imageSmoothingEnabled;
    ctx.imageSmoothingEnabled = true;
    const extS = 5.5;
    SM = sized(SM, Math.max(20, Math.min(72, Math.ceil(2 * extS * R / 2.2))));
    smokeField(SM, extS, t, bx, by, L, 1.3 + (1 - c) * 1.4, c, 0.5 + 0.9 * aw / 100);
    ctx.drawImage(SM.cv, cx - extS * R, sy - extS * R, 2 * extS * R, 2 * extS * R);

    const extK = 1.5;
    SK = sized(SK, Math.max(24, Math.min(96, Math.ceil(2 * extK * R * Math.min(dpr, 2)))));
    // The tail direction in the skull's own (un-tilted) frame.
    const ct0 = Math.cos(tilt), st0 = Math.sin(tilt);
    skullField(SK, extK, t, c, turn, tilt, jaw, lumK, bx * ct0 + by * st0, -bx * st0 + by * ct0);
    ctx.drawImage(SK.cv, cx - extK * R, sy - extK * R, 2 * extK * R, 2 * extK * R);
    ctx.imageSmoothingEnabled = prevSmooth;

    // Embers in the sockets once it is unmaking things (76+), hot at 100.
    if (aw >= 76) {
      const kk = aw >= 100 ? 1 : 0.6;
      const ct = Math.cos(tilt), st = Math.sin(tilt);
      ctx.save();
      ctx.globalCompositeOperation = 'lighter';
      for (let e = 0; e < 2; e++) {
        const lx = (e ? 0.29 : -0.29) + turn * 0.13, ly = 0.11;
        const ex = cx + (lx * ct - ly * st) * R, ey = sy + (lx * st + ly * ct) * R;
        const fl = 0.7 + 0.3 * vn(t * 7 + e * 19.3, 4.2);
        const er = R * (0.24 + 0.06 * kk);
        const eg = ctx.createRadialGradient(ex, ey, 0, ex, ey, er);
        eg.addColorStop(0, `rgba(255,190,90,${0.95 * kk * fl})`);
        eg.addColorStop(0.35, `rgba(255,96,24,${0.45 * kk * fl})`);
        eg.addColorStop(1, 'rgba(255,60,0,0)');
        ctx.fillStyle = eg;
        ctx.fillRect(ex - er, ey - er, er * 2, er * 2);
        ctx.beginPath();
        ctx.arc(ex, ey, Math.max(0.8, R * 0.055 * (0.8 + 0.4 * kk)), 0, Math.PI * 2);
        ctx.fillStyle = `rgba(255,240,190,${fl})`;
        ctx.fill();
      }
      ctx.restore();
    }
  }

  return { draw };
})();

// Cranium radius in HEX_SZ. The whole skull (about 2R tall) is smaller than a
// survivor's portrait circle (drawCharIcon(): r = max(10, 0.28*HEX_SZ)), and
// floored at 7 px against the pawn's 10, so zoomed right out it still reads
// as a face. Only its gas spreads wider.
const DOOM_SKULL_R = 0.2;

// Drawn at every awareness including 0, matching the K10 map screen, which
// plots it always. An earlier `awareness <= 0` early-out meant a dormant Doom
// was plotted on the device but invisible in the browser -- and since
// awareness sits at 0 for most of a run, that was the normal case, not an edge
// case. There is no scent ring any more: how much it cares is in the gas.
function renderDoom(cam) {
  lerpDoomPosition();
  const d = worldState.doom;
  if (!d) return;
  const { ox, oy, meRp } = cam;
  const { vq, vr } = closestWrapCoords(doomRenderPos, meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  const cx = pp.x + ox, cy = pp.y + oy;
  const R = Math.max(7, HEX_SZ * DOOM_SKULL_R);
  // The tail can reach a hex and a half off the skull.
  const cullR = R * 6;
  if (cx < -cullR || cx > cssWidth  + cullR) return;
  if (cy < -cullR || cy > cssHeight + cullR) return;
  DoomGas.draw(ctx, cx, cy, R, d.awareness, doomHeading.x * doomHeading.k, doomHeading.y * doomHeading.k,
               performance.now(), window.devicePixelRatio || 1);
}
