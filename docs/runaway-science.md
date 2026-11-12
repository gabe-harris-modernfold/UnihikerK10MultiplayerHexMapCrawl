# RUNAWAY SCIENCE — the clinic proximity quest

Status (2026-10-26): designed and built across firmware, mock and web client,
underground only. The mock plays it end to end: 28/28 WebSocket checks
(walk into each room, pay, bank, Graft, the note through a fall), and it was
checked by eye in the browser below ground. The firmware compiles clean
(1,640,905 B flash, 31%; 87,216 B RAM, 26%; no warnings). Not flashed, not
synced, no bot run, tuning unmeasured.

The user asked for a story where three items combine into a more powerful
one: wasteland gross, strange fungus, techwires. They wanted a half-human,
half-science-experiment, a room of hypodermic needles, containers of human
parts, and a completely preserved body where only the head can move ("this
guy is runaway science"). There are three quests within 5 hexes of each
other (a proximity quest). It should be hard, because "you must only be able
to spend your points earned in a weird and twisted way". It needs three item
icons, and a cursed note that never leaves your pack and mocks you with one
of 20 curses whenever you look at it.

Mid-build the user added: "lets move this encounter to only underground".
The clinic is three rooms off the bunker tunnels, and nothing of it is on the
surface.

## The story

In 1966 Doctor Orrin Gaunt was dying slowly, "which is the worst way, because
you have time to think about it". So he thought, and what he thought of was
**Continuance**: a body that does not stop. He went in from the feet up. A
little embalming, a little wiring, a little of his culture seeded into the
blood, and the lights kept on upstairs. The orderly was to do his head after
lunch. The orderly went to lunch at a quarter to twelve on Friday 11 November
1966 ([1966-timeline.md](1966-timeline.md)), and the clock in the operating
theatre still says 11:47.

Sixty years on, the Gaunt Institute for Continuance is three rooms off the
bunker tunnels, behind steel doors. Somebody has scratched RUNAWAY SCIENCE
under the stencil on the first one. Gaunt lies on a steel table in the
theatre, perfectly preserved to the collar. His head turns on the pillow,
blinks and talks, and talks. He reaches the other rooms through the
Institute's ceiling speakers. "Nobody stopped me, you see, and then nobody could. That's
what they call runaway science."

He wants three things brought to him: the fungus from the needle ward, his
spare heart from the pantry, and the wires out of his own neck. Bring all
three back, lie down on the second table, and he will talk you through the
operation he once did on himself.

**The twist:** when the heart catches, Gaunt's head stops. His eyes are open
and his mouth is closed, and for the first time in sixty years he is still.
In your pocket, where there was nothing, is a folded note in the neat,
upright hand from a thousand jar labels. It is already laughing. Runaway
science ran away, into you.

## The points (the twisted economy)

**In the clinic, nothing you carry is worth anything. Gaunt can't eat, drink
or carry. He wants what you have done.**

- **Points** are the score: the number on THE ADMIRED board, earned by
  walking new ground, banking hauls and founding towns. Score follows the
  seat through death. Nothing else in the game spends it.
- Every scene in the clinic offers two ways past each obstacle:
  - **Pay with your body.** Roll the dice at base_risk 55-70 (DN 9-10) and
    risk LL, rads and wounds. Some body paths also cost LL or rads up front:
    a tooth for the shelf, a vein for the needles. At risk 40-50 a Guide got
    through 72-92% of the time, which was not the "difficult" that was asked
    for.
  - **Pay with your points.** A choice with `"cost": {"score": N}` and
    `"sure": true` always succeeds, with no roll. It is the only sure thing
    in the clinic. You hand Gaunt a deed ("the longest walk you ever made",
    "the first water you ever found") and it is gone. The score drops, you
    sink on THE ADMIRED, and the log says what he took.
- The Graft has a **fee of 100 points**, and nothing else will pay it. That
  is what makes the quest hard: a fresh survivor cannot afford to finish it,
  and paying for safety on the way in eats the fee.

| Where | Points path | Cost |
|---|---|---|
| Sharps Ward, the door | write your best deed on his clipboard | 15 |
| Sharps Ward, halfway | tell the speaker your longest walk | 25 |
| The Pantry, the aisle | read him the labels you'd want on your own jars | 20 |
| The Pantry, the jar | pay the butcher's card: HEART, ONE, 40 POINTS | 40 |
| The Theatre, first look | ask him a question (he charges by the question) | 10 |
| The Theatre, the cabinet | pay him to say which wire is live | 20 |
| The Theatre, the loom | give him the best night's sleep you ever had | 20 |
| The Graft, the fee | everything you did on your best week | **100** (no other way) |
| The Graft, each of 3 steps | pay him to guide your hand instead of rolling | 30 |

If you buy every sure thing, the whole quest costs 340 points. The poorest
way through costs 100 points and a lot of LL. These numbers are a first
guess and have not been measured against real play.

## The parts

Four scenes in the `lab` pool (`index.json`), written by
`scripts/gen_runaway.py`. Edit the generator, not the JSON. The file number
is the part. Like VACANCY, every scene opens **server-side on arrival**, never
takes a POI, and holds one reader per part at a time.

| # | Scene | Where | Counts when | Pays |
|---|---|---|---|---|
| 1 | The Sharps Ward | the needle room | its item is **banked** | Needlecap (72) |
| 2 | The Pantry | the jar room | its item is **banked** | Brine Heart (73) |
| 3 | The Patient | the theatre (Gaunt) | its item is **banked** | Spine Wires (74) |
| 4 | The Graft | the theatre, for a survivor carrying all three after part 3 | its `lab_ending` node is reached | Runaway Heart (75) + Cursed Note (76) |

- **Underground only.** The three rooms are cells of the tunnel board
  (`G.tunnel`, 16 x 10). They are placed at worldgen, all within `LAB_SPAN`
  (5) cells of each other (`tunDist`): each is 2-4 cells from the theatre,
  and the two outer rooms are 2-5 cells apart. Placement tries three dead-end
  rooms (`TOP_ROOM`) first. Failing that, it puts the theatre in a room and
  the other two on the corridor, and failing that, anywhere on the floor. A
  room is plain Tunnel Floor (14) with no POI (the lighting panels sit in
  rooms) and no armed trap. A world whose tunnels never opened has no clinic.
- **The call.** The first time each day a survivor below walks within
  `LAB_CALL_RANGE` (5) cells of the theatre, Gaunt calls to them over the
  speakers (`ev lab` `w: call`, unicast).
- Parts 1-3 count on the bank, not on reaching the last node. Walking off with
  the heart still in its jar leaves it there. Once an item is banked its room
  closes for everybody.
- At the theatre: part 3 opens until the wires are banked. After that, a
  survivor carrying all three opens part 4. Anyone else gets taunted once a
  day, and the taunt names what they are missing (`w: taunt`, `has` = a
  bitmask of what they carry).
- **Lost parts come back.** At dawn, if a banked part exists nowhere (no pack
  holds it, no seat wears it, no pile has it), its room reopens and the jar
  refills (`w: lost`). This stops a dropped heart left to rot from
  softlocking the quest.
- **Not a panel.** In `tunnels.hpp`, every cell of an unlit lighting section
  reads as a dark panel (`tunnelPanelDark`). So `endEncounter()` and
  `enc_bank` skip their "put the panel's POI back" step for a lab scene, or
  a clinic room would turn into a fake panel.
- **The Graft** (part 4). Its first three nodes each have a `requires_item`
  choice for 72, 73 and 74 (roll Endure) and a gated sure choice (30
  points). The fee comes last, on its own node: 100 points, sure, with no
  other way past it. It comes last so that walking off mid-operation never
  costs the fee twice. Every node keeps an ungated, sure walk-away, following
  the Null Meridian authoring rule. Reaching `"lab_ending": "graft"` spends
  one of each part from the reader's pack, then grants the Runaway Heart and
  the Cursed Note straight into the pack. Overflow goes to the ground at the
  hatch overhead, since piles are surface only. This happens there and then,
  not on the bank: a dropped connection after the operation must not lose
  the heart. The reader gets an `item_result` with act `lab_graft`. No
  points come back: he kept them. The clinic goes quiet for good.

## The items

| id | Name | Kind | What it is |
|---|---|---|---|
| 72 | Needlecap | key | the strange fungus: bruise-purple caps on a fist of needles, breathing |
| 73 | Brine Heart | key | the wasteland gross: G. - SPARE - 1966, beating once to your four, in a jar |
| 74 | Spine Wires | key | the techwires: a yard of braided copper loom with a grey bulb where it was in his neck |
| 75 | Runaway Heart | equipment, body | the graft: **+2 LL ceiling, +3 AP a day while fed 1 food, +1 rad every dawn** |
| 76 | Cursed Note | key, cursed | the note: Gaunt, folded. Can't be dropped, follows you through death, mocks you when looked at |

- 72-74 are `trade = no` key items with no effect of their own. They exist to
  be carried to the table.
- The Runaway Heart is the strongest body piece in the game: a Dent
  Absorber's ceiling plus nearly a Rust Rocket's AP. It eats before you do,
  and the fungus glows in you a little more every morning.
- **The Cursed Note.** `cursed = yes` in items.cfg (`ItemDef.cursed`):
  - `drop_item` refuses it (`item_result` ok false), and the client never
    offers to drop it (key items have no Abandon button).
  - `dropRemains()` leaves it in the pack when you fall, and the respawn path
    carries it into the next survivor's pack. It takes a slot forever. That
    is the curse's real teeth.
  - "Look at it": opening it in the pack shows a fresh curse in place of its
    story, and Read (`use_item`, effect `narrative` 13, client-only) puts
    another one up as a banner. Never the same one twice running. The 20
    curses are `LAB_CURSES` in `data/game-data.js`. `{name}` and `{pts}` are
    filled from the reader.

### The 20 curses

1. Still walking? How quaint. I had a body like yours once. I had it pickled.
2. You call that a pulse? I have seen livelier jars.
3. {name}. Even your name sounds like something I would take out and label.
4. May your water taste of pennies and your pennies taste of water.
5. Every hex you walk, I walked first. In my head. Faster.
6. Your heart beats a half-step behind mine. It always will. Keep up.
7. Remember the best thing you ever did? No? Good. I have it.
8. May every jar you open be the one with the eyes in it.
9. You have {pts} points. I have had more than that out of a single tooth.
10. Die, if you like. I will be in the next one's pocket.
11. May your boots fill with sand and your sand fill with boots.
12. I did not run away. Science does not run. It walks, in you, and you carry it.
13. May you itch where the needles went in. Forever. Just there.
14. Somewhere a jar has your name on it. The date is blank. For now.
15. May your fire go out the moment you get warm, and your shelter leak exactly onto your face.
16. Do not fold me. I hate being folded. I will remember it, and I remember everything.
17. The heart in your chest has opinions about your choices. They are all low.
18. Sixty years on a table, and I still got further than you.
19. Throw me away. Go on. I will be in your other pocket.
20. May you live forever. I mean it. I know exactly how that feels.

## Firmware and wire

All of it lives in **`lab.hpp`**, which is VACANCY's idiom (`vacancy.hpp`).
Touch points:

- `.ino`: `EVT_LAB` (41), `ActiveEncounter.lab`, `ENC_POOL_LAB`, `SAVE_LAB_F`,
  `ItemDef.cursed`, the forward declarations, the include, and
  PROTO_VERSION 13.
- `encounter_engine.hpp`: `cost.score` (`costScore`) and `"sure"`
  (`EncChoice.sure`). A lab scene never restores a POI, nor a panel's.
- `network-msg-encounter.hpp`:
  - the score cost is checked and spent with the others;
  - a sure choice skips the roll and reports `sure` on `enc_res`;
  - `labPartRead()` runs at a terminal node and `labOnBank()` in `enc_bank`;
  - the graft sends a `lab_graft` item_result;
  - a walk-away does not tick the threat clock.
- `inventory_items.hpp`: `dropItem()` refuses a cursed item, `dropRemains()`
  keeps it, and `keepCursed()` / `restoreCursed()` carry it through the
  respawn in `network-msg-player.hpp`.
- `boot-assets.hpp`: the `lab` pool and the `cursed` key.
- `world-system.hpp` calls `labInit()` from `wInit()`.
- `actions_game_loop.hpp` calls `labDawn()` (lost parts, the daily
  call/taunt reset) and saves when the quest moves.
- `survival_state.hpp` calls `labOnArrival()` from `moveTunnel()`, after
  the trap and the Dshingus checks. It is never on the surface step.
- `network-persistence.hpp` writes `/save/lab.bin`. It is its own file, so
  there is **no SAVE_VERSION bump**. A save without it keeps the freshly
  placed clinic.
- `network-sync.hpp`, `network-events.hpp`, `network-msg-player.hpp`
  (`enc_path`), `game-server.hpp` (`/state`).

Wire:

- The world object in `sync`, the tick broadcast's `world` and `/state`:
  `"lab":{"st","g","nq","nr","jq","jr","pq","pr"}`.
  - `st` is 0 open or 1 grafted.
  - `g` is what has been banked: 1 the Needlecap, 2 the Brine Heart, 4 the
    Spine Wires.
  - `n`, `j` and `p` are the needle ward, the jar pantry and the patient's
    theatre, as tunnel cells. These are always sent, and the client draws
    them below, once known.
- `ev lab`: `{w, pid, q, r}`.
  - `w` is call / needle / jar / wires / graft / lost / taunt.
  - `call` and `taunt` are unicast. `call` carries `d` (cells to the
    theatre) and `taunt` carries `has`. The others carry `part`, which is
    the part put back for `lost` and 0 otherwise.
  - `pid` is -1 for the world's events.
- `enc_path` and `ev enc_start` carry `"lab": part`, and `enc_start` also
  carries `"dp": 1`.
- `enc_res` carries `"sure":1` for a choice that never rolled.

## Mock

`mock-server/server.js` ports the rules: placement, the call, the bank rule,
the graft, lost parts, the score cost, sure choices and the cursed item. It
saves nothing. **`dbg_lab`** fields combine, in this order:

| field | does |
|---|---|
| `reset:1` | a freshly placed clinic, nothing banked |
| `here:1` | rebuild the clinic around your tunnel cell (you must be below): you stand in the theatre |
| `at:k` | you below, one step short of room k (0 ward, 1 pantry, 2 theatre); replies `{t:"dbg_lab", at, d, tq, tr}`, so send `m` with `d` to walk in |
| `got:mask` | what has been banked |
| `give:mask` | 1 Needlecap / 2 Brine Heart / 4 Spine Wires / 8 Cursed Note into your pack |
| `pts:N` | set your score |
| `dawn:1` | run the dawn's lost-part check now |

## The web client

`data/lab.js` (after `vacancy.js` in `web-assets.json`), plus small hooks:

- `renderer.js`: the `lab` layer under the pawns, a glow over the dark
  (`lab_light`), and `labQuestAt` in `isPOIRenderable`, so the rooms that
  hold the next step get the quests' yellow outline. All of it is drawn
  below only (`myDepth`).
- The rooms are drawn in code over the tunnel art until there is painted
  art. The pantry is a shelf of brine jars with one pink, beating heart.
  Each room changes once its part is taken.
- The ward is a comic-ink sticker too (`LabArt.ward`). It has an iron bed
  with a mattress full of needles, the head-sized pincushion on a trolley,
  an overflowing SHARPS bin with Gaunt's clipboard, his horn speaker on a
  stand, and clumps of needles in the floor. Every syringe is pushed in
  plunger first, point out, with amber in the barrel. While the Needlecap is
  there, its crown breathes (5 frames, `LabArt.crown`) and glows on
  `lab_light`. A fungus hair with a purple cap grows off the points
  (`LabArt.wardTips`). The hairs are drawn live so they lean toward whoever
  is nearest. Taken: a torn hole in the cushion, and the hairs hang dead.
  Without the ink kit, the old flat burr (`drawLabWardFlat`) comes back.
- The theatre is a comic-ink sticker, `LabArt` at the end of `lab.js`, drawn
  with Bonney's ink kit (`BonneyArt.kit()` in `bonney.js`; without it the old
  flat drawing comes back). The scene: a steel table, the sheeted body with
  a toe tag, a hand hanging out, a specimen cabinet, a drip stand, a lamp on
  his face, and the braided red-and-yellow loom from his neck into a box on
  the floor. His head is drawn over the sticker from 9 turn x 3 mouth
  frames. It turns to whoever is looking and talks. After the Graft it is
  one still frame. Wires taken: a frayed stub in the box that sparks
  (`lab_light`), and no blinking box lamp.
- `network.js`: `ev lab` and the `enc_start` branch.
- `ui-encounter.js`:
  - the kicker reads "RUNAWAY SCIENCE · THE SHARPS WARD / THE PANTRY / THE
    PATIENT / THE GRAFT";
  - a `score` cost chip reads "−N Points";
  - a sure choice shows SURE THING instead of odds;
  - CAN'T AFFORD is judged on points as well;
  - the log says which deed Gaunt took.
- `ui-items.js` / `ui-state.js`: the Cursed Note's curse on look and on read.
- `item-icons.js`: badges for 72-76.

## Not done

- The K10 LCD has chronicle lines but no map marker. The TV observer has no
  line bank yet. Sound reuses existing motifs and adds no new voice.
- Bots: they take the first *open* choice, and a sure choice they can't
  afford is refused (`no_res`) like any other cost. None of them seeks the
  clinic out.
- Tuning: the fees, the 5-hex span and the Runaway Heart's numbers are first
  guesses.
