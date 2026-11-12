// ── RUNAWAY SCIENCE (docs/runaway-science.md) ───────────────────────────────
// Doctor Orrin Gaunt's clinic: three rooms off the bunker tunnels, every one
// within LAB_SPAN cells of the other two -- the proximity quest. It is only
// ever underground (the user, 2026-10-26): the sites are G.tunnel cells, the
// scenes open from moveTunnel(), and nothing of it is on the surface map.
// Gaunt lies preserved to the collar on a table in the theatre; only his head
// moves, and it talks, over the Institute's speakers in every room.
//
// Four scenes, from the "lab" pool in index.json (file = part):
//   1 The Sharps Ward  the needle room. Pays the Needlecap (72).
//   2 The Pantry       the jar room. Pays the Brine Heart (73).
//   3 The Patient      the theatre. Pays the Spine Wires (74).
//   4 The Graft        the theatre again, opened only for a survivor carrying
//                      all three once the wires are out. Its gated choices
//                      need one each; its last node's "lab_ending": "graft"
//                      spends them and grants the Runaway Heart (75) and the
//                      Cursed Note (76) straight into the pack.
// Scenes open server-side on arrival, like VACANCY's (vacancy.hpp): one
// reader per part at a time, and no POI is taken. Parts 1-3 count when their
// item is BANKED (labOnBank); the Graft counts at its last node (labPartRead).
//
// The twisted economy lives in the scene files, not here: every obstacle has
// a body path and a points path ("cost": {"score": N}, "sure": true -- see
// encResolveChoice and encRunChoice), and the Graft's fee is points only.
//
// The proximity: the first time each day a survivor below comes within
// LAB_CALL_RANGE cells of the theatre, Gaunt calls to them (unicast). A banked part
// that later exists nowhere -- no pack, no pile -- is put back at dawn, so a
// heart dropped and left to rot cannot strand the quest.
//
// The quest is the world's: one clinic, one Graft, shared. wInit() (boot,
// regen, and before a save is restored) places a new clinic. Persistence is
// its own file, /save/lab.bin, like vac.bin -- no SAVE_VERSION bump; a save
// without it keeps the freshly placed clinic.
//
// Caller holds G.mutex for everything here.

static constexpr uint8_t LAB_SPAN       = 5;    // every room within this many tunnel cells of the other two
static constexpr uint8_t LAB_RING_MIN   = 2;    // the ward and the pantry: cells from the theatre (and each other)
static constexpr uint8_t LAB_RING_MAX   = 4;
static constexpr uint8_t LAB_CALL_RANGE = 5;    // Gaunt calls to anyone below this close to the theatre, once a day

static constexpr uint8_t LAB_ITEM_CAP   = 72;   // data/items.cfg: the Needlecap
static constexpr uint8_t LAB_ITEM_HEART = 73;   // the Brine Heart
static constexpr uint8_t LAB_ITEM_WIRES = 74;   // the Spine Wires
static constexpr uint8_t LAB_ITEM_GRAFT = 75;   // the Runaway Heart
static constexpr uint8_t LAB_ITEM_NOTE  = 76;   // the Cursed Note

static constexpr uint8_t LAB_PART_WARD = 1, LAB_PART_PANTRY = 2, LAB_PART_PATIENT = 3, LAB_PART_GRAFT = 4;
static constexpr uint8_t LAB_SITE_WARD = 0, LAB_SITE_PANTRY = 1, LAB_SITE_THEATRE = 2;   // LAB.q/r index; part = site + 1
static constexpr uint8_t LAB_ST_OPEN = 0, LAB_ST_GRAFTED = 1;
static constexpr uint8_t LAB_GOT_CAP = 1, LAB_GOT_HEART = 2, LAB_GOT_WIRES = 4;   // bit = 1 << site
// EVT_LAB's amt: what happened (wire "w", network-events.hpp LAB_WHAT).
static constexpr uint8_t LAB_EV_CALL = 1, LAB_EV_NEEDLE = 2, LAB_EV_JAR = 3, LAB_EV_WIRES = 4,
                         LAB_EV_GRAFT = 5, LAB_EV_LOST = 6, LAB_EV_TAUNT = 7;

// Terrain ids (TERRAIN order in data/game-data.js).
static constexpr uint8_t LT_TUNNEL_FLOOR = 14;

struct LabClinic {
  int16_t q[3], r[3];      // LAB_SITE_*: the ward, the pantry, the theatre, as G.tunnel cells; -1 = no clinic
  uint8_t stage;           // LAB_ST_*
  uint8_t got;             // LAB_GOT_*: banked out of each building
  // Runtime only -- never saved
  uint8_t called[MAX_PLAYERS];    // Gaunt has called to them since the last dawn
  uint8_t taunted[MAX_PLAYERS];   // ...and taunted them at his table
  bool    packChanged[MAX_PLAYERS];   // the Graft just changed the pack: owe an item_result
  bool    spilled;                // ...and put something on the ground beside a full pack
  int16_t spillQ, spillR;
  bool    saveWanted;
};
static LabClinic LAB;

static inline bool labLive() { return LAB.q[LAB_SITE_THEATRE] >= 0 && LAB.stage == LAB_ST_OPEN; }

// ── Placement ───────────────────────────────────────────────────────────────
// A tunnel cell a room can be: plain floor (not a shaft, not rock), no POI
// (the lighting panels sit in rooms), no armed booby trap (trapOnArrival()
// runs first and would open the trap instead). roomOnly: a dead-end room
// (TOP_ROOM), the painted ones off the corridors.
static bool labSiteOk(int c, int r, bool roomOnly) {
  const HexCell& h = G.tunnel[r][c];
  if (h.terrain != LT_TUNNEL_FLOOR || h.poi || (h.trap & TRAP_ARMED)) return false;
  return !roomOnly || (G.tunnelOp[r][c] & TOP_ROOM);
}

// The ward and the pantry round a theatre at (tq, tr): each LAB_RING_MIN..MAX
// cells from it, and LAB_RING_MIN..LAB_SPAN from each other, so all three are
// within LAB_SPAN of one another. False if there is no room round it.
static bool labPlaceAround(int16_t tq, int16_t tr, bool roomOnly) {
  int16_t pq[2] = { -1, -1 }, pr[2] = { -1, -1 };
  for (int s = 0; s < 2; s++) {
    uint32_t n = 0;
    for (int r = 0; r < TUN_ROWS; r++) {
      for (int c = 0; c < TUN_COLS; c++) {
        if (!labSiteOk(c, r, roomOnly)) continue;
        const int d = tunDist(tq, tr, c, r);
        if (d < LAB_RING_MIN || d > LAB_RING_MAX) continue;
        if (s == 1) {
          const int e = tunDist(pq[0], pr[0], c, r);
          if (e < LAB_RING_MIN || e > LAB_SPAN) continue;
        }
        if (esp_random() % (++n) == 0) { pq[s] = (int16_t)c; pr[s] = (int16_t)r; }
      }
    }
    if (!n) return false;
  }
  LAB.q[LAB_SITE_WARD] = pq[0];   LAB.r[LAB_SITE_WARD] = pr[0];
  LAB.q[LAB_SITE_PANTRY] = pq[1]; LAB.r[LAB_SITE_PANTRY] = pr[1];
  LAB.q[LAB_SITE_THEATRE] = tq;   LAB.r[LAB_SITE_THEATRE] = tr;
  return true;
}

// Every cell of the board is tried as the theatre, in a random order: three
// rooms if any trio fits, else the theatre in a room and the other two on the
// corridor, else anywhere on the floor. The board is 16 x 10, so this is
// cheap. A world whose tunnels never opened (no hatch) has no clinic.
static void labPlace() {
  static constexpr int N = TUN_COLS * TUN_ROWS;
  uint8_t order[N];
  for (int i = 0; i < N; i++) order[i] = (uint8_t)i;
  for (int i = N - 1; i > 0; i--) {
    const int j = (int)(esp_random() % (uint32_t)(i + 1));
    const uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
  }
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < N; i++) {
      const int16_t tq = (int16_t)(order[i] % TUN_COLS), tr = (int16_t)(order[i] / TUN_COLS);
      if (!labSiteOk(tq, tr, pass < 2)) continue;
      if (!labPlaceAround(tq, tr, pass == 0)) continue;
      // %d only: ArduinoLog reads one character after the %.
      Log.notice("LAB clinic below: theatre (%d,%d) ward (%d,%d) pantry (%d,%d) pass %d",
                 (int)LAB.q[2], (int)LAB.r[2], (int)LAB.q[0], (int)LAB.r[0], (int)LAB.q[1], (int)LAB.r[1], pass);
      return;
    }
  }
  for (int s = 0; s < 3; s++) LAB.q[s] = LAB.r[s] = -1;
  Log.warning("LAB: nowhere in the tunnels fits the clinic -- no Gaunt");
}

// ── Init (wInit: boot, regen, and before a save is restored) ────────────────
static void labInit() {
  memset(&LAB, 0, sizeof(LAB));
  for (int s = 0; s < 3; s++) LAB.q[s] = LAB.r[s] = -1;
  LAB.stage = LAB_ST_OPEN;
  labPlace();
}

// ── Events ──────────────────────────────────────────────────────────────────
// Broadcast unless wsId is set. pid -1 = the world's doing (a dawn).
static void labEmit(uint8_t kind, int pid, uint8_t site, uint8_t res = 0, uint32_t wsId = 0) {
  GameEvent ev = {}; ev.type = EVT_LAB;
  ev.pid = (uint8_t)(pid < 0 ? 255 : pid);
  ev.q = LAB.q[site]; ev.r = LAB.r[site];
  ev.amt = kind;
  ev.res = res;
  ev.evWsId = wsId;
  enqEvt(ev);
}

// Read-and-clear: did the quest change since the last save?
static bool labTakeSaveWanted() {
  bool w = LAB.saveWanted;
  LAB.saveWanted = false;
  return w;
}

// Read-and-clear: the Graft just took the three parts out of pid's pack and
// put the heart and the note in, which no tick carries -- the caller owes
// them an item_result.
static bool labTakePackChanged(int pid) {
  bool w = LAB.packChanged[pid];
  LAB.packChanged[pid] = false;
  return w;
}

// Read-and-clear: something the Graft granted went on the ground (q, r).
static bool labTakeSpill(int& q, int& r) {
  if (!LAB.spilled) return false;
  LAB.spilled = false;
  q = LAB.spillQ; r = LAB.spillR;
  return true;
}

// ── Lost parts ──────────────────────────────────────────────────────────────
// itemExistsInWorld() (inventory_items.hpp) says whether a banked part is
// still somewhere a survivor could get it.

// The dawn block in tickGame(), after the scenes were cut short: Gaunt may
// call and taunt again, and a banked part that is gone from the world goes
// back where it came from -- the jar refills, the cushion regrows.
static void labDawn() {
  memset(LAB.called, 0, sizeof(LAB.called));
  memset(LAB.taunted, 0, sizeof(LAB.taunted));
  if (!labLive()) return;
  for (uint8_t s = 0; s < 3; s++) {
    const uint8_t bit = (uint8_t)(1u << s);
    if (!(LAB.got & bit) || itemExistsInWorld((uint8_t)(LAB_ITEM_CAP + s))) continue;
    LAB.got &= (uint8_t)~bit;
    labEmit(LAB_EV_LOST, -1, s, (uint8_t)(s + 1));
    LAB.saveWanted = true;
    Log.notice("LAB part %d is gone from the world: its building is open again", (int)(s + 1));
  }
}

// ── Scenes ──────────────────────────────────────────────────────────────────
static bool labPartBusy(uint8_t part) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (encounters[i].active && encounters[i].lab == part) return true;
  return false;
}

static bool labOpen(int pid, uint8_t part) {
  if (labPartBusy(part)) return false;
  const char* json = encLoadFile(ENC_POOL_LAB, part);
  if (!json) {
    Log.warning("LAB part %d: file will not load", (int)part);
    return false;
  }
  Player& p = G.players[pid];
  ActiveEncounter& enc = encounters[pid];
  enc = {};
  enc.active  = 1;
  enc.lab     = part;
  enc.encIdx  = part;
  enc.hexQ    = (uint8_t)p.tq;
  enc.hexR    = (uint8_t)p.tr;
  enc.depth   = 1;                 // hexQ/hexR index G.tunnel
  enc.terrain = ENC_POOL_LAB;
  encEnterStartNode(json, enc);
  GameEvent ev = {};
  ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
  ev.q = p.tq; ev.r = p.tr; ev.depth = 1;
  ev.encLab = part;   // wire "lab"
  enqEvt(ev);
  Log.notice("LAB part %d opened pid=%d below (%d,%d)", (int)part, pid, (int)p.tq, (int)p.tr);
  return true;
}

// Called by moveTunnel() after the trap check, steps below only. Returns
// true when a scene opened under the survivor's feet.
static bool labOnArrival(int pid) {
  Player& p = G.players[pid];
  if (!labLive() || p.ll == 0 || !p.depth || encounters[pid].active) return false;
  const int16_t tq = LAB.q[LAB_SITE_THEATRE], tr = LAB.r[LAB_SITE_THEATRE];
  // The proximity: close enough, and the speakers find you.
  if (!LAB.called[pid]) {
    const int d = tunDist(p.tq, p.tr, tq, tr);
    if (d <= LAB_CALL_RANGE) {
      LAB.called[pid] = 1;
      labEmit(LAB_EV_CALL, pid, LAB_SITE_THEATRE, (uint8_t)d, p.wsClientId);
    }
  }
  for (uint8_t s = LAB_SITE_WARD; s <= LAB_SITE_PANTRY; s++)
    if (p.tq == LAB.q[s] && p.tr == LAB.r[s]) return !(LAB.got & (1u << s)) && labOpen(pid, (uint8_t)(s + 1));
  if (p.tq != tq || p.tr != tr) return false;
  if (!(LAB.got & LAB_GOT_WIRES)) return labOpen(pid, LAB_PART_PATIENT);
  const bool cap   = playerHasForChoice(pid, LAB_ITEM_CAP);
  const bool heart = playerHasForChoice(pid, LAB_ITEM_HEART);
  const bool wires = playerHasForChoice(pid, LAB_ITEM_WIRES);
  if (cap && heart && wires) return labOpen(pid, LAB_PART_GRAFT);
  // At his table without all three: he says which, once a day.
  if (!LAB.taunted[pid]) {
    LAB.taunted[pid] = 1;
    labEmit(LAB_EV_TAUNT, pid, LAB_SITE_THEATRE,
            (uint8_t)((cap ? LAB_GOT_CAP : 0) | (heart ? LAB_GOT_HEART : 0) | (wires ? LAB_GOT_WIRES : 0)), p.wsClientId);
  }
  return false;
}

// Take one of itemId out of the pack. True if there was one to take.
static bool labSpendItem(Player& p, uint8_t itemId) {
  const uint8_t slots = effectiveInvSlots(p);
  for (int s = 0; s < slots; s++) {
    if (p.invType[s] != itemId || !p.invQty[s]) continue;
    if (--p.invQty[s] == 0) p.invType[s] = 0;
    return true;
  }
  return false;
}

// Into the pack, or onto the ground beside a full one -- the clinic's own
// floor, since piles carry their board (inventory_items.hpp, groundPutHere).
static void labGrant(Player& p, uint8_t itemId) {
  if (addItemToInv(p, itemId, 1)) return;
  if (groundPutHere(p, itemId, 1)) {
    LAB.spilled = true;
    boardPos(p, &LAB.spillQ, &LAB.spillR);
  }
}

// encRunChoice() reached a terminal, non-escape node of a lab scene. Only the
// Graft counts here; the three buildings count on the bank (labOnBank).
static void labPartRead(int pid, const char* json, const char* nodeKey) {
  if (encounters[pid].lab != LAB_PART_GRAFT || LAB.stage != LAB_ST_OPEN) return;
  char e[8] = {0};
  jsonCopyStr(jsonObjGet(encNode(json, nodeKey), "lab_ending"), e, sizeof(e));
  if (strcmp(e, "graft")) {
    Log.warning("LAB Graft node %s has no lab_ending -- not an ending", nodeKey);
    return;
  }
  Player& p = G.players[pid];
  // The three go in; the heart and the note come out. Granted here, not on
  // the bank: a connection lost after the operation must not lose the heart.
  labSpendItem(p, LAB_ITEM_CAP);
  labSpendItem(p, LAB_ITEM_HEART);
  labSpendItem(p, LAB_ITEM_WIRES);
  labGrant(p, LAB_ITEM_GRAFT);
  labGrant(p, LAB_ITEM_NOTE);
  LAB.packChanged[pid] = true;
  LAB.stage = LAB_ST_GRAFTED;
  labEmit(LAB_EV_GRAFT, pid, LAB_SITE_THEATRE);
  LAB.saveWanted = true;
  Log.notice("LAB the Graft is done: pid=%d carries the Runaway Heart, and Gaunt is still", pid);
}

// handleMsg_enc_bank, after the haul is granted and before the slot is
// cleared: a building is done once its part is out in the world -- in the
// pack, or on the ground beside a full one.
static void labOnBank(int pid) {
  const ActiveEncounter& enc = encounters[pid];
  if (LAB.stage != LAB_ST_OPEN || enc.lab < LAB_PART_WARD || enc.lab > LAB_PART_PATIENT) return;
  const uint8_t site = (uint8_t)(enc.lab - 1), bit = (uint8_t)(1u << site);
  if (LAB.got & bit) return;
  const uint8_t want = (uint8_t)(LAB_ITEM_CAP + site);
  for (int j = 0; j < enc.pendingItemCount; j++) {
    if (enc.pendingItemType[j] != want) continue;
    LAB.got |= bit;
    labEmit((uint8_t)(LAB_EV_NEEDLE + site), pid, site);
    LAB.saveWanted = true;
    Log.notice("LAB part %d taken by pid=%d", (int)enc.lab, pid);
    return;
  }
}

// ── Wire ────────────────────────────────────────────────────────────────────
// ",\"lab\":{...}" in sync, the tick broadcast's world and /state -- with its
// own leading comma, so an overflow (0) leaves the JSON whole. The rooms are
// G.tunnel cells, always sent; the client draws them below, once known.
static int appendLabJson(char* buf, size_t cap) {
  int n = snprintf(buf, cap,
    ",\"lab\":{\"st\":%d,\"g\":%d,\"nq\":%d,\"nr\":%d,\"jq\":%d,\"jr\":%d,\"pq\":%d,\"pr\":%d}",
    (int)LAB.stage, (int)LAB.got,
    (int)LAB.q[LAB_SITE_WARD], (int)LAB.r[LAB_SITE_WARD],
    (int)LAB.q[LAB_SITE_PANTRY], (int)LAB.r[LAB_SITE_PANTRY],
    (int)LAB.q[LAB_SITE_THEATRE], (int)LAB.r[LAB_SITE_THEATRE]);
  return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

// ── Persistence (/save/lab.bin) ─────────────────────────────────────────────
struct __attribute__((packed)) SaveLab {
  uint32_t magic;
  uint8_t  version;
  uint8_t  cols, rows;   // the tunnel board's; a file for another shape is ignored
  int16_t  q[3], r[3];
  uint8_t  stage, got;
};

// Inside saveGame()'s G.mutex hold (staged, save-stage.hpp). Small enough to
// write every time.
static void saveLabFile() {
  SaveOut f = saveOpen(SAVE_LAB_F);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_LAB_F); return; }
  SaveLab s = {};
  s.magic = LAB_SAVE_MAGIC; s.version = 2;   // 2: tunnel cells (1 was the unshipped surface clinic)
  s.cols = (uint8_t)TUN_COLS; s.rows = (uint8_t)TUN_ROWS;
  for (int i = 0; i < 3; i++) { s.q[i] = LAB.q[i]; s.r[i] = LAB.r[i]; }
  s.stage = LAB.stage; s.got = LAB.got;
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

// Called by tryLoadSave() once map.bin -- the tunnel board with it -- has
// loaded (wInit() already placed a fresh clinic on it). No file -- a save
// from before the clinic -- keeps that one.
static void loadLabFile() {
  if (!SD.exists(SAVE_LAB_F)) { Log.notice("No %s: a fresh clinic", SAVE_LAB_F); return; }
  File f = SD.open(SAVE_LAB_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_LAB_F); return; }
  SaveLab s;
  bool ok = f.read((uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  bool sites = ok;
  for (int i = 0; i < 3 && sites; i++)
    sites = (s.q[i] == -1 && s.r[i] == -1) || tunIn(s.q[i], s.r[i]);
  if (!ok || s.magic != LAB_SAVE_MAGIC || s.version != 2 || s.cols != TUN_COLS || s.rows != TUN_ROWS || !sites ||
      s.stage > LAB_ST_GRAFTED || s.got > (LAB_GOT_CAP | LAB_GOT_HEART | LAB_GOT_WIRES)) {
    Log.warning("%s unreadable or for another board - a fresh clinic", SAVE_LAB_F);
    return;
  }
  for (int i = 0; i < 3; i++) { LAB.q[i] = s.q[i]; LAB.r[i] = s.r[i]; }
  LAB.stage = s.stage; LAB.got = s.got;
  Log.notice("SD READ: %s theatre (%d,%d) stage=%d got=%d",
             SAVE_LAB_F, (int)LAB.q[2], (int)LAB.r[2], (int)LAB.stage, (int)LAB.got);
}
