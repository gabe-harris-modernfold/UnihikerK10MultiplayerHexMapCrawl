"""Shared helpers for the terrain builders: canvases, scatter, the ground
recipes several terrains share (scrub, urban, flood water), and small set
pieces used across terrains."""
import math
import numpy as np
from tg_core import (Canvas, Noise, C, mix, shade, hex_pts, hex_sdf, blob_pts, ellipse_pts,
                     CX, CY, R, HEX_TOP, HEX_BOT, HALF_H, S)
from tg_props import (Scene, INK, P, FY, FZ, house, pole, wire, car, rock, tuft, dead_tree, foliage, evil_logo,
                      stroke_text, GREEN_SICK, EVIL_RED, CREAM)
from tg_ground import (rim, crack, ruts, road, smooth_path, water, random_in_hex, stroke_texture, flat_ground)
from tg_relief import relief, grid_ground, rim_falloff, RELIEF_LIGHT


def inside(x, y, m=4.0):
    return hex_sdf(np.float32(x), np.float32(y)) < -m


def scatter(rng, n, margin=8.0, avoid=(), avoid_r=12.0, band=None):
    """n random points in the hex, keeping clear of `avoid` ((x, y) uses
    avoid_r, (x, y, r) its own radius)."""
    pts = []
    tries = 0
    while len(pts) < n and tries < n * 40:
        tries += 1
        x, y = random_in_hex(rng, margin, band)
        ok = True
        for a in list(avoid) + pts:
            rr = a[2] if len(a) > 2 else avoid_r
            if math.hypot(x - a[0], y - a[1]) < rr:
                ok = False
                break
        if ok:
            pts.append((x, y))
    return pts


def new(seed):
    rng = np.random.default_rng(seed)
    nz = Noise(seed % 100000)
    cv = Canvas()
    return cv, rng, nz


def finish(sc, shadows=0.40):
    sc.draw_shadows(shadows)
    sc.cv.ground_clip = False
    sc.draw()
    return sc.cv


def gentle_hills(nz, amp=6.0, scale=0.018, seed_off=0.0):
    X, Y = grid_ground()
    n = nz.fbm(X * scale + 50 + seed_off, Y * scale + 20, 3)
    H = np.clip((n - 0.42) * 2.4, 0, None) * amp
    return H * rim_falloff(X, Y, inner=26)


def swell(cv, nz, amp=6.0):
    """Gentle ground swell with no occlusion: every flat terrain gets a
    little relief so a hex never reads as a printed card."""
    relief(cv, gentle_hills(nz, amp=amp), light=RELIEF_LIGHT, bands=(0.80, 0.55, 0.30),
           mult=(1.08, 0.97, 0.82, 0.68), ink=0.35, hatch=0.0, zmin=1e9)


# ── palettes ──────────────────────────────────────────────────────────
SCRUB_BASE = '#94763f'
SCRUB_STROKES = [('#b8995a', 3.0), ('#6e5532', 2.2), ('#cdb574', 1.2), ('#57442a', 1.0), ('#8a8048', 0.8)]
SCRUB_TUFT = (C('#d6c07c'), C('#4e3c24'))
BUSH = ('#8a8a4a', '#62662f', '#43472a', '#2a2e1c')
WEEDS = ('#9aa648', '#6e8438', '#48662c', '#2a4222')
YARD_TREE = ('#8a9a48', '#5e7a36', '#3e5a2a', '#243a1e')
HOUSE_COLS = ['#c8b894', '#9fb8b0', '#c89a8a', '#b8b08a', '#a0a8b8', '#d0c0a0', '#b8c8a0', '#d8b8a0']
ROOF_COLS = ['#5a3a34', '#4a4450', '#6a4a30', '#3e4a4a', '#5a4a3a', '#6a3030']
CAR_COLS = ['#5f8f86', '#a85a3a', '#c8b060', '#7a6a9a', '#8a3a3a', '#d8d0c0', '#4a6a8a']
ASPHALT = C('#3a383c')
FLOOD = (C('#17302f'), C('#2a4a46'), C('#86a494'))


def scrub_base(cv, nz, rng, strokes=1500, patches=True):
    flat_ground(cv, nz, SCRUB_BASE, var=0.06, tint2='#6f6a44', tint_amt=0.45 if patches else 0.0, gradient=0.14)
    stroke_texture(cv, rng, nz, SCRUB_STROKES, n=strokes, length=(1.8, 4.2), width=(0.55, 1.0), alpha=0.55,
                   flow=-1.5, spread=0.45)
    for i in range(rng.integers(1, 4)):
        x, y = random_in_hex(rng, 18)
        sh = ('poly', blob_pts(x, y, rng.uniform(10, 20), rng.uniform(6, 10), nz, 0.35, phase=x))
        cv.ground_clip = True
        cv.fill(sh, C('#a88c58'), 0.3)
        for k in range(3):
            crack(cv, x + rng.uniform(-6, 6), y + rng.uniform(-3, 3), rng.uniform(6, 14), rng, nz,
                  color=C('#4a3624'), w=0.5, alpha=0.6, branch=1)
    for i in range(50):
        x, y = random_in_hex(rng, 3)
        r = rng.uniform(0.5, 1.2)
        cv.fill(('ellipse', x, y, r, r * 0.7), C('#4a3a28'), 0.8)
        cv.fill(('ellipse', x - r * 0.3, y - r * 0.3, r * 0.5, r * 0.35), C('#dcc690'), 0.7)


def scrub_scatter(sc, rng, nz, tufts=60, bushes=6, avoid=(), avoid_r=14, bush_ramp=BUSH, tuft_cols=SCRUB_TUFT):
    cv = sc.cv
    for (x, y) in scatter(rng, tufts, 4, avoid, avoid_r):
        with sc.at(x, y) as Y:
            sc.add(Y, (lambda x=x, Y=Y: tuft(cv, x, Y, rng.uniform(2.4, 4.4), tuft_cols[0], tuft_cols[1], rng)))
    for (x, y) in scatter(rng, bushes, 10, avoid, avoid_r + 4):
        r = rng.uniform(3.2, 6.0)
        with sc.at(x, y) as Y:
            sc.shadow(('ellipse', x + 2.4, Y + 0.6, r * 1.1, r * 0.5))
            sc.add(Y, (lambda x=x, Y=Y, r=r: foliage(cv, x, Y - r * 0.6, r, nz, ramp=bush_ramp, phase=x + Y, squash=0.72)))


def urban_base(cv, nz, rng, weeds=8, cracks=8, base='#66625a'):
    flat_ground(cv, nz, base, var=0.07, tint2='#58663e', tint_amt=0.35, gradient=0.12)
    stroke_texture(cv, rng, nz, [('#7e7a6e', 3), ('#4a4842', 2), ('#6a7a44', 1.6), ('#8a8474', 1)], n=1100,
                   length=(1.5, 4.0), alpha=0.5, flow=0.0, spread=1.6)
    cv.ground_clip = True
    for i in range(weeds):
        x, y = random_in_hex(rng, 10)
        cv.fill(('poly', blob_pts(x, y, rng.uniform(8, 18), rng.uniform(5, 10), nz, 0.4, phase=x)), C(WEEDS[2]), 0.5)
    for i in range(cracks):
        x, y = random_in_hex(rng, 12)
        crack(cv, x, y, rng.uniform(10, 26), rng, nz, color=C('#24201e'), w=0.55, alpha=0.6)


def yard_tree(sc, x, y, r, nz, ramp=YARD_TREE):
    cv = sc.cv
    sc.shadow(('ellipse', x + r * 0.7, y + 0.5, r, r * 0.5))
    sc.add(y, (lambda: (cv.fill(('line', [(x, y), (x, y - r * 0.8)], 1.6), INK),
                        cv.fill(('line', [(x, y), (x, y - r * 0.8)], 0.7), C('#4a3222')),
                        foliage(cv, x, y - r * 1.2, r, nz, ramp=ramp, phase=x + y))))


def neighbourhood(sc, rng, nz, street, ruin=0.5, over=0.5, lit=0.0, cars=3, spacing=34, setback=21,
                  hw=(20, 25), hd=(13, 15), hh=(8, 10), poles=True, trees=0.6, road_w=11):
    """Houses both sides of a curving suburban street, facing it: kerb,
    drives, a tree per yard, sagging phone lines."""
    cv = sc.cv
    cv.ground_clip = True
    pts = road(cv, street, road_w, ASPHALT, None, dash=C('#a89a70'), rng=rng, noise=nz, broken=0.6)
    cv.fill(('line', pts, road_w + 4), C('#8a8474'), 0.35)
    cv.fill(('line', pts, road_w), ASPHALT)
    for i in range(0, len(pts) - 1, 2):
        pass
    placed = []
    acc = spacing * 0.5
    pole_pts = []
    for i in range(1, len(pts)):
        x0, y0 = pts[i - 1]
        x1, y1 = pts[i]
        L = math.hypot(x1 - x0, y1 - y0) or 1
        acc += L
        if acc < spacing:
            continue
        acc = 0
        dx, dy = (x1 - x0) / L, (y1 - y0) / L
        for side in (-1, 1):
            hx = x1 - dy * side * setback
            hy = y1 + dx * side * setback
            if not inside(hx, hy, 15):
                continue
            yaw = math.atan2(-dy, dx)
            house(sc, hx, hy, rng.uniform(*hw), rng.uniform(*hd), rng.uniform(*hh), yaw,
                  wall=HOUSE_COLS[rng.integers(len(HOUSE_COLS))], roof=ROOF_COLS[rng.integers(len(ROOF_COLS))],
                  ruined=ruin * rng.uniform(0.3, 1.3), overgrown=over * rng.uniform(0.3, 1.3),
                  lit=lit, chimney=rng.random() < 0.4, rng=rng)
            placed.append((hx, hy))
            cv.fill(('line', [(x1 - dy * side * 6, y1 + dx * side * 6), (hx + dy * side * 4, hy - dx * side * 4)], 4), C('#7a766a'), 0.6)
            if rng.random() < trees:
                tx, ty = hx + dx * rng.uniform(11, 15) * rng.choice([-1, 1]), hy + (4 if side > 0 else -4)
                if inside(tx, ty, 8):
                    yard_tree(sc, tx, ty, rng.uniform(4.5, 7), nz)
        if poles and len(pole_pts) < 4:
            px, py = x1 - dy * 8, y1 + dx * 8
            if inside(px, py, 8):
                pole_pts.append((px, py))
    cv.ground_clip = False
    tops = [pole(sc, px, py, h=22, lean=rng.uniform(-0.1, 0.1)) for (px, py) in pole_pts]
    if len(tops) > 1:
        def wires(tops=tops):
            for a, b in zip(tops[:-1], tops[1:]):
                wire(cv, (a[0] - 3, a[1] + 2.5), (b[0] - 3, b[1] + 2.5), sag=5)
                wire(cv, (a[0] + 3, a[1] + 2.5), (b[0] + 3, b[1] + 2.5), sag=6)
        sc.add(999, wires)
    for c in range(cars):
        p = pts[rng.integers(4, len(pts) - 4)]
        if inside(p[0], p[1], 12):
            car(sc, p[0] + rng.uniform(-3, 3), p[1] + rng.uniform(-2, 2), rng.uniform(-1, 1),
                color=CAR_COLS[rng.integers(len(CAR_COLS))], fins=rng.random() < 0.5, rust=0.7, s=1.3)
    return placed, pts


def water_ground(cv, nz, rng, base='#1a3230', tint='#24403a', ripples=60, glints=24, strokes=900):
    flat_ground(cv, nz, base, var=0.12, tint2=tint, tint_amt=0.6, gradient=0.16)
    stroke_texture(cv, rng, nz, [('#2c4c46', 3), ('#12262a', 2), ('#3e5e52', 1)], n=strokes, length=(3, 8), width=(0.5, 0.9),
                   alpha=0.45, flow=0.05, spread=0.15)
    cv.ground_clip = True
    water(cv, [('poly', hex_pts())], FLOOD[0], FLOOD[1], FLOOD[2], nz, rng, ripples=ripples, glints=glints, alpha=0.55)


def waterline(sc, X, Y, rx, ry, reflect=None, rng=None):
    """Ripple ring (and optionally a dark reflection) where something meets
    the floodwater. Drawn just behind the thing itself."""
    cv = sc.cv

    def draw():
        if reflect is not None:
            cv.fill(('poly', blob_pts(X + 1, Y + ry * 1.2, rx * 0.8, ry * 1.4, sc.noise, 0.25, phase=X)), C(reflect), 0.25)
            for k in range(3):
                yy = Y + ry * 0.4 + k * 2.2
                cv.fill(('line', [(X - rx * 0.7, yy), (X + rx * 0.7, yy)], 0.6), FLOOD[1], 0.8)
        cv.fill(('poly', blob_pts(X + 1, Y + 1.0, rx, ry, sc.noise, 0.2, phase=X)), C('#0e1e20'), 0.35)
        cv.fill(('line', ellipse_pts(X, Y + 0.6, rx, ry * 0.9, 24), 0.7), FLOOD[2], 0.6)
    sc.add(Y - 0.02, draw)


def drowned_house(sc, X, Y, yaw, wall, roofc, rng):
    """Only the roof and the top of the walls clear the water."""
    w, d = rng.uniform(22, 28), rng.uniform(13, 16)
    waterline(sc, X, Y, w * 0.62, d * 0.4, reflect=roofc, rng=rng)
    house(sc, X, Y, w, d, 3.0, yaw, wall=wall, roof=roofc, ruined=rng.uniform(0, 0.5),
          overgrown=rng.uniform(0, 0.6), door=False, windows=False, rng=rng)


def drowned_tree(sc, x, y, r, nz):
    cv = sc.cv
    sc.add(y, (lambda: (cv.fill(('line', ellipse_pts(x, y + 1, r * 1.2, r * 0.4, 16), 0.6), FLOOD[2], 0.5),
                        foliage(cv, x, y - r * 0.4, r, nz, ramp=YARD_TREE, phase=x, squash=0.7))))


def cookfire(sc, X, Y, s=1.0):
    cv = sc.cv

    def draw():
        cv.fill(('ellipse', X, Y, 16 * s, 8 * s), C('#ffb050'), 0.2, mode='glow')
        for k in range(5):
            a = math.tau * k / 5
            cv.fill(('line', [(X + math.cos(a) * 4 * s, Y + math.sin(a) * 2 * s), (X - math.cos(a) * s, Y - s)], 1.2), INK)
        flame = [(X - 3 * s, Y), (X, Y - 9 * s), (X + 3 * s, Y)]
        cv.fill(('poly', flame), C('#ff8a2a'))
        cv.fill(('poly', [(X - 1.5 * s, Y), (X, Y - 5 * s), (X + 1.5 * s, Y)]), C('#ffe08a'))
        for k in range(4):
            cv.fill(('ellipse', X + k * 3 * s, Y - (13 + k * 6) * s, (2.5 + k) * s, (2 + k * 0.9) * s), C('#6a6070'), 0.35 - k * 0.07)
    sc.add(Y, draw)


def lamp_string(sc, pts, lift=16):
    """A sagging string of lamps between posts (settlement night light)."""
    cv = sc.cv

    def lights():
        ps = [(x, y - lift) for x, y in pts]
        for a, b in zip(ps[:-1], ps[1:]):
            wire(cv, a, b, sag=4, w=0.4)
            for t in (0.25, 0.5, 0.75):
                x = a[0] + (b[0] - a[0]) * t
                y = a[1] + (b[1] - a[1]) * t + 4 * 4 * t * (1 - t)
                cv.fill(('ellipse', x, y + 1, 0.9, 0.9), C('#ffd27a'))
                cv.fill(('ellipse', x, y + 1, 2.8, 2.8), C('#ff9a3a'), 0.3, mode='glow')
    for (x, y) in pts:
        pole(sc, x, y, h=lift / FZ + 1, cross=False, color='#5a4a3a')
    sc.add(999, lights)


def girder(sc, X, Y, rng, n=6, seg=8):
    cv = sc.cv

    def draw():
        pts = [(X, Y)]
        a = -math.pi / 2 + rng.uniform(-0.4, 0.4)
        for i in range(n):
            a += rng.uniform(-0.6, 0.6)
            pts.append((pts[-1][0] + math.cos(a) * seg, pts[-1][1] + math.sin(a) * seg))
        cv.fill(('line', pts, 3.6), INK)
        cv.fill(('line', pts, 1.8), C('#7a4a30'))
        for p in pts[1:-1:2]:
            cv.fill(('line', [(p[0] - 1.5, p[1]), (p[0] + 1.5, p[1])], 0.6), INK)
    sc.add(Y, draw)


def cave(sc, X, Y, r, rng, glow=False):
    cv = sc.cv

    def draw():
        mouth = ellipse_pts(X, Y, r, r * 1.2, 16, math.pi, math.tau) + [(X + r, Y + 1), (X - r, Y + 1)]
        cv.fill(('poly', mouth), INK, grow=1.0)
        cv.fill(('poly', mouth), C('#0c080e'))
        if glow:
            cv.fill(('poly', mouth), C('#ff9a3a'), 0.3, mode='glow', grow=2)
            cv.fill(('ellipse', X, Y - r * 0.3, r * 0.45, r * 0.4), C('#ffb060'), 0.6, mode='glow')
        for sg in (-1, 1):
            cv.fill(('line', [(X + sg * r * 0.9, Y), (X + sg * r * 0.8, Y - r * 1.1)], 1.6), INK)
            cv.fill(('line', [(X + sg * r * 0.9, Y), (X + sg * r * 0.8, Y - r * 1.1)], 0.8), C('#7a5a3a'))
        cv.fill(('line', [(X - r, Y - r * 1.1), (X + r, Y - r * 1.1)], 1.6), INK)
        cv.fill(('line', [(X - r, Y - r * 1.1), (X + r, Y - r * 1.1)], 0.8), C('#7a5a3a'))
    sc.add(Y, draw)


def mine_cart(sc, X, Y, rng, rails=True):
    cv = sc.cv

    def draw():
        if rails:
            for sg in (-1, 1):
                cv.fill(('line', [(X - 16, Y + sg * 1.4 + 3), (X + 14, Y + sg * 1.4 - 4)], 0.6), C('#5a5048'))
        body = [(X - 5, Y - 1), (X + 5, Y - 3), (X + 4, Y - 8), (X - 6, Y - 6)]
        cv.fill(('poly', body), INK, grow=0.8)
        cv.fill(('poly', body), C('#6a4a36'))
        cv.fill(('poly', [body[3], body[2], (body[2][0] - 1, body[2][1] - 1.5), (body[3][0] + 1, body[3][1] - 1.5)]), C('#2a2020'))
        for wx in (-3, 3):
            cv.fill(('ellipse', X + wx, Y - 0.5 - wx * 0.2, 1.4, 1.4), INK)
    sc.add(Y, draw)
