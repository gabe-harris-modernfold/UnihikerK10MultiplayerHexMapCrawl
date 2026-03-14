# WASTELAND CRAWL — Implementation Notes for Agents

## Current Session: Pacing & Design Fixes

This document tracks **design changes made to fix critical pacing and UX issues**. Future agents should reference this when modifying the game loop or action system.

---

## Issues Fixed (Session: 2026-03-14)

### 1. MP Starvation Destroys Pacing
**Problem:** At LL:2, effective MP was 1 (or 0 with wounds), making the game unplayable — players were stuck moving 1 hex/day and forced into endless REST loops.

**Solution:** Added `max(2, mp)` floor in `effectiveMP()` (game_logic.hpp:96).
- Even at worst LL, players can now move 2 hexes/day minimum
- Preserves LL penalty (still costs MP), but prevents death spiral
- Maintains tension while keeping the core loop viable

**Files:** `game_logic.hpp` line 96

**Testing:** Verify that a player at LL:2 with no wounds can still execute 2 MP of actions (e.g., move 2 hexes, or WATER+move 1 hex).

---

### 2. Exploration Bonus Missing
**Problem:** Players got no reward for visiting new hexes — only resource collection. This made exploration feel like a cost, not a discovery.

**Solution:** Added +1 score per first-visit hex.
- Server checks `footprints` bitmask in `movePlayer()` (game_logic.hpp:~330)
- If player hasn't visited hex before, award +1 score and flag it in EVT_MOVE
- Client logs: "★ New hex — +1 exploration pt"

**Files:**
- `Esp32HexMapCrawl.ino`: Add `int8_t exploD` field to `GameEvent` struct
- `game_logic.hpp` line ~330: Check first-visit and award score
- `network.hpp`: Broadcast `exploD` in EVT_MOVE JSON
- `data/engine.js` line ~350: Handle `ev.exploD` in `mv` case

**Design Note:** Exploration bonus stacks with any resources on the hex. Finding a new hex with loot is extra rewarding.

---

### 3. SURVEY Degenerate Loop
**Problem:** SURVEY was repeatable infinitely on the same hex for +2 pts/day, making it a dominant strategy that sidelined actual exploration.

**Solution:** Added per-hex SURVEY cap — each hex can only be surveyed once per player.
- Server stores `surveyedMap[]` bitset in Player struct (60 bytes per player)
- In `handleAction()` ACT_SURVEY case: check if hex already surveyed; if yes, reveal cells but award 0 pts
- Persists across reboots (added to SavePlayer)

**Files:**
- `Esp32HexMapCrawl.ino` Player struct: Add `uint8_t surveyedMap[(MAP_ROWS * MAP_COLS + 7) / 8]`
- `Esp32HexMapCrawl.ino` SavePlayer struct: Add same field
- `game_logic.hpp` line ~635: Check and set survey bit in handleAction

**Design Note:** SURVEY still reveals terrain at the ring, so it's useful for repeated use in hub areas. But points only reward first discovery, restoring exploration incentive.

---

### 4. Starting Resource Scarcity
**Problem:** Players started with F:6, W:6 and inv=0, giving them 1-2 safe days before scarcity. No early pressure.

**Solution:** Sparse starting inventory to create immediate tension.
- All players: `inv[0] = 2` water, `inv[1] = 1` food
- Mule only: `inv[0] = 2`, `inv[1] = 2`, `inv[3] = 1` (medicine), `inv[4] = 1` (scrap)
- F/W tracks still start at 6 (representing physical health, not tokens)
- This means: by Dawn 2, first threshold crossings happen if players don't forage/collect

**Files:**
- `Esp32HexMapCrawl.ino` line ~1255-1261: Player init loop with archetype conditional

**Design Note:** The 1 food / 2 water starting amounts represent basic rations. Dawn 1 they're consumed; Dawn 2 without resupply triggers LL loss. This creates a "bootstrap" problem that incentivizes movement to water sources early.

---

### 5. BUILD SHELTER UX
**Problem:** Button showed as available but silently failed with "NOT AVAILABLE" due to stale scrap/MP state, or because actual MP cost (1–2 based on scrap) didn't match button label.

**Solution:** Dynamic MP cost computation + early shelter-max warning.
- In `ui.js` action panel (line ~710–780): Compute actual `shelterMpCost = scrap >= 2 ? 2 : 1`
- Use that for both button label and `hasMP` check
- If `shelter >= 2` (improved already built): grey out with reason "Max shelter built here"
- If scrap=0: show "Needs scrap"

**Files:**
- `data/ui.js` line ~710-780: Compute shelter vars, update blockReason logic

**Design Note:** This makes the UX clear: "If I have 2+ scrap and 2 MP, I can upgrade to improved shelter here." No guessing.

---

### 6. Action Menu at MP=0
**Problem:** When MP:0, menu still showed all actions greyed out, creating noise and suggesting actions might work if you had MP.

**Solution:** Suppress action menu entirely when MP=0; only show REST.
- In action panel rebuild function (ui.js ~line 707): If `mp === 0`, replace action list with banner: "EXHAUSTED — REST to recover MP"
- REST button (below panel) is still available

**Files:**
- `data/ui.js` line ~707-730: Add `if (mp === 0)` early exit

**Design Note:** This removes friction and clarifies the only viable choice. The pacing works better when REST is obviously the only option at 0 MP.

---

## Architecture Notes

### MP (Movement Points) System
```
effectiveMP(pid) = max(2, LL - major_wounds*2 - encumbrance)
```

- **Floor of 2:** Preserves gameplay viability even at worst LL
- **LL loss:** Food/Water threshold crossings (F<4, F<2, W<5, W<3, W<1)
- **Action costs:** FORAGE/SCAVENGE 2 MP, WATER/SURVEY 1 MP, REST 0 MP, SHELTER 1–2 MP (dynamic)
- **Movement:** Each hex costs 1–2 MP depending on terrain

### Score System
```
Scoring sources:
- Exploration: +1 per first-visit hex
- FORAGE: +3 success, +1 partial
- WATER: +spend (1–3 pts)
- SCAVENGE: +5 success, +2 partial
- SHELTER: +4 basic, +8 improved
- TREAT: +3 success (varies by condition)
- SURVEY: +2 first survey, +0 repeated
```

All scoring now explicitly logged in event. Clients see scores in the action narrator.

### Surveyed Hexes (per-player)
```
Player.surveyedMap[] = bitset[ (MAP_ROWS * MAP_COLS + 7) / 8 ]
  = 475 cells / 8 bits per byte = 60 bytes per player

Hex index from (q, r): idx = r * MAP_COLS + q = r*25 + q
Bit manipulation: surveyedMap[idx/8] |= (1 << (idx%8))
```

This allows instant lookup without a Set or Map, crucial for performance on ESP32.

---

## Future Work / Known Limitations

### Short-term
- [ ] Test MP floor doesn't trivialize the game (feedback from playtesting)
- [ ] Verify exploration bonus pacing (should encourage at least 1–2 new hexes/day)
- [ ] SURVEY cap: confirm 1 survey per hex per player is fair (no griping about "waste")
- [ ] Score display: ensure all sources are logged clearly in the narrator

### Medium-term
- [ ] Exploration bonus scaling: consider terrain-based multipliers (rare terrain = +2 pts)
- [ ] SURVEY cooldown alternative: instead of per-hex cap, allow 2–3 repeats before penalty
- [ ] Threat Clock: currently unused; consider tying it to Encounter deck generation
- [ ] Encounter system: implement hazards/obstacles that trigger during SCAVENGE in Ruins

### Long-term
- [ ] Contamination token system (§6.5): COLLECT WATER on polluted terrain gives contaminated water
- [ ] Purify action: implement using TREAT/medicine to clean water
- [ ] Item tracking grid: expand from inv[5] to 12 slots with per-item metadata
- [ ] Cooperative rescue: downed players can be revived by teammates (currently instant reset)

---

## Testing Checklist

**Before committing changes:**
- [ ] Starting resources: Confirm Mule gets 1 scrap + 1 medicine; others get 0
- [ ] MP floor: Player at LL:2 should show mp:2 in EVT_DAWN
- [ ] Exploration bonus: New hex visited → +1 score visible in log; repeated hex → no bonus
- [ ] SURVEY cap: First survey of hex → +2 pts; second survey same hex same day → +0 pts
- [ ] Shelter cost: Button shows dynamic cost (1 or 2 MP); label matches actual cost
- [ ] Action menu: At mp:0, only REST visible; at mp:1+, full menu shows

**Load test (multiple players):**
- 6 players, day cycle, verify no double-counting of exploration scores
- Move to same hex simultaneously; confirm each gets +1 bonus independently
- Verify surveyed bitmask doesn't corrupt under concurrent access (Server holds G.mutex)

---

## Code Style & Conventions

**Game state mutations:** Always hold `G.mutex` before reading/writing Player or HexCell.

**Event enqueuing:** Use `enqEvt(ev)` for all game state changes visible to clients. Never broadcast directly to WebSocket.

**Bitwise operations:** Use `MAX_PLAYERS=6` and `(1 << pid)` for bitmasks. Ensure pid is validated.

**Hex math:** Use `wrapQ()` and `wrapR()` for toroidal wrap-around. Never assume hex at (q, r) is in bounds.

---

## References

- **Memory:** Saved in `/memory/` for cross-session context
- **Terrain System:** 11 types, index 0–10; see `T_NAME[]`, `TERRAIN_*[]` arrays
- **Archetype Skills:** 6 types (0=Guide, 1=QM, 2=Medic, 3=Mule, 4=Scout, 5=Endurer); see `ARCHETYPE_SKILLS[][]`
- **WebSocket Protocol:** JSON events (EVT_MOVE, EVT_ACTION, etc.); see `network.hpp` for full list
