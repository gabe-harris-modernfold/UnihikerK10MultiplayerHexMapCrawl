#pragma once
// ── snd-engine.hpp ───────────────────────────────────────────────────────────
// The sound engine's front door. Everything the game says to the speaker
// goes through two calls, both safe from any task on either core:
//
//   sndStory(kind, a, b)   a beat of the story -- someone joined, dawn broke,
//                          the Doom spoke, the clock turned. The engine
//                          decides what that sounds like: an effect, a line of
//                          speech, a leitmotif woven into the music, a change
//                          of style, or all four.
//   sndSetWorld(w)         the slow state (time of day, weather, dread, who
//                          is alive) the composer writes the next bar from.
//
// sndRender() is the whole pipeline for one buffer: drain cues, run the
// composer and the effect players, render voices and speech, reverb and echo,
// master. The board calls it from its audio task (ui-audio.hpp); the desktop
// harness (scripts/sndsim) calls it in a loop and writes WAVs.

#include "snd-core.hpp"
#include "snd-lpc.hpp"
#include "snd-music.hpp"
#include "snd-sfx.hpp"

enum SndStoryKind : uint8_t {
  SS_NONE = 0,
  SS_BOOT,          //
  SS_DAWN,          // a = day number (low byte), b = day high byte
  SS_DUSK,          //
  SS_JOIN,          // a = slot
  SS_LEFT,          // a = slot
  SS_DOWNED,        // a = slot
  SS_THREAT,        // a = band 1..4
  SS_WEATHER,       // a = phase
  SS_ENC_START,     // a = slot
  SS_ENC_WIN,       // a = slot, b = 1 if the odds said it was lost
  SS_ENC_THROWN,    // a = slot
  SS_ENC_HAZARD,    // a = slot
  SS_ENC_CLEARED,   // a = slot
  SS_CARAVAN,       // a = slot
  SS_QUAKE,         //
  SS_FIRE,          // a = slot, b = 1 for a lightning strike
  SS_FLOOD,         // a = slot
  SS_DOOM_TAUNT,    // a = tier 0..3, b = line index (reduced mod 3 like the client)
  SS_DOOM_BAND,     // a = band 0..3 (the old ostinato cue: tickDoomAudio)
  SS_SCORE_UP,
  SS_SCORE_DOWN,
  SS_TUNNEL,        // a = 1 down, 0 up
  SS_REGEN,
  SS_THUNDER,       // the lamps flashed: thunder follows
  SS_SFX,           // a = SfxId: a plain effect (the k10Play compatibility path)
  SS_GEIGER,
  SS_SAY,           // a = vocab id, b = style: one word/line, raw
  SS_CALIBRATE,     // play the measurement sequence (GET /sndtest): see sndCalBlock()
  SS_STOP,          // silence everything now: effects, speech, the sequence (the sound desk)
  SS_COUNT
};
// For the sound desk (GET /sndinfo), in enum order.
static const char* const SND_STORY_NAME[] = {
  "none", "boot", "dawn", "dusk", "join", "left", "downed", "threat", "weather", "enc_start",
  "enc_win", "enc_thrown", "enc_hazard", "enc_cleared", "caravan", "quake", "fire", "flood",
  "doom_taunt", "doom_band", "score_up", "score_down", "tunnel", "regen", "thunder", "sfx",
  "geiger", "say", "calibrate", "stop" };
static_assert(sizeof(SND_STORY_NAME) / sizeof(SND_STORY_NAME[0]) == SS_COUNT, "SND_STORY_NAME must match SndStoryKind");

struct SndCue { uint8_t kind, a, b, c; };
static constexpr int SND_Q = 32;
static SndCue    sndQ[SND_Q];
static uint8_t   sndQHead = 0, sndQTail = 0;
static SndWorld  sndWorldIn;
static bool      sndWorldNew = false;
// A ToneStep sequence with no orchestrated score (k10Play fallback): the audio
// task picks it up and plays it on the music box. Written by any task.
static const void* volatile sndTonesReq = nullptr;
static bool      sndReady = false;

struct SndStats { uint32_t blocks, quietBlocks, cues, dropped; uint8_t maxVoices; };
static SndStats SST;
// The measurement sequence's state (see "Calibration" below). seq 0 = exact
// test tones straight to the DAC; seq 1 = real content through the whole mix.
struct SndCal { bool active; uint8_t seq; uint32_t n; float ph, f; uint32_t rng; float b0, b1, b2, b3, b4, b5, b6; };
static SndCal SCAL;

static void sndStory(uint8_t kind, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) {
  SND_LOCK();
  uint8_t next = (uint8_t)((sndQHead + 1) % SND_Q);
  if (next != sndQTail) { sndQ[sndQHead] = { kind, a, b, c }; sndQHead = next; }
  else SST.dropped++;
  SND_UNLOCK();
}
static void sndSetWorld(const SndWorld& w) {
  SND_LOCK(); sndWorldIn = w; sndWorldNew = true; SND_UNLOCK();
}

static bool sndBegin(uint32_t seed) {
  if (!sndTablesInit() || !sndPatchesInit()) return false;
  sndVoicesInit();
  if (!SC.v) return false;
  bool fx = sndReverbInit() & sndEchoInit();
  sndMasterInit();
  sndMusicInit(seed);
  memset(&SP, 0, sizeof(SP));
  SP.dac8Override = -1; SP.chirpSel = -1;
  // The chip's own voice: full hiss consonants, the raw chirp, no band limit.
  // Softening it on the board (vsoft 0.35, vunv 0.55, a 3.2 kHz lowpass) was
  // heard as muffled. All of it stays live on GET /snddbg.
  SP.unvMul = 1.0f; SP.softMul = 0.0f;
  // Each speaking style's level, in SndSayStyle order (the sound desk tunes
  // them). The whisper -- noise-excited, with the heaviest reverb -- was "a
  // little too loud compared to the rest of the voices" at 1.0 (2026-09-23).
  static const float LVL[SAY_STYLE_COUNT] = { 0.8f, 0.8f, 0.75f, 0.72f, 0.8f, 0.9f };
  for (int i = 0; i < SAY_STYLE_COUNT; i++) SP.styleLevel[i] = LVL[i];
  sndSpeechSetLP(0.0f);
  sndLpcHalfbandInit();
  memset(SFXP, 0, sizeof(SFXP));
  sndReady = true;
  return fx;
}

// ── Story -> sound ───────────────────────────────────────────────────────────
static uint8_t sndSlotWord(uint8_t slot) {
  static const uint8_t W[6] = { VOC_GUIDE, VOC_QUARTERMASTER, VOC_MEDIC, VOC_MULE, VOC_SCOUT, VOC_ENDURER };
  return slot < 6 ? W[slot] : VOC_SURVIVORS;
}
// "day" + the number in words (1..999)
static uint8_t sndNumberWords(uint16_t n, uint8_t* out) {
  uint8_t k = 0;
  n %= 1000;
  if (n >= 100) { out[k++] = (uint8_t)(VOC_N1 + n / 100 - 1); out[k++] = VOC_N100; n %= 100; if (!n) return k; }
  if (n >= 20) { out[k++] = (uint8_t)(VOC_N20 + (n / 10 - 2)); n %= 10; if (n) out[k++] = (uint8_t)(VOC_N1 + n - 1); }
  else if (n >= 1) out[k++] = (uint8_t)(VOC_N1 + n - 1);
  return k;
}
static inline int8_t sndKeyShift() { return (int8_t)(((SMu.tonicPc - 2) + 18) % 12 - 6); }   // in-key effects written in D
static void sndFx(uint8_t id, float delayMs = 0.0f) {
  if (id == SFX_GEIGER) { sndSfxGeiger(SC.rng); return; }
  if (id == SFX_NONE || id >= SFX_COUNT) return;
  const SfxDef& d = SFX_TABLE[id];
  sfxStart(d.s, d.n, d.inKey ? sndKeyShift() : 0, delayMs);
}
static inline bool sndMusicOn() { return SMu.haveWorld && SMu.w.musicLevel > 0 && SMu.style != MS_SILENT; }

// Some beats repeat every world tick while the danger lasts (standing in fire,
// chem rain on bare skin, a geiger counter in a crater). The first one is
// news; the rest get a cooldown, a longer one for the voice than the effect,
// so the box keeps warning without turning into a car alarm.
static uint32_t sndNowMs() { return SST.blocks * 2u; }
static uint32_t sndLastFx[SS_COUNT], sndLastSay[SS_COUNT];
static bool sndCool(uint32_t* last, uint8_t kind, uint32_t ms) {
  uint32_t now = sndNowMs();
  if (last[kind] && now - last[kind] < ms) return false;
  last[kind] = now ? now : 1;
  return true;
}

static void sndHandleCue(const SndCue& c) {
  SST.cues++;
  uint8_t words[8];
  switch (c.kind) {
    case SS_BOOT:
      sndFx(SFX_BOOT);
      { uint8_t w = VOC_WASTELAND; sndSay(&w, 1, SAY_RADIO, SAYP_NORMAL); }
      break;
    case SS_DAWN: {
      uint16_t day = (uint16_t)(c.a | (c.b << 8));
      if (sndMusicOn()) musDawn(SMu.w.aliveMask);
      SP.tonicHz = sndMidiHz((float)SMu.tonicBass);
      uint8_t k = 0; words[k++] = VOC_DAY; k += sndNumberWords(day, words + k);
      sndSay(words, k, SAY_CHANT, SAYP_HIGH, 6, 130);         // after the call has rung out
      static const uint8_t DAWN_LINE[3] = { VOC_DAWN_GREY, VOC_DAWN_THIN, VOC_DAWN_SUN };
      sndSay1(DAWN_LINE[day % 3], SAY_NARRATOR, SAYP_NORMAL);
      break;
    }
    case SS_DUSK:
      if (sndMusicOn()) musStartMotif(DUSK_FALL, 5, P_MUSICBOX, SMu.tonicLead, 0.7f);
      sndSay1((SMu.w.day & 1) ? VOC_NIGHT2 : VOC_NIGHT, SAY_NARRATOR, SAYP_LOW);
      break;
    case SS_JOIN:
      sndFx(SFX_RADIO_BLIP);
      if (sndMusicOn()) musJoin(c.a);
      words[0] = sndSlotWord(c.a); words[1] = VOC_JOINS;
      sndSay(words, 2, SAY_RADIO, SAYP_NORMAL, 4, 80);        // after their signature
      break;
    case SS_LEFT:
      if (!sndCool(sndLastSay, SS_LEFT, 8000)) break;          // a dropped Wi-Fi link is not a eulogy
      words[0] = sndSlotWord(c.a); words[1] = VOC_LEAVES;
      sndSay(words, 2, SAY_NARRATOR, SAYP_LOW, 4);
      break;
    case SS_DOWNED: {
      sndFx(SFX_DEAD_BATTERY);
      if (sndMusicOn()) musElegy(c.a);
      words[0] = sndSlotWord(c.a); words[1] = VOC_DOWN;
      sndSay(words, 2, SAY_NARRATOR, SAYP_CRITICAL, 4);
      uint8_t alive = SMu.w.aliveMask & (uint8_t)~(1u << c.a);
      if (!alive) sndSay1(VOC_NO_ONE, SAY_NARRATOR, SAYP_CRITICAL);
      break;
    }
    case SS_THREAT: {
      uint8_t band = c.a < 1 ? 1 : (c.a > 4 ? 4 : c.a);
      sndSfxToll(band, sndKeyShift());
      if (band == 4) sndFx(SFX_BUNKER_ALARM, 900.0f * band);
      static const uint8_t TC[4] = { VOC_TC1, VOC_TC2, VOC_TC3, VOC_TC4 };
      sndSay1(TC[band - 1], SAY_NARRATOR, SAYP_HIGH);
      if (band == 4) sndSay1(VOC_TOO_LATE, SAY_DOOM, SAYP_HIGH);
      break;
    }
    case SS_WEATHER: {
      uint8_t ph = c.a < 6 ? c.a : 0;
      if (ph == 2) sndFx(SFX_MUTANT_BREATH); else if (ph == 3) sndFx(SFX_ACID_DRIP); else sndFx(SFX_DISTANT_THUD);
      static const uint8_t WX[6] = { VOC_WX0, VOC_WX1, VOC_WX2, VOC_WX3, VOC_WX4, VOC_WX5 };
      sndSay1(WX[ph], SAY_NARRATOR, SAYP_NORMAL);
      break;
    }
    case SS_ENC_START:
      sndFx(SFX_DARK_ENTRY);
      if (sndCool(sndLastSay, SS_ENC_START, 20000)) {
        words[0] = sndSlotWord(c.a); words[1] = VOC_DARK;
        sndSay(words, 2, SAY_NARRATOR, SAYP_LOW, 4);
      }
      break;
    case SS_ENC_WIN:
      sndFx(SFX_DARK_DEPART);
      if (c.b) sndSay1(VOC_CORRECT, SAY_RADIO, SAYP_NORMAL);
      break;
    case SS_ENC_THROWN:
      sndFx(SFX_BROKEN_TECH);
      sndSay1(VOC_INCORRECT, SAY_RADIO, SAYP_NORMAL);
      break;
    case SS_ENC_HAZARD:  sndFx(SFX_SYSTEM_FAULT); break;
    case SS_ENC_CLEARED:
      sndFx(SFX_WEIRD_ANOMALY);
      sndSay1(VOC_CLEARED, SAY_NARRATOR, SAYP_NORMAL);
      break;
    case SS_CARAVAN:
      sndFx(SFX_CALLIOPE_CALL);
      words[0] = VOC_STEP_UP; words[1] = VOC_BARGAIN;
      sndSay(words, 2, SAY_BARKER, SAYP_NORMAL, 25);
      break;
    case SS_QUAKE:
      sndFx(SFX_QUAKE);
      sndSay1(VOC_QUAKE, SAY_NARRATOR, SAYP_NORMAL);
      break;
    case SS_FIRE:
      if (c.b) sndFx(SFX_THUNDER);                               // a strike is always news
      else if (sndCool(sndLastFx, SS_FIRE, 4000)) sndFx(SFX_FIRE_BURST);
      if (sndCool(sndLastSay, SS_FIRE, 25000)) sndSay1(VOC_FIRE, SAY_NARRATOR, SAYP_LOW);
      break;
    case SS_FLOOD:
      if (sndCool(sndLastFx, SS_FLOOD, 3000)) sndFx(SFX_FLOOD);
      if (sndCool(sndLastSay, SS_FLOOD, 25000)) sndSay1(VOC_FLOOD, SAY_NARRATOR, SAYP_LOW);
      break;
    case SS_DOOM_TAUNT: {
      uint8_t tier = c.a > 3 ? 3 : c.a;
      uint8_t line = (uint8_t)(VOC_DOOM00 + tier * 3 + (c.b % 3));
      sndSay1(line, SAY_WHISPER, SAYP_HIGH);
      if (tier == 3) {
        static const uint8_t HUNT[4] = { VOC_I_SEE_YOU, VOC_RUN, VOC_MINE, VOC_COME_HERE };
        sndSay1(HUNT[c.b & 3], SAY_DOOM, SAYP_HIGH);
      }
      if (sndMusicOn() && tier > 0) musDoomPulse();
      break;
    }
    case SS_DOOM_BAND:
      if (sndMusicOn()) { if (c.a > 0) musDoomPulse(); }
      else { static const uint8_t D[4] = { SFX_DOOM_LOST, SFX_DOOM_FAR, SFX_DOOM_NEAR, SFX_DOOM_HUNT }; sndFx(D[c.a & 3]); }
      break;
    case SS_SCORE_UP:   sndFx(SFX_SCORE_UP); break;
    case SS_SCORE_DOWN: sndFx(SFX_ROTTEN_CHORD); break;
    case SS_TUNNEL:     if (sndCool(sndLastFx, SS_TUNNEL, 2500)) sndFx(SFX_SEWER_ECHO); break;
    case SS_REGEN:
      sndFx(SFX_POWER_DOWN);
      musWindDown(0.05f, 1.2f, 2.0f);
      break;
    case SS_THUNDER: {
      if (SMu.w.allUnder) break;
      float delay = 250.0f + 1400.0f * sndRandF(SC.rng);        // the flash, then the count
      sndFx(SFX_THUNDER, delay);
      break;
    }
    case SS_SFX: {
      // the per-tick damage motifs (sludge, acid, rust) share one short cooldown
      bool tick = (c.a == SFX_SLUDGE || c.a == SFX_ACID_DRIP || c.a == SFX_CREEPING_RUST);
      if (!tick || sndCool(sndLastFx, SS_SFX, 1500)) sndFx(c.a);
      break;
    }
    case SS_GEIGER: if (sndCool(sndLastFx, SS_GEIGER, 1200)) sndFx(SFX_GEIGER); break;
    case SS_SAY:    sndSay1(c.a, c.b, SAYP_NORMAL); break;
    case SS_CALIBRATE:
      memset(&SCAL, 0, sizeof(SCAL)); SCAL.active = true; SCAL.rng = 0x1234567u; SCAL.seq = c.a;
      if (SCAL.seq >= 1) { sndReleaseBus(SB_MUSIC, 0.05f); SP.qn = 0; }
      break;
    case SS_STOP:                                                // the composer carries on at its next note
      SCAL.active = false;
      for (int i = 0; i < SFX_PLAYERS; i++) SFXP[i].active = false;
      SP.qn = 0; SP.busy = false; SP.a.active = SP.b.active = false;
      sndReleaseBus(SB_SFX, 0.05f); sndReleaseBus(SB_MUSIC, 0.05f);
      break;
    default: break;
  }
}

// ── Calibration ──────────────────────────────────────────────────────────────
// A fixed measurement sequence for a microphone in front of the board
// (scripts/sndsim/k10measure.py records and analyses it). It bypasses the mix
// and the limiter, so each segment is an exact level -- but it is always
// scaled by the owner's volume setting. The first version skipped the volume
// too and played up to -10 dBFS: on the K10's hot amp that was painfully loud.
// Nothing may ever bypass the volume again.
//   0.0 silence | 1.0 sync 1 kHz -30 | 2.0 log sweep 100 Hz..7.5 kHz -28 (8 s)
//   10.5 ladder 1 kHz -46..-10 step 6 (0.6 s on, 0.2 s off)
//   16.3 ladder 300 Hz -40..-10 step 6 | 21.5 pink noise -30 RMS (3 s)
//   24.5 sync 1 kHz -30 | 25.0 end
static constexpr float SND_PINK_K = 0.0187f;   // scales Kellet's pink to -30 dBFS RMS (checked in sndsim "cal")
static float sndCalTone(float hz, float db, float t, float len) {
  float a = sndDb(db), fade = 0.01f;
  float env = t < fade ? t / fade : (t > len - fade ? (len - t) / fade : 1.0f);
  SCAL.ph += hz * SND_INV_SR; if (SCAL.ph >= 1.0f) SCAL.ph -= 1.0f;
  return a * sndClampF(env, 0.0f, 1.0f) * sndSinP(SCAL.ph);
}
static float sndCalSample(float t) {
  if (t < 1.0f) return 0.0f;
  if (t < 1.5f) return sndCalTone(1000.0f, -30.0f, t - 1.0f, 0.5f);
  if (t < 2.0f) { SCAL.f = 100.0f; return 0.0f; }
  if (t < 10.0f) {                                       // exponential sweep, 8 s
    static const float R = powf(7500.0f / 100.0f, 1.0f / (8.0f * SND_SR));
    float a = sndDb(-28.0f), tt = t - 2.0f;
    float env = tt < 0.02f ? tt / 0.02f : (tt > 7.98f ? (8.0f - tt) / 0.02f : 1.0f);
    SCAL.ph += SCAL.f * SND_INV_SR; if (SCAL.ph >= 1.0f) SCAL.ph -= 1.0f;
    SCAL.f *= R;
    return a * env * sndSinP(SCAL.ph);
  }
  if (t < 10.5f) return 0.0f;
  if (t < 16.1f) {                                       // 1 kHz ladder: -46 -40 -34 -28 -22 -16 -10
    float u = t - 10.5f; int k = (int)(u / 0.8f); float v = u - k * 0.8f;
    return v < 0.6f ? sndCalTone(1000.0f, -46.0f + 6.0f * k, v, 0.6f) : 0.0f;
  }
  if (t < 16.3f) return 0.0f;
  if (t < 21.1f) {                                       // 300 Hz ladder: -40 -34 -28 -22 -16 -10
    float u = t - 16.3f; int k = (int)(u / 0.8f); float v = u - k * 0.8f;
    return v < 0.6f ? sndCalTone(300.0f, -40.0f + 6.0f * k, v, 0.6f) : 0.0f;
  }
  if (t < 21.5f) return 0.0f;
  if (t < 24.5f) {                                       // pink noise (Kellet's filter), ~-30 dBFS RMS
    SCAL.rng ^= SCAL.rng << 13; SCAL.rng ^= SCAL.rng >> 17; SCAL.rng ^= SCAL.rng << 5;
    float w = (float)(int32_t)SCAL.rng * (1.0f / 2147483648.0f);
    SCAL.b0 = 0.99886f * SCAL.b0 + w * 0.0555179f; SCAL.b1 = 0.99332f * SCAL.b1 + w * 0.0750759f;
    SCAL.b2 = 0.96900f * SCAL.b2 + w * 0.1538520f; SCAL.b3 = 0.86650f * SCAL.b3 + w * 0.3104856f;
    SCAL.b4 = 0.55000f * SCAL.b4 + w * 0.5329522f; SCAL.b5 = -0.7616f * SCAL.b5 - w * 0.0168980f;
    float p = SCAL.b0 + SCAL.b1 + SCAL.b2 + SCAL.b3 + SCAL.b4 + SCAL.b5 + SCAL.b6 + w * 0.5362f;
    SCAL.b6 = w * 0.115926f;
    float tt = t - 21.5f, env = tt < 0.05f ? tt / 0.05f : (tt > 2.95f ? (3.0f - tt) / 0.05f : 1.0f);
    return p * SND_PINK_K * env;
  }
  if (t < 25.0f) return sndCalTone(1000.0f, -30.0f, t - 24.5f, 0.5f);
  SCAL.active = false;
  return 0.0f;
}
// seq 1: the instruments and voices the first listen heard fizz on, one at a
// time, through the whole mix at the current volume, with the music paused.
//   0.5 narrator | 4.0 whisper | 8.5 calliope call | 10 calliope A4 held 1.5 s
//   12 music box D6 | 14 bell D4 | 17 harmonium D-minor chord 2 s | 20 end
// seq 2: bare sines through the whole engine (voice, mix, master, limiter,
// volume), then the narrator: tells "the engine adds static" apart from "the
// content is noisy".   0.5 A4 440 | 2.5 1 kHz | 4.5 2 kHz | 6.5 narrator | 10.5 end
static void sndCalContent() {
  const uint32_t n = SCAL.n, B = SND_BLK;
  auto at = [n, B](float s) { uint32_t k = (uint32_t)(s * SND_SR); return k >= n && k < n + B; };
  SP.madness = 0.0f;
  if (SCAL.seq == 2) {
    const int32_t len = (int32_t)(1.5f * SND_SR);
    if (at(0.5f)) sndNoteOn(SB_SFX, 214, P_TESTSINE, 69.0f, 0.9f, len);
    if (at(2.5f)) sndNoteOn(SB_SFX, 214, P_TESTSINE, 83.2131f, 0.9f, len);
    if (at(4.5f)) sndNoteOn(SB_SFX, 214, P_TESTSINE, 95.2131f, 0.9f, len);
    if (at(6.5f)) sndSay1(VOC_WASTELAND, SAY_NARRATOR, SAYP_HIGH);
    SCAL.n += B;
    if (SCAL.n >= (uint32_t)(10.5f * SND_SR)) SCAL.active = false;
    return;
  }
  if (at(0.5f))  sndSay1(VOC_WASTELAND, SAY_NARRATOR, SAYP_HIGH);
  if (at(4.0f))  sndSay1(VOC_DOOM10, SAY_WHISPER, SAYP_HIGH);
  if (at(8.5f))  sndFx(SFX_CALLIOPE_CALL);
  if (at(10.0f)) sndNoteOn(SB_SFX, 210, P_CALLIOPE, 69, 0.9f, (int32_t)(1.5f * SND_SR));
  if (at(12.0f)) sndNoteOn(SB_SFX, 210, P_MUSICBOX, 86, 0.9f, (int32_t)(1.2f * SND_SR));
  if (at(14.0f)) sndNoteOn(SB_SFX, 210, P_BELL, 62, 0.9f, (int32_t)(2.5f * SND_SR));
  if (at(17.0f)) { sndNoteOn(SB_SFX, 211, P_PAD, 62, 0.8f, (int32_t)(2.0f * SND_SR));
                   sndNoteOn(SB_SFX, 212, P_PAD, 65, 0.8f, (int32_t)(2.0f * SND_SR));
                   sndNoteOn(SB_SFX, 213, P_PAD, 69, 0.8f, (int32_t)(2.0f * SND_SR)); }
  SCAL.n += B;
  if (SCAL.n >= (uint32_t)(21.0f * SND_SR)) SCAL.active = false;
}

static void sndCalBlock(int16_t* out) {
  for (int i = 0; i < SND_BLK; i++) {
    float x = SCAL.active ? sndCalSample((float)SCAL.n / SND_SR) : 0.0f;
    SCAL.n++;
    // SM.vol is 1 on the board: the owner's volume is applied downstream, to
    // everything that reaches the speaker (ui-audio.hpp sndOutput).
    float f = x * SM.vol * 32767.0f;
    int32_t s = (int32_t)(f + (f >= 0.0f ? 0.5f : -0.5f));
    out[2 * i] = out[2 * i + 1] = (int16_t)s;
  }
}

// ── Render ───────────────────────────────────────────────────────────────────
static float sndSpeechBuf[SND_BLK];
static float sndMusMix[SND_BLK];
static uint32_t sndTail = 0;           // blocks of effect tail left to render after going quiet

static void sndRenderBlock(int16_t* out) {
  // world + cues
  SND_LOCK();
  bool nw = sndWorldNew; SndWorld w = sndWorldIn; sndWorldNew = false;
  SndCue cq[8]; int nc = 0;
  while (sndQTail != sndQHead && nc < 8) { cq[nc++] = sndQ[sndQTail]; sndQTail = (uint8_t)((sndQTail + 1) % SND_Q); }
  SND_UNLOCK();
  if (nw) {
    SMu.w = w; SMu.haveWorld = true;
    SMu.tensionTgt = SMu.forceTension >= 0.0f ? SMu.forceTension : musTension(w);
    float lvl = w.musicLevel >= 9 ? 1.0f : w.musicLevel / 9.0f;
    SMu.level = lvl;
    // Music is a bed under the voice and the effects; how far under is
    // SM.musGain, which the sound desk (data/sound.html) tunes live.
    SC.busGain[SB_MUSIC] = lvl > 0 ? SM.musGain * sndDb(-14.0f * (1.0f - lvl)) : 0.0f;
    SP.madness = SMu.tension;
    // fog closes the world in; strangle fog more than mist
    float cut = STYLE[SMu.style].worldCut;
    if (w.weather == 4) cut = sndMinF(cut, 1500.0f); else if (w.weather == 5) cut = sndMinF(cut, 3000.0f);
    SM.worldCutTgt = cut;
  }
  for (int i = 0; i < nc; i++) sndHandleCue(cq[i]);
  if (sndTonesReq) { const ToneStepLike* t = (const ToneStepLike*)sndTonesReq; sndTonesReq = nullptr; sndSfxFromTones(t); }
  if (SCAL.active && SCAL.seq == 0) { SST.blocks++; sndCalBlock(out); return; }   // the tones own the DAC
  const bool calContent = SCAL.active && SCAL.seq >= 1;
  if (calContent) sndCalContent();

  memset(SC.dry, 0, sizeof(SC.dry));
  memset(SC.rev, 0, sizeof(SC.rev));
  memset(SC.echo, 0, sizeof(SC.echo));

  if (!calContent) sndMusicBlock();                 // the content test pauses the score
  sndSfxBlock();
  int active = 0, sfxActive = 0;
  for (int i = 0; i < SND_VOICES; i++) {
    if (!SC.v[i].p) continue;
    sndVoiceBlock(SC.v[i]);
    if (SC.v[i].p) { active++; if (i >= SND_MUS_END) sfxActive++; }
  }
  if (active > SST.maxVoices) SST.maxVoices = (uint8_t)active;
  bool talking = sndSpeechBlock(sndSpeechBuf);

  // quiet fast path: nothing sounding and the tails have died away
  if (active || talking) sndTail = (uint32_t)(4.0f * SND_KRATE);
  else if (sndTail > 0) sndTail--;
  SST.blocks++;
  if (!active && !talking && sndTail == 0) {
    SST.quietBlocks++;
    memset(out, 0, sizeof(int16_t) * 2 * SND_BLK);
    return;
  }
  // effects return into the music path, so fog muffles the reverb too
  memcpy(sndMusMix, SC.dry[SB_MUSIC], sizeof(sndMusMix));
  sndReverbBlock(SC.rev, sndMusMix, SC.fxMul);
  sndEchoBlock(SC.echo, sndMusMix, SC.fxMul);
  // speech and effects push the music down
  SM.duckTgt = 1.0f - SM.duckAmt * SP.activity - (sfxActive ? 0.18f : 0.0f);
  sndMasterBlock(sndMusMix, SC.dry[SB_SFX], talking ? sndSpeechBuf : nullptr, out);
}

// frames must be a multiple of SND_BLK. out: interleaved stereo int16.
static void sndRender(int16_t* out, int frames) {
  for (int f = 0; f + SND_BLK <= frames; f += SND_BLK) sndRenderBlock(out + 2 * f);
}
