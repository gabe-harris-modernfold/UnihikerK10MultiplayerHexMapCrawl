#pragma once
// ── snd-music.hpp ────────────────────────────────────────────────────────────
// The composer. Generative, never looped: every bar is written as it is
// about to be played, from the state of the world at that moment.
//
// It is built like a film score that has to be written live:
//
//   STYLE    what kind of music this is -- the day's lonely theremin over a
//            drone, the night's music-box lullaby, the dark-carnival waltz,
//            the storm, the Doom's hunt, the tunnels, the held breath of an
//            encounter, a funeral. Each style is instrumentation, meter,
//            tempo range, harmonic language, and a pattern for every part.
//   TENSION  one number, 0..1, folded from the threat clock, the Doom, the
//            party's bodies, fire and weather. It moves the tempo, swaps the
//            mode (Aeolian -> harmonic minor -> Phrygian), picks darker
//            progressions, thickens the texture, and warps the tape.
//   STORY    events arrive as leitmotifs woven into the next bars in the
//            current key and tempo, so the music says what happened:
//              * the party theme has SIX notes, one owned by each survivor
//                slot. A survivor who is gone leaves a hole in it. At dawn it
//                is played as a roll call, each note on its owner's instrument.
//              * every survivor has a signature phrase: it announces them when
//                they join and it is what the music box plays when they die.
//              * the Creeping Doom is the minor-second pair; when it hunts,
//                the whole key sinks a semitone and the pair becomes the bass.
//              * the caravan is a carnival: its barker's tune is the waltz.
//              * the threat clock tolls its band on a bell.
//
// Parts: DRONE (held pedals), PAD (chords), BASS, LEAD (melody), COUNTER
// (ornament / answers / drips), PERC, TEX (wind, rain, fire, geiger: the
// soundscape), MOTIF (leitmotifs, on top of everything).
//
// Everything here runs inside the audio task (sndMusicBlock()). The game
// talks to it through the SndWorld snapshot and the story queue in
// snd-engine.hpp; nothing in this file touches game state.

// ── 1. Musical material ──────────────────────────────────────────────────────
static constexpr int PPQ = 24;                       // ticks per quarter note

struct SndScale { uint8_t n; int8_t iv[8]; };
static const SndScale SCALE_AEOLIAN   = { 7, { 0, 2, 3, 5, 7, 8, 10 } };
static const SndScale SCALE_DORIAN    = { 7, { 0, 2, 3, 5, 7, 9, 10 } };
static const SndScale SCALE_PHRYGIAN  = { 7, { 0, 1, 3, 5, 7, 8, 10 } };
static const SndScale SCALE_HARMMIN   = { 7, { 0, 2, 3, 5, 7, 8, 11 } };
static const SndScale SCALE_HUNGARIAN = { 7, { 0, 2, 3, 6, 7, 8, 11 } };   // the gypsy minor: two augmented seconds
static const SndScale SCALE_WHOLE     = { 6, { 0, 2, 4, 6, 8, 10 } };
static const SndScale SCALE_PENTMIN   = { 5, { 0, 3, 5, 7, 10 } };

static inline int musFloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
// scale degree (any integer; octaves wrap) -> MIDI
static inline int musDeg(int tonicMidi, const SndScale& s, int deg) {
  int oct = musFloorDiv(deg, s.n);
  return tonicMidi + oct * 12 + s.iv[deg - oct * s.n];
}
// MIDI -> nearest scale degree
static int musNearestDeg(int tonicMidi, const SndScale& s, int midi) {
  int rel = midi - tonicMidi, oct = musFloorDiv(rel, 12), pc = rel - oct * 12;
  int best = 0, bd = 99;
  for (int i = 0; i < s.n; i++) { int d = pc - s.iv[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
  if (12 - pc < bd) return (oct + 1) * s.n;
  return oct * s.n + best;
}
static inline int musPc(int midi) { int r = midi % 12; return r < 0 ? r + 12 : r; }

enum ChordQ : uint8_t {
  CQ_MIN = 0, CQ_MAJ, CQ_DIM, CQ_AUG, CQ_MIN7, CQ_DOM7, CQ_HDIM7, CQ_DIM7, CQ_MMAJ7,
  CQ_SUS2, CQ_SUS4, CQ_POW, CQ_MIN6, CQ_MADD9, CQ_PHRY, CQ_COUNT
};
static const int8_t CQ_T[CQ_COUNT][4] = {
  {0,3,7,-1}, {0,4,7,-1}, {0,3,6,-1}, {0,4,8,-1}, {0,3,7,10}, {0,4,7,10}, {0,3,6,10}, {0,3,6,9}, {0,3,7,11},
  {0,2,7,-1}, {0,5,7,-1}, {0,7,-1,-1}, {0,3,7,9}, {0,3,7,14}, {0,1,7,-1} };
static inline int cqN(uint8_t q) { int n = 0; for (int i = 0; i < 4; i++) if (CQ_T[q][i] >= 0) n++; return n; }

struct SndChord { int8_t root; uint8_t q; };        // root: semitones above the tonic
struct SndProg  { const SndChord* c; uint8_t n; };
#define CH(r, q) { (int8_t)(r), (uint8_t)(q) }
#define PROG(a) { a, (uint8_t)(sizeof(a) / sizeof(a[0])) }

// Progressions. i=0 ii=2 bII=1 III=3 iv=5 #iv=6 v/V=7 bVI=8 VII=10 vii=11.
// Day: bleak modal minor, no leading tone -- nothing out here resolves.
static const SndChord PA_L1[] = { CH(0,CQ_MIN), CH(8,CQ_MAJ), CH(3,CQ_MAJ), CH(10,CQ_MAJ) };
static const SndChord PA_L2[] = { CH(0,CQ_MIN), CH(5,CQ_MIN), CH(0,CQ_MIN), CH(7,CQ_MIN) };
static const SndChord PA_L3[] = { CH(0,CQ_MADD9), CH(10,CQ_MAJ), CH(8,CQ_MAJ), CH(10,CQ_SUS2) };
static const SndChord PA_L4[] = { CH(8,CQ_MAJ), CH(10,CQ_MAJ), CH(0,CQ_MIN), CH(0,CQ_SUS2) };
static const SndChord PA_M1[] = { CH(0,CQ_MIN), CH(8,CQ_MAJ), CH(5,CQ_MIN), CH(7,CQ_MAJ) };
static const SndChord PA_M2[] = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(0,CQ_MIN), CH(7,CQ_DOM7) };
static const SndChord PA_M3[] = { CH(0,CQ_MIN), CH(5,CQ_MIN6), CH(8,CQ_MAJ), CH(7,CQ_MAJ) };
static const SndChord PA_H1[] = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(11,CQ_DIM7), CH(0,CQ_MIN) };
static const SndChord PA_H2[] = { CH(0,CQ_MMAJ7), CH(0,CQ_MIN7), CH(0,CQ_MIN6), CH(8,CQ_MAJ) };
static const SndChord PA_H3[] = { CH(0,CQ_MIN), CH(6,CQ_DIM), CH(1,CQ_MAJ), CH(0,CQ_MIN) };
// Carnival waltz: eight-bar phrases, harmonic minor, the dominant seventh leaning hard.
static const SndChord PC_L1[] = { CH(0,CQ_MIN), CH(0,CQ_MIN), CH(7,CQ_DOM7), CH(7,CQ_DOM7), CH(7,CQ_DOM7), CH(7,CQ_DOM7), CH(0,CQ_MIN), CH(0,CQ_MIN) };
static const SndChord PC_L2[] = { CH(0,CQ_MIN), CH(5,CQ_MIN), CH(7,CQ_DOM7), CH(0,CQ_MIN), CH(8,CQ_MAJ), CH(2,CQ_HDIM7), CH(7,CQ_DOM7), CH(0,CQ_MIN) };
static const SndChord PC_M1[] = { CH(0,CQ_MIN), CH(7,CQ_DOM7), CH(0,CQ_MIN), CH(5,CQ_MIN), CH(0,CQ_MIN), CH(7,CQ_DOM7), CH(8,CQ_MAJ), CH(7,CQ_DOM7) };
static const SndChord PC_M2[] = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(7,CQ_DOM7), CH(0,CQ_MIN), CH(0,CQ_MIN), CH(11,CQ_DIM7), CH(7,CQ_DOM7), CH(0,CQ_MIN) };
static const SndChord PC_H1[] = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(11,CQ_DIM7), CH(11,CQ_DIM7), CH(7,CQ_DOM7), CH(0,CQ_MIN) };
static const SndChord PC_H2[] = { CH(0,CQ_MIN), CH(6,CQ_DIM7), CH(7,CQ_DOM7), CH(8,CQ_MAJ), CH(1,CQ_MAJ), CH(7,CQ_DOM7), CH(0,CQ_MMAJ7), CH(0,CQ_MIN) };
// Nocturne: a lullaby that knows too much.
static const SndChord PN_L1[] = { CH(0,CQ_MIN), CH(5,CQ_MIN), CH(7,CQ_MAJ), CH(0,CQ_MIN) };
static const SndChord PN_L2[] = { CH(0,CQ_MIN), CH(8,CQ_MAJ), CH(5,CQ_MIN), CH(7,CQ_MAJ) };
static const SndChord PN_M1[] = { CH(0,CQ_MIN), CH(3,CQ_AUG), CH(5,CQ_MIN), CH(7,CQ_DOM7) };
static const SndChord PN_H1[] = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(5,CQ_MIN), CH(11,CQ_DIM7) };
// Storm: Phrygian weight.
static const SndChord PS_L1[] = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(0,CQ_MIN), CH(1,CQ_MAJ) };
static const SndChord PS_L2[] = { CH(0,CQ_MIN), CH(10,CQ_MAJ), CH(8,CQ_MAJ), CH(7,CQ_MAJ) };
static const SndChord PS_H1[] = { CH(0,CQ_MIN), CH(5,CQ_MIN), CH(1,CQ_MAJ), CH(7,CQ_DOM7) };
// Chem: augmented chords planing a whole tone at a time. Nothing is home.
static const SndChord PX_L1[] = { CH(0,CQ_AUG), CH(2,CQ_AUG), CH(4,CQ_AUG), CH(2,CQ_AUG) };
static const SndChord PX_H1[] = { CH(0,CQ_AUG), CH(0,CQ_DIM7), CH(1,CQ_DIM7), CH(2,CQ_AUG) };
// Hunted: it is all the tonic and the semitone above it.
static const SndChord PH_1[]  = { CH(0,CQ_PHRY), CH(0,CQ_PHRY), CH(1,CQ_MAJ), CH(0,CQ_PHRY) };
static const SndChord PH_2[]  = { CH(0,CQ_MIN), CH(1,CQ_MAJ), CH(0,CQ_MIN), CH(6,CQ_DIM) };
// Tunnels, threshold, elegy, dawn.
static const SndChord PT_1[]  = { CH(0,CQ_MIN), CH(0,CQ_MIN), CH(8,CQ_MAJ), CH(0,CQ_MIN) };
static const SndChord PT_2[]  = { CH(0,CQ_SUS2), CH(10,CQ_MAJ), CH(0,CQ_MIN), CH(5,CQ_MIN) };
static const SndChord PE_1[]  = { CH(0,CQ_MIN), CH(5,CQ_MIN), CH(0,CQ_MIN), CH(7,CQ_MAJ), CH(8,CQ_MAJ), CH(5,CQ_MIN), CH(7,CQ_MAJ), CH(0,CQ_MIN) };
static const SndChord PD_1[]  = { CH(0,CQ_POW), CH(10,CQ_MAJ), CH(0,CQ_SUS2), CH(0,CQ_MIN) };
static const SndChord PQ_1[]  = { CH(0,CQ_MIN), CH(0,CQ_MIN), CH(0,CQ_MIN), CH(0,CQ_MIN) };

// ── 2. Leitmotifs ────────────────────────────────────────────────────────────
// A note is {semitones from the motif's reference pitch, duration in ticks};
// MOTIF_REST in .s is a rest.
static constexpr int8_t MOTIF_REST = 127;
struct SndMNote { int8_t s; uint8_t d; };

// The party: six notes, one per survivor slot (Guide, Quartermaster, Medic,
// Mule, Scout, Endurer), relative to the tonic in the lead octave. It ends on
// the leading tone and never resolves -- the road does not end.
static const SndMNote PARTY_THEME[6] = { {-5,24}, {0,24}, {3,48}, {2,12}, {0,12}, {-1,72} };
// Each survivor's instrument, and their octave offset for the roll call.
static const uint8_t SLOT_PATCH[6] = { P_CALLIOPE, P_MUSETTE, P_MUSICBOX, P_TUBA, P_WHISTLE, P_BELL };
static const int8_t  SLOT_OCT[6]   = { 0, 0, 12, -24, 12, -12 };
// Signatures, relative to the survivor's own party-theme note.
static const SndMNote SIG_GUIDE[]  = { {0,12}, {5,12}, {7,12}, {12,36} };               // a call outward
static const SndMNote SIG_QM[]     = { {0,8}, {0,8}, {0,8}, {2,24}, {0,24} };            // counting
static const SndMNote SIG_MEDIC[]  = { {0,12}, {2,12}, {3,24}, {2,12}, {0,36} };         // a turn, a lullaby
static const SndMNote SIG_MULE[]   = { {0,24}, {-5,24}, {-7,24}, {-12,48} };             // plodding down
static const SndMNote SIG_SCOUT[]  = { {0,6}, {3,6}, {5,6}, {7,6}, {10,12}, {7,36} };    // quick, gone
static const SndMNote SIG_ENDURE[] = { {0,36}, {8,36}, {7,48} };                         // stoic, a minor sixth
struct SndMotifRef { const SndMNote* n; uint8_t len; };
static const SndMotifRef SIGNATURE[6] = {
  { SIG_GUIDE, 4 }, { SIG_QM, 5 }, { SIG_MEDIC, 5 }, { SIG_MULE, 4 }, { SIG_SCOUT, 6 }, { SIG_ENDURE, 3 } };
// The caravan's barker tune, 3/4, eight bars around the fifth, relative to the tonic.
static const SndMNote CARAVAN_HOOK[] = {
  {7,24},{8,24},{7,24}, {6,24},{7,48}, {3,24},{2,24},{0,24}, {-1,24},{0,48},
  {0,24},{3,24},{7,24}, {12,24},{11,24},{8,24}, {7,24},{5,12},{3,12},{2,24}, {0,72} };
// Dawn: hollow fifths, then the roll call. Dusk: the music box going down.
static const SndMNote DAWN_CALL[]  = { {0,24}, {7,24}, {12,48}, {MOTIF_REST,96} };
static const SndMNote DUSK_FALL[]  = { {12,12}, {10,12}, {7,12}, {3,12}, {0,48} };

// ── 3. Styles ────────────────────────────────────────────────────────────────
enum MusStyle : uint8_t {
  MS_SILENT = 0, MS_ASHFALL, MS_NOCTURNE, MS_CARNIVAL, MS_STORM, MS_CHEM,
  MS_HUNTED, MS_TUNNELS, MS_THRESHOLD, MS_ELEGY, MS_DAWN, MS_IDLE, MS_COUNT
};
static const char* const MUS_STYLE_NAME[MS_COUNT] = {
  "silent", "ashfall", "nocturne", "carnival", "storm", "chem",
  "hunted", "tunnels", "threshold", "elegy", "dawn", "idle" };

enum MusPart : uint8_t { MP_DRONE = 0, MP_PAD, MP_BASS, MP_LEAD, MP_COUNTER, MP_PERC, MP_TEX, MP_MOTIF, MP_COUNT };
enum : uint8_t { DP_NONE = 0, DP_ROOT5, DP_PAIR, DP_TRITONE, DP_CHORDROOT };
enum : uint8_t { BP_NONE = 0, BP_OOM, BP_OSTINATO, BP_DOOM, BP_PEDAL };
enum : uint8_t { CP_NONE = 0, CP_PAD, CP_PAHPAH, CP_ARP, CP_TREMOLO, CP_CHOIR };
enum : uint8_t { LP_NONE = 0, LP_MELODY };
enum : uint8_t { CT_NONE = 0, CT_GLINT, CT_ANSWER, CT_DRIPS, CT_TICK };
enum : uint8_t { PP_NONE = 0, PP_WALTZ, PP_HEART, PP_TIMPANI, PP_TICK, PP_MARCH };

struct SndStyleDef {
  uint8_t beats, beatTicks;          // meter: 3x24 = 3/4, 4x24 = 4/4
  float   bpmLo, bpmHi;
  int8_t  tonicPc;                   // key
  const SndScale *scLo, *scMid, *scHi;
  uint8_t drone, bass, comp, lead, counter, perc;
  uint8_t pDrone, pPad, pBass, pLead, pLead2, pCounter;
  int8_t  leadLo, leadHi;
  float   density, leap, orn;        // melody: note density, leap chance, ornament chance
  float   gDrone, gPad, gBass, gLead, gCounter, gPerc;
  float   revT, revDamp, revWet, echoBeats, echoFb, echoWet;
  float   wow, worldCut;             // tape wow (semitones), world lowpass (Hz)
  uint8_t phrase;                    // bars per phrase
  SndProg lo[3], mid[3], hi[3];      // progression banks (n = 0 marks the end)
};

static const SndStyleDef STYLE[MS_COUNT] = {
  // MS_SILENT
  { 4,24, 60,60, 2, &SCALE_AEOLIAN,&SCALE_AEOLIAN,&SCALE_AEOLIAN, DP_NONE,BP_NONE,CP_NONE,LP_NONE,CT_NONE,PP_NONE,
    P_NONE,P_NONE,P_NONE,P_NONE,P_NONE,P_NONE, 60,84, 0,0,0, 0,0,0,0,0,0, 2,0.3f,0.3f,0.5f,0.3f,0.2f, 0,7000, 4,
    { PROG(PQ_1) }, { PROG(PQ_1) }, { PROG(PQ_1) } },
  // MS_ASHFALL -- the day: a drone, a harmonium, a theremin or someone whistling on the road
  { 4,24, 64,86, 2, &SCALE_AEOLIAN,&SCALE_HARMMIN,&SCALE_PHRYGIAN, DP_ROOT5,BP_NONE,CP_PAD,LP_MELODY,CT_GLINT,PP_NONE,
    P_DRONE,P_PAD,P_TUBA,P_THEREMIN,P_WHISTLE,P_MUSICBOX, 69,88, 0.42f,0.18f,0.08f,
    0.85f,0.8f,0.8f,0.9f,0.55f,0.7f, 3.0f,0.3f,0.52f,0.75f,0.36f,0.26f, 0.05f,7000, 4,
    { PROG(PA_L1), PROG(PA_L2), PROG(PA_L3) }, { PROG(PA_M1), PROG(PA_M2), PROG(PA_M3) }, { PROG(PA_H1), PROG(PA_H2), PROG(PA_H3) } },
  // MS_NOCTURNE -- night: a music-box lullaby under a choir
  { 3,24, 58,72, 2, &SCALE_HARMMIN,&SCALE_HARMMIN,&SCALE_PHRYGIAN, DP_ROOT5,BP_NONE,CP_ARP,LP_MELODY,CT_NONE,PP_NONE,
    P_DRONE,P_MUSICBOX,P_TUBA,P_HOLLOW,P_MUSICBOX,P_CHOIR, 72,88, 0.5f,0.12f,0.05f,
    0.6f,0.82f,0.8f,1.0f,0.5f,0.6f, 3.6f,0.25f,0.6f,1.5f,0.4f,0.3f, 0.09f,6500, 4,
    { PROG(PN_L1), PROG(PN_L2) }, { PROG(PN_M1), PROG(PN_L2) }, { PROG(PN_H1), PROG(PN_M1) } },
  // MS_CARNIVAL -- the caravan, and some nights: oom-pah-pah and a calliope that has lost its mind
  { 3,24, 112,152, 2, &SCALE_HARMMIN,&SCALE_HARMMIN,&SCALE_HUNGARIAN, DP_NONE,BP_OOM,CP_PAHPAH,LP_MELODY,CT_ANSWER,PP_WALTZ,
    P_NONE,P_PAH,P_TUBA,P_CALLIOPE,P_MUSETTE,P_MUSICBOX, 69,91, 0.78f,0.22f,0.32f,
    0.0f,0.85f,0.9f,1.0f,0.5f,0.6f, 1.8f,0.4f,0.34f,1.0f,0.3f,0.2f, 0.12f,7000, 8,
    { PROG(PC_L1), PROG(PC_L2) }, { PROG(PC_M1), PROG(PC_M2) }, { PROG(PC_H1), PROG(PC_H2) } },
  // MS_STORM -- an ostinato under a tremolo, timpani, and the wind
  { 4,24, 96,128, 2, &SCALE_PHRYGIAN,&SCALE_PHRYGIAN,&SCALE_PHRYGIAN, DP_ROOT5,BP_OSTINATO,CP_TREMOLO,LP_MELODY,CT_NONE,PP_TIMPANI,
    P_DRONE,P_PAD,P_TUBA,P_MUSETTE,P_THEREMIN,P_MUSICBOX, 69,88, 0.3f,0.25f,0.05f,
    0.8f,0.55f,0.85f,0.85f,0.4f,0.8f, 2.4f,0.35f,0.4f,0.5f,0.3f,0.18f, 0.04f,6000, 4,
    { PROG(PS_L1), PROG(PS_L2) }, { PROG(PS_L2), PROG(PS_H1) }, { PROG(PS_H1), PROG(PS_L1) } },
  // MS_CHEM -- whole tones, glass, a theremin sliding off the edge
  { 4,24, 66,80, 2, &SCALE_WHOLE,&SCALE_WHOLE,&SCALE_WHOLE, DP_NONE,BP_NONE,CP_CHOIR,LP_MELODY,CT_GLINT,PP_TICK,
    P_NONE,P_GLASS,P_TUBA,P_THEREMIN,P_THEREMIN,P_MUSICBOX, 70,92, 0.35f,0.4f,0.0f,
    0.0f,0.6f,0.0f,0.9f,0.5f,0.35f, 3.2f,0.3f,0.55f,0.75f,0.45f,0.3f, 0.22f,5500, 4,
    { PROG(PX_L1) }, { PROG(PX_L1), PROG(PX_H1) }, { PROG(PX_H1) } },
  // MS_HUNTED -- the key sinks a semitone; the Doom's pair becomes the floor
  { 4,24, 72,132, 1, &SCALE_PHRYGIAN,&SCALE_PHRYGIAN,&SCALE_PHRYGIAN, DP_PAIR,BP_DOOM,CP_NONE,LP_MELODY,CT_TICK,PP_HEART,
    P_DRONE,P_CHOIR,P_TUBA,P_GLASS,P_THEREMIN,P_TICK, 76,92, 0.16f,0.3f,0.0f,
    0.9f,0.5f,1.0f,0.7f,0.5f,0.9f, 2.0f,0.5f,0.42f,0.5f,0.25f,0.14f, 0.08f,5200, 4,
    { PROG(PH_1) }, { PROG(PH_1), PROG(PH_2) }, { PROG(PH_2) } },
  // MS_TUNNELS -- a fourth below the day, a long cave, water somewhere
  { 4,24, 54,62, 9, &SCALE_AEOLIAN,&SCALE_AEOLIAN,&SCALE_PHRYGIAN, DP_ROOT5,BP_NONE,CP_NONE,LP_MELODY,CT_DRIPS,PP_NONE,
    P_DRONE,P_CHOIR,P_TUBA,P_HOLLOW,P_HOLLOW,P_DRIP, 64,81, 0.2f,0.15f,0.0f,
    0.9f,0.4f,0.0f,0.65f,0.9f,0.5f, 5.2f,0.3f,0.7f,1.5f,0.55f,0.36f, 0.04f,3200, 4,
    { PROG(PT_1), PROG(PT_2) }, { PROG(PT_1) }, { PROG(PT_2) } },
  // MS_THRESHOLD -- an encounter: the world holds its breath
  { 4,24, 60,68, 2, &SCALE_PHRYGIAN,&SCALE_PHRYGIAN,&SCALE_PHRYGIAN, DP_TRITONE,BP_NONE,CP_NONE,LP_NONE,CT_GLINT,PP_HEART,
    P_DRONE,P_GLASS,P_TUBA,P_GLASS,P_GLASS,P_MUSICBOX, 76,88, 0,0,0,
    0.9f,0.45f,0.0f,0.0f,0.35f,0.7f, 2.8f,0.35f,0.5f,0.5f,0.3f,0.2f, 0.06f,6000, 4,
    { PROG(PQ_1) }, { PROG(PQ_1) }, { PROG(PQ_1) } },
  // MS_ELEGY -- someone is gone
  { 3,24, 50,56, 2, &SCALE_AEOLIAN,&SCALE_AEOLIAN,&SCALE_AEOLIAN, DP_ROOT5,BP_NONE,CP_CHOIR,LP_MELODY,CT_NONE,PP_NONE,
    P_CHOIR,P_CHOIR,P_TUBA,P_MUSICBOX,P_MUSICBOX,P_BELL, 72,86, 0.4f,0.1f,0.0f,
    0.6f,0.7f,0.0f,0.9f,0.6f,0.0f, 4.0f,0.25f,0.65f,1.5f,0.45f,0.3f, 0.1f,6500, 8,
    { PROG(PE_1) }, { PROG(PE_1) }, { PROG(PE_1) } },
  // MS_DAWN -- the chapter card: a grey sun, and the roll call
  { 4,24, 74,78, 2, &SCALE_DORIAN,&SCALE_DORIAN,&SCALE_AEOLIAN, DP_ROOT5,BP_NONE,CP_PAD,LP_NONE,CT_NONE,PP_NONE,
    P_DRONE,P_PAD,P_TUBA,P_CALLIOPE,P_WHISTLE,P_MUSICBOX, 69,88, 0,0,0,
    0.9f,0.85f,0.0f,0.9f,0.0f,0.0f, 3.2f,0.3f,0.55f,0.75f,0.36f,0.25f, 0.03f,7000, 4,
    { PROG(PD_1) }, { PROG(PD_1) }, { PROG(PD_1) } },
  // MS_IDLE -- nobody on the road: a carnival somewhere over the hill
  { 3,24, 104,110, 2, &SCALE_HARMMIN,&SCALE_HARMMIN,&SCALE_HARMMIN, DP_NONE,BP_OOM,CP_PAHPAH,LP_MELODY,CT_NONE,PP_WALTZ,
    P_NONE,P_PAH,P_TUBA,P_CALLIOPE,P_CALLIOPE,P_MUSICBOX, 69,88, 0.7f,0.2f,0.2f,
    0.0f,0.8f,0.9f,1.0f,0.5f,0.4f, 3.0f,0.45f,0.6f,1.0f,0.5f,0.5f, 0.16f,1100, 8,
    { PROG(PC_L1), PROG(PC_L2) }, { PROG(PC_L1) }, { PROG(PC_L2) } },
};

// ── 4. The world as the composer sees it ─────────────────────────────────────
// Filled by the board (ui-audio.hpp) ten times a second; every field is
// pre-normalised so this file holds no game rules.
struct SndWorld {
  uint8_t  slotMask;         // survivors connected, bit per slot
  uint8_t  aliveMask;        // ...and standing (LL > 0)
  float    dayFrac;          // 0 = dawn, 0.78 = dark
  uint16_t day;
  uint8_t  weather;          // 0 clear 1 rain 2 storm 3 chem 4 strangle fog 5 mist
  uint8_t  tcLevel, tcWeight, doomClose, doomAware;
  uint8_t  attrition, hunger, thirst, radLoad, woundLoad, fireClose;
  uint8_t  under, connected;
  bool     allUnder, encActive;
  uint8_t  caravanNear;      // 0..255, the caravan's closeness to the nearest survivor
  uint8_t  musicLevel;       // 0..9 user setting; 0 = no music or ambience
};

// ── 5. Composer state ────────────────────────────────────────────────────────
struct SndEvt { uint16_t t; uint8_t part, patch; int8_t note; uint8_t vel; uint16_t dur; uint8_t flags; };
enum : uint8_t { EVF_LEGATO = 1, EVF_HOLD = 2 };
static constexpr int MUS_EVT_MAX = 96;

struct SndMotifPlay {                  // a leitmotif being woven in
  const SndMNote* n; uint8_t len, i; uint8_t patch; int8_t ref; float tick; float vel;
  uint8_t slotPatch;                   // 1 = roll call: each note on its slot's instrument
  uint8_t skipMask;                    // roll call: slots that are gone (rests)
  float   speed;                       // 0.5 = half speed (elegy)
  bool    active;
};

struct SndMusic {
  // clock
  float    tick;                       // ticks into the current bar
  int      barTicks;
  float    bpm, bpmTgt;
  float    samplesPerTick;
  uint32_t bar, barInPhrase, phraseN;
  // harmony
  int      tonicPc, tonicLead, tonicBass, tonicMid;
  const SndScale* scale;
  SndChord chord;
  const SndProg* prog; uint8_t progIdx;
  // style
  uint8_t  style, want, prevStyle;
  bool     urgent;
  float    tension, tensionTgt;
  uint32_t styleBars;                  // bars since this style began
  uint32_t holdBars;                   // an override (dawn, elegy) lasts this many bars
  uint8_t  holdStyle;
  // bar events
  SndEvt   ev[MUS_EVT_MAX]; int evN, evI;
  // melody
  int      melLast, melDir;
  uint8_t  motN, motT[8], motD[8]; int8_t motStep[8]; bool motOk;
  uint8_t  lastRhy;
  // held voices
  SndVoice* drone[3]; uint32_t droneSer[3]; int droneNote[3];
  SndVoice* pad[4];   uint32_t padSer[4];
  SndVoice* lead;     uint32_t leadSer;
  SndVoice* wind;     uint32_t windSer;
  // leitmotif
  SndMotifPlay mot;
  bool     rollPending; uint8_t rollMask;   // dawn: the roll call waits for the call to finish
  SndChord padChord; bool padValid;         // the chord the held pad is voicing
  // tape
  float    wowPh1, wowPh2, wowDepth;
  float    tape, tapeTgt, tapeK;       // speed multiplier (wind-down)
  int32_t  tapeHoldBlocks;
  // world
  SndWorld w;
  bool     haveWorld;
  uint32_t rng;
  float    level;                      // 0..1 from musicLevel
  float    fireAcc, radAcc, rainAcc, dripAcc;
  uint8_t  lastWeather;
  bool     carnivalNight; uint16_t carnivalNightDay;
  uint32_t idleBars;
  float    lastTonicHz;
  float    windMul;                    // wind texture level multiplier (GET /snddbg?wind=)
  uint8_t  forceStyle;                 // < MS_COUNT: play this style whatever the world says (the sound desk)
  float    forceTension;               // >= 0: pin the tension (the sound desk); < 0 = the world decides
};
static SndMusic SMu;

// ── 6. Helpers ───────────────────────────────────────────────────────────────
static inline const SndStyleDef& musDef() { return STYLE[SMu.style]; }
static inline bool musVoiceIs(SndVoice* v, uint32_t ser) { return v && v->p && v->serial == ser; }

static void musAddEvt(uint16_t t, uint8_t part, uint8_t patch, int note, float vel, int dur, uint8_t flags = 0) {
  if (SMu.evN >= MUS_EVT_MAX || patch >= P_COUNT || vel <= 0.0f) return;
  if (note < 24 || note > 108) return;
  // keep the list sorted by time (insertion; bars hold a few dozen events)
  int i = SMu.evN++;
  while (i > 0 && SMu.ev[i - 1].t > t) { SMu.ev[i] = SMu.ev[i - 1]; i--; }
  SndEvt& e = SMu.ev[i];
  e.t = t; e.part = part; e.patch = patch; e.note = (int8_t)note;
  e.vel = (uint8_t)sndClampF(vel * 127.0f, 1.0f, 127.0f); e.dur = (uint16_t)(dur < 1 ? 1 : dur); e.flags = flags;
}

// chord tone pitch classes (relative to the tonic)
static int musChordPcs(const SndChord& c, int* out) {
  int n = 0;
  for (int i = 0; i < 4; i++) if (CQ_T[c.q][i] >= 0) out[n++] = musPc(c.root + CQ_T[c.q][i]);
  return n;
}
static bool musIsChordTone(int midi) {
  int pcs[4]; int n = musChordPcs(SMu.chord, pcs);
  int pc = musPc(midi - SMu.tonicPc);
  for (int i = 0; i < n; i++) if (pcs[i] == pc) return true;
  return false;
}
// nearest chord tone to `near`, within [lo, hi]
static int musNearestChordTone(int near, int lo, int hi) {
  int pcs[4]; int n = musChordPcs(SMu.chord, pcs);
  int best = near, bd = 99;
  for (int m = lo; m <= hi; m++) {
    int pc = musPc(m - SMu.tonicPc);
    for (int i = 0; i < n; i++) if (pcs[i] == pc) { int d = m - near; if (d < 0) d = -d; if (d < bd) { bd = d; best = m; } }
  }
  return best;
}
// step `steps` scale degrees from midi
static int musStep(int midi, int steps) {
  int d = musNearestDeg(SMu.tonicLead, *SMu.scale, midi);
  return musDeg(SMu.tonicLead, *SMu.scale, d + steps);
}
// close voicing of the current chord around `center`
static int musVoicing(int center, int* out, int maxN) {
  int pcs[4]; int n = musChordPcs(SMu.chord, pcs);
  if (n > maxN) n = maxN;
  for (int i = 0; i < n; i++) {
    int best = center, bd = 99;
    for (int m = center - 6; m <= center + 6; m++)
      if (musPc(m - SMu.tonicPc) == pcs[i]) { int d = m - center; if (d < 0) d = -d; if (d < bd) { bd = d; best = m; } }
    out[i] = best;
  }
  return n;
}
static inline int musRootMidi(int base) {           // the chord root at or above base
  int m = base + musPc(SMu.tonicPc + SMu.chord.root - base);
  return m;
}

// ── 7. Part generators ───────────────────────────────────────────────────────
static void musGenDrone() {
  const SndStyleDef& S = musDef();
  if (S.drone == DP_NONE || S.pDrone == P_NONE) { for (int i = 0; i < 3; i++) if (musVoiceIs(SMu.drone[i], SMu.droneSer[i])) sndNoteOff(SMu.drone[i]); return; }
  int root = SMu.tonicBass;                            // D3: the speaker can hear its harmonics
  int want[3] = { -1, -1, -1 };
  switch (S.drone) {
    case DP_ROOT5:     want[0] = root; want[1] = root + 7;
                       if (SMu.tension > 0.72f) want[2] = root + 1;            // the b2 creeps in
                       break;
    case DP_PAIR:      want[0] = root; want[1] = root + 1; want[2] = root + 7; break;
    case DP_TRITONE:   want[0] = root; want[1] = root + 6; break;
    case DP_CHORDROOT: want[0] = musRootMidi(root - 5); want[1] = want[0] + 7; break;
  }
  for (int i = 0; i < 3; i++) {
    bool alive = musVoiceIs(SMu.drone[i], SMu.droneSer[i]) && SMu.drone[i]->held;
    // same note on a different instrument (the elegy's choir over the day's
    // saws, both in D) is a new drone, not a held one
    if (alive && SMu.droneNote[i] == want[i] && SMu.drone[i]->p == &sndPatch[S.pDrone]) continue;
    if (alive) sndNoteOff(SMu.drone[i]);
    SMu.drone[i] = nullptr; SMu.droneNote[i] = want[i];
    if (want[i] < 0) continue;
    float vel = S.gDrone * (i == 2 ? 0.6f : 1.0f);
    SndVoice* v = sndNoteOn(SB_MUSIC, MP_DRONE, S.pDrone, (float)want[i], vel, -1);
    if (v) { SMu.drone[i] = v; SMu.droneSer[i] = v->serial;
             if (SMu.style == MS_TUNNELS) v->cutMul = 0.7f; }
  }
}

static void musGenPad() {
  const SndStyleDef& S = musDef();
  const int bt = S.beatTicks, bars = SMu.barTicks;
  int vo[4]; int n;
  switch (S.comp) {
    case CP_PAD: {
      // held chord; re-voiced only when the chord changes, and at each phrase
      bool same = SMu.padValid && SMu.padChord.root == SMu.chord.root && SMu.padChord.q == SMu.chord.q;
      if (same && SMu.barInPhrase != 0) break;
      SMu.padChord = SMu.chord; SMu.padValid = true;
      for (int i = 0; i < 4; i++) if (musVoiceIs(SMu.pad[i], SMu.padSer[i])) sndNoteOff(SMu.pad[i]);
      if (SMu.style == MS_ASHFALL && SMu.tension < 0.3f && sndChance(SMu.rng, 0.25f)) break;   // room to breathe
      n = musVoicing(SMu.tonicMid + 3, vo, 3);
      for (int i = 0; i < n; i++) {
        SndVoice* v = sndNoteOn(SB_MUSIC, MP_PAD, S.pPad, (float)vo[i], S.gPad * 0.8f, -1);
        if (v) { SMu.pad[i] = v; SMu.padSer[i] = v->serial; }
      }
      break;
    }
    case CP_CHOIR: {
      if (SMu.barInPhrase % 2) break;
      n = musVoicing(SMu.tonicMid + 2, vo, 3);
      for (int i = 0; i < n; i++) musAddEvt(0, MP_PAD, S.pPad, vo[i], S.gPad * 0.75f, bars * 2 - 6);
      break;
    }
    case CP_PAHPAH: {
      n = musVoicing(SMu.tonicMid + 2, vo, 3);
      for (int b = 1; b < S.beats; b++)
        for (int i = 0; i < n; i++)
          musAddEvt((uint16_t)(b * bt), MP_PAD, S.pPad, vo[i], S.gPad * (b == 1 ? 0.85f : 0.72f), bt / 2);
      break;
    }
    case CP_ARP: {
      // music-box broken chord in eighths: root 5th 10th 5th 3rd 5th
      int pcs[4]; int cn = musChordPcs(SMu.chord, pcs);
      int root = musRootMidi(SMu.tonicMid - 3);
      int third = root + ((cn > 1) ? musPc(pcs[1] - pcs[0]) : 7);
      int fifth = root + ((cn > 2) ? musPc(pcs[2] - pcs[0]) : 7);
      int pat[6] = { root, fifth, third + 12, fifth, third, fifth };
      int steps = S.beats * 2;
      for (int k = 0; k < steps; k++) {
        float vel = S.gPad * (k == 0 ? 0.9f : 0.62f) * (0.9f + 0.2f * sndRandF(SMu.rng));
        musAddEvt((uint16_t)(k * bt / 2), MP_PAD, S.pPad, pat[k % 6] + 12, vel, bt);
      }
      break;
    }
    case CP_TREMOLO: {
      n = musVoicing(SMu.tonicMid + 2, vo, 3);
      int step = bt / 4;                                     // sixteenths
      for (int t = 0; t < bars; t += step) {
        float swell = 0.45f + 0.55f * (float)t / bars;       // each bar swells
        for (int i = 0; i < n; i++)
          musAddEvt((uint16_t)t, MP_PAD, S.pPad, vo[i], S.gPad * 0.5f * swell, step - 1);
      }
      break;
    }
    default: break;
  }
}

static void musGenBass() {
  const SndStyleDef& S = musDef();
  if (S.bass == BP_NONE) return;
  const int bt = S.beatTicks;
  int root = musRootMidi(SMu.tonicBass);                  // D3..C#4: a toy speaker's idea of bass
  switch (S.bass) {
    case BP_OOM: {
      int note = (SMu.bar & 1) ? root + 7 : root;
      if (note > SMu.tonicBass + 12) note -= 12;
      musAddEvt(0, MP_BASS, S.pBass, note, S.gBass, bt - 4);
      // the last bar of a phrase walks chromatically down into the next downbeat
      if (SMu.barInPhrase == (uint32_t)(S.phrase - 1)) {
        musAddEvt((uint16_t)bt, MP_BASS, S.pBass, root - 1, S.gBass * 0.8f, bt - 4);
        if (S.beats > 2) musAddEvt((uint16_t)(2 * bt), MP_BASS, S.pBass, root - 2, S.gBass * 0.8f, bt - 4);
      }
      break;
    }
    case BP_OSTINATO: {                                     // Phrygian eighths
      static const int8_t OST[8] = { 0, 0, 1, 0, 0, 0, -2, 0 };
      int steps = S.beats * 2;
      for (int k = 0; k < steps; k++) {
        float acc = (k == 0) ? 1.0f : (k % 3 == 0 ? 0.85f : 0.62f);
        musAddEvt((uint16_t)(k * bt / 2), MP_BASS, S.pBass, root + OST[k % 8], S.gBass * acc, bt / 2 - 2);
      }
      break;
    }
    case BP_DOOM: {                                         // the pair: tonic and the semitone above
      float t = SMu.tension;
      int step = (t > 0.8f) ? bt / 4 : (t > 0.5f) ? bt / 2 : bt;
      int k = 0;
      for (int tt = 0; tt < SMu.barTicks; tt += step, k++) {
        float acc = (tt % bt == 0) ? 1.0f : 0.7f;
        musAddEvt((uint16_t)tt, MP_BASS, S.pBass, root + (k & 1), S.gBass * acc, step - 2);
      }
      break;
    }
    case BP_PEDAL:
      musAddEvt(0, MP_BASS, S.pBass, root, S.gBass * 0.8f, SMu.barTicks - 4);
      break;
  }
}

// The melody. Motif-and-variation, so it sounds composed rather than random:
// bar 1 of a phrase invents a rhythm and a shape, bar 2 repeats them on the
// new chord (a sequence), bar 3 contrasts, bar 4 cadences on a long note.
struct SndRhy { uint8_t n; uint8_t t[6]; uint8_t d[6]; };
static const SndRhy RHY3[] = {
  {3,{0,24,48},{24,24,24}}, {2,{0,48},{48,24}}, {3,{0,36,48},{36,12,24}}, {4,{0,12,24,48},{12,12,24,24}},
  {4,{0,24,36,48},{24,12,12,24}}, {4,{0,18,24,48},{18,6,24,24}}, {5,{0,8,16,24,48},{8,8,8,24,24}},
  {2,{24,48},{24,24}}, {1,{0},{72}} };
static const SndRhy RHY4[] = {
  {2,{0,48},{48,48}}, {3,{0,24,48},{24,24,48}}, {4,{0,24,48,72},{24,24,24,24}}, {4,{0,36,48,72},{36,12,24,24}},
  {5,{0,12,24,48,72},{12,12,24,24,24}}, {3,{0,72,84},{72,12,12}}, {2,{0,72},{72,24}},
  {2,{24,48},{24,48}}, {3,{48,60,72},{12,12,24}}, {1,{0},{96}} };
static constexpr int RHY3_N = sizeof(RHY3) / sizeof(RHY3[0]);
static constexpr int RHY4_N = sizeof(RHY4) / sizeof(RHY4[0]);

static void musGenLead() {
  const SndStyleDef& S = musDef();
  if (S.lead == LP_NONE || SMu.mot.active) return;
  const bool three = (S.beats == 3);
  const SndRhy* bank = three ? RHY3 : RHY4;
  const int nBank = three ? RHY3_N : RHY4_N;
  const int cadIdx = nBank - 1, sparseIdx = nBank - 2;          // RHY3[7] {24,48}, RHY4[8] {48,60,72}
  uint32_t bip = SMu.barInPhrase % 4;
  float dens = S.density * (1.0f - 0.35f * (SMu.w.hunger + SMu.w.thirst) / 510.0f);   // a starving tune thins
  int ri;
  if (bip == 3)                         ri = cadIdx;
  else if (bip == 1 && SMu.motOk && sndChance(SMu.rng, 0.72f)) ri = SMu.lastRhy;
  else if (sndChance(SMu.rng, 1.0f - dens)) ri = sparseIdx;
  else {
    ri = sndRandI(SMu.rng, nBank - 2);
    if (dens > 0.6f && bank[ri].n < 3) ri = sndRandI(SMu.rng, nBank - 2);
  }
  if (bip == 3 && three && sndChance(SMu.rng, 0.5f)) ri = 1;   // half + quarter cadence
  const SndRhy& R = bank[ri];
  // whole phrases of silence sometimes: the day is mostly wind
  if (bip == 0 && sndChance(SMu.rng, sndClampF(0.55f - dens, 0.0f, 0.5f))) { SMu.motOk = false; return; }

  uint8_t patch = ((SMu.phraseN & 1) && S.pLead2 != P_NONE) ? S.pLead2 : S.pLead;
  int lo = S.leadLo, hi = S.leadHi;
  if (SMu.melLast < lo || SMu.melLast > hi) SMu.melLast = musNearestChordTone((lo + hi) / 2, lo, hi);
  if (bip == 0) SMu.melDir = sndChance(SMu.rng, 0.5f) ? 1 : -1;
  if (bip == 2) SMu.melDir = -SMu.melDir;

  bool useMotif = (bip == 1 && SMu.motOk && ri == SMu.lastRhy);
  int startDeg = 0;
  if (useMotif) startDeg = musNearestDeg(SMu.tonicLead, *SMu.scale, musNearestChordTone(SMu.melLast + SMu.melDir * 2, lo, hi));
  int prevNote = SMu.melLast;
  const int bt = S.beatTicks;
  for (int j = 0; j < R.n; j++) {
    int t = R.t[j], d = R.d[j];
    bool strong = (t % bt == 0) && (t == 0 || (!three && t == 2 * bt));
    int note;
    if (useMotif && j < SMu.motN) {
      note = musDeg(SMu.tonicLead, *SMu.scale, startDeg + SMu.motStep[j]);
      if (strong && !musIsChordTone(note)) note = musNearestChordTone(note, lo, hi);
    } else if (bip == 3 && j == R.n - 1) {
      // cadence: the tonic at the end of an even phrase, otherwise leave it open
      int want = (SMu.phraseN & 1) ? SMu.tonicLead + 7 : SMu.tonicLead;
      while (want > hi) want -= 12; while (want < lo) want += 12;
      note = musNearestChordTone(want, lo, hi);
    } else if (strong) {
      note = musNearestChordTone(prevNote + SMu.melDir * 2, lo, hi);
    } else if (sndChance(SMu.rng, S.leap)) {
      note = musStep(prevNote, SMu.melDir * (2 + sndRandI(SMu.rng, 3)));
    } else {
      note = musStep(prevNote, SMu.melDir);
    }
    if (note > hi) { note -= 12; SMu.melDir = -1; }
    if (note < lo) { note += 12; SMu.melDir = 1; }
    // after a leap, turn back
    if (note - prevNote > 4) SMu.melDir = -1; else if (prevNote - note > 4) SMu.melDir = 1;
    float vel = S.gLead * (strong ? 0.95f : 0.78f) * (0.92f + 0.16f * sndRandF(SMu.rng));
    uint8_t fl = (sndPatch[patch].mono) ? EVF_LEGATO : 0;
    // ornaments: grace notes, trills, a chromatic run into the downbeat
    if (S.orn > 0 && t >= 3 && sndChance(SMu.rng, S.orn * 0.5f))
      musAddEvt((uint16_t)(t - 2), MP_LEAD, patch, note - 1, vel * 0.7f, 2, fl);
    if (S.orn > 0 && d >= 36 && sndChance(SMu.rng, S.orn * 0.6f)) {
      int up = musStep(note, 1);
      for (int k = 0; k < d / 2; k += 3)
        musAddEvt((uint16_t)(t + k), MP_LEAD, patch, (k / 3) & 1 ? up : note, vel * 0.85f, 3, fl);
      musAddEvt((uint16_t)(t + d / 2), MP_LEAD, patch, note, vel, d - d / 2 - 1, fl);
    } else if (S.orn > 0 && strong && t >= 12 && sndChance(SMu.rng, S.orn * 0.3f)) {
      for (int k = 3; k >= 1; k--) musAddEvt((uint16_t)(t - 4 * k), MP_LEAD, patch, note - k, vel * 0.7f, 4, fl);
      musAddEvt((uint16_t)t, MP_LEAD, patch, note, vel, d - 1, fl);
    } else {
      musAddEvt((uint16_t)t, MP_LEAD, patch, note, vel, d - 1, fl);
    }
    // remember bar 1 as the motif
    if (bip == 0 && j < 8) {
      if (j == 0) startDeg = musNearestDeg(SMu.tonicLead, *SMu.scale, note);
      SMu.motT[j] = (uint8_t)t; SMu.motD[j] = (uint8_t)d;
      SMu.motStep[j] = (int8_t)(musNearestDeg(SMu.tonicLead, *SMu.scale, note) - startDeg);
    }
    prevNote = note;
  }
  if (bip == 0) { SMu.motN = R.n; SMu.motOk = true; SMu.lastRhy = (uint8_t)ri; }
  SMu.melLast = prevNote;
}

static void musGenCounter() {
  const SndStyleDef& S = musDef();
  if (S.counter == CT_NONE || S.pCounter == P_NONE) return;
  const int bt = S.beatTicks;
  switch (S.counter) {
    case CT_GLINT: {
      int n = sndChance(SMu.rng, 0.35f + 0.3f * SMu.tension) ? 1 + sndRandI(SMu.rng, 2) : 0;
      for (int k = 0; k < n; k++) {
        int t = sndRandI(SMu.rng, S.beats * 2) * bt / 2;
        int note = musNearestChordTone(SMu.tonicLead + 12 + sndRandI(SMu.rng, 10), SMu.tonicLead + 7, SMu.tonicLead + 24);
        musAddEvt((uint16_t)t, MP_COUNTER, S.pCounter, note, S.gCounter * (0.6f + 0.3f * sndRandF(SMu.rng)), bt * 2);
      }
      break;
    }
    case CT_ANSWER: {
      // answer the lead in its gaps, a sixth below on the second instrument
      if (SMu.barInPhrase % 4 == 3 || sndChance(SMu.rng, 0.4f)) {
        int t = (S.beats - 1) * bt;
        int note = musNearestChordTone(SMu.melLast - 9, SMu.tonicMid, SMu.tonicLead + 7);
        musAddEvt((uint16_t)t, MP_COUNTER, S.pLead2, note, S.gCounter, bt - 2);
        musAddEvt((uint16_t)(t + bt / 2), MP_COUNTER, S.pLead2, musStep(note, -1), S.gCounter * 0.8f, bt / 2 - 1);
      }
      if (SMu.tension > 0.55f && sndChance(SMu.rng, 0.5f)) {
        int note = musNearestChordTone(SMu.tonicLead + 19, SMu.tonicLead + 12, SMu.tonicLead + 26);
        musAddEvt((uint16_t)(bt * sndRandI(SMu.rng, S.beats)), MP_COUNTER, S.pCounter, note, S.gCounter * 0.7f, bt * 2);
      }
      break;
    }
    case CT_DRIPS: {
      int n = sndRandI(SMu.rng, 3);
      for (int k = 0; k < n; k++) {
        int deg = sndRandI(SMu.rng, 8);
        int note = musDeg(SMu.tonicLead + 12, SCALE_PENTMIN, deg);
        musAddEvt((uint16_t)sndRandI(SMu.rng, SMu.barTicks), MP_COUNTER, S.pCounter, note, S.gCounter * (0.4f + 0.5f * sndRandF(SMu.rng)), 12);
      }
      break;
    }
    case CT_TICK: {
      if (SMu.w.tcLevel < 2 && SMu.style != MS_HUNTED) break;
      for (int b = 0; b < S.beats * 2; b++)
        musAddEvt((uint16_t)(b * bt / 2), MP_COUNTER, P_TICK, (b & 1) ? 91 : 96, S.gCounter * ((b & 1) ? 0.55f : 0.75f), 3);
      break;
    }
  }
}

static void musGenPerc() {
  const SndStyleDef& S = musDef();
  const int bt = S.beatTicks;
  float g = S.gPerc;
  switch (S.perc) {
    case PP_WALTZ:
      musAddEvt(0, MP_PERC, P_THUMP, 45, g * 0.7f, 6);
      for (int b = 1; b < S.beats; b++) musAddEvt((uint16_t)(b * bt), MP_PERC, P_HAT, 80, g * 0.55f, 4);
      if (SMu.barInPhrase == (uint32_t)(S.phrase - 1) && SMu.tension > 0.3f)
        for (int k = 0; k < 6; k++) musAddEvt((uint16_t)(2 * bt + k * 4), MP_PERC, P_SNARE, 60, g * (0.3f + 0.1f * k), 3);
      if (SMu.tension > 0.6f && sndChance(SMu.rng, 0.25f))
        musAddEvt((uint16_t)(bt * sndRandI(SMu.rng, S.beats)), MP_PERC, P_CHAIN, 84, g * 0.5f, 6);
      break;
    case PP_HEART: {
      // lub-dub, faster and heavier with the tension
      int every = (SMu.tension > 0.6f) ? bt : 2 * bt;
      for (int t = 0; t < SMu.barTicks; t += every) {
        musAddEvt((uint16_t)t, MP_PERC, P_THUMP, 43, g * 0.95f, 4);
        musAddEvt((uint16_t)(t + 5), MP_PERC, P_THUMP, 41, g * 0.6f, 4);
      }
      break;
    }
    case PP_TIMPANI:
      musAddEvt(0, MP_PERC, P_TOM, musRootMidi(SMu.tonicBass), g, 20);
      musAddEvt((uint16_t)(2 * bt), MP_PERC, P_TOM, musRootMidi(SMu.tonicBass) - 5, g * 0.8f, 20);
      if (SMu.barInPhrase == 3)
        for (int k = 0; k < 8; k++) musAddEvt((uint16_t)(3 * bt + k * 3), MP_PERC, P_TOM, musRootMidi(SMu.tonicBass), g * (0.35f + 0.08f * k), 3);
      break;
    case PP_TICK:
      for (int b = 0; b < S.beats; b++)
        if (sndChance(SMu.rng, 0.6f)) musAddEvt((uint16_t)(b * bt + sndRandI(SMu.rng, 6)), MP_PERC, P_TICK, 90 + sndRandI(SMu.rng, 8), g * 0.6f, 3);
      break;
    case PP_MARCH:
      musAddEvt(0, MP_PERC, P_THUMP, 45, g, 6);
      musAddEvt((uint16_t)(2 * bt), MP_PERC, P_THUMP, 45, g * 0.8f, 6);
      musAddEvt((uint16_t)bt, MP_PERC, P_SNARE, 60, g * 0.6f, 6);
      musAddEvt((uint16_t)(3 * bt), MP_PERC, P_SNARE, 60, g * 0.6f, 6);
      break;
    default: break;
  }
}

static void musWindDown(float depth, float seconds, float holdSec);

// The threat clock, made audible: from band 3 a tick-tock sits under whatever
// is playing, and at band 4 it is on every eighth. Hunted and the encounter
// already keep their own clocks.
static void musGenClock() {
  const SndStyleDef& S = musDef();
  if (SMu.w.tcLevel < 3 || SMu.style == MS_HUNTED || SMu.style == MS_THRESHOLD ||
      SMu.style == MS_SILENT || SMu.style == MS_IDLE || SMu.style == MS_ELEGY) return;
  const int bt = S.beatTicks;
  int step = (SMu.w.tcLevel >= 4) ? bt / 2 : bt;
  int k = 0;
  for (int t = 0; t < SMu.barTicks; t += step, k++)
    musAddEvt((uint16_t)t, MP_PERC, P_TICK, (k & 1) ? 89 : 94, (k & 1) ? 0.28f : 0.38f, 3);
}

// Tape lurches. The carousel stumbles at the end of a phrase -- pitch and
// tempo sag together for a beat, like a motor losing its grip -- more often
// and deeper as the dread climbs. The night's music box runs down the same
// way now and then. The day never lurches: the day is only tired.
static void musGenLurch() {
  bool carnival = (SMu.style == MS_CARNIVAL || SMu.style == MS_IDLE);
  bool box      = (SMu.style == MS_NOCTURNE || SMu.style == MS_ELEGY);
  if (!carnival && !box) return;
  const SndStyleDef& S = musDef();
  bool phraseEnd = (SMu.barInPhrase + 1 == (uint32_t)S.phrase) || (S.phrase == 8 && SMu.barInPhrase == 3);
  if (!phraseEnd || SMu.tapeHoldBlocks > 0) return;
  float p = carnival ? 0.22f + 0.45f * SMu.tension : 0.18f;
  if (!sndChance(SMu.rng, p)) return;
  float depth = carnival ? 0.93f - 0.1f * SMu.tension : 0.95f;
  float beat  = 60.0f / sndMaxF(SMu.bpm, 30.0f);
  musWindDown(depth, beat * 0.6f, beat * 0.5f);
}

// ── 8. Leitmotifs in play ────────────────────────────────────────────────────
static void musRollCall(uint8_t aliveMask);
static void musStartMotif(const SndMNote* n, uint8_t len, uint8_t patch, int ref, float vel,
                          float speed = 1.0f, uint8_t rollCall = 0, uint8_t skipMask = 0, float delayTicks = 0) {
  SndMotifPlay& m = SMu.mot;
  m.n = n; m.len = len; m.i = 0; m.patch = patch; m.ref = (int8_t)ref; m.tick = delayTicks; m.vel = vel;
  m.speed = speed; m.slotPatch = rollCall; m.skipMask = skipMask; m.active = true;
}

// Advance the motif by dt ticks; fires notes directly (not via the bar list),
// so it can start on any beat and run across bar lines.
static void musMotifTick(float dt) {
  SndMotifPlay& m = SMu.mot;
  if (!m.active) return;
  m.tick -= dt;
  while (m.active && m.tick <= 0.0f) {
    if (m.i >= m.len) { m.active = false; break; }
    const SndMNote& n = m.n[m.i];
    float durT = n.d / m.speed;
    if (n.s != MOTIF_REST) {
      uint8_t patch = m.patch; int note = m.ref + n.s;
      // a gone survivor's note is a rest: the roll call AND the day's hum
      bool skip = (m.skipMask >> (m.i % 6)) & 1;
      if (m.slotPatch) { patch = SLOT_PATCH[m.i % 6]; note += SLOT_OCT[m.i % 6]; }
      if (!skip) {
        int32_t gate = (int32_t)(durT * SMu.samplesPerTick * 0.95f);
        float vel = m.vel;
        if (m.slotPatch && (patch == P_TUBA || patch == P_BELL)) vel *= 0.9f;
        sndNoteOn(SB_MUSIC, MP_MOTIF, patch, (float)note, vel, gate);
      }
    }
    m.tick += durT;
    m.i++;
  }
}

// ── 9. Style, tension, key ───────────────────────────────────────────────────
static float musTension(const SndWorld& w) {
  float doom = w.doomClose / 255.0f;
  float tc   = w.tcWeight / 255.0f;
  float body = sndMaxF(sndMaxF(w.attrition, w.woundLoad), sndMaxF(sndMaxF(w.hunger, w.thirst), w.radLoad)) / 255.0f;
  float fire = w.fireClose / 255.0f;
  static const float WX_T[6] = { 0.0f, 0.1f, 0.38f, 0.42f, 0.3f, 0.1f };
  float wx = WX_T[w.weather < 6 ? w.weather : 0];
  float night = (w.dayFrac > 0.78f) ? 0.08f : 0.0f;
  float t = 0.45f * tc + 0.3f * sndMaxF(body * 0.7f, sndMaxF(fire * 0.6f, wx)) + night + 0.15f * (w.encActive ? 1 : 0);
  t = sndMaxF(t, doom * 0.95f);
  return sndClampF(t, 0.0f, 1.0f);
}

static uint8_t musPickStyle() {
  const SndWorld& w = SMu.w;
  if (SMu.forceStyle < MS_COUNT) return SMu.forceStyle;   // the sound desk auditioning a style
  if (SMu.holdBars > 0) return SMu.holdStyle;
  if (!SMu.haveWorld || w.musicLevel == 0) return MS_SILENT;
  if (w.connected == 0) return MS_IDLE;
  // the Doom outranks the rest, with hysteresis so it does not flap
  bool hunted = (SMu.style == MS_HUNTED) ? (w.doomClose >= 110) : (w.doomClose >= 150);
  if (hunted) return MS_HUNTED;
  if (w.encActive) return MS_THRESHOLD;
  if (w.allUnder) return MS_TUNNELS;
  if (w.weather == 2) return MS_STORM;
  if (w.weather == 3) return MS_CHEM;
  bool caravan = (SMu.style == MS_CARNIVAL) ? (w.caravanNear >= 120) : (w.caravanNear >= 170);
  if (caravan) return MS_CARNIVAL;
  bool night = (w.dayFrac >= 0.78f);          // dayFrac 0 is dawn: the day starts there
  if (night) {
    // Some nights the carnival comes out, decided once per night: never the
    // first (the lullaby introduces the dark), always the second (so everyone
    // meets it early), then about one night in three.
    if (SMu.carnivalNightDay != w.day + 1) {
      SMu.carnivalNightDay = (uint16_t)(w.day + 1);
      uint32_t h = (uint32_t)w.day + 0x9E3779B9u;                 // murmur3 finalizer
      h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
      SMu.carnivalNight = (w.day == 2) || (w.day > 2 && (h % 100) < 35);
    }
    return SMu.carnivalNight ? MS_CARNIVAL : MS_NOCTURNE;
  }
  return MS_ASHFALL;
}

// How soon a style may cut in: 2 = next bar (danger, grief, a new day),
// 1 = next two-bar boundary (weather, the caravan), 0 = next phrase (day/night).
static uint8_t musUrgency(uint8_t s) {
  switch (s) {
    case MS_HUNTED: case MS_THRESHOLD: case MS_ELEGY: case MS_DAWN: case MS_SILENT: return 2;
    case MS_STORM: case MS_CHEM: case MS_CARNIVAL: case MS_TUNNELS: case MS_IDLE: return 1;
    default: return 0;
  }
}

static void musApplyStyle(uint8_t s) {
  SMu.prevStyle = SMu.style;
  SMu.style = s; SMu.styleBars = 0; SMu.bar = 0; SMu.barInPhrase = 0;
  const SndStyleDef& S = STYLE[s];
  SMu.tonicPc   = S.tonicPc;
  SMu.tonicLead = 72 + S.tonicPc;                       // D5
  if (SMu.tonicLead > 76) SMu.tonicLead -= 12;
  SMu.tonicMid  = SMu.tonicLead - 12;                   // D4
  SMu.tonicBass = SMu.tonicLead - 24;                   // D3
  SMu.barTicks  = S.beats * S.beatTicks;
  SMu.motOk = false; SMu.prog = nullptr; SMu.progIdx = 0;
  // release the old style's held parts
  for (int i = 0; i < 4; i++) if (musVoiceIs(SMu.pad[i], SMu.padSer[i])) sndNoteOff(SMu.pad[i]);
  // the effect bus follows the style
  sndReverbSet(S.revT, S.revDamp, S.revWet);
  float beatSec = 60.0f / sndMaxF(S.bpmLo, 30.0f);
  sndEchoSet(beatSec * S.echoBeats, S.echoFb, S.echoWet);
  SM.worldCutTgt = S.worldCut;
  if (s == MS_SILENT) sndReleaseBus(SB_MUSIC, 1.5f);
}

// ── 10. The bar ──────────────────────────────────────────────────────────────
static void musNewBar() {
  // phrase bookkeeping first, so "at a phrase boundary" means this bar starts one
  if (SMu.barInPhrase >= musDef().phrase) { SMu.barInPhrase = 0; SMu.phraseN++; }
  // style switches land on phrase boundaries, or on the next bar if urgent
  uint8_t want = musPickStyle();
  if (want != SMu.style) {
    const SndStyleDef& S = musDef();
    bool atPhrase = (SMu.barInPhrase == 0) || (SMu.barInPhrase == 4 && S.phrase == 8);
    uint8_t u = musUrgency(want), uOld = musUrgency(SMu.style);
    // leaving an urgent style is itself urgent: the danger is over, say so now
    if (u == 2 || uOld == 2 || atPhrase || (u == 1 && (SMu.barInPhrase & 1) == 0) || SMu.forceStyle < MS_COUNT) {
      musApplyStyle(want);
    }
  }
  if (SMu.holdBars > 0) SMu.holdBars--;
  const SndStyleDef& S = musDef();

  // tension, tempo, mode
  SMu.tension += (SMu.tensionTgt - SMu.tension) * 0.25f;
  float t = SMu.tension;
  SMu.bpmTgt = S.bpmLo + (S.bpmHi - S.bpmLo) * t;
  if (SMu.style == MS_HUNTED) SMu.bpmTgt = S.bpmLo + (S.bpmHi - S.bpmLo) * (SMu.w.doomClose / 255.0f);
  if (SMu.w.tcLevel >= 4 && SMu.style == MS_CARNIVAL) SMu.bpmTgt *= 1.12f;   // the ride is out of control
  // a starving, thirsty party drags its feet, and the music with it
  SMu.bpmTgt *= 1.0f - 0.08f * (sndMaxF(SMu.w.hunger, SMu.w.thirst) / 255.0f);
  SMu.scale = (t < 0.34f) ? S.scLo : (t < 0.67f) ? S.scMid : S.scHi;

  // the progression: a new one at every phrase
  if (SMu.barInPhrase == 0 || !SMu.prog) {
    const SndProg* bank = (t < 0.34f) ? S.lo : (t < 0.67f) ? S.mid : S.hi;
    int n = 0; while (n < 3 && bank[n].n) n++;
    const SndProg* pick = &bank[n > 1 ? sndRandI(SMu.rng, n) : 0];
    if (pick == SMu.prog && n > 1) pick = &bank[(pick - bank + 1) % n];
    SMu.prog = pick; SMu.progIdx = 0;
  }
  SMu.chord = SMu.prog->c[SMu.progIdx % SMu.prog->n];
  SMu.progIdx++;
  // high tension darkens the chords as they come
  if (t > 0.6f && SMu.chord.q == CQ_DOM7 && sndChance(SMu.rng, 0.35f)) { SMu.chord.root = 11; SMu.chord.q = CQ_DIM7; }
  if (t > 0.7f && SMu.chord.root == 5 && sndChance(SMu.rng, 0.4f))     { SMu.chord.root = 1;  SMu.chord.q = CQ_MAJ; }

  SMu.evN = 0; SMu.evI = 0;
  if (SMu.style != MS_SILENT) {
    // Leitmotifs are decided first: while one plays, the generated lead rests.
    if (SMu.rollPending && !SMu.mot.active) {
      musRollCall(SMu.rollMask);
      SMu.rollPending = false;
    } else if (!SMu.mot.active && SMu.barInPhrase == 0) {
      // the day hums the party theme to itself now and then -- minus whoever is gone
      if (SMu.style == MS_ASHFALL && sndChance(SMu.rng, 0.14f))
        musStartMotif(PARTY_THEME, 6, P_THEREMIN, SMu.tonicLead, 0.6f, 1.0f, 0, (uint8_t)~SMu.w.aliveMask);
      // the caravan's own tune, when it really is the caravan playing
      else if (SMu.style == MS_CARNIVAL && (SMu.phraseN % 3 == 0) && SMu.w.caravanNear >= 120)
        musStartMotif(CARAVAN_HOOK, (uint8_t)(sizeof(CARAVAN_HOOK) / sizeof(CARAVAN_HOOK[0])), P_CALLIOPE,
                      SMu.tonicLead, 0.9f);
    }
    // the idle carnival plays in episodes: sixteen bars, then the wind alone
    bool quiet = (SMu.style == MS_IDLE) && ((SMu.styleBars / 16) % 3 != 0);
    musGenDrone();
    if (!quiet) { musGenPad(); musGenBass(); musGenLead(); musGenCounter(); musGenPerc(); }
    musGenClock();
    musGenLurch();
  }
  SMu.bar++; SMu.barInPhrase++; SMu.styleBars++;
}

// ── 11. The soundscape ───────────────────────────────────────────────────────
static void musTexture() {
  const SndWorld& w = SMu.w;
  bool on = SMu.haveWorld && w.musicLevel > 0 && SMu.style != MS_SILENT;
  // wind: always there, louder in weather, gone underground
  static const float WIND[6] = { 0.24f, 0.36f, 1.0f, 0.42f, 0.48f, 0.3f };
  float windTgt = on ? WIND[w.weather < 6 ? w.weather : 0] * (w.allUnder ? 0.15f : 1.0f) * SMu.windMul : 0.0f;
  if (SMu.style == MS_THRESHOLD) windTgt *= 0.4f;
  if (SMu.style == MS_CARNIVAL || SMu.style == MS_IDLE) windTgt *= 0.35f;   // the band drowns it out
  if (!musVoiceIs(SMu.wind, SMu.windSer)) {
    SMu.wind = nullptr;
    if (windTgt > 0.01f) {
      SndVoice* v = sndNoteOn(SB_MUSIC, MP_TEX, P_WIND, 60, 1.0f, -1);
      if (v) { SMu.wind = v; SMu.windSer = v->serial; v->level = 0.0f; }
    }
  }
  if (SMu.wind) {
    SMu.wind->level += (windTgt - SMu.wind->level) * 0.002f;
    float gust = 0.7f + 0.6f * sndRandF(SMu.rng);
    SMu.wind->cutMul += ((w.weather == 2 ? 1.6f : 1.0f) * gust - SMu.wind->cutMul) * 0.003f;
    if (windTgt <= 0.01f && SMu.wind->level < 0.01f) { sndNoteOff(SMu.wind); SMu.wind = nullptr; }
  }
  if (!on) return;
  const float dt = SND_BLK * SND_INV_SR;
  // rain on tin: pentatonic drips, so the rain is in the key
  float rainRate = (w.weather == 1) ? 3.5f : (w.weather == 2) ? 7.0f : (w.weather == 3) ? 2.0f : 0.0f;
  if (w.allUnder) rainRate = 0.0f;
  SMu.rainAcc += rainRate * dt;
  if (SMu.rainAcc > 1.0f || (rainRate > 0 && sndChance(SMu.rng, rainRate * dt * 0.3f))) {
    SMu.rainAcc = 0.0f;
    if (sndChance(SMu.rng, 0.7f)) {
      int note = musDeg(SMu.tonicLead + 12, SCALE_PENTMIN, sndRandI(SMu.rng, 8));
      sndNoteOn(SB_MUSIC, MP_TEX, P_DRIP, (float)note, 0.25f + 0.45f * sndRandF(SMu.rng), 400);
    }
  }
  // underground water
  if (w.allUnder) {
    SMu.dripAcc += 0.5f * dt;
    if (SMu.dripAcc > 1.0f && sndChance(SMu.rng, 0.02f)) {
      SMu.dripAcc = 0;
      int note = musDeg(SMu.tonicLead + 7, SCALE_PENTMIN, sndRandI(SMu.rng, 7));
      sndNoteOn(SB_MUSIC, MP_TEX, P_DRIP, (float)note, 0.6f, 500);
    }
  }
  // fire nearby crackles; radiation ticks
  float fireRate = (w.fireClose / 255.0f) * 14.0f;
  if (fireRate > 0 && sndChance(SMu.rng, fireRate * dt))
    sndNoteOn(SB_MUSIC, MP_TEX, P_CRACKLE, 60, 0.35f + 0.6f * sndRandF(SMu.rng), 60);
  // Something out there: before the Doom is close enough to take over the
  // score, it breathes under it now and then -- a low growl that swells and
  // goes, more often the closer it is. Never underground: it cannot follow.
  if (!w.allUnder && w.doomAware >= 51 && w.doomClose > 40 && SMu.style != MS_HUNTED) {
    float rate = (w.doomClose / 255.0f) * 0.12f;               // at most one every ~8 s
    if (sndChance(SMu.rng, rate * dt)) {
      SndVoice* g = sndNoteOn(SB_MUSIC, MP_TEX, P_GROWL, (float)(SMu.tonicBass + (sndChance(SMu.rng, 0.5f) ? 1 : 0)),
                              0.35f + 0.4f * (w.doomClose / 255.0f), (int32_t)(1.6f * SND_SR));
      if (g) { g->bendTo = -1.0f; g->bendK = 0.004f; }
    }
  }
  float radRate = (w.radLoad / 255.0f) * 9.0f;
  if (radRate > 0.3f && sndChance(SMu.rng, radRate * dt * (sndChance(SMu.rng, 0.1f) ? 6.0f : 1.0f)))
    sndNoteOn(SB_MUSIC, MP_TEX, P_GEIGER, 60, 0.5f + 0.5f * sndRandF(SMu.rng), 30);
}

// Tape: wow always, a flutter under dread, and the occasional wind-down.
static void musTape() {
  const SndStyleDef& S = musDef();
  float dread = SMu.tension;
  SMu.wowDepth += ((S.wow * (1.0f + 1.8f * dread)) - SMu.wowDepth) * 0.002f;
  SMu.wowPh1 += 0.53f * SND_BLK * SND_INV_SR; if (SMu.wowPh1 >= 1.0f) SMu.wowPh1 -= 1.0f;
  SMu.wowPh2 += 1.71f * SND_BLK * SND_INV_SR; if (SMu.wowPh2 >= 1.0f) SMu.wowPh2 -= 1.0f;
  float wow = SMu.wowDepth * (0.65f * sndSinP(SMu.wowPh1) + 0.35f * sndSinP(SMu.wowPh2));
  if (SMu.tapeHoldBlocks > 0) { if (--SMu.tapeHoldBlocks == 0) SMu.tapeTgt = 1.0f; }
  SMu.tape += (SMu.tapeTgt - SMu.tape) * SMu.tapeK;
  SC.busPitch[SB_MUSIC] = wow;
  SC.busSpeed[SB_MUSIC] = SMu.tape;
}
static void musWindDown(float depth, float seconds, float holdSec) {
  SMu.tapeTgt = depth;
  SMu.tapeK = sndEnvK(seconds, 3.0f);
  SMu.tapeHoldBlocks = (int32_t)((seconds + holdSec) * SND_KRATE);
}

// ── 12. Per block ────────────────────────────────────────────────────────────
static void sndMusicInit(uint32_t seed) {
  memset(&SMu, 0, sizeof(SMu));
  SMu.rng = seed ? seed : 0xC0FFEEu;
  SMu.style = MS_SILENT; SMu.scale = &SCALE_AEOLIAN;
  SMu.barTicks = 96; SMu.bpm = SMu.bpmTgt = 70;
  SMu.tape = SMu.tapeTgt = 1.0f; SMu.tapeK = 0.01f;
  SMu.melLast = 74; SMu.melDir = 1;
  SMu.tonicPc = 2; SMu.tonicLead = 74; SMu.tonicMid = 62; SMu.tonicBass = 50;
  SMu.tick = 1e9f;                                     // start a bar on the first block
  SMu.windMul = 1.0f;
  SMu.forceStyle = 0xFF; SMu.forceTension = -1.0f;
}

static void sndMusicBlock() {
  musTape();
  musTexture();
  SMu.bpm += (SMu.bpmTgt - SMu.bpm) * 0.004f;
  float ticksPerSec = SMu.bpm * PPQ / 60.0f * SMu.tape;
  SMu.samplesPerTick = SND_SR / sndMaxF(ticksPerSec, 1.0f);
  float dt = ticksPerSec * SND_BLK * SND_INV_SR;
  SMu.tick += dt;
  musMotifTick(dt);
  if (SMu.tick >= SMu.barTicks) {
    SMu.tick = (SMu.tick > 1e8f) ? 0.0f : SMu.tick - SMu.barTicks;
    musNewBar();
  }
  // fire this bar's events that are due
  while (SMu.evI < SMu.evN && SMu.ev[SMu.evI].t <= SMu.tick) {
    const SndEvt& e = SMu.ev[SMu.evI++];
    int32_t gate = (int32_t)(e.dur * SMu.samplesPerTick);
    float vel = e.vel * (1.0f / 127.0f);
    if (e.part == MP_LEAD) {
      SndVoice* prev = musVoiceIs(SMu.lead, SMu.leadSer) ? SMu.lead : nullptr;
      SndVoice* v = sndNoteOn(SB_MUSIC, MP_LEAD, e.patch, (float)e.note, vel, gate,
                              (e.flags & EVF_LEGATO) ? prev : nullptr);
      if (v) { SMu.lead = v; SMu.leadSer = v->serial; }
    } else {
      sndNoteOn(SB_MUSIC, e.part, e.patch, (float)e.note, vel, gate);
    }
  }
}

// ── 13. What the story does to the music ─────────────────────────────────────
static void musHold(uint8_t style, uint32_t bars) {
  SMu.holdStyle = style; SMu.holdBars = bars;
  musApplyStyle(style);
  SMu.tick = (float)SMu.barTicks;                       // start it on the next block
}
static void musRollCall(uint8_t aliveMask) {
  musStartMotif(PARTY_THEME, 6, P_CALLIOPE, SMu.tonicLead, 0.8f, 1.0f, 1, (uint8_t)~aliveMask);
}
static void musDawn(uint8_t aliveMask) {
  musHold(MS_DAWN, 7);
  // the call first; then the roll call, where every living survivor answers
  // on their own instrument and the dead leave their note unplayed
  musStartMotif(DAWN_CALL, 4, P_CALLIOPE, SMu.tonicLead, 0.85f);
  SMu.rollPending = true; SMu.rollMask = aliveMask;
}
static void musJoin(uint8_t slot) {
  if (slot >= 6) return;
  int ref = SMu.tonicLead + PARTY_THEME[slot].s + SLOT_OCT[slot];
  if (SLOT_OCT[slot] < -12) ref += 12;                  // the Mule's signature already plods down
  musStartMotif(SIGNATURE[slot].n, SIGNATURE[slot].len, SLOT_PATCH[slot], ref, 0.85f, 1.0f, 0, 0, 8.0f);
}
static void musElegy(uint8_t slot) {
  if (slot >= 6) return;
  musHold(MS_ELEGY, 8);
  int ref = SMu.tonicLead + PARTY_THEME[slot].s;
  musStartMotif(SIGNATURE[slot].n, SIGNATURE[slot].len, P_MUSICBOX, ref, 0.9f, 0.5f);
  musWindDown(0.93f, 3.0f, 1.0f);                        // the music box winding down
}
static void musDoomPulse() {                              // the Doom's figure, as an accent in the music
  int root = SMu.tonicBass;
  int32_t gate = (int32_t)(PPQ * SMu.samplesPerTick);
  sndNoteOn(SB_MUSIC, MP_BASS, P_GROWL, (float)root, 0.8f, gate);
  sndNoteOn(SB_MUSIC, MP_PERC, P_THUMP, 43, 0.9f, 400);
}
