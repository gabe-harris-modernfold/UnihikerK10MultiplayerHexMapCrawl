# Bunker Tunnel System — spec

Companion to [world-system-spec.md](world-system-spec.md). Covers the second
hex board, how a player gets onto it, and what it costs them.

Status: **steps 2-4 of 5 complete** (terrain, board, generation, persistence,
movement, wire protocol, client rendering), plus the first piece of step 5:
**shelter and bad air** (below). What is still outstanding from the danger
layer is forced encounters, the flare burn and cave-ins.

---

## Client

`data/tunnel-board.js` (registered in `data/web-assets.json` right after
`game-data.js`) owns which board the client is looking at. Nothing outside it
touches `gameMap` / `MAP_COLS` / `MAP_ROWS` for board access any more:

| Accessor | Purpose |
|---|---|
| `boardCells()` / `boardCols()` / `boardRows()` | the grid currently being drawn |
| `boardWraps()` | true on the surface, false in the tunnels |
| `boardNorm(q, r)` | wraps on the surface, bounds-checks underground; `null` = off-board |
| `boardCell(q, r)` | cell or `null` |
| `boardDist(...)` | `hexDistWrap` on the surface, plain `hexDist` underground |
| `playerViewPos(i)` | where to draw player *i*, or `null` if they are on the other board |
| `playerIsBelowUs(i)` | they are underground and we are not |
| `atHatch()` | the local player is on terrain 12/13 on either board |

The two boards differ in more than size — the surface is a torus and the
tunnels are walled — so every wrap calculation had to become a board question.
`boardNorm()` returning `null` is what lets the renderer simply skip off-board
cells; the page background already reads as solid rock.

### Rendering

- The three view loops (terrain, labels, POI outlines) normalise through
  `boardNorm()` and skip `null`.
- `lerpPlayerPositions()` follows `playerViewPos()`, snaps across a board
  change rather than sliding a marker across the map, and only unwraps on the
  surface.
- `buildCamera()`'s `meAct` — the hex vision falloff measures from — is the
  board position, not the surface pin.
- `closestWrapCoords()` short-circuits underground.
- A `renderUnderground()` layer draws a cold vignette under the time-of-day
  tint, so the tunnels never read as "the surface map at night". The day/night
  cycle still runs: your MP budget does not stop because you cannot see the sky.
- The caravan, Doom, weather, quake and ash layers are all skipped underground —
  their coordinates belong to the other board.
- Per-hex labels fall back to row order underground; `hexLabel` is a
  permutation sized for the surface grid.

### The "gone below" marker

A survivor underground is drawn on the hatch they used (their `q`/`r` is still
pinned there) as a violet disc with a downward chevron. It is drawn **last**,
over the character icons, and offset to the lower edge of the hex — the hatch
is usually the hex another survivor is standing on, and the first version was
completely hidden under their portrait.

### HUD and controls

- **There is no descend/ascend control.** Stepping onto a Bunker Entrance or
  Vent Shaft *is* the transition, server-side, as the last act of that move
  (`tunnelStepDown()` / `tunnelStepUp()` in `tunnels.hpp`). The D-pad is the
  only thing that crosses boards, so the client needs no tunnel-specific
  control at all. What stops it looping is that you *arrive* on the paired
  cell by transition, not by a move: standing on a hatch does nothing, and the
  next step off it is an ordinary one.
- `#hud-depth` **replaces** the weather chip rather than sitting beside it:
  underground the weather genuinely does not apply, and `#hud-status` is a
  flex row with `overflow: hidden` that already squeezes the weather chip to a
  few pixels on a phone. The chip shrinks and ellipsises like its neighbours.
- Direction buttons need no tunnel-specific code — `vm` from
  `computeValidMoves()` already greys out rock and the board edge.

---

## Wire protocol

Dispatch is the string-keyed `strncmp` chain in `network-handlers.hpp`.

**In** — no new message at all; movement carries the whole system:

| Message | Meaning |
|---|---|
| `{"t":"m","d":N}` | `movePlayer()` routes to `moveTunnel()` on `p.depth`, so the existing D-pad, keyboard and swipe handlers work underground for free — *and* a step that lands on a hatch or a shaft crosses boards before the reply is built |

`handleMsg_move()` snapshots `p.depth` either side of `movePlayer()`: that is
what tells it to send a `tsync` (0 → 1 only; the client already has `G.map`)
and which board to build the vis disk against. Building it from `p.q`/`p.r`
while a player is below reveals the *surface* around their hatch and leaves
the tunnel board fogged — every step then lands on a cell the client has never
seen, which `applyHexFill()` paints flat black.

**Out:**

| Message | Meaning |
|---|---|
| `{"t":"tsync","cols":16,"rows":10,"vr":N,"q":Q,"r":R,"map":"…"}` | the whole fogged tunnel board, 960 hex chars. Sent on descend, and again from `sendSync()` when someone reconnects while already underground |
| `{"t":"vis","dp":1,…}` | vis disk gains a `dp` field naming the board. Omitted at depth 0, so surface traffic is byte-identical to before |
| `{"t":"s",…,"dp":D,"tq":Q,"tr":R}` | per-player depth and tunnel position on the state broadcast (~14 B/player; buffer 3072 → 3328) |
| `{"t":"ev","k":"tun_in"/"tun_out","pid":P,"q":Q,"r":R,"hatch":H,"mp":M}` | `q`/`r` are the **surface** hatch in both cases, so the client can draw a "went below here" marker without needing the tunnel board |
| `{"t":"ev","k":"mv"/"col"/"col_fail",…,"dp":D}` | every event that names a hex now carries the board |

`GameEvent` gained a `depth` byte for exactly that last reason: without it the
client would patch a surface cell using a tunnel coordinate.

## Movement

`moveTunnel()` in `survival_state.hpp` is a **sibling** of `movePlayer()`, not a
parameterisation — underground the rules genuinely differ. Dropped: weather
penalty (`WEATHER_INTENSITY` is zero down there anyway), flood penalty, tire
tracks, radiation check. Kept: `canEnterTerrain()`, the `MOVE_CD_MS * mc`
cooldown, MP deduction, footprints, first-visit score, `collectResource()` and
the `EVT_MOVE` shape — which is why the client needs no new move path.

`computeValidMoves()` takes the tunnel branch at depth 1, so Collapsed Tunnel
(MC 255) and the board edge both grey out the direction buttons with no
tunnel-specific client code.

## Crossing is never refused

Both transitions charge their cost clamped at 0 rather than refusing when it
cannot be afforded. The step has already landed by the time `tunnelStepDown()`
/ `tunnelStepUp()` run, so a refusal would strand the survivor on the hatch
with no feedback; and on the way up, being unable to climb out means dying in
the bad air with no legal action while it drains LL every tick. **You can
always cross — you may just arrive spent.** Bunker Entrance costs 1 MP, Vent
Shaft costs `VENT_ASCEND_MP` (2) to climb out — the one mechanical difference
between the two entrance types.

A separate sweep in `tickGame()` surfaces anyone who is downed while
underground. That lives in one place rather than at each of the eight
`EVT_DOWNED` sites, so a ninth cannot forget it.

## Vision underground

A separate branch in `playerVisParams()`, not the surface formula with
modifiers — terrain vision, weather and smoke are all meaningless in a corridor.

| | radius |
|---|---|
| base | 1 (your hex plus one ring) |
| carrying ≥1 Bile Flare (item 54) | +1 |
| Scout (archetype 4) | +1 (their surface bonus is +2; down here it is tighter) |

`hasTunnelLight()` scans `invType[]`, **not** `equip[]` — the flare is
`slot = none`, so it can only ever be carried.

## Cross-board leaks that had to be closed

`p.q`/`r` staying pinned at the hatch is what makes the whole design cheap, but
it means a surface player standing on that hatch shares coordinates with a
teammate far below. Every co-location test therefore compares depth too:

- `samehex()` — the single helper both trade paths use
- `groupVisionBonus()` — otherwise a phantom +1 vision per descended teammate
- `countConnectedPlayersOn()` — settlement founding needs three survivors
  actually stood there
- `handleMsg_enc_start()` — bounds, cell lookup, co-location and the
  "another survivor is already inside" check are all board-aware, and
  `ActiveEncounter` gained a `depth` field so `endEncounter(restorePoi)` puts a
  tunnel POI back on the tunnel board
- the world system (fire, lightning, flood seeding, the Doom, the caravan) skips
  `depth != 0` players outright
- the chem-storm and Strangle Fog per-tick hazards skip them too

## Actions underground

`handleAction()` reads terrain off whichever board the player is on, then
refuses three actions explicitly: SHELTER, CRAFT and SURVEY. FORAGE
self-blocks (`TERRAIN_FORAGE_DN[14]` is 0) and TREAT self-gates to Medics, so
neither needs an entry.

**SURVEY is the one that actually matters** — `doSurvey()` writes
`p.surveyedMap[]`, which is sized for the 75×57 surface map and would be indexed
with tunnel coordinates. `data/game-data.js`'s `actAvailable()` mirrors the same
three so the action menu agrees with the server.

**REST was a fourth and is not any more** — see the next section. Refusing it
was also a live desync: `#fab-rest-btn` is not depth-gated, so the client lit
its RESTING state while the server never set `p.resting`, and the day would
not collapse.

## Shelter and bad air

A bunker corridor is **cover**. `dawnUpkeep()` treats `depth != 0` as covered
outright, so the exposure tick — `EXPOSURE_BITE` (2) LL on the 96% of the
surface map with SV < 2 — does not run underground at all.

That was also a bug fix, not only a rule. `q`/`r` stay pinned to the hatch
while you are below (the invariant at the top of this doc), so the check was
reading the *surface* hex: `TERRAIN_SV[12]` is 2 and `TERRAIN_SV[13]` is 0, so
a night below a Bunker Entrance came out sheltered and a night below a Vent
Shaft took the full bite, with both survivors in the same corridor and neither
able to build a shelter to fix it.

What the tunnels charge instead is **bad air**: a REST at depth 1 has a
`TUNNEL_REST_LL_PCT` (30%) chance of costing 1 LL, rolled in the same dawn
tick. Three properties make that a trade rather than a tax:

- **It nets against the rest-heal.** Bad air goes into `llDelta` with the
  food/water losses and the rest recovery, so a survivor who qualifies for the
  heal (F ≥ 2 and W ≥ 2, or a Settlement) absorbs it and still comes out
  ahead. It shows as a drop only when there was nothing to heal with.
- **It is NOT floored at LL 1.** Exposure is, deliberately — it is a silent
  tick on nearly every hex with nothing to react to. Bad air is a roll you
  opted into by bedding down below, it rides the dawn event as `"air"` so it
  reads as something that happened, and it can down you. Flooring it too would
  make the tunnels the one place attrition cannot kill, which is the absorbing
  state at LL 1 that `survival_state.hpp`'s rest-recovery note exists to keep
  out. Dying below is not a soft-lock: `tickGame()`'s downed sweep surfaces
  them through `surfacePlayer()`.
- **A built shelter still beats it.** Shelter protection zeroes the whole dawn
  loss and cannot suffocate anyone; the depth guard on that clause is what
  stops a shelter pitched on the hatch from cancelling a roll far beneath it.
  The ranking is: built shelter > bunker (0.3 LL expected) > open ground
  (2 LL certain).

`doRest()`'s Fire Starter shelter upgrade is surface-only for the same
pinned-coordinate reason. The mock mirrors all of this in `dawnUpkeepAll()`,
`bots/config.py` carries `TUNNEL_REST_LL_PCT`, and `bots/causes.py` attributes
deaths to `bad air` off the new wire field.

## The voice in the corridor

Bad air is what the tunnels cost. This is what they *say*. A survivor camped
underground on a hex with nothing built on it gets needled about it every
`TUNNEL_TAUNT_COOLDOWN` (8) world ticks — ~2 real minutes, matching
`DOOM_TAUNT_COOLDOWN`, so two or three lines per game-day below and ~10 days
before the 30-line table can repeat.

**The joke is that it is wrong.** A bunker is good shelter; the lines imply,
dryly and without ever being actionable, that bedding down there was a
mistake. If any of them ever start reading as advice, they have drifted.

`tickTunnelTaunts()` (`tunnels.hpp`) runs in `tickGame()`'s world-tick slice
right after `tickDoomAudio()`, and copies the Doom's taunt machinery
deliberately:

- **Only a line index goes on the wire.** The 30 wordings live in
  `TUNNEL_TAUNTS` (`data/game-data.js`) and the client reduces the index
  modulo its own table, so lines can be added or reworded without reflashing.
- **Nothing reaches the K10 chronicle.** That book is for things that
  happened; this is a mood, and at one line per survivor every two minutes it
  would bury the log.
- **It is unicast**, which is the one place it differs from the Doom.
  `EVT_DOOM_TAUNT` is broadcast because the party watching it single someone
  out *is* the effect; this is private second-guessing, and with three
  survivors underground a broadcast would be three streams of somebody else's
  doubt in everyone's log.
- **`tunnelTauntTicks[]` counts up, not down**, so zero means "not long
  enough yet". Silence is then the default at a cold boot, a restored save
  and a reconnect alike, with no init call to forget at any of those sites —
  a down-counter needed one and spoke on world tick one to anyone restored at
  depth 1. `tunnelStepDown()` resets it, so a dive gets a full cooldown of
  quiet first; arriving and being needled in the same breath reads as the
  game refusing the descent rather than as a mood.
- It stays quiet while the survivor is downed or inside an encounter (a scene
  has its own prose), and the shelter test is written out rather than folded
  into `p.depth` even though it is always true today — building something is
  the one thing that should silence it if underground shelters ever land.

The toast has its own voice class, `toast-tunnel` (`data/style.css`): italic
and lower-case like the Doom's, but cold concrete instead of arterial red and
with no breathing glow. The Doom is a thing approaching; this is only a mood,
and it must not pull the eye the way a real threat does — while still being
readable against a near-black tunnel board, which the first pass was not.

---

## Why

The surface map is 75×57 and every crossing is paid one hex at a time out of a
daily MP budget. Impassable terrain (Nuke Crater, River Channel) can wall off
whole regions. The tunnels are a second route: fast in hexes, expensive in MP
and survival pressure, and dangerous in ways the surface is not.

Measured over 150 generated worlds, comparing surface distance against the
actual corridor path between the matching shafts:

| Surface gap between hatches | Surface MP (MC ≈1.6) | Tunnel MP (2/step, +2 to descend/ascend) | Saving |
|---|---|---|---|
| 10-19 hexes | 26.4 | 14.9 | 44% |
| 20-29 hexes | 39.6 | 17.0 | 57% |
| 30-39 hexes | 54.7 | 20.9 | 62% |
| 40-49 hexes | 65.6 | 21.7 | 67% |

The saving scales with distance, which is the intended shape: the tunnels are
not worth it for a short hop and decisively worth it for a continental
crossing. Worst corridor path observed was 23 hexes / 48 MP — enough to strand
someone who goes in unprepared.

---

## The load-bearing invariant

**`Player.q`/`r` always index `G.map`.** While a player is underground their
`q`/`r` stay pinned to the hatch they descended through, and `G.tunnel` is
indexed by `tq`/`tr` instead.

This is what lets weather, the world system (doom, fire, flood, caravan),
vision, actions, the LCD minimap and `broadcastState` keep reading
`G.map[p.r][p.q]` completely unchanged — they just skip players with
`depth != 0`, the same one-line gate as the existing
`if (encounters[pid].active) return;` idiom.

Break this and every one of those becomes an out-of-bounds read into a 16×10
array.

---

## Terrain (`NUM_TERRAIN` 12 → 16)

| idx | Name | Board | MC | SV | Notes |
|---|---|---|---|---|---|
| 12 | Bunker Entrance | surface **and** tunnel | 1 | 2 | Blast-door stairwell. 1 MP each way. Counts as shelter *on the surface*. |
| 13 | Vent Shaft | surface **and** tunnel | 1 | 0 | Rough hole. 1 MP down, `VENT_ASCEND_MP` (2) to climb out. |

The SV column is a **surface** question only: underground `dawnUpkeep()` treats
depth as cover whichever hatch you came down, so 12 vs 13 now differs on the
climb out (1 MP against 2) and nothing else.
| 14 | Tunnel Floor | tunnel only | 2 | 1 | Walkable underground. Waters and salvages. |
| 15 | Collapsed Tunnel | tunnel only | 255 | 0 | Solid rock and cave-ins. Impassable, permanent. |

12 and 13 appear on **both** boards — the surface hatch and the shaft cell
directly beneath it share a terrain id, so the art reads as the same landmark
from either side and no fifth terrain type is needed.

Wire encoding is unaffected: `encodeCell()` masks terrain to 4 bits with bits
6/7 as flags, and `decodeCell()` reads `& 0x3F`. 15 still fits.

### Resources

Tunnel Floor yields **Water** (cistern seeps) and **Scrap** (bunker fittings),
via `terrainSpawnRes(14, …)`. Nothing else is available underground. This falls
out of the existing table-driven action system with no new logic:

- `TERRAIN_HAS_WATER[14] = 1` → `doWater()` works unmodified
- `TERRAIN_SALVAGE_DN[14] = 7` → `doScav()` works unmodified
- `TERRAIN_FORAGE_DN[14] = 0` → `doForage()` self-blocks; nothing grows down here
- `TERRAIN_IS_RUINS[14] = 0` → salvaging underground does **not** raise the
  Threat Clock; nothing on the surface hears you
- `doTreat()` self-blocks (Medic or Settlement only), so a Medic can still work
  underground with no special case

`WEATHER_INTENSITY` columns 12-15 are all-zero: weather does not reach
underground, and the hatch mouths are sheltered.

---

## Data layout

Sizes, `BunkerHatch`, `bunkerHatches[]` and `hatchCount` live in
`Esp32HexMapCrawl.ino`, not `tunnels.hpp` — `GameState` and `SaveHeader` need
them before that file is parsed, and `ui-display.hpp` (the LCD minimap) is
included long before it.

```cpp
static constexpr int     TUN_COLS = 16, TUN_ROWS = 10;   // 160 cells
static constexpr uint8_t MAX_HATCHES = 8;
struct __attribute__((packed)) BunkerHatch { int16_t sq, sr; uint8_t tq, tr; };
```

`G.tunnel` is `HexCell (*)[TUN_COLS]`, allocated in `allocPsramGlobals()`
alongside `G.map`. 1440 bytes — above the ~1 KB PSRAM rule in
[dev-loop.md](dev-loop.md), so it goes through `psramStaticAlloc` like every
other large buffer. Use `TUNNEL_BYTES`, never `sizeof(G.tunnel)`.

Reusing `HexCell` verbatim is deliberate: `encodeCell()`, the vis-disk builder
and the client's `decodeCell()` all work on the tunnel board unchanged.

### Player fields

```cpp
uint8_t depth;      // 0 = surface, 1 = tunnels
int16_t tq, tr;     // position on G.tunnel, meaningful only while depth == 1
uint8_t hatchIdx;   // index into bunkerHatches[] of the hatch we came down
```

`resetSurvivor()` zeroes all four. That single hook is what keeps `regen` and
`eraseslot` safe — a stale `depth`/`tq`/`tr` would index a tunnel board the new
world just regenerated out from under it.

---

## Generation

`generateTunnels()` runs as **Phase 6**, the last step of `generateMap()`, so
every earlier phase (rivers, craters, settlements) has already placed its
terrain and hatches can be sited against a finished map.

1. **Surface hatches first.** Rejection-sample Open Scrub, refusing anything
   adjacent to Mountain/Settlement/Crater/River so a hatch is never walled in.
   Modelled on the `MIN_SETTLE` pass in `hex-map.hpp`.
2. **Minimum spacing (`HATCH_MIN_DIST` = 14) relaxes rather than failing** —
   successive rounds at smaller distances. That spacing *is* the feature, so it
   is tried hard before being given up. Measured over 400 worlds: all 8 hatches
   place every time, always at the full distance.
3. **Sorted left-to-right by column**, then zipped against shafts laid out the
   same way, so the pairing is geographically coherent. Without this the network
   is unlearnable — a player has to be able to build a mental model of which
   shaft surfaces where.
4. **Shafts, one per column band.** With 8 hatches over 16 columns the bands are
   two wide and cannot collide.
5. **A junction per shaft** — one adjacent floor cell, carved as a spur. The
   chain runs between *junctions*, never between the shafts themselves. See
   below.
6. **Corridors**: greedy walk toward the target with a ~25% random legal step so
   they bend instead of running dead straight. Chain junction *i* → *i+1*, then
   two long loops across the network. The walk treats every shaft as a wall.
7. **Verify, don't assume**: flood fill from junction 0 with every shaft solid.
   Any junction that does not come back was reachable only by cutting the
   corner across a shaft, so carve it a second way in and re-check. The verdict
   rides the startup log as `shaftFreePaths=ok|FAILED`.
8. Resources, encounter POIs (shuffle-and-deal, same as surface Phase 5) and
   image variants, each with their own pass over the tunnel board — the surface
   loops only walk `G.map`.

**The loops in step 6 are load-bearing.** Cave-ins are permanent and the player
is expected to backtrack; with only a single chain, one collapse would cut the
network in half. Across 400 generated worlds the network was fully connected
every time, at ~50 of 160 cells carved.

### Why shafts are spurs, not chain nodes

Stepping onto a shaft climbs straight out (`tunnelStepUp()`), so a corridor
that *runs through* one ejects anyone merely walking past it. The original
chain was carved shaft *i* → shaft *i+1*, which put every shaft squarely on the
main route: travelling from shaft *i−1* to *i+1* meant surfacing at *i*, and the
network degenerated into "hop to the next shaft and get spat out".

Hanging each shaft off its junction as a one-hex spur fixes it — you step onto
a shaft only when you mean to leave. Measured on the mock generator: the
pre-fix chaining fails the shaft-free reachability check in 8 of 10 worlds; the
junction version passes 25 of 25.

### The tunnel board does not wrap

`wrapQ`/`wrapR` are hardcoded to `MAP_COLS`/`MAP_ROWS`. Tunnel neighbour lookups
go through `tunIn(q, r)` instead, and an out-of-bounds neighbour is simply not a
legal move. `tunDist()` is the same axial formula as `hexDistWrap()` without the
nine-way wrap search.

---

## Terrain rewrites that could orphan a hatch

Runtime terrain mutation is rare but real. Audited:

| Source | Behaviour | Safe? |
|---|---|---|
| Fire burnout → Ash Dunes | `isFlammable()` is an explicit whitelist (0/2/3/4/7/9) | ✅ excluded |
| Flash flood → Marsh / Flooded District | `isWashoutEligible()` is 0 and 7 only | ✅ excluded |
| Quake → Open Scrub | only converts terrain 9 (Settlement) | ✅ excluded |
| **Settlement founding → terrain 9** | `doShelter()` converts a hex outright | ⚠️ **guarded** |

Settlement founding was the one real hazard: a hatch converted to Settlement
leaves `bunkerHatches[]` pointing at a hex that no longer reads as an entrance,
so the shaft below becomes a one-way exit. Both founding paths (single-hex and
the shelter triangle) now refuse hatch hexes. The triangle path rolls the site
onto another corner; all three being hatches is impossible given
`HATCH_MIN_DIST`.

Building an ordinary shelter *on* a hatch is still allowed — it is flavourful
and a Bunker Entrance already has SV 2.

---

## Persistence (`SAVE_VERSION` 16 → 17)

No migration; a v16 save is ignored and the whole world regenerates. That is the
established precedent for every prior bump.

`map.bin` layout:

```
SaveHeader            (now carries BunkerHatch[8] + hatchCount)
G.map                 MAP_BYTES
G.tunnel              TUNNEL_BYTES      <- v17, new
SaveGroundItem × 32   read until EOF
```

The hatch pairings ride the header because they are derived at generation time
and **cannot be recovered from the two boards alone** — the terrain tells you
where the hatches and shafts are, but not which connects to which.

On load everything is bounds-checked, the same defensive posture as the caravan
and doom coordinates:

- A hatch whose `sq/sr` or `tq/tr` is out of range is dropped. The rest still
  work; a fully-dropped table means the tunnels are merely unreachable.
- A player restored with `depth == 1` must have a valid `hatchIdx < hatchCount`,
  an in-range `tq/tr`, and non-rock terrain under them. Anything else surfaces
  them at their stored `q`/`r` rather than leaving `depth = 1` with a bogus
  index.
- A short read of the tunnel block fails the whole load, falling back to
  `generateMap()` rather than leaving `G.tunnel` half-populated.

---

## On-device LCD

`drawMapScreen()` draws hatches in violet-blue (`0x8878E0`) — the one hue not
already spoken for by terrain (amber), the Doom (red) or the caravan (teal) —
under the player markers, so a survivor standing on a hatch still reads as a
player.

Underground survivors still plot at their entrance hex (their `q`/`r` are pinned
there) but render in a dimmer violet instead of player amber, so "they went
below here" is readable at a glance. The legend gained a `HATCH` swatch; four
swatches plus the Doom awareness readout come to ~170 of the 234 px the map
border spans.

`TERR_COL` was widened from `[11]` to `[NUM_TERRAIN]` on the way through, which
also fixed a pre-existing bug: River Channel had no minimap colour and fell
through to a flat brown.

---

## Still to come (steps 3-5)

- **Movement**: `moveTunnel()` as a sibling of `movePlayer()` — no weather, no
  flood penalty, no tire tracks, no rad check, but the same cooldown, MP
  deduction, footprints and `EVT_MOVE` shape so the client needs no new path.
- **Wire**: `{"t":"tun","d":1|0}` in, `tsync` out, `"dp"`/`"tq"`/`"tr"` on the
  state broadcast, and a `"dp"` field on the vis disk so `applyVisDisk` writes
  into the right board. `{"t":"m"}` is reused unchanged.
- **Vision**: base radius 1 underground, +1 carrying a Bile Flare (item 54,
  carried not equipped), +1 for a Scout. A separate branch in
  `playerVisParams()`, not the surface formula.
- **Danger**: forced encounters from a new `"14"` pool in
  `data/encounters/index.json`; permanent cave-ins guarded by a flood fill
  (`tunnelFloodFill()`, already written) that refuses any collapse cutting a
  shaft — or a player — off from the network. Bad air landed early, as a
  dawn roll on REST rather than a per-tick LL/MP drain — see "Shelter and bad
  air" above. The "doubled with no flare" half of it is still open.
- **Client**: `data/tunnel-board.js`, decoder/renderer parameterisation, a
  DESCEND/ASCEND control, and an `#hud-depth` indicator.
