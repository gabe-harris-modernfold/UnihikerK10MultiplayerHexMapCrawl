/* global myId, myVisionR, weatherPhase, groundItems, maxMP, displayMP, nightFade, pickTimeoutId */

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
    if (socket === undefined || !socket) return 'no socket';
    return ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'][socket.readyState] ?? '?';
  }

  function report() {
    const now    = Date.now();
    const upSec  = d.lastConnectedAt ? Math.round((now - d.lastConnectedAt) / 1000) : null;
    const ageSec = Math.round((now - d.sessionStart) / 1000);
    const lastMsgAgo = d.lastMsgAt ? Math.round((now - d.lastMsgAt) / 1000) + 's ago' : '—';
    const myPos = (typeof myId !== 'undefined' && myId >= 0 &&
                   typeof players !== 'undefined' && players[myId])
                  ? `q:${players[myId].q} r:${players[myId].r}` : '—';
    const connCount = typeof players !== 'undefined' ? players.filter(p => p?.on).length : '—';
    console.group('%c[DIAG] Wasteland Crawl — Client Diagnostics', 'color:#4fc;font-weight:bold');
    console.log(`Session age      : ${ageSec}s`);
    console.log(`WS state         : ${wsState()}`);
    console.log(`Connections      : ${d.wsConnections}  |  Disconnects: ${d.wsDisconnects}  |  Errors: ${d.wsErrors}`);
    console.log(`Current uptime   : ${upSec == null ? '—' : upSec + 's'}`);
    console.log(`Dropped sends    : ${d.droppedSends}`);
    console.log(`Msgs sent        : ${d.msgSent}  |  Received: ${d.msgReceived}`);
    console.log(`Last msg recv    : ${lastMsgAgo}`);
    console.log(`Avg RTT          : ${avgRtt() == null ? '—' : avgRtt() + 'ms'}  (${d.responseTimes.length} samples)`);
    console.log(`Min / Max RTT    : ${d.responseTimes.length ? Math.min(...d.responseTimes) + 'ms / ' + Math.max(...d.responseTimes) + 'ms' : '—'}`);
    console.log(`Player ID        : ${myId === undefined ? '—' : myId}`);
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
    console.warn('%c[WS ⚠] Dropped send — socket not open', 'color:#fa0', `total=${d.droppedSends} state=${wsState()}`);
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
    console.info('%c[WS ▲] Connected', 'color:#4fc;font-weight:bold', `#${d.wsConnections} at ${new Date().toISOString()} — prior disconnects: ${d.wsDisconnects}`);
  }

  function onDisconnect() {
    d.wsDisconnects++;
    d.lastDisconnectedAt = Date.now();
    console.warn('%c[WS ▼] Disconnected', 'color:#fa0;font-weight:bold', `#${d.wsDisconnects} — total dropped sends: ${d.droppedSends}`);
    report();
  }

  function onError(ev) {
    d.wsErrors++;
    console.error(`[WS ✖] Error #${d.wsErrors} at ${new Date().toISOString()}`, ev);
    report();
  }

  setInterval(report, 60_000);
  globalThis.diag = { report, data: d, avgRtt };

  return { onSend, onDropped, onMsg, onConnect, onDisconnect, onError, report, wsState, data: d };
})();

// ── Move cooldown tracking (client-side estimate) ─────────────────
let moveCooldownMs = MOVE_COOLDOWN_BASE_MS; // mirrors server MOVE_CD_MS × current terrain MC
let restSent       = false; // guard against REST double-click before server ack

// ── WebSocket ────────────────────────────────────────────────────
const wsUrl = `ws://${location.host}/ws`;

const CONN_LOST_QUIPS = [
  '☠ The signal dies. The wasteland keeps no promises of uptime.',
  '☢ Connection lost. Somewhere, a server breathes its last in the ash.',
  '📡 Dead air. Even the ghosts of the old internet have moved on.',
  '☠ The wire goes cold. Raiders got the repeater again.',
  '☢ Your packets dissolved into the fallout. Lost, like everything else.',
  '☠ Connection severed. The bandwidth died without last rites.',
  '📻 You reach out. Nothing answers but the hum of dead frequencies.',
  '⚡ The handshake fails. Trust is hard to come by after the collapse.',
  '☢ Signal lost. Not even cockroaches get reception out here.',
  '☠ Dropped. Like everything else worth keeping before the bombs fell.',
  '🔌 The plug got pulled. Probably on purpose.',
  '☠ Silence. The kind that follows an explosion.',
  '📡 The tower is down. Someone always shoots the tower.',
  '☢ Timeout. The server achieved a state of peaceful non-existence.',
  '☠ No carrier. No hope. No signal.',
  '⚡ The relay burned out. Third one this month.',
  '📻 Static swallows everything. Including you, apparently.',
  '☢ Lost in the noise. The wasteland is lousy with interference.',
  '☠ Connection refused. Even the machines have given up.',
  '📡 The uplink is gone. Ashes, mostly.',
];
let socket;
let reconnectAttempts = 0;  // drives backoff; reset on a successful onopen
let pendingActions = [];    // [{obj, ts}] — move/act dropped while offline, replayed on reconnect (oldest first)

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
    Diag.data.lastMsgAt = Date.now();  // else the staleness watchdog re-trips on the stale pre-disconnect value
    reconnectAttempts = 0;
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
  socket.onclose   = (ev) => {
    const msSinceEnc = globalThis._lastEncStartT ? (Date.now() - globalThis._lastEncStartT) : '—';
    console.warn('%c[WS ▼] Closed', 'color:#fa0;font-weight:bold', `code=${ev.code} wasClean=${ev.wasClean} reason="${ev.reason}" msSinceEncStart=${msSinceEnc}`);
    showToast(CONN_LOST_QUIPS[Math.floor(Math.random() * CONN_LOST_QUIPS.length)]);
    Diag.onDisconnect(); setStatus('Reconnecting...');
    // Exponential backoff + proportional jitter — spreads out up to 6 clients
    // reconnecting at once (e.g. right after a board reboot) instead of
    // hammering it in lockstep on a flat interval.
    const backoff = Math.min(RECONNECT_BASE_MS * 2 ** reconnectAttempts, RECONNECT_MAX_MS);
    const delay = backoff * (1 - RECONNECT_JITTER_PCT / 2 + Math.random() * RECONNECT_JITTER_PCT);
    reconnectAttempts++;
    setTimeout(connect, delay);
  };
  socket.onerror   = (event) => {
    Diag.onError(event);
    setStatus('Connection error — retrying...');
  };
  socket.onmessage = e => { const msg = JSON.parse(e.data); Diag.onMsg(msg?.t); handleMsg(msg); };
}
function send(obj) {
  if (socket?.readyState === WebSocket.OPEN) {
    Diag.onSend(obj);
    console.log('%c→ TX [%s]', 'color:#0c0;font-weight:bold', obj.t, obj);
    socket.send(JSON.stringify(obj));
  } else {
    Diag.onDropped();
    // Silent auto-replay: only for move/act (server-validated on replay via
    // the existing col_fail/err paths, same as any other rejected action) —
    // not pick/wifi/trade_*/use_item, which either already resend themselves
    // or carry more risk if fired again after the world moved on.
    if (obj.t === 'm' || obj.t === 'act') {
      pendingActions.push({ obj, ts: Date.now() });
      if (pendingActions.length > PENDING_ACTION_MAX) pendingActions.shift();
    }
  }
}

// Replays queued move/act input once a fresh sync confirms we're back in the
// game with current state — called from _msgSync(), not onopen, since a full
// reconnect still has to clear the lobby→pick→sync handshake server-side
// before there's a slot to apply these to. Anything older than the TTL is
// dropped outright rather than replayed into a world that's moved on.
function _flushPendingActions() {
  if (!pendingActions.length) return;
  const now = Date.now();
  const toSend = pendingActions.filter(a => now - a.ts <= PENDING_ACTION_TTL_MS);
  pendingActions = [];
  toSend.forEach(a => send(a.obj));
}

// Staleness watchdog: once in-game, broadcastState() is unconditional every
// 100ms server-side, so a healthy connection is never quiet this long. Catches
// a half-dead socket the browser itself hasn't noticed yet and forces the
// existing reconnect path instead of waiting on it. Gated on myId >= 0: the
// lobby/char-select screen has no such guarantee (no player slot yet =
// nothing to broadcast) and already has its own pick-timeout handling.
setInterval(() => {
  if (myId >= 0 && socket?.readyState === WebSocket.OPEN &&
      Date.now() - Diag.data.lastMsgAt > WS_STALE_THRESHOLD_MS) {
    console.warn('%c[WS ⚠] Stale connection (no msg in %dms) — forcing reconnect', 'color:#fa0', WS_STALE_THRESHOLD_MS);
    socket.close();
  }
}, WS_STALE_CHECK_INTERVAL_MS);


function checkAutoRest() {
  if (myId < 0 || !players[myId]?.on) return;
  if (players[myId].rest) return;           // already resting
  if (uiResting.val) return;
  const others = players.filter(p => p.on && p.id !== myId);
  if (others.length === 0) return;          // solo — don't auto-rest
  if (others.every(p => p.rest)) {
    // All other connected players are resting — auto-rest
    send({ t: 'act', a: ACT_REST });
  }
}

// ── Shared helpers ───────────────────────────────────────────────────────────

function _clearPickTimeout() {
  if (pickTimeoutId) { clearTimeout(pickTimeoutId); pickTimeoutId = null; }
}

// Catch stale downed state on reconnect: if server shows ll:0 we are dead.
// Close the socket so the server resets the slot (p.connected=false) and re-adds us
// to the lobby on reconnect — otherwise a second pick is rejected as "not in lobby".
function _checkDownedState() {
  if (myId >= 0 && !pendingLobbyRedirect && players[myId].ll === 0) {
    myId = -1;
    pendingLobbyRedirect = true;
    addLog('<span class="log-check-fail">☠ DOWNED — the wasteland claims you.</span>');
    showToast('☠ The wasteland claims you. Your story ends in the dust.');
    socket.close();  // triggers server slot reset + auto-reconnect → re-enter lobby
    setTimeout(() => { pendingLobbyRedirect = false; showCharSelect(); }, 3500);
  }
}

// ── handleMsg sub-handlers ───────────────────────────────────────────────────

function _applyGameState(gs) {
  Object.assign(gameState, gs);
  if (gs.wp !== undefined) { weatherPhase = gs.wp; updateWeatherHUD(); }
}

// Merges the "world" sync/state key into worldState. A partial payload (e.g.
// Phase 1's `{caravan:{...}}` with no `doom`/`fire` keys yet) leaves the
// other fields at their current value rather than clobbering them.
let _lastCaravanHex = null;  // {q,r} — last hex we stamped a tire track onto
function _applyWorldState(world) {
  Object.assign(worldState, world);
  if (world.fire) fireField?.setFromSync(world.fire);
  // Stamp tire track immediately, same reason _evMv stamps footprints: the
  // authoritative bit only reaches us via a vis-disk covering that hex, which
  // may never happen for a player who's never nearby. Caravan position rides
  // every state broadcast regardless of vision, so use that instead of
  // waiting on one. Only patches an already-revealed cell — a still-fogged
  // hex gets the real bit for free whenever it's eventually revealed.
  const c = worldState.caravan;
  if (c?.active && (c.q !== _lastCaravanHex?.q || c.r !== _lastCaravanHex?.r)) {
    if (gameMap[c.r]?.[c.q]) gameMap[c.r][c.q] = { ...gameMap[c.r][c.q], tireTrack: 1 };
    _lastCaravanHex = { q: c.q, r: c.r };
  }
}

function _msgAsgn(msg) {
  if (typeof msg.id !== 'number' || msg.id < 0 || msg.id >= MAX_PLAYERS) {
    console.warn('[ASGN] Invalid player ID:', msg.id);
    return;
  }
  console.log('%c[ASGN] Assigned slot %d', 'color:#09f;font-weight:bold', msg.id, `(prev myId=${myId})`);
  myId = msg.id;
  _clearPickTimeout();
  uiPickPending.val = false;
  uiResting.val = false;  // clear any stale resting state from the previous survivor
  hideCharSelect();
}

function _msgLobby(msg) {
  lobbyAvail.val = Array.isArray(msg.avail) ? msg.avail : [];
  // Start preloading hex/shelter/forage-animal art immediately on connect,
  // before the player has picked a character — these counts are static for
  // the boot session, so there's no need to wait for sync. loadTerrainVariants
  // et al. guard against a redundant reload when sync arrives later.
  if (msg.vc) loadTerrainVariants(msg.vc);
  if (msg.sv) loadShelterVariants(msg.sv);
  if (msg.fa) loadForrageAnimalImgs(msg.fa);
  _clearPickTimeout();
  uiPickPending.val = false;
  console.log('%c[LOBBY] avail=%o myId=%d pendingLobbyRedirect=%s', 'color:#09f;font-weight:bold', lobbyAvail.val, myId, pendingLobbyRedirect);
  if (!pendingLobbyRedirect) {
    if (myId >= 0 && lobbyAvail.val.includes(myId)) {
      // Already assigned and slot still free: auto-repick same slot on reconnect (BUG-01)
      console.log('[LOBBY] auto-repick slot %d', myId);
      send({ t: 'pick', arch: myId });
    } else {
      // Slot taken or unassigned — show character select
      if (myId >= 0) { console.log('[LOBBY] slot %d taken — resetting myId, showing char select', myId); myId = -1; }
      else console.log('[LOBBY] unassigned — showing char select');
      showCharSelect();
    }
  } else {
    console.log('[LOBBY] suppressed — pendingLobbyRedirect active');
  }
}

function _msgSync(msg) {
  console.log('%c[SYNC] id=%s vr=%s mapLen=%d playerCount=%d', 'color:#09f', msg.id ?? '(unchanged)', msg.vr ?? '(unchanged)', msg.map?.length ?? 0, msg.p?.length ?? 0);
  if (msg.id  !== undefined) myId = msg.id;
  if (msg.vr  !== undefined) myVisionR = msg.vr;
  if (typeof msg.map !== 'string') return;
  // Sync replaces gameMap wholesale — drop stale locally-cached collect/survey
  // markers so they don't override the fresh server state.
  collectedCells.clear();
  surveyedCells.clear();
  parseMapFog(msg.map);
  if (!Array.isArray(msg.p)) return;
  msg.p.forEach(p => {
    if (typeof p.id !== 'number' || p.id < 0 || p.id >= MAX_PLAYERS) return;
    Object.assign(players[p.id], p);
    players[p.id].rest = !!p.rt;  // map rt → rest (mirrors 's' handler)
    if (p.on) { renderPos[p.id].q = p.q; renderPos[p.id].r = p.r; }
  });
  _packFullRearmCheck();   // reconnect/resync — re-arm if the pack shrank meanwhile
  if (msg.vc) loadTerrainVariants(msg.vc);
  if (msg.sv) loadShelterVariants(msg.sv);
  if (msg.fa) loadForrageAnimalImgs(msg.fa);
  if (msg.gs) _applyGameState(msg.gs);
  if (msg.world) _applyWorldState(msg.world);
  if (Array.isArray(msg.gi)) groundItems = msg.gi;
  hideConnectOverlay();
  if (myId >= 0) hideCharSelect();  // belt-and-suspenders: hide picker if sync arrives before/without asgn
  if (myId >= 0) uiResting.val = !!players[myId].rest;  // sync resting state on reconnect
  updateSidebar();
  // maxMP is the DAY'S BUDGET and only a dawn event knows it. players[].mp is
  // what is LEFT, so seeding it here rescaled the MP track (and renderer.js's
  // time-of-day fade, which divides by maxMP) to whatever was unspent at the
  // moment of a mid-day reconnect. Only raise it, never shrink it: on a fresh
  // connect it is the best estimate available, and the next dawn corrects it.
  if (myId >= 0 && players[myId]?.mp > maxMP) { maxMP = players[myId].mp; uiMaxMP.val = maxMP; }
  if (myId >= 0) displayMP = players[myId].mp ?? 6;
  if (myId >= 0 && players[myId]?.llCap) uiLLCap.val = players[myId].llCap;
  updateTerrainCard();
  updateDirButtons();
  _checkDownedState();
  if (myId >= 0) _flushPendingActions();  // _checkDownedState may have reset myId to -1 above — skip replay for a dead slot
  // Re-render char-sheet if open — ensures wounds/inventory reflect fresh server state (BUG-10)
  if (document.getElementById('char-overlay')?.classList.contains('open')) {
    renderInventory?.();
    renderEquipment?.();
    if (myId >= 0) renderWounds?.(players[myId]);
  }
  globalThis.maybeAutoOpenCaravanTrade?.();
}

function _msgState(msg) {
  if (!Array.isArray(msg.p)) return;
  msg.p.forEach((pd, i) => {
    if (i < 0 || i >= MAX_PLAYERS) return;
    const p = players[i];
    const wasOn = p.on;
    p.on = pd.on; p.q = pd.q; p.r = pd.r; p.sc = pd.sc;
    // Snap renderPos when a player first comes online so they don't lerp from (0,0) (Bug-4)
    if (pd.on && !wasOn) { renderPos[i].q = pd.q; renderPos[i].r = pd.r; }
    p.inv = pd.inv; p.sp = pd.sp;
    p.ll = pd.ll; p.food = pd.food; p.water = pd.water;
    p.rad = pd.rad;

    if (pd.mp  !== undefined) p.mp  = pd.mp;
    if (pd.fth !== undefined) p.fth = pd.fth;   // F threshold bitmask
    if (pd.wth !== undefined) p.wth = pd.wth;   // W threshold bitmask
    if (pd.wnd)               p.wnd = pd.wnd;   // [minor, major] wound counts
    if (pd.vm  !== undefined) p.vm  = pd.vm;    // valid move bitmask
    if (pd.rt  !== undefined) {
      p.rest = !!pd.rt;
      if (i === myId) uiResting.val = !!pd.rt;
    }
    if (pd.it) p.it = pd.it;   // typed inventory types
    if (pd.iq) p.iq = pd.iq;   // typed inventory quantities
    if (pd.eq) p.eq = pd.eq;   // equipment slots
    // is/llCap are the EFFECTIVE values (archetype base + equipment), computed
    // by appendPackArrays() server-side. Never recompute them from eq[] on top
    // of these — that double-counts the bonus.
    if (pd.is    !== undefined) p.is    = pd.is;
    if (pd.llCap !== undefined) p.llCap = pd.llCap;
    if (pd.kr !== undefined) p.kr = pd.kr;  // known-recipes bitmask (mock's 's' sends it; firmware's doesn't — kr there only ever arrives via 'sync' or a craft item_result)
    if (pd.enc !== undefined) p.enc = !!pd.enc;  // encounter lock
    // Bunker tunnels: which board this survivor is on, and where on it.
    // q/r above stay pinned to their entrance hatch while dp is 1.
    if (pd.dp !== undefined) {
      const wasDp = p.dp | 0;
      p.dp = pd.dp | 0;
      p.tq = pd.tq | 0;
      p.tr = pd.tr | 0;
      // Changing board teleports a player as far as the renderer is
      // concerned -- snap rather than lerping them across the screen.
      if (p.dp !== wasDp) { const v = playerViewPos(i); if (v) { renderPos[i].q = v.q; renderPos[i].r = v.r; } }
      if (i === myId) setMyDepth(p.dp);
    }
  });
  _packFullRearmCheck();   // fresh token totals — did the player free up room?
  // Sync can carry stale eq after item_result; refresh equipment grid only if open.
  // Do NOT call renderInventory here — sync fires on every tick and would hammer
  // the item icon requests on every cycle. Inventory is already up-to-date from item_result.
  if (document.getElementById('char-overlay')?.classList.contains('open')) {
    renderEquipment?.();
    if (myId >= 0) renderWounds?.(players[myId]);
  }
  if (msg.gs) _applyGameState(msg.gs);
  if (msg.world) _applyWorldState(msg.world);
  updateSidebar();
  updateTerrainCard();
  updateDirButtons();
  _checkDownedState();
  // Both halves of "am I standing on the caravan?" ride this message, so
  // the shelf pops within a tick of the step (ui-panels.js).
  globalThis.maybeAutoOpenCaravanTrade?.();
}

function _handleSelfVis() {
  const _me = players[myId];
  const _here = playerViewPos(myId) ?? { q: _me.q, r: _me.r };
  const _cell = boardCell(_here.q, _here.r);
  if (_cell) {
    // Mirrors canEnterTerrain() + movePlayer(): terrain MC, the equipment
    // perks that override it, then the weather movement penalty. Reading
    // TERRAIN[].mc alone charged a Vertical Regret wearer the full Mountain
    // MC 4 cooldown for a step the server billed at 2, and ignored the Raft
    // on water entirely.
    let _mc = terrainMCFor(_cell.terrain, _me);
    if (_mc) moveCooldownMs = MOVE_COOLDOWN_BASE_MS * (_mc + (WEATHER_MOVE_PENALTY[weatherPhase] ?? 0));
  }
  // A "hex still holds a resource after the move → pack must be full" toast
  // used to live here. It guessed, and it misfired: the server answers *every*
  // `m` frame with a vis disk (rejected moves included) and use_item fog
  // reveals send one too, so standing on an uncollectable pile re-toasted on
  // every keypress — on top of the col_fail toast for the same pickup. The
  // server's col_fail (reason 2) is authoritative; it is the only notice now.
  // Auto-trigger encounter: vis fires after applyVisDisk so the board is fresh.
  // Coordinates are whichever board we are on — handleMsg_enc_start() reads
  // the player's own depth server-side, so nothing extra rides the message.
  if (_cell?.poi) {
    globalThis._lastEncStartT = Date.now();
    console.log('%c[ENC] POI detected — sending enc_start', 'color:#c0f;font-weight:bold', `q=${_here.q} r=${_here.r} dp=${myDepth} t=${globalThis._lastEncStartT}`);
    send({ t: 'enc_start', q: _here.q, r: _here.r });
  }
}

function _msgVis(msg) {
  if (msg.vr !== undefined) myVisionR = msg.vr;
  // "dp" names the board these cells belong to. Underground the coordinates
  // are tunnel coordinates, so they must land in tq/tr — writing them to q/r
  // would move the player's surface pin off their entrance hatch.
  const depth = msg.dp ? 1 : 0;
  if (msg.q !== undefined && myId >= 0) {
    if (depth) { players[myId].tq = msg.q; players[myId].tr = msg.r; }
    else       { players[myId].q  = msg.q; players[myId].r  = msg.r; }
  }
  applyVisDisk(msg.cells, depth);
  if (myId >= 0) _handleSelfVis();
  updateTerrainCard();
  updateSidebar();
}

// ── Tunnel board sync ────────────────────────────────────────────
// The whole fogged bunker tunnel board, sent on descend and on reconnecting
// while already underground. Carries its own dimensions — the firmware owns
// TUN_COLS/TUN_ROWS and the client just follows.
function _msgTunnelSync(msg) {
  initTunnelMap(msg.cols | 0, msg.rows | 0);
  parseMapFog(msg.map, tunnelMap, tunnelCols, tunnelRows, 'TUN');
  if (msg.vr !== undefined) myVisionR = msg.vr;
  if (msg.q !== undefined && myId >= 0) {
    players[myId].tq = msg.q;
    players[myId].tr = msg.r;
  }
  setMyDepth(1);
  updateTerrainCard();
  updateSidebar();
  updateDirButtons();
}

function _msgGroundUpdate(msg) {
  if (Array.isArray(msg.gi)) groundItems = msg.gi;
  if (document.getElementById('hex-info')?.classList.contains('open') && myId >= 0) {
    renderHexGroundItems?.(players[myId].q, players[myId].r);
  }
}

function _msgWifi(msg) {
  if (msg.status === 'saved') {
    // Server echoed its saved credentials — keep localStorage in sync.
    serverHasWifiCreds = true;
    if (msg.ssid) localStorage.setItem('wifi_ssid', msg.ssid);
    if (msg.pass !== undefined) localStorage.setItem('wifi_pass', msg.pass);
  } else if (msg.status === 'nets') {
    // Roaming list — every network the board will go looking for on its own.
    uiWifiNets.val = Array.isArray(msg.nets) ? msg.nets : [];
    uiWifiCur.val  = msg.cur || '';
  } else if (msg.status === 'link') {
    // How we reached the board. ap=1 means we are on its own softAP, where one
    // radio is doing the job of a whole router - see showUplinkWarning().
    uiApLink.val = !!msg.ap;
    uiApCap.val  = msg.cap | 0;
    uiStaIp.val  = msg.ip || '';
    if (msg.ap) showUplinkWarning(msg.cap | 0, msg.ip || '');
  } else if (msg.status === 'forgot') {
    // Drop our cached copy too, otherwise the next reconnect auto-sends these
    // credentials and the board re-learns the network we just told it to forget.
    if (localStorage.getItem('wifi_ssid') === msg.ssid) {
      localStorage.removeItem('wifi_ssid');
      localStorage.removeItem('wifi_pass');
      const si = document.getElementById('wifi-ssid'); if (si) si.value = '';
      const pi = document.getElementById('wifi-pass'); if (pi) pi.value = '';
    }
  }
}

// ── Message handler ──────────────────────────────────────────────────────────

function handleMsg(msg) {
  if (!msg?.t) {
    console.warn('[RX] Message missing type field', msg);
    return;
  }
  if (msg.t !== 's') console.log('%c← RX [%s]', 'color:#0cf;font-weight:bold', msg.t, msg);

  switch (msg.t) {
    case 'asgn':          _msgAsgn(msg);         break;
    case 'lobby':         _msgLobby(msg);        break;
    case 'sync':          _msgSync(msg);         break;
    case 's':             _msgState(msg);        break;
    case 'vis':           _msgVis(msg);          break;
    case 'tsync':         _msgTunnelSync(msg);   break;
    case 'ev':            handleEvent(msg);      break;
    case 'ground_update': _msgGroundUpdate(msg); break;
    case 'item_result':   _evItemResult(msg);    break;
    case 'res_result':    _evResResult(msg);     break;
    case 'full':
      console.warn('[RX] Server full — all slots taken');
      document.getElementById('connect-box').innerHTML =
        '<h2>SERVER FULL</h2><p>All 6 slots taken.<br>Try again later.</p>' +
        '<button onclick="location.reload()" class="server-full-retry-btn">\u21BA RETRY</button>';
      break;
    case 'wifi':    _msgWifi(msg); break;
    case 'enc_path':
      console.log('%c[ENC] enc_path received', 'color:#c0f;font-weight:bold', `biome=${msg.biome} id=${msg.id} msSinceEncStart=${globalThis._lastEncStartT ? Date.now()-globalThis._lastEncStartT : '—'}`);
      globalThis._startEncounterFetch?.(msg.biome, msg.id);
      break;
    case 'enc_dbg':
      console.warn('[ENC] Server diagnostic:', msg.msg);
      break;
    case 'err':
      console.error('[ERR] Server error:', msg);
      // _onEncError unlocks the encounter dialog if a choice was refused; for
      // any other rejection (e.g. a blocked trade/move/act) it returns false
      // so the player still sees *something* instead of the error vanishing.
      if (!globalThis._onEncError?.(msg.msg)) showToast(msg.msg || 'Action failed.');
      break;
    case 'trade_fail': {
      // `why` only comes back from car_buy (handleMsg_caravan_buy): 1 the
      // caravan left / the item sold out, 2 can't cover the price, 3 no pack
      // room, 4 tried to pay with water (the caravan refuses it).
      const WHY = {
        1: '⇄ The caravan has moved on, or that item just sold out.',
        2: '⇄ Not enough tokens to cover that price.',
        3: '⇄ Pack full — make room before you buy.',
        4: "⇄ The caravan won't take water — pay in food, fuel, meds or scrap.",
      };
      showToast(WHY[msg.why] || '⇄ Trade offer failed — check the target is still on your hex and you have the resources.');
      break;
    }
  }
  buildAgentState();
}

// ── handleEvent sub-handlers ─────────────────────────────────────────────────

// ── Pack-full pickup notice (armed once per "pack filled up" episode) ────────
// The carry cap counts resource *tokens* (Water/Food/Fuel/Med/Scrap), not the
// typed item grid — so `drop_item` does nothing for it. The sinks are: the
// char-sheet inventory boxes (`drop_res` — dumps tokens straight onto the hex),
// TREAT (spends Med), SHELTER (spends Scrap), a trade, an encounter cost, and
// the dawn upkeep that eats Food/Water. Word the notice accordingly, and only
// show it when the situation is actually new:
// with a full pack every step onto a resource hex produces a col_fail, which
// used to mean one toast per step for the rest of the day.
let _packFullNotified = false;   // notice already shown for the current episode
let _packFullAtTotal  = 0;       // token total when it was shown — re-arm below this

function _invTokenTotal(pid) {
  const inv = players[pid]?.inv;
  return Array.isArray(inv) ? inv.reduce((a, b) => a + (b || 0), 0) : 0;
}

// Called whenever fresh server totals land for us. Spending anything (treat,
// shelter, trade, dawn upkeep) re-arms the notice so the next blocked pickup
// tells the player their pack filled up again.
function _packFullRearmCheck() {
  if (!_packFullNotified || myId < 0) return;
  if (_invTokenTotal(myId) < _packFullAtTotal) {
    _packFullNotified = false;
    console.log('[COL] pack-full notice re-armed — tokens dropped below', _packFullAtTotal);
  }
}

function _evCol(ev) {
  // `rem` (post-pickup remaining amount on hex, 0 = drained) was added with the
  // partial-pickup leak fix. Older servers that don't send it default to 0,
  // matching pre-fix behavior.
  const rem = ev.rem ?? 0;
  const grid = ev.dp ? tunnelMap : gameMap;
  if (grid[ev.r]?.[ev.q]) {
    grid[ev.r][ev.q].resource = rem > 0 ? ev.res : 0;
    grid[ev.r][ev.q].amount   = rem;
  } else {
    console.log('[COL] board miss — no cached cell at', ev.q, ev.r, 'dp=', ev.dp | 0, '(client never had it in vision)');
  }
  // Only mark the hex as "locally collected" when fully drained; otherwise the
  // map-decoder guard in applyVisDisk would force-zero a still-present resource.
  // Surface only: collectedCells is keyed "q_r" with no board, and the tunnel
  // visdisk skips that guard for exactly this reason.
  if (!ev.dp) {
    if (rem === 0) collectedCells.add(`${ev.q}_${ev.r}`);
    else           collectedCells.delete(`${ev.q}_${ev.r}`);
  }
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  addLog(`<span class="log-col">${escHtml(who)} +${ev.amt}× ${RES_NAMES[ev.res]}</span>`);
  if (ev.pid === myId) {
    // Optimistically update inv so the sidebar reflects the pickup immediately,
    // before the next 's' broadcast confirms the server-side inv state.
    const idx = ev.res - 1;
    if (idx >= 0 && idx < 5) players[myId].inv[idx] = (players[myId].inv[idx] ?? 0) + ev.amt;
    // Partial pickup: the pack filled mid-pile and the remainder stayed on the
    // hex. Say so once, here — no col_fail is sent for a pickup that partly
    // succeeded, and the player should know the hex still has something on it.
    if (rem > 0) {
      addLog(`<span class="log-col">🎒 Pack full — left ${rem}× ${RES_NAMES[ev.res]} on the hex.</span>`);
      if (!_packFullNotified) {
        showToast(`🎒 Pack full — left ${rem}× ${RES_NAMES[ev.res]} behind.`);
        _packFullNotified = true;
        _packFullAtTotal  = _invTokenTotal(myId);
      }
    }
    updateSidebar();
  }
}

function _evColFail(ev) {
  if (ev.pid !== myId) return;
  // reason: 1 = desync (client thought hex had a resource, server says no),
  //         2 = inventory full
  if (ev.reason === 2) {
    // `cap` is the server's effective pack size (archetype base + equipment
    // slot bonuses). Older firmware doesn't send it — fall back to the base
    // `is` from sync, and to no numbers at all if neither is known.
    // The server only blocks at total >= cap, so floor the displayed count at
    // cap: our local total can lag a broadcast, and reading "(4/6) pack full"
    // is worse than no numbers. Over-cap (encounter loot, trades) still shows.
    const have = Math.max(_invTokenTotal(myId), ev.cap ?? 0);
    const cap  = ev.cap ?? players[myId]?.is ?? 0;
    const size = cap ? ` (${have}/${cap})` : '';
    addLog(`<span class="log-col">🎒 Pickup blocked — pack full${size}: ${RES_NAMES[ev.res]} left on the hex.</span>`);
    if (!_packFullNotified) {
      // Deliberately does NOT say "drop items": drop_item only touches the
      // typed item grid, which is not what the carry cap counts. Tapping a
      // resource box on the character sheet (`drop_res`) is what dumps tokens.
      showToast(`🎒 Pack full${size} — make room: tap a resource on your character sheet to dump it, treat wounds, build a shelter, or trade.`);
      _packFullNotified = true;
      _packFullAtTotal  = have;
    }
  } else {
    // Desync: server's hex is empty but our gameMap still showed a resource.
    // Clear our stale icon so future renders match server truth.
    if (gameMap[ev.r]?.[ev.q]) {
      gameMap[ev.r][ev.q].resource = 0;
      gameMap[ev.r][ev.q].amount   = 0;
    }
    collectedCells.add(`${ev.q}_${ev.r}`);
    console.warn('[COL] desync at', ev.q, ev.r, '— cleared stale icon');
  }
}

function _handleRadiation(ev, pm) {
  pm.rad = ev.rad ?? pm.rad;
  if (ev.pid !== myId) return;
  uiRad.val = pm.rad;
  showToast(`☢ The air hums. Radiation seeps into your bones. (+${ev.radd})`);
  addLog(`<span class="log-check-fail">☢ Entered rad zone +${ev.radd}R → R:${pm.rad}</span>`);
}

function _narrateMove(ev) {
  const cell = gameMap[ev.r]?.[ev.q];
  const tName = TERRAIN[cell?.terrain ?? 0]?.name ?? 'Unknown';
  const shelterNote = cell?.shelter ? ' Shelter present.' : '';
  const exploNote   = ev.exploD > 0 ? ` +${ev.exploD}pt.` : '';
  narrateState(`Moved to ${tName} at q:${ev.q},r:${ev.r}. MP:${ev.mp}.${shelterNote}${exploNote}`);
}

function _evMv(ev) {
  const pm = players[ev.pid];
  if (ev.mp !== undefined) {
    pm.mp = ev.mp;
    if (ev.pid === myId) updateSidebar();
  }
  // Underground, q/r are tunnel coordinates: they belong in tq/tr, and the
  // surface pin must stay on the entrance hatch.
  if (ev.dp) { pm.dp = 1; pm.tq = ev.q; pm.tr = ev.r; }
  else       { pm.dp = 0; pm.q  = ev.q; pm.r  = ev.r; }
  // Surveyed cells are relative to a fixed vantage point — clear on any move.
  if (ev.pid === myId) surveyedCells.clear();
  // Stamp footprint immediately — vis-disk only goes to the moving player,
  // so other players would never see footprints without this client-side update.
  const mgrid = ev.dp ? tunnelMap : gameMap;
  if (mgrid[ev.r]?.[ev.q]) {
    const c = mgrid[ev.r][ev.q];
    mgrid[ev.r][ev.q] = { ...c, footprints: (c.footprints || 0) | (1 << ev.pid) };
  }
  // Same reasoning for a tire track: ev.trk means this step was made with
  // tracks-flagged gear equipped (the Motorbike) — mirrors the caravan's own
  // client-side stamp in _applyWorldState() above. Never set underground:
  // nothing drives a bunker corridor.
  if (ev.trk && !ev.dp && gameMap[ev.r]?.[ev.q]) {
    gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], tireTrack: 1 };
  }
  if (ev.radd && ev.radd > 0) _handleRadiation(ev, pm);
  // Exploration bonus: log first-visit score
  if (ev.exploD && ev.exploD > 0 && ev.pid === myId)
    addLog(`<span class="log-mv">\u2605 New hex \u2014 +${ev.exploD} exploration pt</span>`);
  // Narrate position for accessibility / AI agents
  if (ev.pid === myId) _narrateMove(ev);
}

function _evDowned(ev) {
  // Server has reset our slot — show death message then redirect to char selection
  myId = -1;
  pendingLobbyRedirect = true;
  console.log('%c[DOWNED] Received — starting 3.5s redirect timer', 'color:#f44;font-weight:bold', `lobbyAvail=${JSON.stringify(lobbyAvail.val)}`);
  globalThis._onEncEnd?.();  // close encounter overlay if open when player is downed
  addLog('<span class="log-check-fail">☠ DOWNED — the wasteland claims you. Find shelter next time.</span>');
  showToast('☠ The wasteland claims you. Your story ends in the dust.');
  setTimeout(() => {
    pendingLobbyRedirect = false;
    console.log('[DOWNED] Timer fired — lobbyAvail=%o', lobbyAvail.val);
    if (lobbyAvail.val.length > 0) {
      console.log('[DOWNED] Slots available — showing char select');
      showCharSelect();
    } else {
      console.log('[DOWNED] No slots available — forcing reconnect via socket.close()');
      socket.close();  // onclose → reconnect → sendLobbyMsg → lobby handler calls showCharSelect()
    }
  }, 3500);
}

function _evChk(ev) {
  const who  = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const skNm = SK_NAMES[ev.sk] ?? `SK${ev.sk}`;
  let modTxt;
  if (ev.mod === 0)    modTxt = '';
  else if (ev.mod > 0) modTxt = `+${ev.mod}`;
  else                 modTxt = `${ev.mod}`;
  const icon = ev.suc ? '\u25CF' : '\u25CB';
  const cls  = ev.suc ? 'log-check-ok' : 'log-check-fail';
  addLog(`<span class="${cls}">${icon} ${escHtml(who)} ${skNm}: ${ev.r1}+${ev.r2}+${ev.sv}${modTxt}=${ev.tot} vs DN${ev.dn}</span>`);
}

function _applyDawnToPlayer(ev) {
  const p = players[ev.pid];
  p.food  = ev.f;
  p.water = ev.w;
  p.ll    = ev.ll;
  p.mp    = ev.mp;
  if (ev.pid === myId) {
    maxMP = ev.mp; uiMaxMP.val = ev.mp; displayMP = ev.mp; nightFade = NIGHT_FADE_INIT;
    // Which equipped items could not pay their daily cost this dawn (bitmask,
    // bit 0 = head .. bit 4 = vehicle). Their MP bonus is dormant for the day
    // and the equipment panel greys it out instead of promising it.
    uiUnfuelled.val = ev.unf | 0;
  }
  p.rest = false;
  if (ev.wnd) p.wnd = ev.wnd;
  if (ev.pid === myId) {
    _tickNarrativeEffects();
    // Two different nights, two different banners. Exposure is off entirely
    // underground (survival_state.hpp), so these are mutually exclusive in
    // practice -- bad air is checked first so the ordering says which wins.
    if (uiResting.val && ev.air < 0)       showBadAirWarning();
    else if (uiResting.val && ev.expd < 0) showShelterWarning();
    uiResting.val = false;
    restSent = false;
  }
  if (ev.fth !== undefined) p.fth = ev.fth;
  if (ev.wth !== undefined) p.wth = ev.wth;
  if (ev.rad !== undefined) p.rad = ev.rad;
}

function _buildLlLogTxt(dll) {
  if (dll < 0) return ` \u25BC LL${dll}`;
  if (dll > 0) return ` \u25B2 LL+${dll}`;
  return '';
}

function _toastDawnSelf(ev) {
  let dawnMsg;
  if (ev.dll < 0)      dawnMsg = `☀ Day ${ev.day} — you wake weaker. The wastes take another piece.`;
  else if (ev.dll > 0) dawnMsg = `☀ Day ${ev.day} — you wake mended. The sun feels kind for once.`;
  else                 dawnMsg = `☀ Day ${ev.day} — the sun returns. The wastes endure, and so do you.`;
  showToast(dawnMsg);
  const _ap = document.getElementById('action-panel');
  if (_ap?.classList.contains('open')) setTimeout(openActionPanel, 0);
}

function _evDawn(ev) {
  if (ev.pid >= 0 && ev.pid < MAX_PLAYERS) _applyDawnToPlayer(ev);
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const llTxt = _buildLlLogTxt(ev.dll);
  const cls = ev.dll < 0 ? 'log-check-fail' : 'log-mv';
  if (ev.day !== undefined) gameState.dc = ev.day;
  addLog(`<span class="${cls}">\u2600 Day ${ev.day}: ${escHtml(who)} F:${ev.f} W:${ev.w}${llTxt}</span>`);
  if (ev.pid === myId) _toastDawnSelf(ev);
  updateSidebar();
}

function _applyActState(ev, p) {
  if (!p) return;
  p.mp = ev.mp;
  p.ll = ev.ll;
  if (ev.fd)  p.inv[1] = Math.min(99, (p.inv[1] ?? 0) + ev.fd);
  if (ev.wd)  p.inv[0] = Math.min(99, (p.inv[0] ?? 0) + ev.wd);
  if (ev.rad !== undefined) p.rad = ev.rad;
}

function _buildActOutcome(out) {
  if (out === AO_SUCCESS) return { outTx: '\u25CF', outCl: 'log-check-ok' };
  if (out === AO_PARTIAL) return { outTx: '\u25D1', outCl: 'log-mv' };
  if (out === AO_FAIL)    return { outTx: '\u25CB', outCl: 'log-check-fail' };
  return { outTx: '\u2297', outCl: 'log-mv' };
}

function _buildActDetail(ev) {
  let d = '';
  // Why an AO_BLOCKED action was refused (ABW_* in the .ino). Blocked used to
  // be silent, which is fine while every reason is visible on the player's own
  // screen -- "nothing to salvage here", "no MP". A pack full of scrap is not,
  // and an unexplained dead button is the worst version of that.
  if (ev.out === AO_BLOCKED && ev.bw === ABW_PACK_FULL)
    return ' — pack full, drop something first';
  if (ev.dn)     d += ` DN${ev.dn}=${ev.tot}`;
  if (ev.fd)     d += ` +${ev.fd}Food`;
  if (ev.wd)     d += ` +${ev.wd}Water`;
  if (ev.lld)    d += ` ${ev.lld > 0 ? '+' : ''}${ev.lld}LL`;
  if (ev.radd)   d += ` ${ev.radd > 0 ? '+' : ''}${ev.radd}R`;
  if (ev.ar)     d += ` \u2192 ${getRecipeById(ev.ar)?.name ?? 'something'}`;
  if (ev.scoreD) d += ` \u271F${ev.scoreD}pts`;
  return d;
}

function _handleRestSuccess(ev, who) {
  players[ev.pid].rest = true;
  if (ev.pid === myId) uiResting.val = true;
  checkAutoRest();
  addLog(`<span class="log-mv">${escHtml(who)} is now waiting for dawn</span>`);
}

function _handleShelterSuccess(ev) {
  // Update local gameMap immediately so the shelter icon renders without waiting for next move
  const sp = players[ev.pid];
  if (sp && gameMap[sp.r]?.[sp.q] != null) gameMap[sp.r][sp.q].shelter = ev.cnd;
}

function _narrateActResult(ev, actNm) {
  const actNames = ['Forage','Water','Treat','Scavenge','Shelter','Craft','Survey','Rest'];
  const outNames = ['','Success','Partial','Failed'];
  const pts = ev.scoreD ? ` +${ev.scoreD}pts.` : '';
  narrateState(`${actNames[ev.a] ?? 'Action'} — ${outNames[ev.out] ?? ev.out}. MP:${ev.mp}.${pts}`);
}

function _evAct(ev) {
  const p = ev.pid >= 0 && ev.pid < MAX_PLAYERS ? players[ev.pid] : null;
  _applyActState(ev, p);
  const who   = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const actNm = ACT_NAMES[ev.a] ?? `Act${ev.a}`;
  const { outTx, outCl } = _buildActOutcome(ev.out);
  const detail = _buildActDetail(ev);
  addLog(`<span class="${outCl}">${outTx} ${escHtml(who)} ${actNm}${detail}</span>`);
  if (ev.a === ACT_REST && ev.out === AO_SUCCESS)         _handleRestSuccess(ev, who);
  else if (ev.a === ACT_SHELTER && ev.out === AO_SUCCESS) _handleShelterSuccess(ev);
  // Apply scrap delta for ALL actions (SCAV gives +1, SHELTER spends -1/-2)
  if (ev.sd !== undefined && ev.sd !== 0)
    players[ev.pid].inv[4] = Math.max(0, (players[ev.pid].inv[4] ?? 0) + ev.sd);
  if (ev.pid === myId) _narrateActResult(ev, actNm);
  // TREAT reports the post-roll wound counts; keep the char sheet in step.
  if (ev.wnd) players[ev.pid].wnd = ev.wnd;
  if (ev.md) players[ev.pid].inv[3] = Math.max(0, (players[ev.pid].inv[3] ?? 0) + ev.md);
  if (ev.a === ACT_REST && ev.pid === myId) restSent = false;
  updateSidebar();
}

function _evSurv(ev) {
  // SURVEY result: update gameMap and mark cells as surveyed for rendering.
  try {
    if (ev.cells) {
      applyVisDisk(ev.cells);
      // Extract q,r coords and add to the surveyed set so the render loop
      // shows these cells as terrain instead of fog.
      for (let i = 0; i < ev.cells.length; i += 10) {
        const sq = Number.parseInt(ev.cells.substr(i,     2), 16);
        const sr = Number.parseInt(ev.cells.substr(i + 2, 2), 16);
        surveyedCells.add(`${sq}_${sr}`);
      }
    }
  } catch(e) { console.error('[SURV] Event processing error:', e); }
}

function _evDusk(ev) {
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
    if (pass) showToast('☢ You outlast the rads. Your skin stops crawling.');
    else      showToast('☢ The radiation wins. You bleed from places you forgot you had.');
    updateSidebar();
  }
}

function _evTrdOff(ev) {
  // Trade offer sent: log for everyone; show overlay only for the target
  const fromName = (ev.from >= 0 && ev.from < MAX_PLAYERS && players[ev.from]?.nm) || 'P' + ev.from;
  const toName   = (ev.to   >= 0 && ev.to   < MAX_PLAYERS && players[ev.to]?.nm)   || 'P' + ev.to;
  const giveTxt  = ev.give.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join(' ') || '\u2014';
  const wantTxt  = ev.want.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join(' ') || '\u2014';
  addLog(`<span class="log-col">\u21C4 ${escHtml(fromName)} offers ${escHtml(toName)}: ${giveTxt} for ${wantTxt}</span>`);
  if (ev.to === myId) {
    globalThis._openTradeOffer?.(ev.from, ev.give, ev.want);
  }
}

function _evTrdRes(ev) {
  // Trade result: 1=accepted 2=declined 3=expired 4=failed (accepted but conditions no longer held)
  const fromName = (ev.from >= 0 && ev.from < MAX_PLAYERS && players[ev.from]?.nm) || 'P' + ev.from;
  const toName   = ev.to === CARAVAN_PID ? 'Caravan'
    : (ev.to >= 0 && ev.to < MAX_PLAYERS && players[ev.to]?.nm) || 'P' + ev.to;
  // A caravan purchase (car_buy) names the goods — say what changed hands
  // instead of the generic ACCEPTED line. The buyer's own pack/token update
  // arrives separately as the targeted item_result (act:'buy').
  if (ev.res === 1 && ev.to === CARAVAN_PID && ev.item) {
    const name   = getItemById?.(ev.item)?.name ?? `Item #${ev.item}`;
    const qtyTxt = ev.n > 1 ? `${ev.n}× ` : '';
    addLog(`<span class="log-col">⇄ ${escHtml(fromName)} bought ${qtyTxt}${escHtml(name)} from the Caravan</span>`);
    if (ev.from === myId) showToast(`⇄ ${qtyTxt}${name} is yours. The caravan pockets your tokens.`);
    return;
  }
  const TRADE_LABELS = ['', 'ACCEPTED', 'DECLINED', 'EXPIRED', 'FAILED'];
  const label = TRADE_LABELS[ev.res] ?? 'UNKNOWN';
  const cls   = ev.res === 1 ? 'log-col' : 'log-check-fail';
  addLog(`<span class="${cls}">\u21C4 Trade ${label}: ${escHtml(fromName)} \u2194 ${escHtml(toName)}</span>`);
  if (ev.from === myId || ev.to === myId) {
    globalThis._closeTradeOverlay?.();
    updateSidebar();
  }
}

function _evFireSpread(ev) {
  // amt=0 always means "just burned out" (see world-system.hpp's
  // onFireExtinguished()) — the server already converted the hex to Ash
  // Dunes(1), so mirror that locally the same way the quake event's
  // `converted` list flips Settlement to Open Scrub.
  if (ev.intensity === 0) {
    if (gameMap[ev.r]?.[ev.q]) gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], terrain: 1 };
    return;
  }
  // The fire overlay itself is driven by the next world/state sync's
  // world.fire list (fireField.setFromSync) — this event is just the
  // vision-culled "a hex caught fire nearby" notice.
}

function _evFireDamage(ev) {
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const struck = ev.intensity === 10;  // sentinel: direct lightning strike, not standing-in-fire
  addLog(`<span class="log-check-fail">🔥 ${escHtml(who)} ${struck ? 'struck by lightning' : `burned (fire ${ev.intensity})`}</span>`);
  if (ev.pid === myId) {
    showToast(struck ? '⚡ Lightning strikes home. Your ears ring.'
              : ev.intensity >= 3 ? '🔥 The inferno sears you. Get clear.'
              : '🔥 Flames lick at your skin.');
    updateSidebar();
  }
}

function _evFloodWashout(ev) {
  // ev.intensity is NOT a flood intensity level here — it's the terrain id
  // the hex just became: 3 (Marsh, a swamped edge pushed under by the
  // advancing flood front) or 5 (Flooded District, a swamp that's stayed
  // under long enough to fully drown). See world-system.hpp's spreadFlood()
  // two-stage progression. No dedicated flood effect needed beyond this —
  // the terrain art itself (already rendered generically by whatever
  // gameMap[...].terrain is) tells the story, same as the storm's existing
  // rain/lightning already selling "it's flooding right now".
  if (gameMap[ev.r]?.[ev.q]) gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], terrain: ev.intensity };
}

function _evFloodDamage(ev) {
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  // llLost arrives from firmware/mock; older builds omit it, so the log
  // degrades to the bare sweep rather than printing "-undefined LL".
  const ll = ev.llLost > 0 ? ` (−${ev.llLost} LL)` : '';
  addLog(`<span class="log-check-fail">🌊 ${escHtml(who)} swept off their feet by a flash flood${ll}</span>`);
  if (ev.pid === myId) {
    showToast('🌊 The ground gives way — you\'re swept downstream.');
    updateSidebar();
  }
}

function _evDoomWarn(ev) {
  // 51-75 awareness, adjacent — a world-level dread notice, sent to everyone
  // regardless of position (see world-system-spec.md), not just the named pid.
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  addLog(`<span class="log-check-fail">☠ ${escHtml(who)} senses something close</span>`);
  if (ev.pid === myId) showToast('☠ A cold dread creeps in. Something is near.');
}

function _evDoomAct(ev) {
  // 76-99: a resource node on pid's hex was destroyed. 100: that, plus llLost=1.
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  addLog(`<span class="log-check-fail">☠ ${escHtml(who)}'s supplies rot away${ev.llLost ? ' — and worse' : ''}</span>`);
  if (ev.pid === myId) {
    showToast(ev.llLost ? '☠ It touches you. Something inside you dies a little.'
                         : '☠ Your supplies crumble to dust before your eyes.');
    updateSidebar();
  }
}

// The Doom addressing someone directly. `tier` picks the row of DOOM_TAUNTS
// and `idx` the line within it (reduced mod the row length client-side, so
// the firmware never needs to know how many lines exist) — see
// tickDoomTaunts() in world-system.hpp.
//
// Everyone gets the log line, only the addressed player gets the toast: the
// party seeing that it has singled someone out is most of the effect, but a
// toast in the Doom's voice should only interrupt the person it's about.
function _evDoomTaunt(ev) {
  const line = doomTauntLine(ev.tier, ev.idx);
  const who  = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const cls  = ev.tier === 0 ? 'log-check-ok' : 'log-check-fail';
  addLog(`<span class="${cls}">☠ ${escHtml(who)}: “${escHtml(line)}”</span>`);
  if (ev.pid === myId) showToast(line, 'doom');
}

// Second thoughts in a bare corridor. `idx` is a raw byte the server does not
// interpret, reduced mod TUNNEL_TAUNTS here -- see tickTunnelTaunts() in
// tunnels.hpp.
//
// Unlike the Doom's taunt this arrives unicast, so there is no "is it me"
// branch: if it got here, it is about us. It is deliberately quieter than
// the Doom, too -- a log line and a toast in its own voice, no banner. The
// banner is reserved for things that actually took a Life Level off you
// (showBadAirWarning), and the whole point of this voice is that it is
// commenting on a decision that mostly turned out fine.
function _evTunTaunt(ev) {
  const line = tunnelTauntLine(ev.idx);
  addLog(`<span class="log-tunnel-taunt">\u25BE ${escHtml(line)}</span>`);
  showToast(line, 'tunnel');
}

function _evCarAvail(ev) {
  // Edge-triggered nudge from the server (see world-system-spec.md's proximity
  // debounce) — just a one-time prompt. The player can still open trade with
  // the caravan any time they're co-located; see buildTradeTargetList().
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  addLog(`<span class="log-col">⇄ ${escHtml(who)} met the caravan</span>`);
  if (ev.pid === myId) showToast('⇄ A caravan rolls up. Trade available.');
}

// Server ack for `drop_res` — the char sheet's inventory boxes dumping resource
// tokens. The hex is updated by the 'rsp' event the server broadcasts next to
// this ack (same event the respawn tick uses), so all this does is take the
// authoritative token/score totals and say where the tokens went.
function _evResResult(ev) {
  console.log('%c[INV] _evResResult', 'color:#fc0;font-weight:bold',
              `ok=${ev.ok} pid=${ev.pid} res=${ev.res} qty=${ev.qty} grd=${ev.grd} rem=${ev.rem}`, ev);
  if (ev.pid !== undefined && ev.pid >= 0 && ev.pid < MAX_PLAYERS) {
    const p = players[ev.pid];
    if (ev.inv) p.inv = ev.inv;
    if (ev.sc !== undefined) p.sc = ev.sc;
  }
  if (!ev.ok) {
    console.warn('[INV] drop_res rejected by server', `res=${ev.res} pid=${ev.pid}`);
    showToast('Nothing to abandon.');
  } else if (ev.pid === myId) {
    const name = RES_NAMES[ev.res] ?? '?';
    if (ev.grd) {
      addLog(`<span class="log-col">\u25BC You set down ${ev.qty}\u00d7 ${name} \u2014 ${ev.rem}\u00d7 on this hex now.</span>`);
      showToast(`\u25BC Dropped ${ev.qty}\u00d7 ${name} \u2014 still here if you want it back.`);
    } else {
      addLog(`<span class="log-col">\u25BC You dumped ${ev.qty}\u00d7 ${name} into the dust \u2014 gone for good.</span>`);
      showToast(`\u25BC Dumped ${ev.qty}\u00d7 ${name} \u2014 this hex was already spoken for.`);
    }
  }
  updateSidebar();
  _packFullRearmCheck();   // the pack just shrank — re-arm the pack-full notice
}

function _evItemResult(ev) {
  console.log('%c[INV] _evItemResult', 'color:#fc0;font-weight:bold', `act=${ev.act} ok=${ev.ok} pid=${ev.pid} efxp=${ev.efxp ?? 'none'}`, ev);
  // Server ack for use_item / equip_item / unequip_item
  if (ev.ok) {
    // Dispatch client-side narrative effects
    if (ev.act === 'use' && ev.efxp) handleNarrativeEffect(ev.efxp);
  } else {
    console.warn('[INV] item action rejected by server', `act=${ev.act} pid=${ev.pid}`);
  }
  // Always apply server ground-truth state (server sends current state regardless of ok/fail)
  if (ev.pid !== undefined && ev.pid >= 0 && ev.pid < MAX_PLAYERS) {
    const p = players[ev.pid];
    if (ev.it) { console.log('[INV] applying server inv state', ev.it); p.it = ev.it; }
    if (ev.iq) p.iq = ev.iq;
    if (ev.eq) { console.log('[INV] applying server eq state', ev.eq); p.eq = ev.eq; }
    // Equipping a Backpack changes the pack size and equipping a Dent Absorber
    // changes the LL ceiling — both in the same message that reports the equip,
    // so the grid and the LIFE track move immediately instead of at next sync.
    if (ev.is    !== undefined) p.is    = ev.is;
    if (ev.llCap !== undefined) { p.llCap = ev.llCap; if (ev.pid === myId) uiLLCap.val = ev.llCap; }
    // CRAFT and a caravan BUY are the item actions that also spend resource
    // tokens (inv[]), unlike use/equip/unequip/drop which only touch
    // it[]/iq[]/eq[]. (The buy's log line comes from the trd_res broadcast.)
    if (ev.act === 'craft' || ev.act === 'buy') {
      if (ev.inv) p.inv = ev.inv;
      if (ev.kr !== undefined) p.kr = ev.kr;
      if (ev.pid === myId) {
        if (ev.act === 'craft') {
          const outName = getItemById?.(getRecipeById(ev.recipe)?.outputItem)?.name ?? 'something';
          addLog?.(`<span class="log-col">⚒ Crafted ${escHtml ? escHtml(outName) : outName}.</span>`);
        }
        updateSidebar?.();
      }
    }
  }
  // Always refresh char-sheet inventory/equipment — ghost-tap on mobile can close
  // char-overlay between item-menu dismiss and server ack, causing the open-check to fail
  renderInventory?.();
  renderEquipment?.();
}

function _evEncRes(ev) {
  const who = players[ev.pid]?.nm || `P${ev.pid}`;
  const outClass = ev.out ? 'log-col' : 'log-check-fail';
  addLog(`<span class="${outClass}">\u2299 ${escHtml(who)}: ${ev.out ? 'through.' : 'setback.'}</span>`);
  if (ev.pid === myId) {
    const p = players[myId];
    if (p) {
      if (ev.penLL)  p.ll  = Math.max(0, (p.ll  ?? 0) + ev.penLL);
      if (ev.penRad) p.rad = Math.max(0, (p.rad ?? 0) + ev.penRad);
      // Hazard resource losses — server sends what it actually took.
      if (Array.isArray(ev.penRes)) {
        ev.penRes.forEach((v, ri) => {
          if (v > 0) p.inv[ri] = Math.max(0, (p.inv?.[ri] ?? 0) - v);
        });
      }
    }
    updateSidebar();
    globalThis._onEncResult?.(ev);
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
        showToast(`◉ You press supplies into ${escHtml(ldrName)}'s hand. The debt is shared.`);
        updateSidebar();
      }
    });
  }
}

function _evEncBank(ev) {
  const who = players[ev.pid]?.nm || `P${ev.pid}`;
  const lootSum = (ev.loot ?? []).reduce((a, b) => a + b, 0);
  addLog(`<span class="log-col">\u25a0 ${escHtml(who)} secured loot (${lootSum} items, +${ev.scoreD} pts)</span>`);
  if (ev.pid === myId) {
    if (Array.isArray(ev.loot)) {
      ev.loot.forEach((v, i) => {
        if (i >= 0 && i < 5 && v > 0) players[myId].inv[i] = (players[myId].inv[i] ?? 0) + v;
      });
    }
    // recs is a bitmask — a single scene can walk through several nodes,
    // each granting a different recipe, before ever banking.
    if (ev.recs) {
      players[myId].kr = (players[myId].kr ?? 0) | ev.recs;
      for (let rid = 1; rid <= 32; rid++) {
        if (!((ev.recs >>> (rid - 1)) & 1)) continue;
        const rname = getRecipeById(rid)?.name ?? `recipe ${rid}`;
        addLog(`<span class="log-col">⚒ You learned how to make ${escHtml(rname)}!</span>`);
        showToast(`⚒ Recipe learned: ${rname}`);
      }
    }
    showToast(`★ You drag the spoils from the ruin. +${ev.scoreD}`);
    globalThis._onEncBank?.(ev);
    updateSidebar();
  }
}

function _evEncEnd(ev) {
  const who = players[ev.pid]?.nm || `P${ev.pid}`;
  const ENC_REASON_LABELS = { hazard: 'hazard', abort: 'aborted', dawn: 'dawn', downed: 'downed', disconnect: 'disconnected', regen: 'world remade' };
  const reasonTxt = ENC_REASON_LABELS[ev.reason] ?? ev.reason;
  addLog(`<span class="log-check-fail">\u25a0 ${escHtml(who)} encounter ended (${reasonTxt})</span>`);
  if (ev.pid === myId) {
    const ENC_END_FLAVOR = {
      hazard:     '☠ The place turns on you. You break off, bloodied.',
      abort:      '☠ You back out of the ruin. Better alive than brave.',
      dawn:       '☀ Dawn finds you still rummaging. The moment is gone.',
      downed:     '☠ You fall where you stand. The ruin keeps its secrets.',
      disconnect: '☠ The thread snaps. The scene dissolves around you.',
      regen:      '☠ The ground shifts. The place you were exploring no longer exists.',
    };
    showToast(ENC_END_FLAVOR[ev.reason] ?? `☠ The scene closes: ${reasonTxt}.`);
    globalThis._onEncEnd?.(ev);
    updateSidebar();
  }
}

// ── Bunker tunnels ───────────────────────────────────────────────
// q/r are the SURFACE hatch in both events, so a player still on the surface
// can mark where a teammate went down without ever seeing the tunnel board.
function _evTunIn(ev) {
  const p = players[ev.pid];
  if (p) { p.dp = 1; p.q = ev.q; p.r = ev.r; p.hatchIdx = ev.hatch; }
  if (ev.mp !== undefined && p) p.mp = ev.mp;
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  addLog(`<span class="log-mv">▼ ${escHtml(who)} climbed down into the bunker tunnels</span>`);
  if (ev.pid === myId) {
    updateSidebar();
    showToast('▼ You descend. The air is stale and the dark closes in.');
  }
}

function _evTunOut(ev) {
  const p = players[ev.pid];
  if (p) {
    p.dp = 0; p.q = ev.q; p.r = ev.r; p.hatchIdx = ev.hatch;
    // Surfacing somewhere else entirely is the whole point of the network, so
    // never lerp it -- snap, or the marker slides across the whole map.
    renderPos[ev.pid].q = ev.q;
    renderPos[ev.pid].r = ev.r;
  }
  if (ev.mp !== undefined && p) p.mp = ev.mp;
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  addLog(`<span class="log-mv">▲ ${escHtml(who)} climbed out of the tunnels</span>`);
  if (ev.pid === myId) {
    setMyDepth(0);
    surveyedCells.clear();
    updateSidebar();
    updateTerrainCard();
    updateDirButtons();
    showToast('▲ You haul yourself into daylight.');
  }
}

// A cave-in. amt is the resulting terrain id (15, Collapsed Tunnel) -- the same
// "intensity carries a terrain id" idiom the flash flood uses for washouts.
function _evTunCollapse(ev) {
  if (tunnelMap[ev.r]?.[ev.q])
    tunnelMap[ev.r][ev.q] = { ...tunnelMap[ev.r][ev.q], terrain: ev.amt ?? 15 };
  if (myDepth) {
    addLog('<span class="log-hazard">⚠ The roof gives way somewhere close by</span>');
    updateDirButtons();
  }
}

// ── Event handler ────────────────────────────────────────────────────────────

function handleEvent(ev) {
  switch (ev.k) {
    case 'col':         _evCol(ev);        break;
    case 'col_fail':    _evColFail(ev);    break;
    case 'rsp':
      if (gameMap[ev.r]?.[ev.q])
        gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], resource: ev.res, amount: ev.amt };
      collectedCells.delete(`${ev.q}_${ev.r}`);
      break;
    case 'mv':          _evMv(ev);         break;
    case 'tun_in':      _evTunIn(ev);      break;
    case 'tun_out':     _evTunOut(ev);     break;
    case 'tun_collapse':_evTunCollapse(ev);break;
    case 'join':
      players[ev.pid].on = true;
      // Snap renderPos so the new player doesn't lerp from (0,0) to their hex (Bug-4)
      if (ev.q === undefined) { renderPos[ev.pid].q = players[ev.pid].q; renderPos[ev.pid].r = players[ev.pid].r; }
      else { renderPos[ev.pid].q = ev.q; renderPos[ev.pid].r = ev.r; }
      addLog(`<span class="log-join">&#x25B6; Survivor ${ev.pid} appeared</span>`);
      break;
    case 'regen':
      showToast('☠ The wasteland reshapes itself. Every bearing is lost.');
      addLog('<span class="log-mv">☠ The world has been remade. Find your bearings, survivor.</span>');
      break;
    case 'downed':      _evDowned(ev);     break;
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
    case 'chk':         _evChk(ev);        break;
    case 'dawn':        _evDawn(ev);       break;
    case 'act':         _evAct(ev);        break;
    case 'surv':        _evSurv(ev);       break;
    case 'dusk':        _evDusk(ev);       break;
    case 'trd_off':     _evTrdOff(ev);     break;
    case 'trd_res':     _evTrdRes(ev);     break;
    case 'car_avail':   _evCarAvail(ev);   break;
    case 'doom_warn':   _evDoomWarn(ev);   break;
    case 'doom_act':    _evDoomAct(ev);    break;
    case 'doom_taunt':  _evDoomTaunt(ev);  break;
    case 'tun_taunt':   _evTunTaunt(ev);   break;
    case 'fire_spread':   _evFireSpread(ev);   break;
    case 'fire_dmg':      _evFireDamage(ev);   break;
    case 'flood_washout': _evFloodWashout(ev); break;
    case 'flood_dmg':     _evFloodDamage(ev);  break;
    case 'item_result': _evItemResult(ev); break;
    case 'enc_start': {
      // POI consumed — clear it from local map so eye disappears immediately
      if (gameMap[ev.r]?.[ev.q])
        gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], poi: 0 };
      const encWho = players[ev.pid]?.nm || `P${ev.pid}`;
      addLog(`<span class="log-col">\u2299 ${escHtml(encWho)} enters encounter (${ev.q},${ev.r})</span>`);
      updateSidebar();
      break;
    }
    case 'enc_res':     _evEncRes(ev);     break;
    case 'enc_bank':    _evEncBank(ev);    break;
    case 'enc_end':     _evEncEnd(ev);     break;
    case 'weather':
      weatherPhase = ev.phase ?? 0;
      updateWeatherHUD();
      addLog(`<span class="log-mv">Weather: ${WEATHER_PHASE_NAMES?.[ev.phase] ?? ev.phase}</span>`);
      break;
    case 'quake': {
      // Server picks the fault line, any shelters it destroys, and any
      // Settlement it levels to Open Scrub, so every client shakes the same
      // hexes and sees the same losses — this just mirrors that into the
      // local map and hands the line (plus which cells were leveled, for
      // the extra dust) to the renderer.
      for (const c of ev.destroyed ?? []) {
        if (gameMap[c.r]?.[c.q]) gameMap[c.r][c.q] = { ...gameMap[c.r][c.q], shelter: 0 };
      }
      for (const c of ev.converted ?? []) {
        if (gameMap[c.r]?.[c.q]) gameMap[c.r][c.q] = { ...gameMap[c.r][c.q], terrain: 0 };
      }
      quakeField.spawnFromEvent(ev.cells ?? [], ev.converted ?? []);
      // Briefly override the HUD weather label with "EARTHQUAKE" for the
      // duration of the shake, then let it fall back to the real weather.
      quakeHudUntil = Date.now() + QUAKE_SHAKE_MS + QUAKE_FADE_MS;
      updateWeatherHUD();
      setTimeout(updateWeatherHUD, QUAKE_SHAKE_MS + QUAKE_FADE_MS);
      if (ev.destroyed?.length) {
        const n = ev.destroyed.length;
        addLog(`<span class="log-check-fail">▪ The ground splits — ${n} shelter${n > 1 ? 's' : ''} destroyed.</span>`);
        showToast('☠ The earth splits. A shelter collapses into the fault line.');
      }
      if (ev.converted?.length) {
        const n = ev.converted.length;
        addLog(`<span class="log-check-fail">▪ ${n} settlement${n > 1 ? 's' : ''} leveled to open scrub.</span>`);
        showToast('☠ The quake swallows a settlement whole. Nothing left but scrub.');
      }
      break;
    }
    case 'settle': {
      // Server founded a Settlement — either 3 survivors shared the hex an
      // improved shelter just finished on, or that shelter completed a
      // triangle of 3 mutually-adjacent improved shelters. `removed` lists
      // every hex whose shelter was cleared (1 or 3); (ev.q, ev.r) is the
      // one hex that becomes Settlement (terrain 9).
      for (const c of ev.removed ?? []) {
        if (gameMap[c.r]?.[c.q]) gameMap[c.r][c.q] = { ...gameMap[c.r][c.q], shelter: 0 };
      }
      if (gameMap[ev.r]?.[ev.q]) gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], terrain: 9 };
      addLog(`<span class="log-check-ok">⌂ A settlement rises at (${ev.q},${ev.r})!</span>`);
      showToast('⌂ Survivors raise a settlement from the wasteland.');
      break;
    }
  }
}
