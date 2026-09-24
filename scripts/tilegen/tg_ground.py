"""Ground painting: the hex-clipped floor every tile stands on.

Painted, not generated-looking: a low-frequency value field mapped through a
four-stop ramp (with a soft posterise so it reads as flat cel paint rather
than a smooth gradient), then a few hundred directional brush dabs laid along
a noise flow field, the way a background painter scrubs in dirt.
"""
import math
import numpy as np
from tg_core import (C, mix, shade, final_grid, hex_pts, hex_sdf, in_hex, blob_pts, wobble,
                     CX, CY, R, HEX_TOP, HEX_BOT, HALF_H)
from tg_props import INK


def ramp4(light, mid, dark, deep):
    return [C(deep), C(dark), C(mid), C(light)]


def ramp_interp(stops, t, poster=0.55):
    """t in 0..1 over stops (deep..light). poster>0 pulls values toward the
    stops so large areas sit on flat paint with narrow soft transitions."""
    n = len(stops) - 1
    tt = np.clip(t, 0, 0.9999) * n
    i = np.floor(tt).astype(np.int64)
    f = tt - i
    if poster > 0:
        # smoothstep sharpened toward a step
        g = np.clip((f - 0.5) / max(1e-3, (1 - poster)) + 0.5, 0, 1)
        f = g * g * (3 - 2 * g)
    S = np.asarray(stops, np.float32)
    a = S[i]
    b = S[np.minimum(i + 1, n)]
    return a + (b - a) * f[..., None]


def random_in_hex(rng, margin=6.0, band=None):
    while True:
        x = rng.uniform(CX - R + margin, CX + R - margin)
        y = rng.uniform(HEX_TOP + margin, HEX_BOT - margin)
        if band and not (band[0] <= y <= band[1]):
            continue
        if hex_sdf(np.float32(x), np.float32(y)) < -margin:
            return x, y


def paint_ground(cv, noise, stops, rng, scale=0.028, contrast=1.15, poster=0.55, dabs=260,
                 dab_len=(3.0, 8.0), dab_alpha=0.30, flow=0.0, bias=0.0, detail=0.3):
    X, Y = final_grid()
    n = noise.fbm(X * scale + 11.3, Y * scale + 5.1, 4)
    d = noise.fbm(X * scale * 5.3 + 70, Y * scale * 5.3 + 30, 2)
    t = np.clip((n - 0.5) * 2.2 * contrast + 0.5 + (d - 0.5) * detail + bias, 0, 1)
    rgb = ramp_interp(stops, t, poster)
    cv.ground_clip = True
    cv.fill_array(rgb)
    for i in range(dabs):
        x, y = rng.uniform(CX - R, CX + R), rng.uniform(HEX_TOP - 2, HEX_BOT + 2)
        a = flow + (noise.n1(x * 0.05, y * 0.05) - 0.5) * 1.6
        L = rng.uniform(*dab_len)
        w = L * rng.uniform(0.18, 0.32)
        ca, sa = math.cos(a), math.sin(a)
        pts = [(x + ca * L * math.cos(t2) - sa * w * math.sin(t2), y + sa * L * math.cos(t2) + ca * w * math.sin(t2))
               for t2 in np.linspace(0, math.tau, 10, endpoint=False)]
        tv = float(np.clip((noise.fbm(np.float32(x * scale + 11.3), np.float32(y * scale + 5.1), 4) - 0.5) * 2.2 * contrast + 0.5 + bias, 0, 1))
        k = min(3, max(0, int(tv * 3 + rng.uniform(-0.8, 0.8))))
        col = stops[k] if rng.random() < 0.5 else stops[min(3, k + 1)]
        cv.fill(('poly', pts), col, dab_alpha)
    return t


def rim(cv, alpha=0.55, width=1.5, inner=0.13):
    """The baked grid line: a dark ink seam on the hex edge plus a faint inner
    shade. Baked into the tile so a prop standing in the hex below covers the
    seam instead of the grid pass drawing it over the prop."""
    hp = hex_pts()
    old = cv.ground_clip
    cv.ground_clip = True
    cv.fill(('line', hp + [hp[0]], 7.0), C('#1a1020'), inner)
    cv.fill(('line', hp + [hp[0]], 3.0), C('#1a1020'), inner)
    cv.ground_clip = False
    cv.fill(('line', hp + [hp[0]], width), INK, alpha)
    cv.ground_clip = old


def crack(cv, x, y, L, rng, noise, color=None, w=0.7, alpha=0.8, branch=2):
    color = color or INK
    pts = [(x, y)]
    a = rng.uniform(0, math.tau)
    for i in range(int(L / 2.5)):
        a += rng.uniform(-0.6, 0.6)
        x += math.cos(a) * 2.5
        y += math.sin(a) * 2.5 * 0.7
        pts.append((x, y))
        if branch > 0 and rng.random() < 0.12:
            crack(cv, x, y, L * 0.45, rng, noise, color, w * 0.7, alpha * 0.8, branch - 1)
    cv.fill(('line', pts, w), color, alpha)


def ruts(cv, pts, color, w=1.4, gap=3.0, alpha=0.5):
    """Twin tyre ruts following a polyline."""
    for side in (-1, 1):
        off = []
        for i, (x, y) in enumerate(pts):
            x2, y2 = pts[min(i + 1, len(pts) - 1)]
            x1, y1 = pts[max(i - 1, 0)]
            dx, dy = x2 - x1, y2 - y1
            L = math.hypot(dx, dy) or 1
            off.append((x - dy / L * gap * side, y + dx / L * gap * side))
        cv.fill(('line', off, w), color, alpha)


def smooth_path(ctrl, n=40):
    """Catmull-Rom through control points."""
    out = []
    P = [ctrl[0]] + list(ctrl) + [ctrl[-1]]
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(n // (len(P) - 3) + 1):
            t = k / (n // (len(P) - 3) + 1)
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2
                                    + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in range(2)))
    out.append(ctrl[-1])
    return out


def road(cv, ctrl, width, asphalt, edge, dash=None, alpha=1.0, broken=0.3, rng=None, noise=None):
    pts = smooth_path(ctrl)
    cv.fill(('line', pts, width + 1.6), shade(asphalt, 0.7), alpha * 0.8)
    cv.fill(('line', pts, width), asphalt, alpha)
    if dash is not None:
        seg = []
        acc = 0.0
        for i in range(1, len(pts)):
            L = math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
            acc += L
            if int(acc / 5) % 2 == 0:
                seg.append(pts[i])
            elif seg:
                if len(seg) > 1:
                    cv.fill(('line', seg, 0.7), dash, 0.75)
                seg = []
    if rng is not None and broken > 0:
        for i in range(int(broken * 8)):
            p = pts[rng.integers(len(pts))]
            crack(cv, p[0], p[1], rng.uniform(4, 10), rng, noise, w=0.5, alpha=0.6, branch=1)
    return pts


def water(cv, shapes, deep, mid, light, noise, rng, ripples=18, glints=10, alpha=1.0):
    """Still, murky water: flat deep body, a lighter band toward the far edge
    (sky reflection), ripple strokes and a few cold glints. No glow: water is
    never a light source in this game."""
    cv.fill(shapes, deep, alpha)
    for sh in shapes:
        if sh[0] != 'poly':
            continue
        pts = sh[1]
        ys = [p[1] for p in pts]
        y0, y1 = min(ys), max(ys)
        xs = [p[0] for p in pts]
        x0, x1 = min(xs), max(xs)
        band = [(x0 - 5, y0 - 5), (x1 + 5, y0 - 5), (x1 + 5, y0 + (y1 - y0) * 0.38), (x0 - 5, y0 + (y1 - y0) * 0.22)]
        cv.fill(('poly', band), mid, 0.55 * alpha, clip=sh)
        for i in range(ripples):
            x = rng.uniform(x0, x1)
            y = rng.uniform(y0, y1)
            L = rng.uniform(3, 9)
            rip = [(x - L / 2, y), (x, y - 0.6), (x + L / 2, y)]
            cv.fill(('line', rip, 0.55), light, 0.6 * alpha, clip=sh)
        for i in range(glints):
            x = rng.uniform(x0, x1)
            y = rng.uniform(y0, y0 + (y1 - y0) * 0.5)
            cv.fill(('line', [(x - 1.2, y), (x + 1.2, y)], 0.5), mix(light, C('#ffffff'), 0.4), 0.7 * alpha, clip=sh)


def stroke_texture(cv, rng, nz, palette, n=900, length=(2.0, 5.0), width=(0.6, 1.1), alpha=0.45,
                   flow=-1.35, spread=0.5, band=None, margin=0.0):
    """Painterly ground: many short strokes along a noise flow field, batched
    per colour (one composite per colour). palette = [(colour, weight)]."""
    cols = [C(c) if isinstance(c, str) else c for c, _ in palette]
    w = np.array([wt for _, wt in palette], np.float64)
    w = w / w.sum()
    batches = [[] for _ in cols]
    for i in range(n):
        x = rng.uniform(CX - R - 2, CX + R + 2)
        y = rng.uniform(HEX_TOP - 2, HEX_BOT + 2)
        if band and not (band[0] <= y <= band[1]):
            continue
        if hex_sdf(np.float32(x), np.float32(y)) > 1.5 - margin:
            continue
        a = flow + (nz.n1(x * 0.04, y * 0.04) - 0.5) * 2 * spread
        L = rng.uniform(*length)
        k = rng.choice(len(cols), p=w)
        batches[k].append(('line', [(x, y), (x + math.cos(a) * L, y + math.sin(a) * L)], rng.uniform(*width)))
    old = cv.ground_clip
    cv.ground_clip = True
    for col, b in zip(cols, batches):
        if b:
            cv.fill(b, col, alpha)
    cv.ground_clip = old


def flat_ground(cv, nz, base, var=0.07, scale=0.012, gradient=0.10, tint2=None, tint_amt=0.0, tint_scale=0.02):
    """Quiet base coat: one colour, a whisper of large-scale variation, a
    diorama light falloff from the upper-left, optional second colour in
    soft patches."""
    X, Y = final_grid()
    n = nz.fbm(X * scale + 3.1, Y * scale + 8.7, 3)
    g = 1 + (n - 0.5) * 2 * var
    g = g * (1 + gradient * (-(X - CX) / R * 0.5 - (Y - CY) / HALF_H * 0.5) * 0.5)
    rgb = np.asarray(C(base) if isinstance(base, str) else base, np.float32)[None, None, :] * g[..., None]
    if tint2 is not None and tint_amt > 0:
        t = nz.fbm(X * tint_scale + 40, Y * tint_scale + 12, 3)
        t = np.clip((t - 0.5) * 4 + 0.5, 0, 1) * tint_amt
        rgb = rgb * (1 - t[..., None]) + np.asarray(C(tint2), np.float32)[None, None, :] * t[..., None] * g[..., None]
    old = cv.ground_clip
    cv.ground_clip = True
    cv.fill_array(np.clip(rgb, 0, 1))
    cv.ground_clip = old
