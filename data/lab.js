// ── lab.js ─────────────────────────────────────────────────────────────────
// RUNAWAY SCIENCE in the tunnels (lab.hpp, docs/runaway-science.md): Doctor
// Gaunt's clinic, three rooms off the bunker corridors -- the Sharps Ward,
// the Pantry and the operating theatre -- every one within 5 cells of the
// other two. It is only ever underground: nothing here draws on the surface.
//
// State is worldState.lab (the server's "lab" object, merged in by
// _applyWorldState in network.js): st (0 open / 1 grafted), g (banked: 1 the
// Needlecap, 2 the Brine Heart, 4 the Spine Wires), and the three rooms as
// tunnel cells: nq nr (the ward), jq jr (the pantry), pq pr (the theatre).
// Everyone is told where they are; a room is drawn once its cell is on your
// tunnel map, and whatever holds the next step gets the quests' yellow
// outline (labQuestAt, renderer.js isPOIRenderable).
//
// Two layers: 'lab' under the pawns (the needles, the jars, the table and
// the head that turns) and 'lab_light' over the dark (the fungus glow, the
// brine, the lamp that has burned for sixty years) -- light is light, the
// reasoning vacancy_light uses.
//
// Also here: the log lines for ev lab, for a scene opening, and for the
// points Gaunt takes (labPaid, called by ui-encounter.js). The Cursed Note's
// curses are LAB_CURSES in game-data.js.

const LAB_ITEM_CAP = 72, LAB_ITEM_HEART = 73, LAB_ITEM_WIRES = 74;   // items.cfg
const LAB_PART_NAMES = ['', 'the Sharps Ward', 'the Pantry', 'his table', 'the second table'];
const LAB_PURPLE = '168,90,200';
const LAB_PINK   = '232,110,140';
const LAB_LAMP   = '255,226,170';
const LAB_ART    = 1.5;    // the room drawings, in HEX_SZ: at 1 they filled a third of the room
const LAB_STICKER_HEX = 3.0;   // the ward and theatre stickers' edge, in HEX_SZ (LabArt's 600-unit cell; the hex is 2 across)
const LAB_STICKER_PX  = 640;   // their bitmaps: a 64 px hex at 3x DPR, an 88 px one at 2x

// What Gaunt says over the ceiling speakers the first time each day you
// come within 5 cells of his theatre (ev lab "call").
const LAB_CALL_LINES = [
  'You. Yes, you, with the feet. Come and see me. Bring nothing. I want nothing you can carry.',
  'Visitors. How lovely. Follow the corridor until the needles start.',
  'Do not be frightened of the voice. Be frightened of the rest of me. Come in.',
  'I have not had a visitor since lunch. A long lunch. Come along.',
  'You are walking like somebody with points to spend. I can hear it.',
  'Third door, then the ward, then the pantry, then me. Or any order. I am not going anywhere.',
];
// What he says at his table when you come without all three (ev lab "taunt").
const LAB_TAUNT_TAILS = [
  'I can count, you know. It is all I can do.',
  'I will wait. I am extremely good at waiting.',
  'Off you go. Mind the floor.',
  'Do not look at me like that. I cannot look away.',
];
// What the log says when a choice cost points (labPaid).
const LAB_PAID_LINES = [
  'You cannot remember why you were proud of that.',
  'Somewhere a whole afternoon goes quiet.',
  'He sounds pleased. He sounds fed.',
  'You had done that. Now you have not.',
  'The best part of it is his now.',
  'It leaves a gap the shape of a good day.',
];
const labPick = (a) => a[Math.floor(Math.random() * a.length)];

function labState() { return worldState.lab || null; }
function labMe() { return myId >= 0 ? players[myId] : null; }
function labCarries(id) {
  const p = labMe();
  return !!p && (p.it ?? []).some((t, i) => t === id && (p.iq?.[i] ?? 0) > 0);
}
// [q, r] of room s (0 ward, 1 pantry, 2 theatre), or null.
function labRoom(l, s) {
  const q = [l.nq, l.jq, l.pq][s], r = [l.nr, l.jr, l.pr][s];
  return q >= 0 && r >= 0 ? [q, r] : null;
}
const labKnown = (q, r) => !!tunnelMap[r]?.[q];

// Is (q, r) where the next step waits -- step on it and a scene opens
// (labOnArrival in lab.hpp)? Below only. Same sight rule as a POI.
//   the ward and the pantry, while their part is still in them
//   the theatre while the wires are still in him, and after that only for
//   a survivor carrying all three
function labQuestAt(q, r) {
  const l = labState();
  if (!l || !myDepth || l.pq < 0 || l.st !== 0) return false;
  for (let s = 0; s < 2; s++) {
    const rm = labRoom(l, s);
    if (rm && rm[0] === q && rm[1] === r) return !(l.g & (1 << s));
  }
  if (l.pq !== q || l.pr !== r) return false;
  return !(l.g & 4) || (labCarries(LAB_ITEM_CAP) && labCarries(LAB_ITEM_HEART) && labCarries(LAB_ITEM_WIRES));
}

function labScreen(cam, q, r) {
  const pp = hexToPixel(q, r, HEX_SZ);   // the tunnels are walled: no wrap
  return { x: pp.x + cam.ox, y: pp.y + cam.oy };
}
const labOnScreen = (p, m) => p.x > -m && p.x < cssWidth + m && p.y > -m && p.y < cssHeight + m;
function labHash(a, b) {
  let h = (Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul(b | 0, 0x165667b1)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
  return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
}

// ── The 'lab' layer (under the pawns) ─────────────────────────────────────
function renderLab(cam) {
  const l = labState();
  if (!l || !myDepth || l.pq < 0) return;
  const now = performance.now(), s = HEX_SZ * LAB_ART;
  for (let k = 0; k < 3; k++) {
    const rm = labRoom(l, k);
    if (!rm || !labKnown(rm[0], rm[1])) continue;
    const p = labScreen(cam, rm[0], rm[1]);
    if (!labOnScreen(p, s * 2)) continue;
    if (k === 0) drawLabWard(p.x, p.y, s, now, !(l.g & 1), rm, cam);
    else if (k === 1) drawLabPantry(p.x, p.y, s, now, !(l.g & 2), rm);
    else drawLabTheatre(p.x, p.y, s, now, l.st === 0, !(l.g & 4), cam);
  }
}

// The Sharps Ward: one comic-ink sticker (LabArt.ward, below) -- a bed of
// needles, the pincushion as big as a man's head on its trolley, the SHARPS
// bin running over, his clipboard, his speaker. Every syringe is in up to
// the plunger, point out, an inch of amber in the barrel. While the Needlecap
// is still there, its crown breathes (frames, like the theatre's head) and a
// hair of fungus with a bruise-purple cap grows off every point, drawn live
// so they can lean toward whoever is nearest. Taken: no crown, a torn hole
// where it was, and the hairs hang dead off the points.
function drawLabWard(x, y, s, now, full, rm, cam) {
  const base = LabArt.ward(full);
  if (!base) { drawLabWardFlat(x, y, s, now, full, rm); return; }
  const sz = HEX_SZ * LAB_STICKER_HEX, k = sz / 600, x0 = x - sz / 2, y0 = y - sz / 2;
  ctx.save();
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(base, x0, y0, sz, sz);
  if (full) {
    // which way they lean: toward you if you are down here, harder the
    // closer you are; nobody near (or you standing in it), they only sway
    let lx = 0, ly = 0, pull = 0;
    const me = labMe();
    if (me && myDepth && cam) {
      const mp = labScreen(cam, me.tq | 0, me.tr | 0), dx = mp.x - x, dy = mp.y - y, d = Math.hypot(dx, dy);
      if (d > HEX_SZ * 0.5) { lx = dx / d; ly = dy / d; pull = Math.max(0.3, Math.min(1, 1.5 - d / (HEX_SZ * 5))); }
    }
    const tips = LabArt.wardTips(), breathe = 1 + 0.1 * Math.sin(now / 900);
    const ends = [];
    ctx.beginPath();
    for (let i = 0; i < tips.length; i++) {
      const [tx, ty, ux, uy, h] = tips[i];
      if (h < 0.4) continue;   // not every point has grown one yet
      const sw = 0.3 * Math.sin(now / 1400 + i * 1.7);
      let ex = ux + lx * pull * 1.3 - uy * sw, ey = uy + ly * pull * 1.3 + ux * sw;
      const m = Math.hypot(ex, ey) || 1, L = (10 + 7 * h) * k * breathe;
      ex /= m; ey /= m;
      const ax = x0 + tx * k, ay = y0 + ty * k, bx = ax + ex * L, by = ay + ey * L;
      ctx.moveTo(ax, ay);
      ctx.quadraticCurveTo(ax + ux * L * 0.55, ay + uy * L * 0.55, bx, by);
      ends.push(bx, by, h);
    }
    ctx.lineCap = 'round';
    ctx.strokeStyle = 'rgba(228,220,200,0.8)';
    ctx.lineWidth = Math.max(0.6, 1.1 * k);
    ctx.stroke();
    // the caps: ink, then the bruise, then a wet point on the bigger ones
    const cr = Math.max(0.7, 2.3 * k), ol = Math.max(0.45, 0.8 * k);
    const caps = (grow, col) => {
      ctx.fillStyle = col;
      ctx.beginPath();
      for (let i = 0; i < ends.length; i += 3) {
        const r = cr * (0.75 + 0.5 * ends[i + 2]) * breathe + grow;
        ctx.moveTo(ends[i] + r, ends[i + 1]);
        ctx.arc(ends[i], ends[i + 1], r, 0, Math.PI * 2);
      }
      ctx.fill();
    };
    caps(ol, '#1a1512');
    caps(0, '#8a3f9e');
    if (cr > 1.3) {
      ctx.fillStyle = 'rgba(244,222,252,0.9)';
      ctx.beginPath();
      for (let i = 0; i < ends.length; i += 3) {
        const r = cr * (0.75 + 0.5 * ends[i + 2]);
        ctx.moveTo(ends[i] - r * 0.3, ends[i + 1] - r * 0.35);
        ctx.arc(ends[i] - r * 0.3, ends[i + 1] - r * 0.35, r * 0.3, 0, Math.PI * 2);
      }
      ctx.fill();
    }
    // the crown on the pincushion, very slowly breathing
    const b = 0.5 - 0.5 * Math.cos((now % 3800) / 3800 * Math.PI * 2);
    const f = LabArt.crown(Math.round(b * 4));
    if (f) {
      const c = LabArt.CROWN_BOX;
      ctx.drawImage(f, x0 + c[0] * k, y0 + c[1] * k, c[2] * k, c[3] * k);
    }
  }
  ctx.restore();
}

// The ward as it was first drawn: a burr of hypodermics with purple caps on
// the tips. Only the fallback now (no ink kit).
function drawLabWardFlat(x, y, s, now, full, rm) {
  ctx.save();
  ctx.translate(x, y + s * 0.08);
  const n = 15, R = s * 0.36;
  const me = labMe();
  // they lean toward whoever is nearest -- us, if we are below and close
  const lean = me && myDepth ? Math.atan2((me.tr | 0) - rm[1], (me.tq | 0) - rm[0]) : 0;
  const breathe = full ? 1 + 0.06 * Math.sin(now / 900) : 1;
  ctx.lineCap = 'round';
  for (let i = 0; i < n; i++) {
    const h = labHash(i, rm[0] * 31 + rm[1]);
    let a = (i / n) * Math.PI * 2 + (h - 0.5) * 0.5;
    if (full) a += 0.18 * Math.sin(a - lean) * Math.sin(now / 1400 + i);
    const len = R * (0.7 + 0.35 * h) * breathe;
    const bx = Math.cos(a) * len * 0.45, by = Math.sin(a) * len * 0.45 * 0.8;
    const tx = Math.cos(a) * len, ty = Math.sin(a) * len * 0.8;
    // the barrel, with its inch of amber, then the steel point
    ctx.strokeStyle = 'rgba(160,190,184,0.85)';
    ctx.lineWidth = Math.max(1.5, s * 0.045);
    ctx.beginPath(); ctx.moveTo(Math.cos(a) * s * 0.06, Math.sin(a) * s * 0.05); ctx.lineTo(bx, by); ctx.stroke();
    ctx.strokeStyle = 'rgba(232,168,40,0.9)';
    ctx.lineWidth = Math.max(1, s * 0.025);
    ctx.beginPath(); ctx.moveTo(bx * 0.45, by * 0.45); ctx.lineTo(bx * 0.85, by * 0.85); ctx.stroke();
    ctx.strokeStyle = 'rgba(214,220,210,0.9)';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(bx, by); ctx.lineTo(tx, ty); ctx.stroke();
    if (full && h > 0.3) {
      ctx.fillStyle = `rgba(${LAB_PURPLE},0.95)`;
      ctx.beginPath(); ctx.arc(tx, ty, Math.max(1.4, s * 0.035), 0, Math.PI * 2); ctx.fill();
    }
  }
  // the cushion: grey felt gone to mould
  ctx.fillStyle = full ? 'rgba(84,70,92,0.95)' : 'rgba(70,68,62,0.9)';
  ctx.beginPath(); ctx.ellipse(0, 0, s * 0.13, s * 0.1, 0, 0, Math.PI * 2); ctx.fill();
  if (full) {
    // the crown: three caps, wet on top
    for (let i = 0; i < 3; i++) {
      const cx = (i - 1) * s * 0.09, cy = -s * 0.1 - (i === 1 ? s * 0.05 : 0);
      const cr = s * (i === 1 ? 0.075 : 0.055) * (1 + 0.05 * Math.sin(now / 700 + i));
      ctx.fillStyle = `rgba(${LAB_PURPLE},1)`;
      ctx.beginPath(); ctx.ellipse(cx, cy, cr, cr * 0.7, 0, Math.PI, 0); ctx.fill();
      ctx.fillStyle = 'rgba(240,220,250,0.8)';
      ctx.fillRect(cx - cr * 0.45, cy - cr * 0.45, Math.max(1, s * 0.02), Math.max(1, s * 0.02));
    }
  }
  ctx.restore();
}

// The Pantry: a run of shelving, jars of yellow brine and what floats in it,
// and in the middle one, while it is still there, a heart going once to
// every four of yours.
function drawLabPantry(x, y, s, now, full, rm) {
  ctx.save();
  ctx.translate(x, y + s * 0.05);
  const w = s * 0.13, h = s * 0.2;
  ctx.fillStyle = 'rgba(52,44,38,0.95)';
  ctx.fillRect(-s * 0.42, h * 0.55, s * 0.84, Math.max(2, s * 0.04));   // the shelf
  for (let i = -2; i <= 2; i++) {
    const jx = i * s * 0.16, jy = 0;
    const mid = i === 0;
    const hh = h * (mid ? 1.15 : 0.85 + 0.2 * labHash(i, rm[0]));
    // the glass and the brine
    ctx.fillStyle = mid && !full ? 'rgba(40,52,48,0.85)' : mid ? `rgba(${LAB_PINK},0.55)` : 'rgba(200,170,70,0.55)';
    ctx.beginPath();
    vacRR(jx - w / 2, jy + h * 0.55 - hh, w, hh, w * 0.25);
    ctx.fill();
    ctx.strokeStyle = 'rgba(150,170,160,0.8)';
    ctx.lineWidth = 1;
    ctx.stroke();
    // the waxed lid
    ctx.fillStyle = 'rgba(120,30,24,0.95)';
    ctx.fillRect(jx - w * 0.55, jy + h * 0.55 - hh - Math.max(1.5, s * 0.025), w * 1.1, Math.max(1.5, s * 0.03));
    // something in it: a hand, an eye, a heart
    const cy = jy + h * 0.55 - hh * 0.45;
    if (mid && full) {
      const beat = (now % 3400) < 260 ? 1.18 : 1;   // once to every four of yours
      const r = w * 0.28 * beat;
      ctx.fillStyle = 'rgba(176,36,40,1)';
      ctx.beginPath();
      ctx.arc(jx - r * 0.5, cy - r * 0.2, r * 0.6, 0, Math.PI * 2);
      ctx.arc(jx + r * 0.5, cy - r * 0.2, r * 0.6, 0, Math.PI * 2);
      ctx.moveTo(jx - r * 1.05, cy); ctx.lineTo(jx, cy + r * 1.2); ctx.lineTo(jx + r * 1.05, cy);
      ctx.fill();
    } else if (!mid) {
      ctx.fillStyle = labHash(i, rm[1]) > 0.5 ? 'rgba(210,190,170,0.75)' : 'rgba(240,240,230,0.8)';
      ctx.beginPath(); ctx.arc(jx, cy, w * 0.18, 0, Math.PI * 2); ctx.fill();
    }
  }
  ctx.restore();
}

// The theatre: Gaunt under his sheet on a steel table, propped on his
// pillows with the lamp on him, the loom out of the back of his neck and down
// into the box on the floor -- one comic-ink sticker (LabArt, below), with
// his head drawn over it from a set of frames so it can turn to whoever is
// nearest and keep talking. Wires taken: a torn stub in the box. Grafted: the
// head stops, eyes open, mouth shut. Until the sticker exists (or if the ink
// kit in bonney.js is missing), the old flat drawing.
let labTurn = 0, labTurnAt = 0;
function drawLabTheatre(x, y, s, now, alive, wired, cam) {
  const base = LabArt.room(wired);
  if (!base) { drawLabTheatreFlat(x, y, s, now, alive, wired); return; }
  const sz = HEX_SZ * LAB_STICKER_HEX, k = sz / 600, x0 = x - sz / 2, y0 = y - sz / 2;
  ctx.save();
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(base, x0, y0, sz, sz);
  // The head: toward you if you are down here (a little past you, the way a
  // man stares), a slow search if not. Grafted, it is still for good.
  let frame;
  if (alive) {
    const me = labMe();
    let want = 0.2 * Math.sin(now / 1900);
    if (me && myDepth && cam) {
      const mp = labScreen(cam, me.tq | 0, me.tr | 0);
      want = Math.max(-1, Math.min(1, (mp.x - x) / (HEX_SZ * 2.2) + 0.1 * Math.sin(now / 1900)));
    }
    const dt = Math.min(250, Math.max(0, now - labTurnAt));
    labTurnAt = now;
    labTurn += (want - labTurn) * Math.min(1, dt / 500);
    // talking: phrases with pauses between, a syllable every ~115 ms
    const talking = labHash(Math.floor(now / 2300), 91) < 0.72, h = labHash(Math.floor(now / 115), 93);
    frame = LabArt.head(Math.round(labTurn * 4), !talking || h < 0.3 ? 0 : h < 0.72 ? 1 : 2, false);
  } else frame = LabArt.head(0, 0, true);
  if (frame) {
    const b = LabArt.HEAD_BOX;
    ctx.drawImage(frame, x0 + b[0] * k, y0 + b[1] * k, b[2] * k, b[3] * k);
  }
  // a fly that has found him
  const ft = now / 1000;
  const fx = 352 + 30 * Math.sin(ft * 1.7) + 12 * Math.sin(ft * 4.3 + 1), fy = 268 + 24 * Math.sin(ft * 2.3 + 0.5) + 8 * Math.cos(ft * 5.1);
  const fr = Math.max(0.9, 2.4 * k);
  ctx.fillStyle = 'rgba(210,220,225,0.55)';
  const wb = (now % 60) < 30 ? 1 : 0.6;
  ctx.beginPath(); ctx.ellipse(x0 + fx * k - fr * 0.9, y0 + fy * k - fr * 0.8, fr * 1.1 * wb, fr * 0.6, -0.6, 0, Math.PI * 2); ctx.fill();
  ctx.beginPath(); ctx.ellipse(x0 + fx * k + fr * 0.9, y0 + fy * k - fr * 0.8, fr * 1.1 * wb, fr * 0.6, 0.6, 0, Math.PI * 2); ctx.fill();
  ctx.fillStyle = '#120e0c';
  ctx.beginPath(); ctx.arc(x0 + fx * k, y0 + fy * k, fr, 0, Math.PI * 2); ctx.fill();
  ctx.restore();
}

// The theatre as it was first drawn: a steel table, a sheeted body, a head
// that turns. Only the fallback now.
function drawLabTheatreFlat(x, y, s, now, alive, wired) {
  ctx.save();
  ctx.translate(x, y + s * 0.1);
  // the table
  ctx.fillStyle = 'rgba(110,124,128,0.95)';
  ctx.fillRect(-s * 0.36, -s * 0.06, s * 0.72, s * 0.14);
  ctx.fillStyle = 'rgba(60,68,72,0.95)';
  ctx.fillRect(-s * 0.3, s * 0.08, s * 0.04, s * 0.14);
  ctx.fillRect(s * 0.26, s * 0.08, s * 0.04, s * 0.14);
  // the body under its sheet
  ctx.fillStyle = 'rgba(222,214,190,0.95)';
  ctx.beginPath();
  vacRR(-s * 0.3, -s * 0.13, s * 0.48, s * 0.1, s * 0.04);
  ctx.fill();
  // the head on its pillow, turning
  const turn = alive ? Math.sin(now / 1700) * 0.35 + 0.1 * Math.sin(now / 430) : 0;
  const hx = s * 0.24, hy = -s * 0.09;
  ctx.fillStyle = 'rgba(232,222,200,1)';
  ctx.beginPath(); ctx.ellipse(hx, hy, s * 0.065, s * 0.06, 0, 0, Math.PI * 2); ctx.fill();
  ctx.fillStyle = 'rgba(20,16,14,0.95)';
  const ex = hx + turn * s * 0.04;
  ctx.fillRect(ex - s * 0.025, hy - s * 0.012, Math.max(1, s * 0.016), Math.max(1, s * 0.016));
  ctx.fillRect(ex + s * 0.012, hy - s * 0.012, Math.max(1, s * 0.016), Math.max(1, s * 0.016));
  if (alive) {   // talking
    const m = 0.5 + 0.5 * Math.sin(now / 160);
    ctx.fillRect(ex - s * 0.01, hy + s * 0.022, s * 0.022, Math.max(1, s * 0.012 * m));
  }
  // the loom out of the back of his neck, into the cabinet -- while it is in him
  if (wired) {
    ctx.strokeStyle = 'rgba(196,122,52,0.95)';
    ctx.lineWidth = Math.max(1.5, s * 0.03);
    ctx.beginPath();
    ctx.moveTo(hx + s * 0.05, hy + s * 0.02);
    ctx.quadraticCurveTo(s * 0.42, s * 0.05, s * 0.36, s * 0.22);
    ctx.stroke();
    ctx.fillStyle = 'rgba(40,36,30,0.95)';
    ctx.fillRect(s * 0.3, s * 0.18, s * 0.12, s * 0.12);
    ctx.fillStyle = `rgba(255,150,40,${0.6 + 0.4 * Math.sin(now / 90)})`;
    ctx.fillRect(s * 0.33, s * 0.21, Math.max(1, s * 0.025), Math.max(1, s * 0.025));
  }
  ctx.restore();
}

// ── The 'lab_light' layer (over the dark) ─────────────────────────────────
function renderLabLight(cam) {
  const l = labState();
  if (!l || !myDepth || l.pq < 0) return;
  const now = performance.now(), s = HEX_SZ;
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  for (let k = 0; k < 3; k++) {
    const rm = labRoom(l, k);
    if (!rm || !labKnown(rm[0], rm[1])) continue;
    const p = labScreen(cam, rm[0], rm[1]);
    if (!labOnScreen(p, s * 2)) continue;
    if (k === 0 && !(l.g & 1)) labWardLight(p, now);
    else if (k === 1 && !(l.g & 2)) labGlow({ x: p.x, y: p.y }, LAB_PINK, (now % 3400) < 260 ? 0.24 : 0.14, s * 0.7);
    else if (k === 2) labTheatreLight(p, now, l.st === 0, !(l.g & 4));
  }
  ctx.restore();
}
function labGlow(p, rgb, a, r) {
  const g = ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, r);
  g.addColorStop(0, `rgba(${rgb},${a})`);
  g.addColorStop(1, `rgba(${rgb},0)`);
  ctx.fillStyle = g;
  ctx.beginPath(); ctx.arc(p.x, p.y, r, 0, Math.PI * 2); ctx.fill();
}
// The ward's light while the Needlecap is in it: the crown glows as it
// breathes, and the whole room a little with it.
function labWardLight(p, now) {
  const sz = HEX_SZ * LAB_STICKER_HEX, k = sz / 600;
  const b = 0.5 - 0.5 * Math.cos((now % 3800) / 3800 * Math.PI * 2);
  const [cu, cv] = LabArt.CROWN;
  labGlow(p, LAB_PURPLE, 0.1 + 0.04 * b, HEX_SZ * 0.9);
  labGlow({ x: p.x - sz / 2 + cu * k, y: p.y - sz / 2 + cv * k }, LAB_PURPLE, 0.16 + 0.1 * b, HEX_SZ * 0.42);
}

// The theatre's lights, in LabArt's cell: the lamp that has burned for sixty
// years, coning down onto his face; the box's lamp, blinking while the loom
// is in him; and once it has been torn out, the stub spitting now and then.
function labTheatreLight(p, now, alive, wired) {
  const sz = HEX_SZ * LAB_STICKER_HEX, k = sz / 600, x0 = p.x - sz / 2, y0 = p.y - sz / 2;
  const at = (u, v) => ({ x: x0 + u * k, y: y0 + v * k });
  const fl = 0.9 + 0.1 * Math.sin(now / 2300) * Math.sin(now / 170 + 0.7);
  const [bu, bv] = LabArt.BULB, b = at(bu, bv), f = at(bu, bv + 120);
  const cone = ctx.createLinearGradient(b.x, b.y, f.x, f.y);
  cone.addColorStop(0, `rgba(${LAB_LAMP},${(0.2 * fl).toFixed(3)})`);
  cone.addColorStop(1, `rgba(${LAB_LAMP},0)`);
  ctx.fillStyle = cone;
  ctx.beginPath();
  const c0 = at(bu - 22, bv - 2), c1 = at(bu + 22, bv - 2), c2 = at(bu + 62, bv + 120), c3 = at(bu - 58, bv + 120);
  ctx.moveTo(c0.x, c0.y); ctx.lineTo(c1.x, c1.y); ctx.lineTo(c2.x, c2.y); ctx.lineTo(c3.x, c3.y);
  ctx.closePath(); ctx.fill();
  labGlow(b, LAB_LAMP, 0.32 * fl, HEX_SZ * 0.32);
  labGlow(at(bu - 30, bv + 70), LAB_LAMP, 0.1 * fl, HEX_SZ * 0.95);
  if (alive && wired && (now % 1400) < 700) {
    const q = at(...LabArt.BOX_LAMP);
    labGlow(q, '255,70,40', 0.6, Math.max(3, HEX_SZ * 0.09));
    ctx.fillStyle = 'rgba(255,150,110,0.9)';
    ctx.beginPath(); ctx.arc(q.x, q.y, Math.max(0.8, 2.6 * k), 0, Math.PI * 2); ctx.fill();
  }
  if (alive && !wired && (now % 2900) < 170) {
    const n = Math.floor(now / 2900), q = at(...LabArt.STUB);
    labGlow(q, '255,210,140', 0.55, HEX_SZ * 0.16);
    ctx.strokeStyle = 'rgba(255,230,170,0.95)';
    ctx.lineWidth = Math.max(0.8, 1.6 * k);
    ctx.beginPath();
    for (let i = 0; i < 6; i++) {
      const a = -Math.PI / 2 + (labHash(n, i) - 0.5) * 2.6, r0 = 3 * k, r1 = (8 + 16 * labHash(i, n + 7)) * k;
      ctx.moveTo(q.x + Math.cos(a) * r0, q.y + Math.sin(a) * r0);
      ctx.lineTo(q.x + Math.cos(a) * r1, q.y + Math.sin(a) * r1);
    }
    ctx.stroke();
  }
}

// ── Log lines ────────────────────────────────────────────────────────────────
// A scene opened (ev enc_start "lab", network.js).
function labOnEncStart(ev) {
  const who = ev.pid === myId ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  const WHERE = ['', 'opened the door on the Sharps Ward', 'went down into Gaunt’s pantry',
                 'stood over a man who is only a head', 'lay down on the second table'];
  addLog(`<span class="log-col">✚ ${escHtml(who)} ${WHERE[ev.lab] || 'went into Gaunt’s clinic'}</span>`);
}

// A choice that cost points just went through (ui-encounter.js).
function labPaid(n, part) {
  const who = part ? 'Gaunt' : 'It';
  addLog(`<span class="log-check-fail">✚ ${who} takes ${n | 0} points off you. ${labPick(LAB_PAID_LINES)}</span>`);
}

function _evLab(ev) {
  const mine = ev.pid === myId;
  const who = mine ? 'You' : (players[ev.pid]?.nm || `P${ev.pid}`);
  switch (ev.w) {
    case 'call': {
      const line = labPick(LAB_CALL_LINES);
      addLog(`<span class="log-mv">✚ A ceiling speaker crackles, ${ev.d | 0} cells down the tunnel: ‘${escHtml(line)}’</span>`);
      showToast(`✚ ‘${line}’`, 'lab');
      break;
    }
    case 'taunt': {
      const has = ev.has | 0;
      const want = [];
      if (!(has & 1)) want.push('my mushrooms');
      if (!(has & 2)) want.push('my heart');
      if (!(has & 4)) want.push('my wires');
      const list = want.length > 1 ? `${want.slice(0, -1).join(', ')} and ${want[want.length - 1]}` : want[0] || 'everything';
      const line = `You have come without ${list}. ${labPick(LAB_TAUNT_TAILS)}`;
      addLog(`<span class="log-mv">✚ Gaunt, from his pillow: ‘${escHtml(line)}’</span>`);
      showToast(`✚ ‘${line}’`, 'lab');
      break;
    }
    case 'needle':
      addLog(`<span class="log-col">✚ ${escHtml(who)} came out of the Sharps Ward with a fist of needles, breathing.</span>`);
      if (!mine) showToast('✚ Somebody has the Needlecap. The ward has stopped moving.', 'lab');
      break;
    case 'jar':
      addLog(`<span class="log-col">✚ ${escHtml(who)} took a heart off Gaunt’s top shelf. It is still going.</span>`);
      if (!mine) showToast('✚ Somebody has the Brine Heart. The jar is empty.', 'lab');
      break;
    case 'wires':
      addLog(`<span class="log-col">✚ ${escHtml(who)} pulled the wires out of Gaunt’s neck. He kept talking.</span>`);
      if (!mine) showToast('✚ Somebody has the Spine Wires. Gaunt wants his mushrooms and his heart.', 'lab');
      break;
    case 'graft':
      addLog(`<span class="log-col">✚ ${escHtml(who)} got up off the second table with two hearts. Gaunt is still, for the first time in sixty years.</span>`);
      if (mine) addLog('<span class="log-check-fail">✚ There is a folded note in your pocket that was not there before. It is warm.</span>');
      showBanner('RUNAWAY SCIENCE', mine ? 'two hearts, and a note' : `${escHtml(who)} has two hearts`);
      break;
    case 'lost':
      addLog(`<span class="log-mv">✚ Somewhere below, ${LAB_PART_NAMES[ev.part | 0] || 'Gaunt’s clinic'} has grown back what it lost.</span>`);
      break;
  }
}

// ── LabArt: the ward and the theatre in comic ink ─────────────────────────
// Drawn with bonney.js's ink kit (BonneyArt.kit: textured, shaded from the
// upper left, hatched in shadow, inked heavier on the shadow side) into a
// 600-unit cell, LAB_STICKER_HEX x HEX_SZ wide and centred on the room.
// room(wired) is everything but his head, once per state; head(i, mouth,
// still) is one frame of the head, i = -4..4 across his turn, mouth 0 shut /
// 1 ajar / 2 open, or the still one after the Graft. ward(full) is the Sharps
// Ward, once per state, and wardTips() where its needle points are (the live
// hairs grow from them); crown(i) is one breath of the Needlecap, 0..4.
// Built on first need and kept; a canvas that came back blank (a hidden
// page) is tried again.
const LabArt = (() => {
  const PX = LAB_STICKER_PX;
  let K = null;
  const kit = () => K || (K = typeof BonneyArt !== 'undefined' && BonneyArt.kit ? BonneyArt.kit() : null);
  const C = {
    steel: '#8d9493', steelL: '#aeb3ac', steelD: '#5a6264',
    green: '#5f6e5c', greenD: '#3c473b', olive: '#5e6448',
    sheet: '#d6cdb2', fold: '#6e624a', pillow: '#c4b486',
    skin: '#d5caa8', skinD: '#9d9174', skinDD: '#5e5442', dead: '#a6a48a', nail: '#cbb25c',
    socket: '#3e2c34', lip: '#7c4a46', eyeW: '#e8ddbc', iris: '#1e1610',
    blood: 'rgba(110,34,22,0.85)', bloodD: '#3c120c', brine: '#c8a838', mustard: '#d4b03a',
    leather: '#4b3a2b', brass: '#a8853e', manila: '#cbbf9c', rubber: '#6a2a20',
    wireR: '#b43620', wireY: '#d6aa34', copper: '#c47a34', tape: '#8a8e86', wood: '#7a5636',
    glass: 'rgba(205,215,200,0.16)', dark: '#15130f',
  };
  const HEAD = { x: 372, y: 250, rx: 40, ry: 46 };
  const HEAD_BOX = [314, 190, 114, 114];   // x, y, w, h in cell units: what one head frame covers

  // An oblique box: front face x..x+w with its foot at y, h tall; d deep,
  // running back up and to the right. Corners wander a little.
  function obox(g, x, y, w, h, d, o) {
    const { shape, poly, J } = K;
    const ox = 0.55 * d, oy = -0.85 * d, j = () => J(0.6);
    const a = [x + j(), y + j()], b = [x + w + j(), y + j()], c = [x + w + j(), y - h + j()], e = [x + j(), y - h + j()];
    const c2 = [c[0] + ox, c[1] + oy], e2 = [e[0] + ox, e[1] + oy], b2 = [b[0] + ox, b[1] + oy];
    const lw = o.lw ?? 1.8;
    shape(g, poly([b, b2, c2, c]), { fill: o.side || o.fill, tex: o.tex, texA: o.texA, tone: 0.8, lw, ang: -1.5 });
    shape(g, poly([e, c, c2, e2]), { fill: o.top || o.fill, tex: o.tex, texA: o.texA, tone: 0.15, lw, hatch: false });
    shape(g, poly([a, b, c, e]), { fill: o.front || o.fill, tex: o.tex, texA: o.texA, tone: o.tone ?? 0.45, lw, ang: -1.5 });
  }

  // Catmull-Rom through pts, n samples a span.
  function spline(pts, n) {
    const out = [];
    for (let i = 0; i < pts.length - 1; i++) {
      const p0 = pts[Math.max(0, i - 1)], p1 = pts[i], p2 = pts[i + 1], p3 = pts[Math.min(pts.length - 1, i + 2)];
      for (let s = 0; s < n; s++) {
        const t = s / n, t2 = t * t, t3 = t2 * t;
        const f = (a, b, c, d) => 0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
        out.push([f(p0[0], p1[0], p2[0], p3[0]), f(p0[1], p1[1], p2[1], p3[1])]);
      }
    }
    out.push(pts[pts.length - 1]);
    return out;
  }

  // The loom: a fat braid of red and yellow over copper, taped where it was
  // mended.
  function loom(g, pts, w, mends = true) {
    const { taper, line, shape, poly, INK } = K;
    taper(g, pts, w + 3.4, INK, 'none');
    taper(g, pts, w, C.copper, 'none');
    const sm = spline(pts, 14);
    let acc = 0, n = 0;
    for (let i = 1; i < sm.length; i++) {
      const [ax, ay] = sm[i - 1], [bx, by] = sm[i], L = Math.hypot(bx - ax, by - ay) || 1;
      acc += L;
      if (acc < 3.3) continue;
      acc = 0; n++;
      const ux = (bx - ax) / L, uy = (by - ay) / L, nx = -uy, ny = ux, h = w / 2 - 0.5;
      line(g, `M${bx + nx * h - ux * 2.4},${by + ny * h - uy * 2.4} L${bx - nx * h + ux * 2.4},${by - ny * h + uy * 2.4}`, 2.2, n % 2 ? C.wireR : C.wireY);
      line(g, `M${bx + nx * h - ux * 0.4},${by + ny * h - uy * 0.4} L${bx - nx * h + ux * 4.2},${by - ny * h + uy * 4.2}`, 0.7, 'rgba(26,21,18,0.75)');
    }
    taper(g, pts.map(([x, y]) => [x - w * 0.18, y - w * 0.2]), w * 0.24, 'rgba(255,238,200,0.4)');
    if (!mends) return;
    for (const f of [0.3, 0.68]) {
      const i = Math.floor(f * (sm.length - 2)), [ax, ay] = sm[i], [bx, by] = sm[i + 1], L = Math.hypot(bx - ax, by - ay) || 1;
      const ux = (bx - ax) / L, uy = (by - ay) / L, nx = -uy * (w / 2 + 1.2), ny = ux * (w / 2 + 1.2);
      shape(g, poly([[ax + nx - ux * 3, ay + ny - uy * 3], [ax + nx + ux * 3, ay + ny + uy * 3], [ax - nx + ux * 3, ay - ny + uy * 3], [ax - nx - ux * 3, ay - ny - uy * 3]]),
        { fill: C.tape, tex: 'grime', texA: 1, tone: 0.35, lw: 1.1, hatch: false, rim: false });
    }
  }
  // Where a wire has been torn: a fan of bare copper.
  function frayed(g, x, y, a, n = 7) {
    const { line, rnd } = K;
    for (let i = 0; i < n; i++) {
      const t = a + (rnd() - 0.5) * 1.3, L = 4 + rnd() * 6;
      line(g, `M${x},${y} q${Math.cos(t) * L * 0.5 + (rnd() - 0.5) * 2},${Math.sin(t) * L * 0.5} ${Math.cos(t) * L},${Math.sin(t) * L}`, 0.9, i % 3 ? C.copper : '#e8b070');
    }
  }

  // ── Everything but the head ───────────────────────────────────────────────
  function drawRoom(g, wired) {
    const { seed, rnd, J, poly, shape, taper, line, ellD, lumpy, halftone, fly, cyl, jar, INK } = K;
    seed(4107);
    // the floor: his shadow, and what the drain lets go of
    halftone(g, 298, 416, 150, 26);
    shape(g, lumpy(182, 418, 17, 0.34, 16, 0.3), { fill: C.bloodD, tex: 'grime', texA: 0.7, tone: 0.1, lw: 1.2, hatch: false, rim: false });
    taper(g, [[174, 416], [183, 413], [192, 416]], 1.4, 'rgba(255,200,170,0.3)');

    // ── the specimen cabinet against the back wall, and the brain on top
    obox(g, 226, 300, 88, 118, 14, { fill: C.green, tex: 'rust', texA: 0.55 });
    shape(g, 'M232,188 L308,188 L308,298 L232,298 Z', { fill: C.dark, tex: 'grime', texA: 0.6, tone: 0.5, lw: 1.4, hatch: false, rim: false });
    for (const y of [216, 244, 272]) { line(g, `M232,${y} L308,${y}`, 2.6, INK); line(g, `M232,${y - 0.8} L308,${y - 0.8}`, 1, C.steelD); }
    cyl(g, 240, 215, 4, 15, { fill: '#5e3010', tone: 0.4, lw: 1.2 });
    cyl(g, 250, 215, 3.4, 19, { fill: '#34443e', tone: 0.4, lw: 1.2 });
    shape(g, lumpy(268, 205, 8.5, 1, 16, 0.08), { fill: '#d8cdb0', tex: 'grime', tone: 0.5, lw: 1.3, hatch: false });   // a skull
    shape(g, ellD(265, 205, 2.4, 2.6), { fill: INK, tone: 0, lw: 0, rim: false, hatch: false });
    shape(g, ellD(272, 205, 2.4, 2.6), { fill: INK, tone: 0, lw: 0, rim: false, hatch: false });
    line(g, 'M264,211 L273,211', 1, INK);
    jar(g, 292, 215, 7, 18, { fill: C.brine, level: 0.8, inside: (gg, x, y, r, h) => {   // an eye
      shape(gg, ellD(x, y - h * 0.42, r * 0.55, r * 0.55), { fill: '#efe8d4', tone: 0.3, lw: 1, hatch: false, rim: false });
      shape(gg, ellD(x + 1, y - h * 0.42, r * 0.24, r * 0.24), { fill: '#3c6a8a', tone: 0, lw: 0, hatch: false, rim: false });
    } });
    jar(g, 244, 243, 7.5, 20, { fill: C.brine, level: 0.75, cork: true, inside: (gg, x, y, r, h) =>   // a hand, fingers up
      taper(gg, [[x - 2, y - 2], [x - 1, y - h * 0.4], [x + 1, y - h * 0.6]], 4, '#b0a080') });
    for (let i = 0; i < 5; i++) cyl(g, 262 + i * 8, 243, 2.6, 8 + rnd() * 6, { fill: i % 2 ? '#7a6a3a' : '#4e5a50', tone: 0.4, lw: 1 });
    jar(g, 300, 243, 6, 17, { fill: '#8a9a4a', level: 0.6 });
    // glass: the doors' split, glare, a crack
    line(g, 'M270,188 L270,298', 1.6, INK);
    for (const [a, b] of [[[236, 232], [252, 194]], [[276, 262], [298, 214]], [[240, 292], [262, 246]]]) taper(g, [a, b], 2, 'rgba(230,240,230,0.22)');
    line(g, 'M286,230 l6,-7 M286,230 l-5,-4 M286,230 l3,8 M286,230 l7,2', 0.8, 'rgba(220,230,220,0.6)');
    shape(g, ellD(266, 244, 1.6, 3), { fill: C.brass, tone: 0.2, lw: 0.8, hatch: false, rim: false });
    shape(g, ellD(274, 244, 1.6, 3), { fill: C.brass, tone: 0.2, lw: 0.8, hatch: false, rim: false });
    // on top: ledgers, and a brain under a bell jar
    obox(g, 234, 178, 30, 5, 10, { fill: '#5a2a24', tex: 'leather', texA: 1, lw: 1.2 });
    obox(g, 236, 173, 26, 4, 9, { fill: '#6a5034', tex: 'leather', texA: 1, lw: 1.2 });
    shape(g, ellD(294, 176, 13, 3.6), { fill: C.steelD, tex: 'metal', tone: 0.4, lw: 1.3, hatch: false });
    shape(g, lumpy(294, 168, 9, 0.75, 16, 0.12), { fill: '#b48a86', tex: 'skin', texA: 0.5, tone: 0.5, lw: 1.3, hatch: false });
    line(g, 'M287,167 q3,-3 5,0 t5,0 M289,171 q3,-2 6,0 t5,-1 M294,162 l0,10', 0.8, '#6a3e3c');
    shape(g, 'M282,176 L282,160 C282,148 306,148 306,160 L306,176 Z', { fill: C.glass, tone: 0.15, lw: 1.5, hatch: false, rim: false });
    taper(g, [[285, 172], [285, 158], [290, 152]], 1.6, 'rgba(255,255,250,0.55)');

    // ── the drip stand at his feet, a bottle of brine hung upside down
    line(g, 'M206,178 L206,300', 4.2, INK); line(g, 'M206,178 L206,300', 2.2, C.steel);
    line(g, 'M194,178 L218,178', 3.6, INK); line(g, 'M194,178 L218,178', 1.8, C.steelL);
    line(g, 'M206,178 L206,186', 1, INK);
    const BOT = 'M196,192 C196,185 216,185 216,192 L216,214 C216,220 210,222 209,227 L203,227 C202,222 196,220 196,214 Z';
    g.save(); g.clip(new Path2D(BOT));
    shape(g, 'M190,198 L222,198 L222,232 L190,232 Z', { fill: C.brine, tex: 'grime', texA: 0.5, tone: 0.5, lw: 0, rim: false, hatch: false });
    g.restore();
    shape(g, BOT, { fill: C.glass, tone: 0.18, lw: 1.6, hatch: false, rim: false });
    taper(g, [[199, 190], [199, 214]], 1.8, 'rgba(255,255,250,0.6)');
    shape(g, poly([[203, 202], [214, 201], [214, 210], [203, 211]]), { fill: C.manila, tex: 'grime', texA: 0.9, tone: 0.2, lw: 0.9, hatch: false, rim: false });
    line(g, 'M205,205 l7,0 M205,208 l5,0', 0.7, '#3a2c20');
    shape(g, poly([[202, 227], [210, 227], [210, 233], [202, 233]]), { fill: C.rubber, tone: 0.3, lw: 1.1, hatch: false });
    shape(g, poly([[203, 233], [209, 233], [209, 246], [203, 246]]), { fill: C.glass, tone: 0.1, lw: 1.1, hatch: false, rim: false });
    shape(g, ellD(206, 242, 1.4, 2), { fill: C.brine, tone: 0, lw: 0.5, hatch: false, rim: false });

    // ── the lamp on its stand behind his pillows, turned down on his face
    line(g, 'M338,300 L338,158', 4.6, INK); line(g, 'M338,300 L338,158', 2.4, C.steelD);
    line(g, 'M338,160 Q346,150 360,160', 4.6, INK); line(g, 'M338,160 Q346,150 360,160', 2.4, C.steelD);
    shape(g, ellD(338, 159, 3.6, 3.6), { fill: C.steel, tone: 0.3, lw: 1.2, hatch: false });
    shape(g, 'M348,184 C348,166 358,159 372,159 C386,159 396,166 396,184 Z', { fill: C.green, tex: 'metal', texA: 1, tone: 0.6, lw: 2.2, ang: -1.1 });
    for (const [x, y] of [[362, 170], [384, 175]]) shape(g, lumpy(x, y, 2.6, 0.7, 10, 0.3), { fill: 'rgba(30,26,20,0.5)', tone: 0, lw: 0, hatch: false, rim: false });
    taper(g, [[388, 172], [390, 180], [389, 186]], 1.6, 'rgba(142,74,38,0.8)');
    shape(g, ellD(372, 184, 24, 5.5), { fill: C.greenD, tone: 0.3, lw: 1.6, hatch: false });
    shape(g, ellD(372, 185.5, 20, 3.8), { fill: '#ffd890', tone: 0, lw: 0.8, hatch: false, rim: false });
    shape(g, ellD(372, 187.5, 6, 3.6), { fill: '#fff6d8', tone: 0, lw: 1, hatch: false, rim: false });
    cyl(g, 372, 161, 4, 5, { fill: C.steelD, tone: 0.4, lw: 1.2 });

    // ── what is left behind his neck once the loom has been pulled
    if (!wired) {
      for (const [y, a] of [[284, -0.3], [290, 0.1], [296, 0.4]]) {
        loom(g, [[396, y + 2], [410, y], [419, y - 2 + a * 4]], 4.2, false);
        frayed(g, 420, y - 2 + a * 4, a, 4);
      }
    }

    // ── the table: the pedestal, the pump, the slab
    shape(g, ellD(292, 408, 50, 11), { fill: C.steelD, tex: 'rust', texA: 0.9, tone: 0.5, lw: 2, hatch: false });
    cyl(g, 292, 406, 15, 56, { fill: C.steel, tex: 'rust', texA: 0.6, tone: 0.55 });
    for (const y of [366, 380, 394]) line(g, `M278,${y} Q292,${y + 5} 306,${y}`, 1, 'rgba(26,21,18,0.6)');
    shape(g, poly([[306, 398], [328, 391], [330, 396], [308, 403]]), { fill: C.steelD, tex: 'rust', tone: 0.4, lw: 1.4, hatch: false });
    obox(g, 158, 352, 252, 15, 46, { fill: C.steel, top: C.steelL, side: C.steelD, tex: 'metal', texA: 1 });
    line(g, 'M162,335.5 L408,335.5', 1.1, 'rgba(255,248,226,0.4)');
    for (let x = 168; x < 404; x += 21) shape(g, ellD(x + J(1), 344, 1.4, 1.4), { fill: C.steelL, tone: 0.2, lw: 0.8, hatch: false, rim: false });
    for (const [x, r] of [[196, 8], [356, 6]]) shape(g, lumpy(x, 346, r, 0.6, 12, 0.35), { fill: 'rgba(142,74,38,0.55)', tex: 'rust', texA: 1, tone: 0, lw: 0, hatch: false, rim: false });
    line(g, 'M164,346 L154,354', 4.4, INK); line(g, 'M164,346 L154,354', 2.4, C.steelD);   // the drain
    taper(g, [[154, 356], [158, 380], [170, 410]], 1.8, 'rgba(110,34,22,0.7)', 'start');

    // ── the pillows, gone yellow
    shape(g, lumpy(388, 284, 40, 0.48, 20, 0.12), { fill: C.pillow, tex: 'cloth', texA: 1, tone: 0.55 });
    shape(g, lumpy(380, 299, 41, 0.42, 20, 0.12), { fill: C.pillow, tex: 'cloth', texA: 1, tone: 0.45 });
    shape(g, lumpy(404, 296, 9, 0.6, 14, 0.35), { fill: 'rgba(120,84,40,0.45)', tone: 0, lw: 0, hatch: false, rim: false });
    if (!wired) taper(g, [[408, 290], [412, 298], [410, 308]], 3, 'rgba(70,18,12,0.85)', 'end');

    // ── his neck
    shape(g, 'M356,284 C358,294 354,302 350,310 L396,310 C392,302 388,294 390,284 Z', { fill: C.skinD, tex: 'skin', texA: 0.6, tone: 0.65, lw: 2 });
    taper(g, [[364, 288], [362, 298], [358, 308]], 1.8, C.skinDD);
    taper(g, [[382, 288], [384, 298], [388, 308]], 1.8, C.skinDD);
    shape(g, lumpy(373, 298, 4, 1, 10, 0.1), { fill: C.skinD, tone: 0.5, lw: 1, hatch: false });

    // ── his feet, out of the end of the sheet, and the tag on his toe
    const foot = (dx, dy) => {
      shape(g, `M${206 + dx},${306 + dy} C${198 + dx},${306 + dy} ${192 + dx},${300 + dy} ${191 + dx},${292 + dy} C${190 + dx},${284 + dy} ${192 + dx},${277 + dy} ${196 + dx},${275 + dy} C${200 + dx},${274 + dy} ${203 + dx},${278 + dy} ${203 + dx},${284 + dy} C${204 + dx},${292 + dy} ${206 + dx},${296 + dy} ${208 + dx},${298 + dy} Z`,
        { fill: C.dead, tex: 'skin', texA: 0.8, tone: 0.5, lw: 1.8 });
      shape(g, ellD(196.5 + dx, 276.5 + dy, 2.6, 1.6, -0.3), { fill: C.nail, tone: 0.2, lw: 0.8, hatch: false, rim: false });
    };
    foot(0, 0); foot(-7, 8);
    line(g, 'M190,285 Q181,292 178,301', 0.9, '#d8d0b8');
    shape(g, poly([[168, 300], [184, 298], [186, 316], [170, 318]]), { fill: C.manila, tex: 'grime', texA: 0.9, tone: 0.3, lw: 1.2, hatch: false });
    shape(g, ellD(176.5, 302.5, 1.4, 1.4), { fill: INK, tone: 0, lw: 0, hatch: false, rim: false });
    for (let i = 0; i < 3; i++) taper(g, [[171, 307 + i * 3.5], [177, 306.5 + i * 3.5 + J(0.8)], [183, 306 + i * 3.5]], 0.9, i === 0 ? '#8a2418' : '#3a2c20', 'none');

    // ── the sheet over what is left of him: knees, a sunken belly, the chest
    let hem = '';
    for (let x = 400; x >= 196; x -= 7) hem += ` L${x},${(358 + rnd() * 6).toFixed(1)}`;
    const SHEET = 'M198,322 L199,300 C206,295 222,297 236,298 C246,294 252,284 262,284 C272,284 278,292 292,293 C304,293 312,286 324,287 C334,288 338,297 346,296 C354,292 368,290 384,292 C392,293 398,296 401,300 L402,330' + hem + ' Z';
    shape(g, SHEET, { fill: C.sheet, tex: 'cloth', texA: 1, tone: 0.5, lw: 2.4 });
    for (const f of [[[262, 286], [258, 322], [262, 358]], [[324, 289], [318, 326], [322, 360]], [[236, 299], [233, 330], [236, 359]], [[292, 294], [294, 330], [289, 358]], [[372, 300], [376, 332], [372, 360]]]) {
      taper(g, f, 2.4, C.fold); taper(g, f.map(([x, y]) => [x - 3, y]), 1.3, 'rgba(255,248,226,0.4)');
    }
    taper(g, [[200, 338], [300, 341], [400, 338]], 1.3, 'rgba(60,50,36,0.5)');
    shape(g, 'M344,296 C356,291 372,289 386,292 C394,294 399,297 402,302 L402,310 C390,304 370,302 344,307 Z', { fill: '#e4dcc4', tex: 'cloth', texA: 0.8, tone: 0.3, lw: 1.6, hatch: false });
    // what has come through it
    shape(g, lumpy(252, 328, 11, 0.7, 16, 0.3), { fill: 'rgba(150,118,52,0.35)', tone: 0, lw: 0, hatch: false, rim: false });
    shape(g, lumpy(334, 318, 13, 0.8, 18, 0.32), { fill: C.blood, tex: 'grime', texA: 0.8, tone: 0.3, lw: 0, hatch: false, rim: false });
    shape(g, lumpy(333, 317, 6, 0.8, 12, 0.3), { fill: 'rgba(60,14,8,0.85)', tone: 0, lw: 0, hatch: false, rim: false });
    taper(g, [[334, 326], [337, 344], [334, 360]], 4.2, C.blood, 'end');
    // the straps
    for (const [x0, x1] of [[226, 239], [312, 325]]) {
      shape(g, poly([[x0, 298 + J(1)], [x0 + 12, 298 + J(1)], [x1 - 1, 362], [x1 - 13, 362]]), { fill: C.leather, tex: 'leather', texA: 1, tone: 0.5, lw: 1.6, hatch: false });
      const bx = (x0 + x1) / 2 - 1;
      shape(g, poly([[bx - 6, 324], [bx + 6, 324], [bx + 6, 334], [bx - 6, 334]]), { fill: C.brass, tex: 'rust', texA: 0.5, tone: 0.3, lw: 1.3, hatch: false });
      shape(g, poly([[bx - 3.4, 326.6], [bx + 3.4, 326.6], [bx + 3.4, 331.4], [bx - 3.4, 331.4]]), { fill: C.leather, tone: 0, lw: 0.8, hatch: false, rim: false });
    }

    // ── the drip line, taped down where it goes in under the sheet
    line(g, 'M206,246 C206,280 214,300 228,318 C234,326 240,330 246,332', 2.8, INK);
    line(g, 'M206,246 C206,280 214,300 228,318 C234,326 240,330 246,332', 1.4, 'rgba(232,222,170,0.9)');
    shape(g, poly([[238, 324], [250, 327], [248, 334], [236, 331]]), { fill: C.tape, tex: 'grime', texA: 1, tone: 0.3, lw: 1, hatch: false, rim: false });
    // ── and a hand hanging out from under it, the blood running off a finger
    const HX0 = 340;
    for (const [dx, len] of [[1, 12], [5.5, 15], [10, 14], [14.5, 10]]) {
      const x = HX0 + dx;
      taper(g, [[x, 382], [x - 0.6, 382 + len * 0.6], [x + 0.6, 382 + len]], 4.8, INK, 'none');
      taper(g, [[x, 382], [x - 0.6, 382 + len * 0.6], [x + 0.6, 382 + len]], 2.8, C.dead, 'none');
      shape(g, ellD(x + 0.6, 381 + len, 1.4, 1.1), { fill: C.nail, tone: 0, lw: 0.6, hatch: false, rim: false });
    }
    shape(g, `M${HX0},358 L${HX0 + 15},357 L${HX0 + 17},370 C${HX0 + 18},376 ${HX0 + 17},382 ${HX0 + 15},386 L${HX0},386 C${HX0 - 2},380 ${HX0 - 2},374 ${HX0 - 1},368 Z`, { fill: C.dead, tex: 'skin', texA: 0.8, tone: 0.55, lw: 1.7 });
    taper(g, [[HX0 + 6, 358], [HX0 + 5, 372], [HX0 + 6, 384]], 2.2, C.blood, 'end');
    shape(g, `M${HX0 + 6.1},398 C${HX0 + 8},402 ${HX0 + 8},405 ${HX0 + 6.1},405 C${HX0 + 4.2},405 ${HX0 + 4.2},402 ${HX0 + 6.1},398 Z`, { fill: '#7a2416', tone: 0, lw: 0.8, hatch: false, rim: false });

    // ── the box on the floor his wires go down into: a dial, a lamp, a knife switch
    obox(g, 374, 414, 44, 36, 16, { fill: C.olive, tex: 'rust', texA: 0.85 });
    shape(g, ellD(388, 395, 7.6, 7.6), { fill: '#d8cfb0', tex: 'grime', texA: 0.8, tone: 0.3, lw: 1.5, hatch: false });
    for (let i = 0; i < 5; i++) { const a = Math.PI * (1.15 + i * 0.175); line(g, `M${388 + Math.cos(a) * 5},${395 + Math.sin(a) * 5} L${388 + Math.cos(a) * 6.6},${395 + Math.sin(a) * 6.6}`, 0.7, INK); }
    line(g, 'M388,395 L393,390', 1, '#8a2418');
    shape(g, ellD(407, 392, 3.4, 3.4), { fill: '#6a1a10', tone: 0.2, lw: 1.2, hatch: false });
    shape(g, poly([[401, 409], [407, 399], [413, 409]]), { fill: C.mustard, tone: 0.1, lw: 0.9, hatch: false, rim: false });
    line(g, 'M407.6,401.5 L405.6,405 L408.4,405 L406.4,408', 0.8, INK);
    for (let i = 0; i < 4; i++) line(g, `M${421 + i * 2.2},${386 - i * 2.5} l0,12`, 0.7, 'rgba(20,16,12,0.7)');
    shape(g, poly([[380, 372], [394, 371], [398, 366], [384, 367]]), { fill: '#2a2420', tone: 0.2, lw: 1, hatch: false });
    for (const dx of [0, 6]) { line(g, `M${383 + dx},370 L${376 + dx},357`, 2.6, INK); line(g, `M${383 + dx},370 L${376 + dx},357`, 1.4, C.copper); }
    line(g, 'M375,357 L383,356', 2.4, INK);
    shape(g, ellD(379, 356, 3, 2.4), { fill: '#141210', tone: 0, lw: 1, hatch: false });
    cyl(g, 406, 372, 6.5, 5, { fill: C.steel, tex: 'rust', texA: 0.7, tone: 0.4, lw: 1.4 });

    // ── the loom out of the back of his neck, over the table's corner, into
    // the box -- or, torn out, a stub of it in the box, frayed
    if (wired) {
      loom(g, [[398, 290], [414, 298], [425, 314], [425, 334], [417, 350], [406, 367]], 9);
      line(g, 'M422,326 C432,330 434,342 430,350', 2.4, INK); line(g, 'M422,326 C432,330 434,342 430,350', 1.2, '#3c6a9a');
      frayed(g, 430, 350, 1.5, 3);
    } else {
      loom(g, [[406, 368], [408, 360], [411, 352]], 9, false);
      frayed(g, 411, 351, -1.4, 9);
    }

    // ── the trolley at the front: a dish, a saw, a blade; a drill below
    for (const x of [199, 261]) { line(g, `M${x},381 L${x},430`, 2.8, INK); line(g, `M${x},381 L${x},430`, 1.3, C.steelD); }
    obox(g, 186, 434, 66, 3, 20, { fill: C.steelD, tex: 'metal', texA: 1, lw: 1.4 });
    line(g, 'M206,428 L212,422 L220,428 L226,422', 2.6, INK); line(g, 'M206,428 L212,422 L220,428 L226,422', 1.2, C.steelL);
    shape(g, ellD(228, 422, 3, 3), { fill: C.wood, tone: 0.3, lw: 1, hatch: false });
    for (const x of [188, 252]) {
      line(g, `M${x},398 L${x},438`, 2.8, INK); line(g, `M${x},398 L${x},438`, 1.3, C.steel);
      shape(g, ellD(x, 440, 3, 2.2), { fill: '#1e1a16', tone: 0, lw: 1, hatch: false, rim: false });
    }
    obox(g, 186, 398, 66, 5, 20, { fill: C.steel, top: C.steelL, tex: 'metal', texA: 1, lw: 1.6 });
    shape(g, 'M193,386 C193,381 203,380 207,383 C210,385 213,382 216,384 C220,387 217,392 209,392 C200,392 193,391 193,386 Z', { fill: C.steelL, tex: 'metal', texA: 1, tone: 0.4, lw: 1.3, hatch: false });
    shape(g, lumpy(205, 386, 3.2, 0.8, 10, 0.3), { fill: '#a83020', tone: 0.2, lw: 0.8, hatch: false, rim: false });
    shape(g, poly([[221, 389], [245, 383], [246, 387], [222, 392]]), { fill: C.steelL, tex: 'metal', tone: 0.3, lw: 1.1, hatch: false });
    for (let i = 0; i < 7; i++) line(g, `M${223 + i * 3.4},${391.6 - i * 0.8} l1.2,1.6`, 0.7, INK);
    shape(g, poly([[245, 380], [255, 378], [257, 386], [247, 388]]), { fill: C.wood, tex: 'leather', tone: 0.3, lw: 1.2, hatch: false });
    line(g, 'M228,380 L242,377.6', 2.6, INK); line(g, 'M228,380 L242,377.6', 1.1, C.steelL);

    // ── flies, at the stain
    fly(g, 318, 302, 0.8, true);
    fly(g, 348, 330, 0.7, false);
  }

  // ── His head: one frame ──────────────────────────────────────────────────
  // t -1..1 across his turn (to the viewer's left .. right). A sphere's worth
  // of longitudes: a feature at phi sits at hx + r sin(phi + th) and faces us
  // by cos(phi + th).
  function drawHead(g, t, mouth, still) {
    const { seed, rnd, J, poly, shape, taper, line, ellD, lumpy, INK } = K;
    seed(5309);
    const { x: hx, y: hy, rx, ry } = HEAD, th = t * 0.6;
    const X = (phi, r = rx) => hx + r * Math.sin(phi + th);
    const V = (phi) => Math.cos(phi + th);
    const ear = (side) => {
      const phi = side * 1.5, ex = X(phi, rx + 3), ew = 3.4 + 6.5 * Math.abs(V(phi));
      shape(g, ellD(ex, hy + 4, ew, 13, side * -0.15), { fill: C.skinD, tex: 'skin', texA: 0.6, tone: 0.55, lw: 1.8, hatch: false });
      taper(g, [[ex - side * ew * 0.2, hy - 4], [ex + side * ew * 0.3, hy + 4], [ex, hy + 12]], 1.6, C.skinDD);
    };
    // the far ear, then the skull: a dome of a cranium over a starved jaw
    const far = th > 0 ? 1 : -1;
    ear(far);
    const pts = [];
    for (let i = 0; i < 44; i++) {
      const a = i / 44 * Math.PI * 2, c = Math.cos(a), s = Math.sin(a);
      pts.push([hx + t * 1.5 + rx * c * (s > 0 ? 1 - 0.3 * s * s : 1 + 0.04 * -s) + J(0.5), hy + ry * s * (s < 0 ? 1.05 : 1) + J(0.5)]);
    }
    shape(g, poly(pts), { fill: still ? '#cbc2a6' : C.skin, tex: 'skin', texA: 0.55, tone: 0.55, lw: 2.6 });
    ear(-far);
    // liver spots, the scar where they had the top off, the staples
    for (const [phi, y, r] of [[-0.7, hy - 30, 3.4], [0.45, hy - 38, 2.6], [1.0, hy - 16, 3], [-1.0, hy - 8, 2.2]]) {
      if (V(phi) > 0.12) shape(g, lumpy(X(phi, rx * 0.9), y, r * (0.4 + 0.6 * V(phi)), 0.8, 10, 0.3), { fill: 'rgba(122,92,52,0.5)', tone: 0, lw: 0, hatch: false, rim: false });
    }
    const scar = [];
    for (let phi = -1.4; phi <= 1.41; phi += 0.14) if (V(phi) > 0.05) scar.push([X(phi, rx * 0.97), hy - 27 - 6 * V(phi)]);
    if (scar.length > 1) {
      taper(g, scar, 2.2, '#6a3a34', 'both');
      scar.forEach(([x, y], i) => { if (i % 2) { line(g, `M${x - 0.6},${y - 4} L${x + 0.6},${y + 4}`, 2.2, INK); line(g, `M${x - 0.6},${y - 3.4} L${x + 0.6},${y + 3.4}`, 1, C.steelL); } });
    }
    // forehead lines
    for (const dy of [-16, -11]) {
      const ln = [];
      for (let phi = -0.6; phi <= 0.61; phi += 0.2) if (V(phi) > 0.2) ln.push([X(phi, rx * 0.97), hy + dy - 2 * V(phi) + J(0.6)]);
      if (ln.length > 1) taper(g, ln, 1.2, 'rgba(70,58,42,0.7)');
    }
    // the head mirror on its band: he is the doctor here
    const band = [];
    for (let phi = -1.5; phi <= 1.51; phi += 0.15) if (V(phi) > 0) band.push([X(phi, rx * 1.01), hy - 20 - 3 * V(phi)]);
    if (band.length > 1) { taper(g, band, 4.4, INK, 'none'); taper(g, band, 2.6, C.leather, 'none'); }
    if (V(-0.5) > 0.15) {
      const mx = X(-0.5, rx * 1.02), my = hy - 25, mw = 9 * Math.max(0.3, V(-0.5));
      shape(g, ellD(mx, my, mw, 9), { fill: '#c6cec8', tex: 'metal', texA: 0.8, tone: 0.35, lw: 1.8, hatch: false });
      shape(g, ellD(mx, my, mw * 0.24, 2.2), { fill: INK, tone: 0, lw: 0, hatch: false, rim: false });
      taper(g, [[mx - mw * 0.55, my - 2], [mx - mw * 0.2, my - 6]], 1.6, 'rgba(255,255,250,0.85)');
    }
    // the brow, then the eyes: sunk deep, and too wide
    for (const side of [-1, 1]) {
      const phi = side * 0.42, v = V(phi);
      if (v < 0.12) continue;
      const ex = X(phi, rx * 0.93), ey = hy - 2, ew = 8.6 * v, eh = 7;
      shape(g, ellD(ex, ey + 1, ew + 3.4, eh + 3), { fill: C.socket, tex: 'skin', texA: 0.4, tone: 0.4, lw: 0, hatch: false, rim: false });
      shape(g, ellD(ex, ey, ew, eh * 0.82), { fill: C.eyeW, tone: 0.25, lw: 1.4, hatch: false, rim: false });
      taper(g, [[ex - ew * 0.9, ey + 0.5], [ex - ew * 0.5, ey - 0.5]], 0.8, 'rgba(170,40,30,0.8)');
      taper(g, [[ex + ew * 0.9, ey + 1], [ex + ew * 0.55, ey + 2]], 0.8, 'rgba(170,40,30,0.8)');
      const pr = still ? 3.2 : 2.3, px = ex + (still ? 0 : t * 3.4 * v), py = ey + 0.6;
      shape(g, ellD(px, py, pr * Math.max(0.5, v), pr), { fill: C.iris, tone: 0, lw: 0, hatch: false, rim: false });
      if (!still) shape(g, ellD(px - 0.9, py - 1, 0.9, 0.9), { fill: '#fff8e0', tone: 0, lw: 0, hatch: false, rim: false });
      taper(g, [[ex - ew * 1.05, ey], [ex - ew * 0.2, ey - eh * 0.9], [ex + ew * 1.05, ey - 0.5]], 2.2, INK);
      taper(g, [[ex - ew * 0.8, ey + eh * 0.95], [ex, ey + eh * 1.35], [ex + ew * 0.8, ey + eh * 0.95]], 1.3, C.skinDD);
      taper(g, [[ex - side * ew * 1.1, ey - eh * 1.25], [ex, ey - eh * 1.55 - side * 1.5], [ex + side * ew * 1.15, ey - eh * 1.15]], 2.6, '#4a4034');
    }
    // the nose, hooked, pointing where he looks
    const nx = X(0, rx * 0.95), tip = nx + th * 8;
    shape(g, `M${nx - 2},${hy - 4} C${nx - 3},${hy + 4} ${tip - 6},${hy + 11} ${tip - 4.5},${hy + 15} C${tip - 2},${hy + 18} ${tip + 3},${hy + 17} ${tip + 5},${hy + 14} C${tip + 4},${hy + 8} ${nx + 3},${hy + 2} ${nx + 2},${hy - 4} Z`,
      { fill: C.skin, tex: 'skin', texA: 0.5, tone: 0.55, lw: 1.5, hatch: false });
    shape(g, ellD(tip - 2.4, hy + 15.4, 1.6, 1), { fill: INK, tone: 0, lw: 0, hatch: false, rim: false });
    shape(g, ellD(tip + 2.4, hy + 15.4, 1.6, 1), { fill: INK, tone: 0, lw: 0, hatch: false, rim: false });
    // sunk cheeks
    for (const side of [-1, 1]) {
      const phi = side * 0.66;
      if (V(phi) > 0.2) taper(g, [[X(phi, rx * 0.9), hy + 4], [X(phi, rx * 0.86) + side * 1.5, hy + 14], [X(phi, rx * 0.8), hy + 24]], 3 * V(phi), 'rgba(60,48,36,0.55)');
    }
    // the mouth: shut, ajar, or going
    const mx = X(0, rx * 0.86), my = hy + 27, mw = 11 * V(0);
    if (mouth === 0) {
      taper(g, [[mx - mw, my + 1.5], [mx, my], [mx + mw, my + 1.5]], 2.4, INK);
      taper(g, [[mx - mw * 0.6, my + 3], [mx, my + 4], [mx + mw * 0.6, my + 3]], 1.6, C.lip);
      line(g, `M${mx - mw},${my + 1.5} l-1.6,2 M${mx + mw},${my + 1.5} l1.6,2`, 1, INK);
    } else {
      const oh = mouth === 1 ? 3.2 : 8;
      shape(g, `M${mx - mw},${my} C${mx - mw * 0.5},${my - 2.5} ${mx + mw * 0.5},${my - 2.5} ${mx + mw},${my} C${mx + mw * 0.6},${my + oh} ${mx - mw * 0.6},${my + oh} ${mx - mw},${my} Z`,
        { fill: '#1a0e0c', tone: 0, lw: 1.8, hatch: false, rim: false });
      for (let i = 0; i < 5; i++) {
        if (i === 1 || i === 4) continue;   // the ones he lost
        const x = mx - mw * 0.6 + i * mw * 0.3, d = mouth === 1 ? 1.4 : 2.4;
        shape(g, poly([[x - 1.2, my - 1.2], [x + 1.2, my - 1.2], [x + 1, my + d], [x - 1, my + d]]), { fill: '#d8cc9e', tone: 0, lw: 0.6, hatch: false, rim: false });
      }
      if (mouth === 2) shape(g, ellD(mx, my + 5.2, mw * 0.4, 1.8), { fill: '#4a2a34', tone: 0, lw: 0, hatch: false, rim: false });
      taper(g, [[mx - mw * 0.6, my + oh * 0.75 + 1.5], [mx, my + oh * 0.75 + 2.6], [mx + mw * 0.6, my + oh * 0.75 + 1.5]], 1.6, C.lip);
    }
    // stubble on the chin, wisps of hair off the sides
    for (let i = 0; i < 16; i++) {
      const phi = (rnd() - 0.5) * 1.4, y = hy + 30 + rnd() * 10;
      if (V(phi) > 0.25) shape(g, ellD(X(phi, rx * 0.72), y, 0.6, 0.6), { fill: 'rgba(40,32,24,0.7)', tone: 0, lw: 0, hatch: false, rim: false });
    }
    for (const [phi, y, dx, dy] of [[-1.35, hy - 14, -12, -14], [-1.25, hy - 6, -15, -4], [1.3, hy - 16, 13, -12], [1.4, hy - 4, 14, 2], [0.9, hy - 38, 8, -12]]) {
      const x = X(phi, rx * 0.98);
      if (V(phi) > -0.5) taper(g, [[x, y], [x + dx * 0.5, y + dy * 0.3 - 3], [x + dx, y + dy]], 1.3, '#d2ccb8');
    }
  }

  // ── The Sharps Ward ───────────────────────────────────────────────────────
  // A syringe pushed into something plunger first, point out: from (x, y) on
  // the surface along (ux, uy), L long on the page, w the barrel's width.
  // full: the amber is bright (the Needlecap is still here). Returns
  // [tip x, tip y, ux, uy].
  const SYR = { glass: '#bccbc4', amber: '#e3a238', dull: '#97784a', hub: '#4e5658', steel: '#e6eae0', flange: '#97a39c' };
  function needle(g, x, y, ux, uy, L, full, w = 5) {
    const { line, INK } = K;
    const n = Math.hypot(ux, uy) || 1;
    ux /= n; uy /= n;
    const nx = -uy, ny = ux, hs = nx + ny > 0 ? -1 : 1;   // which side the light is on
    const P = (t, o = 0) => `${(x + ux * L * t + nx * o).toFixed(1)},${(y + uy * L * t + ny * o).toFixed(1)}`;
    const S = (a, b, o = 0) => `M${P(a, o)} L${P(b, o)}`;
    const fl = `M${P(0.03, w * 0.85)} L${P(0.03, -w * 0.85)}`;
    // all the ink first, then the colours over it
    line(g, fl, 3.6, INK); line(g, S(0.03, 0.17), 2.8, INK); line(g, S(0.15, 0.64), w + 2.4, INK);
    line(g, S(0.63, 0.71), w * 0.55 + 2.2, INK); line(g, S(0.7, 1), 2.4, INK);
    line(g, fl, 1.6, SYR.flange); line(g, S(0.03, 0.17), 1, SYR.flange);
    line(g, S(0.16, 0.63), w, SYR.glass);
    line(g, S(0.41, 0.63), Math.max(1, w - 1.6), full ? SYR.amber : SYR.dull);   // the inch of amber
    line(g, S(0.2, 0.37, hs * w * 0.22), 0.9, 'rgba(255,255,250,0.75)');
    line(g, S(0.46, 0.6, hs * w * 0.2), 0.8, 'rgba(255,238,176,0.75)');
    line(g, S(0.635, 0.705), w * 0.55, SYR.hub);
    line(g, S(0.71, 0.995), 0.95, SYR.steel);
    return [x + ux * L, y + uy * L, ux, uy];
  }
  // A steel tube: ink, then the metal.
  function tube(g, d, w = 3, col = C.steel) { K.line(g, d, w + 2.2, K.INK); K.line(g, d, w, col); }
  function caster(g, x, y) { K.shape(g, K.ellD(x, y, 3.2, 2.4), { fill: '#1e1a16', tone: 0, lw: 1, hatch: false, rim: false }); }
  // A clump of them standing in the floor.
  function clump(g, cx, cy, n, full, tips, spread = 14) {
    const { rnd, J, shape, ellD } = K;
    const pts = [];
    for (let i = 0; i < n; i++) pts.push([cx + J(spread), cy + J(spread * 0.4)]);
    pts.sort((a, b) => a[1] - b[1]);
    for (const [x, y] of pts) {
      shape(g, ellD(x + 1.6, y + 0.8, 4, 1.6), { fill: 'rgba(20,16,12,0.55)', tone: 0, lw: 0, hatch: false, rim: false });
      const a = -Math.PI / 2 + J(0.55) + (x - cx) * 0.025;
      tips.push([...needle(g, x, y, Math.cos(a), Math.sin(a), 17 + rnd() * 11, full, 4.4), rnd()]);
    }
  }

  // The pincushion on the trolley, as big as a man's head: a sphere seen a
  // little from above, so the needles are a 3D burr -- the ones round the back
  // go in before it, the ones toward you after.
  const CUSH = { x: 311, y: 328, r: 27 }, TILT = 0.55;
  const CROWN = [311, 290], CROWN_ROOT = [311, 306];
  const CROWN_BOX = [268, 242, 86, 74];
  const CAPS = [[-24, -2, 5], [25, -4, 5.5], [-17, -15, 6.5], [19, -17, 7], [-9, -31, 8.5], [11, -33, 9], [0, -47, 11]];

  function drawWard(g, full, tips) {
    const { seed, rnd, J, poly, shape, taper, line, ellD, lumpy, halftone, fly, INK } = K;
    seed(7211);
    const ct = Math.cos(TILT), st = Math.sin(TILT);

    // ── the floor: shadows, and what has leaked out of them over the years
    halftone(g, 262, 334, 104, 15);
    halftone(g, 310, 424, 66, 12);
    halftone(g, 214, 428, 32, 7);
    for (const [x, y, r] of [[226, 338, 13], [382, 432, 10], [272, 446, 8]]) shape(g, lumpy(x, y, r, 0.4, 16, 0.3), { fill: 'rgba(196,136,40,0.42)', tex: 'grime', texA: 0.8, tone: 0.1, lw: 0, hatch: false, rim: false });
    // the path somebody cleared on their belly, out to the trolley
    for (const dx of [-7, 0, 6]) taper(g, [[300 + dx, 470], [302 + dx * 0.8, 452], [306 + dx * 0.6, 432]], 5, 'rgba(26,20,14,0.3)');
    taper(g, [[296, 466], [299, 450], [304, 438]], 1.4, 'rgba(196,136,40,0.5)');

    // ── the bed at the back: an iron bedstead, the mattress a bed of needles
    tube(g, 'M359,258 L359,296', 2.6, C.steelD); caster(g, 359, 298);
    obox(g, 166, 296, 168, 16, 46, { fill: '#6c6672', top: '#857e8a', side: '#4e4a56', tex: 'cloth', texA: 1 });
    const E = [166, 280], Cc = [334, 280], E2 = [191.3, 240.9];
    const at = (u, v) => [E[0] + (Cc[0] - E[0]) * u + (E2[0] - E[0]) * v, E[1] + (E2[1] - E[1]) * v];
    for (const v of [0.25, 0.5, 0.75]) { const [ax, ay] = at(0.01, v), [bx, by] = at(0.99, v); line(g, `M${ax},${ay} L${bx},${by}`, 0.9, 'rgba(50,40,60,0.4)'); }
    for (const [u, v, r, col] of [[0.3, 0.5, 15, 'rgba(150,104,40,0.45)'], [0.72, 0.35, 11, 'rgba(110,60,130,0.45)'], [0.55, 0.7, 9, 'rgba(150,104,40,0.35)']]) {
      const [x, y] = at(u, v);
      shape(g, lumpy(x, y, r, 0.42, 16, 0.3), { fill: col, tex: 'grime', texA: 0.8, tone: 0, lw: 0, hatch: false, rim: false });
    }
    tube(g, 'M166,301 L334,301', 2.4, C.steelD);
    for (const x of [171, 329]) { tube(g, `M${x},301 L${x},333`, 2.6); caster(g, x, 335); }
    // the head of it: chipped white enamel gone to rust at every chip
    const ENAMEL = '#c4bfae';
    tube(g, 'M195,244 L195,186', 3.2, ENAMEL);
    for (const t of [0.25, 0.5, 0.75]) tube(g, `M${170 + 25 * t},${280 - 39 * t} L${170 + 25 * t},${214 - 28 * t}`, 1.8, ENAMEL);
    tube(g, 'M170,244 L195,212', 2, ENAMEL);
    tube(g, 'M170,334 L170,214 C171,198 182,186 195,184', 3.2, ENAMEL);
    for (const [x, y] of [[170, 256], [195, 200], [182, 190], [170, 232]]) shape(g, lumpy(x, y, 2, 0.8, 8, 0.3), { fill: '#8e4a26', tone: 0, lw: 0, hatch: false, rim: false });
    // the bed of needles, back row first; and a few out of the mattress's side, at you
    for (const v of [0.8, 0.55, 0.3, 0.06]) {
      for (let u = 0.07 + J(0.02); u < 0.95; u += 0.098 + J(0.02)) {
        const [x, y] = at(u + J(0.02), v + J(0.04));
        const t = needle(g, x, y, J(0.35), -1, 19 + rnd() * 9, full, 4.6);
        if (Math.hypot(t[0] - CUSH.x, t[1] - CUSH.y) > CUSH.r + 34) tips.push([...t, rnd()]);
      }
    }
    // the foot of the bed, in front of all that
    tube(g, 'M355,243 L355,214', 3, ENAMEL);
    for (const t of [0.33, 0.66]) tube(g, `M${330 + 25 * t},${280 - 39 * t} L${330 + 25 * t},${256 - 38 * t}`, 1.8, ENAMEL);
    tube(g, 'M330,334 L330,256 C333,238 345,222 355,216', 3, ENAMEL);
    shape(g, lumpy(330, 290, 2, 0.8, 8, 0.3), { fill: '#8e4a26', tone: 0, lw: 0, hatch: false, rim: false });

    // ── his speaker on its stand, the horn turned down at you
    g.save();
    g.translate(-6, 12);   // drawn where it was first tried; this keeps the horn inside the hex
    tube(g, 'M404,322 L404,178', 3, C.steelD);
    tube(g, 'M404,318 L389,331', 2.2, C.steelD); tube(g, 'M404,318 L420,330', 2.2, C.steelD); tube(g, 'M404,318 L406,334', 2.2, C.steelD);
    line(g, 'M415,176 C421,200 411,246 409,296 C408,318 418,332 430,326', 2.6, INK);
    line(g, 'M415,176 C421,200 411,246 409,296 C408,318 418,332 430,326', 1.2, '#3e3a32');
    tube(g, 'M404,180 L411,175', 2.6, C.steelD);
    {
      const D = [413, 172], M = [379, 199], L = Math.hypot(M[0] - D[0], M[1] - D[1]);
      const ux = (M[0] - D[0]) / L, uy = (M[1] - D[1]) / L, nx = -uy, ny = ux, f = (p, a, b) => [p[0] + ux * a + nx * b, p[1] + uy * a + ny * b];
      shape(g, ellD(D[0] - ux * 5, D[1] - uy * 5, 7.5, 7.5), { fill: '#4e5442', tex: 'rust', texA: 0.8, tone: 0.5, lw: 1.8, hatch: false });
      const [a1, a2, b1, b2, q1, q2] = [f(D, 0, 4.5), f(D, 0, -4.5), f(M, 0, 17), f(M, 0, -17), f(D, L * 0.75, 6.5), f(D, L * 0.75, -6.5)];
      shape(g, `M${a1} Q${q1} ${b1} L${b2} Q${q2} ${a2} Z`, { fill: '#6b7058', tex: 'rust', texA: 0.75, tone: 0.55, lw: 2, ang: -1.2 });
      const rot = Math.atan2(ny, nx);
      shape(g, ellD(M[0], M[1], 17, 6.5, rot), { fill: '#5a5e4a', tex: 'rust', texA: 0.6, tone: 0.3, lw: 1.8, hatch: false });
      shape(g, ellD(M[0] + ux * 1.2, M[1] + uy * 1.2, 13.5, 4.4, rot), { fill: '#120f0c', tone: 0, lw: 0.8, hatch: false, rim: false });
      for (const o of [-8, -3, 2, 7]) { const [x0, y0] = f(M, 0.6, o - 1.4), [x1, y1] = f(M, 0.6, o + 1.4); line(g, `M${x0},${y0} L${x1},${y1}`, 0.9, 'rgba(140,140,120,0.7)'); }
      taper(g, [f(M, -2, -15), f(M, -9, -9)], 1.4, 'rgba(255,246,220,0.45)');
    }
    g.restore();

    // ── needles in the floor round the bed
    clump(g, 248, 342, 5, full, tips, 12);
    clump(g, 180, 354, 6, full, tips, 10);
    clump(g, 426, 358, 5, full, tips, 10);

    // ── the trolley: back legs, the shelf and its dish, front legs, the tray
    for (const x of [270, 366]) { tube(g, `M${x},346 L${x},402`, 2.4, C.steelD); caster(g, x, 404); }
    obox(g, 252, 414, 104, 3, 26, { fill: C.steelD, tex: 'metal', texA: 1, lw: 1.4 });
    shape(g, 'M276,402 C274,394 290,392 298,396 C304,398 310,393 318,395 C328,397 326,407 314,408 C300,409 280,410 276,402 Z', { fill: C.steelL, tex: 'metal', texA: 1, tone: 0.4, lw: 1.3, hatch: false });
    needle(g, 284, 401, 1, -0.28, 22, full, 3.6);
    needle(g, 320, 403, -1, -0.12, 18, full, 3.6);
    for (const x of [256, 352]) { tube(g, `M${x},366 L${x},424`, 2.4); caster(g, x, 426); }
    obox(g, 252, 368, 104, 5, 26, { fill: C.steel, top: C.steelL, side: C.steelD, tex: 'metal', texA: 1, lw: 1.6 });
    taper(g, [[338, 368], [339, 378], [338.5, 386]], 2.2, 'rgba(214,150,44,0.85)', 'end');
    shape(g, ellD(338.6, 389, 1.8, 2.4), { fill: '#d6962c', tone: 0, lw: 0.6, hatch: false, rim: false });
    shape(g, ellD(314, 352, 31, 6.5), { fill: 'rgba(20,16,12,0.55)', tone: 0, lw: 0, hatch: false, rim: false });

    // ── the pincushion
    const back = [], front = [];
    const N = 76;
    for (let i = 0; i < N; i++) {
      const yy = 1 - 2 * (i + 0.5) / N, rr = Math.sqrt(1 - yy * yy), ph = i * 2.39996 + J(0.25);
      let nx = rr * Math.cos(ph) + J(0.1), ny = yy + J(0.1), nz = rr * Math.sin(ph) + J(0.1);
      const m = Math.hypot(nx, ny, nz);
      nx /= m; ny /= m; nz /= m;
      if (ny < -0.42 || ny > 0.8) continue;   // in the tray / the crown's
      const o = { d: nz * ct + ny * st, sx: nx, sy: -(ny * ct - nz * st), L: 25 + rnd() * 11, h: rnd() };
      if (Math.hypot(o.sx, o.sy) < 0.42) continue;   // end-on at you: a starburst of glass, not a cushion
      (o.d < 0 ? back : front).push(o);
    }
    const stick = (o) => {
      const t = needle(g, CUSH.x + o.sx * CUSH.r, CUSH.y + o.sy * CUSH.r, o.sx, o.sy, Math.max(5, o.L * Math.hypot(o.sx, o.sy)), full, 4.8);
      if (Math.hypot(t[0] - CUSH.x, t[1] - CUSH.y) > CUSH.r + 2) tips.push([...t, o.h]);
    };
    back.sort((a, b) => a.d - b.d).forEach(stick);
    shape(g, lumpy(CUSH.x, CUSH.y, CUSH.r, 0.94, 26, 0.05), { fill: full ? '#6a5e74' : '#5e5860', tex: 'cloth', texA: 1, tone: 0.62, lw: 2.4 });
    // its tufting: the seams that make it a pincushion and not a ball
    for (const phi of [-1.15, -0.45, 0.25, 0.95]) {
      const seam = [];
      for (let th = 0.2; th < 2.7; th += 0.18) {
        const nx = Math.sin(th) * Math.sin(phi), ny = Math.cos(th), nz = Math.sin(th) * Math.cos(phi);
        if (nz * ct + ny * st > 0.05) seam.push([CUSH.x + nx * CUSH.r * 0.98, CUSH.y - (ny * ct - nz * st) * CUSH.r * 0.98]);
      }
      if (seam.length > 1) taper(g, seam, 1.8, 'rgba(36,26,40,0.6)');
    }
    for (const [dx, dy, r, col] of [[-12, 8, 7, 'rgba(160,150,96,0.42)'], [9, 13, 6, 'rgba(120,60,140,0.5)'], [-4, -10, 5, 'rgba(160,150,96,0.35)']]) {
      shape(g, lumpy(CUSH.x + dx, CUSH.y + dy, r, 0.8, 12, 0.35), { fill: col, tex: 'grime', texA: 0.7, tone: 0, lw: 0, hatch: false, rim: false });
    }
    if (full) {
      // the crown's roots, pale, over the top of it
      for (const [ex, ey] of [[-20, 4], [-12, 12], [-2, 10], [10, 13], [19, 5], [24, -2]]) {
        taper(g, [CROWN_ROOT, [CROWN_ROOT[0] + ex * 0.5, CROWN_ROOT[1] + ey * 0.4 + 1], [CROWN_ROOT[0] + ex, CROWN_ROOT[1] + ey]], 1.3, 'rgba(232,224,204,0.75)');
      }
    } else {
      // taken: a torn hole where it grew, and the roots it left
      shape(g, lumpy(CROWN_ROOT[0], CROWN_ROOT[1] + 1, 12, 0.48, 16, 0.22), { fill: '#7e7484', tex: 'cloth', texA: 1, tone: 0.3, lw: 1.6, hatch: false });
      shape(g, lumpy(CROWN_ROOT[0] + 1, CROWN_ROOT[1] + 1.5, 8.5, 0.42, 14, 0.25), { fill: '#24161e', tone: 0, lw: 1, hatch: false, rim: false });
      for (const [ex, ey] of [[-16, 10], [-6, 14], [6, 16], [15, 9]]) taper(g, [[CROWN_ROOT[0] + ex * 0.4, CROWN_ROOT[1] + 2], [CROWN_ROOT[0] + ex * 0.8, CROWN_ROOT[1] + ey * 0.6], [CROWN_ROOT[0] + ex, CROWN_ROOT[1] + ey + 6]], 1.1, 'rgba(214,206,186,0.8)');
    }
    front.sort((a, b) => a.d - b.d).forEach(stick);

    // ── the SHARPS bin, front left, running over; his clipboard against it
    obox(g, 186, 424, 36, 40, 14, { fill: '#c49a2c', top: '#d4ae46', side: '#94721e', tex: 'grime', texA: 1 });
    shape(g, poly([[186.6, 388], [221.4, 388], [221.4, 393], [186.6, 393]]), { fill: '#8e2a20', tex: 'grime', texA: 0.8, tone: 0.2, lw: 0, hatch: false, rim: false });
    g.save();
    g.font = 'bold 8.6px Arial, Helvetica, sans-serif'; g.textAlign = 'center'; g.fillStyle = INK;
    g.fillText('SHARPS', 204, 404.5);
    g.restore();
    line(g, 'M190,413 l3,-3 l2,3 l3,-3 l2,3 M198,412 l5,0 M205,411 l3,2 l3,-3 l4,2 M190,418 l12,-1', 0.7, 'rgba(255,240,200,0.55)');
    shape(g, poly([[198, 380], [216, 380], [220, 375.5], [202, 375.5]]), { fill: INK, tone: 0, lw: 0.8, hatch: false, rim: false });
    const fan = [];
    for (let i = 0; i < 13; i++) fan.push({ x: 200 + rnd() * 17, y: 377.5 + J(1.2), a: -Math.PI / 2 + J(1.05), L: 15 + rnd() * 11 });
    fan.sort((a, b) => Math.abs(b.a + Math.PI / 2) - Math.abs(a.a + Math.PI / 2));
    for (const o of fan) tips.push([...needle(g, o.x, o.y, Math.cos(o.a), Math.sin(o.a), o.L, full, 4.2), rnd()]);
    needle(g, 228, 432, 1, 0.12, 18, full, 4);
    needle(g, 178, 436, -0.3, -1, 9, full, 4);
    // the clipboard: everybody's best deed, in his keeping
    shape(g, poly([[229, 429], [249, 427], [246, 396], [226, 398]]), { fill: '#7a5a3a', tex: 'leather', texA: 1, tone: 0.5, lw: 1.6, hatch: false });
    shape(g, poly([[231.5, 425], [246.5, 423.5], [244, 401], [229, 402.5]]), { fill: '#d8d0b8', tex: 'grime', texA: 0.9, tone: 0.25, lw: 0.8, hatch: false, rim: false });
    for (let i = 0; i < 6; i++) taper(g, [[231.5 + i * 0.25, 406 + i * 3.2], [237 + J(1), 405.6 + i * 3.2 + J(0.6)], [243.5 - i * 0.2, 405 + i * 3.2]], 0.8, i === 2 ? '#8a2418' : '#3a2c20', 'none');
    shape(g, poly([[232, 400], [242, 399], [242.4, 403], [232.4, 404]]), { fill: C.steelL, tex: 'metal', tone: 0.3, lw: 1, hatch: false });
    line(g, 'M237,400 C244,406 252,420 250,440', 0.7, '#d8d0b8');
    line(g, 'M248,438 L253,446', 3, INK); line(g, 'M248,438 L253,446', 1.6, '#c9a032');

    // ── needles in the floor in front, either side of the path
    clump(g, 388, 402, 7, full, tips, 14);
    clump(g, 362, 440, 5, full, tips, 11);
    clump(g, 236, 450, 4, full, tips, 9);
    // two the crawler flattened
    needle(g, 292, 444, 0.95, 0.3, 17, full, 4);
    needle(g, 318, 458, -1, 0.2, 15, full, 4);
    fly(g, 236, 368, 0.75, true);

    // ── taken: the hairs are dead and hang off the points
    if (!full) {
      for (const [x, y, ux, uy, h] of tips) {
        if (h < 0.35) continue;
        const ex = x + ux * 3 + (ux > 0 ? 2 : -2), ey = y + uy * 2 + 7 + 4 * h;
        taper(g, [[x, y], [x + ux * 3, y + uy * 3], [ex, ey]], 1, 'rgba(150,140,120,0.85)', 'none');
        shape(g, lumpy(ex, ey + 1, 1.9, 0.9, 7, 0.35), { fill: '#3e2c38', tone: 0, lw: 0.7, hatch: false, rim: false });
      }
    }
  }

  // One breath of the Needlecap's crown: caps the size of thumbnails,
  // bruise-purple and wet, on pale stems out of the top of the pincushion.
  // b 0 (out) .. 1 (in).
  function drawCrown(g, b) {
    const { seed, J, shape, taper, ellD, lumpy, INK } = K;
    seed(3141);
    const [rx, ry] = CROWN_ROOT, grow = 1 + 0.07 * b, swell = 1 + 0.1 * b;
    const caps = CAPS.map(([dx, dy, r]) => [rx + dx * grow, ry + dy * grow, r * swell, dx]).sort((a, b) => a[1] - b[1]);
    // stems first, all of them, so every cap sits over every stem; they come
    // up out of one fleshy knot, not a point
    for (const [x, y, r, dx] of caps) {
      const st = [[rx + dx * 0.32, ry - 2], [rx + dx * 0.62 + J(1.5), (ry + y) / 2 + 1], [x, y + r * 0.15]];
      taper(g, st, 6.2, INK, 'none');
      taper(g, st, 3.8, '#b9aab4', 'none');
      taper(g, st.map(([px, py]) => [px - 1, py]), 1, 'rgba(240,232,236,0.6)', 'none');
    }
    shape(g, lumpy(rx, ry - 1, 11 * grow, 0.55, 16, 0.2), { fill: '#a8949e', tex: 'skin', texA: 0.6, tone: 0.55, lw: 1.6, hatch: false });
    shape(g, lumpy(rx - 3, ry - 3, 4, 0.5, 10, 0.3), { fill: 'rgba(240,226,232,0.45)', tone: 0, lw: 0, hatch: false, rim: false });
    for (const [x, y, r, dx] of caps) {
      g.save();
      g.translate(x, y);
      g.rotate(dx * 0.012 + J(0.08));
      shape(g, ellD(0, 0.6, r * 0.92, r * 0.3), { fill: '#3a1a42', tone: 0, lw: 1.2, hatch: false, rim: false });   // gills
      shape(g, `M${-r},0 C${-r},${-r * 1.3} ${r},${-r * 1.3} ${r},0 C${r * 0.55},${r * 0.3} ${-r * 0.55},${r * 0.3} ${-r},0 Z`,
        { fill: '#7e3c90', tex: 'skin', texA: 0.5, tone: 0.6, lw: 1.6, hatch: false });
      shape(g, lumpy(r * 0.35, -r * 0.2, r * 0.4, 0.6, 10, 0.3), { fill: 'rgba(70,20,72,0.6)', tone: 0, lw: 0, hatch: false, rim: false });   // the bruise
      for (const [sx, sy] of [[-0.4, -0.55], [0.1, -0.75], [0.45, -0.45]]) shape(g, ellD(r * sx, r * sy, Math.max(0.6, r * 0.09), Math.max(0.5, r * 0.07)), { fill: '#d4b4de', tone: 0, lw: 0, hatch: false, rim: false });
      taper(g, [[-r * 0.72, -r * (0.3 + 0.05 * b)], [-r * 0.45, -r * 0.78], [-r * (0.05 - 0.1 * b), -r * 0.95]], Math.max(1, r * 0.2), 'rgba(255,244,255,0.82)');
      g.restore();
    }
    // the big one drips, more on the breath in
    const [x, y, r] = caps[0];
    taper(g, [[x + r * 0.5, y + r * 0.2], [x + r * 0.55, y + r * 0.5 + 3 * b], [x + r * 0.5, y + r * 0.7 + 5 * b]], 2, 'rgba(200,160,220,0.75)', 'end');
  }

  function painted(c) {
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    for (let i = 3; i < d.length; i += 64) if (d[i] > 0) return true;
    return false;
  }
  function canvasFor(w, h, bx, by, bw, bh) {
    const c = document.createElement('canvas'); c.width = w; c.height = h;
    const g = c.getContext('2d');
    g.scale(w / bw, h / bh); g.translate(-bx, -by);
    g.lineJoin = 'round'; g.lineCap = 'round';
    return [c, g];
  }
  const rooms = {}, heads = new Map();
  let failed = false;
  function room(wired) {
    const key = wired ? 1 : 0;
    if (rooms[key]) return rooms[key];
    if (failed || !kit()) return null;
    try {
      const [c, g] = canvasFor(PX, PX, 0, 0, 600, 600);
      drawRoom(g, wired);
      if (painted(c)) rooms[key] = c;
      return rooms[key] || null;
    } catch (e) { console.warn('[lab] theatre sticker failed', e); failed = true; return null; }
  }
  function head(i, mouth, still) {
    const key = still ? 'still' : `${i}/${mouth}`;
    if (heads.has(key)) return heads.get(key);
    if (failed || !kit()) return null;
    try {
      const [bx, by, bw, bh] = HEAD_BOX;
      const [c, g] = canvasFor(Math.round(bw * PX / 600), Math.round(bh * PX / 600), bx, by, bw, bh);
      drawHead(g, still ? 0 : Math.max(-4, Math.min(4, i)) / 4, still ? 0 : mouth, !!still);
      if (painted(c)) heads.set(key, c);
      return c;
    } catch (e) { console.warn('[lab] head frame failed', e); failed = true; return null; }
  }
  const wards = {}, wardTipsBy = {}, crowns = [];
  let wardFailed = false;
  function ward(full) {
    const key = full ? 1 : 0;
    if (wards[key]) return wards[key];
    if (wardFailed || !kit()) return null;
    try {
      const [c, g] = canvasFor(PX, PX, 0, 0, 600, 600), tips = [];
      drawWard(g, full, tips);
      if (painted(c)) { wards[key] = c; wardTipsBy[key] = tips; }
      return wards[key] || null;
    } catch (e) { console.warn('[lab] ward sticker failed', e); wardFailed = true; return null; }
  }
  function crown(i) {
    i = Math.max(0, Math.min(4, i | 0));
    if (crowns[i]) return crowns[i];
    if (wardFailed || !kit()) return null;
    try {
      const [bx, by, bw, bh] = CROWN_BOX;
      const [c, g] = canvasFor(Math.round(bw * PX / 600), Math.round(bh * PX / 600), bx, by, bw, bh);
      drawCrown(g, i / 4);
      if (painted(c)) crowns[i] = c;
      return c;
    } catch (e) { console.warn('[lab] crown frame failed', e); wardFailed = true; return null; }
  }
  const wardTips = () => wardTipsBy[1] || [];
  return { room, head, HEAD_BOX, BULB: [372, 188], BOX_LAMP: [407, 392], STUB: [411, 351], ward, crown, wardTips, CROWN, CROWN_BOX };
})();
