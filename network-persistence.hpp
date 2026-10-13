#pragma once
// ── Persistence: SD card save / load ─────────────────────────────────────────
// Included from Esp32HexMapCrawl.ino after actions_game_loop.hpp.
// Has access to all globals, constants, and structs defined above it.

// Replace any non-printable ASCII byte with '_' so names are safe to embed in JSON.
static void sanitizeName(char* s, int maxLen) {
  for (int i = 0; i < maxLen && s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c > 0x7E) s[i] = '_';
  }
}

// ── Trail sides (SAVE_TRL_F) ─────────────────────────────────────────────────
// Sparse: nearly every (hex, walker) has no record, so the file is a list of
// the cells that have one -- uint16 cell index (r * MAP_COLS + q), a uint8
// mask of which slots follow, then one trails.hpp byte per set bit. A world
// walked for days is a few KB; every slot of every hex would be ~42 KB.
struct __attribute__((packed)) SaveTrailHdr {
  uint32_t magic;
  uint8_t  version;
  uint8_t  cols, rows, slots;   // a file for another board shape is ignored
  uint16_t cells;
};

// Inside saveGame()'s G.mutex hold, and only when something moved since the
// last write: a dawn with nobody walking costs nothing.
static void saveTrailFile() {
  if (!trailDirty || !trailSides) return;
  File f = SD.open(SAVE_TRL_F, FILE_WRITE);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_TRL_F); return; }
  SaveTrailHdr h = { TRAIL_SAVE_MAGIC, 1, (uint8_t)MAP_COLS, (uint8_t)MAP_ROWS, (uint8_t)TRAIL_SLOTS, 0 };
  for (int r = 0; r < MAP_ROWS; r++)
    for (int q = 0; q < MAP_COLS; q++)
      for (int s = 0; s < TRAIL_SLOTS; s++)
        if (trailSides[r][q][s] != TRAIL_NONE) { h.cells++; break; }
  size_t bytes = f.write((uint8_t*)&h, sizeof(h));
  uint8_t chunk[512];
  int n = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      uint8_t mask = 0;
      for (int s = 0; s < TRAIL_SLOTS; s++) if (trailSides[r][q][s] != TRAIL_NONE) mask |= (uint8_t)(1 << s);
      if (!mask) continue;
      if (n + 3 + TRAIL_SLOTS > (int)sizeof(chunk)) { bytes += f.write(chunk, n); n = 0; }
      uint16_t idx = (uint16_t)(r * MAP_COLS + q);
      chunk[n++] = (uint8_t)(idx & 0xFF);
      chunk[n++] = (uint8_t)(idx >> 8);
      chunk[n++] = mask;
      for (int s = 0; s < TRAIL_SLOTS; s++) if (mask & (1 << s)) chunk[n++] = trailSides[r][q][s];
    }
  }
  if (n) bytes += f.write(chunk, n);
  f.close();
  trailDirty = false;
  Log.verbose("SD WRITE: %s bytes=%u cells=%u", SAVE_TRL_F, (unsigned)bytes, (unsigned)h.cells);
}

// Called by tryLoadSave() once map.bin has loaded: these sides belong to that
// map's footprint bits. Without the file (a save older than it) every record
// is "none" and clients infer the directions, as they did before it existed.
// A byte that is not a well-formed record is dropped rather than trusted.
static void loadTrailFile() {
  trailClearAll();
  trailDirty = false;   // memory now matches whatever is (or is not) on the card
  if (!SD.exists(SAVE_TRL_F)) { Log.notice("No %s: trail sides start empty", SAVE_TRL_F); return; }
  File f = SD.open(SAVE_TRL_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_TRL_F); return; }
  SaveTrailHdr h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != TRAIL_SAVE_MAGIC || h.version != 1 ||
      h.cols != MAP_COLS || h.rows != MAP_ROWS || h.slots != TRAIL_SLOTS) {
    Log.warning("%s header unreadable or for another board - ignoring", SAVE_TRL_F);
    f.close();
    return;
  }
  auto side = [](uint8_t v) { return v <= 5 || v == TRAIL_NO_SIDE; };
  int loaded = 0;
  for (int i = 0; i < (int)h.cells; i++) {
    uint8_t rec[3 + TRAIL_SLOTS];
    if (f.read(rec, 3) != 3) break;
    int idx = rec[0] | (rec[1] << 8);
    uint8_t mask = rec[2];
    int want = 0;
    for (int s = 0; s < TRAIL_SLOTS; s++) if (mask & (1 << s)) want++;
    if (idx >= MAP_ROWS * MAP_COLS || (mask >> TRAIL_SLOTS) || f.read(rec + 3, want) != want) break;
    int k = 3;
    for (int s = 0; s < TRAIL_SLOTS; s++) {
      if (!(mask & (1 << s))) continue;
      uint8_t b = rec[k++];
      if ((b & 0x80) || !side(b & 7) || !side((b >> 3) & 7)) continue;
      trailSides[idx / MAP_COLS][idx % MAP_COLS][s] = b;
      loaded++;
    }
  }
  f.close();
  Log.notice("SD READ: %s records=%d", SAVE_TRL_F, loaded);
}

// ── Tunnel section lights (SAVE_LIT_F) ───────────────────────────────────────
// Two whole-board byte blocks, G.tunnelSec then G.tunnelLit (tunnels.hpp,
// "Section lights"), 320 bytes at 16x10 -- small enough to write every save.
struct __attribute__((packed)) SaveLightHdr {
  uint32_t magic;
  uint8_t  version;
  uint8_t  cols, rows;   // a file for another board shape is ignored
};

// Inside saveGame()'s G.mutex hold.
static void saveLightFile() {
  File f = SD.open(SAVE_LIT_F, FILE_WRITE);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_LIT_F); return; }
  SaveLightHdr h = { LIGHT_SAVE_MAGIC, 1, (uint8_t)TUN_COLS, (uint8_t)TUN_ROWS };
  size_t bytes = f.write((uint8_t*)&h, sizeof(h));
  bytes += f.write((uint8_t*)G.tunnelSec, TUNNEL_LIGHT_BYTES);
  bytes += f.write((uint8_t*)G.tunnelLit, TUNNEL_LIGHT_BYTES);
  f.close();
  Log.verbose("SD WRITE: %s bytes=%u", SAVE_LIT_F, (unsigned)bytes);
}

// Called by tryLoadSave() once the tunnel board has loaded. No file -- a save
// from before the lights -- or one for another board lays the sections out
// afresh on the loaded one, panels and all, every light off. "Another board"
// is a malformed byte anywhere, or a layout that disagrees with the floor on
// more than a quarter of its cells; a few cells off (nothing collapses a
// corridor today, but a reader should not have to know that) are just put
// right: a section on rock is cleared, and floor with no section stays dark.
static void loadLightFile() {
  bool ok = false;
  if (!SD.exists(SAVE_LIT_F)) {
    Log.notice("No %s: laying out tunnel sections on the loaded board", SAVE_LIT_F);
  } else if (File f = SD.open(SAVE_LIT_F, FILE_READ)) {
    SaveLightHdr h;
    ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == LIGHT_SAVE_MAGIC &&
         h.version == 1 && h.cols == TUN_COLS && h.rows == TUN_ROWS &&
         f.read((uint8_t*)G.tunnelSec, TUNNEL_LIGHT_BYTES) == TUNNEL_LIGHT_BYTES &&
         f.read((uint8_t*)G.tunnelLit, TUNNEL_LIGHT_BYTES) == TUNNEL_LIGHT_BYTES;
    f.close();
    int cells = 0, off = 0;
    for (int r = 0; ok && r < TUN_ROWS; r++)
      for (int q = 0; ok && q < TUN_COLS; q++) {
        uint8_t s = G.tunnelSec[r][q], lt = G.tunnelLit[r][q];
        if (s == TUN_SEC_NONE) { ok = (lt == 0); if (tunLightable(q, r)) off++; continue; }
        ok = s < TUN_SECTIONS_MAX && (lt & 0x80) == 0;
        cells++;
        if (!tunLightable(q, r)) { off++; G.tunnelSec[r][q] = TUN_SEC_NONE; G.tunnelLit[r][q] = 0; }
      }
    if (ok && off * 4 > cells) ok = false;
    if (!ok) Log.warning("%s unreadable or for another board -- laying sections out afresh", SAVE_LIT_F);
  } else {
    Log.error("SD OPEN FAIL (read): %s", SAVE_LIT_F);
  }
  if (!ok) { tunnelPlaceSections(); return; }
  int total = 0, lit = 0;
  tunnelSectionCounts(&total, &lit);
  Log.notice("SD READ: %s sections=%d lit=%d", SAVE_LIT_F, total, lit);
}

// ── SD Save ───────────────────────────────────────────────────────────────────
void saveGame() {
  if (SD.cardType() == CARD_NONE) { Log.warning("saveGame: no SD card"); return; }
  LOG_FN();
  uint32_t _t0 = millis();
  bool _got = (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(100)) == pdTRUE);
  if (_got) {
    if (!SD.exists(SAVE_DIR)) {
      Log.notice("SD mkdir: %s", SAVE_DIR);
      SD.mkdir(SAVE_DIR);
    }
    // Map file.  Active encounters are not persisted, so their POIs are put
    // back on the map for the duration of the write — after a reboot nobody is
    // mid-encounter and the hex is visitable again instead of being lost.
    size_t mapBytes = 0;
    File f = SD.open(SAVE_MAP_F, FILE_WRITE);
    if (f) {
      SaveHeader hdr = {};
      hdr.magic = SAVE_MAGIC; hdr.version = SAVE_VERSION;
      hdr.dayCount = G.dayCount; hdr.threatClock = G.threatClock;
      hdr.weatherPhase = G.weatherPhase; hdr.weatherCounter = G.weatherCounter;
      hdr.dayTick = G.dayTick; hdr.badWeatherTicks = G.badWeatherTicks;
      hdr.caravanQ = W.caravan.q; hdr.caravanR = W.caravan.r;
      hdr.caravanRestockTimer = W.caravan.restockTimer;
      memcpy(hdr.caravanInv, W.caravan.inv, 5);
      hdr.caravanActive = W.caravan.active ? 1 : 0;
      memcpy(hdr.caravanStockItem, W.caravan.stockItem, CARAVAN_STOCK_SLOTS);
      memcpy(hdr.caravanStockQty,  W.caravan.stockQty,  CARAVAN_STOCK_SLOTS);
      hdr.doomQ = W.creepingDoom.q; hdr.doomR = W.creepingDoom.r;
      hdr.doomAwareness = W.creepingDoom.awareness;
      // v17: the surface<->shaft pairing cannot be recovered from the two
      // boards alone, so it rides the header alongside the tunnel block.
      memcpy(hdr.bunkerHatches, bunkerHatches, sizeof(hdr.bunkerHatches));
      hdr.hatchCount = hatchCount;
      // On the board the scene is on (a tunnel panel's hex shares its q/r
      // with some surface hex), and never for a trap, whose encIdx is a trap
      // file and which has no POI to put back -- traps.hpp owns that hex. A
      // panel whose section the scene has already lit stays spent.
      // A Kane chapter has no POI either: its camp and stage live in kane.bin.
      auto savedHex = [](const ActiveEncounter& e) -> HexCell* {
        if (!e.active || e.trap || e.kane || e.vac) return nullptr;
        if (e.depth) return tunnelPanelDark(e.hexQ, e.hexR) ? &G.tunnel[e.hexR][e.hexQ] : nullptr;
        return &G.map[e.hexR][e.hexQ];
      };
      uint8_t savedPoi[MAX_PLAYERS] = {0};
      for (int i = 0; i < MAX_PLAYERS; i++) {
        HexCell* c = savedHex(encounters[i]);
        if (!c) continue;
        savedPoi[i] = c->poi;
        c->poi = encounters[i].encIdx;
      }
      mapBytes += f.write((uint8_t*)&hdr, sizeof(hdr));
      mapBytes += f.write((uint8_t*)G.map, MAP_BYTES);
      // v17: tunnel board, immediately after the surface map. tryLoadSave()
      // reads these blocks back in the same order; the ground-items block
      // still follows and is still read until EOF.
      mapBytes += f.write((uint8_t*)G.tunnel, TUNNEL_BYTES);
      // v20: which sides of each tunnel cell are open (tunnels.hpp). The
      // board's walls live here, not in HexCell, so without it every
      // corridor would load sealed.
      mapBytes += f.write((uint8_t*)G.tunnelOp, TUNNEL_OP_BYTES);
      for (int i = 0; i < MAX_PLAYERS; i++)
        if (HexCell* c = savedHex(encounters[i])) c->poi = savedPoi[i];
      f.close();
      Log.notice("SD WRITE: %s bytes=%u", SAVE_MAP_F, (unsigned)mapBytes);
    } else {
      Log.error("SD OPEN FAIL (write): %s", SAVE_MAP_F);
    }
    // Players file
    size_t plyBytes = 0;
    File p = SD.open(SAVE_PLY_F, FILE_WRITE);
    if (p) {
      for (int i = 0; i < MAX_PLAYERS; i++) {
        Player& pl = G.players[i];
        SavePlayer sp = {};
        memcpy(sp.name, pl.name, 16);
        sp.archetype = pl.archetype;
        memcpy(sp.skills, pl.skills, NUM_SKILLS);
        sp.q = pl.q; sp.r = pl.r;
        sp.ll = pl.ll; sp.food = pl.food; sp.water = pl.water;
        sp.radiation = pl.radiation;
        memcpy(sp.inv, pl.inv, 5);
        memcpy(sp.invType, pl.invType, INV_SLOTS_MAX);
        memcpy(sp.invQty,  pl.invQty,  INV_SLOTS_MAX);
        memcpy(sp.equip,  pl.equip,  EQUIP_SLOTS);
        sp.invSlots = pl.invSlots;
        sp.score = pl.score; sp.steps = pl.steps;
        sp.encCount     = pl.encCount;
        sp.apLeft    = pl.apLeft;
        sp.fThreshBelow = pl.fThreshBelow;
        sp.wThreshBelow = pl.wThreshBelow;
        memcpy(sp.wounds, pl.wounds, NUM_WOUND_TIER);
        memcpy(sp.surveyedMap, pl.surveyedMap, sizeof(sp.surveyedMap));
        sp.radClean     = pl.radClean ? 1 : 0;
        sp.llCapPenalty = pl.llCapPenalty;
        sp.knownRecipes = pl.knownRecipes;
        sp.depth = pl.depth; sp.tq = pl.tq; sp.tr = pl.tr; sp.hatchIdx = pl.hatchIdx;
        sp.used = (pl.name[0] != '\0') ? 1 : 0;
        plyBytes += p.write((uint8_t*)&sp, sizeof(sp));
      }
      p.close();
      Log.notice("SD WRITE: %s bytes=%u players=%d", SAVE_PLY_F,
                 (unsigned)plyBytes, (int)MAX_PLAYERS);
    } else {
      Log.error("SD OPEN FAIL (write): %s", SAVE_PLY_F);
    }
    // Ground items — append to map file as a fixed-size block
    File gf = SD.open(SAVE_MAP_F, FILE_APPEND);
    if (gf) {
      size_t giBytes = 0;
      for (int g = 0; g < MAX_GROUND; g++) {
        SaveGroundItem sgi = { groundItems[g].q, groundItems[g].r,
                               groundItems[g].itemType, groundItems[g].qty };
        giBytes += gf.write((uint8_t*)&sgi, sizeof(sgi));
      }
      gf.close();
      LOG_VERBOSE("SD APPEND: %s ground=%u bytes", SAVE_MAP_F, (unsigned)giBytes);
    } else {
      Log.error("SD OPEN FAIL (append): %s", SAVE_MAP_F);
    }
    // Ground ages + remains records -- their own file, see SAVE_GND_F. The
    // days are in groundItems[] order, the same order as the block above.
    File gd = SD.open(SAVE_GND_F, FILE_WRITE);
    if (gd) {
      SaveGroundHdr gh = { GROUND_SAVE_MAGIC, 1, MAX_GROUND, 0 };
      for (int i = 0; i < MAX_REMAINS; i++) if (remainsTable[i].used) gh.nRemains++;
      uint16_t days[MAX_GROUND];
      for (int g = 0; g < MAX_GROUND; g++) days[g] = groundItems[g].day;
      size_t gdBytes = gd.write((uint8_t*)&gh, sizeof(gh));
      gdBytes += gd.write((uint8_t*)days, sizeof(days));
      for (int i = 0; i < MAX_REMAINS; i++) {
        const Remains& rm = remainsTable[i];
        if (!rm.used) continue;
        SaveRemains sr = {};
        sr.pid = rm.pid; sr.q = rm.q; sr.r = rm.r; sr.day = rm.day;
        memcpy(sr.res,  rm.res,  sizeof(sr.res));
        memcpy(sr.name, rm.name, sizeof(sr.name));
        gdBytes += gd.write((uint8_t*)&sr, sizeof(sr));
      }
      gd.close();
      Log.verbose("SD WRITE: %s bytes=%u remains=%u", SAVE_GND_F,
                  (unsigned)gdBytes, (unsigned)gh.nRemains);
    } else {
      Log.error("SD OPEN FAIL (write): %s", SAVE_GND_F);
    }
    // Which way everyone went (trails.hpp): its own file, only when it changed.
    saveTrailFile();
    // The tunnel sections and their lights (tunnels.hpp).
    saveLightFile();
    // Machine Gun Kane (kane.hpp): the jeep and the arc, a few dozen bytes.
    saveKaneFile();
    // VACANCY (vacancy.hpp): the sign, its count, its two sites.
    saveVacFile();
    // The Understory's scars (/save/scar.bin, ecology.hpp): only when the Eco
    // task has published a change, and on this same serialised save path.
    ecoSaveScarsIfDirty();
    xSemaphoreGive(G.mutex);
    Log.notice("saveGame complete took=%ums", (unsigned)(millis() - _t0));
  } else {
    Log.warning("saveGame: G.mutex timeout (100ms) — skipped");
  }
}

// ── Deferred save ─────────────────────────────────────────────────────────────
// WS handlers run on the async_tcp task, and a save is 110-150 ms of SD writes
// with G.mutex held. When a 5-bot run ended on 2026-09-24 the five disconnects
// each saved inline, ~0.6 s of the network task spent on the SD card, and the
// network wedged at that moment. So handlers only ask: gameLoopTask saves after
// its next tick (<= TICK_MS later), and asks in between coalesce into one save.
// A request that lands while a save is running is kept for the next save.
//
// 2026-10-13: the saves moved off GameLoop to their own task on core 0. On
// GameLoop (core 1, priority 2) the ~200 ms of polled SD writes preempted the
// display loop (loopTask, core 1, priority 1) mid-frame, and every save was a
// 250 ms freeze on the LCD -- one every couple of seconds in a bot run. Core 0
// has the radio and the synth above the Saver and nothing busy beside it.
// Every save goes through here: GameLoop's dawn and drop saves too.
static volatile bool g_saveRequested = false;
static TaskHandle_t  g_saveTask      = nullptr;
static void requestSave() {
  g_saveRequested = true;
  if (g_saveTask) xTaskNotifyGive(g_saveTask);
}
static void saveTaskFn(void*) {
  bool loggedStack = false;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // every request since the last save, as one
    if (!g_saveRequested) continue;
    g_saveRequested = false;
    saveGame();
    if (!loggedStack) {
      loggedStack = true;
      Log.notice("Saver: first save done core=%d stack_free=%u", (int)xPortGetCoreID(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
  }
}
// setup(), once the world is loaded. Requests made before it are kept.
static void startSaveTask() {
  if (xTaskCreatePinnedToCore(saveTaskFn, "Saver", 8192, NULL, 1, &g_saveTask, 0) != pdPASS) {
    g_saveTask = nullptr;
    Log.error("Saver task create FAILED -- GameLoop saves inline");
    return;
  }
  if (g_saveRequested) xTaskNotifyGive(g_saveTask);
}
// GameLoop, after its tick: only the fallback for a Saver that never started.
static void serviceSaveRequest() {
  if (g_saveTask || !g_saveRequested) return;
  g_saveRequested = false;
  saveGame();
}

// ── Ground ages + remains (SAVE_GND_F) ───────────────────────────────────────
// Called by tryLoadSave() once map.bin -- and with it the ground piles and
// G.dayCount -- has loaded. Without the file (a save older than it) every pile
// is dated today, so it gets a full GROUND_AGE_DAYS, and there are no remains.
// A day that is not younger than GROUND_AGE_DAYS cannot be real (the dawn
// sweep runs before the dawn save), so it is treated the same way.
static void loadGroundFile() {
  memset(remainsTable, 0, sizeof(remainsTable));
  for (int g = 0; g < MAX_GROUND; g++) groundItems[g].day = G.dayCount;
  if (!SD.exists(SAVE_GND_F)) { Log.notice("No %s: piles dated today, no remains", SAVE_GND_F); return; }
  File f = SD.open(SAVE_GND_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_GND_F); return; }
  SaveGroundHdr gh;
  if (f.read((uint8_t*)&gh, sizeof(gh)) != sizeof(gh) ||
      gh.magic != GROUND_SAVE_MAGIC || gh.version != 1) {
    Log.warning("%s header unreadable - ignoring", SAVE_GND_F);
    f.close();
    return;
  }
  for (int g = 0; g < (int)gh.nGround; g++) {
    uint16_t d;
    if (f.read((uint8_t*)&d, sizeof(d)) != sizeof(d)) break;
    if (g < MAX_GROUND && groundItems[g].itemType && groundAge(d) < GROUND_AGE_DAYS)
      groundItems[g].day = d;
  }
  int n = 0;
  for (int i = 0; i < (int)gh.nRemains && n < MAX_REMAINS; i++) {
    SaveRemains sr;
    if (f.read((uint8_t*)&sr, sizeof(sr)) != sizeof(sr)) break;
    // Same defensive bounds check as the caravan/doom coords in tryLoadSave().
    if (sr.q < 0 || sr.q >= MAP_COLS || sr.r < 0 || sr.r >= MAP_ROWS || sr.pid >= MAX_PLAYERS) continue;
    Remains& rm = remainsTable[n++];
    rm.used = true;
    rm.pid  = sr.pid;
    rm.q    = sr.q;
    rm.r    = sr.r;
    rm.day  = (groundAge(sr.day) < GROUND_AGE_DAYS) ? sr.day : G.dayCount;
    for (int k = 0; k < 5; k++) rm.res[k] = (uint8_t)min(99, (int)sr.res[k]);
    memcpy(rm.name, sr.name, sizeof(rm.name));
    rm.name[sizeof(rm.name) - 1] = '\0';
    for (int c = 0; rm.name[c]; c++)   // it goes out inside JSON quotes
      if (rm.name[c] < 0x20 || rm.name[c] > 0x7E || rm.name[c] == '"' || rm.name[c] == '\\') rm.name[c] = '_';
  }
  f.close();
  // A record with no tokens left marks its piles; with neither, it is done.
  for (int i = 0; i < MAX_REMAINS; i++)
    if (remainsTable[i].used) remainsPrune(remainsTable[i].q, remainsTable[i].r);
  Log.notice("SD READ: %s remains=%d", SAVE_GND_F, n);
}

// ── SD Load ───────────────────────────────────────────────────────────────────
bool tryLoadSave() {
  LOG_FN();
  if (!SD.exists(SAVE_MAP_F)) { Log.notice("No save: %s missing", SAVE_MAP_F); return false; }
  File f = SD.open(SAVE_MAP_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_MAP_F); return false; }
  Log.notice("SD READ: %s size=%u", SAVE_MAP_F, (unsigned)f.size());
  SaveHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
    Log.warning("Save header read short: %s", SAVE_MAP_F);
    f.close(); return false;
  }
  if (hdr.magic != SAVE_MAGIC || hdr.version != SAVE_VERSION) {
    Log.warning("Save magic/version mismatch magic=0x%x ver=%u — ignoring",
                (unsigned)hdr.magic, (unsigned)hdr.version);
    f.close();
    return false;
  }
  if (f.read((uint8_t*)G.map, MAP_BYTES) != MAP_BYTES) {
    Log.warning("Save map read short: %s", SAVE_MAP_F);
    f.close(); return false;
  }
  // doShelter() used to build on Settlements; it refuses now. Strip any tent
  // a save made before that, or the "rest under a shelter = no LL loss" rule
  // keeps a starvation-proof bed in town. No SAVE_VERSION bump: the layout is
  // unchanged, this only drops a value the rules no longer allow.
  for (int r = 0; r < MAP_ROWS; r++)
    for (int q = 0; q < MAP_COLS; q++)
      if (G.map[r][q].terrain == 9) G.map[r][q].shelter = 0;
  // v17 tunnel board. A short read means a truncated save: bail rather than
  // leave G.tunnel half-populated, since generateMap() (which regenerates the
  // tunnels too) is the safe fallback.
  if (f.read((uint8_t*)G.tunnel, TUNNEL_BYTES) != TUNNEL_BYTES) {
    Log.warning("Save tunnel read short: %s", SAVE_MAP_F);
    f.close(); return false;
  }
  // v20 open sides. Same rule: a board with no walls loaded is not a board.
  if (f.read((uint8_t*)G.tunnelOp, TUNNEL_OP_BYTES) != TUNNEL_OP_BYTES) {
    Log.warning("Save tunnel sides read short: %s", SAVE_MAP_F);
    f.close(); return false;
  }
  // Load ground items if present (appended after map data)
  memset(groundItems, 0, sizeof(groundItems));
  int giLoaded = 0;
  for (int g = 0; g < MAX_GROUND; g++) {
    SaveGroundItem sgi;
    if (f.read((uint8_t*)&sgi, sizeof(sgi)) != sizeof(sgi)) break;
    groundItems[g] = { sgi.q, sgi.r, sgi.itemType, sgi.qty };
    if (sgi.itemType) giLoaded++;
  }
  f.close();
  G.dayCount        = hdr.dayCount;
  G.threatClock     = hdr.threatClock;
  G.weatherPhase    = (hdr.weatherPhase < 6) ? hdr.weatherPhase : WEATHER_CLEAR;
  G.weatherCounter  = hdr.weatherCounter;
  G.dayTick         = (hdr.dayTick < DAY_TICKS) ? hdr.dayTick : 0;
  G.badWeatherTicks = hdr.badWeatherTicks;
  // World-system entities (v13+ only — the version check above already
  // rejects anything older). wInit() first for its side effects (zero-clear
  // W_hex/fireCount/debounce — tracks/fire are deliberately not persisted,
  // and it also gives both entities a valid random q/r as a fallback),
  // then overwrite the persisted fields it randomized. q/r are bounds-
  // checked before trusting them — a corrupted/short SD write (see
  // dev-loop.md's "Short SD writes" note) could otherwise feed an
  // out-of-range index straight into W_hex[r][q]/G.map[r][q] everywhere
  // world-system.hpp touches these entities. Mirrors the weatherPhase/
  // dayTick bound checks above: invalid means "keep wInit()'s value" rather
  // than crash.
  wInit();
  if (hdr.caravanQ >= 0 && hdr.caravanQ < MAP_COLS && hdr.caravanR >= 0 && hdr.caravanR < MAP_ROWS) {
    W.caravan.q = hdr.caravanQ; W.caravan.r = hdr.caravanR;
  }
  W.caravan.restockTimer = hdr.caravanRestockTimer;
  memcpy(W.caravan.inv, hdr.caravanInv, 5);
  W.caravan.active = hdr.caravanActive != 0;
  // v15 shelf. wInit() just rolled a fresh one; the saved shelf replaces it
  // wholesale (an emptied shelf stays empty until the persisted restock
  // timer runs down). Any id items.cfg no longer knows — the cfg is editable
  // without a reflash — is dropped rather than sold as a mystery.
  for (int s = 0; s < CARAVAN_STOCK_SLOTS; s++) {
    uint8_t id = hdr.caravanStockItem[s];
    if (id && hdr.caravanStockQty[s] && getItemDef(id)) {
      W.caravan.stockItem[s] = id;
      W.caravan.stockQty[s]  = (uint8_t)min((int)hdr.caravanStockQty[s], (int)CARAVAN_STOCK_MAX);
    } else {
      W.caravan.stockItem[s] = 0;
      W.caravan.stockQty[s]  = 0;
    }
  }
  if (hdr.doomQ >= 0 && hdr.doomQ < MAP_COLS && hdr.doomR >= 0 && hdr.doomR < MAP_ROWS) {
    W.creepingDoom.q = hdr.doomQ; W.creepingDoom.r = hdr.doomR;
  }
  W.creepingDoom.awareness = (hdr.doomAwareness <= 100) ? hdr.doomAwareness : 0;
  // Taunt state isn't saved; seed the tier from the restored awareness so
  // resuming a hunt doesn't re-announce a tier the player already heard.
  W.creepingDoom.lastTauntTier = doomTauntTier();
  W.creepingDoom.tauntCooldown = DOOM_TAUNT_COOLDOWN;
  // Audio band starts silent for the same reason, but the opposite way round:
  // seeding it from the restored position would let the very first world tick
  // fire the release cue for a hunt this boot never played.
  W.creepingDoom.lastAudioBand = 0;
  W.creepingDoom.audioPhase    = 0;
  // v17 hatch pairings. Same defensive bounds check as the caravan/doom coords
  // above — a short or corrupted SD write must not feed an out-of-range index
  // into G.map[sr][sq] or G.tunnel[tr][tq]. A hatch that fails is dropped; the
  // rest still work, and a fully-dropped table means the tunnels are merely
  // unreachable rather than a crash.
  hatchCount = 0;
  memset(bunkerHatches, 0, sizeof(bunkerHatches));
  for (int i = 0; i < min((int)hdr.hatchCount, (int)MAX_HATCHES); i++) {
    const BunkerHatch& h = hdr.bunkerHatches[i];
    if (h.sq < 0 || h.sq >= MAP_COLS || h.sr < 0 || h.sr >= MAP_ROWS) continue;
    if (h.tq >= TUN_COLS || h.tr >= TUN_ROWS) continue;
    bunkerHatches[hatchCount++] = h;
  }
  if (hatchCount != hdr.hatchCount)
    Log.warning("Save hatches: %u of %u survived bounds check",
                (unsigned)hatchCount, (unsigned)hdr.hatchCount);
  Log.notice("Save map loaded day=%u tick=%lu tc=%u weather=%u groundItems=%d",
             (unsigned)G.dayCount, (unsigned long)G.dayTick, (unsigned)G.threatClock,
             (unsigned)G.weatherPhase, giLoaded);
  loadGroundFile();   // needs G.dayCount, set above
  loadTrailFile();    // the sides of the footprints just loaded (trails.hpp)
  loadLightFile();    // the tunnel's sections and lights, or a fresh layout (tunnels.hpp)
  loadKaneFile();     // the jeep and the arc (kane.hpp); wInit() above rolled the fallback
  loadVacFile();      // the sign and its count (vacancy.hpp); likewise
  File p = SD.open(SAVE_PLY_F, FILE_READ);
  if (p) {
    Log.notice("SD READ: %s size=%u", SAVE_PLY_F, (unsigned)p.size());
    int plyLoaded = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      SavePlayer sp;
      if (p.read((uint8_t*)&sp, sizeof(sp)) != sizeof(sp)) break;
      if (!sp.used) continue;
      Player& pl = G.players[i];
      memcpy(pl.name, sp.name, 16);
      pl.archetype = sp.archetype;
      memcpy(pl.skills, sp.skills, NUM_SKILLS);
      pl.q = sp.q; pl.r = sp.r;
      pl.ll = sp.ll; pl.food = sp.food; pl.water = sp.water;
      pl.radiation = sp.radiation;
      memcpy(pl.inv, sp.inv, 5);
      memcpy(pl.invType, sp.invType, INV_SLOTS_MAX);
      memcpy(pl.invQty,  sp.invQty,  INV_SLOTS_MAX);
      memcpy(pl.equip,  sp.equip,  EQUIP_SLOTS);
      pl.invSlots = sp.invSlots;
      pl.score = sp.score; pl.steps = sp.steps;
      pl.encCount     = sp.encCount;
      pl.apLeft    = sp.apLeft;
      pl.fThreshBelow = sp.fThreshBelow;
      pl.wThreshBelow = sp.wThreshBelow;
      memcpy(pl.wounds, sp.wounds, NUM_WOUND_TIER);
      memcpy(pl.surveyedMap, sp.surveyedMap, sizeof(pl.surveyedMap));
      pl.radClean     = sp.radClean != 0;
      pl.llCapPenalty = sp.llCapPenalty;
      // OR, not assign: a recipe promoted to `starter = yes` after this save
      // was written has to reach survivors who are already in the world.
      pl.knownRecipes = sp.knownRecipes | starterRecipeMask();
      // v17: resume underground. Everything is bounds-checked, and anything
      // that fails drops the survivor back to the surface at their stored q/r
      // rather than leaving depth=1 with a bogus tunnel index — every
      // depth-gated subsystem would then read G.tunnel out of range. Requires
      // a surviving hatch too: if the pairing was dropped above there is no
      // way back up, so surfacing is the only safe resume.
      pl.depth = 0; pl.tq = 0; pl.tr = 0; pl.hatchIdx = 0;
      if (sp.depth == 1 && sp.hatchIdx < hatchCount &&
          sp.tq >= 0 && sp.tq < TUN_COLS && sp.tr >= 0 && sp.tr < TUN_ROWS &&
          G.tunnel[sp.tr][sp.tq].terrain != 15) {
        pl.depth = 1; pl.tq = sp.tq; pl.tr = sp.tr; pl.hatchIdx = sp.hatchIdx;
      } else if (sp.depth == 1) {
        Log.warning("Save player %d was underground at (%d,%d) hatch=%u — surfaced",
                    i, (int)sp.tq, (int)sp.tr, (unsigned)sp.hatchIdx);
      }
      pl.resting      = false;
      pl.connected = false; pl.wsClientId = 0;
      plyLoaded++;
    }
    p.close();
    Log.notice("Save players loaded: %d", plyLoaded);
  } else {
    Log.warning("SD MISSING: %s (map loaded without players)", SAVE_PLY_F);
  }
  return true;
}
