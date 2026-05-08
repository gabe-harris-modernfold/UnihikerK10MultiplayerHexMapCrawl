// Mock Wasteland Crawl server — serves data/ statically and fakes /ws so the
// client can connect, pick a character, see a map, and move.

const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const DATA_DIR = path.resolve(__dirname, '..', 'data');
const PORT     = 8765;

const MAP_COLS = 25, MAP_ROWS = 19, MAX_PLAYERS = 6;

const hex2 = (n) => n.toString(16).padStart(2, '0');

// ── Resource state ──────────────────────────────────────────────────────────
// "q_r" -> { res: 1-5 (0 = empty), amt, respawnTimer }
const resources = {};
const RESPAWN_TICKS = 8;       // ~8 seconds (1 tick/sec)
const INV_SLOTS     = 6;       // small cap so we can test inv-full path
const RES_MAX_TYPE  = 5;

// ── Build a fully-revealed map with sprinkled features ──────────────────────
// 6 hex chars per cell: TT (terrain) DD (data: footprints/shelter/poi) VV (resource/variant)
function buildMap() {
  let s = '';
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      const idx = r * MAP_COLS + c;
      const tt  = idx % 11;                              // terrain 0..10
      let   dd  = 0;
      let   vv  = 0;
      if (idx % 17 === 0) {
        const rType = (idx % 5) + 1;                     // 1..5
        vv = (rType << 4) | (idx & 0x0F);
        resources[`${c}_${r}`] = { res: rType, amt: 2 + (idx % 2), respawnTimer: 0 };
      }
      if (idx % 31 === 7) dd |= 0x80;                    // POI
      if (idx % 23 === 3) dd |= 0x40;                    // shelter
      s += hex2(tt) + hex2(dd) + hex2(vv);
    }
  }
  return s;
}
const MAP_HEX = buildMap();

// Live snapshot of current map state — encodes cells with up-to-date resource
// info so reconnects/syncs reflect collection truth.
function liveMapHex() {
  let s = '';
  for (let r = 0; r < MAP_ROWS; r++) {
    for (let c = 0; c < MAP_COLS; c++) {
      const baseIdx = (r * MAP_COLS + c) * 6;
      const tt = MAP_HEX.substr(baseIdx,     2);
      const dd = MAP_HEX.substr(baseIdx + 2, 2);
      const ddNum = parseInt(dd, 16);
      const cell = resources[`${c}_${r}`];
      const variant = baseIdx & 0x0F;
      const res     = cell ? cell.res : 0;
      const vv = hex2((res << 4) | (variant & 0x0F));
      s += tt + hex2(ddNum) + vv;
    }
  }
  return s;
}

// Build a visdisk around (pq, pr) reflecting live resource state.
function buildVisDisk(pq, pr, vr) {
  let cells = '';
  for (let dr = -vr; dr <= vr; dr++) {
    for (let dq = -vr; dq <= vr; dq++) {
      const s = -(dq + dr);
      if (Math.abs(dq) + Math.abs(dr) + Math.abs(s) > 2 * vr) continue;
      const cq = ((pq + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
      const cr = ((pr + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
      const baseIdx = (cr * MAP_COLS + cq) * 6;
      const tt = MAP_HEX.substr(baseIdx,     2);
      const dd = MAP_HEX.substr(baseIdx + 2, 2);
      const cell = resources[`${cq}_${cr}`];
      const variant = baseIdx & 0x0F;
      const res     = cell ? cell.res : 0;
      const vv = hex2((res << 4) | (variant & 0x0F));
      cells += hex2(cq) + hex2(cr) + tt + dd + vv;
    }
  }
  return cells;
}

// ── Player factory ──────────────────────────────────────────────────────────
function makePlayer(id) {
  return {
    id, on: true,
    q: 12, r: 9,
    sc: 0,
    inv: [0, 0, 0, 0, 0],
    sp: 0,
    ll: 7, food: 6, water: 6, rad: 0,
    mp: 6,
    fth: 0, wth: 0,
    vm: 0x3F,
    rt: 0,
    it: [], iq: [],
    eq: {},
    enc: false,
  };
}

const players = {};       // id -> player
const sockets = new Map(); // ws -> id

// ── Direction deltas (approximate — "mostly works") ─────────────────────────
// Buttons: 0=SE 1=NE 2=N 3=NW 4=SW 5=S
const DIR_DELTA = {
  0: [+1, +1],  // SE
  1: [+1, -1],  // NE
  2: [ 0, -1],  // N
  3: [-1, -1],  // NW
  4: [-1, +1],  // SW
  5: [ 0, +1],  // S
};

const send = (ws, obj) => {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
};

const broadcast = (obj) => {
  for (const ws of sockets.keys()) send(ws, obj);
};

function syncMsg(id) {
  return {
    t: 'sync',
    id,
    vr: 4,
    map: liveMapHex(),
    p: Object.values(players),
    gs: { wp: 0 },
    gi: [],
  };
}

// Mirror of firmware's collectResource — must stay in sync with survival_state.hpp
function tryCollect(p, ws) {
  const k = `${p.q}_${p.r}`;
  const cell = resources[k];
  if (!cell || cell.res === 0 || cell.amt === 0) return; // nothing to collect (and no fail event — server-side knows truly nothing here)

  const total = p.inv.reduce((a, b) => a + b, 0);
  if (total >= INV_SLOTS) {
    // Inv-full: send col_fail to the moving player only.
    send(ws, { t: 'ev', k: 'col_fail', pid: p.id, q: p.q, r: p.r, res: cell.res, reason: 2 });
    console.log(`[col] FAIL inv-full pid=${p.id} q=${p.q} r=${p.r}`);
    return;
  }
  const room = INV_SLOTS - total;
  const gain = Math.min(cell.amt, room);
  if (gain === 0) return;

  const resType = cell.res;
  p.inv[resType - 1] = (p.inv[resType - 1] || 0) + gain;
  p.sc += gain * 10;
  cell.amt -= gain;
  const rem = cell.amt;
  if (rem === 0) {
    cell.res = 0;
    cell.respawnTimer = RESPAWN_TICKS;
  }
  console.log(`[col] OK pid=${p.id} q=${p.q} r=${p.r} res=${resType} gain=${gain} rem=${rem}`);
  broadcast({ t: 'ev', k: 'col', pid: p.id, q: p.q, r: p.r, res: resType, amt: gain, rem });
}

// Respawn tick — broadcasts EVT_RESPAWN to all (no vision cull, mirrors fix).
setInterval(() => {
  for (const k of Object.keys(resources)) {
    const cell = resources[k];
    if (cell.res === 0 && cell.respawnTimer > 0) {
      if (--cell.respawnTimer === 0) {
        cell.res = 1 + Math.floor(Math.random() * RES_MAX_TYPE);
        cell.amt = 1 + Math.floor(Math.random() * 3);
        const [q, r] = k.split('_').map(Number);
        console.log(`[rsp] q=${q} r=${r} res=${cell.res} amt=${cell.amt}`);
        broadcast({ t: 'ev', k: 'rsp', q, r, res: cell.res, amt: cell.amt });
      }
    }
  }
}, 1000);

function stateMsg() {
  // Build dense p[] indexed 0..MAX_PLAYERS-1; missing slots = offline stub.
  const p = [];
  for (let i = 0; i < MAX_PLAYERS; i++) {
    p[i] = players[i] || { on: false, q: 0, r: 0, sc: 0, inv: [0,0,0,0,0], sp: 0,
                           ll: 0, food: 0, water: 0, rad: 0,
                           mp: 0, fth: 0, wth: 0, vm: 0, rt: 0,
                           it: [], iq: [], eq: {}, enc: false };
  }
  return { t: 's', p, gs: { wp: 0 } };
}

function lobbyMsg() {
  const taken = new Set(Object.keys(players).map(Number));
  const avail = [];
  for (let i = 0; i < MAX_PLAYERS; i++) if (!taken.has(i)) avail.push(i);
  return { t: 'lobby', avail };
}

// ── HTTP static server ──────────────────────────────────────────────────────
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js'  : 'application/javascript; charset=utf-8',
  '.css' : 'text/css; charset=utf-8',
  '.png' : 'image/png',
  '.jpg' : 'image/jpeg',
  '.svg' : 'image/svg+xml',
  '.ico' : 'image/x-icon',
  '.json': 'application/json; charset=utf-8',
};

const UPLOAD_DIR = path.resolve(__dirname, 'uploads');

// Mirror the firmware's POST /upload?dest=/data/foo handler so sync_data.ps1
// works against the mock without a board. Body is streamed straight to disk
// under mock-server/uploads/<dest> (gitignored).
function handleUpload(req, res) {
  const u = new URL(req.url, 'http://x');
  let dest = u.searchParams.get('dest');
  if (!dest) {
    res.writeHead(400, { 'Content-Type': 'text/plain' });
    res.end('missing ?dest=');
    return;
  }
  if (dest.includes('..')) {
    res.writeHead(400, { 'Content-Type': 'text/plain' });
    res.end('bad dest');
    return;
  }
  if (!dest.startsWith('/')) dest = '/' + dest;
  const outPath = path.join(UPLOAD_DIR, dest);
  if (!outPath.startsWith(UPLOAD_DIR)) {
    res.writeHead(403); res.end('forbidden'); return;
  }
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  const sink = fs.createWriteStream(outPath);
  let total = 0;
  req.on('data', (chunk) => { total += chunk.length; });
  req.pipe(sink);
  sink.on('finish', () => {
    console.log(`[upload] ${dest}  ${total} bytes`);
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('OK');
  });
  sink.on('error', (e) => {
    console.error('[upload] write fail', e);
    res.writeHead(500); res.end('write fail');
  });
}

const httpServer = http.createServer((req, res) => {
  if (req.method === 'POST' && req.url.split('?')[0] === '/upload') {
    handleUpload(req, res);
    return;
  }
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath === '/' || urlPath === '') urlPath = '/index.html';
  const filePath = path.join(DATA_DIR, urlPath);
  if (!filePath.startsWith(DATA_DIR)) { res.writeHead(403); res.end(); return; }
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404); res.end('Not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(filePath).toLowerCase()] || 'application/octet-stream' });
    res.end(data);
  });
});

// ── WebSocket /ws ───────────────────────────────────────────────────────────
const wss = new WebSocketServer({ noServer: true });

httpServer.on('upgrade', (req, socket, head) => {
  if (req.url !== '/ws') { socket.destroy(); return; }
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req));
});

wss.on('connection', (ws) => {
  console.log('[WS] client connected');
  sockets.set(ws, -1);
  // Tell client which slots are free
  send(ws, lobbyMsg());

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }
    console.log('[RX]', msg.t, msg);

    switch (msg.t) {
      case 'wifi':
        send(ws, { t: 'wifi', status: 'saved', ssid: msg.ssid || '', pass: msg.pass || '' });
        break;

      case 'pick': {
        const id = msg.arch | 0;
        if (id < 0 || id >= MAX_PLAYERS || players[id]) {
          send(ws, lobbyMsg());
          break;
        }
        players[id] = makePlayer(id);
        sockets.set(ws, id);
        send(ws, { t: 'asgn', id });
        send(ws, syncMsg(id));
        broadcast(stateMsg());
        break;
      }

      case 'm': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        const [dq, dr] = DIR_DELTA[msg.d] || [0, 0];
        p.q = ((p.q + dq) % MAP_COLS + MAP_COLS) % MAP_COLS;
        p.r = ((p.r + dr) % MAP_ROWS + MAP_ROWS) % MAP_ROWS;
        p.sp += 1;
        if (p.mp > 0) p.mp -= 1;
        // EVT_MOVE first so client position updates before col event arrives.
        broadcast({ t: 'ev', k: 'mv', pid: id, q: p.q, r: p.r, radd: 0, rad: p.rad, exploD: 0, mp: p.mp });
        tryCollect(p, ws);
        // Visdisk reflects current resource state (post-collection).
        const cells = buildVisDisk(p.q, p.r, 4);
        send(ws, { t: 'vis', q: p.q, r: p.r, vr: 4, cells });
        broadcast(stateMsg());
        break;
      }

      case 'act': {
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p) break;
        // ACT_REST=7
        if (msg.a === 7) {
          p.rt = p.rt ? 0 : 1;
          if (p.rt) { p.mp = 6; p.food = Math.min(6, p.food + 1); p.water = Math.min(6, p.water + 1); }
        } else {
          // Generic: drain a bit, give a resource.
          p.mp = Math.max(0, p.mp - (msg.mp || 1));
          const slot = (msg.a | 0) % 5;
          p.inv[slot] = (p.inv[slot] || 0) + 1;
          p.sc += 1;
        }
        broadcast(stateMsg());
        break;
      }

      case 'enc_start':
        // Pretend nothing here — bounce back an empty event so client unblocks.
        send(ws, { t: 'ev', kind: 'none' });
        break;

      case 'dbg_setinv': {
        // Test-only: clobber inv so we can exercise inv-full / partial-pickup paths.
        const id = sockets.get(ws);
        const p  = players[id];
        if (!p || !Array.isArray(msg.inv)) break;
        for (let i = 0; i < 5; i++) p.inv[i] = msg.inv[i] | 0;
        broadcast(stateMsg());
        break;
      }
    }
  });

  ws.on('close', () => {
    const id = sockets.get(ws);
    sockets.delete(ws);
    if (id >= 0 && players[id]) {
      delete players[id];
      broadcast(stateMsg());
    }
    console.log('[WS] client closed (id=' + id + ')');
  });
});

httpServer.listen(PORT, () => {
  console.log(`Mock server listening on http://localhost:${PORT}/`);
  console.log(`WebSocket at ws://localhost:${PORT}/ws`);
});
