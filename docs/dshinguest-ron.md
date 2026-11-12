# Dshinguest Ron — the Movement Tour

Status (2026-10-26): the scene, the Bender Strat (item 79), the
`spends_item` trade and the Dshingus are built in the firmware, the mock and
the web client. The firmware compiles. They are mock-verified (see "Checked"
below). **Not flashed, not synced, no bot run.**

The user asked for a roaming band that is a nuisance to every settlement it
plays and makes a mess entertaining the locals, known for its legendary
musical bowel movements. The Clean Underwear (36) had to be in the quest, as
the trade for Ron's Bender Stratocaster. The penalty is an infection called
the Dshingus: for 30 days you randomly leave brown spots on the map where you
rest, and anybody who shares your hex catches it.

## The story

A school bus with no glass and a drum kit welded to its roof is parked across
the town well. Down its side it says DSHINGUEST RON — THE MOVEMENT TOUR.
Nobody is sure how to say it, but everybody who has seen them play ends up
saying it the same way.

- Ron plays a pre-war guitar he calls the Bender Stratocaster. The F on the
  headstock was filed into a B long ago. He says it is the only one in the
  world, and he has said that about several.
- The drummer is a placeholder (Lids McGee, Ballcock Barry, Little Plunger,
  Sister Cistern) who plays a kit of toilet cisterns. The bassist plays a sump
  pump.
- Every show ends with the Movement in B-flat. It is called a movement
  because it comes in three parts, and the third part reaches the second row.
- Ron has played his last pair of underwear. He would give the Bender up for
  one clean pair.

The band tours: the scene is one file in the **settlement pool**, so any town
on any board can roll it. A survivor who reads it without Clean Underwear
learns what Ron wants, and can bring a pair the next time the bus comes
through.

## The scene: `data/encounters/settlement/25.json`

`index.json` pool 9 has count 25. Hand-authored, with no generator.

| Node | Choices |
|---|---|
| `bus` (the arrival) | **Find a spot near the front** (Endure, risk 20, hazard `splash_zone`) goes to `the_set`. **Sling the Bender Strat and climb up on the bus** (`requires_item: 79`, so it must be **worn**) goes to `sit_in`. |
| `the_set` (pays 1 food, bankable) | **Go round the back of the bus after the encore** (Navigate, risk 15, hazard `gutter`: the roadie's 1-scrap toll) goes to `backstage`. |
| `backstage` (bankable) | **Grab a mop and help the town clean up** (Scavenge, risk 25, hazard `one_more`) goes to `cleanup`. **Hand Ron your Clean Underwear** (`requires_item: 36`, `spends_item: true`, risk 5, hazard `wrong_hand`) goes to `the_trade`. |
| `cleanup` (end) | Pays 2-4 scrap and 1-2 water. |
| `the_trade` (end) | Pays the **Bender Strat (79)**. |
| `sit_in` (end) | Pays 1-3 food and 1-2 water. You play the Movement with them, and Ron has "another one now". |

Three hazards carry `"dshingus": true`:

- `splash_zone`: the front row, which also costs 1 food.
- `one_more`: an encore while you are under the stage, which also costs 1 LL.
- `wrong_hand`: Ron shakes your hand with the hand he was using a moment ago.
  It costs nothing else.

None of these ends the scene, so you can choose again.

## The trade: `"spends_item": true`

This is a generic choice key, not one written only for this quest. It is
documented with the item gate in
[null-meridian-group.md](null-meridian-group.md). A successful roll takes one
of the `requires_item` out of the pack. The destination node's loot is what
it buys. A failed roll keeps the item. Equipment is never spent.

The pack change reaches the client as an `item_result` with act `enc_spend`.

## Item 79: Bender Strat

| | |
|---|---|
| Category | equipment, hand slot |
| Worn | +1 LL ceiling, +1 AP a day |
| Shop | `trade = no`: the caravan never stocks it, so Ron is the only source |

- The name is "Bender Strat" because items.cfg names are capped at 15 chars.
  The full name is in the story text.
- `game-data.js` has its story text, and `item-icons.js` `I[79]` draws its
  badge: a red offset body on the diagonal, a black guard, white pickups, a
  maple neck, a brown smear and a stink line.

## The Dshingus: `dshingus.hpp`

| | |
|---|---|
| Caught | Three ways: from a scene hazard with `"dshingus": true` (`dshCatch`, from `encRunChoice`); by ending a step on an infected survivor's hex; or when an infected survivor steps onto your hex. Both survivors must be up and connected, and `samehex()` applies, so it spreads in the tunnels too. |
| Lasts | `DSH_DAYS` = 30 dawns. You cannot catch it while you have it, so a running count never restarts. It also clears when the seat takes a new survivor (`dshReleaseSeat` in `handleMsg_pick` and when a seat is wiped). |
| Spots | Each REST has a `DSH_SPOT_PCT` = 50% chance to leave a spot (`dshOnRest` from `doRest`). Spots are surface only, one per hex (a new one freshens the old), and they dry up after `DSH_SPOT_DAYS` = 10 dawns. At most `DSH_SPOT_MAX` = 48 exist; one more replaces the oldest. |
| Costs | Nothing else. Spots are a mark on the map, not a hazard. |

Tuning is a first guess. The user set the 30 days. The 50% chance, the
10-day spot life and the symmetric contagion are this build's choices.

### Wire

- `world.dsh` = `{"d":[days left per seat], "s":[[q,r,day],...]}`.
  - Sync and `/state` carry `s`. The tick carries only `d`.
  - A spot's `day` is `G.dayCount` when it was laid. Clients age spots off
    `gs.dc` the same way the board does (`groundAge`), so a dawn sends
    nothing for spots.
- `{"t":"ev","k":"dsh","w":"catch|spot|clear","pid","src","q","r","d","day"}`
  is a broadcast. `src` is the seat it was caught from, or -1 for the band.
  `d` is the days left.
- `EVT_DSH` = 42. PROTO_VERSION is unchanged: everything is additive, and
  older clients ignore it.

### Persistence

`/save/dsh.bin` ("DSHG" v1) holds the six counts and the spot table. It is its
own file, like `ghosts.bin`, so `SAVE_VERSION` is not bumped. A save without
the file loads with nobody infected.

### Firmware touch points

- `.ino`: `EVT_DSH`, the forward declarations, `SAVE_DSH_F` /
  `DSH_SAVE_MAGIC`, and the include after `lab.hpp`.
- `encounter_engine.hpp`: `EncChoice.hazDsh` and `spendItem`.
- `network-msg-encounter.hpp`: the catch on a failed roll and the trade's
  spend and ack.
- `survival_state.hpp`: `dshOnArrival` in `movePlayer` (after the ghost,
  before the hatch) and in `moveTunnel`.
- `actions_game_loop.hpp`: `dshOnRest` in `doRest`, `dshDawn` in the dawn
  block, and the save flag in the world tick.
- `world-system.hpp`: `dshInit` in `wInit`.
- `network-persistence.hpp`: save and load.
- `network-sync.hpp`: sync and tick.
- `game-server.hpp`: `/state`.
- `network-events.hpp`: the `ev` and the K10 chronicle lines (caught, over
  it).
- `network-msg-player.hpp`: the seat release.

### The web client

`data/dshingus.js` comes after `ghosts.js` in `web-assets.json`. Its hooks:

- `network.js`: `world.dsh` and `ev dsh`.
- `renderer.js`: `renderHexContent` draws a spot beside the footprints, so
  spots show only in sight, like footprints. `renderCharacters` calls
  `dshOverPawn` after the ghost.
- `index.html`: `#hud-dsh`.
- `style.css`: `.hud-dsh`, `.log-dsh` and `.toast-dsh`.

Everything is drawn with math:

- A spot is a lumpy brown splat with an ink rim, a dark heart and a wet sheen,
  plus flung drops. It is seeded by hex and day, so it never shifts. It pales
  as it dries, and has flies for its first two days.
- An infected survivor's portrait has three flies on wandering loops and two
  stink lines.

## Mock testing

`dbg_dsh` (see the handler comment, and the list in
[dev-loop.md](dev-loop.md)). For the trade:

```js
send({ t: 'dbg_setitem', slot: 0, item: 36, qty: 1 })
send({ t: 'dbg_enc', biome: 'settlement', id: 25 })
```

Use `dbg_force` to steer the rolls.

## Checked (mock, 2026-10-26)

- The gig opens with only the ungated choice when you have no guitar.
- A forced failure in the front row catches it: toast, log, `✺ DSHINGUS 30D`
  on the HUD.
- Backstage shows the trade with a red `−1 Clean Underwear` chip.
- Taking the trade emptied the slot, the haul shows the Bender Strat, and
  banking puts it in the pack.
- Equipping the Bender Strat puts it in the hand slot and raises the LL
  ceiling from 7 to 8.
- A REST with the spot forced on laid a spot and logged it.
- At dawn the count went from 30 to 29.
- Contagion works both ways with two seats:
  - the clean survivor stepped onto the infected one's hex and caught it;
  - the infected survivor stepped off and back onto a cleaned survivor's hex
    and gave it.
- The spot and fly art was checked on a large offscreen canvas.

## Not done

- The K10 has only chronicle lines: no LCD marker for spots, no sound, no
  cut scene.
- The observer TV does not draw spots or flies.
- The bots ignore `ev dsh` and the spots. The trade is gated and never first,
  so a bot takes the ungated choices. No balance run has been made.
- It has not been flashed or synced.
