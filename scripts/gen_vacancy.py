# Generates VACANCY, the settlement mini-quest (docs/vacancy.md).
#   python scripts/gen_vacancy.py data/encounters
# Re-running OVERWRITES data/encounters/vacancy/{1..4}.json -- edit here, not
# in the JSON. The file number is the part (vacancy.hpp VAC_PART_*):
#   1  Vacancy       the town under the sign; reading it to the end starts the
#                    nights counting and puts the other two on the map
#   2  The Front Desk  out on the open ground; pays the Guest Register (70)
#   3  Housekeeping    in a ruin; pays the Master Key (71)
#   4  Check-Out     back under the sign; needs both, "vac_ending": "saved"
# A sub-quest counts once its item is BANKED (vacOnBank), not when its last
# node is reached: walking off with the book still on the counter leaves the
# desk where it is.
# Skills: 0 Navigate, 1 Forage, 2 Scavenge, 3 Shelter, 4 Endure.
# Resources (loot "res"): 0 water, 1 food, 2 fuel, 3 med, 4 scrap.
# Text: straight single quotes for speech, no double quotes, no backslashes,
# no newlines. No number of nights in the text: that is VAC_ROOMS in the
# firmware, and the sign shows it.
import json, os, sys

ROOT = sys.argv[1]
NAV, FORAGE, SCAV, SHELTER, ENDURE = 0, 1, 2, 3, 4
WATER, FOOD, FUEL, MED, SCRAP = 0, 1, 2, 3, 4
REGISTER, MASTER_KEY = 70, 71   # data/items.cfg


def ch(label, skill, risk, to, haz=None, needs=None):
    c = {"label": label, "cost": {}, "base_risk": risk, "skill": skill, "success_node": to}
    if needs: c["requires_item"] = needs
    if haz: c["hazard_id"] = haz
    return c


def res(r, lo, hi=None): return {"res": r, "qty": [lo, hi if hi is not None else lo]}
def item(i): return {"item": i, "qty": [1, 1]}


def node(text, choices, bank=True, loot=None, table=None, escape=False, ending=None):
    n = {"text": text}
    if escape: n["escape"] = True
    if ending: n["vac_ending"] = ending
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


def enc(part, slug, title, start, nodes, hazards):
    return {"id": f"vacancy_{part}_{slug}", "title": title, "vacancy": {"part": part},
            "start_node": start, "nodes": nodes, "hazards": hazards}


def check(obj):
    """Every choice lands on a real node and names a real hazard, every node is
    reachable, the text is safe for the firmware's JSON reader, and the gates
    follow the Null Meridian rule: every node keeps an ungated choice, and a
    gated choice never goes first."""
    nodes, hazards = obj["nodes"], obj["hazards"]
    part = obj["vacancy"]["part"]
    assert obj["start_node"] in nodes
    for key, n in nodes.items():
        for c in n["choices"]:
            assert c["success_node"] in nodes, (key, c["success_node"])
            assert c.get("hazard_id") is None or c["hazard_id"] in hazards, (key, c.get("hazard_id"))
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
    ends = [k for k, n in nodes.items() if n.get("vac_ending")]
    assert (part == 4) == bool(ends), obj["id"]
    # The sub-quests pay their item on the last node and nowhere else, so the
    # bank that carries it is the one that closes the site.
    pays = {2: REGISTER, 3: MASTER_KEY}.get(part)
    for k, n in nodes.items():
        got = [e.get("item") for e in n.get("loot", []) if "item" in e]
        assert got == ([pays] if pays and not n["choices"] and not n.get("escape") else []), (obj["id"], k, got)
    gates = {c.get("requires_item") for n in nodes.values() for c in n["choices"]} - {None}
    assert gates == ({REGISTER, MASTER_KEY} if part == 4 else set()), (obj["id"], gates)


def write(rel, obj):
    check(obj)
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    body = json.dumps(obj, indent=2, ensure_ascii=False) + "\n"
    assert len(body.encode("utf-8")) < 16384, (rel, len(body))   # ENC_FILE_CAP
    for k in list(obj["nodes"]) + list(obj["hazards"]): assert len(k) < 24, k   # ENC_KEY_LEN
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    print(f"{rel}: {len(body)} B, {len(obj['nodes'])} nodes")


# ── 1. Vacancy ───────────────────────────────────────────────────────────────
# The town under the sign. Read to the end, it puts the desk and the corridor
# on the map and the nights start counting.
def part_1():
    nodes = {
        "the_sign": node(
            "Over the square stands a sign that was not here the last time anyone came through, that cannot have been here, "
            "because nothing out here has had current in it for sixty years: a tall roadside motel sign on a steel pole, pink "
            "neon tubing bent into the word VACANCY, and under it a smaller box that says NO, unlit. There is no motel. The pole "
            "goes straight into the bare dirt of the square. The townsfolk walk round it without looking up, the way you walk "
            "round a hole in the road you have all agreed not to mention. Its hum gets into your back teeth and sits there.",
            [
                ch("Ask somebody about the sign.", SHELTER, 20, "the_door", "nobody_says"),
                ch("Stand under it and listen to the hum.", ENDURE, 30, "the_door", "the_hum"),
            ], bank=False),
        "the_door": node(
            "An old woman who still remembers things takes you by the sleeve to a house on the east row. Its front door is gone. "
            "In its place, fitted to the old frame as if it had always been there, is a motel door: turquoise paint, a peephole, a "
            "brass number on it, a DO NOT DISTURB card hung on the handle. Through the peephole there are two single beds made up "
            "tight, a painting of a blue sea nobody here has ever seen, a television showing snow with the sound turned down. Two "
            "pairs of boots stand by the beds, toes together. 'It started with the sign,' she says. 'Every night, another house. "
            "In the morning there is a door on it that won't open, and nobody can say who lived there. Only that somebody did. "
            "We count the chairs at supper. There is always one more chair than us.'",
            [
                ch("Try the door.", SCAV, 35, "the_count", "the_peephole"),
                ch("Walk the street and count the doors.", NAV, 25, "the_count", "count_again"),
            ], loot=[res(FOOD, 1)]),
        "the_count": node(
            "Two travellers have come in off the waste this week telling the same nonsense, she says, and both of them now sleep "
            "with the lamp on. One saw a front desk standing on its own out on the open ground, nobody behind it, a lamp lit with "
            "nothing to light it, and lying open on the counter a guest book with this town's names in it. The other got turned "
            "round in a ruin and walked down a motel corridor longer than the building it was in, past a housekeeping cart, and "
            "heard somebody humming behind every door. 'Anybody who has ever stayed anywhere knows you can check out,' she says. "
            "'You need the book, to sign us out. And you need the key, to let us out.' Over the square the sign buzzes louder, "
            "once, as if it heard, and down the east row another window you had not noticed is lit, warm and yellow, with "
            "somebody sitting on the end of a bed inside it, facing out.",
            [], loot=[res(WATER, 1), res(FOOD, 1)]),
    }
    hazards = {
        "nobody_says": haz("Nobody will say. A man you ask smiles and tells you the town has always had a sign, and then he stops "
                           "in the middle of the sentence and stands there with his mouth a little open, trying to remember the "
                           "word for the thing he was saying. You leave him standing. When you look back he is still there.",
                           {"water": -1}),
        "the_hum": haz("The hum gets into your teeth, and then in behind your eyes, and when you come back to yourself you are "
                       "standing with one hand flat on the pole and your nose is bleeding onto your shirt and the light has moved.",
                       {"ll": -1}),
        "the_peephole": haz("You put your eye to the peephole. The room is the same. The painting is the same. But the bed nearest "
                            "the door has a dent in it now, as though somebody has this moment sat down on the end of it, and the "
                            "television has changed channel. When you step back the brass number is warm under your hand.",
                            {"ll": -1}),
        "count_again": haz("You count the doors down the east row and get eleven. You count them back up and get twelve. You count "
                           "them a third time with your hand on every frame, and between two houses where there has never been a "
                           "house there is a turquoise door with a card on the handle. You don't knock.",
                           {"water": -1}),
    }
    return enc(1, "the_sign", "Vacancy", "the_sign", nodes, hazards)


# ── 2. The Front Desk ────────────────────────────────────────────────────────
# Out on the open ground. The night clerk, and the register: the Guest
# Register (item 70) on the last node.
def part_2():
    nodes = {
        "the_carpet": node(
            "On the open ground lies a rectangle of carpet, burgundy, patterned with interlocking gold diamonds, its edges cut as "
            "clean as if with scissors, and on the carpet stands a front desk. Only the desk: a long laminate counter, a brass "
            "bell, a lamp with a fringed shade that is lit with no wire going to it, a rack of pigeonholes with keys on diamond "
            "fobs, a stand of postcards of a beach, a little dish of mints. There is no building. The dust blows across the waste "
            "and does not settle on the carpet; it goes round. Behind the counter a door marked STAFF ONLY stands in its frame, "
            "and through the gap under it you can see the waste going on behind.",
            [
                ch("Ring the bell.", SHELTER, 30, "the_clerk", "ring_twice"),
                ch("Go round behind the counter yourself.", SCAV, 35, "the_clerk", "staff_only"),
            ], bank=False),
        "the_clerk": node(
            "The STAFF ONLY door opens and the night clerk comes out of it, out of the empty air behind it, straightening his tie. "
            "He is tall and very clean, in a maroon blazer with a brass name badge, and the name on the badge belongs to somebody "
            "from the town: the old woman said it. He has that person's face, more or less. It sits on him the way a borrowed "
            "coat does, a little loose at the shoulders. 'Welcome,' he says. 'Checking in?' Behind him there is a key in every "
            "pigeonhole but a handful. On the counter, open, turned toward you, with a pen on a little brass chain, is the "
            "register.",
            [
                ch("Tell him you're here to check somebody out.", SHELTER, 40, "the_register", "policy"),
                ch("Ask about the rates while you slide the book your way.", SCAV, 40, "the_register", "the_pen"),
            ], loot=[res(FOOD, 1)]),
        "the_register": node(
            "The register is a long ledger bound in maroon leatherette, and every page at the back of it is the town: names in a "
            "careful hand you don't know, a room number beside each, an arrival time, and in the column for departures, nothing. "
            "Further forward there are pages and pages of other towns, other names, other rooms, and on the first page of all, "
            "dated Friday the eleventh of November 1966, the arrival column says 11:47 on every line, all the way down, page after "
            "page. The clerk watches you close it. 'Of course,' he says, smiling with somebody else's mouth. 'Take all the time "
            "you need. You can check out any time you like. But you can never - ' and he stops, and frowns, as if a line has been "
            "taken off him in the middle of saying it. You put the book under your arm and step off the carpet, and behind you "
            "the lamp goes out.",
            [], loot=[item(REGISTER), res(SCRAP, 1)]),
    }
    hazards = {
        "ring_twice": haz("You ring once and nobody comes. You ring again. Somewhere in a building that is not there a telephone "
                          "starts ringing in answer, and every ring is nearer than the last, and you are down on your knees with "
                          "your hands over your ears before it stops.",
                          {"ll": -1}),
        "staff_only": haz("Behind the counter the carpet is wet, and not with water. You don't look to see with what. It soaks "
                          "through your boots and up your shins with the cold of a room nobody has heated since 1966, and it "
                          "takes the rest of the day to get the feeling back.",
                          {"water": -1}, wound=[1, 0]),
        "policy": haz("'I'm afraid that would be against policy,' he says, very kindly, and lays his hand flat on yours on the "
                      "counter. It is warm. The warmth goes up your arm and you feel all at once very tired and very welcome, as "
                      "if you have driven a long way and there is a bed turned down for you somewhere close. Taking your hand back "
                      "is the hardest thing you do all week.",
                      {"ll": -1}),
        "the_pen": haz("The pen on its chain starts to write the moment your fingers touch it. It has put the first three letters "
                       "of your name on the next empty line before you can let go. The clerk turns the book back toward himself, "
                       "politely, and blots it, and you find you have walked backwards off the carpet without meaning to.",
                       {"ll": -1}, ends=True),
    }
    return enc(2, "front_desk", "The Front Desk", "the_carpet", nodes, hazards)


# ── 3. Housekeeping ──────────────────────────────────────────────────────────
# In a ruin. The corridor, the maid, and the master key (item 71) on the last
# node.
def part_3():
    nodes = {
        "the_corridor": node(
            "In the ruin there is one doorway in better repair than the building round it: two glass doors, a rubber mat that "
            "says WELCOME, and beyond them a corridor. The building is twenty paces deep. The corridor runs back much further than "
            "that: a low ceiling, strip lights that buzz and stutter, carpet the colour of old wine, an ice machine, a fire hose "
            "behind glass, and doors down both sides, turquoise, numbered, every one with a DO NOT DISTURB card on its handle. A "
            "long way down it, a wheel is squeaking.",
            [
                ch("Follow the squeak.", NAV, 30, "the_cart", "the_ice_machine"),
                ch("Read the room numbers as you go.", ENDURE, 35, "the_cart", "the_numbers"),
            ], bank=False),
        "the_cart": node(
            "A housekeeping cart, piled with folded towels, little wrapped soaps, toilet rolls in paper bands, a bucket, and a "
            "ring of keys hung off the handle on a chain. The maid is in the room with the door propped on its latch, turning "
            "down the beds. Both beds have somebody in them already: an old couple from the town, asleep on their backs, eyes "
            "open, smiling at the ceiling. She folds the sheet down neatly under their chins and puts a mint on each pillow beside "
            "their heads and moves on to the next room, humming. She has no face. There is only the shape a face makes under a "
            "pillowcase.",
            [
                ch("Lift the keys off the cart while she works.", SCAV, 40, "the_key", "she_turns"),
                ch("Wait in the corridor and ask her for the master.", SHELTER, 40, "the_key", "do_not_disturb"),
            ], loot=[res(MED, 1)]),
        "the_key": node(
            "You have the master key: a long brass thing on a plastic fob stamped MANAGEMENT. It is warm, and it is heavier than "
            "brass has any right to be. As your hand closes on it every door in the corridor clicks, all together, the sound of a "
            "whole building coming unlocked at once, and down the whole length of it nobody comes out. Behind you the maid has "
            "started on the next room. The tune she is humming is one you know, off a radio, from a long time ago, from before "
            "you were born. You walk back toward the glass doors and it takes far less time than it took to walk in.",
            [], loot=[item(MASTER_KEY)]),
    }
    hazards = {
        "the_ice_machine": haz("The ice machine drops a load as you pass it. You look down into the bin. It is full to the lid, and "
                               "it is not ice, it is teeth, small ones, children's, and the scoop is standing up in them. You don't "
                               "remember running. You remember the squeak, ahead of you the whole time, never any nearer.",
                               {"water": -1}),
        "the_numbers": haz("The numbers go 114, 115, 116, 117, 117, 117, 1147, and you stop reading them, because the next one is "
                           "your age and the one after it is the year you were born, and your nose has started to bleed and won't "
                           "stop.",
                           {"ll": -1}),
        "she_turns": haz("She turns round with your hand on the keys. She says nothing. She puts a mint in your palm and closes your "
                         "fingers over it with hers, and then you are sitting in the corridor against the wall with no idea how long "
                         "you have been sitting there, and the water in your bottle has gone warm, and there is less of it.",
                         {"water": -2}, ends=True),
        "do_not_disturb": haz("DO NOT DISTURB, says the card on the next door, and the next, and the next, and you realise you "
                              "have been reading them out loud, door after door, for a long time, in a voice that is not quite "
                              "yours.",
                              {"ll": -1}),
    }
    return enc(3, "housekeeping", "Housekeeping", "the_corridor", nodes, hazards)


# ── 4. Check-Out ─────────────────────────────────────────────────────────────
# Back under the sign with both. The register opens the names, the key opens
# the doors; the last node's "vac_ending": "saved" puts the sign out.
def part_4():
    nodes = {
        "under_the_sign": node(
            "The sign is louder than it was. Every house that has turned has its light on now, warm and yellow, and in each lit "
            "window somebody sits on the end of a bed with their hands in their lap, facing out, waiting. The square is full of "
            "the ones still left, standing very still, looking up at the small dark box that says NO. You have the register "
            "under your arm and the master key in your fist. The pen on its little chain has followed the book in off the waste.",
            [
                ch("Not tonight. Walk out of the pink light.", NAV, 10, "walk_away", "light_follows"),
                ch("Open the register under the sign.", ENDURE, 30, "the_names", "your_line", needs=REGISTER),
            ], bank=False),
        "the_names": node(
            "You read the names out, one at a time, under the buzzing tubes, and against each one in the departure column you "
            "write CHECKED OUT and the time. The pen fights you. Each time you finish a line somebody in the square breathes out. "
            "Some of the names belong to people standing right beside you, who have not gone yet, whose arrival times are "
            "tomorrow, and the day after. You strike those too. Near the bottom of the last page is a line with the first three "
            "letters of your own name on it, and an arrival time of tonight.",
            [
                ch("Close the book and get out of the square.", NAV, 10, "walk_away", "light_follows"),
                ch("Strike your own line, then take the key down the street.", ENDURE, 40, "the_doors", "the_last_line",
                   needs=MASTER_KEY),
            ], bank=False),
        "the_doors": node(
            "The master key turns in every turquoise door on the street, and every door opens on the same room: two beds, the "
            "painting of the sea, the snow on the television. In every room somebody from this town is sitting on the end of a "
            "bed, waiting. You read their names out of the book and tell them they have checked out. They look at you as if you "
            "are the first person to speak to them in a very long time. Then they get up, and stand in the doorway, and step over "
            "the sill onto dirt, and the room behind them is a house again: a kitchen, a coat on a hook, a cold stove. The last "
            "door is in the middle of the square, at the foot of the pole, where there has never been a house. It is numbered "
            "1147.",
            [
                ch("Leave that one shut and walk away.", NAV, 10, "walk_away", "light_follows"),
                ch("Put the key in 1147 and open it.", ENDURE, 45, "checked_out", "room_1147"),
            ], bank=False),
        "checked_out": node(
            "Room 1147 is every room. It is the room the whole world checked into at eleven forty-seven on the eleventh of "
            "November 1966, and it goes back further than the waste goes, and every bed in it is made, and every bed is taken. "
            "All of them turn their heads on the pillows to look at you in the doorway. Then the sign above you gives a flat pop "
            "like a bulb going, and the hum stops, and the door is an empty frame with the square showing through it, and then "
            "not even that. The NO never lit. In the morning the town counts its chairs at supper and there is somebody on every "
            "one. Somebody has put the register on the meeting-hall table, where it will never be opened again. You reach into "
            "your pocket for the master key and there is only a mint.",
            [], loot=[res(WATER, 2), res(FOOD, 2), res(MED, 2)], table="settlement_rare", ending="saved"),
        "walk_away": node(
            "You step back out of the pink light. The ones in the square don't turn round. The sign hums on over all of you. It "
            "will still be here. It has always been very patient.",
            [], bank=False, escape=True),
    }
    hazards = {
        "light_follows": haz("The pink light comes after you to the edge of the square, like a hand laid on the back of your neck, "
                             "and lets you go only when you stop and stand still for it. You lose a while standing there.",
                             {"water": -1}),
        "your_line": haz("As you open the book the pen swings on its chain and writes, all on its own, the rest of your name on the "
                         "line that has been waiting for it. For a moment the buzz of the sign is the most restful sound you have "
                         "ever heard, and you have to bite the inside of your cheek until it bleeds to stop yourself walking toward "
                         "the nearest lit door.",
                         {"ll": -1}),
        "the_last_line": haz("Your own line will not be struck. The pen goes through the page and the page under it and the one "
                             "under that, and your line is still there, unbroken, written deeper. Behind you a turquoise door you "
                             "had not seen opens a crack, and the light that comes out of it is warm, and it is meant for you.",
                             {"ll": -1}, wound=[1, 0]),
        "room_1147": haz("You open the door an inch. Something on the other side, very polite and very strong, closes it again, "
                         "and the sign flares so bright the whole square goes pink, and when you can see again you are on your back "
                         "in the dirt with the hum louder than ever and the key still in your fist.",
                         {"ll": -2}, ends=True),
    }
    return enc(4, "check_out", "Check-Out", "under_the_sign", nodes, hazards)


for part, build in ((1, part_1), (2, part_2), (3, part_3), (4, part_4)):
    write(f"vacancy/{part}.json", build())
