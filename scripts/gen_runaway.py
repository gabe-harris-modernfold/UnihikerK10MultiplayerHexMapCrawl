# Generates RUNAWAY SCIENCE, the clinic proximity quest (docs/runaway-science.md).
# The clinic is only ever underground: three rooms off the bunker tunnels.
#   python scripts/gen_runaway.py data/encounters
# Re-running OVERWRITES data/encounters/lab/{1..4}.json -- edit here, not in
# the JSON. The file number is the part (lab.hpp LAB_PART_*):
#   1  The Sharps Ward  the needle room; pays the Needlecap (72)
#   2  The Pantry       the jar room; pays the Brine Heart (73)
#   3  The Patient      the theatre, Doctor Gaunt; pays the Spine Wires (74)
#   4  The Graft        the theatre again, for a survivor carrying all three;
#                       "lab_ending": "graft" spends them and grants the
#                       Runaway Heart (75) and the Cursed Note (76)
# Parts 1-3 count once their item is BANKED (labOnBank), not when the last
# node is reached.
#
# The twisted economy: every obstacle has a body path (a roll at base_risk
# 55-70, so DN 9-10: hard on purpose, sometimes an LL or rad price up front
# too) and a points path -- "cost": {"score": N} with
# "sure": true, which never rolls. Points are the score; nothing else in the
# game spends it. The Graft's fee has no body path at all.
#
# Skills: 0 Navigate, 1 Forage, 2 Scavenge, 3 Shelter, 4 Endure.
# Text: straight single quotes for speech, no double quotes, no backslashes,
# no newlines.
import json, os, sys

ROOT = sys.argv[1]
NAV, FORAGE, SCAV, SHELTER, ENDURE = 0, 1, 2, 3, 4
NEEDLECAP, BRINE_HEART, SPINE_WIRES = 72, 73, 74   # data/items.cfg
FEE = 100                                          # the Graft's fee, in points


def ch(label, skill, risk, to, haz=None, needs=None, cost=None):
    c = {"label": label, "cost": cost or {}, "base_risk": risk, "skill": skill, "success_node": to}
    if needs: c["requires_item"] = needs
    if haz: c["hazard_id"] = haz
    return c


def pay(label, pts, to, needs=None):
    """A points path: costs score, never rolls."""
    c = ch(label, NAV, 0, to, needs=needs, cost={"score": pts})
    c["sure"] = True
    return c


def leave(label):
    """A way out that never rolls and costs nothing."""
    c = ch(label, NAV, 0, "walk_away")
    c["sure"] = True
    return c


def item(i): return {"item": i, "qty": [1, 1]}


def node(text, choices, bank=False, loot=None, escape=False, ending=None):
    n = {"text": text}
    if escape: n["escape"] = True
    if ending: n["lab_ending"] = ending
    n["can_bank"] = bank
    if loot: n["loot"] = loot
    n["choices"] = choices
    return n


def haz(text, pen, ends=False, wound=None):
    h = {"text": text, "penalty": pen}
    if wound: h["wound"] = wound
    h["ends_encounter"] = ends
    return h


def enc(part, slug, title, start, nodes, hazards):
    return {"id": f"lab_{part}_{slug}", "title": title, "lab": {"part": part},
            "start_node": start, "nodes": nodes, "hazards": hazards}


def check(obj):
    """Every choice lands on a real node and names a real hazard, every node is
    reachable, the text is safe for the firmware's JSON reader, the gates
    follow the Null Meridian rule (an ungated choice on every node, never a
    gated one first), and a sure choice never names a hazard (it cannot fail)."""
    nodes, hazards = obj["nodes"], obj["hazards"]
    assert obj["start_node"] in nodes
    for key, n in nodes.items():
        for c in n["choices"]:
            assert c["success_node"] in nodes, (key, c["success_node"])
            assert c.get("hazard_id") is None or c["hazard_id"] in hazards, (key, c.get("hazard_id"))
            assert not (c.get("sure") and c.get("hazard_id")), (key, c["label"], "a sure choice with a hazard")
            assert set(c["cost"]) <= {"ll", "radiation", "food", "water", "scrap", "med", "score"}, c["cost"]
            assert set(c["cost"]) <= {"score"} or not c.get("sure"), (key, "a sure choice paid in anything but points")
        if n["choices"]:
            assert any("requires_item" not in c for c in n["choices"]), (key, "no ungated choice")
            assert "requires_item" not in n["choices"][0], (key, "a gated choice goes first")
    for t in [n["text"] for n in nodes.values()] + [h["text"] for h in hazards.values()] + \
             [c["label"] for n in nodes.values() for c in n["choices"]]:
        assert '"' not in t and "\\" not in t and "\n" not in t, t[:60]
    reach, todo = set(), [obj["start_node"]]
    while todo:
        k = todo.pop()
        if k in reach: continue
        reach.add(k)
        todo += [c["success_node"] for c in nodes[k]["choices"]]
    assert reach == set(nodes), set(nodes) - reach
    for k in list(nodes) + list(hazards): assert len(k) < 24, k   # ENC_KEY_LEN
    assert len((json.dumps(obj, indent=2, ensure_ascii=False) + "\n").encode("utf-8")) < 16384, "over ENC_FILE_CAP"


def write(rel, obj):
    check(obj)
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("wrote", path)


# ── 1  The Sharps Ward ──────────────────────────────────────────────────────
def part_1():
    nodes = {
        "the_ward": node(
            "The corridor ends at a steel door, and stencilled on the door, still legible, is GAUNT INSTITUTE FOR CONTINUANCE "
            "- SHARPS. Under it, scratched in since by somebody with a nail: RUNAWAY SCIENCE. Behind the door is a ward thirty "
            "paces long, cut into the rock, and every inch of it bristles. Walls, ceiling, floor, the frames of the beds: "
            "hypodermic needles, thousands of them, pushed in up to the barrel like pins in a cushion, points out. Every barrel "
            "holds an inch of something amber. From every point grows a hair of pale fungus with a pinhead cap the colour of a "
            "bruise, and when you breathe, the nearest ones lean toward you. A speaker in the ceiling clears its throat. 'Sharps "
            "Ward,' says a man's voice, pleased. 'Mind the floor. There is a clipboard by the door. I accept payment.'",
            [
                ch("Crawl the cleared path on your belly.", ENDURE, 60, "halfway", "pricked"),
                ch("Walk it upright, slowly, and touch nothing.", NAV, 60, "halfway", "pricked"),
                pay("Write the best thing you ever did on his clipboard. He keeps it.", 15, "halfway"),
            ]),
        "halfway": node(
            "Halfway down the ward the needles start to move. Not much: a few degrees, all of them together, the way a field of "
            "grass turns to the wind. They are turning to follow you. The amber in the barrels has gone cloudy. 'They are only "
            "curious,' says the speaker. 'They were filled from me, you see. Everything in here is a little bit me.' At the far "
            "wall, on a steel trolley, sits a pincushion of needles as big as a man's head, and out of it, like a crown, grows "
            "the thing he sent you for: a cluster of caps the size of thumbnails, bruise-purple, wet, and very slowly breathing.",
            [
                ch("Hold your breath and walk the last ten paces.", ENDURE, 65, "the_cap", "the_turn"),
                ch("Let them have a vein. Roll up your sleeve and walk the wall.", ENDURE, 40, "the_cap", "the_turn",
                   cost={"radiation": 2}),
                pay("Tell the speaker the longest walk you ever made. He keeps it.", 25, "the_cap"),
            ]),
        "the_cap": node(
            "You lift the pincushion off the trolley and the fungus comes with it, the needles under it trailing threads of pale "
            "root like a pulled tooth. It is warm. It pulses once against your palm, out of time with you, then corrects itself to "
            "your heartbeat and stays there. Every needle in the ward turns at once to point at you, and holds. 'Needlecap,' says "
            "the speaker, fondly. 'Wrap it in something. It bites when it is cold.'",
            [], bank=True, loot=[item(NEEDLECAP)]),
    }
    hazards = {
        "pricked": haz("You do not feel the first one. You feel the ninth. Something cold and amber goes into the meat of your "
                       "thigh and spreads, and for a moment you can taste a stranger's breakfast, sixty years old.",
                       {"ll": -1, "radiation": 1}),
        "the_turn": haz("The needles turn faster than you do. A whole panel of the wall leans in as you pass and kisses you all "
                        "down one side, and every place it touched has come up in a small purple bloom by the time you reach the "
                        "trolley.",
                        {"ll": -1}, wound=[1, 0]),
    }
    return enc(1, "sharps_ward", "The Sharps Ward", "the_ward", nodes, hazards)


# ── 2  The Pantry ───────────────────────────────────────────────────────────
def part_2():
    nodes = {
        "the_aisles": node(
            "Two flights down from the corridor, the Institute's stores are still stocked. Steel shelving goes up into the dark, aisle after aisle, and every shelf "
            "is lined with jars: big pickling jars sealed with wax and labelled in a neat, upright hand. A jar of hands, all "
            "left. A jar of eyes, which turn to watch you come in. A jar of tongues. A jar of teeth labelled TEETH, which seems "
            "unnecessary. One labelled HALVERS - WOUNDS - KEPT FOR LATER. The brine in all of them is the yellow of old varnish, "
            "and some of the labels are dated this year. The ceiling speaker crackles. 'My pantry,' says the voice. 'Everything "
            "I took out of people to see how it worked, and everything I took out of me. Third aisle, top shelf. You cannot miss "
            "it. It is the one that is still going.'",
            [
                ch("Climb the shelving to the top of the third aisle.", NAV, 60, "the_jar", "shelf_gives"),
                ch("Drag the rolling ladder down the aisle, past the eyes.", SCAV, 55, "the_jar", "the_eyes"),
                pay("Read him the labels you would want on your own jars. He keeps them.", 20, "the_jar"),
            ]),
        "the_jar": node(
            "Top shelf, third aisle: a jar the size of a bucket, and in it, in brine gone pink, a human heart. It is beating. "
            "Slowly, once to every four of yours, and each beat stirs the brine and lifts a little cloud of something off it. The "
            "label says G. - SPARE - 1966. The lid is waxed shut, with a thumbprint in the wax. On the shelf below stands a "
            "printed card, the kind a butcher puts in the window: HEART, ONE, 40 POINTS. 'It is not for sale, exactly,' says "
            "the speaker. 'It is for paying. You have done things. I have not done anything since 1966. I will take some off "
            "you.'",
            [
                ch("Break the wax and put your arm in up to the shoulder.", ENDURE, 60, "the_heart", "the_brine"),
                ch("Leave the shelf something of yours. A tooth will do.", ENDURE, 35, "the_heart", "the_brine",
                   cost={"ll": 1}),
                pay("Pay the card. Forty points. He keeps them.", 40, "the_heart"),
            ]),
        "the_heart": node(
            "The heart comes out of the brine with a sound like a boot out of mud. It goes on beating in your hand, slow and "
            "patient, and the cold of it climbs your arm and stops at the elbow, as though it has reached a door it was told not "
            "to open. All down the aisle the eyes have turned in their jar to watch it go. 'Careful,' says the speaker, very "
            "softly. 'That is mine.'",
            [], bank=True, loot=[item(BRINE_HEART)]),
    }
    hazards = {
        "shelf_gives": haz("The shelving takes your weight for exactly as long as it takes you to trust it. Then a whole run of "
                           "jars goes, and you come down in a wash of yellow brine and glass and somebody's hands, and the hands "
                           "are still trying to hold on to something.",
                           {"ll": -1}, wound=[1, 0]),
        "the_eyes": haz("Every eye in the jar follows you down the aisle. Halfway along you realise they are not looking at you. "
                        "They are looking behind you. You turn round and there is nothing there, and when you turn back you walk "
                        "into the corner of a shelf hard enough to open your scalp.",
                        {"ll": -1}),
        "the_brine": haz("The brine is cold the way a grave is cold, and it does not stop at your skin. Something in the jar takes "
                         "hold of your wrist, gently, the way a nurse does, and holds it while the cold goes in. You are sick on "
                         "the floor. You were always going to be.",
                         {"ll": -1, "food": -1}),
    }
    return enc(2, "pantry", "The Pantry", "the_aisles", nodes, hazards)


# ── 3  The Patient ──────────────────────────────────────────────────────────
def part_3():
    nodes = {
        "the_theatre": node(
            "The operating theatre is the only room down here with its glass still in: a long observation window, and behind "
            "it, under a lamp that has burned for sixty years, a man lies on a steel table under a sheet folded down to the waist. He is perfect. The skin is "
            "waxed and pale as a candle, the nails are clean, the hair is combed. Every inch of him has been preserved, pickled, "
            "sealed and stopped, all the way up to the collar of his gown. Above that, his head turns on the pillow and looks at "
            "you. 'Ah,' says Doctor Orrin Gaunt, with his own mouth this time. 'The help.' A loom of wires as thick as your "
            "wrist, braided copper in cloth, comes out of the back of his neck and runs across the floor into a cabinet that "
            "hums. The clock on the wall says 11:47.",
            [
                ch("Go straight to the humming cabinet.", SCAV, 60, "the_loom", "live_current"),
                pay("Ask him what happened to him. He charges by the question.", 10, "his_story"),
            ]),
        "his_story": node(
            "'I was dying, in 1966,' he says. 'Slowly, which is the worst way, because you have time to think about it. So I "
            "thought. Continuance: a body that does not stop. I went in from the feet up. Embalm a little, wire a little, seed "
            "the culture, and keep the lights on upstairs. The orderly was to do my head after lunch.' His eyes go to the clock. "
            "'He went to lunch at a quarter to twelve. I have been waiting for him for sixty years. Nobody stopped me, you see, "
            "and then nobody could. That is what they call runaway science.' He smiles. 'Now. The wires.'",
            [
                ch("Find the live wire yourself.", SCAV, 55, "the_loom", "live_current"),
                pay("Pay him to say which wire is live. He keeps it.", 20, "the_loom"),
            ]),
        "the_loom": node(
            "The cabinet is full of valves glowing orange, and every one of them ticks. The loom goes into its back through a "
            "brass collar. To get it out you will have to pull it out of him too: the other end goes into the base of his skull. "
            "'Pull,' he says. 'Pull hard and do not stop. It will not kill me. Nothing does.'",
            [
                ch("Brace a foot on his table and pull.", ENDURE, 65, "the_wires", "whiplash"),
                ch("Cut it at the collar with the shears off his tray.", SCAV, 55, "the_wires", "live_current"),
                pay("Give him the best night's sleep you ever had. He keeps it.", 20, "the_wires"),
            ]),
        "the_wires": node(
            "The loom comes out of him like a root out of wet ground: a yard of braided copper with a grey bulb at the end where "
            "it was in his neck. It is still warm, and it still carries something. When you hold it the hairs on your arm stand "
            "up and your fingers twitch in a pattern that is not yours. The valves in the cabinet go out one by one. Gaunt's head "
            "goes on talking, cheerfully, with nothing connected to it at all. 'Lovely. Now my mushrooms, and my heart. Bring "
            "them here and I will show you what they are for.'",
            [], bank=True, loot=[item(SPINE_WIRES)]),
    }
    hazards = {
        "live_current": haz("The cabinet finds you before you find the right wire. It goes in at the fingers and out at the "
                            "heels, and for a long white moment you can hear every valve in the cabinet ticking in your teeth. "
                            "'That one,' says Gaunt. 'I could have told you.'",
                            {"ll": -2}),
        "whiplash": haz("The loom comes a foot and stops, and Gaunt's head comes up off the pillow with it, mouth wide, "
                        "laughing or screaming. The wire whips back out of your hands and opens your palm to the bone.",
                        {"ll": -1}, wound=[1, 0]),
    }
    return enc(3, "patient", "The Patient", "the_theatre", nodes, hazards)


# ── 4  The Graft ────────────────────────────────────────────────────────────
def part_4():
    nodes = {
        "the_second_table": node(
            "There is a second table beside his now, steel, with straps, and the straps are open. You did not see it brought in. "
            "'You will have to do it yourself,' says Gaunt. 'I cannot, obviously. I will talk you through it. I have done this "
            "one before. On me.' On the tray: a scalpel, a hand drill, a funnel, a reel of catgut and a brown bottle of something "
            "amber. 'Lie down. Open yourself under the ribs, on the left. The Needlecap goes in first. It likes to get there "
            "before the heart, to make the bed.'",
            [
                leave("Not today. Step back from the table."),
                ch("Cut under the ribs and seed the Needlecap.", ENDURE, 55, "heart_in", "the_knife", needs=NEEDLECAP),
                pay("Pay him to guide the knife. He keeps it.", 30, "heart_in", needs=NEEDLECAP),
            ]),
        "heart_in": node(
            "The Needlecap goes into you the way a hand goes into a glove. Its roots find your blood at once, warm, busy, and the "
            "cut stops bleeding on its own. 'Good,' says Gaunt. 'Now my heart. In beside yours, not instead of. Two is the whole "
            "trick. One to live and one to keep going when the first one would rather not.' His spare heart beats in your hand, "
            "once to your four, waiting.",
            [
                leave("Stop. Sew yourself shut and get up."),
                ch("Set his heart in beside yours.", ENDURE, 65, "the_wiring", "rejection", needs=BRINE_HEART),
                pay("Pay him to hold it still with his voice. He keeps it.", 30, "the_wiring", needs=BRINE_HEART),
            ]),
        "the_wiring": node(
            "Two hearts. Yours is racing and his is not, and the fungus is threading between them like a seamstress. 'The wires "
            "last,' says Gaunt. 'Base of the skull. The drill is on the tray. Do not think about it, it only makes the hole "
            "crooked.' The loom twitches in your other hand like something that can smell where it is going.",
            [
                leave("Stop here. Get off the table while you still can."),
                ch("Drill the base of your skull and wire yourself in.", ENDURE, 70, "the_fee", "live_spine", needs=SPINE_WIRES),
                pay("Pay him to count you through it. He keeps it.", 30, "the_fee", needs=SPINE_WIRES),
            ]),
        "the_fee": node(
            "It is all in you. The heart sits beside yours, cold and still, waiting for a word. The wires hum in your spine with "
            "nothing to say yet. 'And now my fee,' says Gaunt. 'Nothing personal. I do not do anything for nothing any more, and "
            "I have not done anything at all for sixty years. I want a week. Your best one. Everything you did in it. Then I "
            "will say the word.' He does not want water, or scrap, or medicine. He has never wanted anything you can carry.",
            [
                leave("Get up with a cold heart in you, and walk away."),
                pay("Pay the fee: everything you did on your best week. He keeps it.", FEE, "runaway"),
            ]),
        "runaway": node(
            "He says the word. The heart catches. You feel it before you hear it: a second beat, slower than yours and a "
            "half-step behind, and then yours slows to meet it, and the two of them settle into a rhythm like two men carrying "
            "something heavy between them. The fungus warms through your chest. The wires in your spine begin to sing. You "
            "could walk to the end of the world. You sit up. On the other table, Doctor Gaunt's head lies still on the pillow, "
            "eyes open and dry, mouth closed. For the first time in sixty years he has stopped. In your pocket, where there was "
            "nothing, there is a folded note in the neat, upright hand of a thousand jar labels. You have not read it yet. It is "
            "already laughing.",
            [], bank=True, ending="graft"),
        "walk_away": node(
            "You get off the table. Everything you brought is still in your pack, wrapped and cold, and you are not sure how. "
            "'I will be here,' says Gaunt to the ceiling. 'Obviously.'",
            [], escape=True),
    }
    hazards = {
        "the_knife": haz("The scalpel is sharper than you thought, or you are softer. 'Deeper,' says Gaunt, interested. 'No. Too "
                         "deep. Never mind, it is only blood. You have lots.'",
                         {"ll": -1}, wound=[1, 0]),
        "rejection": haz("The heart will not sit. It fights you like a landed fish, the Needlecap's roots go the wrong way, and "
                         "for a long minute everything in your chest beats in a different time. 'Rejection,' says Gaunt, with "
                         "professional interest. 'Try again. Or die, I suppose. Either way I learn something.'",
                         {"ll": -2}),
        "live_spine": haz("The loom goes into your spine like a dog through a door. Every muscle you own pulls tight at once, and "
                          "you lie on the steel and listen to your own teeth. When it lets you go you are on the floor, the loom "
                          "is not in you, and Gaunt is humming.",
                          {"ll": -2}, ends=True, wound=[0, 1]),
    }
    return enc(4, "graft", "The Graft", "the_second_table", nodes, hazards)


for part, build in ((1, part_1), (2, part_2), (3, part_3), (4, part_4)):
    write(f"lab/{part}.json", build())
