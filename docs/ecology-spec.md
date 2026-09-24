# The Understory — spec

An algorithmic slime mould that lives only in the K10's PSRAM. Every boot
rolls a new genome and a new species germinates on the map; power-off kills
it. It grows in **waves**: a plasmodium spreads as a network of veins toward
food, stops, pulls itself together into **fruiting bodies**, bursts, and
the spores start the next, larger wave somewhere else. What it leaves behind
— **scars** — is saved to SD, so each new boot's growth takes root on the
fossils of the last. It feeds on the game's own in-memory data (footprints,
tracks, terrain, fire, the device's telemetry) through a set of
transformation rules, and in this first cut it is **purely cosmetic**.

It has a second, fixed species: the **Wasteland Daisy**, biting flowers that
seed wherever a survivor was hurt and bloom three days later (see "Second
species"). The slime mould is new every boot; the daisy is always the daisy.

Status: **design draft, nothing implemented.** No firmware, client,
mock-server or bot changes exist yet. Open questions are at the bottom.
"The Understory" is a working name.

**The Understory is its own system.** It shares no state with Creeping Doom,
the Meridian Engine or the caravan, and neither side reacts to the other. It
*reads* the environment those systems change (a fire Doom starts is still a
fire), but it never reads their entity state and never writes anything they
read. It keeps its own clock.

**It is cosmetic — with one exception.** Nothing the slime mould does may
change a number a player, a bot oracle or a balance measurement can observe:
no terrain, resource, loot, encounter, movement cost, vision, damage or
score. The single mechanical effect in the whole system is the **Wasteland
Daisy's bite** (1 LL, see "Bite"). With the bite switched off (`eco`/`bite`
= 0, see "Determinism") or the build flag `ECO_ENABLE=0`, the game must be
identical to one without the Understory.

---

## What players experience

- On the boot splash, one line: *Something has taken root: ASH-VESSEL
  GREY.* The name is generated from the genome, so it is different every
  power-on and the same for everyone playing that session.
- For the first minutes, nothing. Then faint filaments on a few hexes,
  usually where old scars are. They lengthen into a vein network: thick
  tubes between the places it has found food, fine lace at the edges where
  it is still searching.
- Where survivors keep walking the same route, a vein grows along it.
- Then it **stops growing**. The lace withdraws, the veins drain toward one
  to three points, and on those hexes something rises: stalked, beaded
  **fruiting bodies**. They sit there for a few minutes. Then they burst.
- Soon after, new faint patches appear somewhere else — downwind, along the
  caravan's ruts, along the paths people walk — and the next wave begins,
  a little larger than the last.
- It stays **slight**. By the end of a ~2-hour session (2–3 waves) a player
  who looks will see it; one who doesn't may not have noticed. It never
  obscures terrain art, POIs, fire, flood or the survivors' own markers.
- The next boot, the network is gone and something else is growing — but
  the places the last one fruited and the veins it held longest are still
  marked, and the new one tends to germinate there.
- The slime never hurts anyone. It is there, it is growing, and it is
  paying attention to where people go.
- Separately: wherever someone got hurt, a few days later there are
  flowers. Pale, pretty, and wrong — the heads turn to follow you, there
  are teeth in the middle, and if you walk into them they **bite**. The
  bite is an injury too, so the patch you walked into grows thicker.

---

## Lifecycle

### One boot

```
power-on ─► genesis ─► wave 1 ─► wave 2 ─► … ─────────────► power-off
             │                                                │
             │   at dawn: accrue scars ─► /save/scar.bin      │ (no shutdown hook —
             │                                                │  it just stops)
             └─ genome = 16 bytes of esp_random(); spore sites from old scars
```

### One wave (per colony)

```
 SPORES ──► GERMINATE ──► FORAGE ──► FRUIT ──► BURST ──► SPORES (next wave)
 dormant    small agent    veins      veins      spores
 on a hex   population     spread,    retract,   scatter by
            appears        agents     bodies     dispersal
                           multiply   rise       vector
                           up to the
                           wave cap
```

A **colony** is the agent population that grew from one spore site. Several
colonies can be alive at once and in different stages; when two meet their
veins can fuse (they share the trail grid), but each keeps its own stage.

| Stage | Enters when | What happens | Typical length |
|---|---|---|---|
| **Spore** | Genesis, or a burst lands spores on a hex | Nothing visible. Dormant for a genome-set delay | 1–5 min |
| **Germinate** | Dormancy ends | 32–64 agents appear on the hex. Faint filament | ~1 min |
| **Forage** | Immediately after germinating | Physarum agents sense, turn, move, deposit (below). Agents on food spawn new agents at vein tips, up to the colony's **wave cap** | 15–35 min |
| **Fruit** | Colony reaches its wave cap, **or** finds no new food for N ticks (starvation — real Physarum fruits when it runs out) | No more spawning. Agents stop exploring and steer to 1–3 **fruiting sites** (the colony's densest hexes on liked terrain). Outer trail decays, so the network visibly retracts. Fruiting bodies form in stages: forming → mature | 3–6 min |
| **Burst** | Mature body has held for M ticks | Colony agents die off (the plasmodium was spent building the bodies). Each body releases K spores, placed by the dispersal vector. The fruiting hex takes a scar (see "Scars") | instant, plus a short client animation |

**Waves grow.** Each spore generation's wave cap is ×1.4–1.8 (genome) of the
parent's, and more spores land than colonies died, so each wave is larger
and wider than the last. The global caps in "Loudness" hold it to "slight"
no matter how many waves a long session sees; once the global agent cap is
reached, new germinations wait.

### Genesis

Runs on the **first eco tick**, not in `setup()`: by then Wi-Fi is up, and
the ESP32-S3's `esp_random()` is only a true RNG while the radio (or the
bootloader entropy source) is running. The map is already generated or
loaded at this point, and `scar.bin` has been read.

1. Roll the 16-byte genome (or take the pinned seed, see "Determinism").
   Seed the ecology's own PRNG (xorshift32) from it — nothing after genesis
   calls `esp_random()`, so a pinned seed replays the same species.
2. Decode it into the species parameters (below).
3. Place 2–4 spores: weighted toward old fruiting scars, then vein scars,
   then the species' preferred terrain. Never on water, never within 3 hexes
   of a connected player (it should be *found*, not appear underfoot).
4. Log `eco genesis seed=… name=…` and publish the name for the splash and
   `/state`.

### Death

There is no death routine. Power-off, reset, a crash or a flash all end the
ecology equally. The only thing that outlives it is what has already been
written to `scar.bin`.

### Regen

A world regen (`regen` message, `network-msg-player.hpp`) removes
`/save/scar.bin` alongside `map.bin`/`players.bin` and re-runs genesis:
scars belong to the land they were made on, and that land is gone.

---

## The genome

16 bytes, decoded once at genesis. Every field is chosen from a curated
range so that every genome produces *a* living network — no genome is a dud
that fizzles in a minute or a mat that floods the map.

| Bits | Field | Meaning |
|---|---|---|
| 4 | `style` | Index into a tuned table of Physarum parameter sets: sensor angle, sensor distance, rotation angle, step size, deposit, trail decay, trail diffusion. Styles range from thick sparse cables to fine lace to looping reticulum |
| 8 | `jitter` | Small offsets inside that style, so two genomes with the same style still differ |
| 32 | `affinity[16]` | 2 bits per terrain type: hostile / neutral / liked / loved. Water is forced hostile |
| 4 | `vector` | Spore dispersal: wind (weather direction), footprints, tire tracks, fire ash |
| 4 | `tempo` | Agent steps per eco tick (growth speed), within the "Loudness" band |
| 4 | `cycle` | Wave timing: forage length, starvation patience N, fruit hold M, spore dormancy |
| 4 | `growth` | Wave-cap multiplier per generation (×1.4–1.8) and spores per body |
| 4 | `fruit` | Fruiting-body form for the client: stalk count, height, head shape (bead, cup, lattice, pod), cluster spread |
| 4 | `shyness` | Negative = drawn toward survivors' fresh tracks, positive = steers away from them |
| 8 | `hue` | Base colour (client + LCD); fruiting bodies use a shifted accent |
| 4 | `scarLove` | How strongly old scars attract spores and agents |
| rest | `name` | Syllable indices for the species name |

The style table and the cycle timings are the things that need empirical
tuning, off-device (see "Tooling").

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ 4. MEMBRANE    eco wire message, client drawing, LCD,        │
│                /state — plus the one mechanical output:      │
│                the daisy bloom bitset GameLoop bites from    │
├──────────────────────────────────────────────────────────────┤
│ 3. LIFECYCLE   colonies: germinate → forage → fruit → burst  │
├──────────────────────────────────────────────────────────────┤
│ 2. PHYSARUM    agents + trail grid (4× hex resolution)       │
├──────────────────────────────────────────────────────────────┤
│ 1. TRANSFORMS  game datasets → per-hex food / barrier field  │
└──────────────────────────────────────────────────────────────┘
```

### Layer 1 — transforms (the metabolism)

Each transform is a pure function from an **input snapshot** of game state
to the per-hex `food` and `barrier` fields the agents sense. None of them
touch game state.

```cpp
struct EcoHex {        // 4 bytes per hex, 75×57, PSRAM
  uint8_t food;        // attractant written by transforms, 0-255
  uint8_t barrier;     // 0 open … 255 lethal (water, active fire)
  uint8_t age;         // eco ticks this hex has carried a visible vein (saturates)
  uint8_t scar;        // 0-15, loaded from scar.bin; see "Scars"
};
```

| # | Transform | Reads | Effect |
|---|---|---|---|
| T1 | Footfall | `G.map[].footprints` (persistent), `W_hex[].track` (decays in ~60 s) | Footprints = food. Fresh tracks add food, or subtract it if `shyness` is positive |
| T2 | Substrate | `G.map[].terrain` | `affinity` → base `food`; hostile terrain raises `barrier` |
| T3 | Burn | `W_hex[].fire` | Burning hex: `barrier = 255`, trail in it wiped, agents in it die. When it goes out (terrain → Ash Dunes) ash is liked substrate if `vector` = ash |
| T4 | Wash | `W_hex[].flood` | Flooded hex: trail halved per tick; agents pushed out |
| T5 | Ruts | `G.map[].tireTrack` | Rutted hexes get a small food bonus; if `vector` = tire tracks, spores follow ruts |
| T6 | Weather | `G.weatherPhase` | If `vector` = wind, spores drift along the storm direction |
| T7 | Vital signs | free internal heap, Wi-Fi RSSI, connected clients, uptime | One global "metabolic rate" nudging `tempo` ±25%. More clients / lower heap = it grows faster. Cosmetic only: it changes how fast the veins spread, nothing else |
| T8 | Scar memory | `EcoHex.scar` | Adds `scarLove × scar` to food |

**Explicitly not read:** `W.creepingDoom`, `W.caravan` position/stock, any
Meridian state, player inventories, encounters, loot tables, the item
registry. (Tire tracks and fire are environment, not entity state.)

Reading loot tables and encounter text as "food" was discussed and deferred:
it only makes sense once the ecology has mechanical effects.

### Layer 2 — Physarum

The standard agent model (after Jones, 2010), on a trail grid at **4× hex
resolution**: 300×228 sub-cells, wrapping in both axes like the map. Hex
(q, r) owns the 4×4 block of sub-cells at (4q…4q+3, 4r…4r+3); the odd-row
half-hex offset is ignored at this level, because the client never draws
sub-cells (it draws per-hex edge masks, below).

Each agent step:

1. **Sense** three points ahead (left, centre, right at ± sensor angle,
   sensor distance away): `trail + food(hex) − barrier(hex)`.
2. **Turn** toward the strongest (by the rotation angle), random if tied.
3. **Move** one step. Into a lethal barrier = die; off an impassable edge =
   turn back.
4. **Deposit** into the trail.

Each substep, the trail grid is **diffused** (3×3 mean) and **decayed**.
Only active 16×16 tiles (any agent or any trail above threshold) are
diffused; empty land costs nothing, which matters because the network
covers a small fraction of the map.

```cpp
struct EcoAgent {      // 8 bytes
  uint16_t x, y;       // sub-cell position, Q6 fixed point
  uint8_t  heading;    // 0-255 = 0-360°
  uint8_t  colony;     // index into colonies[]
  uint8_t  energy;     // gained on food, spent per step; spawn when full
  uint8_t  flags;      // tip / returning-to-fruit / dying
};
```

- **Agents:** up to 4,096 (global cap). ~32 KB.
- **Trail grid:** 300×228 bytes, double-buffered for diffusion. ~137 KB.
- **Colonies:** up to 16, each `{stage, timer, generation, waveCap,
  agentCount, fruitSites[3], lastFoodTick}`.

### Layer 3 — lifecycle

Runs once per eco tick over `colonies[]`, applying the stage table above.
It changes agent behaviour through two knobs only:

- **Forage:** agents with full `energy` spawn a child at their position
  with a jittered heading (vein tips grow), until `agentCount == waveCap`.
- **Fruit:** agents get the `returning` flag and sense an extra attractant
  centred on their colony's fruiting sites, while the exploring tips lose
  energy and die. The network drains toward the bodies over a few minutes.

**Fruiting bodies** are separate records, not agents:

```cpp
struct FruitBody {     // up to 12 alive
  uint8_t q, r;
  uint8_t colony;
  uint8_t stage;       // 1 forming, 2 mature, 3 bursting (one message only)
  uint8_t timer;
  uint8_t seed;        // per-body variation for the client's drawing
};
```

**Burst** places spores (dormant records `{q, r, timer, generation}`):

| `vector` | Where spores land |
|---|---|
| wind | 4–10 hexes along the current storm direction (random direction in calm weather) |
| footprints | On hexes 3–8 away with the highest footprint counts |
| tire tracks | Along the caravan's rutted hexes, 3–12 away |
| ash | On Ash Dunes hexes within 10; falls back to wind |

Spores never land on water or within 3 hexes of a connected player.

### Layer 4 — membrane

The only ways the Understory leaves memory (all cosmetic except item 6):

1. **`eco` wire message** to every in-game client, once per eco tick (below).
2. **Client overlay** — `data/eco-field.js`, same shape as `fire-field.js`
   / `storm-field.js`: indexes the latest message; the renderer queries it
   in the terrain pass. **Drawn procedurally in canvas code, no image
   assets**, in the genome's hue:
   - **Veins:** for each hex with density > 0, a tube from near the hex
     centre to the midpoint of every edge in its edge mask, thickness from
     density, with a small wobble derived from a hash of (seed, q, r) so it
     is the same every frame and on every client. Because neighbouring
     hexes' masks agree on shared edges, tubes join into one continuous
     network across hex boundaries.
   - **Fruiting bodies:** drawn on their hex from `fruit` + body `seed`:
     thin stalks with beaded / cupped / latticed heads, growing in over the
     forming stage, a faint pulse while mature, and a one-shot puff of
     drifting motes on burst.
   - **Scars:** a very faint stain; fruiting scars a faint ring.
   - **Daisies:** per patch from `stage`, `count` and `seed` (see "Second
     species"). Head-turning and the snap use player positions the client
     already has; no extra wire data.
   Added via `data/web-assets.json`; no firmware change for the art.
3. **LCD map** — `drawMapScreen` draws vein hexes as a dim tint in the hue
   and mature fruiting bodies as a bright dot.
4. **Boot splash** — the species name line.
5. **`/state`** — `eco:{seed, name, style, tick, wave, colonies, agents,
   coverage, fruiting, scarred, daisies:{seeded, growing, bloomed},
   bite}`.
6. **Bloom bitset** — read by the GameLoop to apply daisy bites (see
   "Bite"). The only output with a gameplay effect.

Deferred cosmetic outputs (phase 6): an LED pulse when a body bursts,
occasional flavour toasts ("the ground here is soft", "something has
fruited to the east"), text rot in encounter prose scaled by local density,
and "wrong" hex art variants. Audio stays out: it would fight Doom's
ostinato.

---

## Second species: the Wasteland Daisy

Biting flowers that grow from pain. Unlike the slime mould it is **not**
rolled from the genome: every boot, on every map, it is the same species
with the same look and the same rules.

### Rules

- **Seeds where a survivor was injured.** Only injuries plant seeds — being
  burned, struck, washed out, attacked, bitten. Slow harm (thirst, hunger,
  exposure, bad air, radiation) does not. The split follows the existing
  `DownCause` enum (`Esp32HexMapCrawl.ino`), which every damage site already
  attributes:

  | Seeds daisies (injury) | Does not seed |
  |---|---|
  | `DC_FIRE`, `DC_LIGHTNING`, `DC_FLOOD`, `DC_DOOM`, `DC_ENC_HAZARD`, `DC_CHEM` (chem-storm burns), `DC_DAISY` (new, the bite itself) | `DC_THIRST`, `DC_HUNGER`, `DC_EXPOSURE`, `DC_BAD_AIR`, `DC_RADIATION`, `DC_FOG`, `DC_ENC_COST` (an LL price the player chose to pay), `DC_ACTION`, `DC_UNKNOWN` |

  `DC_CHEM` and `DC_ENC_COST` are judgment calls; the table is one
  `ecoIsInjury(cause)` switch, so moving a cause is a one-line change.
- **Surface only.** Nothing is planted for damage taken underground
  (`depth != 0`), and nothing ever grows in the tunnels.
- **Blooms in three days.** Seeds sprout and bloom over the next three
  in-game dawns (a day is `DAY_TICKS` = ~5 real minutes, shorter when
  everyone rests, so a patch takes ~15 real minutes — resting through the
  nights makes them bloom faster).
- **Fire burns them away.** A daisy patch on a hex that catches fire is
  destroyed at any stage, seed included.
- **More pain, more flowers.** Each further injury on the same hex adds a
  flower (up to 7 per patch) and, if the patch is still growing, does not
  reset its clock. A bite is an injury, so a bitten survivor thickens the
  patch that bit them — but that is the same hex, so the loop only feeds
  itself, it never spreads.
- **Bites.** Stepping onto a hex with a bloomed patch costs 1 LL (see
  "Bite").
- **No spreading.** A bloomed patch stays until it burns or the board
  powers off. It does not seed its neighbours — spreading is the slime's
  job.

### Stages

| Stage | When | Client draws |
|---|---|---|
| Seeded | the tick the hurt is seen | nothing (or a single dark speck) |
| Sprout | after dawn 1 | thin pale shoots |
| Bud | after dawn 2 | closed heads on stalks, slightly swaying |
| Bloom | after dawn 3 | open flowers: pale petals, dark toothed centre; heads turn toward any survivor on or next to the hex, and snap when one steps onto it (the bite) |

### Detecting injuries: the hurt ring

"Only injuries" means the cause matters, so the daisy can't just watch LL
drop between snapshots. Instead, each injury-class damage site (the left
column above — about seven sites across `world-system.hpp`,
`survival_state.hpp`, `network-msg-encounter.hpp` and the new bite) calls

```cpp
ecoNoteHurt(pid, cause);   // no-op unless ecoIsInjury(cause) and depth == 0
```

right where it already sets the `DownCause`. That pushes `{q, r}` of the
player's current hex into a 16-entry ring. Every damage site already holds
`G.mutex`, and the Eco task drains the ring during its own `G.mutex`
snapshot, so the ring needs no lock of its own. A full ring drops the
oldest entry (at 5 s drain intervals it never fills in practice).

This is the only write the game makes into the Understory, and it carries
nothing back: the game never reads the ring.

### Data

```cpp
struct DaisyPatch {    // up to 48 patches, ~6 bytes each
  uint8_t q, r;
  uint8_t stage;       // 0 seeded, 1 sprout, 2 bud, 3 bloom
  uint8_t dawns;       // dawns seen since seeding
  uint8_t count;       // flowers, 1-7
  uint8_t seed;        // per-patch variation for the client's drawing
};
```

At the cap, a new injury on an unplanted hex replaces the youngest seeded
(not yet visible) patch; bloomed patches are never evicted.

The Eco task publishes a **bloom bitset** — one bit per hex, 535 bytes,
double-buffered with the same index swap as the wire buffer — which is all
the GameLoop needs to apply bites without touching the patch array.

**Decided: daisies do not survive a reboot.** Power-off kills every patch,
seeds and blooms included, like the slime. They are not written to
`scar.bin`.

### Bite

**Decided: a bloomed daisy patch bites for 1 LL.**

- **When:** a survivor *enters* a surface hex whose bit is set in the
  published bloom bitset — by moving, by being washed or pushed there, or
  by respawning there. Once per entry: standing still in a patch does not
  bite again; stepping out and back in does.
- **Where it runs:** in the GameLoop's move resolution, under `G.mutex`,
  like fire and flood damage. It reads only the bloom bitset, so the patch
  array stays Eco-task-private.
- **What it does:** `p.ll -= 1` through the same path as other damage (so
  it can down a survivor at 1 LL, like fire), an `EVT_DAMAGE` with the new
  cause `DC_DAISY` (`"wasteland daisy"` in `DC_NAME`, and therefore in
  `bots/causes.py` reports and downed messages), a snap motif / LED flash in
  the style of the other damage feedback, and `ecoNoteHurt(pid, DC_DAISY)` —
  which adds a flower to the same patch.
- **Stale bitset:** the bitset can be up to one eco tick (5 s) old. A patch
  that burned in the last 5 s might still bite once; one that bloomed in the
  last 5 s might not yet. Acceptable. Fire also clears the hex's bit in the
  GameLoop's copy the moment it ignites the hex, so the common case — "I
  burned them, now I walk through" — is exact.
- **Visibility:** bites are only fair if players can see the flowers. Bloomed
  patches are drawn on every revealed hex (and under the accepted fog leak,
  a careful client could know about unrevealed ones). Bud stage is the
  warning: it is visible a full day before the teeth open.
- **Balance:** this is new damage, so it needs a bot run before it ships:
  `docs/bot-testing.md`'s deaths : near-misses ratio (measured 1:1, target
  ~1:4) and the tension-arc measurement. Bots need `dz` decoded and daisy
  hexes weighted in `bots/navigate.py`, or they will walk into every patch
  and skew the numbers.
- **Kill switch:** NVS `eco`/`bite` (default 1). 0 = flowers still grow and
  snap visually, no LL, no `EVT_DAMAGE`. Used by the cosmetic oracle and as
  a field toggle if the balance is wrong.

---

## Scars

**Decided: scars persist across reboots and never fade; only a world regen
clears them.**

Each hex has a scar level 0–15 (4 bits on disk, a byte in RAM). Two ways to
earn it:

- **Vein scars — at dawn**, in the same place `saveGame()` is triggered
  (`actions_game_loop.hpp`, `dawnOccurred`): every hex whose `age` passed a
  threshold during the day gains +1. `age` resets.
- **Fruiting scars — on burst**: the fruiting hex gains +3 immediately (in
  RAM; persisted with the next save). These are the strongest marks and the
  first choice for the next boot's genesis spores.

Capped at 15. A long-lived map is allowed to accumulate scars indefinitely.
Scars render even before the new growth reaches them — the map shows where
things *used to* live.

### File: `/save/scar.bin`

Separate file, **not** part of `SaveHeader`, so adding it does not bump
`SAVE_VERSION` and does not reset anyone's save.

```
uint32 magic   = 'SCAR'
uint8  version = 1
uint8  cols, rows            // must equal MAP_COLS / MAP_ROWS
uint32 bootsSeen             // incremented each genesis; flavour only
uint8  scar[rows*cols/2]     // packed nibbles, row-major
uint32 crc32                 // over everything above
```

~2.2 KB. The eco task publishes a packed copy and sets `ecoScarDirty`;
`saveGame()` writes `scar.bin` in the same `G.mutex` hold as the other
save files when the flag is set, so every SD write stays on the existing,
already-serialised save path. Read at boot after `tryLoadSave()`. Missing
file, bad magic/version/size or CRC mismatch → no scars, no error. Deleted
on regen and whenever `generateMap()` runs from the boot path (no save found
= new land).

---

## Clock and concurrency

Physarum is heavier than the old field model — a substep is a few
milliseconds, not one — so it gets **its own task** rather than running
inline on GameLoop:

- **Task:** `Eco`, pinned to core 1, priority 1 (below GameLoop's 2), ~8 KB
  stack. It runs in the gaps between game ticks and can never delay one.
- **Eco tick:** every 5 s (`ECO_TICK_MS = 5000`), paced by `vTaskDelayUntil`.
- **Ownership:** every ecology buffer belongs to the Eco task. Nothing else
  touches them except through the two published buffers below.
- **Per tick:**
  1. Take `G.mutex`, copy the inputs into a PSRAM snapshot (`terrain`,
     `footprints`, `tireTrack`, `fire`, `flood`, `track`: ~6 B/hex ≈ 26 KB),
     read `weatherPhase`, release. Well under 1 ms of lock.
  2. Run transforms, lifecycle, then `tempo` agent substeps with
     diffuse/decay, on private buffers, no lock. Yield between substeps.
  3. Encode the wire message into a PSRAM buffer and publish it by swapping
     a double-buffer index (one pointer write). Same for the packed scars.
  4. `ws.textAll()` the published buffer.
- **New client / `pick`:** `sendSync` (async_tcp) sends the *published*
  buffer, read through the swapped index, so it never races the encoder.
- **Budget:** log `eco tick us=` at verbose level; target < 40 ms of CPU per
  5 s tick. Levers if it's over: fewer agents, 3× instead of 4× trail
  resolution, fewer substeps. Watch GameLoop's tick timing and the heap
  line, not just the Eco task.
- **Tunnels:** the ecology is surface-only.

---

## Wire protocol

New server → client message, plus the new `DC_DAISY` down/damage cause
(`"wasteland daisy"`); bump `PROTO_VERSION`:

```json
{"t":"eco","tk":123456,"n":"ASH-VESSEL GREY","h":212,"g":9,"w":2,
 "v":"<2 chars per hex>",
 "f":[[q,r,stage,seed],...],
 "dz":[[q,r,stage,count,seed],...],
 "sp":[[fromQ,fromR,toQ,toR],...],
 "s":"<1 char per hex>"}
```

- `v`: per hex, row-major, two chars: **density** 0–15 (hex digit) then
  **edge mask** 0–63 (one base64-alphabet char; bit *d* = the vein crosses
  toward neighbour direction *d*, same order as `DQ`/`DR`). The mask is
  computed from the trail in the border strip of the hex's sub-cell block
  facing each neighbour, row-parity aware. ~8.6 KB.
- `dz`: every daisy patch past the seeded stage (seeded patches are not
  sent — nothing to draw). At the cap of 48 that is under 1 KB.
- `f`: fruiting bodies alive now. `sp`: spore flights from bursts this tick
  (only the tick they happen), for the burst animation.
- `s`: scar level per hex; only in the first message a client gets and in
  the one after scars change.
- `h` hue, `g` genome `fruit` byte (body form), `w` current wave, `n` name.
- **Fog:** the full grid goes to everyone and the client only draws on
  revealed hexes. A client reading the raw message can infer the outline of
  water barriers under fog; that leak is accepted (decided), so there is no
  per-player masking or per-client encode.
- At 6 clients × ~9 KB every 5 s this is ~11 KB/s — small next to the
  10 Hz `s` broadcast, and one PSRAM buffer shared by all clients, not a
  queue. The per-client WS queue is capped at 8 (`build_opt.h`), so it must
  never be sent more often than every few seconds.

Clients and bots must ignore it safely: check `bots/` treats an unknown `t`
as a no-op (`client.py` / `wire.py`) before shipping.

---

## Loudness

"Slight" is a tuning target, checked in the mock-server harness and then on
hardware with a pinned seed:

- First fruiting ~25–40 min after boot; 2–3 waves in a 2-hour session.
- After ~2 h, roughly 10–15% of land hexes carry a visible vein (density
  ≥ 4); never more than ~25% for any genome in the style table. Enforced by
  the global agent cap and a global coverage cap on germination.
- At most 12 fruiting bodies alive at once; typically 2–6 per wave.
- Client alpha stays low (on the order of 0.15–0.35 for the thickest
  veins) so terrain art always reads through; scars fainter still.
  Fruiting bodies may be a little more opaque — they are the moment worth
  noticing.
- `/state` reports `eco.coverage` as a percentage and `eco.wave`, so the
  target is measured, not eyeballed.

---

## Determinism and testing

"New every boot" is the design, and it is hostile to reproducible runs, so:

- **Pinned seed:** NVS key `eco`/`seed` (set via a debug WS message or
  `/state` query param). Non-zero = use it instead of `esp_random()`. The
  seed is always reported in `/state` and the boot log. With the same seed
  and the same player inputs the species and its growth replay exactly (all
  randomness after genesis is the ecology's own PRNG).
- **`ECO_ENABLE=0`** compile flag removes the whole system, task included
  (and with it every daisy bite).
- **Bite switch:** NVS `eco`/`bite`, reported in `/state`.
- **Cosmetic oracle:** a bot check that runs the same pinned scenario with
  the ecology on (bite switched off) and off and diffs every gameplay field
  it observes. Any difference is a bug by definition. A second run with the
  bite on must differ *only* by `DC_DAISY` damage and its consequences.
- **Mock parity:** `mock-server/` gets a JS port of layers 1–3 so offline UI
  work sees a live, fruiting ecology. The same port is the tuning harness.

### Tooling

Tune in the mock-server port first, at an accelerated clock (a 2-hour
session in a couple of minutes), with a debug view of the raw trail grid:

- the **style table** — parameter sets that give distinct, living networks
  on a wrapping 300×228 grid with hex-blocked food and barriers;
- the **cycle timings** — first fruiting and wave count against the
  Loudness targets;
- the **client drawing** — veins and bodies at real alpha over real terrain.

Then copy the constants to firmware. Parameters from the Physarum
literature are a starting point: they assume a much larger grid and far
more agents.

---

## Data layout (PSRAM)

| Buffer | Size | Allocated in |
|---|---|---|
| trail grid ×2 (300×228) | ~137 KB | `allocPsramGlobals()` |
| agents (4,096 × 8 B) | ~32 KB | `allocPsramGlobals()` |
| `EcoHex[57][75]` | ~17 KB | `allocPsramGlobals()` |
| input snapshot | ~26 KB | `allocPsramGlobals()` |
| wire buffers ×2 | ~2 × 12 KB | `allocPsramGlobals()` |
| packed scars ×2 | ~2 × 2.2 KB | `allocPsramGlobals()` |
| colonies, bodies, spores, genome | < 1 KB | static |
| daisy patches (48 × 6 B) + hurt ring (16 × 2 B) | < 400 B | static |
| bloom bitset ×2 | ~2 × 535 B | static (small enough for `.bss`) |

~240 KB of the 8 MB. Nothing goes in internal `.bss`; check the build's
"Global variables" line stays at ~17%. The Eco task's 8 KB stack is
internal RAM — the one internal cost.

---

## Phasing

1. **Foraging network.** Genome, genesis, transforms T1–T4 and T8 (inert
   until scars exist), Physarum agents + trail, one wave that forages and
   holds, Eco task, `eco` message (`v` only), vein drawing in
   `eco-field.js`, `/state`, splash line, pinned seed, `ECO_ENABLE`,
   mock-server port.
2. **Fruiting and waves.** Fruit/burst stages, fruiting bodies, spores and
   dispersal vectors, growing wave caps, `f`/`sp` in the message, body and
   burst drawing.
3. **Scars.** Vein and fruiting scars, `scar.bin`, scar rendering, regen
   cleanup, scar-weighted genesis.
4. **Wasteland Daisy.** `ecoIsInjury` + `ecoNoteHurt` at the injury sites,
   patches, dawn staging, fire burn-off, `dz` in the message, daisy drawing
   with head-turning and snap. Then the bite: bloom bitset, `DC_DAISY`,
   bite switch, bot `dz` decoding + avoidance, and a balance run before it
   is left on.
5. **More inputs.** T5–T7 (ruts, weather, vital signs), LCD drawing.
6. **More membrane.** LEDs, flavour toasts, text rot, wrong-art variants.
7. *(Not planned)* Mechanical effects. Would need its own spec and a round
   of bot balance measurement.

---

## Decided

- It is a **Physarum slime mould**: agents on a trail grid forming vein
  networks, not a reaction-diffusion stain.
- It grows in **waves**: forage → fruiting bodies → burst → spores → a
  larger next wave.
- Scars persist across reboots and never fade; only a world regen clears
  them. Fruiting sites scar hardest.
- It is a third independent system — no shared state or reactions with
  Creeping Doom or the Meridian Engine.
- Phase 1 is cosmetic only.
- A reboot has no meaning in the fiction: every genome is a fresh roll, not
  a descendant (no crash-driven mutation, nothing carried in NVS but an
  optional pinned test seed).
- Fog leak is accepted: the full grid goes to every client, no per-player
  masking.
- Loudness is slight — noticeable by the end of a session to someone who
  looks (see "Loudness").
- Client art is procedural canvas code, not image tiles.
- Second species, the **Wasteland Daisy**: fixed (not genome-rolled), seeds
  only where a survivor was *injured* on the surface, blooms three dawns
  later, never underground, destroyed by fire.
- Bloomed daisies **bite for 1 LL** — the Understory's only mechanical
  effect.
- Daisies do not survive a reboot.

## Open questions

- Name: "The Understory" is a placeholder.
- Should the species name / hue appear anywhere in-game beyond the splash
  (e.g. a survey result, a radio fragment)?
- Can a bite take a survivor's last LL, or does it stop at 1? Stopping at
  1 would make daisies a near-miss generator, which is the shape
  `docs/bot-testing.md` says the game is short of. Specified as "can down
  you, like fire" until a bot run says otherwise.
- Are `DC_CHEM` (injury) and `DC_ENC_COST` (not injury) on the right side
  of the table?
- Should players be told when something fruits or bursts (a toast), or is
  it only ever seen on the map? Deferred to phase 6, but it changes how
  much the fruiting moment matters.
