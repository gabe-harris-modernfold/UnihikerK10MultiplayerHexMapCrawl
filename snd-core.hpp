#pragma once
// ── snd-core.hpp ─────────────────────────────────────────────────────────────
// The K10's synthesizer: oscillators, filters, envelopes, a voice pool, the
// reverb and tape echo, and the master chain that turns it all into 16-bit
// frames for the I2S amp.
//
// What the hardware allows, and what that decided:
//   * One mono 2 W speaker behind a digital class-D amp, fed 16 kHz 16-bit
//     stereo frames (both channels identical). 16 kHz is the rate the K10's
//     own library installs the I2S driver at, so nothing here ever retunes
//     the peripheral -- the old tone task flipped it to 8 kHz and back per cue.
//   * The speaker moves next to nothing below ~200 Hz. Bass therefore never
//     relies on its fundamental: every low instrument is harmonically rich
//     (saw, pulse, FM) so the ear rebuilds the missing fundamental from the
//     harmonics, and the master high-passes at 120 Hz so the amp's headroom
//     is spent on what can actually be heard.
//   * One ESP32-S3 core, shared with Wi-Fi. Envelopes, LFOs and filter
//     coefficients run at the control rate (one 32-sample block = 2 ms);
//     only oscillators, filters and gain ramps run per sample. Hot tables live
//     in internal RAM, the long delay lines in PSRAM, where their sequential
//     access pattern is cache-friendly.
//
// This header has no FreeRTOS, I2S or game dependencies. Under SND_NATIVE it
// compiles on the desktop, where scripts/sndsim renders this exact code to WAV
// (see docs/dev-loop.md). snd-lpc.hpp, snd-music.hpp and snd-sfx.hpp build on
// it; ui-audio.hpp owns the board.

#include <stdint.h>
#include <string.h>
#include <math.h>

// ── 0. Portability ───────────────────────────────────────────────────────────
#ifdef SND_NATIVE
  #include <stdlib.h>
  #include <stdio.h>
  static inline void* sndAlloc(size_t n, bool hot) { (void)hot; return calloc(1, n); }
  #define SND_LOCK()   do {} while (0)
  #define SND_UNLOCK() do {} while (0)
#else
  static portMUX_TYPE sndMux = portMUX_INITIALIZER_UNLOCKED;
  #define SND_LOCK()   taskENTER_CRITICAL(&sndMux)
  #define SND_UNLOCK() taskEXIT_CRITICAL(&sndMux)
  // hot = touched every sample: internal RAM first. Everything else PSRAM.
  static inline void* sndAlloc(size_t n, bool hot) {
    void* p = hot ? heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
    if (!p) p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(1, n);
    return p;
  }
#endif

static constexpr int   SND_SR     = 16000;
static constexpr int   SND_BLK    = 32;                         // control block, 2 ms
static constexpr float SND_INV_SR = 1.0f / SND_SR;
static constexpr float SND_KRATE  = (float)SND_SR / SND_BLK;    // 500 control blocks/s

// ── 1. Small maths ───────────────────────────────────────────────────────────
static inline float sndClampF(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float sndMinF(float a, float b) { return a < b ? a : b; }
static inline float sndMaxF(float a, float b) { return a > b ? a : b; }
static inline float sndAbsF(float x) { return x < 0 ? -x : x; }

// 2^x, ~0.15 cent worst case. Control rate only (pitch, cutoff, gains).
static inline float sndExp2(float x) {
  if (x < -60.0f) return 0.0f;
  if (x >  60.0f) x = 60.0f;
  int   i = (int)x; if ((float)i > x) i--;
  float f = x - (float)i;
  float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f
                  + f * (0.00961813f + f * 0.00133336f))));
  union { float f; int32_t i; } u; u.f = p; u.i += i << 23;
  return u.f;
}
// log2(x) for x > 0, ~1e-4 absolute. Control rate only (the compressor).
// The polynomial is ln(m) on [1,2), so it is scaled by 1/ln 2; the first cut
// added it raw to a -128 bias and read every level about an octave low, which
// turned the compressor into +3 dB of gain right at its threshold.
static inline float sndLog2(float x) {
  union { float f; int32_t i; } u; u.f = x;
  float e = (float)(((u.i >> 23) & 255) - 127);
  u.i = (u.i & 0x007FFFFF) | 0x3F800000;          // mantissa in [1,2)
  float m = u.f;
  return e + 1.44269504f * (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}
static inline float sndMidiHz(float midi) { return 440.0f * sndExp2((midi - 69.0f) * (1.0f / 12.0f)); }
static inline float sndDb(float db) { return sndExp2(db * 0.16609640f); }   // 10^(db/20)

// Fractional part in [0,1) for either sign; the FM phase goes negative. A
// tiny negative f rounds to exactly 1.0f when 1 is added, which would index
// one past the end of the sine table: fold it back to 0.
static inline float sndWrap01(float x) {
  int i = (int)x; float f = x - (float)i;
  if (f < 0.0f) { f += 1.0f; if (f >= 1.0f) f = 0.0f; }
  return f;
}

// xorshift32. The composer owns one, every voice owns one (noise), so a scene
// replays identically in the desktop harness from the same seed.
static inline uint32_t sndRandU(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static inline float    sndRandF(uint32_t& s) { return (float)(sndRandU(s) >> 8) * (1.0f / 16777216.0f); }
static inline float    sndRandBi(uint32_t& s) { return sndRandF(s) * 2.0f - 1.0f; }
static inline int      sndRandI(uint32_t& s, int n) { return n > 0 ? (int)(sndRandU(s) % (uint32_t)n) : 0; }
static inline bool     sndChance(uint32_t& s, float p) { return sndRandF(s) < p; }

// ── 2. Tables ────────────────────────────────────────────────────────────────
// One float sine (the hottest table) and four int16 single-cycle wavetables,
// each in two band-limited mips: the full set of partials below ~900 Hz, a
// three-partial set above it, so a calliope in its top octave doesn't fold
// its seventh harmonic back down over the melody.
static constexpr int SND_SIN_N = 512;
static constexpr int SND_WT_N  = 256;
enum SndWt : uint8_t { WT_CALLIOPE = 0, WT_REED, WT_VOX, WT_HOLLOW, WT_COUNT };

static float*   sndSinT = nullptr;                   // [SND_SIN_N + 1]
static int16_t* sndWtT  = nullptr;                   // [WT_COUNT][2][SND_WT_N + 1]

static inline float sndSinP(float ph) {              // ph in [0,1)
  float x = ph * SND_SIN_N; int i = (int)x; float f = x - (float)i;
  const float* t = sndSinT + i;
  return t[0] + f * (t[1] - t[0]);
}
static inline float sndWtP(const int16_t* t, float ph) {
  float x = ph * SND_WT_N; int i = (int)x; float f = x - (float)i;
  return ((float)t[i] + f * (float)(t[i + 1] - t[i])) * (1.0f / 32767.0f);
}

static void sndBuildWt(int16_t* dst, const float* amps, int n) {
  float tmp[SND_WT_N]; float peak = 0.0f;
  for (int i = 0; i < SND_WT_N; i++) {
    float ph = (float)i / SND_WT_N, s = 0.0f;
    for (int h = 0; h < n; h++) if (amps[h] != 0.0f) s += amps[h] * sinf(6.2831853f * (h + 1) * ph);
    tmp[i] = s; if (sndAbsF(s) > peak) peak = sndAbsF(s);
  }
  for (int i = 0; i < SND_WT_N; i++) dst[i] = (int16_t)(tmp[i] / peak * 32000.0f);
  dst[SND_WT_N] = dst[0];
}

static bool sndTablesInit() {
  sndSinT = (float*)sndAlloc(sizeof(float) * (SND_SIN_N + 1), true);
  sndWtT  = (int16_t*)sndAlloc(sizeof(int16_t) * WT_COUNT * 2 * (SND_WT_N + 1), true);
  if (!sndSinT || !sndWtT) return false;
  for (int i = 0; i <= SND_SIN_N; i++) sndSinT[i] = sinf(6.2831853f * (float)i / SND_SIN_N);
  // Partial amplitudes, fundamental first.
  static const float CAL[] = { 1.0f, 0.36f, 0.20f, 0.09f, 0.05f, 0.03f };           // flue pipe / steam whistle
  static const float REE[] = { 1.0f, 0.62f, 0.78f, 0.46f, 0.52f, 0.30f, 0.33f,
                               0.19f, 0.22f, 0.11f, 0.10f };                          // harmonium, accordion
  static const float VOX[] = { 0.45f, 0.60f, 1.0f, 0.55f, 0.40f, 0.52f, 0.24f,
                               0.12f, 0.07f, 0.05f };                                 // "aah", formants ~700/1100 Hz
  static const float HOL[] = { 1.0f, 0.03f, 0.48f, 0.02f, 0.26f, 0.01f, 0.15f,
                               0.0f, 0.08f };                                         // clarinet: odd partials
  const float* sets[WT_COUNT] = { CAL, REE, VOX, HOL };
  const int    cnt[WT_COUNT]  = { 6, 11, 10, 9 };
  for (int w = 0; w < WT_COUNT; w++) {
    int16_t* lo = sndWtT + (w * 2 + 0) * (SND_WT_N + 1);
    int16_t* hi = sndWtT + (w * 2 + 1) * (SND_WT_N + 1);
    sndBuildWt(lo, sets[w], cnt[w]);
    sndBuildWt(hi, sets[w], cnt[w] < 3 ? cnt[w] : 3);
  }
  return true;
}
// Switch to the three-partial mip before the table's top partial passes
// ~7.5 kHz: the reed's 11th partial folded back as fizz from ~680 Hz up when
// the switch sat at a flat 900 Hz for every table.
static const float SND_WT_MIPHZ[WT_COUNT] = { 7500.0f / 6, 7500.0f / 11, 7500.0f / 10, 7500.0f / 9 };
static inline const int16_t* sndWtFor(uint8_t wt, float hz) {
  return sndWtT + (wt * 2 + (hz > SND_WT_MIPHZ[wt] ? 1 : 0)) * (SND_WT_N + 1);
}

// Band-limited step residual for the saw and pulse (2-sample polyBLEP).
static inline float sndBlep(float t, float dt, float idt) {
  if (t < dt)         { t *= idt;                 return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt)  { t = (t - 1.0f) * idt;     return t * t + t + t + 1.0f; }
  return 0.0f;
}

// ── 3. Patches ───────────────────────────────────────────────────────────────
enum SndOsc : uint8_t { OSC_SINE = 0, OSC_TRI, OSC_SAW, OSC_PULSE, OSC_WT, OSC_FM, OSC_NOISE };
enum SndFlt : uint8_t { FLT_OFF = 0, FLT_LP, FLT_BP, FLT_HP };

struct SndPatch {
  uint8_t osc, wt, flt, mono;       // mono: 1 = legato (retarget pitch instead of retrigger)
  float detune;                     // cents between two copies of the oscillator (0 = one)
  float noise;                      // white noise mixed in before the filter
  float pw;                         // pulse width
  float fmRatio, fmIndex, fmEnv;    // OSC_FM: modulator ratio, base index, index added by the mod env
  float atk, dec, sus, rel;         // amp env: attack (s), decay/release (s to -60 dB), sustain level
  float matk, mdec, msus, mrel;     // mod env (filter / FM index / pitch)
  float cut, cutEnv, cutKey, res;   // filter Hz, octaves from the mod env, key tracking, resonance 0..1
  float fltLfoHz, fltLfoOct;        // slow filter sweep
  float vibHz, vibSemi, vibDelay;   // vibrato, delayed onset like a singer's
  float tremHz, tremDepth;
  float pEnvSemi, pEnvTime;         // pitch starts this far off and slides home
  float drift;                      // cents of slow random wander: out-of-tune instruments
  float glide;                      // portamento time (s), legato patches only
  float gain, rev, echo;            // level, reverb send, echo send
  float xpose;                      // semitones added to every note (lifts a thump into the speaker's range)
};

enum SndPatchId : uint8_t {
  // pitched, the music's orchestra
  P_DRONE = 0, P_PAD, P_CALLIOPE, P_MUSETTE, P_MUSICBOX, P_BELL, P_TUBA, P_PAH,
  P_THEREMIN, P_WHISTLE, P_CHOIR, P_HOLLOW, P_GLASS,
  // percussion
  P_THUMP, P_TICK, P_SNARE, P_HAT, P_SWELL, P_TOM, P_CHAIN,
  // textures
  P_WIND, P_DRIP, P_CRACKLE, P_GEIGER, P_RUMBLE,
  // effects
  P_SIREN, P_GROWL, P_RADIO, P_SQUELCH, P_ZAP, P_SQUISH, P_WHOOSH, P_DOORGRIND,
  P_TESTSINE,       // a bare sine with no sends: the engine's own test tone (calibration seq 2)
  P_COUNT
};
static constexpr uint8_t P_NONE = 255;   // "no instrument" in style tables (P_DRONE is 0)
static SndPatch* sndPatch = nullptr;   // [P_COUNT], built by sndPatchesInit()

static void sndPatchDefaults(SndPatch& p) {
  memset(&p, 0, sizeof(p));
  p.osc = OSC_SINE; p.flt = FLT_OFF; p.pw = 0.5f;
  p.atk = 0.005f; p.dec = 0.3f; p.sus = 0.8f; p.rel = 0.2f;
  p.matk = 0.001f; p.mdec = 0.2f; p.msus = 0.0f; p.mrel = 0.2f;
  p.cut = 2000.0f; p.res = 0.1f; p.gain = 0.3f;
}

// The instruments. Parameter values were chosen for a 2 W speaker in a
// plastic case: nothing depends on a fundamental below ~150 Hz, and the
// lead voices sit where a small cone is loudest (500 Hz - 3 kHz).
static bool sndPatchesInit() {
  sndPatch = (SndPatch*)sndAlloc(sizeof(SndPatch) * P_COUNT, false);
  if (!sndPatch) return false;
  for (int i = 0; i < P_COUNT; i++) sndPatchDefaults(sndPatch[i]);
  SndPatch* p;

  // The ground under everything: two detuned saws, darkly filtered, breathing.
  p = &sndPatch[P_DRONE];   p->osc = OSC_SAW; p->detune = 11; p->flt = FLT_LP; p->cut = 1050; p->cutKey = 0.2f;
  p->res = 0.3f; p->fltLfoHz = 0.07f; p->fltLfoOct = 0.8f; p->atk = 2.2f; p->dec = 1; p->sus = 1; p->rel = 3.5f;
  p->drift = 5; p->gain = 0.2f; p->rev = 0.45f;
  // Harmonium chords: the church in the ruins.
  p = &sndPatch[P_PAD];     p->osc = OSC_WT; p->wt = WT_REED; p->detune = 7; p->flt = FLT_LP; p->cut = 2200;
  p->cutKey = 0.45f; p->atk = 0.32f; p->dec = 0.6f; p->sus = 0.78f; p->rel = 0.9f; p->vibHz = 4.4f;
  p->vibSemi = 0.035f; p->vibDelay = 0.3f; p->drift = 3; p->gain = 0.17f; p->rev = 0.42f; p->echo = 0.08f;
  // The steam calliope, and it has not been tuned since the war.
  p = &sndPatch[P_CALLIOPE]; p->osc = OSC_WT; p->wt = WT_CALLIOPE; p->noise = 0.022f; p->flt = FLT_LP;
  p->cut = 3000; p->cutKey = 0.3f; p->atk = 0.022f; p->dec = 0.25f; p->sus = 0.86f; p->rel = 0.12f;
  p->vibHz = 6.3f; p->vibSemi = 0.17f; p->vibDelay = 0.07f; p->drift = 16; p->gain = 0.30f; p->rev = 0.28f; p->echo = 0.22f;
  // Accordion, musette-tuned: two reeds 14 cents apart beat like a tremolo.
  p = &sndPatch[P_MUSETTE]; p->osc = OSC_WT; p->wt = WT_REED; p->detune = 14; p->flt = FLT_LP; p->cut = 2500;
  p->cutKey = 0.3f; p->atk = 0.03f; p->dec = 0.35f; p->sus = 0.74f; p->rel = 0.14f; p->drift = 6;
  p->gain = 0.22f; p->rev = 0.25f; p->echo = 0.18f;
  // Music box: an FM tine, bright strike, long ring, slightly warped comb.
  p = &sndPatch[P_MUSICBOX]; p->osc = OSC_FM; p->fmRatio = 4.0f; p->fmIndex = 0.25f; p->fmEnv = 2.4f;
  p->mdec = 0.09f; p->atk = 0.002f; p->dec = 1.5f; p->sus = 0; p->rel = 0.9f; p->drift = 7;
  p->gain = 0.26f; p->rev = 0.5f; p->echo = 0.28f;
  // Bell: the inharmonic 1:1.4 FM toll. Counts the threat clock, buries the dead.
  p = &sndPatch[P_BELL];    p->osc = OSC_FM; p->fmRatio = 1.4f; p->fmIndex = 0.8f; p->fmEnv = 3.2f;
  p->mdec = 2.0f; p->atk = 0.003f; p->dec = 4.2f; p->sus = 0; p->rel = 2.2f; p->gain = 0.33f; p->rev = 0.6f; p->echo = 0.12f;
  // Oom: a brass-ish pulse whose filter snaps open and shut on each note.
  p = &sndPatch[P_TUBA];    p->osc = OSC_PULSE; p->pw = 0.36f; p->flt = FLT_LP; p->cut = 780; p->cutEnv = 1.5f;
  p->cutKey = 0.5f; p->res = 0.3f; p->mdec = 0.13f; p->atk = 0.008f; p->dec = 0.4f; p->sus = 0.32f; p->rel = 0.11f;
  p->gain = 0.40f; p->rev = 0.1f;
  // Pah: the waltz's short reed chord on two and three.
  p = &sndPatch[P_PAH];     p->osc = OSC_WT; p->wt = WT_REED; p->detune = 9; p->flt = FLT_LP; p->cut = 1900;
  p->atk = 0.004f; p->dec = 0.17f; p->sus = 0; p->rel = 0.08f; p->gain = 0.16f; p->rev = 0.2f;
  // Theremin: the haunted lead. Swoops between notes, sings late.
  p = &sndPatch[P_THEREMIN]; p->osc = OSC_SINE; p->mono = 1; p->glide = 0.085f; p->atk = 0.11f; p->dec = 0.4f;
  p->sus = 0.9f; p->rel = 0.45f; p->vibHz = 5.7f; p->vibSemi = 0.33f; p->vibDelay = 0.22f; p->drift = 4;
  p->gain = 0.30f; p->rev = 0.5f; p->echo = 0.34f;
  // Whistle: someone on the road, far off.
  p = &sndPatch[P_WHISTLE]; p->osc = OSC_SINE; p->noise = 0.012f; p->flt = FLT_BP; p->cut = 1800; p->cutKey = 1.0f;
  p->res = 0.2f; p->atk = 0.02f; p->dec = 0.3f; p->sus = 0.8f; p->rel = 0.1f; p->vibHz = 6.8f; p->vibSemi = 0.2f;
  p->vibDelay = 0.1f; p->gain = 0.36f; p->rev = 0.35f; p->echo = 0.2f;
  // Choir: dead voices, slow to come and slower to leave.
  p = &sndPatch[P_CHOIR];   p->osc = OSC_WT; p->wt = WT_VOX; p->detune = 8; p->flt = FLT_LP; p->cut = 2300;
  p->atk = 0.75f; p->dec = 0.6f; p->sus = 0.85f; p->rel = 1.6f; p->vibHz = 4.9f; p->vibSemi = 0.07f;
  p->vibDelay = 0.4f; p->gain = 0.18f; p->rev = 0.62f;
  // Hollow: a clarinet heard through a wall. Night melodies.
  p = &sndPatch[P_HOLLOW];  p->osc = OSC_WT; p->wt = WT_HOLLOW; p->flt = FLT_LP; p->cut = 2400; p->cutKey = 0.3f;
  p->atk = 0.06f; p->dec = 0.45f; p->sus = 0.8f; p->rel = 0.25f; p->vibHz = 5.1f; p->vibSemi = 0.1f;
  p->vibDelay = 0.3f; p->drift = 5; p->gain = 0.28f; p->rev = 0.42f; p->echo = 0.2f;
  // Glass: a wet finger on a rim. Sustained, pure, a little sick.
  p = &sndPatch[P_GLASS];   p->osc = OSC_FM; p->fmRatio = 2.0f; p->fmIndex = 0.12f; p->atk = 0.4f; p->dec = 1.0f;
  p->sus = 0.7f; p->rel = 1.2f; p->tremHz = 3.1f; p->tremDepth = 0.25f; p->drift = 9; p->gain = 0.2f; p->rev = 0.6f; p->echo = 0.3f;

  // Thump: a heartbeat / kick the little cone can actually play -- a click
  // and a fast downward sweep ending near 80 Hz, which the ear reads as low.
  p = &sndPatch[P_THUMP];   p->osc = OSC_SINE; p->noise = 0.35f; p->flt = FLT_LP; p->cut = 160; p->cutEnv = 4.5f;
  p->mdec = 0.025f; p->pEnvSemi = 19; p->pEnvTime = 0.035f; p->atk = 0.001f; p->dec = 0.26f; p->sus = 0;
  p->rel = 0.05f; p->gain = 0.62f; p->rev = 0.08f; p->xpose = 9;
  // Tick: the clock.
  p = &sndPatch[P_TICK];    p->osc = OSC_FM; p->fmRatio = 3.51f; p->fmIndex = 0.5f; p->fmEnv = 2.5f; p->mdec = 0.012f;
  p->atk = 0.0005f; p->dec = 0.045f; p->sus = 0; p->rel = 0.02f; p->gain = 0.22f; p->rev = 0.22f;
  p = &sndPatch[P_SNARE];   p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_BP; p->cut = 1900; p->res = 0.35f;
  p->atk = 0.001f; p->dec = 0.16f; p->sus = 0; p->rel = 0.05f; p->gain = 0.30f; p->rev = 0.25f;
  p = &sndPatch[P_HAT];     p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_HP; p->cut = 6200;
  p->atk = 0.0005f; p->dec = 0.045f; p->sus = 0; p->rel = 0.02f; p->gain = 0.07f; p->rev = 0.1f;
  // Swell: a reversed cymbal, the breath before something.
  p = &sndPatch[P_SWELL];   p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_HP; p->cut = 2300; p->res = 0.2f;
  p->atk = 0.55f; p->dec = 0.1f; p->sus = 1.0f; p->rel = 0.04f; p->gain = 0.20f; p->rev = 0.45f;
  p = &sndPatch[P_TOM];     p->osc = OSC_TRI; p->pEnvSemi = 7; p->pEnvTime = 0.05f; p->atk = 0.001f; p->dec = 0.55f;
  p->sus = 0; p->rel = 0.1f; p->gain = 0.48f; p->rev = 0.35f;
  p = &sndPatch[P_CHAIN];   p->osc = OSC_FM; p->fmRatio = 2.76f; p->fmIndex = 1.5f; p->fmEnv = 4.0f; p->mdec = 0.06f;
  p->atk = 0.001f; p->dec = 0.3f; p->sus = 0; p->rel = 0.05f; p->gain = 0.18f; p->rev = 0.3f;

  // Wind: always under the day. The director owns its cutoff and level.
  p = &sndPatch[P_WIND];    p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_LP; p->cut = 720; p->res = 0.7f;
  p->fltLfoHz = 0.09f; p->fltLfoOct = 1.0f; p->atk = 3.0f; p->dec = 1; p->sus = 1; p->rel = 4.0f;
  p->gain = 0.04f; p->rev = 0.25f;
  // Drip: rain on tin, water in a tunnel. Pitched from the key, so the rain is in tune.
  p = &sndPatch[P_DRIP];    p->osc = OSC_SINE; p->pEnvSemi = -9; p->pEnvTime = 0.012f; p->atk = 0.001f;
  p->dec = 0.2f; p->sus = 0; p->rel = 0.05f; p->gain = 0.15f; p->rev = 0.5f; p->echo = 0.35f;
  p = &sndPatch[P_CRACKLE]; p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_HP; p->cut = 1700; p->res = 0.25f;
  p->atk = 0.0005f; p->dec = 0.014f; p->sus = 0; p->rel = 0.01f; p->gain = 0.26f; p->rev = 0.15f;
  p = &sndPatch[P_GEIGER];  p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_HP; p->cut = 3400;
  p->atk = 0.0005f; p->dec = 0.005f; p->sus = 0; p->rel = 0.005f; p->gain = 0.34f;
  // Rumble: thunder, quakes, doors in the dark. Filter falls as it decays.
  p = &sndPatch[P_RUMBLE];  p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_LP; p->cut = 480; p->cutEnv = 1.6f;
  p->res = 0.42f; p->matk = 0.001f; p->mdec = 1.2f; p->atk = 0.015f; p->dec = 2.6f; p->sus = 0; p->rel = 0.8f;
  p->tremHz = 7.5f; p->tremDepth = 0.35f; p->gain = 0.55f; p->rev = 0.45f;

  // Siren: the air-raid wail, a slow tritone sweep.
  p = &sndPatch[P_SIREN];   p->osc = OSC_PULSE; p->pw = 0.42f; p->flt = FLT_LP; p->cut = 1700; p->cutKey = 0.4f;
  p->res = 0.2f; p->vibHz = 0.42f; p->vibSemi = 3.0f; p->atk = 0.25f; p->dec = 1; p->sus = 1; p->rel = 0.5f;
  p->gain = 0.26f; p->rev = 0.3f; p->echo = 0.15f;
  // Growl: the Doom's throat. Detuned saws through a wobbling resonant filter.
  p = &sndPatch[P_GROWL];   p->osc = OSC_SAW; p->detune = 28; p->flt = FLT_LP; p->cut = 540; p->cutKey = 0.3f;
  p->res = 0.55f; p->fltLfoHz = 3.3f; p->fltLfoOct = 0.6f; p->tremHz = 6.5f; p->tremDepth = 0.3f;
  p->atk = 0.12f; p->dec = 1; p->sus = 1; p->rel = 0.5f; p->gain = 0.34f; p->rev = 0.45f;
  p = &sndPatch[P_RADIO];   p->osc = OSC_PULSE; p->pw = 0.5f; p->flt = FLT_BP; p->cut = 1500; p->cutKey = 0.7f;
  p->res = 0.45f; p->atk = 0.002f; p->dec = 0.06f; p->sus = 0.8f; p->rel = 0.02f; p->gain = 0.22f;
  p = &sndPatch[P_SQUELCH]; p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_BP; p->cut = 2100; p->res = 0.5f;
  p->atk = 0.001f; p->dec = 0.09f; p->sus = 0; p->rel = 0.02f; p->gain = 0.20f;
  p = &sndPatch[P_ZAP];     p->osc = OSC_PULSE; p->pw = 0.14f; p->flt = FLT_HP; p->cut = 700; p->pEnvSemi = 14;
  p->pEnvTime = 0.02f; p->atk = 0.001f; p->dec = 0.09f; p->sus = 0; p->rel = 0.03f; p->gain = 0.20f; p->rev = 0.25f;
  // Squish: sludge, wounds. FM with a falling pitch and a clogged filter.
  p = &sndPatch[P_SQUISH];  p->osc = OSC_FM; p->fmRatio = 1.5f; p->fmIndex = 2.2f; p->fmEnv = 3.0f; p->mdec = 0.15f;
  p->flt = FLT_LP; p->cut = 900; p->res = 0.5f; p->pEnvSemi = 7; p->pEnvTime = 0.12f; p->atk = 0.004f;
  p->dec = 0.35f; p->sus = 0; p->rel = 0.1f; p->gain = 0.34f; p->rev = 0.2f;
  p = &sndPatch[P_WHOOSH];  p->osc = OSC_NOISE; p->noise = 1.0f; p->flt = FLT_BP; p->cut = 500; p->cutEnv = 2.2f;
  p->res = 0.4f; p->matk = 0.35f; p->mdec = 0.8f; p->atk = 0.3f; p->dec = 0.9f; p->sus = 0; p->rel = 0.4f;
  p->gain = 0.30f; p->rev = 0.4f;
  p = &sndPatch[P_DOORGRIND]; p->osc = OSC_SAW; p->detune = 35; p->noise = 0.5f; p->flt = FLT_BP; p->cut = 420;
  p->res = 0.6f; p->fltLfoHz = 9.0f; p->fltLfoOct = 0.5f; p->tremHz = 13.0f; p->tremDepth = 0.5f;
  p->atk = 0.08f; p->dec = 1; p->sus = 1; p->rel = 0.25f; p->gain = 0.30f; p->rev = 0.35f;
  p = &sndPatch[P_TESTSINE]; p->osc = OSC_SINE; p->atk = 0.02f; p->dec = 1; p->sus = 1; p->rel = 0.08f;
  p->gain = 0.5f; p->rev = 0; p->echo = 0;
  return true;
}

// ── 4. Envelopes ─────────────────────────────────────────────────────────────
// Exponential, evaluated once per control block; the amp envelope's value is
// ramped linearly across the block, so a 2 ms block never steps audibly.
enum : uint8_t { ENV_IDLE = 0, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };
struct SndEnv { float lvl, aK, dK, rK, sus; uint8_t st; };

static inline float sndEnvK(float sec, float tc) {   // tc time constants over sec, at the control rate
  if (sec <= 0.0005f) return 1.0f;
  float x = tc / (sec * SND_KRATE);
  return x > 20.0f ? 1.0f : 1.0f - expf(-x);
}
static inline void sndEnvStart(SndEnv& e, float a, float d, float s, float r) {
  e.aK = sndEnvK(a, 1.61f);          // aims at 1.25 and stops at 1.0: an analog-style curve
  e.dK = sndEnvK(d, 6.9f);           // decay and release times are "to -60 dB"
  e.rK = sndEnvK(r, 6.9f);
  e.sus = s; e.st = ENV_ATK;         // lvl is kept: a retrigger never clicks down to zero
}
static inline float sndEnvTick(SndEnv& e) {
  switch (e.st) {
    case ENV_ATK:
      e.lvl += (1.25f - e.lvl) * e.aK;
      if (e.lvl >= 1.0f) { e.lvl = 1.0f; e.st = ENV_DEC; }
      break;
    case ENV_DEC:
      e.lvl += (e.sus - e.lvl) * e.dK;
      if (e.lvl - e.sus < 0.0004f) { e.lvl = e.sus; e.st = (e.sus < 0.0004f) ? ENV_IDLE : ENV_SUS; }
      break;
    case ENV_REL:
      e.lvl -= e.lvl * e.rK;
      if (e.lvl < 0.0003f) { e.lvl = 0.0f; e.st = ENV_IDLE; }
      break;
    default: break;
  }
  return e.lvl;
}

// ── 5. Voices ────────────────────────────────────────────────────────────────
enum SndBus : uint8_t { SB_MUSIC = 0, SB_SFX = 1 };
static constexpr int SND_VOICES  = 18;
static constexpr int SND_MUS_END = 12;   // voices [0,12) play music, [12,18) effects

struct SndVoice {
  const SndPatch* p;           // nullptr = free
  uint8_t  bus, part, held, pad;
  uint32_t serial;
  int32_t  gate;               // samples until auto-release; <0 = until sndNoteOff
  float    vel, midi, cur, glideK;
  float    bend, bendTo, bendK;   // semitones; the owner steers sweeps with these
  float    ph1, ph2, fph;
  float    pEnv, pEnvK;
  SndEnv   amp, mod;
  float    lfo, flfo, trem, vibFade, vibFadeK;
  float    drift, driftTgt;
  float    ic1, ic2;
  float    ampPrev;
  float    level;              // owner-controlled gain (textures, fades)
  float    cutMul;             // owner-controlled cutoff multiplier (wind, fog)
  uint32_t rng;
};

// The shared state of the synth: buses, pool, effect parameters.
struct SndCore {
  SndVoice* v;                 // [SND_VOICES]
  uint32_t  serial;
  uint32_t  rng;
  float     busPitch[2];       // semitones added to every voice on the bus (tape wow)
  float     busSpeed[2];       // frequency multiplier (tape speed: wind-down, power-down)
  float     busGain[2];        // level per bus, applied at the voice so it scales the sends too
  float     noiseMul;          // scales every patch's noise (breath, wind, hats); GET /snddbg?noise=
  float     fxMul;             // scales the reverb and echo returns; GET /snddbg?rev=
  // one block of each bus
  float     dry[2][SND_BLK];
  float     rev[SND_BLK];
  float     echo[SND_BLK];
};
static SndCore SC;

static void sndVoicesInit() {
  SC.v = (SndVoice*)sndAlloc(sizeof(SndVoice) * SND_VOICES, true);
  SC.busSpeed[0] = SC.busSpeed[1] = 1.0f;
  SC.busGain[0] = 0.8f; SC.busGain[1] = 1.0f;
  SC.noiseMul = 1.0f;
  SC.fxMul = 1.0f;
  SC.rng = 0x9E3779B9u;
}

static SndVoice* sndVoiceAlloc(uint8_t bus, uint8_t part) {
  int a = (bus == SB_SFX) ? SND_MUS_END : 0, b = (bus == SB_SFX) ? SND_VOICES : SND_MUS_END;
  SndVoice* best = nullptr; uint32_t bestScore = 0;
  for (int i = a; i < b; i++) {
    SndVoice& v = SC.v[i];
    if (!v.p) return &v;
    // Steal order: releasing before sounding, same part before others,
    // oldest first. Stealing a sounding voice can click; it is the last resort.
    uint32_t age = SC.serial - v.serial; if (age > 0x00FFFFFFu) age = 0x00FFFFFFu;
    uint32_t score = age + (v.amp.st == ENV_REL ? 0x04000000u : 0) + (v.part == part ? 0x02000000u : 0)
                   + (v.amp.lvl < 0.05f ? 0x01000000u : 0);
    if (!best || score > bestScore) { best = &v; bestScore = score; }
  }
  return best;
}

// Start a note. gateSamples < 0 holds it until sndNoteOff().
// Debug: bit per music part (MusPart); a cleared bit mutes that part. The
// desktop harness uses it to render stems; the board never touches it.
static uint32_t sndPartMask = 0xFFFFFFFFu;
// Debug: called on every note start (the harness draws a piano roll from it).
static void (*sndNoteHook)(uint8_t bus, uint8_t part, uint8_t patch, float midi, float vel, int32_t gate) = nullptr;

static SndVoice* sndNoteOn(uint8_t bus, uint8_t part, uint8_t patch, float midi, float vel,
                           int32_t gateSamples, SndVoice* legatoFrom = nullptr) {
  if (!SC.v || !sndPatch || patch >= P_COUNT) return nullptr;
  if (bus == SB_MUSIC && part < 32 && !((sndPartMask >> part) & 1u)) return nullptr;
  if (sndNoteHook) sndNoteHook(bus, part, patch, midi, vel, gateSamples);
  const SndPatch* p = &sndPatch[patch];
  SndVoice* v = nullptr;
  bool legato = false;
  // A releasing voice counts too: the composer ends each lead note a tick
  // before the next, so requiring `held` meant a legato patch never glided.
  if (legatoFrom && legatoFrom->p == p && p->mono) { v = legatoFrom; legato = true; }
  if (!v) v = sndVoiceAlloc(bus, part);
  if (!v) return nullptr;
  if (!legato) {
    bool stolen = (v->p != nullptr);
    // Half-level restart softens a steal. The envelope and the output gain
    // are different units: ampPrev carries patch gain, velocity, level and
    // bus gain, so it is halved on its own, not set from the envelope.
    float keepLvl = stolen ? v->amp.lvl * 0.5f : 0.0f;
    float keepAmp = stolen ? v->ampPrev * 0.5f : 0.0f;
    memset(v, 0, sizeof(*v));
    v->amp.lvl = keepLvl; v->ampPrev = keepAmp;
    v->cur = midi;
    v->rng = 0x2545F491u ^ (SC.serial * 747796405u + 1u);
    v->ph1 = 0.0f; v->ph2 = 0.37f; v->fph = 0.0f;
    v->pEnv = p->pEnvSemi;
    v->pEnvK = p->pEnvTime > 0 ? sndEnvK(p->pEnvTime, 3.0f) : 1.0f;
    v->vibFadeK = p->vibDelay > 0 ? sndEnvK(p->vibDelay, 3.0f) : 1.0f;
    v->vibFade = p->vibDelay > 0 ? 0.0f : 1.0f;
    v->drift = p->drift > 0 ? sndRandBi(SC.rng) * p->drift : 0.0f;
    v->driftTgt = v->drift;
    v->level = 1.0f; v->cutMul = 1.0f;
    v->glideK = 1.0f;
    v->lfo = sndRandF(SC.rng); v->flfo = sndRandF(SC.rng); v->trem = sndRandF(SC.rng);
    sndEnvStart(v->amp, p->atk, p->dec, p->sus, p->rel);
    sndEnvStart(v->mod, p->matk, p->mdec, p->msus, p->mrel);
  } else {
    // Legato: slide to the new pitch, restart the mod env, keep the amp going.
    v->glideK = p->glide > 0 ? sndEnvK(p->glide, 3.0f) : 1.0f;
    sndEnvStart(v->mod, p->matk, p->mdec, p->msus, p->mrel);
    if (v->amp.st == ENV_REL || v->amp.st == ENV_IDLE) sndEnvStart(v->amp, p->atk, p->dec, p->sus, p->rel);
  }
  v->p = p; v->bus = bus; v->part = part; v->held = 1;
  v->serial = ++SC.serial;
  v->gate = gateSamples;
  v->vel = sndClampF(vel, 0.0f, 1.0f);
  v->midi = midi;
  return v;
}
static inline void sndNoteOff(SndVoice* v) {
  if (!v || !v->p || !v->held) return;
  v->held = 0;
  v->amp.st = ENV_REL; v->mod.st = ENV_REL;
}
static void sndReleaseBus(uint8_t bus, float relSec) {
  for (int i = 0; i < SND_VOICES; i++) {
    SndVoice& v = SC.v[i];
    if (!v.p || v.bus != bus) continue;
    v.held = 0; v.amp.st = ENV_REL; v.amp.rK = sndEnvK(relSec, 6.9f);
  }
}
static void sndReleasePart(uint8_t bus, uint8_t part, float relSec) {
  for (int i = 0; i < SND_VOICES; i++) {
    SndVoice& v = SC.v[i];
    if (!v.p || v.bus != bus || v.part != part) continue;
    v.held = 0; v.amp.st = ENV_REL; if (relSec >= 0) v.amp.rK = sndEnvK(relSec, 6.9f);
  }
}
static int sndActiveVoices() {
  int n = 0; if (!SC.v) return 0;
  for (int i = 0; i < SND_VOICES; i++) if (SC.v[i].p) n++;
  return n;
}

// One voice, one control block: update the slow stuff, then run the samples.
static void sndVoiceBlock(SndVoice& v) {
  const SndPatch& p = *v.p;
  // ── control rate ──
  if (v.gate >= 0) { v.gate -= SND_BLK; if (v.gate < 0 && v.held) sndNoteOff(&v); }
  float ampLvl = sndEnvTick(v.amp);
  float modLvl = sndEnvTick(v.mod);
  if (v.amp.st == ENV_IDLE && v.ampPrev < 0.0005f) { v.p = nullptr; return; }

  v.cur += (v.midi - v.cur) * v.glideK;
  v.bend += (v.bendTo - v.bend) * v.bendK;
  v.pEnv -= v.pEnv * v.pEnvK;
  if (p.drift > 0) {
    if (sndChance(v.rng, 0.004f)) v.driftTgt = sndRandBi(v.rng) * p.drift;
    v.drift += (v.driftTgt - v.drift) * 0.004f;
  }
  float vib = 0.0f;
  if (p.vibSemi > 0) {
    v.lfo += p.vibHz * (SND_BLK * SND_INV_SR); if (v.lfo >= 1.0f) v.lfo -= 1.0f;
    v.vibFade += (1.0f - v.vibFade) * v.vibFadeK;
    vib = sndSinP(v.lfo) * p.vibSemi * v.vibFade;
  }
  float pitch = v.cur + p.xpose + v.bend + v.pEnv + vib + v.drift * 0.01f + SC.busPitch[v.bus];
  float hz = sndMidiHz(pitch) * SC.busSpeed[v.bus];
  if (hz > 7600.0f) hz = 7600.0f;
  float inc = hz * SND_INV_SR;
  float inc1 = inc, inc2 = inc;
  if (p.detune != 0.0f) {
    float r = sndExp2(p.detune * (1.0f / 2400.0f));
    inc1 = inc / r; inc2 = inc * r;
  }
  float trem = 1.0f;
  if (p.tremDepth > 0) {
    v.trem += p.tremHz * (SND_BLK * SND_INV_SR); if (v.trem >= 1.0f) v.trem -= 1.0f;
    trem = 1.0f - p.tremDepth * (0.5f + 0.5f * sndSinP(v.trem));
  }
  float velG = v.vel * (0.35f + 0.65f * v.vel);           // a gentle velocity curve
  float ampTgt = ampLvl * p.gain * velG * v.level * trem * SC.busGain[v.bus];
  float ampInc = (ampTgt - v.ampPrev) * (1.0f / SND_BLK);
  float amp = v.ampPrev;
  v.ampPrev = ampTgt;

  // filter coefficients (TPT state-variable filter)
  float a1 = 0, a2 = 0, a3 = 0, kq = 0;
  if (p.flt != FLT_OFF) {
    float oct = p.cutEnv * modLvl + p.cutKey * (pitch - 60.0f) * (1.0f / 12.0f);
    if (p.fltLfoOct > 0) {
      v.flfo += p.fltLfoHz * (SND_BLK * SND_INV_SR); if (v.flfo >= 1.0f) v.flfo -= 1.0f;
      oct += p.fltLfoOct * sndSinP(v.flfo);
    }
    float fc = sndClampF(p.cut * sndExp2(oct) * v.cutMul, 25.0f, 0.42f * SND_SR);
    float w = 3.14159265f * fc * SND_INV_SR;               // Pade tan(), plenty below 0.42 fs
    float w2 = w * w;
    float g = w * (15.0f - w2) / (15.0f - 6.0f * w2);
    kq = 2.0f - 2.0f * sndClampF(p.res, 0.0f, 0.97f);
    a1 = 1.0f / (1.0f + g * (g + kq)); a2 = g * a1; a3 = g * a2;
  }
  float idx = p.fmIndex + p.fmEnv * modLvl;
  float finc = inc * p.fmRatio;
  // Keep FM sidebands under Nyquist (Carson: they reach ~fc + (idx+1) fm). A
  // music box at D6 with a 4:1 modulator folded its bright attack back down
  // as inharmonic fizz -- which a 2 W cone, bright at 3-6 kHz, plays loudly.
  if (p.osc == OSC_FM && finc > 0.0f) {
    float fmHz = hz * p.fmRatio, cap = (7200.0f - hz) / fmHz - 1.0f;
    if (cap < 0.05f) cap = 0.05f;
    if (idx > cap) idx = cap;
  }
  const float noiseAmt = p.noise * SC.noiseMul;

  // ── audio rate ──
  float ph1 = v.ph1, ph2 = v.ph2, fph = v.fph, ic1 = v.ic1, ic2 = v.ic2;
  uint32_t rng = v.rng;
  const int16_t* wt = (p.osc == OSC_WT) ? sndWtFor(p.wt, hz) : nullptr;
  const bool two = (p.detune != 0.0f);
  const float idt1 = inc1 > 0 ? 1.0f / inc1 : 0.0f, idt2 = inc2 > 0 ? 1.0f / inc2 : 0.0f;
  float* dry = SC.dry[v.bus];
  const float sRev = p.rev, sEcho = p.echo;
  for (int i = 0; i < SND_BLK; i++) {
    float s;
    switch (p.osc) {
      case OSC_SINE: s = sndSinP(ph1); break;
      case OSC_TRI:  s = ph1 < 0.5f ? 4.0f * ph1 - 1.0f : 3.0f - 4.0f * ph1; break;
      case OSC_SAW:
        s = 2.0f * ph1 - 1.0f - sndBlep(ph1, inc1, idt1);
        if (two) s = 0.5f * (s + 2.0f * ph2 - 1.0f - sndBlep(ph2, inc2, idt2));
        break;
      case OSC_PULSE: {
        float q = ph1 + 1.0f - p.pw; if (q >= 1.0f) q -= 1.0f;
        s = (ph1 < p.pw ? 1.0f : -1.0f) + sndBlep(ph1, inc1, idt1) - sndBlep(q, inc1, idt1);
        if (two) {
          float q2 = ph2 + 1.0f - p.pw; if (q2 >= 1.0f) q2 -= 1.0f;
          s = 0.5f * (s + (ph2 < p.pw ? 1.0f : -1.0f) + sndBlep(ph2, inc2, idt2) - sndBlep(q2, inc2, idt2));
        }
        break;
      }
      case OSC_WT:
        s = sndWtP(wt, ph1);
        if (two) s = 0.5f * (s + sndWtP(wt, ph2));
        break;
      case OSC_FM: {
        float m = sndSinP(fph) * idx;
        s = sndSinP(sndWrap01(ph1 + m));
        fph += finc; if (fph >= 1.0f) fph -= 1.0f;
        break;
      }
      default: s = 0.0f; break;
    }
    ph1 += inc1; if (ph1 >= 1.0f) ph1 -= 1.0f;
    ph2 += inc2; if (ph2 >= 1.0f) ph2 -= 1.0f;
    if (noiseAmt > 0.0f) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                           s += noiseAmt * ((float)(int32_t)rng * (1.0f / 2147483648.0f)); }
    if (p.flt != FLT_OFF) {
      float v3 = s - ic2;
      float v1 = a1 * ic1 + a2 * v3;
      float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
      s = (p.flt == FLT_LP) ? v2 : (p.flt == FLT_BP) ? v1 : (s - kq * v1 - v2);
    }
    amp += ampInc;
    s *= amp;
    dry[i] += s; SC.rev[i] += s * sRev; SC.echo[i] += s * sEcho;
  }
  v.ph1 = ph1; v.ph2 = ph2; v.fph = fph; v.ic1 = ic1; v.ic2 = ic2; v.rng = rng;
}

// ── 6. Reverb ────────────────────────────────────────────────────────────────
// A four-line feedback delay network behind two diffusing allpasses. Dark by
// design: each line lowpasses what it feeds back, so the tail loses its top
// first, the way sound dies in a concrete room.
struct SndReverb {
  float* buf; int len[4]; int pos[4]; float* line[4];
  float  fb[4], lp[4];
  float* ap[2]; int apLen[2], apPos[2];
  float  damp, t60, size, wet;
};
static SndReverb SR_;

static void sndReverbSet(float t60, float damp, float wet) {
  SR_.t60 = sndClampF(t60, 0.3f, 12.0f); SR_.damp = sndClampF(damp, 0.05f, 1.0f); SR_.wet = wet;
  for (int k = 0; k < 4; k++)
    SR_.fb[k] = powf(10.0f, -3.0f * (float)SR_.len[k] / (SR_.t60 * SND_SR));
}
static bool sndReverbInit() {
  static const int L[4] = { 1049, 1307, 1619, 1931 };   // 66..121 ms, mutually prime
  static const int A[2] = { 142, 107 };
  int total = L[0] + L[1] + L[2] + L[3] + A[0] + A[1];
  SR_.buf = (float*)sndAlloc(sizeof(float) * total, false);
  if (!SR_.buf) return false;
  float* q = SR_.buf;
  for (int k = 0; k < 4; k++) { SR_.len[k] = L[k]; SR_.line[k] = q; q += L[k]; SR_.pos[k] = 0; SR_.lp[k] = 0; }
  for (int k = 0; k < 2; k++) { SR_.apLen[k] = A[k]; SR_.ap[k] = q; q += A[k]; SR_.apPos[k] = 0; }
  sndReverbSet(2.4f, 0.35f, 0.5f);
  return true;
}
// in: the send bus; out: added into dst
static void sndReverbBlock(const float* in, float* dst, float gain) {
  if (!SR_.buf) return;
  const float d = SR_.damp, wet = SR_.wet * gain;
  for (int i = 0; i < SND_BLK; i++) {
    float x = in[i];
    for (int k = 0; k < 2; k++) {                          // diffusion
      float* a = SR_.ap[k]; int& p = SR_.apPos[k];
      float z = a[p]; float y = z - 0.6f * x; a[p] = x + 0.6f * y; x = y;
      if (++p >= SR_.apLen[k]) p = 0;
    }
    float o[4];
    for (int k = 0; k < 4; k++) {
      float r = SR_.line[k][SR_.pos[k]];
      SR_.lp[k] += d * (r - SR_.lp[k]);
      o[k] = SR_.lp[k];
    }
    float h0 = 0.5f * (o[0] + o[1] + o[2] + o[3]);
    float h1 = 0.5f * (o[0] - o[1] + o[2] - o[3]);
    float h2 = 0.5f * (o[0] + o[1] - o[2] - o[3]);
    float h3 = 0.5f * (o[0] - o[1] - o[2] + o[3]);
    SR_.line[0][SR_.pos[0]] = x + h0 * SR_.fb[0];
    SR_.line[1][SR_.pos[1]] = x + h1 * SR_.fb[1];
    SR_.line[2][SR_.pos[2]] = x + h2 * SR_.fb[2];
    SR_.line[3][SR_.pos[3]] = x + h3 * SR_.fb[3];
    for (int k = 0; k < 4; k++) if (++SR_.pos[k] >= SR_.len[k]) SR_.pos[k] = 0;
    dst[i] += (o[0] + o[1] + o[2] + o[3]) * 0.35f * wet;
  }
}

// ── 7. Tape echo ─────────────────────────────────────────────────────────────
// One head, a darkening feedback loop, and a read position that glides when
// the time changes -- so a tempo change bends the echoes like real tape.
static constexpr int SND_ECHO_N = 14000;   // 875 ms
struct SndEcho { float* buf; int w; float time, timeTgt, fb, wet, lp, hp, hpz; float wow, wowPh; };
static SndEcho SE_;
static bool sndEchoInit() {
  SE_.buf = (float*)sndAlloc(sizeof(float) * SND_ECHO_N, false);
  if (!SE_.buf) return false;
  SE_.time = SE_.timeTgt = 5600.0f; SE_.fb = 0.38f; SE_.wet = 0.35f;
  return true;
}
static void sndEchoSet(float seconds, float fb, float wet) {
  SE_.timeTgt = sndClampF(seconds * SND_SR, 200.0f, (float)(SND_ECHO_N - 4));
  SE_.fb = sndClampF(fb, 0.0f, 0.85f); SE_.wet = wet;
}
static void sndEchoBlock(const float* in, float* dst, float gain) {
  if (!SE_.buf) return;
  SE_.time += (SE_.timeTgt - SE_.time) * 0.02f;           // per block: ~100 ms glide
  SE_.wowPh += 0.35f * SND_BLK * SND_INV_SR; if (SE_.wowPh >= 1.0f) SE_.wowPh -= 1.0f;
  float t = SE_.time + sndSinP(SE_.wowPh) * 6.0f;         // a little flutter on the head
  const float wet = SE_.wet * gain;
  for (int i = 0; i < SND_BLK; i++) {
    float rp = (float)SE_.w - t; while (rp < 0) rp += SND_ECHO_N;
    int i0 = (int)rp; float f = rp - (float)i0; int i1 = i0 + 1; if (i1 >= SND_ECHO_N) i1 = 0;
    float r = SE_.buf[i0] + f * (SE_.buf[i1] - SE_.buf[i0]);
    SE_.lp += 0.45f * (r - SE_.lp);                       // ~2.3 kHz: each repeat darker
    float hpOut = SE_.lp - SE_.hpz; SE_.hpz += 0.06f * hpOut;   // and thinner
    SE_.buf[SE_.w] = in[i] + hpOut * SE_.fb;
    if (++SE_.w >= SND_ECHO_N) SE_.w = 0;
    dst[i] += hpOut * wet;
  }
}

// ── 8. Master ────────────────────────────────────────────────────────────────
// music (+ reverb and echo returns) -> world filter (fog, depth) -> + effects
// + speech -> DC block -> 4th-order 240 Hz high-pass -> gentle compressor ->
// look-ahead limiter at -3 dBFS -> volume -> int16.
//
// What the board taught (2026-09-23, first listen on the K10): its amp is hot
// -- volume 1 at -22 dBFS was "really loud" -- and the voice and the loud
// effects crackled at every volume setting. The first master pushed 4 dB of
// makeup gain into a cubic soft clipper that sat BEFORE the volume stage, so
// every loud peak was distorted inside the engine and the volume knob only
// made the same crackle quieter. Now nothing is clipped or saturated
// anywhere: the limiter looks one block ahead and turns down before a peak
// arrives. And the 2 W cone gets nothing below ~240 Hz; energy it cannot
// move is energy it rattles on.
struct SndMaster {
  float worldCut, worldCutTgt, wz1, wz2;   // world lowpass (2-pole)
  float hp[2][4];                          // two biquad sections: x1 x2 y1 y2
  float hb[2][5];                          // b0 b1 b2 a1 a2
  float lp[4], lb[5], lpHz;                // treble roll-off (0 Hz = off); GET /snddbg?lp=
  float hpHz;                              // the high-pass corner; GET /snddbg?hp=
  float dcX, dcY;
  float env, gain;                         // compressor
  float compThr;                           // where its 2:1 starts, linear (0.25 = -12 dBFS); GET /snddbg?comp=
  float musGain;                           // the music bus at music level 9; GET /snddbg?mus=
  float duckAmt;                           // how far speech pushes the music down (0.55 = to 45%); GET /snddbg?duck=
  float lim, needPrev;                     // limiter: current gain, what the delayed block needs
  float dly[SND_BLK];                      // one block of look-ahead
  float vol, volTgt;
  float sfxGain, speechGain;
  float duck, duckTgt;
  float peak;                              // for the serial meter (pre-volume, post-limiter)
  uint32_t clipped;                        // samples that reached the final safety clamp (should stay 0)
  uint32_t limited;                        // blocks where the limiter had to act
};
static SndMaster SM;
static constexpr float SND_CEIL = 0.708f;  // -3 dBFS: the loudest sample the engine will ever write

static void sndBiquadHP(float* c, float fc, float q) {
  float w0 = 6.2831853f * fc / SND_SR, cw = cosf(w0), sw = sinf(w0), al = sw / (2.0f * q);
  float a0 = 1.0f + al;
  c[0] = (1.0f + cw) * 0.5f / a0; c[1] = -(1.0f + cw) / a0; c[2] = c[0];
  c[3] = -2.0f * cw / a0;         c[4] = (1.0f - al) / a0;
}
// The treble roll-off. The K10's cone is bright from ~3 kHz up, and the
// first listen heard everything up there -- breath noise, FM fizz, the voice's
// sibilants and its 8 kHz images -- as "static that follows the music".
static void sndMasterSetLP(float hz) {
  SM.lpHz = hz;
  if (hz <= 0.0f) return;
  float w0 = 6.2831853f * sndClampF(hz, 500.0f, 7500.0f) / SND_SR, cw = cosf(w0), sw = sinf(w0);
  float al = sw / (2.0f * 0.7071f), a0 = 1.0f + al;
  SM.lb[0] = (1.0f - cw) * 0.5f / a0; SM.lb[1] = (1.0f - cw) / a0; SM.lb[2] = SM.lb[0];
  SM.lb[3] = -2.0f * cw / a0;         SM.lb[4] = (1.0f - al) / a0;
}
// 4th-order Butterworth high-pass: two sections, Q 0.541 and 1.307.
static void sndMasterSetHP(float hz) {
  SM.hpHz = sndClampF(hz, 40.0f, 1000.0f);
  sndBiquadHP(SM.hb[0], SM.hpHz, 0.5412f);
  sndBiquadHP(SM.hb[1], SM.hpHz, 1.3066f);
}
static void sndMasterInit() {
  memset(&SM, 0, sizeof(SM));
  SM.worldCut = SM.worldCutTgt = 7000.0f;
  sndMasterSetHP(240.0f);
  sndMasterSetLP(4800.0f);
  SM.gain = 1.0f; SM.lim = 1.0f; SM.needPrev = 1.0f;
  SM.vol = SM.volTgt = 0.05f;
  // The owner's mix, tuned by ear on the sound desk ("best setting for mid
  // volume", 2026-09-23): music up 5 dB, effects up 2.3 dB, the voice down
  // 6 dB from the first guess (0.5 / 0.5 / 0.8).
  SM.sfxGain = 0.65f; SM.speechGain = 0.39f;
  SM.musGain = 0.89f; SM.duckAmt = 0.55f; SM.compThr = 0.25f;
  SM.duck = SM.duckTgt = 1.0f;
}

// mus/sfx/speech: one block each (speech may be null). out: SND_BLK stereo frames.
static void sndMasterBlock(float* mus, const float* sfx, const float* speech, int16_t* out) {
  // world lowpass on the music: fog and depth close the world in
  SM.worldCut += (SM.worldCutTgt - SM.worldCut) * 0.01f;
  float w = 3.14159265f * sndClampF(SM.worldCut, 120.0f, 6700.0f) * SND_INV_SR, w2 = w * w;
  float g = w * (15.0f - w2) / (15.0f - 6.0f * w2);
  float a1 = 1.0f / (1.0f + g * (g + 1.414f)), a2 = g * a1, a3 = g * a2;
  SM.duck += (SM.duckTgt - SM.duck) * (SM.duckTgt < SM.duck ? 0.08f : 0.012f);
  SM.vol  += (SM.volTgt - SM.vol) * 0.02f;
  float mg = SM.duck;                      // the music level itself is applied per voice (SC.busGain)
  float blockPeak = 0.0f;
  float mix[SND_BLK];
  // Wide open (no fog, not underground) the world filter is a no-op: skip it.
  const bool open = (SM.worldCut > 6400.0f && SM.worldCutTgt > 6400.0f);
  for (int i = 0; i < SND_BLK; i++) {
    float m = mus[i] * mg, v2;
    if (open) { v2 = m; SM.wz1 = 0.0f; SM.wz2 = m; }
    else {
      float v3 = m - SM.wz2, v1 = a1 * SM.wz1 + a2 * v3;
      v2 = SM.wz2 + a2 * SM.wz1 + a3 * v3;
      SM.wz1 = 2.0f * v1 - SM.wz1; SM.wz2 = 2.0f * v2 - SM.wz2;
    }
    float x = v2 + sfx[i] * SM.sfxGain + (speech ? speech[i] * SM.speechGain : 0.0f);
    // DC block, then the 4th-order high-pass
    float y = x - SM.dcX + 0.995f * SM.dcY; SM.dcX = x; SM.dcY = y;
    for (int s = 0; s < 2; s++) {
      float* h = SM.hp[s]; const float* c = SM.hb[s];
      float o = c[0] * y + c[1] * h[0] + c[2] * h[1] - c[3] * h[2] - c[4] * h[3];
      h[1] = h[0]; h[0] = y; h[3] = h[2]; h[2] = o; y = o;
    }
    if (SM.lpHz > 0.0f) {
      const float* c = SM.lb; float* h = SM.lp;
      float o = c[0] * y + c[1] * h[0] + c[2] * h[1] - c[3] * h[2] - c[4] * h[3];
      h[1] = h[0]; h[0] = y; h[3] = h[2]; h[2] = o; y = o;
    }
    mix[i] = y;
    float a = sndAbsF(y); if (a > blockPeak) blockPeak = a;
  }
  // Compressor, block rate: a gentle 2:1 above -12 dBFS, no makeup gain. It
  // only evens out the loud moments so the limiter rarely has to work.
  float e = blockPeak;
  SM.env = (e > SM.env) ? SM.env + (e - SM.env) * 0.6f : SM.env + (e - SM.env) * 0.012f;
  float gTgt = 1.0f;
  if (SM.env > SM.compThr) gTgt = sndExp2(-sndLog2(SM.env / SM.compThr) * 0.5f);
  float gInc = (gTgt - SM.gain) * (1.0f / SND_BLK);
  float pk = 0.0f;
  for (int i = 0; i < SND_BLK; i++) {
    SM.gain += gInc;
    mix[i] *= SM.gain;
    float a = sndAbsF(mix[i]); if (a > pk) pk = a;
  }
  // Look-ahead limiter. The block just computed goes into the delay line and
  // the previous one comes out, so its gain can already allow for the block
  // behind it: the ramp only ever moves toward min(what the outgoing block
  // needs, what the incoming one needs), falling at once and recovering over
  // ~100 ms. No sample can exceed SND_CEIL, and nothing is waveshaped.
  float needNow = (pk > SND_CEIL) ? SND_CEIL / pk : 1.0f;
  float tgt = sndMinF(SM.needPrev, needNow);
  float g0 = SM.lim, g1 = (tgt < g0) ? tgt : g0 + (tgt - g0) * 0.03f;
  if (tgt < 0.999f) SM.limited++;
  float gStep = (g1 - g0) * (1.0f / SND_BLK), lg = g0;
  float vol = SM.vol;
  for (int i = 0; i < SND_BLK; i++) {
    lg += gStep;
    float x = SM.dly[i] * lg;
    SM.dly[i] = mix[i];
    if (x > 1.0f) { x = 1.0f; SM.clipped++; } else if (x < -1.0f) { x = -1.0f; SM.clipped++; }
    float a = sndAbsF(x); if (a > SM.peak) SM.peak = a;
    // Rounded, not truncated: on the board vol is 1 and this is the
    // full-scale handoff to the output stage (ui-audio.hpp sndOutput), which
    // applies the owner's volume.
    float f = x * vol * 32767.0f;
    int32_t s = (int32_t)(f + (f >= 0.0f ? 0.5f : -0.5f));
    out[2 * i] = out[2 * i + 1] = (int16_t)s;
  }
  SM.lim = g1; SM.needPrev = needNow;
}
