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

  return { onSend, onDropped, onMsg, onConnect, onDisconnect, onError, report, wsState };
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
  socket.onclose   = (ev) => {
    const msSinceEnc = globalThis._lastEncStartT ? (Date.now() - globalThis._lastEncStartT) : '—';
    console.warn('%c[WS ▼] Closed', 'color:#fa0;font-weight:bold', `code=${ev.code} wasClean=${ev.wasClean} reason="${ev.reason}" msSinceEncStart=${msSinceEnc}`);
    showToast(CONN_LOST_QUIPS[Math.floor(Math.random() * CONN_LOST_QUIPS.length)]);
    Diag.onDisconnect(); setStatus('Reconnecting...'); setTimeout(connect, RECONNECT_DELAY_MS);
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
  parseMapFog(msg.map);
  if (!Array.isArray(msg.p)) return;
  msg.p.forEach(p => {
    if (typeof p.id !== 'number' || p.id < 0 || p.id >= MAX_PLAYERS) return;
    Object.assign(players[p.id], p);
    players[p.id].rest = !!p.rt;  // map rt → rest (mirrors 's' handler)
    if (p.on) { renderPos[p.id].q = p.q; renderPos[p.id].r = p.r; }
  });
  if (msg.vc) loadTerrainVariants(msg.vc);
  if (msg.sv) loadShelterVariants(msg.sv);
  if (msg.fa) loadForrageAnimalImgs(msg.fa);
  if (msg.gs) _applyGameState(msg.gs);
  if (Array.isArray(msg.gi)) groundItems = msg.gi;
  hideConnectOverlay();
  if (myId >= 0) hideCharSelect();  // belt-and-suspenders: hide picker if sync arrives before/without asgn
  if (myId >= 0) uiResting.val = !!players[myId].rest;  // sync resting state on reconnect
  updateSidebar();
  if (myId >= 0 && players[myId]?.mp > 0) { maxMP = players[myId].mp; uiMaxMP.val = maxMP; }
  if (myId >= 0) displayMP = players[myId].mp ?? 6;
  updateTerrainCard();
  updateDirButtons();
  _checkDownedState();
  // Re-render char-sheet if open — ensures wounds/status reflect fresh server state (BUG-10)
  if (document.getElementById('char-overlay')?.classList.contains('open')) {
    renderInventory?.();
    renderEquipment?.();
  }
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
  if (msg.gs) _applyGameState(msg.gs);
  updateSidebar();
  updateTerrainCard();
  updateDirButtons();
  _checkDownedState();
}

function _handleSelfVis() {
  const _me = players[myId];
  const _cell = gameMap[_me.r]?.[_me.q];
  if (_cell) {
    const _mc = TERRAIN[_cell.terrain]?.mc;
    if (_mc && _mc !== 255) moveCooldownMs = MOVE_COOLDOWN_BASE_MS * _mc;
  }
  // If the current hex still has a resource after the move, collection was
  // blocked. The only server-side reason is a full inventory — notify the player.
  const _cur = gameMap[_me.r]?.[_me.q];
  if (_cur?.resource > 0 && _cur?.amount > 0) {
    showToast('Carry limit reached — drop or use items to collect resources.');
  }
  // Auto-trigger encounter: vis fires after applyVisDisk so gameMap is guaranteed fresh.
  if (_cur?.poi) {
    globalThis._lastEncStartT = Date.now();
    console.log('%c[ENC] POI detected — sending enc_start', 'color:#c0f;font-weight:bold', `q=${_me.q} r=${_me.r} t=${globalThis._lastEncStartT}`);
    send({ t: 'enc_start', q: _me.q, r: _me.r });
  }
}

function _msgVis(msg) {
  if (msg.vr !== undefined) myVisionR = msg.vr;
  if (msg.q !== undefined && myId >= 0) {
    players[myId].q = msg.q;
    players[myId].r = msg.r;
  }
  applyVisDisk(msg.cells);
  if (myId >= 0) _handleSelfVis();
  updateTerrainCard();
  updateSidebar();
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
  }
}

// ── Message handler ──────────────────────────────────────────────────────────

function handleMsg(msg) {
  if (!msg?.t) {
    console.warn('[RX] Message missing type field', msg);
    return;
  }
  console.log('%c← RX [%s]', 'color:#0cf;font-weight:bold', msg.t, msg);

  switch (msg.t) {
    case 'asgn':          _msgAsgn(msg);         break;
    case 'lobby':         _msgLobby(msg);        break;
    case 'sync':          _msgSync(msg);         break;
    case 's':             _msgState(msg);        break;
    case 'vis':           _msgVis(msg);          break;
    case 'ev':            handleEvent(msg);      break;
    case 'ground_update': _msgGroundUpdate(msg); break;
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
      break;
  }
  buildAgentState();
}

// ── handleEvent sub-handlers ─────────────────────────────────────────────────

function _evCol(ev) {
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
    updateSidebar();
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
  pm.q = ev.q; pm.r = ev.r;
  // Surveyed cells are relative to a fixed vantage point — clear on any move.
  if (ev.pid === myId) surveyedCells.clear();
  // Stamp footprint immediately — vis-disk only goes to the moving player,
  // so other players would never see footprints without this client-side update.
  if (gameMap[ev.r]?.[ev.q]) {
    const c = gameMap[ev.r][ev.q];
    gameMap[ev.r][ev.q] = { ...c, footprints: (c.footprints || 0) | (1 << ev.pid) };
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
  if (ev.pid === myId) { maxMP = ev.mp; uiMaxMP.val = ev.mp; displayMP = ev.mp; nightFade = NIGHT_FADE_INIT; }
  p.rest = false;
  p.au   = 0;
  if (ev.pid === myId) {
    _tickNarrativeEffects();
    if (uiResting.val && ev.expd < 0) showShelterWarning();
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
  if (ev.dn)     d += ` DN${ev.dn}=${ev.tot}`;
  if (ev.fd)     d += ` +${ev.fd}Food`;
  if (ev.wd)     d += ` +${ev.wd}Water`;
  if (ev.lld)    d += ` ${ev.lld > 0 ? '+' : ''}${ev.lld}LL`;
  if (ev.radd)   d += ` ${ev.radd > 0 ? '+' : ''}${ev.radd}R`;
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
  const actNames = ['Forage','Water','','Scavenge','Shelter','Treat','Survey','Rest'];
  const outNames = ['','Success','Partial','Failed'];
  const pts = ev.scoreD ? ` +${ev.scoreD}pts.` : '';
  narrateState(`${actNames[ev.a] ?? 'Action'} — ${outNames[ev.out] ?? ev.out}. MP:${ev.mp}.${pts}`);
}

function _trackActSlot(ev) {
  if (ev.out === AO_BLOCKED) return;
  const meIsScout = (players[ev.pid]?.arch ?? -1) === 4;
  if (!(ev.a === ACT_SURVEY && meIsScout)) players[ev.pid].au = 1;
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
  _trackActSlot(ev);
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
  // Trade result: 1=accepted 2=declined 3=expired
  const fromName = (ev.from >= 0 && ev.from < MAX_PLAYERS && players[ev.from]?.nm) || 'P' + ev.from;
  const toName   = (ev.to   >= 0 && ev.to   < MAX_PLAYERS && players[ev.to]?.nm)   || 'P' + ev.to;
  const TRADE_LABELS = ['', 'ACCEPTED', 'DECLINED', 'EXPIRED'];
  const label = TRADE_LABELS[ev.res] ?? 'UNKNOWN';
  const cls   = ev.res === 1 ? 'log-col' : 'log-check-fail';
  addLog(`<span class="${cls}">\u21C4 Trade ${label}: ${escHtml(fromName)} \u2194 ${escHtml(toName)}</span>`);
  if (ev.from === myId || ev.to === myId) {
    globalThis._closeTradeOverlay?.();
    updateSidebar();
  }
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
    showToast(`★ You drag the spoils from the ruin. +${ev.scoreD}`);
    globalThis._onEncBank?.(ev);
    updateSidebar();
  }
}

function _evEncEnd(ev) {
  const who = players[ev.pid]?.nm || `P${ev.pid}`;
  const ENC_REASON_LABELS = { hazard: 'hazard', abort: 'aborted', dawn: 'dawn', downed: 'downed', disconnect: 'disconnected' };
  const reasonTxt = ENC_REASON_LABELS[ev.reason] ?? ev.reason;
  addLog(`<span class="log-check-fail">\u25a0 ${escHtml(who)} encounter ended (${reasonTxt})</span>`);
  if (ev.pid === myId) {
    const ENC_END_FLAVOR = {
      hazard:     '☠ The place turns on you. You break off, bloodied.',
      abort:      '☠ You back out of the ruin. Better alive than brave.',
      dawn:       '☀ Dawn finds you still rummaging. The moment is gone.',
      downed:     '☠ You fall where you stand. The ruin keeps its secrets.',
      disconnect: '☠ The thread snaps. The scene dissolves around you.',
    };
    showToast(ENC_END_FLAVOR[ev.reason] ?? `☠ The scene closes: ${reasonTxt}.`);
    globalThis._onEncEnd?.(ev);
    updateSidebar();
  }
}

// ── Event handler ────────────────────────────────────────────────────────────

function handleEvent(ev) {
  switch (ev.k) {
    case 'col':         _evCol(ev);        break;
    case 'rsp':
      if (gameMap[ev.r]?.[ev.q])
        gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], resource: ev.res, amount: ev.amt };
      collectedCells.delete(`${ev.q}_${ev.r}`);
      break;
    case 'mv':          _evMv(ev);         break;
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
  }
}
