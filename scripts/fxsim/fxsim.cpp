// fxsim.cpp -- the LCD FX layer (ui-fx.hpp) compiled for the desktop.
//
// Builds the firmware header UNCHANGED under FX_NATIVE and supplies the hooks
// its section 0 expects: the three LovyanGFX font tables (included straight
// from LovyanGFX's own headers, so the glyphs are the board's), a no-op lock,
// calloc for PSRAM, a scripted clock, and survivor names. Then it plays cues
// over a real screen and writes every composed frame out as raw big-endian
// RGB565 -- the byte order the sprite itself holds -- for fxsim.py to turn into
// GIFs at the real frame timing. Run it through fxsim.py, not by hand:
//
//     python scripts/fxsim/fxsim.py [scene ...]
//
// See docs/dev-loop.md, "Previewing the LCD FX on the desktop".
#define FX_NATIVE 1
#define _CRT_SECURE_NO_WARNINGS
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// LovyanGFX's GFX font headers name their types GFXglyph / GFXfont. These are
// field-for-field lgfx::GFXglyph / lgfx::GFXfont minus the IFont base, which is
// all ui-fx.hpp touches: bitmap, glyph[], first, last.
#define PROGMEM
struct FxGfxGlyph { uint32_t bitmapOffset; uint8_t width, height, xAdvance; int8_t xOffset, yOffset; };
struct FxGfxFont  { uint8_t* bitmap; FxGfxGlyph* glyph; uint16_t first, last; uint8_t yAdvance; };
#define GFXglyph FxGfxGlyph
#define GFXfont  FxGfxFont
#include "GFXFF/FreeSansBoldOblique24pt7b.h"   // /I <LovyanGFX>/src/lgfx/Fonts
#undef GFXglyph
#undef GFXfont
#include "glcdfont.h"                           // Font0: font[], 5 bytes per char
#include "Font16.h"                             // Font2: chrtbl_f16 / widtbl_f16

static inline uint16_t c16(uint32_t c) {        // ui-helpers.hpp's, verbatim
  return (uint16_t)(((c & 0xF80000) >> 8) | ((c & 0x00FC00) >> 5) | ((c & 0x0000F8) >> 3));
}
static inline const uint8_t*   fxGlcdTable()        { return font; }
static inline const uint8_t*   fxF2Glyph(uint8_t c) { return chrtbl_f16[c - 32]; }
static inline uint8_t          fxF2W(uint8_t c)     { return widtbl_f16[c - 32]; }
static inline const FxGfxFont& fxSfxFont()          { return FreeSansBoldOblique24pt7b; }
#define FX_LOCK()
#define FX_UNLOCK()
static inline void* fxAlloc(size_t n) { return calloc(1, n); }

static uint32_t g_now = 1;
#include "../../ui-fx.hpp"
#include "../../ui-scenes.hpp"

static uint32_t fxNowMs() { return g_now; }
static void fxNameOf(int8_t who, char* out, size_t cap) {
  static const char* N[6] = { "Vera", "Quartermaster1", "Mox", "Ashenford", "Kell", "Rell" };
  snprintf(out, cap, "%s", (who >= 0 && who < 6) ? N[who] : "Someone");
}

static std::vector<uint16_t> loadRaw(const std::string& p) {
  std::vector<uint16_t> v(FX_W * FX_H, 0);
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) { fprintf(stderr, "missing %s\n", p.c_str()); exit(1); }
  size_t got = fread(v.data(), 2, v.size(), f);
  fclose(f);
  if (got != v.size()) { fprintf(stderr, "short %s\n", p.c_str()); exit(1); }
  return v;
}

struct Step { uint32_t at; int kind; int who; const char* cap; const char* sfx; int arg; };

// One scene: cues fire at their times over `base`; `switchAt` cuts to `base2`
// through the Button-B blade; `contentAt` swaps in `base2` as an ordinary
// repaint (the reprint effect); `dread` drives the madness layer. Frames are
// paced exactly as the board paces them (fxFramePeriod, or 100 ms idle).
static void run(const char* name, const std::string& in, const std::string& out,
                const char* base, const char* base2, uint32_t switchAt, uint32_t contentAt,
                uint32_t endMs, std::vector<Step> steps, FxDread dread, bool forceSub) {
  std::vector<uint16_t> A = loadRaw(in + "/" + base + ".raw");
  std::vector<uint16_t> B = base2 ? loadRaw(in + "/" + base2 + ".raw") : A;
  std::vector<uint16_t> frame(FX_W * FX_H, 0), prev(FX_W * FX_H, 0);
  fxInitTables();
  fxAllocPools();
  FX.level = 2;
  g_now = 1000;
  FX.lastMs = 0;
  FX.nextGlitch = g_now + 2500;
  FX.nextSub = forceSub ? g_now + 1500 : g_now + 999999;
  FX.nextEyes = g_now + 700;
  fxSetDread(dread);
  FX.madness = FX.madTarget;
  const uint16_t* src = A.data();
  fxContentChanged(src, g_now, true);
  FILE* fi = fopen((out + "/" + name + ".txt").c_str(), "w");
  uint32_t t0 = g_now;
  bool switched = false, changed = false;
  size_t si = 0;
  int n = 0;
  while (g_now - t0 < endMs) {
    uint32_t t = g_now - t0;
    while (si < steps.size() && steps[si].at <= t) {
      const Step& s = steps[si++];
      fxCue((uint8_t)s.kind, (int8_t)s.who, s.cap, s.sfx, (uint16_t)s.arg);
    }
    if (switchAt && !switched && t >= switchAt) {
      switched = true;
      prev = frame;
      src = B.data();
      fxContentChanged(src, g_now, true);
      fxBeginSwitch(g_now);
    }
    if (contentAt && !changed && t >= contentAt) {
      changed = true;
      src = B.data();
      fxContentChanged(src, g_now, false);
    }
    fxCompose(src, prev.data(), frame.data(), g_now);
    char fn[1024];
    snprintf(fn, sizeof(fn), "%s/%s_%04d.raw", out.c_str(), name, n);
    FILE* f = fopen(fn, "wb");
    fwrite(frame.data(), 2, frame.size(), f);
    fclose(f);
    uint32_t per = fxAnimating(g_now) ? fxFramePeriod(g_now) : 100;
    fprintf(fi, "%d %u %u\n", n, t, per);
    n++;
    g_now += per;
  }
  fclose(fi);
  printf("%-10s %3d frames\n", name, n);
}

// A picture screen (ui-scenes.hpp): the scene is redrawn into the canvas every
// scenePeriod() ms, exactly as the board's loop does, and composed on top.
static SceneSurv surv(const char* nm, uint8_t arch, uint8_t ll, uint8_t cap, uint8_t food, uint8_t water,
                      uint8_t rad, int8_t moves = 3, uint8_t depth = 0, bool inEnc = false, uint16_t score = 0) {
  SceneSurv v = {};
  v.on = true; snprintf(v.name, sizeof(v.name), "%s", nm);
  v.arch = arch; v.ll = ll; v.llCap = cap; v.food = food; v.water = water; v.rad = rad;
  v.moves = moves; v.depth = depth; v.inEnc = inEnc; v.score = score;
  return v;
}
static void runScene(const char* name, const std::string& out, uint8_t screen, SceneSnap S,
                     uint32_t endMs, std::vector<Step> steps, FxDread dread, uint16_t milPerSec = 0) {
  std::vector<uint16_t> canvasBuf(FX_W * FX_H, 0), frame(FX_W * FX_H, 0), prev(FX_W * FX_H, 0);
  fxInitTables();
  fxAllocPools();
  FX.level = 2;
  g_now = 1000;
  FX.lastMs = 0;
  FX.nextGlitch = g_now + 2500;
  FX.nextSub = g_now + 999999;
  FX.nextEyes = g_now + 999999;
  fxSetDread(dread);
  FX.madness = FX.madTarget;
  scRoadSt.last = 0; scRoadSt.walked = 0; scRoadSt.nar = nullptr;
  FILE* fi = fopen((out + "/" + name + ".txt").c_str(), "w");
  uint32_t t0 = g_now, lastScene = 0;
  uint16_t mil0 = S.dayMil;
  size_t si = 0;
  int n = 0;
  bool first = true;
  while (g_now - t0 < endMs) {
    uint32_t t = g_now - t0;
    while (si < steps.size() && steps[si].at <= t) {
      const Step& s = steps[si++];
      fxCue((uint8_t)s.kind, (int8_t)s.who, s.cap, s.sfx, (uint16_t)s.arg);
    }
    if (first || g_now - lastScene >= scenePeriod(screen)) {
      if (milPerSec) S.dayMil = (uint16_t)((mil0 + (uint32_t)t * milPerSec / 1000) % 1000);
      sceneDraw(screen, S, g_now, canvasBuf.data());
      fxContentChanged(canvasBuf.data(), g_now, true);
      lastScene = g_now;
      first = false;
    }
    fxCompose(canvasBuf.data(), prev.data(), frame.data(), g_now);
    char fn[1024];
    snprintf(fn, sizeof(fn), "%s/%s_%04d.raw", out.c_str(), name, n);
    FILE* f = fopen(fn, "wb");
    fwrite(frame.data(), 2, frame.size(), f);
    fclose(f);
    uint32_t per = fxAnimating(g_now) ? fxFramePeriod(g_now) : 100;
    uint32_t sp = scenePeriod(screen), next = lastScene + sp - g_now;
    if (next < per) per = next < 20 ? 20 : next;
    fprintf(fi, "%d %u %u\n", n, t, per);
    n++;
    g_now += per;
  }
  fclose(fi);
  printf("%-10s %3d frames\n", name, n);
}

static SceneSnap baseSnap() {
  SceneSnap S = {};
  S.day = 12; S.dayMil = 300; S.weather = 0; S.tcWeight = 60; S.threat = 3;
  snprintf(S.ap, sizeof(S.ap), "192.168.47.1");
  snprintf(S.lan, sizeof(S.lan), "10.0.0.23");
  S.lanUp = true;
  return S;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: fxsim <raw-dir> <frame-dir> [scene]\n"); return 2; }
  std::string in = argv[1], out = argv[2], only = argc > 3 ? argv[3] : "";
  FxDread calm = {};
  calm.connected = 5;
  FxDread doom = calm;
  doom.doomAware = 90; doom.doomClose = 235; doom.tc = 200; doom.attrition = 170; doom.hunger = 120;
  auto want = [&](const char* n) { return only.empty() || only == n; };
  if (want("quake"))   run("quake",   in, out, "dashboard", nullptr, 0, 0, 2700,
      { { 250, FXK_QUAKE, -1, "The earth heaves. 2 shelters lost.", nullptr, 0 } }, calm, false);
  if (want("strike"))  run("strike",  in, out, "dashboard", nullptr, 0, 0, 2400,
      { { 250, FXK_STRIKE, 2, "is struck where they stand.", nullptr, 0 } }, calm, false);
  if (want("dawn"))    run("dawn",    in, out, "chronicle", nullptr, 0, 0, 2600,
      { { 250, FXK_DAWN, -1, "Day 12 comes up grey over the waste.", nullptr, 12 } }, calm, false);
  if (want("eye"))     run("eye",     in, out, "dashboard", nullptr, 0, 0, 2900,
      { { 250, FXK_DOOM_EYE, 3, "feels the Doom turn its head.", nullptr, 0 } }, calm, false);
  if (want("hunt"))    run("hunt",    in, out, "encounters", nullptr, 0, 0, 2800,
      { { 250, FXK_DOOM_HUNT, 0, "is being hunted. It has stopped tracking.", nullptr, 0 } }, calm, false);
  if (want("switch"))  run("switch",  in, out, "dashboard", "chronicle", 300, 0, 1200, {}, calm, false);
  if (want("madness")) run("madness", in, out, "dashboard", nullptr, 0, 0, 9000, {}, doom, true);
  if (want("fire"))    run("fire",    in, out, "resources", nullptr, 0, 0, 2800,
      { { 250, FXK_FIRE, 1, "walks into fire and wears it out.", nullptr, 0 } }, calm, false);
  if (want("threat"))  run("threat",  in, out, "dashboard", nullptr, 0, 0, 2500,
      { { 250, FXK_THREAT, -1, "The clock turns. Nothing out here is sleeping now.", nullptr, 3 } }, calm, false);
  if (want("chem"))    run("chem",    in, out, "dashboard", nullptr, 0, 0, 2500,
      { { 250, FXK_CHEM, -1, "The rain turns wrong. Chem burn.", nullptr, 0 } }, calm, false);
  if (want("storm"))   run("storm",   in, out, "dashboard", nullptr, 0, 0, 2500,
      { { 250, FXK_STORM, -1, "A storm walks in off the flats.", nullptr, 0 } }, calm, false);
  if (want("join"))    run("join",    in, out, "dashboard", nullptr, 0, 0, 2300,
      { { 250, FXK_JOIN, 3, "takes up the road with us.", nullptr, 0 } }, calm, false);
  if (want("thrown"))  run("thrown",  in, out, "encounters", nullptr, 0, 0, 2300,
      { { 250, FXK_THROWN, 0, "is thrown back into daylight, bleeding.", nullptr, 0 } }, calm, false);
  if (want("act"))     run("act",     in, out, "dashboard", nullptr, 0, 0, 2800,
      { { 250, FXK_DOOM_ACT, 2, "pays the Doom what it came for.", nullptr, 0 } }, calm, false);
  if (want("odds"))    run("odds",    in, out, "encounters", nullptr, 0, 0, 2400,
      { { 250, FXK_LONGODDS, 0, "gets the panel open and the dark gives.", nullptr, 0 } }, calm, false);
  if (want("fog"))     run("fog",     in, out, "dashboard", nullptr, 0, 0, 2600,
      { { 250, FXK_FOG, -1, "Strangle fog settles in the low ground.", nullptr, 0 } }, calm, false);
  if (want("reprint")) run("reprint", in, out, "dashboard", "dashboard-hurt", 0, 300, 900, {}, calm, false);
  if (want("below"))   run("below",   in, out, "dashboard", nullptr, 0, 0, 7700,
      { { 250, FXK_BELOW, 2, "goes down into the dark.", nullptr, 0 } }, calm, false);
  if (want("crafted")) run("crafted", in, out, "resources", nullptr, 0, 0, 6900,
      { { 250, FXK_CRAFTED, 1, "Sock Puppet Bandage", nullptr, 2 } }, calm, false);
  if (want("crawl"))   run("crawl",   in, out, "dashboard", nullptr, 0, 0, 6500,
      { { 250, FXK_CRAWL, 2, nullptr, nullptr, 0 } }, calm, false);
  if (want("tripwire")) run("tripwire", in, out, "dashboard", nullptr, 0, 0, 3700,
      { { 250, FXK_TRIPWIRE, 2, "puts a foot down and hears it click.", nullptr, 0 } }, calm, false);
  if (want("beartrap")) run("beartrap", in, out, "encounters", nullptr, 0, 0, 5200,
      { { 250, FXK_BEARTRAP, 0, "is caught and held while it bites.", nullptr, 0 } }, calm, false);
  {
    SceneSnap S = baseSnap();
    S.s[0] = surv("Vera", 0, 6, 7, 5, 4, 1);
    S.s[1] = surv("Quartermaster1", 1, 2, 7, 1, 3, 2);
    S.s[3] = surv("Mox", 3, 5, 7, 4, 1, 0);
    S.s[4] = surv("Kell", 4, 7, 8, 3, 3, 8);
    if (want("road_day"))   runScene("road_day", out, 1, S, 6000, {}, calm);
    SceneSnap D = S; D.dayMil = 690; D.weather = 2; D.doomClose = 200; D.doomAware = 90; D.tcWeight = 215;
    D.s[1].ll = 0; D.s[3].inEnc = true;
    if (want("road_storm")) runScene("road_storm", out, 1, D, 6000, {}, doom);
    SceneSnap N = S; N.dayMil = 860; N.weather = 3; N.s[4].depth = 1;
    if (want("road_night")) runScene("road_night", out, 1, N, 5000, {}, calm);
    SceneSnap F = S; F.weather = 5; F.dayMil = 120;
    if (want("road_fog"))   runScene("road_fog", out, 1, F, 5000, {}, calm);
    SceneSnap E = baseSnap(); E.dayMil = 450;
    if (want("road_empty")) runScene("road_empty", out, 1, E, 6000, {}, FxDread{});
    SceneSnap B = S;
    B.s[0].score = 3160; B.s[1].score = 2440; B.s[3].score = 900; B.s[4].score = 60;
    if (want("admired"))    runScene("admired", out, 6, B, 7000, {}, calm);
    SceneSnap B0 = baseSnap();
    if (want("admired_empty")) runScene("admired_empty", out, 6, B0, 5000, {}, FxDread{});
    SceneSnap C = S;
    if (want("road_cycle")) runScene("road_cycle", out, 1, C, 16000, {}, calm, 62);
  }
  return 0;
}
