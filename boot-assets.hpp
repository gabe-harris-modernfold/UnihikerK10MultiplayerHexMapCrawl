#pragma once
// ── boot-assets.hpp ─────────────────────────────────────────────────────────
// Boot splash, SD→PSRAM asset loading, item registry parser, loot tables.
// Included by Esp32HexMapCrawl.ino before gameplay .hpp files.

// ── Web-file discovery helpers ────────────────────────────────
// MIME by extension. Anything not listed is NOT served (items.cfg, dotfiles,
// the sync manifest, ...). Extend here if a new asset type appears.
static const char* webMimeFor(const String& lowerName) {
  if (lowerName.endsWith(".html")) return "text/html";
  if (lowerName.endsWith(".js"))   return "text/javascript";
  if (lowerName.endsWith(".css"))  return "text/css";
  if (lowerName.endsWith(".json")) return "application/json";
  if (lowerName.endsWith(".svg"))  return "image/svg+xml";
  if (lowerName.endsWith(".ico"))  return "image/x-icon";
  if (lowerName.endsWith(".png"))  return "image/png";
  if (lowerName.endsWith(".jpg") || lowerName.endsWith(".jpeg")) return "image/jpeg";
  if (lowerName.endsWith(".txt") || lowerName.endsWith(".map")) return "text/plain";
  if (lowerName.endsWith(".webmanifest")) return "application/manifest+json";
  return nullptr;
}

static int findWebFile(const char* url) {
  for (int i = 0; i < webFileCount; i++)
    if (strcmp(webFiles[i].url, url) == 0) return i;
  return -1;
}

// Read one /data root file into PSRAM and register it under "/<name>".
// "<name>.gz" registers as "/<name>" with gzip=true; if both exist the gz
// copy replaces the plain one (smaller PSRAM, ~4x fewer bytes on the wire).
static void cacheWebFile(File& f, const String& fname) {
  if (fname.length() == 0 || fname[0] == '.') return;          // dotfiles, .upload-manifest.json
  String lower = fname; lower.toLowerCase();
  bool gz = lower.endsWith(".gz");
  String plainLower = gz ? lower.substring(0, lower.length() - 3) : lower;
  const char* mime = webMimeFor(plainLower);
  if (!mime) { Log.verbose("WEB skip (not a web asset): %s", fname.c_str()); return; }

  char url[48];
  String plainName = gz ? fname.substring(0, fname.length() - 3) : fname;
  if (plainName.length() + 1 >= sizeof(url)) {
    Log.error("WEB skip (name too long): %s", fname.c_str()); return;
  }
  snprintf(url, sizeof(url), "/%s", plainName.c_str());

  int existing = findWebFile(url);
  if (existing >= 0) {
    if (webFiles[existing].gzip) {                              // gz already cached — plain loses
      Log.verbose("WEB skip (gz sibling cached): %s", fname.c_str()); return;
    }
    if (!gz) return;                                            // duplicate plain (case-diff) — ignore
  }

  size_t sz = f.size();
  uint8_t* buf = (uint8_t*)ps_malloc(sz ? sz : 1);
  if (!buf) { Log.error("WEB cache ps_malloc FAIL size=%u name=%s", (unsigned)sz, fname.c_str()); return; }
  size_t got = f.read(buf, sz);

  int slot = existing;
  if (slot < 0) {
    if (webFileCount >= MAX_WEB_FILES) {
      Log.error("WEB cache FULL (MAX_WEB_FILES=%d) — not serving %s", MAX_WEB_FILES, fname.c_str());
      free(buf); return;
    }
    slot = webFileCount++;
  } else {
    Log.notice("WEB cache: %s replaces plain copy (gzip preferred)", fname.c_str());
    free(webFiles[slot].buf);
  }
  strlcpy(webFiles[slot].url, url, sizeof(webFiles[slot].url));
  webFiles[slot].mime = mime;
  webFiles[slot].gzip = gz;
  webFiles[slot].buf  = buf;
  webFiles[slot].len  = got;
  Log.notice("WEB cache: %s -> %s (%u B%s)", fname.c_str(), url, (unsigned)got, gz ? ", gzip" : "");
}

// ── Boot-time SD→PSRAM loader ─────────────────────────────────
// Images: every file under /data/img (one subdir deep) → imgCache.
// Web files: every web-typed file in the /data ROOT → webFiles (see the
// WebFile comment in Esp32HexMapCrawl.ino). Order of discovery doesn't
// matter; gz-vs-plain preference is resolved in cacheWebFile().
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
      cacheWebFile(f, fname);
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
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
  if      (strncmp(v, "reveal_fog",    10) == 0) return EFX_REVEAL_FOG;
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
  memset(itemRegistry, 0, MAX_ITEMS * sizeof(ItemDef));   // itemRegistry lives in PSRAM (pointer)

  File f = SD.open("/data/items.cfg");
  if (!f) {
    Log.warning("SD MISSING: /data/items.cfg");
    return;
  }
  Log.notice("Items load: /data/items.cfg size=%u", (unsigned)f.size());

  ItemDef cur = {};
  bool hasItem = false;
  // 256, not 128: items.cfg's section banners run to 211 characters and a
  // truncated read hands the *remainder* back on the next iteration as if it
  // were its own line.  Today every over-long line is a '#' comment, so the
  // fragment strips to nothing and is skipped -- but the first long inline
  // comment on a value line would have the tail parsed as a key.
  char line[256];

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
      cur = ItemDef{}; cur.maxStack = 1;
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
    // "trade" / "value" drive the caravan's shelf (world-system.hpp): a
    // consumable with trade=yes may be stocked and value is its asking price
    // in resource tokens. Player-to-player trades still move tokens only.
    else if (strcmp(key, "trade")    == 0) cur.tradeable    = (val[0] == 'y' || val[0] == 'Y' || val[0] == '1') ? 1 : 0;
    else if (strcmp(key, "value")    == 0) cur.value        = (uint8_t)constrain(atoi(val), 0, 99);
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
    else if (strcmp(key, "tracks")   == 0) cur.leavesTracks    = (val[0] == 'y' || val[0] == 'Y' || val[0] == '1') ? 1 : 0;
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

// ── Recipe registry parser ─────────────────────────────────────────────────────
// Same tiny line-parser as loadItemRegistry(), for /data/recipes.cfg.
static void commitRecipe(RecipeDef& cur, bool& hasRecipe) {
  if (!hasRecipe || cur.id == 0) return;
  if (recipeCount < MAX_RECIPES) {
    recipeRegistry[recipeCount++] = cur;
  } else {
    Log.warning("Recipe registry FULL at %d — dropping id=%d name=%s",
                (int)MAX_RECIPES, (int)cur.id, cur.name);
  }
  hasRecipe = false;
  cur = RecipeDef{};
}

static void loadRecipeRegistry() {
  recipeCount = 0;
  memset(recipeRegistry, 0, MAX_RECIPES * sizeof(RecipeDef));  // recipeRegistry lives in PSRAM (pointer)

  File f = SD.open("/data/recipes.cfg");
  if (!f) {
    Log.warning("SD MISSING: /data/recipes.cfg");
    return;
  }
  Log.notice("Recipes load: /data/recipes.cfg size=%u", (unsigned)f.size());

  RecipeDef cur = {};
  bool hasRecipe = false;
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

    if (strncmp(t, "[recipe]", 8) == 0 || strncmp(t, "[Recipe]", 8) == 0) {
      commitRecipe(cur, hasRecipe);
      cur = RecipeDef{}; cur.outputQty = 1;
      hasRecipe = true;
      continue;
    }

    if (!hasRecipe) continue;

    const char* eq = strchr(t, '=');
    if (!eq) continue;

    char key[32] = {};
    int klen = (int)(eq - t);
    if (klen <= 0 || klen >= (int)sizeof(key)) continue;
    memcpy(key, t, klen); key[klen] = 0;
    trimRight(key);

    const char* val = trimLeft(eq + 1);

    if      (strcmp(key, "id")          == 0) cur.id         = (uint8_t)atoi(val);
    else if (strcmp(key, "name")        == 0) { strncpy(cur.name, val, sizeof(cur.name) - 1); cur.name[sizeof(cur.name) - 1] = 0; }
    else if (strcmp(key, "output_item") == 0) cur.outputItem = (uint8_t)atoi(val);
    else if (strcmp(key, "output_qty")  == 0) cur.outputQty  = (uint8_t)max(1, atoi(val));
    else if (strcmp(key, "mat1")        == 0) cur.matItem[0] = (uint8_t)atoi(val);
    else if (strcmp(key, "matqty1")     == 0) cur.matQty[0]  = (uint8_t)atoi(val);
    else if (strcmp(key, "mat2")        == 0) cur.matItem[1] = (uint8_t)atoi(val);
    else if (strcmp(key, "matqty2")     == 0) cur.matQty[1]  = (uint8_t)atoi(val);
    else if (strcmp(key, "mat3")        == 0) cur.matItem[2] = (uint8_t)atoi(val);
    else if (strcmp(key, "matqty3")     == 0) cur.matQty[2]  = (uint8_t)atoi(val);
    else if (strcmp(key, "water_cost")  == 0) cur.resCost[0] = (uint8_t)atoi(val);
    else if (strcmp(key, "food_cost")   == 0) cur.resCost[1] = (uint8_t)atoi(val);
    else if (strcmp(key, "fuel_cost")   == 0) cur.resCost[2] = (uint8_t)atoi(val);
    else if (strcmp(key, "med_cost")    == 0) cur.resCost[3] = (uint8_t)atoi(val);
    else if (strcmp(key, "scrap_cost")  == 0) cur.resCost[4] = (uint8_t)atoi(val);
    else if (strcmp(key, "starter")     == 0) cur.starter    = (uint8_t)(val[0]=='y'||val[0]=='Y'||val[0]=='1');
  }
  commitRecipe(cur, hasRecipe);
  f.close();
}

// Lookup recipe by ID — O(N) scan over loaded registry.
static const RecipeDef* getRecipeDef(uint8_t id) {
  for (int i = 0; i < (int)recipeCount; i++)
    if (recipeRegistry[i].id == id) return &recipeRegistry[i];
  return nullptr;
}

// knownRecipes bits for every `starter = yes` recipe — the common know-how a
// survivor spawns with, no encounter required. Computed off the registry each
// call (it is a 32-entry scan) so flipping the flag in data/recipes.cfg and
// rebooting is enough; no firmware flash, and no SAVE_VERSION bump, because
// tryLoadSave() ORs this into whatever the save recorded.
// Returns 0 before loadRecipeRegistry() has run — which is the case for the
// boot-time resetSurvivor() pass over the empty player slots in setup(). Those
// slots are re-reset on 'pick', long after the registry is up.
static uint32_t starterRecipeMask() {
  uint32_t mask = 0;
  for (int i = 0; i < (int)recipeCount; i++) {
    const RecipeDef& r = recipeRegistry[i];
    if (r.starter && r.id >= 1 && r.id <= MAX_RECIPES) mask |= (1u << (r.id - 1));
  }
  return mask;
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

// Load /data/encounters/index.json → encPools[0..NUM_TERRAIN-1].
// index.json defines only the terrains that actually have an encounter pool
// (0-9 on the surface, 14 for the bunker tunnels); every other slot stays
// count=0, which handleMsg_enc_start treats as "no encounters here".
static void loadEncounterIndex() {
  File f = SD.open("/data/encounters/index.json");
  if (!f) { Log.warning("SD MISSING: /data/encounters/index.json"); return; }
  Log.notice("Encounter index load: size=%u", (unsigned)f.size());
  size_t sz = min((size_t)f.size(), (size_t)1024);  // grew with the tunnel pool
  char* buf = (char*)malloc(sz + 1);
  if (!buf) { Log.error("encounter index malloc FAIL size=%u", (unsigned)(sz+1)); f.close(); return; }
  f.read((uint8_t*)buf, sz);
  buf[sz] = 0;
  f.close();
  memset(encPools, 0, sizeof(encPools));
  for (int t = 0; t < NUM_TERRAIN; t++) {
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

// Load /encounters/loot_tables.json → lootTables[0..MAX_LOOT_TABLES-1]
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
  while (lootTableCount < MAX_LOOT_TABLES && *p) {
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
    while (tbl.count < LOOT_ENTRIES_MAX) {
      const char* entry = strchr(arr, '{');
      if (!entry) break;
      const char* entryEnd = strchr(entry, '}');
      if (!entryEnd) break;
      // The file is pretty-printed, so one entry ({item, qty:[a,b], weight})
      // runs ~90-115 chars.  This buffer must hold the whole entry: truncating
      // it drops the trailing "weight" key, every weight falls back to 0, and
      // rollLootTable() bails on totalW == 0 - silently killing every drop.
      char eb[192];
      int full = (int)(entryEnd - entry);
      int elen = min(full, (int)sizeof(eb) - 1);
      if (full > (int)sizeof(eb) - 1)
        Log.warning("loot table %s entry %d truncated (%d chars)", tbl.name, (int)tbl.count, full);
      strncpy(eb, entry, elen); eb[elen] = 0;
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
  if (lootTableCount >= MAX_LOOT_TABLES && strchr(p, '"')) {
    Log.warning("loot_tables.json has more than %d tables — some were not loaded", MAX_LOOT_TABLES);
  }
  free(buf);
}

// Roll a weighted loot table entry → writes item ID and qty to out params
static void rollLootTable(const char* tableName, uint8_t* outItem, uint8_t* outQty) {
  *outItem = 0; *outQty = 0;
  for (int i = 0; i < lootTableCount; i++) {
    if (strncmp(lootTables[i].name, tableName, 19) != 0) continue;
    LootTable& tbl = lootTables[i];
    if (tbl.count == 0) { Log.warning("loot table %s is empty", tableName); return; }
    int totalW = 0;
    for (int j = 0; j < tbl.count; j++) totalW += tbl.entries[j].weight;
    // Never silent: a table that parsed with no weight drops nothing, forever.
    if (totalW == 0) { Log.warning("loot table %s has zero total weight", tableName); return; }
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
  Log.warning("loot table %s not found (%d loaded)", tableName, (int)lootTableCount);
}

// ── Encounter DN computation (spec §3) ────────────────────────────────────────
static uint8_t computeEncounterDN(int pid, uint8_t baseRisk, uint8_t /*skill*/) {
  int effectiveRisk = min((int)baseRisk, 100);
  if (G.threatClock >= TC_THRESHOLD_A) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_B) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_C) effectiveRisk += 5;
  if (G.threatClock >= TC_THRESHOLD_D) effectiveRisk += 5;
  effectiveRisk = constrain(effectiveRisk, 0, 100);
  // DN curve. The old `2 + risk*10/100` compressed the authored 0-100 risk
  // range into DN 2-12, but only DN 5-10 is actually contestable against
  // 2d6 + skill -- so every choice below risk 30 was a free pass. Authored
  // base_risk has a median of 30, which meant DN 5: a 92% success at skill 1.
  // Measured over 334 authored choices the whole library failed 9% of the
  // time and cost 0.28 LL per choice, which is why encounter hazards killed
  // nobody across 9 bot runs despite 277 of 320 hazards costing LL (mean -3).
  // `5 + risk*7/100` keeps the authors' relative ordering and moves the band
  // onto the dice: risk 30 -> DN 7 (72%), risk 50 -> DN 8 (58%),
  // risk 70 -> DN 9 (42%). Library-wide that is 26% failure, 0.74 LL/choice.
  // Mirrored in data/ui-encounter.js, mock-server/server.js, bots/encounters.py.
  int rawDN = 5 + (effectiveRisk * 7) / 100;
  Player& p = G.players[pid];
  int bonus = 0;
  if (p.ll > 4)        bonus += (p.ll - 4) / 2;
  if (p.radiation > 3) rawDN += (p.radiation - 3) / 2;
  return (uint8_t)constrain(rawDN - bonus, 2, 12);
}
