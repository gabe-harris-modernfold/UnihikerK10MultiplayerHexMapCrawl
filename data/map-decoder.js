// ── Map decode ──────────────────────────────────────────────────
// Shared by both boards — the 75x57 surface map and the small bunker tunnel
// board (tunnel-board.js). The wire format is identical; only the grid it is
// written into and its dimensions differ.
//
// 3 bytes per cell (6 hex chars) — mirrors encodeCell() in hex-map.hpp:
//   TT = terrain byte (0x00-0x0B) or 0xFF (fog); bit 6 (0x40) = improved
//        shelter, bit 7 (0x80) = caravan has driven through (tire track)
//   DD = bits 0-5: footprint bitmask, bit 6: has shelter (any), bit 7: has POI
//   VV = high nibble: resource type (0-5), low nibble: terrain variant (0-15)
function decodeCell(terrainByte, dataByte, variantByte = 0) {
  if (terrainByte === 0xFF) return null;
  const hasShelter = (dataByte >> 6) & 1;
  const cell = {
    // 0x3F, not 0x0F: terrain now runs 0-15 (the four bunker tunnel types
    // took NUM_TERRAIN to 16) and bits 6/7 are the shelter/track flags.
    terrain:    terrainByte & 0x3F,
    footprints: dataByte & 0x3F,           // bits 0-5: which players visited (bitmask)
    shelter:    hasShelter ? ((terrainByte & 0x40) ? 2 : 1) : 0,  // 0 none, 1 basic, 2 improved
    poi:        (dataByte >> 7) & 1,       // bit 7: 0=none, 1=has POI encounter
    tireTrack:  (terrainByte >> 7) & 1,    // bit 7 of TT: caravan or a ridden vehicle (e.g. Motorbike) has been here
    resource:   (variantByte >> 4) & 0xF, // high nibble: resource type (0=none, 1-5)
    variant:    variantByte & 0xF,        // low nibble: terrain image variant (0-15)
  };
  return cell;
}

// grid/cols/rows default to the surface map; the tsync handler passes the
// tunnel board instead.
function parseMapFog(hexStr, grid = gameMap, cols = MAP_COLS, rows = MAP_ROWS, label = 'MAP') {
  let revealed = 0, fog = 0, poiCount = 0, shelterCount = 0, resourceCount = 0;
  // Surface only — the tunnel board is transient and gets no memory.
  const surface = (grid === gameMap);
  const samples = [];
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const idx = (r * cols + c) * 6;
      const tt  = Number.parseInt(hexStr.substr(idx,     2), 16);
      const dd  = Number.parseInt(hexStr.substr(idx + 2, 2), 16);
      const vv  = Number.parseInt(hexStr.substr(idx + 4, 2), 16);
      grid[r][c] = decodeCell(tt, dd, vv);
      if (tt === 0xFF) { fog++; }
      else {
        revealed++;
        if (surface) samples.push({ key: `${c}_${r}`, cell: grid[r][c] });
        if ((dd >> 7) & 1) poiCount++;
        if ((dd >> 6) & 1) shelterCount++;
        if ((vv >> 4) & 0xF) resourceCount++;
      }
    }
  }
  console.log(`%c[${label}] parseMapFog`, 'color:#09f',
    `hexLen=${hexStr.length} total=${rows * cols} revealed=${revealed} fog=${fog}` +
    ` | poi=${poiCount} shelter=${shelterCount} resource=${resourceCount}`);
  // A sync replaces the whole grid with fog outside your current disk, which
  // is exactly the moment the remembered map has to take over — but it is
  // also the only ground truth we ever get, so check it before trusting it.
  if (surface) {
    if (!memoryAgreesWith(samples)) {
      console.log(`%c[MAP] explored memory dropped — ${memoryCells.size} remembered cells disagree with a fresh sync (board regenerated?)`, 'color:#f60;font-weight:bold');
      memoryCells.clear();
    }
    samples.forEach(s2 => rememberCell(s2.key, s2.cell));
    scheduleMemorySave();
  }
}

// ── Apply vis-disk update ────────────────────────────────────────
// Format: "QQRRTTDDVV..." — 10 hex chars per cell (5 bytes)
//   QQ=col, RR=row, TT=terrain, DD=data, VV=variant
// depth 0 writes the surface map, 1 the bunker tunnel board. The collected-
// resource guard and the amount carry-forward below are surface-only: they
// exist to reconcile a `col` event racing a visdisk, and the tunnel board has
// no such history to preserve on a fresh descend.
function applyVisDisk(cells, depth = 0) {
  const grid = depth ? tunnelMap  : gameMap;
  const cols = depth ? tunnelCols : MAP_COLS;
  const rows = depth ? tunnelRows : MAP_ROWS;
  const cellCount = cells.length / 10;
  const notable   = [];   // cells with POI / shelter / resource — key for encounter tracing

  for (let i = 0; i < cells.length; i += 10) {
    const q  = Number.parseInt(cells.substr(i,     2), 16);
    const r  = Number.parseInt(cells.substr(i + 2, 2), 16);
    const tt = Number.parseInt(cells.substr(i + 4, 2), 16);
    const dd = Number.parseInt(cells.substr(i + 6, 2), 16);
    const vv = Number.parseInt(cells.substr(i + 8, 2), 16);
    if (r < rows && q < cols) {
      const cell = decodeCell(tt, dd, vv);
      const key  = `${q}_${r}`;
      if (!depth && collectedCells.has(key)) {
        if (cell && cell.resource > 0) {
          // Server says resource is back (respawned, or original col event was
          // for a different state). Trust the fresh visdisk and drop the
          // stale local guard so the icon can render.
          collectedCells.delete(key);
          console.log('[MAP] collectedCells cleared by fresh visdisk at', q, r);
        } else if (cell) {
          // Preserve locally-cleared resource — col event may arrive before vis
          cell.resource = 0;
          cell.amount   = 0;
        }
      }
      // Visdisk doesn't carry `amount` — preserve it from the previous cell when
      // the resource type is unchanged. Without this, partial-pickup hexes lose
      // their amount field on the next visdisk and the HUD shows "×undefined".
      const prev = grid[r][q];
      if (cell && prev && cell.resource > 0 && cell.resource === prev.resource && prev.amount != null) {
        cell.amount = prev.amount;
      }
      grid[r][q] = cell;
      if (!depth && cell) rememberCell(key, cell);
      // Collect notable decoded values for logging
      if (cell) {
        const flags = [];
        if (cell.poi)                             flags.push('POI');
        if (cell.shelter)                         flags.push(`shelter=${cell.shelter}`);
        if (cell.resource)                        flags.push(`res=${cell.resource}`);
        if (cell.footprints)                      flags.push(`fp=0x${cell.footprints.toString(16)}`);
        if (cell.tireTrack)                        flags.push('tireTrack');
        if (cell.variant)                         flags.push(`var=${cell.variant}`);
        if (flags.length) notable.push(`(q${q},r${r}) T=${tt.toString(16).padStart(2,'0')} [${flags.join(' ')}]`);
      }
    }
  }

  if (!depth) scheduleMemorySave();

  if (notable.length > 0) {
    console.log(`%c[${depth ? 'TUN' : 'MAP'}] applyVisDisk`, 'color:#09f;font-weight:bold',
      `${cellCount} cells decoded — notable:`);
    notable.forEach(n => console.log('  ', n));
  } else {
    console.log(`%c[${depth ? 'TUN' : 'MAP'}] applyVisDisk`, 'color:#09f', `${cellCount} cells decoded — all clear`);
  }
}

// ── Explored-terrain memory ─────────────────────────────────
// A vis disk is the only thing that ever writes gameMap, and nothing nulls a
// cell when you walk away — so the client has always held an accurate map of
// everywhere it has been. It simply refused to draw it: renderHexTerrain
// gated terrain art on `dist <= visR`, so one step too far turned known
// ground back into blank fog. This keeps that knowledge deliberately, so it
// survives the two things that did erase it: a `sync` (which replaces the
// whole grid with 0xFF outside your current disk) and a page reload.
//
// Terrain is memory; resources are sight. Only the fields that stay true
// while you are not looking are kept — terrain, art variant, shelter, POI.
// Resource type and amount, footprints and tire tracks are live intel that
// depletes, respawns or decays server-side, and remembering them would not be
// generous, it would be wrong.
const MEMORY_STORE_KEY     = 'wl.explored.v1';
const MEMORY_SAVE_DELAY_MS = 1500;  // debounce: a move can rewrite ~40 cells
const MEMORY_MIN_SAMPLES   = 8;     // below this a sync says nothing useful
const MEMORY_MISMATCH_TOL  = 0.25;  // see memoryAgreesWith()
const memoryCells = new Map();      // "q_r" -> {terrain, variant, shelter, poi}

function rememberCell(key, cell) {
  if (!cell) return;
  memoryCells.set(key, {
    terrain: cell.terrain,
    variant: cell.variant,
    shelter: cell.shelter,
    poi:     cell.poi,
  });
}

// Terrain legitimately mutates under you: a flash flood runs dry -> Marsh ->
// Flooded District, fire burns a hex down to Ash Dunes, a quake converts.
// Those are a handful of cells at a time. A regenerated board (reset_board.ps1,
// or the bots harness) reuses the same coordinates with entirely different
// terrain, and there is no world id anywhere in the protocol to spot that
// with. So spot it empirically — every sync hands over a disk of ground
// truth, and if a quarter of it disagrees with what we remember, this is not
// the world we mapped. Incidental conversions stay far under that bar.
function memoryAgreesWith(samples) {
  let checked = 0, mismatched = 0;
  for (const s of samples) {
    const m = memoryCells.get(s.key);
    if (!m) continue;
    checked++;
    if (m.terrain !== s.cell.terrain) mismatched++;
  }
  if (checked < MEMORY_MIN_SAMPLES) return true;
  return (mismatched / checked) <= MEMORY_MISMATCH_TOL;
}

// Dense board-shaped hex string, same 3-bytes-per-cell layout as the wire
// format so decodeCell() can read it straight back. 0xFF = never seen.
// 75x57x6 = 25650 chars, well inside any localStorage budget, and far cheaper
// than JSON over a Map with a few thousand entries.
const _mhex2 = (n) => (n & 0xFF).toString(16).padStart(2, '0').toUpperCase();

function serializeMemory() {
  let out = '';
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let q = 0; q < MAP_COLS; q++) {
      const m = memoryCells.get(`${q}_${r}`);
      if (!m) { out += 'FF0000'; continue; }
      out += _mhex2(m.terrain | (m.shelter >= 2 ? 0x40 : 0)) +
             _mhex2(((m.shelter ? 1 : 0) << 6) | (m.poi ? 0x80 : 0)) +
             _mhex2(m.variant & 0x0F);
    }
  }
  return out;
}

function loadExploredMemory() {
  let raw = null;
  // Private windows, cleared site data and blocked storage all throw here
  // rather than returning null — a lost map is a cosmetic regression, never
  // a reason to take the client down with it.
  try { raw = localStorage.getItem(MEMORY_STORE_KEY); } catch { return; }
  if (!raw || raw.length !== MAP_ROWS * MAP_COLS * 6) return;
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let q = 0; q < MAP_COLS; q++) {
      const i  = (r * MAP_COLS + q) * 6;
      const tt = Number.parseInt(raw.substr(i, 2), 16);
      if (tt === 0xFF) continue;
      const cell = decodeCell(tt, Number.parseInt(raw.substr(i + 2, 2), 16),
                                  Number.parseInt(raw.substr(i + 4, 2), 16));
      if (cell) rememberCell(`${q}_${r}`, cell);
    }
  }
  console.log('%c[MAP] explored memory restored', 'color:#09f', `${memoryCells.size} cells`);
}

let _memorySaveTimer = null;
function scheduleMemorySave() {
  if (_memorySaveTimer) return;
  _memorySaveTimer = setTimeout(() => {
    _memorySaveTimer = null;
    try { localStorage.setItem(MEMORY_STORE_KEY, serializeMemory()); }
    catch (e) { console.warn('[MAP] explored memory not saved:', e?.name || e); }
  }, MEMORY_SAVE_DELAY_MS);
}

loadExploredMemory();
