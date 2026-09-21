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
      uint8_t savedPoi[MAX_PLAYERS] = {0};
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!encounters[i].active) continue;
        HexCell& c = G.map[encounters[i].hexR][encounters[i].hexQ];
        savedPoi[i] = c.poi;
        c.poi = encounters[i].encIdx;
      }
      mapBytes += f.write((uint8_t*)&hdr, sizeof(hdr));
      mapBytes += f.write((uint8_t*)G.map, MAP_BYTES);
      // v17: tunnel board, immediately after the surface map. tryLoadSave()
      // reads these blocks back in the same order; the ground-items block
      // still follows and is still read until EOF.
      mapBytes += f.write((uint8_t*)G.tunnel, TUNNEL_BYTES);
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!encounters[i].active) continue;
        G.map[encounters[i].hexR][encounters[i].hexQ].poi = savedPoi[i];
      }
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
        sp.movesLeft    = pl.movesLeft;
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
      Log.verbose("SD APPEND: %s ground=%u bytes", SAVE_MAP_F, (unsigned)giBytes);
    } else {
      Log.error("SD OPEN FAIL (append): %s", SAVE_MAP_F);
    }
    xSemaphoreGive(G.mutex);
    Log.notice("saveGame complete took=%ums", (unsigned)(millis() - _t0));
  } else {
    Log.warning("saveGame: G.mutex timeout (100ms) — skipped");
  }
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
    Log.warning("Save magic/version mismatch magic=%08x ver=%u — ignoring",
                (unsigned)hdr.magic, (unsigned)hdr.version);
    f.close();
    return false;
  }
  if (f.read((uint8_t*)G.map, MAP_BYTES) != MAP_BYTES) {
    Log.warning("Save map read short: %s", SAVE_MAP_F);
    f.close(); return false;
  }
  // v17 tunnel board. A short read means a truncated save: bail rather than
  // leave G.tunnel half-populated, since generateMap() (which regenerates the
  // tunnels too) is the safe fallback.
  if (f.read((uint8_t*)G.tunnel, TUNNEL_BYTES) != TUNNEL_BYTES) {
    Log.warning("Save tunnel read short: %s", SAVE_MAP_F);
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
      pl.movesLeft    = sp.movesLeft;
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
