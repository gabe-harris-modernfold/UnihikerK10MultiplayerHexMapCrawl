"""Inbound message parsing -> Observation.

Deliberately tolerant: the firmware is under active development (tunnels and
multi-network Wi-Fi landed mid-build), so unknown keys are ignored and missing
keys fall back to defaults rather than raising.  A parser that hard-fails on a
new field would take the whole arena down for a purely additive firmware change.

Outbound types seen from the server:
  lobby full sync s vis ev err asgn enc_path enc_dbg ground_update
  item_result res_result trade_fail tsync wifi
Event kinds (ev.k):
  act car_avail col col_fail dawn doom_act doom_warn downed dusk enc_bank
  enc_end enc_res enc_start fire_dmg fire_spread flood_dmg flood_washout
  join left mv regen rsp trd_off trd_res weather
"""
from dataclasses import dataclass, field
from config import MAX_PLAYERS
from mapdec import WorldMap


@dataclass(slots=True)
class PlayerState:
    """One survivor.  Carries the fields common to both sync and s; the
    sync-only ones (name, archetype, skills, known recipes) keep their last
    known value across the lighter broadcasts."""
    pid: int = -1
    connected: bool = False
    q: int = 0
    r: int = 0
    score: int = 0
    steps: int = 0
    inv: list = field(default_factory=lambda: [0] * 5)
    ll: int = 0
    food: int = 0
    water: int = 0
    rad: int = 0
    mp: int = 0
    valid_moves: int = 0          # vm: 6-bit mask, bit N = direction N legal
    wounds: list = field(default_factory=lambda: [0, 0])  # [minor, major]
    f_thresh: int = 0
    w_thresh: int = 0
    inv_type: list = field(default_factory=lambda: [0] * 12)
    inv_qty: list = field(default_factory=lambda: [0] * 12)
    equip: list = field(default_factory=lambda: [0] * 5)
    in_encounter: bool = False
    depth: int = 0                # 0 surface, 1 bunker tunnel
    tq: int = 0
    tr: int = 0
    name: str = ""
    archetype: int = -1
    inv_slots: int = 8
    skills: list = field(default_factory=lambda: [0] * 5)
    known_recipes: int = 0
    resting: bool = False

    def legal_dirs(self):
        """Directions the server says are enterable right now.  Returns [] when
        downed, resting, out of MP, or disconnected -- computeValidMoves()
        zeroes the mask in all of those cases."""
        return [d for d in range(6) if self.valid_moves & (1 << d)]

    def carried(self):
        return sum(self.inv)

    def update(self, d):
        """Merge a player object from either sync or s."""
        if "id"    in d: self.pid           = d["id"]
        if "on"    in d: self.connected     = bool(d["on"])
        if "q"     in d: self.q             = d["q"]
        if "r"     in d: self.r             = d["r"]
        if "sc"    in d: self.score         = d["sc"]
        if "sp"    in d: self.steps         = d["sp"]
        if "inv"   in d: self.inv           = list(d["inv"])
        if "ll"    in d: self.ll            = d["ll"]
        if "food"  in d: self.food          = d["food"]
        if "water" in d: self.water         = d["water"]
        if "rad"   in d: self.rad           = d["rad"]
        if "mp"    in d: self.mp            = d["mp"]
        if "vm"    in d: self.valid_moves   = d["vm"]
        if "wnd"   in d: self.wounds        = list(d["wnd"])
        if "fth"   in d: self.f_thresh      = d["fth"]
        if "wth"   in d: self.w_thresh      = d["wth"]
        if "it"    in d: self.inv_type      = list(d["it"])
        if "iq"    in d: self.inv_qty       = list(d["iq"])
        if "eq"    in d: self.equip         = list(d["eq"])
        if "enc"   in d: self.in_encounter  = bool(d["enc"])
        if "dp"    in d: self.depth         = d["dp"]
        if "tq"    in d: self.tq            = d["tq"]
        if "tr"    in d: self.tr            = d["tr"]
        if "nm"    in d: self.name          = d["nm"]
        if "arch"  in d: self.archetype     = d["arch"]
        if "is"    in d: self.inv_slots     = d["is"]
        if "sk"    in d: self.skills        = list(d["sk"])
        if "kr"    in d: self.known_recipes = d["kr"]
        if "rt"    in d: self.resting       = bool(d["rt"])


@dataclass(slots=True)
class WorldState:
    """Shared world: caravan, Creeping Doom, burning and flooded hexes."""
    caravan_q: int = -1
    caravan_r: int = -1
    caravan_active: bool = False
    caravan_inv: list = field(default_factory=lambda: [0] * 5)
    caravan_stock: list = field(default_factory=list)
    doom_q: int = -1
    doom_r: int = -1
    doom_awareness: int = 0
    fire: list = field(default_factory=list)    # [[q, r, intensity], ...]
    flood: list = field(default_factory=list)

    def update(self, w):
        car = w.get("caravan") or {}
        self.caravan_q      = car.get("q", self.caravan_q)
        self.caravan_r      = car.get("r", self.caravan_r)
        self.caravan_active = bool(car.get("active", self.caravan_active))
        self.caravan_inv    = list(car.get("inv", self.caravan_inv))
        self.caravan_stock  = list(car.get("stock", self.caravan_stock))
        doom = w.get("doom") or {}
        self.doom_q         = doom.get("q", self.doom_q)
        self.doom_r         = doom.get("r", self.doom_r)
        self.doom_awareness = doom.get("awareness", self.doom_awareness)
        if "fire"  in w: self.fire  = list(w["fire"])
        if "flood" in w: self.flood = list(w["flood"])


class Observation:
    """A single bot's whole view of the game.  The client mutates it in place
    as messages arrive; policies read it and never write it."""

    def __init__(self):
        self.pid = -1
        self.tick = 0
        self.day = 0
        self.threat = 0
        self.weather = 0
        self.vision_r = 0
        self.map = WorldMap()
        self.players = [PlayerState(pid=i) for i in range(MAX_PLAYERS)]
        self.world = WorldState()
        self.ground_items = []
        self.synced = False
        # Set while an encounter overlay is open.  Both m and act are refused
        # by the server in this state, so policies must answer with
        # enc_choice / enc_bank / enc_abort instead.
        self.encounter = None
        self.last_error = None

    @property
    def me(self):
        return self.players[self.pid] if 0 <= self.pid < MAX_PLAYERS else PlayerState()

    def rivals(self):
        return [p for p in self.players if p.pid != self.pid and p.connected]

    def apply(self, msg):
        """Fold one server message in.  Returns its type, for the caller log."""
        t = msg.get("t", "?")
        if t == "sync":
            self.pid      = msg.get("id", self.pid)
            self.tick     = msg.get("tk", self.tick)
            self.vision_r = msg.get("vr", self.vision_r)
            if "map" in msg:
                self.map.load_full(msg["map"])
            for i, pd in enumerate(msg.get("p", [])):
                if i < MAX_PLAYERS:
                    self.players[i].update(pd)
            self._apply_gs(msg.get("gs"))
            if "world" in msg:
                self.world.update(msg["world"])
            self.ground_items = list(msg.get("gi", []))
            self.synced = True
        elif t == "s":
            self.tick = msg.get("tk", self.tick)
            for i, pd in enumerate(msg.get("p", [])):
                if i < MAX_PLAYERS:
                    self.players[i].update(pd)
            self._apply_gs(msg.get("gs"))
            if "world" in msg:
                self.world.update(msg["world"])
        elif t == "vis":
            self.vision_r = msg.get("vr", self.vision_r)
            # dp=1 frames describe the bunker tunnel board, which is a
            # different grid -- ignore rather than corrupting the surface map.
            if not msg.get("dp") and "cells" in msg:
                self.map.apply_vis(msg["cells"])
        elif t == "ev":
            self._apply_event(msg)
        elif t == "err":
            self.last_error = msg.get("msg")
        elif t in ("item_result", "res_result"):
            me = self.me
            if "it"  in msg: me.inv_type = list(msg["it"])
            if "iq"  in msg: me.inv_qty  = list(msg["iq"])
            if "inv" in msg: me.inv      = list(msg["inv"])
        elif t == "ground_update":
            self.ground_items = list(msg.get("gi", self.ground_items))
        elif t == "enc_path":
            self.encounter = msg
        return t

    def _apply_gs(self, gs):
        if not gs:
            return
        self.threat  = gs.get("tc", self.threat)
        self.day     = gs.get("dc", self.day)
        self.weather = gs.get("wp", self.weather)

    def _apply_event(self, ev):
        k = ev.get("k")
        if k == "enc_start":
            self.encounter = self.encounter or {}
        elif k in ("enc_end", "enc_bank"):
            self.encounter = None
        elif k == "regen":
            # New world: the cached map is meaningless now.
            self.synced = False
            self.map = WorldMap()
        elif k == "dawn" and ev.get("pid") == self.pid:
            self.day = ev.get("day", self.day)
        elif k == "weather":
            self.weather = ev.get("wp", self.weather)
