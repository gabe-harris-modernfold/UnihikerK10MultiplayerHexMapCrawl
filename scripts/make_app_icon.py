"""App icon: the W out of the title art (data/img/wastelandTitle0.png).

Cuts the W (halo and all) away from the sky and the A beside it, sets it on
the game's near-black, and writes the icons the web client links to
(data/index.html, data/app.webmanifest). Scaled in whole pixels, so it stays
the same pixel art as the title rather than a blurred enlargement.

    python scripts/make_app_icon.py [--preview out.png]
"""
import os, sys
from collections import deque
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'data', 'img', 'wastelandTitle0.png')
OUT = os.path.join(ROOT, 'data', 'img')
BG = (10, 7, 1)                 # #0a0701, the page's own background
BOX = (4, 10, 64, 74)           # around the W in the title art (240x320)
PAD = 3                         # art pixels of background round the W


def right_edge(y):
    """The cut between the W's right arm and the A, in title-art x: the arm's
    own outer edge, straight down to y 40, then leaning in to its point."""
    if y <= 40: return 56
    return 56 - (y - 40) * 6 // 22


def is_sky(c):
    r, g, b = c
    lum = (r * 299 + g * 587 + b * 114) // 1000
    orange = r > 150 and g > 70 and b < 90 and r - b > 90
    purple = r > g + 25 and b > g + 25          # the logo's shadow band, under the letters
    return (lum < 105 and not orange) or purple


def extract():
    im = Image.open(SRC).convert('RGBA').convert('RGB')
    x0, y0, x1, y1 = BOX
    w, h = x1 - x0, y1 - y0
    px = [[im.getpixel((x0 + x, y0 + y)) for x in range(w)] for y in range(h)]
    keep = [[(x0 + x) <= right_edge(y0 + y) for x in range(w)] for y in range(h)]
    # The sky: everything dark joined to the edge of the box.
    sky = [[False] * w for _ in range(h)]
    q = deque()
    for x in range(w):
        for y in (0, h - 1): q.append((x, y))
    for y in range(h):
        for x in (0, w - 1): q.append((x, y))
    while q:
        x, y = q.popleft()
        if not (0 <= x < w and 0 <= y < h) or sky[y][x]: continue
        if keep[y][x] and not is_sky(px[y][x]): continue
        sky[y][x] = True
        q.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    # Only the W itself: the largest piece left (no specks of sky, no foot of A).
    on = [[keep[y][x] and not sky[y][x] for x in range(w)] for y in range(h)]
    seen, best = set(), []
    for y in range(h):
        for x in range(w):
            if not on[y][x] or (x, y) in seen: continue
            comp, q = [], deque([(x, y)])
            seen.add((x, y))
            while q:
                cx, cy = q.popleft(); comp.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and on[ny][nx] and (nx, ny) not in seen:
                        seen.add((nx, ny)); q.append((nx, ny))
            if len(comp) > len(best): best = comp
    # The letters' own paint stays; their edge is drawn fresh, the logo's way
    # (a dark line, an orange halo hot inside and darker out, a dark rim), the
    # same all round -- the title's own halo is too broken up by the sky and
    # the cut to lift cleanly at this size.
    RINGS = [(34, 16, 8), (246, 150, 62), (214, 108, 44), (40, 16, 6)]
    out = {(x, y): px[y][x] for (x, y) in best}
    edge = set(best)
    for col in RINGS:
        ring = set()
        for (x, y) in edge:
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1), (x + 1, y + 1), (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1)):
                if (nx, ny) not in out: ring.add((nx, ny))
        for q_ in ring: out[q_] = col
        edge = ring
    xs = [x for x, _ in out]; ys = [y for _, y in out]
    side = max(max(xs) - min(xs), max(ys) - min(ys)) + 1 + 2 * PAD
    art = Image.new('RGB', (side, side), BG)
    ox = (side - (max(xs) - min(xs) + 1)) // 2 - min(xs)
    oy = (side - (max(ys) - min(ys) + 1)) // 2 - min(ys)
    for (x, y), c in out.items(): art.putpixel((ox + x, oy + y), c)
    return art


def main():
    art = extract()
    A = art.width
    if '--preview' in sys.argv:
        art.resize((A * 6, A * 6), Image.NEAREST).save(sys.argv[sys.argv.index('--preview') + 1])
        return
    for n in (192, 512):                         # the manifest's: whole-pixel steps, centred
        k = n // A
        big = art.resize((A * k, A * k), Image.NEAREST)
        out = Image.new('RGB', (n, n), BG)
        out.paste(big, ((n - big.width) // 2, (n - big.height) // 2))
        out.save(os.path.join(OUT, f'icon-{n}.png'), optimize=True)
    big = art.resize((A * 8, A * 8), Image.NEAREST)
    big.resize((180, 180), Image.LANCZOS).save(os.path.join(OUT, 'icon-180.png'), optimize=True)   # apple-touch-icon
    big.resize((32, 32), Image.LANCZOS).save(os.path.join(OUT, 'icon-32.png'), optimize=True)      # the tab's favicon
    print('icons written to', OUT)


if __name__ == '__main__':
    main()
