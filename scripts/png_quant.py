#!/usr/bin/env python3
"""Indexed-PNG encoder for the sprite art under data/img/.

Why not just call Image.quantize(): on an RGBA image PIL runs the octree over
the alpha channel too, so Floyd-Steinberg scatters alpha across the whole
sprite -- hexSettlement3 came back with 30k semi-transparent pixels instead of
the ~900-pixel antialiased rim it started with.  So quantize RGB on its own,
snap alpha to a short ramp, and emit one palette slot per (colour, alpha level)
pair that actually occurs.  Typical hex tile lands at ~40-70 slots.

Everything under data/img/ is served byte-for-byte out of the K10's PSRAM
cache (the /img/ route in game-server.hpp) -- the firmware never decodes an
image, so on-device the only thing that matters is byte count, and in the
browser the only thing that matters is that the file stays a real .png.  That
is why this is palette PNG and not WebP: no firmware, engine.js or MIME change
is needed, and PNG stays lossless on the hard ink edges this art is made of.

Used by optimize_art.py and gen_missing_tiles.py.
"""

import io
import math

import numpy as np
from PIL import Image, ImageChops, ImageStat

# Alpha is nearly binary on this art -- a hex tile is a solid silhouette with a
# 1px antialiased rim -- so six levels reproduce the rim without spending a
# palette slot per (colour, alpha) combination that a full 256-level ramp costs.
ALPHA_LEVELS = np.array([0, 48, 100, 152, 204, 255], dtype=np.int16)

# The map background these tiles composite over (renderer.js hex fill floor).
# Quality is judged after compositing, because that is what the player sees.
MAP_BG = (8, 4, 2)

# Colour counts tried in order; the first one that meets the quality bar wins.
LADDER = (24, 32, 48, 64, 96, 128, 160, 192, 256)


def quantize_rgba(im, colors=64, dither=False):
    """RGBA -> P-mode image carrying per-index alpha in info['transparency']."""
    im = im.convert("RGBA")
    w, h = im.size

    d = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    pq = im.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=d)
    pal = np.frombuffer(bytes(pq.getpalette()[: colors * 3]), dtype=np.uint8).reshape(-1, 3)

    idx = np.asarray(pq, dtype=np.int32).ravel()
    alpha = np.asarray(im.getchannel("A"), dtype=np.int16).ravel()
    lvl = np.abs(alpha[:, None] - ALPHA_LEVELS[None, :]).argmin(axis=1).astype(np.int32)

    # Every fully-clear pixel shares one slot, whatever colour hid under it.
    clear = lvl == 0
    key = idx * len(ALPHA_LEVELS) + lvl
    key[clear] = -1

    uniq, inv, counts = np.unique(key, return_inverse=True, return_counts=True)
    if len(uniq) > 256:
        # Too many (colour, alpha) pairs. Keep the commonest and fold the rest
        # onto their fully-opaque twin -- the casualties are rare rim pixels.
        keep = set(uniq[np.argsort(-counts)[:256]].tolist())
        opaque = (key // len(ALPHA_LEVELS)) * len(ALPHA_LEVELS) + (len(ALPHA_LEVELS) - 1)
        key = np.where(np.isin(key, list(keep)), key, opaque)
        key[clear] = -1
        uniq, inv = np.unique(key, return_inverse=True)

    slots = len(uniq)
    rgb = np.zeros((slots, 3), dtype=np.uint8)
    trns = np.zeros(slots, dtype=np.uint8)
    real = uniq >= 0
    rgb[real] = pal[uniq[real] // len(ALPHA_LEVELS)]
    trns[real] = ALPHA_LEVELS[uniq[real] % len(ALPHA_LEVELS)].astype(np.uint8)

    p = Image.new("P", (w, h))
    p.frombytes(inv.astype(np.uint8).tobytes())
    p.putpalette(rgb.tobytes() + b"\x00" * (3 * (256 - slots)))
    p.info["transparency"] = trns.tobytes()
    return p


def to_rgba(p):
    """P + tRNS -> RGBA. Image.convert('RGBA') warns and drops byte-valued
    tRNS, so rebuild it by hand -- the quality check needs the real alpha."""
    idx = np.asarray(p, dtype=np.uint8)
    pal = np.frombuffer(bytes(p.getpalette()), dtype=np.uint8).reshape(-1, 3)
    trns = np.frombuffer(p.info["transparency"], dtype=np.uint8)
    a = np.zeros(256, dtype=np.uint8)
    a[: len(trns)] = trns
    return Image.fromarray(np.dstack([pal[idx], a[idx]]), "RGBA")


def encode(p, pnginfo=None):
    """P-mode image -> PNG bytes, carrying its tRNS chunk (and any tEXt)."""
    b = io.BytesIO()
    p.save(b, "PNG", optimize=True, transparency=p.info["transparency"], pnginfo=pnginfo)
    return b.getvalue()


def _flatten(im, bg=MAP_BG):
    out = Image.new("RGB", im.size, bg)
    im = im.convert("RGBA")
    out.paste(im, (0, 0), im)
    return out


def visible_rmse(orig, cand, bg=MAP_BG):
    """RMSE over pixels the player can actually see.

    Both images are composited over the map background first, and pixels the
    original left transparent are masked out -- otherwise the score is swamped
    by whatever junk RGB happened to sit under the transparent corners, which
    is how a visually perfect requantise scores 87.0.
    """
    mask = orig.convert("RGBA").getchannel("A").point(lambda v: 255 if v > 8 else 0)
    diff = ImageChops.difference(_flatten(orig, bg), _flatten(cand, bg))
    rms = ImageStat.Stat(diff, mask).rms
    return math.sqrt(sum(v * v for v in rms) / len(rms))


def best_encoding(im, max_rmse=3.5, ladder=LADDER, allow_dither=True):
    """Smallest encoding of `im` that stays within `max_rmse`.

    Returns (png_bytes, colors, dithered, rmse). Falls back to the top of the
    ladder (with dithering, which helps the smooth gradients on the title art
    and hurts everywhere else) when nothing clears the bar.
    """
    im = im.convert("RGBA")
    best = None  # lowest-error candidate so far, NOT the smallest: this is the
                 # fallback for art that never clears the bar, where quality is
                 # the whole point of not having stopped at 24 colours.
    for colors in ladder:
        p = quantize_rgba(im, colors)
        e = visible_rmse(im, to_rgba(p))
        data = encode(p)
        if best is None or e < best[3]:
            best = (data, colors, False, e)
        if e <= max_rmse:
            return data, colors, False, e
    if allow_dither:
        p = quantize_rgba(im, ladder[-1], dither=True)
        e = visible_rmse(im, to_rgba(p))
        data = encode(p)
        if e < best[3]:
            return data, ladder[-1], True, e
    return best
