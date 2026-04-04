# Weather System Specification

**Version:** 1.0
**Status:** Design Complete
**Last Updated:** 2026-04-04

---

## Executive Summary

Implement a global weather phase system with per-hex terrain-based intensity modulation. Weather progresses through states (clear → light rain → storm → chem-storm) affecting visibility, movement cost, and health. Rendered via per-hex opacity overlays + lightweight particle system. Chem-storms are a real threat—they cause life loss.

---

## Game Design

### Weather Phases

| Phase | Duration | Color Palette | Mechanics | Threat |
|-------|----------|---------------|-----------|--------|
| **Clear** | 60-100 ticks | None | Baseline visibility, normal movement | Low |
| **Light Rain** | 30-50 ticks | Blue-gray overlay | -2 visibility, +1 MC | Low-Medium |
| **Storm** | 20-40 ticks | Dark gray + lightning | -4 visibility, +2 MC, lightning flashes | Medium |
| **Chem-Storm** | 15-30 ticks | Sickly green glow | -6 visibility, +3 MC, life loss, radiation pulses | High |

### Transition Logic

Server-side state machine (tunable probabilities):

```
clear ──70%──> light_rain ──60%──> storm ──30%──> chem_storm
  ↑             ↑                    ↓                ↓
  └─────40%─────┘                 60% to rain      50% to storm
                                   40% to clear      50% to rain
```

---

## Technical Architecture

### State Structure

**GameState additions** (3 bytes):
```cpp
uint8_t  weatherPhase;    // 0=clear, 1=light_rain, 2=storm, 3=chem_storm
uint16_t weatherCounter;  // ticks in current phase
```

No per-hex weather state. Terrain provides intensity modulation via lookup table.

### Terrain-Based Intensity Modulation

Each terrain type has an intensity multiplier (0.0-1.0) for each weather phase. Higher values = more intense effect.

**Examples:**
- Mountains in storm: 1.0 (full intensity, exposed peak)
- Settlement in light rain: 0.1 (roofed, sheltered)
- Open scrub in chem-storm: 0.95 (fully exposed)
- Urban in chem-storm: 0.3 (bunkers, sealed buildings)

Rendering formula:
```
finalIntensity = basePhaseIntensity × terrainModifier[phase][terrain]
```

---

## Rendering

### Rendering Pipeline

Insert **Pass 2.5** (new) between character icons (Pass 2) and ash particles (Pass 3):

1. **Pass 1:** Hex terrain + resources
2. **Pass 1b:** Grid + POI outlines
3. **Pass 2:** Character icons with shadows
4. **→ Pass 2.5 (NEW): Weather overlays + lightweight particles ←**
5. **Pass 3:** Ash particles + time-of-day tint + night fade

### Weather Visual Effects

#### Light Rain
- Semi-transparent blue-gray wash over hex
- 3 animated diagonal streaks (45° angle, scrolling)
- Gentle particle spawning (2 particles/frame)

#### Storm
- Dark gray overlay
- 5 heavy rain streaks (heavier animation)
- **Lightning animation:**
  - 0-100ms: Bright white flash (intensity 0.6)
  - 100-150ms: Dark period (intensity 0.8, very dark)
  - 150-200ms: Secondary flash (intensity 0.4, dimmer)
  - 200-3000ms: Darkness
  - Repeats every 3 seconds
- Heavy particle spawning (4 particles/frame)

#### Chem-Storm
- Sickly green radial gradient glow
- **Radiation pulse lighting:**
  - 0-100ms: Pulse #1 (intensity 0.7)
  - 100-150ms: Dim (intensity 0.3)
  - 150-220ms: Pulse #2 (intensity 0.6)
  - 220-2500ms: Long dark period with faint baseline flicker
  - 2500-2650ms: Intense surge (intensity 0.8)
  - 2650-4000ms: Darkness
  - Repeats every 4 seconds
- Optional: Outer ring glow expands during high-intensity pulses
- Chem particle spawning (6 particles/frame)

### Lightweight Particle System

**Purpose:** Weather particles (rain drops, chem mist, fog wisps)

**Constraints:**
- Maximum 300 particles (per-frame culling if exceeded)
- Optimized for high density (simple structure)
- Fade in/out smoothly (10-frame fade, 120-frame TTL)

**Particle Properties:**
- Position (x, y)
- Velocity (vx, vy) - drift + fall
- Age & TTL (time to live)
- Opacity (dynamic fade)
- Size (1-2 pixels)

**Rendering:**
- Rain particles: Vertical lines (1 pixel wide, 10 pixels tall)
- Chem particles: Small circles (1.5-2.5px radius, greenish)

---

## Game Mechanics

### Visibility Impact

Lookup table: `VISIBILITY_PENALTY[weatherPhase]`

```
Phase 0 (clear):       0 penalty
Phase 1 (light_rain):  -2 vision range
Phase 2 (storm):       -4 vision range
Phase 3 (chem_storm):  -6 vision range
```

Applied in client's `calculateVisibilityRange()`. E.g., if scout = 5 and storm active, effective range = 1.

### Movement Cost Increase

Lookup table: `WEATHER_MC_PENALTY[weatherPhase]`

```
Phase 0 (clear):       +0
Phase 1 (light_rain):  +1
Phase 2 (storm):       +2
Phase 3 (chem_storm):  +3
```

Applied in server's movement cost calc. Total MC = terrain base + weather penalty.

### Hazard Damage

**Storm (Phase 2):**
- No automatic damage; lightning is purely visual (future feature: optional "lightning strike" encounter)

**Chem-Storm (Phase 3) — REAL THREAT:**
- Per-tick probability: `hazardChance = terrainIntensity[3][terrain] × 0.3` (max 30% per tick)
- On trigger: Player loses 1 life
- Broadcast damage event (shows to other players)
- Terrain modulation: Open terrain (0.95 intensity) = 28.5% damage/tick; settlement (0.3 intensity) = 9% damage/tick

**Mechanics implication:** Players must flee chem-storms or find shelter (settlements, forests, rivers). Movement cost + hazard create real urgency.

---

## Network Synchronization

### Server → Client

**Main sync message (existing):**
```json
{"t":"sync","id":pid,"tk":tickId,"vr":visR,"map":"...","p":[...]}
```

**Extended sync message (add one field):**
```json
{"t":"sync","id":pid,"tk":tickId,"vr":visR,"wp":weatherPhase,"map":"...","p":[...]}
```

**Weather phase change event (broadcast):**
```json
{"t":"ev","k":"weather","phase":2,"ticks":0}
```

### Save/Load Persistence

weatherPhase and weatherCounter are added to GameState and must also be added to `SaveHeader` — see Architectural Note #1. Persisted on dawn save, loaded on restore. `SAVE_VERSION` bump required.

---

## Architectural Notes

These notes reflect gaps and constraints discovered by cross-referencing the spec against the actual codebase. Address them during Phase 1 before any integration work begins.

### 1. Persistence — spec claim is incorrect

The spec states *"No additional persistence logic needed."* This is wrong.

`SaveHeader` (`Esp32HexMapCrawl.ino:504`) currently holds `{magic, version, dayCount, threatClock, pad}` — neither weather field is persisted. Required changes:
- Repurpose `pad` → `weatherPhase` (uint8_t, same header size)
- Append `weatherCounter` (uint16_t, +2 bytes to header)
- Bump `SAVE_VERSION` from `6` → `7`

The version check in `tryLoadSave()` already handles mismatches cleanly (logs "Version mismatch — fresh start"), so bumping the version is safe.

### 2. Weather penalty belongs in `playerVisParams()`, not just the client

The spec assigns the visibility penalty to the client's `calculateVisibilityRange()`. However, `playerVisParams()` is authoritative server-side and its output feeds:
- `encodeMapFog()` in `sendSync()` — **controls which cells are revealed in the fog-of-war map**
- `buildVisDisk()` post-move — determines the vision disk sent after each move
- `buildSurveyDisk()` — survey radius
- Event range-gating in `drainEvents()`

If `playerVisParams()` does not incorporate the weather penalty, the server-encoded fog mask will not shrink during storms. Clients will see terrain they shouldn't. The penalty must be applied here: `effectiveVisR = max(0, baseVisR - WEATHER_VISIBILITY_PENALTY[G.weatherPhase])`.

### 3. `broadcastState()` should also carry `wp`

The spec adds `"wp"` to the initial `sendSync()` message only. The 100ms `broadcastState()` does not include it. A client that was connected before a phase transition will hold a stale phase until the weather event fires. Including `"wp"` in the periodic broadcast's `"gs"` object costs ~6 bytes and makes the channel self-healing.

### 4. Visibility floor

`VISION_R = 3`. Chem-storm penalty = -6. Raw result = -3. The code in `network-handlers.hpp:343` already maps extreme terrain visibility levels to `visR = 0`, but no equivalent clamp is specified for the weather offset. The formula must be `max(0, visR - penalty)` to avoid unsigned underflow.

### 5. Dual-side intensity table must stay in sync

`WEATHER_INTENSITY[phase][terrain]` is needed in two places:
- **C++** — `processCemStormHazard()` damage probability
- **JS** — rendering overlay opacity and chem glow scaling

These tables must be byte-for-byte identical. Add a cross-reference comment in each file pointing at the other. Any balance change requires updating both.

### 6. Chem-storm damage rate is aggressive by design

At `CHEM_HAZARD_PROBABILITY = 0.3` per tick × 10 ticks/sec:
- Open scrub (intensity 0.95): ~2.85 expected HP/sec
- Settlement (intensity 0.3): ~0.9 expected HP/sec

With typical `ll` of 3–5, open-terrain exposure kills in 1–2 seconds. This is intentional — it enforces the "flee or die" pressure. Do not reduce this during initial tuning without considering the design intent.

### 7. `movesLeft` deduction must include weather MC server-side

The movement cost increase is described as client-visible, but `movePlayer()` in `survival_state.hpp` is what actually deducts from `p.movesLeft`. The server-side MC calculation must add `WEATHER_MC_PENALTY[G.weatherPhase]` or players can move further than the spec allows even if the client grays out buttons correctly.

---

## Implementation Phases

### Phase 1: State & Server Logic (1-2 hours)
- [ ] Add weatherPhase, weatherCounter to GameState struct
- [ ] Implement `updateWeatherPhase()` state machine
- [ ] Call in game loop (every tick or at fixed intervals)
- [ ] Extend `playerVisParams()` to subtract weather visibility penalty (clamped to 0)
- [ ] Extend network sync message (add "wp" field to both `sendSync()` and `broadcastState()`)
- [ ] Extend `SaveHeader`: repurpose `pad` → `weatherPhase`, append `weatherCounter`, bump `SAVE_VERSION` 6 → 7
- [ ] Verify save/load (old saves trigger fresh-start cleanly)

**Files:** `Esp32HexMapCrawl.ino`, `network-sync.hpp`, `survival_state.hpp`

### Phase 2: Client Mechanics (1-2 hours)
- [ ] Add WEATHER_INTENSITY lookup table (all 12 terrains × 3 phases)
- [ ] Implement `calculateVisibilityRange()` with visibility penalty
- [ ] Add WEATHER_MC_PENALTY to movement cost
- [ ] Test: visibility reduces, movement slows

**Files:** `data/game-data.js`, `data/engine.js`, `Esp32HexMapCrawl.ino`

### Phase 3: Lightweight Particle System (2-3 hours)
- [ ] Create `data/weather-particles.js` with `WeatherParticle` class + `WeatherParticleSystem`
- [ ] Implement spawn, update, render, TTL culling
- [ ] Integrate into main RAF loop
- [ ] Test particle density + performance

**Files:** `data/weather-particles.js` (NEW), `data/engine.js`

### Phase 4: Rendering Pass 2.5 (2-3 hours)
- [ ] Implement `renderWeatherPass()` for all phases
- [ ] Implement `renderRainOverlay()`, `renderStormOverlay()`, `renderChemOverlay()`
- [ ] Insert Pass 2.5 into main render pipeline
- [ ] Integrate lighting animations (lightning cycle, radiation pulses)
- [ ] Visual testing: opacity, colors, animations smooth

**Files:** `data/engine.js`

### Phase 5: Hazards + Polish (2 hours)
- [ ] Implement `processChemStormHazard()` in turn processing
- [ ] Test life loss mechanics
- [ ] Tune all parameters: phase durations, intensities, particle spawn rates, damage values
- [ ] Audio: ambient weather loop (optional)
- [ ] Playtest end-to-end

**Files:** `Esp32HexMapCrawl.ino`, all rendering files

**Total estimated time:** 8-12 hours across 5 phases

---

## Configuration & Tuning

All values are server-side configurable without code changes (via defines or structs):

```cpp
#define PHASE_DURATION_CLEAR      80    // ticks
#define PHASE_DURATION_LIGHT_RAIN 40
#define PHASE_DURATION_STORM      35
#define PHASE_DURATION_CHEM       25

#define CHEM_HAZARD_PROBABILITY   0.3f  // max 30% per tick
#define CHEM_HAZARD_DAMAGE        1     // life per trigger
```

---

## Testing Checklist

### Functional
- [ ] Weather phase transitions at expected intervals
- [ ] Visibility reduces correctly for each phase
- [ ] Movement cost increases correctly
- [ ] Chem-storm causes life loss (probability-based)
- [ ] Save/load preserves weather state

### Visual
- [ ] Rain overlay visible, opacity correct
- [ ] Storm overlay + lightning flashes (double strike pattern)
- [ ] Chem overlay + radiation pulses (irregular timing)
- [ ] Particles spawn, drift, fade smoothly
- [ ] Per-hex intensity modulation (mountains darker than settlements)
- [ ] 60fps on K10 display

### Gameplay
- [ ] Players feel threatened by chem-storms
- [ ] Terrain modulation incentivizes shelter-seeking
- [ ] Weather transitions feel natural
- [ ] UI updates reflect visibility/threat correctly

---

## Known Limitations / Future Work

**Not in v1:**
- Per-hex weather variation (procedural fronts)
- Weather-triggered encounters
- Chem-storm zones that persist/drift
- Sound effects (rain loop, Geiger counter)
- Weather forecasting mechanic

**Possible v2 features:**
- Chem-storm proximity to craters (bias generation)
- Shelter mechanic (hexes with roof blocks weather hazard)
- Seasonal weather patterns
- Dynamic weather that reacts to player presence

---

## References

- **Encounter Engine Spec:** `docs/encounter-engine-spec.md`
- **Game Logic:** `Esp32HexMapCrawl.ino`, `game-server.hpp`
- **Rendering:** `data/engine.js` (Pass 1-4 pipeline documented inline)
- **Network Protocol:** `network-sync.hpp`
