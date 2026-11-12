"""The map's colour grade ("Amber Dusk", picked 2026-11-05), applied by
hex_sheets.py build as it encodes each page. The master sheets are never
touched: the grade is a fixed per-sheet transform in CIELAB, so it can be
tuned here (or skipped with `build --no-grade`) and every tile added later
gets it too.

    python scripts/palette_grade.py measure    # (re)freeze the anchors from the master sheets

The look (docs/hex-tile-art.md "Palette"):
  - one contrast: every sheet's darkest 2% lands on BLACK and brightest 2% on
    WHITE, whatever the painting's own range was;
  - a lane per terrain: its median lightness, hue and chroma, so terrains
    separate by value first (they still read in greyscale) and hue second;
  - one light: the surface gets umber shadows and amber highlights, the
    underground blue-violet shadows and a lower white point;
  - Ash Dunes and Nuke Crater stay dead grey -- they move in lightness only;
  - ink (the darkest lines) keeps its colour, and accents well off a sheet's
    ground colour (crater glow, rust, signage) keep their full strength.

Anchors: a sheet's own L* percentiles (2, 50, 98) and median a*, b*, measured
once over every painted pixel and stored in art/hex-sheets/palette-anchors.json.
They are frozen on purpose: adding a tile must not regrade the other tiles of
its terrain. Re-run `measure` only when a sheet is repainted wholesale.

Needs numpy + Pillow.
"""
import json
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEETS = os.path.join(ROOT, 'art', 'hex-sheets')
ANCHORS = os.path.join(SHEETS, 'palette-anchors.json')

BLACK, WHITE = 7, 86     # L* of every sheet's 2nd / 98th percentile
LOFF = -3                # shifts every lane's lightness: dusk sits a little low
# Lab a*/b* nudges. shadow and highlight fade in below L* 45 / above 58; mid
# covers everything but the ink. The surface leans warm, the underground cool.
SURFACE = {'shadow': (6, 6), 'highlight': (4, 12), 'mid': (2, 7), 'white_drop': 0}
UNDER = {'shadow': (3, -8), 'highlight': (1, -3), 'mid': (0, 0), 'white_drop': 16}

# Lanes, by anchor key: (median L*, hue deg, chroma, keep, light).
#   keep  how much of the sheet's own colour variation survives (1 = all);
#         below 1 mutes it toward grey -- accents excepted
#   light how strongly SURFACE / UNDER applies
LANES = {
    'hexOpenScrub':        (66, 92, 18, 1, 1),     # sage-khaki: the base colour of the world
    'hexAshDunes':         (74, 75, 3, .5, .5),    # lightest; dead grey
    'hexRustForest':       (34, 88, 10, 1, 1),     # olive-umber, lifted off black ("rust" is only a name)
    'hexMarsh':            (44, 108, 13, 1, 1),    # olive bile
    'hexFloodedDistrict':  (41, 150, 10, 1, 1),    # murky green water
    'hexGlassFields':      (60, 250, 3, .6, .6),   # grey with a faint cold cast: glass, not ash
    'hexRidge':            (64, 78, 24, 1, 1),     # sun-baked ochre
    'hexMountain':         (49, 45, 7, 1, 1),      # dusty mauve-umber stone
    'hexNukeCrater':       (52, 75, 4, .5, .5),    # mid grey, darker than the dunes; dead
    'hexRiverChannel':     (46, 175, 11, 1, 1),    # teal
    'tunnel':              (35, 275, 8, 1, 1),     # cold blue-violet steel
}
# Sheets painted on Open Scrub ground take Open Scrub's transform, so the
# ground matches across the seam with a plain scrub hex.
ON_SCRUB = ['hexBrokenUrban', 'hexSettlement', 'hexBunkerEntrance', 'hexVentShaft']
# The tunnel board is graded as one: one anchor across its three sheets.
TUNNEL = ['tunCorridor', 'tunRoom', 'tunFixture']


def anchor_key(stem):
    """The anchor/lane a sheet is graded by, or None (sprites stay as painted)."""
    if stem in TUNNEL:
        return 'tunnel'
    if stem in ON_SCRUB:
        return 'hexOpenScrub'
    return stem if stem in LANES else None


# ── sRGB <-> CIELAB (D65) ─────────────────────────────────────────
_M = np.array([[0.4124564, 0.3575761, 0.1804375],
               [0.2126729, 0.7151522, 0.0721750],
               [0.0193339, 0.1191920, 0.9503041]])
_MI = np.linalg.inv(_M)
_WP = np.array([0.95047, 1.0, 1.08883])
_D = 6 / 29


def to_lab(rgb8):
    c = rgb8.astype(np.float64) / 255
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    t = (lin @ _M.T) / _WP
    f = np.where(t > _D ** 3, np.cbrt(t), t / (3 * _D * _D) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def to_rgb8(lab):
    fy = (lab[..., 0] + 16) / 116
    f = np.stack([fy + lab[..., 1] / 500, fy, fy - lab[..., 2] / 200], -1)
    t = np.where(f > _D, f ** 3, 3 * _D * _D * (f - 4 / 29)) * _WP
    lin = np.clip(t @ _MI.T, 0, 1)
    c = np.where(lin <= 0.0031308, 12.92 * lin, 1.055 * lin ** (1 / 2.4) - 0.055)
    return np.round(c * 255).astype(np.uint8)


# ── the grade ─────────────────────────────────────────────────────
def _painted(a):
    return to_lab(a[..., :3][a[..., 3] > 200])


def measure_pixels(lab):
    return {'L': [round(float(v), 2) for v in np.percentile(lab[:, 0], [2, 50, 98])],
            'ab': [round(float(v), 2) for v in np.median(lab[:, 1:], 0)]}


_anchors = None


def grade(stem, rgba):
    """A sheet's RGBA array, graded -- or None when the sheet isn't graded."""
    global _anchors
    key = anchor_key(stem)
    if key is None:
        return None
    if _anchors is None:
        _anchors = json.load(open(ANCHORS)) if os.path.exists(ANCHORS) else {}
    if key not in _anchors:
        sys.exit(f'{stem}: no palette anchor "{key}" -- run python scripts/palette_grade.py measure')
    anc, (tl, th, tc, keep, light) = _anchors[key], LANES[key]
    look = UNDER if key == 'tunnel' else SURFACE
    white = WHITE - look['white_drop']
    out = rgba.copy()
    m = rgba[..., 3] > 0
    lab = to_lab(rgba[..., :3][m])
    p2, p50, p98 = anc['L']
    ma, mb = anc['ab']
    mid = float(np.clip(tl + LOFF, BLACK + 8, white - 8))
    L = np.interp(lab[:, 0], [0, p2, p50, p98, 100], [0, BLACK, mid, white, 100])
    ink = np.clip((L - BLACK) / 22, 0, 1)   # 0 on the ink: it keeps its colour
    crel = np.hypot(lab[:, 1] - ma, lab[:, 2] - mb)
    k = keep + (1 - keep) * np.clip((crel - 10) / 10, 0, 1)   # accents keep full strength
    ta, tb = tc * np.cos(np.radians(th)), tc * np.sin(np.radians(th))
    a = lab[:, 1] * (1 - ink) + ((lab[:, 1] - ma) * k + ta) * ink
    b = lab[:, 2] * (1 - ink) + ((lab[:, 2] - mb) * k + tb) * ink
    sh, hi = np.clip((45 - L) / 35, 0, 1), np.clip((L - 58) / 30, 0, 1)
    a += light * (look['shadow'][0] * sh + look['highlight'][0] * hi + look['mid'][0] * ink)
    b += light * (look['shadow'][1] * sh + look['highlight'][1] * hi + look['mid'][1] * ink)
    out[..., :3][m] = to_rgb8(np.stack([np.clip(L, 0, 100), a, b], -1))
    return out


def cmd_measure(sheets=SHEETS):
    groups = {}
    for name in sorted(os.listdir(sheets)):
        stem, ext = os.path.splitext(name)
        key = anchor_key(stem)
        if ext == '.png' and key is not None and stem not in ON_SCRUB:
            groups.setdefault(key, []).append(stem)
    anchors = {}
    for key, stems in sorted(groups.items()):
        lab = np.concatenate([_painted(np.asarray(Image.open(os.path.join(sheets, f'{s}.png')).convert('RGBA')))
                              for s in stems])
        anchors[key] = measure_pixels(lab)
        print(f'  {key}: L* {anchors[key]["L"]}  a*b* {anchors[key]["ab"]}  ({", ".join(stems)})')
    missing = sorted(set(LANES) - set(anchors))
    if missing:
        sys.exit(f'no sheet for lane(s) {missing}')
    with open(ANCHORS, 'w', newline='\n') as f:
        json.dump(anchors, f, indent=1)
        f.write('\n')
    print(f'{len(anchors)} anchors -> {os.path.relpath(ANCHORS, ROOT)}')


if __name__ == '__main__':
    if sys.argv[1:] != ['measure']:
        sys.exit(__doc__)
    cmd_measure()
