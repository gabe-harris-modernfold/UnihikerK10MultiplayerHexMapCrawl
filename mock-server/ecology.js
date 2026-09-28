// ── ecology.js — The Understory, ported for the mock-server ────────────────
// A JS port of ecology.hpp layers 1-3 (transforms, Physarum, lifecycle) plus
// the Wasteland Daisy, so offline UI work sees a live, fruiting ecology and so
// the style table / cycle timings can be tuned at an accelerated clock before
// the constants are copied to firmware. Same integer math, same xorshift32,
// same constants -- a pinned seed here grows the same SPECIES the board would
// (the growth itself differs because the inputs do).
//
// Wire-compatible with the firmware: wireMessage() / scarMessage() produce
// the `eco` messages docs/ecology-spec.md describes, and the same fields the
// firmware puts in /state come out of stateJson().
//
// Usage (server.js):
//   const eco = new Ecology({ seed, terrainAt, ... });   eco.genesis();
//   setInterval(() => { eco.tick(inputs); broadcast(eco.wireMessage()); ... }, ECO_TICK_MS / speed);
//   eco.noteHurt(q, r) at every injury site; eco.bloomAt(q, r) before a bite;
//   eco.blightAt(q, r) in the resource pass (a mouldy hex holds no resource).
'use strict';

const MAP_COLS = 75, MAP_ROWS = 57, NUM_TERRAIN = 16;
const DQ = [1, 1, 0, -1, -1, 0];
const DR = [0, -1, -1, 0, 1, 1];

const ECO_TICK_MS = 5000;
const SUB = 4, W = MAP_COLS * SUB, H = MAP_ROWS * SUB;        // 300 x 228
const XW = W * 64, YW = H * 64;                               // Q6 wrap
const TILE = 16, TX = Math.ceil(W / TILE), TY = Math.ceil(H / TILE);
const HEXES = MAP_ROWS * MAP_COLS;
const MAX_AGENTS = 4096, MAX_COLONIES = 16, MAX_BODIES = 12, MAX_SPORES = 32, MAX_DAISIES = 48, MAX_FLIGHTS = 24;
const COVERAGE_CAP = 25, VISIBLE_DENS = 4, AGE_SCAR_TICKS = 60, FIRST_CAP = 256, COLONY_CAP_MAX = 1024;
// Trail units per density step: (max + 3*mean)/4 over the hex's 16 sub-cells,
// divided by this, capped at 15. 12 needed a mean trail of ~48 before a hex
// counted as veined and 4096 agents covered 3% of the land; 6 puts a single
// well-run vein at the visible threshold.
const DENS_DIV = 6;
const EDGE_T = 20, TILE_T = 6, FOOD_MIN = 32, SPAWN_ENERGY = 200, SPORE_KEEPOUT = 3;
const VEC_WIND = 0, VEC_FOOT = 1, VEC_TIRE = 2, VEC_ASH = 3;
const B_HOSTILE = 160, B_FLOOD = 200, B_LETHAL = 255;
const AF_RETURN = 0x02, AF_SETTLED = 0x04;
const ST_FREE = 0, ST_GERM = 1, ST_FORAGE = 2, ST_FORM = 3, ST_MATURE = 4;
const WEATHER_RAIN = 1, WEATHER_STORM = 2, WEATHER_CHEM = 3;

// Physarum parameter sets -- must match ECO_STYLES in ecology.hpp.
const STYLES = [
  { sa: 16, ra: 32, dep: 8,  sd: 9 * 64,  ss: 64, dk: 418 },
  { sa: 24, ra: 48, dep: 4,  sd: 5 * 64,  ss: 64, dk: 400 },
  { sa: 32, ra: 24, dep: 6,  sd: 7 * 64,  ss: 51, dk: 410 },
  { sa: 12, ra: 20, dep: 10, sd: 12 * 64, ss: 77, dk: 425 },
  { sa: 40, ra: 60, dep: 5,  sd: 6 * 64,  ss: 64, dk: 405 },
  { sa: 20, ra: 40, dep: 6,  sd: 4 * 64,  ss: 45, dk: 395 },
  { sa: 10, ra: 16, dep: 9,  sd: 14 * 64, ss: 83, dk: 428 },
  { sa: 28, ra: 36, dep: 4,  sd: 8 * 64,  ss: 70, dk: 390 },
  { sa: 16, ra: 32, dep: 8,  sd: 9 * 64,  ss: 64, dk: 400 },
  { sa: 24, ra: 48, dep: 6,  sd: 5 * 64,  ss: 64, dk: 400 },
  { sa: 32, ra: 24, dep: 6,  sd: 7 * 64,  ss: 64, dk: 410 },
  { sa: 18, ra: 20, dep: 10, sd: 12 * 64, ss: 77, dk: 425 },
  { sa: 40, ra: 60, dep: 5,  sd: 6 * 64,  ss: 64, dk: 415 },
  { sa: 20, ra: 40, dep: 6,  sd: 6 * 64,  ss: 45, dk: 395 },
  { sa: 10, ra: 16, dep: 7,  sd: 14 * 64, ss: 83, dk: 428 },
  { sa: 28, ra: 28, dep: 4,  sd: 8 * 64,  ss: 70, dk: 390 },
];
const WORD_A = ['ASH','BONE','RUST','GLASS','SALT','TAR','IRON','MILK','SOOT','LIME','CHALK','BLOOD',
  'WAX','SILT','EMBER','DUST','MOTH','VEIL','MARROW','HOLLOW','BRINE','COAL','THORN','SPORE',
  'GRAVE','WIRE','LEAD','HUSK','STATIC','TALLOW','CINDER','FROST'];
const WORD_B = ['VESSEL','LATTICE','CROWN','FINGER','THREAD','ORGAN','TONGUE','CANDLE','LANTERN','ROOT',
  'WEB','CUP','CHOIR','MOUTH','EYE','HAND','BELL','KNOT','SHROUD','COMB','CRADLE','LOOM',
  'SPINE','WING','BLOOM','CHAIN','MASK','SEAM','HALO','ANCHOR','QUILL','GATE'];
const COLOUR = ['CRIMSON','RUST','AMBER','OCHRE','SAFFRON','LIME','JADE','VERDIGRIS',
  'TEAL','CERULEAN','SLATE','INDIGO','VIOLET','MAUVE','ROSE','CARMINE'];
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const HEXC = '0123456789ABCDEF';

const wrapQ = (q) => ((q % MAP_COLS) + MAP_COLS) % MAP_COLS;
const wrapR = (r) => ((r % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
function hexDistWrap(q1, r1, q2, r2) {
  let best = 1 << 30;
  for (let dq = -1; dq <= 1; dq++) for (let dr = -1; dr <= 1; dr++) {
    const aq = q2 + dq * MAP_COLS - q1, ar = r2 + dr * MAP_ROWS - r1;
    const d = (Math.abs(aq) + Math.abs(aq + ar) + Math.abs(ar)) >> 1;
    if (d < best) best = d;
  }
  return best;
}
const isWater = (t) => t === 5 || t === 11;
const popcount8 = (v) => { let n = 0; v &= 255; while (v) { n += v & 1; v >>= 1; } return n; };
const clamp = (v, lo, hi) => v < lo ? lo : v > hi ? hi : v;
const SIN = new Int16Array(256);
for (let i = 0; i < 256; i++) SIN[i] = Math.round(Math.sin(i * Math.PI * 2 / 256) * 1024);
const COS = (h) => SIN[(h + 64) & 255];

class Ecology {
  // inputs: { terrainAt(q,r), footprintsAt(q,r), trackAt(q,r), tireAt(q,r),
  //           fireAt(q,r), floodAt(q,r), weather, day, players: [{q,r}] }
  constructor(opts = {}) {
    this.opts = opts;
    this.trail  = new Uint8Array(H * W);
    this.trailB = new Uint8Array(H * W);
    this.hexFood = new Uint8Array(HEXES);
    this.hexBarrier = new Uint8Array(HEXES);
    this.hexAge = new Uint8Array(HEXES);
    this.hexScar = new Uint8Array(HEXES);
    this.hexDens = new Uint8Array(HEXES);
    this.hexMask = new Uint8Array(HEXES);
    this.agents = [];              // {x,y,heading,colony,energy,flags}
    this.colonies = Array.from({ length: MAX_COLONIES }, () => ({ stage: ST_FREE }));
    this.visited = Array.from({ length: MAX_COLONIES }, () => new Uint8Array(HEXES));
    this.bodies = [];
    this.spores = [];
    this.daisies = [];
    this.flights = [];
    this.bloom = new Uint8Array(HEXES);
    this.tileAgent = new Uint8Array(TX * TY);
    this.tileTrail = new Uint8Array(TX * TY);
    this.tick = 0;
    this.wave = 0;
    this.coverage = 0;
    this.bootsSeen = 0;
    this.lastDay = -1;
    this.biteOn = opts.bite !== undefined ? !!opts.bite : true;
    this.blightOn = opts.blight !== undefined ? !!opts.blight : true;
    this.eaten = 0;                // resources the blight has eaten (server.js counts them)
    this.species = null;
    this.scarMsgStale = true;
    this.scarMsgSend = false;
    this.lastTickMs = 0;
    this.rng = 0x9E3779B9;
    this.snap = null;
    this.hurt = [];
    this.scarredHexes = 0;
    this.log = opts.log || (() => {});
  }

  // ── PRNG ──
  rand() {
    let x = this.rng >>> 0;
    x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
    this.rng = x || 0x2545F491;
    return this.rng;
  }
  randN(n) { return n > 0 ? this.rand() % n : 0; }

  // ── Genome ──
  decodeGenome(seed) {
    const s = { seed: seed >>> 0, genome: new Uint8Array(16) };
    let x = (seed >>> 0) || 0xA511E9B3;
    for (let i = 0; i < 16; i++) {
      x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
      s.genome[i] = (x >>> ((i & 3) * 8)) & 255;
    }
    const g = s.genome;
    s.style = g[0] & 15; s.tempo = g[0] >> 4;
    const jitter = g[1];
    const aff = (g[2] | (g[3] << 8) | (g[4] << 16) | (g[5] << 24)) >>> 0;
    s.affinity = [];
    for (let t = 0; t < NUM_TERRAIN; t++) s.affinity[t] = (aff >>> (t * 2)) & 3;
    s.affinity[5] = 0; s.affinity[11] = 0;
    if (s.affinity[10] === 3) s.affinity[10] = 2;
    s.vector = g[6] & 3; s.cycle = g[6] >> 4;
    s.growth = g[7] & 15; s.fruit = g[7] >> 4;
    s.shyness = (g[8] & 15) - 8; s.scarLove = g[8] >> 4;
    s.hue = g[9];
    const st = STYLES[s.style];
    s.sa = clamp(st.sa + ((jitter & 3) - 1) * 2, 6, 60);
    s.ra = st.ra;
    s.sd = clamp(st.sd + (((jitter >> 2) & 3) - 1) * 64, 3 * 64, 16 * 64);
    s.ss = clamp(st.ss + (((jitter >> 4) & 3) - 1) * 6, 32, 96);
    s.dep = clamp(st.dep + (((jitter >> 6) & 3) - 1), 2, 14);
    s.dk = st.dk;
    const c = s.cycle;
    s.dormancy = 12 + c * 3; s.germTicks = 12; s.forageTicks = 180 + c * 16;
    s.starveTicks = 24 + c * 4; s.formTicks = 12 + c; s.holdTicks = 24 + c * 2;
    s.capMulQ8 = 358 + Math.floor((s.growth * 103) / 15);
    s.sporesPerBody = 2 + (s.growth >> 2);
    s.name = `${WORD_A[g[10] & 31]}-${WORD_B[g[11] & 31]} ${COLOUR[s.hue >> 4]}`;
    this.species = s;
    return s;
  }

  // ── Scars (the mock keeps them in memory; a file is the firmware's job) ──
  loadScars(arr) { if (arr && arr.length === HEXES) this.hexScar.set(arr); }
  clearScars() { this.hexScar.fill(0); this.bootsSeen = 0; }
  publishScars() {
    this.scarredHexes = 0;
    for (let i = 0; i < HEXES; i++) if (this.hexScar[i]) this.scarredHexes++;
    this.scarMsgStale = true;
  }

  // ── Snapshot of the game's inputs ──
  snapshot(inputs) {
    const t = new Uint8Array(HEXES), fp = new Uint8Array(HEXES), tr = new Uint8Array(HEXES);
    const ti = new Uint8Array(HEXES), fi = new Uint8Array(HEXES), fl = new Uint8Array(HEXES);
    for (let r = 0; r < MAP_ROWS; r++) for (let q = 0; q < MAP_COLS; q++) {
      const i = r * MAP_COLS + q;
      const tt = inputs.terrainAt(q, r) | 0;
      t[i] = tt < NUM_TERRAIN ? tt : 0;
      fp[i] = inputs.footprintsAt ? (inputs.footprintsAt(q, r) & 0x3F) : 0;
      tr[i] = inputs.trackAt ? (inputs.trackAt(q, r) & 255) : 0;
      ti[i] = inputs.tireAt ? (inputs.tireAt(q, r) ? 1 : 0) : 0;
      fi[i] = inputs.fireAt ? (inputs.fireAt(q, r) | 0) : 0;
      fl[i] = inputs.floodAt ? (inputs.floodAt(q, r) | 0) : 0;
    }
    this.snap = { terrain: t, foot: fp, track: tr, tire: ti, fire: fi, flood: fl,
                  weather: inputs.weather | 0, day: inputs.day | 0,
                  players: (inputs.players || []).map((p) => ({ q: p.q, r: p.r })),
                  clients: inputs.clients | 0, heapFrac: inputs.heapFrac === undefined ? 100 : inputs.heapFrac };
  }
  nearPlayer(q, r, keepout) {
    for (const p of this.snap.players) if (hexDistWrap(q, r, p.q, p.r) <= keepout) return true;
    return false;
  }

  // ── Transforms ──
  wipeBlock(q, r, halve) {
    for (let y = r * SUB; y < (r + 1) * SUB; y++) {
      const o = y * W + q * SUB;
      for (let x = 0; x < SUB; x++) this.trail[o + x] = halve ? (this.trail[o + x] >> 1) : 0;
    }
  }
  transforms() {
    const s = this.species, sn = this.snap, shy = s.shyness;
    for (let r = 0; r < MAP_ROWS; r++) for (let q = 0; q < MAP_COLS; q++) {
      const i = r * MAP_COLS + q, t = sn.terrain[i];
      let food = 0, barrier = 0;
      if (isWater(t)) barrier = B_LETHAL;
      else {
        switch (s.affinity[t]) { case 0: barrier = B_HOSTILE; break; case 1: food = 8; break; case 2: food = 40; break; default: food = 80; }
        food += popcount8(sn.foot[i]) * 12;
        if (sn.track[i]) { if (shy <= 0) food += Math.floor(sn.track[i] * (1 - shy) / 8); else food -= Math.floor(sn.track[i] * shy / 8); }
        if (sn.tire[i]) food += 16;
        if (s.vector === VEC_ASH && t === 1) food += 40;
        food += Math.floor(s.scarLove * this.hexScar[i] / 2);
        if (sn.flood[i]) { barrier = B_FLOOD; this.wipeBlock(q, r, true); }
      }
      if (sn.fire[i]) { barrier = B_LETHAL; this.wipeBlock(q, r, false); }
      this.hexFood[i] = clamp(food, 0, 255);
      this.hexBarrier[i] = barrier;
    }
  }
  substeps() {
    const base = 4 + (this.species.tempo >> 1);
    const heapFrac = clamp(this.snap.heapFrac, 0, 100);
    const clients = clamp(Math.floor(this.snap.players.length * 100 / 6), 0, 100);
    const rate = 100 + Math.floor(clients * 25 / 100) - Math.floor(heapFrac * 25 / 100);
    return Math.max(1, Math.floor((base * rate + 50) / 100));
  }

  // ── Physarum ──
  markTile(x, y) {
    const tx = Math.floor(x / TILE), ty = Math.floor(y / TILE);
    for (let dy = -1; dy <= 1; dy++) {
      let yy = ty + dy; if (yy < 0) yy += TY; else if (yy >= TY) yy -= TY;
      for (let dx = -1; dx <= 1; dx++) {
        let xx = tx + dx; if (xx < 0) xx += TX; else if (xx >= TX) xx -= TX;
        this.tileAgent[yy * TX + xx] = 1;
      }
    }
  }
  sense(x, y, h) {
    const s = this.species;
    let px = x + ((s.sd * COS(h)) >> 10), py = y + ((s.sd * SIN[h]) >> 10);
    if (px < 0) px += XW; else if (px >= XW) px -= XW;
    if (py < 0) py += YW; else if (py >= YW) py -= YW;
    const sx = px >> 6, sy = py >> 6, hi = (sy >> 2) * MAP_COLS + (sx >> 2);
    return this.trail[sy * W + sx] + this.hexFood[hi] - this.hexBarrier[hi];
  }
  deposit(x, y, amt) {
    const i = (y >> 6) * W + (x >> 6);
    const v = this.trail[i] + amt; this.trail[i] = v > 255 ? 255 : v;
  }
  killAgent(i) {
    const a = this.agents[i];
    const c = this.colonies[a.colony];
    if (c.agentCount) c.agentCount--;
    this.agents[i] = this.agents[this.agents.length - 1];
    this.agents.pop();
  }
  spawnAgent(colony, x, y, heading, energy, flags) {
    if (this.agents.length >= MAX_AGENTS) return false;
    this.agents.push({ x, y, heading: heading & 255, colony, energy, flags });
    this.colonies[colony].agentCount++;
    return true;
  }
  turnToward(h, dx, dy, by) {
    const cross = dx * SIN[h] - dy * COS(h);
    return (cross > 0 ? h - by : h + by) & 255;
  }
  agentStep() {
    const s = this.species, sa = s.sa, ra = s.ra, dep = s.dep, ss = s.ss;
    for (let i = 0; i < this.agents.length;) {
      const a = this.agents[i];
      const col = this.colonies[a.colony];
      let x = a.x, y = a.y;
      let hi = (y >> 8) * MAP_COLS + (x >> 8);
      if (this.hexBarrier[hi] === B_LETHAL || col.stage === ST_FREE) { this.killAgent(i); continue; }
      let h = a.heading;
      if (this.hexBarrier[hi] === B_FLOOD) { h = (h + 128) & 255; a.energy = a.energy > 2 ? a.energy - 2 : 0; }
      let settled = false;
      if (a.flags & AF_RETURN) {
        let best = 1 << 30, bdx = 0, bdy = 0;
        for (let k = 0; k < col.nSites; k++) {
          let dx = col.siteQ[k] * 256 + 128 - x, dy = col.siteR[k] * 256 + 128 - y;
          if (dx > XW / 2) dx -= XW; else if (dx < -XW / 2) dx += XW;
          if (dy > YW / 2) dy -= YW; else if (dy < -YW / 2) dy += YW;
          const d2 = dx * dx + dy * dy;
          if (d2 < best) { best = d2; bdx = dx; bdy = dy; }
        }
        if (col.nSites && best <= (3 * 64) * (3 * 64)) {
          settled = true; a.flags |= AF_SETTLED;
          this.deposit(x, y, dep * 2); this.markTile(x >> 6, y >> 6);
          i++; continue;
        }
        if (col.nSites && (this.rand() & 1)) h = this.turnToward(h, bdx, bdy, ra);
        const drain = best > (24 * 64) * (24 * 64) ? 8 : 1;
        if (a.energy <= drain) { this.killAgent(i); continue; }
        a.energy -= drain;
      }
      if (!settled) {
        const vC = this.sense(x, y, h), vL = this.sense(x, y, (h - sa) & 255), vR = this.sense(x, y, (h + sa) & 255);
        if (vC >= vL && vC >= vR) { /* straight */ }
        else if (vL > vR) h = (h - ra) & 255;
        else if (vR > vL) h = (h + ra) & 255;
        else h = (this.rand() & 1) ? (h + ra) & 255 : (h - ra) & 255;
      }
      let nx = x + ((ss * COS(h)) >> 10), ny = y + ((ss * SIN[h]) >> 10);
      if (nx < 0) nx += XW; else if (nx >= XW) nx -= XW;
      if (ny < 0) ny += YW; else if (ny >= YW) ny -= YW;
      const nhi = (ny >> 8) * MAP_COLS + (nx >> 8);
      // Water or fire ahead: turn back with a random swing rather than step
      // in. Standing in one (the fire spread over the vein, the flood rose
      // under it) is what kills, at the top of the loop. Jones's agents do
      // not enter blocked cells either; dying on contact emptied whole
      // colonies into the rivers within a dozen ticks on this map.
      if (this.hexBarrier[nhi] === B_LETHAL) h = (h + 128 + this.randN(65) - 32) & 255;
      else if (this.hexBarrier[nhi] >= B_HOSTILE && (this.rand() & 1)) h = (h + 128) & 255;
      else { x = nx; y = ny; }
      a.x = x; a.y = y; a.heading = h;
      this.deposit(x, y, dep); this.markTile(x >> 6, y >> 6);
      hi = (y >> 8) * MAP_COLS + (x >> 8);
      const food = this.hexFood[hi];
      if (food >= FOOD_MIN) {
        const en = a.energy + (food >> 3); a.energy = en > 255 ? 255 : en;
        const vis = this.visited[a.colony];
        if (!vis[hi]) { vis[hi] = 1; col.lastFoodTick = this.tick; }
      } else if (a.energy) a.energy--;
      if (col.stage === ST_FORAGE && a.energy >= SPAWN_ENERGY && col.agentCount < col.waveCap && this.agents.length < MAX_AGENTS) {
        const half = a.energy >> 1; a.energy = half;
        this.spawnAgent(a.colony, x, y, (h + this.randN(64) - 32) & 255, half, 0);
      }
      i++;
    }
  }
  diffuse() {
    const dk = this.species.dk, A = this.trail, B = this.trailB;
    for (let ty = 0; ty < TY; ty++) for (let tx = 0; tx < TX; tx++) {
      const ti = ty * TX + tx;
      if (!this.tileAgent[ti] && !this.tileTrail[ti]) continue;
      const x0 = tx * TILE, y0 = ty * TILE, x1 = Math.min(x0 + TILE, W), y1 = Math.min(y0 + TILE, H);
      let tileMax = 0;
      for (let y = y0; y < y1; y++) {
        const ym = y ? y - 1 : H - 1, yp = (y + 1 === H) ? 0 : y + 1;
        const rm = ym * W, r0 = y * W, rp = yp * W;
        for (let x = x0; x < x1; x++) {
          const xm = x ? x - 1 : W - 1, xp = (x + 1 === W) ? 0 : x + 1;
          const sum = A[rm + xm] + A[rm + x] + A[rm + xp] + A[r0 + xm] + A[r0 + x] + A[r0 + xp] + A[rp + xm] + A[rp + x] + A[rp + xp];
          const v = (sum * dk) >> 12;
          B[r0 + x] = v;
          if (v > tileMax) tileMax = v;
        }
      }
      const keep = this.tileAgent[ti] || tileMax >= TILE_T;
      for (let y = y0; y < y1; y++) {
        const o = y * W;
        if (keep) A.set(B.subarray(o + x0, o + x1), o + x0);
        else A.fill(0, o + x0, o + x1);
      }
      this.tileTrail[ti] = keep ? 1 : 0;
    }
    this.tileAgent.fill(0);
  }

  // ── Lifecycle ──
  freeColony() { return this.colonies.findIndex((c) => c.stage === ST_FREE); }
  sporeOk(q, r) {
    const i = r * MAP_COLS + q;
    if (isWater(this.snap.terrain[i]) || this.snap.fire[i]) return false;
    return !this.nearPlayer(q, r, SPORE_KEEPOUT);
  }
  placeSpore(q, r, generation, waveCap) {
    if (this.spores.length >= MAX_SPORES || !this.sporeOk(q, r)) return false;
    const d = this.species.dormancy;
    this.spores.push({ q, r, generation, waveCap, timer: Math.max(6, d + this.randN((d >> 1) + 1) - (d >> 2)) });
    return true;
  }
  windDir() {
    const w = this.snap.weather;
    if (w === WEATHER_RAIN || w === WEATHER_STORM || w === WEATHER_CHEM) { const r = this.randN(10); return r < 6 ? 0 : (r < 8 ? 1 : 5); }
    return this.randN(6);
  }
  noteFlight(fq, fr, tq, tr) { if (this.flights.length < MAX_FLIGHTS) this.flights.push([fq, fr, tq, tr]); }
  disperseWind(q, r, gen, cap) {
    for (let attempt = 0; attempt < 4; attempt++) {
      const d = this.windDir(), dist = 4 + this.randN(7);
      const tq = wrapQ(q + DQ[d] * dist + this.randN(3) - 1), tr = wrapR(r + DR[d] * dist + this.randN(3) - 1);
      if (this.placeSpore(tq, tr, gen, cap)) { this.noteFlight(q, r, tq, tr); return true; }
    }
    return false;
  }
  disperseTo(q, r, gen, cap, minDist, radius, kind) {
    let bestQ = -1, bestR = -1, bestW = 0, seen = 0;
    const sn = this.snap;
    for (let dr = -radius; dr <= radius; dr++) for (let dq = -radius; dq <= radius; dq++) {
      const cq = wrapQ(q + dq), cr = wrapR(r + dr);
      const dist = hexDistWrap(q, r, cq, cr);
      if (dist < minDist || dist > radius) continue;
      const i = cr * MAP_COLS + cq;
      let w = 0;
      if (kind === VEC_FOOT) w = popcount8(sn.foot[i]) * 4 + (sn.track[i] ? 2 : 0);
      else if (kind === VEC_TIRE) w = sn.tire[i] ? 4 : 0;
      else if (kind === VEC_ASH) w = sn.terrain[i] === 1 ? 4 : 0;
      if (!w) continue;
      w += this.randN(3);
      if (w > bestW) { bestW = w; bestQ = cq; bestR = cr; seen = 1; }
      else if (w === bestW && this.randN(++seen) === 0) { bestQ = cq; bestR = cr; }
    }
    if (bestQ >= 0 && this.placeSpore(bestQ, bestR, gen, cap)) { this.noteFlight(q, r, bestQ, bestR); return true; }
    return false;
  }
  disperse(q, r, gen, cap) {
    let placed = 0;
    for (let k = 0; k < this.species.sporesPerBody; k++) {
      let ok = false;
      switch (this.species.vector) {
        case VEC_FOOT: ok = this.disperseTo(q, r, gen, cap, 3, 8, VEC_FOOT); break;
        case VEC_TIRE: ok = this.disperseTo(q, r, gen, cap, 3, 12, VEC_TIRE); break;
        case VEC_ASH:  ok = this.disperseTo(q, r, gen, cap, 1, 10, VEC_ASH); break;
        default: break;
      }
      if (!ok) ok = this.disperseWind(q, r, gen, cap);
      if (ok) placed++;
    }
    // A wave must not end here. If the vector and the wind both found
    // nothing (a body hemmed in by water, or by survivors -- SPORE_KEEPOUT),
    // try anywhere within reach, then anywhere at all.
    for (let tries = 0; tries < 60 && !placed; tries++) {
      const radius = tries < 40 ? 12 : MAP_COLS;
      const tq = wrapQ(q + this.randN(radius * 2 + 1) - radius), tr = wrapR(r + this.randN(radius * 2 + 1) - radius);
      if (this.placeSpore(tq, tr, gen, cap)) { this.noteFlight(q, r, tq, tr); placed++; }
    }
  }
  germinate(sp) {
    const ci = this.freeColony();
    if (ci < 0) return false;
    const c = { stage: ST_GERM, generation: sp.generation, waveCap: sp.waveCap, agentCount: 0, nSites: 0,
                stageTick: this.tick, lastFoodTick: this.tick, originQ: sp.q, originR: sp.r,
                siteQ: [0, 0, 0], siteR: [0, 0, 0], timer: 0, seed: this.rand() & 255 };
    this.colonies[ci] = c;
    this.visited[ci].fill(0);
    const n = 32 + this.randN(33), cx = sp.q * 256 + 128, cy = sp.r * 256 + 128;
    let spawned = 0;
    for (let k = 0; k < n; k++) {
      let x = cx + this.randN(129) - 64, y = cy + this.randN(129) - 64;
      if (x < 0) x += XW; else if (x >= XW) x -= XW;
      if (y < 0) y += YW; else if (y >= YW) y -= YW;
      if (!this.spawnAgent(ci, x, y, this.rand() & 255, 128, 0)) break;
      spawned++;
    }
    if (sp.generation + 1 > this.wave) this.wave = sp.generation + 1;
    this.log(`eco germinate colony=${ci} gen=${c.generation} cap=${c.waveCap} at (${sp.q},${sp.r}) agents=${spawned}/${n}`);
    return true;
  }
  enterFruit(ci) {
    const c = this.colonies[ci];
    const count = new Uint16Array(HEXES);
    for (const a of this.agents) if (a.colony === ci) count[(a.y >> 8) * MAP_COLS + (a.x >> 8)]++;
    const want = Math.min(3, 1 + Math.floor(c.agentCount / 200));
    c.nSites = 0;
    for (let s = 0; s < want; s++) {
      let bestI = -1, bestScore = 0;
      for (let hi = 0; hi < HEXES; hi++) {
        if (!count[hi] || this.hexBarrier[hi] >= B_HOSTILE) continue;
        const q = hi % MAP_COLS, r = Math.floor(hi / MAP_COLS);
        const score = count[hi] * (2 + this.species.affinity[this.snap.terrain[hi]]) * (16 + this.hexDens[hi]);
        let far = true;
        for (let k = 0; k < c.nSites; k++) if (hexDistWrap(q, r, c.siteQ[k], c.siteR[k]) < 3) { far = false; break; }
        if (!far) continue;
        if (score > bestScore) { bestScore = score; bestI = hi; }
      }
      if (bestI < 0) break;
      c.siteQ[c.nSites] = bestI % MAP_COLS; c.siteR[c.nSites] = Math.floor(bestI / MAP_COLS); c.nSites++;
    }
    if (!c.nSites) { c.siteQ[0] = c.originQ; c.siteR[0] = c.originR; c.nSites = 1; }
    for (let s = 0; s < c.nSites; s++) {
      if (this.bodies.length >= MAX_BODIES) break;
      this.bodies.push({ q: c.siteQ[s], r: c.siteR[s], colony: ci, stage: 1, timer: 0, seed: this.rand() & 255 });
    }
    for (const a of this.agents) if (a.colony === ci) a.flags |= AF_RETURN;
    c.stage = ST_FORM; c.timer = this.species.formTicks; c.stageTick = this.tick;
    this.log(`eco fruit colony=${ci} sites=${c.nSites} first=(${c.siteQ[0]},${c.siteR[0]}) agents=${c.agentCount}`);
  }
  burst(ci) {
    const c = this.colonies[ci];
    const cap = Math.min(COLONY_CAP_MAX, (c.waveCap * this.species.capMulQ8) >> 8);
    const gen = Math.min(250, c.generation + 1);
    let bodies = 0;
    for (const b of this.bodies) {
      if (b.colony !== ci) continue;
      b.stage = 3; bodies++;
      const hi = b.r * MAP_COLS + b.q;
      this.hexScar[hi] = Math.min(15, this.hexScar[hi] + 3);
      this.disperse(b.q, b.r, gen, cap);
    }
    if (!bodies) this.disperse(c.siteQ[0], c.siteR[0], gen, cap);
    for (let i = 0; i < this.agents.length;) { if (this.agents[i].colony === ci) this.killAgent(i); else i++; }
    this.log(`eco burst colony=${ci} gen=${c.generation} bodies=${bodies} nextCap=${cap}`);
    this.colonies[ci] = { stage: ST_FREE };
    this.publishScars();
  }
  lifecycle() {
    this.bodies = this.bodies.filter((b) => b.stage !== 3);
    const sn = this.snap;
    this.spores = this.spores.filter((sp) => !(sn.fire[sp.r * MAP_COLS + sp.q] || isWater(sn.terrain[sp.r * MAP_COLS + sp.q])));
    for (const sp of this.spores) {
      if (sp.timer) { sp.timer--; continue; }
      if (this.agents.length + 64 > MAX_AGENTS || this.coverage >= COVERAGE_CAP || this.freeColony() < 0) continue;
      if (this.germinate(sp)) sp.done = true;
    }
    this.spores = this.spores.filter((sp) => !sp.done);
    for (let ci = 0; ci < MAX_COLONIES; ci++) {
      const c = this.colonies[ci];
      if (c.stage === ST_FREE) continue;
      const inStage = this.tick - c.stageTick, s = this.species;
      switch (c.stage) {
        case ST_GERM:
          if (inStage >= s.germTicks) { c.stage = ST_FORAGE; c.stageTick = this.tick; c.lastFoodTick = this.tick; }
          break;
        case ST_FORAGE:
          if (c.agentCount === 0) { this.colonies[ci] = { stage: ST_FREE }; break; }
          if (c.agentCount >= c.waveCap || this.tick - c.lastFoodTick >= s.starveTicks || inStage >= s.forageTicks) this.enterFruit(ci);
          break;
        case ST_FORM:
          if (c.timer) c.timer--;
          else { c.stage = ST_MATURE; c.timer = s.holdTicks; c.stageTick = this.tick; for (const b of this.bodies) if (b.colony === ci) b.stage = 2; }
          break;
        case ST_MATURE:
          if (c.timer) c.timer--; else this.burst(ci);
          break;
        default: break;
      }
    }
  }

  // ── Daisies ──
  noteHurt(q, r) { if (this.hurt.length < 16) this.hurt.push([q, r]); else this.hurt.shift(), this.hurt.push([q, r]); }
  daisyAt(q, r) { return this.daisies.find((d) => d.q === q && d.r === r); }
  daisySeed(q, r) {
    const d = this.daisyAt(q, r);
    if (d) { if (d.count < 7) d.count++; return; }
    if (this.daisies.length >= MAX_DAISIES) {
      let victim = -1, newest = -1;
      this.daisies.forEach((p, i) => { if (p.stage === 0 && p.seededTick >= newest) { newest = p.seededTick; victim = i; } });
      if (victim < 0) return;
      this.daisies.splice(victim, 1);
    }
    this.daisies.push({ q, r, stage: 0, dawns: 0, count: 1, seed: this.rand() & 255, seededTick: this.tick });
  }
  daisyTick(dawn) {
    const sn = this.snap;
    this.daisies = this.daisies.filter((d) => !sn.fire[d.r * MAP_COLS + d.q]);
    if (dawn) for (const d of this.daisies) { if (d.dawns < 255) d.dawns++; d.stage = Math.min(3, d.dawns); }
    for (const [q, r] of this.hurt) this.daisySeed(q, r);
    this.hurt = [];
    this.bloom.fill(0);
    for (const d of this.daisies) if (d.stage === 3) this.bloom[d.r * MAP_COLS + d.q] = 1;
  }
  onIgnite(q, r) { this.bloom[r * MAP_COLS + q] = 0; }
  bloomAt(q, r) { return !!this.bloom[r * MAP_COLS + q]; }
  // The blight (ecoBlightAt in ecology.hpp): any mould the client draws --
  // density 1+, or a mature fruiting body. Surface only, like everything here.
  blightAt(q, r) {
    if (!this.species || !this.blightOn) return false;
    if (this.hexDens[r * MAP_COLS + q]) return true;
    return this.bodies.some((b) => b.stage === 2 && b.q === q && b.r === r);
  }

  // ── Per-hex derivation ──
  stripMean(x0, y0, w, h) {
    let sum = 0;
    for (let y = y0; y < y0 + h; y++) { const o = (y % H) * W; for (let x = x0; x < x0 + w; x++) sum += this.trail[o + (x % W)]; }
    return Math.floor(sum / (w * h));
  }
  derive(dawn) {
    let land = 0, veined = 0;
    const sn = this.snap;
    for (let r = 0; r < MAP_ROWS; r++) for (let q = 0; q < MAP_COLS; q++) {
      let mx = 0, sum = 0;
      for (let y = r * SUB; y < (r + 1) * SUB; y++) { const o = y * W + q * SUB; for (let x = 0; x < SUB; x++) { const v = this.trail[o + x]; sum += v; if (v > mx) mx = v; } }
      const mean = sum >> 4;
      let dens = Math.floor(((mx + 3 * mean) >> 2) / DENS_DIV); if (dens > 15) dens = 15;
      const hi = r * MAP_COLS + q;
      this.hexDens[hi] = dens;
      if (!isWater(sn.terrain[hi])) { land++; if (dens >= VISIBLE_DENS) { veined++; if (this.hexAge[hi] < 255) this.hexAge[hi]++; } }
    }
    this.coverage = land ? Math.floor(veined * 100 / land) : 0;
    for (let r = 0; r < MAP_ROWS; r++) for (let q = 0; q < MAP_COLS; q++) {
      const hi = r * MAP_COLS + q;
      let m = 0;
      if (this.hexDens[hi]) {
        const x0 = q * SUB, y0 = r * SUB;
        { const nq = wrapQ(q + 1);
          if (this.hexDens[r * MAP_COLS + nq] && ((this.stripMean(x0 + 3, y0, 1, 4) + this.stripMean(nq * SUB, y0, 1, 4)) >> 1) >= EDGE_T) m |= 1; }
        { const nq = wrapQ(q + 1), nr = wrapR(r - 1);
          if (this.hexDens[nr * MAP_COLS + nq] && ((this.stripMean(x0 + 2, y0, 2, 2) + this.stripMean(nq * SUB, nr * SUB + 2, 2, 2)) >> 1) >= EDGE_T) m |= 2; }
        { const nr = wrapR(r - 1);
          if (this.hexDens[nr * MAP_COLS + q] && ((this.stripMean(x0, y0, 4, 1) + this.stripMean(x0, nr * SUB + 3, 4, 1)) >> 1) >= EDGE_T) m |= 4; }
      }
      this.hexMask[hi] = m;
    }
    for (let r = 0; r < MAP_ROWS; r++) for (let q = 0; q < MAP_COLS; q++) {
      const hi = r * MAP_COLS + q;
      let m = this.hexMask[hi];
      if (this.hexMask[r * MAP_COLS + wrapQ(q - 1)] & 1) m |= 8;
      if (this.hexMask[wrapR(r + 1) * MAP_COLS + wrapQ(q - 1)] & 2) m |= 16;
      if (this.hexMask[wrapR(r + 1) * MAP_COLS + q] & 4) m |= 32;
      this.hexMask[hi] = m;
    }
    if (dawn) {
      let changed = false;
      for (let hi = 0; hi < HEXES; hi++) {
        if (this.hexAge[hi] >= AGE_SCAR_TICKS && this.hexScar[hi] < 15) { this.hexScar[hi]++; changed = true; }
        this.hexAge[hi] = 0;
      }
      if (changed) this.publishScars();
    }
  }

  // ── Genesis / tick ──
  resetLiving() {
    this.agents = []; this.colonies = Array.from({ length: MAX_COLONIES }, () => ({ stage: ST_FREE }));
    this.bodies = []; this.spores = []; this.daisies = []; this.flights = [];
    this.trail.fill(0); this.hexDens.fill(0); this.hexMask.fill(0); this.hexAge.fill(0); this.bloom.fill(0);
    this.tileAgent.fill(0); this.tileTrail.fill(0);
    this.wave = 0; this.coverage = 0; this.hurt = [];
  }
  placeGenesisSpores() {
    const want = 2 + this.randN(3), sn = this.snap, s = this.species;
    for (let k = 0; k < want; k++) {
      let total = 0, pickQ = -1, pickR = -1;
      for (let r = 0; r < MAP_ROWS; r++) for (let q = 0; q < MAP_COLS; q++) {
        const hi = r * MAP_COLS + q, t = sn.terrain[hi];
        if (isWater(t) || this.nearPlayer(q, r, SPORE_KEEPOUT)) continue;
        const scar = this.hexScar[hi];
        let w = 0;
        if (scar >= 3) w = 8 + scar * (1 + s.scarLove);
        else if (scar) w = 2 + Math.floor(scar * (1 + s.scarLove) / 2);
        if (s.affinity[t] === 3) w += 3; else if (s.affinity[t] === 2) w += 1;
        if (!w) continue;
        if (this.spores.some((sp) => sp.q === q && sp.r === r)) continue;
        total += w;
        if (this.rand() % total < w) { pickQ = q; pickR = r; }
      }
      if (pickQ < 0) {
        for (let tries = 0; tries < 200 && pickQ < 0; tries++) {
          const q = this.randN(MAP_COLS), r = this.randN(MAP_ROWS);
          if (this.sporeOk(q, r)) { pickQ = q; pickR = r; }
        }
        if (pickQ < 0) break;
      }
      this.placeSpore(pickQ, pickR, 0, FIRST_CAP);
      this.log(`eco spore ${k} at (${pickQ},${pickR}) scar=${this.hexScar[pickR * MAP_COLS + pickQ]}`);
    }
  }
  genesis(seed, inputs) {
    const pinned = seed >>> 0;
    const s = pinned || ((Math.random() * 0xFFFFFFFF) >>> 0) || 1;
    this.decodeGenome(s);
    this.rng = (s ^ 0x9E3779B9) >>> 0; this.rand();
    this.resetLiving();
    this.bootsSeen++;
    if (inputs) this.snapshot(inputs);
    else if (!this.snap) this.snapshot({ terrainAt: this.opts.terrainAt || (() => 0) });
    this.placeGenesisSpores();
    this.lastDay = this.snap.day;
    this.publishScars();
    const sp = this.species;
    this.log(`eco genesis seed=${sp.seed} name=${sp.name} style=${sp.style} vector=${sp.vector} tempo=${sp.tempo} cycle=${sp.cycle} growth=${sp.growth} shy=${sp.shyness} hue=${sp.hue} pinned=${pinned ? 1 : 0}`);
    return sp;
  }
  regen(inputs) { this.clearScars(); this.genesis(0, inputs); }
  step(inputs) {
    const t0 = Date.now();
    this.snapshot(inputs);
    this.tick++;
    this.flights = [];
    this.scarMsgSend = false;
    const dawn = this.lastDay >= 0 && this.snap.day !== this.lastDay;
    this.lastDay = this.snap.day;
    this.transforms();
    this.daisyTick(dawn);
    this.lifecycle();
    const sub = this.substeps();
    for (let s = 0; s < sub; s++) { this.agentStep(); this.diffuse(); }
    this.derive(dawn);
    this.lastTickMs = Date.now() - t0;
    return { sub, ms: this.lastTickMs };
  }

  // ── Wire ──
  wireMessage() {
    const s = this.species;
    let v = '';
    for (let hi = 0; hi < HEXES; hi++) v += HEXC[this.hexDens[hi] & 15] + B64[this.hexMask[hi] & 63];
    return {
      t: 'eco', tk: this.tick, n: s.name, h: s.hue, g: s.fruit, w: this.wave, v,
      f: this.bodies.map((b) => [b.q, b.r, b.stage, b.seed]),
      dz: this.daisies.filter((d) => d.stage > 0).map((d) => [d.q, d.r, d.stage, d.count, d.seed]),
      sp: this.flights.slice(),
    };
  }
  scarMessage() {
    let sc = '';
    for (let hi = 0; hi < HEXES; hi++) sc += HEXC[this.hexScar[hi] & 15];
    this.scarMsgStale = false;
    return { t: 'eco', tk: this.tick, s: sc };
  }
  stateJson() {
    const s = this.species || {};
    const colonies = this.colonies.filter((c) => c.stage !== ST_FREE).length;
    const seeded = this.daisies.filter((d) => d.stage === 0).length;
    const bloomed = this.daisies.filter((d) => d.stage === 3).length;
    return {
      on: true, seed: (s.seed || 0) >>> 0, name: s.name || '', style: s.style | 0, vector: s.vector | 0, hue: s.hue | 0,
      tick: this.tick, tickUs: this.lastTickMs * 1000, wave: this.wave, colonies, agents: this.agents.length,
      spores: this.spores.length, coverage: this.coverage, fruiting: this.bodies.length, scarred: this.scarredHexes,
      bootsSeen: this.bootsSeen,
      daisies: { seeded, growing: this.daisies.length - seeded - bloomed, bloomed },
      bite: this.biteOn ? 1 : 0,
      blight: this.blightOn ? 1 : 0, eaten: this.eaten,
    };
  }

  // ── Debug hooks for the mock's dbg_eco message ──
  forceFruit() { let n = 0; for (let ci = 0; ci < MAX_COLONIES; ci++) if (this.colonies[ci].stage === ST_FORAGE || this.colonies[ci].stage === ST_GERM) { this.enterFruit(ci); n++; } return n; }
  forceBurst() { let n = 0; for (let ci = 0; ci < MAX_COLONIES; ci++) if (this.colonies[ci].stage === ST_FORM || this.colonies[ci].stage === ST_MATURE) { this.burst(ci); n++; } return n; }
  forceGerminate() { for (const sp of this.spores) sp.timer = 0; }
  // A colony where you say, growing at once: keepout ignored (water still
  // says no), and if every colony slot is taken the smallest colony is
  // evicted to make room. The way to watch growth from your own hex.
  plantSpore(q, r) {
    if (isWater(this.snap.terrain[r * MAP_COLS + q])) return false;
    const evict = (pick) => {
      let victim = -1, best = pick > 0 ? -1 : 1 << 30;
      for (let ci = 0; ci < MAX_COLONIES; ci++) {
        const c = this.colonies[ci];
        if (c.stage === ST_FREE) continue;
        const n = c.agentCount | 0;
        if (pick > 0 ? n > best : n < best) { best = n; victim = ci; }
      }
      if (victim < 0) return;
      for (let i = 0; i < this.agents.length;) { if (this.agents[i].colony === victim) this.killAgent(i); else i++; }
      this.bodies = this.bodies.filter((b) => b.colony !== victim);
      this.colonies[victim] = { stage: ST_FREE };
    };
    if (this.freeColony() < 0) evict(-1);                       // no slot: the smallest colony goes
    while (this.agents.length + 128 > MAX_AGENTS) evict(+1);    // no agents: the largest goes (a full board spawns nothing)
    return this.germinate({ q, r, generation: this.wave, waveCap: FIRST_CAP });
  }
  plantDaisy(q, r, stage, count) {
    let d = this.daisyAt(q, r);
    if (!d) { this.daisySeed(q, r); d = this.daisyAt(q, r); }
    if (!d) return null;
    d.stage = clamp(stage | 0, 0, 3); d.dawns = d.stage; d.count = clamp(count | 0 || d.count, 1, 7);
    this.bloom.fill(0);
    for (const x of this.daisies) if (x.stage === 3) this.bloom[x.r * MAP_COLS + x.q] = 1;
    return d;
  }
}

module.exports = { Ecology, ECO_TICK_MS, HEXES, MAP_COLS, MAP_ROWS };
