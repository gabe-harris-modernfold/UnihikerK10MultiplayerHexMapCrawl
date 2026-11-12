// ── Dshingus ────────────────────────────────────────────────────────────────
// What Dshinguest Ron's band leaves behind in every town on the Movement Tour
// (data/encounters/settlement/25.json, docs/dshinguest-ron.md). A scene's
// hazard carrying "dshingus": true gives it to the survivor whose roll failed
// (dshCatch, from encRunChoice). For DSH_DAYS dawns after that:
//   - every REST has a DSH_SPOT_PCT chance of leaving a brown spot on the hex
//     they bed down on (surface only), which dries up DSH_SPOT_DAYS dawns on;
//   - sharing a hex passes it on. Whoever ends a step on an infected
//     survivor's hex catches it, and so does everybody on the hex an infected
//     survivor steps onto (dshOnArrival). Both have to be up and on the board.
// It costs nothing else. It clears when its days run out, or when the seat
// takes a new survivor. A survivor who has it cannot catch it again, so a
// running count is never restarted.
//
// Persistence is its own file, /save/dsh.bin, like ghosts.bin: no
// SAVE_VERSION bump, and a save without it loads with nobody infected.
//
// Caller holds G.mutex for everything here.

static constexpr uint8_t DSH_DAYS      = 30;    // dawns a survivor has it
static constexpr uint8_t DSH_SPOT_PCT  = 50;    // chance in 100 that a REST leaves a spot
static constexpr uint8_t DSH_SPOT_DAYS = 10;    // dawns a spot lasts before it dries up
static constexpr uint8_t DSH_SPOT_MAX  = 48;    // spots at once; one more replaces the oldest
static constexpr uint8_t DSH_SRC_BAND  = 255;   // caught in a scene, not off a survivor

// What happened, on EVT_DSH as actType (wire "w", DSH_WHAT below).
static constexpr uint8_t DSH_EV_CATCH = 0, DSH_EV_SPOT = 1, DSH_EV_CLEAR = 2;
static const char* const DSH_WHAT[3] = { "catch", "spot", "clear" };

struct DshSpot {
  bool     used;
  int16_t  q, r;
  uint16_t day;          // G.dayCount it was left on (groundAge)
};
static uint8_t dshDays[MAX_PLAYERS];   // dawns left infected; 0 = clean
static DshSpot dshSpots[DSH_SPOT_MAX];
static bool    dshSaveWanted = false;

static void dshInit() {
  memset(dshDays, 0, sizeof(dshDays));
  memset(dshSpots, 0, sizeof(dshSpots));
  dshSaveWanted = false;
}

// Read-and-clear: somebody caught it or got over it, or a spot came or went.
static bool dshTakeSaveWanted() {
  bool w = dshSaveWanted;
  dshSaveWanted = false;
  return w;
}

// src: the seat it came from, DSH_SRC_BAND for a scene. amt carries the days
// left, so a "catch" says how long.
static void dshEmit(uint8_t what, int pid, uint8_t src, int16_t q, int16_t r) {
  GameEvent ev = {};
  ev.type    = EVT_DSH;
  ev.actType = what;
  ev.pid     = (uint8_t)pid;
  ev.tradeTo = src;
  ev.q = q; ev.r = r;
  ev.amt = dshDays[pid];
  enqEvt(ev);
}

// pid has it now, from src. A no-op for somebody who already has it.
static void dshCatch(int pid, uint8_t src) {
  if (pid < 0 || pid >= MAX_PLAYERS || dshDays[pid]) return;
  dshDays[pid] = DSH_DAYS;
  dshSaveWanted = true;
  const Player& p = G.players[pid];
  Log.notice("DSH pid=%d caught it (src=%d) at (%d,%d)", pid, (int)src, (int)p.q, (int)p.r);
  dshEmit(DSH_EV_CATCH, pid, src, p.q, p.r);
}

// The seat is somebody new now (handleMsg_pick, a seat wiped): the old
// survivor took it with them.
static void dshReleaseSeat(int pid) {
  if (pid < 0 || pid >= MAX_PLAYERS || !dshDays[pid]) return;
  dshDays[pid] = 0;
  dshSaveWanted = true;
}

// Called by movePlayer() and moveTunnel() once the step is taken. Anybody up
// on the same hex (samehex: the same board, the same corridor cell below)
// who has it gives it to pid; then, if pid has it, everybody up on the hex
// who does not catches it.
static void dshOnArrival(int pid) {
  if (G.players[pid].ll == 0) return;
  for (int k = 0; k < MAX_PLAYERS && !dshDays[pid]; k++)
    if (k != pid && dshDays[k] && G.players[k].ll > 0 && samehex(pid, k)) dshCatch(pid, (uint8_t)k);
  if (!dshDays[pid]) return;
  for (int k = 0; k < MAX_PLAYERS; k++)
    if (k != pid && !dshDays[k] && G.players[k].ll > 0 && samehex(pid, k)) dshCatch(k, (uint8_t)pid);
}

// doRest(): pid just bedded down. Surface only -- the tunnel board is
// transient and has nowhere to keep a spot. One spot per hex: a new one on a
// hex that has one freshens it; a full table loses its oldest.
static void dshOnRest(int pid) {
  const Player& p = G.players[pid];
  if (!dshDays[pid] || p.depth) return;
  if (esp_random() % 100 >= DSH_SPOT_PCT) return;
  int slot = -1, freeSlot = -1, oldest = 0, oldestAge = -1;
  for (int i = 0; i < DSH_SPOT_MAX; i++) {
    const DshSpot& s = dshSpots[i];
    if (!s.used) { if (freeSlot < 0) freeSlot = i; continue; }
    if (s.q == p.q && s.r == p.r) { slot = i; break; }
    const int age = groundAge(s.day);
    if (age > oldestAge) { oldestAge = age; oldest = i; }
  }
  if (slot < 0) slot = freeSlot >= 0 ? freeSlot : oldest;
  dshSpots[slot] = { true, p.q, p.r, G.dayCount };
  dshSaveWanted = true;
  Log.notice("DSH pid=%d left a spot at (%d,%d)", pid, (int)p.q, (int)p.r);
  dshEmit(DSH_EV_SPOT, pid, (uint8_t)pid, p.q, p.r);
}

// The dawn block, after the day count moved: a day off every count, and the
// spots DSH_SPOT_DAYS old dry up. The clients age spots off gs.dc the same
// way, so nothing is sent for those.
static void dshDawn() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!dshDays[i]) continue;
    dshSaveWanted = true;
    if (--dshDays[i]) continue;
    Log.notice("DSH pid=%d is over it", i);
    dshEmit(DSH_EV_CLEAR, i, DSH_SRC_BAND, G.players[i].q, G.players[i].r);
  }
  for (int i = 0; i < DSH_SPOT_MAX; i++) {
    if (!dshSpots[i].used || groundAge(dshSpots[i].day) < DSH_SPOT_DAYS) continue;
    dshSpots[i].used = false;
    dshSaveWanted = true;
  }
}

// ── Wire ────────────────────────────────────────────────────────────────────
// ,"dsh":{"d":[days per seat],"s":[[q,r,day],...]}. "d" rides every tick;
// "s" (spots: true) only sync and /state -- a new spot comes as ev dsh
// "spot", and a client ages them off gs.dc itself. Carries its own leading
// comma.
static int appendDshJson(char* buf, size_t cap, bool spots) {
  int n = snprintf(buf, cap, ",\"dsh\":{\"d\":[");
  if (n < 0 || (size_t)n >= cap) return 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    int m = snprintf(buf + n, cap - n, "%s%d", i ? "," : "", (int)dshDays[i]);
    if (m < 0 || (size_t)(n + m) >= cap) return 0;
    n += m;
  }
  int m = snprintf(buf + n, cap - n, spots ? "],\"s\":[" : "]");
  if (m < 0 || (size_t)(n + m) >= cap) return 0;
  n += m;
  if (spots) {
    bool first = true;
    for (int i = 0; i < DSH_SPOT_MAX; i++) {
      const DshSpot& s = dshSpots[i];
      if (!s.used) continue;
      m = snprintf(buf + n, cap - n, "%s[%d,%d,%d]", first ? "" : ",", (int)s.q, (int)s.r, (int)s.day);
      if (m < 0 || (size_t)(n + m) >= cap) return 0;
      n += m;
      first = false;
    }
    m = snprintf(buf + n, cap - n, "]");
    if (m < 0 || (size_t)(n + m) >= cap) return 0;
    n += m;
  }
  m = snprintf(buf + n, cap - n, "}");
  if (m < 0 || (size_t)(n + m) >= cap) return 0;
  return n + m;
}

// ── Persistence (/save/dsh.bin) ─────────────────────────────────────────────
struct __attribute__((packed)) SaveDshSpot {
  uint8_t  used;
  int16_t  q, r;
  uint16_t day;
};
struct __attribute__((packed)) SaveDsh {
  uint32_t    magic;
  uint8_t     version;
  uint8_t     cols, rows;   // a file for another board shape is ignored
  uint8_t     days[MAX_PLAYERS];
  SaveDshSpot s[DSH_SPOT_MAX];
};

// Inside saveGame()'s G.mutex hold (staged, save-stage.hpp). ~360 bytes.
static void saveDshFile() {
  SaveOut f = saveOpen(SAVE_DSH_F);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_DSH_F); return; }
  SaveDsh s = {};
  s.magic = DSH_SAVE_MAGIC; s.version = 1;
  s.cols = (uint8_t)MAP_COLS; s.rows = (uint8_t)MAP_ROWS;
  memcpy(s.days, dshDays, sizeof(s.days));
  for (int i = 0; i < DSH_SPOT_MAX; i++) {
    const DshSpot& d = dshSpots[i];
    if (!d.used) continue;
    s.s[i] = { 1, d.q, d.r, d.day };
  }
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

// Called by tryLoadSave() once map.bin has loaded (wInit() already cleared
// it all). No file -- a save from before the band -- leaves nobody infected.
// A bad count or spot is dropped on its own, not the file.
static void loadDshFile() {
  if (!SD.exists(SAVE_DSH_F)) { Log.notice("No %s: nobody has the Dshingus", SAVE_DSH_F); return; }
  File f = SD.open(SAVE_DSH_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_DSH_F); return; }
  SaveDsh s;
  bool ok = f.read((uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  if (!ok || s.magic != DSH_SAVE_MAGIC || s.version != 1 || s.cols != MAP_COLS || s.rows != MAP_ROWS) {
    Log.warning("%s unreadable or for another board - nobody infected", SAVE_DSH_F);
    return;
  }
  int sick = 0, spots = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    dshDays[i] = s.days[i] <= DSH_DAYS ? s.days[i] : 0;
    if (dshDays[i]) sick++;
  }
  for (int i = 0; i < DSH_SPOT_MAX; i++) {
    const SaveDshSpot& o = s.s[i];
    if (!o.used) continue;
    if (o.q < 0 || o.q >= MAP_COLS || o.r < 0 || o.r >= MAP_ROWS || o.day < 1 || o.day > DAYS_PER_YEAR) continue;
    dshSpots[i] = { true, o.q, o.r, o.day };
    spots++;
  }
  Log.notice("SD READ: %s infected=%d spots=%d", SAVE_DSH_F, sick, spots);
}
