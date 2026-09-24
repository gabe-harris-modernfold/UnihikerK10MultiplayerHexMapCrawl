"""The bunker system: Bunker Entrance (12), Vent Shaft (13), Tunnel Floor
(14), Collapsed Tunnel (15). 12 and 13 are drawn on the surface map AND as
the shaft cells beneath it, so they sit on plain ground."""
import math
import numpy as np
from tg_core import C, mix, shade, hex_pts, hex_sdf, blob_pts, ellipse_pts, CX, CY, R, HEX_TOP, HEX_BOT, HALF_H
from tg_props import (Scene, INK, P, FY, FZ, rot, rock, tuft, foliage, evil_logo, stroke_text, EVIL_RED, CREAM,
                      box_geom, draw_faces, Mat, quad_pt, quad_rect, shadow_of, pole, cylinder)
from tg_props2 import barrel, glowing_eyes, shade_tube
from tg_props3 import blast_door, grate, rubble
from tg_ground import rim, crack, smooth_path, random_in_hex, stroke_texture, flat_ground
from tg_relief import relief, grid_ground, rim_falloff, ridged, RELIEF_LIGHT
from tg_common import inside, scatter, new, finish, swell, scrub_base, scrub_scatter


def bunker_entrance(v, seed):
    cv, rng, nz = new(seed)
    scrub_base(cv, nz, rng, strokes=1100)
    swell(cv, nz, amp=3.0)
    sc = Scene(cv, nz, rng)
    blast_door(sc, CX, CY + 8, rng, jammed=(v == 1))
    if v == 0:
        barrel(sc, CX + 44, CY - 20, s=1.8, color='#3e5a3a', toxic=True)
        barrel(sc, CX + 52, CY - 14, s=1.8, color='#6a3a2a')
    else:
        pole(sc, CX - 44, CY - 26, h=18, cross=False, color='#8a8a90')
        def lamp():
            cv.fill(('ellipse', CX - 44 + 0, CY - 26 - 18 * FZ, 1.4, 1.4), C('#ff3a2a'))
            cv.fill(('ellipse', CX - 44, CY - 26 - 18 * FZ, 4, 4), C('#ff3a2a'), 0.3, mode='glow')
        sc.add(CY - 25, lamp)
    scrub_scatter(sc, rng, nz, tufts=40, bushes=3, avoid=[(CX, CY + 8, 40)], avoid_r=16)
    rim(cv)
    return finish(sc)


def vent_shaft(v, seed):
    cv, rng, nz = new(seed)
    scrub_base(cv, nz, rng, strokes=1100)
    # subsided earth ringed round the drop
    cv.ground_clip = True
    cv.fill(('poly', blob_pts(CX, CY + 4, 52, 34, nz, 0.2, phase=5)), C('#5a4630'), 0.6)
    for k in range(8):
        a = rng.uniform(0, math.tau)
        crack(cv, CX + math.cos(a) * 40, CY + 4 + math.sin(a) * 26, rng.uniform(10, 20), rng, nz, color=C('#3a2a1c'), w=0.6, alpha=0.7)
    grate(sc_dummy(cv, nz, rng), cv, CX, CY + 4, 26, rng, open_panel=(v == 1))
    swell(cv, nz, amp=2.5)
    sc = Scene(cv, nz, rng)
    # torn ducting stubs
    for (x, y, a) in [(CX - 38, CY - 10, 0.3), (CX + 36, CY + 20, -0.5)]:
        pts = [(x, y), (x + math.cos(a) * 16, y - 6 + math.sin(a) * 6)]
        sc.add(y, (lambda pts=pts: shade_tube(cv, pts, [4.5, 4.2], (C('#b0a898'), C('#8a8478'), C('#5a5650'), C('#34322e')))))
    if v == 1:
        def rope():
            cv.fill(('line', [(CX - 30, CY - 14), (CX - 14, CY - 4), (CX - 6, CY + 6)], 0.9), INK)
            cv.fill(('line', [(CX - 30, CY - 14), (CX - 14, CY - 4), (CX - 6, CY + 6)], 0.5), C('#c8a870'))
        sc.add(CY + 10, rope)
    scrub_scatter(sc, rng, nz, tufts=40, bushes=3, avoid=[(CX, CY + 4, 44)], avoid_r=16)
    rim(cv)
    return finish(sc)


class sc_dummy:
    def __init__(self, cv, nz, rng):
        self.cv, self.noise, self.rng = cv, nz, rng


CONCRETE = '#4a4640'


def tunnel_floor(v, seed):
    cv, rng, nz = new(seed)
    flat_ground(cv, nz, CONCRETE, var=0.12, tint2='#3a3a36', tint_amt=0.5, gradient=0.22)
    stroke_texture(cv, rng, nz, [('#5a564e', 3), ('#2a2826', 2), ('#6a665e', 0.8)], n=900, length=(2, 5), alpha=0.45, spread=1.4)
    cv.ground_clip = True
    # slab joints
    for k in range(-4, 5):
        x = CX + k * 30 + rng.uniform(-2, 2)
        cv.fill(('line', [(x, HEX_TOP - 4), (x + 8, HEX_BOT + 4)], 0.8), C('#1e1c1a'), 0.8)
    for k in range(-3, 4):
        y = CY + k * 26 + rng.uniform(-2, 2)
        cv.fill(('line', [(CX - R, y), (CX + R, y + 4)], 0.8), C('#1e1c1a'), 0.8)
    for i in range(6):
        x, y = random_in_hex(rng, 12)
        crack(cv, x, y, rng.uniform(8, 20), rng, nz, color=C('#161412'), w=0.6, alpha=0.8)
    # seep stain
    for i in range(3):
        x, y = random_in_hex(rng, 20)
        cv.fill(('poly', blob_pts(x, y, rng.uniform(10, 20), rng.uniform(6, 12), nz, 0.4, phase=x)), C('#2a3a34'), 0.35)
    if v == 1:
        pud = blob_pts(CX - 10, CY + 10, 40, 20, nz, 0.3, phase=7)
        cv.fill(('poly', pud), C('#1a2a2c'))
        cv.fill(('poly', blob_pts(CX - 18, CY + 4, 22, 8, nz, 0.3, phase=9)), C('#3a5a5a'), 0.5, clip=('poly', pud))
        for k in range(8):
            x = CX - 30 + rng.uniform(0, 50)
            y = CY + rng.uniform(0, 20)
            cv.fill(('line', [(x - 2, y), (x + 2, y)], 0.4), C('#9ab8b8'), 0.6, clip=('poly', pud))
    swell(cv, nz, amp=2.0)
    sc = Scene(cv, nz, rng)
    # cable tray + conduit along the back edge
    def tray():
        y0 = HEX_TOP + 16
        pts = [(CX - R * 0.45, y0), (CX + R * 0.45, y0 - 2)]
        shade_tube(cv, pts, [2.6, 2.6], (C('#9a8a70'), C('#6a5e4c'), C('#44403a'), C('#2a2826')))
        pts2 = [(CX - R * 0.5, y0 + 6), (CX + R * 0.5, y0 + 4)]
        cv.fill(('line', pts2, 3.2), INK)
        cv.fill(('line', pts2, 2.0), C('#5a5048'))
        for k in range(8):
            x = CX - R * 0.45 + k * R * 0.13
            cv.fill(('line', [(x, y0 + 4.5), (x, y0 + 7.5)], 0.6), INK)
        if v == 3:
            for k in range(5):
                x = CX - 30 + k * 14
                hang = [(x, y0 + 7), (x + rng.uniform(-3, 3), y0 + 14), (x + rng.uniform(-4, 4), y0 + 22)]
                cv.fill(('line', hang, 0.9), INK)
                cv.fill(('line', hang, 0.5), C('#c87a3a'))
        # a dead cage lamp
        cv.fill(('ellipse', CX + 50, y0 - 4, 2.4, 2.4), INK)
        cv.fill(('ellipse', CX + 50, y0 - 4, 1.6, 1.6), C('#5a5a50'))
    sc.add(HEX_TOP + 20, tray)
    if v == 2:
        for (x, y, yaw) in [(CX + 10, CY + 20, 0.4), (CX + 30, CY + 30, -0.3), (CX - 30, CY + 36, 0.9)]:
            faces, top, base, rp = box_geom(x, y, 12, 9, 8, yaw)
            sc.shadow(shadow_of(base, 8))
            sc.add(y, (lambda faces=faces, top=top: draw_faces(cv, [(q, Mat('#6a5a40').pick(n)) for q, n, _ in faces] + [(top, C('#8a7a58'))])))
        barrel(sc, CX - 44, CY - 6, s=1.8, color='#3e5a3a', toxic=True)
    if v == 3:
        for k in range(3):
            x, y = random_in_hex(rng, 20)
            sc.add(y, (lambda x=x, y=y: glowing_eyes(cv, x, y, 0.9, color='#ff5a3a')))
    rim(cv)
    return finish(sc, shadows=0.3)


def tunnel_collapsed(v, seed):
    cv, rng, nz = new(seed)
    flat_ground(cv, nz, '#2e2a26', var=0.14, tint2='#3a342e', tint_amt=0.5, gradient=0.2)
    stroke_texture(cv, rng, nz, [('#4a443c', 3), ('#1a1816', 2)], n=700, length=(2, 5), alpha=0.5, spread=1.4)
    X, Y = grid_ground()
    H = 30 * (0.4 + 0.6 * ridged(nz, X, Y, 0.045, sharp=1.8)) * rim_falloff(X, Y, inner=22)
    relief(cv, H, light=RELIEF_LIGHT, bands=(0.80, 0.55, 0.30), mult=(1.1, 0.9, 0.62, 0.42), strata=0.2, strata_step=5)
    sc = Scene(cv, nz, rng)
    # buckled ceiling plates jammed into the spill
    for k in range(3):
        x, y = random_in_hex(rng, 24)
        with sc.at(x, y) as Yl:
            def plate(x=x, Yl=Yl, a=rng.uniform(-0.8, 0.8)):
                p = [(x - 16, Yl), (x + 14, Yl - 6), (x + 12 + a * 6, Yl - 16), (x - 18 + a * 6, Yl - 10)]
                cv.fill(('poly', p), INK, grow=1.0)
                cv.fill(('poly', p), C('#6a6660'))
                for kk in range(3):
                    cv.fill(('line', [quad_pt(p, 0.25 * (kk + 1), 0), quad_pt(p, 0.25 * (kk + 1), 1)], 0.5), INK, 0.5)
            sc.add(Yl, plate)
    for k in range(10):
        x, y = random_in_hex(rng, 8)
        with sc.at(x, y) as Yl:
            rock(sc, x, Yl, rng.uniform(4, 9), color='#5a544c', rng=rng)
    if v == 1:
        def conduit():
            pts = [(CX - 60, CY - 10), (CX - 10, CY - 26), (CX + 50, CY - 20)]
            cv.fill(('line', pts, 3.4), INK)
            cv.fill(('line', pts, 2.0), C('#7a5a3a'))
        sc.add(CY - 20, conduit)
    rim(cv)
    return finish(sc, shadows=0.3)
