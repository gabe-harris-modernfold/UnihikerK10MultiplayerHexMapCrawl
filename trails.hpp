#pragma once
// ── Trail sides: which way each walker crossed each hex ──────────────────────
// HexCell.footprints and HexCell.tireTrack say only THAT someone crossed a
// hex. Tracks are the game's tracking mechanism, and the web client draws them
// as trails of prints and ruts pointing the way their maker went
// (data/trail-field.js) -- so this keeps the way: for every surface hex, per
// seat and for the caravan, the side the latest pass came in by and the side
// it left by.
//
// One byte per (hex, walker), TRAIL_SLOTS walkers:
//   bits 0-2  side it came in by (DQ/DR index), TRAIL_NO_SIDE = it started here
//   bits 3-5  side it left by, TRAIL_NO_SIDE = it has not left (or went below)
//   bit  6    ridden: the step in was made on tracks-flagged gear (Motorbike),
//             so the client draws a rut, not prints
//   0xFF      no record: this walker has never crossed this hex since the
//             world was made (or since before this file existed)
// Only the latest pass is kept; the client keeps more of what it saw itself.
//
// Not part of HexCell, so neither MAP_BYTES nor SAVE_VERSION moved: it is a
// PSRAM block of its own (allocPsramGlobals), saved to its own file
// (SAVE_TRL_F, network-persistence.hpp), and on the wire as a sparse "trl"
// tail on vis disks, survey rings and the sync (hex-map.hpp).
//
// Surface only. The bunker tunnel board is transient and the client reads its
// trails from the footprint bits alone.
//
// Every writer below runs under G.mutex, like the HexCell bits it mirrors.

static constexpr int     TRAIL_SLOTS   = MAX_PLAYERS + 1;   // a slot per seat, then the caravan
static constexpr int     TRAIL_CARAVAN = MAX_PLAYERS;
static constexpr uint8_t TRAIL_NONE    = 0xFF;
static constexpr uint8_t TRAIL_NO_SIDE = 7;
static constexpr uint8_t TRAIL_RIDDEN  = 0x40;
static constexpr size_t  TRAIL_BYTES   = (size_t)MAP_ROWS * MAP_COLS * TRAIL_SLOTS;

static uint8_t (*trailSides)[MAP_COLS][TRAIL_SLOTS] = nullptr;   // PSRAM: [MAP_ROWS] (allocPsramGlobals)
// Set by every change, cleared by the save that writes SAVE_TRL_F. A cleared
// board sets it too, so the next save overwrites the last world's file.
static bool trailDirty = false;

static inline uint8_t trailPack(int in, int out, bool ridden) {
  return (uint8_t)((in & 7) | ((out & 7) << 3) | (ridden ? TRAIL_RIDDEN : 0));
}
static inline int trailWrap(int v, int n) { return ((v % n) + n) % n; }

// A fresh board: every walker's every record gone.
static void trailClearAll() {
  if (!trailSides) return;
  memset(trailSides, TRAIL_NONE, TRAIL_BYTES);
  trailDirty = true;
}

// Walker `slot` has turned up on (q, r) without walking there: a spawn, a
// respawn, climbing out of a hatch. A record still open from arriving here
// (out = none) is kept -- that is a reconnect, standing where they stopped.
static void trailStart(int slot, int q, int r) {
  if (!trailSides || slot < 0 || slot >= TRAIL_SLOTS) return;
  uint8_t& b = trailSides[r][q][slot];
  if (b != TRAIL_NONE && ((b >> 3) & 7) == TRAIL_NO_SIDE) return;
  b = trailPack(TRAIL_NO_SIDE, TRAIL_NO_SIDE, false);
  trailDirty = true;
}

// Walker `slot` stepped from (fq, fr) through side dir. The hex it left gets
// that side as its way out -- on the pass it arrived by if that is still open,
// else on a pass that starts there -- and the hex it reached gets a pass in by
// the opposite side, not yet left. ridden: the step was made on a vehicle.
static void trailStep(int slot, int fq, int fr, int dir, bool ridden) {
  if (!trailSides || slot < 0 || slot >= TRAIL_SLOTS || dir < 0 || dir > 5) return;
  uint8_t& from = trailSides[fr][fq][slot];
  bool open = from != TRAIL_NONE && ((from >> 3) & 7) == TRAIL_NO_SIDE;
  from = open ? (uint8_t)((from & ~0x38) | (dir << 3)) : trailPack(TRAIL_NO_SIDE, dir, ridden);
  int tq = trailWrap(fq + DQ[dir], MAP_COLS), tr = trailWrap(fr + DR[dir], MAP_ROWS);
  trailSides[tr][tq][slot] = trailPack((dir + 3) % 6, TRAIL_NO_SIDE, ridden);
  trailDirty = true;
}

// Which side of (fq, fr) leads to (tq, tr), wrapping; -1 when they are not
// neighbours (the caravan never jumps, but a waypoint reset could).
static int trailSideTo(int fq, int fr, int tq, int tr) {
  for (int d = 0; d < 6; d++)
    if (trailWrap(fq + DQ[d], MAP_COLS) == tq && trailWrap(fr + DR[d], MAP_ROWS) == tr) return d;
  return -1;
}
