# Hex Tile — Image-Gen Prompts

Companion to [items/ICON_PROMPTS.md](items/ICON_PROMPTS.md), same idea, different
asset family. Unlike items (where zero real art exists), most terrains already
have hand-painted tile art — this covers filling the gaps and adding variety,
not starting from scratch. Style was reverse-engineered from the actual files
in this folder (`hexOpenScrub0.png`, `hexSettlement3.png`, etc.), not guessed.

## What's actually on disk today

| Idx | Terrain | Variants now | Fill (behind the art) | Gap |
|----|------------------|:-:|---------|-----|
| 0  | Open Scrub       | 10 (+ reserved #10 for the Jack's Chopper POI) | `#2E2210` | done |
| 1  | Ash Dunes        | 4  | `#201E16` | thin |
| 2  | Rust Forest      | 10 | `#1A2808` | done |
| 3  | Marsh            | 2 real + 2 placeholder | `#081A10` | **priority** |
| 4  | Broken Urban     | 10 | `#1A1814` | done |
| 5  | Flooded District | 5  | `#08121E` | okay |
| 6  | Glass Fields     | 2 real + 2 placeholder | `#121A22` | **priority** |
| 7  | Ridge (Rolling Hills) | 2 slots: 12 common + 5 rare tiles (2026-10-14) | `#1E1A12` | done, see "Rolling Hills prompts" |
| 8  | Mountain         | 3 slots: 12 bare peaks + 8 landmarks + 6 rare tiles (2026-10-14) | `#14141C` | done, see "Mountain prompts"; 4 cave mouths held back |
| 9  | Settlement       | 8  | `#1A1206` | done |
| 10 | Nuke Crater      | 2 real + 2 placeholder | `#0A0E04` | thin |
| 11 | River Channel    | 8 slots + 15 alternates (23 tiles) | `#0B1E0F` | done (2026-10-13), see note below |

**The tiles are one sheet per terrain, not one file per tile.** The master
is `art/hex-sheets/hex<Name>.png`: lossless, and outside `data/` so it never
syncs to the board. It is a grid of 256 px cells, 4 across, with 32 px of
clear space around each one, and cell N is variant N.
`python scripts/hex_sheets.py build` encodes each master to
`data/img/hex<Name>.webp` and writes `data/img/tiles.json`. The firmware reads
that file's `counts` at boot ([game-server.hpp](../../game-server.hpp)
`setupVariantCounts()`). A new tile goes in the next empty cell, and there is
nothing else to register. If a tool wants single files,
`hex_sheets.py split <Name>` cuts a sheet into one PNG per cell.

**Slots marked "placeholder" are flat labelled tiles** from
`scripts/gen_missing_tiles.py`. Each one prints its old per-file name on the
hex, so when one shows up on the map you can read off which cell to paint:
`hexRidge2.png` means cell 2 of the Ridge sheet. To replace one, paint over
that cell and run `hex_sheets.py build`. No `sw.js` bump is needed, because
the pages are fetched with `?v=<hash>`.

**River Channel has painted art since 2026-10-13** (`art/hex-sheets/hexRiverChannel.png`,
8 pool slots, each with alternates listed in `hexRiverChannel.json`). Before
that it had no tiles and was drawn as an animated ripple effect
(`drawRiverRipples()` in [renderer.js](../renderer.js)); the renderer still
falls back to the ripples whenever the page isn't loaded. The two prompts that
made the art are below under "River Channel prompts". The river is usually a
2-wide channel, so its tiles must carry water out to all six edges (no bank
round the rim) and a swirling current with no single direction (the variant is
dealt by position, not by which way the channel runs). Details of the cut and
grade are in [docs/hex-tile-art.md](../../docs/hex-tile-art.md) "River Channel".

## Why variant 0 matters more than the others

Variant choice isn't uniform — [hex-map.hpp](../../hex-map.hpp) `pickVariant()`
weights rank-quadratically: variant 0 is picked roughly `n²` times as often as
the last variant. When you add new files, they append after the current
highest index (rarer slots) — they won't naturally become the "common" look.
If you want a *new* piece to be the common case, that's a deliberate swap of
index 0's content, not just adding a file; do that on purpose, not by accident.

## STYLE (prefix — use for every terrain)

Reverse-engineered from the existing files: painterly/ink-illustrated (not
pixel art — that's the item-icon style, this is a different asset family),
hand-drawn linework on structural elements, soft painted shading, muted
desaturated post-apocalyptic palette. Measured corner/edge alpha on every
sampled file is fully transparent, with a soft antialiased fade right at the
hex boundary — not a hard geometric clip and not a hard outline stroke.
Measured aspect ratio across samples is consistently ~1.1–1.25 : 1 (wider
than tall), matching the game's flat-top hexagon (`drawHexPath` in
renderer.js draws vertices at 0°/60°/120°/180°/240°/300°, bounding-box ratio
2 : √3 ≈ 1.1547).

```
Top-down painterly game tile illustration for a flat-top hexagon board tile,
noticeably wider than tall (~6:5 aspect ratio). Hand-inked linework on any
structures or rock detail, soft painted shading, muted desaturated
post-apocalyptic wasteland palette — no saturated colors, no vibrant
highlights. The painted content fills a flat-top hexagon silhouette (points
left and right, flat edges top and bottom) and fades with a soft, slightly
irregular hand-painted edge into full transparency right at the hexagon
boundary — not a hard geometric cutout, not an outlined border. Fully
transparent background outside the hexagon. This tile sits edge-to-edge next
to five others of the same terrain, so do not draw a frame, vignette
darkening, drop shadow, grid line, text, or watermark — and don't design for
seamless texture continuation across the edge, each tile is an independent
vignette on a shared flat background color, not a repeating pattern. Soft,
diffuse, non-directional lighting (no strong single-direction cast shadow,
since neighboring tiles won't share it).

Subject: {SUBJECT}
```

Generate large (768–1024px on the long edge) for clean linework. Then
downscale it into a 256 px cell of the terrain's sheet, with the hex at full
width and 221.7 px tall, and run `python scripts/hex_sheets.py build`. The
board loads each sheet into the PSRAM image cache at boot as one file, so a
terrain takes one slot (`MAX_IMG_CACHE = 160` in the .ino), not one per tile.

### Canvas geometry: the destination is a SQUARE

`renderHexContent()` in [renderer.js](../renderer.js) draws every tile as

```js
const imgSz = HEX_SZ * 2;
ctx.drawImage(tImg, cx - imgSz / 2, cy - imgSz / 2, imgSz, imgSz);
```

— one **square** box, `2 × HEX_SZ` on both sides, whatever the source aspect
is. A non-square source is not letterboxed, it is stretched to fit. Meanwhile
`drawHexPath()` strokes a hexagon `2 × HEX_SZ` wide but only `√3 × HEX_SZ`
tall. So the rule is:

> **Deliver a square canvas (256×256), with the hexagon spanning the full
> width and the middle 86.6% of the height.** The top and bottom 6.7% bands,
> and the four corners, are dead space that no hex ever covers.

The existing hand-painted tiles do *not* follow this — their hexagon fills the
canvas, so once stretched into the square it comes out 7–15% taller than the
outline and overhangs into the neighbouring hexes (hexFloodedDistrict1 is the
worst at 15.5%, hexOpenScrub0 the mildest at 7%). On the map that reads as a
slight overlap rather than an obvious fault, which is why it went unnoticed —
but new art should be authored to the rule above, not matched to the old
files. The labelled placeholders from `scripts/gen_missing_tiles.py` are
already square and correctly inscribed; open one to see the intended
proportions, and note the dashed square and corner brackets it draws are the
draw box itself.

## Worked example (Ridge, a new variant 1)

```
Top-down painterly game tile illustration for a flat-top hexagon board tile,
noticeably wider than tall (~6:5 aspect ratio). Hand-inked linework on any
structures or rock detail, soft painted shading, muted desaturated
post-apocalyptic wasteland palette — no saturated colors, no vibrant
highlights. The painted content fills a flat-top hexagon silhouette (points
left and right, flat edges top and bottom) and fades with a soft, slightly
irregular hand-painted edge into full transparency right at the hexagon
boundary — not a hard geometric cutout, not an outlined border. Fully
transparent background outside the hexagon. This tile sits edge-to-edge next
to five others of the same terrain, so do not draw a frame, vignette
darkening, drop shadow, grid line, text, or watermark — and don't design for
seamless texture continuation across the edge, each tile is an independent
vignette on a shared flat background color, not a repeating pattern. Soft,
diffuse, non-directional lighting (no strong single-direction cast shadow,
since neighboring tiles won't share it).

Subject: a jagged ridge of compressed grey-tan slag-stone with a loose scree
slope of broken rock sliding down one side, a single wind-bent dead shrub
clinging to a crack near the crest.
```

Save as `data/img/hexRidge1.png`.

---

## Subjects, by terrain

Each row gives a base scene plus a few interchangeable focal details — swap
the detail clause to generate more than one variant from the same base
without repeating yourself. Wording leans on each terrain's `desc`/`tags` in
[game-data.js](../game-data.js) so the tile reads as *that* terrain, not
generic wasteland ground.

| Idx | Terrain | Subject (base scene) | Swap-in details for extra variants |
|----|------------------|------|------|
| 0 | Open Scrub | Windswept flat of pale dry scrub grass and cracked tan earth, scattered pebbles | a half-buried animal skull; faint rabbit tracks; a broken fence post; a lone dead sapling |
| 1 | Ash Dunes | Rolling drifts of fine grey volcanic ash with a faint fallout haze | a rusted fuel drum half-buried in a crest; scattered bone-white driftwood; a torn tarp snagged on ash; exposed rebar poking through a dune |
| 2 | Rust Forest | Dense cluster of skeletal dead trees coated in rust-red fungus, canopy so thick the ground sits in shadow | a fallen trunk bridging a gap; a fungus-crusted animal den; shafts of light breaking through a canopy gap |
| 3 | Marsh | Brackish wetland: dark stagnant water pools between reed clumps and salt-crusted mud flats | a half-sunken shopping cart; a rotted wooden walkway plank; a cluster of bleached reeds; bubbling gas pockets in the mud |
| 4 | Broken Urban | Collapsed concrete hab-block rubble, exposed rebar, a caved-in stairwell | a leaning street sign half-buried in debris; a rusted car chassis; a shattered storefront; a crumbled overpass support |
| 5 | Flooded District | Drowned city street, murky floodwater lapping at submerged car roofs and a tilted lamppost | a half-submerged bus; floating debris raft of planks; a drowned traffic light; ripple rings from something below |
| 6 | Glass Fields | Flat sheet of fused glassy detonation glass, cracked in a radial spiderweb pattern, faint iridescent sheen | a chunk of scrap embedded mid-melt in the glass; a scorched shadow silhouette burned into the surface; sharp glass shards jutting up |
| 7 | Ridge | Jagged ridge of compressed grey-tan slag-stone, loose scree slope, wind-scoured crest | a wind-bent dead shrub in a crack; a lightning-scorched outcrop; a distant view ledge with a cairn of stacked stones |
| 8 | Mountain | Towering cracked slag-mountain peak with a dark cave mouth cut into its base | a rope anchor hammered into rock by the cave; a rockslide of loose boulders; a rusted mining cart abandoned on a ledge |
| 9 | Settlement | Fortified survivor camp: scrap-metal palisade wall, a watchtower, strung warning lights | a cookfire with smoke and a hanging pot; a trade stall under a patched awning; a water tower patched with scrap; a vehicle checkpoint gate |
| 10 | Nuke Crater | Scorched glass-lined detonation crater rim, blackened earth, faint sickly green radioactive glow pooling at the bottom | a twisted girder skeleton at the rim; ash-white fused-glass ripples; a single dead tree silhouette at the edge |
| 11 | River Channel *(done 2026-10-13 — the prompts actually used are under "River Channel prompts")* | Fast dark river current cutting a channel, whitecap ripples breaking around submerged rocks | a snagged branch caught mid-current; a half-collapsed footbridge; a stranded rowboat wedged against the bank |
| 12 | Bunker Entrance | Pre-war blast door set into a cracked concrete apron, hinges intact, hazard striping worn to ghosts, dark stairwell dropping away behind it | a stencilled bunker number; sandbags slumped either side of the frame; a jammed-open door leaking cold air; a rusted keypad housing prised off |
| 13 | Vent Shaft | Collapsed air-intake: a rusted grate over a black drop, ringed by subsided earth and torn ducting | scrub grown through the grate bars; a knotted rope tied off to a duct stub; one grate panel folded back; a warm updraught blurring the air |
| 14 | Tunnel Floor | Cramped service corridor seen from above: cracked concrete slabs, conduit and cable trays along one wall, a seeping cistern stain | a puddle of standing seep water; a toppled supply crate; a dead wall-light in its cage; stripped copper hanging from a tray |
| 15 | Collapsed Tunnel | Total cave-in: fallen rock and buckled ceiling plate filling the corridor floor to roof, dust settled over everything | a crushed conduit run poking through; a bent girder wedged diagonally; a hand-torch buried in the spill |

## Notes

- Priority order if generating incrementally: **Ridge and Mountain first**
  (1 variant each — any repeated hex of these terrains currently looks
  identical), then Marsh and Glass Fields (2 each), then Nuke Crater and Ash
  Dunes, then top up the "done" terrains only if you want extra variety —
  they already avoid obvious repetition.
- The bunker tunnel tiles (12-15) currently ship as **flat placeholder art**
  generated by `scripts/gen_placeholder_tiles.py` — plain hex silhouettes with
  a simple motif. They are meant to be replaced; generate real tiles from the
  rows above and paint them over the same sheet cells.
- Replacing a tile that already shipped needs no `sw.js` bump. A rebuilt sheet
  gets a new `tiles.json` version, and the pages are fetched `?v=<version>`.
- Fill cells in order. The pool runs from cell 0 to the first empty cell, so a
  gap drops every tile after it. `hex_sheets.py build` warns when that happens.
- Hard ceiling: `cell.variant` is packed into 4 bits on the wire
  (`variant & 0x0F` in hex-map.hpp), so only indices **0–15** are ever
  addressable per terrain regardless of how many files exist.
- Pinned art is a fixed image on one specific hex, outside the numbered
  variant pools. It sits in its terrain's sheet at the pinned variant's cell
  and goes to `tiles.json` `poi`, not to `counts`. See "Pinned art" below.
- Item icons (different doc, different style — pixel art, not painterly) are
  in [items/ICON_PROMPTS.md](items/ICON_PROMPTS.md).

## Pinned art

A hex whose `variant` the firmware pins past the end of its terrain's counted
pool. Its art is the sheet cell at that variant. `hex_sheets.py` keeps a
`LANDMARKS` table of each terrain's first sentinel, and treats every cell from
that sentinel on as a landmark. Landmarks go to `tiles.json` `poi` as
`terrain_variant` and are kept out of `counts`. The renderer looks the pair up
in `poi` first and only falls back to the pool when there is no entry.

| Key | Cell | Pinned by | Subject |
|----|----|----|----|
| `0_10` | Open Scrub cell 10 (was `poi_jacks_chopper.png`) | hex-map.hpp Phase 5.5 — the scrub hex holding `scrub/19.json` | named landmark, already shipped |
| `4_10`, `4_11`, `4_12` | Broken Urban cells 10–12 (2026-09-28: apartment block, teal corner block, grey townhouse from gi23, with alternates in `hexBrokenUrban.json`) | hex-map.hpp Phase 4 — any Broken Urban hex with 5+ urban neighbours, one of the three picked at random | **Dense downtown core.** Standing multi-storey hab-block shells rather than ground rubble: walls still up three or four floors, window rows blown out, a street canyon running between them in deep shadow. It must read as *taller and more intact* than `hexBrokenUrban0-9` at a glance — its whole job is to mark the middle of a real city against the fringe rubble around it. Swap-ins for extra variants: a collapsed skybridge between two towers; a toppled crane leaning across the canyon; a gutted parking structure; a billboard frame stripped to its lattice. |

Notes on the downtown core tiles:

- **Draw all three, or none.** With no `poi` entries every pin falls back
  to ordinary rubble and the map stays coherent; with one entry a downtown
  street mixes one core tile against plain rubble and reads as a bug. A
  simulated 200 worlds pins a median of **11** core hexes, clustered inside
  the same few cities, which is why one tile is not enough.
- Phase 4 pins them only where a city has a genuinely dense interior, so a
  typical world marks ~11 hexes and small townships mark none at all.
- Without the cells (an older tiles.json), the renderer silently falls back to the
  numbered Broken Urban pool (`variant % poolLength`), so the firmware side is
  already live and harmless.
- The core tiles can't leak into the pool. Broken Urban's `LANDMARKS`
  sentinel is 10, so cells 10 and up never count toward
  `terrainVariantCount[4]`. Keep ordinary rubble in cells 0–9.
- Adding a fourth? Sentinels `4_13`..`4_15` are free. The wire packs
  `variant` into 4 bits, so 0–15 is the hard ceiling per terrain. Paint
  cell 13 and bump `CITY_CORE_TILES` in hex-map.hpp Phase 4.

## River Channel prompts

The two prompts behind `hexRiverChannel` (2026-10-13). Both ask for a 3x2
sheet of flat-top hexes on flat magenta, which mattes cleanly: black would
merge with the ink outlines (the bunker sheet's trouble) and white with the
foam. Saved as `river-hexes01..08.png`; 01/02/07/08 came from prompt 1,
03/04/05 from prompt 2 (06 was a byte-identical copy of 05).

Shared header (both prompts):

```
A sprite sheet of six separate hexagonal game-board tiles for a post-apocalyptic
wasteland hex-map game, arranged in a 3-by-2 grid with wide, even gaps between
them on a flat, solid magenta (#FF00FF) background. Each tile is a FLAT-TOP
hexagon (pointed ends at left and right, flat edges at top and bottom), about
1.15 times wider than tall, seen straight down from directly overhead. Not
isometric, not tilted, no thickness, no slab sides, no 3D base. Each hexagon
has a crisp, clean edge against the background.
```

Prompt 1, fast current. Style: gritty 1980s comic-book ink, scratchy black
linework, cross-hatching and stipple, flat muted colour with soft washes;
slate grey-teal water, blue-black deep channels, olive-khaki mid-tones, foam in
pale bone off-white. Subject: water to all six edges, no bank, strong swirling
current. Tiles: open current; current splitting round three boulders; dead
tree snag mid-stream; rapids over a rock shelf; rusted oil drum and planks;
collapsed concrete bridge slab.

Prompt 2, marsh-river (same style, plus "like a hand-inked marsh map tile",
bleached reeds and brown-headed cattails). Land only as gravel bars, mud spits
and rock islands inside the water. Tiles: cattail clump on a gravel bar; mossy
river rocks; capsized rowboat wedged on rocks with cattails; eel-like shadows
under the surface; half-submerged house with foam on its upstream wall; a
creature's ridged back and fin surfacing beside cattails.

Both end with: no text, labels, numbers, hex outline stroke, frame, border,
grid lines, vignette, drop shadow or watermark. The generator drew the
outline stroke anyway on 01/03/07; the fit insets past it.

## Mountain prompts

The two prompts behind every `hexMountain` cell (2026-10-14). Same 3x2
magenta-sheet header as the River prompts, except the hexagon is "a flat map
tile seen from directly overhead" while the peaks inside it are drawn in
classic three-quarter map view, which is how the original cell 0 reads.
Mountains clump into ranges ringed by Ridge, so both prompts ask for rock out
to all six edges with no flat ground or foothills at the rim.

Style (both): a hand-inked fantasy-map mountain tile, thick scratchy black
ink ridgelines, gritty 1980s comic-book cross-hatching and stipple, flat
muted colour with soft washes; olive-khaki and sage-grey rock, grey-olive
shadow faces, cold slate-blue clefts, pale bone crests; no snow caps, no blue
sky. Soft, non-directional light.

Prompt 1, the common look (`hex-mountians01..04.png`, slots 0 and 1): a
dense cluster of five or six peaks; one huge broken peak with a sheer cracked
face; twin peaks with a narrow pass; a rockslide of boulders; a high saddle
with a cairn and a rag flag; a cave mouth under an overhang. 01 came out
pointy-top. The cave mouths are held back for later use.

Prompt 2, pre-war excavation, after the mountain encounters
(`hex-mountians-rare01.png`, slot 2): a timber mine adit with a tipped ore
cart; stepped quarry terraces with a broken crane; a rusted chairlift with
one dangling chair; a relay mast with a dented dish; a cracked observatory
dome; a base camp on a cliff ledge with a rope ladder.

Both end with: no text, labels, numbers, hex outline stroke, frame, border,
grid lines, vignette, drop shadow or watermark. The generator still drew sky
(magenta or white on the rare sheet, pink or pale khaki on the common ones)
and a black hex stroke on 01 and 03; see docs/hex-tile-art.md
"Mountain" for how it was filled.

## Rolling Hills prompts

The two prompts behind `hexRidge` (terrain 7: "Ridge" in game-data.js, "Rolling
Hills" in the firmware), 2026-10-14. Same 3x2 magenta-sheet header and
three-quarter-view note as the Mountain prompts. The world generator puts
hills in a ring around mountain ranges (hex-map.hpp Phase 2.5), so the prompts
ask for hills that are lower and softer than mountains, with ground out to
all six edges.

Style (both): a hand-inked fantasy-map hills tile, thick scratchy black ink
along each hill crest, gritty 1980s comic-book cross-hatching and stipple on
the shadow sides, flat muted colour with soft washes; olive-khaki and
sage-green slopes, grey-olive shadow faces, dry tan grass, pale bone crests
and exposed slag-stone; no snow, no water, no blue sky. Soft,
non-directional light. "ROLLING HILLS, the foothills that ring a mountain
range ... no sharp peaks, no cliffs, no towering rock. Ground fills every
hexagon ... no sky, no horizon, no distant range."

Prompt 1, the common look (`hex-hills01.png`, cells 0 and 2-6): overlapping
rounded hills with a footpath; one long ridgeline with a scree flank; small
knolls round a dry stony gully; a hilltop cairn with a rag flag; a
lightning-scorched outcrop with a split dead tree; tumbled dry-stone field
walls on the contours.

Prompt 2, after the ridge encounters (`hex-hills-rare01.png`, cells 1 and
7-10), "one clear feature, large enough to read when the tile is shrunk to a
thumbnail": a rusted fire tower with a padlocked box (ridge/1); a bluff ledge
with a wedged pack (ridge/2); a driverless flatbed with a tuning fork in the
dirt (ridge/3, the Perambulator); a glass observation gallery facing a bowl
(ridge/4); a concrete survey pillar; a toppled wind turbine. The user had
the glass gallery removed after integration, so ridge/4 has no tile.

Both end with: no text, labels, numbers, hex outline stroke, frame, border,
grid lines, vignette, drop shadow or watermark. Both sheets came out flat-top
with clean edges and no stroke, but every tile still painted a flat pale sky
above its horizon in spite of "no sky"; see docs/hex-tile-art.md "Rolling
Hills" for how it was filled.

Prompt 3, nature hills (`hex-hills02.png`, cells 11-16, more alternates of
the common slot). Same header and style, plus small accents of "faded mauve
heather, rust-brown bracken, dusty ochre and bone-white wildflowers", and
"wild country where nature has taken the land back ... no buildings, no
ruins, no vehicles, no people, no animals" (forage animals are their own
sprites). To fight the sky it asked for a view "looking steeply down" with
"the hillside rises all the way to the top edge of every hexagon: the top of
each tile is more hillside, never sky". Tiles: heather and bracken; a
hawthorn and birch copse in a fold; a lichen-crusted boulder tor; seeding
grass with yarrow and ragwort; gorse thickets with an animal trail; a lone
wind-sculpted tree gripping rock. It still drew sky: pale pink on five tiles
(the magenta bleeding in), cream on the sixth.
