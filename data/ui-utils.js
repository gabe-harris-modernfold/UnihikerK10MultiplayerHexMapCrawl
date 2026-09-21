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
  return String(s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;');
}

// ── Toast stack ───────────────────────────────────────────────────
const toastStack = document.getElementById('toast-stack');
const TOAST_MAX   = 3;      // max toasts visible simultaneously
const TOAST_LIFE  = 3200;   // ms before fade starts
const TOAST_FADE  = 300;    // ms fade-out duration
let   toastQueue  = [];     // pending messages
let   toastActive = 0;      // currently visible count

function _nextToast() {
  if (toastActive >= TOAST_MAX || toastQueue.length === 0) return;
  const { msg, variant } = toastQueue.shift();
  toastActive++;
  const el = document.createElement('div');
  el.className   = 'toast-item' + (variant ? ` toast-${variant}` : '');
  el.textContent = msg;
  toastStack.appendChild(el);
  setTimeout(() => el.classList.add('dying'), TOAST_LIFE);
  setTimeout(() => {
    el.remove();
    toastActive--;
    _nextToast();          // show next queued toast when a slot opens
  }, TOAST_LIFE + TOAST_FADE);
}

// `variant` adds a `toast-<variant>` class for callers that need a different
// voice from the default gold system notice — today just 'doom', which styles
// the Creeping Doom's taunts as something speaking rather than the UI
// reporting. Omitted by every other caller, so their toasts are unchanged.
function showToast(msg, variant) {
  toastQueue.push({ msg, variant });
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

// ── Direct-uplink (softAP) warning ──────────────────────────
// Joining WASTELAND directly puts the board's single radio on double duty:
// beaconing, handing out leases, sweeping for a network to join, AND running
// the game. Over a real router it only has to be a client. Players feel that
// difference and blame the game, so the game admits it first, in its own voice.
// Driven by the {t:'wifi',status:'link'} message from handleConnect().
const UPLINK_WARNINGS = [
  ['ONE ANTENNA, TWO MASTERS',
   'you are wired straight into the board. it beacons, it routes, it thinks — and it drops things.'],
  ['THE BOARD IS ITS OWN TOWER NOW',
   'no relay. no redundancy. no mercy. when the world stutters, that was not your reflexes.'],
  ['DIRECT UPLINK — SURVIVABLE, NOT PLEASANT',
   'the board is rationing airtime between hosting you and listening for rescue.'],
  ['YOU ARE DRINKING FROM THE SOURCE',
   'WASTELAND is the board talking to itself. it was never built to carry a crowd.'],
  ['ROOM FOR {cap} AT THIS FIRE',
   'a direct uplink seats {cap}. the next survivor waits outside in the dust.'],
];
let uplinkWarned = false;
const UPLINK_BANNER_HOLD_MAX = 300000;   // stop waiting on character select after 5 min

// cap   - softAP client limit the firmware reports (AP_MAX_CLIENTS)
// staIp - the board's address on a real network, or '' if it has no uplink
function showUplinkWarning(cap, staIp) {
  if (uplinkWarned) return;   // this is a warning, not a nag — once per session
  uplinkWarned = true;
  const n = cap > 0 ? String(cap) : 'few';
  const [main, sub] = UPLINK_WARNINGS[Math.floor(Math.random() * UPLINK_WARNINGS.length)];
  // The log carries the fix and scrolls back, so write it now — it is already
  // in the history by the time the player thinks to look for it.
  addLog('<span class="log-check-fail">⚠ DIRECT UPLINK</span> — one radio is hosting you '
       + 'and running the wasteland at the same time. expect it to stutter.');
  if (staIp) {
    addLog('<span class="log-join">⇒ THE TOWER STILL ANSWERS</span> at <b>' + escHtml(staIp)
         + '</b> — leave WASTELAND, rejoin your own network, browse there. the world steadies.');
  }
  // The banner carries the joke, but it sits at z-index 30 and character select
  // at 900 — firing it now buries it behind the picker and burns its six seconds
  // unseen. Hold until the player is actually in the wasteland, which is also
  // when the stutter it explains starts to show. If they never pick, drop it:
  // the log lines above already said everything that matters.
  const picking = () => !!document.getElementById('char-select-overlay')?.classList.contains('open');
  let held = 0;
  const arm = () => {
    if (!picking()) { showBanner(main.replaceAll('{cap}', n), sub.replaceAll('{cap}', n)); return; }
    held += 500;
    if (held < UPLINK_BANNER_HOLD_MAX) setTimeout(arm, 500);
  };
  setTimeout(arm, 1200);
}

// Bad air. Its own list rather than a SHELTER_WARNINGS entry: the advice at
// the bottom of a shelter warning is "build something", and down here the
// only answer is to climb back out, so nothing in that list is true.
const BAD_AIR_WARNINGS = [
    ['YOU WOKE CHOKING ON DEAD AIR',   'the bunker does not breathe — surface to sleep'],
    ['THE TUNNEL TOOK A BREATH BACK',  'no weather down here, but no air either'],
    ['STILL AIR, HEAVY LUNGS',         'a night below costs what the sky would have'],
    ['SOMETHING IN THE DARK IS FOUL',  'climb out before you sleep here again'],
];

function showBadAirWarning() {
  const [main, sub] = BAD_AIR_WARNINGS[Math.floor(Math.random() * BAD_AIR_WARNINGS.length)];
  showBanner(main, sub);
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
let lastMoveSent = 0;
function move(dir) {
  if (typeof invertedInputTurns !== 'undefined' && invertedInputTurns > 0) dir = (dir + 3) % 6;
  if (myId >= 0 && players[myId]?.ll === 0) {
    addLog('<span class="log-check-fail">☠ Cannot move — you have been downed.</span>');
    return;
  }
  if (myId >= 0 && players[myId]?.enc) return;
  if (myId >= 0 && uiMP.val <= 0) return;
  const vm = myId >= 0 ? (players[myId].vm ?? 0x3F) : 0x3F;
  if (!(vm & (1 << dir))) return;
  const now = Date.now();
  if (now - lastMoveSent < moveCooldownMs - 30) {
    document.querySelectorAll('.dir-btn[data-dir]').forEach(b => {
      b.classList.remove('on-cooldown');
      void b.offsetWidth; // force reflow to restart CSS animation
      b.classList.add('on-cooldown');
    });
    return;
  }
  lastMoveSent = now;
  send({ t: 'm', d: dir });
}

document.querySelectorAll('.dir-btn[data-dir]').forEach(btn => {
  const dir = Number.parseInt(btn.dataset.dir);
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

// ── Segment bar factory ───────────────────────────────────────────
// Creates a .track-row element: [label] [box][box][box]…
// opts: { id, label, count=6, color=null, colorHi=null, labelWidth=null }
//   id        — given to the meter element; pass to renderTrackBoxes() to update
//   label     — text shown to the left (omit to skip label)
//   count     — total number of segments (default 6)
//   color     — CSS color for filled segments (default: amber --gold)
//   colorHi   — CSS color for filled segment border (defaults to color)
//   labelWidth — override label width (default 80px)
// opts.meterOnly — return just the container element without the .track-row wrapper
function makeSegmentBar({ id, label, count = 6, color = null, colorHi = null, labelWidth = null, meterOnly = false } = {}) {
  const meter = document.createElement('div');
  meter.className = 'track-boxes';
  meter.setAttribute('role', 'meter');
  if (id) meter.id = id;
  if (label) meter.setAttribute('aria-label', label);
  meter.setAttribute('aria-valuenow', '0');
  meter.setAttribute('aria-valuemax', String(count));
  if (color)   meter.style.setProperty('--seg-color',    color);
  if (colorHi) meter.style.setProperty('--seg-color-hi', colorHi);
  for (let i = 0; i < count; i++) {
    const b = document.createElement('div');
    b.className = 'track-box';
    meter.appendChild(b);
  }

  if (meterOnly) return meter;

  const row = document.createElement('div');
  row.className = 'track-row';
  if (label) {
    const lbl = document.createElement('span');
    lbl.className = 'track-label';
    if (labelWidth) lbl.style.width = labelWidth;
    lbl.textContent = label;
    row.appendChild(lbl);
  }
  row.appendChild(meter);
  return row;
}

// Keyboard: Q=NW(3) W=N(2) E=NE(1)  S=S(5) D=SE(0)  [A freed for ACTION shortcut]
const keyMap = {
  'KeyQ':'3','KeyW':'2','KeyE':'1','KeyS':'5','KeyD':'0',
  'ArrowUp':'2','ArrowDown':'5','ArrowLeft':'3','ArrowRight':'0',
  'Numpad7':'3','Numpad8':'2','Numpad9':'1',
  'Numpad4':'4','Numpad6':'0','Numpad1':'4','Numpad2':'5','Numpad3':'0',
};
const heldKeys = new Map();
document.addEventListener('keydown', e => {
  // Innermost overlay first: the resource-drop sheet (ui-items.js) sits above
  // the char sheet, so Escape must dismiss it *instead of* closing the sheet
  // underneath. Checked before the INPUT guard so Escape also works while the
  // quantity box has focus.
  if (e.key === 'Escape' && document.getElementById('res-drop-menu')?.classList.contains('open')) {
    e.preventDefault();
    globalThis.closeResDropMenu?.();
    return;
  }
  if (e.target.tagName === 'INPUT') return;
  // Prevent scroll keys from scrolling any overlay or page
  if (['Space','PageUp','PageDown','Home','End'].includes(e.code)) { e.preventDefault(); return; }
  if (e.key === 'Escape') {
    if (uiMenuPage.val) { closeMenu(); return; }
    uiCharOpen.val    = false;
    uiHexInfoOpen.val = false;
    return;
  }
  // Map zoom: +/= in, -/_ out, 0 reset (renderer.js owns the clamp + persistence)
  if (e.key === '+' || e.key === '=') { e.preventDefault(); nudgeZoom(+1); return; }
  if (e.key === '-' || e.key === '_') { e.preventDefault(); nudgeZoom(-1); return; }
  if (e.key === '0')                  { e.preventDefault(); resetZoom();   return; }
  // FAB shortcuts: R=Rest, A=Action, C=Survivor
  if (e.code === 'KeyR') { e.preventDefault(); document.getElementById('fab-rest-btn')?.click(); return; }
  if (e.code === 'KeyA') { e.preventDefault(); document.getElementById('fab-action-btn')?.click(); return; }
  if (e.code === 'KeyC') { e.preventDefault(); document.getElementById('fab-char-btn')?.click(); return; }
  // Block movement while the character selection screen or the resource-drop
  // sheet is showing — stepping off the hex mid-drop would land the tokens
  // somewhere other than the hex the sheet just described.
  if (document.getElementById('char-select-overlay')?.classList.contains('open')) return;
  if (document.getElementById('res-drop-menu')?.classList.contains('open')) return;
  const dir = keyMap[e.code];
  if (dir === undefined) return;
  e.preventDefault();
  if (!heldKeys.has(e.code)) {
    move(Number.parseInt(dir));
    heldKeys.set(e.code, setInterval(() => move(Number.parseInt(dir)), 200));
  }
  // Keep keyboard focus on the game canvas so subsequent keys land here
  document.getElementById('canvas-wrap')?.focus({ preventScroll: true });
});
document.addEventListener('keyup', e => {
  if (heldKeys.has(e.code)) { clearInterval(heldKeys.get(e.code)); heldKeys.delete(e.code); }
});

// Swipe on canvas (one finger = move) / pinch (two fingers = zoom).
// Both gestures share the canvas, so pointer bookkeeping lives in one place:
// the moment a second pointer lands, the pending swipe is cancelled so lifting
// either finger can't fire a spurious move.
let swipeStart = null;
const _swipeEl = document.getElementById('hexCanvas');
const _pointers = new Map();   // pointerId -> {x, y}
let   _pinchStartDist = 0;     // >0 while a pinch is in progress
let   _pinchStartZoom = 0;

const PINCH_PX_PER_STEP = 110; // finger-spread distance that equals one zoom step
                               // (a step is 1.25x now, so this is deliberately long)

function _pinchDist() {
  const [a, b] = [..._pointers.values()];
  return Math.hypot(a.x - b.x, a.y - b.y);
}
function _endPinch(e) {
  _pointers.delete(e.pointerId);
  if (_pointers.size < 2) _pinchStartDist = 0;
}

_swipeEl.addEventListener('pointerdown', e => {
  _pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (_pointers.size === 1) swipeStart = { x: e.clientX, y: e.clientY };
  if (_pointers.size >= 2) {
    swipeStart      = null;              // cancel the move gesture
    _pinchStartDist = _pinchDist();
    _pinchStartZoom = getZoomStep();
  }
});
_swipeEl.addEventListener('pointermove', e => {
  if (!_pointers.has(e.pointerId)) return;
  _pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (_pointers.size !== 2 || _pinchStartDist <= 0) return;
  e.preventDefault();
  setZoomStep(_pinchStartZoom + (_pinchDist() - _pinchStartDist) / PINCH_PX_PER_STEP);
});
_swipeEl.addEventListener('pointercancel', e => { _endPinch(e); swipeStart = null; });
_swipeEl.addEventListener('pointerup',   e => {
  const wasPinching = _pointers.size >= 2;
  _endPinch(e);
  if (wasPinching) { swipeStart = null; return; }
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
