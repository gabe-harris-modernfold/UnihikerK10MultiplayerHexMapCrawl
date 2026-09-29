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
ports) meet on a flat parting plane through the middle of the PCB. Ten
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
SPEAKER = (0.1, 10.8, 17.1, 31.7)          # the big black box on the back; sound hole on the side pointing away from the board
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
ESP_PAD_Z = -3.1 - 0.25   # top of the pad under the ESP32 can: 0.25 mm off it
FLOOR = 1.8
FRONT_CLEAR = 5.7      # PCB front face to the underside of the bezel
BEZEL = 1.6
CORNER_CUT = 0.0       # 45 deg cut on the two top corners (0: plain rounded corners, like the real shell)
R_CORNER = 2.5         # every silhouette corner is rounded this much. Not 2.0 or 3.0: with the top
                       # chamfers those put an arc's end exactly on a chamfer edge and break the STL
CH = 1.0               # perimeter chamfer, front and back faces
CH_TOP_FRONT, CH_TOP_BACK = 1.5, 3.0
PLATEAU_X = (-8.0, 59.6)   # the SNES centre panel on both faces. Its edges are stepped grooves: both faces
                           # print on the bed, so a real raised step would need supports
RIBS = 7                   # slats on each wing, like the real shell
CLAMP_GAP = 0.03       # per side, between the PCB and the gold-finger-end clamps
Y_CLAMP = 9.9          # top of the back shell's finger-end wall
Y_CLAMP_FRONT = LCD_MODULE[1] - 0.7   # front clamp: bare PCB only, 0.7 short of the LCD frame
STOP_GAP = 0.45        # stop rib to the ESP32 can: takes USB-C plug-in force off the display
WINDOW_MARGIN = 0.4
MIC_HOLE_R = 0.8       # the two mic holes above the screen, 1.6 mm across
LABEL_CUT = 0.0        # 45 deg cut on the front label outline's top corners (0: rounded rectangle)
LINE_DEPTH, LINE_W = 1.0, 1.2   # the thin line grooves (screen border, pills): 1 mm deep, walls ~59 deg --
                                # steeper than 45, so still self-supporting face down
SLAT_D, SLAT_W = 2.0, 2.6       # slat grooves on the wings, deep like the real shell's (walls ~57 deg). On the
                                # back they run out over the edge at full depth into the side grooves
STEP_D = 2.0                    # the centre panel's stepped edges
LABEL_RECESS_D = 1.1            # back label recess edge (held to 1.1 by the RST paddle's slot beside it)
LOWER_D, DIVIDER_D = 1.3, 0.8   # three-panel recess: its outline, and the dividers (1 mm left over the mouth)
BAND_TEXT = "UNIHIKER ESP 32"       # in the nameplate pill above the screen ("" = empty pill)
EMBLEM_TEXT = "MADE IN WASTELAND"   # in the matching pill on the back ("" = empty pill)
FRONT_PILL = (50.6, 7.8)            # w h, groove centre line: as big as the space above the screen allows
                                    # (~0.5 mm clear of the label border, the window chamfer and the sensor holes)
BACK_PILL = (64.0, 9.0)             # w h: as big as the space between the two recesses allows
FRONT_PILL_Y, BACK_PILL_Y = 74.8, 39.0
PILL_MARGIN = 0.5                   # text to the inner edge of the pill's groove
GRAVITY_PORTS = False  # True opens the side walls at the P0 / P1 / I2C expansion connectors
SD_POCKET = 3.0        # room past the right inner wall for the end of an installed microSD (no opening)
SD_CARD_OUT = 2.0      # how far an installed card is taken to stick out past the PCB edge (--check)

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
RIB_Y = tuple(round(Y_SHROUD + (YO1 - Y_SHROUD) * k / RIBS, 3) for k in range(1, RIBS))       # grooves between slats
SLAT_C = tuple(round(Y_SHROUD + (YO1 - Y_SHROUD) * (k + 0.5) / RIBS, 3) for k in range(RIBS))  # slat centres

E = 0.01   # overlap so coplanar faces never survive a boolean
BIG = 5.0

# snap tabs
TAB_T, TAB_SLIT, TAB_BACKSLIT = 1.0, 0.5, 0.4
TAB_DOWN = 3.5          # how far a tab hangs below the parting plane
TAB_ROOT_Z = 6.3        # tabs flex from here: 9 mm free length, ~1.7% strain snapping over the hook
LIP, LIP_H = 1.0, 1.6   # hook depth and height (45 deg lead-in, flat catch). The first print's 0.45 mm hooks
                        # held too little once printing tolerances took their share; 1.0 bites 0.9 mm
TABS = [                # (wall, centre along the wall, width)
    # side tabs sit in the middle of a slat, clear of the slat grooves
    ("left", SLAT_C[1], 7.8), ("left", SLAT_C[5], 7.8),
    ("right", SLAT_C[2], 7.8), ("right", SLAT_C[3], 7.8),
    ("top", -14.0, 7.8), ("top", 8.0, 7.8), ("top", 43.5, 7.8), ("top", 65.6, 7.8),
    ("bottom", -13.5, 7.8), ("bottom", 65.1, 7.8),     # on the feet either side of the mouth
]

# back face layout (board X; the back is seen mirrored), after the real shell's back
BACK_LABEL = (-5.0, 45.0, 56.6, 83.2, 3.0)    # x0 y0 x1 y1 r: the big label recess, drawn as a recess edge
LOWER_PANEL = (-5.0, 56.6, 32.5)              # x0 x1 top y: the recess below it, open at the bottom edge,
LOWER_DIVIDERS = 2                            # split into three panels
RST_TONGUE = (44.8, 50.5, 63.5, 81.0)         # x0 x1 y0 y1, root at y0; tip kept clear of the label edge
RST_POST = (48.95, 78.0, 1.5)       # x y r: 3 mm post, as far toward the actuator as the paddle allows
BTN_NUB = 3.0                       # A / B press posts, 3 x 3 mm
SIDE_BEZEL = 1.5                    # plain band the side grooves stop short of, along the front edge
SIDE_GROOVE_D = 1.2                 # side grooves, carrying the slat grooves up the sides
SIDE_GROOVE_DY = 0.25               # nudged off the slat grooves' Y, so the two apex lines never cross
# speaker labyrinth in the left wing, the sound's only way out (see labyrinth())
LAB_X0, LAB_Y = -22.7, (11.0, 31.0)       # outer end (1.25 mm clear of the left tab's hook pocket); Y span, alongside the speaker
LAB_FLOOR = Z_FLOOR_IN + 1.2              # floor of stages 2 and 3, raised so the deep grooves under them keep 1 mm
BAFFLES_X, BAFFLE_T = (-7.8, -15.95), 1.2   # two baffles -> three stages of ~950 mm^3 each
PASSAGE = 5.0                             # opening at one end of each baffle, filled with a fine comb:
COMB_GAP, COMB_FIN, COMB_L = 0.5, 1.0, 3.0    # slit width, fin width, slit length along the flow
OUTLET_RIB, OUTLET_X = 1, (-22.2, -17.1)  # outlets: a 1 mm slot in this slat groove on the back, one in its side groove
DAM = (17.7, 18.9, -4.0)                  # x0 x1 z1: wall on the floor along the speaker's inner side
# cooling: line vents in the back over the ESP32 module's can, inside the label recess
ESP_VENT_Y = (61.0, 63.5, 66.0, 68.5, 71.0, 73.5)
ESP_VENT_X = ((2.0, 8.6), (10.6, 17.0))    # two columns, so the pad under the can stays one comb
SD_Z0 = -2.0                        # floor of the pocket for the installed microSD
ROD_T, ROD_CLR = 2.0, 0.35          # A / B push rods: thickness, and clearance in their channels
NECK_T, TONGUE_GAP = 0.8, 1.0       # rod-to-tongue flexure neck; room behind the tongue = its travel stop
SCREWS = ((-13.5, 5.0), (65.1, 5.0), 2.0)   # fake security screws in the bottom slats on the back, and their radius


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
    """Cartridge silhouette: 45 deg cut top corners (none when cut is 0), all corners rounded by r."""
    if cut <= 0:
        return rect_cs(x0, y0, x1, y1, r)
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


def vgroove_loop(cs, d, z_face, into=+1, hw=None):
    """V-groove d deep and 2*hw wide (45 deg when hw is None) along the edge of
    convex cs, cut into the face at z_face (into=+1 cuts toward +Z). Walls at 45
    deg or steeper are self-supporting on the bed."""
    o = (d if hw is None else hw) * (d + 1) / d      # half-width 1 mm outside the face
    def frustum(sign):
        wide = grow(cs, sign * o)
        return Manifold.batch_hull([slab(wide, z_face - into * 1 - E, z_face - into * 1 + E),
                                    slab(cs, z_face + into * d - E, z_face + into * d + E)])
    return frustum(+1) - frustum(-1)


def vgroove_line(x0, x1, y, d, z_face, hw=None):
    """Straight V-groove along X, d deep and 2*hw wide (45 deg when hw is None),
    cut up into a face at z_face."""
    o = (d if hw is None else hw) * (d + 1) / d
    return Manifold.hull_points([(x, yy, zz) for x in (x0, x1) for yy, zz in
                                 ((y - o, z_face - 1), (y + o, z_face - 1), (y, z_face + d))])


def vgroove_seg(p0, p1, d, z_face, into=+1, hw=None):
    """V-groove along the XY segment p0 -> p1, d deep and 2*hw wide (45 deg when hw
    is None), cut into the face at z_face."""
    (x0, y0), (x1, y1) = p0, p1
    ln = math.hypot(x1 - x0, y1 - y0)
    nx, ny = -(y1 - y0) / ln, (x1 - x0) / ln
    o = (d if hw is None else hw) * (d + 1) / d
    pts = []
    for px, py in (p0, p1):
        pts += [(px + k * o * nx, py + k * o * ny, z_face - into) for k in (-1, 1)]
        pts.append((px, py, z_face + into * d))
    return Manifold.hull_points(pts)


def step_line(x, y0, y1, d, z_face, into, out):
    """Groove along Y that reads as a raised panel's edge: a sheer wall at x on the
    panel side, and a 45 deg slope rising back to the face on the wing side
    (out = -1 or +1). The 0.05 mm flat at the bottom keeps the wall and slope
    from meeting in a knife edge."""
    zo, zf = z_face - into, z_face + into * (d - 0.05)       # 1 mm outside the face; the groove's floor
    return Manifold.hull_points([(xx, y, zz) for y in (y0, y1) for xx, zz in
                                 ((x, zo), (x, zf), (x + out * 0.05, zf), (x + out * (d + 1), zo))])


def recess_loop(cs, d, z_face, into=+1):
    """Groove round convex cs that reads as the edge of a recess: a sheer wall on
    cs, and a 45 deg slope inside it rising back to the face."""
    zo, zf = z_face - into, z_face + into * (d - 0.05)
    wall = slab(cs, min(zo, zf), max(zo, zf))
    slope = Manifold.batch_hull([slab(grow(cs, -(d + 1)), zo - E, zo + E), slab(grow(cs, -0.05), zf - E, zf + E)])
    return wall - slope


def split_span(x0, x1, gaps):
    """[x0, x1] minus the (a, b) gaps, as a list of spans."""
    spans = [(x0, x1)]
    for a, b in gaps:
        spans = [piece for s0, s1 in spans
                 for piece in ((s0, min(s1, a)), (max(s0, b), s1)) if piece[1] - piece[0] > 0.5]
    return spans


def pill_cs(pill, cx, cy):
    """The nameplate pill (w, h), as the centre line of its groove."""
    w, h = pill
    return stadium_cs(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)


def pill_text(s, pill, cx, cy, mirror=False):
    """s as big as it fits inside the pill (w, h): PILL_MARGIN clear of the groove,
    corners included where the words run into the round ends."""
    w, ph = pill
    a = ph / 2 - LINE_W / 2 - PILL_MARGIN            # inner half-height
    run = (w - ph) / 2                               # half-length of the straight sides
    x0, y0, x1, y1 = text_cs(s, 1.0, 0, 0, 1e6).bounds()
    aspect = (x1 - x0) / (y1 - y0)
    lo, hi = 0.0, 2 * a
    for _ in range(40):
        h = (lo + hi) / 2
        dx = max(0.0, h * aspect / 2 - run)
        lo, hi = (h, hi) if dx * dx + h * h / 4 <= a * a else (lo, h)
    return text_cs(s, lo, cx, cy, 1e6, mirror)


def outline(inset=0.0):
    cs = cart_cs(XO0, Y_SHROUD, XO1, YO1, CORNER_CUT, R_CORNER)
    return grow(cs, -inset) if inset else cs


def side_groove_ys(side):
    """Y of each slat groove that carries on up a side face. Skips any that would
    clip a tab slit or a button window."""
    d, keep_off = SIDE_GROOVE_D, 0.3
    bad = []
    for wall, c, w in TABS:
        if wall == side:
            for sc in (c - w / 2 - TAB_SLIT / 2, c + w / 2 + TAB_SLIT / 2):
                bad.append((sc - TAB_SLIT / 2 - d - keep_off, sc + TAB_SLIT / 2 + d + keep_off))
    if side == "right":
        for yc in (BTN_B_Y, BTN_A_Y):
            bad.append((yc - 1.7 - d - keep_off, yc + 8.4 + d + keep_off))
        if GRAVITY_PORTS:
            bad.append((I2C_Y - 5.5 - d - keep_off, I2C_Y + 5.5 + d + keep_off))
    elif GRAVITY_PORTS:
        bad += [(yc - 4.5 - d - keep_off, yc + 4.5 + d + keep_off) for yc in (P0_Y, P1_Y)]
    return [y for y in RIB_Y if not any(a < y + SIDE_GROOVE_DY < b for a, b in bad)]


def side_grooves():
    """Slat grooves up both side faces, from the back edge (where the back slat
    grooves run out into them) to a plain bezel band along the front edge."""
    d, z1 = SIDE_GROOVE_D, Z_FRONT - CH - SIDE_BEZEL
    grooves = []
    for side, xf, k in (("left", XO0, 1), ("right", XO1, -1)):
        for y in side_groove_ys(side):
            y += SIDE_GROOVE_DY
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


def face_line(x0, x1, y, d, z_face, into, hw=None):
    """Straight V-groove along X on the back (into=+1) or front (into=-1) face."""
    g = vgroove_line(x0, x1, y, d, into * z_face, hw)
    return g if into > 0 else g.mirror((0, 0, 1))


def wing_x(side):
    """X span of the slat grooves on a wing: from the flat face's edge to 1 mm short
    of the centre panel's stepped edge."""
    return (FACE_X[0], PLATEAU_X[0] - STEP_D - 1.0) if side == "left" else (PLATEAU_X[1] + STEP_D + 1.0, FACE_X[1])


def plateau_edges(z_face, into):
    """The centre panel's two stepped edges, full height: they stop short of the
    bottom chamfer and run out over the top one. On the back they break where
    the A/B push rods come through the face."""
    y0, y1 = Y_SHROUD + CH + 1.0, YO1 + 1
    gaps = [(yc - 2.0, yc + 2.0) for yc in (BTN_B_Y, BTN_A_Y)] if into > 0 else []
    cuts = [step_line(PLATEAU_X[0], y0, y1, STEP_D, z_face, into, -1)]
    return cuts + [step_line(PLATEAU_X[1], a, b, STEP_D, z_face, into, +1) for a, b in split_span(y0, y1, gaps)]


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

    # face detail, grooves only (this face prints on the bed): the SNES centre panel's
    # stepped edges, slat grooves on both wings, the label border round the screen,
    # and the nameplate pill
    d, hw = LINE_DEPTH, LINE_W / 2
    cuts = plateau_edges(Z_FRONT, -1)
    cuts.append(vgroove_loop(cart_cs(LABEL_X[0], 8.3, LABEL_X[1], 83.6, LABEL_CUT, 2.0), d, Z_FRONT, -1, hw))
    for y in RIB_Y:
        for side in ("left", "right"):
            cuts.append(face_line(*wing_x(side), y, SLAT_D, Z_FRONT, -1, SLAT_W / 2))
    cuts.append(vgroove_loop(pill_cs(FRONT_PILL, XC, FRONT_PILL_Y), d, Z_FRONT, -1, hw))    # nameplate pill above the screen
    if BAND_TEXT:
        cuts.append(slab(pill_text(BAND_TEXT, FRONT_PILL, XC, FRONT_PILL_Y), Z_FRONT - 0.4, Z_FRONT + 1))
    shell = shell - union(cuts)

    # screen window with a 45 deg chamfer on the outside
    x0, y0, x1, y1 = (LCD_LIT[0] - WINDOW_MARGIN, LCD_LIT[1] - WINDOW_MARGIN,
                      LCD_LIT[2] + WINDOW_MARGIN, LCD_LIT[3] + WINDOW_MARGIN)
    shell = shell - box(x0, y0, Z_BEZEL_IN - 1, x1, y1, Z_FRONT + 1)
    c = 0.8
    shell = shell - Manifold.batch_hull([box(x0, y0, Z_FRONT - c, x1, y1, Z_FRONT - c + E),
                                         box(x0 - c - 1, y0 - c - 1, Z_FRONT + 1, x1 + c + 1, y1 + c + 1, Z_FRONT + 1 + E)])

    # sensor ports through the forehead block
    for (x, y), r in ((MIC1, MIC_HOLE_R), (MIC2, MIC_HOLE_R), (LIGHT, 1.2), (AHT20, 0.9)):
        shell = shell - cyl_z(x, y, r, Z_FOREHEAD - 1, Z_FRONT + 1)

    # USB-C: top of the plug overmold opening (the rest is in the back shell)
    zc = -USBC_H / 2
    shell = shell - box(USBC_X - 6.3, YI1 - 1, Z_SPLIT - 1, USBC_X + 6.3, YO1 + 1, zc + 3.4)

    for _, add, cut, _ in tabs_placed():
        shell = (shell - cut) + add
    return shell - side_grooves()          # after the tabs, so the tabs get ribbed too


# ── speaker labyrinth ────────────────────────────────────────────────────────
def labyrinth():
    """Pocket in the left wing that the speaker's sound has to snake through.

    The speaker is a sealed box on the back of the board, against its left edge,
    and nothing in the case opens straight onto it. Its sound enters the first of
    three stages through the wing wall beside it, gets past each baffle only
    through a comb of fine slits at alternate ends, and leaves the last stage by a
    slot in a back slat groove and one in the side groove beside it, neither of
    which faces the speaker or the first stage. The stages and slits form an acoustic low-pass that takes the top off
    the amplifier's hiss (and, less so, off the sound you want). Printed slits
    damp far less than felt would, so PASSAGE is the knob: narrower is duller
    with less hiss, wider is brighter. The front shell's flat underside closes
    the pocket, so the tabs must all be clicked for it to seal."""
    y0, y1 = LAB_Y
    a_x0 = BAFFLES_X[0] - BAFFLE_T / 2 + 0.8      # stage 1 keeps the full depth: it takes the speaker's sound
    cut = box(LAB_X0, y0, LAB_FLOOR, XI0 + E, y1, Z_SPLIT + 1) + box(a_x0, y0, Z_FLOOR_IN, XI0 + E, y1, Z_SPLIT + 1)
    n = round((PASSAGE + COMB_FIN) / (COMB_GAP + COMB_FIN))       # slits per comb
    fin = (PASSAGE - n * COMB_GAP) / (n - 1)
    walls = []
    for k, xb in enumerate(BAFFLES_X):
        p0 = y1 - PASSAGE if k % 2 == 0 else y0       # comb at the USB-C end first, then alternate ends
        s0, s1 = (y0 - 1, p0) if k % 2 == 0 else (p0 + PASSAGE, y1 + 1)
        walls.append(box(xb - BAFFLE_T / 2, s0, Z_FLOOR_IN - 1, xb + BAFFLE_T / 2, s1, Z_SPLIT + 2))
        for i in range(n - 1):                        # comb fins, a slit either side of each
            fy = p0 + COMB_GAP * (i + 1) + fin * i
            walls.append(box(xb - COMB_L / 2, fy, Z_FLOOR_IN - 1, xb + COMB_L / 2, fy + fin, Z_SPLIT + 2))
    return cut - union(walls)


# ── back shell ───────────────────────────────────────────────────────────────
def back_shell():
    shell = body().trim_by_plane((0, 0, -1), -Z_SPLIT)

    cav = box(XI0, Y_SHROUD - BIG, Z_FLOOR_IN, XI1, YI1, Z_SPLIT + BIG)
    keep = union([
        box(XI0 - E, Y_BOT, Z_FLOOR_IN - E, XI1 + E, Y_CLAMP, -2.5),              # finger-end wall
        box(0.3, Y_BOT, -2.5 - E, 16.2, 7.3, -CLAMP_GAP),                        # clamp ribs, clear of the LEDs
        box(35.5, Y_BOT, -2.5 - E, 51.3, 7.3, -CLAMP_GAP),
        box(3.0, 59.25, Z_FLOOR_IN - E, 16.0, 75.25, ESP_PAD_Z),                 # under the ESP32 shield
        box(2.0, 78.3, Z_FLOOR_IN - E, 17.0, 82.6, -1.0),                        # under the module's antenna end
        box(3.0, 57.0, Z_FLOOR_IN - E, 16.0, ESP_SHIELD[1] - STOP_GAP, -2.0),    # stop rib below the can
        box(DAM[0], Y_CLAMP - E, Z_FLOOR_IN - E, DAM[1], SPEAKER[3] - 0.2, DAM[2]),   # speaker dam
    ])
    shell = shell - (cav - keep)
    shell = shell - box(XI0 - SHROUD_MARGIN, Y_SHROUD - 1, Z_FLOOR_IN, XI1 + SHROUD_MARGIN, Y_BOT, Z_SPLIT + 1)  # shroud
    shell = shell - box(21.0, 55.5, Z_FLOOR_IN - 0.6, 31.5, 74.5, Z_FLOOR_IN + E)   # camera headroom (no opening)
    shell = shell - labyrinth()             # the speaker's only way out
    oy = RIB_Y[OUTLET_RIB] + SIDE_GROOVE_DY  # one of its two outlets: a slot in that side groove
    shell = shell - box(XO0 - 1, oy - 0.4, LAB_FLOOR + 0.5, LAB_X0 + E, oy + 0.4, Z_SPLIT - 0.5)

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
    # microSD: no opening, just room in the right wall for the end of an installed card
    shell = shell - box(XI1 - 1, SD_Y[0] + 0.3, SD_Z0, XI1 + SD_POCKET, SD_Y[1] - 0.3, z_top)

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

    # back face, after the real shell's back: the centre panel's stepped edges, slat
    # grooves on the wings (running round onto the sides), a big label recess with a
    # three-panel recess below it, the pill between them, and two screws. This face
    # prints on the bed, so it is all grooves and through-cuts -- never a flat-floored recess.
    d, hw = LINE_DEPTH, LINE_W / 2
    cuts = plateau_edges(Z_BACK, +1)
    wrap_l, wrap_r = set(side_groove_ys("left")), set(side_groove_ys("right"))
    outlet_y = RIB_Y[OUTLET_RIB]
    sd, sw = SLAT_D, SLAT_W / 2
    for y in RIB_Y:
        # where a side groove carries on, the slat groove runs out over the edge at full
        # depth: its apex leaves through the side face, clear of the chamfer's edges
        cuts.append(vgroove_line(XO0 - 1 if y in wrap_l else FACE_X[0], wing_x("left")[1], y, sd, Z_BACK, sw))
        if y == outlet_y:                 # the labyrinth's other outlet, through the floor of the groove
            cuts.append(slab(rect_cs(OUTLET_X[0], y - 0.5, OUTLET_X[1], y + 0.5, 0), Z_BACK - 1, LAB_FLOOR + 1))
        if any(abs(y - yc) < ROD_T / 2 + ROD_CLR + sw + 0.3 for yc in (BTN_B_Y, BTN_A_Y)):
            continue                      # an A/B push rod comes through the face here
        in_btn = any(yc - 1.1 - 0.6 - 1.0 < y < yc + 8.4 + 1.0 for yc in (BTN_B_Y, BTN_A_Y))
        r1 = XO1 + 1 if y in wrap_r else (XO1 - 0.3 - 0.8 - TONGUE_GAP - 1.0 if in_btn else FACE_X[1])
        cuts.append(vgroove_line(wing_x("right")[0], r1, y, sd, Z_BACK, sw))
    lx0, ly0, lx1, ly1, lr = BACK_LABEL
    rods = union([box(lx1 - 2.0, yc - 2.0, Z_BACK - 2, lx1 + 2.0, yc + 2.0, Z_BACK + 2) for yc in (BTN_B_Y, BTN_A_Y)])
    cuts.append(recess_loop(rect_cs(lx0, ly0, lx1, ly1, lr), LABEL_RECESS_D, Z_BACK) - rods)   # label recess, open where the rods pass
    px0, px1, ptop = LOWER_PANEL      # the recess below, open at the bottom: 1.3 deep, so its apex runs out through
    cuts.append(vgroove_loop(rect_cs(px0, Y_SHROUD - 4.0, px1, ptop, lr), LOWER_D, Z_BACK, hw=0.75))   # the bottom face, clear of the chamfer
    for k in range(1, LOWER_DIVIDERS + 1):        # dividers, shallower, running up into its top groove
        x = px0 + (px1 - px0) * k / (LOWER_DIVIDERS + 1)
        cuts.append(vgroove_seg((x, Y_SHROUD - 3.0), (x, ptop), DIVIDER_D, Z_BACK, hw=0.55))
    (s1x, s1y), (s2x, s2y), sr = SCREWS
    for sx, sy in ((s1x, s1y), (s2x, s2y)):      # fake security screws: head ring and a cross
        cuts.append(vgroove_loop(CrossSection.circle(sr, 64).translate((sx, sy)), 0.7, Z_BACK))
        a = sr * 0.5
        cuts.append(vgroove_seg((sx - a, sy - a), (sx + a, sy + a), 0.6, Z_BACK))
        cuts.append(vgroove_seg((sx - a, sy + a), (sx + a, sy - a), 0.6, Z_BACK))
    cuts.append(vgroove_loop(pill_cs(BACK_PILL, XC, BACK_PILL_Y), d, Z_BACK, hw=hw))   # nameplate pill, wider than the front's
    for y in ESP_VENT_Y:              # vents over the ESP32 can, through the floor and the pad under it
        for x0s, x1s in ESP_VENT_X:   # (the pad is left as ribs between them, still backing the board)
            cuts.append(slab(stadium_cs(x0s, y - 0.5, x1s, y + 0.5), Z_BACK - 1, ESP_PAD_Z + 1))
    if EMBLEM_TEXT:                                                              # engraved, reads right from behind
        cuts.append(slab(pill_text(EMBLEM_TEXT, BACK_PILL, XC, BACK_PILL_Y, mirror=True), Z_BACK - 1, Z_BACK + 0.4))
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
        box(PCB_W + SD_CARD_OUT - 15.0, SD_Y[0] + 1.7, -1.4, PCB_W + SD_CARD_OUT, SD_Y[1] - 1.7, -0.6),   # installed card
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
    lab = labyrinth()
    n = round((PASSAGE + COMB_FIN) / (COMB_GAP + COMB_FIN))
    print(f"  speaker labyrinth: {(lab ^ box(LAB_X0, LAB_Y[0], Z_FLOOR_IN, XI0, LAB_Y[1], Z_SPLIT)).volume():.0f} mm^3 "
          f"in 3 stages, combs of {n} x {COMB_GAP:.2f} mm slits, outlets in the back and side grooves at Y {RIB_Y[OUTLET_RIB]}")
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
