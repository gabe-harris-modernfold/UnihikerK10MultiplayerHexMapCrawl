# K10 sound engine — design and tuning

The K10's speaker plays a generative score, orchestrated sound effects and a
Speak & Spell voice, all rendered live on the ESP32-S3 and all driven by the
game's story. This page is how it is built, what it says, and how to change it
without flashing. Build/flash/sync commands are in [dev-loop.md](dev-loop.md).

Status: **built and running on the K10**, tuned by ear on the board on
2026-09-23 (see "What the board taught"). The live `/snddbg` knobs remain for
further tuning.

---

## Files

| File | What |
|---|---|
| [snd-core.hpp](../snd-core.hpp) | The synth: oscillators (sine, tri, polyBLEP saw/pulse, band-limited wavetables, 2-op FM, noise), TPT state-variable filters, envelopes, the 18-voice pool, FDN reverb, tape echo, the master chain, and all 33 instruments (`sndPatchesInit`). |
| [snd-lpc.hpp](../snd-lpc.hpp) | TMS5220 / TMS5100 LPC speech — the Speak & Spell chip as one more voice, with a phrase queue and six speaking styles. |
| [snd-vocab.h](../snd-vocab.h) | **Generated.** The LPC vocabulary (85 lines). Edit [scripts/lpc/phrases.txt](../scripts/lpc/phrases.txt) and re-run the generator. |
| [snd-music.hpp](../snd-music.hpp) | The composer: styles, progressions, the melody generator, leitmotifs, the soundscape, tape wow and lurches. |
| [snd-sfx.hpp](../snd-sfx.hpp) | Effects: short multi-voice scores, the 23 old motifs reorchestrated plus thunder, quake, fire, flood, the threat toll. |
| [snd-engine.hpp](../snd-engine.hpp) | The front door: `sndStory()`, `sndSetWorld()`, the story→sound mapping, and `sndRender()`. |
| [ui-audio.hpp](../ui-audio.hpp) | The board: I2S reinstall, the audio task, the 10 Hz world snapshot, `k10Play()` compatibility, the `SND:` serial line. |
| [scripts/sndsim/](../scripts/sndsim/) | Desktop renderer: compiles the engine headers unchanged with MSVC and writes WAVs, a K10-speaker simulation, spectrograms and piano rolls. |
| [scripts/lpc/](../scripts/lpc/) | Vocabulary pipeline: Windows SAPI TTS → our own LPC-10 encoder → `snd-vocab.h`. |

Everything under `snd-*` is platform-neutral (no FreeRTOS, no I2S, no game
state): the same bytes compile for the board and for `sndsim`.

## Signal chain

```
game ──sndStory()──┐                      ┌── music voices [0,12) ──┐
      sndSetWorld()┤  audio task, core 0  │                         ├─ reverb/echo ─ world LPF ─┐
                   └─► composer + SFX ────┤── effect voices [12,18) ─┘                          ├─ DC/HPF 240 Hz (4th) ─ LP 4.8 kHz ─ comp 2:1 ─ look-ahead limiter -3 dBFS ─ volume ─ 3x FIR ─ dither ─► I2S 48 kHz
                       + LPC speech ──────┴── speech (8 kHz → 16 kHz, ducks the music) ─────────┘
```

* **The engine renders at 16 kHz; the bus runs at 48 kHz.** `sndOutput()`
  (ui-audio.hpp) interpolates 3x with a 48-tap polyphase FIR (flat to 5.8
  kHz, images gone by 10.2 kHz, ~1% of a core) because the amp does not: fed
  16 kHz, it mirrors 1-5 kHz content up to 11-15 kHz (see "What the board
  taught"). `sndI2sInit()` reinstalls I2S0 on the library's pins,
  transmit-only, with 6×768 DMA (96 ms of slack, 18 KB); if the 48 kHz
  install fails at boot it falls back to the library's 16 kHz duplex setup.
  The old tone task flipped I2S to 8 kHz and back for every cue; nothing
  retunes the clock now.
* **The volume costs no resolution until the very last step.** The engine
  hands over full scale (`SM.vol` is 1 on the board); `sndOutput()` applies
  the owner's volume in float after the limiter, interpolates, and only then
  cuts to the bus's 16 bits, with TPDF dither and 2nd-order noise shaping
  that moves the error above 8 kHz (`q` on `/snddbg`). The amp is so hot that
  the comfortable middle, volume 5, is a gain of 0.013 -- ~300 steps at the
  loudest -- so a plain cut there is audible grit (see below). The amp takes
  only 16-bit slots. Between sounds the dither stops (exact zeros once the
  input has been silent longer than the FIR): its floor alone was a hiss.
* **Control rate 2 ms** (32-sample blocks): envelopes, LFOs, filter
  coefficients, pitch. Oscillators, filters and gain ramps run per sample.
* **The speaker cannot play bass**, so nothing depends on a fundamental
  below ~240 Hz: bass instruments are saw/pulse/FM at D3 with open filters
  (the ear rebuilds the fundamental from the harmonics), drums are lifted into
  range (`xpose`), the master high-passes at 240 Hz, 4th order.
* **Nothing is ever clipped or saturated.** The limiter looks one block ahead;
  volume sits after it (-49.7 dB at 1, -37.7 at 5, -31.7 at 9 by default; see
  below), so the mix is equally clean
  at every setting.

## What it says: the storytelling system

### Styles (what kind of music, picked from the world each bar)

| Style | When | Sound |
|---|---|---|
| ashfall | day | drone on D + A, harmonium chords, a theremin or someone whistling on the road; the party theme hummed now and then |
| nocturne | night (most) | music-box lullaby over a choir, harmonic minor, the box winding down at phrase ends |
| carnival | the caravan within ~3 hexes, and some nights | 3/4 oom-pah-pah, a calliope with grace notes and trills, a musette answering, the barker's tune when it really is the caravan; faster, Hungarian minor and more warped as dread rises |
| storm | storm weather | Phrygian tuba ostinato, harmonium tremolo, timpani, wind, thunder after the lightning |
| chem | chem rain | whole-tone augmented chords on glass, a theremin sliding off the edge, geiger ticks |
| hunted | the Doom closing (`doomClose` ≥ 150) | **the key sinks a semitone**; the Doom's pair (tonic + ♭2) becomes the bass, faster as it closes; a heartbeat |
| tunnels | every survivor underground | a fourth below the day, cavern reverb, water dripping in the key |
| threshold | an encounter in progress | a tritone drone, a heartbeat, a clock, glass — the world holds its breath |
| elegy | someone just went down | the music box plays the dead survivor's signature at half speed and winds down; a bell |
| dawn | a new day | a hollow call, then the **roll call**; the chronicler chants "day N" on the tonic |
| idle | nobody connected | a carnival somewhere over the hill (muffled, echoing), sixteen bars on, thirty-two off |

Switches wait for a phrase boundary (day/night), a two-bar boundary (weather,
caravan, tunnels) or happen on the next bar (danger, grief, dawn). Leaving an
urgent style is itself urgent.

**Tension** (0..1) folds the threat clock, the Doom, the worst body in the
party (attrition, wounds, hunger, thirst, radiation), fire, weather and night.
It moves the tempo, the mode (Aeolian → harmonic minor → Phrygian), the
progression bank, the chord substitutions (V7→vii°7, iv→♭II), the tape wow,
and the speech glitching.

### Leitmotifs

* **The party theme** — six notes, one per survivor slot (Guide, Quartermaster,
  Medic, Mule, Scout, Endurer). It ends on the leading tone and never resolves.
  A survivor who is gone leaves a hole in it. At dawn it is played as a roll
  call, each note on its owner's instrument (calliope, musette, music box,
  tuba, whistle, bell).
* **Signatures** — each survivor's own phrase, grown from their note: a call
  outward (Guide), counting (Quartermaster), a lullaby turn (Medic), plodding
  down (Mule), a quick flourish (Scout), stoic long notes (Endurer). Played on
  join; played slowly on a music box when they die.
* **The Creeping Doom** — the minor-second pair from the old motifs. Far off it
  breathes under the score as a growl; hunting, it is the bass line and the key
  has sunk under it.
* **The caravan** — the barker's waltz (chromatic neighbours round the fifth).
  Reserved for the caravan: carnival nights play the style, not the tune.
* **The threat clock** — the bell tolls the band reached; from band 3 a quiet
  tick-tock sits under everything, on every eighth at band 4.

### The voice (Speak & Spell)

It exists to **taunt and to tell the story**. Styles (`SAY_*`, snd-lpc.hpp):

| Style | Used for |
|---|---|
| narrator | weather lines, threat-clock lines, deaths, "night falls" |
| chant | "day N" at dawn, intoned on the key's tonic |
| whisper | the Doom's taunts — the **same line each phone shows** (`DOOM_TAUNTS[tier][idx % 3]`) |
| doom | the Doom itself at the hunt: "I see you", "run", "mine", "come here" — a doubled monotone far below |
| barker | the caravan: "step right up. step right up." |
| radio | joins ("the scout takes up the road with us"), "that is correct / incorrect" on long-odds wins and throws |

Speech ducks the music; a queue of four with priorities (a death cuts off
anything). Rising dread makes the chip stutter — frames re-read, the classic
circuit-bent Speak & Spell.

## The story → sound map

`sndStory(kind, a, b)` from [network-events.hpp](../network-events.hpp) and
friends; the engine decides effect + speech + music. Repeating beats (fire
every tick, rad, chem, flood) have cooldowns, longer for the voice.

| Beat | Raised from | Sound |
|---|---|---|
| boot | `setup()` | squelch, a music box finding its key, "welcome to the wasteland" |
| join | `EVT_JOINED` | radio blip → signature → radio voice |
| dawn | `EVT_DAWN` (deduped per day) | dawn style, roll call, chant |
| dusk | day clock crossing 0.78 | the music box going down, "night falls" |
| downed | `EVT_DOWNED` | a heart monitor to a flatline, the elegy, the line; "no one is left" |
| threat band | `checkScoreAudio()` | n bell tolls, the TC line, siren + "too late" at 4 |
| weather | `EVT_WEATHER` | breath/acid/thud + the WX line; the style follows |
| encounter | `EVT_ENC_*` | into the dark / the one major chord / spark-and-fail / picked clean |
| caravan | `EVT_CARAVAN_TRADE` + proximity | calliope flourish, barker; the carnival while it is near |
| Doom taunt | `EVT_DOOM_TAUNT` | the whisper, and at tier 3 the Doom's own voice |
| thunder | `updateLEDs()` lightning | thunder 0.25-1.6 s after the flash |
| quake, fire, flood, tunnel, regen, score | their events | their effects |

`k10Play(MOTIF_*)` still works everywhere: each motif maps to its
orchestrated effect ([ui-audio.hpp](../ui-audio.hpp)); motifs fired at damage
sites whose event already has a story sound map to nothing.

## Previewing on the desktop (no flash)

```powershell
python scripts/sndsim/sndsim.py              # every scene
python scripts/sndsim/sndsim.py carnival      # one scene (--list for all)
python scripts/sndsim/sndsim.py --seed 7 day  # another roll of the dice
```

Output in `scripts/sndsim/out/` (gitignored): `<scene>.wav` (what the amp
gets), `<scene>_k10.wav` (a rough 2 W speaker model — listen to this one on
headphones), `<scene>.png` (spectrogram + level) and `<scene>_roll.png`
(piano roll by part). `story` is a whole evening compressed into seven minutes.
Scenes are the `SCENES[]` table in `sndsim.cpp`; each is a function fed every
100 ms, exactly like the board's world snapshot.

Stems: `sndsim.exe <outdir> <scene> <seed> <hex part mask> <suffix>` mutes
music parts (bit = `MusPart`). The effects bus is never masked.

## Changing the vocabulary

```powershell
python scripts/lpc/gen_vocab.py            # all phrases (TTS cached by text)
python scripts/lpc/gen_vocab.py --only RUN MINE
```

Add a line to `scripts/lpc/phrases.txt` (`ID | text`), regenerate, and refer
to it as `VOC_<ID>`. Order is load-bearing for `N1..N90` (numbers are built
arithmetically), `DOOM00..DOOM32` (tier×3 + idx, mirroring `DOOM_TAUNTS`),
`TC1..4` and `WX0..5`. The encoder, reference decoder and validation report
live in `scripts/lpc/`; `out/decoded/*.wav` is what the chip will say.

The decoder reads Talkie's bitstream order, so any TMS5220-format Talkie
vocabulary array plays unchanged (and TMS5100 data with `LPC_TMS5100`). The
repo does not include Talkie's vocabularies: Talkie is GPL-3.0. Tables in
snd-lpc.hpp are TI's, via MAME's BSD-3-licensed `tms5110r.hxx`.

## What the board taught (first listens, 2026-09-23)

The desktop renders were fine; the K10 was not, in ways the speaker model
could not have shown. All of them are fixed in the current build.

* **The amp is extremely hot.** Volume 1 at -22 dBFS was "really loud"; at
  -40 dBFS it is faint but clear. `sndVolGain()` mapped 1..9 to -40..-16 dB
  in 3 dB steps; once the static was gone that was "30% louder" all round,
  and then, tuning the mix on the sound desk, the owner's comfortable level
  was -37.7 dB -- "this should be volume level 5". So 5 is -37.7 dB and 1-4
  step down 3 dB each. 9 at -25.7 dB then drove the amp into clipping on loud
  content, far below digital full scale (the amp's rail, or the PC's USB
  sagging), so 6-8 now share the way up to a ceiling that is a desk knob
  (`vmax`, default -31.7 dB), set by ear just below the clip; `vmid` moves 5.
  Nothing may bypass the volume -- a measurement sequence that did (exact
  levels up to -10 dBFS) was painful.
* **The steady hiss that remains is the amp's, not the signal's.** It barely
  changed across all four 16-bit cuts (8-12 dB apart in digital noise), sat
  at a fixed level under the music, and vanished whenever the amp was fed
  pure zeros. Software can only keep the amp fed zeros between sounds (the
  silence gate); under sound, the fixes are cleaner power (not a PC's USB)
  or a series resistor on the speaker with `sndVolGain()` raised to match.
* **Crackle at every volume** came from the first master: makeup gain into a
  cubic soft clipper that sat *before* the volume stage, so loud peaks were
  distorted inside the engine and the volume knob only scaled the crackle.
  The master now has a one-block look-ahead limiter at -3 dBFS and no
  waveshaping anywhere, a 4th-order 240 Hz high-pass (a 2 W cone rattles on
  what it cannot move) and a 4.8 kHz roll-off.
* **"Static that follows the music, loudest on the voice"** -- a fizz band
  with the same ratio to the music at every volume, while a pure sine through
  the whole engine came out clean. Two causes, both in the output stage:
  1. *Imaging.* Fed 16 kHz, the amp mirrors 1-5 kHz content up to 11-15 kHz,
     so the fizz tracks every note (a sine's mirror lands above 15 kHz, out
     of earshot). The bus now runs at 48 kHz behind a 3x FIR: "much better,
     almost there".
  2. *The 16-bit cut after the volume.* The engine applied the volume and
     then truncated; at volume 3 (0.02) the loudest sample was +-464 steps,
     43% of the voice's samples fell under one step, and offline the error
     sat 30 dB under the signal and tracked it -- 8-bit-sampler grit on every
     word and on the quiet music bed. `sndOutput()` now applies the volume in
     float and dithers and noise-shapes the cut (42 dB, uncorrelated); on the
     board the new cut was heard as cleaner.
  Softening the voice first (a smoothed source, quieter hiss consonants, a
  3.2 kHz band limit) only hid the static and was heard as muffled, then
  unintelligible: the voice is back to the chip's own sound, with the
  half-band 8 -> 16 kHz upsampler and the 8-bit DAC grit only on the radio
  voice. The knobs stay on `/snddbg` (below).

Measured on the board: rendering takes ~1 ms of every 16 ms block (5-10% of
core 0), zero dropouts (`late=0`) under content. The bus format was ruled out
(Philips, left-justified and PCM made no difference to the static).

Dead ends worth not repeating:

* **The laptop's "Microphone Array" cannot measure a speaker.** Windows voice
  processing (noise suppression, echo cancellation) is in every capture path
  that delivered audio here; it recorded the laptop's own clean 1 kHz beep at
  1% purity. WASAPI (shared, exclusive, RAW option) delivered zeros.
  `scripts/sndsim/k10measure.py` is correct and works on a clean mic.
* **The K10's own mics read as random bits** (-4.8 dBFS RMS, flat) in duplex
  Philips 16-bit at 16 kHz, with `eAmp_Gain` low or high. They need a
  different RX setup (TDM/PDM or ADC config) that is not worked out;
  `scripts/sndsim/k10loop.py` and `GET /sndrec.wav` are ready for when it is.
* **`eAmp_Gain` high mutes the speaker** (the K10 library raises it while
  recording). Leave it low.
* **32-bit slots play silence.** 48 kHz with 32-bit samples (BCLK = 64 fs),
  so the volume would cost no resolution, was silent even with the low 16
  bits zero: the amp reads the slot's low half or does not take 64 fs. Never
  put data in that half -- if it is read, it is full-scale noise.
* **The TMS5100 chirp** (bipolar, the original Speak & Spell pulse) made the
  TTS-encoded vocabulary harder to follow than the TMS5220's own.

### The sound desk: tune by ear, no flash (`http://<board>/sound.html`)

[data/sound.html](../data/sound.html) is one standalone page served by the
board (like `observer.html`, it is **not** in `web-assets.json`). Every knob
below is a slider or switch, every effect, vocabulary line (in any of the six
speaking styles), music style and story beat is a button, and **SAVE TO
BOARD** keeps the mix in NVS so it survives a reboot with no reflash. It
builds itself from `GET /sndinfo`, so a knob added to `SND_KNOB` or an effect
added to `SFX_NAME` shows up on the desk by itself.

- **EVALUATE** holds scripted listening tests (voice over music, hiss with the
  output live vs muted, the old vs the new 16-bit cut, the six voices). Each
  puts back what it changed when it ends or on STOP (Esc).
- One request in flight at a time, knob changes coalesced: a slider drag is
  never a burst at the board. It polls `/snddbg` every 3 s while idle, so a
  volume change from the game's settings shows up.
- Nothing plays at volume 0 -- the muted audio task drops cues. The desk says
  so instead of failing silently.
- Deploy: push the one file (`POST /upload?dest=/data/sound.html`, or
  `sync_data.ps1`) and reboot; the board caches `data/` at boot.
- Offline: the mock-server answers `/sndinfo`, `/snddbg` and `/sndplay` from
  the firmware headers themselves (nothing plays), so the page can be worked
  on at `http://localhost:8765/sound.html`.

What the knobs are, and where a saved set lives: `SND_KNOB` in ui-audio.hpp.
The mix is NVS namespace `sndmix`; the volume and the music level are the
game's own `k10` keys (`vol`, `mus`), so the desk and the game's settings
panel move the same two values. `style` and `tension` only audition and are
never saved. RESET restores the compiled mix (volume and music level stay)
and forgets the saved one.

### Live tools (HTTP, no flash)

| Route | What |
|---|---|
| `GET /sndinfo` | the desk's catalogue: every knob (`k`, `min`, `max`, `step`, `def`), and the names of every effect, vocabulary line, speaking style, music style and story beat, plus the current state |
| `GET /sndplay?…` | play through the game's own cue queue: `sfx=N`, `say=N&style=S`, `story=K&a=A&b=B`, `seq=1\|2`, `stop=1` |
| `GET /sndtest?seq=0` | test tones (exact levels, scaled by the volume setting): sync, sweep, 1 kHz and 300 Hz ladders, pink noise |
| `GET /sndtest?seq=1` | content: narrator, whisper, calliope, music box, bell, harmonium (music paused) |
| `GET /sndtest?seq=2` | bare sines through the whole engine, then the narrator |
| `GET /sndtest?fmt=N` | reinstall I2S in format N first (`SND_FMT` in ui-audio.hpp): 2 = 16 kHz duplex (the old default), 6 = 32 kHz (2x), 7 = 48 kHz (3x, the default) |
| `GET /snddbg?k=v[&k=v…]` | set knobs; replies with all of them. Mix: `vol` 0-9, `mlvl` music level 0-9, `mus` music bus, `sfx` effect bus, `vgain` voice bus, `duck` how far speech dips the music. Space: `rev`, `noise`, `wind` multipliers. Master: `lp` roll-off Hz (0 off), `hp` high-pass Hz, `comp` compressor threshold dBFS, `q` the 16-bit cut (0 the old truncation / 1 round / 2 TPDF / 3 TPDF + shaping, the default). Voice: `vlp` band Hz (0 off), `vunv` hiss, `vsoft` source smoothing 0-0.9, `vchirp` pitch pulse (-1 chip / 0 TMS5220 / 1 TMS5100), `dac8` (-1/0/1). Voices: `lvl_narrator`, `lvl_chant`, `lvl_whisper`, `lvl_doom`, `lvl_barker`, `lvl_radio`, each style's level on top of `vgain`. Scale: `vmid` volume 5 in dB, `vmax` volume 9 in dB (6-8 share the way up). Audition: `style` force a music style (-1 = the world), `tension` pin it (-1 = the world). `save=1` keeps the mix on the board, `reset=1` restores the compiled one |

## On the board

Serial, every 30 s while sound is enabled:

```
SND: render avg=…us max=…us of 16000us (N% cpu) voices max=… late=… style=… tension=…% stack=…
```

`late` counts renders that came back after the 96 ms DMA queue could have run
dry — an audible dropout. `stack` is the audio task's high-water mark (words).

Settings: **K10 Volume** (0 = the engine stops rendering entirely) and **K10
Music** (0 = effects and voice only) in the web client's settings; the music
level is NVS key `mus` in namespace `k10`.

Tuning knobs worth knowing: patch parameters (`sndPatchesInit`), the `STYLE[]`
table (tempo range, instruments, gains, reverb, wow, world lowpass),
`musTension()`, `musPickStyle()` thresholds, `sndVolGain()`.
