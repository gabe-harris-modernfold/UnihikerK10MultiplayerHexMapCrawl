#pragma once
// 19 post-apocalyptic tone motifs.
// ToneStep format: {freq, beat} | {neg, 0} = silence | {0, 0} = terminator
// toneTaskFn calls vTaskDelay(beat) after every playTone so each note
// plays its full duration before the next fires.
// No trailing silences — task exits immediately after the last note.

// 1. Dark Entry
static const ToneStep MOTIF_DARK_ENTRY[] = {
  {440, 120}, {370, 100}, {330, 200}, {0, 0}
};

// 2. Dark Departure
static const ToneStep MOTIF_DARK_DEPART[] = {
  {330, 100}, {294, 100}, {262, 200}, {0, 0}
};

// 3. Gross Sludge — LL damage
static const ToneStep MOTIF_GROSS_SLUDGE[] = {
  {440, 80}, {370, 100}, {330, 250}, {0, 0}
};

// 4. Broken Tech — encounter hazard eject
static const ToneStep MOTIF_BROKEN_TECH[] = {
  {880, 40}, {440, 40}, {330, 250}, {0, 0}
};

// 5. Heavy Door Drag
static const ToneStep MOTIF_HEAVY_DOOR_DRAG[] = {
  {370, 200}, {330, 250}, {294, 350}, {0, 0}
};

// 6. Mutant Breath — storm arrival
static const ToneStep MOTIF_MUTANT_BREATH[] = {
  {370, 350}, {415, 350}, {330, 500}, {0, 0}
};

// 7. Power Down — map regen
static const ToneStep MOTIF_POWER_DOWN[] = {
  {523, 100}, {392, 150}, {294, 350}, {0, 0}
};

// 8. Acid Drip — chem storm tick
static const ToneStep MOTIF_ACID_DRIP[] = {
  {587, 50}, {440, 60}, {330, 100}, {0, 0}
};

// 9. Bunker Alarm — crisis state
static const ToneStep MOTIF_BUNKER_ALARM[] = {
  {440, 250}, {-200, 0}, {440, 250}, {-200, 0}, {415, 400}, {0, 0}
};

// 10. Creeping Rust
static const ToneStep MOTIF_CREEPING_RUST[] = {
  {370, 80}, {330, 80}, {415, 80}, {294, 250}, {0, 0}
};

// 11. System Fault — encounter hazard continue
static const ToneStep MOTIF_SYSTEM_FAULT[] = {
  {440, 80}, {-100, 0}, {440, 80}, {-100, 0}, {392, 250}, {0, 0}
};

// 12. Distant Thud — weather shift
static const ToneStep MOTIF_DISTANT_THUD[] = {
  {440, 80}, {-250, 0}, {370, 150}, {0, 0}
};

// 13. Warning Grunt — TC alert
static const ToneStep MOTIF_WARNING_GRUNT[] = {
  {370, 80}, {494, 80}, {330, 300}, {0, 0}
};

// 14. Dead Battery — player downed
static const ToneStep MOTIF_DEAD_BATTERY[] = {
  {494, 70}, {440, 80}, {392, 100}, {349, 130}, {294, 220}, {0, 0}
};

// 15. Rotten Chord — score loss
static const ToneStep MOTIF_ROTTEN_CHORD[] = {
  {415, 40}, {440, 40}, {466, 40}, {415, 280}, {0, 0}
};

// 16. Sewer Echo — player join
static const ToneStep MOTIF_SEWER_ECHO[] = {
  {440, 150}, {-150, 0}, {440, 100}, {-150, 0}, {440, 60}, {0, 0}
};

// 17. Weird Anomaly — full clear bonus
static const ToneStep MOTIF_WEIRD_ANOMALY[] = {
  {440, 150}, {622, 150}, {440, 300}, {0, 0}
};

// 18. Geiger Spike — radiation entry
static const ToneStep MOTIF_GEIGER[] = {
  {3000, 20}, {-40, 0}, {3200, 20}, {-50, 0}, {2800, 60}, {0, 0}
};

// 19. Screen Click — screen switch
static const ToneStep MOTIF_SCREEN_CLICK[] = {
  {880, 25}, {660, 20}, {0, 0}
};
