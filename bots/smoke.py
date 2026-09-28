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
o.apply({"t": "s", "tk": 10, "p": [{"id": 2, "on": 1, "sc": 77},
                                   {"id": 4, "on": 1, "sc": 55}]})
check("sparse broadcast lands by id",
      (o.players[2].score, o.players[4].score) == (77, 55))
check("sparse broadcast marks unlisted seats offline",
      [p.connected for p in o.players] == [False, False, True, False, True, False])
o.apply({"t": "err", "msg": "Cannot move during encounter"})
check("err captured", o.last_error.startswith("Cannot move"))
o.apply({"t": "vis", "dp": 1, "cells": "0A05020009"})
check("tunnel vis stays off the surface map", o.map[(10, 5)] is None)
check("tunnel vis lands on the tunnel board", o.tunnel[(10, 5)] is not None)
o.apply({"t": "enc_path", "choices": [1, 2]})
check("encounter opens", o.encounter is not None)
o.apply({"t": "ev", "k": "enc_end", "pid": o.pid})
check("encounter closes", o.encounter is None)
o.apply({"t": "ev", "k": "regen"})
check("regen clears map", not o.synced)
check("tsync without a map is tolerated",
      o.apply({"t": "tsync", "whatever": 1}) == "tsync")
check("unknown type tolerated", o.apply({"t": "nosuchtype"}) == "nosuchtype")
check("missing keys tolerated", o.apply({"t": "s"}) == "s")

print("eco")
from navigate import DAISY_BLOOM_COST, DAISY_BUD_COST
eco = {"t": "eco", "tk": 40, "n": "ASH-VESSEL GREY", "h": 212, "g": 9, "w": 1,
       "v": "0A" * (config.MAP_COLS * config.MAP_ROWS), "f": [[3, 4, 2, 77]],
       "dz": [[10, 5, 3, 2, 7], [11, 5, 2, 1, 3], [12, 5, 1, 1, 3]], "sp": []}
check("eco applies", o.apply(eco) == "eco" and o.eco_name == "ASH-VESSEL GREY")
check("eco keeps the daisies", o.daisies[(10, 5)] == (3, 2) and (12, 5) in o.daisies)
check("bloomed patch is priced", o.map.extra_cost.get((10, 5)) == DAISY_BLOOM_COST)
check("bud is priced lower", o.map.extra_cost.get((11, 5)) == DAISY_BUD_COST)
check("shoot costs nothing", (12, 5) not in o.map.extra_cost)
check("scar-only eco message keeps the daisies",
      o.apply({"t": "eco", "tk": 41, "s": "0" * 10}) == "eco" and (10, 5) in o.daisies)
check("eco without dz keeps the daisies",
      o.apply({"t": "eco", "tk": 42, "w": 2}) == "eco" and o.map.extra_cost)
check("empty dz clears them", o.apply({"t": "eco", "tk": 43, "dz": []}) == "eco"
      and not o.daisies and not o.map.extra_cost)
o.apply(eco)
o.apply({"t": "ev", "k": "regen"})
check("regen clears the daisies", not o.daisies and not o.map.extra_cost)
check("daisy bite is a known cause",
      "wasteland daisy" in __import__("causes").CAUSE_ORDER)

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
# A bloomed daisy patch on (11,10): the straight line to (12,10) now costs
# 1 + DAISY_BLOOM_COST + 1, so the three-step detour around it wins.
w.grid[10][12] = mk()
w.extra_cost = {(11, 10): navigate.DAISY_BLOOM_COST}
dist, first = navigate.dijkstra(w, 10, 10, max_cost=20)
check("a bloomed hex costs its bite", dist[(11, 10)] == 1 + navigate.DAISY_BLOOM_COST)
check("the path goes around the flowers", dist[(12, 10)] == 3 and first[(12, 10)] != 0)
w.extra_cost = {}

print("encounters")
import encounters as enc_mod

check("P(2d6>=2) is certain", enc_mod.p_at_least(2) == 1.0)
check("P(2d6>=13) is zero", enc_mod.p_at_least(13) == 0.0)
check("P(2d6>=7) is 21/36", abs(enc_mod.p_at_least(7) - 21 / 36) < 1e-9)
check("DN clamps at 12", enc_mod.compute_dn(100, 20, 0, 20) == 12)
# The curve itself (5 + risk*7/100), pinned at the points that matter: risk 30
# is the median of the authored library, and it used to be DN 5 -- a 92% pass.
check("zero risk is DN 5 before mods", enc_mod.compute_dn(0, 0, 4, 0) == 5)
check("median authored risk 30 -> DN 7", enc_mod.compute_dn(30, 0, 4, 0) == 7)
check("risk 50 -> DN 8 before mods", enc_mod.compute_dn(50, 0, 4, 0) == 8)
check("risk 100 tops the curve at 12", enc_mod.compute_dn(100, 0, 4, 0) == 12)
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

check("registry has every policy", set(pmod.REGISTRY) ==
      {"drunk", "scoremax", "contentmax", "coward", "rival",
       "subterranean", "tunnelrunner", "sentinel", "gearmax"})
# GearMax is the only policy that will wear a net-negative item, which is
# what makes its runs usable for auditing whether penalties land at all.
check("only gearmax wears junk on purpose",
      pmod.make("gearmax", random.Random(1)).equip_anything
      and not pmod.make("scoremax", random.Random(1)).equip_anything)

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
# enc_start REQUIRES q/r -- handleMsg_enc_start bails at the first strstr
# without them, with no error reply. Every POI attempt in every early run was
# discarded this way, which looked exactly like POIs being unreachable.
check("enc_start carries q/r",
      Action("enc_start", q=7, r=9).to_msg() == {"t": "enc_start", "q": 7, "r": 9})
try:
    Action("enc_start").to_msg()
    check("enc_start without q/r is rejected", False)
except ValueError:
    check("enc_start without q/r is rejected", True)

o3.encounter = {"biome": "dunes", "id": 1}
cm2 = pmod.make("contentmax", random.Random(3))
a = cm2.decide(o3)
check("encounter binds to local json", a.kind in ("enc_choice", "enc_bank", "enc_abort"))
# Drive the first branch until the node is bankable, then take the haul.
check("takes the first option", a.kind == "enc_choice" and a.ci == 0)
cm2.run.node_key = [k for k, n in cm2.run.enc["nodes"].items() if n.get("can_bank")][0]
check("banks once bankable", cm2.decide(o3).kind == "enc_bank")
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
# A REST decided just before a reconnect never reaches the board, but the bot
# has already marked itself rested. With a hard per-day latch it then sits at
# mp 0 and awake forever -- and one awake player stops tickGame() ending the
# day early for the whole fleet. Froze five bots at day 16 in a live run.
import policy.survivor as _surv
g._last_rest_sent -= (_surv.REST_RETRY_S + 1.0)
check("dropped REST retries after cooldown",
      g.decide(o4).to_msg()["a"] == config.ACT_REST)
check("but not before the cooldown elapses",
      all(g.decide(o4).to_msg() != {"t": "act", "a": config.ACT_REST, "mp": 1}
          for _ in range(5)))
# Server-confirmed resting (only ever seen via sync) suppresses it outright.
me4.resting = True
check("confirmed resting suppresses retry", g.rest_once(o4, "x") is None)
me4.resting = False

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

print("shelter arm (the counter-play to the biggest LL drain)")
check("SV table matches the firmware",
      config.TERRAIN_SV == (0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 0, 2, 0, 1, 0))
check("Settlement/Mountain/Flooded are covered",
      not any(config.is_exposed(t) for t in (5, 8, 9, 12)))
check("Scrub is exposed", config.is_exposed(0))

o7 = Observation()
o7.apply(sync)
o7.pid, o7.day = 2, 3
w3 = WorldMap()
for rr in range(config.MAP_ROWS):
    for qq in range(config.MAP_COLS):
        w3.grid[rr][qq] = mk()
o7.map = w3
me7 = o7.players[2]
me7.q, me7.r, me7.ll = 10, 10, 5
me7.valid_moves, me7.inv_slots, me7.archetype = 0b111111, 8, 2
me7.inv = [4, 4, 0, 0, 2]                              # supplies fine, 2 scrap
me7.mp = 1                                             # last move of the day

ps = pmod.make("scoremax", random.Random(3))
ps.set_pid(2)
check("shelter is off by default", ps.decide(o7).to_msg().get("a") != config.ACT_SHELTER)
ps.shelter_when_exposed = True
check("shelters on the last MP when exposed",
      ps.decide(o7).to_msg()["a"] == config.ACT_SHELTER)
me7.mp = 5
check("does not burn a mid-day move on it",
      ps.decide(o7).to_msg().get("a") != config.ACT_SHELTER)
me7.mp = 1
me7.inv = [4, 4, 0, 0, 0]                              # no scrap
check("no scrap, no shelter", ps.decide(o7).to_msg().get("a") != config.ACT_SHELTER)
me7.inv = [4, 4, 0, 0, 2]
w3.grid[10][10] = mk(terrain=9)                        # Settlement, SV 3
check("no shelter where SV >= 2",
      ps.decide(o7).to_msg().get("a") != config.ACT_SHELTER)
w3.grid[10][10] = mk()
me7.archetype = 5                                      # Endurer is exempt
check("Endurer does not waste scrap on shelter",
      ps.decide(o7).to_msg().get("a") != config.ACT_SHELTER)
me7.archetype = 2
# Dry on the last MP: one step cannot reach water, but the shelter zeroes the
# whole dawn loss, so it has to outrank the emergency hunt *here* only.
me7.inv = [0, 4, 0, 0, 2]
w3.grid[10][16] = mk(resource=config.RES_WATER + 1)
check("shelter beats the water hunt on the last MP",
      ps.decide(o7).to_msg().get("a") == config.ACT_SHELTER)
me7.mp = 6
check("but the water hunt still wins earlier in the day",
      ps.decide(o7).kind == "move")
w3.grid[10][16] = mk()
me7.mp = 1

print("exposure never lands the killing blow")
# dawnUpkeep applies exposure last and floors it at LL 1, so a dawn whose only
# loss is exposure can never report ll 0 -- and when it floors out it reports
# expd 0 rather than -1, so the wire stays an honest record of what landed.
# This is the deaths-vs-near-misses fix; if a future edit folds exposure back
# into the lump sum, the ratio silently regresses from ~1:4.6 to ~1:1.2.
EXPOSURE_BITE = 2   # mirrors survival_state.hpp


def _dawn_apply(ll, food_water_delta, exposed, bite=EXPOSURE_BITE):
    """Port of the firmware's apply block, for the invariant only."""
    exp_loss = bite if exposed else 0
    downed = False
    if food_water_delta < 0:
        for _ in range(-food_water_delta):
            if ll > 0:
                ll -= 1
            if ll == 0:
                downed = True
                break
    elif food_water_delta > 0:
        ll = min(ll + food_water_delta, 7)
    landed = 0
    if not downed and exp_loss > 0:
        while landed < exp_loss and ll > 1:
            ll -= 1
            landed += 1
    return ll, -landed, downed

check("exposure bites 2 from a healthy survivor", _dawn_apply(5, 0, True)[0] == 3)
check("exposure wounds down to 1, not past it", _dawn_apply(2, 0, True)[0] == 1)
check("exposure cannot take the last point", _dawn_apply(1, 0, True)[0] == 1)
check("floored exposure reports expd 0", _dawn_apply(1, 0, True)[1] == 0)
# A partial bite must report what LANDED, not what was owed -- at LL 2 only
# one of the two points can be taken, and causes.py attributes from this.
check("a partial bite reports expd -1", _dawn_apply(2, 0, True)[1] == -1)
check("a full bite reports expd -2", _dawn_apply(5, 0, True)[1] == -2)
check("thirst/hunger CAN still kill", _dawn_apply(1, -1, False)[2] is True)
check("thirst kills even with exposure pending", _dawn_apply(1, -1, True)[2] is True)

print("bad air is the tunnels' half of the trade")
# Sleeping underground turns the exposure tick off entirely and rolls
# TUNNEL_REST_LL_PCT for a LL instead. Two properties hold that trade
# together, and both are easy to break by accident:
#
#   1. The bad-air point rides the ORDINARY loss path, not exposure's floored
#      one. Route it through the floor "for consistency" and the tunnels
#      become the one place attrition cannot kill -- an absorbing state at
#      LL 1, which is the failure survival_state.hpp's rest-recovery note
#      spends thirty lines explaining.
#   2. It is netted against the rest-heal first, like every other dawn loss.
#      So a stocked survivor absorbs it and an empty one does not, which is
#      what makes "surface before the supplies run out" the real decision
#      rather than "never sleep down there".
def _dawn_below(ll, heal, air, bite=EXPOSURE_BITE):
    """Port of the apply block for a rest at depth 1: exposure is off, the
    heal and the bad-air point net, and the result goes through the loss
    loop that CAN reach 0."""
    return _dawn_apply(ll, heal - (1 if air else 0), False, bite)

check("no exposure underground, whatever the hatch overhead",
      _dawn_below(5, 0, False)[0] == 5)
check("bad air alone costs one", _dawn_below(5, 0, True)[0] == 4)
check("bad air CAN take the last point", _dawn_below(1, 0, True)[2] is True)
check("a stocked survivor absorbs it", _dawn_below(4, 1, True)[0] == 4)
# The wounded heal is EXPOSURE_BITE + 1, so it beats bad air by more than it
# beats exposure -- climbing out of the hole works below ground too.
check("the badly hurt still climb out below",
      _dawn_below(2, EXPOSURE_BITE + 1, True)[0] == 2 + EXPOSURE_BITE)
# The whole point of the trade: an unsheltered surface night is worse in
# expectation than a night below, and a built shelter still beats both.
check("a bunker beats open ground",
      (EXPOSURE_BITE) > (config.TUNNEL_REST_LL_PCT / 100.0))
# The wounded MUST be able to climb out. A heal that merely MATCHES the
# exposure tick nets zero and pins them at LL 1 -- measured at 37% of all time
# on v4, an absorbing state that collapsed the brush count. The firmware ties
# the wounded heal to EXPOSURE_BITE + 1 so this survives retuning the bite;
# these check the property, not the literal.
WOUNDED_HEAL = EXPOSURE_BITE + 1
check("resting at LL 1 nets +1 against exposure",
      _dawn_apply(1, WOUNDED_HEAL, True)[0] == 2)
check("resting at LL 2 nets +1 against exposure",
      _dawn_apply(2, WOUNDED_HEAL, True)[0] == 3)
# ...and it must hold for any bite, which is what the coupling buys.
for _bite in (1, 2, 3):
    _ll, _, _ = _dawn_apply(1, _bite + 1, True, bite=_bite)
    check(f"climb-out holds at EXPOSURE_BITE={_bite}", _ll == 2)
# A healthy survivor gets only +1, so resting does NOT outrun a bite of 2 --
# that downward drift at full health is what generates the dips.
check("a healthy survivor drifts down while exposed",
      _dawn_apply(5, +1, True)[0] == 4)

print("cause attribution")
from causes import DamageLedger

def _ev(ts, d, arch=0):
    return {"ts": ts, "ch": "rx", "arch": arch, "d": dict(d, t="ev")}

# The killing dawn arrives ~15ms *after* its own downed -- dawnUpkeep enqueues
# EVT_DOWNED inside the loss loop and EVT_DAWN at the end of the function.
rows = [
    _ev(1.0, {"k": "dawn", "pid": 0, "day": 2, "f": 6, "w": 3, "ll": 5,
              "dll": -1, "fth": 0, "wth": 1, "expd": 0}),
    _ev(2.0, {"k": "dawn", "pid": 0, "day": 3, "f": 6, "w": 1, "ll": 3,
              "dll": -2, "fth": 0, "wth": 3, "expd": -1}),
    {"ts": 3.0, "ch": "rx", "arch": 0, "d": {"t": "ev", "k": "downed", "pid": 0}},
    _ev(3.015, {"k": "dawn", "pid": 0, "day": 4, "f": 6, "w": 1, "ll": 0,
                "dll": -1, "fth": 0, "wth": 3, "expd": -1}),
]
led = DamageLedger(rows)
d0 = led.deaths()
check("one death found", len(d0) == 1)
check("dawn after the downed still explains it", d0[0]["cause"] == "exposure")
loss = led.loss_ledger()
check("latched threshold bits are not recounted",
      round(loss.get("thirst", 0), 3) == 2.0)
check("exposure counted once per dawn", round(loss.get("exposure", 0), 3) == 2.0)

# A re-picked slot gets a fresh survivor with the threshold mask zeroed, so
# the same bits break again and must be counted again.  There is no player
# respawn event to key off -- the mask simply drops, and carrying the last
# reported value forward is what handles it.
rows2 = rows + [
    _ev(10.0, {"k": "dawn", "pid": 0, "day": 9, "f": 6, "w": 6, "ll": 7,
               "dll": 0, "fth": 0, "wth": 0, "expd": 0}),
    _ev(11.0, {"k": "dawn", "pid": 0, "day": 10, "f": 6, "w": 3, "ll": 6,
               "dll": -1, "fth": 0, "wth": 1, "expd": 0}),
]
check("thirst is recounted for a fresh survivor",
      round(DamageLedger(rows2).loss_ledger().get("thirst", 0), 3) == 3.0)

# Broadcast events arrive once per connected bot; the ledger must not
# multiply them.
dupes = [_ev(5.0 + i * 0.002, {"k": "fire_dmg", "pid": 1, "q": 1, "r": 1,
                               "intensity": 3}, arch=i) for i in range(5)]
# Five copies of one 2-LL blaze must total 2, not 10.
check("broadcast copies dedupe to one",
      round(DamageLedger(dupes).loss_ledger().get("fire", 0), 3) == 2.0)
strike = [_ev(5.0, {"k": "fire_dmg", "pid": 1, "q": 1, "r": 1, "intensity": 10})]
check("intensity 10 is the lightning sentinel, worth 2 LL",
      DamageLedger(strike).loss_ledger().get("lightning") == 2)
blaze = [_ev(5.0, {"k": "fire_dmg", "pid": 1, "q": 1, "r": 1, "intensity": 3})]
check("a blaze (intensity 3) costs 2 LL, a burn costs 1",
      DamageLedger(blaze).loss_ledger().get("fire") == 2)

# A flash flood zeroes MP *and* takes LL; it used to cost nothing at all, so
# a missing llLost must read as 0 rather than defaulting to 1.
flood = [_ev(5.0, {"k": "flood_dmg", "pid": 1, "q": 1, "r": 1,
                   "intensity": 10, "llLost": 1}),
         {"ts": 5.1, "ch": "rx", "arch": 1,
          "d": {"t": "ev", "k": "downed", "pid": 1}}]
lf = DamageLedger(flood)
check("flood damage is attributed", lf.loss_ledger().get("flood") == 1)
check("a flood can now be a cause of death", lf.deaths()[0]["cause"] == "flood")
old = [_ev(5.0, {"k": "flood_dmg", "pid": 1, "q": 1, "r": 1, "intensity": 10})]
check("a pre-fix flood_dmg with no llLost still scores 0",
      DamageLedger(old).loss_ledger().get("flood", 0) == 0)

# Chem and fog take LL with no event at all; the only evidence is the
# weather phase in the sampled digest.
silent = [
    {"ts": 1.0, "ch": "rx_s", "arch": 0,
     "d": {"tk": 50, "day": 2, "wp": config.WEATHER_CHEM, "ll": 3}},
    {"ts": 2.0, "ch": "rx", "arch": 0, "d": {"t": "ev", "k": "downed", "pid": 0}},
]
check("silent weather death is inferred from the phase",
      DamageLedger(silent).deaths()[0]["cause"] == "chem storm")
silent[0]["d"]["wp"] = config.WEATHER_CLEAR
check("and is not invented when the weather was clear",
      DamageLedger(silent).deaths()[0]["cause"] == "unattributed")

print("bunker tunnels -- the second board")
# Everything here guards a way the tunnel board is NOT the surface board.
# Each of these is silent when it breaks: a tunnel coordinate read against
# the surface map returns a real, plausible cell 60 columns away.
from config import (TERR_BUNKER, TERR_COLLAPSED, TERR_TUNNEL, TERR_VENT,
                    TUN_COLS, TUN_ROWS)

check("tunnel board is 16x10", (config.TUN_COLS, config.TUN_ROWS) == (16, 10))
check("the tunnel board does not wrap",
      not config.tun_in(-1, 4) and not config.tun_in(16, 4) and config.tun_in(15, 9))
check("tunnel distance does not search the wrap",
      config.tun_distance(0, 4, 15, 4) == 15)
check("Tunnel Floor costs 2 MP", config.TERRAIN_MC[TERR_TUNNEL] == config.TUNNEL_MC)
check("Collapsed Tunnel is impassable",
      config.TERRAIN_MC[TERR_COLLAPSED] == config.IMPASSABLE)
check("a Vent Shaft costs double to climb",
      config.ascend_cost(TERR_VENT) == 2 and config.ascend_cost(TERR_BUNKER) == 1)
check("hatches live on both boards",
      config.is_hatch_terrain(12) and config.is_hatch_terrain(13)
      and not config.is_hatch_terrain(14))
# is_exposed() is a SURFACE terrain question. dawnUpkeep() used to apply it to
# an underground survivor as well -- q/r stay pinned to the hatch they came
# down, so SV 2 vs SV 0 decided whether a night in a corridor was sheltered,
# with no shelter buildable to fix it. It now treats depth != 0 as cover
# outright, so the hatch type no longer touches LL. Standing ON either hatch,
# on the surface, still differs, and that is what these two check.
check("standing on a Bunker Entrance is sheltered",
      not config.is_exposed(TERR_BUNKER))
check("standing on a Vent Shaft is not", config.is_exposed(TERR_VENT))
check("SHELTER/CRAFT/SURVEY are refused underground, REST is not",
      config.ACTS_REFUSED_UNDERGROUND ==
      {config.ACT_SHELTER, config.ACT_CRAFT, config.ACT_SURVEY})
check("bad air is the price of sleeping below",
      0 < config.TUNNEL_REST_LL_PCT < 100)
check("Tunnel Floor salvages but does not forage",
      config.can_salvage(TERR_TUNNEL) and not config.can_forage(TERR_TUNNEL))
check("Tunnel Floor waters", config.has_water(TERR_TUNNEL))
check("salvaging underground does not tick the threat clock",
      config.TERRAIN_IS_RUINS[TERR_TUNNEL] == 0)

# A walled board: off the edge is rock, not the far wall.
tb = WorldMap(TUN_ROWS, TUN_COLS, wraps=False)
check("off-board reads as None", tb[(16, 4)] is None and tb[(-1, 4)] is None)
check("in_bounds separates rock from fog",
      not tb.in_bounds(16, 4) and tb.in_bounds(15, 9))
check("the surface still wraps", WorldMap().in_bounds(999, 999))


def tun_payload(cells):
    """Encode a whole tunnel board the way encodeTunnelFog() does."""
    out = []
    for rr in range(TUN_ROWS):
        for qq in range(TUN_COLS):
            tt, dd, vv = cells.get((qq, rr), (0xFF, 0, 0))
            out.append("%02X%02X%02X" % (tt, dd, vv))
    return "".join(out)


ot = Observation()
ot.pid = 2
ot.apply(sync)
ot.players[2].depth = 1
ot.apply({"t": "tsync", "cols": 16, "rows": 10, "vr": 1, "q": 3, "r": 3,
          "map": tun_payload({(3, 3): (TERR_BUNKER, 0, 0),
                              (3, 4): (TERR_TUNNEL, 0, 0x10)})})
check("tsync parses the whole board", ot.tunnel_synced)
check("tsync places us on the tunnel board", ot.tunnel_pos == (3, 3))
check("tsync decodes terrain", ot.tunnel[(3, 3)].terrain == TERR_BUNKER)
check("tsync decodes a pile", ot.tunnel[(3, 4)].resource == 1)
check("tsync leaves the surface map alone", ot.map[(3, 3)] is None)
check("board switches with depth", ot.board is ot.tunnel and ot.underground)
check("pos() is the tunnel position, not the pinned surface one",
      ot.pos() == (3, 3) and (ot.me.q, ot.me.r) == (2, 0))
check("here() reads the tunnel cell", ot.here().terrain == TERR_BUNKER)
check("at_hatch sees the shaft", ot.at_hatch())
# A second tsync (every descent sends one) must not wipe what we mapped.
ot.apply({"t": "tsync", "cols": 16, "rows": 10, "vr": 1, "q": 3, "r": 3,
          "map": tun_payload({(3, 3): (TERR_BUNKER, 0, 0)})})
check("a later tsync does not forget the corridor",
      ot.tunnel[(3, 4)] is not None)
ot.players[2].depth = 0
check("surfacing puts us back on the surface board", ot.board is ot.map)

# Walls between tunnel cells (tunnels.hpp "Open sides"): (3,3) and (4,3) sit
# side by side with rock between them, so the way across is round by
# (3,4) and (4,4).  op rides tsync as 2 hex chars a cell.
_ops = {(3, 3): 1 << 5, (3, 4): (1 << 2) | (1 << 0),
        (4, 4): (1 << 3) | (1 << 2), (4, 3): 1 << 5}
ow = Observation()
ow.pid = 2
ow.apply(sync)
ow.players[2].depth = 1
ow.apply({"t": "tsync", "cols": 16, "rows": 10, "vr": 1, "q": 3, "r": 3,
          "map": tun_payload({c: (TERR_TUNNEL, 0, 0) for c in _ops}),
          "op": "".join("%02X" % _ops.get((qq, rr), 0)
                        for rr in range(TUN_ROWS) for qq in range(TUN_COLS))})
check("tsync decodes each cell's open sides", ow.tunnel[(3, 4)].op == 0b101)
from navigate import dijkstra as _dj
_d, _f = _dj(ow.tunnel, 3, 3)
check("the pathfinder goes round a wall, not through it",
      _d.get((4, 3)) == 3 * config.TERRAIN_MC[TERR_TUNNEL] and _f.get((4, 3)) == 5)
ow.apply({"t": "vis", "dp": 1, "q": 3, "r": 3, "vr": 0, "cells": "0303" + "%02X0000" % TERR_TUNNEL,
          "op": "21"})
check("a tunnel vis disk carries op too", ow.tunnel[(3, 3)].op == 0x21)

# Hatch pairings are never sent -- they are inferred from tun_in / tun_out.
oh = Observation()
oh.pid = 2
oh.apply(sync)
oh.apply({"t": "ev", "k": "tun_in", "pid": 2, "q": 40, "r": 12, "hatch": 3})
check("tun_in learns the surface hatch",
      oh.hatches[3].sq == 40 and oh.hatches[3].sr == 12)
check("the pairing is not complete until we know where we came out",
      not oh.hatches[3].paired)
oh.apply({"t": "tsync", "cols": 16, "rows": 10, "vr": 1, "q": 5, "r": 6,
          "map": tun_payload({(5, 6): (TERR_BUNKER, 0, 0)})})
check("tsync completes the pairing",
      oh.hatches[3].paired and (oh.hatches[3].tq, oh.hatches[3].tr) == (5, 6))
check("hatch_for_shaft finds it by tunnel coords",
      oh.hatch_for_shaft(5, 6).idx == 3)
oh.players[2].depth = 1
oh.apply({"t": "ev", "k": "mv", "pid": 2, "q": 9, "r": 6, "dp": 1})
check("our own dp=1 mv tracks us underground", oh.tunnel_pos == (9, 6))
oh.apply({"t": "ev", "k": "mv", "pid": 4, "q": 1, "r": 1, "dp": 1})
check("another player's tunnel step is not ours", oh.tunnel_pos == (9, 6))
oh.apply({"t": "ev", "k": "tun_out", "pid": 2, "q": 61, "r": 40, "hatch": 5})
check("tun_out pairs the shaft we climbed",
      oh.hatches[5].paired and (oh.hatches[5].tq, oh.hatches[5].tr) == (9, 6)
      and (oh.hatches[5].sq, oh.hatches[5].sr) == (61, 40))
# Other players' crossings teach us the surface half for free -- the client
# needs it to draw the "went below here" marker, so it is broadcast.
oh.apply({"t": "ev", "k": "tun_in", "pid": 4, "q": 7, "r": 7, "hatch": 1})
check("we learn hatches from other players", oh.hatches[1].sq == 7)
oh.apply({"t": "ev", "k": "regen"})
check("regen forgets the tunnels too",
      not oh.hatches and not oh.tunnel_synced and oh.tunnel_pos is None)

print("tunnel pathing")
# A hatch is entered, never traversed: the crossing happens as the last act
# of the step that lands on it. A route THROUGH one does not exist.
from policy.survivor import ends_journey

# Fully known rock with one carved corridor: fog is traversable at
# UNKNOWN_COST, so an unexplored board would make everything "reachable" and
# the wrap check meaningless.
tw = WorldMap(TUN_ROWS, TUN_COLS, wraps=False)
for rr in range(TUN_ROWS):
    for qq in range(TUN_COLS):
        tw.grid[rr][qq] = mk(TERR_COLLAPSED)
for qq in range(2, 13):
    tw.grid[4][qq] = mk(TERR_TUNNEL)
tw.grid[3][3] = mk(TERR_BUNKER)          # spur off the corridor
tw.grid[3][11] = mk(TERR_VENT)           # the far spur
dist, _ = navigate.dijkstra(tw, 3, 4, max_cost=60, stop_at=ends_journey)
check("solid rock is not walkable", (15, 4) not in dist)
# q=0 and q=15 are neighbours on the surface torus; underground they are two
# opposite walls.
tw.grid[4][0] = mk(TERR_TUNNEL)
tw.grid[4][15] = mk(TERR_TUNNEL)
edge, _ = navigate.dijkstra(tw, 0, 4, max_cost=60, stop_at=ends_journey)
check("the tunnel board does not wrap in dijkstra", (15, 4) not in edge)
check("the surface board still does",
      (config.MAP_COLS - 1, 10) in navigate.dijkstra(WorldMap(), 0, 10,
                                                     max_cost=6)[0])
tw.grid[4][0] = mk(TERR_COLLAPSED)
tw.grid[4][15] = mk(TERR_COLLAPSED)
check("a shaft is reachable", (3, 3) in dist)
tw.grid[4][7] = mk(TERR_VENT)            # a shaft sitting ON the corridor
dist2, _ = navigate.dijkstra(tw, 3, 4, max_cost=60, stop_at=ends_journey)
check("nothing routes through a shaft", (9, 4) not in dist2)
check("but the shaft itself is still reachable", (7, 4) in dist2)
dist3, _ = navigate.dijkstra(tw, 3, 4, max_cost=60)
check("without stop_at it would walk straight through one", (9, 4) in dist3)
tw.grid[4][7] = mk(TERR_TUNNEL)
check("frontier_bonus does not count the rock beyond the edge",
      navigate.frontier_bonus(tw, 15, 9) <= 3)

print("tunnel policies")


def underground_obs(policy_pid=2, food=4, water=5, mp=10, ll=6, at=(7, 4)):
    """A bot standing in the corridor of the little test network above."""
    o = Observation()
    o.apply(sync)
    o.pid = policy_pid
    o.map = WorldMap()
    for rr in range(config.MAP_ROWS):
        for qq in range(config.MAP_COLS):
            o.map.grid[rr][qq] = mk()
    o.map.grid[0][2] = mk(TERR_BUNKER)          # the hatch we came down
    o.tunnel = WorldMap(TUN_ROWS, TUN_COLS, wraps=False)
    for qq in range(2, 13):
        o.tunnel.grid[4][qq] = mk(TERR_TUNNEL)
    o.tunnel.grid[3][3] = mk(TERR_BUNKER)
    o.tunnel.grid[3][11] = mk(TERR_VENT)
    o.tunnel_synced = True
    o.tunnel_pos = at
    me = o.players[policy_pid]
    me.depth, me.tq, me.tr = 1, at[0], at[1]
    me.q, me.r = 2, 0
    me.inv = [water, food, 0, 0, 0]
    me.mp, me.ll, me.inv_slots, me.valid_moves = mp, ll, 8, 0b111111
    return o


# A surface policy that falls down a hatch must climb straight back out
# rather than flailing on a board it does not read.
ou = underground_obs()
sc = pmod.make("scoremax", random.Random(3))
sc.set_pid(2)
a = sc.decide(ou)
check("a surface policy climbs out of the tunnels",
      a.kind == "move" and "fell down a hatch" in a.why)
check("and it aims at a shaft", "shaft" in a.why)
# REST works at depth 1 now, so a survivor out of MP down a hole sleeps
# rather than stalling. That matters well beyond this bot: tickGame()
# collapses the day only when EVERY connected player is resting, so the old
# refusal held the whole fleet's day open whenever one of them was below.
ou.players[2].mp = 0
ou.players[2].valid_moves = 0
a = sc.decide(ou)
check("out of MP underground rests instead of stalling",
      a.kind == "act" and a.to_msg().get("a") == config.ACT_REST)
check("and the reason says where it slept", "underground" in a.why)

# The explorer works the corridor instead.
oe = underground_obs()
sub_pol = pmod.make("subterranean", random.Random(3))
sub_pol.set_pid(2)
a = sub_pol.decide(oe)
check("subterranean explores underground",
      a.kind in ("move", "act") and "fell down a hatch" not in a.why)
moves = set()
for _ in range(60):
    act = sub_pol.decide(oe)
    if act.kind == "move":
        moves.add(act.d)
    json.dumps(act.to_msg() or {})
check("and never steps onto a shaft while exploring",
      all(not config.is_hatch_terrain(
          (oe.tunnel[(7 + config.DQ[d], 4 + config.DR[d])] or mk()).terrain)
          for d in moves))
# Out of food: there is none underground, so the dive has to end.
oe2 = underground_obs(food=0)
sub2 = pmod.make("subterranean", random.Random(3))
sub2.set_pid(2)
a = sub2.decide(oe2)
check("no food underground -> head for a shaft",
      a.kind == "move" and "food" in a.why and "shaft" in a.why)
# MP reserve: keep back enough to walk to a way out -- but only the food
# clock actually ends a dive. Running dry underground means sleeping where
# you stand, and that is cheaper than sleeping in the open (a 30% bad-air
# roll against a guaranteed 2 LL of exposure), so a bot that scrambled for
# the surface every dusk would be paying to make itself worse off.
oe3 = underground_obs(mp=3, food=3)
sub3 = pmod.make("subterranean", random.Random(3))
sub3.set_pid(2)
check("the reserve is measured, not guessed", sub3.mp_reserve(oe3) > 2)
check("a thin MP budget alone does not end the dive",
      "leaving while we still can" not in sub3.decide(oe3).why)
oe3b = underground_obs(mp=3, food=1)
sub3b = pmod.make("subterranean", random.Random(3))
sub3b.set_pid(2)
check("thin MP with the food nearly gone does",
      "leaving while we still can" in sub3b.decide(oe3b).why)

# enc_start must carry the TUNNEL coordinates -- handleMsg_enc_start
# cross-checks them against tq/tr while we are below, and the surface q/r are
# pinned to the hatch, which is a different hex entirely.
oe4 = underground_obs()
oe4.tunnel.grid[4][7] = mk(TERR_TUNNEL, poi=True)
sub4 = pmod.make("subterranean", random.Random(3))
sub4.set_pid(2)
a = sub4.decide(oe4)
check("a tunnel POI is opened at its tunnel coords",
      a.kind == "enc_start" and (a.q, a.r) == (7, 4))

# The runner will not climb out of the shaft it came down: that is a round
# trip to nowhere, and the naive nearest-exit rule does it every time.
orun = underground_obs()
runner = pmod.make("tunnelrunner", random.Random(3))
runner.set_pid(2)
runner.set_pid(2)
orun.hatches[0] = __import__("state").Hatch(idx=0, sq=2, sr=0, tq=3, tr=3)
orun.hatches[1] = __import__("state").Hatch(idx=1, sq=60, sr=40, tq=11, tr=3)
runner._dive = {"hatch": 0, "q": 2, "r": 0, "steps": 3}
a = runner.decide(orun)
check("the runner crosses instead of bouncing",
      a.kind == "move" and "(11,3)" in a.why)
check("it reports the crossing as the reason", "crossing" in a.why)
# Starving overrides it -- any way out will do.
orun2 = underground_obs(food=0, at=(4, 4))
orun2.hatches[0] = __import__("state").Hatch(idx=0, sq=2, sr=0, tq=3, tr=3)
runner2 = pmod.make("tunnelrunner", random.Random(3))
runner2.set_pid(2)
runner2._dive = {"hatch": 0, "q": 2, "r": 0, "steps": 3}
a = runner2.decide(orun2)
check("but hunger takes the nearest exit", "food" in a.why and a.kind == "move")


def surface_obs(food=4, water=5, mp=10, ll=6):
    """On the surface at (10,10), with a Bunker Entrance and a Vent Shaft
    the same distance away in opposite directions."""
    o = Observation()
    o.apply(sync)
    o.pid = 2
    w = WorldMap()
    for rr in range(config.MAP_ROWS):
        for qq in range(config.MAP_COLS):
            w.grid[rr][qq] = mk()
    w.grid[10][13] = mk(TERR_BUNKER)
    w.grid[10][7] = mk(TERR_VENT)
    o.map = w
    me = o.players[2]
    me.q, me.r, me.depth = 10, 10, 0
    me.inv = [water, food, 0, 0, 0]
    me.mp, me.ll, me.inv_slots, me.valid_moves = mp, ll, 8, 0b111111
    return o


os_ = surface_obs()
sub5 = pmod.make("subterranean", random.Random(3))
sub5.set_pid(2)
a = sub5.decide(os_)
check("stocked, the explorer heads for a hatch",
      a.kind == "move" and "diving" in a.why)
check("and prefers the Bunker Entrance over the Vent Shaft",
      "(13,10)" in a.why)
# Underfed, it must NOT wander onto a hatch -- that would be an accidental
# descent with nothing to eat down there.
os2 = surface_obs(food=0, water=1)
os2.map.grid[10][12] = mk(resource=config.RES_FOOD + 1)
sub6 = pmod.make("subterranean", random.Random(3))
sub6.set_pid(2)
a = sub6.decide(os2)
check("underfed, it resupplies instead of diving",
      "diving" not in a.why and a.kind in ("move", "act"))
os3 = surface_obs(mp=1)
sub7 = pmod.make("subterranean", random.Random(3))
sub7.set_pid(2)
check("and it will not set off with no MP to walk on",
      "diving" not in sub7.decide(os3).why)

# Dry with a pond 7 hexes off and a hatch 3: the survival floor runs before
# pursue(), so if the emergency hunt does not count the hatch as water the
# dive gate is never reached. On hardware that was 531 of 933 decisions.
os4 = surface_obs(food=3, water=0)
os4.map.grid[10][17] = mk(3)                    # Marsh: has_water
sub8 = pmod.make("subterranean", random.Random(3))
sub8.set_pid(2)
a = sub8.decide(os4)
check("dry, a tunnel bot takes the hatch as its water",
      a.kind == "move" and "EMERGENCY water" in a.why
      and ("(13,10)" in a.why or "(7,10)" in a.why))
sc8 = pmod.make("scoremax", random.Random(3))
sc8.set_pid(2)
a = sc8.decide(os4)
check("a surface bot still walks to the pond", "(17,10)" in a.why)
os5 = surface_obs(food=0, water=0)
os5.map.grid[10][17] = mk(3)                    # Marsh: has_water
sub9 = pmod.make("subterranean", random.Random(3))
sub9.set_pid(2)
check("but not while starving -- there is no food below",
      "(13,10)" not in sub9.decide(os5).why)

# Stocked but no hatch known: explore for one rather than resupply forever.
os6 = surface_obs()
os6.map.grid[10][13] = mk()
os6.map.grid[10][7] = mk()
for qq in range(20, 40):
    for rr in range(5, 15):
        os6.map.grid[rr][qq] = None
sub10 = pmod.make("subterranean", random.Random(3))
sub10.set_pid(2)
check("no hatch in the map -> go looking for one",
      "seeking a hatch" in sub10.decide(os6).why)
# Over the token cap, a pile next door cannot be picked up -- it must not
# out-bid the search (the 3-hex livelock from the 2026-09-23 realtime run).
os7 = surface_obs(food=5, water=4)
os7.map.grid[10][13] = mk()
os7.map.grid[10][7] = mk()
os7.map.grid[10][11] = mk(resource=config.RES_FOOD + 1)
for qq in range(20, 40):
    for rr in range(5, 15):
        os7.map.grid[rr][qq] = None
sub11 = pmod.make("subterranean", random.Random(3))
sub11.set_pid(2)
a = sub11.decide(os7)
check("a full pack ignores the pile it cannot lift",
      "seeking a hatch" in a.why and "(11,10)" not in a.why)

# A reconnect's sync carries only the vision disk; it must not erase the map.
o6 = Observation()
o6.apply(sync)
o6.apply({"t": "vis", "vr": 2, "q": 10, "r": 5, "cells": "0A05020009"})
o6.apply(sync)
check("a reconnect sync keeps what we had already seen",
      o6.map[(10, 5)] is not None and o6.map[(10, 5)].terrain == 2)

# Both tunnel policies must survive a long run of decisions on either board
# without raising or emitting something the wire cannot carry.
for pname in ("subterranean", "tunnelrunner"):
    for obs_maker in (underground_obs, surface_obs):
        pol = pmod.make(pname, random.Random(5))
        pol.set_pid(2)
        o = obs_maker()
        kinds = set()
        for _ in range(80):
            act = pol.decide(o)
            kinds.add(act.kind)
            m = act.to_msg()
            if m is not None:
                json.dumps(m)
        check(f"{pname} on {obs_maker.__name__} yields valid actions",
              kinds and kinds <= {"move", "act", "noop", "enc_start",
                                  "enc_choice", "enc_bank", "enc_abort",
                                  "use_item", "trade_offer"})

# The tunnel metrics are what the runs are read off, so they have to add up.
mp_pol = pmod.make("tunnelrunner", random.Random(5))
mp_pol.set_pid(2)
for e in ({"k": "tun_in", "pid": 2, "q": 10, "r": 10, "hatch": 0},
          {"k": "mv", "pid": 2, "q": 5, "r": 4, "dp": 1},
          {"k": "mv", "pid": 2, "q": 6, "r": 4, "dp": 1},
          {"k": "mv", "pid": 2, "q": 7, "r": 4, "dp": 1},
          {"k": "tun_out", "pid": 2, "q": 40, "r": 10, "hatch": 1}):
    mp_pol.on_event(dict(e, t="ev"))
cs = mp_pol.content_score()
check("descents and ascents are counted",
      cs["descents"] == 1 and cs["ascents"] == 1)
check("underground steps are counted", cs["tunnel_steps"] == 3)
check("the crossing is measured in surface hexes", cs["transit_hexes"] == 30)
check("MP per surface hex is the saving, measured",
      cs["mp_per_surface_hex"] == 0.2)
check("shafts used are distinct", cs["shafts_used"] == 1)
# Other players' crossings must not land in our own totals.
mp_pol.on_event({"t": "ev", "k": "tun_in", "pid": 4, "q": 1, "r": 1, "hatch": 2})
check("another survivor's dive is not ours",
      mp_pol.content_score()["descents"] == 1)
# A dawn spent below costs no exposure under either hatch now -- dawnUpkeep
# treats depth as cover. The split is still counted because the two hatch
# types differ on the climb out, 1 MP against 2.
mp_pol._depth = 1
mp_pol._dive_terrain = TERR_BUNKER
mp_pol.on_event({"t": "ev", "k": "dawn", "pid": 2, "day": 4})
mp_pol._dive_terrain = TERR_VENT
mp_pol.on_event({"t": "ev", "k": "dawn", "pid": 2, "day": 5})
mp_pol.on_event({"t": "ev", "k": "act", "pid": 2, "a": config.ACT_REST})
cs = mp_pol.content_score()
check("dawns below are split by hatch type",
      cs["dawns_below"] == 2 and cs["bunker_dawns"] == 1 and cs["vent_dawns"] == 1)
check("nights actually slept below are counted", cs["rests_below"] == 1)
check("what those dawns cost is recorded", cs["below_dawn_ll"] >= 0)

print("carry cap (the livelock that killed the first hardware run)")
# Four of five bots sat at room 0 asking for water they could not hold: 376
# refused actions a minute, no MP spent, nothing in any log to say why.
# FORAGE and WATER are uncapped now (they overfill and pay encumbrance);
# only SCAVENGE is still bound, and a policy must check before asking.
from policy.survivor import token_room as _room

_full = surface_obs(food=0, water=0)
_full.players[2].inv = [2, 0, 2, 2, 2]      # 8 tokens in an 8-slot pack
_full.players[2].inv_slots = 8
check("a full pack reports no room", _room(_full.players[2]) == 0)

_fp = pmod.make("scoremax", random.Random(5)); _fp.set_pid(2)
_a = _fp.decide(_full)
check("a thirsty bot with a full pack still acts", _a.kind != "noop")
# The point is that it does not sit there repeating a refused action: WATER is
# legal again, so either it drinks (on water terrain) or it goes looking.
check("and it is not stuck on a refused action",
      _a.kind in ("act", "move", "equip_item", "pickup_item"))

_room_left = surface_obs()
_room_left.players[2].inv = [1, 1, 0, 0, 0]
_room_left.players[2].inv_slots = 8
check("a pack with space reports room", _room(_room_left.players[2]) == 6)

# Underground salvage is the one yield action still gated.
_sub = pmod.make("subterranean", random.Random(5)); _sub.set_pid(2)
_su = underground_obs()
_su.players[2].inv = [2, 2, 2, 1, 1]        # 8 of 8 — no room for scrap
_su.players[2].inv_slots = 8
_su.players[2].mp = 10
check("no SCAVENGE request when the pack cannot hold the scrap",
      not (_sub.pursue_below(_su) or Action("noop")).kind == "act"
      or (_sub.pursue_below(_su).a != config.ACT_SCAV))

print("equipment")
# Until gear_action() existed no bot ever sent equip_item, so every balance
# number in docs/bot-testing.md was measured on a survivor wearing nothing.
check("the equipment registry parsed out of items.cfg",
      len(config.EQUIPMENT) >= 20)
check("slots are mapped by name, not guessed",
      config.EQUIPMENT.get(64) == 1 and config.EQUIPMENT.get(26) == 4)

oe = surface_obs()
oe.players[2].inv_type = [64] + [0] * (config.INV_SLOTS_MAX - 1)   # Backpack in slot 0
eq_pol = pmod.make("scoremax", random.Random(4))
eq_pol.set_pid(2)
a = eq_pol.decide(oe)
check("a carried Backpack gets put on", a.kind == "equip_item" and a.slot == 0)
check("and it addresses the body slot in the reason", "-> slot 1" in a.why)
check("the wire message is well formed",
      a.to_msg() == {"t": "equip_item", "slot": 0})

# Already wearing one: the slot is full, so nothing more to do there.
oe.players[2].equip[1] = 64
a = eq_pol.decide(oe)
check("a full slot is not re-equipped", a.kind != "equip_item")

# A consumable is not equipment and must never be sent to equip_item.
oe.players[2].equip[1] = 0
oe.players[2].inv_type = [1] + [0] * (config.INV_SLOTS_MAX - 1)    # Trauma Patch
a = eq_pol.decide(oe)
check("a consumable is never equipped", a.kind != "equip_item")

# Dropped gear is not lost: grantItemOrDrop() spills an overflowing loot item
# onto the hex, and the bot has to be willing to go back for it.
oe.players[2].inv_type = [0] * config.INV_SLOTS_MAX
oe.ground_items = [{"g": 2, "q": 10, "r": 10, "id": 64, "n": 1}]
a = eq_pol.decide(oe)
check("equipment underfoot is picked up", a.kind == "pickup_item" and a.gslot == 2)
check("the pickup wire message is well formed",
      a.to_msg() == {"t": "pickup_item", "gslot": 2})

oe.ground_items = [{"g": 2, "q": 11, "r": 10, "id": 64, "n": 1}]
a = eq_pol.decide(oe)
check("gear on a different hex is left alone", a.kind != "pickup_item")

oe.ground_items = [{"g": 2, "q": 10, "r": 10, "id": 21, "n": 3}]   # Useful Garbage
a = eq_pol.decide(oe)
check("non-equipment on the ground is not chased", a.kind != "pickup_item")

# A full pack cannot take it, and pickupGroundItem() would refuse anyway.
oe.ground_items = [{"g": 2, "q": 10, "r": 10, "id": 64, "n": 1}]
oe.players[2].inv_type = [21] * config.INV_SLOTS_MAX
oe.players[2].inv_slots = config.INV_SLOTS_MAX
a = eq_pol.decide(oe)
check("a full pack does not send a doomed pickup", a.kind != "pickup_item")
oe.players[2].inv_type = [0] * config.INV_SLOTS_MAX
oe.players[2].inv_slots = 8
oe.ground_items = []

# Swapping: the weights have to actually differentiate, or a run measures
# acquisition order rather than which gear suits which playstyle.
_slot_pick = {}
for _name in ("coward", "scoremax", "contentmax", "rival", "subterranean"):
    _w = pmod.make(_name, random.Random(1)).gear_weights
    _slot_pick[_name] = tuple(
        max((config.gear_score(i, _w), i) for i in config.EQUIPMENT
            if config.EQUIPMENT[i] == es)[1]
        for es in range(config.EQUIP_SLOTS))
check("every policy wants a different loadout",
      len(set(_slot_pick.values())) == len(_slot_pick))
check("the coward values exposure immunity above raw stats",
      _slot_pick["coward"][1] == 34)          # Bear Skin Cape, body
check("scoremax takes carry space over armour",
      _slot_pick["scoremax"][1] == 64)        # Backpack, body
check("the digger values sight most",
      _slot_pick["subterranean"][0] == 45)    # Glow Dentures, head


# Swapping returns the displaced item to the pack, so a policy that ever
# prefers what it just took off loops forever at one message per cycle and
# looks like nothing at all from either end. Drive each policy to a fixed
# point against a mirror of equipItem() and assert it never revisits a state.
def _apply_equip(me, slot):
    item = me.inv_type[slot]
    es   = config.EQUIPMENT[item]
    prev = me.equip[es]
    me.equip[es]      = item
    me.inv_type[slot] = 0
    if prev:
        free = next((i for i, t in enumerate(me.inv_type) if not t), None)
        assert free is not None, "nowhere to return the displaced item"
        me.inv_type[free] = prev
    me.inv_slots = max(1, min(config.INV_SLOTS_MAX, 8 + sum(
        config.EQUIP_STATS[e]["slots"] for e in me.equip if e)))


_converged = True
for _name in ("coward", "scoremax", "contentmax", "rival", "subterranean"):
    _p = pmod.make(_name, random.Random(7)); _p.set_pid(2)
    _o = surface_obs()
    _me = _o.players[2]
    # Several competing items per slot, deliberately in a poor order.
    _me.inv_type = [13, 45, 33, 12, 64, 11, 42, 20, 40, 15, 41, 18, 26, 63,
                    0, 0, 0, 0]
    _me.equip = [0] * config.EQUIP_SLOTS
    _seen, _steps = set(), 0
    while _steps < 100:
        _a = _p.gear_action(_o)
        if _a is None:
            break
        _st = (tuple(_me.equip), tuple(_me.inv_type))
        if _st in _seen:
            _converged = False
            break
        _seen.add(_st)
        _apply_equip(_me, _a.slot)
        _steps += 1
    else:
        _converged = False
    if _steps > config.EQUIP_SLOTS:
        _converged = False       # more equips than slots means it churned
check("gear swapping reaches a fixed point and never oscillates", _converged)

# Mid-encounter the server refuses both act and equip, so the policy must not
# try -- decide() answers encounter actions before gear_action is reached.
oe.players[2].inv_type = [64] + [0] * (config.INV_SLOTS_MAX - 1)
oe.players[2].in_encounter = True
check("no equip attempt while an encounter is open",
      eq_pol.gear_action(oe) is None)


print("protocol 2: replies, wire causes, event sequence")
import asyncio
import re

_FW = Path(__file__).resolve().parent.parent

# -- drift: the firmware source is the authority for these, so read it ----
_ino = (_FW / "Esp32HexMapCrawl.ino").read_text(encoding="utf-8")
_m = re.search(r"DC_NAME\[DC_COUNT\]\s*=\s*\{(.*?)\};", _ino, re.S)
_fw_causes = re.findall(r'"([^"]+)"', _m.group(1)) if _m else []
from causes import CAUSE_ORDER
check("firmware DC_NAME parsed", len(_fw_causes) >= 10)
check("every firmware cause name is one metrics.py reports",
      set(_fw_causes) <= set(CAUSE_ORDER))

_disp = (_FW / "network-handlers.hpp").read_text(encoding="utf-8")
_fw_cmds = set(re.findall(r'CMD_IS\("([a-z_]+)"\)', _disp))
check("firmware dispatch table parsed", {"pick", "m", "act"} <= _fw_cmds)
_samples = [Action("move"), Action("act"), Action("enc_start", q=1, r=1),
            Action("enc_choice"), Action("enc_bank"), Action("enc_abort"),
            Action("use_item"), Action("equip_item"), Action("unequip_item"),
            Action("pickup_item"), Action("trade_offer")]
_bot_cmds = {a.to_msg()["t"] for a in _samples} | {"pick", "regen", "eraseslot"}
check("every command a bot sends is one the firmware dispatches "
      f"(missing: {sorted(_bot_cmds - _fw_cmds)})", _bot_cmds <= _fw_cmds)

# -- wire cause and dmg ----------------------------------------------------
rows = [
    _ev(1.0, {"k": "dmg", "pid": 1, "amt": 1, "cause": "chem storm", "ll": 1,
              "sq": 10}),
    {"ts": 1.2, "ch": "rx", "arch": 1,
     "d": {"t": "ev", "k": "downed", "pid": 1, "cause": "chem storm", "sq": 11}},
]
led = DamageLedger(rows)
d1 = led.deaths()
check("a wire cause is taken at its word",
      bool(d1) and d1[0]["cause"] == "chem storm" and d1[0]["via"] == "wire")
check("dmg events reach the loss ledger",
      led.loss_ledger().get("chem storm") == 1)

# -- sq dedupe: exact across bots, and never merges two real events -------
from causes import _dedupe
_same = {"k": "act", "pid": 0, "a": 7, "out": 1, "sq": 5}
rows = [_ev(1.0, _same, arch=0), _ev(1.002, _same, arch=1),  # one event, two sockets
        _ev(1.1, dict(_same, sq=6), arch=0)]                  # a second, identical-looking one
check("sq dedupes one event seen by two bots, keeps a distinct one",
      len(_dedupe(rows)) == 2)
rows = [_ev(1.0, {"k": "downed", "pid": 0, "sq": 7}),
        _ev(1.0, {"k": "left", "pid": 0, "sq": 7})]
check("one sq may carry two message kinds (downed + left)",
      len(_dedupe(rows)) == 2)

# -- state: rt per tick, pv from sync -------------------------------------
o = Observation()
o.apply({"t": "sync", "id": 0, "pv": 2, "tk": 1, "p": [{"on": 1}]})
check("sync pv is recorded", o.proto == 2)
o.apply({"t": "s", "tk": 2, "p": [{"on": 1, "rt": 1}]})
check("tick rt sets resting", o.players[0].resting is True)
o.apply({"t": "s", "tk": 3, "p": [{"on": 1, "rt": 0}]})
check("tick rt clears resting", o.players[0].resting is False)
o2 = Observation()
o2.apply({"t": "sync", "id": 0, "tk": 1, "p": []})
check("sync without pv reads as protocol 1", o2.proto == 1)

# -- client: ack / nack bookkeeping ----------------------------------------
# None of this opens a socket, so smoke.py keeps running without the one
# third-party dependency installed.
try:
    import websockets  # noqa: F401
except ImportError:
    import sys, types
    sys.modules["websockets"] = types.ModuleType("websockets")
from client import BotClient, RateLimiter, SlotBroker
from policy.base import Policy
from findings import Finding, FindingLog, ReproBuffer
from wire import ReplyTracker
from oracles import WireOracle, StateOracle


class _Rec:
    def __init__(self):
        self.rows = []

    def write(self, ch, arch, d):
        self.rows.append((ch, arch, d))


class _Pol(Policy):
    name = "probe"

    def __init__(self):
        super().__init__(random.Random(1))
        self.replies = []

    def on_reply(self, cmd, ok, why):
        self.replies.append((cmd, ok, why))


def _bot():
    rng = random.Random(3)
    rec, pol, broker = _Rec(), _Pol(), SlotBroker()
    bc = BotClient("x", 0, pol, rec, rng, RateLimiter(0, rng),
                   RateLimiter(0, rng), broker)
    return bc, rec, pol, broker


async def _reply_checks():
    bc, rec, pol, broker = _bot()
    rids = [bc.replies.stamp({"t": c})[0]["rid"] for c in ("m", "m", "act", "pick")]
    await bc._on_message({"t": "ack", "rid": rids[0], "cmd": "m"})
    await bc._on_message({"t": "nack", "rid": rids[1], "cmd": "m", "why": "no_mp"})
    await bc._on_message({"t": "nack", "rid": rids[2], "cmd": "act", "why": "resting"})
    ok1 = (bc.acks == 1
           and bc.nacks == {"m": {"no_mp": 1}, "act": {"resting": 1}}
           and pol.replies[1] == ("m", False, "no_mp")
           and any(ch == "nack" for ch, _a, _d in rec.rows))
    # A refused pick gives the slot back at once instead of waiting out the
    # sync watchdog.
    await broker.claim([2], 2)
    bc._pending_slot = 2
    await bc._on_message({"t": "nack", "rid": rids[3], "cmd": "pick", "why": "slot_taken"})
    ok2 = (bc._pending_slot is None and bc.pick_failures == 1
           and await broker.claim([2], 2) == 2)
    # A reply nobody asked for -- answered twice, or never sent -- is a finding.
    await bc._on_message({"t": "ack", "rid": rids[0], "cmd": "m"})
    ok5 = any(s[0] == "reply_unmatched" for s in bc.findings.counts)
    return ok1, ok2, ok5

_r = asyncio.run(_reply_checks())
check("acks and nacks are counted per command and reason", _r[0])
check("a nacked pick releases its slot immediately", _r[1])
check("a reply to a rid already answered is a finding", _r[2])


async def _expiry_checks():
    fl = FindingLog()
    clock = [0.0]
    tr = ReplyTracker(WireOracle(fl, "t"), clock=lambda: clock[0])
    tr.stamp({"t": "m"})
    clock[0] = 100.0
    quiet = tr.expire(proto=1)
    tr.stamp({"t": "m"})
    clock[0] = 200.0
    loud = tr.expire(proto=2)
    return (not quiet and tr.unanswered == 1 and len(loud) == 1
            and ("request_unanswered", "m") in fl.counts)

check("no reply from a protocol-1 board is not a fault, from protocol 2 it is",
      asyncio.run(_expiry_checks()))

# -- telemetry: a reboot mid-run is recorded, not inferred -----------------
import telemetry as _tele

_states = iter([
    {"mem": {"uptimeMs": 600000}, "evtDrops": 0, "boot": {"reset": "POWER_ON"}},
    {"mem": {"uptimeMs": 4000}, "evtDrops": 0,
     "boot": {"reset": "PANIC", "crash": {"task": "async_tcp", "pc": "0x42001234"}}},
])


async def _poll_twice():
    rec = _Rec()
    tp = _tele.TelemetryPoller("x", rec, interval=0.0)
    real = _tele.fetch_state
    _tele.fetch_state = lambda host, timeout=5.0: next(_states)
    try:
        task = asyncio.create_task(tp.run())
        while len([r for r in rec.rows if r[0] == "telemetry"]) < 2:
            await asyncio.sleep(0)
        tp.stop.set()
        await task
    finally:
        _tele.fetch_state = real
    return tp, rec

_tp, _trec = asyncio.run(_poll_twice())
check("uptime going backwards is recorded as a board_reboot",
      any(r[0] == "board_reboot" and r[2]["reset"] == "PANIC" for r in _trec.rows))
check("the reboot carries the crash summary",
      _tp.reboots and _tp.reboots[0]["crash"]["task"] == "async_tcp")


print("findings")
import copy
import findings as fmod

_frec = _Rec()
_fl = FindingLog(_frec)
for _i in range(5):
    _fl.add(Finding(check="ll_over_cap", severity="major", summary="x", key=(1,)))
_fl.add(Finding(check="board_rebooted", severity="critical", summary="y"))
_fl.add(Finding(check="ll_over_cap", severity="major", summary="x", key=(2,)))
check("one signature counts every occurrence", _fl.counts[("ll_over_cap", 1)] == 5)
check("only the first few occurrences are written in full",
      sum(1 for ch, _a, d in _frec.rows if ch == "finding"
          and d["check"] == "ll_over_cap" and d["key"] == [1]) == fmod.WRITE_FIRST)
check("summary puts the worst first", _fl.summary()[0]["check"] == "board_rebooted"
      and _fl.worst() == "critical")
try:
    _fl.add(Finding(check="x", severity="bad", summary=""))
    _bad_sev = False
except ValueError:
    _bad_sev = True
check("an unknown severity is refused", _bad_sev)

with tempfile.TemporaryDirectory() as _td:
    _p = Path(_td) / "f.jsonl"
    with Recorder(_p, {"kind": "t"}) as _r2:
        _fl2 = FindingLog(_r2)
        for _i in range(7):
            _fl2.add(Finding(check="seat_lost", severity="major", summary="s"))
        _fl2.flush()
    _rows = fmod.load([_p])
    import io
    _buf = io.StringIO()
    _n = fmod.report(_rows, out=_buf)
check("findings.py reports the run-end total, not just the rows written",
      _n == 1 and "seen 7x" in _buf.getvalue())

print("oracles")


def _tick(p=None, tk=10, **kw):
    base = {"on": 1, "ll": 5, "llCap": 7, "food": 4, "water": 4, "rad": 0,
            "mp": 3, "inv": [1, 1, 0, 0, 0], "vm": 7, "rt": 0, "sp": 10}
    base.update(kw)
    return {"t": "s", "tk": tk, "p": [base] + [{"on": 0}] * 5}


def _wo(proto=2):
    fl = FindingLog()
    clock = [0.0]
    o = WireOracle(fl, "t", clock=lambda: clock[0])
    o.proto = proto
    ob = Observation()
    ob.pid = 0
    return o, fl, ob, clock


def _checks(fl):
    return {s[0] for s in fl.counts}


_o, _f, _ob, _c = _wo()
_o.on_rx(_tick(), _ob)
check("a healthy tick raises nothing", not _f.counts)
for label, kw, want in [
        ("LL over its cap", {"ll": 8}, "ll_over_cap"),
        ("food outside [1, 6]", {"food": 0}, "food_out_of_range"),
        ("water outside [1, 6]", {"water": 7}, "water_out_of_range"),
        ("radiation outside [0, 10]", {"rad": 11}, "rad_out_of_range"),
        ("a downed survivor with MP", {"ll": 0, "mp": 2}, "downed_with_mp"),
        ("a move mask while out of MP", {"mp": 0}, "vm_when_immobile"),
        ("a move mask while resting", {"rt": 1}, "vm_when_immobile"),
        ("a resource count over 99", {"inv": [100, 0, 0, 0, 0]}, "inv_out_of_range")]:
    _o, _f, _ob, _c = _wo()
    _o.on_rx(_tick(**kw), _ob)
    check(f"oracle catches {label}", want in _checks(_f))

_o, _f, _ob, _c = _wo()
_o.on_rx(_tick(sp=10, tk=10), _ob)
_o.on_rx(_tick(sp=9, tk=9), _ob)
check("oracle catches steps and tick going backwards",
      {"steps_went_backwards", "tick_went_backwards"} <= _checks(_f))
_o, _f, _ob, _c = _wo()
_o.on_rx(_tick(sp=10), _ob)
_o.on_rx({"t": "ev", "k": "join", "pid": 0}, _ob)
_o.on_rx(_tick(sp=0, tk=11), _ob)
check("a join starts a new life: steps may restart", not _f.counts)

_o, _f, _ob, _c = _wo()
for _ev_ in ({"k": "downed", "pid": 0, "sq": 5}, {"k": "left", "pid": 0, "sq": 5},
             {"k": "mv", "pid": 1, "sq": 7}):
    _o.on_rx(dict(_ev_, t="ev"), _ob)
check("one seq may carry downed and left; seq may skip", not _f.counts)
_o.on_rx({"t": "ev", "k": "mv", "pid": 1, "sq": 7}, _ob)
_o.on_rx({"t": "ev", "k": "mv", "pid": 1, "sq": 6}, _ob)
check("oracle catches a repeated and a backwards seq",
      {"ev_duplicate", "ev_sq_backwards"} <= _checks(_f))

_o, _f, _ob, _c = _wo()
_o.on_rx({"t": "ev", "k": "left", "pid": 3, "cause": "thirst"}, _ob)
_o.on_rx({"t": "ev", "k": "join", "pid": 3}, _ob)
_o.on_rx({"t": "ev", "k": "left", "pid": 3, "cause": "exposure"}, _ob)
check("a death, a rejoin and a death is two lives", not _f.counts)
_o.on_rx({"t": "ev", "k": "left", "pid": 3, "cause": "fire"}, _ob)
check("two deaths in one life is the seat-count bug", "double_downed" in _checks(_f))
_o, _f, _ob, _c = _wo()
_o.on_rx({"t": "ev", "k": "left", "pid": 3, "cause": "thirst"}, _ob)
_o.new_socket()   # away: the rejoin happened while we were not looking
_o.on_rx({"t": "ev", "k": "left", "pid": 3, "cause": "fire"}, _ob)
check("a death seen across a reconnect is not called a double death", not _f.counts)
_o.on_rx({"t": "ev", "k": "left", "pid": 4}, _ob)
_o.on_rx({"t": "ev", "k": "left", "pid": 4}, _ob)
check("a plain disconnect is not a death", "double_downed" not in
      {s[0] for s in _f.counts if s[1:] == (4,)})

_o, _f, _ob, _c = _wo()
_o.on_reply(True, "m", None, seated=True)
_o.on_rx({"t": "ev", "k": "mv", "pid": 0, "sq": 1}, _ob)
_c[0] = 10.0
_o.poll()
check("an acked move with its mv raises nothing", not _f.counts)
_o.on_reply(True, "m", None, seated=True)
_c[0] = 20.0
_o.poll()
check("an acked move with no mv is ack_without_effect",
      "ack_without_effect" in _checks(_f))

_o, _f, _ob, _c = _wo()
_o.on_reply(False, "act", "no_mp", seated=True)
_o.on_rx({"t": "ev", "k": "act", "pid": 0, "out": 0, "bw": 3, "sq": 1}, _ob)
_o.on_reply(False, "act", "pack_full", seated=True)
_o.on_rx({"t": "ev", "k": "act", "pid": 0, "out": 0, "bw": 3, "sq": 2}, _ob)
_o.on_reply(False, "act", "terrain", seated=True)
_c[0] = 10.0
_o.poll()
_cs = _checks(_f)
check("a nack and its blocked event must agree on the reason",
      "nack_event_mismatch" in _cs and ("nack_event_mismatch", "no_mp") not in _f.counts)
check("a nack whose blocked event never comes is caught", "nack_without_event" in _cs)
_o, _f, _ob, _c = _wo()
_o.on_reply(False, "act", "in_enc", seated=True)
_c[0] = 10.0
_o.poll()
check("refusals made before any event exist expect none", not _f.counts)
_o.on_reply(False, "m", "not_seated", seated=True)
check("not_seated while holding a seat is a desync", "seat_desync" in _checks(_f))
_o, _f, _ob, _c = _wo(proto=1)
_o.on_reply(True, "m", None, seated=True)
_c[0] = 10.0
_o.poll()
check("a protocol 1 board makes no promises to check", not _f.counts)

_sf = FindingLog()
_so = StateOracle(_sf)
_st = {"connected": 2, "players": [{"pid": 0, "conn": True, "ll": 5, "food": 3, "water": 3}]
       + [{"pid": i, "conn": False} for i in range(1, 6)],
       "mem": {"uptimeMs": 50000, "minHeap": 90000}, "evtDrops": 0}
_so.check(dict(_st, players=_st["players"][:1]))
check("a /state read without the full player list is not compared",
      not _sf.counts)
_so.prev = None
_so.check(_st)
_st2 = copy.deepcopy(_st)
_st2["mem"] = {"uptimeMs": 1000, "minHeap": 20000}
_st2["evtDrops"] = 3
_st2["boot"] = {"reset": "PANIC"}
_so.check(_st2)
check("state oracle: seat count, reboot, dropped events, heap floor",
      {"connected_count_mismatch", "board_rebooted", "events_dropped", "heap_low"}
      <= {s[0] for s in _sf.counts})

print("fuzz corpus")
import fuzz_cases as fz

_names = [c.name for c in fz.CASES]
check("fuzz case names are unique", len(_names) == len(set(_names)))
check("fuzz COMMANDS is the firmware's dispatch table", set(fz.COMMANDS) == _fw_cmds)
check("every command prefix is a case, and 'e'/'r' say what they used to run",
      {"prefix:e", "prefix:r", "prefix:p"} <= set(_names)
      and "eraseslot" in next(c.note for c in fz.CASES if c.name == "prefix:e")
      and "regen" in next(c.note for c in fz.CASES if c.name == "prefix:r"))


def _texts(case):
    """Every concrete payload a case can produce, as text."""
    p = case.payload
    if callable(p):
        ob = copy.deepcopy(o3)
        ob.players[ob.pid].inv_type = [0] * config.INV_SLOTS_MAX
        p = p(ob)
    if p is None:
        return []
    if isinstance(p, dict):
        return [json.dumps(p)]
    return [p if isinstance(p, str) else p.decode("latin-1")]


_unsafe = []
for _c_ in fz.CASES + [fz.BURST]:
    for _t in _texts(_c_):
        _j = None
        try:
            _j = json.loads(_t.replace("@RID@", "1"))
        except (json.JSONDecodeError, ValueError):
            pass
        _type = _j.get("t") if isinstance(_j, dict) else None
        if _type == "regen":
            _unsafe.append((_c_.name, "well-formed regen"))
        if _type == "eraseslot" and isinstance(_j.get("arch"), int) \
                and 0 <= _j["arch"] < config.MAX_PLAYERS:
            _unsafe.append((_c_.name, "eraseslot of a real slot"))
        if '"ssid"' in _t and '"wifi"' in _t and '"wifi_forget"' not in _t:
            _unsafe.append((_c_.name, "wifi with an ssid"))
        if _type == "drop_res" and isinstance(_j.get("res"), int) and 1 <= _j["res"] <= 5 \
                and (_j.get("qty", 1) > 0 if isinstance(_j.get("qty", 1), int) else True):
            _unsafe.append((_c_.name, "a drop_res that drops"))
check(f"the corpus holds nothing destructive {_unsafe}", not _unsafe)

check("judge: silence", fz.judge("none", None) is None
      and fz.judge("none", {"t": "ack"}) is not None)
check("judge: nack reasons", fz.judge("nack:parse", {"t": "nack", "why": "parse"}) is None
      and fz.judge("nack:parse", {"t": "nack", "why": "bad_arg"}) is not None
      and fz.judge("nack:parse", {"t": "ack"}) is not None
      and fz.judge("reply", None) is not None)
check("judge: cmd", fz.judge("cmd:check", {"t": "nack", "cmd": "check"}) is None
      and fz.judge("cmd:check", {"t": "nack", "cmd": "zz"}) is not None)
def _judge_ok(e):
    try:
        fz.judge(e, {"t": "nack", "why": "x", "cmd": "x"})
        fz.judge(e, None)
        return True
    except ValueError:
        return False


check("every expectation in the corpus is one judge understands",
      all(_judge_ok(c.expect) for c in fz.CASES) and not _judge_ok("nack-typo"))


print("probe client (offline, against a fake board)")
import fuzz as fuzzmod
from wire import ProbeClient


class _FakeTransport:
    def __init__(self, ws):
        self.ws = ws

    def abort(self):
        self.ws.aborted = True
        self.ws.inbox.put_nowait(None)


class _FakeWS:
    """A few handlers' worth of protocol-2 board: enough to drive the probe
    and the fuzz runner through every reply path without a K10."""

    def __init__(self):
        self.inbox = asyncio.Queue()
        self.sent = []
        self.aborted = False
        self.close_code = None
        self.transport = _FakeTransport(self)
        self.seated = False
        self.inbox.put_nowait(json.dumps({"t": "lobby", "avail": [3, 5]}))

    def _reply(self, rid, cmd, why=None):
        if rid is None:
            return
        m = {"t": "nack" if why else "ack", "rid": rid, "cmd": cmd}
        if why:
            m["why"] = why
        self.inbox.put_nowait(json.dumps(m))

    async def send(self, data):
        if not isinstance(data, str):
            if not isinstance(data, (bytes, bytearray)):
                data = "".join(data)     # fragmented: the board drops it
                self.sent.append(("frag", data))
                return
            self.sent.append(("bin", data))
            return
        self.sent.append(("text", data))
        # Mimic the firmware's own scanner rather than a JSON parser.
        import re as _re
        t = _re.search(r'"t"\s*:\s*"([^"]*)"', data)
        rid = _re.search(r'"rid"\s*:\s*(-?[^,}]*)', data)
        ridv = None
        if rid:
            raw = rid.group(1).strip()
            try:
                v = int(raw)
                ridv = v % (1 << 32) if v >= 0 and v < (1 << 32) else 4294967295
            except ValueError:
                ridv = 0
        if not t:
            return
        cmd = t.group(1)
        if cmd == "pick":
            self.seated = True
            self._reply(ridv, cmd)
            self.inbox.put_nowait(json.dumps({"t": "asgn", "id": 5}))
            self.inbox.put_nowait(json.dumps({"t": "sync", "id": 5, "pv": 2, "tk": 1,
                                              "p": [{"on": 0}] * 5 + [{"on": 1, "ll": 5}]}))
        elif cmd == "check":
            self._reply(ridv, cmd, "bad_arg" if '"sk":9' in data else None)
        elif cmd == "m" and not self.seated:
            self._reply(ridv, cmd, "not_seated")
        else:
            self._reply(ridv, cmd, "unknown_cmd")

    async def close(self):
        self.close_code = 1000
        self.inbox.put_nowait(None)

    def __aiter__(self):
        return self

    async def __anext__(self):
        m = await self.inbox.get()
        if m is None:
            raise StopAsyncIteration
        return m


async def _probe_checks():
    fws = _FakeWS()

    async def _connect(url, **kw):
        return fws
    fl = FindingLog()
    pc = ProbeClient("fake", fl, min_interval=0.0, connect=_connect)
    fuzzmod.SILENCE_WAIT_S = 0.05
    fuzzmod.REPLY_WAIT_S = 0.5
    await pc.open()
    await pc.wait_for(lambda m: m.get("t") == "lobby", 1.0)
    res = {}
    by = {c.name: c for c in fz.CASES}
    for name in ("unseated:m", "frame:not-json", "frame:binary", "frame:fragmented",
                 "rid:string", "rid:huge", "prefix:e"):
        why, _sent, _reply = await fuzzmod.run_case(pc, by[name])
        res[name] = why
    slot = await pc.seat()
    why, _s, _r = await fuzzmod.run_case(pc, by["check:sk-9"])
    res["check:sk-9"] = why
    wrong = fz.Case("wrong", {"t": "check", "sk": 9, "dn": 5}, "ack")
    res["wrong"], _s, _r = await fuzzmod.run_case(pc, wrong)
    before = pc.arch
    await pc.abort()
    await pc.closed.wait()
    return res, slot, fl, pc, before, fws

_res, _slot, _pfl, _pc, _parch, _fws = asyncio.run(_probe_checks())
check("probe: every fuzz case the fake board satisfies passes",
      all(v is None for k, v in _res.items() if k != "wrong"))
check("probe: a wrong expectation is reported, not swallowed", _res["wrong"] is not None)
check("probe: a mangled rid is matched to what strtoul made of it",
      not any(s[0] == "reply_unmatched" for s in _pfl.counts))
check("probe: seat() takes the highest free slot and waits for sync",
      _slot == 5 and _parch == 5)
check("probe: silent cases leave nothing in flight", not _pc.tracker.inflight)
check("probe: abort drops the socket without a close handshake",
      _fws.aborted and _pc.closed.is_set())
check("probe: fragments really are sent fragmented",
      any(kind == "frag" for kind, _d in _fws.sent))

print("sentinel")
_os = Observation()
_os.apply(copy.deepcopy(sync))
_os.pid = 2
_os.map = w
_me = _os.players[2]
_me.q, _me.r, _me.connected, _me.ll, _me.mp = 10, 10, True, 7, 0
_me.inv = [5, 3, 0, 0, 0]
_me.valid_moves = 0
_sent_pol = pmod.make("sentinel", random.Random(3))
_sm_pol = pmod.make("scoremax", random.Random(3))
_sa = _sent_pol.decide(_os).to_msg()
_ma = _sm_pol.decide(_os).to_msg()
check("scoremax rests out of MP, the sentinel does not",
      _ma and _ma.get("a") == config.ACT_REST
      and not (_sa and _sa.get("t") == "act" and _sa.get("a") == config.ACT_REST))
_me.ll = 1
_sa = _sent_pol.decide(_os).to_msg()
check("not even at critical LL", not (_sa and _sa.get("a") == config.ACT_REST))
import arena as _arena
try:
    _arena.parse_args(["--policies", "sentinel", "--mode", "sprint"])
    _sprint_ok = True
except SystemExit:
    _sprint_ok = False
check("arena refuses a sentinel in sprint mode", not _sprint_ok)
check("arena --target 0 is a time-only run", _arena.parse_args(["--target", "0"]).target == 0)

print("sentinel camp")
from policy.sentinel import camp_value


def _sobs(q=10, r=10, mp=5, inv=(5, 3, 0, 0, 2)):
    o = Observation()
    o.apply(copy.deepcopy(sync))
    o.pid = 2
    o.map = copy.deepcopy(w)
    me = o.players[2]
    me.q, me.r, me.connected, me.ll, me.mp = q, r, True, 6, mp
    me.food, me.water, me.depth = 4, 4, 0
    me.inv = list(inv)
    me.inv_type = [0] * config.INV_SLOTS_MAX
    me.equip = [0] * config.EQUIP_SLOTS
    me.valid_moves = 0b111111 if mp else 0
    return o


def _is(act, a):
    m = act.to_msg()
    return bool(m) and m.get("t") == "act" and m.get("a") == a


check("camp: water beats forage beats bare scrub",
      camp_value(mk(3), 0, 0, 0) > camp_value(mk(0), 0, 0, 0) > camp_value(mk(7), 0, 0, 0))
check("camp: never radioactive, a hatch, the river or impassable",
      all(camp_value(mk(t), 0, 0, 0) is None for t in (1, 6, 10, 11, 12, 13)))
check("camp: an existing shelter is worth walking to",
      camp_value(Cell(terrain=0, footprints=0, shelter=2, poi=False, tire_track=False,
                      resource=0, variant=0), 0, 0, 3) > camp_value(mk(0), 0, 0, 0))

_so = _sobs()
_so.map.grid[10][12] = mk(3)                    # a Marsh two steps east
_sp = pmod.make("sentinel", random.Random(5))
_a = _sp.decide(_so)
check("picks the Marsh and walks to it", _sp.camp == (12, 10) and _a.kind == "move")

_so.players[2].q = 12                           # arrive
_a1 = _sp.decide(_so)
_a2 = _sp.decide(_so)
check("in camp with 2 scrap it builds a shelter", _is(_a1, config.ACT_SHELTER))
check("and does not re-send it while the map catches up",
      not _is(_a2, config.ACT_SHELTER))
_so.apply({"t": "ev", "k": "act", "pid": 2, "a": config.ACT_SHELTER, "out": 1, "cnd": 2})
check("state.py applies the act event's shelter level to the map",
      _so.map[(12, 10)].shelter == 2)
_so.players[2].inv = [1, 3, 0, 0, 0]
check("in camp and short of water it draws it from the Marsh",
      _is(_sp.decide(_so), config.ACT_WATER))
_so.players[2].inv = [5, 3, 0, 0, 0]
_a = _sp.decide(_so)
check("stocked and sheltered, it stays put", _a.kind == "noop" and "camp" in _a.why)
_so.players[2].mp = 0
_so.players[2].valid_moves = 0
check("out of MP early in the day it stays awake", not _is(_sp.decide(_so), config.ACT_REST))
_sp._dawn_tick = _so.tick - _sp.rest_at_ticks   # late in a day it watched begin
_a = _sp.decide(_so)
check("late in the day it rests, in camp", _is(_a, config.ACT_REST)
      and _sp.camp_stats["rests_in_camp"] == 1)

_so.apply({"t": "ev", "k": "quake", "cells": [{"q": 12, "r": 10}],
           "destroyed": [{"q": 12, "r": 10}], "converted": []})
check("state.py: a quake knocks the shelter down", _so.map[(12, 10)].shelter == 0)
_so.apply({"t": "ev", "k": "settle", "removed": [], "q": 12, "r": 10})
check("state.py: a settlement founding changes the terrain", _so.map[(12, 10)].terrain == 9)
_so.apply({"t": "ev", "k": "quake", "destroyed": [{"r": 3}], "converted": []})
check("state.py: an event without coordinates touches nothing",
      _so.map[(74, 3)].shelter == 0 and _so.map[(74, 56)].terrain == 0)

_sc = _sobs(q=30, r=30, mp=2, inv=(5, 3, 0, 0, 1))
_sp2 = pmod.make("sentinel", random.Random(6))
_sp2._day = _sc.day                             # a day it watched begin...
_sp2.camp = (40, 40)                            # ...with camp far away
_sp2._dawn_tick = _sc.tick - _sp2.rest_at_ticks
check("caught out at bedtime on exposed ground, it shelters where it stands",
      _is(_sp2.decide(_sc), config.ACT_SHELTER))
_sc.apply({"t": "ev", "k": "act", "pid": 2, "a": config.ACT_SHELTER, "out": 1, "cnd": 1})
check("then goes to sleep", _is(_sp2.decide(_sc), config.ACT_REST))

print("config drift (firmware source is the authority)")
_fw_src = _ino + "".join((_FW / f).read_text(encoding="utf-8")
                         for f in ("tunnels.hpp",))


def _fw_value(name):
    m = re.search(r"\b" + name + r"\s*(?:\[[^\]]*\])?\s*=\s*(\{[^}]*\}|-?\d+)", _fw_src)
    if not m:
        return None
    v = m.group(1)
    if v.startswith("{"):
        return tuple(int(x) for x in re.findall(r"-?\d+", v))
    return int(v)


_mirrored = ["MAP_COLS", "MAP_ROWS", "MAX_PLAYERS", "MAX_SEATED", "TICK_MS", "DAY_TICKS",
             "NUM_TERRAIN", "INV_SLOTS_MAX", "EQUIP_SLOTS", "TUN_COLS", "TUN_ROWS",
             "MAX_HATCHES", "VENT_ASCEND_MP", "ITEM_BILE_FLARE", "TUNNEL_REST_LL_PCT",
             "DQ", "DR", "TERRAIN_MC", "TERRAIN_SV", "TERRAIN_FORAGE_DN",
             "TERRAIN_SALVAGE_DN", "TERRAIN_HAS_WATER", "TERRAIN_IS_RUINS", "TERRAIN_IS_RAD",
             "ACT_FORAGE", "ACT_WATER", "ACT_TREAT", "ACT_SCAV", "ACT_SHELTER",
             "ACT_CRAFT", "ACT_SURVEY", "ACT_REST",
             "WEATHER_CLEAR", "WEATHER_RAIN", "WEATHER_STORM", "WEATHER_CHEM",
             "WEATHER_FOG", "WEATHER_MIST"]
_drift = []
for _n in _mirrored:
    _fv = _fw_value(_n)
    _pv = getattr(config, _n)
    _pv = tuple(_pv) if isinstance(_pv, (list, tuple)) else _pv
    if _fv is None:
        _drift.append((_n, "not found in firmware"))
    elif _fv != _pv:
        _drift.append((_n, f"firmware {_fv} != config.py {_pv}"))
check(f"config.py matches the firmware on {len(_mirrored)} constants {_drift}", not _drift)

print("soak report")
import soak as soakmod
check("heap slope is per minute", round(soakmod.slope_per_min(
    [(0, 1000), (60, 900), (120, 800)]), 1) == -100.0)
_srows = [{"ts": 0.0, "ch": "run", "arch": -1, "d": {}}]
for _d in range(1, 8):
    _srows.append({"ts": _d * 300.0, "ch": "rx", "arch": 0,
                   "d": {"t": "ev", "k": "dawn", "pid": 0, "day": _d, "sq": _d}})
for _m in range(0, 36):
    _srows.append({"ts": _m * 60.0, "ch": "telemetry", "arch": -1,
                   "d": {"heap": 150000 - _m * 1000, "minHeap": 90000,
                         "maxTickMs": 120, "evtDrops": 0}})
_srows.append({"ts": 400.0, "ch": "rx", "arch": 0,
               "d": {"t": "ev", "k": "dmg", "pid": 0, "amt": 1, "cause": "chem storm", "sq": 50}})
_sfl = FindingLog()
_rep = soakmod.soak_report(_srows, _sfl, out=io.StringIO())
check("soak reads full-length days", _rep["mean_day_min"] == 5.0 and _rep["short_days"] == 0)
check("soak counts real-clock hazards", _rep["hazards"].get("chem storm") == 1)
check("a steady heap decline over a long soak is a leak finding",
      ("heap_trend",) in _sfl.counts)

print("chaos")
import chaos as chaosmod
check("chaos scenarios are registered",
      {"reconnect_storm", "seat_race", "abort_request", "abort_encounter",
       "trade_then_leave", "stalled_reader"} == set(chaosmod.SCENARIOS))

print("gear audit (gearcheck.py -- the tool that checks the items)")
# An audit that cannot fail is not an audit. These feed it one item whose
# declared mod lands and one whose does not, and require it to tell them
# apart -- plus the case that matters most in practice, an item that was
# worn but whose mod nothing on the wire can speak to.
import gearcheck

LL_ITEM = next((i for i, st in config.EQUIP_STATS.items() if st["ll"]), None)
SLOT_ITEM = next((i for i, st in config.EQUIP_STATS.items() if st["slots"]), None)
OPAQUE_ITEM = next((i for i, st in config.EQUIP_STATS.items()
                    if st["terrain"] and not st["ll"] and not st["slots"]
                    and not st["vision"]), None)
check("items.cfg has an LL item to test with", LL_ITEM is not None)


def _log(tmp, rows):
    p = Path(tmp) / "g.jsonl"
    with Recorder(p, {"synthetic": True}) as rec:
        for ch, arch, d in rows:
            rec.write(ch, arch, d)
    return p


def _audit(rows):
    with tempfile.TemporaryDirectory() as td:
        a = gearcheck.GearAudit()
        a.add_run(_log(td, rows))
        return a


def _pack(eq, **kw):
    d = {"t": "item_result", "act": "equip", "eq": list(eq),
         "is": kw.get("is", 8), "llCap": kw.get("llCap", 7)}
    return d


bare = ("rx", 0, _pack([0, 0, 0, 0, 0]))
slot_of = config.EQUIPMENT[LL_ITEM]
worn = [0] * 5
worn[slot_of] = LL_ITEM
bump = config.EQUIP_STATS[LL_ITEM]["ll"]

a = _audit([bare, ("rx", 0, _pack(worn, llCap=7 + bump))])
check("a correctly applied LL mod reads VERIFIED",
      a.ev[(LL_ITEM, "ll")]["pass"] == 1 and a.ev[(LL_ITEM, "ll")]["fail"] == 0)

a = _audit([bare, ("rx", 0, _pack(worn, llCap=7))])
check("an LL mod that never lands is caught",
      a.ev[(LL_ITEM, "ll")]["fail"] == 1)
check("and the failure says what it saw",
      "declared" in a.ev[(LL_ITEM, "ll")]["cases"][0])

a = _audit([bare, ("rx", 0, _pack(worn, llCap=7 + bump + 1))])
check("an LL mod applied TWICE is caught too",
      a.ev[(LL_ITEM, "ll")]["fail"] == 1)

if SLOT_ITEM is not None:
    sslot = config.EQUIPMENT[SLOT_ITEM]
    sworn = [0] * 5
    sworn[sslot] = SLOT_ITEM
    sbump = config.EQUIP_STATS[SLOT_ITEM]["slots"]
    a = _audit([bare, ("rx", 0, _pack(sworn, **{"is": 8 + sbump}))])
    check("a pack-slot mod is verified off `is`",
          a.ev[(SLOT_ITEM, "slots")]["pass"] == 1)
    # invSlots is clamped at INV_SLOTS_MAX, so a bonus that would overflow
    # lands short and that is correct, not a bug.
    a = _audit([("rx", 0, _pack([0, 0, 0, 0, 0], **{"is": config.INV_SLOTS_MAX})),
                ("rx", 0, _pack(sworn, **{"is": config.INV_SLOTS_MAX}))])
    check("the INV_SLOTS_MAX clamp is not reported as a mismatch",
          a.ev[(SLOT_ITEM, "slots")]["fail"] == 0)

# Vision rides a fresh vis disk that pushVisDisk() sends right after the ack.
VIS_ITEM = next((i for i, st in config.EQUIP_STATS.items() if st["vision"]), None)
if VIS_ITEM is not None:
    vslot = config.EQUIPMENT[VIS_ITEM]
    vworn = [0] * 5
    vworn[vslot] = VIS_ITEM
    a = _audit([("rx", 0, {"t": "vis", "vr": 3, "cells": ""}),
                ("rx", 0, _pack([0, 0, 0, 0, 0])),
                ("rx", 0, _pack(vworn)),
                ("rx", 0, {"t": "vis", "vr": 4, "cells": ""})])
    check("a vision mod is verified off the vis disk",
          a.ev[(VIS_ITEM, "vision")]["pass"] == 1)
    a = _audit([("rx", 0, {"t": "vis", "vr": 3, "cells": ""}),
                ("rx", 0, _pack([0, 0, 0, 0, 0])),
                ("rx", 0, _pack(vworn)),
                ("rx", 0, {"t": "vis", "vr": 3, "cells": ""})])
    check("a vision mod that never lands is caught",
          a.ev[(VIS_ITEM, "vision")]["fail"] == 1)
    # A step between the equip and the disk changes vision through terrain
    # and weather, so that disk is no longer evidence either way.
    a = _audit([("rx", 0, {"t": "vis", "vr": 3, "cells": ""}),
                ("rx", 0, _pack([0, 0, 0, 0, 0])),
                ("rx", 0, _pack(vworn)),
                ("rx", 0, {"t": "ev", "k": "mv", "pid": 0, "q": 1, "r": 1}),
                ("rx", 0, {"t": "vis", "vr": 9, "cells": ""})])
    check("a vis disk after a move is not used as evidence",
          a.ev[(VIS_ITEM, "vision")]["pass"] == 0
          and a.ev[(VIS_ITEM, "vision")]["fail"] == 0)

if OPAQUE_ITEM is not None:
    oslot = config.EQUIPMENT[OPAQUE_ITEM]
    oworn = [0] * 5
    oworn[oslot] = OPAQUE_ITEM
    a = _audit([bare, ("rx", 0, _pack(oworn))])
    check("an item worn with only opaque mods is reported, not passed",
          OPAQUE_ITEM in a.worn_ever
          and not any(k[0] == OPAQUE_ITEM and v["pass"]
                      for k, v in a.ev.items()))
    rendered = a.render()
    check("and the report calls it UNTESTED", "UNTESTED" in rendered)

# Survival attribution: a dawn's LL loss lands on whatever was worn at it.
a = _audit([bare,
            ("rx", 0, {"t": "ev", "k": "dawn", "pid": 0, "day": 2, "dll": -2}),
            ("rx", 0, _pack(worn, llCap=7 + bump)),
            ("rx", 0, {"t": "ev", "k": "dawn", "pid": 0, "day": 3, "dll": -1}),
            ("rx", 0, {"t": "ev", "k": "dawn", "pid": 0, "day": 4, "dll": 0})])
check("bare dawns form the baseline", a.baseline_dawns == 1 and a.baseline_ll == 2)
check("worn dawns are attributed to the item",
      a.days_worn[LL_ITEM] == 2 and a.ll_lost[LL_ITEM] == 1)
check("another player's dawn is not ours",
      _audit([bare, ("rx", 0, {"t": "ev", "k": "dawn", "pid": 3, "dll": -5})]
             ).baseline_dawns == 0)

print("canteen (the craftable answer to thirst)")
check("the canteen recipe is found by what it does",
      config.CANTEEN_RECIPE is not None
      and config.EQUIP_STATS[config.CANTEEN_ITEM]["water_cap"] > 0
      and config.RECIPES[config.CANTEEN_RECIPE]["cost"][config.RES_SCRAP] > 0)
check("recipe bits are (id-1)", config.recipe_known(1 << 18, 19)
      and not config.recipe_known(1 << 18, 18))

# tokenLoad(): canteen water is outside the pack, and tokenRoomFor(0) adds
# the canteen's empty space on top.
_me = __import__("state").PlayerState(inv=[5, 2, 0, 0, 1], inv_slots=8, water_cap=3)
check("canteen water does not count against the pack", _me.carried() == 5)
check("water room includes the empty canteen", _room(_me, config.RES_WATER) == 3)
_me.inv = [2, 3, 0, 0, 3]
check("a part-full canteen only offers what is left",
      _room(_me, config.RES_WATER) == 3 and _room(_me) == 2)
_st = Observation()
_st.apply(sync)
_st.pid = 2
_st.apply({"t": "item_result", "ok": True, "wc": 3, "kr": 1 << 18,
           "eq": [0, config.CANTEEN_ITEM, 0, 0, 0]})
check("wc and kr ride an item_result",
      _st.me.water_cap == 3 and _st.me.known_recipes == 1 << 18)
# gearcheck.py: the Canteen's water_cap is checked off `wc`, the way slots is
# off `is`. It used to be left out entirely -- the report said the Canteen
# "declares no stat mods at all", so no run ever tested it.
_cw = [0] * 5
_cw[config.EQUIPMENT[config.CANTEEN_ITEM]] = config.CANTEEN_ITEM
_wcap = config.EQUIP_STATS[config.CANTEEN_ITEM]["water_cap"]
_bare_wc = ("rx", 0, dict(_pack([0] * 5), wc=0))
a = _audit([_bare_wc, ("rx", 0, dict(_pack(_cw), wc=_wcap))])
check("gearcheck verifies the canteen's water_cap off `wc`",
      a.ev[(config.CANTEEN_ITEM, "water_cap")]["pass"] == 1)
a = _audit([_bare_wc, ("rx", 0, dict(_pack(_cw), wc=0))])
check("and catches a canteen that grants nothing",
      a.ev[(config.CANTEEN_ITEM, "water_cap")]["fail"] == 1)


def canteen_obs(scrap=4, terrain=config.TERR_SETTLEMENT, kr=1 << 18,
                equip=None, water=5, food=4):
    o = Observation()
    o.apply(sync)
    o.pid = 2
    w = WorldMap()
    for rr in range(config.MAP_ROWS):
        for qq in range(config.MAP_COLS):
            w.grid[rr][qq] = mk()
    w.grid[10][10] = mk(terrain)
    o.map = w
    me = o.players[2]
    me.q, me.r, me.depth = 10, 10, 0
    me.inv = [water, food, 0, 0, scrap]
    me.mp, me.ll, me.inv_slots, me.valid_moves = 8, 6, 8, 0b111111
    me.known_recipes = kr
    me.inv_type = [0] * config.INV_SLOTS_MAX
    me.equip = list(equip or [0] * 5)
    return o


_c = pmod.make("coward", random.Random(1)); _c.set_pid(2)
a = _c.decide(canteen_obs())
check("at a Settlement with the scrap, it crafts the canteen",
      a.kind == "act" and a.to_msg()["a"] == config.ACT_CRAFT
      and a.to_msg()["r"] == config.CANTEEN_RECIPE)
_c2 = pmod.make("coward", random.Random(1)); _c2.set_pid(2)
_cost = config.RECIPES[config.CANTEEN_RECIPE]["cost"][config.RES_SCRAP]
check("not without the scrap",
      "craft" not in _c2.decide(canteen_obs(scrap=_cost - 1)).why)
_c3 = pmod.make("coward", random.Random(1)); _c3.set_pid(2)
check("not without the recipe", "craft" not in _c3.decide(canteen_obs(kr=0)).why)
_c4 = pmod.make("coward", random.Random(1)); _c4.set_pid(2)
_o = canteen_obs(terrain=0)
_o.map.grid[10][13] = mk(config.TERR_SETTLEMENT)
a = _c4.decide(_o)
check("away from one, it walks to the nearest Settlement",
      a.kind == "move" and "canteen: to settlement (13,10)" in a.why)
_c5 = pmod.make("coward", random.Random(1)); _c5.set_pid(2)
_o = canteen_obs()
_c5.decide(_o)                               # sends the CRAFT
_o.me.inv_type[0] = config.CANTEEN_ITEM      # ...and the item_result lands
a = _c5.decide(_o)
check("a crafted canteen is equipped, not crafted again",
      a.kind == "equip_item" and a.slot == 0)
check("and the craft is counted", _c5.stats["crafted"] == 1)
_c6 = pmod.make("coward", random.Random(1)); _c6.set_pid(2)
_o = canteen_obs()
_c6.decide(_o)
check("a refused craft is not re-sent every cycle",
      "craft" not in _c6.decide(_o).why)
# On a pond, short of its food floor, a Settlement 3 hexes off: the routine
# top-up must not eat the day (the ~100 s park at (48,28) on 2026-09-23).
_o = canteen_obs(terrain=3, water=4, food=1, scrap=_cost)
_o.map.grid[10][13] = mk(config.TERR_SETTLEMENT)
_sub = pmod.make("subterranean", random.Random(1)); _sub.set_pid(2)
a = _sub.decide(_o)
check("a Settlement within today's MP beats topping up",
      a.kind == "move" and "canteen: to settlement (13,10)" in a.why)
_o = canteen_obs(terrain=3, water=1, food=1, scrap=_cost)
_o.map.grid[10][13] = mk(config.TERR_SETTLEMENT)
_sub = pmod.make("subterranean", random.Random(1)); _sub.set_pid(2)
check("but an emergency still comes first", "canteen" not in _sub.decide(_o).why)
# Every policy wants one -- even wearing a Backpack in the shared body slot.
_CS = config.EQUIPMENT[config.CANTEEN_ITEM]
_bp = [0] * 5
_bp[_CS] = 64
_all = [n for n in pmod.REGISTRY if n != "drunk"]
check("every survivor policy wants a canteen, Backpack or not",
      all(pmod.make(n, random.Random(1)).wants_canteen(canteen_obs(equip=_bp))
          for n in _all))
# ...puts it on over the Backpack...
_sm = pmod.make("scoremax", random.Random(1)); _sm.set_pid(2)
_o = canteen_obs(terrain=0, equip=_bp)
_o.me.inv_type[3] = config.CANTEEN_ITEM
a = _sm.decide(_o)
check("scoremax swaps its Backpack out for the canteen",
      a.kind == "equip_item" and a.slot == 3 and "canteen" in a.why)
# ...and never swaps it back, however much its weights like the Backpack.
_o = canteen_obs(terrain=0)
_o.me.equip[_CS] = config.CANTEEN_ITEM
_o.me.inv_type[0] = 64
_sm2 = pmod.make("scoremax", random.Random(1)); _sm2.set_pid(2)
check("a worn canteen is pinned: no swap back to the Backpack",
      _sm2.decide(_o).kind != "equip_item")
# An equip that does not land is not re-sent every cycle.
_o = canteen_obs(terrain=0)
_o.me.inv_type[3] = config.CANTEEN_ITEM
_eq = pmod.make("coward", random.Random(1)); _eq.set_pid(2)
_first = _eq.decide(_o)
check("an unacknowledged equip is not re-sent every cycle",
      _first.kind == "equip_item" and _eq.decide(_o).kind != "equip_item")
# Short of scrap: go and get it -- a pile, or salvage where it stands.
_o = canteen_obs(terrain=0, scrap=0, water=4, food=3)
_o.map.grid[10][12] = mk(resource=config.RES_SCRAP + 1)
_sc = pmod.make("contentmax", random.Random(1)); _sc.set_pid(2)
a = _sc.decide(_o)
check("short of scrap, it walks to a scrap pile",
      a.kind == "move" and "canteen: scrap -> (12,10)" in a.why)
_o = canteen_obs(terrain=4, scrap=0, water=4, food=3)  # Urban: salvageable
_sc2 = pmod.make("contentmax", random.Random(1)); _sc2.set_pid(2)
a = _sc2.decide(_o)
check("standing on salvage, it scavenges for it",
      a.kind == "act" and a.to_msg()["a"] == config.ACT_SCAV)
# Stood on water with a canteen worn: fill it past the ordinary floor.
_f = pmod.make("scoremax", random.Random(1)); _f.set_pid(2)
_o = canteen_obs(terrain=3, water=4, food=2, scrap=0)
_o.me.water_cap = 3
_o.me.equip[config.EQUIPMENT[config.CANTEEN_ITEM]] = config.CANTEEN_ITEM
a = _f.decide(_o)
check("on a pond, a worn canteen gets filled",
      a.kind == "act" and a.to_msg()["a"] == config.ACT_WATER and "canteen" in a.why)

print("death drops (the canteen a respawn used to lose)")
# dropRemains(): a fall leaves the pack and worn gear as ground piles and the
# tokens in a remains record ("rm"), on the hex, for GROUND_AGE_DAYS.
_grave = {"q": 12, "r": 10, "pid": 2, "nm": "Medic", "d": 11, "res": [2, 1, 0, 0, 3]}
_od = Observation()
_od.apply(dict(sync, rm=[_grave]))
check("sync parses the remains records", _od.remains == [_grave])
check("remains_at finds a grave by its hex",
      _od.remains_at(12, 10) is _od.remains[0] and _od.remains_at(10, 10) is None)
_od.apply({"t": "ground_update", "q": 12, "r": 10, "gi": []})
check("a ground_update without rm keeps what we had", len(_od.remains) == 1)
_od.apply({"t": "ground_update", "q": 12, "r": 10, "gi": [], "rm": []})
check("a ground_update with rm replaces it", _od.remains == [])
_mine = {"q": 20, "r": 5, "pid": 2, "nm": "Medic", "d": 11, "res": [1, 0, 0, 0, 0]}
_theirs = {"q": 30, "r": 6, "pid": 4, "nm": "Scout", "d": 11, "res": [0, 1, 0, 0, 0]}
_od.apply({"t": "ground_update", "q": 20, "r": 5, "why": "fell", "pid": 2,
           "gi": [], "rm": [_mine]})
check("our own fall is remembered as our grave", (20, 5) in _od.my_graves)
_od.apply({"t": "ground_update", "q": 30, "r": 6, "why": "fell", "pid": 4,
           "gi": [], "rm": [_mine, _theirs]})
check("somebody else's fall is not",
      (30, 6) not in _od.my_graves and (20, 5) in _od.my_graves)
_od.apply({"t": "ground_update", "q": 20, "r": 5, "gi": [], "rm": [_theirs]})
check("a grave of ours that is gone is forgotten", not _od.my_graves)
_od.apply({"t": "ground_update", "q": 20, "r": 5, "why": "fell", "pid": 2,
           "gi": [{"g": 0, "q": 20, "r": 5, "id": 65, "n": 1, "d": 11}], "rm": [_mine]})
_od.apply({"t": "ev", "k": "regen"})
check("regen forgets graves, remains and piles",
      not _od.remains and not _od.my_graves and not _od.ground_items)
_od.apply(sync)
_od.apply({"t": "loot_result", "ok": True, "why": 0, "pid": 2,
           "got": [2, 0, 0, 0, 0], "inv": [9, 1, 0, 0, 0]})
check("loot_result applies the fresh token counts", _od.me.inv == [9, 1, 0, 0, 0])
check("a grave lasts GROUND_AGE_DAYS",
      config.ground_days_left(11, 11) == config.GROUND_AGE_DAYS)
check("and goes at the dawn that makes it that old",
      config.ground_days_left(11 + config.GROUND_AGE_DAYS, 11) == 0)
check("the day wraps like the board's uint16",
      config.ground_days_left(3, 65534) == config.GROUND_AGE_DAYS - 5)

check("a bare loot takes everything that fits", Action("loot").to_msg() == {"t": "loot"})
check("a loot can name one resource",
      Action("loot", res=5).to_msg() == {"t": "loot", "res": 5})
try:
    Action("loot", res=6).to_msg()
    check("an out-of-range loot res is refused", False)
except ValueError:
    check("an out-of-range loot res is refused", True)


def grave_obs(res=(2, 1, 0, 0, 1), at=(10, 10), pid=2, nm="Medic", d=11, day=11,
              gi=(), base=None, **kw):
    """A grave at `at` holding `res`, with ground piles `gi`.  Built on
    surface_obs() unless `base` is given (canteen_obs() for the craft cases).
    food 2 / water 3 by default: 5 tokens in an 8-slot pack, so there is room
    to loot and nothing counts as an emergency."""
    kw.setdefault("food", 2)
    kw.setdefault("water", 3)
    o = base if base is not None else surface_obs(**kw)
    o.day = day
    o.remains = [{"q": at[0], "r": at[1], "pid": pid, "nm": nm, "d": d, "res": list(res)}]
    o.ground_items = [dict(g) for g in gi]
    o.players[2].inv_type = [0] * config.INV_SLOTS_MAX
    o.players[2].equip = [0] * config.EQUIP_SLOTS
    return o


_CANTEEN_PILE = {"g": 7, "q": 10, "r": 10, "id": config.CANTEEN_ITEM, "n": 1, "d": 11}
_lp = pmod.make("scoremax", random.Random(2)); _lp.set_pid(2)
_o = grave_obs()
a = _lp.decide(_o)
check("stood on a grave with room, it loots", a.kind == "loot" and a.to_msg() == {"t": "loot"})
check("and says whose it was", "our own" in a.why)
check("an unacknowledged loot is not re-sent every cycle", _lp.decide(_o).kind != "loot")
check("the loot is counted", _lp.stats["loots"] == 1)
_lp2 = pmod.make("coward", random.Random(2)); _lp2.set_pid(2)
a = _lp2.decide(grave_obs(pid=4, nm="Scout"))
check("a rival's grave is looted just the same", a.kind == "loot" and "Scout's" in a.why)
_lp3 = pmod.make("scoremax", random.Random(2)); _lp3.set_pid(2)
_o = grave_obs(food=4, water=4)                # 8 of 8: nothing fits
check("a full pack does not send a loot that can only be refused",
      _lp3.decide(_o).kind != "loot")
_o = grave_obs(res=(0, 0, 0, 0, 2), food=4, water=4)
_o.players[2].inv = [4, 3, 0, 0, 0]            # 7 of 8: room for one
check("one token of room is room", _lp3.decide(_o).kind == "loot")
_o = grave_obs()
_o.players[2].depth = 1                        # q/r pinned to the hatch overhead
check("never from below -- underground q/r are the hatch, not the grave",
      _lp3.remains_action(_o) is None)
# The canteen on its own grave is picked up before the canteen errand can
# walk the respawned survivor off to hunt scrap for a new one.
_cn = canteen_obs(terrain=0, scrap=0, water=3, food=2)
_cn.map.grid[10][12] = mk(resource=config.RES_SCRAP + 1)
_o = grave_obs(res=(0, 0, 0, 0, 0), gi=[_CANTEEN_PILE], base=_cn)
_cg = pmod.make("contentmax", random.Random(1)); _cg.set_pid(2)
a = _cg.decide(_o)
check("its canteen on the grave is lifted before any errand",
      a.kind == "pickup_item" and a.gslot == 7 and "grave" in a.why)
check("and a lifted pile is not asked for again every cycle",
      _cg.decide(_o).kind != "pickup_item")
_o = grave_obs(res=(0, 0, 0, 0, 0), gi=[dict(_CANTEEN_PILE, id=21)])
check("litter on a grave stays where it is", _lp3.remains_action(_o) is None)


def recover_obs(at=(14, 10), pid=2, d=11, day=11, gi=None, res=(0, 0, 0, 0, 0),
                water=3, food=2, scrap=0):
    """canteen_obs(): the canteen recipe known, 8 MP, scrap short -- with a
    scrap pile two hexes WEST to tempt the canteen errand, and a grave to the
    east holding the canteen."""
    base = canteen_obs(terrain=0, scrap=scrap, water=water, food=food)
    base.map.grid[10][8] = mk(resource=config.RES_SCRAP + 1)
    pile = dict(_CANTEEN_PILE, q=at[0], r=at[1]) if gi is None else gi
    return grave_obs(res=res, at=at, pid=pid, d=d, day=day,
                     gi=[pile] if pile else [], base=base)


_rp = pmod.make("contentmax", random.Random(1)); _rp.set_pid(2)
_o = recover_obs()
a = _rp.decide(_o)
check("a grave of ours within today's MP: walk back for the canteen",
      a.kind == "move" and a.d == 0 and "recover: our grave at (14,10)" in a.why)
check("and the canteen errand stands down while it does",
      _rp.craft_action(_o) is None)
check("the walk is counted", _rp.stats["recover_moves"] == 1)
_rp2 = pmod.make("contentmax", random.Random(1)); _rp2.set_pid(2)
a = _rp2.decide(recover_obs(at=(25, 10)))
check("beyond today's MP it still goes, once the floor is quiet",
      a.kind == "move" and "recover: our grave at (25,10)" in a.why)
_rp3 = pmod.make("contentmax", random.Random(1)); _rp3.set_pid(2)
a = _rp3.decide(recover_obs(at=(25, 10), day=40, d=12))     # 2 dawns left, 2 days away
check("but not for a grave that ages out on the way", "recover" not in a.why)
_rp4 = pmod.make("contentmax", random.Random(1)); _rp4.set_pid(2)
a = _rp4.decide(recover_obs(day=40, d=11))                   # goes at the next dawn, 4 MP off
check("a grave going at dawn is still worth a walk that ends today",
      "recover: our grave at (14,10)" in a.why)
_rp5 = pmod.make("contentmax", random.Random(1)); _rp5.set_pid(2)
a = _rp5.decide(recover_obs(pid=4))
check("somebody else's grave is not ours to walk back to", "recover" not in a.why)
_o = recover_obs(pid=3)
_o.my_graves.add((14, 10))
_rp6 = pmod.make("contentmax", random.Random(1)); _rp6.set_pid(2)
check("a grave from before a respawn into another seat is still ours",
      "recover: our grave at (14,10)" in _rp6.decide(_o).why)
_o = recover_obs()
_o.map.grid[10][14] = mk(TERR_BUNKER)
_rp7 = pmod.make("contentmax", random.Random(1)); _rp7.set_pid(2)
check("a grave on a hatch is not walked to -- stepping on it is the descent",
      "recover" not in _rp7.decide(_o).why)
_rp8 = pmod.make("contentmax", random.Random(1)); _rp8.set_pid(2)
check("a grave with nothing we could take is not worth the walk",
      "recover" not in _rp8.decide(recover_obs(gi=dict(_CANTEEN_PILE, q=14, id=21))).why)
_rp9 = pmod.make("contentmax", random.Random(1)); _rp9.set_pid(2)
a = _rp9.decide(recover_obs(gi={}, res=(3, 0, 0, 0, 0)))     # tokens only, no canteen
check("a grave of tokens alone is still worth walking to",
      "recover: our grave at (14,10)" in a.why)
check("but it does not stop the canteen errand",
      _rp9.recovery_target(recover_obs(gi={}, res=(3, 0, 0, 0, 0)),
                           canteen_only=True) is None)
_o = recover_obs(water=1)
_o.map.grid[10][11] = mk(resource=config.RES_WATER + 1)
_rp10 = pmod.make("contentmax", random.Random(1)); _rp10.set_pid(2)
check("an emergency still comes first", "EMERGENCY water" in _rp10.decide(_o).why)

# The Rival's pickup used to be a placeholder noop -- which parked it on any
# pile forever. Graves full of consumables made that a freeze.
_rv = pmod.make("rival", random.Random(1)); _rv.set_pid(2)
_o = surface_obs(food=2, water=3)
_o.ground_items = [{"g": 5, "q": 10, "r": 10, "id": 21, "n": 3, "d": 11}]
a = _rv.decide(_o)
check("the rival lifts litter underfoot", a.kind == "pickup_item" and a.gslot == 5)
_o.players[2].inv_type = [21] * config.INV_SLOTS_MAX
_rv2 = pmod.make("rival", random.Random(1)); _rv2.set_pid(2)
check("and with no free slot it moves on instead of freezing",
      _rv2.decide(_o).kind == "move")
# GearMax only walks to piles it will lift; a consumable pile used to pull it
# back and forth forever.
_gm = pmod.make("gearmax", random.Random(1)); _gm.set_pid(2)
_o = surface_obs(food=2, water=3)
_o.ground_items = [{"g": 3, "q": 11, "r": 10, "id": 21, "n": 1, "d": 11}]
a = _gm.pursue(_o)
check("gearmax does not chase a pile it will not pick up",
      float(a.why.split("v=")[1]) < 10)
_o.ground_items = [{"g": 3, "q": 11, "r": 10, "id": 64, "n": 1, "d": 11}]
check("but still chases gear", "(11,10)" in _gm.pursue(_o).why)
_o.players[2].inv_type = [21] * config.INV_SLOTS_MAX
check("unless there is no slot to put it in",
      float(_gm.pursue(_o).why.split("v=")[1]) < 10)

# Every survivor policy, dropped on a grave with tokens and gear on it, must
# keep producing actions the wire can carry.
for _name in (n for n in pmod.REGISTRY if n != "drunk"):
    _pol = pmod.make(_name, random.Random(5)); _pol.set_pid(2)
    _o = grave_obs(gi=[_CANTEEN_PILE, dict(_CANTEEN_PILE, g=8, id=21)])
    _kinds = set()
    for _ in range(40):
        _act = _pol.decide(_o)
        _kinds.add(_act.kind)
        _m = _act.to_msg()
        if _m is not None:
            json.dumps(_m)
    check(f"{_name} on a grave yields valid actions",
          _kinds and _kinds <= {"move", "act", "noop", "loot", "pickup_item",
                                "equip_item", "enc_start", "trade_offer"})

print("booby traps (traps.hpp)")
# TT bit 4 is the per-recipient "a trap you escaped" flag; terrain is 0-3.
_tc = decode_cell(0x04 | 0x10, 0, 0)
check("trap bit decodes off TT bit 4", _tc.trap is True and _tc.terrain == 4)
check("terrain no longer reads the trap bit as terrain 20",
      decode_cell(0x04 | 0x10 | 0x40 | 0x80, 0x40, 0).terrain == 4)
check("no bit, no trap", decode_cell(0x04, 0, 0).trap is False)
check("a known trap is priced, not walled off",
      navigate.step_cost(_tc) == config.TERRAIN_MC[4] + navigate.TRAP_KNOWN_COST)
_tw = WorldMap()
for rr in range(config.MAP_ROWS):
    for qq in range(config.MAP_COLS):
        _tw.grid[rr][qq] = mk()
_tw.grid[10][11] = mk()
_tw.grid[10][11].trap = True
_td, _tf = navigate.dijkstra(_tw, 10, 10, max_cost=20)
check("the path goes around a trap we escaped",
      _td[(12, 10)] == 3 and _tf[(12, 10)] != 0)

_to = Observation()
_to.pid = 2
_to.map = _tw
_to.apply({"t": "ev", "k": "trap", "pid": 2, "q": 13, "r": 10, "dp": 0, "out": "known"})
check("ev trap known lands on our map", _tw.grid[10][13].trap is True)
_to.apply({"t": "ev", "k": "trap", "pid": 4, "q": 13, "r": 10, "dp": 0, "out": "sprung"})
check("anyone springing it clears it", _tw.grid[10][13].trap is False)
_to.apply({"t": "ev", "k": "trap", "pid": 2, "q": 5, "r": 3, "dp": 1, "out": "known"})
check("a trap below stays off the surface map", _tw.grid[3][5].trap is False)
_to.apply({"t": "ev", "k": "trap", "pid": 2, "q": "x", "r": None, "out": "known"})
check("a malformed trap event is tolerated", True)

check("the trap pool is in index.json", lib.index.get("traps", {}).get("count") == 20)
_tlib = [lib.get("traps", i) for i in range(1, 21)]
check("all twenty trap files load", all(e and "nodes" in e for e in _tlib))
_trun = enc_mod.EncounterRun(lib, "traps", 1)
check("a trap run is not at an escape node yet", not _trun.at_escape)
_esc, _wrk = _trun.escape_choice(), _trun.work_choice()
check("a trap start has a way out and a way in",
      _esc is not None and _wrk is not None and _esc != _wrk)
# Every trap file: exactly one back-out at the start, and it leads to a node
# flagged escape -- the rule the bot reads to know which door is which.
_bad = []
for _i in range(1, 21):
    _r = enc_mod.EncounterRun(lib, "traps", _i)
    _chs = _r.choices
    if len(_chs) < 2 or _r.escape_choice() is None or _r.work_choice() is None or _r.can_bank():
        _bad.append(_i)
check("every trap start is back-out-or-work, never bankable", not _bad)


def _trap_obs(ll=6, sk=(1, 1, 3, 1, 1), eid=1):
    _o = surface_obs(ll=ll)
    _o.players[2].skills = list(sk)
    _o.encounter = {"t": "enc_path", "biome": "traps", "id": eid, "trap": 1}
    return _o


_sv = pmod.make("scoremax", random.Random(9)); _sv.set_pid(2)
_o = _trap_obs()
_a = _sv.decide(_o)
check("a trap binds as a trap, not a POI",
      _sv.run is not None and _sv.run.trap and _sv.stats["traps"] == 1
      and _sv.stats["encounters_opened"] == 0)
_pw = enc_mod.success_chance(_sv.run.choices[_sv.run.work_choice()], _o)
check("good odds and LL to spare: work it",
      _pw >= _sv.trap_nerve and _a.kind == "enc_choice" and _a.ci == _sv.run.work_choice())
_sv.run.on_result({"out": 1})
check("the work choice lands on the cache", _sv.run.can_bank())
_a = _sv.decide(_o)
check("from the cache it presses on or takes it",
      _a.kind in ("enc_choice", "enc_bank"))
_sv2 = pmod.make("scoremax", random.Random(9)); _sv2.set_pid(2)
_o2 = _trap_obs(ll=3)
_a = _sv2.decide(_o2)
check("low on Life: back out", _a.kind == "enc_choice" and _a.ci == _sv2.run.escape_choice()
      and _sv2.stats["trap_backouts"] == 1)
_sv2.run.on_result({"out": 1})
check("backing out lands on the escape node", _sv2.run.at_escape)
_a = _sv2.decide(_o2)
check("out of the escape node: enc_bank with nothing (it stays armed)",
      _a.kind == "enc_bank" and not _sv2.run.banked)
_cw = pmod.make("coward", random.Random(9)); _cw.set_pid(2)
_a = _cw.decide(_trap_obs(ll=8, sk=(4, 4, 4, 4, 4)))
check("the coward never works a trap",
      _a.kind == "enc_choice" and _a.ci == _cw.run.escape_choice())
for _name in (n for n in pmod.REGISTRY if n != "drunk"):
    _pol = pmod.make(_name, random.Random(5)); _pol.set_pid(2)
    _k = set()
    for _ in range(6):
        _act = _pol.decide(_trap_obs())
        _k.add(_act.kind)
        if _act.to_msg() is not None:
            json.dumps(_act.to_msg())
    check(f"{_name} in a trap never walks off or aborts",
          _k <= {"enc_choice", "enc_bank"})

from causes import DamageLedger as _DL
_trows = [_ev(1.0, {"k": "enc_res", "pid": 1, "out": 0, "penLL": -2, "trap": 1}),
          _ev(2.0, {"k": "enc_res", "pid": 1, "out": 0, "penLL": -1, "trap": 0})]
_tl = _DL(_trows).loss_ledger()
check("trap hazards are their own cause",
      _tl.get("booby trap") == 2 and _tl.get("encounter hazard") == 1)

from metrics import RunReport as _RR
_mrows = [
    {"ts": 0.0, "ch": "run", "arch": -1, "d": {"meta": 1}},
    _ev(1.0, {"k": "enc_start", "pid": 1, "trap": 1}, arch=0),
    _ev(1.0, {"k": "enc_start", "pid": 1, "trap": 1}, arch=1),
    _ev(2.0, {"k": "enc_res", "pid": 1, "out": 0, "penLL": -2, "tot": 5, "dn": 8, "trap": 1}),
    _ev(3.0, {"k": "enc_start", "pid": 3, "trap": 1}),
    _ev(4.0, {"k": "trap", "pid": 3, "q": 1, "r": 1, "dp": 0, "out": "known"}),
    _ev(5.0, {"k": "enc_start", "pid": 3}),
]
with tempfile.TemporaryDirectory() as _td_:
    _p = Path(_td_) / "trap.jsonl"
    _p.write_text("\n".join(json.dumps(r) for r in _mrows) + "\n", encoding="utf-8")
    try:
        _tr = _RR(_p).traps()
    except Exception as _e:     # the reader's row format may differ; say so
        _tr = {"error": repr(_e)}
check("metrics counts trap scenes, once each, not POIs", _tr.get("opened") == 2)
check("metrics reads what the traps took",
      (_tr.get("sprung"), _tr.get("escaped"), _tr.get("ll_lost")) == (1, 1, 2))

print(f"\n{ok} checks passed")
