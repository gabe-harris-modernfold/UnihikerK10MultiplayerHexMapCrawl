# K10 SNES-style cartridge case

![preview](preview.png)

A two-part, snap-together case for the UNIHIKER K10, shaped like an NTSC Super
Nintendo Game Pak: 104 × 88 mm, with the K10 in the middle.

- The screen is the label.
- The wide wings have SNES grip grooves on the front and back. The grooves wrap
  around the edges and run up both sides to a plain bezel band.
- The back has a label panel, a logo stadium and lock slots.
- The live gold edge connector sits 1.5 mm inside a mouth at the bottom, between
  two solid feet, like an old cart.
- 80s / 90s details, all engraved as 45° V-grooves or cut straight through:
  - a Game Boy-style strip over the screen reading `BAND_TEXT`;
  - slanted speaker slots on the front left wing (real vents) and a hex badge on the right;
  - ▼ insert arrows over the connector on the front and back;
  - fake security screws on the back feet, and `TAGLINE` under the logo;
  - lock notches at the four bottom corners.

| File | What |
|---|---|
| `K10_SNES_Cart_Front.stl` | Bezel and screen window, with 10 snap tabs. Exported face down. |
| `K10_SNES_Cart_Back.stl` | Floor, wings, ports and the A/B push rods. Exported back face down. |
| `k10_snes_case.py` | Parametric source. `python k10_snes_case.py --check` rebuilds both STLs and runs the fit checks. Needs `manifold3d trimesh numpy matplotlib`. |

Outside size: 104.0 × 87.8 × 16.7 mm. `SIDE_WALL` sets the width; each wing is 25.95 mm.

## Printing

- No supports. Print both parts in the orientation they are saved in.
- PETG works best because the tabs, panels and rods flex. PLA works too.
- 0.2 mm layers, at least 3 walls, so the 0.8–1.0 mm flexures print solid.
- 15–20 % infill is plenty for the solid wings.
- Use light-grey filament for the SNES look.

## Assembly

1. Screen facing up, lay the K10 into the back shell. The gold fingers go into
   the mouth at the bottom.
2. Press the front shell straight down until all ten tabs click:
   - two on each side;
   - four along the top;
   - two on the feet at the bottom.
3. To open it, put a fingernail in the gap under each tab and pull the tab
   outward.

## What you can reach

| Feature | Where |
|---|---|
| USB-C | top end; the opening is 12.6 × 6.8 mm, so a normal cable's plug body fits all the way in |
| A / B | flush panels on the right side. Each panel drives a printed push rod, about 23 mm long, through the wing to its switch. A thin neck lets the panel flex while the rod slides straight. |
| RST | the round-tipped paddle at the top corner of the back; press the tip. A 3 mm post under it reaches the switch |
| microSD | a 26 mm channel through the right wing, flared at the side. Push the card in, or push it to eject, with something thin and flat such as a plastic ruler. Tilt the case to slide an ejected card out |
| Edge connector | recessed 1.5 mm inside the 13 × 55 mm mouth. It's protected, but a standard micro:bit edge socket won't fit into it |
| Gravity P0 / P1, I2C | closed; set `GRAVITY_PORTS = True` to open notches in the side walls |
| Speaker | a chamber in the left wing beside the speaker's side port, vented through the front grille and slots in the back |
| Mics, light sensor, temp/humidity | holes above the screen |
| Camera | covered (the app doesn't use it) |

## Fit: where the numbers come from, and what to check

The XY positions come from DFRobot's straight-on product photos, measured by
pixel against the board's 51.6 × 83 mm outline. The measurements are good to
about ±0.3 mm.

The LCD's black metal frame is easy to miss in the photos: it reaches 2.6 mm
past the glass toward the fingers. The first print's front half landed on it.
The frame is now part of the dummy board that `--check` tests against.

The Z heights come from the reference case in `docs/`: 6.0 mm of room behind
the PCB and 5.7 mm in front of it. `--check` places a dummy K10 in the case and
confirms:

- The board doesn't touch either shell, with a display stack anywhere from
  4.0 mm to 5.6 mm thick. The push rods stop 0.4 mm short of the plungers.
- Pushing in a USB-C plug moves the board 0.5 mm before a rib catches the
  ESP32 module's metal can. The display never takes that load.
- Sideways and toward USB-C, the walls stop the board within 0.35 mm.

If a print needs tuning, these are the parameters to change:

- `FRONT_CLEAR` (5.7): the gap in front of the PCB. If the screen sits deep
  behind the bezel, lower it to your display's height plus 0.3 mm.
- `RST_TOP_Z` (−2.0): the RST post stops 0.5 mm short of this. If RST needs a
  long press, make it more negative.
- `BTN_TIP_X`: how far the A/B plungers stick out. The rod tips sit 0.4 mm off
  them at rest.
- `TONGUE_GAP` (1.0): how far the A/B panels can travel before they bottom out.
- `SIDE_WALL`: the width.
- `EMBLEM_TEXT`, `BAND_TEXT`, `TAGLINE`: the engraved words. Set any of them to `""` to leave it off.

The reference STLs in `docs/` were only used for Z. Their XY doesn't match
the board in the photos:

- The screen window sits about 7 mm too far toward the USB-C end.
- The A-button hole misses the button by about 4 mm.
- The sensor holes don't line up with the mics or the light sensor.
