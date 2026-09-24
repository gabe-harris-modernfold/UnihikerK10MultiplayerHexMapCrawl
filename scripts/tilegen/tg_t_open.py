"""Open ground: Open Scrub (0), Ash Dunes (1), Marsh (3), Glass Fields (6),
and the Jack's Chopper landmark."""
import math
import numpy as np
from tg_core import C, mix, shade, hex_pts, hex_sdf, blob_pts, ellipse_pts, CX, CY, R, HEX_TOP, HEX_BOT, HALF_H
from tg_props import (Scene, INK, P, FY, FZ, house, pole, wire, pylon, car, rock, tuft, dead_tree, cactus,
                      foliage, evil_logo, stroke_text, EVIL_RED, CREAM, GREEN_SICK, cylinder)
from tg_props2 import (plane, trailer, barrel, billboard, ribcage, skull, mascot_head, glowing_eyes, reeds,
                       fence, tentacle, shade_tube)
from tg_props3 import (block, googie_sign, ferris_wheel, radio_mast, figure, dome_observatory)
from tg_ground import rim, crack, ruts, road, smooth_path, water, random_in_hex, stroke_texture, flat_ground
from tg_relief import relief, grid_ground, rim_falloff, RELIEF_LIGHT
from tg_common import (inside, scatter, new, finish, gentle_hills, swell, scrub_base, scrub_scatter, SCRUB_TUFT,
                       BUSH, ASPHALT, CAR_COLS, cookfire)


# ── 0 Open Scrub ──────────────────────────────────────────────────────
def scrub(v, seed):
    cv, rng, nz = new(seed)
    scrub_base(cv, nz, rng)
    avoid = []
    sc = Scene(cv, nz, rng)
    if v == 1:
        cv.ground_clip = True
        pts = road(cv, [(CX - R - 4, CY + 34), (CX - 20, CY + 18), (CX + 40, CY + 22), (CX + R + 4, CY + 2)], 13, ASPHALT,
                   None, dash=C('#c8b060'), rng=rng, noise=nz, broken=0.9)
        cv.ground_clip = False
    elif v == 7:
        fur = smooth_path([(CX - 100, CY - 42), (CX - 60, CY - 26), (CX - 24, CY - 8)])
        cv.ground_clip = True
        cv.fill(('line', fur, 9), C('#4a3824'), 0.55)
        cv.fill(('line', fur, 4), C('#2e2218'), 0.6)
        cv.ground_clip = False
    swell(cv, nz, amp=7.0 if v in (0, 2, 3) else 3.0)
    if v == 1:
        billboard(sc, CX - 4, CY - 22, w=78, h=32, lift=20, text=('EVIL CORP', "WE'RE STILL HERE"), tilt=0.12, torn=0.35, rng=rng)
        tops = [pole(sc, x, y, h=26, lean=l) for x, y, l in [(CX - 70, CY + 18, 0.08), (CX - 12, CY + 6, -0.05), (CX + 52, CY + 10, 0.14)]]
        sc.add(999, lambda: [wire(cv, (a[0], a[1] + 2.5), (b[0], b[1] + 2.5), sag=6) for a, b in zip(tops[:-1], tops[1:])])
        car(sc, CX + 30, CY + 26, 0.1, color='#8a3a3a', fins=True, rust=0.8, s=1.5)
        avoid += [(CX - 4, CY - 22, 44), (CX, CY + 24, 30)]
    elif v == 2:
        for i in range(4):
            x, y = random_in_hex(rng, 26)
            with sc.at(x, y) as Y:
                rock(sc, x, Y, rng.uniform(6, 11), color='#8a7258', rng=rng)
            avoid.append((x, y, 18))
        with sc.at(CX + 32, CY + 20) as Y:
            dead_tree(sc, CX + 32, Y, 40, rng)
        for (x, y) in scatter(rng, 3, 20, avoid, 20):
            with sc.at(x, y) as Y:
                cactus(sc, x, Y, rng.uniform(16, 24), rng)
            avoid.append((x, y, 14))
        avoid.append((CX + 32, CY + 20, 16))
    elif v == 3:
        fence(sc, [(CX - 80, CY - 16), (CX - 24, CY - 38), (CX + 40, CY - 30), (CX + 90, CY - 6)], rng=rng, broken=0.4, h=5)
        skull(sc, CX + 8, CY + 24, s=2.6)
        cactus(sc, CX - 44, CY + 30, 26, rng)
        avoid += [(CX + 8, CY + 24, 20), (CX - 44, CY + 30, 16)]
    elif v == 4:
        pylon(sc, CX - 24, CY + 20, h=72, lean=0.04)
        pylon(sc, CX + 64, CY - 40, h=60, lean=0.0, broken=True)
        def wires():
            a = (CX - 24 + 72 * 0.04 - 12, CY + 20 - 72 * FZ * 0.86)
            wire(cv, a, (CX + 64 - 12, CY - 40 - 60 * FZ * 0.72 + 6), sag=10, w=0.5)
            wire(cv, (a[0] + 24, a[1]), (CX + 64 + 12, CY - 40 - 60 * FZ * 0.72 + 6), sag=12, w=0.5)
            wire(cv, (a[0], a[1]), (CX - 110, CY + 60), sag=8, w=0.5)
        sc.add(999, wires)
        avoid += [(CX - 24, CY + 20, 18), (CX + 64, CY - 40, 18)]
    elif v == 5:
        house(sc, CX - 22, CY - 6, 44, 26, 16, 0.25, wall='#c2b08a', roof='#6a3a30', ruined=0.45, chimney=True, rng=rng)
        _windpump(sc, CX + 46, CY - 26, 64, rng)
        car(sc, CX + 26, CY + 44, -0.5, color='#5f9f96', fins=True, rust=0.6, s=1.9)
        fence(sc, [(CX - 84, CY + 26), (CX - 40, CY + 54), (CX + 4, CY + 66)], rng=rng, broken=0.35, h=5)
        avoid += [(CX - 22, CY - 6, 38), (CX + 46, CY - 26, 16), (CX + 26, CY + 44, 20)]
    elif v == 6:
        trailer(sc, CX - 18, CY + 6, 0.3, L=64, rng=rng)
        car(sc, CX + 42, CY + 34, -0.9, color='#a85a3a', fins=True, rust=0.7, s=1.9)
        for i, (bx, by) in enumerate([(CX + 18, CY - 30), (CX + 27, CY - 23), (CX + 20, CY - 18)]):
            barrel(sc, bx, by, s=2.2, color=['#3e5a3a', '#6a3a2a', '#2a4a5a'][i], toxic=i == 0)
        # clothesline with laundry nobody will collect
        a, b = (CX - 64, CY - 30), (CX - 10, CY - 46)
        pole(sc, a[0], a[1], h=14, cross=False)
        pole(sc, b[0], b[1], h=14, cross=False)
        def line():
            p0, p1 = (a[0], a[1] - 14 * FZ), (b[0], b[1] - 14 * FZ)
            wire(cv, p0, p1, sag=3, w=0.5)
            for t, col in [(0.25, '#c8b8a0'), (0.45, '#8a3a3a'), (0.7, '#4a6a8a')]:
                x = p0[0] + (p1[0] - p0[0]) * t
                y = p0[1] + (p1[1] - p0[1]) * t + 3 * 4 * t * (1 - t)
                cv.fill(('poly', [(x - 2, y), (x + 2, y), (x + 2.3, y + 5), (x - 2.2, y + 4.6)]), INK, grow=0.4)
                cv.fill(('poly', [(x - 2, y), (x + 2, y), (x + 2.3, y + 5), (x - 2.2, y + 4.6)]), C(col))
        sc.add(999, line)
        cookfire(sc, CX + 8, CY + 34, 0.8)
        avoid += [(CX - 18, CY + 6, 40), (CX + 42, CY + 34, 22), (CX + 22, CY - 24, 14)]
    elif v == 7:
        plane(sc, CX + 4, CY + 10, 168, 0.42, kind='bomber', color='#6d6f4c', broken=True, overgrown=0.9, rng=rng)
        avoid += [(CX, CY, 40), (CX - 50, CY - 16, 26), (CX + 50, CY + 30, 26)]
    elif v == 8:
        _drive_in(sc, rng)
        avoid += [(CX, CY - 24, 44), (CX, CY + 16, 34)]
    elif v == 9:
        mascot_head(sc, CX + 4, CY + 22, r=44, rng=rng)
        avoid += [(CX, CY + 10, 50)]
    scrub_scatter(sc, rng, nz, tufts=70 if v == 0 else 45, bushes=8 if v == 0 else 4, avoid=avoid, avoid_r=16)
    rim(cv)
    return finish(sc)


def _windpump(sc, X, Y, h, rng):
    cv = sc.cv
    sc.shadow(('line', [(X, Y), (X + 22, Y - 8)], 3))

    def draw():
        top = (X, Y - h * FZ)
        for sg in (-1, 1):
            cv.fill(('line', [(X + sg * 8, Y), top], 1.9), INK)
            cv.fill(('line', [(X + sg * 8, Y), top], 0.8), C('#6a5a4a'))
        for t in (0.3, 0.6):
            a = (X - 8 * (1 - t), Y - h * FZ * t)
            b = (X + 8 * (1 - t), Y - h * FZ * t)
            cv.fill(('line', [a, b], 0.7), INK)
        for k in range(16):
            if k in (3, 9, 10):
                continue
            a = math.tau * k / 16
            p0 = (top[0] + math.cos(a) * 2, top[1] + math.sin(a) * 2)
            p1 = (top[0] + math.cos(a) * 13, top[1] + math.sin(a) * 13)
            cv.fill(('line', [p0, p1], 2.4), INK)
            cv.fill(('line', [p0, p1], 1.3), C('#a09070'))
        vane = [(top[0] + 1, top[1]), (top[0] + 16, top[1] - 4), (top[0] + 16, top[1] + 4)]
        cv.fill(('poly', vane), INK, grow=0.6)
        cv.fill(('poly', vane), C('#b8423a'))
    sc.add(Y, draw)


def _drive_in(sc, rng):
    cv = sc.cv
    X, Y = CX - 4, CY - 30
    w, h, lift = 104, 48, 10
    sc.shadow(('poly', [(X - w / 2, Y), (X + w / 2, Y), (X + w / 2 + 24, Y - 7), (X - w / 2 + 24, Y - 7)]))

    def draw():
        for lx in (-w * 0.4, -w * 0.13, w * 0.15, w * 0.42):
            cv.fill(('line', [(X + lx, Y), (X + lx, Y - (lift + h) * FZ)], 2.6), INK)
            cv.fill(('line', [(X + lx, Y), (X + lx, Y - (lift + h) * FZ)], 1.2), C('#4a3a30'))
        scr = [(X - w / 2, Y - lift * FZ), (X + w / 2, Y - lift * FZ), (X + w / 2, Y - (lift + h) * FZ), (X - w / 2, Y - (lift + h) * FZ)]
        cv.fill(('poly', scr), INK, grow=1.4)
        cv.fill(('poly', scr), C('#d8d0c0'))
        for k in range(6):
            u = rng.uniform(0.05, 0.9)
            vv = rng.uniform(0.0, 0.7)
            pw = rng.uniform(0.08, 0.16)
            ph = rng.uniform(0.15, 0.3)
            hole = [(X - w / 2 + w * u, Y - (lift + h * vv) * FZ), (X - w / 2 + w * (u + pw), Y - (lift + h * vv) * FZ),
                    (X - w / 2 + w * (u + pw), Y - (lift + h * (vv + ph)) * FZ), (X - w / 2 + w * u, Y - (lift + h * (vv + ph)) * FZ)]
            cv.fill(('poly', hole), C('#2a2024'), clip=('poly', scr))
        for k in range(8):
            u = rng.uniform(0, 1)
            cv.fill(('line', [(X - w / 2 + w * u, Y - (lift + h) * FZ), (X - w / 2 + w * u + rng.uniform(-2, 2), Y - (lift + h * 0.3) * FZ)], 1.2),
                    C('#8a7a62'), 0.5, clip=('poly', scr))
        evil_logo(cv, X + w * 0.1, Y - (lift + h * 0.58) * FZ, 10)
        stroke_text(cv, 'TONIGHT', X + w * 0.1, Y - (lift + h * 0.2) * FZ, 4.4, C('#2a2226'), width=1.0, ink=False)
    sc.add(Y, draw)
    for row in range(3):
        for k in range(8):
            x = CX - 62 + k * 17 + row * 5
            y = CY + 10 + row * 20 + math.sin(k * 0.6) * 2
            if inside(x, y, 8):
                pole(sc, x, y, h=6, cross=False, color='#6a5a4a')
    car(sc, CX - 30, CY + 32, 0.0, color='#7a6a9a', fins=True, rust=0.6, s=1.5)


def jacks_chopper(seed):
    """POI 0_10: the military helicopter nose-down in the scrub, rusted
    orange, and Jack in his very pink romper not-crying beside it."""
    cv, rng, nz = new(seed)
    scrub_base(cv, nz, rng)
    fur = smooth_path([(CX - 90, CY - 40), (CX - 50, CY - 20), (CX - 10, CY - 4)])
    cv.ground_clip = True
    cv.fill(('line', fur, 8), C('#4a3824'), 0.5)
    cv.ground_clip = False
    swell(cv, nz, amp=4.0)
    sc = Scene(cv, nz, rng)
    from tg_props3 import helicopter
    helicopter(sc, CX + 4, CY + 6, 120, 0.35, rng)
    figure(sc, CX - 40, CY + 44, s=2.0, body='#ff7ab8', head='#e8c098', pose=0)
    # his toolkit, open
    def kit():
        x, y = CX - 28, CY + 48
        box = [(x - 4, y), (x + 4, y - 1), (x + 4, y - 4), (x - 4, y - 3)]
        cv.fill(('poly', box), INK, grow=0.5)
        cv.fill(('poly', box), C('#b8231c'))
    sc.add(CY + 48, kit)
    scrub_scatter(sc, rng, nz, tufts=45, bushes=4, avoid=[(CX, CY, 50), (CX - 36, CY + 44, 14)], avoid_r=16)
    rim(cv)
    return finish(sc)


# ── 1 Ash Dunes ───────────────────────────────────────────────────────
ASH_STROKES = [('#8a8884', 3), ('#4a4a4e', 2), ('#a6a29a', 1)]


def ash_base(cv, nz, rng, amp=8.0, phase=0.0):
    flat_ground(cv, nz, '#6a6866', var=0.06, tint2='#5a5654', tint_amt=0.3, gradient=0.14)
    stroke_texture(cv, rng, nz, ASH_STROKES, n=1300, length=(2.5, 6), width=(0.4, 0.8), alpha=0.4, flow=-0.2, spread=0.12)
    X, Y = grid_ground()
    ph = (Y * 0.9 + X * 0.3) / 52.0 + (nz.fbm(X * 0.014, Y * 0.014, 3) - 0.5) * 1.3 + phase
    f = ph % 1.0
    saw = np.where(f < 0.72, f / 0.72, (1 - f) / 0.28)
    saw = saw * saw * (3 - 2 * saw)
    H = saw * amp * rim_falloff(X, Y, inner=22)
    relief(cv, H, light=RELIEF_LIGHT, bands=(0.84, 0.62, 0.36), mult=(1.08, 0.98, 0.86, 0.76), ink=0.3, hatch=0.0,
           zmin=1e9)


def ash_drift(sc, X, Y, rx, ry, rng):
    """A tongue of ash banked against the front of a buried thing."""
    cv = sc.cv

    def draw():
        d = blob_pts(X, Y, rx, ry, sc.noise, 0.3, phase=X + Y)
        cv.fill(('poly', d), C('#7a7874'))
        cv.fill(('poly', blob_pts(X - rx * 0.1, Y - ry * 0.3, rx * 0.8, ry * 0.5, sc.noise, 0.3, phase=X + 5)), C('#9a9690'), clip=('poly', d))
        cv.fill(('line', [(X - rx * 0.8, Y - ry * 0.5), (X + rx * 0.7, Y - ry * 0.7)], 0.6), C('#b8b4ac'), 0.8, clip=('poly', d))
    sc.add(Y + ry * 0.5, draw)


def ash_dunes(v, seed):
    cv, rng, nz = new(seed)
    ash_base(cv, nz, rng, amp=6.0, phase=rng.uniform(0, 1))
    sc = Scene(cv, nz, rng)
    if v == 1:
        ribcage(sc, CX, CY + 12, L=150, yaw=0.2, rng=rng, ribs=10, h=38)
        skull(sc, CX + 82, CY - 6, s=3.4, horns=True)
    elif v == 2:
        for (x, y, yaw, col) in [(CX - 30, CY - 6, 0.4, '#5f9f96'), (CX + 38, CY + 26, -0.3, '#c86a4a')]:
            car(sc, x, y, yaw, color=col, fins=True, rust=0.7, s=2.6)
            ash_drift(sc, x + 2, y + 8, 22, 7, rng)
    elif v == 3:
        block(sc, CX - 16, CY + 18, 34, 22, 10, 0.15, wall='#8a8478', roof='#4a4644', rows=1, cols=3, rng=rng,
              sign=('KWSL', '#b8231c', '#e8dcc0'), broken=0.9)
        ash_drift(sc, CX - 16, CY + 30, 34, 9, rng)
        ash_drift(sc, CX + 6, CY + 24, 16, 6, rng)
        radio_mast(sc, CX + 30, CY + 4, 96, rng, lean=0.16)
    elif v == 4:
        googie_sign(sc, CX + 10, CY + 18, 78, 'MOTEL', rng, lit=0.0)
        ash_drift(sc, CX + 10, CY + 22, 26, 8, rng)
    elif v == 5:
        ferris_wheel(sc, CX - 2, CY + 20, 46, rng, sunk=0.55, lean=0.08)
        ash_drift(sc, CX - 2, CY + 26, 60, 12, rng)
    rim(cv)
    return finish(sc)


# ── 3 Marsh ───────────────────────────────────────────────────────────
MARSH_WATER = (C('#132420'), C('#223a30'), C('#8aa08a'))


def marsh_base(cv, nz, rng, pools=7, big=False):
    flat_ground(cv, nz, '#4a4a30', var=0.1, tint2='#3a4a36', tint_amt=0.5, gradient=0.12)
    stroke_texture(cv, rng, nz, [('#6a6844', 3), ('#2e3020', 2), ('#8a8456', 1)], n=1000, length=(1.5, 4), alpha=0.5, flow=-1.5, spread=0.4)
    cv.ground_clip = True
    shapes = []
    for i in range(pools):
        x, y = random_in_hex(rng, 14)
        s = 1.6 if big else 1.0
        shapes.append(('poly', blob_pts(x, y, rng.uniform(14, 30) * s, rng.uniform(8, 15) * s, nz, 0.35, phase=x)))
    water(cv, shapes, *MARSH_WATER, nz, rng, ripples=24, glints=10)
    for sh in shapes:
        cv.fill(('line', sh[1] + [sh[1][0]], 0.8), C('#b8b08a'), 0.35)
    # salt crust flecks
    for i in range(60):
        x, y = random_in_hex(rng, 3)
        cv.fill(('ellipse', x, y, rng.uniform(0.6, 1.4), 0.5), C('#d8d0b0'), 0.35)
    return shapes


def marsh(v, seed):
    cv, rng, nz = new(seed)
    pools = marsh_base(cv, nz, rng, pools=8 if v in (0, 5) else 6, big=v == 5)
    sc = Scene(cv, nz, rng)
    avoid = []
    if v == 1:
        for (x, y) in scatter(rng, 7, 14, [], 24):
            dead_tree(sc, x, y, rng.uniform(28, 44), rng, color='#221e1c', spread=0.95)
        _mist(sc, rng, n=4)
        x, y = random_in_hex(rng, 26)
        sc.add(y, lambda: glowing_eyes(cv, x, y, 1.3, color='#c8ff6a'))
    elif v == 2:
        for i in range(4):
            x, y = random_in_hex(rng, 24)
            _cart(sc, x, y, rng.uniform(-0.8, 0.8), rng)
            avoid.append((x, y, 12))
        car(sc, CX + 20, CY - 4, 0.6, color='#7a6a9a', fins=True, rust=0.8, s=2.0)
        avoid.append((CX + 20, CY - 4, 22))
    elif v == 3:
        _boardwalk(sc, [(CX - R + 8, CY + 26), (CX - 30, CY + 6), (CX + 20, CY + 14), (CX + R - 8, CY - 16)], rng)
        avoid += [(CX, CY + 10, 20)]
    elif v == 4:
        _back_nine(sc, rng)
        avoid += [(CX, CY, 60)]
    elif v == 5:
        for k in range(4):
            x = CX - 20 + k * 16 + rng.uniform(-4, 4)
            y = CY + 8 + math.sin(k) * 10
            tentacle(sc, x, y, rng.uniform(26, 44), rng, curl=rng.uniform(0.7, 1.4) * (1 if k % 2 else -1))
        avoid += [(CX + 4, CY + 8, 50)]
    n_reeds = 26 if v in (0, 1) else 16
    for (x, y) in scatter(rng, n_reeds, 5, avoid, 14):
        reeds(sc, x, y, rng.integers(5, 10), rng, h=rng.uniform(7, 13))
    if v == 0:
        for (x, y) in scatter(rng, 3, 14, avoid, 20):
            dead_tree(sc, x, y, rng.uniform(22, 34), rng, color='#262220', spread=0.9)
    rim(cv)
    return finish(sc, shadows=0.3)


def _mist(sc, rng, n=4):
    cv = sc.cv

    def draw():
        for i in range(n):
            y = HEX_TOP + 30 + i * 40 + rng.uniform(-8, 8)
            x = CX + rng.uniform(-20, 20)
            for k in range(7):
                band = blob_pts(x + rng.uniform(-6, 6), y + rng.uniform(-2, 2), R * (0.5 + 0.07 * k), 3 + k * 1.6,
                                sc.noise, 0.35, phase=y + k)
                cv.fill(('poly', band), C('#b8c8b0'), 0.035)
    sc.add(998, draw)


def _cart(sc, X, Y, yaw, rng):
    """Shopping cart, tipped, wheels in the air or wading."""
    cv = sc.cv

    def draw():
        from tg_props import rot as _rot
        w, d, h = 7, 4.5, 4
        pts = [P(X, Y, *_rot(x, y, yaw), z) for x, y, z in [(-w / 2, -d / 2, 0.5), (w / 2, -d / 2, 0), (w / 2, d / 2, 0), (-w / 2, d / 2, 0.5)]]
        top = [P(X, Y, *_rot(x, y, yaw), z) for x, y, z in [(-w / 2, -d / 2, h), (w / 2, -d / 2, h - 0.5), (w / 2, d / 2, h - 0.5), (-w / 2, d / 2, h)]]
        lines = []
        for i in range(4):
            lines.append(('line', [pts[i], top[i]], 0.5))
            lines.append(('line', [top[i], top[(i + 1) % 4]], 0.5))
        for t in (0.33, 0.66):
            a = (pts[0][0] + (pts[1][0] - pts[0][0]) * t, pts[0][1] + (pts[1][1] - pts[0][1]) * t)
            b = (top[0][0] + (top[1][0] - top[0][0]) * t, top[0][1] + (top[1][1] - top[0][1]) * t)
            lines.append(('line', [a, b], 0.35))
        cv.fill(lines, INK, grow=0.3)
        cv.fill(lines, C('#a8a8a0'))
        cv.fill(('line', ellipse_pts(X, Y + 1, 6, 2.2, 14), 0.5), MARSH_WATER[2], 0.6)
    sc.add(Y, draw)


def _boardwalk(sc, ctrl, rng):
    cv = sc.cv
    pts = smooth_path(ctrl, n=60)

    def draw():
        for i in range(0, len(pts) - 1):
            if rng.random() < 0.12:
                continue
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            dx, dy = x1 - x0, y1 - y0
            L = math.hypot(dx, dy) or 1
            nx, ny = -dy / L * 4, dx / L * 4
            plank = [(x0 - nx, y0 - ny - 2), (x0 + nx, y0 + ny - 2), (x1 + nx, y1 + ny - 2), (x1 - nx, y1 - ny - 2)]
            cv.fill(('poly', plank), INK, grow=0.4)
            cv.fill(('poly', plank), C('#8a6a48') if i % 2 else C('#7a5a3c'))
        for i in range(0, len(pts), 6):
            x, y = pts[i]
            for sg in (-1, 1):
                cv.fill(('line', [(x + sg * 3.6, y - 2), (x + sg * 3.6, y + 2)], 1.1), INK)
    sc.add(CY, draw)


def _back_nine(sc, rng):
    """Forty metres of immaculate, violently green fairway that the marsh
    stops dead at the edge of. Hole seven, par four."""
    cv = sc.cv
    fair = blob_pts(CX + 4, CY + 4, 70, 34, sc.noise, 0.12, n=40, phase=4)
    cv.ground_clip = True
    cv.fill(('poly', fair), INK, 0.6, grow=1.0)
    cv.fill(('poly', fair), C('#5ab83a'))
    for k in range(6):
        y = CY - 26 + k * 10
        cv.fill(('line', [(CX - 70, y), (CX + 80, y + 4)], 3.5), C('#6ac846'), 0.5, clip=('poly', fair))
    green = blob_pts(CX + 36, CY - 8, 18, 10, sc.noise, 0.1, phase=9)
    cv.fill(('poly', green), C('#7ad856'), clip=('poly', fair))
    trap = blob_pts(CX - 26, CY + 16, 14, 6, sc.noise, 0.25, phase=13)
    cv.fill(('poly', trap), C('#e0d4a8'), clip=('poly', fair))
    cv.ground_clip = False

    def flag():
        x, y = CX + 38, CY - 8
        cv.fill(('ellipse', x, y, 1.4, 0.7), INK)
        cv.fill(('line', [(x, y), (x, y - 22)], 1.4), INK)
        cv.fill(('line', [(x, y), (x, y - 22)], 0.6), C('#e8e0d0'))
        fl = [(x, y - 22), (x + 9, y - 19.5), (x, y - 17)]
        cv.fill(('poly', fl), INK, grow=0.5)
        cv.fill(('poly', fl), C('#d8281c'))
        stroke_text(cv, '7', x + 3.5, y - 19.5, 2.4, CREAM, width=0.5, ink=False)
    sc.add(CY - 8, flag)
    # the groundskeeper's cart
    from tg_props3 import block as _block
    car(sc, CX - 50, CY - 10, 0.2, color='#e8e0d0', rust=0.2, s=1.0)
    figure(sc, CX - 40, CY - 4, s=2.0, body='#3a6a3a', head='#d8b890', hat='#e8e0d0')


# ── 6 Glass Fields ────────────────────────────────────────────────────
def glass_base(cv, nz, rng, cracks=True):
    flat_ground(cv, nz, '#1e1a2c', var=0.14, tint2='#2a2238', tint_amt=0.6, gradient=0.2)
    cv.ground_clip = True
    # iridescent sheen: broad soft bands of cyan and magenta laid across
    for k in range(7):
        a = rng.uniform(-0.5, 0.5)
        y = rng.uniform(HEX_TOP, HEX_BOT)
        col = C(['#5ad8d8', '#d85ac8', '#8a7ae8', '#5ae89a'][k % 4])
        band = [(CX - R - 10, y), (CX + R + 10, y + a * 60), (CX + R + 10, y + a * 60 + rng.uniform(4, 12)), (CX - R - 10, y + rng.uniform(4, 12))]
        cv.fill(('poly', band), col, 0.10)
    stroke_texture(cv, rng, nz, [('#3a3050', 3), ('#141020', 2), ('#6a6090', 0.8)], n=700, length=(3, 9), width=(0.4, 0.8), alpha=0.45,
                   flow=0.1, spread=0.2)
    if cracks:
        # fracture web: long radial cracks from a few impact stars, rings
        for s in range(rng.integers(2, 4)):
            ox, oy = random_in_hex(rng, 24)
            for k in range(rng.integers(6, 10)):
                a = rng.uniform(0, math.tau)
                pts = [(ox, oy)]
                L = rng.uniform(30, 90)
                for i in range(int(L / 6)):
                    a += rng.uniform(-0.25, 0.25)
                    pts.append((pts[-1][0] + math.cos(a) * 6, pts[-1][1] + math.sin(a) * 6 * 0.7))
                cv.fill(('line', pts, 0.9), C('#0a0810'), 0.9)
                cv.fill(('line', [(x - 0.4, y - 0.5) for x, y in pts], 0.45), C('#b8e8f0'), 0.55)
            for k in range(2):
                ring = ellipse_pts(ox, oy, 8 + k * 9, (8 + k * 9) * 0.66, 24)
                cv.fill(('line', ring + [ring[0]], 0.5), C('#b8e8f0'), 0.35)
    # hot glints
    for i in range(24):
        x, y = random_in_hex(rng, 4)
        L = rng.uniform(1.5, 4)
        cv.fill(('line', [(x - L, y), (x + L, y)], 0.5), C('#f0ffff'), 0.7)
        cv.fill(('line', [(x, y - L * 0.6), (x, y + L * 0.6)], 0.4), C('#f0ffff'), 0.5)


def shard(sc, X, Y, h, rng, col='#3a2e5a'):
    cv = sc.cv
    sc.shadow(('poly', [(X - 2, Y), (X + 2, Y), (X + SHADOW_X(h), Y - h * 0.1)]))

    def draw():
        lean = rng.uniform(-0.35, 0.35)
        w = h * rng.uniform(0.18, 0.3)
        tip = (X + lean * h, Y - h * FZ)
        tri = [(X - w, Y + 0.5), (X + w, Y + 0.5), tip]
        cv.fill(('poly', tri), INK, grow=0.8)
        cv.fill(('poly', tri), C(col))
        cv.fill(('poly', [(X - w, Y + 0.5), ((X + tip[0]) / 2 - w * 0.2, (Y + tip[1]) / 2), tip]), C('#6a5a9a'), 0.9)
        cv.fill(('line', [(X - w * 0.6, Y), tip], 0.6), C('#c8f0ff'), 0.8)
    sc.add(Y, draw)


def SHADOW_X(h):
    from tg_props import SHADOW_DX
    return SHADOW_DX * h


def glass_fields(v, seed):
    cv, rng, nz = new(seed)
    glass_base(cv, nz, rng, cracks=v != 1)
    sc = Scene(cv, nz, rng)
    avoid = []
    global _NZ
    _NZ = nz
    if v == 1:
        for (x, y) in scatter(rng, 26, 8, [], 12):
            shard(sc, x, y, rng.uniform(8, 22), rng)
    elif v == 2:
        # the shadows of people who were standing here at the moment, burned in
        cv.ground_clip = True
        for (x, y) in scatter(rng, 3, 34, [], 44):
            _burned_shadow(cv, x, y + 16, rng)
        cv.ground_clip = False
    elif v == 3:
        _melted_car(sc, CX + 4, CY + 10, rng)
        avoid.append((CX + 4, CY + 10, 30))
    elif v == 4:
        dome_observatory(sc, CX - 6, CY + 12, 34, rng, broken=True)
        avoid.append((CX - 6, CY + 12, 44))
    if v != 1:
        for (x, y) in scatter(rng, 6, 10, avoid, 16):
            shard(sc, x, y, rng.uniform(6, 14), rng)
    rim(cv)
    return finish(sc, shadows=0.35)


def _burned_shadow(cv, x, y, rng):
    s = rng.uniform(3.0, 3.8)
    # the flash bleached the glass everywhere except where somebody stood
    cv.fill(('poly', blob_pts(x + 4, y - 12, 18, 14, _NZ, 0.3, phase=x)), C('#8a7aa8'), 0.55)
    cv.fill(('poly', blob_pts(x + 4, y - 12, 12, 10, _NZ, 0.3, phase=x + 3)), C('#b8a8d0'), 0.45)
    a = rng.uniform(-0.3, 0.3)
    col = C('#06040a')
    # a figure-shaped stain stretched away from the blast
    body = [(x - 1.4 * s, y), (x + 1.4 * s, y), (x + 1.6 * s + a * 10, y - 8 * s), (x - 1.2 * s + a * 10, y - 8 * s)]
    head = ('ellipse', x + a * 12, y - 9.6 * s, 1.5 * s, 1.3 * s)
    armL = ('line', [(x - 1.2 * s + a * 5, y - 6.5 * s), (x - 3.6 * s + a * 8, y - 9.5 * s)], 1.0 * s)
    armR = ('line', [(x + 1.3 * s + a * 5, y - 6.5 * s), (x + 3.2 * s + a * 6, y - 3.5 * s)], 1.0 * s)
    cv.fill([('poly', body), head, armL, armR], col, 0.75)
    cv.fill([('poly', body), head, armL, armR], C('#5ad8d8'), 0.10, grow=1.5)


def _melted_car(sc, X, Y, rng):
    cv = sc.cv
    car(sc, X, Y, 0.3, color='#8a8a92', rust=0.3, s=2.8)

    def draw():
        pool = blob_pts(X + 2, Y + 3, 34, 10, sc.noise, 0.3, phase=X)
        cv.fill(('poly', pool), C('#2a2240'), 0.95)
        cv.fill(('line', pool + [pool[0]], 0.6), C('#c8f0ff'), 0.6)
        for k in range(5):
            x = X - 20 + k * 10
            cv.fill(('line', [(x, Y - 4), (x + rng.uniform(-1, 1), Y + 2)], 1.4), C('#6a6a74'), 0.9)
    sc.add(Y + 0.5, draw)
