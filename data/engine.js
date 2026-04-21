// ── Vision radius (updated per vis/sync message from server) ─────
let myVisionR = VISION_R;

// ── Weather phase (0=Clear 1=Rain 2=Storm 3=Chem; updated from server gs.wp) ─
let weatherPhase = 0;

// ── Magic number constants ────────────────────────────────────────
// Animation & timing
const LERP_RATE              = 0.26;    // per-frame position interpolation (higher = snappier)
const MOVE_COOLDOWN_BASE_MS  = 220;    // base move cooldown in ms (multiplied by terrain MC)
const NIGHT_FADE_INIT        = 0.72;   // initial alpha when REST action completes
const NIGHT_FADE_DECAY_RATE  = 0.004;  // per-frame decay rate for post-dawn overlay
const RESTING_LERP_RATE      = 0.003;  // MP display lerp while resting (slower)
const MOVING_LERP_RATE       = 0.08;   // MP display lerp while moving (faster)
const ARROW_BOUNCE_PERIOD_MS = 350;    // period of the ▼ bounce animation on the current player

// Rendering dimensions
const HEX_SZ_MIN             = 36;     // minimum hex size in pixels
const HEX_SZ_MAX             = 64;     // maximum hex size in pixels
const HEX_SZ_VIEWPORT_DIVISOR = 11;   // viewport width ÷ this = hex count per row → hex size
const ICON_SIZE_SCALE        = 0.44;  // icon size as fraction of hex size
const ARROW_SIZE_SCALE       = 0.62;  // arrow size as fraction of icon size
const SHADOW_HORIZONTAL_SCALE = 0.36; // shadow ellipse horizontal scale
const SHADOW_VERTICAL_SCALE  = 0.09;  // shadow ellipse vertical scale
const HEAD_RADIUS_SCALE      = 1.05;  // character head radius scale for font size

// River ripple animation
const RIVER_RIPPLE_COUNT     = 3;      // number of concentric ripple rings per river hex
const RIVER_RIPPLE_SPEED     = 0.45;  // how fast rings advance per second (phase units/s)
const RIVER_RIPPLE_SPACING   = 0.33;  // phase offset between successive rings (0–1)
const RIVER_RIPPLE_W_SCALE   = 0.85;  // ellipse horizontal radius as fraction of hex size × wave scale
const RIVER_RIPPLE_H_SCALE   = 0.38;  // ellipse vertical radius as fraction of hex size × wave scale

// Footprint rendering
const FOOTPRINT_RING_RADIUS  = 0.28;  // ring radius (hex-size multiples) for footprint icon layout

// Fog of war & visibility
const FOG_INNER_ALPHA        = 0.78;  // alpha for inner fog ring (more visible)
const SHADOW_ALPHA           = 0.72;  // shadow under character icons

// WebSocket connection
const WIFI_CREDS_SEND_DELAY_MS = 300; // delay before auto-sending WiFi creds
const RECONNECT_DELAY_MS      = 2000; // delay before attempting reconnect

// ── Shared animation state (written by network, read by renderer) ─
let maxMP    = 6;   // plain copy used by non-reactive rendering (time-of-day clock)
let nightFade = 0;  // extra night overlay that fades out after dawn
let displayMP = 6;  // smoothly lerped toward uiMP.val each frame

// ── Image Loading Utility ─────────────────────────────────────────
function createImageWithLoadTracking(src) {
  const img = new Image();
  img.loaded = false;
  img.src = src;
  img.onload = () => { img.loaded = true; };
  img.onerror = () => {
    img.loaded = false;
    console.warn(`Failed to load image: ${src}`);
  };
  return img;
}

// ── Terrain hex images ────────────────────────────────────────────
// Naming: /img/hex<Name><N>.png  (e.g. hexOpenScrub0.png, hexOpenScrub1.png)
// terrainImgVariants[terrain][variant] → Image object (or undefined if missing).
// Populated by loadTerrainVariants(vc) when the sync message arrives.
const TERRAIN_IMG_NAMES = [
  'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
  'BrokenUrban', 'FloodedDistrict', 'GlassFields',
  'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel'
];
const terrainImgVariants = Array.from({ length: NUM_TERRAIN }, () => []);

function loadTerrainVariants(vc) {
  for (let t = 0; t < NUM_TERRAIN; t++) {
    const name = TERRAIN_IMG_NAMES[t];
    const count = (vc && vc[t]) || 0;
    terrainImgVariants[t] = Array.from(
      { length: count },
      (_, v) => createImageWithLoadTracking(`/img/hex${name}${v}.png`)
    );
  }
}

// ── Survivor pawn portrait images ────────────────────────────────
// Indexed by archetype: 0=Guide 1=Quartermaster 2=Medic 3=Mule 4=Scout 5=Endurer
const pawnImgs = ARCHETYPES.map(a =>
  createImageWithLoadTracking(`img/survivors/${a.name.toLowerCase()}Pawn.jpg`)
);

// ── Forage animal images ──────────────────────────────────────────
// Naming: /img/forrageAnimal<N>.png  — shown on cells with food resource (type 2)
let forrageAnimalImgs = [];
const collectedCells = new Set(); // cells cleared by 'col' — guards against vis disk overwrite

function loadForrageAnimalImgs(count) {
  forrageAnimalImgs = Array.from(
    { length: count },
    (_, v) => createImageWithLoadTracking(`/img/forrageAnimal${v}.png`)
  );
}

// ── Shelter images ────────────────────────────────────────────────
// Naming: /img/shelterBasic<N>.png, /img/shelterImproved<N>.png
// shelterImgs[0] = basic variants, shelterImgs[1] = improved variants
const shelterImgs = [[], []];
const SHELTER_IMG_NAMES = ['shelterBasic', 'shelterImproved'];

function loadShelterVariants(sv) {
  for (let s = 0; s < 2; s++) {
    const count = (sv && sv[s]) || 0;
    shelterImgs[s] = Array.from(
      { length: count },
      (_, v) => createImageWithLoadTracking(`/img/${SHELTER_IMG_NAMES[s]}${v}.png`)
    );
  }
}

// ── State ───────────────────────────────────────────────────────
let myId = -1;
// gameMap[r][q] = { terrain, resource, amount } or null (fogged/unknown)
let gameMap = Array.from({ length: MAP_ROWS }, () => Array(MAP_COLS).fill(null));
let players = Array.from({ length: MAX_PLAYERS }, (_, i) => ({
  id: i, on: false, q: 0, r: 0, sc: 0, nm: `Survivor${i}`,
  inv: [0,0,0,0,0], sp: 0, st: 0,
  // Survivor fields
  ll: 7, food: 6, water: 6, rad: 0, res: 3,
  arch: 0, is: 8,
  sk: [0,0,0,0,0],
  wd: [0,0,0],
  it: Array(12).fill(0),
  iq: Array(12).fill(0),
  // §4 Resource economy
  fth: 0, wth: 0, mp: 6,
  // §5 Action tracking
  au: false, rest: false,
  // §6 Encounter
  enc: false,
}));

// Shared game state (Threat Clock, Day, shared stores)
let gameState = { tc: 0, dc: 0, sf: 30, sw: 30 };
// Ground items from latest sync/ground_update
let groundItems = [];

// ── Agent state snapshot ─────────────────────────────────────────
// Updated after every WS message. Read via: window.__gameState
function buildAgentState() {
  const me = myId >= 0 ? players[myId] : null;
  const visibleCells = [];
  if (me) {
    for (let r = 0; r < MAP_ROWS; r++) {
      for (let q = 0; q < MAP_COLS; q++) {
        const c = gameMap[r][q];
        if (c) visibleCells.push({ q, r, terrain: c.terrain, resource: c.resource, amount: c.amount, shelter: c.shelter });
      }
    }
  }
  window.__gameState = {
    myId,
    day:     gameState.dc,
    tc:      gameState.tc,
    visR:    myVisionR,
    me: me ? {
      q: me.q, r: me.r,
      ll: me.ll, food: me.food, water: me.water,
      rad: me.rad,
      mp: me.mp, resting: me.rest,
      inv: { water: me.inv[0], food: me.inv[1], fuel: me.inv[2], med: me.inv[3], scrap: me.inv[4] },
      score: me.sc, name: me.nm,
    } : null,
    players: players.filter(p => p.on && p.id !== myId).map(p => ({
      id: p.id, name: p.nm, q: p.q, r: p.r, ll: p.ll, mp: p.mp,
    })),
    visibleCells,
  };
}
