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
  // number / true / false / null. An empty one is malformed: a stray closing
  // bracket of the wrong kind ("[1, 2}") stops here without moving, and the
  // object/array loop above -- and jsonArrLen's -- would spin on it forever,
  // on async_tcp with G.mutex held.
  const char* s = p;
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\t' && *p != '\r' && *p != '\n') p++;
  return p == s ? nullptr : p;
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

// A penalty that is either a scalar or a 2-array range: "ll": -2 or
// "ll": [-2, -4]. A range is rolled uniformly, inclusive, whichever way round
// its ends are written -- the data writes losses as negatives, so [-2, -4]
// reads "two to four" and must not be treated as an empty range. This is what
// lets damage vary *within* one hazard rather than only across hazards
// (docs/trap-system-spec.md, "Schema additions").
static int jsonRoll(const char* v, int dflt = 0) {
  if (!v) return dflt;
  v = jsWs(v);
  if (*v != '[') return jsonNum(v, dflt);
  int a = jsonNum(jsonArrGet(v, 0), dflt);
  int b = jsonNum(jsonArrGet(v, 1), a);
  int lo = min(a, b), hi = max(a, b);
  return lo + (hi > lo ? (int)(esp_random() % (uint32_t)(hi - lo + 1)) : 0);
}
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
// One PSRAM buffer, reused for every load.  Encounter files are ~2–12 KB.
static constexpr size_t ENC_FILE_CAP = 16384;
static char* encFileBuf = nullptr;

// Taken at boot (setupWiFiAndServer), before PSRAM fragments: taken lazily at
// the first scene, late in a session, it could find no 16 KB PSRAM block and
// sit on the internal heap for good. encLoadFile() still calls it, for a boot
// that loads a scene before the server is up.
static void encAllocFileBuf() {
  if (encFileBuf) return;
  encFileBuf = (char*)ps_malloc(ENC_FILE_CAP);
  if (!encFileBuf) encFileBuf = (char*)malloc(ENC_FILE_CAP);
  if (!encFileBuf) Log.error("encLoadFile: buffer alloc FAIL");
}

// Load /data/encounters/<biome>/<id>.json into encFileBuf.  Returns the buffer
// (NUL-terminated) or nullptr.  Caller holds G.mutex, which guards encFileBuf.
// The card itself is serialised by FATFS: saves write outside G.mutex now
// (save-stage.hpp), so a load that lands mid-save waits for that file's write.
//
// `pool` indexes encPools[]: a terrain id, or ENC_POOL_TRAP. This used to
// refuse anything >= 10, which quietly made the bunker tunnel pool (14)
// unloadable even once index.json defined it -- any pool index.json actually
// filled in is legal now.
//
// Every roll re-reads its scene's file (encRunChoice), so the buffer remembers
// which file it holds: with one survivor in a scene that is the same file
// every time, and the card is not touched again. A file that will not open,
// or comes back short, is asked for again before giving up -- an intermittent
// SD open failure used to refuse the scene ("The way in is blocked") or the
// choice ("That choice is not open to you") outright.
static uint8_t       encFilePool = 0xFF, encFileIdx = 0;   // what encFileBuf holds (0xFF: nothing)
static volatile bool encFileStale = false;                 // /upload wrote something: re-read

// /upload (game-server.hpp) calls this once a file has landed: the copy in
// encFileBuf may be the one it just replaced.
static void encFileCacheDrop() { encFileStale = true; }

static const char* encLoadFile(uint8_t pool, uint8_t idx) {
  if (pool >= ENC_POOL_COUNT || idx == 0) return nullptr;
  if (encPools[pool].count == 0 || !encPools[pool].path[0]) return nullptr;
  encAllocFileBuf();
  if (!encFileBuf) return nullptr;
  if (encFileStale) { encFileStale = false; encFilePool = 0xFF; }
  if (pool == encFilePool && idx == encFileIdx) return encFileBuf;
  encFilePool = 0xFF;   // the buffer is about to be overwritten
  char path[56];
  snprintf(path, sizeof(path), "/data/encounters/%s/%d.json", encPools[pool].path, (int)idx);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) vTaskDelay(pdMS_TO_TICKS(4));
    File f = SD.open(path, FILE_READ);
    if (!f) {
      Log.error("encLoadFile: SD OPEN FAIL %s (try %d)", path, attempt + 1);
      // Not there at all: asking again will not put it there.
      if (!SD.exists(path)) return nullptr;
      continue;
    }
    size_t sz = f.size();
    if (sz == 0 || sz >= ENC_FILE_CAP) {
      Log.error("encLoadFile: %s bad size (%u B)", path, (unsigned)sz);
      f.close(); return nullptr;
    }
    size_t got = f.read((uint8_t*)encFileBuf, sz);
    f.close();
    encFileBuf[got] = 0;
    if (got != sz) { Log.error("encLoadFile: %s short read %u/%u (try %d)", path, (unsigned)got, (unsigned)sz, attempt + 1); continue; }
    encFilePool = pool; encFileIdx = idx;
    return encFileBuf;
  }
  return nullptr;
}

// A surface POI is a file id in the pool of the terrain its hex had at
// worldgen (hex-map.hpp Phase 5, one hex per file). The terrain can change
// under it at runtime and none of those paths touch poi: a burnout turns
// Scrub, Forest, Marsh, Urban, Ridge and Settlement into Ash Dunes (8 files),
// a flood turns Scrub/Ridge into Marsh and Marsh into Flooded, a founding
// makes a Settlement, a quake unmakes one. An Urban 17 that burned then asked
// for dunes/17.json, which does not exist -- "The way in is blocked", and the
// POI stayed on the map to refuse every visit. Fold the id into the pool the
// hex is in now: the scene matches the ground the survivor is standing on.
static uint8_t encPoolId(uint8_t pool, uint8_t idx) {
  const uint8_t n = pool < ENC_POOL_COUNT ? encPools[pool].count : 0;
  if (!n || idx <= n) return idx;
  return (uint8_t)((idx - 1) % n + 1);
}

// Open a scene for a surface or tunnel POI: file `want` of `pool`, and if that
// one will not load (missing off this card, a sync that lagged index.json,
// a card that keeps failing it), the next one in the pool that does. A POI is
// "a place worth going into", not a promise of one file: any scene of the
// ground it stands on keeps the place open, where refusing it left the POI on
// the map to refuse every visit. *outIdx is the file that loaded, which the
// scene keeps (enc.encIdx) so its choices and the client read the same one.
static const char* encOpenPoolFile(uint8_t pool, uint8_t want, uint8_t* outIdx) {
  const uint8_t n = pool < ENC_POOL_COUNT ? encPools[pool].count : 0;
  if (!n) return nullptr;
  if (want < 1 || want > n) want = 1;
  for (uint8_t k = 0; k < n; k++) {
    const uint8_t idx = (uint8_t)((want - 1 + k) % n + 1);
    const char* json = encLoadFile(pool, idx);
    if (json) {
      if (k) Log.warning("encOpenPoolFile: %s/%d would not load, opened %d", encPools[pool].path, (int)want, (int)idx);
      *outIdx = idx;
      return json;
    }
  }
  return nullptr;
}

// The enc_path that tells pid's client which file its scene is. Every kind
// of scene sends it (enc_start, and a step that opened a trap, a Kane chapter,
// a VACANCY part, a clinic room or Bonney's fire). `resume`: the scene is
// already under way -- the client lost it (its enc_path dropped, its /enc
// fetch hung) and asked again -- so it also says where it is and what is on
// the tray: "rs":1, "node", "pl" (tokens), "pi" ([item, qty] pairs), "pr"
// (recipe bits), "as" (allies behind the next roll). Returns the length, 0
// when pid is in no scene. Caller holds G.mutex.
static int encPathJson(int pid, char* buf, int cap, bool resume) {
  const ActiveEncounter& e = encounters[pid];
  if (!(e.active & 1) || e.terrain >= ENC_POOL_COUNT) { if (cap) buf[0] = 0; return 0; }
  int n = appendFmt(buf, cap, 0, "{\"t\":\"enc_path\",\"biome\":\"%s\",\"id\":%d",
                    encPools[e.terrain].path, (int)e.encIdx);
  if      (e.trap)   n = appendFmt(buf, cap, n, ",\"trap\":1");
  else if (e.kane)   n = appendFmt(buf, cap, n, ",\"kane\":%d", (int)e.kane);
  else if (e.vac)    n = appendFmt(buf, cap, n, ",\"vac\":%d", (int)e.vac);
  else if (e.lab)    n = appendFmt(buf, cap, n, ",\"lab\":%d", (int)e.lab);
  else if (e.bonney) n = appendFmt(buf, cap, n, ",\"bonney\":%d,\"apple\":%d,\"own\":%d", (int)e.bonney,
                                   (int)e.bonApple, (e.bonApple && e.bonFrom == pid) ? 1 : 0);
  if (resume) {
    n = appendFmt(buf, cap, n, ",\"rs\":1,\"node\":\"%s\",\"tm\":%d,\"pl\":[%d,%d,%d,%d,%d],\"pi\":[",
                  e.nodeKey, (e.active & (1 << 7)) ? 1 : 0, e.pendingLoot[0], e.pendingLoot[1],
                  e.pendingLoot[2], e.pendingLoot[3], e.pendingLoot[4]);
    for (int j = 0; j < e.pendingItemCount && j < ENC_MAX_ITEMS; j++)
      n = appendFmt(buf, cap, n, j ? ",[%d,%d]" : "[%d,%d]", (int)e.pendingItemType[j], (int)e.pendingItemQty[j]);
    n = appendFmt(buf, cap, n, "],\"pr\":%lu,\"as\":%d", (unsigned long)e.pendingRecipes,
                  (int)__builtin_popcount(e.assistMask));
  }
  return appendFmt(buf, cap, n, "}");
}

// nodes[key] for the loaded file.
static const char* encNode(const char* json, const char* key) {
  return jsonObjGet(jsonObjGet(json, "nodes"), key);
}

// ── Resolved view of one choice ──────────────────────────────────────────────
struct EncChoice {
  int      baseRisk;
  uint8_t  skill;
  uint8_t  reqItem;                       // "requires_item": open only to a survivor who has it (0 = anyone)
  bool     spendItem;                     // "spends_item": true -- a success hands one reqItem over (a trade)
  int      costLL, costRad, costFood, costWat, costScrap, costMed;
  int      costScore;                     // cost "score": points (the score) spent -- RUNAWAY SCIENCE's currency
  bool     sure;                          // "sure": true -- a price paid, not a gamble: never rolled, always through
  char     nextKey[ENC_KEY_LEN];
  bool     nextCanBank;
  bool     nextTerminal;
  bool     nextEscape;                    // destination carries "escape": true
  bool     nextLights;                    // destination carries "lights": true -- a tunnel panel's switch (tunnels.hpp)
  uint8_t  loot[5];                       // rolled resource loot on the destination node
  uint8_t  itemType[2], itemQty[2];       // rolled "item" loot entries (max two)
  uint8_t  recipeId;                      // "recipe" loot entry — a recipe learned on success
  char     lootTable[20];
  int      hazLL, hazRad;
  uint8_t  hazRes[5];                     // resources taken on failure (amounts)
  uint8_t  hazWMin, hazWMaj;
  bool     hazEnds;
  bool     hazDsh;                        // hazard carries "dshingus": true -- the failure gives it (dshingus.hpp)
  bool     hazSteal;                      // hazard penalty "steal": 1 -- Pinch lifts one thing (bonney.hpp)
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
  // An item id, not a cost: nothing is spent. What "has it" means is
  // playerHasForChoice() (inventory_items.hpp) -- worn for equipment, in the
  // pack for anything else (docs/null-meridian-group.md).
  out.reqItem  = (uint8_t)constrain(jsonNum(jsonObjGet(ch, "requires_item"), 0), 0, 255);
  // ...unless the choice also says "spends_item": true. Then it is a trade:
  // a successful roll takes one of it out of the pack (spendItemForChoice).
  // A failed roll keeps it. Never on equipment, which the gate wants worn.
  out.spendItem = out.reqItem && jsonBool(jsonObjGet(ch, "spends_item"));

  const char* cost = jsonObjGet(ch, "cost");
  out.costLL    = jsonNum(jsonObjGet(cost, "ll"));
  out.costRad   = jsonNum(jsonObjGet(cost, "radiation"));
  out.costFood  = jsonNum(jsonObjGet(cost, "food"));
  out.costWat   = jsonNum(jsonObjGet(cost, "water"));
  out.costScrap = jsonNum(jsonObjGet(cost, "scrap"));
  out.costMed   = jsonNum(jsonObjGet(cost, "med"));
  out.costScore = jsonNum(jsonObjGet(cost, "score"));
  out.sure      = jsonBool(jsonObjGet(ch, "sure"));

  // Destination node: loot lives there.
  jsonCopyStr(jsonObjGet(ch, "success_node"), out.nextKey, ENC_KEY_LEN);
  const char* next = out.nextKey[0] ? encNode(json, out.nextKey) : nullptr;
  if (next) {
    out.nextCanBank  = jsonBool(jsonObjGet(next, "can_bank"));
    out.nextTerminal = (jsonArrLen(jsonObjGet(next, "choices")) == 0);
    out.nextEscape   = jsonBool(jsonObjGet(next, "escape"));
    out.nextLights   = jsonBool(jsonObjGet(next, "lights"));
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

  // Hazard on failure. Every number here may be a range (jsonRoll), and it is
  // rolled now, once, whether or not the check then fails -- the resolved
  // view is only read on the failure path, so an unused roll costs nothing.
  char hazId[ENC_KEY_LEN];
  if (jsonCopyStr(jsonObjGet(ch, "hazard_id"), hazId, sizeof(hazId)) && hazId[0]) {
    const char* haz = jsonObjGet(jsonObjGet(json, "hazards"), hazId);
    const char* pen = jsonObjGet(haz, "penalty");
    out.hazLL  = jsonRoll(jsonObjGet(pen, "ll"));
    out.hazRad = jsonRoll(jsonObjGet(pen, "radiation"));
    static const char* RES_KEYS[5] = { "water", "food", "fuel", "med", "scrap" };
    for (int i = 0; i < 5; i++) {
      int v = jsonRoll(jsonObjGet(pen, RES_KEYS[i]));
      if (v < 0) out.hazRes[i] = (uint8_t)min(99, -v);   // data writes losses as negatives
    }
    // "wound" is the floor per tier [minor, major]; the optional "wound_max"
    // alongside it is the ceiling, and each tier rolls between the two. A
    // separate key rather than nested arrays inside "wound", so every file
    // written before it still reads exactly as it did.
    const char* wound = jsonObjGet(haz, "wound");
    const char* wmax  = jsonObjGet(haz, "wound_max");
    int wlo[2], whi[2];
    for (int t = 0; t < 2; t++) {
      wlo[t] = constrain(jsonNum(jsonArrGet(wound, t)), 0, (int)WOUND_MAX_EACH);
      whi[t] = wmax ? constrain(jsonNum(jsonArrGet(wmax, t), wlo[t]), 0, (int)WOUND_MAX_EACH) : wlo[t];
      if (whi[t] < wlo[t]) whi[t] = wlo[t];
      if (whi[t] > wlo[t]) wlo[t] += (int)(esp_random() % (uint32_t)(whi[t] - wlo[t] + 1));
    }
    out.hazWMin = (uint8_t)wlo[0];
    out.hazWMaj = (uint8_t)wlo[1];
    out.hazEnds = jsonBool(jsonObjGet(haz, "ends_encounter"));
    out.hazDsh  = jsonBool(jsonObjGet(haz, "dshingus"));
    out.hazSteal = jsonNum(jsonObjGet(pen, "steal")) > 0;
  }
  return true;
}

// Index of the first choice at nodeKey whose destination carries "escape":
// true -- a trap's "back out" door -- or -1 when this node has none.
static int encEscapeChoice(const char* json, const char* nodeKey) {
  const char* choices = jsonObjGet(encNode(json, nodeKey), "choices");
  int n = jsonArrLen(choices);
  for (int i = 0; i < n; i++) {
    char key[ENC_KEY_LEN];
    jsonCopyStr(jsonObjGet(jsonArrGet(choices, i), "success_node"), key, sizeof(key));
    if (key[0] && jsonBool(jsonObjGet(encNode(json, key), "escape"))) return i;
  }
  return -1;
}

// Read the start node of the loaded file into the ActiveEncounter.
static void encEnterStartNode(const char* json, ActiveEncounter& enc) {
  jsonCopyStr(jsonObjGet(json, "start_node"), enc.nodeKey, ENC_KEY_LEN);
  const char* node = encNode(json, enc.nodeKey);
  enc.canBank = jsonBool(jsonObjGet(node, "can_bank")) ? 1 : 0;
  enc.escape  = jsonBool(jsonObjGet(node, "escape")) ? 1 : 0;
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
  // A trap has no POI to put back; its hex state is traps.hpp's business.
  // An involuntary end re-arms it exactly as it was (nobody learned
  // anything), a hazard or a fall means it went off. ESCAPED was settled by
  // the caller (the survivor now knows where it is); REGEN has no board left.
  if (enc.trap) {
    if (restorePoi) trapSettle(pid, TRAP_SETTLE_REARM);
    else if (reason == ENC_END_HAZARD || reason == ENC_END_DOWNED) trapSettle(pid, TRAP_OUT_SPRUNG);
    restorePoi = false;
  }
  // A Kane chapter (kane.hpp) never took a POI: its camp is world state and
  // stays put until somebody reads to the end.
  // Nor a VACANCY part (vacancy.hpp): the sign and its sites are world state.
  // Nor a RUNAWAY SCIENCE part (lab.hpp): the clinic is world state too.
  // Nor a ONE-HAND BONNEY section (bonney.hpp): her fire is.
  if (enc.kane || enc.vac || enc.lab || enc.bonney) restorePoi = false;
  // A tunnel control panel is a fixture, not a cache (tunnels.hpp, "Section
  // lights"): the scene only closes for good once its section is lit. Driven
  // out, downed or walked away short of the switch, the panel stays.
  // Not a clinic room (lab.hpp): every cell of a dark section reads as a dark
  // panel here, and a lab scene must never leave a panel's POI behind.
  if (enc.depth && !enc.trap && !enc.lab && reason != ENC_END_REGEN && tunnelPanelDark(hq, hr)) restorePoi = true;
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
