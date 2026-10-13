// ── VACANCY (docs/vacancy.md) ───────────────────────────────────────────────
// The settlement mini-quest. Every board puts a motel sign over one random
// Settlement -- VACANCY in pink neon, a dark NO under it, no motel -- and its
// houses start checking in, one a night, until the town is full.
//
// Four scenes, from the "vacancy" pool in index.json (file = part):
//   1 Vacancy        the town hex. Reading it to the end starts the nights
//                    counting and places the other two (vacPlaceSites).
//   2 The Front Desk open ground, VAC_SITE_MIN..MAX hexes out. Pays the Guest
//                    Register (70).
//   3 Housekeeping   a ruin. Pays the Master Key (71).
//   4 Check-Out      the town hex again, opened only for a survivor carrying
//                    both. Its gated choices need one each; its last node's
//                    "vac_ending": "saved" puts the sign out and spends both.
// Scenes open server-side on arrival, like Kane's chapters (kane.hpp): one
// reader per part at a time, and no POI is taken. Parts 1 and 4 count when
// their last node is reached (vacPartRead); 2 and 3 count when their item is
// BANKED (vacOnBank), so walking off with the book still on the counter
// leaves the desk where it was.
//
// The nights: every dawn somebody is connected for, one more house checks in
// (VAC.rooms--). At none left the sign says NO VACANCY and the town falls:
// its hex becomes Broken Urban, for good, until the board is regenerated. A
// dawn with nobody connected counts nothing -- the sign only buzzes for an
// audience. A quake or a debris strike that levels the town first ends the
// quest the same way, without the sign's help (VAC_EV_LOST).
//
// The quest is the world's, not a survivor's: one sign, one count, shared.
// wInit() (boot, regen, and before a save is restored) rolls a new town.
// Persistence is its own file, /save/vac.bin, like kane.bin -- no
// SAVE_VERSION bump; a save without it gets the freshly rolled sign.
//
// Caller holds G.mutex for everything here.

static constexpr uint8_t VAC_ROOMS         = 8;    // nights from the first reading to NO VACANCY
static constexpr uint8_t VAC_SITE_MIN      = 4;    // the desk and the corridor: hexes from the town
static constexpr uint8_t VAC_SITE_MAX      = 8;
static constexpr uint8_t VAC_SITE_WIDE     = 12;   // ...widened when nothing fits the first band
static constexpr int16_t VAC_SAVE_SCORE    = 40;   // to whoever puts the sign out (a founding is 20)
static constexpr uint8_t VAC_ITEM_REGISTER = 70;   // data/items.cfg
static constexpr uint8_t VAC_ITEM_KEY      = 71;

static constexpr uint8_t VAC_PART_SIGN = 1, VAC_PART_DESK = 2, VAC_PART_KEYS = 3, VAC_PART_OUT = 4;
static constexpr uint8_t VAC_ST_SIGN = 0, VAC_ST_OPEN = 1, VAC_ST_SAVED = 2, VAC_ST_FALLEN = 3;
static constexpr uint8_t VAC_GOT_REGISTER = 1, VAC_GOT_KEY = 2;
// EVT_VAC's amt: what happened (wire "w", network-events.hpp VAC_WHAT).
static constexpr uint8_t VAC_EV_HOOK = 1, VAC_EV_DESK = 2, VAC_EV_KEYS = 3, VAC_EV_ROOM = 4,
                         VAC_EV_SAVED = 5, VAC_EV_FALLEN = 6, VAC_EV_LOST = 7, VAC_EV_HUM = 8;

// Terrain ids (TERRAIN order in data/game-data.js).
static constexpr uint8_t VT_SCRUB = 0, VT_DUNES = 1, VT_URBAN = 4, VT_FLOODED = 5,
                         VT_BUNKER = 12, VT_VENT = 13;

struct VacancySign {
  int16_t townQ, townR;    // the Settlement under the sign; -1 = no Settlement on this board
  int16_t deskQ, deskR;    // The Front Desk (part 2); -1 until the town is read
  int16_t keysQ, keysR;    // Housekeeping (part 3)
  uint8_t stage;           // VAC_ST_*
  uint8_t rooms;           // vacancies left; 0 = NO VACANCY
  uint8_t got;             // VAC_GOT_*: banked out of the desk / the corridor
  // Runtime only -- never saved
  uint8_t hummed[MAX_PLAYERS];   // told what the sign wants since the last dawn
  bool    packChanged[MAX_PLAYERS];   // Check-Out just spent the two: owe an item_result
  bool    saveWanted;
};
static VacancySign VAC;

static inline bool vacLive() { return VAC.townQ >= 0 && VAC.stage <= VAC_ST_OPEN; }

// ── Placement ───────────────────────────────────────────────────────────────
// Any Settlement on the board, reservoir-sampled so every town is as likely.
static void vacPickTown() {
  uint32_t n = 0;
  VAC.townQ = VAC.townR = -1;
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      if (G.map[r][c].terrain == TERRAIN_SETTLEMENT && esp_random() % (++n) == 0) {
        VAC.townQ = (int16_t)c; VAC.townR = (int16_t)r;
      }
  // %d, not %lu: ArduinoLog reads one character after the % (it printed "26u").
  if (n) Log.notice("VAC sign over (%d,%d), one of %d towns", (int)VAC.townQ, (int)VAC.townR, (int)n);
  else   Log.warning("VAC: no Settlement on this board -- no sign");
}

// Somewhere a scene can stand: walkable, not a town, not a hatch (stepping
// onto one drops you below after the arrival hooks), not the other site,
// and not an armed booby trap: trapOnArrival() runs first and would open
// the trap instead of the scene on every visit until somebody sprang it.
static bool vacSiteOk(int c, int r) {
  const uint8_t t = G.map[r][c].terrain;
  if (TERRAIN_MC[t] == 255 || t == TERRAIN_SETTLEMENT || t == VT_BUNKER || t == VT_VENT) return false;
  if (G.map[r][c].trap & TRAP_ARMED) return false;
  if (c == VAC.deskQ && r == VAC.deskR) return false;
  return true;
}

// A hex `want` (a terrain bitmask) in the band round the town. Widens the
// band, then drops the terrain, before giving up on anywhere passable.
static void vacPlaceSite(int16_t& outQ, int16_t& outR, uint16_t want, const char* what) {
  uint32_t n = 0;
  for (int pass = 0; pass < 3 && !n; pass++) {
    const int lo = pass ? 3 : VAC_SITE_MIN, hi = pass ? VAC_SITE_WIDE : VAC_SITE_MAX;
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        if (!vacSiteOk(c, r)) continue;
        if (pass < 2 && !(want & (1u << G.map[r][c].terrain))) continue;
        const int d = hexDistWrap(VAC.townQ, VAC.townR, c, r);
        if (d < lo || d > hi) continue;
        if (esp_random() % (++n) == 0) { outQ = (int16_t)c; outR = (int16_t)r; }
      }
    }
    if (n) Log.notice("VAC %s at (%d,%d)%s", what, (int)outQ, (int)outR, pass ? " (wide)" : "");
  }
  if (!n) {
    pickPassableHex(outQ, outR);
    Log.warning("VAC %s: nowhere near the town -- (%d,%d)", what, (int)outQ, (int)outR);
  }
}

static void vacPlaceSites() {
  VAC.deskQ = VAC.deskR = VAC.keysQ = VAC.keysR = -1;
  vacPlaceSite(VAC.deskQ, VAC.deskR, (1u << VT_SCRUB) | (1u << VT_DUNES), "front desk");      // the open ground
  vacPlaceSite(VAC.keysQ, VAC.keysR, (1u << VT_URBAN) | (1u << VT_FLOODED), "housekeeping");  // a ruin
}

// ── Init (wInit: boot, regen, and before a save is restored) ────────────────
static void vacInit() {
  memset(&VAC, 0, sizeof(VAC));
  VAC.deskQ = VAC.deskR = VAC.keysQ = VAC.keysR = -1;
  VAC.stage = VAC_ST_SIGN;
  VAC.rooms = VAC_ROOMS;
  vacPickTown();
}

// ── Events ──────────────────────────────────────────────────────────────────
static void vacEmit(uint8_t kind, int pid) {
  GameEvent ev = {}; ev.type = EVT_VAC;
  ev.pid = (uint8_t)(pid < 0 ? 255 : pid);   // 255 = the world's doing (a dawn)
  ev.q = VAC.townQ; ev.r = VAC.townR;
  ev.amt = kind;
  ev.res = VAC.rooms;
  enqEvt(ev);
}

// The town is gone. `bySign`: the count ran out and it checked in -- its hex
// turns to Broken Urban here. Otherwise something else levelled it first and
// the terrain has already changed.
static void vacFall(bool bySign) {
  VAC.stage = VAC_ST_FALLEN;
  VAC.deskQ = VAC.deskR = VAC.keysQ = VAC.keysR = -1;
  if (bySign) {
    HexCell& c = G.map[VAC.townR][VAC.townQ];
    c.terrain = VT_URBAN;   // a dead motel; every door open, every bed made
    c.poi = 0;              // nobody left to tell it
    VAC.rooms = 0;
  }
  vacEmit(bySign ? VAC_EV_FALLEN : VAC_EV_LOST, -1);
  VAC.saveWanted = true;
  Log.notice("VAC town (%d,%d) %s", (int)VAC.townQ, (int)VAC.townR, bySign ? "fell: NO VACANCY" : "lost to something else");
}

// ── The world tick and dawn ─────────────────────────────────────────────────
// A quake (actions_game_loop.hpp) or a debris strike can level a Settlement
// to scrub or dunes. If it was this one, the sign went with it.
static void tickVacancy() {
  if (!vacLive()) return;
  if (G.map[VAC.townR][VAC.townQ].terrain != TERRAIN_SETTLEMENT) vacFall(false);
}

// The dawn block in tickGame(), after the scenes were cut short: another
// house checks in, if anyone is connected to see it.
static void vacDawn(bool watched) {
  memset(VAC.hummed, 0, sizeof(VAC.hummed));
  if (!vacLive() || VAC.stage != VAC_ST_OPEN || !watched) return;
  if (VAC.rooms) VAC.rooms--;
  if (!VAC.rooms) { vacFall(true); return; }
  vacEmit(VAC_EV_ROOM, -1);
  VAC.saveWanted = true;
  Log.notice("VAC a house checked in: %d rooms left", (int)VAC.rooms);
}

// Read-and-clear: did the quest change since the last save?
static bool vacTakeSaveWanted() {
  bool w = VAC.saveWanted;
  VAC.saveWanted = false;
  return w;
}

// Read-and-clear: Check-Out just took the register and the key out of pid's
// pack, which no tick carries -- the caller owes them an item_result.
static bool vacTakePackChanged(int pid) {
  bool w = VAC.packChanged[pid];
  VAC.packChanged[pid] = false;
  return w;
}

// ── Scenes ──────────────────────────────────────────────────────────────────
static bool vacPartBusy(uint8_t part) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (encounters[i].active && encounters[i].vac == part) return true;
  return false;
}

static bool vacOpen(int pid, uint8_t part) {
  if (vacPartBusy(part)) return false;
  const char* json = encLoadFile(ENC_POOL_VAC, part);
  if (!json) {
    Log.warning("VAC part %d: file will not load", (int)part);
    return false;
  }
  Player& p = G.players[pid];
  ActiveEncounter& enc = encounters[pid];
  enc = {};
  enc.active  = 1;
  enc.vac     = part;
  enc.encIdx  = part;
  enc.hexQ    = (uint8_t)p.q;
  enc.hexR    = (uint8_t)p.r;
  enc.depth   = 0;
  enc.terrain = ENC_POOL_VAC;
  encEnterStartNode(json, enc);
  GameEvent ev = {};
  ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
  ev.q = p.q; ev.r = p.r;
  ev.encOut = part;   // EVT_ENC_START-specific: a VACANCY part, wire "vac"
  enqEvt(ev);
  Log.notice("VAC part %d opened pid=%d (%d,%d)", (int)part, pid, (int)p.q, (int)p.r);
  return true;
}

// Called by movePlayer() after Kane's check, surface steps only. Returns true
// when a scene opened under the survivor's feet.
static bool vacOnArrival(int pid) {
  Player& p = G.players[pid];
  if (!vacLive() || p.ll == 0 || p.depth || encounters[pid].active) return false;
  const bool town = (p.q == VAC.townQ && p.r == VAC.townR);
  if (VAC.stage == VAC_ST_SIGN) return town && vacOpen(pid, VAC_PART_SIGN);
  if (!(VAC.got & VAC_GOT_REGISTER) && p.q == VAC.deskQ && p.r == VAC.deskR) return vacOpen(pid, VAC_PART_DESK);
  if (!(VAC.got & VAC_GOT_KEY)      && p.q == VAC.keysQ && p.r == VAC.keysR) return vacOpen(pid, VAC_PART_KEYS);
  if (!town) return false;
  const bool reg = playerHasForChoice(pid, VAC_ITEM_REGISTER);
  const bool key = playerHasForChoice(pid, VAC_ITEM_KEY);
  if (reg && key) return vacOpen(pid, VAC_PART_OUT);
  // Under the sign without both: it hums at you, once a day, and the client
  // says which half is missing.
  if (!VAC.hummed[pid]) {
    VAC.hummed[pid] = 1;
    GameEvent ev = {}; ev.type = EVT_VAC; ev.pid = (uint8_t)pid;
    ev.q = VAC.townQ; ev.r = VAC.townR;
    ev.amt = VAC_EV_HUM;
    ev.res = (uint8_t)((reg ? VAC_GOT_REGISTER : 0) | (key ? VAC_GOT_KEY : 0));   // what they DO carry
    ev.evWsId = p.wsClientId;
    enqEvt(ev);
  }
  return false;
}

// Take one of itemId out of the pack. True if there was one to take.
static bool vacSpendItem(Player& p, uint8_t itemId) {
  const uint8_t slots = effectiveInvSlots(p);
  for (int s = 0; s < slots; s++) {
    if (p.invType[s] != itemId || !p.invQty[s]) continue;
    if (--p.invQty[s] == 0) p.invType[s] = 0;
    return true;
  }
  return false;
}

// encRunChoice() reached a terminal, non-escape node of a VACANCY scene. The
// town (1) and Check-Out (4) count here; the desk and the corridor count on
// the bank (vacOnBank). `json` is the loaded file, `nodeKey` that last node.
static void vacPartRead(int pid, const char* json, const char* nodeKey) {
  const uint8_t part = encounters[pid].vac;
  if (part == VAC_PART_SIGN) {
    if (VAC.stage != VAC_ST_SIGN) return;
    VAC.stage = VAC_ST_OPEN;
    VAC.rooms = VAC_ROOMS;
    vacPlaceSites();
    vacEmit(VAC_EV_HOOK, pid);
    VAC.saveWanted = true;
    Log.notice("VAC town read by pid=%d: %d nights", pid, (int)VAC.rooms);
  } else if (part == VAC_PART_OUT) {
    if (VAC.stage != VAC_ST_OPEN) return;
    char e[8] = {0};
    jsonCopyStr(jsonObjGet(encNode(json, nodeKey), "vac_ending"), e, sizeof(e));
    if (strcmp(e, "saved")) {
      Log.warning("VAC Check-Out node %s has no vac_ending -- not an ending", nodeKey);
      return;
    }
    Player& p = G.players[pid];
    // The register stays on the meeting-hall table; the key is a mint.
    vacSpendItem(p, VAC_ITEM_REGISTER);
    vacSpendItem(p, VAC_ITEM_KEY);
    VAC.packChanged[pid] = true;
    p.score = (uint16_t)min((int)p.score + (int)VAC_SAVE_SCORE, 65535);
    VAC.stage = VAC_ST_SAVED;
    VAC.deskQ = VAC.deskR = VAC.keysQ = VAC.keysR = -1;
    vacEmit(VAC_EV_SAVED, pid);
    VAC.saveWanted = true;
    Log.notice("VAC town (%d,%d) saved by pid=%d with %d rooms left", (int)VAC.townQ, (int)VAC.townR, pid, (int)VAC.rooms);
  }
}

// handleMsg_enc_bank, after the haul is granted and before the slot is
// cleared: the desk or the corridor is done once its item is out in the
// world -- in the pack, or on the ground beside a full one.
static void vacOnBank(int pid) {
  const ActiveEncounter& enc = encounters[pid];
  if (VAC.stage != VAC_ST_OPEN) return;
  const uint8_t bit = enc.vac == VAC_PART_DESK ? VAC_GOT_REGISTER : enc.vac == VAC_PART_KEYS ? VAC_GOT_KEY : 0;
  if (!bit || (VAC.got & bit)) return;
  const uint8_t want = (bit == VAC_GOT_REGISTER) ? VAC_ITEM_REGISTER : VAC_ITEM_KEY;
  for (int j = 0; j < enc.pendingItemCount; j++) {
    if (enc.pendingItemType[j] != want) continue;
    VAC.got |= bit;
    vacEmit(bit == VAC_GOT_REGISTER ? VAC_EV_DESK : VAC_EV_KEYS, pid);
    VAC.saveWanted = true;
    Log.notice("VAC %s taken by pid=%d", bit == VAC_GOT_REGISTER ? "register" : "master key", pid);
    return;
  }
}

// The LCD map's marker (snapshotWorldMarkers, world-system.hpp).
// A town lost to something else (fallen with rooms still on the count) has
// no sign left to draw: it reports as dark, like a saved one.
static void vacSnapshot(int16_t& q, int16_t& r, uint8_t& stage) {
  q = VAC.townQ; r = VAC.townR;
  stage = (VAC.stage == VAC_ST_FALLEN && VAC.rooms) ? VAC_ST_SAVED : VAC.stage;
}

// ── Wire ────────────────────────────────────────────────────────────────────
// ",\"vac\":{...}" in sync, the tick broadcast's world and /state -- with its
// own leading comma, so an overflow (0) leaves the JSON whole. Everyone is
// told where the sign is; the client draws it once the hex is known, and at
// night from further off. The sites ride only while they are still open.
static int appendVacJson(char* buf, size_t cap) {
  const bool open = (VAC.stage == VAC_ST_OPEN);
  const bool desk = open && !(VAC.got & VAC_GOT_REGISTER);
  const bool keys = open && !(VAC.got & VAC_GOT_KEY);
  int n = snprintf(buf, cap,
    ",\"vac\":{\"q\":%d,\"r\":%d,\"st\":%d,\"rm\":%d,\"g\":%d,\"dq\":%d,\"dr\":%d,\"kq\":%d,\"kr\":%d}",
    (int)VAC.townQ, (int)VAC.townR, (int)VAC.stage, (int)VAC.rooms, (int)VAC.got,
    desk ? (int)VAC.deskQ : -1, desk ? (int)VAC.deskR : -1,
    keys ? (int)VAC.keysQ : -1, keys ? (int)VAC.keysR : -1);
  return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

// ── Persistence (/save/vac.bin) ─────────────────────────────────────────────
struct __attribute__((packed)) SaveVac {
  uint32_t magic;
  uint8_t  version;
  uint8_t  cols, rows;   // a file for another board shape is ignored
  int16_t  townQ, townR, deskQ, deskR, keysQ, keysR;
  uint8_t  stage, rooms, got;
};

// Inside saveGame()'s G.mutex hold. Small enough to write every time.
static void saveVacFile() {
  File f = SD.open(SAVE_VAC_F, FILE_WRITE);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_VAC_F); return; }
  SaveVac s = { VAC_SAVE_MAGIC, 1, (uint8_t)MAP_COLS, (uint8_t)MAP_ROWS,
                VAC.townQ, VAC.townR, VAC.deskQ, VAC.deskR, VAC.keysQ, VAC.keysR,
                VAC.stage, VAC.rooms, VAC.got };
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

// Called by tryLoadSave() once map.bin has loaded (wInit() already rolled a
// fresh sign on that map). No file -- a save from before VACANCY -- keeps it.
static void loadVacFile() {
  if (!SD.exists(SAVE_VAC_F)) { Log.notice("No %s: a fresh sign", SAVE_VAC_F); return; }
  File f = SD.open(SAVE_VAC_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_VAC_F); return; }
  SaveVac s;
  bool ok = f.read((uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  auto in = [](int16_t q, int16_t r) { return q >= 0 && q < MAP_COLS && r >= 0 && r < MAP_ROWS; };
  auto inOrNone = [&](int16_t q, int16_t r) { return (q == -1 && r == -1) || in(q, r); };
  if (!ok || s.magic != VAC_SAVE_MAGIC || s.version != 1 || s.cols != MAP_COLS || s.rows != MAP_ROWS ||
      !inOrNone(s.townQ, s.townR) || !inOrNone(s.deskQ, s.deskR) || !inOrNone(s.keysQ, s.keysR) ||
      s.stage > VAC_ST_FALLEN || s.rooms > VAC_ROOMS || s.got > (VAC_GOT_REGISTER | VAC_GOT_KEY)) {
    Log.warning("%s unreadable or for another board - a fresh sign", SAVE_VAC_F);
    return;
  }
  VAC.townQ = s.townQ; VAC.townR = s.townR;
  VAC.deskQ = s.deskQ; VAC.deskR = s.deskR;
  VAC.keysQ = s.keysQ; VAC.keysR = s.keysR;
  VAC.stage = s.stage; VAC.rooms = s.rooms; VAC.got = s.got;
  Log.notice("SD READ: %s town (%d,%d) stage=%d rooms=%d got=%d",
             SAVE_VAC_F, (int)VAC.townQ, (int)VAC.townR, (int)VAC.stage, (int)VAC.rooms, (int)VAC.got);
}
