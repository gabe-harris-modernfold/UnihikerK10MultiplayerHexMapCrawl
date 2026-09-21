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
| 7  | Ridge            | 1 real + 3 placeholder | `#1E1A12` | **priority** |
| 8  | Mountain         | 1 real + 3 placeholder | `#14141C` | **priority** |
| 9  | Settlement       | 8  | `#1A1206` | done |
| 10 | Nuke Crater      | 2 real + 2 placeholder | `#0A0E04` | thin |
| 11 | River Channel    | 0  | `#0B1E0F` | **see note below** |

**Slots marked "placeholder" are flat labelled tiles** from
`scripts/gen_missing_tiles.py` — they print their own filename on the hex, so
when one shows up on the map you can read straight off it which file to paint.
Replacing one is a drop-in: same name, same folder, then re-run
`scripts/optimize_art.py --apply` and bump `CACHE` in `data/sw.js`.

`terrainVariantCount[]` is derived at boot by scanning `data/img/` for
`hex<Name><N>.png` and taking the highest `N` found ([game-server.hpp](../../game-server.hpp)
`setupVariantCounts()`) — so new files just need to continue the numbering
with no gaps; nothing else to register.

**River Channel is currently drawn as an animated ripple effect
(`drawRiverRipples()` in [renderer.js](../renderer.js)), not a static image** —
that's why it has zero files, and it's likely deliberate (fast-moving water
reads better animated than as a painted still). Adding `hexRiverChannel0.png`
would work — the renderer falls back to art first, ripples only when no image
is loaded — but check with yourself before spending generations on it; this
doc includes a subject for it in case you decide you want one anyway.

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

Generate large (768–1024px on the long edge) for clean linework, then
downscale. Export as PNG with transparency and run it through
`python scripts/optimize_art.py --apply`, which re-encodes to palette PNG and
caps the long edge at 256 — every file under `data/img/*/` loads into the
PSRAM image cache at boot (`MAX_IMG_CACHE = 160` in the .ino).

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
| 11 | River Channel *(optional — see note above)* | Fast dark river current cutting a channel, whitecap ripples breaking around submerged rocks | a snagged branch caught mid-current; a half-collapsed footbridge; a stranded rowboat wedged against the bank |
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
  rows above and overwrite the same filenames.
- Replacing a tile under a filename that already shipped? **Bump `CACHE` in
  `data/sw.js`** — the service worker caches `/img/*` cache-first, forever, so
  browsers will otherwise keep serving the old image.
- Keep new files' numbering contiguous from the current max (e.g. Ridge's
  next file is `hexRidge1.png`, not `hexRidge2.png`) — the boot scan takes
  `max(index) + 1` as the count, so a gap silently wastes a slot rather than
  erroring.
- Hard ceiling: `cell.variant` is packed into 4 bits on the wire
  (`variant & 0x0F` in hex-map.hpp), so only indices **0–15** are ever
  addressable per terrain regardless of how many files exist.
- Pinned art (a fixed image on one specific hex, outside the numbered variant
  pools) is keyed `terrain_variant` in `POI_ART` in [../engine.js](../engine.js)
  and is **not** picked up by the boot variant scan. See "Pinned art" below.
- Item icons (different doc, different style — pixel art, not painterly) are
  in [items/ICON_PROMPTS.md](items/ICON_PROMPTS.md).

## Pinned art

A hex whose `variant` the firmware pins past the end of its terrain's counted
pool. The renderer looks the pair up in `POI_ART` first and only falls back to
the pool when there is no entry, so these files are addressed by name and the
filename must **not** start with `hex<TerrainImgName>` — the boot scan would
otherwise fold it into that terrain's numbered variants.

| Key | File | Pinned by | Subject |
|----|----|----|----|
| `0_10` | `poi_jacks_chopper.png` | hex-map.hpp Phase 5.5 — the scrub hex holding `scrub/19.json` | named landmark, already shipped |
| `4_10`, `4_11`, `4_12` | *(not yet drawn)* `poi_city_core0-2.png` | hex-map.hpp Phase 4 — any Broken Urban hex with 5+ urban neighbours, one of the three picked at random | **Dense downtown core.** Standing multi-storey hab-block shells rather than ground rubble: walls still up three or four floors, window rows blown out, a street canyon running between them in deep shadow. It must read as *taller and more intact* than `hexBrokenUrban0-9` at a glance — its whole job is to mark the middle of a real city against the fringe rubble around it. Swap-ins for extra variants: a collapsed skybridge between two towers; a toppled crane leaning across the canyon; a gutted parking structure; a billboard frame stripped to its lattice. |

Notes on the downtown core tiles:

- **Draw all three, or none.** With no `POI_ART` entries every pin falls back
  to ordinary rubble and the map stays coherent; with one entry a downtown
  street mixes one core tile against plain rubble and reads as a bug. A
  simulated 200 worlds pins a median of **11** core hexes, clustered inside
  the same few cities, which is why one tile is not enough.
- Phase 4 pins them only where a city has a genuinely dense interior, so a
  typical world marks ~11 hexes and small townships mark none at all.
- Until the files exist the renderer silently falls back to the numbered
  Broken Urban pool (`poiArtFor()` returns undefined, then
  `variant % poolLength`), so the firmware side is already live and harmless.
- Do **not** name them `hexBrokenUrban10.png` and up. That pushes
  `terrainVariantCount[4]` past 10, `pickVariant()` could then hand out 10-12
  on its own, and every ordinary rubble hex could roll a downtown tile.
- Adding a fourth? Sentinels `4_13`..`4_15` are free (the wire packs
  `variant` into 4 bits, so 0-15 is the hard ceiling per terrain) — add the
  `POI_ART` entry and bump `CITY_CORE_TILES` in hex-map.hpp Phase 4.
