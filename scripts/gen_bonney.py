# Generates ONE-HAND BONNEY, the Ash Dunes story quest (docs/one-hand-bonney.md).
#   python scripts/gen_bonney.py data/encounters
# Re-running OVERWRITES data/encounters/bonney/{1,2}.json -- edit here, not in
# the JSON. The file number is the section (bonney.hpp BON_SEC_*):
#   1  Guess Which Hand   Bonney at her fire, Turnbull already turning on the
#                         spit; Pinch, her runaway hand, robs the reader.
#                         "bonney_ending": "deal" (she gets something) or
#                         "caught" (Pinch is caught and gives it back). Either
#                         one and she goes back to her crank: she stays put,
#                         and the same fire now holds section 2.
#   2  Supper at Bonney's the same fire: a pit the size of a car and Dennis
#                         Turnbull on the spit, with the stolen thing in his
#                         mouth like the apple in a roast pig. "rescue" hands
#                         it back to the reader; "bargain" (a toenail) frees
#                         him and she eats it. Either one sends her off, to
#                         turn up near a settlement at the next dawn.
#
# Theft is a hazard: "penalty": {"steal": 1}. On a failed roll Pinch lifts one
# thing from the reader's pack (cursed things first), once per scene.
# {{stolen}} and {{apple}} are filled by the client from the server's word
# (ev bonney) and carry their own determiner ("your Cursed Note"); the
# placeholders below are only the fallback an older client shows.
#
# Skills: 0 Navigate, 1 Forage, 2 Scavenge, 3 Shelter, 4 Endure.
# Text: straight single quotes for speech, no double quotes, no backslashes,
# no newlines.
import json, os, sys

ROOT = sys.argv[1]
NAV, FORAGE, SCAV, SHELTER, ENDURE = 0, 1, 2, 3, 4
WATER, FOOD, FUEL, MED, SCRAP = 0, 1, 2, 3, 4


def ch(label, skill, risk, to, haz=None, cost=None):
    c = {"label": label, "cost": cost or {}, "base_risk": risk, "skill": skill, "success_node": to}
    if haz: c["hazard_id"] = haz
    return c


def res(r, lo, hi=None): return {"res": r, "qty": [lo, hi if hi is not None else lo]}


def node(text, choices, bank=False, loot=None, table=None, escape=False, ending=None):
    n = {"text": text}
    if escape: n["escape"] = True
    if ending: n["bonney_ending"] = ending
    n["can_bank"] = bank
    if loot: n["loot"] = loot
    if table: n["loot_table"] = table
    n["choices"] = choices
    return n


def haz(text, pen, wound=None, ends=False):
    h = {"text": text, "penalty": pen}
    if wound: h["wound"] = wound
    h["ends_encounter"] = ends
    return h


def enc(sec, slug, title, start, nodes, hazards, placeholders):
    return {"id": f"bonney_{sec}_{slug}", "title": title, "bonney": {"section": sec},
            "start_node": start, "nodes": nodes, "hazards": hazards, "placeholders": placeholders}


ENDINGS = {1: {"deal", "caught"}, 2: {"rescue", "bargain"}}


def check(obj):
    """Every choice lands on a real node and names a real hazard, every node is
    reachable, every terminal node is an escape or one of the section's
    endings, a theft only ever comes from a hazard, the text is safe for the
    firmware's JSON reader, and the file fits ENC_FILE_CAP."""
    nodes, hazards, sec = obj["nodes"], obj["hazards"], obj["bonney"]["section"]
    assert obj["start_node"] in nodes
    for key, n in nodes.items():
        for c in n["choices"]:
            assert c["success_node"] in nodes, (key, c["success_node"])
            assert c.get("hazard_id") is None or c["hazard_id"] in hazards, (key, c.get("hazard_id"))
            assert set(c["cost"]) <= {"ll", "radiation", "food", "water", "scrap", "med"}, c["cost"]
        if not n["choices"]:
            assert n.get("escape") or n.get("bonney_ending") in ENDINGS[sec], (key, "a dead end that is neither")
        else:
            assert "bonney_ending" not in n, (key, "an ending with choices after it")
    for h in hazards.values():
        assert set(h["penalty"]) <= {"ll", "radiation", "food", "water", "fuel", "med", "scrap", "steal"}, h["penalty"]
    assert {n.get("bonney_ending") for n in nodes.values()} - {None} == ENDINGS[sec]
    for t in [n["text"] for n in nodes.values()] + [h["text"] for h in hazards.values()] + \
             [c["label"] for n in nodes.values() for c in n["choices"]] + \
             [o for opts in obj["placeholders"].values() for o in opts]:
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


# ── 1  Guess Which Hand ─────────────────────────────────────────────────────
def section_1():
    nodes = {
        "her_fire": node(
            "You smell her before you see her: tyre smoke, hot fat, and something under both that makes your stomach growl "
            "before your head can stop it. Then you are over the dune and there it is. A pit dug in the ash and full of fire, "
            "tyres and chair legs burning in it, and over it, on two engine blocks, a spit made from the back axle of a truck. A "
            "man is trussed to the spit with fence wire, in his underwear, with an apple in his mouth, turning slowly. He rolls "
            "his eyes at you as he comes round, apologetically. Propped against the wind behind the fire is the bonnet of a car, "
            "piled with jars; across its front, chrome letters once spelled a longer word, five are left, B O N N E, and someone "
            "has scratched a Y after them with a nail. At the crank stands a woman, if she is one. She is tall when she "
            "straightens up, and then she straightens some more: a hump like a sack of rocks under a coat of stitched tarp, skin "
            "the yellow-grey of old lard, a hooked nose with a wart on it that has hairs of its own, wild grey hair under a rag, "
            "a brass hoop through an ear stretched long. Her eyes are the wrong colour, a muddy gold, and they do not blink "
            "enough. Her tongue comes out and tastes the air in your direction. 'Ohhh,' she says, in a voice like a drain. 'You "
            "smell like you've got something for Bonney.' She looks you up and down the way a butcher looks at a pig. 'Skinny. "
            "Bit of gristle. Still.' Her left sleeve hangs empty, knotted at the wrist with a bootlace. Her right hand is the "
            "size of a shovel, all knuckle, with somebody else's rings on it, and it never stops turning the crank.",
            [
                ch("Stand your ground and let her have a sniff.", ENDURE, 35, "the_jars", "the_sniff"),
                ch("Ask her what she has in all those jars.", NAV, 30, "the_jars", "pinched_lean"),
                ch("Back off slowly, with your pack in front of you.", NAV, 0, "walk_away"),
            ]),
        "the_jars": node(
            "She lets go of the crank to show you, because she can't not, and the spit stops, and the man on it says 'Oh, thank "
            "God,' very quietly. Jar after jar off the bonnet, held up to the firelight in the one hand. Teeth, sorted by size "
            "in a muffin tin, the gold ones at the front. Toenails in a snuff tin, 'from all over'. A ball of belly-button "
            "fluff as big as your head, 'sixty years of it, everybody's, every colour'. Candles that smell of ears, because that is "
            "what they are made of. Jars of grey fog: 'Last breaths, dumpling. You catch them at the very end. It's all in the "
            "wrist.' She has, you notice, only the one wrist. Then she smiles, and the teeth in her mouth are not all the same "
            "size, and not all hers. 'Bonney's short a back one,' she says, tapping her gum with a nail like an oyster shell. "
            "'Little molar. You've got loads.' Behind you, very softly, the ash shifts.",
            [
                ch("Give her a tooth. A little one, at the back.", NAV, 0, "guess_which", cost={"ll": 1}),
                ch("Keep your teeth, and keep your eyes on her hand.", NAV, 40, "guess_which", "pinched_watch"),
                ch("Spin round fast and look at the ash behind you.", FORAGE, 45, "guess_which", "the_burrow"),
            ]),
        "guess_which": node(
            "'Let's have a game,' says Bonney. 'Bonney's favourite.' She puts the one hand behind her back, does something there "
            "with a great deal of knuckle-cracking, and brings it round in front of your face, closed into a fist the size of a "
            "cabbage. 'Guess which hand.' There is only one hand. You look at it. She looks at you looking at it, and her gold "
            "eyes do not blink once. The empty left sleeve, knotted at the wrist, gives a small and definite twitch. 'Go "
            "on,' she says, sweet as rot. 'Guess.'",
            [
                ch("Laugh along, and shake on it.", NAV, 0, "the_deal"),
                ch("Grab the empty sleeve.", SCAV, 45, "caught_it", "the_slap"),
                ch("Ignore her. Slap a hand down on your own pack.", ENDURE, 50, "caught_it", "pinched_bite"),
            ]),
        "the_deal": node(
            "You laugh, because what else is there, and you shake on it. Her hand closes round yours, warm and damp, every joint "
            "working, and it does not let go. She keeps you there pumping your arm, telling you how she came down out of the "
            "bright on the Friday, and how she was born in a tent at the county fair, which she says was the same day, while "
            "something light and quick runs up the back of your leg, over your pack, and away. When she lets go, your pack is "
            "open. Across the ash a hand is running for the bonnet on its fingertips like a crab, her other hand, holding {{stolen}} "
            "over its head like a trophy. 'Pinch,' says Bonney fondly. 'Left me years ago. Got ideas. Still comes home for "
            "supper.' She presses something into your palm: a pickled egg, or something egg-shaped that has been pickled. 'Fair "
            "trade. Come back and eat with Bonney, sweetmeat. Supper won't be long.' Then she takes up her crank again, humming, "
            "and Turnbull comes round over the flames and looks at you like a man who knows exactly what you have just lost.",
            [], bank=True, loot=[res(FOOD, 1)], ending="deal"),
        "caught_it": node(
            "Your hand closes on something that is not yours. It is cold and leathery and fighting: a hand, on its own, ending at "
            "the wrist in a neat puckered knot, with the strap of your pack wound round two of its fingers. On its back is a faded "
            "tattoo of an eye. It bucks like a landed fish, then goes limp and sulky, and you prise back everything it had hold "
            "of, a finger at a time. Bonney's face does several things at once, none of them a face's job. Then she laughs, a "
            "long, wet, delighted noise. 'Quick eyes,' she says. 'Bonney likes eyes. That's Pinch. Left me years ago. Got ideas.' "
            "She holds out her sleeve and the hand climbs up into it and lies there, sulking. 'Come back for supper, quick eyes. "
            "Bonney's got a roast on.' She leaves you a jar off the bonnet for your trouble and takes up her crank again. "
            "Turnbull, going round, gives you a small, careful nod.",
            [], bank=True, table="dunes_common", ending="caught"),
        "walk_away": node(
            "You back away up the dune with your pack held in front of you like a shield. She doesn't follow. She goes back to "
            "her crank and watches you go over it, licking her one hand clean a finger at a time. The man on the spit watches "
            "you go too, with something like envy. 'Bonney'll see you again,' she calls after you. 'Bonney always does.'",
            [], escape=True),
    }
    hazards = {
        "the_sniff": haz(
            "She closes the distance in two long steps and sniffs you, hard, from boots to crown, like a dog at a gate. Then the "
            "tongue comes out and licks one of your eyes. Just the one. You bring up everything you drank today.",
            {"water": -1}),
        "pinched_lean": haz(
            "She leans down to answer, and down, and down, until her face is all you can see and her breath is all you can smell: "
            "hot pennies and lard. Behind you something tugs at your pack, gently, twice. When she straightens up and you turn "
            "round there is nothing there, only a line of small holes running off across the ash, like the tracks of a crab.",
            {"steal": 1}),
        "pinched_watch": haz(
            "You watch her hand, and only her hand, and it doesn't do a thing. That is the trick. Something brushes your hip, "
            "quick as a moth, and when you look down the flap of your pack is open and a line of little holes runs off across "
            "the ash.",
            {"steal": 1}),
        "the_burrow": haz(
            "The ash bursts open at your heel. Something hard and five-fingered clamps on your ankle and squeezes, nails first, "
            "then dives back under like a mole. Bonney giggles behind her hand.",
            {"ll": -1}, wound=[1, 0]),
        "the_slap": haz(
            "You grab for the sleeve and get her other hand instead, the big one, across the side of your head. It is like being "
            "hit with a wet saddle. When the lights stop going off she is tutting at you.",
            {"ll": -1}),
        "pinched_bite": haz(
            "You slap your hand down on your pack and find a hand already in it, a cold one, which bites you. Hands should not be "
            "able to bite. By the time you have stopped shouting it is gone, and so is something of yours.",
            {"ll": -1, "steal": 1}),
    }
    write("bonney/1.json", enc(1, "guess_which_hand", "Guess Which Hand", "her_fire", nodes, hazards,
                              {"stolen": ["something of yours"]}))


# ── 2  Supper at Bonney's ───────────────────────────────────────────────────
def section_2():
    nodes = {
        "the_fire": node(
            "Bonney's fire has burned down to a deep red roar, the way a fire does when it is ready to cook on. The tyres in the "
            "pit have gone to glowing hoops, the piano is down to its iron frame, and over it all the truck axle turns on its "
            "engine blocks with a man wired to it, pink down one side and done on the other. 'Oh, hello,' says the man as he "
            "comes round. 'Don't mind me.' On the far side of the fire Bonney is stirring a hubcap of something brown with a car "
            "aerial. She sees you and her whole face splits. 'Sweetmeat! You came! Sit, sit. Supper's nearly done.'",
            [
                ch("Sit down at her fire.", SHELTER, 30, "the_spit", "the_heat"),
                ch("Walk round the fire to the man on the spit.", ENDURE, 35, "the_spit", "the_drip"),
                ch("Back out of the firelight.", NAV, 0, "walk_away"),
            ]),
        "the_spit": node(
            "Close to, the man on the spit is thin and polite and very pink down one side. 'Turnbull,' he says as he comes "
            "round. 'Dennis Turnbull. Since Tuesday. She does me low and slow. She says it is the only way.' He glistens. He has "
            "been basted, with what you would rather not know. The crank on the end of the axle is being turned, round and "
            "round, by a hand. Just a hand, braced on its fingertips on the engine block, working: Pinch. And wedged in "
            "Turnbull's mouth, the way you would put an apple in a roast pig, so that he has to talk round it, is {{apple}}. "
            "'Mm,' he says, apologetically. 'Mm-hm. Sorry. Hers.' Bonney gets up with the hubcap. 'Gravy's catching. Bonney's "
            "fetching more fat. Don't you touch Bonney's supper.' She ambles off into the dark behind the bonnet.",
            [
                ch("Wave a bit of bright scrap at Pinch.", SCAV, 30, "the_wire", "pinch_bites", cost={"scrap": 1}),
                ch("Kick the crank off its block.", ENDURE, 45, "the_wire", "the_crank"),
                ch("Ask Turnbull how she ties her knots.", NAV, 35, "the_wire", "the_gravy"),
            ]),
        "the_wire": node(
            "With Pinch off the crank the spit stops, and Turnbull hangs there over the flames with one side crackling. 'Thank "
            "you,' he says round his mouthful. 'Very kind. The wire's at the back. Mind your hands, it has been in the fire since "
            "Tuesday.' Out in the dark something big is coming back, humming, with a bucket sloshing on its hip. 'Bonney can smell "
            "you near her supper,' it sings. 'Bonney can smell you touching it.'",
            [
                ch("Cut the wire and drag him clear.", SCAV, 45, "rescued", "the_fat"),
                ch("Heave the axle off the blocks, man and all.", ENDURE, 50, "rescued", "the_fall"),
                ch("Wait for her by the fire, and make her an offer.", NAV, 20, "the_offer", "the_ladle"),
            ]),
        "the_offer": node(
            "Bonney comes back into the firelight with a bucket of fat on her hip and looks at you, and at her supper, and at you. "
            "'Ohhh,' she says. 'You want him.' She sets the bucket down and thinks about it with her whole face, which takes some "
            "time. 'Fair's fair. Bonney'll swap. A toenail for the man. A big one, mind, off the big toe. And what's in his mouth, "
            "Bonney keeps. That's hers. That's afters.'",
            [
                ch("Sit down in the ash and take off your boot.", NAV, 0, "the_bargain", cost={"ll": 1}),
                ch("Go for the wire while she is still talking.", SCAV, 55, "rescued", "the_fat"),
            ]),
        "rescued": node(
            "The wire parts and Turnbull comes off the spit all at once, lands in the ash smoking, rolls, keeps rolling, and gets "
            "up. He takes {{apple}} out of his mouth, wipes it on his underwear and hands it to you, very formally. 'I believe "
            "this is yours. Or somebody's. Thank you so much.' Bonney screams. It goes on a long time and it is not a sound a "
            "throat should make. Then it stops, and she laughs, and wipes her eyes on her sleeve. 'Spoilt it,' she says. 'Spoilt "
            "Bonney's supper.' She tips the gravy on the fire, which goes up like a bomb, and when you can see again she is "
            "loading the bonnet, with Pinch riding on top. 'Never mind. Bonney'll go where there's more people. There's always more "
            "people.' She walks off into the dark. Turnbull, half done, shakes your hand for a long time and then sets off the "
            "other way at a run, smoking gently. In her hurry Bonney has left a jar behind.",
            [], bank=True, loot=[res(MED, 1)], table="dunes_rare", ending="rescue"),
        "the_bargain": node(
            "You sit down in the ash and take off your boot, and she takes the nail, the whole nail, between two of her too-long "
            "fingers, with one quick, practised twist. It goes in the snuff tin with the others. 'Lovely,' she says, and means it. "
            "She lifts the spit off the blocks, unwinds the wire and tips Turnbull out into the ash like a tray of roast potatoes. "
            "Then she takes {{apple}} out of his mouth and eats it. All of it. It goes down with a noise like a drain taking a "
            "sock. 'Afters,' she says, and pats her stomach, and her stomach pats back. Turnbull presses his water bottle on you "
            "as he runs past. By morning the fire is out and the bonnet is gone, and there is only a ring of ash where she was. "
            "She'll be somewhere with people in it. There are always more people.",
            [], bank=True, loot=[res(WATER, 1)], ending="bargain"),
        "walk_away": node(
            "You back out of the firelight. Turnbull watches you go as he comes round. 'Fair enough,' he says. 'I would too.' "
            "Bonney waves the aerial after you. 'He'll keep,' she calls. 'Bonney does him low and slow.'",
            [], escape=True),
    }
    hazards = {
        "the_heat": haz(
            "You sit too close. Bonney likes a fire you can see bones through. The heat comes at you like a wall, your eyebrows "
            "go, and the side of your face that was nearest comes up in one long blister.",
            {"ll": -1}),
        "the_drip": haz(
            "A gob of hot fat comes off the spit as it turns and lands on the back of your hand, and keeps cooking there. "
            "Turnbull says sorry. He seems to mean it.",
            {"ll": -1}, wound=[1, 0]),
        "pinch_bites": haz(
            "Pinch is not interested in shiny. Pinch is interested in your thumb, which it takes hold of with its own and bends "
            "back until something gives. Then it goes back to the crank as if nothing has happened.",
            {"ll": -1}, wound=[1, 0]),
        "the_crank": haz(
            "You kick the crank and it kicks back: a full turn of a truck axle with a man on it, straight across your shin.",
            {"ll": -1}, wound=[1, 0]),
        "the_gravy": haz(
            "'How she what?' says Bonney, right behind you, much too close. She has come back early. She ladles something hot and "
            "brown over your head to see how you take it, then licks her finger. 'Not bad,' she says. 'Needs salt.'",
            {"ll": -1, "food": -1}),
        "the_ladle": haz(
            "Bonney comes back in a mood and finds you standing over her supper. She hits you with the ladle, twice, without "
            "putting the bucket down, and goes on humming.",
            {"ll": -1}),
        "the_fat": haz(
            "The wire has been in the fire since Tuesday. You let go of it a great deal faster than you took hold of it, and "
            "leave some of your palm behind.",
            {"ll": -1}, wound=[1, 0]),
        "the_fall": haz(
            "An engine block shifts and the axle drops a foot, Turnbull and all, into the fire and out again. He says a word "
            "nobody says out here any more. You go down in the ash with your back on fire in a different way.",
            {"ll": -1}, wound=[1, 0]),
    }
    write("bonney/2.json", enc(2, "supper", "Supper at Bonney's", "the_fire", nodes, hazards,
                              {"apple": ["an apple, a real one, red and shining, the first you have seen in your life"]}))


section_1()
section_2()
