// ── Vision radius (updated per vis/sync message from server) ─────
let myVisionR = VISION_R;

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
const uiLL          = van.state(6);
const uiFood        = van.state(6);
const uiWater       = van.state(6);
const uiResolve     = van.state(3);
const uiMP          = van.state(6);
// §6.2 Radiation track
const uiRad         = van.state(0);
// §6.4 Exposure markers
const uiColdExp     = van.state(0);
const uiHeatExp     = van.state(0);
// §5 Action tracking
const uiActUsed     = van.state(false);
const uiResting     = van.state(false);  // true after REST until dawn
let maxMP = 6;  // set from dawnMP at EVT_DAWN; personal day-length for time-of-day clock
// Menu navigation (null=closed, 'main'|'howto'|'settings'|'about')
const uiMenuPage    = van.state(null);
function openMenu(page = 'main') { uiMenuPage.val = page; }
function closeMenu()             { uiMenuPage.val = null; }
// Lobby / character selection
const lobbyAvail    = van.state([]);    // archetype indices currently available to pick
const uiPickPending = van.state(false); // true while waiting for server pick response
// Skill check panel
const uiPushAvail   = van.state(false);  // last check failed → Push eligible

// ── Move cooldown tracking (client-side estimate) ─────────────────
let lastMoveSent   = 0;   // Date.now() of last move sent
let moveCooldownMs = 220; // mirrors server MOVE_CD_MS × current terrain MC

// ── Smooth animation ─────────────────────────────────────────────
const renderPos = Array.from({ length: MAX_PLAYERS }, () => ({ q: 0.0, r: 0.0 }));

// ── Footprint timestamps ──────────────────────────────────────────
// key: 'q_r_pid' → Date.now() when that player last stepped on that hex.
// Used to drive the bright-red → black colour fade on canvas.
const footprintTimestamps = new Map();
const FOOTPRINT_FADE_MS   = 4000;  // ms to fully fade from red to near-black

// ── Surveyed cells ────────────────────────────────────────────────
// Cells revealed by the SURVEY action beyond normal vision radius.
// Rendered at reduced opacity as scouted-but-not-directly-visible.
// Cleared whenever the local player moves.
const surveyedCells = new Set(); // 'q_r' keys

// ── Terrain hex images ────────────────────────────────────────────
// Naming: /img/hex<Name><N>.png  (e.g. hexOpenScrub0.png, hexOpenScrub1.png)
// terrainImgVariants[terrain][variant] → Image object (or undefined if missing).
// Populated by loadTerrainVariants(vc) when the sync message arrives.
const TERRAIN_IMG_NAMES = [
  'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
  'BrokenUrban', 'FloodedDistrict', 'GlassFields',
  'Ridge', 'Mountain', 'Settlement', 'NukeCrater'
];
const terrainImgVariants = Array.from({ length: NUM_TERRAIN }, () => []);

function loadTerrainVariants(vc) {
  for (let t = 0; t < NUM_TERRAIN; t++) {
    const name  = TERRAIN_IMG_NAMES[t];
    const count = (vc && vc[t]) || 0;
    terrainImgVariants[t] = [];
    for (let v = 0; v < count; v++) {
      const img  = new Image();
      img.loaded = false;
      img.src    = '/img/hex' + name + v + '.png';
      img.onload  = () => { img.loaded = true; };
      img.onerror = () => { img.loaded = false; };
      terrainImgVariants[t][v] = img;
    }
  }
}

// ── State ───────────────────────────────────────────────────────
let myId = -1;
// gameMap[r][q] = { terrain, resource, amount } or null (fogged/unknown)
let gameMap = Array.from({ length: MAP_ROWS }, () => Array(MAP_COLS).fill(null));
let players = Array.from({ length: MAX_PLAYERS }, (_, i) => ({
  id: i, on: false, q: 0, r: 0, sc: 0, nm: `Survivor${i}`,
  inv: [0,0,0,0,0], st: 100, pr: 2, sr: 2, sp: 0,
  // Wayfarer fields
  ll: 6, food: 6, water: 6, fat: 0, rad: 0, res: 3,
  arch: 0, sb: 0, is: 8,
  sk: [0,0,0,0,0,0],
  wd: [0,0,0],
  it: Array(12).fill(0),
  iq: Array(12).fill(0),
  // §4 Resource economy
  fth: 0, wth: 0, mp: 6,
  // §5 Action tracking
  au: false, rest: false,
}));

// Shared game state (Threat Clock, Day, shared stores)
let gameState = { tc: 0, dc: 0, sf: 30, sw: 30 };

// ── Character selection overlay helpers ──────────────────────────
function showCharSelect() {
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

function connect() {
  socket = new WebSocket(wsUrl);
  socket.onopen    = () => {
    setStatus('Connected');
    serverHasWifiCreds = false;  // reset on each new connection
    // Auto-send saved WiFi credentials if server doesn't already have them
    // (server will send {t:'wifi',status:'saved'} if it does).
    setTimeout(() => {
      if (!serverHasWifiCreds) {
        const ssid = localStorage.getItem('wifi_ssid');
        if (ssid) send({ t: 'wifi', ssid, pass: localStorage.getItem('wifi_pass') ?? '' });
      }
    }, 300);  // brief delay to receive 'saved' message first if server has creds
  };
  socket.onclose   = () => { setStatus('Reconnecting...'); setTimeout(connect, 2000); };
  socket.onerror   = () => {};
  socket.onmessage = e => handleMsg(JSON.parse(e.data));
}
function send(obj) {
  if (socket && socket.readyState === WebSocket.OPEN)
    socket.send(JSON.stringify(obj));
}

// ── Rest bubbles & auto-rest ─────────────────────────────────────
function updateRestBubbles() {
  const bar = document.getElementById('rest-bubbles');
  if (!bar) return;
  bar.innerHTML = '';
  const connected = players.filter(p => p.on);
  if (connected.length === 0) { bar.style.display = 'none'; return; }
  bar.style.display = 'flex';
  for (const p of connected) {
    const bubble = document.createElement('div');
    bubble.className = 'rest-bubble' + (p.rest ? ' rest-bubble-on' : '');
    bubble.style.setProperty('--pc', PLAYER_COLORS[p.id]);
    const tag = (p.nm || `P${p.id}`).substring(0, 6).toUpperCase();
    bubble.innerHTML = p.rest
      ? `<span class="rb-zzz">Zzz</span><span class="rb-name">${tag}</span>`
      : `<span class="rb-name">${tag}</span>`;
    bar.appendChild(bubble);
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
function handleMsg(msg) {
  switch (msg.t) {
    case 'asgn':
      myId = msg.id;
      uiPickPending.val = false;
      hideCharSelect();
      break;

    case 'lobby':
      lobbyAvail.val    = msg.avail || [];
      uiPickPending.val = false;
      if (!pendingLobbyRedirect) showCharSelect();  // suppressed during downed-death pause
      break;

    case 'sync':
      if (msg.id  !== undefined) myId = msg.id;
      if (msg.vr  !== undefined) myVisionR = msg.vr;
      parseMapFog(msg.map);
      msg.p.forEach(p => {
        Object.assign(players[p.id], p);
        if (p.on) { renderPos[p.id].q = p.q; renderPos[p.id].r = p.r; }
      });
      if (msg.vc) loadTerrainVariants(msg.vc);
      if (msg.gs) Object.assign(gameState, msg.gs);
      hideConnectOverlay();
      updateSidebar();
      if (myId >= 0 && players[myId]?.mp > 0 && maxMP <= 0) maxMP = players[myId].mp;
      updateTerrainCard();
      updateRestBubbles();
      break;

    case 's':
      msg.p.forEach((pd, i) => {
        const p = players[i];
        p.on = pd.on; p.q = pd.q; p.r = pd.r; p.sc = pd.sc;
        p.inv = pd.inv; p.sp = pd.sp;
        p.ll = pd.ll; p.food = pd.food; p.water = pd.water;
        p.fat = pd.fat; p.rad = pd.rad; p.sb = pd.sb;
        if (pd.mp  !== undefined) p.mp  = pd.mp;
        if (pd.res !== undefined) p.res = pd.res;   // resolve (§4.4)
        if (pd.fth !== undefined) p.fth = pd.fth;   // F threshold bitmask
        if (pd.wth !== undefined) p.wth = pd.wth;   // W threshold bitmask
        if (pd.au  !== undefined) p.au  = !!pd.au;  // action used this day
      });
      if (msg.gs) Object.assign(gameState, msg.gs);
      updateSidebar();
      updateTerrainCard();
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
          if (_mc && _mc !== 255) moveCooldownMs = 220 * _mc;
        }
      }
      updateTerrainCard();
      updateSidebar();
      break;

    case 'ev':
      handleEvent(msg);
      break;

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
  }
}

function handleEvent(ev) {
  switch (ev.k) {
    case 'col': {
      const me = myId >= 0 ? players[myId] : null;
      if (me && hexDistWrap(me.q, me.r, ev.q, ev.r) <= myVisionR) {
        if (gameMap[ev.r] && gameMap[ev.r][ev.q])
          gameMap[ev.r][ev.q] = { ...gameMap[ev.r][ev.q], resource: 0, amount: 0 };
      }
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
      break;
    case 'mv': {
      const pm = players[ev.pid];
      pm.q = ev.q; pm.r = ev.r;
      // Surveyed cells are relative to a fixed vantage point — clear on any move.
      if (ev.pid === myId) surveyedCells.clear();
      // Stamp footprint immediately — vis-disk only goes to the moving player,
      // so other players would never see footprints without this client-side update.
      if (gameMap[ev.r]?.[ev.q]) {
        const c = gameMap[ev.r][ev.q];
        gameMap[ev.r][ev.q] = { ...c, footprints: (c.footprints || 0) | (1 << ev.pid) };
      }
      // Record timestamp for the red→black fade animation.
      footprintTimestamps.set(`${ev.q}_${ev.r}_${ev.pid}`, Date.now());
      if (ev.radd && ev.radd > 0) {
        pm.rad = ev.rad ?? pm.rad;
        if (ev.pid === myId) {
          uiRad.val = pm.rad;
          const radTxt = ev.radd > 0 ? `\u2622 +${ev.radd} Radiation (R:${pm.rad})` : '';
          if (radTxt) showToast(radTxt);
          addLog(`<span class="log-check-fail">\u2622 Entered rad zone +${ev.radd}R → R:${pm.rad}</span>`);
        }
      }
      break;
    }
    case 'join':
      players[ev.pid].on = true;
      addLog(`<span class="log-join">&#x25B6; Survivor ${ev.pid} appeared</span>`);
      updateRestBubbles();
      break;
    case 'downed': {
      // Server has reset our slot — show death message then redirect to char selection
      myId = -1;
      pendingLobbyRedirect = true;
      addLog('<span class="log-check-fail">☠ DOWNED — the wasteland claims you. Find shelter next time.</span>');
      showToast('☠ YOU HAVE BEEN DOWNED — re-selecting survivor...');
      setTimeout(() => {
        pendingLobbyRedirect = false;
        showCharSelect();
      }, 3500);
      break;
    }

    case 'left':
      players[ev.pid].on = false;
      players[ev.pid].rest = false;
      addLog(`<span class="log-mv">&#x25C4; Survivor ${ev.pid} gone dark</span>`);
      updateRestBubbles();
      checkAutoRest();
      break;
    case 'nm':
      players[ev.pid].nm = ev.nm;
      if (ev.pid !== myId) updateSidebar();
      break;
    case 'chk':
    case 'push': {
      const isPush = ev.k === 'push';
      const who    = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      const skNm   = SK_NAMES[ev.sk] ?? `SK${ev.sk}`;
      const modTxt = ev.mod !== 0 ? (ev.mod > 0 ? `+${ev.mod}` : `${ev.mod}`) : '';
      const icon   = ev.suc ? '\u25CF' : '\u25CB';
      const cls    = ev.suc ? 'log-check-ok' : 'log-check-fail';
      const pfx    = isPush ? '\u21BA ' : '';
      const sfx    = isPush ? ' <span class="log-fat-lbl">(+Fat)</span>' : '';
      addLog(`<span class="${cls}">${icon} ${escHtml(who)} ${pfx}${skNm}: ${ev.r1}+${ev.r2}+${ev.sv}${modTxt}=${ev.tot} vs DN${ev.dn}${sfx}</span>`);
      if (ev.pid === myId) {
        const canPush = !ev.suc && !isPush && (players[myId]?.fat ?? 0) < 6;
        uiPushAvail.val = canPush;
        document.getElementById('check-push-btn').classList.toggle('push-visible', canPush);
        showToast(`${pfx}${skNm}: ${ev.suc ? 'SUCCESS' : 'FAIL'}${canPush ? ' — push?' : ''}`);
      }
      break;
    }
    case 'nopush':
      if (ev.pid === myId) {
        uiPushAvail.val = false;
        showToast('Cannot push — fatigue full or no failed check');
      }
      break;
    case 'dawn': {
      // Update local player state from dawn event (all players, not just self)
      if (ev.pid >= 0 && ev.pid < MAX_PLAYERS) {
        players[ev.pid].food  = ev.f;
        players[ev.pid].water = ev.w;
        players[ev.pid].ll    = ev.ll;
        players[ev.pid].mp    = ev.mp;
        if (ev.pid === myId) maxMP = ev.mp;  // lock in this day's MP budget for clock
        players[ev.pid].au    = false;  // new day: action slot restored
        players[ev.pid].rest = false;
        if (ev.pid === myId) {
          if (uiResting.val && ev.dll < 0) showShelterWarning();
          uiResting.val = false;  // clear resting at dawn
        }
        updateRestBubbles();
        if (ev.fth !== undefined) players[ev.pid].fth = ev.fth;
        if (ev.wth !== undefined) players[ev.pid].wth = ev.wth;
        if (ev.rad !== undefined) players[ev.pid].rad = ev.rad;
        if (ev.fat !== undefined) players[ev.pid].fat = ev.fat;
      }
      const who    = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
      const llTxt  = ev.dll < 0 ? ` \u25BC LL${ev.dll}` : ev.dll > 0 ? ` \u25B2 LL+${ev.dll}` : '';
      const cls    = ev.dll < 0 ? 'log-check-fail' : 'log-mv';
      addLog(`<span class="${cls}">\u2600 Day ${ev.day}: ${escHtml(who)} F:${ev.f} W:${ev.w}${llTxt}</span>`);
      if (ev.pid === myId) {
        if (ev.dll < 0) showToast(`\u2600 Dawn LL${ev.dll}  F:${ev.f} W:${ev.w}`);
        else showToast(`\u2600 Day ${ev.day} — MP:${ev.mp}`);
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
        p.fat  = ev.fat;
        p.au   = (ev.out !== AO_BLOCKED);  // BLOCKED = action slot not consumed
        if (ev.fd)   p.inv[1] = Math.min(99, (p.inv[1] ?? 0) + ev.fd);
        if (ev.wd)   p.inv[0] = Math.min(99, (p.inv[0] ?? 0) + ev.wd);
        if (ev.lld)  p.ll     = Math.max(0, Math.min(6, p.ll + (ev.lld ?? 0)));
        if (ev.rad !== undefined) p.rad = ev.rad;
        if (ev.resd) p.res    = Math.max(0, Math.min(5, (p.res ?? 3) + ev.resd));
        // Update wound/status bits on successful Treat (§7.3A)
        if (ev.a === ACT_TREAT && ev.out === AO_SUCCESS && p.wd) {
          if (ev.cnd === TC_MINOR   && p.wd[0] > 0)  p.wd[0]--;
          if (ev.cnd === TC_BLEED)                    p.sb = (p.sb ?? 0) & ~0x04;
          if (ev.cnd === TC_FEVER)                    p.sb = (p.sb ?? 0) & ~0x08;
          if (ev.cnd === TC_GRIEVOUS && p.wd[2] > 0) p.wd[2]--;
        }
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
      if (ev.resd) detail += ` +${ev.resd}Resolve`;
      if (ev.a === ACT_TREAT && ev.out === AO_SUCCESS && ev.cnd === TC_GRIEVOUS)
        detail += ' \u2605Grievous healed';
      addLog(`<span class="${outCl}">${outTx} ${escHtml(who)} ${actNm}${detail}</span>`);
      // Special toasts for REST and SHELTER
      if (ev.a === ACT_REST && ev.out === AO_SUCCESS) {
        players[ev.pid].rest = true;
        if (ev.pid === myId) uiResting.val = true;
        updateRestBubbles();
        checkAutoRest();
        const msg = ev.pid === myId ? 'Resting — waiting for dawn' : `${escHtml(who)} is resting`;
        showToast(`\ud83d\ude34 ${msg}`);
        if (ev.pid !== myId) addLog(`<span class="log-mv">\ud83d\ude34 ${escHtml(who)} is now waiting for dawn</span>`);
      } else if (ev.a === ACT_SHELTER && ev.out === AO_SUCCESS) {
        const shelterName = ev.cnd === 2 ? 'an improved shelter 🏠' : 'a shelter ⛺';
        const msg = ev.pid === myId ? `Built ${shelterName}!` : `${escHtml(who)} built ${shelterName}`;
        showToast(`🔨 ${msg}`);
        // Update local gameMap immediately so the shelter icon renders without waiting for next move
        const sp = players[ev.pid];
        if (sp && gameMap[sp.r]?.[sp.q] !== undefined) {
          gameMap[sp.r][sp.q].shelter = ev.cnd;
        }
      // Track scrap delta for all actions (SCAV gives +1, SHELTER spends -1/-2)
      if (ev.sd !== undefined && ev.sd !== 0) {
        players[ev.pid].inv[4] = Math.max(0, (players[ev.pid].inv[4] ?? 0) + ev.sd);
      }
      } else if (ev.pid === myId) {
        const toastMsg =
          ev.out === AO_BLOCKED  ? `\u2297 ${actNm}: not available` :
          ev.out === AO_SUCCESS  ? `\u25CF ${actNm}: success` :
          ev.out === AO_PARTIAL  ? `\u25D1 ${actNm}: partial success` :
                                   `\u25CB ${actNm}: failed`;
        showToast(toastMsg);
      }
      updateSidebar();
      break;
    }

    case 'surv': {
      // SURVEY result: update gameMap and mark cells as surveyed for rendering.
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
  }
}

// ── Map decode ──────────────────────────────────────────────────
// New encoding: 2 bytes per cell (4 hex chars)
//   terrainByte = 0x00-0x0A (terrain index) or 0xFF (fog)
//   dataByte    = (footprints[0-5])|(shelter<<6)  shelter: 0=none, 1=shelter, 2=improved shelter
function decodeCell(terrainByte, dataByte, variantByte = 0) {
  if (terrainByte === 0xFF) return null;
  return {
    terrain:    terrainByte,
    footprints: dataByte & 0x3F,  // bits 0-5: which players visited (bitmask)
    shelter:    (dataByte >> 6) & 3, // bits 6-7: 0=none, 1=shelter, 2=improved shelter
    variant:    variantByte
  };
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
// Format: "QQRRTTDD..." — 8 hex chars per cell
//   QQ=col, RR=row, TT=terrain, DD=data
function applyVisDisk(cells) {
  for (let i = 0; i < cells.length; i += 10) {
    const q  = parseInt(cells.substr(i,     2), 16);
    const r  = parseInt(cells.substr(i + 2, 2), 16);
    const tt = parseInt(cells.substr(i + 4, 2), 16);
    const dd = parseInt(cells.substr(i + 6, 2), 16);
    const vv = parseInt(cells.substr(i + 8, 2), 16);
    if (r < MAP_ROWS && q < MAP_COLS)
      gameMap[r][q] = decodeCell(tt, dd, vv);
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
let HEX_SZ   = 56;

function resize() {
  const wrap = document.getElementById('canvas-wrap');
  canvas.width  = wrap.clientWidth;
  canvas.height = wrap.clientHeight;
  HEX_SZ = Math.max(36, Math.min(64, Math.floor(canvas.width / 11)));
}
window.addEventListener('resize', resize);
resize();

// ── Draw terrain icon centred in a hex ───────────────────────────
function drawTerrainIcon(ctx, cx, cy, hexSz, terrainIdx, hasResource) {
  const t = TERRAIN[terrainIdx];
  if (!t) return;
  const sz   = Math.max(10, hexSz * 0.44);
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

// ── Draw character/player icon ────────────────────────────────────
function drawCharIcon(ctx, cx, cy, hexSz, color, label, isMe, nm) {
  const scale  = hexSz * 0.44;
  const headR  = scale * 0.40;
  const headCY = cy - scale * 0.32;
  const torsoT = cy - scale * 0.00;
  const torsoB = cy + scale * 0.75;
  const torsoW = scale * 0.46;

  // Ground shadow
  ctx.save();
  ctx.beginPath();
  ctx.ellipse(cx + 1, cy + scale * 1.0, scale * 0.36, scale * 0.09, 0, 0, Math.PI * 2);
  ctx.fillStyle = 'rgba(0,0,0,0.40)';
  ctx.fill();
  ctx.restore();

  if (isMe) {
    ctx.save();
    ctx.shadowColor = color;
    ctx.shadowBlur  = 20;
  }

  // Torso
  const tr = torsoW * 0.35;
  ctx.beginPath();
  ctx.moveTo(cx - torsoW + tr, torsoT);
  ctx.lineTo(cx + torsoW - tr, torsoT);
  ctx.arcTo(cx + torsoW, torsoT, cx + torsoW, torsoT + tr, tr);
  ctx.lineTo(cx + torsoW, torsoB - tr);
  ctx.arcTo(cx + torsoW, torsoB, cx + torsoW - tr, torsoB, tr);
  ctx.lineTo(cx - torsoW + tr, torsoB);
  ctx.arcTo(cx - torsoW, torsoB, cx - torsoW, torsoB - tr, tr);
  ctx.lineTo(cx - torsoW, torsoT + tr);
  ctx.arcTo(cx - torsoW, torsoT, cx - torsoW + tr, torsoT, tr);
  ctx.closePath();
  ctx.fillStyle = isMe ? color : color + 'AA';
  ctx.fill();

  // Head
  ctx.beginPath();
  ctx.arc(cx, headCY, headR, 0, Math.PI * 2);
  ctx.fillStyle = color;
  ctx.fill();

  if (isMe) {
    ctx.strokeStyle = '#FFD700';
    ctx.lineWidth   = 2.5;
    ctx.stroke();
    ctx.restore(); // end glow

    const arrowSz  = Math.max(10, scale * 0.62);
    const arrowBot = headCY - headR - 3;
    ctx.save();
    ctx.fillStyle    = '#FFD700';
    ctx.font         = `bold ${arrowSz}px monospace`;
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'bottom';
    const bounce = Math.sin(Date.now() / 350) * 2;
    ctx.fillText('▼', cx, arrowBot + bounce);
    ctx.restore();
  }

  // Player number in head
  ctx.save();
  ctx.fillStyle    = isMe ? '#000' : '#EEE';
  ctx.font         = `bold ${Math.max(7, Math.round(headR * 1.05))}px monospace`;
  ctx.textAlign    = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(label, cx, headCY + 0.5);
  ctx.restore();

  // Call sign above icon
  if (nm) {
    const tag      = nm.substring(0, 8).toUpperCase();
    const nameSz   = Math.max(8, Math.round(hexSz * 0.21));
    const arrowSz  = Math.max(10, scale * 0.62);
    // Position: above the ▼ arrow for self, above head for others
    const topY     = isMe ? headCY - headR - arrowSz - 6 : headCY - headR - 4;
    ctx.save();
    ctx.font        = `${nameSz}px 'Courier New', monospace`;
    ctx.textAlign   = 'center';
    ctx.textBaseline = 'bottom';
    const tw = ctx.measureText(tag).width;
    // Dark pill backdrop
    const padX = 3, padY = 2;
    ctx.fillStyle = 'rgba(0,0,0,0.72)';
    ctx.beginPath();
    ctx.roundRect(cx - tw / 2 - padX, topY - nameSz - padY, tw + padX * 2, nameSz + padY * 2, 2);
    ctx.fill();
    // Text in player colour, slightly dimmed for others
    ctx.fillStyle = isMe ? color : color + 'CC';
    ctx.letterSpacing = '1px';
    ctx.fillText(tag, cx, topY);
    ctx.restore();
  }
}

// ── Time-of-day clock ────────────────────────────────────────────
const TIME_PHASES = [
  { name:'FIRST LIGHT', icon:'☀',      r:255, g:180, b:100, a:0.10 },
  { name:'HIGH WATCH',  icon:'☀',      r:255, g:220, b:150, a:0.05 },
  { name:'NOON BURN',   icon:'☀',      r:255, g:240, b:200, a:0.02 },
  { name:'LOW SUN',     icon:'🌇',r:255, g:160, b: 60, a:0.18 },
  { name:'DUST HOUR',   icon:'🌆',r:210, g: 90, b: 20, a:0.38 },
  { name:'DARK WATCH',  icon:'🌑',r: 15, g:  8, b: 40, a:0.72 },
];
function getTimePhase() {
  if (myId < 0 || maxMP <= 0) return TIME_PHASES[0];
  const f = 1 - (uiMP.val / maxMP);
  return TIME_PHASES[f < 0.17 ? 0 : f < 0.34 ? 1 : f < 0.51 ? 2 : f < 0.68 ? 3 : f < 0.84 ? 4 : 5];
}

// ── Render ──────────────────────────────────────────────────────
function render() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#050301';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

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
  const ox   = canvas.width  / 2 - cp.x;
  const oy   = canvas.height / 2 - cp.y;

  // Fog uses the ACTUAL integer position (accurate fog boundary)
  const meAct = (myId >= 0) ? players[myId] : { q: 0, r: 0 };

  const viewQ   = Math.ceil(canvas.width  / (HEX_SZ * 1.5)) + 2;
  const viewR   = Math.ceil(canvas.height / (HEX_SZ * SQRT3)) + 2;
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

      if (cx < -HEX_SZ * 2 || cx > canvas.width  + HEX_SZ * 2) continue;
      if (cy < -HEX_SZ * 2 || cy > canvas.height + HEX_SZ * 2) continue;

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
        ctx.globalAlpha = 0.9;   // outer fog ring — slightly transparent
        ctx.fillStyle   = '#141008';
      } else {
        ctx.globalAlpha = 1;     // deep fog — fully opaque
        ctx.fillStyle   = '#080402';
      }
      ctx.strokeStyle = 'rgba(160,160,160,0.25)';
      ctx.lineWidth   = 0.4;
      ctx.fill();
      ctx.globalAlpha = 1;
      ctx.stroke();

      if (!visible && !surveyed) continue;

      // Surveyed cells draw terrain at reduced opacity; fully visible cells at full opacity.
      ctx.globalAlpha = surveyed ? 0.7 : 1;

      // Terrain hex image (if loaded) — alpha channel handles hex shape, no clipping
      const _tv  = terrainImgVariants[cell.terrain];
      const tImg = (_tv && _tv[cell.variant || 0]) || (_tv && _tv[0]);
      if (tImg && tImg.loaded) {
        const imgSz = HEX_SZ * 2;
        ctx.drawImage(tImg, cx - imgSz / 2, cy - imgSz / 2, imgSz, imgSz);
      }

      // Terrain icon (no resource indicator) — only when no image
      if (!tImg || !tImg.loaded) {
        drawTerrainIcon(ctx, cx, cy, HEX_SZ, cell.terrain, false);
      }

      // Reset alpha — footprints/shelter always render at full opacity.
      ctx.globalAlpha = 1;

      // Footprints: show which players have visited this hex.
      // Fresh footprints glow bright red, fading to near-black over FOOTPRINT_FADE_MS.
      // ctx.filter is used to tint the emoji (fillStyle has no effect on emoji colour).
      if (cell.footprints > 0) {
        const footprintSize = Math.max(6, Math.round(HEX_SZ * 0.22));
        let footprintCount  = 0;
        for (let i = 0; i < 6; i++) if (cell.footprints & (1 << i)) footprintCount++;

        let footprintIdx = 0;
        const now = Date.now();
        for (let fpid = 0; fpid < 6; fpid++) {
          if ((cell.footprints & (1 << fpid)) === 0) continue;

          const angle  = (footprintIdx * Math.PI * 2) / Math.max(1, footprintCount);
          const radius = HEX_SZ * 0.28;
          const fx = cx + Math.cos(angle) * radius;
          const fy = cy + Math.sin(angle) * radius;

          // t=0 → just stamped (bright red); t=1 → fully faded (near black)
          const ts = footprintTimestamps.get(`${mapQ}_${mapR}_${fpid}`);
          const t  = ts ? Math.min(1, (now - ts) / FOOTPRINT_FADE_MS) : 1;

          // sepia(1)+hue-rotate(330deg) shifts the emoji palette into the red spectrum.
          // saturate and brightness interpolate from vivid/bright → dim/dark.
          const sat = (10 * (1 - t)).toFixed(1);
          const bri = (1.8 * (1 - t) + 0.06).toFixed(2);

          ctx.save();
          ctx.filter     = `sepia(1) hue-rotate(330deg) saturate(${sat}) brightness(${bri})`;
          ctx.globalAlpha = 0.9;
          ctx.font        = `${footprintSize}px monospace`;
          ctx.textAlign   = 'center';
          ctx.textBaseline = 'middle';
          ctx.fillText('👣', fx, fy);
          ctx.restore();  // also resets ctx.filter to 'none'

          footprintIdx++;
        }
      }

      // Shelter indicator: 1=shelter (⛺), 2=improved shelter (🏠)
      if (cell.shelter) {
        ctx.save();
        ctx.fillStyle    = cell.shelter === 2 ? '#7EC8E3' : '#D4A574';
        ctx.font         = `${Math.max(12, Math.round(HEX_SZ * 0.5))}px monospace`;
        ctx.textAlign    = 'right';
        ctx.textBaseline = 'top';
        ctx.fillText(cell.shelter === 2 ? '🏠' : '⛺', cx + HEX_SZ * 0.35, cy - HEX_SZ * 0.35);
        ctx.restore();
      }
    }
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
    if (pcx < -HEX_SZ * 2 || pcx > canvas.width  + HEX_SZ * 2) continue;
    if (pcy < -HEX_SZ * 2 || pcy > canvas.height + HEX_SZ * 2) continue;

    drawCharIcon(ctx, pcx, pcy, HEX_SZ, PLAYER_COLORS[i], i, i === myId, players[i].nm);
  }

  // ── Time-of-day tint overlay ──────────────────────────────────
  {
    const phase = getTimePhase();
    ctx.save();
    ctx.globalAlpha = phase.a;
    ctx.fillStyle   = `rgb(${phase.r},${phase.g},${phase.b})`;
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.restore();
  }

  requestAnimationFrame(render);
}
