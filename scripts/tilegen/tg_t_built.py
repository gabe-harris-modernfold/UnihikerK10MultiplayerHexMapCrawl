"""The built world: Broken Urban (4), Flooded District (5), Settlement (9),
and the downtown-core landmark tiles (POI 4_10..4_12)."""
import math
import numpy as np
from tg_core import C, mix, shade, hex_pts, hex_sdf, blob_pts, ellipse_pts, CX, CY, R, HEX_TOP, HEX_BOT, HALF_H
from tg_props import (Scene, INK, P, FY, FZ, rot, house, factory, smokestack, cylinder, water_tower, pole, wire,
                      pylon, car, rock, dead_tree, foliage, evil_logo, stroke_text, EVIL_RED, CREAM, GREEN_SICK,
                      box_geom, draw_faces, Mat, quad_pt, quad_rect, shadow_of)
from tg_props2 import (plane, bus, trailer, dome, tent, palisade, barrel, billboard, fence, shade_tube, glowing_eyes)
from tg_props3 import (block, rubble, parking, gas_station, tall_sign, googie_sign, church, container_stack, pool,
                       ferris_wheel, figure)
from tg_ground import rim, crack, road, smooth_path, water, random_in_hex, stroke_texture, flat_ground
from tg_common import (inside, scatter, new, finish, swell, urban_base, neighbourhood, yard_tree, water_ground,
                       waterline, drowned_house, drowned_tree, cookfire, lamp_string, girder, HOUSE_COLS, ROOF_COLS,
                       CAR_COLS, ASPHALT, FLOOD, WEEDS, YARD_TREE)


def cars_along(sc, rng, pts, n, s=1.3):
    for c in range(n):
        p = pts[rng.integers(2, len(pts) - 2)]
        if inside(p[0], p[1], 12):
            car(sc, p[0] + rng.uniform(-3, 3), p[1] + rng.uniform(-2, 2), rng.uniform(-1, 1),
                color=CAR_COLS[rng.integers(len(CAR_COLS))], fins=rng.random() < 0.5, rust=0.7, s=s)


def weeds_over(sc, rng, nz, n, avoid=()):
    for (x, y) in scatter(rng, n, 10, avoid, 16):
        r = rng.uniform(3, 6)
        sc.shadow(('ellipse', x + 2.4, y + 0.6, r * 1.1, r * 0.5))
        sc.add(y, (lambda x=x, y=y, r=r: foliage(sc.cv, x, y - r * 0.5, r, nz, ramp=WEEDS, phase=x + y, squash=0.7)))


# ── 4 Broken Urban ────────────────────────────────────────────────────
def broken_urban(v, seed):
    cv, rng, nz = new(seed)
    urban_base(cv, nz, rng)
    sc = Scene(cv, nz, rng)
    avoid = []
    if v == 0:
        street = [(CX - R - 4, CY + 20), (CX - 40, CY - 4), (CX + 10, CY + 10), (CX + R + 4, CY - 16)]
        neighbourhood(sc, rng, nz, street, ruin=0.6, over=0.7)
    elif v == 1:
        cv.ground_clip = True
        road(cv, [(CX - R, CY + 58), (CX + R, CY + 42)], 13, ASPHALT, None, dash=C('#a89a70'), rng=rng, noise=nz)
        cv.ground_clip = False
        fence(sc, [(CX - 92, CY + 30), (CX - 62, CY - 58), (CX + 60, CY - 68), (CX + 98, CY + 22)], h=5, color='#7a7a80', rng=rng, broken=0.25)
        factory(sc, CX - 18, CY, 88, 46, 18, 0.12, saw=6, stacks=2, ruined=0.4, rng=rng, logo=True, smoke=True)
        water_tower(sc, CX + 64, CY - 30, s=1.35)
        for i in range(4):
            barrel(sc, CX + 38 + i * 6, CY + 24 + (i % 2) * 4, s=1.7, color=['#3e5a3a', '#6a3a2a', '#2a4a5a', '#5a5a2a'][i], toxic=i in (0, 3))
        car(sc, CX - 68, CY + 38, 0.1, color='#c8b060', rust=0.8, s=1.7)
    elif v == 2:
        # strip mall: a long low block of storefronts, its lot, its sign
        cv.ground_clip = True
        lot = [(CX - 92, CY + 6), (CX + 92, CY - 6), (CX + 96, CY + 56), (CX - 88, CY + 70)]
        parking(cv, lot, rng)
        road(cv, [(CX - R, CY + 80), (CX + R, CY + 66)], 12, ASPHALT, None, dash=C('#a89a70'), rng=rng, noise=nz)
        cv.ground_clip = False
        block(sc, CX - 6, CY - 22, 150, 26, 12, -0.06, wall='#c8bca0', roof='#5a5654',
              awnings=['#b8423a', '#3a6a7a', '#c8a040', '#6a8a4a', '#8a4a7a', '#b8423a'], rng=rng, ruin=0.35,
              sign=None, rows=1)
        tall_sign(sc, CX + 70, CY + 30, 40, ('MALL',), rng, lean=0.08)
        for (x, y) in [(CX - 60, CY + 28), (CX - 30, CY + 42), (CX + 10, CY + 30), (CX + 40, CY + 48), (CX - 70, CY + 52)]:
            car(sc, x, y, rng.uniform(-0.3, 0.3) + math.pi / 2 * (rng.random() < 0.3), color=CAR_COLS[rng.integers(len(CAR_COLS))],
                fins=rng.random() < 0.5, rust=0.8, s=1.5)
        for (x, y) in [(CX - 44, CY + 36), (CX + 26, CY + 40)]:
            pole(sc, x, y, h=20, cross=False, color='#8a8a90')
    elif v == 3:
        fence(sc, [(CX - 96, CY + 20), (CX - 56, CY - 60), (CX + 50, CY - 70), (CX + 100, CY + 10)], h=5, color='#7a7a80', rng=rng, broken=0.3)
        block(sc, CX - 30, CY - 20, 70, 40, 20, 0.1, wall='#8a8a82', roof='#4e4c4a', rows=2, rng=rng, ruin=0.4,
              sign=('EVIL CORP', '#b8231c', '#e8dcc0'))
        container_stack(sc, CX + 48, CY + 6, 0.3, rng, n=4, levels=2)
        container_stack(sc, CX - 20, CY + 46, -0.1, rng, n=3, levels=1)
        car(sc, CX + 20, CY + 40, 0.9, color='#d8a020', rust=0.6, s=1.8)
    elif v == 4:
        # cul-de-sac: houses round a bulb, and in the middle the pool with the
        # neighbour's car in it, nose first, for sixty years
        cv.ground_clip = True
        road(cv, [(CX - R - 4, CY + 50), (CX - 50, CY + 34), (CX - 20, CY + 14)], 11, ASPHALT, None, rng=rng, noise=nz)
        cv.fill(('ellipse', CX + 6, CY + 4, 30, 18), ASPHALT)
        cv.ground_clip = False
        for k, a in enumerate([-2.6, -1.9, -1.2, -0.5, 0.2, 0.9]):
            hx = CX + 6 + math.cos(a) * 62
            hy = CY + 4 + math.sin(a) * 40
            if inside(hx, hy, 16):
                house(sc, hx, hy, rng.uniform(22, 26), rng.uniform(13, 15), rng.uniform(8, 10), a + math.pi / 2,
                      wall=HOUSE_COLS[k % len(HOUSE_COLS)], roof=ROOF_COLS[k % len(ROOF_COLS)], ruined=rng.uniform(0.2, 0.8),
                      overgrown=rng.uniform(0.2, 0.8), rng=rng, chimney=k % 2 == 0)
        pool(sc, cv, CX + 4, CY + 58, 36, 18, -0.1, rng)
        weeds_over(sc, rng, nz, 6, [(CX + 6, CY + 4, 40), (CX + 4, CY + 58, 26)])
    elif v == 5:
        cv.ground_clip = True
        pts = road(cv, [(CX - R - 4, CY + 44), (CX, CY + 30), (CX + R + 4, CY + 40)], 14, ASPHALT, None, dash=C('#c8b060'), rng=rng, noise=nz)
        cv.ground_clip = False
        gas_station(sc, CX - 6, CY - 8, 0.0, rng, ruin=0.5)
        car(sc, CX - 60, CY + 20, 0.2, color='#8a3a3a', fins=True, rust=0.9, s=1.6)
        weeds_over(sc, rng, nz, 5, [(CX - 6, CY - 8, 50)])
    elif v == 6:
        fence(sc, [(CX - 70, CY + 30), (CX - 40, CY - 40), (CX + 60, CY - 44), (CX + 80, CY + 36), (CX - 70, CY + 30)], h=5,
              color='#8a8a90', rng=rng, broken=0.2)
        for i in range(3):
            for j in range(2):
                x = CX - 30 + i * 30 + j * 8
                y = CY - 16 + j * 30
                faces, top, base, rp = box_geom(x, y, 12, 8, 9, 0.0)
                sc.shadow(shadow_of(base, 9))
                sc.add(y, (lambda faces=faces, top=top: draw_faces(cv, [(q, Mat('#6a7a70').pick(n)) for q, n, _ in faces] + [(top, C('#8a9a90'))])))
                cylinder(sc, x - 3, y - 5, 1.6, 9, 15, '#c8b8a0')
                cylinder(sc, x + 3, y - 5, 1.6, 9, 15, '#c8b8a0')
        pylon(sc, CX - 80, CY - 10, h=64)
        pylon(sc, CX + 90, CY - 30, h=58, broken=True)
    elif v == 7:
        church(sc, CX - 20, CY - 10, 0.15, rng, ruin=0.5)
        # graveyard: rows of little stones
        for i in range(4):
            for j in range(5):
                x = CX + 24 + j * 9 + i * 3
                y = CY + 12 + i * 10
                if inside(x, y, 8):
                    sc.add(y, (lambda x=x, y=y: (cv.fill(('poly', [(x - 1.8, y), (x + 1.8, y), (x + 1.8, y - 4), (x, y - 5), (x - 1.8, y - 4)]), INK, grow=0.4),
                                                  cv.fill(('poly', [(x - 1.8, y), (x + 1.8, y), (x + 1.8, y - 4), (x, y - 5), (x - 1.8, y - 4)]), C('#a8a49a')))))
        dead_tree(sc, CX + 70, CY - 20, 40, rng)
        weeds_over(sc, rng, nz, 5, [(CX - 20, CY - 10, 40)])
    elif v == 8:
        cv.ground_clip = True
        parking(cv, [(CX - 86, CY + 24), (CX + 80, CY + 14), (CX + 84, CY + 70), (CX - 80, CY + 80)], rng)
        cv.ground_clip = False
        block(sc, CX - 14, CY - 22, 76, 34, 44, 0.08, wall='#8a9aa4', roof='#4a5058', glass=True, rng=rng, broken=0.5, ruin=0.3)
        # monument sign: EVIL CORP, grinning at an empty lot
        def sign():
            x, y = CX + 56, CY + 30
            slab = [(x - 16, y), (x + 16, y), (x + 16, y - 11), (x - 16, y - 11)]
            cv.fill(('poly', slab), INK, grow=1.0)
            cv.fill(('poly', slab), C('#d8d0c0'))
            evil_logo(cv, x - 10, y - 5.5, 3.2)
            stroke_text(cv, 'EVIL CORP', x + 3, y - 5.5, 3.0, EVIL_RED, width=0.8, ink=False)
        sc.add(CY + 30, sign)
        for (x, y) in [(CX - 50, CY + 40), (CX + 10, CY + 52)]:
            car(sc, x, y, rng.uniform(-0.3, 0.3), color=CAR_COLS[rng.integers(len(CAR_COLS))], rust=0.8, s=1.5)
    elif v == 9:
        # the water tower came down on the street; the grin is still on it
        street = [(CX - R - 4, CY + 36), (CX - 20, CY + 24), (CX + R + 4, CY + 10)]
        neighbourhood(sc, rng, nz, street, ruin=0.7, over=0.5, poles=False)
        _toppled_tower(sc, CX + 4, CY - 20, rng)
    rim(cv)
    return finish(sc)


def _toppled_tower(sc, X, Y, rng):
    cv = sc.cv

    def draw():
        # legs snapped, tank on its side across a roof
        for k in range(4):
            a = (X - 40 + k * 7, Y - 18 + k * 2)
            b = (a[0] + 26, a[1] + 8 + rng.uniform(-2, 2))
            cv.fill(('line', [a, b], 2.2), INK)
            cv.fill(('line', [a, b], 1.0), C('#4a4040'))
        tank = [(X - 8, Y - 4), (X + 30, Y + 4)]
        shade_tube(cv, tank, [15, 15], (C('#e0d8c8'), C('#b8b0a0'), C('#7a746c'), C('#4a4644')))
        cv.fill(('ellipse', X + 31, Y + 4, 9, 15), INK, grow=0.8)
        cv.fill(('ellipse', X + 31, Y + 4, 9, 15), C('#8a8478'))
        evil_logo(cv, X + 10, Y - 2, 6)
        stroke_text(cv, 'EVIL', X + 10, Y + 9, 3.6, EVIL_RED, width=1.0, ink=False)
    sc.add(Y + 30, draw)
    rubble(sc, X - 26, Y + 10, 12, rng)


def city_core(k, seed):
    """Dense downtown: standing hab-block shells three and four floors up,
    window rows blown out, a street canyon in deep shadow between them."""
    cv, rng, nz = new(seed)
    urban_base(cv, nz, rng, weeds=4, base='#58544e')
    cv.ground_clip = True
    road(cv, [(CX - R - 4, CY + 16), (CX + R + 4, CY - 2)], 18, ASPHALT, None, dash=C('#a89a70'), rng=rng, noise=nz)
    road(cv, [(CX + 10, HEX_TOP - 4), (CX - 6, HEX_BOT + 4)], 14, ASPHALT, None, rng=rng, noise=nz)
    cv.ground_clip = False
    sc = Scene(cv, nz, rng)
    towers = [(CX - 52, CY - 34, 40, 28, 92), (CX + 50, CY - 40, 36, 26, 78), (CX - 60, CY + 44, 38, 26, 58),
              (CX + 56, CY + 34, 40, 26, 66)]
    cols = ['#8a8478', '#9a8a7a', '#7a8088', '#8a7a6a']
    for i, (x, y, w, d, h) in enumerate(towers):
        if not inside(x, y, 20):
            continue
        block(sc, x, y, w, d, h, rng.uniform(-0.08, 0.08), wall=cols[i % 4], roof='#4a4644', rng=rng,
              broken=0.8, ruin=0.5, glass=(i == 1 and k == 0))
        _blown_top(sc, x, y, w, h, rng)
    if k == 0:
        # collapsed skybridge between the two back towers
        def bridge():
            a = (CX - 30, CY - 34 - 60 * FZ)
            b = (CX + 30, CY - 40 - 52 * FZ)
            m = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2 + 18)
            for p, q in [(a, (m[0] - 3, m[1])), ((m[0] + 3, m[1] + 3), b)]:
                cv.fill(('line', [p, q], 5.0), INK)
                cv.fill(('line', [p, q], 3.2), C('#8a8478'))
                cv.fill(('line', [(p[0], p[1] - 1), (q[0], q[1] - 1)], 0.8), C('#3a4a52'))
        sc.add(999, bridge)
    elif k == 1:
        # tower crane toppled across the canyon
        def crane():
            base = (CX - 20, CY + 4)
            top = (CX + 70, CY - 60)
            cv.fill(('line', [base, top], 4.2), INK)
            for t in np.linspace(0, 1, 14):
                p = (base[0] + (top[0] - base[0]) * t, base[1] + (top[1] - base[1]) * t)
                cv.fill(('line', [(p[0] - 2, p[1] - 2), (p[0] + 2, p[1] + 2)], 0.6), INK)
            cv.fill(('line', [base, top], 2.2), C('#d8a020'))
        sc.add(CY + 4, crane)
    else:
        # gutted parking structure: open decks, a car hanging off the edge
        def decks():
            x, y = CX + 6, CY + 6
            for lv in range(4):
                z = lv * 9
                slab = [P(x, y, -22, -12, z), P(x, y, 22, -12, z), P(x, y, 22, 12, z), P(x, y, -22, 12, z)]
                cv.fill(('poly', slab), INK, grow=0.9)
                cv.fill(('poly', slab), C('#9a968e'))
                edge = [P(x, y, -22, -12, z), P(x, y, 22, -12, z), P(x, y, 22, -12, z + 1.5), P(x, y, -22, -12, z + 1.5)]
                cv.fill(('poly', edge), C('#6a6660'))
            for px in (-20, 0, 20):
                cv.fill(('line', [P(x, y, px, -12, 0), P(x, y, px, -12, 36)], 1.6), INK)
        sc.add(CY + 18, decks)
        billboard(sc, CX + 70, CY + 60, w=36, h=16, lift=14, text=('EVIL CORP', "TOMORROW, TODAY"), torn=0.8, rng=rng)
    for (x, y) in [(CX - 10, CY + 14), (CX + 20, CY + 4), (CX - 40, CY + 22)]:
        car(sc, x, y, rng.uniform(-0.4, 0.4), color=CAR_COLS[rng.integers(len(CAR_COLS))], rust=0.9, s=1.4)
    rubble(sc, CX - 20, CY - 4, 10, rng)
    rim(cv)
    return finish(sc, shadows=0.5)


def _blown_top(sc, X, Y, w, h, rng):
    """Upper floors blown out: a jagged dark bite from the top, rebar."""
    cv = sc.cv

    def draw():
        top = Y - h * FZ
        pts = [(X - w * 0.5, top - 2)]
        for i in range(7):
            pts.append((X - w * 0.5 + w * (i + 1) / 8, top + rng.uniform(2, h * FZ * 0.22)))
        pts.append((X + w * 0.55, top - 2))
        cv.fill(('poly', pts), C('#140e12'), 0.9)
        for i in range(5):
            x = X - w * 0.4 + rng.uniform(0, w * 0.8)
            cv.fill(('line', [(x, top + 2), (x + rng.uniform(-2, 2), top - rng.uniform(3, 7))], 0.5), C('#6a3a2a'))
    sc.add(Y + 0.05, draw)


# ── 5 Flooded District ────────────────────────────────────────────────
def flooded(v, seed):
    cv, rng, nz = new(seed)
    water_ground(cv, nz, rng)
    sc = Scene(cv, nz, rng)
    spots = []
    if v == 0:
        spots = [(CX - 54, CY - 30), (CX - 10, CY - 46), (CX + 38, CY - 34), (CX + 66, CY + 4),
                 (CX - 44, CY + 20), (CX + 12, CY + 30), (CX - 82, CY - 4), (CX + 50, CY + 48), (CX - 20, CY + 60)]
        for (x, y) in spots:
            if inside(x, y, 18):
                drowned_house(sc, x, y, rng.uniform(-0.5, 0.5), HOUSE_COLS[rng.integers(len(HOUSE_COLS))],
                              ROOF_COLS[rng.integers(len(ROOF_COLS))], rng)
        tops = [pole(sc, CX + 30, CY + 6, h=26, lean=-0.18), pole(sc, CX - 30, CY - 4, h=26, lean=0.1),
                pole(sc, CX - 86, CY + 20, h=26, lean=0.05)]
        sc.add(999, lambda: [wire(cv, (a[0], a[1] + 2.5), (b[0], b[1] + 2.5), sag=7) for a, b in zip(tops[:-1], tops[1:])])
    elif v == 1:
        waterline(sc, CX, CY + 8, 40, 10, rng=rng)
        bus(sc, CX, CY + 8, 0.35, L=78, sunk=0.5, rng=rng)
        tops = [pole(sc, CX - 64, CY - 14, h=30, lean=0.15), pole(sc, CX + 64, CY - 30, h=30, lean=-0.05)]
        sc.add(999, lambda: wire(cv, (tops[0][0], tops[0][1] + 2.5), (tops[1][0], tops[1][1] + 2.5), sag=10))
        spots = [(CX, CY + 8)]
    elif v == 2:
        waterline(sc, CX - 6, CY - 18, 80, 14, rng=rng)
        block(sc, CX - 6, CY - 18, 140, 24, 5, -0.05, wall='#c8bca0', roof='#5a5654', rows=1, rng=rng, ruin=0.4,
              clutter=True, win_col=C('#1a2a2a'))
        tall_sign(sc, CX + 64, CY + 36, 44, ('MALL',), rng, lean=0.1)
        waterline(sc, CX + 64, CY + 36, 8, 3, rng=rng)
        spots = [(CX, CY - 18), (CX + 64, CY + 36)]
    elif v == 3:
        waterline(sc, CX - 10, CY, 60, 16, rng=rng)
        factory(sc, CX - 10, CY, 90, 46, 8, 0.1, saw=6, stacks=2, ruined=0.5, rng=rng, logo=True)
        water_tower(sc, CX + 70, CY - 34, s=1.3)
        waterline(sc, CX + 70, CY - 34, 10, 4, rng=rng)
        spots = [(CX, CY), (CX + 70, CY - 34)]
    elif v == 4:
        # slapstick: a whole house afloat on its side, a man fishing off the
        # gutter, a rowboat tied to the chimney
        waterline(sc, CX - 10, CY + 4, 34, 10, rng=rng)
        _floating_house(sc, CX - 10, CY + 4, rng)
        figure(sc, CX - 18, CY - 26, s=2.2, body='#6a5a3a', head='#d8b890', hat='#8a6a3a')
        def rod():
            cv.fill(('line', [(CX - 16, CY - 34), (CX + 14, CY - 48)], 0.6), INK)
            cv.fill(('line', [(CX + 14, CY - 48), (CX + 18, CY - 20)], 0.3), INK, 0.8)
        sc.add(CY + 10, rod)
        _rowboat(sc, CX + 50, CY + 30, 0.4, rng)
        spots = [(CX - 10, CY + 4), (CX + 50, CY + 30)]
    elif v == 5:
        waterline(sc, CX, CY + 20, 50, 10, rng=rng)
        ferris_wheel(sc, CX, CY + 20, 52, rng, sunk=0.45, lean=-0.06)
        for (x, y) in [(CX - 70, CY + 40), (CX + 70, CY - 20)]:
            _tent_top(sc, x, y, rng)
        spots = [(CX, CY + 20), (CX - 70, CY + 40), (CX + 70, CY - 20)]
    for (x, y) in scatter(rng, 5 if v == 0 else 3, 12, [(a, b, 24) for a, b in spots], 20):
        drowned_tree(sc, x, y, rng.uniform(5, 8), nz)
    for i in range(10):
        x, y = random_in_hex(rng, 10)
        cv.fill(('poly', blob_pts(x, y, rng.uniform(1.5, 3.5), rng.uniform(0.8, 1.4), nz, 0.4, phase=x)), C('#5a4a3a'), 0.8)
    rim(cv)
    return finish(sc, shadows=0.25)


def _floating_house(sc, X, Y, rng):
    cv = sc.cv

    def draw():
        # a house rolled onto its back: roof down in the water, floor up
        body = [(X - 26, Y), (X + 24, Y - 4), (X + 22, Y - 20), (X - 28, Y - 16)]
        cv.fill(('poly', body), INK, grow=1.2)
        cv.fill(('poly', body), C('#c89a8a'))
        cv.fill(('poly', [body[3], body[2], (body[2][0] - 2, body[2][1] - 6), (body[3][0] + 2, body[3][1] - 6)]), C('#e0c0b0'))
        for k in range(3):
            u = 0.2 + k * 0.28
            p = quad_pt(body, u, 0.5)
            cv.fill(('poly', [(p[0] - 3, p[1] - 2), (p[0] + 3, p[1] - 2.4), (p[0] + 3, p[1] + 3), (p[0] - 3, p[1] + 3.4)]), C('#1e2a2c'))
        roof = [(X - 26, Y), (X + 24, Y - 4), (X + 20, Y + 8), (X - 22, Y + 12)]
        cv.fill(('poly', roof), C('#5a3a34'), 0.8)
        chim = [(X + 12, Y - 20), (X + 16, Y - 21), (X + 18, Y - 12), (X + 14, Y - 11)]
        cv.fill(('poly', chim), INK, grow=0.6)
        cv.fill(('poly', chim), C('#8a4a38'))
    sc.add(Y, draw)


def _rowboat(sc, X, Y, yaw, rng):
    cv = sc.cv

    def draw():
        hull_ = [P(X, Y, *rot(x, y, yaw), z) for x, y, z in [(-10, 0, 1), (-6, -3.5, 0.5), (8, -3, 0.5), (12, 0, 1.2), (8, 3, 0.5), (-6, 3.5, 0.5)]]
        cv.fill(('poly', hull_), INK, grow=0.8)
        cv.fill(('poly', hull_), C('#8a5a3a'))
        inner = [P(X, Y, *rot(x, y, yaw), z) for x, y, z in [(-8, 0, 1.4), (-5, -2.4, 1), (7, -2, 1), (10, 0, 1.6), (7, 2, 1), (-5, 2.4, 1)]]
        cv.fill(('poly', inner), C('#4a2e20'))
        cv.fill(('line', [P(X, Y, *rot(-2, -3.4, yaw), 1), P(X, Y, *rot(-2, 3.4, yaw), 1)], 0.8), C('#a07a52'))
        cv.fill(('line', ellipse_pts(X, Y + 1, 14, 4, 16), 0.6), FLOOD[2], 0.55)
    sc.add(Y, draw)


def _tent_top(sc, X, Y, rng):
    """Striped fairground tent, only its peak above the flood."""
    cv = sc.cv
    waterline(sc, X, Y, 14, 4, rng=rng)

    def draw():
        apex = (X, Y - 16)
        for k in range(6):
            a0 = math.pi + k * math.pi / 6
            a1 = a0 + math.pi / 6
            p0 = (X + math.cos(a0) * 14, Y + math.sin(a0) * 4 * -1 * 0)
            p1 = (X + math.cos(a1) * 14, Y)
            seg = [apex, (X - 14 + k * 28 / 6, Y), (X - 14 + (k + 1) * 28 / 6, Y)]
            cv.fill(('poly', seg), INK, grow=0.6)
            cv.fill(('poly', seg), C('#c8322a') if k % 2 else C('#e8dcc0'))
        cv.fill(('line', [apex, (apex[0], apex[1] - 5)], 0.8), INK)
        cv.fill(('poly', [(apex[0], apex[1] - 5), (apex[0] + 4, apex[1] - 4), (apex[0], apex[1] - 3)]), C('#c8322a'))
    sc.add(Y, draw)


# ── 9 Settlement ──────────────────────────────────────────────────────
def settle_base(cv, nz, rng):
    flat_ground(cv, nz, '#7a5e3a', var=0.08, tint2='#5e5a34', tint_amt=0.3, gradient=0.14)
    stroke_texture(cv, rng, nz, [('#9a7a4c', 3), ('#4e3c26', 2), ('#b09060', 1)], n=1200, length=(1.5, 4), alpha=0.5, spread=1.2)
    yard = blob_pts(CX, CY + 16, 86, 50, nz, 0.12, n=40, phase=2)
    cv.ground_clip = True
    cv.fill(('poly', yard), C('#8e6e44'), 0.5)
    for i in range(4):
        a = rng.uniform(0, math.tau)
        cv.fill(('line', smooth_path([(CX, CY + 30), (CX + math.cos(a) * 50, CY + 30 + math.sin(a) * 30),
                                      (CX + math.cos(a) * 100, CY + 30 + math.sin(a) * 60)]), 3), C('#a4845a'), 0.5)


WALL = [(CX - 96, CY + 20), (CX - 66, CY + 60), (CX - 10, CY + 80), (CX + 56, CY + 66), (CX + 98, CY + 22)]


def settlement(v, seed):
    cv, rng, nz = new(seed)
    if v == 5:
        water_ground(cv, nz, rng)
    else:
        settle_base(cv, nz, rng)
        swell(cv, nz, amp=3.0)
    sc = Scene(cv, nz, rng)
    TENTS = ['#8a6a44', '#6a5a7a', '#7a4a3a', '#5a6a4a']
    if v == 0:
        plane(sc, CX - 6, CY - 6, 190, -0.28, kind='airliner', color='#c0c4c4', broken=True, overgrown=0.3,
              rng=rng, logo=True, lost_wing=True, stripe='#b8231c')
        palisade(sc, WALL, h=11, rng=rng, gaps=0.06)
        for (x, y) in [(CX - 46, CY + 38), (CX + 34, CY + 44), (CX + 62, CY + 28)]:
            tent(sc, x, y, 1.7, rng.uniform(-0.4, 0.4), color=TENTS[rng.integers(4)], rng=rng)
        cookfire(sc, CX - 4, CY + 56)
        lamp_string(sc, [(CX - 70, CY + 6), (CX - 30, CY + 20), (CX + 10, CY + 26), (CX + 50, CY + 18)])
    elif v == 1:
        for k, a in enumerate(np.linspace(-2.8, 0.2, 5)):
            x = CX + math.cos(a) * 58
            y = CY + 10 + math.sin(a) * 40
            trailer(sc, x, y, a + math.pi / 2, L=40, rng=rng, lit=0.7)
        palisade(sc, WALL, h=9, rng=rng, gaps=0.1)
        cookfire(sc, CX, CY + 22)
        lamp_string(sc, [(CX - 50, CY + 40), (CX - 10, CY + 50), (CX + 40, CY + 44)])
        figure(sc, CX + 12, CY + 30, s=1.9, body='#7a5a3a')
    elif v == 2:
        for (x, y, r) in [(CX - 40, CY - 14, 22), (CX + 34, CY - 20, 26), (CX - 6, CY + 30, 20), (CX + 60, CY + 30, 16)]:
            dome(sc, x, y, r, color=['#9a8a6a', '#8a8a7a', '#a08a60'][rng.integers(3)], rng=rng, lit=1)
        palisade(sc, WALL, h=9, rng=rng, gaps=0.12)
        cookfire(sc, CX + 10, CY + 4)
    elif v == 3:
        cv.ground_clip = True
        road(cv, [(CX - R - 4, CY + 40), (CX, CY + 28), (CX + R + 4, CY + 38)], 14, ASPHALT, None, dash=C('#c8b060'), rng=rng, noise=nz)
        cv.ground_clip = False
        gas_station(sc, CX - 6, CY - 10, 0.0, rng, lit=1.0, ruin=0.1)
        palisade(sc, [(CX - 96, CY + 10), (CX - 80, CY + 60)], h=10, rng=rng)
        palisade(sc, [(CX + 80, CY + 56), (CX + 98, CY + 10)], h=10, rng=rng)
        tent(sc, CX - 60, CY + 20, 1.6, 0.2, color=TENTS[1], rng=rng)
        figure(sc, CX + 30, CY + 16, s=1.9, body='#8a4a3a')
    elif v == 4:
        bus(sc, CX - 20, CY - 6, 0.25, L=66, color='#b8322a', rng=rng, school=False)
        bus(sc, CX + 30, CY + 26, -0.5, L=58, color='#d9a02a', rng=rng)
        palisade(sc, WALL, h=11, rng=rng, gaps=0.05)
        cookfire(sc, CX - 30, CY + 44)
        lamp_string(sc, [(CX - 70, CY + 20), (CX - 20, CY + 30), (CX + 20, CY + 50)])
    elif v == 5:
        # stilt village over the flood: huts on posts, walkways between
        huts = [(CX - 50, CY - 20), (CX + 10, CY - 34), (CX + 60, CY - 4), (CX - 20, CY + 30), (CX + 40, CY + 44)]
        for (x, y) in huts:
            waterline(sc, x, y + 8, 14, 4, rng=rng)
            _stilt_hut(sc, x, y, rng)
        def walks():
            for a, b in [(0, 1), (1, 2), (0, 3), (3, 4), (2, 4)]:
                (x0, y0), (x1, y1) = huts[a], huts[b]
                cv.fill(('line', [(x0, y0 - 10), (x1, y1 - 10)], 2.6), INK)
                cv.fill(('line', [(x0, y0 - 10), (x1, y1 - 10)], 1.4), C('#8a6a48'))
        sc.add(CY - 34, walks)
    elif v == 6:
        cv.ground_clip = True
        parking(cv, [(CX - 90, CY + 20), (CX + 90, CY + 10), (CX + 92, CY + 60), (CX - 86, CY + 70)], rng, lines=False)
        cv.ground_clip = False
        block(sc, CX - 8, CY - 20, 130, 22, 11, 0.0, wall='#d8c8a8', roof='#6a4a3a', rows=1, cols=7, rng=rng, lit=0.7,
              awnings=None)
        googie_sign(sc, CX + 72, CY + 34, 62, 'MOTEL', rng, lit=1.0)
        lamp_string(sc, [(CX - 70, CY + 30), (CX - 20, CY + 40), (CX + 30, CY + 36)])
        cookfire(sc, CX - 30, CY + 52)
        figure(sc, CX + 10, CY + 46, s=1.9, body='#5a6a8a')
    elif v == 7:
        palisade(sc, WALL, h=11, rng=rng, gaps=0.04)
        palisade(sc, [(CX - 96, CY + 18), (CX - 70, CY - 40), (CX, CY - 60), (CX + 72, CY - 44), (CX + 98, CY + 20)], h=11, rng=rng, gaps=0.04)
        for i in range(3):
            for j in range(2):
                x = CX - 50 + i * 44 + j * 12
                y = CY - 16 + j * 34
                _stall(sc, x, y, rng)
        for k in range(5):
            x, y = random_in_hex(rng, 30, band=(CY - 10, CY + 50))
            figure(sc, x, y, s=1.8, body=['#8a4a3a', '#5a6a8a', '#7a6a3a', '#6a3a5a'][k % 4])
        cookfire(sc, CX, CY + 10)
    rim(cv)
    return finish(sc)


def _stilt_hut(sc, X, Y, rng):
    cv = sc.cv
    from tg_props import house as _house
    for (dx, dy) in [(-7, 4), (7, 4), (-7, -3), (7, -3)]:
        pole(sc, X + dx, Y + dy + 8, h=10, cross=False, color='#5a4230')
    _house(sc, X, Y - 1, 18, 11, 6, rng.uniform(-0.3, 0.3), wall='#8a6a48', roof='#5a4a3a', rng=rng, lit=0.8)


def _stall(sc, X, Y, rng):
    cv = sc.cv
    cols = ['#b8423a', '#3a6a7a', '#c8a040', '#6a8a4a']
    col = cols[rng.integers(len(cols))]

    def draw():
        for dx in (-7, 7):
            cv.fill(('line', [(X + dx, Y), (X + dx, Y - 9)], 1.2), INK)
        aw = [(X - 9, Y - 9), (X + 9, Y - 10), (X + 10, Y - 6), (X - 10, Y - 5)]
        cv.fill(('poly', aw), INK, grow=0.5)
        cv.fill(('poly', aw), C(col))
        cv.fill(('line', [(X - 9, Y - 7), (X + 9, Y - 8)], 0.5), CREAM, 0.7)
        table = [(X - 7, Y - 3), (X + 7, Y - 3.5), (X + 7, Y - 1), (X - 7, Y - 0.5)]
        cv.fill(('poly', table), INK, grow=0.4)
        cv.fill(('poly', table), C('#8a6a48'))
        for k in range(4):
            cv.fill(('ellipse', X - 5 + k * 3.3, Y - 4, 1.1, 0.9), C(['#c8b060', '#8a3a2a', '#5a7a3a', '#d8d0c0'][k]))
    sc.add(Y, draw)
