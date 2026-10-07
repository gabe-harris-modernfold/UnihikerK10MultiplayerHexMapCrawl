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
#include <functional>

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
                     uint32_t endMs, std::vector<Step> steps, FxDread dread, uint16_t milPerSec = 0,
                     std::function<void(SceneSnap&, uint32_t)> mut = nullptr) {
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
      if (mut) mut(S, t);
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

// Contact sheets of the party (scFigure), straight into the canvas with no FX
// over them: every archetype in every pose, walking in place.
//   figs        k 1.2, a row per archetype: walk stand reach hold crouch sit lie
//   figs_big    k 2.1, three archetypes a frame (walk, stand, crouch), turning every 1.5 s
//   figs_night  k 1.35, round a fire: the firelight rim
static void figSheet(uint16_t* buf, uint32_t now, int mode) {
  scReset(buf);
  scFigT = now;
  const float ph = now * 0.0105f;
  if (mode == 0) {
    static const uint8_t PO[7] = { SCP_WALK, SCP_STAND, SCP_REACH, SCP_HOLD, SCP_CROUCH, SCP_SIT, SCP_LIE };
    static const float   PXs[7] = { 16, 48, 78, 106, 134, 170, 204 };
    for (int a = 0; a < 6; a++) {
      int y0 = a * 53, gy = y0 + 47;
      for (int y = y0; y < y0 + 53; y++) scTone(y, 0, FX_W, y < gy ? fxLerp(4.0f, 10.5f, (float)(y - y0) / 47.0f) : 2.0f);
      for (int p = 0; p < 7; p++) {
        ScFig F = { PXs[p], (float)gy, 1, (uint8_t)a, PO[p], (uint8_t)a, ph + p, FXP_BRICK, -0.7f, -0.7f, 1.2f };
        if (p == 0) F.shadow = 1.2f;
        for (int q = 0; q < 3; q++) scFigure(F, q);
      }
    }
  } else if (mode == 1) {
    int g0 = ((now / 1500) & 1) * 3;
    static const uint8_t PO[3] = { SCP_WALK, SCP_STAND, SCP_CROUCH };
    static const float   PXs[3] = { 44, 128, 190 };
    for (int r = 0; r < 3; r++) {
      int y0 = r * 106, gy = y0 + 98;
      for (int y = y0; y < y0 + 106; y++) scTone(y, 0, FX_W, y < gy ? fxLerp(3.0f, 10.8f, (float)(y - y0) / 98.0f) : 2.0f);
      for (int p = 0; p < 3; p++) {
        ScFig F = { PXs[p], (float)gy, 1, (uint8_t)(g0 + r), PO[p], (uint8_t)(g0 + r), ph, FXP_BRICK, -0.7f, -0.7f, 2.1f };
        for (int q = 0; q < 3; q++) scFigure(F, q);
      }
    }
  } else {
    for (int y = 0; y < FX_H; y++) scTone(y, 0, FX_W, y < 150 ? 0.4f : 1.0f);
    const float fx = 120, fy = 150;
    scFireGlow(fx, fy, now);
    static const float CAMP[6] = { -38, 38, -74, 74, -104, 104 };
    for (int a = 0; a < 6; a++) {
      float x = fx + CAMP[a], dir = x < fx ? 1.0f : -1.0f;
      ScFig F = { x, fy + (a & 2 ? 2.0f : 0.0f), dir, (uint8_t)a, SCP_SIT, (uint8_t)a, 0, FXP_FLAME, dir, -0.4f, 1.35f };
      for (int q = 0; q < 3; q++) scFigure(F, q);
    }
    scFire(fx, fy, now);
    // and by day, the same six standing: the sky rim
    for (int y = 170; y < FX_H; y++) scTone(y, 0, FX_W, y < 300 ? fxLerp(2.0f, 11.0f, (y - 170) / 130.0f) : 1.5f);
    for (int a = 0; a < 6; a++) {
      ScFig F = { 20.0f + a * 40, 300, 1, (uint8_t)a, SCP_STAND, (uint8_t)a, 0, FXP_BRICK, 0.7f, -0.7f, 1.35f };
      F.shadow = 1.4f;
      for (int q = 0; q < 3; q++) scFigure(F, q);
    }
  }
  scReset(buf);
}
static void runFigs(const char* name, const std::string& out, int mode, uint32_t endMs) {
  std::vector<uint16_t> canvasBuf(FX_W * FX_H, 0);
  fxInitTables();
  FILE* fi = fopen((out + "/" + name + ".txt").c_str(), "w");
  int n = 0;
  for (uint32_t t = 0; t < endMs; t += 140, n++) {
    figSheet(canvasBuf.data(), 1000 + t, mode);
    char fn[1024];
    snprintf(fn, sizeof(fn), "%s/%s_%04d.raw", out.c_str(), name, n);
    FILE* f = fopen(fn, "wb");
    fwrite(canvasBuf.data(), 2, canvasBuf.size(), f);
    fclose(f);
    fprintf(fi, "%d %u %u\n", n, t, 140);
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
    auto logAt = [](SceneSnap& X, int i, const char* txt, uint8_t gly, uint8_t tone, int8_t who, int8_t who2, uint16_t serial) {
      SceneLog& e = X.log[i];
      snprintf(e.text, sizeof(e.text), "%s", txt);
      e.glyph = gly; e.tone = tone; e.day = X.day; e.who = who; e.who2 = who2; e.serial = serial;
    };
    // The chronicle: three pages of it, each one a set of three panels.
    struct Ev { const char* t; uint8_t g, tone; int8_t w, w2; };
    static const Ev EV[] = {
      { "Vera finds a seep still running.",                SC_GLY_WATER,     SC_TONE_GOOD,  0, -1 },
      { "Mox gets the panel open and the dark gives.",     SC_GLY_LIGHT,     SC_TONE_GOOD,  3, -1 },
      { "Kell is caught out when the rain turns wrong.",   SC_GLY_CHEM,      SC_TONE_ILL,   4, -1 },
      { "Quartermaster1 trades Vera two tins for a filter.", SC_GLY_TRADE,   SC_TONE_PLAIN, 1,  0 },
      { "Day 13 comes up grey over the waste.",            SC_GLY_DAWN,      SC_TONE_PLAIN, -1, -1 },
      { "Mox walks into fire and wears it out.",           SC_GLY_FIRE,      SC_TONE_ILL,   3, -1 },
      { "Kell goes through a door that was shut for a reason.", SC_GLY_THRESHOLD, SC_TONE_OMEN, 4, -1 },
      { "Vera meets something with too many teeth.",       SC_GLY_CLASH,     SC_TONE_ILL,   0, -1 },
      { "The Doom turns its head toward Quartermaster1.",  SC_GLY_DOOM,      SC_TONE_OMEN,  1, -1 },
      { "Mox beds down and the night lets them.",          SC_GLY_REST,      SC_TONE_PLAIN, 3, -1 },
      { "A storm walks in off the flats.",                 SC_GLY_STORM,     SC_TONE_ILL,   4, -1 },
      { "Commended: Heavy Load. Vera, for carrying it all out.", SC_GLY_MEDAL, SC_TONE_GOOD, 0, -1 },
    };
    SceneSnap H = S;
    for (int i = 0; i < 3; i++) logAt(H, i, EV[i].t, EV[i].g, EV[i].tone, EV[i].w, EV[i].w2, (uint16_t)(40 + i));
    H.logN = 3; H.logTotal = 42;
    if (want("chronicle")) runScene("chronicle", out, 2, H, 6000, {}, calm);
    for (int page = 1; page < 4; page++) {
      char nm[32];
      snprintf(nm, sizeof(nm), "chronicle%d", page);
      SceneSnap Hp = S;
      for (int i = 0; i < 3; i++) { const Ev& e = EV[page * 3 + i]; logAt(Hp, i, e.t, e.g, e.tone, e.w, e.w2, (uint16_t)(50 + page * 3 + i)); }
      Hp.logN = 3; Hp.logTotal = (uint16_t)(53 + page * 3);
      if (want(nm)) runScene(strdup(nm), out, 2, Hp, 5000, {}, calm);
    }
    // Every picture, three to a page: chron_g0 .. chron_g10 (a contact sheet).
    for (int pg = 0; pg < 11; pg++) {
      char nm[32];
      snprintf(nm, sizeof(nm), "chron_g%d", pg);
      if (!want(nm)) continue;
      SceneSnap Hg = S;
      static const char* const TN[4] = { "plain", "good", "ill", "omen" };
      for (int i = 0; i < 3; i++) {
        int g = pg * 3 + i;
        if (g >= SC_GLY_COUNT) g = 0;
        char txt[64];
        snprintf(txt, sizeof(txt), "Glyph %d, %s tone. Vera did a thing here.", g, TN[g % 4]);
        logAt(Hg, i, txt, (uint8_t)g, (uint8_t)(g % 4), (int8_t)(g % 5 == 2 ? 2 : g % 5), 1, (uint16_t)(100 + g));
      }
      Hg.logN = 3; Hg.logTotal = 200;
      runScene(strdup(nm), out, 2, Hg, 1500, {}, calm);
    }
    // The stash: a Mule with a full pack, and a Scout with nothing.
    {
      SceneSnap T = S;
      T.s[3].res[0] = 5; T.s[3].res[1] = 4; T.s[3].res[2] = 1; T.s[3].res[3] = 2; T.s[3].res[4] = 11;
      T.s[3].packCap = 16; T.s[3].packUsed = 14;
      static const char* IT[14] = { "Rusty Knife", "Sock Puppet Bandage", "Rope", "Gas Mask Filter", "Tin Opener",
        "Canteen", "Raft Kit", "Hand Crank Radio", "Duct Tape", "Iodine Tabs", "Road Map (wrong)", "Spark Plug",
        "Lucky Bottlecap", "Uranium Candy" };
      for (int i = 0; i < 14; i++) { snprintf(T.item[i], sizeof(T.item[i]), "%s", IT[i]); T.itemQty[i] = (uint8_t)(1 + (i * 7) % 4); }
      T.itemN = 14; T.page = 3;
      if (want("stash")) runScene("stash", out, 3, T, 5000, {}, calm);
      SceneSnap U = S; U.page = 4; U.s[4].res[0] = 0; U.s[4].res[1] = 1; U.s[4].res[2] = 0; U.s[4].res[3] = 0; U.s[4].res[4] = 2;
      U.s[4].packCap = 8; U.s[4].packUsed = 0; U.itemN = 0;
      if (want("stash_bare")) runScene("stash_bare", out, 3, U, 4000, {}, calm);
      // stash_cast: the same stash knelt over by each archetype in turn, 1.5 s each
      auto kneel = [](SceneSnap& X, uint32_t t) { X.s[3].arch = (uint8_t)((t / 1500) % 6); };
      if (want("stash_cast")) runScene("stash_cast", out, 3, T, 9000, {}, calm, 0, kneel);
    }
    // The odds: a record so far, then a roll lands at 0.6 s -- a win, and a loss.
    {
      SceneSnap O = S;
      O.rolls = 18; O.wins = 11; O.poiLeft = 34; O.poiTotal = 80;
      O.hasRoll = true; O.rollWho = 1; O.rollSkill = 2; O.rollDN = 8; O.rollTotal = 5; O.rollWon = false;
      O.s[0].encRolls = 7; O.s[0].encWins = 5; O.s[0].hasEnd = true; O.s[0].lastEnd = 6; O.s[0].encCount = 7;
      O.s[1].encRolls = 6; O.s[1].encWins = 2; O.s[1].hasEnd = true; O.s[1].lastEnd = 0; O.s[1].encCount = 6;
      O.s[3].inEnc = true; snprintf(O.s[3].biome, sizeof(O.s[3].biome), "flooded"); O.s[3].loot = 4;
      O.s[4].encCount = 0;
      auto win = [](SceneSnap& X, uint32_t t) { if (t >= 600) { X.rolls = 19; X.wins = 12; X.rollWho = 0; X.rollSkill = 1; X.rollDN = 7; X.rollTotal = 10; X.rollWon = true; } };
      auto lose = [](SceneSnap& X, uint32_t t) { if (t >= 600) { X.rolls = 19; X.rollWho = 4; X.rollSkill = 4; X.rollDN = 11; X.rollTotal = 6; X.rollWon = false; } };
      if (want("odds_win"))  runScene("odds_win", out, 4, O, 3600, {}, calm, 0, win);
      if (want("odds_lose")) runScene("odds_lose", out, 4, O, 3600, {}, calm, 0, lose);
      SceneSnap O0 = S;
      if (want("odds_none")) runScene("odds_none", out, 4, O0, 2500, {}, calm);
      // odds_cast: every archetype takes a swing, one every 2.4 s (win, lose, win...)
      auto cast = [](SceneSnap& X, uint32_t t) {
        int a = (int)(t / 2400) % 6;
        X.s[0].arch = (uint8_t)a; X.rollWho = 0; X.rolls = (uint16_t)(20 + t / 2400);
        X.rollWon = (a & 1) == 0; X.rollTotal = X.rollWon ? 10 : 5; X.rollDN = 8; X.rollSkill = (uint8_t)(a % 5);
      };
      if (want("odds_cast")) runScene("odds_cast", out, 4, O, 14400, {}, calm, 0, cast);
    }
    // The waste: a made-up ring of terrain from value noise, the party spread over it.
    {
      static uint8_t T[57][75];
      for (int r = 0; r < 57; r++)
        for (int q = 0; q < 75; q++) {
          float n = fxNoise2(q * 0.11f, r * 0.13f, 5) * 0.7f + fxNoise2(q * 0.3f, r * 0.3f, 9) * 0.3f;
          float rv = fabsf(fxNoise2(q * 0.05f, r * 0.05f, 21) - 0.5f);
          uint8_t t = n < 0.3f ? 1 : n < 0.42f ? 0 : n < 0.5f ? 2 : n < 0.57f ? 3 : n < 0.68f ? 4 : n < 0.76f ? 6 : 0;
          if (rv < 0.02f) t = 11;
          uint32_t h = fxHash2((uint32_t)(r * 75 + q), 77);
          if (h % 211 == 0) t = 9;
          if (h % 397 == 0) t = 10;
          T[r][q] = t;
        }
      SceneSnap M = S;
      M.terr = &T[0][0]; M.cols = 75; M.rows = 57;
      M.s[0].q = 30; M.s[0].r = 20; M.s[1].q = 33; M.s[1].r = 24; M.s[3].q = 28; M.s[3].r = 26; M.s[4].q = 52; M.s[4].r = 12; M.s[4].depth = 1;
      M.caravanOn = true; M.caravanQ = 12; M.caravanR = 40;
      M.doomQ = 47; M.doomR = 30; M.doomRadius = 5; M.doomAware = 82;
      M.hatchN = 3; M.hatchQ[0] = 52; M.hatchR[0] = 12; M.hatchQ[1] = 8; M.hatchR[1] = 8; M.hatchQ[2] = 66; M.hatchR[2] = 48;
      if (want("waste")) runScene("waste", out, 5, M, 6000, {}, calm);
    }
    SceneSnap H0 = S;
    if (want("chronicle_empty")) runScene("chronicle_empty", out, 2, H0, 4000, {}, calm);
    SceneSnap C = S;
    if (want("road_cycle")) runScene("road_cycle", out, 1, C, 16000, {}, calm, 62);
  }
  if (want("figs"))       runFigs("figs", out, 0, 1400);
  if (want("figs_big"))   runFigs("figs_big", out, 1, 3000);
  if (want("figs_night")) runFigs("figs_night", out, 2, 1400);
  return 0;
}
