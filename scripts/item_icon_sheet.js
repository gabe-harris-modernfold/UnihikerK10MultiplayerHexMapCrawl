// item_icon_sheet.js -- contact sheet of the procedural item icons.
//
// Runs data/item-icons.js exactly as the browser does (no DOM needed: it
// draws into a plain RGBA buffer) and lays every icon out at the sizes the UI
// actually shows them, zoomed so the pixels can be judged:
//
//   top:    32 px (item sheet)        x zoom
//   bottom: 26 px (pack / caravan)    x 2,   16 px (ground list) x 2,
//           14 px (haul chip)         x 2
//
// on the slot background (--bg-deep) inside the occupied-slot border. The
// number in each cell's corner is the item id.
//
//   node scripts/item_icon_sheet.js                  -> item_icon_sheet.png
//   node scripts/item_icon_sheet.js --ids 1-10,63 --zoom 4 --out a.png
//
// No dependencies: the PNG is written with zlib.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const zlib = require('zlib');

const args = process.argv.slice(2);
const opt = (name, dflt) => { const i = args.indexOf('--' + name); return i >= 0 ? args[i + 1] : dflt; };
const ZOOM = Math.max(1, parseInt(opt('zoom', '3'), 10));
const COLS = Math.max(1, parseInt(opt('cols', '8'), 10));
const OUT = opt('out', 'item_icon_sheet.png');

const ctx = { console };
ctx.globalThis = ctx;
vm.createContext(ctx);
const src = path.join(__dirname, '..', 'data', 'item-icons.js');
vm.runInContext(fs.readFileSync(src, 'utf8'), ctx, { filename: src });
const II = ctx.ItemIcons;

function parseIds(spec) {
  const ids = [];
  for (const part of spec.split(',')) {
    const m = part.match(/^(\d+)-(\d+)$/);
    if (m) for (let i = +m[1]; i <= +m[2]; i++) ids.push(i);
    else if (/^\d+$/.test(part)) ids.push(+part);
    else if (part) ids.push(part);
  }
  return ids;
}
const ids = parseIds(opt('ids', '1-65,cat0,cat1,cat2,cat3,skull'));

// ── Canvas ────────────────────────────────────────────────────────
const PAD = 6;
const big = 32 * ZOOM;
const cellW = Math.max(big, 26 * 2 + 16 * 2 + 14 * 2 + 12) + PAD * 2;
const cellH = big + 26 * 2 + PAD * 3 + 4;
const rows = Math.ceil(ids.length / COLS);
const W = cellW * COLS, H = cellH * rows;
const img = new Uint8Array(W * H * 4);

const rgb = (h) => [(h >> 16) & 255, (h >> 8) & 255, h & 255];
function fill(x0, y0, w, h, c) {
  const [r, g, b] = rgb(c);
  for (let y = Math.max(0, y0); y < Math.min(H, y0 + h); y++) {
    for (let x = Math.max(0, x0); x < Math.min(W, x0 + w); x++) {
      const o = (y * W + x) * 4;
      img[o] = r; img[o + 1] = g; img[o + 2] = b; img[o + 3] = 255;
    }
  }
}
function blit(px, n, x0, y0, z) {
  for (let j = 0; j < n; j++) {
    for (let i = 0; i < n; i++) {
      const s = (j * n + i) * 4;
      if (px[s + 3] === 0) continue;
      for (let dy = 0; dy < z; dy++) {
        for (let dx = 0; dx < z; dx++) {
          const o = ((y0 + j * z + dy) * W + (x0 + i * z + dx)) * 4;
          img[o] = px[s]; img[o + 1] = px[s + 1]; img[o + 2] = px[s + 2]; img[o + 3] = 255;
        }
      }
    }
  }
}
// 3x5 digits (and a few letters for the category glyphs), for cell labels.
const GLYPH = {
  0: '111101101101111', 1: '010110010010111', 2: '111001111100111', 3: '111001111001111',
  4: '101101111001001', 5: '111100111001111', 6: '111100111101111', 7: '111001010010010',
  8: '111101111101111', 9: '111101111001111', c: '000111100100111', a: '000011101101011',
  t: '010111010010011', s: '000011110011110', k: '100101110101101', u: '000101101101111',
  l: '110010010010111',
};
function label(text, x0, y0, c) {
  let x = x0;
  for (const ch of String(text)) {
    const g = GLYPH[ch];
    if (g) for (let k = 0; k < 15; k++) if (g[k] === '1') fill(x + (k % 3) * 2, y0 + ((k / 3) | 0) * 2, 2, 2, c);
    x += 8;
  }
}

fill(0, 0, W, H, 0x080604);
ids.forEach((id, idx) => {
  const cx = (idx % COLS) * cellW, cy = ((idx / COLS) | 0) * cellH;
  fill(cx + 2, cy + 2, cellW - 4, cellH - 4, 0x6A3008);              // .item-slot.occupied border
  fill(cx + 3, cy + 3, cellW - 6, cellH - 6, 0x050402);              // --bg-deep
  blit(II.rgba(id, 32), 32, cx + ((cellW - big) >> 1), cy + PAD, ZOOM);
  let x = cx + PAD;
  const y = cy + PAD * 2 + big;
  for (const n of [26, 16, 14]) { blit(II.rgba(id, n), n, x, y, 2); x += n * 2 + 4; }
  label(id, cx + 5, cy + 5, 0xE8A828);
});

// ── PNG ───────────────────────────────────────────────────────────
const CRC = new Int32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  return c;
});
function crc32(buf) {
  let c = -1;
  for (const b of buf) c = CRC[(c ^ b) & 255] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
const raw = Buffer.alloc((W * 4 + 1) * H);
for (let y = 0; y < H; y++) Buffer.from(img.buffer, y * W * 4, W * 4).copy(raw, y * (W * 4 + 1) + 1);
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 6;
fs.writeFileSync(OUT, Buffer.concat([
  Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
  chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0)),
]));
console.log(`wrote ${OUT}: ${ids.length} icons, ${W}x${H}`);
