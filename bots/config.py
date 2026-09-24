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
# Entering these rolls Endure DN6 or +1 radiation (movePlayer), and they keep
# the day from being "clean" for the dawn R-1 -- Ash Dunes, Glass, Crater.
TERRAIN_IS_RAD    = (0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0)
# Shelter value (Esp32HexMapCrawl.ino:430).  dawnUpkeep's §7.3 exposure check
# costs 1 LL per dawn wherever SV < 2 and nothing is built -- which is every
# terrain except Flooded, Mountain, Settlement and Bunker, i.e. under 4% of a
# generated map.  It is the single largest LL drain in the game, and the only
# counter-play on the other 96% is building a shelter (1 scrap, 1 MP).
TERRAIN_SV = (0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 0, 2, 0, 1, 0)


def can_forage(terrain: int) -> bool:
    return 0 <= terrain < NUM_TERRAIN and TERRAIN_FORAGE_DN[terrain] != 0


def has_water(terrain: int) -> bool:
    return 0 <= terrain < NUM_TERRAIN and bool(TERRAIN_HAS_WATER[terrain])


def is_exposed(terrain: int) -> bool:
    """True where sleeping unsheltered costs EXPOSURE_BITE LL at dawn
    (survival_state.hpp §7.3).  The Endurer archetype and a Bear Skin Cape are
    both immune, and so is anyone underground -- dawnUpkeep() treats depth != 0
    as cover outright.  All three are the caller's to account for; this is
    terrain only, and it is a *surface* terrain question."""
    return 0 <= terrain < NUM_TERRAIN and TERRAIN_SV[terrain] < 2

# ── Bunker tunnels (tunnels.hpp; the sizes live in Esp32HexMapCrawl.ino) ─────
# A second, much smaller hex board reached by stepping onto a hatch.  Two
# things about it break assumptions the surface code is allowed to make:
#
#   1. It does NOT wrap.  wrapQ/wrapR are hardcoded to MAP_COLS/MAP_ROWS, so
#      the tunnel board uses tun_in() and an off-board neighbour is simply not
#      a legal move -- a wall, not the far wall.
#   2. A player underground keeps their surface q/r pinned to the hatch they
#      came down (the load-bearing invariant at the top of tunnels.hpp).  Their
#      real position is tq/tr on the tunnel board.  Anything that reads
#      obs.map[(me.q, me.r)] while me.depth is 1 is reading the wrong board.
TUN_COLS, TUN_ROWS = 16, 10
MAX_HATCHES = 8

TERR_BUNKER      = 12   # Bunker Entrance -- on BOTH boards. 1 MP each way.
TERR_VENT        = 13   # Vent Shaft      -- on BOTH boards. VENT_ASCEND_MP up.
TERR_TUNNEL      = 14   # Tunnel Floor    -- tunnel only. Waters and salvages.
TERR_COLLAPSED   = 15   # Collapsed Tunnel-- tunnel only. Impassable, permanent.

TUNNEL_MC      = 2      # MC of Tunnel Floor; mirrors TERRAIN_MC[14]
DESCEND_MP     = 1      # tunnelStepDown() always charges 1
VENT_ASCEND_MP = 2      # climbing out of a Vent Shaft costs double
# Underground sight: your own hex plus one ring, +1 carrying a Bile Flare
# (item 54, carried NOT equipped -- hasTunnelLight scans invType[]), +1 Scout.
TUNNEL_VIS_BASE  = 1
TUNNEL_VIS_SCOUT = 1
# Bad air (TUNNEL_REST_LL_PCT, tunnels.hpp).  Underground the dawn exposure
# tick is off entirely -- a corridor is cover -- and this is what the tunnels
# charge in its place: a rest at depth 1 has this percent chance of costing a
# LL.  It is netted against the rest-heal like every other dawn loss, so it
# only shows as a drop when there was nothing to heal with; when it does land
# alone it is NOT floored at LL 1 the way exposure is, and can down you.
TUNNEL_REST_LL_PCT = 30
ITEM_BILE_FLARE  = 54
ARCH_SCOUT       = 4

# -- Equipment (data/items.cfg) ----------------------------------------------
# Parsed from the live items.cfg rather than hardcoded, because the registry is
# SD-loaded on the board and edited far more often than this file.  The bots
# need two things from it: which ids are equippable, and which equip slot each
# one claims, so a policy can tell "I am already wearing something there" from
# "this slot is empty".
#
# Until this existed no bot ever sent equip_item, which means every balance
# number in docs/bot-testing.md was measured on a survivor wearing nothing.
EQUIP_SLOT_BY_NAME = {"head": 0, "body": 1, "hand": 2, "feet": 3, "vehicle": 4}
EQUIP_SLOTS = 5
# INV_SLOTS_MAX in the .ino: the width of invType[]/invQty[] and the ceiling
# effectiveInvSlots() clamps to. Was 12 -- the same as the Mule's base, which
# is why slot-granting gear did nothing for that archetype.
INV_SLOTS_MAX = 18


def _parse_items(path=None):
    """Every category=equipment entry in items.cfg, as (slot, stats) pairs."""
    import os
    if path is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "data", "items.cfg")
    slots, stats, blocks, cur = {}, {}, [], None
    try:
        with open(path, encoding="utf-8") as fh:
            for raw_line in fh:
                line = raw_line.split("#")[0].strip()
                if not line:
                    continue
                if line == "[item]":
                    cur = {}
                    blocks.append(cur)
                    continue
                if cur is None or "=" not in line:
                    continue
                k, v = (x.strip() for x in line.split("=", 1))
                cur[k] = v
    except OSError:
        return {}, {}          # detached from the repo: bots simply never equip

    def sbyte(v):
        """items.cfg writes negative effect params as unsigned (253 = -3)."""
        n = int(v)
        return n - 256 if n > 127 else n

    for d in blocks:
        if d.get("category") != "equipment" or "id" not in d:
            continue
        slot = EQUIP_SLOT_BY_NAME.get(d.get("slot", ""), -1)
        if slot < 0:
            continue
        iid = int(d["id"])
        slots[iid] = slot
        fx = [(d.get("effect"), d.get("param")), (d.get("effect2"), d.get("param2"))]
        st = {
            "ll":      int(d.get("ll", 0)),
            "mp":      int(d.get("mp", 0)),
            "slots":   int(d.get("slots", 0)),
            # rad and threat are stored as written: negative is the good one.
            "rad":     int(d.get("rad", 0)),
            "threat":  sum(sbyte(p) for e, p in fx if e == "threat_mod" and p),
            "vision":  sum(1 for e, p in fx if e == "reveal_fog" and p and int(p) == 1),
            "terrain": int(d.get("terrain", 0)),
            "narrative": {int(p) for e, p in fx if e == "narrative" and p},
            # A *_cost item's mp only lands on a dawn where the cost was paid
            # (applyDawnItemCosts), so it is worth strictly less than the same
            # mp for free.
            "gated":   any(d.get(k) for k in
                           ("water_cost", "food_cost", "fuel_cost",
                            "med_cost", "scrap_cost")),
        }
        stats[iid] = st
    return slots, stats


EQUIPMENT, EQUIP_STATS = _parse_items()

# NAR_* params worth weighting (items.cfg "narrative").
NAR_FIRE_STARTER, NAR_COLD_IMMUNE = 20, 21
NAR_SCAV_DOUBLE, NAR_RIVER_FORAGE, NAR_LAND_FORAGE = 30, 31, 32

# How much a fuel-gated mp bonus is discounted. Not zero -- a Motorbike with
# fuel is the best mobility in the game -- but not face value either, because
# the bots run their fuel down and the bonus silently vanishes on a dry dawn.
GATED_MP_DISCOUNT = 0.5


def gear_score(item_id, weights):
    """How much a policy with these weights wants to wear `item_id`.

    Pure function of items.cfg plus the policy's own preferences, so adding an
    item to the registry needs no code change here. Higher is better; an
    unknown or unequippable id scores 0, which is also what "wearing nothing"
    scores, so a strictly-greater comparison never equips junk.
    """
    st = EQUIP_STATS.get(item_id)
    if not st:
        return 0.0
    mp = st["mp"] * (GATED_MP_DISCOUNT if st["gated"] else 1.0)
    score = (weights.get("ll", 0.0)     * st["ll"]
             + weights.get("mp", 0.0)    * mp
             + weights.get("slots", 0.0) * st["slots"]
             + weights.get("vision", 0.0) * st["vision"]
             # rad and threat are good when negative, so flip them.
             + weights.get("rad", 0.0)    * -st["rad"]
             + weights.get("threat", 0.0) * -st["threat"])
    if st["terrain"]:
        score += weights.get("terrain", 0.0)
    for nar in st["narrative"]:
        score += weights.get("nar", {}).get(nar, 0.0)
    return score

# Salvage DN per terrain (Esp32HexMapCrawl.ino:256).  Tunnel Floor salvages at
# 7 and TERRAIN_IS_RUINS[14] is 0, so scavenging underground is the one place
# in the game that yields scrap *without* raising the threat clock.
TERRAIN_SALVAGE_DN = (0, 0, 0, 0, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 7, 0)

# handleAction() refuses these three outright at depth 1 (actions_game_loop.hpp).
# REST used to be a fourth and is not any more: you can sleep in a bunker, the
# weather cannot reach you there, and the price is the TUNNEL_REST_LL_PCT
# bad-air roll above.  That matters to sprint mode -- tickGame() collapses the
# day only when *every* connected player is resting, and a bot underground can
# now hold up its end of that instead of stalling the whole fleet.
ACTS_REFUSED_UNDERGROUND = frozenset((ACT_SHELTER, ACT_CRAFT, ACT_SURVEY))


def tun_in(q: int, r: int) -> bool:
    """The tunnel board is walled, not toroidal (tunIn, tunnels.hpp:39)."""
    return 0 <= q < TUN_COLS and 0 <= r < TUN_ROWS


def tun_distance(q1: int, r1: int, q2: int, r2: int) -> int:
    """Axial hex distance with no wrap search (tunDist, tunnels.hpp:45)."""
    dq, dr = q2 - q1, r2 - r1
    return (abs(dq) + abs(dq + dr) + abs(dr)) // 2


def is_hatch_terrain(t: int) -> bool:
    """Bunker Entrance or Vent Shaft.  Appears on both boards: on the surface
    it is the way down, on the tunnel board it is the way up."""
    return t in (TERR_BUNKER, TERR_VENT)


def is_tunnel_terrain(t: int) -> bool:
    return TERR_BUNKER <= t <= TERR_COLLAPSED


def ascend_cost(shaft_terrain: int) -> int:
    """MP to climb out through this shaft (tunnelAscendCost)."""
    return VENT_ASCEND_MP if shaft_terrain == TERR_VENT else 1


def can_salvage(terrain: int) -> bool:
    return 0 <= terrain < NUM_TERRAIN and TERRAIN_SALVAGE_DN[terrain] != 0


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
