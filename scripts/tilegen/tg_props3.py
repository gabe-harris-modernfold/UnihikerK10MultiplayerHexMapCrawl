"""Structures: the built world, 1940-2000, after the bombs and the Change.
Strip malls, gas stations, towers, churches, office parks, fairgrounds,
radio masts, lookout towers, blast doors. Same projection and light as
tg_props: (X, Y) is where it stands, faces cel-shaded, one ink silhouette."""
import math
import numpy as np
from tg_core import C, mix, shade, blob_pts, ellipse_pts
from tg_props import (P, rot, rot3, FY, FZ, INK, INK_W, LIGHT, SHADOW_DX, SHADOW_DY, Mat, make_ramp, box_geom,
                      draw_faces, visible, light_of, shadow_of, hull, foliage, quad_pt, quad_rect, evil_logo,
                      stroke_text, EVIL_RED, CREAM, GREEN_SICK, cylinder, pole, wire, hatch_poly, lum, _ruin)


def RPf(X, Y, yaw):
    return lambda x, y, z: P(X, Y, *rot(x, y, yaw), z)


# ── generic flat-roofed block ──────────────────────────────────────────
def block(sc, X, Y, w, d, h, yaw, wall='#8a8478', roof='#4a4644', rows=None, cols=None, lit=0.0,
          broken=0.4, ruin=0.0, sign=None, awnings=None, rng=None, glass=False, clutter=True,
          collapse=0.0, ink_w=INK_W, win_col=None, draw_now=False):
    """Flat-roofed building. rows/cols: window grid per wall (auto if None).
    sign=(text, colour, bg) on the front fascia. awnings=[colours] one per bay
    along the front. collapse 0..1 knocks the top of the east end down."""
    cv, rng = sc.cv, rng or sc.rng
    wm, rm = Mat(wall, 0.9), Mat(roof, 0.9)
    faces, top, base, rp = box_geom(X, Y, w, d, h, yaw)
    sc.shadow(shadow_of(base, h))
    RP = RPf(X, Y, yaw)

    def draw():
        items = [(q, wm.pick(n)) for q, n, i in faces]
        roofp = top
        if collapse > 0:
            # east end slumped: lower two top corners
            cut = h * collapse
            t2 = [RP(-w / 2, -d / 2, h), RP(w / 2, -d / 2, h - cut), RP(w / 2, d / 2, h - cut * 0.8), RP(-w / 2, d / 2, h)]
            new_items = []
            for q, col in items:
                q2 = [q[0], q[1], (q[2][0], q[2][1] + cut * FZ * (0.9 if abs(q[2][0] - t2[1][0]) < 1 or abs(q[2][0] - t2[2][0]) < 1 else 0)),
                      (q[3][0], q[3][1] + cut * FZ * (0.9 if abs(q[3][0] - t2[1][0]) < 1 or abs(q[3][0] - t2[2][0]) < 1 else 0))]
                new_items.append((q2, col))
            items = new_items
            roofp = t2
        items.append((roofp, rm.r[1]))
        draw_faces(cv, items, ink_w=ink_w)
        # parapet line + roof clutter
        inset = [quad_pt(roofp, 0.04, 0.06), quad_pt(roofp, 0.96, 0.06), quad_pt(roofp, 0.96, 0.94), quad_pt(roofp, 0.04, 0.94)]
        cv.fill(('line', inset + [inset[0]], 0.45), INK, 0.45)
        if clutter:
            for k in range(int(1 + w * d / 500)):
                u, v = rng.uniform(0.2, 0.8), rng.uniform(0.25, 0.75)
                p = quad_pt(roofp, u, v)
                ac = [(p[0] - 1.6, p[1]), (p[0] + 1.6, p[1]), (p[0] + 1.6, p[1] - 1.8), (p[0] - 1.6, p[1] - 1.8)]
                cv.fill(('poly', ac), INK, grow=0.4)
                cv.fill(('poly', ac), shade(rm.r[0], 1.05))
        # windows
        for q, n, i in faces:
            L = w if i % 2 == 0 else d
            nc = cols or max(1, int(L / 5.5))
            nr = rows or max(1, int(h / 5.5))
            for r_ in range(nr):
                for c_ in range(nc):
                    u0 = (c_ + 0.22) / nc
                    u1 = (c_ + 0.78) / nc
                    v0 = (r_ + 0.3) / nr * 0.92
                    v1 = (r_ + 0.75) / nr * 0.92
                    if awnings and i == 0 and r_ == 0:
                        continue
                    win = quad_rect(q, u0, v0, u1, v1)
                    if lit > 0 and rng.random() < lit:
                        cv.fill(('poly', win), C('#ffc15a'))
                        cv.fill(('poly', win), C('#ff9a30'), 0.3, grow=1.2, mode='glow')
                    elif glass:
                        cv.fill(('poly', win), C('#3a5a66') if rng.random() > broken else C('#141418'))
                        cv.fill(('line', [win[3], win[1]], 0.3), C('#9ac0c8'), 0.5)
                    else:
                        cv.fill(('poly', win), win_col or (C('#1a1618') if rng.random() < 0.5 + broken * 0.5 else C('#4a5a60')))
        if awnings:
            q = faces[0][0] if faces and faces[0][2] == 0 else None
            if q:
                nb = len(awnings)
                for k, col in enumerate(awnings):
                    u0, u1 = k / nb + 0.02, (k + 1) / nb - 0.02
                    aw = [quad_pt(q, u0, 0.42), quad_pt(q, u1, 0.42), quad_pt(q, u1 + 0.01, 0.3), quad_pt(q, u0 - 0.01, 0.3)]
                    aw = [aw[0], aw[1], (aw[2][0], aw[2][1] + 2.2), (aw[3][0], aw[3][1] + 2.2)]
                    cv.fill(('poly', aw), INK, grow=0.5)
                    cv.fill(('poly', aw), C(col))
                    for s in range(3):
                        uu = u0 + (u1 - u0) * (s + 0.5) / 3
                        cv.fill(('line', [quad_pt(q, uu, 0.42), (quad_pt(q, uu, 0.3)[0], quad_pt(q, uu, 0.3)[1] + 2.2)], 0.5), CREAM, 0.6)
                    door = quad_rect(q, u0 + (u1 - u0) * 0.15, 0.0, u1 - (u1 - u0) * 0.15, 0.28)
                    cv.fill(('poly', door), C('#141216') if rng.random() < 0.7 else C('#3a4a50'))
        if sign and faces:
            text, col, bg = sign
            q = faces[0][0]
            band = quad_rect(q, 0.08, 0.8, 0.92, 0.97)
            if bg:
                cv.fill(('poly', band), INK, grow=0.4)
                cv.fill(('poly', band), C(bg))
            c = quad_pt(q, 0.5, 0.885)
            stroke_text(cv, text, c[0], c[1], max(2.0, h * 0.1), C(col), width=max(0.5, h * 0.022), ink=False)
        if ruin > 0:
            _ruin(cv, sc, roofp, faces, ruin, rng)
    if draw_now:
        draw()
    else:
        sc.add(Y, draw)


def rubble(sc, X, Y, r, rng, cols=('#8a8478', '#6a5e52', '#9a6a52', '#5a5652')):
    """Heap of broken concrete and brick."""
    cv = sc.cv

    def draw():
        chunks = []
        for i in range(int(6 + r)):
            a = rng.uniform(0, math.tau)
            rr = r * math.sqrt(rng.uniform(0, 1))
            x = X + math.cos(a) * rr
            y = Y + math.sin(a) * rr * 0.5
            s = rng.uniform(0.8, 2.2)
            pts = [(x + math.cos(t) * s * rng.uniform(0.6, 1.3), y + math.sin(t) * s * 0.8 * rng.uniform(0.6, 1.3))
                   for t in np.linspace(0, math.tau, 5, endpoint=False)]
            chunks.append(pts)
        chunks.sort(key=lambda p: sum(q[1] for q in p) / len(p))
        cv.fill([('poly', p) for p in chunks], INK, grow=0.5)
        for p in chunks:
            col = C(cols[rng.integers(len(cols))])
            cv.fill(('poly', p), col)
            cv.fill(('poly', [p[0], p[1], p[2]]), shade(col, 1.12), 0.8)
        for k in range(3):
            x = X + rng.uniform(-r, r)
            y = Y + rng.uniform(-r * 0.3, r * 0.3)
            cv.fill(('line', [(x, y), (x + rng.uniform(-4, 4), y - rng.uniform(3, 7))], 0.5), C('#5a3a2a'))
    sc.add(Y, draw)


def parking(cv, poly, rng, lines=True, cracks=True):
    """Asphalt lot with faded bay lines."""
    cv.fill(('poly', poly), C('#3a383a'))
    cv.fill(('poly', poly), C('#4a4a46'), 0.3, grow=-0.5)
    if lines:
        a, b, c_, d = poly
        for k in range(1, 9):
            u = k / 9
            p0 = (a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u)
            p1 = (p0[0] + (d[0] - a[0]) * 0.35, p0[1] + (d[1] - a[1]) * 0.35)
            cv.fill(('line', [p0, p1], 0.4), C('#c8c0a0'), 0.45)


def gas_station(sc, X, Y, yaw, rng, brand='EVIL', lit=0.0, ruin=0.4):
    cv = sc.cv
    RP = RPf(X, Y, yaw)
    # the shop behind, the canopy in front
    block(sc, X + rot(0, 16, yaw)[0], Y - rot(0, 16, yaw)[1] * FY, 30, 14, 9, yaw, wall='#d8d0b8', roof='#5a5654',
          sign=('FOOD', '#b8231c', '#efe3c6'), rng=rng, ruin=ruin * 0.5, lit=lit, awnings=None)
    cw, cd, ch = 40, 18, 11
    posts = [(-cw * 0.38, -cd * 0.3), (cw * 0.38, -cd * 0.3), (-cw * 0.38, cd * 0.3), (cw * 0.38, cd * 0.3)]
    base = [RP(-cw / 2, -cd / 2, 0), RP(cw / 2, -cd / 2, 0), RP(cw / 2, cd / 2, 0), RP(-cw / 2, cd / 2, 0)]
    sc.shadow(shadow_of(base, 6))

    def draw():
        for (px, py) in posts:
            a, b = RP(px, py, 0), RP(px, py, ch)
            cv.fill(('line', [a, b], 1.9), INK)
            cv.fill(('line', [a, b], 1.0), C('#c8c0b0'))
        # pumps
        for k in (-1, 1):
            pf, pt, pb, _ = box_geom(*RP(k * cw * 0.18, 0, 0), 3.0, 2.0, 5.0, yaw)
            draw_faces(cv, [(q, Mat('#b8231c').pick(n)) for q, n, i in pf] + [(pt, C('#e8e0d0'))], ink_w=0.6, edge_w=0.3)
        slab_top = [RP(-cw / 2, -cd / 2, ch + 2), RP(cw / 2, -cd / 2, ch + 2), RP(cw / 2, cd / 2, ch + 2), RP(-cw / 2, cd / 2, ch + 2)]
        slab_front = [RP(-cw / 2, -cd / 2, ch), RP(cw / 2, -cd / 2, ch), RP(cw / 2, -cd / 2, ch + 2), RP(-cw / 2, -cd / 2, ch + 2)]
        cv.fill([('poly', slab_top), ('poly', slab_front)], INK, grow=INK_W)
        cv.fill(('poly', slab_top), C('#dcd6c8'))
        cv.fill(('poly', slab_front), EVIL_RED)
        if ruin > 0:
            for k in range(3):
                u = rng.uniform(0.1, 0.9)
                v = rng.uniform(0.2, 0.8)
                p = quad_pt(slab_top, u, v)
                cv.fill(('poly', blob_pts(p[0], p[1], rng.uniform(2, 5), rng.uniform(1.5, 3), sc.noise, 0.5, phase=u * 31)), C('#1c1418'))
        c = quad_pt(slab_front, 0.5, 0.5)
        stroke_text(cv, brand, c[0], c[1], 1.5, CREAM, width=0.45, ink=False)
        if lit:
            cv.fill(('poly', slab_front), C('#ff9a30'), 0.25, grow=2, mode='glow')
    sc.add(Y, draw)
    # the tall price sign
    sx, sy = X + rot(cw * 0.62, -cd * 0.2, yaw)[0], Y - rot(cw * 0.62, -cd * 0.2, yaw)[1] * FY
    tall_sign(sc, sx, sy, 30, ('GAS', '19c'), rng, lit=lit)


def tall_sign(sc, X, Y, h, lines, rng, lit=0.0, color='#e8dcc0', text_col='#b8231c', lean=0.0, logo=True):
    cv = sc.cv
    sc.shadow(('line', [(X, Y), (X + SHADOW_DX * h, Y + SHADOW_DY * h)], 1.4))

    def draw():
        top = (X + lean * h, Y - h * FZ)
        cv.fill(('line', [(X, Y), top], 2.2), INK)
        cv.fill(('line', [(X, Y), top], 1.1), C('#6a6460'))
        bw, bh = 13, 11
        box = [(top[0] - bw / 2, top[1] - bh), (top[0] + bw / 2, top[1] - bh), (top[0] + bw / 2, top[1] + 1), (top[0] - bw / 2, top[1] + 1)]
        cv.fill(('poly', box), INK, grow=1.0)
        cv.fill(('poly', box), C(color))
        if logo:
            evil_logo(cv, top[0], top[1] - bh * 0.62, 2.6)
            stroke_text(cv, lines[0], top[0], top[1] - bh * 0.18, 2.6, C(text_col), width=0.7, ink=False)
        else:
            stroke_text(cv, lines[0], top[0], top[1] - bh * 0.6, 3.2, C(text_col), width=0.8, ink=False)
            if len(lines) > 1:
                stroke_text(cv, lines[1], top[0], top[1] - bh * 0.2, 2.4, C('#2a2226'), width=0.6, ink=False)
        if lit:
            cv.fill(('poly', box), C('#ffb050'), 0.25, grow=3, mode='glow')
    sc.add(Y, draw)


def googie_sign(sc, X, Y, h, text, rng, lit=0.0, sunk=0.0, color='#2a8a8a'):
    """1950s motel sign: a tilted arrow board on a pole, a starburst on top,
    letters stacked down the arrow. `sunk` buries its lower part (dunes)."""
    cv = sc.cv
    ramp = make_ramp(color, 1.0)
    sc.shadow(('line', [(X, Y), (X + SHADOW_DX * h, Y + SHADOW_DY * h)], 3))

    def draw():
        base_y = Y
        top = (X, Y - h * FZ)
        cv.fill(('line', [(X, base_y), top], 2.4), INK)
        cv.fill(('line', [(X, base_y), top], 1.2), C('#8a8478'))
        # arrow board, pointing down-right
        ax, ay = X - 2, top[1] + h * FZ * 0.12
        board = [(ax - 5, ay), (ax + 8, ay + 2), (ax + 8, ay + h * FZ * 0.55), (ax + 14, ay + h * FZ * 0.55),
                 (ax + 4, ay + h * FZ * 0.75), (ax - 6, ay + h * FZ * 0.55), (ax - 1, ay + h * FZ * 0.55), (ax - 5, ay + 2)]
        cv.fill(('poly', board), INK, grow=1.1)
        cv.fill(('poly', board), ramp[1])
        cv.fill(('poly', [board[0], board[1], board[2], board[7]]), ramp[0], 0.35)
        n = len(text)
        for i, ch in enumerate(text):
            yy = ay + 3.5 + i * (h * FZ * 0.5 / max(1, n))
            stroke_text(cv, ch, ax + 1.5, yy, 2.8, C('#ff5a8a') if lit else CREAM, width=0.8, ink=False)
        if lit:
            cv.fill(('poly', board), C('#ff5a8a'), 0.18, grow=3, mode='glow')
        # starburst
        sx, sy = X + 1, top[1] - 1
        for k in range(8):
            a = k * math.pi / 4
            cv.fill(('line', [(sx, sy), (sx + math.cos(a) * 4.5, sy + math.sin(a) * 4.5)], 1.2), INK)
            cv.fill(('line', [(sx, sy), (sx + math.cos(a) * 4, sy + math.sin(a) * 4)], 0.5), C('#e8c040'))
    sc.add(Y, draw)


def church(sc, X, Y, yaw, rng, wall='#c8bca4', roof='#4a3a3a', ruin=0.5, steeple_h=34):
    from tg_props import house
    house(sc, X, Y, 34, 18, 12, yaw, wall=wall, roof=roof, rh=9, ruined=ruin, rng=rng, door=True)
    sx, sy = rot(-17 - 3, 0, yaw)
    tx, ty = X + sx, Y - sy * FY + 0.3
    faces, top, base, rp = box_geom(tx, ty, 8, 8, steeple_h, yaw)
    sc.shadow(shadow_of(base, steeple_h + 10))
    cv = sc.cv
    M = Mat(wall, 0.9)

    def draw():
        draw_faces(cv, [(q, M.pick(n)) for q, n, i in faces] + [(top, M.r[0])])
        apex = P(tx, ty, 0, 0, steeple_h + 14)
        for i in range(4):
            tri = [top[i], top[(i + 1) % 4], apex]
            cx_ = (tri[0][0] + tri[1][0]) / 2
            col = shade(C(roof), 1.1) if cx_ < apex[0] else shade(C(roof), 0.75)
            cv.fill(('poly', tri), INK, grow=0.9)
            cv.fill(('poly', tri), col)
        # belfry opening + a cross leaning off true
        for q, n, i in faces:
            cv.fill(('poly', quad_rect(q, 0.3, 0.72, 0.7, 0.9)), C('#141216'))
        cx_, cy_ = apex
        cv.fill(('line', [(cx_, cy_), (cx_ + 1.2, cy_ - 7)], 1.6), INK)
        cv.fill(('line', [(cx_ - 2.2, cy_ - 4.6), (cx_ + 3.0, cy_ - 5.2)], 1.6), INK)
        cv.fill(('line', [(cx_, cy_), (cx_ + 1.2, cy_ - 7)], 0.7), C('#c8b890'))
        cv.fill(('line', [(cx_ - 2.2, cy_ - 4.6), (cx_ + 3.0, cy_ - 5.2)], 0.7), C('#c8b890'))
    sc.add(ty, draw)


def container_stack(sc, X, Y, yaw, rng, n=4, levels=2):
    cols = ['#9a3a2a', '#2a5a7a', '#6a7a3a', '#b88a2a', '#5a4a6a', '#8a8a8a']
    for lv in range(levels):
        for k in range(n - lv):
            ox, oy = rot(-n * 3 + k * 6.4 + lv * 3, 0, yaw)
            _container(sc, X + ox, Y - oy * FY, 16, 6, 6, yaw + math.pi / 2, cols[rng.integers(len(cols))], z0=lv * 6)


def _container(sc, X, Y, w, d, h, yaw, col, z0=0):
    cv = sc.cv
    M = Mat(col, 1.0)
    faces, top, base, rp = box_geom(X, Y, w, d, h, yaw, z0=z0)
    if z0 == 0:
        sc.shadow(shadow_of(base, h))

    def draw():
        draw_faces(cv, [(q, M.pick(n)) for q, n, i in faces] + [(top, M.r[0])], ink_w=0.9, edge_w=0.35)
        for q, n, i in faces:
            for k in range(1, 8):
                u = k / 8
                cv.fill(('line', [quad_pt(q, u, 0.05), quad_pt(q, u, 0.95)], 0.3), INK, 0.3)
    sc.add(Y + z0 * 0.01, draw)


def pool(sc, cv, X, Y, w, d, yaw, rng, water='#5a8a3a', car_in=True):
    """Backyard swimming pool gone green, with the neighbour's car in it."""
    RP = RPf(X, Y, yaw)
    rim_ = [RP(-w / 2, -d / 2, 0), RP(w / 2, -d / 2, 0), RP(w / 2, d / 2, 0), RP(-w / 2, d / 2, 0)]
    inner = [RP(-w / 2 + 1.5, -d / 2 + 1.5, 0), RP(w / 2 - 1.5, -d / 2 + 1.5, 0), RP(w / 2 - 1.5, d / 2 - 1.5, 0), RP(-w / 2 + 1.5, d / 2 - 1.5, 0)]
    cv.fill(('poly', rim_), C('#c8c4b8'))
    cv.fill(('poly', rim_), INK, 0.7, grow=0.5, sub=('poly', rim_))
    cv.fill(('poly', inner), C('#1e2a1c'))
    # far wall of the pool visible (it's a hole)
    wall = [inner[3], inner[2], (inner[2][0], inner[2][1] + 3), (inner[3][0], inner[3][1] + 3)]
    cv.fill(('poly', wall), C('#8a9a98'), clip=('poly', inner))
    wat = [(inner[0][0], inner[0][1] + 0), (inner[1][0], inner[1][1]), (inner[2][0], inner[2][1] + 3), (inner[3][0], inner[3][1] + 3)]
    cv.fill(('poly', wat), C(water), clip=('poly', inner))
    for k in range(6):
        u, v = rng.uniform(0.1, 0.9), rng.uniform(0.3, 0.9)
        p = quad_pt(inner, u, v)
        cv.fill(('line', [(p[0] - 2, p[1]), (p[0] + 2, p[1])], 0.45), C('#9ac060'), 0.7, clip=('poly', inner))
    # diving board
    a, b = RP(-w / 2 - 2, 0, 0.6), RP(-w / 2 + 4, 0, 0.8)
    cv.fill(('line', [a, b], 1.6), INK)
    cv.fill(('line', [a, b], 0.8), C('#d8d0c0'))
    if car_in:
        from tg_props import car as _car
        # nose-down in the deep end: drawn as a tilted car
        _car(sc, X + 2, Y + 1, yaw + 0.4, color='#c8b060', fins=True, rust=0.6, s=1.4)


def ferris_wheel(sc, X, Y, r, rng, sunk=0.0, lean=0.0, color='#c8c0b0', gondola='#b8423a', lit=0.0):
    """Fairground wheel: rim, spokes, gondolas, A-frame legs. sunk buries
    the bottom (ash dunes, floodwater)."""
    cv = sc.cv
    hub = (X + lean * r, Y - (r + 4 - sunk * r) * FZ)
    sc.shadow(('poly', [(X - r * 0.6, Y), (X + r * 0.6, Y), (X + r * 0.6 + SHADOW_DX * r * 2, Y + SHADOW_DY * r * 2),
                        (X - r * 0.6 + SHADOW_DX * r * 2, Y + SHADOW_DY * r * 2)]))

    def draw():
        ground_y = Y
        clip = ('poly', [(X - r * 2, Y - r * 4), (X + r * 2, Y - r * 4), (X + r * 2, ground_y), (X - r * 2, ground_y)])
        for sg in (-1, 1):
            a, b = (X + sg * r * 0.55, ground_y), hub
            cv.fill(('line', [a, b], 2.0), INK, clip=clip)
            cv.fill(('line', [a, b], 0.9), C('#6a6a70'), clip=clip)
        rim_pts = ellipse_pts(hub[0], hub[1], r, r * 0.96, 40)
        cv.fill(('line', rim_pts + [rim_pts[0]], 2.0), INK, clip=clip)
        cv.fill(('line', rim_pts + [rim_pts[0]], 0.9), C(color), clip=clip)
        inner = ellipse_pts(hub[0], hub[1], r * 0.88, r * 0.85, 40)
        cv.fill(('line', inner + [inner[0]], 0.5), INK, 0.8, clip=clip)
        for k in range(12):
            a = k * math.tau / 12 + 0.1
            p = (hub[0] + math.cos(a) * r, hub[1] + math.sin(a) * r * 0.96)
            cv.fill(('line', [hub, p], 0.45), INK, 0.8, clip=clip)
        for k in range(12):
            if rng.random() < 0.15:
                continue
            a = k * math.tau / 12 + 0.1
            p = (hub[0] + math.cos(a) * r, hub[1] + math.sin(a) * r * 0.96)
            g = [(p[0] - 2, p[1] + 1), (p[0] + 2, p[1] + 1), (p[0] + 1.6, p[1] + 4), (p[0] - 1.6, p[1] + 4)]
            cv.fill(('poly', g), INK, grow=0.5, clip=clip)
            cv.fill(('poly', g), C(gondola), clip=clip)
            if lit and rng.random() < lit:
                cv.fill(('ellipse', p[0], p[1], 1.0, 1.0), C('#ffd27a'), clip=clip)
        cv.fill(('ellipse', hub[0], hub[1], 1.6, 1.6), INK)
    sc.add(Y, draw)


def radio_mast(sc, X, Y, h, rng, lean=0.12):
    """Guyed lattice mast at a drunk angle, a red lamp on top that still blinks."""
    cv = sc.cv
    sc.shadow(('line', [(X, Y), (X + SHADOW_DX * h, Y + SHADOW_DY * h)], 2))

    def draw():
        top = (X + lean * h, Y - h * FZ)
        # guy wires
        for (gx, gy) in [(-h * 0.45, h * 0.1), (h * 0.5, h * 0.06), (h * 0.05, -h * 0.08)]:
            t = (X + lean * h * 0.7, Y - h * FZ * 0.7)
            cv.fill(('line', [(X + gx, Y + gy * 0.3), t], 0.4), INK, 0.7)
        n = 10
        for s in (-1, 1):
            a = (X + s * 2.2, Y)
            b = (top[0] + s * 0.8, top[1])
            cv.fill(('line', [a, b], 1.2), INK)
        for i in range(n):
            t0, t1 = i / n, (i + 1) / n
            w0, w1 = 2.2 - 1.4 * t0, 2.2 - 1.4 * t1
            p0 = (X + lean * h * t0, Y - h * FZ * t0)
            p1 = (X + lean * h * t1, Y - h * FZ * t1)
            cv.fill(('line', [(p0[0] - w0, p0[1]), (p1[0] + w1, p1[1])], 0.5), INK, 0.9)
            cv.fill(('line', [(p0[0] + w0, p0[1]), (p1[0] - w1, p1[1])], 0.5), INK, 0.9)
        cv.fill(('ellipse', top[0], top[1] - 1, 1.2, 1.2), C('#ff3a2a'))
        cv.fill(('ellipse', top[0], top[1] - 1, 4, 4), C('#ff3a2a'), 0.3, mode='glow')
    sc.add(Y, draw)


def lookout_tower(sc, X, Y, h, rng, lit=False):
    """Fire lookout: four splayed legs, cross-bracing, a cab with a hat roof."""
    cv = sc.cv
    sc.shadow(('poly', [(X - 5, Y), (X + 5, Y), (X + 5 + SHADOW_DX * h, Y + SHADOW_DY * h), (X - 5 + SHADOW_DX * h, Y + SHADOW_DY * h)]))

    def draw():
        legs = [(-6, -3), (6, -3), (-5, 3), (5, 3)]
        tops = []
        for lx, ly in legs:
            b = P(X, Y, lx, ly, 0)
            t = P(X, Y, lx * 0.45, ly * 0.45, h)
            tops.append(t)
            cv.fill(('line', [b, t], 1.4), INK)
            cv.fill(('line', [b, t], 0.6), C('#6a5a48'))
        for f in (0.33, 0.66):
            a = P(X, Y, -6 + 6 * 0.55 * f, -3, h * f)
            b = P(X, Y, 6 - 6 * 0.55 * f, -3, h * f)
            cv.fill(('line', [a, b], 0.6), INK)
            cv.fill(('line', [P(X, Y, -6, -3, 0), P(X, Y, 6 * 0.72, -3, h * 0.5)], 0.45), INK, 0.8)
        cf, ct, cb, _ = box_geom(X, Y, 11, 9, 7, 0.0, z0=h)
        M = Mat('#8a7a5a', 0.9)
        items = [(q, M.pick(n)) for q, n, i in cf] + [(ct, M.r[0])]
        draw_faces(cv, items)
        for q, n, i in cf:
            cv.fill(('poly', quad_rect(q, 0.1, 0.45, 0.9, 0.85)), C('#ffc15a') if lit else C('#1e2226'))
        apex = P(X, Y, 0, 0, h + 12)
        roof = [(ct[0][0] - 2, ct[0][1] + 1), (ct[1][0] + 2, ct[1][1] + 1), apex]
        cv.fill(('poly', roof), INK, grow=0.9)
        cv.fill(('poly', roof), C('#5a3a30'))
        cv.fill(('poly', [roof[0], apex, ((roof[0][0] + roof[1][0]) / 2, roof[0][1])]), C('#7a4a38'))
    sc.add(Y, draw)


def dome_observatory(sc, X, Y, r, rng, broken=True, color='#d8d4cc'):
    cv = sc.cv
    ramp = make_ramp(color, 1.1)
    cylinder(sc, X, Y, r, 0, r * 0.7, '#8a8680', top_color=C('#6a6660'))

    def draw():
        cy = Y - r * 0.7 * FZ
        dome = ellipse_pts(X, cy, r * 1.02, r * 0.95, 28, math.pi, math.tau)
        cv.fill(('poly', dome), INK, grow=INK_W)
        cv.fill(('poly', dome), ramp[2])
        cv.fill(('poly', ellipse_pts(X - r * 0.15, cy - r * 0.05, r * 0.78, r * 0.75, 20, math.pi, math.tau)), ramp[1], clip=('poly', dome))
        cv.fill(('poly', ellipse_pts(X - r * 0.35, cy - r * 0.3, r * 0.34, r * 0.3, 14, math.pi, math.tau)), ramp[0], clip=('poly', dome))
        slit = [(X - r * 0.12, cy), (X + r * 0.12, cy), (X + r * 0.1, cy - r * 0.95), (X - r * 0.1, cy - r * 0.95)]
        cv.fill(('poly', slit), C('#141216'), clip=('poly', dome))
        if broken:
            hole = blob_pts(X + r * 0.4, cy - r * 0.45, r * 0.3, r * 0.25, sc.noise, 0.4, phase=X)
            cv.fill(('poly', hole), C('#141216'), clip=('poly', dome))
            for k in range(4):
                a = rng.uniform(0, math.tau)
                p = (X + r * 0.4 + math.cos(a) * r * 0.25, cy - r * 0.45 + math.sin(a) * r * 0.2)
                cv.fill(('line', [p, (p[0] + math.cos(a) * 3, p[1] + math.sin(a) * 3)], 0.5), INK, clip=('poly', dome))
    sc.add(Y + 0.01, draw)


def chairlift(sc, pts, rng, h=14):
    """Towers up a slope with the cable sagging between them and chairs still
    hanging off it. pts = [(x, y_screen_base), ...] bottom to top."""
    cv = sc.cv
    tops = []
    for (x, y) in pts:
        tops.append((x, y - h * FZ))
        sc.shadow(('line', [(x, y), (x + SHADOW_DX * h, y + SHADOW_DY * h)], 1.2))

    def draw():
        for (x, y), t in zip(pts, tops):
            cv.fill(('line', [(x, y), t], 2.0), INK)
            cv.fill(('line', [(x, y), t], 0.9), C('#7a7a80'))
            cv.fill(('line', [(t[0] - 4, t[1]), (t[0] + 4, t[1])], 1.6), INK)
        for a, b in zip(tops[:-1], tops[1:]):
            for side in (-3.5, 3.5):
                aa, bb = (a[0] + side, a[1]), (b[0] + side, b[1])
                wire(cv, aa, bb, sag=4, w=0.45, alpha=0.9)
                for t in (0.3, 0.6):
                    x = aa[0] + (bb[0] - aa[0]) * t
                    y = aa[1] + (bb[1] - aa[1]) * t + 4 * 4 * t * (1 - t)
                    if rng.random() < 0.8:
                        cv.fill(('line', [(x, y), (x, y + 3)], 0.4), INK)
                        seat = [(x - 1.6, y + 3), (x + 1.6, y + 3), (x + 1.6, y + 4.4), (x - 1.6, y + 4.4)]
                        cv.fill(('poly', seat), INK, grow=0.3)
                        cv.fill(('poly', seat), C('#c83a2a'))
    sc.add(max(p[1] for p in pts), draw)


def jet(sc, X, Y, L, yaw, rng, color='#8a9098', broken=True):
    """A delta-winged fighter, nose in the dirt."""
    cv = sc.cv
    ramp = make_ramp(color, 1.1)
    RP = RPf(X, Y, yaw)
    from tg_props2 import shade_tube
    body = [RP(-L / 2, 0, 3), RP(L * 0.3, 0, 2.2), RP(L / 2, 0, 1.0)]
    wing = [RP(-L * 0.15, 0, 1.6), RP(-L * 0.42, L * 0.36, 1.2), RP(-L * 0.42, -L * 0.36, 1.2)]
    sc.shadow(('poly', hull([(x + 4, y + 1.5) for x, y in wing] + wing)))

    def draw():
        cv.fill(('poly', wing), INK, grow=INK_W)
        cv.fill(('poly', wing), ramp[1])
        cv.fill(('poly', [wing[0], wing[1], ((wing[0][0] + wing[2][0]) / 2, (wing[0][1] + wing[2][1]) / 2)]), ramp[0], 0.6)
        shade_tube(cv, body, [L * 0.07, L * 0.06, L * 0.035], ramp)
        fin = [RP(-L * 0.46, 0, 3), RP(-L * 0.3, 0, 3), RP(-L * 0.44, 0, 3 + L * 0.2)]
        cv.fill(('poly', fin), INK, grow=INK_W)
        cv.fill(('poly', fin), ramp[1])
        c = RP(L * 0.28, 0, 3.4)
        cv.fill(('ellipse', c[0], c[1], L * 0.06, L * 0.03), C('#2a3a44'))
        evil_logo(cv, *RP(-L * 0.25, L * 0.14, 1.8), L * 0.035)
    sc.add(Y, draw)


def helicopter(sc, X, Y, L, yaw, rng, color='#8a4a26', nose_down=True):
    """Pre-war military chopper, nose-down, rusted orange, tail boom bent in
    one last argument with the ground (Jack's)."""
    cv = sc.cv
    ramp = make_ramp(color, 1.1)
    RP = RPf(X, Y, yaw)
    from tg_props2 import shade_tube
    r = L * 0.16
    sc.shadow(('ellipse', X + 6, Y + 2, L * 0.5, L * 0.18))

    def draw():
        # tail boom, bent
        tb = [RP(-L * 0.05, 0, r * 1.3), RP(-L * 0.45, 0, r * 1.6), RP(-L * 0.62, L * 0.08, r * 1.2)]
        shade_tube(cv, tb, [r * 0.35, r * 0.25, r * 0.2], ramp)
        # tail rotor, bent
        tr = tb[-1]
        for k in range(2):
            a = k * math.pi / 2 + 0.4
            cv.fill(('line', [(tr[0] - math.cos(a) * r * 0.7, tr[1] - math.sin(a) * r * 0.7),
                              (tr[0] + math.cos(a) * r * 0.7, tr[1] + math.sin(a) * r * 0.7)], 1.2), INK)
        # cabin: fat tube, nose down
        cab = [RP(-L * 0.1, 0, r * 1.3), RP(L * 0.12, 0, r * 0.9), RP(L * 0.28, 0, r * 0.45)]
        shade_tube(cv, cab, [r, r * 0.95, r * 0.6], ramp)
        # glass
        g = RP(L * 0.24, 0, r * 0.7)
        cv.fill(('ellipse', g[0], g[1], r * 0.5, r * 0.35), C('#22303a'))
        cv.fill(('ellipse', g[0] - r * 0.15, g[1] - r * 0.12, r * 0.18, r * 0.1), C('#8aa0a8'), 0.8)
        # door gap
        d = RP(0, -r * 0.6, r * 0.9)
        cv.fill(('poly', [(d[0] - r * 0.3, d[1] - r * 0.4), (d[0] + r * 0.3, d[1] - r * 0.4), (d[0] + r * 0.3, d[1] + r * 0.4), (d[0] - r * 0.3, d[1] + r * 0.4)]),
                C('#141216'))
        # skids
        for sg in (-1, 1):
            a, b = RP(-L * 0.2, sg * r * 0.8, 0.3), RP(L * 0.2, sg * r * 0.8, 0.0)
            cv.fill(('line', [a, b], 1.4), INK)
            cv.fill(('line', [a, b], 0.6), C('#5a5048'))
        # main rotor: one blade drooping into the dirt, one up
        hub = RP(0, 0, r * 2.2)
        cv.fill(('line', [RP(0, 0, r * 1.9), hub], 1.6), INK)
        for (dx, dy, dz) in [(L * 0.55, L * 0.1, -r * 1.6), (-L * 0.5, -L * 0.2, r * 0.3), (L * 0.1, -L * 0.55, -r * 0.6)]:
            tip = RP(dx, dy, r * 2.2 + dz)
            cv.fill(('line', [hub, tip], 2.0), INK)
            cv.fill(('line', [hub, tip], 0.9), C('#4a4440'))
        # rust
        for k in range(5):
            p = RP(rng.uniform(-L * 0.1, L * 0.2), 0, r * rng.uniform(0.6, 1.4))
            cv.fill(('ellipse', p[0], p[1], r * 0.2, r * 0.12), C('#5a2a14'), 0.6)
    sc.add(Y, draw)


def figure(sc, X, Y, s=1.0, body='#8a6a4a', head='#e0b890', pose=0, hat=None):
    """A tiny survivor. pose 0 standing, 1 arms up (waving / despair)."""
    cv = sc.cv
    sc.shadow(('ellipse', X + 1.5 * s, Y, 2 * s, 0.8 * s))

    def draw():
        legs = [('line', [(X - 0.6 * s, Y), (X - 0.4 * s, Y - 2.4 * s)], 0.9 * s), ('line', [(X + 0.6 * s, Y), (X + 0.4 * s, Y - 2.4 * s)], 0.9 * s)]
        torso = ('poly', [(X - 1.3 * s, Y - 2.2 * s), (X + 1.3 * s, Y - 2.2 * s), (X + 1.0 * s, Y - 5.0 * s), (X - 1.0 * s, Y - 5.0 * s)])
        hd = ('ellipse', X, Y - 6.1 * s, 1.1 * s, 1.1 * s)
        arms = []
        if pose == 1:
            arms = [('line', [(X - 1.0 * s, Y - 4.6 * s), (X - 2.2 * s, Y - 7.0 * s)], 0.7 * s), ('line', [(X + 1.0 * s, Y - 4.6 * s), (X + 2.2 * s, Y - 7.0 * s)], 0.7 * s)]
        else:
            arms = [('line', [(X - 1.1 * s, Y - 4.6 * s), (X - 1.6 * s, Y - 2.6 * s)], 0.7 * s), ('line', [(X + 1.1 * s, Y - 4.6 * s), (X + 1.6 * s, Y - 2.6 * s)], 0.7 * s)]
        shapes = legs + [torso, hd] + arms
        cv.fill(shapes, INK, grow=0.5)
        cv.fill(legs, C('#3a3030'))
        cv.fill([torso] + arms, C(body))
        cv.fill(hd, C(head))
        if hat:
            cv.fill(('ellipse', X, Y - 6.9 * s, 1.5 * s, 0.6 * s), C(hat))
    sc.add(Y, draw)


def bomb_casing(sc, X, Y, L, yaw, rng, color='#5a6048', text='EVIL CORP'):
    """A dud, half buried nose-first at the crater's edge, fins in the air."""
    cv = sc.cv
    ramp = make_ramp(color, 1.1)
    from tg_props2 import shade_tube
    RP = RPf(X, Y, yaw)

    def draw():
        axis = [RP(0, 0, 0), RP(-L * 0.3, 0, L * 0.35), RP(-L * 0.55, 0, L * 0.62)]
        rad = [L * 0.14, L * 0.15, L * 0.1]
        cv.fill(('ellipse', X, Y + 1, L * 0.2, L * 0.07), C('#1a1612'), 0.7)
        shade_tube(cv, axis, rad, ramp)
        t = axis[-1]
        for k in range(4):
            a = k * math.pi / 2 + 0.3
            fin = [(t[0], t[1]), (t[0] + math.cos(a) * L * 0.16, t[1] + math.sin(a) * L * 0.1 - L * 0.12), (t[0] + math.cos(a) * L * 0.1, t[1] + math.sin(a) * L * 0.06)]
            cv.fill(('poly', fin), INK, grow=0.8)
            cv.fill(('poly', fin), ramp[1])
        mid = axis[1]
        cv.fill(('line', [(mid[0] - L * 0.12, mid[1] + L * 0.04), (mid[0] + L * 0.12, mid[1] - L * 0.04)], L * 0.035), C('#d8c020'), 0.9)
        stroke_text(cv, text[:4], mid[0] + L * 0.02, mid[1] - L * 0.1, L * 0.06, CREAM, width=L * 0.012, ink=False)
    sc.add(Y, draw)


def blast_door(sc, X, Y, rng, jammed=False, w=84, d=50):
    """Pre-war bunker hatch: concrete apron, a slanted door frame with hazard
    stripes worn to ghosts, sandbags."""
    cv = sc.cv
    RP = RPf(X, Y, 0.0)
    apron = [RP(-w / 2, -d / 2, 0), RP(w / 2, -d / 2, 0), RP(w / 2, d / 2, 0), RP(-w / 2, d / 2, 0)]
    cv.fill(('poly', apron), INK, grow=1.0)
    cv.fill(('poly', apron), C('#8a8680'))
    for k in range(1, 4):
        u = k / 4
        cv.fill(('line', [quad_pt(apron, u, 0), quad_pt(apron, u, 1)], 0.4), INK, 0.4)
    for k in range(4):
        cv.fill(('line', [quad_pt(apron, rng.uniform(0, 1), rng.uniform(0, 1)), quad_pt(apron, rng.uniform(0, 1), rng.uniform(0, 1))], 0.4), INK, 0.35)
    # the hatch: a raised concrete frame sloping into the ground
    fr_w, fr_d, fr_h = 48, 28, 15
    faces, top, base, rp = box_geom(X, Y + 2, fr_w, fr_d, fr_h, 0.0)
    sc.shadow(shadow_of(base, fr_h))

    def draw():
        M = Mat('#9a968e', 0.9)
        slope = [P(X, Y + 2, -fr_w / 2, -fr_d / 2, 2), P(X, Y + 2, fr_w / 2, -fr_d / 2, 2), top[2], top[3]]
        draw_faces(cv, [(q, M.pick(n)) for q, n, i in faces if i != 0] + [(slope, M.r[0])])
        door = [quad_pt(slope, 0.12, 0.1), quad_pt(slope, 0.88, 0.1), quad_pt(slope, 0.88, 0.9), quad_pt(slope, 0.12, 0.9)]
        cv.fill(('poly', door), INK, grow=0.6)
        cv.fill(('poly', door), C('#5a5e5a') if not jammed else C('#101014'))
        if jammed:
            leaf = [door[0], (door[0][0] + (door[1][0] - door[0][0]) * 0.5, door[0][1] - 5), (door[3][0] + (door[2][0] - door[3][0]) * 0.5, door[3][1] - 5), door[3]]
            cv.fill(('poly', leaf), INK, grow=0.6)
            cv.fill(('poly', leaf), C('#6a6e6a'))
            cv.fill(('poly', door), C('#9ab8c8'), 0.12, mode='glow', grow=2)
        # hazard stripes on the frame lip
        for k in range(8):
            u0, u1 = k / 8, (k + 0.5) / 8
            s = [quad_pt(slope, u0, 0.92), quad_pt(slope, u1, 0.92), quad_pt(slope, u1 + 0.05, 1.0), quad_pt(slope, u0 + 0.05, 1.0)]
            cv.fill(('poly', s), C('#d8b020'), 0.55)
        # wheel on the door
        c = quad_pt(door, 0.5, 0.5)
        cv.fill(('ellipse', c[0], c[1], 5.5, 3.8), INK, grow=0.4)
        cv.fill(('ellipse', c[0], c[1], 4.6, 3.1), C('#8a3a2a'))
        cv.fill(('line', [(c[0] - 4.6, c[1]), (c[0] + 4.6, c[1])], 0.6), INK)
        cv.fill(('line', [(c[0], c[1] - 3.1), (c[0], c[1] + 3.1)], 0.6), INK)
        stroke_text(cv, 'B-7', *quad_pt(slope, 0.5, 0.03), 3.6, CREAM, width=0.8, ink=False)
    sc.add(Y + 2, draw)
    # sandbags either side
    for sgn in (-1, 1):
        for k in range(5):
            bx = X + sgn * (fr_w / 2 + 6) + rng.uniform(-1, 1)
            by = Y + 10 - k * 3.8
            sc.add(by, (lambda bx=bx, by=by: (cv.fill(('ellipse', bx, by, 3.6, 1.7), INK, grow=0.5),
                                              cv.fill(('ellipse', bx, by, 3.6, 1.7), C('#9a8a62')),
                                              cv.fill(('ellipse', bx - 0.8, by - 0.6, 2.2, 0.8), C('#b8a878'), 0.8))))


def grate(sc, cv, X, Y, r, rng, open_panel=False):
    """Collapsed intake: a black drop, rusted grate across it, torn ducting."""
    hole = blob_pts(X, Y, r * 1.25, r * 0.8, sc.noise, 0.22, phase=X)
    cv.fill(('poly', hole), C('#3a3024'))
    cv.fill(('poly', blob_pts(X, Y + 1, r, r * 0.62, sc.noise, 0.15, phase=X + 3)), C('#060406'))
    ring = ellipse_pts(X, Y, r * 1.02, r * 0.66, 28)
    cv.fill(('line', ring + [ring[0]], 2.4), INK)
    cv.fill(('line', ring + [ring[0]], 1.2), C('#7a4a2a'))
    bars = []
    for k in range(-4, 5):
        x = X + k * r * 0.22
        hh = math.sqrt(max(0, 1 - (k * 0.22) ** 2)) * r * 0.64
        if open_panel and k > 1:
            continue
        bars.append(('line', [(x, Y - hh), (x, Y + hh)], 1.0))
    cv.fill(bars, INK, grow=0.3)
    cv.fill(bars, C('#8a5a32'))
    if open_panel:
        flap = [(X + r * 0.3, Y - r * 0.6), (X + r * 1.0, Y - r * 0.5), (X + r * 1.3, Y - r * 1.5), (X + r * 0.6, Y - r * 1.7)]
        cv.fill(('poly', flap), INK, grow=0.6)
        cv.fill(('poly', flap), C('#7a4a2a'))
        for k in range(4):
            cv.fill(('line', [quad_pt(flap, k / 4 + 0.1, 0), quad_pt(flap, k / 4 + 0.1, 1)], 0.6), INK, 0.8)
