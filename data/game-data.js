// ── Constants (must match .ino) ─────────────────────────────────
const MAP_COLS    = 75;
const MAP_ROWS    = 57;
const MAX_PLAYERS = 6;
const CARAVAN_PID = 254;  // sentinel trade partner id — matches world-system.hpp
const VISION_R    = 1;   // base vision radius (server may send higher/lower via vr field)
const SQRT3       = Math.sqrt(3);
// ── 11 Terrain types ─────────────────────────────────────────────
// vis: +1=HIGH(+2 range), 0=STANDARD, -1=PENALTY(resources masked)
// mc : movement cost (255 = impassable)
// sv : shelter value
const TERRAIN = [
  { name:'Open Scrub',      mc:1,   sv:0, vis: 0, icon:'🌾',
    fill:'#2E2210', stroke:'#504030',   /* cracked-earth tan */
    tags:['Open Horizon','Forage','Hunting Ground'],
    desc:'Wind-scoured flats of pale scrub and cracked earth. Small game — birds, feral rabbits, scavenger rodents — move through the open ground. A successful hunt yields 3 food (2 on a partial). The open horizon grants long sight lines, but the animals can see you coming.' },
  { name:'Ash Dunes',       mc:2,   sv:0, vis: 0, icon:'🏜',
    fill:'#201E16', stroke:'#3C3A2C',   /* desaturated ash grey */
    tags:['Radiation'],
    desc:'Rolling dunes of grey volcanic ash laced with fallout. Fuel caches and scrap lie buried beneath the drifts. Prolonged exposure without a mask is hazardous.' },
  { name:'Rust Forest',     mc:2,   sv:1, vis:-3, icon:'🌲',
    fill:'#1A2808', stroke:'#344A18',   /* dark rust-tinged green */
    tags:['Forage','Wild Game','Blind Ground'],
    desc:'Skeletal trees coated in rust-red fungus. The dense canopy blocks all sight lines. A full Forage success here yields 3 food — the root networks are rich with edible fungi. Partial success still yields 2. Visibility drops to near zero.' },
  { name:'Marsh',           mc:3,   sv:0, vis: 0, icon:'🌿',
    fill:'#081A10', stroke:'#183428',   /* very dark brackish green */
    tags:['Water','Treacherous'],
    desc:'Brackish wetlands and salt flats. Water is abundant beneath the surface but undrinkable. Treacherous footing slows movement to a crawl. Avoid after dark.' },
  { name:'Broken Urban',    mc:2,   sv:1, vis:-2, icon:'🏚',
    fill:'#1A1814', stroke:'#34302A',   /* cold concrete grey */
    tags:['Salvage','Blind Ground'],
    desc:'Collapsed hab-blocks and fractured infrastructure. Salvage and medicine lie in the rubble. Every crumbled wall cuts line-of-sight. Watch for floor voids and gas pockets.' },
  { name:'Flooded District',mc:3,   sv:2, vis: 0, icon:'🌊',
    fill:'#08121E', stroke:'#142030',   /* cold steel blue-grey */
    tags:['Water','Treacherous','Blind Ground'],
    desc:'Former city streets drowned under murky floodwater. Water is plentiful here, but stay clear of craters and glass fields or it will be tainted. Visibility drops to zero beneath the surface. Every step is blind.' },
  { name:'Glass Fields',    mc:3,   sv:0, vis: 1, icon:'✨',
    fill:'#121A22', stroke:'#243444',   /* iridescent cold blue */
    tags:['Salvage','Open Horizon','Radiation'],
    desc:'Fused earth and melted debris from a detonation event. The flat reflective surface gives an unobstructed view for kilometres. Scrap can be carefully extracted from the glass.' },
  { name:'Ridge',           mc:2,   sv:1, vis: 2, icon:'⛰',
    fill:'#1E1A12', stroke:'#3C3424',   /* warm slag-stone */
    tags:['Vantage','Open Horizon'],
    desc:'Elevated ridgelines of compressed slag-stone. A superior vantage point — the surrounding terrain is visible in detail. Exposed to wind, lightning, and distant sight lines.' },
  { name:'Mountain',        mc:4,   sv:2, vis: 2, icon:'🗻',
    fill:'#14141C', stroke:'#28283A',   /* cold dark mineral */
    tags:['Vantage','Waypoint'],
    desc:'Towering slag-mountains and pre-war excavation sites. Heavy going, but caves and overhangs offer excellent shelter. Medicine and scrap can be found deep in the tunnels.' },
  { name:'Settlement',      mc:1,   sv:3, vis:-1, icon:'🏕',
    fill:'#1A1206', stroke:'#382814',   /* warm amber glow */
    tags:['Haven','Barter'],
    desc:'A fortified survivor camp with trading posts and basic shelter. All resource types can be found or traded here. The only true safe zone on the wasteland.' },
  { name:'Nuke Crater',     mc:255, sv:0, vis: 0, icon:'☢',
    fill:'#0A0E04', stroke:'#1A2008',   /* scorched void, green-black */
    tags:['Dead Zone','Radiation'],
    desc:'A direct-strike detonation crater. The ground is fused glass and irradiated rubble. Radiation at the rim is immediately lethal. No one goes in. No one comes back.' },
  { name:'River Channel',   mc:255, sv:0, vis:-3, icon:'〰',
    fill:'#0B1E0F', stroke:'#162B18',   /* brackish murky green — wasteland water */
    tags:['Impassable'],
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
// UI glyph sprite strip (img/ui_glyphs.png, built by scripts/gen_pixel_glyphs.py):
// 16px white pixel glyphs, tinted at draw time by drawGlyph() in renderer.js.
// Indices 0..11 match TERRAIN order; the rest are named here.
const GLYPH_SHEET = 'img/ui_glyphs.png';
const GLYPH_CELL  = 16;
const GLYPH = { WATER:12, FUEL:13, MED:14, SCRAP:15, FOOTPRINT:16, TENT:17, HUT:18, RAIN:19 };
// Canvas glyph per resource type (-1 = none; food uses the forage-animal PNG instead)
const RES_GLYPH = [-1, GLYPH.WATER, -1, GLYPH.FUEL, GLYPH.MED, GLYPH.SCRAP];
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
const ACT_FORAGE  = 0, ACT_WATER = 1, ACT_TREAT = 2, ACT_SCAV = 3;
const ACT_SHELTER = 4, ACT_CRAFT = 5, ACT_SURVEY = 6, ACT_REST = 7;
// ACT_TRADE is a client-only sentinel (no server action type, above the
// server's 0-7 range) — ACT_CRAFT=5 is a REAL server action id (fills the
// slot TRADE used before it got its own trade_offer/trade_accept protocol).
const ACT_TRADE = 8;
const RES_SHORT = ['WAT', 'FOD', 'FUL', 'MED', 'SCP'];  // short labels for trade resource steppers
const AO_BLOCKED = 0, AO_SUCCESS = 1, AO_PARTIAL = 2, AO_FAIL = 3;
const ACT_NAMES = ['FORAGE','COLLECT WATER','TREAT WOUND','SCAVENGE',
                   'BUILD SHELTER','CRAFT','SURVEY','REST'];

// ── Recipes — secret until learned via an encounter's "recipe" loot entry
// (see data/ui-encounter.js), then craftable at any Settlement (ACT_CRAFT).
// Mirrors data/recipes.cfg / RecipeDef in Esp32HexMapCrawl.ino byte-for-byte:
// matItem/matQty are up to 3 material ItemDef ids + counts, resCost is
// water/food/fuel/med/scrap tokens consumed (same order as RES_SHORT).
const RECIPES = [
  { id: 1,  name: 'Field Trauma Patch', outputItem: 1,  outputQty: 1,
    matItem: [24, 0, 0], matQty: [1, 0, 0], resCost: [0, 0, 0, 1, 0] },
  { id: 2,  name: 'Sock Puppet Bandage', outputItem: 53, outputQty: 1,
    matItem: [36, 24, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 3,  name: 'Squelch Bandage', outputItem: 57, outputQty: 1,
    matItem: [39, 24, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 4,  name: 'Blister Balm', outputItem: 61, outputQty: 1,
    matItem: [48, 24, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 5,  name: 'Gutter Broth', outputItem: 52, outputQty: 1,
    matItem: [21, 0, 0], matQty: [2, 0, 0], resCost: [1, 0, 0, 0, 0] },
  { id: 6,  name: 'Cricket Paste', outputItem: 55, outputQty: 1,
    matItem: [35, 0, 0], matQty: [2, 0, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 7,  name: 'Marrow Jelly', outputItem: 59, outputQty: 1,
    matItem: [21, 39, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 8,  name: 'Wired Knuckles', outputItem: 40, outputQty: 1,
    matItem: [22, 0, 0], matQty: [2, 0, 0], resCost: [0, 0, 1, 0, 0] },
  { id: 9,  name: 'Nostril Salts', outputItem: 58, outputQty: 1,
    matItem: [22, 0, 0], matQty: [1, 0, 0], resCost: [0, 0, 1, 0, 0] },
  { id: 10, name: 'Static Chew', outputItem: 60, outputQty: 1,
    matItem: [22, 46, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 11, name: 'Tooth Whiskey', outputItem: 56, outputQty: 1,
    matItem: [23, 22, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 12, name: 'Bile Flare', outputItem: 54, outputQty: 1,
    matItem: [48, 46, 0], matQty: [1, 1, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 13, name: 'Fur-Lined Cape', outputItem: 34, outputQty: 1,
    matItem: [39, 0, 0], matQty: [2, 0, 0], resCost: [0, 0, 0, 0, 1] },
  { id: 14, name: 'Corroded Edge', outputItem: 42, outputQty: 1,
    matItem: [48, 0, 0], matQty: [2, 0, 0], resCost: [0, 0, 0, 0, 1] },
  { id: 15, name: 'Screaming Spike', outputItem: 8,  outputQty: 1,
    matItem: [46, 0, 0], matQty: [3, 0, 0], resCost: [0, 0, 0, 0, 0] },
  { id: 16, name: 'Panic Dart', outputItem: 5,  outputQty: 1,
    matItem: [35, 0, 0], matQty: [1, 0, 0], resCost: [0, 0, 0, 0, 0] },
];
function getRecipeById(id) { return RECIPES.find(r => r.id === id) ?? null; }
function knowsRecipe(kr, id) { return ((kr ?? 0) >>> (id - 1)) & 1; }
// MP costs live on the action cards in ui-panels.js (shelter and water are dynamic).
// Which terrain indices allow each action (matches server terrain arrays)
// Forage: Open Scrub(0) DN7, Rust Forest(2) DN6, Marsh(3) DN8, River(11) DN6
// Water:  Marsh(3), Flooded(5), River(11)
// Scavenge: Broken Urban(4) DN6, Flooded(5) DN7, Glass Fields(6) DN8
// Treat:  anywhere for the Medic, Settlement(9) for everyone else
// Others: any terrain
// River Channel (11) is reachable with the right equipment: it forages and waters.
const TERRAIN_FORAGE_DN  = [7,0,6,8,0,0,0,0,0,0,0, 6];
const TERRAIN_SALVAGE_DN = [0,0,0,0,6,7,8,0,0,0,0, 0];
const TERRAIN_HAS_WATER  = [0,0,0,1,0,1,0,0,0,0,0, 1];
function actAvailable(actId, terrainIdx) {
  if (terrainIdx == null || terrainIdx > 11) return false;
  switch (actId) {
    case ACT_FORAGE:  return TERRAIN_FORAGE_DN[terrainIdx]  > 0;
    case ACT_WATER:   return TERRAIN_HAS_WATER[terrainIdx]  > 0;
    case ACT_SCAV:    return TERRAIN_SALVAGE_DN[terrainIdx] > 0;
    default:          return true;   // REST, SHELTER, SURVEY available everywhere
  }
}

// ── Survivor archetypes (§9.4 Synergy Roles) ─────────────────────
// Indices 0-5 mirror server ARCHETYPE_NAME[] and slot assignment.
// skills: [Navigate, Forage, Scavenge, Shelter, Endure]  0=none 1=trained 2=expert
const ARCHETYPES = [
  {
    name: 'GUIDE',
    icon: '\u29BF',   // ⦿ crosshair
    color: '#8B4513',
    trait: 'Allies moving onto a hex you are standing on pay MC\u22121 (min\u00a01).',
    skills: [2, 1, 0, 1, 1],
    invSlots: 8,
    desc: 'Natural pathfinder. Leads allies through hostile terrain, reducing the movement cost for anyone following in their footsteps.',
    flavor: 'Points the way. Usually directly into trouble.'
  },
  {
    name: 'QUARTERMASTER',
    icon: '\u25A3',   // ▣ box
    color: '#C07818',
    trait: 'In a Camp (2+ survivors sharing your hex), every 2 Food/Water consumed restores\u00a01\u00a0extra.',
    skills: [0, 2, 1, 1, 0],
    invSlots: 8,
    desc: 'Supply expert. Stretches the group\'s rations when camped with other survivors. Trait is inactive when travelling solo.',
    flavor: 'Stretches rations until they\u2019re unrecognizable. Survival tastes like cardboard.'
  },
  {
    name: 'MEDIC',
    icon: '\u2764',   // ♥ heart
    color: '#6B5449',
    trait: 'May TREAT a Major Wound anywhere at DN\u00a09. Others must stand in a Settlement.',
    skills: [0, 0, 1, 0, 2],
    invSlots: 8,
    desc: 'Field surgeon. Can stabilise Major Wounds anywhere, at 2\u00a0MP and 1\u00a0Medicine a time.',
    flavor: 'Patches survivors back together. What remains is... functional.'
  },
  {
    name: 'MULE',
    icon: '\u26BF',   // ⚿ key
    color: '#A07828',
    trait: '12\u00a0inventory slots \u2014 hauls twice the standard load.',
    skills: [0, 1, 2, 1, 1],
    invSlots: 12,
    desc: 'Pack carrier. Hauls twice the standard load.',
    flavor: 'Carries everything. Even the weight of everyone\u2019s poor decisions.'
  },
  {
    name: 'SCOUT',
    icon: '\u25CE',   // ◎ circle
    color: '#7A6055',
    trait: 'Survey costs 0\u00a0MP, and your vision radius is +2.',
    skills: [2, 1, 1, 0, 1],
    invSlots: 8,
    desc: 'Recon specialist. Sees two rings further than anyone else and surveys for free, giving the group an early read on the terrain ahead.',
    flavor: 'Gets there first. Doesn\u2019t always come back.'
  },
  {
    name: 'ENDURER',
    icon: '\u25D9',   // ◙ inverse circle
    color: '#6A3008',
    trait: 'Endure counts as\u00a01\u00a0higher on every check, and exposure never costs you Life.',
    skills: [1, 0, 0, 2, 2],
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
// usable: key items (category 3) only — true when items.cfg gives this item a
// real effect (effectId != EFX_NONE), meaning useItem() in inventory_items.hpp
// will actually dispatch something and the item can be "read" repeatedly.
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
    postUse: 'Wired. Alert. Slightly insane. Burns through reserves. -1 Food.',
    story:   null },
  { id:6,  name:'Sweet Oblivion',   category:0, slot:0,
    img:'img/items/item_6.png',  icon:'img/items/icon_6.png',
    preUse:  'One dose. Only use if you can afford to be slow.',
    postUse: 'The pain goes somewhere quieter. +1 LL.',
    story:   null },
  { id:7,  name:'Calorie Brick',    category:0, slot:0,
    img:'img/items/item_7.png',  icon:'img/items/icon_7.png',
    preUse:  'Dense, dry, aggressively optimistic packaging.',
    postUse: 'Hits fast. Fades fast. +1 Food, -1 Water.',
    story:   null },
  { id:8,  name:'Screaming Spike',  category:0, slot:0,
    img:'img/items/item_8.png',  icon:'img/items/icon_8.png',
    preUse:  'You won\'t feel the needle. You won\'t remember using it either.',
    postUse: 'Your legs move before your brain does. +3 MP now.',
    story:   null },
  { id:9,  name:'Anti-Rot Kit',     category:0, slot:0,
    img:'img/items/item_9.png',  icon:'img/items/icon_9.png',
    preUse:  'Antibiotics, antiseptic, and a prayer.',
    postUse: 'Wound cleaned and closed. Recovery begins.',
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
    story:   'Thick, yellow, and suffocating. A full seal against fallout: no radiation check when you walk into hot terrain, and Rad bleeds off each dawn.' },
  { id:13, name:'Wheeze Filter',    category:1, slot:1,
    img:'img/items/item_13.png', icon:'img/items/icon_13.png',
    preUse:  null, postUse: null,
    story:   'Filters ash and particulates. Uncomfortable to sleep in. No radiation check when you walk into hot terrain.' },
  { id:14, name:'Dark Goggles',     category:1, slot:1,
    img:'img/items/item_14.png', icon:'img/items/icon_14.png',
    preUse:  null, postUse: null,
    story:   'Military surplus. One lens is cracked but it works. Extends vision radius +1 while equipped.' },
  { id:15, name:'Trudge Stompers',  category:1, slot:4,
    img:'img/items/item_15.png', icon:'img/items/icon_15.png',
    preUse:  null, postUse: null,
    story:   'Steel-toed, broken-in to someone else\'s feet. +1 MP while equipped.' },
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
    story:   'Forty metres of woven polyester. Rated to 500kg. You\'re betting your life on it. Mountains cost 2 MP instead of 4.' },
  { id:20, name:'Doom Clicker',     category:1, slot:3,
    img:'img/items/item_20.png', icon:'img/items/icon_20.png',
    preUse:  null, postUse: null,
    story:   'Vintage civil defence issue. Every click is a data point. Every data point is bad news. You learn to hear what is out there: +1 vision.' },
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
  { id:25, name:'Doomed Diary',     category:3, slot:0, usable:true,
    img:'img/items/item_25.png', icon:'img/items/icon_25.png',
    preUse:  null, postUse: null,
    story:   'A worn journal, pages stained with ash. Someone survived long enough to write this. Their luck ran out. Yours might too. Reading it maps the three hexes around you.' },
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
    postUse: 'Cool, dense, and impossibly soothing. +3 LL.',
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
  { id:37, name:'Cursed Device',    category:3, slot:0, usable:true,
    img:'img/items/item_37.png', icon:'img/items/icon_37.png',
    preUse:  null, postUse: null,
    story:   'A humming, slightly warm black box covered in symbols that shouldn\'t exist yet. It fell from the sky. Nothing about it is okay.' },
  { id:38, name:'Pre-War Net Map',  category:3, slot:0, usable:true,
    img:'img/items/item_38.png', icon:'img/items/icon_38.png',
    preUse:  null, postUse: null,
    story:   'A recovered network node uplink. Pulls every active signal on the grid. Reading it surveys the entire map; it can be read again any time.' },
  { id:39, name:'Irradiated Fur',   category:2, slot:0,
    img:'img/items/item_39.png', icon:'img/items/icon_39.png',
    preUse: null, postUse: null, story: null },
  { id:40, name:'Shock Knuckles',   category:1, slot:3,
    img:'img/items/item_40.png', icon:'img/items/icon_40.png',
    preUse: null, postUse: null,
    story: 'Knuckle guards wrapped in strips of Irradiated Fur. They hum faintly. Nobody has tested them on anything living, and nobody wants to. +1 LL ceiling.' },
  { id:41, name:'Squatch Sliprs',   category:1, slot:4,
    img:'img/items/item_41.png', icon:'img/items/icon_41.png',
    preUse: null, postUse: null,
    story: 'Enormous felted slippers sewn from Sasquatch fur. Completely silent. Whatever is out there stops noticing you: the Threat Clock winds down by 4 each dawn.' },
  { id:42, name:'Knife-Wrench',     category:1, slot:3,
    img:'img/items/item_42.png', icon:'img/items/icon_42.png',
    preUse: null, postUse: null,
    story: 'Half knife, half wrench, all disappointment. Functions as a terrible version of both, but it does replace two tools with one: +1 pack slot.' },
  { id:43, name:'Valid License',    category:3, slot:0,
    img:'img/items/item_43.png', icon:'img/items/icon_43.png',
    preUse:  null, postUse: null,
    story:   'Laminated, machine-stamped, completely legitimate. Obtained through a process no one should have to endure twice. Opens doors that shouldn\'t still exist.' },
  { id:44, name:'Jar of Sweats',    category:0, slot:0,
    img:'img/items/item_44.png', icon:'img/items/icon_44.png',
    preUse:  'You open it. The smell is a physical force.',
    postUse: 'You drink it. Your body accepts the hydration and immediately rejects the experience. +4 Water, −2 MP.',
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
  // ── CRAFTED CONCOCTIONS — recipe-only outputs (data/recipes.cfg), never drop as loot ──
  { id:52, name:'Gutter Broth',       category:0, slot:0,
    img:'img/items/item_52.png', icon:'img/items/icon_52.png',
    preUse:  'It\'s gray. It\'s warm. Something in it used to have a wrapper.',
    postUse: 'Tastes like a dumpster\'s memory of soup. Fills you up anyway. +2 Food, +1 Rad.',
    story:   null },
  { id:53, name:'Sock Puppet Bandage', category:0, slot:0,
    img:'img/items/item_53.png', icon:'img/items/icon_53.png',
    preUse:  'A sock, boiled, folded, and pretending very hard to be medical gauze.',
    postUse: 'It holds. You decide not to think about which sock. Closes a minor wound.',
    story:   null },
  { id:54, name:'Bile Flare',         category:0, slot:0,
    img:'img/items/item_54.png', icon:'img/items/icon_54.png',
    preUse:  'The jar hisses when you crack the seal. Your eyes water in advance.',
    postUse: 'It pops, hisses, and stinks so bad that whatever was watching reconsiders. Threat Clock −2.',
    story:   null },
  { id:55, name:'Cricket Paste',      category:0, slot:0,
    img:'img/items/item_55.png', icon:'img/items/icon_55.png',
    preUse:  'Ground fine. Still faintly chirping, somehow.',
    postUse: 'Crunchy, then chewy, then gone. Better than it has any right to be. +1 Food, +1 LL.',
    story:   null },
  { id:56, name:'Tooth Whiskey',      category:0, slot:0,
    img:'img/items/item_56.png', icon:'img/items/icon_56.png',
    preUse:  'It\'s the color of an electrical fire and smells about the same.',
    postUse: 'Goes down like a live wire, twice. You feel great. You also feel slightly warm inside. +2 LL, +1 Rad.',
    story:   null },
  { id:57, name:'Squelch Bandage',    category:0, slot:0,
    img:'img/items/item_57.png', icon:'img/items/icon_57.png',
    preUse:  'Wet. Warm. You were told not to ask what cured it. You don\'t.',
    postUse: 'It squelches once, then sets. Closes a minor wound.',
    story:   null },
  { id:58, name:'Nostril Salts',      category:0, slot:0,
    img:'img/items/item_58.png', icon:'img/items/icon_58.png',
    preUse:  'One whiff and your eyes are already open.',
    postUse: 'A jolt of ammonia-and-worse snaps you upright, wide awake. +1 MP.',
    story:   null },
  { id:59, name:'Marrow Jelly',       category:0, slot:0,
    img:'img/items/item_59.png', icon:'img/items/icon_59.png',
    preUse:  'Rendered down slow, from something with bones you didn\'t recognize.',
    postUse: 'Wet, rich, and hydrating in a way you choose not to examine too closely. +2 Water, +1 LL.',
    story:   null },
  { id:60, name:'Static Chew',        category:0, slot:0,
    img:'img/items/item_60.png', icon:'img/items/icon_60.png',
    preUse:  'A live wire, chewed like gum. This was, at some point, someone\'s idea.',
    postUse: 'Your teeth go numb and the wasteland snaps into focus for a moment. Reveals the terrain nearby.',
    story:   null },
  { id:61, name:'Blister Balm',       category:0, slot:0,
    img:'img/items/item_61.png', icon:'img/items/icon_61.png',
    preUse:  'A paste the color of a bruise, and it smells like one too.',
    postUse: 'It draws the glow out through weeping blisters. Unpleasant. Effective. −2 Rad.',
    story:   null },
];

// Placeholder image paths — shown when item_<id>.png / icon_<id>.png doesn't exist.
// Monochrome amber pixel-art (16px grid @2x) so missing art still matches the UI.
const ITEM_IMG_PLACEHOLDER  = 'img/items/item_placeholder.png';   // skull, 128px
const ITEM_ICON_PLACEHOLDER = 'img/items/icon_placeholder.png';   // skull, 32px
// Per-category badge fallback (indexed by item.category): flask / wrench / gear / key
const ITEM_ICON_FALLBACK = [
  'img/items/icon_gulpable.png',  // 0 Gulpable  (consumable)
  'img/items/icon_bolton.png',    // 1 Bolt-On   (equipment)
  'img/items/icon_salvage.png',   // 2 Salvage   (material)
  'img/items/icon_relic.png',     // 3 Relic     (key item)
];
// Fallback badge for an item id — category icon, else the generic skull.
function getItemIconFallback(id) {
  const item = getItemById(id);
  return (item && ITEM_ICON_FALLBACK[item.category]) || ITEM_ICON_PLACEHOLDER;
}

// ── Equipment stat modifiers ────────────────────────────────────────────────
// Mirrors stat fields in /data/items.cfg (mp, ll, slots, rad, vision, *_cost).
// Used by the character sheet to display equipped-item bonuses to the player.
// NOTE: Display only — actual stat calculations are authoritative on the server.
//   mp/ll/slots/vision/rad : passive modifier while equipped
//   fuelCost/waterCost/etc : tokens consumed at dawn while equipped
//   note                   : qualitative effect (terrain unlock, special)
const ITEM_MODS = {
  11: { ll: +2 },                                                  // Dent Absorber
  12: { rad: -1, note: 'Sealed: no rad check entering hot terrain' }, // Glow Suit
  13: { note: 'Sealed: no rad check entering hot terrain' },       // Wheeze Filter
  14: { vision: +1 },                                              // Dark Goggles
  15: { mp: +1 },                                                  // Trudge Stompers
  16: { slots: +4 },                                               // Hoarder's Rig
  17: { mp: +4, fuelCost: 1 },                                     // Rust Rocket
  18: { note: 'Cross River Channel (MC 2)' },                      // Floaty Disaster
  19: { note: 'Climbing gear: Mountain costs MC 2' },              // Vertical Regret
  20: { vision: +1 },                                              // Doom Clicker
  26: { mp: +5, fuelCost: 1 },                                     // Motorbike
  27: { note: 'Doubles scrap from SCAVENGE' },                     // Portable Forge
  28: { note: 'Cross River Channel; doubles river forage' },       // Fishing Pole
  29: { note: 'Doubles land food forage yield' },                  // Compound Bow
  32: { note: 'REST upgrades a basic shelter to improved' },       // Fire Starter
  33: { note: 'Threat Clock −3 each dawn' },                       // Intimidate Mask
  34: { ll: +2, note: 'No exposure loss at dawn' },                // Bear Skin Cape
  40: { ll: +1 },                                                  // Shock Knuckles
  41: { note: 'Threat Clock −4 each dawn' },                       // Squatch Sliprs
  42: { slots: +1 },                                               // Knife-Wrench
  45: { vision: +1 },                                              // Glow Dentures
  47: { mp: -1, rad: -5 },                                         // Lead Snuggie
};

function getItemMods(id) { return ITEM_MODS[id] || null; }

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

// Short skill labels for display
const SK_SHORT = ['Nav', 'For', 'Scav', 'Shel', 'End'];

// ── Wounds (mirrors server WOUND_* / Player.wounds[]) ────────────
// wd[0] = minor (−1 Endure each), wd[1] = major (−1 all skills and −1 MP each)
const WOUND_MINOR = 0, WOUND_MAJOR = 1, WOUND_MAX_EACH = 3;
const WOUND_NAMES = ['MINOR', 'MAJOR'];
const TREAT_DN    = 9;

// ── Skill check constants (mirrors server SK_* / SKILL_NAME) ─────
const SK_NAMES  = ['NAVIGATE','FORAGE','SCAVENGE','SHELTER','ENDURE'];

// ── Weather system constants (must stay byte-for-byte identical to C++ tables) ─
// Phase IDs: 0=Clear 1=Rain 2=Storm 3=Chem-Storm 4=Fog ("Strangle Fog")
// 5=Mist ("Fog" — plain, cosmetic-only; MIST is the internal name, kept
// distinct from FOG/"Strangle Fog" so the two are never confused in code)
const WEATHER_PHASE_NAMES = ['CLEAR', 'RAIN', 'STORM', 'CHEM', 'STRANGLE FOG', 'FOG'];
// Visibility subtracted from server visR per phase (floored at 0)
const WEATHER_VIS_PENALTY = [0, 1, 3, 5, 4, 2];
// Extra movement cost per hex in each phase — added to terrain MC by movePlayer()
const WEATHER_MOVE_PENALTY = [0, 1, 2, 3, 1, 1];
// Terrain intensity [phase][terrain idx 0-11] — matches C++ WEATHER_INTENSITY exactly
// Terrains: 0=OpenScrub 1=AshDunes 2=RustForest 3=Marsh 4=BrokenUrban
//           5=FloodRuins 6=GlassFields 7=RollingHills 8=Mountain
//           9=Settlement 10=NukeCrater(impassable) 11=RiverChannel(impassable)
// Fog is worst in dense/wet terrain (Rust Forest, Marsh, Flooded Ruins) and
// weakest on high dry ground (Rolling Hills, Mountain) — drives its own
// per-tick MP/LL hazard on the firmware, same shape as chem's row. Mist's
// row is all-zero: purely cosmetic, no per-tick hazard.
const WEATHER_INTENSITY = [
  [0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0   ],
  [0.5,  0.4,  0.6,  0.8,  0.4,  0.9,  0.5,  0.6,  0.7,  0.1,  0,    0   ],
  [0.7,  0.6,  0.7,  0.9,  0.5,  1.0,  0.8,  0.9,  1.0,  0.2,  0,    0   ],
  [0.95, 0.85, 0.75, 0.90, 0.6,  0.95, 0.90, 0.90, 0.85, 0.1,  0,    0   ],
  [0.45, 0.35, 0.7,  0.75, 0.25, 0.65, 0.5,  0.3,  0.2,  0.1,  0,    0   ],
  [0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0   ],
];
