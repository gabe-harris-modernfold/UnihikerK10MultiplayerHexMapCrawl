"""Drunk -- uniform random over legal actions.

This is the Phase 1 soak policy.  Its job is not to play well; it is to
exercise every code path on the board (move, all 8 actions, encounter
open/choose/bank/abort) at a controlled rate so we can find the throttle
ceiling before investing in real policy logic.

It also doubles as a fuzzer: `wild` is the probability of deliberately
sending something the server should refuse (a blocked direction, an action
with no MP).  That exercises the rejection paths and the err channel, which
a purely well-behaved bot would never touch.  Keep it low -- refused messages
still cost the AsyncTCP task a dispatch.
"""
import random

from config import ACT_REST, ACT_FORAGE, ACT_WATER, ACT_SCAV, ACT_SURVEY, ACT_SHELTER
from .base import Policy, Action

# Actions worth rolling for a soak.  TREAT and CRAFT are omitted: TREAT is a
# no-op without wounds and CRAFT needs a known recipe id, so both would mostly
# produce refusals rather than exercising anything.
SOAK_ACTIONS = (ACT_FORAGE, ACT_WATER, ACT_SCAV, ACT_SHELTER, ACT_SURVEY, ACT_REST)


class DrunkPolicy(Policy):
    name = "drunk"

    def __init__(self, rng: random.Random, wild: float = 0.05,
                 move_bias: float = 0.6):
        super().__init__(rng)
        self.wild = wild
        self.move_bias = move_bias

    def decide(self, obs) -> Action:
        # Encounters gate everything else -- the server refuses m and act
        # while one is open, so resolve it first.
        if obs.encounter is not None:
            return self._decide_encounter(obs)

        me = obs.me
        if not me.connected or me.ll == 0:
            return Action("noop", why="downed or disconnected")

        legal = me.legal_dirs()

        # Fuzz: occasionally send something that should be refused.
        if self.rng.random() < self.wild:
            blocked = [d for d in range(6) if d not in legal]
            if blocked:
                d = self.rng.choice(blocked)
                return Action("move", d=d, why="fuzz: blocked direction")

        if legal and self.rng.random() < self.move_bias:
            d = self.rng.choice(legal)
            return Action("move", d=d, why="random legal move")

        a = self.rng.choice(SOAK_ACTIONS)
        mp = self.rng.randint(1, 3) if a == ACT_WATER else 1
        return Action("act", a=a, mp=mp, why="random action")

    def _decide_encounter(self, obs) -> Action:
        enc = obs.encounter or {}
        choices = enc.get("choices") or enc.get("ch") or []
        # Bank or walk away once the node offers it; otherwise pick a branch.
        if enc.get("can_bank") or enc.get("cb"):
            if self.rng.random() < 0.7:
                return Action("enc_bank", why="random bank")
            return Action("enc_abort", why="random abort")
        n = len(choices) if choices else 3
        # Without a parsed choice list, guessing an index is harmless: the
        # server clamps and ignores an out-of-range ci.
        return Action("enc_choice", ci=self.rng.randrange(max(1, n)),
                      why="random choice")
