// ── The observer's voice ─────────────────────────────────────────
// Line banks only. No logic lives here; observer.js does the picking.
//
// THE VOICE IS NOT NEW. It is the one already written down in game-data.js
// above ADMIRED: dry, lowercase, one line; the joke is on the survivor;
// nothing is ever advice; never explain the number. TUNNEL_TAUNTS states it
// hardest — the joke is that the line is WRONG, and it stays on the
// survivor's own bad judgement rather than on any threat that is really out
// there. A line has drifted the moment it becomes a tip: "should have filled
// up at the marsh" is broken, "passed the marsh at speed, on principle" is
// not.
//
// ONE RULE DOES NOT CARRY ACROSS. ADMIRED is written entirely about the dead,
// so it is entirely past tense. The observer narrates the LIVING, in the
// PRESENT. Past tense is reserved for the dead — and then the tense shift
// does the work for free: the first time the narrator says "was" about
// somebody still on screen, the audience knows before the meter does. Every
// bank below is present tense except OBIT_*, which is past tense throughout.
//
// NO PRONOUNS FOR SURVIVORS. Six slots, six arbitrary characters, nothing on
// the wire says who any of them are. Lines are written name-first and
// pronoun-free; where a pronoun is unavoidable it is "they".
//
// KEYED kind x PHASE, not kind x severity. Severity already decides headline
// versus ticker; the phase decides MEANING. Pushing your luck in THE LEDGER
// is ambition and it is funny. Pushing it in THE REAPING is the ending. Where
// a beat genuinely means the same thing in both phases the bank is filed
// under ANY and the picker falls back to it — padding a REAPING bank with
// reworded LEDGER lines would buy nothing and cost the register.
//
// Slots, substituted by observer.js: {name} {arch} {n} {terrain} {item}
// {day} {score} {scene} {room} {risk} {haul}.
//
// Minimum four lines per bank — the anti-repeat ring holds the last
// min(3, len-1) picks, so three would leave exactly one legal choice and the
// bank would alternate audibly.
(function (root) {
  'use strict';
  const OBS = root.OBS || (root.OBS = {});

  OBS.LINES = {

    // ── Beat frame ────────────────────────────────────────────────
    // Establish fires on every cut, Leave on every cut-away. Both are
    // mandatory in both beat shapes; Leave is written KNOWING the beat is
    // over, which is where the comedy lands.
    ESTABLISH: {
      LEDGER: [
        '{name}. {arch}. {score} points of other people’s property.',
        'this is {name}, day {day}, {n} left and no complaints on record.',
        '{name} the {arch}, doing well enough to be worth the camera.',
        'over to {name}, who is up on the day and knows it.',
        '{name}, {score}, and a horizon full of opportunities to stop.',
      ],
      REAPING: [
        '{name}. {n} left. that is the headline.',
        'this is {name} now. it was going better.',
        '{name} the {arch}, day {day}, running on {n}.',
        '{name}, who is still here, technically.',
        'over to {name}. nothing has gone right since the {terrain}.',
      ],
    },
    LEAVE: {
      LEDGER: [
        'we leave {name} in good order. it will not last.',
        '{n} hexes today, and a rock.',
        'that is {name}. more later, probably.',
        'leaving {name} exactly where {name} wants to be.',
        'nothing further from {name}. the day is still young and so is the mistake.',
      ],
      REAPING: [
        'we leave {name} there. nobody is coming.',
        'and that is {name}, for now.',
        'leaving {name} to it. it is not going well.',
        'we look away. {name} carries on.',
        'that is where we leave {name}. the ground does not care either.',
      ],
    },

    // ── The scene ─────────────────────────────────────────────────
    THRESHOLD: {
      LEDGER: [
        '{name} steps into {scene}. day {day}, and still curious.',
        '{scene}. {name} goes in carrying {haul}.',
        '{name} has found a door and taken it personally.',
        'into {scene}, on the strength of no information at all.',
        '{name} goes inside. nothing about this was necessary.',
      ],
      REAPING: [
        '{name} goes into {scene} with {n} left. that is the whole plan.',
        '{scene}. {name} is not in a position to be choosy.',
        '{name} steps inside. outside was not working either.',
        'into {scene}, because the alternative was more walking.',
        '{name} goes in on {n}. the building has more than that.',
      ],
    },
    ROOM: {
      ANY: [
        '{room}. {name} reads it like a receipt.',
        'the {room} holds what it holds.',
        '{name} is in the {room} now. the way on is not cheap.',
        'nothing in the {room} is labelled.',
        '{name} stands in the {room} and considers the arithmetic.',
      ],
    },
    // The centrepiece. encCanBank says they may walk out with encLoot right
    // now; the next choice's base_risk says what walking deeper costs. Both
    // numbers on screen, and the audience watches somebody decide.
    CAN_LEAVE: {
      LEDGER: [
        'the door out is open. {name} is looking at the cage.',
        '{haul} in the bag and a clear way out. {name} is doing arithmetic.',
        '{name} could leave with {haul} right now. {risk} says otherwise.',
        'the exit is four steps behind. {name} has not turned round.',
        '{haul} is enough. {name} has never found enough convincing.',
      ],
      REAPING: [
        'the way out is open. {name} is worth more than {haul}.',
        '{haul}, and a door. this is the part that matters.',
        '{name} can walk out with {haul}. {risk} is not a suggestion.',
        'the door is open and {name} is still counting.',
        '{haul} and an exit, on {n}. the room is winning the argument.',
      ],
    },
    DOOR: {
      ANY: [
        '{name} gets through. the {room}, and it is quiet.',
        'through to the {room}. that door held.',
        'the {room} opens up. {name} is further in than before.',
        '{name} makes the {room}. nobody was stopping them.',
        'the way on works. {name} is in the {room}, deeper and pleased.',
      ],
    },
    DEEPER: {
      ANY: [
        '{haul} now. {name} is committed.',
        'the bag gets heavier. {name} does not.',
        '{name} takes {item} as well. why not.',
        'more in the bag, less in the tank.',
        '{haul}. none of it is water.',
      ],
    },
    // A hazard fired. The file's own prose beats every one of these, so these
    // only run when the signature could not be matched to a hazard.
    PRICE: {
      ANY: [
        'that costs {n}. {name} keeps going.',
        '{scene} takes {n} off {name} and carries on being a building.',
        '{n} for that. {name} is standing in exactly the same room.',
        'it goes wrong. {name} pays {n} and does not leave.',
        'something in there objects. {n}, and the door is still shut.',
      ],
    },
    TERMINAL: {
      ANY: [
        'that is all of it. {name} is standing in the last room.',
        'nothing further in. only out, with {haul}.',
        '{name} has cleared it. the building has no more opinions.',
        'the {room} is the end of it. {haul} and a walk.',
      ],
    },
    BANKED: {
      LEDGER: [
        '{name} walks out with {haul}. day {day}, and up to {score}.',
        'banked. {scene} is somebody else’s problem now.',
        '{name} takes the money. rare.',
        'out with {haul} and all the original limbs.',
        '{haul} out of {scene}, and {n} still in the tank. {name} calls that a margin.',
      ],
      REAPING: [
        '{name} gets out with {haul}. it will not be enough.',
        'banked {haul}. that buys days. not many.',
        'out alive and carrying. {name} is having a good hour.',
        '{haul} out of {scene}. spend it quickly.',
        '{name} comes out with {haul} and {n}. one of those numbers is the problem.',
      ],
    },
    LOST_IT: {
      ANY: [
        '{name} leaves {scene} with nothing. the bag was full a minute ago.',
        'all of it, gone at the door.',
        '{name} had {haul}. {name} has a story.',
        '{scene} takes it back. it was only ever a loan.',
        'out the way {name} came in, lighter, and not by choice.',
      ],
    },
    // The most under-rated beat in the game: a survivor standing in a room
    // full of medicine who decides it is not worth it. The stance of this
    // whole voice is bad judgement, so the one time somebody shows GOOD
    // judgement gets a line of its own.
    ABORTED: {
      ANY: [
        '{name} looks at the rest of it and leaves. good judgement, once.',
        '{name} walks out on purpose. nobody on this board has done that yet.',
        'the room still has things in it. {name} is outside.',
        '{name} decides it is not worth it. write that down.',
        '{name} backs out of {scene} with {haul} and no explanation.',
      ],
    },
    EXIT_DAWN: {
      ANY: [
        'dawn finds {name} still in there. the moment closes.',
        'the light comes up and {scene} stops being interesting.',
        'day {day} arrives mid-sentence.',
        '{name} runs out of night.',
      ],
    },
    // Second scene opens while the camera is locked. The show is allowed to
    // say "we will get to that."
    QUEUED: {
      ANY: [
        '{name} has found a door as well. we will get to that.',
        'elsewhere: {name} goes inside. hold that thought.',
        '{name} is in {scene} now too. one thing at a time.',
        'noted, {name}. stay in there.',
      ],
    },

    // ── Survival: the consequences the scene leaves behind ────────
    HURT: {
      LEDGER: [
        '{name} is down to {n}. no story attached.',
        'the {terrain} takes a little off {name}. it does that.',
        '{n} now. {name} has not noticed.',
        'small tax on the {terrain}. {name} pays it.',
      ],
      REAPING: [
        '{n}. that is what is left of {name}.',
        '{name} loses more on the {terrain}. there is less to lose from.',
        'down to {n}. the margin was the last thing {name} had.',
        'another one off {name}. the {terrain} is not even trying.',
      ],
    },
    HURT_BAD: {
      LEDGER: [
        '{name} takes a real one. {n} left, and a lesson unlearned.',
        'that hurt. {name} is on {n} and still walking.',
        '{name} drops to {n} in the {terrain}. day {day} is not over.',
        'the {terrain} makes its case. {name} is on {n}.',
      ],
      REAPING: [
        '{name} is on {n}. that is the whole of it.',
        'the {terrain} takes a serious piece. {n}.',
        '{name} is down to {n} and out of margin.',
        '{n} left. {name} has run out of room to be wrong in.',
      ],
    },
    FAILING: {
      ANY: [
        '{name} is under half. the ground is uphill from here.',
        'half gone. {name} has plans for the other half.',
        '{name} crosses into the part of the day nobody films.',
        'that is {name} below the line, on day {day}.',
      ],
    },
    MAULED: {
      ANY: [
        'something out there has teeth and {name} found it.',
        '{name} picks up a major wound in the {terrain}. nothing was chasing anyone.',
        'that is a bad one. {name} keeps the leg, mostly.',
        '{name} meets the {terrain} at close range.',
      ],
    },
    HUNGER: {
      LEDGER: [
        '{name} is out of food on day {day}. the {terrain} has none either.',
        'food gone. {name} is optimistic about the next hex.',
        '{name} eats the last of it and keeps walking.',
        'nothing left to eat. {name} has decided this is temporary.',
      ],
      REAPING: [
        '{name} has not eaten since day {day}.',
        'empty. {name} is running on {n} and opinion.',
        'no food, no shelter, no plan. {name} is consistent.',
        '{name} is hungry in a way that does not stop.',
      ],
    },
    THIRST: {
      LEDGER: [
        '{name} runs dry on day {day}. the plan was always the weather.',
        'no water. {name} is treating that as a detail.',
        '{name} finishes the water and says nothing.',
        'dry. {name} is counting on the weather.',
      ],
      REAPING: [
        '{name} has been dry since day {day}.',
        'no water and {n} left. one of those runs out first.',
        '{name} is thirsty. everything out here is.',
        'dry on day {day}, in the {terrain}, on {n}.',
      ],
    },
    GLOW: {
      ANY: [
        '{name} picks up more rad in the {terrain}. it does not itch yet.',
        'the count goes up. {name} walks on.',
        '{name} is carrying {n} of something that does not weigh anything.',
        'rads. {name} will worry about that later, or not at all.',
      ],
    },
    GROUND_TURNED: {
      ANY: [
        'the ground under {name} becomes {terrain}. nobody asked.',
        '{name} is standing on {terrain} now. it was not, a minute ago.',
        'the hex changes its mind. {terrain}, and {name} is on it.',
        'that is {terrain} under {name} now. the map was wrong.',
      ],
    },
    HAUL: {
      LEDGER: [
        '{name} is up {n}. day {day}, and the bag is honest work.',
        '{score}. {name} is doing this properly.',
        'that is {n} in one go. {name} has found something worth carrying.',
        '{name} clears {score}. none of it converts to water.',
      ],
      REAPING: [
        '{name} is up {n}, which changes nothing.',
        '{score} points and {n} left. the board is not the problem.',
        '{name} gets paid. the ledger is not the thing running out.',
        'another {n} for {name}. the numbers are going opposite ways.',
      ],
    },
    // Score crossed an ADMIRED row. The row's own line does the work; this is
    // only the lead-in, and it gets the headline to itself.
    PASSED_THE_DEAD: {
      ANY: [
        '{name} passes {scene}.',
        'that puts {name} above {scene}.',
        '{name} goes past {scene}, who is not moving.',
        '{scene}, passed, by {name}, on day {day}.',
      ],
    },
    ARRIVED: {
      ANY: [
        'somebody new. {name}, and no idea.',
        '{name} the {arch} joins, on a board that is already going.',
        '{name} arrives. day {day}. bad timing.',
        'a slot fills. {name}, {arch}, full tank.',
      ],
    },

    // ── The world ─────────────────────────────────────────────────
    DAWN: {
      LEDGER: [
        'day {day}. everyone still on the board.',
        'the light comes up on day {day} and finds everybody optimistic.',
        'day {day}. nobody has done anything irreversible yet.',
        'morning, day {day}. the wasteland files no report.',
      ],
      REAPING: [
        'day {day}. fewer of them.',
        'the light comes up on day {day}. it is not an improvement.',
        'day {day} starts. the board is shorter than it was.',
        'morning again. day {day}, and the arithmetic has not changed.',
      ],
    },
    WEATHER_TURN: {
      ANY: [
        '{terrain}. it arrives over everybody at once.',
        'the sky turns {terrain}. nobody is under anything.',
        '{terrain} now, across the whole ring.',
        'weather. {terrain}, and it has opinions about open ground.',
      ],
    },
    ESCALATION: {
      ANY: [
        'the clock moves. everything out there gets harder by the same amount.',
        'that is the threat clock at {n}. the doors all just got heavier.',
        '{n} on the clock. nothing looks different and everything is.',
        'the wasteland notices. {n}, and the odds shift under everyone.',
      ],
    },
    VANISHED: {
      ANY: [
        '{name} is gone from the board.',
        'the slot goes quiet. {name}.',
        '{name} stops. no further signal.',
        'and {name} is not there any more.',
      ],
    },

    // ── Obituaries ────────────────────────────────────────────────
    // PAST TENSE — the only banks that are. This is the ADMIRED register
    // exactly: admired not for surviving but for the manner of not
    // surviving, written up afterwards by people who were not there.
    // The ledger supplies {scene} and {room} wherever it has them.
    OBIT_SCENE: [
      '{name} died in {scene}. the {room} keeps what it took.',
      '{name} went into {scene} on day {day} and did not come back out of the {room}.',
      'died in the {room}, carrying {haul}, which nobody has collected.',
      '{name} made it as far as the {room}. admired for the distance.',
    ],
    OBIT_TEETH: [
      '{name} met something with teeth in the {terrain} on day {day}.',
      'something took {name} apart in the {terrain}. it did not need to be personal.',
      '{name} died of a wound that had a mouth. day {day}.',
      'the {terrain} had something living in it. {name} found out on day {day}.',
    ],
    OBIT_STARVED: [
      '{name} starved on day {day}, a short walk from nothing in particular.',
      'ran out of food on day {day} and kept walking. admired for the commitment.',
      '{name} died hungry on day {day}, having earned {score} and eaten none of it.',
      'starved in the {terrain}. {name} had {score} points and no dinner.',
    ],
    OBIT_THIRST: [
      '{name} died of thirst on day {day}. the marsh is still where it was.',
      'ran dry and kept going. {name}, day {day}, {score}.',
      '{name} died with {score} points and no water, which is the usual ratio.',
      'thirst got {name} in the {terrain} on day {day}. quick, at the end.',
    ],
    OBIT_GLOW: [
      '{name} died glowing, on day {day}, having felt fine for most of it.',
      'the count went up and {name} did not. day {day}.',
      '{name} carried it out of the {terrain} and no further.',
      'died of something that had no weight and no smell. admired for the confidence.',
    ],
    OBIT_UNKNOWN: [
      'cause recorded as the wasteland. the wasteland declined to comment.',
      '{name} stopped on day {day}. no note, no wound, no explanation.',
      'nobody saw it. {name}, day {day}, {score}, and then nothing.',
      '{name} is not on the board. that is all anyone is prepared to say.',
    ],
    // Not a death. LL was high, the slot just emptied — somebody's battery
    // died. This check runs FIRST in observer.js for exactly this reason:
    // nothing cheapens a death board faster than burying a quitter.
    OBIT_WALKED_OUT: [
      '{name} left the board in good health. no eulogy required.',
      '{name} walked away on {n}. the only sensible thing anyone has done.',
      'the slot empties. {name} was fine, which is its own kind of ending.',
      '{name} quit while ahead, at {score}. unprecedented.',
    ],
  };
}(typeof globalThis !== 'undefined' ? globalThis : this));
