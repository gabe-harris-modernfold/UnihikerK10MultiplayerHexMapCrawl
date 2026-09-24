"""Bake every hex tile into an atlas + manifest for the game.

    python scripts/tilegen/build_tiles.py                 # render all -> data/img/tiles*.webp + tiles.json
    python scripts/tilegen/build_tiles.py --only 0,8      # just some terrains (preview only, no atlas write)
    python scripts/tilegen/build_tiles.py --sheet         # also write a contact sheet + map patch to --preview-dir

Output (data/img/):
    tiles.json      manifest: counts per terrain (firmware reads these at boot
                    to size pickVariant()), cell geometry, where each tile
                    sits in which atlas page, and the pinned POI tiles
    tiles0.webp...  atlas pages

Why an atlas: the K10 serves /img/* out of a PSRAM cache with a file-count
cap, sync_data uploads file by file, and the client's AssetLoader runs two
requests at a time. ~70 tile PNGs cost ~70 round trips on every cold load;
two atlas pages cost two.

Geometry: every cell is CELL_W x CELL_H. The flat-top hex (radius R) is
centred at (CX, CY) inside it, with HEADROOM above for props that stand up
into the hex behind (the renderer draws rows back to front, so a mountain
peak overdraws the hex above it). Draw a cell at hex size S (px radius) as:

    scale = S / R
    drawImage(page, sx, sy, CELL_W, CELL_H,
              cx - CX * scale, cy - CY * scale, CELL_W * scale, CELL_H * scale)
"""
import argparse
import hashlib
import json
import math
import os
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from tg_core import CELL_W, CELL_H, CX, CY, R  # noqa: E402
import tg_terrains as T  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
IMG = os.path.join(REPO, 'data', 'img')

# MIRRORED from TERRAIN_IMG_NAMES in data/engine.js and TERRAIN_IMG_NAME in
# the firmware -- index = terrain id.
TERRAIN_NAMES = [
    'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
    'BrokenUrban', 'FloodedDistrict', 'GlassFields',
    'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
    'BunkerEntrance', 'VentShaft', 'TunnelFloor', 'TunnelCollapsed',
]
# Variant pool size per terrain. Hard limits:
#  - the wire packs cell.variant into 4 bits, so 16 is the ceiling;
#  - Open Scrub (0) and Broken Urban (4) must stay <= 10, because
#    hex-map.hpp pins sentinel variants 10 (Jack's Chopper) and 10..12 (city
#    core) past the end of those pools;
#  - River Channel (11) stays 0 on purpose: rivers are drawn by the renderer
#    as a connected, animated channel, never as a per-hex still.
COUNTS = [10, 6, 8, 6, 10, 6, 5, 5, 5, 8, 4, 0, 2, 2, 4, 2]
# Pinned landmark art, keyed "terrain_variant" as the firmware pins it.
POI = {
    '0_10': ('scrub', 'jacks_chopper'),
    '4_10': ('urban', 'city_core0'),
    '4_11': ('urban', 'city_core1'),
    '4_12': ('urban', 'city_core2'),
}
PAGE_COLS = 8
PAGE_ROWS = 7


def seed_for(t, v):
    return int(hashlib.sha1(f'wasteland-tile-{t}-{v}'.encode()).hexdigest()[:8], 16)


def render(t, v):
    fn = T.BUILDERS.get(t)
    if fn is None:
        return None
    return fn(v, seed_for(t, v)).result()


def render_poi(key):
    fn = T.POI_BUILDERS.get(key) if hasattr(T, 'POI_BUILDERS') else None
    if fn is None:
        return None
    return fn(seed_for(99, key)).result()


def pack(tiles):
    """tiles: list of (key, PIL). -> pages, placements {key: [page, x, y]}"""
    per = PAGE_COLS * PAGE_ROWS
    pages, place = [], {}
    for pi in range(0, len(tiles), per):
        chunk = tiles[pi:pi + per]
        rows = math.ceil(len(chunk) / PAGE_COLS)
        page = Image.new('RGBA', (PAGE_COLS * CELL_W, rows * CELL_H), (0, 0, 0, 0))
        for i, (key, im) in enumerate(chunk):
            x = (i % PAGE_COLS) * CELL_W
            y = (i // PAGE_COLS) * CELL_H
            page.alpha_composite(im, (x, y))
            place[key] = [len(pages), x, y]
        pages.append(page)
    return pages, place


def clean_alpha(im):
    """Zero the colour under fully transparent pixels so lossy WebP doesn't
    spend bits (or bleed fringes) on invisible junk."""
    a = np.asarray(im).copy()
    a[a[..., 3] == 0, :3] = 0
    return Image.fromarray(a, 'RGBA')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', help='comma list of terrain ids (preview only)')
    ap.add_argument('--quality', type=int, default=86)
    ap.add_argument('--out', default=IMG)
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--preview-dir', default=os.path.join(HERE, 'out'))
    ap.add_argument('--png', action='store_true', help='also write PNG pages for size comparison')
    args = ap.parse_args()
    only = [int(x) for x in args.only.split(',')] if args.only else None

    t0 = time.time()
    rendered = {}          # (t, v) -> PIL
    counts = [0] * len(COUNTS)
    for t, n in enumerate(COUNTS):
        if only is not None and t not in only:
            continue
        got = 0
        for v in range(n):
            im = render(t, v)
            if im is None:
                break
            rendered[(t, v)] = im
            got += 1
        counts[t] = got
        print(f'  {TERRAIN_NAMES[t]:16s} {got}/{n}  ({time.time() - t0:5.1f}s)')
    pois = {}
    for key in POI:
        im = render_poi(key)
        if im is not None:
            pois[key] = im
    print(f'  POI              {len(pois)}/{len(POI)}')

    os.makedirs(args.preview_dir, exist_ok=True)
    if args.sheet or only is not None:
        write_sheet(rendered, pois, os.path.join(args.preview_dir, 'contact.png'))
    if only is not None:
        print(f'preview only; {time.time() - t0:.1f}s')
        return

    tiles = [((t, v), rendered[(t, v)]) for t in range(len(COUNTS)) for v in range(counts[t])]
    tiles += [(k, pois[k]) for k in POI if k in pois]
    pages, place = pack(tiles)
    names = []
    total = 0
    digest = hashlib.sha1()
    for i, page in enumerate(pages):
        name = f'tiles{i}.webp'
        path = os.path.join(args.out, name)
        clean_alpha(page).save(path, 'WEBP', quality=args.quality, method=6, alpha_quality=100)
        raw = open(path, 'rb').read()
        digest.update(raw)
        total += len(raw)
        names.append(name)
        if args.png:
            page.save(os.path.join(args.preview_dir, f'tiles{i}.png'), optimize=True)
        print(f'  {name}: {page.size[0]}x{page.size[1]}  {len(raw) // 1024} KB')
    manifest = {
        'version': digest.hexdigest()[:12],
        'pages': names,
        'cell': [CELL_W, CELL_H],
        'anchor': [CX, round(CY, 3)],
        'radius': R,
        'counts': counts,
        'tiles': [[place[(t, v)] for v in range(counts[t])] for t in range(len(COUNTS))],
        'poi': {k: place[k] for k in POI if k in place},
    }
    with open(os.path.join(args.out, 'tiles.json'), 'w', newline='\n') as f:
        # counts first and on one line: the firmware scans for "counts":[
        f.write(json.dumps(manifest, separators=(',', ':')))
    print(f'atlas: {len(tiles)} tiles, {len(pages)} pages, {total // 1024} KB, version {manifest["version"]}, '
          f'{time.time() - t0:.1f}s')
    if args.sheet:
        write_patch(rendered, os.path.join(args.preview_dir, 'patch.png'))


def write_sheet(rendered, pois, path):
    from PIL import ImageDraw
    C = 112
    rows = sorted(set(t for t, v in rendered))
    ncol = max([v for t, v in rendered] + [0]) + 1
    W = 130 + ncol * (C + 4)
    H = (len(rows) + (1 if pois else 0)) * (int(C * CELL_H / CELL_W) + 4) + 4
    rh = int(C * CELL_H / CELL_W)
    sheet = Image.new('RGBA', (W, H), (10, 8, 8, 255))
    d = ImageDraw.Draw(sheet)
    for ri, t in enumerate(rows):
        y = 4 + ri * (rh + 4)
        d.text((4, y + rh // 2), f'{t} {TERRAIN_NAMES[t]}', fill=(230, 200, 140, 255))
        for v in range(ncol):
            if (t, v) in rendered:
                im = rendered[(t, v)].resize((C, rh), Image.LANCZOS)
                sheet.alpha_composite(im, (130 + v * (C + 4), y))
                d.text((130 + v * (C + 4) + 2, y + 2), str(v), fill=(255, 255, 0, 255))
    if pois:
        y = 4 + len(rows) * (rh + 4)
        d.text((4, y + rh // 2), 'POI', fill=(230, 200, 140, 255))
        for i, (k, im) in enumerate(pois.items()):
            sheet.alpha_composite(im.resize((C, rh), Image.LANCZOS), (130 + i * (C + 4), y))
            d.text((130 + i * (C + 4) + 2, y + 2), k, fill=(255, 255, 0, 255))
    sheet.save(path)
    print('  sheet ->', path)


def write_patch(rendered, path):
    from tg_preview import patch
    rng = np.random.default_rng(7)
    keys = list(rendered.keys())
    grid = {}
    for c in range(9):
        for rr in range(7):
            grid[(c, rr - (c // 2))] = rendered[keys[rng.integers(len(keys))]]
    patch(grid, 48).save(path)
    print('  patch ->', path)


if __name__ == '__main__':
    main()
