# THE LISTENING POST: environmental awareness (no camera)

Status (2026-11-06, second pass): **design only, no code.** Nothing here is
decided by the user yet. Every *(rec)* is a recommendation waiting for a yes.
The camera is out of scope by request.

## What the second pass changed

I checked the first draft against the code and the bot logs. Eight things
did not hold up:

1. **Night gating would have hidden almost everything.**
   - Night is only the last 66 s of a 5-minute day (dayTick ≥ 0.78).
   - The day also ends early the moment every connected player is resting
     (`tickGame()`, the `allResting` check), so night is often skipped
     entirely.
   - HUSH is no longer night-only. The rest of the night rules are gone.
2. **Room noise must never raise the Doom's awareness.**
   - Awareness widens its smell radius: `6 + awareness/25`.
   - It adds steps: 2 at 76, 3 at 100.
   - From 76 up it sets a fire on every tick, scent or not.
   - Noise "nudges" would have ended up as fires. A loud camp now only stops
     the Doom forgetting.
3. **An empty room would win every ritual.** Put the box in a closet and it
   is permanently dark and quiet. So rituals now reward a deliberate act,
   meaning a change someone has to make, never a state.
4. **The board can't hear the room over itself.** The mics are 23 mm from
   a hot 2 W speaker. So every ritual silences the engine and the LEDs, and
   ambient sound only counts when the board can prove it is hearing the
   room and not itself.
5. **Conversation is not "≈ 0".** Table talk sits roughly 15–25 dB above a
   quiet room's floor. "Quiet" and "loud" now need two different baselines.
6. **Button A is volume, not the FX level** (`checkFxButton`,
   `ui-screens.hpp`). Both buttons fire on the press edge, so an A+B chord
   would also change the volume and the screen. The chord is dropped.
7. **The accelerometer runs at 10 Hz** (the library writes reg 0x20 =
   0x27). It cannot see a knock, so knocks are heard by the mic only.
8. **Turning on the mics has costs I had missed.**
   - Duplex roughly doubles the I2S DMA in internal RAM (~+18 KB).
   - The mute path skips the RX drain.
   - It turns the existing `/sndrec.wav` loopback tool into a room
     recorder that anyone on the network can open.

## The idea

The K10 sits on the table. **The room it is in is the camp.** The fiction:
the K10 is the camp's salvaged radio, and its open mic leaks across the
whole waste. Anything out there that listens can hear the camp. That is why
the room affects world-level things (the Doom, the weather, ghosts) and
not one hex. It is also why it doesn't matter where each survivor is
standing.

**The interface contract: the phone arms it, the box performs it, the
server judges it.**

- A player (or the server itself, for HUSH) opens a short **box task**: a
  window of a few seconds in which the room has to do something.
- The people at the table do it to the box: cover it, knock, flash a torch,
  tilt it.
- The server decides from the readings and resolves the outcome.

Content (encounters, quests) uses box tasks through data and never touches
sensor code. There is only one box, so there is only one box task at a
time.

## Ground rules

1. **Nudges, never verdicts.** No reading on its own kills, wounds or takes
   anything.
2. **Reward the act, not the state.**
   - A ritual passes on a change someone made at that moment: lux falls
     sharply, the box is turned over, knocks arrive in a pattern.
   - A room that is simply dark or silent proves nothing, because an empty
     closet is dark and silent.
3. **The room's normal is neutral.** Readings are measured against the
   room's own baselines. Talking, lights on and normal heat cost nothing.
4. **If players can't see an effect or cause it on purpose, it doesn't
   exist.** Every coupling is announced on screen when it acts.
5. **OFF is today's firmware.** OFF runs the exact current audio path
   (fmt 7, transmit-only) with no RX DMA and the mics dark.
6. **Turning on the mics is a physical act.**
   - The mode changes on the box itself, never from a phone.
   - The LCD shows an ear glyph whenever the mics are live.
   - No audio is stored or leaves the board.
7. **Nobody is locked out.** Every box task has an in-game alternative.
8. **Testable.** Bots, the mock and `/envdbg` can inject readings.

## What the numbers say

**How often would HUSH fire?** I took the `doom_warn`/`doom_act` events
from 34 bot runs in `bots/runs/` (2026-09-21..10-13), deduplicated them by
`sq`, and counted an episode as events less than 2 minutes apart:

| Mode | Runs | Hours | Doom-adjacency episodes / hour |
|---|---|---|---|
| realtime (5 bots) | 17 | 3.4 | **1.2** |
| sprint (5 bots) | 17 | 4.3 | 3.3 |

- About 93% of `doom_act` events lost LL, which means the Doom was at
  awareness 100 when it arrived.
- At human pace, HUSH would come up about **once an hour**. That is the
  right rarity for a headline beat. With fewer, quieter humans it will be
  rarer.
- Night-gating it would have cut that to roughly once a session at best.

**Doom levers** (`world-system.hpp`):

| Lever | Value |
|---|---|
| Scent gain (`AWARENESS_GAIN`) | +12 per world tick (15 s) |
| Cold decay (`AWARENESS_DECAY`) | −5 per world tick |
| `SCENT_THRESHOLD` | 4 |
| Track decay | −1 per world tick |
| Heat laid | 2 per AP spent |
| Smell radius | `6 + aw/25` |
| Steps per tick | 1 / 2 / 3 at aw <76 / 76 / 100 |
| Fires | ignites its hex every tick at ≥ 76, scent or not |
| At 100 with scent | ignores the trail and goes for the nearest player |

The Doom causes about 1% of deaths (`project_tension_measurement`).
Softening it with HUSH costs almost no balance. What it buys is the
experience.

## What the board has (verified 2026-11-06)

| Sense | Part / where | State today | Use here | Gotchas |
|---|---|---|---|---|
| Light | LTR303-ALS, I2C 0x29. The case has a Ø2.4 window at (34.9, 81.05). | `k10.begin()` sets gain 1, 100 ms integration, 500 ms rate. Not read by the game. | Comfort dimming. The cover/lights-off transition in HUSH. Torch pulses (needs 50 ms integration while a window is open). | It sees the K10's own LEDs. In a pitch-dark room, a hand over it *raises* the reading, because it reflects the LCD back. Face-down is the fallback that always works. |
| Temp / RH | AHT20, I2C 0x38. The case has a Ø1.8 hole at (42.4, 81.0). | The library class is never created. Its read blocks ~80 ms. | Damp/dry for weather (P3). | It sits about 6 mm from the S3 can. Deltas only. Correct heat soak with `temperatureRead()`. |
| Motion | SC7A20H, I2C 0x19, at **10 Hz**. | The library's `gesture_task` (core 1, prio 5) polls it. `getAccelerometerX/Y/Z()` and `isGesture()` are available, and `isGesture()` clears its latch when read. | Pose (face-down, upright, flat), handling, tilt for the Dial. | Cannot see knocks. Raising the ODR changes the library's gesture thresholds. |
| Sound | 2 top-port MEMS mics, 23.4 mm apart, I2S0 RX on GPIO 39. Case holes Ø1.6. | **Off.** Fmt 7 (`tx-i2s-x3-48k`) is transmit-only. The library's own install is 16 kHz duplex and does no I2C setup for the mic chain. | Silence, loudness, onsets (knock or clap). | Needs a `duplex-i2s-x3-48k` format. Whether the mic ADC is happy at 48 kHz is **untested**. Face-down muffles the ports against the table. |
| Buttons | A, B (XL9535) | B steps the 6 screens. A steps the volume. Both fire on the press edge. | A on the new AIR screen changes the mode. | No chords. |
| Radio | STA + soft-AP | Running. | Parked (presence by RSSI). | No scanning. |
| Clock | NTP (`network-session.hpp`), `rtcSynced` | UTC only. | Small hours, moon. | The local offset comes from a phone. |
| Expansion | Gravity P0 / P1 / I2C | Unused. The case flag `GRAVITY_PORTS` opens its side walls. | Probes (P6). | Check that the pins are free. |

## Architecture

```
LTR303 ─┐
AHT20  ─┼─► env.hpp sampler (task, core 1, prio ~3, 50 ms; no blocking I2C)
accel  ─┘          │
mics ─► audio task: features → atomics (also in the muted path)
                   ▼
            g_env (EnvSnapshot, published like g_dread)
                   │
     ┌─────────────┼───────────────┬──────────────────┐
     ▼             ▼               ▼                  ▼
 box task      world rules     comfort (LED      wire: gs "env",
 (HUSH, knock, (tickCreeping-   ceiling, LCD     /state "env",
 lamp, dial)   Doom, weather    dim, AIR         /envdbg
     │          roll, ghosts)   screen)
     ▼
 enqEvt → drainEvents → fxCue / sndStory / chronicle / encounter resolve
```

**The mode picks the I2S format.**

- OFF installs fmt 7, transmit-only, exactly as today.
- SENSE and LIVE install `duplex-i2s-x3-48k`.
- The switch reinstalls I2S between writes, using the same path
  `/sndtest?fmt=` already uses. One click at a settings change is
  acceptable.

**Mic features run in the audio task.**

- The task already drains RX on every pass in duplex.
- Today the `s_audioVol == 0` branch `continue`s before the drain, so at
  volume 0 the mics would never be read. The muted branch has to drain and
  measure too, paced by `vTaskDelay`.
- Per 16 ms block:
  - decimate 48 kHz to 16 kHz
  - a 100 Hz high-pass
  - an RMS envelope
  - an onset detector (spectral-flux-lite: broadband energy rise over
    ~3 ms against the trailing 100 ms)
  - the output RMS of the same block, taken from `sndOutput`, for gating
- The results are written to atomics, the same way `sndRxRms` is today.

**I2C.** The sampler, the library's `gesture_task` and the XL9535 button
reads all share Wire. Arduino-ESP32 2.x (IDF 4.4) holds the bus lock across
a repeated start, so mixed readers are safe. Every read stays under 2 ms.
The AHT20 is triggered, then read on a later pass.

**Box task.** This is one global slot owned by `env.hpp`:
`{kind, armedBy, startMs, deadlineMs, phase, refLux, result}`.

- Priority: HUSH > encounter gate > lamp / dial.
- HUSH **preempts** an open gate. The gate's player gets their choice back
  with nothing spent: "THE TERMINAL GOES DARK. SOMETHING'S COMING".
- While a task is open, the engine is silenced except for its own cue, and
  the LEDs are off.

### Self-interference

| Source | Remedy |
|---|---|
| Speaker → mic | Every box task silences the engine. Ambient loudness is measured only on blocks where the engine is silent. Otherwise it uses `room = mic − k·tx` (energy), with `k` fitted per volume level using `k10loop.py`. If the leftover error at the current volume is over a limit, `loudOk = 0`. The board admits it can't hear the room: the AIR screen reads **EARS FULL**. |
| LEDs → ALS | LEDs are off during any box task. For comfort, the LED share of the reading is fitted online by regressing lux against total LED drive. The lamp cues change the LEDs often enough for the fit to converge. |
| LCD → ALS | The backlight is constant, so the baselines absorb it. Hand-cover in a dark room reads brighter, so face-down is the universal "cover". |
| Heat → AHT20 | 30-minute warm-up, then subtract the die-temperature slope. Only deltas against the 24-hour median count. |

### Baselines (NVS namespace `env`, saved hourly)

| Name | What | Used by |
|---|---|---|
| `floorSilent` | 10th percentile of room level, learned **only while the engine is silent** (volume 0, box tasks). Seeded by ATTUNE. | HUSH fail threshold, onsets |
| `norm` | Median room level over 10 minutes. | "loud" |
| `luxNorm` | Median lux over 10 minutes. | Comfort |
| `rhMed`, `tMed` | 24-hour medians. | Damp/dry |
| `k[vol]` | Speaker-to-mic coupling per volume level. | `loudOk` |

For the first 2 minutes after boot, every derived flag reads neutral.

## The vocabulary: `EnvSnapshot`

Only what a consumer actually reads:

```cpp
struct EnvSnapshot {
  uint8_t  mode, ok, reliable;   // OFF/SENSE/LIVE; live-sensor bits; loudOk etc.
  int8_t   dbOverFloor;          // room level vs floorSilent, -20..+60
  bool     loud;                 // ≥ norm + 12 dB for ≥ 60% of the last 15 s (needs loudOk)
  uint32_t onsetMs[8];           // ring of recent onset times (knock/clap)
  uint8_t  onsetN;
  uint16_t lux;                  // LED share removed
  uint8_t  pose;                 // FLAT / UP / FACEDOWN / HELD
  int8_t   tiltX, tiltY;         // half-degrees, smoothed
  int8_t   rhDelta, tDelta;      // vs 24 h medians (%RH, half-degC)
  bool     smallHours;           // real local 01:00-04:00
  uint8_t  moon;                 // 0-29, 255 unknown
};
```

The lamp's fast lux samples live inside the box task, not in the snapshot.

## ATTUNE: calibration that teaches the game

The first time LIVE is turned on (and again from the AIR screen), the
board runs a 40-second walkthrough. It gives the detectors their room
numbers and teaches the table its vocabulary at the same time:

1. **"SILENCE." 10 s.** The engine is silent. This seeds `floorSilent`.
2. **"TALK." 10 s.** Normal table talk. This sanity-checks `norm`.
3. **"COVER ME." 5 s.** Hand or face-down. This measures the cover drop
   and the muffling.
4. **"KNOCK THREE TIMES."** This measures onset level and shape.

A failed step says which sense didn't answer. Then SENSE stays usable and
that sense's features are flagged unreliable.

## Layer 0: comfort and the AIR screen (SENSE and LIVE)

**The AIR screen (LISTENING POST)** is a 7th LCD screen in the B cycle,
drawn as amber gauges:

- SIGNAL (the room level against its floor and norm)
- LUX
- DAMP
- POSE
- the mode
- EARS FULL when `loudOk` is 0

On this screen only, **button A steps the mode** (OFF → SENSE → LIVE)
instead of the volume, and a toast confirms it. That makes turning the mics
on a physical act at the box. Because this adds a firmware screen, check
`docs/dev-loop.md` first.

**Comfort:**

- **LEDs:** `s_ledBright` is a ceiling. A dark room scales the LEDs down
  from there, never up.
- **LCD:** the backlight is on/off only, so `fxPresent()` scales the image
  in a dark room, with a 40% floor. Face-down turns the backlight off.
- **Sound:** never louder than the owner's volume.

## Layer 1: ambient (LIVE only, small on purpose)

| Room | Effect | Shown as | Hook |
|---|---|---|---|
| **Loud camp** (`loud`, needs `loudOk`) | The Doom's cold-trail decay is skipped that world tick. It doesn't forget. Awareness is never raised. | LCD and phones: "THE WASTE HEARS THE CAMP" (once per loud spell) | `tickCreepingDoom()` decay branch |
| **Damp** (`rhDelta ≥ +8`) | The dawn roll tilts by up to +10 points toward RAIN/MIST. | Dawn line "THE AIR IS WET TODAY" | `updateWeatherPhase()` |
| **Dry** (`rhDelta ≤ −8`) | The same tilt, toward CLEAR. | "BONE DRY" | the same |
| **Small hours** | Ghosts take +1 step per tick. | "THE DEAD WALK LATE" | `tickGhosts()` |
| **Moon** | The stash sky's broken moon matches the real phase. | — | `ui-scenes.hpp` |

**Cut:**

- **Warm → fire:** nobody could see it or cause it.
- **Bang → awareness:** it would turn into fires (point 2 of the second
  pass).
- **Gone dark as an ambient bonus:** the closet exploit.

## Layer 2: rituals

### R1 HUSH: cover the radio until it passes *(the headline)*

- **Call (server-armed, any time of day).**
  - The Doom is at awareness ≥ 76, and it will reach a surface survivor
    next tick: `hexDistWrap ≤ doomStepsPerTick() + 1`.
  - The same exclusions as `resolveDoomProximity()` apply: connected, not
    in an encounter, not underground, not in an oasis.
  - Cooldown 3 minutes, and at most one call per approach.
- **On the call.**
  - The EYES cut-in, held as a scene, with the lettering **COVER THE
    RADIO** and then **HUSH**.
  - One whispered TMS5220 line, then the engine goes dead silent. A sudden
    silence is an alarm in itself.
  - The LEDs go off.
  - Phones and the TV show "COVER THE RADIO" and the countdown; the TV is
    in the room too. Phones also buzz where the browser allows it.
- **The Doom freezes** for the window. Its movement and proximity effects
  skip those world ticks: it has stopped to listen.
- **Test: two phases, both acts.**
  1. **Cover, within 8 s.** It counts if lux drops ≥ 70% from `refLux`
     (the reading just before the call), or the pose turns FACEDOWN. This
     is the act that proves someone is there: room lights off, a hand over
     it, or turning it over.
  2. **Hold, 15 s.** It stays covered, and no sound gets over the fail
     threshold (`floorSilent` + the muffling ATTUNE measured + 15 dB).
     Muffling is accepted. Smothering the radio is the fiction. A shout
     still gets through.
- **Results:**

| Outcome | Effect | Chronicle |
|---|---|---|
| **Held** | The Doom **loses the scent**: every track within its radius is cooled to `SCENT_THRESHOLD − 1`, and awareness −30. At 100 that lands at 70, below the fire band. It wanders off until someone acts nearby again. | "THE RADIO HELD ITS BREATH" |
| **Broken** (covered, then a sound got out or it was uncovered) | Awareness +10 (cap 100), and one free step toward the nearest survivor. | "IT HEARD" |
| **No answer** (no cover in 8 s) | Nothing. The approach plays out as it does today. | — |

- **Why:** the room's behavior *is* the beat. Someone dives for the box and
  everyone shushes. An empty room can't win, because nobody covers the box.
  The self-hearing problem disappears, because the engine is silent.
- **Bots:** with no injection, bots never cover the box, so there is no
  answer and their runs are unchanged. The harness injects cover and hold
  to A/B the effect.
- **Mock:** the mock has no scent model, so "held" just steps the Doom away
  with awareness −30. Divergence noted.

### R2 KNOCK: secret codes on the box or the table

- **Detection.**
  - Mic onsets only; the accelerometer is too slow. A clap counts the same
    as a knock.
  - The window is silent (no engine, no LEDs), so the onset detector hears
    only the room.
- **Pattern.**
  - Knocks inside a group are < 600 ms apart. Groups are > 900 ms apart.
  - The whole code fits in 8 s. Codes have at most 3 groups of at most 5
    knocks, for example `3-2` or `1-4-1`.
- **Codes are seeded per world** from `G.seed`, so they have to be found
  again in every world. They are taught in encounter text and found notes:
  "the hatch answers to three, then two". No save change, because they are
  derived from the seed.
- **Flow.** The player picks the knock choice on their phone. The box task
  opens for 15 s and the LCD shows LISTENING. The table knocks. The server
  matches the pattern.
- **Refused** in the last 15 s of the day and while HUSH is open.

### R3 SIGNAL LAMP: a torch, blinked at the box

- **Detection.** While the window is open, the ALS runs 50 ms
  integration at a 50 ms rate. On/off durations become short and long
  pulses. The alphabet is small: SOS plus a few 3-pulse codes. The LEDs
  are off.
- **Uses:**
  - Call the caravan, which turns toward the caller.
  - Mark your hex for everyone (team play).
  - Answer a gate ("two long at the mast").
- **Cost:** a lit camp is a beacon. The Doom gets +8 awareness and ghosts
  are drawn. This is the one ritual allowed to raise awareness, because the
  player chose it knowing that.
- **Risk:** contrast in a bright room. A phone torch at 1 m may add only
  10–30% on top of room light. P4's exit test settles it.

### R4 THE DIAL: tune the radio

- **Flow.** A player taps TUNE on their phone, which arms it, and the
  reward goes to them. Then someone tilts the box to sweep the band (the
  accelerometer at 10 Hz is plenty for a smoothed tilt).
- **Sound and screen:** the engine plays static. The LCD draws a needle.
- **Signals:** the server seeds 2–3 signals, refreshed every 15 real
  minutes. Holding steady on one for 3 s locks it, and the TMS5220 reads
  the broadcast:
  - a cache (reveals a POI hex for everyone)
  - Kane's next stop
  - a Doom whisper
  - a lore fragment
- **Size:** it needs no mics, but it is the biggest content build.

### Parked

- **Shake → tremor:** it fires by accident whenever someone handles the
  box.
- **Breath:** cute, but niche.
- **Two-mic direction finding:** 23.4 mm is about 3 samples at 48 kHz, so
  left/right is the most it could give.
- **Presence by phone RSSI:** only works in direct-connect play.

## Layer 3: encounter gates (the real "interface")

A **choice** gains an optional `env` field. Everything else in the schema
stays as it is:

```json
{ "label": "Knock on the hatch the way the note said.",
  "cost": {}, "base_risk": 0, "skill": 0,
  "env": { "kind": "knock", "code": 2, "window": 15 },
  "success_node": "hatch_open", "hazard_id": "hatch_jams" }
```

- **Kinds:** `knock` (code index into the world's seeded codes), `lamp`
  (code), `hush` (seconds, cover-and-hold, player-armed), and later
  `probe`.
- **Pass:** a pass **skips the roll** and goes to `success_node`. Failing
  or timing out goes to `hazard_id` like a failed roll.
- **The alternative is simply another choice in the same node** (the
  ordinary rolled option). The loader **rejects a node whose only choices
  are `env` ones**, so a remote player always has a way through.
- **Deferred resolution is new work.**
  - `encRunChoice()` (`network-msg-encounter.hpp`) runs synchronously from
    the WS handler and replies on the `client`.
  - An `env` choice instead arms the box task and replies "pending".
  - The game loop resolves it later through a **pid-targeted** variant of
    the same path, which must also send the `item_result` acks.
- **Dawn** force-aborts encounters (`ENC_END_DAWN`), which closes the task
  with no result. **A reboot** drops it as well.

## Privacy

- **No raw audio is ever stored or sent.** Only features exist.
  `/envdbg` shows levels and onset times, never samples.
- **`/sndtest?rec=1` + `/sndrec.wav` must be gated before duplex ships.**
  Today the recorder is inert because fmt 7 has no RX. Once SENSE is on,
  it would record the room for anyone on the network. *(rec: arming it
  needs a press of B on the box within 10 s, and it is compiled out unless
  `SND_LOOPBACK_REC` is set.)*
- **The mode changes on the box only** (button A on the AIR screen). The LCD
  ear glyph is always visible while the mics are live.
- **No device scanning.**

## Wire and tooling

- **PROTO:** take the next free number when this is built. Several
  unflashed bumps are already queued.
- **gs:**
  `"env":{"m":2,"t":{"k":"hush","ph":1,"s":11,"by":-1},"msg":"..."}` carries
  the mode, the open task (kind, phase, seconds left, armer) and the latest
  announcement. `data/ui-env.js` draws the ear glyph, the countdown ring and
  the toasts.
- **hello:** the phone sends `tz` (minutes east of UTC). The first seated
  phone sets the local offset.
- **`/state`:** carries `env` for the observer TV. It mirrors HUSH big,
  because it is in the room.
- **`GET /envdbg`:** raw readings, features, baselines and `k[]`.
  - `?inject=cover:1,8` / `?inject=loud:1,30` / `?inject=onsets:3-2` fake
    a reading for N seconds, on hardware too.
  - `?attune=1` starts ATTUNE.
- **Serial:** a 30 s line, `ENV: mode lux dB/floor/norm loudOk RH T pose`.
- **Mock:** the same fields plus `dbg_env`. Box tasks resolve from
  injected values.
- **Bots:** they read `env`. The harness injects through `/envdbg`.
  `docs/bot-testing.md` gets a section.
- **Desktop sim:** compile the mic feature code into a `scripts/sndsim`-style
  harness and run it over WAVs (k10loop captures plus room clips: talk, a
  TV, claps, knocks, a dog, a mug set down). Tune the onset detector there
  before flashing.

## Cost

- **Internal RAM.** The legacy IDF 4.4 I2S driver gives RX the same
  `dma_buf_count × dma_buf_len` as TX. At fmt 7's 6 × 768 frames × 4 B,
  that is **+18 KB internal**, against a measured 5-bot low point of
  ~91 KB. Measure it in P0. If it is too tight, a 6 × 512 duplex format
  (12 KB each way, 64 ms of speaker slack) is the fallback.
- **CPU (core 0, audio task):** decimation plus 3 filters on 16 kHz blocks
  is well under 2%. Check it against `sndStUs`.
- **I2C:** 4 bytes from the ALS every 500 ms; the AHT20 every 10 s.
- **Static RAM:** under 2 KB.
- **SD:** none.

## Phases

| | Scope | Exit test |
|---|---|---|
| **P0** | Plumbing, no gameplay. The mode selects the format; OFF = today. `duplex-i2s-x3-48k`, the drain in the muted path, `env.hpp`, the AIR screen, `/envdbg`, NVS baselines, the `/sndrec` gate. | **Measure:** the mic ADC works at 48 kHz; ear A/B against fmt 7 for hiss; internal heap low point with 5 bots, SENSE vs OFF; speaker-to-mic coupling at LOW/MED/HIGH (which decides `loudOk`); a day of readings in the real room. Flash with the board idle. |
| **P1** | HUSH, ATTUNE, the box task slot. Wire, mock, TV, bot injection. | HUSH holds, breaks and goes unanswered on cue in the room. Bot A/B with injected cover. |
| **P2** | Knock codes, encounter `env` gates, deferred resolution, first content (Bunker Entrance hatch, a tunnel door). | ≥ 90% hits on codes, 0 false passes over an hour of table noise. |
| **P3** | Ambient: loud camp, damp/dry, small hours, moon. | Weather transition counts shift within the cap over a run. Announcements fire. |
| **P4** | Signal lamp, plus the caravan call. | A phone torch decodes SOS from 1 m with room lights on. |
| **P5** | The Dial. | Content plus an ear check. |
| **P6** | Probes. A **Geiger counter** on Gravity P0 gives real CPM as an in-game rad trickle and a `probe` gate. A PIR gives "someone walked into camp". CO2/VOC on Gravity I2C. | Per probe. |

## Open decisions

| # | Question | (rec) |
|---|---|---|
| 1 | Default mode | **OFF**, and SENSE/LIVE are changed on the box. *(The first draft said SENSE.)* |
| 2 | Where the mode is changed | Button A on the AIR screen. |
| 3 | When HUSH is called | "It reaches a survivor next tick", any time of day. |
| 4 | What a held HUSH does | Scent cooled within its radius, plus awareness −30. |
| 5 | What counts as cover | Lux −70%, or face-down. Room lights off counts. |
| 6 | Cover window and hold | 8 s + 15 s. |
| 7 | Does a gate pass skip the roll? | Yes. The rolled choice beside it stays open. |
| 8 | Ambient sound | Only "the Doom doesn't forget". |
| 9 | If speaker coupling is too high at volume | Ambient sound only at volume 0, or EARS FULL. NLMS echo cancelling only if this proves to matter. |
| 10 | +18 KB internal DMA | Accept it if the P0 low point stays ≥ 60 KB; otherwise the 6 × 512 format. |
| 11 | Knock codes | Seeded per world, knock or clap. |
| 12 | The Dial | Armed from a phone, no buttons. |
| 13 | First probe | Geiger. |

## Rejected

- **Camera:** out of scope by request.
- **Wi-Fi/BLE scanning:** bystander privacy, it disrupts the AP channel, and
  BT heap is the wedge risk.
- **Speech recognition:** heavy, and it breaks the "nothing understands
  your words" promise. Knock codes do the job.
- **Phone sensors:** the game is served over `http://`, and current
  browsers require a secure context for motion, mic and light.
- **Night-only gating, noise that raises awareness, and an ambient
  dark bonus:** see "What the second pass changed".

## Risks

- **The mic ADC at 48 kHz is untested.** If it fails, the fallbacks are a
  16 kHz duplex with the hiss band, or **HUSH without the mic** (cover plus
  pose plus stillness only, and no hold test). Knocks would wait.
- **Hiss and clicks.** Ear A/B back to back, announce before playing, never
  above the owner's volume.
- **Flash discipline.** P0 changes the audio task and adds a task. Compile,
  ask, flash idle, check the boot log, `/state` and one save, and only then
  load the board.
- **Heap.** The +18 KB of DMA lands in internal RAM, which is where the
  wedge lived.
- **False onsets** (dice, mugs) only matter inside a 15 s window that a
  player opened.
- **The mock diverges** on scent. Validate HUSH on hardware.

## Files it would touch

**New:**

- `env.hpp`
- `data/ui-env.js`

**Edited:**

| File | Change |
|---|---|
| `ui-audio.hpp` | Format, mute-path drain, features, `/sndrec` gate |
| `ui-screens.hpp` | AIR screen, A on AIR |
| `world-system.hpp` | HUSH freeze, scent cool, loud decay skip |
| `actions_game_loop.hpp` | Damp tilt |
| `ghosts.hpp` | Small hours |
| `encounter_engine.hpp`, `network-msg-encounter.hpp` | `env` choice, deferred resolve |
| `network-events.hpp` | `EVT_ENV_*` |
| `network-sync.hpp`, `game-server.hpp` | gs, `/state`, `/envdbg` |
| `network-msg-player.hpp` | `tz` |
| `ui-fx.hpp` | HUSH scene, dim |
| `ui-leds.hpp` | |
| `data/observer*.js` | |
| `mock-server/server.js` | |
| `bots/`, `docs/bot-testing.md` | |
| `docs/dev-loop.md` | `/envdbg`, AIR screen |
