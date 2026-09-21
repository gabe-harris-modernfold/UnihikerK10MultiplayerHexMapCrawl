#pragma once
// ── Encounter engine: server-authoritative resolution of encounter JSON ──────
// Included from Esp32HexMapCrawl.ino after actions_game_loop.hpp.
//
// The client only ever tells the server *which* choice it picked
// ({"t":"enc_choice","ci":N}), plus how much of the haul it wants to keep when
// it banks ({"t":"enc_bank","keep":[5]}, clamped against the server's own
// pendingLoot in network-msg-encounter.hpp).  Everything that has a gameplay
// consequence —
// the choice's cost, skill, risk, the hazard's penalty, the destination node's
// loot, loot table, can_bank flag and whether it is terminal — is read here
// from the encounter file on the SD card.  The client keeps its own copy of
// the JSON purely for presentation.
//
// The JSON scanner below is deliberately tiny: it understands objects, arrays,
// strings, numbers, true/false/null, and nothing else.  It is scope-aware
// (jsonObjGet() only matches keys at the top level of the given object), which
// is what the old strstr()-based helpers in boot-assets.hpp were not.

// ── Minimal scope-aware JSON scanner ─────────────────────────────────────────
static const char* jsWs(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

// Skip one complete value starting at p.  Returns pointer just past it, or
// nullptr on malformed input.
static const char* jsSkip(const char* p) {
  p = jsWs(p);
  if (!*p) return nullptr;
  if (*p == '"') {
    p++;
    while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
    return *p ? p + 1 : nullptr;
  }
  if (*p == '{' || *p == '[') {
    char close = (*p == '{') ? '}' : ']';
    p++;
    for (;;) {
      p = jsWs(p);
      if (!*p) return nullptr;
      if (*p == close) return p + 1;
      if (*p == ',' || *p == ':') { p++; continue; }
      p = jsSkip(p);
      if (!p) return nullptr;
    }
  }
  // number / true / false / null
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\t' && *p != '\r' && *p != '\n') p++;
  return p;
}

// obj points at '{'.  Returns pointer to the value of `key` at this object's
// top level, or nullptr.
static const char* jsonObjGet(const char* obj, const char* key) {
  if (!obj) return nullptr;
  obj = jsWs(obj);
  if (*obj != '{') return nullptr;
  const char* p = obj + 1;
  size_t klen = strlen(key);
  for (;;) {
    p = jsWs(p);
    if (*p == '}' || !*p) return nullptr;
    if (*p == ',') { p++; continue; }
    if (*p != '"') return nullptr;
    const char* ks = p + 1;
    const char* ke = ks;
    while (*ke && *ke != '"') { if (*ke == '\\' && ke[1]) ke++; ke++; }
    if (!*ke) return nullptr;
    const char* colon = jsWs(ke + 1);
    if (*colon != ':') return nullptr;
    const char* val = jsWs(colon + 1);
    if ((size_t)(ke - ks) == klen && strncmp(ks, key, klen) == 0) return val;
    p = jsSkip(val);
    if (!p) return nullptr;
  }
}

// arr points at '['.  Returns pointer to element idx, or nullptr.
static const char* jsonArrGet(const char* arr, int idx) {
  if (!arr) return nullptr;
  arr = jsWs(arr);
  if (*arr != '[') return nullptr;
  const char* p = arr + 1;
  for (int i = 0;; i++) {
    p = jsWs(p);
    if (*p == ']' || !*p) return nullptr;
    if (*p == ',') { p++; i--; continue; }
    if (i == idx) return p;
    p = jsSkip(p);
    if (!p) return nullptr;
  }
}

// Number of elements in the array at arr (0 if missing / not an array).
static int jsonArrLen(const char* arr) {
  if (!arr) return 0;
  arr = jsWs(arr);
  if (*arr != '[') return 0;
  int n = 0;
  const char* p = arr + 1;
  for (;;) {
    p = jsWs(p);
    if (*p == ']' || !*p) return n;
    if (*p == ',') { p++; continue; }
    n++;
    p = jsSkip(p);
    if (!p) return n;
  }
}

static int  jsonNum(const char* v, int dflt = 0) { return v ? atoi(jsWs(v)) : dflt; }
static bool jsonBool(const char* v) { v = v ? jsWs(v) : v; return v && strncmp(v, "true", 4) == 0; }
static bool jsonCopyStr(const char* v, char* out, int cap) {
  out[0] = 0;
  if (!v) return false;
  v = jsWs(v);
  if (*v != '"') return false;
  v++;
  int i = 0;
  while (*v && *v != '"' && i < cap - 1) out[i++] = *v++;
  out[i] = 0;
  return true;
}

// ── Encounter file access ────────────────────────────────────────────────────
// One PSRAM buffer, reused for every load.  Encounter files are ~2–7 KB.
static constexpr size_t ENC_FILE_CAP = 16384;
static char* encFileBuf = nullptr;

// Load /data/encounters/<biome>/<id>.json into encFileBuf.  Returns the buffer
// (NUL-terminated) or nullptr.  Caller holds G.mutex (SD access is serialised
// behind it everywhere else in the firmware).
static const char* encLoadFile(uint8_t terrain, uint8_t idx) {
  if (terrain >= 10 || idx == 0) return nullptr;
  if (!encFileBuf) {
    encFileBuf = (char*)ps_malloc(ENC_FILE_CAP);
    if (!encFileBuf) encFileBuf = (char*)malloc(ENC_FILE_CAP);
    if (!encFileBuf) { Log.error("encLoadFile: buffer alloc FAIL"); return nullptr; }
  }
  char path[56];
  snprintf(path, sizeof(path), "/data/encounters/%s/%d.json", encPools[terrain].path, (int)idx);
  File f = SD.open(path, FILE_READ);
  if (!f) { Log.error("encLoadFile: SD OPEN FAIL %s", path); return nullptr; }
  size_t sz = f.size();
  if (sz >= ENC_FILE_CAP) {
    Log.error("encLoadFile: %s too large (%u B)", path, (unsigned)sz);
    f.close(); return nullptr;
  }
  size_t got = f.read((uint8_t*)encFileBuf, sz);
  f.close();
  encFileBuf[got] = 0;
  return encFileBuf;
}

// nodes[key] for the loaded file.
static const char* encNode(const char* json, const char* key) {
  return jsonObjGet(jsonObjGet(json, "nodes"), key);
}

// ── Resolved view of one choice ──────────────────────────────────────────────
struct EncChoice {
  int      baseRisk;
  uint8_t  skill;
  int      costLL, costRad, costFood, costWat, costScrap, costMed;
  char     nextKey[ENC_KEY_LEN];
  bool     nextCanBank;
  bool     nextTerminal;
  uint8_t  loot[5];                       // rolled resource loot on the destination node
  uint8_t  itemType[2], itemQty[2];       // rolled "item" loot entries (max two)
  uint8_t  recipeId;                      // "recipe" loot entry — a recipe learned on success
  char     lootTable[20];
  int      hazLL, hazRad;
  uint8_t  hazRes[5];                     // resources taken on failure (amounts)
  uint8_t  hazWMin, hazWMaj;
  bool     hazEnds;
};

// Resolve choice `ci` of node `nodeKey` in the loaded file.  Returns false if
// the node or choice does not exist.
static bool encResolveChoice(const char* json, const char* nodeKey, int ci, EncChoice& out) {
  memset(&out, 0, sizeof(out));
  const char* node    = encNode(json, nodeKey);
  const char* choices = jsonObjGet(node, "choices");
  const char* ch      = jsonArrGet(choices, ci);
  if (!ch) return false;

  out.baseRisk = constrain(jsonNum(jsonObjGet(ch, "base_risk"), 50), 0, 100);
  out.skill    = (uint8_t)constrain(jsonNum(jsonObjGet(ch, "skill"), 0), 0, NUM_SKILLS - 1);

  const char* cost = jsonObjGet(ch, "cost");
  out.costLL    = jsonNum(jsonObjGet(cost, "ll"));
  out.costRad   = jsonNum(jsonObjGet(cost, "radiation"));
  out.costFood  = jsonNum(jsonObjGet(cost, "food"));
  out.costWat   = jsonNum(jsonObjGet(cost, "water"));
  out.costScrap = jsonNum(jsonObjGet(cost, "scrap"));
  out.costMed   = jsonNum(jsonObjGet(cost, "med"));

  // Destination node: loot lives there.
  jsonCopyStr(jsonObjGet(ch, "success_node"), out.nextKey, ENC_KEY_LEN);
  const char* next = out.nextKey[0] ? encNode(json, out.nextKey) : nullptr;
  if (next) {
    out.nextCanBank  = jsonBool(jsonObjGet(next, "can_bank"));
    out.nextTerminal = (jsonArrLen(jsonObjGet(next, "choices")) == 0);
    jsonCopyStr(jsonObjGet(next, "loot_table"), out.lootTable, sizeof(out.lootTable));
    const char* loot = jsonObjGet(next, "loot");
    int n = jsonArrLen(loot);
    int items = 0;
    for (int i = 0; i < n; i++) {
      const char* e   = jsonArrGet(loot, i);
      const char* qty = jsonObjGet(e, "qty");
      int mn = jsonNum(jsonArrGet(qty, 0), 1);
      int mx = jsonNum(jsonArrGet(qty, 1), mn);
      if (mx < mn) mx = mn;
      int q = mn + (mx > mn ? (int)(esp_random() % (uint32_t)(mx - mn + 1)) : 0);
      q = constrain(q, 0, 99);
      const char* resV    = jsonObjGet(e, "res");
      const char* itemV   = jsonObjGet(e, "item");
      const char* recipeV = jsonObjGet(e, "recipe");
      if (resV) {
        int res = jsonNum(resV, -1);
        if (res >= 0 && res < 5) out.loot[res] = (uint8_t)min(99, (int)out.loot[res] + q);
      } else if (itemV && items < 2) {
        int item = jsonNum(itemV, 0);
        if (item > 0 && q > 0) { out.itemType[items] = (uint8_t)item; out.itemQty[items] = (uint8_t)q; items++; }
      } else if (recipeV) {
        // No qty — a recipe is a one-time knowledge grant, not a stack.
        int rid = jsonNum(recipeV, 0);
        if (rid > 0) out.recipeId = (uint8_t)rid;
      }
    }
  } else {
    // A choice with no reachable destination ends the scene as a full clear.
    out.nextTerminal = true;
    out.nextCanBank  = true;
  }

  // Hazard on failure.
  char hazId[ENC_KEY_LEN];
  if (jsonCopyStr(jsonObjGet(ch, "hazard_id"), hazId, sizeof(hazId)) && hazId[0]) {
    const char* haz = jsonObjGet(jsonObjGet(json, "hazards"), hazId);
    const char* pen = jsonObjGet(haz, "penalty");
    out.hazLL  = jsonNum(jsonObjGet(pen, "ll"));
    out.hazRad = jsonNum(jsonObjGet(pen, "radiation"));
    static const char* RES_KEYS[5] = { "water", "food", "fuel", "med", "scrap" };
    for (int i = 0; i < 5; i++) {
      int v = jsonNum(jsonObjGet(pen, RES_KEYS[i]));
      if (v < 0) out.hazRes[i] = (uint8_t)min(99, -v);   // data writes losses as negatives
    }
    const char* wound = jsonObjGet(haz, "wound");
    out.hazWMin = (uint8_t)constrain(jsonNum(jsonArrGet(wound, 0)), 0, (int)WOUND_MAX_EACH);
    out.hazWMaj = (uint8_t)constrain(jsonNum(jsonArrGet(wound, 1)), 0, (int)WOUND_MAX_EACH);
    out.hazEnds = jsonBool(jsonObjGet(haz, "ends_encounter"));
  }
  return true;
}

// Read the start node of the loaded file into the ActiveEncounter.
static void encEnterStartNode(const char* json, ActiveEncounter& enc) {
  jsonCopyStr(jsonObjGet(json, "start_node"), enc.nodeKey, ENC_KEY_LEN);
  const char* node = encNode(json, enc.nodeKey);
  enc.canBank = jsonBool(jsonObjGet(node, "can_bank")) ? 1 : 0;
  if (jsonArrLen(jsonObjGet(node, "choices")) == 0) enc.active |= (1 << 7);
}

// ── End an encounter without banking ─────────────────────────────────────────
// Clears the slot and queues EVT_ENC_END.  With restorePoi the encounter goes
// back on its hex (used when the ending was not the player's choice: dawn,
// disconnect).  Caller holds G.mutex.
static void endEncounter(int pid, uint8_t reason, bool restorePoi) {
  ActiveEncounter& enc = encounters[pid];
  if (!enc.active) return;
  uint8_t hq = enc.hexQ, hr = enc.hexR;
  // Put the POI back on the board it came from. Without enc.depth a tunnel
  // encounter ended involuntarily (dawn, disconnect, hazard) would restore
  // its POI onto a surface hex that happens to share those coordinates.
  if (restorePoi) {
    if (enc.depth) {
      if (hq < TUN_COLS && hr < TUN_ROWS && G.tunnel[hr][hq].poi == 0)
        G.tunnel[hr][hq].poi = enc.encIdx;
    } else if (hq < MAP_COLS && hr < MAP_ROWS && G.map[hr][hq].poi == 0) {
      G.map[hr][hq].poi = enc.encIdx;
    }
  }
  enc = {};
  GameEvent ev = {}; ev.type = EVT_ENC_END; ev.pid = (uint8_t)pid;
  ev.q = (int16_t)hq; ev.r = (int16_t)hr; ev.encOut = reason;
  enqEvt(ev);
}
