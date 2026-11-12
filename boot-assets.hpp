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

// ── Scripts only the bundle needs ─────────────────────────────
// With assets.json on the card the game client loads app.bundle.js and never
// asks for the scripts it was built from; only the web-assets.json fallback,
// for a card without assets.json, loads them one by one. Caching them anyway
// was 337 KB of PSRAM -- more than tunRoom.webp, which went missing for it
// (2026-11-06). A source another page loads by name stays: observer.html
// takes game-data.js, wind.js and ground-fx.js straight off the board.
static const int BUNDLED_NAME_MAX = 32;
static char (*g_bundledOnly)[BUNDLED_NAME_MAX] = nullptr;   // only while loadWebFilesToRAM runs
static int  g_bundledOnlyN = 0;

// A small SD text file in internal RAM, NUL-terminated (free() it). Kept out
// of PSRAM so the hole it leaves is not in the middle of the cache.
static char* sdReadText(const char* path) {
  File f = SD.open(path);
  if (!f) return nullptr;
  size_t sz = f.size();
  char* buf = (char*)heap_caps_malloc(sz + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf) buf[f.read((uint8_t*)buf, sz)] = 0;
  f.close();
  return buf;
}

static void findBundledOnly() {
  char* man = sdReadText("/data/assets.json");
  if (!man) return;                                  // no manifest: the fallback needs every script
  const char* p = strstr(man, "\"sources\"");
  p = p ? strchr(p, '[') : nullptr;
  const char* end = p ? strchr(p, ']') : nullptr;
  if (end) g_bundledOnly = (char(*)[BUNDLED_NAME_MAX])malloc(sizeof(*g_bundledOnly) * MAX_WEB_FILES);
  while (g_bundledOnly && g_bundledOnlyN < MAX_WEB_FILES) {
    const char* a = strchr(p, '"');
    const char* b = a ? strchr(a + 1, '"') : nullptr;
    if (!b || b > end) break;
    size_t n = b - a - 1;
    if (n > 0 && n < BUNDLED_NAME_MAX) {
      memcpy(g_bundledOnly[g_bundledOnlyN], a + 1, n);
      g_bundledOnly[g_bundledOnlyN++][n] = 0;
    }
    p = b + 1;
  }
  free(man);

  // Every other page keeps the sources it names ('wind.js' or "wind.js").
  File dir = SD.open("/data");
  File f = dir ? dir.openNextFile() : File();
  while (f && g_bundledOnlyN) {
    String lower = String(f.name()); lower.toLowerCase();
    String path = String("/data/") + f.name();
    bool page = !f.isDirectory() && lower.endsWith(".html") && lower != "index.html";
    f.close();
    char* text = page ? sdReadText(path.c_str()) : nullptr;
    for (int i = 0; text && i < g_bundledOnlyN; ) {
      char q1[BUNDLED_NAME_MAX + 2], q2[BUNDLED_NAME_MAX + 2];
      snprintf(q1, sizeof(q1), "'%s'", g_bundledOnly[i]);
      snprintf(q2, sizeof(q2), "\"%s\"", g_bundledOnly[i]);
      if (strstr(text, q1) || strstr(text, q2)) {
        LOG_VERBOSE("WEB keep %s (%s loads it)", g_bundledOnly[i], path.c_str());
        memcpy(g_bundledOnly[i], g_bundledOnly[--g_bundledOnlyN], BUNDLED_NAME_MAX);
      } else {
        i++;
      }
    }
    free(text);
    f = dir.openNextFile();
  }
  if (f) f.close();
  if (dir) dir.close();
}

static bool isBundledOnly(const char* lowerName) {
  for (int i = 0; i < g_bundledOnlyN; i++)
    if (strcasecmp(g_bundledOnly[i], lowerName) == 0) return true;
  return false;
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
  if (!mime) { LOG_VERBOSE("WEB skip (not a web asset): %s", fname.c_str()); return; }
  if (isBundledOnly(plainLower.c_str())) { LOG_VERBOSE("WEB skip (in app.bundle.js): %s", fname.c_str()); return; }

  char url[48];
  String plainName = gz ? fname.substring(0, fname.length() - 3) : fname;
  if (plainName.length() + 1 >= sizeof(url)) {
    Log.error("WEB skip (name too long): %s", fname.c_str()); return;
  }
  snprintf(url, sizeof(url), "/%s", plainName.c_str());

  int existing = findWebFile(url);
  if (existing >= 0) {
    if (webFiles[existing].gzip) {                              // gz already cached — plain loses
      LOG_VERBOSE("WEB skip (gz sibling cached): %s", fname.c_str()); return;
    }
    if (!gz) return;                                            // duplicate plain (case-diff) — ignore
  }
  // A plain file whose .gz is also on the card: the gz wins anyway, so don't
  // read it in only to free it. For app.bundle.js that was a 1 MB block, read
  // off the card and dropped at every boot.
  if (!gz && existing < 0 && SD.exists(String("/data/") + fname + ".gz")) {
    LOG_VERBOSE("WEB skip (gz sibling on the card): %s", fname.c_str()); return;
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
// WebFile comment in Esp32HexMapCrawl.ino), less the scripts only the bundle
// needs (findBundledOnly). Order of discovery doesn't matter; gz-vs-plain
// preference is resolved in cacheWebFile().
// A per-file terrain tile (hex<Name><N>.png), landmark (poi_*.png), shelter
// sprite (shelter<Kind><N>.png, plus the unused basicShelter<N>.png copies)
// or forage animal (forrageAnimal<N>.png) that the sprite sheets replaced.
// An SD synced before them still has ~100; with tiles.json on the card they
// would only burn PSRAM and cache slots. The sheets are .webp, so never match.
static bool isLegacyTileFile(const char* name) {
  // The pre-picker alternates page (tile-alts0.webp + tile-alts.json): the
  // picker reads alternates out of tiles.json now and nothing loads it
  // (docs/hex-tile-art.md), but its 288 KB cost four survivors their portraits.
  if (strncmp(name, "tile-alts", 9) == 0) return true;
  const size_t n = strlen(name);
  if (n < 5 || strcasecmp(name + n - 4, ".png") != 0) return false;
  return strncmp(name, "hex", 3) == 0 || strncmp(name, "poi_", 4) == 0 ||
         strncmp(name, "shelter", 7) == 0 || strncmp(name, "basicShelter", 12) == 0 ||
         strncmp(name, "forrageAnimal", 13) == 0;
}

// Only what the /img/ route serves goes in the cache. The prompt notes kept
// beside the art (HEX_TILE_PROMPTS.md, items/ICON_PROMPTS.md, ...) were 44 KB
// of PSRAM no browser ever asks for.
static bool isImgAsset(const char* name) {
  const char* dot = strrchr(name, '.');
  return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".webp") || !strcasecmp(dot, ".jpg") ||
                 !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".json"));
}

// Read one /data/img file into PSRAM under `cacheName` ("x.webp", "survivors/x.jpg").
static void cacheImgFile(File& file, const char* cacheName) {
  if (!isImgAsset(cacheName)) { LOG_VERBOSE("IMG skip (not an image asset): %s", cacheName); return; }
  size_t sz = file.size();
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) { Log.error("IMG cache ps_malloc FAIL size=%u name=%s", (unsigned)sz, cacheName); return; }
  file.read(buf, sz);
  strncpy(imgCache[imgCacheCount].name, cacheName, 39);
  imgCache[imgCacheCount].name[39] = 0;
  imgCache[imgCacheCount].buf = buf;
  imgCache[imgCacheCount].len = sz;
  LOG_VERBOSE("IMG cache: %s (%u B)", imgCache[imgCacheCount].name, (unsigned)sz);
  imgCacheCount++;
}

static void loadWebFilesToRAM() {
  File dir = SD.open("/data");
  if (!dir) { Log.error("SD OPEN FAIL: /data"); return; }
  findBundledOnly();
  const int bundledOnly = g_bundledOnlyN;
  const bool atlas = SD.exists("/data/img/tiles.json");
  int legacySkipped = 0;
  File f = dir.openNextFile();
  while (f) {
    String fname = String(f.name());
    if (f.isDirectory() && fname.equalsIgnoreCase("img")) {
      // Two passes: the subfolders first (survivors/, items/: small, and on
      // every player's screen), then the sheets. When new art outgrows PSRAM
      // it is whatever loads last that goes missing -- better one sheet than
      // every survivor's face, which is how the portraits went on 2026-11-06.
      for (int pass = 0; pass < 2; pass++) {
        f.rewindDirectory();
        File imgFile = f.openNextFile();
        while (imgFile && imgCacheCount < MAX_IMG_CACHE) {
          if (imgFile.isDirectory()) {
            if (pass == 0) {
              String subDirName = String(imgFile.name());
              File subFile = imgFile.openNextFile();
              while (subFile && imgCacheCount < MAX_IMG_CACHE) {
                if (!subFile.isDirectory()) {
                  char cacheName[40];
                  snprintf(cacheName, sizeof(cacheName), "%s/%s", subDirName.c_str(), subFile.name());
                  cacheImgFile(subFile, cacheName);
                }
                subFile.close();
                subFile = imgFile.openNextFile();
              }
            }
          } else if (pass == 1) {
            if (atlas && isLegacyTileFile(imgFile.name())) legacySkipped++;
            else cacheImgFile(imgFile, imgFile.name());
          }
          imgFile.close();
          imgFile = f.openNextFile();
        }
      }
    } else if (!f.isDirectory()) {
      cacheWebFile(f, fname);
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  free(g_bundledOnly);
  g_bundledOnly = nullptr;
  g_bundledOnlyN = 0;
  if (bundledOnly)
    Log.notice("WEB cache: %d scripts left on the card, app.bundle.js carries them", bundledOnly);
  if (legacySkipped)
    Log.notice("IMG cache: tile atlas present, skipped %d superseded per-file tiles (safe to delete from SD)",
               legacySkipped);
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
    else if (strcmp(key, "ap")       == 0) cur.statMods[STAT_AP]      = (int8_t)atoi(val);
    else if (strcmp(key, "slots")    == 0) cur.statMods[STAT_SLOTS]   = (int8_t)atoi(val);
    else if (strcmp(key, "water_cap") == 0) cur.statMods[STAT_WATER_CAP] = (int8_t)atoi(val);
    else if (strcmp(key, "water_cost") == 0) cur.opCost[0] = (uint8_t)atoi(val);
    else if (strcmp(key, "food_cost")  == 0) cur.opCost[1] = (uint8_t)atoi(val);
    else if (strcmp(key, "fuel_cost")  == 0) cur.opCost[2] = (uint8_t)atoi(val);
    else if (strcmp(key, "med_cost")   == 0) cur.opCost[3] = (uint8_t)atoi(val);
    else if (strcmp(key, "scrap_cost") == 0) cur.opCost[4] = (uint8_t)atoi(val);
    else if (strcmp(key, "terrain")  == 0) cur.passTerrainBits = (uint8_t)atoi(val);
    else if (strcmp(key, "tracks")   == 0) cur.leavesTracks    = (val[0] == 'y' || val[0] == 'Y' || val[0] == '1') ? 1 : 0;
    else if (strcmp(key, "cursed")   == 0) cur.cursed          = (val[0] == 'y' || val[0] == 'Y' || val[0] == '1') ? 1 : 0;
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
    else if (strcmp(key, "output_res")  == 0) {
      // Token output: water | food | fuel | med | scrap (RES order, +1 so 0 = none).
      static const char* const RES_KEY[5] = { "water", "food", "fuel", "med", "scrap" };
      for (int k = 0; k < 5; k++)
        if (strncasecmp(val, RES_KEY[k], strlen(RES_KEY[k])) == 0) { cur.outputRes = (uint8_t)(k + 1); break; }
    }
    else if (strcmp(key, "known_by")    == 0) {
      // Archetype names (ARCHETYPE_NAME), comma-separated: those survivors
      // spawn knowing this recipe, the way everybody knows a starter one.
      char names[64];
      strncpy(names, val, sizeof(names) - 1); names[sizeof(names) - 1] = 0;
      for (char* tok = strtok(names, ", \t\r"); tok; tok = strtok(nullptr, ", \t\r")) {
        bool hit = false;
        for (int a = 0; a < NUM_ARCHETYPES; a++)
          if (strcasecmp(tok, ARCHETYPE_NAME[a]) == 0) { cur.archMask |= (uint8_t)(1u << a); hit = true; }
        if (!hit) Log.warning("recipes.cfg: id=%d known_by names no archetype '%s'", (int)cur.id, tok);
      }
    }
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

// knownRecipes bits a survivor of archetype `arch` spawns with on top of the
// starter ones: every recipe whose `known_by` names that archetype (the
// Medic's Medicine, Super Juice and Trauma Patch). Same registry-scan, same
// no-flash / no-SAVE_VERSION story as starterRecipeMask(): tryLoadSave() ORs
// it in too.
static uint32_t archRecipeMask(uint8_t arch) {
  if (arch >= NUM_ARCHETYPES) return 0;
  uint32_t mask = 0;
  for (int i = 0; i < (int)recipeCount; i++) {
    const RecipeDef& r = recipeRegistry[i];
    if ((r.archMask & (1u << arch)) && r.id >= 1 && r.id <= MAX_RECIPES) mask |= (1u << (r.id - 1));
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
  size_t sz = min((size_t)f.size(), (size_t)1536);  // grew with the tunnel pool, then the traps
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
  // The booby-trap pool (traps.hpp): not keyed by terrain, so it has a name
  // of its own, and "tiers" -- file counts per tier, cheap first -- which
  // become contiguous id ranges so worldgen can pick a tier without opening
  // a single file:  "traps": {"count": 20, "path": "traps", "tiers": [6, 9, 5]}
  // No "tiers" means one undivided pool, dealt as cheap.
  for (int t = 0; t < TRAP_TIERS; t++) { trapTierLo[t] = 1; trapTierHi[t] = 0; }
  if (const char* te = strstr(buf, "\"traps\"")) {
    te += 7;
    int copyLen = min(120, (int)(sz - (size_t)(te - buf)));
    char tmp[124]; strncpy(tmp, te, copyLen); tmp[copyLen] = 0;
    if (char* close = strchr(tmp, '}')) *close = 0;   // this entry only
    EncPoolInfo& tp = encPools[ENC_POOL_TRAP];
    const char* cv = jsonFindKey(tmp, "count");
    const char* pv = jsonFindKey(tmp, "path");
    if (cv) tp.count = (uint8_t)jsonInt(cv);
    if (pv) jsonStr(pv, tp.path, sizeof(tp.path));
    int next = 1;
    const char* tv = jsonFindKey(tmp, "tiers");
    const char* lb = tv ? strchr(tv, '[') : nullptr;
    if (lb) {
      const char* q = lb + 1;
      for (int t = 0; t < TRAP_TIERS && next <= tp.count; t++) {
        while (*q == ' ') q++;
        int n = atoi(q);
        if (n > 0) {
          trapTierLo[t] = (uint8_t)next;
          trapTierHi[t] = (uint8_t)min((int)tp.count, next + n - 1);
          next = trapTierHi[t] + 1;
        }
        const char* comma = strchr(q, ',');
        if (!comma) break;
        q = comma + 1;
      }
    } else if (tp.count) {
      trapTierLo[0] = 1; trapTierHi[0] = tp.count;
    }
    Log.notice("Trap pool: %d files at /%s, tiers %d-%d / %d-%d / %d-%d", (int)tp.count, tp.path,
               (int)trapTierLo[0], (int)trapTierHi[0], (int)trapTierLo[1], (int)trapTierHi[1],
               (int)trapTierLo[2], (int)trapTierHi[2]);
  }
  // Machine Gun Kane's chapters (kane.hpp): named like the traps, file N =
  // chapter N.   "kane": {"count": 5, "path": "kane"}
  if (const char* ke = strstr(buf, "\"kane\"")) {
    ke += 6;
    int copyLen = min(80, (int)(sz - (size_t)(ke - buf)));
    char tmp[84]; strncpy(tmp, ke, copyLen); tmp[copyLen] = 0;
    if (char* close = strchr(tmp, '}')) *close = 0;   // this entry only
    EncPoolInfo& kp = encPools[ENC_POOL_KANE];
    const char* cv = jsonFindKey(tmp, "count");
    const char* pv = jsonFindKey(tmp, "path");
    if (cv) kp.count = (uint8_t)jsonInt(cv);
    if (pv) jsonStr(pv, kp.path, sizeof(kp.path));
    Log.notice("Kane pool: %d chapters at /%s", (int)kp.count, kp.path);
  }
  // VACANCY's four parts (vacancy.hpp), file N = part N.
  //   "vacancy": {"count": 4, "path": "vacancy"}
  if (const char* ve = strstr(buf, "\"vacancy\"")) {
    ve += 9;
    int copyLen = min(80, (int)(sz - (size_t)(ve - buf)));
    char tmp[84]; strncpy(tmp, ve, copyLen); tmp[copyLen] = 0;
    if (char* close = strchr(tmp, '}')) *close = 0;   // this entry only
    EncPoolInfo& vp = encPools[ENC_POOL_VAC];
    const char* cv = jsonFindKey(tmp, "count");
    const char* pv = jsonFindKey(tmp, "path");
    if (cv) vp.count = (uint8_t)jsonInt(cv);
    if (pv) jsonStr(pv, vp.path, sizeof(vp.path));
    Log.notice("VACANCY pool: %d parts at /%s", (int)vp.count, vp.path);
  }
  // RUNAWAY SCIENCE's four parts (lab.hpp), file N = part N.
  //   "lab": {"count": 4, "path": "lab"}
  if (const char* le = strstr(buf, "\"lab\"")) {
    le += 5;
    int copyLen = min(80, (int)(sz - (size_t)(le - buf)));
    char tmp[84]; strncpy(tmp, le, copyLen); tmp[copyLen] = 0;
    if (char* close = strchr(tmp, '}')) *close = 0;   // this entry only
    EncPoolInfo& lp = encPools[ENC_POOL_LAB];
    const char* cv = jsonFindKey(tmp, "count");
    const char* pv = jsonFindKey(tmp, "path");
    if (cv) lp.count = (uint8_t)jsonInt(cv);
    if (pv) jsonStr(pv, lp.path, sizeof(lp.path));
    Log.notice("RUNAWAY SCIENCE pool: %d parts at /%s", (int)lp.count, lp.path);
  }
  // ONE-HAND BONNEY's two sections (bonney.hpp), file N = section N.
  //   "bonney": {"count": 2, "path": "bonney"}
  if (const char* be = strstr(buf, "\"bonney\"")) {
    be += 8;
    int copyLen = min(80, (int)(sz - (size_t)(be - buf)));
    char tmp[84]; strncpy(tmp, be, copyLen); tmp[copyLen] = 0;
    if (char* close = strchr(tmp, '}')) *close = 0;   // this entry only
    EncPoolInfo& bp = encPools[ENC_POOL_BONNEY];
    const char* cv = jsonFindKey(tmp, "count");
    const char* pv = jsonFindKey(tmp, "path");
    if (cv) bp.count = (uint8_t)jsonInt(cv);
    if (pv) jsonStr(pv, bp.path, sizeof(bp.path));
    Log.notice("ONE-HAND BONNEY pool: %d sections at /%s", (int)bp.count, bp.path);
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
