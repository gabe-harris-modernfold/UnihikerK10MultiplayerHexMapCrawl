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

// ── tokenRoomFor ──────────────────────────────────────────────────────────
// Spare resource-token capacity: the pack size in effect minus everything
// already carried, floored at 0.  FORAGE / WATER / SCAVENGE used to add their
// yield under nothing but a `min(..., 99)` guard, so the pack size bound
// collectResource() and the encumbrance penalty but not the three actions that
// produce the most tokens in the game -- which is most of why slot-granting
// gear felt like it did nothing.  Deliberately NOT applied to enc_bank: the
// haul tray is the player's own trim, and clamping it a second time would
// silently contradict what they just set.  Must hold G.mutex.
static int tokenRoomFor(const Player& p) {
  int used = 0;
  for (int k = 0; k < 5; k++) used += (int)p.inv[k];
  return max(0, (int)effectiveInvSlots(p) - used);
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
    PSRAM_STATIC(uint16_t, surveyed, [totalCells]);
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
static uint8_t applyDawnItemCosts(int pid) {
  Player& p = G.players[pid];
  uint8_t unfuelled = 0;
  // A survivor who went down during this dawn has had movesLeft zeroed and is
  // not going anywhere; charging their vehicle a fuel token and handing the
  // corpse +5 MP is both wrong and expensive.
  bool downed = (p.ll == 0);
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    uint8_t eid = p.equip[s];
    if (!eid) continue;
    const ItemDef* def = getItemDef(eid);
    if (!def) continue;
    bool hasCost = false;
    for (int k = 0; k < 5; k++) if (def->opCost[k]) { hasCost = true; break; }
    if (!hasCost) continue;
    // Check affordability
    bool canAfford = !downed;
    for (int k = 0; k < 5; k++) {
      if (def->opCost[k] > p.inv[k]) { canAfford = false; break; }
    }
    if (canAfford) {
      for (int k = 0; k < 5; k++) p.inv[k] -= def->opCost[k];
      // STAT_MP bonus is added to movesLeft here (set by dawnUpkeep BEFORE this call)
      p.movesLeft = (int8_t)min(127, (int)p.movesLeft + (int)def->statMods[STAT_MP]);
    } else if (!downed) {
      // Can't afford it: the item's STAT_MP bonus does NOT apply.  Other
      // statMods (LL, etc.) still show via effectiveMaxLL — game design
      // choice: equipment stays on, but fuel-gated bonuses are dormant.
      // Flagged so EVT_DAWN can say so; this used to be an empty else.
      unfuelled |= (uint8_t)(1 << s);
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
  return unfuelled;
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
//
// Every bound here is the pack size the swap *ends* with, never the one it
// started with: trading a slots-granting item (Hoarder's Rig, Backpack,
// Knife-Wrench) for one without shrinks the pack, and anything left past the
// new cap would be unreachable — every pack loop iterates 0..effectiveInvSlots.
// Same orphan guard unequipItem() below has, with one extra allowance: slotIdx
// is exempt, since the incoming item vacates it on its way to the equip slot.
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
  uint8_t prev  = p.equip[eslot];

  // Slot count once the swap is done — incoming item on, outgoing item off.
  p.equip[eslot]   = itemId;
  uint8_t newSlots = effectiveInvSlots(p);
  p.equip[eslot]   = prev;

  // Refuse if that would strand anything past the new cap.  Resource tokens
  // are deliberately NOT part of this test: a typed item past the cap is
  // unreachable (every pack loop stops at effectiveInvSlots), whereas tokens
  // over the cap are merely encumbering -- a legal, reachable state that
  // effectiveMP() already prices at -1 MP, and one enc_bank's haul tray can
  // put you in anyway.  Different failure modes, deliberately different rules.
  for (int i = newSlots; i < INV_SLOTS_MAX; i++) {
    if (i != (int)slotIdx && p.invType[i]) return false;
  }

  // Commit: the new item leaves the pack for the equipment slot.
  uint8_t qty0       = p.invQty[slotIdx];
  p.equip[eslot]     = itemId;
  p.invType[slotIdx] = 0;
  p.invQty[slotIdx]  = 0;

  // The old item takes the first free slot under the new cap — possibly the
  // one just vacated. If the shrunken pack has nowhere to put it, the swap
  // cannot happen at all: put everything back rather than drop it.
  if (prev) {
    bool placed = false;
    for (int i = 0; i < newSlots && !placed; i++) {
      if (!p.invType[i]) {
        p.invType[i] = prev; p.invQty[i] = 1;
        placed = true;
      }
    }
    if (!placed) {
      p.equip[eslot]     = prev;
      p.invType[slotIdx] = itemId;
      p.invQty[slotIdx]  = qty0;
      return false;
    }
  }

  // The swap can LOWER the LL ceiling -- both Dent Absorber (+2 LL) and the
  // Backpack are body slot, so trading one for the other drops the cap from 9
  // back to 7.  unequipItem() clamps for exactly this reason and equipItem()
  // did not, which left the survivor holding every point they had gained under
  // the old ceiling: the dawn heal clamps gains against effectiveMaxLL but the
  // loss path only ever decrements, so nothing took them back.  Equip the
  // armour, rest to 9, swap it out, keep the 9 -- repeatable, and it persisted
  // into the save.
  uint8_t newCap = effectiveMaxLL(pid);
  if (p.ll > newCap) p.ll = newCap;

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

// ── invRoomFor ────────────────────────────────────────────────────────────
// How many more of itemId the given slot arrays could hold within `slots`:
// spare capacity on every existing stack of itemId, plus a full stack per
// empty slot. The single source of truth for "does this fit" — addItemToInv
// places exactly what this promises, and applyRecipe dry-runs against it on
// a scratch copy of the pack, so the two can never disagree about a
// placement. Must hold G.mutex.
static int invRoomFor(const uint8_t* types, const uint8_t* qtys, uint8_t slots, uint8_t itemId) {
  const ItemDef* def = getItemDef(itemId);
  int maxStack = def ? def->maxStack : 1;
  int room = 0;
  for (int i = 0; i < slots; i++) {
    if (types[i] == itemId)  room += max(0, maxStack - (int)qtys[i]);
    else if (!types[i])      room += maxStack;
  }
  return room;
}

// ── addItemToInv ──────────────────────────────────────────────────────────
// Place up to `qty` of itemId into the player's pack: top up every existing
// stack with room first, then open new stacks in empty slots. Returns how
// many were actually placed (0..qty) — the caller is responsible for
// whatever didn't fit (ground pile, abort, etc). grantItemOrDrop() is built
// on this, so loot, pickups and crafting all fill the pack by one rule: a
// full stack never blocks placement while an empty slot is free. Must hold
// G.mutex.
static uint8_t addItemToInv(Player& p, uint8_t itemId, uint8_t qty) {
  if (!itemId || !qty) return 0;
  const ItemDef* def = getItemDef(itemId);
  int maxStack  = def ? def->maxStack : 1;
  uint8_t slots = effectiveInvSlots(p);
  int left = qty;
  for (int i = 0; i < slots && left > 0; i++) {
    if (p.invType[i] != itemId || (int)p.invQty[i] >= maxStack) continue;
    int add = min(left, maxStack - (int)p.invQty[i]);
    p.invQty[i] = (uint8_t)(p.invQty[i] + add);
    left -= add;
  }
  for (int i = 0; i < slots && left > 0; i++) {
    if (p.invType[i]) continue;
    int add = min(left, maxStack);
    p.invType[i] = itemId;
    p.invQty[i]  = (uint8_t)add;
    left -= add;
  }
  return (uint8_t)(qty - left);
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

  uint8_t canTake = addItemToInv(p, gi.itemType, gi.qty);
  if (!canTake) return false;

  gi.qty -= canTake;
  if (!gi.qty) { gi.itemType = 0; gi.q = 0; gi.r = 0; }

  return true;
}

// ── applyRecipe ───────────────────────────────────────────────────────────
// Consumes a known recipe's material items + resource tokens and grants its
// output item. Returns nullptr on success, else a short player-facing reason
// (static string) with the pack left untouched — every affordability check
// and the output-room check run on a scratch copy before any mutation, so a
// blocked craft never partially consumes materials. The room check looks at
// the pack AFTER the materials come out of it: a full pack whose last unit
// of a material occupies a slot can still take the result in that slot.
// Must hold G.mutex. Caller (doCraft in actions_game_loop.hpp) has already
// verified terrain, MP, and that the player has discovered this recipe.
static const char* applyRecipe(int pid, uint8_t recipeId) {
  Player& p = G.players[pid];
  const RecipeDef* r = getRecipeDef(recipeId);
  if (!r || !r->outputItem) return "No such recipe";

  for (int i = 0; i < 5; i++)
    if (r->resCost[i] > p.inv[i]) return "Not enough resources for that recipe";

  // Consume the materials on a scratch copy of the pack.
  uint8_t types[INV_SLOTS_MAX], qtys[INV_SLOTS_MAX];
  memcpy(types, p.invType, INV_SLOTS_MAX);
  memcpy(qtys,  p.invQty,  INV_SLOTS_MAX);
  for (int m = 0; m < RECIPE_MAX_MATS; m++) {
    if (!r->matItem[m]) continue;
    int need = (int)r->matQty[m];
    for (int s = 0; s < INV_SLOTS_MAX && need > 0; s++) {
      if (types[s] != r->matItem[m]) continue;
      int take = min(need, (int)qtys[s]);
      qtys[s] = (uint8_t)(qtys[s] - take);
      need -= take;
      if (!qtys[s]) types[s] = 0;
    }
    if (need > 0) return "Missing materials for that recipe";
  }

  if (invRoomFor(types, qtys, effectiveInvSlots(p), r->outputItem) < (int)r->outputQty)
    return "No room in your pack for the result";

  // Everything fits — commit the scratch pack and pay the tokens.
  for (int i = 0; i < 5; i++) p.inv[i] = (uint8_t)(p.inv[i] - r->resCost[i]);
  memcpy(p.invType, types, INV_SLOTS_MAX);
  memcpy(p.invQty,  qtys,  INV_SLOTS_MAX);
  addItemToInv(p, r->outputItem, r->outputQty);
  return nullptr;
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

// ── hasTunnelLight ─────────────────────────────────────────────────────────
// True when pid is CARRYING at least one Bile Flare -- the underground light
// source. Deliberately scans invType[] rather than equip[]: the flare is
// slot=none in items.cfg, so it can only ever sit in the pack, and "do I have
// a light on me" is the question the tunnels ask. Forward-declared in
// hex-map.hpp for playerVisParams(). Must hold G.mutex.
static bool hasTunnelLight(int pid) {
  const Player& p = G.players[pid];
  for (int s = 0; s < INV_SLOTS_MAX; s++)
    if (p.invType[s] == ITEM_BILE_FLARE && p.invQty[s] > 0) return true;
  return false;
}

// ── hasTireTracks ──────────────────────────────────────────────────────────
// Returns true if any equipped item on player pid leaves a tire track when
// moved (items.cfg "tracks" — the Motorbike today). Loops every slot like
// hasPassTerrainBit() rather than hardcoding EQUIP_VEHICLE, since nothing
// stops a future non-vehicle item from setting the flag too. Must hold G.mutex.
static bool hasTireTracks(int pid) {
  const Player& p = G.players[pid];
  for (int s = 0; s < EQUIP_SLOTS; s++) {
    if (!p.equip[s]) continue;
    const ItemDef* def = getItemDef(p.equip[s]);
    if (def && def->leavesTracks) return true;
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
  if (TERRAIN_HAS_WATER[t] && hasPassTerrainBit(pid, TERR_PASS_WATER)) {
    mc = RAFT_MC;  // raft: any water terrain crosses at the standard 1 MP
  } else if (mc == 255) {
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
  // Softened LL coupling.  This was `ll + 3`, one MP lost per point of LL,
  // which made the death spiral self-accelerating: losing health cost
  // mobility, and mobility is how you reach water, which is what was killing
  // people.  Bot runs measured the consequences — survivors spent only 2.4%
  // of the time at LL 4, passing straight through the wounded-but-coping band
  // rather than living in it, and brushes with death resolved into actual
  // deaths at a ratio of 1:1.1 instead of the intended handful of near misses.
  //
  // Halving the slope keeps a hurt survivor mobile enough to dig itself out
  // while leaving a healthy one exactly where it was:
  //
  //    LL   7   6   5   4   3   2   1
  //    was 10   9   8   7   6   5   4
  //    now 10   9   9   8   8   7   7
  //
  // One line to dial if it overshoots; docs/bot-testing.md covers measuring
  // the effect.
  int     mp = 6 + ((int)p.ll + 1) / 2;
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

// ── appendFmt ─────────────────────────────────────────────────────────────
// Bounded printf-append.  Returns the new offset, clamped to cap, so a chain
// of these can never run off the end or hand the next call a negative size.
// snprintf() returns what it WOULD have written, so `pos += snprintf(...)` --
// the idiom used throughout network-sync.hpp -- walks past cap on truncation
// and then passes a wrapped size_t to the next call.
static int appendFmt(char* buf, size_t cap, int pos, const char* fmt, ...) {
  if (pos < 0) pos = 0;
  if ((size_t)pos >= cap) return (int)cap;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + pos, cap - (size_t)pos, fmt, ap);
  va_end(ap);
  if (n < 0) return (int)cap;
  pos += n;
  return ((size_t)pos > cap) ? (int)cap : pos;
}

// ── appendPackArrays ──────────────────────────────────────────────────────
// Writes  "it":[...],"iq":[...],"eq":[...],"is":N,"llCap":N  at buf+pos and
// returns the new offset.  Every packet that carries a survivor's typed pack
// goes through here.
//
// It exists because INV_SLOTS_MAX used to be written out by hand as twelve
// "%d,%d,..." conversions in eleven different format strings.  Changing the
// constant meant finding all eleven; missing one shipped a JSON array of the
// wrong length, and the ack buffers were sized tight enough that the overflow
// would have been a silently truncated packet rather than a loud one.
//
// `is` is effectiveInvSlots(), not the archetype base: the client sizes its
// pack grid from it, and equipping a Backpack has to move that number in the
// same message that reports the equip.  `llCap` rides along for the same
// reason -- the LIFE LEVEL track is drawn to it, and nothing else tells the
// client the ceiling moved.  Must hold G.mutex.
static int appendPackArrays(char* buf, size_t cap, int pos, int pid) {
  const Player& p = G.players[pid];
  pos = appendFmt(buf, cap, pos, "\"it\":[");
  for (int i = 0; i < INV_SLOTS_MAX; i++)
    pos = appendFmt(buf, cap, pos, i ? ",%d" : "%d", (int)p.invType[i]);
  pos = appendFmt(buf, cap, pos, "],\"iq\":[");
  for (int i = 0; i < INV_SLOTS_MAX; i++)
    pos = appendFmt(buf, cap, pos, i ? ",%d" : "%d", (int)p.invQty[i]);
  pos = appendFmt(buf, cap, pos, "],\"eq\":[");
  for (int i = 0; i < EQUIP_SLOTS; i++)
    pos = appendFmt(buf, cap, pos, i ? ",%d" : "%d", (int)p.equip[i]);
  return appendFmt(buf, cap, pos, "],\"is\":%d,\"llCap\":%d",
                   (int)effectiveInvSlots(p), (int)effectiveMaxLL(pid));
}

// ── Trade helpers (call while holding G.mutex) ────────────────────────────────

// Returns true if both players are connected and standing on the same hex.
// Depth is part of "same hex": an underground survivor keeps q/r pinned to
// the hatch they descended through, so without it someone standing on that
// hatch could hand resources to a teammate several hundred metres below.
// Single source of truth for co-location -- both trade paths use it.
static bool samehex(int pidA, int pidB) {
  if (pidA < 0 || pidA >= MAX_PLAYERS || pidB < 0 || pidB >= MAX_PLAYERS) return false;
  Player& a = G.players[pidA];
  Player& b = G.players[pidB];
  if (!a.connected || !b.connected || a.depth != b.depth) return false;
  return a.depth ? (a.tq == b.tq && a.tr == b.tr) : (a.q == b.q && a.r == b.r);
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
  // Legacy resource transfer — clamp to 99 like collectResource() so a trade
  // can't push a stack past the display/parsing convention used everywhere
  // else (constrain(...,0,99) at offer time, min(...,99) on collection).
  for (int i = 0; i < 5; i++) {
    fr.inv[i] = (uint8_t)min((int)fr.inv[i] - offer.give[i] + offer.want[i], 99);
    to.inv[i] = (uint8_t)min((int)to.inv[i] - offer.want[i] + offer.give[i], 99);
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
  // Fresh survivor: the common know-how only, even if this slot held someone
  // else before. Everything past that is earned from encounters.
  p.knownRecipes = starterRecipeMask();

  p.ll = 7; p.food = 6; p.water = 6; p.radiation = 0;
  p.llCapPenalty = 0;
  p.fThreshBelow = 0; p.wThreshBelow = 0;
  p.radClean  = true;
  p.resting   = false;
  p.lastMoveMs = 0;
  // Back on the surface. This is the single hook that keeps regen and
  // eraseslot safe: a stale depth/tq/tr would index a tunnel board that the
  // new world just regenerated out from under it.
  p.depth = 0; p.tq = 0; p.tr = 0; p.hatchIdx = 0;
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

// Social spawn.  A survivor entering the world drops in beside a randomly
// chosen survivor who is already out there, so nobody starts the run alone on
// the far side of the map.  Falls back to pickSpawnHex() when this slot is the
// only one in play.  Caller holds G.mutex; `self` is skipped because
// handleMsg_pick marks the slot connected before calling.
static void pickSpawnNearPlayer(Player& p, uint8_t self) {
  uint8_t anchors[MAX_PLAYERS];
  int n = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i == (int)self) continue;
    // Downed survivors (ll 0) aren't a place to land — they're waiting to
    // respawn themselves, and their hex is wherever they fell.
    if (G.players[i].connected && G.players[i].ll > 0) anchors[n++] = (uint8_t)i;
  }
  if (n == 0) { pickSpawnHex(p); return; }

  const Player& host = G.players[anchors[esp_random() % n]];

  // Walk the 6 neighbours from a random start so successive joins fan out
  // around the host instead of always piling onto the same side.  Two passes:
  // prefer passable and non-radioactive, then settle for merely passable —
  // same preference order as pickSpawnHex().
  int start = (int)(esp_random() % 6);
  for (int pass = 0; pass < 2; pass++) {
    for (int k = 0; k < 6; k++) {
      int d  = (start + k) % 6;
      int nq = wrapQ((int)host.q + DQ[d]);
      int nr = wrapR((int)host.r + DR[d]);
      uint8_t st = G.map[nr][nq].terrain;
      if (TERRAIN_MC[st] == 255) continue;
      if (pass == 0 && TERRAIN_IS_RAD[st]) continue;
      p.q = (int16_t)nq; p.r = (int16_t)nr;
      return;
    }
  }
  // Ringed in by impassable terrain — share the host's own hex.
  p.q = host.q; p.r = host.r;
}

// Fresh weather for a new world: CLEAR for a random spell in the CLEAR duration
// range (game-days), no bad-weather streak.
static void resetWeather() {
  G.weatherPhase   = WEATHER_CLEAR;
  uint16_t durDays = 1 + WEATHER_DUR_MIN[WEATHER_CLEAR] +
    (uint16_t)(esp_random() % (WEATHER_DUR_MAX[WEATHER_CLEAR] - WEATHER_DUR_MIN[WEATHER_CLEAR] + 1));
  durDays = (durDays * 3 + 2) / 4;          // -25%, matches updateWeatherPhase()
  if (durDays < 1) durDays = 1;
  G.weatherCounter = durDays - 1;
  G.badWeatherTicks = 0;
}
