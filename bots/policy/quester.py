"""Quester -- plays every quest in the content to the end, in order, and
checks the order holds.

A ContentMax (it opens every POI it can reach, goes after Machine Gun
Kane's chapters, shadowing the jeep before the last, and after VACANCY's
four parts -- the sign's position and its two sites ride the world object,
so those are walked to, not hunted) with the quest journal in charge
(quests.py):

* **Finds the steps.**  The map shows only a POI bit, never which file is
  on a hex, so a quest step is found by opening POIs -- and every file of a
  pool is placed exactly once (hex-map.hpp Phase 5).  POIs on terrains whose
  pools still hold an open step are worth far more; Jack's Chopper is the
  one quest POI the map does show (Open Scrub, art variant 10).
* **Plays the steps.**  Quest goal nodes and quest items dominate its
  encounter planner, so it walks the branch that advances a chain rather
  than the richest one, takes a `requires_item` shortcut whenever it holds
  the item, and wears gate gear (the Dirty Glove) while a gate still needs it.
* **Keeps what the next step needs.**  Quest items are never dropped, sold
  or crafted away, and are picked up off the ground and off graves.
* **Checks the gates.**  Every `requires_item` choice it lacks the item for
  is sent once anyway, expecting no_item; a step finished without its item
  is `quest_ungated`, one finished out of order `quest_out_of_order`.
* **Runs the workshop.**  Every recipe it learns it crafts once (the fleet's
  first), so each recipe is checked end to end: taught, known, made.

Its numbers are a coverage report, not a balance measurement: it takes
risks for a goal node that no other policy would.  Read the quest ledger at
the end of an arena run (arena.py prints it) or `python metrics.py`.
"""
from config import TERR_SETTLEMENT
from navigate import best_target, frontier_bonus
from quests import LEDGER
from .base import Action
from .contentmax import ContentMaxPolicy, NEW_HEX_VALUE, FRONTIER_WEIGHT, PILE_VALUE
from .survivor import ends_journey

# How much more a POI is worth on a terrain whose pool still holds an open
# quest step, and the one hex the map shows outright.
QUEST_TERRAIN_MULT = 2.5
JACKS_CHOPPER_VALUE = 400.0
POI_VALUE = 60.0
OPEN_SCRUB, CHOPPER_VARIANT = 0, 10


def _terrain_of_pool(lib) -> dict:
    """{pool path: terrain index} from index.json (terrain keys are digits)."""
    out = {}
    for key, pool in (lib.index or {}).items():
        if key.isdigit() and isinstance(pool, dict) and pool.get("path"):
            out[pool["path"]] = int(key)
    return out


class QuesterPolicy(ContentMaxPolicy):
    name = "quester"
    quest_drive = True
    probe_gates = True
    novelty_bonus = 1.0
    encounter_risk = 0.8
    # Sleep under a roof when the scrap is spare.  Exposure is 2 LL a
    # night on most of the map and the rest heal needs water the pack
    # rarely has, so on the K10 run of 2026-10-13 the questers spent most
    # of their days at LL 1-3 -- resting instead of walking, and too
    # hurt to take a scene they found.  A basic shelter is 1 scrap and
    # the last AP of the day and zeroes the night's whole loss, thirst
    # included.  Down to the last scrap: the first rerun held three back
    # for a canteen no quester ever crafted and slept in the open.
    shelter_when_exposed = True

    def __init__(self, rng, library=None):
        super().__init__(rng, library)
        self._pool_terrain = _terrain_of_pool(self.lib)

    # -- the workshop ---------------------------------------------------------
    def craft_also_wanted(self, obs, rid) -> bool:
        """Every taught recipe made once, fleet-wide: the craft step of the
        workshop quest is open until somebody has made it."""
        st = LEDGER.steps.get(f"recipe.{rid}.craft")
        return st is None or st.get("status") != "done"

    # -- finding the steps -----------------------------------------------------
    def quest_terrains(self, obs) -> set:
        """Terrains whose POI pools hold a step still open for us."""
        out = set()
        for s in self.journal.open_steps(obs.world):
            if s.chapter or s.vac_part:
                continue                    # Kane's chapters and VACANCY's parts are not POIs
            for biome, _eid in s.files:
                t = self._pool_terrain.get(biome)
                if t is not None:
                    out.add(t)
        return out

    def pursue(self, obs) -> Action:
        me = obs.me
        here = obs.map[(me.q, me.r)]
        if here is not None and here.poi and me.ap > 0:
            act = self.try_open_poi(obs, "POI underfoot")
            if act is not None:
                return act
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        hot = self.quest_terrains(obs)
        chopper_open = any(s.key.startswith("jack.") for s in self.journal.open_steps())
        full = self.pack_full(obs)
        settle_poi = any(s.key.endswith(".learn") for s in self.journal.open_steps())

        def value(cell, q, r, cost):
            if cost <= 0:
                return None
            v = 0.0
            if cell is None:
                v += NEW_HEX_VALUE + FRONTIER_WEIGHT * 3
            else:
                if cell.poi and self._poi_tries.get((0, q, r), 0) < 2:
                    pv = POI_VALUE
                    if cell.terrain in hot:
                        pv *= QUEST_TERRAIN_MULT
                    if cell.terrain == TERR_SETTLEMENT and settle_poi:
                        pv *= QUEST_TERRAIN_MULT
                    if (chopper_open and cell.terrain == OPEN_SCRUB
                            and cell.variant == CHOPPER_VARIANT):
                        pv = JACKS_CHOPPER_VALUE
                    v += pv
                if cell.resource and not full:
                    v += PILE_VALUE
                if not cell.visited_by(obs.pid):
                    v += NEW_HEX_VALUE
                v += FRONTIER_WEIGHT * frontier_bonus(obs.map, q, r)
            return None if v <= 0 else v / cost

        target = best_target(obs.map, me.q, me.r, value, max_cost=60,
                             stop_at=ends_journey)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d, why=f"quest -> ({q},{r}) cost={cost} v={val:.1f}")
        return Action("move", d=self.rng.choice(legal), why="fallback step")
