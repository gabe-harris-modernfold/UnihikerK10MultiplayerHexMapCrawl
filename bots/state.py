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
from config import (EQUIP_SLOTS, INV_SLOTS_MAX, MAX_PLAYERS, TUN_COLS,
                    TUN_ROWS, TUNNEL_VIS_BASE, is_hatch_terrain)
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
    # INV_SLOTS_MAX in the .ino. Sized generously rather than exactly: the
    # server sends the whole array and these are only the pre-sync defaults.
    inv_type: list = field(default_factory=lambda: [0] * INV_SLOTS_MAX)
    inv_qty: list = field(default_factory=lambda: [0] * INV_SLOTS_MAX)
    equip: list = field(default_factory=lambda: [0] * EQUIP_SLOTS)
    # Effective values the server computes (base + equipment); appendPackArrays.
    inv_slots: int = 8            # is:    pack size in effect
    ll_cap: int = 7               # llCap: LL ceiling in effect
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
        if "is"    in d: self.inv_slots     = int(d["is"])
        if "llCap" in d: self.ll_cap        = int(d["llCap"])
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
class Hatch:
    """One entry of the firmware's bunkerHatches[] table, as far as a client
    can reconstruct it.

    The pairing -- which surface hatch comes out at which shaft -- is derived
    at generation time and rides the save header; it is never sent on the
    wire.  The only way to learn it is to watch a tun_in / tun_out event,
    which names the hatch index and the SURFACE hex, and pair that against
    where the survivor actually was underground.  So `sq`/`sr` are cheap (any
    client sees them, for any player) and `tq`/`tr` are earned: you learn a
    shaft's surface exit by using it, or by watching someone else use it and
    being down there to see where they went.
    """
    idx: int
    sq: int = -1
    sr: int = -1
    tq: int = -1
    tr: int = -1

    @property
    def paired(self) -> bool:
        return self.sq >= 0 and self.tq >= 0


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
        # The bunker tunnel board: a second, walled 16x10 grid.  Kept
        # separately because a tunnel coordinate applied to the surface map
        # would corrupt a hex 60 columns away, and vice versa.
        self.tunnel = WorldMap(TUN_ROWS, TUN_COLS, wraps=False)
        self.tunnel_synced = False
        self.tunnel_vision_r = TUNNEL_VIS_BASE
        # Our own position underground.  p.q/p.r stay pinned to the hatch we
        # came down, so they are NOT where we are -- see the load-bearing
        # invariant in tunnels.hpp.  Fed by tsync, by our own dp=1 mv events
        # and by the broadcast's tq/tr, whichever lands last.
        self.tunnel_pos = None
        self.hatches: dict[int, Hatch] = {}
        self._pending_hatch = None      # idx of a descent awaiting its tsync
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

    # ── Which board are we on? ──────────────────────────────────────────
    # Every policy that reads the map has to go through these.  Reading
    # obs.map[(me.q, me.r)] while underground silently describes the surface
    # hex above the hatch we came down -- which is a real, plausible-looking
    # cell, so the mistake produces confident nonsense rather than a crash.
    @property
    def underground(self) -> bool:
        return bool(self.me.depth)

    @property
    def board(self):
        """The grid we are actually standing on."""
        return self.tunnel if self.underground else self.map

    def pos(self) -> tuple[int, int]:
        """Our position on the board we are standing on."""
        me = self.me
        if not me.depth:
            return (me.q, me.r)
        if self.tunnel_pos is not None:
            return self.tunnel_pos
        return (me.tq, me.tr)

    def here(self):
        """The cell under our feet, on whichever board that is."""
        return self.board[self.pos()]

    def at_hatch(self) -> bool:
        """Standing on a Bunker Entrance or Vent Shaft.  On the surface that
        is the way down; underground it is the way up.  Note that merely
        standing on one does nothing -- you arrive by transition, and only a
        fresh step ONTO one crosses the boards."""
        cell = self.here()
        return cell is not None and is_hatch_terrain(cell.terrain)

    def surface_hatches(self):
        """Every hatch hex we have revealed on the surface, as (q, r, cell)."""
        return [(q, r, c) for q, r, c in self.map.known_cells()
                if is_hatch_terrain(c.terrain)]

    def tunnel_shafts(self):
        """Every shaft we have revealed underground, as (q, r, cell).  These
        are the ways out; the pairing to a surface hex is only known for the
        ones in self.hatches with `paired` set."""
        return [(q, r, c) for q, r, c in self.tunnel.known_cells()
                if is_hatch_terrain(c.terrain)]

    def hatch_for_shaft(self, tq: int, tr: int):
        """The Hatch record for the shaft at (tq, tr), if we have paired it."""
        for h in self.hatches.values():
            if h.tq == tq and h.tr == tr and h.paired:
                return h
        return None

    def _hatch(self, idx: int) -> Hatch:
        h = self.hatches.get(idx)
        if h is None:
            h = self.hatches[idx] = Hatch(idx=idx)
        return h

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
            self._sync_tunnel_pos()
        elif t == "s":
            self.tick = msg.get("tk", self.tick)
            for i, pd in enumerate(msg.get("p", [])):
                if i < MAX_PLAYERS:
                    self.players[i].update(pd)
            self._apply_gs(msg.get("gs"))
            if "world" in msg:
                self.world.update(msg["world"])
            self._sync_tunnel_pos()
        elif t == "tsync":
            # The whole fogged tunnel board, unicast on descent and again on
            # reconnect-while-underground.  Merged, not replaced: see
            # WorldMap.load_full.
            self.tunnel.resize(msg.get("rows", TUN_ROWS), msg.get("cols", TUN_COLS))
            self.tunnel_vision_r = msg.get("vr", self.tunnel_vision_r)
            if "q" in msg and "r" in msg:
                self.tunnel_pos = (msg["q"], msg["r"])
                # tsync is the first message that knows where we came out, so
                # it is what completes the pairing our own tun_in started.
                if self._pending_hatch is not None:
                    h = self._hatch(self._pending_hatch)
                    h.tq, h.tr = msg["q"], msg["r"]
                    self._pending_hatch = None
            if "map" in msg:
                try:
                    self.tunnel.load_full(msg["map"], merge=True)
                    self.tunnel_synced = True
                except ValueError:
                    # A short board is a firmware buffer problem, not ours.
                    # Keep whatever we already had rather than dropping the run.
                    pass
        elif t == "vis":
            # dp=1 frames describe the bunker tunnel board, which is a
            # different grid -- applying them to the surface map would rewrite
            # a hex 60 columns away from anywhere the player has ever been.
            if msg.get("dp"):
                self.tunnel_vision_r = msg.get("vr", self.tunnel_vision_r)
                if "q" in msg and "r" in msg:
                    self.tunnel_pos = (msg["q"], msg["r"])
                if "cells" in msg:
                    self.tunnel.apply_vis(msg["cells"])
            else:
                self.vision_r = msg.get("vr", self.vision_r)
                if "cells" in msg:
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
            # eq/is/llCap ride every item_result too (appendPackArrays). Without
            # these the pack updated instantly but the equipment slots and the
            # pack size did not, so for one tick after an equip the bot saw the
            # item gone from its pack and not yet on its body.
            if "eq"    in msg: me.equip      = list(msg["eq"])
            if "is"    in msg: me.inv_slots  = int(msg["is"])
            if "llCap" in msg: me.ll_cap     = int(msg["llCap"])
        elif t == "ground_update":
            self.ground_items = list(msg.get("gi", self.ground_items))
        elif t == "enc_path":
            self.encounter = msg
        return t

    def _sync_tunnel_pos(self):
        """Keep tunnel_pos honest against the broadcast.

        tq/tr ride every `s`, so the broadcast is the authority; our own dp=1
        mv events are merely fresher between ticks.  Surfacing clears it --
        the firmware leaves tq/tr pointing at the shaft we climbed out of, and
        a stale position there would have us pathing on the wrong board the
        next time we went down."""
        me = self.me
        if me.depth:
            # (0, 0) is treated as "the broadcast did not carry tq/tr" rather
            # than as a position: PlayerState defaults them to 0, so a
            # firmware that stopped sending them would otherwise teleport us
            # to the top-left corner of the tunnel board every tick. It is a
            # legal cell, so a real (0,0) is still adopted when we have no
            # other fix, and the dp=1 mv events correct it either way.
            if (me.tq, me.tr) != (0, 0) or self.tunnel_pos is None:
                self.tunnel_pos = (me.tq, me.tr)
        else:
            self.tunnel_pos = None

    def _apply_gs(self, gs):
        if not gs:
            return
        self.threat  = gs.get("tc", self.threat)
        self.day     = gs.get("dc", self.day)
        self.weather = gs.get("wp", self.weather)

    def _apply_event(self, ev):
        k = ev.get("k")
        # NOTE: ev messages go out via ws.textAll(), so every one of these is
        # about *some* player, not necessarily us.  Only act on our own.
        mine = ev.get("pid") == self.pid
        if k == "enc_start":
            # Deliberately does NOT open self.encounter.  Only enc_path carries
            # the biome/id needed to bind the local JSON; fabricating an empty
            # dict here made every policy see an unidentifiable encounter and
            # immediately bank out of it.
            pass
        elif k in ("enc_end", "enc_bank") and mine:
            self.encounter = None
        elif k == "mv":
            # Every event that names a hex carries the board it belongs to.
            # Our own underground steps are the freshest position we get --
            # the broadcast's tq/tr is up to a tick behind.
            if mine and ev.get("dp") and "q" in ev and "r" in ev:
                self.tunnel_pos = (ev["q"], ev["r"])
        elif k in ("tun_in", "tun_out"):
            # q/r are the SURFACE hatch in both directions, for every player,
            # so a hatch's surface hex is learned just by watching anyone use
            # it -- including from the other board.
            idx = ev.get("hatch")
            if isinstance(idx, int) and idx >= 0:
                h = self._hatch(idx)
                if "q" in ev and "r" in ev:
                    h.sq, h.sr = ev["q"], ev["r"]
                if mine and k == "tun_in":
                    # The shaft we land on is only named by the tsync that
                    # follows; hold the index until it arrives.
                    self._pending_hatch = idx
                elif mine and k == "tun_out":
                    # We climbed out of whichever shaft we were standing on,
                    # which pairs it with the surface hex in this event.
                    if self.tunnel_pos is not None:
                        h.tq, h.tr = self.tunnel_pos
                    self._pending_hatch = None
                    self.tunnel_pos = None
        elif k == "regen":
            # New world: both cached boards are meaningless now, and so is
            # every hatch pairing -- generateTunnels() re-rolls the lot.
            self.synced = False
            self.map = WorldMap()
            self.tunnel = WorldMap(TUN_ROWS, TUN_COLS, wraps=False)
            self.tunnel_synced = False
            self.tunnel_pos = None
            self.hatches = {}
            self._pending_hatch = None
        elif k == "dawn":
            self.day = ev.get("day", self.day)
            if mine:
                # dawnUpkeep() clears p.resting, but the periodic broadcast
                # carries no `rt` field -- only sync does -- so this event is
                # the only way to learn we have stopped resting.
                self.players[self.pid].resting = False
        elif k == "weather":
            self.weather = ev.get("wp", self.weather)
