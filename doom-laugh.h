#pragma once
// -- doom-laugh.h ------------------------------------------------------------
// The Creeping Doom's laughs: what it does when someone goes down.
//
// One score, three readers, so the voice and the two pictures of it cannot
// drift apart:
//   * scripts/lpc/laugh.py scrapes DOOM_LAUGH_* out of this file by regex and
//     synthesises each laugh (breath, hum, glottal pulses through vowel
//     formants) to that rhythm and pitch; gen_vocab.py encodes the result into
//     the Speak & Spell vocabulary as VOC_LAUGH0.. (snd-vocab.h).
//   * the LCD (scDeathDraw, ui-scenes.hpp) opens the gas skull's jaw on every
//     syllable -- doomLaughOpen() below.
//   * the TV (data/observer-fx.js, LAUGH) does the same. MIRRORED-IN
//     data/observer-fx.js: change a table here, change it there.
//
// Times are ms from the first breath of the laugh, which the speaker starts
// DOOM_LAUGH_DELAY_MS after the death (sndStory(SS_DOWNED) queues it with that
// delay; the screens count from DeathUI::begin()). These are the times it is
// heard at: the SAY_DOOM voice plays a laugh at speed 1 with its own pitch
// contour, and a `talk` one (DOOM_LAUGH_VOICE) was measured at that voice's
// own speed.
//
// Plain C++: compiled by the board, by scripts/fxsim, and read by Python.

#include <stdint.h>

// A syllable: a consonant onset (`cons`) and then the vowel, `len` ms in all.
//   amp   0-255, how hard: the loudness of the breath, and how wide the jaw goes
//   cons  'h' breath, 'H' a short hard breath bitten off at the end (a
//         cackle), 'm' a closed-mouth hum that opens (mwa), 'k' a click
//   vowel 'a' ah, 'o' aw, 'e' eh, 'u' uh, 'x' a as in hat
//   f0a, f0b  the pitch across the vowel, Hz (the second voice runs an octave
//             under it -- snd-lpc.hpp, sndSpeechNextWord)
struct DoomLaughSyl { uint16_t at, len; uint8_t amp; char cons, vowel; uint8_t f0a, f0b; };

static const uint16_t DOOM_LAUGH_DELAY_MS = 600;   // the knock lands, the face forms, then it laughs
static const int      DOOM_LAUGH_N = 3;

// 0: the villain. A hum that opens into the first one, a run that climbs, the
// big one with the head thrown back, and the last of it wheezed out.
static const DoomLaughSyl DOOM_LAUGH_0[] = {
  {    0, 420, 150, 'm', 'a', 105, 150 },
  {  560, 150, 170, 'h', 'a', 170, 140 },
  {  735, 150, 185, 'h', 'a', 175, 142 },
  {  910, 150, 200, 'h', 'a', 180, 145 },
  { 1085, 150, 215, 'h', 'a', 188, 150 },
  { 1260, 160, 230, 'h', 'a', 196, 155 },
  { 1470, 720, 255, 'h', 'a', 235, 120 },
  { 2290, 140, 160, 'h', 'a', 150, 120 },
  { 2480, 140, 130, 'h', 'a', 140, 112 },
  { 2700, 420, 105, 'h', 'o', 125,  85 },
};
// 1: the jeer. Not a laugh at all: "HA! ... you dead." in the voice it says
// RUN and MINE in (DOOM_LAUGH_VOICE `talk`), chosen by ear over two synthetic
// cackles on 2026-10-13. SAPI speaks it (scripts/lpc/phrases.txt), so only
// at/len/amp mean anything here -- measured off the encoded frames at that
// voice's 0.8 speed (a 25 ms chip frame plays as 31.25 ms) -- and the rest of
// each row is a placeholder laugh.py never renders.
static const DoomLaughSyl DOOM_LAUGH_1[] = {
  {   30, 390, 230, 'h', 'a', 100,  95 },     // HA!
  { 1620, 190, 200, 'h', 'u', 105,  95 },     // you
  { 1810, 410, 255, 'h', 'e',  93,  76 },     // dead.
};
// 2: the gloat. Three slow booming ones, a quick rolling run, and one long
// one from the bottom of the chest.
static const DoomLaughSyl DOOM_LAUGH_2[] = {
  {    0, 380, 220, 'h', 'o',  98,  86 },
  {  620, 380, 230, 'h', 'o',  96,  84 },
  { 1240, 400, 245, 'h', 'o',  94,  80 },
  { 1820, 150, 170, 'h', 'o', 110,  95 },
  { 2000, 150, 175, 'h', 'o', 108,  94 },
  { 2180, 150, 180, 'h', 'o', 106,  92 },
  { 2360, 150, 185, 'h', 'o', 104,  90 },
  { 2560, 650, 255, 'h', 'o', 120,  75 },
};

// How the Doom's voice (SAY_DOOM, snd-lpc.hpp) delivers each one. `talk`:
// in its ordinary voice -- the doubled monotone it says RUN and MINE in, at
// that voice's 0.8 speed -- instead of following the encoded pitch. Otherwise
// `under` is the second decoder's period multiplier (2 an octave below,
// rasping; 0 no second voice) and rev/echo its sends.
struct DoomLaughVoice { bool talk; float under, rev, echo; };
static const DoomLaughVoice DOOM_LAUGH_VOICE[3] = {
  { false, 2.0f, 0.50f, 0.28f },
  { true,  0.0f, 0.00f, 0.00f },
  { false, 2.0f, 0.50f, 0.28f },
};

// What the screens letter, syllable by syllable, for one that is said rather
// than laughed; nullptr: the screens spell the laugh out of the syllable
// itself (MWA, HA, HO, a long loud one stretched and shouted).
static const char* const DOOM_LAUGH_SAYS_1[] = { "HA!", "YOU", "DEAD." };
static const char* const* const DOOM_LAUGH_SAYS[3] = { nullptr, DOOM_LAUGH_SAYS_1, nullptr };

struct DoomLaugh { const DoomLaughSyl* s; uint8_t n; };
static const DoomLaugh DOOM_LAUGH[DOOM_LAUGH_N] = {
  { DOOM_LAUGH_0, (uint8_t)(sizeof(DOOM_LAUGH_0) / sizeof(DOOM_LAUGH_0[0])) },
  { DOOM_LAUGH_1, (uint8_t)(sizeof(DOOM_LAUGH_1) / sizeof(DOOM_LAUGH_1[0])) },
  { DOOM_LAUGH_2, (uint8_t)(sizeof(DOOM_LAUGH_2) / sizeof(DOOM_LAUGH_2[0])) },
};

// How long a laugh lasts, breath to last breath.
static inline uint32_t doomLaughMs(uint8_t v) {
  const DoomLaugh& L = DOOM_LAUGH[v % DOOM_LAUGH_N];
  const DoomLaughSyl& s = L.s[L.n - 1];
  return (uint32_t)s.at + s.len;
}

// How open the jaw is `ms` into laugh `v`, 0-1, and which syllable is
// sounding (-1 between them). The mouth opens on the breath -- you open to
// laugh before the sound comes -- is widest as the vowel lands, and closes
// over the rest of it; a long one holds open and closes late. A hum opens
// from shut. `amp` scales the lot, so the big one gapes.
static inline float doomLaughOpen(uint8_t v, int32_t ms, int* syl = nullptr) {
  const DoomLaugh& L = DOOM_LAUGH[v % DOOM_LAUGH_N];
  if (syl) *syl = -1;
  for (int i = 0; i < L.n; i++) {
    const DoomLaughSyl& s = L.s[i];
    int32_t t = ms - (int32_t)s.at;
    if (t < 0 || t >= (int32_t)s.len) continue;
    if (syl) *syl = i;
    float a = s.amp / 255.0f;
    float lead = s.cons == 'm' ? 0.38f * s.len : 45.0f;          // the hum stays shut a while
    if (t < lead) return a * (s.cons == 'm' ? 0.04f : 0.55f * (float)t / lead);
    float u = (t - lead) / (float)(s.len - lead);                // across the vowel
    float rise = u < 0.18f ? 0.55f + 0.45f * (u / 0.18f) : 1.0f;
    float fall = s.len > 300 ? (u > 0.7f ? 1.0f - (u - 0.7f) / 0.3f : 1.0f)
                             : (u > 0.35f ? 1.0f - 0.85f * (u - 0.35f) / 0.65f : 1.0f);
    return a * rise * fall;
  }
  return 0.0f;
}
