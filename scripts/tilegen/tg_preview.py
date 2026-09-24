"""Preview: lay tiles out the way renderer.js does (flat-top axial, drawn in
r-then-q order so a prop standing up into the hex behind overdraws it), at a
real on-screen hex size, over the fog colour. Tiles are judged in context or
not at all."""
import math
from PIL import Image, ImageDraw
from tg_core import CELL_W, CELL_H, CX, CY

FOG = (8, 4, 2, 255)


def hex_to_px(q, r, size):
    return size * 1.5 * q, size * math.sqrt(3) * (r + q / 2)


def patch(grid, size, pad=None, bg=FOG, label=None):
    """grid: dict {(q, r): PIL tile}. size: HEX_SZ in px (tile drawn 2*size wide)."""
    k = 2 * size / CELL_W
    cells = sorted(grid.keys(), key=lambda qr: (qr[1], qr[0]))
    pts = [hex_to_px(q, r, size) for q, r in grid]
    minx = min(p[0] for p in pts) - size
    maxx = max(p[0] for p in pts) + size
    miny = min(p[1] for p in pts) - CY * k
    maxy = max(p[1] for p in pts) + (CELL_H - CY) * k
    pad = pad if pad is not None else int(size * 0.3)
    W = int(maxx - minx + 2 * pad)
    H = int(maxy - miny + 2 * pad)
    out = Image.new('RGBA', (W, H), bg)
    cache = {}
    # painter's order that renderer.js uses: r outer, q inner
    for (q, r) in sorted(grid.keys(), key=lambda qr: (qr[1], qr[0])):
        tile = grid[(q, r)]
        key = id(tile)
        if key not in cache:
            cache[key] = tile.resize((max(1, round(CELL_W * k)), max(1, round(CELL_H * k))), Image.LANCZOS)
        t = cache[key]
        x, y = hex_to_px(q, r, size)
        dx = int(round(x - minx + pad - CX * k))
        dy = int(round(y - miny + pad - CY * k))
        out.alpha_composite(t, (dx, dy))
    if label:
        d = ImageDraw.Draw(out)
        d.text((6, 4), label, fill=(230, 200, 140, 255))
    return out


def rect_axial(cols, rows):
    """Axial coords for a cols x rows 'even-q' rectangle."""
    out = []
    for c in range(cols):
        for rr in range(rows):
            out.append((c, rr - (c // 2)))
    return out
