from .base import Policy, Action, NOOP
from .survivor import SurvivorPolicy
from .drunk import DrunkPolicy
from .scoremax import ScoreMaxPolicy
from .contentmax import ContentMaxPolicy
from .coward import CowardPolicy
from .rival import RivalPolicy
from .subterranean import SubterraneanPolicy, TunnelPolicy
from .tunnelrunner import TunnelRunnerPolicy
from .sentinel import SentinelPolicy

REGISTRY = {
    "drunk": DrunkPolicy,
    "scoremax": ScoreMaxPolicy,
    "contentmax": ContentMaxPolicy,
    "coward": CowardPolicy,
    "rival": RivalPolicy,
    # The Subterranean Explorers. Both play the bunker tunnel board on
    # purpose; every other policy treats a hatch as a hole to fall into.
    "subterranean": SubterraneanPolicy,
    "tunnelrunner": TunnelRunnerPolicy,
    # Never rests, so days run their full 5 real minutes and the real-clock
    # hazards get sampled. The soak bot's policy (soak.py); realtime only.
    "sentinel": SentinelPolicy,
}

# The encounter library is ~100 JSON files; parse it once and share the
# instance across every policy in a run rather than per bot.
_SHARED_LIBRARY = None


def shared_library():
    global _SHARED_LIBRARY
    if _SHARED_LIBRARY is None:
        from encounters import EncounterLibrary
        _SHARED_LIBRARY = EncounterLibrary()
        _SHARED_LIBRARY.load_all()
    return _SHARED_LIBRARY


def make(name: str, rng, **kw) -> Policy:
    try:
        cls = REGISTRY[name]
    except KeyError:
        raise SystemExit(
            f"unknown policy {name!r}; have: {', '.join(sorted(REGISTRY))}")
    if issubclass(cls, SurvivorPolicy):
        kw.setdefault("library", shared_library())
    return cls(rng, **kw)


__all__ = ["Policy", "Action", "NOOP", "SurvivorPolicy", "DrunkPolicy",
           "ScoreMaxPolicy", "ContentMaxPolicy", "CowardPolicy", "RivalPolicy",
           "TunnelPolicy", "SubterraneanPolicy", "TunnelRunnerPolicy", "SentinelPolicy",
           "REGISTRY", "make", "shared_library"]
