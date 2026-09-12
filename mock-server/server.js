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
const TREAT_DN       = 9; // mirrors TREAT_DN in Esp32HexMapCrawl.ino
// INV_SLOTS_MAX in Esp32HexMapCrawl.ino — typed-item slots are a fixed,
// index-stable array (0 = empty), not a growable list. See ITEM_DEFS note below.
const INV_SLOTS_MAX = 12;

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
    it: new Array(INV_SLOTS_MAX).fill(0), iq: new Array(INV_SLOTS_MAX).fill(0),
    eq: [0, 0, 0, 0, 0], // EQUIP_SLOTS=5 in Esp32HexMapCrawl.ino — array, not {}; 0 = empty slot
    arch: id,
    is: ARCHETYPE_INV_SLOTS[id] ?? 8,
    sk: (ARCHETYPE_SKILLS[id] ?? [0, 0, 0, 0, 0]).slice(),
    wnd: [0, 0],
    enc: false,
  };
}

// ── Item registry — mirrors enough of boot-assets.hpp's loadItemRegistry() to
// validate equip/unequip/use/drop/pickup and apply a consumable's stat deltas.
// Still a simplification: no pack-slot bonus (STAT_SLOTS) and no dawn *_cost
// gating — equipping here never changes LL ceiling, pack size, or day/night
// stats (mock has no equip stat mods, same simplification as the LL/MP math
// elsewhere in this file).
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
    else if (key === 'effect')   cur.effect  = val;
    else if (key === 'param')    cur.param   = parseInt(val, 10) || 0;
    else if (key === 'effect2')  cur.effect2 = val;
    else if (key === 'param2')   cur.param2  = parseInt(val, 10) || 0;
  }
  return defs;
}
const ITEM_DEFS = loadItemRegistry();

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

// Real-time floor: a day normally takes DAY_TICKS (5 real minutes) but ends
// early the moment every connected player is resting, so back-to-back REST
// spam could otherwise collapse days to seconds and cycle weather absurdly
// fast. Mirrors the same ~2-3 real-minute floor as actions_game_loop.hpp's
// updateWeatherPhase() — not persisted, resets on server restart.
let lastWeatherChangeAt = 0;
let weatherNextGapMs    = 120000;
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
    weatherNextGapMs    = 120000 + Math.random() * 60000; // 2-3 real minutes until the next one
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
    spreadFire();
    tickCaravan();
    resolveCaravanProximity(connected);
    // Firmware's broadcastState() runs unconditionally every game tick (see
    // game-server.hpp), so caravan movement is never stale there. This mock
    // only broadcasts opportunistically (from message handlers) otherwise,
    // so without this, a client wouldn't see the caravan move until some
    // other action happened to trigger a broadcast.
    broadcast(stateMsg());
  }
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
  for (const { it, iq } of e.pendingItems) grantItemOrDrop(p, it, iq);
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
const WORLD_TICK_INTERVAL   = 150;  // game ticks between world updates (~15s) — matches firmware
let worldTickCounter = 0;
const caravan = {
  q: Math.floor(Math.random() * MAP_COLS),
  r: Math.floor(Math.random() * MAP_ROWS),
  wq: 0, wr: 0,
  active: true,
  inv: [0, 0, 0, 0, 0],
  restockTimer: 0,
};
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
const LIGHTNING_IGNITE_CHANCE = 15;  // % per world tick while a storm (phase 2) is active
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
  const q = ((ref.q + Math.floor(Math.random() * 9) - 4) % MAP_COLS + MAP_COLS) % MAP_COLS;
  const r = ((ref.r + Math.floor(Math.random() * 9) - 4) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
  igniteHex(q, r, 2);  // strikes in hot, not just an ember; no-ops on immune terrain

  // Direct strike — anyone standing exactly on the struck hex takes damage
  // regardless of whether the terrain was flammable.
  for (const p of connected) {
    if (p.q !== q || p.r !== r) continue;
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
    p.ll = Math.max(0, p.ll - 1);
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

// ── World system: Creeping Doom — simplified, not behaviour-parity: no
// track/scent modeling here (nothing needs the AI to be faithful, just the
// client-visible position/awareness/effects to exercise the aura render and
// doom_warn/doom_act events). Homes in on the nearest connected player once
// within detection radius instead of chasing scent; otherwise wanders and
// cools off. Same awareness-tier effects as world-system.hpp's
// resolveDoomProximity().
const AWARENESS_GAIN  = 12;
const AWARENESS_DECAY = 5;
const doom = {
  q: Math.floor(Math.random() * MAP_COLS),
  r: Math.floor(Math.random() * MAP_ROWS),
  awareness: 0,
};

function doomDetectionRadius() {
  return 2 + Math.floor(doom.awareness / 25);
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

function tickCreepingDoom(connected) {
  if (doom.awareness >= 100) {
    const nearest = nearestConnectedPlayer(connected);
    if (nearest) {
      stepDoomToward(nearest.p.q, nearest.p.r, true);
      igniteHex(doom.q, doom.r, 2);
      return;
    }
    // no player connected — fall through and let awareness decay
  }

  const radius  = doomDetectionRadius();
  const nearest = nearestConnectedPlayer(connected);
  if (nearest && nearest.dist <= radius) {
    stepDoomToward(nearest.p.q, nearest.p.r, false);
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

// Prefers open ground away from fire and Creeping Doom — same scoring as
// pickCaravanWaypoint() in world-system.hpp (8-sample search).
function pickCaravanWaypoint() {
  let bestQ = null, bestR = null, bestScore = -1;
  for (let i = 0; i < 8; i++) {
    const q = Math.floor(Math.random() * MAP_COLS);
    const r = Math.floor(Math.random() * MAP_ROWS);
    let score = (fireGrid[`${q}_${r}`] || 0) * 10;
    if (hexDistWrap(q, r, doom.q, doom.r) < 3) score += 50;
    if (bestQ === null || score < bestScore) { bestScore = score; bestQ = q; bestR = r; }
  }
  caravan.wq = bestQ;
  caravan.wr = bestR;
}
pickCaravanWaypoint();

function restockCaravan() {
  caravan.restockTimer = CARAVAN_RESTOCK_TICKS;
  for (let i = 0; i < 5; i++) caravan.inv[i] = Math.min(99, caravan.inv[i] + 4 + Math.floor(Math.random() * 5));
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
  return true;
}

function tickCaravan() {
  if (!caravan.active) return;
  if (caravan.q !== caravan.wq || caravan.r !== caravan.wr) {
    if (moveCaravanOneStep()) clearCaravanDebounce();
  } else {
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
    caravan: { q: caravan.q, r: caravan.r, active: caravan.active, inv: caravan.inv },
    doom: { q: doom.q, r: doom.r, awareness: doom.awareness },
    fire: fireArray(),
  };
}

function syncMsg(id) {
  return {
    t: 'sync',
    id,
    vr: 4,
    map: liveMapHex(),
    p: Object.values(players),
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
    p[i] = players[i] || { on: false, q: 0, r: 0, sc: 0, inv: [0,0,0,0,0], sp: 0,
                           ll: 0, food: 0, water: 0, rad: 0,
                           mp: 0, fth: 0, wth: 0, vm: 0, rt: 0, wnd: [0, 0],
                           it: [], iq: [], eq: [0, 0, 0, 0, 0], enc: false };
  }
  return { t: 's', p, gs: { wp: weatherPhase, dc: dayCount, tc: threatClock }, world: worldStateMsg() };
}

function lobbyMsg() {
  const taken = new Set(Object.keys(players).map(Number));
  const avail = [];
  for (let i = 0; i < MAX_PLAYERS; i++) if (!taken.has(i)) avail.push(i);
  return { t: 'lobby', avail };
}

// ── Item actions — mirrors equipItem()/unequipItem()/useItem()/dropItem()/
// pickupGroundItem() in inventory_items.hpp. it[]/iq[] are fixed INV_SLOTS_MAX
// arrays, index-stable like the firmware's invType[]/invQty[] (0 = empty) —
// equip/unequip/use/drop clear or fill a slot in place, they never shift
// other slots around.

// First empty typed-item slot within this player's pack (mock has no equip
// slot bonuses, so the cap is just p.is — see ITEM_DEFS note above).
function firstFreeInvSlot(p) {
  for (let i = 0; i < p.is; i++) if (!p.it[i]) return i;
  return -1;
}

// Place itemId x qty into p's pack (stack first, then a free slot); overflow
// goes to the ground at the player's hex. Mirrors grantItemOrDrop() in
// network-msg-encounter.hpp.
function grantItemOrDrop(p, itemId, qty) {
  if (!itemId || !qty) return;
  const cap = ITEM_DEFS[itemId]?.maxStack || 1;
  for (let s = 0; s < p.is && qty > 0; s++) {
    if (p.it[s] !== itemId || p.iq[s] >= cap) continue;
    const add = Math.min(qty, cap - p.iq[s]);
    p.iq[s] += add; qty -= add;
  }
  for (let s = 0; s < p.is && qty > 0; s++) {
    if (p.it[s]) continue;
    const add = Math.min(qty, cap);
    p.it[s] = itemId; p.iq[s] = add; qty -= add;
  }
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
  if (state.llDelta) p.ll = Math.max(0, Math.min(LL_CAP, p.ll + state.llDelta));
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

// Pick up all of ground item gslot; player must be standing on that hex.
// Mirrors pickupGroundItem() in inventory_items.hpp.
function pickupGroundItem(p, gslot) {
  if (gslot < 0 || gslot >= MAX_GROUND) return false;
  const gi = groundItems[gslot];
  if (!gi.itemType || gi.q !== p.q || gi.r !== p.r) return false;

  const itemId = gi.itemType;
  let freeSlot = -1, stackSlot = -1;
  for (let i = 0; i < p.is; i++) {
    if (p.it[i] === itemId && stackSlot < 0) stackSlot = i;
    if (!p.it[i] && freeSlot < 0) freeSlot = i;
  }
  const targetSlot = stackSlot >= 0 ? stackSlot : freeSlot;
  if (targetSlot < 0) return false;
  const maxStack = ITEM_DEFS[itemId]?.maxStack || 1;

  let canTake = gi.qty;
  if (stackSlot >= 0) canTake = Math.min(canTake, Math.max(0, maxStack - p.iq[stackSlot]));
  if (!canTake) return false;

  p.it[targetSlot] = itemId;
  p.iq[targetSlot] = Math.min(maxStack, p.iq[targetSlot] + canTake);
  gi.qty -= canTake;
  if (!gi.qty) { gi.itemType = 0; gi.q = 0; gi.r = 0; }
  return true;
}

function equipItem(ws, id, msg) {
  const p = players[id];
  if (!p) return;
  const slotIdx = msg.slot | 0;
  const itemId  = p.it[slotIdx];
  const def     = itemId ? ITEM_DEFS[itemId] : null;
  let ok = !!def && def.category === 'equipment' && def.eslot >= 0;
  let freeSlot = -1;
  if (ok) {
    const prev = p.eq[def.eslot];
    if (prev) {
      freeSlot = firstFreeInvSlot(p);
      if (freeSlot < 0) ok = false; // no room to hold the item being swapped out
    }
    if (ok) {
      if (prev) { p.it[freeSlot] = prev; p.iq[freeSlot] = 1; }
      p.eq[def.eslot] = itemId;
      p.it[slotIdx] = 0;
      p.iq[slotIdx] = 0;
    }
  }
  send(ws, { t: 'item_result', ok, act: 'equip', slot: slotIdx, pid: id, it: p.it, iq: p.iq, eq: p.eq });
  if (ok) broadcast(stateMsg());
}

function unequipItem(ws, id, msg) {
  const p = players[id];
  if (!p) return;
  const eslot  = msg.eslot | 0;
  const itemId = p.eq[eslot];
  let ok = !!itemId;
  if (ok) {
    const freeSlot = firstFreeInvSlot(p);
    ok = freeSlot >= 0;
    if (ok) { p.it[freeSlot] = itemId; p.iq[freeSlot] = 1; p.eq[eslot] = 0; }
  }
  send(ws, { t: 'item_result', ok, act: 'unequip', eslot, pid: id, it: p.it, iq: p.iq, eq: p.eq });
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
        delete lastCaravanHex[id];  // moved — re-arm the caravan trade prompt
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
      case 'enc_bank':   encBank(ws, sockets.get(ws));        break;
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
        send(ws, { t: 'item_result', ok, act: 'use', slot: slotIdx, pid: id, it: p.it, iq: p.iq, eq: p.eq, efxp: narParam });
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
        send(ws, { t: 'item_result', ok, act: 'drop', slot: slotIdx, pid: id, it: p.it, iq: p.iq, eq: p.eq });
        if (ok) { broadcast(groundUpdateMsg(p.q, p.r)); broadcast(stateMsg()); }
        break;
      }

      case 'pickup_item': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const gslot = msg.gslot | 0;
        const ok = pickupGroundItem(p, gslot);
        send(ws, { t: 'item_result', ok, act: 'pickup', gslot, pid: id, it: p.it, iq: p.iq, eq: p.eq });
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
