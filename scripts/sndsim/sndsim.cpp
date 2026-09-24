// sndsim.cpp -- render the K10 sound engine to WAV on the desktop.
//
// Compiles the real snd-engine.hpp (and everything it includes) unchanged
// under SND_NATIVE, feeds it scripted scenes -- a world snapshot every 100 ms
// the way ui-audio.hpp publishes one on the board, plus story cues at set
// times -- and writes 16 kHz mono WAVs. scripts/sndsim/sndsim.py builds this
// with MSVC, runs it, and post-processes (speaker simulation, spectrograms).
//
//   sndsim.exe <outdir> <scene> [seed]
//   sndsim.exe --list

#define SND_NATIVE 1
#include "../../snd-engine.hpp"

#include <chrono>
#include <string>
#include <vector>

// ── WAV ──────────────────────────────────────────────────────────────────────
static void writeWav(const std::string& path, const std::vector<int16_t>& pcm) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); exit(2); }
  uint32_t dataLen = (uint32_t)(pcm.size() * 2), riff = 36 + dataLen, sr = SND_SR, br = SND_SR * 2, fmtLen = 16;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
  fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataLen, 4, 1, f);
  fwrite(pcm.data(), 2, pcm.size(), f);
  fclose(f);
}

// ── Scenes ───────────────────────────────────────────────────────────────────
// A scene function runs every 100 ms of simulated time. at() is true on the
// one tick that crosses `when`.
struct Ctx { float t, dt; SndWorld w; };
static bool at(const Ctx& c, float when) { return when > c.t - c.dt && when <= c.t; }
static SndWorld baseWorld() {
  SndWorld w; memset(&w, 0, sizeof(w));
  w.slotMask = 0x0F; w.aliveMask = 0x0F; w.connected = 4;
  w.dayFrac = 0.30f; w.day = 3; w.weather = 0;
  w.tcLevel = 1; w.tcWeight = 60; w.musicLevel = 7;
  return w;
}
static float ramp(float t, float t0, float t1) { return t <= t0 ? 0.0f : t >= t1 ? 1.0f : (t - t0) / (t1 - t0); }

static void scDay(Ctx& c) {
  if (at(c, 3.0f)) sndStory(SS_JOIN, 4);
  if (at(c, 4.0f)) c.w.slotMask |= 0x10, c.w.aliveMask |= 0x10, c.w.connected = 5;
  if (at(c, 50.0f)) sndStory(SS_SCORE_UP);
  c.w.dayFrac = 0.25f + c.t * 0.001f;
}
static void scNight(Ctx& c) { c.w.dayFrac = 0.86f; c.w.day = 4; }            // day 4: a nocturne night
static void scCarnival(Ctx& c) {
  c.w.caravanNear = 230;
  if (at(c, 1.0f)) sndStory(SS_CARAVAN, 1);
}
static void scCarnivalMad(Ctx& c) {
  c.w.caravanNear = 230; c.w.tcLevel = 4; c.w.tcWeight = 255; c.w.attrition = 160; c.w.hunger = 120;
}
static void scNightCarnival(Ctx& c) { c.w.dayFrac = 0.86f; c.w.day = 2; }   // day 2 hashes to a carnival night
static void scStorm(Ctx& c) {
  if (at(c, 0.5f)) sndStory(SS_WEATHER, 2);
  if (c.t > 0.5f) c.w.weather = 2;
  if (at(c, 12.0f) || at(c, 27.0f) || at(c, 41.0f)) sndStory(SS_THUNDER);
  c.w.tcWeight = 110;
}
static void scChem(Ctx& c) {
  if (at(c, 0.5f)) sndStory(SS_WEATHER, 3);
  if (c.t > 0.5f) c.w.weather = 3;
  c.w.radLoad = (uint8_t)(120 * ramp(c.t, 5, 30));
}
static void scFog(Ctx& c) {
  if (at(c, 0.5f)) sndStory(SS_WEATHER, 4);
  if (c.t > 0.5f) c.w.weather = 4;
}
static void scRain(Ctx& c) {
  if (at(c, 0.5f)) sndStory(SS_WEATHER, 1);
  if (c.t > 0.5f) c.w.weather = 1;
}
static void scHunted(Ctx& c) {
  float k = ramp(c.t, 2, 50);
  c.w.doomAware = (uint8_t)(51 + 49 * k);
  c.w.doomClose = (uint8_t)(c.t < 62 ? 255 * k : 0);
  if (at(c, 4.0f))  sndStory(SS_DOOM_TAUNT, 1, 0);
  if (at(c, 20.0f)) sndStory(SS_DOOM_TAUNT, 2, 1);
  if (at(c, 38.0f)) sndStory(SS_DOOM_TAUNT, 3, 0);
  if (at(c, 52.0f)) sndStory(SS_DOOM_TAUNT, 3, 1);
  if (at(c, 63.0f)) sndStory(SS_DOOM_TAUNT, 0, 2);
  if (at(c, 63.0f)) c.w.doomAware = 40;
}
static void scTunnels(Ctx& c) {
  if (at(c, 0.5f)) sndStory(SS_TUNNEL, 1);
  c.w.allUnder = true; c.w.under = 4;
}
static void scEncounter(Ctx& c) {
  if (at(c, 1.0f)) sndStory(SS_ENC_START, 2);
  c.w.encActive = (c.t > 1.0f && c.t < 26.0f);
  if (at(c, 14.0f)) sndStory(SS_ENC_HAZARD, 2);
  if (at(c, 26.0f)) sndStory(SS_ENC_WIN, 2, 1);
  if (at(c, 32.0f)) sndStory(SS_ENC_CLEARED, 2);
}
static void scDawn(Ctx& c) {
  c.w.slotMask = 0x3F; c.w.aliveMask = 0x3B; c.w.connected = 6;             // the Mule is gone: a hole in the roll call
  if (at(c, 1.0f)) { c.w.dayFrac = 0.0f; sndStory(SS_DAWN, 7, 0); }
  c.w.dayFrac = c.t < 1.0f ? 0.99f : 0.0f + c.t * 0.0005f;
  c.w.day = c.t < 1.0f ? 6 : 7;
}
static void scDeath(Ctx& c) {
  if (at(c, 12.0f)) sndStory(SS_DOWNED, 2);
  if (c.t > 12.0f) c.w.aliveMask = 0x0B;
  c.w.attrition = 200; c.w.woundLoad = 180;
}
static void scThreat(Ctx& c) {
  for (int b = 1; b <= 4; b++) if (at(c, 2.0f + (b - 1) * 14.0f)) { sndStory(SS_THREAT, (uint8_t)b); c.w.tcLevel = (uint8_t)b; c.w.tcWeight = (uint8_t)(b * 63); }
}
static void scSfx(Ctx& c) {
  for (int i = 1; i < SFX_COUNT; i++) if (at(c, 1.0f + (i - 1) * 3.0f)) sndStory(SS_SFX, (uint8_t)i);
  c.w.musicLevel = 0;
}
static void scSpeech(Ctx& c) {
  // one line in each voice, then the numbers
  if (at(c, 0.5f))  sndStory(SS_SAY, VOC_WASTELAND, SAY_RADIO);
  if (at(c, 4.0f))  sndStory(SS_SAY, VOC_TC2, SAY_NARRATOR);
  if (at(c, 9.0f))  sndStory(SS_SAY, VOC_DOOM10, SAY_WHISPER);
  if (at(c, 14.0f)) sndStory(SS_SAY, VOC_I_SEE_YOU, SAY_DOOM);
  if (at(c, 18.0f)) sndStory(SS_SAY, VOC_STEP_UP, SAY_BARKER);
  if (at(c, 22.0f)) sndStory(SS_DAWN, 47, 0);
  if (at(c, 30.0f)) sndStory(SS_SAY, VOC_CORRECT, SAY_RADIO);
  c.w.musicLevel = 0;
}
static void scIdle(Ctx& c) { c.w.slotMask = 0; c.w.aliveMask = 0; c.w.connected = 0; }
// The measurement sequence GET /sndtest plays on the board: the exact digital
// reference that scripts/sndsim/k10measure.py compares a mic recording against.
static void scCal(Ctx& c) { if (at(c, 0.1f)) sndStory(SS_CALIBRATE); c.w.musicLevel = 0; }
// A whole evening, compressed: the showcase.
static void scStory(Ctx& c) {
  float t = c.t;
  c.w.slotMask = 0; c.w.aliveMask = 0; c.w.connected = 0;
  c.w.caravanNear = 0; c.w.weather = 0;
  if (at(c, 0.3f)) sndStory(SS_BOOT);
  static const float JOIN_T[4] = { 8, 12, 15, 19 };
  static const uint8_t JOIN_S[4] = { 0, 2, 3, 4 };
  uint8_t mask = 0;
  for (int i = 0; i < 4; i++) { if (at(c, JOIN_T[i])) sndStory(SS_JOIN, JOIN_S[i]); if (t > JOIN_T[i]) mask |= (uint8_t)(1u << JOIN_S[i]); }
  c.w.slotMask = mask; c.w.aliveMask = mask; c.w.connected = 0;
  for (int i = 0; i < 6; i++) if (mask & (1u << i)) c.w.connected++;
  c.w.dayFrac = 0.2f + t * 0.0022f;                 // ~4.5 minutes to nightfall
  if (t > 60 && t < 100) c.w.weather = 1;           // rain
  if (at(c, 60.0f)) sndStory(SS_WEATHER, 1);
  if (at(c, 100.0f)) sndStory(SS_WEATHER, 0);
  if (t > 118 && t < 170) c.w.caravanNear = 220;    // the caravan passes
  if (at(c, 120.0f)) sndStory(SS_CARAVAN, 2);
  if (at(c, 175.0f)) sndStory(SS_THREAT, 2);
  c.w.tcLevel = t > 175 ? 2 : 1; c.w.tcWeight = (uint8_t)(t > 175 ? 140 : 70);
  if (at(c, 205.0f)) sndStory(SS_ENC_START, 3);
  c.w.encActive = (t > 205 && t < 225);
  if (at(c, 225.0f)) sndStory(SS_ENC_THROWN, 3);
  // dusk ~ t=264
  if (at(c, 264.0f)) sndStory(SS_DUSK);
  // the Doom finds them
  float k = ramp(t, 285, 330);
  c.w.doomAware = t > 285 ? (uint8_t)(51 + 49 * k) : 0;
  c.w.doomClose = (t > 285 && t < 350) ? (uint8_t)(255 * k) : 0;
  if (at(c, 290.0f)) sndStory(SS_DOOM_TAUNT, 1, 1);
  if (at(c, 310.0f)) sndStory(SS_DOOM_TAUNT, 2, 0);
  if (at(c, 330.0f)) sndStory(SS_DOOM_TAUNT, 3, 2);
  if (at(c, 342.0f)) sndStory(SS_DOWNED, 3);
  if (t > 342) c.w.aliveMask &= (uint8_t)~0x08;
  if (at(c, 352.0f)) sndStory(SS_DOOM_TAUNT, 0, 0);
  if (at(c, 380.0f)) { sndStory(SS_DAWN, 2, 0); }
  if (t > 380) c.w.dayFrac = (t - 380) * 0.002f;
  c.w.day = t > 380 ? 2 : 1;
}

struct NoteLog { float t; uint8_t bus, part, patch; float midi, vel, dur; };
static std::vector<NoteLog> gNotes;
static float gNow = 0.0f;
static void noteHook(uint8_t bus, uint8_t part, uint8_t patch, float midi, float vel, int32_t gate) {
  gNotes.push_back({ gNow, bus, part, patch, midi, vel, gate < 0 ? -1.0f : (float)gate / SND_SR });
}

struct Scene { const char* name; float dur; void (*fn)(Ctx&); const char* what; };
static const Scene SCENES[] = {
  { "day",           75, scDay,           "ashfall: drone, harmonium, theremin/whistle; the Scout joins" },
  { "night",         60, scNight,         "nocturne: music-box lullaby under a choir" },
  { "carnival",      75, scCarnival,      "the caravan: dark-carnival waltz and the barker's tune" },
  { "carnival_mad",  50, scCarnivalMad,   "the carnival at threat 4 and starving: faster, warped, Hungarian minor" },
  { "night_carnival",60, scNightCarnival, "a carnival night (one night in three)" },
  { "storm",         55, scStorm,         "storm: Phrygian ostinato, tremolo, timpani, thunder" },
  { "chem",          45, scChem,          "chem rain: whole tones, glass, geiger" },
  { "fog",           40, scFog,           "strangle fog: the day, muffled" },
  { "rain",          40, scRain,          "rain: the day with drips in the key" },
  { "hunted",        75, scHunted,        "the Doom closes in, taunts, hunts, loses the trail" },
  { "tunnels",       50, scTunnels,       "underground: a fourth down, a long cave, water" },
  { "encounter",     40, scEncounter,     "an encounter: held breath, a hazard, a long-odds win, picked clean" },
  { "dawn",          40, scDawn,          "dawn of day 7: the call, the roll call with the Mule's note missing, the chant" },
  { "death",         45, scDeath,         "the Medic goes down: the flatline, the elegy on her signature" },
  { "threat",        60, scThreat,        "the threat clock turns four times" },
  { "sfx",           94, scSfx,           "every effect in turn, music off" },
  { "speech",        36, scSpeech,        "the voices: radio, narrator, whisper, Doom, barker, chant" },
  { "idle",          90, scIdle,          "nobody on the road: a carnival over the hill" },
  { "story",        420, scStory,         "a whole evening compressed: boot to the second dawn" },
  { "cal",           26, scCal,           "the /sndtest measurement sequence (reference for k10measure.py)" },
};

// Decode every vocabulary entry with the firmware's LPC decoder, plain (no
// style), at the chip's 8 kHz: lpc_<index>.wav. scripts/lpc's reference decoder
// writes out/decoded/<ID>.wav from the same bits; compare the two.
static void writeWav8k(const std::string& path, const std::vector<int16_t>& pcm) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  uint32_t dataLen = (uint32_t)(pcm.size() * 2), riff = 36 + dataLen, sr = 8000, br = 16000, fmtLen = 16;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
  fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataLen, 4, 1, f);
  fwrite(pcm.data(), 2, pcm.size(), f); fclose(f);
}
static int lpcDump(const std::string& outdir) {
  for (int i = 0; i < VOC_COUNT; i++) {
    SndLpc d; memset(&d, 0, sizeof(d));
    d.speed = 1.0f; d.pitchMul = 1.0f; d.dac8 = true;
    sndLpcStart(d, SND_VOCAB[i].bits, SND_VOCAB[i].bytes, &LPC_TMS5220);
    std::vector<int16_t> pcm;
    while (d.active && pcm.size() < 8000 * 20) pcm.push_back((int16_t)(sndLpcSample(d) * 32767.0f));
    writeWav8k(outdir + "/lpc_" + std::to_string(i) + ".wav", pcm);
  }
  printf("%d%c", (int)VOC_COUNT, 10);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--lpcdump") return lpcDump(argv[2]);
  if (argc >= 2 && std::string(argv[1]) == "--list") {
    for (const Scene& s : SCENES) printf("%-15s %4.0fs  %s\n", s.name, s.dur, s.what);
    return 0;
  }
  if (argc < 3) { fprintf(stderr, "usage: sndsim <outdir> <scene> [seed]\n"); return 1; }
  std::string outdir = argv[1], name = argv[2];
  uint32_t seed = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 12345u;
  // optional 4th arg: part mask (hex), for stems. bit = MusPart: 0 drone 1 pad 2 bass
  // 3 lead 4 counter 5 perc 6 tex 7 motif
  if (argc > 4) sndPartMask = (uint32_t)strtoul(argv[4], nullptr, 16);
  std::string suffix = argc > 5 ? argv[5] : "";
  const Scene* sc = nullptr;
  for (const Scene& s : SCENES) if (name == s.name) sc = &s;
  if (!sc) { fprintf(stderr, "unknown scene %s\n", name.c_str()); return 1; }

  if (!sndBegin(seed)) { fprintf(stderr, "sndBegin failed\n"); return 3; }
  sndNoteHook = noteHook;
  SM.volTgt = SM.vol = 1.0f;
  Ctx c; c.t = 0; c.dt = 0.1f; c.w = baseWorld();
  std::vector<int16_t> pcm;
  const int total = (int)(sc->dur * SND_SR);
  pcm.reserve(total);
  int16_t buf[2 * SND_BLK];
  float nextTick = 0.0f;
  double renderSec = 0.0;
  uint64_t voiceBlocks = 0, blocks = 0;
  int maxV = 0;
  for (int n = 0; n < total; n += SND_BLK) {
    float t = (float)n / SND_SR;
    if (t >= nextTick) {
      c.t = nextTick + 0.1f;
      sc->fn(c);
      sndSetWorld(c.w);
      nextTick += 0.1f;
    }
    gNow = t;
    auto t0 = std::chrono::high_resolution_clock::now();
    sndRender(buf, SND_BLK);
    auto t1 = std::chrono::high_resolution_clock::now();
    renderSec += std::chrono::duration<double>(t1 - t0).count();
    int av = sndActiveVoices(); voiceBlocks += av; blocks++; if (av > maxV) maxV = av;
    static uint8_t lastStyle = 255;
    if (SMu.style != lastStyle) {
      lastStyle = SMu.style;
      fprintf(stderr, "t=%6.1fs style=%-9s tension=%.2f bpm=%.0f tonic=%d%c", t, MUS_STYLE_NAME[SMu.style],
              SMu.tension, SMu.bpmTgt, SMu.tonicPc, 10);
    }
    for (int i = 0; i < SND_BLK; i++) pcm.push_back(buf[2 * i]);
  }
  writeWav(outdir + "/" + name + suffix + ".wav", pcm);
  if (suffix.empty()) {
    FILE* nf = fopen((outdir + "/" + name + "_notes.csv").c_str(), "w");
    if (nf) {
      fprintf(nf, "t,bus,part,patch,midi,vel,dur%c", 10);
      for (const NoteLog& e : gNotes) fprintf(nf, "%.4f,%u,%u,%u,%.2f,%.3f,%.3f%c", e.t, e.bus, e.part, e.patch, e.midi, e.vel, e.dur, 10);
      fclose(nf);
    }
  }
  printf("{\"scene\":\"%s\",\"seconds\":%.1f,\"render_x_realtime\":%.4f,\"avg_voices\":%.2f,"
         "\"max_voices\":%d,\"clipped\":%u,\"peak\":%.3f,\"cues\":%u,\"dropped\":%u,\"quiet_blocks\":%u,"
         "\"final_style\":\"%s\"}\n",
         name.c_str(), sc->dur, renderSec / sc->dur, blocks ? (double)voiceBlocks / blocks : 0.0, maxV,
         (unsigned)SM.clipped, SM.peak, (unsigned)SST.cues, (unsigned)SST.dropped, (unsigned)SST.quietBlocks,
         MUS_STYLE_NAME[SMu.style]);
  return 0;
}
