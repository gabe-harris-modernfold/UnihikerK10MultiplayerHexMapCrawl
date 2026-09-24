"""Landforms: peaks, ridges, craters, dunes. These are the big shapes a tile
is built around, drawn as painted masses with an inked silhouette, a lit west
face, a cool shadowed east face and a few hand strokes of strata."""
import math
import numpy as np
from tg_core import C, mix, shade, blob_pts, ellipse_pts, hex_sdf, CX, CY, R, HEX_TOP, HEX_BOT, HALF_H
from tg_props import P, FY, FZ, INK, INK_W, SHADOW_DX, SHADOW_DY, make_ramp, hull


def _mdisp(a, b, depth, rough, rng):
    """Midpoint displacement between two points (vertical jitter)."""
    if depth == 0:
        return [a]
    mx = (a[0] + b[0]) / 2 + rng.uniform(-0.15, 0.15) * abs(b[0] - a[0])
    my = (a[1] + b[1]) / 2 + rng.uniform(-1, 1) * rough
    m = (mx, my)
    return _mdisp(a, m, depth - 1, rough * 0.55, rng) + _mdisp(m, b, depth - 1, rough * 0.55, rng)


def peak(sc, X, Y, w, h, rng, ramp=('#8a80a0', '#645a7c', '#453e5c', '#2c2640'), rust=0.3, cave=False,
         snow=None, glow_cave=False, strata=5, lean=0.0):
    """One mountain: jagged silhouette from the ground line up to an apex,
    split by a ridge line into a lit west face and a dark east face."""
    cv, nz = sc.cv, sc.noise
    rp = [C(c) for c in ramp]
    apex = (X + lean * w + rng.uniform(-w * 0.08, w * 0.08), Y - h)
    L0 = (X - w / 2, Y)
    R0 = (X + w / 2, Y)
    left = _mdisp(L0, apex, 4, h * 0.10, rng)
    right = _mdisp(apex, R0, 4, h * 0.10, rng)
    sil = left + right + [R0, (X + w / 2, Y + 3), (X - w / 2, Y + 3)]
    # ridge line from the apex down to the base, drifting east
    ridge = _mdisp(apex, (X + w * 0.12 + lean * w * 0.3, Y + 2), 4, w * 0.05, rng)
    ridge = [(x + rng.uniform(-1.5, 1.5), y) for x, y in ridge] + [(X + w * 0.12, Y + 3)]
    sc.shadow(('poly', [(X - w * 0.1, Y), (X + w / 2, Y), (X + w / 2 + SHADOW_DX * h * 0.6, Y + SHADOW_DY * h * 0.6)]))

    def draw():
        cv.fill(('poly', sil), INK, grow=INK_W * 1.2)
        cv.fill(('poly', sil), rp[2])
        west = left + ridge[::-1]
        cv.fill(('poly', west), rp[1], clip=('poly', sil))
        # sunlit shoulders near the top of the west face
        hi = [left[i] for i in range(len(left) // 2, len(left))] + [(apex[0] - w * 0.05, apex[1] + h * 0.35)]
        cv.fill(('poly', hi), rp[0], 0.9, clip=('poly', west))
        # east face deep shadow lower half
        cv.fill(('poly', [ridge[len(ridge) // 2], R0, (X + w / 2, Y + 3), ridge[-1]]), rp[3], 0.7, clip=('poly', sil))
        # strata / erosion gullies: short inked strokes following the slope
        for i in range(strata):
            t = rng.uniform(0.25, 0.85)
            side = -1 if rng.random() < 0.55 else 1
            y0 = apex[1] + h * t
            x0 = X + side * w * 0.5 * t * rng.uniform(0.2, 0.9)
            L = h * rng.uniform(0.12, 0.25)
            a = math.pi / 2 + side * rng.uniform(0.25, 0.6)
            g = [(x0, y0), (x0 + math.cos(a) * L * 0.5 + rng.uniform(-1, 1), y0 + math.sin(a) * L * 0.5),
                 (x0 + math.cos(a) * L, y0 + math.sin(a) * L)]
            cv.fill(('line', g, 0.7), INK, 0.55, clip=('poly', sil))
        if rust > 0:
            for i in range(int(rust * 8)):
                t = rng.uniform(0.3, 0.9)
                x0 = X + rng.uniform(-w * 0.35, w * 0.35) * t
                y0 = apex[1] + h * t
                cv.fill(('line', [(x0, y0), (x0 + rng.uniform(-1, 1), y0 + rng.uniform(4, 9))], 1.1),
                        C('#8a4424'), 0.45, clip=('poly', sil))
        if snow:
            cap = [p for p in left if p[1] < apex[1] + h * 0.22] + [apex] + [p for p in right if p[1] < apex[1] + h * 0.22]
            if len(cap) >= 3:
                cv.fill(('poly', cap), C(snow), 0.85, clip=('poly', sil))
        if cave:
            cx0 = X - w * 0.08
            cw, ch = w * 0.14, h * 0.18
            mouth = ellipse_pts(cx0, Y, cw, ch, 16, math.pi, math.tau) + [(cx0 + cw, Y + 1), (cx0 - cw, Y + 1)]
            cv.fill(('poly', mouth), INK, grow=0.8)
            cv.fill(('poly', mouth), C('#0e0a10'))
            if glow_cave:
                cv.fill(('poly', mouth), C('#ff9a3a'), 0.25, mode='glow', grow=1.5)
                cv.fill(('ellipse', cx0, Y - ch * 0.2, cw * 0.4, ch * 0.3), C('#ffb060'), 0.5, mode='glow')
    sc.add(Y, draw)
    return apex


def rock_spine(sc, pts, rng, height=18, width=16, ramp=('#b89a74', '#8a7056', '#5e4a3c', '#3e3028')):
    """A ridge: rock masses strung along a ground path, each a small peak,
    overlapping into one jagged spine."""
    n = len(pts)
    for i, (x, y) in enumerate(pts):
        t = i / max(1, n - 1)
        hh = height * (0.55 + 0.45 * math.sin(math.pi * t)) * rng.uniform(0.8, 1.15)
        ww = width * rng.uniform(0.85, 1.2)
        peak(sc, x, y, ww, hh, rng, ramp=ramp, rust=0.15, strata=2)


def crater(sc, X, Y, rx, ry, rng, glow='#8cff5a', rim=('#6a5e4c', '#4e4436', '#352c26', '#1e1a18'), depth=0.55):
    """A detonation bowl seen from the south: raised lip, the far inner wall
    lit and visible, a black throat, a poisonous pool that glows."""
    cv, nz = sc.cv, sc.noise
    rp = [C(c) for c in rim]

    def draw():
        outer = blob_pts(X, Y, rx * 1.12, ry * 1.12, nz, 0.06, n=40, phase=X)
        lip = blob_pts(X, Y, rx, ry, nz, 0.05, n=40, phase=X + 4)
        bowl = blob_pts(X, Y + ry * 0.12, rx * 0.8, ry * 0.72, nz, 0.06, n=36, phase=X + 9)
        pool = blob_pts(X, Y + ry * 0.28, rx * 0.34, ry * 0.26, nz, 0.12, n=24, phase=X + 13)
        # fused-glass ejecta rings on the ground
        for k in range(3):
            ring = blob_pts(X, Y, rx * (1.25 + 0.14 * k), ry * (1.25 + 0.14 * k), nz, 0.07, n=40, phase=X + k * 7)
            cv.fill(('line', ring + [ring[0]], 0.8), C('#8a8a6a'), 0.35)
        cv.fill(('poly', outer), rp[1])
        cv.fill(('poly', outer), INK, 0.8, grow=0.8, sub=('poly', outer))
        cv.fill(('poly', lip), rp[0])
        cv.fill(('poly', bowl), INK, grow=INK_W)
        cv.fill(('poly', bowl), rp[3])
        # the far inner wall catches light: a crescent on the top of the bowl
        far = blob_pts(X, Y - ry * 0.05, rx * 0.78, ry * 0.55, nz, 0.06, n=36, phase=X + 21)
        cv.fill(('poly', far), rp[2], clip=('poly', bowl), sub=('poly', blob_pts(X, Y + ry * 0.2, rx * 0.72, ry * 0.6, nz, 0.06, n=36, phase=X + 25)))
        for k in range(7):
            a = math.pi + rng.uniform(0.15, 0.85) * math.pi
            x0 = X + math.cos(a) * rx * 0.75
            y0 = Y + ry * 0.12 + math.sin(a) * ry * 0.65
            cv.fill(('line', [(x0, y0), (x0 + (X - x0) * 0.25, y0 + (Y + ry * 0.3 - y0) * 0.3)], 0.6), INK, 0.5, clip=('poly', bowl))
        if glow:
            cv.fill(('poly', pool), C(glow), 0.25, grow=5, mode='glow')
            cv.fill(('poly', pool), C(glow), 0.35, grow=2, mode='glow')
            cv.fill(('poly', pool), mix(C(glow), C('#1a3a10'), 0.3))
            cv.fill(('poly', blob_pts(X - rx * 0.06, Y + ry * 0.24, rx * 0.16, ry * 0.09, nz, 0.2, phase=X + 30)),
                    mix(C(glow), C('#ffffff'), 0.5), 0.9)
    sc.add(Y - ry * 1.3, draw)


def dune_field(cv, noise, rng, stops, crest_light, lee_dark, n=4, angle=-0.25, amp=10):
    """Ground-level dunes: wavy crest lines across the hex; each crest casts a
    lee shadow downwind and carries a bright windward lip."""
    ys = np.linspace(HEX_TOP + 18, HEX_BOT - 10, n)
    for i, y0 in enumerate(ys):
        y0 += rng.uniform(-6, 6)
        ph = rng.uniform(0, 50)
        pts = []
        for k in range(29):
            x = CX - R - 4 + (2 * R + 8) * k / 28
            y = y0 + math.sin(x * 0.045 + ph) * amp * 0.6 + (noise.n1(x * 0.03 + ph) - 0.5) * amp + (x - CX) * angle
            pts.append((x, y))
        lee = pts + [(p[0], p[1] + rng.uniform(8, 14)) for p in reversed(pts)]
        cv.fill(('poly', lee), lee_dark, 0.55)
        cv.fill(('line', pts, 2.2), crest_light, 0.75)
        cv.fill(('line', [(x, y + 1.2) for x, y in pts], 0.6), INK, 0.35)
        # wind ripples
        for r_ in range(6):
            x = rng.uniform(CX - R, CX + R)
            y = y0 - rng.uniform(4, 16) + (x - CX) * angle
            rip = [(x - 4, y), (x, y - 0.8), (x + 4, y)]
            cv.fill(('line', rip, 0.45), crest_light, 0.45)
