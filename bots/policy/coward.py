"""Coward -- refuses all risk and tries only to stay alive.

Never opens a POI, keeps food and water deep, rests early, and actively
avoids burning hexes, the Creeping Doom and radioactive ground.

Its job is to answer one question: **is passive play viable?** If the Coward
survives indefinitely and still accumulates a respectable score just by
pottering around collecting piles, then the survival systems are not applying
real pressure and the risk mechanics are decoration -- a player can opt out of
the whole threat model and still do fine.

If it dies, the pressure is real and the question becomes whether it dies for
interesting reasons.
"""
from config import (RES_FOOD, RES_WATER, ACT_REST, TERRAIN_IS_RUINS,
                    NUM_TERRAIN, NAR_COLD_IMMUNE, NAR_FIRE_STARTER,
                    NAR_LAND_FORAGE, NAR_RIVER_FORAGE, NAR_SCAV_DOUBLE)
from navigate import best_target, hex_distance
from .base import Action
from .survivor import SurvivorPolicy

SAFE_FOOD = 4
SAFE_WATER = 5
FIRE_AVOID_RADIUS = 2
DOOM_AVOID_RADIUS = 4
NUKE_CRATER = 10


class CowardPolicy(SurvivorPolicy):
    name = "coward"
    # Gear the way it plays: nothing but not dying. Exposure is the largest
    # single LL drain measured, so the Bear Skin Cape's immunity outranks any
    # raw stat; sealed suits matter because this bot refuses radioactive
    # ground and a suit lets it stop refusing. Mobility and carry space are
    # worth almost nothing to a survivor whose plan is to sit still.
    gear_weights = {
        "ll": 4.0, "rad": 3.0, "threat": 2.0, "terrain": 2.0,
        "mp": 0.5, "slots": 0.5, "vision": 0.5,
        "nar": {NAR_COLD_IMMUNE: 8.0, NAR_FIRE_STARTER: 3.0,
                NAR_LAND_FORAGE: 1.0, NAR_RIVER_FORAGE: 0.5,
                NAR_SCAV_DOUBLE: 0.5},
    }
    engage_encounters = True
    rest_below_ll = 4          # rests much earlier than the others
    pack_headroom = 2

    def _danger(self, obs, q, r) -> float:
        """Soft penalty for hexes near things that hurt."""
        d = 0.0
        for fq, fr, intensity in obs.world.fire:
            if hex_distance(q, r, fq, fr) <= FIRE_AVOID_RADIUS:
                d += 40.0 * max(1, intensity)
        if obs.world.doom_q >= 0:
            if hex_distance(q, r, obs.world.doom_q, obs.world.doom_r) <= DOOM_AVOID_RADIUS:
                d += 60.0
        for fq, fr, *_ in obs.world.flood:
            if (q, r) == (fq, fr):
                d += 25.0
        return d

    def pursue(self, obs) -> Action:
        me = obs.me

        # Stocked up and unhurt: sit still.  Resting also ends the day early
        # once everyone is doing it, which is free time-compression.
        stocked = (me.inv[RES_FOOD] >= SAFE_FOOD and
                   me.inv[RES_WATER] >= SAFE_WATER)
        if stocked and me.ll >= 5:
            act = self.rest_once(obs, "stocked and healthy, sit tight")
            if act is not None:
                return act
            return Action("noop", why="already resting")

        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        need_water = me.inv[RES_WATER] < SAFE_WATER
        need_food = me.inv[RES_FOOD] < SAFE_FOOD
        full = self.pack_full(obs)

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = 0.0
            if cell is not None:
                if cell.poi:
                    return None                      # never walk onto a POI hex
                if cell.terrain == NUKE_CRATER:
                    return None
                if cell.terrain < NUM_TERRAIN and TERRAIN_IS_RUINS[cell.terrain]:
                    v -= 5.0                         # ruins raise the threat clock
                if cell.resource and not full:
                    if cell.resource == RES_WATER + 1 and need_water:
                        v += 30.0
                    elif cell.resource == RES_FOOD + 1 and need_food:
                        v += 25.0
                    else:
                        v += 6.0
                if cell.shelter:
                    v += 8.0                         # shelter blunts weather
            else:
                v += 1.0                             # mild curiosity only
            v -= self._danger(obs, q, r)
            return None if v <= 0 else v / cost

        target = best_target(obs.map, me.q, me.r, value, max_cost=25)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d, why=f"safe -> ({q},{r}) v={val:.1f}")

        # Nothing appealing and nothing urgent: rest rather than wander into
        # something unpleasant.
        act = self.rest_once(obs, "nothing safe worth walking to")
        return act if act is not None else Action("noop", why="holding position")
