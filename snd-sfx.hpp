#pragma once
// ── snd-sfx.hpp ──────────────────────────────────────────────────────────────
// Sound effects: short scores for the effects bus. Each is a list of steps
// (when, which instrument, which note, how long, how hard, and an optional
// pitch slide). Up to four play at once, each on its own voices, so a quake
// does not cut off the storm that caused it.
//
// The 23 motifs in tone-motifs.hpp were written for a single sine voice; their
// pitch contours (the tritone plunges, the semitone creeps, the Doom's pair)
// are kept, and each is orchestrated here with real instruments and layers.
// Effects marked in-key are written in D and transposed to whatever key the
// music is in when they fire, so a stinger lands inside the harmony.

struct SfxStep { uint16_t t; uint8_t patch; int8_t note; uint16_t dur; uint8_t vel; int8_t bend; uint16_t bendMs; };
struct SfxDef  { const SfxStep* s; uint8_t n; uint8_t inKey; };
#define SFXDEF(a, k) { a, (uint8_t)(sizeof(a) / sizeof(a[0])), k }

enum SfxId : uint8_t {
  SFX_NONE = 0,
  SFX_DARK_ENTRY, SFX_DARK_DEPART, SFX_SLUDGE, SFX_BROKEN_TECH, SFX_DOOR_DRAG, SFX_MUTANT_BREATH,
  SFX_POWER_DOWN, SFX_ACID_DRIP, SFX_BUNKER_ALARM, SFX_CREEPING_RUST, SFX_SYSTEM_FAULT, SFX_DISTANT_THUD,
  SFX_WARNING_GRUNT, SFX_DEAD_BATTERY, SFX_ROTTEN_CHORD, SFX_SEWER_ECHO, SFX_RADIO_BLIP, SFX_WEIRD_ANOMALY,
  SFX_GEIGER, SFX_CLICK, SFX_DOOM_FAR, SFX_DOOM_NEAR, SFX_DOOM_HUNT, SFX_DOOM_LOST, SFX_SCORE_UP,
  SFX_THUNDER, SFX_QUAKE, SFX_FIRE_BURST, SFX_FLOOD, SFX_BOOT, SFX_CALLIOPE_CALL,
  SFX_COUNT,
  SFX_TOLL = 200    // procedural: n bell strokes (sndSfxToll)
};

// 1. Dark Entry -- a step into an encounter: a breath drawn in, the creep, a bell in the dark
static const SfxStep FX_DARK_ENTRY[] = {
  {0, P_SWELL, 60, 400, 80, 0, 0}, {0, P_HOLLOW, 55, 110, 95, 0, 0}, {110, P_HOLLOW, 54, 90, 90, 0, 0},
  {200, P_BELL, 48, 2400, 115, 0, 0}, {200, P_THUMP, 40, 200, 100, 0, 0}, {200, P_GROWL, 36, 900, 55, 0, 0} };
// 2. Dark Departure -- out into daylight: the one major chord in the whole score
static const SfxStep FX_DARK_DEPART[] = {
  {0, P_MUSICBOX, 62, 500, 90, 0, 0}, {110, P_MUSICBOX, 66, 500, 90, 0, 0}, {220, P_MUSICBOX, 69, 700, 95, 0, 0},
  {330, P_MUSICBOX, 74, 1100, 105, 0, 0}, {330, P_GLASS, 78, 1300, 55, 0, 0} };
// 3. Gross Sludge -- life lost: a wet flinch and a thud
static const SfxStep FX_SLUDGE[] = {
  {0, P_SQUISH, 66, 180, 110, -14, 180}, {60, P_THUMP, 40, 150, 95, 0, 0}, {90, P_SQUISH, 55, 260, 95, -10, 260} };
// 4. Broken Tech -- thrown out of an encounter: a diminished spark-and-fail
static const SfxStep FX_BROKEN_TECH[] = {
  {0, P_ZAP, 78, 40, 105, 0, 0}, {40, P_ZAP, 75, 40, 105, 0, 0}, {80, P_ZAP, 72, 40, 105, 0, 0},
  {120, P_ZAP, 69, 200, 105, -12, 200}, {20, P_CRACKLE, 60, 30, 100, 0, 0}, {55, P_CRACKLE, 60, 30, 90, 0, 0},
  {95, P_CRACKLE, 60, 30, 100, 0, 0}, {140, P_CRACKLE, 60, 30, 80, 0, 0}, {130, P_SQUELCH, 60, 220, 85, 0, 0} };
// 5. Heavy Door Drag
static const SfxStep FX_DOOR_DRAG[] = {
  {0, P_DOORGRIND, 43, 1100, 105, -4, 1000}, {0, P_RUMBLE, 48, 800, 60, 0, 0}, {1000, P_THUMP, 38, 300, 115, 0, 0} };
// 6. Mutant Breath -- a storm arrives: two breaths, a growl, the thunder behind it
static const SfxStep FX_MUTANT_BREATH[] = {
  {0, P_WHOOSH, 60, 700, 95, 0, 0}, {750, P_WHOOSH, 55, 700, 85, 0, 0}, {600, P_GROWL, 41, 1200, 70, -5, 1200},
  {1300, P_SNARE, 60, 120, 100, 0, 0}, {1320, P_RUMBLE, 45, 2800, 120, 0, 0} };
// 7. Power Down -- the world is regenerated: everything winds to a stop
static const SfxStep FX_POWER_DOWN[] = {
  {0, P_RADIO, 76, 1000, 95, -36, 1000}, {0, P_DOORGRIND, 48, 900, 50, -12, 900}, {900, P_THUMP, 36, 300, 110, 0, 0} };
// 8. Acid Drip -- chem burn: a sizzle and three falling drops
static const SfxStep FX_ACID_DRIP[] = {
  {0, P_CRACKLE, 60, 30, 110, 0, 0}, {0, P_HAT, 80, 200, 90, 0, 0}, {0, P_DRIP, 90, 150, 95, -5, 100},
  {70, P_DRIP, 86, 150, 85, -5, 100}, {140, P_DRIP, 81, 220, 85, -7, 150}, {90, P_CRACKLE, 60, 30, 80, 0, 0} };
// 9. Bunker Alarm -- the clock has run out: the old air-raid wail over a bell
static const SfxStep FX_BUNKER_ALARM[] = {
  {0, P_SIREN, 58, 3000, 115, 0, 0}, {0, P_BELL, 50, 3000, 95, 0, 0}, {1500, P_BELL, 50, 3000, 85, 0, 0} };
// 10. Creeping Rust
static const SfxStep FX_CREEPING_RUST[] = {
  {0, P_CHAIN, 64, 80, 85, 0, 0}, {80, P_CHAIN, 62, 80, 85, 0, 0}, {160, P_CHAIN, 56, 80, 95, 0, 0},
  {240, P_DOORGRIND, 48, 420, 70, -2, 400} };
// 11. System Fault -- a hazard, pressing on: a stutter and a crash
static const SfxStep FX_SYSTEM_FAULT[] = {
  {0, P_RADIO, 66, 80, 100, 0, 0}, {170, P_RADIO, 66, 80, 100, 0, 0}, {340, P_ZAP, 60, 260, 105, -12, 250},
  {340, P_SQUELCH, 60, 200, 95, 0, 0}, {340, P_THUMP, 40, 200, 95, 0, 0} };
// 12. Distant Thud -- the weather turns: something heavy, far off
static const SfxStep FX_DISTANT_THUD[] = {
  {0, P_THUMP, 36, 300, 100, 0, 0}, {0, P_RUMBLE, 45, 2000, 75, 0, 0}, {420, P_THUMP, 33, 400, 70, 0, 0} };
// 13. Warning Grunt -- the clock turns (single toll; SFX_TOLL counts the band)
static const SfxStep FX_WARNING_GRUNT[] = {
  {0, P_BELL, 50, 3200, 115, 0, 0}, {0, P_THUMP, 38, 220, 95, 0, 0}, {0, P_GROWL, 38, 700, 45, 0, 0} };
// 14. Dead Battery -- down: a heart monitor slowing into a flatline, and a bell
static const SfxStep FX_DEAD_BATTERY[] = {
  {0, P_RADIO, 83, 70, 85, 0, 0}, {420, P_RADIO, 83, 70, 75, 0, 0}, {1050, P_RADIO, 83, 70, 70, 0, 0},
  {1950, P_RADIO, 83, 1500, 72, 0, 0}, {1950, P_BELL, 38, 4200, 110, 0, 0} };
// 15. Rotten Chord -- score lost: a sour cluster sagging
static const SfxStep FX_ROTTEN_CHORD[] = {
  {0, P_MUSETTE, 56, 520, 90, 0, 0}, {40, P_MUSETTE, 57, 480, 90, 0, 0}, {80, P_MUSETTE, 58, 440, 90, 0, 0},
  {120, P_TUBA, 50, 420, 105, -1, 420} };
// 16. Sewer Echo -- down a hatch or up out of one
static const SfxStep FX_SEWER_ECHO[] = {
  {0, P_HOLLOW, 50, 110, 90, 0, 0}, {100, P_HOLLOW, 53, 90, 90, 0, 0}, {180, P_HOLLOW, 57, 70, 90, 0, 0},
  {240, P_HOLLOW, 62, 320, 100, 0, 0}, {0, P_WHOOSH, 60, 600, 60, 0, 0}, {240, P_DRIP, 86, 300, 80, 0, 0} };
// 16b. Radio Blip -- someone joins: squelch, two keyed blips
static const SfxStep FX_RADIO_BLIP[] = {
  {0, P_SQUELCH, 60, 90, 90, 0, 0}, {90, P_RADIO, 69, 60, 100, 0, 0}, {170, P_RADIO, 62, 90, 100, 0, 0} };
// 17. Weird Anomaly -- an encounter picked clean: a whole-tone music box and a glass shimmer
static const SfxStep FX_WEIRD_ANOMALY[] = {
  {0, P_SWELL, 60, 350, 50, 0, 0}, {0, P_MUSICBOX, 69, 700, 100, 0, 0}, {120, P_MUSICBOX, 71, 700, 100, 0, 0},
  {240, P_MUSICBOX, 73, 700, 100, 0, 0}, {360, P_MUSICBOX, 75, 1600, 112, 0, 0}, {360, P_GLASS, 87, 1600, 55, 0, 0} };
// 19. Screen click
static const SfxStep FX_CLICK[] = { {0, P_TICK, 96, 20, 55, 0, 0}, {25, P_TICK, 91, 20, 40, 0, 0} };
// 20-23. The Doom's pair, for when the music is off and cannot carry it
static const SfxStep FX_DOOM_FAR[] = {
  {0, P_GROWL, 48, 200, 95, 0, 0}, {200, P_GROWL, 49, 200, 95, 0, 0}, {850, P_GROWL, 48, 200, 95, 0, 0}, {1050, P_GROWL, 49, 260, 95, 0, 0} };
static const SfxStep FX_DOOM_NEAR[] = {
  {0, P_GROWL, 48, 170, 100, 0, 0}, {170, P_GROWL, 49, 170, 100, 0, 0}, {540, P_GROWL, 48, 170, 100, 0, 0},
  {710, P_GROWL, 49, 170, 100, 0, 0}, {1080, P_GROWL, 48, 170, 100, 0, 0}, {1250, P_GROWL, 49, 220, 100, 0, 0},
  {0, P_THUMP, 41, 150, 80, 0, 0}, {540, P_THUMP, 41, 150, 80, 0, 0}, {1080, P_THUMP, 41, 150, 80, 0, 0} };
static const SfxStep FX_DOOM_HUNT[] = {
  {0, P_GROWL, 48, 180, 110, 0, 0}, {180, P_GROWL, 49, 180, 110, 0, 0}, {360, P_GROWL, 48, 170, 110, 0, 0},
  {530, P_GROWL, 49, 170, 110, 0, 0}, {700, P_GROWL, 48, 160, 110, 0, 0}, {860, P_GROWL, 49, 160, 110, 0, 0},
  {1020, P_GROWL, 48, 150, 115, 0, 0}, {1170, P_GROWL, 49, 150, 115, 0, 0}, {1320, P_GROWL, 43, 420, 120, 0, 0},
  {0, P_THUMP, 41, 150, 100, 0, 0}, {360, P_THUMP, 41, 150, 100, 0, 0}, {700, P_THUMP, 41, 150, 100, 0, 0},
  {1020, P_THUMP, 41, 150, 110, 0, 0}, {1320, P_THUMP, 38, 300, 120, 0, 0} };
static const SfxStep FX_DOOM_LOST[] = {
  {0, P_GROWL, 48, 180, 90, 0, 0}, {180, P_GROWL, 49, 180, 90, 0, 0}, {660, P_GROWL, 48, 420, 80, -2, 400} };
// Score up -- a bright arpeggio, in key
static const SfxStep FX_SCORE_UP[] = {
  {0, P_MUSICBOX, 74, 500, 85, 0, 0}, {70, P_MUSICBOX, 78, 500, 85, 0, 0}, {140, P_MUSICBOX, 81, 600, 90, 0, 0},
  {210, P_MUSICBOX, 86, 1000, 100, 0, 0} };
// Thunder: the crack, then the roll
static const SfxStep FX_THUNDER[] = {
  {0, P_SNARE, 60, 120, 112, 0, 0}, {0, P_CRACKLE, 60, 40, 112, 0, 0}, {30, P_RUMBLE, 45, 3000, 122, 0, 0},
  {650, P_RUMBLE, 40, 2600, 85, 0, 0} };
// The earth heaves
static const SfxStep FX_QUAKE[] = {
  {0, P_RUMBLE, 40, 3600, 127, 0, 0}, {0, P_THUMP, 33, 420, 122, 0, 0}, {300, P_DOORGRIND, 36, 1500, 72, -3, 1500},
  {220, P_CHAIN, 70, 200, 60, 0, 0}, {700, P_CHAIN, 67, 200, 52, 0, 0}, {1100, P_THUMP, 31, 400, 100, 0, 0} };
// Caught in the burn
static const SfxStep FX_FIRE_BURST[] = {
  {0, P_WHOOSH, 62, 500, 105, 0, 0}, {0, P_ZAP, 72, 300, 60, -12, 300}, {40, P_CRACKLE, 60, 30, 110, 0, 0},
  {110, P_CRACKLE, 60, 30, 100, 0, 0}, {160, P_CRACKLE, 60, 30, 110, 0, 0}, {260, P_CRACKLE, 60, 30, 90, 0, 0},
  {330, P_CRACKLE, 60, 30, 100, 0, 0}, {450, P_CRACKLE, 60, 30, 80, 0, 0} };
// Swept by a flash flood
static const SfxStep FX_FLOOD[] = {
  {0, P_WHOOSH, 55, 1200, 112, 0, 0}, {200, P_WHOOSH, 50, 1400, 92, 0, 0}, {300, P_DRIP, 84, 200, 80, 0, 0},
  {420, P_DRIP, 79, 200, 70, 0, 0}, {560, P_DRIP, 88, 200, 75, 0, 0}, {800, P_DRIP, 81, 200, 60, 0, 0} };
// Boot: a squelch, and a music box finding its key
static const SfxStep FX_BOOT[] = {
  {0, P_SQUELCH, 60, 120, 80, 0, 0}, {150, P_MUSICBOX, 62, 700, 80, 0, 0}, {300, P_MUSICBOX, 65, 700, 80, 0, 0},
  {450, P_MUSICBOX, 69, 700, 85, 0, 0}, {600, P_MUSICBOX, 74, 1400, 95, 0, 0}, {600, P_DRONE, 50, 1800, 70, 0, 0} };
// The caravan arrives: a calliope flourish up the harmonic minor
static const SfxStep FX_CALLIOPE_CALL[] = {
  {0, P_CALLIOPE, 69, 90, 100, 0, 0}, {90, P_CALLIOPE, 70, 90, 100, 0, 0}, {180, P_CALLIOPE, 69, 90, 100, 0, 0},
  {270, P_CALLIOPE, 68, 90, 100, 0, 0}, {360, P_CALLIOPE, 69, 360, 110, 0, 0}, {360, P_TUBA, 38, 300, 110, 0, 0},
  {360, P_PAH, 62, 150, 90, 0, 0}, {360, P_PAH, 65, 150, 90, 0, 0}, {360, P_THUMP, 45, 100, 90, 0, 0} };

static const SfxDef SFX_TABLE[SFX_COUNT] = {
  { nullptr, 0, 0 },
  SFXDEF(FX_DARK_ENTRY, 0), SFXDEF(FX_DARK_DEPART, 1), SFXDEF(FX_SLUDGE, 0), SFXDEF(FX_BROKEN_TECH, 0),
  SFXDEF(FX_DOOR_DRAG, 0), SFXDEF(FX_MUTANT_BREATH, 0), SFXDEF(FX_POWER_DOWN, 0), SFXDEF(FX_ACID_DRIP, 0),
  SFXDEF(FX_BUNKER_ALARM, 0), SFXDEF(FX_CREEPING_RUST, 0), SFXDEF(FX_SYSTEM_FAULT, 0), SFXDEF(FX_DISTANT_THUD, 0),
  SFXDEF(FX_WARNING_GRUNT, 1), SFXDEF(FX_DEAD_BATTERY, 0), SFXDEF(FX_ROTTEN_CHORD, 0), SFXDEF(FX_SEWER_ECHO, 1),
  SFXDEF(FX_RADIO_BLIP, 0), SFXDEF(FX_WEIRD_ANOMALY, 1), { nullptr, 0, 0 } /* geiger: procedural */,
  SFXDEF(FX_CLICK, 0), SFXDEF(FX_DOOM_FAR, 0), SFXDEF(FX_DOOM_NEAR, 0), SFXDEF(FX_DOOM_HUNT, 0),
  SFXDEF(FX_DOOM_LOST, 0), SFXDEF(FX_SCORE_UP, 1), SFXDEF(FX_THUNDER, 0), SFXDEF(FX_QUAKE, 0),
  SFXDEF(FX_FIRE_BURST, 0), SFXDEF(FX_FLOOD, 0), SFXDEF(FX_BOOT, 1), SFXDEF(FX_CALLIOPE_CALL, 1),
};
static const char* const SFX_NAME[SFX_COUNT] = {
  "none", "dark_entry", "dark_depart", "sludge", "broken_tech", "door_drag", "mutant_breath", "power_down",
  "acid_drip", "bunker_alarm", "creeping_rust", "system_fault", "distant_thud", "warning_grunt",
  "dead_battery", "rotten_chord", "sewer_echo", "radio_blip", "weird_anomaly", "geiger", "click",
  "doom_far", "doom_near", "doom_hunt", "doom_lost", "score_up", "thunder", "quake", "fire_burst",
  "flood", "boot", "calliope_call" };

// ── Players ──────────────────────────────────────────────────────────────────
static constexpr int SFX_PLAYERS = 4;
static constexpr int SFX_BUF = 24;
struct SfxPlayer {
  const SfxStep* s; uint8_t n, i; float ms; int8_t xpose; bool active;
  SfxStep buf[SFX_BUF];                 // procedural effects are written here
};
static SfxPlayer SFXP[SFX_PLAYERS];

static SfxPlayer* sfxClaim() {
  SfxPlayer* best = &SFXP[0];
  for (int i = 0; i < SFX_PLAYERS; i++) {
    if (!SFXP[i].active) return &SFXP[i];
    if (SFXP[i].ms > best->ms) best = &SFXP[i];   // steal the one furthest along
  }
  return best;
}
// delayMs pushes the whole effect later (thunder after the flash)
static void sfxStart(const SfxStep* s, uint8_t n, int8_t xpose, float delayMs = 0.0f) {
  if (!s || !n) return;
  SfxPlayer* p = sfxClaim();
  p->s = s; p->n = n; p->i = 0; p->ms = -delayMs; p->xpose = xpose; p->active = true;
}
static void sfxStartBuf(SfxPlayer* p, uint8_t n, int8_t xpose, float delayMs = 0.0f) {
  p->s = p->buf; p->n = n; p->i = 0; p->ms = -delayMs; p->xpose = xpose; p->active = true;
}

// Steps need not be sorted: each player scans for the next due step.
static void sndSfxBlock() {
  const float blockMs = SND_BLK * 1000.0f * SND_INV_SR;
  for (int k = 0; k < SFX_PLAYERS; k++) {
    SfxPlayer& p = SFXP[k];
    if (!p.active) continue;
    float prev = p.ms;
    p.ms += blockMs;
    bool pending = false;
    for (uint8_t j = 0; j < p.n; j++) {
      const SfxStep& st = p.s[j];
      // a step fires once: in the block whose [prev, ms) holds its time
      if ((float)st.t >= prev && (float)st.t < p.ms) {
        int32_t gate = (int32_t)st.dur * (SND_SR / 1000);
        SndVoice* v = sndNoteOn(SB_SFX, (uint8_t)(200 + k), st.patch, (float)(st.note + p.xpose),
                                st.vel * (1.0f / 127.0f), gate);
        if (v && st.bend) { v->bendTo = (float)st.bend; v->bendK = sndEnvK(st.bendMs * 0.001f, 3.0f); }
      }
      if ((float)st.t >= p.ms) pending = true;
    }
    if (!pending) p.active = false;
  }
}

// ── Procedural effects ───────────────────────────────────────────────────────
// The threat clock tolls its band: n strokes on the low tonic, the clock
// ticking between them, the last stroke left to ring.
static void sndSfxToll(uint8_t n, int8_t xpose) {
  if (n < 1) n = 1; if (n > 4) n = 4;
  SfxPlayer* p = sfxClaim(); uint8_t k = 0;
  for (uint8_t i = 0; i < n && k < SFX_BUF - 4; i++) {
    uint16_t t = (uint16_t)(i * 950);
    p->buf[k++] = { t, P_BELL, 50, (uint16_t)(i + 1 == n ? 4200 : 1400), 115, 0, 0 };
    p->buf[k++] = { t, P_THUMP, 38, 200, 90, 0, 0 };
    if (i + 1 < n) { p->buf[k++] = { (uint16_t)(t + 475), P_TICK, 91, 20, 60, 0, 0 }; }
  }
  sfxStartBuf(p, k, xpose);
}
// A geiger burst: clicks bunched the way decay really bunches them.
static void sndSfxGeiger(uint32_t& rng) {
  SfxPlayer* p = sfxClaim(); uint8_t k = 0; float t = 0;
  while (k < 14 && t < 600.0f) {
    p->buf[k++] = { (uint16_t)t, P_GEIGER, 60, 6, (uint8_t)(90 + sndRandI(rng, 37)), 0, 0 };
    t += 8.0f + 90.0f * sndRandF(rng) * sndRandF(rng);
  }
  sfxStartBuf(p, k, 0);
}
// Unknown ToneStep sequences (anything k10Play is handed that has no score
// here) still play, on the music box, with their original timing.
struct ToneStepLike { int freq; int beat; };
static void sndSfxFromTones(const ToneStepLike* seq) {
  SfxPlayer* p = sfxClaim(); uint8_t k = 0; uint32_t t = 0;
  for (const ToneStepLike* s = seq; s->freq != 0 && k < SFX_BUF && t < 60000; s++) {
    int ms = s->freq < 0 ? -s->freq : s->beat;
    if (s->freq > 0) {
      float midi = 69.0f + 12.0f * sndLog2((float)s->freq / 440.0f);
      p->buf[k++] = { (uint16_t)t, P_MUSICBOX, (int8_t)(midi + 0.5f), (uint16_t)(ms + 200), 100, 0, 0 };
    }
    t += (uint32_t)ms;
  }
  sfxStartBuf(p, k, 0);
}
