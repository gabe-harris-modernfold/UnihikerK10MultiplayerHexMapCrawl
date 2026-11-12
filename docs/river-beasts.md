# River Beasts: Momma Kevin & Grandma Clamp

Status: **design only, agreed in chat 2026-10-26.** No code, art or sound exists
yet, and phase 1 is on hold. This file is a snapshot of the
[design doc](https://claude.ai/artifact/6YF6GJ3BaJ77kMnPzGaK6E) at rev 32. One
question from a doc comment is still unconfirmed: does walking a Flooded
District without a Raft count as wading?

## Summary

Two mutant creatures make the River Channel dangerous. **Momma Kevin**, a fast catfish, roams the whole river and bites anyone it catches on the water. **Grandma Clamp**, a slow snapping turtle, keeps to one stretch. It snaps at anyone who crosses it on the water, and grabs and pins anyone standing on the bank beside it.

Momma Kevin croaks before it strikes, and a survivor with a Fishing Pole can kill it for 10 food. Wading without a boat doubles all damage. The design was agreed in chat on 2026-10-26; no code, art or sound exists yet.

## Why the river needs this

Once you own a boat, the river is a road with no risk, because nothing lives in it. Today water only does four things:

- **Drink:** 1 AP gives 1 water on any wet hex (Marsh, Flooded District, River Channel, flooded tunnel). All water is equally good.
- **Slow or block you:** Marsh and Flooded District cost 3 AP. River Channel is impassable, one unbroken wall around the world.
- **Fish:** the Fishing Pole doubles Forage yield on River Channel.
- **Flood:** storms spread water outward, from dry ground to Marsh to Flooded District.

Three items let you onto the river:

| Item | Slot | AP per river hex | How you travel |
| --- | --- | --- | --- |
| Floaty Disaster (18) | vehicle | 2 | afloat |
| Raft (63) | vehicle | 1 (any water) | afloat |
| Fishing Pole (28) | hand | 2 | wading |

The creatures turn the river into a place you cross with a plan, and a place a fisher goes to hunt.

## Momma Kevin, the catfish

Momma Kevin is a giant mutant catfish. It is fast, roams the whole river, and croaks a warning before it bites.

- **Where it lives:** River Channel, plus any Flooded District connected to it. When a flash flood turns ground next to its water into Flooded District, it can swim in. It never enters Marsh or dry land.
- **How it moves:** it keeps swimming the way it is heading and turns back at a dead end. The main channel is a closed loop around the wrap-around map, so it circles the world. It moves faster at night.
- **Croak:** when any survivor is within 3 hexes, on the water or on the bank, it surfaces and croaks. You hear it on the K10 speaker and see its head on the river tile and the TV. The croak does no harm: it is the warning, like Kane's call. Once it has croaked at someone on the water, it swims toward them.
- **Bite:** when it shares your hex while you are on the water, whether it swims onto you or you onto it. It can also reach a survivor on the bank next to its hex who is pinned there or fails a HOOK, and bites them at the wading rate.
  - Afloat (Raft or Floaty Disaster): −1 LL, +1 rad.
  - Wading (Fishing Pole only, or walking a Flooded District without a Raft): −2 LL, +2 rad.
- **After a bite:** it dives and swims off for a few world ticks, so one long trip can't turn into a string of bites.
- **Killing it:** with the Fishing Pole equipped, on its hex or next to it (the bank counts), Forage becomes **HOOK**. HOOK costs 2 AP and uses a Forage check harder than normal fishing. Success kills it and gives +10 food. Failure means it bites you at your current rate, or at the wading rate from the bank.
- **It comes back:** another Momma Kevin appears about 7 days after a kill, far from every survivor.

Radiation runs 0 to 10. Above 3 it raises the DN of encounter choices, and at 10 you automatically fail the dusk Endure check. So +1 rad per bite stacks into a real threat over a long trip.

## Grandma Clamp, the snapping turtle

Grandma Clamp is a huge mutant snapping turtle. It is slow and keeps to one stretch of river. On the water it snaps as you pass over it; on the bank it grabs you and holds on.

- **Where it lives:** River Channel only. It keeps to a home stretch of the channel and drifts within it.
- **How it moves:** barely, one hex every few world ticks.
- **Day and night:** by day it basks on the surface, and anyone who can see that hex sees a shell on the river tile. At night it is underwater and hidden, so you only find it when it snaps.
- **Snap on the water:** when it shares your hex while you are on the water. −1 LL afloat, −2 LL wading. It doesn't hold on, so you can keep going.
- **Grab on the bank:** when you step onto a land hex next to its river hex, it lunges out, takes 1 LL and pins you at the water's edge. You can't move off the hex. Each move attempt costs 1 AP and rolls Endure; a pass breaks you free and the move goes ahead. If you never pass, it lets go after 2 world ticks. It grabs once per visit: it won't grab you again until you leave the hex and come back.
- **It calls Momma Kevin:** while you are pinned, Momma Kevin swims to the water next to you and bites at the wading rate.
- **It can't be killed.** HOOK does nothing against the shell. You beat it by spotting it in daylight and keeping off its hex and the bank beside it.

## How they play together

Grandma Clamp holds you and Momma Kevin finishes you. On the water each step checks for Grandma Clamp first, then Momma Kevin. On the bank, Grandma Clamp's grab is what brings Momma Kevin in.

One step by the river, bank and water paths:

```text
ON THE BANK
  step onto the bank beside Grandma Clamp
    -> grabbed, pinned (-1 LL; Endure frees you)
    -> Momma Kevin comes (called by the grab)
    -> bitten (-2 LL, +2 rad: the wading rate)

ON THE WATER
  step onto water -> Grandma Clamp here?
    yes -> snapped (-1 LL, -2 wading), not held -> carry on
    no  -> carry on
  Momma Kevin within 3?
    no  -> safe step
    yes -> croak warning (it heads for you)
             get out           -> leave the water, no harm
             fish it           -> HOOK it: pass +10 food, fail -> bitten
             stay on the water -> bitten (-1 LL, +1 rad; double if wading)
```

A survivor pinned on the bank can't walk away from the croak. Their only answers are an Endure roll or, with a Fishing Pole, HOOK.

By day you watch for the shell; at night you listen for the croak.

| Time | Catfish | Turtle |
| --- | --- | --- |
| Day | Slower: 1 hex a world tick | Basking, visible as a shell |
| Night | Faster: 2 hexes a world tick | Underwater, hidden until it snaps |

## Numbers at a glance

Every value is a starting point for tuning. Decided rows came from you in chat; Proposed rows are my defaults. A world tick is about 15 seconds, and a game day is 20 world ticks.

| Creature | Setting | Value | Status |
| --- | --- | --- | --- |
| Catfish | Speed | 1 hex per world tick by day, 2 at night | Proposed |
| Catfish | Croak range | 3 hexes | Proposed |
| Catfish | Bite, afloat | −1 LL, +1 rad | Decided |
| Catfish | Bite, wading (LL) | −2 LL | Decided |
| Catfish | Bite, wading (rad) | +2 rad | Decided |
| Catfish | Bite from the bank (pinned, or a failed HOOK) | −2 LL, +2 rad (the wading rate) | Decided |
| Catfish | Rest after a bite | 3 world ticks (about 45 s) | Proposed |
| Catfish | Water it swims | River Channel + connected Flooded District | Decided |
| Catfish | HOOK cost | 2 AP | Proposed |
| Catfish | HOOK check | Forage vs DN 8 (river fishing is DN 6) | Proposed |
| Catfish | HOOK reward | +10 food | Decided |
| Catfish | Respawn | 7 days after a kill | Proposed |
| Turtle | Speed | 1 hex every 4 world ticks (about 1 min) | Proposed |
| Turtle | Home stretch | 12 river hexes | Proposed |
| Turtle | Grab on the bank | −1 LL and pinned; on the water it never pins | Decided |
| Turtle | Snap on the water, afloat | −1 LL | Decided |
| Turtle | Snap on the water, wading | −2 LL | Decided |
| Turtle | Break free | Endure vs DN 7, 1 AP per try | Proposed |
| Turtle | Lets go on its own | After 2 world ticks (about 30 s) | Proposed |
| Turtle | Grabs again | Not until you leave the hex and come back | Proposed |
| Turtle | Visibility | Seen by day, hidden at night | Proposed |
| Turtle | Can be killed | No | Proposed |
| Turtle | Calls Momma Kevin while you are pinned | Yes, from anywhere on the river; bites at the wading rate | Proposed |

## Build plan

Build it the way Machine Gun Kane was built: one new firmware file, its own save file, and small hooks into the shared files. Three phases, each ending with your sign-off.

1. **Rules with placeholder art.** Firmware creatures and rules, protocol version 13 → 14, a mock port with a debug river, web client markers and the HOOK button, and croak, snap and splash sounds previewed in sndsim. Sign-off: you play it in the mock and approve the sounds.
2. **Art and screens.** Catfish head and turtle shell art, with contact-sheet previews before anything is built into tiles. Then the TV observer, LCD map markers, cut scenes and chronicle panels. Sign-off: you approve the previews.
3. **Bots and balance.** Bots read both creatures, avoid their hexes, handle the pin and use HOOK. A K10 run measures deaths and food. Sign-off: numbers you are happy with, then flash and sync.

| Layer | Files | What changes |
| --- | --- | --- |
| Firmware, new | `river-beasts.hpp` | Both creatures: movement, croak, bite, snap, pin, HOOK, respawn. Saved to `/save/river.bin` like `kane.bin`, so no `SAVE_VERSION` bump. The turtle holds the pinned survivor's id, so `Player` doesn't change. |
| Firmware, hooks | `Esp32HexMapCrawl.ino` | New events (croak, bite, snap, grab, release, hooked), a catfish death cause, `PROTO_VERSION` 14 |
| | `world-system.hpp`, `actions_game_loop.hpp` | Init beside Kane's; the world-tick step after Kane's; HOOK branch in the Forage action |
| | `survival_state.hpp` | Bite or snap when you step onto a creature, the grab when you step onto its bank; a pinned survivor's move is refused |
| | `network-sync.hpp`, `network-events.hpp`, `game-server.hpp` | Creature positions and states in sync and `/state`; event encoding |
| | `network-persistence.hpp` | `river.bin` load and save |
| | `snd-sfx.hpp` | Croak, snap and splash effects |
| | `ui-screens.hpp`, `ui-scenes.hpp`, `ui-fx.hpp` | LCD markers and cut scenes (phase 2) |
| Mock | `mock-server/server.js` | Rule-for-rule port; `dbg_river` paints a river strip and spawns both creatures |
| Web | new `data/river-beasts.js`, `renderer.js`, `network.js`, `game-data.js`, `web-assets.json` | Head and shell on river tiles, event toasts, HOOK label on the Forage button; rebuild `app.bundle.js` |
| TV | `data/observer.js`, `observer-fx.js` | Markers and splashes (phase 2) |
| Bots | `bots/config.py`, `state.py`, `navigate.py`, `causes.py`, new `river_check.py` | Danger map, pin handling, HOOK, death cause (phase 3) |

## Testing and verification

The mock can prove the rules and the client, but only the K10 can prove movement and AP costs, and the mock has no river today.

- **No river in the mock.** Its map is a fixed pattern of terrain 0 to 10, so River Channel (11) never appears. `dbg_river` paints a river strip through the mock's existing terrain-override map and spawns both creatures near a chosen survivor.
- **No movement costs in the mock.** Every mock move costs 1 AP on any terrain. It can check croak, bite, snap, pin, HOOK, respawn, events and art, but not AP costs or who may enter the river.
- **On the K10:** river entry with each of the three items; bite and snap damage afloat versus wading; the bank grab, and a pinned move refused, then freed by Endure or the 2-tick release; `river.bin` surviving a reboot; the croak on the speaker.
- **Sounds:** rendered to WAV with `scripts/sndsim/sndsim.py` first. You tune them on the desk, and I announce before anything plays.
- **Bots:** `smoke.py` checks the new constants, and a `river_check.py` oracle works like `kane_check.py`.
- **Flashing:** build through `build.ps1`, and no untested firmware goes onto the K10 in the middle of a test.

## Risks and balance

The biggest risk is the combo: a survivor grabbed on the bank at night can lose 3 of a starting 7 LL in about 30 seconds.

- **Lethality.** The grab takes 1 LL, then Momma Kevin arrives and bites from the water for 2 more plus 2 rad. A wader on the water can still lose 4 to a snap and a bite, but isn't held. The brakes are the croak warning, Momma Kevin's rest after a bite, and Grandma Clamp letting go after 2 world ticks. Phase 3 measures it.
- **Flooded District becomes dangerous ground.** Survivors walk it at 3 AP without a boat, so once the catfish can reach it they count as wading. This touches players who never go near the river. If it is too much, the catfish could enter only hexes a storm has flooded.
- **Food spike.** One HOOK gives 10 food, against 2 for a river forage, or 4 with the Fishing Pole. With a 7-day respawn there is at most one catfish kill every 7 game days for the whole server. A ghost riding you takes its third, as with any forage.
- **Bots walk in blind until phase 3.** Bots that raft or fish will take bites they can't see coming. Treat bot numbers from phases 1 and 2 as unmeasured.
- **Protocol 14.** The web client and the bots have to update together with the firmware.

## Decisions and open questions

Twelve decisions are settled, and every open question below is answered.

| Date | Decision |
| --- | --- |
| 2026-10-26 | Momma Kevin bites a survivor Grandma Clamp has pinned on the bank, at the wading rate (−2 LL, +2 rad) |
| 2026-10-26 | Names: the catfish is Momma Kevin, the turtle is Grandma Clamp |
| 2026-10-26 | Grandma Clamp's snap costs 1 LL afloat and never pins on the water; it pins only on the bank |
| 2026-10-26 | A wader's bite doubles the rad too: +2 (read from your tick) |
| 2026-10-26 | A failed HOOK from the bank bites you (read from your tick) |
| 2026-10-26 | Momma Kevin swims all connected Flooded District, not only storm-flooded hexes (read from your tick) |
| 2026-10-26 | The turtle's snap pins you (read from your "yes"; later narrowed to the bank) |
| 2026-10-26 | The catfish can follow into Flooded District |
| 2026-10-26 | The Fishing Pole can kill the catfish for 10 food |
| 2026-10-26 | Fishing Pole waders are targets and take double damage |
| 2026-10-26 | Catfish bite on a Raft or Floaty Disaster: −1 LL, +1 rad |
| 2026-10-26 | Two river-only creatures: a mutant catfish and a mutant snapping turtle |

- [x] Does double damage for waders double the rad too (+2), or only the LL?
- [x] Does a failed HOOK from the bank bite you, or only when you are on the water?
- [x] Does the turtle's snap cost 1 LL afloat, or only pin you?
- [x] Should the catfish enter all connected Flooded District, or only hexes a storm flooded?
- [x] What are they called in game, on toasts and in the chronicle?
- [x] Should Momma Kevin bite a survivor Grandma Clamp has pinned on the bank? I've assumed yes, at the wading rate (−2 LL, +2 rad).
