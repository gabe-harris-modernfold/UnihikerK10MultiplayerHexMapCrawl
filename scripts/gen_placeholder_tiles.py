#!/usr/bin/env python3
"""Generate placeholder hex tiles for the bunker tunnel terrains (12-15).

These are deliberately plain: a flat-top hexagon silhouette in the terrain's
own TERRAIN[].fill / .stroke colour from data/game-data.js, with a simple
centred motif so the four tiles are tellable apart on the map.  They exist so
the feature is legible and testable before the real painterly art lands --
they are meant to be replaced, and they read as obviously unfinished next to
the hand-painted tiles.

Sizing matches the existing tiles (hexOpenScrub0 is 111x97, hexRidge0 108x96).
Naming must stay contiguous from 0: setupVariantCounts() in game-server.hpp
takes max(index)+1 per terrain, so a gap silently wastes a variant slot.

    python scripts/gen_placeholder_tiles.py

Replacing one of these with real art under the SAME filename?  Bump CACHE in
data/sw.js -- the service worker caches /img/* cache-first, forever.

Style reference for the real tiles: data/img/HEX_TILE_PROMPTS.md
"""

import math
import os

from PIL import Image, ImageDraw

W, H = 112, 97
OUT = os.path.join(os.path.dirname(__file__), "..", "data", "img")

# name, fill, stroke, motif -- fill/stroke mirror TERRAIN[] in data/game-data.js
TILES = [
    ("hexBunkerEntrance0",  "#1C1A22", "#4A4458", "door"),
    ("hexVentShaft0",       "#16140E", "#38321F", "grate"),
    ("hexTunnelFloor0",     "#141210", "#2A2622", "floor"),
    ("hexTunnelCollapsed0", "#0A0908", "#1A1614", "rubble"),
]

SS = 4  # supersample factor -- drawn big, downsampled for cheap antialiasing


def hex_points(cx, cy, w, h):
    """Flat-top hexagon: points left and right, flat edges top and bottom."""
    rx, ry = w / 2.0, h / 2.0
    return [
        (cx + rx * math.cos(math.radians(a)), cy + ry * math.sin(math.radians(a)))
        for a in (0, 60, 120, 180, 240, 300)
    ]


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def draw_motif(d, kind, cx, cy, ink, dim):
    """Centred placeholder motif, drawn in supersampled space."""
    if kind == "door":
        # blast door: an arched slab with a seam down the middle
        w, h = 30 * SS, 40 * SS
        d.rounded_rectangle([cx - w, cy - h, cx + w, cy + h * 0.9],
                            radius=10 * SS, fill=dim, outline=ink, width=3 * SS)
        d.line([cx, cy - h, cx, cy + h * 0.9], fill=ink, width=3 * SS)
        for i in (-1, 1):
            d.ellipse([cx + i * 18 * SS - 3 * SS, cy - 3 * SS,
                       cx + i * 18 * SS + 3 * SS, cy + 3 * SS], fill=ink)
    elif kind == "grate":
        # vent: a round hole behind slatted bars
        r = 32 * SS
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=dim, outline=ink, width=3 * SS)
        for i in (-2, -1, 0, 1, 2):
            y = cy + i * 12 * SS
            dx = math.sqrt(max(0.0, r * r - (i * 12 * SS) ** 2)) * 0.88
            d.line([cx - dx, y, cx + dx, y], fill=ink, width=3 * SS)
    elif kind == "floor":
        # corridor: staggered slabs receding to a centre line
        for row, y in enumerate(range(-30, 40, 18)):
            off = (row % 2) * 13 * SS
            for x in range(-39, 52, 26):
                d.rectangle([cx + x * SS + off, cy + y * SS,
                             cx + (x + 22) * SS + off, cy + (y + 13) * SS],
                            fill=dim, outline=ink, width=2 * SS)
    elif kind == "rubble":
        # cave-in: a blocked X over broken slabs
        for sx, sy, s in ((-22, -12, 17), (8, -20, 14), (-6, 12, 19), (24, 10, 12)):
            d.rectangle([cx + sx * SS, cy + sy * SS,
                         cx + (sx + s) * SS, cy + (sy + s) * SS],
                        fill=dim, outline=ink, width=2 * SS)
        d.line([cx - 30 * SS, cy - 30 * SS, cx + 30 * SS, cy + 30 * SS], fill=ink, width=5 * SS)
        d.line([cx + 30 * SS, cy - 30 * SS, cx - 30 * SS, cy + 30 * SS], fill=ink, width=5 * SS)


def build(name, fill, stroke, motif):
    bw, bh = W * SS, H * SS
    img = Image.new("RGBA", (bw, bh), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = bw / 2.0, bh / 2.0

    f, s = rgb(fill), rgb(stroke)
    # hexagon body, inset a touch so the outline isn't clipped at the edges
    pts = hex_points(cx, cy, bw - 4 * SS, bh - 4 * SS)
    d.polygon(pts, fill=f + (255,))

    # a few concentric inner rings, brightening toward the rim -- stands in for
    # the soft hand-painted edge fade the real tiles have
    for i, t in enumerate((0.92, 0.84, 0.76)):
        d.polygon(hex_points(cx, cy, (bw - 4 * SS) * t, (bh - 4 * SS) * t),
                  fill=lerp(f, s, 0.10 * (3 - i)) + (255,))

    draw_motif(d, motif, cx, cy, s + (255,), lerp(f, s, 0.35) + (255,))
    d.polygon(pts, outline=s + (255,), width=3 * SS)

    img = img.resize((W, H), Image.LANCZOS)
    path = os.path.normpath(os.path.join(OUT, name + ".png"))
    img.save(path, optimize=True)
    print("  %-24s %dx%d  %d bytes" % (name + ".png", W, H, os.path.getsize(path)))


if __name__ == "__main__":
    print("Placeholder hex tiles -> data/img/")
    for t in TILES:
        build(*t)
    print("Done. Replace these with real art (see data/img/HEX_TILE_PROMPTS.md);")
    print("bump CACHE in data/sw.js if you reuse the same filenames.")
