"""Policy interface.

A policy is a pure decision function: it reads an Observation and returns one
Action.  It never touches the socket, never sleeps, and never mutates the
observation -- the client owns all of that.  That keeps policies swappable and
makes them trivially unit-testable against a canned Observation.

The server refuses both m and act while an encounter is open, so decide() must
check obs.encounter first and answer with an encounter action instead.
"""
import time
from dataclasses import dataclass, field

# A period for Throttle.ok() that never elapses: once per key, ever.
ONCE = float("inf")


class Throttle:
    """Every sender that the board can refuse without changing anything
    needs a cooldown, or the same decision comes straight back next cycle
    and the message is re-sent forever -- EQUIP_RETRY_S was the first lesson
    (60 identical swaps in 25 s), and by 2026-10-13 there were fifteen
    hand-rolled copies of the same three lines across the policies, none of
    them testable without sleeping.  This is the one copy: keyed however the
    sender likes, on a clock the policy owns, so a test moves time instead
    of waiting for it.

        if not self.throttle.ok(("equip", slot, item), EQUIP_RETRY_S):
            return None                     # sent too recently

    `ready` and `mark` are the two halves for a sender that has to pass two
    cooldowns before it records either; `hold` refuses a key for a while on
    its own (a chapter hex to leave alone), and `clear` forgets one (a craft
    that landed need not wait out its refusal cooldown).
    """

    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self._at: dict = {}         # key -> when it last went
        self._until: dict = {}      # key -> held until

    def ready(self, key, period: float) -> bool:
        """May `key` go now?  Records nothing."""
        now = self.clock()
        if now < self._until.get(key, now):
            return False
        return now - self._at.get(key, -period) >= period

    def mark(self, key) -> None:
        self._at[key] = self.clock()

    def ok(self, key, period: float) -> bool:
        """ready() and mark() in one: True exactly when the send may go."""
        if not self.ready(key, period):
            return False
        self.mark(key)
        return True

    def hold(self, key, seconds: float) -> None:
        """Refuse `key` for `seconds` from now, whatever its period."""
        self._until[key] = self.clock() + seconds

    def held(self, key) -> bool:
        return self.clock() < self._until.get(key, -1.0)

    def holding(self) -> list:
        """Every key under a hold that has not run out."""
        now = self.clock()
        return [k for k, t in self._until.items() if now < t]

    def clear(self, key) -> None:
        self._at.pop(key, None)
        self._until.pop(key, None)

    def reset(self) -> None:
        self._at.clear()
        self._until.clear()


@dataclass(slots=True)
class Action:
    """One outbound message.  kind maps to the wire t value."""
    kind: str                       # move|act|enc_start|enc_choice|enc_bank|
                                    # enc_abort|use_item|equip_item|
                                    # unequip_item|pickup_item|loot|
                                    # drop_res|drop_item|trade_offer|
                                    # trade_accept|trade_decline|car_buy|noop
    d: int = 0                      # move: direction 0-5
    a: int = 0                      # act: ACT_* id
    ap: int = 1                     # act: AP to spend (ACT_WATER reads this)
    recipe: int = 0                 # act: ACT_CRAFT recipe id
    ci: int = 0                     # enc_choice: choice index
    q: int = -1                     # enc_start: hex being opened (REQUIRED)
    r: int = -1
    keep: list = field(default_factory=list)   # enc_bank: per-resource keep
    slot: int = 0                   # use_item / equip_item: inventory slot
    eslot: int = 0                  # unequip_item: equipment slot 0-4
    gslot: int = 0                  # pickup_item: ground-pile slot
    res: int = 0                    # loot: 1-5 for one resource, 0 = all that fits
                                    # drop_res: 1-5, the resource to put down
    qty: int = 1                    # drop_res: how many
    to: int = 0                     # trade_offer: target pid
    give: list = field(default_factory=list)   # trade_offer / car_buy payment
    want: list = field(default_factory=list)
    item: int = 0                   # car_buy: item id off the caravan shelf
    n: int = 1                      # car_buy: how many (1-3)
    frm: int = -1                   # trade_accept / trade_decline: offerer pid
    why: str = ""                   # free-text rationale, recorded not sent

    def to_msg(self):
        """Render to the wire message, or None for a noop."""
        k = self.kind
        if k == "noop":        return None
        if k == "move":        return {"t": "m", "d": self.d}
        if k == "act":
            m = {"t": "act", "a": self.a, "ap": self.ap}
            if self.recipe:    m["r"] = self.recipe
            return m
        if k == "enc_start":
            # q/r are mandatory. handleMsg_enc_start does
            #   strstr(data, "\"q\""); if (!qp) return;
            # so a message without them is discarded at the first line with no
            # error reply at all -- which is why every POI attempt across every
            # early run silently did nothing and looked like the POIs simply
            # were not reachable. The server also cross-checks them against the
            # player's own position ("Not at that hex"), so these must be where
            # the survivor actually is.
            if self.q < 0 or self.r < 0:
                raise ValueError("enc_start requires q and r")
            return {"t": "enc_start", "q": self.q, "r": self.r}
        if k == "enc_choice":  return {"t": "enc_choice", "ci": self.ci}
        if k == "enc_bank":
            m = {"t": "enc_bank"}
            if self.keep:      m["keep"] = self.keep
            return m
        if k == "enc_abort":   return {"t": "enc_abort"}
        if k == "use_item":    return {"t": "use_item", "slot": self.slot}
        if k == "equip_item":  return {"t": "equip_item", "slot": self.slot}
        if k == "pickup_item": return {"t": "pickup_item", "gslot": self.gslot}
        if k == "loot":
            # Tokens off the remains on our own hex (handleMsg_loot). No res
            # takes everything that fits, in inv[] order -- water first.
            if not 0 <= self.res <= 5:
                raise ValueError("loot res must be 0-5")
            return {"t": "loot", "res": self.res} if self.res else {"t": "loot"}
        if k == "unequip_item": return {"t": "unequip_item", "eslot": self.eslot}
        if k == "drop_res":
            # Tokens out of the pack onto our own hex (handleMsg_drop_res) --
            # the character sheet's inventory boxes. res is 1-based like loot.
            if not 1 <= self.res <= 5 or self.qty < 1:
                raise ValueError("drop_res needs res 1-5 and qty >= 1")
            return {"t": "drop_res", "res": self.res, "qty": self.qty}
        if k == "trade_offer":
            return {"t": "trade_offer", "to": self.to,
                    "give": self.give, "want": self.want}
        if k == "drop_item":
            # A typed item out of the pack onto our hex (handleMsg_drop_item).
            return {"t": "drop_item", "slot": self.slot, "qty": max(1, self.qty)}
        if k == "car_buy":
            # Off the caravan shelf, standing on its hex (handleMsg_caravan_buy):
            # give[] must be worth at least value x n, and may not hold water.
            if self.item <= 0 or not 1 <= self.n <= 3 or len(self.give) != 5:
                raise ValueError("car_buy needs item, n 1-3 and give[5]")
            if self.give[0]:
                raise ValueError("car_buy refuses water as payment")
            return {"t": "car_buy", "item": self.item, "n": self.n, "give": self.give}
        if k in ("trade_accept", "trade_decline"):
            if self.frm < 0:
                raise ValueError(f"{k} needs the offerer's pid")
            return {"t": k, "from": self.frm}
        raise ValueError(f"unknown action kind {k!r}")


NOOP = Action("noop", why="nothing to do")


class Policy:
    """Base class.  Subclasses override decide()."""

    name = "base"

    def __init__(self, rng):
        # Every policy gets an injected RNG so a whole run is reproducible
        # from one seed.  Never use the module-level random module directly.
        self.rng = rng
        # Set once the pick is confirmed.  Needed because ev messages are
        # broadcast with ws.textAll() -- every bot sees every other bot's
        # rolls, so any per-player bookkeeping must filter on this.
        self.pid = -1
        # The policy's clock, and every send cooldown on it (Throttle).  A
        # test replaces `clock` to move time; nothing in a policy should
        # read time.monotonic() directly.
        self.clock = time.monotonic
        self.throttle = Throttle(lambda: self.clock())

    def set_pid(self, pid: int) -> None:
        self.pid = pid

    def decide(self, obs) -> Action:
        raise NotImplementedError

    def on_event(self, ev) -> None:
        """Optional hook: called for every ev message, for policies that want
        to accumulate history (e.g. tracking which POIs a rival has taken)."""
        pass

    def on_reply(self, cmd: str, ok: bool, why: str | None) -> None:
        """Optional hook: the board's verdict on one of our own requests
        (protocol 2+ only -- see docs/bot-testing.md "Replies").  `why` is
        the nack code, e.g. "no_ap", "resting", "in_enc"; None on an ack."""
        pass

    def __repr__(self):
        return f"<{type(self).__name__} {self.name}>"
