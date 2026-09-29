# How the world ended — the 1966 / 1999 encounter series

Thirteen standalone encounters, each a dated witness account. They are ordinary
pool encounters (random draw, no chaining), so a survivor meets them out of
order and assembles the year themselves. Every one is a 3-node chain like the
rest of the pool; the last node pays out and often carries the key line.

## Canon

- **The end: Friday 11 November 1966, 11:47.** The game is set ~60 years on
  (22,000 days). Anything clock-shaped in the series reads 11:47.
- **Cause is never settled** (see [meridian-engine-spec.md](meridian-engine-spec.md)).
  Each witness holds a different reading: the official war, nothing was
  launched and *we fired*, a measurement the war "wrote down", the birds knew,
  and the town that refuses to pick one ("never pick one — the answer may move
  in"). Do not add an encounter that confirms one.
- **The Rift: 31 Dec 1999, 00:00.** Thirty-three years of "nothing happened"
  ended when a vertical seam opened in the sky and on the ground. Through it
  it is still 1966. Reading offered by the series: the world's one attempt to
  go back and do it again properly. Survivors do not go through. Offerings are
  left at the line.
- Recurring motifs: 11:47, the Emergency Broadcast tone (*this is a test / this
  is not a test* — both), **THE NEW ONE** (the fourth figure), "sixty years",
  the sky "without stars", things that arrive before they leave.

## The series

| Date | Pool | File | Title | Voice |
|---|---|---|---|---|
| Jan 1966 | scrub | `scrub/22` | Tomorrow Starts Here | attendant's logbook; time capsule sealed till 2000 |
| Apr 1966 | dunes | `dunes/8` | The Birdless Spring | farm wife's jam-jar ledger |
| Aug 1966 | urban | `urban/25` | The {{adjective}} Shelter Sale | department store, mannequin family |
| Sep 1966 | forest | `forest/4` | The Evacuation Special | conductor; 412 children, destination Meridian County |
| Oct–Nov 1966 | mountain | `mountain/13` | The Duty Officer | bunker log: no launch detected, *we fired* (drops Doomed Diary) |
| Nov 1966 | ridge | `ridge/4` | The Tuning Gallery | engineers; test #42, "the instrument is observed" |
| 11 Nov 11:47 | glass | `glass/4` | 11:47 | the fused kitchen, the radio |
| 11 Nov night | marsh | `marsh/14` | The Last Picture Show | drive-in, one reel of the audience |
| 12 Nov 66 – 31 Dec 99 | flooded | `flooded/5` | The Meridian Ledger | 33 years of NOTHING HAPPENS, last edition: OPEN |
| Present | settlement | `settlement/22` | The {{adjective}} Pageant | annual retelling, three endings, a fourth child |
| 31 Dec 1999 | urban | `urban/26` | The Y2K Bunker | prepper; six neighbours walk into the line (drops Doomed Diary) |
| 31 Dec 1999 | settlement | `settlement/23` | The Millennium Vigil | the countdown party |
| 31 Dec 1999 | glass | `glass/5` | The Seam | the Rift itself; the kitchen, whole |

File numbers are the slots at the time of writing. Other sessions add to the same
pools (the Perambulator took `scrub/21`, `glass/3`, `ridge/3`), so check
`index.json` rather than trusting this table.

## Mechanics used

Nothing new. Standard `cost`/`base_risk`/`skill`, `ll` / `radiation` / `food` /
`water` / `scrap` penalties and wounds, existing `*_common` / `*_rare` loot
tables. One new table, `drop_1966_diary` in `loot_tables.json` (the Doomed Diary,
item 25 — reveals a 3-hex radius), used on two finales. Dialogue uses straight
single quotes; text uses no backslashes.
