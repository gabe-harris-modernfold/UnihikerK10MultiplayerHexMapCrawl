// ── Map decode ──────────────────────────────────────────────────
// 3 bytes per cell (6 hex chars):
//   TT = terrain byte (0x00-0x0B) or 0xFF (fog)
//   DD = bits 0-5: footprint bitmask, bit 6: has shelter (any), bit 7: has POI
//   VV = high nibble: resource type (0-5), low nibble: terrain variant (0-15)
function decodeCell(terrainByte, dataByte, variantByte = 0) {
  if (terrainByte === 0xFF) return null;
  const cell = {
    terrain:    terrainByte,
    footprints: dataByte & 0x3F,           // bits 0-5: which players visited (bitmask)
    shelter:    (dataByte >> 6) & 1,       // bit 6: 0=none, 1=has shelter
    poi:        (dataByte >> 7) & 1,       // bit 7: 0=none, 1=has POI encounter
    resource:   (variantByte >> 4) & 0xF, // high nibble: resource type (0=none, 1-5)
    variant:    variantByte & 0xF,        // low nibble: terrain image variant (0-15)
  };
  return cell;
}

function parseMapFog(hexStr) {
  let revealed = 0, fog = 0, poiCount = 0, shelterCount = 0, resourceCount = 0;
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      const idx = (r * MAP_COLS + c) * 6;
      const tt  = Number.parseInt(hexStr.substr(idx,     2), 16);
      const dd  = Number.parseInt(hexStr.substr(idx + 2, 2), 16);
      const vv  = Number.parseInt(hexStr.substr(idx + 4, 2), 16);
      gameMap[r][c] = decodeCell(tt, dd, vv);
      if (tt === 0xFF) { fog++; }
      else {
        revealed++;
        if ((dd >> 7) & 1) poiCount++;
        if ((dd >> 6) & 1) shelterCount++;
        if ((vv >> 4) & 0xF) resourceCount++;
      }
    }
  }
  console.log('%c[MAP] parseMapFog', 'color:#09f',
    `hexLen=${hexStr.length} total=${MAP_ROWS * MAP_COLS} revealed=${revealed} fog=${fog}` +
    ` | poi=${poiCount} shelter=${shelterCount} resource=${resourceCount}`);
}

// ── Apply vis-disk update ────────────────────────────────────────
// Format: "QQRRTTDDVV..." — 10 hex chars per cell (5 bytes)
//   QQ=col, RR=row, TT=terrain, DD=data, VV=variant
function applyVisDisk(cells) {
  const cellCount = cells.length / 10;
  const notable   = [];   // cells with POI / shelter / resource — key for encounter tracing

  for (let i = 0; i < cells.length; i += 10) {
    const q  = Number.parseInt(cells.substr(i,     2), 16);
    const r  = Number.parseInt(cells.substr(i + 2, 2), 16);
    const tt = Number.parseInt(cells.substr(i + 4, 2), 16);
    const dd = Number.parseInt(cells.substr(i + 6, 2), 16);
    const vv = Number.parseInt(cells.substr(i + 8, 2), 16);
    if (r < MAP_ROWS && q < MAP_COLS) {
      const cell = decodeCell(tt, dd, vv);
      // Preserve locally-cleared resource — col event may arrive before vis disk
      if (collectedCells.has(`${q}_${r}`) && cell && cell.resource > 0) {
        cell.resource = 0;
        cell.amount   = 0;
      }
      gameMap[r][q] = cell;
      // Collect notable decoded values for logging
      if (cell) {
        const flags = [];
        if (cell.poi)                             flags.push('POI');
        if (cell.shelter)                         flags.push(`shelter=${cell.shelter}`);
        if (cell.resource)                        flags.push(`res=${cell.resource}`);
        if (cell.footprints)                      flags.push(`fp=0x${cell.footprints.toString(16)}`);
        if (cell.variant)                         flags.push(`var=${cell.variant}`);
        if (flags.length) notable.push(`(q${q},r${r}) T=${tt.toString(16).padStart(2,'0')} [${flags.join(' ')}]`);
      }
    }
  }

  if (notable.length > 0) {
    console.log('%c[MAP] applyVisDisk', 'color:#09f;font-weight:bold',
      `${cellCount} cells decoded — notable:`);
    notable.forEach(n => console.log('  ', n));
  } else {
    console.log('%c[MAP] applyVisDisk', 'color:#09f', `${cellCount} cells decoded — all clear`);
  }
}
