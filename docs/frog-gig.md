# FROG GIG — the night-wade mini-game

Status (2026-10-26): **designed with the user, no code yet.** The feel was
tried out in two throwaway prototypes in chat (`frog_gig_feel_prototype`,
top-down, then `frog_gig_first_person_v2`). Neither is saved in the repo. The
firmware, mock and web client know nothing about gigging yet.

Decided with the user:
- **You need a Flashlight.** It's a starter recipe: 3 Sparky Bits.
- **Slow is the whole game.** You see the pole, and you have to move it
  slowly. Move too fast and the frog jumps. Some frogs are jumpier than
  others.
- **First person.** You see the top of a hand curled around a pole at the
  bottom of the screen, and a vague, dimly lit bank with two glowing eyes. You
  also see the end of the gig and the circle of the flashlight. You ease the
  tip closer and closer, then press a button to strike.
- **Character skill and player dexterity together decide the outcome.**
  Forage drives both: how steady the pole is, and the roll.

## The scene

It's night, in the Marsh. The view is first person, looking down the pole:

- **Bottom of the screen.** The top of a hand curled around the pole. The
  hand is wet knuckles, wrapped rag and grime, drawn like the rest of the
  cast (textured, hatched, junk-dense), not a flat glove.
- **The pole.** It runs from the hand up into the dark and ends in the gig, a
  three-tined spear head bent from rebar or forks.
- **The bank.** A vague, dimly lit bank: reeds, mud, black water. It's almost
  nothing until the flashlight passes over it.
- **The flashlight.** A soft circle of light that follows the tip. Inside it,
  the bank has colour. Outside it, the bank is shapes.
- **The eyes.** Two glowing eyes on the bank, catching the light. They blink.
  When the frog gets nervous, its throat pulses, it crouches lower and it
  blinks faster. Those tells are how you read it.

Art direction is the project's usual mix: 80s comic dark, amber terminal UI
and manga cut-ins, all drawn in code rather than bitmaps.

## The Flashlight

It's a starter recipe, so every survivor knows it from spawn. That's the
"recipe that's given to you". The planned ids are **recipe 21** and **item
80**. Check that both are still free before using them.

```
[recipe]
id          = 21
name        = Flashlight
output_item = 80
mat1        = 22             # Sparky Bits
matqty1     = 3
starter     = yes            # a bulb, a cell, a switch -- anyone can
```

- Crafting works like any other recipe: on a Settlement hex, for 1 AP, with no
  roll (`doCraft`, `actions_game_loop.hpp`).
- `starterRecipeMask()` ORs starter recipes into every loaded save, so no
  `SAVE_VERSION` bump is needed. The mock mirrors this in `STARTER_RECIPES`
  (`mock-server/server.js`). `data/game-data.js`'s `RECIPES` array must stay
  byte-for-byte identical to `recipes.cfg`.
- Sparky Bits (22) are a high-value trade good (value 3). Three of them is a
  real price for a light.
- **Slot (open, see below).** The proposal is a pack item, `slot = none`. You
  only need to carry it to gig. There is a single HAND slot, and a hand-held
  light would fight the Fishing Pole, the Bender Strat and every weapon for
  it.

## How a wade plays

Proposed, not confirmed: one wade costs **2 AP** and gives you **3 frogs**,
in the **Marsh**, at **night**, with a Flashlight in the pack. You face the
frogs one at a time. For each one:

1. **Approach.** Drag to move the tip. The drag is relative, so your finger
   never covers the frog. The pole sways on its own, and lower Forage means
   more sway:

   `sway amplitude = (4 - Forage) * 3.6 + 3` px

2. **Nerve.** The frog has a nerve value from 0 to 1. Moving the tip faster
   than a calm pace near the frog raises it. It drains back down while you
   hold still.

   ```
   q     = max(0, 1 - dist/250)                  # only matters up close
   gain  = frog.g * max(0, speed - CALM) * q^2 * 0.02 * (lit ? 0.7 : 1.3) * dt
   nerve = nerve + gain - frog.rec * dt
   ```

   Here CALM = 35 px/s. A frog sitting in the light is steadier than one at
   the edge of it. At nerve 1 the frog jumps, it's gone, and there's no roll.

3. **Jab.** Press the separate jab button (Space on a keyboard). If the
   frog's nerve is above 0.5, the jab itself can startle it: it bolts first
   with a chance of `(nerve - 0.5) * 1.6`.

4. **Aim.** Where the tip lands sets the aim bonus. `zone` is the frog's
   strike zone, from the frog table below.

| Tip distance from the eyes | Aim bonus |
|---|---|
| ≤ 0.5 × zone | +4 |
| ≤ zone | +2 |
| ≤ 1.5 × zone | 0 |
| ≤ 2 × zone | −2 |
| > 2 × zone | splash: a miss, no roll |

### The frogs

These are the constants from the prototype. Tune them on the device.

| Frog | Eye gap | Eye r | Strike zone | DN | Nerve gain | Recovery | Food |
|---|---|---|---|---|---|---|---|
| Bullfrog | 15 | 3.6 | 16 | 8 | 0.55 | 0.30 | 2 |
| Leopard | 10 | 2.8 | 12 | 9 | 1.1 | 0.22 | 1 |
| Peeper | 6 | 2.2 | 9 | 10 | 1.9 | 0.15 | 1 |

You can tell a bullfrog by its wide-set eyes. That's also a hook into the
**Toll Frog** (`data/encounters/marsh/7.json`): the van-sized bullfrog that
speaks with Ferris's voice. Its kin are watching.

## The roll

The server decides. The client plays the scene and reports how it went, and
the K10 rolls the standard check:

```
resolveCheck(pid, SK_FORAGE, frog.dn, aimBonus)
  = 2d6 + Forage + checkSkillMod(Forage) + aimBonus  vs  DN 8 / 9 / 10
```

- `broadcastCheck` shows the dice on the K10 and the TV, the same as any
  other skill check.
- **Hit:** the frog's food goes into `inv[1]`. `ghostTithe(pid, 1, yield)`
  runs first, so a ghost riding you still takes its third. Every inventory
  change gets a targeted `item_result` ack.
- **Miss, jump or splash:** nothing. The AP was spent when the wade began.
- The Compound Bow's `NAR_LAND_FORAGE` doubling **does not** apply. It's a
  spear, not a bow.

Forage counts twice: once in the sway, which shapes the aim the player can
manage, and once in the roll. A good forager with shaky hands still lands
some frogs. A poor forager with steady hands gets a fair chance too.

## Time and rest

Gigging has to fit the day as it already runs. Here is how the code handles
time today:

| Fact | Where |
|---|---|
| A day is `DAY_TICKS` 3000 × `TICK_MS` 100, so **5 real minutes**. | `Esp32HexMapCrawl.ino` |
| Night is `dayTick ≥ 0.78 × DAY_TICKS` (`KANE_DUSK_TICK`), so **only the last 66 s** (3:54 to 5:00). | `kane.hpp` |
| The clock never pauses, not even during an encounter. | `tickGame()` |
| When every connected player is resting, **dawn fires immediately**, which skips the night. | `actions_game_loop.hpp` `tickGame()` |
| Dawn force-ends any open encounter (`ENC_END_DAWN`). | `tickGame()` |
| AP refills only at dawn. `effectiveAP` drops as LL drops. | `dawnUpkeep()` |
| At dawn, 1 food token is eaten **before** the rest-heal check (F ≥ 2, W ≥ 2). | `survival_state.hpp` |
| The Marsh has SV 0. Sleeping there without a shelter costs **2 LL** to exposure. Exposure never takes the last point. | `TERRAIN_SV`, `dawnUpkeep()` |
| Resting under a shelter on your hex cancels all LL loss at dawn. | `dawnUpkeep()` |
| Kane makes camp at dusk and doesn't shoot after dark. | `tickKane()` |
| Only Survey and Rest refuse while you're resting. | `doSurvey`, `doRest` |

What that means for the wade:

- **The night is the timer.** You get 66 s to take 3 frogs, and the slow
  approach is racing first light. On screen, the sky greying at the top of the
  bank and the eyes dimming are the countdown.
- **You have to save AP for it.** AP comes back only at dawn, so the 2 AP for
  a wade must be held through the day. You need to be standing in the Marsh
  before 3:54 with them still in hand.
- **Rest skips the night.** A solo player who rests skips straight to dawn,
  so the order is: day → Marsh → dusk → wade → REST.
- **In a group, the gigger holds the night open.** As long as you aren't
  resting, everyone else's REST can't end the day. They wait at most 66 s.
- **No gigging while resting** (proposed). Without that rule, you could rest,
  start a wade, and the others resting would end the day halfway through a
  jab. Refuse it the same way Survey does (`ABW_RESTING`).
- **Dawn ends the wade** (proposed). The eyes go out, any frog you haven't
  struck is lost, and the AP stays spent. This matches how dawn ends an
  encounter. Refuse to start a wade in the last ~15 s of night.
- **The catch pays off the same morning.** The frog you gigged is the food
  dawn eats. It can lift F to 2 and unlock the rest heal: +1 LL, or +3 when
  you're at LL 2 or less.
- **Sleeping where you gig costs you.** The Marsh is SV 0, so bedding down
  there means 2 LL to exposure and the slept-rough cut scene. Walking out
  after dark costs AP. A good gig night is: pitch a shelter on the Marsh hex
  during the day, wade at dusk, then rest under it.
- **Kane leaves you alone.** He makes camp at dusk, so the wade is the one
  time on the open map he won't shoot.
- **Waiting for dusk is dead time** (open). A solo player who's done by 2:00
  has about 2 minutes to stand around before the frogs come out. One fix is a
  "Wait for dark" state on a Marsh hex: it counts toward the all-resting check,
  but when anyone is waiting, the clock jumps to `KANE_DUSK_TICK` rather than
  to dawn. Weather only changes at dawn, so a jump to dusk doesn't touch it.
  Kane would make camp as normal.

## Firmware and wire (to build)

- **Action.** `ACT_GIG`, dispatched next to `ACT_FORAGE` in
  `actions_game_loop.hpp` (`doForage` is the template). It checks, in order:
  - not resting
  - surface only (`depth == 0`)
  - Marsh
  - night, with more than ~15 s left
  - Flashlight in the pack
  - AP ≥ 2

  Then it spends the AP and opens a gig.
- **Gig state, one per player.** Holds the frog types (rolled by the
  server), how many are left and a deadline. Dawn, a disconnect, a death and
  a timeout all close it, the same places `endEncounter` is called.
- **Messages.** The next `PROTO_VERSION` bump (it's 13 today).
  - `gig_go` (server → client): `seed`, the three frog types, ms left until
    dawn.
  - `gig_jab` (client → server): frog index, aim bonus (clamped to −2..+4),
    and whether it was a splash. The server rolls, broadcasts the check, and
    answers with the result and `item_result`.
  - `gig_lost` (client → server): the frog jumped or bolted. No roll.
  - The server ignores repeated or out-of-order frog indexes, and anything
    after the deadline.
- **Trusting the client.** The aim bonus is reported by the client. On a home
  LAN, co-op game that's acceptable. The clamp keeps it honest enough.
- **Auto-resolve.** Bots and a "skip" button send no jabs. When the deadline
  passes, the server rolls each frog that's still open with aim 0. This is
  also the bots' only way to gig.
- **K10.** Dice go through `broadcastCheck`. Sound is open (see below).
  `sndStory(SS_DUSK)` already fires at the 0.78 mark (`ui-audio.hpp`), which
  is the natural moment for the frog chorus to start when someone is in the
  Marsh.

## Mock and client (to build)

- **`mock-server/server.js`.** Recipe 21 in `STARTER_RECIPES`, the gig
  handlers, and a debug message to jump the clock to dusk, so it can be
  tested without waiting 4 minutes. The mock has to match the firmware's rest
  and dawn rules.
- **Web client.** A new canvas scene for the wade. A GIG button on the
  icon-only amber action deck, shown only when `actAvailable()` in
  `game-data.js` agrees with the firmware's checks. The Flashlight needs a
  badge in `item-icons.js` (procedural). Recipe 21 goes in the `RECIPES`
  mirror.
- **TV observer.** Open: live wade or result only.
- **Bots.** Teach `bots/` that a wade needs the AP held back and REST held
  off until after dusk. Otherwise bots never see night: they all rest, and
  the day collapses to 3-5 s. A gigging bot slows a run by up to 66 s per
  night.

## Open questions

Recommendations are listed first.

- **Where the Flashlight lives:** pack item (`slot = none`) / headlamp (HEAD)
  / hand.
- **Whether the gig itself is an item:** improvised from the pole, no item /
  a crafted gig.
- **Where and when:** Marsh at night only / any wet hex at night / Marsh any
  time with a night bonus.
- **Price:** 2 AP for 3 frogs / 1 AP per frog / 2 AP for 5 frogs.
- **Waiting for dusk:** a "Wait for dark" state that jumps to dusk / wait it
  out in real time.
- **Partial results:** whether DN − 1 counts for anything (`doForage` treats
  it as a partial), or only clean hits count.
- **Toxic frogs:** an eye colour that means "don't eat this one".
- **The TV:** mirror the wade live, or show only the dice and the catch.
- **Sound:** a frog chorus at dusk, going quiet as the frog's nerve rises,
  played on the K10 speaker.

## Not done

All of it. No firmware, mock, web client, TV, sound, bots, item or recipe
yet.
