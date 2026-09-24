// ── Bunker tunnel board ───────────────────────────────────────────
// Client half of the tunnel system (firmware: tunnels.hpp, spec:
// docs/tunnel-system-spec.md). There are two hex boards now — the 75x57
// surface map and a small underground one — and this file owns which one the
// renderer, decoder and HUD are currently looking at.
//
// The server's invariant matters here: a player's q/r ALWAYS stay on the
// surface, pinned to the hatch they descended through, and tq/tr carry their
// position underground. So "where do I draw player i" is not p.q/p.r — it is
// playerViewPos(i), which also returns null when that player is on the other
// board and therefore not visible at all.
//
// Everything routes through the board* accessors below rather than touching
// gameMap / MAP_COLS / MAP_ROWS directly, because the two boards differ in
// more than size: the surface wraps toroidally, the tunnels are walled.

let tunnelCols = 16;
let tunnelRows = 10;
let tunnelMap  = [];      // [r][q] — same cell shape as gameMap; null = fogged
let myDepth    = 0;       // 0 = surface, 1 = in the tunnels

// Rebuild (and clear) the tunnel board. Called from the tsync handler, which
// also carries the authoritative dimensions — the firmware owns TUN_COLS/ROWS
// and this client just follows whatever it is told.
function initTunnelMap(cols, rows) {
  tunnelCols = cols | 0;
  tunnelRows = rows | 0;
  tunnelMap  = [];
  for (let r = 0; r < tunnelRows; r++) tunnelMap.push(new Array(tunnelCols).fill(null));
}
initTunnelMap(tunnelCols, tunnelRows);

// ── Board accessors ───────────────────────────────────────────────
function boardCells() { return myDepth ? tunnelMap  : gameMap; }
function boardCols()  { return myDepth ? tunnelCols : MAP_COLS; }
function boardRows()  { return myDepth ? tunnelRows : MAP_ROWS; }
function boardWraps() { return !myDepth; }

// Normalise a virtual (possibly out-of-range) coordinate onto the board.
// Returns null when there is no such cell — which underground means the view
// has run off the edge into solid rock, and the renderer simply draws nothing
// there (the page background already reads as rock).
function boardNorm(q, r) {
  if (boardWraps()) {
    return { q: ((q % MAP_COLS) + MAP_COLS) % MAP_COLS,
             r: ((r % MAP_ROWS) + MAP_ROWS) % MAP_ROWS };
  }
  if (q < 0 || q >= tunnelCols || r < 0 || r >= tunnelRows) return null;
  return { q, r };
}

function boardCell(q, r) {
  const n = boardNorm(q, r);
  return n ? boardCells()[n.r]?.[n.q] ?? null : null;
}

// Distance on the current board — wrap-aware on the surface, plain axial in
// the tunnels. Used for the vision falloff in renderHexTerrain().
function boardDist(q1, r1, q2, r2) {
  if (boardWraps()) return hexDistWrap(q1, r1, q2, r2);
  return hexDist(q1, r1, q2, r2);
}

// ── Where I am, on the board I am on ──────────────────────────────
// me.q/r stay pinned to the entrance hatch while below, so anything asking
// "what am I standing on" or "who is here with me" goes through these rather
// than gameMap[me.r][me.q] -- underground that is the hatch hex up on the
// surface, not the corridor under your feet. Keyed on myDepth (the board being
// shown), not me.dp, so the panels always agree with what is drawn.
function myBoardPos() {
  const me = players[myId];
  if (!me) return null;
  return myDepth ? { q: me.tq | 0, r: me.tr | 0 } : { q: me.q, r: me.r };
}

function myBoardCell() {
  const pos = myBoardPos();
  return pos ? boardCells()[pos.r]?.[pos.q] ?? null : null;
}

// Same hex AND the same board -- the client mirror of samehex()
// (inventory_items.hpp), which is what the server checks a trade against.
function sharesMyHex(p) {
  const me = players[myId];
  if (!p || !me || (p.dp | 0) !== myDepth) return false;
  return myDepth ? ((p.tq | 0) === (me.tq | 0) && (p.tr | 0) === (me.tr | 0))
                 : (p.q === me.q && p.r === me.r);
}

// The caravan drives the surface only; parked on your hatch is not "here".
function caravanSharesMyHex() {
  const me = players[myId];
  const c  = worldState.caravan;
  return !!(me && !myDepth && c?.active && c.q === me.q && c.r === me.r);
}

// ── Where to draw a player ────────────────────────────────────────
// null = they are on the other board and should not be drawn at all.
function playerViewPos(i) {
  const p = players[i];
  if (!p) return null;
  if ((p.dp | 0) !== myDepth) return null;
  return myDepth ? { q: p.tq | 0, r: p.tr | 0 } : { q: p.q, r: p.r };
}

// True when player i is underground while WE are on the surface, and their
// pinned q/r therefore marks the hatch they went down. The renderer draws a
// muted "gone below" marker there so the party can see where someone went.
function playerIsBelowUs(i) {
  const p = players[i];
  return !!p && !myDepth && (p.dp | 0) === 1;
}

// ── Depth transitions ─────────────────────────────────────────────
// Set from the state broadcast and from the tun_in/tun_out events. Returns
// true when the value actually changed, so callers can reset view state
// (surveyed cells are vantage-relative and meaningless on the other board).
function setMyDepth(d) {
  const nd = d ? 1 : 0;
  if (nd === myDepth) return false;
  myDepth = nd;
  if (typeof surveyedCells !== 'undefined') surveyedCells.clear();
  if (typeof uiDepth !== 'undefined') uiDepth.val = myDepth;
  // Particles and bolts live in screen space, so anything in flight when you
  // cross would keep falling over the other board: a fog puff outlives a
  // step by seconds. Fire flames go too -- fire only burns on the surface
  // (renderer.js skips the fire field underground), so none are respawned.
  if (typeof weatherParticles !== 'undefined' && weatherParticles) weatherParticles.particles = [];
  if (typeof lightningSystem  !== 'undefined' && lightningSystem)  lightningSystem.bolts = [];
  return true;
}

// Crossing between the boards is not a control the player presses: stepping
// onto a Bunker Entrance (12) or Vent Shaft (13) does it, server-side, as the
// last act of that move (tunnelStepDown/tunnelStepUp in tunnels.hpp). The
// client just receives a tsync / vis disk for the board it is now on.
