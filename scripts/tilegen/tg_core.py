"""Core raster engine for the hex tile generator.

Everything is drawn on a supersampled float canvas (premultiplied RGB + alpha)
and box-filtered down at the end, so every edge is antialiased without PIL
needing to know how. Shapes are rasterised into bbox-local 'L' masks with
ImageDraw and composited with numpy, which keeps a tile to a second or two.

Coordinates everywhere are FINAL tile pixels (floats). The supersample factor
only exists inside this module.

Tile geometry (see docs in build_tiles.py): a CELL_W x CELL_H cell, the
flat-top hexagon of radius R centred at (CX, CY), with HEADROOM px above the
hex's top edge for props that stand up into the hex behind.
"""
import math
import numpy as np
from PIL import Image, ImageDraw

S = 4                       # supersample factor
CELL_W, CELL_H = 224, 272
R = 112.0                   # hex radius in final px (tile width = 2R)
HALF_H = R * math.sqrt(3) / 2   # 96.99
CX = 112.0
CY = CELL_H - 8 - HALF_H        # hex centre: 8 px of bottom margin
HEX_TOP = CY - HALF_H           # ~70: everything above is headroom
HEX_BOT = CY + HALF_H

SQ3_2 = math.sqrt(3) / 2


def hex_pts(cx=CX, cy=CY, r=R):
    return [(cx + r * math.cos(math.pi / 3 * i), cy + r * math.sin(math.pi / 3 * i)) for i in range(6)]


def hex_sdf(x, y, cx=CX, cy=CY, r=R):
    """Signed distance (px, negative inside) to a flat-top hexagon. Vectorised."""
    dx = np.abs(x - cx)
    dy = np.abs(y - cy)
    return np.maximum(dy - SQ3_2 * r, SQ3_2 * dx + 0.5 * dy - SQ3_2 * r)


def in_hex(x, y, margin=0.0):
    return hex_sdf(np.asarray(x, np.float32), np.asarray(y, np.float32)) < -margin


# ── colour ──────────────────────────────────────────────────────────────
def C(h, a=None):
    """'#rrggbb' or (r,g,b) 0-255 -> float rgb tuple 0..1."""
    if isinstance(h, str):
        h = h.lstrip('#')
        t = tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    else:
        t = tuple(v / 255.0 if v > 1.0 else float(v) for v in h[:3])
    return t


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def shade(c, k):
    """k<1 darkens toward a cool purple-black, k>1 lightens toward warm cream.
    Painted-background shadows go cool and lights go warm; pure black/white
    mixing is what makes procedural colour look like maths."""
    if k < 1:
        return mix(c, (0.09, 0.06, 0.11), 1 - k)
    return mix(c, (1.0, 0.95, 0.82), min(1.0, k - 1))


# ── noise ───────────────────────────────────────────────────────────────
class Noise:
    def __init__(self, seed):
        rng = np.random.default_rng(seed)
        self.g = rng.random((256, 256)).astype(np.float32)

    def val(self, x, y):
        x = np.asarray(x, np.float32)
        y = np.asarray(y, np.float32)
        xi = np.floor(x).astype(np.int64)
        yi = np.floor(y).astype(np.int64)
        xf = x - xi
        yf = y - yi
        u = xf * xf * (3 - 2 * xf)
        v = yf * yf * (3 - 2 * yf)
        g = self.g
        a = g[yi & 255, xi & 255]
        b = g[yi & 255, (xi + 1) & 255]
        c = g[(yi + 1) & 255, xi & 255]
        d = g[(yi + 1) & 255, (xi + 1) & 255]
        return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v

    def fbm(self, x, y, octaves=4, lac=2.03, gain=0.5):
        amp, tot, norm = 1.0, 0.0, 0.0
        x = np.asarray(x, np.float32)
        y = np.asarray(y, np.float32)
        for o in range(octaves):
            tot = tot + self.val(x + o * 17.13, y + o * 31.7) * amp
            norm += amp
            x = x * lac
            y = y * lac
            amp *= gain
        return tot / norm

    def n1(self, t, row=0.0):
        return float(self.val(np.float32(t), np.float32(row + 0.5)))


# ── geometry helpers ───────────────────────────────────────────────────
def wobble(pts, noise, amp=0.6, seg=6.0, closed=True, freq=0.35, row=0.0):
    """Subdivide a polyline and push each point along its normal by smooth
    noise: hand-inked instead of ruled."""
    if amp <= 0:
        return list(pts)
    out = []
    n = len(pts)
    m = n if closed else n - 1
    acc = row * 13.0
    for i in range(m):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        L = math.hypot(x1 - x0, y1 - y0)
        k = max(1, int(L / seg))
        nx, ny = (-(y1 - y0) / L, (x1 - x0) / L) if L > 1e-6 else (0.0, 0.0)
        for j in range(k):
            t = j / k
            d = (noise.n1(acc * freq, row) - 0.5) * 2 * amp
            out.append((x0 + (x1 - x0) * t + nx * d, y0 + (y1 - y0) * t + ny * d))
            acc += L / k
    if not closed:
        out.append(pts[-1])
    return out


def blob_pts(cx, cy, rx, ry, noise, lumps=0.22, n=28, phase=0.0, freq=1.6):
    pts = []
    for i in range(n):
        a = math.tau * i / n
        k = 1 + lumps * (noise.n1(phase + math.cos(a) * freq + 7, math.sin(a) * freq + 3) - 0.5) * 2
        pts.append((cx + math.cos(a) * rx * k, cy + math.sin(a) * ry * k))
    return pts


def ellipse_pts(cx, cy, rx, ry, n=32, a0=0.0, a1=math.tau):
    return [(cx + math.cos(a0 + (a1 - a0) * i / n) * rx, cy + math.sin(a0 + (a1 - a0) * i / n) * ry)
            for i in range(n + 1 if a1 - a0 < math.tau - 1e-6 else n)]


def bbox_of(shapes, pad=0.0):
    xs, ys = [], []
    for sh in shapes:
        k = sh[0]
        if k in ('poly', 'line'):
            w = sh[2] / 2 if k == 'line' else 0
            for x, y in sh[1]:
                xs += [x - w, x + w]
                ys += [y - w, y + w]
        elif k == 'ellipse':
            _, cx, cy, rx, ry = sh
            xs += [cx - rx, cx + rx]
            ys += [cy - ry, cy + ry]
    if not xs:
        return None
    return (min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad)


# ── canvas ──────────────────────────────────────────────────────────────
class Canvas:
    def __init__(self, w=CELL_W, h=CELL_H, s=S):
        self.w, self.h, self.s = w, h, s
        self.pm = np.zeros((h * s, w * s, 3), np.float32)
        self.a = np.zeros((h * s, w * s), np.float32)
        self.ground_clip = False     # multiply every composite by the hex mask
        self.ground_grow = 1.2       # px the ground bleeds past the hex edge
        self.zbuf = None             # relief depth: ground y (final px) of the terrain at each pixel
        self.zlimit = None           # ground y of the prop being drawn; terrain nearer than this hides it
        self.H = None

    # ---- rasterisation ----
    def _box(self, bb):
        s = self.s
        x0 = max(0, int(math.floor(bb[0] * s)) - 2)
        y0 = max(0, int(math.floor(bb[1] * s)) - 2)
        x1 = min(self.w * s, int(math.ceil(bb[2] * s)) + 2)
        y1 = min(self.h * s, int(math.ceil(bb[3] * s)) + 2)
        if x1 <= x0 or y1 <= y0:
            return None
        return x0, y0, x1, y1

    def _raster(self, shapes, box, grow=0.0):
        x0, y0, x1, y1 = box
        s = self.s
        im = Image.new('L', (x1 - x0, y1 - y0), 0)
        d = ImageDraw.Draw(im)
        T = lambda p: (p[0] * s - x0, p[1] * s - y0)
        g = grow * s
        for sh in shapes:
            k = sh[0]
            if k == 'poly':
                pts = [T(p) for p in sh[1]]
                if len(pts) < 3:
                    continue
                d.polygon(pts, fill=255)
                if g > 0.5:
                    d.line(pts + [pts[0]], fill=255, width=max(1, int(round(g * 2))), joint='curve')
                    for (px, py) in pts:
                        d.ellipse((px - g, py - g, px + g, py + g), fill=255)
            elif k == 'ellipse':
                _, cx, cy, rx, ry = sh
                cx, cy = T((cx, cy))
                rx = rx * s + g
                ry = ry * s + g
                d.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=255)
            elif k == 'line':
                pts = [T(p) for p in sh[1]]
                wpx = sh[2] * s + g * 2
                if len(pts) >= 2:
                    d.line(pts, fill=255, width=max(1, int(round(wpx))), joint='curve')
                r = wpx / 2
                for (px, py) in (pts[0], pts[-1]):
                    d.ellipse((px - r, py - r, px + r, py + r), fill=255)
        return np.asarray(im, np.float32) * (1.0 / 255.0)

    def _hexmask(self, box, grow):
        x0, y0, x1, y1 = box
        s = self.s
        xs = (np.arange(x0, x1, dtype=np.float32) + 0.5) / s
        ys = (np.arange(y0, y1, dtype=np.float32) + 0.5) / s
        X, Y = np.meshgrid(xs, ys)
        return (hex_sdf(X, Y) < grow).astype(np.float32)

    def mask(self, shapes, grow=0.0, bb=None):
        bb = bb or bbox_of(shapes, grow + 1)
        if bb is None:
            return None
        box = self._box(bb)
        if box is None:
            return None
        return box, self._raster(shapes, box, grow)

    # ---- compositing ----
    def _comp(self, box, m, color, alpha, mode):
        x0, y0, x1, y1 = box
        k = m * alpha
        if self.ground_clip:
            k = k * self._hexmask(box, self.ground_grow)
        if self.zbuf is not None and self.zlimit is not None:
            k = k * (self.zbuf[y0:y1, x0:x1] <= self.zlimit + 2.5)
        pm = self.pm[y0:y1, x0:x1]
        a = self.a[y0:y1, x0:x1]
        col = np.asarray(color, np.float32)
        kk = k[..., None]
        if mode == 'normal':
            pm[...] = pm * (1 - kk) + col * kk
            a[...] = a * (1 - k) + k
        elif mode == 'multiply':
            pm[...] = pm * (1 - kk * (1 - col))
        elif mode == 'screen':
            pm[...] = pm + col * kk * a[..., None] - pm * col * kk
        elif mode == 'add':
            pm[...] = np.minimum(pm + col * kk, a[..., None] + col * kk)
            a[...] = np.minimum(1.0, a + k * max(col) * 0.0)
        elif mode == 'glow':   # adds light AND alpha, for halos over transparency
            pm[...] = pm + col * kk
            a[...] = a + k * (1 - a)
            np.minimum(pm, a[..., None], out=pm)
        elif mode == 'erase':
            pm[...] = pm * (1 - kk)
            a[...] = a * (1 - k)

    def fill(self, shapes, color, alpha=1.0, mode='normal', grow=0.0, clip=None, clip_grow=0.0, sub=None):
        if not shapes:
            return
        if isinstance(shapes, tuple):
            shapes = [shapes]
        bb = bbox_of(shapes, grow + 1)
        if bb is None:
            return
        box = self._box(bb)
        if box is None:
            return
        m = self._raster(shapes, box, grow)
        if clip is not None:
            m = m * self._raster(clip if isinstance(clip, list) else [clip], box, clip_grow)
        if sub is not None:
            m = m * (1 - self._raster(sub if isinstance(sub, list) else [sub], box, 0.0))
        self._comp(box, m, color, alpha, mode)

    def fill_mask(self, box, m, color, alpha=1.0, mode='normal'):
        self._comp(box, m, color, alpha, mode)

    def fill_array(self, rgb, mask=None, alpha=1.0):
        """Composite a full-res final-pixel RGB array (h, w, 3) through the hex
        (if ground_clip) or an optional (h, w) mask. Upsampled nearest to the
        supersampled grid; the ground is soft enough that nothing shows."""
        s = self.s
        big = np.repeat(np.repeat(rgb.astype(np.float32), s, 0), s, 1)
        if mask is None:
            m = np.ones((self.h * s, self.w * s), np.float32)
        else:
            m = np.repeat(np.repeat(mask.astype(np.float32), s, 0), s, 1)
        k = m * alpha
        if self.ground_clip:
            k = k * self._hexmask((0, 0, self.w * s, self.h * s), self.ground_grow)
        kk = k[..., None]
        self.pm[...] = self.pm * (1 - kk) + big * kk
        self.a[...] = self.a * (1 - k) + k

    # ---- output ----
    def result(self):
        s = self.s
        h, w = self.h, self.w
        pm = self.pm.reshape(h, s, w, s, 3).mean(axis=(1, 3))
        a = self.a.reshape(h, s, w, s).mean(axis=(1, 3))
        rgb = np.where(a[..., None] > 1e-4, pm / np.maximum(a[..., None], 1e-4), 0)
        out = np.dstack([np.clip(rgb, 0, 1), np.clip(a, 0, 1)])
        return Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA')


def final_grid():
    ys, xs = np.mgrid[0:CELL_H, 0:CELL_W].astype(np.float32)
    return xs + 0.5, ys + 0.5
