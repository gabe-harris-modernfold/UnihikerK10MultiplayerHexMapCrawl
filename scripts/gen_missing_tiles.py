#!/usr/bin/env python3
"""Fill empty hex-tile variant slots with labelled placeholders.

These are deliberately plain: a flat hexagon in the terrain's own colour with
a hazard stripe and the target filename printed on it, so that when one shows
up on the map you can read straight off the tile which file to go and paint.
They are not art and are not trying to be -- see HEX_TILE_PROMPTS.md for the
image-gen prompts that produce the real thing.

Naming has to stay contiguous from 0: setupVariantCounts() in game-server.hpp
takes max(index)+1 per terrain, so a gap silently wastes a variant slot. This
script only ever appends at the next free index, so it cannot open one.

Placement is in your favour: pickVariant() in hex-map.hpp weights
rank-quadratically, so variant 0 is drawn roughly n^2 as often as the last
one. Appending means these land in the *rarest* slots and the hand-painted
tiles stay the common case.

    python scripts/gen_missing_tiles.py                 # dry run, prints plan
    python scripts/gen_missing_tiles.py --apply
    python scripts/gen_missing_tiles.py --apply --min 6

Output goes through png_quant at 8 colours, so a tile lands at roughly 3.5 KB
-- about a sixth of what a real hand-painted tile costs.

Not covered by default:
  * River Channel (11) has no tile art on purpose -- renderer.js draws it with
    the animated drawRiverRipples() and falls back to that only when no image
    loads. Adding hexRiverChannel0.png would switch it to a static still.
    --include-river if you actually want that.
  * The tunnel terrains (12-15) already have placeholders from
    gen_placeholder_tiles.py, which draws proper motifs. --include-tunnels
    to top them up with labelled ones as well.

After applying, bump CACHE in data/sw.js -- the service worker caches /img/*
cache-first and forever.
"""

import argparse
import math
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont, PngImagePlugin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_quant import encode, quantize_rgba  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "img")

# SQUARE, because renderHexContent() draws every tile as
#     imgSz = HEX_SZ * 2;  drawImage(img, cx-imgSz/2, cy-imgSz/2, imgSz, imgSz)
# -- one square box, whatever the source aspect. A non-square source is simply
# stretched into it. So the source is square too, and the hexagon is inscribed
# in it rather than filling it: drawHexPath() strokes vertices at 0/60/.../300
# degrees with radius HEX_SZ, giving a hex 2*HEX_SZ wide but only sqrt(3)*HEX_SZ
# tall. The hex is therefore 100% of the square's width and 86.6% of its
# height, centred -- and the leftover 13.4% top and bottom, plus the four
# corners, is dead space no hex ever covers.
#
# 256 matches what optimize_art.py caps the real tiles at: the widest a tile is
# ever painted is 2 * HEX_SZ = 250 CSS px, so 256 covers a 2x display at the
# default zoom.
W = H = 256
SS = 3  # supersample, downsampled at the end for clean edges and text
HEX_H_RATIO = math.sqrt(3) / 2   # hex height as a fraction of the square

# Written into a tEXt chunk so --regen can find its own output again, and so
# you can tell at a glance which tiles are still awaiting real art.
TAG_KEY, TAG = "Comment", "wasteland-placeholder-v1"

# (image name, display name, fill, stroke) -- mirrors TERRAIN[] in
# data/game-data.js; index here is the terrain id.
TERRAIN = [
    ("OpenScrub",       "Open Scrub",       "#2E2210", "#504030"),
    ("AshDunes",        "Ash Dunes",        "#201E16", "#3C3A2C"),
    ("RustForest",      "Rust Forest",      "#1A2808", "#344A18"),
    ("Marsh",           "Marsh",            "#081A10", "#183428"),
    ("BrokenUrban",     "Broken Urban",     "#1A1814", "#34302A"),
    ("FloodedDistrict", "Flooded District", "#08121E", "#142030"),
    ("GlassFields",     "Glass Fields",     "#121A22", "#243444"),
    ("Ridge",           "Ridge",            "#1E1A12", "#3C3424"),
    ("Mountain",        "Mountain",         "#14141C", "#28283A"),
    ("Settlement",      "Settlement",       "#1A1206", "#382814"),
    ("NukeCrater",      "Nuke Crater",      "#0A0E04", "#1A2008"),
    ("RiverChannel",    "River Channel",    "#0B1E0F", "#162B18"),
    ("BunkerEntrance",  "Bunker Entrance",  "#1C1A22", "#4A4458"),
    ("VentShaft",       "Vent Shaft",       "#16140E", "#38321F"),
    ("TunnelFloor",     "Tunnel Floor",     "#141210", "#2A2622"),
    ("TunnelCollapsed", "Collapsed Tunnel", "#0A0908", "#1A1614"),
]
RIVER, TUNNELS = 11, range(12, 16)

FONT_DIRS = ("C:/Windows/Fonts", "/usr/share/fonts/truetype/dejavu",
             "/usr/share/fonts/TTF", "/Library/Fonts", "/System/Library/Fonts")
BOLD = ("arialbd.ttf", "DejaVuSans-Bold.ttf", "Arial Bold.ttf", "Helvetica.ttc")
MONO = ("consola.ttf", "DejaVuSansMono.ttf", "Menlo.ttc", "cour.ttf")


def font(names, size):
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, size)
                except OSError:
                    pass
    return ImageFont.load_default()


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, t):
    return tuple(max(0, min(255, round(a[i] + (b[i] - a[i]) * t))) for i in range(3))


def hexpts(cx, cy, w, h):
    """Flat-top: points left and right, flat edges top and bottom. Matches
    drawHexPath() in renderer.js, which uses 0/60/.../300 degrees."""
    rx, ry = w / 2.0, h / 2.0
    return [(cx + rx * math.cos(math.radians(a)), cy + ry * math.sin(math.radians(a)))
            for a in (0, 60, 120, 180, 240, 300)]


def centred(d, text, f, cy, w, fill):
    x0, y0, x1, y1 = d.textbbox((0, 0), text, font=f)
    d.text(((w - (x1 - x0)) / 2 - x0, cy - (y1 - y0) / 2 - y0), text, font=f, fill=fill)


def hex_width_at(dy, half_w):
    """Full width of a flat-top hexagon `dy` above/below its centre.

    Vertices sit at (+-R, 0) and (+-R/2, +-sqrt(3)R/2), so the slanted edges
    take the half-width from R down to R/2 linearly in y: R - |dy|/sqrt(3).
    Text placed off-centre has to fit *this*, not the square."""
    return 2 * max(0.0, half_w - abs(dy) / math.sqrt(3))


def fit(d, text, names, size, avail):
    """Largest font <= `size` from `names` that keeps `text` inside `avail`."""
    while size > 6:
        f = font(names, int(size))
        x0, _, x1, _ = d.textbbox((0, 0), text, font=f)
        if x1 - x0 <= avail:
            return f
        size *= 0.94
    return font(names, 6)


def dashed_rect(d, box, fill, width, dash, gap):
    x0, y0, x1, y1 = box
    for x in range(int(x0), int(x1), dash + gap):
        xe = min(x + dash, x1)
        d.line([x, y0, xe, y0], fill=fill, width=width)
        d.line([x, y1, xe, y1], fill=fill, width=width)
    for y in range(int(y0), int(y1), dash + gap):
        ye = min(y + dash, y1)
        d.line([x0, y, x0, ye], fill=fill, width=width)
        d.line([x1, y, x1, ye], fill=fill, width=width)


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def build(img_name, disp_name, fill, stroke, idx, guide_square=True):
    w, h = W * SS, H * SS
    # Tint towards the terrain's own stroke colour so the tile is still
    # identifiable at a glance, but keep lifting it until dark terrains like
    # Marsh and Nuke Crater have enough contrast to read the filename.
    base = mix(rgb(stroke), (168, 166, 148), 0.45)
    while lum(base) < 120:
        base = mix(base, (196, 194, 176), 0.18)
    ink = mix(rgb(fill), (0, 0, 0), 0.35)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = cy = w / 2

    # The hexagon is INSCRIBED in the square, not filling it: hexpts() puts the
    # vertices on an ellipse, so passing the full square back gives a shape
    # that is w wide and 0.866*w tall -- exactly what drawHexPath() strokes.
    inset = SS * 2
    pts = hexpts(cx, cy, w - inset * 2, h - inset * 2)
    hex_half_w = (w - inset * 2) / 2
    hex_h = (h - inset * 2) * HEX_H_RATIO
    d.polygon(pts, fill=base + (255,))

    # Hazard stripes along the hex's own top and bottom -- unmistakably WIP.
    band = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(band)
    step = SS * 16
    for x in range(-h, w + h, step):
        bd.polygon([(x, 0), (x + step // 2, 0), (x + step // 2 + h, h), (x + h, h)],
                   fill=mix(base, rgb(fill), 0.55) + (255,))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    keep = Image.new("L", (w, h), 0)
    kd = ImageDraw.Draw(keep)
    kd.rectangle([0, cy - hex_h / 2, w, cy - hex_h * 0.37], fill=255)
    kd.rectangle([0, cy + hex_h * 0.37, w, cy + hex_h / 2], fill=255)
    mask = Image.composite(mask, Image.new("L", (w, h), 0), keep)
    im.paste(band, (0, 0), mask)

    d.polygon(pts, outline=ink + (255,), width=int(SS * 2.5))

    # ── The square guide ────────────────────────────────────────────
    # This is the actual drawImage() destination box. Everything outside the
    # hexagon but inside it is dead space on the map, so it shows the artist
    # exactly how much of their canvas will never be seen. Note the squares of
    # neighbouring tiles visibly overlap on the map -- they are meant to: the
    # box is 2*HEX_SZ tall where the hex is only sqrt(3)*HEX_SZ.
    # Pale, not dark: the corners of the square sit over the map's near-black
    # background, so a guide tinted towards the terrain fill vanished entirely
    # at map zoom. It has to out-contrast #080402, not the tile.
    if guide_square:
        guide = mix(base, (255, 248, 230), 0.62) + (190,)
        e = SS * 1
        dashed_rect(d, (e, e, w - e - 1, h - e - 1), guide, int(SS * 1.4), SS * 6, SS * 5)
        arm, lw = SS * 22, int(SS * 2.6)
        for sx, sy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            px = e if sx == 0 else w - e - 1
            py = e if sy == 0 else h - e - 1
            d.line([px, py, px + (arm if sx == 0 else -arm), py], fill=guide, width=lw)
            d.line([px, py, px, py + (arm if sy == 0 else -arm)], fill=guide, width=lw)

    # ── Labels, each shrunk to the hex's width at its own height ────
    dim = mix(ink, base, 0.35) + (255,)
    rows = []
    words = disp_name.split()
    if len(words) > 1:
        rows.append((words[0].upper(), BOLD, SS * 24, -0.139, ink + (255,)))
        rows.append((" ".join(words[1:]).upper(), BOLD, SS * 24, 0.0, ink + (255,)))
    else:
        rows.append((disp_name.upper(), BOLD, SS * 24, -0.069, ink + (255,)))
    rows.append((f"VARIANT {idx}", BOLD, SS * 15, 0.133, dim))
    rows.append((f"hex{img_name}{idx}.png", MONO, SS * 13, 0.254,
                 mix(ink, base, 0.25) + (255,)))
    for text, fam, size, k, col in rows:
        dy = hex_h * k
        avail = hex_width_at(dy, hex_half_w) * 0.86
        centred(d, text, fit(d, text, fam, size, avail), cy + dy, w, col)

    return im.resize((W, H), Image.LANCZOS)


def write_tile(path, im, colors):
    meta = PngImagePlugin.PngInfo()
    meta.add_text(TAG_KEY, TAG)
    data = encode(quantize_rgba(im, colors=colors), pnginfo=meta)
    with open(path, "wb") as fh:
        fh.write(data)
    return len(data)


def is_placeholder(path):
    try:
        with Image.open(path) as im:
            return im.text.get(TAG_KEY) == TAG
    except Exception:
        return False


def existing_max(img_name):
    """Highest N present for hex<Name><N>.png, or -1. Anchored so that
    e.g. 'Marsh' never matches a file belonging to another terrain."""
    pat = re.compile(rf"^hex{re.escape(img_name)}(\d+)\.png$", re.IGNORECASE)
    top = -1
    for f in os.listdir(OUT):
        m = pat.match(f)
        if m:
            top = max(top, int(m.group(1)))
    return top


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="write files (default: dry run)")
    ap.add_argument("--min", type=int, default=4, help="target variants per terrain (default 4)")
    ap.add_argument("--include-river", action="store_true")
    ap.add_argument("--include-tunnels", action="store_true")
    ap.add_argument("--no-guide", dest="guide", action="store_false",
                    help="omit the dashed square / corner brackets; the tile is "
                         "still square, you just stop seeing the draw box")
    ap.add_argument("--regen", action="store_true",
                    help="also redraw tiles this script made earlier (tagged in "
                         "a tEXt chunk); never touches hand-painted art")
    ap.add_argument("--colors", type=int, default=8,
                    help="palette size; flat art needs few, and 16 is visually "
                         "identical to 8 here for 14%% more bytes")
    args = ap.parse_args()

    total = 0
    for t, (img_name, disp_name, fill, stroke) in enumerate(TERRAIN):
        if t == RIVER and not args.include_river:
            continue
        if t in TUNNELS and not args.include_tunnels:
            continue
        have = existing_max(img_name) + 1
        todo = list(range(have, args.min))
        if args.regen:
            todo = sorted(set(todo) | {i for i in range(have)
                                       if is_placeholder(os.path.join(
                                           OUT, f"hex{img_name}{i}.png"))})
        if not todo:
            print(f"  {t:2d} {disp_name:<17} {have} variants - ok")
            continue
        made = []
        for idx in todo:
            path = os.path.join(OUT, f"hex{img_name}{idx}.png")
            verb = "~" if idx < have else "+"
            im = build(img_name, disp_name, fill, stroke, idx, args.guide)
            if args.apply:
                n = write_tile(path, im, args.colors)
            else:
                n = len(encode(quantize_rgba(im, colors=args.colors)))
            made.append(f"{verb}{idx}({n}B)")
            total += n
        grew = f"{have} -> {max(args.min, have)}" if args.min > have else f"{have}"
        print(f"  {t:2d} {disp_name:<17} {grew:<9} " + " ".join(made))

    print(f"\n{total/1024:.1f} KB of placeholders")
    if not args.apply:
        print("dry run -- nothing written. Re-run with --apply.")
    else:
        print("written. Now bump CACHE in data/sw.js so browsers refetch /img/*.")


if __name__ == "__main__":
    main()
