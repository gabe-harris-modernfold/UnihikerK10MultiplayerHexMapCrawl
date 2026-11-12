# Forest oasis — five safe groves in the Rust Forest

Status (2026-11-04): **built in firmware, mock and web client; not flashed,
not synced.** See "Where it stands" at the bottom.

A forest oasis is a 7-hex grove of Rust Forest. In the middle is a bed: a
root hollow or a den. All six neighbours are dense overhead canopy. Every
generated map has **5** of them. While a survivor stands inside one, nothing
from the world can hurt them. Bedding down there for the night heals them the
way a Settlement does, with no food or water needed for the heal.

## Decided with the user (2026-11-04)

- **Rest = Settlement heal.** Resting in a grove always heals at dawn: +1 LL,
  or +3 at LL 2 or below, and a minor wound knits. There is no F>=2 / W>=2
  gate, and no exposure bite. Food and water **are still eaten**, so hunger
  and thirst still cost LL. The grove gives you a bed, not food. (Rejected:
  cancelling every LL loss like a built shelter does, which would make it
  starvation-proof. Also rejected: exposure cover only.)
- **The grove blocks world and creature hazards** whether you are awake or
  asleep: weather, terrain, fire, lightning, mould, the dusk radiation roll,
  the Creeping Doom, Kane's burst and ghost feeding. Encounters, traps and
  your own actions can still hurt. A grove has no POI and no trap, so in
  practice that leaves only your own actions.
- **Shape = 7-hex grove.** It is a centre and its full ring, and the whole
  grove counts as safe, not just the centre.

## The tiles

The art is in `art/hex-sheets/hexRustForest.png`, which the tile-sheet
session owns. Rust Forest's landmark sentinel is 6 (`LANDMARKS[2] = 6`), so
cells 6 and up are pinned art in `tiles.json` `poi`. `pickVariant()` never
deals them.

| Variant | Key | Where | Subject |
|----|----|----|----|
| 8 | `2_8` | centre | root hollow under a fallen tree |
| 9 | `2_9` | centre | a den under the roots |
| 10 | `2_10` | ring | dense overhead canopy, A |
| 11 | `2_11` | ring | dense overhead canopy, B |
| 12 | `2_12` | ring | dense overhead canopy, C |

- **Centre:** the groves alternate hollow and den down the list, from a
  random start. Five groves means three of one and two of the other.
- **Ring:** going round the six sides in direction order `d = 0..5`, the
  canopy tiles go 10, 11, 12, 10, 11, 12 from a random offset. Each tile
  shows exactly twice, and no two neighbours in the ring match.

```
        10
    12      11
        8/9
    11      12
        10
```

(Flat-top hexes: the ring is the six hexes around the bed.)

**The variant is the flag.** A grove hex is `terrain == 2 && variant in
8..12` (`oasisCell()` in hex-map.hpp, `isOasisCell()` in game-data.js). There is
no new HexCell field and nothing new on the wire: `variant` already rides
the VV low nibble. So there is no PROTO bump and no SAVE_VERSION bump.

**Encounters `forest/1` and `forest/3` lost their pinned tiles.** Cells 8
and 9 used to be their art. Both encounters still place and run as before.
Their hexes are now dealt plain forest art from the pool.

## Generation: Phase 5.6

This is `placeForestOases()` in hex-map.hpp. It runs after Phase 5.5 (the
landmark pins) and before Phase 6 (the hatches).

1. **Keep the groves already standing.** A grove counts if its centre is
   8 or 9 and all six sides are 10..12. On a new world there are none yet.
2. **Re-deal strays.** Any other forest hex showing 8..12 gets a new variant
   from the pool, capped below 8. Two things can leave strays:
   - A world saved before oases existed. Rust Forest had ten pool tiles then,
     so 8 and 9 were dealt at random.
   - A stale `tiles.json` on the card that still says ten.
3. **Pick the rest, one at a time, best score first.** A centre is a
   candidate only if:
   - all seven hexes are Rust Forest, or open ground the forest can grow
     over (Open Scrub, Ash Dunes, Rolling Hills);
   - none of the seven has an encounter POI, a trap or a built shelter;
   - it is at least 4 hexes from every grove already placed, so there is a
     clear hex between two rings.

   Candidates are scored:

   `score = 6 x (forest among the 7) + (forest on the 12 hexes two out) + 3 x min(distance to nearest grove, 14) + rand(0..3)`

   - **Deep forest wins.** A grove that is already forest all through
     (7 x 6 = 42) beats open ground.
   - **Spread wins up to 14 hexes.** Past that, distance earns nothing more.
   - **Growing forest.** Any of the seven that is not forest yet becomes
     Rust Forest. That is how a map short on forest still gets five groves.
     A grown hex keeps its resource, since food is the only surface pile and
     the forest grows food too.
4. **Stamp the grove:** centre variant first, then the ring.

What later phases do near a grove:

- **Phase 6 (hatches):** goes on Open Scrub only, so it can never land on a
  grove.
- **Phase 7 (traps):** skips grove hexes (`traps.hpp`).
- **Settlement founding:** can't happen on a grove, because a shelter can't
  be built there (see below).
- **Water and quakes:** floods only wash out Scrub and Hills, and quakes
  only change Settlements. Neither touches the forest.
- **Fire:** can't burn a grove (see below).

**Old saves.** `tryLoadSave()` calls `placeForestOases()` once `map.bin` has
loaded.
- A world that already has its five groves comes through untouched.
- An older world gets its strays re-dealt and five groves planted. It then
  asks for a save.
- **There is no SAVE_VERSION bump, so the K10's current world needs
  regenerating.** The re-deal only touches 8..12. A world made before
  2026-11-04 was dealt forest variants 0..9 at random, and with the new sheet
  its stray 6s and 7s draw as the forester's hut and the Evacuation Special
  on random hexes. Those cells are the pinned art for `{2,2,6}` / `{2,4,7}`
  in `LANDMARK_PINS`. New worlds are fine, since `pickVariant()` deals only
  from the pool. Regenerate the K10's world once this is flashed.

The mock runs the same placement over its baked map (`placeOases()` in
server.js, after its fixed hatches). That map's terrain is a stripe pattern
(`idx % 11`) that never lines up seven hexes of growable ground. So after the
firmware's strict pass finds nothing, the mock falls back to a second pass
that takes any ground except a town, crater, river or hatch. That fallback
is mock-only; the firmware keeps the strict rule.

## What the grove blocks

Each gate checks `inOasis(p)`: the survivor is on the surface
(`depth == 0`) and standing on a grove hex.

| Hazard | Where | Inside a grove |
|----|----|----|
| Exposure at dawn | `dawnUpkeep()` survival_state.hpp | covered, so no bite and no "slept rough" |
| Rest heal at dawn | `dawnUpkeep()` | `restedWell` whatever F/W are, like `inSettlement` |
| Dusk radiation roll | `duskCheck()` | skipped. R is still carried, and the clean-day R-1 still works |
| Chem storm, per tick | `tickGame()` actions_game_loop.hpp | full cover |
| Chem storm, travel | `movePlayer()` | a step into a grove counts as reaching cover |
| Strangle fog (LL and AP) | `tickGame()` | skipped entirely |
| Lightning, direct strike | `maybeIgniteLightning()` world-system.hpp | the canopy takes it |
| Fire | `isFlammableAt()` world-system.hpp | a grove never ignites, and fire never spreads into it, so it never burns out to Ash Dunes. `resolveFireDamage()` also skips grove survivors |
| Flood washout | `spreadFlood()` | Rust Forest is never washout ground already |
| Creeping Doom | `resolveDoomProximity()` | no warning, no node eaten, no LL |
| Machine Gun Kane | `kaneExposed()` kane.hpp | not "in the open", so no question and no burst |
| Ghosts | `ghostCanHaunt()`, `tickGhosts()` ghosts.hpp | nothing latches on inside. A rider already on you waits: no drain while you are in the grove |
| Slime mould / daisy | `ecoBiteCheck()` ecology.hpp | no bite, and the slime run ends |

What still happens:
- **Hunger and thirst** at dawn.
- **Encounters:** none are placed on a grove.
- **Traps:** none are placed on a grove either.
- **Your own actions:** e.g. an item that costs LL.
- **Bad air:** underground only, so it never applies in a grove.
- **Mould still grows over a grove.** It just can't bite there.

**No shelter on a grove.** `doShelter()` refuses with `ABW_NOT_NEEDED`. A
grove is already full cover. A tent there could also only grow into a
Settlement, which would stamp the grove out.

## Clients

All of these read the variant the same way: `isOasisCell()` /
`cellTerrainName()` in game-data.js, and an inline `isGrove()` in
observer.js and observer-fx.js.

- **Map:** the tiles come from `tiles.json` `poi` (`2_8`..`2_12`) like every
  pinned landmark. If an entry is missing, the client falls back to
  `variant % pool`, so a grove draws as plain forest.
- **Walking in:** the first step into a grove from outside shows a toast and
  a log line: "FOREST OASIS: Safe rest spot: no weather, fire or creature can
  reach you here, and resting heals without food or water." (`_evMv` in
  network.js). The move narration says "Forest Oasis ... Safe rest spot."
- **Action deck** (ui-panels.js):
  - The panel's place line reads "IN THE FOREST OASIS".
  - REST's description becomes "Sleep safe in the oasis — heals at dawn
    with no food or water needed, nothing can reach you".
  - BUILD SHELTER is disabled with "The oasis canopy is shelter enough".
- **TV:**
  - A soft green ring sits just inside each grove hex in the disk
    (observer.js).
  - The place line reads "FOREST OASIS · SAFE REST".
  - The dawn "slept rough" cut never fires for a survivor in a grove
    (`fromRough()` in observer-fx.js).
- **LCD:** nothing yet. The K10 screens do not name the oasis.
- **Bots:** see `docs/bot-testing.md` "Forest oasis". A bot that is about
  to bed down prefers a grove within reach, and never tries to build in one.

## Testing it on the mock

The mock has two test-only messages for this (`docs/dev-loop.md`):
- `dbg_oasis` puts you in a grove: `{n}` for the bed, `{n, d}` for ring
  side d.
- `dbg_hurt` takes optional `f`/`w` to set the food and water tracks.

All 8 checks passed on 2026-11-04. They ran from a throwaway WebSocket script that is not in the repo:

| Check | Grove | Open ground (control) |
|----|----|----|
| Dawn at F1/W1 with no tokens, LL 4 | `expd 0`, `dll +1` | `expd -2`, `dll -2` |
| `dbg_ignite` on your own hex | no `fire_spread` | the neighbour catches |
| Doom adjacent at awareness 100 | no `doom_act` / `doom_warn` | `doom_act`, `llLost 1` |
| Ghost on your hex, one tick | no latch | not run |

Before the dawn checks, one warm-up night at F1/W1 trips the one-off
F/W threshold losses. Without it the two nights would not be comparable.

Checked by reading the code only (their rates are too slow to measure in a
short mock run): chem, fog, lightning, mould and Kane.

## Where it stands

- **Firmware:** compiles through `scripts/build.ps1`: 1,651,497 B flash
  (31%), 87,456 B RAM (26%). No PROTO or SAVE_VERSION change. **Not
  flashed.** Phase 5.6 and the load-path top-up have never run on the board.
- **Mock:** five groves on boot, and the checks above pass.
- **Web client and TV:** checked against the mock in the browser. The grove
  tiles draw, the toast and deck lines show, and there are no console errors.
- **SD card:** not synced. The forest art (`hexRustForest` sheet,
  `tiles.json`) belongs to the tile-sheet session.
- **Bots:** updated, but no bot run yet.
- **K10 world:** regenerate it after flashing (see "Old saves").
