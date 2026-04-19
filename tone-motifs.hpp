#pragma once
// 18 post-apocalyptic tone motifs — uses existing ToneStep format:
//   {freq, beat}  →  positive freq = playTone(freq, beat ms)
//   {neg, 0}      →  silence for abs(neg) ms
//   {0, 0}        →  terminator (end of sequence)

// 1. Dark Entry — stepping down into the gloom
static const ToneStep MOTIF_DARK_ENTRY[] = {
  {110, 300}, {103, 200}, {98, 500}, {0, 0}
};

// 2. Dark Departure — ominous, ending on dissonant bass
static const ToneStep MOTIF_DARK_DEPART[] = {
  {98, 250}, {87, 250}, {77, 800}, {0, 0}
};

// 3. Gross Sludge — toxic low-frequency burp (LL damage)
static const ToneStep MOTIF_GROSS_SLUDGE[] = {
  {73, 150}, {65, 200}, {55, 600}, {0, 0}
};

// 4. Broken Tech — power-failure blip dropping to a rattle
static const ToneStep MOTIF_BROKEN_TECH[] = {
  {220, 50}, {110, 50}, {45, 600}, {0, 0}
};

// 5. Heavy Door Drag — rusted bunker door scraping open
static const ToneStep MOTIF_HEAVY_DOOR_DRAG[] = {
  {45, 400}, {40, 500}, {35, 700}, {0, 0}
};

// 6. Mutant Breath — slow, undulating, unsettling thrum
static const ToneStep MOTIF_MUTANT_BREATH[] = {
  {60, 800}, {65, 800}, {55, 1200}, {0, 0}
};

// 7. Power Down — machine giving up its last bit of battery
static const ToneStep MOTIF_POWER_DOWN[] = {
  {120, 200}, {90, 300}, {60, 800}, {0, 0}
};

// 8. Acid Drip — heavy viscous drop hitting a metallic puddle
static const ToneStep MOTIF_ACID_DRIP[] = {
  {150, 100}, {80, 150}, {40, 200}, {0, 0}
};

// 9. Bunker Alarm — slow, low, terrifying pulse (crisis state)
static const ToneStep MOTIF_BUNKER_ALARM[] = {
  {100, 500}, {-200, 0}, {100, 500}, {-200, 0}, {95, 800}, {0, 0}
};

// 10. Creeping Rust — jagged, uncomfortable low steps
static const ToneStep MOTIF_CREEPING_RUST[] = {
  {80, 150}, {75, 150}, {85, 150}, {70, 400}, {0, 0}
};

// 11. System Fault — harsh low buzz for a critical error
static const ToneStep MOTIF_SYSTEM_FAULT[] = {
  {50, 100}, {-50, 0}, {50, 100}, {-50, 0}, {45, 500}, {0, 0}
};

// 12. Distant Thud — giant footstep or far-off explosion (weather shift)
static const ToneStep MOTIF_DISTANT_THUD[] = {
  {40, 100}, {-200, 0}, {35, 200}, {0, 0}
};

// 13. Warning Grunt — sudden low-pitched mechanical bark (TC alert)
static const ToneStep MOTIF_WARNING_GRUNT[] = {
  {80, 150}, {100, 150}, {70, 600}, {0, 0}
};

// 14. Dead Battery — sputtering out into silence (player downed)
static const ToneStep MOTIF_DEAD_BATTERY[] = {
  {90, 100}, {80, 150}, {70, 200}, {60, 300}, {50, 500}, {0, 0}
};

// 15. Rotten Chord — dissonant low cluster (score loss)
static const ToneStep MOTIF_ROTTEN_CHORD[] = {
  {65, 50}, {68, 50}, {73, 50}, {65, 500}, {0, 0}
};

// 16. Sewer Echo — haunting repeating tone, fading (player join)
static const ToneStep MOTIF_SEWER_ECHO[] = {
  {110, 300}, {-150, 0}, {110, 200}, {-150, 0}, {110, 100}, {0, 0}
};

// 17. Weird Anomaly — alien tritone jump (loot banked / anomaly)
static const ToneStep MOTIF_WEIRD_ANOMALY[] = {
  {155, 300}, {220, 300}, {155, 600}, {0, 0}
};

// 18. Geiger Spike — high-contrast radiation clicks (rad entry)
static const ToneStep MOTIF_GEIGER[] = {
  {3000, 20}, {-30, 0}, {3200, 20}, {-40, 0}, {2800, 50}, {0, 0}
};

// 19. Screen Click — short low tactile click for screen switching
static const ToneStep MOTIF_SCREEN_CLICK[] = {
  {180, 18}, {120, 12}, {0, 0}
};
