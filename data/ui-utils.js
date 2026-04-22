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
    ['ASH RAIN SCOURED YOUR LUNGS', 'roofs block the fallout — build a shelter'],
    ['THE HOWLING DUST CHOKED YOU BLIND', 'secure a shelter or hovel to breathe'],
    ['SCAVENGERS TORE AT YOU IN THE DARK', 'four walls keep the vermin out'],
    ['IRRADIATED DEW BURNED YOUR SKIN', 'craft a shelter before the midnight fog'],
    ['BLACK SHIVER SET IN AS THE SUN DIED', 'a campfire and walls will warm you'],
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
