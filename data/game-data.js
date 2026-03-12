// ── Constants (must match .ino) ─────────────────────────────────
const MAP_COLS    = 25;
const MAP_ROWS    = 19;
const MAX_PLAYERS = 6;
const VISION_R    = 1;   // base vision radius (server may send higher/lower via vr field)
const SQRT3       = Math.sqrt(3);
const LERP        = 0.26;

// ── 11 Terrain types ─────────────────────────────────────────────
// vis: +1=HIGH(+2 range), 0=STANDARD, -1=PENALTY(resources masked)
// mc : movement cost (255 = impassable)
// sv : shelter value
const TERRAIN = [
  { name:'Open Scrub',      mc:1,   sv:0, vis: 1, icon:'🌾',
    fill:'#2E2210', stroke:'#504030',   /* cracked-earth tan */
    tags:['HighVis'],
    hazard:'Radiation drift',
    desc:'Wind-scoured flats of pale scrub and cracked earth. Any resource may surface here. The open horizon grants exceptional sight lines — a survivor can see for kilometres.' },
  { name:'Ash Dunes',       mc:2,   sv:0, vis: 0, icon:'🏜',
    fill:'#201E16', stroke:'#3C3A2C',   /* desaturated ash grey */
    tags:['Radiation'],
    hazard:'Radioactive ash fall',
    desc:'Rolling dunes of grey volcanic ash laced with fallout. Fuel caches and scrap lie buried beneath the drifts. Prolonged exposure without a mask is hazardous.' },
  { name:'Rust Forest',     mc:2,   sv:1, vis:-1, icon:'🌲',
    fill:'#1A2808', stroke:'#344A18',   /* dark rust-tinged green */
    tags:['Forage','VisPenalty'],
    hazard:'Toxic spore clouds',
    desc:'Skeletal trees coated in rust-red fungus. The dense canopy blocks all sight lines. Foragers find food and scrap among the root networks, but visibility drops to near zero.' },
  { name:'Marsh',           mc:3,   sv:0, vis: 0, icon:'🌿',
    fill:'#081A10', stroke:'#183428',   /* very dark brackish green */
    tags:['Water','Hazard'],
    hazard:'Quicksand, infection',
    desc:'Brackish wetlands and salt flats. Water is abundant beneath the surface but heavily contaminated. Treacherous footing slows movement to a crawl. Avoid after dark.' },
  { name:'Broken Urban',    mc:2,   sv:1, vis:-1, icon:'🏚',
    fill:'#1A1814', stroke:'#34302A',   /* cold concrete grey */
    tags:['Salvage','VisPenalty'],
    hazard:'Structural collapse',
    desc:'Collapsed hab-blocks and fractured infrastructure. Salvage and medicine lie in the rubble. Every crumbled wall cuts line-of-sight. Watch for floor voids and gas pockets.' },
  { name:'Flooded District',mc:3,   sv:1, vis:-1, icon:'🌊',
    fill:'#08121E', stroke:'#142030',   /* cold steel blue-grey */
    tags:['Water','Hazard','VisPenalty'],
    hazard:'Submerged debris, current',
    desc:'Former city streets drowned under murky floodwater. Water is plentiful here, but stay clear of craters and glass fields or it will be tainted. Visibility drops to zero beneath the surface. Every step is blind.' },
  { name:'Glass Fields',    mc:3,   sv:0, vis: 1, icon:'✨',
    fill:'#121A22', stroke:'#243444',   /* iridescent cold blue */
    tags:['Salvage','HighVis','Radiation'],
    hazard:'Cutting edges, radiation',
    desc:'Fused earth and melted debris from a detonation event. The flat reflective surface gives an unobstructed view for kilometres. Scrap can be carefully extracted from the glass.' },
  { name:'Ridge',           mc:2,   sv:1, vis: 1, icon:'⛰',
    fill:'#1E1A12', stroke:'#3C3424',   /* warm slag-stone */
    tags:['HighGround','HighVis'],
    hazard:'Exposure, rockfall',
    desc:'Elevated ridgelines of compressed slag-stone. A superior vantage point — the surrounding terrain is visible in detail. Exposed to wind, lightning, and distant sight lines.' },
  { name:'Mountain',        mc:4,   sv:2, vis: 0, icon:'🗻',
    fill:'#14141C', stroke:'#28283A',   /* cold dark mineral */
    tags:['HighGround','Landmark'],
    hazard:'Altitude, rockslide',
    desc:'Towering slag-mountains and pre-war excavation sites. Heavy going, but caves and overhangs offer excellent shelter. Medicine and scrap can be found deep in the tunnels.' },
  { name:'Settlement',      mc:1,   sv:3, vis: 0, icon:'🏕',
    fill:'#1A1206', stroke:'#382814',   /* warm amber glow */
    tags:['Safe','Trade'],
    hazard:'None',
    desc:'A fortified survivor camp with trading posts and basic shelter. All resource types can be found or traded here. The only true safe zone on the wasteland.' },
  { name:'Nuke Crater',     mc:255, sv:0, vis: 0, icon:'☢',
    fill:'#0A0E04', stroke:'#1A2008',   /* scorched void, green-black */
    tags:['Impassable','Radiation'],
    hazard:'Lethal radiation, unstable ground',
    desc:'A direct-strike detonation crater. The ground is fused glass and irradiated rubble. Radiation at the rim is immediately lethal. No one goes in. No one comes back.' }
];
const NUM_TERRAIN = TERRAIN.length;

// Tag badge class names (matches .hi-badge.tag-X in style.css)
const TAG_CLASS = {
  'HighVis':    'hi-badge tag-HighVis',
  'VisPenalty': 'hi-badge tag-VisPenalty',
  'HighGround': 'hi-badge tag-HighGround',
  'Radiation':  'hi-badge tag-Radiation',
  'Forage':     'hi-badge tag-Forage',
  'Water':      'hi-badge tag-Water',
  'Hazard':     'hi-badge tag-Hazard',
  'Salvage':    'hi-badge tag-Salvage',
  'Landmark':   'hi-badge tag-Landmark',
  'Safe':       'hi-badge tag-Safe',
  'Trade':      'hi-badge tag-Trade',
  'Impassable': 'hi-badge tag-Impassable',
};

const RES_COLOR = ['','#2A5C8A','#4A7828','#8C4418','#7A1E1E','#5C5448'];
const RES_LABEL = ['','≈','#','Ω','+','%'];
const RES_NAMES = ['','Water','Food','Fuel','Medicine','Scrap'];
// Resource badge class names (matches .hi-badge.res-X in style.css)
const RES_BADGE_CLASS = ['','hi-badge res-water','hi-badge res-food','hi-badge res-fuel','hi-badge res-med','hi-badge res-scrap'];

const PLAYER_COLORS = [
  '#FF4444','#44FF44','#4488FF','#FFFF44','#FF44FF',
  '#44FFFF','#FF8844','#FF44AA','#AAFFAA','#AAAAFF'
];

// ── Action system constants (mirrors server ACT_* / AO_*) ────────
const ACT_FORAGE  = 0, ACT_WATER = 1, ACT_PURIFY = 2, ACT_SCAV = 3;
const ACT_SHELTER = 4, ACT_TREAT = 5, ACT_SURVEY  = 6, ACT_REST = 7;
const AO_BLOCKED = 0, AO_SUCCESS = 1, AO_PARTIAL = 2, AO_FAIL = 3;
const TC_MINOR = 0, TC_BLEED = 1, TC_FEVER = 2, TC_MAJOR = 3, TC_RAD = 4, TC_GRIEVOUS = 5;
const ACT_NAMES = ['FORAGE','COLLECT WATER','','SCAVENGE',
                   'BUILD SHELTER','TREAT','SURVEY','REST'];
const ACT_MP    = [2, 1, 1, 2, 3, 2, 1, 0];  // default MP cost per action
// Which terrain indices allow each action (matches server terrain arrays)
// Forage: Rust Forest(2), Marsh(3)
// Water:  Marsh(3), Flooded(5)
// Scavenge: Broken Urban(4), Glass Fields(6)
// Others: any terrain
const TERRAIN_FORAGE_DN  = [0,0,6,8,0,0,0,0,0,0,0];
const TERRAIN_SALVAGE_DN = [0,0,0,0,6,0,8,0,0,0,0];
const TERRAIN_HAS_WATER  = [0,0,0,1,0,1,0,0,0,0,0];
function actAvailable(actId, terrainIdx) {
  if (terrainIdx == null || terrainIdx > 10) return false;
  switch (actId) {
    case ACT_FORAGE:  return TERRAIN_FORAGE_DN[terrainIdx]  > 0;
    case ACT_WATER:   return TERRAIN_HAS_WATER[terrainIdx]  > 0;
    case ACT_SCAV:    return TERRAIN_SALVAGE_DN[terrainIdx] > 0;
    default:          return true;   // REST, TREAT, SHELTER, SURVEY available everywhere
  }
}

// ── Wayfarer archetypes (§9.4 Synergy Roles) ─────────────────────
// Indices 0-5 mirror server ARCHETYPE_NAME[] and slot assignment.
// skills: [Navigate, Forage, Scavenge, Treat, Shelter, Endure]  0=none 1=trained 2=expert
const ARCHETYPES = [
  {
    name: 'GUIDE',
    icon: '\u29BF',   // ⦿ crosshair
    color: '#FF6644',
    trait: 'Companions entering your hex via your movement pay MC\u22121 (min\u00a01).',
    skills: [2, 1, 0, 0, 1, 1],
    invSlots: 8,
    desc: 'Natural pathfinder. Leads allies through hostile terrain, reducing the movement cost for anyone following in their footsteps.',
    flavor: 'Points the way. Usually directly into trouble.'
  },
  {
    name: 'QUARTERMASTER',
    icon: '\u25A3',   // ▣ box
    color: '#FFAA22',
    trait: 'In a Camp (2+ Wayfarers), every 2 Food/Water consumed restores\u00a01\u00a0extra.',
    skills: [0, 2, 1, 1, 1, 0],
    invSlots: 8,
    desc: 'Supply expert. Stretches the group\'s rations when camped with other survivors. Trait is inactive when travelling solo.',
    flavor: 'Stretches rations until they\u2019re unrecognizable. Survival tastes like cardboard.'
  },
  {
    name: 'MEDIC',
    icon: '\u2764',   // ♥ heart
    color: '#44FF88',
    trait: 'May treat Major Wounds in the field at DN\u00a09. Each successful Treat\u00a0+1\u00a0Fatigue restored.',
    skills: [0, 0, 1, 2, 0, 2],
    invSlots: 8,
    desc: 'Field surgeon. Can stabilise serious wounds without a settlement, and each treatment restores extra stamina to the patient.',
    flavor: 'Patches survivors back together. What remains is... functional.'
  },
  {
    name: 'MULE',
    icon: '\u26BF',   // ⚿ key
    color: '#AAAAFF',
    trait: '12\u00a0inventory slots. May transfer items to allies in the same hex as a free action once per turn.',
    skills: [0, 1, 2, 0, 1, 1],
    invSlots: 12,
    desc: 'Pack carrier. Hauls twice the standard load and can redistribute supplies to teammates without spending MP.',
    flavor: 'Carries everything. Even the weight of everyone\u2019s poor decisions.'
  },
  {
    name: 'SCOUT',
    icon: '\u25CE',   // ◎ circle
    color: '#44FFFF',
    trait: 'Survey costs 0\u00a0MP. May Survey before moving on the same turn without consuming the action slot.',
    skills: [2, 1, 1, 0, 0, 1],
    invSlots: 8,
    desc: 'Recon specialist. Survey is free and combinable with movement, giving the group an early read on terrain ahead.',
    flavor: 'Gets there first. Doesn\u2019t always come back.'
  },
  {
    name: 'ENDURER',
    icon: '\u25D9',   // ◙ inverse circle
    color: '#FF44FF',
    trait: 'Endure skill treated as\u00a01\u00a0higher for all checks (does not affect Collective Endure).',
    skills: [1, 0, 0, 1, 2, 2],
    invSlots: 8,
    desc: 'Hardened survivor. Built to endure radiation, exhaustion and injury. The last one standing when conditions reach their worst.',
    flavor: 'Lasts longer than most. Which isn\u2019t saying much.'
  },
];

// Short skill labels for display
const SK_SHORT = ['Nav', 'For', 'Scav', 'Trt', 'Shel', 'End'];

// ── Skill check constants (mirrors server SK_* / SKILL_NAME) ─────
const SK_NAMES  = ['NAVIGATE','FORAGE','SCAVENGE','TREAT','SHELTER','ENDURE'];
// Suggested DN per skill index, indexed by terrain type 0-10
// terrain:        0   1   2   3   4   5   6   7   8   9  99
const SK_DN = [
  [ 5,  5,  7,  7,  7,  7,  5,  5,  7,  5, 99], // 0 Navigate
  [ 7,  9,  6,  8,  8,  8,  8,  8,  8,  8, 99], // 1 Forage
  [ 8,  8,  8,  8,  6,  8,  8,  8,  8,  8, 99], // 2 Scavenge
  [ 9,  9,  9,  9,  9,  9,  9,  9,  9,  9, 99], // 3 Treat (field default; items lower it)
  [ 8,  8,  8,  8,  8,  8,  8,  8,  8,  8, 99], // 4 Shelter (field default)
  [ 7,  7,  7,  7,  7,  7,  7,  7,  9,  7, 99], // 5 Endure (Mountain = severe)
];
function suggestDN(skill, terrain) {
  if (terrain == null || terrain > 10) return 7;
  return SK_DN[skill]?.[terrain] ?? 7;
}
