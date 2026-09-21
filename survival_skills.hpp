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

// ── Skill check resolution ────────────────────────────────────────────────────

// Standing modifier applied to every skill check, from wounds and archetype.
//   Major wounds  → −1 each on ALL skills.
//   Minor wounds  → −1 each on Endure only.
//   Endurer (5)   → +1 on Endure (archetype trait).
// Call while holding G.mutex.
static int checkSkillMod(const Player& p, uint8_t skill) {
  int mod = -(int)p.wounds[WOUND_MAJOR];
  if (skill == (uint8_t)SK_ENDURE) {
    mod -= (int)p.wounds[WOUND_MINOR];
    if (p.archetype == 5) mod++;
  }
  return mod;
}

// Call while holding G.mutex (reads player skills/wounds).
// `bonus` is a situational modifier from the caller; wound and archetype
// modifiers are folded in here so no call site can forget them.
static CheckResult resolveCheck(int pid, uint8_t skill, uint8_t dn, int bonus) {
  uint32_t rnd  = esp_random();
  uint8_t  sk   = (skill < NUM_SKILLS) ? skill : 0;
  const Player& p = G.players[pid];
  CheckResult r;
  r.r1       = 1 + (int)(rnd         % 6);
  r.r2       = 1 + (int)((rnd >> 8)  % 6);
  r.dn       = (int)dn;
  r.skillVal = (int)p.skills[sk];
  r.mods     = bonus + checkSkillMod(p, sk);
  r.total    = r.r1 + r.r2 + r.skillVal + r.mods;
  r.success  = (r.total >= r.dn);
  return r;
}

// ── Wound helpers ─────────────────────────────────────────────────────────────
// Apply a wound of the given tier, capped at WOUND_MAX_EACH.  Returns the
// number actually inflicted (0 if the tier was already full).
static uint8_t addWound(Player& p, int tier, uint8_t count) {
  if (tier < 0 || tier >= NUM_WOUND_TIER || count == 0) return 0;
  uint8_t before = p.wounds[tier];
  uint8_t after  = (uint8_t)min((int)before + (int)count, (int)WOUND_MAX_EACH);
  p.wounds[tier] = after;
  return (uint8_t)(after - before);
}

// Heal one wound of the given tier.  Returns true if one was cleared.
static bool healWound(Player& p, int tier) {
  if (tier < 0 || tier >= NUM_WOUND_TIER || p.wounds[tier] == 0) return false;
  p.wounds[tier]--;
  return true;
}

// ── §4 Resource economy helpers ───────────────────────────────────────────────

// Move F track by dir (+1 or -1).  Checks threshold crossings and accumulates
// llDelta (+1 = restore LL, -1 = lose LL).  F clamps [1,6].
// F thresholds: box 4 (fThreshBelow bit0), box 2 (bit1), box 1 (bit2 — fires
// when F==1 and dir==-1).
//
// The floor penalty (bit2) mirrors applyWStep's: without it food had two
// breakpoints to water's three, and starving *at the floor* cost nothing at
// all once both bits had latched — so a survivor could sit at F1 indefinitely
// and only thirst would keep billing them. Measured across 9 bot runs,
// hunger was 0.2% of all LL lost and killed nobody, against thirst's 34%.
// This is the asymmetry that made starvation decorative.
static void applyFStep(Player& p, int dir, int& llDelta) {
  uint8_t oldF = p.food;
  if (dir > 0) {
    if (p.food < 6) p.food++;
    // Upward crossings → restore LL if threshold was previously tripped
    if (oldF < 4 && p.food >= 4 && (p.fThreshBelow & 1)) { p.fThreshBelow &= ~1; llDelta++; }
    if (oldF < 2 && p.food >= 2 && (p.fThreshBelow & 2)) { p.fThreshBelow &= ~2; llDelta++; }
    // Clear the floor penalty when F rises off 1, same as W's bit2
    if (oldF < 2 && p.food >= 2 && (p.fThreshBelow & 4)) { p.fThreshBelow &= ~4; llDelta++; }
  } else {
    if (p.food > 1) {
      p.food--;
      // Downward crossings → LL loss on first crossing below each threshold
      if (oldF >= 4 && p.food < 4 && !(p.fThreshBelow & 1)) { p.fThreshBelow |= 1; llDelta--; }
      if (oldF >= 2 && p.food < 2 && !(p.fThreshBelow & 2)) { p.fThreshBelow |= 2; llDelta--; }
    } else {
      // F is already at floor 1: "crossing below box 1" — fires once
      if (!(p.fThreshBelow & 4)) { p.fThreshBelow |= 4; llDelta--; }
    }
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
  wOnPlayerAction(p.q, p.r, (uint8_t)cost);  // lays a scent track (world-system-spec.md)
}
static inline void addScore(Player& p, GameEvent& ev, int pts) {
  ev.actScoreD = (int16_t)pts;
  p.score      = (uint16_t)min((int)p.score + pts, 65535);
}
