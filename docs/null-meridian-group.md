# The Null Meridian Group — story and items

Status: **phase 1 built 2026-09-28: compiled and mock-verified, not flashed or
synced.** Fits the sixth outer slot of
[meridian-engine-spec.md](meridian-engine-spec.md), which is currently the
"Beamline" placeholder. The draft said phase 1 needed no firmware. That was
wrong: nothing could gate a choice on an item, so phase 1 added one small gate
(see [What was built](#what-was-built-phase-1)).

## The story

Eleven staff of the prewar resonator tuning group stood in the observation
gallery the day the Engine fired, and looked straight at the field while it ran.
It looked back. What walks the wasteland now is their *pattern*, still trying to
finish a measurement that cannot be finished. Their flatbed, **the Perambulator**,
has no driver's seat and arrives before it leaves. Wherever it stops it plants a
tuning fork, and the hex goes wrong.

Their leader, Dr. Varga-Okoro, is a standing wave: reflections, radio hiss, the
gap between two lightning flashes. There are eleven names on the roster, but
you keep counting twelve. The extra one is whoever is reading it.

**Tone:** the spec's — clinical and sacred at once. No lens reads them as
obviously wrong (Machine: a maintenance crew. Ritual: a cult of measurement.
Resonance: the only ones who understand. Prison: people working the lock on
something that should stay shut. Mirror: the notes are about you).

**The wrongness** (encounter text, no new mechanics needed):
a door that opens onto a listening dark; your shadow lagging, then staying;
radios that say what you will say later; Lichtenberg burns whose branches match
the map; a tent holding your own grave pile, still warm; scrap that recounts
differently each time; static that has grammar.

**The rule underneath:** the more you understand, the more of you is out there.
Every instrument below works by looking at something too closely.

## The site

**The Null Camp** — the sixth outer site (replaces the Beamline placeholder).
Surface only, so it avoids the underground `encLoadFile()` limit. The
Perambulator is a later, optional roaming encounter that pays out the same
items. Quest shape (3 stages): reach the camp, read the roster, choose which
instrument to sound at the node — each lens has a right answer and a wrong one,
and a wrong one costs progress, not life (per the spec's setback rule).

## The four instruments

| Id | Item | Category / slot | What it is |
|----|------|-----------------|------------|
| 66 | **Strange Tuning Forks** | key | Five forks that don't match: three tines, a tine bent back into the handle, one with no tines that still rings. One per lens. Unlocks fork-tagged choices at Blooms and nodes; the wrong fork makes the hex answer back. They keep humming in your pack after you set them down. |
| 67 | **Upside-Down Pendulum** | key | A bob on a rigid rod rising from the floor, swinging toward whatever does not belong. Reveals nearby anomalies. Circles your own feet if you are the anomaly (undecided belief). |
| 68 | **Inside-Out Microscope** | key | Eyepiece faces inward: you see the target's view of you. Adds lens-tagged loot and "what this remembers" on examine. The smallest organism is always looking at the observer, and counting. |
| 69 | **Dirty Doctor's Glove** | equipment, hand | Left-hand surgical glove, never sterile. Lets you *palpate* a survivor (heal a wound tier), a corpse (how it died), a Bloom (which fork), or an echo (it has a pulse, slower than yours). Every use adds a stain; the more stains, the worse it chooses. |

### Simplifications from the earlier discussion

- Dropped: Faraday Shroud, Gauss Spectacles, Entangled Radio, Uncertain Ration,
  Extra Chair, Observer's Notebook. Any can come back as a second wave.
- The forks are **one item** that cycles lens at the moment of use, not five ids.
- Stains, the "seen" tally and "the glove chooses" are **text that escalates in
  the encounters** for phase 1, not counters. Real counters need a `Player`
  field and a `SAVE_VERSION` bump.
- The glove reuses what already exists: an equipment item with a dawn `rad`
  cost, plus encounter choices gated on carrying it.

## Draft `items.cfg` entries

Only documented keys; effects reuse `reveal_fog` and `narrative`. Ids 66–69 are
free (highest in use is 65). Tune values with the bots.

```
[item]
id       = 66
name     = Strange Forks
category = key
slot     = none
stack    = 1
trade    = no
value    = 0                 # story item — encounter choices check for it

[item]
id       = 67
name     = Upside Pendulum
category = key
slot     = none
stack    = 1
trade    = no
value    = 0
effect   = reveal_fog
param    = 4                 # bearing on what does not belong, 4-hex radius

[item]
id       = 68
name     = Inside-Out Scope
category = key
slot     = none
stack    = 1
trade    = no
value    = 0
effect   = narrative
param    = 0                 # client text hook; examine choices check for it

[item]
id       = 69
name     = Dirty Glove
category = equipment
slot     = hand
stack    = 1
trade    = no
value    = 0
rad      = +1                # applied each dawn while worn — it is never clean
```

Names are kept under the 15-character limit.

## What was built (phase 1)

**Items** (`data/items.cfg`, `ITEMS` and `ITEM_MODS` in `data/game-data.js`,
badges `I[66]`–`I[69]` in `data/item-icons.js`), as drafted above except:

- **68 is named "Inward Scope".** "Inside-Out Scope" is 16 characters, and
  `boot-assets.hpp` cuts names at 15.
- `narrative` param 0 has no handler on the server or the client, so using
  the Scope only shows its text.
- The Pendulum's `reveal_fog` 4 is a key-item effect, so it is free and never
  spent: a 4-hex survey every time it is read.

**The item gate.** A choice can carry `"requires_item": <id>`. It opens only
for a survivor who has that item. **Equipment counts only while worn**, so the
Glove's dawn Rad buys something. Anything else counts anywhere in the pack.
Nothing is spent. Four copies, kept in step:

| Where | What |
|---|---|
| `encounter_engine.hpp` | `EncChoice.reqItem`, parsed in `encResolveChoice()` |
| `inventory_items.hpp` | `playerHasForChoice()`, the rule |
| `network-msg-encounter.hpp` | `encRunChoice()` refuses before the cost: err "That choice is not open to you", nack `no_item` |
| `data/ui-encounter.js` | `hasForChoice()` **hides** the choice. It isn't shown greyed out. Buttons are numbered by what's on screen, and `ci` is still the choice's index in the file. |
| `mock-server/server.js` | `hasForChoice()` plus the same refusal in `encChoice()` |
| `bots/encounters.py` | `has_for_choice()` and `EncounterRun.first_open_choice()`. `SurvivorPolicy` takes the first *open* choice, not choice 0. |

Authoring rule (checked by the generator and `bots/smoke.py`): the **first**
choice at every node with choices is ungated. So nobody is ever stuck, and
anything that still sends choice 0 blind never hits a refusal.

**The Perambulator** runs in three biome pools: `scrub/21`, `glass/3` and
`ridge/3` (`index.json` counts are now 21/3/3). Hex-map Phase 5 places each
file once per map, so there are three chances per map to meet it. Each
sighting pays all four instruments across its branches, at most three per
visit (`ENC_MAX_ITEMS`). There are fork and scope choices at the arrival, and
pendulum and glove choices on the flatbed. Nothing stops a survivor from
taking a second copy of an item they already hold.

**The Null Camp** is `data/encounters/meridian/null_camp/{1,2,3}.json`. These
files **cannot be reached yet**. They wait for the Meridian load path. Each
file carries a top-level `"meridian": {site, stage, of, hint}` block for that
path to read, which the firmware ignores today. The choices carry `"lens"`
tags, and the key nodes carry `text_by_lens` in the spec's format (these stay
unresolved until belief is built). Stage 1 pays the Forks and the Pendulum.
Stage 2 pays the Scope and the Glove. Stage 3 has four fork choices, one tine
per lens. The Pendulum, Scope and Glove each settle the node first and lower
the risk (75 → 55). Anyone without the Forks can only hum the note, at 85. A
wrong sounding ends the attempt with +1 Rad, a setback and not a death. The
"right tine per lens" waits for belief.

All six files are generated by `scripts/gen_null_meridian.py`. Edit the
generator, not the JSON: re-running it overwrites all six. It enforces these
rules: the file stays under the 16 KB `ENC_FILE_CAP`, node keys are under 24
characters, every node is reachable, and the first choice is never gated.

**Starting items.** `grantRandomStartItem()` now draws only `trade = yes`
items. Before, it excluded key items only, so a new survivor could start
with the Dirty Glove. The same change keeps out the recipe-only outputs that
items.cfg already says never drop (Raft, Backpack, Canteen and the 11
concoctions).

**Not done:** flash, `sync_data`, a bot run over the new content, stain and
"seen" counters (phase 4), and fork choices at Blooms.

## Build order

1. `items.cfg` entries above; check `narrative` `param` values against the
   client's existing use before picking one.
2. The Null Camp encounter chain in `data/encounters/meridian/null_camp/`
   (the spec's not-yet-built load path applies).
3. Perambulator roaming encounter.
4. Optional firmware: stain/"seen" counters, glove-chooses-the-patient.

## Open questions

1. ~~Forks: one cycling item (recommended) or five?~~ One item. It "cycles"
   as one gated choice per lens.
2. Pendulum: points at hazards (overlaps traps) or only at the strange?
3. Should the glove ever refuse to come off? (Suggested: at high stain, and
   only the forks free it.)
4. Do the four form a bonus set, or stay independent?
