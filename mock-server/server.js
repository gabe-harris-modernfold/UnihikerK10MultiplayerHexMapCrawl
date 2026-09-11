// Mock Wasteland Crawl server — serves data/ statically and fakes /ws so the
// client can connect, pick a character, see a map, and move.

const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const DATA_DIR = path.resolve(__dirname, '..', 'data');
const PORT     = process.env.PORT || 8765;

const MAP_COLS = 75, MAP_ROWS = 57, MAX_PLAYERS = 6;

const hex2 = (n) => n.toString(16).padStart(2, '0');

// ── Derive image variant counts from data/img — mirrors setupVariantCounts()
// in game-server.hpp so the client loads the same hex<Name><N>.png set the
// firmware would find on the SD card.
const TERRAIN_IMG_NAMES = [
  'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
  'BrokenUrban', 'FloodedDistrict', 'GlassFields',
  'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
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
function ddFor(c, r, ddNum) {
  let out = ddNum;
  if (consumedPoi.has(`${c}_${r}`))       out &= ~0x80;
  if (destroyedShelters.has(`${c}_${r}`)) out &= ~0x40;
  return out;
}
function ttFor(c, r, ttNum) {
  const ov = terrainOverrides.get(`${c}_${r}`);
  return ov === undefined ? ttNum : ov;
}

// Live snapshot of current map state — encodes cells with up-to-date resource
// info so reconnects/syncs reflect collection truth.
function liveMapHex() {
  let s = '';
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
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

function makePlayer(id) {
  return {
    id, on: true,
    q: 12, r: 9,
    sc: 0,
    inv: [0, 0, 0, 0, 0],
    sp: 0,
    ll: 7, food: 6, water: 6, rad: 0,
    mp: 6,
    fth: 0, wth: 0,
    vm: 0x3F,
    rt: 0,
    it: [], iq: [],
    eq: [0, 0, 0, 0, 0], // EQUIP_SLOTS=5 in Esp32HexMapCrawl.ino — array, not {}; 0 = empty slot
    arch: id,
    is: ARCHETYPE_INV_SLOTS[id] ?? 8,
    sk: (ARCHETYPE_SKILLS[id] ?? [0, 0, 0, 0, 0]).slice(),
    wnd: [0, 0],
    enc: false,
  };
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
    loot: [0, 0, 0, 0, 0], items: [], lootTable: '',
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
      else if (e.item && q && out.items.length < 2) out.items.push({ it: e.item | 0, iq: q });
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
                     pendingLoot: [0, 0, 0, 0, 0], pendingItems: [],
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
  let dn = 2 + Math.floor((risk * 10) / 100);
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
  } else {
    if (p.food > 1) p.food--;
    if (oldF >= 4 && p.food < 4 && !(p.fth & 1)) { p.fth |= 1; state.llDelta--; }
    if (oldF >= 2 && p.food < 2 && !(p.fth & 2)) { p.fth |= 2; state.llDelta--; }
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
    const inSettlement = terrainAt(p.q, p.r) === 9;

    if (!inSettlement) {
      if (p.inv[1] > 0) { p.inv[1]--; applyFStep(p, +1, state); }
      else                applyFStep(p, -1, state);

      const use = Math.min(p.inv[0], 2);
      p.inv[0] -= use;
      for (let i = 0; i < use;     i++) applyWStep(p, +1, state);
      for (let i = 0; i < 2 - use; i++) applyWStep(p, -1, state);
    }

    let expDelta = 0;
    const sv       = TERRAIN_SV[terrainAt(p.q, p.r)] ?? 0;
    const covered  = hasShelter(p.q, p.r) || sv >= 2;
    if (!covered) { state.llDelta--; expDelta = -1; }

    // Shelter protection: resting under a shelter suppresses all LL loss this dawn.
    if (p.rt && hasShelter(p.q, p.r) && state.llDelta < 0) {
      state.llDelta = 0;
      expDelta = 0;
    }

    // Rest recovery: resting with food>=4 and water>=3 → +1 LL and a minor wound heals.
    const restedWell = !!p.rt && p.food >= 4 && p.water >= 3;
    if (restedWell && p.ll < LL_CAP) state.llDelta++;
    if (restedWell) healWound(p, 0);

    const prevLL = p.ll;
    if (state.llDelta < 0) {
      for (let i = 0; i < -state.llDelta && p.ll > 0; i++) p.ll--;
    } else if (state.llDelta > 0) {
      p.ll = Math.min(p.ll + state.llDelta, LL_CAP);
    }
    const actualDelta = p.ll - prevLL;

    p.mp = (p.ll === 0) ? 0 : Math.max(2, p.ll + 3 - p.wnd[1]);
    p.rt = 0;

    broadcast({
      t: 'ev', k: 'dawn', pid: id, day: dayCount,
      f: p.food, w: p.water, ll: p.ll, mp: p.mp, dll: actualDelta,
      fth: p.fth, wth: p.wth, rad: p.rad, expd: expDelta, wnd: p.wnd.slice(),
    });
    console.log(`[dawn] day=${dayCount} pid=${id} f=${p.food} w=${p.water} ll=${p.ll} dll=${actualDelta}`);
  }
  broadcast(stateMsg());
}

function advanceWeather() {
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
const QUAKE_MIN_GAP_MS = 45000;
const QUAKE_TRIGGER_CHANCE = 0.02; // rolled once per game tick (100ms) once the gap has elapsed
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
}, TICK_MS);

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
                  drains: [0, 0, 0, 0, 0, 0] };
  let ended = false;
  if (ok) {
    for (let i = 0; i < 5; i++) {
      e.pendingLoot[i] = Math.min(99, e.pendingLoot[i] + ch.loot[i]);
      ev.loot[i] = ch.loot[i];
    }
    const items = [...ch.items];
    const rolled = ch.lootTable ? rollLootTable(ch.lootTable) : null;
    if (rolled) items.push(rolled);
    items.slice(0, 3).forEach((it, k) => {
      e.pendingItems.push(it);
      if (k === 0)      { ev.it  = it.it; ev.iq  = it.iq; }
      else if (k === 1) { ev.it2 = it.it; ev.iq2 = it.iq; }
    });
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

function encBank(ws, id) {
  const p = players[id], e = encounters[id];
  if (!p || !e) return;
  if (!e.fullClear && !e.canBank) { send(ws, { t: 'err', msg: "You can't carry loot out from here" }); return; }
  let total = 0;
  for (let i = 0; i < 5; i++) { p.inv[i] = Math.min(99, p.inv[i] + e.pendingLoot[i]); total += e.pendingLoot[i]; }
  for (const { it, iq } of e.pendingItems) { p.it.push(it); p.iq.push(iq); }
  const scoreD = total * 3 + (e.fullClear ? 10 : 0);
  p.sc += scoreD;
  broadcast({ t: 'ev', k: 'enc_bank', pid: id, q: e.q, r: e.r, loot: e.pendingLoot, scoreD });
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

// ── Direction deltas (approximate — "mostly works") ─────────────────────────
// Buttons: 0=SE 1=NE 2=N 3=NW 4=SW 5=S
const DIR_DELTA = {
  0: [+1, +1],  // SE
  1: [+1, -1],  // NE
  2: [ 0, -1],  // N
  3: [-1, -1],  // NW
  4: [-1, +1],  // SW
  5: [ 0, +1],  // S
};

const send = (ws, obj) => {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
};

const broadcast = (obj) => {
  for (const ws of sockets.keys()) send(ws, obj);
};

function syncMsg(id) {
  return {
    t: 'sync',
    id,
    vr: 4,
    map: liveMapHex(),
    p: Object.values(players),
    gs: { wp: weatherPhase, dc: dayCount, tc: threatClock },
    gi: [],
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
    send(ws, { t: 'ev', k: 'col_fail', pid: p.id, q: p.q, r: p.r, res: cell.res, reason: 2 });
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

function stateMsg() {
  // Build dense p[] indexed 0..MAX_PLAYERS-1; missing slots = offline stub.
  const p = [];
  for (let i = 0; i < MAX_PLAYERS; i++) {
    p[i] = players[i] || { on: false, q: 0, r: 0, sc: 0, inv: [0,0,0,0,0], sp: 0,
                           ll: 0, food: 0, water: 0, rad: 0,
                           mp: 0, fth: 0, wth: 0, vm: 0, rt: 0, wnd: [0, 0],
                           it: [], iq: [], eq: [0, 0, 0, 0, 0], enc: false };
  }
  return { t: 's', p, gs: { wp: weatherPhase, dc: dayCount, tc: threatClock } };
}

function lobbyMsg() {
  const taken = new Set(Object.keys(players).map(Number));
  const avail = [];
  for (let i = 0; i < MAX_PLAYERS; i++) if (!taken.has(i)) avail.push(i);
  return { t: 'lobby', avail };
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

const httpServer = http.createServer((req, res) => {
  if (req.method === 'POST' && req.url.split('?')[0] === '/upload') {
    handleUpload(req, res);
    return;
  }
  if (req.method === 'GET' && req.url.split('?')[0] === '/enc') {
    handleEnc(req, res);
    return;
  }
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath === '/' || urlPath === '') urlPath = '/index.html';
  const filePath = path.join(DATA_DIR, urlPath);
  if (!filePath.startsWith(DATA_DIR)) { res.writeHead(403); res.end(); return; }
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404); res.end('Not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(filePath).toLowerCase()] || 'application/octet-stream',
                         'Cache-Control': 'no-store' });  // dev loop: always pick up edits
    res.end(data);
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

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }
    console.log('[RX]', msg.t, msg);

    switch (msg.t) {
      case 'wifi':
        send(ws, { t: 'wifi', status: 'saved', ssid: msg.ssid || '', pass: msg.pass || '' });
        break;

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
        p.q = ((p.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
        p.r = ((p.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
        p.sp += 1;
        if (p.mp > 0) p.mp -= 1;
        // EVT_MOVE first so client position updates before col event arrives.
        broadcast({ t: 'ev', k: 'mv', pid: id, q: p.q, r: p.r, radd: 0, rad: p.rad, exploD: 0, mp: p.mp });
        tryCollect(p, ws);
        // Visdisk reflects current resource state (post-collection).
        const cells = buildVisDisk(p.q, p.r, 4);
        send(ws, { t: 'vis', q: p.q, r: p.r, vr: 4, cells });
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
        } else {
          // Generic: drain a bit, give a resource.
          p.mp = Math.max(0, p.mp - (msg.mp || 1));
          const slot = (msg.a | 0) % 5;
          p.inv[slot] = (p.inv[slot] || 0) + 1;
          p.sc += 1;
        }
        broadcast(stateMsg());
        break;
      }

      case 'enc_start':  encStart(ws, sockets.get(ws), msg);  break;
      case 'enc_choice': encChoice(ws, sockets.get(ws), msg); break;
      case 'enc_bank':   encBank(ws, sockets.get(ws));        break;
      case 'enc_abort':  encAbort(sockets.get(ws));           break;

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
    }
  });

  ws.on('close', () => {
    const id = sockets.get(ws);
    sockets.delete(ws);
    if (id >= 0 && players[id]) {
      if (encounters[id]) encEnd(id, 'disconnect');
      delete players[id];
      broadcast(stateMsg());
    }
    console.log('[WS] client closed (id=' + id + ')');
  });
});

httpServer.listen(PORT, () => {
  console.log(`Mock server listening on http://localhost:${PORT}/`);
  console.log(`WebSocket at ws://localhost:${PORT}/ws`);
});
