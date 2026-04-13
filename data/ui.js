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
  document.getElementById('hi-title').textContent = t.name.toUpperCase();

  // Move cost
  document.getElementById('hi-mc').textContent =
    t.mc === 255 ? 'IMPASSABLE' : `${t.mc}× MP`;

  // Shelter — actual built level on this specific hex
  const shelterLabels = ['None', 'Basic ⛺', 'Improved 🏠', 'Fortified 🏰'];
  document.getElementById('hi-shelter').textContent =
    shelterLabels[cell.shelter] || 'None';

  // Vision modifier — t.vis range is -3..+2; shift by 3 for array index
  const visLabels = ['BLIND (vis 0)', 'MASKED (vis limited)', 'LOW (−1 hex)', 'STANDARD', 'HIGH (+1 hex)', 'HIGH (+2 hex)'];
  document.getElementById('hi-vis').textContent = visLabels[t.vis + 3] ?? 'STANDARD';

  // Hazard
  document.getElementById('hi-hazard').textContent = t.hazard || 'None';

  // Available actions for this terrain
  const actionList = document.getElementById('hi-actions');
  actionList.innerHTML = '';
  const actionDefs = [
    { id: ACT_FORAGE, label: 'FORAGE' },
    { id: ACT_WATER,  label: 'WATER'  },
    { id: ACT_SCAV,   label: 'SCAVENGE' },
  ];
  actionDefs.forEach(({ id, label }) => {
    if (actAvailable(id, cell.terrain)) {
      const b = document.createElement('span');
      b.className = 'hi-act-badge';
      b.textContent = label;
      actionList.appendChild(b);
    }
  });
  if (!actionList.hasChildNodes()) {
    actionList.innerHTML = '<span class="res-none-label">—</span>';
  }

  // Resource tokens on this hex
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

  // Ground items at this hex (renderHexGroundItems defined later)
  renderHexGroundItems?.(q, r);
}

// Keep --hud-h in sync for fixed elements that offset below the HUD.
(function () {
  const hud = document.getElementById('hud');
  const update = () =>
    document.documentElement.style.setProperty('--hud-h', hud.offsetHeight + 'px');
  update();
  new ResizeObserver(update).observe(hud);
})();

document.getElementById('terrain-card').addEventListener('click', () => {
  if (myId < 0) return;
  const me   = players[myId];
  const cell = gameMap[me.r] && gameMap[me.r][me.q];
  if (!cell) return;
  if (!uiHexInfoOpen.val) populateHexInfo(me.q, me.r, cell);
  uiHexInfoOpen.val = !uiHexInfoOpen.val;
});

document.getElementById('hex-close').addEventListener('click', e => {
  e.stopPropagation();
  uiHexInfoOpen.val = false;
});
document.getElementById('hex-close').addEventListener('keydown', e => {
  if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); uiHexInfoOpen.val = false; }
});

// ── Weather HUD indicator ─────────────────────────────────────────────────────
function updateWeatherHUD() {
  const el = document.getElementById('hud-weather');
  if (!el) return;
  const icons   = ['\u2600', '\uD83C\uDF27', '\u26A1', '\u2623'];
  const classes = ['', 'wx-rain', 'wx-storm', 'wx-chem'];
  const phase = (typeof weatherPhase !== 'undefined') ? weatherPhase : 0;
  el.textContent = `${icons[phase] ?? ''} ${WEATHER_PHASE_NAMES?.[phase] ?? ''}`;
  el.className = 'hud-weather ' + (classes[phase] ?? '');
}

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
  uiFat.val     = me.fat  ?? 0;
  uiResolve.val = me.res  ?? 3;
  uiMP.val      = me.mp   ?? 0;
  uiRad.val     = me.rad  ?? 0;
  uiHasCond.val = ((me.wd?.[0] ?? 0) > 0 || (me.sb & 0x8C) !== 0);
  uiPlayers.val = players
    .map((p, i) => ({ p, i }))
    .filter(({ p }) => p.on)
    .map(({ p, i }) => ({ i, nm: p.nm || `P${i}`, sc: p.sc, color: PLAYER_COLORS[i], isMe: i === myId }))
    .sort((a, b) => b.sc - a.sc)
    .map((entry, rank) => ({ ...entry, rank: rank + 1 }));
  const _dayEl = document.getElementById('hud-day');
  if (_dayEl) _dayEl.textContent = gameState.dc > 0 ? `DAY ${gameState.dc}` : '';
  updateClock();
  // REST button turns dark green when any connected player is resting
  const restBtn = document.getElementById('fab-rest-btn');
  if (restBtn) {
    const anyResting = players.some(p => p.on && p.rest);
    restBtn.style.background = anyResting ? '#1a4a1a' : '';
  }
  updateRestIndicator();
}

// ── Rest state indicator ──────────────────────────────────────────
function updateRestIndicator() {
  const el = document.getElementById('hud-rest');
  if (!el) return;
  const connectedPlayers = players.filter(p => p.on);
  const restingPlayers   = connectedPlayers.filter(p => p.rest);
  const n = restingPlayers.length;
  const m = connectedPlayers.length;
  const allResting = m > 0 && n === m;

  el.classList.remove('rest-wait', 'rest-on', 'rest-all');

  if (typeof restSent !== 'undefined' && restSent && !uiResting.val) {
    // Request sent, awaiting server ack
    el.classList.add('rest-wait');
    el.textContent = 'WAIT';
  } else if (allResting) {
    // All connected players resting — early dawn imminent
    el.classList.add('rest-all');
    el.textContent = `REST ${n}/${m}`;
  } else if (uiResting.val) {
    // This player confirmed resting
    el.classList.add('rest-on');
    el.textContent = n > 0 ? `REST ${n}/${m}` : 'REST';
  }
  // else: no rest active — element hidden via CSS (no class, display:none)
}

// ── Direction button blocked state ──────────────────────────────
function updateDirButtons() {
  if (myId < 0) return;
  const me       = players[myId];
  const inEnc    = !!me.enc;
  const resting  = me.rest || uiResting.val;
  const exhausted = (me.mp ?? 1) === 0;
  // While in encounter, resting, or exhausted, show all 6 buttons dimmed with a helpful tooltip (BUG-12)
  const vm = (inEnc || resting || exhausted) ? 0 : (me.vm ?? 0x3F);
  const blockMsg = inEnc ? 'Inside encounter \u2014 complete or bank first'
    : resting ? 'Resting \u2014 waiting for dawn'
    : exhausted ? 'No MP \u2014 REST to recover'
    : 'Blocked (impassable)';
  for (let d = 0; d < 6; d++) {
    const btn = document.getElementById(`dir-btn-${d}`);
    if (!btn) continue;
    const blocked = !(vm & (1 << d));
    btn.classList.toggle('blocked', blocked);
    btn.title = blocked ? blockMsg : '';
  }
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

  // Archetype banner + portrait
  const arch     = ARCHETYPES[me.arch] || ARCHETYPES[0];
  const archIcon = document.getElementById('cs-arch-icon');
  const archName = document.getElementById('cs-arch-name');
  if (archIcon) { archIcon.textContent = arch.icon; archIcon.style.color = arch.color; }
  if (archName) { archName.textContent = arch.name; archName.style.color = arch.color; }
  document.getElementById('char-sheet')
    .style.setProperty('--cs-portrait', `url('img/survivors/${arch.name.toLowerCase()}.jpg')`);
  const archTrait = document.getElementById('cs-arch-trait');
  if (archTrait) archTrait.textContent = arch.trait || '';

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
      label.textContent = name;
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
    if (me.sb & 0x80) conds.push('STINK');
    wdCond.textContent = conds.join(' ');
    wdCond.style.color = conds.length ? '#CC4422' : '';
  }

  uiCharOpen.val = true;
  // Render typed inventory and equipment slots (defined later in this file)
  renderInventory?.();
  renderEquipment?.();
}

document.getElementById('menu-btn').addEventListener('click', () => openMenu('main'));
document.getElementById('menu-overlay').addEventListener('click', e => {
  if (e.target === document.getElementById('menu-overlay')) closeMenu();
});

// Char-overlay visibility — driven by uiCharOpen state
van.derive(() => {
  const overlay = document.getElementById('char-overlay');
  overlay.classList.toggle('open', uiCharOpen.val);
  // Move focus out before hiding so aria-hidden never traps a focused descendant
  if (!uiCharOpen.val && overlay.contains(document.activeElement)) {
    document.getElementById('menu-btn')?.focus();
  }
  overlay.setAttribute('aria-hidden', uiCharOpen.val ? 'false' : 'true');
});
document.getElementById('char-close').addEventListener('click', () => { uiCharOpen.val = false; });
document.getElementById('char-close').addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); uiCharOpen.val = false; } });
document.getElementById('char-overlay').addEventListener('click', e => {
  if (e.target === document.getElementById('char-overlay')) uiCharOpen.val = false;
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
  if (!inner) return;  // guard: element missing during init or layout race
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
  const scrap = players[myId]?.inv?.[4] ?? 0;
  if (scrap === 0) {
    showBanner('EXPOSED TO THE ELEMENTS', 'find scrap \u2014 then build a shelter');
  } else {
    const [main, sub] = SHELTER_WARNINGS[Math.floor(Math.random() * SHELTER_WARNINGS.length)];
    showBanner(main, sub);
  }
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
  if (typeof invertedInputTurns !== 'undefined' && invertedInputTurns > 0) dir = (dir + 3) % 6;
  if (myId >= 0 && players[myId]?.ll === 0) {
    showToast('☠ You have been downed — select a new survivor');
    addLog('<span class="log-check-fail">☠ Cannot move — you have been downed.</span>');
    return;
  }
  if (myId >= 0 && players[myId]?.enc) {
    showToast('\u26D4 Inside encounter \u2014 complete or bank first');
    return;
  }
  if (myId >= 0 && uiMP.val <= 0) {
    showToast('\u26A0 Exhausted \u2014 wait for dawn');
    return;
  }
  const vm = myId >= 0 ? (players[myId].vm ?? 0x3F) : 0x3F;
  if (!(vm & (1 << dir))) {
    showToast('\u26D4 Impassable \u2014 direction blocked');
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

// Keyboard: Q=NW(3) W=N(2) E=NE(1)  S=S(5) D=SE(0)  [A freed for ACTION shortcut]
const keyMap = {
  'KeyQ':'3','KeyW':'2','KeyE':'1','KeyS':'5','KeyD':'0',
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
    uiCharOpen.val    = false;
    uiHexInfoOpen.val = false;
    return;
  }
  // FAB shortcuts: R=Rest, A=Action, C=Survivor
  if (e.code === 'KeyR') { e.preventDefault(); document.getElementById('fab-rest-btn')?.click(); return; }
  if (e.code === 'KeyA') { e.preventDefault(); document.getElementById('fab-action-btn')?.click(); return; }
  if (e.code === 'KeyC') { e.preventDefault(); document.getElementById('fab-char-btn')?.click(); return; }
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
      connDot.setAttribute('aria-label', conn === 'Connected' ? 'connected' : conn === 'Connecting...' ? 'connecting' : 'disconnected');
    });
  }

  // Hourglass on #cd-center when disconnected
  const cdCenter = document.getElementById('cd-center');
  if (cdCenter) {
    van.derive(() => {
      cdCenter.classList.toggle('cd-disconnected', uiConn.val !== 'Connected');
    });
  }

  // Movement points — rendered as track boxes in #hud-mp-track (see #hud-ll)

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
  let prevScore = -1;  // -1 = uninitialized; first update is a sync, not a real delta
  van.derive(() => {
    const sc    = uiScore.val;
    if (prevScore < 0) { prevScore = sc; return; }  // first sync — initialize silently, no animation
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

}

// Track box renderer — builds/updates N child divs inside containerId.
// thresholds: [{box, bit}]; firedMask: server bitmask (fth/wth).
//   armed (bit=0): orange arrow; spent (bit=1): dim arrow.
function renderTrackBoxes(containerId, value, thresholds, firedMask = 0, count = 6) {
  const el = document.getElementById(containerId);
  if (!el) return;
  // Trim excess boxes when count decreases (e.g. maxMP changes between days)
  while (el.children.length > count) el.removeChild(el.lastChild);
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
  el.setAttribute('aria-valuenow', value);
  el.setAttribute('aria-valuemax', count);
}

function initCharSheetBindings() {
  // Live stat elements
  const stepsEl   = document.getElementById('cs-steps');
  const visionEl  = document.getElementById('cs-vision');

  if (stepsEl)  { stepsEl.textContent  = ''; van.add(stepsEl,  () => String(uiSteps.val));  }
  if (visionEl) { visionEl.textContent = ''; van.add(visionEl, () => String(uiVision.val)); }

  // LL track (survivor)
  van.derive(() => {
    renderTrackBoxes('cs-ll-track', uiLL.val, [], 0, 7);
  });

  // LL mini-track (HUD)
  van.derive(() => {
    renderTrackBoxes('hud-ll-track', uiLL.val, [], 0, 7);
    const v = document.getElementById('hud-ll-val');
    if (v) v.textContent = String(uiLL.val);
  });

  // MP mini-track (HUD) — boxes match player's actual maxMP for the day
  van.derive(() => {
    renderTrackBoxes('hud-mp-track', uiMP.val, [], 0, uiMaxMP.val || 6);
  });

  // Food track (thresholds at boxes 4, 2)
  van.derive(() => {
    const fth = (myId >= 0 ? players[myId] : null)?.fth ?? 0;
    renderTrackBoxes('cs-food-track', uiFood.val,
      [{ box: 4, bit: 1 }, { box: 2, bit: 2 }], fth);
  });

  // Water track (thresholds at boxes 5, 3, 1)
  van.derive(() => {
    const wth = (myId >= 0 ? players[myId] : null)?.wth ?? 0;
    renderTrackBoxes('cs-water-track', uiWater.val,
      [{ box: 5, bit: 1 }, { box: 3, bit: 2 }, { box: 1, bit: 4 }], wth);

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
    const rStat = document.getElementById('cs-rad-status');
    if (rStat) {
      rStat.textContent = rad >= 7 ? '\u00a0\u2622 DUSK CHECK'
                        : rad >= 4 ? '\u00a0RAD-SICK'
                        :            '';
      rStat.className = 'track-suffix' +
        (rad >= 7 ? ' rad-status-critical' : rad >= 4 ? ' rad-status-sick' : '');
    }
  });

  // Fatigue track (0–8, no thresholds)
  van.derive(() => {
    renderTrackBoxes('cs-fat-track', uiFat.val, [], 0, 8);
  });

}

function initMapBindings() {
  // Hex-info overlay visibility — driven by uiHexInfoOpen state
  const hexInfo = document.getElementById('hex-info');
  const terrainCard = document.getElementById('terrain-card');
  van.derive(() => {
    hexInfo.classList.toggle('open', uiHexInfoOpen.val);
    terrainCard.classList.toggle('expanded', uiHexInfoOpen.val);
    terrainCard.setAttribute('aria-expanded', uiHexInfoOpen.val ? 'true' : 'false');
  });
  terrainCard.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); terrainCard.click(); }
  });

  // Terrain card — reactive repaint from uiCurrentCell
  van.derive(() => {
    const cc = uiCurrentCell.val;
    if (!cc) return;
    const t     = TERRAIN[cc.terrain] || TERRAIN[0];
    const mcStr = t.mc === 255 ? 'IMPASSABLE' : `MC:${t.mc}  SV:${t.sv}`;
    document.getElementById('tc-name').textContent = t.name;
    terrainCard.setAttribute('aria-label', `${t.name} — click for hex details`);
    document.getElementById('tc-sub').textContent  =
      (cc.resource > 0 && cc.amount > 0)
        ? `${RES_NAMES[cc.resource]} ×${cc.amount}`
        : mcStr;
    const badge = document.getElementById('tc-vis-badge');
    badge.className   = t.vis > 0 ? 'vis-high' : t.vis < 0 ? 'vis-penalty' : '';
    badge.textContent = t.vis > 0 ? '◉ HIGH VIS +2' : t.vis < 0 ? '◎ VIS PENALTY' : '';
    if (uiHexInfoOpen.val) populateHexInfo(cc.q, cc.r, cc);
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
  const actionPanel     = document.getElementById('action-panel');
  const actionBtnList   = document.getElementById('action-btn-list');
  const actionStatusBar = document.getElementById('action-status-bar');
  let terrName = '';  // hoisted so closeActionPanel can read the last-opened terrain name

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

  // ── Trade sub-control ──────────────────────────────────────────
  let tradeTargetPid = -1;
  const tradeGive = [0, 0, 0, 0, 0];
  const tradeWant = [0, 0, 0, 0, 0];

  function buildStepperRow(rowId, arr, maxFn) {
    const el = document.getElementById(rowId);
    el.innerHTML = '';
    RES_SHORT.forEach((label, i) => {
      const wrap = document.createElement('div');
      wrap.className = 'trade-stepper';
      const lbl = document.createElement('span');
      lbl.className   = 'trade-stepper-label';
      lbl.textContent = label;
      const minus = document.createElement('button');
      minus.textContent = '\u2212';
      const valSpan = document.createElement('span');
      valSpan.className   = 'trade-stepper-val';
      valSpan.textContent = arr[i];
      const plus = document.createElement('button');
      plus.textContent = '+';
      minus.addEventListener('click', (e) => {
        e.preventDefault();
        if (arr[i] > 0) { arr[i]--; valSpan.textContent = arr[i]; }
      });
      plus.addEventListener('click', (e) => {
        e.preventDefault();
        const max = maxFn(i);
        if (arr[i] < max) { arr[i]++; valSpan.textContent = arr[i]; }
      });
      wrap.append(lbl, minus, valSpan, plus);
      el.appendChild(wrap);
    });
  }

  function buildTradeOfferForm() {
    // Reset arrays
    tradeGive.fill(0);
    tradeWant.fill(0);
    buildStepperRow('trade-give-row', tradeGive, i => (myId >= 0 ? (players[myId]?.inv?.[i] ?? 0) : 0));
    buildStepperRow('trade-want-row', tradeWant, () => 30);
    document.getElementById('action-trade-offer-form').style.display = '';
  }

  function buildTradeTargetList() {
    const el = document.getElementById('action-trade-target-list');
    el.innerHTML = '';
    document.getElementById('action-trade-offer-form').style.display = 'none';
    tradeTargetPid = -1;
    if (myId < 0) return;
    const me = players[myId];
    const colocated = players.filter(p => p.id !== myId && p.on && p.q === me.q && p.r === me.r);
    if (colocated.length === 0) {
      el.innerHTML = '<div class="action-no-cond">No survivors on this hex</div>';
      return;
    }
    colocated.forEach(p => {
      const btn = document.createElement('button');
      btn.className   = 'chk-action-btn';
      btn.textContent = '\u21C4 Trade with ' + escHtml(p.nm || 'P' + p.id);
      btn.addEventListener('click', () => {
        tradeTargetPid = p.id;
        el.style.display = 'none';
        buildTradeOfferForm();
      });
      el.appendChild(btn);
    });
  }

  document.getElementById('action-trade-send').addEventListener('click', () => {
    if (myId < 0 || tradeTargetPid < 0) return;
    const allZero = tradeGive.every(v => v === 0) && tradeWant.every(v => v === 0);
    if (allZero) { showToast('\u2297 Offer must include at least one resource'); return; }
    send({ t: 'trade_offer', to: tradeTargetPid, give: [...tradeGive], want: [...tradeWant] });
    closeActionPanel();
    showToast('\u21C4 Trade offer sent');
  });

  function closeActionPanel() {
    document.getElementById('fab-action-btn')?.focus();
    actionPanel.setAttribute('aria-hidden', 'true');
    actionPanel.classList.remove('open');
    actionBtnList.style.display = '';
    const _terrSub = document.getElementById('act-panel-terrain-sub');
    if (_terrSub) _terrSub.textContent = terrName ? 'IN THE ' + terrName.toUpperCase() : '';
    document.getElementById('action-water-ctrl').style.display = 'none';
    document.getElementById('action-trade-ctrl').style.display = 'none';
  }

  function openActionPanel() {
    if (myId < 0) return;
    if (players[myId]?.ll === 0) {
      showToast('☠ You have been downed — select a new survivor');
      addLog('<span class="log-check-fail">☠ Cannot act — you have been downed.</span>');
      return;
    }
    if (players[myId]?.enc) {
      showToast('\u26D4 Inside encounter \u2014 complete or bank first');
      return;
    }
    const me   = players[myId];
    const cell = gameMap[me.r]?.[me.q];
    const terr        = cell?.terrain ?? null;
    const mp          = me.mp  ?? 0;
    const scrap       = me.inv?.[4] ?? 0;
    const shelterLevel = cell?.shelter ?? 0;
    const isScout     = (me.arch ?? -1) === 4;  // Scout: Survey free + no action slot
    const actUsed     = !!(me.au ?? 0);

    // Always update terrain header immediately so it never shows a stale hex name (BUG-04)
    terrName = (terr != null && terr <= 10) ? (TERRAIN[terr]?.name ?? 'Unknown') : 'Unknown';
    const _terrSub = document.getElementById('act-panel-terrain-sub');
    if (_terrSub) _terrSub.textContent = 'IN THE ' + terrName.toUpperCase();

    // Fix: suppress action menu entirely at MP:0 — only REST makes sense
    // Exception: TRADE is free (0 MP) and must remain available if a co-located player exists
    if (mp === 0) {
      const tradeAvailExhausted = players.some(p => p.id !== myId && p.on && p.q === me.q && p.r === me.r);
      let exhaustedHTML = '<div class="act-exhausted-msg">\u26A1 EXHAUSTED \u2014 use \u25BC REST to recover MP</div>';
      if (tradeAvailExhausted) {
        exhaustedHTML +=
          `<button id="action-btn-6" class="action-item-btn" role="listitem" aria-label="TRADE — Exchange resources with a co-located survivor">` +
          `<span class="act-icon">\u21C4</span>` +
          `<span class="act-body"><span class="act-label">TRADE</span>` +
          `<span class="act-desc">Exchange resources with a co-located survivor \u2014 free</span></span></button>`;
      }
      actionBtnList.innerHTML = exhaustedHTML;
      actionStatusBar.innerHTML =
        `<span class="act-mp-badge act-mp-zero">MP: 0</span>` +
        '<span class="act-used-badge">\u2297 NO MOVEMENT POINTS</span>';
      actionPanel.classList.add('open');
      actionPanel.setAttribute('aria-hidden', 'false');
      if (tradeAvailExhausted) {
        const tb = document.getElementById('action-btn-6');
        if (tb) tb.addEventListener('click', () => {
          buildTradeTargetList();
          document.getElementById('action-trade-ctrl').style.display = '';
          actionBtnList.style.display = 'none';
        });
      }
      return;
    }

    // Fix: suppress action menu when resting — show resting banner instead
    if (uiResting.val) {
      actionBtnList.innerHTML =
        '<div class="act-exhausted-msg">\ud83d\ude34 RESTING \u2014 waiting for dawn</div>';
      actionStatusBar.innerHTML =
        `<span class="act-mp-badge">MP: ${mp}</span>` +
        '<span class="act-used-badge">\u2297 RESTING</span>';
      actionPanel.classList.add('open');
      actionPanel.setAttribute('aria-hidden', 'false');
      return;
    }

    // terrName and _terrSub already updated above before early-returns (BUG-04 fix)
    const forageHere = terr != null && TERRAIN_FORAGE_DN[terr] > 0;
    const scavHere   = terr != null && TERRAIN_SALVAGE_DN[terr] > 0;
    const waterHere  = terr != null && TERRAIN_HAS_WATER[terr] > 0;
    const terrTags   = [forageHere && 'Forage', scavHere && 'Salvage', waterHere && 'Water'].filter(Boolean).join(' · ');
    actionStatusBar.innerHTML =
      `<span class="act-mp-badge">MP: ${mp}</span>` +
      `<span class="act-terrain-ctx">${terrName}${terrTags ? ' · ' + terrTags : ''}</span>`;

    document.getElementById('action-water-ctrl').style.display = 'none';

    // Fix: compute actual shelter MP cost dynamically — fall back to basic (1 MP) if not enough MP for improved
    const shelterMpCost = (scrap >= 2 && mp >= 2) ? 2 : 1;
    const shelterLabel  = shelterLevel >= 1 ? 'UPGRADE SHELTER' : 'BUILD SHELTER';

    actionBtnList.innerHTML = '';
    const actionDefs = [
      { id: ACT_FORAGE,  icon: '\u2698', label: 'FORAGE',        mpCost: 2,             desc: 'Search for food (Skill check)' },
      { id: ACT_WATER,   icon: '\u2248', label: 'COLLECT WATER', mpCost: 1,             desc: 'Gather water tokens (1-3 MP)' },
      { id: ACT_SCAV,    icon: '\u26B2', label: 'SCAVENGE',      mpCost: 2,             desc: 'Search for items (Skill check)' },
      { id: ACT_SHELTER, icon: '\u26FA', label: shelterLabel,    mpCost: shelterMpCost, desc: 'Construct shelter — needs scrap (1–2 MP, no roll)' },
      { id: ACT_TRADE,   icon: '\u21C4', label: 'TRADE',         mpCost: 0,             desc: 'Exchange resources with a co-located survivor — free' },
    ];
    // Scout-exclusive: SURVEY is hidden for non-Scouts
    if (isScout) {
      actionDefs.push({ id: ACT_SURVEY, icon: '\u25CE', label: 'SURVEY', mpCost: 0, desc: 'Reveal terrain beyond vision — free for Scout' });
    }

    // TRADE availability: requires another connected player on the same hex
    const tradeAvail  = players.some(p => p.id !== myId && p.on && p.q === me.q && p.r === me.r);

    actionDefs.forEach(def => {
      // Fix: shelter unavailable if improved shelter already built here
      const shelterMaxed = def.id === ACT_SHELTER && shelterLevel >= 2;
      // If cell hasn't loaded yet (null — race between 'asgn' and 'sync' messages),
      // allow terrain-dependent actions optimistically; the server validates.
      const available   = def.id === ACT_TRADE  ? tradeAvail
                        : (terr === null ? true : (!shelterMaxed && actAvailable(def.id, terr)));
      const hasMP      = mp >= def.mpCost;
      const needsScrap = def.id === ACT_SHELTER;
      const hasScrap   = !needsScrap || scrap > 0;
      // Actions that bypass the action slot (deterministic — no skill roll):
      const slotless   = def.id === ACT_SHELTER || def.id === ACT_WATER || def.id === ACT_TRADE
                       || (def.id === ACT_SURVEY && isScout);
      const slotFree   = slotless || !actUsed;
      const canAct     = available && hasMP && hasScrap && slotFree;

      // Dynamic desc: BUILD/UPGRADE SHELTER shows actual cost
      let desc = def.desc;
      if (def.id === ACT_SHELTER) {
        desc = shelterMaxed                          ? 'Improved shelter already here — nothing to build'
             : shelterLevel === 1 && scrap === 0     ? 'Shelter here — needs scrap to upgrade'
             : shelterLevel === 1 && scrap >= 2 && mp >= 2 ? '2 scrap → improved shelter \uD83C\uDFE0 (2 MP, +8 pts)'
             : shelterLevel === 1                    ? '1 scrap → upgrade to improved \uD83C\uDFE0 (1 MP, +4 pts)'
             : scrap === 0                           ? 'Needs scrap — none in pack'
             : scrap === 1                           ? '1 scrap → shelter \u26FA (1 MP, +4 pts)'
             : mp < 2                                ? '1 scrap → shelter \u26FA (1 MP, +4 pts) — not enough MP for improved'
             :                                        '2 scrap → improved shelter \uD83C\uDFE0 (2 MP, +8 pts)';
      }

      // Compute the inline block reason shown under the button label
      const blockReason = shelterMaxed                      ? 'Max shelter built here'
                        : def.id === ACT_SHELTER && shelterLevel === 1 && !hasScrap
                                                            ? 'Shelter here — need scrap to upgrade'
                        : !available   ? (def.id === ACT_FORAGE ? 'Needs Forage terrain (Rust Forest · Marsh · Open Scrub)'
                                        : def.id === ACT_WATER  ? 'Needs Water terrain (Marsh \u00b7 Flooded District)'
                                        : def.id === ACT_SCAV   ? 'Needs Salvage terrain (Broken Urban · Glass Fields)'
                                        : def.id === ACT_TRADE  ? 'No survivors on this hex'
                                        : 'Not available here')
                        : !hasMP       ? `Needs ${def.mpCost} MP (have ${mp})`
                        : !hasScrap    ? `Needs scrap (have ${scrap})`
                        : !slotFree    ? 'Action used this cycle \u2014 REST to reset'
                        : '';

      const btn = document.createElement('button');
      btn.id        = 'action-btn-' + def.id;   // stable ID for AI agents
      btn.setAttribute('role', 'listitem');
      btn.className = 'action-item-btn' + (canAct ? '' : ' action-disabled');
      btn.setAttribute('aria-label', def.label + (blockReason ? ' — ' + blockReason : ''));
      btn.innerHTML =
        `<span class="act-icon">${def.icon}</span>` +
        `<span class="act-body">` +
          `<span class="act-label">${def.label}</span>` +
          `<span class="act-desc">${desc}</span>` +
          (blockReason ? `<span class="act-unavail-reason">${blockReason}</span>` : '') +
        `</span>` +
        `<span class="act-cost">${def.mpCost > 0 ? def.mpCost + ' MP' : 'Free'}</span>`;

      btn.addEventListener('click', () => {
        if (!canAct) {
          showToast(!available ? '\u2297 Not available here' : !hasMP ? '\u2297 Insufficient MP' : '\u2297 Need scrap to build');
          return;
        }
        if (def.id === ACT_WATER) {
          waterMpVal = Math.min(3, mp);
          document.getElementById('action-water-v').textContent = waterMpVal;
          document.getElementById('action-water-ctrl').style.display = '';
          actionBtnList.style.display = 'none';
          return;
        }
        if (def.id === ACT_TRADE) {
          buildTradeTargetList();
          document.getElementById('action-trade-ctrl').style.display = '';
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
  document.getElementById('fab-action-btn').addEventListener('click', () => {
    actionPanel.classList.contains('open') ? closeActionPanel() : openActionPanel();
  });
  document.getElementById('action-close').addEventListener('click', closeActionPanel);
  document.getElementById('action-close').addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); closeActionPanel(); } });

  document.getElementById('fab-rest-btn').addEventListener('click', () => {
    if (myId >= 0 && players[myId]?.ll === 0) {
      showToast('☠ You have been downed — select a new survivor');
      return;
    }
    if (myId >= 0 && players[myId]?.enc) {
      showToast('\u26D4 Inside encounter \u2014 complete or bank first');
      return;
    }
    if (uiResting.val || restSent) { showToast('\u2297 Already resting'); return; }
    restSent = true;
    updateRestIndicator();
    send({ t: 'act', a: ACT_REST });
  });

  van.derive(() => {
    const btn = document.getElementById('fab-action-btn');
    if (!btn) return;
    btn.classList.toggle('has-condition', uiHasCond.val);
  });
  van.derive(() => {
    const btn = document.getElementById('fab-rest-btn');
    if (!btn) return;
    btn.classList.toggle('rest-btn-used', uiResting.val);
    // Pulse when exhausted (out of MP) and not yet resting — nudge player to rest
    btn.classList.toggle('rest-exhausted', uiMP.val <= 0 && !uiResting.val);
  });
  document.getElementById('fab-char-btn').addEventListener('click', openCharSheet);
  // Expose for engine.js (dawn event re-renders the panel if it's open)
  window.openActionPanel = openActionPanel;
}

function initTradeOverlay() {
  const overlay = document.getElementById('trade-overlay');
  let pendingTradeFrom = -1;
  let expireInterval   = null;
  let expireEnd        = 0;

  window._openTradeOffer = function(fromPid, give, want) {
    pendingTradeFrom = fromPid;
    expireEnd = Date.now() + 30000;
    const fromName = players[fromPid]?.nm || 'P' + fromPid;
    document.getElementById('trade-from-label').textContent = 'From: ' + escHtml(fromName);
    document.getElementById('trade-give-display').textContent =
      give.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join('  ') || '\u2014';
    document.getElementById('trade-want-display').textContent =
      want.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join('  ') || '\u2014';
    document.getElementById('trade-expire-fill').style.width = '100%';
    overlay.classList.add('open');
    overlay.style.display = '';
    clearInterval(expireInterval);
    expireInterval = setInterval(() => {
      const pct = Math.max(0, (expireEnd - Date.now()) / 30000 * 100);
      document.getElementById('trade-expire-fill').style.width = pct + '%';
      if (pct <= 0) { clearInterval(expireInterval); _closeTradeOverlay(); }
    }, 500);
  };

  function _closeTradeOverlay() {
    pendingTradeFrom = -1;
    clearInterval(expireInterval);
    overlay.classList.remove('open');
    overlay.style.display = 'none';
  }
  window._closeTradeOverlay = _closeTradeOverlay;

  document.getElementById('trade-accept-btn').addEventListener('click', () => {
    if (pendingTradeFrom < 0) return;
    send({ t: 'trade_accept', from: pendingTradeFrom });
    _closeTradeOverlay();
  });
  document.getElementById('trade-decline-btn').addEventListener('click', () => {
    if (pendingTradeFrom < 0) return;
    send({ t: 'trade_decline', from: pendingTradeFrom });
    _closeTradeOverlay();
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
          vspan({ class: 'p-sc', title: 'Score' }, `\u2605${sc}`)
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
      const volSlider = document.getElementById('k10-vol-slider');
      const ledSlider = document.getElementById('k10-led-slider');
      if (volSlider) {
        const v = parseInt(localStorage.getItem('k10_audioVol') ?? '5');
        volSlider.value = v;
        const lbl = document.getElementById('k10-vol-val');
        if (lbl) lbl.textContent = v === 0 ? '0 (mute)' : String(v);
      }
      if (ledSlider) {
        const b = parseInt(localStorage.getItem('k10_ledBright') ?? '5');
        ledSlider.value = b;
        const lbl = document.getElementById('k10-led-val');
        if (lbl) lbl.textContent = b === 0 ? '0 (off)' : String(b);
      }
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
      mb({ class: 'menu-item-btn', onclick: () => { closeMenu(); openCharSheet(); } }, '\u25C8  SURVIVOR'),
      mb({ class: 'menu-item-btn', onclick: () => openMenu('about')    }, '\u25A3  ABOUT'),
      mb({ class: 'menu-item-btn', onclick: () => { closeMenu(); const ov = document.getElementById('help-overlay'); ov.classList.add('open'); ov.removeAttribute('aria-hidden'); } }, '\u2139  AGENT HELP'),
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
          'Your Move Points (MP) = max(2, Life Level − major wounds − encumbrance). ' +
          'Even at worst condition, you retain minimum 2 MP to stay mobile. ' +
          'Each hex costs MP equal to its terrain cost (1–2 MP). ' +
          'First visit to any hex grants +1 exploration point automatically.'
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
            ms({ class: 'ht-track-desc' }, 'Starts at 3. Gain +1 by resting in good shelter (SV 3+, or SV 2+ with a built shelter on the hex).')
          ),
        )
      ),

      sec('Actions',
        mp({ class: 'menu-text-body' },
          'Open the ☞ ACTION menu to choose an action. ' +
          'Actions cost MP and most require a skill check. Every action awards points (see Scoring). Settlement terrain reduces all TREAT difficulty by 2.'
        ),
        md({ class: 'ht-act-list' },
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '⚗ FORAGE'),
            ms({ class: 'ht-act-cost' }, '2 MP'),
            ms({ class: 'ht-act-desc' }, 'Search for food on Forage terrain. Forage check — success +1–2 food (+3 pts), partial +1 food (+1 pt).')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '≈ COLLECT WATER'),
            ms({ class: 'ht-act-cost' }, '1–3 MP'),
            ms({ class: 'ht-act-desc' }, 'Gather water on Water terrain. Spend 1–3 MP; each token collected = +1 pt.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '⚲ SCAVENGE'),
            ms({ class: 'ht-act-cost' }, '2 MP'),
            ms({ class: 'ht-act-desc' }, 'Rifle through Salvage terrain. Scavenge check — success +5 pts, partial +2 pts.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '⛺ BUILD SHELTER'),
            ms({ class: 'ht-act-cost' }, '1–2 MP'),
            ms({ class: 'ht-act-desc' }, 'Requires scrap. 1 scrap = shelter ⛺ (1 MP, +4 pts); 2+ scrap = improved shelter 🏠 (2 MP, +8 pts). Boosts rest quality for all campers.')
          ),
          md({ class: 'ht-act-row' },
            ms({ class: 'ht-act-name' }, '◎ SURVEY'),
            ms({ class: 'ht-act-cost' }, '1 MP'),
            ms({ class: 'ht-act-desc' }, 'Reveal the ring of hexes one ring beyond your vision. Scout: free. Each hex can only be surveyed once per player (+2 pts first time, +0 repeats). Surveyed hexes dim when you move.')
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
          'Meeting or exceeding DN = success. ' +
          'Six skills: NAVIGATE · FORAGE · SCAVENGE · TREAT · SHELTER · ENDURE.'
        )
      ),

      sec('Scoring',
        mp({ class: 'menu-text-body' },
          'Every action awards points. Score is earned through:'
        ),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            ms({}, 'Exploration'), ms({}, '+1 pt per new hex')
          ),
          md({ class: 'ht-track-row' },
            ms({}, 'FORAGE'), ms({}, '+3 pts success, +1 partial')
          ),
          md({ class: 'ht-track-row' },
            ms({}, 'WATER'), ms({}, '+1 pt per token')
          ),
          md({ class: 'ht-track-row' },
            ms({}, 'SCAVENGE'), ms({}, '+5 pts success, +2 partial')
          ),
          md({ class: 'ht-track-row' },
            ms({}, 'SHELTER'), ms({}, '+4 pts basic, +8 pts improved')
          ),
          md({ class: 'ht-track-row' },
            ms({}, 'TREAT'), ms({}, '+3 pts success')
          ),
          md({ class: 'ht-track-row' },
            ms({}, 'SURVEY'), ms({}, '+2 pts first per hex, +0 repeats')
          ),
        )
      ),

      sec('AI Agents',
        mp({ class: 'menu-text-body' },
          'This game exposes machine-readable HTTP endpoints, a JS state object, and accessible DOM elements to assist AI agents and automation tools.'
        ),
        mp({ class: 'menu-text-body' }, mb({}, '\u26A0 Session tip'), ' — Navigating away from the game page disconnects your WebSocket session. Open /state (or /state?pid=N for view data) in a separate browser window or tab to avoid losing your connection.'),

        mp({ class: 'menu-text-body' }, mb({}, 'GET /state'), ' — Full server game state. Returns JSON with:'),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Global'),
            mp({ class: 'ht-track-desc' }, 'day, dayTick, tc (Threat Clock), crisis, connected, evtQueue')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Map'),
            mp({ class: 'ht-track-desc' }, 'Shelter counts, resource totals per type, cell count per terrain (11 types)')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Players'),
            mp({ class: 'ht-track-desc' }, 'All 6 slots: name, archetype, q/r position, survival tracks (ll/food/water/fatigue/rad/resolve), statusBits, wounds[3], skills[6], inv[5] quick totals + full invType/invQty grids, turn state (mp/actUsed/resting/radClean), chkSk/chkDn/chkBonus, score/steps. conn:false = empty slot.')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'statusBits'),
            mp({ class: 'ht-track-desc' }, 'Bitmask: bit0=Wounded, bit1=RadSick, bit2=Bleeding, bit3=Fevered, bit4=Downed, bit5=Stable, bit6=Panicked')
          )
        ),

        mp({ class: 'menu-text-body' }, mb({}, 'GET /state?pid=N'), ' — Same as /state but also includes a ', mb({}, 'view'), ' object with visible hex cells for player N. Returns:'),
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

        mp({ class: 'menu-text-body' }, mb({}, 'window.__gameState'), ' — in-page JS object, updated after every WS message. Read without navigating away:'),
        md({ class: 'ht-track-list' },
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'How to read'),
            mp({ class: 'ht-track-desc' }, 'javascript_tool: return JSON.stringify(window.__gameState)')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Fields'),
            mp({ class: 'ht-track-desc' }, 'myId, day, tc, visR, me (position/stats/inv/mp), players[] (other online survivors), visibleCells[] (terrain/resource/shelter per known hex)')
          ),
          md({ class: 'ht-track-row' },
            md({ class: 'ht-track-label' }, 'Advantage'),
            mp({ class: 'ht-track-desc' }, 'No navigation required — WS session stays alive. Best choice for in-browser agents.')
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
            mp({ class: 'ht-track-desc' }, 'Prefer window.__gameState for in-browser agents — no navigation needed. To use REST endpoints, open them in a new window: e.g. http://192.168.4.1/state?pid=0 for player zero, /state?pid=1 for player one, etc.')
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
        mp({ class: 'settings-label' }, 'Event Log'),
        mb({
          class: 'menu-item-btn',
          style: () => `padding:3px 10px;font-size:var(--fs-d);border-color:${uiLogVisible.val ? 'var(--gold)' : 'var(--bdr-mid)'}`,
          onclick: () => {
            uiLogVisible.val = !uiLogVisible.val;
            localStorage.setItem('logVisible', uiLogVisible.val ? '1' : '0');
          }
        }, () => uiLogVisible.val ? 'VISIBLE  ▣' : 'HIDDEN  ▢')
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
        mp({ class: 'settings-label' }, 'Server IP'),
        mp({ class: 'settings-val' }, () => uiConn.val === 'Connected' ? (window.location.hostname || 'unknown') : 'unknown')
      ),

      md({ class: 'settings-row' },
        mp({ class: 'settings-label' }, 'Connection'),
        mp({ class: 'settings-val' }, () => uiConn.val)
      ),

      sec('K10 Hardware',
        md({ class: 'settings-row' },
          mp({ class: 'settings-label' }, 'K10 Volume'),
          md({ class: 'settings-slider-row' },
            minput({
              id: 'k10-vol-slider', type: 'range', min: '0', max: '9', step: '1',
              value: localStorage.getItem('k10_audioVol') ?? '5',
              class: 'settings-slider',
              oninput: e => {
                const v = parseInt(e.target.value);
                const lbl = document.getElementById('k10-vol-val');
                if (lbl) lbl.textContent = v === 0 ? '0 (mute)' : String(v);
                localStorage.setItem('k10_audioVol', v);
                send({ t: 'settings', audioVol: v, ledBright: parseInt(document.getElementById('k10-led-slider')?.value ?? '5') });
              }
            }),
            ms({ id: 'k10-vol-val', class: 'settings-slider-val' },
              (() => { const v = parseInt(localStorage.getItem('k10_audioVol') ?? '5'); return v === 0 ? '0 (mute)' : String(v); })()
            )
          )
        ),
        md({ class: 'settings-row' },
          mp({ class: 'settings-label' }, 'K10 LED Brightness'),
          md({ class: 'settings-slider-row' },
            minput({
              id: 'k10-led-slider', type: 'range', min: '0', max: '9', step: '1',
              value: localStorage.getItem('k10_ledBright') ?? '5',
              class: 'settings-slider',
              oninput: e => {
                const v = parseInt(e.target.value);
                const lbl = document.getElementById('k10-led-val');
                if (lbl) lbl.textContent = v === 0 ? '0 (off)' : String(v);
                localStorage.setItem('k10_ledBright', v);
                send({ t: 'settings', audioVol: parseInt(document.getElementById('k10-vol-slider')?.value ?? '5'), ledBright: v });
              }
            }),
            ms({ id: 'k10-led-val', class: 'settings-slider-val' },
              (() => { const v = parseInt(localStorage.getItem('k10_ledBright') ?? '5'); return v === 0 ? '0 (off)' : String(v); })()
            )
          )
        )
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
      md({ class: 'settings-section-divider' }),
      md({ class: 'settings-row settings-danger-row' },
        mp({ class: 'settings-label settings-danger-label' }, '⚠ SURVIVORS'),
        mp({ class: 'settings-val settings-danger-desc' },
          'Permanently erase a survivor — wipes all progress and saved data.'
        )
      ),
      md({ class: 'erase-slot-grid' },
        ...ARCHETYPES.map((a, i) =>
          mb({
            class: 'menu-erase-slot-btn',
            onclick: () => {
              if (confirm(`Erase ${a.name}?\nAll progress, score and saved data will be permanently deleted.`)) {
                send({ t: 'eraseslot', arch: i });
                showToast(`☠ ${a.name} erased.`);
              }
            }
          }, a.name)
        )
      ),
      md({ class: 'settings-section-divider' }),
      md({ class: 'settings-row settings-danger-row' },
        mp({ class: 'settings-label settings-danger-label' }, '⚠ WORLD'),
        mp({ class: 'settings-val settings-danger-desc' },
          'Regenerate the wasteland. Survivors scattered. All progress lost.'
        )
      ),
      mb({
        id: 'menu-regen-btn',
        class: 'menu-regen-btn',
        onclick: () => {
          if (confirm('Regenerate the wasteland?\nAll survivors will be scattered and progress will be lost.')) {
            send({ t: 'regen' });
            uiMenuPage.val = null;
            document.getElementById('menu-overlay')?.classList.remove('open');
          }
        }
      }, '☠ REGENERATE WORLD')
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
      h2({ class: 'cs-sel-title' }, '\u2620 CHOOSE YOUR SURVIVOR'),
      p({ class: 'cs-sel-sub' },
        'Each role has a unique trait. Select carefully \u2014 you cannot change once the wasteland claims you.'
      ),
      div({ class: 'arch-grid' },
        ...ARCHETYPES.map((arch, i) => {
          const taken = !availSet.has(i);
          if (taken) return null;
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

          return div({
            class: `arch-card${taken ? ' arch-taken' : ''}${pending ? ' arch-btn-pending' : ''}`,
            style: `--arch-color:${color}; --arch-portrait:url('img/survivors/${arch.name.toLowerCase()}.jpg')`,
            onclick: taken ? undefined : () => {
              if (pending) return;
              // If a survivor is already active, require confirmation before abandoning them
              if (myId >= 0 && players[myId]?.on) {
                if (!confirm('\u26A0 Abandon your current survivor?\nAll progress, score and position will be lost.')) return;
              }
              uiPickPending.val = true;
              send({ t: 'pick', arch: i });
              if (pickTimeoutId) clearTimeout(pickTimeoutId);
              pickTimeoutId = setTimeout(() => {
                if (uiPickPending.val) {
                  uiPickPending.val = false;
                  showToast('\u26A0 Server did not respond \u2014 please try selecting again.');
                }
                pickTimeoutId = null;
              }, 8000);
            }
          },
            div({ class: 'arch-header' },
              div({ class: 'arch-name-wrap' },
                span({ class: 'arch-name' }, arch.name)
              )
            ),
            div({ class: 'arch-skills' }, ...skillDots),
            null
          );
        })
      )
    );
  });
}

// ── Item System UI ────────────────────────────────────────────────

const CAT_NAMES = ['Gulpable', 'Bolt-On', 'Salvage', 'Relic'];
const CAT_CLASSES = ['cat-consumable', 'cat-equipment', 'cat-material', 'cat-key'];

function _itemIcon(id) {
  const item = getItemById?.(id);
  return item?.icon || ITEM_ICON_PLACEHOLDER;
}

// Render typed inventory slots into #cs-item-grid
function renderInventory() {
  const grid = document.getElementById('cs-item-grid');
  if (!grid || myId < 0) return;
  const me = players[myId];
  const arch = me.arch ?? 0;
  const slots = ARCHETYPES[arch]?.invSlots ?? 8;
  grid.innerHTML = '';
  for (let i = 0; i < slots; i++) {
    const typeId = me.it?.[i] ?? 0;
    const qty    = me.iq?.[i] ?? 0;
    const div    = document.createElement('div');
    div.className = 'item-slot' + (typeId ? ' occupied' : '');
    div.setAttribute('data-slot', i);
    if (typeId) {
      const item = getItemById?.(typeId);
      const catClass = CAT_CLASSES[item?.category ?? 0] ?? 'cat-consumable';
      div.innerHTML =
        `<img class="item-slot-icon item-icon-img" src="${escHtml(_itemIcon(typeId))}" alt="" width="26" height="26" onerror="this.src='${ITEM_ICON_PLACEHOLDER}'">` +
        `<span class="item-slot-qty">${qty > 1 ? qty : ''}</span>` +
        `<span class="item-slot-name">${escHtml(item?.name ?? '?')}</span>` +
        `<span class="item-cat-badge ${catClass}">${CAT_NAMES[item?.category ?? 0]?.slice(0, 4) ?? '?'}</span>`;
      div.addEventListener('click', () => openItemMenu(i, false));
    } else {
      div.innerHTML = `<span class="item-slot-empty-icon">\u25A1</span>`;
    }
    grid.appendChild(div);
  }
}

// Render equipment slots into #cs-equip-grid (EQUIP_HEAD..VEHICLE, equip[0..4])
function renderEquipment() {
  const grid = document.getElementById('cs-equip-grid');
  if (!grid || myId < 0) return;
  const me = players[myId];
  const SLOT_LABELS = ['NOGGIN', 'HIDE', 'MITTS', 'HOOVES', 'RUST BUCKET'];
  grid.innerHTML = '';
  for (let s = 0; s < 5; s++) {
    const itemId = me.eq?.[s] ?? 0;
    const div    = document.createElement('div');
    div.className = 'equip-slot' + (itemId ? ' filled' : '');
    div.setAttribute('data-eslot', s);
    if (itemId) {
      const item = getItemById?.(itemId);
      div.innerHTML =
        `<span class="equip-slot-name">${SLOT_LABELS[s]}</span>` +
        `<img class="equip-slot-icon item-icon-img" src="${escHtml(_itemIcon(itemId))}" alt="" width="28" height="28" onerror="this.src='${ITEM_ICON_PLACEHOLDER}'">` +
        `<span class="equip-slot-label">${escHtml(item?.name ?? '?')}</span>`;
      div.addEventListener('click', () => openItemMenu(s, true));
    } else {
      div.innerHTML =
        `<span class="equip-slot-name">${SLOT_LABELS[s]}</span>` +
        `<span class="equip-slot-empty">\u2500</span>` +
        `<span class="equip-slot-label" style="color:var(--txt-dim)">nothing</span>`;
    }
    grid.appendChild(div);
  }
}

// Item action context menu (slide-up sheet)
function openItemMenu(slotIdx, isEquipped) {
  if (myId < 0) return;
  const me = players[myId];

  let itemId, itemQty;
  if (isEquipped) {
    itemId  = me.eq?.[slotIdx] ?? 0;
    itemQty = 1;
  } else {
    itemId  = me.it?.[slotIdx] ?? 0;
    itemQty = me.iq?.[slotIdx] ?? 0;
  }
  if (!itemId) return;

  const item   = getItemById?.(itemId);
  const name   = item?.name ?? ('Item #' + itemId);
  const story  = item?.story ?? null;
  const isEquip = !isEquipped && item?.category === 1; // ITEM_EQUIPMENT=1
  const isCons  = !isEquipped && item?.category === 0; // ITEM_CONSUMABLE=0

  document.getElementById('item-menu-icon').src = _itemIcon(itemId);
  document.getElementById('item-menu-name').textContent = name;

  const storyEl = document.getElementById('item-menu-story');
  storyEl.textContent = story ?? '';
  storyEl.style.display = story ? '' : 'none';

  const btnsEl = document.getElementById('item-menu-btns');
  btnsEl.innerHTML = '';

  function addBtn(label, cls, onClick) {
    const b = document.createElement('button');
    b.className = 'item-menu-btn' + (cls ? ' ' + cls : '');
    b.textContent = label;
    b.addEventListener('click', onClick);
    btnsEl.appendChild(b);
  }

  if (isCons) {
    const preUse = item?.preUse ?? null;
    addBtn('\u25B6 Choke Down' + (preUse ? ' — ' + preUse : ''), '', () => {
      closeItemMenu();
      if (item?.postUse) showBanner(item.postUse, null);
      send({ t: 'use_item', slot: slotIdx });
    });
  }
  if (isEquip) {
    addBtn('\u25A3 Strap On', '', () => {
      closeItemMenu();
      send({ t: 'equip_item', slot: slotIdx });
    });
  }
  if (isEquipped) {
    addBtn('\u25A1 Tear Off', '', () => {
      closeItemMenu();
      send({ t: 'unequip_item', eslot: slotIdx });
    });
  }
  if (!isEquipped && item?.category !== 3) { // KEY items not shown drop button
    addBtn('\u25BC Abandon', 'danger', () => {
      closeItemMenu();
      send({ t: 'drop_item', slot: slotIdx, qty: itemQty });
    });
  }

  const menu     = document.getElementById('item-action-menu');
  const backdrop = document.getElementById('item-menu-backdrop');
  menu.classList.add('open');
  backdrop.classList.add('open');
}

function closeItemMenu() {
  document.getElementById('item-action-menu')?.classList.remove('open');
  document.getElementById('item-menu-backdrop')?.classList.remove('open');
}

// Ground items for the hex info panel
function renderHexGroundItems(q, r) {
  const row  = document.getElementById('hi-ground-row');
  const list = document.getElementById('hi-ground-list');
  if (!list || !row) return;
  const here = (typeof groundItems !== 'undefined' ? groundItems : [])
    .filter(gi => gi.q === q && gi.r === r && gi.id > 0);
  if (here.length === 0) {
    row.style.display = 'none';
    list.innerHTML = '';
    return;
  }
  row.style.display = '';
  list.innerHTML = '';
  here.forEach(gi => {
    const item = getItemById?.(gi.id);
    const name = item?.name ?? ('Item #' + gi.id);
    const span = document.createElement('span');
    span.className = 'hi-ground-pickup';
    span.title = `Pick up ${name}`;
    span.innerHTML =
      `<img class="item-icon-img" src="${escHtml(_itemIcon(gi.id))}" alt="" width="16" height="16" onerror="this.src='${ITEM_ICON_PLACEHOLDER}'">` +
      `${escHtml(name)}` +
      (gi.n > 1 ? ` \u00d7${gi.n}` : '') +
      ` <span class="gp-plus">+</span>`;
    span.addEventListener('click', () => {
      send({ t: 'pickup_item', gslot: gi.g });
      showToast(`Picking up: ${name}`);
    });
    list.appendChild(span);
  });
}

// Close item menu on cancel button or backdrop tap
document.getElementById('item-menu-close')?.addEventListener('click', closeItemMenu);
document.getElementById('item-menu-backdrop')?.addEventListener('click', closeItemMenu);

// ── Encounter overlay + ally banner ──────────────────────────────
function initEncounterOverlay() {
  const overlay    = document.getElementById('enc-overlay');
  const nodeText   = document.getElementById('enc-node-text');
  const dnLabel    = document.getElementById('enc-dn-label');
  const riskFill   = document.getElementById('enc-risk-fill');
  const lootDisp   = document.getElementById('enc-loot-display');
  const outcomeDiv = document.getElementById('enc-outcome');
  const choiceList = document.getElementById('enc-choice-list');
  const bankRow    = document.getElementById('enc-bank-row');
  const bankBtn    = document.getElementById('enc-bank-btn');
  const abortBtn   = document.getElementById('enc-abort-btn');

  const RES_NAMES_ENC = ['Water','Food','Fuel','Meds','Scrap'];

  // State for the active encounter
  let currentEnc      = null;   // loaded encounter JSON
  let currentNode     = null;   // current node object
  let pendingLoot     = [0,0,0,0,0];
  let canBank         = false;
  let pendingNextKey  = '';     // next node key sent in enc_choice, consumed by _onEncResult
  let pendingHazText  = '';     // resolved hazard narration, shown on failure

  function resolveText(text, placeholders) {
    if (!placeholders) return text;
    return text.replace(/\{\{(\w+)\}\}/g, (_, key) => {
      const opts = placeholders[key];
      return (Array.isArray(opts) && opts.length) ? opts[Math.floor(Math.random() * opts.length)] : key;
    });
  }

  function renderLoot() {
    const parts = pendingLoot.map((v, i) => v > 0 ? `${RES_NAMES_ENC[i]}×${v}` : null).filter(Boolean);
    lootDisp.textContent = parts.length ? `Pending: ${parts.join('  ')}` : '';
  }

  const SKILL_LABELS_ENC = ['Navigate','Forage','Scavenge','Treat','Shelter','Endure'];
  const RISK_LABELS_ENC  = ['', 'CAUTIOUS', 'CAUTIOUS', 'RISKY', 'DANGEROUS', 'DESPERATE'];
  function riskLabel(pct) {
    if (pct <= 0)  return '';
    if (pct <= 25) return 'CAUTIOUS';
    if (pct <= 50) return 'RISKY';
    if (pct <= 75) return 'DANGEROUS';
    return 'DESPERATE';
  }

  function renderNode(node) {
    currentNode = node;
    nodeText.textContent = resolveText(node.text, currentEnc.placeholders);

    const baseRisk = Math.max(0, ...(node.choices ?? []).map(c => c.base_risk ?? 0));
    dnLabel.textContent = riskLabel(baseRisk);
    riskFill.style.width = `${baseRisk}%`;

    choiceList.innerHTML = '';
    (node.choices ?? []).forEach(ch => {
      const btn = document.createElement('button');
      btn.className = 'enc-choice-btn';
      const riskClass = ch.base_risk <= 30 ? 'enc-risk-low' : ch.base_risk <= 60 ? 'enc-risk-mid' : 'enc-risk-high';
      const cost = ch.cost ?? {};
      const costParts = [
        cost.ll        ? `${cost.ll} LL`        : null,
        cost.fatigue   ? `${cost.fatigue} Fat`   : null,
        cost.radiation ? `${cost.radiation} Rad` : null,
        cost.food      ? `${cost.food} Food`     : null,
        cost.water     ? `${cost.water} Water`   : null,
      ].filter(Boolean);
      btn.innerHTML =
        `<span>${escHtml(resolveText(ch.label, currentEnc.placeholders))}</span>` +
        `<span class="enc-cost-tag">${costParts.length ? 'Cost: ' + costParts.join(' \u00b7 ') : 'No cost'}</span>` +
        `<span class="enc-risk-tag ${riskClass}">${SKILL_LABELS_ENC[ch.skill] ?? 'Skill'}</span>`;
      btn.addEventListener('click', () => sendChoice(ch));
      choiceList.appendChild(btn);
    });

    canBank = node.can_bank ?? false;
    bankRow.style.display = canBank ? '' : 'none';
    renderLoot();
  }

  function sendChoice(ch) {
    const haz    = (ch.hazard_id && currentEnc.hazards) ? (currentEnc.hazards[ch.hazard_id] ?? {}) : {};
    pendingHazText = haz.text ? resolveText(haz.text, currentEnc.placeholders) : '';
    const hazPen = haz.penalty ?? {};
    const wound  = Array.isArray(haz.wound) ? haz.wound : [0, 0];
    const cost   = ch.cost ?? {};
    const nextKey = ch.success_node ?? '';

    // Loot is defined on the destination node, not on the choice.
    // Roll qty client-side; server trusts the values (SD is not player-modifiable).
    const nextNode = nextKey ? currentEnc?.nodes?.[nextKey] : null;
    const nodeLoot = [0,0,0,0,0];
    if (nextNode?.loot) {
      nextNode.loot.forEach(entry => {
        const res = entry.res;
        if (res >= 0 && res < 5 && Array.isArray(entry.qty)) {
          const mn = entry.qty[0], mx = entry.qty[1] ?? entry.qty[0];
          nodeLoot[res] += mn + Math.floor(Math.random() * (mx - mn + 1));
        }
      });
    }
    const lootTable = nextNode?.loot_table ?? '';

    send({
      t:           'enc_choice',
      base_risk:   ch.base_risk  ?? 50,
      skill:       ch.skill      ?? 2,
      loot:        nodeLoot,
      lt:          lootTable,
      can_bank:    nextNode?.can_bank ?? false,
      ci:          nextKey,
      cost_ll:     cost.ll        ?? 0,
      cost_fat:    cost.fatigue   ?? 0,
      cost_rad:    cost.radiation ?? 0,
      cost_food:   cost.food      ?? 0,
      cost_water:  cost.water     ?? 0,
      haz_ll:      hazPen.ll        ?? 0,
      haz_fat:     hazPen.fatigue   ?? 0,
      haz_rad:     hazPen.radiation ?? 0,
      haz_st:      haz.status       ?? 0,
      haz_wt:      wound[0]         ?? 0,
      haz_wc:      wound[1]         ?? 0,
      haz_ends:    haz.ends_encounter ? 1 : 0,
      is_terminal: nextKey === '' ? 1 : 0,
    });
    pendingNextKey = nextKey;
    choiceList.querySelectorAll('.enc-choice-btn').forEach(b => b.disabled = true);
  }

  function openEncounter(enc) {
    currentEnc  = enc;
    pendingLoot = [0,0,0,0,0];
    overlay.classList.add('open');
    overlay.style.display = '';
    const startNode = enc.nodes?.[enc.start_node] ?? Object.values(enc.nodes ?? {})[0];
    if (startNode) renderNode(startNode);
  }

  function closeEncounter() {
    overlay.classList.remove('open');
    overlay.style.display = 'none';
    currentEnc     = null;
    currentNode    = null;
    pendingLoot    = [0,0,0,0,0];
    pendingNextKey = '';
    pendingHazText = '';
    canBank        = false;
    lootDisp.textContent  = '';
    outcomeDiv.classList.remove('visible');
    outcomeDiv.innerHTML  = '';
    bankRow.style.display = 'none';
    choiceList.innerHTML  = '';
  }

  // Called from engine.js enc_path handler
  window._startEncounterFetch = function(biome, id) {
    fetch(`/enc?biome=${encodeURIComponent(biome)}&id=${encodeURIComponent(id)}`)
      .then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
      .then(enc => openEncounter(enc))
      .catch(e => showToast(`\u2297 Encounter load failed: ${e.message}`));
  };

  const SUCCESS_PHRASES = [
    'You push through.', 'Barely.', 'Fortune holds.', 'Against the odds.',
    'Clean exit.', 'You manage.', 'Just in time.', 'Through.'
  ];

  // Called from engine.js enc_res handler — show outcome then advance or stay
  window._onEncResult = function(ev) {
    if (ev.ends) { closeEncounter(); return; }

    const nextKey = pendingNextKey;
    pendingNextKey = '';

    // Build delta line
    const deltaItems = [];
    if (ev.out && Array.isArray(ev.loot)) {
      ev.loot.forEach((v, i) => { if (v > 0) deltaItems.push({ txt: `+${v} ${RES_NAMES_ENC[i]}`, pos: true }); });
    } else {
      if (ev.penLL  < 0) deltaItems.push({ txt: `${ev.penLL} Life`,         pos: false });
      if (ev.penFat > 0) deltaItems.push({ txt: `+${ev.penFat} Fatigue`,    pos: false });
      if (ev.penRad > 0) deltaItems.push({ txt: `+${ev.penRad} Radiation`,  pos: false });
    }
    const deltaHtml = deltaItems.map(d =>
      `<span class="${d.pos ? 'enc-out-pos' : 'enc-out-neg'}">${escHtml(d.txt)}</span>`
    ).join('  ');

    // Flavor text: use authored hazard narration on failure, generic phrase on success
    const flavor = ev.out
      ? SUCCESS_PHRASES[Math.floor(Math.random() * SUCCESS_PHRASES.length)]
      : (pendingHazText || 'The wasteland takes its toll.');
    pendingHazText = '';

    outcomeDiv.innerHTML =
      `<span class="enc-out-flavor">${escHtml(flavor)}</span>` +
      (deltaHtml ? `<span class="enc-out-delta">${deltaHtml}</span>` : '');
    outcomeDiv.classList.add('visible');

    setTimeout(() => {
      outcomeDiv.classList.remove('visible');
      outcomeDiv.innerHTML = '';
      if (ev.out) {
        if (Array.isArray(ev.loot))
          ev.loot.forEach((v, i) => { pendingLoot[i] = (pendingLoot[i] ?? 0) + v; });
        if (nextKey && currentEnc?.nodes?.[nextKey]) {
          renderNode(currentEnc.nodes[nextKey]);
        } else {
          renderLoot();
          choiceList.innerHTML = '';
          canBank = true;
          bankRow.style.display = '';
        }
      } else {
        renderNode(currentNode);
      }
    }, 2200);
  };

  window._onEncBank = function() { closeEncounter(); };
  window._onEncEnd  = function() { closeEncounter(); };

  bankBtn.addEventListener('click', () => {
    send({ t: 'enc_bank' });
    closeEncounter();
  });

  abortBtn.addEventListener('click', () => {
    send({ t: 'enc_abort' });
    closeEncounter();
  });

  // Fatigue track in encounter overlay
  van.derive(() => {
    renderTrackBoxes('enc-fat-track', uiFat.val, [], 0, 8);
  });
}

// ── VanJS entry point ─────────────────────────────────────────────
function initVanJS() {
  initHudBindings();
  initCharSheetBindings();
  initMapBindings();
  initActionPanel();
  initTradeOverlay();
  initPlayerList();
  initMenuSystem();
  initCharSelect();
  initEncounterOverlay();
}

// ── Boot ──────────────────────────────────────────────────────────
initVanJS();
connect();
requestAnimationFrame(render);
