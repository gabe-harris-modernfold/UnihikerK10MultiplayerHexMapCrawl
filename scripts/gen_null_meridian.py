# Generates the Null Meridian Group encounter files (docs/null-meridian-group.md).
#   python scripts/gen_null_meridian.py data/encounters
# Re-running OVERWRITES the six files -- edit here, not in the JSON.
#   perambulator  -> data/encounters/{scrub/21,glass/3,ridge/3}.json  (biome pools, live now)
#   null camp     -> data/encounters/meridian/null_camp/{1,2,3}.json  (waits for the Meridian load path)
# Skills: 0 Navigate, 1 Forage, 2 Scavenge, 3 Shelter, 4 Endure.
# Resources (loot "res"): 0 water, 1 food, 2 fuel, 3 med, 4 scrap.
import json, os, sys

ROOT = sys.argv[1]
FORKS, PENDULUM, SCOPE, GLOVE = 66, 67, 68, 69
NAV, FORAGE, SCAV, SHELTER, ENDURE = 0, 1, 2, 3, 4


def ch(label, skill, risk, to, haz=None, cost=None, req=None, lens=None):
    c = {"label": label, "cost": cost or {}, "base_risk": risk, "skill": skill, "success_node": to}
    if haz: c["hazard_id"] = haz
    if req: c["requires_item"] = req
    if lens: c["lens"] = lens
    return c


def res(r, lo, hi=None): return {"res": r, "qty": [lo, hi if hi is not None else lo]}
def item(i): return {"item": i, "qty": [1, 1]}


def node(text, choices, bank=True, loot=None, table=None, by_lens=None):
    n = {"text": text}
    if by_lens: n["text_by_lens"] = by_lens
    n["can_bank"] = bank
    if loot: n["loot"] = loot
    if table: n["loot_table"] = table
    n["choices"] = choices
    return n


def haz(text, pen, ends=False, wound=None):
    h = {"text": text, "penalty": pen}
    if wound: h["wound"] = wound
    h["ends_encounter"] = ends
    return h


def write(rel, obj):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    body = json.dumps(obj, indent=2, ensure_ascii=False) + "\n"
    assert len(body.encode("utf-8")) < 16384, (rel, len(body))   # ENC_FILE_CAP
    for k in obj["nodes"]: assert len(k) < 24, k                   # ENC_KEY_LEN
    for k in obj["hazards"]: assert len(k) < 24, k
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    print(f"{rel}: {len(body)} B, {len(obj['nodes'])} nodes")


def check(obj):
    """Every choice lands on a real node and names a real hazard; every
    non-terminal node has at least one choice anyone can take, or a survivor
    without the instruments would be stuck on it -- and that choice comes
    FIRST, so anything that still sends ci 0 blind never hits a refusal."""
    nodes, hazards = obj["nodes"], obj["hazards"]
    assert obj["start_node"] in nodes
    for key, n in nodes.items():
        if n["choices"]:
            assert "requires_item" not in n["choices"][0], f"{key}: first choice is gated"
        for c in n["choices"]:
            assert c["success_node"] in nodes, (key, c["success_node"])
            assert c.get("hazard_id") is None or c["hazard_id"] in hazards, (key, c.get("hazard_id"))
        items = [e for e in n.get("loot", []) if "item" in e]
        assert len(items) <= 2, key                                   # encResolveChoice keeps two
    reach, todo = set(), [obj["start_node"]]
    while todo:
        k = todo.pop()
        if k in reach: continue
        reach.add(k)
        todo += [c["success_node"] for c in nodes[k]["choices"]]
    assert reach == set(nodes), set(nodes) - reach


# ── The Perambulator ──────────────────────────────────────────────────────────
# One scene, stopped in three pools. Each sighting is its own file so each is
# placed once per map (hex-map.hpp Phase 5): three chances a map to meet it.
SIGHTINGS = {
    "scrub/21.json": ("scrub", "the scrub", "The brush around it has grown in a perfect ring, every stem leaning away."),
    "glass/3.json":  ("glass", "the fused glass", "Lichtenberg burns branch across the glass under its wheels. The branches match the map."),
    "ridge/3.json":  ("ridge", "the ridge crest", "The wind on the crest has stopped. It has stopped in one place only, the place you are standing."),
}


def perambulator(biome, ground, oddity):
    nodes = {
        "arrival": node(
            f"A flatbed truck stands on {ground} with no driver's seat and no tracks leading in. "
            "The engine is cold. The dust it raised is still settling, and has been, you think, for some time. "
            "A tuning fork is planted in the dirt by the rear wheel, humming a note just under hearing. "
            f"{oddity}",
            [
                ch("Climb up onto the flatbed.", NAV, 35, "flatbed", "lagging_shadow"),
                ch("Pull the planted fork out of the ground.", ENDURE, 55, "fork_pulled", "fork_answers"),
                ch("Strike your forks against the planted one and listen for which tine agrees.",
                   ENDURE, 15, "in_tune", "wrong_tine", req=FORKS, lens="resonance"),
                ch("Look at the driver's seat that isn't there through the Inward Scope.",
                   NAV, 25, "remembers", "eyepiece_blink", req=SCOPE, lens="machine"),
            ], bank=False),
        "fork_pulled": node(
            "It comes out easily, as if it had been waiting for you to ask. It is not one fork but five, bound in wire, "
            "and none of them match: three tines, a tine bent back into the handle, one with no tines at all that still rings. "
            "The hex rights itself. Your shadow catches up with you.",
            [ch("Climb onto the flatbed while it is quiet.", NAV, 25, "flatbed", "lagging_shadow")],
            loot=[item(FORKS)]),
        "in_tune": node(
            "The fork with no tines agrees with theirs. For one breath the hex is exactly where the map says it is, "
            "and the flatbed is only a truck. Someone has left a ration tin on the running board, "
            "still warm, with a spoon in it that is yours.",
            [ch("Climb onto the flatbed while the note holds.", NAV, 10, "flatbed", "lagging_shadow")],
            loot=[res(1, 1, 2), res(4, 1)]),
        "remembers": node(
            "Through the backward eyepiece the cab is full. Eleven people in lab coats, standing, facing a window "
            "that is not there, all looking at something very bright. One of them turns to look at you. "
            "You lower the scope. The seat is still missing, but the footwell is full of dropped supplies.",
            [ch("Climb onto the flatbed. They are still looking.", NAV, 20, "flatbed", "lagging_shadow")],
            loot=[res(3, 1), res(2, 1)]),
        "flatbed": node(
            "The bed is laid out like a lab bench: clamps, a cold soldering iron, eleven clipboards on eleven hooks. "
            "A twelfth hook has your name on it in handwriting you almost recognise. Three cases are strapped down. "
            "A radio bolted to the rail is quietly saying what you are about to say.",
            [
                ch("Unstrap the tall case. Something inside is swinging.", SCAV, 45, "pendulum_case", "rod_rises"),
                ch("Open the cold box with the brass eyepiece on the lid.", SCAV, 55, "scope_case", "eyepiece_blink"),
                ch("Reach into the glove box. There is no cab for it to be in.", ENDURE, 60, "glove_box", "glove_box_bites"),
                ch("Hold the Upside Pendulum over the cases and follow where it leans.",
                   NAV, 15, "leaning", "rod_rises", req=PENDULUM, lens="machine"),
                ch("Palpate the flatbed's tyre with the Dirty Glove.",
                   ENDURE, 20, "tyre_pulse", "glove_chooses", req=GLOVE, lens="ritual"),
            ],
            loot=[res(4, 1, 2)]),
        "pendulum_case": node(
            "A brass bob on a rigid rod, fixed to a base. It does not hang. It stands, and swings upward, "
            "toward whatever does not belong here. Right now it is pointing at you. Then, slowly, away.",
            [ch("Take the clipboard with your name on it.", NAV, 50, "twelfth_hook", "static_grammar")],
            loot=[item(PENDULUM)]),
        "scope_case": node(
            "A microscope built the wrong way round, eyepiece facing into the body. You look in and see yourself, "
            "very small, from below, lit. Something on the slide is looking at the observer, and counting.",
            [ch("Take the clipboard with your name on it.", NAV, 50, "twelfth_hook", "static_grammar")],
            loot=[item(SCOPE)]),
        "glove_box": node(
            "Your hand closes on latex. One left-hand surgical glove, never sterile, stained in a pattern you do not "
            "want to read. It is warm inside, as if someone just took it off. It fits.",
            [ch("Take the clipboard with your name on it.", NAV, 50, "twelfth_hook", "static_grammar")],
            loot=[item(GLOVE)]),
        "leaning": node(
            "The bob leans past the cases to a panel in the bed you would never have found. Under it, packed in straw, "
            "is what the group kept for themselves.",
            [ch("Take the clipboard with your name on it.", NAV, 40, "twelfth_hook", "static_grammar")],
            loot=[res(3, 1, 2), res(0, 1, 2)], table=f"{biome}_rare"),
        "tyre_pulse": node(
            "Through the glove the tyre has a pulse, slower than yours and very patient. The truck is alive the way a "
            "tuning fork is alive. It lets you take the fuel can lashed behind the wheel. The glove is darker than it was.",
            [ch("Take the clipboard with your name on it.", NAV, 40, "twelfth_hook", "static_grammar")],
            loot=[res(2, 1, 2), res(4, 1)]),
        "twelfth_hook": node(
            "The roster on it lists eleven staff of the resonator tuning group, and a twelfth line, blank, "
            "already initialled. The last entry in the log is tomorrow's date. When you look up, "
            "the Perambulator has not moved, but it has left.",
            [],
            loot=[res(3, 1), res(4, 1, 2)]),
    }
    hazards = {
        "lagging_shadow": haz("Your shadow is a step behind you. Then two. Then it stays where it is while you go on without it. "
                              "It catches up eventually, cold.", {"radiation": 1}),
        "fork_answers": haz("The fork comes free and the hex answers back: a tone in your fillings, your ears, "
                            "the long bones of your arms. It goes back into the ground on its own.",
                            {"ll": -1, "radiation": 1}),
        "wrong_tine": haz("The wrong tine. The hex answers back, and everything in it is suddenly half a step to the left of itself.",
                          {"radiation": 1}),
        "eyepiece_blink": haz("Something on the other side of the eyepiece blinks first. You lose a minute. "
                              "Your scrap has been recounted and comes out different.", {"scrap": -1}),
        "rod_rises": haz("The rod rises out of its case toward you and taps you, once, over the heart. "
                         "You are, apparently, the thing that does not belong.", {"radiation": 1}),
        "glove_box_bites": haz("The glove box shuts on your wrist. Not hard. Long enough for you to understand it could.",
                               {"ll": -1}, wound=[1, 0]),
        "glove_chooses": haz("The glove chooses where to press, and it chooses wrong. The tyre flinches. So do you.",
                             {"radiation": 1}, wound=[1, 0]),
        "static_grammar": haz("The radio on the rail stops saying what you will say and starts saying what you are thinking. "
                              "The static has grammar. You get down off the truck.", {"radiation": 1}, ends=True),
    }
    return {
        "id": f"null_perambulator_{biome}",
        "title": "The Perambulator",
        "start_node": "arrival",
        "nodes": nodes,
        "hazards": hazards,
    }


# ── The Null Camp ─────────────────────────────────────────────────────────────
# Three stages (the Meridian spec's quest shape). Failure is a setback: every
# check that can fail either ends the attempt (the stage has to be tried
# again) or costs Rad, and none of them costs more than 1 LL.

def meta(stage, hint):
    return {"site": "null_camp", "stage": stage, "of": 3, "hint": hint}


def null_camp_1():
    nodes = {
        "perimeter": node(
            "A ring of canvas tents stands around a rectangle of broken glass lying flat in the dirt: the floor of an observation "
            "gallery, with no gallery around it. Eleven cots. A kettle on a cold stove is warm. No one is here, and every tent flap "
            "is tied back, as if for someone expected.",
            [
                ch("Log the instruments staked round the perimeter before you cross it.", NAV, 55, "camp_center", "log_disagrees", lens="machine"),
                ch("Walk the ring of tents once, in the footprints already worn into it.", ENDURE, 55, "camp_center", "ring_tightens", lens="ritual"),
                ch("Cross where the hum is loudest. That is where they would have stood.", NAV, 65, "camp_center", "hum_takes_hold", lens="resonance"),
                ch("Go in through the cut in the fence. It was cut from the inside.", SCAV, 60, "camp_center", "fence_remembers", lens="prison"),
            ], bank=False,
            by_lens={
                "machine": "A field station, abandoned mid-shift: eleven cots, instrument stakes at even intervals, a kettle still warm. Someone should be logging this.",
                "ritual": "A camp laid out as a circle, tents facing inward to the glass. The dirt between them is worn into a path by bare feet.",
                "resonance": "The tents stand on the nodes of a standing wave. You can feel where the glass floor hums and where it doesn't.",
                "prison": "A cordon. Every tent faces the glass, every stake points at it. They were not studying it. They were watching it.",
                "mirror": "Eleven cots. Twelve. Eleven. The kettle was warm before you got here, and it is warm because you got here.",
            }),
        "camp_center": node(
            "You stand on the glass floor. Under it, very far down, there is a room where eleven people are looking up at you. "
            "The tents around you hold what they left: a survey tent, a sleeping tent, and a tent with its flap sewn shut.",
            [
                ch("Search the survey tent.", SCAV, 50, "survey_tent", "scrap_recounts"),
                ch("Search the sleeping tent.", SCAV, 45, "sleeping_tent", "radio_early"),
                ch("Palpate the sewn-shut flap with the Dirty Glove before you cut it.", ENDURE, 30, "sewn_tent", "glove_chooses",
                   req=GLOVE, lens="ritual"),
            ],
            loot=[res(0, 1, 2)]),
        "survey_tent": node(
            "Folding tables, a theodolite aimed straight down, and a roll of five tuning forks that do not match. "
            "They hum when you pick them up. They go on humming after you put them in your pack.",
            [ch("Look in the tent with its flap sewn shut.", SCAV, 60, "sewn_tent", "grave_pile")],
            loot=[item(FORKS), res(4, 1, 2)]),
        "sleeping_tent": node(
            "Eleven sleeping bags, zipped, empty, each shaped around a body that is not there. At the foot of the twelfth "
            "stands a pendulum on a rigid rod, rising from the floor, leaning toward the survey tent.",
            [ch("Look in the tent with its flap sewn shut.", SCAV, 60, "sewn_tent", "grave_pile")],
            loot=[item(PENDULUM), res(1, 1)]),
        "sewn_tent": node(
            "Inside is a grave pile: stones, a boot, a water bottle. It is yours. It is still warm. Someone has scratched a "
            "note into the bottle: the roster is in the gallery, and you are already on it.",
            [],
            loot=[res(3, 1, 2), res(0, 1)],
            by_lens={
                "machine": "A cairn over a supply cache, marked with your survivor number. A clerical error, surely.",
                "ritual": "A grave dressed with care: stones, a boot, a bottle. Someone mourned you before you arrived.",
                "resonance": "A grave pile, and the stones in it are arranged in the figure the nodes make on the map.",
                "prison": "A grave pile, sealed from the inside. Whatever was put down here tried to get out through the bottle.",
                "mirror": "It is your grave pile. It is warm. You check your pulse, and it is slower than it should be.",
            }),
    }
    hazards = {
        "log_disagrees": haz("Every instrument reads the same number, and the number is your name. You lose your nerve and the light.",
                             {"radiation": 1}, ends=True),
        "ring_tightens": haz("The ring of footprints is smaller on the second half of the circuit. You come out where you went in.",
                             {"food": -1}, ends=True),
        "hum_takes_hold": haz("The hum gets into your teeth and holds. When it lets go you are back at the perimeter, "
                              "and the sun has moved.", {"radiation": 1}, ends=True),
        "fence_remembers": haz("The cut wire closes behind you, then in front of you. You back out the way you came.",
                               {"ll": -1}, ends=True),
        "scrap_recounts": haz("You count the scrap on the table three times and get three numbers. You leave with the smallest.",
                              {"scrap": -1}),
        "radio_early": haz("A radio in the bedding says what you will say next, a second before you say it. You stop talking. "
                           "You stop searching.", {"radiation": 1}),
        "glove_chooses": haz("The glove chooses where to press, and it chooses wrong. Something on the other side of the canvas presses back.",
                             {"radiation": 1}, wound=[1, 0]),
        "grave_pile": haz("The stitches come apart on their own before you touch them. You do not go in. Not this time.",
                          {"radiation": 1}, ends=True),
    }
    return {"id": "null_camp_1_arrival", "title": "The Null Camp",
            "meridian": meta(1, "Reach the camp, and find out whose grave is in the sewn tent."),
            "start_node": "perimeter", "nodes": nodes, "hazards": hazards}


def null_camp_2():
    nodes = {
        "gallery": node(
            "At the edge of the glass floor, a lectern with a clipboard chained to it. The roster of the resonator tuning group: "
            "eleven names in the same hand, Dr. Varga-Okoro at the top. You count them. Twelve.",
            [
                ch("Count them again, slowly, with your finger on each line.", ENDURE, 55, "counted", "twelfth_line", lens="machine"),
                ch("Read the names aloud, the way a roll is called.", ENDURE, 60, "called", "answered", lens="ritual"),
                ch("Read the roster through the Inward Scope.", NAV, 30, "notes_on_you", "eyepiece_blink", req=SCOPE, lens="prison"),
                ch("Lay the Dirty Glove flat on the page and feel for a pulse.", ENDURE, 30, "pulse_on_page", "glove_chooses",
                   req=GLOVE, lens="resonance"),
            ], bank=False,
            by_lens={
                "machine": "A shift roster for the resonator tuning group, observation gallery, day of the test. Eleven staff. The count keeps coming out as twelve, which is an error somewhere.",
                "ritual": "A roll of eleven names, kept like a litany. There is room at the bottom for one more, and the space has been worn smooth by a thumb.",
                "resonance": "Eleven names spaced like the nodes of a wave. The gap after the last one is exactly one name wide.",
                "prison": "A sign-in sheet for a door that should stay shut. Eleven went in. The twelfth line is for whoever lets them out.",
                "mirror": "The roster lists eleven names. The twelfth is whoever is reading it, and it is in your handwriting.",
            }),
        "counted": node(
            "Eleven. You were sure. Then a twelfth line under Varga-Okoro's, blank, initialled in your initials. "
            "Beside it someone has drawn a tuning fork with no tines.",
            [ch("Look under the lectern.", SCAV, 45, "lectern_shelf", "scrap_recounts")],
            loot=[res(4, 1)]),
        "called": node(
            "Eleven names, and on each one the static in the gallery answers \"here\". On the twelfth it says nothing, "
            "and then it says your name, and \"here\" in your voice.",
            [ch("Look under the lectern.", SCAV, 45, "lectern_shelf", "scrap_recounts")],
            loot=[res(3, 1)]),
        "notes_on_you": node(
            "Through the backward eyepiece the roster is not a roster. It is a page of field notes, and the subject is you: "
            "the way you walk, what you carry, which fork you will choose. The last line is underlined twice.",
            [ch("Look under the lectern.", SCAV, 35, "lectern_shelf", "scrap_recounts")],
            loot=[res(3, 1), res(4, 1)]),
        "pulse_on_page": node(
            "The paper has a pulse, slower than yours. It quickens under the twelfth line. The glove comes away with a new stain "
            "the shape of a name.",
            [ch("Look under the lectern.", SCAV, 35, "lectern_shelf", "scrap_recounts")],
            loot=[res(3, 1), res(2, 1)]),
        "lectern_shelf": node(
            "On the shelf under the lectern: a microscope built the wrong way round, and a single left-hand surgical glove, "
            "never sterile. A note in Varga-Okoro's hand: whoever finishes the measurement will need both.",
            [],
            loot=[item(SCOPE), item(GLOVE)]),
    }
    hazards = {
        "twelfth_line": haz("You count twelve. You count eleven. You count twelve, and the twelfth is standing behind you. "
                            "You do not turn around. You leave.", {"radiation": 1}, ends=True),
        "answered": haz("Every name answers, including one you did not read. You stop. The gallery is full, and then it is empty.",
                        {"radiation": 1}, ends=True),
        "eyepiece_blink": haz("Something on the other side of the eyepiece blinks first. You lose a minute, and your place in the notes.",
                              {"radiation": 1}),
        "glove_chooses": haz("The glove chooses where to press, and it chooses the twelfth line. The page goes cold under it.",
                             {"radiation": 1}, wound=[1, 0]),
        "scrap_recounts": haz("There is a box of scrap under the lectern. You count it and get a different number each time, "
                              "each smaller than the last.", {"scrap": -1}),
    }
    return {"id": "null_camp_2_roster", "title": "The Roster",
            "meridian": meta(2, "Read the roster in the gallery. Count the names."),
            "start_node": "gallery", "nodes": nodes, "hazards": hazards}


def null_camp_3():
    # The forks cycle lens at the moment of use: one fork choice per lens,
    # each gated on the one item. The Pendulum, Scope and Glove each settle
    # the node first (a lower-risk way to the same four tines). A wrong
    # sounding ends the attempt: the stage is lost, not the survivor.
    def forks(risk):
        return [
            ch("Sound the three-tined fork: a clean reference tone to measure against.", ENDURE, risk, "sounded", "node_answers",
               req=FORKS, lens="machine"),
            ch("Sound the fork bent back into its own handle, and walk it round the node three times.", ENDURE, risk, "sounded",
               "node_answers", req=FORKS, lens="ritual"),
            ch("Sound the fork with no tines. It still rings, and the node rings with it.", ENDURE, risk, "sounded", "node_answers",
               req=FORKS, lens="resonance"),
            ch("Sound the heaviest fork, hard and short, to damp the node rather than wake it.", ENDURE, risk, "sounded",
               "node_answers", req=FORKS, lens="prison"),
        ]

    hum = ch("Hum the note yourself. You have heard it enough times now.", ENDURE, 85, "sounded", "node_answers")
    nodes = {
        "the_node": node(
            "Under the glass floor, at the exact centre of the camp, the node: a tuning fork the height of a person, planted "
            "tines-down in the earth, silent. Eleven chalk marks surround it where eleven people stood. "
            "There is room for a twelfth.",
            [hum] + forks(75) + [
                ch("Let the Upside Pendulum find the node's true centre first.", NAV, 35, "centred", "rod_rises",
                   req=PENDULUM, lens="machine"),
                ch("Look at the node through the Inward Scope.", NAV, 35, "centred", "eyepiece_blink", req=SCOPE, lens="prison"),
                ch("Palpate the node with the Dirty Glove.", ENDURE, 35, "centred", "glove_chooses", req=GLOVE, lens="ritual"),
            ], bank=False,
            by_lens={
                "machine": "The node: a resonator element the height of a person, seated tines-down, out of tune. Eleven chalk positions mark the calibration crew. One position is unassigned.",
                "ritual": "The node: a great fork planted in the earth like a standing stone, with eleven stations chalked around it and a twelfth left bare.",
                "resonance": "The node: a fork so large its note is below hearing. The camp, the ring, the eleven marks are its overtones. The twelfth is missing.",
                "prison": "The node: a bolt driven into the ground, tines-down, holding something under the glass. Eleven guards. One post empty.",
                "mirror": "The node. You have stood here before. You are standing here now, on the twelfth mark, which was always yours.",
            }),
        "centred": node(
            "The instrument settles the node. For a moment you can see where each of the eleven stood and which fork they held. "
            "The note is waiting.",
            [hum] + forks(55),
            bank=False),
        "sounded": node(
            "The node takes the note and holds it. Under the glass, eleven people lower their eyes at last. The measurement "
            "that could not be finished is finished, or it is started again, and you cannot tell which. The camp's supplies are "
            "yours. The Perambulator is parked at the edge of the glass, facing you.",
            [],
            loot=[res(3, 2, 3), res(2, 1, 2), res(0, 1, 2)],
            by_lens={
                "machine": "Resonance locked. The element holds its frequency. Calibration complete, twelve of twelve. The camp stores are released to the crew.",
                "ritual": "The circuit is walked and the note is sung. The eleven are at rest. What they kept is given to the one who finished.",
                "resonance": "The node and the fork agree, and the whole camp agrees with them. You can feel the next node, very far off, answer.",
                "prison": "The lock takes the note and closes. Whatever was pressing up against the glass settles back down. For now.",
                "mirror": "It worked, because you believed it would. The eleven look up at you, and each of them has your face.",
            }),
    }
    hazards = {
        "node_answers": haz("Wrong. The node answers back. The hex lurches half a step out of true, the chalk marks rub themselves "
                            "out, and you are standing at the edge of the camp again. The measurement has to be started over.",
                            {"radiation": 1}, ends=True),
        "rod_rises": haz("The pendulum's rod rises and points at you, not the node. You are the thing that does not belong.",
                         {"radiation": 1}),
        "eyepiece_blink": haz("Through the scope the node is looking at you, and it blinks first.", {"radiation": 1}),
        "glove_chooses": haz("The glove chooses where to press, and it chooses wrong. The node's pulse stops. So, for a moment, does yours.",
                             {"ll": -1}, wound=[1, 0]),
    }
    return {"id": "null_camp_3_node", "title": "The Null Node",
            "meridian": meta(3, "Choose which instrument to sound at the node."),
            "start_node": "the_node", "nodes": nodes, "hazards": hazards}


if __name__ == "__main__":
    base = os.path.join(ROOT)
    for rel, (biome, ground, oddity) in SIGHTINGS.items():
        e = perambulator(biome, ground, oddity); check(e); write(rel, e)
    for i, fn in enumerate((null_camp_1, null_camp_2, null_camp_3), 1):
        e = fn(); check(e); write(f"meridian/null_camp/{i}.json", e)
