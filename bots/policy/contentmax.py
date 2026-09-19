"""ContentMax -- plays to touch as much authored content as possible.

Seeks POIs, opens every one it reaches, and pushes deeper into encounter trees
than a score-driven bot would.  It is scored on `content_score()` -- encounters
opened and banked, distinct nodes reached, recipes learned, roll win rate --
not on the score counter, because with banking at 3/token against 10/token for
simply walking onto a pile, it is structurally going to lose a score race.

That loss is the measurement, not a bug. ScoreMax ignores POIs entirely, so
the score gap between the two is the price of engaging with the content.

POIs are permanently consumed: enc_start sets cell.poi = 0 and only an
*involuntary* end (dawn, disconnect) restores it. Aborting or failing out
keeps it consumed. So there are only 108 on the map and every one this bot
opens is one a rival can never have.
"""
from navigate import best_target, frontier_bonus
from .base import Action
from .survivor import SurvivorPolicy

POI_VALUE = 60         # dominates piles; content is the whole point
NEW_HEX_VALUE = 2      # exploring is how you find more POIs
FRONTIER_WEIGHT = 1.2
PILE_VALUE = 4         # still worth stepping on, just not worth a detour


class ContentMaxPolicy(SurvivorPolicy):
    name = "contentmax"
    engage_encounters = True
    # Willing to take worse odds and push deeper than a cautious bot, because
    # unseen nodes are the objective -- but not suicidally: a downed bot stops
    # reaching content at all.
    min_success = 0.30
    bank_greed = 0.40
    rest_below_ll = 3

    def pursue(self, obs) -> Action:
        me = obs.me
        cell_here = obs.map[(me.q, me.r)]

        # Standing on an unopened POI: take it.  try_open_poi returns None once
        # it has tried enough -- a consumed POI leaves a stale bit in our map
        # and enc_start is silent, so falling through to movement is the only
        # way out of that.
        if cell_here is not None and cell_here.poi and me.mp > 0:
            act = self.try_open_poi(obs, "POI underfoot")
            if act is not None:
                return act

        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        full = self.pack_full(obs)

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = 0.0
            if cell is None:
                v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
            else:
                if cell.poi:
                    v += POI_VALUE
                if cell.resource and not full:
                    v += PILE_VALUE
                if not cell.visited_by(obs.pid):
                    v += NEW_HEX_VALUE
                v += FRONTIER_WEIGHT * frontier_bonus(obs.map, q, r)
            return None if v <= 0 else v / cost

        # Longer horizon than ScoreMax: a POI is worth walking a long way for.
        target = best_target(obs.map, me.q, me.r, value, max_cost=45)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d,
                          why=f"-> ({q},{r}) cost={cost} v={val:.1f}")
        return Action("move", d=self.rng.choice(legal), why="fallback step")
