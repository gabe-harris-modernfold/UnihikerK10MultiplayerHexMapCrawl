#pragma once
// ── Item system: effects, equipment, trading, ground items ────────────────────
// Included from Esp32HexMapCrawl.ino after survival_skills.hpp.
// Has access to all globals, constants, game_logic helpers, and structs.

// ═══════════════════════════════════════════════════════════════════════════
// ── Item System ──────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

// ── Item system strings ───────────────────────────────────────────────────
[[maybe_unused]] static const char* EQUIP_SLOT_NAMES[6] = {
  "None","Head","Body","Hand","Feet","Vehicle"
};

// ── Equipment queries ─────────────────────────────────────────────────────
// All take the pid and read G.players[pid].equip[]; call while holding G.mutex.

// True if any equipped item carries EFX_NARRATIVE with the given NAR_* param.
static bool hasNarrativeParam(int pid, uint8_t param) {
  const Player& p = G.players[pid];
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (!def) continue;
    if (def->effectId  == EFX_NARRATIVE && def->effectParam  == param) return true;
    if (def->effectId2 == EFX_NARRATIVE && def->effectParam2 == param) return true;
  }
  return false;
}

// +1 vision per equipped item with EFX_REVEAL_FOG param 1 (Dark Goggles, Glow Dentures, …).
static int equipVisionBonus(int pid) {
  const Player& p = G.players[pid];
  int bonus = 0;
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (!def) continue;
    if (def->effectId  == EFX_REVEAL_FOG && def->effectParam  == 1) bonus++;
    if (def->effectId2 == EFX_REVEAL_FOG && def->effectParam2 == 1) bonus++;
  }
  return bonus;
}

// Typed-inventory slot count in effect right now: archetype base plus STAT_SLOTS
// from equipment (Hoarder's Rig), capped at the array size.  This is the ONE
// number every slot loop and carry-cap check must use.
static uint8_t effectiveInvSlots(const Player& p) {
  int slots = (int)p.invSlots;
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (def) slots += (int)def->statMods[STAT_SLOTS];
  }
  return (uint8_t)constrain(slots, 1, (int)INV_SLOTS_MAX);
}

// ── Effect dispatch table ─────────────────────────────────────────────────
// Function signature: (pid, itemId, param)
typedef void (*EffectFn)(int pid, uint8_t itemId, uint8_t param);
static EffectFn effectTable[EFX_COUNT] = {};  // indexed by EffectId enum

static void efxThreatMod(int pid, uint8_t itemId, uint8_t param) {
  // param is treated as signed int8_t: positive raises TC, negative lowers it.
  int delta = (int)(int8_t)param;
  G.threatClock = (uint8_t)constrain((int)G.threatClock + delta, 0, 20);
}

// EFX_CURE_STATUS — the status-condition system was removed; "curing" now
// means closing wounds.  param = number of wounds healed, minor tier first.
static void efxCureStatus(int pid, uint8_t itemId, uint8_t param) {
  (void)itemId;
  Player& p = G.players[pid];
  for (int n = 0; n < (int)param; n++) {
    if (!healWound(p, WOUND_MINOR) && !healWound(p, WOUND_MAJOR)) break;
  }
}

// EFX_NARRATIVE — server-side handler.  Only the NAR_* params listed in the
// .ino are acted on here; passive equipment params (NAR_FIRE_STARTER,
// NAR_COLD_IMMUNE, NAR_*_FORAGE, NAR_SCAV_DOUBLE) are queried where they apply
// via hasNarrativeParam().  Params 11/12 are client-side and are echoed back
// in the item_result "efxp" field.
static void efxNarrative(int pid, uint8_t itemId, uint8_t param) {
  (void)itemId;
  if (param == NAR_LL_CAP_DOWN) {
    Player& p = G.players[pid];
    if (effectiveMaxLL(pid) > 1) p.llCapPenalty++;
    uint8_t cap = effectiveMaxLL(pid);
    if (p.ll > cap) p.ll = cap;
    return;
  }
  if (param == NAR_TELEPORT) {
    // teleport_random — move player to a random surveyed hex
    // surveyedMap bitmask: bit (r*MAP_COLS+q) => q = idx%MAP_COLS, r = idx/MAP_COLS
    static constexpr int totalCells = MAP_ROWS * MAP_COLS;
    static uint16_t surveyed[totalCells];
    int count = 0;
    const Player& pl = G.players[pid];
    for (int idx = 0; idx < totalCells; idx++) {
      if ((pl.surveyedMap[idx / 8] >> (idx % 8)) & 1) {
        surveyed[count++] = (uint16_t)idx;
      }
    }
    if (count > 1) {
      int pick = random(0, count);
      int attempts = 0;
      int16_t tq = (int16_t)(surveyed[pick] % MAP_COLS);
      int16_t tr = (int16_t)(surveyed[pick] / MAP_COLS);
      while (tq == G.players[pid].q && tr == G.players[pid].r && attempts++ < 10) {
        pick = random(0, count);
        tq   = (int16_t)(surveyed[pick] % MAP_COLS);
        tr   = (int16_t)(surveyed[pick] / MAP_COLS);
      }
      G.players[pid].q = tq;
      G.players[pid].r = tr;
    }
  }
}

// EFX_REVEAL_FOG — param==1 is a passive vision bonus handled in playerVisParams().
// param>=2: one-shot reveal — mark all cells within radius in surveyedMap.
// param==99: reveal entire map.
static void efxRevealFog(int pid, uint8_t itemId, uint8_t param) {
  (void)itemId;
  if (param < 2) return;
  Player& p = G.players[pid];
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int q = 0; q < MAP_COLS; q++) {
      if (param == 99 || hexDistWrap((int)p.q, (int)p.r, q, r) <= (int)param) {
        int idx = r * MAP_COLS + q;
        p.surveyedMap[idx / 8] |= (uint8_t)(1 << (idx % 8));
      }
    }
  }
}

static void initEffectTable() {
  effectTable[EFX_THREAT_MOD]  = efxThreatMod;
  effectTable[EFX_CURE_STATUS] = efxCureStatus;
  effectTable[EFX_NARRATIVE]   = efxNarrative;
  effectTable[EFX_REVEAL_FOG]  = efxRevealFog;
}

static void dispatchEffect(int pid, const ItemDef& item) {
  if (item.effectId  && item.effectId  < EFX_COUNT && effectTable[item.effectId])
    effectTable[item.effectId](pid, item.id, item.effectParam);
  if (item.effectId2 && item.effectId2 < EFX_COUNT && effectTable[item.effectId2])
    effectTable[item.effectId2](pid, item.id, item.effectParam2);
}

// ── applyDawnItemCosts ────────────────────────────────────────────────────
// Called at dawn for each connected player. For each equipped item with an
// opCost[], checks if the player can afford it. If yes: deducts resources and
// the item's statMods are considered active for the day (they are always
// included in calcEffectiveStat). If no: marks the item as "unfuelled" this
// dawn by logging — statMods via calcEffectiveStat remain present in code
// but the STAT_MP bonus is gated here by conditionally adding to movesLeft.
// We gate STAT_MP specifically because it's the only stat that is set once at
// dawn (movesLeft) rather than computed dynamically.
static void applyDawnItemCosts(int pid) {
  Player& p = G.players[pid];
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    uint8_t eid = p.equip[s];
    if (!eid) continue;
    const ItemDef* def = getItemDef(eid);
    if (!def) continue;
    bool hasCost = false;
    for (int k = 0; k < 5; k++) if (def->opCost[k]) { hasCost = true; break; }
    if (!hasCost) continue;
    // Check affordability
    bool canAfford = true;
    for (int k = 0; k < 5; k++) {
      if (def->opCost[k] > p.inv[k]) { canAfford = false; break; }
    }
    if (canAfford) {
      for (int k = 0; k < 5; k++) p.inv[k] -= def->opCost[k];
      // STAT_MP bonus is added to movesLeft here (set by dawnUpkeep BEFORE this call)
      p.movesLeft = (int8_t)min(127, (int)p.movesLeft + (int)def->statMods[STAT_MP]);
    } else {
      // If we can't afford the cost, the item's STAT_MP bonus does NOT apply.
      // Other statMods (LL, etc.) still show via calcEffectiveStat — game design
      // choice: equipment stays on, but fuel-gated bonuses are dormant.
    }
  }
  // Passive EFX_THREAT_MOD from equipped items — applied each dawn to keep TC suppressed
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    uint8_t eid = p.equip[s];
    if (!eid) continue;
    const ItemDef* def = getItemDef(eid);
    if (!def) continue;
    if (def->effectId  == EFX_THREAT_MOD) G.threatClock = (uint8_t)constrain((int)G.threatClock + (int)(int8_t)def->effectParam,  0, 20);
    if (def->effectId2 == EFX_THREAT_MOD) G.threatClock = (uint8_t)constrain((int)G.threatClock + (int)(int8_t)def->effectParam2, 0, 20);
  }
}

// ── useItem ───────────────────────────────────────────────────────────────
// Use the item in inventory slot slotIdx.  Consumables apply their statMods
// and effects and lose one charge.  Key items with an effect (Pre-War Net
// Map, Cursed Device) may be "read" any number of times and are never
// consumed.  Equipment and materials cannot be used.  Must hold G.mutex.
static bool useItem(int pid, uint8_t slotIdx) {
  if (slotIdx >= INV_SLOTS_MAX) return false;
  Player& p = G.players[pid];
  uint8_t itemId = p.invType[slotIdx];
  if (!itemId) return false;
  const ItemDef* def = getItemDef(itemId);
  if (!def) return false;
  bool isKeyWithEffect = (def->category == ITEM_KEY) && (def->effectId != EFX_NONE);
  if (def->category != ITEM_CONSUMABLE && !isKeyWithEffect) return false;


  // Apply stat modifiers — food/water via threshold-aware steps to propagate LL events
  int llDelta = 0;
  if (def->statMods[STAT_FOOD]) {
    int steps = (int)def->statMods[STAT_FOOD];
    for (int i = 0; i < abs(steps); i++) applyFStep(p, steps > 0 ? 1 : -1, llDelta);
  }
  if (def->statMods[STAT_WATER]) {
    int steps = (int)def->statMods[STAT_WATER];
    for (int i = 0; i < abs(steps); i++) applyWStep(p, steps > 0 ? 1 : -1, llDelta);
  }
  llDelta += (int)def->statMods[STAT_LL];
  if (llDelta != 0) p.ll = (uint8_t)constrain((int)p.ll + llDelta, 0, (int)effectiveMaxLL(pid));
  if (def->statMods[STAT_RAD]) p.radiation = (uint8_t)constrain((int)p.radiation + def->statMods[STAT_RAD], 0, 10);
  if (def->statMods[STAT_MP])  p.movesLeft = (int8_t)max(0, (int)p.movesLeft + def->statMods[STAT_MP]);

  // Dispatch effects
  dispatchEffect(pid, *def);

  if (isKeyWithEffect) return true;  // key items are not spent

  // Decrement quantity; clear slot if exhausted
  if (p.invQty[slotIdx] > 1) {
    p.invQty[slotIdx]--;
  } else {
    p.invType[slotIdx] = 0;
    p.invQty[slotIdx]  = 0;
  }
  return true;
}

// ── equipItem ─────────────────────────────────────────────────────────────
// Move item from inventory slot slotIdx into the appropriate equipment slot.
// Swaps with any currently equipped item (returns it to the first free inv slot).
// Returns true on success. Must hold G.mutex.
static bool equipItem(int pid, uint8_t slotIdx) {
  if (slotIdx >= INV_SLOTS_MAX) return false;
  Player& p = G.players[pid];
  uint8_t itemId = p.invType[slotIdx];
  if (!itemId) return false;
  const ItemDef* def = getItemDef(itemId);
  if (!def) return false;
  if (def->equipSlot == EQUIP_NONE) return false; // not equippable
  if (def->category != ITEM_EQUIPMENT) return false;

  uint8_t eslot = (uint8_t)(def->equipSlot - 1); // 0-indexed (EQUIP_HEAD=1 → 0)

  // Unequip current item in that slot if any, return it to inventory
  uint8_t prev = p.equip[eslot];
  if (prev) {
    // Find a free inv slot
    bool placed = false;
    uint8_t slots = effectiveInvSlots(p);
    for (int i = 0; i < slots && !placed; i++) {
      if (!p.invType[i]) {
        p.invType[i] = prev; p.invQty[i] = 1;
        placed = true;
      }
    }
    if (!placed) {
      return false;
    }
  }

  // Place new item in equipment slot; clear inventory slot
  p.equip[eslot]     = itemId;
  p.invType[slotIdx] = 0;
  p.invQty[slotIdx]  = 0;

  return true;
}

// ── unequipItem ───────────────────────────────────────────────────────────
// Remove item from equipment slot (0-indexed eslot, 0=HEAD..4=VEHICLE).
// Moves it to the first free inventory slot. Returns true on success.
// Refused if the item grants pack slots that are currently occupied — the
// pack would shrink around items the player could no longer reach.
// Must hold G.mutex.
static bool unequipItem(int pid, uint8_t eslot) {
  if (eslot >= EQUIP_SLOTS) return false;
  Player& p = G.players[pid];
  uint8_t itemId = p.equip[eslot];
  if (!itemId) return false;
  // Slot count once this item is gone
  p.equip[eslot] = 0;
  uint8_t newSlots = effectiveInvSlots(p);
  p.equip[eslot] = itemId;
  for (int i = newSlots; i < INV_SLOTS_MAX; i++) {
    if (p.invType[i]) return false;  // would orphan an item beyond the new cap
  }
  // Find free inv slot within the new cap
  for (int i = 0; i < newSlots; i++) {
    if (!p.invType[i]) {
      p.invType[i] = itemId; p.invQty[i] = 1;
      p.equip[eslot] = 0;
      // Clamp LL to new effective ceiling now that the item is no longer equipped
      uint8_t newCap = effectiveMaxLL(pid);
      if (p.ll > newCap) p.ll = newCap;
      return true;
    }
  }
  return false;
}

// ── dropItem ──────────────────────────────────────────────────────────────
// Drop qty of item from inventory slot slotIdx at the player's current hex.
// Creates/extends a GroundItem entry. Returns true on success.
// Must hold G.mutex.
static bool dropItem(int pid, uint8_t slotIdx, uint8_t qty) {
  if (slotIdx >= INV_SLOTS_MAX || qty == 0) return false;
  Player& p = G.players[pid];
  if (!p.invType[slotIdx]) return false;
  if (p.invQty[slotIdx] < qty) return false;
  uint8_t itemId = p.invType[slotIdx];

  // Find or create a GroundItem slot at this hex
  int gslot = -1;
  for (int g = 0; g < MAX_GROUND; g++) {
    if (groundItems[g].itemType == itemId &&
        groundItems[g].q == p.q && groundItems[g].r == p.r) {
      gslot = g; break; // stack onto existing pile
    }
  }
  if (gslot < 0) {
    for (int g = 0; g < MAX_GROUND; g++) {
      if (!groundItems[g].itemType) { gslot = g; break; }
    }
  }
  if (gslot < 0) {
    return false;
  }

  // Remove from inventory
  p.invQty[slotIdx] -= qty;
  if (!p.invQty[slotIdx]) { p.invType[slotIdx] = 0; }

  // Place on ground
  groundItems[gslot].q        = p.q;
  groundItems[gslot].r        = p.r;
  groundItems[gslot].itemType = itemId;
  groundItems[gslot].qty      = (uint8_t)min(255, (int)groundItems[gslot].qty + (int)qty);

  return true;
}

// ── pickupGroundItem ──────────────────────────────────────────────────────
// Pick up all of a ground item at gslot. Player must be in same hex.
// Returns true on success, false if blocked (different hex / no inv space).
// Must hold G.mutex.
static bool pickupGroundItem(int pid, uint8_t gslot) {
  if (gslot >= MAX_GROUND) return false;
  Player& p = G.players[pid];
  GroundItem& gi = groundItems[gslot];
  if (!gi.itemType) return false;
  if (gi.q != p.q || gi.r != p.r) return false; // wrong hex

  uint8_t itemId = gi.itemType;
  uint8_t qty    = gi.qty;

  // Find existing stack or free slot in inventory
  int freeSlot = -1, stackSlot = -1;
  uint8_t slots = effectiveInvSlots(p);
  for (int i = 0; i < slots; i++) {
    if (p.invType[i] == itemId && stackSlot < 0) stackSlot = i;
    if (!p.invType[i] && freeSlot < 0) freeSlot = i;
  }
  int targetSlot = (stackSlot >= 0) ? stackSlot : freeSlot;
  if (targetSlot < 0) {
    return false;
  }
  const ItemDef* def = getItemDef(itemId);
  uint8_t maxStack = def ? def->maxStack : 1;

  // How many can we take?
  uint8_t canTake = qty;
  if (stackSlot >= 0) {
    uint8_t room = (uint8_t)max(0, (int)maxStack - (int)p.invQty[stackSlot]);
    canTake = min(qty, room);
  }
  if (!canTake) {
    return false;
  }

  p.invType[targetSlot] = itemId;
  p.invQty[targetSlot]  = (uint8_t)min((int)maxStack, (int)p.invQty[targetSlot] + (int)canTake);
  gi.qty -= canTake;
  if (!gi.qty) { gi.itemType = 0; gi.q = 0; gi.r = 0; }

  return true;
}

// ── hasPassTerrainBit ─────────────────────────────────────────────────────
// Returns true if any equipped item on player pid unlocks the given terrain bit.
// Must hold G.mutex.
static bool hasPassTerrainBit(int pid, uint8_t terrainBit) {
  const Player& p = G.players[pid];
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (def && (def->passTerrainBits & terrainBit)) return true;
  }
  return false;
}

// ── canEnterTerrain ───────────────────────────────────────────────────────
// Single source of truth for "may pid step onto terrain t, and at what base
// MC".  Used by movePlayer() and computeValidMoves() so the direction mask the
// client receives always agrees with what the server will accept.
// Must hold G.mutex.
static bool canEnterTerrain(int pid, uint8_t t, uint8_t* mcOut) {
  if (t >= NUM_TERRAIN) return false;
  uint8_t mc = TERRAIN_MC[t];
  if (mc == 255) {
    if (t == 11 && hasPassTerrainBit(pid, TERR_PASS_RIVER)) mc = RIVER_MC;
    else return false;
  }
  if (t == 8 && hasPassTerrainBit(pid, TERR_PASS_CLIFF)) mc = CLIFF_MC;  // climbing gear
  if (mcOut) *mcOut = mc;
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════

// Effective Movement Points = LL + 3 − major wounds − encumbrance penalty
//   + STAT_MP from equipment (items with no opCost always active;
//     fuel-gated items like motorbike have STAT_MP applied in applyDawnItemCosts).
// Floored at 2.  Keep data/ui-panels.js's MP help text in sync with this.
// Called while holding G.mutex.
static int effectiveMP(int pid) {
  Player& p  = G.players[pid];
  int     mp = (int)p.ll + 3;
  mp -= (int)p.wounds[WOUND_MAJOR];        // each major wound costs 1 MP/day
  // Encumbrance: resource tokens carried above the pack size cost 1 MP
  int used = 0;
  for (int k = 0; k < 5; k++) used += (int)p.inv[k];
  if (used > (int)effectiveInvSlots(p)) mp--;
  // Add free (no opCost) STAT_MP bonuses from equipped items
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (!def || !def->statMods[STAT_MP]) continue;
    // Check if this item has any opCost — if yes, its MP bonus is handled by applyDawnItemCosts
    bool hasCost = false;
    for (int k = 0; k < 5; k++) if (def->opCost[k]) { hasCost = true; break; }
    if (!hasCost) mp += (int)def->statMods[STAT_MP];
  }
  return max(2, mp);  // floor of 2
}

// ── effectiveMaxLL ────────────────────────────────────────────────────────────
// Returns the player's current LL ceiling: 7, plus STAT_LL bonuses from all
// equipped items (e.g. Body Armor +2 → 9), minus any permanent penalty
// (Uranium Candy).  Never below 1.  Must hold G.mutex.
static uint8_t effectiveMaxLL(int pid) {
  int cap = 7 - (int)G.players[pid].llCapPenalty;
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!G.players[pid].equip[s]) continue;
    const ItemDef* def = getItemDef(G.players[pid].equip[s]);
    if (def) cap += (int)def->statMods[STAT_LL];
  }
  return (uint8_t)max(1, cap);
}

// ── Trade helpers (call while holding G.mutex) ────────────────────────────────

// Returns true if both players are connected and standing on the same hex.
static bool samehex(int pidA, int pidB) {
  if (pidA < 0 || pidA >= MAX_PLAYERS || pidB < 0 || pidB >= MAX_PLAYERS) return false;
  Player& a = G.players[pidA];
  Player& b = G.players[pidB];
  return a.connected && b.connected && a.q == b.q && a.r == b.r;
}

// Returns true if player's inv[] has at least qty[i] of every resource.
static bool hasResources(int pid, const uint8_t qty[5]) {
  if (pid < 0 || pid >= MAX_PLAYERS) return false;
  Player& p = G.players[pid];
  for (int i = 0; i < 5; i++)
    if (p.inv[i] < qty[i]) return false;
  return true;
}

// Transfer give[] from fromPid to toPid and want[] from toPid to fromPid.
// Legacy resource transfer only — typed item slots are not sent by client.
// Caller must hold G.mutex and have already verified sufficiency.
static void executeTrade(int fromPid, int toPid, const TradeOffer& offer) {
  Player& fr = G.players[fromPid];
  Player& to = G.players[toPid];
  // Legacy resource transfer
  for (int i = 0; i < 5; i++) {
    fr.inv[i] = (uint8_t)(fr.inv[i] - offer.give[i] + offer.want[i]);
    to.inv[i] = (uint8_t)(to.inv[i] - offer.want[i] + offer.give[i]);
  }
}

// Grants one random non-key item (qty 1) to the first free inventory slot.
// Call once for standard survivors, twice for Mule/Quartermaster.
static void grantRandomStartItem(Player& p) {
  uint8_t pool[MAX_ITEMS];
  int poolSize = 0;
  for (int i = 0; i < (int)itemCount; i++) {
    if (itemRegistry[i].id != 0 && itemRegistry[i].category != ITEM_KEY)
      pool[poolSize++] = itemRegistry[i].id;
  }
  if (poolSize == 0) return;
  uint8_t itemId = pool[esp_random() % poolSize];
  uint8_t slots = effectiveInvSlots(p);
  for (int s = 0; s < slots; s++) {
    if (p.invType[s] == 0) { p.invType[s] = itemId; p.invQty[s] = 1; return; }
  }
}

// ── Survivor (re)initialisation ───────────────────────────────────────────────
// Resets every gameplay field of a Player for archetype `arch`: vitals, skills,
// pack, equipment, wounds, MP, and the starting resource kit.  Position, name,
// score, steps, and connection fields are left to the caller.
//
// The starting kit must fit under the archetype's pack size, otherwise the
// survivor spawns encumbered and collectResource() refuses every pickup.
//   standard  : 2 water, 1 food, 1 fuel, 1 med, 1 scrap  = 6 of 8
//   Quarterm. : +1 food                                 = 7 of 8
//   Medic     : +1 med                                  = 7 of 8
//   Mule      : +1 food, +1 med, +1 scrap                = 9 of 12
static void resetSurvivor(Player& p, uint8_t arch) {
  if (arch >= NUM_ARCHETYPES) arch = 0;
  p.archetype = arch;
  p.invSlots  = ARCHETYPE_INV_SLOTS[arch];
  memcpy(p.skills, ARCHETYPE_SKILLS[arch], NUM_SKILLS);

  memset(p.inv, 0, sizeof(p.inv));
  p.inv[0] = 2; p.inv[1] = 1; p.inv[2] = 1; p.inv[3] = 1; p.inv[4] = 1;
  if (arch == 1) { p.inv[1]++; }
  if (arch == 2) { p.inv[3]++; }
  if (arch == 3) { p.inv[1]++; p.inv[3]++; p.inv[4]++; }

  memset(p.invType,     0, sizeof(p.invType));
  memset(p.invQty,      0, sizeof(p.invQty));
  memset(p.equip,       0, sizeof(p.equip));
  memset(p.surveyedMap, 0, sizeof(p.surveyedMap));
  memset(p.wounds,      0, sizeof(p.wounds));

  p.ll = 7; p.food = 6; p.water = 6; p.radiation = 0;
  p.llCapPenalty = 0;
  p.fThreshBelow = 0; p.wThreshBelow = 0;
  p.radClean  = true;
  p.resting   = false;
  p.lastMoveMs = 0;
  p.movesLeft = (int8_t)(p.ll + 3);  // == effectiveMP() for a fresh, unencumbered survivor
}

// Random passable spawn hex.  Prefers non-radioactive terrain; after 50 tries
// settles for merely passable.  Caller holds G.mutex.
static void pickSpawnHex(Player& p) {
  int attempts = 0;
  while (attempts < 50) {
    p.q = (int16_t)(esp_random() % MAP_COLS);
    p.r = (int16_t)(esp_random() % MAP_ROWS);
    attempts++;
    uint8_t st = G.map[p.r][p.q].terrain;
    if (TERRAIN_MC[st] != 255 && !TERRAIN_IS_RAD[st]) return;
  }
  while (TERRAIN_MC[G.map[p.r][p.q].terrain] == 255 && attempts < 200) {
    p.q = (int16_t)(esp_random() % MAP_COLS);
    p.r = (int16_t)(esp_random() % MAP_ROWS);
    attempts++;
  }
}

// Fresh weather for a new world: CLEAR for a random spell in the CLEAR duration
// range (game-days), no bad-weather streak.
static void resetWeather() {
  G.weatherPhase   = WEATHER_CLEAR;
  G.weatherCounter = WEATHER_DUR_MIN[WEATHER_CLEAR] +
    (uint16_t)(esp_random() % (WEATHER_DUR_MAX[WEATHER_CLEAR] - WEATHER_DUR_MIN[WEATHER_CLEAR] + 1));
  G.badWeatherTicks = 0;
}
