# Observer Screen — spec

A full-screen HTML page you load on a TV and leave running. It watches a live
game over `/state`, picks whose story to tell, and narrates the wasteland in
the house voice.

**Encounters are the story.** Everything else on the board is weather and
arithmetic; the encounter is the only place a survivor makes a decision an
audience can second-guess. The design below treats the scene as the spine and
the survival meters as the consequences that scene leaves behind.

Status: **plan only — nothing built.** Phases and acceptance checks at the
bottom.

Companion docs: [dev-loop.md](dev-loop.md) (build/flash/sync),
[world-system-spec.md](world-system-spec.md) (weather, threat clock, Doom),
[bot-testing.md](bot-testing.md) (six live players without six humans).

---

## The constraint that shapes everything

**The observer never opens a WebSocket.** `handleConnect()`
([network-session.hpp:54](../network-session.hpp:54)) hands every `/ws` client
a player slot and there are only [6](../Esp32HexMapCrawl.ino:198). A spectator
on `/ws` eats a seat and draws itself on everyone's map.

The feed is **`GET /state`** ([game-server.hpp:401](../game-server.hpp:401)):
read-only, `Access-Control-Allow-Origin: *`, consumes nothing. The script for
each scene comes from **`GET /enc`**
([game-server.hpp:603](../game-server.hpp:603)).

### Polling contract

| Knob | Value | Why |
|---|---|---|
| `POLL_MS` | 1000 | Below the 100 ms tick, above what the board minds |
| Backoff | 2500 to 8000 on error, reset on success | The board wedges under HTTP load; do not pile on |
| `AbortController` | one in-flight request, always | A stalled fetch must not queue a second |
| `?sd=1` | **never** | `SD.totalBytes()`/`usedBytes()` are slow; it is opt-in for a reason |
| `?pid=N` | every poll, N = current camera subject | Free — see below |
| `/enc` | **once per encounter file, ever** | It takes `G.mutex` and reads SD. Cache forever; never re-fetch |

`/state?pid=N` returns the global roster **and** that player's full vision
disk in one response: every cell with `q/r/dq/dr`, `terrain`, `shelter`,
`resource`, `amount`, `footprints`, `tireTrack`, `poi`. That disk *is* the
camera. One request per poll feeds both the roster rail and the map panel.

On a camera cut, fire one extra immediate fetch with the new `pid` rather than
waiting for the next tick, or the map lags the headline by a second.

### What the roster gives us

Per player, per poll: `name`, `archName`, `q/r`, `ll`, `llCap`, `food`,
`water`, `rad`, `mp`, `wounds[minor,major]`, `resting`, `score`, `steps`,
`encActive` / `encNode` / `encCanBank` / `encLoot`, `invType[]` / `invQty[]`,
`equip[]`, `skills[]`, `conn`, `connectMs`.

World: `day`, `dayTick` (of `DAY_TICKS` 3000, so a real time-of-day arc), `tc`
(threat clock; escalates at 5/9/13/17), `weather` (indexes
`WEATHER_PHASE_NAMES`), `connected`, and map totals.

---

## The encounter is the story

111 encounter files, 721 KB, across ten biome pools plus traps. Each one is a
small branching scene with prose, named risks and a payout — already written,
already in the house voice. The observer's job is to cover them live.

### The three-line firmware change this needs

`ActiveEncounter` ([Esp32HexMapCrawl.ino:697](../Esp32HexMapCrawl.ino:697))
holds **`encIdx`** (the file) and **`terrain`** (the pool index, whose
`encPools[terrain].path` is the folder name). Together those are exactly the
`/enc?biome=<path>&id=<encIdx>` address of the scene being played.

**`/state` publishes neither.** It gives `encActive`, `encQ`, `encR`,
`encNode`, `encCanBank`, `encLoot` — enough to know *that* someone is in a
scene and which node they stand on, but not *which scene*, so the prose, the
title, the choices and the named hazards are all unreachable.

Adding `encBiome` (the resolved path string) and `encId` to the player block
is three lines beside the existing `encNode` emit. **Do this.** Without it the
observer narrates "she is in a building"; with it, it narrates the Gutted
Pharmacy, the padlocked dispensary cage, and the bolt-cutter hanging there
like an invitation.

Everything below marked *(needs `encId`)* depends on it. Everything unmarked
works against `/state` as it ships today.

### What the file gives us *(needs `encId`)*

From `data/encounters/urban/1.json`, fetched once and cached:

| Field | What the observer does with it |
|---|---|
| `title` | names the scene — "The {{adjective}} Pharmacy" |
| `nodes[key].text` | the actual prose of the room they are standing in |
| `nodes[key].can_bank` | whether they can walk out with the haul right now |
| `nodes[key].loot` | what this room is worth (`res` 0-4 = Water/Food/Fuel/Med/Scrap) |
| `choices[].label` | the doors on the table — **including the one refused** |
| `choices[].base_risk` | 15 / 20 / 30 / 40 — the live odds |
| `choices[].skill` | NAV / FORAGE / SCAV / SHELT / ENDURE |
| `choices[].hazard_id` | what is waiting behind that specific door |
| `hazards[id].text` | prose for the thing that just went wrong |
| `hazards[id].penalty` / `wound` | the signature used to identify which hazard fired |
| `hazards[id].ends_encounter` | whether that door is the one that throws them out |
| `choices: []` | empty means terminal — they are through, and only banking is left |

### The scene model

An encounter is a five-act shape the observer reads straight off the poll:

| Act | Detected by | The beat |
|---|---|---|
| **THRESHOLD** | `encActive` false→true | Lock the camera. Name the place. |
| **THE ROOM** | `encNode` value | Where they are, what it holds, what it costs to go on |
| **THE DOOR** | `encNode` changed | They got through. Name the room they chose — and the one they did not |
| **THE PRICE** | `ll`/`wounds`/`rad` drop while `encActive` | A hazard fired. Match the penalty signature to `hazards{}` and read its prose |
| **THE EXIT** | `encActive` true→false | Banked, bled out, backed out, or dawn took it |

Two ambiguities to hedge rather than fake:

- When two choices share a `success_node` (both entry doors in the pharmacy
  lead to `dispensary`) the observer knows the room reached, not the door
  taken. Say the room.
- A failed choice with `ends_encounter: false` leaves `encNode` **unchanged**
  and drops LL. That is its own beat — they are still in there, and worse.

### The greed meter

This is the centrepiece, and it is fully visible in `/state` today.

`encCanBank` says they may leave with `encLoot` right now. The next choice's
`base_risk` says what walking deeper costs. Put those two numbers on the
screen together and the audience watches somebody decide:

> three medicine in the bag. the door out is open. she is looking at the cage.

The pharmacy is exactly this trap: `dispensary` is `can_bank: true` with
medicine already in hand, and the way on is `base_risk: 40` behind
`cage_trap`, whose `ends_encounter` is `true`. Push and lose and you leave
with nothing.

Push-your-luck is the best dark comedy the game produces, it resolves inside a
single camera cut, and it needs no firmware change. **Build this first.**

### The debt ledger

Encounters are not just beats — they are the **causes** that every later beat
pays off. Four LL bought in a pharmacy on day 6 is what kills someone in a
rainstorm on day 9.

So the shadow roster's tally carries an explicit ledger: *which scene, which
hazard, what it cost, which day*. Two things read it:

- **The narrator**, so a decline has a named origin rather than a gauge: "the
  pharmacy took four off her on day six. she has been paying it back ever
  since."
- **The obituary**, which names the encounter that started the fall even when
  thirst finished the job. That is the difference between a death and a story.

### What stays invisible without `/chronicle`

`EVT_ENC_RESULT` ([network-events.hpp:452](../network-events.hpp:452)) is the
richest event in the game — `out`, `skill`, **`dn`** (the difficulty number),
**`tot`** (what they actually rolled), loot, items, `penLL`, `penRad`,
`penRes[5]`, `penWnd[2]`, `ends`, `drains[6]`, `rec` — and it is broadcast to
every `/ws` client. The observer is not one, and polling cannot recover a
roll: only its aftermath.

The firmware already knows this is the moment worth marking. On a win at
`dn >= 8` it fires a `PLATE_AWARD` reading *"DN 8 held"* with `AWD_LONG_ODDS`.
A commentator that cannot say the odds is calling a different sport. See
[Open decisions](#open-decisions).

---

## Three gaps in the feed, and what we do about them

### 1. No event stream

`/state` is snapshots. Everything is a **difference between two snapshots**,
so the observer carries a diff engine. Not purely a workaround: deltas are
what a commentator talks about anyway.

### 2. Death wipes the slot

`EVT_DOWNED` ([network-events.hpp:288](../network-events.hpp:288)) clears
`connected`, zeroes `wsClientId`, and hands the slot back to the lobby for
re-pick. From `/state` a death and a quit look **identical**.

Fix: a **shadow roster** — the last 30 samples per player, a running tally,
and the encounter debt ledger above. The samples infer cause of death; the
ledger is what lets the obituary name a room.

### 3. Slots get recycled, names repeat

The stable identity is **`pid` + `connectMs`**, both already in `/state`. Key
the shadow roster on that pair and a respawn is never confused for a recovery.

---

## Architecture

Five files, all standalone — **none of them go in `data/web-assets.json`**.
That file is the game SPA's bundle order (`build_web.ps1` concatenates exactly
what it lists); adding the observer there would load it into the game client.
`sync_data.ps1` pushes everything under `data/` recursively, so they deploy
with no extra wiring, at the cost of one HTTP route each on the board.

| File | Owns |
|---|---|
| `data/observer.html` | Markup + its own script tags. No bundle. |
| `data/observer.css` | Layout + type scale. Copy the `:root` tokens from `style.css`. |
| `data/observer.js` | Poll loop, diff engine, shadow roster, director, render. |
| `data/observer-scene.js` | `/enc` fetch + cache, node/choice/hazard lookup, greed meter. |
| `data/observer-lines.js` | The voice. Line banks only, zero logic. |

Data flow, once per poll:

```
fetch /state?pid=<subject>
  -> normalise()        roster keyed pid:connectMs, world block
  -> scene.ensure(pid)  on THRESHOLD: one /enc fetch, cached forever
  -> diff(prev, next)   -> DerivedEvent[], scene acts resolved against the file
  -> shadowRoster.push()  30 samples/player + tally + debt ledger
  -> director.consider(events, roster)  -> subject, phase, cuts
  -> narrator.emit(events, phase)       -> headline? ticker lines
  -> render(subject, roster, world, view, scene)
```

Keep the render pure against a single state object. It makes the synthetic dev
feed a drop-in replacement for `fetch`.

---

## The diff engine

Each entry emits a `DerivedEvent { kind, pid, severity, data }`. Severity
drives both the narrator's bank and the director's interrupt logic.

**Encounter** — the spine:

| Signal | Kind | Sev |
|---|---|---|
| `encActive` false to true | `THRESHOLD` | max |
| `encNode` changed | `DOOR` (name the room reached) | high |
| `ll`/`wounds`/`rad` drop while `encActive` | `PRICE` (match hazard signature) | high |
| `encCanBank` false to true | `CAN_LEAVE` (greed meter arms) | high |
| `encLoot` grew | `DEEPER` | mid |
| node has `choices: []` | `TERMINAL` (through; only banking left) | high |
| `encActive` true to false, loot reached inventory | `BANKED` | high |
| `encActive` true to false, loot lost | `LOST_IT` | max |

**Survival** — the consequences:

| Signal | Kind | Sev |
|---|---|---|
| `conn` true to false | `VANISHED` | max |
| `conn` false to true (new `connectMs`) | `ARRIVED` | mid |
| terrain under player changed between polls | `GROUND_TURNED` (flood/fire) | high |
| `wounds[1]` (major) up outside an encounter | `MAULED` | high |
| `ll` down by 3+ / by 1-2 | `HURT_BAD` / `HURT` | high / mid |
| `ll/llCap` crosses 1/2 or 1/4 downward | `FAILING` | high |
| `food` crosses 4 or 0 downward | `HUNGER` | mid |
| `water` crosses 3 or 0 downward | `THIRST` | mid |
| `rad` up | `GLOW` | mid |
| `score` up by 10 or more | `HAUL` | mid |
| crosses an `ADMIRED` row | `PASSED_THE_DEAD` | high |
| `ll` up / `score` up 1-9 / `invQty` up / `resting` on | `PATCHED` / `PICKING` / `LOOT` / `CAMPED` | low |
| `q/r` changed | *aggregate only* — steps since cut | — |
| `day` up / `weather` changed | `DAWN` / `WEATHER_TURN` (world) | mid |
| `tc` crosses 5 / 9 / 13 / 17 | `ESCALATION` (world) | high |

**`low` exists for the render, not the voice.** Those move a meter on a card
and say nothing; the narrator gets "eleven hexes today, and a rock" once, at
cut time.

`PASSED_THE_DEAD` reuses `admiredPassed(prev, now)` from
[data/game-data.js:159](../data/game-data.js:159) — it already returns exactly
the rows a score delta crossed.

### Reading the exit

`encActive` going false has four different stories behind it, and the firmware
already enumerates them as `ENC_END_*`
([Esp32HexMapCrawl.ino:709](../Esp32HexMapCrawl.ino:709)) with labels the
client mirrors at [data/network.js:1240](../data/network.js:1240) — reuse
those words. Polling infers the reason:

| Inference | Reason | The story |
|---|---|---|
| loot landed in `invQty`/resources | banked | they took the money |
| loot gone, LL dropped, hazard had `ends_encounter` | hazard | the place threw them out |
| loot gone, no damage, node was `can_bank` | abort | **they walked. on purpose.** |
| `day` incremented on the same poll | dawn | they ran out of night |
| `conn` also went false | downed / disconnect | see cause of death |

The abort row is the most under-rated beat in the game: a survivor standing in
a room full of medicine who decides it is not worth it. The voice's whole
stance is bad judgement, so the one time somebody shows *good* judgement is
worth a line of its own.

### Cause of death

Run against the shadow roster when a player `VANISHED`. First match wins:

1. last `ll / llCap` at 0.6 or above — **walked out** (a quit, not a death)
2. `encActive` on the last sample — **died in the scene**, and the ledger can
   now name the room and the hazard
3. major wounds rose within the last 3 samples — something with teeth
4. `food === 0` in 3 or more of the last 5 samples — starved
5. `water === 0` in 3 or more of the last 5 samples — thirst
6. `rad` rose across the window — the glow
7. otherwise — unknown

**The healthy check runs first.** It sat sixth in the first draft, which let a
quitter who happened to be mid-encounter collect a hero's eulogy. Nothing
cheapens a death board faster than burying someone whose battery died.

Whatever rule fires, the obituary also reads the debt ledger. Rules 3-6 name
the scene that started the decline; rule 7 is not a failure — "cause recorded
as the wasteland; the wasteland declined to comment" is the funniest line on
the page and it is honest.

---

## The director

Ranks on **`ll / llCap`**, never raw `ll`. `llCap` is `effectiveMaxLL()` and
moves with equipment, so raw LL is not comparable across players.

### The encounter lock

**An open encounter outranks everything.** `THRESHOLD` takes the camera
immediately and holds it until `THE EXIT`, ignoring `MAX_DWELL_MS` — a scene
is a complete story with an ending, and cutting away mid-room to show someone
else's water meter throws away the only three-act structure the game has.

Only two things break the lock: a `VANISHED` anywhere on the board, or a
second player opening an encounter — in which case the camera finishes the
scene it is on and the second is queued, with a one-line promise that it is
coming. The show is allowed to say "we will get to that."

If nobody is in a scene, the phase heat below decides.

### Phases

**1. THE LEDGER** — opening. Camera follows the money.

```
heat = 0.45 * norm(score)
     + 0.30 * scoreVelocity(last 60s, normalised)
     + 0.15 * recentEventWeight(last 20s)
     + 0.10 * returnBias
```

**2. THE REAPING** — the turn. Camera inverts:

```
heat = 0.50 * (1 - ll/llCap)
     + 0.25 * llVelocityDown(last 60s)
     + 0.15 * pressure(food==0, water==0, rad, wounds)
     + 0.10 * returnBias
```

`returnBias` is not garnish: pure heat ranking churns the subject and the
audience never learns a name, so a player the show has already invested in
outranks a stranger at equal heat. A recurring character is the difference
between a story and a scoreboard.

### When the turn happens

THE REAPING triggers on **the first death**, or on half or more of the
connected players sitting at `ll <= llCap/2`, whichever comes first. The
threshold alone mistimes the turn both ways: six players limping trips it
before anything has happened, and one catastrophic death among five healthy
players does not trip it at all — the exact moment an audience feels a show
change gear. First death is the dramatic event; the threshold is the fallback
for a grinding run where everyone starves quietly and nobody has died yet.

Robustness, both required:

- `PHASE_DEBOUNCE = 3` — the threshold condition must hold on 3 consecutive
  polls. (A death needs no debounce; it is unambiguous.)
- **v1 assumption: the flip is one-way.** Once THE REAPING starts it does not
  go back. A deliberate dramatic choice, not an oversight — but see
  [Open decisions](#open-decisions); this codebase has been bitten before by a
  one-way door nobody chose on purpose (the creeping doom's awareness latch,
  [world-system-spec.md](world-system-spec.md)). If it should relax, require
  the same 3-poll debounce **and** a 30 s floor since the last flip.

Compute the fraction over **connected players only**. A dead slot is not a
healthy player.

### Cutting

| Knob | Value |
|---|---|
| `MIN_DWELL_MS` | 16000 — below this it is unwatchable |
| `MAX_DWELL_MS` | 40000 — force a cut so it never stares (suspended during a scene) |
| `CUT_FLOOR_MS` | 4000 — no cut within 4 s of a cut, whatever happens |

Between `MIN` and `MAX`, cut when another player's heat exceeds the subject's
by 20 % or more (a margin, or the camera oscillates between two near-equal
players). Before `MIN_DWELL_MS`, only a **hard interrupt** preempts: severity
`high` or `max`. Still subject to `CUT_FLOOR_MS`.

---

## The narrator

**Do not invent a voice — the repo has one.**
[data/game-data.js:87](../data/game-data.js:87) states it outright: *dry,
lowercase, one line; the joke is on the survivor; nothing is ever advice;
never explain the number.* `DOOM_TAUNTS`, `TUNNEL_TAUNTS` and the encounter
lines in `drainEvents()` are all the same register.

A line drifts the moment it becomes a tip. "she should have filled up at the
marsh" is broken; "she passed the marsh at speed, on principle" is not.

`TUNNEL_TAUNTS`' design comment is the sharpest statement of the stance in the
repo: *the joke is that it is wrong*, and it stays *on the survivor's own bad
judgement rather than on any threat that is actually down there.* The narrator
is not a helpful commentator. Encounters are where bad judgement becomes
visible, which is why they carry the show.

**One rule cannot be copied across, because `ADMIRED` is written entirely
about the dead: tense.** The observer narrates the living in the present.
Reserve the past tense for the dead — then the tense shift does the work by
itself. The first time the narrator says *was* about someone still on screen,
the audience knows before the meter does.

### It speaks about the subject, not the feed

`narrator.emit()` sees every derived event; it may speak about exactly three
things: **the camera subject, the world, and a hard interrupt.** Everything
else is a meter moving and belongs in the render. Without it the ticker
degenerates into a six-player status console with jokes in it — six threads,
no thread.

### Two beat shapes

**Out in the open**, a 16–40 s cut carries at most four lines:

| Slot | Fires | Job |
|---|---|---|
| Establish | on the cut | who this is and how bad it is |
| Develop | on `mid`+ events | what the wasteland did about it |
| Turn | on a `high` event (optional) | the moment the beat is about |
| Leave | on cut-away | the joke, or the silence |

**In a scene**, the acts already supply the shape, so the narrator follows the
encounter instead of imposing on it:

| Act | The line's job |
|---|---|
| THRESHOLD | name the place, and what they were carrying when they went in |
| THE ROOM | what it holds, and what the next door costs |
| CAN_LEAVE | the greed line — the open door, and the thing they are staring at |
| THE DOOR / THE PRICE | the hazard's own prose, trimmed to one line |
| THE EXIT | the verdict — and for a bank, what the whole thing cost |

Establish and Leave stay mandatory in both. Leave is written *knowing the beat
is over*, which is where the comedy lands.

### Banks

```js
LINES.CAN_LEAVE.LEDGER  = ['the door out is open. {name} is looking at the cage.', ...];
LINES.CAN_LEAVE.REAPING = [...];
```

- Keyed `kind` × **phase**, not `kind` × severity. Severity already decides
  headline vs ticker; phase decides *meaning*. Pushing your luck in THE LEDGER
  is ambition and it is funny. Pushing it in THE REAPING is the ending.
- Slots: `{name} {arch} {n} {terrain} {item} {day} {score} {scene} {room}
  {risk} {haul}`.
- **The scene's own prose beats a template.** When the file has a line for
  this moment — the hazard text, the room text — trim that to one clause and
  use it. The banks fill the gaps between written prose, they do not replace
  it. 111 files of hand-written scene text is the largest body of voice in the
  project; a generic bank line on top of it is a downgrade.
- **Continuity comes from the ledger, not the tick.** "third day without
  water" is a story; "water low" is a gauge.
- **Anti-repeat:** per bank, a ring of the last `min(3, len-1)` indices; pick
  seeded by `(pid ^ tickId)`, then linear-probe past the ring.
- 4 lines per bank minimum.

### Reuse `ADMIRED` verbatim

[data/game-data.js:110](../data/game-data.js:110) is 36 rows of finished
one-liners in exactly this voice, already scored. Each roster card shows the
rank title the player currently occupies plus `admiredNextAbove(score)` — the
name they are climbing toward. A live player at 3,180 sitting directly under
THE AVERAGE MAN ("*got exactly this far, like almost all of you. admired for
the punctuality*") does more comedic work than anything written fresh.

`PASSED_THE_DEAD` gets the headline to itself: the passed name, its line, and
nothing else on screen for four seconds.

### The death is the set-piece

Everything else is build-up. A `VANISHED` that resolves to a death holds the
headline for 12 s — longer than anything else earns — and the roster card
keeps that obituary permanently, with the room from the ledger in it.
`ADMIRED` is the proof this voice is at its best writing epitaphs.

Everything else paces at one ticker line per 4 s, headline 8 s, priority
queue, dropping unread `mid` lines older than 20 s. No LLM in the loop:
deterministic, offline, on-voice, surviving the board's Wi-Fi hiccuping.

---

## Screen

Built for three metres away: no hover, no scroll, `overflow: hidden`, type
scaled off `vw` so 1080p and 4K both work untouched.

The subject panel has **two states**. Out in the open it shows survival; in a
scene it becomes the stakes board and the meters shrink to a strip.

```
+----------------------------------------------------------------+
| DAY 14  [====day arc====]   STORM   THREAT ####----   ALIVE 4/6 |
+-------------------------------------+--------------------------+
|  IN THE GUTTED PHARMACY             |  THE FIELD               |
|  RUSTBUCKET3 / SCAVENGER            |  +--------------------+  |
|                                     |  | 1 MULE2      3,180 |  |
|  the dispensary cage is padlocked.  |  |   THE AVERAGE MAN  |  |
|                                     |  +--------------------+  |
|  IN THE BAG   med x3                |  | 2 ...              |  |
|  >> CAN WALK OUT NOW <<             |  +--------------------+  |
|                                     |  | + dead: greyed,    |  |
|  FORCE THE CAGE .......... 40% SCAV |  |   obituary line    |  |
|  SEARCH THE SHELVES ...... 30% SCAV |  +--------------------+  |
|                                     |                          |
|  LL [######----] 6/14 F### W# RAD## |                          |
+-------------------------------------+--------------------------+
| HEADLINE: the door out is open. she is looking at the cage.     |
| ticker . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
+----------------------------------------------------------------+
```

- **CAN WALK OUT NOW** is the loudest element on the screen when it is lit.
  It is the whole gag: the audience knows the exit is open and watches the
  decision anyway.
- Risk percentages come straight from `base_risk`; the skill name from
  `choices[].skill` (NAV/FORAGE/SCAV/SHELT/ENDURE).
- Out of a scene, this panel reverts to the big LL bar, the meters, and the
  hex vision disk from `?pid=`.
- **Palette:** copy the `:root` tokens from
  [data/style.css:10](../data/style.css:10) — amber-on-black stencil.
- **Hex disk:** `TERRAIN[]` ([data/game-data.js:188](../data/game-data.js:188))
  carries `fill`, `stroke`, `icon` and `name` per terrain, so a flat-colour
  map works with zero art loading.
- The dead never leave the rail. The board filling up with them is the arc,
  and it is the only element on screen that only ever moves one way.

---

## Hosting and the dev loop

**Serve it from the board** as `data/observer.html`, so `host =
location.origin` and the TV never needs an IP typed into it with a remote
control. The board registers a route per discovered `data/` file
([game-server.hpp:368](../game-server.hpp:368)), so the page itself needs no
firmware change. A `?host=` override keeps desktop development easy, and the
board's dynamic IP stops mattering — the TV bookmarks whatever URL it uses.

Deploy: `.\scripts\sync_data.ps1 <board-ip>` picks the new files up
automatically (it walks `data/` recursively).

### The mock has no `/state`

`mock-server/server.js` serves `/ws`, `/upload`, `/enc` and static `data/` —
**there is no `/state` route** (only a comment at `mock-server/server.js:128`
referring to one). Note it *does* already serve `/enc`, so the scene layer is
testable offline the moment `/state` exists. Development needs:

1. **`GET /state` in the mock**, mirroring the firmware's JSON from the mock's
   own `players` map — including `encBiome`/`encId` if the firmware gets them,
   or the scene layer cannot be integration-tested at all.
2. **A synthetic feed** (`?feed=fake`) — a scripted six-player run that walks
   two players through a real encounter file and kills everyone on a timer.
   The only way to test the acts, the greed meter and the obituary
   deterministically, and the way to tune line banks with no hardware.

---

## Build phases

### Phase 1 — feed and roster *(no director, no voice)*

- `observer.html/css/js`, poll loop with backoff and `AbortController`.
- Shadow roster keyed `pid` + `connectMs`: samples, tally, empty ledger.
- World rail + six roster cards with LL/food/water/score.
- `?host=` override; `?feed=fake` synthetic run.

**Accept:** 10 minutes against the mock without a leak or a stalled fetch; a
player quitting is retained as "gone", not silently dropped.

### Phase 2 — the scene *(the spine; do this before the director)*

- Firmware: `encBiome` + `encId` on the `/state` player block.
- `observer-scene.js`: one `/enc` fetch per file, cached forever.
- The five acts, hazard-signature matching, the exit inference table.
- **The greed meter and the stakes board** — `encCanBank` + `encLoot` + the
  next `base_risk`, as the loudest thing on the screen.
- The debt ledger written on every `PRICE`.

**Accept:** a player walking the pharmacy start-to-bank produces all five acts
in order, names the room reached at each `DOOR`, identifies the hazard that
fired, and the ledger holds what it cost. No `/enc` file is fetched twice.

### Phase 3 — the camera

- Director with both phases, both triggers, debounce, dwell, interrupts.
- **The encounter lock**, including the queued-second-scene promise.
- Hex vision disk from `?pid=` for the out-of-scene panel.

**Accept:** the turn fires on the first death and never flickers; no cut is
shorter than `MIN_DWELL_MS` except on a logged hard interrupt; no scene is
ever cut away from except by a `VANISHED`.

### Phase 4 — the voice

- `observer-lines.js`, 4+ lines per bank, anti-repeat ring.
- Both beat shapes; scene prose preferred over bank lines where it exists.
- `ADMIRED` rank titles; `PASSED_THE_DEAD`; obituaries naming the room.

**Accept:** a 20-minute synthetic run repeats no line inside any 60-second
window; every cut has an Establish and a Leave; no living player is described
in the past tense; nothing reads as advice.

### Phase 5 — polish, on real hardware

- Real hex art, day/night tint, weather badge.
- 4K check, burn-in avoidance (drift the layout a few px on a slow cycle).
- Verify against a live K10 with `bots/` driving six players —
  [bot-testing.md](bot-testing.md) warns that a `pick` can be silently refused
  and `enc_start` silently dropped without `q`/`r`, which on this screen would
  look like a scene that never opens. Watch for it.

**Accept:** two hours unattended, no reload, heap flat on `/state` `mem.heap`,
and `/enc` fetch count equal to the number of distinct encounters played.

---

## Open decisions

1. **`encBiome` + `encId` on `/state`** — three lines, and the difference
   between narrating a building and narrating the Gutted Pharmacy. The
   recommendation is to do it before Phase 2; everything else in the plan is
   client-only.
2. **`/chronicle` — now the strong option, not the escape hatch.** A 32-entry
   ring buffer of recent `GameEvent`s behind a JSON route hands the observer
   `EVT_ENC_RESULT` whole: the DN, the roll, the skill, the exact penalties,
   and the real `ENC_END_*` reason instead of five inference rules. Encounters
   being the spine is precisely what raises its value — the roll is the most
   narratable instant in the game and polling cannot see it.
3. **Is the phase flip one-way?** Planned one-way for v1, flagged so it is a
   choice and not an accident.

---

## Known data gaps

Facts about the feed, found while planning.

- **`/state` omits `encIdx` and `terrain`** — see Open decision 1. Without
  them every scene is anonymous.
- **`{{placeholders}}` resolve per run.** `"The {{adjective}} Pharmacy"` is
  Gutted on one run and Bleached on the next; the file holds the list, not the
  pick. An observer choosing its own would contradict the player's screen. Use
  the bare noun ("the pharmacy") unless the resolved title is added to the
  wire — or accept a deliberate mismatch and never show both.
- **`view.cells[]` has no `variant`.** The renderer picks tile art with
  `cell.variant` ([data/renderer.js:682](../data/renderer.js:682)); the
  `?pid=` payload omits it. Use variant 0, hash `(q,r)` (looks right, is
  wrong), or add the field.
- **`view.cells[].poi` is booleanised.** The cell stores `enc.encIdx` in
  `poi`, so the map knows which scene sits on a hex — but `/state` flattens it
  to true/false, losing a free way to label the POIs the camera can see.
- **`terrainName` is empty for terrain 12-15.** `TNAME_FULL`
  ([game-server.hpp:402](../game-server.hpp:402)) is declared `[NUM_TERRAIN]`
  (16) but initialised with 12 strings, so hatch and tunnel terrains index
  `nullptr`. Harmless — `String::concat` null-checks — but take names from
  `TERRAIN[]` client-side.
- **`/enc` takes `G.mutex`** (500 ms timeout) and reads SD. One fetch per file
  ever, cached; never poll it.
- **`invType[]`/`invQty[]` need `items.cfg`** to become names. Parse it once
  at boot for the `{item}` slot, and `loot_tables.json` for what a named
  `loot_table` can pay out.
