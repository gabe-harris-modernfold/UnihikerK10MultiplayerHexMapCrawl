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
    tags:['Open Horizon','Forage','Hunting Ground'],
    hazard:'Radiation drift',
    desc:'Wind-scoured flats of pale scrub and cracked earth. Small game — birds, feral rabbits, scavenger rodents — move through the open ground. A successful hunt yields 2 food. The open horizon grants long sight lines, but the animals can see you coming.' },
  { name:'Ash Dunes',       mc:2,   sv:0, vis: 0, icon:'🏜',
    fill:'#201E16', stroke:'#3C3A2C',   /* desaturated ash grey */
    tags:['Radiation'],
    hazard:'Radioactive ash fall',
    desc:'Rolling dunes of grey volcanic ash laced with fallout. Fuel caches and scrap lie buried beneath the drifts. Prolonged exposure without a mask is hazardous.' },
  { name:'Rust Forest',     mc:2,   sv:1, vis:-1, icon:'🌲',
    fill:'#1A2808', stroke:'#344A18',   /* dark rust-tinged green */
    tags:['Forage','Wild Game','Blind Ground'],
    hazard:'Toxic spore clouds',
    desc:'Skeletal trees coated in rust-red fungus. The dense canopy blocks all sight lines. A full Forage success here yields 2 food — the root networks are rich with edible fungi. Partial success still yields 1. Visibility drops to near zero.' },
  { name:'Marsh',           mc:3,   sv:0, vis: 0, icon:'🌿',
    fill:'#081A10', stroke:'#183428',   /* very dark brackish green */
    tags:['Water','Treacherous'],
    hazard:'Quicksand, infection',
    desc:'Brackish wetlands and salt flats. Water is abundant beneath the surface but undrinkable. Treacherous footing slows movement to a crawl. Avoid after dark.' },
  { name:'Broken Urban',    mc:2,   sv:1, vis:-1, icon:'🏚',
    fill:'#1A1814', stroke:'#34302A',   /* cold concrete grey */
    tags:['Salvage','Blind Ground'],
    hazard:'Structural collapse',
    desc:'Collapsed hab-blocks and fractured infrastructure. Salvage and medicine lie in the rubble. Every crumbled wall cuts line-of-sight. Watch for floor voids and gas pockets.' },
  { name:'Flooded District',mc:3,   sv:1, vis:-1, icon:'🌊',
    fill:'#08121E', stroke:'#142030',   /* cold steel blue-grey */
    tags:['Water','Treacherous','Blind Ground'],
    hazard:'Submerged debris, current',
    desc:'Former city streets drowned under murky floodwater. Water is plentiful here, but stay clear of craters and glass fields or it will be tainted. Visibility drops to zero beneath the surface. Every step is blind.' },
  { name:'Glass Fields',    mc:3,   sv:0, vis: 1, icon:'✨',
    fill:'#121A22', stroke:'#243444',   /* iridescent cold blue */
    tags:['Salvage','Open Horizon','Radiation'],
    hazard:'Cutting edges, radiation',
    desc:'Fused earth and melted debris from a detonation event. The flat reflective surface gives an unobstructed view for kilometres. Scrap can be carefully extracted from the glass.' },
  { name:'Ridge',           mc:2,   sv:1, vis: 1, icon:'⛰',
    fill:'#1E1A12', stroke:'#3C3424',   /* warm slag-stone */
    tags:['Vantage','Open Horizon'],
    hazard:'Exposure, rockfall',
    desc:'Elevated ridgelines of compressed slag-stone. A superior vantage point — the surrounding terrain is visible in detail. Exposed to wind, lightning, and distant sight lines.' },
  { name:'Mountain',        mc:4,   sv:2, vis: 0, icon:'🗻',
    fill:'#14141C', stroke:'#28283A',   /* cold dark mineral */
    tags:['Vantage','Waypoint'],
    hazard:'Altitude, rockslide',
    desc:'Towering slag-mountains and pre-war excavation sites. Heavy going, but caves and overhangs offer excellent shelter. Medicine and scrap can be found deep in the tunnels.' },
  { name:'Settlement',      mc:1,   sv:3, vis: 0, icon:'🏕',
    fill:'#1A1206', stroke:'#382814',   /* warm amber glow */
    tags:['Haven','Barter'],
    hazard:'None',
    desc:'A fortified survivor camp with trading posts and basic shelter. All resource types can be found or traded here. The only true safe zone on the wasteland.' },
  { name:'Nuke Crater',     mc:255, sv:0, vis: 0, icon:'☢',
    fill:'#0A0E04', stroke:'#1A2008',   /* scorched void, green-black */
    tags:['Dead Zone','Radiation'],
    hazard:'Lethal radiation, unstable ground',
    desc:'A direct-strike detonation crater. The ground is fused glass and irradiated rubble. Radiation at the rim is immediately lethal. No one goes in. No one comes back.' },
  { name:'River Channel',   mc:255, sv:0, vis:-3, icon:'〰',
    fill:'#0B1E0F', stroke:'#162B18',   /* brackish murky green — wasteland water */
    tags:['Impassable'],
    hazard:'Impassable current',
    desc:'A fast-moving river cutting through the wasteland. The current is too dangerous to cross. Navigate around it or find a ford. Water is visible but unreachable from the banks.' }
];
const NUM_TERRAIN = TERRAIN.length;

// Tag badge class names (matches .hi-badge.tag-X in style.css)
const TAG_CLASS = {
  'Open Horizon':  'hi-badge tag-HighVis',
  'Blind Ground':  'hi-badge tag-VisPenalty',
  'Vantage':       'hi-badge tag-HighGround',
  'Radiation':     'hi-badge tag-Radiation',
  'Forage':          'hi-badge tag-Forage',
  'Wild Game':       'hi-badge tag-Forage',
  'Hunting Ground':  'hi-badge tag-Forage',
  'Water':         'hi-badge tag-Water',
  'Treacherous':   'hi-badge tag-Hazard',
  'Salvage':       'hi-badge tag-Salvage',
  'Waypoint':      'hi-badge tag-Landmark',
  'Haven':         'hi-badge tag-Safe',
  'Barter':        'hi-badge tag-Trade',
  'Dead Zone':     'hi-badge tag-Impassable',
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

// Archetype ring colours (indexed 0-5, matching ARCHETYPES order)
const ARCHETYPE_COLORS = [
  '#00CED1',  // 0 Guide         — Teal
  '#FFD700',  // 1 Quartermaster — Gold
  '#00C86E',  // 2 Medic         — Green
  '#FF8C00',  // 3 Mule          — Orange
  '#9B59B6',  // 4 Scout         — Purple
  '#E74C3C',  // 5 Endurer       — Red
];

// ── Action system constants (mirrors server ACT_* / AO_*) ────────
const ACT_FORAGE  = 0, ACT_WATER = 1, ACT_SCAV = 3;
const ACT_SHELTER = 4, ACT_SURVEY = 6, ACT_REST = 7;
const ACT_TRADE = 8;  // client-only sentinel — no server action type (above server's 0–7 range)
const RES_SHORT = ['WAT', 'FOD', 'FUL', 'MED', 'SCP'];  // short labels for trade resource steppers
const AO_BLOCKED = 0, AO_SUCCESS = 1, AO_PARTIAL = 2, AO_FAIL = 3;
const ACT_NAMES = ['FORAGE','COLLECT WATER','','SCAVENGE',
                   'BUILD SHELTER','','SURVEY','REST'];
const ACT_MP    = [2, 1, 0, 2, 3, 0, 1, 0];  // default MP cost per action
// Which terrain indices allow each action (matches server terrain arrays)
// Forage: Open Scrub(0) DN7, Rust Forest(2) DN6, Marsh(3) DN8
// Water:  Marsh(3), Flooded(5)
// Scavenge: Broken Urban(4) DN6, Glass Fields(6) DN8
// Others: any terrain
const TERRAIN_FORAGE_DN  = [7,0,6,8,0,0,0,0,0,0,0, 0];
const TERRAIN_SALVAGE_DN = [0,0,0,0,6,7,8,0,0,0,0, 0];
const TERRAIN_HAS_WATER  = [0,0,0,1,0,1,0,0,0,0,0, 0];
function actAvailable(actId, terrainIdx) {
  if (terrainIdx == null || terrainIdx > 11) return false;
  switch (actId) {
    case ACT_FORAGE:  return TERRAIN_FORAGE_DN[terrainIdx]  > 0;
    case ACT_WATER:   return TERRAIN_HAS_WATER[terrainIdx]  > 0;
    case ACT_SCAV:    return TERRAIN_SALVAGE_DN[terrainIdx] > 0;
    default:          return true;   // REST, TREAT, SHELTER, SURVEY available everywhere
  }
}

// ── Survivor archetypes (§9.4 Synergy Roles) ─────────────────────
// Indices 0-5 mirror server ARCHETYPE_NAME[] and slot assignment.
// skills: [Navigate, Forage, Scavenge, Treat, Shelter, Endure]  0=none 1=trained 2=expert
const ARCHETYPES = [
  {
    name: 'GUIDE',
    icon: '\u29BF',   // ⦿ crosshair
    color: '#8B4513',
    trait: 'Companions entering your hex via your movement pay MC\u22121 (min\u00a01).',
    skills: [2, 1, 0, 0, 1, 1],
    invSlots: 8,
    desc: 'Natural pathfinder. Leads allies through hostile terrain, reducing the movement cost for anyone following in their footsteps.',
    flavor: 'Points the way. Usually directly into trouble.'
  },
  {
    name: 'QUARTERMASTER',
    icon: '\u25A3',   // ▣ box
    color: '#C07818',
    trait: 'In a Camp (2+ Survivors), every 2 Food/Water consumed restores\u00a01\u00a0extra.',
    skills: [0, 2, 1, 1, 1, 0],
    invSlots: 8,
    desc: 'Supply expert. Stretches the group\'s rations when camped with other survivors. Trait is inactive when travelling solo.',
    flavor: 'Stretches rations until they\u2019re unrecognizable. Survival tastes like cardboard.'
  },
  {
    name: 'MEDIC',
    icon: '\u2764',   // ♥ heart
    color: '#6B5449',
    trait: 'May treat Major Wounds in the field at DN\u00a09. Each successful Treat\u00a0+1\u00a0Fatigue restored.',
    skills: [0, 0, 1, 2, 0, 2],
    invSlots: 8,
    desc: 'Field surgeon. Can stabilise serious wounds without a settlement, and each successful treatment reduces the patient\'s Fatigue by 1.',
    flavor: 'Patches survivors back together. What remains is... functional.'
  },
  {
    name: 'MULE',
    icon: '\u26BF',   // ⚿ key
    color: '#A07828',
    trait: '12\u00a0inventory slots \u2014 hauls twice the standard load.',
    skills: [0, 1, 2, 0, 1, 1],
    invSlots: 12,
    desc: 'Pack carrier. Hauls twice the standard load.',
    flavor: 'Carries everything. Even the weight of everyone\u2019s poor decisions.'
  },
  {
    name: 'SCOUT',
    icon: '\u25CE',   // ◎ circle
    color: '#7A6055',
    trait: 'Survey costs 0\u00a0MP. May Survey before moving on the same turn without consuming the action slot.',
    skills: [2, 1, 1, 0, 0, 1],
    invSlots: 8,
    desc: 'Recon specialist. Survey is free and combinable with movement, giving the group an early read on terrain ahead.',
    flavor: 'Gets there first. Doesn\u2019t always come back.'
  },
  {
    name: 'ENDURER',
    icon: '\u25D9',   // ◙ inverse circle
    color: '#6A3008',
    trait: 'Endure skill treated as\u00a01\u00a0higher for all checks (does not affect Collective Endure).',
    skills: [1, 0, 0, 1, 2, 2],
    invSlots: 8,
    desc: 'Hardened survivor. Built to endure radiation, exhaustion and injury. The last one standing when conditions reach their worst.',
    flavor: 'Lasts longer than most. Which isn\u2019t saying much.'
  },
];

// ── Item system ──────────────────────────────────────────────────────────────
// Mirrors ItemCategory / EquipSlot enums in Esp32HexMapCrawl.ino
const ITEM_CATEGORY = { CONSUMABLE:0, EQUIPMENT:1, MATERIAL:2, KEY:3 };
const EQUIP_SLOT    = { NONE:0, HEAD:1, BODY:2, HAND:3, FEET:4, VEHICLE:5 };
const EQUIP_SLOT_NAMES = ['','Noggin','Hide','Mitts','Hooves','Rust Bucket'];
const ITEM_CATEGORY_NAMES = ['Gulpable','Bolt-On','Salvage','Relic'];

// Item catalog — mirrors /data/items.cfg on SD card.
// Image paths: img/items/item_<id>.png (illustration) and img/items/icon_<id>.png (badge)
// Narrative: preUse (shown before use prompt), postUse (after effect), story (key item lore)
// Missing image files fall back to placeholder via getItemImg() / getItemIcon()
const ITEMS = [
  { id:1,  name:'Trauma Patch',     category:0, slot:0,
    img:'img/items/item_1.png',  icon:'img/items/icon_1.png',
    preUse:  'You tear it open. It smells like antiseptic and desperation.',
    postUse: 'Slapped on. Definitely going to scar. +2 LL.',
    story:   null },
  { id:2,  name:'Mystery Rations',  category:0, slot:0,
    img:'img/items/item_2.png',  icon:'img/items/icon_2.png',
    preUse:  'The label just says "FOOD". That\'s optimistic.',
    postUse: 'You don\'t ask what it was. Your body forgives you. +3 Food.',
    story:   null },
  { id:3,  name:'Almost Water',     category:0, slot:0,
    img:'img/items/item_3.png',  icon:'img/items/icon_3.png',
    preUse:  'Filtered, sealed, and technically drinkable.',
    postUse: 'Tastes like nothing. In a good way. +3 Water.',
    story:   null },
  { id:4,  name:'Glow Flush',       category:0, slot:0,
    img:'img/items/item_4.png',  icon:'img/items/icon_4.png',
    preUse:  'The injector hisses. You hold your breath.',
    postUse: 'A wave of nausea, then clarity. You stop ticking. -4 Rad.',
    story:   null },
  { id:5,  name:'Panic Juice',      category:0, slot:0,
    img:'img/items/item_5.png',  icon:'img/items/icon_5.png',
    preUse:  'The needle goes in. Your heart immediately disagrees.',
    postUse: 'Wired. Alert. Slightly insane. -4 Fatigue, +1 Resolve, -1 Food.',
    story:   null },
  { id:6,  name:'Sweet Oblivion',   category:0, slot:0,
    img:'img/items/item_6.png',  icon:'img/items/icon_6.png',
    preUse:  'One dose. Only use if you can afford to be slow.',
    postUse: 'The pain goes somewhere quieter. +1 LL, -3 Fatigue.',
    story:   null },
  { id:7,  name:'Calorie Brick',    category:0, slot:0,
    img:'img/items/item_7.png',  icon:'img/items/icon_7.png',
    preUse:  'Dense, dry, aggressively optimistic packaging.',
    postUse: 'Hits fast. Fades fast. +1 Food, -2 Fatigue, -1 Water.',
    story:   null },
  { id:8,  name:'Screaming Spike',  category:0, slot:0,
    img:'img/items/item_8.png',  icon:'img/items/icon_8.png',
    preUse:  'You won\'t feel the needle. You won\'t remember using it either.',
    postUse: 'Your legs move before your brain does. +3 MP now.',
    story:   null },
  { id:9,  name:'Anti-Rot Kit',     category:0, slot:0,
    img:'img/items/item_9.png',  icon:'img/items/icon_9.png',
    preUse:  'Antibiotics, antiseptic, and a prayer.',
    postUse: 'Fever breaks. The rot stops spreading. Status cleared.',
    story:   null },
  { id:10, name:'Bright Bad Idea',  category:0, slot:0,
    img:'img/items/item_10.png', icon:'img/items/icon_10.png',
    preUse:  'Burning red light, visible for miles. Everyone will know.',
    postUse: 'The sky lights up. Hope and danger arrive together.',
    story:   null },
  { id:11, name:'Dent Absorber',    category:1, slot:2,
    img:'img/items/item_11.png', icon:'img/items/icon_11.png',
    preUse:  null, postUse: null,
    story:   'Cracked ceramic plates stitched into a salvaged vest. Won\'t stop everything, but it\'ll buy you seconds. +2 LL ceiling while equipped.' },
  { id:12, name:'Glow Suit',        category:1, slot:2,
    img:'img/items/item_12.png', icon:'img/items/icon_12.png',
    preUse:  null, postUse: null,
    story:   'Thick, yellow, and suffocating. A full seal against chemical and radiation hazards. Reduces Rad each dawn.' },
  { id:13, name:'Wheeze Filter',    category:1, slot:1,
    img:'img/items/item_13.png', icon:'img/items/icon_13.png',
    preUse:  null, postUse: null,
    story:   'Filters ash and particulates. Uncomfortable to sleep in. Unlocks traversal through toxic terrain.' },
  { id:14, name:'Dark Goggles',     category:1, slot:1,
    img:'img/items/item_14.png', icon:'img/items/icon_14.png',
    preUse:  null, postUse: null,
    story:   'Military surplus. One lens is cracked but it works. Extends vision radius +1 while equipped.' },
  { id:15, name:'Trudge Stompers',  category:1, slot:4,
    img:'img/items/item_15.png', icon:'img/items/icon_15.png',
    preUse:  null, postUse: null,
    story:   'Steel-toed, broken-in to someone else\'s feet. -1 Fatigue, +1 MP while equipped.' },
  { id:16, name:'Hoarder\'s Rig',   category:1, slot:2,
    img:'img/items/item_16.png', icon:'img/items/icon_16.png',
    preUse:  null, postUse: null,
    story:   'Loop after loop, pocket after pocket. If you can strap it on, you can carry it. +4 inventory slots while equipped.' },
  { id:17, name:'Rust Rocket',      category:1, slot:5,
    img:'img/items/item_17.png', icon:'img/items/icon_17.png',
    preUse:  null, postUse: null,
    story:   'Still runs. Barely. Costs 1 fuel at dawn — feed it and it moves fast. +4 MP while fuelled.' },
  { id:18, name:'Floaty Disaster',  category:1, slot:5,
    img:'img/items/item_18.png', icon:'img/items/icon_18.png',
    preUse:  null, postUse: null,
    story:   'Lashed together from oil drums and wishful thinking. Slow on land, essential on the river. Unlocks River Channel traversal.' },
  { id:19, name:'Vertical Regret',  category:1, slot:3,
    img:'img/items/item_19.png', icon:'img/items/icon_19.png',
    preUse:  null, postUse: null,
    story:   'Forty metres of woven polyester. Rated to 500kg. You\'re betting your life on it. Unlocks cliff traversal.' },
  { id:20, name:'Doom Clicker',     category:1, slot:3,
    img:'img/items/item_20.png', icon:'img/items/icon_20.png',
    preUse:  null, postUse: null,
    story:   'Vintage civil defence issue. Every click is a data point. Every data point is bad news. Reveals radiation on adjacent hexes.' },
  { id:21, name:'Useful Garbage',   category:2, slot:0,
    img:'img/items/item_21.png', icon:'img/items/icon_21.png',
    preUse: null, postUse: null, story: null },
  { id:22, name:'Sparky Bits',      category:2, slot:0,
    img:'img/items/item_22.png', icon:'img/items/icon_22.png',
    preUse: null, postUse: null, story: null },
  { id:23, name:'Burn Juice Can',   category:2, slot:0,
    img:'img/items/item_23.png', icon:'img/items/icon_23.png',
    preUse: null, postUse: null, story: null },
  { id:24, name:'Expired Meds',     category:2, slot:0,
    img:'img/items/item_24.png', icon:'img/items/icon_24.png',
    preUse: null, postUse: null, story: null },
  { id:25, name:'Doomed Diary',     category:3, slot:0,
    img:'img/items/item_25.png', icon:'img/items/icon_25.png',
    preUse:  null, postUse: null,
    story:   'A worn journal, pages stained with ash. Someone survived long enough to write this. Their luck ran out. Yours might too.' },
  // ── NEW ITEMS ──────────────────────────────────────────────────────────────
  { id:26, name:'Motorbike',        category:1, slot:5,
    img:'img/items/item_26.png', icon:'img/items/icon_26.png',
    preUse: null, postUse: null,
    story: 'Louder than a landmine and twice as reckless. Drinks fuel like it has a personal grievance against your reserves. +5 MP when fuelled.' },
  { id:27, name:'Portable Forge',   category:1, slot:3,
    img:'img/items/item_27.png', icon:'img/items/icon_27.png',
    preUse: null, postUse: null,
    story: 'A compact smelter cobbled together from a car exhaust and military ration tins. Doubles what you pull out of any scrap pile.' },
  { id:28, name:'Fishing Pole',     category:1, slot:3,
    img:'img/items/item_28.png', icon:'img/items/icon_28.png',
    preUse: null, postUse: null,
    story: 'Telescoping carbon fibre with a hook bent from a safety pin. Whatever lives in the river now, you can probably eat it. Doubles river foraging yield.' },
  { id:29, name:'Compound Bow',     category:1, slot:3,
    img:'img/items/item_29.png', icon:'img/items/icon_29.png',
    preUse: null, postUse: null,
    story: 'Pre-war hunting bow. Still in spec. Silent, reusable, and a genuine upgrade over throwing rocks at things you want to eat. Doubles land food foraging.' },
  { id:30, name:'Sour Cream Tub',   category:0, slot:0,
    img:'img/items/item_30.png', icon:'img/items/icon_30.png',
    preUse:  'The seal is unbroken. Against all odds, it smells fine.',
    postUse: 'Cool, dense, and impossibly soothing. The fever breaks. +3 LL, cures Fevered.',
    story:   null },
  { id:31, name:'Trippy Juice',     category:0, slot:0,
    img:'img/items/item_31.png', icon:'img/items/icon_31.png',
    preUse:  'It glows neon orange. Your hands are already shaking.',
    postUse: 'Your skull fills with light. The world turns beautiful and completely untrustworthy for a while. +5 LL.',
    story:   null },
  { id:32, name:'Fire Starter',     category:1, slot:3,
    img:'img/items/item_32.png', icon:'img/items/icon_32.png',
    preUse: null, postUse: null,
    story: 'Flint, magnesium strip, and a practiced flick. Anyone can start a fire. You can start a good one. Auto-upgrades camp shelter when resting.' },
  { id:33, name:'Intimidate Mask',  category:1, slot:1,
    img:'img/items/item_33.png', icon:'img/items/icon_33.png',
    preUse: null, postUse: null,
    story: 'Carved from a resin casting mould into something deeply wrong-looking. Most things in the wasteland will decide you aren\'t worth it. Reduces ambush threat.' },
  { id:34, name:'Bear Skin Cape',   category:1, slot:2,
    img:'img/items/item_34.png', icon:'img/items/icon_34.png',
    preUse: null, postUse: null,
    story: 'Something very large died to make this. You can still smell it. Provides armour and complete immunity to cold weather events. +2 LL ceiling.' },
  { id:35, name:'Loose Tentacle',   category:2, slot:0,
    img:'img/items/item_35.png', icon:'img/items/icon_35.png',
    preUse: null, postUse: null, story: null },
  { id:36, name:'Clean Underwear',  category:2, slot:0,
    img:'img/items/item_36.png', icon:'img/items/icon_36.png',
    preUse: null, postUse: null, story: null },
  { id:37, name:'Cursed Device',    category:3, slot:0,
    img:'img/items/item_37.png', icon:'img/items/icon_37.png',
    preUse:  null, postUse: null,
    story:   'A humming, slightly warm black box covered in symbols that shouldn\'t exist yet. It fell from the sky. Nothing about it is okay.' },
  { id:38, name:'Pre-War Net Map',  category:3, slot:0,
    img:'img/items/item_38.png', icon:'img/items/icon_38.png',
    preUse:  null, postUse: null,
    story:   'A recovered network node uplink. Broadcasts your position and pulls every active signal on the grid. Reveals all encounters and players on the map.' },
  { id:39, name:'Irradiated Fur',   category:2, slot:0,
    img:'img/items/item_39.png', icon:'img/items/icon_39.png',
    preUse: null, postUse: null, story: null },
  { id:40, name:'Shock Knuckles',   category:1, slot:3,
    img:'img/items/item_40.png', icon:'img/items/icon_40.png',
    preUse: null, postUse: null,
    story: 'Knuckle guards wrapped in strips of Irradiated Fur. Every punch builds charge. Every third hit discharges it through whatever you\'re hitting. +1 LL ceiling.' },
  { id:41, name:'Squatch Sliprs',   category:1, slot:4,
    img:'img/items/item_41.png', icon:'img/items/icon_41.png',
    preUse: null, postUse: null,
    story: 'Enormous felted slippers sewn from Sasquatch fur. Completely silent. Quicksand doesn\'t know what to do with them. Reduces ambush rates and grants slip immunity.' },
  { id:42, name:'Knife-Wrench',     category:1, slot:3,
    img:'img/items/item_42.png', icon:'img/items/icon_42.png',
    preUse: null, postUse: null,
    story: 'Half knife, half wrench, all disappointment. Functions as a terrible version of both. 20% chance of failing catastrophically and taking the damage yourself.' },
  { id:43, name:'Valid License',    category:3, slot:0,
    img:'img/items/item_43.png', icon:'img/items/icon_43.png',
    preUse:  null, postUse: null,
    story:   'Laminated, machine-stamped, completely legitimate. Obtained through a process no one should have to endure twice. Opens doors that shouldn\'t still exist.' },
  { id:44, name:'Jar of Sweats',    category:0, slot:0,
    img:'img/items/item_44.png', icon:'img/items/icon_44.png',
    preUse:  'You open it. The smell is a physical force.',
    postUse: 'You drink it. Your body accepts the hydration and immediately rejects the experience. +4 Water, lose next turn.',
    story:   null },
  { id:45, name:'Glow Dentures',    category:1, slot:1,
    img:'img/items/item_45.png', icon:'img/items/icon_45.png',
    preUse: null, postUse: null,
    story: 'A full set of pre-war dentures, mildly radioactive. They glow a faint blue in the dark. You don\'t need a flashlight. You also can\'t close your mouth all the way. +1 vision radius.' },
  { id:46, name:'Sonic Spines',     category:2, slot:0,
    img:'img/items/item_46.png', icon:'img/items/icon_46.png',
    preUse: null, postUse: null, story: null },
  { id:47, name:'Lead Snuggie',     category:1, slot:2,
    img:'img/items/item_47.png', icon:'img/items/icon_47.png',
    preUse: null, postUse: null,
    story: 'A full-body lead-lined fleece blanket with a hood. Near-total radiation immunity. Weighs as much as a bad decision. -1 MP, but you never glow. -5 Rad each dawn.' },
  { id:48, name:'Corrosive Syrup',  category:2, slot:0,
    img:'img/items/item_48.png', icon:'img/items/icon_48.png',
    preUse: null, postUse: null, story: null },
  { id:49, name:'Crater Deed',      category:2, slot:0,
    img:'img/items/item_49.png', icon:'img/items/icon_49.png',
    preUse: null, postUse: null, story: null },
  { id:50, name:'Uranium Candy',    category:0, slot:0,
    img:'img/items/item_50.png', icon:'img/items/icon_50.png',
    preUse:  'Hard, yellow-green, and slightly warm. The wrapper says "SAFE". It doesn\'t say for what.',
    postUse: 'Immediate, total energy restoration. Your cells notice the cost. +5 LL, +3 Rad, max health ceiling reduced.',
    story:   null },
  { id:51, name:'Sticky Note',      category:3, slot:0,
    img:'img/items/item_51.png', icon:'img/items/icon_51.png',
    preUse:  null, postUse: null,
    story:   'A faded yellow post-it note. In careful ballpoint: "admin / admin". The most powerful document in the wasteland.' },
];

// Placeholder image paths — shown when item_<id>.png / icon_<id>.png doesn't exist
const ITEM_IMG_PLACEHOLDER  = 'img/items/item_placeholder.png';
const ITEM_ICON_PLACEHOLDER = 'img/items/icon_placeholder.png';

// Get item definition by ID. Returns undefined if not found.
function getItemById(id) { return ITEMS.find(i => i.id === id); }

// Get illustration path. Falls back to placeholder (onerror should also be set on <img>).
function getItemImg(id)  {
  const item = getItemById(id);
  return item ? item.img  : ITEM_IMG_PLACEHOLDER;
}
function getItemIcon(id) {
  const item = getItemById(id);
  return item ? item.icon : ITEM_ICON_PLACEHOLDER;
}

// Get narrative text. phase: 'preUse' | 'postUse' | 'story'. Returns null if not set.
function getItemNarrative(id, phase) {
  const item = getItemById(id);
  return item ? (item[phase] ?? null) : null;
}

// Short skill labels for display
const SK_SHORT = ['Nav', 'For', 'Scav', 'Trt', 'Shel', 'End'];

// ── Skill check constants (mirrors server SK_* / SKILL_NAME) ─────
const SK_NAMES  = ['NAVIGATE','FORAGE','SCAVENGE','TREAT','SHELTER','ENDURE'];
// Suggested DN per skill index, indexed by terrain type 0-11
// terrain:        0   1   2   3   4   5   6   7   8   9  10  11  99
const SK_DN = [
  [ 5,  5,  7,  7,  7,  7,  5,  5,  7,  5, 99,  6, 99], // 0 Navigate
  [ 7,  9,  6,  8,  8,  8,  8,  8,  8,  8, 99, 99, 99], // 1 Forage (River impassable)
  [ 8,  8,  8,  8,  6,  8,  8,  8,  8,  8, 99, 99, 99], // 2 Scavenge
  [ 9,  9,  9,  9,  9,  9,  9,  9,  9,  9, 99,  9, 99], // 3 Treat
  [ 8,  8,  8,  8,  8,  8,  8,  8,  8,  8, 99, 99, 99], // 4 Shelter
  [ 7,  7,  7,  7,  7,  7,  7,  7,  9,  7, 99,  7, 99], // 5 Endure
];
function suggestDN(skill, terrain) {
  if (terrain == null || terrain > 11) return 7;
  return SK_DN[skill]?.[terrain] ?? 7;
}

// ── Weather system constants (must stay byte-for-byte identical to C++ tables) ─
// Phase IDs: 0=Clear 1=Rain 2=Storm 3=Chem-Storm
const WEATHER_PHASE_NAMES = ['CLEAR', 'RAIN', 'STORM', 'CHEM'];
// Visibility subtracted from server visR per phase (floored at 0)
const WEATHER_VIS_PENALTY = [0, 2, 4, 6];
// Terrain intensity [phase][terrain idx 0-11] — matches C++ WEATHER_INTENSITY exactly
// Terrains: 0=OpenScrub 1=AshDunes 2=RustForest 3=Marsh 4=BrokenUrban
//           5=FloodRuins 6=GlassFields 7=RollingHills 8=Mountain
//           9=Settlement 10=NukeCrater(impassable) 11=RiverChannel(impassable)
const WEATHER_INTENSITY = [
  [0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0],
  [0.5,  0.4,  0.6,  0.8,  0.4,  0.9,  0.5,  0.6,  0.7,  0.1,  0.0,  0.0],
  [0.7,  0.6,  0.7,  0.9,  0.5,  1.0,  0.8,  0.9,  1.0,  0.2,  0.0,  0.0],
  [0.95, 0.85, 0.75, 0.90, 0.6,  0.95, 0.90, 0.90, 0.85, 0.1,  0.0,  0.0],
];
