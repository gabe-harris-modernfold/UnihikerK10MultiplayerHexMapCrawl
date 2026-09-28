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
// 5 clients reconnecting at once (e.g. right after a board reboot) instead of
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
  // Listeners, not onload/onerror: callers overwrite those (the atlas pages do).
  // Settle on a later task: this listener runs BEFORE the onload handler that
  // sets img.loaded (and builds the atlas mips), and promise reactions run
  // between listeners, so settling here would release the boot screen early.
  artLoads.push(new Promise(res => {
    const settle = () => setTimeout(() => { artSettledCount++; res(); });
    img.addEventListener('load', settle, { once: true });
    img.addEventListener('error', settle, { once: true });
  }));
  if (!window.AssetLoader) img.src = src;
  img.onload = () => { img.loaded = true; };
  img.onerror = () => {
    img.loaded = false;
  };
  return img;
}

// ── Boot art gate ─────────────────────────────────────────────────
// index.html's boot screen holds until every image asked for so far has
// loaded or failed, so the character picker never opens onto blank pawns and
// flat hexes. One promise per createImageWithLoadTracking() call.
const artLoads = [];
let artSettledCount = 0;
let tilesQueued = Promise.resolve();   // tiles.json -> atlas pages is async; see loadTerrainVariants

async function artSettled(onProgress) {
  await tilesQueued;
  if (onProgress) onProgress(artSettledCount, artLoads.length);
  const tick = onProgress ? setInterval(() => onProgress(artSettledCount, artLoads.length), 150) : 0;
  try {
    // A load can queue another (tiles.json -> pages), so re-check the length.
    for (let n = -1; n !== artLoads.length;) {
      n = artLoads.length;
      await Promise.all(artLoads.slice());
    }
  } finally {
    clearInterval(tick);
  }
  if (onProgress) onProgress(artSettledCount, artLoads.length);
}

// ── UI glyph sprite strip (terrain/resource/overlay pixel glyphs) ──
const glyphImg = createImageWithLoadTracking('/' + GLYPH_SHEET);

// ── Terrain tiles ─────────────────────────────────────────────────
// scripts/hex_sheets.py packs each terrain's tiles into one sheet
// (/img/hex<Name>.webp, from the masters in art/hex-sheets/) plus
// /img/tiles.json, which says where each terrain's variants and each pinned
// landmark sit. 15 requests and PSRAM cache slots on the K10 instead of ~80 --
// the firmware reads the same manifest's `counts` to size pickVariant().
//
// Sheet tiles are `flat`: a square cell with the hex at full width, drawn
// exactly like the per-file tiles were (grid over them, nothing overhangs).
// The manifest format also carries the rejected 3/4 dioramas of
// scripts/tilegen/build_tiles.py: a cell taller than its hex, the headroom
// above holding whatever stands up into the hex behind, the hex edge baked in
// -- tile.diorama, for which renderer.js splits the overhang off and skips
// the grid.
//
// No manifest (an older board, or a failed fetch) falls back to the per-file
// /img/hex<Name><N>.png tiles the board counted into `vc`.
const TERRAIN_IMG_NAMES = [
  'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
  'BrokenUrban', 'FloodedDistrict', 'GlassFields',
  'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
  'BunkerEntrance', 'VentShaft', 'TunnelFloor', 'TunnelCollapsed',
];
const terrainImgVariants = Array.from({ length: NUM_TERRAIN }, () => []);   // legacy per-file tiles
// state: idle -> loading -> atlas | legacy. cell/anchor/radius are atlas px.
// shelters: [basic, improved] lists of [page, sx, sy]; forage: one such list.
// Null when the manifest has none (then they load per file, see
// loadShelterVariants / loadForrageAnimalImgs).
// caravan: one such list too, the convoy sticker (caravanSprite). Null
// without it: renderCaravan() falls back to its plain canvas badge.
// tunnel: the bunker board's corridor/room/fixture cells (tiles.json "tunnel",
// drawn through tunnelTile() in tunnel-board.js), null without them.
const tileAtlas = { state: 'idle', pages: [], cell: [224, 272], anchor: [112, 167], radius: 112, tiles: [], poi: {}, flat: false,
                    shelters: null, shelterCell: [224, 224], forage: null, forageCell: [80, 80],
                    caravan: null, caravanCell: [384, 384], tunnel: null };

function loadTerrainVariants(vc) {
  // Counts are static for the whole session (fixed at boot from the SD card)
  // and arrive on both 'lobby' (on connect) and 'sync' (on pick), so only
  // the first call does anything.
  if (tileAtlas.state !== 'idle') return;
  tileAtlas.state = 'loading';
  const req = window.AssetLoader
    ? AssetLoader.fetch('/img/tiles.json', { cache: 'no-cache', attempts: 3 })
    : fetch('/img/tiles.json', { cache: 'no-cache' }).then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r; });
  tilesQueued = req.then(r => r.json()).then(m => {
    if (!m || !Array.isArray(m.pages) || !m.pages.length) throw new Error('tiles.json lists no pages');
    Object.assign(tileAtlas, { cell: m.cell, anchor: m.anchor, radius: m.radius, tiles: m.tiles || [], poi: m.poi || {}, flat: !!m.flat,
                               shelters: Array.isArray(m.shelters) ? m.shelters : null, shelterCell: m.shelterCell || tileAtlas.shelterCell,
                               forage: Array.isArray(m.forage?.[0]) ? m.forage[0] : null, forageCell: m.forageCell || tileAtlas.forageCell,
                               caravan: m.caravan?.[0]?.length ? m.caravan[0] : null, caravanCell: m.caravanCell || tileAtlas.caravanCell,
                               tunnel: Array.isArray(m.tunnel?.corridor) ? m.tunnel : null });
    tileAtlas.pages = m.pages.map(p => {
      // ?v= is the atlas hash: /img/* is cached forever (sw.js, and the
      // board's immutable Cache-Control), so a rebuilt atlas is a new URL.
      const img = createImageWithLoadTracking(`/img/${p}?v=${encodeURIComponent(m.version || '')}`);
      img.onload = () => { img.loaded = true; buildTileMips(img); };
      return img;
    });
    tileAtlas.state = 'atlas';
  }).catch(e => {
    console.warn('[tiles] no atlas (%s) — loading per-file tiles', e && e.message);
    tileAtlas.state = 'legacy';
    for (let t = 0; t < NUM_TERRAIN; t++) {
      const name = TERRAIN_IMG_NAMES[t];
      terrainImgVariants[t] = Array.from(
        { length: vc?.[t] || 0 },
        (_, v) => createImageWithLoadTracking(`/img/hex${name}${v}.png`)
      );
    }
    POI_ART['0_10'] = createImageWithLoadTracking('/img/poi_jacks_chopper.png');  // Jack's Chopper — scrub/19.json
  });
}

// Half- and quarter-size copies of each atlas page, made once on decode.
// Canvas drawImage shrinks with a 2x2 bilinear tap, so a 224 px tile
// squeezed into a 50 px hex shimmers; sampling a pre-shrunk page doesn't.
function buildTileMips(img) {
  const mips = [];
  let src = img, w = img.naturalWidth, h = img.naturalHeight;
  for (let i = 0; i < 2 && w > 64; i++) {
    w = Math.max(1, w >> 1);
    h = Math.max(1, h >> 1);
    const c = document.createElement('canvas');
    c.width = w;
    c.height = h;
    const g = c.getContext('2d');
    g.imageSmoothingEnabled = true;
    g.imageSmoothingQuality = 'high';
    g.drawImage(src, 0, 0, w, h);
    mips.push(c);
    src = c;
  }
  img.mips = mips;
}

// ── Point-of-interest landmark art ─────────────────────────────────
// Named art for a specific guaranteed-encounter hex, keyed by the
// "terrain_variant" the firmware pins on that hex (see hex-map.hpp Phase
// 5.5). In the atlas these are tiles.json `poi` entries; this table is only
// filled on the per-file fallback.
const POI_ART = {};
function poiArtFor(terrain, variant) {
  return POI_ART[`${terrain}_${variant}`];
}

// The tile to draw for a cell, or null (no art, or not decoded yet: the
// caller draws the flat fallback). Atlas tiles are { page, sx, sy, diorama };
// the per-file fallback is { img }.
function terrainTile(terrain, variant) {
  if (tileAtlas.state === 'atlas') {
    const pool = tileAtlas.tiles[terrain];
    // Wrap like the old pool did: a pinned variant with no entry of its own
    // degrades to an ordinary tile of its terrain.
    const at = tileAtlas.poi[`${terrain}_${variant}`] ||
               (pool?.length ? pool[((variant % pool.length) + pool.length) % pool.length] : null);
    const page = at && tileAtlas.pages[at[0]];
    return page?.loaded ? { page, sx: at[1], sy: at[2], diorama: !tileAtlas.flat } : null;
  }
  const _tv = terrainImgVariants[terrain];
  const img = poiArtFor(terrain, variant) ||
              (_tv?.length > 0 ? (_tv[variant % _tv.length] || _tv[0]) : null);
  return img?.loaded ? { img } : null;
}

// Top-left and size of a tile's box for the hex at (cx, cy), radius `size`.
function tileBox(cx, cy, size) {
  const k = size / tileAtlas.radius;
  return { x: cx - tileAtlas.anchor[0] * k, y: cy - tileAtlas.anchor[1] * k,
           w: tileAtlas.cell[0] * k, h: tileAtlas.cell[1] * k };
}

function drawTerrainTile(g, tile, cx, cy, size) {
  if (tile.img) {                      // per-file tile: a 2*size square, as ever
    g.drawImage(tile.img, cx - size, cy - size, size * 2, size * 2);
    return;
  }
  const [cw, ch] = tileAtlas.cell;
  if (tile.rot || tile.mirror) {
    // A tunnel corridor piece turned onto its cell (tunnelTile): rot steps of
    // 60 degrees and a mirror, about the hex centre. A flat-top hex maps onto
    // itself under both, so the piece still fills its hex exactly.
    g.save();
    g.translate(cx, cy);
    if (tile.rot) g.rotate(-tile.rot * Math.PI / 3);
    if (tile.mirror) g.scale(-1, 1);
    const b = tileBox(0, 0, size);
    drawAtlasCell(g, tile.page, tile.sx, tile.sy, cw, ch, b.x, b.y, b.w, b.h);
    g.restore();
    return;
  }
  const b = tileBox(cx, cy, size);
  drawAtlasCell(g, tile.page, tile.sx, tile.sy, cw, ch, b.x, b.y, b.w, b.h);
}

// One sw x sh cell of an atlas page into (dx, dy, dw, dh). Shrunk past 2x it
// samples the half- or quarter-size copy instead (buildTileMips), which is
// why every sheet's cells sit on multiples of 4.
function drawAtlasCell(g, page, sx, sy, sw, sh, dx, dy, dw, dh) {
  const px = (dw / sw) * (window.devicePixelRatio || 1);   // device px per atlas px
  const mips = page.mips;
  if (mips?.length && px < 0.5) {
    const lvl = (px < 0.25 && mips[1]) ? 1 : 0;
    const f = 2 << lvl;
    g.drawImage(mips[lvl], sx / f, sy / f, sw / f, sh / f, dx, dy, dw, dh);
    return;
  }
  g.drawImage(page, sx, sy, sw, sh, dx, dy, dw, dh);
}

// ── Survivor pawn portrait images ────────────────────────────────
// Indexed by archetype: 0=Guide 1=Quartermaster 2=Medic 3=Mule 4=Scout 5=Endurer
const pawnImgs = ARCHETYPES.map(a =>
  createImageWithLoadTracking(`img/survivors/${a.name.toLowerCase()}Pawn.jpg`)
);

// ── Forage animal images ──────────────────────────────────────────
// Shown on cells with food resource (type 2). One sheet,
// /img/forrageAnimal.webp (scripts/hex_sheets.py), placed by tiles.json
// `forage`; without it (an older board) /img/forrageAnimal<N>.png per file.
let forrageAnimalImgs = [];
let forageAsked = false;
const collectedCells = new Set(); // cells cleared by 'col' — guards against vis disk overwrite

function loadForrageAnimalImgs(count) {
  if (forageAsked) return;  // static for the session — see loadTerrainVariants
  forageAsked = true;
  tilesQueued.then(() => {  // after tiles.json has had its say, as loadShelterVariants
    if (tileAtlas.forage) return;
    forrageAnimalImgs = Array.from(
      { length: count },
      (_, v) => createImageWithLoadTracking(`/img/forrageAnimal${v}.png`)
    );
  });
}

// The forage animal for a food hex, or null (none, or not decoded yet),
// picked by position. Sheet sprites are { page, sx, sy, s }; per file { img }.
function forageSprite(q, r) {
  const pick = n => (((q * 31 + r * 17) % n) + n) % n;
  if (tileAtlas.forage) {
    const pool = tileAtlas.forage;
    const at = pool.length ? pool[pick(pool.length)] : null;
    const page = at && tileAtlas.pages[at[0]];
    return page?.loaded ? { page, sx: at[1], sy: at[2], s: tileAtlas.forageCell[0] } : null;
  }
  const img = forrageAnimalImgs.length ? forrageAnimalImgs[pick(forrageAnimalImgs.length)] : null;
  return img?.loaded ? { img } : null;
}

// ── Shelter images ────────────────────────────────────────────────
// One sheet per kind (/img/shelterBasic.webp, /img/shelterImproved.webp, from
// scripts/hex_sheets.py), placed by tiles.json `shelters`. Without them (an
// older board) they load per file: /img/shelterBasic<N>.png and
// /img/shelterImproved<N>.png, shelterImgs[0] = basic, [1] = improved.
const shelterImgs = [];
const SHELTER_IMG_NAMES = ['shelterBasic', 'shelterImproved'];
let sheltersAsked = false;

function loadShelterVariants(sv) {
  if (sheltersAsked) return;  // static for the session — see loadTerrainVariants
  sheltersAsked = true;
  // After tiles.json has had its say: the lobby asks for terrain first.
  tilesQueued.then(() => {
    if (tileAtlas.shelters) return;
    for (let s = 0; s < 2; s++) {
      const count = sv?.[s] || 0;
      shelterImgs[s] = Array.from(
        { length: count },
        (_, v) => createImageWithLoadTracking(`/img/${SHELTER_IMG_NAMES[s]}${v}.png`)
      );
    }
  });
}

// The shelter sprite for a hex, or null (none, or not decoded yet). kind is
// cell.shelter: 1 basic, 2 improved. There is no variant on the wire, so the
// pick is by position. Sheet sprites are { page, sx, sy, s }; the per-file
// fallback is { img }.
function shelterSprite(kind, q, r) {
  const pick = n => (((q * 31 + r * 17) % n) + n) % n;
  if (tileAtlas.shelters) {
    const pool = tileAtlas.shelters[kind - 1];
    const at = pool?.length ? pool[pick(pool.length)] : null;
    const page = at && tileAtlas.pages[at[0]];
    return page?.loaded ? { page, sx: at[1], sy: at[2], s: tileAtlas.shelterCell[0] } : null;
  }
  const imgs = shelterImgs[kind - 1];
  const img = imgs?.length ? imgs[pick(imgs.length)] : null;
  return img?.loaded ? { img } : null;
}

// ── Caravan sticker ───────────────────────────────────────────────
// /img/caravan.webp (scripts/hex_sheets.py), placed by tiles.json
// `caravan`: the APC-led convoy, painted heading towards the viewer's lower
// left. Only on a board synced with the sheets -- there is no per-file
// fallback, renderCaravan() draws its canvas badge instead. Returns
// { page, sx, sy, s } or null (none, or not decoded yet).
function caravanSprite() {
  const at = tileAtlas.caravan?.[0];
  const page = at && tileAtlas.pages[at[0]];
  return page?.loaded ? { page, sx: at[1], sy: at[2], s: tileAtlas.caravanCell[0] } : null;
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
// Remains -- where survivors fell and the tokens they left -- from the same
// messages ("rm"). Their items are ordinary groundItems piles on that hex.
let remains = [];

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
