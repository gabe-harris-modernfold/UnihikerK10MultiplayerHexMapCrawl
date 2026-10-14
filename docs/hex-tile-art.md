# Hex tile art: clip, grade, place

This is how the hex terrain tiles are being redrawn:

- Every object is clipped from the user's own art.
- Each clip is graded into the palette of their painted tiles.
- Clips are placed sparsely on ground stitched together from their paintings.
- No tile art is drawn in code.

**Status (2026-09-23):**

- **Open Scrub:** 10 of its 43 tiles ship, one per variant slot, as cells 0–9
  of the Open Scrub sheet. The other 33 are waiting for alternates support;
  see [Integration](#integration).
- **Marsh:** 6 tiles ship as cells 0–5 of the Marsh sheet. These are whole
  painted hexes, reshaped and regraded; see [Marsh](#marsh).
- **Broken Urban (2026-09-28):** all 24 buildings of gi23, one per hex on
  Open Scrub ground, as 10 slots + 3 downtown pins with alternates; see
  [Broken Urban](#broken-urban).
- **Other terrains:** not started.
- **Delivery (2026-09-27):** every terrain is one sheet now, not one file per
  tile. Paint into `art/hex-sheets/hex<Name>.png`, then run
  `python scripts/hex_sheets.py build`. See [Integration](#integration).
  Wherever this doc says `hex<Name><N>.png`, read "cell N of that terrain's
  sheet".
- **Shelters and forage animals too (2026-09-27):**
  - Shelters: `art/hex-sheets/shelterBasic.png` and `shelterImproved.png`,
    224 px cells. The art is the user's AI re-render of the old shelters,
    kept in `art/hex-sheets/src/`.
  - Forage animals: `art/hex-sheets/forrageAnimal.png`, 80 px cells.
  - See `docs/dev-loop.md`, "Art assets".
- **Bunker Entrance (2026-10-12):** six props cut from the user's
  image-gen sheet and set on Open Scrub ground. See
  [Bunker Entrance](#bunker-entrance-2026-10-12).
- **Vent Shaft (2026-10-12):** six vent shafts cut from the user's second
  image-gen sheet the same way, set on Open Scrub ground. See
  [Vent Shaft](#vent-shaft-2026-10-12).
- **River Channel (2026-10-13):** 23 tiles from the user's image-gen
  sheets, as 8 slots with alternates. Until now River had no art and drew
  animated ripples. See [River Channel](#river-channel-2026-10-13).
- **Mountain (2026-10-14):** 26 tiles from the user's five image-gen
  sheets, as three slots with alternates: 12 bare peaks, 8 landmarks
  (rockslides and cairns) and 6 rare structures. The old painted tile is
  retired. Four cave mouths are held back for later. See
  [Mountain](#mountain-2026-10-14).
- **Rolling Hills (2026-10-14):** 17 tiles from the user's three image-gen
  sheets, as a common slot (12) and a rare slot (5), each with alternates.
  The old painted tile is retired. See [Rolling Hills](#rolling-hills-2026-10-14).
- **Tunnels (2026-09-27):** the bunker board is drawn from the user's
  underground sheets now: 17 overhead corridor pieces turned onto each cell's
  open sides, 18 angled rooms, and shaft and cave-in interiors. See
  [Tunnels](#tunnels).

## For AI coding agents (read first)

- **Never draw tile art in code.** Code-drawn tiles were rejected twice.
  Every pixel comes from the user's art: the sheets on `D:\` and their existing
  tiles in `data/img`. The procedural generator in `scripts/tilegen/` is
  rejected. Only its `tg_core.Noise` is still used, for the green-splash mask.
- **Stop after every preview.** Wait for an explicit go before building more.
  The first redraw was thrown out because it was built out without a check-in.
- **`D:\` also holds personal documents.** Open only the art sheets listed in
  [Source art](#source-art).
- **Grade every clip.** A clip pasted in its own colours looks stuck on. The
  one exception is telephone poles; see [Grade](#2-grade).
- **One feature per tile, mostly open ground.** Scrubland is sparse.
- **Fill the hex exactly: crop, stretch ×1.035, clip.** The user's old tiles
  don't fill their canvas, which is why the first attempt left gaps between
  hexes.
- **The scripts live in a temporary session folder.** See
  [Where the tools are](#where-the-tools-are).

## Rules from the user

| Rule | Note |
|---|---|
| Use only their art; clip and scale it | "you don't even need to make stuff up you can clip it from here D:\" |
| Grade clips to the terrain | "adjust the color palette on the clips instead of just pasting them" |
| At most one small feature per tile | "it's supposed to [be] sparse scrub lands" |
| Weight toward open ground | "20 scrub spots, weighted more on open empty terrain" |
| No gaps between hexes | "you haven't identified the correct hex tile size" |
| Splashes of the common green | the green of the in-game `hexOpenScrub` tiles |
| Tone the black ground cracks down by 90% | |
| Varied ruined telephone poles | |
| Cars +25%, planes +15% | now about 110–115 px and 150–180 px wide on the 256 px tile |

## Source art

`gi{N}` means `D:/generated-image (N).png`, and `gi0` is
`D:/generated-image.png`. The sheets are RGB: their "transparent" background is
a painted checkerboard.

| Sheet | Contents | Used for so far |
|---|---|---|
| gi0 | hex sheet: leaning pole, forest, water tower, house, pond, cracked ground | snapped leaning pole |
| gi3 | hex sheet: forest, boulders, ruined house, fallen poles with transformer | rocks, boulders, fallen pole pair |
| gi4, gi5 | hex sheets: fields, rail line, farm buildings, poles | three poles |
| gi7, gi13, gi14 | unlabelled terrain hex sheets | — |
| gi8, gi15, gi16 | labelled terrain hex sheets (OPEN SCRUB, ASH DUNES, …) | gi8's OPEN SCRUB hex: ground, dead tree, rocks, tufts |
| gi17 | props: rock outcrop, rusted sedan, skull, toxic pool, DINER sign, barrels, dead tree, bus stop, vending machine, mine, junk | sedan, dead oak |
| gi18–22, gi28 | buildings | — |
| gi23 | 24 small-town buildings (houses, shops, church, civic) | all 24: [Broken Urban](#broken-urban) |
| gi24, gi25 | cars, vans, trucks, buses | pickup `gi24_05`. Approved but unused: `gi24_06`, `gi25_01/04/08/09` |
| gi26 | crashed aircraft | fuselage `gi26_07`, Huey `gi26_12`, fighter `gi26_16` |
| gi27 | plants: bushes, grass, dead trees, stumps, logs, brush | all dead trees and bushes except the dead oak |
| gi29 | marsh pieces | — |
| `Six … hex tiles.png` | motorcycles, rocket bikes, military vehicles on sage hexes | — |
| dry-scrub hex with a cow skull (257×194, pasted into chat) | the user's painted scrub | main ground source, cow skull |

## 1. Clip

**Asset sheets with a checker background** (`clip_assets.py`):

- **Background:**
  - Light, neutral pixels (max − min < 18) connected to the sheet border.
  - Enclosed neutral regions whose light and dark squares follow the checker
    (square size and phase are fitted on the border; more than 86% must match).
  - Pale gaps under 500 px with a minimum channel above 185. They're too small
    to show the checker pattern. There is no hole filling.
- **Clean-up:** open 1 px, erode 1 px, feather σ 0.6.
- **Output:** each component of at least `min_area` px is saved as
  `assets/gi{N}_{kk}.png`, along with a `_catalog_gi{N}.png` contact sheet.
- **Run:** `python clip_assets.py "D:/generated-image (27).png" assets 1200`

**Props painted into a hex's ground** (`cut_v2.py`) cover the OPEN SCRUB hex's
dead tree and rocks and the gi3 boulders:

- Model that hex's ground with k-means, keeping clusters over 12% of the face.
- Object = pixels more than 40 away from every ground colour, or ink.
- Keep the largest components.
- `rgba()` removes the ground colour from the soft edges.

**Telephone poles** (`cut_pole.py`, with per-pole settings in `run_poles.py`):

- On these sheets the ground and its shadows are green (Lab a* < 0), and wood
  and metal are not.
- Object = not-green or dark ink, inside a box or polygon, minus the hex border
  lines (`cut_lines`).
- Keep what touches the `anchors` (the pole and its wires), plus loose wires of
  at least `keep_loose` px that sit well inside the region.
- Fill only holes under 60 px. Larger holes are ground seen between wires.
- Wires that run off the cut fade out over `fade` px.

**Leftover checker and dust** (`trees_bushes.clean()`):

- Remove neutral light pixels still inside a clip: chroma < 6 and L* > 62.
  The gi17 sheet's checker is darker, so use L* > 45 there.
- Also remove a 1 px fringe around them, and crumbs under 8 px.
- `dust=(L, chroma, frac)` also removes the pale grey dust painted around a
  prop's foot. It never matches the new ground.

## 2. Grade

Grading happens in Lab, in two steps:

1. **Transfer (Reinhard):** shift and scale each channel toward a target mean
   and standard deviation. The statistics are measured on the clip's body, and
   ink (L* < 22) is left out of the transfer so outlines stay black.
2. **Snap:** pull each pixel part of the way toward the nearest colour in a
   k-means palette.

| Grade | Target | Transfer / snap | Used for |
|---|---|---|---|
| `grade_light` | scrub palette (k=36 over the ground source) | 0.55 / 0.30 | cars, planes |
| `grade_wood` | wood of the dead tree in the OPEN SCRUB hex (`cuts2/os_1`): L* 47, a* 0, b* 17 | 0.70 / 0.30 | gi27 dead trees, stump, brush pile, branches, log |
| `grade_family` | the graded gi27 dead trees | 0.95 / 0.35 | the gi17 dead oak (painted orange and flat-lit) |
| `grade_leaf` | a darker shade of the in-game `hexOpenScrub` green: L* 46, a*/b* × 0.82 | 0.75 / 0.25 | bushes, tall grass |
| `grade_dry` | scrub palette | 0.55 / 0.30 | dry tuft |
| none | — | — | poles; rocks, tufts and skull cut from the scrub paintings themselves |

- **Why poles are ungraded:** grading washed the wood out into the ground. The
  poles are cut from the user's hex sheets, which are painted in the same
  style, so their own colours already fit.
- **Next terrain:** `grade.py`'s `Palette(families=[...])` builds a palette
  from any terrain's painted tiles in `data/img`, placeholders excluded.
- **Checking:** compare a strip of raw and graded clips on the ground colour
  (`treesbush_grade.png`).

## 3. Ground

The ground is built only from the user's two scrub paintings.

1. **Source** (`compose_scrub3.build_source`):
   - The top face of the dry-scrub hex, with the skull and rock boxes left out.
   - The OPEN SCRUB hex from gi8 at 0.65 scale, with its props, creek and label
     masked out.
   - A mirrored copy of both, so no one patch gets stamped in rows.
2. **Cracks toned down 90%** (`crack_soften.py`):
   - Ink = pixels more than 10 L* darker than a 5×5 grey closing.
   - Each stroke is classified:

     | Kind | Test | Kept? |
     |---|---|---|
     | speck | under 6 px | kept |
     | pebble | a closed ring under 16 px | kept |
     | crack | fill < 0.28, or ≤ 3 px tall and ≥ 6 px wide | toned |
     | tuft | anything else | kept |

   - Dense clumps inside a crack (7×7 density ≥ 0.36, at least 18 px) stay as
     tufts.
   - Cracks are inpainted (Telea) and blended 90% toward the result.
3. **Flavours:** `grassy`, `cracked` (dry) and `mixed` are regions of the
   source.
4. **Quilting** (`quilt.py`, Efros–Freeman):
   - Patch 36, overlap 12, 500 candidates.
   - The pick is random among candidates within 1.6× of the best error.
   - Seams follow the minimum-error path.
5. **Green splashes** (`compose_scrub20.green_splash`):
   - An fbm noise mask. Inside it, the ground is recoloured in Lab toward the
     mean and standard deviation of the `hexOpenScrub0–3` green: lightness 80%,
     hue fully.
   - The ground's own tufts carry through.
   - Amount per flavour: grassy 0.5, mixed 0.35, cracked 0.18.

## 4. Place a feature

- Put one feature per tile.
- The foot sits at `C + base` (C = 128). Pick `base` between 30 and 58 so the
  object's visual centre lands near the hex centre.
- Contact shadows are colour (58, 50, 30) at 30%, blurred 3 px.

| Feature | Size on the 256 px tile | Shadow |
|---|---|---|
| dry tuft | 48 px wide | ellipse |
| bushes, tall grass | 62–90 px wide | ellipse, 85% of the width |
| stump, brush pile, branches, log | 52–78 px wide | ellipse |
| stone, rocks, boulder, skull | 34–70 px wide | ellipse; none for the skull |
| dead trees | 100–112 px tall | ellipse at the trunk, 24–44 px |
| poles | 92–136 px tall | ellipse at the foot only, 16–20 px, because the wires make the clip wide; none for the fallen pair |
| cars | 110–115 px wide | ellipse |
| planes | 152–179 px wide | silhouette: alpha blurred 3 px, offset (3, 4), 34% (an ellipse under wings reads as a hole) |

## 5. Fit the hex

- **Canvas:** the tile is 256×256. The flat-top hex spans the full width and is
  256·√3/2 ≈ 221.7 px tall. The renderer draws each tile as a 2×HEX_SZ square.
- **Old tiles:** the user's old tiles don't fill their canvas. For example,
  `hexOpenScrub0`'s painted hex covers rows 6–95 of 97, with a soft fringe.
  Crop an old tile to its painted hex and stretch it to the grid hex before
  using it. Back it with its solid median colour.
- **Bleed:** the ground is quilted 1.035× larger (`BLEED`) plus 2 px, centred,
  then clipped to the hex grown by 1.035. The clip is anti-aliased at 4×.
- **Verify:**
  - The minimum alpha inside the grid hex must be 255.
  - A patch composited over magenta must show no magenta between hexes.

## 6. Variant slots

The board stores a hex's variant in 4 bits, and `pickVariant()` in
`hex-map.hpp` picks it rank-quadratically: variant i of n has weight (n−i)².
Scrub has 10 slots, because variant 10 is Jack's Chopper's POI (`0_10`). Their
shares are:

| Slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Share % | 26.0 | 21.0 | 16.6 | 12.7 | 9.4 | 6.5 | 4.2 | 2.3 | 1.0 | 0.3 |

There are more tiles than slots, so each slot holds several alternates:

- The client picks an alternate by a stable hash of the hex's map position:
  `(q*73856093) ^ (r*19349663)`, then a murmur-style finaliser so two
  alternates don't fall into a checkerboard, mod the slot's tile count
  (`altPick()` in engine.js and observer.js). Built 2026-09-28; see
  [Integration](#integration).
- A tile's frequency is its slot's share divided by the number of alternates
  in that slot.
- Rare things go in the high slots.

## 7. Preview, then stop

Send these three previews, then wait for the user:

- **Candidate sheet:** each new clip alone on a tile, labelled with its source
  clip (`treesbush_sheet.png`).
- **Slot sheet:** every tile in its slot, with its % (`scrub_mix_sheet.png`).
- **Map patch:** a 15×9 board drawn the way the game picks tiles
  (`scrub_mix_patch.png`). Say when a patch happens to show more of something
  than average.

For a tweak such as size or colour, send a before/after of just the tiles that
changed.

## Open Scrub

The approved set has 43 tiles:

| Kind | Share of scrub hexes |
|---|---|
| Open ground | 68% |
| Bushes | 10% |
| Dead wood | 9% |
| Poles | 6% |
| Rocks and skull | 5% |
| Planes | 1.5% |
| Cars | 0.5% |

The tile in the **Shipped** column is the one in `data/img` as
`hexOpenScrub<slot>.png`. It's also the first tile of that slot in
`scrub_mix.py`.

| Slot | Share | Shipped | Waiting for alternates |
|---|---|---|---|
| 0 | 26.0% | open, mixed | 3 open |
| 1 | 21.0% | open, grassy | 2 open |
| 2 | 16.6% | open, dry | 2 open |
| 3 | 12.7% | thorny scrub bush | 2 open, low bushes, tall grass clump, dry tuft |
| 4 | 9.4% | rocks in the grass | stone, short pole, leafy shrub, stump, brush pile |
| 5 | 6.5% | gnarled dead tree | dead tree over rocks, boulder, leaning pole, twisted dead tree, dead oak |
| 6 | 4.2% | snapped pole, leaning | cow skull, the same pole mirrored, dead snag, fallen branches, fighter |
| 7 | 2.3% | airliner fuselage | double-arm pole, pole broken with its top down, stump with its top beside it |
| 8 | 1.0% | rusted sedan | fallen poles and transformer, fallen log, Huey |
| 9 | 0.3% | pickup with a sapling in the bed | — |

Until the alternates ship, each slot shows its one tile everywhere. For
example, the thorny bush is on 1 in 8 scrub hexes.

The 8 ruined poles come from four sheets:

- gi0: the snapped, leaning pole with dangling wires, plus a mirrored copy.
- gi5: the double-crossarm pole tilted 13°, and a leaning pole with one wire.
- gi4: a short pole with dangling wires.
- gi3: the fallen crossed pair with its transformer.
- Two more are poles snapped in two, with the stump standing and the top lying
  beside it (from gi5 and gi0). `pole_variants.py` makes these and the tilt.

## Marsh

Marsh works differently from Open Scrub. Each tile is one of the user's whole
painted MARSH hexes, not ground plus a clip. The user said "i like them all
standarize the palette and get them shaped properly".

| Slot | Share | Source | What it shows |
|---|---|---|---|
| 0 | 39.6% | gi7 | braided blue-grey pools, cattails, cracked mud |
| 1 | 27.5% | old `hexMarsh0` | grey-green channel |
| 2 | 17.6% | gi14 | flat-top diorama: open water, reeds, shopping cart |
| 3 | 9.9% | gi8 (= gi15 = gi16, the same file) | dark pond, algae, planks; the "MARSH" label is painted out |
| 4 | 4.4% | old `hexMarsh1` | reedy green bog |
| 5 | 1.1% | gi13 | pointy-top diorama: pools, reeds, cart |

- **Slot order:** the calm tiles are frequent. The two cart tiles are split
  between a common slot and the rarest one, so carts don't repeat.
- **Marsh needs no alternates picker.** It has no POI sentinel variant, so the
  pool is simply however many `hexMarsh<N>.png` files exist, up to 16.
- **The old tiles are soft.** `hexMarsh0` and `hexMarsh1` were painted at about
  118 px, so they are upscaled about 2.2×. The originals are in git history
  (`5d9a9ea`) and in the scratchpad's `bak/orig_marsh/`.
- **gi29** (the loose marsh props) is catalogued but unused.

**Shaping** (`marsh_shape.py`):
- Face corners are picked by hand, on the centre of the black outline. The
  diorama slab walls are left out.
- Each face is inset 5 px on the sheets and 1.5 px on the old tiles, to get
  past the outline.
- Flat-top faces are warped with a centre-fan piecewise affine, vertex to
  vertex, onto the grid hex grown by `BLEED`.
- **gi13 is a pointy-top face.** A vertex fan onto a flat-top hex shears every
  sector 30°: the cart twisted and the top folded. It uses a radial warp
  instead: scale the face to the target's width and height, then stretch each
  ray from the centre so the source outline lands on the target outline. As a
  side effect, its reeds come out about twice as tall.

**Palette** (`marsh_grade.py`):
- A k-means over all six tiles pooled gives three classes: water
  (L* 32, b* 3), ground (L* 47, b* 9) and pale mud or reeds (L* 61, b* 11).
- Each pixel is soft-assigned to the classes (τ 6). Each class in a tile is
  moved 80% of the way to the pooled class mean and std (per-class Reinhard).
- Then a 25% snap to a shared 32-colour k-means palette.
- **Ink** means thin dark strokes: L* < 28, minus a 7×7 opening, so large dark
  areas don't count. Ink keeps its lightness and takes half the hue shift.
  gi8's near-black pond is large, so it grades as water; with a plain L*
  threshold, 40% of that tile would have been locked as ink.
- Result: the tiles' b* spread went from 0.5–16.5 to 2.3–11.9.

**Encoding:** `encode_tile.py`, 25–31 KB each, visible RMSE ≤ 3.5. `data/sw.js`
is bumped to `img-v8`. Checked in the mock: the log shows 6 Marsh variants and
all six load at 256 px. Not synced to the board.

## Flooded District cells 5-6 (2026-09-27)

Two whole painted hexes from the user, fitted and graded to cells 0-4 (the
Ash Dunes cell 4 method: `fit_ash.py` palette, transfer 1.0, snap 0.5).
Script: `fit_flood.py` + `fitlib.py` in scratchpad
`76cb17bd-cfc7-4769-be22-fe6bdb2d6c4b`, pasted sources in `..\images\`.

| Cell | Source | Fit |
|---|---|---|
| 5 | slab: storefronts, traffic light, rain rings | bottom vertices lifted 13 px (the painted side), inset 7, 6-triangle fan |
| 6 | 1.84:1 wide: poles, two cars, barrier, planks | "FLOODED DISTRICT" label inpainted (Telea), then a per-row fit |

- **Wide hexes:** a fan warp at 1.84:1 compresses the top/bottom sectors to
  0.31 of the vertical scale and the side sectors to 0.39 with shear, so poles
  kink on the centre->vertex seams. Cell 6 maps each target row edge to edge
  onto the same source row through u_s = 0.75u + 0.25u^3: the centre (cars,
  barrier) keeps ~0.8 of its proportions, the poles at the rim take the squeeze,
  and nothing is cropped. The alternative, `fit_uniform(rel=0.8)`, has truer
  proportions but loses the right poles and a plank.
- Every warp is 4x supersampled, then area-averaged down.
- Page: 7 tiles, q90, 128 KB, worst visible RMSE 3.11. Mock-verified in the
  map (both drawn by the real renderer). Not synced.

## Broken Urban

The user: "replace the Broken Urban with assets from D:\generated-image
(23).png", then "more variety" (use all 24, not 13). The old Broken Urban
master is backed up as `hexBrokenUrban.bak.png` in scratchpad 2a362bec.

- **Clips:** gi23's 24 buildings, cut in session 186e549f by
  `clip_assets.py` (`assets/gi23_*.png`), cleaned with `trees_bushes.clean()`.
- **Ground:** Open Scrub master cells 0–2, the approved open ground, dealt
  `clip % 3`. The buildings carry their own grass and rubble skirts.
- **Grade:** one transform for the whole set, measured on all 24 bodies
  pooled, so the teal, brick and white stay distinct from each other. a*/b*
  move to the ground's mean at 0.85× chroma, L* halfway to it; applied at
  0.35 strength, faded out on ink.
- **Placement:** fit inside 186×172 px, centred 6 px low, silhouette shadow
  (alpha blurred 3 px, offset (3, 4), 30%), clipped to the 1.035 hex. The
  user kept this size ("current is good") over 80% and 65% versions.
- **Clutter** (`bu_clutter.py`), asked for in steps: "clip some bushes and
  barrels and various clutter", then "a little bigger", "more clutter per
  hex", "even more", "little more". Where it landed:
  - **Pieces:** the approved graded gi27 bushes and wood from `treesbush/`
    (thorny bush, low bushes, leafy shrub, tall grass, bramble, dry tuft,
    brush pile, branches, log), plus gi27 ivy rubble, fenced bush, barrel
    planter and slab and gi17 barrels, jersey barrier and junk pile, given
    the buildings' own set grade. The vending machine read as a stray box and
    was dropped. `wasteland items.png` and gi6/9/10 are inventory icons, not
    ground clutter.
  - **Size:** 1.6× true scale, so it reads at map zoom.
  - **Count:** asks 14 per house, 16 per shop or civic; 10–16 fit.
  - **Where:** feet within 28 px of the walls, three times likelier along
    the sides and front than behind. In front, a piece may stand on the
    clip's grass skirt (the bottom 22 px of each column) and cover at most
    15% of wall. Behind, it must stay whole and clear of the roofline, or it
    reads as sitting on the roof. Pieces may overlap each other by 38%, so
    they heap.
  - **Mix:** houses place a piece of debris first, shops a prop and a
    debris, so bushes don't crowd out the street junk. A prop appears at
    most twice per tile, a bush three times.
  - **Contact shadows:** (58, 50, 30) at 30%, blurred 3 px.
- **Slots** (`hexBrokenUrban.json` names every cell):

  | Variant | Share | Own cell | Alternate |
  |---|---|---|---|
  | 0 | 26.0% | burnt two-storey | caved-in bungalow |
  | 1 | 21.0% | brick duplex | green cottage |
  | 2 | 16.6% | ivy farmhouse | colonial house |
  | 3 | 12.7% | bare frame | log cabin |
  | 4 | 9.4% | caravan trailer | garage |
  | 5 | 6.5% | awning shop | diner |
  | 6 | 4.2% | gas station | laundromat |
  | 7 | 2.3% | pharmacy | post office |
  | 8 | 1.0% | motel | — |
  | 9 | 0.3% | church | — |
  | 4_10 core | pinned | apartment block | gutted brick |
  | 4_11 core | pinned | teal corner block | bank |
  | 4_12 core | pinned | grey townhouse | fire station |

- **Page:** 1184×1760, 624 KB (188 KB before gi23, 461 KB before the
  clutter). The clutter's high-frequency detail tops the lossy ladder out at
  q95 with the worst tile at visible RMSE 3.66, past the 3.5 bar, like
  Settlement's 3.95.
- **Scripts** (scratchpad 2a362bec): `bu_compose.py` (candidates, grade),
  `bu_clutter.py` (clutter, into `cand2/`), `bu_patch.py` / `bu_patch2.py`
  (slot sheet, city patches), `bu_master.py` (writes the master from
  `cand2/` and the sidecar). They import from scratchpad 186e549f.
- **Checked:** mock client, all 24 cells reached through `terrainTile()`,
  and the cluttered tiles drawn in the game over a painted-in city;
  observer loads clean. Not synced, not flashed. The firmware is untouched:
  Broken Urban's count is still 10.

## Bunker Entrance (2026-10-12)

The user generated six bunker entrances from an image-gen prompt
(`bunker-hexes01.png`, on their Desktop) and said "they are turned wrong so
you will have to clip and use the open scrub background". The painting is
pointy-top isometric hexes on slabs, so only the props are used.

| Cell | Prop |
|---|---|
| 0 | concrete stairwell, hazard-striped steps |
| 1 | round vault hatch, lid open, ladder |
| 2 | blast door in a grassed earth mound |
| 3 | concrete elevator head-house, doors pried open |
| 4 | steel cellar doors, jerrycan, rope |
| 5 | sandbagged trench to a riveted bunker door, barbed wire |

- **Face:** the backdrop is black connected to the image border, found after
  a 5x5 opening, because the lid's outline joins the hatch pit to it. Pad the
  crop first: the source's black margin is only about 4 px. The slab comes
  off column by column, from the bottom up to the ink line above it.
- **Matte:** a hand outline per prop. Ground floods in from outside it through
  ground-coloured pixels only (b* > 14 and L* > 55, stopped by ink). Sand
  and the pale green patches are yellow (b* 17-25) and concrete is grey
  (b* 8-10), so the flood never enters concrete. Colour clustering and
  GrabCut both failed here, because the ground's tufts and cracks are the same
  black as the pits and outlines. The mound, the embankment and the sandbags
  are ground-coloured, so they are forced polygons that fade over 18 px where
  the source hex edge cut them. The ink rim gets the warm ink colour at the
  ink fraction its lightness implies.
- **Ground:** Open Scrub master cells 0-2, dealt `i % 3`. Every surface hatch
  sits on Open Scrub (`placeSurfaceHatches()`), so the tile melts into its
  neighbours.
- **Grade:** the Broken Urban recipe at 0.40 strength. The source's pure
  black ink is lifted to the sheets' warm black (30, 28, 20).
- **Placement:** one scale (0.60) for all six, with the source face centre
  4 px below the hex centre. Silhouette shadow (alpha blurred 3 px, offset
  (3, 4), 30%).
- **No firmware or client change:** the hatch keeps its Open Scrub variant
  and the client wraps it `% 6`. Page: q92, 106 KB, worst visible RMSE 3.40.
  Checked in the mock: all six are drawn by the real renderer. Not synced.
- Scripts `gcut.py` (outlines, face), `matte2.py` and `compose.py` are in
  scratchpad `8564a390-76e9-4d95-94f4-75ac3bf1312a\scratchpad\bunk`.

## Vent Shaft (2026-10-12)

The user generated six surface vent shafts (`bunker-shaft-hexes01.png`, on
their Desktop) and asked for "the same preprocessing" as the bunkers. The
sheet differs from the bunker sheet in two ways that change the cut.

| Cell | Prop |
|---|---|
| 0 | concrete vent stack, rusted rain cap, grille torn open, ladder |
| 1 | louvred intake housing with fan, panel on the sand, pit and ladder |
| 2 | open round shaft with hazard collar and ladder, grate dragged aside |
| 3 | two gooseneck exhaust pipes on a pad, floor grate pried open |
| 4 | storm-drain culvert, bars bent apart, in bushes and tall grass |
| 5 | riveted rusty sheet-steel vent tower, side hatch open onto a ladder |

- **White backdrop:** the face mask is the bunker one with the backdrop as
  border-connected near-white (min channel >= 225) and the crop padded with
  white. The slab scan now skips the slab's own bottom outline before
  looking for the dividing ink line.
- **Bushes round every prop:** the generator ignored "at most one small
  bush". The bushes touching the shaft are kept as part of the prop, and
  isolated tufts, agaves and stones are left out by the hand outline. The
  bushes share the ground patches' hue, so the b* gate would eat them.
  The flood gate is each tile's own two ground colours instead (k-means on
  the face outside the outline, dE < 9 with L* weighted 0.5). Bush bodies
  sit 14+ dE from both and concrete 13+.
- **Enclosed pockets:** only sand-coloured ones count as ground (L* > 80,
  dE < 7 to the sand colour). Bush highlights are patch-coloured pockets
  and must stay; a general pocket rule punched holes in every bush.
- **Rim fade for all:** anything within 24 px of the cut hex rim fades,
  except NOFADE polygons round structure that overhangs the hex outline (the
  rain cap, the pipe elbows, the tower lip). A DROP polygon removes a painted
  dune shadow behind the culvert's grass (non-ink pixels only).
- **Scale 0.57:** the bunkers' 0.60 times the face-width ratio of the sheets
  (376 vs 397 px), so props keep the same size against the hex. The face
  centre is the middle of the hex's vertical side edges, not the face bbox,
  because tall props overhang the top vertex. Prop reach is 0.93-1.02 of the
  hex; only faded bush tips reach the edge.
- Ground, grade, shadow, DY: as for the bunkers. No code change: the client
  wraps the hatch's Open Scrub variant `% 6`. Page: q95, 150 KB, worst
  visible RMSE 3.49 (tiles.json ec31e464ef3f). Checked in the mock: all six
  drawn by the real renderer. Not synced.
- Scripts `shaftcut.py` (face, outlines, matte) and `compose.py` are in
  scratchpad `8564a390-76e9-4d95-94f4-75ac3bf1312a\scratchpad\shaft`.

## River Channel (2026-10-13)

River had no tiles; `renderHexContent()` drew `drawRiverRipples()` instead,
and still does whenever the page isn't loaded. The user generated 8 sheets
from two prompts (both in `data/img/HEX_TILE_PROMPTS.md` "River Channel
prompts"): `river-hexes01..08.png` on their OneDrive Desktop, 1408x768, six
flat-top hexes each on flat magenta. 06 is byte-identical to 05.

- **Candidates:** all 42 hexes cut to `art/hex-sheets/river-candidates/`
  (`NN_sSS_cC.png` = candidate, sheet, cell) with a numbered
  `contact-sheet.png`; columns are subjects, rows are sheets. The user picked
  23 by number.
- **Matte:** magenta-ness `min(R,B) - G`, alpha ramped over 40..215, then
  de-contaminated (`fg = (p - (1-a) M) / a`). No visible magenta survives.
- **Fit:** 6 vertices from `approxPolyDP` on the matte, bucketed to the
  nearest 60 deg; the black hex stroke (3-4 px on sheets 01/03/07, under
  1 px on 02/04/05/08) is measured inward from the matte at 30 points
  and the vertices pulled in by stroke + 1.5 px; a centre-fan piecewise affine
  maps that onto the flat-top hex of R = 128 * 1.035 centred in the 256 cell
  (the Marsh/Glass geometry, rows 13-242), at 3x then INTER_AREA down.
  Source hexes are 1.21-1.27 : 1, so they squash slightly to 1.155.
- **Grade:** the picks came in two waters, slate blue-grey (b* about -3) and
  tan silt (b* +7 to +11), which patchworked along a 2-wide channel. Only the
  water moves: weight `w = clip((78-L)/12) * clip((L-10)/8)` (0 on foam and
  ink), each tile's full-weight water mean shifted toward the pooled water
  mean (Lab 41.9, -5.7, 1.7) by 0.5 in L* and 0.75 in a*/b*. Foam, rocks,
  the drum's rust and ink keep their colour.
- **Slots** (`hexRiverChannel.json` lists the alternates and each cell's
  source); shares are pickVariant's rank-quadratic weights for 8:

  | Slot | Share | Subject | Candidates (own cell first) |
  |---|---|---|---|
  | 0 | 31.4% | open current | 1, 7, 13, 19 |
  | 1 | 24.0% | river rocks | 26, 38 |
  | 2 | 17.6% | boulders | 2, 8, 14, 20 |
  | 3 | 12.3% | rapids | 4, 10, 16, 22 |
  | 4 | 7.8% | tree snag | 3, 15 |
  | 5 | 4.4% | oil drum | 5, 11, 17, 23 |
  | 6 | 2.0% | ruined boat | 27, 39 |
  | 7 | 0.5% | creature | 36 |

- **Page:** q92, 428 KB, worst visible RMSE 3.25 (tiles.json 64783636acd4).
  No firmware, client or mock change: counts[11] = 8 reaches
  `setupVariantCounts()`, and the alternates go through the existing
  `altPick()`. A world generated while River's count was 0 has every river
  hex at variant 0, so it shows slot 0 and its three alternates until a new
  world is made.
- **The mock never generates River** (`buildMap()` is `idx % 11`), so it was
  checked by flipping hexes to terrain 11 client-side.
- Scripts `contact.py`, `fit.py`, `grade.py` (also writes `preview-slots.png`
  and `preview-map.png`) and `master.py` are in scratchpad
  `8f1fa5c3-5666-44ad-9ed6-171bac40583c\scratchpad\river`.

## Mountain (2026-10-14)

The user generated five sheets from the two prompts in
`data/img/HEX_TILE_PROMPTS.md` "Mountain prompts", saved on their OneDrive
Desktop: `hex-mountians01..04.png` (prompt 1, the common peaks) and
`hex-mountians-rare01.png` (prompt 2, the rare structures). Each holds six
hexes on magenta.

**The rare sheet** (1376x768, flat-top) went in first:

- **Sheet quirks:** the top row has no top edge (the peaks stand against the
  magenta), and the bottom row's upper part is pure white sky (255).
- **Geometry:** left/right extremes, then the bottom edge at the last row
  still wider than 40% of the hex, with the two lower edges line-fitted to
  meet it. The top edge is mirrored about the middle row. Inset 3 px, then a
  centre-fan affine onto R = 128 * 1.035, at 3x and INTER_AREA down.
- **Sky matte:** magenta-ness `min(R,B) - G` ramped over 40..215, plus every
  pure-white blob (min channel >= 246) of 20+ px. A flood from the top edge
  misses the three white wedges the relay mast's guy-wires close off; no prop
  on the sheet is that white. De-contaminated, then a despill (R and B both
  above G) within 4 px of magenta, which leaves rock (B < G) and slate
  (R < G) alone.
- **Sky fill: a hazed far range, not flat haze.** Flat haze puts a pale band
  across the top of every row of a range. The fill is one unflipped strip of
  bare peaks from inside the bottom-row hexes (tile 4 below its ridge, tile 5
  below the dome; no tile gets its own), scaled to the hex width,
  top-aligned, blurred 0.9 px, and mixed 62% at the top to 50% at the bottom
  into the sheet's pale-rock haze (RGB 169, 168, 141). Mirror-tiling the strip
  made Rorschach X-shapes, so don't.
- **Grade:** only a*/b* move, 0.75 of the way to the old cell 0's mean; ink
  (L* < 15) is left alone.

**The common sheets** all hold the same six subjects: dense peaks, a broken
peak, twin peaks with a pass, a rockslide, a saddle with a cairn and rag
flag, and a cave mouth. All 24 were cut to `art/hex-sheets/mountain-candidates/`
(`NN_sSS_cC.png`) with a numbered `contact-sheet.png` that also shows them at
map size beside the old cell 0. The user kept 20 and held back the four cave
mouths (6, 12, 18, 24) for later; graded copies are
`cave-mouth-graded-NN.png` in the same folder.

- **Sheet quirks:** 02, 03 and 04 are flat-top. 04 is flat-top too, but
  squashed (1.08-1.11 : 1), and its outer hexes are clipped 1-2 px by the
  sheet edge. 01 is pointy-top (1264x848). 01 has a 3.5 px black hex stroke
  and 03 a 2.5 px one. 02 has a pink sky (RGB 250, 165, 251); 01, 03 and 04
  have a flat pale-khaki sky (std < 2).
- **Geometry:** one line per hex edge, fitted to the matte's contour within
  20 deg of that edge's normal, with points off the line dropped and refitted
  (a peak poking through an edge); the vertices are the line intersections. A
  vertex more than 2.5 px off its row's median shape takes the median shape's
  vertex. Inset by stroke + 1.5 px (3 px with no stroke).
- **Fit:** flat-top sheets use the centre-fan affine. Pointy-top 01 can't: a
  rotation turns the peaks sideways and an inscribed flat hex crops a quarter.
  It takes a radial warp instead. Every direction keeps its angle, the centre
  keeps a uniform (equal-area) scale, and the radius is eased to meet the
  flat-top boundary at the rim: `r_src = r * (s0 + (bp/bf - s0) * (r/bf)^1.5)`.
  Only the rim stretches (up to about 15%), and nothing is cropped.
- **Sky matte:** 02's pink is every pink blob (dE < 5) of 20+ px, since
  nothing painted is pink. The khaki skies are also the ground colour round
  the lower peaks on 03, so a flood from the top leaks down the sides. Sky is
  instead a straight run down each column from the top edge, plus sky-coloured
  pockets that touch the run (under the flag, beside a leaning peak). Both
  stop above a cap at 55% of the hex height, faded over 24 px, so 03 keeps
  its khaki round the peak bases. The edge is ramped 5..14 dE over 2 px and
  de-contaminated against the tile's own sky colour.
- **Sky fill:** the rare-sheet method, built in game space after the fit, with
  the same haze. Each sheet takes rows 105-185, x 34-222 of another sheet's
  dense-peaks tile (01 from 03, 02 from 04, 03 from 01, 04 from 02), mirrored
  on even subjects.
- **Grade:** the picks were already close (L* 51-57, b* 7-13, against the
  rare tiles' 54.7 and 12.7). Each moves 0.5 of the way in L* and 0.6 in
  a*/b* toward the rare tiles' mean; ink (L* < 15) is left alone. The bluest
  pass tiles warm by up to 3 b*.
- **Slots** (pool 3; `hexMountain.json` lists the alternates and each cell's
  source):

  | Slot | Share | Subject | Tiles (own cell first) | Each |
  |---|---|---|---|---|
  | 0 | 64.3% | bare peaks: dense, broken, twin + pass | 7, 1, 2, 3, 8, 9, 13, 14, 15, 19, 20, 21 | 5.4% |
  | 1 | 28.6% | landmarks: rockslide, cairn + flag | 10, 4, 5, 11, 16, 17, 22, 23 | 3.6% |
  | 2 | 7.1% | rare: mine, quarry, chairlift, relay mast, observatory, base camp | rare01 cells 1-6 | 1.2% |

  The old cell 0, a soft painted map-symbol that reads as cracked flat ground
  at map size, is retired; git HEAD keeps it.
- **Worlds made before this** carry Mountain variants 0-1 (counts[8] was 1,
  then 2). They show bare peaks and landmarks at once, but the rare
  structures only in a new world.
- **Previews** in `art/hex-sheets/mountain-candidates/`: `preview-map.png`
  (a 15x9 patch at the real mix, HEX_SZ 50) and `preview-slots.png`.
- Scripts are in scratchpad
  `eb4c16b1-c495-4b77-94e0-712006c097bb\scratchpad\mtn`: `geom.py` (edge
  lines), `cut.py` (matte, fan and radial fit, far range), `contact.py`,
  `master2.py` and `patch2.py` (previews). The rare-sheet scripts (`fit.py`,
  `compose.py`, `master.py`) are copied there too. The masters before each
  step are in `bak/` (cell 0 only) and `bak2/` (cell 0 + the rare slot).

## Rolling Hills (2026-10-14)

Terrain 7 is "Ridge" in game-data.js and "Rolling Hills" in the firmware; its
sheet is still `hexRidge`. The user generated two sheets from the prompts in
`data/img/HEX_TILE_PROMPTS.md` "Rolling Hills prompts", saved on their
OneDrive Desktop: `hex-hills01.png` (1408x768, common) and
`hex-hills-rare01.png` (1376x768, rare). Both are six flat-top hexes on
magenta with every edge drawn and no outline stroke.

- **Geometry:** every vertex comes from the matte: left/right extremes, the
  top and bottom rows still wider than 40% of the hex, and lines fitted to the
  four slanted edges. Inset 3 px, then the Mountain centre-fan affine onto
  R = 128 * 1.035. Any pixel with magenta in it (R and B both above G;
  nothing painted is) takes its nearest clean neighbour first, because the
  hex corners are blunted past the inset.
- **Sky:** every tile painted a flat pale sky (std < 1.5 per channel, about
  RGB 203, 202, 173 on the common sheet and 223, 219, 196 on the rare one).
  It is flooded from the hex's top edge: Lab distance to the tile's own sky
  colour, below 5 dE as the core, ramped 5..14 dE over a 5 px band, kept only
  where connected to the top. The horizon's antialiased edge is
  de-contaminated against the sky colour.
- **Fire tower pockets (rare cell 1):** the sky showing through the tower's
  cross-braces, railing and cabin windows is enclosed, so the top flood
  missed it and it shipped as cream triangles. No colour rule separates it
  from the bluff's pale limestone or the turbine's white blades, so
  `compose.py` `POCKETS` gives that tile a box (tile-local source px
  140,40-310,300) inside which anything within 11 dE of the sky is sky,
  ramped 11..20. The wider threshold takes the lighter halo (L* 89 against
  87) the generator left round each pocket. Only that cell was rewritten.
- **Sky fill:** the Mountain method, a hazed far range. Kept sky left a cream
  wedge on top of every hills hex on the map. Three strips, the below-horizon
  ground of common tiles 1-3 (no props), are used at 1:1 hex width,
  top-aligned, blurred 1.2 px, and mixed 55% at the top to 45% at the bottom
  into the common sheet's haze (RGB 186, 176, 138). Each tile takes one
  strip, some mirrored, never its own ground. Scaling one strip up 1.6x for
  variety made the far hills bigger than the near ones, so don't.
- **No grade:** the tiles sit at L* 59-61, b* 18, between Open Scrub (L* 66)
  and Mountain (L* 50), which is right for the ring between them. The old
  cell 0 (L* 48, a soft map-symbol) clashed on the map and was retired; git
  HEAD keeps it.
- **Slots:** pool 2. Cell 0 (rolling hills) is the common slot with
  alternates 2-6 and 11-16 (80%, about 6.7% each). Cell 1 (fire tower) is
  the rare slot with alternates 7-10 (20%, 4% each), all in `hexRidge.json`.
  The rare sheet's glass observation gallery was removed at the user's
  request after integration; the cells after it moved down one.
  Worlds made while counts[7] was 1 have every hill at variant 0, so they
  show the common tiles at once and the rare ones only in a new world.
- **Nature tiles (`hex-hills02.png`, same day):** heather, copse, tor,
  wildflower grass, gorse, lone tree, appended as cells 12-17 (11-16 since
  the gallery's removal) and listed as
  more alternates of variant 0, so the common slot holds 12 (about 6.7%
  each). Cells 0-11 were read back from the master unchanged
  (`master_nature.py`).
  - Five of its skies are pale pink (RGB 253, 160, 255 and 250, 207, 247),
    one cream. A pink sky is matched as every blob of its colour, not only
    the part touching the top: the tor has pink pockets between its
    boulders. A cream sky stays top-connected, because pale grass crests
    match cream.
  - The heather is mauve, so R and B are both above G (up to 15). Inside a
    tile the magenta cleanup only takes blends above 20; the 4 px rim takes
    anything above 4.
  - Pink sky also left a fringe on the horizon ink, so R and B are despilled
    down to G within 4 px of a pink sky.
  - Ungraded. They are a little darker (L* 54, the gorse tile 46, against 60)
    and mix in on the map.
- The user does not want the original painted hills tile back.
- Scripts `fit.py`, `compose.py` (`python compose.py n` for the nature
  sheet), `preview.py` (map-scale patch), `master.py` and `master_nature.py`
  are in scratchpad
  `53ab439a-ef8d-4331-bd17-a70366f4eda7\scratchpad\hills`, with the
  pre-hills master, page and tiles.json in `bak/` and the 12-tile ones in
  `bak2/`.

## Integration

**Done (2026-09-23): the 10 shipped tiles.**

- They're palette PNGs made with `encode_tile.py` at the repo's quality bar
  (visible RMSE ≤ 3.5). `scripts/png_quant.py` crashes on these tiles with
  "invalid palette size": the anti-aliased rim has more than 256
  (colour, alpha) pairs, so `encode_tile.py` gives the rim its own small
  palette.
- The 10 tiles total 282 KB. The old ones were 52 KB.
- `data/sw.js` `CACHE` is bumped to `img-v6`, because the files changed in
  place.
- They've been checked in the mock server. They haven't been synced to the
  board.
- The old tiles are still in git history (`5d9a9ea`). They are also the colour
  reference for the common green and the leaf grade, so the scripts read the
  copies in the scratchpad's `bak/orig_scrub/`, not `data/img`. Rebuilding from
  those copies is byte-identical.

**Done (2026-09-27): one sheet per terrain.** The user asked for "a hex sheet
not individual hexes" and said to "leave space between each hex".

- **Masters:** `art/hex-sheets/hex<Name>.png`. They are lossless RGBA, in a
  grid of 256 px cells, 4 across, with 32 px of clear space around and
  between each cell. Cell N is variant N.
  - Old tiles smaller than 256 px (such as the 108×96 Ridge tile) are baked
    stretched to 256×256, the same stretch the renderer always applied to
    them.
  - Open Scrub cell 10 is Jack's Chopper, the old `poi_jacks_chopper.png`.
    Cells from a terrain's pinned sentinel on are landmarks (`LANDMARKS` in
    the script).
  - All 79 cells were checked pixel for pixel against the per-file tiles
    they replaced.
- **Shipped:** `data/img/hex<Name>.webp`, 15 pages and 1.4 MB in total. The
  per-file tiles they replaced were 1.31 MB.
  - Each page is lossy with lossless alpha. `hex_sheets.py build` picks the
    lowest of q90/92/95 that keeps the page's worst tile at visible
    RMSE ≤ 3.5.
  - Settlement is the exception: it tops out at q95 / 3.95, because the
    yellow school bus hits the 4:2:0 chroma floor. That is invisible at game
    size, and only slightly soft at 3× zoom.
- **Manifest:** `data/img/tiles.json`, the same format the old atlas plumbing
  used, plus a `flat: true` flag.
  - `flat` keeps the painted tiles drawing exactly as the per-file ones did:
    grid stroked over them, no overhang split, and remembered hexes
    unclipped (`tile.diorama` in renderer.js).
  - Readers: `engine.js`, `observer.js`, the mock's `scanVariantCounts()`
    and the firmware's `setupVariantCounts()`.
- **Where it's been checked:** mock-verified on 2026-09-27. The client and the
  observer fetch only the 15 sheets, the mock logs identical variant counts,
  and landmark lookup works: `0_10` resolves to Open Scrub cell 10, and
  `4_1x` wraps into the pool.
- **Not checked on the board.** It hasn't been synced to the board. The
  firmware atlas path (the `tiles.json` counts, the `hex*.png` skip in
  `boot-assets.hpp`) is in HEAD but has never run with a real `tiles.json`
  on the card.
- **The per-file tiles are gone from `data/img`.** Only git has them now: the
  shipped versions in HEAD before 2026-09-27, and the originals in `5d9a9ea`.

**Done (2026-09-28): the alternates picker.** A terrain sheet may carry a
sidecar `hex<Name>.json` with `{"alts": {"<variant>": [cell, ...]}}`. The
cells sit past the pool on the same sheet; `build` puts them in `tiles.json`
`alts` and keeps them out of `counts` and `poi`. `terrainTile(t, v, q, r)` in
engine.js and `Art.tile(..., q, r)` in observer.js deal one of [the variant's
own cell, *its alternates] with `altPick(q, r, n)`. A landmark variant can
have alternates too.

**Not done:**

1. **Open Scrub's 33 alternates.** The picker exists now, but they still sit
   in the separate `data/img/tile-alts0.webp` (264 px stride, with
   `tile-alts.json`), which nothing loads. To use them, paint them into
   `hexOpenScrub.png` past cell 10 and list them in a `hexOpenScrub.json`.
2. **Jack's Chopper.**
   - Its POI art, `poi_jacks_chopper.png`, is a gas station, but
     `data/encounters/scrub/19.json` describes a crashed military helicopter.
   - The Huey (`gi26_12`) has been proposed for it. That's still undecided.

## Tunnels

The user asked for "rooms and corridors" using "as many hexes as possible".
All 41 hexes on their underground sheets are used.

**Source art (`D:\`):**

| Sheet | Hexes | Used as |
|---|---|---|
| `generated-image (1).png` | 8 angled, **pointy-top** | stairwell (entrance), catwalk pit, crate store, shelving, command dome, blast door, cable tunnel, sewer outflow (vent) |
| `generated-image (2).png` | 6 angled | generator, barracks, washroom, HVAC fan (vent), vault door (entrance), hydroponics |
| `addtional/generated-image.png` | 6 angled, black rock corners | junction, slab collapse (cave-in), rusted door, flooded floor, wall vents (vent), cave-in |
| `addtional/generated-image (3).png` | 6 angled, black rock corners | infirmary, server racks, mess hall, elevator (entrance), water tanks, guard booth |
| `addtional/generated-image (4).png` | 9 overhead, grey | 2 straights, the bend (used twice), a T, 4 crossings, the Y (also used for a second T) |
| `addtional/generated-image (5).png` | 6 overhead, olive | 4 straights (chevrons, dashes, alcove, S-curve), 2 crossings |

**How they were made (scratchpad `tun/`, session 35fecc17):**

- The painted checkerboard is found locally: a pixel is background where its
  31 px neighbourhood is mostly the sheet's two checker greys (the dark grey
  drifts down the sheet on `(3).png`). A regular hexagon is fitted to each
  cell by IoU; the grid split comes from projection minima.
- Every cell is affine-fitted to the game hex at the 1.035 bleed. Pointy-top
  cells take the *inscribed* flat hex, so nothing bends. The painted white
  vignette rim is then never sampled: a soft radial remap reads radius
  0.78–0.90 for output 0.78–1.05.
- **Overhead corridors are square-grid art** (east–west runs through the hex
  corners, 45° diagonals, "+" crossings). Each arm is found on a ring at 0.86
  of the hex radius, then warped so it leaves through a flat-top edge midpoint
  with a 62 px mouth. Straights are rotated and scaled rigidly, with their
  ends mirror-filled along the axis. Everything else goes through a fan of
  affine triangles from the centre, with knots at every arm wall and hex
  vertex. A crossing keeps its vertical run and tilts the other by 30°.
- **One palette (`unify.py`).** The user: "at least 3 different palettes in
  play. we just need one palette". The olive corridor sheet is first graded
  to the grey one. Then every tile is graded per *source sheet* toward the
  pooled target of all six, each weighted equally, so no one sheet is the
  palette:
  - lightness mean and contrast, linear, at 0.75 strength. A quantile map
    stretched the corridors' narrow rock peak into camouflage blotches;
  - the tint at every lightness band, so shadow, midtone and highlight tint
    each land on the shared one, with the chroma stretch capped at 0.6–1.35×;
  - a hue snap (0.45) toward one shared 32-colour k-means palette, which
    gives one green, one rust and one black;
  - **one rock:** the graded corridor rock is measured, and anything darker,
    or up to 12 L above it, is pulled onto it. The a0/a3 rooms' pure-black
    corners become that same rock, and rock is kept out of the snap.
  Measured after: every sheet sits at L ≈ 40 ± 18, chroma ≈ 6.2, b ≈ 6.1.
  Before, they spread over L 33–43, chroma 5.5–7.1 and b 5.2–7.0.
- Plain rock (`tunFixture` cell 8) is that graded rock colour, the one
  generated cell: it only has to disappear into the rock around a corridor.

**Shipped:** `art/hex-sheets/tunCorridor.png` + `tunCorridor.json` (the open
sides each piece is drawn with), `tunRoom.png` + `tunRoom.json` (the
corridor-like "chamber" rooms), `tunFixture.png` + `tunFixture.json`
(entrance, vent, cave and rock cells). `hex_sheets.py build` writes three
pages (about 600 KB) and the manifest's `tunnel` and `tunnelCounts`. The
placeholder `hexTunnelFloor` / `hexTunnelCollapsed` sheets are gone. The
surface Bunker Entrance and Vent Shaft art is untouched: the interiors are
drawn only underground.

**How the art is chosen:** the board has walls now (tunnels.hpp, "Open
sides"). Each cell's `op` byte rides the wire next to it. The client turns
and mirrors a corridor piece onto the open sides (`tunnelTile()` in
`data/tunnel-board.js`). Rooms, shafts and cave-ins are dealt an art index by
the firmware and drawn upright, which is why they always hang above their
corridor: the open front of an angled room faces down.

## Where the tools are

All of the scripts are in the session scratchpad, which is a temporary
directory:
`%LOCALAPPDATA%\Temp\claude\C--SourceCode-Esp32HexMapCrawl\186e549f-0ba5-4f5a-96e2-d36eb73ca21e\scratchpad`.
The pasted dry-scrub hex is `..\images\1.png` beside it. **Copy them into the
repo before relying on them.**

The Marsh scripts, `marsh_shape.py` then `marsh_grade.py`, are in a different
session scratchpad:
`%LOCALAPPDATA%\Temp\claude\C--SourceCode-Esp32HexMapCrawl\fae94b65-0537-4e4f-893b-6bbc7632df72\scratchpad`.
They write `marsh/shaped/` and then `marsh/graded/`. That folder also holds
copies of `clip_assets.py`, `encode_tile.py` and the rest.

| Script | Does | Writes |
|---|---|---|
| `clip_assets.py` | cuts objects off a checker sheet | `assets/gi{N}_{kk}.png`, catalog |
| `cut_v2.py` | cuts props off a painted hex | `cuts2/` (cut by hand, no driver script) |
| `scrub_ground.py` | dry-scrub hex face mask; skull and rock boxes | `ref_cuts/` |
| `cut_pole.py`, `run_poles.py` | pole cut-outs | `poles/` |
| `pole_variants.py` | flips, tilts, poles snapped in two | `poles/var_*.png` |
| `quilt.py` | image quilting | — |
| `crack_soften.py` | crack classifier and 90% toning | — |
| `grade.py` | palette from any terrain's painted tiles, plus the grade | — |
| `compose_scrub3.py` | ground source, `ScrubPalette`, shadow / put / `clip_hex` | — |
| `compose_scrub20.py` | open ground (flavours, splashes), poles, the 27 base tiles | `scrub20/` |
| `trees_bushes.py` | dead trees and bushes: clean, wood/leaf/family grades | `treesbush/` |
| `planes.py` | aircraft with silhouette shadows | `planes/` |
| `scrub_mix.py` | slot allocation, slot sheet, map patch | `scrub_mix_*.png` |
| `encode_tile.py` | palette-PNG encoder that handles the hex rim | the shipped `hexOpenScrub*.png` |

- **Run order:** `clip_assets` → `run_poles` → `pole_variants` →
  `compose_scrub20` → `trees_bushes` → `planes` → `scrub_mix`.
  `compose_scrub20` expects `cuts2/` and `ref_cuts/` to exist already.
- **Requirements:** Python 3.12, Pillow, numpy, scipy, scikit-image and
  opencv-python. `compose_scrub20` imports `tg_core` from `scripts/tilegen/`.

## Next terrain

1. Ask which of the user's art to use, then catalog those sheets with
   `clip_assets.py`.
2. Take the terrain's palette from its painted tiles (`grade.py`) or from its
   reference painting.
3. Quilt the ground from the user's paintings of that terrain. **Show it and
   stop.**
4. Make candidate features, graded, one per tile. **Show them and stop.**
5. Draft the slot allocation and a map patch. **Show them and stop.**
6. Integrate only after an explicit go: paint the tiles into the terrain's
   sheet in `art/hex-sheets/`, then run `python scripts/hex_sheets.py build`.
