# Item Icon — Image-Gen Prompts

Companion to [IMAGES_NEEDED.md](IMAGES_NEEDED.md) (that file's item list is
stale — it predates several item renames/additions in `data/items.cfg`).
This covers all **64** current items (ids 1–64, no gaps). None of the
per-item `icon_<id>.png` files exist yet — only the 4 generic category
fallbacks (`icon_gulpable/bolton/salvage/relic.png`) and the skull
placeholder do, all procedurally drawn by `scripts/gen_pixel_glyphs.py`,
not art.

## How to use this

1. Copy **STYLE** once.
2. For each item, copy its **Subject** line into the `{SUBJECT}` slot.
3. Generate at a large size (e.g. 512×512) with pixel-art constraints, then
   downscale with nearest-neighbour (no smoothing) to 32×32 and export as
   PNG with transparency. Save as `data/img/items/icon_<id>.png`.
4. Generate one item at a time — text-to-image models can't reliably
   produce 64 distinct, correctly-labeled icons in a single grid. If you do
   want a contact sheet for quick style comparisons, batch 6–10 *same-category*
   subjects into one STYLE prompt listed as "a grid of separate icons: 1) …
   2) …" and crop afterward, but treat that as a preview pass, not final art.
5. Every file under `data/img/*/` loads into the PSRAM image cache at boot
   (`MAX_IMG_CACHE = 100` in the .ino) — keep exports small/optimized PNGs.

## STYLE (prefix — use for every item)

```
Pixel art game icon, 32x32 native resolution, crisp hard pixel edges, no
anti-aliasing, no blur. Single object, centered, a few pixels of padding,
isolated on a fully transparent background. Post-apocalyptic wasteland
scavenger aesthetic: weathered, grimy, patched with tape/wire/rust,
hand-me-down. Limited palette of 8–12 colors per icon, flat cel-shading
with one hard shadow tone and one highlight tone, no gradients. Strong
high-contrast silhouette that still reads clearly at thumbnail size. The
item can use desaturated rust/bone/olive/steel tones — it does not need to
be monochrome — but should sit naturally against a near-black UI
(background #080604) next to amber accent colors (#E8A828 bright amber,
#C07818 mid amber, #6A3008 rust). No text, no watermark, no signature, no
drop shadow, no background scene, no border.

Subject: {SUBJECT}
```

## NEGATIVE (suffix — for tools that take a separate negative prompt, e.g. Stable Diffusion)

```
blurry, soft gradient, anti-aliasing, photorealistic, 3D render, glossy,
background scenery, multiple objects, text, watermark, signature, frame,
border, isometric room, full scene
```

## Worked example (item 1)

```
Pixel art game icon, 32x32 native resolution, crisp hard pixel edges, no
anti-aliasing, no blur. Single object, centered, a few pixels of padding,
isolated on a fully transparent background. Post-apocalyptic wasteland
scavenger aesthetic: weathered, grimy, patched with tape/wire/rust,
hand-me-down. Limited palette of 8–12 colors per icon, flat cel-shading
with one hard shadow tone and one highlight tone, no gradients. Strong
high-contrast silhouette that still reads clearly at thumbnail size. The
item can use desaturated rust/bone/olive/steel tones — it does not need to
be monochrome — but should sit naturally against a near-black UI
(background #080604) next to amber accent colors (#E8A828 bright amber,
#C07818 mid amber, #6A3008 rust). No text, no watermark, no signature, no
drop shadow, no background scene, no border.

Subject: a square adhesive trauma patch, gauze pad taped at each corner,
a dark dried bloodstain soaking through the center, one corner peeling up.
```

---

## Subjects, by category (order matches `items.cfg`)

### Consumables

| ID | File | Item | Subject |
|----|------|------|---------|
| 1 | icon_1.png | Trauma Patch | Square adhesive gauze bandage, dark dried bloodstain soaking through the center, one corner peeling up |
| 2 | icon_2.png | Mystery Rations | Dented tin can, plain faded label reading only "FOOD", rusted rim |
| 3 | icon_3.png | Almost Water | Sealed translucent water pouch, small clipped-on filter cartridge, condensation droplets |
| 4 | icon_4.png | Glow Flush | Auto-injector syringe filled with sickly green fluid, radiation-trefoil sticker on the barrel |
| 5 | icon_5.png | Panic Juice | Glass vial of red stimulant fluid in a spring-loaded injector, hazard-striped label, hairline crack |
| 6 | icon_6.png | Sweet Oblivion | Small brown glass tincture bottle, cork stopper, hand-scrawled paper label |
| 7 | icon_7.png | Calorie Brick | Dense foil-wrapped ration bar, brick-shaped, one bite torn out of the corner |
| 8 | icon_8.png | Screaming Spike | Chunky adrenaline auto-injector, bright red plunger cap, exposed needle tip |
| 9 | icon_9.png | Anti-Rot Kit | Small rusted first-aid tin box, faded cross painted on the lid, dented corners |
| 10 | icon_10.png | Bright Bad Idea | Lit road flare, sparking red glow at the tip, thin smoke wisp |
| 30 | icon_30.png | Sour Cream Tub | Plastic tub, peeling label, a spoon stuck upright in the contents |
| 31 | icon_31.png | Trippy Juice | Glowing neon-orange vial, swirling liquid, biohazard tape wrapped around the neck |
| 44 | icon_44.png | Jar of Sweats | Murky glass jar of brownish liquid, hand-written label, rusted screw lid |
| 50 | icon_50.png | Uranium Candy | Twisted hard candy in a yellow-green foil wrapper, faint radioactive glow, tiny trefoil printed on the twist |
| 52 | icon_52.png | Gutter Broth | Dented tin cup of grey murky broth, thin steam wisp rising |
| 53 | icon_53.png | Sock Puppet Bandage | A boiled sock repurposed as a wound wrap, held with a bent safety pin |
| 54 | icon_54.png | Bile Flare | Corked glass jar of sickly yellow-green bile, faint fumes leaking from the seal |
| 55 | icon_55.png | Cricket Paste | Small pot of grey-brown paste, one cricket leg poking out of the surface |
| 56 | icon_56.png | Tooth Whiskey | Flask of electric-amber liquid, jagged cracked-tooth shape etched on the label |
| 57 | icon_57.png | Squelch Bandage | Wet-wrapped bandage roll, faint sickly green stain seeping through |
| 58 | icon_58.png | Nostril Salts | Small cracked-open vial of crystalline salts, thin vapor lines rising |
| 59 | icon_59.png | Marrow Jelly | Glass jar of pale jelly with a visible bone fragment suspended inside |
| 60 | icon_60.png | Static Chew | Twisted wad of bare copper wire chewed like gum, tiny spark lines around it |
| 61 | icon_61.png | Blister Balm | Small tin of bruise-purple ointment, open lid, fingerprint smear across the surface |
| 62 | icon_62.png | Panic Dart | Whittled dart with a barbed tip and a fletching of torn cloth, faint motion lines |

### Equipment — worn (head / body / hand / feet)

| ID | File | Item | Subject |
|----|------|------|---------|
| 11 | icon_11.png | Dent Absorber | Sleeveless vest with cracked ceramic plates stitched over scavenged padding |
| 12 | icon_12.png | Glow Suit | Full yellow hazmat suit with sealed hood and round goggle lenses |
| 13 | icon_13.png | Wheeze Filter | Cloth-wrapped respirator mask with two round filter canisters |
| 14 | icon_14.png | Dark Goggles | Military-surplus goggles, one lens cracked, worn elastic strap |
| 15 | icon_15.png | Trudge Stompers | Heavy steel-toed work boot, sole patched with duct tape |
| 16 | icon_16.png | Hoarder's Rig | Canvas backpack harness covered in straps, pouches, and carabiner clips |
| 19 | icon_19.png | Vertical Regret | Coiled climbing rope with a worn carabiner clipped through it |
| 20 | icon_20.png | Doom Clicker | Vintage civil-defense Geiger counter, dial face and wand probe, chipped yellow paint |
| 27 | icon_27.png | Portable Forge | Compact anvil-and-bellows rig built from a car muffler, glowing coals inside |
| 28 | icon_28.png | Fishing Pole | Telescoping rod bent from scavenged pipe, line and hook dangling, small bobber |
| 29 | icon_29.png | Compound Bow | Pre-war compound bow, string taut, arrow nocked |
| 32 | icon_32.png | Fire Starter | Flint striker and magnesium rod, small spark shower |
| 33 | icon_33.png | Intimidate Mask | Carved resin mask with jagged teeth and deep hollow eye holes |
| 34 | icon_34.png | Bear Skin Cape | Thick fur cape with claws still attached at the collar clasp |
| 40 | icon_40.png | Shock Knuckles | Knuckle-duster wrapped in coiled wire, faint blue electric arc between the studs |
| 41 | icon_41.png | Squatch Sliprs | Oversized shaggy fur slipper-boots |
| 42 | icon_42.png | Knife-Wrench | Welded hybrid tool, half wrench jaw, half knife blade |
| 45 | icon_45.png | Glow Dentures | Full set of false teeth with a faint blue radioactive glow |
| 47 | icon_47.png | Lead Snuggie | Heavy quilted hooded blanket-poncho with lead-grey lined panels |
| 64 | icon_64.png | Backpack | Bulging patched-canvas rucksack, bedroll lashed under the flap, mismatched webbing straps |

### Equipment — vehicle

| ID | File | Item | Subject |
|----|------|------|---------|
| 17 | icon_17.png | Rust Rocket | Rusted motor scooter, patched fuel tank, thin exhaust smoke |
| 18 | icon_18.png | Floaty Disaster | Crude raft of lashed oil drums and mismatched planks |
| 26 | icon_26.png | Motorbike | Beat-up chopper-style motorbike with mismatched welded parts, exhaust smoke |
| 63 | icon_63.png | Raft | Bound driftwood-log raft, cinched tight with knotted salvaged cord |

### Materials

| ID | File | Item | Subject |
|----|------|------|---------|
| 21 | icon_21.png | Useful Garbage | Jumbled pile of bent scrap metal, nails, and wire |
| 22 | icon_22.png | Sparky Bits | Tangle of copper wire, a circuit-board fragment, and a small battery |
| 23 | icon_23.png | Burn Juice Can | Dented jerry can with a fuel-splash icon and rust streaks |
| 24 | icon_24.png | Expired Meds | Orange pill bottle, faded label, a few pills spilled beside it |
| 35 | icon_35.png | Loose Tentacle | Severed rubbery tentacle with suckers visible, damp sheen |
| 36 | icon_36.png | Clean Underwear | Neatly folded white underwear, oddly pristine, tiny sparkle |
| 39 | icon_39.png | Irradiated Fur | Tuft of matted fur, faint green glow, scorched tips |
| 46 | icon_46.png | Sonic Spines | Cluster of barbed cactus spines, faint sound-wave arcs radiating out |
| 48 | icon_48.png | Corrosive Syrup | Bottle of thick neon-green syrup, a drip sizzling where it touches metal |
| 49 | icon_49.png | Crater Deed | Official-looking deed paper with a wax seal, singed edges |

### Key / story items

| ID | File | Item | Subject |
|----|------|------|---------|
| 25 | icon_25.png | Doomed Diary | Worn leather journal, ash-stained pages, tied shut with frayed string |
| 37 | icon_37.png | Cursed Device | Small black obelisk covered in faint glowing alien symbols |
| 38 | icon_38.png | Pre-War Net Map | Cracked data-slate tablet, screen glowing with a map grid |
| 43 | icon_43.png | Valid License | Laminated ID badge with an official stamp and a blank photo silhouette |
| 51 | icon_51.png | Sticky Note | Small curled yellow sticky note with faded ballpoint handwriting |

---

## Notes

- Subjects lean on each item's flavor text in `data/game-data.js` / `data/items.cfg`
  so the icon reads as *that specific joke*, not a generic potion/gear
  placeholder — keep that specificity if you tweak wording.
- `icon_18` (Floaty Disaster) and `icon_63` (Raft) are deliberately similar
  silhouettes (same slot, same job) — make the raft visibly cruder/smaller
  so they're still distinguishable in the inventory grid at 26–28px.
- Same caution for `icon_16` (Hoarder's Rig) and `icon_64` (Backpack): both
  are body-slot carry gear worth +4 slots. Draw the Rig as a strap-and-pouch
  *harness* worn on the chest and the Backpack as a single closed-flap sack,
  so the silhouettes read differently at icon size.
- Once icons exist, `item_<id>.png` (128×128 full illustrations) can reuse
  the same Subject lines with "more surface detail, dirtier, worn" per the
  Art Style Notes in `IMAGES_NEEDED.md` — that's a separate follow-up pass.
