// ── Trails: footprints and tire tracks ───────────────────────────
// What a hex shows of who has crossed it. HexCell.footprints (one bit per
// seat, which is one per archetype) and HexCell.tireTrack (the caravan, or
// anyone riding "tracks" gear such as the Motorbike) say only THAT someone
// crossed it. Tracks are the game's tracking mechanism, though, and a track
// you can follow has to say which way it went. The way comes from three
// places, best first:
//
//   Recorded. Every client hears every `mv` event (network.js _evMv, whose
//   "d" names the side stepped through), and the caravan's position rides
//   every world broadcast, so each step is seen as it happens: the side a
//   walker came in by, the side they left by, and when. Kept in localStorage
//   like the explored map, up to TRAIL_MAX_PER_HEX passes a walker.
//
//   Served. The server keeps each walker's latest pass on every hex
//   (trails.hpp) and sends it as "trl" beside the cells of a vis disk,
//   survey ring or sync -- cell.trl[slot] (map-decoder.js applyTrailRecs).
//   That is what a fresh browser, or a phone, reads old trails from.
//
//   Inferred. A track laid before the server kept sides has only its bit. It
//   is joined to the neighbouring hexes that carry the same bit, and pointed
//   at whoever laid it: the neighbour fewer steps along the trail from the
//   maker's current hex is the way out (_ranker). Someone following a fresh
//   trail is walking toward its maker, so that is right far more often than
//   not. For an old trail that doubled back it is a plausible misreading,
//   which is what old tracks are.
//
// The bits stay authoritative. A recorded pass draws only while its hex still
// carries the bit, so a regenerated board can never show a stale trail -- and
// the server sends a side only while its bit is set.
//
// Each seat leaves its own sole (TRAIL_SOLES), so you can tell whose tracks
// they are, as a real tracker would. Hard ground keeps fewer prints than ash
// does (TRAIL_GROUND), and a pass fades as it ages. Geometry is built once per
// hex in hex units (HEX_SZ = 1) into Path2Ds and cached. A frame then costs a
// handful of fills per tracked hex.

const TRAIL_STORE_KEY     = 'wl.trails.v1';
const TRAIL_SAVE_DELAY_MS = 5000;   // debounce: the caravan and every seat all step
const TRAIL_MAX_PASSES    = 6000;   // ~50 bytes each in storage
const TRAIL_MAX_PER_HEX   = 3;      // per walker per hex; the newest are kept
const TRAIL_CACHE_MAX     = 600;    // built hex geometries
const TRAIL_RANK_MS       = 250;    // how long a walker's trail ranking is reused
const TRAIL_RANK_MAX      = 900;    // hexes walked back along one trail, at most
// The server's trail-sides bytes (trails.hpp): slot 6 is the caravan's, and
// bit 6 marks a step made riding.
const TRAIL_CARAVAN_SLOT  = 6;
const TRAIL_RIDDEN_BIT    = 0x40;

// Flat-top axial, in the firmware's DQ/DR order (Esp32HexMapCrawl.ino), so a
// side index d here is the same d as the tunnel `op` bits.
const TRAIL_DQ = [1, 1, 0, -1, -1, 0];
const TRAIL_DR = [0, -1, -1, 0, 1, 1];
const TRAIL_APOTHEM = SQRT3 / 2;    // centre to a side's midpoint, hex units
// Unit vector from a hex's centre through the middle of side d (screen, y down).
const TRAIL_SIDE = TRAIL_DQ.map((dq, d) => {
  const x = 1.5 * dq, y = SQRT3 / 2 * dq + SQRT3 * TRAIL_DR[d];
  const l = Math.hypot(x, y);
  return { x: x / l, y: y / l };
});

// Where a trail crosses a side, as an offset along it (the side is 1 long).
// Feet wander across most of it; a vehicle's ruts already span a third of it.
const TRAIL_FOOT_EDGE  = 0.5;
const TRAIL_WHEEL_EDGE = 0.18;

// Print size: a share of HEX_SZ, never under a few pixels, or the sole stops
// reading as a sole. The tread inside it only shows once there is room.
const TRAIL_PRINT        = 0.10;
const TRAIL_PRINT_MIN_PX = 5.2;
const TRAIL_DETAIL_PX    = 7;
const TRAIL_WHEEL_DETAIL = 40;      // HEX_SZ from which tyre tread is drawn
// Ruts are wide and continuous, so at a print's opacity they read far darker
// than a line of prints does: drawn at this share of it.
const TRAIL_WHEEL_ALPHA  = 0.525;

// Age buckets (ms): fresh, recent, old, stale. Every inferred pass is stale --
// it was laid before this browser was watching.
const TRAIL_AGE_MS    = [10 * 60e3, 60 * 60e3, 6 * 3600e3];
const TRAIL_AGE_ALPHA = [1, 0.9, 0.8, 0.72];
// How hard a print was pressed in: a scuff, a print, a deep one.
const TRAIL_DEPTH_ALPHA = [0.55, 0.82, 1];

// The ground. keep: share of prints it holds. a: the impression's opacity.
// dark / rim: rgb of the impression and of the lit lip of soil around it.
const _TG = (keep, a, dark, rim, rimA) => ({ keep, a, dark, rim, rimA });
const TRAIL_GROUND = [
  _TG(0.85, 0.78, '24,15,6',  '255,226,180', 0.22),  //  0 Open Scrub: cracked earth
  _TG(1.00, 0.82, '16,15,13', '226,222,210', 0.24),  //  1 Ash Dunes: soft ash holds everything
  _TG(0.65, 0.70, '20,12,4',  '230,180,130', 0.16),  //  2 Rust Forest: leaf litter
  _TG(0.95, 0.80, '2,12,10',  '160,200,180', 0.20),  //  3 Marsh: the prints fill with water
  _TG(0.50, 0.68, '20,18,16', '230,225,215', 0.20),  //  4 Broken Urban: dust on concrete
  _TG(0.35, 0.55, '4,12,20',  '160,190,215', 0.16),  //  5 Flooded District: silt, mostly washed out
  _TG(0.65, 0.70, '10,16,16', '210,240,235', 0.24),  //  6 Glass Fields
  _TG(0.55, 0.72, '22,14,6',  '240,215,180', 0.20),  //  7 Ridge: scree
  _TG(0.40, 0.65, '18,13,8',  '240,220,200', 0.20),  //  8 Mountain: bare rock
  _TG(0.80, 0.75, '24,15,6',  '255,226,180', 0.20),  //  9 Settlement: trampled
  _TG(0.60, 0.70, '14,16,6',  '220,240,170', 0.20),  // 10 Nuke Crater
  _TG(0.30, 0.50, '4,12,20',  '160,190,215', 0.14),  // 11 River Channel: only a raft gets here
  _TG(0.75, 0.72, '20,15,10', '230,215,190', 0.20),  // 12 Bunker Entrance
  _TG(0.75, 0.72, '20,15,10', '230,215,190', 0.20),  // 13 Vent Shaft
  _TG(0.90, 0.78, '16,13,10', '220,210,190', 0.20),  // 14 Tunnel Floor: decades of dust
  _TG(0.50, 0.65, '16,13,10', '220,210,190', 0.16),  // 15 Collapsed Tunnel
];

// One sole per seat (seat = archetype, ARCHETYPES order). len / wid: print
// size; stride: one step, in print lengths; gait: how far either side of the
// line of travel the feet fall. heel: a boot's separate heel block (the arch
// leaves no mark). tread: what is pressed into the print when it has room.
const TRAIL_SOLES = [
  { len: 1.00, wid: 1.00, stride: 3.0, gait: 1.00, heel: true,  tread: 'lugs'    },              // 0 Guide: hiking boots
  { len: 1.06, wid: 0.96, stride: 2.9, gait: 0.90, heel: true,  tread: 'chevron' },              // 1 Quartermaster: combat boots
  { len: 0.94, wid: 0.92, stride: 3.0, gait: 1.00, heel: false, tread: 'dots'    },              // 2 Medic: trainers
  { len: 1.16, wid: 1.18, stride: 2.5, gait: 1.25, heel: true,  tread: 'blocks', deep: true },   // 3 Mule: work boots, short heavy steps under load
  { len: 0.98, wid: 0.88, stride: 3.5, gait: 0.80, heel: false, tread: null, soft: true },       // 4 Scout: rag-wrapped feet, a long light stride
  { len: 0.92, wid: 0.95, stride: 2.9, gait: 1.00, bare: true,  tread: null },                   // 5 Endurer: bare feet
];

// Sole outlines, right foot, one print long: u runs heel (-0.5) to toe (0.5),
// v is the outside of the foot. Mirrored for the left.
const TRAIL_SOLE_FORE = [[0.50, 0.00], [0.46, 0.11], [0.35, 0.185], [0.20, 0.20], [0.05, 0.175], [-0.08, 0.15],
                         [-0.09, -0.10], [0.05, -0.14], [0.20, -0.18], [0.35, -0.17], [0.46, -0.10]];
const TRAIL_SOLE_HEEL = [[-0.24, 0.14], [-0.36, 0.145], [-0.47, 0.11], [-0.51, 0.00], [-0.47, -0.10],
                         [-0.36, -0.135], [-0.24, -0.12]];
const TRAIL_SOLE_WHOLE = [[0.50, 0.00], [0.46, 0.11], [0.34, 0.18], [0.18, 0.19], [0.02, 0.15], [-0.16, 0.12],
                          [-0.32, 0.13], [-0.45, 0.10], [-0.50, 0.00], [-0.45, -0.10], [-0.32, -0.12],
                          [-0.16, -0.075], [0.02, -0.12], [0.18, -0.17], [0.34, -0.165], [0.46, -0.10]];
const TRAIL_SOLE_WRAP = [[0.48, 0.00], [0.40, 0.13], [0.20, 0.18], [-0.05, 0.16], [-0.30, 0.15], [-0.46, 0.08],
                         [-0.49, -0.04], [-0.36, -0.13], [-0.10, -0.12], [0.15, -0.16], [0.38, -0.12]];
// A bare foot: ball, outer edge and heel, the arch lifted clear of the
// ground on the inside -- and the toes, each its own little pit.
const TRAIL_SOLE_BARE = [[0.30, 0.17], [0.16, 0.185], [0.00, 0.15], [-0.20, 0.135], [-0.38, 0.125], [-0.48, 0.06],
                         [-0.49, -0.04], [-0.40, -0.10], [-0.28, -0.08], [-0.14, -0.02], [0.02, -0.07],
                         [0.18, -0.15], [0.30, -0.12], [0.35, 0.02]];
const TRAIL_TOES = [[0.44, -0.10, 0.065], [0.45, -0.005, 0.045], [0.42, 0.07, 0.04], [0.38, 0.13, 0.035], [0.32, 0.18, 0.03]];
// Tread, as polygons in the same frame.
const _rect = (u, v, du, dv) => [[u - du, v - dv], [u + du, v - dv], [u + du, v + dv], [u - du, v + dv]];
const _vee  = (u) => [[u - 0.02, -0.12], [u + 0.06, 0.005], [u - 0.02, 0.13], [u - 0.06, 0.13], [u + 0.02, 0.005], [u - 0.06, -0.12]];
const TRAIL_TREAD = {
  lugs:    [_rect(0.08, 0.01, 0.022, 0.115), _rect(0.22, 0.01, 0.022, 0.13), _rect(0.36, 0.0, 0.022, 0.11),
            _rect(-0.33, 0.01, 0.022, 0.09), _rect(-0.43, 0.0, 0.022, 0.07)],
  chevron: [_vee(0.04), _vee(0.19), _vee(0.34), _rect(-0.37, 0.005, 0.03, 0.09)],
  dots:    [_rect(0.10, -0.07, 0.02, 0.02), _rect(0.10, 0.08, 0.02, 0.02), _rect(0.25, -0.09, 0.02, 0.02),
            _rect(0.25, 0.09, 0.02, 0.02), _rect(0.38, 0.0, 0.02, 0.02), _rect(-0.36, -0.05, 0.02, 0.02),
            _rect(-0.36, 0.06, 0.02, 0.02)],
  blocks:  [_rect(0.10, -0.08, 0.04, 0.04), _rect(0.10, 0.09, 0.04, 0.04), _rect(0.30, -0.075, 0.04, 0.04),
            _rect(0.30, 0.08, 0.04, 0.04), _rect(-0.36, -0.055, 0.045, 0.045), _rect(-0.36, 0.065, 0.045, 0.045)],
};

function _trailHash(a, b, c) {
  let h = Math.imul(a | 0, 374761393) ^ Math.imul(b | 0, 668265263) ^ Math.imul(c | 0, 1274126177);
  h = Math.imul(h ^ (h >>> 15), 2246822519);
  h = Math.imul(h ^ (h >>> 13), 3266489917);
  return (h ^ (h >>> 16)) >>> 0;
}
// mulberry32: every print's jitter comes from its pass's seed, so a hex draws
// the same tracks every time its geometry is rebuilt.
function _trailRng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s + 0x6D2B79F5) | 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const _trailSeed = () => (Math.random() * 4294967296) >>> 0;

// Which side of surface hex (q0, r0) leads to (q1, r1); -1 when they are not
// neighbours. Recording is surface only, so this always wraps.
function _trailSideTo(q0, r0, q1, r1) {
  for (let d = 0; d < 6; d++) {
    if ((((q0 + TRAIL_DQ[d]) % MAP_COLS) + MAP_COLS) % MAP_COLS === q1 &&
        (((r0 + TRAIL_DR[d]) % MAP_ROWS) + MAP_ROWS) % MAP_ROWS === r1) return d;
  }
  return -1;
}

// The point where a trail crosses side d, off along it, hex units from the centre.
function _trailEdge(d, off) {
  const u = TRAIL_SIDE[d];
  return { x: u.x * TRAIL_APOTHEM - u.y * off, y: u.y * TRAIL_APOTHEM + u.x * off };
}

// A quadratic curve sampled by arc length: [{x, y, tx, ty, s}], unit tangents.
function _trailSample(cv, n) {
  const pts = [];
  const fx = cv.p2.x - cv.p0.x, fy = cv.p2.y - cv.p0.y, fl = Math.hypot(fx, fy) || 1;
  let s = 0, lx = cv.p0.x, ly = cv.p0.y;
  for (let j = 0; j <= n; j++) {
    const u = j / n, a = (1 - u) * (1 - u), b = 2 * (1 - u) * u, c = u * u;
    const x = a * cv.p0.x + b * cv.p1.x + c * cv.p2.x;
    const y = a * cv.p0.y + b * cv.p1.y + c * cv.p2.y;
    let tx = 2 * (1 - u) * (cv.p1.x - cv.p0.x) + 2 * u * (cv.p2.x - cv.p1.x);
    let ty = 2 * (1 - u) * (cv.p1.y - cv.p0.y) + 2 * u * (cv.p2.y - cv.p1.y);
    const tl = Math.hypot(tx, ty);
    if (tl < 1e-6) { tx = fx / fl; ty = fy / fl; } else { tx /= tl; ty /= tl; }
    if (j) s += Math.hypot(x - lx, y - ly);
    pts.push({ x, y, tx, ty, s });
    lx = x; ly = y;
  }
  return pts;
}
function _trailAt(pts, s) {
  let j = 0;
  while (j < pts.length - 2 && pts[j + 1].s < s) j++;
  const a = pts[j], b = pts[j + 1];
  const k = b.s > a.s ? Math.min(1, Math.max(0, (s - a.s) / (b.s - a.s))) : 0;
  const tx = a.tx + (b.tx - a.tx) * k, ty = a.ty + (b.ty - a.ty) * k, tl = Math.hypot(tx, ty) || 1;
  return { x: a.x + (b.x - a.x) * k, y: a.y + (b.y - a.y) * k, tx: tx / tl, ty: ty / tl };
}

// A closed blob through pts (quadratics through the midpoints), wound one way
// whatever the mirroring: two prints that overlap must add, never cut a hole.
function _trailArea(pts) {
  let area = 0;
  for (let i = 0, n = pts.length; i < n; i++) {
    const p = pts[i], q = pts[(i + 1) % n];
    area += p.x * q.y - q.x * p.y;
  }
  return area;
}
function _trailBlob(path, pts) {
  if (_trailArea(pts) < 0) pts.reverse();
  const n = pts.length;
  const z = pts[n - 1], f = pts[0];
  path.moveTo((z.x + f.x) / 2, (z.y + f.y) / 2);
  for (let i = 0; i < n; i++) {
    const p = pts[i], q = pts[(i + 1) % n];
    path.quadraticCurveTo(p.x, p.y, (p.x + q.x) / 2, (p.y + q.y) / 2);
  }
  path.closePath();
}
function _trailPoly(path, pts) {
  if (_trailArea(pts) < 0) pts.reverse();
  path.moveTo(pts[0].x, pts[0].y);
  for (let i = 1; i < pts.length; i++) path.lineTo(pts[i].x, pts[i].y);
  path.closePath();
}

class TrailField {
  constructor() {
    this.passes = new Map();   // "q_r" -> [pass], surface hexes only
    this.last   = new Map();   // "f3" / "v0" / "b1" -> {q, r}: that walker's last sighting
    this.count  = 0;
    this.serial = 0;           // bumps whenever a pass changes, for the geometry key
    this.geo    = new Map();   // signature -> built geometry
    this._ranks = new Map();   // walker -> {t, at}, see _ranker()
    this._saveTimer = null;
    this._load();
  }

  // ── Recording ──────────────────────────────────────────────────
  // kind: 'f' on foot (id = seat), 'v' the caravan's convoy, 'b' a ridden
  // bike (id = seat). The walker is now on surface hex (q, r). side: the side
  // the server says it stepped through (the mv event's "d"), which settles
  // where it came from. Without one, hq, hr: where the caller believes it just
  // was, used only when this field's own last sighting is not next door. That
  // sighting wins when it is: a state broadcast can move players[] on ahead of
  // the mv event that matters.
  step(kind, id, q, r, hq, hr, side = -1) {
    const tag  = kind + id;
    const prev = this.last.get(tag);
    if (prev && prev.q === q && prev.r === r) return;
    this.last.set(tag, { q, r });
    let d = -1, fq, fr;
    if (side >= 0 && side < 6) {
      d  = side;
      fq = (((q - TRAIL_DQ[side]) % MAP_COLS) + MAP_COLS) % MAP_COLS;
      fr = (((r - TRAIL_DR[side]) % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
    } else if (prev) {
      d  = _trailSideTo(prev.q, prev.r, q, r);
      fq = prev.q; fr = prev.r;
    }
    if (d < 0 && Number.isFinite(hq) && Number.isFinite(hr)) {
      d = _trailSideTo(hq, hr, q, r);
      fq = hq; fr = hr;
    }
    const now = Date.now();
    if (d < 0) {
      // A respawn, a surfacing, the first sighting of a fresh page: the
      // trail starts here.
      this._push(q, r, this._pass(kind, id, -1, 0, now));
    } else {
      const off  = (Math.random() - 0.5) * (kind === 'f' ? TRAIL_FOOT_EDGE : TRAIL_WHEEL_EDGE);
      const open = this._openPass(fq, fr, tag);
      if (open) { open.o = d; open.ot = off; open.n = ++this.serial; }
      else      this._push(fq, fr, { ...this._pass(kind, id, -1, 0, now), o: d, ot: off });
      // The same crossing point seen from the far side: the side runs the
      // other way round from there, so the offset changes sign.
      this._push(q, r, this._pass(kind, id, (d + 3) % 6, -off, now));
    }
    this._scheduleSave();
  }

  // The walker has left the surface board (down a hatch), or is somebody new
  // in that seat: its next step starts a new trail rather than joining this one.
  lose(kind, id) { this.last.delete(kind + id); }

  clear() {
    this.passes.clear();
    this.last.clear();
    this.geo.clear();
    this.count = 0;
    try { localStorage.removeItem(TRAIL_STORE_KEY); } catch { /* storage blocked: nothing to forget */ }
  }

  _pass(k, id, i, it, t) {
    return { k, id, i, it, o: -1, ot: 0, t, s: _trailSeed(), n: ++this.serial };
  }

  _openPass(q, r, tag) {
    const arr = this.passes.get(`${q}_${r}`);
    if (!arr) return null;
    for (let j = arr.length - 1; j >= 0; j--) {
      const p = arr[j];
      if (p.o < 0 && p.k + p.id === tag) return p;
    }
    return null;
  }

  _push(q, r, pass) {
    const key = `${q}_${r}`;
    let arr = this.passes.get(key);
    if (!arr) { arr = []; this.passes.set(key, arr); }
    arr.push(pass);
    this.count++;
    const tag = pass.k + pass.id;
    let mine = 0;
    for (const p of arr) if (p.k + p.id === tag) mine++;
    if (mine > TRAIL_MAX_PER_HEX) {
      arr.splice(arr.findIndex((p) => p.k + p.id === tag), 1);
      this.count--;
    }
    if (this.count > TRAIL_MAX_PASSES) this._prune();
  }

  // Drop the oldest tenth. Rare: the cap is weeks of play.
  _prune() {
    const all = [];
    for (const arr of this.passes.values()) for (const p of arr) all.push(p.t);
    all.sort((a, b) => a - b);
    const cut = all[Math.floor(all.length / 10)];
    this.count = 0;
    for (const [key, arr] of this.passes) {
      const keep = arr.filter((p) => p.t > cut);
      if (keep.length) { this.passes.set(key, keep); this.count += keep.length; }
      else this.passes.delete(key);
    }
  }

  _scheduleSave() {
    if (this._saveTimer) return;
    this._saveTimer = setTimeout(() => { this._saveTimer = null; this._save(); }, TRAIL_SAVE_DELAY_MS);
  }

  _save() {
    const rows = [];
    for (const [key, arr] of this.passes) {
      for (const p of arr) rows.push([key, p.k, p.id, p.i, Math.round(p.it * 1000), p.o, Math.round(p.ot * 1000), p.t, p.s]);
    }
    try { localStorage.setItem(TRAIL_STORE_KEY, JSON.stringify(rows)); }
    catch (e) { console.warn('[TRAIL] not saved:', e?.name || e); }
  }

  _load() {
    let rows = null;
    // Same stance as the explored map: blocked or corrupt storage costs the
    // remembered directions, never the client.
    try { rows = JSON.parse(localStorage.getItem(TRAIL_STORE_KEY) || 'null'); } catch { return; }
    if (!Array.isArray(rows)) return;
    const side = (v) => Number.isInteger(v) && v >= -1 && v < 6;
    for (const row of rows) {
      if (!Array.isArray(row) || row.length < 9) continue;
      const [key, k, id, i, it, o, ot, t, s] = row;
      if (typeof key !== 'string' || !['f', 'v', 'b'].includes(k) || !Number.isInteger(id) ||
          id < 0 || id >= 6 || !side(i) || !side(o) || !Number.isFinite(t)) continue;
      let arr = this.passes.get(key);
      if (!arr) { arr = []; this.passes.set(key, arr); }
      arr.push({ k, id, i, it: (it | 0) / 1000, o, ot: (ot | 0) / 1000, t, s: s >>> 0, n: ++this.serial });
      this.count++;
    }
    if (this.count) console.log('%c[TRAIL] restored', 'color:#09f', `${this.count} passes`);
  }

  // ── Reading a hex ──────────────────────────────────────────────
  // Every trail segment on (q, r) of the board being drawn:
  // [{k, id, i, it, o, ot, s, n, age}], i/o the side it came in and left by,
  // -1 for "started here" / "stopped here".
  _segments(cell, q, r, fp, tt) {
    const rec = myDepth ? null : this.passes.get(`${q}_${r}`);
    const now = Date.now();
    const out = [];
    if (tt) this._walker(out, rec, cell, q, r, 'v', 0, now);
    for (let p = 0; p < 6; p++) if (fp & (1 << p)) this._walker(out, rec, cell, q, r, 'f', p, now);
    return out;
  }

  _walker(out, rec, cell, q, r, kind, id, now) {
    const veh  = kind !== 'f';
    const ours = veh ? (p) => p.k !== 'f' : (p) => p.k === 'f' && p.id === id;
    const salt = veh ? 9 : id + 1;
    const conns = this._links(cell, q, r, this._mark(kind, id));
    const rank  = this._ranker(kind, id);
    const here  = rank.at(q, r);

    // What is known of this hex: the passes this browser watched, then the
    // server's latest pass (trails.hpp) unless it is one of those. On foot, a
    // ridden step is the rut's to draw -- it only says the hex is accounted for.
    const served = this._served(cell, kind, id);
    // A watched pass that "starts here" only because this page first saw the
    // walker here is the tail of the served one that came in: drop it for that.
    const tail   = (p) => p.i < 0 && served.some((sp) => sp.k === p.k && sp.id === p.id && sp.o === p.o && sp.i >= 0);
    const mine   = rec ? rec.filter((p) => ours(p) && !tail(p)) : [];
    const rode   = !veh && rec ? rec.filter((p) => p.k === 'b' && p.id === id && !tail(p)) : [];
    const used = new Set();
    for (const p of mine.concat(rode)) {
      used.add(p.i); used.add(p.o);
      if (p.k === 'f' || veh) {
        out.push({ k: p.k, id: p.id, i: p.i, it: p.it, o: p.o, ot: p.ot, s: p.s, n: p.n, age: this._age(now - p.t) });
      }
    }
    for (const sp of served) {
      used.add(sp.i); used.add(sp.o);
      if (sp.cover || mine.some((p) => p.k === sp.k && p.id === sp.id && p.i === sp.i && p.o === sp.o)) continue;
      out.push(this._inferred(sp.k, sp.id, q, r, sp.i, sp.o, salt, `s${sp.b}`, 2));
    }
    if (mine.length || rode.length || served.length) {
      // A neighbour on the same trail that nothing here explains was walked
      // before anyone was keeping sides -- unless that hex has a record of its
      // own, which then says the trail never came this way.
      for (const c of conns) {
        if (used.has(c.d)) continue;
        const nc = boardCells()[c.n.r][c.n.q];
        if (!myDepth && this.passes.get(`${c.n.q}_${c.n.r}`)?.some(ours)) continue;
        if (this._served(nc, kind, id).length) continue;
        out.push(rank.at(c.n.q, c.n.r) < here ? this._inferred(kind, id, q, r, -1, c.d, salt)
                                              : this._inferred(kind, id, q, r, c.d, -1, salt));
      }
      return;
    }

    if (!conns.length) { out.push(this._inferred(kind, id, q, r, -1, -1, salt)); return; }
    // The way out is the neighbour nearest the maker. The trail only ends
    // here if this is where the maker stands, or it is the trail's one end.
    let exit = null, best = Infinity;
    for (const c of conns) {
      const v = rank.at(c.n.q, c.n.r);
      if (v < best) { best = v; exit = c; }
    }
    if (best >= here && (here === 0 || conns.length === 1)) exit = null;
    const ins = conns.filter((c) => c !== exit).slice(0, 3);
    if (!ins.length) out.push(this._inferred(kind, id, q, r, -1, exit.d, salt));
    for (const c of ins) out.push(this._inferred(kind, id, q, r, c.d, exit ? exit.d : -1, salt));
  }

  // The server's latest pass(es) on a cell for this walker, from cell.trl
  // (map-decoder.js applyTrailRecs): [{k, id, i, o, b, cover}], sides -1 for
  // none. On foot that is the seat's byte -- a ridden one only covers, since
  // the rut draws it, unless the cell has no rut to draw it in. For the ruts:
  // the caravan's byte, and every seat's ridden one.
  _served(cell, kind, id) {
    const t = myDepth ? null : cell?.trl;
    if (!t) return [];
    const pass = (k, pid, b, cover) => {
      const i = b & 7, o = (b >> 3) & 7;
      if (b === 0xFF || (b & 0x80) || (i > 5 && i !== 7) || (o > 5 && o !== 7)) return null;
      return { k, id: pid, i: i === 7 ? -1 : i, o: o === 7 ? -1 : o, b, cover };
    };
    const out = [];
    if (kind === 'f') {
      const b = t[id];
      if (b === undefined) return out;
      const ridden = (b & TRAIL_RIDDEN_BIT) && cell.tireTrack;
      const p = pass(ridden ? 'b' : 'f', id, b, !!ridden);
      if (p) out.push(p);
      return out;
    }
    if (t[TRAIL_CARAVAN_SLOT] !== undefined) {
      const p = pass('v', 0, t[TRAIL_CARAVAN_SLOT], false);
      if (p) out.push(p);
    }
    for (let s = 0; s < TRAIL_CARAVAN_SLOT; s++) {
      if (t[s] === undefined || !(t[s] & TRAIL_RIDDEN_BIT)) continue;
      const p = pass('b', s, t[s], false);
      if (p) out.push(p);
    }
    return out;
  }

  // Does a cell carry this walker's mark.
  _mark(kind, id) {
    return kind === 'f' ? (c) => !!(c && (c.footprints & (1 << id))) : (c) => !!c?.tireTrack;
  }

  // The sides of (q, r) whose neighbour carries the mark: [{d, n}]. Underground
  // a corridor wall is no way through, whatever is on the far side of it.
  _links(cell, q, r, has) {
    const cells = boardCells();
    const out = [];
    for (let d = 0; d < 6; d++) {
      if (myDepth && cell.op != null && !((cell.op >> d) & 1)) continue;
      const n = boardNorm(q + TRAIL_DQ[d], r + TRAIL_DR[d]);
      if (n && has(cells[n.r][n.q])) out.push({ d, n });
    }
    return out;
  }

  // How far a hex is from whoever made the trail, which is what points an
  // inferred trail: lower is nearer. Counted in steps along the trail itself
  // wherever it is joined up to the maker's hex, so a trail that winds still
  // points one way all along; a stretch the trail does not join up to ranks
  // behind all of that, as the crow flies. A maker nobody can see (an offline
  // seat, a caravan off the road) is replaced by a fixed point per walker --
  // the sense is a guess then, but at least the same guess hex to hex.
  // Rebuilt at most every TRAIL_RANK_MS: the maker moves a hex at a time.
  _ranker(kind, id) {
    const key = (kind === 'f' ? id : 9) + (myDepth ? 16 : 0);
    const now = performance.now();
    const memo = this._ranks.get(key);
    if (memo && now - memo.t < TRAIL_RANK_MS) return memo;
    const cols = boardCols(), rows = boardRows(), cells = boardCells();
    let maker = this._maker(kind, id);
    const real = !!maker;
    if (!maker) maker = { q: _trailHash(id, kind === 'f' ? 1 : 2, 77) % cols, r: _trailHash(id, 3, 78) % rows };
    const steps = new Map();   // q + r*cols -> steps from the maker
    const has = this._mark(kind, id);
    const start = cells[maker.r]?.[maker.q];
    if (real && has(start)) {
      steps.set(maker.r * cols + maker.q, 0);
      const queue = [maker];
      for (let h = 0; h < queue.length && steps.size < TRAIL_RANK_MAX; h++) {
        const c = queue[h], sc = steps.get(c.r * cols + c.q);
        for (const { n } of this._links(cells[c.r][c.q], c.q, c.r, has)) {
          const k = n.r * cols + n.q;
          if (steps.has(k)) continue;
          steps.set(k, sc + 1);
          queue.push(n);
        }
      }
    }
    const res = {
      t: now,
      at: (q, r) => steps.get(r * cols + q) ?? TRAIL_RANK_MAX + boardDist(q, r, maker.q, maker.r),
    };
    this._ranks.set(key, res);
    return res;
  }

  // Where the trail's maker is now, if anyone can see: a seated survivor
  // (underground, the hatch they went down is where a surface trail leads),
  // or the caravan.
  _maker(kind, id) {
    if (kind !== 'f') {
      const c = worldState?.caravan;
      return !myDepth && c?.active ? { q: c.q, r: c.r } : null;
    }
    const p = players[id];
    if (!p?.on) return null;
    if (myDepth) return p.dp ? { q: p.tq, r: p.tr } : null;
    return { q: p.q, r: p.r };
  }

  _age(ms) {
    let b = 0;
    while (b < TRAIL_AGE_MS.length && ms > TRAIL_AGE_MS[b]) b++;
    return b;
  }

  // A segment this browser did not watch being made -- inferred, or the
  // server's (n, age passed in) -- everything about it hashed from the hex so
  // it is the same segment every frame. Its crossing points are hashed from
  // the side itself, so the hex across the side puts the trail in the same place.
  _inferred(kind, id, q, r, i, o, salt, n = 'x', age = 3) {
    const range = kind === 'f' ? TRAIL_FOOT_EDGE : TRAIL_WHEEL_EDGE;
    return {
      k: kind, id, i, o,
      it: i >= 0 ? this._edgeOff(q, r, i, salt, range) : 0,
      ot: o >= 0 ? this._edgeOff(q, r, o, salt, range) : 0,
      s: _trailHash(q * 131 + i + 1, r * 137 + o + 1, salt), n, age,
    };
  }

  _edgeOff(q, r, d, salt, range) {
    const n = boardNorm(q + TRAIL_DQ[d], r + TRAIL_DR[d]);
    if (!n) return 0;
    const cols = boardCols();
    const a = r * cols + q, b = n.r * cols + n.q;
    const lo = a < b ? a : b, side = a < b ? d : (d + 3) % 6, sign = a < b ? 1 : -1;
    return sign * (_trailHash(lo, side, salt) / 4294967296 - 0.5) * range;
  }

  // ── Drawing ─────────────────────────────────────────────────────
  // cx, cy: the hex centre on screen; S: HEX_SZ. Drawn over the terrain art
  // and under everything else on the hex (renderHexContent).
  drawHex(ctx, cx, cy, cell, q, r, S) {
    const fp = cell.footprints & 0x3F;
    const tt = !myDepth && cell.tireTrack;
    if (!fp && !tt) return;
    const segs = this._segments(cell, q, r, fp, tt);
    if (!segs.length) return;
    // Machine Gun Kane's step still being driven (kane-jeep.js): its ruts stop
    // under the jeep.
    const cut = tt && typeof kaneTrackCut === 'function' ? kaneTrackCut(q, r) : null;
    let sig = `${myDepth}_${q}_${r}_${cell.terrain}_${Math.round(S * 4)}`;
    for (const g of segs) sig += `|${g.k}${g.id}.${g.i}.${g.o}.${g.n}.${g.age}`;
    if (cut) sig += `@${cut.x}_${cut.y}_${cut.i}_${cut.o}`;
    let geo = this.geo.get(sig);
    if (!geo) {
      geo = this._build(segs, cell.terrain, S, cut);
      this.geo.set(sig, geo);
      if (this.geo.size > TRAIL_CACHE_MAX) this.geo.delete(this.geo.keys().next().value);
    }
    const G = geo.ground;
    const dark = `rgb(${G.dark})`, rim = `rgb(${G.rim})`;
    ctx.save();
    ctx.translate(cx, cy);
    ctx.scale(S, S);
    for (const l of geo.groups) {
      const A = TRAIL_AGE_ALPHA[l.age] * (l.wheel ? TRAIL_WHEEL_ALPHA : 1);
      ctx.fillStyle   = rim;
      ctx.globalAlpha = G.rimA * A;
      ctx.fill(l.rim);
      ctx.fillStyle = dark;
      for (let b = 0; b < 3; b++) {
        if (!l.used[b]) continue;
        ctx.globalAlpha = G.a * A * TRAIL_DEPTH_ALPHA[b];
        ctx.fill(l.dark[b]);
      }
      if (l.used[3]) {
        ctx.globalAlpha = Math.min(1, G.a * A * 1.3);
        ctx.fill(l.deep);
      }
    }
    ctx.restore();
  }

  // cut: kaneTrackCut's answer for this hex, or null.
  _build(segs, terrain, S, cut = null) {
    const ground = TRAIL_GROUND[terrain] || TRAIL_GROUND[0];
    const driving = (sg) => cut && sg.k === 'v' && ((cut.o >= 0 && sg.o === cut.o) || (cut.i >= 0 && sg.i === cut.i));
    const groups = new Map();   // age bucket + feet/wheels -> paths
    for (const sg of segs) {
      const wheel = sg.k !== 'f', key = sg.age * 2 + (wheel ? 1 : 0);
      let g = groups.get(key);
      if (!g) {
        g = { age: sg.age, wheel, rim: new Path2D(), dark: [new Path2D(), new Path2D(), new Path2D()], deep: new Path2D(), used: [0, 0, 0, 0] };
        groups.set(key, g);
      }
      const rng = _trailRng(sg.s);
      if (sg.k === 'f') this._feet(g, sg, rng, ground, S);
      else              this._wheels(g, sg, rng, ground, S, driving(sg) ? cut : null);
    }
    return { ground, groups: [...groups.values()] };
  }

  // The line a segment follows: in at one side, out at another, bending
  // through the middle of the hex the way a walker cuts a corner. null when
  // it neither comes in nor goes out (somebody who only ever stood here).
  _curve(sg, rng) {
    const centre = () => ({ x: (rng() - 0.5) * 0.3, y: (rng() - 0.5) * 0.3 });
    if (sg.i < 0 && sg.o < 0) return null;
    let it = sg.it, ot = sg.ot;
    // In and back out the same side: keep the two legs of the turn apart.
    if (sg.i >= 0 && sg.i === sg.o && Math.abs(it - ot) < 0.14) {
      const m = (it + ot) / 2, sp = m > 0 ? -0.09 : 0.09;
      it = m + sp; ot = m - sp;
    }
    const p0 = sg.i >= 0 ? _trailEdge(sg.i, it) : centre();
    const p2 = sg.o >= 0 ? _trailEdge(sg.o, ot) : centre();
    const mx = (p0.x + p2.x) / 2, my = (p0.y + p2.y) / 2;
    // How far the bend is drawn in toward the centre, by how sharp the turn is
    // (0 = back out the same side, 3 = straight through).
    const turn = Math.min((sg.i - sg.o + 6) % 6, (sg.o - sg.i + 6) % 6);
    const pull = sg.i >= 0 && sg.o >= 0 ? [0.7, 0.5, 0.6, 0.5][turn] : 0;
    return {
      p0, p2,
      p1: { x: mx * (1 - pull) + (rng() - 0.5) * 0.16, y: my * (1 - pull) + (rng() - 0.5) * 0.16 },
    };
  }

  // ── Feet ──
  _feet(g, sg, rng, G, S) {
    const sole = TRAIL_SOLES[sg.id] || TRAIL_SOLES[0];
    const L    = Math.max(TRAIL_PRINT_MIN_PX, TRAIL_PRINT * S) * sole.len / S;
    const pr   = { g, sole, L, rng, G, rim: 0.8 / S, detail: L * S >= TRAIL_DETAIL_PX };
    const gait = L * 0.5 * sole.gait;
    const cv   = this._curve(sg, rng);
    if (cv) {
      const pts    = _trailSample(cv, 32);
      const total  = pts[pts.length - 1].s;
      const stride = L * sole.stride;
      let foot = rng() < 0.5 ? 1 : -1;
      for (let x = rng() * stride; x < total; x += stride * (0.88 + rng() * 0.24), foot = -foot) {
        const at  = _trailAt(pts, x);
        const lat = foot * gait + (rng() - 0.5) * 0.22 * L;
        const fwd = (rng() - 0.5) * 0.18 * L;
        const px  = at.x - at.ty * lat + at.tx * fwd;
        const py  = at.y + at.tx * lat + at.ty * fwd;
        // Right foot toes out clockwise (y is down), the left the other way.
        const ang = Math.atan2(at.ty, at.tx) + foot * (0.11 + (rng() - 0.5) * 0.1) + (rng() - 0.5) * 0.1;
        const held = rng();
        if (held < G.keep) this._print(pr, px, py, ang, foot, held < G.keep * 0.12);
        else if (held < G.keep + (1 - G.keep) * 0.35) this._print(pr, px, py, ang, foot, true);   // the ground kept a trace
        // A foot dragged between steps: tired, loaded, or wrapped in rags.
        if (rng() < (sole.soft ? 0.14 : sole.deep ? 0.07 : 0.03)) this._drag(pr, at, lat, stride);
      }
    }
    // Where a trail starts or stops, the walker stood: two feet side by side,
    // a shuffle or two around them. Somebody who only ever stood here gets
    // two stances and the shuffling between.
    const stands = [];
    if (!cv) stands.push({ x: (rng() - 0.5) * 0.3, y: (rng() - 0.5) * 0.3, a: rng() * Math.PI * 2 },
                         { x: (rng() - 0.5) * 0.4, y: (rng() - 0.5) * 0.4, a: rng() * Math.PI * 2 });
    else {
      if (sg.i < 0) stands.push({ x: cv.p0.x, y: cv.p0.y, a: Math.atan2(cv.p1.y - cv.p0.y, cv.p1.x - cv.p0.x) + (rng() - 0.5) * 1.2 });
      if (sg.o < 0) stands.push({ x: cv.p2.x, y: cv.p2.y, a: Math.atan2(cv.p2.y - cv.p1.y, cv.p2.x - cv.p1.x) + (rng() - 0.5) * 1.2 });
    }
    for (const st of stands) {
      const nx = -Math.sin(st.a), ny = Math.cos(st.a);
      for (const foot of [1, -1]) {
        const w = gait * 1.15 * foot;
        this._print(pr, st.x + nx * w, st.y + ny * w, st.a + foot * 0.18 + (rng() - 0.5) * 0.15, foot, false);
      }
      for (let k = 1 + Math.floor(rng() * 2); k > 0; k--) {
        const ra = rng() * Math.PI * 2, rd = L * (0.7 + rng() * 0.6);
        this._print(pr, st.x + Math.cos(ra) * rd, st.y + Math.sin(ra) * rd, st.a + (rng() - 0.5) * 2.6,
                    rng() < 0.5 ? 1 : -1, rng() < 0.4);
      }
    }
  }

  // One print at (x, y) heading ang. foot: 1 right, -1 left. faint: only part
  // of the sole bit -- the heel strike, or the toe digging off.
  _print(pr, x, y, ang, foot, faint) {
    const { g, sole, L, rng, rim } = pr;
    const cos = Math.cos(ang), sin = Math.sin(ang);
    const wid = sole.wid * L * 1.2;   // the smoothing pulls the outline in this much
    const jit = sole.soft ? 0.1 : 0.045;
    // A faint continuous sole is the toe end of one, pressed in short.
    const uScale = faint && !sole.heel ? 0.62 : 1, uShift = faint && !sole.heel ? 0.2 : 0;
    const place = (tpl) => tpl.map(([u, v]) => {
      const uu = (u * uScale + uShift) * L * (1 + (rng() - 0.5) * jit);
      const vv = v * foot * wid * (1 + (rng() - 0.5) * jit * 1.6);
      return { x: x + uu * cos - vv * sin, y: y + uu * sin + vv * cos };
    });
    const depth = faint ? 0 : (sole.deep || rng() < 0.55 ? 2 : 1);
    const shapes = [];
    if (sole.bare) {
      shapes.push(place(TRAIL_SOLE_BARE));
      if (!faint || rng() < 0.5) {
        for (const [u, v, rt] of TRAIL_TOES) {
          if (rng() < 0.12) continue;   // a toe that never quite touched
          shapes.push(place([[u + rt, v], [u + rt * 0.3, v + rt * 0.9], [u - rt * 0.7, v + rt * 0.75],
                             [u - rt, v], [u - rt * 0.7, v - rt * 0.75], [u + rt * 0.3, v - rt * 0.9]]));
        }
      }
    } else if (sole.heel) {
      const which = faint ? (rng() < 0.5 ? 'heel' : 'fore') : 'both';
      if (which !== 'heel') shapes.push(place(TRAIL_SOLE_FORE));
      if (which !== 'fore') shapes.push(place(TRAIL_SOLE_HEEL));
    } else {
      shapes.push(place(sole.soft ? TRAIL_SOLE_WRAP : TRAIL_SOLE_WHOLE));
    }
    for (const pts of shapes) {
      _trailBlob(g.dark[depth], pts);
      _trailBlob(g.rim, pts.map((p) => ({ x: p.x + rim, y: p.y + rim })));
    }
    g.used[depth] = 1;
    if (pr.detail && sole.tread && !faint) {
      for (const tpl of TRAIL_TREAD[sole.tread]) {
        _trailPoly(g.deep, tpl.map(([u, v]) => {
          const uu = u * L, vv = v * foot * wid;
          return { x: x + uu * cos - vv * sin, y: y + uu * sin + vv * cos };
        }));
      }
      g.used[3] = 1;
    }
  }

  // A thin scuff where a foot dragged on through the gap between two prints.
  _drag(pr, at, lat, stride) {
    const { g, L, rng } = pr;
    const len = stride * (0.35 + rng() * 0.3), w = L * 0.07;
    const sx = at.x - at.ty * lat * 0.6 + at.tx * L * 0.5, sy = at.y + at.tx * lat * 0.6 + at.ty * L * 0.5;
    const nx = -at.ty * w, ny = at.tx * w;
    _trailBlob(g.dark[0], [
      { x: sx + nx, y: sy + ny }, { x: sx + at.tx * len * 0.5 + nx * 1.4, y: sy + at.ty * len * 0.5 + ny * 1.4 },
      { x: sx + at.tx * len, y: sy + at.ty * len }, { x: sx + at.tx * len * 0.5 - nx * 1.4, y: sy + at.ty * len * 0.5 - ny * 1.4 },
      { x: sx - nx, y: sy - ny },
    ]);
    g.used[0] = 1;
  }

  // ── Wheels ──
  // The convoy is an APC on wide singles with a cargo truck on duals behind
  // it, and the truck never runs quite in the APC's ruts: two pairs, almost
  // on top of each other, at different depths, so the ruts have doubled
  // edges. A bike is one narrow knobbly rut. Everyone's tread points the way
  // they were going.
  // cut: the jeep's position on this hex when this segment is a step Kane is
  // still driving -- the ruts are laid only as far along it as he has got.
  _wheels(g, sg, rng, G, S, cut = null) {
    let cv = this._curve(sg, rng);
    if (!cv) {
      // Parked and never seen moving: a short stretch of rut through the middle.
      const a = rng() * Math.PI * 2, dx = Math.cos(a) * 0.34, dy = Math.sin(a) * 0.34;
      cv = { p0: { x: -dx, y: -dy }, p1: { x: 0, y: 0 }, p2: { x: dx, y: dy } };
    }
    const pts    = _trailSample(cv, 40);
    const detail = S >= TRAIL_WHEEL_DETAIL;
    const px     = 1 / S;
    let lim = Infinity;
    if (cut) {
      let best = Infinity;
      for (const p of pts) {
        const dd = (p.x - cut.x) ** 2 + (p.y - cut.y) ** 2;
        if (dd < best) { best = dd; lim = p.s; }
      }
    }
    if (sg.k === 'b') {
      this._rut(g, pts, 0, Math.max(0.03, 1.5 * px), 1, 'knobs', rng, G, detail, px, lim);
      return;
    }
    const drift = (rng() - 0.5) * 0.05;
    for (const side of [-1, 1]) {
      this._rut(g, pts, side * 0.15,                 Math.max(0.066, 2.2 * px), 1, 'chevron', rng, G, detail, px, lim);   // APC
      this._rut(g, pts, side * 0.165 + drift,        Math.max(0.085, 2.6 * px), 0, 'bars',    rng, G, detail, px, lim);   // truck
    }
  }

  // One rut along pts, off to the right of the line by `off`, W wide. Broken
  // into lengths the ground did or did not hold, with a lip of thrown soil
  // either side and its tread pressed into the bed. Nothing past arc length
  // lim is laid, but the rng is drawn exactly as if it were, so the part that
  // is laid does not change as lim grows.
  _rut(g, pts, off, W, depth, tread, rng, G, detail, px, lim = Infinity) {
    const total = pts[pts.length - 1].s;
    // A driver's slow drift across the line, not a wobble.
    const ph1 = rng() * 6.283, ph2 = rng() * 6.283, amp = 0.003 + rng() * 0.004;
    const sway = (s) => amp * (Math.sin(s * 3.1 + ph1) + 0.5 * Math.sin(s * 7.3 + ph2));
    const at = (s) => {
      const a = _trailAt(pts, s), o = off + sway(s);
      return { x: a.x - a.ty * o, y: a.y + a.tx * o, tx: a.tx, ty: a.ty };
    };
    const hold = Math.min(1, G.keep + 0.3);
    const lip  = Math.max(0.012, 1.1 * px);
    for (let s0 = 0; s0 < total; ) {
      const len = 0.1 + rng() * 0.12, s1 = Math.min(total, s0 + len);
      if (rng() < hold) {
        const left = [], right = [], lipL = [], lipR = [];
        const n = Math.max(2, Math.ceil((s1 - s0) / 0.03));
        for (let j = 0; j <= n; j++) {
          const s = s0 + (s1 - s0) * j / n;
          const p = at(s);
          const w = W * (0.95 + rng() * 0.08) / 2;   // the bed is never a ruled line
          if (s > lim) continue;
          const nx = -p.ty, ny = p.tx;
          left.push({ x: p.x + nx * w, y: p.y + ny * w });
          right.push({ x: p.x - nx * w, y: p.y - ny * w });
          lipL.push({ x: p.x + nx * (w + lip), y: p.y + ny * (w + lip) });
          lipR.push({ x: p.x - nx * (w + lip), y: p.y - ny * (w + lip) });
        }
        if (left.length > 1) {
          const leftBack = left.slice().reverse(), rightBack = right.slice().reverse();
          _trailPoly(g.dark[depth], left.concat(rightBack));
          _trailPoly(g.rim, lipL.concat(leftBack));
          _trailPoly(g.rim, lipR.concat(rightBack));
          g.used[depth] = 1;
        }
        if (detail) this._tread(g, at, s0, s1, W, tread, rng, lim);
      }
      s0 = s1;
    }
  }

  _tread(g, at, s0, s1, W, tread, rng, lim = Infinity) {
    const gap = tread === 'knobs' ? 0.026 : tread === 'bars' ? 0.04 : 0.05;
    for (let s = s0 + gap * rng(); s < s1; s += gap) {
      if (rng() < 0.2) continue;   // worn, or the soil slumped back in
      const p = at(s);
      const tx = p.tx, ty = p.ty, nx = -ty, ny = tx;
      const P = (f, l) => ({ x: p.x + tx * f + nx * l, y: p.y + ty * f + ny * l });
      const laid = s <= lim;
      if (tread === 'chevron') {
        // A V whose point leads: tip forward, arms trailing back to the edges.
        const a = W * 0.42, k = 0.016, t = 0.012;
        if (laid) _trailPoly(g.deep, [P(-k, a), P(k, 0), P(-k, -a), P(-k - t, -a), P(k - t, 0), P(-k - t, a)]);
      } else if (tread === 'bars') {
        const a = W * 0.4, t = 0.007;
        if (laid) _trailPoly(g.deep, [P(-t, a), P(t, a), P(t, -a), P(-t, -a)]);
      } else {
        const side = rng() < 0.5 ? 1 : -1, c = W * 0.22 * side, h = Math.max(0.006, W * 0.18);
        if (laid) _trailPoly(g.deep, [P(-h, c - h), P(h, c - h), P(h, c + h), P(-h, c + h)]);
      }
    }
    g.used[3] = 1;
  }
}
