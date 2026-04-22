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

  return { onSend, onDropped, onMsg, onConnect, onDisconnect, onError, report, wsState };
})();

// ── Move cooldown tracking (client-side estimate) ─────────────────
let lastMoveSent   = 0;   // Date.now() of last move sent
let moveCooldownMs = MOVE_COOLDOWN_BASE_MS; // mirrors server MOVE_CD_MS × current terrain MC
let restSent       = false; // guard against REST double-click before server ack

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
  socket.onclose   = (ev) => {
    const msSinceEnc = window._lastEncStartT ? (Date.now() - window._lastEncStartT) : '—';
    console.warn(`[DIAG] WS closed  code=${ev.code}  wasClean=${ev.wasClean}  reason="${ev.reason}"  msSinceEncStart=${msSinceEnc}`);
    Diag.onDisconnect(); setStatus('Reconnecting...'); setTimeout(connect, RECONNECT_DELAY_MS);
  };
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
          window._lastEncStartT = Date.now();
          console.log(`[ENC] sending enc_start q=${_me.q} r=${_me.r} t=${window._lastEncStartT}  wsState=${socket?.readyState ?? '?'}  buffered=${socket?.bufferedAmount ?? '?'}`);
          send({ t: 'enc_start', q: _me.q, r: _me.r });
          console.log(`[ENC] enc_start send() returned  wsState=${socket?.readyState ?? '?'}  buffered=${socket?.bufferedAmount ?? '?'}`);
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
      console.log(`[ENC] enc_path recv biome=${msg.biome} id=${msg.id} t=${Date.now()}  msSinceEncStart=${window._lastEncStartT ? Date.now()-window._lastEncStartT : '—'}  wsState=${Diag.wsState()}`);
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
