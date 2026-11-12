// ── bonney.js ──────────────────────────────────────────────────────────────
// ONE-HAND BONNEY (bonney.hpp, docs/one-hand-bonney.md): a huge, foul hag at
// her fire on the Ash Dunes, Dennis Turnbull turning on her truck-axle spit,
// and Pinch -- her runaway left hand -- going through your pack while you look
// at the one she has. She stays put: section 1 and then section 2 open on the
// same hex, and a finished quest sends her off, to turn up near a random
// settlement at the next dawn.
//
// State is worldState.bon (the server's "bon" object, merged in by
// _applyWorldState in network.js): q r (her fire; -1 while she is away), m
// (0 section 1 / 1 section 2 / 2 away), n (passes finished), aq ar (the ring
// of ash she left; -1 none).
//
// Two layers: 'bonney' under the pawns (her, cooking: one sticker, drawn in
// code once and blitted -- BonneyArt below) and 'bonney_light' over the night
// tint (the fire is a light; its smoke reaches past your vision, so you can
// follow it in). Her hex wears the quests' yellow outline (bonneyQuestAt,
// renderer.js isPOIRenderable).
//
// Also here: the log and toast lines for ev bonney and for a scene opening,
// and the scene text's {{stolen}} / {{apple}} (bonneyPlaceholders, called by
// ui-encounter.js). Her calls are BONNEY_LINES in game-data.js.

const BONNEY_SPRITE_HEX = 0.75;   // sticker edge in HEX_SZ: half Kane's 1.5 (the user, 2026-10-26)
const BONNEY_STICKER_PX = 256;    // the sticker's bitmap: enough for 0.75 x HEX_SZ_MAX at 3x DPR
const BONNEY_SMOKE_REACH = 3;     // her smoke is drawn this many hexes past your vision
const BONNEY_FIRE = [416, 414];   // the pit in the sticker's 600-unit cell
const BONNEY_SECTION_NAMES = ['', 'GUESS WHICH HAND', 'SUPPER AT BONNEY’S'];

const bonPick = (a) => a[Math.floor(Math.random() * a.length)];
function bonState() { const b = worldState.bon; return b && b.m !== 2 && b.q >= 0 ? b : null; }
function bonItemName(id) { return (typeof ITEMS !== 'undefined' && ITEMS.find((i) => i.id === id)?.name) || `item ${id}`; }
function bonWho(pid) { return pid === myId ? 'You' : (players[pid]?.nm || `P${pid}`); }
function bonDist(q, r) {
  if (myId < 0 || !players[myId]) return 0;
  const me = players[myId];
  return hexDistWrap(me.q, me.r, q, r);
}
const bonInSight = (q, r) => bonDist(q, r) <= (myVisionR ?? 3);

// Is (q, r) her fire -- step on it and a section opens (bonneyOnArrival)?
function bonneyQuestAt(q, r) {
  const b = bonState();
  return !!b && !myDepth && b.q === q && b.r === r;
}

function bonScreen(cam, q, r) {
  const { vq, vr } = closestWrapCoords({ q, r }, cam.meRp);
  const pp = hexToPixel(vq, vr, HEX_SZ);
  return { x: pp.x + cam.ox, y: pp.y + cam.oy };
}
const bonOnScreen = (p, m) => p.x > -m && p.x < cssWidth + m && p.y > -m && p.y < cssHeight + m;

// The sticker, drawn once on first need (a canvas built while the page is
// hidden can come back blank, so it is redrawn until it actually holds paint).
let bonSticker = null;
function bonneySticker() {
  if (bonSticker) return bonSticker;
  try {
    const c = BonneyArt.sticker(BONNEY_STICKER_PX);
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let painted = false;
    for (let i = 3; i < d.length && !painted; i += 64) painted = d[i] > 0;
    if (painted) bonSticker = c;
  } catch (e) { console.warn('[bonney] sticker failed', e); bonSticker = false; }
  return bonSticker;
}

// ── The 'bonney' layer (under the pawns) ──────────────────────────────────
function renderBonney(cam) {
  const w = worldState.bon;
  if (!w) return;
  // The ring of ash she left, for a day, once its hex is on your map.
  if (w.aq >= 0 && gameMap[w.ar]?.[w.aq]) {
    const p = bonScreen(cam, w.aq, w.ar);
    if (bonOnScreen(p, HEX_SZ)) {
      ctx.save();
      ctx.fillStyle = 'rgba(40,36,32,0.55)'; ctx.strokeStyle = 'rgba(20,16,14,0.8)'; ctx.lineWidth = Math.max(1, HEX_SZ * 0.03);
      ctx.beginPath(); ctx.ellipse(p.x, p.y + HEX_SZ * 0.08, HEX_SZ * 0.34, HEX_SZ * 0.15, 0, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      ctx.fillStyle = 'rgba(120,114,104,0.5)';
      ctx.beginPath(); ctx.ellipse(p.x, p.y + HEX_SZ * 0.08, HEX_SZ * 0.2, HEX_SZ * 0.08, 0, 0, Math.PI * 2); ctx.fill();
      ctx.restore();
    }
  }
  const b = bonState();
  if (!b || myDepth || !bonInSight(b.q, b.r)) return;
  const p = bonScreen(cam, b.q, b.r);
  const sz = HEX_SZ * BONNEY_SPRITE_HEX;
  if (!bonOnScreen(p, sz)) return;
  const stk = bonneySticker();
  if (stk) { ctx.drawImage(stk, p.x - sz / 2, p.y - sz / 2, sz, sz); return; }
  // Until the sticker exists: a fire, and a hunched shape at it.
  ctx.save();
  ctx.fillStyle = '#e46a26'; ctx.beginPath(); ctx.arc(p.x + sz * 0.15, p.y + sz * 0.15, sz * 0.14, 0, Math.PI * 2); ctx.fill();
  ctx.fillStyle = '#4b4135'; ctx.beginPath(); ctx.ellipse(p.x - sz * 0.2, p.y, sz * 0.14, sz * 0.24, -0.2, 0, Math.PI * 2); ctx.fill();
  ctx.restore();
}

// ── The 'bonney_light' layer (over the night tint) ────────────────────────
// Her fire glows like the light it is, and its smoke stands up past the edge
// of what you can see -- follow it in.
function renderBonneyLight(cam) {
  const b = bonState();
  if (!b || myDepth) return;
  const d = bonDist(b.q, b.r), vr = myVisionR ?? 3;
  if (d > vr + BONNEY_SMOKE_REACH) return;
  const p = bonScreen(cam, b.q, b.r);
  if (!bonOnScreen(p, HEX_SZ * 3)) return;
  const now = performance.now(), sz = HEX_SZ * BONNEY_SPRITE_HEX;
  const fx = p.x + (BONNEY_FIRE[0] / 600 - 0.5) * sz, fy = p.y + (BONNEY_FIRE[1] / 600 - 0.5) * sz;
  // The smoke: a column of soft puffs leaning downwind, always drawn.
  ctx.save();
  const lean = typeof windPrevailing === 'function' ? windPrevailing().x : 0.35;
  for (let i = 0; i < 7; i++) {
    const t = ((now / 5200) + i / 7) % 1;
    const x = fx + lean * t * HEX_SZ * 0.9 + Math.sin(t * 6 + i) * HEX_SZ * 0.04;
    const y = fy - sz * 0.25 - t * HEX_SZ * 1.3;
    const r = HEX_SZ * (0.07 + t * 0.16);
    ctx.fillStyle = `rgba(46,42,40,${(0.5 * (1 - t)).toFixed(3)})`;
    ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill();
  }
  ctx.restore();
  if (d > vr) return;
  // The glow, flickering.
  const fl = 0.75 + 0.25 * Math.sin(now / 90) * Math.sin(now / 37 + 1.3);
  const g = ctx.createRadialGradient(fx, fy, 0, fx, fy, HEX_SZ * 0.9);
  g.addColorStop(0, `rgba(255,170,80,${(0.32 * fl).toFixed(3)})`);
  g.addColorStop(1, 'rgba(255,120,40,0)');
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  ctx.fillStyle = g; ctx.beginPath(); ctx.arc(fx, fy, HEX_SZ * 0.9, 0, Math.PI * 2); ctx.fill();
  ctx.restore();
}

// ── The scene text ─────────────────────────────────────────────────────────
// What ui-encounter.js puts in for {{apple}} when a section opens (the
// enc_path's "apple" and "own", kept by network.js in _bonEncPath). {{stolen}}
// arrives later, with ev bonney "pinch" (bonneyStolenText).
function bonneyPlaceholders(sec) {
  const e = globalThis._bonEncPath;
  if (sec !== 2 || !e || !e.apple) return {};
  return { apple: `${e.own ? 'your' : 'somebody’s'} ${bonItemName(e.apple)}` };
}
function bonneyStolenText(item) {
  return item ? `your ${bonItemName(item)}` : 'a pinch of lint from the bottom of your pack';
}

// ── Log lines ────────────────────────────────────────────────────────────────
// A section opened at her fire (ev enc_start "bonney", network.js).
function bonneyOnEncStart(ev) {
  const WHERE = ['', 'walked into Bonney’s firelight', 'sat down to supper at Bonney’s fire'];
  addLog(`<span class="log-col">✋ ${escHtml(bonWho(ev.pid))} ${WHERE[ev.bonney] || 'walked into Bonney’s firelight'}</span>`);
}

function _evBonney(ev) {
  const mine = ev.pid === myId;
  const who = bonWho(ev.pid);
  const item = ev.item | 0;
  switch (ev.w) {
    case 'call': {
      const lines = typeof BONNEY_LINES !== 'undefined' ? BONNEY_LINES : ['Come here, sweetmeat.'];
      const line = lines[(ev.i | 0) % lines.length];
      addLog(`<span class="log-mv">✋ Somebody calls from a fire on the dunes: ‘${escHtml(line)}’</span>`);
      showToast(`✋ ‘${line}’`, 'bonney');
      break;
    }
    case 'pinch':
      // Only ever ours (unicast): fill {{stolen}} for the rest of the scene.
      globalThis._encSetPlaceholder?.('stolen', bonneyStolenText(item));
      addLog(item ? `<span class="log-check-fail">✋ Something light and quick has been through your pack. Your ${escHtml(bonItemName(item))} is gone.</span>`
                  : '<span class="log-mv">✋ Something went through your pack and came out with nothing but lint.</span>');
      break;
    case 'caught':
      addLog(item ? `<span class="log-col">✋ ${escHtml(who)} caught a hand in ${mine ? 'your' : 'their'} pack and took back ${mine ? 'your' : 'a'} ${escHtml(bonItemName(item))}.</span>`
                  : `<span class="log-col">✋ ${escHtml(who)} caught Bonney’s other hand before it got anything.</span>`);
      break;
    case 'supper':
      addLog('<span class="log-mv">✋ Bonney goes back to her crank. Supper won’t be long.</span>');
      break;
    case 'rescue':
      addLog(`<span class="log-col">✋ ${escHtml(who)} cut Dennis Turnbull down off Bonney’s spit${item ? `, and ${mine ? 'got' : 'walked off with'} ${escHtml(bonItemName(item))}` : ''}.</span>`);
      showBanner('ONE-HAND BONNEY', mine ? 'Turnbull is off the spit' : `${escHtml(who)} spoilt Bonney’s supper`);
      break;
    case 'bargain':
      addLog(`<span class="log-check-fail">✋ ${escHtml(who)} traded a toenail for Dennis Turnbull. Bonney ate ${item ? `the ${escHtml(bonItemName(item))}` : 'the apple'}.</span>`);
      showBanner('ONE-HAND BONNEY', item ? `she ate the ${escHtml(bonItemName(item))}` : 'she ate the apple');
      break;
    case 'back':
      addLog('<span class="log-mv">✋ Somewhere near a town, a new fire is lit. Something is turning on it.</span>');
      break;
  }
}

// ── BonneyArt: the sticker, drawn in code ─────────────────────────────────────
// One closure, so none of the drawing helpers leak into the shared scope. The
// same code renders the concept sheet in .claude/bonney-preview/ (sheet.html):
// change it there, preview it, then run build_client.py beside it.
const BonneyArt = (() => {
  // Comic-ink toolkit (canvas 2D), from the concept sheet in .claude/bonney-preview/.
  // Every shape is filled, textured, shaded toward the lower right (light from
  // the upper left), hatched in its shadow, rim-lit on its lit edge, and inked
  // with a line that is heavier on the shadow side.
  const U = 600;            // sticker units (a 600 x 600 cell)
  const SC = 2;             // canvas pixels per unit
  const INK = '#1a1512';

  // ── seeded randomness
  let _seed = 1;
  const rnd = () => { _seed |= 0; _seed = _seed + 0x6D2B79F5 | 0; let t = Math.imul(_seed ^ _seed >>> 15, 1 | _seed); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
  const seed = (s) => { _seed = s; };
  const J = (a) => (rnd() * 2 - 1) * a;
  function hash2(x, y, s) {
    let h = (Math.imul(x, 374761393) + Math.imul(y, 668265263) + Math.imul(s, 1442695041)) | 0;
    h = Math.imul(h ^ (h >>> 13), 1274126177); h ^= h >>> 16;
    return (h >>> 0) / 4294967295;
  }
  function vnoise(x, y, s) {
    const xi = Math.floor(x), yi = Math.floor(y), xf = x - xi, yf = y - yi;
    const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    const a = hash2(xi, yi, s), b = hash2(xi + 1, yi, s), c = hash2(xi, yi + 1, s), d = hash2(xi + 1, yi + 1, s);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
  }
  function fbm(x, y, s, o = 4) { let t = 0, a = 0.5, f = 1, n = 0; for (let i = 0; i < o; i++) { t += a * vnoise(x * f, y * f, s + i * 17); n += a; f *= 2; a *= 0.5; } return t / n; }
  const clamp = (v, a = 0, b = 1) => Math.max(a, Math.min(b, v));
  const sstep = (a, b, v) => { const t = clamp((v - a) / (b - a)); return t * t * (3 - 2 * t); };

  // ── textures: one U x U canvas per kind, sampled in place by every shape
  const TEX = {};
  const TU = 200;          // texture pixels across the cell
  function makeTex(kind, fn) {
    const c = document.createElement('canvas'); c.width = TU; c.height = TU;
    const g = c.getContext('2d'), im = g.createImageData(TU, TU), d = im.data;
    for (let y = 0; y < TU; y++) for (let x = 0; x < TU; x++) {
      const [r, gg, b, a] = fn(x * U / TU, y * U / TU), i = (y * TU + x) * 4;
      d[i] = r; d[i + 1] = gg; d[i + 2] = b; d[i + 3] = clamp(a) * 255;
    }
    g.putImageData(im, 0, 0); TEX[kind] = c;
  }
  function buildTextures() {
    makeTex('grime', (x, y) => {
      const n = fbm(x / 34, y / 34, 3), sp = hash2(x, y, 9);
      return [38, 28, 18, sstep(0.52, 0.78, n) * 0.55 + (sp > 0.986 ? 0.55 : 0)];
    });
    makeTex('cloth', (x, y) => {
      const fib = fbm(x / 1.3, y / 7, 5, 2), bl = fbm(x / 26, y / 26, 6), sp = hash2(x, y, 11);
      return [34, 28, 18, Math.abs(fib - 0.5) * 0.5 + sstep(0.55, 0.8, bl) * 0.5 + (sp > 0.99 ? 0.5 : 0)];
    });
    makeTex('burlap', (x, y) => {
      const w = fbm(x / 1.6, y / 1.6, 21, 2), bl = fbm(x / 22, y / 22, 22);
      const weave = (hash2(x >> 1, y >> 1, 23) > 0.5) !== ((x >> 1) % 2 === (y >> 1) % 2) ? 0.18 : 0;
      return [40, 30, 16, weave + Math.abs(w - 0.5) * 0.4 + sstep(0.56, 0.82, bl) * 0.45];
    });
    makeTex('skin', (x, y) => {
      const m = fbm(x / 14, y / 14, 31), p = fbm(x / 3, y / 3, 32, 2), sp = hash2(x, y, 33);
      const dark = sstep(0.5, 0.72, m), pore = sstep(0.62, 0.75, p) * 0.25;
      return [52, 60, 38, dark * 0.5 + pore + (sp > 0.992 ? 0.45 : 0)];
    });
    makeTex('rust', (x, y) => {
      const m = fbm(x / 18, y / 18, 41), pit = fbm(x / 3.5, y / 3.5, 42, 2), st = fbm(x / 5, y / 38, 43, 3);
      const a = sstep(0.46, 0.6, m), dark = sstep(0.64, 0.76, pit);
      if (dark > 0.2 && a > 0.3) return [48, 22, 12, 0.75 * dark];
      return [150 + 30 * (st - 0.5), 70, 30, a * 0.9 + sstep(0.6, 0.75, st) * 0.3];
    });
    makeTex('metal', (x, y) => {
      const st = fbm(x / 70, y / 2.6, 51, 3), bl = fbm(x / 30, y / 30, 52);
      return [30, 30, 30, Math.abs(st - 0.5) * 0.55 + sstep(0.58, 0.8, bl) * 0.4];
    });
    makeTex('leather', (x, y) => {
      const cr = fbm(x / 4, y / 4, 61, 3), bl = fbm(x / 20, y / 20, 62);
      return [24, 16, 10, sstep(0.6, 0.66, cr) * 0.6 + sstep(0.55, 0.8, bl) * 0.4];
    });
  }

  // ── path helpers
  function bbox(d) {
    const n = d.match(/-?\d*\.?\d+(?:e-?\d+)?/g).map(Number);
    let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (let i = 0; i + 1 < n.length; i += 2) { x0 = Math.min(x0, n[i]); x1 = Math.max(x1, n[i]); y0 = Math.min(y0, n[i + 1]); y1 = Math.max(y1, n[i + 1]); }
    return [x0, y0, x1, y1];
  }
  function poly(pts, close = true) { return 'M' + pts.map(p => p[0].toFixed(1) + ',' + p[1].toFixed(1)).join(' L') + (close ? ' Z' : ''); }

  // Hatching inside the current clip: lines across the bbox, invisible on the
  // lit side, darkening toward the shadow corner; a cross-hatch in deep shade.
  function hatch(g, bb, t, ang = -0.62, gap = 3.1) {
    const [x0, y0, x1, y1] = bb, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, R = Math.hypot(x1 - x0, y1 - y0) / 2 + 4;
    const pass = (a, start, alpha, sp) => {
      const gr = g.createLinearGradient(x0 + (x1 - x0) * 0.2, y0 + (y1 - y0) * 0.15, x1, y1);
      gr.addColorStop(0, 'rgba(26,21,18,0)'); gr.addColorStop(clamp(start, 0.01, 0.98), 'rgba(26,21,18,0)'); gr.addColorStop(1, `rgba(26,21,18,${alpha})`);
      g.strokeStyle = gr;
      const dx = Math.cos(a), dy = Math.sin(a), nx = -dy, ny = dx;
      for (let o = -R; o <= R; o += sp) {
        const j = J(0.7), s0 = -R * (0.75 + rnd() * 0.25), s1 = R * (0.75 + rnd() * 0.25);
        // a gap in some lines, the way a pen skips
        const br = rnd() < 0.35 ? s0 + (s1 - s0) * (0.3 + rnd() * 0.4) : null;
        g.lineWidth = 0.55 + rnd() * 0.5;
        g.beginPath();
        const P = (s) => [cx + nx * (o + j) + dx * s, cy + ny * (o + j) + dy * s];
        if (br === null) { g.moveTo(...P(s0)); g.lineTo(...P(s1)); }
        else { g.moveTo(...P(s0)); g.lineTo(...P(br - 2)); g.moveTo(...P(br + 3)); g.lineTo(...P(s1)); }
        g.stroke();
      }
    };
    pass(ang, 0.78 - 0.38 * t, 0.85 * t, gap);
    if (t > 0.42) pass(ang + 1.2, 0.93 - 0.3 * t, 0.7 * t, gap * 1.2);
  }

  // The workhorse. o: fill, tex, texA, tone (0 flat .. 1 deep), lw, rim, rimC, hatch, ang, bb, rule
  function shape(g, d, o = {}) {
    const p = new Path2D(d), bb = o.bb || bbox(d), lw = o.lw ?? 2.3;
    g.save();
    g.fillStyle = o.fill || '#888'; g.fill(p, o.rule || 'nonzero');
    g.save(); g.clip(p, o.rule || 'nonzero');
    if (o.tex) { g.globalAlpha = o.texA ?? 0.85; g.drawImage(TEX[o.tex], 0, 0, U, U); g.globalAlpha = 1; }
    const t = o.tone ?? 0.5;
    if (t > 0) {
      const [x0, y0, x1, y1] = bb;
      const gr = g.createLinearGradient(x0 + (x1 - x0) * 0.1, y0 + (y1 - y0) * 0.05, x1, y1);
      gr.addColorStop(0, 'rgba(255,248,226,0.10)'); gr.addColorStop(0.42, 'rgba(0,0,0,0)'); gr.addColorStop(1, `rgba(22,15,10,${0.5 * t})`);
      g.fillStyle = gr; g.fillRect(x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4);
      if (o.hatch !== false) hatch(g, bb, t, o.ang ?? -0.62, o.gap ?? 3.1);
    }
    if (o.rim !== false) { g.translate(1.5, 1.5); g.strokeStyle = o.rimC || 'rgba(255,246,220,0.32)'; g.lineWidth = 2.2; g.stroke(p); }
    g.restore();
    if (lw > 0) {
      // the shadow-side weight: a fatter line peeking out lower-right, outside only
      g.save();
      const out = new Path2D(); out.rect(-200, -200, U + 400, U + 400); out.addPath(p);
      g.clip(out, 'evenodd');
      g.translate(0.9, 1.1); g.lineWidth = lw * 1.6; g.strokeStyle = INK; g.stroke(p);
      g.restore();
      g.lineWidth = lw; g.strokeStyle = INK; g.stroke(p);
    }
    g.restore();
  }

  // A brush stroke: Catmull-Rom through pts, width swelling then tapering.
  function taper(g, pts, w, col = INK, prof = 'both') {
    const sm = [];
    for (let i = 0; i < pts.length - 1; i++) {
      const p0 = pts[Math.max(0, i - 1)], p1 = pts[i], p2 = pts[i + 1], p3 = pts[Math.min(pts.length - 1, i + 2)];
      for (let k = 0; k < 8; k++) {
        const t = k / 8, t2 = t * t, t3 = t2 * t;
        sm.push([0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3),
                 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)]);
      }
    }
    sm.push(pts[pts.length - 1]);
    const L = [], R = [];
    for (let i = 0; i < sm.length; i++) {
      const a = sm[Math.max(0, i - 1)], b = sm[Math.min(sm.length - 1, i + 1)];
      let nx = -(b[1] - a[1]), ny = b[0] - a[0]; const n = Math.hypot(nx, ny) || 1; nx /= n; ny /= n;
      const t = i / (sm.length - 1);
      const k = prof === 'both' ? Math.pow(Math.sin(Math.PI * t), 0.6) : prof === 'start' ? Math.pow(1 - t, 0.7) : prof === 'end' ? Math.pow(t, 0.7) : 1;
      const hw = Math.max(0.15, w * k / 2);
      L.push([sm[i][0] + nx * hw, sm[i][1] + ny * hw]); R.push([sm[i][0] - nx * hw, sm[i][1] - ny * hw]);
    }
    g.fillStyle = col; g.fill(new Path2D(poly(L.concat(R.reverse()))));
  }
  function line(g, d, w = 1.4, col = INK) { g.save(); g.lineWidth = w; g.strokeStyle = col; g.lineCap = 'round'; g.lineJoin = 'round'; g.stroke(new Path2D(d)); g.restore(); }
  function ellD(cx, cy, rx, ry, rot = 0, n = 28) {
    const pts = []; for (let i = 0; i < n; i++) { const a = i / n * Math.PI * 2; const x = Math.cos(a) * rx, y = Math.sin(a) * ry; pts.push([cx + x * Math.cos(rot) - y * Math.sin(rot), cy + x * Math.sin(rot) + y * Math.cos(rot)]); }
    return poly(pts);
  }
  function lumpy(cx, cy, r, sq = 0.85, n = 22, j = 0.18) {
    const pts = []; for (let i = 0; i < n; i++) { const a = i / n * Math.PI * 2, rr = r * (1 + J(j)); pts.push([cx + Math.cos(a) * rr, cy + Math.sin(a) * rr * sq]); }
    return poly(pts);
  }

  // Halftone ground shadow: dots that shrink toward the rim.
  function halftone(g, cx, cy, rx, ry, rot = 0, k = 1) {
    g.save(); g.fillStyle = INK;
    const step = 4.2;
    for (let y = -ry; y <= ry; y += step) for (let x = -rx; x <= rx; x += step) {
      const xx = x + ((Math.round(y / step) % 2) ? step / 2 : 0);
      const d = (xx * xx) / (rx * rx) + (y * y) / (ry * ry);
      if (d > 1) continue;
      const r = 1.45 * k * Math.pow(1 - d, 0.6);
      if (r < 0.25) continue;
      const X = cx + xx * Math.cos(rot) - y * Math.sin(rot), Y = cy + xx * Math.sin(rot) + y * Math.cos(rot);
      g.beginPath(); g.arc(X, Y, r, 0, Math.PI * 2); g.fill();
    }
    g.restore();
  }

  // Twisted rope between points (with a slight sag).
  function rope(g, a, b, w = 5, sag = 6, col = '#8f7a55') {
    const m = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2 + sag];
    const d = `M${a[0]},${a[1]} Q${m[0]},${m[1]} ${b[0]},${b[1]}`;
    line(g, d, w + 2.4, INK); line(g, d, w, col);
    const L = Math.hypot(b[0] - a[0], b[1] - a[1]), n = Math.floor(L / 4.5);
    for (let i = 1; i < n; i++) {
      const t = i / n, x = (1 - t) * (1 - t) * a[0] + 2 * (1 - t) * t * m[0] + t * t * b[0], y = (1 - t) * (1 - t) * a[1] + 2 * (1 - t) * t * m[1] + t * t * b[1];
      const dx = 2 * (1 - t) * (m[0] - a[0]) + 2 * t * (b[0] - m[0]), dy = 2 * (1 - t) * (m[1] - a[1]) + 2 * t * (b[1] - m[1]); const dl = Math.hypot(dx, dy) || 1;
      const nx = -dy / dl, ny = dx / dl, ux = dx / dl, uy = dy / dl;
      line(g, `M${x + nx * w / 2 - ux * 1.5},${y + ny * w / 2 - uy * 1.5} L${x - nx * w / 2 + ux * 1.5},${y - ny * w / 2 + uy * 1.5}`, 0.8, '#4c3f2a');
    }
  }

  // A chain: alternating links along a line.
  function chain(g, a, b, sag = 8, k = 1) {
    const m = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2 + sag], L = Math.hypot(b[0] - a[0], b[1] - a[1]), n = Math.floor(L / (7 * k));
    for (let i = 0; i <= n; i++) {
      const t = i / n, x = (1 - t) * (1 - t) * a[0] + 2 * (1 - t) * t * m[0] + t * t * b[0], y = (1 - t) * (1 - t) * a[1] + 2 * (1 - t) * t * m[1] + t * t * b[1];
      const dx = 2 * (1 - t) * (m[0] - a[0]) + 2 * t * (b[0] - m[0]), dy = 2 * (1 - t) * (m[1] - a[1]) + 2 * t * (b[1] - m[1]), ang = Math.atan2(dy, dx);
      if (i % 2) shape(g, ellD(x, y, 5 * k, 1.6 * k, ang), { fill: '#6f6a62', tex: 'rust', texA: 0.8, tone: 0.3, lw: 1.1, hatch: false, rim: false });
      else { g.save(); g.translate(x, y); g.rotate(ang); g.lineWidth = 3.4 * k; g.strokeStyle = INK; g.beginPath(); g.ellipse(0, 0, 5 * k, 3 * k, 0, 0, Math.PI * 2); g.stroke(); g.lineWidth = 1.6 * k; g.strokeStyle = '#8a6a4a'; g.stroke(); g.restore(); }
    }
  }

  // A fly, with a little looping flight path.
  function fly(g, x, y, s = 1, loop = true) {
    if (loop) { g.save(); g.setLineDash([2, 2.6]); line(g, `M${x - 14 * s},${y + 6 * s} C${x - 22 * s},${y - 10 * s} ${x - 4 * s},${y - 14 * s} ${x - 3 * s},${y - 2 * s}`, 0.8 * s); g.restore(); }
    g.save(); g.fillStyle = 'rgba(220,230,235,0.75)'; g.strokeStyle = INK; g.lineWidth = 0.6 * s;
    g.beginPath(); g.ellipse(x - 1.6 * s, y - 2.4 * s, 2.6 * s, 1.4 * s, -0.6, 0, Math.PI * 2); g.fill(); g.stroke();
    g.beginPath(); g.ellipse(x + 1.6 * s, y - 2.4 * s, 2.6 * s, 1.4 * s, 0.6, 0, Math.PI * 2); g.fill(); g.stroke();
    g.fillStyle = INK; g.beginPath(); g.ellipse(x, y, 1.6 * s, 2.4 * s, 0, 0, Math.PI * 2); g.fill(); g.restore();
  }

  // An iso cylinder standing at base (x,y): returns nothing, draws body + top.
  function cyl(g, x, y, r, h, o = {}) {
    const ry = r * 0.42;
    const body = `M${x - r},${y - h} L${x - r},${y} C${x - r},${y + ry * 1.33} ${x + r},${y + ry * 1.33} ${x + r},${y} L${x + r},${y - h} Z`;
    shape(g, body, { fill: o.fill || '#777', tex: o.tex, texA: o.texA, tone: o.tone ?? 0.55, lw: o.lw ?? 1.8, ang: -1.2 });
    shape(g, ellD(x, y - h, r, ry), { fill: o.top || o.fill || '#888', tex: o.tex, texA: o.texA, tone: 0.2, lw: o.lw ?? 1.8, hatch: false });
  }

  // A glass jar with something in it, a cork or lid, a paper label.
  function jar(g, x, y, r, h, o = {}) {
    const ry = r * 0.42, lv = y - h * (o.level ?? 0.78);
    const body = `M${x - r},${y - h} L${x - r},${y} C${x - r},${y + ry * 1.33} ${x + r},${y + ry * 1.33} ${x + r},${y} L${x + r},${y - h} Z`;
    // contents
    g.save(); g.clip(new Path2D(body));
    shape(g, `M${x - r},${lv} L${x - r},${y + ry * 2} L${x + r},${y + ry * 2} L${x + r},${lv} Z`, { fill: o.fill || '#8a9a4a', tex: o.tex || 'grime', texA: 0.6, tone: 0.55, lw: 0, rim: false, ang: -1.3 });
    if (o.inside) o.inside(g, x, y, r, h, lv);
    g.restore();
    // glass: a pale wash, a long highlight, the outline
    shape(g, body, { fill: 'rgba(205,215,200,0.16)', tone: 0.18, lw: 1.9, hatch: false, rim: false });
    taper(g, [[x - r * 0.6, y - h + 4], [x - r * 0.64, y - h * 0.5], [x - r * 0.6, y - 3]], 2.6, 'rgba(255,255,250,0.7)');
    taper(g, [[x + r * 0.55, y - h * 0.7], [x + r * 0.58, y - h * 0.45]], 1.4, 'rgba(255,255,250,0.5)');
    if (o.label) {
      shape(g, `M${x - r * 0.35},${y - h * 0.55} L${x + r * 0.8},${y - h * 0.5} L${x + r * 0.78},${y - h * 0.22} L${x - r * 0.36},${y - h * 0.27} Z`, { fill: '#cbbf9c', tex: 'grime', texA: 0.9, tone: 0.3, lw: 1, hatch: false, rim: false });
      for (let i = 0; i < 2; i++) taper(g, [[x - r * 0.2, y - h * (0.45 - i * 0.1)], [x + r * 0.3, y - h * (0.44 - i * 0.1) + J(1)], [x + r * 0.65, y - h * (0.43 - i * 0.1)]], 1, '#3a2c20', 'none');
    }
    // the lid or the cork
    if (o.cork) cyl(g, x, y - h + 2, r * 0.72, 9, { fill: '#a07f52', tex: 'leather', tone: 0.4, lw: 1.5 });
    else cyl(g, x, y - h + 1, r + 1.2, 6, { fill: o.lid || '#6b5d48', tex: 'rust', texA: 0.8, tone: 0.45, lw: 1.6 });
  }

  // An iso box (crate / battery): base front corner at (x,y), w along (0.87,-0.5) right, d along (-0.87,-0.5) left, h up.
  function box(g, x, y, w, d, h, o = {}) {
    const R = [0.87 * w, -0.5 * w], L = [-0.87 * d, -0.5 * d];
    const p0 = [x, y], p1 = [x + R[0], y + R[1]], p2 = [x + R[0] + L[0], y + R[1] + L[1]], p3 = [x + L[0], y + L[1]];
    const up = (p) => [p[0], p[1] - h];
    shape(g, poly([p3, p0, up(p0), up(p3)]), { fill: o.left || o.fill, tex: o.tex, texA: o.texA, tone: 0.35, lw: o.lw ?? 1.8, ang: -1.5 });
    shape(g, poly([p0, p1, up(p1), up(p0)]), { fill: o.right || o.fill, tex: o.tex, texA: o.texA, tone: 0.75, lw: o.lw ?? 1.8, ang: -1.5 });
    shape(g, poly([up(p0), up(p1), up(p2), up(p3)]), { fill: o.top || o.fill, tex: o.tex, texA: o.texA, tone: 0.15, lw: o.lw ?? 1.8, hatch: false });
    return { p0, p1, p2, p3, up };
  }

  // One-Hand Bonney, concept sticker: her, at the crank of her spit (camp.js draws the fire).
  const C = {
    skin: '#bfa784', skinD: '#8e7656', skinDD: '#5a4632', skinL: '#d8c3a0', pus: '#e4d79a', hair: '#a39d90', lip: '#9c6458',
    coat: '#5c5940', coatD: '#3b3827', burlap: '#8b7a58', rag: '#6e6556', trou: '#4f4a40', tape: '#a3a39c',
    leather: '#4b3a2b', rubber: '#2b2724', sole: '#231d19',
    paint: '#6c8a86', paintD: '#4b6360', chrome: '#d6d1c2', brass: '#a8853e', steel: '#8d8f8a',
    rustC: '#8e4a26', bone: '#ddd3bb', tooth: '#e3dbc3', gold: '#cfa63c', rot: '#9d8b62',
    tongue: '#2a2e44', tongueL: '#59618a', mouth: '#2a1614', eye: '#e1b23a', blood: '#6f2a1c',
    stopRed: '#8e2a20', wood: '#8a6a44', fog: '#b8bab3', bile: '#8c9a40', amber: '#b97c32'
  };

  // ── BONNEY: facing lower-right, her one hand out on the crank handle at
  // (600,340) in these units. camp.js scales her into the left of its cell.
  function drawBonney(g) {
    seed(202);
    halftone(g, 404, 520, 104, 17, 0);
    // tow rope from the lip to the knot at the small of her back

    // far leg (behind), heel up, foot bound in tyre rubber
    shape(g, 'M336,428 L368,428 L360,470 L352,496 L334,496 L338,466 Z', { fill: C.rag, tex: 'cloth', texA: 1, tone: 0.8 });
    for (const y of [444, 462, 480]) taper(g, [[336, y], [352, y + 3], [366, y - 1]], 2, C.coatD);
    shape(g, 'M332,492 L356,490 C366,498 374,506 380,514 C382,520 376,523 368,522 L344,519 C336,513 330,503 332,492 Z', { fill: C.rubber, tex: 'leather', texA: 1, tone: 0.6 });
    for (let i = 0; i < 4; i++) line(g, `M${344 + i * 8},${504 + i * 3} l4,-4 l4,4`, 1, '#5a544c');
    line(g, 'M336,500 L360,494 M340,508 L370,502', 1.6, '#b39a6a');
    // near leg: rags and duct tape, the busted boot with the toes out
    shape(g, 'M406,428 L440,428 L446,462 L450,494 L426,496 L418,466 Z', { fill: C.trou, tex: 'cloth', texA: 1, tone: 0.55 });
    shape(g, 'M412,444 L444,442 L446,453 L414,456 Z', { fill: C.tape, tex: 'grime', texA: 0.9, tone: 0.3, lw: 1.4 });
    shape(g, 'M418,468 L447,466 L449,477 L420,480 Z', { fill: C.tape, tex: 'grime', texA: 0.9, tone: 0.3, lw: 1.4 });
    shape(g, 'M418,486 L452,484 L460,498 C476,502 494,508 500,516 C502,522 496,526 486,526 L420,526 C414,524 412,516 416,508 Z', { fill: C.leather, tex: 'leather', texA: 1, tone: 0.55, lw: 2.6 });
    shape(g, 'M414,519 L501,519 C503,524 500,529 492,529 L418,529 C413,528 412,523 414,519 Z', { fill: C.sole, tone: 0.3, lw: 2 });
    shape(g, 'M466,506 C478,508 491,512 499,517 L470,520 Z', { fill: '#140f0c', tone: 0, lw: 1.4, rim: false });
    shape(g, ellD(486, 514.5, 6, 3.6, 0.3), { fill: C.skinL, tex: 'skin', tone: 0.4, lw: 1.3, hatch: false });
    shape(g, ellD(495, 518, 4.6, 3, 0.3), { fill: C.skinL, tex: 'skin', tone: 0.4, lw: 1.3, hatch: false });
    shape(g, ellD(491, 513, 2.2, 1.6, 0.3), { fill: '#d1bf6a', tone: 0, lw: 0.8, rim: false });
    shape(g, ellD(499, 516.5, 1.8, 1.3, 0.3), { fill: '#d1bf6a', tone: 0, lw: 0.8, rim: false });
    line(g, 'M424,490 l6,8 l6,-9 l6,8 l6,-9', 1.4, '#b9b9b2');                                               // wire for laces
    taper(g, [[420, 500], [440, 506], [462, 504]], 1.6, 'rgba(255,246,220,0.35)');

    // the greatcoat: tarp, hunched, hem gone to strips
    let hem = '';
    for (let x = 276; x <= 458; x += 8) {
      const base = 440 - (x - 268) * 6 / 194, deep = x < 336 ? 14 + rnd() * 18 : 6 + rnd() * 12;
      hem += ` L${x},${base + deep} L${x + 4},${base + 1 + rnd() * 3}`;
    }
    const COAT = `M418,212 C404,186 384,160 358,146 C336,136 312,134 296,146 C282,158 278,180 278,206 C278,240 288,270 284,300 C280,330 272,360 270,392 L268,440${hem} L462,434 C462,400 465,360 463,322 C461,290 456,262 446,244 C438,230 428,220 418,212 Z`;
    shape(g, COAT, { fill: C.coat, tex: 'cloth', texA: 1, tone: 0.7, lw: 2.8 });
    // folds hanging off the belt, a flannel patch, burn holes
    for (const f of [[[296, 262], [288, 336], [292, 440]], [[326, 292], [322, 364], [327, 446]], [[368, 372], [372, 440]], [[410, 372], [416, 436]]]) {
      taper(g, f, 2.6, '#26231a'); taper(g, f.map(([x, y]) => [x - 3.5, y]), 1.4, 'rgba(255,246,220,0.25)');
    }
    shape(g, 'M384,392 L414,389 L416,420 L386,423 Z', { fill: '#7a3a2a', tex: 'cloth', texA: 1, tone: 0.45, lw: 1.6 });
    for (let i = 0; i < 4; i++) { line(g, `M${386 + i * 8},${392} L${387 + i * 8},${422}`, 0.8, 'rgba(20,14,10,0.6)'); line(g, `M384,${397 + i * 7} L415,${394 + i * 7}`, 0.8, 'rgba(230,200,160,0.4)'); }
    for (let i = 0; i < 8; i++) line(g, `M${384 + i * 4},${390 - (i % 2) * 2} l2,4`, 1, INK);
    for (const [x, y, r] of [[312, 394, 5], [434, 392, 3.6], [352, 414, 3]]) {
      shape(g, lumpy(x, y, r + 2.4, 0.8, 12, 0.25), { fill: '#6a4a2a', tone: 0, lw: 0, rim: false });
      shape(g, lumpy(x, y, r, 0.8, 12, 0.3), { fill: '#120c09', tone: 0, lw: 1, rim: false });
    }

    // belt of tyre tread, and what hangs off it
    shape(g, 'M274,338 Q366,362 454,344 L454,358 Q366,376 274,352 Z', { fill: C.rubber, tex: 'leather', texA: 1, tone: 0.45, lw: 2.2 });
    for (let i = 0; i < 16; i++) { const t = i / 15, x = 280 + t * 170, y = 345 + Math.sin(t * Math.PI) * 11 - t * 2; line(g, `M${x - 3},${y - 2} l3,3 l3,-3`, 1, '#5d5750'); }
    shape(g, 'M358,354 L374,355 L373,371 L357,370 Z', { fill: '#6d5a3c', tex: 'rust', texA: 1, tone: 0.4, lw: 1.6, hatch: false });
    shape(g, 'M362,358 L369,358.6 L368.6,367 L361.6,366.4 Z', { fill: C.rubber, tone: 0, lw: 1, rim: false });
    const dangle = (x0, y0, x1, y1) => line(g, `M${x0},${y0} Q${x0 + 2},${(y0 + y1) / 2} ${x1},${y1}`, 1.1, '#3a2c1c');
    dangle(300, 352, 301, 372); cyl(g, 302, 392, 7.5, 18, { fill: '#9a948a', tex: 'rust', texA: 0.9, tone: 0.5 });
    shape(g, 'M294.5,382 L309.5,382 L309.5,388 L294.5,388 Z', { fill: '#9a3a2a', tex: 'grime', tone: 0.3, lw: 0.8, hatch: false });
    // a doll's head, by its hair, one glass eye left
    dangle(338, 361, 340, 378);
    taper(g, [[340, 376], [338, 382], [341, 386]], 5, '#4a3424');
    shape(g, lumpy(341, 396, 11, 1, 18, 0.05), { fill: '#dcd0b8', tex: 'grime', texA: 1, tone: 0.55, lw: 1.8 });
    shape(g, ellD(337, 396, 3, 3.2), { fill: '#120c0a', tone: 0, lw: 0.8, rim: false });
    shape(g, ellD(346, 395.5, 3, 3.2), { fill: '#eae4d4', tone: 0, lw: 0.8, rim: false });
    shape(g, ellD(346.6, 395.5, 1.6, 1.7), { fill: '#3c6a8a', tone: 0, lw: 0, rim: false });
    line(g, 'M343,386 l-2,5 l3,3 l-1,5', 0.9, INK); line(g, 'M337,402 q4,2 8,0', 1, '#8a3a34');
    // keys
    dangle(374, 367, 373, 380);
    g.save(); g.lineWidth = 2.4; g.strokeStyle = INK; g.beginPath(); g.ellipse(373, 384, 5, 4, 0, 0, 7); g.stroke(); g.lineWidth = 1.2; g.strokeStyle = C.brass; g.stroke(); g.restore();
    for (const [dx, a, col] of [[-3, 0.3, C.brass], [1, 0, C.steel], [4, -0.35, C.brass]]) {
      g.save(); g.translate(373 + dx, 388); g.rotate(a);
      shape(g, 'M-1.6,0 L1.6,0 L1.6,12 L3.6,13 L1.6,14.4 L3.2,16 L1.6,17 L-1.6,17 Z', { fill: col, tone: 0.3, lw: 1, hatch: false });
      shape(g, ellD(0, -1, 3.2, 2.6), { fill: col, tone: 0.2, lw: 1, hatch: false });
      g.restore();
    }
    // a gas-mask filter, and a bone
    dangle(410, 366, 411, 374); cyl(g, 411, 396, 9.5, 20, { fill: '#5d6448', tex: 'metal', texA: 0.9, top: '#6f7656', tone: 0.6 });
    for (const y of [381, 387, 393]) line(g, `M401.5,${y} Q411,${y + 4} 420.5,${y}`, 1, '#2e3224');
    dangle(436, 360, 437, 372);
    shape(g, 'M432,372 C428,370 428,376 432,376 L433,388 C429,388 429,394 434,392 C436,395 440,394 439,390 L438,377 C442,377 442,371 438,372 C437,368 433,368 432,372 Z', { fill: C.bone, tex: 'grime', tone: 0.45, lw: 1.4, hatch: false });

    // the burlap cape over the hump
    let fringe = '';
    const fr = [];
    for (let i = 0; i <= 20; i++) { const t = i / 20, x = 292 + t * 158, y = 262 - t * 10; fr.push([x, y]); fringe += ` L${x.toFixed(1)},${(y + (i % 2 ? 8 + rnd() * 9 : rnd() * 3)).toFixed(1)}`; }
    const CAPE = `M420,214 C404,188 382,162 356,150 C334,140 312,138 298,148 C286,158 282,180 284,206 C285,226 288,244 292,262${fringe} L450,252 C446,240 438,230 430,222 C426,218 423,216 420,214 Z`;
    shape(g, CAPE, { fill: C.burlap, tex: 'burlap', texA: 1, tone: 0.62, lw: 2.6 });
    fr.forEach(([x, y], i) => { if (i % 2) taper(g, [[x, y + 8], [x + J(2), y + 16 + rnd() * 8]], 1.5, '#5a4a30', 'start'); });
    for (const f of [[[302, 176], [298, 216], [300, 252]], [[336, 168], [342, 210], [344, 256]], [[380, 178], [390, 218], [394, 252]]]) taper(g, f, 2.2, '#3e3220');
    // a cut-up STOP sign, sewn on as a patch, a bullet hole through it
    g.save(); g.translate(336, 310); g.rotate(-0.1);
    const oct = []; for (let i = 0; i < 8; i++) { const a = (i + 0.5) / 8 * Math.PI * 2; oct.push([Math.cos(a) * 25, Math.sin(a) * 23]); }
    oct.splice(1, 1, [14, 22], [17, 15], [22, 17], [24, 10]);    // a corner torn off
    shape(g, poly(oct), { fill: C.stopRed, tex: 'grime', texA: 1, tone: 0.4, lw: 2.2 });
    g.save(); g.clip(new Path2D(poly(oct)));
    g.lineWidth = 2.4; g.strokeStyle = 'rgba(232,226,210,0.9)';
    const oc2 = []; for (let i = 0; i < 8; i++) { const a = (i + 0.5) / 8 * Math.PI * 2; oc2.push([Math.cos(a) * 20.5, Math.sin(a) * 18.8]); }
    g.stroke(new Path2D(poly(oc2)));
    g.font = '900 12.5px "Arial Black", Impact, sans-serif'; g.fillStyle = 'rgba(232,226,210,0.92)'; g.textAlign = 'center'; g.textBaseline = 'middle'; g.fillText('STOP', 0, 1);
    g.drawImage(TEX.grime, -336, -310, U, U); g.drawImage(TEX.rust, -336, -310, U, U);
    g.restore();
    for (let i = 0; i < 14; i++) { const a = (i + 0.3) / 14 * Math.PI * 2, x = Math.cos(a) * 25, y = Math.sin(a) * 23; line(g, `M${x - 2},${y - 2} L${x + 2},${y + 2} M${x + 2},${y - 2} L${x - 2},${y + 2}`, 1.1, '#d8cba8'); }
    shape(g, ellD(-13, 9, 2.6, 2.4), { fill: '#0e0a08', tone: 0, lw: 1, rim: false }); line(g, 'M-16,7 l-3,-2 M-11,6 l1,-3', 0.8, '#d8cba8');
    g.restore();
    // a rolled blanket lashed across the top of the hump
    g.save(); g.translate(318, 152); g.rotate(-0.5);
    shape(g, 'M-38,-12 L34,-12 C42,-12 42,12 34,12 L-38,12 Z', { fill: '#6b6a5a', tex: 'cloth', texA: 1, tone: 0.55, lw: 2.4, ang: -1.2 });
    for (const x of [-26, -20, 14, 20]) line(g, `M${x},-11 L${x},11`, 2.6, '#8a2f26');
    shape(g, ellD(-38, 0, 6, 12), { fill: '#7a7968', tex: 'cloth', tone: 0.4, lw: 2 });
    line(g, 'M-38,0 C-36,-4 -41,-6 -42,-1 C-43,5 -35,7 -34,0 C-33,-8 -42,-10 -44,-3', 1, INK);
    for (const x of [-4, 24]) { line(g, `M${x},-14 C${x + 4},-4 ${x + 4},4 ${x},14`, 5, INK); line(g, `M${x},-14 C${x + 4},-4 ${x + 4},4 ${x},14`, 3, '#8f7a55'); }
    g.restore();
    // the hood, rolled down and bunched at the back of her neck
    shape(g, lumpy(426, 208, 19, 0.9, 16, 0.14), { fill: C.coat, tex: 'cloth', texA: 1, tone: 0.6, lw: 2.4 });
    taper(g, [[414, 200], [424, 212], [436, 214]], 1.8, '#26231a');
    // the harness rope over her back to the shoulder, and its knot
    rope(g, [296, 302], [412, 224], 4.4, -4);
    shape(g, lumpy(296, 302, 8, 0.85, 12, 0.2), { fill: '#8f7a55', tex: 'burlap', tone: 0.5, lw: 2 });

    // the empty left sleeve, folded across her chest, safety-pinned, the bootlace knot
    shape(g, 'M422,250 C434,260 448,276 456,292 L459,307 C446,300 431,286 418,270 Z', { fill: C.coat, tex: 'cloth', texA: 1, tone: 0.55, lw: 2 });
    shape(g, lumpy(457, 310, 7, 0.9, 12, 0.25), { fill: C.coatD, tex: 'cloth', tone: 0.6, lw: 1.8 });
    g.save(); g.strokeStyle = INK; g.lineWidth = 2.6; g.beginPath(); g.ellipse(452, 290, 10, 3.4, 0.62, 0, 7); g.stroke(); g.strokeStyle = '#c9c6bc'; g.lineWidth = 1.3; g.stroke(); g.restore();
    shape(g, ellD(459, 296, 3, 2.4, 0.6), { fill: '#c9c6bc', tone: 0.3, lw: 1, hatch: false });
    taper(g, [[456, 314], [452, 324], [448, 332]], 2, '#c7a35a'); taper(g, [[458, 314], [462, 326], [460, 336]], 2, '#c7a35a');
    taper(g, [[457, 312], [450, 307], [452, 316], [457, 312], [464, 308], [462, 317], [457, 312]], 1.8, '#c7a35a', 'none');

    // neck, wattle, the necklace of other people's teeth and bottle caps
    shape(g, 'M414,206 C428,194 448,194 462,202 L476,252 C468,264 446,266 430,258 C422,242 416,224 414,206 Z', { fill: C.skin, tex: 'skin', texA: 1, tone: 0.62 });
    shape(g, 'M474,254 C488,266 506,272 520,268 C514,282 492,286 476,278 Z', { fill: C.skinL, tex: 'skin', texA: 1, tone: 0.55, lw: 2 });
    for (const f of [[[426, 210], [436, 228], [440, 250]], [[440, 204], [450, 224], [454, 250]], [[482, 272], [496, 278], [510, 276]]]) taper(g, f, 1.8, C.skinDD);
    line(g, 'M424,250 Q446,270 474,262', 1.2, '#3a2c1c');
    for (let i = 0; i < 9; i++) {
      const t = i / 8, x = 424 + t * 50, y = 250 + Math.sin(t * Math.PI) * 13 + t * 12 - t * t * 2;
      if (i % 3 === 1) { shape(g, ellD(x, y + 3, 4, 3.2), { fill: i === 4 ? '#9e2f24' : '#b8b2a2', tex: 'rust', texA: 0.5, tone: 0.3, lw: 1, hatch: false }); for (let k = 0; k < 8; k++) { const a = k / 8 * 6.28; line(g, `M${x + Math.cos(a) * 3.6},${y + 3 + Math.sin(a) * 2.8} l${Math.cos(a) * 1.4},${Math.sin(a) * 1.2}`, 0.6, INK); } }
      else shape(g, `M${x - 2.2},${y} L${x + 2.2},${y} L${x + 0.4},${y + 7} Z`, { fill: i === 6 ? C.gold : C.tooth, tone: 0.3, lw: 0.9, hatch: false });
    }

    // the near arm: sleeve to the elbow, the bandaged forearm out to the crank
    crankArm(g);
    // the hubcap on her shoulder, wired on
    shape(g, ellD(424, 228, 31, 19, 0.36), { fill: '#b9b5aa', tex: 'metal', texA: 1, tone: 0.45, lw: 2.4 });
    shape(g, ellD(425, 228, 20, 12, 0.36), { fill: '#a39f94', tex: 'rust', texA: 0.7, tone: 0.4, lw: 1.4, hatch: false });
    shape(g, ellD(426, 228, 7.5, 4.6, 0.36), { fill: '#cfcbbf', tone: 0.3, lw: 1.3, hatch: false });
    shape(g, ellD(426.5, 228.2, 2.2, 1.5, 0.36), { fill: INK, tone: 0, lw: 0, rim: false });
    for (let k = 0; k < 8; k++) { const a = k / 8 * 6.283, c = Math.cos(a), s = Math.sin(a), R = (x, y) => [425 + x * Math.cos(0.36) - y * Math.sin(0.36), 228 + x * Math.sin(0.36) + y * Math.cos(0.36)]; const p1 = R(c * 9, s * 5.4), p2 = R(c * 18, s * 11); line(g, `M${p1} L${p2}`, 0.9, 'rgba(26,21,18,0.7)'); }
    line(g, 'M398,224 l-4,6 l5,2 l-4,6 M450,240 l4,6 l-5,2 l4,6', 1.6, '#c9c6bc');

    crankFist(g);

    // ── the head, facing right, 3/4
    g.save(); g.translate(504, 224); g.scale(0.92, 0.92);
    drawHead(g);
    g.restore();

    // flies round her mouth, dust at her feet
    seed(19); fly(g, 584, 206, 1.2); fly(g, 566, 170, 1); fly(g, 590, 262, 1.1, false); fly(g, 548, 300, 0.9);
    for (const [x, y, r] of [[512, 520, 7], [522, 512, 5], [326, 516, 6], [316, 510, 4]]) shape(g, lumpy(x, y, r, 0.7, 12, 0.2), { fill: '#a7a399', tex: 'grime', tone: 0.3, lw: 1.2, hatch: false });
  }

  // Her head, facing right, 3/4: a human hag. Hooked nose with a wart on it,
  // a scowl under bushy brows, crooked teeth, a pointed bristly chin, a long
  // stretched earlobe with a hoop in it, wild matted grey hair under a rag.
  // The only thing not quite human is the eyes: the irises are a muddy gold.
  function drawHead(g) {
    seed(303);
    // the matted mass of hair behind the head, and hanks of it down her back
    shape(g, 'M-28,-42 C-50,-34 -60,-8 -58,18 C-56,36 -50,50 -42,60 L-30,44 C-34,26 -34,4 -26,-14 Z', { fill: C.hair, tex: 'cloth', texA: 1, tone: 0.65, lw: 2.2 });
    for (let i = 0; i < 10; i++) taper(g, [[-18 - i * 3, -46 + i * 3], [-44 - i * 1.4 + J(3), -18 + i * 4], [-54 - i * 0.6 + J(4), 16 + i * 4], [-50 - i * 1.6 + J(5), 46 + i * 5]], 2.4, i % 3 ? '#6e695e' : '#2a2620', 'end');
    // the skull and face
    const HEAD = 'M-40,-8 C-42,-32 -24,-50 0,-50 C18,-50 30,-42 33,-28 C34,-22 35,-19 38,-16 C44,-10 52,-4 58,2 C60,5 57,8 53,8 C50,8 48,8 47,9 C48,11 49,13 48,15 C50,18 50,22 48,24 C47,27 46,29 46,31 C50,34 55,40 52,45 C48,50 40,51 32,50 C20,52 6,52 -6,46 C-16,42 -24,36 -30,26 C-37,16 -40,4 -40,-8 Z';
    shape(g, HEAD, { fill: C.skin, tex: 'skin', texA: 0.9, tone: 0.62, lw: 2.8 });
    // the rag tied round her head, its knot and tails at the back
    shape(g, 'M-41,-20 C-30,-40 -6,-50 22,-44 L26,-35 C0,-41 -26,-33 -40,-9 Z', { fill: '#6a3a30', tex: 'cloth', texA: 1, tone: 0.5, lw: 2 });
    shape(g, 'M-44,-20 C-52,-16 -56,-6 -54,2 L-48,0 C-49,-6 -46,-12 -42,-14 Z', { fill: '#5a3028', tex: 'cloth', tone: 0.6, lw: 1.6 });
    shape(g, 'M-42,-16 C-48,-8 -48,4 -44,12 L-39,9 C-42,2 -41,-6 -38,-12 Z', { fill: '#6a3a30', tex: 'cloth', tone: 0.5, lw: 1.6 });
    for (let i = 0; i < 22; i++) {
      const t = i / 21, bx = -40 + t * 58, by = -18 - Math.sin(t * Math.PI * 0.9) * 28 - t * 6;
      const a = -Math.PI / 2 - 0.9 + t * 0.6 + J(0.35), L = 8 + rnd() * 12;
      taper(g, [[bx, by], [bx + Math.cos(a) * L * 0.5 + J(2), by + Math.sin(a) * L * 0.5], [bx + Math.cos(a + J(0.5)) * L, by + Math.sin(a + J(0.5)) * L]], 2.2, i % 3 ? '#9a9488' : '#3a352e', 'start');
    }
    // greasy hanks hanging out from under the rag at the temple
    for (const h of [[[-6, -38], [-12, -24], [-15, -8], [-13, 6]], [[-14, -36], [-22, -20], [-25, -2]], [[2, -40], [-1, -32], [-3, -24]]]) taper(g, h, 2.6, '#7c776c', 'end');
    // the ear: big, stretched lobe, a brass hoop through it
    g.save(); g.translate(-12, 0); g.scale(0.78, 0.78); g.translate(12, 0);
    shape(g, 'M-10,-20 C-18,-22 -22,-12 -20,-2 C-19,4 -16,8 -13,10 C-12,16 -14,21 -10,21 C-6,19 -7,11 -6,4 C-5,-4 -4,-16 -10,-20 Z', { fill: C.skin, tex: 'skin', tone: 0.55, lw: 2 });
    taper(g, [[-11, -15], [-16, -8], [-14, 0], [-10, 2]], 1.6, C.skinDD);
    g.save(); g.lineWidth = 3.8; g.strokeStyle = INK; g.beginPath(); g.ellipse(-10, 28, 6.4, 8.6, 0.15, -1.3, 4.4); g.stroke(); g.lineWidth = 2.1; g.strokeStyle = C.brass; g.stroke(); g.restore();
    g.restore();
    // furrowed forehead, the scowl lines between the brows, liver spots
    taper(g, [[0, -34], [14, -36], [28, -32]], 1.5, C.skinDD); taper(g, [[4, -29], [16, -30], [27, -27]], 1.3, C.skinDD);
    taper(g, [[29, -24], [28.4, -30]], 1.4, C.skinDD); taper(g, [[32, -24], [31.6, -29]], 1.2, C.skinDD);
    for (const [x, y, r] of [[-10, -30, 2.4], [-4, -37, 1.6], [-22, -16, 2], [-26, 6, 1.8], [8, 30, 1.6]]) shape(g, ellD(x, y, r, r * 0.8), { fill: 'rgba(110,70,40,0.7)', tone: 0, lw: 0, rim: false });
    // the near eye: deep-set, bagged, bloodshot, a muddy gold iris
    shape(g, 'M3,-20 C10,-26 24,-26 29,-18 C28,-11 22,-7 15,-7 C8,-7 3,-12 3,-20 Z', { fill: 'rgba(70,50,36,0.55)', tone: 0, lw: 0, rim: false });
    shape(g, 'M7,-15 Q16,-21 25,-15.6 Q16,-10.6 7,-15 Z', { fill: '#e2d6b8', tone: 0.3, lw: 1.3, hatch: false, rim: false });
    line(g, 'M8,-15 l3,0.6 M9,-14 l2,1.6 M24,-15.6 l-3,0.8', 0.6, '#a33a24');
    shape(g, ellD(18, -15.6, 3.4, 3.4), { fill: '#a08c34', tone: 0.3, lw: 1, hatch: false, rim: false });
    g.fillStyle = INK; g.beginPath(); g.arc(18.4, -15.6, 1.6, 0, 7); g.fill();
    g.fillStyle = '#fff'; g.beginPath(); g.arc(19.6, -16.8, 0.8, 0, 7); g.fill();
    taper(g, [[5, -15.6], [12, -21], [20, -21.4], [27, -16]], 3, INK);                                        // the heavy upper lid
    taper(g, [[9, -12], [16, -11], [24, -14]], 1, C.skinDD);
    taper(g, [[8, -9], [16, -6], [25, -9]], 1.4, C.skinDD); taper(g, [[10, -5], [18, -2], [26, -5]], 1.2, C.skinDD);   // bags
    for (const c of [[[4, -15], [-3, -18]], [[4, -14], [-3, -12]], [[5, -12], [-1, -8]]]) taper(g, c, 1.1, C.skinDD);  // crow's feet
    // the far eye, mostly behind the bridge of the nose
    shape(g, 'M31,-16 Q34.6,-19.4 38,-16.6 Q34.6,-13.4 31,-16 Z', { fill: '#e2d6b8', tone: 0.3, lw: 1, hatch: false, rim: false });
    shape(g, ellD(35, -16.2, 2, 2.2), { fill: '#a08c34', tone: 0, lw: 0.6, rim: false });
    g.fillStyle = INK; g.beginPath(); g.arc(35.3, -16.2, 1, 0, 7); g.fill();
    taper(g, [[30.6, -16.4], [34.6, -19.6], [38.4, -17]], 2.2, INK);
    // brows: bushy, grey, dragged down into a scowl
    for (let i = 0; i < 26; i++) { const t = i / 25, x = 1 + t * 26, y = -23 - Math.sin(t * Math.PI) * 3 + t * t * 4; taper(g, [[x, y + 2.2 + J(0.8)], [x + 3.6 + J(1.2), y - 2 + J(1.2)]], 2.3, i % 3 ? '#8a8478' : '#2a2620', 'end'); }
    for (let i = 0; i < 6; i++) { const t = i / 5, x = 31 + t * 7, y = -21 - t * 1.6; taper(g, [[x, y + 1.2], [x + 2.4, y - 1.2]], 1.4, i % 2 ? '#8a8478' : '#2a2620', 'end'); }
    // the nose: long, hooked, a wart with hairs in it, one dark nostril
    taper(g, [[36, -20], [44, -10], [53, -1]], 2.4, 'rgba(255,240,214,0.55)');
    taper(g, [[49, 9.6], [54, 9], [57, 7]], 2.2, 'rgba(70,50,36,0.55)');
    shape(g, 'M42,3 C44,0 48,0 50,3 C51,6 50,8 47,9 C44,9 42,6 42,3 Z', { fill: C.skin, tex: 'skin', tone: 0.35, lw: 1.1, hatch: false, rim: false });
    shape(g, 'M46.8,6 C48,5.2 50,5.6 50.6,7.2 C49.6,8.2 47.6,8.4 46.8,6 Z', { fill: '#1e140e', tone: 0, lw: 0.6, rim: false });
    shape(g, lumpy(50, -4, 3.4, 0.9, 10, 0.2), { fill: '#a88e6a', tex: 'skin', tone: 0.4, lw: 1.2, hatch: false });
    line(g, 'M50,-7 l1,-4 M51.6,-6 l3,-3 M48.4,-6.4 l-1.6,-3.4', 0.8);
    line(g, 'M49,8.4 l0.6,2.4 M50.4,8.2 l1.4,2', 0.7);                                                   // nose hair
    // cheek: hollow under the bone, boils on it; the deep fold down to the mouth
    taper(g, [[0, -2], [6, 8], [10, 20]], 2.2, 'rgba(70,50,36,0.55)');
    taper(g, [[41, 5], [34, 13], [27, 24]], 2, C.skinDD);
    for (const [x, y, r] of [[4, 12, 2.6], [10, 16, 2], [2, 18, 1.6]]) {
      shape(g, ellD(x, y, r + 1.4, (r + 1.4) * 0.9), { fill: 'rgba(160,70,50,0.6)', tone: 0, lw: 0, rim: false });
      shape(g, ellD(x, y, r, r * 0.9), { fill: C.pus, tone: 0.3, lw: 0.9, hatch: false });
      g.fillStyle = 'rgba(255,255,240,0.95)'; g.beginPath(); g.arc(x - r * 0.35, y - r * 0.35, r * 0.3, 0, 7); g.fill();
    }
    // the mouth: a mean grin, teeth like a broken fence, one gold, one gone
    shape(g, 'M48,17 C42,15.6 34,17.6 28,22 C34,26.6 42,26.4 48,23.4 Z', { fill: '#24120f', tone: 0, lw: 1.6, rim: false });
    [[45.4, 2.8, 3.4, C.tooth], [42, 3, 3.8, C.gold], [35.4, 3.2, 3.4, C.rot], [32, 2.4, 2.4, C.tooth]].forEach(([x, w, h, col]) => {
      const y = 17 + (48 - x) * 0.25;
      shape(g, `M${x - w / 2},${y - 0.4} L${x - w / 2 + 0.4},${y + h} L${x + w / 2},${y + h - 0.6} L${x + w / 2},${y - 0.4} Z`, { fill: col, tone: 0.2, lw: 0.8, hatch: false, rim: false });
    });
    [[44, 2.6, 2.8], [38.6, 2.8, 3.2], [33.6, 2.2, 2]].forEach(([x, w, h]) => {
      const y = 24.4 - (48 - x) * 0.05;
      shape(g, `M${x - w / 2},${y} L${x - w / 2 + 0.3},${y - h} L${x + w / 2},${y - h + 0.5} L${x + w / 2},${y} Z`, { fill: C.tooth, tone: 0.2, lw: 0.8, hatch: false, rim: false });
    });
    taper(g, [[27, 21.6], [36, 16.4], [49, 16.2]], 2.4, INK, 'none');                                       // thin upper lip
    shape(g, 'M29,23.4 C36,28 44,27.6 48.4,23.6 C48,27.4 44,30 37,29.6 C33,29 30,26.6 29,23.4 Z', { fill: C.lip, tex: 'skin', tone: 0.45, lw: 1.4, hatch: false });
    shape(g, lumpy(43.6, 28, 2.2, 0.8, 8, 0.25), { fill: '#8a2c20', tone: 0.2, lw: 0.8, hatch: false });       // a cold sore
    taper(g, [[27.4, 22], [23.6, 25.6]], 1.6, C.skinDD);
    // the tongue tip, out at the front, tasting her lip
    shape(g, 'M47,20 C50,18.6 54,19 55.4,21 C55,23.4 51,24 48,23 Z', { fill: '#9a5a5e', tex: 'skin', tone: 0.4, lw: 1.3, hatch: false, rimC: 'rgba(255,220,220,0.6)' });
    taper(g, [[52, 23.6], [52.6, 30], [51.6, 38]], 1.3, 'rgba(214,228,188,0.95)', 'none');                   // drool
    shape(g, ellD(51.4, 39.8, 1.8, 2.2), { fill: 'rgba(214,228,188,0.95)', tone: 0, lw: 0.6, hatch: false });
    // the chin: pointed, bristly, a mole with a hair out of it; a jowl
    shape(g, ellD(43, 39, 2.2, 2), { fill: '#6a4a32', tone: 0, lw: 0.8, hatch: false });
    line(g, 'M43,37 q1,-5 4,-7 M43.6,38 q3,-3 6,-2', 0.8);
    for (let i = 0; i < 12; i++) { const x = 36 + rnd() * 14, y = 32 + rnd() * 14; line(g, `M${x},${y} l${0.8 + rnd()},${1.6 + rnd()}`, 0.7); }
    taper(g, [[-6, 30], [4, 42], [16, 47]], 1.8, C.skinDD);
    // the goggles, pushed up on the rag, one lens cracked
    for (const [x, y, rx, ry] of [[22, -45, 8.6, 6.4], [1, -43, 12, 8.4]]) {
      shape(g, ellD(x, y, rx, ry, -0.16), { fill: C.brass, tex: 'metal', texA: 1, tone: 0.4, lw: 2 });
      shape(g, ellD(x + 0.4, y + 0.2, rx * 0.7, ry * 0.68, -0.16), { fill: '#33503f', tex: 'grime', texA: 0.8, tone: 0.55, lw: 1.2, hatch: false, rimC: 'rgba(200,240,210,0.6)' });
    }
    line(g, 'M-5,-47 L0,-42 L-2,-38 M0,-42 L5,-43', 0.9, 'rgba(230,240,230,0.9)');
    taper(g, [[-6, -45.6], [-3, -47.4], [1, -47.8]], 1.6, 'rgba(255,255,255,0.75)');
    line(g, 'M13,-44 L14.6,-44.6', 3, INK);
  }

  // The arm reaching forward to a spit's crank: sleeve to the elbow, the
  // bandaged forearm out level, the fist round the handle at (600,340).
  function crankArm(g) {
    shape(g, 'M404,232 C420,220 446,222 458,238 L510,286 L503,300 L495,292 L489,306 L481,296 L473,302 C452,282 426,258 404,232 Z', { fill: C.coat, tex: 'cloth', texA: 1, tone: 0.6 });
    taper(g, [[430, 238], [462, 262], [490, 290]], 2, '#26231a');
    shape(g, 'M480,298 L508,288 C540,302 566,314 592,326 L586,348 C560,338 530,326 482,312 Z', { fill: C.skin, tex: 'skin', texA: 1, tone: 0.55 });
    taper(g, [[500, 300], [540, 316], [580, 334]], 2.2, '#3c4a3a'); taper(g, [[528, 314], [546, 326], [560, 336]], 1.6, '#3c4a3a');
    for (const [x, y, r] of [[506, 306, 3], [566, 336, 2.4]]) { shape(g, lumpy(x, y, r + 1.6, 0.9, 10, 0.2), { fill: '#c49a7a', tone: 0, lw: 0, rim: false }); shape(g, lumpy(x, y, r, 0.9, 10, 0.3), { fill: '#7a2e1e', tone: 0.3, lw: 0.8, hatch: false }); }
    shape(g, 'M528,304 L543,310 L536,330 L521,324 Z', { fill: '#c4b99c', tex: 'grime', texA: 1, tone: 0.4, lw: 1.6 });
    shape(g, lumpy(533, 317, 4.4, 0.8, 12, 0.3), { fill: 'rgba(111,42,28,0.85)', tone: 0, lw: 0, rim: false });
    shape(g, 'M524,326 C520,334 518,342 520,352 L526,350 C525,342 526,336 530,329 Z', { fill: '#c4b99c', tex: 'grime', tone: 0.4, lw: 1.3, hatch: false });
  }
  function crankFist(g) {
    shape(g, 'M584,324 C590,316 606,316 614,324 C620,332 619,348 611,354 C603,359 591,357 586,350 C580,344 579,332 584,324 Z', { fill: C.skin, tex: 'skin', texA: 1, tone: 0.55, lw: 2.4 });
    [[613, 327], [615, 334], [615, 341], [612, 348]].forEach(([x, y], i) => {
      shape(g, `M${x - 6},${y - 3.4} C${x + 1},${y - 4.6} ${x + 5.4},${y - 1.6} ${x + 4.6},${y + 1.6} C${x + 4},${y + 4.4} ${x - 2},${y + 4.6} ${x - 6},${y + 3}`, { fill: C.skin, tex: 'skin', tone: 0.45, lw: 1.6, hatch: false });
      line(g, `M${x + 1},${y - 2.6} l-1.2,4`, 0.9, C.skinDD);
      shape(g, `M${x - 6},${y - 1.6} l-3,1.6 l3,1.6 Z`, { fill: '#cbbd7a', tone: 0, lw: 0.7, hatch: false, rim: false });
      if (i === 1 || i === 2) shape(g, `M${x - 1.6},${y - 4.4} L${x + 1.4},${y - 4.4} L${x + 1.4},${y + 4.4} L${x - 1.6},${y + 4.4} Z`, { fill: i === 1 ? C.gold : '#9a9a92', tex: 'metal', texA: 0.6, tone: 0.3, lw: 1, hatch: false });
    });
    shape(g, 'M588,322 C592,312 604,310 610,316 C613,320 609,324 604,322 C600,320 594,322 590,326 Z', { fill: C.skin, tex: 'skin', tone: 0.45, lw: 1.8, hatch: false });     // thumb over the top
    for (const [x, y] of [[596, 320], [604, 318]]) line(g, `M${x},${y} l-1,-3`, 0.7, INK);
  }

  // Supper at Bonney's: the fire pit, the truck-axle spit, Dennis Turnbull on
  // it with an apple in his mouth, and Bonney at the crank, licking her lips.

  // Bonney's walk-pose frame, scaled into the left of the cell. Her fist (600,340
  // in her own units) lands on the crank handle at CRANK below.
  const BON_K = 0.72, BON_TX = -174, BON_TY = 138;
  const CRANK = [600 * BON_K + BON_TX, 340 * BON_K + BON_TY];      // [258, 382.8]
  const AX_L = [274, 366], AX_R = [560, 318];                      // the axle, crank end to far end

  function flame(g, x, y, w, h, lean = 0) {
    const l = x - w / 2, r = x + w / 2, tx = x + lean + J(2), ty = y - h;
    shape(g, `M${l},${y} C${l - w * 0.18},${y - h * 0.45} ${x + lean - w * 0.4},${y - h * 0.68} ${tx},${ty} C${x + lean + w * 0.18},${y - h * 0.6} ${r + w * 0.22},${y - h * 0.4} ${r},${y} Z`, { fill: '#e46a26', tone: 0, lw: 2, rim: false, hatch: false });
    shape(g, `M${l + w * 0.2},${y} C${l + w * 0.08},${y - h * 0.34} ${x + lean * 0.7 - w * 0.16},${y - h * 0.52} ${x + lean * 0.72},${y - h * 0.72} C${x + lean * 0.7 + w * 0.12},${y - h * 0.48} ${r - w * 0.1},${y - h * 0.3} ${r - w * 0.2},${y} Z`, { fill: '#f3b03e', tone: 0, lw: 0, rim: false, hatch: false });
    shape(g, `M${x - w * 0.12},${y} C${x - w * 0.18},${y - h * 0.2} ${x + lean * 0.4},${y - h * 0.3} ${x + lean * 0.44},${y - h * 0.42} C${x + lean * 0.4 + w * 0.08},${y - h * 0.26} ${x + w * 0.14},${y - h * 0.14} ${x + w * 0.12},${y} Z`, { fill: '#fbe8a6', tone: 0, lw: 0, rim: false, hatch: false });
  }

  function burningTyre(g, x, y, rx, ry) {
    shape(g, ellD(x, y + 4, rx, ry), { fill: '#141110', tone: 0, lw: 2, rim: false });
    shape(g, ellD(x, y, rx, ry), { fill: '#262120', tex: 'leather', texA: 1, tone: 0.35, lw: 2.2, hatch: false });
    shape(g, ellD(x, y - 1, rx * 0.5, ry * 0.46), { fill: '#ff9a3c', tone: 0, lw: 1.6, rim: false });
    shape(g, ellD(x, y - 1, rx * 0.3, ry * 0.26), { fill: '#ffd98a', tone: 0, lw: 0, rim: false });
    for (let i = 0; i < 14; i++) { const a = i / 14 * Math.PI * 2, c = Math.cos(a), s = Math.sin(a); line(g, `M${x + c * rx * 0.62},${y + s * ry * 0.62} L${x + c * rx * 0.94},${y + s * ry * 0.94}`, 1, '#4a423c'); }
  }

  function engineBlock(g, x, y) {
    const b = box(g, x, y, 44, 30, 36, { fill: '#6c6f6d', left: '#7d807d', right: '#555856', top: '#8f928f', tex: 'rust', texA: 0.9 });
    const u = b.up;
    for (let i = 0; i < 3; i++) {
      const t = 0.2 + i * 0.3, px = u(b.p0)[0] + (u(b.p1)[0] - u(b.p0)[0]) * t + (u(b.p3)[0] - u(b.p0)[0]) * 0.5, py = u(b.p0)[1] + (u(b.p1)[1] - u(b.p0)[1]) * t + (u(b.p3)[1] - u(b.p0)[1]) * 0.5;
      shape(g, ellD(px, py, 6, 3), { fill: '#1e1f1e', tone: 0, lw: 1.2, rim: false });
    }
    for (const k of [0.3, 0.7]) { const p = [b.p0[0] + (b.p1[0] - b.p0[0]) * k, b.p0[1] + (b.p1[1] - b.p0[1]) * k]; taper(g, [[p[0], p[1] - 34], [p[0] + 1, p[1] - 18], [p[0], p[1] - 4]], 2.6, 'rgba(142,74,38,0.85)', 'start'); }
  }

  // Turnbull, in the axle's own frame: x along the bar (his feet at the crank
  // end), y across it. Thin, polite, very pink down one side, done on the other.
  function turnbull(g) {
    const PINK = '#e2a48a', PINKD = '#bf7c62', CRISP = '#9e562e', PANTS = '#ece6d6';
    g.save(); g.scale(1, 1.35);
    // far leg and arm (behind the bar's line)
    shape(g, 'M-38,-10 L-80,-9 L-116,-10 L-116,-2 L-80,-1 L-38,-2 Z', { fill: PINKD, tex: 'skin', texA: 0.6, tone: 0.5, lw: 1.8 });
    shape(g, 'M58,-13 L100,-12 L142,-8 L142,-1 L100,-4 L58,-5 Z', { fill: PINKD, tex: 'skin', texA: 0.6, tone: 0.5, lw: 1.8 });
    // near leg: thigh, knee, shin, a bare foot with its toes curled
    shape(g, 'M-38,-1 L-80,0 L-118,-1 L-118,9 L-80,11 L-38,12 Z', { fill: PINK, tex: 'skin', texA: 0.6, tone: 0.5, lw: 2 });
    shape(g, 'M-38,6 L-80,7 L-118,5 L-118,9 L-80,11 L-38,12 Z', { fill: CRISP, tex: 'grime', texA: 0.8, tone: 0.3, lw: 0, rim: false });
    shape(g, ellD(-80, 4, 6, 6.6), { fill: PINK, tone: 0.4, lw: 1.4, hatch: false });
    shape(g, 'M-118,-6 C-126,-9 -134,-6 -134,0 C-134,6 -128,10 -120,9 Z', { fill: PINK, tex: 'skin', tone: 0.45, lw: 1.8, hatch: false });
    for (let i = 0; i < 4; i++) line(g, `M${-132 + i * 0.6},${-5 + i * 3.4} q-3,0 -3.6,1.6`, 1, INK);
    // the Y-fronts
    shape(g, 'M-46,-12 C-38,-15 -24,-15 -16,-13 L-16,14 C-26,16 -40,16 -46,13 Z', { fill: PANTS, tex: 'grime', texA: 0.8, tone: 0.4, lw: 2 });
    line(g, 'M-40,-12 Q-31,0 -22,-13', 1.1, '#a8a291');
    // body: ribs, a sad little belly, browned and grill-marked along the bottom
    shape(g, 'M-17,-13 C4,-19 40,-19 62,-15 L64,14 C40,18 4,18 -17,14 Z', { fill: PINK, tex: 'skin', texA: 0.6, tone: 0.5, lw: 2.4 });
    shape(g, 'M-17,6 C4,9 40,9 64,6 L64,14 C40,18 4,18 -17,14 Z', { fill: CRISP, tex: 'grime', texA: 0.9, tone: 0.35, lw: 0, rim: false });
    for (let i = 0; i < 6; i++) taper(g, [[8 + i * 8, -14], [10 + i * 8, -4], [8 + i * 8, 4]], 1.3, PINKD);
    for (let i = 0; i < 7; i++) line(g, `M${-10 + i * 11},${15} l6,-5`, 1.6, '#4a2010');
    shape(g, ellD(-2, 0, 2, 1.4), { fill: PINKD, tone: 0, lw: 0.8, hatch: false });      // navel
    // near arm, up past his head, wrists wired at the far end
    shape(g, 'M58,2 L100,1 L142,1 L142,8 L100,9 L58,12 Z', { fill: PINK, tex: 'skin', texA: 0.6, tone: 0.5, lw: 2 });
    shape(g, 'M142,-9 C150,-10 156,-4 154,2 C152,8 146,10 142,8 Z', { fill: PINK, tone: 0.4, lw: 1.6, hatch: false });
    g.restore();
    // his head: bald, a comb-over, ears, eyebrows up, an apple in his mouth
    g.save(); g.translate(86, -1); g.scale(1.45, 1.45); g.translate(-86, 1);
    shape(g, ellD(86, -1, 17, 16), { fill: PINK, tex: 'skin', texA: 0.6, tone: 0.5, lw: 2.4 });
    shape(g, ellD(70, -2, 3.4, 5), { fill: PINKD, tone: 0.3, lw: 1.3, hatch: false });
    taper(g, [[76, -14], [86, -17], [98, -12]], 1.4, '#5b4636'); taper(g, [[74, -11], [86, -14.6], [99, -9]], 1.2, '#5b4636');
    taper(g, [[78, -6], [83, -9], [87, -8]], 1.8, INK); taper(g, [[90, -8], [94, -10], [99, -7]], 1.8, INK);
    shape(g, ellD(83, -3, 2.6, 3), { fill: '#fff', tone: 0, lw: 1, hatch: false }); shape(g, ellD(83.6, -2.6, 1.2, 1.3), { fill: INK, tone: 0, lw: 0, rim: false });
    shape(g, ellD(94, -3, 2.6, 3), { fill: '#fff', tone: 0, lw: 1, hatch: false }); shape(g, ellD(94.6, -2.6, 1.2, 1.3), { fill: INK, tone: 0, lw: 0, rim: false });
    shape(g, ellD(89, 1.4, 2.4, 1.8), { fill: PINKD, tone: 0.3, lw: 1, hatch: false });
    shape(g, ellD(89, 9.4, 10.4, 9), { fill: '#3a1a14', tone: 0, lw: 1.6, rim: false });
    shape(g, ellD(89, 9, 8.6, 7.6), { fill: '#b8302a', tex: 'grime', texA: 0.5, tone: 0.45, lw: 2, hatch: false });
    taper(g, [[84, 6], [88, 4], [92, 5]], 1.8, 'rgba(255,190,180,0.8)');
    line(g, 'M90,2 l1,-3', 1.4); shape(g, 'M91,-1 C95,-5 99,-3 100,-1 C96,1 93,1 91,-1 Z', { fill: '#6e8a3a', tone: 0, lw: 0.8, hatch: false });
    shape(g, 'M104,-14 C107,-9 107,-6 104,-5 C101,-6 101,-9 104,-14 Z', { fill: '#cfe4ee', tone: 0, lw: 1, hatch: false });       // sweat: this is awkward for everybody
    g.restore();
    g.save(); g.scale(1, 1.35);
    // fence wire round him and the bar, twisted tight
    for (const x of [-104, -62, 4, 40, 128]) {
      for (const dx of [0, 4]) { line(g, `M${x + dx},-15 L${x + dx + 2},15`, 2.8, INK); line(g, `M${x + dx},-15 L${x + dx + 2},15`, 1.2, '#c9c6bc'); }
      line(g, `M${x + 4},15 l2,4 l-3,2`, 1, INK);
    }
    g.restore();
  }

  function drawCamp(g) {
    seed(404);
    // scorched ash round the pit, the shadow under everything
    { const gr = g.createRadialGradient(410, 420, 10, 410, 420, 220); gr.addColorStop(0, 'rgba(40,34,30,0.75)'); gr.addColorStop(0.7, 'rgba(70,64,58,0.3)'); gr.addColorStop(1, 'rgba(70,64,58,0)');
      g.save(); g.translate(410, 420); g.scale(1, 0.36); g.translate(-410, -420); g.fillStyle = gr; g.beginPath(); g.arc(410, 420, 220, 0, 7); g.fill(); g.restore(); }
    halftone(g, 400, 446, 200, 60, 0);
    // black tyre smoke, rolling up off the fire and away downwind
    seed(41);
    for (let i = 0; i < 14; i++) {
      const t = i / 13, x = 430 + t * 90 + Math.sin(t * 4.4) * 8, y = 320 - t * 190, r = 14 + t * 22;
      const col = t < 0.4 ? '#2b2725' : t < 0.7 ? '#4a4541' : '#6e6862', a = 0.9 - t * 0.55;
      for (const dx of [-0.42, 0.38]) {
        const cx = x + dx * r + J(3), cy = y + J(3);
        g.globalAlpha = a;
        shape(g, lumpy(cx, cy, r * (0.8 + rnd() * 0.25), 0.84, 30, 0.05), { fill: col, tex: 'grime', texA: 0.5, tone: 0.35, lw: 1, hatch: false, rimC: 'rgba(210,200,190,0.35)' });
        g.globalAlpha = 1;
      }
    }
    // the pit: a lumpy rim of ash and slag, coals inside
    shape(g, lumpy(416, 414, 150, 0.36, 34, 0.05), { fill: '#5d5a55', tex: 'grime', texA: 1, tone: 0.55, lw: 2.6 });
    shape(g, ellD(416, 412, 128, 42), { fill: '#2a1f1b', tone: 0, lw: 2, rim: false });
    { const gr = g.createRadialGradient(416, 414, 4, 416, 414, 110); gr.addColorStop(0, '#ffd27a'); gr.addColorStop(0.45, '#e9742b'); gr.addColorStop(1, 'rgba(122,36,20,0.2)');
      g.save(); g.translate(416, 414); g.scale(1, 0.32); g.translate(-416, -414); g.fillStyle = gr; g.beginPath(); g.arc(416, 414, 110, 0, 7); g.fill(); g.restore(); }
    // what's burning: the end of a piano, chair legs, tyres
    shape(g, 'M486,402 L530,372 L542,382 L500,414 Z', { fill: '#3b2a22', tex: 'grime', tone: 0.5, lw: 2 });
    shape(g, 'M486,402 L530,372 L532,366 L488,396 Z', { fill: '#e9e2cf', tone: 0.3, lw: 1.6, hatch: false });
    for (let i = 0; i < 8; i++) { const x = 492 + i * 5, y = 393 - i * 3.4; line(g, `M${x},${y} l2.6,-1.9`, 2.4, i % 3 === 1 ? '#e9e2cf' : INK); }
    for (const [a, b] of [[[330, 404], [318, 372]], [[344, 410], [346, 378]]]) { line(g, `M${a} L${b}`, 5, INK); line(g, `M${a} L${b}`, 3, '#7a5636'); }
    burningTyre(g, 368, 414, 34, 13); burningTyre(g, 462, 424, 32, 12); burningTyre(g, 414, 398, 28, 10);
    // back flames, licking up round the man
    seed(43);
    flame(g, 382, 404, 46, 104, -8); flame(g, 426, 400, 54, 128, 6); flame(g, 470, 408, 40, 96, 12); flame(g, 340, 412, 36, 78, -12); flame(g, 506, 414, 28, 62, 14);
    // the far engine block and the axle across to the crank end
    engineBlock(g, 554, 372);
    line(g, `M${AX_L} L${AX_R}`, 11, INK); line(g, `M${AX_L} L${AX_R}`, 7.6, '#6c6f6d'); line(g, `M${AX_L[0]},${AX_L[1] - 2} L${AX_R[0]},${AX_R[1] - 2}`, 2, '#9a9d9a');
    shape(g, ellD(AX_R[0] - 4, AX_R[1] + 1, 5, 14, -0.17), { fill: '#4a4c4a', tex: 'rust', tone: 0.4, lw: 2, hatch: false });
    // Turnbull, along the bar
    const ang = Math.atan2(AX_R[1] - AX_L[1], AX_R[0] - AX_L[0]);
    g.save(); g.translate((AX_L[0] + AX_R[0]) / 2 + 2, (AX_L[1] + AX_R[1]) / 2 + 4); g.rotate(ang); turnbull(g); g.restore();
    // fat dripping into the fire
    for (const [x, y] of [[368, 368], [404, 362], [440, 356], [470, 352], [388, 384]]) shape(g, `M${x},${y} C${x + 3},${y + 6} ${x + 2},${y + 9} ${x},${y + 9} C${x - 2},${y + 9} ${x - 3},${y + 6} ${x},${y} Z`, { fill: '#f4d77a', tone: 0, lw: 0.9, hatch: false });
    // front flames over his crisp side
    seed(47);
    flame(g, 404, 422, 40, 74, 4); flame(g, 450, 428, 34, 62, 8); flame(g, 360, 426, 32, 56, -6); flame(g, 490, 430, 24, 42, 10);
    // the near engine block, the hub, the crank
    engineBlock(g, 266, 426);
    shape(g, ellD(AX_L[0] + 4, AX_L[1] + 1, 5, 14, -0.17), { fill: '#4a4c4a', tex: 'rust', tone: 0.4, lw: 2, hatch: false });
    line(g, `M${AX_L} L${CRANK[0]},${CRANK[1] - 14}`, 7, INK); line(g, `M${AX_L} L${CRANK[0]},${CRANK[1] - 14}`, 4, '#5a5c5a');
    line(g, `M${CRANK[0]},${CRANK[1] - 16} L${CRANK[0]},${CRANK[1] + 14}`, 6, INK); line(g, `M${CRANK[0]},${CRANK[1] - 16} L${CRANK[0]},${CRANK[1] + 14}`, 3, '#8a6a4a');
    // Bonney at the crank, leaning in, tasting her lip
    g.save(); g.translate(BON_TX, BON_TY); g.scale(BON_K, BON_K); drawBonney(g); g.restore();
    // the hubcap of gravy with a car aerial for a spoon
    shape(g, ellD(536, 470, 30, 12), { fill: '#b9b5aa', tex: 'metal', texA: 1, tone: 0.45, lw: 2.2 });
    shape(g, ellD(536, 468, 23, 8.6), { fill: '#6a4524', tex: 'grime', texA: 0.8, tone: 0.4, lw: 1.4, hatch: false });
    taper(g, [[524, 466], [536, 463], [548, 467]], 2, 'rgba(200,150,90,0.8)');
    line(g, 'M540,468 L566,404', 3.2, INK); line(g, 'M540,468 L566,404', 1.6, '#b9bcb9');
    shape(g, ellD(566.6, 402.6, 2.8, 2.8), { fill: '#b9bcb9', tone: 0, lw: 1, hatch: false });
    // a bone or two in the ash
    for (const [x, y, a] of [[318, 482, 0.3], [492, 494, -0.5]]) {
      g.save(); g.translate(x, y); g.rotate(a);
      shape(g, 'M-12,-2 C-16,-6 -18,0 -14,2 C-18,4 -14,8 -11,4 L10,4 C13,8 17,4 14,2 C18,0 16,-6 12,-2 Z', { fill: '#ddd3bb', tex: 'grime', tone: 0.45, lw: 1.4, hatch: false });
      g.restore();
    }
    // sparks going up, flies round the roast
    seed(53);
    for (let i = 0; i < 14; i++) { const x = 360 + rnd() * 150, y = 240 + rnd() * 90; shape(g, ellD(x, y, 1.6 + rnd(), 1.6 + rnd()), { fill: '#ff9a3c', tone: 0, lw: 0.6, hatch: false, rim: false }); }
    fly(g, 520, 300, 1.1); fly(g, 360, 300, 1); fly(g, 488, 270, 0.9, false);
  }


  // The sticker: her at her fire, drawn once into a px x px canvas.
  let texturesBuilt = false;
  function sticker(px) {
    if (!texturesBuilt) { buildTextures(); texturesBuilt = true; }
    const c = document.createElement('canvas'); c.width = px; c.height = px;
    const g = c.getContext('2d');
    g.scale(px / U, px / U); g.lineJoin = 'round'; g.lineCap = 'round';
    drawCamp(g);
    return c;
  }
  // The comic-ink kit itself, for the other code-drawn stickers (lab.js's
  // theatre). Shapes are in the same 600-unit cell, so the textures line up.
  function kit() {
    if (!texturesBuilt) { buildTextures(); texturesBuilt = true; }
    return { U, INK, seed, rnd, J, fbm, poly, shape, taper, line, ellD, lumpy, halftone, rope, chain, fly, cyl, jar, box };
  }
  return { sticker, kit };

})();
