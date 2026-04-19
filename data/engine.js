// ── Vision radius (updated per vis/sync message from server) ─────
let myVisionR = VISION_R;

// ── Weather phase (0=Clear 1=Rain 2=Storm 3=Chem; updated from server gs.wp) ─
let weatherPhase = 0;
const weatherParticles = (typeof WeatherParticleSystem !== 'undefined')
  ? new WeatherParticleSystem() : null;

// ── VanJS reactive UI state ───────────────────────────────────────
const uiConn        = van.state('Connecting...');
const uiInv         = Array.from({length:5}, () => van.state(0));
const uiScore       = van.state(0);
const uiPos         = van.state('Q:—  R:—');
const uiPlayers     = van.state([]);
// Char sheet live stats
const uiSteps       = van.state(0);
const uiVision      = van.state(VISION_R);
// Terrain card reactive cell
const uiCurrentCell = van.state(null);
// Cooldown ring (0=ready, 1=freshly moved)
const uiCooldown    = van.state(0);
// §4 Survival tracks
const uiLL          = van.state(7);
const uiFood        = van.state(6);
const uiWater       = van.state(6);
const uiMP          = van.state(6);
// §6.2 Radiation track
const uiRad         = van.state(0);
// §5 Action tracking
const uiResting     = van.state(false);  // true after REST until dawn
const uiHasCond     = van.state(false);  // true when player has treatable conditions
let maxMP = 6;           // plain copy used by non-reactive rendering (time-of-day clock)
const uiMaxMP = van.state(6);  // reactive — drives MP track box count in HUD
// Menu navigation (null=closed, 'main'|'howto'|'settings'|'about')
const uiMenuPage    = van.state(null);
// Log panel visibility (persisted to localStorage)
const uiLogVisible  = van.state(localStorage.getItem('logVisible') === '1');
// K10 screen flip (persisted to localStorage)
const uiScreenFlip  = van.state(localStorage.getItem('k10_screenFlip') === '1');
// Overlay open states — toggled via .val; van.derive in ui.js handles class changes
const uiHexInfoOpen = van.state(false);
const uiCharOpen    = van.state(false);
van.derive(() => {
  const lp = document.getElementById('log-panel');
  if (lp) lp.style.display = uiLogVisible.val ? '' : 'none';
});
function openMenu(page = 'main') { uiMenuPage.val = page; }
function closeMenu()             { uiMenuPage.val = null; }
function narrateState(msg) {
  const el = document.getElementById('game-narrator');
  if (el) el.textContent = msg;
}

// ── Narrative effect system (EFX_NARRATIVE item params) ──────────────────
let _trippyTurns = 0;       // turns remaining with .trippy class active
let invertedInputTurns = 0; // turns remaining with reversed WASD
function handleNarrativeEffect(param) {
  if (!param || param === 0) return;
  switch (param) {
    case 11: // Trippy Juice — UI hue-scramble for 3 turns
      document.body.classList.add('trippy');
      _trippyTurns = 3;
      showToast('\uD83C\uDF00 Reality bends. The map will look... different for a while.');
      break;
    case 12: // Cursed Device — reverse movement keys for 5 turns
      invertedInputTurns = 5;
      showToast('\u26A0\uFE0F Movement keys reversed for 5 turns.');
      break;
    case 27: // Jar of Sweats — vomit, lose turn
      showToast('\uD83E\uDD22 Your body rejects the experience. You lose your next turn.');
      break;
    case 29: // Uranium Hard Candy — warn about LL ceiling
      showToast('\u2622\uFE0F The radiation settles in permanently. Your maximum health has decreased.');
      break;
    default:
      break;
  }
}
// Clear trippy effect when a new day starts (called on dawn event)
function _tickNarrativeEffects() {
  if (_trippyTurns > 0) { _trippyTurns--; if (_trippyTurns === 0) document.body.classList.remove('trippy'); }
  if (invertedInputTurns > 0) invertedInputTurns--;
}
// Lobby / character selection
const lobbyAvail    = van.state([]);    // archetype indices currently available to pick
const uiPickPending = van.state(false); // true while waiting for server pick response
let pickTimeoutId = null;               // safety: auto-clear uiPickPending if server never responds

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

// ── Client Diagnostics ───────────────────────────────────────────────────────
// Access via window.diag.report() or window.diag.data in the browser console.
// Auto-dumps every 60 s; also dumps on every disconnect and WS error.
const Diag = (() => {
  const d = {
    sessionStart:       Date.now(),
    wsConnections:      0,
    wsDisconnects:      0,
    wsErrors:           0,
    droppedSends:       0,
    msgSent:            0,
    msgReceived:        0,
    msgByType:          {},
    lastConnectedAt:    null,
    lastDisconnectedAt: null,
    lastMsgAt:          null,
    responseTimes:      [],  // RTT samples (ms), capped at 100
    _pendingActionAt:   null,
  };

  function avgRtt() {
    if (!d.responseTimes.length) return null;
    return Math.round(d.responseTimes.reduce((a, b) => a + b, 0) / d.responseTimes.length);
  }

  function wsState() {
    if (typeof socket === 'undefined' || !socket) return 'no socket';
    return ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'][socket.readyState] ?? '?';
  }

  function report() {
    const now   = Date.now();
    const upSec = d.lastConnectedAt ? Math.round((now - d.lastConnectedAt) / 1000) : null;
    const ageSec = Math.round((now - d.sessionStart) / 1000);
    const lastMsgAgo = d.lastMsgAt ? Math.round((now - d.lastMsgAt) / 1000) + 's ago' : '—';
    const myPos = (typeof myId !== 'undefined' && myId >= 0 &&
                   typeof players !== 'undefined' && players[myId])
                  ? `q:${players[myId].q} r:${players[myId].r}` : '—';
    const connCount = (typeof players !== 'undefined')
                      ? players.filter(p => p?.on).length : '—';

    console.group('%c[DIAG] Wasteland Crawl — Client Diagnostics', 'color:#4fc;font-weight:bold');
    console.log(`Session age      : ${ageSec}s`);
    console.log(`WS state         : ${wsState()}`);
    console.log(`Connections      : ${d.wsConnections}  |  Disconnects: ${d.wsDisconnects}  |  Errors: ${d.wsErrors}`);
    console.log(`Current uptime   : ${upSec != null ? upSec + 's' : '—'}`);
    console.log(`Dropped sends    : ${d.droppedSends}`);
    console.log(`Msgs sent        : ${d.msgSent}  |  Received: ${d.msgReceived}`);
    console.log(`Last msg recv    : ${lastMsgAgo}`);
    console.log(`Avg RTT (action→sync): ${avgRtt() != null ? avgRtt() + 'ms' : '—'}  (${d.responseTimes.length} samples)`);
    console.log(`Min / Max RTT    : ${d.responseTimes.length ? Math.min(...d.responseTimes) + 'ms / ' + Math.max(...d.responseTimes) + 'ms' : '—'}`);
    console.log(`Player ID        : ${typeof myId !== 'undefined' ? myId : '—'}`);
    console.log(`My position      : ${myPos}`);
    console.log(`Connected players: ${connCount}`);
    console.table(d.msgByType);
    console.groupEnd();
  }

  function onSend(obj) {
    d.msgSent++;
    if (obj.t === 'move' || obj.t === 'act') d._pendingActionAt = Date.now();
  }

  function onDropped() {
    d.droppedSends++;
    console.warn(`[DIAG] Dropped send — socket not open (total dropped: ${d.droppedSends}, ws: ${wsState()})`);
  }

  function onMsg(t) {
    d.msgReceived++;
    d.lastMsgAt = Date.now();
    d.msgByType[t] = (d.msgByType[t] || 0) + 1;
    if (d._pendingActionAt && (t === 'sync' || t === 'mov' || t === 'asgn')) {
      const rtt = Date.now() - d._pendingActionAt;
      d.responseTimes.push(rtt);
      if (d.responseTimes.length > 100) d.responseTimes.shift();
      d._pendingActionAt = null;
    }
  }

  function onConnect() {
    d.wsConnections++;
    d.lastConnectedAt = Date.now();
    console.info(`[DIAG] WS connected (#${d.wsConnections}) at ${new Date().toISOString()} — prior disconnects: ${d.wsDisconnects}`);
  }

  function onDisconnect() {
    d.wsDisconnects++;
    d.lastDisconnectedAt = Date.now();
    console.warn(`[DIAG] WS closed (#${d.wsDisconnects}) — dropped sends total: ${d.droppedSends}`);
    report();
  }

  function onError(ev) {
    d.wsErrors++;
    console.error(`[DIAG] WS error #${d.wsErrors} at ${new Date().toISOString()}`, ev);
    report();
  }

  setInterval(report, 60_000);
  window.diag = { report, data: d, avgRtt };

  return { onSend, onDropped, onMsg, onConnect, onDisconnect, onError, report };
})();

// ── Move cooldown tracking (client-side estimate) ─────────────────
let lastMoveSent   = 0;   // Date.now() of last move sent
let moveCooldownMs = MOVE_COOLDOWN_BASE_MS; // mirrors server MOVE_CD_MS × current terrain MC
let restSent       = false; // guard against REST double-click before server ack

// ── Smooth animation ─────────────────────────────────────────────
const renderPos = Array.from({ length: MAX_PLAYERS }, () => ({ q: 0.0, r: 0.0 }));

// ── Name-tag width cache ──────────────────────────────────────────
// Avoids a ctx.measureText() call every frame per visible player.
// Keyed by player index; invalidated when tag text or font size changes.
const nameWidthCache = new Array(MAX_PLAYERS).fill(null);

// ── Surveyed cells ────────────────────────────────────────────────
// Cells revealed by the SURVEY action beyond normal vision radius.
// Rendered at reduced opacity as scouted-but-not-directly-visible.
// Cleared whenever the local player moves.
const surveyedCells = new Set(); // 'q_r' keys

// ── Image Loading Utility ─────────────────────────────────────────
/**
 * Factory function to create an Image object with load tracking.
 * Sets up onload/onerror callbacks to track loading state.
 * @param {string} src - Image URL to load
 * @returns {Image} Image object with .loaded property (boolean)
 */
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

/**
 * Load terrain variant images for all terrain types.
 * @param {Array<number>|null} vc - Variant counts per terrain type
 */
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

/**
 * Load forage animal images.
 * @param {number} count - Number of forage animal variants
 */
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

/**
 * Load shelter variant images for both basic and improved types.
 * @param {Array<number>|null} sv - Variant counts [basic_count, improved_count]
 */
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
  arch: 0, sb: 0, is: 8,
  sk: [0,0,0,0,0,0],
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
      statusBits: me.sb,
      score: me.sc, name: me.nm,
    } : null,
    players: players.filter(p => p.on && p.id !== myId).map(p => ({
      id: p.id, name: p.nm, q: p.q, r: p.r, ll: p.ll, mp: p.mp,
    })),
    visibleCells,
  };
}

// ── Character selection overlay helpers ──────────────────────────
function showCharSelect() {
  // Guard: never show char-select while a live survivor exists
  if (myId >= 0 && players[myId]?.on) {
    console.log('[charSelect] BLOCKED by guard: myId=%d on=%s', myId, players[myId]?.on);
    return;
  }
  console.log('[charSelect] SHOW — myId=%d lobbyAvail=%o', myId, lobbyAvail.val);
  // Hide the connecting overlay (behind char-select anyway, but clean it up)
  const co = document.getElementById('connect-overlay');
  if (co) { co.classList.add('fading-out'); setTimeout(() => { co.classList.remove('fading-out'); co.classList.add('hidden'); }, 400); }
  document.getElementById('char-select-overlay').classList.add('open');
}
function hideCharSelect() {
  document.getElementById('char-select-overlay').classList.remove('open');
  // Return keyboard focus to the game canvas immediately
  setTimeout(() => document.getElementById('canvas-wrap')?.focus({ preventScroll: true }), 50);
}

// ── WebSocket ────────────────────────────────────────────────────
const wsUrl = `ws://${location.host}/ws`;
let socket;

// Set to true once server confirms it has saved creds; prevents redundant auto-sends.
let serverHasWifiCreds = false;
let pendingLobbyRedirect = false;  // true while downed-death pause is running

/**
 * Establish WebSocket connection to game server.
 * Handles initial connection setup and auto-reconnection on close.
 */
function connect() {
  socket = new WebSocket(wsUrl);
  socket.onopen    = () => {
    Diag.onConnect();
    setStatus('Connected');
    serverHasWifiCreds = false;  // reset on each new connection
    // Auto-send saved WiFi credentials if server doesn't already have them
    // (server will send {t:'wifi',status:'saved'} if it does).
    setTimeout(() => {
      if (!serverHasWifiCreds) {
        const ssid = localStorage.getItem('wifi_ssid');
        if (ssid) send({ t: 'wifi', ssid, pass: localStorage.getItem('wifi_pass') ?? '' });
      }
    }, WIFI_CREDS_SEND_DELAY_MS);  // brief delay to receive 'saved' message first if server has creds
  };
  socket.onclose   = () => { Diag.onDisconnect(); setStatus('Reconnecting...'); setTimeout(connect, RECONNECT_DELAY_MS); };
  socket.onerror   = (event) => {
    Diag.onError(event);
    setStatus('Connection error — retrying...');
  };
  socket.onmessage = e => { const msg = JSON.parse(e.data); Diag.onMsg(msg?.t); handleMsg(msg); };
}
function send(obj) {
  if (socket && socket.readyState === WebSocket.OPEN) {
    Diag.onSend(obj);
    socket.send(JSON.stringify(obj));
  } else {
    Diag.onDropped();
  }
}


function checkAutoRest() {
  if (myId < 0 || !players[myId]?.on) return;
  if (players[myId].rest) return;           // already resting
  if (uiResting.val) return;
  const others = players.filter(p => p.on && p.id !== myId);
  if (others.length === 0) return;          // solo — don't auto-rest
  if (others.every(p => p.rest)) {
    // All other connected players are resting — auto-rest
    showToast('\ud83d\ude34 Auto-rest — all others are down');
    send({ t: 'act', a: ACT_REST });
  }
}

// ── Message handler ──────────────────────────────────────────────
/**
 * Main WebSocket message router. Processes all server messages and updates game state.
 * Validates message structure and player IDs before updating state.
 * @param {Object} msg - Message object from server with type field (t)
 */
function handleMsg(msg) {
  if (!msg || !msg.t) {
    console.warn('Invalid message: missing type field', msg);
    return;
  }

  switch (msg.t) {
    case 'asgn':
      if (typeof msg.id !== 'number' || msg.id < 0 || msg.id >= MAX_PLAYERS) {
        console.warn('Invalid assignment: bad player ID', msg.id);
        break;
      }
      myId = msg.id;
      if (pickTimeoutId) { clearTimeout(pickTimeoutId); pickTimeoutId = null; }
      uiPickPending.val = false;
      uiResting.val = false;  // clear any stale resting state from the previous survivor
      hideCharSelect();
      break;

    case 'lobby':
      lobbyAvail.val    = Array.isArray(msg.avail) ? msg.avail : [];
      if (pickTimeoutId) { clearTimeout(pickTimeoutId); pickTimeoutId = null; }
      uiPickPending.val = false;
      console.log('[lobby] avail=%o myId=%d pendingLobbyRedirect=%s', lobbyAvail.val, myId, pendingLobbyRedirect);
      if (!pendingLobbyRedirect) {
        if (myId >= 0 && lobbyAvail.val.includes(myId)) {
          // Already assigned and slot still free: auto-repick same slot on reconnect (BUG-01)
          console.log('[lobby] auto-repick myId=%d', myId);
          send({ t: 'pick', arch: myId });
        } else {
          // Slot taken or unassigned — show character select
          if (myId >= 0) { console.log('[lobby] slot taken, resetting myId'); myId = -1; }
          showCharSelect();
        }
      } else {
        console.log('[lobby] suppressed by pendingLobbyRedirect');
      }
      break;

    case 'sync':
      if (msg.id  !== undefined) myId = msg.id;
      if (msg.vr  !== undefined) myVisionR = msg.vr;
      if (typeof msg.map !== 'string') {
        console.warn('Sync message: missing or invalid map data');
        break;
      }
      parseMapFog(msg.map);
      if (!Array.isArray(msg.p)) {
        console.warn('Sync message: player array invalid');
        break;
      }
      msg.p.forEach(p => {
        if (typeof p.id !== 'number' || p.id < 0 || p.id >= MAX_PLAYERS) {
          console.warn('Sync: invalid player ID', p.id);
          return;
        }
        Object.assign(players[p.id], p);
        players[p.id].rest = !!p.rt;  // map rt → rest (mirrors 's' handler)
        if (p.on) { renderPos[p.id].q = p.q; renderPos[p.id].r = p.r; }
      });
      if (msg.vc) loadTerrainVariants(msg.vc);
      if (msg.sv) loadShelterVariants(msg.sv);
      if (msg.fa) loadForrageAnimalImgs(msg.fa);
      if (msg.gs) { Object.assign(gameState, msg.gs); if (msg.gs.wp !== undefined) { weatherPhase = msg.gs.wp; updateWeatherHUD(); } }
      if (Array.isArray(msg.gi)) groundItems = msg.gi;
      hideConnectOverlay();
      if (myId >= 0) hideCharSelect();  // belt-and-suspenders: hide picker if sync arrives before/without asgn
      if (myId >= 0) uiResting.val = !!players[myId].rest;  // sync resting state on reconnect
      updateSidebar();
      if (myId >= 0 && players[myId]?.mp > 0) { maxMP = players[myId].mp; uiMaxMP.val = maxMP; }
      if (myId >= 0) displayMP = players[myId].mp ?? 6;
      updateTerrainCard();
      updateDirButtons();
      // Catch stale downed state on reconnect: if server synced us with ll:0 we are dead.
      // Close the socket so the server resets the slot (p.connected=false) and re-adds us
      // to the lobby on reconnect — otherwise a second pick is rejected as "not in lobby".
      if (myId >= 0 && !pendingLobbyRedirect && players[myId].ll === 0) {
        myId = -1;
        pendingLobbyRedirect = true;
        addLog('<span class="log-check-fail">☠ DOWNED — the wasteland claims you.</span>');
        showToast('☠ YOU HAVE BEEN DOWNED — re-selecting survivor...');
        socket.close();  // triggers server slot reset + auto-reconnect → re-enter lobby
        setTimeout(() => { pendingLobbyRedirect = false; showCharSelect(); }, 3500);
      }
      // Re-render char-sheet if open — ensures wounds/status reflect fresh server state (BUG-10)
      if (document.getElementById('char-overlay')?.classList.contains('open')) {
        renderInventory?.();
        renderEquipment?.();
      }
      break;

    case 's':
      if (!Array.isArray(msg.p)) {
        console.warn('State update: player array invalid');
        break;
      }
      msg.p.forEach((pd, i) => {
        if (i < 0 || i >= MAX_PLAYERS) {
          console.warn('State update: player index out of bounds', i);
          return;
        }
        const p = players[i];
        const wasOn = p.on;
        p.on = pd.on; p.q = pd.q; p.r = pd.r; p.sc = pd.sc;
        // Snap renderPos when a player first comes online so they don't lerp from (0,0) (Bug-4)
        if (pd.on && !wasOn) { renderPos[i].q = pd.q; renderPos[i].r = pd.r; }
        p.inv = pd.inv; p.sp = pd.sp;
        p.ll = pd.ll; p.food = pd.food; p.water = pd.water;
        p.rad = pd.rad; p.sb = pd.sb;

        if (pd.mp  !== undefined) p.mp  = pd.mp;
        if (pd.fth !== undefined) p.fth = pd.fth;   // F threshold bitmask
        if (pd.wth !== undefined) p.wth = pd.wth;   // W threshold bitmask
        if (pd.vm  !== undefined) p.vm  = pd.vm;    // valid move bitmask
        if (pd.rt  !== undefined) {
          p.rest = !!pd.rt;
          if (i === myId) uiResting.val = !!pd.rt;
        }
        if (pd.it) p.it = pd.it;   // typed inventory types
        if (pd.iq) p.iq = pd.iq;   // typed inventory quantities
        if (pd.eq) p.eq = pd.eq;   // equipment slots
        if (pd.enc !== undefined) p.enc = !!pd.enc;  // encounter lock
      });
      // Sync can carry stale eq after item_result; refresh equipment grid only if open.
      // Do NOT call renderInventory here — sync fires on every tick and would hammer
      // the item icon requests on every cycle. Inventory is already up-to-date from item_result.
      if (document.getElementById('char-overlay')?.classList.contains('open')) {
        renderEquipment?.();
      }
      if (msg.gs) { Object.assign(gameState, msg.gs); if (msg.gs.wp !== undefined) { weatherPhase = msg.gs.wp; updateWeatherHUD(); } }
      updateSidebar();
      updateTerrainCard();
      updateDirButtons();
      // Catch missed downed event: if server state shows ll:0 we are dead.
      // Close the socket so the server resets the slot (p.connected=false) and re-adds us
      // to the lobby on reconnect — otherwise a second pick is rejected as "not in lobby".
      if (myId >= 0 && !pendingLobbyRedirect && players[myId].ll === 0) {
        myId = -1;
        pendingLobbyRedirect = true;
        addLog('<span class="log-check-fail">☠ DOWNED — the wasteland claims you.</span>');
        showToast('☠ YOU HAVE BEEN DOWNED — re-selecting survivor...');
        socket.close();  // triggers server slot reset + auto-reconnect → re-enter lobby
        setTimeout(() => { pendingLobbyRedirect = false; showCharSelect(); }, 3500);
      }
      break;

    case 'vis':
      // Fresh vision-disk data after each move; includes effective vision radius
      if (msg.vr !== undefined) myVisionR = msg.vr;
      if (msg.q !== undefined && myId >= 0) {
        players[myId].q = msg.q;
        players[myId].r = msg.r;
      }
      applyVisDisk(msg.cells);
      // Update client-side cooldown estimate from newly revealed terrain
      if (myId >= 0) {
        const _me = players[myId];
        const _cell = gameMap[_me.r]?.[_me.q];
        if (_cell) {
          const _mc = TERRAIN[_cell.terrain]?.mc;
          if (_mc && _mc !== 255) moveCooldownMs = MOVE_COOLDOWN_BASE_MS * _mc;
        }
        // If the current hex still has a resource after the move, collection was
        // blocked. The only server-side reason is a full inventory — notify the player.
        const _cur = gameMap[_me.r]?.[_me.q];
        if (_cur?.resource > 0) {
          const _totalInv = (_me.inv ?? [0,0,0,0,0]).reduce((a, b) => a + (b || 0), 0);
          if (_totalInv >= (_me.sp ?? 6))
            showToast(`\u22a0 Inventory full \u2014 ${RES_NAMES[_cur.resource] ?? 'resource'} left behind`);
        }
        // Auto-trigger encounter: vis fires after applyVisDisk so gameMap is guaranteed fresh.
        if (_cur?.poi) {
          console.log(`[ENC] sending enc_start q=${_me.q} r=${_me.r} t=${Date.now()}`);
          send({ t: 'enc_start', q: _me.q, r: _me.r });
        }
      }
      updateTerrainCard();
      updateSidebar();
      break;

    case 'ev':
      handleEvent(msg);
      break;

    case 'ground_update': {
      // Server sends this as a top-level message when any player drops or picks up an item
      if (Array.isArray(msg.gi)) groundItems = msg.gi;
      if (document.getElementById('hex-info')?.classList.contains('open') && myId >= 0) {
        const me = players[myId];
        renderHexGroundItems?.(me.q, me.r);
      }
      break;
    }

    case 'full':
      document.getElementById('connect-box').innerHTML =
        '<h2>SERVER FULL</h2><p>All 6 slots taken.<br>Try again later.</p>' +
        '<button onclick="location.reload()" class="server-full-retry-btn">\u21BA RETRY</button>';
      break;

    case 'wifi':
      if (msg.status === 'saved') {
        // Server echoed its saved credentials — keep localStorage in sync.
        serverHasWifiCreds = true;
        if (msg.ssid) localStorage.setItem('wifi_ssid', msg.ssid);
        if (msg.pass !== undefined) localStorage.setItem('wifi_pass', msg.pass);
      } else {
        showToast(
          msg.status === 'ok'   ? `\u25CF WiFi connected  ${msg.ip ?? ''}`.trim() :
          msg.status === 'fail' ? '\u25CB WiFi failed \u2014 check credentials'   :
                                  `WiFi: ${msg.status ?? 'unknown'}`
        );
      }
      break;

    case 'enc_path':
      // Direct message: server sends encounter location to the active player
      console.log(`[ENC] enc_path recv biome=${msg.biome} id=${msg.id} t=${Date.now()}`);
      window._startEncounterFetch?.(msg.biome, msg.id);
      break;

    case 'enc_dbg':
      console.warn('[ENC/DBG] server diagnostic:', msg.msg, 't='+Date.now());
      showToast(`\u26A0 ENC DEBUG: ${msg.msg}`);
      break;

    case 'err':
      console.warn('[ENC/ERR] Server error:', msg);
      showToast(`\u22a0 ${msg.msg ?? 'Server error'}`);
      break;
  }
  buildAgentState();
}

function handleEvent(ev) {
  switch (ev.k) {
    case 'col': {
      // Clear resource unconditionally — mutate in-place so cached cell refs stay valid
      if (gameMap[ev.r]?.[ev.q]) {
        gameMap[ev.r][ev.q].resource = 0;
        gameMap[ev.r][ev.q].amount   = 0;
      }
      collectedCells.add(`${ev.q}_${ev.r}`);
      const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      addLog(`<span class="log-col">${escHtml(who)} +${ev.amt}× ${RES_NAMES[ev.res]}</span>`);
      if (ev.pid === myId) {
        // Optimistically update inv so the sidebar reflects the pickup immediately,
        // before the next 's' broadcast confirms the server-side inv state.
        const idx = ev.res - 1;
        if (idx >= 0 && idx < 5) players[myId].inv[idx] = (players[myId].inv[idx] ?? 0) + ev.amt;
        showToast(`+${ev.amt} ${RES_NAMES[ev.res]}`);
        updateSidebar();
      }
      break;
    }
    case 'rsp':
      if (gameMap[ev.r] && gameMap[ev.r][ev.q])
        gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], resource: ev.res, amount: ev.amt };
      collectedCells.delete(`${ev.q}_${ev.r}`);
      break;
    case 'mv': {
      const pm = players[ev.pid];
      if (ev.mp !== undefined) {
        pm.mp = ev.mp;
        if (ev.pid === myId) updateSidebar();
      }
      pm.q = ev.q; pm.r = ev.r;
      // Surveyed cells are relative to a fixed vantage point — clear on any move.
      if (ev.pid === myId) surveyedCells.clear();
      // Stamp footprint immediately — vis-disk only goes to the moving player,
      // so other players would never see footprints without this client-side update.
      if (gameMap[ev.r]?.[ev.q]) {
        const c = gameMap[ev.r][ev.q];
        gameMap[ev.r][ev.q] = { ...c, footprints: (c.footprints || 0) | (1 << ev.pid) };
      }
      if (ev.radd && ev.radd > 0) {
        pm.rad = ev.rad ?? pm.rad;
        if (ev.pid === myId) {
          uiRad.val = pm.rad;
          const radTxt = ev.radd > 0 ? `\u2622 +${ev.radd} Radiation (R:${pm.rad})` : '';
          if (radTxt) showToast(radTxt);
          addLog(`<span class="log-check-fail">\u2622 Entered rad zone +${ev.radd}R → R:${pm.rad}</span>`);
        }
      }
      // Exploration bonus: log first-visit score
      if (ev.exploD && ev.exploD > 0 && ev.pid === myId) {
        addLog(`<span class="log-mv">\u2605 New hex \u2014 +${ev.exploD} exploration pt</span>`);
      }
      // Narrate position for accessibility / AI agents
      if (ev.pid === myId) {
        const cell = gameMap[ev.r]?.[ev.q];
        const tName = TERRAIN[cell?.terrain ?? 0]?.name ?? 'Unknown';
        const shelterNote = cell?.shelter ? ' Shelter present.' : '';
        const exploNote   = ev.exploD > 0 ? ` +${ev.exploD}pt.` : '';
        narrateState(`Moved to ${tName} at q:${ev.q},r:${ev.r}. MP:${ev.mp}.${shelterNote}${exploNote}`);
      }
      break;
    }
    case 'join':
      players[ev.pid].on = true;
      // Snap renderPos so the new player doesn't lerp from (0,0) to their hex (Bug-4)
      if (ev.q !== undefined) { renderPos[ev.pid].q = ev.q; renderPos[ev.pid].r = ev.r; }
      else { renderPos[ev.pid].q = players[ev.pid].q; renderPos[ev.pid].r = players[ev.pid].r; }
      addLog(`<span class="log-join">&#x25B6; Survivor ${ev.pid} appeared</span>`);
      break;
    case 'regen':
      showToast('☠ Wasteland reborn. Survivors scattered.');
      addLog('<span class="log-mv">☠ The world has been remade. Find your bearings, survivor.</span>');
      break;
    case 'downed': {
      // Server has reset our slot — show death message then redirect to char selection
      myId = -1;
      pendingLobbyRedirect = true;
      console.log('[downed] received — starting 3.5s timer');
      window._onEncEnd?.();  // close encounter overlay if open when player is downed
      addLog('<span class="log-check-fail">☠ DOWNED — the wasteland claims you. Find shelter next time.</span>');
      showToast('☠ YOU HAVE BEEN DOWNED — re-selecting survivor...');
      setTimeout(() => {
        pendingLobbyRedirect = false;
        console.log('[downed] timer fired — lobbyAvail=%o', lobbyAvail.val);
        if (lobbyAvail.val.length > 0) {
          showCharSelect();
        } else {
          console.log('[downed] avail empty — forcing reconnect');
          socket.close();  // onclose → reconnect → sendLobbyMsg → lobby handler calls showCharSelect()
        }
      }, 3500);
      break;
    }

    case 'left':
      players[ev.pid].on = false;
      players[ev.pid].rest = false;
      addLog(`<span class="log-mv">&#x25C4; Survivor ${ev.pid} gone dark</span>`);
      checkAutoRest();
      break;
    case 'nm':
      players[ev.pid].nm = ev.nm;
      if (ev.pid !== myId) updateSidebar();
      break;
    case 'chk': {
      const who    = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      const skNm   = SK_NAMES[ev.sk] ?? `SK${ev.sk}`;
      const modTxt = ev.mod !== 0 ? (ev.mod > 0 ? `+${ev.mod}` : `${ev.mod}`) : '';
      const icon   = ev.suc ? '\u25CF' : '\u25CB';
      const cls    = ev.suc ? 'log-check-ok' : 'log-check-fail';
      addLog(`<span class="${cls}">${icon} ${escHtml(who)} ${skNm}: ${ev.r1}+${ev.r2}+${ev.sv}${modTxt}=${ev.tot} vs DN${ev.dn}</span>`);
      if (ev.pid === myId) {
        // Show the roll result only — action outcome (incl. PARTIAL) arrives in the 'act' event
        const modTxt2 = ev.mod !== 0 ? (ev.mod > 0 ? `+${ev.mod}` : `${ev.mod}`) : '';
        showToast(`${skNm}: ${ev.r1}+${ev.r2}+${ev.sv}${modTxt2}=${ev.tot} vs DN${ev.dn}`);
      }
      break;
    }
    case 'dawn': {
      // Update local player state from dawn event (all players, not just self)
      if (ev.pid >= 0 && ev.pid < MAX_PLAYERS) {
        players[ev.pid].food  = ev.f;
        players[ev.pid].water = ev.w;
        players[ev.pid].ll    = ev.ll;
        players[ev.pid].mp    = ev.mp;
        if (ev.pid === myId) { maxMP = ev.mp; uiMaxMP.val = ev.mp; displayMP = ev.mp; nightFade = NIGHT_FADE_INIT; }  // lock MP; snap displayMP; start night fade
        players[ev.pid].rest = false;
        players[ev.pid].au   = 0;  // reset action slot at dawn
        if (ev.pid === myId) {
          _tickNarrativeEffects();
          if (uiResting.val && ev.expd < 0) showShelterWarning();
          uiResting.val = false;  // clear resting at dawn
          restSent = false;
        }
          if (ev.fth !== undefined) players[ev.pid].fth = ev.fth;
        if (ev.wth !== undefined) players[ev.pid].wth = ev.wth;
        if (ev.rad !== undefined) players[ev.pid].rad = ev.rad;
      }
      const who    = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      const llTxt  = ev.dll < 0 ? ` \u25BC LL${ev.dll}` : ev.dll > 0 ? ` \u25B2 LL+${ev.dll}` : '';
      const cls    = ev.dll < 0 ? 'log-check-fail' : 'log-mv';
      if (ev.day !== undefined) gameState.dc = ev.day;
      addLog(`<span class="${cls}">\u2600 Day ${ev.day}: ${escHtml(who)} F:${ev.f} W:${ev.w}${llTxt}</span>`);
      if (ev.pid === myId) {
        const llTxt = ev.dll < 0 ? ` LL${ev.dll}` : ev.dll > 0 ? ` LL+${ev.dll}` : '';
        showToast(`\u2600 Day ${ev.day}${llTxt} F:${ev.f} W:${ev.w}`);
        // Re-render action panel if open — was showing stale resting message
        const _ap = document.getElementById('action-panel');
        if (_ap?.classList.contains('open')) setTimeout(openActionPanel, 0);
      }
      updateSidebar();
      break;
    }

    case 'act': {
      // Action result — update player state
      const p = ev.pid >= 0 && ev.pid < MAX_PLAYERS ? players[ev.pid] : null;
      if (p) {
        p.mp   = ev.mp;
        p.ll   = ev.ll;
        if (ev.fd)   p.inv[1] = Math.min(99, (p.inv[1] ?? 0) + ev.fd);
        if (ev.wd)   p.inv[0] = Math.min(99, (p.inv[0] ?? 0) + ev.wd);

        if (ev.rad !== undefined) p.rad = ev.rad;
      }
      const who   = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      const actNm = ACT_NAMES[ev.a] ?? `Act${ev.a}`;
      const outTx = ev.out === AO_SUCCESS ? '\u25CF' : ev.out === AO_PARTIAL ? '\u25D1' :
                    ev.out === AO_FAIL    ? '\u25CB' : '\u2297';
      const outCl = ev.out === AO_SUCCESS ? 'log-check-ok' : ev.out === AO_PARTIAL ? 'log-mv' :
                    ev.out === AO_FAIL    ? 'log-check-fail' : 'log-mv';
      let detail = '';
      if (ev.dn)   detail += ` DN${ev.dn}=${ev.tot}`;
      if (ev.fd)   detail += ` +${ev.fd}Food`;
      if (ev.wd)   detail += ` +${ev.wd}Water`;
      if (ev.lld)  detail += ` ${ev.lld > 0 ? '+' : ''}${ev.lld}LL`;
      if (ev.radd) detail += ` ${ev.radd > 0 ? '+' : ''}${ev.radd}R`;
      if (ev.scoreD) detail += ` \u271F${ev.scoreD}pts`;
      addLog(`<span class="${outCl}">${outTx} ${escHtml(who)} ${actNm}${detail}</span>`);
      // Special toasts for REST and SHELTER
      if (ev.a === ACT_REST && ev.out === AO_SUCCESS) {
        players[ev.pid].rest = true;
        if (ev.pid === myId) uiResting.val = true;
          checkAutoRest();
        const msg = ev.pid === myId ? 'Resting — waiting for dawn' : `${escHtml(who)} is resting`;
        showToast(`\ud83d\ude34 ${msg}`);
        addLog(`<span class="log-mv">\ud83d\ude34 ${escHtml(who)} is now waiting for dawn</span>`);
      } else if (ev.a === ACT_SHELTER && ev.out === AO_SUCCESS) {
        const shelterName = ev.cnd === 2 ? 'an improved shelter 🏠' : 'a shelter ⛺';
        const msg = ev.pid === myId ? `Built ${shelterName}!` : `${escHtml(who)} built ${shelterName}`;
        showToast(`🔨 ${msg}`);
        // Update local gameMap immediately so the shelter icon renders without waiting for next move
        const sp = players[ev.pid];
        if (sp && gameMap[sp.r]?.[sp.q] != null) {
          gameMap[sp.r][sp.q].shelter = ev.cnd;
        }
      } else if (ev.pid === myId) {
        const toastMsg =
          ev.out === AO_BLOCKED  ? `\u2297 ${actNm}: not available` :
          ev.out === AO_SUCCESS  ? `\u25CF ${actNm}: success` :
          ev.out === AO_PARTIAL  ? `\u25D1 ${actNm}: partial success` :
                                   `\u25CB ${actNm}: failed`;
        showToast(toastMsg);
      }
      // Apply scrap delta for ALL actions (SCAV gives +1, SHELTER spends -1/-2)
      if (ev.sd !== undefined && ev.sd !== 0) {
        players[ev.pid].inv[4] = Math.max(0, (players[ev.pid].inv[4] ?? 0) + ev.sd);
      }
      // Narrate action result for accessibility / AI agents
      if (ev.pid === myId) {
        const actNames = ['Forage','Water','','Scavenge','Shelter','Treat','Survey','Rest'];
        const outNames = ['','Success','Partial','Failed'];
        const pts = ev.scoreD ? ` +${ev.scoreD}pts.` : '';
        narrateState(`${actNames[ev.a] ?? 'Action'} — ${outNames[ev.out] ?? ev.out}. MP:${ev.mp}.${pts}`);
      }
      // Track action-slot state client-side (mirrors server p.actUsed)
      if (ev.out !== AO_BLOCKED) {
        const meIsScout = (players[ev.pid]?.arch ?? -1) === 4;
        if (!(ev.a === ACT_SURVEY && meIsScout)) {
          players[ev.pid].au = 1;  // slot consumed
        }
      }
      if (ev.a === ACT_REST && ev.pid === myId) restSent = false;
      updateSidebar();
      break;
    }

    case 'surv': {
      // SURVEY result: update gameMap and mark cells as surveyed for rendering.
      try {
        if (ev.cells) {
          applyVisDisk(ev.cells);
          // Extract q,r coords and add to the surveyed set so the render loop
          // shows these cells as terrain instead of fog.
          for (let i = 0; i < ev.cells.length; i += 10) {
            const sq = parseInt(ev.cells.substr(i,     2), 16);
            const sr = parseInt(ev.cells.substr(i + 2, 2), 16);
            surveyedCells.add(`${sq}_${sr}`);
          }
        }
        if (ev.pid === myId) showToast('\u25CE Survey: outer ring revealed');
      } catch(e) { console.error('surv event error', e); }
      break;
    }

    case 'dusk': {
      // End-of-day radiation Endure check (R ≥ 7) — §6.2
      const pd = ev.pid >= 0 && ev.pid < MAX_PLAYERS ? players[ev.pid] : null;
      if (pd) {
        pd.ll  = ev.ll;
        pd.rad = ev.rad ?? pd.rad;
      }
      const who  = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      const pass = ev.out === AO_SUCCESS;
      const cls  = pass ? 'log-check-ok' : 'log-check-fail';
      const totTxt = ev.tot === 0 ? 'AUTO-FAIL' : `DN${ev.dn}=${ev.tot}`;
      const lldTxt = ev.lld < 0 ? ` LL${ev.lld} +Wound` : '';
      addLog(`<span class="${cls}">\u2622 DUSK ${escHtml(who)}: Endure ${totTxt} → ${pass ? 'PASS' : 'FAIL'}${lldTxt}</span>`);
      if (ev.pid === myId) {
        if (pass) showToast('\u2622 Dusk Endure: passed');
        else      showToast(`\u2622 Dusk Endure: FAILED — LL${ev.lld}, +Major Wound`);
        updateSidebar();
      }
      break;
    }

    case 'trd_off': {
      // Trade offer sent: log for everyone; show overlay only for the target
      const fromName = (ev.from >= 0 && ev.from < MAX_PLAYERS && players[ev.from]?.nm) || 'P' + ev.from;
      const toName   = (ev.to   >= 0 && ev.to   < MAX_PLAYERS && players[ev.to]?.nm)   || 'P' + ev.to;
      const giveTxt  = ev.give.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join(' ') || '\u2014';
      const wantTxt  = ev.want.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join(' ') || '\u2014';
      addLog(`<span class="log-col">\u21C4 ${escHtml(fromName)} offers ${escHtml(toName)}: ${giveTxt} for ${wantTxt}</span>`);
      if (ev.to === myId) {
        window._openTradeOffer?.(ev.from, ev.give, ev.want);
      }
      break;
    }

    case 'trd_res': {
      // Trade result: 1=accepted 2=declined 3=expired
      const fromName = (ev.from >= 0 && ev.from < MAX_PLAYERS && players[ev.from]?.nm) || 'P' + ev.from;
      const toName   = (ev.to   >= 0 && ev.to   < MAX_PLAYERS && players[ev.to]?.nm)   || 'P' + ev.to;
      const TRADE_LABELS = ['', 'ACCEPTED', 'DECLINED', 'EXPIRED'];
      const label = TRADE_LABELS[ev.res] ?? 'UNKNOWN';
      const cls   = ev.res === 1 ? 'log-col' : 'log-check-fail';
      addLog(`<span class="${cls}">\u21C4 Trade ${label}: ${escHtml(fromName)} \u2194 ${escHtml(toName)}</span>`);
      if (ev.from === myId || ev.to === myId) {
        showToast('\u21C4 Trade ' + label);
        window._closeTradeOverlay?.();
        updateSidebar();
      }
      break;
    }

    case 'item_result': {
      // Server ack for use_item / equip_item / unequip_item
      if (ev.ok) {
        if (ev.msg) showToast(ev.msg);
        // Dispatch client-side narrative effects
        if (ev.act === 'use' && ev.efxp) handleNarrativeEffect(ev.efxp);
      } else {
        showToast('\u2297 ' + (ev.msg || 'Action failed'));
      }
      // Always apply server ground-truth state (server sends current state regardless of ok/fail)
      if (ev.pid !== undefined && ev.pid >= 0 && ev.pid < MAX_PLAYERS) {
        const p = players[ev.pid];
        if (ev.it) p.it = ev.it;
        if (ev.iq) p.iq = ev.iq;
        if (ev.eq) p.eq = ev.eq;
      }
      // Always refresh char-sheet inventory/equipment — ghost-tap on mobile can close
      // char-overlay between item-menu dismiss and server ack, causing the open-check to fail
      renderInventory?.();
      renderEquipment?.();
      break;
    }

    case 'enc_start': {
      // POI consumed — clear it from local map so eye disappears immediately
      if (gameMap[ev.r]?.[ev.q])
        gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], poi: 0 };
      const who = players[ev.pid]?.nm || `P${ev.pid}`;
      addLog(`<span class="log-col">\uD83D\uDC41 ${escHtml(who)} enters encounter (${ev.q},${ev.r})</span>`);
      updateSidebar();
      break;
    }

    case 'enc_res': {
      const who = players[ev.pid]?.nm || `P${ev.pid}`;
      const outClass = ev.out ? 'log-col' : 'log-check-fail';
      addLog(`<span class="${outClass}">\uD83D\uDC41 ${escHtml(who)}: ${ev.out ? 'through.' : 'setback.'}</span>`);
      if (ev.pid === myId) {
        const p = players[myId];
        if (p) {
          if (ev.penLL)  p.ll  = Math.max(0, (p.ll  ?? 0) + ev.penLL);
          if (ev.penRad) p.rad = Math.max(0, (p.rad ?? 0) + ev.penRad);
          if (ev.st) p.sb = ev.st;
        }
        updateSidebar();
        window._onEncResult?.(ev);
      }
      // Apply auto-drain to co-located allies on failure
      if (!ev.out && Array.isArray(ev.drains)) {
        const ldrName = players[ev.pid]?.nm || `P${ev.pid}`;
        ev.drains.forEach((d, apid) => {
          if (d <= 0 || !players[apid]) return;
          let toDeduct = d;
          for (let ri = 0; ri < 5 && toDeduct > 0; ri++) {
            const cur = players[apid].inv?.[ri] ?? 0;
            if (cur > 0) { const take = Math.min(cur, toDeduct); players[apid].inv[ri] = cur - take; toDeduct -= take; }
          }
          if (apid === myId) {
            showToast(`\uD83D\uDC41 You shared supplies with ${escHtml(ldrName)}.`);
            updateSidebar();
          }
        });
      }
      break;
    }

    case 'enc_bank': {
      const who = players[ev.pid]?.nm || `P${ev.pid}`;
      const lootSum = (ev.loot ?? []).reduce((a, b) => a + b, 0);
      addLog(`<span class="log-col">\u25a0 ${escHtml(who)} secured loot (${lootSum} items, +${ev.scoreD} pts)</span>`);
      if (ev.pid === myId) {
        showToast(`\u25a0 Loot secured! +${ev.scoreD} pts`);
        window._onEncBank?.(ev);
        updateSidebar();
      }
      break;
    }

    case 'enc_end': {
      const who = players[ev.pid]?.nm || `P${ev.pid}`;
      const ENC_REASON_LABELS = { hazard: 'hazard', abort: 'aborted', dawn: 'dawn', downed: 'downed', disconnect: 'disconnected' };
      const reasonTxt = ENC_REASON_LABELS[ev.reason] ?? ev.reason;
      addLog(`<span class="log-check-fail">\u25a0 ${escHtml(who)} encounter ended (${reasonTxt})</span>`);
      if (ev.pid === myId) {
        showToast(`\u25a0 Encounter ended: ${reasonTxt}`);
        window._onEncEnd?.(ev);
        updateSidebar();
      }
      break;
    }

    case 'weather': {
      weatherPhase = ev.phase ?? 0;
      updateWeatherHUD();
      addLog(`<span class="log-mv">Weather: ${WEATHER_PHASE_NAMES?.[ev.phase] ?? ev.phase}</span>`);
      break;
    }

  }
}

// ── Map decode ──────────────────────────────────────────────────
// 3 bytes per cell (6 hex chars):
//   TT = terrain byte (0x00-0x0B) or 0xFF (fog)
//   DD = bits 0-5: footprint bitmask, bit 6: has shelter (any), bit 7: has POI
//   VV = high nibble: resource type (0-5), low nibble: terrain variant (0-15)
function decodeCell(terrainByte, dataByte, variantByte = 0) {
  if (terrainByte === 0xFF) return null;
  const cell = {
    terrain:    terrainByte,
    footprints: dataByte & 0x3F,           // bits 0-5: which players visited (bitmask)
    shelter:    (dataByte >> 6) & 1,       // bit 6: 0=none, 1=has shelter
    poi:        (dataByte >> 7) & 1,       // bit 7: 0=none, 1=has POI encounter
    resource:   (variantByte >> 4) & 0xF, // high nibble: resource type (0=none, 1-5)
    variant:    variantByte & 0xF,        // low nibble: terrain image variant (0-15)
  };
  return cell;
}

function parseMapFog(hexStr) {
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      const idx = (r * MAP_COLS + c) * 6;
      const tt  = parseInt(hexStr.substr(idx,     2), 16);
      const dd  = parseInt(hexStr.substr(idx + 2, 2), 16);
      const vv  = parseInt(hexStr.substr(idx + 4, 2), 16);
      gameMap[r][c] = decodeCell(tt, dd, vv);
    }
  }
}

// ── Apply vis-disk update ────────────────────────────────────────
// Format: "QQRRTTDDVV..." — 10 hex chars per cell (5 bytes)
//   QQ=col, RR=row, TT=terrain, DD=data, VV=variant
function applyVisDisk(cells) {
  for (let i = 0; i < cells.length; i += 10) {
    const q  = parseInt(cells.substr(i,     2), 16);
    const r  = parseInt(cells.substr(i + 2, 2), 16);
    const tt = parseInt(cells.substr(i + 4, 2), 16);
    const dd = parseInt(cells.substr(i + 6, 2), 16);
    const vv = parseInt(cells.substr(i + 8, 2), 16);
    if (r < MAP_ROWS && q < MAP_COLS) {
      const cell = decodeCell(tt, dd, vv);
      // Preserve locally-cleared resource — col event may arrive before vis disk
      if (collectedCells.has(`${q}_${r}`) && cell && cell.resource > 0) {
        cell.resource = 0;
        cell.amount   = 0;
      }
      gameMap[r][q] = cell;
    }
  }
}

// ── Hex math (flat-top) ─────────────────────────────────────────
function hexToPixel(q, r, size) {
  return { x: size * 1.5 * q, y: size * (SQRT3 / 2 * q + SQRT3 * r) };
}

function hexDist(q1, r1, q2, r2) {
  const dq = q2 - q1, dr = r2 - r1;
  return (Math.abs(dq) + Math.abs(dq + dr) + Math.abs(dr)) / 2;
}
function hexDistWrap(q1, r1, q2, r2) {
  let min = Infinity;
  for (let dq = -1; dq <= 1; dq++)
    for (let dr = -1; dr <= 1; dr++)
      min = Math.min(min, hexDist(q1, r1, q2 + dq * MAP_COLS, r2 + dr * MAP_ROWS));
  return min;
}

function drawHexPath(ctx, cx, cy, size) {
  ctx.beginPath();
  for (let i = 0; i < 6; i++) {
    const a = Math.PI / 3 * i;
    i === 0 ? ctx.moveTo(cx + size * Math.cos(a), cy + size * Math.sin(a))
            : ctx.lineTo(cx + size * Math.cos(a), cy + size * Math.sin(a));
  }
  ctx.closePath();
}

// ── Canvas setup ─────────────────────────────────────────────────
const canvas = document.getElementById('hexCanvas');
const ctx    = canvas.getContext('2d');
let HEX_SZ    = 56;
// CSS-pixel dimensions used throughout the render loop.
// canvas.width/height hold physical pixels (= cssWidth/Height × devicePixelRatio).
let cssWidth  = 0;
let cssHeight = 0;

/**
 * Recalculate hex size and canvas resolution based on viewport size.
 * Scales the canvas buffer by devicePixelRatio for crisp rendering on
 * HiDPI / Retina screens while keeping all draw coordinates in CSS pixels.
 */
function resize() {
  const wrap = document.getElementById('canvas-wrap');
  const dpr  = window.devicePixelRatio || 1;
  cssWidth   = wrap.clientWidth;
  cssHeight  = wrap.clientHeight;
  canvas.width  = cssWidth  * dpr;
  canvas.height = cssHeight * dpr;
  canvas.style.width  = cssWidth  + 'px';
  canvas.style.height = cssHeight + 'px';
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);  // reset any prior transform then scale
  HEX_SZ = Math.max(HEX_SZ_MIN, Math.min(HEX_SZ_MAX, Math.floor(cssWidth / HEX_SZ_VIEWPORT_DIVISOR)));
}
window.addEventListener('resize', resize);
resize();

/**
 * Draw terrain icon centered in a hex cell.
 * @param {CanvasRenderingContext2D} ctx - Canvas context
 * @param {number} cx - Center X coordinate
 * @param {number} cy - Center Y coordinate
 * @param {number} hexSz - Hex size in pixels
 * @param {number} terrainIdx - Terrain type index
 * @param {boolean} hasResource - Whether cell has a resource (affects positioning/opacity)
 */
function drawTerrainIcon(ctx, cx, cy, hexSz, terrainIdx, hasResource) {
  const t = TERRAIN[terrainIdx];
  if (!t) return;
  const sz   = Math.max(10, hexSz * ICON_SIZE_SCALE);
  const offY = hasResource ? -hexSz * 0.42 : 0;

  ctx.save();
  ctx.globalAlpha  = hasResource ? 0.45 : 0.70;
  ctx.font         = `${sz}px serif`;
  ctx.textAlign    = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle    = '#FFF';
  ctx.fillText(t.icon, cx, cy + offY);
  ctx.restore();
}

/**
 * Draw character/player icon on the hex grid.
 * Includes head, torso, name label, and ground shadow.
 * @param {CanvasRenderingContext2D} ctx - Canvas context
 * @param {number} cx - Center X coordinate
 * @param {number} cy - Center Y coordinate
 * @param {number} hexSz - Hex size in pixels
 * @param {string} color - Character color (CSS color string)
 * @param {string} label - Character label (1-2 chars)
 * @param {boolean} isMe - Whether this is the current player (affects glow)
 * @param {string} nm - Character name for label tooltip
 */
function drawCharIcon(ctx, cx, cy, hexSz, color, label, isMe, nm, arch, sc) {
  const scale     = hexSz * ICON_SIZE_SCALE;
  const r         = Math.max(10, hexSz * 0.28);   // portrait circle radius
  const portraitCY = cy - scale * 0.15;            // circle center, slightly above hex centre

  // Ground shadow — fuzzy ellipse beneath the circle
  ctx.save();
  ctx.filter = 'blur(3px)';
  ctx.beginPath();
  ctx.ellipse(cx, cy + r * 0.85, r * 0.65, r * 0.18, 0, 0, Math.PI * 2);
  ctx.fillStyle = 'rgba(0,0,0,0.45)';
  ctx.fill();
  ctx.restore();

  // Portrait circle (clipped image, or fallback solid colour + number)
  const img = pawnImgs[arch];
  if (img?.loaded) {
    ctx.save();
    ctx.beginPath();
    ctx.arc(cx, portraitCY, r, 0, Math.PI * 2);
    ctx.clip();
    ctx.drawImage(img, cx - r, portraitCY - r, r * 2, r * 2);
    ctx.restore();
  } else {
    ctx.beginPath();
    ctx.arc(cx, portraitCY, r, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
    ctx.save();
    ctx.fillStyle    = '#000';
    ctx.font         = `bold ${Math.max(7, Math.round(r * 0.9))}px monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, cx, portraitCY + 0.5);
    ctx.restore();
  }

  // Coloured outline — archetype ring (all players)
  const outlineW = Math.max(1.5, r * 0.09);
  ctx.beginPath();
  ctx.arc(cx, portraitCY, r, 0, Math.PI * 2);
  ctx.strokeStyle = color;
  ctx.lineWidth   = outlineW;
  ctx.stroke();

  // Current player: brighter/thicker second ring just outside
  if (isMe) {
    ctx.beginPath();
    ctx.arc(cx, portraitCY, r + outlineW + 1, 0, Math.PI * 2);
    ctx.strokeStyle = color;
    ctx.lineWidth   = Math.max(2, outlineW * 0.7);
    ctx.globalAlpha = 0.7;
    ctx.stroke();
    ctx.globalAlpha = 1;
  }

  const nameSz = Math.max(8, Math.round(hexSz * 0.21));
  // Call sign above circle
  if (nm) {
    const tag      = nm.substring(0, 8).toUpperCase();
    const topY     = portraitCY - r - 4;
    ctx.save();
    ctx.font         = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'bottom';
    ctx.fillStyle    = '#000';
    ctx.letterSpacing = '1px';
    ctx.fillText(tag, cx, topY);
    ctx.restore();
  }

  // Score below circle
  if (sc !== undefined && sc !== null) {
    const scoreStr = String(sc);
    const botY     = portraitCY + r + nameSz + 6;
    ctx.save();
    ctx.font         = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'bottom';
    ctx.fillStyle    = '#000';
    ctx.letterSpacing = '1px';
    ctx.fillText(scoreStr, cx, botY);
    ctx.restore();
  }
}

// ── Time-of-day clock ────────────────────────────────────────────
const TIME_PHASES = [
  { name:'FIRST LIGHT', icon:'☀',      r:255, g:180, b:100, a:0.03 },
  { name:'HIGH WATCH',  icon:'☀',      r:255, g:220, b:150, a:0.01 },
  { name:'NOON BURN',   icon:'☀',      r:255, g:240, b:200, a:0.00 },
  { name:'LOW SUN',     icon:'🌇',r:255, g:160, b: 60, a:0.06 },
  { name:'DUST HOUR',   icon:'🌆',r:210, g: 90, b: 20, a:0.14 },
  { name:'DARK WATCH',  icon:'🌑',r: 15, g:  8, b: 40, a:0.72 },
];
function getTimePhase(mpVal) {
  const _mp = (mpVal !== undefined) ? mpVal : uiMP.val;
  if (myId < 0 || maxMP <= 0) return TIME_PHASES[0];
  const f = Math.max(0, Math.min(1, 1 - _mp / maxMP));
  const scaled = f * (TIME_PHASES.length - 1);
  const i = Math.floor(scaled);
  const t = scaled - i;
  const p0 = TIME_PHASES[Math.min(i, TIME_PHASES.length - 1)];
  const p1 = TIME_PHASES[Math.min(i + 1, TIME_PHASES.length - 1)];
  return {
    icon: p0.icon, name: p0.name,
    r: Math.round(p0.r + t * (p1.r - p0.r)),
    g: Math.round(p0.g + t * (p1.g - p0.g)),
    b: Math.round(p0.b + t * (p1.b - p0.b)),
    a: p0.a + t * (p1.a - p0.a)
  };
}
var nightFade = 0;  // extra night overlay that fades out after dawn
var displayMP = 6;    // smoothly lerped toward uiMP.val each frame

// ── Ash particle system (Pass 3) ────────────────────────────────
const ashParticles = (typeof AshParticleSystem !== 'undefined') ? new AshParticleSystem({
  maxAlpha:         0.52,   // peak opacity at spawn point
  driftSpeed:       0.04,   // px/ms base speed (randomized ±)
  wobbleAmp:        8,      // px of sine-wave wobble layered on drift
  wobbleFreq:       0.0007, // wobble sine frequency (rad/ms)
  maxDistanceHexes: 3,      // hex-distance at which particle is garbage collected
  countPerTerrain:  2,      // target active particles per ash/mountain hex
  radiusMin:        1.5,    // min gradient radius as multiple of HEX_SZ
  radiusMax:        3.0,    // max gradient radius as multiple of HEX_SZ
}) : null;

// ── Render ──────────────────────────────────────────────────────
function render() {
  ctx.clearRect(0, 0, cssWidth, cssHeight);
  ctx.fillStyle = '#050301';
  ctx.fillRect(0, 0, cssWidth, cssHeight);

  // Lerp all player render positions
  for (let i = 0; i < MAX_PLAYERS; i++) {
    const p = players[i];
    if (!p.on) { renderPos[i].q = p.q; renderPos[i].r = p.r; continue; }
    let tq = p.q, tr = p.r;
    while (tq - renderPos[i].q >  MAP_COLS / 2) tq -= MAP_COLS;
    while (renderPos[i].q - tq >  MAP_COLS / 2) tq += MAP_COLS;
    while (tr - renderPos[i].r >  MAP_ROWS / 2) tr -= MAP_ROWS;
    while (renderPos[i].r - tr >  MAP_ROWS / 2) tr += MAP_ROWS;
    renderPos[i].q += (tq - renderPos[i].q) * LERP;
    renderPos[i].r += (tr - renderPos[i].r) * LERP;
  }

  // Camera: centre on my smooth position
  const meRp = (myId >= 0) ? renderPos[myId] : { q: 0, r: 0 };
  const cp   = hexToPixel(meRp.q, meRp.r, HEX_SZ);
  const ox   = cssWidth  / 2 - cp.x;
  const oy   = cssHeight / 2 - cp.y;

  // Fog uses the ACTUAL integer position (accurate fog boundary)
  const meAct = (myId >= 0) ? players[myId] : { q: 0, r: 0 };

  const viewQ   = Math.ceil(cssWidth  / (HEX_SZ * 1.5)) + 2;
  const viewR   = Math.ceil(cssHeight / (HEX_SZ * SQRT3)) + 2;
  const centreQ = Math.round(meRp.q);
  const centreR = Math.round(meRp.r);

  // ── Pass 1: Hex fills + terrain icons + resources ─────────────
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq   = centreQ + dq;
      const vr   = centreR + dr;
      const mapQ = ((vq % MAP_COLS) + MAP_COLS) % MAP_COLS;
      const mapR = ((vr % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;

      const px = hexToPixel(vq, vr, HEX_SZ);
      const cx = px.x + ox;
      const cy = px.y + oy;

      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;

      const dist     = hexDistWrap(meAct.q, meAct.r, mapQ, mapR);
      const cell     = gameMap[mapR][mapQ];
      const visible  = dist <= myVisionR && cell !== null;
      // Surveyed: beyond normal vision but revealed by SURVEY action this turn.
      const surveyed = !visible && cell !== null && surveyedCells.has(`${mapQ}_${mapR}`);

      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      if (visible) {
        ctx.globalAlpha = 1;
        const t = TERRAIN[cell.terrain];
        ctx.fillStyle   = t?.fill || '#2A2010';
      } else if (surveyed) {
        ctx.globalAlpha = 0.7;   // scouted — terrain visible but dimmed
        const t = TERRAIN[cell.terrain];
        ctx.fillStyle   = t?.fill || '#2A2010';
      } else if (dist === myVisionR + 1) {
        ctx.globalAlpha = FOG_INNER_ALPHA;  // inner fog ring — more visible
        ctx.fillStyle   = '#141008';
      } else if (dist === myVisionR + 2) {
        ctx.globalAlpha = 0.92;  // outer fog ring — subtly transparent
        ctx.fillStyle   = '#141008';
      } else {
        ctx.globalAlpha = 1;     // deep fog — fully opaque
        ctx.fillStyle   = '#080402';
      }
      ctx.fill();
      ctx.globalAlpha = 1;

      if (!visible && !surveyed) continue;

      // Surveyed cells draw terrain at reduced opacity; fully visible cells at full opacity.
      ctx.globalAlpha = surveyed ? 0.7 : 1;

      // Terrain hex image (if loaded) — alpha channel handles hex shape, no clipping
      const _tv  = terrainImgVariants[cell.terrain];
      const tImg = (_tv && _tv.length > 0) ? (_tv[cell.variant % _tv.length] || _tv[0]) : null;
      if (tImg && tImg.loaded) {
        const imgSz = HEX_SZ * 2;
        ctx.drawImage(tImg, cx - imgSz / 2, cy - imgSz / 2, imgSz, imgSz);
      }

      // Terrain icon (no resource indicator) — only when no image
      if (!tImg || !tImg.loaded) {
        if (cell.terrain !== 11) drawTerrainIcon(ctx, cx, cy, HEX_SZ, cell.terrain, false);
      }

      // ── River ripples (terrain 11) ────────────────────────────────
      // Animated concentric ellipses using the RAF timestamp for smooth motion.
      if (cell.terrain === 11 && (!tImg || !tImg.loaded)) {
        const rt = performance.now() / 1000;
        ctx.save();
        ctx.lineWidth = 1;
        for (let w = 0; w < RIVER_RIPPLE_COUNT; w++) {
          const phase = (rt * RIVER_RIPPLE_SPEED + w * RIVER_RIPPLE_SPACING) % 1;
          const scale = 0.25 + phase * 0.55;
          const alpha = 0.18 * (1 - phase);
          ctx.strokeStyle = `rgba(30,70,90,${alpha.toFixed(3)})`;
          ctx.beginPath();
          ctx.ellipse(cx, cy, HEX_SZ * scale * RIVER_RIPPLE_W_SCALE, HEX_SZ * scale * RIVER_RIPPLE_H_SCALE, 0, 0, Math.PI * 2);
          ctx.stroke();
        }
        ctx.restore();
      }

      // Reset alpha — footprints/shelter always render at full opacity.
      ctx.globalAlpha = 1;

      // Footprints: dark brown 👣 icon per player that has visited this hex.
      if (cell.footprints > 0) {
        const footprintSize = Math.max(6, Math.round(HEX_SZ * 0.22));
        let footprintCount = 0;
        for (let i = 0; i < 6; i++) if (cell.footprints & (1 << i)) footprintCount++;
        let footprintIdx = 0;
        for (let fpid = 0; fpid < 6; fpid++) {
          if ((cell.footprints & (1 << fpid)) === 0) continue;
          const angle  = (footprintIdx * Math.PI * 2) / Math.max(1, footprintCount);
          const radius = HEX_SZ * FOOTPRINT_RING_RADIUS;
          const fx = cx + Math.cos(angle) * radius;
          const fy = cy + Math.sin(angle) * radius;
          ctx.save();
          ctx.filter       = 'sepia(1) saturate(0.5) brightness(0.35)';
          ctx.globalAlpha  = 0.75;
          ctx.font         = `${footprintSize}px monospace`;
          ctx.textAlign    = 'center';
          ctx.textBaseline = 'middle';
          ctx.fillText('👣', fx, fy);
          ctx.restore();
          footprintIdx++;
        }
      }

      // Food resource — forage animal PNG centred on hex (resource type 2 = food)
      if (cell.resource === 2 && forrageAnimalImgs.length > 0) {
        const v    = (mapQ * 31 + mapR * 17) % forrageAnimalImgs.length;
        const fImg = forrageAnimalImgs[v];
        if (fImg && fImg.loaded) {
          const sz = HEX_SZ * 0.45;
          ctx.drawImage(fImg, cx - sz / 2, cy - sz / 2, sz, sz);
        }
      }


      // Shelter indicator: 1=has shelter — PNG centred on hex, emoji fallback
      if (cell.shelter) {
        const si   = 0;  // basic shelter (bit 6 is now boolean)
        const imgs = shelterImgs[si];
        const v    = imgs.length > 0 ? (mapQ * 31 + mapR * 17) % imgs.length : -1;
        const sImg = v >= 0 ? imgs[v] : null;
        if (sImg && sImg.loaded) {
          const sz = HEX_SZ * 0.9;
          ctx.drawImage(sImg, cx - sz / 2, cy - sz / 2, sz, sz);
        } else {
          ctx.save();
          ctx.fillStyle    = cell.shelter === 2 ? '#7EC8E3' : '#D4A574';
          ctx.font         = `${Math.max(12, Math.round(HEX_SZ * 0.5))}px monospace`;
          ctx.textAlign    = 'right';
          ctx.textBaseline = 'top';
          ctx.fillText(cell.shelter === 2 ? '🏠' : '⛺', cx + HEX_SZ * 0.35, cy - HEX_SZ * 0.35);
          ctx.restore();
        }
      }

      // Weather icon — small 🌧 in upper-left corner of each visible hex
      if (weatherPhase > 0) {
        ctx.save();
        ctx.font         = `${Math.max(8, Math.round(HEX_SZ * 0.28))}px serif`;
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'top';
        ctx.globalAlpha  = 0.75;
        ctx.fillText('🌧', cx - HEX_SZ * 0.48, cy - HEX_SZ * 0.48);
        ctx.restore();
      }
    }
  }

  // ── Pass 1b: Hex grid lines (drawn on top of terrain PNGs) ──────────
  ctx.strokeStyle = 'rgba(50,50,50,0.5)';
  ctx.lineWidth   = 1.0;
  ctx.globalAlpha = 1;
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq = centreQ + dq;
      const vr = centreR + dr;
      const px = hexToPixel(vq, vr, HEX_SZ);
      const cx = px.x + ox;
      const cy = px.y + oy;
      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;
      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      ctx.stroke();
    }
  }

  // ── POI hex outline (yellow, pulsing, visible cells only) ──
  const _poiAlpha = 0.7 + 0.2 * Math.sin(performance.now() / 400);
  ctx.strokeStyle = `rgba(255,210,0,${_poiAlpha})`;
  ctx.lineWidth   = 3.0;
  ctx.globalAlpha = 1;
  for (let dr = -viewR; dr <= viewR; dr++) {
    for (let dq = -viewQ; dq <= viewQ; dq++) {
      const vq   = centreQ + dq;
      const vr   = centreR + dr;
      const mapQ = ((vq % MAP_COLS) + MAP_COLS) % MAP_COLS;
      const mapR = ((vr % MAP_ROWS) + MAP_ROWS) % MAP_ROWS;
      const px   = hexToPixel(vq, vr, HEX_SZ);
      const cx   = px.x + ox;
      const cy   = px.y + oy;
      if (cx < -HEX_SZ * 2 || cx > cssWidth  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > cssHeight + HEX_SZ * 2) continue;
      const cell = gameMap[mapR]?.[mapQ];
      if (!cell?.poi) continue;
      const dist2 = hexDistWrap(meAct.q, meAct.r, mapQ, mapR);
      if (dist2 > myVisionR && !surveyedCells.has(`${mapQ}_${mapR}`)) continue;
      drawHexPath(ctx, cx, cy, HEX_SZ - 1);
      ctx.stroke();
    }
  }

  // ── Current hex outline highlight ────────────────────────────────
  if (myId >= 0 && players[myId]?.on) {
    const mePx = hexToPixel(meAct.q, meAct.r, HEX_SZ);
    drawHexPath(ctx, mePx.x + ox, mePx.y + oy, HEX_SZ - 1);
    ctx.globalAlpha = 1;
    ctx.strokeStyle = 'rgba(90,90,90,0.5)';
    ctx.lineWidth   = 1.0;
    ctx.stroke();
  }

  // ── Pass 2: Character icons ────────────────────────────────────
  for (let i = 0; i < MAX_PLAYERS; i++) {
    const p = players[i];
    if (!p.on) continue;

    const rp = renderPos[i];
    let vq = rp.q, vr = rp.r;
    let bestD = Infinity;
    for (let dq2 = -1; dq2 <= 1; dq2++) {
      for (let dr2 = -1; dr2 <= 1; dr2++) {
        const d = hexDist(meRp.q, meRp.r,
                          rp.q + dq2 * MAP_COLS, rp.r + dr2 * MAP_ROWS);
        if (d < bestD) { bestD = d; vq = rp.q + dq2 * MAP_COLS; vr = rp.r + dr2 * MAP_ROWS; }
      }
    }

    const pp  = hexToPixel(vq, vr, HEX_SZ);
    const pcx = pp.x + ox;
    const pcy = pp.y + oy;
    if (pcx < -HEX_SZ * 2 || pcx > cssWidth  + HEX_SZ * 2) continue;
    if (pcy < -HEX_SZ * 2 || pcy > cssHeight + HEX_SZ * 2) continue;

    const _archColor = ARCHETYPE_COLORS[players[i].arch ?? 0] ?? PLAYER_COLORS[i];
    drawCharIcon(ctx, pcx, pcy, HEX_SZ, _archColor, i, i === myId,
                 players[i].nm, players[i].arch ?? 0, players[i].sc ?? 0);
  }

  // ── Pass 2.5: Weather overlay + particles ────────────────────
  if (weatherPhase > 0) {
    const wNow = Date.now();
    // Helper: draw many short individual rain drops falling downward.
    // Each drop is a short 1px line at a slight angle; opacity varies per drop
    // using a golden-angle seed so the variation is distributed evenly, not banded.
    function drawRainDrops(count, dropLen, sinA, color, speedMs) {
      const cosA    = Math.sqrt(Math.max(0, 1 - sinA * sinA));
      const cycle   = cssHeight + dropLen;
      const scrollY = (wNow / speedMs * cycle) % cycle;
      ctx.save();
      ctx.strokeStyle = color;
      ctx.lineWidth   = 1;
      for (let i = 0; i < count; i++) {
        const seed = i * 137.508;                           // golden-angle spread
        const x    = seed % cssWidth;
        const y    = ((seed * 0.618 + scrollY) % cycle) - dropLen;
        ctx.globalAlpha = 0.18 + (i % 13) / 13 * 0.50;    // vary 0.18–0.68 per drop
        ctx.beginPath();
        ctx.moveTo(x,               y);
        ctx.lineTo(x + dropLen * sinA, y + dropLen * cosA);
        ctx.stroke();
      }
      ctx.restore();
    }

    if (weatherPhase === 1) {
      // Light blue-gray wash
      ctx.save(); ctx.globalAlpha = 0.09; ctx.fillStyle = '#667799';
      ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      // 60 drops, 12px long, slight angle, 1.8s to cross the screen
      drawRainDrops(60, 12, 0.18, '#AACCEE', 1800);
      if (weatherParticles) weatherParticles.emit(2, 1, cssWidth, cssHeight);

    } else if (weatherPhase === 2) {
      // Heavier dark overlay
      ctx.save(); ctx.globalAlpha = 0.18; ctx.fillStyle = '#334455';
      ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      // 130 drops, 20px long, steeper angle, 1.0s to cross — heavier & faster
      drawRainDrops(130, 20, 0.28, '#7799BB', 1000);
      // Lightning (3 s cycle: 0–100ms flash, 100–150ms dark, 150–200ms secondary)
      const lc = wNow % 3000;
      if (lc < 100) {
        ctx.save(); ctx.globalAlpha = Math.sin(lc / 100 * Math.PI) * 0.55;
        ctx.fillStyle = '#DDEEFF'; ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      } else if (lc >= 150 && lc < 200) {
        ctx.save(); ctx.globalAlpha = (1 - (lc - 150) / 50) * 0.25;
        ctx.fillStyle = '#BBCCEE'; ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      }
      if (weatherParticles) weatherParticles.emit(4, 2, cssWidth, cssHeight);

    } else {
      // Chem-Storm: sickly green radial gradient
      const cx2 = cssWidth / 2, cy2 = cssHeight / 2;
      const grad = ctx.createRadialGradient(cx2, cy2, 0, cx2, cy2,
        Math.max(cssWidth, cssHeight) * 0.8);
      grad.addColorStop(0,   'rgba(60,120,20,0.18)');
      grad.addColorStop(0.6, 'rgba(30,80,10,0.10)');
      grad.addColorStop(1,   'rgba(0,0,0,0)');
      ctx.save(); ctx.fillStyle = grad;
      ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      // Radiation pulse (4 s cycle)
      const rc = wNow % 4000;
      let pa = 0;
      if      (rc < 100)  pa = (rc / 100)          * 0.12;
      else if (rc < 150)  pa = ((150 - rc) / 50)   * 0.06;
      else if (rc < 220)  pa = ((rc - 150) / 70)   * 0.12;
      else if (rc < 2500) pa = 0;
      else if (rc < 2650) pa = ((rc - 2500) / 150) * 0.20;
      else                pa = ((4000 - rc) / 1350) * 0.20;
      if (pa > 0) {
        ctx.save(); ctx.globalAlpha = pa; ctx.fillStyle = '#44BB22';
        ctx.fillRect(0, 0, cssWidth, cssHeight); ctx.restore();
      }
      if (weatherParticles) weatherParticles.emit(6, 3, cssWidth, cssHeight);
    }
    if (weatherParticles) { weatherParticles.update(); weatherParticles.render(ctx); }
  }

  // ── Pass 3: Ash particle overlay ─────────────────────────────
  if (ashParticles) {
    ashParticles.update(gameMap, HEX_SZ);
    ashParticles.render(ctx, ox, oy, HEX_SZ);
  }

  // ── Time-of-day tint overlay ──────────────────────────────────
  {
    // Lerp displayMP: fast toward uiMP.val normally; slow drift to 0 while resting.
    // Exception: if nightFade > 0 a dawn event just fired — don't lerp toward 0 (BUG-13).
    // nightFade > 0 means dawn already happened; trust displayMP (snapped at dawn) not resting flag.
    const stillResting = uiResting.val && nightFade <= 0;
    const mpTarget = stillResting ? 0 : uiMP.val;
    const lerpRate = stillResting ? RESTING_LERP_RATE : MOVING_LERP_RATE;
    displayMP += (mpTarget - displayMP) * lerpRate;
    const phase = getTimePhase(displayMP);
    ctx.save();
    ctx.globalAlpha = phase.a;
    ctx.fillStyle   = `rgb(${phase.r},${phase.g},${phase.b})`;
    ctx.fillRect(0, 0, cssWidth, cssHeight);
    ctx.restore();
  }

  // ── Night linger fade (dawn transition) ──────────────────────
  if (nightFade > 0) {
    ctx.save();
    ctx.globalAlpha = nightFade;
    ctx.fillStyle = 'rgb(15,8,40)';
    ctx.fillRect(0, 0, cssWidth, cssHeight);
    ctx.restore();
    nightFade = Math.max(0, nightFade - NIGHT_FADE_DECAY_RATE); // ~3 s at 60fps
  }

  requestAnimationFrame(render);
}
