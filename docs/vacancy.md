# VACANCY — the settlement mini-quest

Status (2026-10-13): **firmware, mock, web client, K10 LCD marker and TV
observer built**; the firmware compiles clean (29% flash, 26% RAM). The bots
are covered in [bot-testing.md](bot-testing.md) ("VACANCY"). Sound is two
existing motifs, no new voice.

The user asked for a mini quest to save a settlement from destruction:
weird, liminal, horror; two items from new sub-quests; the quest on a random
settlement every time the board is regenerated.

## The story

A pink neon motel sign stands over one town: VACANCY, and under it a small
dark box that says NO. There is no motel. Every night another house in the
town *checks in*: its front door is a turquoise motel door in the morning, the
room behind it is a motel room, and nobody can remember who lived there, only
that somebody did. The town counts its chairs at supper. When the last room is
taken the NO lights and the town is gone.

The way out is to check them out. A guest register lies open on a front desk
standing alone in the open ground, with the town's names in it and nothing in
the departures column. A master key hangs off a housekeeping cart in a
corridor longer than the ruin it is in. Bring both back, sign the town out
under the sign, open every door, and open room 1147.

Canon: the register's first page is dated Friday 11 November 1966, every
arrival at 11:47 ([1966-timeline.md](1966-timeline.md)). Room 1147 is the room
the whole world checked into that morning. Nothing in the quest says what the
sign is.

## The parts

Four scenes in the `vacancy` pool (`index.json`), written by
`scripts/gen_vacancy.py` (edit the generator, not the JSON). The file number
is the part. All four open **server-side on arrival**, like Kane's chapters;
none takes a POI, and one survivor at a time can be inside each.

| # | Scene | Where | Counts when | Pays |
|---|---|---|---|---|
| 1 | Vacancy | the town | its last node is reached | food, water |
| 2 | The Front Desk | open ground (Open Scrub / Ash Dunes), 4-8 hexes out | its item is **banked** | Guest Register (70), scrap |
| 3 | Housekeeping | a ruin (Broken Urban / Flooded District), 4-8 hexes out | its item is **banked** | Master Key (71) |
| 4 | Check-Out | the town, only for a survivor carrying both | its `vac_ending` node is reached | water 2, food 2, med 2, `settlement_rare`, +40 score |

- Part 1 starts the count and places parts 2 and 3. The band widens to 3-12
  hexes, then drops the terrain, if nothing fits. Never a town, a hatch or an
  armed booby trap (the trap's arrival hook runs first and would open instead
  of the scene, every visit, until somebody sprang it; found by the bots).
- Parts 2 and 3 count on the bank, not the last node: walking off with the
  book still on the counter leaves the desk where it was. Once banked, the
  site closes for everybody (`vacOnBank`).
- Part 4's first node has a `requires_item: 70` choice and its second a
  `requires_item: 71` choice, each behind an ungated walk-away (the Null
  Meridian authoring rule). Reaching its last node saves the town and spends
  one of each from the reader's pack (an `item_result`, act `vac_out`). The
  register stays on the meeting-hall table; the key is a mint.
- On the town without both items, the sign hums at you once a day, saying
  which half you are missing (`ev vac` `w: hum`, unicast).

## The nights

- `VAC_ROOMS` = 8. The count starts when part 1 is read, not at worldgen: the
  sign waits for a guest.
- Every dawn, one room goes, but **only if somebody is connected**. Days run
  with an empty board too, and a town should not fall to nobody.
- At none left, the town falls: its hex becomes **Broken Urban** and loses its
  POI, for good, until the board is regenerated. The sign stays as a
  landmark, steady red NO VACANCY.
- A quake or a debris strike that levels the town first ends the quest as
  `lost`. Its terrain was already changed by that.
- Tuning is a first guess. One survivor makes about 5 hexes a day, so 8 nights
  is a solo run with chores, or a comfortable team run. Measure it before
  calling it tuned.

## Firmware and wire

All in **`vacancy.hpp`**, built on Kane's idiom (`kane.hpp`). Touch points:

- `.ino`: `EVT_VAC` (39), `ActiveEncounter.vac`, `ENC_POOL_VAC`, `SAVE_VAC_F`,
  forward declarations, the include, and PROTO_VERSION 10.
- `world-system.hpp`: `vacInit()` from `wInit()`, and the LCD marker snapshot.
- `actions_game_loop.hpp`: `vacDawn()` in the dawn block, after the scenes are
  cut short; `tickVacancy()` in the world tick; a save when it moves.
- `survival_state.hpp`: `vacOnArrival()` after Kane's.
- `encounter_engine.hpp`: a VACANCY scene never restores a POI.
- `network-msg-encounter.hpp`: `vacPartRead()` at a terminal node,
  `vacOnBank()` in `enc_bank`, the `vac_out` item_result, and no threat-clock
  tick on a walk-away.
- `network-persistence.hpp`: `/save/vac.bin` (its own file, so there is **no
  SAVE_VERSION bump**; a save without it keeps the freshly rolled sign).
- `network-sync.hpp`, `network-events.hpp`, `network-msg-player.hpp`
  (`enc_path`), `game-server.hpp` (`/state`), `boot-assets.hpp` (the pool).
- `ui-screens.hpp` / `ui-scenes.hpp`: the sign on the LCD map, a box on a pole
  that stutters, steady red once the town has fallen.

Wire:

- World object in `sync`, the tick broadcast's `world`, and top-level in
  `/state`: `"vac":{"q","r","st","rm","g","dq","dr","kq","kr"}`.
  - `st`: 0 lit / 1 counting / 2 saved / 3 fallen.
  - `rm`: rooms left.
  - `g`: what has been banked (1 the register, 2 the key).
  - `dq/dr`, `kq/kr`: the two sites, or -1 unless they are still open.
- `ev vac`: `{w, pid, q, r, rm}`. `w` is hook / desk / keys / room / saved /
  fallen / lost, plus `hum` (unicast, carries `has` instead). `pid` is -1 for
  the world's events.
- `enc_path` and `ev enc_start` carry `"vac": part`.

## Mock

`mock-server/server.js` ports the rules: the same constants, the bank rule,
the dawn rule (the mock only runs days with somebody connected), the fall to
Broken Urban, and `lost`. It saves nothing. **`dbg_vac`** fields combine, in
this order:

| field | does |
|---|---|
| `reset:1` | a fresh sign over a random town, nothing read |
| `town:1` / `town:[q,r]` | the sign over your hex / that hex (made a Settlement if it is not one) |
| `stage:N` | 1 places the sites if they are missing |
| `desk:1|[q,r]`, `keys:1|[q,r]` | that site under you / at that hex |
| `rooms:N`, `got:mask` | nights left; what has been banked |
| `give:mask` | the register (1) and/or the key (2) into your pack |
| `dawn:1` | run the dawn's check-in now |
| `fall:1` | NO VACANCY now |
| `lost:1` | a quake levels the town to scrub first, so it goes `lost` |

## The web client

`data/vacancy.js` (after `kane-jeep.js` in `web-assets.json`), plus small
hooks:

- `renderer.js`: the `vacancy` layer under the pawns, `vacancy_light` over the
  night tint, and `vacQuestAt` in `isPOIRenderable`.
- Landmark tiles (2026-10-14): the desk, the corridor and the full town wear
  painted tiles, swapped in by position (`vacTileKey`, through `questTileKey()`
  in `engine.js`; mirrored in `observer.js` for the TV). Desk `0_12` / `1_15`
  on Ash Dunes, corridor `4_13` / `5_15` in water, town `4_14`. The code-drawn
  desk and doorway are only the fallback until the sheet loads; the lamp and
  strip light stay, anchored on the painted ones (`VAC_TILE_LIGHT`). See
  [hex-tile-art.md](hex-tile-art.md) "Quest landmarks".
- `network.js`: `ev vac`, and the `enc_start` branch.
- `ui-encounter.js`: the kicker reads "VACANCY · THE SIGN / THE REGISTER /
  THE KEY / CHECK-OUT".
- `style.css`: `.toast-vac`.

**The sign** is a wreck, drawn in code (the user, 2026-10-13: "partially
broken and flickering, wasteland vibe"):

- **The box and pole:** the pole leans. The box has slumped off a
  rusted-through bracket; its turquoise paint is down to flakes on rust, one
  corner is torn back to the angle iron, and there are bullet holes. A cut
  cable hangs to the dirt and spits sparks. The letter board under it hangs
  off one chain, with half its backlight dead and crooked slot-in letters.
- **The tubes:** single-stroke neon letters, each tube with a health that is
  ranked per town:
  - two are snapped, the far half hanging;
  - one is dying, lighting dim and orange near its electrode;
  - a few stutter in bursts;
  - the rest work. Every tube is aged differently, with burnt ends and a
    patchy core.
- **Brown-outs:** the generator browns the whole sign out about five times a
  minute, with sparks off the broken bracket.
- **Quakes shake it** (the user, 2026-10-13). It takes the whole-view camera
  shake like everything else, plus the fault line's per-hex jitter when its
  town is on the line. On top of that the pole whips, the box rattles and the
  board swings on its one chain; off the line this is a third as strong. The
  generator stutters hard and the bracket and cable spit sparks while it
  shakes. A full sign rides the quake out without a flicker. The desk and the
  corridor take their hex's jitter too.
- **It mends as the town fills.** Every house that checks in heals a tube. The
  stutter steadies and the brown-outs stop. With one room left only the worst
  tube is still broken, and the night the town is full every tube burns steady
  red. Saved, it is dead glass for good, and so is a town lost to a quake or
  a falling sky before the count ran out (never the red of a full town; the
  LCD draws nothing for it).

It is drawn once the town is on your map, and at night from 10 hexes off
whatever the fog: it is the only lit thing out there. On joining, the log says
which way the sign is and how far.

Other drawings:

- The front desk: a counter on a square of burgundy carpet, with a lit lamp and
  a STAFF ONLY door with nothing behind it.
- The corridor: glass doors in a ruin, with a perspective corridor of turquoise
  doors and a stuttering strip light.
- Each house that has checked in is a warm window on the town hex, with
  somebody sitting in it.
- Items 70 and 71 have procedural badges in `item-icons.js`.

## K10 and TV

- **K10 LCD:** the map marker (above), and chronicle lines for every part
  opening and every `ev vac`. `MOTIF_POWER_DOWN` plays when the town is saved
  and `MOTIF_ROTTEN_CHORD` when it falls.
- **TV observer:** `/state`'s `vac` goes through the diff engine as world
  events: `VAC_SIGN`, `VAC_HOOK`, `VAC_DESK`, `VAC_KEYS`, `VAC_ROOM`,
  `VAC_SAVED`, `VAC_FALLEN`, `VAC_LOST`.
  - Each has a line bank in `observer-lines.js`, in the house voice. The reader
    is named from whoever was in the scene.
  - Saved and fallen take the headline.
  - When the town is inside the camera subject's vision disk, a still of the
    sign is drawn on it.

## Verified (mock, 2026-10-13)

- The sign at every stage, by day and at night: before anyone has read it,
  counting, one room left, full, and saved. No console errors.
- Flicker statistics, measured over simulated time: on a fresh sign,
  brown-outs start about 5.6 times a minute and dim it about 3% of the time,
  falling to none when the town is full. Flaky tubes are lit 93% of the time,
  and the dying one is mostly dark.
- The fall: the terrain becomes Broken Urban, and the TV says "no vacancy. the
  town is full." as a headline.

On hardware since 2026-10-13: ELF b3fbfd59 (with the trap rule and the log
fix) on 192.168.4.67, data synced. Not played there yet.

## Bots (2026-10-13)

`bots/vac_check.py` plays it end to end on a private mock (port 8771, one
quester): sign read, both halves banked, Check-Out, then a staged fall. See
[bot-testing.md](bot-testing.md) "VACANCY". About half the attempts save the
town (food and water chores, and Check-Out's last roll); the losses are not
bugs. What the bots showed about the quest:

- **The halves can split.** With two seekers, the register and the key can
  end up in different packs, and both are trade-locked. Humans can drop one
  for the other to pick up; the bots never do, so vac_check runs one quester.
- **`bed` livelock** (bot-side): the quester's and contentmax's `bed` layer
  fires at LL 4 or less and walks them back to cover before the long walk
  gets anywhere. The `vacancy_today` layer sidesteps it within today's AP.

## Open questions

- 8 nights and the 4-8 hex band are untested against real play.
- Should the two items be tradeable, so a team that split them can meet up?
- Should a saved town be worth something lasting (a cheaper caravan, a rest
  bonus)? Today it pays once.
- A voice for the hum (`snd-vocab.h` has no "vacancy")?
