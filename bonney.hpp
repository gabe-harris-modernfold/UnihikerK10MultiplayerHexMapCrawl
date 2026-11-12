// ── ONE-HAND BONNEY (docs/one-hand-bonney.md) ───────────────────────────────
// A huge, foul, human hag who is never anywhere without her fire: a pit of
// burning tyres, a truck-axle spit over it, and Dennis Turnbull turning on the
// spit with an apple in his mouth. Her runaway left hand, Pinch, does her
// stealing. The fire IS her hex, and she stays put on it (the user,
// 2026-10-26): only a finished quest moves her, to somewhere near a random
// settlement at the next dawn.
//
// Two scenes, from the "bonney" pool in index.json (file = section):
//   1 Guess Which Hand   her fire while she wanders. A failed roll on a
//                        "steal" hazard lets Pinch lift one thing (cursed
//                        first). "bonney_ending": "deal" -- robbed if Pinch
//                        has not yet been -- or "caught" -- everything this
//                        scene took comes back. Either way she goes back to
//                        her crank, and the same fire now holds section 2.
//   2 Supper at Bonney's that fire. Turnbull has the stolen thing in his mouth
//                        (the apple: the newest hoard entry stolen from the
//                        reader, else anyone's). "rescue" hands it to the
//                        reader; "bargain" (a toenail) frees him and she eats
//                        it. Either way she is gone, and comes back at the
//                        next dawn, fire and all, usually near a settlement.
// Scenes open server-side on arrival, like Kane's chapters: one reader per
// section at a time, no POI taken. Coming within BON_CALL_RANGE of her fire,
// she calls to you, once a day (unicast).
//
// Her taste: anything cursed (ItemDef.cursed, and the Cursed Device, 37, a
// curse by name), else the most valuable thing in the pack. Never worn gear,
// never a key item that is not cursed (no other quest can be broken by her),
// one of a stack. Stealing does not go through dropItem(), so a curse cannot
// stop her -- letting her eat it is the one way out of the Cursed Note.
//
// The stage is the world's: one Bonney, shared. wInit() (boot, regen, and
// before a save is restored) places a fresh one. Persistence is its own file,
// /save/bonney.bin, like kane.bin -- no SAVE_VERSION bump.
//
// Caller holds G.mutex for everything here.

static constexpr uint8_t BON_CALL_RANGE = 3;    // she calls to a survivor this close to her fire, once a day
static constexpr uint8_t BON_TOWN_AVOID = 6;    // a comeback skips a town this close to where she was run off
static constexpr uint8_t BON_HOME_MIN   = 2;    // a comeback: this far from a settlement
static constexpr uint8_t BON_HOME_MAX   = 4;
static constexpr uint8_t BON_FIRST_MIN  = 3;    // the first placement: an Ash Dunes hex this far from a settlement
static constexpr uint8_t BON_FIRST_MAX  = 8;
static constexpr uint8_t BON_HOARD      = 6;    // what she has stolen and not given back, oldest first

static constexpr uint8_t BON_ITEM_DEVICE = 37;  // data/items.cfg: the Cursed Device, a curse by name though not flagged
static constexpr uint8_t BON_LINT        = 255; // enc.bonStole: Pinch went in and found nothing it wanted

static constexpr uint8_t BON_HAND = 0, BON_SUPPER = 1, BON_AWAY = 2;   // which section her fire holds, or gone
static constexpr uint8_t BON_SEC_HAND = 1, BON_SEC_SUPPER = 2;
// EVT_BONNEY's amt: what happened (wire "w", network-events.hpp BON_WHAT).
static constexpr uint8_t BON_EV_CALL = 1, BON_EV_PINCH = 2, BON_EV_CAUGHT = 3, BON_EV_SUPPER = 4,
                         BON_EV_RESCUE = 5, BON_EV_BARGAIN = 6, BON_EV_BACK = 7;
// Why the pack changed (the item_result's act).
static constexpr uint8_t BON_PACK_PINCH = 1, BON_PACK_CAUGHT = 2, BON_PACK_RESCUE = 3;

// Terrain ids (TERRAIN order in data/game-data.js).
static constexpr uint8_t BT_SCRUB = 0, BT_DUNES = 1, BT_MARSH = 3, BT_URBAN = 4, BT_HILLS = 7;

struct BonneyLoot { uint8_t item, pid; };   // a stolen thing and the seat it came from

struct OneHandBonney {
  int16_t  q, r;           // her and her fire, one position; -1 while she is away
  int16_t  lastQ, lastR;   // where her last pass ended: the comeback avoids that town
  int16_t  ringQ, ringR;   // the ring of ash she left; -1 = none
  uint16_t ringDay;        // G.dayCount it was left (it lasts the day)
  uint8_t  mode;           // BON_HAND / BON_SUPPER / BON_AWAY
  uint16_t n;              // passes finished: the chronicle's count
  BonneyLoot hoard[BON_HOARD];
  uint8_t  hoardN;
  // Runtime only -- never saved
  uint8_t  called[MAX_PLAYERS];   // she has called to them since the last dawn
  uint8_t  packAct[MAX_PLAYERS];  // BON_PACK_*: the pack changed, an item_result is owed
  bool     spilled;               // something she gave back went on the ground beside a full pack
  int16_t  spillQ, spillR;
  bool     saveWanted;
};
static OneHandBonney BON;

static inline bool bonWalkable(uint8_t t) {
  return t == BT_SCRUB || t == BT_DUNES || t == BT_MARSH || t == BT_URBAN || t == BT_HILLS;
}

// A hex her fire can be on: her ground, no armed booby trap (trapOnArrival()
// runs first and would open the trap instead), and nobody standing on it --
// a survivor walks into her firelight, she is never lit under one.
static bool bonHexOk(int q, int r) {
  if (q < 0 || q >= MAP_COLS || r < 0 || r >= MAP_ROWS) return false;
  const HexCell& h = G.map[r][q];
  if (!bonWalkable(h.terrain) || (h.trap & TRAP_ARMED)) return false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player& p = G.players[i];
    if (p.connected && !p.depth && p.q == q && p.r == r) return false;
  }
  return true;
}

// ── Events ──────────────────────────────────────────────────────────────────
// Broadcast unless wsId is set. pid -1 = the world's doing (a dawn).
static void bonEmit(uint8_t kind, int pid, uint8_t res = 0, uint32_t wsId = 0) {
  GameEvent ev = {}; ev.type = EVT_BONNEY;
  ev.pid = (uint8_t)(pid < 0 ? 255 : pid);
  ev.q = BON.q; ev.r = BON.r;
  ev.amt = kind;
  ev.res = res;
  ev.evWsId = wsId;
  enqEvt(ev);
}

// ── Placement ───────────────────────────────────────────────────────────────
// Ash Dunes first, then Open Scrub, then the rest of her ground.
static int bonGroundScore(uint8_t t) { return t == BT_DUNES ? 2 : t == BT_SCRUB ? 1 : 0; }

// The best-scoring hex dlo..dhi from (cq, cr) her fire can be on, reservoir-
// sampled among equals. False if there is none.
static bool bonPickAround(int cq, int cr, int dlo, int dhi, int16_t& oq, int16_t& orr) {
  int best = -1; uint32_t n = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      const int d = hexDistWrap(cq, cr, c, r);
      if (d < dlo || d > dhi || !bonHexOk(c, r)) continue;
      const int s = bonGroundScore(G.map[r][c].terrain);
      if (s > best) { best = s; n = 0; }
      if (s == best && esp_random() % (++n) == 0) { oq = (int16_t)c; orr = (int16_t)r; }
    }
  }
  return best >= 0;
}

// Any Ash Dunes hex whose nearest settlement is dlo..dhi away (dhi 0: any
// dunes at all). False if there is none.
static bool bonPickDunes(int dlo, int dhi, int16_t& oq, int16_t& orr) {
  // The towns first, once: a few dozen hexes, not the whole map per dune.
  static constexpr int TOWN_CAP = 160;
  int16_t tq[TOWN_CAP], tr[TOWN_CAP];
  int towns = 0;
  if (dhi)
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS && towns < TOWN_CAP; c++)
        if (G.map[r][c].terrain == TERRAIN_SETTLEMENT) { tq[towns] = (int16_t)c; tr[towns] = (int16_t)r; towns++; }
  uint32_t n = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      if (G.map[r][c].terrain != BT_DUNES || !bonHexOk(c, r)) continue;
      if (dhi) {
        int near = 999;
        for (int t = 0; t < towns && near >= dlo; t++) near = min(near, hexDistWrap(c, r, tq[t], tr[t]));
        if (near < dlo || near > dhi) continue;
      }
      if (esp_random() % (++n) == 0) { oq = (int16_t)c; orr = (int16_t)r; }
    }
  }
  return n > 0;
}

// The first time: an Ash Dunes hex BON_FIRST_MIN..MAX from a settlement.
// A comeback: near a random settlement -- not the town she was last run out
// of, when there is another. Fallbacks: any dunes, any of her ground.
static void bonPlace(bool first) {
  int16_t q = -1, r = -1;
  bool ok = false;
  if (first) {
    ok = bonPickDunes(BON_FIRST_MIN, BON_FIRST_MAX, q, r);
  } else {
    // A random settlement hex, away from where her last pass ended if any
    // other town is; then her ground 2-4 hexes out from it.
    int16_t tq = -1, tr = -1; uint32_t n = 0, far = 0;
    for (int pass = 0; pass < 2 && tq < 0; pass++) {
      for (int sr = 0; sr < MAP_ROWS; sr++)
        for (int sc = 0; sc < MAP_COLS; sc++) {
          if (G.map[sr][sc].terrain != TERRAIN_SETTLEMENT) continue;
          if (pass == 0 && BON.lastQ >= 0 && hexDistWrap(sc, sr, BON.lastQ, BON.lastR) <= BON_TOWN_AVOID) continue;
          if (esp_random() % (++(pass ? n : far)) == 0) { tq = (int16_t)sc; tr = (int16_t)sr; }
        }
    }
    if (tq >= 0) ok = bonPickAround(tq, tr, BON_HOME_MIN, BON_HOME_MAX, q, r);
  }
  if (!ok) ok = bonPickDunes(0, 0, q, r);
  if (!ok) {
    for (int i = 0; i < 400 && !ok; i++) {
      const int16_t tq = (int16_t)(esp_random() % MAP_COLS), tr = (int16_t)(esp_random() % MAP_ROWS);
      if (bonHexOk(tq, tr)) { q = tq; r = tr; ok = true; }
    }
  }
  if (!ok) pickPassableHex(q, r);   // a map with none of her ground: anywhere she can stand
  BON.q = q;
  BON.r = r;
  BON.mode = BON_HAND;
  Log.notice("BONNEY her fire goes down at (%d,%d) %s", (int)q, (int)r, first ? "first time" : "comeback");
}

// ── Init (wInit: boot, regen, and before a save is restored) ────────────────
static void bonneyInit() {
  memset(&BON, 0, sizeof(BON));
  BON.lastQ = BON.lastR = -1;
  BON.ringQ = BON.ringR = -1;
  bonPlace(true);
}

// ── Read-and-clear hooks ────────────────────────────────────────────────────
static bool bonneyTakeSaveWanted() {
  bool w = BON.saveWanted;
  BON.saveWanted = false;
  return w;
}

// The pack changed in a way no tick carries: the caller owes pid an
// item_result with the returned act ("" = nothing owed).
static const char* bonneyTakePackAct(int pid) {
  const uint8_t a = BON.packAct[pid];
  BON.packAct[pid] = 0;
  return a == BON_PACK_PINCH ? "bonney_pinch" : a == BON_PACK_CAUGHT ? "bonney_caught" :
         a == BON_PACK_RESCUE ? "bonney_rescue" : "";
}

static bool bonneyTakeSpill(int& q, int& r) {
  if (!BON.spilled) return false;
  BON.spilled = false;
  q = BON.spillQ; r = BON.spillR;
  return true;
}

// ── The hoard ───────────────────────────────────────────────────────────────
static void bonHoardPush(uint8_t item, uint8_t pid) {
  if (BON.hoardN == BON_HOARD) {   // the oldest falls out of the bottom of the sack, gone
    memmove(&BON.hoard[0], &BON.hoard[1], sizeof(BonneyLoot) * (BON_HOARD - 1));
    BON.hoardN--;
  }
  BON.hoard[BON.hoardN++] = { item, pid };
}

// Take the newest entry of item stolen from pid (255 = from anyone) out of
// the hoard. False if she no longer has it.
static bool bonHoardTake(uint8_t item, uint8_t pid) {
  for (int i = (int)BON.hoardN - 1; i >= 0; i--) {
    if (BON.hoard[i].item != item || (pid != 255 && BON.hoard[i].pid != pid)) continue;
    memmove(&BON.hoard[i], &BON.hoard[i + 1], sizeof(BonneyLoot) * (BON.hoardN - 1 - i));
    BON.hoardN--;
    return true;
  }
  return false;
}

// Into the pack, or onto the ground beside a full one.
static void bonGive(Player& p, uint8_t item) {
  if (addItemToInv(p, item, 1)) return;
  if (groundPut(p.q, p.r, item, 1)) { BON.spilled = true; BON.spillQ = p.q; BON.spillR = p.r; }
}

// ── The theft ───────────────────────────────────────────────────────────────
// Pinch's pick from pid's pack: anything cursed, else the most valuable
// thing, ties at random. Never a key item that is not cursed. 0 = nothing.
static uint8_t bonPickTheft(const Player& p, int& slotOut) {
  int bestRank = -1; uint32_t n = 0; uint8_t pick = 0; slotOut = -1;
  const uint8_t slots = effectiveInvSlots(p);
  for (int s = 0; s < slots; s++) {
    const uint8_t id = p.invType[s];
    if (!id || !p.invQty[s]) continue;
    const ItemDef* d = getItemDef(id);
    if (!d) continue;
    const bool curse = d->cursed || id == BON_ITEM_DEVICE;
    if (!curse && d->category == ITEM_KEY) continue;
    const int rank = curse ? 1000 : (int)d->value;
    if (rank > bestRank) { bestRank = rank; n = 0; }
    if (rank == bestRank && esp_random() % (++n) == 0) { pick = id; slotOut = s; }
  }
  return pick;
}

// A failed roll on a "steal" hazard in a Bonney scene (encRunChoice), or the
// "deal" ending with nothing taken yet. Once a scene: after the first go
// Pinch is satisfied, whatever it found.
static void bonneyPinch(int pid) {
  ActiveEncounter& enc = encounters[pid];
  if (!enc.bonney || enc.bonStole) return;
  Player& p = G.players[pid];
  int slot = -1;
  const uint8_t id = bonPickTheft(p, slot);
  if (!id) {
    enc.bonStole = BON_LINT;
    bonEmit(BON_EV_PINCH, pid, 0, p.wsClientId);
    Log.notice("BONNEY Pinch went through pid=%d's pack and found only lint", pid);
    return;
  }
  if (--p.invQty[slot] == 0) p.invType[slot] = 0;
  bonHoardPush(id, (uint8_t)pid);
  enc.bonStole = id;
  BON.packAct[pid] = BON_PACK_PINCH;
  BON.saveWanted = true;
  bonEmit(BON_EV_PINCH, pid, id, p.wsClientId);
  Log.notice("BONNEY Pinch took item %d from pid=%d (hoard %d)", (int)id, pid, (int)BON.hoardN);
}

// ── Between the sections ────────────────────────────────────────────────────
// After section 1 she goes back to her crank: the same fire now holds supper.
static void bonToSupper(int pid) {
  BON.mode = BON_SUPPER;
  bonEmit(BON_EV_SUPPER, pid);
  Log.notice("BONNEY back at her crank at (%d,%d): supper's on", (int)BON.q, (int)BON.r);
}

// After section 2: gone, leaving a ring of ash; she comes back at dawn.
static void bonPassDone() {
  BON.ringQ = BON.lastQ = BON.q;
  BON.ringR = BON.lastR = BON.r;
  BON.ringDay = G.dayCount;
  BON.mode = BON_AWAY;
  BON.q = BON.r = -1;
  BON.n++;
}

// ── Dawn (the dawn block, after the scenes are cut short) ───────────────────
static void bonneyDawn() {
  memset(BON.called, 0, sizeof(BON.called));
  if (BON.ringQ >= 0 && BON.ringDay != G.dayCount) BON.ringQ = BON.ringR = -1;   // the ash has blown off
  if (BON.mode == BON_AWAY) {
    bonPlace(false);
    bonEmit(BON_EV_BACK, -1);
    BON.saveWanted = true;
    return;
  }
  // A flood or a river put her fire out: the one time she moves unfinished,
  // to the nearest of her ground, whichever section it was holding.
  if (BON.q >= 0 && !bonWalkable(G.map[BON.r][BON.q].terrain)) {
    int16_t q = BON.q, r = BON.r;
    for (int d = 1; d <= BON_HOME_MAX && q == BON.q && r == BON.r; d++) bonPickAround(BON.q, BON.r, d, d, q, r);
    if (q != BON.q || r != BON.r) {
      BON.q = q; BON.r = r;
      bonEmit(BON_EV_BACK, -1);
      BON.saveWanted = true;
    }
  }
}

// ── Scenes ──────────────────────────────────────────────────────────────────
static bool bonSectionBusy(uint8_t sec) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (encounters[i].active && encounters[i].bonney == sec) return true;
  return false;
}

static bool bonOpen(int pid, uint8_t sec) {
  if (bonSectionBusy(sec)) return false;
  const char* json = encLoadFile(ENC_POOL_BONNEY, sec);
  if (!json) {
    Log.warning("BONNEY section %d: file will not load", (int)sec);
    return false;
  }
  Player& p = G.players[pid];
  ActiveEncounter& enc = encounters[pid];
  enc = {};
  enc.active  = 1;
  enc.bonney  = sec;
  enc.encIdx  = sec;
  enc.hexQ    = (uint8_t)p.q;
  enc.hexR    = (uint8_t)p.r;
  enc.depth   = 0;
  enc.terrain = ENC_POOL_BONNEY;
  enc.bonFrom = 255;
  // Supper: what is in Turnbull's mouth is fixed now -- the newest thing she
  // took off this survivor, else off anyone, else a real apple (0).
  if (sec == BON_SEC_SUPPER) {
    for (int i = (int)BON.hoardN - 1; i >= 0 && !enc.bonApple; i--)
      if (BON.hoard[i].pid == pid) { enc.bonApple = BON.hoard[i].item; enc.bonFrom = (uint8_t)pid; }
    if (!enc.bonApple && BON.hoardN) {
      enc.bonApple = BON.hoard[BON.hoardN - 1].item;
      enc.bonFrom  = BON.hoard[BON.hoardN - 1].pid;
    }
  }
  encEnterStartNode(json, enc);
  GameEvent ev = {};
  ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
  ev.q = p.q; ev.r = p.r;
  ev.encBonney   = sec;            // wire "bonney"
  ev.encBonApple = enc.bonApple;   // wire "apple"
  ev.encBonOwn   = (enc.bonApple && enc.bonFrom == pid) ? 1 : 0;   // wire "own"
  enqEvt(ev);
  Log.notice("BONNEY section %d opened pid=%d (%d,%d) apple=%d", (int)sec, pid, (int)p.q, (int)p.r, (int)enc.bonApple);
  return true;
}

// Called by movePlayer() after the trap check, surface steps only: walking
// into her firelight opens the section her fire holds. True when a scene opened.
static bool bonneyOnArrival(int pid) {
  Player& p = G.players[pid];
  if (p.ll == 0 || p.depth || encounters[pid].active || BON.mode == BON_AWAY) return false;
  if (p.q != BON.q || p.r != BON.r) {
    // Close enough to smell her fire: she calls, once a day.
    if (!BON.called[pid] && hexDistWrap(p.q, p.r, BON.q, BON.r) <= BON_CALL_RANGE) {
      BON.called[pid] = 1;
      bonEmit(BON_EV_CALL, pid, (uint8_t)(esp_random() & 0xFF), p.wsClientId);
    }
    return false;
  }
  return bonOpen(pid, BON.mode == BON_HAND ? BON_SEC_HAND : BON_SEC_SUPPER);
}

// encRunChoice() reached a terminal, non-escape node of a Bonney scene: its
// "bonney_ending" says which way it went.
static void bonneySectionRead(int pid, const char* json, const char* nodeKey) {
  ActiveEncounter& enc = encounters[pid];
  char e[10] = {0};
  jsonCopyStr(jsonObjGet(encNode(json, nodeKey), "bonney_ending"), e, sizeof(e));
  Player& p = G.players[pid];
  if (enc.bonney == BON_SEC_HAND && BON.mode == BON_HAND) {
    if (!strcmp(e, "deal")) {
      bonneyPinch(pid);   // the handshake: robbed now, if Pinch had not got there already
    } else if (!strcmp(e, "caught")) {
      // Pinch caught in your own pack: everything it took this scene comes back.
      const uint8_t id = enc.bonStole;
      if (id && id != BON_LINT && bonHoardTake(id, (uint8_t)pid)) {
        bonGive(p, id);
        BON.packAct[pid] = BON_PACK_CAUGHT;
        bonEmit(BON_EV_CAUGHT, pid, id);
      } else {
        bonEmit(BON_EV_CAUGHT, pid, 0);
      }
    } else {
      Log.warning("BONNEY section 1 node %s has no bonney_ending -- not an ending", nodeKey);
      return;
    }
    bonToSupper(pid);
    BON.saveWanted = true;
    Log.notice("BONNEY section 1 read by pid=%d (%s), stole=%d", pid, e, (int)enc.bonStole);
    return;
  }
  if (enc.bonney == BON_SEC_SUPPER && BON.mode == BON_SUPPER) {
    const uint8_t id = enc.bonApple;
    if (!strcmp(e, "rescue")) {
      // Turnbull hands over what was in his mouth: a curse comes back a curse.
      if (id && bonHoardTake(id, enc.bonFrom)) {
        bonGive(p, id);
        BON.packAct[pid] = BON_PACK_RESCUE;
      } else if (!id) {
        enc.pendingLoot[1] = (uint8_t)min(99, (int)enc.pendingLoot[1] + 1);   // a real apple: banked as food
      }
      bonEmit(BON_EV_RESCUE, pid, id);
    } else if (!strcmp(e, "bargain")) {
      if (id) bonHoardTake(id, enc.bonFrom);   // she ate it: gone for good
      bonEmit(BON_EV_BARGAIN, pid, id);
    } else {
      Log.warning("BONNEY section 2 node %s has no bonney_ending -- not an ending", nodeKey);
      return;
    }
    bonPassDone();
    BON.saveWanted = true;
    Log.notice("BONNEY supper read by pid=%d (%s), apple=%d; pass %d done", pid, e, (int)id, (int)BON.n);
  }
}

// ── Wire ────────────────────────────────────────────────────────────────────
// ",\"bon\":{...}" in sync, the tick broadcast's world and /state -- with its
// own leading comma, so an overflow (0) leaves the JSON whole. Everyone gets
// her hex; the client draws her inside its own vision (her smoke a little
// further out).
static int appendBonneyJson(char* buf, size_t cap) {
  int n = snprintf(buf, cap,
    ",\"bon\":{\"q\":%d,\"r\":%d,\"m\":%d,\"n\":%d,\"aq\":%d,\"ar\":%d}",
    (int)BON.q, (int)BON.r, (int)BON.mode, (int)BON.n, (int)BON.ringQ, (int)BON.ringR);
  return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

// ── Persistence (/save/bonney.bin) ──────────────────────────────────────────
struct __attribute__((packed)) SaveBonney {
  uint32_t magic;
  uint8_t  version;
  uint8_t  cols, rows;   // a file for another board shape is ignored
  int16_t  q, r, lastQ, lastR, ringQ, ringR;
  uint16_t ringDay;
  uint8_t  mode;
  uint16_t n;
  uint8_t  hoardN;
  BonneyLoot hoard[BON_HOARD];
};

// Inside saveGame()'s G.mutex hold (staged, save-stage.hpp). Small enough to
// write every time.
static void saveBonneyFile() {
  SaveOut f = saveOpen(SAVE_BONNEY_F);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", SAVE_BONNEY_F); return; }
  SaveBonney s = {};
  s.magic = BONNEY_SAVE_MAGIC; s.version = 1;
  s.cols = (uint8_t)MAP_COLS; s.rows = (uint8_t)MAP_ROWS;
  s.q = BON.q; s.r = BON.r;
  s.lastQ = BON.lastQ; s.lastR = BON.lastR; s.ringQ = BON.ringQ; s.ringR = BON.ringR;
  s.ringDay = BON.ringDay; s.mode = BON.mode; s.n = BON.n;
  s.hoardN = BON.hoardN;
  memcpy(s.hoard, BON.hoard, sizeof(s.hoard));
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

// Called by tryLoadSave() once map.bin has loaded (wInit() already placed a
// fresh Bonney). No file -- a save from before her -- keeps that one. Every
// coordinate is bounds-checked, like Kane's.
static void loadBonneyFile() {
  if (!SD.exists(SAVE_BONNEY_F)) { Log.notice("No %s: a fresh Bonney", SAVE_BONNEY_F); return; }
  File f = SD.open(SAVE_BONNEY_F, FILE_READ);
  if (!f) { Log.error("SD OPEN FAIL (read): %s", SAVE_BONNEY_F); return; }
  SaveBonney s;
  bool ok = f.read((uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  auto in = [](int16_t q, int16_t r) { return q >= 0 && q < MAP_COLS && r >= 0 && r < MAP_ROWS; };
  auto inOrNone = [&](int16_t q, int16_t r) { return (q == -1 && r == -1) || in(q, r); };
  if (!ok || s.magic != BONNEY_SAVE_MAGIC || s.version != 1 || s.cols != MAP_COLS || s.rows != MAP_ROWS ||
      s.mode > BON_AWAY || (s.mode == BON_AWAY ? !inOrNone(s.q, s.r) : !in(s.q, s.r)) ||
      !inOrNone(s.lastQ, s.lastR) || !inOrNone(s.ringQ, s.ringR) || s.hoardN > BON_HOARD) {
    Log.warning("%s unreadable or for another board - a fresh Bonney", SAVE_BONNEY_F);
    return;
  }
  BON.q = s.q; BON.r = s.r;
  BON.lastQ = s.lastQ; BON.lastR = s.lastR; BON.ringQ = s.ringQ; BON.ringR = s.ringR;
  BON.ringDay = s.ringDay; BON.mode = s.mode; BON.n = s.n;
  BON.hoardN = s.hoardN;
  memcpy(BON.hoard, s.hoard, sizeof(BON.hoard));
  for (int i = 0; i < BON.hoardN; i++) if (BON.hoard[i].pid >= MAX_PLAYERS) BON.hoard[i].pid = 255;
  Log.notice("SD READ: %s fire (%d,%d) mode=%d passes=%d hoard=%d",
             SAVE_BONNEY_F, (int)BON.q, (int)BON.r, (int)BON.mode, (int)BON.n, (int)BON.hoardN);
}
