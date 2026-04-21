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
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      const idx = (r * MAP_COLS + c) * 6;
      const tt  = parseInt(hexStr.substr(idx,     2), 16);
      const dd  = parseInt(hexStr.substr(idx + 2, 2), 16);
      const vv  = parseInt(hexStr.substr(idx + 4, 2), 16);
      gameMap[r][c] = decodeCell(tt, dd, vv);
    }
  }
}

// ── Apply vis-disk update ────────────────────────────────────────
// Format: "QQRRTTDDVV..." — 10 hex chars per cell (5 bytes)
//   QQ=col, RR=row, TT=terrain, DD=data, VV=variant
function applyVisDisk(cells) {
  for (let i = 0; i < cells.length; i += 10) {
    const q  = parseInt(cells.substr(i,     2), 16);
    const r  = parseInt(cells.substr(i + 2, 2), 16);
    const tt = parseInt(cells.substr(i + 4, 2), 16);
    const dd = parseInt(cells.substr(i + 6, 2), 16);
    const vv = parseInt(cells.substr(i + 8, 2), 16);
    if (r < MAP_ROWS && q < MAP_COLS) {
      const cell = decodeCell(tt, dd, vv);
      // Preserve locally-cleared resource — col event may arrive before vis disk
      if (collectedCells.has(`${q}_${r}`) && cell && cell.resource > 0) {
        cell.resource = 0;
        cell.amount   = 0;
      }
      gameMap[r][q] = cell;
    }
  }
}
