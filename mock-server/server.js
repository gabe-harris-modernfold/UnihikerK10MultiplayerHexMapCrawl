// Mock Wasteland Crawl server — serves data/ statically and fakes /ws so the
// client can connect, pick a character, see a map, and move.

const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const DATA_DIR = path.resolve(__dirname, '..', 'data');
const argPort  = (process.argv.find(a => a.startsWith('--port=')) || '').slice(7);
const PORT     = Number(argPort) || process.env.PORT || 8765;
// --bundle / MOCK_BUNDLE=1: serve the built data/assets.json + app.bundle.js
// (what the K10 serves) instead of the synthesised per-file dev manifest.
const BUNDLE_MODE = !!process.env.MOCK_BUNDLE || process.argv.includes('--bundle');

const MAP_COLS = 75, MAP_ROWS = 57, MAX_PLAYERS = 6;

const hex2 = (n) => n.toString(16).padStart(2, '0');

// ── Derive image variant counts from data/img — mirrors setupVariantCounts()
// in game-server.hpp so the client loads the same hex<Name><N>.png set the
// firmware would find on the SD card.
const TERRAIN_IMG_NAMES = [
  'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
  'BrokenUrban', 'FloodedDistrict', 'GlassFields',
  'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
  'BunkerEntrance', 'VentShaft', 'TunnelFloor', 'TunnelCollapsed',
];
const SHELTER_IMG_NAMES = ['shelterBasic', 'shelterImproved'];

function scanVariantCounts() {
  let files = [];
  try {
    files = fs.readdirSync(path.join(DATA_DIR, 'img'));
  } catch {
    // no img dir — leave everything at 0
  }
  const countFor = (prefix) => {
    let max = 0;
    for (const fname of files) {
      if (!fname.startsWith(prefix) || !fname.endsWith('.png')) continue;
      const numStr = fname.slice(prefix.length, -4);
      if (numStr.length === 0 || !/^\d+$/.test(numStr)) continue;
      max = Math.max(max, parseInt(numStr, 10) + 1);
    }
    return max;
  };
  const vc = TERRAIN_IMG_NAMES.map((name) => countFor(`hex${name}`));
  const sv = SHELTER_IMG_NAMES.map((name) => countFor(name));
  const fa = countFor('forrageAnimal');
  console.log(`[variants] terrain=${JSON.stringify(vc)} shelter=${JSON.stringify(sv)} forrageAnimal=${fa}`);
  return { vc, sv, fa };
}
const VARIANT_COUNTS = scanVariantCounts();

// ── Resource state ──────────────────────────────────────────────────────────
// "q_r" -> { res: 1-5 (0 = empty), amt, respawnTimer }
const resources = {};
const RESPAWN_TICKS = 8;       // ~8 seconds (1 tick/sec)
const INV_SLOTS     = 6;       // small cap so we can test inv-full path
const RES_MAX_TYPE  = 5;

// ── Build a fully-revealed map with sprinkled features ──────────────────────
// 6 hex chars per cell: TT (terrain) DD (data: footprints/shelter/poi) VV (resource/variant)
function buildMap() {
  let s = '';
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      const idx = r * MAP_COLS + c;
      const tt  = idx % 11;                              // terrain 0..10
      let   dd  = 0;
      let   vv  = 0;
      if (idx % 17 === 0) {
        const rType = (idx % 5) + 1;                     // 1..5
        vv = (rType << 4) | (idx & 0x0F);
        resources[`${c}_${r}`] = { res: rType, amt: 2 + (idx % 2), respawnTimer: 0 };
      }
      if (idx % 31 === 7) dd |= 0x80;                    // POI
      if (idx % 23 === 3) dd |= 0x40;                    // shelter
      s += hex2(tt) + hex2(dd) + hex2(vv);
    }
  }
  return s;
}
const MAP_HEX = buildMap();

// POIs consumed by enc_start ("q_r"). Cleared from the 0x80 bit in every
// map/visdisk we send so the eye disappears like it does on the firmware.
const consumedPoi = new Set();
// Shelters destroyed by a quake fault line ("q_r"). Cleared from the 0x40
// bit the same way — permanent, same as a consumed POI.
const destroyedShelters = new Set();
// Terrain overwritten by a quake ("q_r" -> new terrain index). MAP_HEX is a
// static baked string, so — same trick as the two Sets above — this overlay
// is consulted everywhere terrain is read instead of mutating MAP_HEX itself.
const terrainOverrides = new Map();
// Hexes a vehicle has driven through ("q_r") — the caravan (every step) or a
// player riding items.cfg "tracks"-flagged gear (the Motorbike) — mirrors
// HexCell.tireTrack in world-system.hpp. OR'd into TT bit 7 the same way a
// quake's terrain swap replaces TT outright; never cleared (parity with
// footprints/POI/shelter overlays — this is a "wasteland map reset" concern,
// not one this mock simulates). Kept the caravan-era name since it's still
// mostly caravan traffic and a rename would ripple through every use below.
const caravanTracks = new Set();
function ddFor(c, r, ddNum) {
  let out = ddNum;
  if (consumedPoi.has(`${c}_${r}`))       out &= ~0x80;
  if (destroyedShelters.has(`${c}_${r}`)) out &= ~0x40;
  return out;
}
function ttFor(c, r, ttNum) {
  const ov  = terrainOverrides.get(`${c}_${r}`);
  const out = ov === undefined ? ttNum : ov;
  return caravanTracks.has(`${c}_${r}`) ? (out | 0x80) : out;
}

// Live snapshot of current map state — encodes cells with up-to-date resource
// info so reconnects/syncs reflect collection truth.
// The map a `sync` carries. Fogged to the receiving player's sight radius,
// mirroring encodeMapFog() in hex-map.hpp: every hex past visR goes out as
// 0xFF and the client has no terrain for it at all.
//
// This used to send the whole board in the clear, which handed the client
// omniscience the firmware never grants and made every fog-of-war behaviour
// untestable offline — anything gated on "have I seen this hex" looked
// correct here because the answer was always yes. Pass (pq, pr, vr) to fog;
// called with no arguments it still returns the full board, which is what
// the debug /state dump wants.
function liveMapHex(pq, pr, vr) {
  const fogged = (vr !== undefined);
  let s = '';
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      if (fogged && hexDistWrap(pq, pr, c, r) > vr) { s += 'FF0000'; continue; }
      const baseIdx = (r * MAP_COLS + c) * 6;
      const ttNum = ttFor(c, r, parseInt(MAP_HEX.substr(baseIdx, 2), 16));
      const dd = MAP_HEX.substr(baseIdx + 2, 2);
      const ddNum = ddFor(c, r, parseInt(dd, 16));
      const cell = resources[`${c}_${r}`];
      const variant = baseIdx & 0x0F;
      const res     = cell ? cell.res : 0;
      const vv = hex2((res << 4) | (variant & 0x0F));
      s += hex2(ttNum) + hex2(ddNum) + vv;
    }
  }
  return s;
}

// Build a visdisk around (pq, pr) reflecting live resource state.
function buildVisDisk(pq, pr, vr) {
  let cells = '';
  for (let dr = -vr; dr <= vr; dr++) {
    for (let dq = -vr; dq <= vr; dq++) {
      const s = -(dq + dr);
      if (Math.abs(dq) + Math.abs(dr) + Math.abs(s) > 2 * vr) continue;
      const cq = ((pq + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
      const cr = ((pr + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
      const baseIdx = (cr * MAP_COLS + cq) * 6;
      const tt = hex2(ttFor(cq, cr, parseInt(MAP_HEX.substr(baseIdx, 2), 16)));
      const dd = hex2(ddFor(cq, cr, parseInt(MAP_HEX.substr(baseIdx + 2, 2), 16)));
      const cell = resources[`${cq}_${cr}`];
      const variant = baseIdx & 0x0F;
      const res     = cell ? cell.res : 0;
      const vv = hex2((res << 4) | (variant & 0x0F));
      cells += hex2(cq) + hex2(cr) + tt + dd + vv;
    }
  }
  return cells;
}

// ── Bunker tunnel board ─────────────────────────────────────────────────────
// Mirrors tunnels.hpp: a second, much smaller hex board reached through hatch
// hexes (terrain 12/13) on the surface. Wire-compatible with the firmware; the
// generation is a simplified stand-in, not the same algorithm.
//
// Terrain: 12 Bunker Entrance, 13 Vent Shaft (both boards), 14 Tunnel Floor,
// 15 Collapsed Tunnel (impassable).
const TUN_COLS = 16, TUN_ROWS = 10, MAX_HATCHES = 8;
const TUNNEL_MC = 2, VENT_ASCEND_MP = 2;
// Bad air — mirrors TUNNEL_REST_LL_PCT in tunnels.hpp. Underground the dawn
// exposure tick is off entirely (a corridor is cover), and this is what the
// tunnels charge in its place, rolled only when the survivor actually slept
// down there.
const TUNNEL_REST_LL_PCT = 30;
const TUNNEL_VIS_BASE = 1, TUNNEL_VIS_SCOUT = 1;
const ITEM_BILE_FLARE = 54;

// tunnel[r][q] = { tt, res, amt, variant, poi, footprints }
const tunnel = [];
const bunkerHatches = [];          // { sq, sr, tq, tr }

function tunIn(q, r) { return q >= 0 && q < TUN_COLS && r >= 0 && r < TUN_ROWS; }

// Distance metric for fog radius. The firmware uses proper axial hex distance,
// but DIR_DELTA above is the mock's own "approximate — mostly works" geometry
// (note it has no (+-1, 0) step), so an axial metric would disagree with what
// a player can actually walk. Chebyshev matches these deltas closely enough
// for a fog disc, and the client just renders whatever cells arrive.
function tunDist(q1, r1, q2, r2) {
  return Math.max(Math.abs(q2 - q1), Math.abs(r2 - r1));
}

// Is (q,r) one of the shafts? Keyed on bunkerHatches[] rather than the terrain
// id, because buildTunnels() needs the answer before the 12/13 stamping pass.
function isShaftCell(q, r) {
  return bunkerHatches.some((h) => h.tq === q && h.tr === r);
}

// Carve with the SAME deltas the move handler uses, so every corridor cell is
// reachable by an actual player move. Greedy on squared Euclidean distance,
// with a ~25% random legal step so corridors bend. Shaft cells are walls to
// this walk: stepping onto one climbs out (stepUpIfShaft), so a corridor
// routed through a shaft would eject anyone merely passing by.
function carveCorridor(q1, r1, q2, r2) {
  let q = q1, r = r1;
  for (let step = 0; step < TUN_COLS * TUN_ROWS * 4; step++) {
    if (q === q2 && r === r2) return;
    const legal = [];
    let bestD = -1, bestDist = Infinity;
    for (let d = 0; d < 6; d++) {
      const [dq, dr] = DIR_DELTA[d];
      const nq = q + dq, nr = r + dr;
      if (!tunIn(nq, nr) || isShaftCell(nq, nr)) continue;
      legal.push(d);
      const ddq = q2 - nq, ddr = r2 - nr;
      const dist = ddq * ddq + ddr * ddr;
      if (dist < bestDist) { bestDist = dist; bestD = d; }
    }
    if (!legal.length) return;
    const d = (Math.random() < 0.25) ? legal[(Math.random() * legal.length) | 0] : bestD;
    const [dq, dr] = DIR_DELTA[d];
    q += dq; r += dr;
    if (tunnel[r][q].tt === 15) tunnel[r][q].tt = 14;
  }
}

function buildTunnels() {
  // Surface hatches: the mock's terrain is a deterministic `idx % 11` stripe,
  // so rather than rejection-sampling we just pick 8 well-separated scrub-ish
  // cells and override them. terrainOverrides is the existing mechanism for
  // rewriting the baked MAP_HEX string.
  for (let i = 0; i < MAX_HATCHES; i++) {
    const sq = Math.round(4 + (i * (MAP_COLS - 9)) / (MAX_HATCHES - 1));
    const sr = 6 + ((i * 5) % (MAP_ROWS - 12));
    bunkerHatches.push({ sq, sr, tq: 0, tr: 0 });
    terrainOverrides.set(`${sq}_${sr}`, (i & 1) ? 13 : 12);
  }
  bunkerHatches.sort((a, b) => a.sq - b.sq);

  for (let r = 0; r < TUN_ROWS; r++) {
    tunnel.push([]);
    for (let q = 0; q < TUN_COLS; q++)
      tunnel[r].push({ tt: 15, res: 0, amt: 0, variant: 0, poi: 0, footprints: 0 });
  }

  const n = bunkerHatches.length;
  for (let i = 0; i < n; i++) {
    const b0 = Math.floor((i * TUN_COLS) / n);
    const b1 = Math.max(b0 + 1, Math.floor(((i + 1) * TUN_COLS) / n));
    const tq = Math.min(b0 + ((Math.random() * (b1 - b0)) | 0), TUN_COLS - 1);
    const tr = 1 + ((Math.random() * (TUN_ROWS - 2)) | 0);
    bunkerHatches[i].tq = tq;
    bunkerHatches[i].tr = tr;
    tunnel[tr][tq].tt = 14;
  }
  // Junctions: one adjacent floor cell per shaft, and it is the junctions the
  // chain runs between. A shaft must not be load-bearing -- stepping onto one
  // climbs straight out, so a chain carved shaft-to-shaft ejects anyone
  // walking from shaft i-1 to shaft i+1. Each shaft hangs off the network as a
  // one-hex spur instead. Mirrors the junction pass in generateTunnels().
  const jun = bunkerHatches.map((h) => {
    const pick = [];
    for (let d = 0; d < 6; d++) {
      const [dq, dr] = DIR_DELTA[d];
      const nq = h.tq + dq, nr = h.tr + dr;
      if (!tunIn(nq, nr) || isShaftCell(nq, nr)) continue;
      pick.push([nq, nr]);
    }
    if (!pick.length) return [h.tq, h.tr];          // walled in by edge and peers
    const [jq, jr] = pick[(Math.random() * pick.length) | 0];
    tunnel[jr][jq].tt = 14;                         // the spur off the shaft
    return [jq, jr];
  });

  for (let i = 0; i + 1 < n; i++)
    carveCorridor(jun[i][0], jun[i][1], jun[i + 1][0], jun[i + 1][1]);
  if (n >= 4) {
    carveCorridor(jun[0][0], jun[0][1], jun[n >> 1][0], jun[n >> 1][1]);
    carveCorridor(jun[1][0], jun[1][1], jun[n - 1][0], jun[n - 1][1]);
  }

  // Prove it rather than assume it: flood fill from junction 0 with every
  // shaft solid. Any junction that does not come back was reachable only by
  // cutting the corner across a shaft -- carve it a second way in and recheck.
  let shaftFree = false;
  for (let pass = 0; pass <= n && !shaftFree; pass++) {
    const seen = Array.from({ length: TUN_ROWS }, () => new Array(TUN_COLS).fill(false));
    const stack = [jun[0]];
    seen[jun[0][1]][jun[0][0]] = true;
    while (stack.length) {
      const [cq, cr] = stack.pop();
      for (let d = 0; d < 6; d++) {
        const [dq, dr] = DIR_DELTA[d];
        const nq = cq + dq, nr = cr + dr;
        if (!tunIn(nq, nr) || seen[nr][nq]) continue;
        if (tunnel[nr][nq].tt === 15 || isShaftCell(nq, nr)) continue;
        seen[nr][nq] = true;
        stack.push([nq, nr]);
      }
    }
    const orphan = jun.findIndex(([jq, jr], i) => i > 0 && !seen[jr][jq]);
    if (orphan < 0) { shaftFree = true; break; }
    if (pass === n) break;                          // last pass was verify-only
    console.log(`[tunnels] junction ${orphan} only reachable through a shaft - rerouting`);
    carveCorridor(jun[0][0], jun[0][1], jun[orphan][0], jun[orphan][1]);
  }
  // Shafts share the surface hatch's terrain id so the art reads the same
  // from either side.
  bunkerHatches.forEach((h, i) => { tunnel[h.tr][h.tq].tt = (i & 1) ? 13 : 12; });

  // Water (1) and Scrap (5) only — mirrors terrainSpawnRes(14) in hex-map.hpp.
  let floor = 0;
  for (let r = 0; r < TUN_ROWS; r++)
    for (let q = 0; q < TUN_COLS; q++) {
      if (tunnel[r][q].tt !== 14) continue;
      floor++;
      if (Math.random() < 0.19) {
        tunnel[r][q].res = Math.random() < 0.5 ? 1 : 5;
        tunnel[r][q].amt = 1 + ((Math.random() * 3) | 0);
      }
    }
  console.log(`[tunnels] ${TUN_COLS}x${TUN_ROWS} hatches=${n} floor=${floor} ` +
    `shaftFreePaths=${shaftFree ? 'ok' : 'FAILED'} ` +
    bunkerHatches.map((h, i) => `#${i}(${h.sq},${h.sr})->(${h.tq},${h.tr})`).join(' '));
}

// Valid-move bitmask for a player underground — mirrors computeValidMoves()
// in survival_state.hpp. The surface branch of the mock has no terrain cost
// logic at all, but the tunnels need this or the client cannot grey out a
// direction that walks into rock or off the edge of the board.
function tunnelValidMoves(p) {
  if (!p.dp || p.mp <= 0 || p.ll === 0) return 0;
  let mask = 0;
  for (let d = 0; d < 6; d++) {
    const [dq, dr] = DIR_DELTA[d];
    const nq = p.tq + dq, nr = p.tr + dr;
    if (!tunIn(nq, nr)) continue;
    if (tunnel[nr][nq].tt === 15) continue;   // Collapsed Tunnel is MC 255
    mask |= (1 << d);
  }
  return mask;
}

function hatchAtSurface(q, r) { return bunkerHatches.findIndex((h) => h.sq === q && h.sr === r); }
function hatchAtShaft(q, r)   { return bunkerHatches.findIndex((h) => h.tq === q && h.tr === r); }

// Underground sight: your hex plus one ring, +1 carrying a Bile Flare,
// +1 for a Scout (archetype 4). Mirrors playerVisParams() in hex-map.hpp.
function tunnelVis(p) {
  let vr = TUNNEL_VIS_BASE;
  if (p.it && p.it.some((id, i) => id === ITEM_BILE_FLARE && p.iq[i] > 0)) vr += 1;
  if (p.arch === 4) vr += TUNNEL_VIS_SCOUT;
  return vr;
}

// Surface sight. The mock does not model TERRAIN_VIS, the Scout bonus or
// equipment reveal — but it DOES model the weather penalty, because that is
// the one modifier big enough to zero the radius outright, and a flat vr: 4
// hid a whole class of bug: anything gated on fog of war looked fine here and
// vanished on the board the moment a storm rolled in. Table is verbatim from
// WEATHER_VIS_PENALTY in Esp32HexMapCrawl.ino; the subtraction mirrors
// playerVisParams() in hex-map.hpp. Like the firmware, this is only resampled
// when a vis disk is sent (on move), so a phase change mid-stand doesn't
// retroactively blind you until your next step.
const SURFACE_VIS_BASE   = 4;
const WEATHER_VIS_PENALTY = [0, 1, 2, 3, 0, 2];
function surfaceVis(p) {
  // equipVisionBonus before the weather penalty, same order as
  // playerVisParams(). Passing no player keeps the old bare-radius behaviour
  // for the few callers that have none to hand.
  const gear = p ? equipVisionBonus(p) : 0;
  return Math.max(0, SURFACE_VIS_BASE + gear - WEATHER_VIS_PENALTY[weatherPhase]);
}

function tunCellHex(q, r) {
  const c = tunnel[r][q];
  return hex2(c.tt) + hex2((c.footprints & 0x3F) | (c.poi ? 0x80 : 0)) +
         hex2(((c.res & 0xF) << 4) | (c.variant & 0x0F));
}

// Whole fogged tunnel board (960 hex chars at 16x10) — sent on descend.
function tunnelMapHex(pq, pr, vr) {
  let s = '';
  for (let r = 0; r < TUN_ROWS; r++)
    for (let q = 0; q < TUN_COLS; q++)
      s += (tunDist(q, r, pq, pr) <= vr) ? tunCellHex(q, r) : 'FF0000';
  return s;
}

// -- Crossing between the boards --------------------------------------------
// There is no DESCEND/ASCEND control: stepping onto a Bunker Entrance (12) or
// Vent Shaft (13) IS the transition. Mirrors tunnelStepDown()/tunnelStepUp()
// in tunnels.hpp -- called as the last act of a move, so arriving on the
// paired cell of the other board never re-triggers one. Both charge clamped at
// 0: the step has already landed, and refusing it would strand the player.
// Return true when the board changed, which is the caller's cue to skip the
// vis disk it would otherwise have sent for the board they just left.
function stepDownIfHatch(ws, id, p) {
  const h = hatchAtSurface(p.q, p.r);
  if (h < 0) return false;
  p.mp = Math.max(0, p.mp - 1);
  p.dp = 1; p.hatchIdx = h;
  p.tq = bunkerHatches[h].tq; p.tr = bunkerHatches[h].tr;
  tunnel[p.tr][p.tq].footprints |= (1 << id);
  tunnelTauntTicks[id] = 0;  // a moment's peace before the doubt starts
  broadcast({ t: 'ev', k: 'tun_in', pid: id, q: p.q, r: p.r, hatch: h, mp: p.mp });
  sendTunnelSync(ws, p);      // the client has never seen the tunnel board
  const tvr = tunnelVis(p);
  send(ws, { t: 'vis', dp: 1, q: p.tq, r: p.tr, vr: tvr,
             cells: buildTunnelVisDisk(p.tq, p.tr, tvr) });
  return true;
}

function stepUpIfShaft(ws, id, p) {
  const h = hatchAtShaft(p.tq, p.tr);
  if (h < 0) return false;
  p.mp = Math.max(0, p.mp - (tunnel[p.tr][p.tq].tt === 13 ? VENT_ASCEND_MP : 1));
  p.dp = 0; p.hatchIdx = h;
  p.q = bunkerHatches[h].sq; p.r = bunkerHatches[h].sr;
  broadcast({ t: 'ev', k: 'tun_out', pid: id, q: p.q, r: p.r, hatch: h, mp: p.mp });
  const svr = surfaceVis(p);
  send(ws, { t: 'vis', q: p.q, r: p.r, vr: svr, cells: buildVisDisk(p.q, p.r, svr) });
  return true;
}

function buildTunnelVisDisk(pq, pr, vr) {
  let cells = '';
  for (let dr = -vr; dr <= vr; dr++)
    for (let dq = -vr; dq <= vr; dq++) {
      const s = -(dq + dr);
      if (Math.abs(dq) + Math.abs(dr) + Math.abs(s) > 2 * vr) continue;
      const cq = pq + dq, cr = pr + dr;
      if (!tunIn(cq, cr)) continue;       // walled, not toroidal
      cells += hex2(cq) + hex2(cr) + tunCellHex(cq, cr);
    }
  return cells;
}

function sendTunnelSync(ws, p) {
  const vr = tunnelVis(p);
  send(ws, { t: 'tsync', cols: TUN_COLS, rows: TUN_ROWS, vr, q: p.tq, r: p.tr,
             map: tunnelMapHex(p.tq, p.tr, vr) });
}

// ── Player factory ──────────────────────────────────────────────────────────
// Mirrors ARCHETYPE_SKILLS / ARCHETYPE_INV_SLOTS in Esp32HexMapCrawl.ino.
// Slot index == archetype index, same as the firmware.
const ARCHETYPE_SKILLS = [
  [2, 1, 0, 1, 1],  // 0 Guide
  [0, 2, 1, 1, 0],  // 1 Quartermaster
  [0, 0, 1, 0, 2],  // 2 Medic
  [0, 1, 2, 1, 1],  // 3 Mule
  [2, 1, 1, 0, 1],  // 4 Scout
  [1, 0, 0, 2, 2],  // 5 Endurer
];
const ARCHETYPE_INV_SLOTS = [8, 8, 8, 12, 8, 8];
const NUM_SKILLS     = 5;
const WOUND_MAX_EACH = 3;
const TREAT_DN       = 9; // mirrors TREAT_DN in Esp32HexMapCrawl.ino
// INV_SLOTS_MAX in Esp32HexMapCrawl.ino — typed-item slots are a fixed,
// index-stable array (0 = empty), not a growable list. See ITEM_DEFS note below.
// 18, not 12: 12 was also the Mule's base, so slot-granting gear clamped to
// nothing for that archetype. Keep in step with the .ino and data/game-data.js.
const INV_SLOTS_MAX = 18;

// Social spawn — mirrors pickSpawnNearPlayer() in inventory_items.hpp: a
// joining survivor lands on a random neighbour hex of a randomly chosen
// survivor already in the world, and only falls back to the fixed start hex
// when nobody else is in play. The mock has no terrain-passability model, so
// unlike the firmware there is no passable/radioactive filtering here.
const SPAWN_DIRS = [
  [1, 0], [1, -1], [0, -1], [-1, 0], [-1, 1], [0, 1],
];

function pickSpawnPos(id) {
  const anchors = Object.values(players).filter((o) => o.id !== id && o.on && o.ll > 0);
  if (!anchors.length) return { q: 12, r: 9 };
  const host = anchors[(Math.random() * anchors.length) | 0];
  const [dq, dr] = SPAWN_DIRS[(Math.random() * SPAWN_DIRS.length) | 0];
  return {
    q: ((host.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS,
    r: ((host.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS,
  };
}

function makePlayer(id) {
  const spawn = pickSpawnPos(id);
  return {
    id, on: true,
    q: spawn.q, r: spawn.r,
    sc: 0,
    inv: [0, 0, 0, 0, 0],
    sp: 0,
    ll: 7, food: 6, water: 6, rad: 0,
    mp: 6,
    fth: 0, wth: 0,
    vm: 0x3F,
    rt: 0,
    it: new Array(INV_SLOTS_MAX).fill(0), iq: new Array(INV_SLOTS_MAX).fill(0),
    eq: [0, 0, 0, 0, 0], // EQUIP_SLOTS=5 in Esp32HexMapCrawl.ino — array, not {}; 0 = empty slot
    arch: id,
    is: ARCHETYPE_INV_SLOTS[id] ?? 8,
    sk: (ARCHETYPE_SKILLS[id] ?? [0, 0, 0, 0, 0]).slice(),
    wnd: [0, 0],
    enc: false,
    // Bunker tunnels (tunnels.hpp): q/r above always stay on the surface,
    // pinned to the hatch this player descended through.
    dp: 0, tq: 0, tr: 0, hatchIdx: 0,
    kr: STARTER_RECIPES,  // knownRecipes bitmask — bit (id-1) per known RecipeDef:
                          // starter recipes from spawn, the rest via encounters
  };
}

// ── Item registry — mirrors enough of boot-assets.hpp's loadItemRegistry() to
// validate equip/unequip/use/drop/pickup and apply a consumable's stat deltas.
// Equipment stat mods ARE modelled now: "slots" (effectiveInvSlots), "ll"
// (effectiveMaxLL), "mp" and the dawn *_cost gate (effectiveMP /
// applyDawnItemCosts), "rad" at dawn, and reveal_fog param 1 (surfaceVis).
// They used to be absent, which meant a whole class of equipment bug looked
// fine offline and only showed up on the board — the same trap the weather
// visibility penalty was added here to close.
// Still simplified: no terrain movement costs at all (every step is 1 MP), so
// the TERR_PASS_* perks cannot be exercised here.
const EQUIP_SLOT_BY_NAME = { head: 0, body: 1, hand: 2, feet: 3, vehicle: 4 };
function loadItemRegistry() {
  const defs = {};
  let text;
  try { text = fs.readFileSync(path.join(DATA_DIR, 'items.cfg'), 'utf8'); }
  catch { return defs; }
  let cur = null;
  for (const rawLine of text.split('\n')) {
    const line = rawLine.split('#')[0].trim();
    if (!line) continue;
    if (line === '[item]') { cur = { maxStack: 1 }; continue; }
    if (!cur) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    const val = line.slice(eq + 1).trim();
    if (key === 'id')            { cur.id = parseInt(val, 10) || 0; defs[cur.id] = cur; }
    else if (key === 'category') cur.category = val;
    else if (key === 'slot')     cur.eslot = EQUIP_SLOT_BY_NAME[val] ?? -1;
    else if (key === 'stack')    cur.maxStack = Math.max(1, parseInt(val, 10) || 1);
    else if (key === 'll')       cur.ll      = parseInt(val, 10) || 0;
    else if (key === 'food')     cur.food    = parseInt(val, 10) || 0;
    else if (key === 'water')    cur.water   = parseInt(val, 10) || 0;
    else if (key === 'rad')      cur.rad     = parseInt(val, 10) || 0;
    else if (key === 'mp')       cur.mp      = parseInt(val, 10) || 0;
    else if (key === 'slots')    cur.slots   = parseInt(val, 10) || 0;   // pack slot bonus while equipped
    else if (key === 'effect')   cur.effect  = val;
    else if (key === 'param')    cur.param   = parseInt(val, 10) || 0;
    else if (key === 'effect2')  cur.effect2 = val;
    else if (key === 'param2')   cur.param2  = parseInt(val, 10) || 0;
    else if (key === 'trade')    cur.trade   = /^(y|1)/i.test(val);    // caravan may stock it (consumables only)
    else if (key === 'value')    cur.value   = parseInt(val, 10) || 0;  // caravan asking price, tokens of any mix
    else if (key === 'tracks')   cur.tracks  = /^(y|1)/i.test(val);    // leaves a tire track when equipped and moved
    // Daily operating costs, same order as inv[]: water, food, fuel, med, scrap.
    else if (key === 'water_cost') (cur.opCost ??= [0,0,0,0,0])[0] = parseInt(val, 10) || 0;
    else if (key === 'food_cost')  (cur.opCost ??= [0,0,0,0,0])[1] = parseInt(val, 10) || 0;
    else if (key === 'fuel_cost')  (cur.opCost ??= [0,0,0,0,0])[2] = parseInt(val, 10) || 0;
    else if (key === 'med_cost')   (cur.opCost ??= [0,0,0,0,0])[3] = parseInt(val, 10) || 0;
    else if (key === 'scrap_cost') (cur.opCost ??= [0,0,0,0,0])[4] = parseInt(val, 10) || 0;
  }
  return defs;
}

// ── Equipment queries — mirror inventory_items.hpp ──────────────────────────
// Does this equipped item have a daily cost? Its mp only applies on a dawn
// where that cost was actually paid (applyDawnItemCosts).
function itemHasOpCost(def) {
  return !!(def?.opCost && def.opCost.some(c => c > 0));
}

// 7 plus every equipped item's "ll", minus any permanent penalty from
// Uranium Candy. Mirrors effectiveMaxLL(); LL_CAP is the base it starts from.
function effectiveMaxLL(p) {
  let cap = LL_CAP - (p.llCapPenalty | 0);
  for (const eid of p.eq) if (eid) cap += (ITEM_DEFS[eid]?.ll | 0);
  return Math.max(1, cap);
}

// Mirrors effectiveMP(): halved LL slope, minus major wounds, minus the
// encumbrance penalty, plus every NON-cost-gated item's mp. Cost-gated mp is
// added by applyDawnItemCosts() only when the cost was paid.
function effectiveMP(p) {
  let mp = 6 + ((p.ll + 1) >> 1) - (p.wnd[1] | 0);
  const carried = p.inv.reduce((a, b) => a + b, 0);
  if (carried > effectiveInvSlots(p)) mp--;
  for (const eid of p.eq) {
    const def = ITEM_DEFS[eid];
    if (def && def.mp && !itemHasOpCost(def)) mp += def.mp;
  }
  return Math.max(2, mp);
}

// Charge each equipped item's daily cost and grant its mp if it was paid.
// Returns the bitmask of slots that could NOT pay, which rides EVT_DAWN as
// "unf" so the client can grey out a bonus that is dormant today.
function applyDawnItemCosts(p) {
  let unfuelled = 0;
  const downed = p.ll === 0;
  p.eq.forEach((eid, slot) => {
    const def = ITEM_DEFS[eid];
    if (!def || !itemHasOpCost(def)) return;
    const canAfford = !downed && def.opCost.every((c, k) => c <= p.inv[k]);
    if (canAfford) {
      def.opCost.forEach((c, k) => { p.inv[k] -= c; });
      p.mp = Math.min(127, p.mp + (def.mp | 0));
    } else if (!downed) {
      unfuelled |= (1 << slot);
    }
  });
  return unfuelled;
}

// Spare resource-token capacity. Mirrors tokenRoomFor() in inventory_items.hpp.
function tokenRoomFor(p) {
  const carried = p.inv.reduce((a, b) => a + b, 0);
  return Math.max(0, effectiveInvSlots(p) - carried);
}

// +1 vision per equipped item with reveal_fog param 1 (Dark Goggles, Glow
// Dentures, Doom Clicker). Mirrors equipVisionBonus().
function equipVisionBonus(p) {
  let bonus = 0;
  for (const eid of p.eq) {
    const def = ITEM_DEFS[eid];
    if (!def) continue;
    if (def.effect  === 'reveal_fog' && def.param  === 1) bonus++;
    if (def.effect2 === 'reveal_fog' && def.param2 === 1) bonus++;
  }
  return bonus;
}
const ITEM_DEFS = loadItemRegistry();

// ── Recipe registry — mirrors boot-assets.hpp's loadRecipeRegistry() for
// /data/recipes.cfg. Recipes are secret: a player only sees/can-craft one
// once its bit is set in p.kr (learned via an encounter's "recipe" loot).
function loadRecipeRegistry() {
  const defs = {};
  let text;
  try { text = fs.readFileSync(path.join(DATA_DIR, 'recipes.cfg'), 'utf8'); }
  catch { return defs; }
  let cur = null;
  for (const rawLine of text.split('\n')) {
    const line = rawLine.split('#')[0].trim();
    if (!line) continue;
    if (line === '[recipe]') { cur = { outputQty: 1, matItem: [0, 0, 0], matQty: [0, 0, 0], resCost: [0, 0, 0, 0, 0] }; continue; }
    if (!cur) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    const val = line.slice(eq + 1).trim();
    if      (key === 'id')          { cur.id = parseInt(val, 10) || 0; defs[cur.id] = cur; }
    else if (key === 'name')        cur.name       = val;
    else if (key === 'output_item') cur.outputItem = parseInt(val, 10) || 0;
    else if (key === 'output_qty')  cur.outputQty  = Math.max(1, parseInt(val, 10) || 1);
    else if (key === 'mat1')        cur.matItem[0] = parseInt(val, 10) || 0;
    else if (key === 'matqty1')     cur.matQty[0]  = parseInt(val, 10) || 0;
    else if (key === 'mat2')        cur.matItem[1] = parseInt(val, 10) || 0;
    else if (key === 'matqty2')     cur.matQty[1]  = parseInt(val, 10) || 0;
    else if (key === 'mat3')        cur.matItem[2] = parseInt(val, 10) || 0;
    else if (key === 'matqty3')     cur.matQty[2]  = parseInt(val, 10) || 0;
    else if (key === 'starter')     cur.starter    = /^(y|1)/i.test(val);   // known from spawn, no encounter
    else if (key === 'water_cost')  cur.resCost[0] = parseInt(val, 10) || 0;
    else if (key === 'food_cost')   cur.resCost[1] = parseInt(val, 10) || 0;
    else if (key === 'fuel_cost')   cur.resCost[2] = parseInt(val, 10) || 0;
    else if (key === 'med_cost')    cur.resCost[3] = parseInt(val, 10) || 0;
    else if (key === 'scrap_cost')  cur.resCost[4] = parseInt(val, 10) || 0;
  }
  return defs;
}
// Pack size in effect right now: archetype base (p.is) plus every equipped
// item's items.cfg "slots" bonus, clamped to the fixed it[]/iq[] array width.
// Mirrors effectiveInvSlots() in inventory_items.hpp — the ONE number every
// pack loop and carry-cap check in this file must use, never p.is directly.
function effectiveInvSlots(p) {
  let slots = p.is | 0;
  for (const eid of p.eq) if (eid) slots += (ITEM_DEFS[eid]?.slots | 0);
  return Math.max(1, Math.min(INV_SLOTS_MAX, slots));
}

// Wire view of a player: the live object plus the two EFFECTIVE numbers the
// client draws from. p.is stays the ARCHETYPE BASE internally (that is what
// effectiveInvSlots adds the equipment on top of), so the substitution has to
// happen here, at serialisation, exactly once -- mirrors appendPackArrays()
// in inventory_items.hpp, which is the only place the firmware emits them.
function playerView(p) {
  return { ...p, is: effectiveInvSlots(p), llCap: effectiveMaxLL(p) };
}
// The same two fields for a targeted item_result ack.
function packFields(p) {
  return { is: effectiveInvSlots(p), llCap: effectiveMaxLL(p) };
}

const RECIPE_DEFS = loadRecipeRegistry();

// knownRecipes bits every survivor spawns with — mirrors starterRecipeMask()
// in boot-assets.hpp. `starter = yes` in recipes.cfg is basic know-how that
// needs no encounter; everything else stays secret until one is banked.
const STARTER_RECIPES = Object.values(RECIPE_DEFS).reduce(
  (m, r) => (r.starter && r.id >= 1 && r.id <= 32) ? (m | (1 << (r.id - 1))) : m, 0);

// Mirrors applyRecipe() in inventory_items.hpp: returns null on success, else
// the same player-facing refusal string the firmware toasts. Resource and
// material affordability plus the output-room check all run on a scratch
// copy of the pack before any mutation — and the room check sees the pack
// AFTER the materials come out, so a full pack whose last unit of a material
// occupies a slot can still take the result there.
function craftRecipe(p, recipeId) {
  const r = RECIPE_DEFS[recipeId];
  if (!r || !r.outputItem) return 'No such recipe';
  for (let i = 0; i < 5; i++) if (r.resCost[i] > (p.inv[i] || 0)) return 'Not enough resources for that recipe';

  const it = [...p.it], iq = [...p.iq];
  for (let m = 0; m < 3; m++) {
    if (!r.matItem[m]) continue;
    let need = r.matQty[m];
    for (let s = 0; s < INV_SLOTS_MAX && need > 0; s++) {
      if (it[s] !== r.matItem[m]) continue;
      const take = Math.min(need, iq[s]);
      iq[s] -= take; need -= take;
      if (!iq[s]) it[s] = 0;
    }
    if (need > 0) return 'Missing materials for that recipe';
  }
  if (invRoomFor(it, iq, effectiveInvSlots(p), r.outputItem) < r.outputQty) return 'No room in your pack for the result';

  for (let i = 0; i < 5; i++) p.inv[i] -= r.resCost[i];
  for (let s = 0; s < INV_SLOTS_MAX; s++) { p.it[s] = it[s]; p.iq[s] = iq[s]; }
  addItemToInv(p, r.outputItem, r.outputQty);
  return null;
}

// ── Ground items — mirrors GroundItem groundItems[MAX_GROUND] in the .ino ───
const MAX_GROUND = 32;
const groundItems = Array.from({ length: MAX_GROUND }, () => ({ q: 0, r: 0, itemType: 0, qty: 0 }));
function groundItemsList() {
  const out = [];
  for (let g = 0; g < MAX_GROUND; g++) {
    if (groundItems[g].itemType) out.push({ g, q: groundItems[g].q, r: groundItems[g].r, id: groundItems[g].itemType, n: groundItems[g].qty });
  }
  return out;
}
function groundUpdateMsg(q, r) {
  return { t: 'ground_update', q, r, gi: groundItemsList() };
}

// ── Encounters (mirrors network-msg-encounter.hpp closely enough for UI work) ──
const ENC_INDEX = (() => {
  try { return JSON.parse(fs.readFileSync(path.join(DATA_DIR, 'encounters', 'index.json'), 'utf8')); }
  catch { return {}; }
})();
const encounters = {};   // pid -> { q, r, biome, encId, json, nodeKey, canBank, pendingLoot[5], pendingItems[], fullClear }

// Load an encounter file the same way the firmware's encLoadFile() does.
function loadEncounterJson(biome, encId) {
  try { return JSON.parse(fs.readFileSync(path.join(DATA_DIR, 'encounters', biome, `${encId}.json`), 'utf8')); }
  catch (e) { console.warn(`[enc] cannot load ${biome}/${encId}.json: ${e.message}`); return null; }
}

// Roll a loot table from data/encounters/loot_tables.json (mirrors rollLootTable()).
const LOOT_TABLES = (() => {
  try { return JSON.parse(fs.readFileSync(path.join(DATA_DIR, 'encounters', 'loot_tables.json'), 'utf8')); }
  catch { return {}; }
})();
function rollLootTable(name) {
  const tbl = LOOT_TABLES[name];
  if (!Array.isArray(tbl) || !tbl.length) return null;
  const total = tbl.reduce((a, e) => a + (e.weight | 0), 0);
  if (!total) return null;
  let roll = Math.floor(Math.random() * total);
  for (const e of tbl) {
    roll -= (e.weight | 0);
    if (roll < 0) {
      const mn = e.qty?.[0] | 0, mx = e.qty?.[1] ?? mn;
      const qty = mn + Math.floor(Math.random() * (Math.max(mn, mx) - mn + 1));
      return (e.item | 0) && qty ? { it: e.item | 0, iq: qty } : null;
    }
  }
  return null;
}

// Mirrors encResolveChoice() in encounter_engine.hpp.
function resolveChoice(json, nodeKey, ci) {
  const ch = json?.nodes?.[nodeKey]?.choices?.[ci];
  if (!ch) return null;
  const cost = ch.cost ?? {};
  const nextKey = ch.success_node ?? '';
  const next = nextKey ? json.nodes?.[nextKey] : null;
  const out = {
    baseRisk: Math.max(0, Math.min(100, ch.base_risk ?? 50)),
    skill:    Math.max(0, Math.min(NUM_SKILLS - 1, ch.skill | 0)),
    cost:     { ll: cost.ll | 0, rad: cost.radiation | 0, food: cost.food | 0,
                water: cost.water | 0, scrap: cost.scrap | 0, med: cost.med | 0 },
    nextKey, nextCanBank: true, nextTerminal: true,
    loot: [0, 0, 0, 0, 0], items: [], lootTable: '', recipeId: 0,
    hazLL: 0, hazRad: 0, hazRes: [0, 0, 0, 0, 0], hazWMin: 0, hazWMaj: 0, hazEnds: false,
  };
  if (next) {
    out.nextCanBank  = !!next.can_bank;
    out.nextTerminal = !(Array.isArray(next.choices) && next.choices.length);
    out.lootTable    = next.loot_table ?? '';
    for (const e of next.loot ?? []) {
      const mn = e.qty?.[0] ?? 1, mx = e.qty?.[1] ?? mn;
      const q  = Math.max(0, Math.min(99, mn + Math.floor(Math.random() * (Math.max(mn, mx) - mn + 1))));
      if (e.res !== undefined) { if (e.res >= 0 && e.res < 5) out.loot[e.res] = Math.min(99, out.loot[e.res] + q); }
      else if (e.item !== undefined && q && out.items.length < 2) out.items.push({ it: e.item | 0, iq: q });
      // "recipe": N — a one-time knowledge grant, no qty involved.
      else if (e.recipe !== undefined && (e.recipe | 0) > 0) out.recipeId = e.recipe | 0;
    }
  }
  const haz = ch.hazard_id ? json.hazards?.[ch.hazard_id] : null;
  if (haz) {
    const pen = haz.penalty ?? {};
    out.hazLL  = pen.ll | 0;
    out.hazRad = pen.radiation | 0;
    ['water', 'food', 'fuel', 'med', 'scrap'].forEach((k, i) => { const v = pen[k] | 0; if (v < 0) out.hazRes[i] = Math.min(99, -v); });
    out.hazWMin = Math.max(0, Math.min(WOUND_MAX_EACH, haz.wound?.[0] | 0));
    out.hazWMaj = Math.max(0, Math.min(WOUND_MAX_EACH, haz.wound?.[1] | 0));
    out.hazEnds = !!haz.ends_encounter;
  }
  return out;
}

function openEncounter(ws, id, p, q, r, biome, encId, consumePoi) {
  const json = loadEncounterJson(biome, encId);
  if (!json) { send(ws, { t: 'err', msg: 'The way in is blocked' }); return false; }
  if (consumePoi) consumedPoi.add(`${q}_${r}`);
  if (threatClock < 20) threatClock++;
  const startKey = json.nodes?.[json.start_node] ? json.start_node : Object.keys(json.nodes ?? {})[0];
  const start = json.nodes?.[startKey];
  encounters[id] = { q, r, biome, encId, json, nodeKey: startKey,
                     canBank: !!start?.can_bank,
                     pendingLoot: [0, 0, 0, 0, 0], pendingItems: [], pendingRecipes: 0,
                     fullClear: !(Array.isArray(start?.choices) && start.choices.length) };
  p.enc = true;
  send(ws, { t: 'enc_path', biome, id: encId });
  broadcast({ t: 'ev', k: 'enc_start', pid: id, q, r });
  console.log(`[enc] start pid=${id} ${biome}/${encId}.json node=${startKey}`);
  return true;
}
let   threatClock = 0;
let   forcedOutcome = null;   // dbg_force: null | 0 | 1 — overrides the next roll

function terrainAt(q, r) {
  return ttFor(q, r, parseInt(MAP_HEX.substr((r * MAP_COLS + q) * 6, 2), 16));
}
function hasPoi(q, r) {
  const dd = parseInt(MAP_HEX.substr((r * MAP_COLS + q) * 6 + 2, 2), 16);
  return (dd & 0x80) !== 0 && !consumedPoi.has(`${q}_${r}`);
}
function computeDN(p, baseRisk) {
  let risk = Math.min(baseRisk, 100);
  if (threatClock >= 5)  risk += 5;
  if (threatClock >= 9)  risk += 5;
  if (threatClock >= 13) risk += 5;
  if (threatClock >= 17) risk += 5;
  risk = Math.max(0, Math.min(100, risk));
  let dn = 5 + Math.floor((risk * 7) / 100);   // mirrors computeEncounterDN()
  let bonus = 0;
  if (p.ll > 4)  bonus += Math.floor((p.ll - 4) / 2);
  if (p.rad > 3) dn    += Math.floor((p.rad - 3) / 2);
  return Math.max(2, Math.min(12, dn - bonus));
}
const d6 = () => 1 + Math.floor(Math.random() * 6);

// ── Day / dawn cycle — mirrors tickGame()'s early-dawn-on-all-resting check
// (actions_game_loop.hpp) and dawnUpkeep() (survival_state.hpp) closely enough
// to exercise REST end-to-end. Weather is a simplified 3-roll cycle, not the
// firmware's full Markov chain with bad-weather-streak capping.
const DAY_TICKS  = 3000;  // TICK_MS(100) x 3000 = 5 min/day, matches firmware
const TICK_MS    = 100;
const LL_CAP     = 7;     // effectiveMaxLL() base cap — mock has no equip stat mods
const TERRAIN_SV = [0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 0]; // mirrors TERRAIN_SV[] in Esp32HexMapCrawl.ino
const WEATHER_NAMES = ['CLEAR', 'RAIN', 'STORM', 'CHEM', 'STRANGLE FOG', 'FOG'];

let dayTick      = 0;
let dayCount     = 0;
let weatherPhase = 0;

function hasShelter(q, r) {
  if (destroyedShelters.has(`${q}_${r}`)) return false;
  const dd = parseInt(MAP_HEX.substr((r * MAP_COLS + q) * 6 + 2, 2), 16);
  return (dd & 0x40) !== 0;
}

// Move the F/W track by dir (+1/-1), clamp [1,6], and flag an LL delta on each
// threshold crossing — ports applyFStep()/applyWStep() from survival_skills.hpp.
function applyFStep(p, dir, state) {
  const oldF = p.food;
  if (dir > 0) {
    if (p.food < 6) p.food++;
    if (oldF < 4 && p.food >= 4 && (p.fth & 1)) { p.fth &= ~1; state.llDelta++; }
    if (oldF < 2 && p.food >= 2 && (p.fth & 2)) { p.fth &= ~2; state.llDelta++; }
    if (oldF < 2 && p.food >= 2 && (p.fth & 4)) { p.fth &= ~4; state.llDelta++; }
  } else {
    if (p.food > 1) {
      p.food--;
      if (oldF >= 4 && p.food < 4 && !(p.fth & 1)) { p.fth |= 1; state.llDelta--; }
      if (oldF >= 2 && p.food < 2 && !(p.fth & 2)) { p.fth |= 2; state.llDelta--; }
    } else {
      // F already at floor 1 — the box-1 crossing, mirrors applyWStep's bit2
      if (!(p.fth & 4)) { p.fth |= 4; state.llDelta--; }
    }
  }
}
function applyWStep(p, dir, state) {
  const oldW = p.water;
  if (dir > 0) {
    if (p.water < 6) p.water++;
    if (oldW < 5 && p.water >= 5 && (p.wth & 1)) { p.wth &= ~1; state.llDelta++; }
    if (oldW < 3 && p.water >= 3 && (p.wth & 2)) { p.wth &= ~2; state.llDelta++; }
    if (oldW < 2 && p.water >= 2 && (p.wth & 4)) { p.wth &= ~4; state.llDelta++; }
  } else if (p.water > 1) {
    p.water--;
    if (oldW >= 5 && p.water < 5 && !(p.wth & 1)) { p.wth |= 1; state.llDelta--; }
    if (oldW >= 3 && p.water < 3 && !(p.wth & 2)) { p.wth |= 2; state.llDelta--; }
  } else if (!(p.wth & 4)) {
    p.wth |= 4; state.llDelta--;
  }
}

function healWound(p, tier) {
  if (!p.wnd[tier]) return false;
  p.wnd[tier]--;
  return true;
}

// Runs dawn upkeep for every connected player — mirrors dawnUpkeep() end to end
// (food/water consumption, exposure, shelter-protects-rest, rest recovery),
// then resets each player's resting flag and refills MP, and emits a proper
// EVT_DAWN-shaped event so the client's day counter / toast / REST button reset.
function dawnUpkeepAll() {
  dayCount++;
  if (threatClock > 0) threatClock--;
  advanceWeather();

  for (const idStr of Object.keys(players)) {
    const id = Number(idStr);
    const p  = players[id];
    if (!p || p.ll === 0) continue; // downed — skipped, same as firmware

    const state = { llDelta: 0 };
    // Settlement flag — food/water consumption exemption removed; still used
    // below to guarantee the rest-recovery heal regardless of supply levels.
    const inSettlement = terrainAt(p.q, p.r) === 9;

    // Equipment radiation delta at dawn (Glow Suit -1, Lead Snuggie -5).
    // Signed, matching items.cfg and survival_state.hpp.
    for (const eid of p.eq) {
      const rd = ITEM_DEFS[eid]?.rad | 0;
      if (rd) p.rad = Math.max(0, Math.min(10, p.rad + rd));
    }

    if (p.inv[1] > 0) { p.inv[1]--; applyFStep(p, +1, state); }
    else                applyFStep(p, -1, state);

    const use = Math.min(p.inv[0], 2);
    p.inv[0] -= use;
    for (let i = 0; i < use;     i++) applyWStep(p, +1, state);
    for (let i = 0; i < 2 - use; i++) applyWStep(p, -1, state);

    let expDelta = 0;
    const sv       = TERRAIN_SV[terrainAt(p.q, p.r)] ?? 0;
    // `|| p.dp` mirrors survival_state.hpp: underground there is no weather to
    // be exposed to, and q/r stay pinned to the hatch down there, so without
    // it this reads the surface hex and the answer depends on whether the
    // survivor happened to descend through an Entrance (SV 2) or a Vent (SV 0).
    const covered  = hasShelter(p.q, p.r) || sv >= 2 || !!p.dp;
    // Bite of 2, mirrors EXPOSURE_BITE in survival_state.hpp — safe only
    // because exposure can no longer take the last point (see below).
    const EXPOSURE_BITE = 2;
    if (!covered) { state.llDelta -= EXPOSURE_BITE; expDelta = -EXPOSURE_BITE; }

    // Shelter protection: resting under a shelter suppresses all LL loss this
    // dawn. Surface only — a shelter pitched on the hatch is not down the hole
    // with you, so it cannot cancel the bad-air roll below it.
    if (p.rt && !p.dp && hasShelter(p.q, p.r) && state.llDelta < 0) {
      state.llDelta = 0;
      expDelta = 0;
    }

    // Bad air — mirrors survival_state.hpp. Sleeping underground trades a
    // guaranteed 2 LL of exposure for a TUNNEL_REST_LL_PCT chance of 1, and
    // unlike exposure this one is NOT floored at LL 1: it rides through the
    // ordinary loss path below and can down you. See survival_state.hpp for
    // why the two are treated differently.
    let airDelta = 0;
    if (p.rt && p.dp && Math.random() * 100 < TUNNEL_REST_LL_PCT) {
      state.llDelta -= 1;
      airDelta = -1;
    }

    // Rest recovery: resting with food>=4 and water>=3 → +1 LL and a minor wound heals.
    // Settlements always qualify regardless of supply levels — mirrors survival_state.hpp.
    // Gate eased from F>=4 && W>=3 — mirrors survival_state.hpp; see there
    // for why the old gate turned close calls into deaths.
    // +2 while badly hurt so the wounded can climb out — a flat +1 exactly
    // cancels the dawn exposure tick and pins them at LL 1. See
    // survival_state.hpp for the measurement that forced this.
    // EXPOSURE_BITE + 1 while badly hurt, so rest beats the dawn tick by
    // exactly one and the wounded can climb out. Coupled deliberately —
    // see survival_state.hpp.
    // Ceiling is the survivor's own, not the bare LL_CAP: +LL gear raises it.
    const llCap = effectiveMaxLL(p);
    const restedWell = !!p.rt && ((p.food >= 2 && p.water >= 2) || inSettlement);
    if (restedWell && p.ll < llCap) state.llDelta += (p.ll <= 2) ? (EXPOSURE_BITE + 1) : 1;
    if (restedWell) healWound(p, 0);

    const prevLL = p.ll;
    // Exposure is applied last and floors at LL 1 — it never lands the
    // killing blow. Mirrors survival_state.hpp; see there for the reasoning
    // (it is the fix for deaths-vs-near-misses, not for the death rate).
    const expLoss    = -expDelta;
    const otherDelta = state.llDelta - expDelta;
    let   downed     = false;
    if (otherDelta < 0) {
      for (let i = 0; i < -otherDelta && p.ll > 0; i++) { p.ll--; if (p.ll === 0) downed = true; }
    } else if (otherDelta > 0) {
      p.ll = Math.min(p.ll + otherDelta, llCap);
    }
    if (!downed && expLoss > 0) {
      let landed = 0;
      for (let i = 0; i < expLoss && p.ll > 1; i++) { p.ll--; landed++; }
      expDelta = -landed;   // report what landed, not what was owed
    } else if (downed) {
      expDelta = 0;
    }
    const actualDelta = p.ll - prevLL;

    // Mirrors effectiveMP() in inventory_items.hpp. Slope halved from the
    // original `ll + 3`: full health is unchanged at 10, but a wounded
    // survivor keeps enough mobility to reach water instead of spiralling.
    // effectiveMP() folds in equipment mp and encumbrance; applyDawnItemCosts()
    // then charges the daily *_costs and adds the mp they unlock.
    p.mp = (p.ll === 0) ? 0 : effectiveMP(p);
    p.rt = 0;
    const unfuelled = applyDawnItemCosts(p);

    // Dawn upkeep can take the last point, and the mock never said so — the
    // firmware enqueues EVT_DOWNED from inside the loss loop, i.e. *before*
    // EVT_DAWN. Same order here so a client (or bots/causes.py, which matches
    // a death against the damage record nearest it) sees what hardware sends.
    if (downed) broadcast({ t: 'ev', k: 'downed', pid: id });
    broadcast({
      t: 'ev', k: 'dawn', pid: id, day: dayCount,
      f: p.food, w: p.water, ll: p.ll, mp: p.mp, dll: actualDelta,
      fth: p.fth, wth: p.wth, rad: p.rad, expd: expDelta, air: airDelta,
      wnd: p.wnd.slice(), unf: unfuelled,
    });
    console.log(`[dawn] day=${dayCount} pid=${id} f=${p.food} w=${p.water} ll=${p.ll} dll=${actualDelta}`);
  }
  broadcast(stateMsg());
}

// Real-time floor: a day normally takes DAY_TICKS (5 real minutes) but ends
// early the moment every connected player is resting, so back-to-back REST
// spam could otherwise collapse days to seconds and cycle weather absurdly
// fast. Mirrors the same ~1.1-1.9 real-minute floor as actions_game_loop.hpp's
// updateWeatherPhase() — not persisted, resets on server restart. Unlike the
// firmware (which also has a WEATHER_DUR_MIN/MAX days-per-phase counter),
// this floor is the mock's ONLY weather-pacing knob — it has no day-duration
// concept, so advanceWeather() re-rolls every dawn once this elapses.
let lastWeatherChangeAt = 0;
let weatherNextGapMs    = 67500;
function advanceWeather() {
  if (lastWeatherChangeAt && Date.now() - lastWeatherChangeAt < weatherNextGapMs) return;
  const prev = weatherPhase;
  const roll = Math.random() * 100;
  switch (weatherPhase) {
    case 0: weatherPhase = roll < 50 ? 1 : (roll < 70 ? 2 : (roll < 90 ? 5 : 4)); break;
    case 1: weatherPhase = roll < 40 ? 2 : (roll < 75 ? 0 : 5); break;
    case 2: weatherPhase = roll < 30 ? 3 : (roll < 55 ? 1 : (roll < 80 ? 0 : 4)); break;
    case 3: weatherPhase = roll < 20 ? 0 : (roll < 45 ? 2 : (roll < 70 ? 1 : 4)); break;
    case 4: weatherPhase = roll < 50 ? 0 : (roll < 75 ? 1 : 2); break;
    default: weatherPhase = roll < 60 ? 0 : (roll < 85 ? 1 : 4); break; // 5=mist ("Fog")
  }
  if (weatherPhase !== prev) {
    lastWeatherChangeAt = Date.now();
    weatherNextGapMs    = 67500 + Math.random() * 45000; // 1.1-1.9 real minutes until the next one
    broadcast({ t: 'ev', k: 'weather', phase: weatherPhase, ticks: 0 });
    console.log(`[weather] ${WEATHER_NAMES[prev]} -> ${WEATHER_NAMES[weatherPhase]}`);
  }
}

// ── Quakes ───────────────────────────────────────────────────────────────
// Occasional earthquake: ruptures a straight line of QUAKE_MIN_LEN..MAX_LEN
// hexes near a connected player, permanently destroys any shelter caught on
// it, and levels any Settlement on the line to Open Scrub. Server-
// authoritative — the fault line's location and its losses come from here so
// every client shakes the same hexes and sees the same losses, instead of
// each browser rolling its own random line.
const QUAKE_MIN_LEN = 7;
const QUAKE_MAX_LEN = 10;
// Mirrors actions_game_loop.hpp's tuning: was a 45s floor + 2%/tick roll
// (recurred roughly every 45-55 real seconds on average — too often for a
// dramatic one-off). Now a 3 min floor + 1%/tick roll averages out to
// roughly once every 3-3.5 real minutes.
const QUAKE_MIN_GAP_MS = 180000;
const QUAKE_TRIGGER_CHANCE = 0.01; // rolled once per game tick (100ms) once the gap has elapsed
const QUAKE_DIRS = [
  { dq: 1, dr: 0 }, { dq: 1, dr: -1 }, { dq: 0, dr: -1 },
  { dq: -1, dr: 0 }, { dq: -1, dr: 1 }, { dq: 0, dr: 1 },
];
const TERRAIN_SETTLEMENT = 9;
const TERRAIN_SCRUB      = 0;
const TERRAIN_HILLS      = 7;
const TERRAIN_MOUNTAIN   = 8;
let lastQuakeAt = 0;

function isMountainous(q, r) {
  const t = terrainAt(q, r);
  return t === TERRAIN_HILLS || t === TERRAIN_MOUNTAIN;
}

// Samples a few candidate start points around (refQ, refR) and prefers one
// that lands on Hills/Mountain — fault lines are drawn to real rough country
// instead of landing uniformly at random.
function pickQuakeOrigin(refQ, refR) {
  let best = null;
  for (let i = 0; i < 6; i++) {
    const q = ((Math.round(refQ + (Math.random() - 0.5) * 16) % MAP_COLS) + MAP_COLS) % MAP_COLS;
    const r = ((Math.round(refR + (Math.random() - 0.5) * 12) % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
    const mountainous = isMountainous(q, r);
    if (!best) best = { q, r };
    if (mountainous) return { q, r };
  }
  return best;
}

function triggerQuake(refQ, refR) {
  const len = QUAKE_MIN_LEN + Math.floor(Math.random() * (QUAKE_MAX_LEN - QUAKE_MIN_LEN + 1));
  const dir = QUAKE_DIRS[Math.floor(Math.random() * QUAKE_DIRS.length)];
  const origin = pickQuakeOrigin(refQ, refR);
  let q = origin.q;
  let r = origin.r;
  const cells = [];
  const destroyed = [];
  const converted = [];
  for (let i = 0; i < len; i++) {
    const cq = ((q % MAP_COLS) + MAP_COLS) % MAP_COLS;
    const cr = ((r % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
    cells.push({ q: cq, r: cr });
    if (hasShelter(cq, cr)) {
      destroyedShelters.add(`${cq}_${cr}`);
      destroyed.push({ q: cq, r: cr });
    }
    if (terrainAt(cq, cr) === TERRAIN_SETTLEMENT) {
      terrainOverrides.set(`${cq}_${cr}`, TERRAIN_SCRUB);
      converted.push({ q: cq, r: cr });
    }
    q += dir.dq; r += dir.dr;
  }
  lastQuakeAt = Date.now();
  broadcast({ t: 'ev', k: 'quake', cells, destroyed, converted });
  if (destroyed.length) broadcast(stateMsg());
  console.log(`[quake] len=${len} destroyed=${destroyed.length} converted=${converted.length} cells=${JSON.stringify(cells)}`);
}

function maybeTriggerQuake(connected) {
  if (Date.now() - lastQuakeAt < QUAKE_MIN_GAP_MS) return;
  // Standing near Hills/Mountain makes a quake several times more likely —
  // real fault country, not just a uniform roll anywhere on the map.
  const nearMountain = connected.find((p) => isMountainous(p.q, p.r));
  const chance = nearMountain ? QUAKE_TRIGGER_CHANCE * 3 : QUAKE_TRIGGER_CHANCE;
  if (Math.random() > chance) return;
  const ref = nearMountain || connected[Math.floor(Math.random() * connected.length)];
  triggerQuake(ref.q, ref.r);
}

// ── Strangle Fog per-tick hazard ────────────────────────────────────────
// Mirrors actions_game_loop.hpp's WEATHER_FOG block closely enough to feel
// out the rates here before flashing: MP bleeds away steadily while standing
// in fog, LL loss is far rarer. Same terrain shape as the firmware's
// WEATHER_INTENSITY[WEATHER_FOG] row (worst in dense/wet terrain, weakest on
// high dry ground); Settlement/Broken Urban are immune. Unlike the firmware,
// this mock's map data doesn't distinguish basic vs improved shelter (just
// shelter-or-not), so any shelter here only halves the rate rather than
// granting the firmware's full immunity at the improved tier.
const FOG_INTENSITY     = [0.45, 0.35, 0.7, 0.75, 0.25, 0.65, 0.5, 0.3, 0.2, 0.1, 0, 0];
const FOG_MP_TICK_RATE  = 1 / 300;
const FOG_LL_TICK_RATE  = 1 / 1800;
function fogTick(connected) {
  if (weatherPhase !== 4) return;
  let changed = false;
  for (const p of connected) {
    if (p.ll === 0) continue;
    const t = terrainAt(p.q, p.r);
    if (t === 9 || t === 4) continue; // Settlement, Broken Urban — immune
    let mpProb = (FOG_INTENSITY[t] ?? 0) * FOG_MP_TICK_RATE;
    let llProb = (FOG_INTENSITY[t] ?? 0) * FOG_LL_TICK_RATE;
    if (hasShelter(p.q, p.r)) { mpProb *= 0.5; llProb *= 0.5; }
    if (p.mp > 0 && Math.random() < mpProb) { p.mp--; changed = true; }
    if (Math.random() < llProb) { p.ll = Math.max(0, p.ll - 1); changed = true; }
  }
  if (changed) broadcast(stateMsg());
}

// Day tick — mirrors tickGame(): normal timeout OR (if anyone's connected) every
// connected player resting triggers dawn immediately, same as the firmware,
// which is what makes solo REST end the day right away.
setInterval(() => {
  const connected = Object.values(players);
  if (connected.length === 0) return; // nobody connected — don't burn days
  dayTick++;
  const allResting = connected.every((p) => p.rt);
  if (dayTick >= DAY_TICKS || allResting) {
    dayTick = 0;
    dawnUpkeepAll();
  }
  fogTick(connected);
  maybeTriggerQuake(connected);
  worldTickCounter++;
  if (worldTickCounter % WORLD_TICK_INTERVAL === 0) {
    // Order matches world-system.hpp: Doom moves first (before anything else
    // reads/decays state this tick), fire damage resolves against THIS
    // tick's fresh intensity before spreadFire() decays it, and the caravan
    // prompt runs after the caravan has actually moved.
    tickCreepingDoom(connected);
    maybeIgniteLightning(connected);
    resolveFireDamage(connected);
    resolveDoomProximity(connected);
    tickDoomTaunts(connected);   // after the act event, so the voice follows the damage
    tickTunnelTaunts(connected); // the other voice: second thoughts, for anyone camped in a bare corridor
    spreadFire();
    maybeTriggerFlashFlood(connected);
    spreadFlood(connected);
    tickCaravan(connected);
    resolveCaravanProximity(connected);
    // Firmware's broadcastState() runs unconditionally every game tick (see
    // game-server.hpp), so caravan movement is never stale there. This mock
    // only broadcasts opportunistically (from message handlers) otherwise,
    // so without this, a client wouldn't see the caravan move until some
    // other action happened to trigger a broadcast.
    broadcast(stateMsg());
  }
}, TICK_MS);

// Lightweight heartbeat, independent of the game-tick/world-tick timing above:
// firmware's broadcastState() is unconditional every 100ms, so an idle
// in-game client there always sees frequent traffic. This mock's own
// broadcasts are opportunistic (player actions) plus the 15s world tick,
// which is fine for game-state correctness but too sparse for the client's
// staleness watchdog (network.js, WS_STALE_THRESHOLD_MS=3000) — an idle
// player in the mock would get force-reconnected every few seconds without
// this. Just re-sends current state; doesn't touch simulation timing.
setInterval(() => {
  if (Object.keys(players).length === 0) return;
  broadcast(stateMsg());
}, 2000);

function encStart(ws, id, msg) {
  const p = players[id];
  if (!p) { send(ws, { t: 'enc_dbg', msg: 'no_slot' }); return; }
  const q = msg.q | 0, r = msg.r | 0;
  if (encounters[id])            { send(ws, { t: 'err', msg: 'Already in an encounter — abort first' }); return; }
  if (p.ll === 0)                { send(ws, { t: 'err', msg: 'Cannot enter — you are downed' }); return; }
  if (p.q !== q || p.r !== r)    { send(ws, { t: 'err', msg: 'Not at that hex' }); return; }
  if (!hasPoi(q, r))             { send(ws, { t: 'err', msg: 'Already looted' }); return; }
  const pool = ENC_INDEX[String(terrainAt(q, r))];
  if (!pool || !pool.count)      { send(ws, { t: 'err', msg: 'No encounters here' }); return; }
  const encId = 1 + Math.floor(Math.random() * pool.count);
  openEncounter(ws, id, p, q, r, pool.path, encId, true);
}

// Ends an encounter without banking — mirrors endEncounter() in
// encounter_engine.hpp. reason is one of the ENC_REASON_LABELS keys the
// client already knows (hazard/abort/dawn/downed/disconnect/regen).
// dawn/disconnect restore the POI (not the player's choice to leave);
// the rest consume it for good, same as the firmware's restorePoi flag.
function encEnd(id, reason) {
  const e = encounters[id];
  if (!e) return;
  if (reason === 'dawn' || reason === 'disconnect') consumedPoi.delete(`${e.q}_${e.r}`);
  delete encounters[id];
  const p = players[id];
  if (p) p.enc = false;
  broadcast({ t: 'ev', k: 'enc_end', pid: id, q: e.q, r: e.r, reason });
  console.log(`[enc] end pid=${id} reason=${reason}`);
}

function encChoice(ws, id, m) {
  const p = players[id], e = encounters[id];
  if (!p || !e || p.ll === 0) return;
  // {"t":"enc_choice","ci":N} — everything else comes from the JSON file.
  const ch = resolveChoice(e.json, e.nodeKey, m.ci | 0);
  if (!ch) { send(ws, { t: 'err', msg: 'That choice is not open to you' }); return; }
  const c = ch.cost;
  const canAfford = p.ll >= c.ll && p.rad + c.rad <= 10 &&
                    p.inv[1] >= c.food && p.inv[0] >= c.water && p.inv[4] >= c.scrap &&
                    p.inv[3] >= c.med;
  if (!canAfford) { send(ws, { t: 'err', msg: 'Cannot afford cost' }); return; }
  p.ll     = Math.max(0, Math.min(7, p.ll - c.ll));
  p.rad    = Math.max(0, Math.min(10, p.rad + c.rad));
  p.inv[1] = Math.max(0, p.inv[1] - c.food);
  p.inv[0] = Math.max(0, p.inv[0] - c.water);
  p.inv[4] = Math.max(0, p.inv[4] - c.scrap);
  p.inv[3] = Math.max(0, p.inv[3] - c.med);

  const skill = ch.skill;
  const dn    = computeDN(p, ch.baseRisk);
  // Mirrors checkSkillMod(): major wounds hit every skill, minor wounds hit
  // Endure only, and the Endurer (archetype 5) reads Endure 1 higher.
  let mod = -(p.wnd?.[1] ?? 0);
  if (skill === 4) { mod -= (p.wnd?.[0] ?? 0); if (p.arch === 5) mod += 1; }
  let   tot   = d6() + d6() + (p.sk[skill] || 0) + mod;
  if (forcedOutcome !== null) { tot = forcedOutcome ? dn : dn - 1; forcedOutcome = null; }
  const ok    = tot >= dn;
  const ev    = { t: 'ev', k: 'enc_res', pid: id, out: ok ? 1 : 0, skill, dn, tot,
                  loot: [0, 0, 0, 0, 0], it: 0, iq: 0, it2: 0, iq2: 0, penLL: 0, penRad: 0,
                  penRes: [0, 0, 0, 0, 0], penWnd: [0, 0], ends: 0,
                  drains: [0, 0, 0, 0, 0, 0], rec: 0 };
  let ended = false;
  if (ok) {
    for (let i = 0; i < 5; i++) {
      e.pendingLoot[i] = Math.min(99, e.pendingLoot[i] + ch.loot[i]);
      ev.loot[i] = ch.loot[i];
    }
    const items = [...ch.items];
    const rolled = ch.lootTable ? rollLootTable(ch.lootTable) : null;
    if (rolled) items.push(rolled);
    // The firmware caps ENC_MAX_ITEMS over the whole scene, not per choice
    // (network-msg-encounter.hpp) — cap the same way, or the mock banks items
    // a board never would and the haul tray disagrees with the pack.
    items.slice(0, Math.max(0, 3 - e.pendingItems.length)).forEach((it, k) => {
      e.pendingItems.push(it);
      if (k === 0)      { ev.it  = it.it; ev.iq  = it.iq; }
      else if (k === 1) { ev.it2 = it.it; ev.iq2 = it.iq; }
    });
    // A recipe is a one-time knowledge grant, pending like the loot above
    // until the player banks — see encBank below. OR'd into a bitmask (not
    // overwritten): a scene can walk through several nodes, each granting a
    // different recipe, before ever banking.
    // Bounded like the firmware (1 << 32+ silently wraps in JS): a hand-authored
    // id past MAX_RECIPES must not light up some other recipe's bit.
    if (ch.recipeId && ch.recipeId <= 32) { e.pendingRecipes |= (1 << (ch.recipeId - 1)); ev.rec = ch.recipeId; }
    e.nodeKey = ch.nextKey;
    e.canBank = ch.nextCanBank;
    if (ch.nextTerminal) e.fullClear = true;
    if (p.ll === 0) ended = true;
  } else {
    ev.penLL = ch.hazLL; ev.penRad = ch.hazRad; ev.ends = ch.hazEnds ? 1 : 0;
    if (ch.hazLL > 0)      p.ll = Math.min(7, p.ll + ch.hazLL);
    else if (ch.hazLL < 0) p.ll = Math.max(0, p.ll + ch.hazLL);
    p.rad = Math.max(0, Math.min(10, p.rad + ch.hazRad));
    for (let i = 0; i < 5; i++) {
      const take = Math.min(ch.hazRes[i], p.inv[i]);
      p.inv[i] -= take;
      ev.penRes[i] = take;
    }
    // Hazard wounds ("wound": [minor, major] in the encounter JSON)
    [ch.hazWMin, ch.hazWMaj].forEach((want, tier) => {
      const before = p.wnd[tier];
      p.wnd[tier] = Math.min(WOUND_MAX_EACH, before + want);
      ev.penWnd[tier] = p.wnd[tier] - before;
    });
    if (ev.penWnd[1]) p.mp = Math.max(0, p.mp - ev.penWnd[1]);
    ended = p.ll === 0 || ch.hazEnds;
  }
  console.log(`[enc] choice pid=${id} skill=${skill} dn=${dn} tot=${tot} ok=${ok} ends=${ended}`);
  broadcast(ev);
  if (ended) {
    if (p.ll === 0) broadcast({ t: 'ev', k: 'downed', pid: id });
    encEnd(id, p.ll === 0 ? 'downed' : 'hazard');
  }
  broadcast(stateMsg());
}

function encBank(ws, id, m) {
  const p = players[id], e = encounters[id];
  if (!p || !e) return;
  if (!e.fullClear && !e.canBank) { send(ws, { t: 'err', msg: "You can't carry loot out from here" }); return; }
  // Optional haul trim from the tray steppers. Absent "keep" means "bank
  // everything" (older clients), and every entry is clamped against our own
  // pendingLoot below — mirrors handleMsg_enc_bank in network-msg-encounter.hpp.
  const keep = (Array.isArray(m?.keep) ? m.keep : [99, 99, 99, 99, 99])
    .map(v => Math.max(0, Math.min(99, v | 0)));
  let total = 0;
  for (let i = 0; i < 5; i++) {
    // Write the taken amount back into pendingLoot so the enc_bank broadcast
    // below reports what was actually banked, not what was rolled — otherwise
    // the client's _evEncBank adds the untrimmed amount to its inv[].
    const take = Math.min(e.pendingLoot[i], keep[i] ?? 99);
    p.inv[i] = Math.min(99, p.inv[i] + take);
    e.pendingLoot[i] = take;
    total += take;
  }
  const hadItems = e.pendingItems.length > 0;
  for (const { it, iq } of e.pendingItems) grantItemOrDrop(p, it, iq);
  p.kr = (p.kr | 0) | (e.pendingRecipes | 0);
  const scoreD = total * 3 + (e.fullClear ? 10 : 0);
  p.sc += scoreD;
  broadcast({ t: 'ev', k: 'enc_bank', pid: id, q: e.q, r: e.r, loot: e.pendingLoot, scoreD, recs: e.pendingRecipes || 0 });
  // grantItemOrDrop() just mutated it[]/iq[] — like every other item action,
  // that needs its own targeted ack, since the generic 'ev' broadcast above
  // never carries pack contents (mirrors handleMsg_enc_bank's item_result in
  // network-msg-encounter.hpp).
  if (hadItems) send(ws, { t: 'item_result', ok: true, act: 'enc_bank', pid: id, it: p.it, iq: p.iq, ...packFields(p) });
  delete encounters[id];
  p.enc = false;
  broadcast(stateMsg());
  console.log(`[enc] bank pid=${id} loot=${JSON.stringify(e.pendingLoot)} +${scoreD}`);
}

function encAbort(id) {
  if (!encounters[id]) return;
  if (threatClock < 20) threatClock++;
  encEnd(id, 'abort');
  broadcast(stateMsg());
}

// GET /enc?biome=X&id=Y → data/encounters/X/Y.json (same validation as firmware)
function handleEnc(req, res) {
  const u = new URL(req.url, 'http://x');
  const biome = u.searchParams.get('biome') || '', id = u.searchParams.get('id') || '';
  if (!/^[A-Za-z0-9_]+$/.test(biome) || !/^\d+$/.test(id)) {
    res.writeHead(400, { 'Content-Type': 'text/plain' }); res.end('Invalid biome or id'); return;
  }
  const filePath = path.join(DATA_DIR, 'encounters', biome, `${id}.json`);
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); res.end('Encounter not found'); return; }
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(data);
  });
}

const players = {};       // id -> player
const sockets = new Map(); // ws -> id

// Known-WiFi roaming list (firmware: wifi-store.hpp, NVS "wifinets").
// Seeded with two entries so the settings panel has something to show offline.
let knownNets  = ['WASTELAND-HOME', 'friends-house-5G'];
let currentSsid = 'WASTELAND-HOME';
const wifiNetsMsg = () => ({ t: 'wifi', status: 'nets', cur: currentSsid, nets: knownNets });
// Link mode (firmware: handleConnect in network-session.hpp). The real board
// decides this by comparing the client IP against softAPIP(); offline there is
// no AP, so drive it from the environment:
//   --ap  (or MOCK_AP=1)          -> direct-uplink warning, board off-network
//   --ap --sta-ip=192.168.1.42    -> warning plus "the tower answers at ..."
const MOCK_AP     = process.env.MOCK_AP === '1' || process.argv.includes('--ap');
const MOCK_AP_CAP = Number(process.env.MOCK_AP_CAP || 4);
const MOCK_STA_IP = process.env.MOCK_STA_IP
  || (process.argv.find(a => a.startsWith('--sta-ip=')) || '').slice(9);
const wifiLinkMsg = () => ({
  t: 'wifi', status: 'link',
  ap: MOCK_AP ? 1 : 0, cap: MOCK_AP_CAP, ip: MOCK_AP ? MOCK_STA_IP : '192.168.1.42',
});

// ── Direction deltas (approximate — "mostly works") ─────────────────────────
// Buttons: 0=SE 1=NE 2=N 3=NW 4=SW 5=S
// Which inv[] slot each action fills (ACT_* -> RES_*). FORAGE pays food,
// WATER pays water, SCAV pays scrap; the rest pay nothing here.
const ACT_RESOURCE = { 0: 1, 1: 0, 3: 4 };
// handleAction() refuses these three at depth 1 (actions_game_loop.hpp).
// REST is deliberately NOT here: it is legal underground and pays the
// TUNNEL_REST_LL_PCT bad-air roll in dawnUpkeep instead.
const ACTS_REFUSED_UNDERGROUND = new Set([4, 5, 6]);

const DIR_DELTA = {
  0: [+1, +1],  // SE
  1: [+1, -1],  // NE
  2: [ 0, -1],  // N
  3: [-1, -1],  // NW
  4: [-1, +1],  // SW
  5: [ 0, +1],  // S
};

// Tunnel generation uses DIR_DELTA above, so it cannot run at module top --
// const is in the temporal dead zone until this point.
buildTunnels();

const send = (ws, obj) => {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
};

const broadcast = (obj) => {
  for (const ws of sockets.keys()) send(ws, obj);
};

// Unicast by player id. The firmware does this through ev.evWsId +
// ws.client(id) (network-events.hpp); here the ws -> id map is small enough
// to scan. Used by the tunnel taunts, which are nobody else's business.
const sendToPid = (id, obj) => {
  for (const [ws, pid] of sockets.entries()) if (pid === id) send(ws, obj);
};

// ── Trade offers — mirrors network-msg-trade.hpp closely enough for UI work:
// one outstanding offer per sender, same-hex + resource checks, 30s expiry.
const tradeOffers = {}; // fromId -> { active, fromId, toId, give, want, expiresAt }
const TRADE_EXPIRE_MS = 30000;

function sameHex(a, b) {
  return !!a && !!b && a.on && b.on && a.q === b.q && a.r === b.r;
}
function hasResources(p, qty) {
  return !!p && qty.every((n, i) => (p.inv[i] || 0) >= n);
}

// ── World system: Caravan — simplified, not behaviour-parity (see
// docs/world-system-spec.md and dev-loop.md's mock-server section). No
// terrain/passability checks, matching this mock's player movement (case
// 'm' below doesn't check terrain either) — this exists so front-end work
// (caravan marker, trade panel) can be dev-looped without hardware.
const CARAVAN_PID = 254;
const CARAVAN_RESTOCK_TICKS = 40;   // world ticks — matches firmware
const CARAVAN_STOCK_SLOTS   = 4;    // consumable shelf slots — matches the .ino
const CARAVAN_STOCK_MAX     = 3;    // max units per shelf slot — matches world-system.hpp
const CARAVAN_TRADE_HOLD    = 4;    // world ticks the caravan waits on a co-located trader — matches world-system.hpp
const WORLD_TICK_INTERVAL   = 150;  // game ticks between world updates (~15s) — matches firmware
let worldTickCounter = 0;
const caravan = {
  q: Math.floor(Math.random() * MAP_COLS),
  r: Math.floor(Math.random() * MAP_ROWS),
  wq: 0, wr: 0,
  pq: -1, pr: -1,  // settlement just departed; -1,-1 = none yet — mirrors Caravan.pq/pr in world-system.hpp
  active: true,
  inv: [0, 0, 0, 0, 0],
  restockTimer: 0,
  // Consecutive world ticks spent waiting on a survivor standing here — mirrors
  // Caravan.holdTicks in world-system.hpp (runtime only, never persisted).
  holdTicks: 0,
  // Consumable shelf — mirrors Caravan.stockItem[]/stockQty[] in world-system.hpp
  // (id 0 = empty). Price isn't stored: caravanPrice() reads items.cfg's value.
  stock: Array.from({ length: CARAVAN_STOCK_SLOTS }, () => ({ id: 0, qty: 0 })),
};

// ── Caravan shelf — mirrors caravanPrice()/caravanCanStock()/
// rollCaravanStockItem()/caravanStockSlot() in world-system.hpp.
function caravanPrice(itemId) { return ITEM_DEFS[itemId]?.value || 1; }
// Token worth when paying the caravan — water 0 (refused), fuel 2, rest 1.
// Mirrors CARAVAN_TOKEN_WORTH in world-system.hpp / game-data.js.
const CARAVAN_TOKEN_WORTH = [0, 1, 2, 1, 1];
function caravanPaymentValue(give) { return give.reduce((a, n, i) => a + n * CARAVAN_TOKEN_WORTH[i], 0); }
function caravanCanStock(def) { return !!def && def.category === 'consumable' && !!def.trade; }
function rollCaravanStockItem() {
  const onShelf = new Set(caravan.stock.map(s => s.id));
  const pool = Object.values(ITEM_DEFS).filter(d => caravanCanStock(d) && !onShelf.has(d.id));
  return pool.length ? pool[Math.floor(Math.random() * pool.length)].id : 0;
}
function caravanStockSlot(itemId) {
  return itemId ? caravan.stock.findIndex(s => s.id === itemId && s.qty > 0) : -1;
}
const lastCaravanHex = {}; // pid -> "q_r" last prompted (edge-triggers car_avail)

function hexDistWrap(q1, r1, q2, r2) {
  let best = Infinity;
  for (let dq = -1; dq <= 1; dq++) {
    for (let dr = -1; dr <= 1; dr++) {
      const aq = q2 + dq * MAP_COLS - q1;
      const ar = r2 + dr * MAP_ROWS - r1;
      const dist = (Math.abs(aq) + Math.abs(aq + ar) + Math.abs(ar)) / 2;
      if (dist < best) best = dist;
    }
  }
  return best;
}

// ── World system: Fire + Tracks — simplified, not behaviour-parity (see
// docs/world-system-spec.md and world-system.hpp, which this mirrors).
// Tracks/scent aren't modeled here — nothing reads them yet (Creeping Doom
// isn't built anywhere). Fire is: lightning ignition during a storm, spread,
// decay, rain dousing everything outright, and the permanent burn-to-Ash-
// Dunes conversion via the same terrainOverrides map quakes already use.
const FIRE_SPREAD_CHANCE      = 20;  // % chance a burning hex (intensity >=2) ignites each flammable neighbour per world tick
const FIRE_CAP                = 20;  // max simultaneously burning hexes
const LIGHTNING_IGNITE_CHANCE = 17;  // % per world tick while a storm (phase 2) is active (was 15: +15% relative)
const TERRAIN_ASH_DUNES       = 1;
const fireGrid = {};  // "q_r" -> intensity (1-3)
let   fireCount = 0;

function isFlammable(t) {
  // Open Scrub(0), Rust Forest(2), Marsh(3), Broken Urban(4), Rolling Hills(7),
  // Settlement(9). Mountain(8) and Glass Fields(6) are explicitly immune.
  return t === 0 || t === 2 || t === 3 || t === 4 || t === 7 || t === 9;
}

function igniteHex(q, r, intensity) {
  if (!isFlammable(terrainAt(q, r))) return;
  const key = `${q}_${r}`;
  const wasUnlit = !fireGrid[key];
  if (wasUnlit) {
    if (fireCount >= FIRE_CAP) return;
    fireCount++;
  }
  fireGrid[key] = Math.max(fireGrid[key] || 0, intensity);
  if (wasUnlit) broadcast({ t: 'ev', k: 'fire_spread', q, r, intensity: fireGrid[key] });
}

// Permanent, one-way: Ash Dunes isn't flammable, so a hex only ever burns
// once. intensity:0 always means "just went out" (see network.js's
// _evFireSpread) since a hex is never re-broadcast at 0 any other time.
function extinguishHex(q, r) {
  delete fireGrid[`${q}_${r}`];
  fireCount--;
  terrainOverrides.set(`${q}_${r}`, TERRAIN_ASH_DUNES);
  broadcast({ t: 'ev', k: 'fire_spread', q, r, intensity: 0 });
}

// Lightning during a storm — the only ignition source until Creeping Doom
// exists. Struck hex is near a random connected player (same bias
// maybeTriggerQuake() uses) so it's actually observable.
function maybeIgniteLightning(connected) {
  if (weatherPhase !== 2) return;  // 2 = STORM
  if (Math.random() * 100 >= LIGHTNING_IGNITE_CHANCE) return;
  if (connected.length === 0) return;
  const ref = connected[Math.floor(Math.random() * connected.length)];
  // Explicit direct-hit chance, then a jitter box for the near misses —
  // mirrors maybeIgniteLightning() in world-system.hpp.
  const LIGHTNING_DIRECT_CHANCE = 30;
  const LIGHTNING_JITTER = 2, LIGHTNING_SPAN = LIGHTNING_JITTER * 2 + 1;
  const jit = () => Math.floor(Math.random() * LIGHTNING_SPAN) - LIGHTNING_JITTER;
  const direct = Math.random() * 100 < LIGHTNING_DIRECT_CHANCE;
  const q = direct ? ref.q : ((ref.q + jit()) % MAP_COLS + MAP_COLS) % MAP_COLS;
  const r = direct ? ref.r : ((ref.r + jit()) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
  igniteHex(q, r, 2);  // strikes in hot, not just an ember; no-ops on immune terrain

  // Direct strike — anyone standing exactly on the struck hex takes damage
  // regardless of whether the terrain was flammable.
  for (const p of connected) {
    if (p.q !== q || p.r !== r) continue;
    if (p.ll === 0) continue;   // already down — mirrors the guard in world-system.hpp
    p.ll = Math.max(0, p.ll - 2);
    broadcast({ t: 'ev', k: 'fire_dmg', pid: p.id, q, r, intensity: 10 }); // 10 = direct strike sentinel, outside fire's 1-3 range
    if (p.ll === 0) broadcast({ t: 'ev', k: 'downed', pid: p.id });
  }
}

// Snapshot-iterates the current fire keys so a hex ignited by spread this
// pass doesn't itself spread again in the same pass — same non-cascading
// intent as world-system.hpp's double-buffer, just via a different
// mechanism. Plain rain (not the lightning-bearing storm phase) douses
// everything outright.
function spreadFire() {
  if (weatherPhase === 1) {  // 1 = RAIN
    for (const key of Object.keys(fireGrid)) {
      const [q, r] = key.split('_').map(Number);
      extinguishHex(q, r);
    }
    return;
  }

  for (const key of Object.keys(fireGrid)) {
    const [q, r] = key.split('_').map(Number);
    const intensity = fireGrid[key];
    if (intensity - 1 <= 0) { extinguishHex(q, r); continue; }
    fireGrid[key] = intensity - 1;

    if (intensity >= 2) {
      for (const dir of Object.keys(DIR_DELTA)) {
        const [dq, dr] = DIR_DELTA[dir];
        const nq = ((q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
        const nr = ((r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
        if (fireGrid[`${nq}_${nr}`]) continue;               // already burning
        if (!isFlammable(terrainAt(nq, nr))) continue;        // immune terrain
        if (Math.random() * 100 >= FIRE_SPREAD_CHANCE) continue;
        igniteHex(nq, nr, 1);
      }
    }
  }
}

// Level-triggered (every tick standing in it hurts) — mirrors
// resolveProximity()'s fire-damage half in world-system.hpp.
function resolveFireDamage(connected) {
  for (const p of connected) {
    const intensity = fireGrid[`${p.q}_${p.r}`] || 0;
    if (intensity < 2 || p.ll === 0) continue;
    // Scales with intensity, mirroring fireDamageFor() in world-system.hpp
    p.ll = Math.max(0, p.ll - (intensity >= 3 ? 2 : 1));
    if (intensity === 3) p.rad = Math.min(255, p.rad + 1);
    broadcast({ t: 'ev', k: 'fire_dmg', pid: p.id, q: p.q, r: p.r, intensity });
    if (p.ll === 0) broadcast({ t: 'ev', k: 'downed', pid: p.id });
  }
}

function fireArray() {
  return Object.entries(fireGrid).map(([key, intensity]) => {
    const [q, r] = key.split('_').map(Number);
    return [q, r, intensity];
  });
}

// ── World system: Flash Flood — the other storm-gated hazard alongside
// lightning. Mirrors world-system.hpp's maybeTriggerFlashFlood()/
// spreadFlood(): instead of igniting flammable terrain, it needs to find
// actual water terrain (River Channel(11)/Flooded District(5)) to start
// from, then can permanently wash out an adjacent dry hex into Flooded
// District. Unlike fire, a flooded hex never "burns out" — it recedes
// (decays) once the storm passes, and the permanent conversion only ever
// happens to the washed-out neighbour, never the source.
const FLASH_FLOOD_CHANCE      = 12;  // % per world tick while a storm (phase 2) is active
const FLOOD_SPREAD_CHANCE     = 20;  // % chance a flooded hex (intensity >=2) washes out each eligible neighbour per world tick
const FLOOD_CAP               = 15;  // max simultaneously flooded hexes
const FLOOD_MAX_INTENSITY     = 3;
const FLOOD_LL_DAMAGE         = 1;   // mirrors world-system.hpp: being swept costs 1 LL on top of the wiped turn
const FLOOD_SEARCH_RADIUS     = 4;   // jitter box (± radius) searched for a water hex near the reference player
const TERRAIN_MARSH             = 3;
const TERRAIN_FLOODED_DISTRICT  = 5;
const floodGrid = {};  // "q_r" -> intensity (1-3)
let   floodCount = 0;

function hasWater(t) {
  return t === 5 || t === 11;  // Flooded District, River Channel
}

// Washout-eligible: Open Scrub(0), Rolling Hills(7) only — mirrors
// world-system.hpp's isWashoutEligible().
function isWashoutEligible(t) {
  return t === 0 || t === 7;
}

function maybeTriggerFlashFlood(connected) {
  if (weatherPhase !== 2) return;  // 2 = STORM
  if (Math.random() * 100 >= FLASH_FLOOD_CHANCE) return;
  if (connected.length === 0) return;
  const ref = connected[Math.floor(Math.random() * connected.length)];
  const span = FLOOD_SEARCH_RADIUS * 2 + 1;
  for (let i = 0; i < span * span; i++) {
    const dq = Math.floor(Math.random() * span) - FLOOD_SEARCH_RADIUS;
    const dr = Math.floor(Math.random() * span) - FLOOD_SEARCH_RADIUS;
    const q = ((ref.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
    const r = ((ref.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
    if (!hasWater(terrainAt(q, r))) continue;
    const key = `${q}_${r}`;
    const wasDry = !floodGrid[key];
    if (wasDry) {
      if (floodCount >= FLOOD_CAP) return;
      floodCount++;
    }
    floodGrid[key] = Math.max(floodGrid[key] || 0, 2);
    return;
  }
}

// Snapshot-iterates current flood keys, same non-cascading intent as
// spreadFire(). While the storm continues, every flooded hex rises in
// intensity and (once >=2) can wash out an eligible dry neighbour —
// permanent, one-way, and the newly-flooded neighbour keeps carrying flood
// intensity afterward instead of self-extinguishing. Once the storm passes,
// floods simply recede (decay by 1/tick, no spread) — there's no single-tick
// "doused outright" moment the way rain douses fire.
function spreadFlood(connected) {
  if (weatherPhase !== 2) {
    for (const key of Object.keys(floodGrid)) {
      const next = floodGrid[key] - 1;
      if (next <= 0) { delete floodGrid[key]; floodCount--; }
      else floodGrid[key] = next;
    }
    return;
  }

  for (const key of Object.keys(floodGrid)) {
    const [q, r] = key.split('_').map(Number);
    const intensity = floodGrid[key];
    if (intensity < FLOOD_MAX_INTENSITY) {
      floodGrid[key] = intensity + 1;
      // Second stage: a swamped edge hex that's stayed underwater long
      // enough (just reached max intensity) fully drowns into Flooded
      // District — mirrors world-system.hpp's spreadFlood().
      if (floodGrid[key] === FLOOD_MAX_INTENSITY && terrainAt(q, r) === TERRAIN_MARSH) {
        terrainOverrides.set(key, TERRAIN_FLOODED_DISTRICT);
        broadcast({ t: 'ev', k: 'flood_washout', q, r, intensity: TERRAIN_FLOODED_DISTRICT });
      }
    }

    if (intensity >= 2) {
      for (const dir of Object.keys(DIR_DELTA)) {
        const [dq, dr] = DIR_DELTA[dir];
        const nq = ((q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
        const nr = ((r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
        const nkey = `${nq}_${nr}`;
        if (floodGrid[nkey]) continue;                        // already flooded this pass
        if (!isWashoutEligible(terrainAt(nq, nr))) continue;   // not washout-eligible
        if (Math.random() * 100 >= FLOOD_SPREAD_CHANCE) continue;
        if (floodCount >= FLOOD_CAP) continue;

        // First stage: dry ground pushed by the advancing flood front
        // becomes Marsh — a swampy edge, not open water outright.
        terrainOverrides.set(nkey, TERRAIN_MARSH);
        floodGrid[nkey] = 1;
        floodCount++;
        broadcast({ t: 'ev', k: 'flood_washout', q: nq, r: nr, intensity: TERRAIN_MARSH });

        // Anyone standing on the hex the instant it washes out is swept —
        // same "hit the player who happened to be there" idiom as
        // maybeIgniteLightning()'s direct strike.
        for (const p of connected) {
          if (p.q !== nq || p.r !== nr) continue;
          if (p.ll === 0) continue;   // already down, slot reset pending
          p.ll = Math.max(0, p.ll - FLOOD_LL_DAMAGE);
          p.mp = 0;
          // 10 = swept-away sentinel, outside flood's 1-3 range; llLost is what it actually cost
          broadcast({ t: 'ev', k: 'flood_dmg', pid: p.id, q: nq, r: nr, intensity: 10, llLost: FLOOD_LL_DAMAGE });
          if (p.ll === 0) broadcast({ t: 'ev', k: 'downed', pid: p.id });
        }
      }
    }
  }
}

function floodArray() {
  return Object.entries(floodGrid).map(([key, intensity]) => {
    const [q, r] = key.split('_').map(Number);
    return [q, r, intensity];
  });
}

// ── World system: Creeping Doom — simplified, not behaviour-parity: no
// track/scent modeling here (nothing needs the AI to be faithful, just the
// client-visible position/awareness/effects to exercise the aura render and
// doom_warn/doom_act events). Homes in on the nearest connected player once
// within detection radius instead of chasing scent; otherwise wanders and
// cools off. Same awareness-tier effects as world-system.hpp's
// resolveDoomProximity().
const AWARENESS_GAIN   = 12;
const AWARENESS_DECAY  = 5;
const DOOM_BASE_RADIUS = 6;  // matches world-system.hpp
const DOOM_TAUNT_COOLDOWN = 8;  // world ticks between repeat taunts — matches world-system.hpp
const DOOM_TAUNT_MIN_GAP  = 2;  // floor between any two taunts, incl. tier changes
const doom = {
  q: Math.floor(Math.random() * MAP_COLS),
  r: Math.floor(Math.random() * MAP_ROWS),
  awareness: 0,
  lastTauntTier: 0,
  tauntCooldown: 0,
};

function doomDetectionRadius() {
  return DOOM_BASE_RADIUS + Math.floor(doom.awareness / 25);
}

function nearestConnectedPlayer(connected) {
  let best = null, bestDist = Infinity;
  for (const p of connected) {
    if (p.enc) continue;
    const d = hexDistWrap(doom.q, doom.r, p.q, p.r);
    if (d < bestDist) { bestDist = d; best = p; }
  }
  return best ? { p: best, dist: bestDist } : null;
}

// stopAdjacent: true for the awareness-100 lock-on ("adjacent, not on top of
// the player" — same distance-1 stopping rule as world-system.hpp's
// stepAdjacentTo()); false for the ordinary scent-chase close-in.
function stepDoomToward(tq, tr, stopAdjacent) {
  const curDist = hexDistWrap(doom.q, doom.r, tq, tr);
  if (stopAdjacent && curDist <= 1) return;
  let bestDist = curDist, bestDq = 0, bestDr = 0, moved = false;
  for (const dir of Object.keys(DIR_DELTA)) {
    const [dq, dr] = DIR_DELTA[dir];
    const nq = ((doom.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
    const nr = ((doom.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
    const dist = hexDistWrap(nq, nr, tq, tr);
    if (dist < bestDist) { bestDist = dist; bestDq = dq; bestDr = dr; moved = true; }
  }
  if (!moved) return;
  doom.q = ((doom.q + bestDq) % MAP_COLS + MAP_COLS) % MAP_COLS;
  doom.r = ((doom.r + bestDr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
}

// Mirrors doomStepsPerTick() in world-system.hpp — hexes covered per world
// tick, keyed off the same 76/100 thresholds as every other behaviour tier.
function doomStepsPerTick() {
  if (doom.awareness >= 100) return 3;
  if (doom.awareness >=  76) return 2;
  return 1;
}

// `inRange` stands in for the firmware's `hasScent`: this mock has no track
// grid, so proximity is what it keys off. The gate matters for the same
// reason it does there — the awareness-100 lock-on used to `return` before
// anything could touch awareness, so once it hit 100 it stayed at 100 forever
// and there was no way to shake it off.
function tickCreepingDoom(connected) {
  const radius  = doomDetectionRadius();
  const nearest = nearestConnectedPlayer(connected);
  const inRange = !!nearest && nearest.dist <= radius;
  const steps   = doomStepsPerTick();

  if (doom.awareness >= 100 && inRange) {
    for (let s = 0; s < steps; s++) stepDoomToward(nearest.p.q, nearest.p.r, true);
    igniteHex(doom.q, doom.r, 2);
    return;
  }

  if (inRange) {
    for (let s = 0; s < steps; s++) stepDoomToward(nearest.p.q, nearest.p.r, false);
    doom.awareness = Math.min(100, doom.awareness + AWARENESS_GAIN);
  } else {
    const dirs = Object.values(DIR_DELTA);
    const [dq, dr] = dirs[Math.floor(Math.random() * dirs.length)];
    doom.q = ((doom.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
    doom.r = ((doom.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
    doom.awareness = Math.max(0, doom.awareness - AWARENESS_DECAY);
  }
  if (doom.awareness >= 76) igniteHex(doom.q, doom.r, 2);
}

// Mirrors doomTauntTier()/tickDoomTaunts() in world-system.hpp. Only the tier
// and a raw line index go on the wire; the wording lives in DOOM_TAUNTS
// (data/game-data.js) and is picked client-side.
function doomTauntTier() {
  if (doom.awareness >= 100) return 3;
  if (doom.awareness >=  76) return 2;
  if (doom.awareness >=  51) return 1;
  return 0;
}

function tickDoomTaunts(connected) {
  const tier    = doomTauntTier();
  const changed = tier !== doom.lastTauntTier;
  if (doom.tauntCooldown > 0) doom.tauntCooldown--;
  const sinceLast = DOOM_TAUNT_COOLDOWN - doom.tauntCooldown;
  const speak = changed ? sinceLast >= DOOM_TAUNT_MIN_GAP
                        : tier > 0 && doom.tauntCooldown === 0;
  if (!speak) return;
  if (tier === 0 && doom.lastTauntTier === 0) return;  // never rose, nothing to release
  const nearest = nearestConnectedPlayer(connected);
  doom.lastTauntTier = tier;
  if (!nearest) return;
  doom.tauntCooldown = DOOM_TAUNT_COOLDOWN;
  broadcast({ t: 'ev', k: 'doom_taunt', pid: nearest.p.id, tier,
              idx: Math.floor(Math.random() * 256) });
  console.log(`[doom] taunt tier=${tier} at pid=${nearest.p.id}`);
}

// Mirrors tickTunnelTaunts() in tunnels.hpp. A survivor camped underground on
// a hex with nothing built on it gets needled about it on a cooldown -- dry,
// unhelpful, and mechanically wrong, which is the joke. Only a raw line index
// goes on the wire; the 30 wordings live in TUNNEL_TAUNTS (data/game-data.js).
// Unicast, unlike the Doom's: this is a survivor's own second-guessing.
const TUNNEL_TAUNT_COOLDOWN = 8;   // world ticks; matches tunnels.hpp
const tunnelTauntTicks = {};       // pid -> world ticks camped below, counting up

function tickTunnelTaunts(connected) {
  for (const p of connected) {
    // The shelter test is always true today (SHELTER is refused at depth 1,
    // so a tunnel cell's shelter is always 0) but it is written out here for
    // the same reason it is in the firmware: building something is the one
    // thing that should shut this voice up if underground shelters ever land.
    const sheltered = p.dp && tunnel[p.tr]?.[p.tq]?.shelter > 0;
    if (!p.dp || p.ll === 0 || p.enc || sheltered) {
      tunnelTauntTicks[p.id] = 0;
      continue;
    }
    // Counts up, so an absent key reads as "just arrived" -- see tunnels.hpp.
    tunnelTauntTicks[p.id] = (tunnelTauntTicks[p.id] || 0) + 1;
    if (tunnelTauntTicks[p.id] < TUNNEL_TAUNT_COOLDOWN) continue;
    tunnelTauntTicks[p.id] = 0;
    sendToPid(p.id, { t: 'ev', k: 'tun_taunt', pid: p.id,
                      idx: Math.floor(Math.random() * 256) });
    console.log(`[tunnels] taunt at pid=${p.id}`);
  }
}

// Awareness-driven proximity effect, adjacent to Doom only — mirrors
// world-system.hpp's resolveDoomProximity(). Doom already moved to its new
// hex this tick by the time this runs.
function resolveDoomProximity(connected) {
  if (doom.awareness < 51) return;
  for (const p of connected) {
    if (p.enc) continue;
    if (hexDistWrap(p.q, p.r, doom.q, doom.r) > 1) continue;

    if (doom.awareness < 76) {
      broadcast({ t: 'ev', k: 'doom_warn', pid: p.id });
      continue;
    }
    const key = `${p.q}_${p.r}`;
    if (resources[key]) resources[key].amt = 0;
    let llLost = 0;
    if (doom.awareness >= 100 && p.ll > 0) {
      p.ll = Math.max(0, p.ll - 1);
      llLost = 1;
      if (p.ll === 0) broadcast({ t: 'ev', k: 'downed', pid: p.id });
    }
    broadcast({ t: 'ev', k: 'doom_act', pid: p.id, q: p.q, r: p.r, llLost });
  }
}

// Nearest Settlement to the caravan's position — a trade route, not a
// wander. Excludes the hex it's standing on and, when another candidate
// exists, the settlement it just left (caravan.pq/pr), so it doesn't settle
// into bouncing between two mutually-nearest settlements. Mirrors
// pickCaravanWaypoint() in world-system.hpp.
function pickCaravanWaypoint() {
  let bestQ = 0, bestR = 0, bestDist = 0, found = false;     // excludes current + previous
  let altQ  = 0, altR  = 0, altDist  = 0, altFound = false;  // excludes current only

  for (let r = 0; r < MAP_ROWS; r++) {
    for (let q = 0; q < MAP_COLS; q++) {
      if (terrainAt(q, r) !== TERRAIN_SETTLEMENT) continue;
      if (q === caravan.q && r === caravan.r) continue;
      const dist = hexDistWrap(caravan.q, caravan.r, q, r);
      if (!altFound || dist < altDist) { altDist = dist; altQ = q; altR = r; altFound = true; }
      if (q === caravan.pq && r === caravan.pr) continue;
      if (!found || dist < bestDist) { bestDist = dist; bestQ = q; bestR = r; found = true; }
    }
  }

  if (found || altFound) {
    caravan.pq = caravan.q;
    caravan.pr = caravan.r;
    caravan.wq = found ? bestQ : altQ;
    caravan.wr = found ? bestR : altR;
  } else {
    // No settlement anywhere on the map — degenerate fallback.
    caravan.wq = Math.floor(Math.random() * MAP_COLS);
    caravan.wr = Math.floor(Math.random() * MAP_ROWS);
  }
}
pickCaravanWaypoint();

function restockCaravan() {
  caravan.restockTimer = CARAVAN_RESTOCK_TICKS;
  for (let i = 0; i < 5; i++) caravan.inv[i] = Math.min(99, caravan.inv[i] + 4 + Math.floor(Math.random() * 5));
  // Shelf: rotate one occupied slot out, re-roll empties, top the rest up by
  // one to CARAVAN_STOCK_MAX — same rule as world-system.hpp.
  const rotate = Math.floor(Math.random() * CARAVAN_STOCK_SLOTS);
  if (caravan.stock[rotate].qty) caravan.stock[rotate] = { id: 0, qty: 0 };
  for (const s of caravan.stock) {
    if (!s.qty) {
      s.id  = rollCaravanStockItem();
      s.qty = s.id ? 1 + Math.floor(Math.random() * CARAVAN_STOCK_MAX) : 0;
    } else if (s.qty < CARAVAN_STOCK_MAX) {
      s.qty++;
    }
  }
}
restockCaravan();

function clearCaravanDebounce() {
  for (const k of Object.keys(lastCaravanHex)) delete lastCaravanHex[k];
}

// Steps one hex toward the waypoint via the same (approximate) DIR_DELTA
// table player movement uses. Returns true if it actually moved.
function moveCaravanOneStep() {
  const curDist = hexDistWrap(caravan.q, caravan.r, caravan.wq, caravan.wr);
  let bestDist = curDist, bestDq = 0, bestDr = 0, moved = false;
  for (const dir of Object.keys(DIR_DELTA)) {
    const [dq, dr] = DIR_DELTA[dir];
    const nq = ((caravan.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
    const nr = ((caravan.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
    const dist = hexDistWrap(nq, nr, caravan.wq, caravan.wr);
    if (dist < bestDist) { bestDist = dist; bestDq = dq; bestDr = dr; moved = true; }
  }
  if (!moved) return false;
  caravan.q = ((caravan.q + bestDq) % MAP_COLS + MAP_COLS) % MAP_COLS;
  caravan.r = ((caravan.r + bestDr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
  caravanTracks.add(`${caravan.q}_${caravan.r}`);
  return true;
}

// Is anyone standing in the shop? Mirrors caravanHasCustomer() in
// world-system.hpp: connected, not locked in an encounter, not downed.
function caravanHasCustomer(connected) {
  return connected.some(p => !p.enc && p.ll > 0 && p.q === caravan.q && p.r === caravan.r);
}

function tickCaravan(connected) {
  if (!caravan.active) return;
  // Wait while someone is trading rather than rolling off mid-purchase — see
  // the same guard in world-system.hpp's tickCaravan(). Capped at
  // CARAVAN_TRADE_HOLD so a camper cannot pin the trade route indefinitely.
  if (caravanHasCustomer(connected) && caravan.holdTicks < CARAVAN_TRADE_HOLD) {
    caravan.holdTicks++;
  } else if (caravan.q !== caravan.wq || caravan.r !== caravan.wr) {
    caravan.holdTicks = 0;
    if (moveCaravanOneStep()) clearCaravanDebounce();
  } else {
    caravan.holdTicks = 0;
    pickCaravanWaypoint();
  }
  if (caravan.restockTimer > 0) caravan.restockTimer--;
  else restockCaravan();
}

// Edge-triggered nudge, same debounce rule as world-system-spec.md's
// resolveProximity(): only fires the first tick a player is co-located.
function resolveCaravanProximity(connected) {
  if (!caravan.active) return;
  for (const p of connected) {
    if (p.enc) continue;
    if (p.q !== caravan.q || p.r !== caravan.r) continue;
    const key = `${caravan.q}_${caravan.r}`;
    if (lastCaravanHex[p.id] === key) continue;
    lastCaravanHex[p.id] = key;
    broadcast({ t: 'ev', k: 'car_avail', pid: p.id });
  }
}

function worldStateMsg() {
  return {
    caravan: {
      q: caravan.q, r: caravan.r, active: caravan.active, inv: caravan.inv,
      // [[itemId, qty, pricePerUnit], ...] — empty slots skipped (appendCaravanStock)
      stock: caravan.stock.filter(s => s.id && s.qty).map(s => [s.id, s.qty, caravanPrice(s.id)]),
    },
    doom: { q: doom.q, r: doom.r, awareness: doom.awareness },
    fire: fireArray(),
    flood: floodArray(),
  };
}

function syncMsg(id) {
  return {
    t: 'sync',
    id,
    vr: surfaceVis(players[id]),
    map: liveMapHex(players[id].q, players[id].r, surfaceVis(players[id])),
    p: Object.values(players).map(playerView),
    gs: { wp: weatherPhase, dc: dayCount, tc: threatClock },
    world: worldStateMsg(),
    gi: groundItemsList(),
    vc: VARIANT_COUNTS.vc,
    sv: VARIANT_COUNTS.sv,
    fa: VARIANT_COUNTS.fa,
  };
}

// Mirror of firmware's collectResource — must stay in sync with survival_state.hpp
function tryCollect(p, ws) {
  const k = `${p.q}_${p.r}`;
  const cell = resources[k];
  if (!cell || cell.res === 0 || cell.amt === 0) return; // nothing to collect (and no fail event — server-side knows truly nothing here)

  const total = p.inv.reduce((a, b) => a + b, 0);
  if (total >= INV_SLOTS) {
    // Inv-full: send col_fail to the moving player only.
    // cap mirrors the firmware's effectiveInvSlots() — the client shows it as "(have/cap)".
    send(ws, { t: 'ev', k: 'col_fail', pid: p.id, q: p.q, r: p.r, res: cell.res, reason: 2, cap: INV_SLOTS });
    console.log(`[col] FAIL inv-full pid=${p.id} q=${p.q} r=${p.r}`);
    return;
  }
  const room = INV_SLOTS - total;
  const gain = Math.min(cell.amt, room);
  if (gain === 0) return;

  const resType = cell.res;
  p.inv[resType - 1] = (p.inv[resType - 1] || 0) + gain;
  p.sc += gain * 10;
  cell.amt -= gain;
  const rem = cell.amt;
  if (rem === 0) {
    cell.res = 0;
    cell.respawnTimer = RESPAWN_TICKS;
  }
  console.log(`[col] OK pid=${p.id} q=${p.q} r=${p.r} res=${resType} gain=${gain} rem=${rem}`);
  broadcast({ t: 'ev', k: 'col', pid: p.id, q: p.q, r: p.r, res: resType, amt: gain, rem });
}

// Mirror of firmware's dropResource — must stay in sync with survival_state.hpp.
// Tokens land back on the hex when it can hold them (empty, or the same
// resource); a hex holding a different resource can't take them and they are
// lost. The collect award is refunded off the score either way, otherwise
// drop→collect on one hex is an infinite score pump.
function dropResource(p, res, qty) {
  if (res < 1 || res > RES_MAX_TYPE) return { dropped: 0, onGround: false, rem: 0 };
  const idx  = res - 1;
  const take = Math.min(Math.max(0, qty | 0), p.inv[idx] || 0);
  if (take === 0) {
    console.log(`[drop_res] SKIP empty pid=${p.id} res=${res} qty=${qty}`);
    return { dropped: 0, onGround: false, rem: 0 };
  }
  p.inv[idx] -= take;
  p.sc = Math.max(0, p.sc - take * 10);

  const k    = `${p.q}_${p.r}`;
  const cell = resources[k];
  const onGround = !cell || cell.res === 0 || cell.res === res;
  let rem = 0;
  if (onGround) {
    const pile = (cell && cell.res === res ? cell.amt : 0) + take;
    rem = Math.min(pile, 99);
    resources[k] = { res, amt: rem, respawnTimer: 0 };
  }
  console.log(`[drop_res] OK pid=${p.id} q=${p.q} r=${p.r} res=${res} qty=${take} ground=${onGround} pile=${rem} sc=${p.sc}`);
  return { dropped: take, onGround, rem };
}

// Respawn tick — broadcasts EVT_RESPAWN to all (no vision cull, mirrors fix).
setInterval(() => {
  for (const k of Object.keys(resources)) {
    const cell = resources[k];
    if (cell.res === 0 && cell.respawnTimer > 0) {
      if (--cell.respawnTimer === 0) {
        cell.res = 1 + Math.floor(Math.random() * RES_MAX_TYPE);
        cell.amt = 1 + Math.floor(Math.random() * 3);
        const [q, r] = k.split('_').map(Number);
        console.log(`[rsp] q=${q} r=${r} res=${cell.res} amt=${cell.amt}`);
        broadcast({ t: 'ev', k: 'rsp', q, r, res: cell.res, amt: cell.amt });
      }
    }
  }
}, 1000);

// Trade offer expiry sweep — mirrors the 30s window in game-server.hpp.
setInterval(() => {
  const now = Date.now();
  for (const fromId of Object.keys(tradeOffers)) {
    const offer = tradeOffers[fromId];
    if (offer.active && now >= offer.expiresAt) {
      offer.active = false;
      broadcast({ t: 'ev', k: 'trd_res', from: offer.fromId, to: offer.toId, res: 3 });
      console.log(`[trade] expired from=${offer.fromId} to=${offer.toId}`);
    }
  }
}, 1000);

function stateMsg() {
  // Build dense p[] indexed 0..MAX_PLAYERS-1; missing slots = offline stub.
  const p = [];
  for (let i = 0; i < MAX_PLAYERS; i++) {
    // Underground the direction mask is real (see tunnelValidMoves); on the
    // surface the mock still reports all six, since it models no terrain cost.
    if (players[i]?.dp) players[i].vm = tunnelValidMoves(players[i]);
    else if (players[i]) players[i].vm = 0x3F;
    p[i] = (players[i] && playerView(players[i])) || { on: false, q: 0, r: 0, sc: 0, inv: [0,0,0,0,0], sp: 0,
                           ll: 0, food: 0, water: 0, rad: 0,
                           mp: 0, fth: 0, wth: 0, vm: 0, rt: 0, wnd: [0, 0],
                           it: [], iq: [], eq: [0, 0, 0, 0, 0], enc: false,
                           dp: 0, tq: 0, tr: 0 };
  }
  return { t: 's', p, gs: { wp: weatherPhase, dc: dayCount, tc: threatClock }, world: worldStateMsg() };
}

function lobbyMsg() {
  const taken = new Set(Object.keys(players).map(Number));
  const avail = [];
  for (let i = 0; i < MAX_PLAYERS; i++) if (!taken.has(i)) avail.push(i);
  // vc/sv/fa mirror syncMsg()'s fields — firmware now sends these on the
  // lobby message too (see network-sync.hpp sendLobbyMsg), so the client can
  // start preloading hex/shelter/forage-animal art before a character is picked.
  return { t: 'lobby', avail, vc: VARIANT_COUNTS.vc, sv: VARIANT_COUNTS.sv, fa: VARIANT_COUNTS.fa };
}

// ── Item actions — mirrors equipItem()/unequipItem()/useItem()/dropItem()/
// pickupGroundItem() in inventory_items.hpp. it[]/iq[] are fixed INV_SLOTS_MAX
// arrays, index-stable like the firmware's invType[]/invQty[] (0 = empty) —
// equip/unequip/use/drop clear or fill a slot in place, they never shift
// other slots around.

// (There is deliberately no shared "first free slot" helper: equip and unequip
// both have to search against the cap the swap ENDS with, not the current one,
// and each computes its own. A helper reading the live cap would be wrong for
// both — that was the orphan bug.)

// How many more of itemId a pack (it/iq arrays, `slots` wide) could hold:
// spare capacity on every existing stack plus a full stack per empty slot.
// Mirrors invRoomFor() in inventory_items.hpp — the one rule addItemToInv()
// places by and craftRecipe() dry-runs against (on scratch arrays), so the
// two never disagree about what fits.
function invRoomFor(it, iq, slots, itemId) {
  const cap = ITEM_DEFS[itemId]?.maxStack || 1;
  let room = 0;
  for (let s = 0; s < slots; s++) {
    if (it[s] === itemId)  room += Math.max(0, cap - (iq[s] || 0));
    else if (!it[s])       room += cap;
  }
  return room;
}

// Place up to qty of itemId into p's pack — top up every existing stack with
// room first, then open new stacks in empty slots. Returns how many landed.
// Mirrors addItemToInv() in inventory_items.hpp: pickup, loot and crafting
// all fill the pack by this one rule, so a full stack never blocks placement
// while an empty slot is free.
function addItemToInv(p, itemId, qty) {
  if (!itemId || !qty) return 0;
  const cap = ITEM_DEFS[itemId]?.maxStack || 1;
  const slots = effectiveInvSlots(p);
  let left = qty;
  for (let s = 0; s < slots && left > 0; s++) {
    if (p.it[s] !== itemId || p.iq[s] >= cap) continue;
    const add = Math.min(left, cap - p.iq[s]);
    p.iq[s] += add; left -= add;
  }
  for (let s = 0; s < slots && left > 0; s++) {
    if (p.it[s]) continue;
    const add = Math.min(left, cap);
    p.it[s] = itemId; p.iq[s] = add; left -= add;
  }
  return qty - left;
}

// Place itemId x qty into p's pack via addItemToInv(); overflow goes to the
// ground at the player's hex. Mirrors grantItemOrDrop() in
// network-msg-encounter.hpp.
function grantItemOrDrop(p, itemId, qty) {
  if (!itemId || !qty) return;
  qty -= addItemToInv(p, itemId, qty);
  if (!qty) return;
  let gslot = groundItems.findIndex((g) => g.itemType === itemId && g.q === p.q && g.r === p.r);
  if (gslot < 0) gslot = groundItems.findIndex((g) => !g.itemType);
  if (gslot < 0) return; // ground full — item lost, matches the firmware's warn-and-drop behaviour
  groundItems[gslot].q = p.q; groundItems[gslot].r = p.r;
  groundItems[gslot].itemType = itemId;
  groundItems[gslot].qty = Math.min(255, groundItems[gslot].qty + qty);
}

// Use the item in inventory slot slotIdx. Consumables apply their stat deltas
// and effects, then lose one charge; key items with an effect can be "read"
// any number of times and are never spent. Mirrors useItem() in
// inventory_items.hpp — cure_status/threat_mod are applied here; reveal_fog
// (vis-disk resend) and narrative (client-only efxp echo) are handled by the
// use_item switch case below since they need the pre-use item def.
function useItem(p, slotIdx) {
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) return false;
  const itemId = p.it[slotIdx];
  if (!itemId) return false;
  const def = ITEM_DEFS[itemId];
  if (!def) return false;
  const hasEffect       = !!def.effect && def.effect !== 'none';
  const isKeyWithEffect = def.category === 'key' && hasEffect;
  if (def.category !== 'consumable' && !isKeyWithEffect) return false;

  const state = { llDelta: 0 };
  if (def.food)  { const steps = def.food;  for (let i = 0; i < Math.abs(steps); i++) applyFStep(p, steps > 0 ? 1 : -1, state); }
  if (def.water) { const steps = def.water; for (let i = 0; i < Math.abs(steps); i++) applyWStep(p, steps > 0 ? 1 : -1, state); }
  state.llDelta += (def.ll || 0);
  if (state.llDelta) p.ll = Math.max(0, Math.min(effectiveMaxLL(p), p.ll + state.llDelta));
  if (def.rad) p.rad = Math.max(0, Math.min(10, p.rad + def.rad));
  if (def.mp)  p.mp  = Math.max(0, p.mp + def.mp);

  for (const [fx, param] of [[def.effect, def.param], [def.effect2, def.param2]]) {
    if (fx === 'cure_status') {
      for (let i = 0; i < (param || 0); i++) { if (!healWound(p, 0) && !healWound(p, 1)) break; }
    } else if (fx === 'threat_mod') {
      const delta = param > 127 ? param - 256 : param; // signed int8_t, matches items.cfg's 253/254 convention
      threatClock = Math.max(0, Math.min(20, threatClock + delta));
    }
  }

  if (isKeyWithEffect) return true;
  if (p.iq[slotIdx] > 1) p.iq[slotIdx]--;
  else { p.it[slotIdx] = 0; p.iq[slotIdx] = 0; }
  return true;
}

// Drop qty of the item in slotIdx at the player's current hex. Mirrors
// dropItem() in inventory_items.hpp.
function dropItem(p, slotIdx, qty) {
  if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX || qty <= 0) return false;
  if (!p.it[slotIdx] || p.iq[slotIdx] < qty) return false;
  const itemId = p.it[slotIdx];

  let gslot = groundItems.findIndex((g) => g.itemType === itemId && g.q === p.q && g.r === p.r);
  if (gslot < 0) gslot = groundItems.findIndex((g) => !g.itemType);
  if (gslot < 0) return false;

  p.iq[slotIdx] -= qty;
  if (!p.iq[slotIdx]) p.it[slotIdx] = 0;

  groundItems[gslot].q = p.q; groundItems[gslot].r = p.r;
  groundItems[gslot].itemType = itemId;
  groundItems[gslot].qty = Math.min(255, groundItems[gslot].qty + qty);
  return true;
}

// Pick up as much of ground item gslot as fits; player must be standing on
// that hex. Mirrors pickupGroundItem() in inventory_items.hpp.
function pickupGroundItem(p, gslot) {
  if (gslot < 0 || gslot >= MAX_GROUND) return false;
  const gi = groundItems[gslot];
  if (!gi.itemType || gi.q !== p.q || gi.r !== p.r) return false;

  const canTake = addItemToInv(p, gi.itemType, gi.qty);
  if (!canTake) return false;
  gi.qty -= canTake;
  if (!gi.qty) { gi.itemType = 0; gi.q = 0; gi.r = 0; }
  return true;
}

// Mirrors equipItem() in inventory_items.hpp, orphan guard included: every
// bound is the pack size the swap ENDS with, because trading a slots-granting
// item (Hoarder's Rig, Backpack, Knife-Wrench) for one without shrinks the
// pack and anything left past the new cap becomes unreachable. slotIdx is
// exempt from the guard — the incoming item vacates it.
// Mirrors pushVisDisk() in network-msg-items.hpp. Putting on (or taking off)
// gear with reveal_fog param 1 moves the sight radius, and the client only
// ever learns a radius from a vis message -- without this the goggles reveal
// nothing until the next step, and removing them leaves the client too wide.
function pushVisDisk(ws, p) {
  if (p.dp) return;                       // underground vis is its own path
  const svr = surfaceVis(p);
  send(ws, { t: 'vis', q: p.q, r: p.r, vr: svr, cells: buildVisDisk(p.q, p.r, svr) });
}

function equipItem(ws, id, msg) {
  const p = players[id];
  if (!p) return;
  const visBefore = equipVisionBonus(p);
  const slotIdx = msg.slot | 0;
  const itemId  = (slotIdx >= 0 && slotIdx < INV_SLOTS_MAX) ? p.it[slotIdx] : 0;
  const def     = itemId ? ITEM_DEFS[itemId] : null;
  let ok = !!def && def.category === 'equipment' && def.eslot >= 0;
  if (ok) {
    const prev = p.eq[def.eslot];

    // Slot count once the swap is done — incoming item on, outgoing item off.
    p.eq[def.eslot] = itemId;
    const newSlots  = effectiveInvSlots(p);
    p.eq[def.eslot] = prev;

    // Refuse if that would strand anything past the new cap.
    for (let i = newSlots; i < INV_SLOTS_MAX; i++) {
      if (i !== slotIdx && p.it[i]) { ok = false; break; }
    }

    if (ok) {
      const qty0 = p.iq[slotIdx];
      p.eq[def.eslot] = itemId;
      p.it[slotIdx] = 0;
      p.iq[slotIdx] = 0;

      // The old item takes the first free slot under the new cap — possibly
      // the one just vacated. Nowhere to put it means the swap cannot happen.
      if (prev) {
        let dest = -1;
        for (let i = 0; i < newSlots; i++) if (!p.it[i]) { dest = i; break; }
        if (dest < 0) {
          p.eq[def.eslot] = prev;
          p.it[slotIdx] = itemId; p.iq[slotIdx] = qty0;
          ok = false;
        } else {
          p.it[dest] = prev; p.iq[dest] = 1;
        }
      }
    }
  }
  // A swap can LOWER the ceiling (Dent Absorber -> Backpack, both body).
  // unequipItem clamps for this; equipItem did not, on either side, and the
  // survivor kept points earned under the old ceiling forever.
  if (ok) p.ll = Math.min(p.ll, effectiveMaxLL(p));
  send(ws, { t: 'item_result', ok, act: 'equip', slot: slotIdx, pid: id, it: p.it, iq: p.iq, eq: p.eq, ...packFields(p) });
  if (ok && equipVisionBonus(p) !== visBefore) pushVisDisk(ws, p);
  if (ok) broadcast(stateMsg());
}

function unequipItem(ws, id, msg) {
  const p = players[id];
  if (!p) return;
  const visBefore = equipVisionBonus(p);
  const eslot  = msg.eslot | 0;
  const itemId = p.eq[eslot];
  let ok = !!itemId;
  if (ok) {
    // Pack size once this item is off — refuse if anything sits beyond it,
    // else the pack would shrink around items the player couldn't reach.
    // Mirrors the orphan guard in unequipItem(), inventory_items.hpp.
    p.eq[eslot] = 0;
    const newSlots = effectiveInvSlots(p);
    p.eq[eslot] = itemId;
    for (let i = newSlots; i < INV_SLOTS_MAX; i++) if (p.it[i]) ok = false;
    if (ok) {
      const freeSlot = p.it.findIndex((t, i) => i < newSlots && !t);
      ok = freeSlot >= 0;
      if (ok) { p.it[freeSlot] = itemId; p.iq[freeSlot] = 1; p.eq[eslot] = 0; }
    }
  }
  if (ok) p.ll = Math.min(p.ll, effectiveMaxLL(p));
  send(ws, { t: 'item_result', ok, act: 'unequip', eslot, pid: id, it: p.it, iq: p.iq, eq: p.eq, ...packFields(p) });
  if (ok && equipVisionBonus(p) !== visBefore) pushVisDisk(ws, p);
  if (ok) broadcast(stateMsg());
}

// ── HTTP static server ──────────────────────────────────────────────────────
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js'  : 'application/javascript; charset=utf-8',
  '.css' : 'text/css; charset=utf-8',
  '.png' : 'image/png',
  '.jpg' : 'image/jpeg',
  '.svg' : 'image/svg+xml',
  '.ico' : 'image/x-icon',
  '.json': 'application/json; charset=utf-8',
};

const UPLOAD_DIR = path.resolve(__dirname, 'uploads');

// Mirror the firmware's POST /upload?dest=/data/foo handler so sync_data.ps1
// works against the mock without a board. Body is streamed straight to disk
// under mock-server/uploads/<dest> (gitignored).
function handleUpload(req, res) {
  const u = new URL(req.url, 'http://x');
  let dest = u.searchParams.get('dest');
  if (!dest) {
    res.writeHead(400, { 'Content-Type': 'text/plain' });
    res.end('missing ?dest=');
    return;
  }
  if (dest.includes('..')) {
    res.writeHead(400, { 'Content-Type': 'text/plain' });
    res.end('bad dest');
    return;
  }
  if (!dest.startsWith('/')) dest = '/' + dest;
  const outPath = path.join(UPLOAD_DIR, dest);
  if (!outPath.startsWith(UPLOAD_DIR)) {
    res.writeHead(403); res.end('forbidden'); return;
  }
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  const sink = fs.createWriteStream(outPath);
  let total = 0;
  req.on('data', (chunk) => { total += chunk.length; });
  req.pipe(sink);
  sink.on('finish', () => {
    console.log(`[upload] ${dest}  ${total} bytes`);
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('OK');
  });
  sink.on('error', (e) => {
    console.error('[upload] write fail', e);
    res.writeHead(500); res.end('write fail');
  });
}

// Dev asset manifest. The browser's AssetLoader (index.html) fetches
// /assets.json first. On the K10 that is the file scripts/build_web.ps1
// generates (one gzipped bundle). Here we synthesise it from
// data/web-assets.json so every source file loads individually — real
// filenames + line numbers in devtools, no build step. The version is derived
// from the newest mtime so it only changes when you edit something.
// Set MOCK_BUNDLE=1 to serve the built data/assets.json + app.bundle.js
// instead and exercise the exact path the firmware takes.
function handleDevManifest(res) {
  let src;
  try { src = JSON.parse(fs.readFileSync(path.join(DATA_DIR, 'web-assets.json'), 'utf8')); }
  catch (e) {
    res.writeHead(500, { 'Content-Type': 'text/plain' });
    res.end('data/web-assets.json unreadable: ' + e.message);
    return;
  }
  const stat  = (f) => { try { return fs.statSync(path.join(DATA_DIR, f)); } catch (_) { return null; } };
  const entry = (f) => { const s = stat(f); return { url: f, size: s ? s.size : 0 }; };
  const styles  = Array.isArray(src.styles)  ? src.styles  : [];
  const scripts = Array.isArray(src.scripts) ? src.scripts : [];
  const newest  = Math.max(0, ...[...styles, ...scripts, 'index.html'].map(f => { const s = stat(f); return s ? s.mtimeMs : 0; }));
  const body = JSON.stringify({
    version:   'dev-' + Math.round(newest).toString(36),
    generated: new Date().toISOString(),
    mode:      'sources',
    styles:    styles.map(entry),
    scripts:   scripts.map(entry),
  }, null, 2);
  res.writeHead(200, { 'Content-Type': MIME['.json'], 'Cache-Control': 'no-store' });
  res.end(body);
}

const httpServer = http.createServer((req, res) => {
  const reqPath = req.url.split('?')[0];
  if (req.method === 'POST' && reqPath === '/upload') {
    handleUpload(req, res);
    return;
  }
  if (req.method === 'GET' && reqPath === '/enc') {
    handleEnc(req, res);
    return;
  }
  if (req.method === 'GET' && reqPath === '/assets.json' && !BUNDLE_MODE) {
    handleDevManifest(res);
    return;
  }
  let urlPath = decodeURIComponent(reqPath);
  if (urlPath === '/' || urlPath === '') urlPath = '/index.html';
  const filePath = path.join(DATA_DIR, urlPath);
  if (!filePath.startsWith(DATA_DIR)) { res.writeHead(403); res.end(); return; }
  const mime = MIME[path.extname(filePath).toLowerCase()] || 'application/octet-stream';
  fs.readFile(filePath, (err, data) => {
    if (!err) {
      res.writeHead(200, { 'Content-Type': mime, 'Cache-Control': 'no-store' });  // dev loop: always pick up edits
      res.end(data);
      return;
    }
    // Mirror the firmware: a "<name>.gz" sibling answers for "<name>".
    fs.readFile(filePath + '.gz', (gzErr, gz) => {
      if (gzErr) { res.writeHead(404); res.end('Not found'); return; }
      res.writeHead(200, { 'Content-Type': mime, 'Content-Encoding': 'gzip',
                           'Cache-Control': 'no-store' });
      res.end(gz);
    });
  });
});

// ── WebSocket /ws ───────────────────────────────────────────────────────────
const wss = new WebSocketServer({ noServer: true });

httpServer.on('upgrade', (req, socket, head) => {
  if (req.url !== '/ws') { socket.destroy(); return; }
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req));
});

wss.on('connection', (ws) => {
  console.log('[WS] client connected');
  sockets.set(ws, -1);
  // Tell client which slots are free
  send(ws, lobbyMsg());
  send(ws, wifiNetsMsg());
  send(ws, wifiLinkMsg());

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }
    console.log('[RX]', msg.t, msg);

    switch (msg.t) {
      case 'wifi': {
        const ssid = (msg.ssid || '').trim();
        send(ws, { t: 'wifi', status: 'saved', ssid, pass: msg.pass || '' });
        if (ssid) {
          knownNets   = [ssid, ...knownNets.filter(s => s !== ssid)].slice(0, 8);
          currentSsid = ssid;
          send(ws, { t: 'wifi', status: 'ok', ip: '192.168.1.42' });
          broadcast(wifiNetsMsg());
        }
        break;
      }

      case 'wifi_forget': {
        const ssid = msg.ssid || '';
        if (!knownNets.includes(ssid)) break;
        knownNets = knownNets.filter(s => s !== ssid);
        broadcast({ t: 'wifi', status: 'forgot', ssid });
        broadcast(wifiNetsMsg());
        break;
      }

      case 'pick': {
        const id = msg.arch | 0;
        if (id < 0 || id >= MAX_PLAYERS || players[id]) {
          send(ws, lobbyMsg());
          break;
        }
        players[id] = makePlayer(id);
        sockets.set(ws, id);
        send(ws, { t: 'asgn', id });
        send(ws, syncMsg(id));
        broadcast(stateMsg());
        break;
      }

      case 'm': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const [dq, dr] = DIR_DELTA[msg.d] || [0, 0];
        // ── Underground ──
        // Separate path, same as moveTunnel() in survival_state.hpp: the board
        // does not wrap, Collapsed Tunnel (15) is impassable, and a step costs
        // TUNNEL_MC. This is also the only place the mock enforces terrain at
        // all -- the surface branch below still moves unconditionally.
        if (p.dp) {
          const nq = p.tq + dq, nr = p.tr + dr;
          if (!tunIn(nq, nr)) break;                 // walled, not toroidal
          if (tunnel[nr][nq].tt === 15) break;       // collapsed
          p.tq = nq; p.tr = nr;
          p.sp += 1;
          p.mp = Math.max(0, p.mp - TUNNEL_MC);
          tunnel[nr][nq].footprints |= (1 << id);
          broadcast({ t: 'ev', k: 'mv', pid: id, q: p.tq, r: p.tr, radd: 0,
                      rad: p.rad, exploD: 0, mp: p.mp, trk: 0, dp: 1 });
          const cell = tunnel[nr][nq];
          if (cell.res && cell.amt) {
            const gain = cell.amt;
            p.inv[cell.res - 1] = Math.min(99, p.inv[cell.res - 1] + gain);
            p.sc += gain * 10;
            broadcast({ t: 'ev', k: 'col', pid: id, q: p.tq, r: p.tr,
                        res: cell.res, amt: gain, rem: 0, dp: 1 });
            cell.res = 0; cell.amt = 0;
          }
          // Landing on a shaft climbs out. After the pickup, so the last thing
          // you grab on the way past still lands in the pack.
          if (stepUpIfShaft(ws, id, p)) { broadcast(stateMsg()); break; }
          const tvr = tunnelVis(p);
          send(ws, { t: 'vis', dp: 1, q: p.tq, r: p.tr, vr: tvr,
                     cells: buildTunnelVisDisk(p.tq, p.tr, tvr) });
          broadcast(stateMsg());
          break;
        }
        p.q = ((p.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
        p.r = ((p.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
        delete lastCaravanHex[id];  // moved — re-arm the caravan trade prompt
        p.sp += 1;
        if (p.mp > 0) p.mp -= 1;
        // Mirrors hasTireTracks() in inventory_items.hpp: any equipped item
        // flagged items.cfg "tracks" (the Motorbike) marks the hex like the
        // caravan does. Carried on the mv event (trk) so every client, not
        // just this player's own vis-disk, can stamp an already-revealed hex —
        // same reasoning as footprints (see data/network.js's _evMv).
        const laidTrack = p.eq.some((itemId) => ITEM_DEFS[itemId]?.tracks);
        if (laidTrack) caravanTracks.add(`${p.q}_${p.r}`);
        // EVT_MOVE first so client position updates before col event arrives.
        broadcast({ t: 'ev', k: 'mv', pid: id, q: p.q, r: p.r, radd: 0, rad: p.rad, exploD: 0, mp: p.mp, trk: laidTrack ? 1 : 0 });
        tryCollect(p, ws);
        // Landing on a Bunker Entrance / Vent Shaft drops you into the tunnels.
        if (stepDownIfHatch(ws, id, p)) { broadcast(stateMsg()); break; }
        // Visdisk reflects current resource state (post-collection).
        const svr = surfaceVis(p);
        const cells = buildVisDisk(p.q, p.r, svr);
        send(ws, { t: 'vis', q: p.q, r: p.r, vr: svr, cells });
        broadcast(stateMsg());
        break;
      }

      case 'act': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        // ACT_REST=7 — idempotent like doRest(): once set, only dawnUpkeepAll()
        // clears it. All-connected-players-resting is what ends the day early,
        // so recovery comes from dawn upkeep, not an instant buff here.
        if (msg.a === 7) {
          p.rt = 1;
        } else if (msg.a === 2) {
          // ACT_TREAT — mirrors doTreat() in actions_game_loop.hpp: the Medic
          // (archetype 2) may treat a Major Wound anywhere; everyone else
          // needs to be standing in a Settlement. Costs 2 MP + 1 Medicine,
          // Endure vs TREAT_DN; a near miss downgrades the Major Wound to a
          // Minor one instead of clearing it outright.
          const isMedic      = p.arch === 2;
          const inSettlement = terrainAt(p.q, p.r) === TERRAIN_SETTLEMENT;
          if (!isMedic && !inSettlement) {
            send(ws, { t: 'err', msg: 'Only the Medic can treat outside a Settlement' });
          } else if (!p.wnd[1]) {
            send(ws, { t: 'err', msg: 'No Major Wound to treat' });
          } else if (p.inv[3] < 1 || p.mp < 2) {
            send(ws, { t: 'err', msg: 'Needs 2 MP and 1 Medicine' });
          } else {
            p.mp -= 2;
            p.inv[3]--;
            // Mirrors checkSkillMod(): major wounds hit every skill, minor
            // wounds hit Endure only, and the Endurer (archetype 5) reads
            // Endure 1 higher.
            let mod = -p.wnd[1] - p.wnd[0];
            if (p.arch === 5) mod += 1;
            let tot = d6() + d6() + (p.sk[4] || 0) + mod;
            if (forcedOutcome !== null) { tot = forcedOutcome ? TREAT_DN : TREAT_DN - 1; forcedOutcome = null; }
            if (tot >= TREAT_DN) {
              healWound(p, 1);
              p.sc += 6;
            } else if (tot >= TREAT_DN - 1) {
              healWound(p, 1);
              p.wnd[0] = Math.min(WOUND_MAX_EACH, p.wnd[0] + 1);
              p.sc += 2;
            }
          }
        } else if (msg.a === 5) {
          // ACT_CRAFT — mirrors doCraft()/applyRecipe() in
          // actions_game_loop.hpp/inventory_items.hpp: Settlement-only,
          // 1 MP, deterministic (no skill roll). Recipes are secret — p.kr
          // must already have this bit set (learned via an encounter's
          // "recipe" loot entry, see encChoice/encBank below).
          const recipeId      = msg.r | 0;
          const inSettlement   = terrainAt(p.q, p.r) === TERRAIN_SETTLEMENT;
          const knownRecipe    = recipeId > 0 && recipeId <= 32 && ((p.kr >> (recipeId - 1)) & 1) === 1;
          // Refusal strings match doCraft()/applyRecipe() in the firmware
          // byte-for-byte so the client's err toast reads the same either way.
          let blocked = null;
          if (!inSettlement)     blocked = 'CRAFT requires a Settlement';
          else if (p.mp < 1)     blocked = 'Not enough MP';
          else if (!knownRecipe) blocked = "You don't know that recipe";
          else                   blocked = craftRecipe(p, recipeId);
          if (blocked) {
            send(ws, { t: 'err', msg: blocked });
          } else {
            p.mp -= 1;
            p.sc += 2;
            // Targeted ack — mirrors the firmware's craftAck in
            // network-msg-player.hpp so the client's "Crafted X" log line
            // fires the same way against the mock as against real hardware.
            send(ws, { t: 'item_result', ok: true, act: 'craft', pid: id, recipe: recipeId,
                       it: p.it, iq: p.iq, inv: p.inv, kr: p.kr, ...packFields(p) });
          }
        } else if (p.dp && ACTS_REFUSED_UNDERGROUND.has(msg.a | 0)) {
          // Mirrors handleAction(): SHELTER, CRAFT and SURVEY are refused
          // outright at depth 1, silently. SURVEY is the one that matters --
          // doSurvey() writes a buffer sized for the 75x57 surface map.
          break;
        } else if (p.dp && (msg.a | 0) === 0) {
          // FORAGE self-blocks underground: TERRAIN_FORAGE_DN[14] is 0.
          // Nothing grows down there, which is what puts every dive on a
          // food clock.
          break;
        } else {
          // Generic: drain a bit, give a resource.
          //
          // The resource used to be inv[a % 5], which is wrong for every
          // action there is -- FORAGE handed out water, WATER handed out
          // food, SCAV handed out medicine. A bot drinking itself to death
          // while its water track fell is how it was found.
          const slot = ACT_RESOURCE[msg.a | 0];
          // Only SCAVENGE is bound by the pack size (mirrors doScav): FORAGE
          // and WATER are survival and may overfill, paying the encumbrance
          // penalty in effectiveMP() instead of being refused. Capping those
          // two let a pack full of scrap starve a survivor through a button
          // that did nothing and said nothing.
          if ((msg.a | 0) === 3 && tokenRoomFor(p) <= 0) {
            broadcast({ t: 'ev', k: 'act', pid: id, a: msg.a | 0, out: 0,
                        bw: 1, mp: p.mp, ll: p.ll });
            break;
          }
          p.mp = Math.max(0, p.mp - (msg.mp || 1));
          if (slot !== undefined) p.inv[slot] = (p.inv[slot] || 0) + 1;
          p.sc += 1;
        }
        broadcast(stateMsg());
        break;
      }

      case 'trade_offer': {
        const id   = sockets.get(ws);
        const from = players[id];
        if (!from) break;
        const toId = msg.to | 0;
        const to   = players[toId];
        if (encounters[id] || encounters[toId]) {
          send(ws, { t: 'err', msg: 'Cannot trade during encounter' });
          break;
        }
        const give = [0, 0, 0, 0, 0], want = [0, 0, 0, 0, 0];
        for (let i = 0; i < 5; i++) {
          give[i] = Math.max(0, Math.min(99, (Array.isArray(msg.give) ? msg.give[i] : 0) | 0));
          want[i] = Math.max(0, Math.min(99, (Array.isArray(msg.want) ? msg.want[i] : 0) | 0));
        }
        const total = give.reduce((a, b) => a + b, 0) + want.reduce((a, b) => a + b, 0);
        // One outstanding offer per sender — a second offer (even to a
        // different target) is blocked until the first resolves/expires.
        const valid = total > 0 && toId !== id && !tradeOffers[id]?.active &&
          sameHex(from, to) && hasResources(from, give);
        if (!valid) { send(ws, { t: 'trade_fail' }); break; }
        tradeOffers[id] = { active: true, fromId: id, toId, give, want, expiresAt: Date.now() + TRADE_EXPIRE_MS };
        broadcast({ t: 'ev', k: 'trd_off', from: id, to: toId, give, want });
        break;
      }

      case 'trade_accept': {
        const id     = sockets.get(ws);
        const me     = players[id];
        if (!me) break;
        const fromId = msg.from | 0;
        if (encounters[id] || encounters[fromId]) {
          send(ws, { t: 'err', msg: 'Cannot trade during encounter' });
          break;
        }
        const offer = tradeOffers[fromId];
        const from  = players[fromId];
        if (!offer?.active || offer.toId !== id) break;  // nothing addressed to me — ignore
        let res;
        if (Date.now() < offer.expiresAt && sameHex(from, me) &&
            hasResources(from, offer.give) && hasResources(me, offer.want)) {
          for (let i = 0; i < 5; i++) {
            from.inv[i] = Math.min(99, from.inv[i] - offer.give[i] + offer.want[i]);
            me.inv[i]   = Math.min(99, me.inv[i]   - offer.want[i] + offer.give[i]);
          }
          res = 1;
        } else {
          res = 4; // accepted, but conditions no longer hold (moved, disconnected, spent resources)
        }
        offer.active = false;
        broadcast({ t: 'ev', k: 'trd_res', from: fromId, to: id, res });
        broadcast(stateMsg());
        break;
      }

      case 'trade_decline': {
        const id     = sockets.get(ws);
        const fromId = msg.from | 0;
        const offer  = tradeOffers[fromId];
        if (id === undefined || !offer?.active || offer.toId !== id) break;
        offer.active = false;
        broadcast({ t: 'ev', k: 'trd_res', from: fromId, to: id, res: 2 });
        break;
      }

      case 'enc_start':  encStart(ws, sockets.get(ws), msg);  break;
      case 'enc_choice': encChoice(ws, sockets.get(ws), msg); break;
      case 'enc_bank':   encBank(ws, sockets.get(ws), msg);   break;
      case 'enc_abort':  encAbort(sockets.get(ws));           break;

      case 'equip_item':   equipItem(ws, sockets.get(ws), msg);   break;
      case 'unequip_item': unequipItem(ws, sockets.get(ws), msg); break;

      case 'use_item': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const slotIdx = msg.slot | 0;
        if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) break;
        const preDef = p.it[slotIdx] ? ITEM_DEFS[p.it[slotIdx]] : null;
        let narParam = 0, revealParam = 0;
        if (preDef) {
          if (preDef.effect  === 'reveal_fog') revealParam = preDef.param;
          if (preDef.effect2 === 'reveal_fog') revealParam = preDef.param2;
          if (preDef.effect  === 'narrative')  narParam    = preDef.param;
          if (preDef.effect2 === 'narrative')  narParam    = preDef.param2;
        }
        const ok = useItem(p, slotIdx);
        send(ws, { t: 'item_result', ok, act: 'use', slot: slotIdx, pid: id, it: p.it, iq: p.iq, eq: p.eq, efxp: narParam, ...packFields(p) });
        if (ok) {
          broadcast(stateMsg());
          if (revealParam >= 2) {
            // No persistent fog memory to "reveal" in the mock, so a one-shot
            // or whole-map read just resends a bigger vis-disk — capped well
            // under map size so the payload stays reasonable.
            const vr = Math.min(revealParam === 99 ? 30 : revealParam, 30);
            const cells = buildVisDisk(p.q, p.r, vr);
            send(ws, { t: 'vis', q: p.q, r: p.r, vr, cells });
          }
        }
        break;
      }

      case 'drop_item': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const slotIdx = msg.slot | 0;
        const qty     = Math.max(1, msg.qty | 0);
        const ok = dropItem(p, slotIdx, qty);
        send(ws, { t: 'item_result', ok, act: 'drop', slot: slotIdx, pid: id, it: p.it, iq: p.iq, eq: p.eq, ...packFields(p) });
        if (ok) { broadcast(groundUpdateMsg(p.q, p.r)); broadcast(stateMsg()); }
        break;
      }

      case 'drop_res': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const res = msg.res | 0;
        const qty = msg.qty | 0;
        const { dropped, onGround, rem } = dropResource(p, res, qty);
        send(ws, { t: 'res_result', ok: dropped > 0, pid: id, res, qty: dropped,
                   grd: onGround ? 1 : 0, rem, q: p.q, r: p.r, inv: p.inv, sc: p.sc });
        if (dropped > 0) {
          // Same event the respawn tick uses — every client updates the hex.
          if (onGround) broadcast({ t: 'ev', k: 'rsp', q: p.q, r: p.r, res, amt: rem });
          broadcast(stateMsg());
        }
        break;
      }

      case 'pickup_item': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const gslot = msg.gslot | 0;
        const ok = pickupGroundItem(p, gslot);
        send(ws, { t: 'item_result', ok, act: 'pickup', gslot, pid: id, it: p.it, iq: p.iq, eq: p.eq, ...packFields(p) });
        if (ok) { broadcast(groundUpdateMsg(p.q, p.r)); broadcast(stateMsg()); }
        break;
      }

      case 'dbg_force':   // Test-only: {"t":"dbg_force","out":0} forces the next roll to fail (1 = succeed)
        forcedOutcome = msg.out ? 1 : 0;
        break;

      case 'dbg_enc': {
        // Test-only: open a specific encounter regardless of position/POI.
        // {"t":"dbg_enc","biome":"urban","id":3}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p || encounters[id]) break;
        openEncounter(ws, id, p, p.q, p.r, String(msg.biome || 'urban'), (msg.id | 0) || 1, false);
        break;
      }

      case 'dbg_setinv': {
        // Test-only: clobber inv so we can exercise inv-full / partial-pickup paths.
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p || !Array.isArray(msg.inv)) break;
        for (let i = 0; i < 5; i++) p.inv[i] = msg.inv[i] | 0;
        broadcast(stateMsg());
        break;
      }

      case 'dbg_setitem': {
        // Test-only: place a typed item directly into a slot, bypassing
        // pickup/loot, so use/equip/drop/pickup can be exercised without
        // waiting on an encounter. {"t":"dbg_setitem","slot":0,"item":25,"qty":1}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const slotIdx = msg.slot | 0;
        if (slotIdx < 0 || slotIdx >= INV_SLOTS_MAX) break;
        p.it[slotIdx] = msg.item | 0;
        p.iq[slotIdx] = Math.max(0, msg.qty | 0);
        broadcast(stateMsg());
        break;
      }

      case 'dbg_weather': {
        // Test-only: force weatherPhase without waiting for a dawn roll.
        // {"t":"dbg_weather","phase":2}  (0=clear 1=rain 2=storm 3=chem 4=strangle fog 5=mist/fog)
        const phase = Math.max(0, Math.min(5, msg.phase | 0));
        if (phase === weatherPhase) break;
        weatherPhase = phase;
        broadcast({ t: 'ev', k: 'weather', phase: weatherPhase, ticks: 0 });
        broadcast(stateMsg());
        console.log(`[weather] forced -> ${WEATHER_NAMES[weatherPhase]}`);
        break;
      }

      case 'dbg_quake': {
        // Test-only: force a quake near the sender immediately, ignoring the cooldown.
        // {"t":"dbg_quake"}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        triggerQuake(p.q, p.r);
        break;
      }

      case 'dbg_settle': {
        // Test-only: force a settlement to form on the sender's hex, for
        // exercising the client's 'settle' handler without real firmware.
        // {"t":"dbg_settle"} — the mock doesn't track real shelter-building
        // state (see the generic 'act' handler above), so this just fakes
        // the wire message the firmware's broadcastSettle() would send.
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        destroyedShelters.add(`${p.q}_${p.r}`);
        terrainOverrides.set(`${p.q}_${p.r}`, TERRAIN_SETTLEMENT);
        broadcast({ t: 'ev', k: 'settle', removed: [{ q: p.q, r: p.r }], q: p.q, r: p.r });
        broadcast(stateMsg());
        console.log(`[settle] test-forced at (${p.q},${p.r})`);
        break;
      }

      case 'dbg_caravan': {
        // Test-only: teleport the caravan onto the sender's hex so the trade
        // panel can be exercised without waiting for it to path over.
        // {"t":"dbg_caravan"}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        caravan.q = p.q; caravan.r = p.r;
        broadcast(stateMsg());
        console.log(`[caravan] forced to (${p.q},${p.r}) by pid=${id}`);
        break;
      }

      case 'dbg_ignite': {
        // Test-only: ignite the sender's own hex at intensity 2, ignoring
        // weather/flammability's usual gate (still no-ops if the terrain is
        // immune — see isFlammable()). {"t":"dbg_ignite"}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        igniteHex(p.q, p.r, 2);
        broadcast(stateMsg());
        console.log(`[fire] dbg_ignite at (${p.q},${p.r}) by pid=${id}`);
        break;
      }

      // Test-only: put the sender at bunker hatch #n (default 0) and refill MP,
      // so the descend/ascend flow can be exercised without walking the map.
      //   {"t":"dbg_tunnel","h":3}              surface, standing on the hatch
      //   {"t":"dbg_tunnel","h":3,"below":1}    underground, on that shaft
      //   {"t":"dbg_tunnel","h":3,"below":1,"mp":0}   ...with a pinned MP budget
      // The "below" form is what proves the shortcut: descend at one hatch,
      // jump to another shaft, ascend, and you surface somewhere else entirely.
      case 'dbg_tunnel': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const h = Math.max(0, Math.min(bunkerHatches.length - 1, msg.h | 0));
        // "mp" lets a test pin the budget (0 proves ascend is never MP-gated).
        p.mp = (msg.mp === undefined) ? 6 : Math.max(0, msg.mp | 0);
        p.hatchIdx = h;
        if (msg.below) {
          p.dp = 1;
          p.tq = bunkerHatches[h].tq;
          p.tr = bunkerHatches[h].tr;
          p.q  = bunkerHatches[h].sq;
          p.r  = bunkerHatches[h].sr;
          sendTunnelSync(ws, p);
          const tvr = tunnelVis(p);
          send(ws, { t: 'vis', dp: 1, q: p.tq, r: p.tr, vr: tvr,
                     cells: buildTunnelVisDisk(p.tq, p.tr, tvr) });
        } else {
          p.dp = 0;
          p.q = bunkerHatches[h].sq;
          p.r = bunkerHatches[h].sr;
          send(ws, { t: 'vis', q: p.q, r: p.r, vr: surfaceVis(p), cells: buildVisDisk(p.q, p.r, surfaceVis(p)) });
        }
        broadcast(stateMsg());
        console.log(`[dbg_tunnel] pid=${id} -> hatch #${h} ` +
          (msg.below ? `shaft (${p.tq},${p.tr})` : `surface (${p.q},${p.r})`));
        break;
      }

      // Test-only: collapse the tunnel hex in front of the sender (direction d,
      // default 0), so the blocked-route behaviour can be exercised.
      // {"t":"dbg_collapse"} or {"t":"dbg_collapse","d":2}
      case 'dbg_collapse': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p || !p.dp) break;
        const [dq, dr] = DIR_DELTA[msg.d | 0] || [0, 0];
        const nq = p.tq + dq, nr = p.tr + dr;
        if (!tunIn(nq, nr)) break;
        tunnel[nr][nq].tt = 15;
        broadcast({ t: 'ev', k: 'tun_collapse', q: nq, r: nr, amt: 15 });
        const tvr = tunnelVis(p);
        send(ws, { t: 'vis', dp: 1, q: p.tq, r: p.tr, vr: tvr,
                   cells: buildTunnelVisDisk(p.tq, p.tr, tvr) });
        broadcast(stateMsg());
        console.log(`[dbg_collapse] (${nq},${nr}) -> rock`);
        break;
      }

      case 'dbg_flood': {
        // Test-only: force-flood the sender's own hex at intensity 2,
        // ignoring the water-terrain/storm gates (still respects FLOOD_CAP).
        // {"t":"dbg_flood"}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const key = `${p.q}_${p.r}`;
        if (!floodGrid[key]) {
          if (floodCount >= FLOOD_CAP) break;
          floodCount++;
        }
        floodGrid[key] = Math.max(floodGrid[key] || 0, 2);
        broadcast(stateMsg());
        console.log(`[flood] dbg_flood at (${p.q},${p.r}) by pid=${id}`);
        break;
      }

      case 'dbg_doom': {
        // Test-only: teleport Doom adjacent to the sender and set its
        // awareness (default 100 — the most dramatic tier).
        // {"t":"dbg_doom","awareness":100}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const [dq, dr] = DIR_DELTA[0]; // SE neighbor — "adjacent", not on top
        doom.q = ((p.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
        doom.r = ((p.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
        doom.awareness = Math.max(0, Math.min(100, (msg.awareness ?? 100) | 0));
        broadcast(stateMsg());
        console.log(`[doom] forced to (${doom.q},${doom.r}) awareness=${doom.awareness} by pid=${id}`);
        break;
      }

      case 'car_trade': {
        // Dedicated one-shot handler — no offer/accept round-trip, mirrors
        // handleMsg_caravan_trade in network-msg-trade.hpp.
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const give = (Array.isArray(msg.give) ? msg.give : [0, 0, 0, 0, 0]).map(v => Math.max(0, Math.min(99, v | 0)));
        const want = (Array.isArray(msg.want) ? msg.want : [0, 0, 0, 0, 0]).map(v => Math.max(0, Math.min(99, v | 0)));
        if (give.every(v => v === 0) && want.every(v => v === 0)) break;
        const coLocated = p.q === caravan.q && p.r === caravan.r;
        if (!p.enc && caravan.active && coLocated && hasResources(p, give) && hasResources(caravan, want)) {
          for (let i = 0; i < 5; i++) {
            p.inv[i]       = Math.min(99, (p.inv[i] || 0) - give[i] + want[i]);
            caravan.inv[i] = Math.min(99, (caravan.inv[i] || 0) - want[i] + give[i]);
          }
          broadcast({ t: 'ev', k: 'trd_res', from: id, to: CARAVAN_PID, res: 1 });
          broadcast(stateMsg());
          console.log(`[caravan] trade pid=${id} give=${JSON.stringify(give)} want=${JSON.stringify(want)}`);
        } else {
          send(ws, { t: 'trade_fail' });
        }
        break;
      }

      case 'car_buy': {
        // Resource tokens → a consumable off the caravan's shelf. Mirrors
        // handleMsg_caravan_buy in network-msg-trade.hpp: all-or-nothing
        // checks, then EVT_TRADE_RESULT (with item/n) for everyone plus a
        // targeted item_result so the buyer's pack updates at once.
        // {"t":"car_buy","item":ID,"n":QTY,"give":[5]}
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const itemId = msg.item | 0;
        const n      = msg.n === undefined ? 1 : (msg.n | 0);
        if (itemId <= 0 || itemId > 254 || n < 1 || n > CARAVAN_STOCK_MAX) break;
        const give = (Array.isArray(msg.give) ? msg.give : [0, 0, 0, 0, 0]).map(v => Math.max(0, Math.min(99, v | 0)));
        const paid = caravanPaymentValue(give);
        const def  = ITEM_DEFS[itemId];
        const ss   = caravanStockSlot(itemId);
        const coLocated = p.q === caravan.q && p.r === caravan.r;
        let why = 1;
        if (!p.enc && caravan.active && coLocated && def && ss >= 0 && caravan.stock[ss].qty >= n) {
          const price = caravanPrice(itemId) * n;
          if (give[0])                                         why = 4;  // water refused outright
          else if (paid < price || !hasResources(p, give))     why = 2;
          else if (invRoomFor(p.it, p.iq, effectiveInvSlots(p), itemId) < n) why = 3;
          else {
            for (let i = 0; i < 5; i++) {
              p.inv[i]       = (p.inv[i] || 0) - give[i];
              caravan.inv[i] = Math.min(99, (caravan.inv[i] || 0) + give[i]);
            }
            caravan.stock[ss].qty -= n;
            if (!caravan.stock[ss].qty) caravan.stock[ss].id = 0;
            grantItemOrDrop(p, itemId, n);
            why = 0;
          }
        }
        if (why) { send(ws, { t: 'trade_fail', why }); break; }
        broadcast({ t: 'ev', k: 'trd_res', from: id, to: CARAVAN_PID, res: 1, item: itemId, n });
        send(ws, { t: 'item_result', ok: true, act: 'buy', pid: id, item: itemId, n, it: p.it, iq: p.iq, inv: p.inv, ...packFields(p) });
        broadcast(stateMsg());
        console.log(`[caravan] buy pid=${id} item=${itemId} n=${n} give=${JSON.stringify(give)}`);
        break;
      }
    }
  });

  ws.on('close', () => {
    const id = sockets.get(ws);
    sockets.delete(ws);
    if (id >= 0 && players[id]) {
      if (encounters[id]) encEnd(id, 'disconnect');
      // Slots are reused by archetype — kill any outstanding offer this slot
      // made so a later, unrelated player can't inherit it.
      if (tradeOffers[id]) tradeOffers[id].active = false;
      delete lastCaravanHex[id];
      delete players[id];
      broadcast(stateMsg());
    }
    console.log('[WS] client closed (id=' + id + ')');
  });
});

httpServer.listen(PORT, () => {
  console.log(`Mock server listening on http://localhost:${PORT}/  (assets: ${BUNDLE_MODE ? 'BUNDLE — data/assets.json + app.bundle.js' : 'dev — per-file manifest from web-assets.json'})`);
  console.log(`WebSocket at ws://localhost:${PORT}/ws`);
});
