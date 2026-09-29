"""Map sprite sheets: one sheet per terrain, shelter kind and forage set, not one file each.

    python scripts/hex_sheets.py build             # art/hex-sheets/*.png -> data/img/*.webp + tiles.json
    python scripts/hex_sheets.py build --check     # validate + report only, write nothing
    python scripts/hex_sheets.py split OpenScrub   # a sheet -> one PNG per cell (%TEMP%/hex-cells/)
    python scripts/hex_sheets.py split shelterBasic
    python scripts/hex_sheets.py import            # per-file data/img tiles -> sheets
    python scripts/hex_sheets.py import --src DIR --force   # re-seed sheets from <name><N>.png cells

The MASTER sheets live in art/hex-sheets/, outside data/ so sync_data never
pushes them to the board. They are lossless RGBA; edit those:
    hex<Name>.png         terrain tiles, TERRAINS order
    shelterBasic.png      shelter sprites, cell.shelter 1
    shelterImproved.png   shelter sprites, cell.shelter 2
    forrageAnimal.png     forage animals, drawn on food (resource 2) hexes
    caravan.png           the caravan's convoy sticker (worldState.caravan)
    tunCorridor.png, tunRoom.png, tunFixture.png
                          the bunker tunnel board (see TUNNEL_SHEETS below),
                          with tunCorridor.json / tunFixture.json sidecars
`build` encodes each one as a lossy WebP page (same name, .webp) in data/img/
and writes data/img/tiles.json, the manifest engine.js, observer.js, the mock
and the firmware (setupVariantCounts: "counts", "shelterCounts", "forageCounts")
read.

Sheet layout: square cells in a grid, a gutter of clear space around and
between every cell -- 256 px / 32 px for terrain, 224 / 16 for shelters,
80 / 16 for forage, 384 / 16 for the caravan. A cell holds the sprite exactly
as the renderer draws it, stretched to a square (a tile is a 2*HEX_SZ square,
a shelter 0.9*HEX_SZ, a forage animal 0.45*HEX_SZ, the caravan 1.5*HEX_SZ):
    x = gutter + col * (cell + gutter),  y = gutter + row * (cell + gutter)
Cells run left to right, top to bottom. A fully transparent cell is empty.

A terrain's pool (what pickVariant() deals from) is cells 0..k-1: up to the
first empty cell, or the terrain's first landmark sentinel (LANDMARKS below),
whichever comes first. Cells at or past a sentinel are pinned landmark art,
written to the manifest's `poi` under "<terrain>_<variant>" -- e.g. Open
Scrub cell 10 is Jack's Chopper (hex-map.hpp Phase 5.5 pins variant 10).
pickVariant() weights slot 0 heaviest, so order a sheet common -> rare.
Shelters and forage animals have no variant on the wire: the client picks
one by position, so their order doesn't matter.

Alternates: a terrain sheet may have a sidecar hex<Name>.json,
    {"alts": {"0": [13, 14], "10": [21]}}
listing, per variant (a pool slot or a landmark), more cells of the same
sheet that share its slot. The client deals one of [the variant's own cell,
*its alternates] by a hash of the hex's position (altPick() in engine.js,
mirrored in observer.js), so a slot holds several tiles while the firmware
still counts -- and weights -- only the pool. Alternate cells sit past the
pool and are neither pool tiles nor landmarks; the manifest carries them as
"alts": {"<terrain>": {"<variant>": [[page, sx, sy], ...]}}.

Needs Pillow + numpy.
"""
import argparse
import hashlib
import io
import json
import os
import sys
import tempfile
from collections import namedtuple

import numpy as np
from PIL import Image, PngImagePlugin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_quant import visible_rmse  # noqa: E402  (the repo's art quality metric)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEETS = os.path.join(ROOT, 'art', 'hex-sheets')
IMG = os.path.join(ROOT, 'data', 'img')

# cell: sprite edge. gutter: clear px around and between cells, a multiple of
# 4 so the half/quarter mips engine.js builds keep whole-px cells.
# lossless: how the page is encoded. Otherwise lossy WebP (alpha kept
# lossless), per sheet the lowest QUALITY_LADDER rung that keeps its worst
# cell inside optimize_art.py's visible-RMSE bar, else the top rung -- 4:2:0
# chroma is the floor, and the yellow school bus never gets under ~4.
Grid = namedtuple('Grid', 'cell gutter lossless')
# A tile is drawn as a 2*HEX_SZ square, HEX_SZ tops out near 125.
TILE = Grid(256, 32, False)
# A shelter is drawn as a 0.9*HEX_SZ square (~225 device px at the top zoom
# on a 2x screen). 224 px is the resolution of the 2026-09-27 re-render; at
# that size the lossy ladder passes (q90-95).
SHELTER = Grid(224, 16, False)
# A forage animal is drawn as a 0.45*HEX_SZ square; the widest is 77 px.
# Lossless: small ink-outlined sprites never get under the bar lossy (the old
# 112 px shelters were 4.2 even at q100), and lossless is tiny at this size.
FORAGE = Grid(80, 16, True)
# The caravan (the roaming trader, world-entities.js) is one convoy sticker
# drawn as a 1.5*HEX_SZ square: 384 device px at the top zoom on a 2x screen.
CARAVAN = Grid(384, 16, False)
MAX_COLS = 4
QUALITY_LADDER = (90, 92, 95)
MAX_RMSE = 3.5

# terrain index order = TERRAIN_IMG_NAMES in engine.js / game-server.hpp
TERRAINS = [
    'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
    'BrokenUrban', 'FloodedDistrict', 'GlassFields',
    'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
    'BunkerEntrance', 'VentShaft', 'TunnelFloor', 'TunnelCollapsed',
]
# cell.shelter 1, 2 = SHELTER_IMG_NAMES in engine.js / observer.js
SHELTERS = ['shelterBasic', 'shelterImproved']
# Sprite families with no variant on the wire: the client picks a sprite by
# position, so a sheet is just a pool. (manifest key, its counts key -- a flat
# array the firmware scans for --, its cell key, the sheets, their grid).
Family = namedtuple('Family', 'key counts cell sheets grid')
FAMILIES = [
    Family('shelters', 'shelterCounts', 'shelterCell', SHELTERS, SHELTER),
    # food resource (type 2) hexes; the firmware's forrageAnimalCount / wire `fa`
    Family('forage', 'forageCounts', 'forageCell', ['forrageAnimal'], FORAGE),
    # worldState.caravan: the APC-led convoy. One cell today; more would be
    # picked by position like the others.
    Family('caravan', 'caravanCounts', 'caravanCell', ['caravan'], CARAVAN),
]
# The bunker tunnel board (tunnels.hpp) is not drawn per terrain. Each cell
# carries a 6-bit open-sides mask on the wire (`op`), and the art follows it:
#   tunCorridor.png  overhead corridor pieces. tunCorridor.json gives each
#                    cell's open-side mask as drawn (bit d = direction d,
#                    DQ/DR order). The client rotates/mirrors a piece onto a
#                    cell's mask, so one piece serves every orientation.
#   tunRoom.png      angled rooms, the dead ends; the firmware deals each world
#                    a set of distinct room indices. tunRoom.json "chamber"
#                    lists the corridor-like ones, which also draw a corridor
#                    cell whose shape no piece covers.
#   tunFixture.png   shaft interiors, cave-ins and plain rock, the roles and
#                    their cells listed in tunFixture.json.
# Manifest: "tunnel" (where every cell sits) and the flat
# "tunnelCounts":[rooms, entrance, vent, cave] the firmware scans.
TUNNEL_SHEETS = ['tunCorridor', 'tunRoom', 'tunFixture']
TUNNEL_ROLES = ['entrance', 'vent', 'cave', 'rock']
# First sentinel variant the firmware pins per terrain (hex-map.hpp): cells
# from here on are landmark art, not pool tiles.
LANDMARKS = {
    0: 10,   # Jack's Chopper (Phase 5.5), was poi_jacks_chopper.png
    4: 10,   # downtown core, variants 10-12 (Phase 4 city-core pin), gi23 art 2026-09-28
}
# The per-file landmark art `import` folds into a sheet.
LEGACY_POI = {(0, 10): 'poi_jacks_chopper.png'}
POOL_MAX = 16   # cell.variant is 4 bits on the wire


def cell_xy(i, cols, g):
    return g.gutter + (i % cols) * (g.cell + g.gutter), g.gutter + (i // cols) * (g.cell + g.gutter)


def sheet_size(n, g):
    cols = min(max(n, 1), MAX_COLS)
    rows = (max(n, 1) + cols - 1) // cols
    return cols, g.gutter + cols * (g.cell + g.gutter), g.gutter + rows * (g.cell + g.gutter)


def grid_of(im, name, g):
    """Columns and rows of a sheet, from its size alone."""
    w, h = im.size
    step = g.cell + g.gutter
    if (w - g.gutter) % step or (h - g.gutter) % step:
        sys.exit(f'{name}: {w}x{h} is not a {g.cell}px / {g.gutter}px-gutter grid')
    return (w - g.gutter) // step, (h - g.gutter) // step


def to_cell(im, g):
    """A sprite as the renderer draws it: stretched onto a square cell.

    The old tiles are small and not square (hexRidge0 is 108x96, a shelter
    about 100x85); drawImage stretches them to a square on screen, so baking
    the same stretch in keeps them looking exactly as they do on the map.
    Resampled premultiplied, so the transparent corners don't bleed dark
    into the rim.
    """
    im = im.convert('RGBA')
    if im.size != (g.cell, g.cell):
        im = im.convert('RGBa').resize((g.cell, g.cell), Image.LANCZOS).convert('RGBA')
    return im


def cells_of(im, name, g):
    cols, rows = grid_of(im, name, g)
    a = np.asarray(im.convert('RGBA'))
    out = []
    for i in range(cols * rows):
        x, y = cell_xy(i, cols, g)
        c = a[y:y + g.cell, x:x + g.cell]
        out.append(c if c[..., 3].any() else None)
    # Anything painted in a gutter would be sampled by neither cell, and the
    # half-size mips would drag it into both neighbours' rims.
    alpha = a[..., 3].copy()
    for i in range(cols * rows):
        x, y = cell_xy(i, cols, g)
        alpha[y:y + g.cell, x:x + g.cell] = 0
    if alpha.any():
        ys, xs = np.nonzero(alpha)
        print(f'  warning: {name} has paint in a gutter near ({xs[0]}, {ys[0]}) -- it is ignored')
    return cols, out


def clean_alpha(im):
    """Zero the colour under fully transparent pixels: it is never seen and
    otherwise costs bytes (and can bleed into mips)."""
    a = np.asarray(im.convert('RGBA')).copy()
    a[a[..., 3] == 0] = 0
    return Image.fromarray(a, 'RGBA')


def save_sheet(path, cells, g):
    cols, w, h = sheet_size(len(cells), g)
    sheet = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for i, c in enumerate(cells):
        if c is not None:
            sheet.paste(c, cell_xy(i, cols, g))
    info = PngImagePlugin.PngInfo()
    info.add_text('hex-sheet', json.dumps({'cell': g.cell, 'gutter': g.gutter, 'cols': cols}))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    clean_alpha(sheet).save(path, 'PNG', optimize=True, pnginfo=info)
    return cols


def encode_page(sheet, cells, cols, g, quality):
    """WebP bytes for a sheet, and its worst cell's visible RMSE (scored the
    way optimize_art.py scores its PNGs). quality None = lossless."""
    buf = io.BytesIO()
    if quality is None:
        sheet.save(buf, 'WEBP', lossless=True, quality=100, method=6, exact=False)
    else:
        sheet.save(buf, 'WEBP', quality=quality, method=6, alpha_quality=100, exact=False)
    buf.seek(0)
    dec = np.asarray(Image.open(buf).convert('RGBA'))
    worst = 0.0
    for i, c in enumerate(cells):
        if c is not None:
            x, y = cell_xy(i, cols, g)
            worst = max(worst, visible_rmse(Image.fromarray(c, 'RGBA'),
                                            Image.fromarray(dec[y:y + g.cell, x:x + g.cell], 'RGBA')))
    return buf.getvalue(), worst


def numbered(src, prefix):
    """prefix0.png, prefix1.png, ... up to the first gap."""
    out = []
    while os.path.exists(os.path.join(src, f'{prefix}{len(out)}.png')):
        out.append(os.path.join(src, f'{prefix}{len(out)}.png'))
    return out


def write_master(args, stem, cells, g, note):
    path = os.path.join(args.sheets, f'{stem}.png')
    if os.path.exists(path) and not args.force:
        print(f'  {stem}.png exists, skipped (--force to overwrite)')
        return
    cols = save_sheet(path, cells, g)
    print(f'  {stem}.png: {note}, {cols} cols')


def cmd_import(args):
    """Per-file tiles and sprites -> master sheets (cell N = file N)."""
    src = args.src or IMG
    for t, name in enumerate(TERRAINS):
        tiles = numbered(src, f'hex{name}')
        landmarks = {v: os.path.join(src, f) for (tt, v), f in LEGACY_POI.items()
                     if tt == t and os.path.exists(os.path.join(src, f))}
        if not tiles and not landmarks:
            continue
        n = max([len(tiles)] + [v + 1 for v in landmarks])
        if landmarks and len(tiles) != LANDMARKS[t]:
            sys.exit(f'{name}: {len(tiles)} pool tiles but the landmark sentinel is {LANDMARKS[t]}')
        cells = [None] * n
        for v, f in enumerate(tiles):
            cells[v] = to_cell(Image.open(f), TILE)
        for v, f in landmarks.items():
            cells[v] = to_cell(Image.open(f), TILE)
        extra = f' + landmark{"s" if len(landmarks) > 1 else ""} {sorted(landmarks)}' if landmarks else ''
        write_master(args, f'hex{name}', cells, TILE, f'{len(tiles)} tiles{extra}')
    for fam in FAMILIES:
        for name in fam.sheets:
            files = numbered(src, name)
            if files:
                write_master(args, name, [to_cell(Image.open(f), fam.grid) for f in files], fam.grid,
                             f'{len(files)} {fam.key}')


class Pages:
    """The WebP pages a build writes, in manifest order."""

    def __init__(self, args):
        self.args, self.names, self.digest, self.total = args, [], hashlib.sha1(), 0

    def add(self, stem, cells, cols, g, note):
        src = clean_alpha(Image.open(os.path.join(self.args.sheets, f'{stem}.png')))
        ladder = [None] if g.lossless else [self.args.quality] if self.args.quality else QUALITY_LADDER
        for q in ladder:
            raw, worst = encode_page(src, cells, cols, g, q)
            if worst <= MAX_RMSE:
                break
        out = f'{stem}.webp'
        self.digest.update(raw)
        self.total += len(raw)
        self.names.append(out)
        if not self.args.check:
            with open(os.path.join(self.args.out, out), 'wb') as f:
                f.write(raw)
        enc = 'lossless' if q is None else f'q{q}'
        print(f'  {out}: {note}, {enc}, {len(raw) // 1024} KB, worst visible RMSE {worst:.2f}')
        return len(self.names) - 1


def load_alts(sheets, stem, cells):
    """hex<Name>.json "alts" -> {variant: [cell, ...]}, validated against the sheet."""
    side = os.path.join(sheets, f'{stem}.json')
    if not os.path.exists(side):
        return {}
    alts = {int(v): list(c) for v, c in json.load(open(side)).get('alts', {}).items()}
    seen = set()
    for v, cs in alts.items():
        if not 0 <= v < POOL_MAX:
            sys.exit(f'{stem}.json: alts for variant {v}, but a variant is 4 bits')
        for c in cs:
            if not 0 <= c < len(cells) or cells[c] is None:
                sys.exit(f'{stem}.json: variant {v} alternate cell {c} is empty or off the sheet')
            if c in seen or c in alts:
                sys.exit(f'{stem}.json: cell {c} is listed twice, or is itself a variant with alternates')
            seen.add(c)
    return alts


def cmd_build(args):
    pages = Pages(args)
    counts, tiles, poi, all_alts = [], [], {}, {}
    for t, name in enumerate(TERRAINS):
        stem = f'hex{name}'
        path = os.path.join(args.sheets, f'{stem}.png')
        if not os.path.exists(path):
            counts.append(0)
            tiles.append([])
            continue
        cols, cells = cells_of(Image.open(path), f'{stem}.png', TILE)
        alts = load_alts(args.sheets, stem, cells)
        alt_cells = {c for cs in alts.values() for c in cs}
        sentinel = LANDMARKS.get(t, POOL_MAX)
        pool = 0
        while pool < min(len(cells), sentinel) and cells[pool] is not None and pool not in alt_cells:
            pool += 1
        for i in range(pool, min(len(cells), sentinel)):
            if cells[i] is not None and i not in alt_cells:
                print(f'  warning: {stem}.png cell {i} is past an empty cell -- it is not in the pool')
        if pool > POOL_MAX:
            sys.exit(f'{stem}.png: {pool} tiles, but a variant is 4 bits (max {POOL_MAX})')
        marks = [i for i in range(sentinel, min(len(cells), POOL_MAX)) if cells[i] is not None and i not in alt_cells]
        for i in range(POOL_MAX, len(cells)):
            if cells[i] is not None and i not in alt_cells:
                print(f'  warning: {stem}.png cell {i} is past the 4-bit variants and not an alternate -- it is not used')
        for v in alts:
            if v >= pool and v not in marks:
                sys.exit(f'{stem}.json: alternates for variant {v}, which is neither a pool slot nor a landmark')
        lm = f' + landmark {", ".join(f"{t}_{i}" for i in marks)}' if marks else ''
        al = f' + {len(alt_cells)} alternates' if alt_cells else ''
        page = pages.add(stem, cells, cols, TILE, f'{pool} tiles{lm}{al}')
        counts.append(pool)
        tiles.append([[page, *cell_xy(i, cols, TILE)] for i in range(pool)])
        for i in marks:
            poi[f'{t}_{i}'] = [page, *cell_xy(i, cols, TILE)]
        if alts:
            all_alts[str(t)] = {str(v): [[page, *cell_xy(c, cols, TILE)] for c in cs] for v, cs in sorted(alts.items())}

    families = {}   # key -> (counts, pools)
    for fam in FAMILIES:
        fam_counts, pools = [], []
        for name in fam.sheets:
            path = os.path.join(args.sheets, f'{name}.png')
            if not os.path.exists(path):
                fam_counts.append(0)
                pools.append([])
                continue
            cols, cells = cells_of(Image.open(path), f'{name}.png', fam.grid)
            n = 0
            while n < len(cells) and cells[n] is not None:
                n += 1
            if any(c is not None for c in cells[n:]):
                print(f'  warning: {name}.png has a cell past an empty one -- it is not used')
            page = pages.add(name, cells, cols, fam.grid, f'{n} {fam.key}')
            fam_counts.append(n)
            pools.append([[page, *cell_xy(i, cols, fam.grid)] for i in range(n)])
        families[fam.key] = (fam_counts, pools)

    tunnel, tunnel_counts = build_tunnel(pages, args)

    manifest = {
        # counts first, each a flat array: the firmware scans for "counts":[,
        # "shelterCounts":[ and "forageCounts":[
        'counts': counts,
        **{fam.counts: families[fam.key][0] for fam in FAMILIES},
        'tunnelCounts': tunnel_counts,
        'version': pages.digest.hexdigest()[:12],
        'flat': True,     # painted flat tiles: grid drawn over them, no overhang
        'cell': [TILE.cell, TILE.cell],
        'anchor': [TILE.cell // 2, TILE.cell // 2],
        'radius': TILE.cell // 2,
        'pages': pages.names,
        'tiles': tiles,
        'poi': poi,
        'alts': all_alts,
    }
    for fam in FAMILIES:
        manifest[fam.cell] = [fam.grid.cell, fam.grid.cell]
        manifest[fam.key] = families[fam.key][1]
    if tunnel:
        manifest['tunnel'] = tunnel
    sprites = ' + '.join(f'{sum(families[f.key][0])} {f.key}' for f in FAMILIES)
    if tunnel:
        sprites += f' + {len(tunnel["corridor"])} corridors, {len(tunnel["room"])} rooms'
    print(f'{len(pages.names)} sheets, {sum(counts)} tiles + {len(poi)} landmark + {sprites}, '
          f'{pages.total // 1024} KB, version {manifest["version"]}')
    if args.check:
        return
    with open(os.path.join(args.out, 'tiles.json'), 'w', newline='\n') as f:
        f.write(json.dumps(manifest, separators=(',', ':')))


def build_tunnel(pages, args):
    """The tunnel sheets -> (manifest "tunnel", "tunnelCounts"), or ({}, zeros)
    when they are not there -- the client then draws the tunnel board flat."""
    paths = {s: os.path.join(args.sheets, f'{s}.png') for s in TUNNEL_SHEETS}
    if not all(os.path.exists(p) for p in paths.values()):
        return {}, [0] * 4

    def pool(stem):
        cols, cells = cells_of(Image.open(paths[stem]), f'{stem}.png', TILE)
        n = 0
        while n < len(cells) and cells[n] is not None:
            n += 1
        return cols, cells, n

    cols, cells, n = pool('tunCorridor')
    masks = json.load(open(os.path.join(args.sheets, 'tunCorridor.json')))['masks']
    if len(masks) != n:
        sys.exit(f'tunCorridor.json lists {len(masks)} masks for {n} cells')
    if any(not 0 < m < 64 for m in masks):
        sys.exit('tunCorridor.json: a mask is not a 6-bit open-sides set')
    page = pages.add('tunCorridor', cells, cols, TILE, f'{n} corridor pieces')
    corridor = [[page, *cell_xy(i, cols, TILE), masks[i]] for i in range(n)]

    cols, cells, n = pool('tunRoom')
    if n > 32:
        sys.exit(f'tunRoom.png: {n} rooms, but a room index is 5 bits on the wire (max 32)')
    page = pages.add('tunRoom', cells, cols, TILE, f'{n} rooms')
    room = [[page, *cell_xy(i, cols, TILE)] for i in range(n)]
    side = os.path.join(args.sheets, 'tunRoom.json')
    chamber_idx = json.load(open(side)).get('chamber', []) if os.path.exists(side) else []
    if any(not 0 <= i < n for i in chamber_idx):
        sys.exit('tunRoom.json: a "chamber" index is past the last room on the sheet')
    chamber = [room[i] for i in chamber_idx] or room

    cols, cells, n = pool('tunFixture')
    roles = json.load(open(os.path.join(args.sheets, 'tunFixture.json')))['roles']
    for role in TUNNEL_ROLES:
        if not roles.get(role):
            sys.exit(f'tunFixture.json has no "{role}" cells')
        if max(roles[role]) >= n:
            sys.exit(f'tunFixture.json: a "{role}" cell is past the sheet\'s last tile')
    page = pages.add('tunFixture', cells, cols, TILE, ', '.join(f'{len(roles[r])} {r}' for r in TUNNEL_ROLES))
    tunnel = {'corridor': corridor, 'room': room, 'chamber': chamber,
              **{r: [[page, *cell_xy(i, cols, TILE)] for i in roles[r]] for r in TUNNEL_ROLES}}
    return tunnel, [len(room), len(roles['entrance']), len(roles['vent']), len(roles['cave'])]


def cmd_split(args):
    """A master sheet -> one PNG per non-empty cell, named <name><cell>.png."""
    fam = next((f for f in FAMILIES if args.name in f.sheets), None)
    stem = args.name if fam or args.name in TUNNEL_SHEETS else f'hex{args.name}'
    g = fam.grid if fam else TILE
    path = os.path.join(args.sheets, f'{stem}.png')
    cols, cells = cells_of(Image.open(path), os.path.basename(path), g)
    os.makedirs(args.dir, exist_ok=True)
    for i, c in enumerate(cells):
        if c is not None:
            Image.fromarray(c, 'RGBA').save(os.path.join(args.dir, f'{args.name}{i}.png'))
    print(f'{sum(c is not None for c in cells)} cells -> {args.dir}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sheets', default=SHEETS, help='master sheet folder (default art/hex-sheets)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='master sheets -> data/img WebP pages + tiles.json')
    b.add_argument('--out', default=IMG)
    b.add_argument('--quality', type=int, help='one WebP quality for every sheet (default: the ladder)')
    b.add_argument('--check', action='store_true', help='validate and report, write nothing')
    s = sub.add_parser('split', help='one sheet -> a PNG per cell')
    s.add_argument('name', help='a terrain (OpenScrub) or a sprite sheet (shelterBasic, forrageAnimal)')
    s.add_argument('--dir', default=os.path.join(tempfile.gettempdir(), 'hex-cells'))
    i = sub.add_parser('import', help='per-file tiles and sprites (<name><N>.png) -> master sheets')
    i.add_argument('--src', help='folder holding the per-file images (default data/img)')
    i.add_argument('--force', action='store_true')
    args = ap.parse_args()
    {'build': cmd_build, 'split': cmd_split, 'import': cmd_import}[args.cmd](args)


if __name__ == '__main__':
    main()
