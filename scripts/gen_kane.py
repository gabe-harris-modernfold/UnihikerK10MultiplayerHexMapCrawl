# Generates the Machine Gun Kane arc (docs/machine-gun-kane.md).
#   python scripts/gen_kane.py data/encounters
# Re-running OVERWRITES data/encounters/kane/{1..5}.json -- edit here, not in the JSON.
# The file number is the chapter. The pool is not in index.json yet: nothing
# loads these until the firmware's ENC_POOL_KANE exists.
#   1-3  his camps, left at dawn where he sat the night (camp marker, cell 3)
#   4    the house in the glass, inside the ring of his tracks
#   5    his fire, at night; the last node's "kane_ending" retires the jeep
# Skills: 0 Navigate, 1 Forage, 2 Scavenge, 3 Shelter, 4 Endure.
# Resources (loot "res"): 0 water, 1 food, 2 fuel, 3 med, 4 scrap.
# Text: straight single quotes for speech, no double quotes, no backslashes,
# no newlines. Tone from the song the user gave; never its words.
import json, os, sys

ROOT = sys.argv[1]
NAV, FORAGE, SCAV, SHELTER, ENDURE = 0, 1, 2, 3, 4
WATER, FOOD, FUEL, MED, SCRAP = 0, 1, 2, 3, 4


def ch(label, skill, risk, to, haz=None):
    c = {"label": label, "cost": {}, "base_risk": risk, "skill": skill, "success_node": to}
    if haz: c["hazard_id"] = haz
    return c


def res(r, lo, hi=None): return {"res": r, "qty": [lo, hi if hi is not None else lo]}


def node(text, choices, bank=True, loot=None, table=None, escape=False, ending=None):
    n = {"text": text}
    if escape: n["escape"] = True
    if ending: n["kane_ending"] = ending
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


def enc(chapter, slug, title, start, nodes, hazards):
    return {"id": f"kane_{chapter}_{slug}", "title": title, "kane": {"chapter": chapter},
            "start_node": start, "nodes": nodes, "hazards": hazards}


def check(obj):
    """Every choice lands on a real node and names a real hazard, every node is
    reachable, and the text is safe for the firmware's JSON reader."""
    nodes, hazards = obj["nodes"], obj["hazards"]
    assert obj["start_node"] in nodes
    for key, n in nodes.items():
        for c in n["choices"]:
            assert c["success_node"] in nodes, (key, c["success_node"])
            assert c.get("hazard_id") is None or c["hazard_id"] in hazards, (key, c.get("hazard_id"))
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
    ends = [n.get("kane_ending") for n in nodes.values() if n.get("kane_ending")]
    assert (obj["kane"]["chapter"] == 5) == bool(ends), obj["id"]


def write(rel, obj):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    body = json.dumps(obj, indent=2, ensure_ascii=False) + "\n"
    assert len(body.encode("utf-8")) < 16384, (rel, len(body))   # ENC_FILE_CAP
    for k in list(obj["nodes"]) + list(obj["hazards"]): assert len(k) < 24, k   # ENC_KEY_LEN
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    print(f"{rel}: {len(body)} B, {len(obj['nodes'])} nodes")


# ── 1. Have You Seen Her ─────────────────────────────────────────────────────
# What he does: he is looking for someone he cannot picture.
def chapter_1():
    nodes = {
        "the_camp": node(
            "Tyre tracks come in off the open ground and stop in a scuffed oval where something heavy sat idling all night. "
            "A ring of stones, the ash still warm under its crust. Around it, swept into a careful second ring, lie spent brass "
            "cases: hundreds of them, more than one night's worth, more than one year's. Someone carries his brass with him and "
            "lays it out wherever he stops. Against a rock leans a board, painted and repainted until the paint is thicker than "
            "the wood: HAVE YOU SEEN HER.",
            [
                ch("Read the board.", NAV, 20, "the_board", "engine_far"),
                ch("Sift the brass for anything that isn't empty.", SCAV, 35, "the_board", "cook_off"),
            ], bank=False),
        "the_board": node(
            "Nailed to the board is a photograph, or what passes for one: a page torn from a 1966 mail-order catalog. A woman "
            "in a belted dress smiles at nobody, one hand on her hip, a price printed under her chin. On the back, in a child's "
            "careful capitals: MAMA. Under that, in a shaking grown-up hand, much newer: IS THIS HER. Under that, again: IS THIS "
            "HER. It goes on down the page, smaller and smaller, until the pencil went through.",
            [
                ch("Open the ammo can by the fire.", SCAV, 30, "the_catalog", "can_hinge"),
                ch("Follow his tracks a little way out.", NAV, 25, "the_catalog", "figure_eight"),
            ], loot=[res(SCRAP, 1, 2)]),
        "the_catalog": node(
            "The ammo can holds no ammunition. It holds the rest of the catalog, every page, and every page has a hole in it "
            "the shape of a woman. Dresses with nobody in them. Winter coats with nobody in them. A whole spread of kitchen "
            "ranges with a neat empty outline standing at each stove. He cannot remember her face, so he is going through the "
            "book, holding them up one at a time against whatever is left of her. Scratched into the bottom of the can, over and "
            "over: NOT HER. NOT HER. NOT HER.",
            [], loot=[res(SCRAP, 1, 2)], table="scrub_common"),
    }
    hazards = {
        "engine_far": haz("Somewhere past the horizon an engine coughs, catches and comes on. You are flat in the ash before you "
                          "know you have moved. It fades. You get up with your heart going and your mouth dry.",
                          {"water": -1}),
        "cook_off": haz("One of them isn't empty. It is lying in the warm ash, and as your fingers find it the heat finds it too. "
                        "It goes off with a crack that rings for a long minute and takes a strip of skin off the back of your hand.",
                        {"ll": -1}, wound=[1, 0]),
        "can_hinge": haz("The lid is rusted half shut and the hinge bites when it gives. You bleed on a page of winter coats.",
                         {"ll": -1}, wound=[1, 0]),
        "figure_eight": haz("His tracks loop. Out, round and back, out again, a figure eight pressed a foot deep, as if he couldn't "
                            "decide which way she went. By the time you have walked one loop the light has moved, and so has the "
                            "engine, nearer. You go back to the fire the short way.",
                            {"water": -1}),
    }
    return enc(1, "have_you_seen_her", "Have You Seen Her", "the_camp", nodes, hazards)


# ── 2. The Tag ───────────────────────────────────────────────────────────────
# Where he came from: the one child who got off the Evacuation Special (forest/4).
def chapter_2():
    nodes = {
        "the_second_camp": node(
            "Another of his camps, another ring of brass, laid out with the same care. This time he left something on purpose: "
            "a jerrycan stood upright on a flat stone like a headstone, and folded on top of it a child's wool coat. It is too "
            "small for anyone. The elbows are worn through from the inside, the way they would be if a grown man had been "
            "wearing it for a very long time.",
            [
                ch("Unfold the coat.", SHELTER, 25, "the_tag", "moth_cloud"),
                ch("Lift the jerrycan off its stone.", SCAV, 30, "the_tag", "under_the_stone"),
            ], bank=False),
        "the_tag": node(
            "A paper tag is pinned to the collar with a safety pin gone brown. The printing is the same as on the coats on the "
            "train in the trees: NAME, SCHOOL, DESTINATION. KANE. AGE 9. MERIDIAN COUNTY. On the back, in a woman's hand, in ink "
            "that has held up better than anything else out here: BE GOOD. DON'T LOOK BACK AT THE TRAIN, IT MAKES IT WORSE. I "
            "WILL COME AND GET YOU. MAMA.",
            [
                ch("Open the jerrycan.", SCAV, 30, "the_escort_log", "fumes"),
                ch("Search the camp for whatever else he keeps.", FORAGE, 35, "the_escort_log", "brass_slide"),
            ], loot=[res(WATER, 1)]),
        "the_escort_log": node(
            "The can is stencilled EVAC ESCORT 3, and it hasn't held fuel in a lifetime. Rolled up inside is the escort's "
            "logbook, in pencil and army shorthand. SEPT 14. TRAIN STOPPED AT SIDING, TREES ACROSS LINE. ONE BOY OFF, WILL NOT "
            "GET BACK ON. SAYS HIS MOTHER WON'T FIND HIM IF HE IS ON IT. SEPT 15. TRAIN GONE. DID NOT SEE IT GO. KEPT THE BOY. "
            "TEACHING HIM TO DRIVE, HE IS A NATURAL. HE ASKS ABOUT HIS MOTHER EVERY MILE. The entries thin out over the weeks. "
            "The last one reads: TOLD HIM THE TRAIN WILL COME BACK FOR US. TOLD HIM I WOULD PUT HIM ON IT MYSELF. HE HAS GONE "
            "QUIET. HE HAS FOUND WHERE I KEEP THE. There is no more book.",
            [], loot=[res(FUEL, 1, 2)], table="scrub_common"),
    }
    hazards = {
        "moth_cloud": haz("The coat comes apart in your hands into a cloud of moth and dust that gets into your eyes and throat. "
                          "You cough until the camp swims.",
                          {"water": -1}),
        "under_the_stone": haz("Something has made a nest under the stone, something with too many legs, and it does not want "
                               "to be disturbed.",
                               {"ll": -1}, wound=[1, 0]),
        "fumes": haz("It was a fuel can once. What is left of the fuel has gone to something worse, and the first breath of it "
                     "puts you on your knees.",
                     {"water": -1}),
        "brass_slide": haz("You kneel on the brass and the brass moves, every case at once, and you go down hard among them.",
                           {"ll": -1}, wound=[1, 0]),
    }
    return enc(2, "the_tag", "The Tag", "the_second_camp", nodes, hazards)


# ── 3. The Rules ─────────────────────────────────────────────────────────────
# Why he shoots: the gun is the only voice that carries, and nobody puts him on a train.
def chapter_3():
    nodes = {
        "the_third_camp": node(
            "This camp is tidier than the others, as if he stayed longer. The brass isn't in a ring. It stands in rows, every "
            "case upright, like a crowd waiting for a train. By the dead fire lies an ammo-can lid, scratched deep with a nail "
            "and gone over and over until the letters are rounded with years of going back. It is a list.",
            [
                ch("Read the lid.", NAV, 25, "the_rules", "rows_fall"),
                ch("Count the rows of brass first.", ENDURE, 35, "the_rules", "the_count"),
            ], bank=False),
        "the_rules": node(
            "THE RULES. ONE: ASK FIRST. ALWAYS ASK FIRST. TWO: NEVER GET ON A TRAIN. THREE: SHOOT BEFORE THEY CAN PUT YOU ON "
            "ONE. FOUR: MAKE IT LOUD. A VOICE DOESN'T CARRY. SHE WILL HEAR THE GUN. FIVE: NEVER SHOOT AT A DOOR. SHE COULD BE "
            "BEHIND ANY DOOR. SIX: NEVER LOOK AT THE GLASS. SEVEN: KEEP THE FIRE LIT AND DON'T SHOOT AFTER DARK, SHE MIGHT BE THE "
            "ONE WALKING IN. EIGHT: DON'T SLEEP. Under the eighth, scratched later and much lighter: YOU DON'T HAVE TO ANYMORE.",
            [
                ch("Turn the lid over.", SCAV, 30, "the_tally", "lid_edge"),
                ch("Look for a ninth rule.", NAV, 40, "the_tally", "the_count"),
            ], loot=[res(SCRAP, 1)]),
        "the_tally": node(
            "On the back of the lid is a tally. Fives crossed through, in rows, in blocks of rows, edge to edge and then over the "
            "top of themselves where he ran out of room. Above them, one word: CALLS. Every burst he has ever fired was him "
            "calling her the only way that carries out here. Sixty years of calling, and he counted every one. Down in the "
            "corner, where there is barely room, a ninth rule has been started and given up on. NINE: IF SHE",
            [], loot=[res(SCRAP, 1, 2)], table="scrub_common"),
    }
    hazards = {
        "rows_fall": haz("Your boot catches one case and it tips the next, and the whole crowd of brass goes down in one long "
                         "bright ripple you are sure can be heard for miles. Far off, an engine answers. You go.",
                         {"water": -1}, ends=True),
        "the_count": haz("You start counting and can't stop. Row on row, upright, and the counting takes on a rhythm like an "
                         "engine idling, and when you come out of it the light has gone long and you have lost the number.",
                         {"food": -1}),
        "lid_edge": haz("The lid's edge has been filed to a blade by years of scratching. It opens your palm.",
                        {"ll": -1}, wound=[1, 0]),
    }
    return enc(3, "the_rules", "The Rules", "the_third_camp", nodes, hazards)


# ── 4. The Ring ──────────────────────────────────────────────────────────────
# Why he never stops: he knows. Her house is in the glass, and the jeep can't go in.
# Not glass/4's kitchen -- that family is someone else's.
def chapter_4():
    nodes = {
        "the_trench": node(
            "At the edge of the glass the ground has been worn into a trench: tyre tracks laid on tyre tracks until they are a "
            "foot deep and smooth as a riverbed, running round the glass field in one long loop. Where the trench comes nearest "
            "the glass it swerves away, every lap, at the same place, the way a hand jerks back from a stove. A stone's throw out "
            "on the glass stands a house, fused through, and whole.",
            [
                ch("Walk out across the glass to the house.", ENDURE, 35, "the_door", "glass_burn"),
                ch("Walk the trench first.", NAV, 30, "the_door", "lap_after_lap"),
            ], bank=False),
        "the_door": node(
            "The front door is open and a woman stands in it, set in clear green glass. She has her coat on and a small "
            "suitcase at her feet, and one hand raised, palm out, the way you wave to someone leaving, or someone coming home. "
            "Behind her the clock in the hall says 11:47. Her face is turned toward the trench. From her doorway she would see "
            "every lap of it.",
            [
                ch("Look at the suitcase.", SCAV, 30, "the_letter", "suitcase_glass"),
                ch("Look where she is looking.", ENDURE, 40, "the_letter", "dust_on_the_loop"),
            ], loot=[res(WATER, 1), res(FOOD, 1)]),
        "the_letter": node(
            "Under the suitcase handle, fused into the glass with it, is a letter she meant to post on the way. It is addressed "
            "to a boy, care of an evacuation office, and you can read it through the glass: THEY SAY THE TRAINS ARE RUNNING "
            "AGAIN ON SATURDAY. I AM COMING TO FETCH YOU MYSELF. WAIT FOR ME WHERE THEY PUT YOU. I WILL BE THE ONE WAVING. It is "
            "dated Friday the eleventh of November. He knows. He has known for sixty years. The trench goes round and round her "
            "so that he never has to stop, because stopping would be arriving.",
            [], loot=[res(MED, 1)], table="glass_common"),
    }
    hazards = {
        "glass_burn": haz("The glass is warm through your boots and gets warmer the further out you go. Whatever fused it hasn't "
                          "finished with it.",
                          {"radiation": 1}),
        "lap_after_lap": haz("You walk the trench and it doesn't end, because it doesn't, and you lose an hour going round "
                             "before you notice you have passed the same rock three times.",
                             {"water": -1}),
        "suitcase_glass": haz("The suitcase is glass all the way through, and so is the edge of the handle. It takes a slice "
                              "out of your fingers.",
                              {"ll": -1}, wound=[1, 0]),
        "dust_on_the_loop": haz("You look where she is looking. Far round the loop a plume of dust is coming on along the "
                                "trench. You don't wait to see whose. You run, and the glass takes the soles of your boots.",
                                {"ll": -1}, ends=True),
    }
    return enc(4, "the_ring", "The Ring", "the_trench", nodes, hazards)


# ── 5. The Goodbye ───────────────────────────────────────────────────────────
# His fire, at night. Two endings retire the jeep; running changes nothing.
def chapter_5():
    nodes = {
        "the_firelight": node(
            "You walk into his firelight with your hands where he can see them. He doesn't reach for the gun. It is after dark, "
            "and there are rules. Close to, he is older than the jeep. His skin has crazed like old glaze, the cracks dry to the "
            "bottom, and in the cold his breath doesn't smoke, because there isn't any. The grin you have seen go past at sixty "
            "miles an hour isn't a grin: the mouth just doesn't close any more. He looks you over the way he has looked over "
            "everyone for sixty years, and he asks it, in a voice that is still nine years old. 'Have you seen my mama?'",
            [
                ch("Back out of the light.", NAV, 10, "out_of_the_light", "kicked_ember"),
                ch("Sit down across the fire from him.", SHELTER, 25, "the_question", "her_side"),
                ch("Tell him you found his camps.", NAV, 30, "the_question", "you_followed_me"),
            ], bank=False),
        "the_question": node(
            "He listens the way children listen, with his whole face. When you get as far as the glass he stops you with one "
            "finger. 'I don't look at the glass,' he says. 'That's a rule.' Then, after a long time, with the fire going down "
            "between you: 'You went in, though. Didn't you. You saw the door.' He waits. He has been waiting since 1966, and "
            "this is the first time in sixty years that anyone has had an answer.",
            [
                ch("Say nothing. Back out of the light.", NAV, 10, "out_of_the_light", "kicked_ember"),
                ch("Tell him the truth: she died in her doorway at 11:47, on her way to fetch him.",
                   ENDURE, 55, "the_truth", "point_blank"),
                ch("Lie to him: she is at the Seam, where it is still 1966, coming for him on Saturday.",
                   NAV, 30, "the_lie", "not_her_words"),
            ], bank=False),
        "out_of_the_light": node(
            "You back away until the firelight lets go of you. He doesn't follow, and he doesn't call after you. He goes back to "
            "watching the dark the way he watches it every night, in case she is the one walking in. At dawn he will drive on. "
            "Nothing has changed. You can come back another night.",
            [], bank=False, escape=True),
        "the_truth": node(
            "He doesn't shoot. He takes it the way the brass takes the fire: all at once, and then nothing. After a while he "
            "unhooks the cable from the steering wheel and lays it on the dash. He takes the paper tag off the mirror and puts "
            "it in your hand. Then he climbs down out of the jeep, for the first time in longer than you have been alive, and "
            "walks out onto the glass. He doesn't look back at the jeep. It makes it worse. In the morning the jeep is still "
            "there, engine cold, and one set of boot prints goes out across the glass toward a house with its door open, and "
            "doesn't come back.",
            [], loot=[res(FUEL, 2), res(SCRAP, 2)], ending="truth"),
        "the_lie": node(
            "His whole face changes. 'Saturday,' he says. 'She said Saturday.' He is already turning the key. He doesn't ask you "
            "the way. He has always known where the Seam is, the way he has always known where the glass is. The jeep goes out "
            "of the firelight with no lamps lit, and you hear it a long time going west, and then you don't. One day somebody "
            "will tell you they saw it at the line, empty, among the offerings, a child's coat folded on the seat. Nobody goes "
            "through the Seam. That's the rule. He was never any good at other people's rules.",
            [], loot=[res(FUEL, 1), res(SCRAP, 1), res(WATER, 1)], ending="lie"),
    }
    hazards = {
        "kicked_ember": haz("You back into the edge of the fire and an ember goes up your trouser leg. You don't make a sound. "
                            "You are very sure you shouldn't.",
                            {"ll": -1}),
        "her_side": haz("You sit where she would have sat. He sees it before you do, and for a moment his hand is on the wheel, "
                        "and on the cable, and the night isn't safe at all. Then he lets go. You move round the fire. Your heart "
                        "takes longer.",
                        {"water": -1}),
        "you_followed_me": haz("'You followed me,' he says, and for one moment he isn't nine at all. The fire spits. You keep "
                               "very still until he looks away.",
                               {"food": -1}),
        "point_blank": haz("He breaks a rule. It is the first one he has broken in sixty years, and the gun is very close. "
                           "Afterwards he sits looking at it as if somebody else had done it. Then he starts the engine and goes, "
                           "lamps dark, long before dawn, and the fire burns down on its own.",
                           {"ll": -2}, ends=True, wound=[0, 1]),
        "not_her_words": haz("You get her wrong: some word, some small thing. 'She wouldn't say it like that,' he says. He looks "
                             "at you a long time. Then he kicks out his own fire, which he never does, and drives off into the "
                             "dark without lamps. You sit in the smoke and think about the letter, and the day she put in it.",
                             {"water": -1}, ends=True),
    }
    return enc(5, "the_goodbye", "The Goodbye", "the_firelight", nodes, hazards)


if __name__ == "__main__":
    for i, fn in enumerate((chapter_1, chapter_2, chapter_3, chapter_4, chapter_5), 1):
        e = fn(); check(e); write(f"kane/{i}.json", e)
