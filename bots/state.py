"""Inbound message parsing -> Observation.

Deliberately tolerant: the firmware is under active development (tunnels and
multi-network Wi-Fi landed mid-build), so unknown keys are ignored and missing
keys fall back to defaults rather than raising.  A parser that hard-fails on a
new field would take the whole arena down for a purely additive firmware change.

Outbound types seen from the server:
  lobby full sync s vis ev err asgn enc_path enc_dbg ground_update
  item_result res_result loot_result trade_fail tsync wifi eco
Event kinds (ev.k):
  act car_avail col col_fail dawn doom_act doom_warn downed dusk enc_bank
  enc_end enc_res enc_start fire_dmg fire_spread flood_dmg flood_washout
  join left mv regen rsp trd_off trd_res weather dmg
  kane_call kane_fire kane_arc
"""
import time
from dataclasses import dataclass, field
from config import (EQUIP_SLOTS, INV_SLOTS_MAX, MAX_PLAYERS, TUN_COLS,
                    TUN_ROWS, TUNNEL_VIS_BASE, is_hatch_terrain,
                    KANE_DRIVE, KANE_GONE, KANE_RANGE, MAP_COLS,
                    MAP_ROWS, TICK_MS, WORLD_TICK_INTERVAL, kane_open_ground)
from mapdec import WorldMap
from navigate import (DAISY_BLOOM_COST, DAISY_BUD_COST, KANE_CALLED_COST,
                      KANE_DANGER_COST, hex_dist_wrap)

# Machine Gun Kane asks, and the burst comes one world tick later
# (WORLD_TICK_INTERVAL game ticks). His question stays "open" for that long
# plus slack for a slow board; after that a tick has passed without a burst,
# which means we were out of the open in time (or he went to his fire).
KANE_CALL_TTL_S = WORLD_TICK_INTERVAL * TICK_MS / 1000.0 + 5.0


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
    ap: int = 0
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
    water_cap: int = 0            # wc:    canteenCap(), water riding outside the pack
    in_encounter: bool = False
    depth: int = 0                # 0 surface, 1 bunker tunnel
    tq: int = 0
    tr: int = 0
    name: str = ""
    archetype: int = -1
    skills: list = field(default_factory=lambda: [0] * 5)
    known_recipes: int = 0
    resting: bool = False

    def legal_dirs(self):
        """Directions the server says are enterable right now.  Returns [] when
        downed, resting, out of AP, or disconnected -- computeValidMoves()
        zeroes the mask in all of those cases."""
        return [d for d in range(6) if self.valid_moves & (1 << d)]

    def carried(self):
        """Tokens counted against the pack -- tokenLoad(), not the raw sum:
        water sitting in a canteen is outside the cap it is meant to spare."""
        return sum(self.inv) - min(self.inv[0], self.water_cap)

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
        if "ap"    in d: self.ap            = d["ap"]
        if "vm"    in d: self.valid_moves   = d["vm"]
        if "wnd"   in d: self.wounds        = list(d["wnd"])
        if "fth"   in d: self.f_thresh      = d["fth"]
        if "wth"   in d: self.w_thresh      = d["wth"]
        if "is"    in d: self.inv_slots     = int(d["is"])
        if "llCap" in d: self.ll_cap        = int(d["llCap"])
        if "wc"    in d: self.water_cap     = int(d["wc"])
        if "it"    in d: self.inv_type      = list(d["it"])
        if "iq"    in d: self.inv_qty       = list(d["iq"])
        if "eq"    in d: self.equip         = list(d["eq"])
        if "enc"   in d: self.in_encounter  = bool(d["enc"])
        if "dp"    in d: self.depth         = d["dp"]
        if "tq"    in d: self.tq            = d["tq"]
        if "tr"    in d: self.tr            = d["tr"]
        if "nm"    in d: self.name          = d["nm"]
        if "arch"  in d: self.archetype     = d["arch"]
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
    # Machine Gun Kane (kane.hpp appendKaneJson): the jeep, and the arc it
    # leaves behind, shared by the whole party. `kane_seen` is False on a
    # board that does not send him (older firmware).
    kane_seen: bool = False
    kane_q: int = -1
    kane_r: int = -1
    kane_mode: int = KANE_DRIVE     # KANE_DRIVE / KANE_CAMP (his fire) / KANE_GONE
    kane_stage: int = 0             # chapters read, 0-5
    kane_ending: int = 0            # 0 none, 1 truth, 2 lie
    kane_camp_q: int = -1           # the camp holding chapter stage+1 (1-3)
    kane_camp_r: int = -1
    kane_site_q: int = -1           # the house in the glass (chapter 4), only while it is next
    kane_site_r: int = -1

    @property
    def kane_driving(self) -> bool:
        """On the road, and so able to ask -- and then shoot."""
        return self.kane_seen and self.kane_mode == KANE_DRIVE and not self.kane_ending

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
        k = w.get("kane")
        if isinstance(k, dict):
            self.kane_seen   = True
            self.kane_q      = k.get("q", self.kane_q)
            self.kane_r      = k.get("r", self.kane_r)
            self.kane_mode   = k.get("m", self.kane_mode)
            self.kane_stage  = k.get("st", self.kane_stage)
            self.kane_ending = k.get("e", self.kane_ending)
            self.kane_camp_q = k.get("cq", self.kane_camp_q)
            self.kane_camp_r = k.get("cr", self.kane_camp_r)
            self.kane_site_q = k.get("gq", self.kane_site_q)
            self.kane_site_r = k.get("gr", self.kane_site_r)


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
        # Where survivors fell and the resource tokens they left there:
        # [{q, r, pid, nm, d, res[5]}] from sync / ground_update "rm".  Their
        # items are ordinary ground_items piles on the same hex, and the record
        # lasts while either half does (dropRemains / remainsPrune).
        self.remains = []
        # Graves THIS bot's survivors left, as (q, r): a ground_update "fell"
        # that named our seat.  Matching on pid later is not enough -- a
        # respawn can land in a different seat -- so it is remembered here,
        # where it survives the reconnect (the client keeps this object).
        self.my_graves = set()
        self.synced = False
        # Set while an encounter overlay is open.  Both m and act are refused
        # by the server in this state, so policies must answer with
        # enc_choice / enc_bank / enc_abort instead.
        self.encounter = None
        self.last_error = None
        # Firmware PROTO_VERSION from sync "pv" (absent = 1, the pre-ack
        # firmware and the mock). 2+ answers every request that carries a
        # "rid" with ack/nack, stamps ev with "sq" and sends "rt" per tick.
        self.proto = 1
        # The Understory (docs/ecology-spec.md, protocol 3).  Cosmetic except
        # for the Wasteland Daisy, so the ~9 KB vein grid is dropped on the
        # floor here and only the daisy patches are kept: (q, r) -> (stage,
        # count).  Stage 3 bites for 1 LL on entry; stage 2 blooms at the
        # next dawn.  Both are priced into map.extra_cost for the pathfinder.
        self.eco_tick = 0
        self.eco_name = ""
        self.daisies: dict[tuple[int, int], tuple[int, int]] = {}
        # Machine Gun Kane has asked US (a road `kane_call`, unicast): {t, q, r}
        # with t on time.monotonic(), or None.  The burst follows one world
        # tick later if we are still in the open -- see kane_call_pending.
        self.kane_call = None
        self._kane_sig = None           # what danger_cost was last built from
        self._surface_rev = 0           # bumps when surface cells are revealed
        # Trades offered TO us that are still open: offerer pid -> {give,
        # want, t}.  `give` is what they hand over, `want` what they ask of us.
        self.trade_offers: dict[int, dict] = {}
        self.last_trade_fail = None
        self.last_item_result = None

    # The board lets an offer lapse after 30 s (network-msg-trade.hpp).
    TRADE_OFFER_TTL_S = 28.0

    def trade_offers_live(self) -> dict:
        now = time.monotonic()
        for k in [k for k, o in self.trade_offers.items()
                  if now - o["t"] > self.TRADE_OFFER_TTL_S]:
            del self.trade_offers[k]
        return self.trade_offers

    # ── Machine Gun Kane ────────────────────────────────────────────────
    @property
    def kane_call_pending(self) -> bool:
        """He asked us and the burst has not come yet: one world tick to be
        somewhere he cannot hit.  Cleared by our burst, by his fire (he does
        not shoot after dark), or by KANE_CALL_TTL_S running out."""
        c = self.kane_call
        if c is None:
            return False
        if not self.world.kane_driving or time.monotonic() - c["t"] > KANE_CALL_TTL_S:
            self.kane_call = None
            return False
        return True

    def kane_distance(self, q: int, r: int) -> int:
        """Hexes from the jeep to surface hex (q, r); a large number when
        the board has not sent him."""
        w = self.world
        if not w.kane_seen or w.kane_q < 0:
            return 1 << 10
        return hex_dist_wrap(w.kane_q, w.kane_r, q, r)

    def kane_exposed_at(self, q: int, r: int) -> bool:
        """Could he hit a survivor standing on surface hex (q, r)?  Mirrors
        kaneExposed(): open ground within KANE_RANGE of a jeep on the road.
        An unrevealed hex in his reach counts as open: scrub and dunes are
        most of the map, and being wrong the other way costs LL."""
        if not self.world.kane_driving or self.kane_distance(q, r) > KANE_RANGE:
            return False
        cell = self.map[(q, r)]
        return cell is None or kane_open_ground(cell.terrain)

    def kane_exposed(self) -> bool:
        """Are WE where he can hit us right now?  Never underground."""
        if self.me.depth:
            return False
        me = self.me
        return self.kane_exposed_at(me.q, me.r)

    def _refresh_kane_danger(self):
        """Price his reach into the surface map for the pathfinder: open
        ground within KANE_RANGE of the jeep while he is on the road, at
        KANE_CALLED_COST once he has asked us.  Rebuilt only when the jeep,
        his mode, our call or the revealed map changed."""
        w = self.world
        called = self.kane_call_pending
        sig = (w.kane_driving, w.kane_q, w.kane_r, called, self._surface_rev, id(self.map))
        if sig == self._kane_sig:
            return
        self._kane_sig = sig
        dc = {}
        if w.kane_driving and w.kane_q >= 0:
            cost = KANE_CALLED_COST if called else KANE_DANGER_COST
            for dq in range(-KANE_RANGE, KANE_RANGE + 1):
                for dr in range(max(-KANE_RANGE, -dq - KANE_RANGE),
                                min(KANE_RANGE, -dq + KANE_RANGE) + 1):
                    q, r = (w.kane_q + dq) % MAP_COLS, (w.kane_r + dr) % MAP_ROWS
                    cell = self.map[(q, r)]
                    if cell is None:
                        dc[(q, r)] = cost // 2
                    elif kane_open_ground(cell.terrain):
                        dc[(q, r)] = cost
        self.map.danger_cost = dc

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

    def remains_at(self, q: int, r: int):
        """The remains record on surface hex (q, r), or None.  Surface only:
        underground me.q/me.r name the hatch overhead, not where we stand."""
        for rm in self.remains:
            if rm.get("q") == q and rm.get("r") == r:
                return rm
        return None

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
            self.proto    = msg.get("pv", 1)
            self.tick     = msg.get("tk", self.tick)
            self.vision_r = msg.get("vr", self.vision_r)
            if "map" in msg:
                # Merged, like tsync.  sendSync() encodes only the current
                # vision disk (encodeMapFog around me.q/me.r), so replacing
                # the map wiped everything outside it on every reconnect --
                # and seat loss reconnects a bot 6-13 times a run.  Measured
                # 2026-09-23: known cells fell 177 -> 35 -> 7 over a realtime
                # run and neither tunnel bot ever held a hatch in its map
                # long enough to dive.  Nothing from another world survives
                # the merge: the `regen` event replaces both boards outright.
                self.map.load_full(msg["map"], merge=True)
            for i, pd in enumerate(msg.get("p", [])):
                if i < MAX_PLAYERS:
                    self.players[i].update(pd)
            self._apply_gs(msg.get("gs"))
            if "world" in msg:
                self.world.update(msg["world"])
            self._surface_rev += 1
            self._refresh_kane_danger()
            self.ground_items = list(msg.get("gi", []))
            self.remains = list(msg.get("rm", []))
            self._prune_graves()
            self.synced = True
            self._sync_tunnel_pos()
        elif t == "s":
            self.tick = msg.get("tk", self.tick)
            # PROTO 4 lists only seated players, each with its seat as "id";
            # a seat missing from p is offline.  No "id" = the old dense form.
            plist = msg.get("p", [])
            sparse = any(isinstance(pd, dict) and "id" in pd for pd in plist)
            listed = set()
            for i, pd in enumerate(plist):
                i = pd.get("id", i) if isinstance(pd, dict) else i
                if 0 <= i < MAX_PLAYERS:
                    self.players[i].update(pd)
                    listed.add(i)
            if sparse:
                for i in range(MAX_PLAYERS):
                    if i not in listed:
                        self.players[i].connected = False
            self._apply_gs(msg.get("gs"))
            if "world" in msg:
                self.world.update(msg["world"])
            self._refresh_kane_danger()
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
                    self.tunnel.load_full(msg["map"], merge=True, ops=msg.get("op"))
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
                    self.tunnel.apply_vis(msg["cells"], ops=msg.get("op"))
            else:
                self.vision_r = msg.get("vr", self.vision_r)
                if "cells" in msg:
                    self.map.apply_vis(msg["cells"])
                    self._surface_rev += 1
                    self._refresh_kane_danger()
        elif t == "ev":
            self._apply_event(msg)
        elif t == "err":
            self.last_error = msg.get("msg")
        elif t in ("item_result", "res_result", "loot_result"):
            # loot_result carries only inv (and got/why); the ground_update
            # that follows it is what shrinks the remains record.
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
            # wc moves with an equip exactly as is/llCap do; kr rides a
            # craft ack (a crafted recipe is never consumed, but the server
            # resends the mask and it is the only copy outside a sync).
            if "wc"    in msg: me.water_cap  = int(msg["wc"])
            if "kr"    in msg: me.known_recipes = int(msg["kr"])
            if t == "item_result":
                self.last_item_result = msg
        elif t == "trade_fail":
            # car_buy / car_trade / trade_offer refused (why: 1 no caravan,
            # 2 cannot pay, 3 no room, 4 offered water). Protocol 2 also nacks.
            self.last_trade_fail = msg.get("why", 0)
        elif t == "ground_update":
            self.ground_items = list(msg.get("gi", self.ground_items))
            self.remains = list(msg.get("rm", self.remains))
            # "fell" is sent while our pid still names the seat that fell --
            # downed, left and this all go out in one drainEvents() pass,
            # ahead of the broadcast that shows the seat empty.
            if (msg.get("why") == "fell" and self.pid >= 0
                    and msg.get("pid") == self.pid
                    and "q" in msg and "r" in msg):
                self.my_graves.add((msg["q"], msg["r"]))
            self._prune_graves()
        elif t == "enc_path":
            self.encounter = msg
        elif t == "eco":
            # Every field is optional: the scar grid rides a message of its
            # own (`s` alone), and a main message always carries `dz`, so
            # a message without it must not be read as "no daisies".
            self.eco_tick = msg.get("tk", self.eco_tick)
            if "n" in msg:
                self.eco_name = msg["n"]
            if "dz" in msg:
                self.daisies = {(d[0], d[1]): (d[2], d[3])
                                for d in msg["dz"] if len(d) >= 4}
                self.map.extra_cost = {
                    qr: (DAISY_BLOOM_COST if stage >= 3 else DAISY_BUD_COST)
                    for qr, (stage, _count) in self.daisies.items()
                    if stage >= 2}
        return t

    def _prune_graves(self):
        """A grave of ours only means anything while its record is there.
        Once the last pile is lifted or it ages out, somebody else falling on
        the same hex later must not read as ours."""
        live = {(rm.get("q"), rm.get("r")) for rm in self.remains}
        self.my_graves &= live

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

    def _surface_cell(self, d):
        """The surface cell a {q, r} dict names, or None if it names none --
        the surface wraps, so a missing coordinate must not become -1."""
        q, r = d.get("q"), d.get("r")
        if not (isinstance(q, int) and isinstance(r, int)):
            return None
        return self.map[(q, r)]

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
            # A banked scene's recipes are the only knowledge grant in the
            # game, and kr rides sync (not `s`): the browser ORs `recs` in
            # here (network.js _evEncBank) and so must we, or a recipe learned
            # in a scene stays unknown to the bot until its next reconnect.
            if k == "enc_bank" and isinstance(ev.get("recs"), int):
                self.me.known_recipes |= ev["recs"]
        elif k == "trd_off":
            # An offer TO us (they give `give`, they want `want`); anyone
            # else's is none of our business. It lapses on the board after
            # TRADE_OFFER_TTL_S, so a stale one is dropped by trade_offers_live.
            if ev.get("to") == self.pid and isinstance(ev.get("from"), int):
                self.trade_offers[ev["from"]] = {
                    "give": list(ev.get("give") or [0] * 5),
                    "want": list(ev.get("want") or [0] * 5),
                    "t": time.monotonic()}
        elif k == "trd_res":
            # Accepted, declined, expired or failed: either way the offer is
            # gone. Keyed by the offerer, so clear it from whichever side.
            frm, to = ev.get("from"), ev.get("to")
            if to == self.pid:
                self.trade_offers.pop(frm, None)
            elif frm == self.pid:
                self.trade_offers.pop(to, None)
        elif k == "trap":
            # A booby trap changed state (traps.hpp). "known" only ever comes
            # to the survivor who escaped it -- it is on our map now -- and
            # "sprung"/"spent" to everyone, so a trap we knew stops being one.
            q, r = ev.get("q"), ev.get("r")
            if isinstance(q, int) and isinstance(r, int):
                grid = self.tunnel if ev.get("dp") else self.map
                cell = grid[(q, r)]
                if cell is not None:
                    cell.trap = ev.get("out") == 'known'
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
        elif k == "kane_call":
            # Unicast, so it is always about us -- but a reconnect can leave
            # an old seat's pid in flight, so check anyway.  camp 1 is the
            # question at his fire, which is harmless; camp 0 is on the road,
            # and the burst follows a world tick later if we are still in
            # the open.
            if mine and not ev.get("camp"):
                self.kane_call = {"t": time.monotonic(), "q": ev.get("q"), "r": ev.get("r")}
                self._refresh_kane_danger()
        elif k == "kane_fire":
            # The burst answers the question; the next one needs a new call.
            if mine:
                self.kane_call = None
                if isinstance(ev.get("ll"), int):
                    self.players[self.pid].ll = ev["ll"]
                self._refresh_kane_danger()
        elif k == "kane_arc":
            # A chapter was read (anyone's): the world object says so on the
            # next tick anyway, this just does not wait for it.
            if isinstance(ev.get("stage"), int):
                self.world.kane_stage = max(self.world.kane_stage, ev["stage"])
            if ev.get("ending") in ("truth", "lie"):
                self.world.kane_ending = 1 if ev["ending"] == "truth" else 2
                self.world.kane_mode = KANE_GONE
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
            # regen empties groundItems[] and remainsTable[]; the sync that
            # follows refreshes both lists, but nothing refreshes my_graves.
            self.ground_items = []
            self.remains = []
            self.my_graves = set()
            # The species died with its world (ecoRequestRegen); the new
            # map above already has an empty extra_cost.
            self.daisies = {}
            # A fresh jeep and an unread arc come with the sync that follows.
            self.world.kane_seen = False
            self.kane_call = None
            self._kane_sig = None
        elif k == "dawn":
            self.day = ev.get("day", self.day)
            if mine:
                # dawnUpkeep() clears p.resting. Protocol 2+ also carries
                # `rt` in every tick, but older firmware sent it only in
                # sync, which made this event the only signal -- keep it.
                self.players[self.pid].resting = False
        elif k == "act" and ev.get("out") == 1 and ev.get("a") in (4, 7):
            # SHELTER (4), and REST (7) with a Fire Starter, report the hex's
            # shelter level afterwards as "cnd" -- the same field the browser
            # uses (_handleShelterSuccess), because no vis disk follows an
            # action and the map would otherwise say "no shelter" until the
            # next step.  SHELTER is refused underground, so the actor's
            # surface q/r is the hex.
            pid = ev.get("pid")
            if isinstance(pid, int) and 0 <= pid < MAX_PLAYERS                     and not self.players[pid].depth and "cnd" in ev:
                p = self.players[pid]
                cell = self.map[(p.q, p.r)]
                if cell is not None and (ev["a"] == 4 or ev["cnd"] == 2):
                    cell.shelter = ev["cnd"]
        elif k == "fire_spread":
            # intensity 0 is "just burned out": the server has already turned
            # the hex into Ash Dunes (onFireExtinguished), and the browser
            # mirrors that (_evFireSpread).  Nothing else refreshes a hex
            # nobody steps onto, and a bot that still read it as Forest sent
            # FORAGE there 112 times on 2026-10-13, refused `terrain` every
            # time, while the Creeping Doom ate it.
            if ev.get("intensity") == 0:
                cell = self._surface_cell(ev)
                if cell is not None:
                    cell.terrain = 1
        elif k == "flood_washout":
            # `intensity` is the terrain id the hex just became -- 3 (Marsh,
            # the flood front) or 5 (Flooded District, drowned for good); see
            # spreadFlood() and _evFloodWashout.  Flooded grows no food.
            t = ev.get("intensity")
            cell = self._surface_cell(ev)
            if cell is not None and isinstance(t, int) and 0 <= t < 12:
                cell.terrain = t
        elif k == "quake":
            # Shelters on the fault line are gone; settlements on it level to
            # Open Scrub (the browser's quake handler does the same).
            for c in ev.get("destroyed", []) or []:
                cell = self._surface_cell(c)
                if cell is not None:
                    cell.shelter = 0
            for c in ev.get("converted", []) or []:
                cell = self._surface_cell(c)
                if cell is not None:
                    cell.terrain, cell.shelter = 0, 0
        elif k == "settle":
            for c in ev.get("removed", []) or []:
                cell = self._surface_cell(c)
                if cell is not None:
                    cell.shelter = 0
            cell = self._surface_cell(ev)
            if cell is not None:
                cell.terrain = 9
        elif k == "weather":
            self.weather = ev.get("wp", self.weather)
