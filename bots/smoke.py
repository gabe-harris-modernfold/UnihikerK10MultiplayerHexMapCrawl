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
o.apply({"t": "ev", "k": "enc_end", "pid": o.pid})
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

print("navigate")
import navigate
from mapdec import Cell


def mk(terrain=0, resource=0, poi=False, fp=0):
    return Cell(terrain=terrain, footprints=fp, shelter=0, poi=poi,
                tire_track=False, resource=resource, variant=0)


w = WorldMap()
for rr in range(config.MAP_ROWS):
    for qq in range(config.MAP_COLS):
        w.grid[rr][qq] = mk()
check("scrub costs 1", navigate.step_cost(mk(0)) == 1)
check("crater impassable", navigate.step_cost(mk(10)) is None)
check("river impassable", navigate.step_cost(mk(11)) is None)
check("fog costs UNKNOWN_COST", navigate.step_cost(None) == navigate.UNKNOWN_COST)
dist, first = navigate.dijkstra(w, 10, 10, max_cost=6)
check("dijkstra expands", len(dist) > 30)
check("dijkstra start is free", dist[(10, 10)] == 0)
check("neighbour costs 1 on scrub", dist[(11, 10)] == 1)
check("first_dir points at the neighbour", first[(11, 10)] == 0)
w.grid[10][14] = mk(resource=2)
t = navigate.best_target(w, 10, 10,
                         lambda c, q, r, cost: (20.0 / cost) if c and c.resource else None,
                         max_cost=20)
check("best_target finds the pile", t is not None and (t[0], t[1]) == (14, 10))
check("best_target returns a legal first dir", t[2] in range(6))
# Torus: q=0 and q=74 are neighbours.
check("distance wraps", navigate.hex_distance(0, 10, 74, 10) == 1)
w.grid[10][12] = None
check("frontier_bonus counts fog", navigate.frontier_bonus(w, 11, 10) == 1)

print("encounters")
import encounters as enc_mod

check("P(2d6>=2) is certain", enc_mod.p_at_least(2) == 1.0)
check("P(2d6>=13) is zero", enc_mod.p_at_least(13) == 0.0)
check("P(2d6>=7) is 21/36", abs(enc_mod.p_at_least(7) - 21 / 36) < 1e-9)
check("DN floor is 2", enc_mod.compute_dn(0, 0, 7, 0) == 2)
check("DN clamps at 12", enc_mod.compute_dn(100, 20, 0, 20) == 12)
check("risk 50 -> DN 7 before mods", enc_mod.compute_dn(50, 0, 4, 0) == 7)
check("high LL lowers DN", enc_mod.compute_dn(50, 0, 8, 0) < enc_mod.compute_dn(50, 0, 4, 0))
check("radiation raises DN", enc_mod.compute_dn(50, 0, 4, 9) > enc_mod.compute_dn(50, 0, 4, 0))
check("threat clock raises DN", enc_mod.compute_dn(50, 20, 4, 0) > enc_mod.compute_dn(50, 0, 4, 0))

lib = enc_mod.EncounterLibrary()
n = lib.load_all()
check("encounter library loads from repo", n > 50)
check("no malformed encounter files", not lib.failed)
check("index.json parsed", len(lib.index) >= 10)
check("nodes present", lib.node_count() > 100)
sample = lib.get("dunes", 1)
check("named encounter loads", sample is not None and "nodes" in sample)
run = enc_mod.EncounterRun(lib, "dunes", 1)
check("run starts at start_node", run.node_key == sample["start_node"])
check("start node has choices", len(run.choices) >= 1)
run.choose(0)
run.on_result({"out": 1})
check("success advances the node", run.node_key != sample["start_node"])
before = run.node_key
run.choose(0)
run.on_result({"out": 0})
check("failure does not advance", run.node_key == before)

print("policies")
import policy as pmod

check("registry has all five", set(pmod.REGISTRY) ==
      {"drunk", "scoremax", "contentmax", "coward", "rival"})

o3 = Observation()
o3.apply(sync)
o3.pid = 2
o3.map = w
o3.players[2].q, o3.players[2].r = 10, 10
o3.players[2].inv = [5, 3, 0, 0, 0]   # above WATER_FLOOR/FOOD_FLOOR, so the
                                      # survival floor does not pre-empt pursue()
o3.players[2].inv_slots = 8
o3.players[2].valid_moves = 0b111111
for pname in sorted(pmod.REGISTRY):
    pol = pmod.make(pname, random.Random(3))
    kinds = set()
    for _ in range(60):
        act = pol.decide(o3)
        kinds.add(act.kind)
        m = act.to_msg()
        if m is not None:
            json.dumps(m)
    check(f"{pname} yields valid actions", kinds and kinds <= {
        "move", "act", "noop", "enc_start", "enc_choice", "enc_bank",
        "enc_abort", "use_item", "trade_offer"})

sm = pmod.make("scoremax", random.Random(3))
check("scoremax walks toward the pile", sm.decide(o3).kind == "move")
# Survival pre-empts pursuit: starve it and it forages instead of walking.
o3.players[2].inv = [5, 0, 0, 0, 0]
check("hunger pre-empts pursuit", sm.decide(o3).to_msg()["a"] == config.ACT_FORAGE)
o3.players[2].inv = [5, 3, 0, 0, 0]
o3.players[2].ll = 0
check("scoremax noops when downed", sm.decide(o3).kind == "noop")
o3.players[2].ll = 7
o3.players[2].mp = 0
check("out of MP -> rest", sm.decide(o3).to_msg()["a"] == config.ACT_REST)
o3.players[2].mp = 10

cm = pmod.make("contentmax", random.Random(3))
w.grid[10][10] = mk(poi=True)
check("contentmax opens a POI underfoot", cm.decide(o3).kind == "enc_start")
w.grid[10][10] = mk()
cw = pmod.make("coward", random.Random(3))
w.grid[10][11] = mk(poi=True)
moves = {cw.decide(o3).kind for _ in range(30)}
check("coward never opens encounters", "enc_start" not in moves)
w.grid[10][11] = mk()

o3.encounter = {"biome": "dunes", "id": 1}
cm2 = pmod.make("contentmax", random.Random(3))
a = cm2.decide(o3)
check("encounter binds to local json", a.kind in ("enc_choice", "enc_bank", "enc_abort"))
check("content stats accumulate", cm2.content_score()["encounters_opened"] == 1)
o3.encounter = {"biome": "nosuch", "id": 999}
cm3 = pmod.make("contentmax", random.Random(3))
check("unknown encounter banks out", cm3.decide(o3).kind == "enc_bank")
o3.encounter = None

print("loop guards (regressions from the first four-policy run)")
o4 = Observation()
o4.apply(sync)
o4.pid = 2
o4.map = w
o4.day = 5
me4 = o4.players[2]
me4.q, me4.r, me4.inv, me4.inv_slots = 10, 10, [5, 3, 0, 0, 0], 8
me4.valid_moves, me4.ll, me4.mp = 0b111111, 7, 0
g = pmod.make("scoremax", random.Random(3))
g.set_pid(2)
first = g.decide(o4)
check("mp 0 -> rest once", first.to_msg()["a"] == config.ACT_REST)
check("second call does not re-rest",
      all(g.decide(o4).to_msg() != first.to_msg() for _ in range(20)))
o4.day = 6
check("new day re-arms rest", g.decide(o4).to_msg()["a"] == config.ACT_REST)

# ev is broadcast to everyone, so an unfiltered policy counts rivals' rolls.
g2 = pmod.make("contentmax", random.Random(3))
g2.set_pid(2)
for _ in range(5):
    g2.on_event({"t": "ev", "k": "enc_res", "pid": 4, "out": 1, "rec": 1})
check("other players' rolls ignored", g2.content_score()["rolls"] == 0)
g2.on_event({"t": "ev", "k": "enc_res", "pid": 2, "out": 1, "rec": 1})
check("own rolls counted", g2.content_score()["rolls"] == 1)

o5 = Observation()
o5.pid = 2
o5.apply({"t": "ev", "k": "enc_start", "pid": 2})
check("enc_start does not fabricate an encounter", o5.encounter is None)
o5.apply({"t": "enc_path", "biome": "dunes", "id": 1})
check("enc_path opens the encounter", o5.encounter is not None)
o5.apply({"t": "ev", "k": "enc_end", "pid": 4})
check("another player's enc_end is ignored", o5.encounter is not None)
o5.apply({"t": "ev", "k": "enc_end", "pid": 2})
check("own enc_end closes it", o5.encounter is None)

o5.players[2].resting = True
o5.apply({"t": "ev", "k": "dawn", "pid": 2, "day": 9})
check("dawn clears resting", o5.players[2].resting is False and o5.day == 9)

# A consumed POI leaves a stale bit in our map and enc_start is silent.
g3 = pmod.make("contentmax", random.Random(3))
g3.set_pid(2)
w.grid[10][10] = mk(poi=True)
o4.day, me4.mp = 7, 10
opens = [g3.decide(o4).kind for _ in range(10)]
check("stale POI does not loop forever", opens.count("enc_start") <= 2)
check("falls through to movement", "move" in opens)
w.grid[10][10] = mk()

print("staple emergency (the day-7 thirst death)")
o6 = Observation()
o6.apply(sync)
o6.pid = 2
o6.day = 3
w2 = WorldMap()
for rr in range(config.MAP_ROWS):
    for qq in range(config.MAP_COLS):
        w2.grid[rr][qq] = mk()
w2.grid[10][16] = mk(resource=config.RES_WATER + 1)   # a pond 6 hexes east
o6.map = w2
me6 = o6.players[2]
me6.q, me6.r, me6.ll, me6.mp = 10, 10, 5, 6
me6.valid_moves, me6.inv_slots = 0b111111, 8
me6.inv = [0, 3, 0, 0, 0]                              # bone dry, food fine
for pname in ("scoremax", "coward", "rival", "contentmax"):
    p6 = pmod.make(pname, random.Random(3))
    p6.set_pid(2)
    a6 = p6.decide(o6)
    check(f"{pname} hunts water when dry",
          a6.kind == "move" and "EMERGENCY water" in a6.why)
# Thirst must outrank resting: dawn drinks 2 water whether you moved or not.
me6.ll = 1
p7 = pmod.make("coward", random.Random(3))
p7.set_pid(2)
check("thirst outranks resting", "EMERGENCY water" in p7.decide(o6).why)
# With MP gone there is no choice but to rest.
me6.mp = 0
check("no MP -> rest anyway", p7.decide(o6).to_msg()["a"] == config.ACT_REST)
# Standing on water, drink instead of walking.
me6.mp = 6
w2.grid[10][10] = mk(terrain=3)                        # Marsh has water
check("drinks on water terrain",
      pmod.make("scoremax", random.Random(3)).decide(o6).to_msg()["a"] == config.ACT_WATER)
w2.grid[10][10] = mk()

rv = pmod.make("rival", random.Random(3))
rv._since_trade = 999
o3.players[0].connected, o3.players[0].ll = True, 7
o3.players[0].q, o3.players[0].r = 11, 10
tr = None
for _ in range(20):
    act = rv.decide(o3)
    if act.kind == "trade_offer":
        tr = act
        break
check("rival makes a lowball offer", tr is not None and sum(tr.want) > sum(tr.give))

print(f"\n{ok} checks passed")
