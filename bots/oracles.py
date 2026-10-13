"""Invariant oracles -- what every bot checks on everything it receives.

A balance bot used to be blind to bugs: it played, and if the board did
something impossible the only symptom was a strange number in metrics.py a
day later.  Now every connection carries a `WireOracle`, and the telemetry
poller a `StateOracle`, so every run -- balance, soak, fuzz or chaos -- is
also a bug hunt, and what it finds lands in one FindingLog (findings.py).

Every check here is something the firmware must never do, taken from the
firmware source rather than from observation:

  on the tick broadcast (every connected, living player)
    ll <= llCap                     llCap is appendPackArrays' effectiveMaxLL
    1 <= food, water <= 6           applyFStep / applyWStep clamp [1, 6]
    0 <= rad <= 10                  every rad write constrains to [0, 10]
    ap >= 0, inv[] in [0, 99]
    ll == 0  ->  ap == 0            every EVT_DOWNED site zeroes apLeft
    resting / ap == 0 / ll == 0  ->  vm == 0     computeValidMoves()
    steps never go down within one life
    tk never goes backwards on one socket

  on events
    sq never goes backwards, and one (sq, kind) arrives once per socket
    at most one death per life: two `left`+cause for a pid with no `join`
      between is the double-EVT_DOWNED that corrupts the seat count
    a death names a cause (protocol 2): "unattributed" is a DOWNED site
      somebody forgot to tag
    a blocked `act` names why (bw != 0 on protocol 2)
    Machine Gun Kane (kane.hpp, protocol 8):
      a burst at us follows a road `kane_call` to us -- rule one is ALWAYS
        ASK FIRST, and the question is wiped by the burst it earns
      llLost <= min(hits, KANE_BURST_CAP), hits <= KANE_ROUNDS
      the arc's stage never goes backwards (short of a regen)
    VACANCY (vacancy.hpp, protocol 10), from the `vac` world object and events:
      over one town, `st` never goes backwards (a regen or a new town resets)
      while `st` is 1 (counting), `rm` never goes up
      Check-Out (enc_start vac 4) opens for us only with both halves -- the
        Guest Register and the Master Key -- in the pack (vacOnArrival)
      `saved` for us only after a Check-Out that opened with both, and the
        pair then leaves the pack (vacSpendItem)
      our bank of the Front Desk / Housekeeping with its item in the haul
        sets the sign's bit (`g`) -- vacOnBank

  on replies (protocol 2)
    an acked `m` is followed by our own `mv`
    an acked `act` is followed by our own `act` with out != 0
    an `act` nacked for an ABW_* reason is followed by our own blocked `act`
      carrying that same bw code -- the nack and the event must agree
    `not_seated` while we hold a seat means the board lost track of us

  on /state (StateOracle)
    connected == number of players with conn -- the seat-count invariant
    uptime never goes backwards (a reboot), evtDrops never rises,
    minHeap stays above the level that preceded both measured crashes

Checks never fire on protocol 1 boards where the firmware makes no promise.
"""
import time
from collections import deque

from config import (KANE_BURST_CAP, KANE_ROUNDS, VAC_GOT_KEY, VAC_GOT_REGISTER,
                    VAC_ITEM_KEY, VAC_ITEM_REGISTER, VAC_PART_DESK, VAC_PART_KEYS,
                    VAC_PART_OUT, VAC_ST_OPEN)
from findings import Finding, FindingLog, ReproBuffer

# How long an acked request has to show its effect.  The event is drained on
# the next 100 ms game tick; this is generous so a busy board is not a bug.
EFFECT_WINDOW_S = 3.0
# VACANCY: a bank or a Check-Out has this long to show on the sign or in the
# pack (the world object rides the next tick broadcast; a busy K10 has taken
# seconds to drain an event).
VAC_EFFECT_WINDOW_S = 8.0


def _held(me, iid) -> int:
    """How many of `iid` the pack holds."""
    return sum(q for t, q in zip(me.inv_type or [], me.inv_qty or []) if t == iid and q > 0)
# minHeap fell 47 -> 27 -> 21 KB before one measured crash (docs/bot-testing.md).
HEAP_FLOOR = 24 * 1024
# Measured 149-405 ms with 5 bots; a full second is a stall worth recording.
TICK_STALL_MS = 1000

# ABW_* in Esp32HexMapCrawl.ino, keyed by the nack code abwName() emits.
ABW_CODE = {"blocked": 0, "pack_full": 1, "terrain": 2, "no_ap": 3,
            "no_res": 4, "not_needed": 5, "resting": 6, "archetype": 7,
            "craft": 8, "bad_act": 9}
# act nacks that come from inside handleAction's switch -- those still
# broadcast their AO_BLOCKED event.  Every other act nack (parse, downed,
# in_enc, underground, busy, not_seated) is refused before any event exists.
ABW_EVENT_REASONS = set(ABW_CODE) - {"bad_act"}


class WireOracle:
    """Watches one bot's connection, across reconnects.  Everything that
    depends on having *seen* every event -- ordering, lives, steps -- resets
    in new_socket(): a client that was away missed the joins and erasures in
    between, and would otherwise read a legitimate second life as a double
    death.  Resetting can only miss a defect, never invent one."""

    def __init__(self, findings: FindingLog, source: str = "",
                 repro: ReproBuffer | None = None, clock=time.monotonic):
        self.findings = findings
        self.source = source
        self.repro = repro or ReproBuffer()
        self.clock = clock
        self.proto = 1
        self.dead: set[int] = set()        # pids whose death has no join yet
        self.steps: dict[int, int] = {}
        self._expect: deque = deque()      # [deadline, kind, detail]
        self.new_socket()

    # -- plumbing ----------------------------------------------------------
    def new_socket(self) -> None:
        self.last_tk = -1
        self.last_sq = 0
        self.kinds_at_sq: set[str] = set()
        # Machine Gun Kane: has he asked us since his last burst at us, and
        # the furthest the arc has got. A reconnect can miss the question, so
        # both start over -- resetting can only miss a defect, not invent one.
        self.kane_asked = None          # None = unknown on this socket
        self.kane_stage = -1
        # VACANCY: the sign as this socket last saw it (town, stage, rooms,
        # bits), our own Check-Out as it opened -- (had both, n register,
        # n key) -- the part scene we are in, and what a bank or a save
        # still owes ([deadline, kind, detail]).
        self.vac_town = None
        self.vac_st = -1
        self.vac_rm = -1
        self.vac_g = 0
        self.vac_out = None
        self.vac_scene = None
        self.vac_expect = []
        self._expect.clear()
        self.dead.clear()
        self.steps.clear()

    def flag(self, check, severity, summary, key=(), **detail) -> None:
        self.findings.add(Finding(check=check, severity=severity, summary=summary,
                                  source=self.source, detail=detail,
                                  repro=self.repro.snapshot(), key=tuple(key)))

    def on_tx(self, msg) -> None:
        self.repro.add(msg)

    # -- inbound -----------------------------------------------------------
    def on_rx(self, msg: dict, obs) -> None:
        t = msg.get("t")
        if t == "sync":
            self.proto = msg.get("pv", 1)
            self._check_tk(msg)
            self._check_players(msg.get("p", []), obs, full=False)
        elif t == "s":
            self._check_tk(msg)
            self._check_players(msg.get("p", []), obs, full=True)
        elif t == "ev":
            self._on_event(msg, obs)
        elif t == "enc_path" and msg.get("vac"):
            # One of VACANCY's parts is open for us (unicast).
            self.vac_scene = {"part": int(msg["vac"]), "items": set()}
        if t in ("sync", "s"):
            v = (msg.get("world") or {}).get("vac")
            if isinstance(v, dict):
                self._check_vac(v)
        if self.vac_expect:
            self._vac_settle(obs)

    # -- VACANCY -------------------------------------------------------------
    def _check_vac(self, v) -> None:
        """The `vac` world object, every tick: over one town the stage only
        goes forward, and while it counts the rooms only go down."""
        st, rm, g = v.get("st"), v.get("rm"), v.get("g")
        if not all(isinstance(x, int) for x in (st, rm, g)):
            return
        town = (v.get("q"), v.get("r"))
        if town != self.vac_town:
            # A new sign (a regen, or staging): nothing to compare it with.
            self.vac_town, self.vac_st, self.vac_rm = town, -1, -1
        if st < self.vac_st:
            self.flag("vac_stage_backwards", "major",
                      "VACANCY's sign went back a stage over the same town",
                      st=st, prev=self.vac_st, town=list(town))
        if st == VAC_ST_OPEN and self.vac_st == VAC_ST_OPEN and rm > self.vac_rm:
            self.flag("vac_rooms_rose", "major",
                      "VACANCY's count went up while it was counting",
                      rm=rm, prev=self.vac_rm, town=list(town))
        self.vac_st, self.vac_rm, self.vac_g = st, rm, g

    def _vac_settle(self, obs) -> None:
        """Strike off what a bank or a save owed once it shows: the sign's
        bit (or the sign moving on), or the pair gone from the pack."""
        me = obs.me
        keep = []
        for exp in self.vac_expect:
            _dl, kind, d = exp
            if kind == "bit" and (self.vac_g & d["bit"] or self.vac_st != VAC_ST_OPEN):
                continue
            if (kind == "spent" and _held(me, VAC_ITEM_REGISTER) <= d["register"]
                    and _held(me, VAC_ITEM_KEY) <= d["master_key"]):
                continue
            keep.append(exp)
        self.vac_expect = keep

    def _vac_event(self, k, msg, obs, mine) -> None:
        """Our own VACANCY scenes, from their open to their bank, and the
        sign's events."""
        if k == "enc_start" and mine and msg.get("vac") == VAC_PART_OUT:
            me = obs.me
            n_reg, n_key = _held(me, VAC_ITEM_REGISTER), _held(me, VAC_ITEM_KEY)
            self.vac_out = (n_reg > 0 and n_key > 0, n_reg, n_key)
            if not self.vac_out[0]:
                self.flag("vac_checkout_ungated", "critical",
                          "Check-Out opened for us without both the Guest Register "
                          "and the Master Key in the pack", key=(msg.get("pid"),),
                          register=n_reg, master_key=n_key)
        elif k == "enc_res" and mine and self.vac_scene is not None:
            if msg.get("out"):
                for tk in ("it", "it2"):
                    if msg.get(tk):
                        self.vac_scene["items"].add(int(msg[tk]))
            if msg.get("ends"):
                self.vac_scene = None
        elif k == "enc_end" and mine:
            self.vac_scene = None
        elif k == "enc_bank" and mine:
            sc, self.vac_scene = self.vac_scene, None
            if sc is None or sc["part"] not in (VAC_PART_DESK, VAC_PART_KEYS):
                return
            desk = sc["part"] == VAC_PART_DESK
            want = VAC_ITEM_REGISTER if desk else VAC_ITEM_KEY
            bit = VAC_GOT_REGISTER if desk else VAC_GOT_KEY
            if want in sc["items"] and self.vac_st == VAC_ST_OPEN and not self.vac_g & bit:
                self.vac_expect.append([self.clock() + VAC_EFFECT_WINDOW_S, "bit",
                                        {"part": sc["part"], "bit": bit}])
        elif k == "vac":
            w = msg.get("w")
            if w == "desk":
                self.vac_g |= VAC_GOT_REGISTER
            elif w == "keys":
                self.vac_g |= VAC_GOT_KEY
            elif w == "saved" and mine:
                out, self.vac_out = self.vac_out, None
                if out is None:
                    return                  # its open was before this socket
                if not out[0]:
                    self.flag("vac_saved_without_items", "critical",
                              "the town was saved by us, from a Check-Out that opened "
                              "without both halves", key=(msg.get("pid"),),
                              register=out[1], master_key=out[2])
                else:
                    self.vac_expect.append([self.clock() + VAC_EFFECT_WINDOW_S, "spent",
                                            {"register": out[1] - 1, "master_key": out[2] - 1}])

    def _check_tk(self, msg) -> None:
        tk = msg.get("tk")
        if not isinstance(tk, int):
            return
        if tk < self.last_tk:
            self.flag("tick_went_backwards", "major",
                      "tick id decreased on one socket", tk=tk, prev=self.last_tk)
        self.last_tk = max(self.last_tk, tk)

    def _check_players(self, plist, obs, full: bool) -> None:
        for pid, d in enumerate(plist):
            if not isinstance(d, dict) or not d.get("on"):
                continue
            pid = d.get("id", pid)   # PROTO 4 "s" lists seated players only
            ll = d.get("ll")
            if ll is None:
                continue
            # Steps: a respawn keeps lifetime steps and eraseslot/regen go
            # through `left` or `regen`, both of which reset this.
            sp = d.get("sp")
            if isinstance(sp, int):
                prev = self.steps.get(pid)
                if prev is not None and sp < prev:
                    self.flag("steps_went_backwards", "major",
                              "a player's lifetime steps decreased", key=(pid,),
                              pid=pid, steps=sp, prev=prev)
                self.steps[pid] = sp
            if not full:
                continue
            ap = d.get("ap", 0)
            if ll == 0:
                if ap:
                    self.flag("downed_with_ap", "major",
                              "a downed survivor still has AP", key=(pid,),
                              pid=pid, ap=ap)
                continue
            cap = d.get("llCap")
            if isinstance(cap, int) and ll > cap:
                self.flag("ll_over_cap", "major", "LL above its effective cap",
                          key=(pid,), pid=pid, ll=ll, cap=cap)
            for name in ("food", "water"):
                v = d.get(name)
                if isinstance(v, int) and not 1 <= v <= 6:
                    self.flag(f"{name}_out_of_range", "major",
                              f"{name} track outside [1, 6]", key=(pid,),
                              pid=pid, value=v)
            rad = d.get("rad")
            if isinstance(rad, int) and not 0 <= rad <= 10:
                self.flag("rad_out_of_range", "major", "radiation outside [0, 10]",
                          key=(pid,), pid=pid, rad=rad)
            if isinstance(ap, int) and ap < 0:
                self.flag("ap_negative", "major", "negative AP", key=(pid,),
                          pid=pid, ap=ap)
            inv = d.get("inv")
            if isinstance(inv, list) and any(not 0 <= x <= 99 for x in inv):
                self.flag("inv_out_of_range", "major",
                          "a resource count outside [0, 99]", key=(pid,),
                          pid=pid, inv=inv)
            vm = d.get("vm", 0)
            if vm and (ap == 0 or d.get("rt")):
                self.flag("vm_when_immobile", "minor",
                          "legal-move mask set while unable to move",
                          key=(pid,), pid=pid, vm=vm, ap=ap, rt=d.get("rt"))

    def _on_event(self, msg, obs) -> None:
        k = msg.get("k")
        sq = msg.get("sq")
        if isinstance(sq, int):
            if sq < self.last_sq:
                self.flag("ev_sq_backwards", "major",
                          "event sequence went backwards on one socket",
                          sq=sq, prev=self.last_sq, k=k)
            elif sq == self.last_sq:
                # One game event can produce two messages (downed + left)
                # sharing a seq -- but never the same kind twice.
                if k in self.kinds_at_sq:
                    self.flag("ev_duplicate", "major",
                              "the same event arrived twice on one socket",
                              key=(k,), sq=sq, k=k)
                self.kinds_at_sq.add(k)
            else:
                self.last_sq = sq
                self.kinds_at_sq = {k}

        pid = msg.get("pid")
        mine = pid is not None and pid == obs.pid
        if k == "join" and isinstance(pid, int):
            self.dead.discard(pid)
            self.steps.pop(pid, None)
        elif k == "left" and isinstance(pid, int):
            self.steps.pop(pid, None)
            cause = msg.get("cause")
            if cause:                              # protocol 2: a death
                if pid in self.dead:
                    self.flag("double_downed", "critical",
                              "a second death for one life (corrupts the seat count)",
                              key=(pid,), pid=pid, cause=cause)
                self.dead.add(pid)
                if cause == "unattributed":
                    self.flag("death_unattributed", "minor",
                              "a DOWNED site did not name its cause", pid=pid)
        elif k == "regen":
            self.dead.clear()
            self.steps.clear()
            self.kane_stage = -1
            self.vac_town, self.vac_st, self.vac_rm, self.vac_g = None, -1, -1, 0
            self.vac_out = self.vac_scene = None
            self.vac_expect = []
        elif k == "kane_call" and mine and not msg.get("camp"):
            self.kane_asked = True
        elif k == "kane_fire":
            hits, lost = msg.get("hits", 0), msg.get("llLost", 0)
            if not (0 <= hits <= KANE_ROUNDS and 0 <= lost <= min(hits, KANE_BURST_CAP)):
                self.flag("kane_burst_out_of_range", "major",
                          "a burst outside KANE_ROUNDS / KANE_BURST_CAP", key=(pid,),
                          pid=pid, hits=hits, llLost=lost)
            if mine:
                if self.kane_asked is False:
                    self.flag("kane_fired_unasked", "major",
                              "Kane fired at us without asking first (ALWAYS ASK FIRST)",
                              key=(pid,), pid=pid)
                self.kane_asked = False
        elif k == "kane_arc":
            st = msg.get("stage")
            if isinstance(st, int):
                if st < self.kane_stage:
                    self.flag("kane_stage_backwards", "major",
                              "Machine Gun Kane's arc went back a chapter",
                              stage=st, prev=self.kane_stage)
                self.kane_stage = max(self.kane_stage, st)
        elif k == "act" and self.proto >= 2:
            if msg.get("out") == 0 and not msg.get("bw"):
                self.flag("blocked_without_reason", "minor",
                          "a blocked action named no ABW_* reason",
                          key=(msg.get("a"),), a=msg.get("a"))
        if k in ("enc_start", "enc_res", "enc_end", "enc_bank", "vac"):
            self._vac_event(k, msg, obs, mine)
        if mine:
            self._match_effect(k, msg)

    # -- replies -------------------------------------------------------------
    def on_reply(self, ok: bool, cmd: str, why, seated: bool) -> None:
        if self.proto < 2:
            return
        now = self.clock()
        if ok and cmd == "m":
            self._expect.append([now + EFFECT_WINDOW_S, "mv", {"cmd": cmd}])
        elif ok and cmd == "act":
            self._expect.append([now + EFFECT_WINDOW_S, "act_done", {"cmd": cmd}])
        elif not ok and cmd == "act" and why in ABW_EVENT_REASONS:
            self._expect.append([now + EFFECT_WINDOW_S, "act_blocked",
                                 {"cmd": cmd, "why": why, "bw": ABW_CODE[why]}])
        if not ok and why == "not_seated" and seated:
            self.flag("seat_desync", "major",
                      "board says not seated while this client holds a seat",
                      key=(cmd,), cmd=cmd)

    def _match_effect(self, k, msg) -> None:
        """Own events satisfy the oldest pending expectation of their kind."""
        want = None
        if k == "mv":
            want = "mv"
        elif k == "act":
            want = "act_blocked" if msg.get("out") == 0 else "act_done"
        if want is None:
            return
        for exp in self._expect:
            if exp[1] == want:
                if want == "act_blocked" and msg.get("bw") != exp[2]["bw"]:
                    self.flag("nack_event_mismatch", "major",
                              "an action's nack and its blocked event disagree",
                              key=(exp[2]["why"],), nack=exp[2]["why"],
                              event_bw=msg.get("bw"))
                self._expect.remove(exp)
                return

    def poll(self) -> None:
        """Expire expectations whose effect never arrived."""
        now = self.clock()
        for exp in [e for e in self.vac_expect if e[0] < now]:
            self.vac_expect.remove(exp)
            if exp[1] == "bit":
                self.flag("vac_bank_not_counted", "major",
                          "a Front Desk / Housekeeping bank with its item in the haul "
                          "never set the sign's bit (vacOnBank)", key=(exp[2]["part"],),
                          **exp[2])
            else:
                self.flag("vac_items_not_spent", "major",
                          "the town was saved but the Guest Register and the Master Key "
                          "did not leave the pack (vacSpendItem)", **exp[2])
        while self._expect and self._expect[0][0] < now:
            _dl, kind, d = self._expect.popleft()
            if kind == "act_blocked":
                self.flag("nack_without_event", "major",
                          "an action was nacked but its blocked event never came",
                          key=(d["why"],), **d)
            else:
                self.flag("ack_without_effect", "major",
                          "the board acked a request whose effect never arrived",
                          key=(d["cmd"],), expected=kind, **d)


class StateOracle:
    """Checks /state.  Owned by the telemetry poller."""

    def __init__(self, findings: FindingLog, source: str = "telemetry"):
        self.findings = findings
        self.source = source
        self.prev: dict | None = None

    def flag(self, check, severity, summary, key=(), **detail) -> None:
        self.findings.add(Finding(check=check, severity=severity, summary=summary,
                                  source=self.source, detail=detail, key=tuple(key)))

    def check(self, st: dict) -> None:
        mem = st.get("mem", {}) or {}
        players = st.get("players", []) or []
        seated = sum(1 for p in players if p.get("conn"))
        conn = st.get("connected")
        # Only a complete read can be compared: /state that could not take
        # G.mutex in time comes back without the player list.
        if isinstance(conn, int) and len(players) == 6 and conn != seated:
            self.flag("connected_count_mismatch", "critical",
                      "G.connectedCount disagrees with the players actually seated",
                      connected=conn, seated=seated)
        for p in players:
            if not p.get("conn") or not p.get("ll"):
                continue
            for name in ("food", "water"):
                v = p.get(name)
                if isinstance(v, int) and not 1 <= v <= 6:
                    self.flag(f"{name}_out_of_range", "major",
                              f"{name} track outside [1, 6] in /state",
                              key=(p.get("pid"),), pid=p.get("pid"), value=v)
        heap = mem.get("minHeap")
        if isinstance(heap, int) and heap < HEAP_FLOOR:
            self.flag("heap_low", "major", "minHeap below the pre-crash level",
                      minHeap=heap, floor=HEAP_FLOOR)
        tick = mem.get("maxTickMs")
        if isinstance(tick, int) and tick > TICK_STALL_MS:
            self.flag("tick_stall", "minor", "a game tick took over a second",
                      maxTickMs=tick)
        prev = self.prev
        if prev is not None:
            up, pup = mem.get("uptimeMs"), (prev.get("mem") or {}).get("uptimeMs")
            if isinstance(up, int) and isinstance(pup, int) and up < pup:
                boot = st.get("boot") or {}
                self.flag("board_rebooted", "critical",
                          "the board restarted mid-run",
                          key=(boot.get("reset"),), reset=boot.get("reset"),
                          crash=boot.get("crash"), up_before_ms=pup)
            d, pd = st.get("evtDrops"), prev.get("evtDrops")
            if isinstance(d, int) and isinstance(pd, int) and d > pd:
                self.flag("events_dropped", "major",
                          "the event queue overflowed and lost events",
                          dropped=d - pd, total=d)
        self.prev = st
