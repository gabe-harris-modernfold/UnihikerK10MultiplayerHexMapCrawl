"""Offline smoke test -- no board, no network.

Covers the decode/parse/policy/record path with synthetic data so a broken
parser is caught before it wastes a live run.  Run: python smoke.py
"""
import json
import random
import tempfile
from pathlib import Path

import config
from mapdec import WorldMap, decode_cell
from state import Observation
from policy import make as make_policy
from policy.base import Action
from record import Recorder, read as read_run
from telemetry import digest

ok = 0


def check(label, cond):
    global ok
    assert cond, f"FAIL: {label}"
    ok += 1
    print(f"  ok  {label}")


print("config")
check("map is 75x57", (config.MAP_COLS, config.MAP_ROWS) == (75, 57))
check("torus wraps", config.wrap_q(-1) == 74 and config.wrap_r(57) == 0)
check("neighbor dir0 is +q", config.neighbor(10, 10, 0) == (11, 10))
check("river impassable", config.TERRAIN_MC[config.TERR_RIVER] == config.IMPASSABLE)
check("mp floor", config.base_mp(0) == 3 and config.base_mp(-5) == config.MP_FLOOR)

print("mapdec")
c = decode_cell(0x02 | 0x40 | 0x80, 0x09 | 0x40 | 0x80, 0x47)
check("terrain masked off flag bits", c.terrain == 2)
check("improved shelter", c.shelter == 2)
check("poi bit", c.poi is True)
check("tire track bit", c.tire_track is True)
check("resource high nibble", c.resource == 4)
check("variant low nibble", c.variant == 7)
check("footprints bitmask", c.visited_by(0) and c.visited_by(3) and not c.visited_by(1))
check("fog decodes to None", decode_cell(0xFF, 0, 0) is None)

m = WorldMap()
check("vis disk applies", m.apply_vis("0A05020009") == 1)
check("vis disk lands at q,r", m[(10, 5)] is not None and m[(10, 5)].terrain == 2)
full = "FFFFFF" * (config.MAP_ROWS * config.MAP_COLS)
stats = WorldMap().load_full(full)
check("all-fog map parses", stats["fog"] == config.MAP_ROWS * config.MAP_COLS)
try:
    WorldMap().load_full("00")
    check("short map rejected", False)
except ValueError:
    check("short map rejected", True)

print("state")
o = Observation()
sync = {"t": "sync", "id": 2, "tk": 5, "vr": 3, "map": full,
        "p": [{"id": i, "on": 1, "q": i, "r": 0, "sc": i * 10, "vm": 0b101010,
               "ll": 7, "mp": 10, "inv": [1, 2, 3, 4, 5], "sk": [2, 1, 0, 1, 1],
               "nm": config.ARCHETYPE_NAME[i], "arch": i} for i in range(6)],
        "gs": {"tc": 1, "dc": 11, "wp": 2},
        "world": {"caravan": {"q": 1, "r": 2, "active": 1, "inv": [1, 2, 3, 4, 5],
                              "stock": []},
                  "doom": {"q": 9, "r": 9, "awareness": 50},
                  "fire": [[1, 1, 2]], "flood": []},
        "gi": []}
check("sync applies", o.apply(sync) == "sync" and o.synced)
check("pid from sync", o.pid == 2)
check("day/weather from gs", (o.day, o.weather, o.threat) == (11, 2, 1))
check("me resolves", o.me.score == 20 and o.me.name == "Medic")
check("vm -> legal dirs", o.me.legal_dirs() == [1, 3, 5])
check("rivals excludes self", len(o.rivals()) == 5)
check("world block", (o.world.doom_awareness, len(o.world.fire)) == (50, 1))
check("carried sums inv", o.me.carried() == 15)

o.apply({"t": "s", "tk": 9, "p": [{"sc": 999}]})
check("broadcast patches in place", o.tick == 9 and o.players[0].score == 999)
check("broadcast leaves sync-only fields", o.players[0].name == "Guide")
o.apply({"t": "err", "msg": "Cannot move during encounter"})
check("err captured", o.last_error.startswith("Cannot move"))
o.apply({"t": "vis", "dp": 1, "cells": "0A05020009"})
check("tunnel vis ignored", o.map[(10, 5)] is None)
o.apply({"t": "enc_path", "choices": [1, 2]})
check("encounter opens", o.encounter is not None)
o.apply({"t": "ev", "k": "enc_end"})
check("encounter closes", o.encounter is None)
o.apply({"t": "ev", "k": "regen"})
check("regen clears map", not o.synced)
check("unknown type tolerated", o.apply({"t": "tsync", "whatever": 1}) == "tsync")
check("missing keys tolerated", o.apply({"t": "s"}) == "s")

print("policy")
check("move msg", Action("move", d=3).to_msg() == {"t": "m", "d": 3})
check("act msg", Action("act", a=7, mp=1).to_msg() == {"t": "act", "a": 7, "mp": 1})
check("craft carries recipe", Action("act", a=5, recipe=9).to_msg()["r"] == 9)
check("noop renders None", Action("noop").to_msg() is None)
check("bank with keep", Action("enc_bank", keep=[1, 0, 0, 0, 0]).to_msg()["keep"][0] == 1)
check("bank without keep omits it", "keep" not in Action("enc_bank").to_msg())

o2 = Observation()
o2.apply(sync)
o2.pid = 2
pol = make_policy("drunk", random.Random(7))
kinds = set()
for _ in range(400):
    act = pol.decide(o2)
    kinds.add(act.kind)
    msg = act.to_msg()
    if msg is not None:
        json.dumps(msg)   # must be serialisable
check("drunk produces moves and acts", {"move", "act"} <= kinds)
o2.encounter = {"can_bank": True}
enc_kinds = {pol.decide(o2).kind for _ in range(120)}
check("drunk resolves encounters", enc_kinds <= {"enc_bank", "enc_abort"})
o2.encounter = None
o2.players[2].ll = 0
check("downed bot noops", pol.decide(o2).kind == "noop")

print("record")
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "t.jsonl"
    with Recorder(p, {"hello": "world"}) as r:
        r.write("tx", 0, {"t": "m", "d": 1})
        r.write("rx_s", 1, {"sc": 5})
    rows = read_run(p)
    check("recorder round-trips", len(rows) == 3 and rows[0]["d"]["hello"] == "world")
    check("channels preserved", [x["ch"] for x in rows] == ["run", "tx", "rx_s"])
    p.write_text(p.read_text(encoding="utf-8") + '{"truncated', encoding="utf-8")
    check("truncated tail tolerated", len(read_run(p)) == 3)

print("telemetry")
d = digest({"day": 11, "tickId": 4560, "weather": 2, "connected": 0, "evtQueue": 0,
            "mem": {"heap": 112324, "minHeap": 43468, "maxTickMs": 95,
                    "broadcastPartial": 1195},
            "players": [{"conn": True, "score": 797}, {"conn": False, "score": 35}]})
check("digest pulls health keys", d["maxTickMs"] == 95 and d["minHeap"] == 43468)
check("digest scores only connected", d["scores"] == [797])

print(f"\n{ok} checks passed")
