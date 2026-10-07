// ── Bike dust ─────────────────────────────────────────────────────────────────
// The dust a Motorbike throws up behind its rear tyre while its pawn is moving.
// drawCharIcon() (renderer.js) says where the tyre meets the ground, and
// renderCharacters() feeds that point here with the distance travelled that
// frame.
//
// Unlike the weather, quake and fire particles, which live in screen px, these
// live in map space: hex units, (screen - camera offset) / HEX_SZ. Riding moves
// the camera with you, so a screen-space cloud would ride along beside the bike
// instead of hanging in the air where it was kicked up. Hex units also keep a
// cloud in place through a zoom.
//
// Drawn as pre-baked lumpy puffs (no per-particle gradients, see the fire
// sprite bank for why), one set per terrain colour, in the layer just under
// the pawns, so each bike sits in front of its own cloud.
// Plain global class, like the other particle systems.

const BIKE_DUST_CAP     = 240;
const BIKE_DUST_PER_HEX = 16;     // puffs per HEX_SZ travelled, per bike
// The dust each terrain throws, by terrain index: the ground's light band
// measured off the hex sheets, pulled toward a pale haze, because airborne
// dust reads lighter than the ground it came off. null = wet ground, no dust.
const BIKE_DUST_COLOR = [
  [194, 184, 151],   //  0 Open Scrub
  [170, 166, 157],   //  1 Ash Dunes
  [134, 118,  92],   //  2 Rust Forest
  null,              //  3 Marsh
  [178, 174, 152],   //  4 Broken Urban
  null,              //  5 Flooded District
  [172, 172, 166],   //  6 Glass Fields
  [155, 151, 128],   //  7 Ridge
  [156, 152, 129],   //  8 Mountain
  [193, 183, 151],   //  9 Settlement
  [178, 172, 158],   // 10 Nuke Crater
  null,              // 11 River Channel
  [150, 142, 128],   // 12 Bunker Entrance
  [150, 142, 128],   // 13 Vent Shaft
  [150, 142, 128],   // 14 Tunnel Floor
  [150, 142, 128],   // 15 Tunnel Collapsed
];
const BIKE_DUST_VARIANTS = 3;     // puff shapes per colour

class BikeDust {
  constructor() {
    this.parts   = [];
    this.riders  = new Map();   // pawn id -> { x, y, seen, acc }: last frame's tyre point
    this.sprites = new Map();   // "r,g,b|v" -> canvas
    this.frame   = 0;
    this.board   = 0;
  }

  // One riding pawn, this frame. wx, wy: its rear tyre's contact point, hex
  // units. vx, vy: how far the pawn itself moved this frame (renderPos, not the
  // tyre, which also jumps when a pack reshuffles), hex units. terrain: the
  // terrain index under it (null when fogged). scale: the pawn's size (packs
  // are drawn smaller).
  kick(id, wx, wy, vx, vy, terrain, scale) {
    const prev = this.riders.get(id);
    const acc  = prev ? prev.acc : 0;
    this.riders.set(id, { x: wx, y: wy, seen: this.frame, acc });
    // Only a rider that was here last frame too: back in view after a gap,
    // or wrapped round the torus to the other side of us, is a jump, not a ride.
    if (!prev || this.frame - prev.seen > 1) return;
    const jump = Math.hypot(wx - prev.x, wy - prev.y);
    const dist = Math.hypot(vx, vy);
    if (dist < 0.004 || jump > 1.2) { this.riders.get(id).acc = 0; return; }
    const col = BIKE_DUST_COLOR[terrain] ?? (terrain == null ? BIKE_DUST_COLOR[0] : null);
    if (!col) return;

    // A pack's pawns are drawn smaller, and so throw less: five bikes on a
    // hex raise one big cloud, not five full-size ones.
    let a = acc + dist * BIKE_DUST_PER_HEX * scale;
    const n = Math.floor(a);
    this.riders.get(id).acc = a - n;
    if (!n) return;
    // Thrown back along the way the bike came, harder the faster it's going.
    const ux = -vx / dist, uy = -vy / dist;
    const pace = Math.min(1, dist / 0.3);
    for (let k = 0; k < n && this.parts.length < BIKE_DUST_CAP; k++) {
      // Spread along the ground the tyre covered this frame, so a fast
      // frame leaves a trail rather than a clump.
      const f  = (k + Math.random()) / n;
      const sp = (0.006 + 0.022 * pace + Math.random() * 0.008) * scale;
      const side = (Math.random() - 0.5) * 1.5;           // fan out off the line
      const off  = (Math.random() - 0.5) * 0.22 * scale;  // and don't sit on it
      this.parts.push({
        x: prev.x + (wx - prev.x) * f - uy * off,
        y: prev.y + (wy - prev.y) * f + ux * off * 0.6,
        vx: (ux - uy * side) * sp,
        vy: (uy + ux * side) * sp * 0.6 - (0.002 + Math.random() * 0.003) * scale,
        // Radius, HEX_SZ: born about tyre-sized, opening out to a cloud the
        // size of the bike.
        s0: (0.1 + Math.random() * 0.12) * scale,
        s1: (0.4 + Math.random() * 0.45) * scale,
        a:  0.6 + Math.random() * 0.25,
        // Barely turned: the puff is lit from above, and a spun one would
        // carry its lit crown round to the side.
        rot: (Math.random() - 0.5) * 0.7,
        spin: (Math.random() - 0.5) * 0.004,
        age: 0,
        ttl: 100 + Math.random() * 90,
        key: `${col}|${(Math.random() * BIKE_DUST_VARIANTS) | 0}`,
      });
    }
  }

  // Age everything a frame: it slows fast, keeps rising a little, and opens
  // out. Riders not fed this frame are forgotten after a moment.
  update() {
    this.frame++;
    for (const p of this.parts) {
      p.age++;
      p.x  += p.vx;
      p.y  += p.vy;
      p.vx *= 0.93;
      p.vy  = p.vy * 0.93 - 0.00035;
      p.rot += p.spin;
    }
    this.parts = this.parts.filter(p => p.age < p.ttl);
    if (this.frame % 60 === 0) {
      for (const [id, r] of this.riders) if (this.frame - r.seen > 60) this.riders.delete(id);
    }
  }

  // Changing board (a descent, a climb) drops every cloud: they belong to the
  // other map.
  render(ctx, ox, oy, hexSz, board, w, h) {
    if (board !== this.board) { this.parts.length = 0; this.riders.clear(); this.board = board; }
    for (const p of this.parts) {
      const t  = p.age / p.ttl;
      const e  = 1 - (1 - t) * (1 - t) * (1 - t);         // opens out fast, then hangs
      const s  = (p.s0 + (p.s1 - p.s0) * e) * hexSz;
      const x  = p.x * hexSz + ox, y = p.y * hexSz + oy;
      if (x < -s || y < -s || x > w + s || y > h + s) continue;
      const al = p.a * (t < 0.06 ? t / 0.06 : Math.pow(1 - (t - 0.06) / 0.94, 1.1));
      if (al < 0.01) continue;
      ctx.save();
      ctx.globalAlpha = al;
      ctx.translate(x, y);
      // Low and wide while it hugs the ground, rounder as it lifts.
      ctx.scale(1, 0.55 + 0.45 * t);
      ctx.rotate(p.rot);
      // Lumpy while it's fresh off the tyre, thinning to haze as it spreads.
      const hz = Math.min(1, Math.max(0, (t - 0.15) / 0.6));
      if (hz < 1) { ctx.globalAlpha = al * (1 - hz); ctx.drawImage(this._sprite(p.key), -s, -s, s * 2, s * 2); }
      if (hz > 0) { ctx.globalAlpha = al * hz;       ctx.drawImage(this._haze(p.key),   -s, -s, s * 2, s * 2); }
      ctx.restore();
    }
  }

  // A lumpy puff, cumulus-style: a cluster of lumps, each lit on its own
  // crown (the gradient's hot spot sits up and to the left) and shaded round
  // its rim, then the whole mound darkened toward its base. That self-shading
  // is what makes a dust cloud read on any ground -- a flat smudge of the
  // ground's own colour vanishes into it. Baked once per colour and shape.
  _sprite(key) {
    let c = this.sprites.get(key);
    if (c) return c;
    const S = 96;
    c = document.createElement('canvas');
    c.width = c.height = S;
    const g = c.getContext('2d');
    const [r0, g0, b0] = key.split('|')[0].split(',').map(Number);
    const tone = (k) => `${Math.max(0, Math.min(255, r0 + k)) | 0},${Math.max(0, Math.min(255, g0 + k)) | 0},${Math.max(0, Math.min(255, b0 + k * 0.9)) | 0}`;
    const hi = tone(34), mid = tone(0), lo = tone(-38);
    let seed = 0;
    for (const ch of key) seed = (seed * 31 + ch.charCodeAt(0)) >>> 0;
    const rnd = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
    g.filter = 'blur(2.5px)';   // no-op where unsupported
    // Back to front: the lower lumps last, so the mound's base sits in front.
    const lumps = [];
    for (let i = 0; i < 11; i++) {
      const a = rnd() * Math.PI * 2, d = Math.sqrt(rnd()) * 20;
      lumps.push({ x: S / 2 + Math.cos(a) * d * 1.15, y: S / 2 + Math.sin(a) * d * 0.8, r: 11 + rnd() * 13 });
    }
    lumps.sort((p, q) => p.y - q.y);
    for (const L of lumps) {
      const grad = g.createRadialGradient(L.x - L.r * 0.35, L.y - L.r * 0.4, 0, L.x, L.y, L.r);
      grad.addColorStop(0,    `rgba(${hi},0.95)`);
      grad.addColorStop(0.45, `rgba(${mid},0.85)`);
      grad.addColorStop(0.8,  `rgba(${lo},0.5)`);
      grad.addColorStop(1,    `rgba(${lo},0)`);
      g.fillStyle = grad;
      g.beginPath();
      g.arc(L.x, L.y, L.r, 0, Math.PI * 2);
      g.fill();
    }
    g.filter = 'none';
    g.globalCompositeOperation = 'source-atop';
    const base = g.createLinearGradient(0, S * 0.35, 0, S * 0.9);
    base.addColorStop(0, 'rgba(40,30,20,0)');
    base.addColorStop(1, 'rgba(40,30,20,0.4)');
    g.fillStyle = base;
    g.fillRect(0, 0, S, S);
    this.sprites.set(key, c);
    return c;
  }

  // The same puff gone to haze: its lumps blurred away, a touch wider.
  _haze(key) {
    const hk = key + '|haze';
    let c = this.sprites.get(hk);
    if (c) return c;
    const src = this._sprite(key), S = src.width;
    c = document.createElement('canvas');
    c.width = c.height = S;
    const g = c.getContext('2d');
    g.filter = 'blur(7px)';
    g.drawImage(src, S * 0.06, S * 0.06, S * 0.88, S * 0.88);
    g.filter = 'none';
    this.sprites.set(hk, c);
    return c;
  }
}
