"""The fuzzer's corpus.  Data, not code -- so smoke.py can check it offline.

Every case says what it sends and what the board must answer.  The expected
verdicts come from reading the handlers (network-msg-*.hpp, network-reply.hpp),
not from watching the board, so a case that fails is either a firmware bug or
a wrong reading of the firmware; both are worth knowing.

    expect      meaning
    ------      -------
    "none"      no reply at all (not JSON, no "t", binary, fragmented)
    "reply"     exactly one ack or nack, either is fine
    "ack"       an ack
    "nack"      a nack, any reason
    "nack:X"    a nack with why == X
    "cmd:X"     a reply whose cmd is X (what a real JSON parser would do)

Payloads:
    dict        sent via ProbeClient.request() -- a rid is appended
    str         sent as a raw text frame; "@RID@" is replaced by a tracked rid
    bytes       sent as a binary frame
    callable    called with the Observation, returns one of the above

SAFETY.  The corpus never contains, and smoke.py asserts it never will:
  * a well-formed regen or eraseslot (either wipes the world)
  * a wifi message with an "ssid" (the board would try to join it, and on
    failure drops to AP-only and off the LAN)
  * a drop_res or drop_item that could drop something the probe carries
Prefixes like "e" and "r" ARE sent: on protocol 2 they must be unknown_cmd.
On protocol 1 firmware "e" ran eraseslot and "r" ran regen, which is why the
fuzzer refuses to run against a board that does not report pv >= 2.
"""
from dataclasses import dataclass
from typing import Any

# Every command handleMessage() dispatches (network-handlers.hpp).  smoke.py
# checks this against the firmware source, so it cannot drift silently.
COMMANDS = ("pick", "m", "n", "wifi", "wifi_forget", "check", "regen",
            "eraseslot", "act", "trade_offer", "trade_accept", "trade_decline",
            "car_trade", "car_buy", "use_item", "equip_item", "unequip_item",
            "drop_item", "drop_res", "pickup_item", "settings", "enc_start",
            "enc_choice", "enc_bank", "enc_abort")

# Commands whose well-formed use is destructive or takes the board off the
# network.  Only malformed / out-of-range forms of these may appear.
DANGEROUS = ("regen", "eraseslot", "wifi")


@dataclass(frozen=True)
class Case:
    name: str
    payload: Any
    expect: str
    seated: bool = True         # run after the probe has a seat
    fragments: int = 0          # >1: split across continuation frames
    rid_as_parsed: int | None = None   # for rid-mangling cases: what strtoul makes of it
    note: str = ""


def _prefix_cases():
    """Every proper prefix of every command.  The old dispatcher compared only
    the sender's length, so a prefix ran the first command (in dispatch order)
    that it began -- the note records which, so a regression reads plainly."""
    out, seen = [], set(COMMANDS)
    for cmd in COMMANDS:
        for i in range(1, len(cmd)):
            p = cmd[:i]
            if p in seen:
                continue            # already a case, or itself a command ("wifi")
            seen.add(p)
            old = next(c for c in COMMANDS if c.startswith(p))
            out.append(Case(f"prefix:{p}", {"t": p}, "nack:unknown_cmd", seated=False,
                            note=f"ran {old} before protocol 2"))
    return out


def _empty_slot(obs):
    """An inventory slot the probe is not using, so a drop there drops nothing."""
    me = obs.me
    for i in range(len(me.inv_type) - 1, -1, -1):
        if not me.inv_type[i]:
            return i
    return None


def _drop_item_no_colon(obs):
    s = _empty_slot(obs)
    if s is None:
        return None             # pack full of items: skip rather than drop one
    # rid first: after "qty" there must be no ':' anywhere, or strchr() finds
    # that one and the NULL path this case exists for is never reached.
    return '{"t":"drop_item","rid":@RID@,"slot":%d,"qty"}' % s


def _not_here(obs):
    me = obs.me
    if me.depth:
        return {"t": "enc_start", "q": (me.tq + 1) % 16, "r": me.tr}
    return {"t": "enc_start", "q": (me.q + 1) % 75, "r": me.r}


def _trade_self(obs):
    return {"t": "trade_offer", "to": obs.pid, "give": [0, 0, 0, 0, 0],
            "want": [1, 0, 0, 0, 0]}


def _trade_accept_nobody(obs):
    return {"t": "trade_accept", "from": (obs.pid + 1) % 6}


def _trade_decline_nobody(obs):
    return {"t": "trade_decline", "from": (obs.pid + 1) % 6}


CASES: list[Case] = [
    # -- dispatch: exact names only ----------------------------------------
    *_prefix_cases(),
    Case("dispatch:empty-t", {"t": ""}, "nack:unknown_cmd", seated=False),
    Case("dispatch:upper", {"t": "PICK"}, "nack:unknown_cmd", seated=False),
    Case("dispatch:trailing-space", {"t": "m "}, "nack:unknown_cmd", seated=False),
    Case("dispatch:superstring", {"t": "regenerate"}, "nack:unknown_cmd", seated=False),
    Case("dispatch:superstring-m", {"t": "mm"}, "nack:unknown_cmd", seated=False),
    Case("dispatch:unknown", {"t": "zz_no_such_cmd"}, "nack:unknown_cmd", seated=False),

    # -- framing: nothing to reply to ----------------------------------------
    Case("frame:not-json", "hello", "none", seated=False),
    Case("frame:empty", "", "none", seated=False),
    Case("frame:no-t", '{"x":1,"rid":@RID@}', "none", seated=False),
    Case("frame:t-key-only", '"t"', "none", seated=False),
    Case("frame:binary", b'\x00\xff{"t":"n","name":"bin"}', "none", seated=False),
    Case("frame:fragmented", '{"t":"check","sk":9,"dn":5,"rid":@RID@}', "none",
         seated=False, fragments=3,
         note="handleMessage only takes a message that arrives as one final frame"),

    # -- before seating: every gameplay command must say so --------------------
    Case("unseated:m", {"t": "m", "d": 0}, "nack:not_seated", seated=False),
    Case("unseated:act", {"t": "act", "a": 7}, "nack:not_seated", seated=False),
    Case("unseated:n", {"t": "n", "name": "x"}, "nack:not_seated", seated=False),
    Case("unseated:enc_bank", {"t": "enc_bank"}, "nack:not_seated", seated=False),
    Case("unseated:use_item", {"t": "use_item", "slot": 0}, "nack:not_seated",
         seated=False),
    Case("unseated:trade_accept", {"t": "trade_accept", "from": 0},
         "nack:not_seated", seated=False),

    # -- rid handling ------------------------------------------------------------
    Case("rid:string", '{"t":"check","sk":9,"dn":5,"rid":"abc"}', "nack:bad_arg",
         seated=False, rid_as_parsed=0, note="strtoul reads a non-number as 0"),
    Case("rid:negative", '{"t":"check","sk":9,"dn":5,"rid":-1}', "nack:bad_arg",
         seated=False, rid_as_parsed=4294967295),
    Case("rid:huge", '{"t":"check","sk":9,"dn":5,"rid":99999999999}', "nack:bad_arg",
         seated=False, rid_as_parsed=4294967295),

    # -- parser: first occurrence of a key, anywhere, wins -----------------------
    # These expect what a real JSON parser would do.  The hand-rolled strstr()
    # lookups take the first match in the raw text, nested or not, so these
    # are expected to FAIL on current firmware: they document the defect class.
    Case("parse:nested-t", '{"x":{"t":"zz_nested"},"t":"check","sk":9,"dn":5,"rid":@RID@}',
         "cmd:check", seated=False,
         note="dispatch reads the nested \"t\" first"),
    Case("parse:nested-field", '{"t":"check","x":{"sk":9},"sk":0,"dn":5,"rid":@RID@}',
         "ack", note="the nested sk:9 is read instead of sk:0"),
    Case("parse:duplicate-key", '{"t":"m","d":9,"d":0,"rid":@RID@}', "nack:bad_dir",
         note="first wins; most JSON parsers take the last -- documented, not a bug"),
    Case("parse:truncated-json", '{"t":"check","sk":0,"dn":5,"rid":@RID@', "reply",
         note="accepted although it is not JSON -- by design of the scanner"),

    # -- m ------------------------------------------------------------------------
    Case("m:no-d", {"t": "m"}, "nack:parse"),
    Case("m:d-6", {"t": "m", "d": 6}, "nack:bad_dir"),
    Case("m:d-neg", {"t": "m", "d": -1}, "nack:bad_dir"),
    Case("m:d-overflow", '{"t":"m","d":99999999999,"rid":@RID@}', "nack:bad_dir"),
    Case("m:d-string", {"t": "m", "d": "2"}, "reply", note="atoi('\"2\"') is 0"),
    Case("m:d-null", {"t": "m", "d": None}, "reply"),

    # -- act ----------------------------------------------------------------------
    Case("act:no-a", {"t": "act"}, "nack:parse"),
    Case("act:a-8", {"t": "act", "a": 8}, "nack:bad_act"),
    Case("act:a-neg", {"t": "act", "a": -1}, "nack:bad_act"),
    Case("act:a-255", {"t": "act", "a": 255}, "nack:bad_act"),

    # -- check / n / pick / settings ----------------------------------------------
    Case("check:no-sk", {"t": "check", "dn": 5}, "nack:parse"),
    Case("check:no-dn", {"t": "check", "sk": 0}, "nack:parse"),
    Case("check:sk-9", {"t": "check", "sk": 9, "dn": 5}, "nack:bad_arg"),
    Case("n:no-name", {"t": "n"}, "nack:parse"),
    Case("n:long-256", {"t": "n", "name": "N" * 256}, "ack", note="truncated to 11"),
    Case("n:long-1400", {"t": "n", "name": "N" * 1400}, "ack",
         note="near one TCP segment"),
    Case("n:long-4000", {"t": "n", "name": "N" * 4000}, "ack",
         note="spans several TCP segments; handleMessage needs one callback"),
    Case("n:restore", {"t": "n", "name": "fuzz"}, "ack"),
    Case("pick:seated", {"t": "pick", "arch": 0}, "nack:not_in_lobby"),
    Case("pick:no-arch", {"t": "pick"}, "nack:parse"),
    Case("pick:arch-9", {"t": "pick", "arch": 9}, "nack:bad_arg"),
    Case("settings:empty", {"t": "settings"}, "ack"),

    # -- the dangerous ones: malformed only --------------------------------------
    Case("eraseslot:no-arch", {"t": "eraseslot"}, "nack:parse"),
    Case("eraseslot:arch-9", {"t": "eraseslot", "arch": 9}, "nack:bad_arg"),
    Case("eraseslot:arch-neg", {"t": "eraseslot", "arch": -1}, "nack:bad_arg"),
    Case("wifi:no-ssid", {"t": "wifi"}, "nack:parse"),
    Case("wifi_forget:no-ssid", {"t": "wifi_forget"}, "nack:parse"),
    Case("wifi_forget:unknown", {"t": "wifi_forget", "ssid": "__fuzz_no_such_net__"},
         "nack:not_found"),

    # -- trade ----------------------------------------------------------------------
    Case("trade_offer:no-to", {"t": "trade_offer", "want": [1, 0, 0, 0, 0]}, "nack:parse"),
    Case("trade_offer:empty", {"t": "trade_offer", "to": 0}, "nack:empty"),
    Case("trade_offer:to-9", {"t": "trade_offer", "to": 9, "want": [1, 0, 0, 0, 0]},
         "nack:bad_arg"),
    Case("trade_offer:self", _trade_self, "nack:self"),
    Case("trade_accept:no-from", {"t": "trade_accept"}, "nack:parse"),
    Case("trade_accept:from-9", {"t": "trade_accept", "from": 9}, "nack:bad_arg"),
    Case("trade_accept:no-offer", _trade_accept_nobody, "nack"),
    Case("trade_decline:no-offer", _trade_decline_nobody, "nack"),
    Case("car_trade:empty", {"t": "car_trade"}, "nack:empty"),
    Case("car_buy:no-item", {"t": "car_buy"}, "nack:parse"),
    Case("car_buy:item-0", {"t": "car_buy", "item": 0}, "nack:bad_arg"),
    Case("car_buy:item-300", {"t": "car_buy", "item": 300}, "nack:bad_arg"),
    Case("car_buy:n-0", {"t": "car_buy", "item": 1, "n": 0}, "nack:bad_arg"),

    # -- items -------------------------------------------------------------------------
    Case("use_item:no-slot", {"t": "use_item"}, "nack:parse"),
    Case("use_item:slot-99", {"t": "use_item", "slot": 99}, "nack:bad_arg"),
    Case("use_item:slot-neg", {"t": "use_item", "slot": -1}, "nack:bad_arg"),
    Case("equip_item:slot-99", {"t": "equip_item", "slot": 99}, "nack:bad_arg"),
    Case("unequip_item:no-eslot", {"t": "unequip_item"}, "nack:parse"),
    Case("unequip_item:eslot-5", {"t": "unequip_item", "eslot": 5}, "nack:bad_arg"),
    Case("drop_item:slot-99", {"t": "drop_item", "slot": 99}, "nack:bad_arg"),
    Case("drop_item:qty-0", {"t": "drop_item", "slot": 0, "qty": 0}, "nack:bad_arg"),
    Case("drop_item:qty-no-colon", _drop_item_no_colon, "reply",
         note="crashed the board before protocol 2 (atoi(NULL + 1))"),
    Case("drop_res:no-res", {"t": "drop_res"}, "nack:parse"),
    Case("drop_res:res-9", {"t": "drop_res", "res": 9}, "nack:bad_arg"),
    Case("drop_res:qty-neg", {"t": "drop_res", "res": 1, "qty": -5}, "nack:bad_arg"),
    Case("pickup_item:no-gslot", {"t": "pickup_item"}, "nack:parse"),
    Case("pickup_item:gslot-99", {"t": "pickup_item", "gslot": 99}, "nack:bad_arg"),

    # -- encounters, out of order ---------------------------------------------------------
    Case("enc_start:no-q", {"t": "enc_start", "r": 0}, "nack:parse"),
    Case("enc_start:no-r", {"t": "enc_start", "q": 0}, "nack:parse"),
    Case("enc_start:q-neg", {"t": "enc_start", "q": -1, "r": 0}, "nack:bad_arg"),
    Case("enc_start:off-board", {"t": "enc_start", "q": 999, "r": 999}, "nack:bad_arg"),
    Case("enc_start:not-here", _not_here, "nack:not_here"),
    Case("enc_choice:no-ci", {"t": "enc_choice"}, "nack:parse"),
    Case("enc_choice:ci-99", {"t": "enc_choice", "ci": 99}, "nack:bad_arg"),
    Case("enc_choice:no-enc", {"t": "enc_choice", "ci": 0}, "nack:no_enc"),
    Case("enc_bank:no-enc", {"t": "enc_bank"}, "nack:no_enc"),
    Case("enc_abort:no-enc", {"t": "enc_abort"}, "nack:no_enc"),
]

# Opt-in (fuzz.py --burst): back-to-back requests with no throttle.  The
# harness doc measured 2.5x the default send rate starving the heap, so this
# is a deliberate stress, not part of a default run.
BURST = Case("burst:check", {"t": "check", "sk": 9, "dn": 5}, "nack:bad_arg")


def judge(expect: str, reply) -> str | None:
    """None if `reply` satisfies `expect`, else what went wrong."""
    got = "silence" if reply is None else (
        f"{reply.get('t')}" + (f":{reply.get('why')}" if reply.get("t") == "nack" else "")
        + (f" cmd={reply.get('cmd')}" if reply.get("cmd") else ""))
    if expect == "none":
        return None if reply is None else f"expected silence, got {got}"
    if reply is None:
        return f"expected {expect}, got silence"
    t = reply.get("t")
    if expect == "reply":
        return None
    if expect == "ack":
        return None if t == "ack" else f"expected ack, got {got}"
    if expect == "nack":
        return None if t == "nack" else f"expected nack, got {got}"
    if expect.startswith("nack:"):
        want = expect[5:]
        return None if (t == "nack" and reply.get("why") == want) \
            else f"expected {expect}, got {got}"
    if expect.startswith("cmd:"):
        want = expect[4:]
        return None if reply.get("cmd") == want else f"expected {expect}, got {got}"
    raise ValueError(f"unknown expectation {expect!r}")
