#pragma once
// ── snd-lpc.hpp ──────────────────────────────────────────────────────────────
// A TMS5220 / TMS5100 linear-predictive speech voice: the Speak & Spell chip,
// rebuilt as one more voice in the synth so it can talk OVER the music.
//
// The Talkie libraries this descends from (going-digital, ArminJo, Adafruit)
// drive an AVR/SAMD timer interrupt into a PWM or DAC pin. The ESP32-S3 has
// no DAC and the K10's speaker is on I2S, which the synth already owns, so
// none of that hardware layer is usable. What survives is the idea and the
// bitstream: frames are read in Talkie's order, so a Talkie vocabulary array
// plays unchanged. The decoder is written from the chip's published
// behaviour; the coefficient tables are TI's, as documented in MAME's
// src/devices/sound/tms5110r.hxx (BSD-3-Clause, copyright Frank Palazzolo,
// Couriersud, Jonathan Gevaryahu).
//
// Chip model: 8 kHz, a 25 ms frame is 8 interpolation steps of 25 samples; a
// voiced frame excites a 10-pole lattice with the chirp pulse once per pitch
// period, an unvoiced one with a 16-bit LFSR; the output clips to 12 bits and
// is truncated to the 8-bit DAC. That truncation, the clipping and the
// quantised reflection coefficients ARE the sound -- smoothing any of them
// away gives you a polite modern vocoder instead of a toy from 1978.
//
// Bitstream (Talkie order): bytes in order, bits LSB first within a byte,
// fields assembled MSB first. Frame = E(4) [0 = silence, 15 = stop] R(1)
// P(6, or 5 on a TMS5100) then, unless R, K1 K2 (5) K3 K4 (4) and, if voiced,
// K5 K6 K7 (4) K8 K9 K10 (3).
//
// Performance: one lattice at 8 kHz is ~20 multiply-adds a sample, ~0.2% of a
// core. Output is interpolated to 16 kHz and handed to the master as its own
// bus, which ducks the music while it speaks.

// ── Chip tables (TI, via MAME tms5110r.hxx) ──────────────────────────────────
static const uint8_t LPC_E_5220[16] = { 0,1,2,3,4,6,8,11,16,23,33,47,63,85,114,0 };
static const uint8_t LPC_P_5220[64] = {
  0,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,44,46,48,
  50,52,53,56,58,60,62,65,68,70,72,76,78,80,84,86,91,94,98,101,105,109,114,118,122,127,132,137,142,148,153,159 };
static const int16_t LPC_K1_5220[32] = { -501,-498,-497,-495,-493,-491,-488,-482,-478,-474,-469,-464,-459,-452,-445,-437,
                                         -412,-380,-339,-288,-227,-158,-81,-1,80,157,226,287,337,379,411,436 };
static const int16_t LPC_K2_5220[32] = { -328,-303,-274,-244,-211,-175,-138,-99,-59,-18,24,64,105,143,180,215,
                                         248,278,306,331,354,374,392,408,422,435,445,455,463,470,476,506 };
static const int16_t LPC_K3_5220[16] = { -441,-387,-333,-279,-225,-171,-117,-63,-9,45,98,152,206,260,314,368 };
static const int16_t LPC_K4_5220[16] = { -328,-273,-217,-161,-106,-50,5,61,116,172,228,283,339,394,450,506 };
static const int16_t LPC_K5_5220[16] = { -328,-282,-235,-189,-142,-96,-50,-3,43,90,136,182,229,275,322,368 };
static const int16_t LPC_K6_5220[16] = { -256,-212,-168,-123,-79,-35,10,54,98,143,187,232,276,320,365,409 };
static const int16_t LPC_K7_5220[16] = { -308,-260,-212,-164,-117,-69,-21,27,75,122,170,218,266,314,361,409 };
static const int16_t LPC_K8_5220[8]  = { -256,-161,-66,29,124,219,314,409 };
static const int16_t LPC_K9_5220[8]  = { -256,-176,-96,-15,65,146,226,307 };
static const int16_t LPC_K10_5220[8] = { -205,-132,-59,14,87,160,234,307 };
static const int8_t  LPC_CHIRP_5220[52] = {                       // the "later" chirp, decapped
  0,3,15,40,76,108,113,80,37,38,76,68,26,50,59,19,55,26,37,31,29 };

// TMS5100 / TMC0281 -- the original Speak & Spell. 5-bit pitch, patent tables.
static const uint8_t LPC_E_5100[16] = { 0,0,1,1,2,3,5,7,10,15,21,30,43,61,86,0 };
static const uint8_t LPC_P_5100[32] = { 0,41,43,45,47,49,51,53,55,58,60,63,66,70,73,76,
                                        79,83,87,90,94,99,103,107,112,118,123,129,134,140,147,153 };
static const int16_t LPC_K1_5100[32] = { -501,-497,-493,-488,-480,-471,-460,-446,-427,-405,-378,-344,-305,-259,-206,-148,
                                         -86,-21,45,110,171,227,277,320,357,388,413,434,451,464,474,498 };
static const int16_t LPC_K2_5100[32] = { -349,-328,-305,-280,-252,-223,-192,-158,-124,-88,-51,-14,23,60,97,133,
                                         167,199,230,259,286,310,333,354,372,389,404,417,429,439,449,506 };
static const int16_t LPC_K3_5100[16] = { -397,-365,-327,-282,-229,-170,-104,-36,35,104,169,228,281,326,364,396 };
static const int16_t LPC_K4_5100[16] = { -369,-334,-293,-245,-191,-131,-67,-1,64,128,188,243,291,332,367,397 };
static const int16_t LPC_K5_5100[16] = { -319,-286,-250,-211,-168,-122,-74,-25,24,73,121,167,210,249,285,318 };
static const int16_t LPC_K6_5100[16] = { -290,-252,-209,-163,-114,-62,-9,44,97,147,194,238,278,313,344,371 };
static const int16_t LPC_K7_5100[16] = { -291,-256,-216,-174,-128,-80,-31,19,69,117,163,206,246,283,316,345 };
static const int16_t LPC_K8_5100[8]  = { -218,-133,-38,59,152,235,305,361 };
static const int16_t LPC_K9_5100[8]  = { -226,-157,-82,-3,76,151,220,280 };
static const int16_t LPC_K10_5100[8] = { -179,-122,-61,1,62,123,179,231 };
static const int8_t  LPC_CHIRP_5100[52] = {                       // the patent chirp (also Talkie's)
  0,42,-44,50,-78,18,37,20,2,-31,-59,2,95,90,5,15,38,-4,-91,-91,-42,-35,-36,-4,
  37,43,34,33,15,-1,-8,-18,-19,-17,-9,-10,-6,0,3,2,1 };

struct SndLpcChip {
  const uint8_t* energy; const uint8_t* pitch; uint8_t pitchBits;
  const int16_t* k[10]; const int8_t* chirp;
};
static const uint8_t LPC_KBITS[10] = { 5, 5, 4, 4, 4, 4, 4, 3, 3, 3 };
static const SndLpcChip LPC_TMS5220 = { LPC_E_5220, LPC_P_5220, 6,
  { LPC_K1_5220, LPC_K2_5220, LPC_K3_5220, LPC_K4_5220, LPC_K5_5220,
    LPC_K6_5220, LPC_K7_5220, LPC_K8_5220, LPC_K9_5220, LPC_K10_5220 }, LPC_CHIRP_5220 };
static const SndLpcChip LPC_TMS5100 = { LPC_E_5100, LPC_P_5100, 5,
  { LPC_K1_5100, LPC_K2_5100, LPC_K3_5100, LPC_K4_5100, LPC_K5_5100,
    LPC_K6_5100, LPC_K7_5100, LPC_K8_5100, LPC_K9_5100, LPC_K10_5100 }, LPC_CHIRP_5100 };

// ── One decoder ──────────────────────────────────────────────────────────────
struct SndLpc {
  const SndLpcChip* chip;
  const uint8_t* bits; uint32_t nBytes;
  uint32_t pos; uint8_t bit;             // read cursor
  uint32_t framePos; uint8_t frameBit;   // start of the current frame (for stutter)
  bool     active;
  bool     ending;                       // stop frame seen: ring out one frame, then done
  uint8_t  kIdx[10];
  float    prevE, prevP, prevK[10];
  float    tgtE,  tgtP,  tgtK[10];
  float    curE,  curP,  curK[10];
  bool     prevSilent, prevVoiced, interp;
  int      step, samp;                   // interpolation step (0..7), sample within it
  float    x[10];
  float    pc;
  uint16_t rng;
  // performance
  float    speed;                        // >1 faster. Scales samples per step.
  float    pitchMul;                     // period multiplier: 1.5 = a fifth lower
  float    lockPeriod;                   // >0: every voiced frame sings this period
  float    whisper;                      // 0..1 blend toward noise excitation
  float    stutter;                      // chance per frame of re-reading it (circuit-bent)
  uint32_t srng;
  int      stutterLeft;
  bool     dac8;
  float    stepLen, stepAcc;             // samples per step with speed applied
  float    unv;                          // unvoiced (hiss) excitation level, 1 = the chip's
  float    soft;                         // 0..0.9: one-pole smoothing of the excitation (0 = raw chirp)
  float    excLp;
  const int8_t* chirp;                   // the pitch pulse: the chip's own unless SP.chirpSel overrides it
};

static inline uint8_t sndLpcBit(SndLpc& d) {
  if (d.pos >= d.nBytes) return 0;
  uint8_t b = (d.bits[d.pos] >> d.bit) & 1;
  if (++d.bit >= 8) { d.bit = 0; d.pos++; }
  return b;
}
static inline uint8_t sndLpcRead(SndLpc& d, uint8_t n) {
  uint8_t v = 0;
  while (n--) v = (uint8_t)((v << 1) | sndLpcBit(d));
  return v;
}

static void sndLpcStart(SndLpc& d, const uint8_t* bits, uint32_t nBytes, const SndLpcChip* chip) {
  float speed = d.speed > 0 ? d.speed : 1.0f, pm = d.pitchMul > 0 ? d.pitchMul : 1.0f;
  float lock = d.lockPeriod, wh = d.whisper, st = d.stutter; bool dac = d.dac8;
  float unv = d.unv > 0 ? d.unv : 1.0f, soft = d.soft;
  uint32_t srng = d.srng ? d.srng : 0x1234567u;
  memset(&d, 0, sizeof(d));
  d.speed = speed; d.pitchMul = pm; d.lockPeriod = lock; d.whisper = wh; d.stutter = st; d.dac8 = dac;
  d.unv = unv; d.soft = soft;
  d.srng = srng;
  d.chip = chip ? chip : &LPC_TMS5220;
  d.chirp = d.chip->chirp;
  d.bits = bits; d.nBytes = nBytes; d.active = (bits && nBytes);
  d.rng = 1; d.prevSilent = true;
  d.step = 8;                            // forces a frame read on the first sample
}

// Parse the next frame into the targets. Returns false at end of data.
static bool sndLpcFrame(SndLpc& d) {
  const SndLpcChip& c = *d.chip;
  // stutter: the bent chip reads the same frame again
  if (d.stutterLeft > 0) { d.stutterLeft--; d.pos = d.framePos; d.bit = d.frameBit; }
  else if (d.stutter > 0 && sndChance(d.srng, d.stutter)) {
    d.stutterLeft = 1 + sndRandI(d.srng, 3);
    d.framePos = d.pos; d.frameBit = d.bit;
  } else { d.framePos = d.pos; d.frameBit = d.bit; }

  for (int i = 0; i < 10; i++) d.prevK[i] = d.curK[i];
  d.prevE = d.curE; d.prevP = d.curP;
  bool wasSilent = d.prevSilent, wasVoiced = d.prevVoiced;

  if (d.pos >= d.nBytes) { d.tgtE = 0; d.ending = true; d.prevSilent = true; d.interp = false; return false; }
  uint8_t e = sndLpcRead(d, 4);
  if (e == 15) { d.tgtE = 0; d.ending = true; d.prevSilent = true; d.interp = false; return false; }
  if (e == 0) {
    d.tgtE = 0; d.interp = false; d.prevSilent = true;
    return true;
  }
  uint8_t rep = sndLpcRead(d, 1);
  uint8_t pi  = sndLpcRead(d, c.pitchBits);
  bool voiced = (pi != 0);
  if (!rep) {
    for (int i = 0; i < 4; i++) d.kIdx[i] = sndLpcRead(d, LPC_KBITS[i]);
    if (voiced) for (int i = 4; i < 10; i++) d.kIdx[i] = sndLpcRead(d, LPC_KBITS[i]);
  }
  d.tgtE = c.energy[e];
  d.tgtP = c.pitch[pi];
  for (int i = 0; i < 10; i++) {
    if (!voiced && i >= 4) d.tgtK[i] = 0.0f;
    else                   d.tgtK[i] = (float)c.k[i][d.kIdx[i]] * (1.0f / 512.0f);
  }
  // The chip does not interpolate across silence or a voicing change.
  d.interp = !(wasSilent || voiced != wasVoiced);
  d.prevSilent = false; d.prevVoiced = voiced;
  return true;
}

// One 8 kHz sample.
static inline float sndLpcSample(SndLpc& d) {
  if (d.step >= 8) {                                   // frame boundary
    if (d.ending) { d.active = false; return 0.0f; }
    if (!sndLpcFrame(d) && !d.ending) { d.active = false; return 0.0f; }
    d.step = 0; d.samp = 0; d.stepAcc = 0;
    d.stepLen = 25.0f / (d.speed > 0.05f ? d.speed : 0.05f);
    if (!d.interp) { d.curE = d.tgtE; d.curP = d.tgtP; for (int i = 0; i < 10; i++) d.curK[i] = d.tgtK[i]; }
  }
  if (d.samp == 0 && d.interp) {                       // new interpolation step
    float t = (float)(d.step + 1) * 0.125f;
    d.curE = d.prevE + (d.tgtE - d.prevE) * t;
    d.curP = d.prevP + (d.tgtP - d.prevP) * t;
    for (int i = 0; i < 10; i++) d.curK[i] = d.prevK[i] + (d.tgtK[i] - d.prevK[i]) * t;
  }
  // excitation
  float exc;
  float period = d.curP;
  if (period > 0 && d.lockPeriod > 0) period = d.lockPeriod;
  else period *= d.pitchMul;
  float noise;
  d.rng = (uint16_t)((d.rng >> 1) ^ ((d.rng & 1) ? 0xB800 : 0));
  noise = (d.rng & 1) ? -64.0f : 64.0f;
  if (period > 0) {
    int ci = (int)d.pc;
    float ch = (ci < 52) ? (float)d.chirp[ci] : 0.0f;
    exc = (d.whisper > 0) ? ch + (noise * 0.7f - ch) * d.whisper : ch;
    d.pc += 1.0f; if (d.pc >= period) d.pc -= period;
  } else exc = noise * d.unv;
  // The chirp is a click every pitch period and the hiss is white: on the
  // K10's bright cone both read as static. A gentle lowpass on the source
  // keeps the robot and loses the grit (x (1 + soft) roughly restores level).
  if (d.soft > 0.0f) { d.excLp += (1.0f - d.soft) * (exc - d.excLp); exc = d.excLp * (1.0f + d.soft); }
  // lattice
  float u[11];
  // 0.125 is the chip's own scaling; the extra 0.8 keeps this (hot-encoded)
  // vocabulary off the 12-bit clamp, whose flat tops crackle on a small cone.
  u[10] = d.curE * exc * (0.125f * 0.8f);
  for (int i = 9; i >= 0; i--) u[i] = u[i + 1] - d.curK[i] * d.x[i];
  for (int i = 9; i >= 1; i--) d.x[i] = d.x[i - 1] + d.curK[i - 1] * u[i - 1];
  d.x[0] = u[0];
  float y = u[0];
  if (y > 2047.0f) y = 2047.0f; else if (y < -2048.0f) y = -2048.0f;
  // The 8-bit DAC. Truncating toward zero rather than flooring: a floor turns
  // the lattice's last faint ringing into a 0/-16 toggle through every pause,
  // a -48 dBFS whine the real chip (fixed point, decays to exactly 0) never had.
  if (d.dac8) y = (float)(int)(y * (1.0f / 16.0f)) * 16.0f;
  // advance the interpolation clock
  d.stepAcc += 1.0f;
  if (d.stepAcc >= d.stepLen) { d.stepAcc -= d.stepLen; d.samp = 0; d.step++; }
  else d.samp++;
  return y * (1.0f / 2048.0f);
}

// ── The speaking voice: a queue of phrases, styles, and the 16 kHz bus ──────
#include "snd-vocab.h"

enum SndSayStyle : uint8_t {
  SAY_NARRATOR = 0,   // the terminal's chronicler: natural prosody, radio band
  SAY_CHANT,          // the chronicler intoning on the key's tonic (dawn)
  SAY_WHISPER,        // the waste whispering the Doom's taunts
  SAY_DOOM,           // the Doom itself: a doubled monotone far below
  SAY_BARKER,         // the caravan's showman
  SAY_RADIO,          // a transmission: narrow, crushed, squelched
  SAY_STYLE_COUNT
};
static const char* const SAY_STYLE_NAME[] = { "narrator", "chant", "whisper", "doom", "barker", "radio" };
static_assert(sizeof(SAY_STYLE_NAME) / sizeof(SAY_STYLE_NAME[0]) == SAY_STYLE_COUNT, "SAY_STYLE_NAME must match SndSayStyle");
enum SndSayPrio : uint8_t { SAYP_LOW = 0, SAYP_NORMAL, SAYP_HIGH, SAYP_CRITICAL };

struct SndSayReq {
  uint8_t words[8]; uint8_t n; uint8_t style, prio;
  uint8_t gapMs10;    // silence between words, x10 ms
  uint8_t delay20;    // silence before the first word, x20 ms (lets a musical cue land first)
};

struct SndSpeech {
  SndLpc   a, b;              // b doubles a for the Doom's voice
  bool     useB;
  SndSayReq q[4]; uint8_t qn;
  SndSayReq cur; uint8_t wi; bool busy;
  int32_t  gapLeft;           // samples of silence before the next word
  uint8_t  style;
  float    prev8a, prev8b;    // 8->16 kHz interpolation
  float    lp, hpz, hpx;      // tone shaping
  float    level;
  float    hist[12];          // half-band upsampler: the last 12 samples at 8 kHz
  float    lp2, hp2z;         // radio band
  int8_t   dac8Override;      // -1 = the style decides; 0/1 forces (GET /snddbg?dac8=)
  // The pitch pulse (GET /snddbg?vchirp=): -1 the chip's own, 0 the TMS5220's
  // "later" chirp (all positive: a DC-heavy click every period), 1 the
  // TMS5100 patent chirp (bipolar, near zero-mean, the same energy).
  int8_t   chirpSel;
  float    unvMul, softMul;   // the voice's hiss level and source smoothing (GET /snddbg?vunv= &vsoft=)
  float    styleLevel[SAY_STYLE_COUNT];   // each speaking style's level (GET /snddbg?lvl_whisper= ..., the sound desk)
  float    vlpHz, vlb[5], vlp[4];   // the voice's own band limit, 2-pole (GET /snddbg?vlp=, 0 = off)
  float    rev, echo;
  float    tonicHz;           // for SAY_CHANT: set by the composer
  float    madness;           // 0..1: stutter and pitch sag
  float    wob;               // barker vibrato phase
  float    activity;          // smoothed, for ducking
  uint32_t lastEndMs;
};
static SndSpeech SP;

// 8 -> 16 kHz: a 23-tap half-band FIR (Blackman-windowed sinc). One output
// phase is the input delayed, the other is this 12-tap interpolator, so the
// images of 0-4 kHz mirrored into 4-8 kHz come out ~60 dB down. The first cut
// interpolated linearly behind two one-pole lowpasses: its images were only
// 6-15 dB down, and on the K10's bright little cone they were the fizz on
// every word.
static float LPC_HB[12];
static void sndLpcHalfbandInit() {
  float sum = 0.0f;
  for (int i = 0; i < 12; i++) {
    int k = 2 * i, j = k - 11;                            // j odd: the non-zero taps
    float h = 1.0f / (3.14159265f * (float)j) * ((((j + 1) / 2) & 1) ? 1.0f : -1.0f);
    if (j < 0) h = 1.0f / (3.14159265f * (float)(-j)) * ((((-j + 1) / 2) & 1) ? 1.0f : -1.0f);
    float w = 0.42f - 0.5f * cosf(6.2831853f * (k + 0.5f) / 23.0f) + 0.08f * cosf(12.5663706f * (k + 0.5f) / 23.0f);
    LPC_HB[i] = h * w; sum += LPC_HB[i];
  }
  for (int i = 0; i < 12; i++) LPC_HB[i] /= sum;          // unity gain at DC for the interpolated phase
}

static void sndSpeechStyle(uint8_t st) {
  SP.style = st;
  SndLpc& a = SP.a; SndLpc& b = SP.b;
  a.speed = 1.0f; a.pitchMul = 1.0f; a.lockPeriod = 0; a.whisper = 0; a.dac8 = true;
  a.stutter = SP.madness * 0.10f;
  // The chip's output is hot (the vocabulary is encoded to hit full scale), so
  // levels sit below 1 -- one per style, tuned on the sound desk -- and the
  // master's compressor does the rest.
  SP.useB = false; SP.level = SP.styleLevel[st < SAY_STYLE_COUNT ? st : 0]; SP.rev = 0.06f; SP.echo = 0.03f;
  // The chip's 8-bit DAC grit is character, but on the K10 it was heard as
  // static on every word: only the radio voice keeps it now.
  a.dac8 = (st == SAY_RADIO);
  a.unv = SP.unvMul; a.soft = SP.softMul;
  switch (st) {
    case SAY_CHANT: {
      // Sing on the tonic, in the low male range the TTS voice lives in.
      float hz = SP.tonicHz > 0 ? SP.tonicHz : 146.8f;
      while (hz > 150.0f) hz *= 0.5f; while (hz < 90.0f) hz *= 2.0f;
      a.lockPeriod = 8000.0f / hz; a.speed = 0.92f; SP.rev = 0.3f; SP.echo = 0.16f;
      break;
    }
    case SAY_WHISPER:
      a.pitchMul = 1.4f; a.speed = 0.88f; a.whisper = 0.72f;
      SP.rev = 0.5f; SP.echo = 0.32f; a.stutter = 0.03f + SP.madness * 0.12f;
      break;
    case SAY_DOOM:
      a.lockPeriod = 8000.0f / 62.0f; a.speed = 0.8f; a.whisper = 0.25f;
      if (SP.dac8Override >= 0) a.dac8 = (SP.dac8Override != 0);
      b = a; b.lockPeriod = 8000.0f / 93.0f; b.whisper = 0.5f;       // a fifth above, rasping
      SP.useB = true; SP.rev = 0.5f; SP.echo = 0.28f;                   // two decoders summed
      break;
    case SAY_BARKER:
      a.pitchMul = 0.84f; a.speed = 1.07f; SP.rev = 0.2f; SP.echo = 0.24f;
      break;
    case SAY_RADIO:
      a.speed = 1.02f; SP.rev = 0.02f; SP.echo = 0.0f;
      break;
    default: break;
  }
  if (SP.dac8Override >= 0) { a.dac8 = (SP.dac8Override != 0); b.dac8 = a.dac8; }
  b.unv = a.unv; b.soft = a.soft;
}
// The voice's band limit: telephone-ish, where a 2 W cone speaks clearly.
static void sndSpeechSetLP(float hz) {
  SP.vlpHz = hz;
  if (hz <= 0.0f) return;
  float w0 = 6.2831853f * sndClampF(hz, 800.0f, 7000.0f) / SND_SR, cw = cosf(w0), sw = sinf(w0);
  float al = sw / (2.0f * 0.7071f), a0 = 1.0f + al;
  SP.vlb[0] = (1.0f - cw) * 0.5f / a0; SP.vlb[1] = (1.0f - cw) / a0; SP.vlb[2] = SP.vlb[0];
  SP.vlb[3] = -2.0f * cw / a0;         SP.vlb[4] = (1.0f - al) / a0;
}

static bool sndSpeechBusy() { return SP.busy || SP.qn > 0; }

// Queue a phrase. A higher-priority request clears lower ones still waiting;
// a CRITICAL one also cuts off whatever is being said.
static void sndSay(const uint8_t* words, uint8_t n, uint8_t style, uint8_t prio, uint8_t gapMs10 = 9,
                   uint8_t delay20 = 0) {
  if (!n) return;
  SndSayReq r; memset(&r, 0, sizeof(r));
  r.n = n > 8 ? 8 : n; memcpy(r.words, words, r.n); r.style = style; r.prio = prio; r.gapMs10 = gapMs10;
  r.delay20 = delay20;
  SND_LOCK();
  uint8_t k = 0;
  for (uint8_t i = 0; i < SP.qn; i++) if (SP.q[i].prio >= prio) SP.q[k++] = SP.q[i];
  SP.qn = k;
  if (prio == SAYP_CRITICAL && SP.busy) { SP.busy = false; SP.a.active = SP.b.active = false; }
  if (SP.qn < 4) SP.q[SP.qn++] = r;
  else if (prio > SP.q[3].prio) SP.q[3] = r;
  SND_UNLOCK();
}
static inline void sndSay1(uint8_t w, uint8_t style, uint8_t prio) { sndSay(&w, 1, style, prio); }

static void sndSpeechNextWord() {
  uint8_t w = SP.cur.words[SP.wi];
  if (w >= VOC_COUNT) { SP.busy = false; return; }
  const SndVocabEntry& e = SND_VOCAB[w];
  sndLpcStart(SP.a, e.bits, e.bytes, &LPC_TMS5220);
  if (SP.useB) sndLpcStart(SP.b, e.bits, e.bytes, &LPC_TMS5220);
  if (SP.chirpSel >= 0) SP.a.chirp = SP.b.chirp = SP.chirpSel ? LPC_CHIRP_5100 : LPC_CHIRP_5220;
}

// Render one block of speech at 16 kHz into out[]; returns false when silent.
static bool sndSpeechBlock(float* out) {
  if (!SP.busy) {
    bool got = false;
    SND_LOCK();
    if (SP.qn) {
      // highest priority first, FIFO within a priority
      uint8_t bi = 0;
      for (uint8_t i = 1; i < SP.qn; i++) if (SP.q[i].prio > SP.q[bi].prio) bi = i;
      SP.cur = SP.q[bi];
      for (uint8_t i = bi; i + 1 < SP.qn; i++) SP.q[i] = SP.q[i + 1];
      SP.qn--; got = true;
    }
    SND_UNLOCK();
    if (got) {
      SP.busy = true; SP.wi = 0; SP.gapLeft = (int32_t)SP.cur.delay20 * 320;
      sndSpeechStyle(SP.cur.style);
      sndSpeechNextWord();
    }
  }
  bool speaking = SP.busy && SP.gapLeft <= 0;
  SP.activity += ((speaking ? 1.0f : 0.0f) - SP.activity) * (speaking ? 0.2f : 0.01f);
  if (!SP.busy) { memset(out, 0, sizeof(float) * SND_BLK); return false; }

  // The radio voice is narrower (~2.2 kHz) and thinner (~500 Hz), like a handset.
  const bool  radio = (SP.style == SAY_RADIO);
  const float lpK = 0.6f;
  const float hpK = radio ? 0.18f : 0.095f;
  for (int i = 0; i < SND_BLK; i += 2) {
    float s = 0.0f, sb = 0.0f;
    if (SP.gapLeft > 0) { SP.gapLeft -= 2; }
    else if (SP.a.active) {
      if (SP.style == SAY_BARKER) {                    // a showman's wobble
        SP.wob += 5.2f / 8000.0f; if (SP.wob >= 1.0f) SP.wob -= 1.0f;
        SP.a.pitchMul = 0.84f * (1.0f + 0.035f * sndSinP(SP.wob));
      }
      s = sndLpcSample(SP.a);
      if (SP.useB && SP.b.active) sb = sndLpcSample(SP.b);
      if (!SP.a.active) {                              // word finished
        if (++SP.wi < SP.cur.n) { SP.gapLeft = SP.cur.gapMs10 * 160; sndSpeechNextWord(); }
        else { SP.busy = false; }
      }
    }
    float x = s + sb * 0.7f;
    // 8 -> 16 kHz through the half-band FIR: interpolated sample, then the
    // input itself (delayed 5 samples to line up with the filter's centre)
    for (int k = 11; k > 0; k--) SP.hist[k] = SP.hist[k - 1];
    SP.hist[0] = x;
    float interp = 0.0f;
    for (int k = 0; k < 12; k++) interp += LPC_HB[k] * SP.hist[k];
    for (int k = 0; k < 2; k++) {
      float y = k == 0 ? interp : SP.hist[5];
      if (radio) { SP.lp += lpK * (y - SP.lp); SP.lp2 += lpK * (SP.lp - SP.lp2); y = SP.lp2; }
      // one-pole high-pass: the terminal's little speaker grille
      float hp = y - SP.hpz; SP.hpz += hpK * hp;
      if (radio) { float d = hp * 1.8f; hp = d / (1.0f + sndAbsF(d)) * 0.8f; }   // a handset's drive
      if (SP.vlpHz > 0.0f && !radio) {
        const float* c = SP.vlb; float* h = SP.vlp;
        float o2 = c[0] * hp + c[1] * h[0] + c[2] * h[1] - c[3] * h[2] - c[4] * h[3];
        h[1] = h[0]; h[0] = hp; h[3] = h[2]; h[2] = o2; hp = o2;
      }
      float o = hp * SP.level * 0.9f;
      out[i + k] = o;
      SC.rev[i + k]  += o * SP.rev;
      SC.echo[i + k] += o * SP.echo;
    }
  }
  return true;
}
