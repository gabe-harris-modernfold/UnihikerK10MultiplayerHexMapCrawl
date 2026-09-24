"""Wild and high ground: Rust Forest (2), Ridge (7), Mountain (8),
Nuke Crater (10)."""
import math
import numpy as np
from tg_core import C, mix, shade, hex_pts, hex_sdf, blob_pts, ellipse_pts, CX, CY, R, HEX_TOP, HEX_BOT, HALF_H, S
from tg_props import (Scene, INK, P, FY, FZ, rot, house, car, rock, dead_tree, foliage, evil_logo, stroke_text,
                      EVIL_RED, CREAM, GREEN_SICK, water_tower, pole, wire, cylinder)
from tg_props2 import (plane, glowing_eyes, mushroom, ribcage, skull, billboard, shade_tube)
from tg_props3 import (lookout_tower, radio_mast, dome_observatory, chairlift, jet, bomb_casing, figure)
from tg_ground import rim, crack, smooth_path, random_in_hex, stroke_texture, flat_ground
from tg_relief import relief, grid_ground, rim_falloff, massif, ridged, RELIEF_LIGHT
from tg_common import inside, scatter, new, finish, gentle_hills, girder, cave, mine_cart


# ── 2 Rust Forest ─────────────────────────────────────────────────────
RUST_FUNGUS = ('#ea7a3a', '#bc4e26', '#86301c', '#4c1a16')


def rust_forest(v, seed):
    cv, rng, nz = new(seed)
    flat_ground(cv, nz, '#3e2620', var=0.1, tint2='#2a1a18', tint_amt=0.5, gradient=0.12)
    stroke_texture(cv, rng, nz, [('#5a3424', 3), ('#2a1814', 2), ('#7a4a2c', 1), ('#8a3a1e', 0.8)], n=1200,
                   length=(1.5, 3.5), alpha=0.5, flow=0.2, spread=1.5)
    for i in range(160):
        x, y = random_in_hex(rng, 2)
        cv.fill(('ellipse', x, y, 0.7, 0.5), C(RUST_FUNGUS[rng.integers(0, 3)]), 0.7)
    if v == 1:
        cv.ground_clip = True
        clearing = blob_pts(CX, CY + 10, 46, 28, nz, 0.25, phase=3)
        cv.fill(('poly', clearing), C('#5a3a28'), 0.8)
        cv.fill(('line', smooth_path([(CX - R, CY + 50), (CX - 30, CY + 24), (CX + 10, CY + 10)]), 5), C('#6a4430'), 0.7)
        cv.ground_clip = False
    relief(cv, gentle_hills(nz, amp=6.0), light=RELIEF_LIGHT, mult=(1.08, 0.95, 0.78, 0.62), ink=0.35, hatch=0.0, zmin=1e9)
    sc = Scene(cv, nz, rng)
    avoid = []
    if v == 1:
        avoid.append((CX, CY + 10, 40))
        for k in range(3):
            x, y = CX + rng.uniform(-60, 60), CY + rng.uniform(-50, -20)
            sc.add(y + 0.5, (lambda x=x, y=y: glowing_eyes(cv, x, y - 3, 1.4)))
        figure(sc, CX - 6, CY + 16, s=2.0, body='#6a5a3a', pose=1)
    elif v == 2:
        car(sc, CX - 10, CY + 24, 0.4, color='#9a5a3a', fins=True, rust=0.9, s=2.4)
        dead_tree(sc, CX - 8, CY + 22, 58, rng, color='#2a1816', spread=1.1, fungus=RUST_FUNGUS, noise=nz)
        avoid.append((CX - 10, CY + 24, 26))
    elif v == 3:
        house(sc, CX + 4, CY + 12, 40, 24, 15, -0.3, wall='#8a8a7a', roof='#4a3a3a', ruined=0.75, rng=rng, overgrown=0.3)
        avoid.append((CX + 4, CY + 12, 38))
    elif v == 4:
        for (x, y, h, r) in [(CX - 30, CY + 20, 36, 18), (CX + 34, CY - 10, 46, 22), (CX + 10, CY + 50, 22, 12)]:
            mushroom(sc, x, y, h, r, rng, cap=['#c8423a', '#e07a3a', '#a8322a'][rng.integers(3)], glow=True)
            avoid.append((x, y, r + 10))
    elif v == 5:
        plane(sc, CX + 6, CY + 12, 150, -0.35, kind='prop', color='#8a8a7a', broken=True, overgrown=1.0, rng=rng, lost_wing=True)
        avoid += [(CX, CY + 10, 44), (CX - 40, CY, 26), (CX + 50, CY + 20, 26)]
    elif v == 6:
        water_tower(sc, CX + 6, CY + 10, s=1.6)
        avoid.append((CX + 6, CY + 10, 30))
    elif v == 7:
        # a hut on stilts in the rust, one window lit: someone lives out here
        for (dx, dy) in [(-8, 4), (8, 4), (-8, -4), (8, -4)]:
            pole(sc, CX + dx, CY + 10 + dy, h=14, cross=False, color='#3a2418')
        house(sc, CX, CY + 4, 24, 16, 9, 0.2, wall='#6a4a36', roof='#3a2a24', rng=rng, lit=0.6, windows=True)
        def lamp():
            cv.fill(('ellipse', CX - 14, CY - 20, 12, 10), C('#ffb050'), 0.12, mode='glow')
        sc.add(CY + 20, lamp)
        avoid.append((CX, CY + 8, 30))
    n = {0: 22, 1: 16, 2: 14, 3: 15, 4: 12, 5: 12, 6: 14, 7: 15}[v]
    def in_front(x, y):
        # a tree rooted in front of (below) a focal prop hides it completely
        return any(abs(x - ax) < ar * 1.1 and ay - ar * 0.4 < y < ay + ar * 2.4 for ax, ay, ar in avoid)
    pts = [p for p in scatter(rng, n + 10, 5, avoid, 16) if not in_front(*p)][:n]
    for (x, y) in pts:
        with sc.at(x, y) as Y:
            dead_tree(sc, x, Y, rng.uniform(32, 56), rng, color='#2a1816', spread=1.15, fungus=RUST_FUNGUS, noise=nz)
    if v in (0, 3, 6):
        x, y = random_in_hex(rng, 24)
        sc.add(y + 0.5, (lambda x=x, y=y: glowing_eyes(cv, x, y - 3, 1.4)))
    rim(cv)
    return finish(sc)


# ── 7 Ridge ───────────────────────────────────────────────────────────
RIDGE_GROUND = '#6a5a44'


def ridge_H(nz, rng, pts, height=48, width=26):
    X, Y = grid_ground()
    d = np.full(X.shape, 1e9, np.float32)
    for (x0, y0), (x1, y1) in zip(pts[:-1], pts[1:]):
        dx, dy = x1 - x0, y1 - y0
        L2 = dx * dx + dy * dy
        t = np.clip(((X - x0) * dx + (Y - y0) * dy) / L2, 0, 1)
        px, py = x0 + t * dx, y0 + t * dy
        d = np.minimum(d, np.sqrt((X - px) ** 2 + ((Y - py) * 1.25) ** 2))
    crest = np.exp(-(d / width) ** 2 * 1.6)
    rn = ridged(nz, X, Y, 0.05, sharp=2.4)
    along = 0.75 + 0.25 * nz.fbm(X * 0.02 + 9, Y * 0.02, 2)
    H = height * crest * along * (0.7 + 0.5 * rn)
    return H * rim_falloff(X, Y, inner=20)


def ridge(v, seed):
    cv, rng, nz = new(seed)
    flat_ground(cv, nz, RIDGE_GROUND, var=0.1, tint2='#7a6a52', tint_amt=0.3, gradient=0.12)
    stroke_texture(cv, rng, nz, [('#8a7458', 3), ('#3e3226', 2.5), ('#a08a68', 1), ('#5a4a3a', 1)], n=1500,
                   length=(1.5, 4), alpha=0.55, flow=0.0, spread=0.4)
    for i in range(80):
        x, y = random_in_hex(rng, 3)
        cv.fill(('ellipse', x, y, rng.uniform(0.6, 1.6), rng.uniform(0.4, 1.0)), C('#3a3026'), 0.7)
    pts = [(CX - R + 10, CY + 36 + rng.uniform(-10, 10)), (CX - 30, CY + 4 + rng.uniform(-10, 10)),
           (CX + 30, CY - 8 + rng.uniform(-10, 10)), (CX + R - 10, CY - 30 + rng.uniform(-10, 10))]
    H = ridge_H(nz, rng, pts, height=52 if v != 3 else 40, width=28)
    relief(cv, H, light=RELIEF_LIGHT, bands=(0.80, 0.55, 0.30), mult=(1.14, 0.94, 0.68, 0.48), strata=0.3, strata_step=6)
    sc = Scene(cv, nz, rng)
    crest = smooth_path(pts, n=30)
    def on_crest(t):
        i = int(t * (len(crest) - 1))
        return crest[i]
    if v == 1:
        x, y = on_crest(0.55)
        with sc.at(x, y) as Y:
            lookout_tower(sc, x, Y, 40, rng)
    elif v == 2:
        x, y = on_crest(0.4)
        with sc.at(x, y) as Y:
            _cairn(sc, x, Y, rng)
        x2, y2 = on_crest(0.7)
        with sc.at(x2, y2) as Y2:
            radio_mast(sc, x2, Y2, 60, rng, lean=-0.06)
    elif v == 3:
        # the ridge is a spine: vertebrae along the crest, ribs down the flanks
        for t in np.linspace(0.08, 0.92, 11):
            x, y = on_crest(t)
            with sc.at(x, y) as Y:
                _vertebra(sc, x, Y, rng, t)
        x, y = on_crest(0.97)
        with sc.at(x, y) as Y:
            skull(sc, x - 4, Y + 6, s=3.6)
    elif v == 4:
        x, y = on_crest(0.5)
        with sc.at(x, y) as Y:
            jet(sc, x, Y, 70, -0.45, rng)
    for i in range(8):
        x, y = random_in_hex(rng, 10)
        with sc.at(x, y) as Yl:
            rock(sc, x, Yl, rng.uniform(3, 6), color='#8a7458', rng=rng)
    for i in range(3):
        x, y = random_in_hex(rng, 12)
        with sc.at(x, y) as Yl:
            dead_tree(sc, x, Yl, rng.uniform(14, 22), rng, color='#2a2220', spread=0.8, lean=rng.uniform(-0.3, 0.3))
    rim(cv)
    return finish(sc)


def _cairn(sc, X, Y, rng):
    cv = sc.cv

    def draw():
        y = Y
        for k, r in enumerate([6, 5, 4, 3.2, 2.4]):
            st = blob_pts(X + rng.uniform(-0.8, 0.8), y - r * 0.5, r, r * 0.6, sc.noise, 0.2, phase=k * 7 + X)
            cv.fill(('poly', st), INK, grow=0.6)
            cv.fill(('poly', st), C('#9a8a70'))
            cv.fill(('poly', blob_pts(X - r * 0.3, y - r * 0.7, r * 0.5, r * 0.3, sc.noise, 0.2, phase=k)), C('#c0b090'), 0.8)
            y -= r * 1.0
    sc.add(Y, draw)


def _vertebra(sc, X, Y, rng, t):
    cv = sc.cv
    bone = ('#e0d4b8', '#b8ac90', '#7a7060')

    def draw():
        s = 1.0 + 0.6 * math.sin(math.pi * t)
        body = blob_pts(X, Y - 4 * s, 5 * s, 3.4 * s, sc.noise, 0.12, phase=t * 50)
        spine = [(X - 1.2 * s, Y - 6 * s), (X + 1.2 * s, Y - 6 * s), (X + 0.4 * s, Y - 15 * s), (X - 0.6 * s, Y - 15 * s)]
        ribs = [('line', [(X - 4 * s, Y - 4 * s), (X - 10 * s, Y - 1 * s), (X - 13 * s, Y + 5 * s)], 1.3 * s),
                ('line', [(X + 4 * s, Y - 4 * s), (X + 10 * s, Y - 1 * s), (X + 13 * s, Y + 5 * s)], 1.3 * s)]
        cv.fill([('poly', body), ('poly', spine)] + ribs, INK, grow=0.7)
        cv.fill(ribs, C(bone[1]))
        cv.fill(('poly', spine), C(bone[1]))
        cv.fill(('poly', body), C(bone[0]))
        cv.fill(('line', [(X - 3 * s, Y - 2.5 * s), (X + 3 * s, Y - 2.5 * s)], 0.5), C(bone[2]))
    sc.add(Y, draw)


# ── 8 Mountain ────────────────────────────────────────────────────────
MOUNT_PEAKS = {
    0: [(CX + 12, CY - 10, 84, 110), (CX - 46, CY + 4, 62, 72), (CX + 60, CY + 30, 46, 46), (CX - 10, CY + 46, 42, 30)],
    1: [(CX - 26, CY - 12, 70, 100), (CX + 34, CY - 18, 72, 106), (CX + 64, CY + 36, 40, 40)],
    2: [(CX + 34, CY - 22, 80, 104), (CX - 36, CY - 18, 64, 74), (CX - 62, CY + 40, 36, 28)],
    3: [(CX, CY - 6, 100, 92), (CX - 64, CY + 32, 40, 36), (CX + 64, CY + 24, 44, 42)],
    4: [(CX + 4, CY - 10, 100, 108), (CX - 66, CY + 34, 36, 32), (CX + 66, CY + 36, 36, 30)],
}


def mountain(v, seed):
    cv, rng, nz = new(seed)
    flat_ground(cv, nz, '#5a5068', var=0.1, tint2='#6a4a44', tint_amt=0.3, gradient=0.1)
    stroke_texture(cv, rng, nz, [('#7a6e88', 3), ('#3a3248', 2.5), ('#8a5a44', 0.8), ('#9a90a8', 1)], n=1600,
                   length=(1.5, 4.0), alpha=0.55, flow=0.0, spread=0.3)
    X, Y = grid_ground()
    peaks = MOUNT_PEAKS[v % len(MOUNT_PEAKS)]
    H = massif(nz, X, Y, peaks, rough=0.55, scale=0.034, octaves=3, sharp=1.6)
    if v == 4:
        # a sheer cut face on the front of the main peak for the carving
        face = np.exp(-(((X - CX - 4) / 30) ** 2 + ((Y - CY - 18) / 22) ** 2))
        H = np.maximum(H, H * (1 - face) + face * 70)
    H = H * rim_falloff(X, Y, inner=18)
    relief(cv, H, light=RELIEF_LIGHT, bands=(0.80, 0.55, 0.30), mult=(1.16, 0.92, 0.66, 0.46), strata=0.35, strata_step=8)
    sc = Scene(cv, nz, rng)
    if v == 1:
        x, y = CX - 34, CY + 50
        with sc.at(x, y) as Yl:
            cave(sc, x, Yl, 12, rng, glow=True)
        with sc.at(x + 26, y + 10) as Yl:
            mine_cart(sc, x + 26, Yl, rng)
    elif v == 2:
        pts = []
        for t in np.linspace(0.0, 1.0, 5):
            gx = CX - 70 + t * 90
            gy = CY + 60 - t * 70
            pts.append((gx, gy))
        lifted = []
        for gx, gy in pts:
            yi = int(min(max(gy * S, 0), H.shape[0] - 1))
            xi = int(min(max(gx * S, 0), H.shape[1] - 1))
            lifted.append((gx, gy - float(H[yi, xi]) * FZ))
        with sc.at(pts[0][0], pts[0][1]):
            chairlift(sc, lifted, rng, h=18)
    elif v == 3:
        yi, xi = np.unravel_index(np.argmax(H), H.shape)
        sx, sy = (xi + 0.5) / S, (yi + 0.5) / S
        with sc.at(sx, sy + 4) as Yl:
            dome_observatory(sc, sx, Yl + 2, 12, rng, broken=True)
    elif v == 4:
        _carved_face(sc, CX + 4, CY + 18 - 70 * FZ * 0.55, rng)
    for i in range(8):
        x, y = random_in_hex(rng, 10, band=(CY + 52, HEX_BOT - 4))
        with sc.at(x, y) as Yl:
            rock(sc, x, Yl, rng.uniform(3, 6), color='#6e6488', rng=rng)
    rim(cv)
    return finish(sc)


def _carved_face(sc, X, Y, rng):
    """The founder, sixty feet high, carved into the slag. Still smiling."""
    cv = sc.cv
    stone = (C('#a89cb8'), C('#7a7096'), C('#453e5c'), C('#2a2440'))

    def draw():
        s = 1.9
        # the dressed stone of the cut face, paler than the raw slag round it
        plate = blob_pts(X, Y - 2 * s, 24 * s, 22 * s, sc.noise, 0.08, phase=X)
        cv.fill(('poly', plate), stone[1], 0.9)
        cv.fill(('poly', blob_pts(X - 6 * s, Y - 8 * s, 14 * s, 12 * s, sc.noise, 0.1, phase=X + 2)), stone[0], 0.6, clip=('poly', plate))
        brow = [(X - 20 * s, Y - 12 * s), (X, Y - 16 * s), (X + 20 * s, Y - 12 * s)]
        cv.fill(('line', brow, 3.2 * s), stone[3])
        cv.fill(('line', [(x, y - 1.2) for x, y in brow], 1.6 * s), stone[0])
        for sg in (-1, 1):
            ex = X + sg * 10 * s
            cv.fill(('ellipse', ex, Y - 7 * s, 5 * s, 3.6 * s), stone[3])
            cv.fill(('ellipse', ex, Y - 6.2 * s, 2.2 * s, 1.6 * s), C('#140e18'))
        nose = [(X - 1.5 * s, Y - 10 * s), (X + 2 * s, Y - 10 * s), (X + 5 * s, Y + 3 * s), (X - 4 * s, Y + 3 * s)]
        cv.fill(('poly', nose), stone[1])
        cv.fill(('poly', [nose[1], nose[2], (X + 1 * s, Y + 3 * s)]), stone[3])
        cv.fill(('line', [(X - 4 * s, Y + 3 * s), (X + 5 * s, Y + 3 * s)], 1.2 * s), stone[3])
        grin = [(X - 16 * s, Y + 7 * s), (X - 6 * s, Y + 14 * s), (X + 6 * s, Y + 14 * s), (X + 16 * s, Y + 7 * s)]
        cv.fill(('line', grin, 3.0 * s), stone[3])
        cv.fill(('line', [(x, y - 1.6) for x, y in grin], 1.0 * s), stone[0], 0.8)
        for k in range(6):
            x = X - 10 * s + k * 4 * s
            cv.fill(('line', [(x, Y + 9 * s), (x, Y + 12 * s)], 0.6), stone[3], 0.8)
        # a crack running through the grin and the Evil Corp roundel on the brow
        cv.fill(('line', [(X + 8 * s, Y - 20 * s), (X + 5 * s, Y - 4 * s), (X + 9 * s, Y + 8 * s), (X + 6 * s, Y + 22 * s)], 0.8), INK)
        evil_logo(cv, X, Y - 22 * s, 4.2)
    sc.add(CY + 60, draw)


# ── 10 Nuke Crater ────────────────────────────────────────────────────
def nuke_crater(v, seed):
    cv, rng, nz = new(seed)
    flat_ground(cv, nz, '#3a3428', var=0.12, tint2='#24201a', tint_amt=0.5, gradient=0.1)
    cx0, cy0 = CX + rng.uniform(-6, 6), CY + 4 + rng.uniform(-4, 4)
    rays = []
    for k in range(40):
        a = rng.uniform(0, math.tau)
        r0, r1 = 50, rng.uniform(80, 130)
        rays.append(('line', [(cx0 + math.cos(a) * r0, cy0 + math.sin(a) * r0 * 0.66),
                              (cx0 + math.cos(a) * r1, cy0 + math.sin(a) * r1 * 0.66)], rng.uniform(1, 3)))
    cv.ground_clip = True
    cv.fill(rays, C('#1a1612'), 0.4)
    stroke_texture(cv, rng, nz, [('#4a4232', 3), ('#1e1a16', 2), ('#6a6450', 1)], n=900, length=(1.5, 3.5), alpha=0.45)
    ring = blob_pts(cx0, cy0, 74, 52, nz, 0.06, n=48, phase=3)
    cv.fill(('poly', ring), C('#4e5244'), 0.6)
    pool = blob_pts(cx0, cy0 + 6, 20, 13, nz, 0.15, n=24, phase=9)
    cv.fill(('poly', pool), C('#3aa040'))
    cv.fill(('poly', blob_pts(cx0 - 3, cy0 + 4, 12, 7, nz, 0.2, n=20, phase=11)), C('#9cff6a'))
    X, Y = grid_ground()
    d = np.sqrt(((X - cx0) / 62) ** 2 + ((Y - cy0) / 42) ** 2)
    bowl = -22 * np.clip(1 - d, 0, 1) ** 1.1
    lip = 14 * np.exp(-((d - 1.02) / 0.2) ** 2)
    H = (bowl + lip) * (1 + 0.25 * (ridged(nz, X, Y, 0.06) - 0.5)) * rim_falloff(X, Y, inner=14)
    relief(cv, H, light=RELIEF_LIGHT, bands=(0.78, 0.52, 0.28), mult=(1.16, 0.96, 0.7, 0.5), strata=0.25, strata_step=5)
    sc = Scene(cv, nz, rng)

    def glow():
        gy = cy0 + 6 + 22 * FZ * 0.85
        cv.fill(('ellipse', cx0, gy, 44, 22), C('#8cff5a'), 0.16, mode='glow')
        cv.fill(('ellipse', cx0, gy, 22, 11), C('#b8ff80'), 0.35, mode='glow')
        cv.fill(('ellipse', cx0 - 3, gy - 1, 9, 4), C('#f0ffd0'), 0.5, mode='glow')
    sc.add(0, glow)
    if v == 1:
        for x in (CX - 76, CX + 70):
            with sc.at(x, CY - 22) as Yl:
                girder(sc, x, Yl, rng)
    elif v == 2:
        with sc.at(cx0 + 52, cy0 - 24) as Yl:
            bomb_casing(sc, cx0 + 52, Yl, 42, 0.6, rng)
    elif v == 3:
        with sc.at(cx0 - 10, cy0 - 50) as Yl:
            billboard(sc, cx0 - 10, Yl, w=60, h=22, lift=12, text=('EVIL CORP', 'SITE OF OUR FIRST SUCCESS'), torn=0.4, rng=rng, tilt=-0.06)
    for i in range(10):
        x, y = random_in_hex(rng, 6)
        if math.hypot((x - cx0) / 86, (y - cy0) / 62) > 1:
            with sc.at(x, y) as Yl:
                dead_tree(sc, x, Yl, rng.uniform(12, 20), rng, color='#141010', spread=0.8)
    rim(cv)
    return finish(sc)
