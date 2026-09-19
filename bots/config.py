"""Constants mirrored from the firmware.

Every value here has a counterpart in Esp32HexMapCrawl.ino or hex-map.hpp.
If the firmware changes, this file must follow -- nothing detects drift
automatically.  Sources are named per-block so a mismatch is easy to chase.
"""

# ── Map / session (Esp32HexMapCrawl.ino "Constants") ─────────────────────────
MAP_COLS    = 75
MAP_ROWS    = 57
MAX_PLAYERS = 6
TICK_MS     = 100
DAY_TICKS   = 3000      # TICK_MS * DAY_TICKS = 5 real minutes per game-day

# The map is a torus: moving off one edge wraps to the other (wrapQ/wrapR,
# hex-map.hpp:7).  Any pathing the policies do must wrap too.
def wrap_q(q: int) -> int: return q % MAP_COLS
def wrap_r(r: int) -> int: return r % MAP_ROWS

# ── Flat-top axial hex directions (Esp32HexMapCrawl.ino:464) ─────────────────
DQ = ( 1,  1,  0, -1, -1,  0)
DR = ( 0, -1, -1,  0,  1,  1)
NUM_DIRS = 6

def neighbor(q: int, r: int, d: int) -> tuple[int, int]:
    return wrap_q(q + DQ[d]), wrap_r(r + DR[d])

# ── Archetypes (Esp32HexMapCrawl.ino:399) ────────────────────────────────────
# NOTE: the archetype index IS the player slot -- handleMsg_pick does
# `Player& p = G.players[arch]`.  Bot N claims slot N.
ARCHETYPE_NAME = ("Guide", "Quartermaster", "Medic", "Mule", "Scout", "Endurer")

# ── Actions (ACT_* , Esp32HexMapCrawl.ino:186) ───────────────────────────────
ACT_FORAGE, ACT_WATER, ACT_TREAT, ACT_SCAV = 0, 1, 2, 3
ACT_SHELTER, ACT_CRAFT, ACT_SURVEY, ACT_REST = 4, 5, 6, 7
ACT_NAME = ("FORAGE", "WATER", "TREAT", "SCAV", "SHELTER", "CRAFT", "SURVEY", "REST")

# ── Skills (5-skill enum; do NOT reintroduce id 5 "Treat") ───────────────────
SK_NAVIGATE, SK_FORAGE, SK_SCAVENGE, SK_SHELTER, SK_ENDURE = range(5)
SK_NAME = ("NAVIGATE", "FORAGE", "SCAVENGE", "SHELTER", "ENDURE")

# ── Resource token indices into inv[5] ───────────────────────────────────────
RES_WATER, RES_FOOD, RES_FUEL, RES_MED, RES_SCRAP = range(5)
RES_NAME = ("water", "food", "fuel", "med", "scrap")

# ── Terrain (Esp32HexMapCrawl.ino:418; NUM_TERRAIN went 12 -> 16 with the
#    bunker tunnel terrains, so ids 12-15 are tunnel-only) ────────────────────
NUM_TERRAIN = 16
TERRAIN_NAME = ("Scrub", "Dunes", "Forest", "Marsh", "Urban", "Flooded",
                "Glass", "Hills", "Mountain", "Settlement", "Crater", "River",
                "Tunnel12", "Tunnel13", "Tunnel14", "Tunnel15")
# Move cost in MP.  255 == impassable (Crater, River, and one tunnel terrain).
# River is passable at 1 MP *with a Raft equipped* (TERR_PASS_WATER).
TERRAIN_MC = (1, 2, 2, 3, 2, 3, 3, 2, 4, 1, 255, 255, 1, 1, 2, 255)
IMPASSABLE = 255
TERR_SETTLEMENT = 9
TERR_RIVER      = 11

# Which terrain supports which action (Esp32HexMapCrawl.ino:254-264).
# FORAGE only works where the DN is non-zero: Scrub, Forest, Marsh, River.
TERRAIN_FORAGE_DN = (7, 0, 6, 8, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0)
# Drinkable water is genuinely scarce -- Marsh (26 hexes) and Flooded (175)
# on a 4275-cell map, plus River which needs a Raft to stand on.  Most water
# therefore has to come from collected piles, not the WATER action.
TERRAIN_HAS_WATER = (0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0)
TERRAIN_IS_RUINS  = (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)


def can_forage(terrain: int) -> bool:
    return 0 <= terrain < NUM_TERRAIN and TERRAIN_FORAGE_DN[terrain] != 0


def has_water(terrain: int) -> bool:
    return 0 <= terrain < NUM_TERRAIN and bool(TERRAIN_HAS_WATER[terrain])

# ── Weather phases (G.weatherPhase) ──────────────────────────────────────────
WEATHER_CLEAR, WEATHER_RAIN, WEATHER_STORM = 0, 1, 2
WEATHER_CHEM, WEATHER_FOG, WEATHER_MIST    = 3, 4, 5
WEATHER_NAME = ("clear", "rain", "storm", "chem", "strangle-fog", "mist")

# ── MP budget (effectiveMP, inventory_items.hpp:525) ─────────────────────────
# mp = ll + 3, minus major wounds, minus 1 if encumbered; floor of 2.
MP_FLOOR = 2
def base_mp(ll: int) -> int:
    return max(MP_FLOOR, ll + 3)

# ── Scoring (for the bots' own bookkeeping; server is authoritative) ─────────
SCORE_PER_RES_TOKEN   = 10   # collectResource(), automatic on stepping onto a pile
SCORE_EXPLORE_NEW_HEX = 1
SCORE_FORAGE_SUCCESS  = 3
SCORE_FORAGE_PARTIAL  = 1
SCORE_SHELTER_BASIC   = 4
SCORE_SHELTER_IMPROVED = 8
SCORE_SETTLEMENT_FOUND = 20
SCORE_ENC_PER_TOKEN   = 3
SCORE_ENC_FULL_CLEAR  = 10
