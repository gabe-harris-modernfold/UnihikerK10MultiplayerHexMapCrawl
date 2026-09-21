"""Subterranean Explorers -- bots that live in the bunker tunnels.

Every other policy plays the 75x57 surface board and treats a hatch as a hole
to fall into.  These two play the *other* board on purpose:

  `subterranean`  maps the network, works the corridors, and comes up only
                  for the things the tunnels cannot give it.
  `tunnelrunner`  (tunnelrunner.py) uses the tunnels as transport -- dive,
                  cross, surface somewhere far away.

Both share this file's `TunnelPolicy`, which owns the dive/surface cycle and
the cross-board bookkeeping.  What they differ on is where they go once they
are down there.

## What the tunnels actually cost (tunnels.hpp, actions_game_loop.hpp)

The spec sells the tunnels as a shortcut -- 44-67% of the MP for a long
crossing.  Playing them turns up four prices the surface does not charge, and
measuring those is the point of these bots:

1. **2 MP a step**, against a surface average of ~1.6, out of a daily budget
   of `ll + 3`.  A healthy survivor gets four or five steps underground per
   day.  Everything else follows from that number.
2. **Bad air.**  REST *is* allowed at depth 1 now (SHELTER, CRAFT and SURVEY
   are still refused), so running out of MP down there no longer ends the
   day -- but dawnUpkeep() rolls `TUNNEL_REST_LL_PCT` (30%) for a LL when you
   sleep below.  That point is netted against the rest-heal like any other
   dawn loss, so it is absorbed while the survivor has the food and water to
   heal with (F>=2, W>=2) and it is what kills them when they do not: unlike
   exposure, bad air is not floored at LL 1.  The budget `mp_reserve()` keeps
   back is therefore no longer about getting out before dark, it is about
   getting out before the *supplies* run out; `stranded_dawns` counts the
   dives that misjudged it.
3. **No food.**  `TERRAIN_FORAGE_DN[14]` is 0; Tunnel Floor waters and
   salvages and that is all.  Dawn still eats a food token whether you are
   under the sky or not, so every trip down is on a food clock.
4. **No exposure at all** -- the one price the tunnels do *not* charge, and
   the reason to sleep down there on purpose.  dawnUpkeep() treats depth != 0
   as cover, so a night below saves the 2 LL bite that 96% of the surface
   charges: a bunker beats an unsheltered camp outright, 2 LL certain against
   0.3 expected.  It does not beat a *built* shelter, which zeroes the whole
   dawn loss and cannot suffocate anyone.

   This clause used to read the hex you were NOT standing on: q/r stay pinned
   to the hatch (the invariant at the top of tunnels.hpp), so with
   `TERRAIN_SV[12]` at 2 and `TERRAIN_SV[13]` at 0, a night below a Bunker
   Entrance was sheltered and a night below a Vent Shaft took the full bite --
   same corridor, opposite answer.  That is fixed, so `bunker_dawns` /
   `vent_dawns` no longer measure LL.  They only separate the two hatch types
   now, which still differ by the MP it costs to climb out, 1 against 2.

## How a dive is steered

A hatch is never a waypoint.  Stepping onto one IS the crossing -- it happens
as the last act of the move, there is no control and no confirmation -- so
every search on either board passes `stop_at=ends_journey`, and hatches score
`None` unless crossing is the actual intent.  Without that the pathfinder
plots a tidy line straight through a hatch and the survivor is spat out onto
the other board halfway to wherever they were going.

## Learning the network

The pairing between a surface hatch and the shaft it comes out at is decided
at generation time and never sent on the wire.  A client can only learn it by
watching a `tun_in` / `tun_out` event -- which names the hatch index and the
*surface* hex -- and pairing it against where the survivor was underground.
`Observation.hatches` accumulates that, including from other players' events,
so a fleet of these bots maps the network faster than one of them could.
"""
from config import (ACT_REST, ACT_SCAV, NAR_COLD_IMMUNE, NAR_FIRE_STARTER,
                    NAR_LAND_FORAGE, NAR_RIVER_FORAGE, NAR_SCAV_DOUBLE,
                    RES_FOOD, RES_SCRAP, RES_WATER, TERR_BUNKER, ascend_cost,
                    can_salvage, is_hatch_terrain)
from navigate import best_target, dijkstra, frontier_bonus, hex_distance
from .base import Action
from .survivor import token_room
from .survivor import SurvivorPolicy, ends_journey

# ── Dive gating ──────────────────────────────────────────────────────────────
# Do not go down without the supplies to come back up.  Dawn eats 1 food and
# 2 water wherever you sleep, and there is no food underground at all.
#
# These are deliberately near the floor rather than comfortable.  An earlier
# cut wanted food 3, water 4, LL 4 and MP 6 *simultaneously*, and measured
# against a real economy that conjunction almost never came true: the
# survival floor tops the pack up to its own thresholds and no further, LL
# spends most of a run in the 2-4 band, and MP is gone by mid-afternoon.  The
# bots resupplied forever and never once went underground.  What a dive
# actually needs is a day's food in hand, water enough to reach a cistern
# (Tunnel Floor waters, so this one is refillable down there), an LL that is
# not already critical, and a couple of corridor steps' worth of MP.
DIVE_FOOD  = 2
# Water is deliberately almost not a gate at all.  TERRAIN_HAS_WATER[14] is 1,
# so *every* corridor cell is drinkable -- underground is the wettest place on
# the map, against 4.7% of the surface.  Requiring a full canteen to go
# somewhere that refills it kept the bots on the surface through an entire
# 25-day mock run, thirsty, standing on dry scrub.
DIVE_WATER = 1
# Enough MP to take a step at all, and no more.  The gate used to want a
# whole day's budget in hand, which sounds prudent and is not: the walk to
# the hatch spends exactly that budget, so the bot re-tested it every 0.35 s
# on the way and turned round each time a pile or a drink dropped it under.
# One policy in a 25-day mock run reached the gate zero times.  Arriving
# underground spent is no longer a problem worth avoiding either -- REST
# works down there, so the survivor sleeps and dawn refills.
DIVE_MP    = 2
# Surface when the food is gone outright.  Any higher and the dive ends the
# morning after it starts: dawn takes a token, and a bot that leaves at 1
# would spend its whole life on the ladder.
SURFACE_FOOD = 0
# Slack on top of the measured cost of walking to the nearest way out.  One
# spare step, because the exit we can see may not be the exit we can reach.
EXIT_SLACK = 2
# Fallback reserve when no shaft has been revealed yet: base vision
# underground is 1, so the first steps of a dive are genuinely blind.
EXIT_UNKNOWN_RESERVE = 6

# The tunnel board is 160 cells at 2 MP a step; this crosses all of it.
TUNNEL_SEARCH_COST = 80
SURFACE_SEARCH_COST = 45


class TunnelPolicy(SurvivorPolicy):
    """Shared dive/surface machinery.  Subclasses pick the destinations."""

    name = "tunnel"
    tunnel_capable = True
    engage_encounters = True
    # Underground sight is TUNNEL_VIS_BASE 1 -- your own hex and one ring --
    # so a single +1 is proportionally worth far more down there than the
    # same point is on the surface. A corridor step costs TUNNEL_MC 2, which
    # doubles what every MP is worth. Weather never reaches a corridor, so
    # exposure immunity and sealed suits are close to dead weight on a bot
    # that spends its life below.
    gear_weights = {
        "vision": 5.0, "mp": 3.5, "ll": 2.5, "slots": 1.5,
        "rad": 0.5, "threat": 0.5, "terrain": 0.5,
        "nar": {NAR_SCAV_DOUBLE: 2.0, NAR_COLD_IMMUNE: 0.5,
                NAR_FIRE_STARTER: 0.5, NAR_LAND_FORAGE: 0.5,
                NAR_RIVER_FORAGE: 0.25},
    }
    # POIs underground come from the "14" encounter pool, which does not exist
    # in data/encounters/index.json yet -- generateTunnels() places none until
    # it does. The handling is here so the bots exercise it the day it lands.
    rest_below_ll = 2
    # Salvage a corridor when there is MP to spare: Tunnel Floor has
    # TERRAIN_SALVAGE_DN 7 and TERRAIN_IS_RUINS 0, making it the only scrap in
    # the game that does not tick the threat clock up.
    scav_underground = True
    scrap_cap = 6
    # Provision one token above the dive gate, so topping up to the floor
    # actually clears it.  Setting the floor *equal* to the gate means the
    # bot stops harvesting at the exact point it would be allowed to leave,
    # and one dawn puts it back under.
    #
    # Water sits one under the ordinary floor, because invSlots is the
    # *token* cap (resetSurvivor: "2 water, 1 food ... = 6 of 8") and
    # collectResource() refuses every pickup once it is reached.  Food 3 plus
    # water 4 is 7 of 8 and leaves a survivor unable to pick up the piles it
    # walks over -- and of the two, water is the one the tunnels restock.
    food_floor = DIVE_FOOD + 1
    water_floor = 3

    def __init__(self, rng, library=None):
        super().__init__(rng, library)
        self.stats.update({
            "descents": 0, "ascents": 0,
            "tunnel_steps": 0, "surface_steps": 0,
            "pairings_learned": 0, "dawns_below": 0,
            "bunker_dawns": 0, "vent_dawns": 0, "stranded_dawns": 0,
            # Nights actually slept below (each one a bad-air roll) and what
            # those dawns cost in LL. There is no bad-air event on the wire,
            # so below_dawn_ll folds in hunger and thirst too -- read it
            # against a surface policy's exposure line, not on its own.
            "rests_below": 0, "below_dawn_ll": 0,
            # Surface hexes between the hatch we went in and the one we came
            # out of, against the underground steps it took. This pair IS the
            # spec's 44-67% saving claim, measured instead of modelled.
            "transit_hexes": 0, "transit_steps": 0, "longest_transit": 0,
            "tunnel_scav": 0,
        })
        self._shafts_used = set()
        self._hatches_seen = set()  # hatch indices anyone has been seen using
        self._dive = None           # bookkeeping for the trip in progress
        self._wanted_out = False    # surface_reason fired and we are still down
        self._depth = 0             # depth at the last decide(), for dawn
        self._dive_terrain = 0      # 12 or 13: which hatch we came down
        self._surfaced_reason = ""
        # Walking to a hatch spends the very MP that ready_to_dive() tests,
        # so without a latch the decision flips halfway there and the bot
        # oscillates between the hatch and the nearest berry bush forever.
        self._committed = False

    # ── metrics ─────────────────────────────────────────────────────────
    def content_score(self) -> dict:
        s = super().content_score()
        s["shafts_used"] = len(self._shafts_used)
        s["hatches_known"] = len(self._hatches_seen)
        steps = s["transit_steps"]
        # MP per surface hex crossed. Under 1.6 (the surface average MC) the
        # tunnels paid for themselves on that trip; over it they did not.
        s["mp_per_surface_hex"] = (
            round((steps * 2) / s["transit_hexes"], 2) if s["transit_hexes"] else None)
        return s

    def on_event(self, ev):
        super().on_event(ev)
        # super() drops other players' events, but it returns rather than
        # raising, so re-check here instead of assuming.
        if self.pid >= 0 and ev.get("pid") not in (None, self.pid):
            return
        k = ev.get("k")
        if k == "tun_in":
            self.stats["descents"] += 1
            self._dive = {"hatch": ev.get("hatch"), "q": ev.get("q"),
                          "r": ev.get("r"), "steps": 0}
        elif k == "tun_out":
            self.stats["ascents"] += 1
            idx = ev.get("hatch")
            if idx is not None:
                self._shafts_used.add(idx)
            if self._dive is not None:
                sq, sr = self._dive.get("q"), self._dive.get("r")
                if sq is not None and ev.get("q") is not None:
                    gap = hex_distance(sq, sr, ev["q"], ev["r"])
                    self.stats["transit_hexes"] += gap
                    self.stats["transit_steps"] += self._dive["steps"]
                    if gap > self.stats["longest_transit"]:
                        self.stats["longest_transit"] = gap
            self._dive = None
        elif k == "tun_in" or k == "tun_out":
            self._committed = False
            self._wanted_out = False
        if k == "mv":
            if ev.get("dp"):
                self.stats["tunnel_steps"] += 1
                if self._dive is not None:
                    self._dive["steps"] += 1
            else:
                self.stats["surface_steps"] += 1
        elif k == "act" and self._depth:
            if ev.get("a") == ACT_SCAV:
                self.stats["tunnel_scav"] += 1
            elif ev.get("a") == ACT_REST:
                self.stats["rests_below"] += 1
        elif k == "dawn" and self._depth:
            # Woke up underground: no exposure either way (dawnUpkeep treats
            # depth as cover), but a TUNNEL_REST_LL_PCT bad-air roll if we
            # slept. Which hatch we came down no longer changes the LL -- it
            # only changes the climb out. See the file header.
            self.stats["dawns_below"] += 1
            self.stats["below_dawn_ll"] += max(0, -int(ev.get("dll", 0) or 0))
            if self._dive_terrain == TERR_BUNKER:
                self.stats["bunker_dawns"] += 1
            else:
                self.stats["vent_dawns"] += 1
            # Wanting out at dawn and still being down here means the dive
            # outlasted its own budget. Not fatal any more -- you sleep where
            # you stand -- but it is the miss that mp_reserve() exists to
            # prevent, so it is worth counting rather than absorbing.
            if self._wanted_out:
                self.stats["stranded_dawns"] += 1

    # ── entry point ─────────────────────────────────────────────────────
    def decide(self, obs) -> Action:
        me = obs.me
        self._depth = me.depth
        # Hatch pairings accumulate from every player's tun_in / tun_out, so
        # this counts what the *fleet* has worked out, not just this bot.
        self._hatches_seen = set(obs.hatches)
        if me.depth:
            # q/r are pinned to the hatch we descended through, so this reads
            # the hatch itself -- which is how we know what climbing out of
            # this dive will cost (Bunker Entrance 1 MP, Vent Shaft 2).
            above = obs.map[(me.q, me.r)]
            if above is not None and is_hatch_terrain(above.terrain):
                self._dive_terrain = above.terrain
        return super().decide(obs)

    def pursue(self, obs) -> Action:
        return self.pursue_below(obs) if obs.underground else self.pursue_above(obs)

    # ── underground ─────────────────────────────────────────────────────
    def mp_reserve(self, obs) -> int:
        """MP we must keep back to reach a way out.

        Measured, not guessed: the real cost of walking to the cheapest shaft
        we have revealed, plus the climb and a step of slack.  Crossing itself
        is never refused -- tunnelStepUp clamps the charge at 0 -- so the only
        thing that can strand a survivor is not reaching the shaft at all.
        """
        q, r = obs.pos()
        dist, _ = dijkstra(obs.tunnel, q, r, max_cost=TUNNEL_SEARCH_COST,
                           stop_at=ends_journey)
        best = None
        for (cq, cr), cost in dist.items():
            cell = obs.tunnel[(cq, cr)]
            if cell is None or not is_hatch_terrain(cell.terrain):
                continue
            total = cost + ascend_cost(cell.terrain)
            if best is None or total < best:
                best = total
        if best is None:
            return EXIT_UNKNOWN_RESERVE
        return best + EXIT_SLACK

    def surface_reason(self, obs) -> str | None:
        """Why this dive should end, or None to stay down."""
        me = obs.me
        if me.inv[RES_FOOD] <= SURFACE_FOOD:
            return f"food {me.inv[RES_FOOD]}: nothing to forage underground"
        # Sleeping down here is allowed and costs no exposure, but the
        # bad-air roll is only harmless while the rest-heal can absorb it.
        # Hurt and under-supplied is the one combination that kills below.
        if me.ll <= self.rest_below_ll + 1 and not (me.food >= 2 and me.water >= 2):
            return f"LL {me.ll} on thin supplies: bad air can finish that"
        # Deliberately NOT a bare "out of MP": running dry underground means
        # sleeping where we stand, which costs a 30% roll against the 2 LL a
        # night in the open costs for certain. The reserve only matters when
        # the food to walk out on is nearly gone as well.
        reserve = self.mp_reserve(obs)
        if me.inv[RES_FOOD] <= SURFACE_FOOD + 1 and me.mp <= reserve:
            return (f"food {me.inv[RES_FOOD]} with the exit {reserve} MP "
                    f"away: leaving while we still can")
        if not self.work_below(obs):
            return "nothing left down here"
        return None

    def work_below(self, obs) -> bool:
        """Is there anything underground still worth a step?"""
        return self.below_target(obs) is not None

    def below_target(self, obs):
        """Subclass hook: (q, r, dir, cost, value) or None."""
        raise NotImplementedError

    def pursue_below(self, obs) -> Action:
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="underground with no legal move")

        why = self.surface_reason(obs)
        self._wanted_out = why is not None
        if why is not None:
            self._surfaced_reason = why
            out = self.climb_out(obs, prefer=self.exit_preference(obs), why=why,
                                 explore_if_blind=False)
            if out is not None:
                return out
            # Wanting out and being able to find a way out are different
            # things: one ring of vision means most of a dive is spent with
            # no shaft in sight. Fall through and keep exploring -- the
            # network is connected, so looking for anything finds an exit.

        act = self.salvage_here(obs)
        if act is not None:
            return act

        target = self.below_target(obs)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d,
                          why=f"tunnel -> ({q},{r}) c={cost} v={val:.1f}")
        # The pathfinder wants a direction the server refuses. Any legal step
        # reveals new corridor: vision underground is a single ring, so even a
        # blind step is information.
        return Action("move", d=self.rng.choice(legal), why="tunnel fallback step")

    def exit_preference(self, obs):
        """Ranking handed to climb_out().  Default: cheapest way out, with a
        nudge toward Bunker Entrances -- 1 MP to climb instead of 2."""
        def prefer(cell, q, r, cost):
            if cell is None or not is_hatch_terrain(cell.terrain):
                return None
            return (10.0 - ascend_cost(cell.terrain)) / cost
        return prefer

    def salvage_here(self, obs) -> Action | None:
        """Work the corridor for scrap when there is MP going spare.

        doScav costs 2 MP and pays 2 scrap plus 5 score on a clean success.
        Underground it is the only action besides WATER that does anything,
        and TERRAIN_IS_RUINS[14] is 0, so unlike every ruin on the surface it
        does not raise the threat clock.
        """
        me = obs.me
        if not self.scav_underground:
            return None
        cell = obs.here()
        if cell is None or not can_salvage(cell.terrain):
            return None
        if me.inv[RES_SCRAP] >= self.scrap_cap or self.pack_full(obs):
            return None
        # 2 MP for the action and one step's worth left over. This used to
        # demand the whole walk-out reserve on top, which in practice meant
        # never: the reserve is the cost of crossing the network, so it is
        # routinely larger than a day's MP, and the bots salvaged nothing at
        # all in a 36-day run. Ending the day down here is not the disaster
        # it was when REST was refused -- you sleep where you stand.
        if me.mp < 4:
            return None
        # SCAVENGE is the one yield action still bound by the pack size
        # (doScav). Asking with a full pack is refused with ABW_PACK_FULL and
        # costs no MP, so a policy that does not check simply asks again next
        # cycle -- which is how the first hardware run livelocked.
        if not token_room(me):
            return None
        return Action("act", a=ACT_SCAV, mp=2,
                      why=f"salvaging the corridor ({me.inv[RES_SCRAP]} scrap, "
                          f"no threat clock down here)")

    # ── surface ─────────────────────────────────────────────────────────
    def provisioned(self, obs) -> bool:
        """Enough in the pack to spend a day below.  Dawn takes 1 food and
        2 water wherever you sleep, and only one of those can be refilled
        underground."""
        me = obs.me
        # Note what is NOT here: a health gate.  It used to refuse to dive
        # below LL 3, which had the rule exactly backwards now that depth is
        # cover -- a hurt survivor sleeping in the open loses 2 LL a night for
        # certain, and the same survivor asleep in a corridor rolls 30% for 1.
        # The bots spent an 84-day mock run too battered to ever qualify for
        # the one place that would have let them heal.  What a dive genuinely
        # needs is supplies; being hurt only matters if the rest-heal cannot
        # fire, because that is what makes bad air lethal.
        return (me.inv[RES_FOOD] >= DIVE_FOOD
                and me.inv[RES_WATER] >= DIVE_WATER
                and (me.ll > self.rest_below_ll
                     or (me.food >= 2 and me.water >= 2)))

    def ready_to_dive(self, obs) -> bool:
        """Stocked, healthy and with a day's MP in hand.

        The MP gate is what stops the hatch becoming a revolving door: we
        surface at low MP, so we cannot immediately turn round and go back
        down -- we have to resupply and sleep first.  It is checked only to
        *start* a dive; once committed, the walk to the hatch is allowed to
        spend it, or the bot would change its mind halfway there every time.
        """
        if self._committed:
            return self.provisioned(obs)
        return self.provisioned(obs) and obs.me.mp >= DIVE_MP

    def hatch_bonus(self, obs, cell, q, r) -> float:
        """Subclass hook: how much we want to dive through THIS hatch,
        before the walk to it is priced in."""
        base = 60.0
        # A Bunker Entrance costs 1 MP to climb back out against the Vent
        # Shaft's 2. That used to be the smaller half of the difference --
        # the hatch overhead also decided the dawn exposure -- and it is now
        # the whole of it.
        if cell.terrain == TERR_BUNKER:
            base += 25.0
        return base

    def dive_move(self, obs) -> Action | None:
        """Walk to a hatch and step onto it -- the step IS the descent."""
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(cell, q, r, cost):
            if cost <= 0 or cell is None or not is_hatch_terrain(cell.terrain):
                return None
            return self.hatch_bonus(obs, cell, q, r) / cost

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=SURFACE_SEARCH_COST, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, val = target
        self._committed = True
        return Action("move", d=d, why=f"diving at hatch ({q},{r}) c={cost}")

    def surface_value(self, obs, cell, q, r, cost) -> float | None:
        """What a surface hex is worth while we are up here resupplying.

        Deliberately narrow: food and water are what we came up for, and
        wandering off after score defeats the point of a tunnel bot.  The
        pull back toward a hatch keeps the walk home short.
        """
        me = obs.me
        if cost <= 0:
            return None
        v = 0.0
        if cell is None:
            v += 2.0                       # unseen ground might hold either
        else:
            if is_hatch_terrain(cell.terrain):
                return None                # not on purpose, not yet
            if cell.resource == RES_FOOD + 1:
                v += 45.0
            elif cell.resource == RES_WATER + 1:
                v += 35.0
            elif cell.resource:
                v += 12.0
            if not cell.visited_by(obs.pid):
                v += 1.0
        v += 0.4 * frontier_bonus(obs.map, q, r)
        return None if v <= 0 else v / cost

    def pursue_above(self, obs) -> Action:
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why="no legal move")

        if self.ready_to_dive(obs):
            dive = self.dive_move(obs)
            if dive is not None:
                return dive
            self._committed = False     # no hatch reachable: resupply instead
        else:
            self._committed = False

        def value(cell, q, r, cost):
            return self.surface_value(obs, cell, q, r, cost)

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=SURFACE_SEARCH_COST, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            q, r, d, cost, val = target
            return Action("move", d=d,
                          why=f"resupply -> ({q},{r}) c={cost} v={val:.1f}")
        return Action("move", d=self.rng.choice(legal), why="surface fallback step")


# ── the explorer ─────────────────────────────────────────────────────────────
UNSEEN_VALUE    = 16     # fogged corridor: mapping the network is the job
NEW_HEX_VALUE   = 5      # floor we have never stood on: +1 score, +1 map
PILE_VALUE      = 20     # 10/token, 1-3 tokens -- the same economy as above
POI_VALUE       = 70
FRONTIER_WEIGHT = 2.5


class SubterraneanPolicy(TunnelPolicy):
    """Maps the bunker network and works it for everything it holds.

    The measurement it exists for: **is the underground worth living in?**
    It collects the same 10-points-a-token piles as a surface bot, out of a
    board of ~50 walkable cells instead of 4275, at 2 MP a step instead of
    1.6, with no food and no rest.  Run it alongside a `scoremax` and the
    score gap is the price of the tunnels -- and `mp_per_surface_hex`,
    `dawns_below` and `stranded_dawns` say where that price was paid.
    """

    name = "subterranean"
    # Pushes deeper than a surface bot would: unseen corridor is the point.
    min_success = 0.35
    bank_greed = 0.50

    def below_target(self, obs):
        q, r = obs.pos()

        def value(cell, cq, cr, cost):
            if cost <= 0:
                return None
            v = 0.0
            if cell is None:
                v += UNSEEN_VALUE
            else:
                if is_hatch_terrain(cell.terrain):
                    return None            # stepping on it ends the dive
                if cell.poi:
                    v += POI_VALUE
                if cell.resource and not self.pack_full(obs):
                    v += PILE_VALUE
                if not cell.visited_by(obs.pid):
                    v += NEW_HEX_VALUE
                v += FRONTIER_WEIGHT * frontier_bonus(obs.tunnel, cq, cr)
            return None if v <= 0 else v / cost

        return best_target(obs.tunnel, q, r, value, max_cost=TUNNEL_SEARCH_COST,
                           stop_at=ends_journey)

    def hatch_bonus(self, obs, cell, q, r):
        """Prefer a hatch whose shaft we have never come out of.

        An unused pairing is both unexplored corridor and a piece of the map
        we cannot otherwise learn -- the surface-to-shaft table is never sent
        on the wire, only inferred from tun_in / tun_out.
        """
        v = super().hatch_bonus(obs, cell, q, r)
        idx = self._hatch_index_at(obs, q, r)
        if idx is None:
            v += 40.0                       # nobody has been seen using it
        elif idx not in self._shafts_used:
            v += 25.0
        return v

    @staticmethod
    def _hatch_index_at(obs, q, r):
        for h in obs.hatches.values():
            if (h.sq, h.sr) == (q, r):
                return h.idx
        return None
