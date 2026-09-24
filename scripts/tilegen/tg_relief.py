"""Relief: turn the flat painted ground into a 3D landform.

The ground is painted flat first (texture, decals, roads, water...). A
heightfield H(x, y) (final px, 0 at the hex rim) then lifts it: in the tile's
oblique projection a ground point (x, y) lands on screen at (x, y - H * FZ), x
unchanged, so every screen column is independent. For a screen pixel the
visible ground point is the FRONTMOST one whose projection reaches it:

    y*(ys) = max{ y : min_{y' >= y} (y' - k*H(y')) <= ys }

which is a searchsorted on the running minimum from the front. That gives
correct self-occlusion (a near ridge hides the valley behind it, a crater's
near lip hides its own inner wall) for the price of one searchsorted per
column. The visible ground point also fills a depth buffer, so props drawn
afterwards are hidden behind hills they stand behind.

Shading is cel: n.L quantised into four bands, cool purple shadows, warm
lights, ink hatching in the deepest band, and an ink line wherever y* jumps
(an occlusion edge) -- which is exactly where a comic artist puts one.
"""
import math
import numpy as np
from tg_core import C, CELL_W, CELL_H, S, hex_sdf, CX, CY, R, HALF_H
from tg_props import FZ, LIGHT, INK

SHADOW_TINT = np.array([0.20, 0.14, 0.30], np.float32)
LIGHT_TINT = np.array([1.00, 0.93, 0.78], np.float32)


def grid_ground():
    """Supersampled ground coordinates in final px."""
    h, w = CELL_H * S, CELL_W * S
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    return (xs + 0.5) / S, (ys + 0.5) / S


def rim_falloff(X, Y, inner=14.0, outer=2.0):
    """1 inside, easing to 0 toward the hex edge, so relief meets its
    neighbours on flat ground."""
    d = -hex_sdf(X, Y)
    t = np.clip((d - outer) / max(1e-3, inner - outer), 0, 1)
    return t * t * (3 - 2 * t)


def relief(cv, H, bands=(0.78, 0.52, 0.28), mult=(1.10, 0.97, 0.78, 0.60), ink=0.95, hatch=0.30,
           ink_thresh=3.0, cavity=0.0, light=None, strata=0.0, strata_step=7.0, strata_min_slope=0.8,
           zmin=3.0):
    """Re-project cv's flat ground through heightfield H (supersampled array,
    final px). Writes cv.zbuf (ground y in final px, -1e9 where empty)."""
    s = S
    h, w = H.shape
    k = FZ * s                               # height px -> supersampled screen px
    L = np.asarray(light or LIGHT, np.float32)
    # normals from the height gradient (world y is north = -screen y)
    gy, gx = np.gradient(H * s)              # d/dy_s, d/dx per supersampled px -> dimensionless
    nx, ny, nz = -gx, gy, np.ones_like(H)
    nrm = np.sqrt(nx * nx + ny * ny + nz * nz)
    b = (nx * L[0] + ny * L[1] + nz * L[2]) / nrm
    # soft-edged cel bands -> brightness multiplier m and a tint weight
    def step(v, e, wdt=0.03):
        return np.clip((v - e) / wdt + 0.5, 0, 1)
    m = np.full(H.shape, mult[3], np.float32)
    m = m + (mult[2] - mult[3]) * step(b, bands[2])
    m = m + (mult[1] - mult[2]) * step(b, bands[1])
    m = m + (mult[0] - mult[1]) * step(b, bands[0])
    if cavity > 0:
        from numpy.lib.stride_tricks import sliding_window_view  # noqa: F401
        blur = H.copy()
        for _ in range(3):
            blur = (np.roll(blur, 6, 0) + np.roll(blur, -6, 0) + np.roll(blur, 6, 1) + np.roll(blur, -6, 1) + blur) / 5
        cav = np.clip((blur - H) * 0.08, 0, 0.35) * cavity
        m = m * (1 - cav)
    pm0, a0 = cv.pm.copy(), cv.a.copy()
    ysrc = np.arange(h, dtype=np.float32)
    proj = ysrc[:, None] - H * k             # screen y of each ground row, per column
    # running minimum from the front (bottom) row upward
    runmin = np.minimum.accumulate(proj[::-1], axis=0)[::-1]
    out_pm = np.zeros_like(pm0)
    out_a = np.zeros_like(a0)
    ystar = np.full((h, w), -1, np.int64)
    qs = np.arange(h, dtype=np.float32) + 0.5
    for x in range(w):
        col = runmin[:, x]
        # runmin is non-decreasing with y; y* = last index with runmin <= ys
        idx = np.searchsorted(col, qs, side='right') - 1
        ystar[:, x] = idx
    valid = ystar >= 0
    yi = np.clip(ystar, 0, h - 1)
    xi = np.broadcast_to(np.arange(w)[None, :], (h, w))
    src_pm = pm0[yi, xi]
    src_a = a0[yi, xi]
    mm = m[yi, xi]
    # colour: multiply, and tint toward cool shadow / warm light
    shadow_w = np.clip((1.0 - mm) / 0.4, 0, 1)[..., None]
    light_w = np.clip((mm - 1.0) / 0.1, 0, 1)[..., None]
    col = src_pm * mm[..., None]
    col = col * (1 - 0.35 * shadow_w) + SHADOW_TINT * src_a[..., None] * mm[..., None] * 0.35 * shadow_w
    col = col * (1 - 0.25 * light_w) + LIGHT_TINT * src_a[..., None] * 0.25 * light_w * np.minimum(1.0, mm[..., None])
    out_pm[valid] = col[valid]
    out_a[valid] = src_a[valid]
    # strata: iso-height lines on steep ground read as rock bedding in the
    # oblique view (and give cliffs crisp detail the stretched texture lacks)
    if strata > 0:
        slope = np.sqrt(gx * gx + gy * gy)
        Hv = H[yi, xi]
        sv = slope[yi, xi]
        ph = (Hv / strata_step) % 1.0
        line = np.clip(1 - np.abs(ph - 0.5) / 0.09, 0, 1)
        line = line * np.clip((sv - strata_min_slope) / 0.6, 0, 1) * strata * out_a
        out_pm = out_pm * (1 - line[..., None]) + np.asarray(INK, np.float32) * line[..., None]
    # hatching in the deepest band (screen-space diagonals)
    if hatch > 0:
        Ys, Xs = np.mgrid[0:h, 0:w]
        lines = (((Xs + Ys) % (5 * s)) < s * 0.55).astype(np.float32)
        deep = np.clip((mult[2] - mm) / 0.08, 0, 1) * lines * hatch * out_a
        out_pm = out_pm * (1 - deep[..., None]) + np.asarray(INK, np.float32) * deep[..., None]
    # occlusion ink: where the visible ground row jumps between screen rows
    if ink > 0:
        ys_f = ystar.astype(np.float32)
        jump = np.zeros((h, w), np.float32)
        d = ys_f[1:, :] - ys_f[:-1, :]
        jump[1:, :] = (d > ink_thresh * s).astype(np.float32)
        # silhouette against empty headroom
        top = np.zeros((h, w), np.float32)
        top[1:, :] = ((ystar[1:, :] >= 0) & (ystar[:-1, :] < 0)).astype(np.float32)
        e = np.maximum(jump, top)
        # thicken ~1.3 px (final), downward into the near surface and a little sideways
        thick = e.copy()
        for dy in range(1, int(1.3 * s)):
            thick[dy:, :] = np.maximum(thick[dy:, :], e[:-dy, :])
        thick[:, 1:] = np.maximum(thick[:, 1:], thick[:, :-1] * 0.6)
        thick[:, :-1] = np.maximum(thick[:, :-1], thick[:, 1:] * 0.6)
        kk = np.clip(thick, 0, 1) * ink * out_a
        out_pm = out_pm * (1 - kk[..., None]) + np.asarray(INK, np.float32) * kk[..., None]
    cv.pm[...] = out_pm
    cv.a[...] = out_a
    zb = np.where(valid, yi.astype(np.float32) / s, -1e9).astype(np.float32)
    # only real landforms occlude props: gentle ground swell must never clip
    # a long prop (a fuselage) that extends in front of its own anchor
    elevated = np.abs(H[yi, xi]) > zmin
    cv.zbuf = np.where(valid & elevated, zb, -1e9).astype(np.float32) if zmin < 1e8 else None
    cv.H = H


def lift(cv, x, y):
    """Screen y of the ground point (x, y) after relief (final px)."""
    H = getattr(cv, 'H', None)
    if H is None:
        return y
    xi = int(min(max(x * S, 0), H.shape[1] - 1))
    yi = int(min(max(y * S, 0), H.shape[0] - 1))
    return y - float(H[yi, xi]) * FZ


def height_at(cv, x, y):
    H = getattr(cv, 'H', None)
    if H is None:
        return 0.0
    xi = int(min(max(x * S, 0), H.shape[1] - 1))
    yi = int(min(max(y * S, 0), H.shape[0] - 1))
    return float(H[yi, xi])


# ── heightfield recipes ────────────────────────────────────────────────
def ridged(nz, X, Y, scale, octaves=5, sharp=2.0):
    amp, tot, norm = 1.0, 0.0, 0.0
    fx, fy = X * scale, Y * scale
    for o in range(octaves):
        n = nz.val(fx + o * 19.1, fy + o * 7.7)
        r = 1.0 - np.abs(n * 2 - 1)
        tot = tot + (r ** sharp) * amp
        norm += amp
        fx, fy = fx * 2.07, fy * 2.07
        amp *= 0.5
    return tot / norm


def massif(nz, X, Y, peaks, rough=0.35, scale=0.035, sharp=2.0, octaves=5):
    """Sum of peaked mounds (cx, cy, radius, height) roughened with ridged
    noise, so the flanks break into spurs and gullies. Mounds are smooth at
    the foot (no visible skirt) and pointed at the top."""
    H = np.zeros_like(X)
    for (px, py, pr, ph) in peaks:
        d2 = ((X - px) / pr) ** 2 + ((Y - py) / (pr * 0.78)) ** 2
        foot = np.exp(-d2 * 2.2)                 # smooth base
        tip = np.clip(1 - np.sqrt(d2), 0, 1) ** 1.6  # pointed summit
        mound = 0.55 * foot + 0.45 * tip
        H = np.maximum(H, mound * ph)
    rn = ridged(nz, X, Y, scale, octaves=octaves, sharp=sharp)
    # crags strongest mid-slope: none on the flat, none at the very tip
    hmax = max(p[3] for p in peaks)
    band = np.clip(H / hmax, 0, 1)
    band = band * (1 - band) * 4
    H = H * (1 - rough * band + rough * band * 1.7 * rn)
    return H


RELIEF_LIGHT = (-0.80, 0.02, 0.60)
