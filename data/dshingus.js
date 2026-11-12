// ── dshingus.js ─────────────────────────────────────────────────────────────
// The Dshingus (dshingus.hpp): what Dshinguest Ron's band leaves behind in
// every town on the Movement Tour (encounters/settlement/25.json). For 30
// dawns a survivor who has it may leave a brown spot where they sleep, and
// passes it to anybody who shares their hex.
//
// State is worldState.dsh, merged in by _applyWorldState in network.js:
// {d: [days left per seat], s: [[q, r, day], ...]}. The tick carries d only;
// s comes with sync, and each new spot as ev "dsh" w "spot" (_evDsh below,
// with w "catch" and "clear"). A spot ages off gameState.dc exactly as the
// board ages it (groundAge), so a dawn sends nothing for them.
//
// Drawn with math, no sprite. A spot is a lumpy brown splat with a wet sheen
// and a few flung drops, seeded by its hex and day so it never shifts, and it
// dries out paler as it ages; a fresh one has flies. Whoever has it carries a
// few flies round their portrait and a stink line over it (dshOverPawn,
// called by renderCharacters). Spots are drawn by renderHexContent beside the
// footprints, so like them only inside your sight.

const DSH_SPOT_DAYS = 10;    // dawns a spot lasts (dshingus.hpp)
const DSH_YEAR      = 365;   // DAYS_PER_YEAR: the day count wraps 365 -> 1
let   dshDays  = [0, 0, 0, 0, 0, 0];   // dawns left per seat; 0 = clean
let   dshSpots = new Map();            // "q_r" -> the day it was left

const dshAge = (day) => ((((gameState.dc | 0) - day) % DSH_YEAR) + DSH_YEAR) % DSH_YEAR;
const dshName = (pid) => (pid === myId ? 'You' : escHtml(players[pid]?.nm || `P${pid}`));

// ── World state ─────────────────────────────────────────────────────────────
function dshApplyWorld() {
  const w = worldState.dsh;
  if (!w) return;
  if (Array.isArray(w.d)) dshDays = w.d.slice(0, MAX_PLAYERS).map((v) => v | 0);
  if (Array.isArray(w.s)) dshSpots = new Map(w.s.map(([q, r, day]) => [`${q}_${r}`, day | 0]));
  dshUpdateHud();
}

function dshUpdateHud() {
  const el = document.getElementById('hud-dsh');
  if (!el) return;
  const d = myId >= 0 ? (dshDays[myId] | 0) : 0;
  el.style.display = d ? 'inline' : 'none';
  if (d) {
    el.textContent = `✺ DSHINGUS ${d}D`;
    el.title = `You have the Dshingus: ${d} more ${d === 1 ? 'dawn' : 'dawns'}. You may leave a brown spot wherever you rest, and anybody who shares your hex catches it.`;
  }
}

// ── Events ──────────────────────────────────────────────────────────────────
// {"t":"ev","k":"dsh","w":"catch|spot|clear","pid","src","q","r","d","day"}
// src -1 = caught in a scene (the band); d = days left; day = the board's day.
function _evDsh(ev) {
  const pid = ev.pid | 0;
  if (pid >= 0 && pid < MAX_PLAYERS) dshDays[pid] = ev.d | 0;
  const me = pid === myId;
  if (ev.w === 'catch') {
    const src  = ev.src | 0;
    const from = src < 0 ? 'off Dshinguest Ron' : src === myId ? 'off you' : `off ${escHtml(players[src]?.nm || `P${src}`)}`;
    addLog(`<span class="log-dsh">✺ ${dshName(pid)} caught the Dshingus ${from}.</span>`);
    if (me) showToast(src < 0 ? 'You caught the Dshingus. The band calls it a souvenir.'
                              : 'You stood too close. You have the Dshingus now.', 'dsh');
    else if (src === myId) showToast(`You gave ${players[pid]?.nm || 'them'} the Dshingus.`, 'dsh');
  } else if (ev.w === 'spot') {
    dshSpots.set(`${ev.q}_${ev.r}`, ev.day | 0);
    if (me) addLog('<span class="log-dsh">✺ You left a brown spot where you slept.</span>');
  } else if (ev.w === 'clear') {
    addLog(`<span class="log-col">✺ ${dshName(pid)} ${me ? 'are' : 'is'} over the Dshingus.</span>`);
  }
  dshUpdateHud();
}

// ── Drawing ─────────────────────────────────────────────────────────────────
// A small stable RNG per spot: the same splat every frame, on every client.
function dshRng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s + 0x6D2B79F5) >>> 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// A closed, lumpy blob through n jittered radii, smoothed through the
// midpoints so it reads as poured, not plotted.
function dshBlob(c, x, y, R, rnd, n, lump, squash) {
  const pts = [];
  for (let i = 0; i < n; i++) {
    const a = (i / n) * Math.PI * 2 + (rnd() - 0.5) * 0.35;
    const rr = R * (1 - lump + rnd() * lump * 2);
    pts.push([x + Math.cos(a) * rr, y + Math.sin(a) * rr * squash]);
  }
  c.beginPath();
  for (let i = 0; i <= n; i++) {
    const p = pts[i % n], q = pts[(i + 1) % n];
    const mx = (p[0] + q[0]) / 2, my = (p[1] + q[1]) / 2;
    if (i === 0) c.moveTo(mx, my); else c.quadraticCurveTo(p[0], p[1], mx, my);
  }
  c.closePath();
}

// A fly: a dark body and two wing smears that flicker.
function dshFly(c, x, y, s, t, k) {
  const flap = 0.5 + 0.5 * Math.sin(t * 0.09 + k * 2.1);
  c.fillStyle = `rgba(210,220,200,${(0.25 + flap * 0.3).toFixed(2)})`;
  c.beginPath();
  c.ellipse(x - s * 0.9, y - s * 0.7, s * 0.9, s * 0.45, -0.6, 0, Math.PI * 2);
  c.ellipse(x + s * 0.9, y - s * 0.7, s * 0.9, s * 0.45, 0.6, 0, Math.PI * 2);
  c.fill();
  c.fillStyle = '#120c06';
  c.beginPath();
  c.ellipse(x, y, s * 0.7, s * 0.95, 0, 0, Math.PI * 2);
  c.fill();
}

// renderHexContent, after the footprints: the spot on (q, r), if there is one.
// Off the hex centre, where a pawn does not stand on it.
function dshDrawHexSpot(c, cx, cy, q, r) {
  if (!dshSpots.size) return;
  const key = `${q}_${r}`;
  const day = dshSpots.get(key);
  if (day === undefined) return;
  const age = dshAge(day);
  if (age >= DSH_SPOT_DAYS) { dshSpots.delete(key); return; }
  const wet = 1 - age / DSH_SPOT_DAYS;            // 1 tonight .. 0.1 the day before it is gone
  const rnd = dshRng(Math.imul(q + 1, 73856093) ^ Math.imul(r + 1, 19349663) ^ Math.imul(day, 83492791));
  const ang = rnd() * Math.PI * 2;
  const x = cx + Math.cos(ang) * HEX_SZ * 0.3, y = cy + Math.sin(ang) * HEX_SZ * 0.22;
  const R = Math.max(3, HEX_SZ * 0.13);
  c.save();
  c.globalAlpha = 0.45 + 0.5 * wet;
  // Drops flung out one way, some drawn out into streaks.
  const throwA = rnd() * Math.PI * 2;
  c.fillStyle = '#2a1708';
  for (let i = 0, n = 4 + Math.floor(rnd() * 3); i < n; i++) {
    const a = throwA + (rnd() - 0.5) * 1.6, d = R * (1.25 + rnd() * 0.8), s = R * (0.1 + rnd() * 0.12);
    c.beginPath();
    c.ellipse(x + Math.cos(a) * d, y + Math.sin(a) * d * 0.7, s * (1 + rnd() * 1.4), s, a, 0, Math.PI * 2);
    c.fill();
  }
  // The splat, an ink rim and all; it pales as it dries.
  const body = wet > 0.5 ? '#4a2a10' : '#5e4226';
  dshBlob(c, x, y, R, rnd, 13, 0.28, 0.72);
  c.fillStyle = body;
  c.fill();
  c.lineWidth = Math.max(1, R * 0.12);
  c.strokeStyle = '#1a0e05';
  c.stroke();
  // A darker heart, and the wet sheen while it is still wet.
  dshBlob(c, x + R * 0.1, y + R * 0.08, R * 0.55, rnd, 9, 0.3, 0.7);
  c.fillStyle = '#2c1806';
  c.fill();
  if (wet > 0.3) {
    c.globalAlpha *= wet;
    c.fillStyle = '#8a5a2a';
    dshBlob(c, x - R * 0.35, y - R * 0.25, R * 0.28, rnd, 7, 0.25, 0.6);
    c.fill();
    c.fillStyle = '#d8b07a';
    c.fillRect(Math.round(x - R * 0.42), Math.round(y - R * 0.32), Math.max(1, R * 0.12), Math.max(1, R * 0.1));
  }
  c.restore();
  // The first two days it has flies.
  if (age < 2) {
    const t = performance.now(), s = Math.max(1, HEX_SZ * 0.018);
    for (let k = 0; k < 2; k++) {
      const p = t * (0.0021 + k * 0.0009) + k * 3;
      dshFly(c, x + Math.sin(p * 1.7) * R * 1.1 + Math.sin(p * 5.3) * R * 0.15,
                y - R * 0.6 + Math.cos(p * 1.3) * R * 0.5, s, t, k);
    }
  }
}

// renderCharacters, after the pawn: the flies that follow whoever has it,
// and the stink coming off them. px/py/pr: the portrait circle on screen.
// c: the map's context unless a preview passes its own.
function dshOverPawn(pid, px, py, pr, c = ctx) {
  if (!(dshDays[pid] > 0)) return;
  const t = performance.now(), s = Math.max(1.2, pr * 0.07);
  c.save();
  // Two stink lines wavering up off the head.
  c.lineWidth = Math.max(1, pr * 0.06);
  c.lineCap = 'round';
  for (let k = 0; k < 2; k++) {
    const bx = px + (k ? 0.35 : -0.3) * pr, rise = ((t / 1600 + k * 0.5) % 1);
    c.strokeStyle = `rgba(150,140,60,${(0.55 * (1 - rise)).toFixed(2)})`;
    c.beginPath();
    for (let i = 0; i <= 8; i++) {
      const v = i / 8, y = py - pr * (1.05 + v * 0.7 + rise * 0.35);
      const x = bx + Math.sin(v * 7 + t / 260 + k * 2) * pr * 0.1;
      if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
    }
    c.stroke();
  }
  // Three flies on wandering loops round the portrait.
  for (let k = 0; k < 3; k++) {
    const p = t * (0.0017 + k * 0.0006) + k * 2.1 + pid;
    const rr = pr * (1.15 + 0.25 * Math.sin(p * 2.3));
    dshFly(c, px + Math.cos(p) * rr + Math.sin(p * 7.1) * pr * 0.06,
              py + Math.sin(p * 1.3) * rr * 0.8, s, t, k + pid);
  }
  c.restore();
}
