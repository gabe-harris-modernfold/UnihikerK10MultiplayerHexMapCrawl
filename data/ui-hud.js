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

  // Ground items at this hex (renderHexGroundItems defined in ui-items.js)
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
  const cell = gameMap[me.r]?.[me.q];
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
  const phase = (typeof weatherPhase === 'undefined') ? 0 : weatherPhase;
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
  uiMP.val      = me.mp   ?? 0;
  uiRad.val     = me.rad  ?? 0;
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
  let blockMsg;
  if (inEnc)           blockMsg = 'Inside encounter \u2014 complete or bank first';
  else if (resting)    blockMsg = 'Resting \u2014 waiting for dawn';
  else if (exhausted)  blockMsg = 'No MP \u2014 REST to recover';
  else                 blockMsg = 'Blocked (impassable)';
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
  el.textContent = phase.icon + ' ' + phase.name;
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
  if (archIcon) { archIcon.textContent = arch.icon; archIcon.style.color = ''; }
  if (archName) { archName.textContent = arch.name; archName.style.color = ''; }
  document.getElementById('char-sheet')
    .style.setProperty('--cs-portrait', `url('img/survivors/${arch.name.toLowerCase()}.jpg')`);
  const archTrait = document.getElementById('cs-arch-trait');
  if (archTrait) archTrait.textContent = arch.trait || '';

  // Skills grid
  const grid = document.getElementById('cs-skills-grid');
  if (grid) {
    grid.innerHTML = '';
    SK_NAMES.forEach((name, i) => {
      const lvl = me.sk?.[i] || 0;
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
      let lvlLabel;
      if (lvl === 2)      lvlLabel = 'EXPERT';
      else if (lvl === 1) lvlLabel = 'TRAINED';
      else                lvlLabel = '—';
      lbl.textContent = lvlLabel;
      row.appendChild(label);
      row.appendChild(dots);
      row.appendChild(lbl);
      grid.appendChild(row);
    });
  }

  uiCharOpen.val = true;
  // renderInventory and renderEquipment are defined in ui-items.js
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
  if (uiCharOpen.val) resetLastEqKey(); // force fresh render on open (defined in ui-items.js)
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

// ── VanJS HUD / char-sheet / map bindings ────────────────────────

function initHudBindings() {
  // Connection dot
  const connDot = document.getElementById('hud-conn-dot');
  if (connDot) {
    van.derive(() => {
      const conn = uiConn.val;
      connDot.style.color = (conn === 'Connected' ? 'var(--gold-hi)' : '#C06030');
      let ariaLabel;
      if (conn === 'Connected')     ariaLabel = 'connected';
      else if (conn === 'Connecting...') ariaLabel = 'connecting';
      else                          ariaLabel = 'disconnected';
      connDot.setAttribute('aria-label', ariaLabel);
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
function _trackBoxClass(filled, thr, fired) {
  let cls = 'track-box';
  if (filled) cls += ' filled';
  if (thr && !fired) cls += filled ? ' thresh-filled'       : ' thresh';
  if (thr &&  fired) cls += filled ? ' thresh-spent-filled' : ' thresh-spent';
  return cls;
}

function renderTrackBoxes(containerId, value, thresholds, firedMask = 0, count = 6) {
  const el = document.getElementById(containerId);
  if (!el) return;
  // Trim excess boxes when count decreases (e.g. maxMP changes between days)
  while (el.children.length > count) el.lastChild.remove();
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
    b.className  = _trackBoxClass(filled, thr, fired);
  }
  el.setAttribute('aria-valuenow', value);
  el.setAttribute('aria-valuemax', count);
}

function applyRadColors(rad) {
  const trackEl = document.getElementById('cs-rad-track');
  if (!trackEl) return;
  for (let i = 0; i < 10; i++) {
    const b   = trackEl.children[9 - i];
    const box = i + 1;
    b.classList.remove('rad-filled', 'rad-filled-yellow', 'rad-filled-red');
    if (box <= rad) {
      if      (box >= 7) b.classList.add('rad-filled-red');
      else if (box >= 4) b.classList.add('rad-filled-yellow');
      else               b.classList.add('rad-filled');
    }
  }
}

function applyRadStatus(rad) {
  const rStat = document.getElementById('cs-rad-status');
  if (!rStat) return;
  let text = '';
  let suffix = '';
  if (rad >= 7)      { text = '\u00a0\u2622 DUSK CHECK'; suffix = ' rad-status-critical'; }
  else if (rad >= 4) { text = '\u00a0RAD-SICK';          suffix = ' rad-status-sick'; }
  rStat.textContent = text;
  rStat.className   = 'track-suffix' + suffix;
}

function initSurvivalTracks() {
  // Char sheet tracks — built by makeSegmentBar
  const section = document.getElementById('cs-survival-section');
  if (section) {
    section.appendChild(makeSegmentBar({ id: 'cs-ll-track',    label: 'LIFE LEVEL', count: 7  }));
    section.appendChild(makeSegmentBar({ id: 'cs-food-track',  label: 'FOOD',       count: 6  }));
    section.appendChild(makeSegmentBar({ id: 'cs-water-track', label: 'WATER',      count: 6  }));
    const radRow = makeSegmentBar({ id: 'cs-rad-track', label: 'RADIATION', count: 10 });
    radRow.querySelector('.track-boxes').classList.add('rad-track');
    const radStatus = document.createElement('span');
    radStatus.className = 'track-suffix';
    radStatus.id = 'cs-rad-status';
    radRow.appendChild(radStatus);
    section.appendChild(radRow);
  }

  // HUD tracks — replace static meter elements in-place
  ['hud-ll-track', 'hud-mp-track'].forEach(oldId => {
    const old = document.getElementById(oldId);
    if (!old) return;
    const count = oldId === 'hud-ll-track' ? 7 : 6;
    old.replaceWith(makeSegmentBar({ id: oldId, count, meterOnly: true }));
  });
}

function initCharSheetBindings() {
  initSurvivalTracks();

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


  // Radiation track (10 boxes; colour zones: 1-3 green, 4-6 yellow, 7-10 red)
  van.derive(() => {
    const rad = uiRad.val;
    renderTrackBoxes('cs-rad-track', rad, [{ box: 4, bit: 0 }, { box: 7, bit: 0 }], 0, 10);
    applyRadColors(rad);
    applyRadStatus(rad);
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
    let badgeClass = '';
    let badgeText  = '';
    if (t.vis > 0)      { badgeClass = 'vis-high';    badgeText = '◉ HIGH VIS +2'; }
    else if (t.vis < 0) { badgeClass = 'vis-penalty'; badgeText = '◎ VIS PENALTY'; }
    badge.className   = badgeClass;
    badge.textContent = badgeText;
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
