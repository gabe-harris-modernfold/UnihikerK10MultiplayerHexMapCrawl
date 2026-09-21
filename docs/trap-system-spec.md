# Booby Trap System — spec

Companion to [world-system-spec.md](world-system-spec.md). Covers the
`trapped` hex attribute, the forced press-your-luck scene it opens, and who
gets to see it afterwards.

Status: **design agreed, nothing implemented.** Three reference encounter
files exist (`data/encounters/traps/1-3.json`) as a tone and schema sample.
No firmware, no worldgen placement, no client rendering, no mock-server or bot
mirrors. Two open questions at the bottom are still blocking.

---

## The mechanic in one paragraph

Some hexes are trapped. Walking onto one **forces** an encounter — the player
did not ask for it and cannot decline it. The scene offers two doors: back out
(Navigate) or work the mechanism (Scavenge). Working it opens a cache you can
bank or press deeper into, at a rising DN, with the whole unbanked haul
forfeited if a check fails. Leaving with nothing means the trap is still armed
and you personally can now see it on the map. Leaving with anything, or
springing it and surviving, means the trap is spent and gone for everyone.

## Why this is not just another encounter

The existing encounter engine is already a press-your-luck machine — rising
`base_risk`, `can_bank` per node, and `endEncounter(restorePoi=false)` wiping
`pendingLoot` on a terminal hazard. Traps add the three things it cannot do:

1. **You did not choose to be here.** Every other encounter is opt-in via
   `enc_start`. A trap opens from inside `movePlayer()`.
2. **The hex remembers.** Armed/spent is per-hex state that outlives the scene.
3. **Knowledge is asymmetric.** Only the survivor who escaped one can see it.

---

## The scene graph

Every trap file is the same shape. The escape door exists at the start node
only; once you are working the mechanism, `can_bank` is the way out.

```
armed            can_bank:false
  ├ "back out"        Navigate  risk 20-30  → escaped      | FAIL → hazard (light)
  └ "work it"         Scavenge  risk 30-40  → cache_1      | FAIL → hazard (heavy)

escaped          escape:true, terminal, no loot

cache_1          can_bank:true, loot tier 1
  └ "deeper"          Scavenge  risk 45-55  → cache_2      | FAIL → hazard, haul forfeited

cache_2          can_bank:true, loot tier 2   (top tier adds cache_3 at risk ~70)
```

`base_risk` → DN is `5 + risk*7/100` (`computeEncounterDN`, boot-assets.hpp),
plus up to +20 risk from `G.threatClock` and a subtraction for high LL. So
risk 30 ≈ DN 7 (72% at skill 1), risk 55 ≈ DN 8, risk 70 ≈ DN 9-10.

Failing the **escape** check is deliberately lighter than failing the
**disarm** check — you got out messy rather than got caught with your hands
inside it. That asymmetry is what makes Navigate 2 on the Guide and Scout
worth having, and it is the only reason to ever take the escape door on a
cheap trap.

## Outcomes

| Outcome | Trap after | Who can see it | Haul |
|---|---|---|---|
| Escape check passed, walked away | **armed** | the escapee only | none |
| Banked from any cache node | **spent** | n/a — gone | what you banked |
| Any check failed (trap springs) | **spent** | n/a — gone | forfeited |

Two rules fall out and are worth stating plainly:

- **Leave with nothing → it is still there.**
- **Leave with anything → it is spent.** Taking the bait is what disarms it.

Every trap hazard is `ends_encounter: true`. A sprung trap is a spent trap, so
a survivor who eats it clears the hex for the whole party. Somebody taking it
in the shins so the others can walk through is a good co-op beat and it is
free.

---

## Visibility and knowledge

Per-player. A survivor who escapes a trap can see it; nobody else can, and
witnessing from an adjacent hex does **not** count. There is no way to mark it
for the party — "you didn't tell me about the tripwire" is the point.

### The fog problem

There is no explored-memory rendering in this game. `renderHexTerrain` draws
cell content only when `visible || surveyed`, where
`visible = dist <= effectiveVR`. A hex outside vision draws nothing at all.
`WEATHER_VIS_PENALTY` drops `visR` to 0-1 in storm/chem/strangle-fog.

So a naive implementation means: escape a trap, walk three hexes, the icon is
gone; come back and you are forced into it again with no warning; and in bad
weather you never see it at all, even though you "know".

**Resolution: on escape, add the hex to `surveyedCells`.** That path already
renders outside vision, already persists, already has a dimmed
remembered-not-seen look, and is already per-player. Escaping a trap also
revealing what terrain you were standing in is narratively correct — you were
*in there*. Zero new rendering rules.

### Icon

Glyph strip index 21 in `data/img/ui_glyphs.png` (next free after
`TIRE_TRACK:20`), drawn **top-right** of the hex at the same size as the rain
glyph. `GLYPH.RAIN` owns the top-*left* at `cx - HEX_SZ * 0.48`, ~28% of hex
size — the trap must not share that corner or it vanishes under a storm, which
is exactly when you least want it hidden.

---

## Placement (worldgen)

Static at generation. No dynamic re-arming.

Traps are a city phenomenon. `hex-map.hpp` Phase 2.55 stamps city cores, and
Phase 5 already stable-partitions downtown hexes to the front of the shuffle
for urban encounters (`un >= 2` neighbour test) — trap placement biases the
same way and can reuse that partition.

| Band | Density |
|---|---|
| City core (Broken Urban, `un >= 5`) | **open — see Q1** |
| Urban outskirts / lone ruins | **open — see Q1** |
| Everywhere else | **open — see Q1** |
| Bunker tunnels | yes — a sealed bunker is exactly where someone rigs the door |

Traps and POIs are **mutually exclusive** on a hex. A trapped hex never also
carries `cell.poi`, which keeps the icon unambiguous and avoids stacking two
encounters on one tile.

Trap tier is chosen at worldgen to match how rich the hex already is — fat
hexes get nasty traps, thin ones get cheap ones. That is how "harder trap =
more generous haul" and "the trap guards the hex's normal bait" reconcile
without inventing a second loot source.

### Tunnels are currently unreachable

`index.json` has no entry for terrain 14, so `encPools[14].count` is 0 and
`tunnels.hpp` places nothing. Worse, `encLoadFile()` rejects `terrain >= 10`
outright, so even an authored tunnel pool could not load. Putting traps
underground means fixing that path first.

---

## Content library

20 traps minimum, tiered. Final counts pending Q1.

| Tier | Shape | Severity ceiling |
|---|---|---|
| Cheap | Annoying, funny, one cache | ~1 LL, a minor wound, some spoiled water |
| Mid | Two caches, real resource loss | 2-4 LL, a major wound |
| Top | Three caches, best haul | the floor-of-1 monsters |

### Tone

Gross, dark wasteland comedy, with an ironic ending. Matches the item voice
already in `items.cfg` — *Almost Water*, *Glow Flush*, *Panic Juice*, *Sweet
Oblivion*.

The dials used in the three reference files:

- Gross-out lives almost entirely in the **hazard** text.
- The irony lands in the **success** text.
- The escape node still gets a beat rather than a shrug.
- Hazard prose is longest at the top tier — the maiming is the set piece.

Reference files:

| File | Tier | Hook | Irony |
|---|---|---|---|
| `traps/1.json` | cheap | A swept doormat in a dead city; a bucket of six-year-old rendered fat on picture wire | The trap *is* the treasure — the bucket is candle-grade fuel |
| `traps/2.json` | mid | PLEASE KNOCK in ruler-straight letters; six rust-filled gouges at chest height; rebar on a garage spring | The trapper is in the pantry under their own second device, one hand on a tin |
| `traps/3.json` | top | A fitted carpet sitting 4 cm proud of the floor over forty upward-bolted dental drills | *IF YOU ARE READING THIS THE FLOOR WORKED AND I AM SORRY ABOUT THE FLOOR. IT WAS THE ONLY THING I HAD.* It isn't. The cabinet is full. |

### File ids

Contiguous id ranges per tier, declared in `index.json` so the server can pick
a tier without opening files. Ranges get assigned once the final counts are
settled; the three reference files are 1-3 and will be renumbered.

---

## Tuning: scares, not deaths

`bots/metrics.py` counts a **brush** as LL crossing from >2 down to ≤2 without
reaching 0. `TARGET_BRUSHES = 4` per session; last measured **1.84**. The game
is short on scares, not short on deaths.

**Traps maim. The wasteland kills.**

A trap hazard can leave you at 1 LL with two major wounds, your water gone and
your MP wiped — and then it lets you walk. It never takes the last point.
Everything that makes the walk home lethal already exists: thirst is 34% of
all LL lost, and a major wound is −1 on every skill plus an MP cost for the
rest of the day. A trap that leaves you crippled twenty hexes from water is a
better story than one that kills you outright, and it feeds the death into
systems that are already instrumented.

Mechanically: **an LL floor of 1 on trap hazards specifically.** Cheap to
implement, invisible to the player, and it frees the top tier to be genuinely
horrifying on every other axis.

Forced entry plus wildly varying damage plus city-dense placement is a large
lethality swing. Measure with the bot harness (`docs/bot-testing.md`) before
flashing.

---

## Schema additions

All three are backward-compatible. The reference files already use them.

| Addition | Form | Why |
|---|---|---|
| Ranged penalties | `"ll": [-2, -4]`, `"water": [-1, -2]` — scalar or 2-array | "Damage varies wildly" *within* a type, not just across types. The engine's hazard is otherwise fixed per `hazard_id`. |
| `"wound_max"` | `[minor, major]` alongside the existing `"wound"`; roll each tier between them | Same, for wounds. Avoids nesting arrays inside `wound`. |
| `"escape": true` | node flag | Ends the scene **without** marking the trap spent. Without it, a terminal node with no loot banks as a full clear (+10 score) and would wrongly clear the trap. |

Mirrors that must move together: `encounter_engine.hpp`,
`mock-server/server.js`, `bots/encounters.py`. The DN-curve comment at
`boot-assets.hpp:565` already names this set.

---

## Data layout

`HexCell` gains a `trap` byte:

| Bits | Meaning |
|---|---|
| 7 | armed |
| 0-5 | known-by-player bitmask (`MAX_PLAYERS` = 6), same idiom as `footprints` |
| 6 | spare |

Cost: 4275 B of PSRAM on the surface map, plus the tunnel board.
`MAP_BYTES` changes, so **`SAVE_VERSION` 17 → 18** and a one-shot world regen —
the same precedent as v15→v16 and v16→v17.

The alternative — stealing the 7 spare bits in `tireTrack` (a bool in a whole
`uint8_t`) — fits armed + the 6-bit mask exactly with zero headroom and no
version bump. Rejected: no room to grow, and the field would then mean two
unrelated things.

## Wire protocol

The 3-byte cell has no room. `TT` bits 4-5 are the only spare ones, and
`map-decoder.js:21` masks terrain with `0x3F`, so they are not actually free on
the client today. Also watch terrain 15 + all four high bits = `0xFF`, which
collides with the fog sentinel.

Because visibility is per-player, `encodeCell()` needs a `pid` parameter and
emits the trap bit only when that player's mask bit is set. All three callers
(`sendSync`, `buildVisDisk`, `buildSurveyDisk`) already know the pid, so this
is mechanical.

If the bit cannot be found, the fallback is a sparse per-player
`"trap":[[q,r]]` array, same idiom as `appendFireArray` / `appendFloodArray` —
but that would need unicast rather than riding `broadcastState`.

---

## Forced entry

New path. Today an encounter only opens when the client sends `enc_start` for
the hex it is standing on. A trap opens from inside `movePlayer()`: the server
pushes `enc_path` + `EVT_ENC_START` unprompted and the client opens the panel
on its own.

Cases it has to survive:

- Arriving with 0 MP.
- Arriving via `tunnelStepDown()` / `tunnelStepUp()`.
- Dawn or a disconnect firing mid-scene. `endEncounter(restorePoi=true)`
  restores a POI; a trap needs its own restore path (armed, not spent).
- Two survivors stepping onto the same trapped hex. `enc_start`'s `claimed`
  check has no equivalent on the forced path.
- Terrain conversion under an armed trap (flash flood dry→Marsh→Flooded
  District, fire, quake).
- The caravan and the Creeping Doom crossing a trapped hex. Default: ignore
  them, traps are player-triggered only.

**Resource collection happens first.** `movePlayer()` calls
`collectResource()` before anything else, so you grab the bottle on your way
into the punji pit. That ordering is intentional.

---

## Open questions

**Q1 — density.** "Increase the trap count by 3 in each category" maps onto two
different questions and they lead to different work:

- *Density reading*: city-core 15% / outskirts 5% / elsewhere 1%, +3 points
  each → **18% / 8% / 4%**. Roughly one trapped hex in five downtown, which
  with forced entry makes crossing a city a gauntlet of 3-4 forced scenes.
- *Type-count reading*: tiers 6/9/5 + 3 each → **9/12/8 = 29 trap files**.
  Partly contradicts the separate "your defaults are good" on the 6/9/5 spread.

**Q2 — `surveyedCells` for escaped traps.** Route escaped-trap hexes through
`surveyedCells` so the icon persists (recommended, see *The fog problem*), or
accept that the icon is a short-range proximity warning that disappears in
weather?

---

## Still to do

1. Resolve Q1 and Q2.
2. Schema additions in `encounter_engine.hpp` + mock + bots.
3. `HexCell.trap`, `SAVE_VERSION` 18, `encodeCell(pid)`, decoder mask to `0x0F`.
4. Worldgen placement phase, tier-by-richness, POI exclusion.
5. Forced-entry path in `movePlayer()` and the client's unprompted panel open.
6. Glyph 21 art, top-right draw, `surveyedCells` hook.
7. Author the remaining traps to the agreed tier counts.
8. Fix the tunnel encounter pool (`index.json` terrain 14, `encLoadFile`'s
   `terrain >= 10` rejection) if traps go underground.
9. Bot-harness pass on brush rate before flashing.
