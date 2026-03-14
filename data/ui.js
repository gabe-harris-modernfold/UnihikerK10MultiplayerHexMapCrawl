// ── Terrain card — sets reactive state; DOM updates via van.derive ─
function updateTerrainCard() {
  if (myId < 0) return;
  const me   = players[myId];
  const cell = gameMap[me.r]?.[me.q];
  if (!cell) return;
  uiCurrentCell.val = { ...cell, q: me.q, r: me.r };
}

function populateHexInfo(q, r, cell) {
  const t = TERRAIN[cell.terrain] || TERRAIN[0];
  document.getElementById('hi-icon').textContent    = t.icon;
  document.getElementById('hi-title').textContent   = t.name.toUpperCase();
  document.getElementById('hi-coords').textContent  = `Q:${q}  R:${r}`;
  document.getElementById('hi-terrain').textContent = t.name;
  document.getElementById('hi-hazard').textContent  = t.hazard;
  document.getElementById('hi-desc').textContent    = t.desc;

  document.getElementById('hi-movement').textContent =
    t.mc === 255 ? 'IMPASSABLE' : `Cost ${t.mc}×  |  Shelter ${t.sv}`;

  const visLabels = ['PENALTY — terrain only', 'STANDARD', 'HIGH — +2 hex range'];
  document.getElementById('hi-visibility').textContent = visLabels[t.vis + 1] || 'STANDARD';

  const tagList = document.getElementById('hi-tag-list');
  tagList.innerHTML = '';
  (t.tags || []).forEach(tag => {
    const b = document.createElement('span');
    b.className = TAG_CLASS[tag] || 'hi-badge';
    b.textContent = tag;
    tagList.appendChild(b);
  });

  const resList = document.getElementById('hi-res-list');
  resList.innerHTML = '';
  if (cell.resource > 0 && cell.amount > 0) {
    const b = document.createElement('span');
    b.className = RES_BADGE_CLASS[cell.resource] || 'hi-badge';
    b.textContent = `${RES_NAMES[cell.resource]} ×${cell.amount}`;
    resList.appendChild(b);
  } else {
    resList.innerHTML = '<span class="res-none-label">None visible</span>';
  }
}

document.getElementById('terrain-card').addEventListener('click', () => {
  if (myId < 0) return;
  const me   = players[myId];
  const cell = gameMap[me.r] && gameMap[me.r][me.q];
  if (!cell) return;
  const info = document.getElementById('hex-info');
  const card = document.getElementById('terrain-card');
  if (info.classList.contains('open')) {
    info.classList.remove('open');
    card.classList.remove('expanded');
  } else {
    populateHexInfo(me.q, me.r, cell);
    const cardRect = card.getBoundingClientRect();
    const wrapRect = document.getElementById('canvas-wrap').getBoundingClientRect();
    info.style.top = (cardRect.bottom - wrapRect.top + 4) + 'px';
    info.classList.add('open');
    card.classList.add('expanded');
  }
});

document.getElementById('hex-close').addEventListener('click', e => {
  e.stopPropagation();
  document.getElementById('hex-info').classList.remove('open');
  document.getElementById('terrain-card').classList.remove('expanded');
});

// ── Sidebar UI ──────────────────────────────────────────────────
function updateSidebar() {
  if (myId < 0) return;
  const me = players[myId];
  for (let i = 0; i < 5; i++) uiInv[i].val = me.inv[i];
  uiScore.val   = me.sc;
  uiPos.val     = `Q:${me.q}  R:${me.r}`;
  uiSteps.val  = me.sp ?? 0;
  uiVision.val = myVisionR;
  uiLL.val      = me.ll   ?? 6;
  uiFood.val    = me.food ?? 6;
  uiWater.val   = me.water ?? 6;
  uiResolve.val = me.res  ?? 3;
  uiMP.val      = me.mp   ?? 0;
  uiRad.val     = me.rad  ?? 0;
  uiColdExp.val = me.cx   ?? 0;
  uiHeatExp.val = me.hx   ?? 0;
  uiActUsed.val = me.au   ?? false;
  uiPlayers.val = players
    .map((p, i) => ({ p, i }))
    .filter(({ p }) => p.on)
    .map(({ p, i }) => ({ i, nm: p.nm || `P${i}`, sc: p.sc, color: PLAYER_COLORS[i], isMe: i === myId }))
    .sort((a, b) => b.sc - a.sc)
    .map((entry, rank) => ({ ...entry, rank: rank + 1 }));
  updateClock();
}

// ── Narrative time-of-day clock ──────────────────────────────────
function updateClock() {
  const el = document.getElementById('hud-clock');
  if (!el) return;
  const phase = getTimePhase();
  el.textContent = phase.icon + ' ' + phase.name;
  el.classList.toggle('phase-dark', phase.name === 'DARK WATCH');
}

// ── Character Sheet ─────────────────────────────────────────────
function openCharSheet() {
  if (myId < 0) return;
  const me = players[myId];
  document.getElementById('cs-name-input').value = me.nm || '';

  // Archetype banner
  const arch     = ARCHETYPES[me.arch] || ARCHETYPES[0];
  const archIcon = document.getElementById('cs-arch-icon');
  const archName = document.getElementById('cs-arch-name');
  if (archIcon) { archIcon.textContent = arch.icon; archIcon.style.color = arch.color; }
  if (archName) { archName.textContent = arch.name; archName.style.color = arch.color; }

  // Skills grid
  const grid = document.getElementById('cs-skills-grid');
  if (grid) {
    grid.innerHTML = '';
    SK_NAMES.forEach((name, i) => {
      const lvl = (me.sk && me.sk[i]) || 0;
      const row = document.createElement('div');
      row.className = 'cs-skill-row';
      const label = document.createElement('span');
      label.className = 'cs-skill-name';
      label.textContent = SK_SHORT[i];
      const dots = document.createElement('span');
      dots.className = 'cs-skill-dots';
      for (let d = 1; d <= 2; d++) {
        const dot = document.createElement('span');
        dot.className = 'cs-skill-dot' + (d <= lvl ? ' filled' : '');
        dots.appendChild(dot);
      }
      const lbl = document.createElement('span');
      lbl.className = 'cs-skill-level';
      lbl.textContent = lvl === 2 ? 'EXPERT' : lvl === 1 ? 'TRAINED' : '—';
      row.appendChild(label);
      row.appendChild(dots);
      row.appendChild(lbl);
      grid.appendChild(row);
    });
  }

  // Wounds & Conditions
  const wd0 = document.getElementById('cs-wd0');
  const wd1 = document.getElementById('cs-wd1');
  const wd2 = document.getElementById('cs-wd2');
  const wdCond = document.getElementById('cs-conditions');
  if (wd0) {
    const minor   = me.wd?.[0] || 0;
    const major   = me.wd?.[1] || 0;
    const grievous= me.wd?.[2] || 0;
    wd0.textContent = minor   > 0 ? `${minor}×MIN`  : '—';
    wd0.className   = 'wound-badge' + (minor   > 0 ? ' minor'   : '');
    wd1.textContent = major   > 0 ? `${major}×MAJ`  : '—';
    wd1.className   = 'wound-badge' + (major   > 0 ? ' major'   : '');
    wd2.textContent = grievous> 0 ? `${grievous}×GRV`: '—';
    wd2.className   = 'wound-badge' + (grievous> 0 ? ' grievous': '');
    const conds = [];
    if (me.sb & 0x04) conds.push('BLEED');
    if (me.sb & 0x08) conds.push('FEVER');
    wdCond.textContent = conds.join(' ');
    wdCond.style.color = conds.length ? '#CC4422' : '';
  }

  document.getElementById('char-overlay').classList.add('open');
}

document.getElementById('menu-btn').addEventListener('click', () => openMenu('main'));
document.getElementById('menu-overlay').addEventListener('click', e => {
  if (e.target === document.getElementById('menu-overlay')) closeMenu();
});

document.getElementById('char-btn').addEventListener('click', openCharSheet);
document.getElementById('char-close').addEventListener('click', () =>
  document.getElementById('char-overlay').classList.remove('open'));
document.getElementById('char-overlay').addEventListener('click', e => {
  if (e.target === document.getElementById('char-overlay'))
    document.getElementById('char-overlay').classList.remove('open');
});

document.getElementById('cs-name-btn').addEventListener('click', () => {
  const nm = document.getElementById('cs-name-input').value.trim().slice(0, 11);
  if (!nm) return;
  send({ t: 'n', name: nm });
  if (myId >= 0) { players[myId].nm = nm; updateSidebar(); }
  showToast(`Call sign set: ${nm}`);
});
document.getElementById('cs-name-input').addEventListener('keydown', e => {
  if (e.key === 'Enter') document.getElementById('cs-name-btn').click();
});

// ── Log panel ────────────────────────────────────────────────────
const logLines = [];
function addLog(html) {
  logLines.push(html);
  if (logLines.length > 40) logLines.shift();
  const inner = document.getElementById('log-inner');
  inner.innerHTML = logLines.slice(-14).map(l => `<div class="log-line">${l}</div>`).join('');
}
function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// ── Toast stack ───────────────────────────────────────────────────
const toastStack = document.getElementById('toast-stack');
const TOAST_MAX   = 3;      // max toasts visible simultaneously
const TOAST_LIFE  = 1800;   // ms before fade starts
const TOAST_FADE  = 300;    // ms fade-out duration
let   toastQueue  = [];     // pending messages
let   toastActive = 0;      // currently visible count

function _nextToast() {
  if (toastActive >= TOAST_MAX || toastQueue.length === 0) return;
  const msg = toastQueue.shift();
  toastActive++;
  const el = document.createElement('div');
  el.className   = 'toast-item';
  el.textContent = msg;
  toastStack.appendChild(el);
  setTimeout(() => el.classList.add('dying'), TOAST_LIFE);
  setTimeout(() => {
    el.remove();
    toastActive--;
    _nextToast();          // show next queued toast when a slot opens
  }, TOAST_LIFE + TOAST_FADE);
}

function showToast(msg) {
  toastQueue.push(msg);
  _nextToast();
}

// ── Flavor banner ─────────────────────────────────────────────────
const SHELTER_WARNINGS = [
  ['THE WASTES TOOK THEIR TOLL', 'build shelter before nightfall'],
  ['EXPOSURE WEAKENS YOU', 'find cover or build a camp'],
  ['YOU WOKE BLEEDING COLD', 'seek shelter before the next dusk'],
  ['THE OPEN GROUND IS KILLING YOU', 'construct a shelter — use your scrap'],
  ['ANOTHER HARD NIGHT IN THE RUINS', 'a shelter here could save your life'],
];
let bannerTimer = null;
function showBanner(main, sub) {
  const el = document.getElementById('flavor-banner');
  if (!el) return;
  el.innerHTML = main + (sub ? `<span class="banner-sub">${sub}</span>` : '');
  el.classList.remove('dying');
  el.classList.add('visible');
  if (bannerTimer) clearTimeout(bannerTimer);
  el.onclick = () => dismissBanner();
  bannerTimer = setTimeout(dismissBanner, 6000);
}
function dismissBanner() {
  const el = document.getElementById('flavor-banner');
  if (!el) return;
  el.classList.add('dying');
  setTimeout(() => { el.classList.remove('visible', 'dying'); }, 420);
  if (bannerTimer) { clearTimeout(bannerTimer); bannerTimer = null; }
}
function showShelterWarning() {
  const [main, sub] = SHELTER_WARNINGS[Math.floor(Math.random() * SHELTER_WARNINGS.length)];
  showBanner(main, sub);
}

// ── Overlays ──────────────────────────────────────────────────────
function hideConnectOverlay() {
  const el = document.getElementById('connect-overlay');
  el.classList.add('fading-out');
  setTimeout(() => { el.classList.remove('fading-out'); el.classList.add('hidden'); }, 500);
}
function setStatus(s) { uiConn.val = s; }

// ── Movement ──────────────────────────────────────────────────────
function move(dir) {
  if (myId >= 0 && uiMP.val <= 0) {
    showToast('\u26A0 Exhausted \u2014 wait for dawn');
    return;
  }
  const now = Date.now();
  if (now - lastMoveSent < moveCooldownMs - 30) {
    document.querySelectorAll('.dir-btn[data-dir]').forEach(b => {
      b.classList.remove('on-cooldown');
      void b.offsetWidth;
      b.classList.add('on-cooldown');
    });
    return;
  }
  lastMoveSent = now;
  send({ t: 'm', d: dir });
}

document.querySelectorAll('.dir-btn[data-dir]').forEach(btn => {
  const dir = parseInt(btn.dataset.dir);
  btn.addEventListener('pointerdown', e => {
    e.preventDefault(); btn.classList.add('pressed'); move(dir);
  });
  btn.addEventListener('pointerup',    e => { e.preventDefault(); btn.classList.remove('pressed'); });
  btn.addEventListener('pointerleave', () => btn.classList.remove('pressed'));

  let ht, hi;
  const clearHold = () => { clearTimeout(ht); clearInterval(hi); };
  btn.addEventListener('pointerdown',  () => {
    ht = setTimeout(() => { hi = setInterval(() => move(dir), 200); }, 350);
  });
  btn.addEventListener('pointerup',     clearHold);
  btn.addEventListener('pointerleave',  clearHold);
  btn.addEventListener('pointercancel', clearHold);
});

// Keyboard: Q=NW(3) W=N(2) E=NE(1)  A=SW(4) S=S(5) D=SE(0)
const keyMap = {
  'KeyQ':'3','KeyW':'2','KeyE':'1','KeyA':'4','KeyS':'5','KeyD':'0',
  'ArrowUp':'2','ArrowDown':'5','ArrowLeft':'3','ArrowRight':'0',
  'Numpad7':'3','Numpad8':'2','Numpad9':'1',
  'Numpad4':'4','Numpad6':'0','Numpad1':'4','Numpad2':'5','Numpad3':'0',
};
const heldKeys = new Map();
document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  // Prevent scroll keys from scrolling any overlay or page
  if (['Space','PageUp','PageDown','Home','End'].includes(e.code)) { e.preventDefault(); return; }
  if (e.key === 'Escape') {
    if (uiMenuPage.val) { closeMenu(); return; }
    document.getElementById('char-overlay').classList.remove('open');
    document.getElementById('hex-info').classList.remove('open');
    document.getElementById('terrain-card').classList.remove('expanded');
    return;
  }
  // Block movement while character selection screen is showing
  if (document.getElementById('char-select-overlay')?.classList.contains('open')) return;
  const dir = keyMap[e.code];
  if (dir === undefined) return;
  e.preventDefault();
  if (!heldKeys.has(e.code)) {
    move(parseInt(dir));
    heldKeys.set(e.code, setInterval(() => move(parseInt(dir)), 200));
  }
  // Keep keyboard focus on the game canvas so subsequent keys land here
  document.getElementById('canvas-wrap')?.focus({ preventScroll: true });
});
document.addEventListener('keyup', e => {
  if (heldKeys.has(e.code)) { clearInterval(heldKeys.get(e.code)); heldKeys.delete(e.code); }
});

// Swipe on canvas
let swipeStart = null;
const _swipeEl = document.getElementById('hexCanvas');
_swipeEl.addEventListener('pointerdown',  e => { swipeStart = { x: e.clientX, y: e.clientY }; });
_swipeEl.addEventListener('pointercancel',() => { swipeStart = null; });
_swipeEl.addEventListener('pointerup',   e => {
  if (!swipeStart) return;
  const dx = e.clientX - swipeStart.x, dy = e.clientY - swipeStart.y;
  if (Math.hypot(dx, dy) < 20) { swipeStart = null; return; }
  const ang = Math.atan2(dy, dx) * 180 / Math.PI;
  let dir;
  if      (ang <  -120) dir = 3;
  else if (ang <   -60) dir = 2;
  else if (ang <     0) dir = 1;
  else if (ang <    60) dir = 0;
  else if (ang <   120) dir = 5;
  else                  dir = 4;
  move(dir); swipeStart = null;
});

// ── VanJS sub-initializers ────────────────────────────────────────

function initHudBindings() {
  // Connection dot
  const connDot = document.getElementById('hud-conn-dot');
  if (connDot) {
    van.derive(() => {
      const conn = uiConn.val;
      connDot.style.color = (conn === 'Connected' ? 'var(--gold-hi)' : '#C06030');
    });
  }

  // Movement points
  const mpEl = document.getElementById('hud-mp');
  if (mpEl) {
    mpEl.textContent = '';
    van.add(mpEl, () => `${uiMP.val} MP`);
  }

  // Visibility for current hex
  const visEl = document.getElementById('hud-vis');
  if (visEl) {
    visEl.textContent = '';
    van.add(visEl, () => {
      if (myId < 0 || !players[myId]) return '—';
      const q = players[myId].q, r = players[myId].r;
      const cell = gameMap[r]?.[q];
      if (!cell) return '—';
      const t = TERRAIN[cell.terrain];
      const vis = t?.vis ?? 0;
      if (vis > 0) return 'HIGH VIS';
      if (vis < 0) return 'PENALTY';
      return 'STANDARD';
    });
  }

  // Inventory: sidebar + mobile HUD + char sheet; bump animation on increase
  for (let i = 0; i < 5; i++) {
    const inv = document.getElementById(`inv${i}`);
    const hr  = document.getElementById(`hr${i}`);
    const cs  = document.getElementById(`cs-inv${i}`);
    inv.textContent = '';
    van.add(inv, () => String(uiInv[i].val));
    if (hr) { hr.textContent = ''; van.add(hr, () => String(uiInv[i].val)); }
    if (cs) { cs.textContent = ''; van.add(cs, () => String(uiInv[i].val)); }

    let prevInv = 0;
    van.derive(() => {
      const v = uiInv[i].val;
      if (v > prevInv) {
        [inv, hr, cs].filter(Boolean).forEach(el => {
          el.classList.remove('bumped');
          void el.offsetWidth;
          el.classList.add('bumped');
        });
      }
      prevInv = v;
    });
  }

  // Score: display + bump + floating delta
  const scoreEl   = document.getElementById('my-score');
  const scoreWrap = document.getElementById('score-wrap');
  scoreEl.textContent = '';
  van.add(scoreEl, () => String(uiScore.val));
  let prevScore = 0;
  van.derive(() => {
    const sc    = uiScore.val;
    const delta = sc - prevScore;
    prevScore   = sc;
    if (delta <= 0) return;
    scoreEl.classList.remove('bumped');
    void scoreEl.offsetWidth;
    scoreEl.classList.add('bumped');
    if (scoreWrap) {
      const d = document.createElement('span');
      d.className   = 'score-delta';
      d.textContent = `+${delta}`;
      scoreWrap.appendChild(d);
      setTimeout(() => d.remove(), 1550);
    }
  });
  const csScore  = document.getElementById('cs-score');
  const hudScore = document.getElementById('hud-score');
  if (csScore)  { csScore.textContent  = ''; van.add(csScore,  () => String(uiScore.val)); }
  if (hudScore) { hudScore.textContent = ''; van.add(hudScore, () => String(uiScore.val)); }

  // Position
  const posEl = document.getElementById('pos-info');
  posEl.textContent = '';
  van.add(posEl, () => uiPos.val);
}

function initCharSheetBindings() {
  // Live stat elements
  const stepsEl   = document.getElementById('cs-steps');
  const visionEl  = document.getElementById('cs-vision');

  if (stepsEl)  { stepsEl.textContent  = ''; van.add(stepsEl,  () => String(uiSteps.val));  }
  if (visionEl) { visionEl.textContent = ''; van.add(visionEl, () => String(uiVision.val)); }

  // Track box renderer — builds/updates N child divs inside containerId.
  // thresholds: [{box, bit}]; firedMask: server bitmask (fth/wth).
  //   armed (bit=0): orange arrow; spent (bit=1): dim arrow.
  function renderTrackBoxes(containerId, value, thresholds, firedMask = 0, count = 6) {
    const el = document.getElementById(containerId);
    if (!el) return;
    while (el.children.length < count) {
      const b = document.createElement('div');
      b.className = 'track-box';
      el.appendChild(b);
    }
    for (let i = 1; i <= count; i++) {
      const b      = el.children[count - i];
      const filled = i <= value;
      const thr    = thresholds.find(t => t.box === i);
      const fired  = thr ? !!(firedMask & thr.bit) : false;
      b.className  = 'track-box' +
        (filled ? ' filled' : '') +
        (thr && !fired ? (filled ? ' thresh-filled'       : ' thresh')       : '') +
        (thr &&  fired ? (filled ? ' thresh-spent-filled' : ' thresh-spent') : '');
    }
  }

  // LL track (dossier)
  van.derive(() => {
    renderTrackBoxes('cs-ll-track', uiLL.val, []);
    const llVal = document.getElementById('cs-ll-val');
    const mpVal = document.getElementById('cs-mp-val');
    if (llVal) llVal.textContent = String(uiLL.val);
    if (mpVal) mpVal.textContent = String(uiMP.val);
  });

  // LL mini-track (HUD)
  van.derive(() => {
    renderTrackBoxes('hud-ll-track', uiLL.val, []);
    const v = document.getElementById('hud-ll-val');
    if (v) v.textContent = String(uiLL.val);
  });

  // Food track (thresholds at boxes 4, 2)
  van.derive(() => {
    const fth = (myId >= 0 ? players[myId] : null)?.fth ?? 0;
    renderTrackBoxes('cs-food-track', uiFood.val,
      [{ box: 4, bit: 1 }, { box: 2, bit: 2 }], fth);
    const fVal = document.getElementById('cs-food-val');
    const fTok = document.getElementById('cs-food-tokens');
    if (fVal) fVal.textContent = String(uiFood.val);
    if (fTok) fTok.textContent = String(uiInv[1].val);
  });

  // Water track (thresholds at boxes 5, 3, 1)
  van.derive(() => {
    const wth = (myId >= 0 ? players[myId] : null)?.wth ?? 0;
    renderTrackBoxes('cs-water-track', uiWater.val,
      [{ box: 5, bit: 1 }, { box: 3, bit: 2 }, { box: 1, bit: 4 }], wth);
    const wVal = document.getElementById('cs-water-val');
    const wTok = document.getElementById('cs-water-tokens');
    const cCnt = document.getElementById('cs-contam-count');
    if (wVal) wVal.textContent = String(uiWater.val);
    if (wTok) wTok.textContent = String(uiInv[0].val);
    if (cCnt) {
      const cw = (myId >= 0 ? players[myId] : null)?.cw ?? 0;
      cCnt.textContent = cw > 0 ? `\u2623${cw}` : '';
      cCnt.title = cw > 0 ? `${cw} contaminated token${cw > 1 ? 's' : ''} — consumes +1 R each` : '';
    }
  });

  // Resolve tokens (max 5)
  van.derive(() => {
    const el = document.getElementById('cs-resolve-tokens');
    if (!el) return;
    el.innerHTML = '';
    for (let i = 1; i <= 5; i++) {
      const dot = document.createElement('span');
      dot.className = 'resolve-token' + (i <= uiResolve.val ? ' filled' : '');
      el.appendChild(dot);
    }
    const rVal = document.getElementById('cs-resolve-val');
    if (rVal) rVal.textContent = String(uiResolve.val);
  });

  // Radiation track (10 boxes; colour zones: 1-3 green, 4-6 yellow, 7-10 red)
  van.derive(() => {
    const rad = uiRad.val;
    renderTrackBoxes('cs-rad-track', rad, [{ box: 4, bit: 0 }, { box: 7, bit: 0 }], 0, 10);
    const trackEl = document.getElementById('cs-rad-track');
    if (trackEl) {
      for (let i = 0; i < 10; i++) {
        const b   = trackEl.children[9 - i];
        const box = i + 1;
        if (box <= rad) {
          b.classList.add('rad-filled');
          b.classList.remove('rad-filled-yellow', 'rad-filled-red');
          if      (box >= 7) { b.classList.remove('rad-filled'); b.classList.add('rad-filled-red');    }
          else if (box >= 4) { b.classList.remove('rad-filled'); b.classList.add('rad-filled-yellow'); }
        } else {
          b.classList.remove('rad-filled', 'rad-filled-yellow', 'rad-filled-red');
        }
      }
    }
    const rVal  = document.getElementById('cs-rad-val');
    const rStat = document.getElementById('cs-rad-status');
    if (rVal)  rVal.textContent = String(rad);
    if (rStat) {
      rStat.textContent = rad >= 7 ? '\u00a0\u2622 DUSK CHECK'
                        : rad >= 4 ? '\u00a0RAD-SICK'
                        :            '';
      rStat.className = 'track-suffix' +
        (rad >= 7 ? ' rad-status-critical' : rad >= 4 ? ' rad-status-sick' : '');
    }
  });

  // §6.4 Exposure markers
  van.derive(() => {
    const cx        = uiColdExp.val;
    const hx        = uiHeatExp.val;
    const coldBadge = document.getElementById('cs-cold-badge');
    const heatBadge = document.getElementById('cs-heat-badge');
    const coldVal   = document.getElementById('cs-cold-val');
    const heatVal   = document.getElementById('cs-heat-val');
    const effects   = document.getElementById('cs-exp-effects');
    const row       = document.getElementById('cs-exposure-row');
    if (coldVal)   coldVal.textContent   = String(cx);
    if (heatVal)   heatVal.textContent   = String(hx);
    if (coldBadge) coldBadge.className   = 'exp-badge cold' + (cx > 0 ? ' active' : '');
    if (heatBadge) heatBadge.className   = 'exp-badge heat' + (hx > 0 ? ' active' : '');
    if (effects) {
      const parts = [];
      if (cx >= 1) parts.push('\u2744End\u22121');
      if (cx >= 2) parts.push('Nav\u22121');
      if (cx >= 3) parts.push('+Fat/Dawn');
      if (hx >= 1) parts.push('+H\u2082O/Dawn');
      if (hx >= 2) parts.push('For\u22121');
      if (hx >= 3) parts.push('+Fat/Dawn');
      effects.textContent = parts.length ? '\u00a0' + parts.join(' ') : '';
    }
    if (row) row.style.display = (cx > 0 || hx > 0) ? '' : 'none';
  });
}

function initMapBindings() {
  // Terrain card — reactive repaint from uiCurrentCell
  van.derive(() => {
    const cc = uiCurrentCell.val;
    if (!cc) return;
    const t     = TERRAIN[cc.terrain] || TERRAIN[0];
    const mcStr = t.mc === 255 ? 'IMPASSABLE' : `MC:${t.mc}  SV:${t.sv}`;
    document.getElementById('tc-icon').textContent = t.icon;
    document.getElementById('tc-name').textContent = t.name;
    document.getElementById('tc-sub').textContent  =
      (cc.resource > 0 && cc.amount > 0)
        ? `${RES_NAMES[cc.resource]} ×${cc.amount}`
        : mcStr;
    const badge = document.getElementById('tc-vis-badge');
    badge.className   = t.vis > 0 ? 'vis-high' : t.vis < 0 ? 'vis-penalty' : '';
    badge.textContent = t.vis > 0 ? '◉ HIGH VIS +2' : t.vis < 0 ? '◎ VIS PENALTY' : '';
    if (document.getElementById('hex-info').classList.contains('open'))
      populateHexInfo(cc.q, cc.r, cc);
  });

  // Cooldown SVG ring — own rAF loop, separate from the canvas render loop
  const cdArc = document.getElementById('cd-arc');
  if (cdArc) {
    const CIRCUM = 87.96;
    van.derive(() => {
      const frac = uiCooldown.val;
      cdArc.style.strokeDashoffset = String(CIRCUM * (1 - frac));
      cdArc.classList.toggle('cd-active', frac > 0.05);
    });
    (function tickCooldown() {
      const frac = Math.max(0, Math.min(1, 1 - (Date.now() - lastMoveSent) / moveCooldownMs));
      if (Math.abs(frac - uiCooldown.val) > 0.012) uiCooldown.val = frac;
      requestAnimationFrame(tickCooldown);
    })();
  }
}

function initActionPanel() {
  // Push roll button
  document.getElementById('check-push-btn').addEventListener('click', () => {
    if (myId < 0) return;
    send({ t: 'push' });
    document.getElementById('check-push-btn').classList.remove('push-visible');
    uiPushAvail.val = false;
  });

  const actionPanel     = document.getElementById('action-panel');
  const actionBtnList   = document.getElementById('action-btn-list');
  const actionStatusBar = document.getElementById('action-status-bar');

  // Water MP stepper
  let waterMpVal = 1;
  document.getElementById('action-water-m').addEventListener('click', () => {
    if (waterMpVal > 1) { waterMpVal--; document.getElementById('action-water-v').textContent = waterMpVal; }
  });
  document.getElementById('action-water-p').addEventListener('click', () => {
    const maxMp = Math.min(3, myId >= 0 ? (players[myId]?.mp ?? 0) : 3);
    if (waterMpVal < maxMp) { waterMpVal++; document.getElementById('action-water-v').textContent = waterMpVal; }
  });
  document.getElementById('action-water-go').addEventListener('click', () => {
    if (myId < 0) return;
    send({ t: 'act', a: ACT_WATER, mp: waterMpVal });
    closeActionPanel();
  });

  function buildTreatButtons() {
    const el = document.getElementById('action-treat-list');
    el.innerHTML = '';
    if (myId < 0) return;
    const p            = players[myId];
    const cell         = gameMap[p.r]?.[p.q];
    const inSettlement = (cell?.terrain === 9);
    const conditions   = [];
    if ((p.wd?.[0] ?? 0) > 0)   conditions.push({ id: TC_MINOR,   label: `Minor Wound \u00d7${p.wd[0]}` });
    if (p.sb & 0x04)             conditions.push({ id: TC_BLEED,   label: 'Bleeding' });
    if (p.sb & 0x08)             conditions.push({ id: TC_FEVER,   label: 'Fever' });
    if ((p.wd?.[2] ?? 0) > 0 && inSettlement)
                                 conditions.push({ id: TC_GRIEVOUS, label: `Grievous Wound \u00d7${p.wd[2]} \u2605Settl DN8` });
    if (conditions.length === 0) {
      el.innerHTML = '<div class="action-no-cond">No treatable conditions</div>';
      return;
    }
    if (inSettlement) {
      const note = document.createElement('div');
      note.className   = 'action-settl-note';
      note.textContent = '\u2605 Settlement: all Treat DN \u22122';
      el.appendChild(note);
    }
    conditions.forEach(c => {
      const btn = document.createElement('button');
      btn.className   = 'chk-action-btn';
      btn.textContent = `\u2764 Treat ${c.label}`;
      btn.addEventListener('click', () => { send({ t: 'act', a: ACT_TREAT, cnd: c.id }); closeActionPanel(); });
      el.appendChild(btn);
    });
  }

  function closeActionPanel() {
    actionPanel.setAttribute('aria-hidden', 'true');
    actionPanel.classList.remove('open');
    actionBtnList.style.display = '';
    document.getElementById('action-water-ctrl').style.display = 'none';
    document.getElementById('action-treat-ctrl').style.display = 'none';
  }

  function openActionPanel() {
    if (myId < 0) return;
    const me   = players[myId];
    const cell = gameMap[me.r]?.[me.q];
    const terr = cell?.terrain ?? null;
    const mp      = me.mp  ?? 0;
    const used    = me.au  ?? false;
    const scrap   = me.inv?.[4] ?? 0;
    const isScout = (me.arch ?? -1) === 4;  // Scout: Survey free + no action slot

    // Terrain context for action panel header
    const terrName   = (terr != null && terr <= 10) ? (TERRAIN[terr]?.name ?? 'Unknown') : 'Unknown';
    const forageHere = terr != null && TERRAIN_FORAGE_DN[terr] > 0;
    const scavHere   = terr != null && TERRAIN_SALVAGE_DN[terr] > 0;
    const waterHere  = terr != null && TERRAIN_HAS_WATER[terr] > 0;
    const terrTags   = [forageHere && 'Forage', scavHere && 'Salvage', waterHere && 'Water'].filter(Boolean).join(' · ');
    actionStatusBar.innerHTML =
      `<span class="act-mp-badge">MP: ${mp}</span>` +
      (used ? '<span class="act-used-badge">\u2297 ACTION USED</span>'
            : '<span class="act-avail-badge">\u25CF ACTION READY</span>') +
      `<span class="act-terrain-ctx">${terrName}${terrTags ? ' · ' + terrTags : ''}</span>`;

    document.getElementById('action-water-ctrl').style.display = 'none';
    document.getElementById('action-treat-ctrl').style.display = 'none';

    actionBtnList.innerHTML = '';
    const actionDefs = [
      { id: ACT_FORAGE,  icon: '\u2698', label: 'FORAGE',        mpCost: 2, desc: 'Search for food (Skill check)' },
      { id: ACT_WATER,   icon: '\u2248', label: 'COLLECT WATER', mpCost: 1, desc: 'Gather water tokens (1-3 MP)' },
      { id: ACT_SCAV,    icon: '\u26B2', label: 'SCAVENGE',      mpCost: 2, desc: 'Search for items (Skill check)' },
      { id: ACT_SHELTER, icon: '\u26FA', label: 'BUILD SHELTER', mpCost: 1, desc: 'Construct shelter — needs scrap (1–2 MP, no roll)' },
      { id: ACT_TREAT,   icon: '\u2764', label: 'TREAT',         mpCost: 2, desc: 'Field medicine (Skill check)' },
      { id: ACT_SURVEY,  icon: '\u25CE', label: 'SURVEY',        mpCost: isScout ? 0 : 1, desc: isScout ? 'Reveal terrain beyond vision — free for Scout' : 'Reveal terrain beyond vision (1 MP)' },
    ];

    actionDefs.forEach(def => {
      const available   = actAvailable(def.id, terr);
      const hasMP       = mp >= def.mpCost;
      const needsScrap  = def.id === ACT_SHELTER;
      const hasScrap    = !needsScrap || scrap > 0;
      const actionUsed  = (def.id === ACT_SURVEY && isScout) ? false : used;  // Scout ignores actUsed for Survey
      const canAct      = available && hasMP && !actionUsed && hasScrap;

      // Dynamic desc: BUILD SHELTER shows what will actually be built
      let desc = def.desc;
      if (def.id === ACT_SHELTER) {
        desc = scrap === 0 ? 'Needs scrap — none in pack'
             : scrap === 1 ? '1 scrap → shelter ⛺ (1 MP)'
             :               scrap + ' scrap → improved shelter 🏠 (2 MP)';
      }

      // Compute the inline block reason shown under the button label
      const blockReason = actionUsed  ? 'Action already used today'
                        : !available  ? (def.id === ACT_FORAGE ? 'Needs Forage terrain (Rust Forest · Marsh · Open Scrub)'
                                       : def.id === ACT_WATER  ? 'Needs Water terrain (Marsh · Flooded Ruins)'
                                       : def.id === ACT_SCAV   ? 'Needs Salvage terrain (Broken Urban · Glass Fields)'
                                       : 'Not available here')
                        : !hasMP      ? `Needs ${def.mpCost} MP (have ${mp})`
                        : !hasScrap   ? `Needs scrap (have ${scrap})`
                        : '';

      const btn = document.createElement('button');
      btn.id        = 'action-btn-' + def.id;   // stable ID for AI agents
      btn.setAttribute('role', 'listitem');
      btn.className = 'action-item-btn' + (canAct ? '' : ' action-disabled');
      btn.setAttribute('aria-label', def.label + (blockReason ? ' — ' + blockReason : ''));
      btn.innerHTML =
        `<span class="act-icon">${def.icon}</span>` +
        `<span class="act-label">${def.label}</span>` +
        `<span class="act-cost">${def.mpCost > 0 ? def.mpCost + ' MP' : 'Free'}</span>` +
        (blockReason ? `<span class="act-unavail-reason">${blockReason}</span>` : '');
      btn.title = desc;

      btn.addEventListener('click', () => {
        if (!canAct) {
          showToast(actionUsed ? '\u2297 Action already used today' : !available ? '\u2297 Not available here' : !hasMP ? '\u2297 Insufficient MP' : '\u2297 Need scrap to build');
          return;
        }
        if (def.id === ACT_WATER) {
          waterMpVal = Math.min(3, mp);
          document.getElementById('action-water-v').textContent = waterMpVal;
          document.getElementById('action-water-ctrl').style.display = '';
          actionBtnList.style.display = 'none';
          return;
        }
        if (def.id === ACT_TREAT) {
          buildTreatButtons();
          document.getElementById('action-treat-ctrl').style.display = '';
          actionBtnList.style.display = 'none';
          return;
        }
        send({ t: 'act', a: def.id });
        closeActionPanel();
      });
      actionBtnList.appendChild(btn);
    });

    actionBtnList.style.display = '';
    actionPanel.classList.add('open');
    actionPanel.setAttribute('aria-hidden', 'false');
  }

  actionPanel.addEventListener('click', e => { if (e.target === actionPanel) closeActionPanel(); });
  document.getElementById('action-hud-btn').addEventListener('click', () => {
    actionPanel.classList.contains('open') ? closeActionPanel() : openActionPanel();
  });
  document.getElementById('action-close').addEventListener('click', closeActionPanel);

  document.getElementById('rest-hud-btn').addEventListener('click', () => {
    if (uiResting.val) { showToast('\u2297 Already resting'); return; }
    send({ t: 'act', a: ACT_REST });
  });

  van.derive(() => {
    const btn = document.getElementById('action-hud-btn');
    if (btn) btn.classList.toggle('action-btn-used', uiActUsed.val);
  });
  van.derive(() => {
    const btn = document.getElementById('rest-hud-btn');
    if (!btn) return;
    btn.classList.toggle('rest-btn-used', uiResting.val);
    // Pulse when exhausted (out of MP) and not yet resting — nudge player to rest
    btn.classList.toggle('rest-exhausted', uiMP.val <= 0 && !uiResting.val);
  });
}

function initPlayerList() {
  const { div: vdiv, span: vspan } = van.tags;
  van.hydrate(document.getElementById('player-list'), () => {
    const wrap = document.createElement('div');
    for (const { nm, sc, color, isMe, rank } of uiPlayers.val)
      van.add(wrap,
        vdiv({ class: 'player-row' },
          vspan({ class: `p-rank${rank === 1 ? ' gold' : ''}` }, `#${rank}`),
          vdiv({ class: 'p-dot', style: `--pc:${color}` }),
          vspan({ class: `p-name${isMe ? ' me' : ''}` }, nm),
          vspan({ class: 'p-sc' }, String(sc))
        )
      );
    return wrap;
  });
}

function initMenuSystem() {
  const { div: md, span: ms, button: mb, h2: mh2, h3: mh3, p: mp, input: minput, br: mbr } = van.tags;

  const menuOverlay = document.getElementById('menu-overlay');
  van.derive(() => { menuOverlay.classList.toggle('open', uiMenuPage.val !== null); });

  // Pre-populate settings inputs after VanJS commits new DOM
  van.derive(() => {
    if (uiMenuPage.val !== 'settings') return;
    setTimeout(() => {
      const inp = document.getElementById('menu-name-input');
      if (inp && myId >= 0) inp.value = players[myId].nm || '';
      const ssidInp = document.getElementById('wifi-ssid');
      const passInp = document.getElementById('wifi-pass');
      if (ssidInp) ssidInp.value = localStorage.getItem('wifi_ssid') || '';
      if (passInp) passInp.value = localStorage.getItem('wifi_pass') || '';
    }, 0);
  });

  const RES_GUIDE = [
    { name:'Water',    cls:'dot-water', desc:'Marshes, settlements, open scrub.' },
    { name:'Food',     cls:'dot-food',  desc:'Rust forests, marshes.'            },
    { name:'Fuel',     cls:'dot-fuel',  desc:'Ash dunes, ridges, urban ruins.'   },
    { name:'Medicine', cls:'dot-med',   desc:'Broken urban, glass fields.'       },
    { name:'Scrap',    cls:'dot-scrap', desc:'Almost anywhere except craters.'   },
  ];

  van.add(document.getElementById('menu-content'), () => {
    const page = uiMenuPage.val;
    if (!page) return ms();

    const wrap = (...children) => md({ class: 'menu-page' }, ...children);
    const back  = (to) => mb({ class: 'menu-back', onclick: () => openMenu(to) }, '◀ BACK');
    const sec   = (title, ...children) =>
      md({ class: 'menu-section' },
        mh3({ class: 'menu-section-title' }, title),
        ...children
      );

    if (page === 'main') return wrap(
      mh2({ class: 'menu-title' }, '\u2630 COMMAND'),
      mb({ class: 'menu-item-btn', onclick: () => openMenu('howto')    }, '\u2B21  HOW TO PLAY'),
      mb({ class: 'menu-item-btn', onclick: () => openMenu('settings') }, '\u25C9  SETTINGS'),
      mb({ class: 'menu-item-btn', onclick: () => { closeMenu(); openCharSheet(); } }, '\u25C8  DOSSIER'),
      mb({ class: 'menu-item-btn', onclick: () => openMenu('about')    }, '\u25A3  ABOUT'),
      mb({ class: 'menu-resume-btn', onclick: closeMenu }, '\u25B6 RESUME'),
    );

    if (page === 'howto') return wrap(
      back('main'),
      mh2({ class: 'menu-sub-title' }, 'HOW TO PLAY'),

      sec('The Mission',
        mp({ class: 'menu-text-body' },
          'Six survivors share a 25×19 toroidal wasteland. The map wraps — walk far enough ' +
          'in any direction and you come back around. Explore hexes to reveal terrain, collect ' +
          'resources, and keep each other alive. A game day lasts 5 minutes. Survive as many ' +
          'days as you can.'
        )
      ),

      sec('Movement',
        mp({ class: 'menu-text-body' },
          'Your Move Points (MP) equal your current Life Level, reduced by wounds and fatigue. ' +
          'Each hex costs MP equal to its movement cost (shown below). You cannot move at 0 MP.'
        ),
        md({ class: 'ctrl-ref' },
          md({},
            mp({ class: 'ctrl-label' }, 'KEYBOARD'),
            md({ class: 'key-grid' },
              ms({ class: 'key-badge' }, 'Q'), ms({ class: 'key-badge' }, 'W'), ms({ class: 'key-badge' }, 'E'),
              ms({ class: 'key-badge' }, 'A'), ms({ class: 'key-badge key-badge-hidden' }, ''),
              ms({ class: 'key-badge' }, 'D'),
              ms({ class: 'key-badge key-badge-hidden' }, ''),
              ms({ class: 'key-badge' }, 'S'),
              ms({ class: 'key-badge key-badge-hidden' }, ''),
            ),
            md({ class: 'key-dir-grid' },
              ms({}, 'NW'), ms({}, 'N'), ms({}, 'NE'),
              ms({}, 'SW'), ms({}, ''),  ms({}, 'SE'),
              ms({}, ''),   ms({}, 'S'), ms({}, ''),
            ),
            mp({ class: 'menu-text-hint' }, 'Arrow keys and numpad also work.')
          ),
          md({},
            mp({ class: 'ctrl-label' }, 'TOUCH'),
            mp({ class: 'menu-text-body' },
              'Swipe anywhere on the map canvas in the direction you want to move. ' +
              'Or use the direction pad below the map.'
            )
          )
        )
      ),

      sec('The Day Cycle',
        mp({ class: 'menu-text-body' },
          'At dawn each day your body demands resources. The server automatically consumes ' +
          '1 Food token and 2 Water tokens from your inventory. If you don\'t have them, ' +
          'your Food or Water track drops instead.'
        ),
        mp({ class: 'menu-text-body' },
          'Crossing downward thresholds costs Life Level: Food drops at 4 and 2; ' +
          'Water drops at 5, 3, and 1. Recovery works in reverse — cross back up and you ' +
          'gain the LL back.'
        ),
        mp({ class: 'menu-text-body' },
          'If every connected player hits REST before dawn, the day ends immediately ' +
          'and dawn triggers early.'
        )
      ),

      sec('Survival Tracks',
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            ms({ class: 'ht-track-lbl' }, 'LIFE LEVEL'),
            ms({ class: 'ht-track-val' }, '1 – 6'),
            ms({ class: 'ht-track-desc' }, 'Core health. Drops from starvation, thirst, wounds, radiation. Reaches 0 = downed. Restored by REST in good conditions or treating a Grievous Wound.')
          ),
          md({ class: 'ht-track-row' },
            ms({ class: 'ht-track-lbl' }, 'FOOD'),
            ms({ class: 'ht-track-val' }, '1 – 6'),
            ms({ class: 'ht-track-desc' }, 'Hunger level. Drops each dawn without food tokens. Thresholds at 4 and 2 cost LL. Use FORAGE to gather food tokens.')
          ),
          md({ class: 'ht-track-row' },
            ms({ class: 'ht-track-lbl' }, 'WATER'),
            ms({ class: 'ht-track-val' }, '1 – 6'),
            ms({ class: 'ht-track-desc' }, 'Hydration level. Requires 2 water tokens at dawn. Thresholds at 5, 3, and 1 cost LL. Use COLLECT WATER on water-source terrain.')
          ),
          md({ class: 'ht-track-row' },
            ms({ class: 'ht-track-lbl' }, 'RADIATION'),
            ms({ class: 'ht-track-val' }, '0 – 10'),
            ms({ class: 'ht-track-desc' }, 'R track. Gained entering rad-tagged terrain (failed Endure DN 6). R ≥ 4 = RAD-SICK. R ≥ 7 = Dusk Check at day\'s end (Endure DN 8, fail = −1 LL). Spending a full day off rad terrain reduces R by 1 at dawn. Use TREAT (Radiation) to remove 2 R.')
          ),
          md({ class: 'ht-track-row' },
            ms({ class: 'ht-track-lbl' }, 'FATIGUE'),
            ms({ class: 'ht-track-val' }, '0 – 8'),
            ms({ class: 'ht-track-desc' }, 'Reduces effective MP each point above 4. Cleared by REST (−2 normally, −3 if another survivor shares your hex).')
          ),
          md({ class: 'ht-track-row' },
            ms({ class: 'ht-track-lbl' }, 'RESOLVE'),
            ms({ class: 'ht-track-val' }, '0 – 5'),
            ms({ class: 'ht-track-desc' }, 'Starts at 3. Spend to push a failed skill check once per check. Gain +1 by resting in good shelter (SV 3+, or SV 2+ with a built shelter on the hex).')
          ),
        )
      ),

      sec('Actions',
        mp({ class: 'menu-text-body' },
          'You get one action per day. Open the ⚔ ACTION menu to choose. ' +
          'Actions cost MP and most require a skill check. Settlement terrain reduces all TREAT difficulty by 2.'
        ),
        md({ class: 'ht-act-list' },
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '⚗ FORAGE'),
            ms({ class: 'ht-act-cost' }, '2 MP'),
            ms({ class: 'ht-act-desc' }, 'Search for food on Forage-tagged terrain. Forage skill check — success yields food tokens.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '≈ COLLECT WATER'),
            ms({ class: 'ht-act-cost' }, '1–3 MP'),
            ms({ class: 'ht-act-desc' }, 'Gather water on Water-tagged terrain. Spend 1–3 MP; each MP spent collects 1 water token.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '⚲ SCAVENGE'),
            ms({ class: 'ht-act-cost' }, '2 MP'),
            ms({ class: 'ht-act-desc' }, 'Rifle through ruins on Salvage-tagged terrain. Scavenge check — partial success still finds scrap.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '⛺ BUILD SHELTER'),
            ms({ class: 'ht-act-cost' }, '1–2 MP'),
            ms({ class: 'ht-act-desc' }, 'Requires scrap — no skill roll. 1 scrap builds a shelter ⛺ (1 MP); 2+ scrap automatically builds an improved shelter 🏠 (2 MP). Improves rest quality for everyone who camps here.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '❤ TREAT'),
            ms({ class: 'ht-act-cost' }, '2 MP'),
            ms({ class: 'ht-act-desc' },
              'Field medicine. Pick a condition to treat (Treat skill check):',
              md({ class: 'ht-treat-list' },
                ms({}, 'Minor Wound DN 7 — remove 1 minor wound'),
                ms({}, 'Bleeding DN 7 — stop bleeding (fail = +1 fatigue)'),
                ms({}, 'Fever DN 9 — clear fever'),
                ms({}, 'Radiation DN 7 — remove 2 R'),
                ms({}, 'Grievous Wound DN 10 — Settlement only; remove wound + restore 1 LL'),
              )
            )
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '◎ SURVEY'),
            ms({ class: 'ht-act-cost' }, '1 MP'),
            ms({ class: 'ht-act-desc' }, 'Reveal the ring of hexes one step beyond your normal vision radius. One-time snapshot — surveyed hexes dim when you move away.')
          ),
        )
      ),

      sec('Rest',
        mp({ class: 'menu-text-body' },
          'The ▼ REST button is always available — it does not use your action slot. ' +
          'Resting reduces Fatigue by 2 (or 3 if another survivor shares your hex). ' +
          'If you are well-fed (Food ≥ 4), hydrated (Water ≥ 3), and not too tired (Fatigue < 4 after reduction), ' +
          'you also recover 1 Life Level. Resting in good shelter (SV 3+, or SV 2 + built shelter) gains +1 Resolve. ' +
          'Once you REST you wait for dawn — if all connected players have rested, dawn triggers immediately.'
        )
      ),

      sec('Vision & Fog',
        mp({ class: 'menu-text-body' },
          'Your base vision radius is ' + VISION_R + ' hex. Terrain modifies this:'
        ),
        md({ class: 'ht-vis-list' },
          md({ class: 'ht-vis-row' }, ms({ class: 'ht-vis-tag hi' }, '+2 VHIGH'), ms({}, 'Ridge, Mountain — sweeping view from elevation.')),
          md({ class: 'ht-vis-row' }, ms({ class: 'ht-vis-tag hi' }, '+1 HIGH'),  ms({}, 'Glass Fields — flat reflective surface, open sightlines.')),
          md({ class: 'ht-vis-row' }, ms({ class: 'ht-vis-tag lo' }, 'BLIND'),    ms({}, 'Rust Forest — canopy blocks all vision. Range drops to 0.')),
          md({ class: 'ht-vis-row' }, ms({ class: 'ht-vis-tag lo' }, 'MASKED'),   ms({}, 'Broken Urban — vision range 1 and resource contents are hidden until you stand on the hex.')),
        )
      ),

      sec('Terrain',
        md({ class: 'terrain-grid' },
          ...TERRAIN.map(t => {
            const vis = t.vis > 0 ? ms({ class: 'tr-vis-hi'  }, '\u25B2 HIGH')
                      : t.vis < 0 ? ms({ class: 'tr-vis-pen' }, '\u25BC MASK')
                      :             ms({ class: 'vis-std' }, 'STD');
            const mc  = t.mc === 255 ? ms({ class: 'mc-block' }, 'BLOCK')
                      :               ms({}, `MC:${t.mc}`);
            return md({ class: 'terrain-ref-card' },
              md({ class: 'tr-head' },
                ms({ class: 'tr-icon' }, t.icon),
                ms({ class: 'tr-name' }, t.name)
              ),
              md({ class: 'tr-stats' }, mc, ms({}, ' \u00B7 '), vis)
            );
          })
        )
      ),

      sec('Fatigue',
        mp({ class: 'menu-text-body' },
          'Fatigue builds when foraging fails or partially succeeds, and when a Bleed treatment fails. It caps at 8. ' +
          'There is no automatic recovery — only REST clears it.'
        ),
        mp({ class: 'menu-text-body' },
          'REST recovery depends on where you sleep: ' +
          'open ground −2 · shelter −3 · improved shelter −4. ' +
          'You can only restore a Life Level while resting if fatigue drops below 4 (or below 6 in an improved shelter). ' +
          'Exhausted survivors should build shelter before resting.'
        )
      ),

      sec('Wounds & Conditions',
        mp({ class: 'menu-text-body' },
          'Wounds reduce skill checks. Minor Wounds penalise Endure; Major Wounds penalise all skills. ' +
          'Grievous Wounds require a Settlement and a successful Treat to remove — and restore 1 LL when cleared.'
        ),
        mp({ class: 'menu-text-body' },
          'Bleeding adds fatigue if left untreated. Fever penalises Forage checks. ' +
          'Both clear with a successful TREAT action.'
        )
      ),

      sec('Skill Checks',
        mp({ class: 'menu-text-body' },
          'Roll 2d6 + skill value + modifiers vs. the Difficulty Number (DN). ' +
          'Meeting or exceeding DN = success. Spend 1 Resolve to re-roll once (push). ' +
          'Six skills: NAVIGATE · FORAGE · SCAVENGE · TREAT · SHELTER · ENDURE.'
        )
      ),

      sec('Scoring',
        mp({ class: 'menu-text-body' },
          'Points are earned when discovering resources in new hexes. Each hex yields points once the first time you or your team reveals its loot. ' +
          'Different terrain types and resource quality grant variable points — risk exploration to maximize your score.'
        )
      ),

      sec('AI Agents',
        mp({ class: 'menu-text-body' },
          'This game exposes machine-readable HTTP endpoints and accessible DOM elements to assist AI agents and automation tools.'
        ),

        mp({ class: 'menu-text-body' }, mb({}, 'GET /state'), ' — Full server game state. Returns JSON with:'),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Global'),
            mp({ class: 'ht-track-desc' }, 'day, dayTick, tc (Threat Clock), crisis, connected, sharedFood, sharedWater, evtQueue')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Map'),
            mp({ class: 'ht-track-desc' }, 'Shelter counts, resource totals per type, cell count per terrain (11 types)')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Players'),
            mp({ class: 'ht-track-desc' }, 'All 6 slots: name, archetype, q/r position, survival tracks (ll/food/water/fatigue/rad/resolve), statusBits, wounds[3], skills[6], inv[5] quick totals + full invType/invQty grids, turn state (mp/actUsed/resting/radClean), chkSk/chkDn/chkPushable, score/steps. conn:false = empty slot.')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'statusBits'),
            mp({ class: 'ht-track-desc' }, 'Bitmask: bit0=Wounded, bit1=RadSick, bit2=Bleeding, bit3=Fevered, bit4=Downed, bit5=Stable, bit6=Panicked')
          )
        ),

        mp({ class: 'menu-text-body' }, mb({}, 'GET /view?pid=N'), ' — Visible hex cells for player N. Returns:'),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Header'),
            mp({ class: 'ht-track-desc' }, 'pid, name, q, r, visR (vision radius)')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'cells[]'),
            mp({ class: 'ht-track-desc' }, 'Each hex: q, r, dq (col offset from player), dr (row offset), terrain index, terrainName, shelter (0/1/2), resource index, resourceName, amount, footprints bitmask')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Direction'),
            mp({ class: 'ht-track-desc' }, 'Use dq/dr to find neighbours without hex math. dq:0,dr:-1 = North. dq:+1,dr:0 = NE. dq:-1,dr:+1 = SW.')
          )
        ),

        mp({ class: 'menu-text-body' }, mb({}, 'Accessible DOM IDs'), ' — readable without screenshots:'),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, '#game-narrator'),
            mp({ class: 'ht-track-desc' }, 'aria-live region. textContent updates after every move ("Moved to Broken Urban at q:5,r:3. MP:3.") and every action ("Scavenge \u2014 Success. MP:1. +5pts.").')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, '#tc-name'),
            mp({ class: 'ht-track-desc' }, 'Current hex terrain name. Updated on every move.')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, '#hud-mp'),
            mp({ class: 'ht-track-desc' }, 'Movement points remaining. e.g. "3 MP"')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, '#inv0\u2013#inv4'),
            mp({ class: 'ht-track-desc' }, 'Inventory counts: water / food / fuel / medicine / scrap')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, '#action-btn-1 to -7'),
            mp({ class: 'ht-track-desc' }, '1=Forage 2=Water 3=Scavenge 4=Shelter 5=Treat 6=Survey 7=Rest. Has aria-label. .action-disabled class when blocked.')
          )
        ),

        mp({ class: 'menu-text-body' }, mb({}, 'WebSocket'), ' — ws://192.168.4.1/ws'),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Join'),
            mp({ class: 'ht-track-desc' }, '{"t":"join","name":"BOT","arch":0}  arch: 0=Guide 1=Quartermaster 2=Medic 3=Mule 4=Scout 5=Endurer')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Move'),
            mp({ class: 'ht-track-desc' }, '{"t":"mv","d":N}  dirs 0\u20135 = NW / N / NE / SW / SE / S')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Action'),
            mp({ class: 'ht-track-desc' }, '{"t":"act","a":N}  a: 1=Forage 2=Water 3=Scavenge 4=Shelter 5=Treat 6=Survey 7=Rest')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Tip'),
            mp({ class: 'ht-track-desc' }, 'Poll GET /view after each move for spatial context. Poll GET /state for full party status between turns.')
          )
        )
      ),
    );

    if (page === 'settings') return wrap(
      back('main'),
      mh2({ class: 'menu-sub-title' }, 'SETTINGS'),

      md({ class: 'settings-row' },
        mp({ class: 'settings-label' }, 'Call Sign'),
        md({ class: 'settings-name-row' },
          minput({
            id: 'menu-name-input', type: 'text', maxlength: '11',
            placeholder: 'Enter call sign...', class: 'settings-name-input',
            value: (myId >= 0 ? players[myId].nm : '') || '',
            onkeydown: e => { if (e.key === 'Enter') document.getElementById('menu-name-ok')?.click(); }
          }),
          mb({
            id: 'menu-name-ok', class: 'menu-item-btn',
            onclick: () => {
              const nm = document.getElementById('menu-name-input')?.value.trim().slice(0, 11);
              if (!nm) return;
              send({ t: 'n', name: nm });
              if (myId >= 0) { players[myId].nm = nm; updateSidebar(); }
              showToast(`Call sign: ${nm}`);
            }
          }, 'CONFIRM')
        )
      ),

      md({ class: 'settings-row' },
        mp({ class: 'settings-label' }, 'Vision Range'),
        mp({ class: 'settings-val' }, () => `${uiVision.val} hexes  (base ${VISION_R} + terrain modifier)`)
      ),

      md({ class: 'settings-row' },
        mp({ class: 'settings-label' }, 'Position'),
        mp({ class: 'settings-val' }, () => uiPos.val)
      ),

      md({ class: 'settings-row' },
        mp({ class: 'settings-label' }, 'Connection'),
        mp({ class: 'settings-val' }, () => uiConn.val)
      ),

      sec('WiFi Network',
        md({ class: 'settings-row' },
          mp({ class: 'settings-label' }, 'SSID'),
          minput({
            id: 'wifi-ssid', type: 'text', maxlength: '32',
            placeholder: 'Network name...', class: 'settings-name-input',
            onkeydown: e => { if (e.key === 'Enter') document.getElementById('wifi-pass')?.focus(); }
          })
        ),
        md({ class: 'settings-row' },
          mp({ class: 'settings-label' }, 'Password'),
          minput({
            id: 'wifi-pass', type: 'password', maxlength: '64',
            placeholder: 'Password...', class: 'settings-name-input',
            onkeydown: e => { if (e.key === 'Enter') document.getElementById('wifi-connect')?.click(); }
          })
        ),
        mb({
          id: 'wifi-connect', class: 'menu-item-btn',
          onclick: () => {
            const ssid = document.getElementById('wifi-ssid')?.value.trim();
            const pass = document.getElementById('wifi-pass')?.value ?? '';
            if (!ssid) { showToast('Enter a network name'); return; }
            localStorage.setItem('wifi_ssid', ssid);
            localStorage.setItem('wifi_pass', pass);
            send({ t: 'wifi', ssid, pass });
            showToast('Credentials saved \u2014 connecting...');
          }
        }, '\u25B6 CONNECT TO NETWORK')
      ),
    );

    if (page === 'about') return wrap(
      back('main'),
      md({ class: 'about-header' },
        mp({ class: 'about-logo' }, '\u2620 WASTELAND CRAWL'),
        mp({ class: 'about-sub'  }, 'Post-Apocalyptic Hex Crawl', mbr(), '6-player co-op scavenging on ESP32-S3'),
        mp({ class: 'about-sub'  },
          'Explore a 25\u00D719 toroidal wasteland.', mbr(),
          'Scavenge water, food, fuel, medicine and scrap.', mbr(),
          'Survive.'
        ),
        mp({ class: 'about-ver' }, '11 terrain types \u00B7 5 resource types \u00B7 6 survivors'),
        mp({ class: 'about-ver' }, 'WebSocket \u00B7 AP mode \u00B7 SSID: WASTELAND \u00B7 192.168.4.1'),
      )
    );

    return ms(); // unreachable fallback
  });
}

// ── Character Selection Screen ────────────────────────────────────
function initCharSelect() {
  const { div, span, button, h2, p } = van.tags;
  const container = document.getElementById('char-select-content');
  if (!container) return;

  van.add(container, () => {
    const avail   = lobbyAvail.val;
    const pending = uiPickPending.val;
    const availSet = new Set(avail);

    return div({ class: 'cs-page' },
      h2({ class: 'cs-sel-title' }, '\u2620 CHOOSE YOUR WAYFARER'),
      p({ class: 'cs-sel-sub' },
        'Each role has a unique trait. Select carefully \u2014 you cannot change once the wasteland claims you.'
      ),
      div({ class: 'arch-grid' },
        ...ARCHETYPES.map((arch, i) => {
          const taken = !availSet.has(i);
          const color = arch.color;

          const skillDots = SK_NAMES.map((sk, si) => {
            const lvl = arch.skills[si];
            return div({ class: 'arch-skill-row' },
              span({ class: 'arch-sk-name' }, sk),
              span({ class: `arch-sk-val sk-lvl-${lvl}` },
                '\u25CF'.repeat(lvl) + '\u25CB'.repeat(2 - lvl)
              )
            );
          });

          const selectBtn = taken
            ? span({ class: 'arch-taken-stamp' }, '\u2612 TAKEN')
            : button({
                class: 'arch-select-btn' + (pending ? ' arch-btn-pending' : ''),
                style: `--arch-color:${color}`,
                disabled: pending || undefined,
                onclick: () => {
                  if (pending || taken) return;
                  uiPickPending.val = true;
                  send({ t: 'pick', arch: i });
                }
              }, pending ? '\u2022\u2022\u2022' : `\u25B6 SELECT ${arch.name}`);

          return div({
            class: `arch-card${taken ? ' arch-taken' : ''}`,
            style: `--arch-color:${color}`
          },
            div({ class: 'arch-header' },
              span({ class: 'arch-icon' }, arch.icon),
              div({ class: 'arch-name-wrap' },
                span({ class: 'arch-name' }, arch.name),
                span({ class: 'arch-inv-label' }, `\u25A1 ${arch.invSlots} Inventory`)
              )
            ),
            div({ class: 'arch-skills' }, ...skillDots),
            div({ class: 'arch-trait' },
              span({ class: 'arch-trait-label' }, 'TRAIT \u2014 '),
              span({ class: 'arch-trait-text' }, arch.trait)
            ),
            div({ class: 'arch-flavor' }, arch.flavor),
            div({ class: 'arch-card-footer' }, selectBtn)
          );
        })
      )
    );
  });
}

// ── VanJS entry point ─────────────────────────────────────────────
function initVanJS() {
  initHudBindings();
  initCharSheetBindings();
  initMapBindings();
  initActionPanel();
  initPlayerList();
  initMenuSystem();
  initCharSelect();
}

// ── Boot ──────────────────────────────────────────────────────────
initVanJS();
connect();
requestAnimationFrame(render);
