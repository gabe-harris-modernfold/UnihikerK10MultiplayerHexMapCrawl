"""Policy interface.

A policy is a pure decision function: it reads an Observation and returns one
Action.  It never touches the socket, never sleeps, and never mutates the
observation -- the client owns all of that.  That keeps policies swappable and
makes them trivially unit-testable against a canned Observation.

The server refuses both m and act while an encounter is open, so decide() must
check obs.encounter first and answer with an encounter action instead.
"""
from dataclasses import dataclass, field


@dataclass(slots=True)
class Action:
    """One outbound message.  kind maps to the wire t value."""
    kind: str                       # move|act|enc_start|enc_choice|enc_bank|
                                    # enc_abort|use_item|trade_offer|noop
    d: int = 0                      # move: direction 0-5
    a: int = 0                      # act: ACT_* id
    mp: int = 1                     # act: MP to spend (ACT_WATER reads this)
    recipe: int = 0                 # act: ACT_CRAFT recipe id
    ci: int = 0                     # enc_choice: choice index
    keep: list = field(default_factory=list)   # enc_bank: per-resource keep
    slot: int = 0                   # use_item: inventory slot
    to: int = 0                     # trade_offer: target pid
    give: list = field(default_factory=list)
    want: list = field(default_factory=list)
    why: str = ""                   # free-text rationale, recorded not sent

    def to_msg(self):
        """Render to the wire message, or None for a noop."""
        k = self.kind
        if k == "noop":        return None
        if k == "move":        return {"t": "m", "d": self.d}
        if k == "act":
            m = {"t": "act", "a": self.a, "mp": self.mp}
            if self.recipe:    m["r"] = self.recipe
            return m
        if k == "enc_start":   return {"t": "enc_start"}
        if k == "enc_choice":  return {"t": "enc_choice", "ci": self.ci}
        if k == "enc_bank":
            m = {"t": "enc_bank"}
            if self.keep:      m["keep"] = self.keep
            return m
        if k == "enc_abort":   return {"t": "enc_abort"}
        if k == "use_item":    return {"t": "use_item", "slot": self.slot}
        if k == "trade_offer":
            return {"t": "trade_offer", "to": self.to,
                    "give": self.give, "want": self.want}
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

    def set_pid(self, pid: int) -> None:
        self.pid = pid

    def decide(self, obs) -> Action:
        raise NotImplementedError

    def on_event(self, ev) -> None:
        """Optional hook: called for every ev message, for policies that want
        to accumulate history (e.g. tracking which POIs a rival has taken)."""
        pass

    def __repr__(self):
        return f"<{type(self).__name__} {self.name}>"
