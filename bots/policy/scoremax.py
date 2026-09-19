"""ScoreMax -- plays purely for the score counter.

Design note, because this looks like a stacked deck and is not:

`engage_encounters = False`.  ScoreMax deliberately never opens a POI, and
ContentMax opens every one it can reach.  Those two running in the same arena
turn "are encounters worth engaging?" into a direct measurement -- the score
gap between them *is* the answer -- instead of burying it inside one bot's
internal cost model, where a tuning mistake would look like a game-balance
result.

Its strategy follows from the measured economy: collectResource() pays
10/token automatically on stepping onto a pile, exploration pays 1 for a new
hex, and 48.5% of hexes hold a pile.  So the optimal play is close to "keep
moving, prefer piles, keep pack space free" -- which is exactly the finding
worth confirming or refuting on hardware.
"""
from config import RES_FOOD, RES_WATER
from navigate import best_target, frontier_bonus
from .base import Action
from .survivor import SurvivorPolicy

# A full pile is 1-3 tokens at 10 points each; a new hex is 1 point.  These
# weights are relative value per MP, not absolute score.
PILE_VALUE = 20
NEW_HEX_VALUE = 1
FRONTIER_WEIGHT = 0.8
STAPLE_BONUS = 12      # extra pull toward water/food piles when running low


class ScoreMaxPolicy(SurvivorPolicy):
    name = "scoremax"
    # Opens a POI it is standing on (handled in SurvivorPolicy.decide) but
    # never detours for one -- that is the contrast against ContentMax.
    engage_encounters = True
    rest_below_ll = 2

    def pursue(self, obs) -> Action:
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        full = self.pack_full(obs)
        need_water = me.inv[RES_WATER] < 4
        need_food = me.inv[RES_FOOD] < 2

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = 0.0
            if cell is None:
                # Unrevealed: worth the exploration point plus whatever it hides.
                v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
            else:
                if cell.resource and not full:
                    v += PILE_VALUE
                    # resource ids are 1-based: 1 water, 2 food.
                    if need_water and cell.resource == RES_WATER + 1:
                        v += STAPLE_BONUS
                    if need_food and cell.resource == RES_FOOD + 1:
                        v += STAPLE_BONUS
                if not cell.visited_by(obs.pid):
                    v += NEW_HEX_VALUE
                v += FRONTIER_WEIGHT * frontier_bonus(obs.map, q, r)
            return None if v <= 0 else v / cost

        target = best_target(obs.map, me.q, me.r, value, max_cost=30)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d,
                          why=f"-> ({q},{r}) cost={cost} v={val:.1f}")

        # Pathfinder wants a direction the server currently refuses (fog, or a
        # hazard it can see and we cannot).  Take any legal step rather than
        # standing still -- movement is where the points are.
        return Action("move", d=self.rng.choice(legal), why="fallback step")
