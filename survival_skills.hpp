#pragma once
// ── Survival skills: check resolution, radiation, resource economy ────────────
// Included from Esp32HexMapCrawl.ino before inventory_items.hpp.
// Has access to all globals, constants, and structs defined above it in .ino.

// broadcastCheck is defined in network-sync.hpp (included after this file).
static void broadcastCheck(int pid, uint8_t skill, CheckResult& r);
// saveGame() is defined in network-persistence.hpp (included after this file).
void saveGame();
// ledFlash is defined in Esp32HexMapCrawl.ino (Core 1 safe — sets LED + endMs timer).
void ledFlash(uint8_t r, uint8_t g, uint8_t b);
// effectiveMaxLL is defined in inventory_items.hpp (included after this file).
static uint8_t effectiveMaxLL(int pid);

// ── §6.2 Radiation status sync ───────────────────────────────────────────────
// Sync ST_RADSICK to current radiation level (R ≥ 4 = Rad-Sick).
static void updateRadStatus(Player& p) {
  if (p.radiation >= 4) p.statusBits |=  ST_RADSICK;
  else                  p.statusBits &= ~ST_RADSICK;
}

// ── Skill check resolution ────────────────────────────────────────────────────
// Call while holding G.mutex (reads player skills/status).
// bonus = item bonus from client (0–2). Condition penalties applied automatically.
static CheckResult resolveCheck(int pid, uint8_t skill, uint8_t dn, uint8_t bonus) {
  uint32_t rnd  = esp_random();
  CheckResult r;
  r.r1       = 1 + (int)(rnd         % 6);
  r.r2       = 1 + (int)((rnd >> 8)  % 6);
  r.dn       = (int)dn;
  r.skillVal = (int)G.players[pid].skills[skill < NUM_SKILLS ? skill : 0];

  // Net modifier: item bonus + condition penalties (§3.1 Step 4)
  int cm = (int)bonus;
  uint8_t sb = G.players[pid].statusBits;
  if (sb & ST_RADSICK)  cm -= 1;                           // Rad-Sick: −1 all
  if (sb & ST_FEVERED)  cm -= 1;                           // Fevered:  −1 all
  if (G.players[pid].wounds[1] > 0)                cm -= 1; // Major wound: −1 all
  if (skill == SK_ENDURE && G.players[pid].wounds[0] > 0) cm -= 1; // Minor: −1 Endure only
  r.mods   = cm;
  r.total  = r.r1 + r.r2 + r.skillVal + r.mods;
  r.success = (r.total >= r.dn);
  return r;
}

// ── §4 Resource economy helpers ───────────────────────────────────────────────

// Move F track by dir (+1 or -1).  Checks threshold crossings and accumulates
// llDelta (+1 = restore LL, -1 = lose LL).  F clamps [1,6].
// F thresholds: box 4 (fThreshBelow bit0), box 2 (fThreshBelow bit1).
static void applyFStep(Player& p, int dir, int& llDelta) {
  uint8_t oldF = p.food;
  if (dir > 0) {
    if (p.food < 6) p.food++;
    // Upward crossings → restore LL if threshold was previously tripped
    if (oldF < 4 && p.food >= 4 && (p.fThreshBelow & 1)) { p.fThreshBelow &= ~1; llDelta++; }
    if (oldF < 2 && p.food >= 2 && (p.fThreshBelow & 2)) { p.fThreshBelow &= ~2; llDelta++; }
  } else {
    if (p.food > 1) p.food--;
    // Downward crossings → LL loss on first crossing below each threshold
    if (oldF >= 4 && p.food < 4 && !(p.fThreshBelow & 1)) { p.fThreshBelow |= 1; llDelta--; }
    if (oldF >= 2 && p.food < 2 && !(p.fThreshBelow & 2)) { p.fThreshBelow |= 2; llDelta--; }
  }
}

// Move W track by dir (+1 or -1).  Checks threshold crossings.  W clamps [1,6].
// W thresholds: box 5 (bit0), box 3 (bit1), box 1 (bit2 — fires when W==1 and dir==-1).
static void applyWStep(Player& p, int dir, int& llDelta) {
  uint8_t oldW = p.water;
  if (dir > 0) {
    if (p.water < 6) p.water++;
    if (oldW < 5 && p.water >= 5 && (p.wThreshBelow & 1)) { p.wThreshBelow &= ~1; llDelta++; }
    if (oldW < 3 && p.water >= 3 && (p.wThreshBelow & 2)) { p.wThreshBelow &= ~2; llDelta++; }
    // bit2 (floor penalty): clear when W rises from 1 → 2; oldW<1 was dead code since W floors at 1
    if (oldW < 2 && p.water >= 2 && (p.wThreshBelow & 4)) { p.wThreshBelow &= ~4; llDelta++; }
  } else {
    if (p.water > 1) {
      p.water--;
      if (oldW >= 5 && p.water < 5 && !(p.wThreshBelow & 1)) { p.wThreshBelow |= 1; llDelta--; }
      if (oldW >= 3 && p.water < 3 && !(p.wThreshBelow & 2)) { p.wThreshBelow |= 2; llDelta--; }
    } else {
      // W is already at floor 1; "crossing below box 1" — only fires once
      if (!(p.wThreshBelow & 4)) { p.wThreshBelow |= 4; llDelta--; }
    }
  }
}

// ── Action helpers ────────────────────────────────────────────────────────────
static inline void spendMP(Player& p, int cost) {
  p.movesLeft = (int8_t)max(0, (int)p.movesLeft - cost);
}
static inline void addScore(Player& p, GameEvent& ev, int pts) {
  ev.actScoreD = (int16_t)pts;
  p.score      = (uint16_t)min((int)p.score + pts, 65535);
}
