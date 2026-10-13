// ── Machine Gun Kane (docs/machine-gun-kane.md) ─────────────────────────────
// A rusted M151 that roams the open road and shoots survivors caught in the
// open, and the five-chapter story it leaves behind it.
//
// The jeep: 1 hex a world tick on open ground (every other tick in a
// Settlement, where he slows down to ask). He never fires blind -- the first world tick he sees
// you exposed he calls out (EVT_KANE_CALL) and stops to wait for an answer;
// if you are still in the open on the next tick, he fires a burst
// (EVT_KANE_FIRE). Then he asks again. Cover is your own hex's terrain, not a
// sight line. He never fires into a Settlement ("never shoot at a door"), the
// Glass Fields ("never look at the glass"), underground, at anyone inside an
// encounter, at a downed survivor, or after dark. There is no fighting back.
//
// Nights: at dusk he stops and sits awake at a fire until two world ticks
// after dawn (he never sleeps -- part zombie). Breaking camp leaves the next
// unread chapter (1-3) on the hex he sat on. Chapter 3 reveals the house in
// the glass (chapter 4) and stamps his sixty-year ring of tracks round it.
// Chapter 5 is his fire: walk onto his hex at night once chapter 4 is read.
//
// The arc is the world's, not a survivor's: one stage, shared, and an ending
// retires the jeep for everyone until the map is regenerated. Scenes open
// server-side on arrival, like a booby trap (traps.hpp), from the "kane"
// pool in index.json (file number = chapter). A chapter counts once its
// reader reaches its last node; walking away, a hazard that ends the scene,
// dawn or a disconnect leave it where it was. Persistence is its own file,
// /save/kane.bin, so adding Kane did not need a SAVE_VERSION bump: a save
// without it gets a fresh jeep and an unread arc.
//
// Caller holds G.mutex for everything here.

// Halved 2026-10-12 at the user's call (was 2): one hex a world tick on the
// open road, and in a Settlement only every other tick (kaneDrive()).
static constexpr uint8_t KANE_STEPS        = 1;    // hexes per world tick on the open road
static constexpr uint8_t KANE_RANGE        = 3;    // hexes he can reach with a burst
static constexpr uint8_t KANE_ROUNDS       = 3;    // rounds per burst
static constexpr uint8_t KANE_HIT_PCT      = 35;   // chance each round hits
static constexpr uint8_t KANE_BURST_CAP    = 2;    // most LL one burst can take
static constexpr uint8_t KANE_BREAK_TICKS  = 2;    // world ticks after dawn before he drives on
static constexpr uint8_t KANE_STUCK_LIMIT  = 6;    // world ticks without closing on the waypoint before re-routing
static constexpr uint8_t KANE_WAYPOINT_MIN = 5;    // waypoint distance band, hexes
static constexpr uint8_t KANE_WAYPOINT_MAX = 25;
static constexpr uint32_t KANE_DUSK_TICK   = (uint32_t)DAY_TICKS * 78 / 100;   // = SS_DUSK's 0.78

static constexpr uint8_t KANE_DRIVE = 0, KANE_CAMP = 1, KANE_GONE = 2;
static constexpr uint8_t KANE_END_NONE = 0, KANE_END_TRUTH = 1, KANE_END_LIE = 2;
static constexpr uint8_t KANE_CHAPTERS = 5;

// Terrain ids (TERRAIN order in data/game-data.js).
static constexpr uint8_t KT_SCRUB = 0, KT_DUNES = 1, KT_MARSH = 3, KT_URBAN = 4,
                         KT_GLASS = 6, KT_CRATER = 10;

struct MachineGunKane {
  int16_t q, r;
  int16_t wq, wr;          // waypoint: a Settlement or Broken Urban hex, where she might be
  int16_t pq, pr;          // the waypoint just reached, kept out of the next pick
  uint8_t mode;            // KANE_DRIVE / KANE_CAMP / KANE_GONE
  int8_t  facing;          // -1 heading west, +1 east: the client mirrors the sticker
  uint8_t stuckTicks;
  uint8_t breakTicks;      // world ticks of daylight spent still at the fire
  uint8_t townSkip;        // in a Settlement he moves every other tick; runtime only
  // The arc (shared by the whole party)
  uint8_t stage;           // chapters read, 0-5
  uint8_t ending;          // KANE_END_*
  int16_t campQ, campR;    // the live camp holding chapter stage+1 (1-3); -1 = none
  int16_t siteQ, siteR;    // chapter 4's house in the glass; -1 until chapter 3 is read
  // Runtime only -- never saved
  uint8_t called[MAX_PLAYERS];    // asked this survivor last tick; still exposed now = the burst
  uint8_t visited[MAX_PLAYERS];   // already heard his question at the fire tonight
  bool    saveWanted;             // arc or camp changed: the next save should catch it
};
static MachineGunKane K;

static inline bool kaneIsNight() { return G.dayTick >= KANE_DUSK_TICK; }

// Open road: scrub, dunes, ruined streets, settlements. Not the hatches -- a
// camp or his fire on one would drop whoever walked onto it into the tunnels
// in the middle of the scene (tunnelStepDown runs after the arrival hooks).
static inline bool kaneDrivable(uint8_t t) {
  return t == KT_SCRUB || t == KT_DUNES || t == KT_URBAN || t == TERRAIN_SETTLEMENT;
}
// Ground he can hit you on. Everything else is cover, or a rule.
static inline bool kaneOpenGround(uint8_t t) {
  return t == KT_SCRUB || t == KT_DUNES || t == KT_MARSH;
}

static bool kaneExposed(const Player& p) {
  if (p.q < 0 || p.q >= MAP_COLS || p.r < 0 || p.r >= MAP_ROWS) return false;
  return hexDistWrap(K.q, K.r, p.q, p.r) <= KANE_RANGE && kaneOpenGround(G.map[p.r][p.q].terrain);
}

// ── Placement + routing ─────────────────────────────────────────────────────
static void kanePickDrivable(int16_t& q, int16_t& r) {
  for (int i = 0; i < 400; i++) {
    int16_t tq = (int16_t)(esp_random() % MAP_COLS), tr = (int16_t)(esp_random() % MAP_ROWS);
    if (kaneDrivable(G.map[tr][tq].terrain)) { q = tq; r = tr; return; }
  }
  pickPassableHex(q, r);   // a map with no open road at all: anywhere he can stand
}

// A Settlement or Broken Urban hex in the distance band, reservoir-sampled so
// the whole map gets searched; the last one reached is excluded so he does
// not bounce between two towns. Falls back to any drivable hex.
static void kanePickWaypoint() {
  uint32_t n = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t t = G.map[r][c].terrain;
      if (t != TERRAIN_SETTLEMENT && t != KT_URBAN) continue;
      if (c == K.pq && r == K.pr) continue;
      int d = hexDistWrap(K.q, K.r, c, r);
      if (d < KANE_WAYPOINT_MIN || d > KANE_WAYPOINT_MAX) continue;
      if (esp_random() % (++n) == 0) { K.wq = (int16_t)c; K.wr = (int16_t)r; }
    }
  }
  if (!n) kanePickDrivable(K.wq, K.wr);
  K.stuckTicks = 0;
}

// One step toward the waypoint over drivable ground only. Equal-distance
// sidesteps (reservoir-sampled) let him slide along a river or a forest edge,
// the same trick moveOneStep() uses; tickKane() watches for no progress.
static bool kaneStep() {
  int best = hexDistWrap(K.q, K.r, K.wq, K.wr);
  int bestD = -1, sideD = -1;
  uint32_t sideN = 0;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(K.q + DQ[d]), nr = wrapR(K.r + DR[d]);
    if (!kaneDrivable(G.map[nr][nq].terrain)) continue;
    int dist = hexDistWrap(nq, nr, K.wq, K.wr);
    if (dist < best) { best = dist; bestD = d; }
    else if (dist == best && bestD < 0 && (esp_random() % (++sideN)) == 0) sideD = d;
  }
  if (bestD < 0) bestD = sideD;
  if (bestD < 0) return false;
  const int fromQ = K.q, fromR = K.r;
  K.q = (int16_t)wrapQ(K.q + DQ[bestD]);
  K.r = (int16_t)wrapR(K.r + DR[bestD]);
  if (DQ[bestD] > 0) K.facing = 1; else if (DQ[bestD] < 0) K.facing = -1;
  // His ruts share the caravan's bit and trail slot: one set of tyre tracks.
  G.map[K.r][K.q].tireTrack = 1;
  int side = trailSideTo(fromQ, fromR, K.q, K.r);
  if (side >= 0) trailStep(TRAIL_CARAVAN, fromQ, fromR, side, true);
  else           trailStart(TRAIL_CARAVAN, K.q, K.r);
  return true;
}

static void kaneDrive() {
  // In town he slows down to ask: half his road speed, a step every other tick.
  // A skipped tick is not a stuck one, so it returns before the progress check.
  if (G.map[K.r][K.q].terrain == TERRAIN_SETTLEMENT) {
    K.townSkip ^= 1;
    if (K.townSkip) return;
  } else {
    K.townSkip = 0;
  }
  int before = hexDistWrap(K.q, K.r, K.wq, K.wr);
  for (int s = 0; s < KANE_STEPS; s++) {
    if (K.q == K.wq && K.r == K.wr) {
      K.pq = K.wq; K.pr = K.wr;
      kanePickWaypoint();
      before = hexDistWrap(K.q, K.r, K.wq, K.wr);
    }
    if (!kaneStep()) break;
    if (G.map[K.r][K.q].terrain == TERRAIN_SETTLEMENT) break;   // into town: slow down and ask
  }
  int after = hexDistWrap(K.q, K.r, K.wq, K.wr);
  if (after < before) K.stuckTicks = 0;
  else if (++K.stuckTicks >= KANE_STUCK_LIMIT) kanePickWaypoint();
}

// ── Chapter 4's site: the house in the glass, and his ring round it ─────────
// A Glass Fields hex with open road beside it (the trench needs somewhere to
// run). Without any glass, a crater rim stands in -- the text still says
// glass, which is docs/machine-gun-kane.md's open question.
static void kaneStampRing(int16_t sq, int16_t sr) {
  const uint8_t kind = G.map[sr][sq].terrain;
  static constexpr int RING_CAP = 160;
  int16_t qq[RING_CAP], rr[RING_CAP];
  int n = 0, head = 0;
  qq[n] = sq; rr[n] = sr; n++;
  while (head < n) {
    int q = qq[head], r = rr[head]; head++;
    for (int d = 0; d < 6; d++) {
      int nq = wrapQ(q + DQ[d]), nr = wrapR(r + DR[d]);
      uint8_t t = G.map[nr][nq].terrain;
      if (t == kind) {
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) seen = (qq[k] == nq && rr[k] == nr);
        if (!seen && n < RING_CAP) { qq[n] = (int16_t)nq; rr[n] = (int16_t)nr; n++; }
      } else if (kaneDrivable(t)) {
        G.map[nr][nq].tireTrack = 1;   // sixty years of laps
      }
    }
  }
}

static void kanePlaceSite() {
  uint32_t n = 0;
  for (int pass = 0; pass < 2 && !n; pass++) {
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        uint8_t t = G.map[r][c].terrain;
        bool cand = false;
        if (pass == 0) {
          if (t != KT_GLASS) continue;
          for (int d = 0; d < 6 && !cand; d++)
            cand = kaneDrivable(G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain);
        } else {
          // No glass on this map: a walkable hex on a crater's rim.
          if (TERRAIN_MC[t] == 255 || kaneDrivable(t)) continue;
          for (int d = 0; d < 6 && !cand; d++)
            cand = G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == KT_CRATER;
        }
        if (cand && esp_random() % (++n) == 0) { K.siteQ = (int16_t)c; K.siteR = (int16_t)r; }
      }
    }
    if (n) Log.notice("KANE site (%d,%d) %s", (int)K.siteQ, (int)K.siteR, pass ? "crater rim (no glass)" : "glass");
  }
  if (!n) {
    pickPassableHex(K.siteQ, K.siteR);
    Log.warning("KANE site: no glass and no crater rim -- (%d,%d)", (int)K.siteQ, (int)K.siteR);
  }
  kaneStampRing(K.siteQ, K.siteR);
}

// The lie ending: he drove to the line. The jeep is left empty on the road
// at the edge of the glass, next to her house.
static void kaneParkAtSite() {
  if (K.siteQ < 0) return;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(K.siteQ + DQ[d]), nr = wrapR(K.siteR + DR[d]);
    if (kaneDrivable(G.map[nr][nq].terrain)) { K.q = (int16_t)nq; K.r = (int16_t)nr; return; }
  }
}

// ── Init (wInit: boot, regen, and before a save is restored) ────────────────
static void kaneInit() {
  memset(&K, 0, sizeof(K));
  kanePickDrivable(K.q, K.r);
  K.pq = K.pr = -1;
  K.campQ = K.campR = -1;
  K.siteQ = K.siteR = -1;
  K.facing = -1;
  K.mode = KANE_DRIVE;
  kanePickWaypoint();
}

// ── The world tick ──────────────────────────────────────────────────────────
static void kaneEmitCall(int pid, uint8_t camp) {
  GameEvent ev = {}; ev.type = EVT_KANE_CALL; ev.pid = (uint8_t)pid;
  ev.q = K.q; ev.r = K.r;
  ev.amt = camp;                            // 0 = on the road, 1 = at his fire
  ev.res = (uint8_t)(esp_random() & 0xFF);  // line index; the client reduces it mod KANE_LINES
  ev.evWsId = G.players[pid].wsClientId;
  enqEvt(ev);
}

// Calls and bursts for this tick. True while someone has just been asked:
// he stops the jeep and waits for the answer.
static bool kaneFire() {
  bool asked = false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player& p = G.players[i];
    if (!p.connected || p.ll == 0 || p.depth || encounters[i].active || !kaneExposed(p)) {
      K.called[i] = 0;
      continue;
    }
    if (!K.called[i]) {
      K.called[i] = 1;
      asked = true;
      kaneEmitCall(i, 0);
      continue;
    }
    // Still in the open a whole tick after the question: the burst. Then the
    // slate is wiped -- rule one is ALWAYS ASK FIRST, so the next burst needs
    // a new question.
    K.called[i] = 0;
    int hits = 0;
    for (int k = 0; k < KANE_ROUNDS; k++) if ((esp_random() % 100) < KANE_HIT_PCT) hits++;
    int lost = min(min(hits, (int)KANE_BURST_CAP), (int)p.ll);
    p.ll = (uint8_t)(p.ll - lost);
    if (lost) ecoNoteHurt(i, DC_KANE);
    GameEvent ev = {}; ev.type = EVT_KANE_FIRE; ev.pid = (uint8_t)i;
    ev.q = K.q; ev.r = K.r;                 // where the burst came from: the client draws the tracers
    ev.amt = (uint8_t)hits;
    ev.res = (uint8_t)lost;
    ev.actNewLL = p.ll;
    enqEvt(ev);
    if (lost) ledFlash(255, 90, 0);
    if (p.ll == 0) {
      p.apLeft = 0;
      GameEvent dev = {}; dev.type = EVT_DOWNED; dev.pid = (uint8_t)i;
      dev.res = DC_KANE; dev.evWsId = p.wsClientId;
      enqEvt(dev);
    }
    Log.notice("KANE burst pid=%d from (%d,%d) hits=%d lost=%d ll=%d", i, (int)K.q, (int)K.r, hits, lost, (int)p.ll);
  }
  return asked;
}

static void kaneBreakCamp() {
  K.mode = KANE_DRIVE;
  K.breakTicks = 0;
  memset(K.visited, 0, sizeof(K.visited));
  if (K.stage < 3 && K.campQ < 0) {
    K.campQ = K.q; K.campR = K.r;
    Log.notice("KANE camp left at (%d,%d): chapter %d", (int)K.q, (int)K.r, (int)K.stage + 1);
  }
  kanePickWaypoint();
  K.saveWanted = true;
}

static void tickKane() {
  if (K.mode == KANE_GONE) return;
  if (K.mode == KANE_CAMP) {
    if (kaneIsNight()) { K.breakTicks = 0; return; }
    if (++K.breakTicks >= KANE_BREAK_TICKS) kaneBreakCamp();
    return;
  }
  if (kaneIsNight()) {
    // Dusk: he stops where he is and lights the fire. He doesn't shoot after
    // dark, so anyone he had just asked is let go.
    K.mode = KANE_CAMP;
    K.breakTicks = 0;
    memset(K.called, 0, sizeof(K.called));
    K.saveWanted = true;
    Log.notice("KANE camps at (%d,%d)", (int)K.q, (int)K.r);
    return;
  }
  if (!kaneFire()) kaneDrive();
}

// The LCD map's marker (snapshotWorldMarkers, world-system.hpp).
static void kaneSnapshot(int16_t& q, int16_t& r, uint8_t& mode) { q = K.q; r = K.r; mode = K.mode; }

// Read-and-clear: did the arc or the camp change since the last save?
static bool kaneTakeSaveWanted() {
  bool w = K.saveWanted;
  K.saveWanted = false;
  return w;
}

// ── Scenes ──────────────────────────────────────────────────────────────────
// Someone is already inside this chapter: one reader at a time.
static bool kaneChapterBusy(uint8_t ch) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (encounters[i].active && encounters[i].kane == ch) return true;
  return false;
}

static bool kaneOpen(int pid, uint8_t ch) {
  if (kaneChapterBusy(ch)) return false;
  const char* json = encLoadFile(ENC_POOL_KANE, ch);
  if (!json) {
    Log.warning("KANE chapter %d: file will not load", (int)ch);
    return false;
  }
  Player& p = G.players[pid];
  ActiveEncounter& enc = encounters[pid];
  enc = {};
  enc.active  = 1;
  enc.kane    = ch;
  enc.encIdx  = ch;
  enc.hexQ    = (uint8_t)p.q;
  enc.hexR    = (uint8_t)p.r;
  enc.depth   = 0;
  enc.terrain = ENC_POOL_KANE;
  encEnterStartNode(json, enc);
  GameEvent ev = {};
  ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
  ev.q = p.q; ev.r = p.r;
  ev.res = ch;   // EVT_ENC_START-specific: a Kane chapter, wire "kane"
  enqEvt(ev);
  Log.notice("KANE chapter %d opened pid=%d (%d,%d)", (int)ch, pid, (int)p.q, (int)p.r);
  return true;
}

// Called by movePlayer() after the trap check, surface steps only. Returns
// true when a chapter opened under the survivor's feet.
static bool kaneOnArrival(int pid) {
  Player& p = G.players[pid];
  if (p.ll == 0 || p.depth || encounters[pid].active || K.ending != KANE_END_NONE) return false;
  if (K.stage < 3 && K.campQ >= 0 && p.q == K.campQ && p.r == K.campR)
    return kaneOpen(pid, (uint8_t)(K.stage + 1));
  if (K.stage == 3 && K.siteQ >= 0 && p.q == K.siteQ && p.r == K.siteR)
    return kaneOpen(pid, 4);
  if (K.mode == KANE_CAMP && p.q == K.q && p.r == K.r) {
    if (K.stage == 4) return kaneOpen(pid, 5);
    // Before the glass there is nothing to tell him. He asks his question,
    // looks at you a long time, and turns back to the fire.
    if (!K.visited[pid]) { K.visited[pid] = 1; kaneEmitCall(pid, 1); }
  }
  return false;
}

// encRunChoice() reached a terminal, non-escape node of a Kane scene: the
// chapter is read. `json` is the loaded file, `nodeKey` that last node.
static void kaneChapterRead(int pid, const char* json, const char* nodeKey) {
  const uint8_t ch = encounters[pid].kane;
  if (!ch || ch > KANE_CHAPTERS || ch <= K.stage) return;
  uint8_t ending = KANE_END_NONE;
  if (ch == KANE_CHAPTERS) {
    char e[8] = {0};
    jsonCopyStr(jsonObjGet(encNode(json, nodeKey), "kane_ending"), e, sizeof(e));
    ending = !strcmp(e, "truth") ? KANE_END_TRUTH : !strcmp(e, "lie") ? KANE_END_LIE : KANE_END_NONE;
    if (ending == KANE_END_NONE) {
      Log.warning("KANE chapter 5 node %s has no kane_ending -- not an ending", nodeKey);
      return;
    }
  }
  K.stage = ch;
  if (ch <= 3) { K.campQ = K.campR = -1; }
  if (ch == 3) kanePlaceSite();
  if (ending) {
    K.ending = ending;
    K.mode = KANE_GONE;
    if (ending == KANE_END_LIE) kaneParkAtSite();
  }
  GameEvent ev = {}; ev.type = EVT_KANE_ARC; ev.pid = (uint8_t)pid;
  ev.amt = K.stage; ev.res = K.ending;
  enqEvt(ev);
  K.saveWanted = true;
  Log.notice("KANE chapter %d read by pid=%d stage=%d ending=%d", (int)ch, pid, (int)K.stage, (int)K.ending);
}

// ── Wire ────────────────────────────────────────────────────────────────────
// The "kane" object in sync and the tick broadcast. Everyone gets the jeep's
// position; the client only draws it inside its own vision, same as a pawn.
// The site rides only while it is the next thing to find.
static int appendKaneJson(char* buf, size_t cap) {
  const bool site = (K.stage == 3);
  int n = snprintf(buf, cap,
    "\"kane\":{\"q\":%d,\"r\":%d,\"m\":%d,\"f\":%d,\"st\":%d,\"e\":%d,\"cq\":%d,\"cr\":%d,\"gq\":%d,\"gr\":%d}",
    (int)K.q, (int)K.r, (int)K.mode, (int)K.facing, (int)K.stage, (int)K.ending,
    (int)K.campQ, (int)K.campR, site ? (int)K.siteQ : -1, site ? (int)K.siteR : -1);
  return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

// ── Persistence (/save/kane.bin) ────────────────────────────────────────────
struct __attribute__((packed)) SaveKane {
  uint32_t magic;
  uint8_t  version;
  uint8_t  cols, rows;   // a file for another board shape is ignored
  int16_t  q, r, wq, wr, pq, pr;
  uint8_t  mode;
  int8_t   facing;
  uint8_t  stage, ending;
  int16_t  campQ, campR, siteQ, siteR;
};

// Inside saveGame()'s G.mutex hold. Small enough to write every time.
static void saveKaneFile() {
  File f = SD.open(SAVE_KANE_F, FILE_WRITE);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_KANE_F); return; }
  SaveKane s = { KANE_SAVE_MAGIC, 1, (uint8_t)MAP_COLS, (uint8_t)MAP_ROWS,
                 K.q, K.r, K.wq, K.wr, K.pq, K.pr, K.mode, K.facing, K.stage, K.ending,
                 K.campQ, K.campR, K.siteQ, K.siteR };
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

// Called by tryLoadSave() once map.bin has loaded (wInit() already rolled a
// fresh jeep). No file -- a save from before Kane -- keeps that fresh jeep
// and an unread arc. Every coordinate is bounds-checked, like the caravan's.
static void loadKaneFile() {
  if (!SD.exists(SAVE_KANE_F)) { Log.notice("No %s: a fresh jeep, the arc unread", SAVE_KANE_F); return; }
  File f = SD.open(SAVE_KANE_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_KANE_F); return; }
  SaveKane s;
  bool ok = f.read((uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  auto in = [](int16_t q, int16_t r) { return q >= 0 && q < MAP_COLS && r >= 0 && r < MAP_ROWS; };
  auto inOrNone = [&](int16_t q, int16_t r) { return (q == -1 && r == -1) || in(q, r); };
  if (!ok || s.magic != KANE_SAVE_MAGIC || s.version != 1 || s.cols != MAP_COLS || s.rows != MAP_ROWS ||
      !in(s.q, s.r) || !in(s.wq, s.wr) || !inOrNone(s.pq, s.pr) || !inOrNone(s.campQ, s.campR) ||
      !inOrNone(s.siteQ, s.siteR) || s.mode > KANE_GONE || s.stage > KANE_CHAPTERS || s.ending > KANE_END_LIE) {
    Log.warning("%s unreadable or for another board - a fresh jeep", SAVE_KANE_F);
    return;
  }
  K.q = s.q; K.r = s.r; K.wq = s.wq; K.wr = s.wr; K.pq = s.pq; K.pr = s.pr;
  K.mode = s.mode; K.facing = (s.facing >= 0) ? 1 : -1;
  K.stage = s.stage; K.ending = s.ending;
  K.campQ = s.campQ; K.campR = s.campR; K.siteQ = s.siteQ; K.siteR = s.siteR;
  Log.notice("SD READ: %s jeep (%d,%d) mode=%d stage=%d ending=%d",
             SAVE_KANE_F, (int)K.q, (int)K.r, (int)K.mode, (int)K.stage, (int)K.ending);
}
