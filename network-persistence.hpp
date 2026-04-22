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

// ── SD Save ───────────────────────────────────────────────────────────────────
void saveGame() {
  if (SD.cardType() == CARD_NONE) { Serial.println("[SAVE] SKIP: no SD card"); return; }
  uint32_t _t0 = millis();
  bool _got = (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(100)) == pdTRUE);
  Serial.printf("[SAVE] mutex_wait=%lums ok=%d  heap=%u\n",
    (unsigned long)(millis()-_t0), _got ? 1 : 0, (unsigned)ESP.getFreeHeap());
  Serial.flush();
  if (_got) {
    if (!SD.exists(SAVE_DIR)) SD.mkdir(SAVE_DIR);
    // Map file
    File f = SD.open(SAVE_MAP_F, FILE_WRITE);
    if (f) {
      SaveHeader hdr = { SAVE_MAGIC, SAVE_VERSION, G.dayCount, G.threatClock,
                         G.weatherPhase, G.weatherCounter };
      f.write((uint8_t*)&hdr, sizeof(hdr));
      f.write((uint8_t*)G.map, sizeof(G.map));
      f.close();
    }
    // Players file
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
        memcpy(sp.invType, pl.invType, 12);
        memcpy(sp.invQty, pl.invQty, 12);
        memcpy(sp.equip,  pl.equip,  EQUIP_SLOTS);
        sp.invSlots = pl.invSlots;
        sp.score = pl.score; sp.steps = pl.steps;
        sp.encCount     = pl.encCount;
        sp.movesLeft    = pl.movesLeft;
        sp.fThreshBelow = pl.fThreshBelow;
        sp.wThreshBelow = pl.wThreshBelow;
        memcpy(sp.surveyedMap, pl.surveyedMap, sizeof(sp.surveyedMap));
        sp.used = (pl.name[0] != '\0') ? 1 : 0;
        p.write((uint8_t*)&sp, sizeof(sp));
      }
      p.close();
    }
    // Ground items — append to map file as a fixed-size block
    File gf = SD.open(SAVE_MAP_F, FILE_APPEND);
    if (gf) {
      for (int g = 0; g < MAX_GROUND; g++) {
        SaveGroundItem sgi = { groundItems[g].q, groundItems[g].r,
                               groundItems[g].itemType, groundItems[g].qty };
        gf.write((uint8_t*)&sgi, sizeof(sgi));
      }
      gf.close();
    }
    xSemaphoreGive(G.mutex);
    Serial.printf("[SAVE] Day %d saved  t=%lums\n", (int)G.dayCount, (unsigned long)(millis()-_t0));
    Serial.flush();
  } else {
    Serial.println("[SAVE] MUTEX TIMEOUT (100ms) — save skipped");
    Serial.flush();
  }
}

// ── SD Load ───────────────────────────────────────────────────────────────────
bool tryLoadSave() {
  if (!SD.exists(SAVE_MAP_F)) return false;
  File f = SD.open(SAVE_MAP_F, FILE_READ);
  if (!f) return false;
  SaveHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }
  if (hdr.magic != SAVE_MAGIC || hdr.version != SAVE_VERSION) {
    f.close();
    Serial.printf("[SAVE] Version mismatch (file:%d expected:%d) — fresh start\n",
      (int)hdr.version, (int)SAVE_VERSION);
    return false;
  }
  if (f.read((uint8_t*)G.map, sizeof(G.map)) != sizeof(G.map)) { f.close(); return false; }
  // Load ground items if present (appended after map data)
  memset(groundItems, 0, sizeof(groundItems));
  for (int g = 0; g < MAX_GROUND; g++) {
    SaveGroundItem sgi;
    if (f.read((uint8_t*)&sgi, sizeof(sgi)) != sizeof(sgi)) break;
    groundItems[g] = { sgi.q, sgi.r, sgi.itemType, sgi.qty };
  }
  f.close();
  G.dayCount       = hdr.dayCount;
  G.threatClock    = hdr.threatClock;
  G.weatherPhase   = hdr.weatherPhase;
  G.weatherCounter = hdr.weatherCounter;
  File p = SD.open(SAVE_PLY_F, FILE_READ);
  if (p) {
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
      memcpy(pl.invType, sp.invType, 12);
      memcpy(pl.invQty, sp.invQty, 12);
      memcpy(pl.equip,  sp.equip,  EQUIP_SLOTS);
      pl.invSlots = sp.invSlots;
      pl.score = sp.score; pl.steps = sp.steps;
      pl.encCount     = sp.encCount;
      pl.movesLeft    = sp.movesLeft;
      pl.fThreshBelow = sp.fThreshBelow;
      pl.wThreshBelow = sp.wThreshBelow;
      memcpy(pl.surveyedMap, sp.surveyedMap, sizeof(pl.surveyedMap));
      pl.connected = false; pl.wsClientId = 0;
    }
    p.close();
  }
  Serial.printf("[SAVE] Loaded Day %d from SD\n", (int)G.dayCount);
  return true;
}
