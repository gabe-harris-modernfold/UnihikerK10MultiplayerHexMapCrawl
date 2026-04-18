#pragma once
// ── Item system: effects, equipment, trading, ground items ────────────────────
// Included from Esp32HexMapCrawl.ino after survival_skills.hpp.
// Has access to all globals, constants, game_logic helpers, and structs.

// ═══════════════════════════════════════════════════════════════════════════
// ── Item System ──────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

// ── Item system strings ───────────────────────────────────────────────────
static const char* EQUIP_SLOT_NAMES[6] = {
  "None","Head","Body","Hand","Feet","Vehicle"
};

// ── Effect dispatch table ─────────────────────────────────────────────────
// Function signature: (pid, itemId, param)
typedef void (*EffectFn)(int pid, uint8_t itemId, uint8_t param);
static EffectFn effectTable[16] = {};  // indexed by EffectId enum

static void efxThreatMod(int pid, uint8_t itemId, uint8_t param) {
  // param is treated as signed int8_t: positive raises TC, negative lowers it.
  int delta = (int)(int8_t)param;
  G.threatClock = (uint8_t)constrain((int)G.threatClock + delta, 0, 20);
  Serial.printf("[ITEM]  P%d efxThreatMod item:%d TC%+d → %d\n",
    pid, itemId, delta, (int)G.threatClock);
}
static void efxCureStatus(int pid, uint8_t itemId, uint8_t param) {
  // param = bitmask of ST_* bits to clear
  G.players[pid].statusBits &= ~param;
  Serial.printf("[ITEM]  P%d efxCureStatus item:%d cleared 0x%02X\n", pid, itemId, (int)param);
}

// EFX_NARRATIVE — server-side handler for params requiring server action.
// param==10: teleport_random (Rambling Drifter, Slippery Cave)
// All other params (11=UI scramble, 12=reverse keys, 20-32=misc) are client-side;
// the server just broadcasts the item-use event with the param for the client to handle.
static void efxNarrative(int pid, uint8_t itemId, uint8_t param) {
  Serial.printf("[ITEM]  P%d efxNarrative item:%d param:%d\n", pid, itemId, (int)param);
  if (param == 10) {
    // teleport_random — move player to a random surveyed hex
    // surveyedMap bitmask: bit (r*MAP_COLS+q) => q = idx%MAP_COLS, r = idx/MAP_COLS
    static constexpr int totalCells = MAP_ROWS * MAP_COLS;
    uint16_t surveyed[totalCells];
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
      Serial.printf("[ITEM]  P%d teleported to (%d,%d)\n",
        pid, (int)G.players[pid].q, (int)G.players[pid].r);
    }
  }
}

static void initEffectTable() {
  effectTable[EFX_THREAT_MOD]  = efxThreatMod;
  effectTable[EFX_CURE_STATUS] = efxCureStatus;
  effectTable[EFX_NARRATIVE]   = efxNarrative;
}

static void dispatchEffect(int pid, const ItemDef& item) {
  if (item.effectId  && item.effectId  < 16 && effectTable[item.effectId])
    effectTable[item.effectId](pid, item.id, item.effectParam);
  if (item.effectId2 && item.effectId2 < 16 && effectTable[item.effectId2])
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
      Serial.printf("[ITEM]  P%d dawn cost paid for \"%s\" (item %d)\n",
        pid, def->name, (int)def->id);
      // STAT_MP bonus is added to movesLeft here (set by dawnUpkeep BEFORE this call)
      p.movesLeft = (int8_t)min(127, (int)p.movesLeft + (int)def->statMods[STAT_MP]);
    } else {
      Serial.printf("[ITEM]  P%d dawn cost NOT paid for \"%s\" — item inactive\n",
        pid, def->name);
      // If we can't afford the cost, the item's STAT_MP bonus does NOT apply.
      // Other statMods (LL, etc.) still show via calcEffectiveStat — game design
      // choice: equipment stays on, but fuel-gated bonuses are dormant.
    }
  }
}

// ── useItem ───────────────────────────────────────────────────────────────
// Use a consumable in inventory slot slotIdx. Applies statMods and dispatches
// effect. Returns true on success, false if slot empty / wrong category.
// Must hold G.mutex.
static bool useItem(int pid, uint8_t slotIdx) {
  if (slotIdx >= INV_SLOTS_MAX) return false;
  Player& p = G.players[pid];
  uint8_t itemId = p.invType[slotIdx];
  if (!itemId) return false;
  const ItemDef* def = getItemDef(itemId);
  if (!def) return false;
  // Equipment must be equipped, not used directly (use equipItem instead)
  if (def->category == ITEM_EQUIPMENT) return false;

  Serial.printf("[ITEM]  P%d use \"%s\" (slot %d qty %d)\n",
    pid, def->name, (int)slotIdx, (int)p.invQty[slotIdx]);

  // Apply stat modifiers
  if (def->statMods[STAT_LL])      p.ll        = (uint8_t)constrain((int)p.ll      + def->statMods[STAT_LL],      0, (int)effectiveMaxLL(pid));
  if (def->statMods[STAT_FOOD])    p.food      = (uint8_t)constrain((int)p.food    + def->statMods[STAT_FOOD],    1, 6);
  if (def->statMods[STAT_WATER])   p.water     = (uint8_t)constrain((int)p.water   + def->statMods[STAT_WATER],   1, 6);
  if (def->statMods[STAT_FATIGUE]) p.fatigue   = (uint8_t)constrain((int)p.fatigue + def->statMods[STAT_FATIGUE], 0, 8);
  if (def->statMods[STAT_RAD])     p.radiation = (uint8_t)constrain((int)p.radiation + def->statMods[STAT_RAD],   0, 10);
  if (def->statMods[STAT_RESOLVE]) p.resolve   = (uint8_t)constrain((int)p.resolve + def->statMods[STAT_RESOLVE], 0, 5);
  if (def->statMods[STAT_MP])      p.movesLeft = (int8_t)max(0, (int)p.movesLeft   + def->statMods[STAT_MP]);
  updateRadStatus(p);

  // Dispatch effects
  dispatchEffect(pid, *def);

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
    for (int i = 0; i < INV_SLOTS_MAX && !placed; i++) {
      if (!p.invType[i]) {
        p.invType[i] = prev; p.invQty[i] = 1;
        placed = true;
      }
    }
    if (!placed) {
      Serial.printf("[ITEM]  P%d equip \"%s\" BLOCKED — no inv space for displaced item\n",
        pid, def->name);
      return false;
    }
  }

  // Place new item in equipment slot; clear inventory slot
  p.equip[eslot]     = itemId;
  p.invType[slotIdx] = 0;
  p.invQty[slotIdx]  = 0;

  Serial.printf("[ITEM]  P%d equipped \"%s\" → slot %s\n",
    pid, def->name, EQUIP_SLOT_NAMES[def->equipSlot]);
  return true;
}

// ── unequipItem ───────────────────────────────────────────────────────────
// Remove item from equipment slot (0-indexed eslot, 0=HEAD..4=VEHICLE).
// Moves it to the first free inventory slot. Returns true on success.
// Must hold G.mutex.
static bool unequipItem(int pid, uint8_t eslot) {
  if (eslot >= EQUIP_SLOTS) return false;
  Player& p = G.players[pid];
  uint8_t itemId = p.equip[eslot];
  if (!itemId) return false;
  // Find free inv slot
  for (int i = 0; i < INV_SLOTS_MAX; i++) {
    if (!p.invType[i]) {
      p.invType[i] = itemId; p.invQty[i] = 1;
      p.equip[eslot] = 0;
      // Clamp LL to new effective ceiling now that the item is no longer equipped
      uint8_t newCap = effectiveMaxLL(pid);
      if (p.ll > newCap) p.ll = newCap;
      const ItemDef* def = getItemDef(itemId);
      Serial.printf("[ITEM]  P%d unequipped \"%s\" from slot %d (LL cap now %d)\n",
        pid, def ? def->name : "?", (int)eslot, (int)newCap);
      return true;
    }
  }
  Serial.printf("[ITEM]  P%d unequip BLOCKED — inventory full\n", pid);
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
    Serial.println("[ITEM]  dropItem BLOCKED — MAX_GROUND reached");
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

  const ItemDef* def = getItemDef(itemId);
  Serial.printf("[ITEM]  P%d dropped \"%s\" x%d at (%d,%d) gslot:%d\n",
    pid, def ? def->name : "?", (int)qty, (int)p.q, (int)p.r, gslot);
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
  for (int i = 0; i < INV_SLOTS_MAX; i++) {
    if (p.invType[i] == itemId && stackSlot < 0) stackSlot = i;
    if (!p.invType[i] && freeSlot < 0) freeSlot = i;
  }
  int targetSlot = (stackSlot >= 0) ? stackSlot : freeSlot;
  if (targetSlot < 0) {
    Serial.printf("[ITEM]  P%d pickup BLOCKED — inventory full\n", pid);
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
    Serial.printf("[ITEM]  P%d pickup BLOCKED — stack full\n", pid);
    return false;
  }

  p.invType[targetSlot] = itemId;
  p.invQty[targetSlot]  = (uint8_t)min((int)maxStack, (int)p.invQty[targetSlot] + (int)canTake);
  gi.qty -= canTake;
  if (!gi.qty) { gi.itemType = 0; gi.q = 0; gi.r = 0; }

  Serial.printf("[ITEM]  P%d picked up \"%s\" x%d from (%d,%d)\n",
    pid, def ? def->name : "?", (int)canTake, (int)p.q, (int)p.r);
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

// ═══════════════════════════════════════════════════════════════════════════

// Effective Movement Points = LL − major-wound penalty (max 2) − encumbrance penalty
//   + STAT_MP from equipment (items with no opCost always active;
//     fuel-gated items like motorbike have STAT_MP applied in applyDawnItemCosts).
// Called while holding G.mutex.
static int effectiveMP(int pid) {
  Player& p  = G.players[pid];
  int     mp = (int)p.ll + 3;
  mp -= min((int)p.wounds[1], 2);          // major wounds: −1 each, max −2
  // Encumbrance: count legacy inv slots used
  int used = 0;
  for (int k = 0; k < 5; k++) used += (int)p.inv[k];
  // Effective slot cap includes STAT_SLOTS equipment bonus
  int effSlots = (int)p.invSlots;
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (def) effSlots += (int)def->statMods[STAT_SLOTS];
  }
  if (used > effSlots) mp--;               // encumbrance
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
// Returns the player's current LL ceiling, including STAT_LL bonuses from all
// equipped items (e.g. Body Armor +2 → ceiling 9 instead of 7).
// Must hold G.mutex.
static uint8_t effectiveMaxLL(int pid) {
  int cap = 7;
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
  for (int s = 0; s < p.invSlots; s++) {
    if (p.invType[s] == 0) { p.invType[s] = itemId; p.invQty[s] = 1; return; }
  }
}
