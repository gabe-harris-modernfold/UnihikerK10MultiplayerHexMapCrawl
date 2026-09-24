# Hex tile art: clip, grade, place

This is how the hex terrain tiles are being redrawn:

- Every object is clipped from the user's own art.
- Each clip is graded into the palette of their painted tiles.
- Clips are placed sparsely on ground stitched together from their paintings.
- No tile art is drawn in code.

**Status (2026-09-23):**

- **Open Scrub:** 10 of its 43 tiles are in `data/img`, one per variant slot,
  as `hexOpenScrub0–9.png`. The other 33 are waiting for alternates support;
  see [Integration](#integration).
- **Marsh:** 6 tiles in `data/img` as `hexMarsh0–5.png`. These are whole
  painted hexes, reshaped and regraded; see [Marsh](#marsh).
- **Other terrains:** not started.

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
| gi18–23, gi28 | buildings | — |
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

- The client picks an alternate by a stable hash of the hex's position:
  `((q*73856093) ^ (r*19349663)) % alternates`. This is not implemented yet;
  see [Integration](#integration).
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

**Not done:**

1. **Alternates picker.**
   - Map (terrain, variant, q, r) to an alternate using the hash above.
   - Do it in `terrainTile()` in `data/engine.js`, and match it in `Art.tile`
     in `data/observer.js`.
2. **Delivery.**
   - WebP at quality 80 is about 10 KB per tile, so Open Scrub is about 430 KB.
   - The working tree has uncommitted atlas plumbing: `data/img/tiles.json`
     plus `tiles<N>.webp` pages.
     - Loaded by `engine.js`, `renderer.js` and `observer.js`.
     - Counted by `game-server.hpp` and `mock-server/server.js`.
     - `boot-assets.hpp` skips the per-file tiles when `tiles.json` exists.
   - Without `tiles.json`, everything falls back to per-file
     `hex<Name><N>.png`.
   - The firmware side has never been compiled or flashed.
   - Atlas versus per-file is still undecided.
3. **Jack's Chopper.**
   - Its POI art, `poi_jacks_chopper.png`, is a gas station, but
     `data/encounters/scrub/19.json` describes a crashed military helicopter.
   - The Huey (`gi26_12`) has been proposed for it. That's still undecided.

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
6. Integrate only after an explicit go.
