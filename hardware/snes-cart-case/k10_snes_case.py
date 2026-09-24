#!/usr/bin/env python3
"""SNES-style snap-together cartridge case for the DFRobot UNIHIKER K10.

    python k10_snes_case.py            # writes the two printable STLs next to this file
    python k10_snes_case.py --check    # also runs the fit checks against a dummy K10

Needs: pip install manifold3d trimesh numpy matplotlib

Board frame (every number below, mm)
    X  front-left PCB edge -> right edge            0 .. 51.6
    Y  gold-finger edge    -> USB-C edge            0 .. 83.0
    Z  PCB back face (0)   -> front / screen side   PCB front face at 1.6

Where the numbers come from
    * XY positions of the screen, sensors, buttons and connectors were measured
      off DFRobot's straight-on product photos (front: lit screen + dimension
      shot, back: DFR0992_Back_03). The B-button plunger landed within 0.15 mm
      of the reference case's opening, so the photo scale is trustworthy.
    * Z heights are taken from the reference case (docs/UnihikerK10Case*.stl):
      6.0 mm of room behind the PCB, 5.7 mm in front of it. Photos can't give Z.
    * The reference case is NOT used for XY: its screen window sits ~7 mm too
      far toward the USB-C end, its A-button hole misses the button by ~4 mm and
      its sensor holes don't line up with the mics / light sensor.

The case: front shell (bezel + screen window) and back shell (floor, walls,
ports) meet on a flat parting plane through the middle of the PCB. Six
cantilever tabs on the front shell hang down into recesses in the back
shell's outer wall and hook under a ledge. The PCB is clamped at the gold-
finger end between the two shells and exits through the bottom face, so the
edge connector stays exposed like a cartridge's contacts.

Both shells print without supports: the front shell face down, the back
shell back face down (the STLs are written in those poses).
"""
import argparse
import math
import os

import numpy as np
from manifold3d import CrossSection, FillRule, JoinType, Manifold, OpType

HERE = os.path.dirname(os.path.abspath(__file__))

# ── K10 board (board frame) ──────────────────────────────────────────────────
PCB_W, PCB_L, PCB_T = 51.6, 83.0, 1.6

LCD_LIT = (4.28, 10.80, 47.86, 68.63)      # lit pixel area x0 y0 x1 y1
LCD_MODULE = (0.20, 8.00, 50.95, 77.92)    # black metal frame -- reaches 2.6 mm past the glass toward the fingers
LCD_T_CHECK = 4.0                          # display thickness used by --check

MIC1 = (26.1, 81.9)                        # top-port MEMS mic sound ports
MIC2 = (49.5, 81.9)
LIGHT = (34.9, 81.05)                      # ambient light sensor window
AHT20 = (42.4, 81.0)                       # temp / humidity sensor

USBC_X, USBC_W, USBC_H = 25.8, 8.94, 3.26  # receptacle on the back, mouth at Y=83
RST_TOP_Z = -2.0                           # 3x4 mm tactile on the back, top-right corner, actuator at (50.05, 78.0)
BTN_A_Y, BTN_B_Y = 70.1, 57.1              # side-push plungers on the right edge
BTN_Z = -1.75                              # plunger centre height
BTN_TIP_X = PCB_W + 0.75                   # plungers stick 0.75 mm past the PCB edge
SD_Y = (10.9, 25.3)                        # microSD socket, card enters from the right edge
P0_Y, P1_Y = 49.2, 37.1                    # Gravity 3-pin PH2.0, left edge, side entry
I2C_Y = 44.3                               # Gravity 4-pin PH2.0, right edge, side entry
SPEAKER = (0.1, 10.8, 17.1, 31.7)
LEDS_X = (17.4, 34.3)                      # three RGB LEDs on the back, Y 7.7..10.4
ESP_MODULE = (0.2, 56.6, 19.5, 83.0)       # ESP32-S3-WROOM-1 on the back, 0.8 mm PCB
ESP_SHIELD = (1.7, 58.23, 18.0, 75.96)     # its can, 3.1 mm tall overall

# ── Case parameters ──────────────────────────────────────────────────────────
GAP_SIDE = 0.25        # PCB edge to side wall
GAP_TOP = 0.30         # PCB USB-C edge to top wall
WALL = 3.0             # top wall (USB-C end)
SIDE_WALL = 25.95      # side wings: 51.6 board + 2 x 0.25 gap + 2 x 25.95 = 104.0 mm wide
Y_BOT = 6.3            # inner end of the connector mouth; the PCB is clamped just above it
Y_SHROUD = -1.5        # open end of the thin-walled shroud: the fingers sit 1.5 mm inside it
SHROUD_MARGIN = 1.5    # connector mouth = the board cavity plus this each side; the wings are its feet
Z_SPLIT = 0.8          # parting plane, through the middle of the PCB
BACK_CLEAR = 6.0       # PCB back face to the inside of the back floor
FLOOR = 1.8
FRONT_CLEAR = 5.7      # PCB front face to the underside of the bezel
BEZEL = 1.6
CORNER_CUT = 5.0       # 45 deg cut on the two top corners, like the SNES shell
R_CORNER = 2.0         # every silhouette corner is rounded this much
CH = 1.0               # perimeter chamfer, front and back faces
CH_TOP_FRONT, CH_TOP_BACK = 1.5, 3.0
CLAMP_GAP = 0.03       # per side, between the PCB and the gold-finger-end clamps
Y_CLAMP = 9.9          # top of the back shell's finger-end wall
Y_CLAMP_FRONT = LCD_MODULE[1] - 0.7   # front clamp: bare PCB only, 0.7 short of the LCD frame
STOP_GAP = 0.45        # stop rib to the ESP32 can: takes USB-C plug-in force off the display
WINDOW_MARGIN = 0.4
LABEL_DEPTH = 0.5      # V-groove outlining the front "label" around the screen
DETAIL_DEPTH = 0.5     # V-grooves on the back: grip wings, label and logo outlines
EMBLEM_TEXT = "WASTELAND"   # engraved in the stadium on the back ("" = plain stadium)
BAND_TEXT = "WASTELAND  HEX CRAWL"   # Game Boy-style strip above the screen ("" = lines only)
TAGLINE = "MADE IN THE WASTELAND"    # small print under the logo on the back ("" = none)
GRAVITY_PORTS = False  # True opens the side walls at the P0 / P1 / I2C expansion connectors
SD_FLARE = 3.0         # 45 deg funnel on the outside of the SD slot, so a fingernail reaches the card

# derived
XI0, XI1 = -GAP_SIDE, PCB_W + GAP_SIDE
YI1 = PCB_L + GAP_TOP
XO0, XO1 = XI0 - SIDE_WALL, XI1 + SIDE_WALL
YO1 = YI1 + WALL
XC = PCB_W / 2
LABEL_X = (XI0 - 1.0, XI1 + 1.0)  # front label outline around the screen
FACE_X = (XO0 + CH + 1.0, XO1 - CH - 1.0)   # flat front/back face, inside the edge chamfers
Z_FLOOR_IN = -BACK_CLEAR
Z_BACK = Z_FLOOR_IN - FLOOR
Z_BEZEL_IN = PCB_T + FRONT_CLEAR
Z_FRONT = Z_BEZEL_IN + BEZEL
Y_FOREHEAD = LCD_MODULE[3] + 0.8  # sensor block starts just above the LCD frame
Z_FOREHEAD = PCB_T + 1.6          # clears the ~1.1 mm sensors on the top strip

E = 0.01   # overlap so coplanar faces never survive a boolean
BIG = 5.0

# snap tabs
TAB_T, TAB_SLIT, TAB_BACKSLIT = 1.0, 0.5, 0.4
TAB_DOWN = 3.0          # how far a tab hangs below the parting plane
TAB_ROOT_Z = 6.3        # tabs flex from here (free length ~8 mm, <1% strain)
LIP, LIP_H = 0.45, 0.9  # hook depth and height (45 deg lead-in, flat catch)
TABS = [                # (wall, centre along the wall, width)
    # side tabs sit on the grip-groove grid: centre = 23.5 + 2.5k with width 7
    # puts both outline slits midway between grooves, so the tabs blend in
    ("left", 13.5, 7.0), ("left", 68.5, 7.0),
    ("right", 33.5, 7.0), ("right", 46.0, 7.0),
    ("top", -14.0, 7.0), ("top", 8.0, 7.0), ("top", 43.5, 7.0), ("top", 65.6, 7.0),
    ("bottom", -13.5, 7.0), ("bottom", 65.1, 7.0),     # on the feet either side of the mouth
]

# back face layout (board X; the back is seen mirrored)
WING_L, WING_R = (0.3, 6.5), (PCB_W - 6.5, PCB_W - 0.3)   # grooved grip wings
GROOVE_W, GROOVE_PITCH, GROOVE_Y = 1.0, 2.5, (18.5, 81.5)
RST_TONGUE = (44.8, 50.5, 64.0, 82.0)                      # x0 x1 y0 y1, root at y0
RST_POST = (48.95, 78.0, 1.5)       # x y r: 3 mm post, as far toward the actuator as the paddle allows
BTN_NUB = 3.0                       # A / B press posts, 3 x 3 mm
SIDE_GROOVE_Y = (3.5, 78.5)         # grip grooves up the side faces, same pitch/phase as the wings
SIDE_BEZEL = 1.5                    # plain band the side grooves stop short of, along the front edge
SIDE_GROOVE_D = 0.45                # a hair shallower than the wings, so the two apex lines never meet on the chamfer
FACE_LINES_Y = (6.0, 3.5)           # two full-width lines across the shroud end, front and back
WING_GROOVE_Y0 = 8.5                # face grooves on the wide wings start here (grid 3.5 + 2.5k)
SPEAKER_CHAMBER = (-23.5, 11.0, 31.0)   # x0 y0 y1: pocket in the left wing off the speaker's side port,
                                        # vented to the front grille and out the back
BACK_VENT_X0 = -5.5                 # back-face speaker slots start here
SD_Z0 = -2.0                        # floor of the SD channel through the right wing
ROD_T, ROD_CLR = 2.0, 0.35          # A / B push rods: thickness, and clearance in their channels
NECK_T, TONGUE_GAP = 0.8, 1.0       # rod-to-tongue flexure neck; room behind the tongue = its travel stop
# 80s / 90s detail
GRILLE = dict(c=(-13.0, 21.0), n=6, angle=60.0, w=1.5, l=12.0, pitch=3.0)   # front speaker slots, Game Boy style
HEX_BADGE = (65.1, 21.0, 7.0)       # x y circumradius: two-ring hex on the front right wing
ARROW = (25.8, 1.2, 6.6, 6.4)       # x, tip y, top y, width: insert arrow over the connector, both faces
SCREWS = ((-13.5, 4.3), (65.1, 4.3), 2.6)   # fake security screws on the back feet, and their radius
LOCK_NOTCH = (3.0, 2.6)             # bottom corner notches: depth into the side, top Y


# ── helpers ──────────────────────────────────────────────────────────────────
def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube((x1 - x0, y1 - y0, z1 - z0)).translate((x0, y0, z0))


def union(parts):
    parts = [p for p in parts if not p.is_empty()]
    return Manifold.batch_boolean(parts, OpType.Add) if parts else Manifold()


def grow(cs, d):
    """Offset with round joins. simplify() drops the near-duplicate points Clipper
    leaves behind; extruded, those become zero-width slivers that break the STL."""
    return cs.offset(d, JoinType.Round, 2.0, 64).simplify(1e-4)


def rounded(cs, r):
    """Round every convex corner of a cross-section by r."""
    return grow(grow(cs, -r), r)


def cart_cs(x0, y0, x1, y1, cut, r):
    """Cartridge silhouette: 45 deg cut top corners, all corners rounded by r."""
    return rounded(CrossSection([[(x0, y0), (x1, y0), (x1, y1 - cut), (x1 - cut, y1),
                                  (x0 + cut, y1), (x0, y1 - cut)]]), r)


def rect_cs(x0, y0, x1, y1, r):
    return rounded(CrossSection([[(x0, y0), (x1, y0), (x1, y1), (x0, y1)]]), r) if r > 0 else \
        CrossSection([[(x0, y0), (x1, y0), (x1, y1), (x0, y1)]])


def stadium_cs(x0, y0, x1, y1):
    return rect_cs(x0, y0, x1, y1, min(x1 - x0, y1 - y0) / 2 - 1e-3)


def slab(cs, z0, z1):
    return Manifold.extrude(cs, z1 - z0).translate((0, 0, z0))


def cyl_z(x, y, r, z0, z1, seg=40):
    return Manifold.cylinder(z1 - z0, r, r, seg).translate((x, y, z0))


def prism_xz(pts_xz, y0, y1):
    """Convex prism: XZ profile swept along Y."""
    return Manifold.hull_points([(x, y, z) for x, z in pts_xz for y in (y0, y1)])


def text_cs(s, height, cx, cy, max_w, mirror=False):
    """Bold text outline centred on (cx, cy), cap height `height`."""
    from matplotlib.font_manager import FontProperties
    from matplotlib.textpath import TextPath
    tp = TextPath((0, 0), s, size=1.0, prop=FontProperties(family="DejaVu Sans", weight="bold"))
    cs = CrossSection([p for p in tp.to_polygons() if len(p) >= 3], FillRule.EvenOdd)
    x0, y0, x1, y1 = cs.bounds()
    k = min(height / (y1 - y0), max_w / (x1 - x0))
    cs = cs.translate((-(x0 + x1) / 2, -(y0 + y1) / 2)).scale((k, k))
    if mirror:
        cs = cs.mirror((1, 0))
    return cs.translate((cx, cy))


def vgroove_loop(cs, d, z_face, into=+1):
    """45 deg V-groove, 2d wide and d deep, along the edge of convex cs, cut into
    the face at z_face (into=+1 cuts toward +Z). Self-supporting on the bed."""
    def frustum(sign):
        wide = grow(cs, sign * (d + 1))
        return Manifold.batch_hull([slab(wide, z_face - into * 1 - E, z_face - into * 1 + E),
                                    slab(cs, z_face + into * d - E, z_face + into * d + E)])
    return frustum(+1) - frustum(-1)


def vgroove_line(x0, x1, y, d, z_face):
    """Straight 45 deg V-groove along X, cut up into a face at z_face."""
    return Manifold.hull_points([(x, yy, zz) for x in (x0, x1) for yy, zz in
                                 ((y - d - 1, z_face - 1), (y + d + 1, z_face - 1), (y, z_face + d))])


def vgroove_seg(p0, p1, d, z_face, into=+1):
    """45 deg V-groove along the XY segment p0 -> p1, cut into the face at z_face."""
    (x0, y0), (x1, y1) = p0, p1
    ln = math.hypot(x1 - x0, y1 - y0)
    nx, ny = -(y1 - y0) / ln, (x1 - x0) / ln
    pts = []
    for px, py in (p0, p1):
        pts += [(px + k * (d + 1) * nx, py + k * (d + 1) * ny, z_face - into) for k in (-1, 1)]
        pts.append((px, py, z_face + into * d))
    return Manifold.hull_points(pts)


def split_span(x0, x1, gaps):
    """[x0, x1] minus the (a, b) gaps, as a list of spans."""
    spans = [(x0, x1)]
    for a, b in gaps:
        spans = [piece for s0, s1 in spans
                 for piece in ((s0, min(s1, a)), (max(s0, b), s1)) if piece[1] - piece[0] > 0.5]
    return spans


def arrow_cs():
    x, tip, top, w = ARROW
    return CrossSection([[(x - w / 2, top), (x, tip), (x + w / 2, top)]])


def hex_cs(cx, cy, r):
    return CrossSection([[(cx + r * math.cos(math.radians(90 + 60 * k)), cy + r * math.sin(math.radians(90 + 60 * k)))
                          for k in range(6)]])


def outline(inset=0.0):
    cs = cart_cs(XO0, Y_SHROUD, XO1, YO1, CORNER_CUT, R_CORNER)
    return grow(cs, -inset) if inset else cs


def side_groove_ys(side):
    """Y of each grip groove running up a side face. Skips any that would clip
    a tab slit, the edge of the SD notch, or a button window."""
    d, keep_off = SIDE_GROOVE_D, 0.3
    bad = []
    for wall, c, w in TABS:
        if wall == side:
            for sc in (c - w / 2 - TAB_SLIT / 2, c + w / 2 + TAB_SLIT / 2):
                bad.append((sc - TAB_SLIT / 2 - d - keep_off, sc + TAB_SLIT / 2 + d + keep_off))
    if side == "right":
        for edge in (SD_Y[0] + 0.3 - SD_FLARE, SD_Y[1] - 0.3 + SD_FLARE):
            bad.append((edge - d - keep_off, edge + d + keep_off))
        for yc in (BTN_B_Y, BTN_A_Y):
            bad.append((yc - 1.7 - d - keep_off, yc + 8.4 + d + keep_off))
        if GRAVITY_PORTS:
            bad.append((I2C_Y - 5.5 - d - keep_off, I2C_Y + 5.5 + d + keep_off))
    elif GRAVITY_PORTS:
        bad += [(yc - 4.5 - d - keep_off, yc + 4.5 + d + keep_off) for yc in (P0_Y, P1_Y)]
    ys, y = [], SIDE_GROOVE_Y[0]
    while y <= SIDE_GROOVE_Y[1] + 1e-6:
        if not any(a < y < b for a, b in bad):
            ys.append(round(y, 3))
        y += GROOVE_PITCH
    return ys


def side_grooves():
    """Grip grooves up both side faces, from the back edge (where they meet the
    back-wing grooves) to a plain bezel band along the front edge."""
    d, z1 = SIDE_GROOVE_D, Z_FRONT - CH - SIDE_BEZEL
    grooves = []
    for side, xf, k in (("left", XO0, 1), ("right", XO1, -1)):
        for y in side_groove_ys(side):
            pts = [(xf - k, y - d - 1), (xf - k, y + d + 1), (xf + k * d, y)]
            grooves.append(Manifold.hull_points([(x, yy, z) for x, yy in pts for z in (Z_BACK - 1, z1)]))
    return union(grooves)


# ── outer body ───────────────────────────────────────────────────────────────
def body():
    b = Manifold.batch_hull([
        slab(outline(CH), Z_BACK, Z_BACK + E),
        slab(outline(0), Z_BACK + CH, Z_FRONT - CH),
        slab(outline(CH), Z_FRONT - E, Z_FRONT),
    ])
    s2 = math.sqrt(0.5)
    # bigger 45 deg chamfers along the top (USB-C) edge -- the grip slope on the back
    b = b.trim_by_plane((0, -s2, -s2), -s2 * ((YO1 - CH_TOP_FRONT) + Z_FRONT))
    b = b.trim_by_plane((0, -s2, s2), -s2 * ((YO1 - CH_TOP_BACK) - Z_BACK))
    nd, ny = LOCK_NOTCH                   # SNES-style lock notches at the four bottom corners
    for x0, x1 in ((XO0 - 1, XO0 + nd), (XO1 - nd, XO1 + 1)):
        b = b - box(x0, Y_SHROUD - 1, Z_BACK - 1, x1, ny, Z_FRONT + 1)
    return b


# ── snap tabs: built for the left wall, then mirrored / rotated ───────────────
def tab_parts(c, w):
    """(front_add, front_cut, back_cut) for a tab centred at Y=c on the left wall."""
    xo = XO0 + 0.05                 # tab outer face (hair inside the wall face)
    xi = xo + TAB_T                 # tab inner face
    zb = Z_SPLIT - TAB_DOWN         # tab tip
    y0, y1 = c - w / 2, c + w / 2
    tab = box(xo, y0, zb, xi, y1, Z_SPLIT + E)
    lip = prism_xz([(xi - E, zb), (xi + LIP, zb + LIP), (xi + LIP, zb + LIP_H), (xi - E, zb + LIP_H)], y0, y1)
    front_add = tab + lip
    front_cut = union([
        box(XO0 - E, y0 - TAB_SLIT, Z_SPLIT - E, xi + TAB_BACKSLIT, y0, TAB_ROOT_Z),
        box(XO0 - E, y1, Z_SPLIT - E, xi + TAB_BACKSLIT, y1 + TAB_SLIT, TAB_ROOT_Z),
        box(xi, y0 - E, Z_SPLIT - E, xi + TAB_BACKSLIT, y1 + E, TAB_ROOT_Z),
    ])
    ry0, ry1 = y0 - TAB_SLIT, y1 + TAB_SLIT
    back_cut = union([
        box(XO0 - BIG, ry0, zb - 1.0, xi + 0.1, ry1, Z_SPLIT + E),                 # recess + 1 mm pry gap
        box(xi + 0.1 - E, ry0, zb - 0.2, xi + LIP + 0.2, ry1, zb + LIP_H + 0.1),   # hook pocket
    ])
    return front_add, front_cut, back_cut


def place_tab(m, wall):
    if wall == "left":
        return m
    if wall == "right":
        return m.mirror((1, 0, 0)).translate((XO0 + XO1, 0, 0))
    if wall == "top":        # local -X (outward) -> +Y, local Y -> X
        return m.rotate((0, 0, -90)).translate((0, YO1 + XO0, 0))
    # bottom: local -X (outward) -> -Y, local Y -> -X (callers pass c = -x)
    return m.rotate((0, 0, 90)).translate((0, Y_SHROUD - XO0, 0))


def tabs_placed():
    """(wall, front_add, front_cut, back_cut) for every snap tab, in world position."""
    out = []
    for wall, cpos, w in TABS:
        parts = tab_parts(-cpos if wall == "bottom" else cpos, w)
        out.append((wall,) + tuple(place_tab(m, wall) for m in parts))
    return out


def face_line(x0, x1, y, d, z_face, into):
    """Straight V-groove along X on the back (into=+1) or front (into=-1) face."""
    g = vgroove_line(x0, x1, y, d, into * z_face)
    return g if into > 0 else g.mirror((0, 0, 1))


def wing_x(side):
    """X span of the plain wing on the front / back face, outside the board area."""
    return (FACE_X[0], LABEL_X[0] - 1.5) if side == "left" else (LABEL_X[1] + 1.5, FACE_X[1])


# ── front shell ──────────────────────────────────────────────────────────────
def front_shell():
    shell = body().trim_by_plane((0, 0, 1), Z_SPLIT)

    cav = box(XI0, Y_SHROUD - BIG, Z_SPLIT - BIG, XI1, YI1, Z_BEZEL_IN)
    keep = union([
        box(XI0 - E, Y_BOT, PCB_T + CLAMP_GAP, XI1 + E, Y_CLAMP_FRONT, Z_BEZEL_IN + E),        # finger-end clamp
        box(XI0 - E, Y_FOREHEAD, Z_FOREHEAD, XI1 + E, YI1 + E, Z_BEZEL_IN + E),                 # sensor block
        box(1.0, Y_FOREHEAD + 0.2, PCB_T + CLAMP_GAP, 19.5, 82.8, Z_FOREHEAD + E),              # presses free PCB top-left
    ])
    shell = shell - (cav - keep)
    # connector shroud: the mouth around the live gold fingers; the wings are its feet
    shell = shell - box(XI0 - SHROUD_MARGIN, Y_SHROUD - 1, Z_SPLIT - 1, XI1 + SHROUD_MARGIN, Y_BOT, Z_BEZEL_IN)

    # face detail, all 45 deg V-grooves (this face prints on the bed):
    # label border round the screen, SNES grip lines on both wings, two lines across the shroud end
    d = LABEL_DEPTH
    cuts = [vgroove_loop(cart_cs(LABEL_X[0], 8.3, LABEL_X[1], 83.6, 4.0, 2.0), d, Z_FRONT, into=-1)]
    g = GRILLE
    grille_y = (g["c"][1] - 10.5, g["c"][1] + 10.5)
    badge_y = (HEX_BADGE[1] - HEX_BADGE[2] - 1.0, HEX_BADGE[1] + HEX_BADGE[2] + 1.0)
    y = WING_GROOVE_Y0
    while y <= GROOVE_Y[1] + 1e-6:        # grip lines on both wings, clear of the grille and the badge
        if not grille_y[0] < y < grille_y[1]:
            cuts.append(face_line(*wing_x("left"), y, d, Z_FRONT, -1))
        if not badge_y[0] < y < badge_y[1]:
            cuts.append(face_line(*wing_x("right"), y, d, Z_FRONT, -1))
        y += GROOVE_PITCH
    ax, _, _, aw = ARROW
    for y in FACE_LINES_Y:                # two lines across the shroud end, broken for the arrow
        for x0s, x1s in split_span(*FACE_X, [(ax - aw / 2 - 2.0, ax + aw / 2 + 2.0)]):
            cuts.append(face_line(x0s, x1s, y, d, Z_FRONT, -1))
    cuts.append(vgroove_loop(arrow_cs(), d, Z_FRONT, into=-1))
    for r in (HEX_BADGE[2], HEX_BADGE[2] * 0.6):
        cuts.append(vgroove_loop(hex_cs(*HEX_BADGE[:2], r), d, Z_FRONT, into=-1))
    # Game Boy-style text strip above the screen
    for y in (71.3, 77.3):
        cuts.append(face_line(LCD_LIT[0], LCD_LIT[2], y, 0.45, Z_FRONT, -1))
    if BAND_TEXT:
        cuts.append(slab(text_cs(BAND_TEXT, 3.4, (LCD_LIT[0] + LCD_LIT[2]) / 2, 74.3, 38.0), Z_FRONT - 0.4, Z_FRONT + 1))
    shell = shell - union(cuts)

    # speaker grille: slanted slots through the left wing into the speaker chamber
    ang = math.radians(g["angle"])
    for k in range(g["n"]):
        off = (k - (g["n"] - 1) / 2) * g["pitch"]
        cx, cy = g["c"][0] - off * math.sin(ang), g["c"][1] + off * math.cos(ang)
        slot = stadium_cs(-g["l"] / 2, -g["w"] / 2, g["l"] / 2, g["w"] / 2).rotate(g["angle"]).translate((cx, cy))
        shell = shell - slab(slot, Z_SPLIT - 1, Z_FRONT + 1)

    # screen window with a 45 deg chamfer on the outside
    x0, y0, x1, y1 = (LCD_LIT[0] - WINDOW_MARGIN, LCD_LIT[1] - WINDOW_MARGIN,
                      LCD_LIT[2] + WINDOW_MARGIN, LCD_LIT[3] + WINDOW_MARGIN)
    shell = shell - box(x0, y0, Z_BEZEL_IN - 1, x1, y1, Z_FRONT + 1)
    c = 0.8
    shell = shell - Manifold.batch_hull([box(x0, y0, Z_FRONT - c, x1, y1, Z_FRONT - c + E),
                                         box(x0 - c - 1, y0 - c - 1, Z_FRONT + 1, x1 + c + 1, y1 + c + 1, Z_FRONT + 1 + E)])

    # sensor ports through the forehead block
    for (x, y), r in ((MIC1, 0.65), (MIC2, 0.65), (LIGHT, 1.2), (AHT20, 0.9)):
        shell = shell - cyl_z(x, y, r, Z_FOREHEAD - 1, Z_FRONT + 1)

    # USB-C: top of the plug overmold opening (the rest is in the back shell)
    zc = -USBC_H / 2
    shell = shell - box(USBC_X - 6.3, YI1 - 1, Z_SPLIT - 1, USBC_X + 6.3, YO1 + 1, zc + 3.4)

    for _, add, cut, _ in tabs_placed():
        shell = (shell - cut) + add
    return shell - side_grooves()          # after the tabs, so the tabs get ribbed too


# ── back shell ───────────────────────────────────────────────────────────────
def back_shell():
    shell = body().trim_by_plane((0, 0, -1), -Z_SPLIT)

    cav = box(XI0, Y_SHROUD - BIG, Z_FLOOR_IN, XI1, YI1, Z_SPLIT + BIG)
    keep = union([
        box(XI0 - E, Y_BOT, Z_FLOOR_IN - E, XI1 + E, Y_CLAMP, -2.5),              # finger-end wall
        box(0.3, Y_BOT, -2.5 - E, 16.2, 7.3, -CLAMP_GAP),                        # clamp ribs, clear of the LEDs
        box(35.5, Y_BOT, -2.5 - E, 51.3, 7.3, -CLAMP_GAP),
        box(3.0, 60.0, Z_FLOOR_IN - E, 16.0, 74.5, -3.1 - 0.25),                 # under the ESP32 shield
        box(2.0, 78.3, Z_FLOOR_IN - E, 17.0, 82.6, -1.0),                        # under the module's antenna end
        box(3.0, 57.0, Z_FLOOR_IN - E, 16.0, ESP_SHIELD[1] - STOP_GAP, -2.0),    # stop rib below the can
    ])
    shell = shell - (cav - keep)
    shell = shell - box(XI0 - SHROUD_MARGIN, Y_SHROUD - 1, Z_FLOOR_IN, XI1 + SHROUD_MARGIN, Y_BOT, Z_SPLIT + 1)  # shroud
    shell = shell - box(21.0, 55.5, Z_FLOOR_IN - 0.6, 31.5, 74.5, Z_FLOOR_IN + E)   # camera headroom (no opening)
    # speaker: its port faces the left edge, so give it a chamber in the wing (vented through the back)
    shell = shell - box(SPEAKER_CHAMBER[0], SPEAKER_CHAMBER[1], Z_FLOOR_IN, XI0 + E, SPEAKER_CHAMBER[2], Z_SPLIT + 1)

    # USB-C plug overmold opening, 12.6 x 6.8, bottom corners chamfered
    zc = -USBC_H / 2
    shell = shell - Manifold.batch_hull([
        box(USBC_X - 6.3, YI1 - 1, zc - 3.4 + 1.5, USBC_X + 6.3, YO1 + 1, Z_SPLIT + 1),
        box(USBC_X - 4.8, YI1 - 1, zc - 3.4, USBC_X + 4.8, YO1 + 1, Z_SPLIT + 1),
    ])

    # openings (notches down from the parting plane)
    z_top = Z_SPLIT + 1
    if GRAVITY_PORTS:
        shell = shell - box(XO0 - 1, P0_Y - 4.5, -5.8, XI0 + 1, P0_Y + 4.5, z_top)
        shell = shell - box(XO0 - 1, P1_Y - 4.5, -5.8, XI0 + 1, P1_Y + 4.5, z_top)
        shell = shell - box(XI1 - 1, I2C_Y - 5.5, -5.8, XO1 + 1, I2C_Y + 5.5, z_top)
    # microSD: a channel through the right wing to the socket, flared at the side face
    sd0, sd1, f = SD_Y[0] + 0.3, SD_Y[1] - 0.3, SD_FLARE
    shell = shell - box(XI1 - 1, sd0, SD_Z0, XO1 + 1, sd1, z_top)
    shell = shell - Manifold.batch_hull([box(XO1 - f, sd0, SD_Z0, XO1 - f + E, sd1, z_top),
                                         box(XO1 + 1, sd0 - f - 1, SD_Z0 - f - 1, XO1 + 1 + E, sd1 + f + 1, z_top)])

    # A / B: a flush flexing panel on the side face drives a printed push rod
    # through the wing to the switch plunger. A thin neck joins panel and rod,
    # so the panel can swing while the rod slides straight in its channel.
    outer = body()
    for yc in (BTN_B_Y, BTN_A_Y):
        tip, root = yc - 1.1, yc + 8.4
        xo = XO1 - 0.3                            # panel outer face: a hair under the side
        xt = xo - 0.8                             # panel inner face (0.8 mm tongue)
        xn = xt - TONGUE_GAP                      # far side of the gap behind the panel
        xp = BTN_TIP_X + 0.4                      # rod tip, 0.4 mm clear of the plunger at rest
        z1 = Z_SPLIT - 0.3
        shell = shell - box(xn, tip - 0.6, Z_BACK - 1, XO1 + 1, root, Z_SPLIT + 1)              # panel pocket
        shell = shell - box(XI1 - E, yc - ROD_T / 2 - ROD_CLR, Z_BACK - 1, xn + E,
                            yc + ROD_T / 2 + ROD_CLR, Z_SPLIT + 1)                             # rod channel
        panel = box(xt, tip, Z_BACK, xo, root + 0.6, z1)
        neck = box(xn - E, yc - NECK_T / 2, Z_BACK, xt + E, yc + NECK_T / 2, z1)
        rod = box(xp, yc - ROD_T / 2, Z_BACK, xn + E, yc + ROD_T / 2, z1)
        shell = shell + ((panel + neck + rod) ^ outer)

    # RST: round-tipped tongue in the floor, root toward the middle, post up to the switch
    tx0, tx1, ty0, ty1 = RST_TONGUE
    r_tip = (tx1 - tx0) / 2 - 0.05
    tongue = rounded(CrossSection([[(tx0, ty0 - 5), (tx1, ty0 - 5), (tx1, ty1), (tx0, ty1)]]), r_tip)
    past_root = rect_cs(tx0 - 5, ty0, tx1 + 5, ty1 + 5, 0)
    shell = shell - slab((grow(tongue, 0.7) - tongue) ^ past_root, Z_BACK - 1, Z_FLOOR_IN + E)
    shell = shell - slab(grow(tongue, 0.1) ^ past_root,
                         Z_BACK + 1.0, Z_FLOOR_IN + 1)                                   # thin the tongue to 1.0
    shell = shell + cyl_z(RST_POST[0], RST_POST[1], RST_POST[2], Z_BACK + 1.0 - E, RST_TOP_Z - 0.5, 64)

    # back face, SNES style: grooved grip wings, label, logo stadium, lock slots.
    # This face prints on the bed, so the details are 45 deg V-grooves or
    # through-cuts -- never a flat-floored recess.
    d = DETAIL_DEPTH
    cuts = []
    wrap_l, wrap_r = set(side_groove_ys("left")), set(side_groove_ys("right"))
    slot_x = [(sum(WING_L) / 2 - 1.5, sum(WING_L) / 2 + 1.5), (sum(WING_R) / 2 - 1.5, sum(WING_R) / 2 + 1.5)]
    y = WING_GROOVE_Y0
    while y <= GROOVE_Y[1] + 1e-6:
        yk, low = round(y, 3), y < GROOVE_Y[0]    # below the wings proper, stop short of the lock slots
        l0 = XO0 - 1 if yk in wrap_l else FACE_X[0]
        l1 = slot_x[0][0] - 1.0 if low else WING_L[1]
        cuts.append(vgroove_line(l0, l1, y, d, Z_BACK))
        if SPEAKER[1] + 0.3 < y - GROOVE_W / 2 and y + GROOVE_W / 2 < SPEAKER[3] - 0.2:   # speaker vents
            cuts.append(slab(rect_cs(BACK_VENT_X0, y - GROOVE_W / 2, min(l1, WING_L[1]) - 0.3,
                                     y + GROOVE_W / 2, 0), Z_BACK - 1, Z_FLOOR_IN + 1))
        near_rod = any(abs(y - yc) < ROD_T / 2 + ROD_CLR + d + 0.3 for yc in (BTN_B_Y, BTN_A_Y))
        if not near_rod:
            r0 = slot_x[1][1] + 1.0 if low else (WING_R[0] if y + GROOVE_W / 2 < ty0 - 1.2 else tx1 + 0.7 + 1.2)
            in_btn = any(yc - 1.1 - 0.6 - 1.0 < y < yc + 8.4 + 1.0 for yc in (BTN_B_Y, BTN_A_Y))
            r1 = XO1 + 1 if yk in wrap_r else (XO1 - 0.3 - 0.8 - TONGUE_GAP - 1.0 if in_btn else FACE_X[1])
            cuts.append(vgroove_line(r0, r1, y, d, Z_BACK))
        y += GROOVE_PITCH
    (s1x, s1y), (s2x, s2y), sr = SCREWS
    ax, _, _, aw = ARROW
    gaps = [(ax - aw / 2 - 2.0, ax + aw / 2 + 2.0), (s1x - sr - 1.0, s1x + sr + 1.0), (s2x - sr - 1.0, s2x + sr + 1.0)]
    for y in FACE_LINES_Y:                        # two lines across the shroud end, broken for arrow and screws
        for x0s, x1s in split_span(*FACE_X, gaps):
            cuts.append(vgroove_line(x0s, x1s, y, d, Z_BACK))
    cuts.append(vgroove_loop(arrow_cs(), d, Z_BACK))
    for sx, sy in ((s1x, s1y), (s2x, s2y)):      # fake security screws: head ring and a cross
        cuts.append(vgroove_loop(CrossSection.circle(sr, 64).translate((sx, sy)), 0.4, Z_BACK))
        a = sr * 0.5
        cuts.append(vgroove_seg((sx - a, sy - a), (sx + a, sy + a), 0.35, Z_BACK))
        cuts.append(vgroove_seg((sx - a, sy + a), (sx + a, sy - a), 0.35, Z_BACK))
    if TAGLINE:
        cuts.append(slab(text_cs(TAGLINE, 3.0, XC, 29.5, 30.0, mirror=True), Z_BACK - 1, Z_BACK + 0.4))
    for x0s, x1s in slot_x:                       # lock slots, through
        cuts.append(slab(stadium_cs(x0s, Y_CLAMP + 0.5, x1s, 17.5), Z_BACK - 1, Z_FLOOR_IN + 1))
    cuts.append(vgroove_loop(rect_cs(9.5, 47.0, PCB_W - 9.5, 79.5, 2.0), d, Z_BACK))   # label border
    cuts.append(vgroove_loop(stadium_cs(XC - 15.0, 35.0, XC + 15.0, 43.0), d, Z_BACK))  # logo stadium
    if EMBLEM_TEXT:                                                              # engraved, reads right from behind
        cuts.append(slab(text_cs(EMBLEM_TEXT, 4.2, XC, 39.0, 25.0, mirror=True), Z_BACK - 1, Z_BACK + 0.4))
    shell = shell - union(cuts)

    for _, _, _, cut in tabs_placed():
        shell = shell - cut
    return shell - side_grooves()


# ── dummy K10 for fit checks ─────────────────────────────────────────────────
def dummy_k10(lcd_t=LCD_T_CHECK):
    g = LCD_MODULE
    parts = [
        box(0, 0, 0, PCB_W, PCB_L, PCB_T),
        box(g[0], g[1], PCB_T, g[2], g[3], PCB_T + lcd_t),
        box(20.5, 79.5, PCB_T, 50.8, 82.5, PCB_T + 1.3),                          # top-strip sensors
        box(USBC_X - USBC_W / 2, 74.95, -USBC_H, USBC_X + USBC_W / 2, 83.2, 0),
        box(ESP_MODULE[0], ESP_MODULE[1], -0.8, ESP_MODULE[2], ESP_MODULE[3], 0),     # ESP32-S3 module
        box(ESP_SHIELD[0], ESP_SHIELD[1], -3.1, ESP_SHIELD[2], ESP_SHIELD[3], -0.8),
        box(48.6, 75.7, RST_TOP_Z, 51.6, 80.3, 0),
        box(36.9, SD_Y[0], -1.9, 51.5, SD_Y[1], 0),
        box(0, P0_Y - 5.2, -5.4, 7.8, P0_Y + 5.2, 0), box(0, P1_Y - 5.2, -5.4, 7.8, P1_Y + 5.2, 0),
        box(43.8, I2C_Y - 5.1, -5.4, PCB_W, I2C_Y + 5.1, 0),
        box(39.0, 28.2, -5.4, 46.4, 36.2, 0),                                     # BAT IN
        box(SPEAKER[0], SPEAKER[1], -4.5, SPEAKER[2], SPEAKER[3], 0),
        box(22.8, 57.5, -5.0, 29.8, 64.5, 0), cyl_z(26.3, 69.1, 1.8, -3.5, 0),   # camera, amber part
        box(35.2, 54.0, -1.6, 38.2, 56.6, 0),                                     # BOOT
        box(LEDS_X[0], 7.67, -1.0, LEDS_X[1], 10.44, 0),
    ]
    for yc in (BTN_A_Y, BTN_B_Y):
        parts += [box(47.0, yc - 1.6, -3.2, PCB_W, yc + 1.6, 0),
                  box(PCB_W - E, yc - 0.75, BTN_Z - 0.75, BTN_TIP_X, yc + 0.75, BTN_Z + 0.75)]
    return union(parts)


# ── output ───────────────────────────────────────────────────────────────────
def to_trimesh(m):
    import trimesh
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                           faces=np.asarray(mesh.tri_verts), process=False)


def print_pose_front(m):
    """Front face down on the bed."""
    m = m.rotate((0, 180, 0))
    b = m.bounding_box()
    return m.translate((-b[0], -b[1], -b[2]))


def print_pose_back(m):
    b = m.bounding_box()
    return m.translate((-b[0], -b[1], -b[2]))


def check(front, back):
    ok = True
    for t in (LCD_T_CHECK, FRONT_CLEAR - 0.1):
        k10 = dummy_k10(t)
        for name, part in (("front", front), ("back", back)):
            v = (part ^ k10).volume()
            ok &= v < 1e-3
            print(f"  K10 (display {t:.1f} mm) vs {name} shell: {v:.4f} mm^3  {'ok' if v < 1e-3 else 'INTERFERES'}")
    v = (front ^ back).volume()
    ok &= v < 1e-3
    print(f"  front vs back shell: {v:.4f} mm^3  {'ok' if v < 1e-3 else 'INTERFERES'}")

    # how far the board can slide before something stops it, and what stops it
    k10 = dummy_k10()
    for label, axis, sign in (("-Y (USB-C plug pushed in)", 1, -1), ("+Y (pushed into an edge socket)", 1, 1),
                              ("-X", 0, -1), ("+X", 0, 1)):
        for step in range(1, 41):
            dist = step * 0.05
            t = [0.0, 0.0, 0.0]
            t[axis] = sign * dist
            moved = k10.translate(tuple(t))
            hit = [(n, p ^ moved) for n, p in (("front", front), ("back", back))]
            hit = [(n, x) for n, x in hit if x.volume() > 1e-3]
            if hit:
                where = "; ".join(f"{n} shell near ({', '.join(f'{(a + b) / 2:.1f}' for a, b in zip(x.bounding_box()[:3], x.bounding_box()[3:]))})"
                                  for n, x in hit)
                print(f"  slide {label}: stopped after {dist:.2f} mm by the {where}")
                break
        else:
            print(f"  slide {label}: free for 2 mm")
    for name, part in (("front", front), ("back", back)):
        print(f"  {name}: {part.volume() / 1000:.2f} cm^3, {part.num_tri()} tris, genus {part.genus()}, "
              f"bbox {tuple(round(v, 2) for v in part.bounding_box())}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="run fit checks against a dummy K10")
    ap.add_argument("--out", default=HERE)
    args = ap.parse_args()

    front, back = front_shell(), back_shell()
    if args.check and not check(front, back):
        raise SystemExit("fit check failed")
    for name, part in (("K10_SNES_Cart_Front.stl", print_pose_front(front)),
                       ("K10_SNES_Cart_Back.stl", print_pose_back(back))):
        path = os.path.join(args.out, name)
        to_trimesh(part).export(path)
        if args.check:   # what a slicer sees: STL re-welded by vertex position
            import trimesh
            ok = trimesh.load(path).is_watertight
            print(f"  {name}: watertight after re-weld: {ok}")
            if not ok:
                raise SystemExit("exported STL is not watertight")
    print("wrote K10_SNES_Cart_Front.stl, K10_SNES_Cart_Back.stl ->", args.out)


if __name__ == "__main__":
    main()
