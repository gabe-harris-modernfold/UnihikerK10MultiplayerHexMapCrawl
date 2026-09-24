"""Terrain builder registry.

    BUILDERS[terrain](variant, seed) -> Canvas
    POI_BUILDERS["terrain_variant"](seed) -> Canvas

Variant 0 is the common, quiet ground (pickVariant weights rank-quadratically,
so v0 is ~n^2 times as common as the last slot); the weird stuff lives in the
high slots where a player stumbles on it. Builders live in:

    tg_t_open.py   Open Scrub, Ash Dunes, Marsh, Glass Fields, Jack's Chopper
    tg_t_built.py  Broken Urban, Flooded District, Settlement, city cores
    tg_t_high.py   Rust Forest, Ridge, Mountain, Nuke Crater
    tg_t_under.py  Bunker Entrance, Vent Shaft, Tunnel Floor, Collapsed Tunnel
"""
import tg_t_open as O
import tg_t_built as B
import tg_t_high as Hh
import tg_t_under as U

BUILDERS = {
    0: O.scrub,
    1: O.ash_dunes,
    2: Hh.rust_forest,
    3: O.marsh,
    4: B.broken_urban,
    5: B.flooded,
    6: O.glass_fields,
    7: Hh.ridge,
    8: Hh.mountain,
    9: B.settlement,
    10: Hh.nuke_crater,
    # 11 River Channel: deliberately none (see build_tiles.COUNTS)
    12: U.bunker_entrance,
    13: U.vent_shaft,
    14: U.tunnel_floor,
    15: U.tunnel_collapsed,
}

POI_BUILDERS = {
    '0_10': O.jacks_chopper,
    '4_10': lambda seed: B.city_core(0, seed),
    '4_11': lambda seed: B.city_core(1, seed),
    '4_12': lambda seed: B.city_core(2, seed),
}
