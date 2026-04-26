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
const uiLL          = van.state(7);
const uiFood        = van.state(6);
const uiWater       = van.state(6);
const uiMP          = van.state(6);
// §6.2 Radiation track
const uiRad         = van.state(0);
// §5 Action tracking
const uiResting     = van.state(false);  // true after REST until dawn
const uiMaxMP = van.state(6);  // reactive — drives MP track box count in HUD
// Menu navigation (null=closed, 'main'|'howto'|'settings'|'about')
const uiMenuPage    = van.state(null);
// Log panel visibility (persisted to localStorage)
const uiLogVisible  = van.state(localStorage.getItem('logVisible') === '1');
// K10 screen flip (persisted to localStorage)
const uiScreenFlip  = van.state(localStorage.getItem('k10_screenFlip') === '1');
// Overlay open states — toggled via .val; van.derive in ui.js handles class changes
const uiHexInfoOpen = van.state(false);
const uiCharOpen    = van.state(false);
van.derive(() => {
  const lp = document.getElementById('log-panel');
  if (lp) lp.style.display = uiLogVisible.val ? '' : 'none';
});
function openMenu(page = 'main') { uiMenuPage.val = page; }
function closeMenu()             { uiMenuPage.val = null; }
function narrateState(msg) {
  const el = document.getElementById('game-narrator');
  if (el) el.textContent = msg;
}

// ── Narrative effect system (EFX_NARRATIVE item params) ──────────────────
let _trippyTurns = 0;       // turns remaining with .trippy class active
let invertedInputTurns = 0; // turns remaining with reversed WASD
function handleNarrativeEffect(param) {
  if (!param || param === 0) return;
  switch (param) {
    case 11: // Trippy Juice — UI hue-scramble for 3 turns
      document.body.classList.add('trippy');
      _trippyTurns = 3;
      showToast('✦ Reality slips its leash. The map no longer holds still.');
      break;
    case 12: // Cursed Device — reverse movement keys for 5 turns
      invertedInputTurns = 5;
      showToast('◌ Your senses flip inside out. North is south, and south mocks you.');
      break;
    case 27: // Jar of Sweats — vomit, lose turn
      showToast('☠ Your gut heaves. You retch the next turn into the dirt.');
      break;
    case 29: // Uranium Hard Candy — warn about LL ceiling
      showToast('☢ The rads settle into your marrow. You will never be whole again.');
      break;
    case 25: // Knife-Wrench — catastrophic failure, self-damage
      showToast('✦ The Knife-Wrench turns on you. You take the damage yourself.');
      break;
    default:
      break;
  }
}
// Clear trippy effect when a new day starts (called on dawn event)
function _tickNarrativeEffects() {
  if (_trippyTurns > 0) { _trippyTurns--; if (_trippyTurns === 0) document.body.classList.remove('trippy'); }
  if (invertedInputTurns > 0) invertedInputTurns--;
}

// Lobby / character selection
const lobbyAvail    = van.state([]);    // archetype indices currently available to pick
const uiPickPending = van.state(false); // true while waiting for server pick response
let pickTimeoutId = null;               // safety: auto-clear uiPickPending if server never responds

// ── Character selection overlay helpers ──────────────────────────
function showCharSelect() {
  // Guard: never show char-select while a live survivor exists
  if (myId >= 0 && players[myId]?.on) {
    console.log('[charSelect] BLOCKED by guard: myId=%d on=%s', myId, players[myId]?.on);
    return;
  }
  console.log('[charSelect] SHOW — myId=%d lobbyAvail=%o', myId, lobbyAvail.val);
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
