#pragma once
// ── boot-assets.hpp ─────────────────────────────────────────────────────────
// Boot splash, SD→PSRAM asset loading, item registry parser, periodic status.
// Included by Esp32HexMapCrawl.ino before gameplay .hpp files.

// ── Boot-time SD→PSRAM loader ─────────────────────────────────
static void loadWebFilesToRAM() {
  File dir = SD.open("/data");
  if (!dir) { Log.error("SD OPEN FAIL: /data"); return; }
  File f = dir.openNextFile();
  while (f) {
    String fname = String(f.name());
    if (f.isDirectory() && fname.equalsIgnoreCase("img")) {
      File imgFile = f.openNextFile();
      while (imgFile && imgCacheCount < MAX_IMG_CACHE) {
        if (imgFile.isDirectory()) {
          String subDirName = String(imgFile.name());
          File subFile = imgFile.openNextFile();
          while (subFile && imgCacheCount < MAX_IMG_CACHE) {
            if (!subFile.isDirectory()) {
              size_t sz = subFile.size();
              uint8_t* buf = (uint8_t*)ps_malloc(sz);
              if (buf) {
                subFile.read(buf, sz);
                char cacheName[40];
                snprintf(cacheName, sizeof(cacheName), "%s/%s", subDirName.c_str(), subFile.name());
                strncpy(imgCache[imgCacheCount].name, cacheName, 39);
                imgCache[imgCacheCount].name[39] = 0;
                imgCache[imgCacheCount].buf = buf;
                imgCache[imgCacheCount].len = sz;
                Log.verbose("IMG cache: %s (%u B)", imgCache[imgCacheCount].name, (unsigned)sz);
                imgCacheCount++;
              } else {
                Log.error("IMG cache ps_malloc FAIL size=%u name=%s/%s",
                          (unsigned)sz, subDirName.c_str(), subFile.name());
              }
            }
            subFile.close();
            subFile = imgFile.openNextFile();
          }
        } else {
          size_t sz = imgFile.size();
          uint8_t* buf = (uint8_t*)ps_malloc(sz);
          if (buf) {
            imgFile.read(buf, sz);
            strncpy(imgCache[imgCacheCount].name, imgFile.name(), 39);
            imgCache[imgCacheCount].name[39] = 0;
            imgCache[imgCacheCount].buf = buf;
            imgCache[imgCacheCount].len = sz;
            Log.verbose("IMG cache: %s (%u B)", imgCache[imgCacheCount].name, (unsigned)sz);
            imgCacheCount++;
          } else {
            Log.error("IMG cache ps_malloc FAIL size=%u name=%s", (unsigned)sz, imgFile.name());
          }
        }
        imgFile.close();
        imgFile = f.openNextFile();
      }
    } else if (!f.isDirectory()) {
      for (int i = 0; i < WEB_FILE_COUNT; i++) {
        if (fname.equalsIgnoreCase(WEB_FILES[i].sdName)) {
          size_t sz = f.size();
          uint8_t* buf = (uint8_t*)ps_malloc(sz);
          if (buf) {
            size_t got = f.read(buf, sz);
            WEB_FILES[i].buf = buf;
            WEB_FILES[i].len = got;
            Log.notice("WEB cache: %s (%u B)", WEB_FILES[i].sdName, (unsigned)got);
          } else {
            Log.error("WEB cache ps_malloc FAIL size=%u name=%s",
                      (unsigned)sz, WEB_FILES[i].sdName);
          }
          break;
        }
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
}

// ── Periodic player status table ───────────────────────────────
static void printStatus() {
  struct Snap {
    bool     on; int16_t q, r; uint16_t score, steps;
    uint8_t  terrain;
    char     name[12]; uint32_t connectMs;
    uint8_t  ll, food, water, radiation;
    uint8_t  archetype;
    uint8_t  skills[NUM_SKILLS];
  } snap[MAX_PLAYERS];
  [[maybe_unused]] uint32_t tick = 0;
  [[maybe_unused]] uint8_t  snapTC = 0;
  [[maybe_unused]] uint16_t snapDay = 0;
  uint32_t nowMs = millis();

  uint16_t mapRes[6] = {0};

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  tick    = G.tickId;
  snapTC  = G.threatClock;
  snapDay = G.dayCount;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p       = G.players[i];
    snap[i].on      = p.connected;
    snap[i].q       = p.q; snap[i].r = p.r;
    snap[i].score   = p.score; snap[i].steps = p.steps;
    snap[i].terrain = G.map[p.r][p.q].terrain;
    snap[i].connectMs = p.connectMs;
    memcpy(snap[i].name, p.name, 12);
    snap[i].ll        = p.ll;
    snap[i].food      = p.food;
    snap[i].water     = p.water;
    snap[i].radiation = p.radiation;
    snap[i].archetype = p.archetype;
    memcpy(snap[i].skills, p.skills, NUM_SKILLS);
  }
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t res = G.map[r][c].resource;
      if (res > 0 && res < 6) mapRes[res]++;
    }
  xSemaphoreGive(G.mutex);

  [[maybe_unused]] uint32_t upSec = nowMs / 1000;


  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!snap[i].on) continue;
    uint8_t  t      = snap[i].terrain < NUM_TERRAIN ? snap[i].terrain : 0;
    int8_t   vl     = TERRAIN_VIS[t];
    [[maybe_unused]] int      effVR  = (vl <= -3) ? 0 : (vl == -2) ? 1 : (vl == -1) ? 2 : (vl == 0) ? VISION_R : (vl == 1) ? VISION_R + 1 : VISION_R + 2;
    uint32_t sessMs = nowMs - snap[i].connectMs;
    [[maybe_unused]] uint32_t sessSec = sessMs / 1000;
    [[maybe_unused]] uint8_t  arch   = snap[i].archetype < NUM_ARCHETYPES ? snap[i].archetype : 0;

  }
}

// ── Item registry parser ──────────────────────────────────────────────────────
static void trimRight(char* s) {
  int n = (int)strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
    s[--n] = 0;
}

static const char* trimLeft(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static void stripComment(char* s) {
  for (char* p = s; *p; p++) {
    if (*p == '#') { *p = 0; break; }
  }
  trimRight(s);
}

static EffectId parseEffectId(const char* v) {
  if      (strncmp(v, "unlock_action", 13) == 0) return EFX_UNLOCK_ACTION;
  else if (strncmp(v, "reveal_fog",    10) == 0) return EFX_REVEAL_FOG;
  else if (strncmp(v, "narrative",      9) == 0) return EFX_NARRATIVE;
  else if (strncmp(v, "threat_mod",    10) == 0) return EFX_THREAT_MOD;
  else if (strncmp(v, "cure_status",   11) == 0) return EFX_CURE_STATUS;
  return EFX_NONE;
}

static void commitItem(ItemDef& cur, bool& hasItem) {
  if (!hasItem || cur.id == 0) return;
  if (itemCount < MAX_ITEMS) {
    itemRegistry[itemCount++] = cur;
  } else {
    Log.warning("Item registry FULL at %d — dropping id=%d name=%s",
                (int)MAX_ITEMS, (int)cur.id, cur.name);
  }
  hasItem = false;
  cur = ItemDef{};
}

static void loadItemRegistry() {
  itemCount = 0;
  memset(itemRegistry, 0, sizeof(itemRegistry));

  File f = SD.open("/data/items.cfg");
  if (!f) {
    Log.warning("SD MISSING: /data/items.cfg");
    return;
  }
  Log.notice("Items load: /data/items.cfg size=%u", (unsigned)f.size());

  ItemDef cur = {};
  bool hasItem = false;
  char line[128];

  while (f.available()) {
    int n = 0;
    while (f.available() && n < (int)sizeof(line) - 1) {
      char c = (char)f.read();
      if (c == '\n') break;
      line[n++] = c;
    }
    line[n] = 0;
    stripComment(line);
    const char* t = trimLeft(line);
    if (*t == 0) continue;

    if (strncmp(t, "[item]", 6) == 0 || strncmp(t, "[Item]", 6) == 0) {
      commitItem(cur, hasItem);
      cur = ItemDef{}; cur.maxStack = 1; cur.tradeable = true;
      hasItem = true;
      continue;
    }

    if (!hasItem) continue;

    const char* eq = strchr(t, '=');
    if (!eq) continue;

    char key[32] = {};
    int klen = (int)(eq - t);
    if (klen <= 0 || klen >= (int)sizeof(key)) continue;
    memcpy(key, t, klen); key[klen] = 0;
    trimRight(key);

    const char* val = trimLeft(eq + 1);

    if      (strcmp(key, "id")       == 0) cur.id          = (uint8_t)atoi(val);
    else if (strcmp(key, "name")     == 0) { strncpy(cur.name, val, 15); cur.name[15] = 0; }
    else if (strcmp(key, "category") == 0) {
      if      (strncmp(val, "consumable", 10) == 0) cur.category = ITEM_CONSUMABLE;
      else if (strncmp(val, "equipment",   9) == 0) cur.category = ITEM_EQUIPMENT;
      else if (strncmp(val, "material",    8) == 0) cur.category = ITEM_MATERIAL;
      else if (strncmp(val, "key",         3) == 0) cur.category = ITEM_KEY;
    }
    else if (strcmp(key, "slot")     == 0) {
      if      (strncmp(val, "head",    4) == 0) cur.equipSlot = EQUIP_HEAD;
      else if (strncmp(val, "body",    4) == 0) cur.equipSlot = EQUIP_BODY;
      else if (strncmp(val, "hand",    4) == 0) cur.equipSlot = EQUIP_HAND;
      else if (strncmp(val, "feet",    4) == 0) cur.equipSlot = EQUIP_FEET;
      else if (strncmp(val, "vehicle", 7) == 0) cur.equipSlot = EQUIP_VEHICLE;
      else                                       cur.equipSlot = EQUIP_NONE;
    }
    else if (strcmp(key, "stack")    == 0) cur.maxStack     = (uint8_t)max(1, atoi(val));
    else if (strcmp(key, "trade")    == 0) cur.tradeable    = (strncmp(val, "yes", 3) == 0 || val[0] == '1');
    else if (strcmp(key, "value")    == 0) cur.tradeValue   = (uint8_t)atoi(val);
    else if (strcmp(key, "ll")       == 0) cur.statMods[STAT_LL]      = (int8_t)atoi(val);
    else if (strcmp(key, "food")     == 0) cur.statMods[STAT_FOOD]    = (int8_t)atoi(val);
    else if (strcmp(key, "water")    == 0) cur.statMods[STAT_WATER]   = (int8_t)atoi(val);
    else if (strcmp(key, "rad")      == 0) cur.statMods[STAT_RAD]     = (int8_t)atoi(val);
    else if (strcmp(key, "mp")       == 0) cur.statMods[STAT_MP]      = (int8_t)atoi(val);
    else if (strcmp(key, "slots")    == 0) cur.statMods[STAT_SLOTS]   = (int8_t)atoi(val);
    else if (strcmp(key, "water_cost") == 0) cur.opCost[0] = (uint8_t)atoi(val);
    else if (strcmp(key, "food_cost")  == 0) cur.opCost[1] = (uint8_t)atoi(val);
    else if (strcmp(key, "fuel_cost")  == 0) cur.opCost[2] = (uint8_t)atoi(val);
    else if (strcmp(key, "med_cost")   == 0) cur.opCost[3] = (uint8_t)atoi(val);
    else if (strcmp(key, "scrap_cost") == 0) cur.opCost[4] = (uint8_t)atoi(val);
    else if (strcmp(key, "terrain")  == 0) cur.passTerrainBits = (uint8_t)atoi(val);
    else if (strcmp(key, "effect")   == 0) cur.effectId       = parseEffectId(val);
    else if (strcmp(key, "param")    == 0) cur.effectParam     = (uint8_t)atoi(val);
    else if (strcmp(key, "effect2")  == 0) cur.effectId2      = parseEffectId(val);
    else if (strcmp(key, "param2")   == 0) cur.effectParam2   = (uint8_t)atoi(val);
  }
  commitItem(cur, hasItem);
  f.close();

}

// Lookup item by ID — O(N) scan over loaded registry.
static const ItemDef* getItemDef(uint8_t id) {
  for (int i = 0; i < (int)itemCount; i++)
    if (itemRegistry[i].id == id) return &itemRegistry[i];
  return nullptr;
}

// ── Encounter engine: boot loading ────────────────────────────────────────────

// JSON helpers (minimal, for known encounter JSON formats)
static const char* jsonFindKey(const char* json, const char* key) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char* p = strstr(json, search);
  if (!p) return nullptr;
  p += strlen(search);
  while (*p == ' ' || *p == '\t' || *p == ':') p++;
  return p;
}
static int   jsonInt(const char* p) { if (!p) return 0; while (*p == ' ') p++; return atoi(p); }
static bool  jsonStr(const char* p, char* buf, int bufLen) {
  if (!p) return false;
  while (*p == ' ') p++;
  if (*p != '"') return false;
  p++;
  int i = 0;
  while (*p && *p != '"' && i < bufLen - 1) buf[i++] = *p++;
  buf[i] = 0;
  return true;
}

// Load /data/encounters/index.json → encPools[0..9]
static void loadEncounterIndex() {
  File f = SD.open("/data/encounters/index.json");
  if (!f) { Log.warning("SD MISSING: /data/encounters/index.json"); return; }
  Log.notice("Encounter index load: size=%u", (unsigned)f.size());
  size_t sz = min((size_t)f.size(), (size_t)512);
  char* buf = (char*)malloc(sz + 1);
  if (!buf) { Log.error("encounter index malloc FAIL size=%u", (unsigned)(sz+1)); f.close(); return; }
  f.read((uint8_t*)buf, sz);
  buf[sz] = 0;
  f.close();
  memset(encPools, 0, sizeof(encPools));
  for (int t = 0; t <= 9; t++) {
    char tKey[5]; snprintf(tKey, sizeof(tKey), "\"%d\"", t);
    const char* entry = strstr(buf, tKey);
    if (!entry) continue;
    entry += strlen(tKey);
    int copyLen = min(80, (int)(sz - (size_t)(entry - buf)));
    char tmp[80]; strncpy(tmp, entry, copyLen); tmp[copyLen] = 0;
    const char* cv = jsonFindKey(tmp, "count");
    const char* pv = jsonFindKey(tmp, "path");
    if (cv) encPools[t].count = (uint8_t)jsonInt(cv);
    if (pv) jsonStr(pv, encPools[t].path, sizeof(encPools[t].path));
  }
  free(buf);
}

// Load /encounters/loot_tables.json → lootTables[0..19]
static void loadLootTables() {
  File f = SD.open("/data/encounters/loot_tables.json");
  if (!f) { Log.warning("SD MISSING: /data/encounters/loot_tables.json"); return; }
  Log.notice("Loot tables load: size=%u", (unsigned)f.size());
  size_t sz = f.size();
  char* buf = (char*)ps_malloc(sz + 1);
  if (!buf) buf = (char*)malloc(sz + 1);
  if (!buf) { Log.error("loot tables malloc FAIL size=%u", (unsigned)(sz+1)); f.close(); return; }
  f.read((uint8_t*)buf, sz);
  buf[sz] = 0;
  f.close();
  lootTableCount = 0;
  const char* p = buf;
  while (lootTableCount < 20 && *p) {
    // Find next quoted key
    const char* nameStart = strchr(p, '"');
    if (!nameStart) break;
    nameStart++;
    const char* nameEnd = strchr(nameStart, '"');
    if (!nameEnd) break;
    // Verify this is a table name (followed by ": [")
    const char* after = nameEnd + 1;
    while (*after == ' ' || *after == '\t') after++;
    if (*after != ':') { p = nameEnd + 1; continue; }
    after++;
    while (*after == ' ' || *after == '\t') after++;
    if (*after != '[') { p = nameEnd + 1; continue; }
    int nameLen = min((int)(nameEnd - nameStart), 19);
    LootTable& tbl = lootTables[lootTableCount];
    strncpy(tbl.name, nameStart, nameLen); tbl.name[nameLen] = 0;
    tbl.count = 0;
    // Parse entries in the array
    const char* arr = after + 1;
    while (tbl.count < 8) {
      const char* entry = strchr(arr, '{');
      if (!entry) break;
      const char* entryEnd = strchr(entry, '}');
      if (!entryEnd) break;
      int elen = min((int)(entryEnd - entry), 80);
      char eb[80]; strncpy(eb, entry, elen); eb[elen] = 0;
      LootEntry& le = tbl.entries[tbl.count];
      le.item = 0; le.qtyMin = 1; le.qtyMax = 1; le.weight = 10;
      const char* iv = jsonFindKey(eb, "item");   if (iv) le.item   = (uint8_t)jsonInt(iv);
      const char* wv = jsonFindKey(eb, "weight"); if (wv) le.weight = (uint8_t)jsonInt(wv);
      const char* qv = jsonFindKey(eb, "qty");
      if (qv) {
        while (*qv == ' ') qv++;
        if (*qv == '[') {
          qv++;
          le.qtyMin = (uint8_t)atoi(qv);
          const char* comma = strchr(qv, ',');
          le.qtyMax = comma ? (uint8_t)atoi(comma + 1) : le.qtyMin;
        }
      }
      tbl.count++;
      arr = entryEnd + 1;
      while (*arr == ' ' || *arr == '\t' || *arr == '\n' || *arr == '\r') arr++;
      if (*arr == ']') { arr++; break; }
    }
    lootTableCount++;
    p = arr;
  }
  free(buf);
}

// Roll a weighted loot table entry → writes item ID and qty to out params
static void rollLootTable(const char* tableName, uint8_t* outItem, uint8_t* outQty) {
  *outItem = 0; *outQty = 0;
  for (int i = 0; i < lootTableCount; i++) {
    if (strncmp(lootTables[i].name, tableName, 19) != 0) continue;
    LootTable& tbl = lootTables[i];
    if (tbl.count == 0) return;
    int totalW = 0;
    for (int j = 0; j < tbl.count; j++) totalW += tbl.entries[j].weight;
    if (totalW == 0) return;
    int roll = (int)((uint32_t)esp_random() % (uint32_t)totalW);
    int cum = 0;
    for (int j = 0; j < tbl.count; j++) {
      cum += tbl.entries[j].weight;
      if (roll < cum) {
        *outItem = tbl.entries[j].item;
        uint8_t range = tbl.entries[j].qtyMax - tbl.entries[j].qtyMin;
        *outQty = tbl.entries[j].qtyMin + (range > 0 ? (uint8_t)(esp_random() % (range + 1)) : 0);
        return;
      }
    }
    return;
  }
}

// ── Encounter DN computation (spec §3) ────────────────────────────────────────
static uint8_t computeEncounterDN(int pid, uint8_t baseRisk, uint8_t /*skill*/) {
  int effectiveRisk = min((int)baseRisk, 100);
  if (G.threatClock >= TC_THRESHOLD_A) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_B) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_C) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_D) effectiveRisk += 5;
  effectiveRisk = constrain(effectiveRisk, 0, 100);
  int rawDN = 2 + (effectiveRisk * 10) / 100;
  Player& p = G.players[pid];
  int bonus = 0;
  if (p.ll > 4)        bonus += (p.ll - 4) / 2;
  if (p.radiation > 3) rawDN += (p.radiation - 3) / 2;
  return (uint8_t)constrain(rawDN - bonus, 2, 12);
}
