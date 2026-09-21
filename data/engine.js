// ── Vision radius (updated per vis/sync message from server) ─────
let myVisionR = VISION_R;

// ── Weather phase (0=Clear 1=Rain 2=Storm 3=Chem 4=Fog aka "Strangle Fog" 5=Mist aka plain "Fog"; updated from server gs.wp) ─
let weatherPhase = 0;

// Effective vision radius. Use this everywhere visibility is calculated so the
// rendered view, the char sheet display and the AI-agent state object stay
// consistent.
//
// The server already subtracts WEATHER_VIS_PENALTY in playerVisParams() before
// it sends `vr`, and it only ships fog data for that radius — so this must NOT
// subtract it a second time. Doing so double-penalised rain and drew fog over
// hexes the client had been given.
function getEffectiveVR() {
  return Math.max(0, myVisionR);
}

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
// Zoom is a re-layout (HEX_SZ changes), not a canvas transform — art stays
// crisp, but zooming out grows the per-frame cell loop quadratically, so the
// floor here is a frame-cost decision, not a taste one.
const MAP_ZOOM_MIN_PX        = 24;     // smallest hex size reachable by zooming out
const MAP_ZOOM_MAX_PX        = 128;    // largest hex size reachable by zooming in
const MAP_ZOOM_STORAGE_KEY   = 'map_zoom';
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
// Outside your sight radius the map fades to black across FOG_FADE_RINGS rings
// instead of dropping to the void in one step.
//
// The gradient is in the fill *colour*, at alpha 1 — deliberately not
// globalAlpha. The fog fill is the first thing drawn on a cleared canvas, so
// alpha only blends it toward the page backdrop (rgb(8,6,4)), which is itself
// slightly *darker* than the fog colour: lowering alpha made a ring darker
// rather than more see-through, and the whole knob spanned about 12/255
// end to end. There is also nothing underneath to reveal — the server sends
// tt = 0xFF for every hex past visR (encodeMapFog in hex-map.hpp), so the
// client has no terrain for them. Colour gives the full range and runs in the
// direction the name implies.
const FOG_FADE_RINGS         = 3;             // rings from sight edge to full dark
const FOG_NEAR_RGB           = [42, 35, 20];  // first hidden ring: "something is there"
const FOG_VOID_RGB           = [8, 4, 2];     // past the fade: unknown map
// Precomputed per-ring fill, indexed by min(dist - visR - 1, FOG_FADE_RINGS).
// Built once rather than per hex: at minimum zoom the terrain pass touches
// ~2000 cells a frame and most of them are fogged.
const FOG_RING_FILL = Array.from({ length: FOG_FADE_RINGS + 1 }, (_, i) => {
  const t = i / FOG_FADE_RINGS;
  const ch = (n) => Math.round(FOG_NEAR_RGB[n] + (FOG_VOID_RGB[n] - FOG_NEAR_RGB[n]) * t);
  return `rgb(${ch(0)},${ch(1)},${ch(2)})`;
});
const SHADOW_ALPHA           = 0.72;  // shadow under character icons

// ── Sight-edge fade ───────────────────────────────────────────────
// The FOG_* ramp above fades fog into fog on cells the client has no terrain
// for, so the only hard edge left was the one that actually mattered: terrain
// art stopped dead at dist == visR. This pushes the transition *inside* the
// disk — the outermost rings you can still see are drawn normally, then
// veiled toward FOG_NEAR_RGB, so the last visible ring meets the first fogged
// ring at roughly the same colour and there is no cliff anywhere.
//
// Unlike the fog fill, this veil IS a globalAlpha blend, and the warning
// above does not apply to it: it composites over terrain art already on the
// canvas, so alpha reveals what is underneath exactly as the name implies.
// The fog fill had nothing beneath it, which is what made alpha useless there.
const SIGHT_FADE_RINGS = 3;
// Veil opacity per fade level (0 = crisp, SIGHT_FADE_RINGS = the sight edge).
const SIGHT_FADE_ALPHA = [0, 0.22, 0.45, 0.78];
const SIGHT_FADE_FILL  = `rgb(${FOG_NEAR_RGB[0]},${FOG_NEAR_RGB[1]},${FOG_NEAR_RGB[2]})`;

// Fade level for a hex inside the vision disk: 0 crisp, SIGHT_FADE_RINGS at
// the edge. The band compresses when visR is smaller than SIGHT_FADE_RINGS so
// it always spans the whole disk instead of running off the inside — at
// visR 1 (Broken Urban, or a CHEM phase) your own hex stays crisp and the one
// ring around it is fully hazed. That is deliberate: low-vision terrain
// should look different, not merely smaller.
// ── Explored-terrain veil ────────────────────────────────
// Ground you have walked and left behind (see memoryCells in map-decoder.js).
// Deliberately a COLD wash, where the sight fade above is a warm one: the two
// tiers sit next to each other constantly, and hue separates them at a glance
// where another step of brightness would just read as more distance. Warm and
// dim = the edge of what you can see; cold and flat = what you are recalling.
// Mutable so it can be dialled from the console while looking at the board:
// setExploredVeil(0.9). `fill` is the pre-composed string the hot loop reads
// — at minimum zoom the terrain pass touches ~2000 cells a frame and most of
// them are remembered, so it must not rebuild an rgba() string per hex.
//
// The first pass at this was 0.62 and it was far too generous: remembered
// hexes read nearly as clearly as live ones, which quietly cancels the whole
// point of a small vision radius — the board looked fully explored during a
// chem storm. Memory should tell you the shape of the ground, not let you
// read it.
const EXPLORED_VEIL = { rgb: '24, 28, 38', alpha: 0.90, fill: 'rgba(24, 28, 38, 0.90)' };

// Per-phase override of the veil's hue, indexed by weatherPhase. Chem lights
// the whole sky green; a cold blue memory tier underneath read as two
// unrelated effects stacked on one board rather than one poisoned landscape.
// Only the hue moves — the alpha stays wherever setExploredVeil() put it, so
// remembered ground is no more legible under chem than under clear sky.
const EXPLORED_VEIL_PHASE_RGB = { 3: '14, 36, 22' };   // 3 = CHEM

// Precomputed per phase: the terrain pass touches ~2000 cells a frame and
// must not rebuild an rgba() string per hex.
let EXPLORED_VEIL_FILL = [];
function _rebuildExploredVeilFills() {
  EXPLORED_VEIL_FILL = Array.from({ length: 6 }, (_, ph) =>
    `rgba(${EXPLORED_VEIL_PHASE_RGB[ph] || EXPLORED_VEIL.rgb},${EXPLORED_VEIL.alpha})`);
  EXPLORED_VEIL.fill = EXPLORED_VEIL_FILL[0];
}
function setExploredVeil(alpha) {
  EXPLORED_VEIL.alpha = alpha;
  _rebuildExploredVeilFills();
}
_rebuildExploredVeilFills();

function sightFadeLevel(dist, vr) {
  if (vr <= 0 || dist <= 0) return 0;
  const span  = Math.min(vr, SIGHT_FADE_RINGS);
  const inner = vr - span;
  if (dist <= inner) return 0;
  return Math.min(SIGHT_FADE_RINGS,
                  Math.round(SIGHT_FADE_RINGS * (dist - inner) / span));
}

// WebSocket connection
const WIFI_CREDS_SEND_DELAY_MS = 300; // delay before auto-sending WiFi creds

// Reconnect backoff: base * 2^attempts, capped, ± jitter — spreads out up to
// 6 clients reconnecting at once (e.g. right after a board reboot) instead of
// all retrying in lockstep on a flat interval.
const RECONNECT_BASE_MS       = 1000;
const RECONNECT_MAX_MS        = 8000;
const RECONNECT_JITTER_PCT    = 0.25;

// Dropped move/act input while disconnected: replayed on reconnect (silent
// auto-replay), bounded so a long outage doesn't replay stale intent.
const PENDING_ACTION_MAX      = 3;    // a couple of clicks, not a backlog
const PENDING_ACTION_TTL_MS   = 3000;

// Staleness watchdog: broadcastState() is unconditional every 100ms
// server-side, so no message for this long means the connection is half-dead
// even though the browser hasn't noticed yet.
const WS_STALE_THRESHOLD_MS      = 3000;
const WS_STALE_CHECK_INTERVAL_MS = 2000;

// ── Shared animation state (written by network, read by renderer) ─
let maxMP    = 6;   // plain copy used by non-reactive rendering (time-of-day clock)
let nightFade = 0;  // extra night overlay that fades out after dawn
let displayMP = 6;  // smoothly lerped toward uiMP.val each frame

// ── Image Loading Utility ─────────────────────────────────────────
function createImageWithLoadTracking(src) {
  // Route through index.html's AssetLoader queue when present: bounded
  // concurrency + retries against the K10 (a bare `new Image()` per hex
  // variant fired ~110 requests at once after the first sync and wedged the
  // board). img.dataset.src keeps the original path; img.src becomes a blob:
  // URL once fetched. Falls back to a plain Image outside the game page.
  const img = window.AssetLoader ? AssetLoader.image(src) : new Image();
  img.loaded = false;
  if (!img.dataset.src) img.dataset.src = src;
  if (!window.AssetLoader) img.src = src;
  img.onload = () => { img.loaded = true; };
  img.onerror = () => {
    img.loaded = false;
  };
  return img;
}

// ── UI glyph sprite strip (terrain/resource/overlay pixel glyphs) ──
const glyphImg = createImageWithLoadTracking('/' + GLYPH_SHEET);

// ── Terrain hex images ────────────────────────────────────────────
// Naming: /img/hex<Name><N>.png  (e.g. hexOpenScrub0.png, hexOpenScrub1.png)
// terrainImgVariants[terrain][variant] → Image object (or undefined if missing).
// Populated by loadTerrainVariants(vc) when the sync message arrives.
const TERRAIN_IMG_NAMES = [
  'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
  'BrokenUrban', 'FloodedDistrict', 'GlassFields',
  'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
  'BunkerEntrance', 'VentShaft', 'TunnelFloor', 'TunnelCollapsed',
];
const terrainImgVariants = Array.from({ length: NUM_TERRAIN }, () => []);

function loadTerrainVariants(vc) {
  // Counts are static for the whole session (fixed at boot from the SD card
  // scan) — now arrives on both 'lobby' (on connect) and 'sync' (on pick), so
  // guard against rebuilding every array and re-fetching every image twice.
  if (terrainImgVariants.some(a => a.length)) return;
  for (let t = 0; t < NUM_TERRAIN; t++) {
    const name = TERRAIN_IMG_NAMES[t];
    const count = vc?.[t] || 0;
    terrainImgVariants[t] = Array.from(
      { length: count },
      (_, v) => createImageWithLoadTracking(`/img/hex${name}${v}.png`)
    );
  }
}

// ── Point-of-interest landmark art ─────────────────────────────────
// Named art for a specific guaranteed-encounter hex, keyed by the
// "terrain_variant" the firmware pins on that hex (see hex-map.hpp Phase
// 5.5). Loaded directly by filename, independent of the per-terrain
// variant pool above — each entry is one fixed image for one fixed
// landmark, not a randomly-chosen variant.
const POI_ART = {
  '0_10': createImageWithLoadTracking('/img/poi_jacks_chopper.png'),  // Jack's Chopper — scrub/19.json
};
function poiArtFor(terrain, variant) {
  return POI_ART[`${terrain}_${variant}`];
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
  if (forrageAnimalImgs.length) return;  // static for the session — see loadTerrainVariants
  forrageAnimalImgs = Array.from(
    { length: count },
    (_, v) => createImageWithLoadTracking(`/img/forrageAnimal${v}.png`)
  );
}

// ── Shelter images ────────────────────────────────────────────────
// Naming: /img/shelterBasic<N>.png, /img/shelterImproved<N>.png
// shelterImgs[0] = basic variants, shelterImgs[1] = improved variants
const shelterImgs = [];
const SHELTER_IMG_NAMES = ['shelterBasic', 'shelterImproved'];

function loadShelterVariants(sv) {
  if (shelterImgs.some(a => a?.length)) return;  // static for the session — see loadTerrainVariants
  for (let s = 0; s < 2; s++) {
    const count = sv?.[s] || 0;
    shelterImgs[s] = Array.from(
      { length: count },
      (_, v) => createImageWithLoadTracking(`/img/${SHELTER_IMG_NAMES[s]}${v}.png`)
    );
  }
}

// ── State ───────────────────────────────────────────────────────
let myId = -1;
// gameMap[r][q] = { terrain, resource, amount } or null (fogged/unknown)
let gameMap = Array.from({ length: MAP_ROWS }, () => new Array(MAP_COLS).fill(null));
let players = Array.from({ length: MAX_PLAYERS }, (_, i) => ({
  id: i, on: false, q: 0, r: 0, sc: 0, nm: `Survivor${i}`,
  inv: [0,0,0,0,0], sp: 0,
  // Survivor fields
  ll: 7, food: 6, water: 6, rad: 0,
  arch: 0, is: 8,
  eq: [0,0,0,0,0],
  sk: [0,0,0,0,0],
  wnd: [0,0],           // wounds: [minor, major] — NB: act events use `wd` for water delta
  it: new Array(12).fill(0),
  iq: new Array(12).fill(0),
  // §4 Resource economy
  fth: 0, wth: 0, mp: 6,
  // §5 Action tracking
  rest: false,
  // §6 Encounter
  enc: false,
}));

// Shared game state (Threat Clock, Day, weather phase) — from gs on sync/state
let gameState = { tc: 0, dc: 0, wp: 0 };
// World system entities (Caravan/Fire/Doom) — from world on sync/state.
// Plain global like gameState, not VanJS-reactive. Partial payloads are
// normal: Phase 1 only ever sends `caravan`, so `doom`/`fire` stay at these
// defaults until later phases add those keys server-side.
let worldState = { caravan: null, doom: null, fire: [] };
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
  globalThis.__gameState = {
    myId,
    day:     gameState.dc,
    tc:      gameState.tc,
    visR:    getEffectiveVR(),
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
