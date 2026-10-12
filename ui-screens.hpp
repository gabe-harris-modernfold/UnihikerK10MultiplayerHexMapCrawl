#pragma once
// ── ui-screens.hpp ───────────────────────────────────────────────────────────
// The K10 display screens and button-B screen cycling. Every screen is a
// picture now (ui-scenes.hpp); this file takes the snapshot they draw from.

// ── World entity markers for the minimap ─────────────────────────────────────
// The map screen plots the Caravan and Creeping Doom alongside the players, but
// world-system.hpp (which owns `W`) is included *after* this file — see the
// include block in Esp32HexMapCrawl.ino. Declare the accessor here and define
// it there; drawMapScreen() only ever calls it at runtime, so the prototype is
// all this translation unit needs. Caller must hold G.mutex: W is protected by
// it exactly like G.map/G.players (see the world-tick block in
// actions_game_loop.hpp).
struct WorldMarkers {
  int16_t caravanQ, caravanR;
  bool    caravanActive;
  int16_t doomQ, doomR;
  uint8_t doomAwareness;   // 0-100
  int16_t doomRadius;      // hexes of scent detection at the current awareness
};
static void snapshotWorldMarkers(WorldMarkers& out);

// Defined in inventory_items.hpp, which is included *after* this file: pack
// size in effect right now = archetype base + every equipped item's "slots"
// bonus (Hoarder's Rig, Backpack, Knife-Wrench), capped at INV_SLOTS_MAX.
// The resource screen's PACK readout has to use this, not Player::invSlots,
// or the bonus slots and everything sitting in them go missing from the LCD.
// Caller must hold G.mutex.
static uint8_t effectiveInvSlots(const Player& p);
// Same deal: the survivor's LL ceiling, 7 plus every equipped item's "ll"
// bonus minus any permanent penalty.  The dashboard's LIFE bar is drawn
// against it rather than a literal 7.
static uint8_t effectiveMaxLL(int pid);

// The column every screen aligns its right-hand edge to. Proportional faces
// cannot be positioned by counting characters, so the canvasText*R helpers in
// ui-helpers.hpp take this as the column the string ENDS at.
static constexpr int DASH_R = 238;

// -- Screens 1 and 6: pictures ---------------------------------
// The road (the party, their day and their ledger) and the boneyard of the
// admired are drawn by ui-scenes.hpp straight into the canvas's buffer, and
// they animate, so they are repainted every scenePeriod() ms rather than
// every SCREEN_MS (see the display block in loop()). All the board side does
// is fill the snapshot they draw from, under G.mutex, and keep the one piece
// of diagnostics that survived: the join and LAN addresses.
static_assert(SC_SEATS == MAX_PLAYERS, "SceneSnap has one row per seat");

static void sceneSnapFill(SceneSnap& S) {
  // Kept between calls: if the lock is busy this frame the scene draws the
  // last good snapshot instead of an empty party.
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    S.day     = G.dayCount;
    uint32_t mil = (uint32_t)G.dayTick * 1000u / DAY_TICKS;
    S.dayMil  = (uint16_t)(mil > 999 ? 999 : mil);
    S.weather = G.weatherPhase < 6 ? G.weatherPhase : 0;
    S.threat  = G.threatClock;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Player& p = G.players[i];
      SceneSurv& v = S.s[i];
      v.on    = p.connected;
      memcpy(v.name, p.name, sizeof(v.name));
      v.name[sizeof(v.name) - 1] = 0;
      v.arch  = p.archetype < NUM_ARCHETYPES ? p.archetype : 0;
      v.ll    = p.ll;
      v.llCap = effectiveMaxLL(i);   // base 7 + equipment - penalties
      v.food  = p.food;
      v.water = p.water;
      v.rad   = p.radiation;
      v.ap = p.apLeft;
      v.depth = p.depth;
      v.inEnc = (encounters[i].active & 1) != 0;
      v.score = p.score;
      v.q     = p.q;
      v.r     = p.r;
      memcpy(v.res, p.inv, 5);
      v.packCap = effectiveInvSlots(p);   // base + equipped "slots" bonuses
      uint8_t used = 0;
      for (int k = 0; k < v.packCap && k < INV_SLOTS_MAX; k++) if (p.invType[k]) used++;
      v.packUsed = used;
    }
    xSemaphoreGive(G.mutex);
  }
  S.tcWeight  = g_dread.tcWeight;
  S.doomClose = g_dread.doomClose;
  S.doomAware = g_dread.doomAware;

  IPAddress apIp = WiFi.softAPIP(), staIp = WiFi.localIP();
  snprintf(S.ap, sizeof(S.ap), "%d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  S.lanUp = staIp[0] != 0;
  if (S.lanUp)                snprintf(S.lan, sizeof(S.lan), "%d.%d.%d.%d", staIp[0], staIp[1], staIp[2], staIp[3]);
  else if (bootWifiPending)   snprintf(S.lan, sizeof(S.lan), "connecting");
  else if (savedSsid[0])      snprintf(S.lan, sizeof(S.lan), "%.14s", savedSsid);
  else if (g_knownCount > 0)  snprintf(S.lan, sizeof(S.lan), "searching");   // the roaming sweep is looking
  else                        snprintf(S.lan, sizeof(S.lan), "no credentials");
}

// Draw a scene into the canvas. What a frame costs goes to the log every 30 s
// while one is up, next to the compositor's own "LCD FX" line: together they
// are the whole price of an animated screen.
static void sceneLogFill(SceneSnap& S);   // with the chronicle's helpers, below

static void sceneScreenFill(SceneSnap& S, uint8_t screen);   // screens 3-5, below

static void drawSceneScreen(uint8_t screen) {
  static SceneSnap snap = {};
  static uint32_t  nF = 0, sum = 0, mx = 0, lastLog = 0;
  uint32_t t0 = micros();
  sceneSnapFill(snap);
  if (screen == 2) sceneLogFill(snap);
  sceneScreenFill(snap, screen);
  sceneDraw(screen, snap, millis(), (uint16_t*)canvas.getBuffer());
  uint32_t dt = micros() - t0, now = millis();
  nF++; sum += dt; if (dt > mx) mx = dt;
  if (now - lastLog >= 30000) {
    Log.notice("LCD scene %u: %u frames/30s draw avg=%uus max=%uus",
               (unsigned)screen, (unsigned)nF, (unsigned)(sum / nF), (unsigned)mx);
    nF = sum = mx = 0;
    lastLog = now;
  }
}

// ── Screen 2: the chronicle ────────────────────────────────────
// A comic page now (scChronicleDraw in ui-scenes.hpp): the last three entries,
// each a drawn panel picked by its margin mark. What stays here is the part
// that needs the board -- turning a log entry into the sentence the page
// shows (names are filled in at draw time, because drainEvents() cannot take
// G.mutex to read them when the entry is logged) and the plates' figures.
static_assert(GLY_COUNT == SC_GLY_COUNT && GLY_FOG == SC_GLY_FOG && GLY_MEDAL == SC_GLY_MEDAL,
              "SC_GLY_* in ui-scenes.hpp mirrors K10Glyph");
static_assert(TONE_OMEN == SC_TONE_OMEN && TONE_GOOD == SC_TONE_GOOD, "SC_TONE_* mirrors K10Tone");

// Name to write for a player: their own if they have one, else a stand-in.
static const char* bookName(int8_t pid, const char names[][16]) {
  static char fallback[12];
  if (pid < 0 || pid >= MAX_PLAYERS) return "Someone";
  if (names[pid][0]) return names[pid];
  snprintf(fallback, sizeof(fallback), "Walker %d", (int)pid + 1);
  return fallback;
}

// Expand one entry into the whole sentence the page shows.
static void bookSentence(const K10LogEntry& e, const char names[][16],
                         char* out, size_t cap) {
  size_t n = 0;
  if (e.who >= 0) {
    const char* nm = bookName(e.who, names);
    while (*nm && n + 2 < cap) out[n++] = *nm++;
    if (n + 2 < cap) out[n++] = ' ';
  }
  for (const char* p = e.text; *p && n + 1 < cap; p++) {
    if (*p == '\x01') {
      const char* nm = bookName(e.who2, names);
      while (*nm && n + 1 < cap) out[n++] = *nm++;
    } else {
      out[n++] = *p;
    }
  }
  out[n] = '\0';
}

// The plates: moments too big for a line, which used to be pressed into the
// book as framed blocks. On the page each is a panel like any other; the
// table says which picture it gets, and pv[] carries its figures.
struct BookPlateDef { const char* title; uint8_t glyph; char rule; };
static const BookPlateDef BOOK_PLATE[PLATE_COUNT] = {
  { nullptr,     GLY_NONE,  '-' },   // NONE  — never drawn
  { "WOUNDS",    GLY_WOUND, '-' },   // pv: minor, major, LL now
  { "RADIATION", GLY_RAD,   '-' },   // pv: dose, level now, level max
  { "HAUL",      GLY_HAUL,  '-' },   // pv: the five resources carried out
  { "COMMENDED", GLY_MEDAL, '=' },   // pv: K10Award; text is the citation
};

// The commendations, in K10Award order. The citation under each one is
// written at the call site, because only the call site knows the numbers.
static const char* const BOOK_AWARD[AWD_COUNT] = {
  "Swept the Place",
  "Heavy Load",
  "Off the Scent",
  "Long Odds",
};

// "[####......]" — a dose read off at a glance.
static void bookPlateMeter(char* out, size_t cap, uint8_t v, uint8_t vmax) {
  const uint8_t CELLS = 10;
  uint8_t on = 0;
  if (vmax > 0) {
    uint16_t f = ((uint16_t)v * CELLS + vmax - 1) / vmax;
    on = (f > CELLS) ? CELLS : (uint8_t)f;
  }
  size_t n = 0;
  if (cap) out[n++] = '[';
  for (uint8_t i = 0; i < CELLS && n + 2 < cap; i++) out[n++] = (i < on) ? '#' : '.';
  if (n + 1 < cap) out[n++] = ']';
  out[n] = '\0';
}

// Fill a plate's two figure lines from pv[], per kind.
static void bookPlateFigures(const K10LogEntry& e, const char names[][16],
                             char* a, char* b, size_t cap) {
  const char* who = (e.who >= 0) ? bookName(e.who, names) : "The party";
  a[0] = b[0] = '\0';
  switch (e.plate) {
    case PLATE_WOUND:
      if (e.text[0]) snprintf(a, cap, "%s, %s", who, e.text);
      else           snprintf(a, cap, "%s", who);
      snprintf(b, cap, "minor %u  major %u   LL %u",
               (unsigned)e.pv[0], (unsigned)e.pv[1], (unsigned)e.pv[2]);
      break;
    case PLATE_RAD: {
      char bar[16];
      bookPlateMeter(bar, sizeof(bar), e.pv[1], e.pv[2]);
      snprintf(a, cap, "%s takes %u rad%s", who,
               (unsigned)e.pv[0], (e.pv[0] == 1) ? "" : "s");
      snprintf(b, cap, "%s %u of %u", bar, (unsigned)e.pv[1], (unsigned)e.pv[2]);
      break;
    }
    case PLATE_HAUL: {
      unsigned all = 0;
      for (uint8_t i = 0; i < 5; i++) all += e.pv[i];
      snprintf(a, cap, "WTR %u  FOD %u  FUL %u",
               (unsigned)e.pv[0], (unsigned)e.pv[1], (unsigned)e.pv[2]);
      snprintf(b, cap, "MED %u  SCR %u      all %u",
               (unsigned)e.pv[3], (unsigned)e.pv[4], all);
      break;
    }
    case PLATE_AWARD:
      snprintf(a, cap, "%s", (e.pv[0] < AWD_COUNT) ? BOOK_AWARD[e.pv[0]] : "Noted");
      if (e.text[0]) snprintf(b, cap, "%s, %s", who, e.text);
      else           snprintf(b, cap, "%s", who);
      break;
    default:
      break;
  }
}

// The last three entries, written out, into the scene's snapshot. Names come
// from the snapshot itself (sceneSnapFill runs first).
static void sceneLogFill(SceneSnap& S) {
  K10LogEntry e[3];
  uint8_t  n;
  uint16_t total;
  taskENTER_CRITICAL(&k10LogMux);
  n = k10LogCount < 3 ? k10LogCount : 3;
  for (uint8_t i = 0; i < n; i++)
    e[i] = k10Log[(k10LogHead + k10LogCount - n + i) % K10_LOG_SIZE];
  total = k10LogTotal;
  taskEXIT_CRITICAL(&k10LogMux);

  char names[MAX_PLAYERS][16];
  for (int i = 0; i < MAX_PLAYERS; i++) memcpy(names[i], S.s[i].name, 16);
  for (uint8_t i = 0; i < n; i++) {
    SceneLog& L = S.log[i];
    const K10LogEntry& E = e[i];
    if (E.plate != PLATE_NONE && E.plate < PLATE_COUNT) {
      char a[64], b[64];
      bookPlateFigures(E, names, a, b, sizeof(a));
      const char* who = (E.who >= 0) ? bookName(E.who, names) : "The party";
      if (E.plate == PLATE_HAUL)       snprintf(L.text, sizeof(L.text), "%s carries it all out. %s %s", who, a, b);
      else if (E.plate == PLATE_AWARD) snprintf(L.text, sizeof(L.text), "Commended: %s. %s", a, b);
      else                             snprintf(L.text, sizeof(L.text), "%s. %s", a, b);
      L.glyph = BOOK_PLATE[E.plate].glyph;
    } else {
      bookSentence(E, names, L.text, sizeof(L.text));
      L.glyph = E.glyph < GLY_COUNT ? E.glyph : (uint8_t)GLY_NONE;
    }
    L.tone   = E.tone < TONE_COUNT ? E.tone : (uint8_t)TONE_PLAIN;
    L.day    = E.day;
    L.who    = E.who;
    L.who2   = E.who2;
    L.serial = (uint16_t)(total - (n - 1 - i));
  }
  S.logN     = n;
  S.logTotal = total;
}

// -- Screen 3: the stash --------------------------------------
// A scene (scStashDraw): one survivor per page, turning every RES_PAGE_MS
// through whoever is online. The board picks the page, because only the board
// can read the pack's item names (getItemDef) to put them on the manifest.
static constexpr uint32_t RES_PAGE_MS = 8000;

// ── Screen 4: the odds ─────────────────────────────────────────
// A scene now (scOddsDraw: the strength-tester). What stays here are the
// tallies it reads. The old layout notes below describe what they count.
// What the encounter system is doing right now, not just a leaderboard:
// sites left on the map, the party's hit rate, the most recent roll in full
// (who, skill, target number, what they rolled), and per survivor whether
// they are inside an encounter (which biome, how much unbanked loot is at
// stake) or, if not, their own record and how their last one ended.
//
//   y   0-29   header band   "ENCOUNTERS" 26 px
//   y  31-49   strip         SITES left/total      WON wins/rolls (right)
//   y  54-88   last roll     [n] skill 16 px       WON / LOST 26 px (right)
//                            NEEDED n   ROLLED n
//   y  96-317  6 cards x 37  [n] name 16 px        ENTERED count (right)
//                            INSIDE biome  CARRYING n  |  WON w/r  last end
//
// Same three faces as screens 1 and 3. The verdict gets the 26 px face on its
// own, because it is the one thing here that is a yes or a no -- everything
// else is a count you have to read. And with labels at 6 px there was room to
// stop abbreviating: POI became SITES, ENC became ENTERED, NEED/GOT became
// NEEDED/ROLLED, and IN/LOOT became INSIDE/CARRYING.
//
// The tallies come from the event stream: drainEvents() in
// network-events.hpp calls encStatsNote() for every event it drains (that is
// the one place every EVT_ENC_* passes through). Counters are session-only —
// they are not saved and start at zero after a reboot.

struct EncStats {
  uint16_t rolls, wins;                 // party-wide
  bool     hasLast;
  uint8_t  lastPid, lastSkill, lastDN;
  int8_t   lastTotal;
  uint8_t  lastOut;                     // 1 = success
  struct {
    uint8_t rolls, wins;
    uint8_t lastEnd;                    // ENC_END_* reason
    bool    hasEnd;
  } p[MAX_PLAYERS];
};
static EncStats     encStats    = {};
static portMUX_TYPE encStatsMux = portMUX_INITIALIZER_UNLOCKED;

// Called once per drained GameEvent, from whichever task runs drainEvents().
static void encStatsNote(const GameEvent& ev) {
  if (ev.type != EVT_ENC_RESULT && ev.type != EVT_ENC_END) return;
  if (ev.pid >= MAX_PLAYERS) return;
  taskENTER_CRITICAL(&encStatsMux);
  if (ev.type == EVT_ENC_RESULT) {
    encStats.rolls++;
    if (ev.encOut) encStats.wins++;
    if (encStats.p[ev.pid].rolls < 255) encStats.p[ev.pid].rolls++;
    if (ev.encOut && encStats.p[ev.pid].wins < 255) encStats.p[ev.pid].wins++;
    encStats.hasLast   = true;
    encStats.lastPid   = ev.pid;
    encStats.lastSkill = ev.encSkill;
    encStats.lastDN    = ev.encDN;
    encStats.lastTotal = ev.encTotal;
    encStats.lastOut   = ev.encOut ? 1 : 0;
  } else {
    encStats.p[ev.pid].lastEnd = ev.encOut;
    encStats.p[ev.pid].hasEnd  = true;
  }
  taskEXIT_CRITICAL(&encStatsMux);
}

// ── Screens 3-5: the rest of the snapshot ──────────────────────
// Filled only for the screen that needs it: the stash's item names, the
// odds' tallies and whoever is inside, the map's terrain. Defined down here
// because it reads EncStats (above) -- drawSceneScreen forward-declares it.
static void sceneScreenFill(SceneSnap& S, uint8_t screen) {
  if (screen == 3) {
    int online[MAX_PLAYERS], cnt = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) if (S.s[i].on) online[cnt++] = i;
    S.page  = cnt ? (int8_t)online[(millis() / RES_PAGE_MS) % (uint32_t)cnt] : (int8_t)-1;
    S.itemN = 0;
    if (S.page >= 0 && xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      const Player& p = G.players[S.page];
      uint8_t slots = effectiveInvSlots(p);
      for (int k = 0; k < slots && k < INV_SLOTS_MAX && S.itemN < 18; k++) {
        if (!p.invType[k]) continue;
        const ItemDef* def = getItemDef(p.invType[k]);
        strlcpy(S.item[S.itemN], def ? def->name : "unknown", sizeof(S.item[0]));
        S.itemQty[S.itemN] = p.invQty[k];
        S.itemN++;
      }
      xSemaphoreGive(G.mutex);
    }
  } else if (screen == 4) {
    EncStats st;
    taskENTER_CRITICAL(&encStatsMux);
    st = encStats;
    taskEXIT_CRITICAL(&encStatsMux);
    S.rolls = st.rolls; S.wins = st.wins;
    S.hasRoll = st.hasLast; S.rollWon = st.lastOut != 0;
    S.rollWho = (int8_t)st.lastPid; S.rollSkill = st.lastSkill; S.rollDN = st.lastDN; S.rollTotal = st.lastTotal;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      S.s[i].encRolls = st.p[i].rolls; S.s[i].encWins = st.p[i].wins;
      S.s[i].lastEnd = st.p[i].lastEnd; S.s[i].hasEnd = st.p[i].hasEnd;
    }
    // Every encounter file is placed once at map generation, so the pool
    // sizes are the number of sites the map started with.
    S.poiTotal = 0;
    for (int t = 0; t < 10; t++) S.poiTotal += encPools[t].count;
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      uint16_t left = 0;
      for (int r = 0; r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) if (G.map[r][c].poi) left++;
      S.poiLeft = left;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        SceneSurv& v = S.s[i];
        v.encCount = G.players[i].encCount;
        const ActiveEncounter& e = encounters[i];
        v.biome[0] = 0; v.loot = 0;
        if (v.inEnc) {
          uint8_t t = e.terrain < ENC_POOL_COUNT ? e.terrain : 0;   // the trap pool sits past the terrains
          strlcpy(v.biome, encPools[t].path, sizeof(v.biome));
          int l = e.pendingItemCount;
          for (int k = 0; k < 5; k++) l += e.pendingLoot[k];
          v.loot = (uint8_t)min(99, l);
        }
      }
      xSemaphoreGive(G.mutex);
    }
  } else if (screen == 5) {
    PSRAM_STATIC(uint8_t, terr, [MAP_ROWS][MAP_COLS]);
    WorldMarkers wm = {};
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      for (int r = 0; r < MAP_ROWS; r++)
        for (int c = 0; c < MAP_COLS; c++) terr[r][c] = G.map[r][c].terrain;
      snapshotWorldMarkers(wm);   // W lives under G.mutex too
      xSemaphoreGive(G.mutex);
      S.caravanOn = wm.caravanActive; S.caravanQ = wm.caravanQ; S.caravanR = wm.caravanR;
      S.doomQ = wm.doomQ; S.doomR = wm.doomR; S.doomRadius = wm.doomRadius;
      S.doomAware = wm.doomAwareness;
    }
    S.terr = &terr[0][0];
    // The Understory's published copy: read without the lock, as it always was.
    S.eco  = ecoPublishedDensity();
    S.cols = MAP_COLS; S.rows = MAP_ROWS;
    S.hatchN = 0;
    for (int i = 0; i < hatchCount && S.hatchN < 8; i++) {
      S.hatchQ[S.hatchN] = bunkerHatches[i].sq; S.hatchR[S.hatchN] = bunkerHatches[i].sr; S.hatchN++;
    }
  }
}

// ── Screen dispatch ────────────────────────────────────────────────────────
// Renders whichever screen button B has landed on into the canvas. Does not
// push — the caller decides whether the frame goes out clean or as one step
// of the switch transition below.
static void drawActiveScreen() {
  drawSceneScreen(k10Screen);   // every screen is a picture now (ui-scenes.hpp)
}

// ── Screen switch transition: fade out, jitter, fade in ────────────────────
// The panel backlight isn't on a PWM we can borrow mid-loop, so the fade is
// done on the sprite instead: fillRectAlpha() blends the whole canvas toward
// black before each push. That works cumulatively on the way *out* — the
// outgoing screen is still sitting in the canvas from the last push, so no
// redraw is needed — but not on the way in: brightening an already-blended
// buffer is lossy, so each fade-in step re-renders the new screen and blends
// that down to the step's darkness. The step counts are deliberately small.
// A full-screen alpha blend is a per-pixel read-modify-write over a 240×320
// PSRAM sprite, and that, not the delays, is what this costs.

// Push the canvas with a per-band horizontal offset so the frame lands torn
// rather than square — the switch reads as a tube dropout rather than a
// clean crossfade. `mag` is the worst-case offset in pixels; 0 pushes clean.
// The bands are clipped on the *display*, not the sprite, so the sprite is
// never disturbed and the next fade step still has a whole screen to blend.
// Whatever a band's offset uncovers is blacked out, or it would keep showing
// a sliver of the frame before it.
static void pushCanvasJittered(int mag) {
  if (mag <= 0) { canvas.pushSprite(0, 0); return; }
  const int BANDS = 5;
  const int BH    = 320 / BANDS;
  for (int b = 0; b < BANDS; b++) {
    int y  = b * BH;
    int h  = (b == BANDS - 1) ? 320 - y : BH;
    int dx = (int)random(-mag, mag + 1);
    tft.setClipRect(0, y, 240, h);
    canvas.pushSprite(&tft, dx, 0);
    tft.clearClipRect();
    if      (dx > 0) tft.fillRect(0, y, dx, h, 0);
    else if (dx < 0) tft.fillRect(240 + dx, y, -dx, h, 0);
  }
}

static void screenSwitchTransition() {
  // Out: cumulative blends toward black, the jitter widening as it goes.
  for (int i = 0; i < 3; i++) {
    canvas.fillRectAlpha(0, 0, 240, 320, 120, (uint16_t)0);
    pushCanvasJittered(1 + i * 2);
  }
  tft.fillScreen(0);
  delay(30);   // the beat the switch hangs on

  // In: re-render per step, blended down to that step's darkness, settling
  // to a clean unjittered push on the last one.
  static const uint8_t IN_ALPHA[3]  = {170, 80, 0};
  static const int8_t  IN_JITTER[3] = {4, 2, 0};
  for (int i = 0; i < 3; i++) {
    drawActiveScreen();
    if (IN_ALPHA[i]) canvas.fillRectAlpha(0, 0, 240, 320, IN_ALPHA[i], (uint16_t)0);
    pushCanvasJittered(IN_JITTER[i]);
  }
}

// ── K10 button B screen switching ──────────────────────────────────────────
// Screens: 1=The road 2=The chronicle 3=The stash 4=The odds 5=The waste 6=The admired
static void checkGestureSwitch() {
  bool btnB = k10.buttonB && k10.buttonB->isPressed();
  if (btnB && !k10BtnBLast) {
    k10Screen = (k10Screen % 6) + 1;
    k10ScreenXition = true;   // consumed by the display block in the .ino loop
    k10Play(MOTIF_SCREEN_CLICK);
  }
  k10BtnBLast = btnB;
}

// ── K10 button A: speaker volume ───────────────────────────────────────────
// Button A is only read at boot (hold it for the USB drive), so in play it is
// free. Each press steps the speaker LOW -> MED -> HIGH -> OFF and round
// again, and says which in a toast over the screen. A level set elsewhere
// (the web slider, the sound desk) steps up to the next preset above it.
// Saved with the other K10 prefs as `vol`, the same 0..9 the slider writes.
static const uint8_t VOL_LOW = 2, VOL_MED = 5, VOL_HIGH = 9;
static void checkFxButton(uint32_t now) {
  bool a = k10.buttonA && k10.buttonA->isPressed();
  if (a && !fxBtnALast) {
    uint8_t v = s_audioVol;
    s_audioVol = v == 0 ? VOL_LOW : v < VOL_MED ? VOL_MED : v < VOL_HIGH ? VOL_HIGH : 0;
    const char* nm = s_audioVol == 0 ? "OFF" : s_audioVol == VOL_LOW ? "LOW" : s_audioVol == VOL_MED ? "MED" : "HIGH";
    Log.notice("ButtonA: volume %s (%d)", nm, (int)s_audioVol);
    if (fxReady) {
      snprintf(FX.toastMsg, sizeof(FX.toastMsg), "VOLUME  %s", nm);
      FX.toastUntil = now + 1600;
    }
    saveK10Prefs();
    k10Play(MOTIF_SCREEN_CLICK);   // heard at the new level; silent at OFF
    k10Dirty = true;
  }
  fxBtnALast = a;
}
