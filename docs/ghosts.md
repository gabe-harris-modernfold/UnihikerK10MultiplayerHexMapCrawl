# Wasteland ghosts

A survivor who falls leaves a ghost on the hex where they went down. It drifts
toward the living, latches onto whoever it shares a hex with, and feeds on
them until they reach a Settlement. Firmware `ghosts.hpp`; mock
`mock-server/server.js` (search `Wasteland ghosts`); client `data/ghosts.js`.

## Rules

| | |
|---|---|
| Rises | On every death, at the survivor's surface hex (pinned to the hatch if they fell below), named for them. Rising takes 2 world ticks (~30 s) and it holds nobody yet: teammates at the body have time to scatter. |
| Drifts | One hex every other world tick (~30 s). Floats over anything, water and rock included, except a Settlement. Steps toward the nearest survivor it can haunt within 4 hexes, else wanders. |
| Latches | When it shares a hex with a survivor who is up, on the surface, not in a scene, not in a Settlement and not already ridden -- by its move or by their step. One ghost per survivor. |
| Drains | Every 8 world ticks (~2 min), one token off the biggest stack in the pack. Paused while the survivor is offline or inside a scene. |
| Tithes | A third of every gain: hex pickups (surface and tunnel), FORAGE / WATER / SCAVENGE yields, a quest haul as it is banked. Small gains count toward the next token (`tithe` carry), so it is exactly a third over time. Score is paid on what they keep. Remains looting, trades and the caravan are not touched. |
| Rides | Everywhere, the tunnels included. |
| Lets go | Walking into a Settlement: it stays on the hex they came from and shuns everyone for 4 world ticks (~1 min). Founding a Settlement under it does the same on the next world tick. Silently when the survivor goes down or their seat is picked by somebody new. |
| Fades | After 7 dawns. Its hoard drops as a Remains record (the 30-day ground age applies) on its hex, or the nearest hex a survivor can stand on. |
| Cap | 6 at once; another death fades the oldest first. |

Tuning lives at the top of `ghosts.hpp` (`GHOST_*`) and is mirrored by name in
the mock.

## Wire (PROTO 12)

- `world.ghosts` in sync, the tick broadcast and `/state`:
  `[[slot, q, r, haunt, state, hoard, src, "name"], ...]`, live ghosts only.
  `haunt` -1 drifting; `state` 0 drifting / 1 rising / 2 shunning; `hoard` =
  tokens carried; `src` = the seat it rose from.
- `{"t":"ev","k":"ghost","w":W,"g":slot,"src":S,"pid":P,"q":Q,"r":R,"res":1-5,"n":N}`
  for `latch`, `letgo`, `drain`, `tithe`, `fade` (`n` = the hoard it left).
  `rise` comes from the `EVT_DOWNED` handler and carries `"name"` instead of
  `res`/`n`.
- A haunted survivor's `col` `amt`, `act` deltas and `enc_bank` `loot` are what
  they kept.

## Persistence

`/save/ghosts.bin` ("GHST" v1): slots, names, positions, rides, days left,
hoards, tithe carry. Its own file like `kane.bin`, so no `SAVE_VERSION` bump;
a save without it loads with no ghosts. A ride on a seat that comes back
downed is let go on the first world tick.

## Mock testing

`{"t":"dbg_ghost", ...}` -- see the handler's comment. `here:1` puts one on you
and latches it; `drain`, `tick`, `dawn`, `fade`, `days`, `hold`, `clear`.
The mock does not model impassable terrain, so a fade drops the hoard on the
ghost's own hex.

## The K10

When a ghost latches on, the K10 makes it an event:

- **LCD:** a jump scare, `FXK_GHOUL` (ui-fx.hpp section 13c). Its face fills
  the glass at once under a white-and-negative flash, the jaw drops into a
  screaming maw, "SKREEE" slams in, the tube strobes, and it lunges and comes
  apart. Then the chronicle line is its caption. 30 s cooldown, priority 3.
  Preview: `python scripts/fxsim/fxsim.py ghoul`.
- **Speaker:** `SS_GHOST` -- `SFX_GHOUL_GROAN` (snd-sfx.hpp), then one of
  three narrator jokes 2.2 s later: "you make friends easily", "someone is
  very attached to you", "you will never be lonely again" (`VOC_GHOST0..2`,
  scripts/lpc/phrases.txt). Preview: `python scripts/sndsim/sndsim.py ghost`
  (and `ghost_dry`, music off).
- **Lamps:** a cold flash. The chronicle gets a line on latch, let-go and a
  hoard-dropping fade.
- **TV:** data/observer-fx.js section 13c plays the same scene, laid out for
  the wide screen (the hands hold the bottom corners). It comes off the
  board's `fx` ring; without one (the mock) `fromGhosts` cues it when a ghost
  in `/state` goes from drifting to riding someone. `?fx=reel` includes it.

## Not done

- The K10 LCD map and the observer TV's map don't draw the ghosts themselves.
- The bots don't see or avoid them; a balance run has not been made.
