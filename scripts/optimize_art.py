#!/usr/bin/env python3
"""Shrink the art under data/img/ without changing how anything loads it.

Two passes per file:

  1. Cap the long edge of the HEX TILES (default 256px). Tiles are drawn at
     `imgSz = HEX_SZ * 2` (renderer.js renderHexContent) and HEX_SZ tops out
     around 125, so 250 CSS px is the widest a tile is ever painted; 256 covers
     a 2x display at the default zoom. Only ever downscales -- upscaling a
     111px tile to 256 buys no detail and costs bytes. Nothing else is
     resized: wastelandTitle0.png is 240x320 for a reason and the item icons
     and shelters are already well under the cap.
  2. Re-encode as palette PNG with a real alpha ramp (png_quant), picking the
     smallest colour count that stays inside --quality (visible RMSE, measured
     after compositing over the map background).

Filenames, extensions and dimensions-as-far-as-anything-cares are unchanged,
so setupVariantCounts() in game-server.hpp, the /img/ MIME literal, the
`/img/hex<Name><N>.png` paths in engine.js and data/sw.js all keep working
untouched. Nothing here needs a firmware change.

    python scripts/optimize_art.py                 # dry run, prints the table
    python scripts/optimize_art.py --apply         # rewrite in place
    python scripts/optimize_art.py --apply data/img/hexSettlement3.png

Files that are ALREADY palette PNGs are skipped, because re-quantising a
quantised image measures its error against the degraded version rather than
the original and happily shaves another 10% off every pass -- run it four
times and the art is visibly banded. All the source art here is RGB/RGBA, so
mode 'P' is a reliable "this is already my output" marker. --force overrides.

Originals are in git -- `git checkout -- data/img` puts them back.

After applying, bump CACHE in data/sw.js: the service worker caches /img/*
cache-first and forever, so a browser that has already seen the old bytes will
keep serving them.
"""

import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_quant import best_encoding  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "img")

# ui_glyphs.png is a 16px sprite strip recoloured with `source-in` in
# renderer.js _glyphTile(), so only its alpha survives to the screen -- snapping
# that alpha to 6 levels would chew the glyph antialiasing for a saving of
# under a kilobyte. Not worth it.
SKIP = {"ui_glyphs.png"}


def resizable(name):
    """Only the hex-scale art gets the --max-edge cap: those are the files
    whose on-screen size I can bound (imgSz = HEX_SZ * 2). Everything else
    keeps its native dimensions and is merely re-encoded."""
    return name.startswith("hex") or name.startswith("poi_")


def iter_pngs(paths):
    if paths:
        for p in paths:
            yield os.path.abspath(p)
        return
    for dirpath, _dirs, files in os.walk(ROOT):
        for f in sorted(files):
            if f.lower().endswith(".png") and f not in SKIP:
                yield os.path.join(dirpath, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="specific files (default: all of data/img)")
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry run)")
    ap.add_argument("--max-edge", type=int, default=256,
                    help="cap the long edge at this many px (0 = never resize)")
    ap.add_argument("--quality", type=float, default=3.5,
                    help="max visible RMSE; lower is fussier (default 3.5, ~invisible)")
    ap.add_argument("--force", action="store_true",
                    help="re-process already-indexed PNGs (compounds quality loss)")
    args = ap.parse_args()

    rows = []
    skipped = 0
    before = after = 0
    for path in iter_pngs(args.paths):
        if not os.path.exists(path):
            print(f"  missing: {path}", file=sys.stderr)
            continue
        name = os.path.relpath(path, ROOT).replace("\\", "/")
        old = os.path.getsize(path)
        src = Image.open(path)
        if src.mode == "P" and not args.force:
            before += old
            after += old
            skipped += 1
            continue
        im = src.convert("RGBA")
        w, h = im.size

        if args.max_edge and resizable(os.path.basename(path)) and max(w, h) > args.max_edge:
            s = args.max_edge / max(w, h)
            nw, nh = max(1, round(w * s)), max(1, round(h * s))
            im = im.resize((nw, nh), Image.LANCZOS)
        nw, nh = im.size

        data, colors, dither, rmse = best_encoding(im, max_rmse=args.quality)
        new = len(data)
        shrank = new < old
        if shrank and args.apply:
            with open(path, "wb") as fh:
                fh.write(data)

        before += old
        after += new if shrank else old
        dim = f"{w}x{h}" + (f"->{nw}x{nh}" if (nw, nh) != (w, h) else "")
        rows.append((name, dim, old, new, colors, dither, rmse, shrank))

    rows.sort(key=lambda r: r[2] - r[3], reverse=True)
    print(f"{'file':<34}{'dims':<16}{'before':>9}{'after':>9}{'save':>7}  {'col':>4} {'rmse':>5}")
    for name, dim, old, new, colors, dither, rmse, shrank in rows:
        pct = f"{(old - new) * 100 // old}%" if shrank and old else "-"
        flag = "d" if dither else " "
        print(f"{name:<34}{dim:<16}{old:>9}{new:>9}{pct:>7}  {colors:>4}{flag}{rmse:>5.1f}")
    pct = (before - after) * 100 // before if before else 0
    note = f", {skipped} already indexed (skipped)" if skipped else ""
    print(f"\n{len(rows)} files{note}   {before/1024:.0f} KB -> {after/1024:.0f} KB   ({pct}% smaller)")
    if not args.apply:
        print("dry run -- nothing written. Re-run with --apply.")
    else:
        print("written. Now bump CACHE in data/sw.js so browsers refetch /img/*.")


if __name__ == "__main__":
    main()
