# ONE-HAND BONNEY — the Ash Dunes story quest

Status (2026-10-26): **built across firmware, mock and web client.**
- **Firmware** compiles clean: 1,648,473 B flash (31%), 87,312 B RAM (26%),
  PROTO 14.
- **Mock** plays both sections end to end (21/21 checks).
- **Browser:** the client was played through on the mock.
- **Not done:** nothing is flashed or synced, no bot run, and the tuning is
  unmeasured.

`scripts/gen_bonney.py` writes `data/encounters/bonney/1-2.json`; edit the
generator, not the JSON.

Decided with the user the same day:
- **She looks human**, a grotesque hag, not a creature.
- **She is always at her fire.** There is no separate sled-hauling state.
- **She stays put** (the user, 2026-10-26: "she doesn't really move around,
  she stays put"). Only finishing her quest moves her, to somewhere near a
  random settlement.

The user asked for an Ash Dunes ("ashlands") story quest in two sections.
**One-Hand Bonney** is mean, ugly, quite gross and very intimidating, maybe
even alien, with gross, odd interests. She tricks you and steals an item from
your pack, cursed items included. She sits in the waste as an unfinished
quest. Once both sections are done, she turns up somewhere else on the map,
usually near a settlement. Her special quest is someone being cooked on a
spit over a big campfire.

## Who she is

- **Big, human, and foul.** She is tall when she straightens up from her
  crank, and then she straightens some more. Her hump is like a sack of rocks
  under a coat of stitched tarp. She has:
  - skin the yellow-grey of old lard, with liver spots and boils;
  - a hooked nose with a wart on it that has hairs of its own;
  - wild grey hair under a rag, and a brass hoop through an ear stretched long;
  - teeth like a broken fence, one gold and not all of them hers;
  - breath that smells of hot pennies and lard.

  Her tongue tastes the air in your direction.
- **One hand.** Her right hand is the size of a shovel, all knuckle, with
  somebody else's rings on it and nails like oyster shells. The left sleeve
  hangs empty, knotted at the wrist with a bootlace.
- **Pinch.** Her left hand left her years ago ("got ideas"). It runs on its
  fingertips like a crab, burrows under the ash like a mole, bites, and has a
  faded tattoo of an eye on its back. It does her stealing. **That is the
  trick:** she keeps you looking at the one hand she has while the other one
  goes through your pack.
- **Always at her fire.** You never see her without it.
  - Wherever she is, the pit is dug, tyres and chair legs burn, and Turnbull
    turns on a truck-axle spit over two engine blocks, an apple in his mouth.
  - She doesn't wander. When her quest is done she loads the bonnet and
    goes, and her next fire is lit near some other town.
- **Her name.** She drags her collection and her camp over the dunes on an
  old car bonnet. The chrome letters across its front once spelled a longer word.
  Five are left, B O N N E, and someone has scratched a Y after them.
- **What she collects.** Teeth (in a muffin tin, gold ones at the front),
  toenails (in a snuff tin, "from all over"), sixty years of everybody's
  belly-button fluff, candles made of earwax, and jars of last breaths ("all
  in the wrist"). Above all she loves **curses**: she can smell one in a pack,
  and it's the first thing Pinch goes for.
- **Alien, maybe.** In the flesh she is human, except for her eyes. They are a
  muddy gold, and they don't blink enough. She came down out of the bright on
  the Friday. She was
  also born in a tent at the county fair, on what she says was the same day.
  The canon rule holds ([1966-timeline.md](1966-timeline.md)): her stories
  contradict each other, and nothing settles what she is or what ended the
  world.
- **Her voice.** Wet, slow and sweet as rot, and she talks about herself as
  Bonney. She calls you sweetmeat, dumpling, skinny, gristle. She sizes you up
  the way a butcher looks at a pig.
- **Dennis Turnbull** is the man on the spit. He's thin and polite and very
  pink down one side, and he's been there since Tuesday ("she does me low and
  slow"). He keeps apologising.

## The two sections

Both scenes sit in the `bonney` pool. As with Kane, the file number is the
section. Each opens **server-side on arrival**, never takes a POI, and holds
one reader at a time.

### 1 — Guess Which Hand (her fire, first time)

You walk into her firelight: her hex, while it holds section 1. Turnbull is
turning on the spit with a real apple in his mouth, rolling his eyes at you
apologetically. She sniffs you. Then she lets go of the crank to show you the
jars, and Turnbull says "Oh, thank God" very quietly. She wants a back tooth
("little molar, you've got loads"). Then comes
the game: she closes her one hand into a fist and says *guess which hand*.

| Node | Choices |
|---|---|
| `her_fire` | let her sniff you (Endure 35, *retch: −1 water*) · ask about the jars (Navigate 30, ***Pinch steals***) · back off (escape) |
| `the_jars` | give her a tooth (−1 LL, no roll) · watch her hand (Navigate 40, ***Pinch steals***) · spin round (Forage 45, *bitten ankle*) |
| `guess_which` | laugh and shake on it (no roll → **deal**) · grab the empty sleeve (Scavenge 45 → **caught**, *slapped*) · slap a hand on your pack (Endure 50 → **caught**, *bitten, **Pinch steals***) |

There are two endings. Both count as section 1 done, and both end with her
going back to her crank ("come back for supper"). She stays put, and the
same fire now holds section 2:

- **`deal`.** While she pumps your hand, Pinch robs you. If it hasn't stolen
  anything earlier in the scene, it steals now. It runs off across the ash
  holding `{{stolen}}` over its head like a trophy. She pays you with a
  pickled egg (+1 food) and says "come back for supper".
- **`caught`.** You catch Pinch by the wrist in your own pack, and everything
  it took in this scene comes back. She's delighted ("quick eyes, Bonney
  likes eyes") and leaves you a jar (`dunes_common`).

The easy line through her game always gets you robbed. Keeping your things
takes a hard roll. Backing off is the escape node: nothing changes, and
anything Pinch took before you backed off stays taken.

### 2 — Supper at Bonney's (the spit)

This is the user's special quest. It takes place at the same fire, burned
down now to a red roar: a pit in the ash the size of a car, full of burning
tyres, chair legs and something that was once a piano.
Over it, on two engine blocks, turns a spit made from a truck's back axle.
On the spit, trussed with fence wire, is Dennis Turnbull. Wedged in his mouth,
the way you would put an apple in a roast pig, is **`{{apple}}`: the thing
Pinch stole.** She has swapped it for the apple he had in section 1. Pinch turns the crank, and Bonney stirs gravy in a hubcap with
a car aerial.

| Node | Choices |
|---|---|
| `the_fire` | sit at her fire (Shelter 30, *blistered*) · walk round to the spit (Endure 35, *hot fat*) · back out (escape) |
| `the_spit` | wave scrap at Pinch (−1 scrap, Scavenge 30, *thumb bent back*) · kick the crank (Endure 45, *shin*) · ask Turnbull about her knots (Navigate 35, *Bonney back early, gravy: −1 LL −1 food*) |
| `the_wire` | cut the wire (Scavenge 45 → **rescue**) · heave the axle off (Endure 50 → **rescue**) · wait and make her an offer (Navigate 20 → `the_offer`) |
| `the_offer` | take off your boot (−1 LL, a big toenail, no roll → **bargain**) · go for the wire while she talks (Scavenge 55 → **rescue**) |

Again there are two endings, and both finish the quest:

- **`rescue`.** Turnbull comes off the spit and hands you `{{apple}}`: "I
  believe this is yours. Or somebody's." **The stolen item goes back into
  your pack**, and if it was a curse, the curse comes back with it. Bonney
  screams, laughs, tips the gravy on the fire and loads the bonnet. You also get
  +1 med and a `dunes_rare` jar.
- **`bargain`.** She takes a toenail, tips Turnbull off the spit "like a tray
  of roast potatoes", and **eats `{{apple}}`**: "Afters." The item is gone for
  good. **This is the way to be rid of a curse.** You get +1 water from
  Turnbull's bottle as he runs past.

Backing out of the firelight is the escape node: "He'll keep. Bonney does
him low and slow."

## The loop

```
 her fire: section 1 ──(read)──► the same fire: section 2 ──(read)──► away ──(next dawn)──► her fire: section 1
 (she stays put)                 (she stays put)                     (ash ring       (lit 2-4 hexes from a
                                                                      left behind)    random settlement)
```

Bonney is **one world-wide figure with one shared stage**, like Kane. One
survivor's ending moves her on for everybody. She never finishes for good:
every pass ends with her turning up somewhere new.

**She stays put, and she is never anywhere without her fire.** The fire *is*
her hex. The map always shows the same sticker of her cooking at the spit.

- **Section 1 (mode 0).**
  - **Opening it.** Stepping onto her hex opens section 1.
  - **The call.** Coming within 3 hexes of her fire, she calls to you once a
    day (`w: call`, a line from `BONNEY_LINES`, unicast).
- **Section 2 (mode 1).**
  - **When.** After a section 1 ending she goes back to her crank. She
    doesn't move: the same hex now opens section 2.
- **Away (mode 2).**
  - **When.** After a section 2 ending she is gone at once, leaving a ring of
    ash on the hex for a day (cosmetic).
  - **Where she comes back.** At the next dawn her fire is lit 2-4 hexes from
    a random settlement, never the town she was just run out of if there is
    another. She prefers Ash Dunes ground, then Open Scrub. If nothing fits:
    any Ash Dunes hex, then any of her ground.
- **Her ground.** Ash Dunes, Open Scrub, Rolling Hills, Broken Urban and
  Marsh. Never a Settlement, water, Flooded District, River Channel,
  Mountain, Glass Fields, Nuke Crater, Rust Forest, a hatch, a vent, an armed
  trap, or a hex a survivor is standing on.
- **A flood** (or a river move) under her fire is the one thing that moves
  her unfinished. At dawn she goes to the nearest of her ground, still on
  the same section.
- **The first time** (world generation) she is placed on an Ash Dunes hex 3-8
  hexes from a settlement, with the same fallbacks.
- **The count.** `n` counts finished passes, for the chronicle ("Bonney has
  been run off N times").

## The theft

A theft is a hazard: `"penalty": {"steal": 1}`. When a roll fails, Pinch
lifts one thing.

- **Once per scene.** After one theft, later steal hazards apply only their
  other penalties (`enc.bonStole`).
- **Pack only, never worn gear.** It takes **one** of a stack.
- **What she picks**, in order:
  1. **Anything cursed:** `cursed = yes` in items.cfg (the Cursed Note, 76),
     and the Cursed Device (37), which is a curse by name though not flagged.
  2. Otherwise the most valuable thing (`value`), with ties broken at random.
  3. **Never a key item that isn't cursed.** Quest pieces such as the
     Needlecap and Brine Heart stay safe, so no other quest can be broken by
     her.
  4. If there's nothing she'll take, nothing is taken. `{{stolen}}` then reads
     "a pinch of lint from the bottom of your pack" (she collects that too).
- **Curses can't stop her.** Dropping refuses a cursed item, but stealing
  doesn't go through `dropItem()`. It removes the item directly. **Being
  robbed by Bonney, then letting her eat it, is the only way out of the
  Cursed Note.**
- **Her hoard.** What she takes goes into her hoard: 6 entries of item and
  victim seat, first in, first out. The oldest falls out and is gone.
- **The reader is told.** Each theft mutates `invType`/`invQty`, so it owes
  the reader a targeted `item_result` (act `bonney_pinch`), per the item
  result invariant. `caught` and `rescue` give back items the same way
  (`bonney_caught`, `bonney_rescue`), with any overflow going to a ground
  pile.
- **The apple** (section 2) is fixed when the scene opens: the newest hoard
  entry stolen from the reader, else the newest entry of anyone's, else
  nothing, in which case it's a real apple and `rescue` pays +1 food.
  - In a solo game it's always your own thing.
  - In a shared world you can rescue Turnbull and walk off with somebody
    else's stolen gear. That's the waste.

## Placeholders

The client fills both placeholders from the server's word, and each carries
its own determiner:

| Key | Filled with |
|---|---|
| `{{stolen}}` | "your Cursed Note" from `ev bonney` `w: pinch` for this scene; or the lint line when nothing was taken |
| `{{apple}}` | "your Lockpick" (`own: 1`) / "somebody's Lockpick" from `enc_start` `bonney.apple`; or the real apple |

The `placeholders` block in each file is only the fallback an older client
shows ("something of yours" / the apple).

## Firmware and wire

The idiom is Kane's surface figure plus the arrival-opened scenes from
VACANCY and RUNAWAY SCIENCE. It all lives in **`bonney.hpp`**.

- **World state** `OneHandBonney B`:
  - `q, r` (her and her fire, one position), `mode`, `lastQ/R` (where her
    last pass ended)
  - `ringQ/R` (the ash ring, with the day it was left)
  - `n` (the pass count)
  - `hoard[6]` = `{item, pid}`
  - runtime only: `called[]` (once a day), `packAct[]`, `saveWanted`
- **Save:** `/save/bonney.bin`, its own file, so **no SAVE_VERSION bump**. A
  save without it places a fresh Bonney.
- **Pool:** `index.json` `"bonney": {"count": 2, "path": "bonney"}` and
  `ENC_POOL_BONNEY`. The file is the section. `enc_path` and `enc_start` carry
  `"bonney": section`; section 2 also carries `apple` and `own`.
- **Hooks:**
  - `.ino`:
    - `EVT_BONNEY` (43), `ENC_POOL_BONNEY`, `SAVE_BONNEY_F`;
    - `ActiveEncounter.bonney` / `bonStole` / `bonApple` / `bonFrom`;
    - `GameEvent.encBonney` / `encBonApple` / `encBonOwn`;
    - the forward declarations, the include, and PROTO_VERSION 14.
  - `encounter_engine.hpp`: `steal` becomes `EncChoice.hazSteal`, and a
    Bonney scene never restores a POI.
  - `network-msg-encounter.hpp`: the theft on a failed roll, the ending hook
    at terminal nodes (`bonney_ending`), the item_result acks, and a walk-away
    that doesn't tick the threat clock.
  - `survival_state.hpp`: `bonneyOnArrival()`.
  - `actions_game_loop.hpp`: `bonneyDawn()` (the comeback), and a save when
    she moves stage. She has no world tick: she stays put.
  - `world-system.hpp`: `bonneyInit()` in `wInit()`.
  - `network-persistence.hpp`, `network-sync.hpp`, `network-events.hpp`,
    `network-msg-player.hpp`, `game-server.hpp` (`/state`) and
    `boot-assets.hpp` (the pool).
- **The world object** is `"bon":{"q","r","m","n","aq","ar"}`, sent in sync,
  the tick broadcast's `world` and `/state`.
  - `m` is 0 section 1, 1 section 2, 2 away.
  - `aq/ar` is the ash ring, or -1.
  - Her position goes to everyone. The client draws her only inside its own
    vision; her smoke reaches a little further.
- **`ev bonney`** is `{w, pid, q, r, item}`:
  - `w` is call / pinch / caught / supper / rescue / bargain / back.
  - `call` and `pinch` are unicast.
  - `pid` is -1 for the world's own events (`back`).
- **item_result** acts are `bonney_pinch`, `bonney_caught` and
  `bonney_rescue`. A rescue or a catch beside a full pack puts the item on the
  ground instead.
- **Reads `ItemDef.cursed`**, from RUNAWAY SCIENCE
  ([runaway-science.md](runaway-science.md)).

## Mock and client

- **Mock:** `mock-server/server.js`, the block after the clinic's, ports
  `bonney.hpp` rule for rule. It saves nothing. **`dbg_bonney`** fields
  combine, in this order:

| field | does |
|---|---|
| `reset:1` | a fresh Bonney, first placement, empty hoard |
| `here:1` (`dir:0-5`) | her fire onto the hex beside you, on that side; replies `{"t":"dbg_bonney","d"}`, so send `m d` to walk in |
| `mode:N` | 0 section 1 / 1 section 2 / 2 away |
| `give:id` | that item into your pack, for her to steal |
| `hoard:[ids]` | her hoard: these, stolen from you |
| `dawn:1` | run her dawn now (an away Bonney comes back) |

  The test script is `.claude/bonney-preview/bonney_mock_test.js`. Run it
  from PowerShell, since the Bash sandbox refuses localhost sockets:

  ```
  $env:WS_PATH='mock-server\node_modules\ws'; node .claude\bonney-preview\bonney_mock_test.js 8765
  ```

  It plays three passes, 21 checks:
  - **Pass 1:** the call, a curse-first theft that happens once per scene,
    the deal, staying put, supper with the note in Turnbull's mouth, the
    rescue handing it back, and the comeback at dawn.
  - **Pass 2:** caught, then the bargain with a real apple.
  - **Pass 3:** lint from an empty pack, and walking away.
- **Client:** `data/bonney.js`, after `lab.js` in `web-assets.json`.
  - **Her, always cooking.** There is one sticker: Bonney at the crank of the
    truck-axle spit over a pit of burning tyres. Turnbull is trussed on the
    spit with an apple in his mouth, black smoke rises, and a hubcap of gravy
    sits beside the pit (the user, 2026-10-26: "she's supposed to be cooking
    someone over a fire", then "always show her cooking at her fire").
  - **How it's drawn.** The sticker is code-drawn: `BonneyArt`, at the bottom
    of `bonney.js`. It is rendered once, at 256 px, the first time she is on
    screen, then blitted. Its textures are built at 200 px.
  - **Glow and smoke.** The glow sits over the night tint. The smoke plume is
    drawn up to 3 hexes past your vision, so you can follow it.
  - **The ash ring** she leaves shows for a day.
  - **Size.** The sticker edge is 0.75 HEX_SZ (`BONNEY_SPRITE_HEX`). That's
    half of Kane's 1.5, at the user's call (2026-10-26).
  - **Concept art** is in `.claude/bonney-preview/`: `ink.js` (the toolkit),
    `bonney.js` (her) and `camp.js` (the fire and the spit). Serve it with the
    "Bonney Preview" launch config on port 8793 (`sheet.html?p=camp`,
    `?p=map&r=112`). After changing it, run
    `python .claude/bonney-preview/build_client.py`, which rewrites the
    BonneyArt block of `data/bonney.js`.
  - **Hooks:**
    - the quests' yellow outline on her hex (`bonneyQuestAt` in
      `isPOIRenderable`)
    - the `bonney` and `bonney_light` layers in `renderer.js`
    - `ev bonney` in `network.js`: toasts (`.toast-bonney`), log lines, and
      banners for the rescue and the bargain
    - the `enc_start` branch, and `enc_path`'s `apple`/`own`, kept in
      `_bonEncPath`
    - in `ui-encounter.js`: the kicker "ONE-HAND BONNEY · GUESS WHICH HAND /
      SUPPER AT BONNEY'S", the walk-away lines, `{{apple}}` filled when the
      scene opens, and `{{stolen}}` filled on `ev bonney` "pinch"
      (`_encSetPlaceholder`)
    - `BONNEY_LINES` in `game-data.js`
- **Art:** code-drawn for now. If the user image-generates a sticker
  instead, the prompt is below.

### Her calls (`BONNEY_LINES`)

1. Psst. Sweetmeat. Over here. Bonney's got something to show you.
2. Bonney can smell you from here. You smell like pockets.
3. Come and see the jars. Nobody ever wants to see the jars.
4. Skinny thing. Come closer. Bonney won't bite. Pinch might.
5. You've got lovely teeth. Has anyone told you? Bonney's telling you.
6. Supper's on. There's always room for one more. On the plate or at it.
7. What's in the pack, dumpling? Something heavy? Something cursed? Ohh, say cursed.
8. Don't run. Running makes you stringy.
9. Bonney came down in the bright, you know. Or was born in a tent. One of those.
10. Come here and guess which hand.
11. Bonney collects things. Bonney could collect you.
12. Mind the ash. Things live in it. One of them's mine.

### Art prompt (for the user to image-generate)

> Two-panel sticker sheet, 1980s dark comic book style, heavy black inks,
> muted ash-grey and rust palette, transparent background, each panel a single
> top-down three-quarter game sticker:
> (1) a huge stooped human hag at her campfire, turning the crank of a spit
> with her one enormous knuckly right hand, stolen rings on it; her left
> sleeve empty and knotted with a bootlace; a hump under a patched tarp coat,
> a hubcap strapped on one shoulder, sallow liver-spotted skin, a hooked nose
> with a hairy wart, wild grey hair under a filthy rag, cracked goggles pushed
> up, a brass hoop earring, muddy gold eyes, licking her lips. The spit is a
> truck axle on two rusty engine blocks over a pit of burning tyres, with a
> thin polite man in underwear trussed on it with fence wire, an apple in his
> mouth like a roast pig. Black tyre smoke rises. Behind the fire is a rusted
> car bonnet piled with jars, its chrome letters reading BONNE with a
> scratched Y. A hubcap of gravy with a car aerial in it sits beside the pit.
> (2) a severed hand with an eye tattoo scuttling on its fingertips like a crab.

## Not done

- Nothing is flashed or synced. A deploy needs both the firmware (PROTO 14)
  and `sync_data`: the `bonney/` scenes, `index.json`, the client files and
  `web-assets.json`.
- The TV observer, the LCD map marker and sound. The K10 chronicle has lines
  for her events; her voice would need new LPC words.
- Pinch's own art. He exists only in the text.
- Bots don't seek her out. One that walks into her firelight takes the first
  open choice each time, which leads to the deal, so it is always robbed.

The numbers (the 3-hex call, the risks, the 6-entry hoard) are first guesses.
