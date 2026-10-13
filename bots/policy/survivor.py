"""Shared competence for the non-random policies.

Everything here is about *not losing* -- staying alive, keeping the pack
usable, and driving an encounter to a sane conclusion.  What a bot actually
chases is left to `pursue()` in the subclass, which is where the policies
differ and where the experiment lives.

The survival model, from dawnUpkeep() in survival_state.hpp:

  inv[RES_FOOD]  feeds the 0-6 food track: 1 token consumed per dawn
  inv[RES_WATER] feeds the 0-6 water track: 2 tokens consumed per dawn

Running a token to zero does not just stall the track, it steps it *down*,
and crossing a threshold costs LL.  So the bots budget tokens, not tracks.

Water is the real constraint.  ACT_WATER needs Marsh, Flooded or River
terrain -- about 4.7% of the map once the raft-gated River is excluded --
so most water has to come from walking onto piles.
"""
from dataclasses import replace

from config import (ACT_CRAFT, ACT_FORAGE, ACT_REST, ACT_SCAV, ACT_SHELTER,
                    ACT_WATER, can_salvage,
                    CANTEEN_ITEM, CANTEEN_RECIPE, EQUIPMENT, RECIPES,
                    TERR_SETTLEMENT, recipe_known,
                    EQUIP_STATS, INV_SLOTS_MAX, NAR_COLD_IMMUNE,
                    NAR_FIRE_STARTER, NAR_LAND_FORAGE, NAR_RIVER_FORAGE,
                    NAR_SCAV_DOUBLE, RES_FOOD, RES_SCRAP, RES_WATER,
                    ascend_cost, can_forage, gear_score, ground_days_left,
                    has_water, is_exposed, is_hatch_terrain,
                    KANE_CAMP, KANE_GONE, KANE_RANGE, neighbor)
from encounters import (EncounterLibrary, EncounterRun, can_pay,
                        has_for_choice, plan, success_chance)
import items as IT
from navigate import begin_cycle, best_target, dijkstra, end_cycle
from quests import QuestJournal, LEDGER
from .base import Policy, Action, ONCE
from .itemuse import ItemUseMixin

# Dawn eats 1 food + 2 water.  Keep a day of slack on top of that so a bot is
# never one bad step from a threshold break.
FOOD_FLOOR = 2
WATER_FLOOR = 4

# Below this many tokens, finding more overrides everything else.  Measured
# death spiral from the first four-policy run: water tokens hit 0 on day 2,
# the track then fell 6 -> 4 -> 1 over four days, LL followed it down, and
# because AP is ll + 3 the survivor lost the mobility it needed to go find
# water.  Dead on day 7.  The window to fix it is while AP is still high.
EMERGENCY_WATER = 1
EMERGENCY_FOOD = 0
# Hunting staples justifies a much longer walk than ordinary pursuit.
STAPLE_SEARCH_COST = 60
# Seconds between REST retries when we cannot tell whether the last one
# landed. Short enough to unstick a dropped REST within one dawn, long
# enough that it is nothing like the old per-cycle spam.
REST_RETRY_S = 4.0
# How far a survivor will walk to a Settlement to craft its canteen.  The
# errand runs only once the survival floor is quiet, and there are ~29
# Settlements on the 4275-hex map, so this is roughly "the nearest one we
# have seen, if it is not across the world".
CRAFT_SEARCH_COST = 40
# Seconds to stop asking after a craft that did not land.  doCraft() refuses
# with an err toast and no state change, so without this a refusal the bot
# did not predict (a full item grid, a recipe the board does not have) would
# repeat once a cycle forever -- the same shape as the stale-POI loop.
CRAFT_RETRY_S = 20.0
# Seconds before re-sending an identical equip_item.  equipItem() can refuse
# silently, and a socket the board has stopped servicing swallows it; with no
# cooldown gear_action() re-sent one swap 60 times in 25 s on 2026-09-23.
EQUIP_RETRY_S = 5.0
# The same guard for the two ground messages.  pickupGroundItem() and
# lootRemains() both refuse without changing anything -- a pile someone lifted
# a moment ago, tokens a rival looted first -- and until the ground_update that
# corrects our view arrives, the decision that sent them comes straight back.
PICKUP_RETRY_S = 5.0
LOOT_RETRY_S = 5.0
# A scene the board says is open for us (the "enc" flag, or an in_enc nack)
# whose enc_path never arrived.  handleMsg_enc_start() and the trap step send
# enc_path with a bare client->text(), which a socket whose queue is full
# drops; on a busy board it is (broadcastPartial in the thousands).  Without
# the biome and id there is no way to play the scene, and every m and act is
# refused in_enc -- on 2026-09-29 two bots froze like this and held the whole
# fleet's day open, since nobody in a scene rests.  Wait this long for a late
# enc_path, then enc_abort, retrying at most this often.
LOST_SCENE_GRACE_S = 2.0
LOST_SCENE_RETRY_S = 3.0
# Longest to wait for a choice's enc_res before assuming it was lost
# (EncounterRun.awaiting).
CHOICE_WAIT_S = 6.0
# Death drops (dropRemains): a fall leaves the whole pack on the hex, and the
# survivor who comes back for it is a fresh one.  How far it will walk: the
# same reach as the canteen errand, since the canteen it would otherwise
# rebuild from three scrap is usually lying on the grave.
RECOVER_SEARCH_COST = 40
# AP a day of walking is assumed to cover when asking whether a grave will
# still be there on arrival.  effectiveAP() is 6 + (ll+1)/2 -- about 10 fresh --
# so this errs short, which errs toward not setting off for a grave that ages
# out on the way.
RECOVER_AP_PER_DAY = 8
# Scrap-errand values: a pile is collected just by stepping on it; salvage
# terrain needs a 2 AP SCAV and a check, so it is worth less per AP.
SCRAP_PILE_VALUE = 100.0
SALVAGE_VALUE = 60.0
# How far to look for a way out when a surface policy falls down a hatch.
# The whole tunnel board is 16x10 and a corridor step costs 2 AP, so this
# reaches across all of it.
TUNNEL_ESCAPE_COST = 80
# Machine Gun Kane (kane.hpp).  A chapter hex we stood on without a scene
# opening -- somebody else is in it (one reader at a time), or a failed check
# threw us out -- is left alone this long before we come back and step on it
# again.  A scene only ever opens on arrival.
KANE_QUEST_RETRY_S = 20.0
# Shadowing him before the last chapter (kane_shadow): stay within this many
# hexes so his fire is a short walk at dusk, looking up to KANE_SHADOW_COST AP
# for a place to stand that he cannot hit.
KANE_SHADOW_FAR = 7
KANE_SHADOW_COST = 40


# Layer name -> method, for the names that are not a method already.  A
# subclass adds its own layers here (sentinel.py, biker.py) by name.
LAYER_METHOD = {
    "remains": "remains_action", "trade_reply": "trade_reply_action",
    "item": "item_action", "survival": "survival_action", "gear": "gear_action",
    "reveal": "reveal_action", "recover": "recover_action", "craft": "craft_action",
    "craft_extra": "craft_extra_action", "caravan": "caravan_action",
    "kane": "kane_story", "poi": "poi_action", "climb_out": "climb_out_action",
}


def token_room(me, res: int = -1) -> int:
    """Spare resource-token capacity — mirrors tokenRoomFor() in the firmware.

    `res` is the inv[] index being added.  Water also gets whatever canteen
    space is still empty: that sits outside the pack, so it is room even in a
    full or overfull one.  me.carried() is already tokenLoad().

    Only SCAVENGE is gated by this: FORAGE and WATER may overfill and pay the
    encumbrance penalty instead. Gating all three is what livelocked the first
    hardware run — four of five bots sat at room 0 asking for water they could
    not hold, 376 refused actions a minute, no AP spent and no way to tell.
    """
    room = max(0, me.inv_slots - me.carried())
    if res == RES_WATER:
        room += max(0, me.water_cap - me.inv[RES_WATER])
    return room


def ends_journey(cell, q, r) -> bool:
    """A hatch or shaft is enterable but never walked through: the crossing
    happens as the last act of the step that lands on it, so a route that
    merely passes over one puts the survivor on the other board instead.
    Hand this to dijkstra/best_target as `stop_at` on either board."""
    return cell is not None and is_hatch_terrain(cell.terrain)


class SurvivorPolicy(ItemUseMixin, Policy):
    """Base for scoremax / contentmax / coward / rival."""

    # --- knobs subclasses override -------------------------------------
    engage_encounters = True    # open a POI when standing on one
    # Play a scene by expected value over its node graph (encounters.plan):
    # odds, loot, what a failed roll costs, quest goals.  False is the old
    # rule -- the first open choice until a node is bankable, then bank --
    # kept as a baseline arm, since it is what every number in
    # docs/bot-testing.md before 2026-10-13 was measured with.
    plan_encounters = True
    # Scales every hazard in that plan: a coward prices a failed roll higher.
    encounter_risk = 1.0
    # Worth of reaching a node this bot has never seen (contentmax).
    novelty_bonus = 0.0
    # Let the quest journal steer: goal nodes and quest items are worth a
    # great deal to the planner, quest items are never dropped or used up,
    # and gate gear is worn.  Every bot keeps a journal; only these act on it.
    quest_drive = False
    # Deliberately send each requires_item choice once WITHOUT the item, to
    # check the board refuses it (no_item).  The quester's job, nobody else's.
    probe_gates = False
    # Does this policy know what to do underground?  Stepping onto a hatch IS
    # the descent -- there is no control to refuse and no confirmation -- and
    # terrain 12/13 costs 1 AP, which makes it the *cheapest* hex on the
    # surface map, so a pile-chasing pathfinder walks onto one sooner or
    # later.  A policy with no tunnel plan must climb straight back out
    # rather than flail on a board it is not reading.
    tunnel_capable = False
    min_success = 0.45          # do not take a branch below this success odds
    bank_greed = 0.60           # push on past a bankable node below this odds
    rest_below_ll = 2           # REST when LL drops to or below this
    pack_headroom = 1           # keep this many free slots for collecting
    # How deep to keep the pack. Defaults to the module floors; a policy that
    # has to carry a day's supplies somewhere there is nothing to eat raises
    # them, because the floor is also the only thing that makes a bot harvest
    # when it is standing on something harvestable.
    food_floor = FOOD_FLOOR
    water_floor = WATER_FLOOR
    # Build a basic shelter before sleeping on exposed ground.  Off by
    # default so the measured baseline stays comparable with earlier runs;
    # arena.py's --shelter turns it on for the other arm of the experiment.
    # It exists because the first cause-attributed runs put ~67% of all LL
    # lost on exposure, which no policy had any answer to -- measuring that
    # without this arm measures the bot's blind spot, not the game's balance.
    shelter_when_exposed = False
    # Every survivor gets a canteen and keeps it on: gather the scrap, craft
    # it at a Settlement, wear it, and never swap it back out.  Thirst was
    # every single death in the realtime runs of 2026-09-23, and the canteen
    # is the one piece of kit that answers it that anyone can make.  It
    # deliberately overrides gear_weights -- a policy that prefers the
    # Backpack's slots gives them up for the water.
    craft_canteen = True

    # Booby traps (traps.hpp): a forced scene with two doors. Work it -- the
    # Scavenge door -- when the odds are at least `trap_nerve` and LL is at
    # least `trap_min_ll`; otherwise back out. From a cache, press on once when
    # the next check is at least `trap_push` and LL has a point to spare.
    # Deliberately a player's judgement, not "always the first choice": the
    # brush-rate question is about what traps do to people who engage them.
    trap_nerve = 0.60
    trap_push = 0.60
    trap_min_ll = 4

    # Machine Gun Kane (kane.hpp, docs/machine-gun-kane.md). He asks before he
    # fires, and the burst comes a world tick (~15 s) later if we are still on
    # open ground within his reach -- so a survivor who listens never has to
    # be shot. `kane_dodge` is that: take cover when asked, and do not step
    # back into the open until the question is answered.
    #
    # His story is the world's: five chapters, each opened by stepping onto a
    # hex -- the camp he left at dawn (1-3), the house in the glass (4), his
    # own fire at night once that is read (5). `kane_quest` walks there when
    # it is within `kane_quest_reach` AP (None = whatever is left today, so it
    # is an errand on the way rather than an expedition); `kane_shadow` keeps
    # close to the jeep before the last chapter so the fire is a short walk at
    # dusk. Below `kane_min_ll` the story waits: a failed check costs LL.
    # Chapter 5 tells him the truth when the odds are at least
    # `kane_truth_nerve`, and the lie otherwise -- both end his road.
    kane_dodge = True
    kane_quest = True
    kane_quest_reach = None
    kane_shadow = False
    kane_min_ll = 3
    kane_truth_nerve = 0.50

    # The decision chain, top priority first.  Each name is a layer: a
    # method of `obs` returning an Action or None (LAYER_METHOD maps the
    # names), and the first Action wins; its `why` is prefixed with the
    # layer's name so the run log says which rule decided.  A subclass
    # reorders, drops or inserts by name -- the sentinel and the biker do --
    # instead of carrying its own copy of the chain.  The guards that run
    # before it (downed, a scene open, Kane's question) are in _decide().
    #
    # Why this order: everything that costs no AP sits first (a grave
    # underfoot, a trade that only holds while we share the hex, a
    # consumable that pays now), then the two errands bounded by today's
    # AP (a grave of ours, the canteen -- see recover_today), then the
    # survival floor, then Kane's fire (lit only at night), then the free
    # gear and map items, then the walking errands, and pursuit last.
    layers = ("remains", "trade_reply", "item", "recover_today", "craft_today",
              "survival", "kane_urgent", "gear", "reveal", "recover", "craft",
              "craft_extra", "caravan", "kane", "poi", "climb_out", "pursue")

    def __init__(self, rng, library: EncounterLibrary | None = None):
        super().__init__(rng)
        self.lib = library if library is not None else EncounterLibrary()
        self.run: EncounterRun | None = None
        # REST is idempotent server-side (doRest returns early if already
        # resting) but the periodic broadcast carries no `rt` field, so a
        # naive "rest while ap <= 0" check re-sends it every cycle forever.
        # Track it against the game-day instead; dawnUpkeep clears resting.
        self._rested_day = None
        self._last_rest_sent = 0.0
        # enc_start is silent when the POI has already been consumed, and our
        # local map keeps the stale poi bit until the next vis disk. Cap the
        # attempts per hex so a stale bit cannot become an infinite loop.
        self._poi_tries: dict[tuple[int, int], int] = {}
        # Content metrics -- ContentMax is judged on these rather than score,
        # since it will structurally lose a score race.
        self.stats = {"encounters_opened": 0, "encounters_banked": 0,
                      "encounters_aborted": 0, "rolls": 0, "rolls_won": 0,
                      "nodes_seen": set(), "recipes": 0, "downed": 0,
                      "crafted": 0,
                      # Death drops, as decided (not as acked): loot messages
                      # sent, and steps taken walking back to a grave of ours.
                      "loots": 0, "recover_moves": 0,
                      # Booby traps: forced scenes, which door, and how they ended.
                      "traps": 0, "trap_backouts": 0, "trap_worked": 0,
                      "trap_banked": 0, "trap_sprung": 0,
                      # Machine Gun Kane: questions put to us, the ones we got
                      # out of the open for, bursts taken, rounds that hit and
                      # LL they took; his chapters opened and read by us,
                      # steps walked toward them, steps spent shadowing him.
                      "kane_calls": 0, "kane_dodged": 0, "kane_bursts": 0,
                      "kane_hits": 0, "kane_ll_lost": 0, "kane_scenes": 0,
                      "kane_chapters": 0, "kane_quest_moves": 0,
                      "kane_shadow_moves": 0, "kane_ending": None}
        # Every send cooldown lives in self.throttle (base.Throttle): equip,
        # pickup, loot, craft, shelter, the lost-scene abort, Kane's chapter
        # hexes, and the item layer's.
        self._lost_since = None     # when a scene we cannot see was first noticed
        self._kane_dodged = None    # the question (its kane_call["t"]) last counted as dodged
        # (depth, q, r) hexes the board refused an act on for `terrain`: our
        # map is wrong about them (see on_reply).
        self._terrain_refused: set[tuple[int, int, int]] = set()
        self._items_init()
        # Quests (quests.py): every bot keeps a journal so any step any bot
        # stumbles through reaches the ledger; quest_drive lets it steer.
        self.journal = QuestJournal()
        self._obs = None            # the client's Observation, for event hooks
        self._probe = None          # (key, ci, sent_at) of a gate probe in flight
        self._last_item_result = None   # the item_result _book_craft last read
        self.stats.update({"quest_steps": 0, "gates_probed": 0, "plans": 0})

    def attach(self, flag=None, who=None) -> None:
        """BotClient hands over its oracle's flag() and its label, so the
        quest journal's sequencing checks become findings like any other."""
        if flag is not None:
            self.journal.flag = flag
        if who is not None:
            self.journal.who = who

    def protected_items(self, obs) -> set:
        return self.journal.protected_items()

    # --- event plumbing -------------------------------------------------
    def on_event(self, ev):
        # ev goes to every client via ws.textAll(), so without this filter a
        # bot counts every seated survivor's rolls as its own.
        if self.pid >= 0 and ev.get("pid") not in (None, self.pid):
            return
        k = ev.get("k")
        obs = self._obs
        day = obs.day if obs is not None else 0
        if k == "enc_res":
            self.stats["rolls"] += 1
            if ev.get("out"):
                self.stats["rolls_won"] += 1
            elif ev.get("trap"):
                self.stats["trap_sprung"] += 1
            if ev.get("rec"):
                self.stats["recipes"] += 1
            if self._probe is not None:
                # A roll answered our gate probe: the board ran a choice it
                # should have refused no_item.  journal.on_result books it.
                LEDGER.gates_probed[self._probe[0]] = "ACCEPTED without the item"
                self._probe = None
            if self.run is not None:
                self.run.on_result(ev)
                if self.run.node_key:
                    self.stats["nodes_seen"].add(
                        f"{self.run.biome}/{self.run.eid}/{self.run.node_key}")
                if obs is not None:
                    done = len(self.journal.done)
                    self.journal.on_result(self.run, ev, obs.me, day)
                    self.stats["quest_steps"] += len(self.journal.done) - done
                if ev.get("ends"):
                    self.run = None
        elif k == "enc_bank":
            self.stats["encounters_banked"] += 1
            if obs is not None:
                done = len(self.journal.done)
                if self.run is not None:
                    self.journal.on_bank(self.run, obs.me, day)
                if isinstance(ev.get("recs"), int) and ev["recs"]:
                    self.journal.on_recipes(ev["recs"], day)
                self.stats["quest_steps"] += len(self.journal.done) - done
            self.run = None
        elif k == "act" and ev.get("a") == ACT_CRAFT and ev.get("out") == 1:
            # A craft landed: the workshop's "made one" step, and no reason
            # to sit out craft_at_settlement's refusal cooldown for the next.
            self.throttle.clear("craft")
            if ev.get("ar"):
                done = len(self.journal.done)
                self.journal.on_craft(int(ev["ar"]), day)
                self.stats["quest_steps"] += len(self.journal.done) - done
        elif k == "enc_end":
            self.run = None
        elif k == "downed":
            self.stats["downed"] += 1
        elif k == "kane_call" and not ev.get("camp"):
            self.stats["kane_calls"] += 1
        elif k == "kane_fire":
            self.stats["kane_bursts"] += 1
            self.stats["kane_hits"] += ev.get("hits", 0) or 0
            self.stats["kane_ll_lost"] += ev.get("llLost", 0) or 0
        elif k == "kane_arc":
            self.stats["kane_chapters"] += 1
            if ev.get("ending") in ("truth", "lie"):
                self.stats["kane_ending"] = ev["ending"]
            if isinstance(ev.get("stage"), int):
                self.journal.on_kane(ev["stage"], True, day)

    def on_reply(self, cmd, ok, why):
        # The board says this hex is not what our map says it is (a FORAGE
        # or WATER refused `terrain`): a world event changed it under us
        # and no vis disk follows until we move.  Remember the hex, so the
        # harvest and the hunts stop asking -- the 2026-10-13 quester run had
        # two bots refused 483 times between them, standing still, starving.
        if not ok and cmd == "act" and why == "terrain" and self._obs is not None:
            self._terrain_refused.add((self._obs.me.depth,) + self._obs.pos())
        # A gate probe's verdict: no_item is the board doing its job.
        if self._probe is not None and cmd == "enc_choice":
            key = self._probe[0]
            if not ok:
                LEDGER.gates_probed[key] = "refused" if why == "no_item" else f"refused ({why})"
                self._probe = None
        # No scene is open on the board (see BotClient._on_reply): drop the
        # node tracker too, or the next scene with the same id resumes it.
        if not ok and why == "no_enc":
            self.run = None
        # A choice refused outright (can't pay, no item, gone) changed
        # nothing: strike it off so the next cycle picks something else.
        elif not ok and cmd == "enc_choice" and self.run is not None:
            self.run.refuse()

    def content_score(self) -> dict:
        s = dict(self.stats)
        s["nodes_seen"] = len(self.stats["nodes_seen"])
        return s

    # --- main entry -----------------------------------------------------
    def decide(self, obs) -> Action:
        # Whatever the layers below chose, never step back into Machine Gun
        # Kane's reach while his question is open (kane_hold).  Subclasses
        # override _decide(), or call super().decide(), so this covers them.
        self._obs = obs
        self.journal.check_arrivals(obs)
        self._book_craft(obs)
        # One decision, one search: the layers below ask navigate for the
        # same route up to eight times a cycle.  The memo lives exactly as
        # long as this call (see navigate.begin_cycle).
        begin_cycle()
        try:
            return self.kane_hold(obs, self._decide(obs))
        finally:
            end_cycle()

    def _book_craft(self, obs) -> None:
        """A craft the board confirmed, read off its item_result rather than
        the act event: the firmware sends both (`ar` on the event, `recipe`
        on the ack) but the mock sends only the ack, and the workshop's
        "made one" step must not depend on which board it is played on."""
        ir = obs.last_item_result
        if ir is None or ir is self._last_item_result:
            return
        self._last_item_result = ir
        if ir.get("act") == "craft" and ir.get("ok") and isinstance(ir.get("recipe"), int):
            self.throttle.clear("craft")
            if ir["recipe"] == CANTEEN_RECIPE:
                self.stats["crafted"] += 1
            done = len(self.journal.done)
            self.journal.on_craft(int(ir["recipe"]), obs.day)
            self.stats["quest_steps"] += len(self.journal.done) - done

    def _decide(self, obs) -> Action:
        """The guards, then the chain (`layers`)."""
        me = obs.me
        if not me.connected or me.ll == 0:
            return Action("noop", why="downed or disconnected")
        if obs.encounter is not None:
            self._lost_since = None
            return self.decide_encounter(obs)
        if me.in_encounter:
            return self.lost_scene_action(obs)
        self._lost_since = None
        # Machine Gun Kane has asked us and we are still in the open: the
        # burst is one world tick away, and nothing below is worth up to 2 LL.
        act = self.kane_cover_action(obs)
        if act is not None:
            return act
        return self.run_layers(obs)

    def run_layers(self, obs) -> Action:
        """The first layer in `layers` with something to do decides.
        Resolved by name on every call, not once, so a test or a driver that
        replaces a method on the instance (quest_check.py pins `pursue`) is
        honoured."""
        for name in self.layers:
            act = getattr(self, LAYER_METHOD.get(name, name))(obs)
            if act is None:
                continue
            if act.why and not act.why.startswith(name + ":"):
                act = replace(act, why=f"{name}: {act.why}")
            return act
        return Action("noop", why="no layer had anything to do")

    # --- the layers that are not a plain method already -------------------
    def recover_today(self, obs) -> Action | None:
        """A grave of our own within today's AP, ahead of the survival
        floor: the canteen is usually lying on it.  Not in an emergency."""
        if self.in_emergency(obs):
            return None
        return self.recover_action(obs, max_cost=obs.me.ap)

    def craft_today(self, obs) -> Action | None:
        """A canteen within today's AP outranks topping up the pack.  The
        top-up floor is often above what the economy sustains, so on hardware
        it ate every AP of every day and a survivor sat on a pond 4 hexes
        from a Settlement for ~100 s holding the scrap (2026-09-23).
        Emergencies still win -- this only pre-empts the routine harvest."""
        if self.in_emergency(obs):
            return None
        return self.craft_action(obs, max_cost=obs.me.ap)

    def kane_urgent(self, obs) -> Action | None:
        """His fire is lit only at night and for two world ticks after dawn,
        so the last chapter goes ahead of every errand that can wait a day."""
        return self.kane_quest_action(obs, urgent=True)

    def kane_story(self, obs) -> Action | None:
        """The camp he left, the house in the glass, or (kane_shadow)
        staying near him until his fire is lit."""
        return self.kane_quest_action(obs)

    def poi_action(self, obs) -> Action | None:
        """Standing on an unopened POI: take it, whatever the policy is.
        Walking past one is pure waste -- POIs are consumed permanently, so
        the alternative is leaving it for a rival -- and exercising the
        encounter content is the whole point of these runs.
        handleMsg_enc_start() is board-aware, so this works on either board."""
        if not self.engage_encounters:
            return None
        here = obs.here()
        if here is not None and here.poi and obs.me.ap > 0:
            return self.try_open_poi(obs, "POI underfoot")
        return None

    def climb_out_action(self, obs) -> Action | None:
        """A surface policy that fell down a hatch climbs straight back out
        rather than flailing on a board it is not reading."""
        if obs.underground and not self.tunnel_capable:
            return self.climb_out(obs, why="fell down a hatch")
        return None

    # --- equipment ------------------------------------------------------
    # What this policy wants out of a piece of gear.  One weight per stat that
    # items.cfg can carry; `nar` weights the narrative perks by NAR_* id.
    # Subclasses override to the degree they actually play differently -- the
    # point is that a run measures which gear suits which playstyle, not which
    # gear happened to drop first.  gear_score() in config.py does the maths.
    #
    # The default is the survival floor this class already encodes: staying
    # alive first, then the mobility to go fix a problem.
    gear_weights = {
        "ll": 3.0, "rad": 1.5, "ap": 1.5, "slots": 1.0,
        "vision": 1.0, "threat": 0.5, "terrain": 0.5, "water_cap": 3.0,
        "nar": {NAR_COLD_IMMUNE: 4.0, NAR_FIRE_STARTER: 2.0,
                NAR_LAND_FORAGE: 1.5, NAR_RIVER_FORAGE: 0.5,
                NAR_SCAV_DOUBLE: 1.0},
    }

    # Put an item in an empty slot even when gear_score() says it is not
    # worth wearing. Off by default -- a survival policy should leave a
    # net-negative item in the pack -- but a policy built to measure whether
    # penalties are actually applied has to be willing to wear one, or the
    # penalty is never observed. Only affects EMPTY slots; a swap still has
    # to justify itself.
    equip_anything = False

    def gear_swap_ok(self, obs, worn_id, cand_id) -> bool:
        """Would equipItem() actually accept this swap?

        The orphan guard refuses a swap that shrinks the pack around occupied
        slots.  Asking first matters: gear_action runs every cycle, so a swap
        the server will always refuse is an infinite retry at one message per
        cycle that never looks like an error from either end.
        """
        me = obs.me
        delta = (EQUIP_STATS.get(cand_id, {}).get("slots", 0)
                 - EQUIP_STATS.get(worn_id, {}).get("slots", 0))
        if delta >= 0:
            return True
        new_slots = max(1, min(INV_SLOTS_MAX, me.inv_slots + delta))
        # The incoming item vacates its own slot, so it is exempt -- same
        # allowance equipItem() makes.
        return not any(t for i, t in enumerate(me.inv_type)
                       if i >= new_slots and t != cand_id)

    def gear_action(self, obs):
        """Pick up equipment underfoot, then put on anything carried whose
        slot is still empty.

        The pickup half matters more than it looks: grantItemOrDrop() puts
        loot that does not fit the pack on the ground instead, and until the
        bots could pick it up again that gear was gone for good -- a full pack
        at the wrong moment silently deleted the drop from the experiment.

        Deliberately conservative: it fills empty slots and never swaps.  A
        swap needs a value judgement between two items, and getting that wrong
        is worse than wearing the first one found -- equipItem() also returns
        the displaced item to the pack, so a bad swap policy can thrash a slot
        forever, one message per cycle, and never notice.

        Nothing here existed before.  The bots parsed eq[] out of every state
        message and never sent a single equip_item, so every number in
        docs/bot-testing.md was measured on a survivor wearing nothing at all:
        no +LL ceiling, no vision, no carry slots, no fuel-gated AP.
        """
        me = obs.me
        if me.in_encounter:
            return None

        act = self.pickup_gear(obs)
        if act is not None:
            return act

        # The canteen goes on first, over whatever holds the body slot, and
        # is pinned there: the loop below never proposes a swap out of it.
        # Without the pin a Backpack-preferring policy would take it straight
        # back off, and equipItem() returning the displaced item to the pack
        # would make the pair trade places once a cycle forever.
        ceslot = EQUIPMENT.get(CANTEEN_ITEM) if CANTEEN_ITEM else None
        if (self.craft_canteen and ceslot is not None
                and me.equip[ceslot] != CANTEEN_ITEM
                and CANTEEN_ITEM in me.inv_type):
            slot = me.inv_type.index(CANTEEN_ITEM)
            worn = me.equip[ceslot]
            if not worn or self.gear_swap_ok(obs, worn, CANTEEN_ITEM):
                verb = f"swap {worn} for" if worn else "equip"
                return self._equip(slot, CANTEEN_ITEM,
                                   f"{verb} canteen -> slot {ceslot}")

        # A quest gate that wants gear WORN (the Dirty Glove at the
        # Perambulator's tyre): put it on, whatever it scores, and pin it
        # below until the gate is done.  Never over the canteen.
        pinned = self.quest_pinned(obs)
        for item_id in sorted(pinned):
            eslot = EQUIPMENT.get(item_id)
            if eslot is None or me.equip[eslot] == item_id or item_id not in me.inv_type:
                continue
            worn = me.equip[eslot]
            if worn and (worn == CANTEEN_ITEM or worn in pinned
                         or not self.gear_swap_ok(obs, worn, item_id)):
                continue
            return self._equip(me.inv_type.index(item_id), item_id,
                               f"quest: wear the {IT.name(item_id)} for its gate")

        # Best carried candidate per equip slot, against whatever is worn.
        # Strictly greater, so equal-scoring items never trade places -- a tie
        # that swapped would put the loser straight back in the pack and swap
        # again next cycle, forever.
        best = None
        for slot, item_id in enumerate(me.inv_type):
            if not item_id:
                continue
            eslot = EQUIPMENT.get(item_id)
            if eslot is None:
                continue
            worn = me.equip[eslot]
            if self.craft_canteen and worn and worn == CANTEEN_ITEM:
                continue                    # pinned
            if worn and worn in pinned:
                continue                    # a quest gate still needs it on
            gain = gear_score(item_id, self.gear_weights) - gear_score(worn, self.gear_weights)
            if gain <= 0 and not (self.equip_anything and not worn):
                continue
            if worn and not self.gear_swap_ok(obs, worn, item_id):
                continue
            if best is None or gain > best[0]:
                best = (gain, slot, item_id, eslot, worn)
        if best is None:
            return None
        _, slot, item_id, eslot, worn = best
        verb = f"swap {worn} for" if worn else "equip"
        return self._equip(slot, item_id, f"{verb} item {item_id} -> slot {eslot}")

    def quest_pinned(self, obs) -> set:
        """Gear a quest gate needs worn, that we hold (quest_drive only)."""
        if not self.quest_drive:
            return set()
        me = obs.me
        need = {s.needs for s in self.journal.open_steps(obs.world) if s.worn and s.needs}
        return {i for i in need if i in me.inv_type or i in me.equip}

    @staticmethod
    def pack_has_slot(me) -> bool:
        """A free typed slot -- what pickupGroundItem() needs for an item that
        does not stack onto one already carried."""
        return sum(1 for t in me.inv_type if t) < me.inv_slots

    def pickup_gear(self, obs, why="pick up gear {id} underfoot") -> Action | None:
        """Equipment lying on this hex, and room to take it.

        pickupGroundItem() refuses a different hex, a pack with no space, and
        (like every ground pile) a survivor underground, whose q/r are pinned
        to the hatch overhead -- so check all three here rather than firing a
        message that can only be refused.  Non-equipment is left where it is:
        only RivalPolicy wants litter.
        """
        me = obs.me
        if me.depth or not self.pack_has_slot(me):
            return None
        quest = self.protected_items(obs) | set(self.journal.item_bonus(me))
        for gi in obs.ground_items:
            if gi.get("id") not in EQUIPMENT and gi.get("id") not in quest:
                continue
            if (gi.get("q"), gi.get("r")) != (me.q, me.r):
                continue
            act = self._pickup(gi, why.format(id=gi["id"]))
            if act is not None:
                return act
        return None

    def _pickup(self, gi, why) -> Action | None:
        """pickup_item for ground pile `gi`, at most once per PICKUP_RETRY_S
        for the same (gslot, item) -- see PICKUP_RETRY_S."""
        if not self.throttle.ok(("pickup", gi.get("g", 0), gi.get("id")), PICKUP_RETRY_S):
            return None
        return Action("pickup_item", gslot=gi.get("g", 0), why=why)

    def _equip(self, slot, item_id, why) -> Action | None:
        """equip_item, at most once per EQUIP_RETRY_S for the same (slot,
        item).  A refusal or a dead socket leaves the pack unchanged, so the
        same decision comes back every cycle; this is what stops it being
        re-sent every cycle."""
        if not self.throttle.ok(("equip", slot, item_id), EQUIP_RETRY_S):
            return None
        return Action("equip_item", slot=slot, why=why)

    # --- crafting -------------------------------------------------------
    @staticmethod
    def owns(me, item_id) -> bool:
        return bool(item_id) and (item_id in me.equip or item_id in me.inv_type)

    def wants_canteen(self, obs) -> bool:
        """Known and not already owned.  Not weighed against the body slot:
        every policy wears one (see craft_canteen)."""
        me = obs.me
        if not self.craft_canteen or CANTEEN_RECIPE is None:
            return False
        if not recipe_known(me.known_recipes, CANTEEN_RECIPE):
            return False
        return not self.owns(me, CANTEEN_ITEM)

    @staticmethod
    def can_afford(me, rid) -> bool:
        rec = RECIPES.get(rid)
        if rec is None:
            return False
        if any(me.inv[i] < c for i, c in enumerate(rec["cost"])):
            return False
        for item, qty in rec["mats"]:
            if sum(q for t, q in zip(me.inv_type, me.inv_qty) if t == item) < qty:
                return False
        return True

    def in_emergency(self, obs) -> bool:
        """The states the staple hunt and the critical-LL rest exist for."""
        me = obs.me
        return (me.inv[RES_WATER] <= EMERGENCY_WATER
                or me.inv[RES_FOOD] <= EMERGENCY_FOOD
                or me.ll <= self.rest_below_ll)

    def craft_action(self, obs, max_cost=CRAFT_SEARCH_COST) -> Action | None:
        """Walk to a Settlement and craft the canteen.

        `max_cost` bounds the walk: decide() calls this with today's AP ahead
        of the survival floor, and with CRAFT_SEARCH_COST after it.

        doCraft(): Settlement terrain, 1 AP, the recipe bit, the tokens, and
        item room for the result -- and refused underground outright.  The
        crafted item lands in the pack; gear_action() puts it on.
        """
        me = obs.me
        if obs.underground or me.ap < 1 or not self.wants_canteen(obs):
            return None
        # The canteen we would craft is lying on a grave of ours that we can
        # still reach: walk back for it (recover_action) rather than spend
        # three scrap on a second one.  Only when the grave holds one -- a
        # grave of tokens alone does not make crafting pointless.
        if self.recovery_target(obs, canteen_only=True) is not None:
            return None
        if not self.can_afford(me, CANTEEN_RECIPE):
            return self.scrap_errand(obs, max_cost)
        return self.craft_at_settlement(obs, CANTEEN_RECIPE, max_cost, "canteen",
                                        f"craft canteen ({me.inv[RES_SCRAP]} scrap)")

    @staticmethod
    def result_fits(me, rid) -> bool:
        """Would applyRecipe()'s invRoomFor check pass?  It looks at the pack
        AFTER the materials come out, so a material whose last unit sits in a
        slot frees that slot for the result.  Every craftable here stacks 1,
        so the result needs an empty slot."""
        rec = RECIPES.get(rid)
        if rec is None:
            return False
        types, qtys = list(me.inv_type), list(me.inv_qty)
        for item, need in rec["mats"]:
            for s in range(len(types)):
                if need <= 0:
                    break
                if types[s] != item:
                    continue
                take = min(need, qtys[s])
                qtys[s] -= take
                need -= take
                if qtys[s] <= 0:
                    types[s] = 0
        return any(not t for t in types[:me.inv_slots])

    def craft_at_settlement(self, obs, rid, max_cost, label, why) -> Action | None:
        """CRAFT `rid` here if this is a Settlement, else walk to the nearest
        one within `max_cost`.  The caller has checked the recipe is known and
        affordable; this checks the rest of doCraft(): the surface, 1 AP, and
        room for the result.  `label` prefixes the walk's why, `why` is the
        craft's."""
        me = obs.me
        if obs.underground or me.ap < 1 or not self.result_fits(me, rid):
            return None
        if not self.throttle.ready("craft", CRAFT_RETRY_S):
            return None
        cell = obs.here()
        if cell is not None and cell.terrain == TERR_SETTLEMENT:
            self.throttle.mark("craft")
            return Action("act", a=ACT_CRAFT, recipe=rid, why=why)
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(c, q, r, cost):
            if cost <= 0 or c is None or c.terrain != TERR_SETTLEMENT:
                return None
            return 100.0 / cost

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=max_cost, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, _ = target
        return Action("move", d=d, why=f"{label}: to settlement ({q},{r}) c={cost}")

    def scrap_errand(self, obs, max_cost) -> Action | None:
        """Short of the canteen's scrap: salvage here, or walk to a scrap pile
        or salvageable ground.  A pile pays on contact; salvage costs 2 AP and
        a check (and a threat tick on ruins).  Both need token room --
        collectResource() and doScav() refuse a full pack."""
        me = obs.me
        if token_room(me, RES_SCRAP) <= 0:
            return None
        cell = obs.here()
        if (cell is not None and can_salvage(cell.terrain) and me.ap >= 2
                and not self.terrain_doubtful(obs, me.q, me.r)):
            return Action("act", a=ACT_SCAV, ap=2,
                          why=f"canteen: salvaging scrap ({me.inv[RES_SCRAP]} held)")
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(c, q, r, cost):
            if cost <= 0 or c is None or self.terrain_doubtful(obs, q, r):
                return None
            if c.resource == RES_SCRAP + 1:
                return SCRAP_PILE_VALUE / cost
            if can_salvage(c.terrain):
                return SALVAGE_VALUE / cost
            return None

        target = best_target(obs.map, me.q, me.r, value,
                             max_cost=max_cost, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, _ = target
        return Action("move", d=d, why=f"canteen: scrap -> ({q},{r}) c={cost}")

    # --- death drops ----------------------------------------------------
    # A downed survivor leaves everything it carried on the hex where it fell
    # (dropRemains, inventory_items.hpp): pack and worn gear as ordinary
    # ground piles, resource tokens in a remains record ("rm").  It is anyone's
    # for GROUND_AGE_DAYS.  Before this the bots lost their canteen to every
    # death -- respawn is a fresh survivor -- and had to scrape together the
    # scrap for another; now it waits on the grave.
    @staticmethod
    def room_for_remains(me, rm) -> bool:
        """Some kind of token on the record that the pack could take.
        lootRemains() caps each kind at tokenRoomFor(), so without room a loot
        is refused and changes nothing."""
        res = (rm.get("res") or [])[:5]
        return any(n > 0 and token_room(me, k) > 0 for k, n in enumerate(res))

    @staticmethod
    def is_my_grave(obs, rm) -> bool:
        """Our seat's record, or one a "fell" of ours created -- a respawn
        can land in another seat, and the old pid then names someone else."""
        return (rm.get("pid") == obs.pid
                or (rm.get("q"), rm.get("r")) in obs.my_graves)

    def worth_recovering(self, obs, rm) -> bool:
        """Is there anything on this grave we could actually take?

        Only what the two pickups will lift counts: tokens we have room for,
        and equipment with a free slot to put it in.  Litter the policy would
        leave there anyway is not worth the walk -- counting it would bounce
        a bot between the grave and wherever pursuit wanted it next.
        """
        me = obs.me
        if self.room_for_remains(me, rm):
            return True
        if not self.pack_has_slot(me):
            return False
        at = (rm.get("q"), rm.get("r"))
        return any(gi.get("id") in EQUIPMENT and (gi.get("q"), gi.get("r")) == at
                   for gi in obs.ground_items)

    def remains_action(self, obs) -> Action | None:
        """Stood on a grave: take what is on it before anything moves us off.

        Both halves are free -- no AP, no check -- which is why this runs
        ahead of every errand.  It has to: the canteen errand would otherwise
        walk a respawned survivor off its own grave to hunt scrap for a NEW
        canteen before gear_action() got round to lifting the old one, and
        recover_action() would then walk it straight back.

        Any grave, ours or a rival's.  Tokens first (a bare loot takes what
        fits, in inv[] order, so water first), then equipment.  Never below:
        remains are surface coordinates, and underground me.q/me.r are the
        hatch overhead -- which is exactly where a fall below leaves its grave.
        """
        me = obs.me
        if me.depth or me.in_encounter:
            return None
        rm = obs.remains_at(me.q, me.r)
        if rm is None:
            return None
        if self.room_for_remains(me, rm):
            if self.throttle.ok(("loot", me.q, me.r), LOOT_RETRY_S):
                self.stats["loots"] += 1
                whose = "our own" if self.is_my_grave(obs, rm) else f"{rm.get('nm') or 'someone'}'s"
                return Action("loot", why=f"loot {whose} remains {rm.get('res')}")
        return self.pickup_gear(obs, why="recover gear {id} from the grave")

    def recovery_target(self, obs, max_cost=RECOVER_SEARCH_COST,
                        canteen_only=False, item=None):
        """The grave of ours worth walking back to, as best_target()'s
        (q, r, dir, cost, value), or None.

        A grave counts while it holds something we can take
        (worth_recovering) and will still be there when we arrive: it goes at
        the dawn that makes it GROUND_AGE_DAYS old, and a walk longer than
        today's AP arrives RECOVER_AP_PER_DAY at a time.  Never one on a
        hatch -- stepping onto a hatch IS the descent, so a surface grave on
        one cannot be reached on foot (a fall underground leaves it there).
        `canteen_only` narrows it to graves with the canteen on them, and
        `item` to graves with that item on them (the Biker's bike).
        """
        if canteen_only:
            item = CANTEEN_ITEM
        me = obs.me
        if me.depth or me.ap < 1 or not obs.remains:
            return None
        graves = {}
        for rm in obs.remains:
            at = (rm.get("q"), rm.get("r"))
            if None in at or at == (me.q, me.r):
                continue                # malformed, or remains_action's job
            if not self.is_my_grave(obs, rm) or not self.worth_recovering(obs, rm):
                continue
            if item is not None and not any(
                    gi.get("id") == item and (gi.get("q"), gi.get("r")) == at
                    for gi in obs.ground_items):
                continue
            cell = obs.map[at]
            if cell is not None and is_hatch_terrain(cell.terrain):
                continue
            graves[at] = rm
        if not graves:
            return None
        ap_today = me.ap

        def value(cell, q, r, cost):
            rm = graves.get((q, r))
            if rm is None or cost <= 0:
                return None
            left = ground_days_left(obs.day, rm.get("d", obs.day))
            if cost <= ap_today:
                if left < 1:
                    return None
            else:
                days = -(-(cost - ap_today) // RECOVER_AP_PER_DAY)
                if left <= days + 1:    # a day of slack: AP is an estimate
                    return None
            return 100.0 / cost

        return best_target(obs.map, me.q, me.r, value,
                           max_cost=max_cost, stop_at=ends_journey)

    def recover_action(self, obs, max_cost=RECOVER_SEARCH_COST) -> Action | None:
        """Walk back toward a grave of ours (recovery_target).

        decide() calls this twice, the way it calls craft_action(): with
        today's AP ahead of the survival floor, and with RECOVER_SEARCH_COST
        once the floor is quiet.  Arriving is the whole errand --
        remains_action() takes it from there.
        """
        target = self.recovery_target(obs, max_cost)
        if target is None or target[2] not in obs.me.legal_dirs():
            return None
        q, r, d, cost, _ = target
        self.stats["recover_moves"] += 1
        return Action("move", d=d, why=f"recover: our grave at ({q},{r}) c={cost}")

    # --- survival floor -------------------------------------------------
    def survival_action(self, obs) -> Action | None:
        me = obs.me
        # obs.here() rather than obs.map[(me.q, me.r)]: underground those
        # coordinates still name the surface hatch, so the unqualified read
        # decides whether to forage based on a hex 30 metres overhead.
        cell = obs.here()
        terr = cell.terrain if cell else 0
        if self.terrain_doubtful(obs, *obs.pos()):
            terr = -1               # the board refused this hex: harvest nothing here

        # Harvest right here first -- always cheaper than walking.  A worn
        # canteen raises the target by its capacity, but only while there is
        # room to put the water: canteen space is outside the pack, and it is
        # the whole reason to carry one -- stood on a pond with it empty is
        # the moment it pays for itself.
        water_goal = self.water_floor
        if me.water_cap and token_room(me, RES_WATER) > 0:
            water_goal += me.water_cap
        if me.inv[RES_WATER] < water_goal and has_water(terr) and me.ap >= 1:
            why = (f"water {me.inv[RES_WATER]} low" if me.inv[RES_WATER] < self.water_floor
                   else f"filling the canteen ({me.inv[RES_WATER]}/{water_goal})")
            return Action("act", a=ACT_WATER, ap=min(2, me.ap), why=why)
        if me.inv[RES_FOOD] < self.food_floor and can_forage(terr) and me.ap >= 2:
            return Action("act", a=ACT_FORAGE,
                          why=f"food {me.inv[RES_FOOD]} low")

        # About to sleep on exposed ground: 1 scrap and 1 AP buys a basic
        # shelter, and dawnUpkeep does more than cancel the exposure hit --
        # "resting in shelter suppresses all LL losses" zeroes llDelta
        # outright, thirst and hunger included.  It pays 4 points on top and
        # cannot fail.
        #
        # Deliberately ahead of the staple hunt: on the last AP of a day, one
        # more step cannot reach water (most terrain costs 2-4 AP to enter)
        # but the shelter cancels the whole night's loss, so building strictly
        # dominates walking there.  The hunt still outranks it on any earlier
        # move, which is where it actually saves a survivor.
        shelter = self.shelter_here(obs)
        if shelter is not None:
            return shelter

        # Out of staples: go and get some while there is still AP to do it
        # with.  This deliberately outranks resting -- dawn consumes 1 food
        # and 2 water whether you moved or not, so resting through a shortage
        # makes it strictly worse.
        hunt = self.staple_hunt(obs)
        if hunt is not None:
            return hunt

        # Out of AP: resting both heals at dawn and, when every connected
        # player is resting, ends the day immediately (tickGame).  This holds
        # underground too now -- REST is no longer refused at depth 1, so a
        # bot down a hole still pulls its weight in sprint mode instead of
        # holding the whole fleet's day open.
        if me.ap <= 0:
            return self.rest_once(obs, "out of AP underground"
                                       if obs.underground else "out of AP")

        # At critical LL the two boards want opposite things.  On the surface
        # a rest is free upside.  Underground it rolls TUNNEL_REST_LL_PCT for
        # bad air, and that point is netted against the rest-heal -- so it is
        # harmless while we still have the food and water to heal with, and it
        # is the thing that kills us when we do not.  Rest below only when the
        # heal will actually fire (survival_state.hpp: F>=2 and W>=2).
        if me.ll <= self.rest_below_ll:
            if not obs.underground:
                return self.rest_once(obs, f"LL {me.ll} critical")
            if me.food >= 2 and me.water >= 2:
                return self.rest_once(
                    obs, f"LL {me.ll} critical, stocked enough to out-heal bad air")

        return None

    def terrain_doubtful(self, obs, q: int, r: int) -> bool:
        """Did the board refuse an act on this hex of this board for
        `terrain`?  Then our cell for it is stale and it is no harvest."""
        return (obs.me.depth, q, r) in self._terrain_refused

    def wants_shelter(self, obs) -> bool:
        """Subclass hook: may shelter_here() spend scrap on a roof tonight?
        The --shelter experiment arm by default; the Biker also spends the
        scrap its build does not need."""
        return self.shelter_when_exposed

    def shelter_here(self, obs) -> Action | None:
        """Build a basic shelter if this is where the day is going to end.

        doShelter() takes 1 scrap and 1 AP and always succeeds -- there is no
        check to fail -- so the only judgement is *when*.  Firing it on the
        last AP of the day is what a player does: earlier wastes a move that
        could have been a step, later is a dawn too late.
        """
        me = obs.me
        if not self.wants_shelter(obs):
            return None
        # SHELTER is one of the three actions still refused at depth 1, and
        # the exposure check it answers is off underground anyway -- a
        # corridor is already cover.
        if obs.underground:
            return None
        # Archetype 5 (Endurer) needs no shelter -- dawnUpkeep exempts it, so
        # building one is a wasted scrap.
        if me.archetype == 5:
            return None
        if me.ap < 1 or me.inv[RES_SCRAP] < 1:
            return None
        # Only on the last move of the day, or when already hurt enough that
        # another exposure tick is the difference.
        if me.ap > 1 and me.ll > self.rest_below_ll:
            return None
        cell = obs.here()
        if cell is None or cell.shelter > 0 or not is_exposed(cell.terrain):
            return None
        # Once per hex per day.  A shelter that lands shows on the act
        # event's cnd, so the cell.shelter check above stops the next one; a
        # refusal the bot did not predict changes nothing, and at low LL this
        # fires on every AP, so without it one refusal ate the whole day.
        # The mock has no doShelter() at all, which is how it was found.
        if not self.throttle.ok(("shelter", obs.day, me.q, me.r), ONCE):
            return None
        return Action("act", a=ACT_SHELTER,
                      why=f"exposed at SV<2, {me.inv[RES_SCRAP]} scrap, ap={me.ap}")

    def staple_hunt(self, obs) -> Action | None:
        """Walk toward water or food when critically short of either.

        Water is the hard one: ACT_WATER needs Marsh, Flooded or River, about
        4.7% of the map once the raft-gated River is excluded, so most water
        has to come from piles.  Searches much further than normal pursuit
        because dying of thirst two hexes from a pond is the failure mode this
        exists to prevent.
        """
        me = obs.me
        dry = me.inv[RES_WATER] <= EMERGENCY_WATER
        starving = me.inv[RES_FOOD] <= EMERGENCY_FOOD
        if not (dry or starving) or me.ap <= 0:
            return None
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(cell, q, r, cost):
            if cost <= 0 or cell is None or self.terrain_doubtful(obs, q, r):
                return None
            v = 0.0
            if dry:
                if cell.resource == RES_WATER + 1:
                    v += 100.0
                if has_water(cell.terrain):
                    v += 70.0
            if starving:
                if cell.resource == RES_FOOD + 1:
                    v += 80.0
                if can_forage(cell.terrain):
                    v += 40.0
            v += self.staple_value(obs, cell, dry, starving)
            return None if v <= 0 else v / cost

        # Hunt on whichever board we are on.  Underground that means Tunnel
        # Floor cistern seeps and water piles only -- TERRAIN_FORAGE_DN[14] is
        # 0, so there is no food down there at all, which is exactly why the
        # tunnel policies have to surface for it.
        my_q, my_r = obs.pos()
        # stop_at: a route *through* a hatch does not exist (it ends on the
        # other board), so an emergency walk must never be planned across one.
        target = best_target(obs.board, my_q, my_r, value,
                             max_cost=STAPLE_SEARCH_COST, stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, val = target
        need = "water" if dry else "food"
        return Action("move", d=d, why=f"EMERGENCY {need} -> ({q},{r}) c={cost}")

    def staple_value(self, obs, cell, dry: bool, starving: bool) -> float:
        """Subclass hook: extra worth of `cell` to an emergency staple hunt.
        The tunnel policies count a hatch as water."""
        return 0.0

    def rest_once(self, obs, why: str) -> Action | None:
        """REST, with a cooldown rather than a once-per-day latch.

        Two failure modes to thread between.

        Re-sending REST every decision cycle is a no-op server-side (doRest
        returns early when already resting) but burns a message each time --
        2054 dead sends in one early run.

        A strict once-per-day latch is worse.  The decision is latched before
        the send clears the rate limiter, so a REST decided just before a
        reconnect never reaches the board while the bot still believes it
        rested.  It then sits at ap == 0, *not* resting, unable to move and
        unable to retry until dawn -- and because tickGame() only ends the day
        early when every connected player is resting, one bot stuck like that
        stops the whole fleet's day from collapsing.  Observed live: five bots
        frozen at day 16, days reverting from ~3 s to the full 5 minutes.

        There is no way to confirm it landed: the periodic broadcast carries no
        `rt` field, so "resting" and "simply out of AP" look identical.  So
        retry on a cooldown instead.  REST is idempotent, and one send every
        few seconds is 60x less traffic than the original spam while being
        self-healing.
        """
        if obs.me.resting:
            return None
        # No depth guard: REST works at depth 1. It is not free down there --
        # dawnUpkeep rolls TUNNEL_REST_LL_PCT for bad air -- but that is the
        # caller's judgement, not this sender's, and the caller that cares
        # makes it (the critical-LL branch in emergency()).
        now = self.clock()
        if (self._rested_day == obs.day
                and now - self._last_rest_sent < REST_RETRY_S):
            return None
        self._rested_day = obs.day
        self._last_rest_sent = now
        return Action("act", a=ACT_REST, why=why)

    def pack_full(self, obs) -> bool:
        me = obs.me
        return me.carried() >= max(0, me.inv_slots - self.pack_headroom)

    def try_open_poi(self, obs, why: str) -> Action | None:
        """Send enc_start for the hex underfoot, at most a few times.

        A consumed POI makes enc_start a silent no-op while our cached map
        still shows the bit set, so this needs its own attempt cap rather than
        trusting the map.
        """
        q, r = obs.pos()
        # Key on the board as well: (3,4) underground and (3,4) on the surface
        # are different hexes, and a shared attempt counter would let a dead
        # tunnel POI veto a live surface one.
        key = (obs.me.depth, q, r)
        tries = self._poi_tries.get(key, 0)
        if tries >= 2:
            return None
        self._poi_tries[key] = tries + 1
        # q/r are required and are validated against our own position --
        # against tq/tr while underground (handleMsg_enc_start is board-aware),
        # which is exactly what obs.pos() returns.
        return Action("enc_start", q=q, r=r, why=why)

    # --- encounters -----------------------------------------------------
    def ensure_run(self, obs) -> None:
        """Bind the open encounter to its local JSON the first time we see it."""
        enc = obs.encounter or {}
        biome, eid = enc.get("biome"), enc.get("id")
        if biome is None or eid is None:
            return
        if self.run is None or (self.run.biome, self.run.eid) != (biome, eid):
            self.run = EncounterRun(self.lib, biome, eid)
            self.run.trap = bool(enc.get("trap"))
            # A Machine Gun Kane chapter (enc_path "kane": n, kane.hpp): opened
            # by arriving, like a trap, and not a POI -- counted on its own.
            self.run.kane = int(enc.get("kane") or 0)
            self.stats["traps" if self.run.trap else
                       "kane_scenes" if self.run.kane else "encounters_opened"] += 1
            if self.run.node_key:
                self.stats["nodes_seen"].add(f"{biome}/{eid}/{self.run.node_key}")
            self.journal.on_open(self.run, obs.day)

    def lost_scene_action(self, obs) -> Action:
        """The board has a scene open for us that we never heard about (see
        LOST_SCENE_GRACE_S): give its enc_path a moment, then walk away.  A
        trap's start is a back-out roll, and a failed one nacks "trap" and
        leaves us in -- the retry rolls again.  The enc_end that follows, or
        the next broadcast's enc flag, is what clears me.in_encounter."""
        now = self.clock()
        if self._lost_since is None:
            self._lost_since = now
        if now - self._lost_since < LOST_SCENE_GRACE_S:
            return Action("noop", why="in a scene, waiting for its enc_path")
        if not self.throttle.ok("lost_abort", LOST_SCENE_RETRY_S):
            return Action("noop", why="lost scene: abort sent, waiting")
        self.stats["encounters_aborted"] += 1
        return Action("enc_abort", why="in a scene whose enc_path never came: walking away")

    def decide_encounter(self, obs) -> Action:
        self.ensure_run(obs)
        run = self.run
        if run is None or not run.enc:
            # No local copy (new content, or a biome path we do not know).
            # Banking keeps whatever was already won rather than gambling blind.
            return Action("enc_bank", why="unknown encounter, bank out")
        if run.awaiting(CHOICE_WAIT_S):
            return Action("noop", why=f"waiting for the roll at {run.node_key}")
        # Items work inside a scene: a threat reducer that drops the clock a
        # tier before the next roll, a heal on the last LL.
        act = self.encounter_item_action(obs, run)
        if act is not None:
            return act
        if run.trap:
            return self.decide_trap(obs, run)
        if run.kane:
            return self.decide_kane_chapter(obs, run)

        act = self.probe_gate(obs, run)
        if act is not None:
            return act

        if self.plan_encounters:
            return self.planned_choice(obs, run)

        # The baseline arm: drive straight down the first branch until the
        # node is bankable, then take the haul.  Deliberately not odds-
        # weighted -- it answers "is the content reachable and what does it
        # pay" -- and what every number before 2026-10-13 was measured with.
        # Aborting also consumes the POI for good (restorePoi=false).
        if run.can_bank():
            run.banked = True
            return Action("enc_bank", why=f"bankable at node {run.node_key}")

        choices = run.choices
        if not choices:
            # A terminal node: enc_bank is allowed at any of them (the full
            # clear, +10) even when it is not marked can_bank and nothing is
            # pending -- an abort here forfeits that and ticks the threat
            # clock, which is what this used to send.
            run.banked = True
            return Action("enc_bank", why=f"end of the scene at {run.node_key}")

        # First option we may take: a "requires_item" choice for an item we
        # lack is refused server-side, and a refused pick changes nothing,
        # so sending it would stall the scene (docs/bot-testing.md).
        ci = run.first_open_choice(obs.me)
        if ci is None:
            self.stats["encounters_aborted"] += 1
            return Action("enc_abort", why=f"no open option at node {run.node_key}")
        run.choose(ci)
        self.journal.on_choice(run, ci, obs.me)
        p = success_chance(choices[ci], obs)
        return Action("enc_choice", ci=ci,
                      why=f"first open option at {run.node_key} (p={p:.2f})")

    # --- planning a scene -------------------------------------------------
    def worth(self, obs):
        """This policy's loot values for encounters.plan(): its gear weights,
        its staple floors, the quest journal's items and goals (quest_drive),
        and unseen nodes (novelty_bonus)."""
        me = obs.me
        quest_nodes = self.journal.node_bonus(me) if self.quest_drive else None
        seen = self.stats["nodes_seen"]
        nov = self.novelty_bonus

        def node_bonus(biome, eid, key):
            v = quest_nodes(biome, eid, key) if quest_nodes else 0.0
            if nov and f"{biome}/{eid}/{key}" not in seen:
                v += nov
            return v

        return IT.Worth(me, weights=self.gear_weights,
                        floors=(self.water_floor, self.food_floor, 0, 0, 0),
                        bonus=self.journal.item_bonus(me) if self.quest_drive else {},
                        node_bonus=node_bonus)

    def planned_choice(self, obs, run) -> Action:
        """encounters.plan(): the best of banking, each open choice, and
        (where banking is not allowed) walking away, by expected value."""
        kind, ci, val = plan(run, obs, self.worth(obs), risk=self.encounter_risk)
        self.stats["plans"] += 1
        if kind == "bank":
            run.banked = True
            return Action("enc_bank", why=f"plan: bank at {run.node_key} (v={val:.1f})")
        if kind == "abort":
            self.stats["encounters_aborted"] += 1
            return Action("enc_abort", why=f"plan: nothing worth the risk at {run.node_key}")
        run.choose(ci)
        self.journal.on_choice(run, ci, obs.me)
        p = success_chance(run.choices[ci], obs)
        return Action("enc_choice", ci=ci,
                      why=f"plan: {run.node_key}[{ci}] p={p:.2f} v={val:.1f}")

    def probe_gate(self, obs, run) -> Action | None:
        """probe_gates: send a requires_item choice we may NOT take, once per
        (file, node, choice) across the fleet, and expect the board to refuse
        it no_item.  An enc_res instead is gate_bypassed (journal.on_result).
        The refusal changes nothing, so the scene carries on as before."""
        if not self.probe_gates:
            return None
        now = self.clock()
        if self._probe is not None:
            if now - self._probe[2] < CHOICE_WAIT_S:
                return Action("noop", why="gate probe: waiting for the verdict")
            # No reply and no roll: a pre-protocol-2 board (the mock) refusing
            # with an err and nothing else.
            LEDGER.gates_probed[self._probe[0]] = "refused (no reply)"
            self._probe = None
        me = obs.me
        for ci, ch in enumerate(run.choices):
            need = int(ch.get("requires_item", 0) or 0)
            if not need or has_for_choice(ch, me) or run.is_refused(ci):
                continue
            key = (run.biome, run.eid, run.node_key, ci)
            if key in LEDGER.gates_probed:
                continue
            LEDGER.gates_probed[key] = "sent"
            self._probe = (key, ci, now)
            self.stats["gates_probed"] += 1
            run.choose(ci)
            self.journal.on_choice(run, ci, me)
            return Action("enc_choice", ci=ci,
                          why=f"gate probe: {run.node_key}[{ci}] needs the {IT.name(need)}, "
                              f"which we lack -- expecting no_item")
        return None

    def decide_trap(self, obs, run) -> Action:
        """A booby trap (traps.hpp): there is no walking away at the start.

        Back out (Navigate) or work it (Scavenge) at the start; from a cache,
        bank -- taking anything disarms it -- or press on once; out of an
        escape node, leave with nothing (it stays armed, and on our map).
        enc_bank out of an escape node is what the client sends too."""
        me = obs.me
        if run.at_escape or not run.choices:
            if run.can_bank():
                run.banked = True
                self.stats["trap_banked"] += 1
            return Action("enc_bank", why=f"trap: out at {run.node_key}")
        if run.can_bank():
            p = success_chance(run.choices[0], obs)
            if (run.pushed < 1 and me.ll > self.trap_min_ll and p >= self.trap_push
                    and not run.is_refused(0)):
                run.pushed += 1
                run.choose(0)
                return Action("enc_choice", ci=0, why=f"trap: press on from {run.node_key} (p={p:.2f})")
            run.banked = True
            self.stats["trap_banked"] += 1
            return Action("enc_bank", why=f"trap: take it from {run.node_key}")
        esc, work = run.escape_choice(), run.work_choice()
        # A door the board refused (the node moved on under us) is shut.
        esc = None if run.is_refused(esc) else esc
        work = None if run.is_refused(work) else work
        if esc is None and work is None and run.is_refused(0):
            self.stats["encounters_aborted"] += 1
            return Action("enc_abort", why=f"trap: every door at {run.node_key} refused")
        if work is not None:
            pw = success_chance(run.choices[work], obs)
            if me.ll >= self.trap_min_ll and pw >= self.trap_nerve:
                run.choose(work)
                self.stats["trap_worked"] += 1
                return Action("enc_choice", ci=work, why=f"trap: work it (p={pw:.2f})")
        ci = esc if esc is not None else 0
        run.choose(ci)
        self.stats["trap_backouts"] += 1
        return Action("enc_choice", ci=ci,
                      why=f"trap: back out (p={success_chance(run.choices[ci], obs):.2f})")

    # --- Machine Gun Kane (kane.hpp) -------------------------------------
    def kane_cover_action(self, obs) -> Action | None:
        """He has asked us and we are still where he can hit us: be
        somewhere else before the next world tick.

        Cover is the terrain of the hex you end on (kaneOpenGround: scrub,
        dunes and marsh are open, everything else is not), or being more than
        KANE_RANGE from the jeep, or being underground -- a hatch counts.  He
        stops to wait for the answer, so his position is the one that matters.
        The search ignores his reach (danger=False): crossing it on the way is
        free, only where the move ENDS is checked.  With no cover in today's
        AP, a scene is cover too -- he never fires into one."""
        if not self.kane_dodge or obs.underground or not obs.kane_call_pending:
            return None
        me = obs.me
        if not obs.kane_exposed():
            asked = obs.kane_call["t"]
            if self._kane_dodged != asked:  # one dodge per question
                self._kane_dodged = asked
                self.stats["kane_dodged"] += 1
            return None
        if me.ap <= 0 or me.resting:
            return None
        legal = me.legal_dirs()
        q0, r0 = obs.pos()

        def safe(cell, q, r, cost):
            if cost <= 0 or cost > me.ap or obs.kane_exposed_at(q, r):
                return None
            # The nearest cover; further from him breaks ties, since he asks
            # again the moment we are back in the open.
            return 100.0 / cost + 0.1 * min(obs.kane_distance(q, r), 2 * KANE_RANGE)

        target = best_target(obs.map, q0, r0, safe, max_cost=me.ap,
                             stop_at=ends_journey, danger=False) if legal else None
        if target is not None and target[2] in legal:
            q, r, d, cost, _ = target
            return Action("move", d=d, why=f"KANE asked: cover at ({q},{r}) c={cost}")
        here = obs.here()
        if self.engage_encounters and here is not None and here.poi:
            return self.try_open_poi(obs, "KANE asked: no cover in reach, a scene is cover")
        return None

    def kane_hold(self, obs, act):
        """While his question is open, the one move that costs LL is a step
        back into the open: every other layer of decide() prices his reach,
        but a fallback or a pile-chase can still pick it.  Hold instead.  Once
        we are already exposed (no cover was in reach) any step may help."""
        if (act is None or act.kind != "move" or not self.kane_dodge
                or obs.underground or not obs.kane_call_pending):
            return act
        me = obs.me
        q, r = neighbor(me.q, me.r, act.d)
        if obs.kane_exposed() or not obs.kane_exposed_at(q, r):
            return act
        return Action("noop", why=f"KANE asked: holding cover, not stepping to ({q},{r})")

    @staticmethod
    def kane_quest_target(obs):
        """(q, r, chapter) of the hex where his story goes on, or None.
        Mirrors kaneOnArrival(): the live camp holds chapter stage+1 (1-3),
        the house in the glass chapter 4, and once that is read his own hex
        while he sits at his fire is chapter 5 -- before then, walking up to
        him there only gets you his question."""
        w = obs.world
        if not w.kane_seen or w.kane_ending or w.kane_mode == KANE_GONE:
            return None
        if w.kane_stage < 3 and w.kane_camp_q >= 0:
            return (w.kane_camp_q, w.kane_camp_r, w.kane_stage + 1)
        if w.kane_stage == 3 and w.kane_site_q >= 0:
            return (w.kane_site_q, w.kane_site_r, 4)
        if w.kane_stage == 4 and w.kane_mode == KANE_CAMP:
            return (w.kane_q, w.kane_r, 5)
        return None

    def kane_quest_action(self, obs, urgent: bool = False) -> Action | None:
        """Walk to the next chapter.  `urgent` asks only about his fire
        (chapter 5), which goes out two world ticks after dawn; the camps and
        the house keep until somebody reads them.  The route prices his reach
        like any other, so a survivor walking to his camp does not walk into
        his gun -- at his fire he does not shoot, so there is nothing to price."""
        if not self.kane_quest or obs.underground:
            return None
        me = obs.me
        if me.ll <= self.kane_min_ll or me.ap <= 0:
            return None
        tgt = self.kane_quest_target(obs)
        if tgt is None:
            return None if urgent else self.kane_shadow_action(obs)
        q, r, ch = tgt
        if urgent != (ch == 5):
            return None
        key = ("kane", obs.world.kane_stage, q, r)
        if (me.q, me.r) == (q, r):
            # On it and no scene: somebody else is reading it, or a failed
            # check put us out. A scene only opens on arrival, so come back.
            self.throttle.hold(key, KANE_QUEST_RETRY_S)
            return None
        if self.throttle.held(key):
            return None
        legal = me.legal_dirs()
        if not legal:
            return None
        reach = self.kane_quest_reach if self.kane_quest_reach is not None else me.ap
        dist, first = dijkstra(obs.map, me.q, me.r, max_cost=reach + 1,
                               stop_at=ends_journey)
        cost = dist.get((q, r))
        if cost is None or cost > reach:
            return None
        d = first.get((q, r), -1)
        if d not in legal:
            return None
        self.stats["kane_quest_moves"] += 1
        return Action("move", d=d, why=f"KANE chapter {ch} at ({q},{r}) c={cost}")

    def kane_shadow_action(self, obs) -> Action | None:
        """Before the last chapter: his fire is only lit at night, so keep
        within KANE_SHADOW_FAR of the jeep -- on ground he cannot hit -- and
        the walk in at dusk is short.  Only while he is further than that;
        inside it the rest of the day carries on as usual."""
        w = obs.world
        if not self.kane_shadow or w.kane_stage != 4 or not w.kane_driving:
            return None
        me = obs.me
        if obs.kane_distance(me.q, me.r) <= KANE_SHADOW_FAR:
            return None
        legal = me.legal_dirs()
        if not legal:
            return None

        def value(cell, q, r, cost):
            if cost <= 0 or obs.kane_distance(q, r) > KANE_SHADOW_FAR \
                    or obs.kane_exposed_at(q, r):
                return None
            return 10.0 / cost

        target = best_target(obs.map, me.q, me.r, value, max_cost=KANE_SHADOW_COST,
                             stop_at=ends_journey)
        if target is None or target[2] not in legal:
            return None
        q, r, d, cost, _ = target
        self.stats["kane_shadow_moves"] += 1
        return Action("move", d=d, why=f"KANE: keeping near the jeep -> ({q},{r}) c={cost}")

    def kane_choice(self, obs, run):
        """The choice to send in one of his chapters, or None.  Never one
        that leads to an escape node (backing out of the light reads nothing),
        and otherwise the best odds -- a failed check ends the scene with the
        chapter unread.  At the ending: the truth when its odds clear
        kane_truth_nerve, else whichever ending is likelier."""
        me = obs.me
        nodes = run.enc.get("nodes") or {}
        best, best_p, endings = None, -1.0, {}
        for i, ch in enumerate(run.choices):
            if run.is_refused(i) or not has_for_choice(ch, me) or not can_pay(ch, me):
                continue
            nxt = nodes.get(ch.get("success_node", ""), {})
            if nxt.get("escape"):
                continue
            p = success_chance(ch, obs)
            if nxt.get("kane_ending"):
                endings[nxt["kane_ending"]] = (i, p)
            if p > best_p:
                best, best_p = i, p
        if "truth" in endings and endings["truth"][1] >= self.kane_truth_nerve:
            return endings["truth"][0]
        if endings:
            return max(endings.values(), key=lambda ip: ip[1])[0]
        return best

    def decide_kane_chapter(self, obs, run) -> Action:
        """One of Machine Gun Kane's chapters (kane.hpp).  It counts once its
        LAST node is reached (kaneChapterRead) -- so unlike a POI it is never
        banked early: pocketing a haul halfway leaves the chapter unread, and
        nobody can open it again without stepping off and back on.  At the
        last node, or an escape node, bank whatever was won (that is what the
        browser's leave button sends).  Too hurt to risk a check: walk away,
        the chapter keeps."""
        me = obs.me
        if run.at_escape or not run.choices:
            if run.can_bank():
                run.banked = True
            what = "backed out" if run.at_escape else "read"
            return Action("enc_bank", why=f"KANE chapter {run.kane} {what} at {run.node_key}")
        if me.ll <= self.kane_min_ll:
            self.stats["encounters_aborted"] += 1
            return Action("enc_bank" if run.can_bank() else "enc_abort",
                          why=f"KANE chapter {run.kane}: LL {me.ll}, leaving it for later")
        ci = self.kane_choice(obs, run)
        if ci is None:
            self.stats["encounters_aborted"] += 1
            return Action("enc_abort", why=f"KANE chapter {run.kane}: no way on at {run.node_key}")
        run.choose(ci)
        p = success_chance(run.choices[ci], obs)
        return Action("enc_choice", ci=ci,
                      why=f"KANE chapter {run.kane} at {run.node_key} (p={p:.2f})")

    # --- crossing back to the surface -----------------------------------
    def climb_out(self, obs, prefer=None, why: str = "climbing out",
                  explore_if_blind: bool = True):
        """Head for a shaft and step onto it.

        Stepping onto a Bunker Entrance or Vent Shaft IS the ascent, so there
        is nothing to send but a move.  Two wrinkles:

        - **You cannot ascend from the shaft you are standing on.** You
          arrived on it by transition, not by a step, and only a fresh step
          onto one crosses the boards. So when we are already on a shaft the
          move is to step OFF it and come back -- which is also why an
          accidental descent costs about 4 AP to undo, not 1.
        - Shafts hang off the corridor as one-hex spurs, so pathing to one
          never routes *through* another and ejects us somewhere we did not
          choose.

        `prefer(cell, q, r, cost)` lets a subclass rank the exits (the tunnel
        runner wants a specific distant one); returning None rejects a shaft
        outright. The default prefers the cheapest exit, with a nudge toward
        Bunker Entrances because a Vent Shaft charges VENT_ASCEND_AP to climb.

        With `explore_if_blind=False` this returns None instead of wandering
        when no acceptable shaft is in sight, so a caller with its own idea of
        where to look -- a tunnel policy that would rather chase a pile while
        it hunts for the exit -- can take over.
        """
        me = obs.me
        legal = me.legal_dirs()
        if not legal:
            return Action("noop", why=f"{why}: no legal move underground")
        q, r = obs.pos()
        here = obs.tunnel[(q, r)]

        if here is not None and is_hatch_terrain(here.terrain):
            # On the shaft already: step off so the next step back on counts.
            return Action("move", d=self.rng.choice(legal),
                          why=f"{why}: stepping off the shaft to re-enter it")

        def value(cell, cq, cr, cost):
            if cell is None or not is_hatch_terrain(cell.terrain):
                return None
            if prefer is not None:
                return prefer(cell, cq, cr, cost)
            return (10.0 - ascend_cost(cell.terrain)) / cost

        target = best_target(obs.tunnel, q, r, value,
                             max_cost=TUNNEL_ESCAPE_COST, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            tq, tr, d, cost, _ = target
            return Action("move", d=d,
                          why=f"{why}: shaft ({tq},{tr}) c={cost}")

        if not explore_if_blind:
            return None

        # No shaft revealed yet -- the vision radius underground is 1, so this
        # is the normal state on the first few steps of a descent. Push into
        # the dark rather than standing still; the corridors are a connected
        # network, so any exploration finds one.
        def explore(cell, cq, cr, cost):
            if cost <= 0:
                return None
            return (3.0 if cell is None else 1.0) / cost

        target = best_target(obs.tunnel, q, r, explore,
                             max_cost=TUNNEL_ESCAPE_COST, stop_at=ends_journey)
        if target is not None and target[2] in legal:
            return Action("move", d=target[2], why=f"{why}: searching for a shaft")
        return Action("move", d=self.rng.choice(legal), why=f"{why}: fallback step")

    # --- subclass hook --------------------------------------------------
    def pursue(self, obs) -> Action:
        raise NotImplementedError
