from .base import Policy, Action, NOOP
from .drunk import DrunkPolicy

# Registry used by arena.py --policies.  Phase 2 adds scoremax, contentmax,
# coward and rival here.
REGISTRY = {
    "drunk": DrunkPolicy,
}


def make(name: str, rng, **kw) -> Policy:
    try:
        cls = REGISTRY[name]
    except KeyError:
        raise SystemExit(f"unknown policy {name!r}; have: {', '.join(sorted(REGISTRY))}")
    return cls(rng, **kw)


__all__ = ["Policy", "Action", "NOOP", "DrunkPolicy", "REGISTRY", "make"]
