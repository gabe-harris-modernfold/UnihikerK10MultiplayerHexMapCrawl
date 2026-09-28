// ── Eco Field ──────────────────────────────────────────────────────────────
// The Understory's client half (docs/ecology-spec.md, ecology.hpp,
// mock-server/ecology.js). Same shape as FireField / StormField: it indexes
// the latest `eco` message and the terrain pass queries it per hex, so
// nothing here runs a simulation of its own -- the veins, fruiting bodies,
// daisies and scars are all server-authoritative. What the client adds is
// the *time* between messages: veins fill in rather than pop, a fruiting
// body grows and pulses, a burst throws spores, a daisy sways and turns its
// head toward whoever is standing next to it.
//
// Everything is drawn with math -- no image assets. The look is mycelium
// runners in blood and bruise-purple: tapered, creased cords that fork into
// hyphae, probing tips at the growth front, a faint lub-dub heartbeat rolling
// through the network, pustules that swell and pop, and necrotic staining
// under the thick growth. The genome hue (`h`) only picks where the species
// sits between purple-leaning and blood-leaning; the fruiting-body form comes
// from the genome's fruit nibble (`g`). Both arrive on every message, so a
// client that joins mid-boot sees the same species as one that watched it
// germinate.
//
// Each hex's growth is built once as Path2D in hex-local coordinates and
// cached by (hex, half-step density, mask, open sides, size, hue); a frame
// only fills/strokes those plus the live bits (heartbeat, pulse, tips, boils).
//
// Surface only. The renderer gates every call on !myDepth, like the fire
// field: the grid is keyed by surface q/r and would land on tunnel cells that
// merely share a burning hex's numbers.

const ECO_DQ  = [1, 1, 0, -1, -1, 0];   // mirrors DQ[]/DR[] in Esp32HexMapCrawl.ino
const ECO_DR  = [0, -1, -1, 0, 1, 1];
const ECO_B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const ECO_TAU = Math.PI * 2;

// Deterministic 0..1 from three ints -- every wobble, lean and petal phase is
// hashed from the hex (or the record's seed) so it is the same on every frame
// and every client.
function ecoHash(a, b, c) {
  let h = (a * 374761393 + b * 668265263 + c * 2246822519) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}
function ecoEaseOut(t) { return 1 - (1 - t) * (1 - t); }
function ecoHslToRgb(h, s, l) {
  const f = (n) => {
    const k = (n + h * 12) % 12;
    const a = s * Math.min(l, 1 - l);
    return Math.round(255 * (l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))));
  };
  return [f(0), f(8), f(4)];
}
// hexToPixel() lives in renderer.js, which loads after this file; the call
// sites all run at render time so it is defined by then, but the offsets used
// here are tiny and local, so keep our own copy rather than reach across.
function ecoHexOffset(dq, dr, size) {
  return { x: size * 1.5 * dq, y: size * (Math.sqrt(3) / 2 * dq + Math.sqrt(3) * dr) };
}

// Unit direction of each neighbour in the renderer's flat-top pixel space.
const ECO_DIR = ECO_DQ.map((dq, d) => {
  const o = ecoHexOffset(dq, ECO_DR[d], 1);
  const len = Math.hypot(o.x, o.y);
  return { x: o.x / len, y: o.y / len, a: Math.atan2(o.y, o.x) };
});
const ECO_BEAT_MS = 1700;

// Lub-dub: a sharp beat, a smaller one 0.2 later, then rest.
function ecoBeat(ph) {
  const a = ph / 0.06, b = (ph - 0.19) / 0.07, c = (ph - 1) / 0.06;
  return Math.exp(-a * a) + 0.6 * Math.exp(-b * b) + Math.exp(-c * c);
}
function ecoAngDiff(a, b) {
  let d = a - b;
  while (d >  Math.PI) d -= ECO_TAU;
  while (d < -Math.PI) d += ECO_TAU;
  return d;
}
function ecoRgb(c, a) { return `rgba(${c[0]},${c[1]},${c[2]},${a.toFixed(3)})`; }
function ecoMix(c1, c2, t) {
  return [Math.round(c1[0] + (c2[0] - c1[0]) * t), Math.round(c1[1] + (c2[1] - c1[1]) * t),
          Math.round(c1[2] + (c2[2] - c1[2]) * t)];
}

// A tapered ribbon along pts ({x,y}[]) with per-point widths, appended to
// path as one closed outline. Always wound the same way -- left side out,
// round end cap, right side back, round start cap -- so every ribbon and knot
// in a hex unions under the nonzero rule instead of punching holes.
function ecoRibbon(path, pts, ws, cap) {
  const n = pts.length;
  if (n < 2) return;
  const L = new Array(n), R = new Array(n), N = new Array(n);
  for (let i = 0; i < n; i++) {
    const a = pts[Math.max(0, i - 1)], b = pts[Math.min(n - 1, i + 1)];
    let tx = b.x - a.x, ty = b.y - a.y;
    const tl = Math.hypot(tx, ty) || 1;
    tx /= tl; ty /= tl;
    const nx = -ty, ny = tx, hw = Math.max(0.01, ws[i] / 2);
    L[i] = [pts[i].x + nx * hw, pts[i].y + ny * hw];
    R[i] = [pts[i].x - nx * hw, pts[i].y - ny * hw];
    N[i] = Math.atan2(ny, nx);
  }
  path.moveTo(L[0][0], L[0][1]);
  for (let i = 1; i < n; i++) path.lineTo(L[i][0], L[i][1]);
  if (cap) path.arc(pts[n - 1].x, pts[n - 1].y, Math.max(0.01, ws[n - 1] / 2), N[n - 1], N[n - 1] - Math.PI, true);
  else     path.lineTo(R[n - 1][0], R[n - 1][1]);
  for (let i = n - 2; i >= 0; i--) path.lineTo(R[i][0], R[i][1]);
  path.arc(pts[0].x, pts[0].y, Math.max(0.01, ws[0] / 2), N[0] - Math.PI, N[0] - ECO_TAU, true);
  path.closePath();
}

// A lumpy closed blob, wound the same way as ecoRibbon (decreasing angle).
function ecoBlob(path, x, y, R, seed, lump, k = 9) {
  const pts = [];
  for (let i = 0; i < k; i++) {
    const a = -(i / k) * ECO_TAU;
    const rr = R * (1 - lump + 2 * lump * ecoHash(seed, i, 71));
    pts.push([x + Math.cos(a) * rr, y + Math.sin(a) * rr]);
  }
  const mid = (i) => { const p = pts[i % k], q = pts[(i + 1) % k]; return [(p[0] + q[0]) / 2, (p[1] + q[1]) / 2]; };
  const m0 = mid(0);
  path.moveTo(m0[0], m0[1]);
  for (let i = 1; i <= k; i++) { const p = pts[i % k], m = mid(i); path.quadraticCurveTo(p[0], p[1], m[0], m[1]); }
  path.closePath();
}
// ecoBlob on the context's current path, for the one-off shapes drawn live.
function ecoBlobCtx(ctx, x, y, R, seed, lump) {
  const k = 9, pts = [];
  for (let i = 0; i < k; i++) {
    const a = (i / k) * ECO_TAU, rr = R * (1 - lump + 2 * lump * ecoHash(seed, i, 72));
    pts.push([x + Math.cos(a) * rr, y + Math.sin(a) * rr]);
  }
  ctx.moveTo((pts[0][0] + pts[1][0]) / 2, (pts[0][1] + pts[1][1]) / 2);
  for (let i = 1; i <= k; i++) {
    const p = pts[i % k], n = pts[(i + 1) % k];
    ctx.quadraticCurveTo(p[0], p[1], (p[0] + n[0]) / 2, (p[1] + n[1]) / 2);
  }
  ctx.closePath();
}

// The Wasteland Daisy's ink-and-wash palette (EcoField._drawDaisies): fixed,
// not genome-rolled -- the daisy is always the daisy.
const DZ_INK    = 'rgba(20,13,12,0.6)';
const DZ_STEM   = 'rgba(92,111,71,0.95)';
const DZ_STEMDK = 'rgba(35,45,29,0.95)';
const DZ_LEAF   = 'rgba(83,105,63,0.97)';
const DZ_LEAFHI = 'rgba(126,142,94,0.9)';

// A lancet: base at (0,0), tip at (L,0), widest at 35% of the length.
function dzLancet(ctx, L, W) {
  ctx.beginPath();
  ctx.moveTo(0, 0);
  ctx.quadraticCurveTo(L * 0.35, -W, L, 0);
  ctx.quadraticCurveTo(L * 0.35,  W, 0, 0);
  ctx.closePath();
}

class EcoField {
  constructor() {
    this.cols = MAP_COLS;
    this.rows = MAP_ROWS;
    const n = this.cols * this.rows;
    this.dens  = new Uint8Array(n);    // 0-15 vein density, from the message
    this.mask  = new Uint8Array(n);    // 6-bit edge mask, from the message
    this.scar  = new Uint8Array(n);    // 0-15 scar level, from the rarer `s` message
    this.shown = new Float32Array(n);  // density eased toward dens, so growth fills in
    this.bodies  = new Map();          // hex index -> {q,r,stage,seed,stageAt,seen}
    this.daisies = new Map();          // hex index -> {q,r,stage,count,seed,stageAt,snapAt}
    this.flights = [];                 // spore flights in the air: {fq,fr,tq,tr,t0}
    this.bursts  = [];                 // fruiting bodies bursting: {q,r,seed,t0}
    this.name = ''; this.hue = -1; this.fruit = 0; this.wave = 0; this.tick = 0;
    this.live = false;                 // true once a `v` grid has arrived
    this._b64 = new Int8Array(128).fill(-1);
    for (let i = 0; i < 64; i++) this._b64[ECO_B64.charCodeAt(i)] = i;
    this._setHue(0);
  }

  // A world regen kills the species and clears the scars (ecoRequestRegen).
  // The next message rebuilds all of it.
  reset() {
    this.dens.fill(0); this.mask.fill(0); this.scar.fill(0); this.shown.fill(0);
    this.bodies.clear(); this.daisies.clear();
    this.flights.length = 0; this.bursts.length = 0;
    this._geo.clear(); this._scarGeo?.clear();
    this.live = false;
  }

  _setHue(h) {
    this.hue = h;
    // The genome picks a spot between a bruise-purple species (0) and an
    // arterial one (1). Both colours are always present -- the balance moves.
    const m = (h & 255) / 255;
    const H = (deg) => ((deg % 360) + 360) % 360 / 360;
    this.cRim    = ecoHslToRgb(H(292 + 30 * m), 0.22, 0.055);   // near-black bruise
    this.cFlesh  = ecoHslToRgb(H(280 + 58 * m), 0.24 + 0.08 * m, 0.16);
    this.cRot    = ecoHslToRgb(H(300 + 20 * m), 0.16, 0.07);   // necrotic core
    this.cBlood  = ecoHslToRgb(H(354 + 4 * m), 0.48, 0.27);
    this.cHot    = ecoHslToRgb(H(352 + 4 * m), 0.46, 0.38);    // fresh tips, the pulse
    this.cHair   = ecoHslToRgb(H(300 + 40 * m), 0.20, 0.21);
    this.cStain  = ecoHslToRgb(H(296 + 18 * m), 0.25, 0.06);
    this.cSkin   = ecoHslToRgb(H(300 + 48 * m), 0.24, 0.21);   // boils, bodies
    this.cPus    = [196, 170, 156];
    // Colours are baked into the cached gradients: a new species rebuilds.
    this._geo = new Map();
  }

  // ── Wire ─────────────────────────────────────────────────────────────────
  // Every field is optional: the scar grid rides its own message (`s` alone),
  // the main message never carries `s`, and a stale or partial message must
  // never blank what is already drawn.
  applyMessage(msg) {
    const now = Date.now();
    const n = this.dens.length;
    if (typeof msg.tk === 'number') this.tick = msg.tk;
    if (typeof msg.s === 'string' && msg.s.length >= n) {
      for (let i = 0; i < n; i++) {
        const c = msg.s.charCodeAt(i);
        this.scar[i] = c <= 57 ? c - 48 : ((c & 0xDF) - 55);
      }
    }
    if (typeof msg.v === 'string' && msg.v.length >= n * 2) {
      const b64 = this._b64;
      for (let i = 0, j = 0; i < n; i++, j += 2) {
        const c = msg.v.charCodeAt(j);
        this.dens[i] = c <= 57 ? c - 48 : ((c & 0xDF) - 55);
        const m = b64[msg.v.charCodeAt(j + 1) & 127];
        this.mask[i] = m < 0 ? 0 : m;
      }
      this.live = true;
    }
    if (typeof msg.n === 'string') this.name = msg.n;
    if (typeof msg.h === 'number' && msg.h !== this.hue) this._setHue(msg.h);
    if (typeof msg.g === 'number') this.fruit = msg.g & 15;
    if (typeof msg.w === 'number') this.wave = msg.w;
    if (Array.isArray(msg.f))  this._mergeBodies(msg.f, now);
    if (Array.isArray(msg.dz)) this._mergeDaisies(msg.dz, now);
    if (Array.isArray(msg.sp)) {
      for (const e of msg.sp) {
        if (!Array.isArray(e) || e.length < 4) continue;
        this.flights.push({ fq: e[0], fr: e[1], tq: e[2], tr: e[3], t0: now });
      }
    }
  }

  _mergeBodies(list, now) {
    const seen = new Set();
    for (const e of list) {
      if (!Array.isArray(e) || e.length < 4) continue;
      const [q, r, stage, seed] = e;
      const i = r * this.cols + q;
      seen.add(i);
      let b = this.bodies.get(i);
      if (!b) { b = { q, r, stage: 0, seed, stageAt: now, seen: now }; this.bodies.set(i, b); }
      if (stage !== b.stage) {
        b.stage = stage; b.stageAt = now;
        // Stage 3 is sent exactly once (ecoBurst): the body is gone from the
        // next message, so the puff is queued here, not when it disappears.
        if (stage === 3) this.bursts.push({ q, r, seed, t0: now });
      }
      b.seen = now;
    }
    for (const [i, b] of this.bodies) {
      if (seen.has(i)) continue;
      // A burst body lingers while it fades; anything else missing (a fire
      // took it) is simply gone.
      if (b.stage === 3 && now - b.stageAt < 2200) continue;
      this.bodies.delete(i);
    }
  }

  _mergeDaisies(list, now) {
    const seen = new Set();
    for (const e of list) {
      if (!Array.isArray(e) || e.length < 5) continue;
      const [q, r, stage, count, seed] = e;
      const i = r * this.cols + q;
      seen.add(i);
      let d = this.daisies.get(i);
      if (!d) { d = { q, r, stage, count, seed, stageAt: now, snapAt: 0 }; this.daisies.set(i, d); continue; }
      if (stage !== d.stage) { d.stage = stage; d.stageAt = now; }
      d.count = count; d.seed = seed;
    }
    for (const i of this.daisies.keys()) if (!seen.has(i)) this.daisies.delete(i);
  }

  // A `dmg` event with cause "wasteland daisy" landed on this hex: the
  // flowers there snap shut for a moment.
  noteBite(q, r) {
    const d = this.daisies.get(r * this.cols + q);
    if (d) d.snapAt = Date.now();
  }

  // Heartbeat phase for a hex: one wave rolling across the map in a
  // species-set direction, ~7 hexes long, so the pumping visibly travels
  // through the network rather than every knot beating in lockstep.
  _beatPh(q, r, now) {
    const a = this.hue * 0.0245;
    const x = 1.5 * q, y = 0.866 * q + 1.732 * r;
    const off = (x * Math.cos(a) + y * Math.sin(a)) / 12;
    const t = now / ECO_BEAT_MS - off;
    return t - Math.floor(t);
  }

  // ── Per-hex drawing (called from drawCellOverlays for every visible hex) ──
  drawHex(ctx, cx, cy, q, r, sz, now, players, renderPos) {
    const i = r * this.cols + q;
    const scar   = this.scar[i];
    const target = this.dens[i];
    let shown = this.shown[i];
    if (shown !== target) {
      shown += (target - shown) * 0.06;
      if (Math.abs(target - shown) < 0.05) shown = target;
      this.shown[i] = shown;
    }
    const body  = this.bodies.get(i);
    const patch = this.daisies.get(i);
    if (!scar && shown < 0.5 && !body && !patch) return;
    if (scar)         this._drawScar(ctx, cx, cy, q, r, sz, scar);
    if (shown >= 0.5) this._drawVeins(ctx, cx, cy, q, r, sz, shown, this.mask[i], now);
    if (body)         this._drawBody(ctx, cx, cy, sz, body, now);
    if (patch)        this._drawDaisies(ctx, cx, cy, q, r, sz, patch, now, players, renderPos);
  }

  // ── Geometry ──────────────────────────────────────────────────────────────
  _nbr(q, r, d) {
    const nq = (q + ECO_DQ[d] + this.cols) % this.cols;
    const nr = (r + ECO_DR[d] + this.rows) % this.rows;
    return nr * this.cols + nq;
  }

  _cordW(d, sz) { return sz * (0.014 + 0.036 * Math.pow(Math.max(0, d) / 15, 0.9)); }

  // Where a vein crosses edge d, its direction there, and its width -- all
  // hashed from the shared edge so both hexes agree to the pixel and the
  // cord runs straight through the seam instead of meeting at a midpoint.
  _cross(q, r, d, sz) {
    const ia = r * this.cols + q, ib = this._nbr(q, r, d);
    const lo = Math.min(ia, ib), hi = Math.max(ia, ib);
    const t0 = 0.5 + (ecoHash(lo, hi, 11) - 0.5) * 0.46;
    const t  = ia <= ib ? t0 : 1 - t0;
    const skew = (ecoHash(lo, hi, 12) - 0.5) * 0.9;
    const a = ECO_DIR[d].a;
    const c1x = Math.cos(a - Math.PI / 6) * sz, c1y = Math.sin(a - Math.PI / 6) * sz;
    const c2x = Math.cos(a + Math.PI / 6) * sz, c2y = Math.sin(a + Math.PI / 6) * sz;
    const va = a + skew;
    return {
      x: c1x + (c2x - c1x) * t, y: c1y + (c2y - c1y) * t,
      vx: Math.cos(va), vy: Math.sin(va),
      w: this._cordW(Math.min(this.dens[ia], this.dens[ib]), sz),
    };
  }

  // A cord from the knot to an edge crossing: a cubic that leaves the knot
  // on a hashed heading and arrives along the crossing's shared direction,
  // bent by two octaves of wobble that vanish (value and slope) at both ends.
  _trunk(K, X, wK, grow, seed, sz) {
    const N = 14;
    const dx = X.x - K.x, dy = X.y - K.y, Ld = Math.hypot(dx, dy) || 1;
    const a0 = Math.atan2(dy, dx) + (ecoHash(seed, 1, 40) - 0.5) * 1.1;
    const p1x = K.x + Math.cos(a0) * Ld * 0.38, p1y = K.y + Math.sin(a0) * Ld * 0.38;
    const p2x = X.x - X.vx * Ld * 0.38,        p2y = X.y - X.vy * Ld * 0.38;
    const f1 = 0.8 + ecoHash(seed, 2, 40) * 0.8, ph1 = ecoHash(seed, 3, 40);
    const f2 = 3.5 + ecoHash(seed, 4, 40) * 3,   ph2 = ecoHash(seed, 5, 40);
    const A1 = sz * (0.03 + 0.05 * ecoHash(seed, 6, 40)), A2 = sz * 0.012;
    const raw = [];
    for (let k = 0; k <= N; k++) {
      const s = k / N, u = 1 - s;
      raw.push({
        x: u * u * u * K.x + 3 * u * u * s * p1x + 3 * u * s * s * p2x + s * s * s * X.x,
        y: u * u * u * K.y + 3 * u * u * s * p1y + 3 * u * s * s * p2y + s * s * s * X.y,
      });
    }
    const pts = [], ws = [];
    for (let k = 0; k <= N; k++) {
      const s = k / N;
      const a = raw[Math.max(0, k - 1)], b = raw[Math.min(N, k + 1)];
      let tx = b.x - a.x, ty = b.y - a.y; const tl = Math.hypot(tx, ty) || 1; tx /= tl; ty /= tl;
      const env = Math.pow(Math.sin(Math.PI * s), 2);
      const wob = env * (A1 * Math.sin(ECO_TAU * (f1 * s + ph1)) + A2 * Math.sin(ECO_TAU * (f2 * s + ph2)));
      pts.push({ x: raw[k].x - ty * wob, y: raw[k].y + tx * wob, s });
      // Lumpy along its length (swollen segments), exact at the seam.
      const lump = 1 + 0.32 * (1 - s) * Math.sin(ECO_TAU * (2.3 * s + ecoHash(seed, 7, 40))) * ecoHash(seed, 8, 40);
      const sm = s * s * (3 - 2 * s);
      ws.push((wK + (X.w * grow - wK) * sm) * lump);
    }
    // Run on past the seam: whichever of the two hexes draws second lays its
    // overshoot over the other's tile bleed, so the cord never shows a gap.
    const ov = sz * 0.07;
    pts.push({ x: X.x + X.vx * ov, y: X.y + X.vy * ov, s: 1 }); ws.push(ws[N]);
    return { pts, ws };
  }

  // A runner that wanders off a point and tapers to nothing, forking.
  // Ribbon while it is wider than a hair, hair after; its end is a tip.
  _branch(g, x, y, a, len, w, depth, seed, sz, creep) {
    const steps = 9, hairW = g.hairW;
    const pts = [{ x, y }], ws = [w];
    const ph = ecoHash(seed, 1, 50) * ECO_TAU, bend = (ecoHash(seed, 2, 50) - 0.5) * 0.5;
    const R = sz * 0.8;
    for (let k = 1; k <= steps; k++) {
      a += bend * 0.35 + 0.28 * Math.sin(k * 1.3 + ph) + (ecoHash(seed, k, 51) - 0.5) * 0.35;
      const rr = Math.hypot(x, y);
      if (rr > R * 0.85) a += ecoAngDiff(Math.atan2(-y, -x), a) * Math.min(0.6, (rr - R * 0.85) / (R * 0.2));
      x += Math.cos(a) * len / steps; y += Math.sin(a) * len / steps;
      pts.push({ x, y }); ws.push(w * Math.pow(1 - k / steps, 0.85));
    }
    let cut = pts.length;
    for (let k = 0; k < pts.length; k++) if (ws[k] < hairW) { cut = k; break; }
    if (cut >= 2) ecoRibbon(g.flesh, pts.slice(0, cut), ws.slice(0, cut), true);
    const h0 = Math.max(0, cut - 1);
    if (h0 < pts.length - 1) {
      g.hair.moveTo(pts[h0].x, pts[h0].y);
      for (let k = h0 + 1; k < pts.length; k++) g.hair.lineTo(pts[k].x, pts[k].y);
    }
    const tip = pts[pts.length - 1];
    if (creep) g.tips.push({ x: tip.x, y: tip.y, a, L: sz * (0.07 + 0.08 * ecoHash(seed, 3, 50)), ph: ecoHash(seed, 4, 50) * ECO_TAU, sp: 0.7 + ecoHash(seed, 5, 50) * 0.8 });
    else this._fuzz(g, tip.x, tip.y, a, sz * 0.045, seed);
    if (depth > 0 && len > sz * 0.1) {
      const at = 4 + ((ecoHash(seed, 6, 50) * 3) | 0);
      const side = ecoHash(seed, 7, 50) < 0.5 ? -1 : 1;
      const p = pts[at];
      this._branch(g, p.x, p.y, Math.atan2(pts[at + 1].y - p.y, pts[at + 1].x - p.x) + side * (0.55 + 0.45 * ecoHash(seed, 8, 50)),
                   len * 0.6, ws[at] * 0.8, depth - 1, seed * 7 + 13, sz, creep);
    }
  }

  // A little fan of hyphae at a resting tip.
  _fuzz(g, x, y, a, L, seed) {
    for (let k = 0; k < 3; k++) {
      const aa = a + (k - 1) * 0.7 + (ecoHash(seed, k, 60) - 0.5) * 0.4;
      const ll = L * (0.5 + ecoHash(seed, k, 61));
      g.hair.moveTo(x, y);
      g.hair.lineTo(x + Math.cos(aa) * ll, y + Math.sin(aa) * ll);
    }
  }

  _build(ctx, q, r, sz, d, mask, open) {
    const seed = r * 977 + q * 131 + 7;
    const t = Math.min(1, d / 15);
    const g = {
      flesh: new Path2D(), core: new Path2D(), hair: new Path2D(), wrinkle: new Path2D(), mottle: new Path2D(), stain: null, grad: null,
      trunks: [], tips: [], boils: [], hairW: Math.max(0.7, sz * 0.014), t,
    };
    const K = { x: (ecoHash(q, r, 1) - 0.5) * sz * 0.5, y: (ecoHash(q, r, 2) - 0.5) * sz * 0.5 };
    g.K = K;
    const wK = this._cordW(d, sz) * 1.25;
    const grow = Math.min(1, d / Math.max(1, this.dens[r * this.cols + q]));

    // Necrotic ground: a bruise under anything dense enough to have sat a while.
    if (d >= 4) {
      g.stain = new Path2D();
      const n = 2 + (d >= 10 ? 1 : 0);
      for (let k = 0; k < n; k++) {
        const aa = ecoHash(seed, k, 20) * ECO_TAU, rad = sz * 0.16 * ecoHash(seed, k, 21);
        ecoBlob(g.stain, K.x + Math.cos(aa) * rad, K.y + Math.sin(aa) * rad, sz * (0.18 + 0.30 * t) * (0.7 + 0.4 * ecoHash(seed, k, 22)), seed + k, 0.35, 11);
      }
      g.grad = ctx.createRadialGradient(K.x, K.y, 0, K.x, K.y, sz * (0.2 + 0.4 * t));
      g.grad.addColorStop(0, ecoRgb(this.cStain, 0.35 + 0.35 * t));
      g.grad.addColorStop(0.55, ecoRgb(this.cStain, 0.2 + 0.25 * t));
      g.grad.addColorStop(0.85, ecoRgb(ecoMix(this.cStain, this.cBlood, 0.35), 0.08 + 0.12 * t));
      g.grad.addColorStop(1, ecoRgb(this.cStain, 0));
    }

    // Trunks to every crossed edge.
    for (let dd = 0; dd < 6; dd++) {
      if (!(mask & (1 << dd))) continue;
      const X = this._cross(q, r, dd, sz);
      const tr = this._trunk(K, X, wK, grow, seed * 11 + dd, sz);
      ecoRibbon(g.flesh, tr.pts, tr.ws, false);
      ecoRibbon(g.core, tr.pts, tr.ws.map((w) => w * 0.38), false);
      g.trunks.push(tr);
      // Side runners, appearing one by one as the density climbs, each
      // creeping out to its length rather than popping in.
      const nb = Math.min(4, Math.floor(d / 3.2));
      for (let b = 0; b < nb; b++) {
        const thr = 1.5 + b * 3.2 + ecoHash(seed, dd * 8 + b, 30) * 1.5;
        const gb = Math.min(1, Math.max(0, (d - thr) / 3));
        if (gb <= 0) continue;
        const k = 2 + ((ecoHash(seed, dd * 8 + b, 31) * 9) | 0);
        const p = tr.pts[k], pn = tr.pts[k + 1];
        const side = ecoHash(seed, dd * 8 + b, 32) < 0.5 ? -1 : 1;
        const a = Math.atan2(pn.y - p.y, pn.x - p.x) + side * (0.6 + 0.6 * ecoHash(seed, dd * 8 + b, 33));
        this._branch(g, p.x, p.y, a, sz * (0.16 + 0.22 * ecoHash(seed, dd * 8 + b, 34)) * gb,
                     tr.ws[k] * 0.55, d >= 8 ? 2 : 1, seed * 31 + dd * 8 + b, sz, false);
      }
      // Skin: creases across the cord, and necrotic patches on the old,
      // thick ones.
      for (let k = 1; k < tr.pts.length - 2; k++) {
        const p = tr.pts[k], pn = tr.pts[k + 1];
        const ta = Math.atan2(pn.y - p.y, pn.x - p.x), hw = tr.ws[k] * 0.5;
        if (hw > sz * 0.012 && ecoHash(seed, dd * 40 + k, 39) < 0.75) {
          const na = ta + Math.PI / 2 + (ecoHash(seed, dd * 40 + k, 41) - 0.5) * 0.6;
          const cx = p.x + (pn.x - p.x) * 0.5, cy = p.y + (pn.y - p.y) * 0.5;
          g.wrinkle.moveTo(cx + Math.cos(na) * hw * 0.9, cy + Math.sin(na) * hw * 0.9);
          g.wrinkle.quadraticCurveTo(cx + Math.cos(ta) * hw * 0.4, cy + Math.sin(ta) * hw * 0.4,
                                     cx - Math.cos(na) * hw * 0.9, cy - Math.sin(na) * hw * 0.9);
        }
        if (d >= 7 && ecoHash(seed, dd * 40 + k, 42) < 0.18 + 0.25 * t) {
          ecoBlob(g.mottle, p.x + (ecoHash(seed, dd * 40 + k, 43) - 0.5) * hw, p.y + (ecoHash(seed, dd * 40 + k, 44) - 0.5) * hw,
                  hw * (0.5 + 0.6 * ecoHash(seed, dd * 40 + k, 45)), seed + dd * 40 + k, 0.4, 7);
        }
      }
      // A web of finer hyphae wandering off the cord, forking twice.
      const nh = Math.min(6, Math.floor(d / 2.2));
      for (let b = 0; b < nh; b++) {
        const thr = 1 + b * 2.2 + ecoHash(seed, dd * 16 + b, 55) * 1.2;
        const gb = Math.min(1, Math.max(0, (d - thr) / 2.5));
        if (gb <= 0) continue;
        const k = 1 + ((ecoHash(seed, dd * 16 + b, 56) * 11) | 0);
        const p = tr.pts[k], pn = tr.pts[k + 1];
        const side = ecoHash(seed, dd * 16 + b, 57) < 0.5 ? -1 : 1;
        const a = Math.atan2(pn.y - p.y, pn.x - p.x) + side * (0.7 + 0.8 * ecoHash(seed, dd * 16 + b, 58));
        this._branch(g, p.x, p.y, a, sz * (0.1 + 0.2 * ecoHash(seed, dd * 16 + b, 59)) * gb,
                     g.hairW * 0.9, 2, seed * 17 + dd * 16 + b + 500, sz, false);
      }
      // Fibrous fuzz along the cord: short hairs off its flanks.
      if (d >= 3) {
        for (let k = 1; k < tr.pts.length - 1; k += 1) {
          if (ecoHash(seed, dd * 40 + k, 35) > 0.25 + t * 0.4) continue;
          const p = tr.pts[k], pn = tr.pts[k + 1];
          const ta = Math.atan2(pn.y - p.y, pn.x - p.x);
          const side = ecoHash(seed, dd * 40 + k, 36) < 0.5 ? -1 : 1;
          const na = ta + side * (Math.PI / 2) + (ecoHash(seed, dd * 40 + k, 37) - 0.5) * 1.2;
          const hw = tr.ws[k] / 2, ll = sz * (0.02 + 0.045 * ecoHash(seed, dd * 40 + k, 38));
          const sx = p.x + Math.cos(ta + side * Math.PI / 2) * hw, sy = p.y + Math.sin(ta + side * Math.PI / 2) * hw;
          g.hair.moveTo(sx, sy); g.hair.lineTo(sx + Math.cos(na) * ll, sy + Math.sin(na) * ll);
        }
      }
    }

    // The growth front: toward every empty neighbour a thin runner reaches
    // most of the way and breaks into probing tips. A hex with no crossings
    // at all (a germinating spore, a stranded remnant) sprawls every way.
    const fronts = [];
    for (let dd = 0; dd < 6; dd++) if (open & (1 << dd)) fronts.push(dd);
    const isolated = mask === 0;
    const heads = isolated ? 5 + ((ecoHash(seed, 0, 45) * 3) | 0) : fronts.length;
    for (let k = 0; k < heads; k++) {
      const a = isolated ? (k / heads) * ECO_TAU + ecoHash(seed, k, 46) * 0.8
                         : ECO_DIR[fronts[k]].a + (ecoHash(seed, fronts[k], 47) - 0.5) * 0.8;
      const reach = sz * (isolated ? 0.28 + 0.2 * ecoHash(seed, k, 48) : 0.45 + 0.2 * ecoHash(seed, k, 48)) * (0.35 + 0.65 * Math.min(1, d / 5));
      const w0 = this._cordW(d, sz) * 0.7;
      // Main runner, then a fan of 2-4 creeping tips off its end.
      const run = { pts: [], ws: [] };
      let x = K.x, y = K.y, aa = a;
      const steps = 8;
      run.pts.push({ x, y }); run.ws.push(w0);
      for (let s = 1; s <= steps; s++) {
        aa += (ecoHash(seed, k * 20 + s, 49) - 0.5) * 0.5;
        x += Math.cos(aa) * reach / steps; y += Math.sin(aa) * reach / steps;
        run.pts.push({ x, y }); run.ws.push(w0 * (1 - 0.8 * s / steps));
      }
      ecoRibbon(g.flesh, run.pts, run.ws, true);
      const fan = 2 + ((ecoHash(seed, k, 52) * 3) | 0);
      for (let f = 0; f < fan; f++) {
        const fa = aa + (f - (fan - 1) / 2) * 0.55 + (ecoHash(seed, k * 9 + f, 53) - 0.5) * 0.3;
        this._branch(g, x, y, fa, sz * (0.08 + 0.1 * ecoHash(seed, k * 9 + f, 54)), run.ws[steps] * 0.9, 0, seed * 3 + k * 9 + f, sz, true);
      }
    }

    // The knot: a swollen, necrotic node where the cords meet.
    ecoBlob(g.flesh, K.x, K.y, wK * 1.15 + sz * 0.01, seed, 0.28);
    ecoBlob(g.core, K.x, K.y, wK * 0.55, seed + 1, 0.3);
    g.fleshGrad = ctx.createRadialGradient(K.x, K.y, 0, K.x, K.y, sz * 0.55);
    g.fleshGrad.addColorStop(0, ecoRgb(ecoMix(this.cFlesh, this.cRot, Math.min(1, t * 1.3)), 1));
    g.fleshGrad.addColorStop(0.25 + 0.2 * (1 - t), ecoRgb(this.cFlesh, 1));
    g.fleshGrad.addColorStop(1, ecoRgb(ecoMix(this.cFlesh, this.cBlood, 0.12), 1));

    // Boils: on the knot once it's thick, then along the fattest cords.
    if (d >= 6 && g.trunks.length) {
      const nboil = 1 + (d >= 10 ? 1 : 0) + (d >= 14 ? 1 : 0);
      for (let b = 0; b < nboil; b++) {
        let x = K.x, y = K.y;
        if (b > 0) {
          const tr = g.trunks[(b * 3 + seed) % g.trunks.length];
          const p = tr.pts[4 + ((ecoHash(seed, b, 80) * 6) | 0)];
          x = p.x; y = p.y;
        }
        g.boils.push({
          x: x + (ecoHash(seed, b, 81) - 0.5) * wK, y: y + (ecoHash(seed, b, 82) - 0.5) * wK,
          R: sz * (0.05 + 0.04 * ecoHash(seed, b, 83)) * (0.65 + 0.45 * t),
          T: 9000 + 11000 * ecoHash(seed, b, 84), ph: ecoHash(seed, b, 85), seed: seed * 5 + b,
        });
      }
    }
    return g;
  }

  _geoFor(ctx, q, r, sz, shown, mask) {
    let open = 0;
    const i = r * this.cols + q;
    for (let d = 0; d < 6; d++) if (!(mask & (1 << d)) && this.dens[this._nbr(q, r, d)] === 0) open |= 1 << d;
    const step = Math.round(shown * 2) / 2;
    const key = `${step}|${mask}|${open}|${sz}|${this.hue}`;
    let e = this._geo.get(i);
    if (e && e.key === key) return e.g;
    e = { key, g: this._build(ctx, q, r, sz, step, mask, open) };
    this._geo.delete(i);
    this._geo.set(i, e);
    if (this._geo.size > 900) {
      let n = 150;
      for (const k of this._geo.keys()) { this._geo.delete(k); if (--n <= 0) break; }
    }
    return e.g;
  }

  // ── Veins ────────────────────────────────────────────────────────────────
  _drawVeins(ctx, cx, cy, q, r, sz, shown, mask, now) {
    const g = this._geoFor(ctx, q, r, sz, shown, mask);
    const target = this.dens[r * this.cols + q];
    // Withdrawing (the colony spent itself fruiting): the cords rot black.
    const rot = shown > target + 0.3 ? Math.min(1, (shown - target) / 4) : 0;
    const ph = this._beatPh(q, r, now);
    const hb = ecoBeat(ph);
    const fade = Math.min(1, shown / 1.5);
    ctx.save();
    ctx.translate(cx, cy);
    ctx.globalAlpha = fade;
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';

    if (g.stain) { ctx.fillStyle = g.grad; ctx.fill(g.stain); }

    // Rim, flesh, and the blood core that swells on every beat.
    ctx.strokeStyle = ecoRgb(this.cRim, 0.85);
    ctx.lineWidth = Math.max(1.2, sz * 0.028);
    ctx.stroke(g.flesh);
    ctx.fillStyle = g.fleshGrad;
    ctx.fill(g.flesh);
    ctx.fillStyle = ecoRgb(this.cBlood, (0.65 + 0.12 * hb) * (1 - rot));
    ctx.fill(g.core);
    if (hb > 0.05 && !rot) {
      ctx.strokeStyle = ecoRgb(this.cBlood, 0.25 * hb);
      ctx.lineWidth = sz * 0.01 * hb * (0.4 + g.t);
      ctx.stroke(g.core);
    }
    ctx.fillStyle = ecoRgb(this.cRot, 0.7);
    ctx.fill(g.mottle);
    ctx.strokeStyle = ecoRgb(this.cRim, 0.55);
    ctx.lineWidth = Math.max(0.6, sz * 0.008);
    ctx.stroke(g.wrinkle);
    if (rot) { ctx.fillStyle = ecoRgb(this.cRot, 0.75 * rot); ctx.fill(g.flesh); }

    // Hyphae.
    ctx.strokeStyle = ecoRgb(ecoMix(this.cHair, this.cRot, rot), 0.75);
    ctx.lineWidth = g.hairW;
    ctx.stroke(g.hair);

    // The pulse: a swelling pushed out along every cord, knot to edge, just
    // after the beat -- something being pumped.
    if (!rot) {
      const s = (ph - 0.02) / 0.55;
      if (s > 0 && s < 1) {
        const al = Math.sin(Math.PI * s);
        ctx.fillStyle = ecoRgb(this.cHot, 0.18 * al);
        for (const tr of g.trunks) {
          const f = s * (tr.pts.length - 1), k = Math.min(tr.pts.length - 2, f | 0), u = f - k;
          const a = tr.pts[k], b = tr.pts[k + 1];
          const x = a.x + (b.x - a.x) * u, y = a.y + (b.y - a.y) * u;
          const w = tr.ws[k] + (tr.ws[k + 1] - tr.ws[k]) * u;
          ctx.beginPath();
          ctx.ellipse(x, y, w * 0.95, w * 0.5, Math.atan2(b.y - a.y, b.x - a.x), 0, ECO_TAU);
          ctx.fill();
        }
      }
    }

    // Creeping tips: each probes forward and sweeps side to side, slowly.
    if (g.tips.length) {
      const tp = new Path2D();
      const beads = [];
      for (const tip of g.tips) {
        const tt = now * 0.00035 * tip.sp + tip.ph;
        const ext = 0.45 + 0.55 * (0.5 + 0.5 * Math.sin(tt));
        const sw = 0.45 * Math.sin(tt * 1.7 + 1.3);
        const a = tip.a + sw, L = tip.L * ext;
        const mx = tip.x + Math.cos(tip.a + sw * 0.4) * L * 0.5, my = tip.y + Math.sin(tip.a + sw * 0.4) * L * 0.5;
        const ex = tip.x + Math.cos(a) * L, ey = tip.y + Math.sin(a) * L;
        tp.moveTo(tip.x, tip.y); tp.quadraticCurveTo(mx, my, ex, ey);
        for (let k = -1; k <= 1; k++) {
          const fa = a + k * 0.75 + 0.3 * Math.sin(tt * 2.3 + k);
          const fl = sz * 0.03 * (0.6 + 0.4 * ext);
          tp.moveTo(ex, ey); tp.lineTo(ex + Math.cos(fa) * fl, ey + Math.sin(fa) * fl);
        }
        beads.push(ex, ey);
      }
      ctx.strokeStyle = ecoRgb(ecoMix(this.cHot, this.cHair, 0.5), 0.8 * (1 - rot));
      ctx.lineWidth = g.hairW;
      ctx.stroke(tp);
      ctx.fillStyle = ecoRgb(this.cHot, 0.8 * (1 - rot));
      for (let k = 0; k < beads.length; k += 2) {
        ctx.beginPath(); ctx.arc(beads[k], beads[k + 1], g.hairW * 0.8, 0, ECO_TAU); ctx.fill();
      }
    }

    for (const b of g.boils) this._drawBoil(ctx, b, now, hb, sz, rot);
    ctx.restore();
  }

  // A pustule that swells over ten-odd seconds, ripens, pops in a spatter,
  // leaves a wet crater, and starts again.
  _drawBoil(ctx, b, now, hb, sz, rot) {
    const u = ((now / b.T + b.ph) % 1 + 1) % 1;
    const R = b.R;
    if (u < 0.84) {
      const g = u / 0.84;
      const s = (0.3 + 0.7 * g * g * (3 - 2 * g)) * (1 + 0.03 * hb) * (1 - 0.3 * rot);
      const rr = R * s;
      ctx.fillStyle = ecoRgb(this.cRim, 0.8);
      ctx.beginPath(); ctx.ellipse(b.x, b.y, rr * 1.15, rr * 1.0, 0, 0, ECO_TAU); ctx.fill();
      const gr = ctx.createRadialGradient(b.x - rr * 0.3, b.y - rr * 0.35, rr * 0.1, b.x, b.y, rr);
      gr.addColorStop(0, ecoRgb(ecoMix(this.cHot, this.cSkin, 0.2 + 0.6 * rot), 1));
      gr.addColorStop(0.6, ecoRgb(this.cSkin, 1));
      gr.addColorStop(1, ecoRgb(this.cRim, 1));
      ctx.fillStyle = gr;
      ctx.beginPath(); ctx.ellipse(b.x, b.y, rr * 1.05, rr * 0.92, 0, 0, ECO_TAU); ctx.fill();
      // Veins over the skin.
      ctx.strokeStyle = ecoRgb(this.cRim, 0.55);
      ctx.lineWidth = Math.max(0.6, rr * 0.09);
      ctx.beginPath();
      for (let k = 0; k < 3; k++) {
        const a = ecoHash(b.seed, k, 90) * ECO_TAU;
        ctx.moveTo(b.x + Math.cos(a) * rr * 0.95, b.y + Math.sin(a) * rr * 0.85);
        ctx.quadraticCurveTo(b.x + Math.cos(a + 0.6) * rr * 0.4, b.y + Math.sin(a + 0.6) * rr * 0.4,
                             b.x + Math.cos(a + 1.2) * rr * 0.2, b.y + Math.sin(a + 1.2) * rr * 0.2);
      }
      ctx.stroke();
      // The head comes up as it ripens.
      if (g > 0.55) {
        const hh = (g - 0.55) / 0.45;
        ctx.fillStyle = ecoRgb(this.cPus, 0.85 * hh);
        ctx.beginPath(); ctx.arc(b.x - rr * 0.12, b.y - rr * 0.18, rr * 0.32 * hh, 0, ECO_TAU); ctx.fill();
      }
      ctx.fillStyle = `rgba(255,236,236,${(0.35 * s).toFixed(3)})`;
      ctx.beginPath(); ctx.arc(b.x - rr * 0.42, b.y - rr * 0.45, rr * 0.13, 0, ECO_TAU); ctx.fill();
    } else {
      // Popped: a spatter flying out, then a wet crater that dries.
      const v = (u - 0.84) / 0.16;
      ctx.fillStyle = ecoRgb(this.cBlood, 0.55 * (1 - v));
      ctx.beginPath(); ecoBlobCtx(ctx, b.x, b.y, R * (1.1 + 0.5 * Math.min(1, v * 6)), b.seed, 0.4); ctx.fill();
      ctx.strokeStyle = ecoRgb(this.cRim, 0.7 * (1 - v));
      ctx.lineWidth = Math.max(0.8, R * 0.2);
      ctx.beginPath(); ctx.arc(b.x, b.y, R * 0.45, 0, ECO_TAU); ctx.stroke();
      if (v < 0.3) {
        const e = v / 0.3;
        ctx.fillStyle = ecoRgb(this.cBlood, 0.9 * (1 - e));
        for (let k = 0; k < 7; k++) {
          const a = ecoHash(b.seed, k, 92) * ECO_TAU, d = R * (0.8 + 2.4 * ecoHash(b.seed, k, 93)) * (0.3 + 0.7 * e);
          ctx.beginPath(); ctx.arc(b.x + Math.cos(a) * d, b.y + Math.sin(a) * d, R * 0.16 * (1 - e * 0.5), 0, ECO_TAU); ctx.fill();
        }
      }
    }
  }

  // ── Scars: last boot's growth, dead ───────────────────────────────────────
  // Dried husks of runners on a dried-blood stain. The species that made it
  // is gone and its colour was never saved, so these are the same dead
  // brown-black whatever grows now.
  _drawScar(ctx, cx, cy, q, r, sz, level) {
    const i = r * this.cols + q;
    if (!this._scarGeo) this._scarGeo = new Map();
    const key = `${level}|${sz}`;
    let e = this._scarGeo.get(i);
    if (!e || e.key !== key) {
      const seed = r * 613 + q * 37 + 3;
      const stain = new Path2D(), husk = new Path2D(), crumbs = [];
      const n = 1 + (level >= 8 ? 1 : 0);
      for (let k = 0; k < n; k++) {
        const a = ecoHash(seed, k, 1) * ECO_TAU, rad = sz * 0.18 * ecoHash(seed, k, 2);
        ecoBlob(stain, Math.cos(a) * rad, Math.sin(a) * rad, sz * (0.22 + 0.2 * ecoHash(seed, k, 3)) * (0.6 + level / 25), seed + k, 0.4, 11);
      }
      const nh = 2 + Math.min(3, (level / 4) | 0);
      const ox = (ecoHash(seed, 9, 4) - 0.5) * sz * 0.3, oy = (ecoHash(seed, 9, 5) - 0.5) * sz * 0.3;
      for (let k = 0; k < nh; k++) {
        let a = (k / nh) * ECO_TAU + ecoHash(seed, k, 6) * 1.2;
        let x = ox, y = oy;
        const len = sz * (0.25 + 0.3 * ecoHash(seed, k, 7)), steps = 10;
        for (let s = 0; s < steps; s++) {
          a += (ecoHash(seed, k * 30 + s, 8) - 0.5) * 0.7;
          const nx = x + Math.cos(a) * len / steps, ny = y + Math.sin(a) * len / steps;
          if (ecoHash(seed, k * 30 + s, 9) > 0.22) { husk.moveTo(x, y); husk.lineTo(nx, ny); }
          x = nx; y = ny;
          if (Math.hypot(x, y) > sz * 0.78) break;
        }
        crumbs.push(x, y);
      }
      e = { key, stain, husk, crumbs };
      this._scarGeo.set(i, e);
    }
    const a = 0.10 + (level / 15) * 0.22;
    ctx.save();
    ctx.translate(cx, cy);
    ctx.fillStyle = `rgba(20,6,10,${a.toFixed(3)})`;
    ctx.fill(e.stain);
    ctx.lineCap = 'round';
    ctx.strokeStyle = `rgba(14,6,8,${Math.min(0.8, a * 2.6).toFixed(3)})`;
    ctx.lineWidth = Math.max(1.4, sz * 0.03);
    ctx.stroke(e.husk);
    ctx.strokeStyle = `rgba(92,52,50,${Math.min(0.6, a * 1.8).toFixed(3)})`;
    ctx.lineWidth = Math.max(0.6, sz * 0.009);
    ctx.stroke(e.husk);
    ctx.fillStyle = `rgba(14,6,8,${Math.min(0.8, a * 2.6).toFixed(3)})`;
    for (let k = 0; k < e.crumbs.length; k += 2) {
      ctx.beginPath(); ctx.arc(e.crumbs[k], e.crumbs[k + 1], Math.max(1, sz * 0.02), 0, ECO_TAU); ctx.fill();
    }
    ctx.restore();
  }

  // ── Fruiting bodies ───────────────────────────────────────────────────────
  // Small clumps scattered over the hex -- 2-4 of them (seed), each a squat
  // mound crowded with 2-5 stubby stalks (fruit nibble) of mixed heights,
  // each carrying a head whose form is the nibble's upper bits: a blood
  // blister, a drooping gill cap, a red cage, or a seamed pod. Stage 1 swells
  // up out of the ground clump by clump, stage 2 stands and throbs faintly
  // with the network's heartbeat, stage 3 ruptures while drawAir() lets the
  // spores go from every clump.
  _clumps(seed, sz) {
    const n = 2 + ((ecoHash(seed, 0, 20) * 3) | 0);
    const out = [];
    const a0 = ecoHash(seed, 1, 20) * ECO_TAU;
    for (let c = 0; c < n; c++) {
      const a = a0 + (c / n) * ECO_TAU + (ecoHash(seed, c, 21) - 0.5) * 1.2;
      const rad = sz * (c === 0 ? 0.08 : 0.2 + 0.2 * ecoHash(seed, c, 22));
      out.push({ x: Math.cos(a) * rad, y: Math.sin(a) * rad * 0.85, s: 0.7 + 0.5 * ecoHash(seed, c, 23) });
    }
    return out;
  }

  _drawBody(ctx, cx, cy, sz, body, now) {
    const g = this.fruit;
    const stalks = 2 + (g & 3);
    const shape  = (g >> 2) & 3;
    const age = now - body.stageAt;
    const hb = ecoBeat(this._beatPh(body.q, body.r, now));
    let grow = 1, fade = 1, pulse = 1, burst = 0;
    if (body.stage === 1) {
      grow = ecoEaseOut(Math.min(1, age / 6000)) * 0.85;
      pulse = 1 + 0.012 * hb;
    } else if (body.stage === 2) {
      grow = 0.85 + 0.15 * ecoEaseOut(Math.min(1, age / 2000));
      pulse = 1 + 0.025 * hb;
    } else if (body.stage === 3) {
      burst = Math.min(1, age / 1800);
      fade = 1 - burst;
    }
    if (grow <= 0.02 || fade <= 0.02) return;
    const seed = body.seed | 0;
    const clumps = this._clumps(seed, sz);
    ctx.save();
    ctx.translate(cx, cy);
    ctx.globalAlpha = fade;
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    const items = [];
    for (let c = 0; c < clumps.length; c++) {
      const cl = clumps[c];
      // Clumps come up one after another, not all at once.
      const gc = Math.min(1, Math.max(0, grow * 1.25 - c * 0.12));
      if (gc <= 0.02) continue;
      const S = sz * 0.2 * cl.s * gc;
      // Mound: a squat dark lump the stalks crowd out of.
      ctx.fillStyle = 'rgba(0,0,0,0.3)';
      ctx.beginPath(); ctx.ellipse(cl.x, cl.y + S * 0.12, S * 0.75, S * 0.28, 0, 0, ECO_TAU); ctx.fill();
      const mound = new Path2D();
      ecoBlob(mound, 0, 0, S * 0.55, seed + c * 7, 0.3, 9);
      ctx.save(); ctx.translate(cl.x, cl.y); ctx.scale(1, 0.5);
      ctx.fillStyle = ecoRgb(this.cRim, 1); ctx.fill(mound);
      ctx.fillStyle = ecoRgb(this.cSkin, 0.5); ctx.scale(0.7, 0.7); ctx.fill(mound);
      ctx.restore();
      const n = stalks + (ecoHash(seed, c, 24) < 0.5 ? 1 : 0);
      for (let k = 0; k < n; k++) {
        const a = ecoHash(seed, c * 10 + k, 25) * ECO_TAU, rr = S * 0.3 * Math.sqrt(ecoHash(seed, c * 10 + k, 26));
        const bx = cl.x + Math.cos(a) * rr, by = cl.y + Math.sin(a) * rr * 0.45;
        // Lean away from the clump's middle, so the cluster fans.
        const lean = Math.cos(a) * S * 0.25 + (ecoHash(seed, c * 10 + k, 27) - 0.5) * S * 0.3;
        const h = S * (0.45 + 0.75 * ecoHash(seed, c * 10 + k, 28)) * pulse;
        items.push({ bx, by, lean, h, S, k: c * 10 + k });
      }
    }
    // Back to front so the nearer stalks overlap.
    items.sort((a, b) => a.by - b.by);
    for (const it of items) {
      const { bx, by, lean, h, S, k } = it;
      const tx = bx + lean, ty = by - h;
      const stem = new Path2D();
      const pts = [], ws = [];
      for (let s = 0; s <= 6; s++) {
        const u = s / 6;
        pts.push({ x: bx + lean * (0.25 * u + 0.75 * u * u), y: by - h * u });
        ws.push(S * (0.24 - 0.1 * u) * (1 + 0.15 * Math.sin(u * 7 + k)));
      }
      ecoRibbon(stem, pts, ws, true);
      ctx.strokeStyle = ecoRgb(this.cRim, 0.9); ctx.lineWidth = Math.max(0.8, S * 0.05); ctx.stroke(stem);
      ctx.fillStyle = ecoRgb(this.cSkin, 1); ctx.fill(stem);
      this._drawHead(ctx, tx, ty, S * (0.2 + 0.08 * ecoHash(seed, k, 29)) * pulse, shape, lean / S, seed + k, burst, hb, now);
    }
    ctx.restore();
  }

  _drawHead(ctx, x, y, R, shape, lean, seed, burst, hb, now) {
    const rim = ecoRgb(this.cRim, 0.95);
    const swell = 1 + burst * 0.3 - (burst > 0.35 ? (burst - 0.35) * 1.1 : 0);
    R *= Math.max(0.3, swell);
    switch (shape) {
      case 0: {  // blood blister: a taut translucent sac, veined, tearing open
        const gr = ctx.createRadialGradient(x - R * 0.3, y - R * 0.35, R * 0.1, x, y, R);
        gr.addColorStop(0, ecoRgb(this.cHot, 1));
        gr.addColorStop(0.7, ecoRgb(this.cBlood, 1));
        gr.addColorStop(1, ecoRgb(this.cRim, 1));
        ctx.fillStyle = gr;
        ctx.beginPath(); ctx.arc(x, y, R, 0, ECO_TAU); ctx.fill();
        ctx.strokeStyle = ecoRgb(this.cSkin, 0.8); ctx.lineWidth = Math.max(0.6, R * 0.08);
        ctx.beginPath();
        for (let k = 0; k < 4; k++) {
          const a = ecoHash(seed, k, 5) * ECO_TAU;
          ctx.moveTo(x + Math.cos(a) * R, y + Math.sin(a) * R);
          ctx.quadraticCurveTo(x + Math.cos(a + 0.5) * R * 0.5, y + Math.sin(a + 0.5) * R * 0.5, x + Math.cos(a + 1) * R * 0.15, y + Math.sin(a + 1) * R * 0.15);
        }
        ctx.stroke();
        if (burst > 0.35) {
          ctx.strokeStyle = rim; ctx.lineWidth = Math.max(1, R * 0.25);
          ctx.beginPath(); ctx.moveTo(x - R * 0.7, y - R * 0.1); ctx.lineTo(x - R * 0.1, y + R * 0.15); ctx.lineTo(x + R * 0.6, y - R * 0.2); ctx.stroke();
        }
        ctx.fillStyle = `rgba(255,230,230,${(0.4 * (1 - burst)).toFixed(3)})`;
        ctx.beginPath(); ctx.arc(x - R * 0.4, y - R * 0.45, R * 0.16, 0, ECO_TAU); ctx.fill();
        break;
      }
      case 1: {  // gill cap: a drooping dome, red gills under the lip, dripping
        ctx.fillStyle = ecoRgb(this.cBlood, 1);
        ctx.beginPath(); ctx.ellipse(x, y + R * 0.2, R * 1.3, R * 0.42, lean * 0.3, 0, ECO_TAU); ctx.fill();
        ctx.strokeStyle = ecoRgb(this.cRim, 0.8); ctx.lineWidth = Math.max(0.6, R * 0.06);
        ctx.beginPath();
        for (let k = 0; k < 9; k++) {
          const a = Math.PI * (k / 8);
          ctx.moveTo(x, y + R * 0.2); ctx.lineTo(x + Math.cos(a) * R * 1.25, y + R * 0.2 + Math.sin(a) * R * 0.4);
        }
        ctx.stroke();
        ctx.fillStyle = ecoRgb(this.cSkin, 1);
        ctx.beginPath(); ctx.ellipse(x, y + R * 0.1, R * 1.35, R * 1.05, lean * 0.3, Math.PI, ECO_TAU); ctx.fill();
        ctx.strokeStyle = rim; ctx.lineWidth = Math.max(1, R * 0.1); ctx.stroke();
        ctx.fillStyle = ecoRgb(this.cRim, 0.6);
        for (let k = 0; k < 4; k++) {
          const a = Math.PI + (0.2 + 0.6 * ecoHash(seed, k, 6)) * Math.PI;
          ctx.beginPath(); ctx.arc(x + Math.cos(a) * R * 0.8, y + R * 0.1 + Math.sin(a) * R * 0.6, R * 0.14, 0, ECO_TAU); ctx.fill();
        }
        // A drop gathers at the lip and falls.
        const dt = ((now * 0.0004 + ecoHash(seed, 7, 7)) % 1);
        const dx = x + R * (ecoHash(seed, 8, 7) - 0.5) * 1.8;
        const dy = y + R * 0.45 + (dt > 0.6 ? (dt - 0.6) * R * 6 : 0);
        ctx.fillStyle = ecoRgb(this.cBlood, dt > 0.95 ? 0 : 0.95);
        ctx.beginPath(); ctx.ellipse(dx, dy, R * 0.12, R * (0.12 + Math.min(0.6, dt) * 0.18), 0, 0, ECO_TAU); ctx.fill();
        break;
      }
      case 2: {  // cage: a red lattice ball round a dark, stinking heart
        ctx.fillStyle = ecoRgb(this.cRot, 0.95);
        ctx.beginPath(); ctx.arc(x, y, R * 0.9, 0, ECO_TAU); ctx.fill();
        ctx.strokeStyle = rim; ctx.lineWidth = Math.max(1.4, R * 0.36);
        const lattice = () => {
          ctx.beginPath(); ctx.arc(x, y, R * 1.02, 0, ECO_TAU);
          for (let k = 0; k < 3; k++) {
            const a = k * Math.PI / 3 + ecoHash(seed, 9, 9) * 0.6;
            ctx.moveTo(x + Math.cos(a) * R, y + Math.sin(a) * R);
            ctx.quadraticCurveTo(x + Math.cos(a + Math.PI / 2) * R * 0.35, y + Math.sin(a + Math.PI / 2) * R * 0.35, x - Math.cos(a) * R, y - Math.sin(a) * R);
          }
        };
        lattice(); ctx.stroke();
        ctx.strokeStyle = ecoRgb(ecoMix(this.cBlood, this.cHot, 0.4 + 0.12 * hb), 1); ctx.lineWidth = Math.max(0.9, R * 0.2);
        lattice(); ctx.stroke();
        break;
      }
      default: {  // pod: a seamed sac that splits and oozes
        const rot = lean * 0.6;
        ctx.save();
        ctx.translate(x, y);
        ctx.rotate(rot);
        ctx.fillStyle = ecoRgb(this.cRim, 1);
        ctx.beginPath(); ctx.ellipse(0, 0, R * 0.85, R * 1.45, 0, 0, ECO_TAU); ctx.fill();
        ctx.fillStyle = ecoRgb(this.cSkin, 1);
        ctx.beginPath(); ctx.ellipse(0, 0, R * 0.75, R * 1.35, 0, 0, ECO_TAU); ctx.fill();
        const open = 0.12 + 0.04 * hb + burst * 0.5;
        ctx.fillStyle = ecoRgb(this.cBlood, 1);
        ctx.beginPath(); ctx.ellipse(0, 0, R * open, R * 1.1, 0, 0, ECO_TAU); ctx.fill();
        ctx.fillStyle = ecoRgb(this.cHot, 0.8);
        ctx.beginPath(); ctx.ellipse(0, R * 0.9 + burst * R * 0.6, R * 0.14, R * (0.18 + burst * 0.3), 0, 0, ECO_TAU); ctx.fill();
        ctx.restore();
      }
    }
  }

  // The Wasteland Daisy. 1-6 clumps (one per injury on the hex, so a patch
  // runs from a few scattered flowers to a dense field), each 1-3 plants,
  // inked like the painted tiles. Stage 1 rosettes with seed leaves, stage 2
  // throbbing buds split by a red seam, stage 3 open flowers -- blood-flushed
  // petals, an ochre disc and a breathing toothed mouth -- that turn toward
  // any survivor on or next to the hex and snap shut, throwing red, when
  // noteBite() says one just got bitten.
  _drawDaisies(ctx, cx, cy, q, r, sz, patch, now, players, renderPos) {
    const stage = patch.stage;
    const grow = ecoEaseOut(Math.min(1, (now - patch.stageAt) / 4000));
    const snapT = patch.snapAt ? (now - patch.snapAt) / 700 : 9;
    const snapping = snapT < 1;
    const seed = patch.seed | 0;

    // Nearest survivor within a hex and a half, as a pixel offset from the
    // hex centre. renderPos is the lerped position, so heads track a walker
    // smoothly rather than jumping when the mv event lands.
    let tx = 0, ty = 0, has = false, best = 9;
    if (stage === 3 && players && renderPos) {
      for (let i = 0; i < players.length; i++) {
        const p = players[i];
        if (!p || !p.on || p.dp) continue;
        let dq = renderPos[i].q - q, dr = renderPos[i].r - r;
        while (dq >  this.cols / 2) dq -= this.cols;
        while (dq < -this.cols / 2) dq += this.cols;
        while (dr >  this.rows / 2) dr -= this.rows;
        while (dr < -this.rows / 2) dr += this.rows;
        const dist = (Math.abs(dq) + Math.abs(dq + dr) + Math.abs(dr)) / 2;
        if (dist < best && dist <= 1.6) {
          best = dist;
          const off = ecoHexOffset(dq, dr, sz);
          tx = off.x; ty = off.y; has = true;
        }
      }
    }

    // Layout: 1-6 clumps across the hex -- one per injury, more pain, more
    // clumps -- each 1-3 plants round a larger one. Bases are sorted back to
    // front so nearer heads overlap farther ones. Scratch arrays live on the
    // instance: nothing allocates per frame.
    const MAXP = 18;
    const bx  = this._dzX || (this._dzX = new Float32Array(MAXP));
    const by  = this._dzY || (this._dzY = new Float32Array(MAXP));
    const bg  = this._dzB || (this._dzB = new Float32Array(MAXP));
    const ord = this._dzO || (this._dzO = new Uint8Array(MAXP));
    const ps = sz * 0.8;                        // plant scale
    const clumps = Math.min(6, (patch.count | 0) + 1);
    const turn = ecoHash(seed, 0, 50) * ECO_TAU;
    let n = 0;
    for (let c = 0; c < clumps; c++) {
      // One clump sits near the middle; the rest ring it, evenly spaced with a
      // hashed jitter so they never form a visible circle.
      const ring = clumps === 1 || (clumps >= 5 && c === 0) ? 0 : 1;
      const slots = clumps >= 5 ? clumps - 1 : clumps;
      const slot = clumps >= 5 ? c - 1 : c;
      const ca = turn + (slot / slots) * ECO_TAU + (ecoHash(seed, c, 51) - 0.5) * 0.8;
      const cr = ring ? sz * (0.58 + 0.14 * ecoHash(seed, c, 52)) : sz * 0.08 * ecoHash(seed, c, 52);
      const ccx = cx + Math.cos(ca) * cr;
      const ccy = cy + Math.sin(ca) * cr * 0.9 + sz * 0.04;
      const plants = 1 + ((ecoHash(seed, c, 53) * 3) | 0);
      for (let j = 0; j < plants && n < MAXP; j++, n++) {
        // Companions scatter loosely round the clump's lead plant, spaced
        // round it so they don't stack on each other.
        const pa = ecoHash(seed, c, 54) * ECO_TAU + (j / plants) * ECO_TAU + (ecoHash(seed, n, 1) - 0.5) * 1.2;
        const pr = j === 0 ? 0 : ps * (0.14 + 0.18 * ecoHash(seed, n, 2));
        bx[n] = ccx + Math.cos(pa) * pr;
        by[n] = ccy + Math.sin(pa) * pr * 0.8;
        bg[n] = j === 0 ? (c === 0 ? 1.25 : 1.1) : 0.75 + 0.3 * ecoHash(seed, n, 3);
        ord[n] = n;
      }
    }
    for (let a = 1; a < n; a++) {           // insertion sort on by
      const v = ord[a]; let b = a - 1;
      while (b >= 0 && by[ord[b]] > by[v]) { ord[b + 1] = ord[b]; b--; }
      ord[b + 1] = v;
    }

    ctx.save();
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';

    // The ground each clump grew from: a faint rust-dark stain.
    ctx.fillStyle = `rgba(53,26,21,${(0.09 * grow).toFixed(3)})`;
    for (let k = 0; k < n; k++) {
      const rr = ps * 0.12 * bg[k];
      ctx.beginPath();
      ctx.ellipse(bx[k], by[k], rr, rr * 0.5, 0, 0, ECO_TAU);
      ctx.fill();
    }

    // Pass 1: rosettes, flat on the ground, under every stem.
    for (let o = 0; o < n; o++) {
      const k = ord[o];
      const big = bg[k];
      const leaves = 4 + ((ecoHash(seed, k, 4) * 3) | 0);
      const L = ps * 0.13 * big * (stage === 1 ? 0.85 : 1) * grow;
      if (L < 1) continue;
      for (let j = 0; j < leaves; j++) {
        const la = (j / leaves) * ECO_TAU + ecoHash(seed, k, 5) * 2;
        ctx.save();
        ctx.translate(bx[k], by[k]);
        ctx.scale(1, 0.5);
        ctx.rotate(la);
        const LL = L * (0.8 + 0.4 * ecoHash(seed, k * 8 + j, 6));
        dzLancet(ctx, LL, L * 0.4);
        ctx.fillStyle = (j & 1) ? DZ_LEAF : DZ_LEAFHI; ctx.fill();
        ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.6, ps * 0.01); ctx.stroke();
        ctx.restore();
      }
    }

    // Pass 2: stems and heads.
    for (let o = 0; o < n; o++) {
      const k = ord[o];
      const h3 = ecoHash(seed, k, 3);
      const ph = h3 * ECO_TAU;
      const big = bg[k];
      const sway = Math.sin(now * 0.0016 + ph) * (stage >= 2 ? 1 : 0.4)
                 + (snapping ? Math.sin(now * 0.09 + ph) * 0.6 * (1 - snapT) : 0);
      const H = ps * (stage === 1 ? 0.09 : stage === 2 ? 0.18 : 0.22) * big * grow;
      if (H < 1) continue;
      const x0 = bx[k], y0 = by[k];
      // Blooms bend toward whoever is near, as well as swaying.
      let lean = sway * H * 0.22;
      if (has && stage === 3) lean += Math.max(-1, Math.min(1, (tx - (x0 - cx)) / ps)) * H * 0.2;
      const topX = x0 + lean, topY = y0 - H;
      const cxs = x0 + lean * 0.15, cys = y0 - H * 0.6;
      const sw = Math.max(1, ps * 0.015 * big);

      ctx.beginPath();
      ctx.moveTo(x0, y0);
      ctx.quadraticCurveTo(cxs, cys, topX, topY);
      ctx.strokeStyle = DZ_STEMDK; ctx.lineWidth = sw + Math.max(0.7, ps * 0.012); ctx.stroke();
      ctx.strokeStyle = DZ_STEM;   ctx.lineWidth = sw; ctx.stroke();

      if (stage >= 2) {
        // One leaf off the stem, on the side the hash picks.
        const side = ecoHash(seed, k, 7) < 0.5 ? -1 : 1;
        const lx = (x0 + cxs) / 2, ly = (y0 + cys) / 2;
        ctx.save();
        ctx.translate(lx, ly);
        ctx.rotate(side > 0 ? -0.5 + sway * 0.1 : Math.PI + 0.5 + sway * 0.1);
        dzLancet(ctx, H * 0.55, H * 0.16);
        ctx.fillStyle = DZ_LEAFHI; ctx.fill();
        ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.8, ps * 0.012); ctx.stroke();
        ctx.restore();
      }

      if (stage === 1) {
        // Two seed leaves opening in a V at the tip.
        for (let s = -1; s <= 1; s += 2) {
          ctx.save();
          ctx.translate(topX, topY);
          ctx.rotate(-Math.PI / 2 + s * (0.9 + 0.15 * sway));
          dzLancet(ctx, ps * 0.07 * big, ps * 0.03 * big);
          ctx.fillStyle = DZ_LEAFHI; ctx.fill();
          ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.6, ps * 0.01); ctx.stroke();
          ctx.restore();
        }
        continue;
      }

      if (stage === 2) {
        // A pointed bud; the pale tip splits the sepals, a red seam where it
        // will open. It throbs, slowly.
        const R = ps * 0.055 * big;
        const throb = 1 + 0.06 * Math.sin(now * 0.004 + ph);
        ctx.save();
        ctx.translate(topX, topY);
        ctx.rotate(sway * 0.25);
        ctx.scale(throb, throb);
        ctx.beginPath();
        ctx.moveTo(0, -R * 2.0);
        ctx.bezierCurveTo(R * 1.1, -R * 1.2, R * 1.0, R * 0.4, 0, R * 0.6);
        ctx.bezierCurveTo(-R * 1.0, R * 0.4, -R * 1.1, -R * 1.2, 0, -R * 2.0);
        ctx.fillStyle = 'rgba(228,222,208,0.97)'; ctx.fill();
        ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.8, ps * 0.014); ctx.stroke();
        ctx.strokeStyle = 'rgba(136,54,57,0.8)'; ctx.lineWidth = Math.max(0.7, ps * 0.01);
        ctx.beginPath(); ctx.moveTo(0, -R * 1.8); ctx.lineTo(0, -R * 0.2); ctx.stroke();
        // Sepals cupping the base.
        ctx.fillStyle = DZ_LEAF;
        for (let s = -1; s <= 1; s += 2) {
          ctx.beginPath();
          ctx.moveTo(0, R * 0.7);
          ctx.quadraticCurveTo(s * R * 1.3, R * 0.2, s * R * 0.45, -R * 0.9);
          ctx.quadraticCurveTo(s * R * 0.3, R * 0.1, 0, R * 0.7);
          ctx.fill();
          ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.7, ps * 0.01); ctx.stroke();
        }
        ctx.restore();
        continue;
      }

      // ── Bloom ──
      // The head is a disc facing the camera, foreshortened along the line to
      // the survivor: it has turned to look at them.
      const R = ps * 0.10 * big;
      let lookA = 0, tilt = 0;
      if (has) {
        const vx = tx - (topX - cx), vy = ty - (topY - cy);
        lookA = Math.atan2(vy, vx);
        tilt = 0.3;
      }
      const jit = snapping ? (ecoHash(seed, k, (now / 40) | 0) - 0.5) * ps * 0.03 * (1 - snapT) : 0;
      const hx = topX + Math.cos(lookA) * R * tilt * 0.3 + jit;
      const hy = topY + Math.sin(lookA) * R * tilt * 0.3;
      // Snap: petals fold in fast, then relax back open over the rest of the
      // 700 ms.
      const shut = snapping ? (snapT < 0.15 ? snapT / 0.15 : 1 - (snapT - 0.15) / 0.85) : 0;
      const breathe = 0.5 + 0.5 * Math.sin(now * 0.0025 + ph * 3);
      const openness = (1 - shut * 0.8);

      // Shadow the head throws on the ground.
      ctx.fillStyle = 'rgba(0,0,0,0.22)';
      ctx.beginPath();
      ctx.ellipse(x0 + lean * 0.5 + R * 0.3, y0 + R * 0.1, R * 1.1 * openness, R * 0.4, 0, 0, ECO_TAU);
      ctx.fill();

      ctx.save();
      ctx.translate(hx, hy);
      ctx.rotate(lookA);
      ctx.scale(1 - tilt, 1);
      ctx.rotate(-lookA);
      ctx.scale(1, 0.88);   // a touch of 3/4 view even when nobody is near

      // Petals: two rings of lancets, back ring darker. Each hashed a little
      // longer or shorter; one per plant is torn short.
      const petals = 9;
      const torn = (ecoHash(seed, k, 11) * petals) | 0;
      for (let ring = 0; ring < 2; ring++) {
        const off = ring ? Math.PI / petals : 0;
        for (let j = 0; j < petals; j++) {
          const pa = (j / petals) * ECO_TAU + off + ph;
          let L = R * (ring ? 1.0 : 1.18) * (0.85 + 0.3 * ecoHash(seed, k * 32 + j, 12 + ring));
          if (ring && j === torn) L *= 0.55;
          L *= openness;
          ctx.save();
          ctx.rotate(pa);
          ctx.translate(R * 0.22, 0);
          dzLancet(ctx, L, R * 0.30 * (1 - shut * 0.4));
          ctx.fillStyle = ring ? 'rgba(244,241,230,0.99)' : 'rgba(210,203,188,0.99)';
          ctx.fill();
          ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.5, ps * 0.008); ctx.stroke();
          // Blood-flush at the petal root.
          if (ring) {
            dzLancet(ctx, L * 0.42, R * 0.16);
            ctx.fillStyle = 'rgba(160,64,67,0.6)'; ctx.fill();
          }
          ctx.restore();
        }
      }

      // The disc: ochre pollen ring, so it still reads "daisy" ...
      const dR = R * 0.48;
      ctx.fillStyle = '#B69142';
      ctx.beginPath(); ctx.arc(0, 0, dR, 0, ECO_TAU); ctx.fill();
      ctx.strokeStyle = DZ_INK; ctx.lineWidth = Math.max(0.8, ps * 0.012); ctx.stroke();
      ctx.fillStyle = 'rgba(215,188,115,0.9)';
      for (let j = 0; j < 7; j++) {
        const a = ecoHash(seed, k * 16 + j, 20) * ECO_TAU;
        const d = dR * (0.72 + 0.18 * ecoHash(seed, k * 16 + j, 21));
        ctx.beginPath(); ctx.arc(Math.cos(a) * d, Math.sin(a) * d, Math.max(0.6, R * 0.05), 0, ECO_TAU); ctx.fill();
      }
      // ... and in it a mouth, breathing, ringed with teeth pointing in.
      const mR = dR * (0.66 + 0.12 * breathe) * (1 - shut * 0.85);
      if (mR > 0.6) {
        ctx.fillStyle = '#180A0C';
        ctx.beginPath(); ctx.arc(0, 0, mR, 0, ECO_TAU); ctx.fill();
        ctx.fillStyle = `rgba(134,41,47,${(0.55 + 0.25 * breathe).toFixed(3)})`;
        ctx.beginPath(); ctx.arc(0, mR * 0.15, mR * 0.55, 0, ECO_TAU); ctx.fill();
        ctx.fillStyle = 'rgba(236,230,216,0.95)';
        const teeth = 9;
        ctx.beginPath();
        for (let j = 0; j < teeth; j++) {
          const a = (j / teeth) * ECO_TAU + ph;
          const w = ECO_TAU / teeth * 0.38;
          ctx.moveTo(Math.cos(a - w) * mR, Math.sin(a - w) * mR);
          ctx.lineTo(Math.cos(a) * mR * 0.45, Math.sin(a) * mR * 0.45);
          ctx.lineTo(Math.cos(a + w) * mR, Math.sin(a + w) * mR);
        }
        ctx.fill();
      } else {
        // Clamped shut: a dark seam.
        ctx.strokeStyle = '#180A0C'; ctx.lineWidth = Math.max(1, R * 0.12);
        ctx.beginPath(); ctx.moveTo(-dR * 0.6, 0); ctx.lineTo(dR * 0.6, 0); ctx.stroke();
      }
      ctx.restore();

      // Flecks of red thrown off on the snap.
      if (snapping && k === ord[n - 1]) {
        const e = ecoEaseOut(snapT);
        ctx.fillStyle = `rgba(152,46,52,${(0.9 * (1 - snapT)).toFixed(3)})`;
        for (let j = 0; j < 6; j++) {
          const a = ecoHash(seed, j, 40) * ECO_TAU;
          const d = ps * (0.1 + 0.25 * ecoHash(seed, j, 41)) * e;
          ctx.beginPath();
          ctx.arc(hx + Math.cos(a) * d, hy + Math.sin(a) * d * 0.7 + e * e * ps * 0.1,
                  Math.max(0.8, ps * 0.018 * (1 - snapT * 0.5)), 0, ECO_TAU);
          ctx.fill();
        }
      }
    }
    ctx.restore();
  }

  // ── Screen-space pass (LAYERS 'eco_air', after the entities) ─────────────
  // Everything here is drawn at the wrapped copy nearest the camera, like the
  // caravan.
  _unwrap(q, r, cq, cr) {
    while (q - cq >  this.cols / 2) q -= this.cols;
    while (cq - q >  this.cols / 2) q += this.cols;
    while (r - cr >  this.rows / 2) r -= this.rows;
    while (cr - r >  this.rows / 2) r += this.rows;
    return [q, r];
  }

  // A burst is a fine mist: from every clump a cloud of specks puffs out,
  // slows, rises and drifts, thinning as it goes, over a faint haze. A spore
  // in flight is a small drifting knot of the same specks trailing a thread
  // of them, and settles in a last little puff where it lands.
  _speck(ctx, x, y, r, c, a) {
    ctx.globalAlpha = a;
    ctx.fillStyle = c;
    ctx.fillRect(x - r / 2, y - r / 2, r, r);
  }

  drawAir(ctx, cam, now, sz, w, h) {
    if (!this.flights.length && !this.bursts.length) return;
    const { ox, oy, centreQ, centreR } = cam;
    const margin = sz * 3;
    const pr = Math.max(1, sz * 0.02);   // one speck, in px
    // Mostly dark spores -- the wasteland is mostly pale ground -- with a
    // quarter dusty-pale ones so the mist still reads over forest and water.
    const pale = [214, 196, 196];
    const tints = [ecoRgb(this.cRim, 1), ecoRgb(ecoMix(this.cBlood, this.cRim, 0.3), 1), ecoRgb(ecoMix(this.cSkin, this.cRim, 0.4), 1),
                   ecoRgb(ecoMix(this.cSkin, pale, 0.6), 1)];
    ctx.save();
    if (this.flights.length) {
      this.flights = this.flights.filter((f) => now - f.t0 < 3800);
      const ease = (tt) => tt < 0.5 ? 2 * tt * tt : 1 - Math.pow(-2 * tt + 2, 2) / 2;
      for (const f of this.flights) {
        const t = Math.min(1, (now - f.t0) / 3000);
        const [fq, fr] = this._unwrap(f.fq, f.fr, centreQ, centreR);
        const [tq, tr] = this._unwrap(f.tq, f.tr, fq, fr);
        const p0 = ecoHexOffset(fq, fr, sz), p1 = ecoHexOffset(tq, tr, sz);
        const x0 = p0.x + ox, y0 = p0.y + oy, x1 = p1.x + ox, y1 = p1.y + oy;
        if ((x0 < -margin && x1 < -margin) || (x0 > w + margin && x1 > w + margin)) continue;
        if ((y0 < -margin && y1 < -margin) || (y0 > h + margin && y1 > h + margin)) continue;
        const fs = (f.tq * 31 + f.tr * 17) | 0;
        const at = (tt) => {
          const e = ease(tt);
          return [x0 + (x1 - x0) * e + Math.sin(tt * 9 + fs) * sz * 0.06, y0 + (y1 - y0) * e - Math.sin(tt * Math.PI) * sz * 1.1];
        };
        if (t < 1) {
          // Trail: a thread of specks falling behind and thinning out.
          for (let k = 1; k <= 10; k++) {
            const tt = Math.max(0, t - k * 0.025);
            const [x, y] = at(tt);
            const j = ecoHash(fs, k, 3) - 0.5;
            this._speck(ctx, x + j * sz * 0.05 * k * 0.3, y + k * sz * 0.006, pr, tints[k & 1], 0.35 * (1 - k / 11));
          }
          const [x, y] = at(t);
          for (let k = 0; k < 7; k++) {
            const a = ecoHash(fs, k, 1) * ECO_TAU + now * 0.002 * (k & 1 ? 1 : -1);
            const d = sz * 0.035 * ecoHash(fs, k, 2);
            this._speck(ctx, x + Math.cos(a) * d, y + Math.sin(a) * d, pr, tints[k % 3], 0.75);
          }
        } else {
          const v = Math.min(1, (now - f.t0 - 3000) / 800);
          for (let k = 0; k < 12; k++) {
            const a = ecoHash(fs, k, 4) * ECO_TAU, d = sz * (0.04 + 0.14 * ecoHash(fs, k, 5)) * ecoEaseOut(v);
            this._speck(ctx, x1 + Math.cos(a) * d, y1 + Math.sin(a) * d * 0.6 - v * sz * 0.05, pr, tints[k % 3], 0.6 * (1 - v));
          }
        }
      }
    }
    if (this.bursts.length) {
      const DUR = 4200;
      this.bursts = this.bursts.filter((b) => now - b.t0 < DUR);
      for (const b of this.bursts) {
        const t = Math.min(1, (now - b.t0) / DUR);
        const [q, r] = this._unwrap(b.q, b.r, centreQ, centreR);
        const p = ecoHexOffset(q, r, sz);
        const hx = p.x + ox, hy = p.y + oy;
        if (hx < -margin || hx > w + margin || hy < -margin || hy > h + margin) continue;
        const clumps = this._clumps(b.seed | 0, sz);
        const e = 1 - Math.pow(1 - t, 3);          // puffs out fast, then hangs
        const fadeIn = Math.min(1, t / 0.05);
        const life = Math.pow(1 - t, 0.8) * fadeIn;
        const rise = sz * 0.4 * t;
        for (let c = 0; c < clumps.length; c++) {
          const cl = clumps[c];
          const px = hx + cl.x, py = hy + cl.y - sz * 0.12 * cl.s;
          // The haze the specks hang in.
          const hr = sz * (0.12 + 0.42 * e) * cl.s;
          const gr = ctx.createRadialGradient(px, py - rise, 0, px, py - rise, hr);
          gr.addColorStop(0, ecoRgb(this.cSkin, 0.22 * life));
          gr.addColorStop(1, ecoRgb(this.cSkin, 0));
          ctx.globalAlpha = 1;
          ctx.fillStyle = gr;
          ctx.beginPath(); ctx.arc(px, py - rise, hr, 0, ECO_TAU); ctx.fill();
          const n = 90;
          for (let k = 0; k < n; k++) {
            const s = b.seed * 13 + c * 97 + k;
            const a = ecoHash(s, 1, 7) * ECO_TAU;
            const sp = sz * (0.06 + 0.6 * Math.pow(ecoHash(s, 2, 7), 1.3)) * cl.s;
            const drift = Math.sin(t * 4 + ecoHash(s, 3, 7) * ECO_TAU) * sz * 0.05 * t;
            const x = px + Math.cos(a) * sp * e + drift;
            const y = py + Math.sin(a) * sp * e * 0.6 - rise * (0.6 + 0.8 * ecoHash(s, 4, 7));
            this._speck(ctx, x, y, pr * (0.7 + 0.6 * ecoHash(s, 5, 7)), tints[k & 3], 0.95 * life * (0.55 + 0.45 * ecoHash(s, 6, 7)));
          }
        }
      }
    }
    ctx.restore();
  }
}
