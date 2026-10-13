# Team play

The rules that make survivors on the same hex matter to each other, and the
one that makes standing together cost something. PROTO 9. Firmware, mock
(`mock-server/server.js`) and web client move together for every rule here.

"Same hex" is always `samehex()` (inventory_items.hpp): same board, and
underground the same corridor cell — a survivor standing on a hatch is not
with the one below it.

## Encounter assist

A survivor on the hex of someone mid-scene can back that scene's **next roll**.

- `{"t":"enc_assist","to":PID,"res":0-4}` — costs the helper one token of
  `res` from their own pack, **no AP**.
- Each helper takes `ENC_ASSIST_DN` (1) off the roll's DN, floor 2. At most
  `ENC_ASSIST_CAP` (2) helpers per roll, one token each.
- The roll spends the help, hit or miss (`ActiveEncounter.assistMask` is
  cleared in `encRunChoice()`), so every roll has to be backed afresh.
- If the roll succeeds, each helper scores `ENC_ASSIST_SCORE` (+1).
- The helper must be seated, standing, and not in a scene of their own; the
  target must be in a scene with a roll left (not on a terminal node).
- Nacks: `self`, `downed`, `in_enc`, `no_target`, `no_roll`, `not_same_hex`,
  `dup_assist`, `assist_full`, `no_res`.
- Broadcast `ev enc_ast` `{pid: helper, to, res, n: helpers now behind the roll}`.
  `ev enc_res` carries `as` (helpers behind that roll; its `dn` already
  includes them).

It is in DN, not risk, on purpose: the original encounter spec's "−4 risk per
ally" was written for `2 + risk*10/100`; under today's `5 + risk*7/100` it is
0.28 of a DN and integer division rounds it to nothing.

**Removed:** the involuntary drain. Every hazard used to take 1–2 tokens from
everyone on the hex for nothing in return (`encDrains`, the `drains` array on
`enc_res`), which made standing beside a teammate who opened a POI strictly
worse than standing anywhere else.

Client: the ◉ ASSIST deck cell appears while anyone on your hex is in a scene
(`openAssistPicker`, ui-panels.js); the scene overlay shows "N allies have your
back" and its odds count them (`encComputeDN(me, risk, assists)`).

## TREAT a teammate

`{"t":"act","a":2,"to":PID}` — `doTreat(pid, tgt, …)`.

- The **patient's** wounds and LL decide the job: a Major Wound → TREAT WOUND
  (2 AP, Endure vs `TREAT_DN`); none → the Settlement HEAL (1 AP, +1 LL).
- Everything else is the **treater's**: the AP, the Medicine, the Medic
  privilege (close a Major Wound anywhere), the Endure roll and the score.
- The patient must be on your hex, up, and not in a scene; otherwise the act
  is blocked with `ABW_NO_TARGET` (10, nack `no_target`).
- `ev act` carries `tg` — whose `wnd` / `lld` the event reports (== `pid`
  when treating yourself).

Client: when anyone else on your hex could use it, the TREAT cell opens a
patient picker (`openTreatPicker`).

## Founding a Settlement

Unchanged triggers (3+ survivors on the hex when an improved shelter
finishes, or a triangle of improved shelters). Now every **founder** — every
survivor standing on the hex for trigger (a), or on any of the three corners
for (b) — scores `SETTLEMENT_FOUND_BONUS` (+20), not just the builder
(`creditFounders()`, actions_game_loop.hpp). `ev settle` carries `fd` (the
founders' seats) and `bonus`.

## Gathering draws the Doom

A crowd is loud. Wherever the Creeping Doom is, and whether or not it has a
scent:

- every world tick (~15 s), each surface survivor who shares a hex with
  another adds `DOOM_GATHER_TICK` (3) awareness — `tickDoomGathering()`;
- a step (or a climb out of a hatch) onto a hex another survivor holds adds
  `DOOM_GATHER_ARRIVE` (5) at once — `doomNoteArrival()`.

Against the cold decay of 5 a tick, a pair keeps it from forgetting them (+6),
three (+9) or a full five (+15) drive it up — 0 to 100 in about 2.5 minutes.
Higher awareness widens its scent radius and, past 76, sets fire to what it
crosses; the hunt still needs a live trail. Survivors mid-scene count, so
backing a teammate's roll from the doorway has a price. Underground survivors
do not count — the Doom never goes below.

The client logs "Survivors gather — the Doom feels it" at most once a minute
when you are part of a crowd (`_noteGathering`, network.js).

## Still individual

Score and THE ADMIRED ladder stay per survivor, POIs stay first come first
served, and death piles are anyone's. The bots (`bots/`) do not use any of
the above yet: no policy sends `enc_assist` or a TREAT with `to`, so arena
runs will not measure these rules until one does.
