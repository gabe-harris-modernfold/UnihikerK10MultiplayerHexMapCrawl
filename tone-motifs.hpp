#pragma once
// 23 post-apocalyptic tone motifs.
// ToneStep format: {freq, beat} | {neg, 0} = silence | {0, 0} = terminator
// toneTaskFn calls vTaskDelay(beat) after every playTone so each note
// plays its full duration before the next fires.
// No trailing silences — task exits immediately after the last note.
//
// Tonal palette (dark / ancient-tech-from-the-70s, monophonic sine only):
//   tritone      — the "devil's interval", used for every plunge into danger
//   minor 2nd    — half-step creep/grunt, used for tension before a drop
//   diminished-7 arpeggio — fast unstable spin, used for malfunction/spark
//   whole-tone run        — ungrounded/alien, used for the one uncanny bonus cue
//   chromatic descent     — slowing half-steps, used for "dying" cues
//   minor-2nd ostinato    — the pair, repeating and tightening as it closes;
//                           Creeping Doom only (motifs 20-23), the one motif
//                           family where tempo, not pitch, carries the meaning
// Register mostly sits in octaves 2-4 (low/mid) so it reads as heavy old
// machinery rather than a toy; octave 5+ is reserved for alarms and the
// geiger clicks, and the Doom family sits below all of it (<=104 Hz).
// SEQ_SCORE_UP (Esp32HexMapCrawl.ino) and SCREEN_CLICK below
// are deliberately left out of this palette — one's the only upbeat cue,
// the other's a neutral UI blip.

// 1. Dark Entry — semitone creep (G3-F#3) then a tritone plunge into C3
static const ToneStep MOTIF_DARK_ENTRY[] = {
  {196, 110}, {185, 90}, {131, 240}, {0, 0}
};

// 2. Dark Departure — tritone up off the floor (C3-F#3), settles a minor 3rd short of home
static const ToneStep MOTIF_DARK_DEPART[] = {
  {131, 110}, {185, 130}, {147, 230}, {0, 0}
};

// 3. Gross Sludge — LL damage: semitone flinch (F#4-F4), then a plunge to a low-C thud
static const ToneStep MOTIF_GROSS_SLUDGE[] = {
  {370, 70}, {349, 70}, {131, 260}, {0, 0}
};

// 4. Broken Tech — encounter hazard eject: descending diminished-7th arpeggio, spark-and-fail
static const ToneStep MOTIF_BROKEN_TECH[] = {
  {740, 40}, {622, 40}, {523, 40}, {440, 180}, {0, 0}
};

// 5. Heavy Door Drag — whole-tone descent (G2-F2-D#2), grinding
static const ToneStep MOTIF_HEAVY_DOOR_DRAG[] = {
  {98, 200}, {87, 250}, {78, 350}, {0, 0}
};

// 6. Mutant Breath — storm arrival: semitone wobble (F3-F#3) breathing, then a tritone drop into a growl
static const ToneStep MOTIF_MUTANT_BREATH[] = {
  {175, 350}, {185, 350}, {131, 500}, {0, 0}
};

// 7. Power Down — map regen: semitone sag (C4-B3), then a collapse a tritone-plus-octave down
static const ToneStep MOTIF_POWER_DOWN[] = {
  {262, 100}, {247, 150}, {87, 350}, {0, 0}
};

// 8. Acid Drip — chem storm tick: semitone drip (F#5-F5), then a drop to D5 — quick sizzle
static const ToneStep MOTIF_ACID_DRIP[] = {
  {740, 50}, {698, 60}, {587, 100}, {0, 0}
};

// 9. Bunker Alarm — crisis state: alternating tritone siren (G3 <-> C#4), old air-raid character
static const ToneStep MOTIF_BUNKER_ALARM[] = {
  {196, 250}, {-200, 0}, {277, 250}, {-200, 0}, {196, 400}, {0, 0}
};

// 10. Creeping Rust — whole-tone creep (E3-D3), tritone lurch to G#2, unresolved settle
static const ToneStep MOTIF_CREEPING_RUST[] = {
  {165, 80}, {147, 80}, {104, 80}, {131, 250}, {0, 0}
};

// 11. System Fault — encounter hazard continue: stutter-glitch on F#4, tritone crash to C4
static const ToneStep MOTIF_SYSTEM_FAULT[] = {
  {370, 80}, {-90, 0}, {370, 80}, {-90, 0}, {262, 250}, {0, 0}
};

// 12. Distant Thud — weather shift: G3 ... long silence ... C#3, a tritone apart, distant echo
static const ToneStep MOTIF_DISTANT_THUD[] = {
  {196, 90}, {-260, 0}, {139, 180}, {0, 0}
};

// 13. Warning Grunt — TC alert: semitone grunt (F3-F#3), tritone drop to C3
static const ToneStep MOTIF_WARNING_GRUNT[] = {
  {175, 80}, {185, 80}, {131, 300}, {0, 0}
};

// 14. Dead Battery — player downed: true chromatic descent B3-A#3-A3-G#3, slowing, final minor-3rd collapse
static const ToneStep MOTIF_DEAD_BATTERY[] = {
  {247, 70}, {233, 80}, {220, 100}, {208, 130}, {175, 240}, {0, 0}
};

// 15. Rotten Chord — score loss: chromatic climb G#3-A3-A#3 (sour cluster), collapses a tritone off the root
static const ToneStep MOTIF_ROTTEN_CHORD[] = {
  {208, 40}, {220, 40}, {233, 40}, {147, 280}, {0, 0}
};

// 16. Charge Up — player join: D-minor arpeggio rising and quickening (D3-F3-A3-D4), lands a clean octave — a build, not a drop
static const ToneStep MOTIF_SEWER_ECHO[] = {
  {147, 100}, {175, 80}, {220, 60}, {294, 280}, {0, 0}
};

// 17. Weird Anomaly — full clear bonus: whole-tone run A4-B4-C#5, lands on the tritone-related D#5
static const ToneStep MOTIF_WEIRD_ANOMALY[] = {
  {440, 120}, {494, 120}, {554, 120}, {622, 300}, {0, 0}
};

// 18. Geiger Spike — radiation entry: irregular high clicks, unstable pitch between hits
static const ToneStep MOTIF_GEIGER[] = {
  {3300, 15}, {-35, 0}, {2900, 15}, {-45, 0}, {3400, 15}, {-30, 0}, {2600, 50}, {0, 0}
};

// 19. Screen Click — screen switch: neutral UI blip, kept clean (not part of the dark palette)
static const ToneStep MOTIF_SCREEN_CLICK[] = {
  {880, 25}, {660, 20}, {0, 0}
};

// ── Creeping Doom: the pair ──────────────────────────────────────────────────
// Motifs 20-23 are one family and the only ostinato in the palette: two notes
// a minor 2nd apart, alternating, with the *tempo* carrying the information.
// Pitch never moves — what changes as the Doom closes is how fast the pair
// repeats and how little silence sits between reps, which is why these are
// selected by distance rather than by awareness (see doomAudioBand(),
// world-system.hpp). Still the lowest recurring voice in the palette — the
// pair sits below every motif except HEAVY_DOOR_DRAG's tail — so the Doom
// reads as underneath everything else the box says.
//
// Register history: the family first sat at 98/104 Hz (G2/G#2). That is the
// bottom of what this speaker can move, and at a low audioVol setting almost
// nothing of the fundamental survived — what reached the ear was mostly the
// onset transient, i.e. a click. Moved up a fourth to C3/C#3 for roughly
// double the acoustic output while staying firmly in the cellar. The tritone
// drop lands on the old DOOM_LO, which is also HEAVY_DOOR_DRAG's opening note
// and therefore known-good on this hardware.
static constexpr int DOOM_LO   = 131;  // C3
static constexpr int DOOM_HI   = 139;  // C#3 — the minor 2nd above
static constexpr int DOOM_DROP =  98;  // G2  — a tritone below DOOM_HI

// Note-length floor. At DOOM_LO one cycle is ~7.6 ms, and a tone needs roughly
// 10 cycles before the ear hears it as a pitch rather than a click — so no
// note in this family goes below 150 ms (~20 cycles at C3). The first cut of
// these motifs accelerated by shortening notes to 50-90 ms, which at the
// then-98 Hz DOOM_LO is 5-9 cycles: it came out of the speaker as a burst of
// clicks, not a figure. Raising the register raised the ceiling on how fast
// this family *could* go, but the pacing was deliberately not taken back —
// the first cut was also simply too fast to read as an ostinato.
// Tempo therefore comes from shrinking the GAPS and dropping them entirely,
// never from shortening the notes. MOTIF_HEAVY_DOOR_DRAG is the reference
// point for what this register needs — it never goes under ~20 cycles.

// 20. Doom Far — it has your scent. The pair twice, wide gap, unhurried.
static const ToneStep MOTIF_DOOM_FAR[] = {
  {DOOM_LO, 200}, {DOOM_HI, 200}, {-450, 0},
  {DOOM_LO, 200}, {DOOM_HI, 260}, {0, 0}
};

// 21. Doom Near — same notes and lengths, gap less than half, one more rep.
static const ToneStep MOTIF_DOOM_NEAR[] = {
  {DOOM_LO, 170}, {DOOM_HI, 170}, {-200, 0},
  {DOOM_LO, 170}, {DOOM_HI, 170}, {-200, 0},
  {DOOM_LO, 170}, {DOOM_HI, 220}, {0, 0}
};

// 22. Doom Hunt — the gaps are gone entirely and the pair tightens 180->150
// (still ~15 cycles at the floor), then the tritone drops out underneath.
static const ToneStep MOTIF_DOOM_HUNT[] = {
  {DOOM_LO, 180}, {DOOM_HI, 180}, {DOOM_LO, 170}, {DOOM_HI, 170},
  {DOOM_LO, 160}, {DOOM_HI, 160}, {DOOM_LO, 150}, {DOOM_HI, 150},
  {DOOM_DROP, 400}, {0, 0}
};

// 23. Doom Lost — the pair breaks. The answering note never comes; that
// absence is the release cue, so this one ends on the LOW note alone.
static const ToneStep MOTIF_DOOM_LOST[] = {
  {DOOM_LO, 180}, {DOOM_HI, 180}, {-300, 0}, {DOOM_LO, 400}, {0, 0}
};
