"""Props for the hex tile generator: the things that stand up off the ground.

Projection: an oblique "diorama" view. The ground IS the screen (the game's
hex grid is regular, so the ground plane cannot be foreshortened), and a prop's
own local geometry is squashed in depth by FY and extruded upward by FZ:

    screen = (X + x', Y - y' * FY - z * FZ)      (x', y') = yaw-rotated local

where (X, Y) is where the prop stands on the tile. Faces are back-face culled
against the view direction and cel-shaded by their normal against LIGHT (high,
from the west-south-west, so fronts read, west walls glow and east walls go
dark). Shadows are cast east-north-east along the ground.
"""
import math
from contextlib import contextmanager
from tg_core import C, mix, shade, blob_pts, wobble, ellipse_pts

FY = 0.60
FZ = 0.95
_L = (-0.62, -0.30, 0.72)
_n = math.sqrt(sum(v * v for v in _L))
LIGHT = tuple(v / _n for v in _L)
# ground-plane shadow offset per unit height, in screen px
SHADOW_DX = -LIGHT[0] / LIGHT[2] * 0.55
SHADOW_DY = LIGHT[1] / LIGHT[2] * 0.55 * FY

INK = C('#160d14')
INK_W = 1.25            # default outline width in final px


def P(X, Y, x, y, z):
    return (X + x, Y - y * FY - z * FZ)


def rot(x, y, yaw):
    c, s = math.cos(yaw), math.sin(yaw)
    return (x * c - y * s, x * s + y * c)


def visible(n):
    # view direction toward the camera is (0, -FZ, FY) (see module doc)
    return -n[1] * FZ + n[2] * FY > 1e-4


def light_of(n):
    L = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1
    return (n[0] * LIGHT[0] + n[1] * LIGHT[1] + n[2] * LIGHT[2]) / L


def ramp_pick(ramp, b):
    """ramp = (light, mid, dark, deep); b = n.L in -1..1"""
    if b > 0.62:
        return ramp[0]
    if b > 0.30:
        return ramp[1]
    if b > 0.02:
        return ramp[2]
    return ramp[3]


def make_ramp(base, spread=1.0):
    b = C(base) if isinstance(base, str) else base
    return (shade(b, 1 + 0.22 * spread), b, shade(b, 1 - 0.30 * spread), shade(b, 1 - 0.52 * spread))


# ── scene ───────────────────────────────────────────────────────────────
class Scene:
    """Props sorted back-to-front by the ground y they stand on."""

    def __init__(self, cv, noise, rng):
        self.cv, self.noise, self.rng = cv, noise, rng
        self.items = []          # (sort_y, order, draw_fn)
        self.shadows = []        # list of shapes (ground-plane)
        self._n = 0
        self._ground = None

    def add(self, y, fn):
        self._n += 1
        self.items.append((self._ground if self._ground is not None else y, self._n, fn))

    @contextmanager
    def at(self, x, yg):
        """Place props on relief: yields the lifted screen y for ground point
        (x, yg); everything added inside sorts and depth-tests at yg."""
        old = self._ground
        self._ground = yg
        H = self.cv.H
        if H is None:
            ys = yg
        else:
            from tg_core import S as _S
            xi = int(min(max(x * _S, 0), H.shape[1] - 1))
            yi = int(min(max(yg * _S, 0), H.shape[0] - 1))
            ys = yg - float(H[yi, xi]) * FZ
        try:
            yield ys
        finally:
            self._ground = old

    def shadow(self, shape):
        self.shadows.append(shape)

    def draw_shadows(self, alpha=0.34, color=None):
        if not self.shadows:
            return
        cv = self.cv
        old = cv.ground_clip
        cv.ground_clip = True
        cv.fill(self.shadows, color or C('#2a1a38'), alpha, mode='multiply')
        cv.ground_clip = old

    def draw(self):
        cv = self.cv
        for y, _, fn in sorted(self.items, key=lambda t: (t[0], t[1])):
            cv.zlimit = y
            fn()
        cv.zlimit = None


def shadow_of(poly_base, height):
    """Ground shadow of a footprint polygon extruded to `height`: the hull of
    the footprint and the footprint pushed along the shadow vector."""
    dx, dy = SHADOW_DX * height, SHADOW_DY * height
    pts = list(poly_base) + [(x + dx, y + dy) for x, y in poly_base]
    return ('poly', hull(pts))


def hull(pts):
    pts = sorted(set((round(x, 3), round(y, 3)) for x, y in pts))
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def lerp2(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def quad_pt(q, u, v):
    """Bilinear point on a quad [p00, p10, p11, p01]."""
    a = lerp2(q[0], q[1], u)
    b = lerp2(q[3], q[2], u)
    return lerp2(a, b, v)


def quad_rect(q, u0, v0, u1, v1):
    return [quad_pt(q, u0, v0), quad_pt(q, u1, v0), quad_pt(q, u1, v1), quad_pt(q, u0, v1)]


# ── building blocks ─────────────────────────────────────────────────────
def box_geom(X, Y, w, d, h, yaw, z0=0.0):
    """Returns (faces, top, base) where faces = [(quad, normal3, edge_index)]
    for the visible walls (quad = [b0, b1, t1, t0] bottom-left first)."""
    loc = [(-w / 2, -d / 2), (w / 2, -d / 2), (w / 2, d / 2), (-w / 2, d / 2)]
    rp = [rot(x, y, yaw) for x, y in loc]
    base = [P(X, Y, x, y, z0) for x, y in rp]
    top = [P(X, Y, x, y, z0 + h) for x, y in rp]
    faces = []
    for i in range(4):
        j = (i + 1) % 4
        ex, ey = rp[j][0] - rp[i][0], rp[j][1] - rp[i][1]
        L = math.hypot(ex, ey) or 1
        n = (ey / L, -ex / L, 0.0)
        if visible(n):
            faces.append(([base[i], base[j], top[j], top[i]], n, i))
    return faces, top, base, rp


class Mat:
    def __init__(self, base, spread=1.0):
        self.r = make_ramp(base, spread)

    def pick(self, n):
        return ramp_pick(self.r, light_of(n))


def hatch_poly(cv, poly, spacing=2.0, angle=0.9, w=0.32, alpha=0.55, color=None):
    """Comic shadow hatching: parallel ink lines clipped to a face."""
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    ca, sa = math.cos(angle), math.sin(angle)
    diag = math.hypot(x1 - x0, y1 - y0)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    lines = []
    k = -diag / 2
    while k < diag / 2:
        px, py = cx - sa * k, cy + ca * k
        lines.append(('line', [(px - ca * diag, py - sa * diag), (px + ca * diag, py + sa * diag)], w))
        k += spacing
    if lines:
        cv.fill(lines, color or INK, alpha, clip=('poly', poly))


def lum(c):
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]


def draw_faces(cv, polys_colors, ink=True, ink_w=INK_W, edge_w=0.55, edges=True, hatch=0.22):
    """polys_colors: [(poly, colour)] drawn in order over one ink silhouette.
    Faces in deep shadow get comic hatching."""
    shapes = [('poly', p) for p, _ in polys_colors]
    if ink:
        cv.fill(shapes, INK, 1.0, grow=ink_w)
    for p, col in polys_colors:
        cv.fill(('poly', p), col)
        if hatch and lum(col) < hatch:
            hatch_poly(cv, p, spacing=1.8, alpha=0.45)
    if edges:
        for p, _ in polys_colors:
            cv.fill(('line', list(p) + [p[0]], edge_w), INK, 0.55)


# ── buildings ───────────────────────────────────────────────────────────
def house(sc, X, Y, w, d, h, yaw, wall='#b8a888', roof='#6e4a3c', rh=None, ruined=0.0,
          lit=0.0, windows=True, overgrown=0.0, door=True, chimney=False, rng=None):
    """Gable house. ruined 0..1 knocks holes in the roof and bites the walls;
    lit 0..1 turns windows into warm lamp light (settlements); overgrown puts
    sick green growth over it."""
    cv, rng = sc.cv, rng or sc.rng
    rh = rh if rh is not None else d * 0.55
    ov = 1.2
    wallM, roofM = Mat(wall, 0.9), Mat(roof, 1.0)
    faces, top, base, rp = box_geom(X, Y, w, d, h, yaw)
    sc.shadow(shadow_of(base, h + rh * 0.7))

    def draw():
        items = []
        for q, n, i in faces:
            items.append((q, wallM.pick(n)))
        # gable roof along local x; ridge at y = 0
        def RP(x, y, z):
            rx, ry = rot(x, y, yaw)
            return P(X, Y, rx, ry, z)
        hw, hd = w / 2 + ov, d / 2 + ov
        s_plane = [RP(-hw, -hd, h - 0.4), RP(hw, -hd, h - 0.4), RP(hw, 0, h + rh), RP(-hw, 0, h + rh)]
        n_plane = [RP(-hw, hd, h - 0.4), RP(-hw, 0, h + rh), RP(hw, 0, h + rh), RP(hw, hd, h - 0.4)]
        ns = rot3((0, -rh, d / 2), yaw)
        nn = rot3((0, rh, d / 2), yaw)
        # gable ends
        for sx in (-1, 1):
            tri = [RP(sx * w / 2, -d / 2, h), RP(sx * w / 2, d / 2, h), RP(sx * w / 2, 0, h + rh)]
            ng = rot3((sx, 0, 0), yaw)
            if visible(ng):
                items.append((tri, wallM.pick(ng)))
        roofs = []
        if visible(nn):
            roofs.append((n_plane, roofM.pick(nn)))
        if visible(ns):
            roofs.append((s_plane, roofM.pick(ns)))
        draw_faces(cv, items + roofs)
        # windows / door on visible walls
        for q, n, i in faces:
            long_wall = (i % 2 == 0)
            nwin = max(1, int((w if long_wall else d) / 7))
            if windows:
                for k in range(nwin):
                    u0 = (k + 0.5) / nwin - 0.09
                    u1 = u0 + 0.18
                    win = quad_rect(q, u0, 0.42, u1, 0.78)
                    if lit > 0 and rng.random() < lit:
                        cv.fill(('poly', win), C('#ffc15a'))
                        cv.fill(('poly', win), C('#ff9a30'), 0.35, grow=1.4, mode='glow')
                    else:
                        cv.fill(('poly', win), shade(wallM.r[3], 0.55))
            if door and i == 0:
                dr = quad_rect(q, 0.62, 0.0, 0.78, 0.62)
                cv.fill(('poly', dr), shade(wallM.r[3], 0.5))
        if chimney:
            cx0, cy0 = rot(w * 0.25, d * 0.15, yaw)
            ch_f, ch_t, _, _ = box_geom(X + cx0, Y - cy0 * FY, 2.2, 2.2, rh * 0.85, yaw, z0=h + rh * 0.55)
            draw_faces(cv, [(q, Mat('#8a4a38').pick(n)) for q, n, _ in ch_f] + [(ch_t, C('#3a2420'))], edges=False)
        if ruined > 0:
            _ruin(cv, sc, s_plane if visible(ns) else n_plane, faces, ruined, rng)
        if overgrown > 0:
            _overgrow(cv, sc, [p for p, _ in items + roofs], overgrown, rng)
    sc.add(Y, draw)


def rot3(n, yaw):
    x, y = rot(n[0], n[1], yaw)
    return (x, y, n[2])


def _ruin(cv, sc, roof_q, faces, amount, rng):
    """Punch jagged holes through a roof plane and gnaw wall tops."""
    k = int(1 + amount * 2.5)
    for _ in range(k):
        u, v = rng.uniform(0.15, 0.85), rng.uniform(0.15, 0.8)
        r = rng.uniform(0.12, 0.22) * (0.6 + amount)
        pts = []
        for i in range(9):
            a = math.tau * i / 9
            rr = r * rng.uniform(0.55, 1.15)
            pts.append(quad_pt(roof_q, min(0.98, max(0.02, u + math.cos(a) * rr)), min(0.98, max(0.02, v + math.sin(a) * rr * 1.4))))
        cv.fill(('poly', pts), C('#1c1214'))
        # a rafter or two across the hole
        a0 = quad_pt(roof_q, u - r, v - r * 0.4)
        a1 = quad_pt(roof_q, u + r, v + r * 0.2)
        cv.fill(('line', [a0, a1], 0.8), C('#5a3a28'), 0.9)
        cv.fill(('line', [a0, a1], 0.4), INK, 0.4)
    for q, n, i in faces:
        if rng.random() < amount * 0.8:
            u = rng.uniform(0.1, 0.6)
            bite = [quad_pt(q, u, 1.02), quad_pt(q, u + 0.12, 0.72), quad_pt(q, u + 0.2, 0.85),
                    quad_pt(q, u + 0.34, 0.66), quad_pt(q, u + 0.42, 1.02)]
            cv.fill(('poly', bite), C('#1c1214'))


GREEN_SICK = ('#9aa84a', '#6f8a36', '#4a6a2c', '#2c4424')


def foliage(cv, cx, cy, r, noise, ramp=GREEN_SICK, phase=0.0, ink=True, squash=0.8, lumps=0.28):
    """A cel-shaded lumpy canopy: deep base, mid offset toward the light, a
    small warm highlight; ink silhouette under it all."""
    rp = C(ramp[0]), C(ramp[1]), C(ramp[2]), C(ramp[3])
    outer = blob_pts(cx, cy, r, r * squash, noise, lumps=lumps, n=26, phase=phase)
    if ink:
        cv.fill(('poly', outer), INK, 1.0, grow=INK_W * 0.9)
    cv.fill(('poly', outer), rp[3])
    mid = blob_pts(cx - r * 0.12, cy - r * 0.14, r * 0.86, r * 0.86 * squash, noise, lumps=lumps, n=24, phase=phase + 3)
    cv.fill(('poly', mid), rp[2], clip=('poly', outer))
    hi = blob_pts(cx - r * 0.3, cy - r * 0.34, r * 0.6, r * 0.6 * squash, noise, lumps=lumps * 1.2, n=20, phase=phase + 9)
    cv.fill(('poly', hi), rp[1], clip=('poly', outer))
    hi2 = blob_pts(cx - r * 0.42, cy - r * 0.46, r * 0.3, r * 0.3 * squash, noise, lumps=lumps, n=14, phase=phase + 17)
    cv.fill(('poly', hi2), rp[0], clip=('poly', outer))
    return outer


def _overgrow(cv, sc, polys, amount, rng):
    n = int(2 + amount * 6)
    allp = [p for poly in polys for p in poly]
    if not allp:
        return
    for _ in range(n):
        x, y = allp[rng.integers(len(allp))]
        foliage(cv, x + rng.uniform(-2, 2), y + rng.uniform(-1, 2), rng.uniform(2.0, 4.5) * (0.6 + amount),
                sc.noise, phase=rng.uniform(0, 99))


def factory(sc, X, Y, w, d, h, yaw, wall='#8f8a80', roof='#5d5a58', saw=4, stacks=1, ruined=0.3,
            rng=None, logo=False, smoke=False):
    """Light-industrial shed: sawtooth north-light roof, a smokestack or two."""
    cv, rng = sc.cv, rng or sc.rng
    wallM, roofM = Mat(wall, 0.8), Mat(roof, 0.9)
    faces, top, base, rp = box_geom(X, Y, w, d, h, yaw)
    sc.shadow(shadow_of(base, h + 3))

    def RP(x, y, z):
        rx, ry = rot(x, y, yaw)
        return P(X, Y, rx, ry, z)

    def draw():
        items = [(q, wallM.pick(n)) for q, n, i in faces]
        items.append((top, roofM.r[2]))
        draw_faces(cv, items)
        # sawtooth teeth running along local x
        th = 3.2
        for k in range(saw):
            y0 = -d / 2 + d * k / saw
            y1 = y0 + d / saw
            glass = [RP(-w / 2 + 0.6, y1, h), RP(w / 2 - 0.6, y1, h), RP(w / 2 - 0.6, y1, h + th), RP(-w / 2 + 0.6, y1, h + th)]
            slope = [RP(-w / 2 + 0.6, y0, h), RP(w / 2 - 0.6, y0, h), RP(w / 2 - 0.6, y1, h + th), RP(-w / 2 + 0.6, y1, h + th)]
            ns = rot3((0, -th, d / saw), yaw)
            draw_faces(cv, [(slope, roofM.pick(ns))], ink_w=0.6)
            ng = rot3((0, 1, 0), yaw)
            if visible(ng):
                cv.fill(('poly', glass), C('#2a3a44'))
        # windows row on the front wall
        for q, n, i in faces:
            nw = int(max(2, (w if i % 2 == 0 else d) / 5))
            for k in range(nw):
                u0 = (k + 0.25) / nw
                win = quad_rect(q, u0, 0.5, u0 + 0.5 / nw, 0.8)
                cv.fill(('poly', win), C('#1e2226') if rng.random() < 0.7 else C('#3a4a50'))
            if logo and i == 0:
                evil_logo(cv, *quad_pt(q, 0.2, 0.3), 2.6)
        if ruined > 0:
            _ruin(cv, sc, top, [], ruined * 0.6, rng)
    sc.add(Y, draw)
    for s in range(stacks):
        sx, sy = rot(w * (0.3 - 0.25 * s), d * 0.2, yaw)
        smokestack(sc, X + sx, Y - sy * FY + 0.2, 2.4, h + 18 + 6 * s, smoke=smoke)


def smokestack(sc, X, Y, r, h, color='#8a4a38', smoke=False):
    cylinder(sc, X, Y, r, 0, h, color, bands=True)
    if smoke:
        cv, n = sc.cv, sc.noise
        def draw():
            for i in range(5):
                t = i / 4
                cx = X + 3 + t * 10
                cy = Y - h * FZ - 3 - t * 9
                cv.fill(('poly', blob_pts(cx, cy, 3 + t * 5, 2.4 + t * 3.5, n, 0.3, phase=i * 7)),
                        mix(C('#6b6470'), C('#3b3444'), t), 0.55 - t * 0.35)
        sc.add(Y + 0.01, draw)


def cylinder(sc, X, Y, r, z0, z1, color, bands=True, top_color=None, ink=True, rim=True, draw_now=False):
    cv = sc.cv
    ramp = make_ramp(color, 1.0)
    sc.shadow(('poly', shadow_of(ellipse_pts(X, Y, r, r * FY, 16), z1 - z0)[1]))

    def draw():
        cb = P(X, Y, 0, 0, z0)
        ct = P(X, Y, 0, 0, z1)
        ry = r * FY
        body = ellipse_pts(cb[0], cb[1], r, ry, 16, 0, math.pi) + ellipse_pts(ct[0], ct[1], r, ry, 16, math.pi, 0)
        body = [p for p in body]
        body_poly = [(cb[0] + r, cb[1])] + ellipse_pts(cb[0], cb[1], r, ry, 16, 0, math.pi)[1:] + \
                    [(ct[0] - r, ct[1]), (ct[0] + r, ct[1])]
        topE = ('ellipse', ct[0], ct[1], r, ry)
        if ink:
            cv.fill([('poly', body_poly), topE], INK, 1.0, grow=INK_W)
        cv.fill(('poly', body_poly), ramp[2])
        if bands:
            # lit west flank, bright sliver, dark east flank
            def band(a0, a1):
                arcb = ellipse_pts(cb[0], cb[1], r, ry, 8, a1, a0)
                xs0, xs1 = cb[0] + math.cos(a0) * r, cb[0] + math.cos(a1) * r
                return [(xs0, ct[1] + math.sin(a0) * ry)] + [(xs1, ct[1] + math.sin(a1) * ry)] + arcb
            cv.fill(('poly', band(math.pi * 0.62, math.pi * 1.0 - 0.0001)), ramp[1], clip=('poly', body_poly))
            cv.fill(('poly', band(math.pi * 0.80, math.pi * 0.98)), ramp[0], clip=('poly', body_poly))
            cv.fill(('poly', band(0.0001, math.pi * 0.25)), ramp[3], clip=('poly', body_poly))
        cv.fill(topE, top_color or ramp[0])
        if rim:
            cv.fill(('line', ellipse_pts(ct[0], ct[1], r, ry, 20), 0.5), INK, 0.6)
    if draw_now:
        draw()
    else:
        sc.add(Y, draw)


def water_tower(sc, X, Y, s=1.0, tank='#b8b0a0', legs='#4a4040', text='EVIL CORP', toppled=False, rng=None):
    cv = sc.cv
    lh = 26 * s
    r = 8 * s
    th = 10 * s
    sc.shadow(('poly', shadow_of(ellipse_pts(X, Y - 1, r * 0.9, r * 0.9 * FY, 12), lh + th)[1]))

    def draw():
        for (lx, ly) in [(-r * 0.75, -r * 0.4), (r * 0.75, -r * 0.4), (-r * 0.6, r * 0.5), (r * 0.6, r * 0.5)]:
            b = P(X, Y, lx, ly, 0)
            t = P(X, Y, lx * 0.8, ly * 0.8, lh)
            cv.fill(('line', [b, t], 1.3 * s), INK)
            cv.fill(('line', [b, t], 0.6 * s), C(legs))
        a = P(X, Y, -r * 0.75, -r * 0.4, lh * 0.45)
        b2 = P(X, Y, r * 0.75, -r * 0.4, lh * 0.45)
        cv.fill(('line', [a, b2], 0.6), INK)
        cv.fill(('line', [P(X, Y, -r * 0.75, -r * 0.4, 0), P(X, Y, r * 0.6, -r * 0.4, lh * 0.8)], 0.45), INK, 0.8)
        cylinder(sc, X, Y, r, lh, lh + th, tank, draw_now=True)
        # conical cap
        ct = P(X, Y, 0, 0, lh + th)
        apex = P(X, Y, 0, 0, lh + th + 6 * s)
        cone = [(ct[0] - r - 0.6, ct[1]), (ct[0] + r + 0.6, ct[1]), apex]
        cv.fill(('poly', cone), INK, grow=INK_W)
        cv.fill(('poly', cone), shade(C(tank), 0.72))
        cv.fill(('poly', [(ct[0] - r - 0.6, ct[1]), (apex[0] - 0.4, apex[1]), (ct[0] - r * 0.1, ct[1])]), shade(C(tank), 1.05))
        mid = P(X, Y, 0, -r, lh + th * 0.5)
        if text:
            stroke_text(cv, text, mid[0], mid[1], 2.4 * s, C('#b8231c'), width=0.9 * s, ink=False, spacing=0.9)
    sc.add(Y, draw)


def pole(sc, X, Y, h=22, lean=0.0, cross=True, color='#5a4232'):
    cv = sc.cv
    top = (X + lean * h, Y - h * FZ)
    sc.shadow(('line', [(X, Y), (X + SHADOW_DX * h, Y + SHADOW_DY * h)], 1.2))

    def draw():
        cv.fill(('line', [(X, Y), top], 1.9), INK)
        cv.fill(('line', [(X, Y), top], 0.9), C(color))
        if cross:
            a = (top[0] - 4, top[1] + 2.5 + lean * 3)
            b = (top[0] + 4, top[1] + 2.5 - lean * 3)
            cv.fill(('line', [a, b], 1.5), INK)
            cv.fill(('line', [a, b], 0.6), C(color))
    sc.add(Y, draw)
    return top


def wire(cv, a, b, sag=4.0, w=0.45, alpha=0.8):
    pts = []
    for i in range(13):
        t = i / 12
        pts.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t + sag * 4 * t * (1 - t)))
    cv.fill(('line', pts, w), INK, alpha)


def pylon(sc, X, Y, h=40, lean=0.0, broken=False):
    """Lattice transmission tower (the old tiles loved these)."""
    cv = sc.cv
    sc.shadow(('poly', [(X - 5, Y), (X + 5, Y), (X + 5 + SHADOW_DX * h, Y + SHADOW_DY * h), (X - 1 + SHADOW_DX * h, Y + SHADOW_DY * h)]))

    def draw():
        def at(t, side):
            w = 6.5 * (1 - t) + 1.2 * t
            return (X + side * w + lean * h * t, Y - h * FZ * t)
        lines = []
        levels = [0, 0.18, 0.36, 0.55, 0.72, 0.86, 1.0]
        if broken:
            levels = levels[:5]
        for s in (-1, 1):
            lines.append([at(levels[0], s), at(levels[-1], s)])
        for i in range(len(levels) - 1):
            lines.append([at(levels[i], -1), at(levels[i + 1], 1)])
            lines.append([at(levels[i], 1), at(levels[i + 1], -1)])
            lines.append([at(levels[i + 1], -1), at(levels[i + 1], 1)])
        for t in (0.72, 0.86):
            if t in levels:
                a, b = at(t, -1), at(t, 1)
                lines.append([(a[0] - 6, a[1]), (b[0] + 6, b[1])])
        for ln in lines:
            cv.fill(('line', ln, 1.1), INK, 0.95)
        for ln in lines[:2]:
            cv.fill(('line', ln, 0.5), C('#6a6a70'))
    sc.add(Y, draw)


def car(sc, X, Y, yaw, color='#5f8f86', fins=False, rust=0.5, flipped=False, s=1.0, rng=None):
    cv = sc.cv
    rng = rng or sc.rng
    L, Wd = 9.5 * s, 4.6 * s
    body = Mat(mix(C(color), C('#7a4a2a'), rust * 0.6), 0.9)
    faces, top, base, rp = box_geom(X, Y, L, Wd, 2.4 * s, yaw)
    sc.shadow(shadow_of(base, 3.5 * s))

    def draw():
        items = [(q, body.pick(n)) for q, n, i in faces] + [(top, body.r[0])]
        draw_faces(cv, items, ink_w=0.9, edge_w=0.35)
        # cabin
        cf, ctop, cb, _ = box_geom(X + rot(-0.6 * s, 0, yaw)[0], Y - rot(-0.6 * s, 0, yaw)[1] * FY - 2.4 * s * FZ,
                                  L * 0.5, Wd * 0.86, 1.9 * s, yaw)
        items2 = [(q, C('#1e262a')) for q, n, i in cf] + [(ctop, body.r[1])]
        draw_faces(cv, items2, ink_w=0.7, edge_w=0.3)
        if fins:
            for sy in (-1, 1):
                a = P(X, Y, *rot(-L / 2, sy * Wd * 0.4, yaw), 2.4 * s)
                b = P(X, Y, *rot(-L / 2 + 2.5 * s, sy * Wd * 0.4, yaw), 2.4 * s)
                c = P(X, Y, *rot(-L / 2, sy * Wd * 0.4, yaw), 4.2 * s)
                cv.fill(('poly', [a, b, c]), INK, grow=0.5)
                cv.fill(('poly', [a, b, c]), body.r[0])
    sc.add(Y, draw)


def rock(sc, X, Y, r, color='#7a6a5a', rng=None, h=None):
    cv, rng = sc.cv, rng or sc.rng
    h = h if h is not None else r * rng.uniform(0.7, 1.1)
    ramp = make_ramp(color, 1.0)
    n = rng.integers(5, 8)
    base = []
    for i in range(n):
        a = math.tau * i / n + rng.uniform(-0.3, 0.3)
        rr = r * rng.uniform(0.75, 1.1)
        base.append((X + math.cos(a) * rr, Y + math.sin(a) * rr * FY))
    apex = (X - r * 0.15 + rng.uniform(-r * 0.2, r * 0.2), Y - h * FZ)
    sc.shadow(shadow_of(base, h * 0.8))

    def draw():
        sil = hull(base + [apex])
        cv.fill(('poly', sil), INK, grow=INK_W * 0.8)
        cv.fill(('poly', sil), ramp[2])
        # facets from the apex to each base vertex; west-facing ones lit
        ordered = sorted(base, key=lambda p: math.atan2(p[1] - Y, p[0] - X))
        for i in range(len(ordered)):
            a, b = ordered[i], ordered[(i + 1) % len(ordered)]
            mx = (a[0] + b[0]) / 2 - X
            my = (a[1] + b[1]) / 2 - Y
            if my < -r * 0.2 * FY:
                continue
            nx = mx / (r or 1)
            col = ramp[0] if nx < -0.35 else ramp[1] if nx < 0.25 else ramp[3]
            cv.fill(('poly', [apex, a, b]), col, clip=('poly', sil))
        cv.fill(('poly', [apex, (apex[0] - r * 0.5, apex[1] + h * 0.5), (apex[0] - r * 0.05, apex[1] + h * 0.35)]),
                ramp[0], 0.8, clip=('poly', sil))
    sc.add(Y, draw)


def tuft(cv, x, y, s, col_light, col_dark, rng):
    """Dry grass tuft: three to five ink-tipped blades."""
    n = rng.integers(3, 6)
    for i in range(n):
        a = -math.pi / 2 + rng.uniform(-0.75, 0.75)
        L = s * rng.uniform(0.7, 1.3)
        tip = (x + math.cos(a) * L, y + math.sin(a) * L)
        cv.fill(('line', [(x + rng.uniform(-0.6, 0.6), y), tip], 0.7), col_dark)
        cv.fill(('line', [(x, y), ((x + tip[0]) / 2 - 0.2, (y + tip[1]) / 2)], 0.5), col_light, 0.9)


def dead_tree(sc, X, Y, h, rng, color='#3a2a26', lean=0.0, spread=1.0, fungus=None, noise=None):
    """Branching bare tree; with fungus=ramp it grows rust shelf-fungus clumps
    along its limbs (the Rust Forest)."""
    cv = sc.cv
    segs = []

    def grow(x, y, a, L, w, depth):
        if depth == 0 or L < 1.5:
            return
        x2 = x + math.cos(a) * L
        y2 = y + math.sin(a) * L
        segs.append(((x, y), (x2, y2), w))
        k = 2 if depth > 1 else rng.integers(1, 3)
        for i in range(k):
            da = rng.uniform(0.35, 0.8) * (1 if i % 2 == 0 else -1) * spread
            grow(x2, y2, a + da + rng.uniform(-0.15, 0.15), L * rng.uniform(0.62, 0.78), w * 0.66, depth - 1)
    a0 = -math.pi / 2 + lean
    grow(X, Y, a0, h * 0.42, max(1.2, h * 0.07), 5)
    sc.shadow(('line', [(X, Y), (X + SHADOW_DX * h * 0.8, Y + SHADOW_DY * h * 0.8)], max(1.0, h * 0.06)))

    def draw():
        for (a, b, w) in segs:
            cv.fill(('line', [a, b], w + 1.1), INK)
        for (a, b, w) in segs:
            cv.fill(('line', [a, b], max(0.35, w * 0.6)), C(color))
        if fungus:
            nz = noise or sc.noise
            tips = [b for (a, b, w) in segs if w < h * 0.05]
            joints = [a for (a, b, w) in segs[1:]]
            pts = tips[::2] + joints[::3]
            for i, (x, y) in enumerate(pts):
                rr = rng.uniform(1.6, 3.4) * (h / 30)
                foliage(cv, x, y, rr, nz, ramp=fungus, phase=i * 3.1 + X, squash=0.7, lumps=0.35)
    sc.add(Y, draw)


def conifer(sc, X, Y, h, rng, ramp=('#6f7a4a', '#4c5a36', '#34402a', '#1e2a1e'), dead=False):
    cv = sc.cv
    w = h * 0.38
    sc.shadow(('poly', [(X - 1, Y), (X + 1, Y), (X + SHADOW_DX * h, Y + SHADOW_DY * h)]))

    def draw():
        tiers = 4
        pts_all = []
        for t in range(tiers):
            y0 = Y - h * FZ * (0.12 + t * 0.2)
            y1 = y0 - h * FZ * 0.42
            ww = w * (1 - t * 0.2)
            tri = [(X - ww, y0), (X + ww, y0), (X + rng.uniform(-0.6, 0.6), y1)]
            pts_all.append(tri)
        cv.fill([('poly', p) for p in pts_all], INK, grow=0.9)
        for t, tri in enumerate(pts_all):
            cv.fill(('poly', tri), C(ramp[2]))
            cv.fill(('poly', [tri[0], ((tri[0][0] + tri[1][0]) / 2 - 1, tri[0][1]), tri[2]]), C(ramp[1]))
        cv.fill(('line', [(X, Y), (X, Y - h * FZ * 0.14)], 1.2), C('#3a2a22'))
    sc.add(Y, draw)


def cactus(sc, X, Y, h, rng, color='#6f8a5a'):
    """Saguaro: 1940s roadside-postcard desert, now it sings."""
    cv = sc.cv
    ramp = make_ramp(color, 1.0)
    w = max(1.6, h * 0.12)
    sc.shadow(('line', [(X, Y), (X + SHADOW_DX * h, Y + SHADOW_DY * h)], w))

    def draw():
        trunk = [(X, Y), (X, Y - h * FZ)]
        arms = []
        for s in (-1, 1):
            if rng.random() < 0.8:
                y0 = Y - h * FZ * rng.uniform(0.35, 0.55)
                ax = X + s * w * 1.8
                arms.append([(X, y0), (ax, y0), (ax, y0 - h * FZ * rng.uniform(0.25, 0.4))])
        shapes = [('line', trunk, w * 2)] + [('line', a, w * 1.4) for a in arms]
        cv.fill(shapes, INK, grow=INK_W * 0.8)
        cv.fill(shapes, ramp[2])
        cv.fill([('line', [(p[0] - w * 0.35, p[1]) for p in trunk], w * 0.8)] +
                [('line', [(p[0] - w * 0.25, p[1]) for p in a], w * 0.5) for a in arms], ramp[0], clip=shapes)
    sc.add(Y, draw)


# ── Evil Corp and lettering ─────────────────────────────────────────────
EVIL_RED = C('#b8231c')
CREAM = C('#efe3c6')


def evil_logo(cv, x, y, r, ink=True):
    """EVIL CORP: a red roundel, a cream E, two little horns. Reads as a red
    dot with ears at 3 px, which is all it needs to do from across the map."""
    horns = [[(x - r * 0.55, y - r * 0.72), (x - r * 0.85, y - r * 1.45), (x - r * 0.2, y - r * 0.9)],
             [(x + r * 0.55, y - r * 0.72), (x + r * 0.85, y - r * 1.45), (x + r * 0.2, y - r * 0.9)]]
    disc = ('ellipse', x, y, r, r)
    if ink:
        cv.fill([disc] + [('poly', h) for h in horns], INK, grow=0.6)
    cv.fill([('poly', h) for h in horns], EVIL_RED)
    cv.fill(disc, EVIL_RED)
    w = max(0.35, r * 0.22)
    cv.fill([('line', [(x + r * 0.35, y - r * 0.45), (x - r * 0.3, y - r * 0.45), (x - r * 0.3, y + r * 0.45),
                        (x + r * 0.35, y + r * 0.45)], w),
             ('line', [(x - r * 0.3, y), (x + r * 0.2, y)], w)], CREAM)


# 5x7 stroke font: each glyph is a list of polylines on a 0..4 x 0..6 grid
_F = {
    'A': [[(0, 6), (0, 2), (2, 0), (4, 2), (4, 6)], [(0, 3.5), (4, 3.5)]],
    'B': [[(0, 0), (0, 6), (3, 6), (4, 5), (4, 4), (3, 3), (0, 3)], [(0, 0), (3, 0), (4, 1), (4, 2), (3, 3)]],
    'C': [[(4, 0.5), (3, 0), (1, 0), (0, 1), (0, 5), (1, 6), (3, 6), (4, 5.5)]],
    'D': [[(0, 0), (0, 6), (2.5, 6), (4, 4.5), (4, 1.5), (2.5, 0), (0, 0)]],
    'E': [[(4, 0), (0, 0), (0, 6), (4, 6)], [(0, 3), (3, 3)]],
    'F': [[(4, 0), (0, 0), (0, 6)], [(0, 3), (3, 3)]],
    'G': [[(4, 0.5), (3, 0), (1, 0), (0, 1), (0, 5), (1, 6), (3, 6), (4, 5), (4, 3.5), (2.2, 3.5)]],
    'H': [[(0, 0), (0, 6)], [(4, 0), (4, 6)], [(0, 3), (4, 3)]],
    'I': [[(1, 0), (3, 0)], [(2, 0), (2, 6)], [(1, 6), (3, 6)]],
    'K': [[(0, 0), (0, 6)], [(4, 0), (0, 3.5)], [(1.4, 2.6), (4, 6)]],
    'L': [[(0, 0), (0, 6), (4, 6)]],
    'M': [[(0, 6), (0, 0), (2, 3), (4, 0), (4, 6)]],
    'N': [[(0, 6), (0, 0), (4, 6), (4, 0)]],
    'O': [[(1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0)]],
    'P': [[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2.2), (3, 3.2), (0, 3.2)]],
    'R': [[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2.2), (3, 3.2), (0, 3.2)], [(2, 3.2), (4, 6)]],
    'S': [[(4, 0.6), (3, 0), (1, 0), (0, 1), (0, 2.2), (1, 3), (3, 3), (4, 3.8), (4, 5), (3, 6), (1, 6), (0, 5.4)]],
    'T': [[(0, 0), (4, 0)], [(2, 0), (2, 6)]],
    'U': [[(0, 0), (0, 5), (1, 6), (3, 6), (4, 5), (4, 0)]],
    'V': [[(0, 0), (2, 6), (4, 0)]],
    'W': [[(0, 0), (1, 6), (2, 3), (3, 6), (4, 0)]],
    'Y': [[(0, 0), (2, 3), (4, 0)], [(2, 3), (2, 6)]],
    'Z': [[(0, 0), (4, 0), (0, 6), (4, 6)]],
    '0': [[(1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0)]],
    '1': [[(1, 1), (2, 0), (2, 6)], [(1, 6), (3, 6)]],
    '2': [[(0, 1), (1, 0), (3, 0), (4, 1), (4, 2.5), (0, 6), (4, 6)]],
    '5': [[(4, 0), (0, 0), (0, 3), (3, 2.6), (4, 3.6), (4, 5), (3, 6), (0, 6)]],
    '7': [[(0, 0), (4, 0), (1.5, 6)]],
    '!': [[(2, 0), (2, 4)], [(2, 5.6), (2, 6)]],
    '-': [[(0.8, 3), (3.2, 3)]],
    "'": [[(2, 0), (1.6, 1.6)]],
    '.': [[(2, 5.6), (2, 6)]],
}


def stroke_text(cv, text, x, y, size, color, width=0.8, ink=True, spacing=1.0, ink_w=0.7):
    """Centered at (x, y); size = cap height in px."""
    k = size / 6.0
    adv = (4 + 1.6 * spacing) * k
    total = adv * len(text) - 1.6 * spacing * k
    x0 = x - total / 2
    y0 = y - size / 2
    lines = []
    for i, ch in enumerate(text.upper()):
        for pl in _F.get(ch, []):
            lines.append(('line', [(x0 + i * adv + px * k, y0 + py * k) for px, py in pl], width))
    if not lines:
        return
    if ink:
        cv.fill(lines, INK, grow=ink_w)
    cv.fill(lines, color)
