"""Big set pieces: aircraft, vehicles, camp structures, bones, signage,
landforms. Same projection and light as tg_props."""
import math
import numpy as np
from tg_core import C, mix, shade, blob_pts, ellipse_pts, wobble
from tg_props import (P, rot, rot3, FY, FZ, INK, INK_W, LIGHT, SHADOW_DX, SHADOW_DY, Mat, make_ramp,
                      box_geom, draw_faces, visible, light_of, ramp_pick, shadow_of, hull, foliage,
                      quad_pt, quad_rect, evil_logo, stroke_text, EVIL_RED, CREAM, GREEN_SICK, cylinder)


# ── tubes: fuselages, buses, pipes ─────────────────────────────────────
def tube_shapes(pts, radii):
    """Union of discs along a screen polyline with per-point radius, plus the
    tangent quads joining them -> a smooth tapered capsule."""
    shapes = []
    for (x, y), r in zip(pts, radii):
        shapes.append(('ellipse', x, y, r, r))
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        r0, r1 = radii[i], radii[i + 1]
        dx, dy = x1 - x0, y1 - y0
        L = math.hypot(dx, dy) or 1
        nx, ny = -dy / L, dx / L
        shapes.append(('poly', [(x0 + nx * r0, y0 + ny * r0), (x1 + nx * r1, y1 + ny * r1),
                                (x1 - nx * r1, y1 - ny * r1), (x0 - nx * r0, y0 - ny * r0)]))
    return shapes


def shade_tube(cv, pts, radii, ramp, ink=True):
    sil = tube_shapes(pts, radii)
    if ink:
        cv.fill(sil, INK, grow=INK_W)
    cv.fill(sil, ramp[2])
    lit = tube_shapes([(x - r * 0.14, y - r * 0.36) for (x, y), r in zip(pts, radii)], [r * 0.72 for r in radii])
    cv.fill(lit, ramp[1], clip=sil)
    hi = tube_shapes([(x - r * 0.2, y - r * 0.62) for (x, y), r in zip(pts, radii)], [r * 0.3 for r in radii])
    cv.fill(hi, ramp[0], clip=sil)
    lo = tube_shapes([(x + r * 0.05, y + r * 0.72) for (x, y), r in zip(pts, radii)], [r * 0.55 for r in radii])
    cv.fill(lo, ramp[3], clip=sil)
    return sil


def flat_poly3(X, Y, pts3, yaw):
    return [P(X, Y, *rot(x, y, yaw), z) for x, y, z in pts3]


def plane(sc, X, Y, L, yaw, kind='bomber', color='#6b6e4e', broken=True, nose_down=0.0,
          overgrown=0.6, rng=None, logo=False, lost_wing=True, stripe=None):
    """A crashed aircraft. kind: 'bomber' (1940s four-engine, olive drab, a
    faded star), 'airliner' (aluminium, Evil Air tail), 'prop' (twin).
    Broken-backed, one wing snapped and lying beside it, grown over."""
    cv, rng, nz = sc.cv, rng or sc.rng, sc.noise
    ramp = make_ramp(color, 1.15)
    r = L * (0.068 if kind != 'airliner' else 0.08)
    span = L * (1.12 if kind == 'bomber' else 0.98)
    chord = L * (0.17 if kind == 'bomber' else 0.15)
    wx = L * 0.05
    wz = r * 0.35
    za = r * 0.82
    tb = 0.36                     # break point, fraction from the tail
    dyaw = -0.26 if broken else 0.0

    def to_screen(x, y, z, piece):
        if piece == 0 and broken:
            # tail piece: pivot about the break, knocked back and round
            px = -L / 2 + L * tb
            x2, y2 = rot(x - px, y, dyaw)
            x, y = x2 + px - L * 0.05, y2
        return P(X, Y, *rot(x, y, yaw), z)

    def prof(t):
        if t < 0.3:
            return r * (0.3 + 0.7 * (t / 0.3) ** 0.7)
        if t > 0.86:
            return r * (0.45 + 0.55 * math.sqrt(max(0.0, 1 - ((t - 0.86) / 0.14) ** 2)))
        return r

    def fus(piece):
        t0, t1 = ((0.0, tb) if piece == 0 else (tb + 0.035, 1.0)) if broken else (0.0, 1.0)
        pts, rad = [], []
        n = 16
        for i in range(n + 1):
            t = t0 + (t1 - t0) * i / n
            x = -L / 2 + L * t
            z = max(prof(t) * 0.7, za - nose_down * (t - 0.5) * L * 0.2)
            pts.append(to_screen(x, 0, z, piece))
            rad.append(prof(t) * 0.97)
        return pts, rad

    def wing_poly(sgn, frac0=0.0, frac1=1.0):
        def at(f, edge):
            x = wx - L * 0.05 * f + (chord * (0.5 - 0.25 * f) if edge else -chord * (0.5 - 0.22 * f))
            return (x, sgn * (r * 0.6 + (span / 2 - r * 0.6) * f), wz + L * 0.006 * f)
        return [at(frac0, 0), at(frac0, 1), at(frac1, 1), at(frac1, 0)]

    snap = 0.45
    wings = []
    for sgn in (-1, 1):
        f1 = snap if (lost_wing and broken and sgn == -1) else 1.0
        w3 = wing_poly(sgn, 0.0, f1)
        poly = [P(X, Y, *rot(x, y, yaw), z) for x, y, z in w3]
        wings.append((sgn, poly, w3, f1))
    # which wing is behind the fuselage on screen
    wings.sort(key=lambda w: sum(p[1] for p in w[1]) / 4)
    far, near = wings[0], wings[1]
    debris = None
    if lost_wing and broken:
        w3 = wing_poly(-1, snap + 0.04, 1.0)
        cx_ = sum(p[0] for p in w3) / 4
        cy_ = sum(p[1] for p in w3) / 4
        dx, dy = -L * 0.08, -r * 1.6
        rot_ = rng.uniform(0.35, 0.8)
        pts = []
        for x, y, z in w3:
            x2, y2 = rot(x - cx_, y - cy_, rot_)
            pts.append((x2 + cx_ + dx, y2 + cy_ + dy, 0.6))
        debris = [P(X, Y, *rot(x, y, yaw), z) for x, y, z in pts]
    # ground shadow: footprint of everything, pushed along the light
    foot = []
    for sgn, poly, w3, f1 in wings:
        foot += [P(X, Y, *rot(x, y, yaw), 0) for x, y, z in w3]
    for piece in ((0, 1) if broken else (1,)):
        pts, rad = fus(piece)
        foot += [(p[0], p[1] + za * FZ) for p in pts[::4]]
    sc.shadow(('poly', hull([(x + SHADOW_DX * r * 1.4, y + SHADOW_DY * r * 1.4) for x, y in foot] + foot)))
    rust = C('#7a3a1c')

    def paint_skin(poly):
        """Flaking paint and rust on a flat surface."""
        for _ in range(4):
            u, v = rng.uniform(0.1, 0.9), rng.uniform(0.15, 0.85)
            p = quad_pt(poly, u, v)
            cv.fill(('poly', blob_pts(p[0], p[1], rng.uniform(2, 5), rng.uniform(1.2, 2.4), nz, 0.45, phase=u * 40 + v)),
                    rust, 0.55, clip=('poly', poly))
        for _ in range(2):
            u, v = rng.uniform(0.1, 0.9), rng.uniform(0.15, 0.85)
            p = quad_pt(poly, u, v)
            cv.fill(('poly', blob_pts(p[0], p[1], rng.uniform(1.5, 3), 1.0, nz, 0.4, phase=u * 70)),
                    C('#b8b8b0'), 0.5, clip=('poly', poly))

    def draw_wing(poly, stub=False):
        cv.fill(('poly', poly), INK, grow=INK_W)
        cv.fill(('poly', poly), ramp[1])
        cv.fill(('poly', [poly[0], poly[3], quad_pt(poly, 0.45, 1), quad_pt(poly, 0.45, 0)]), ramp[0], 0.5)
        for k in (0.33, 0.66):
            cv.fill(('line', [quad_pt(poly, 0, k), quad_pt(poly, 1, k)], 0.35), INK, 0.35)
        paint_skin(poly)
        if stub:
            for k in (0.3, 0.6):
                a = quad_pt(poly, k, 1)
                cv.fill(('line', [a, (a[0] + rng.uniform(-2, 2), a[1] + rng.uniform(1, 3))], 0.8), INK)

    def draw_engines(sgn, f1):
        if kind == 'airliner':
            ks = (0.34,)
        elif kind == 'bomber':
            ks = (0.24, 0.5)
        else:
            ks = (0.3,)
        for k in ks:
            if k > f1:
                continue
            ex = wx - L * 0.05 * k + chord * 0.4
            ey = sgn * (r * 0.6 + (span / 2 - r * 0.6) * k)
            a = P(X, Y, *rot(ex - chord * 0.3, ey, yaw), wz + r * 0.1)
            b = P(X, Y, *rot(ex + chord * 0.45, ey, yaw), wz + r * 0.1)
            shade_tube(cv, [a, b], [r * 0.46, r * 0.4], make_ramp(shade(C(color), 0.9), 1.0))
            if kind != 'airliner':
                for bl in range(3):
                    ang = bl * math.tau / 3 + rng.uniform(-0.3, 0.3)
                    tip = (b[0] + math.cos(ang) * r * 1.1, b[1] + math.sin(ang) * r * 0.9 + rng.uniform(0, 1.2))
                    cv.fill(('line', [b, tip], 1.5), INK)
                    cv.fill(('line', [b, tip], 0.6), C('#5a5048'))

    def draw():
        if debris is not None and sum(p[1] for p in debris) / 4 < Y:
            draw_wing(debris)
        draw_wing(far[1], stub=far[3] < 1)
        draw_engines(far[0], far[3])
        tx = -L / 2 + L * 0.05
        stab_far = [to_screen(tx - L * 0.02, 0, za, 0), to_screen(tx + L * 0.07, 0, za, 0),
                    to_screen(tx + L * 0.02, L * 0.17, za, 0), to_screen(tx - L * 0.04, L * 0.17, za, 0)]
        stab_near = [to_screen(tx - L * 0.02, 0, za, 0), to_screen(tx + L * 0.07, 0, za, 0),
                     to_screen(tx + L * 0.02, -L * 0.17, za, 0), to_screen(tx - L * 0.04, -L * 0.17, za, 0)]
        cv.fill(('poly', stab_far), INK, grow=INK_W)
        cv.fill(('poly', stab_far), ramp[1])
        pieces = [fus(0), fus(1)] if broken else [fus(1)]
        order = sorted(range(len(pieces)), key=lambda i: sum(p[1] for p in pieces[i][0]) / len(pieces[i][0]))
        for i in order:
            pts, rad = pieces[i]
            sil = shade_tube(cv, pts, rad, ramp)
            if stripe:
                cv.fill(('line', [(x, y + rr * 0.05) for (x, y), rr in zip(pts, rad)], max(0.6, r * 0.16)), C(stripe), 0.9, clip=sil)
            for _ in range(5):
                j = rng.integers(1, len(pts) - 1)
                x, y = pts[j]
                cv.fill(('poly', blob_pts(x + rng.uniform(-2, 2), y + rad[j] * rng.uniform(-0.5, 0.4), rad[j] * rng.uniform(0.3, 0.6),
                                          rad[j] * 0.25, nz, 0.5, phase=x)), rust, 0.5, clip=sil)
            step = 1 if kind == 'airliner' else 3
            for j in range(2, len(pts) - 2, step):
                x, y = pts[j]
                cv.fill(('ellipse', x - rad[j] * 0.05, y - rad[j] * 0.12, max(0.45, r * 0.09), max(0.45, r * 0.11)), C('#16141a'), 0.9, clip=sil)
            if broken:
                end = -1 if i == 0 else 0
                x, y = pts[end]
                rr = rad[end]
                cv.fill(('poly', blob_pts(x, y, rr * 0.82, rr * 0.8, nz, 0.4, phase=x + i)), C('#120a0e'), clip=sil)
                for k in range(5):
                    a = rng.uniform(0, math.tau)
                    cv.fill(('line', [(x + math.cos(a) * rr * 0.6, y + math.sin(a) * rr * 0.6),
                                      (x + math.cos(a) * rr * 1.1, y + math.sin(a) * rr * 1.05)], 0.6), INK, 0.9)
        fin = [to_screen(tx - L * 0.03, 0, za + r * 0.5, 0), to_screen(tx + L * 0.12, 0, za + r * 0.6, 0),
               to_screen(tx - L * 0.01, 0, za + r * 0.6 + L * 0.16, 0), to_screen(tx - L * 0.06, 0, za + r * 0.6 + L * 0.15, 0)]
        cv.fill(('poly', fin), INK, grow=INK_W)
        cv.fill(('poly', fin), ramp[1])
        cv.fill(('poly', [fin[0], fin[3], quad_pt(fin, 0.4, 1), quad_pt(fin, 0.4, 0)]), ramp[0], 0.6)
        cv.fill(('poly', stab_near), INK, grow=INK_W)
        cv.fill(('poly', stab_near), ramp[1])
        fm = quad_pt(fin, 0.45, 0.5)
        if logo:
            evil_logo(cv, fm[0], fm[1], r * 0.55)
        elif kind == 'bomber':
            pts5 = []
            for k in range(10):
                a = -math.pi / 2 + k * math.pi / 5
                rr = r * (0.5 if k % 2 == 0 else 0.2)
                pts5.append((fm[0] + math.cos(a) * rr, fm[1] + math.sin(a) * rr))
            cv.fill(('poly', pts5), CREAM, 0.7)
        draw_wing(near[1], stub=near[3] < 1)
        draw_engines(near[0], near[3])
        if debris is not None and sum(p[1] for p in debris) / 4 >= Y:
            draw_wing(debris)
        pts, rad = fus(1)
        nose = pts[-2]
        cv.fill(('ellipse', nose[0] - r * 0.15, nose[1] - r * 0.3, r * 0.42, r * 0.28), C('#22303a'))
        cv.fill(('ellipse', nose[0] - r * 0.28, nose[1] - r * 0.42, r * 0.14, r * 0.09), C('#9ab0b8'), 0.8)
        if overgrown > 0:
            _overgrow_plane(cv, sc, nz, rng, pieces, [far[1], near[1]] + ([debris] if debris else []), overgrown, r)
    sc.add(Y, draw)


MOSS = (C('#56702e'), C('#3a5226'), C('#7a8a3a'))


def _overgrow_plane(cv, sc, nz, rng, pieces, wings, amount, r):
    """Moss on the upper surfaces, vines down the flanks, grass up the belly,
    a sapling out of the break. Nature has had sixty years."""
    for poly in wings:
        for _ in range(int(3 + amount * 5)):
            u, v = rng.uniform(0.05, 0.95), rng.uniform(0.1, 0.9)
            p = quad_pt(poly, u, v)
            cv.fill(('poly', blob_pts(p[0], p[1], rng.uniform(2.5, 6.5), rng.uniform(1.4, 3), nz, 0.5, phase=u * 90 + v)),
                    MOSS[rng.integers(0, 2)], 0.75, clip=('poly', poly))
    for pts, rad in pieces:
        sil = tube_shapes(pts, rad)
        for j in range(1, len(pts) - 1):
            if rng.random() < amount * 0.55:
                x, y = pts[j]
                rr = rad[j]
                cv.fill(('poly', blob_pts(x + rng.uniform(-2, 2), y - rr * 0.55, rr * rng.uniform(0.4, 0.9), rr * 0.35, nz, 0.5, phase=x + j)),
                        MOSS[rng.integers(0, 3)], 0.85, clip=sil)
            if rng.random() < amount * 0.45:
                x, y = pts[j]
                rr = rad[j]
                vine = [(x + rng.uniform(-1, 1), y - rr * 0.85)]
                for k2 in range(5):
                    vine.append((vine[-1][0] + rng.uniform(-0.9, 0.9), vine[-1][1] + rr * 0.38))
                cv.fill(('line', vine, 0.9), C(GREEN_SICK[3]), 0.95)
                cv.fill(('line', vine, 0.45), C(GREEN_SICK[1]), 0.9)
                for (vx, vy) in vine[1::2]:
                    cv.fill(('ellipse', vx + 0.6, vy, 0.9, 0.6), C(GREEN_SICK[1]), 0.9)
        for j in range(0, len(pts)):
            x, y = pts[j]
            rr = rad[j]
            by = y + rr * 0.9
            for k in range(3):
                bx = x + rng.uniform(-2, 2)
                h = rng.uniform(2.5, 5.5)
                a = -math.pi / 2 + rng.uniform(-0.5, 0.5)
                tip = (bx + math.cos(a) * h, by + math.sin(a) * h)
                cv.fill(('line', [(bx, by + 0.6), tip], 0.8), C('#4a3a22'))
                cv.fill(('line', [(bx, by), tip], 0.45), C('#b8a060'), 0.9)
    if len(pieces) > 1 and amount > 0.5:
        x, y = pieces[1][0][0]
        cv.fill(('line', [(x, y), (x - 1, y - r * 1.8)], 1.6), INK)
        cv.fill(('line', [(x, y), (x - 1, y - r * 1.8)], 0.8), C('#4a3222'))
        foliage(cv, x - 1, y - r * 2.4, r * 1.2, nz, ramp=GREEN_SICK, phase=x)


def bus(sc, X, Y, yaw, L=30, color='#d9a02a', tilt=0.0, sunk=0.0, rng=None, school=True):
    """School bus (the flooded district loved one)."""
    cv, rng = sc.cv, rng or sc.rng
    Wd, H = L * 0.3, L * 0.3
    M = Mat(color, 0.9)
    z0 = -sunk * H
    faces, top, base, rp = box_geom(X, Y, L, Wd, H, yaw, z0=z0)
    if sunk < 0.5:
        sc.shadow(shadow_of(base, H * 0.8))

    def draw():
        items = [(q, M.pick(n)) for q, n, i in faces] + [(top, M.r[0])]
        draw_faces(cv, items)
        for q, n, i in faces:
            nw = 6 if i % 2 == 0 else 2
            for k in range(nw):
                u0 = 0.08 + k * 0.84 / nw
                win = quad_rect(q, u0, 0.5, u0 + 0.84 / nw * 0.7, 0.82)
                cv.fill(('poly', win), C('#22262a'))
            if school and i % 2 == 0:
                cv.fill(('line', [quad_pt(q, 0.02, 0.4), quad_pt(q, 0.98, 0.4)], 0.5), INK, 0.8)
        # rusted roof streaks
        for k in range(4):
            u = rng.uniform(0.1, 0.9)
            cv.fill(('line', [quad_pt(top, u, 0.1), quad_pt(top, u + 0.05, 0.9)], 0.8), C('#7a3e1c'), 0.5)
    sc.add(Y, draw)


def trailer(sc, X, Y, yaw, L=20, color='#c9c4b8', rng=None, lit=0.0):
    """Airstream: 1950s aluminium lozenge."""
    cv, rng = sc.cv, rng or sc.rng
    r = L * 0.22
    a3 = [(-L / 2, 0, r), (L / 2, 0, r)]
    pts = [P(X, Y, *rot(x, y, yaw), z) for x, y, z in a3]
    sc.shadow(('poly', hull([(p[0] + SHADOW_DX * r * 2, p[1] + SHADOW_DY * r * 2 + r * FZ) for p in pts] +
                           [(p[0], p[1] + r * FZ) for p in pts])))

    def draw():
        mids = [((pts[0][0] * (1 - t) + pts[1][0] * t), (pts[0][1] * (1 - t) + pts[1][1] * t)) for t in np.linspace(0, 1, 8)]
        rad = [r * (0.8 + 0.2 * math.sin(math.pi * t)) for t in np.linspace(0, 1, 8)]
        shade_tube(cv, mids, rad, make_ramp(color, 1.25))
        for t in (0.3, 0.5, 0.7):
            x = pts[0][0] * (1 - t) + pts[1][0] * t
            y = pts[0][1] * (1 - t) + pts[1][1] * t
            if lit and rng.random() < lit:
                cv.fill(('ellipse', x, y - r * 0.1, r * 0.25, r * 0.2), C('#ffc15a'))
                cv.fill(('ellipse', x, y - r * 0.1, r * 0.5, r * 0.4), C('#ff9a30'), 0.3, mode='glow')
            else:
                cv.fill(('ellipse', x, y - r * 0.1, r * 0.22, r * 0.18), C('#22262a'))
    sc.add(Y, draw)


def dome(sc, X, Y, r, color='#9a8a6a', rng=None, lit=0.0):
    """Geodesic scrap dome hut."""
    cv, rng = sc.cv, rng or sc.rng
    ramp = make_ramp(color, 1.0)
    sc.shadow(('ellipse', X + SHADOW_DX * r, Y + SHADOW_DY * r * 0.5, r * 1.1, r * FY * 1.1))

    def draw():
        cx, cy = X, Y - r * 0.15
        outer = ellipse_pts(cx, cy, r, r * 0.9, 28, math.pi, math.tau) + [(cx + r, cy + r * FY * 0.4), (cx - r, cy + r * FY * 0.4)]
        cv.fill(('poly', outer), INK, grow=INK_W)
        cv.fill(('poly', outer), ramp[2])
        cv.fill(('poly', ellipse_pts(cx - r * 0.15, cy - r * 0.1, r * 0.75, r * 0.7, 20, math.pi, math.tau)), ramp[1], clip=('poly', outer))
        cv.fill(('poly', ellipse_pts(cx - r * 0.3, cy - r * 0.25, r * 0.4, r * 0.38, 16, math.pi, math.tau)), ramp[0], clip=('poly', outer))
        # triangle panel lines
        for k in range(1, 4):
            a = math.pi + math.pi * k / 4
            cv.fill(('line', [(cx, cy - r * 0.9), (cx + math.cos(a) * r, cy + r * FY * 0.3)], 0.4), INK, 0.5)
        for yy in (0.35, 0.65):
            hw = r * math.sqrt(1 - yy * yy)
            cv.fill(('line', [(cx - hw, cy - r * 0.9 * yy), (cx + hw, cy - r * 0.9 * yy)], 0.4), INK, 0.45)
        door = [(cx - r * 0.18, cy + r * FY * 0.35), (cx - r * 0.18, cy - r * 0.2), (cx + r * 0.18, cy - r * 0.2), (cx + r * 0.18, cy + r * FY * 0.35)]
        if lit:
            cv.fill(('poly', door), C('#ffb84a'))
            cv.fill(('poly', door), C('#ff9030'), 0.35, grow=2.5, mode='glow')
        else:
            cv.fill(('poly', door), C('#1c1418'))
    sc.add(Y, draw)


def tent(sc, X, Y, s, yaw, color='#8a6a44', rng=None):
    cv = sc.cv
    M = Mat(color, 1.0)
    w, d, h = 8 * s, 6 * s, 6 * s
    def RP(x, y, z):
        return P(X, Y, *rot(x, y, yaw), z)
    base = [RP(-w / 2, -d / 2, 0), RP(w / 2, -d / 2, 0), RP(w / 2, d / 2, 0), RP(-w / 2, d / 2, 0)]
    sc.shadow(shadow_of(base, h * 0.8))

    def draw():
        s_pl = [RP(-w / 2, -d / 2, 0), RP(w / 2, -d / 2, 0), RP(w / 2, 0, h), RP(-w / 2, 0, h)]
        n_pl = [RP(-w / 2, d / 2, 0), RP(-w / 2, 0, h), RP(w / 2, 0, h), RP(w / 2, d / 2, 0)]
        items = []
        nn = rot3((0, h, d / 2), yaw)
        ns = rot3((0, -h, d / 2), yaw)
        if visible(nn):
            items.append((n_pl, M.pick(nn)))
        for sx in (-1, 1):
            tri = [RP(sx * w / 2, -d / 2, 0), RP(sx * w / 2, d / 2, 0), RP(sx * w / 2, 0, h)]
            ng = rot3((sx, 0, 0), yaw)
            if visible(ng):
                items.append((tri, shade(M.pick(ng), 0.8)))
        if visible(ns):
            items.append((s_pl, M.pick(ns)))
        draw_faces(cv, items, ink_w=0.9)
    sc.add(Y, draw)


def palisade(sc, pts, h=7, color='#6a5a4a', rng=None, gaps=0.1):
    """Scrap-metal wall along a ground polyline: corrugated sheets and
    stakes, each panel its own rusty colour."""
    cv, rng = sc.cv, rng or sc.rng
    panels = []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        L = math.hypot(x1 - x0, y1 - y0)
        n = max(1, int(L / 4.5))
        for k in range(n):
            if rng.random() < gaps:
                continue
            a = (x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n)
            b = (x0 + (x1 - x0) * (k + 1) / n, y0 + (y1 - y0) * (k + 1) / n)
            hh = h * rng.uniform(0.8, 1.15)
            panels.append((a, b, hh))
    for a, b, hh in panels:
        sc.shadow(('poly', [a, b, (b[0] + SHADOW_DX * hh, b[1] + SHADOW_DY * hh), (a[0] + SHADOW_DX * hh, a[1] + SHADOW_DY * hh)]))
    cols = [C('#6a5a4a'), C('#7a4a32'), C('#5a5a5e'), C('#8a6a3a'), C('#4a4a3a')]
    for a, b, hh in panels:
        def draw(a=a, b=b, hh=hh):
            q = [a, b, (b[0], b[1] - hh * FZ), (a[0], a[1] - hh * FZ * rng.uniform(0.85, 1.1))]
            col = cols[rng.integers(len(cols))]
            dx = b[0] - a[0]
            dy = b[1] - a[1]
            # facing: panels whose run goes left->right face the viewer
            lit = light_of((dy, -dx, 0)) if (dx or dy) else 0
            col = shade(col, 1.08) if lit > 0.2 else shade(col, 0.8)
            cv.fill(('poly', q), INK, grow=0.8)
            cv.fill(('poly', q), col)
            for k in range(1, 4):
                u = k / 4
                cv.fill(('line', [quad_pt(q, u, 0), quad_pt(q, u, 1)], 0.35), INK, 0.4)
        sc.add(max(a[1], b[1]), draw)


def barrel(sc, X, Y, s=1.0, color='#3e5a3a', toxic=False, tipped=False):
    cylinder(sc, X, Y, 1.8 * s, 0, 4.2 * s, color, top_color=shade(C(color), 1.15))
    if toxic:
        cv = sc.cv
        def draw():
            cv.fill(('ellipse', X, Y - 2.2 * s * FZ, 1.0 * s, 0.9 * s), C('#d8c020'))
            cv.fill(('ellipse', X, Y - 5.4 * s * FZ + 0.4, 1.2 * s, 0.6 * s), C('#9cff5a'), 0.5, mode='glow')
        sc.add(Y + 0.02, draw)


def billboard(sc, X, Y, w=36, h=16, lift=16, yaw=0.0, text=('EVIL CORP', "WE'RE STILL HERE"), tilt=0.0,
              torn=0.3, rng=None, color='#e8dcc0'):
    """Roadside billboard facing the viewer, on two legs."""
    cv, rng = sc.cv, rng or sc.rng

    def RP(x, z):
        return P(X, Y, *rot(x, 0, yaw), z)
    sc.shadow(('poly', [(X - w / 2, Y), (X + w / 2, Y), (X + w / 2 + SHADOW_DX * (lift + h), Y + SHADOW_DY * (lift + h)),
                        (X - w / 2 + SHADOW_DX * (lift + h), Y + SHADOW_DY * (lift + h))]))

    def draw():
        for lx in (-w * 0.3, w * 0.3):
            a, b = RP(lx, 0), RP(lx + tilt * lift, lift + 1)
            cv.fill(('line', [a, b], 2.0), INK)
            cv.fill(('line', [a, b], 1.0), C('#4a3a30'))
        q = [RP(-w / 2 + tilt * lift, lift), RP(w / 2 + tilt * lift, lift + tilt * 4), RP(w / 2 + tilt * lift, lift + h + tilt * 4),
             RP(-w / 2 + tilt * lift, lift + h)]
        cv.fill(('poly', q), INK, grow=INK_W)
        cv.fill(('poly', q), C(color))
        # the ad: logo left, slogan right
        lp = quad_pt(q, 0.16, 0.5)
        evil_logo(cv, lp[0], lp[1] + 1, h * 0.26)
        if text:
            # size each line to the board: a glyph advances 5.6/6 of its height
            fit = lambda s, frac: min(h * frac, (w * 0.64) / max(1, len(s) * 0.95))
            t1 = quad_pt(q, 0.63, 0.66)
            s1 = fit(text[0], 0.26)
            stroke_text(cv, text[0], t1[0], t1[1], s1, EVIL_RED, width=s1 * 0.26, ink=False)
            if len(text) > 1:
                t2 = quad_pt(q, 0.63, 0.3)
                s2 = fit(text[1], 0.14)
                stroke_text(cv, text[1], t2[0], t2[1], s2, C('#2a2226'), width=s2 * 0.3, ink=False, spacing=0.8)
        if torn > 0:
            for _ in range(int(torn * 5)):
                u, v = rng.uniform(0.05, 0.95), rng.uniform(0.1, 0.9)
                p = quad_pt(q, u, v)
                cv.fill(('poly', blob_pts(p[0], p[1], w * 0.07, h * 0.12, sc.noise, 0.5, phase=u * 50)), C('#9a8a70'), 0.9,
                        clip=('poly', q))
            for _ in range(3):
                u = rng.uniform(0.05, 0.95)
                cv.fill(('line', [quad_pt(q, u, 1), quad_pt(q, u + rng.uniform(-0.02, 0.02), rng.uniform(0.2, 0.7))], 0.7),
                        C('#6a4a30'), 0.6)
    sc.add(Y, draw)


def ribcage(sc, X, Y, L=40, yaw=0.0, color='#d8ccb0', rng=None, ribs=8, h=12):
    """Giant beast skeleton: spine and a row of arched ribs, half sunk."""
    cv, rng = sc.cv, rng or sc.rng
    ramp = make_ramp(color, 1.0)
    pairs = []
    for k in range(ribs):
        x = -L / 2 + L * (k + 0.5) / ribs
        hh = h * (0.55 + 0.45 * math.sin(math.pi * (k + 0.5) / ribs))
        pairs.append((x, hh))
    sp = [P(X, Y, *rot(x, 0, yaw), hh * 0.95) for x, hh in pairs]
    sc.shadow(('line', [(p[0] + SHADOW_DX * 8, p[1] + SHADOW_DY * 8 + h * 0.7) for p in sp], 5))

    def rib(x, hh, sgn):
        pts = []
        for i in range(9):
            a = math.pi / 2 * i / 8
            pts.append(P(X, Y, *rot(x - math.sin(a) * 1.5, sgn * math.sin(a) * L * 0.18, yaw), hh * math.cos(a) + 0.3))
        return pts

    def draw():
        far, near = [], []
        for x, hh in pairs:
            for sgn in (-1, 1):
                pts = rib(x, hh, sgn)
                (far if pts[-1][1] < pts[0][1] + hh * FZ * 0.5 else near).append(pts)
        for pts in far:
            cv.fill(('line', pts, 2.6), INK)
            cv.fill(('line', pts, 1.3), ramp[2])
        cv.fill(('line', sp, 3.6), INK)
        cv.fill(('line', sp, 2.0), ramp[1])
        cv.fill(('line', [(x - 0.3, y - 0.5) for x, y in sp], 0.8), ramp[0])
        for pts in near:
            cv.fill(('line', pts, 2.9), INK)
            cv.fill(('line', pts, 1.5), ramp[1])
            cv.fill(('line', [(x - 0.3, y - 0.3) for x, y in pts[:5]], 0.6), ramp[0], 0.8)
    sc.add(Y, draw)


def skull(sc, X, Y, s=1.0, color='#e0d4b8', horns=True):
    cv = sc.cv
    ramp = make_ramp(color, 1.0)

    def draw():
        head = blob_pts(X, Y - 2.4 * s, 3.0 * s, 2.3 * s, sc.noise, 0.1, phase=X)
        shapes = [('poly', head)]
        if horns:
            for sg in (-1, 1):
                shapes.append(('line', [(X + sg * 2.4 * s, Y - 3.6 * s), (X + sg * 5.2 * s, Y - 5.0 * s), (X + sg * 6.4 * s, Y - 7.4 * s)], 1.2 * s))
        cv.fill(shapes, INK, grow=0.7)
        cv.fill(shapes, ramp[1])
        cv.fill(('poly', blob_pts(X - 0.6 * s, Y - 3.0 * s, 1.8 * s, 1.2 * s, sc.noise, 0.1, phase=X + 3)), ramp[0], clip=shapes[0])
        for sg in (-1, 1):
            cv.fill(('ellipse', X + sg * 1.1 * s, Y - 2.6 * s, 0.75 * s, 0.7 * s), C('#1a1216'))
        cv.fill(('line', [(X - 0.8 * s, Y - 0.6 * s), (X + 0.8 * s, Y - 0.6 * s)], 0.5 * s), INK, 0.8)
    sc.add(Y, draw)


def mascot_head(sc, X, Y, r=18, rng=None, sunk=0.45):
    """The Evil Corp mascot, a fiberglass giant grinning head, buried to the
    nose. The grin survived the bombs better than anyone."""
    cv, rng = sc.cv, rng or sc.rng
    skin = make_ramp('#e8c49a', 1.1)
    sc.shadow(('ellipse', X + SHADOW_DX * r, Y + SHADOW_DY * r, r * 1.05, r * FY))

    def draw():
        cy = Y - r * (1 - sunk) * FZ
        headp = blob_pts(X, cy, r, r * 1.02, sc.noise, 0.04, n=36, phase=X)
        # cut the buried part off at the ground line
        clip = ('poly', [(X - r * 1.4, Y - r * 3), (X + r * 1.4, Y - r * 3), (X + r * 1.4, Y + r * 0.1), (X - r * 1.4, Y + r * 0.1)])
        cv.fill(('poly', headp), INK, grow=INK_W, clip=clip, clip_grow=INK_W)
        cv.fill(('poly', headp), skin[2], clip=clip)
        cv.fill(('poly', blob_pts(X - r * 0.14, cy - r * 0.14, r * 0.84, r * 0.86, sc.noise, 0.04, phase=X + 2)), skin[1], clip=clip)
        cv.fill(('poly', blob_pts(X - r * 0.34, cy - r * 0.4, r * 0.38, r * 0.34, sc.noise, 0.06, phase=X + 5)), skin[0], clip=clip)
        # propeller beanie
        cap = ellipse_pts(X, cy - r * 0.35, r * 0.82, r * 0.72, 18, math.pi, math.tau)
        cv.fill(('poly', cap), INK, grow=INK_W * 0.8)
        cv.fill(('poly', cap), EVIL_RED)
        cv.fill(('poly', ellipse_pts(X - r * 0.2, cy - r * 0.5, r * 0.4, r * 0.4, 12, math.pi, math.tau)), shade(EVIL_RED, 1.2), clip=('poly', cap))
        cv.fill(('line', [(X, cy - r * 1.07), (X, cy - r * 1.3)], 1.0), INK)
        cv.fill(('line', [(X - r * 0.45, cy - r * 1.34), (X + r * 0.45, cy - r * 1.26)], 1.4), INK)
        cv.fill(('line', [(X - r * 0.45, cy - r * 1.34), (X + r * 0.45, cy - r * 1.26)], 0.7), C('#e0c040'))
        # eyes: wide, pinned, delighted
        for sg in (-1, 1):
            ex, ey = X + sg * r * 0.33, cy - r * 0.05
            cv.fill(('ellipse', ex, ey, r * 0.2, r * 0.24), INK, grow=0.4)
            cv.fill(('ellipse', ex, ey, r * 0.2, r * 0.24), CREAM)
            cv.fill(('ellipse', ex + sg * r * 0.04, ey + r * 0.02, r * 0.07, r * 0.09), INK)
        # the grin, which the dirt has reached the bottom of
        g = [(X - r * 0.55, cy + r * 0.28), (X - r * 0.2, cy + r * 0.56), (X + r * 0.2, cy + r * 0.56), (X + r * 0.55, cy + r * 0.28)]
        cv.fill(('poly', g), INK, clip=clip)
        cv.fill(('poly', [(X - r * 0.42, cy + r * 0.33), (X + r * 0.42, cy + r * 0.33), (X + r * 0.3, cy + r * 0.42), (X - r * 0.3, cy + r * 0.42)]), CREAM, clip=clip)
        # a crack across the forehead
        cr = [(X - r * 0.7, cy - r * 0.3), (X - r * 0.35, cy - r * 0.22), (X - r * 0.28, cy - r * 0.05), (X + r * 0.05, cy + r * 0.02)]
        cv.fill(('line', cr, 0.7), INK, 0.8)
    sc.add(Y, draw)


def glowing_eyes(cv, x, y, s=1.0, color='#ffd24a', n=1, rng=None, spread=6):
    """Pairs of eyes in the dark. Additive, the only light in the forest."""
    for i in range(n):
        ex = x + (rng.uniform(-spread, spread) if rng is not None and n > 1 else 0)
        ey = y + (rng.uniform(-spread * 0.5, spread * 0.5) if rng is not None and n > 1 else 0)
        for sg in (-1, 1):
            cv.fill(('ellipse', ex + sg * 1.1 * s, ey, 0.7 * s, 0.45 * s), C(color))
            cv.fill(('ellipse', ex + sg * 1.1 * s, ey, 1.8 * s, 1.3 * s), C(color), 0.25, mode='glow')


def reeds(sc, X, Y, n, rng, h=8, color=('#c8b878', '#8a7a4a')):
    cv = sc.cv
    blades = []
    for i in range(n):
        x = X + rng.uniform(-3, 3)
        a = -math.pi / 2 + rng.uniform(-0.35, 0.35)
        L = h * rng.uniform(0.6, 1.2)
        blades.append(((x, Y), (x + math.cos(a) * L, Y + math.sin(a) * L)))

    def draw():
        for a, b in blades:
            cv.fill(('line', [a, b], 1.2), INK, 0.9)
        for a, b in blades:
            cv.fill(('line', [a, b], 0.55), C(color[1]))
            cv.fill(('line', [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2), b], 0.5), C(color[0]))
            if rng.random() < 0.3:
                cv.fill(('ellipse', b[0], b[1] + 0.8, 0.6, 1.4), C('#5a3a28'))
    sc.add(Y, draw)


def fence(sc, pts, h=4.0, color='#6a5040', rng=None, broken=0.2):
    cv, rng = sc.cv, rng or sc.rng
    posts = []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        L = math.hypot(x1 - x0, y1 - y0)
        n = max(1, int(L / 6))
        for k in range(n + (1 if i == len(pts) - 2 else 0)):
            t = k / n
            posts.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))

    def draw():
        tops = []
        for (x, y) in posts:
            lean = rng.uniform(-0.2, 0.2) if rng.random() < broken else 0
            t = (x + lean * h, y - h * FZ)
            tops.append(t)
            cv.fill(('line', [(x, y), t], 1.5), INK)
            cv.fill(('line', [(x, y), t], 0.7), C(color))
        for i in range(len(tops) - 1):
            if rng.random() < broken:
                continue
            for f in (0.35, 0.8):
                a = (posts[i][0] + (tops[i][0] - posts[i][0]) * f, posts[i][1] + (tops[i][1] - posts[i][1]) * f)
                b = (posts[i + 1][0] + (tops[i + 1][0] - posts[i + 1][0]) * f, posts[i + 1][1] + (tops[i + 1][1] - posts[i + 1][1]) * f)
                cv.fill(('line', [a, b], 0.4), INK, 0.7)
    sc.add(max(p[1] for p in posts) if posts else 0, draw)


def mushroom(sc, X, Y, h, r, rng, cap='#b8423a', stalk='#d8c8a8', glow=False):
    """Mutant toadstool the size of a house."""
    cv = sc.cv
    capR = make_ramp(cap, 1.1)
    stR = make_ramp(stalk, 0.9)
    sc.shadow(('ellipse', X + SHADOW_DX * h * 0.8, Y + SHADOW_DY * h * 0.5, r * 1.1, r * FY))

    def draw():
        top = (X + rng.uniform(-1, 1), Y - h * FZ)
        st = [(X - r * 0.22, Y), (X + r * 0.22, Y), (top[0] + r * 0.16, top[1]), (top[0] - r * 0.16, top[1])]
        cv.fill(('poly', st), INK, grow=INK_W)
        cv.fill(('poly', st), stR[1])
        cv.fill(('poly', [st[1], st[2], ((st[1][0] + st[2][0]) / 2 - r * 0.1, (st[1][1] + st[2][1]) / 2)]), stR[2])
        capp = ellipse_pts(top[0], top[1] + r * 0.1, r, r * 0.75, 24, math.pi, math.tau) + \
               ellipse_pts(top[0], top[1] + r * 0.1, r, r * 0.25, 12, 0, math.pi)
        cv.fill(('poly', capp), INK, grow=INK_W)
        cv.fill(('poly', capp), capR[2])
        cv.fill(('poly', ellipse_pts(top[0] - r * 0.12, top[1], r * 0.8, r * 0.62, 20, math.pi, math.tau)), capR[1], clip=('poly', capp))
        cv.fill(('poly', ellipse_pts(top[0] - r * 0.35, top[1] - r * 0.25, r * 0.35, r * 0.26, 12, math.pi, math.tau)), capR[0], clip=('poly', capp))
        for _ in range(6):
            a = rng.uniform(math.pi * 1.1, math.pi * 1.9)
            rr = rng.uniform(0.3, 0.8)
            cv.fill(('ellipse', top[0] + math.cos(a) * r * rr, top[1] + math.sin(a) * r * 0.6 * rr, r * 0.1, r * 0.08), CREAM, 0.9, clip=('poly', capp))
        if glow:
            cv.fill(('ellipse', top[0], top[1] + r * 0.3, r * 1.3, r * 0.6), C('#c0ff6a'), 0.12, mode='glow')
    sc.add(Y, draw)


def tentacle(sc, X, Y, h, rng, color='#6a4a5a', curl=1.0):
    """Something in the water. Only the arm is ever seen."""
    cv = sc.cv
    ramp = make_ramp(color, 1.1)
    pts, rad = [], []
    n = 14
    a = -math.pi / 2 - 0.3 * curl
    x, y = X, Y
    for i in range(n):
        t = i / (n - 1)
        pts.append((x, y))
        rad.append(max(0.5, (1 - t) * h * 0.1 + 0.4))
        a += 0.23 * curl * (1 + t)
        x += math.cos(a) * h * 0.09
        y += math.sin(a) * h * 0.09

    def draw():
        shade_tube(cv, pts, rad, ramp)
        for i in range(2, n - 2, 2):
            x, y = pts[i]
            cv.fill(('ellipse', x + rad[i] * 0.4, y + rad[i] * 0.2, rad[i] * 0.3, rad[i] * 0.25), CREAM, 0.8)
        cv.fill(('ellipse', X, Y + 0.5, h * 0.18, h * 0.06), C('#9ab0a8'), 0.5)
    sc.add(Y, draw)
