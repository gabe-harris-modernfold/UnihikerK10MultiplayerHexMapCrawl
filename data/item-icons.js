// ── item-icons.js ────────────────────────────────────────────────
// Every item badge in the game, drawn with math. There is no icon_<id>.png:
// each icon below is a handful of signed-distance shapes that get lit,
// outlined and dirtied, then rasterised on demand at exactly the size the slot
// shows it and handed to an <img> as a data: URL.
//
// Why not bitmaps. 65 items is 65 PNGs (130 with the illustrations), and every
// file under data/img/ takes a slot in the K10's PSRAM image cache
// (MAX_IMG_CACHE, docs/dev-loop.md) and a TCP connection per fetch. This is
// one script in the bundle, and the icons are sharp at every size the UI uses
// (14 px haul chips through the 32 px item sheet) because each size is drawn,
// never resampled.
//
// The look is data/img/items/ICON_PROMPTS.md's STYLE block, which is the
// brief the art was going to be generated from anyway:
//   - pixel art: hard pixel edges, no anti-aliasing; one art pixel is one CSS
//     pixel, so a 26 px slot is a 26 x 26 drawing, not a shrunk 32
//   - one object, centred, a few pixels of padding, transparent background
//   - flat cel shading: one shadow tone and one highlight tone, lit from the
//     top left; a glint on anything wet or glassy
//   - desaturated rust / bone / olive / steel (style.css's "ash grey, rust
//     orange, dried-blood brown, sickly yellow-green, cold steel blue"), with
//     the UI's own ambers for anything amber, so it sits on the near-black UI
//   - weathered: dents, rust, tape, grime -- and all of it from the house
//     noise (ui-fx.hpp's lowbias32 + value noise), so nothing is a pattern.
//     Same rule as the LCD: everything is calculated, nothing may look it.
//     Edges wander, dots jitter, stains are blotches, never circles.
//   - a strong silhouette: every solid gets a 1 px outline in its own darkest
//     tone, and a part laid over another gets the same line where it overlaps
//
// Drawing model, per pixel, in a 32 x 32 design grid (y down) whatever the
// output size:
//   solid  -- the topmost solid whose distance is < 0 owns the pixel. It is
//             lit by its shade model (bevel, round, cyl, sphere, glow, flat),
//             then grime / rust / speckle push it down the ramp.
//   decal  -- paint on one solid (a label, a stain, a seam). Inherits the
//             solid's lighting unless it brings its own, so a label on a can
//             still wraps round the can.
//   fx     -- unlit marks over everything: sparks, smoke, motion lines.
//   glow   -- a solid may scatter a sparse, noise-broken aura into the empty
//             pixels around it (the radioactive things).
//
// Adding an icon: write I[id] = (k) => { ... } below, next to its category;
// ids with no drawing fall back to their category glyph, then the skull.
// Preview every icon at every slot size without a browser:
//   node scripts/item_icon_sheet.js      (writes a PNG contact sheet)
//
// API:  ItemIcons.url(id, px)  -> data: URL for a px-sized <img>
//       ItemIcons.rgba(id, n)  -> Uint8ClampedArray, n x n x 4 (no DOM)
(function (root) {
  'use strict';

  // ── 1. Hash and noise ───────────────────────────────────────────
  // lowbias32 and smoothstepped value noise, as ui-fx.hpp / observer-fx.js
  // have them: the rust here comes from the same place as the LCD's static.
  function hash(x) {
    x >>>= 0;
    x ^= x >>> 16; x = Math.imul(x, 0x7feb352d); x ^= x >>> 15; x = Math.imul(x, 0x846ca68b); x ^= x >>> 16;
    return x >>> 0;
  }
  const hash2 = (a, b) => hash((Math.imul(a >>> 0, 0x9E3779B1) + hash(b)) >>> 0);
  const u01 = (h) => (h >>> 8) / 16777216;
  const smooth = (t) => t * t * (3 - 2 * t);
  const clamp = (v, lo, hi) => (v < lo ? lo : (v > hi ? hi : v));
  const PI = Math.PI;
  // noise2h takes hash(seed) ready-made; it is hash2's inner half, and the
  // one call per sample that never changes. Same values as noise2.
  function noise2h(x, y, hs) {
    const fx = Math.floor(x), fy = Math.floor(y);
    const tx = smooth(x - fx), ty = smooth(y - fy);
    const h0 = hash(hash(Math.imul(fy >>> 0, 0x9E3779B1) + hs)), h1 = hash(hash(Math.imul((fy + 1) >>> 0, 0x9E3779B1) + hs));
    const ka = Math.imul(fx >>> 0, 0x9E3779B1), kb = Math.imul((fx + 1) >>> 0, 0x9E3779B1);
    const a = u01(hash(ka + h0)), b = u01(hash(kb + h0)), c = u01(hash(ka + h1)), d = u01(hash(kb + h1));
    const ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
  }
  const noise2 = (x, y, seed) => noise2h(x, y, hash(seed));
  // Two octaves: blotches with ragged edges.
  const fbm = (x, y, s) => noise2(x, y, s) * 0.65 + noise2(x * 2.3 + 17.1, y * 2.3 - 5.3, s ^ 0x5bd1e995) * 0.35;

  // ── 2. The palette ──────────────────────────────────────────────
  // Every material is a ramp: line (outline, crevices), shadow, base,
  // highlight, glint. Tones index into it.
  const LINE = 0, SHD = 1, BASE = 2, HI = 3, GLINT = 4;
  const RAMPS = {
    steel:  ['#141a1e', '#34424a', '#5a6a70', '#8e9c9c', '#d2d8cc'],
    iron:   ['#0e1012', '#24292c', '#3c4346', '#5e6868', '#98a29e'],
    foil:   ['#1a1c1e', '#4e5458', '#7e868a', '#b4bcbc', '#eef2ea'],
    rust:   ['#1e0c04', '#4e1e0c', '#7e3614', '#b0561e', '#d8843a'],
    bone:   ['#241a10', '#6a5a40', '#a4926a', '#d2c294', '#f0e6c4'],
    white:  ['#2a2620', '#8a8478', '#c8c2b0', '#eeeade', '#ffffff'],
    tan:    ['#2a1c0e', '#6e5434', '#9c7c50', '#c4a472', '#e2ca98'],
    leather:['#1a0e06', '#3c2210', '#5e3818', '#865226', '#a87038'],
    wood:   ['#1c1008', '#422a14', '#6a4624', '#926634', '#b88a4e'],
    cork:   ['#241808', '#5a4020', '#86643a', '#b08a54', '#d0ae78'],
    ration: ['#1e1208', '#4a3018', '#74502a', '#9c7440', '#c09a60'],
    olive:  ['#14160a', '#33381a', '#545c2c', '#7a8442', '#a0aa62'],
    canvas: ['#1e1a0e', '#463e24', '#6c6040', '#968a5e', '#bab082'],
    blood:  ['#1a0404', '#3e0c06', '#62160c', '#8a2412', '#a8402a'],
    red:    ['#200604', '#5a1008', '#962014', '#c83c22', '#f07a4a'],
    amber:  ['#2a1004', '#6a3008', '#c07818', '#e8a828', '#fff0c8'],
    mustard:['#2a2006', '#6e5410', '#a8841e', '#d4b03a', '#eed878'],
    rad:    ['#0c1604', '#2a4a0c', '#5a8a18', '#98c832', '#dcf87c'],
    toxic:  ['#06160a', '#145a1e', '#2ea030', '#6ee04a', '#c8ff9a'],
    bile:   ['#181806', '#4a4a10', '#7a7a1c', '#a8a434', '#d6d27a'],
    glass:  ['#0c1210', '#1e2a26', '#34443e', '#5c7068', '#b8ccc0'],
    brownglass: ['#140a04', '#3a1c08', '#5e3010', '#8a4e1e', '#e0b070'],
    water:  ['#0a1218', '#1c3444', '#34586c', '#5e8698', '#c0dce4'],
    blue:   ['#061228', '#143e70', '#2e74b8', '#6cb0e8', '#d8f2ff'],
    orange: ['#2a0c02', '#7a2a06', '#d0601a', '#ff9a3a', '#ffe0a0'],
    fire:   ['#3a0c02', '#a82a08', '#e87018', '#ffc048', '#fff4d0'],
    copper: ['#200c04', '#5a2a10', '#92501e', '#c47a34', '#f0b070'],
    purple: ['#140814', '#34183a', '#56285a', '#7e4478', '#a86a9a'],
    flesh:  ['#200a0c', '#52202a', '#7e3a40', '#a8605e', '#d49a8a'],
    gum:    ['#2a0a10', '#6a2432', '#9e4450', '#c87070', '#e8a0a0'],
    fur:    ['#140e08', '#342618', '#56422a', '#7c6442', '#9c8660'],
    ash:    ['#121210', '#2e2c26', '#4e4a3e', '#767060', '#9c9686'],
    black:  ['#060608', '#121216', '#202026', '#3a3a44', '#6a6a78'],
    tape:   ['#161816', '#3c403c', '#626862', '#8a908a', '#aab0a8'],
    ceramic:['#1a1a18', '#4a4a44', '#7a786e', '#a6a498', '#cac8bc'],
    note:   ['#2e2606', '#8a7418', '#c8aa30', '#e6cc54', '#f6e690'],
    pcb:    ['#081208', '#1a3418', '#2e5428', '#4e7a3e', '#7aa45e'],
    jelly:  ['#2a1a16', '#7a5a50', '#b08e7c', '#d8bca6', '#f4e4d4'],
    teal:   ['#081614', '#1a3c38', '#2e605a', '#4e8a80', '#8ec0b4'],
    lead:   ['#121416', '#30363a', '#4c5458', '#6e787a', '#9aa4a4'],
    clay:   ['#1e0e08', '#4e2614', '#7a4024', '#a05c34', '#c4804e'],
    smoke:  ['#1a1816', '#2a2622', '#4a4540', '#6a645c', '#8a847a'],
  };
  // ImageData is little-endian RGBA: pack each rung as ABGR once.
  const PACK = {};
  for (const m in RAMPS) {
    PACK[m] = RAMPS[m].map((h) => {
      const v = parseInt(h.slice(1), 16);
      return ((255 << 24) | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF)) >>> 0;
    });
  }
  const colour = (m, t) => { const r = PACK[m] || PACK.steel; return r[t < r.length ? t : r.length - 1]; };

  // ── 3. Distance functions ───────────────────────────────────────
  // Design units: a 32 x 32 grid, y down, whatever size gets drawn.
  // Negative inside. Angles in degrees, clockwise on screen.
  //
  // Every field carries .amp: how far noise may have pushed it off a true
  // distance (0 for the primitives, summed through rough()). The rasterizer
  // uses it to skip a part wherever it provably cannot reach. A hand-written
  // lambda has no .amp, counts as Infinity, and is simply never skipped.
  const len = (x, y) => Math.sqrt(x * x + y * y);     // Math.hypot is ~10x slower
  const tag = (fn, amp) => { fn.amp = amp; return fn; };
  function ampOf(fs) {
    let a = 0;
    for (let i = 0; i < fs.length; i++) { const v = fs[i].amp; a = Math.max(a, v === undefined ? Infinity : v); }
    return a;
  }
  const circle = (cx, cy, r) => tag((x, y) => len(x - cx, y - cy) - r, 0);
  function ellipse(cx, cy, rx, ry) {
    return tag((x, y) => {                   // IQ's cheap bound; plenty at icon size
      const dx = (x - cx) / rx, dy = (y - cy) / ry;
      const k0 = len(dx, dy), k1 = len(dx / rx, dy / ry);
      return k1 < 1e-9 ? -Math.min(rx, ry) : k0 * (k0 - 1) / k1;
    }, 0);
  }
  const box = (cx, cy, hw, hh, r = 0) => tag((x, y) => {
    const qx = Math.abs(x - cx) - hw + r, qy = Math.abs(y - cy) - hh + r;
    return len(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - r;
  }, 0);
  // A capsule from a to b, radius r0 at a easing to r1 at b.
  function seg(ax, ay, bx, by, r0, r1 = r0) {
    const dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy || 1e-9;
    return tag((x, y) => {
      const h = clamp(((x - ax) * dx + (y - ay) * dy) / l2, 0, 1);
      return len(x - ax - dx * h, y - ay - dy * h) - (r0 + (r1 - r0) * h);
    }, 0);
  }
  // A closed polygon from a flat [x0, y0, x1, y1, ...] list.
  function poly(p) {
    const n = p.length >> 1;
    return tag((x, y) => {
      let d = Infinity, s = 1;
      for (let i = 0, j = n - 1; i < n; j = i++) {
        const xi = p[2 * i], yi = p[2 * i + 1], ex = p[2 * j] - xi, ey = p[2 * j + 1] - yi;
        const wx = x - xi, wy = y - yi;
        const h = clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1);
        const bx = wx - ex * h, by = wy - ey * h, q = bx * bx + by * by;
        if (q < d) d = q;
        const c1 = y >= yi, c2 = y < p[2 * j + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
      }
      return s * Math.sqrt(d);
    }, 0);
  }
  // A brush stroke along a polyline, tapering r0 -> r1 over its length.
  function stroke(p, r0 = 0, r1 = r0) {
    const n = p.length >> 1, cum = [0];
    for (let i = 1; i < n; i++) cum.push(cum[i - 1] + len(p[2 * i] - p[2 * i - 2], p[2 * i + 1] - p[2 * i - 1]));
    const L = cum[n - 1] || 1, parts = [];
    for (let i = 0; i < n - 1; i++) {
      parts.push(seg(p[2 * i], p[2 * i + 1], p[2 * i + 2], p[2 * i + 3],
        r0 + (r1 - r0) * cum[i] / L, r0 + (r1 - r0) * cum[i + 1] / L));
    }
    return union(...parts);
  }
  // Quadratic curve a -> b pulled toward c, as a tapering stroke.
  function curve(ax, ay, cx, cy, bx, by, r0 = 0, r1 = r0) {
    const p = [];
    for (let i = 0; i <= 10; i++) {
      const t = i / 10, s = 1 - t;
      p.push(s * s * ax + 2 * s * t * cx + t * t * bx, s * s * ay + 2 * s * t * cy + t * t * by);
    }
    return stroke(p, r0, r1);
  }
  function arc(cx, cy, r, a0, a1, w = 0) {
    const p = [], n = Math.max(3, Math.ceil(Math.abs(a1 - a0) / 15));
    for (let i = 0; i <= n; i++) {
      const a = (a0 + (a1 - a0) * i / n) * PI / 180;
      p.push(cx + Math.cos(a) * r, cy + Math.sin(a) * r);
    }
    return stroke(p, w);
  }
  // Distance to the nearest of a set of points: dots, rivets, pinholes.
  const dots = (p, r = 0) => tag((x, y) => {
    let d = Infinity;
    for (let i = 0; i < p.length; i += 2) { const q = len(x - p[i], y - p[i + 1]); if (q < d) d = q; }
    return d - r;
  }, 0);
  function union(...fs) {
    const n = fs.length;
    return tag((x, y) => { let d = Infinity; for (let i = 0; i < n; i++) { const v = fs[i](x, y); if (v < d) d = v; } return d; }, ampOf(fs));
  }
  function minus(a, ...bs) {
    const n = bs.length;
    return tag((x, y) => { let d = a(x, y); for (let i = 0; i < n; i++) { const v = -bs[i](x, y); if (v > d) d = v; } return d; },
      ampOf([a, ...bs]));
  }
  const inter = (a, b) => tag((x, y) => Math.max(a(x, y), b(x, y)), ampOf([a, b]));
  function rotate(f, deg, cx = 16, cy = 16) {
    const c = Math.cos(-deg * PI / 180), s = Math.sin(-deg * PI / 180);
    return tag((x, y) => { const dx = x - cx, dy = y - cy; return f(cx + dx * c - dy * s, cy + dx * s + dy * c); }, ampOf([f]));
  }
  const shift = (f, dx, dy) => tag((x, y) => f(x - dx, y - dy), ampOf([f]));
  const grow = (f, r) => tag((x, y) => f(x, y) - r, ampOf([f]));
  const shell = (f, w) => tag((x, y) => Math.abs(f(x, y)) - w, ampOf([f]));
  // Push an edge in and out by noise: dents, chips, frayed cloth.
  function rough(f, amp, freq = 0.55, seed = 1) {
    const hs = hash(seed);
    return tag((x, y) => f(x, y) + (noise2h(x * freq, y * freq, hs) - 0.5) * 2 * amp, ampOf([f]) + Math.abs(amp));
  }
  // Radiation trefoil: two blades up, one down, a hub. (Not a distance.)
  function trefoil(cx, cy, r) {
    return (x, y) => {
      const dx = x - cx, dy = y - cy, q = len(dx, dy);
      if (q < r * 0.22) return -1;
      if (q > r || q < r * 0.38) return 1;
      const a = ((Math.atan2(dy, dx) * 180 / PI) + 360) % 360;
      for (const b of [90, 210, 330]) { const g = Math.abs(((a - b + 540) % 360) - 180); if (g < 31) return -1; }
      return 1;
    };
  }
  // Stripes of period p across the direction (dx, dy): negative on half.
  const stripes = (p, dx = 1, dy = 1) => (x, y) => ((((x * dx + y * dy) % p) + p) % p) - p / 2;
  // A teardrop of radius r, its tip pointing along deg (-90 = up).
  function drop(cx, cy, r, deg = -90) {
    const a = deg * PI / 180, tx = cx + Math.cos(a) * r * 2.3, ty = cy + Math.sin(a) * r * 2.3;
    const px = -Math.sin(a) * r * 0.92, py = Math.cos(a) * r * 0.92;
    return union(circle(cx, cy, r), poly([cx + px, cy + py, tx, ty, cx - px, cy - py]));
  }
  // Deterministic jitter for hand-placed points: nothing lands on a grid.
  function jit(p, amt, seed) {
    return p.map((v, i) => v + (u01(hash2(i, seed)) - 0.5) * 2 * amt);
  }

  // ── 4. Light ────────────────────────────────────────────────────
  // One lamp, up and to the left, a little in front: the pixel-art default.
  const LX = -0.58, LY = -0.62, LZ = 0.53;
  const L2X = -0.683, L2Y = -0.730;          // the same, flattened onto the page

  // Outward normal of f at (x, y), by forward difference off the value f0
  // already known there. Written to GX / GY rather than allocated.
  let GX = 0, GY = 0;
  function gradient(f, x, y, f0) {
    const gx = f(x + 0.3, y) - f0, gy = f(x, y + 0.3) - f0;
    const l = Math.sqrt(gx * gx + gy * gy) || 1;
    GX = gx / l; GY = gy / l;
  }

  // Tone for a point inside part P: d < 0 is its distance with any stroke
  // width taken off, raw the field's own value there. unit = design units
  // per art pixel.
  function lit(P, x, y, d, unit, raw) {
    const sh = P.shade;
    let nx = 0, ny = 0, nz = 1;
    switch (sh.t) {
      case 'flat': return sh.tone == null ? BASE : sh.tone;
      case 'glow': {                         // emissive: bright to a white-hot core
        const e = -d / unit;
        return e < (sh.rim || 1) ? BASE : (e < (sh.core || 2.6) ? HI : GLINT);
      }
      case 'sphere': {
        nx = (x - sh.cx) / sh.r; ny = (y - sh.cy) / sh.r;
        const q = nx * nx + ny * ny;
        if (q >= 1) { const s = 1 / Math.sqrt(q); nx *= s; ny *= s; nz = 0; } else nz = Math.sqrt(1 - q);
        break;
      }
      case 'cyl': {                          // axis a -> b, radius r
        const t = clamp(((x - sh.ax) * sh.px + (y - sh.ay) * sh.py) / sh.r, -1, 1);
        nx = t * sh.px; ny = t * sh.py; nz = Math.sqrt(1 - t * t);
        break;
      }
      case 'round': {                        // a dome sh.r deep, off the distance field
        const s = clamp(1 + d / sh.r, 0, 1);
        if (s > 0) { gradient(P.f, x, y, raw); nx = GX * s; ny = GY * s; nz = Math.sqrt(Math.max(0, 1 - s * s)); }
        break;
      }
      default: {                             // bevel: a hard edge a pixel or two wide
        const e = -d / unit, hw = sh.hw || 1, sw = sh.sw || 1.6;
        if (e >= hw && e >= sw) return sh.tone == null ? BASE : sh.tone;
        gradient(P.f, x, y, raw);
        const facing = GX * L2X + GY * L2Y;
        if (e < hw && facing > 0.3) return HI;
        if (e < sw && facing < -0.3) return SHD;
        return sh.tone == null ? BASE : sh.tone;
      }
    }
    const I = nx * LX + ny * LY + nz * LZ;
    if (I > (sh.g || 0.97)) return GLINT;
    if (I > (sh.hi || 0.74)) return HI;
    if (I < (sh.lo || 0.24)) return SHD;
    return BASE;
  }
  // Cylinder shading helper: axis a -> b, radius r.
  function cyl(ax, ay, bx, by, r, o) {
    const l = len(bx - ax, by - ay) || 1;
    return Object.assign({ t: 'cyl', ax, ay, px: -(by - ay) / l, py: (bx - ax) / l, r }, o);
  }
  // The same, for a shape drawn through rotate(f, deg): the axis has to turn
  // with it, or the bands stay upright on a tilted can.
  function rcyl(deg, ax, ay, bx, by, r, o) {
    const c = Math.cos(deg * PI / 180), s = Math.sin(deg * PI / 180);
    const t = (x, y) => [16 + (x - 16) * c - (y - 16) * s, 16 + (x - 16) * s + (y - 16) * c];
    const a = t(ax, ay), b = t(bx, by);
    return cyl(a[0], a[1], b[0], b[1], r, o);
  }

  // ── 5. The drawing ──────────────────────────────────────────────
  const I = {};                               // id -> (k) => void, filled in below

  function builder(id) {
    const solids = [], decals = [], fxs = [];
    let s = hash(Math.imul(id | 0, 0x2545F491) + 0x51ED);
    const nextSeed = () => (s = hash(s + 0x9E3779B9));
    const shadeOf = (v) => (typeof v === 'string' ? { t: v } : v);
    const k = {
      seed: s,
      rnd: (i) => u01(hash2(i, s ^ 0xC0FFEE)),         // stable per-icon random, [0,1)
      // A solid part. Options: shade, outline, grime, rust, speck, wear, w,
      // glow {r, mat, p}, group. Returns its index (for decal `on`).
      solid(f, mat, o = {}) {
        const P = Object.assign({ mat, shade: 'bevel', outline: true, grime: 0.22, rust: 0, speck: 0.03,
          wear: 0.28, w: 0, glow: null, seed: nextSeed() }, o);
        P.shade = shadeOf(P.shade);
        if (P.shade.t === 'glow') { if (o.outline == null) P.outline = false; P.grime = 0; P.speck = 0; }
        P.f = P.wear > 0 ? rough(f, P.wear, 0.55, P.seed) : f;
        if (P.group == null) P.group = solids.length;
        solids.push(P);
        return solids.length - 1;
      },
      // Paint on a solid (default: the last one drawn). Options: tone (fixed),
      // dt (shift the lit tone), shade (own lighting), mat, w, p, on.
      decal(f, mat, o = {}) {
        const D = Object.assign({ f, mat, on: solids.length - 1, tone: null, dt: 0, shade: null, w: 0, p: 1,
          seed: nextSeed() }, o);
        if (D.shade) D.shade = shadeOf(D.shade);
        decals.push(D);
      },
      // A 1 px line on a solid: seams, cracks, stitches. tone defaults to LINE.
      line(f, o = {}) { k.decal(f, o.mat || null, Object.assign({ tone: LINE, w: 1 }, o)); },
      // Unlit marks over everything. under: only on empty pixels.
      fx(f, mat, o = {}) {
        fxs.push(Object.assign({ f, mat, tone: BASE, w: 0, p: 1, under: false, seed: nextSeed() }, o));
      },
    };
    return { k, solids, decals, fxs };
  }

  const NB = [1, 0, -1, 0, 0, 1, 0, -1];

  // Which 4 x 4-unit cells of the design grid a field can reach at all. A
  // cell whose centre is further out than its half-diagonal, plus twice the
  // noise the field may carry, plus pad, holds no inside point: skip it.
  const CELL = 4, CN = 8, REACH = CELL * Math.SQRT1_2 + 0.75;
  function reach(f, pad) {
    const m = new Uint8Array(CN * CN);
    const amp = f.amp === undefined ? Infinity : f.amp;
    if (!(amp < Infinity)) return m.fill(1);
    const lim = REACH + 2 * amp + pad;
    for (let cy = 0; cy < CN; cy++) {
      for (let cx = 0; cx < CN; cx++) m[cy * CN + cx] = f((cx + 0.5) * CELL, (cy + 0.5) * CELL) < lim ? 1 : 0;
    }
    return m;
  }

  function rasterize(id, n) {
    const def = I[id] || I[fallbackKey(id)] || I.skull;
    const B = builder(typeof id === 'number' ? id : 999);
    def(B.k, n);
    const { solids, decals, fxs } = B;
    const unit = 32 / n, NN = n * n;
    const on = solids.map(() => []);            // decals, bucketed by the solid they paint
    for (const D of decals) if (on[D.on]) on[D.on].push(D);
    for (const S of solids) {
      S.off = S.w * 0.5 * unit;
      S.cells = reach(S.f, S.off);
      if (S.glow) S.gcells = reach(S.f, S.off + S.glow.r);
    }
    for (const F of fxs) { F.off = F.w * 0.5 * unit; F.cells = reach(F.f, F.off); }
    const cellOf = new Uint8Array(NN);
    for (let j = 0; j < n; j++) {
      const cy = Math.min(CN - 1, ((j + 0.5) * unit / CELL) | 0);
      for (let i = 0; i < n; i++) cellOf[j * n + i] = cy * CN + Math.min(CN - 1, ((i + 0.5) * unit / CELL) | 0);
    }
    const top = new Int16Array(NN).fill(-1);
    const pm = new Array(NN), pt = new Int8Array(NN);
    const out = new Uint32Array(NN);

    for (let j = 0; j < n; j++) {
      for (let i = 0; i < n; i++) {
        const x = (i + 0.5) * unit, y = (j + 0.5) * unit, p = j * n + i, c = cellOf[p];
        let si = solids.length - 1, raw = 0;
        for (; si >= 0; si--) {
          const S = solids[si];
          if (!S.cells[c]) continue;
          raw = S.f(x, y);
          if (raw - S.off < 0) break;
        }
        if (si < 0) continue;
        const P = solids[si], d = raw - P.off;
        let m = P.mat, t = lit(P, x, y, d, unit, raw);
        if (P.shade.t !== 'glow') {
          if (P.rust > 0 && fbm(x * 0.42, y * 0.42, P.seed ^ 0x2C1B) > 1 - P.rust * 0.55) m = P.rustMat || 'rust';
          if (P.grime > 0 && t >= BASE && fbm(x * 0.3, y * 0.3, P.seed) > 1 - P.grime * 0.5) t--;
          if (P.speck > 0 && t >= BASE && u01(hash2(i + j * 131, P.seed ^ 0x77)) < P.speck) t--;
        }
        const ds = on[si];
        for (let q = 0; q < ds.length; q++) {
          const D = ds[q], draw = D.f(x, y), dd = draw - D.w * 0.5 * unit;
          if (dd >= 0) continue;
          if (D.p < 1 && u01(hash2(i * 7 + j * 1031, D.seed)) >= D.p) continue;
          if (D.mat) m = D.mat;
          if (D.tone != null) t = D.tone;
          else if (D.shade) t = lit(D, x, y, dd, unit, draw);
          else t = clamp(t + D.dt, 0, 4);
        }
        top[p] = si; pm[p] = m; pt[p] = t;
      }
    }

    // A part laid over another gets a line where it sits on it; the line is
    // drawn on the part underneath, so the one on top keeps its full shape.
    for (let p = 0; p < NN; p++) {
      const si = top[p];
      if (si < 0) continue;
      const i = p % n, j = (p / n) | 0;
      for (let q = 0; q < 8; q += 2) {
        const a = i + NB[q], b = j + NB[q + 1];
        if (a < 0 || b < 0 || a >= n || b >= n) continue;
        const ti = top[b * n + a];
        if (ti > si && solids[ti].outline && solids[ti].group !== solids[si].group) { pt[p] = LINE; break; }
      }
    }

    for (let p = 0; p < NN; p++) {
      const si = top[p];
      if (si >= 0) { out[p] = colour(pm[p], pt[p]); continue; }
      const i = p % n, j = (p / n) | 0;
      let best = -1;                        // outline: the topmost outlined neighbour
      for (let q = 0; q < 8; q += 2) {
        const a = i + NB[q], b = j + NB[q + 1];
        if (a < 0 || b < 0 || a >= n || b >= n) continue;
        const ti = top[b * n + a];
        if (ti > best && solids[ti].outline) best = ti;
      }
      if (best >= 0) { out[p] = colour(solids[best].mat, LINE); continue; }
      const x = (i + 0.5) * unit, y = (j + 0.5) * unit, c = cellOf[p];
      for (let s2 = solids.length - 1; s2 >= 0; s2--) {   // aura: sparse, falling off
        const S = solids[s2], G = S.glow;
        if (!G || !S.gcells[c]) continue;
        const dd = S.f(x, y) - S.off;
        if (dd >= G.r) continue;
        const f = 1 - Math.max(0, dd) / G.r;
        if (u01(hash2(i * 13 + j * 911, S.seed ^ 0x3D)) < (G.p || 0.5) * f * f) {
          out[p] = colour(G.mat, f > 0.6 ? BASE : SHD);
          break;
        }
      }
    }

    for (const F of fxs) {
      for (let p = 0; p < NN; p++) {
        if (!F.cells[cellOf[p]] || (F.under && top[p] >= 0)) continue;
        const i = p % n, j = (p / n) | 0, x = (i + 0.5) * unit, y = (j + 0.5) * unit;
        if (F.f(x, y) - F.off >= 0) continue;
        if (F.p < 1 && u01(hash2(i * 17 + j * 499, F.seed)) >= F.p) continue;
        out[p] = colour(F.mat, F.tone);
      }
    }
    return out;
  }

  // Items with no drawing of their own wear their category's glyph.
  function fallbackKey(id) {
    const item = typeof getItemById === 'function' ? getItemById(id) : null;
    return item ? 'cat' + item.category : 'skull';
  }

  // ════════════════════════════════════════════════════════════════
  // The icons. Light is top left; keep objects inside ~3..29.
  // ════════════════════════════════════════════════════════════════

  // ── Gulpables ───────────────────────────────────────────────────

  // 1 Trauma Patch: adhesive square, gauze pad, dried blood soaking through,
  // one corner peeling up.
  I[1] = (k) => {
    const r = (f) => rotate(f, -12);
    k.solid(r(minus(box(16, 16.5, 11, 11, 2.5), poly([29, 4, 29, 13, 20, 4]))), 'tan', { grime: 0.3 });
    k.line(r(dots(jit([8, 8.5, 12, 7.5, 8, 24, 13, 25.5, 24.5, 25, 25, 20, 7.5, 16], 0.4, 3))), { tone: SHD });
    k.solid(r(box(16.2, 16.7, 6.6, 6.4, 1)), 'bone', { grime: 0.15, wear: 0.2 });
    k.decal(r(rough(circle(16.6, 17.2, 3.5), 1.4, 0.5, 11)), 'blood', { shade: { t: 'flat', tone: BASE } });
    k.decal(r(rough(circle(15.6, 18.2, 1.6), 0.8, 0.9, 12)), 'blood', { tone: SHD });
    k.solid(r(poly([20.2, 4.4, 28.6, 12.8, 25.8, 6.2])), 'tan', { shade: { t: 'flat', tone: HI }, grime: 0 });
  };

  // 2 Mystery Rations: dented tin, faded label that only says FOOD.
  I[2] = (k, n) => {
    const body = minus(union(box(16, 19, 8, 9), ellipse(16, 28, 8, 1.8)), rough(circle(25.4, 21.5, 2.2), 0.5, 0.9, 4));
    k.solid(body, 'steel', { shade: cyl(16, 0, 16, 32, 8.4), rust: 0.35, grime: 0.3 });
    k.decal(rough(box(16, 19.5, 9, 5, 0), 0.5, 0.7, 5), 'bone', { dt: 0 });
    if (n >= 30) {                                                    // F O O D
      const L = [9.5, 17.5, 9.5, 21.5, 9.5, 17.5, 11.5, 17.5];
      k.line(union(stroke(L), stroke([9.5, 19.5, 11, 19.5]),
        stroke([13.5, 17.5, 15.5, 17.5, 15.5, 21.5, 13.5, 21.5, 13.5, 17.5]),
        stroke([17.5, 17.5, 19.5, 17.5, 19.5, 21.5, 17.5, 21.5, 17.5, 17.5]),
        stroke([21.5, 17.5, 21.5, 21.5, 22.6, 21.5, 23.2, 20.5, 23.2, 18.5, 22.6, 17.5, 21.5, 17.5])),
        { mat: 'rust', tone: SHD });
    } else {                                                          // too small to spell
      k.decal(box(16, 19.5, 6, 1.6), 'rust', { tone: SHD });
    }
    k.line(stroke([23.4, 23.4, 24.6, 20.2]), { tone: SHD });          // the dent's crease
    k.decal(union(stroke([10.5, 11, 10.8, 14.5], 0.6, 0.2), stroke([19.5, 11, 19.2, 13.4], 0.6, 0.2)), 'rust', { dt: 0 });
    k.solid(ellipse(16, 10, 8, 2.4), 'steel', { shade: { t: 'flat', tone: HI }, rust: 0.5 });
    k.decal(ellipse(16, 10.3, 6.2, 1.4), null, { tone: SHD });
  };

  // 3 Almost Water: sealed pouch sagging with water, spout, clip-on filter,
  // beads of condensation.
  I[3] = (k) => {
    const bag = rough(grow(poly([9.5, 10.5, 19.5, 10.5, 22, 26.5, 6.5, 26.5]), 2.2), 0.45, 0.6, 2);
    k.solid(bag, 'water', { shade: { t: 'round', r: 6 }, grime: 0.12 });
    k.decal(rough(box(14, 10, 10, 3.2), 0.5, 0.8, 3), null, { dt: 1 });          // the air above the water
    k.decal(rough(stroke([7.5, 13.4, 11, 12.8, 14.5, 13.5, 18, 12.9, 21, 13.4]), 0.25, 1, 4), 'water', { tone: GLINT, w: 1 });
    k.decal(dots(jit([10.5, 18, 13, 23.5, 18.5, 17, 16, 21, 19.5, 23.5], 0.7, 7)), 'water', { tone: GLINT, w: 1 });
    k.decal(stroke([7.6, 16.5, 7.2, 22.5]), 'water', { tone: HI, w: 1 });
    k.solid(box(14.5, 7.6, 2.4, 1.6, 0.4), 'water', { shade: 'bevel' });            // spout
    k.solid(box(14.5, 4.9, 3, 1.6, 0.8), 'white', { grime: 0.3 });                   // cap
    k.fx(dots([13, 29]), 'water', { tone: HI, w: 1 });                                // a drip
    k.solid(box(25.2, 18.5, 2.5, 5.4, 1.2), 'olive', { shade: cyl(25.2, 0, 25.2, 32, 2.5) });
    k.solid(union(box(25.2, 12.6, 1.8, 1, 0.3), box(25.2, 24.4, 1.8, 1, 0.3)), 'iron');
    k.solid(box(23.2, 15.4, 2.2, 0.9, 0.2), 'steel', { shade: { t: 'flat', tone: HI }, wear: 0 });  // the clip
  };

  // 4 Glow Flush: auto-injector of sickly green, trefoil on the barrel.
  I[4] = (k) => {
    k.solid(stroke([4.5, 27.5, 8.5, 23.5]), 'steel', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(seg(8.2, 23.8, 10, 22, 1.8), 'iron');
    k.solid(seg(10, 22, 21, 11, 3.6), 'glass', { shade: cyl(10, 22, 21, 11, 3.6), grime: 0.1 });
    k.decal(seg(11, 21, 19.6, 12.4, 2.4), 'rad', { shade: { t: 'glow', rim: 0.8, core: 1.9 } });
    k.decal(trefoil(15.2, 16.8, 2.9), 'black', { tone: BASE });
    k.decal(stroke([10.8, 18.6, 17.4, 12]), 'glass', { tone: GLINT, w: 1 });
    k.solid(seg(21, 11, 23.5, 8.5, 2.9), 'iron', { shade: cyl(21, 11, 23.5, 8.5, 2.9) });
    k.solid(seg(24.2, 7.8, 27.2, 4.8, 1.4), 'steel', { shade: cyl(24.2, 7.8, 27.2, 4.8, 1.4) });
  };

  // 5 Panic Juice: red stimulant in a spring-loaded injector, hazard label,
  // hairline crack.
  I[5] = (k) => {
    // Upright: tilted, its dozen small parts scattered into noise at 26 px.
    k.solid(stroke([16, 27, 16, 30.4]), 'steel', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(box(16, 25.6, 2.4, 1.4, 0.4), 'iron');
    k.solid(box(16, 17.6, 5, 7, 1.8), 'glass', { shade: cyl(16, 0, 16, 32, 5), grime: 0.1 });
    k.decal(box(16, 19.4, 3.8, 4.8, 1.2), 'red', { shade: { t: 'glow', rim: 0.9, core: 2.4 } });
    k.decal(box(16, 16.6, 5.2, 2), 'mustard', { dt: 0 });
    k.decal(inter(box(16, 16.6, 5.2, 2), stripes(3.2, 1, 1)), 'black', { tone: BASE });
    k.decal(stroke([13.4, 11.4, 14.8, 13, 14.2, 14.2]), 'glass', { tone: GLINT, w: 1 });
    k.decal(stroke([17.8, 20.4, 18.6, 22.6, 17.6, 23.8]), 'red', { tone: GLINT, w: 1 });   // the crack runs on
    k.solid(stroke([13, 9.4, 19, 8, 13, 6.6, 19, 5.2]), 'steel', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(box(16, 10.4, 5.6, 1.2, 0.4), 'iron', { wear: 0 });
    k.solid(box(16, 3.4, 4.6, 1.8, 1.2), 'red', { shade: { t: 'round', r: 1.6 }, grime: 0, speck: 0, wear: 0 });
  };

  // 6 Sweet Oblivion: little brown tincture bottle, cork, scrawled label.
  I[6] = (k) => {
    const body = union(box(16, 21, 6.4, 7.2, 2.2), ellipse(16, 14.4, 6.4, 3), box(16, 11, 2.6, 2.4));
    k.solid(body, 'brownglass', { shade: cyl(16, 0, 16, 32, 6.4), grime: 0.15 });
    k.decal(rough(box(16, 21.2, 6.8, 4, 0.3), 0.45, 0.7, 9), 'bone', { dt: 0 });
    k.line(union(stroke(jit([11, 20, 12.5, 19.3, 14, 20.4, 15.5, 19.4, 17, 20.3, 18.5, 19.6, 20.5, 20.2], 0.35, 5)),
      stroke(jit([11.5, 22.8, 13.5, 22.2, 15, 23, 17.5, 22.4], 0.35, 6))), { mat: 'blood', tone: SHD });
    k.decal(stroke([11.4, 14.8, 11.4, 17.2]), 'brownglass', { tone: GLINT, w: 1 });
    k.decal(stroke([11.4, 25, 11.4, 26.6]), 'brownglass', { tone: HI, w: 1 });
    k.solid(rough(box(16, 7.4, 3.1, 2.4, 0.8), 0.35, 0.9, 3), 'cork', { grime: 0.3, speck: 0.12 });
  };

  // 7 Calorie Brick: foil-wrapped slab, loud red band, a bite out of the
  // corner showing the brick underneath.
  I[7] = (k) => {
    const r = (f) => rotate(f, -9);
    const slab = r(box(16, 17, 11.5, 5.8, 1));
    const bite = r(rough(circle(26.5, 11.8, 4.6), 0.9, 1.3, 21));
    const peel = r(rough(circle(26.5, 11.8, 6.8), 1.1, 0.8, 22));
    k.solid(minus(slab, bite), 'ration', { grime: 0.2, speck: 0.2 });
    k.solid(minus(slab, peel), 'foil', { shade: { t: 'bevel', hw: 1, sw: 1.6 }, grime: 0.2 });
    k.decal(r(box(12.5, 17, 3.2, 6)), 'red', { dt: 0 });
    k.decal(r(circle(12.5, 17, 2.5)), 'mustard', { tone: HI });                     // a smiley. Of course.
    k.decal(r(union(dots([11.6, 16.2, 13.4, 16.2]), curve(11.2, 17.6, 12.5, 19.2, 13.8, 17.6))), 'mustard', { tone: LINE, w: 1 });
    k.line(r(union(stroke(jit([6.5, 13, 7.2, 21], 0.3, 1)), stroke(jit([8, 12.5, 8.6, 21.5], 0.3, 2)))), { tone: SHD });
    k.line(r(union(stroke([17.5, 13.5, 19.5, 15]), stroke([19, 19.5, 21.5, 18.5]), stroke([16.5, 21, 18, 20.2]))), { tone: HI });
  };

  // 8 Screaming Spike: fat auto-injector, red cap, needle out, and it's
  // yelling about it.
  I[8] = (k) => {
    k.solid(stroke([22.5, 22.5, 27.5, 27.5]), 'steel', { w: 1, shade: { t: 'flat', tone: GLINT }, wear: 0 });
    k.solid(seg(20.5, 20.5, 22.8, 22.8, 2), 'iron');
    k.solid(seg(11, 11, 20.5, 20.5, 4.6), 'olive', { shade: cyl(11, 11, 20.5, 20.5, 4.6), grime: 0.3, rust: 0 });
    k.decal(seg(14.5, 14.5, 18.2, 18.2, 2.2), 'amber', { shade: { t: 'glow', rim: 0.8, core: 5 } });
    k.decal(stroke([10.2, 16.8, 16.8, 10.2]), 'olive', { tone: SHD, w: 1 });
    k.solid(seg(6.6, 6.6, 10.6, 10.6, 5.1), 'red', { shade: cyl(6.6, 6.6, 10.6, 10.6, 5.1) });
    k.fx(union(stroke([2.5, 13.5, 0.8, 15.4]), stroke([3.4, 17, 2.2, 19.4]), stroke([13.2, 2.8, 15.4, 1]), stroke([16.8, 3.8, 19.4, 2.4])),
      'amber', { tone: HI, w: 1 });
  };

  // 9 Anti-Rot Kit: rusted first-aid tin, faded cross, dented corners.
  I[9] = (k) => {
    const tin = minus(box(16, 17, 12, 9, 2.4), rough(circle(28.6, 7.6, 2.2), 0.4, 1, 3), rough(circle(3.4, 26.8, 1.8), 0.4, 1, 4));
    k.solid(tin, 'steel', { rust: 0.6, grime: 0.35 });
    k.decal(box(16, 24.4, 12.5, 1.7), null, { dt: -1 });                          // the tin's side
    k.line(stroke([4.6, 22.6, 27.4, 22.6]), { tone: LINE });
    k.decal(rough(union(box(16, 14.6, 1.9, 5.2), box(16, 14.6, 5.2, 1.9)), 0.35, 0.9, 7), 'red', { dt: 0, p: 0.86 });
    k.solid(box(16, 25.2, 2.2, 1.2, 0.3), 'iron');                                 // latch
    k.solid(union(box(9, 8, 1.6, 0.9), box(23, 8, 1.6, 0.9)), 'iron');            // hinges
  };

  // 10 Bright Bad Idea: a lit road flare, spitting, smoking.
  I[10] = (k) => {
    k.fx(rough(curve(23.5, 8, 21, 3, 15.5, 2.2, 1.4, 0.4), 0.5, 0.7, 5), 'smoke', { tone: BASE, under: true });
    k.solid(seg(6.5, 26.5, 21, 12, 2.8), 'red', { shade: cyl(6.5, 26.5, 21, 12, 2.8), grime: 0.25 });
    k.decal(seg(6, 27, 8.6, 24.4, 3), 'bone', { dt: 0 });
    k.decal(stroke([11.6, 19.6, 13.4, 21.4]), 'red', { tone: LINE, w: 1 });
    k.solid(rough(circle(22.4, 10.6, 3.2), 0.9, 1.1, 8), 'fire', { shade: { t: 'glow', rim: 0.9, core: 2 },
      glow: { r: 5.5, mat: 'fire', p: 0.55 } });
    k.fx(dots(jit([27.5, 6.5, 28.5, 12.5, 25.5, 4, 18.5, 5.5, 26.5, 16, 29.5, 9], 0.9, 9)), 'fire', { tone: HI, w: 1 });
  };

  // 30 Sour Cream Tub: plastic tub, peeling label, a spoon standing up in it.
  I[30] = (k) => {
    k.solid(grow(poly([8, 12.5, 24, 12.5, 22.4, 27, 9.6, 27]), 1.2), 'white', { shade: cyl(16, 0, 16, 32, 9.4), grime: 0.3 });
    k.decal(rough(poly([7.4, 16.6, 24.6, 16.6, 23.8, 23.6, 8.2, 23.6]), 0.35, 0.8, 5), 'water', { dt: 0 });
    k.decal(stroke([7.4, 20.2, 24.6, 20.2]), 'white', { dt: 0, w: 1 });
    k.solid(poly([20.6, 16.6, 24.4, 16.6, 23.4, 21.2]), 'water', { shade: { t: 'flat', tone: HI }, grime: 0, wear: 0.1 });
    k.solid(ellipse(16, 12.5, 9.4, 2.5), 'white', { shade: { t: 'flat', tone: HI } });
    k.decal(ellipse(16, 12.7, 7.8, 1.5), 'bone', { tone: GLINT });
    k.solid(seg(17.6, 12.4, 22.6, 3, 1.15), 'steel', { shade: cyl(17.6, 12.4, 22.6, 3, 1.15) });
  };

  // 31 Trippy Juice: round flask of glowing neon orange, a swirl going round
  // in it, biohazard tape on the neck.
  I[31] = (k) => {
    k.solid(union(circle(16, 20.5, 7.6), box(16, 10.5, 2.6, 4.5)), 'glass',
      { shade: { t: 'sphere', cx: 16, cy: 20.5, r: 7.6 }, grime: 0.1, glow: { r: 4.5, mat: 'orange', p: 0.45 } });
    k.decal(minus(circle(16, 20.8, 6.5), box(16, 13, 8, 2.6)), 'orange', { shade: { t: 'glow', rim: 1, core: 3.2 } });
    k.decal(rough(curve(11.2, 20.5, 16, 14.5, 20.6, 22.5), 0.3, 1, 4), 'fire', { tone: GLINT, w: 1 });
    k.decal(curve(12.6, 24.2, 17, 27.4, 20, 23), 'orange', { tone: SHD, w: 1 });
    k.decal(stroke([10.8, 17.6, 10.4, 20.6]), 'glass', { tone: GLINT, w: 1 });
    k.solid(box(16, 10.8, 3.6, 1.8, 0.3), 'mustard', { grime: 0.3 });
    k.decal(dots([16, 10.8]), 'black', { tone: BASE, w: 2 });
    k.solid(poly([19.2, 9.4, 25, 7.6, 23.8, 10.8, 19.4, 11.8]), 'mustard', { shade: { t: 'flat', tone: HI } });
    k.solid(box(16, 6, 2.9, 1.8, 0.8), 'cork');
  };

  // 44 Jar of Sweats: murky jar of something brown, hand-written label,
  // rusted lid, and it is sweating.
  I[44] = (k) => {
    k.solid(box(16, 19.5, 8.4, 8.6, 2.6), 'glass', { shade: cyl(16, 0, 16, 32, 8.4), grime: 0.2 });
    k.decal(box(16, 21.4, 7.4, 6.4, 1.8), 'ration', { dt: -1 });
    k.decal(rough(stroke([8.4, 15.4, 12, 14.8, 16, 15.5, 20, 14.9, 23.6, 15.4]), 0.25, 1, 2), 'ration', { tone: HI, w: 1 });
    k.decal(dots(jit([11, 18.5, 14.5, 17.4, 19.5, 18, 21.5, 25.5, 12, 26], 0.6, 3)), 'bile', { tone: BASE, w: 1 });
    k.decal(rough(box(16, 22, 5.2, 2.6, 0.3), 0.4, 0.8, 6), 'bone', { dt: 0 });
    k.line(stroke(jit([12, 22, 13.5, 21.2, 15, 22.3, 16.5, 21.3, 18, 22.2, 19.8, 21.6], 0.3, 8)), { mat: 'blood', tone: SHD });
    k.decal(stroke([9.4, 13.5, 9.4, 24.5]), 'glass', { tone: GLINT, w: 1 });
    k.solid(box(16, 9.8, 9, 2.2, 0.8), 'rust', { grime: 0.35 });
    k.line(union(...[10, 13, 16, 19, 22].map((x) => stroke([x, 8.6, x, 11]))), { tone: SHD });
    k.solid(drop(26.5, 8, 1.3, -60), 'water', { shade: { t: 'flat', tone: HI }, grime: 0, wear: 0 });
    k.solid(drop(4.6, 13, 1.1, -120), 'water', { shade: { t: 'flat', tone: HI }, grime: 0, wear: 0 });
  };

  // 50 Uranium Candy: a twist-wrapped boiled sweet, sickly green and warm.
  // The wrapper says SAFE; it has a trefoil on it.
  I[50] = (k) => {
    const r = (f) => rotate(f, -24);
    k.solid(r(union(poly([10.5, 16, 4, 10.4, 5.6, 16, 4, 21.6]), poly([21.5, 16, 28, 10.4, 26.4, 16, 28, 21.6]))),
      'rad', { shade: { t: 'flat', tone: SHD }, grime: 0.1 });
    k.line(r(union(stroke([5.2, 12.6, 9, 15.4]), stroke([5.4, 19.6, 9, 16.8]), stroke([26.8, 12.6, 23, 15.4]),
      stroke([26.6, 19.6, 23, 16.8]))), { tone: HI });
    k.solid(r(ellipse(16, 16, 6.6, 5)), 'rad', { shade: { t: 'round', r: 4 }, glow: { r: 5.5, mat: 'rad', p: 0.5 } });
    k.decal(r(trefoil(16, 16, 3.1)), 'black', { tone: BASE });
  };

  // 52 Gutter Broth: dented tin cup of grey broth with a wrapper corner
  // poking out of it, steam coming off.
  I[52] = (k) => {
    k.fx(rough(curve(13.5, 9.5, 10.5, 5.5, 14, 1.8, 1.2, 0.3), 0.6, 0.6, 3), 'smoke', { tone: HI, under: true });
    k.fx(rough(curve(18.5, 10, 21, 6.8, 18.5, 3.6, 0.9, 0.2), 0.5, 0.7, 4), 'smoke', { tone: BASE, under: true });
    k.solid(shell(circle(24.2, 18.5, 3.4), 1.1), 'steel');
    const cup = minus(union(box(15, 19.5, 8.6, 8), ellipse(15, 27.4, 8.6, 1.6)), rough(circle(7, 22.5, 1.8), 0.4, 1, 6));
    k.solid(cup, 'steel', { shade: cyl(15, 0, 15, 32, 8.6), rust: 0.25, grime: 0.35 });
    k.solid(ellipse(15, 11.6, 8.6, 2.2), 'steel', { shade: { t: 'flat', tone: HI } });
    k.decal(ellipse(15, 11.9, 7.2, 1.4), 'ash', { tone: BASE });
    k.decal(dots([11.5, 12, 16.5, 11.4]), 'ash', { tone: HI, w: 1 });
    k.solid(poly([17, 11.8, 20.4, 8.4, 21.6, 10.6, 18.8, 12.4]), 'red', { shade: { t: 'flat', tone: BASE } });
  };

  // 53 Sock Puppet Bandage: a boiled sock doing its best as a dressing,
  // held on with a bent safety pin -- and it is a sock puppet, so: a face.
  I[53] = (k) => {
    const r = (f) => rotate(f, -16);
    const sock = r(union(box(12, 10.5, 4.4, 7.6, 1.4), seg(12, 19.5, 21, 22, 4.8), circle(21.8, 22.4, 4.6)));
    k.solid(rough(sock, 0.4, 0.7, 2), 'bone', { shade: { t: 'round', r: 3.5 }, grime: 0.35 });
    k.decal(r(box(12, 4.6, 4.8, 2.2)), null, { dt: -1 });
    k.decal(r(union(box(12, 6.9, 4.8, 0.6), box(12, 3.4, 4.8, 0.5))), 'red', { dt: 0 });
    k.decal(r(union(circle(20.2, 20.6, 1.15), circle(24.4, 21.2, 1.15))), 'black', { tone: BASE });
    k.decal(r(dots([19.8, 20.2, 24, 20.8])), 'white', { tone: HI, w: 1 });
    k.line(r(curve(19.6, 24.6, 22.6, 26.8, 25.8, 24.6)));
    k.solid(r(union(stroke([8, 13.8, 17, 12.2]), stroke([8.4, 15.8, 17, 12.2]))), 'steel',
      { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(r(circle(7.6, 14.8, 1.3)), 'steel');
  };

  // 54 Bile Flare: corked, round-bellied jar of yellow-green bile, fuming
  // at the seal.
  I[54] = (k) => {
    k.fx(union(rough(curve(9.6, 11, 6.5, 8, 8.6, 4.4, 1.3, 0.3), 0.6, 0.7, 1), rough(curve(22.8, 10.6, 26, 7.6, 23.8, 3.6, 1.1, 0.3), 0.6, 0.7, 2),
      rough(curve(16, 8.4, 14.4, 5.4, 16.6, 2.2, 0.9, 0.2), 0.5, 0.8, 3)), 'bile', { tone: BASE, p: 0.7, under: true });
    k.solid(box(16, 20, 9.4, 7.6, 3.2), 'glass', { shade: cyl(16, 0, 16, 32, 9.4), grime: 0.15 });
    k.decal(box(16, 21.2, 8.4, 6.2, 2.4), 'bile', { shade: cyl(16, 0, 16, 32, 8.4) });
    k.decal(rough(stroke([7.8, 15.4, 12, 14.8, 16, 15.5, 20, 14.9, 24.2, 15.4]), 0.25, 1, 5), 'bile', { tone: HI, w: 1 });
    k.decal(dots(jit([11.5, 19, 18.5, 23, 14.5, 25, 21, 18.6], 0.6, 4)), 'bile', { tone: GLINT, w: 1 });
    k.decal(stroke([8.6, 15, 8.6, 24.5]), 'glass', { tone: GLINT, w: 1 });
    k.solid(box(16, 11.6, 7.6, 1.6, 0.6), 'glass');                                   // the jar's lip
    k.solid(rough(box(16, 9.2, 6.4, 2.4, 1), 0.4, 0.9, 3), 'cork', { speck: 0.12 });
    k.decal(dots(jit([11.5, 9.4, 20.5, 9], 0.3, 1)), 'bile', { tone: HI, w: 1.4 });    // it weeps at the seal
  };

  // 55 Cricket Paste: clay pot of grey-brown paste, one cricket leg still
  // poking out of it.
  I[55] = (k) => {
    // A stone mortar, not a flowerpot: a leg sticking out of a pot is a plant.
    k.solid(rough(inter(ellipse(16, 16, 11.4, 11), box(16, 22.4, 12, 6.6)), 0.4, 0.7, 1), 'ceramic',
      { shade: { t: 'round', r: 5 }, grime: 0.35, speck: 0.08 });
    k.solid(rough(union(ellipse(16, 16.4, 10, 2.4), ellipse(14.6, 14.8, 5.6, 2.4)), 0.5, 0.8, 5), 'ration',
      { shade: { t: 'round', r: 3 }, speck: 0.2 });
    k.fx(union(stroke([23.4, 4.4, 24.8, 2.6]), stroke([25.8, 6.2, 27.8, 5.4])), 'amber', { tone: HI, w: 1 });   // chirp
    // The hind leg: a fat drumstick of a thigh up to a sharp knee, then a thin
    // spined shin kicking back down. Dark chitin, not twig brown.
    k.solid(curve(10.6, 13.4, 7.4, 7.2, 3.6, 6.4), 'iron', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });   // a feeler
    k.solid(stroke([27.6, 12.4, 29.4, 13.8]), 'iron', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(stroke([21.6, 3.8, 27.8, 12.6], 0.7, 0.45), 'iron', { shade: { t: 'flat', tone: HI }, wear: 0 });
    k.fx(union(stroke([23.4, 7.4, 24.6, 6.2]), stroke([25, 9.6, 26.2, 8.4])), 'iron', { tone: HI, w: 1 });
    k.solid(seg(15.4, 14, 21.4, 3.8, 2.2, 1), 'iron', { shade: { t: 'round', r: 1.8 }, wear: 0 });
  };

  // 56 Tooth Whiskey: glass hip flask of electric-amber liquor, a cracked
  // tooth on the label, and it crackles.
  I[56] = (k) => {
    k.solid(union(box(16, 19.5, 8, 8.8, 3), box(16, 9.6, 2.4, 2)), 'glass', { shade: { t: 'round', r: 4 }, grime: 0.12 });
    k.decal(box(16, 21, 6.8, 6.8, 2.2), 'amber', { shade: { t: 'glow', rim: 1, core: 3.6 } });
    k.decal(rough(box(16, 20.5, 4.2, 4.4, 0.5), 0.3, 0.9, 3), 'bone', { dt: 0 });
    k.decal(union(box(16, 19.2, 2.6, 1.8, 0.8), seg(14.6, 20.5, 14.2, 23.2, 0.9), seg(17.4, 20.5, 17.8, 23.2, 0.9)), 'white', { tone: HI });
    k.line(stroke([16.4, 17.6, 15.6, 19.2, 16.6, 20.4]), { mat: 'blood', tone: SHD });
    k.decal(stroke([9.4, 13.5, 9.4, 18]), 'glass', { tone: GLINT, w: 1 });
    k.solid(box(16, 6.6, 3.1, 1.6, 0.6), 'steel');
    k.fx(union(stroke([24.5, 12.5, 26.4, 11, 25.4, 10.2, 27.4, 8.4]), stroke([6.2, 25.4, 4.4, 27.2])), 'amber', { tone: GLINT, w: 1 });
  };

  // 57 Squelch Bandage: a wet roll of bandage with something green seeping
  // through and a tail hanging off it.
  I[57] = (k) => {
    const r = (f) => rotate(f, -14);
    k.solid(r(poly([18, 20.5, 27.5, 24.5, 26.8, 28.5, 17, 24.6])), 'bone', { grime: 0.35 });
    k.solid(r(box(17.5, 15, 8.5, 6.5, 1.5)), 'bone', { shade: rcyl(-14, 0, 15, 32, 15, 6.5), grime: 0.3 });
    k.decal(r(rough(circle(20, 14, 3.4), 1.3, 0.6, 9)), 'rad', { dt: -1 });
    k.decal(r(dots(jit([13, 11.5, 23, 12.4, 16, 12], 0.4, 2))), 'white', { tone: GLINT, w: 1 });
    k.solid(r(ellipse(9, 15, 3.2, 6.5)), 'bone', { shade: { t: 'flat', tone: BASE } });
    k.line(r(union(shell(ellipse(9, 15, 1.9, 4.2), 0), dots([9, 15]))), { tone: SHD });
    k.solid(r(drop(21.5, 26.5, 1.1, -90)), 'rad', { shade: { t: 'flat', tone: BASE }, wear: 0, grime: 0 });
  };

  // 58 Nostril Salts: a vial of smelling salts cracked open, crystals
  // spilling out, fumes curling off them.
  I[58] = (k) => {
    k.fx(union(stroke(jit([19.5, 12.5, 17.5, 10.5, 19.5, 8.5, 17.8, 6.5, 19.2, 4.4], 0.35, 1)),
      stroke(jit([23.5, 14.5, 25.4, 12.4, 23.6, 10.4, 25.2, 8.4], 0.35, 2)),
      stroke(jit([27, 17, 28.6, 15.4, 27.2, 13.6], 0.3, 3))), 'white', { tone: SHD, w: 1, p: 0.85, under: true });
    const vial = minus(seg(5.6, 26.4, 16.4, 15.6, 3.4), rough(circle(18.4, 13.6, 3.1), 1, 1.1, 4));
    k.solid(vial, 'glass', { shade: cyl(5.6, 26.4, 16.4, 15.6, 3.4), grime: 0.12 });
    k.decal(seg(6.6, 25.4, 14, 18, 2.2), 'white', { dt: 0, p: 0.9 });
    k.decal(stroke([4.4, 23.4, 11.8, 16]), 'glass', { tone: GLINT, w: 1 });
    k.solid(dots(jit([18, 19, 20.5, 17, 22.5, 20, 25, 18.4, 20, 22, 26.8, 21.6], 0.5, 6), 0.95), 'white',
      { shade: { t: 'flat', tone: HI }, wear: 0, grime: 0 });
  };

  // 59 Marrow Jelly: a jar of pale jelly with a bone fragment hanging in it.
  I[59] = (k) => {
    k.solid(box(16, 19.5, 8, 8.6, 2.6), 'glass', { shade: cyl(16, 0, 16, 32, 8), grime: 0.15 });
    k.decal(box(16, 20.8, 7, 7.2, 2), 'jelly', { shade: { t: 'round', r: 4 } });
    const bone = union(seg(13, 22.6, 19, 18.4, 1), circle(12.2, 23.2, 1.4), circle(13.6, 24, 1.2), circle(19.6, 17.4, 1.4), circle(20.3, 18.9, 1.2));
    k.decal(grow(bone, 0.8), 'jelly', { tone: SHD });
    k.decal(bone, 'bone', { tone: HI });
    k.decal(stroke([9.4, 13.5, 9.4, 24.5]), 'glass', { tone: GLINT, w: 1 });
    k.solid(box(16, 9.8, 8.8, 2, 0.8), 'steel', { rust: 0.35 });
  };

  // 60 Static Chew: a wad of bare copper wire, chewed like gum, sparking.
  I[60] = (k) => {
    const wire = { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 };
    k.solid(stroke([21.5, 12, 25.5, 7.5, 27.5, 4.5]), 'copper', wire);
    k.solid(stroke([9.4, 21, 4.5, 23.6]), 'copper', wire);
    k.solid(rough(ellipse(15.5, 17, 8, 6.8), 1.1, 0.5, 7), 'copper', { shade: { t: 'round', r: 5 }, grime: 0.2 });
    k.line(union(curve(8.5, 15, 14, 9.5, 21.5, 12), curve(9, 20, 15, 14, 23, 18), curve(11, 23.5, 18, 19, 22.5, 22.5),
      curve(12, 11.5, 10, 17, 14, 23)), { tone: SHD });
    k.line(union(curve(9.5, 16.5, 14.5, 11.5, 20.5, 14), curve(10, 21.5, 16, 16, 22, 20.5)), { tone: GLINT });
    k.fx(union(stroke([3.6, 11.6, 5.8, 13.4, 4.6, 14.6, 6.8, 16.2]), stroke([25, 24, 27.6, 25, 26, 26.4, 28.5, 27.6]),
      stroke([16.6, 4.6, 18.2, 6.8, 16.6, 7.6, 18, 9.6])), 'blue', { tone: HI, w: 1 });
  };

  // 61 Blister Balm: open tin of bruise-purple ointment with somebody's
  // fingerprint in it.
  I[61] = (k) => {
    k.solid(ellipse(21.5, 11, 8, 4.8), 'steel', { rust: 0.3 });
    k.decal(ellipse(21.5, 11, 6.2, 3.2), null, { dt: -1 });
    k.solid(union(box(14, 21.4, 10, 3.4), ellipse(14, 24.8, 10, 3)), 'steel', { shade: cyl(14, 0, 14, 32, 10), rust: 0.25 });
    k.solid(ellipse(14, 18, 10, 3.6), 'steel', { shade: { t: 'flat', tone: HI } });
    k.decal(ellipse(14, 18.2, 8.6, 2.8), 'purple', { shade: { t: 'round', r: 2.5 } });
    k.decal(union(shell(ellipse(15.6, 18, 3.2, 1.4), 0), shell(ellipse(15.6, 18, 1.6, 0.6), 0)), 'purple', { tone: HI, w: 1 });
  };

  // 62 Panic Dart: a whittled length of tentacle, bone barb for a point, rags
  // for flights. Already in the air.
  I[62] = (k) => {
    k.fx(union(stroke([2.6, 21.5, 7.5, 20.6]), stroke([4, 26.5, 8.6, 24.8]), stroke([8.6, 29.4, 11.6, 27.6])), 'smoke', { tone: HI, w: 1 });
    k.solid(rough(union(poly([9.4, 22, 4.6, 18.4, 7.2, 17.4, 11.6, 20.4]), poly([10.2, 23.2, 8, 28.2, 6.4, 25.2, 8.8, 22])), 0.6, 1, 3),
      'red', { grime: 0.4, shade: { t: 'flat', tone: BASE } });
    k.solid(seg(9, 23, 22.5, 9.5, 1.5, 1.9), 'flesh', { shade: cyl(9, 23, 22.5, 9.5, 1.9) });
    k.decal(dots(jit([12.4, 20.4, 15.4, 17.4, 18.4, 14.4], 0.2, 2)), 'flesh', { tone: GLINT, w: 1 });
    k.solid(poly([21, 9, 28.6, 3.4, 24, 12.4]), 'bone');
    k.solid(poly([22.6, 11.8, 25.8, 12.2, 23.8, 14.2]), 'bone');
  };

  // ── Bolt-Ons, worn ──────────────────────────────────────────────

  // 11 Dent Absorber: sleeveless vest with cracked ceramic plates stitched
  // over scavenged padding.
  I[11] = (k) => {
    const vest = grow(poly([9.6, 2.6, 13, 2.6, 16, 11, 19, 2.6, 22.4, 2.6, 23.4, 7, 27, 11.4, 27, 28,
      5, 28, 5, 11.4, 8.6, 7]), 0.6);
    k.solid(rough(vest, 0.45, 0.6, 1), 'olive', { shade: { t: 'round', r: 3 }, grime: 0.35 });
    k.line(stroke([16, 11.6, 16, 28.4]), { tone: LINE });
    const plate = (x, y, s) => rough(box(x, y, 4.2, 3.5, 0.8), 0.35, 0.9, s);
    k.solid(minus(union(plate(11, 15.8, 1), plate(21, 15.8, 2), plate(11, 23.6, 3), plate(21, 23.6, 4)),
      rough(circle(25.6, 27.4, 2.2), 0.6, 1.2, 8)), 'ceramic', { grime: 0.3 });
    k.line(stroke(jit([18.6, 12.6, 20.4, 15.4, 19.4, 16.6, 22, 19], 0.2, 5)));                   // the cracks
    k.line(stroke(jit([7.2, 21.2, 9.8, 23.4, 8.8, 26.4], 0.2, 6)));
    k.decal(dots(jit([6.4, 13.4, 15.2, 13.4, 16.8, 13.4, 25.6, 13.4, 6.4, 26.6, 15.2, 20, 16.8, 20], 0.25, 7)), 'bone',
      { tone: HI, w: 1, on: 0 });
  };

  // 12 Glow Suit: full yellow hazmat suit, sealed hood, round goggle lenses.
  I[12] = (k) => {
    const suit = union(circle(16, 8, 5), box(16, 17.5, 6, 6.5, 2.2), seg(10.6, 13.2, 6.6, 21.6, 2.3), seg(21.4, 13.2, 25.4, 21.6, 2.3),
      box(12.8, 25.2, 2.7, 3.8, 1), box(19.2, 25.2, 2.7, 3.8, 1));
    k.solid(rough(suit, 0.35, 0.6, 2), 'mustard', { shade: { t: 'round', r: 3.6 }, grime: 0.35 });
    k.line(union(stroke([16, 13, 16, 24]), stroke([12.8, 22.6, 19.2, 22.6])), { tone: SHD });
    k.decal(box(16, 8.2, 3.6, 2.4, 1.2), 'black', { tone: BASE });
    k.decal(union(circle(14.4, 8.2, 1.3), circle(17.6, 8.2, 1.3)), 'glass', { tone: HI });
    k.decal(dots([14, 7.8, 17.2, 7.8]), 'glass', { tone: GLINT, w: 1 });
    k.solid(circle(16, 13.2, 1.8), 'olive');
    k.solid(union(circle(6.4, 22.8, 2), circle(25.6, 22.8, 2)), 'black');
    k.solid(union(box(12.8, 29.2, 3, 1.5, 0.6), box(19.2, 29.2, 3, 1.5, 0.6)), 'black');
  };

  // 13 Wheeze Filter: cloth-wrapped respirator, twin filter canisters.
  I[13] = (k) => {
    k.solid(union(stroke([11, 11.4, 2.6, 8.6]), stroke([21, 11.4, 29.4, 8.6])), 'black', { w: 1.6, shade: { t: 'flat', tone: HI } });
    k.solid(grow(poly([11.4, 7.4, 20.6, 7.4, 23.6, 18, 16, 24.4, 8.4, 18]), 2.2), 'olive', { shade: { t: 'round', r: 3.4 }, grime: 0.3 });
    k.decal(rough(union(stroke([7.6, 11.4, 24.6, 14.6], 1.7), stroke([8.4, 18.4, 23.4, 16], 1.5)), 0.5, 0.8, 4), 'canvas', { dt: 0 });
    k.decal(stroke(jit([9.4, 10.8, 12, 11.6, 15, 12.2], 0.2, 1)), 'canvas', { tone: SHD, w: 1 });
    k.solid(circle(16, 21.2, 2.5), 'iron');
    k.line(union(stroke([14.4, 20.4, 17.6, 20.4]), stroke([14.4, 22, 17.6, 22])), { tone: LINE });
    for (const [x, s] of [[7, 1], [25, -1]]) {
      k.solid(circle(x, 21, 4.5), 'iron', { shade: { t: 'sphere', cx: x - s * 0.8, cy: 20.2, r: 4.8 }, grime: 0.3, rust: 0.2 });
      k.decal(shell(circle(x, 21, 2.7), 0.5), 'steel', { tone: HI });
      k.line(union(stroke([x - 1.4, 21, x + 1.4, 21]), stroke([x, 19.6, x, 22.4])), { tone: LINE });
    }
  };

  // 14 Dark Goggles: surplus goggles, one lens cracked, worn strap.
  I[14] = (k) => {
    const r = (f) => rotate(f, -8);
    k.solid(r(rough(union(box(4, 17.4, 3.4, 2.6, 0.6), box(28, 17.4, 3.4, 2.6, 0.6)), 0.4, 0.9, 1)), 'canvas', { grime: 0.4 });
    k.solid(r(union(circle(9.8, 16.6, 6.4), circle(22.2, 16.6, 6.4), seg(14, 14.6, 18, 14.6, 2))), 'iron', { shade: { t: 'round', r: 2.6 }, grime: 0.3 });
    k.solid(r(circle(9.8, 16.6, 4.3)), 'water', { shade: { t: 'sphere', cx: 8.4, cy: 15, r: 5.4 }, grime: 0, wear: 0.1 });
    k.solid(r(circle(22.2, 16.6, 4.3)), 'water', { shade: { t: 'sphere', cx: 20.8, cy: 15, r: 5.4 }, grime: 0, wear: 0.1 });
    k.line(r(union(stroke([19, 13.4, 22.4, 16.8, 25.4, 15.2]), stroke([22.4, 16.8, 21.2, 20.4]), stroke([22.4, 16.8, 24.8, 19.6]))),
      { tone: GLINT });
    k.decal(r(dots([7.8, 14.6])), 'water', { tone: GLINT, w: 1.4, on: 2 });
  };

  // 15 Trudge Stompers: heavy steel-toed boot, sole patched with duct tape.
  I[15] = (k) => {
    const boot = grow(poly([8, 3.6, 16.6, 3.6, 17, 14.6, 25.6, 17.4, 27, 22.4, 5, 22.4, 5, 18.6]), 1.2);
    k.solid(rough(boot, 0.4, 0.6, 3), 'leather', { shade: { t: 'round', r: 3.4 }, grime: 0.3 });
    k.line(union(stroke([16.8, 14.6, 13.4, 22.4]), stroke([6, 16.6, 13.4, 17.6])), { tone: SHD });
    k.line(union(...[6.2, 8.6, 11, 13.4].map((y) => stroke([15.2, y, 17.8, y + 1.6]))), { mat: 'bone', tone: HI });
    k.solid(inter(boot, ellipse(25, 20, 4.8, 4.4)), 'steel', { rust: 0.2 });
    k.solid(minus(box(16.2, 25.6, 12.4, 2, 0.8), union(...[7.5, 11.5, 15.5, 19.5, 23.5].map((x) => box(x, 27.8, 0.7, 0.8)))), 'black');
    k.solid(rotate(box(21.5, 25, 2.2, 3, 0.2), -12, 21.5, 25), 'tape', { shade: { t: 'bevel', hw: 1, sw: 1 }, grime: 0.2 });
    k.solid(box(8.8, 3.6, 1.8, 1.4, 0.4), 'leather');
  };

  // 16 Hoarder's Rig: a chest harness that is all straps, pouches and
  // carabiners.
  I[16] = (k) => {
    k.solid(union(curve(10.8, 14.6, 9.6, 7, 5.4, 2.4, 1.7, 1.5), curve(21.2, 14.6, 22.4, 7, 26.6, 2.4, 1.7, 1.5)), 'olive', { grime: 0.3 });
    k.solid(box(16, 8.6, 7.6, 1.1, 0.3), 'olive', { grime: 0.3 });
    k.solid(box(16, 8.6, 1.5, 1.5, 0.3), 'steel', { wear: 0 });
    k.solid(box(16, 15.6, 12.6, 2, 0.6), 'olive', { grime: 0.3 });
    k.decal(stroke([4, 15.6, 28, 15.6]), 'olive', { tone: SHD, w: 1, p: 0.6 });
    const pouch = (x, y, hw, hh, m, s) => {
      k.solid(rough(box(x, y, hw, hh, 1), 0.3, 0.9, s), m, { shade: { t: 'round', r: 2 }, grime: 0.35 });
      k.line(stroke([x - hw + 0.6, y - hh + 2.2, x + hw - 0.6, y - hh + 2.2]), { tone: SHD });
      k.decal(dots([x, y - hh + 3.2]), 'iron', { tone: HI, w: 1 });
    };
    pouch(7.6, 20.8, 3, 4, 'canvas', 1);
    pouch(13.8, 21.4, 2.8, 4.6, 'olive', 2);
    pouch(19.6, 20.8, 2.6, 4, 'canvas', 3);
    pouch(24.8, 20.2, 2.4, 3.4, 'tan', 4);
    k.solid(box(16, 15.6, 1.8, 1.6, 0.3), 'steel');
    k.solid(union(shell(box(10.4, 27.4, 1.3, 2.2, 1.2), 0.45), shell(box(22.4, 26.4, 1.3, 2.2, 1.2), 0.45)), 'steel', { wear: 0 });
  };

  // 19 Vertical Regret: a coil of climbing rope, a worn carabiner through it.
  I[19] = (k) => {
    k.solid(rough(stroke([10, 24, 8.6, 27.4, 10.4, 29.4], 1.3, 0.9), 0.3, 1, 1), 'teal', { grime: 0.2 });
    const coil = union(shell(ellipse(15, 17, 10, 8), 2.2), shell(ellipse(16.4, 16.4, 9.2, 7.4), 2.2));
    k.solid(coil, 'teal', { shade: { t: 'round', r: 2 }, grime: 0.25 });
    k.decal(inter(coil, stripes(2.3, 1, -0.9)), null, { dt: -1 });
    k.solid(shell(box(23.8, 9, 3, 4.6, 2.8), 0.9), 'steel', { rust: 0.2, wear: 0.1 });
    k.line(stroke([26.8, 7.2, 26.8, 11]), { tone: LINE });
  };

  // 20 Doom Clicker: civil-defence Geiger counter, dial in the red, probe on
  // a coiled lead, chipped yellow paint.
  I[20] = (k) => {
    k.solid(inter(shell(box(13.6, 11, 6, 3.6, 2.4), 1), box(13.6, 9, 8, 3)), 'black');
    k.solid(stroke([19.4, 24.6, 20.6, 26.4, 19.4, 27.2, 21.2, 28.8, 22.6, 27.6]), 'black', { w: 1, wear: 0 });
    k.solid(seg(22.4, 27.4, 28.4, 21.4, 1.9), 'steel', { shade: cyl(22.4, 27.4, 28.4, 21.4, 1.9) });
    k.solid(box(13.6, 19, 9.6, 7, 1.6), 'mustard', { rust: 0.3, rustMat: 'iron', grime: 0.3 });
    k.solid(circle(10.6, 18.4, 4.2), 'iron');
    k.decal(circle(10.6, 18.4, 3.2), 'bone', { tone: HI });
    k.decal(arc(10.6, 18.8, 2.4, 200, 340), 'bone', { tone: SHD, w: 1 });
    k.decal(arc(10.6, 18.8, 2.4, 300, 340), 'red', { tone: BASE, w: 1 });
    k.decal(stroke([10.6, 19.4, 12.8, 16.4]), 'red', { tone: SHD, w: 1 });
    k.solid(circle(18.6, 15.8, 1.6), 'black');
    k.decal(dots([17, 20.4, 19.4, 20.4, 21.4, 20.4, 18.2, 22.6, 20.4, 22.6]), 'mustard', { tone: LINE, w: 1, on: 3 });
  };

  // 27 Portable Forge: car muffler turned smelter, coals glowing inside,
  // bellows at one end, a stub of anvil on top.
  I[27] = (k) => {
    k.fx(rough(curve(28.6, 15, 30, 10.5, 27, 7.5, 1, 0.3), 0.5, 0.8, 5), 'smoke', { tone: BASE, under: true });
    k.solid(union(box(10.4, 27.2, 1.2, 2), box(21.6, 27.2, 1.2, 2)), 'iron');
    k.solid(seg(26, 18.4, 29, 16.4, 1.4), 'iron');
    k.solid(box(15.6, 20.8, 10.8, 5.8, 4.4), 'rust', { shade: cyl(0, 20.8, 32, 20.8, 5.8), rust: 0.4, rustMat: 'steel', grime: 0.3 });
    k.decal(rough(ellipse(15, 21.4, 4.8, 3), 0.3, 1, 2), 'black', { tone: SHD });
    k.decal(rough(ellipse(15, 22.2, 4, 2), 0.7, 1.1, 3), 'fire', { shade: { t: 'glow', rim: 0.6, core: 1.6 } });
    k.decal(dots(jit([13, 22, 16.4, 21.4, 14.8, 23.4], 0.4, 4)), 'fire', { tone: GLINT, w: 1 });
    k.line(union(stroke([8.4, 15.8, 8.4, 25.8]), stroke([22.8, 15.8, 22.8, 25.8])), { tone: SHD });
    k.solid(grow(poly([10, 9, 22, 9, 20.4, 12, 17.8, 12.4, 17.8, 15.2, 14, 15.2, 14, 12.4, 11.6, 12]), 0.5), 'iron', { rust: 0.15 });
    k.solid(grow(poly([2.2, 17, 5.2, 18.2, 5.2, 23.4, 2.2, 24.6]), 0.8), 'leather', { shade: { t: 'round', r: 2 } });
    k.line(union(stroke([3, 19.6, 4.8, 19.6]), stroke([3, 21.8, 4.8, 21.8])), { tone: SHD });
  };

  // 28 Fishing Pole: telescoping rod bent from scavenged pipe, line, bobber,
  // hook.
  I[28] = (k) => {
    k.solid(stroke([24.4, 4.8, 24.4, 20.4]), 'white', { w: 1, shade: { t: 'flat', tone: SHD }, outline: false, wear: 0 });
    k.solid(stroke([24.4, 20.4, 24.4, 23.4, 23.4, 24.2, 22.2, 23.2, 22.4, 22]), 'steel', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(circle(24.4, 13.6, 1.9), 'white', { shade: { t: 'sphere', cx: 24, cy: 13.2, r: 2 } });
    k.decal(box(24.4, 14.8, 2.2, 1.2), 'red', { dt: 0 });
    k.solid(union(seg(5.4, 26.6, 12.6, 19.4, 1.6), seg(12.6, 19.4, 19.2, 12.8, 1.2), seg(19.2, 12.8, 24.4, 4.8, 0.8, 0.6)), 'iron', { rust: 0.25, wear: 0.1 });
    k.solid(union(circle(12.6, 19.4, 1.8), circle(19.2, 12.8, 1.4)), 'steel', { wear: 0 });
    k.solid(seg(3.4, 28.6, 7.6, 24.4, 2.2), 'cork', { speck: 0.2 });
    k.solid(circle(10, 24.6, 2.8), 'steel', { shade: { t: 'sphere', cx: 9.4, cy: 24, r: 3 } });
    k.solid(seg(10, 24.6, 12.8, 27, 0.7), 'iron', { wear: 0 });
  };

  // 29 Compound Bow: pre-war bow, cams at the tips, string taut, arrow nocked.
  I[29] = (k) => {
    k.solid(stroke([13.4, 4.2, 13.4, 27.8]), 'bone', { w: 1, shade: { t: 'flat', tone: HI }, outline: false, wear: 0 });
    k.solid(union(curve(18.4, 12, 18.6, 6, 14.2, 3.6, 1.3, 1), curve(18.4, 20, 18.6, 26, 14.2, 28.4, 1.3, 1)), 'olive', { grime: 0.3, wear: 0.15 });
    k.solid(union(circle(14, 3.8, 1.9), circle(14, 28.2, 1.9)), 'steel');
    k.solid(stroke([6.2, 16, 28, 16]), 'wood', { w: 1.4, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(poly([27.4, 13.8, 30.6, 16, 27.4, 18.2]), 'steel');
    k.solid(union(poly([6.4, 16, 9.8, 16, 8, 13.2, 4.8, 13.2]), poly([6.4, 16, 9.8, 16, 8, 18.8, 4.8, 18.8])), 'red', { shade: { t: 'flat', tone: BASE } });
    k.solid(box(18.8, 16, 2, 4.8, 1), 'black', { shade: { t: 'round', r: 1.6 } });
  };

  // 32 Fire Starter: ferro rod and striker, crossed, throwing sparks.
  I[32] = (k) => {
    k.solid(union(seg(6.6, 25.4, 19.4, 12.6, 1.5), circle(4.6, 27.4, 2.5)), 'black', { shade: cyl(6.6, 25.4, 19.4, 12.6, 1.6) });
    k.solid(seg(4.2, 27.8, 7.8, 24.2, 2.2), 'olive');
    k.solid(rotate(box(18, 18, 7.6, 1.5, 0.4), 42, 18, 18), 'steel', { rust: 0.2 });
    k.solid(rotate(box(23.6, 23, 2.6, 2.4, 1), 42, 23.6, 23), 'wood');
    k.fx(union(...[[15.6, 14.6, 20.6, 7.4], [16.6, 15, 25.4, 9.4], [15, 15.4, 17, 5.6], [17, 15.8, 27.4, 13.6]].map(([a, b, c, d]) => stroke([a, b, c, d]))),
      'fire', { tone: HI, w: 1, p: 0.7 });
    k.fx(dots(jit([22, 6, 27, 8.4, 18, 3.8, 28.8, 12, 24.6, 4.2], 0.6, 7)), 'fire', { tone: GLINT, w: 1 });
  };

  // 33 Intimidate Mask: carved resin mask, jagged teeth, deep hollow eyes.
  I[33] = (k) => {
    const face = rough(union(ellipse(16, 13.4, 9.4, 9.6), poly([8, 16, 24, 16, 20, 26.4, 16, 29.4, 12, 26.4])), 0.4, 0.6, 2);
    k.solid(face, 'bone', { shade: { t: 'round', r: 4.4 }, grime: 0.35 });
    k.decal(union(stroke([9, 9.2, 12, 20.4], 0.7, 1.1), stroke([23, 9.2, 20, 20.4], 0.7, 1.1)), 'blood', { dt: 0 });
    k.decal(union(poly([9, 11.4, 14.6, 13.2, 13.6, 16.4, 9.6, 15.6]), poly([23, 11.4, 17.4, 13.2, 18.4, 16.4, 22.4, 15.6])), 'black', { tone: LINE });
    k.decal(poly([10.4, 20.4, 21.6, 20.4, 19.6, 25, 12.4, 25]), 'black', { tone: LINE });
    k.decal(union(poly([10.8, 20.2, 12.4, 23.6, 13.8, 20.2]), poly([13.8, 20.2, 15, 23.8, 16.4, 20.2]), poly([16.4, 20.2, 17.6, 24, 19, 20.2]),
      poly([19, 20.2, 20.2, 23.2, 21.2, 20.2]), poly([12.6, 25.2, 13.8, 22.4, 15, 25.2]), poly([15.8, 25.2, 17, 22.6, 18.2, 25.2])), 'white', { tone: HI });
    k.line(stroke(jit([20.6, 4.4, 19.6, 6.8, 20.8, 8.2, 20, 10.4], 0.2, 4)));
  };

  // 34 Bear Skin Cape: a whole bear, worn: the head for a hood, the hide for
  // a cape, its claws at the clasp.
  I[34] = (k) => {
    const cape = rough(poly([11, 9, 21, 9, 27.4, 26, 25, 28.6, 16, 27.4, 7, 28.6, 4.6, 26]), 0.9, 0.7, 1);
    k.solid(cape, 'fur', { shade: { t: 'round', r: 5 }, grime: 0.2, speck: 0.12 });
    k.line(union(curve(10, 16, 9, 21, 8, 26), curve(22, 16, 23, 21, 24, 26), curve(16, 17, 16.4, 22, 15.8, 26.6)), { tone: SHD });
    k.line(union(curve(12.6, 13, 11.8, 17, 11, 20), curve(19.4, 13, 20.2, 17, 21, 20)), { tone: HI });
    k.solid(rough(union(ellipse(16, 7.6, 5.6, 4.4), circle(11.2, 3.6, 1.9), circle(20.8, 3.6, 1.9)), 0.5, 0.9, 2), 'fur', { shade: { t: 'round', r: 3 }, grime: 0.2 });
    k.decal(ellipse(16, 9.4, 2.4, 1.6), 'tan', { dt: 0 });
    k.decal(union(dots([13.4, 6.6, 18.6, 6.6]), dots([16, 8.6])), 'black', { tone: LINE, w: 1.2 });
    k.solid(union(curve(12, 11.2, 13, 13.6, 12.2, 15.6, 0.9, 0.3), curve(14, 11.8, 14.8, 14, 14, 16, 0.9, 0.3),
      curve(20, 11.2, 19, 13.6, 19.8, 15.6, 0.9, 0.3), curve(18, 11.8, 17.2, 14, 18, 16, 0.9, 0.3)), 'bone', { shade: { t: 'flat', tone: HI }, wear: 0 });
  };

  // 40 Shock Knuckles: knuckle-duster wrapped in wire, humming between the
  // studs.
  I[40] = (k) => {
    const rings = union(...[8.4, 13.4, 18.6, 23.6].map((x) => circle(x, 13.6, 3.3)));
    const holes = union(...[8.4, 13.4, 18.6, 23.6].map((x) => circle(x, 13.8, 1.6)));
    k.solid(minus(union(rings, box(16, 19, 9.4, 2.6, 2.4)), holes), 'steel', { shade: { t: 'round', r: 2.2 }, rust: 0.2 });
    k.decal(union(...[9.5, 12, 14.5, 17, 19.5, 22].map((x) => stroke([x, 17, x + 1.6, 21.4]))), 'copper', { tone: HI, w: 1 });
    k.solid(union(...[8.4, 13.4, 18.6, 23.6].map((x) => poly([x - 1.4, 10.6, x, 7.6, x + 1.4, 10.6]))), 'steel', { shade: { t: 'flat', tone: HI } });
    k.solid(rough(box(16, 23.6, 5, 1.8, 0.8), 0.6, 0.9, 5), 'ash', { grime: 0.2, speck: 0.15 });
    k.fx(union(stroke([9.4, 7.4, 10.4, 5.4, 11.4, 7, 12.6, 5.4]), stroke([14.6, 7.2, 15.6, 4.8, 16.6, 6.8, 17.6, 5]),
      stroke([19.8, 7.4, 21, 5.2, 21.8, 7, 22.6, 5.6])), 'blue', { tone: HI, w: 1 });
    k.fx(dots([4.6, 9, 27.4, 9.6, 16, 3]), 'blue', { tone: GLINT, w: 1 });
  };

  // 41 Squatch Sliprs: enormous shaggy slippers, felted from Sasquatch.
  I[41] = (k) => {
    // Seen from above: a pair of huge hairy feet, toes, claws and all, and the
    // ankle holes are what make them slippers.
    const foot = (cx, s, m) => {                             // m = +1 right foot, -1 left
      const toes = [[-3.4, 7.4, 1.5], [-1.2, 5.6, 1.7], [1.2, 5.4, 1.8], [3.4, 6.8, 1.9]].map(([dx, y, r]) => [cx + dx * m, y, r]);
      const body = union(ellipse(cx, 18.6, 4.6, 9.4), ellipse(cx + 0.4 * m, 11.6, 5.2, 4.8), ...toes.map(([x, y, r]) => circle(x, y, r)));
      k.solid(rough(body, 0.8, 0.9, s), 'fur', { shade: { t: 'round', r: 3.6 }, grime: 0.15, speck: 0.15 });
      k.line(union(...toes.map(([x, y, r]) => stroke([x - 0.6, y + r + 0.4, x + 0.2, y + r + 1.8]))), { tone: SHD });
      k.line(union(curve(cx - 2.6, 13, cx - 3.4, 17, cx - 2.8, 21), curve(cx + 2.4, 14, cx + 3.2, 17.6, cx + 2.8, 21.4)), { tone: SHD });
      k.line(union(curve(cx - 1, 11.4, cx - 1.8, 14, cx - 1.2, 16.6), curve(cx + 1.6, 11, cx + 1, 13.6, cx + 1.6, 16)), { tone: HI });
      k.decal(ellipse(cx, 22.8, 3, 3.8), 'tan', { tone: HI });
      k.decal(ellipse(cx, 23.2, 2.1, 2.9), 'black', { tone: BASE });
      k.fx(union(...toes.map(([x, y, r]) => stroke([x, y - r + 0.6, x + 0.2 * m, y - r - 0.8]))), 'bone', { tone: BASE, w: 1 });
    };
    foot(9.4, 1, -1);
    foot(22.6, 2, 1);
  };

  // 42 Knife-Wrench: half wrench, half knife, welded in the middle, taped.
  I[42] = (k) => {
    const jaw = minus(circle(7.4, 24.6, 4.8), rotate(box(5.4, 26.6, 1.9, 5), 45, 5.4, 26.6), circle(7.4, 24.6, 1.4));
    k.solid(union(jaw, seg(9.4, 22.6, 15.4, 16.6, 1.8)), 'steel', { rust: 0.3 });
    k.solid(poly([14.4, 17.8, 17.4, 13.4, 28.6, 3.2, 19.8, 16.4]), 'steel', { shade: { t: 'bevel', hw: 1.2, sw: 1.2 }, grime: 0.15 });
    k.decal(stroke([19.8, 16.2, 28.4, 3.4]), 'steel', { tone: GLINT, w: 1 });
    k.solid(rough(circle(15.6, 16.6, 2.3), 0.6, 1.4, 3), 'rust', { shade: { t: 'round', r: 1.6 }, speck: 0.2 });
    k.solid(rotate(box(11.8, 20.2, 2.4, 2.6, 0.2), 45, 11.8, 20.2), 'tape', { shade: { t: 'bevel', hw: 1, sw: 1 } });
  };

  // 45 Glow Dentures: a full set of pre-war falsies, faintly radioactive blue.
  I[45] = (k) => {
    const G = { r: 5, mat: 'blue', p: 0.45 };
    const upper = minus(ellipse(16, 13.4, 11, 7), ellipse(16, 16.6, 8.2, 5.4), box(16, 22, 14, 5));
    const lower = minus(ellipse(16, 22.6, 11, 6.2), ellipse(16, 19.4, 8.2, 4.6), box(16, 13, 14, 6));
    k.solid(upper, 'gum', { shade: { t: 'round', r: 2.6 }, glow: G, grime: 0.1 });
    k.solid(lower, 'gum', { shade: { t: 'round', r: 2.6 }, glow: G, grime: 0.1 });
    const teeth = (y, h, dir) => union(...[7.6, 10.4, 13.4, 16.6, 19.6, 22.4].map((x) => box(x + (x - 16) * 0.02, y + dir * Math.abs(x - 15) * 0.1, 1.2, h, 0.6)));
    k.solid(teeth(16.6, 1.8, -1), 'white', { shade: { t: 'bevel', hw: 1, sw: 1 }, grime: 0.15 });
    k.solid(teeth(19.8, 1.6, 1), 'white', { shade: { t: 'bevel', hw: 1, sw: 1 }, grime: 0.15 });
    k.fx(dots([10.8, 15.8, 17, 15.8, 20, 20.2]), 'blue', { tone: GLINT, w: 1 });
  };

  // 47 Lead Snuggie: hooded, quilted, lead-lined blanket with sleeves.
  I[47] = (k) => {
    const body = union(ellipse(16, 8.4, 5.6, 5.4), poly([10.4, 10, 21.6, 10, 25.4, 28.4, 6.6, 28.4]),
      seg(11, 13.4, 4, 20.4, 3), seg(21, 13.4, 28, 20.4, 3));
    k.solid(rough(body, 0.4, 0.6, 1), 'lead', { shade: { t: 'round', r: 4 }, grime: 0.25 });
    k.decal(ellipse(16, 9, 3.4, 3.4), 'black', { tone: BASE });
    const q = inter(rough(poly([10.4, 12, 21.6, 12, 25.4, 28.4, 6.6, 28.4]), 0.4, 0.6, 1),
      (x, y) => Math.min(Math.abs((((x + y) % 4.4) + 4.4) % 4.4 - 2.2), Math.abs((((x - y) % 4.4) + 4.4) % 4.4 - 2.2)) - 0.01);
    k.line(q, { tone: SHD, w: 1, p: 0.8 });
    k.solid(circle(16, 17.6, 2.6), 'mustard', { shade: { t: 'flat', tone: HI } });
    k.decal(trefoil(16, 17.6, 2.2), 'black', { tone: BASE });
  };

  // 64 Backpack: bulging patched-canvas rucksack, bedroll under the flap,
  // mismatched straps.
  I[64] = (k) => {
    k.solid(rough(box(16, 18.6, 10, 9.6, 4), 0.45, 0.6, 2), 'canvas', { shade: { t: 'round', r: 5 }, grime: 0.35 });
    k.solid(box(16, 8.2, 12.4, 2.6, 2.4), 'red', { shade: cyl(0, 8.2, 32, 8.2, 2.6), grime: 0.35 });
    k.line(union(stroke([7.4, 6, 7.4, 10.4]), stroke([24.6, 6, 24.6, 10.4])), { tone: SHD });
    k.solid(rough(grow(poly([8, 9, 24, 9, 23, 15.4, 16, 16.8, 9, 15.4]), 1), 0.35, 0.8, 3), 'olive', { shade: { t: 'round', r: 2.4 }, grime: 0.3 });
    k.solid(rough(box(16, 23.4, 5.6, 3.8, 1.4), 0.3, 0.9, 4), 'canvas', { shade: { t: 'round', r: 2 }, grime: 0.3 });
    k.decal(rough(box(19, 24.2, 2, 1.8, 0.2), 0.3, 1, 5), 'tan', { dt: 0 });
    k.decal(shell(box(19, 24.2, 2, 1.8, 0.2), 0.01), 'tan', { tone: SHD, w: 1, p: 0.6 });
    k.solid(box(12, 16.8, 1.1, 7, 0.4), 'tan', { grime: 0.3 });
    k.solid(box(20, 16.8, 1.1, 6.4, 0.4), 'leather', { grime: 0.3 });
    k.solid(union(box(12, 17.4, 1.7, 1.2, 0.3), box(20, 17.4, 1.7, 1.2, 0.3)), 'steel', { wear: 0 });
  };

  // 65 Canteen: three scrap plates hammered round and riveted shut, cap,
  // strap.
  I[65] = (k) => {
    k.solid(curve(7.6, 12, 16, -1.6, 24.4, 12, 1.1), 'canvas', { grime: 0.3 });
    const body = rough(circle(16, 18.6, 9.6), 0.4, 0.7, 2);
    k.solid(body, 'steel', { shade: { t: 'round', r: 5.5 }, grime: 0.3 });
    k.decal(inter(body, poly([16, 18.6, 30, 10, 30, 32, 16, 32])), 'rust', { dt: 0 });
    k.decal(inter(body, poly([16, 18.6, 2, 10, 2, 32, 16, 32])), 'iron', { dt: 0 });
    const seam = union(stroke(jit([16, 9, 16.4, 13.6, 16, 18.6], 0.25, 1)), stroke(jit([16, 18.6, 11.6, 23.6, 8.2, 27], 0.25, 2)),
      stroke(jit([16, 18.6, 20.4, 23.6, 23.8, 27], 0.25, 3)));
    k.line(seam, { tone: LINE, on: 1 });
    k.decal(dots(jit([16.8, 11, 17, 15.6, 13.4, 22.2, 10.6, 25, 18.6, 22.2, 21.4, 25], 0.25, 4)), 'steel', { tone: GLINT, w: 1, on: 1 });
    k.solid(union(box(7.6, 12, 1.6, 1.2, 0.4), box(24.4, 12, 1.6, 1.2, 0.4)), 'iron');
    k.solid(box(16, 8.4, 2.2, 1.6, 0.3), 'steel');
    k.solid(box(16, 6, 3, 1.6, 0.8), 'black');
  };

  // ── Bolt-Ons, vehicles ──────────────────────────────────────────

  // 17 Rust Rocket: a rusted-out motor scooter, patched tank, coughing.
  I[17] = (k) => {
    k.fx(union(rough(circle(3.6, 21.4, 1.8), 0.6, 1, 1), rough(circle(2.6, 17.6, 1.2), 0.5, 1, 2)), 'smoke', { tone: BASE });
    const wheel = (x, y) => {
      k.solid(circle(x, y, 3.8), 'black', { shade: { t: 'round', r: 1.6 } });
      k.decal(circle(x, y, 1.5), 'steel', { tone: HI });
    };
    wheel(8.6, 25.4);
    wheel(24.4, 25.4);
    k.solid(union(ellipse(10.6, 19.8, 7.2, 4.6), box(16, 22.6, 6.2, 1.4, 0.5),
      poly([20, 23.6, 21.6, 11, 24.6, 11, 24.4, 22.6])), 'teal', { shade: { t: 'round', r: 3 }, rust: 0.55, grime: 0.3 });
    k.decal(rough(box(8.6, 19.6, 2.6, 2, 0.3), 0.3, 1, 3), 'steel', { dt: 0 });
    k.decal(dots([6.4, 18, 10.8, 18, 6.4, 21.4, 10.8, 21.4]), 'iron', { tone: HI, w: 1 });
    k.solid(box(10.4, 14, 4.8, 1.3, 1), 'black');
    k.solid(union(seg(23, 11.4, 23, 6.8, 0.9), seg(20.6, 6.6, 26.2, 5.8, 0.9)), 'steel', { wear: 0 });
    k.solid(circle(25.4, 9.6, 1.5), 'bone', { shade: { t: 'flat', tone: HI } });
  };

  // 18 Floaty Disaster: oil drums, mismatched planks, rope, a hopeful flag.
  I[18] = (k) => {
    k.solid(stroke([21.6, 16, 22.6, 3.4]), 'wood', { w: 1.2, shade: { t: 'flat', tone: BASE }, wear: 0 });
    k.solid(rough(poly([22.4, 3.6, 29.2, 5, 27.6, 7.4, 29, 9.4, 22.8, 9.4]), 0.4, 1, 4), 'bone', { shade: { t: 'flat', tone: BASE }, grime: 0.5 });
    const drum = (x, m, s) => {
      k.solid(circle(x, 22.8, 4.2), m, { shade: { t: 'sphere', cx: x - 1, cy: 21.6, r: 4.8 }, rust: 0.45, grime: 0.3 });
      k.line(shell(circle(x, 22.8, 2.6), 0), { tone: SHD });
      k.decal(dots([x + 0.8, 21.6]), m, { tone: LINE, w: 1.4 });
      return s;
    };
    drum(7.6, 'water', 1);
    drum(16, 'red', 2);
    drum(24.4, 'mustard', 3);
    k.solid(rotate(box(15, 16.8, 12, 1.5, 0.3), -3, 15, 16.8), 'wood', { grime: 0.3 });
    k.solid(rotate(box(18.2, 14.2, 10.4, 1.3, 0.3), 4, 18.2, 14.2), 'cork', { grime: 0.3 });
    k.solid(union(stroke([10.6, 13.2, 12.2, 20]), stroke([20.2, 12.6, 21.6, 19.8])), 'tan', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
  };

  // 26 Motorbike: beat-up chopper, raked fork, welded from spares, smoking.
  I[26] = (k) => {
    k.fx(union(rough(circle(3.2, 18.6, 1.9), 0.6, 1, 1), rough(circle(2.4, 14.4, 1.2), 0.5, 1, 2)), 'smoke', { tone: BASE });
    const wheel = (x, y, r) => {
      k.solid(minus(circle(x, y, r), circle(x, y, r - 1.6)), 'black', { shade: { t: 'round', r: 1.2 } });
      k.solid(union(circle(x, y, 1.1), stroke([x - r + 1.4, y, x + r - 1.4, y]), stroke([x, y - r + 1.4, x, y + r - 1.4])), 'steel',
        { w: 0.9, outline: false, wear: 0, shade: { t: 'flat', tone: SHD } });
    };
    wheel(7.8, 23.6, 5);
    wheel(25.8, 24.4, 4.2);
    k.solid(stroke([20.2, 8.4, 25.8, 24.4]), 'steel', { w: 1.4, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(stroke([7.8, 23.6, 11, 15.6, 20.4, 11, 18.6, 20.4, 11.4, 21.2, 7.8, 23.6]), 'iron', { w: 1.3, wear: 0 });
    k.solid(box(15.2, 19.2, 3.4, 2.6, 0.8), 'steel', { rust: 0.3 });
    k.line(union(stroke([12.6, 18.4, 17.8, 18.4]), stroke([12.6, 20, 17.8, 20])), { tone: SHD });
    k.solid(seg(11.6, 21.4, 3.4, 20.6, 0.9), 'steel');
    k.solid(rough(ellipse(16.4, 12.4, 4.8, 2.6), 0.3, 0.9, 3), 'red', { shade: { t: 'round', r: 2.4 }, rust: 0.3 });
    k.decal(rough(box(17.6, 12, 1.8, 1.4), 0.3, 1, 4), 'steel', { dt: 0 });
    k.solid(ellipse(9.8, 14.4, 3.2, 1.4), 'leather');
    k.solid(stroke([20.4, 9, 19.6, 4.4, 17.2, 4]), 'steel', { w: 1.2, wear: 0 });
    k.solid(circle(22.2, 10.8, 1.3), 'bone', { shade: { t: 'flat', tone: HI } });
  };

  // 63 Raft: driftwood logs cinched with knotted salvaged cord. Cruder and
  // smaller than the Floaty Disaster on purpose.
  I[63] = (k) => {
    const r = (f) => rotate(f, -16);
    const logs = [[7.6, 7.4, 25.4, 2.1], [12, 6, 26.8, 2.3], [16.4, 8, 25.8, 2], [20.8, 6.6, 27.2, 2.3], [25, 8.8, 24.8, 2]];
    logs.forEach(([x, y0, y1, w], i) => {
      k.solid(r(seg(x, y0, x + (i % 2 ? 0.4 : -0.4), y1, w)), 'wood', { shade: rcyl(-16, x, 0, x, 32, w), grime: 0.3, wear: 0.4 });
      k.decal(r(dots(jit([x, y0 + 5, x, y1 - 4], 0.5, i))), 'wood', { tone: LINE, w: 1 });
    });
    k.solid(r(union(stroke(jit([5, 11.6, 11, 12.4, 17, 11.4, 23, 12.4, 27.4, 11.8], 0.3, 1)),
      stroke(jit([5, 21.4, 11, 22.2, 17, 21.2, 23, 22.4, 27.4, 21.6], 0.3, 2)))), 'tan', { w: 1.2, shade: { t: 'flat', tone: HI }, wear: 0 });
    k.solid(r(union(circle(4.6, 11.8, 1.3), circle(27.8, 21.4, 1.3))), 'tan');
  };

  // ── Salvage ─────────────────────────────────────────────────────

  // 21 Useful Garbage: bent plate, a length of rusty pipe, nails, wire.
  I[21] = (k) => {
    k.solid(rough(union(ellipse(16, 24, 12, 4.6), ellipse(13, 20.6, 7, 4.4)), 0.9, 0.7, 1), 'iron', { shade: { t: 'round', r: 3 }, grime: 0.3, rust: 0.3 });
    k.solid(seg(8, 20, 25.6, 9.2, 2), 'rust', { shade: cyl(8, 20, 25.6, 9.2, 2), rust: 0 });
    k.solid(ellipse(25.8, 9.1, 1.4, 2), 'black', { outline: false });
    k.solid(rough(poly([13, 15.4, 21.4, 13, 24.4, 20.6, 17, 23.4]), 0.4, 0.8, 2), 'steel', { rust: 0.35 });
    k.line(stroke([16.6, 16.4, 19.6, 21]), { tone: SHD });
    const nail = (ax, ay, bx, by) => {
      k.solid(stroke([ax, ay, bx, by]), 'steel', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
      k.solid(circle(bx, by, 1.1), 'steel', { wear: 0 });
    };
    nail(9, 23, 6.4, 15.4);
    nail(22, 22, 27, 17.6);
    k.solid(curve(4.4, 26, 10, 18, 14, 26.4), 'copper', { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
  };

  // 22 Sparky Bits: a broken circuit board, a tangle of copper, a battery.
  I[22] = (k) => {
    const board = rough(minus(rotate(box(13.6, 13.6, 9, 7.4, 0.5), -14, 13.6, 13.6), poly([18, 2, 26, 2, 26, 12])), 0.5, 1.1, 1);
    k.solid(board, 'pcb', { shade: { t: 'bevel', hw: 1, sw: 1.4 }, grime: 0.2 });
    k.decal(union(stroke([6.4, 12.6, 11.6, 11.4, 12.8, 14.8, 20, 13.4]), stroke([8.2, 18.6, 13.6, 17.4, 14.4, 20.4]), stroke([11.4, 9, 13, 11.8])),
      'copper', { tone: HI, w: 1 });
    k.solid(union(rotate(box(15.4, 9.8, 2.2, 1.5, 0.2), -14, 15.4, 9.8), rotate(box(9.6, 15.4, 1.5, 1.2, 0.2), -14, 9.6, 15.4)), 'black');
    k.solid(box(23.4, 22.4, 3.6, 6, 1), 'mustard', { shade: cyl(23.4, 0, 23.4, 32, 3.6), grime: 0.3 });
    k.decal(box(23.4, 25, 3.8, 3.2), 'black', { dt: 0 });
    k.solid(box(23.4, 15.6, 1.4, 0.9, 0.2), 'steel');
    k.solid(union(curve(20, 20, 10, 20, 7, 27.6), curve(7, 27.6, 14.6, 30, 19.8, 25.4), curve(11, 24, 15, 21, 16.4, 27)), 'copper',
      { w: 1, shade: { t: 'flat', tone: HI }, wear: 0 });
  };

  // 23 Burn Juice Can: dented jerry can, X-pressed sides, fuel sticker,
  // rust running down it.
  I[23] = (k) => {
    k.solid(union(seg(11, 5.4, 19, 5.4, 1.3), seg(11, 5.4, 10, 8.6, 1.1), seg(15, 5.4, 15, 8.6, 1.1), seg(19, 5.4, 19.4, 8.6, 1.1)), 'red', { wear: 0.1 });
    k.solid(seg(23.4, 8.4, 26.4, 5, 1.6), 'steel');
    const can = minus(box(15.6, 18, 9.6, 10, 1.6), rough(circle(26, 25, 2.2), 0.5, 1, 1));
    k.solid(can, 'red', { shade: { t: 'bevel', hw: 1.2, sw: 1.6 }, rust: 0.4, grime: 0.35 });
    k.decal(box(15.6, 18.6, 7, 7.4, 0.5), null, { dt: -1 });
    k.line(union(stroke([9, 11.6, 22.2, 25.6]), stroke([22.2, 11.6, 9, 25.6])), { tone: HI });
    k.line(union(stroke([9.8, 11.6, 22.6, 25]), stroke([21.4, 11.6, 8.6, 24.8])), { tone: LINE, p: 0.7 });
    k.decal(drop(11, 13.4, 1.4, -90), 'mustard', { tone: HI });
    k.decal(union(stroke([19.4, 9, 19.6, 13.4], 0.6, 0.2), stroke([7.4, 20, 7.6, 26], 0.6, 0.2)), 'rust', { dt: 0 });
  };

  // 24 Expired Meds: orange pill bottle, faded label, pills spilled.
  I[24] = (k) => {
    k.solid(box(13, 18.6, 6.6, 8.6, 1.2), 'orange', { shade: cyl(13, 0, 13, 32, 6.6), grime: 0.2 });
    k.decal(rough(box(13, 19.6, 6.8, 4.2, 0.2), 0.35, 0.8, 1), 'bone', { dt: 0, p: 0.92 });
    k.line(union(stroke([8.6, 18.4, 16, 18.4]), stroke([8.6, 20.6, 14, 20.6])), { mat: 'bone', tone: SHD });
    k.decal(stroke([8, 11.6, 8, 26]), 'orange', { tone: GLINT, w: 1 });
    k.solid(box(13, 8.8, 7.4, 2.2, 0.8), 'white', { grime: 0.2 });
    k.line(union(...[8, 10.5, 13, 15.5, 18].map((x) => stroke([x, 7.4, x, 10.2]))), { tone: SHD });
    k.solid(rotate(union(circle(22.4, 25.6, 1.6), circle(25.6, 25.6, 1.6), box(24, 25.6, 1.6, 1.6)), -30, 24, 25.6), 'white', { shade: 'bevel' });
    k.decal(rotate(box(25.6, 25.6, 1.8, 1.8), -30, 24, 25.6), 'red', { dt: 0 });
    k.solid(union(circle(22, 21, 1.5), circle(27.4, 20.6, 1.4)), 'white');
  };

  // 35 Loose Tentacle: severed, curling, suckers down one side, still wet.
  I[35] = (k) => {
    const path = [6.4, 25, 11, 27.4, 16.6, 26.4, 21.4, 22.6, 24.4, 17, 24.4, 11.4, 21.6, 7.6, 18, 7.4, 16.6, 10.4, 18.6, 12.4];
    k.solid(stroke(path, 3.6, 0.6), 'flesh', { shade: { t: 'round', r: 2.6 }, grime: 0.15, wear: 0.2 });
    k.decal(dots(jit([10.4, 23.8, 14.6, 23.4, 18.4, 21, 20.8, 17.4, 21.4, 13.4, 19.8, 10.6], 0.3, 1)), 'jelly', { tone: HI, w: 2 });
    k.decal(dots(jit([10.4, 23.8, 14.6, 23.4, 18.4, 21, 20.8, 17.4, 21.4, 13.4, 19.8, 10.6], 0.3, 1)), 'flesh', { tone: LINE, w: 1 });
    k.decal(dots([9.6, 26.4, 15.4, 27.8, 23.6, 20]), 'flesh', { tone: GLINT, w: 1 });
    k.solid(ellipse(5.8, 24.4, 1.8, 3.4), 'blood', { shade: { t: 'flat', tone: HI } });
    k.decal(ellipse(5.8, 24.4, 0.9, 2), 'blood', { tone: SHD });
  };

  // 36 Clean Underwear: white briefs, oddly pristine, with a sparkle. The
  // only thing in the wasteland with no grime on it.
  I[36] = (k) => {
    const briefs = grow(poly([5.6, 8.6, 26.4, 8.6, 26, 14.4, 20.6, 17.4, 18.6, 24.4, 13.4, 24.4, 11.4, 17.4, 6, 14.4]), 1);
    k.solid(briefs, 'white', { shade: { t: 'round', r: 3 }, grime: 0, speck: 0, wear: 0.15 });
    k.decal(box(16, 9.4, 11.4, 1.6), 'white', { tone: HI });
    k.line(stroke([4.8, 11.2, 27.2, 11.2]), { tone: SHD, p: 0.7 });
    k.line(union(curve(11.6, 17.6, 15.8, 16.6, 16, 24), curve(20.4, 17.6, 17.2, 17, 16.2, 22.6)), { tone: SHD });
    k.fx(union(stroke([26.4, 17.6, 26.4, 23.4]), stroke([23.6, 20.5, 29.2, 20.5])), 'white', { tone: GLINT, w: 1 });
    k.fx(dots([5.4, 20.6, 28.4, 4.2]), 'white', { tone: GLINT, w: 1 });
  };

  // 39 Irradiated Fur: a matted tuft, scorched at the tips, glowing faintly.
  I[39] = (k) => {
    // Clumps, not a blob: each lock is its own tapering stroke so the dark
    // line between them keeps them apart. Scorched at the tips, glowing.
    const G = { r: 4.6, mat: 'rad', p: 0.4 };
    const locks = [[6.6, 9.4, 9.6], [11.4, 5, 12.8], [17.4, 3.6, 15.8], [22.6, 5.6, 18.6], [26.4, 10.6, 21.4]];
    k.solid(rough(ellipse(16, 24.4, 9, 4.4), 0.8, 0.8, 1), 'fur', { shade: { t: 'round', r: 3 }, grime: 0.2, speck: 0.15, glow: G });
    locks.forEach(([x, y, bx], i) => {
      const lock = rough(curve(bx, 25, (bx + x) / 2 + (x < 16 ? 1.6 : -1.6), 15, x, y, 2.8, 0.5), 0.4, 1, i + 3);
      k.solid(lock, 'fur', { shade: { t: 'round', r: 2.2 }, grime: 0.15, speck: 0.12, glow: G });
      k.decal(circle(x, y, 2.6), 'rad', { tone: HI });
      k.decal(circle(x, y, 1.2), 'black', { tone: HI });
    });
    k.decal(dots(jit([12, 22.6, 18.8, 23.4, 15.4, 26], 0.5, 2)), 'rad', { tone: BASE, w: 1, on: 0 });
  };

  // 46 Sonic Spines: a clump of barbed cactus spines, humming.
  I[46] = (k) => {
    k.fx(union(rough(arc(16, 22, 12.4, 200, 240), 0.3, 1, 1), rough(arc(16, 22, 14.6, 205, 235), 0.3, 1, 2),
      rough(arc(16, 22, 12.4, 300, 340), 0.3, 1, 3), rough(arc(16, 22, 14.6, 305, 335), 0.3, 1, 4)), 'amber', { tone: HI, w: 1, p: 0.85 });
    const tips = [[6, 11], [10, 5.6], [16, 3.4], [22, 5.6], [26, 11]];
    tips.forEach(([x, y], i) => {
      k.solid(stroke([16 + (i - 2) * 1.2, 23, x, y], 1.2, 0.3), 'bone', { shade: { t: 'flat', tone: HI }, wear: 0 });
      const mx = 16 + (x - 16) * 0.55, my = 23 + (y - 23) * 0.55;
      k.solid(stroke([mx, my, mx + (x < 16 ? -1.6 : 1.6), my + 1.4]), 'bone', { w: 1, shade: { t: 'flat', tone: SHD }, wear: 0, outline: false });
    });
    k.solid(rough(ellipse(16, 25, 7, 4), 0.6, 0.8, 5), 'olive', { shade: { t: 'round', r: 3 }, grime: 0.3 });
    k.decal(dots(jit([12, 24, 15.4, 22.6, 19, 24.2, 14, 27, 18, 27.4], 0.4, 6)), 'bone', { tone: HI, w: 1 });
  };

  // 48 Corrosive Syrup: a bottle of thick neon-green syrup, one drip eating
  // a hole in the scrap it landed on.
  I[48] = (k) => {
    k.fx(union(rough(curve(24.6, 23, 26.6, 19, 24.8, 15.6, 0.9, 0.2), 0.4, 0.9, 1)), 'smoke', { tone: HI, under: true });
    k.solid(rough(poly([17, 27.4, 29.4, 26, 30, 29.4, 16, 30]), 0.3, 1, 2), 'steel', { rust: 0.4 });
    k.decal(rough(ellipse(24.4, 27.8, 3, 1.1), 0.4, 1.2, 3), 'toxic', { tone: BASE });
    k.decal(ellipse(24.6, 28, 1.2, 0.6), 'black', { tone: LINE });
    k.solid(union(box(12.4, 19.4, 6.8, 8, 2.4), box(12.4, 9, 2.4, 3.4)), 'glass', { shade: cyl(12.4, 0, 12.4, 32, 6.8), grime: 0.15 });
    k.decal(box(12.4, 21, 5.8, 6, 1.8), 'toxic', { shade: { t: 'glow', rim: 1, core: 3 } });
    k.decal(stroke([7.2, 14, 7.2, 24.6]), 'glass', { tone: GLINT, w: 1 });
    k.solid(box(12.4, 5.4, 3.2, 1.4, 0.6), 'red', { grime: 0.3 });
    k.solid(union(stroke([14.8, 6.8, 20.6, 10.4, 21.6, 16, 23.8, 21.4], 1, 0.7), drop(24.4, 24.2, 1.2, -110)), 'toxic',
      { shade: { t: 'glow', rim: 0.6, core: 9 }, outline: true });
    k.fx(dots(jit([22, 26.4, 27, 25.6, 25.6, 23.4], 0.3, 4)), 'toxic', { tone: GLINT, w: 1 });
  };

  // 49 Crater Deed: official deed, wax seal and ribbon, edges singed.
  I[49] = (k) => {
    const paper = rough(minus(rotate(box(15, 15.6, 10, 12, 0.5), -6, 15, 15.6), rough(circle(6, 27, 2.6), 0.8, 1, 2)), 0.5, 0.9, 1);
    k.solid(paper, 'bone', { shade: { t: 'bevel', hw: 1, sw: 1.2 }, grime: 0.35 });
    k.decal((x, y) => (-paper(x, y) - 1.5 + (noise2(x * 0.7, y * 0.7, 5) - 0.5) * 2.4), 'leather', { dt: 0, on: 0 });
    k.decal((x, y) => (-paper(x, y) - 0.7 + (noise2(x * 0.9, y * 0.9, 6) - 0.5) * 1.2), 'black', { tone: SHD, on: 0 });
    k.line(rotate(stroke([9.4, 7.4, 19.6, 7.4]), -6, 15, 15.6), { mat: 'blood', tone: SHD, w: 1.6 });
    k.line(rotate(union(...[11.4, 14, 16.6, 19.2].map((y, i) => stroke(jit([8.6, y, 21 - i * 1.6, y], 0.3, i)))), -6, 15, 15.6), { tone: SHD, p: 0.85 });
    k.solid(union(poly([19.4, 24, 18, 30, 20.4, 28.6, 21.6, 25]), poly([23.6, 24, 25.6, 30.2, 22.8, 28.6, 22, 25])), 'red', { shade: { t: 'flat', tone: SHD } });
    k.solid(rough(circle(21.8, 23, 3.6), 0.6, 1, 3), 'red', { shade: { t: 'round', r: 2.4 }, grime: 0.1 });
    k.decal(rough(shell(circle(21.8, 23, 1.8), 0), 0.2, 1, 4), 'red', { tone: SHD, w: 1 });
  };

  // ── Relics ──────────────────────────────────────────────────────

  // 25 Doomed Diary: ash-stained leather journal tied shut with frayed
  // string, the days scratched into the cover.
  I[25] = (k) => {
    const r = (f) => rotate(f, -8);
    k.solid(r(box(17.6, 16.6, 9.6, 11.8, 1.2)), 'bone', { shade: { t: 'flat', tone: BASE }, grime: 0.3 });
    k.line(r(union(stroke([26.6, 7.4, 26.6, 26]), stroke([25.4, 5.6, 25.4, 27.8]))), { tone: SHD });
    k.solid(r(box(15.6, 16.6, 9.6, 12.2, 1.6)), 'leather', { shade: { t: 'round', r: 3 }, grime: 0.3, speck: 0.08 });
    k.decal(r(rough(circle(19, 10.4, 3.2), 1.2, 0.6, 2)), 'ash', { dt: -1 });
    k.decal(r(rough(circle(11.2, 23.6, 2.4), 1, 0.7, 3)), 'ash', { dt: -1 });
    k.line(r(union(...[10, 11.8, 13.6, 15.4].map((x) => stroke([x, 12.8, x - 0.4, 17.4])), stroke([8.6, 16.6, 16.8, 13.4]))), { tone: HI });
    k.solid(r(box(15.6, 20.4, 10.4, 0.7)), 'tan', { shade: { t: 'flat', tone: HI }, wear: 0.1 });
    k.solid(r(union(stroke([21.4, 20.8, 23.6, 27.6], 0.6, 0.3), stroke([22, 20.8, 26.4, 25.6], 0.6, 0.3), circle(21.6, 20.6, 1.3))), 'tan',
      { shade: { t: 'flat', tone: BASE }, wear: 0.2 });
  };

  // 37 Cursed Device: a small black obelisk crawling with symbols that
  // should not exist yet.
  I[37] = (k) => {
    const obelisk = poly([10.6, 28.4, 21.4, 28.4, 19.4, 8.4, 16, 2.6, 12.6, 8.4]);
    k.solid(obelisk, 'iron', { shade: { t: 'bevel', hw: 1.2, sw: 1.6 }, grime: 0.15, glow: { r: 4.6, mat: 'toxic', p: 0.4 } });
    k.decal(poly([16, 2.6, 19.4, 8.4, 21.4, 28.4, 16, 28.4]), 'black', { tone: HI });
    k.line(stroke([16, 3, 16, 28]), { tone: LINE });
    // Runes: a few strokes each, never twice the same, never quite a letter.
    const rune = (x, y, s) => stroke(jit([x - 1, y - 1.4, x - 1, y + 1.4, x + 1, y + 0.2], 0.3, s));
    k.decal(union(rune(14, 10.6, 1), rune(14.2, 16.4, 2), rune(13.6, 22.2, 3), stroke([17.8, 12.6, 18.4, 14.8, 17.6, 15.6]),
      stroke([18, 19.4, 19, 21]), dots([18.6, 25.2])), 'toxic', { tone: GLINT, w: 1 });
    k.fx(dots(jit([7.6, 12, 25, 16, 8.4, 22, 23.6, 7.4], 0.8, 5)), 'toxic', { tone: HI, w: 1 });
  };

  // 38 Pre-War Net Map: cracked data slate, its screen an amber map grid.
  I[38] = (k) => {
    const r = (f) => rotate(f, 6);
    k.solid(r(box(16, 16.4, 12, 9.6, 2)), 'iron', { shade: { t: 'bevel', hw: 1, sw: 1.6 }, grime: 0.3 });
    k.decal(r(box(15.4, 16.4, 9.4, 7.6, 0.6)), 'amber', { tone: SHD });
    const grid = (x, y) => Math.min(Math.abs(((x - 6) % 3.4 + 3.4) % 3.4 - 1.7), Math.abs(((y - 8.8) % 3.2 + 3.2) % 3.2 - 1.6)) - 1.2;
    k.decal(r(inter(box(15.4, 16.4, 9.4, 7.6), (x, y) => -grid(x, y) - 0.9)), 'amber', { tone: BASE, p: 0.8 });
    k.decal(r(rough(ellipse(13, 15, 5, 3.6), 1.2, 0.5, 3)), 'amber', { tone: HI });
    k.decal(r(rough(ellipse(20.4, 20, 2.2, 1.6), 0.6, 0.8, 4)), 'amber', { tone: HI });
    k.decal(r(dots([18.6, 13.6])), 'amber', { tone: GLINT, w: 2 });
    k.line(r(stroke(jit([23, 9.4, 19.4, 13.6, 20.6, 16, 16.6, 19.6, 17.6, 22, 14.6, 24], 0.2, 5))), { mat: 'white', tone: HI });
    k.solid(r(circle(26.2, 16.4, 0.9)), 'black', { outline: false });
  };

  // 43 Valid License: laminated ID, a blank head in the photo, an official
  // stamp over the corner.
  I[43] = (k) => {
    k.solid(box(16, 17.4, 12.6, 8.4, 1.6), 'white', { shade: { t: 'bevel', hw: 1, sw: 1.4 }, grime: 0.25 });
    k.decal(box(16, 10.8, 12.6, 1.8), 'water', { dt: 0 });
    k.decal(box(9.6, 18.6, 4, 4.6, 0.4), 'ash', { tone: HI });
    k.decal(union(circle(9.6, 17.4, 1.9), ellipse(9.6, 22.6, 3.4, 2.4)), 'ash', { tone: SHD });
    k.line(union(stroke([15.6, 15.6, 25.6, 15.6]), stroke([15.6, 18.4, 23.4, 18.4]), stroke([15.6, 21.2, 24.6, 21.2])), { tone: SHD });
    k.decal(stroke([6, 25.4, 20, 9.6]), 'white', { tone: GLINT, w: 1 });
    k.decal(rough(shell(circle(13.8, 22.6, 3.4), 0.55), 0.35, 1, 3), 'red', { tone: BASE, p: 0.85 });
    k.decal(dots([13.8, 22.6]), 'red', { tone: BASE, w: 2 });
    k.solid(box(16, 7.2, 1.8, 1.2, 0.4), 'black', { outline: false });
    k.solid(inter(shell(box(16, 4.8, 2.6, 2.6, 1.8), 0.6), box(16, 3.6, 4, 2.4)), 'steel', { wear: 0 });
  };

  // 51 Sticky Note: a curled yellow post-it. In careful ballpoint:
  // admin / admin. The most powerful document in the wasteland.
  I[51] = (k, n) => {
    const r = (f) => rotate(f, 7);
    k.solid(r(minus(box(16, 16, 11, 11, 0.4), poly([30, 18, 30, 30, 18, 30]))), 'note', { shade: { t: 'bevel', hw: 1, sw: 1.4 }, grime: 0.2 });
    k.decal(r(box(16, 6.4, 11, 1.6)), null, { dt: -1 });
    if (n >= 30) {
      // "admin" twice, in 3 px letters: a d m i n
      const word = (x, y) => union(
        stroke([x + 2, y + 1, x, y + 1, x, y + 3, x + 2, y + 3, x + 2, y + 1.6]),
        stroke([x + 6, y - 1.4, x + 6, y + 3, x + 4, y + 3, x + 4, y + 1, x + 6, y + 1]),
        stroke([x + 8, y + 3, x + 8, y + 1, x + 10, y + 1, x + 10, y + 3]), stroke([x + 9, y + 1, x + 9, y + 3]),
        stroke([x + 12, y + 1, x + 12, y + 3]), dots([x + 12, y - 0.8]),
        stroke([x + 14, y + 3, x + 14, y + 1, x + 16, y + 1, x + 16, y + 3]));
      k.line(r(union(word(7.5, 10.5), word(8.5, 17.5))), { mat: 'blue', tone: SHD });
    } else {
      k.line(r(union(stroke(jit([7.5, 12, 11, 11.4, 14, 12.4, 17, 11.4, 21, 12.2, 23.5, 11.6], 0.3, 1)),
        stroke(jit([8.5, 19, 12, 18.4, 15, 19.4, 18, 18.4, 22, 19.2, 24.5, 18.6], 0.3, 2)))), { mat: 'blue', tone: SHD });
    }
    k.solid(r(poly([18, 27, 27, 18, 25.6, 26])), 'note', { shade: { t: 'flat', tone: HI }, grime: 0 });
  };

  // ── Category glyphs (items without a drawing of their own) ──────
  I.cat0 = (k) => {                                        // Gulpable: flask
    k.solid(union(circle(16, 20, 7.5), box(16, 10, 2.6, 4)), 'glass', { shade: { t: 'sphere', cx: 16, cy: 20, r: 7.5 } });
    k.decal(minus(circle(16, 20, 6.2), box(16, 15, 8, 3)), 'amber', { shade: { t: 'glow', core: 3 } });
    k.solid(box(16, 5.6, 3.2, 1.6, 0.6), 'cork');
  };
  I.cat1 = (k) => {                                        // Bolt-On: wrench
    const w = rotate(union(seg(16, 9, 16, 26, 2.2), minus(circle(16, 7, 5.2), box(16, 3.4, 2, 3.4))), 40);
    k.solid(w, 'steel', { rust: 0.3 });
  };
  I.cat2 = (k) => {                                        // Salvage: gear
    const teeth = (x, y) => { const a = Math.atan2(y - 16, x - 16); return Math.hypot(x - 16, y - 16) - (9.5 + (Math.cos(a * 8) > 0.2 ? 2.4 : 0)); };
    k.solid(minus(teeth, circle(16, 16, 3.4)), 'rust', { shade: 'bevel' });
  };
  I.cat3 = (k) => {                                        // Relic: key
    k.solid(union(minus(circle(10, 12, 5.5), circle(10, 12, 2.4)), seg(14, 16, 25, 27, 1.8), seg(21, 23, 24, 20, 1.5), seg(23.6, 25.6, 26.6, 22.6, 1.5)), 'amber', { rust: 0.2 });
  };
  I.skull = (k) => {
    k.solid(union(circle(16, 13.5, 9), box(16, 21, 5.6, 4, 1.5)), 'bone', { shade: { t: 'round', r: 5 } });
    k.decal(union(rough(circle(12.4, 14, 2.4), 0.4, 1, 1), rough(circle(19.6, 14, 2.4), 0.4, 1, 2)), 'black', { tone: SHD });
    k.line(union(stroke([13.5, 22.5, 13.5, 25]), stroke([16, 22.5, 16, 25]), stroke([18.5, 22.5, 18.5, 25])));
  };

  // ── 6. Output ───────────────────────────────────────────────────
  function rgba(id, n) {
    const px = rasterize(id, n | 0);
    return new Uint8ClampedArray(px.buffer);
  }

  // The PNG is written here, not by a canvas. canvas.toDataURL costs ~5 ms an
  // icon (a GPU readback, then a real zlib pass) -- ten times what drawing
  // it took. An icon is a few dozen colours in long runs, so an indexed PNG
  // whose deflate stream only knows "repeat the last byte" is enough, is
  // built in a fraction of a millisecond, and comes out smaller.
  const CRC = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    CRC[i] = c >>> 0;
  }
  const LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];

  // zlib stream, one fixed-Huffman block, literals plus distance-1 runs.
  function deflateRuns(src, out) {
    let acc = 0, nb = 0;
    const bits = (v, n) => { acc |= v << nb; nb += n; while (nb >= 8) { out.push(acc & 255); acc >>>= 8; nb -= 8; } };
    const code = (c, n) => { let r = 0; for (let i = 0; i < n; i++) { r = (r << 1) | (c & 1); c >>= 1; } bits(r, n); };
    const lit = (v) => (v < 144 ? code(0x30 + v, 8) : code(0x190 + v - 144, 9));
    out.push(0x78, 0x01);
    bits(1, 1); bits(1, 2);                                  // last block, fixed codes
    for (let i = 0; i < src.length;) {
      const v = src[i++];
      lit(v);
      let r = 0;
      while (r < 258 && i + r < src.length && src[i + r] === v) r++;
      if (r >= 3) {                                          // <length r, distance 1>
        let li = 28;
        while (LBASE[li] > r) li--;
        const c = 257 + li;
        if (c < 280) code(c - 256, 7); else code(0xC0 + c - 280, 8);
        if (LEXT[li]) bits(r - LBASE[li], LEXT[li]);
        code(0, 5);
        i += r;
      }
    }
    code(0, 7);                                              // end of block
    if (nb > 0) out.push(acc & 255);
    let a = 1, b = 0;
    for (let i = 0; i < src.length; i++) { a = (a + src[i]) % 65521; b = (b + a) % 65521; }
    out.push(b >>> 8, b & 255, a >>> 8, a & 255);
  }
  function chunk(out, type, data) {
    const n = data.length, at = out.length;
    out.push(n >>> 24, (n >>> 16) & 255, (n >>> 8) & 255, n & 255);
    for (let i = 0; i < 4; i++) out.push(type.charCodeAt(i));
    for (let i = 0; i < n; i++) out.push(data[i]);
    let c = 0xFFFFFFFF;
    for (let i = at + 4; i < out.length; i++) c = CRC[(c ^ out[i]) & 255] ^ (c >>> 8);
    c = (c ^ 0xFFFFFFFF) >>> 0;
    out.push(c >>> 24, (c >>> 16) & 255, (c >>> 8) & 255, c & 255);
  }
  // px: n x n packed pixels; s: whole-number scale. Returns the PNG's bytes,
  // or null if the icon somehow has more than 256 colours.
  function png(px, n, s) {
    const pal = new Map(), idx = new Uint8Array(n * n);
    for (let p = 0; p < n * n; p++) {
      const v = px[p] >>> 24 ? px[p] : 0;                    // one entry for all of transparent
      let k = pal.get(v);
      if (k === undefined) { k = pal.size; if (k > 255) return null; pal.set(v, k); }
      idx[p] = k;
    }
    const N = n * s, raw = new Uint8Array(N * (N + 1));
    for (let y = 0; y < N; y++) {
      const o = y * (N + 1);
      if (y % s) { raw[o] = 2; continue; }                   // filter Up: a repeated row is all zeros
      const row = (y / s) * n;
      for (let x = 0; x < N; x++) raw[o + 1 + x] = idx[row + ((x / s) | 0)];
    }
    const plte = [], trns = [];
    for (const v of pal.keys()) { plte.push(v & 255, (v >>> 8) & 255, (v >>> 16) & 255); trns.push(v >>> 24); }
    const z = [];
    deflateRuns(raw, z);
    const out = [137, 80, 78, 71, 13, 10, 26, 10];
    chunk(out, 'IHDR', [N >>> 24, (N >>> 16) & 255, (N >>> 8) & 255, N & 255, N >>> 24, (N >>> 16) & 255, (N >>> 8) & 255, N & 255, 8, 3, 0, 0, 0]);
    chunk(out, 'PLTE', plte);
    chunk(out, 'tRNS', trns);
    chunk(out, 'IDAT', z);
    chunk(out, 'IEND', []);
    return out;
  }
  function b64(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 4096) s += String.fromCharCode.apply(null, bytes.slice(i, i + 4096));
    return root.btoa(s);
  }

  const cache = new Map();
  // A data: URL for an <img> shown at px CSS pixels. Drawn to land on whole
  // device pixels: the biggest integer pixel scale the display allows (2 on a
  // 2x phone, so art pixels stay chunky), and the rest drawn natively -- a
  // 1.25x laptop gets a 33-pixel drawing for a 26 px slot rather than a
  // 26-pixel one stretched unevenly. Resampling is what bitmaps would need.
  function url(id, px = 26) {
    const dpr = root.devicePixelRatio || 1;
    const s = Math.max(1, Math.floor(dpr + 0.01));
    const n = Math.max(8, Math.round((px | 0) * dpr / s));
    const key = id + ':' + n + ':' + s;
    let u = cache.get(key);
    if (u) return u;
    try {
      const bytes = png(rasterize(id, n), n, s);
      if (!bytes) return '';
      u = 'data:image/png;base64,' + b64(bytes);
    } catch (e) {
      console.warn('[ICON] draw failed', id, e);
      return '';
    }
    cache.set(key, u);
    return u;
  }

  root.ItemIcons = { url, rgba, has: (id) => !!I[id], ids: () => Object.keys(I) };
}(typeof globalThis !== 'undefined' ? globalThis : this));
