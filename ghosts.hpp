// ── Wasteland ghosts ────────────────────────────────────────────────────────
// A survivor who falls leaves a ghost on the hex where they went down (the
// EVT_DOWNED handler calls ghostRise). It drifts a hex every other world tick,
// over anything but a Settlement, and toward the living it can feel. When it
// shares a hex with a survivor it latches on and haunts them:
//   - every GHOST_DRAIN_TICKS world ticks it takes one token off the biggest
//     stack in their pack, and
//   - it takes a third of everything they gain -- a hex pickup, a FORAGE /
//     WATER / SCAVENGE yield, a quest haul as it is banked (ghostTithe).
// What it takes, it keeps (hoard). Walking into a Settlement shakes it off --
// it cannot follow -- and it drifts away, shunning the living for a minute.
// After GHOST_DAYS dawns it fades, and its hoard drops where it is as a
// Remains record anyone can loot.
//
// At most GHOST_MAX walk at once; another death fades the oldest. A ghost
// rides a haunted survivor everywhere, the tunnels included (its q/r follow
// their surface hex, pinned to the hatch while they are below). Persistence
// is its own file, /save/ghosts.bin, so ghosts needed no SAVE_VERSION bump: a
// save without it has none.
//
// Caller holds G.mutex for everything here.

static constexpr uint8_t GHOST_MAX         = 6;
static constexpr uint8_t GHOST_DAYS        = 7;   // dawns a ghost walks before it fades
static constexpr uint8_t GHOST_RISE_TICKS  = 2;   // world ticks rising off the body (~30 s): it latches onto nobody yet
static constexpr uint8_t GHOST_MOVE_EVERY  = 2;   // world ticks per hex drifted (~30 s)
static constexpr uint8_t GHOST_SENSE       = 4;   // hexes it can feel the living from
static constexpr uint8_t GHOST_DRAIN_TICKS = 8;   // world ticks between tokens drained (~2 min)
static constexpr uint8_t GHOST_TITHE_DIV   = 3;   // it takes one token in this many of every gain
static constexpr uint8_t GHOST_SHUN_TICKS  = 4;   // world ticks a ghost shaken off at a Settlement latches onto nobody (~1 min)
static constexpr uint8_t GHOST_NONE        = 255; // haunt: drifting, nobody's

// What happened, on EVT_GHOST as actType (wire "w", GHOST_WHAT below).
static constexpr uint8_t GH_LATCH = 1, GH_LETGO = 2, GH_DRAIN = 3, GH_TITHE = 4, GH_FADE = 5;
static const char* const GHOST_WHAT[6] = { "rise", "latch", "letgo", "drain", "tithe", "fade" };

struct Ghost {
  bool    used;
  uint8_t src;            // the seat that fell (seat == archetype: it colours the client's ghost)
  char    name[12];       // who it was (JSON-safe, like Remains)
  int16_t q, r;
  uint8_t haunt;          // the seat it rides, GHOST_NONE drifting
  uint8_t daysLeft;       // dawns until it fades, 1..GHOST_DAYS
  uint8_t hoard[5];       // water/food/fuel/med/scrap it has taken
  uint8_t tithe;          // gains counted toward its next whole token, 0..GHOST_TITHE_DIV-1
  // Runtime only -- never saved
  uint8_t rise;           // world ticks still rising
  uint8_t shun;           // world ticks it latches onto nobody
  uint8_t drain;          // world ticks since it last drained
  uint8_t moveSkip;       // drifts on every GHOST_MOVE_EVERY-th tick
};
static Ghost ghosts[GHOST_MAX];
static bool  ghostSaveWanted = false;

static void ghostInit() {
  memset(ghosts, 0, sizeof(ghosts));
  ghostSaveWanted = false;
}

// Read-and-clear: a ghost rose, latched, let go or faded since the last save.
static bool ghostTakeSaveWanted() {
  bool w = ghostSaveWanted;
  ghostSaveWanted = false;
  return w;
}

static void ghostEmit(int gi, uint8_t what, int pid, uint8_t res = 0, uint8_t amt = 0) {
  const Ghost& g = ghosts[gi];
  GameEvent ev = {};
  ev.type    = EVT_GHOST;
  ev.actType = what;
  ev.actOut  = (uint8_t)gi;
  ev.tradeTo = g.src;
  ev.pid     = (uint8_t)(pid < 0 ? GHOST_NONE : pid);
  ev.q = g.q; ev.r = g.r;
  ev.res = res; ev.amt = amt;
  enqEvt(ev);
}

static int ghostHaunting(int pid) {
  for (int i = 0; i < GHOST_MAX; i++)
    if (ghosts[i].used && ghosts[i].haunt == pid) return i;
  return -1;
}

// Somebody a drifting ghost can take hold of: up, on the surface, not inside
// a scene, not in a Settlement, and not already ridden.
static bool ghostCanHaunt(int pid) {
  const Player& p = G.players[pid];
  if (!p.connected || p.ll == 0 || p.depth || encounters[pid].active) return false;
  if (G.map[p.r][p.q].terrain == TERRAIN_SETTLEMENT) return false;
  if (inOasis(p)) return false;   // nothing takes hold under the canopy (docs/forest-oasis.md)
  return ghostHaunting(pid) < 0;
}

static inline bool ghostCanDrift(uint8_t t) { return t != TERRAIN_SETTLEMENT; }

// One hex toward (tq, tr), greedy with reservoir-sampled sidesteps like
// kaneStep(). Water, rock and glass are nothing to it; a Settlement is.
static void ghostStepToward(Ghost& g, int16_t tq, int16_t tr) {
  int best = hexDistWrap(g.q, g.r, tq, tr);
  int bestD = -1, sideD = -1;
  uint32_t sideN = 0;
  for (int d = 0; d < 6; d++) {
    int nq = wrapQ(g.q + DQ[d]), nr = wrapR(g.r + DR[d]);
    if (!ghostCanDrift(G.map[nr][nq].terrain)) continue;
    int dist = hexDistWrap(nq, nr, tq, tr);
    if (dist < best) { best = dist; bestD = d; }
    else if (dist == best && bestD < 0 && (esp_random() % (++sideN)) == 0) sideD = d;
  }
  if (bestD < 0) bestD = sideD;
  if (bestD < 0) return;
  g.q = (int16_t)wrapQ(g.q + DQ[bestD]);
  g.r = (int16_t)wrapR(g.r + DR[bestD]);
}

static void ghostWander(Ghost& g) {
  int d  = (int)(esp_random() % 6);
  int nq = wrapQ(g.q + DQ[d]), nr = wrapR(g.r + DR[d]);
  if (!ghostCanDrift(G.map[nr][nq].terrain)) return;   // a town in the way: hang there this tick
  g.q = (int16_t)nq; g.r = (int16_t)nr;
}

// A hex beside (q, r) that is not a Settlement, for a ghost a town has just
// shut out. Rings 1 and 2; failing that it stays where it is.
static void ghostOutsideTown(int16_t& q, int16_t& r) {
  for (int ring = 1; ring <= 2; ring++) {
    uint32_t n = 0;
    int16_t bq = q, br = r;
    for (int dr = -ring; dr <= ring; dr++) {
      for (int dq = -ring; dq <= ring; dq++) {
        int nq = wrapQ(q + dq), nr = wrapR(r + dr);
        if (hexDistWrap(q, r, nq, nr) != ring) continue;
        if (!ghostCanDrift(G.map[nr][nq].terrain)) continue;
        if (esp_random() % (++n) == 0) { bq = (int16_t)nq; br = (int16_t)nr; }
      }
    }
    if (n) { q = bq; r = br; return; }
  }
}

// Where a fading ghost's hoard can lie: its own hex if a survivor can stand
// there, else the nearest one that can -- it may be hanging over the river.
static void ghostGroundHex(int16_t& q, int16_t& r) {
  if (TERRAIN_MC[G.map[r][q].terrain] != 255) return;
  for (int ring = 1; ring <= 3; ring++) {
    for (int dr = -ring; dr <= ring; dr++) {
      for (int dq = -ring; dq <= ring; dq++) {
        int nq = wrapQ(q + dq), nr = wrapR(r + dr);
        if (hexDistWrap(q, r, nq, nr) != ring) continue;
        if (TERRAIN_MC[G.map[nr][nq].terrain] == 255) continue;
        q = (int16_t)nq; r = (int16_t)nr;
        return;
      }
    }
  }
}

static void ghostLatch(int gi, int pid) {
  Ghost& g = ghosts[gi];
  g.haunt = (uint8_t)pid;
  g.drain = 0;
  g.tithe = 0;
  ghostSaveWanted = true;
  Log.notice("GHOST %d (%s) latches onto pid=%d at (%d,%d)", gi, g.name, pid, (int)g.q, (int)g.r);
  ghostEmit(gi, GH_LATCH, pid);
}

// Lets go at (q, r). told: say so (a Settlement shook it off) -- not when the
// one it rode went down, or their seat went to somebody new.
static void ghostLetGo(int gi, int16_t q, int16_t r, bool told) {
  Ghost& g = ghosts[gi];
  const int pid = g.haunt;
  g.haunt = GHOST_NONE;
  g.q = q; g.r = r;
  g.shun = GHOST_SHUN_TICKS;
  g.moveSkip = 0;
  ghostSaveWanted = true;
  Log.notice("GHOST %d (%s) lets go of pid=%d at (%d,%d)%s", gi, g.name, pid, (int)q, (int)r, told ? "" : " (silent)");
  if (told) ghostEmit(gi, GH_LETGO, pid);
}

// The seat is somebody new now (handleMsg_pick): whatever rode the old
// survivor has nobody to ride.
static void ghostReleaseSeat(int pid) {
  int gi = ghostHaunting(pid);
  if (gi >= 0) ghostLetGo(gi, ghosts[gi].q, ghosts[gi].r, /*told=*/false);
}

// Fades, leaving its hoard as Remains. True when that changed the ground.
static bool ghostFade(int gi) {
  Ghost& g = ghosts[gi];
  int tokens = 0;
  for (int k = 0; k < 5; k++) tokens += g.hoard[k];
  bool dropped = false;
  if (tokens) {
    int16_t dq = g.q, dr = g.r;
    ghostGroundHex(dq, dr);
    g.q = dq; g.r = dr;   // the fade event says where the hoard went
    Remains* rm = remainsFor(dq, dr);
    if (!rm->name[0]) {
      rm->pid = g.src;
      memcpy(rm->name, g.name, sizeof(rm->name));
      rm->name[sizeof(rm->name) - 1] = '\0';
    }
    rm->day = G.dayCount;
    for (int k = 0; k < 5; k++) rm->res[k] = (uint8_t)min(99, (int)rm->res[k] + (int)g.hoard[k]);
    dropped = true;
  }
  Log.notice("GHOST %d (%s) fades at (%d,%d), leaving %d tokens", gi, g.name, (int)g.q, (int)g.r, tokens);
  ghostEmit(gi, GH_FADE, g.haunt == GHOST_NONE ? -1 : g.haunt, 0, (uint8_t)min(255, tokens));
  memset(&g, 0, sizeof(g));
  ghostSaveWanted = true;
  return dropped;
}

// A survivor just went down at their (surface) hex: the ghost rises off the
// body. Anything riding them lets go first, silently -- the death is the news.
// A full table fades its oldest. Returns the new ghost's slot; *groundChanged
// is set when a fade put a hoard on the ground. Called from the EVT_DOWNED
// handler, which says it rose: the name is right there.
static int ghostRise(int pid, bool* groundChanged) {
  *groundChanged = false;
  Player& p = G.players[pid];
  int rode = ghostHaunting(pid);
  if (rode >= 0) ghostLetGo(rode, p.q, p.r, /*told=*/false);

  int slot = -1;
  for (int i = 0; i < GHOST_MAX && slot < 0; i++) if (!ghosts[i].used) slot = i;
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < GHOST_MAX; i++) if (ghosts[i].daysLeft < ghosts[slot].daysLeft) slot = i;
    *groundChanged = ghostFade(slot);
  }
  Ghost& g = ghosts[slot];
  memset(&g, 0, sizeof(g));
  g.used = true;
  g.src  = (uint8_t)pid;
  size_t n = 0;
  for (; n + 1 < sizeof(g.name) && p.name[n]; n++) {
    char c = p.name[n];
    g.name[n] = (c < 0x20 || c > 0x7E || c == '"' || c == '\\') ? '_' : c;
  }
  g.name[n] = '\0';
  if (!n) snprintf(g.name, sizeof(g.name), "Walker %d", pid + 1);
  g.q = p.q; g.r = p.r;
  g.haunt    = GHOST_NONE;
  g.daysLeft = GHOST_DAYS;
  g.rise     = GHOST_RISE_TICKS;
  ghostSaveWanted = true;
  Log.notice("GHOST %d rises: %s at (%d,%d)", slot, g.name, (int)g.q, (int)g.r);
  return slot;
}

// One token off the biggest stack, into the hoard.
static void ghostDrain(int gi) {
  Ghost& g = ghosts[gi];
  Player& p = G.players[g.haunt];
  int best = -1;
  for (int k = 0; k < 5; k++)
    if (p.inv[k] > 0 && (best < 0 || p.inv[k] > p.inv[best])) best = k;
  if (best < 0) return;
  p.inv[best]--;
  g.hoard[best] = (uint8_t)min(99, (int)g.hoard[best] + 1);
  ghostEmit(gi, GH_DRAIN, g.haunt, (uint8_t)(best + 1), 1);
}

// A haunted survivor gains `gain` of resource idx (0-4). Returns what they
// keep; the ghost's share goes in its hoard. The share is exactly a third
// over time: a gain too small to split is counted toward the next one, so
// three single tokens cost one, rather than each costing nothing (or one).
static uint8_t ghostTithe(int pid, uint8_t idx, uint8_t gain) {
  if (!gain || idx > 4) return gain;
  int gi = ghostHaunting(pid);
  if (gi < 0) return gain;
  Ghost& g = ghosts[gi];
  int owed = (int)g.tithe + gain;
  uint8_t cut = (uint8_t)(owed / GHOST_TITHE_DIV);
  g.tithe = (uint8_t)(owed % GHOST_TITHE_DIV);
  if (!cut) return gain;
  g.hoard[idx] = (uint8_t)min(99, (int)g.hoard[idx] + cut);
  ghostEmit(gi, GH_TITHE, pid, (uint8_t)(idx + 1), cut);
  return (uint8_t)(gain - cut);
}

// Called by movePlayer() after the arrival hooks, surface steps only, before
// any hatch drops them below. (fromQ, fromR) is the hex they left.
static void ghostOnArrival(int pid, int fromQ, int fromR) {
  Player& p = G.players[pid];
  int gi = ghostHaunting(pid);
  if (gi >= 0) {
    // Into town: it stays outside, on the hex they came from.
    if (G.map[p.r][p.q].terrain == TERRAIN_SETTLEMENT) ghostLetGo(gi, (int16_t)fromQ, (int16_t)fromR, /*told=*/true);
    else { ghosts[gi].q = p.q; ghosts[gi].r = p.r; }
    return;
  }
  if (!ghostCanHaunt(pid)) return;
  for (int i = 0; i < GHOST_MAX; i++) {
    Ghost& g = ghosts[i];
    if (!g.used || g.haunt != GHOST_NONE || g.rise || g.shun) continue;
    if (g.q == p.q && g.r == p.r) { ghostLatch(i, pid); return; }
  }
}

// The world tick: ride or drift, then take hold, then feed.
static void tickGhosts() {
  for (int i = 0; i < GHOST_MAX; i++) {
    Ghost& g = ghosts[i];
    if (!g.used) continue;
    if (g.rise) g.rise--;
    if (g.shun) g.shun--;

    if (g.haunt != GHOST_NONE) {
      Player& p = G.players[g.haunt];
      // Down already, or a save restored a ride on a seat that went down.
      if (p.ll == 0) { ghostLetGo(i, g.q, g.r, /*told=*/false); continue; }
      g.q = p.q; g.r = p.r;   // pinned to the hatch while they are below
      if (!p.connected) continue;   // gone from the board: it waits with them
      if (!p.depth && G.map[p.r][p.q].terrain == TERRAIN_SETTLEMENT) {
        // A town under them that no step brought them into (they founded it).
        int16_t oq = p.q, orr = p.r;
        ghostOutsideTown(oq, orr);
        ghostLetGo(i, oq, orr, /*told=*/true);
        continue;
      }
      if (encounters[g.haunt].active) continue;   // inside a scene: it waits at the door
      if (inOasis(p)) continue;                   // in a grove: it rides, but cannot feed
      if (++g.drain >= GHOST_DRAIN_TICKS) { g.drain = 0; ghostDrain(i); }
      continue;
    }

    if (g.rise) continue;   // still standing up off the body
    if (++g.moveSkip >= GHOST_MOVE_EVERY) {
      g.moveSkip = 0;
      // The nearest living survivor it can feel, unless it is shunning them.
      int tgt = -1, bestD = GHOST_SENSE + 1;
      if (!g.shun) {
        for (int k = 0; k < MAX_PLAYERS; k++) {
          if (!ghostCanHaunt(k)) continue;
          int d = hexDistWrap(g.q, g.r, G.players[k].q, G.players[k].r);
          if (d < bestD) { bestD = d; tgt = k; }
        }
      }
      if (tgt >= 0 && bestD > 0) ghostStepToward(g, G.players[tgt].q, G.players[tgt].r);
      else if (tgt < 0)          ghostWander(g);
    }
    if (g.shun) continue;
    for (int k = 0; k < MAX_PLAYERS; k++) {
      if (!ghostCanHaunt(k)) continue;
      if (G.players[k].q == g.q && G.players[k].r == g.r) { ghostLatch(i, k); break; }
    }
  }
}

// The dawn block. True when a fading ghost left its hoard on the ground.
static bool ghostDawn() {
  bool ground = false;
  for (int i = 0; i < GHOST_MAX; i++) {
    Ghost& g = ghosts[i];
    if (!g.used) continue;
    if (g.daysLeft > 0) g.daysLeft--;
    if (g.daysLeft == 0) ground |= ghostFade(i);
  }
  return ground;
}

// ── Wire ────────────────────────────────────────────────────────────────────
// ,"ghosts":[[slot,q,r,haunt,state,hoard,src,"name"],...] in sync, the tick
// broadcast and /state, live ghosts only. haunt -1 = drifting; state 0
// drifting / 1 rising / 2 shunning; hoard = tokens it carries. Everyone is
// told where they are; the client draws one only inside its own sight, or on
// the survivor it rides. Carries its own leading comma.
static int appendGhostJson(char* buf, size_t cap) {
  int n = snprintf(buf, cap, ",\"ghosts\":[");
  if (n < 0 || (size_t)n >= cap) return 0;
  bool first = true;
  for (int i = 0; i < GHOST_MAX; i++) {
    const Ghost& g = ghosts[i];
    if (!g.used) continue;
    int hoard = 0;
    for (int k = 0; k < 5; k++) hoard += g.hoard[k];
    const int st = g.rise ? 1 : g.shun ? 2 : 0;
    int m = snprintf(buf + n, cap - n, "%s[%d,%d,%d,%d,%d,%d,%d,\"%s\"]", first ? "" : ",",
                     i, (int)g.q, (int)g.r, g.haunt == GHOST_NONE ? -1 : (int)g.haunt, st, hoard,
                     (int)g.src, g.name);
    if (m < 0 || (size_t)(n + m) >= cap) return 0;
    n += m;
    first = false;
  }
  int m = snprintf(buf + n, cap - n, "]");
  if (m < 0 || (size_t)(n + m) >= cap) return 0;
  return n + m;
}

// ── Persistence (/save/ghosts.bin) ──────────────────────────────────────────
struct __attribute__((packed)) SaveGhost {
  uint8_t used, src;
  char    name[12];
  int16_t q, r;
  uint8_t haunt, daysLeft, tithe;
  uint8_t hoard[5];
};
struct __attribute__((packed)) SaveGhosts {
  uint32_t  magic;
  uint8_t   version;
  uint8_t   cols, rows;   // a file for another board shape is ignored
  SaveGhost g[GHOST_MAX];
};

// Inside saveGame()'s G.mutex hold (staged, save-stage.hpp). ~170 bytes.
static void saveGhostFile() {
  SaveOut f = saveOpen(SAVE_GHOST_F);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_GHOST_F); return; }
  SaveGhosts s = {};
  s.magic = GHOST_SAVE_MAGIC; s.version = 1;
  s.cols = (uint8_t)MAP_COLS; s.rows = (uint8_t)MAP_ROWS;
  for (int i = 0; i < GHOST_MAX; i++) {
    const Ghost& g = ghosts[i];
    if (!g.used) continue;
    SaveGhost& o = s.g[i];
    o.used = 1; o.src = g.src;
    memcpy(o.name, g.name, sizeof(o.name));
    o.q = g.q; o.r = g.r;
    o.haunt = g.haunt; o.daysLeft = g.daysLeft; o.tithe = g.tithe;
    memcpy(o.hoard, g.hoard, sizeof(o.hoard));
  }
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

// Called by tryLoadSave() once map.bin has loaded (wInit() already cleared
// the table). No file -- a save from before the ghosts -- leaves none. Each
// entry is checked on its own; a bad one is dropped, not the file. A ride on
// a seat that comes back downed is let go by the first world tick.
static void loadGhostFile() {
  if (!SD.exists(SAVE_GHOST_F)) { Log.notice("No %s: no ghosts walk", SAVE_GHOST_F); return; }
  File f = SD.open(SAVE_GHOST_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_GHOST_F); return; }
  SaveGhosts s;
  bool ok = f.read((uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  if (!ok || s.magic != GHOST_SAVE_MAGIC || s.version != 1 || s.cols != MAP_COLS || s.rows != MAP_ROWS) {
    Log.warning("%s unreadable or for another board - no ghosts", SAVE_GHOST_F);
    return;
  }
  int loaded = 0;
  for (int i = 0; i < GHOST_MAX; i++) {
    const SaveGhost& o = s.g[i];
    if (!o.used) continue;
    bool good = o.q >= 0 && o.q < MAP_COLS && o.r >= 0 && o.r < MAP_ROWS && o.src < MAX_PLAYERS &&
                (o.haunt == GHOST_NONE || o.haunt < MAX_PLAYERS) &&
                o.daysLeft >= 1 && o.daysLeft <= GHOST_DAYS && o.tithe < GHOST_TITHE_DIV;
    for (int k = 0; k < 5 && good; k++) good = o.hoard[k] <= 99;
    if (!good) { Log.warning("%s: ghost %d out of range - dropped", SAVE_GHOST_F, i); continue; }
    Ghost& g = ghosts[i];
    memset(&g, 0, sizeof(g));
    g.used = true; g.src = o.src;
    memcpy(g.name, o.name, sizeof(g.name));
    g.name[sizeof(g.name) - 1] = '\0';
    for (size_t c = 0; g.name[c]; c++)
      if (g.name[c] < 0x20 || g.name[c] > 0x7E || g.name[c] == '"' || g.name[c] == '\\') g.name[c] = '_';
    g.q = o.q; g.r = o.r;
    g.haunt = o.haunt; g.daysLeft = o.daysLeft; g.tithe = o.tithe;
    memcpy(g.hoard, o.hoard, sizeof(g.hoard));
    loaded++;
  }
  Log.notice("SD READ: %s ghosts=%d", SAVE_GHOST_F, loaded);
}
