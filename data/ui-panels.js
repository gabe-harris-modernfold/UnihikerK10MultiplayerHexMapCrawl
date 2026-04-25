function getShelterDesc(shelterMaxed, shelterLevel, scrap, mp) {
  if (shelterMaxed) return 'Improved shelter already here — nothing to build';
  if (shelterLevel === 1 && scrap === 0) return 'Shelter here — needs scrap to upgrade';
  if (shelterLevel === 1 && scrap >= 2 && mp >= 2) return '2 scrap → improved shelter \u2302 (2 MP, +8 pts)';
  if (shelterLevel === 1) return '1 scrap → upgrade to improved \u2302 (1 MP, +4 pts)';
  if (scrap === 0) return 'Needs scrap — none in pack';
  if (scrap === 1) return '1 scrap → shelter \u2302 (1 MP, +4 pts)';
  if (mp < 2) return '1 scrap → shelter \u2302 (1 MP, +4 pts) — not enough MP for improved';
  return '2 scrap → improved shelter \u2302 (2 MP, +8 pts)';
}

function getBlockReason(def, shelterLevel, available, hasMP, slotFree, mp, scrap) {
  const shelterMaxed = def.id === ACT_SHELTER && shelterLevel >= 2;
  const hasScrap = def.id !== ACT_SHELTER || scrap > 0;
  if (shelterMaxed) return 'Max shelter built here';
  if (def.id === ACT_SHELTER && shelterLevel === 1 && !hasScrap) return 'Shelter here — need scrap to upgrade';
  if (!available) {
    if (def.id === ACT_FORAGE) return 'Needs Forage terrain (Rust Forest · Marsh · Open Scrub)';
    if (def.id === ACT_WATER)  return 'Needs Water terrain (Marsh \u00b7 Flooded District)';
    if (def.id === ACT_SCAV)   return 'Needs Salvage terrain (Broken Urban · Glass Fields)';
    if (def.id === ACT_TRADE)  return 'No survivors on this hex';
    return 'Not available here';
  }
  if (!hasMP)    return `Needs ${def.mpCost} MP (have ${mp})`;
  if (!hasScrap) return `Needs scrap (have ${scrap})`;
  if (!slotFree) return 'Action used this cycle \u2014 REST to reset';
  return '';
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
    if (allZero) return;
    send({ t: 'trade_offer', to: tradeTargetPid, give: [...tradeGive], want: [...tradeWant] });
    closeActionPanel();
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

  function showExhaustedPanel(me) {
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
  }

  function showRestingPanel(mp) {
    actionBtnList.innerHTML =
      '<div class="act-exhausted-msg">\ud83d\ude34 RESTING \u2014 waiting for dawn</div>';
    actionStatusBar.innerHTML =
      `<span class="act-mp-badge">MP: ${mp}</span>` +
      '<span class="act-used-badge">\u2297 RESTING</span>';
    actionPanel.classList.add('open');
    actionPanel.setAttribute('aria-hidden', 'false');
  }

  function openActionPanel() {
    if (myId < 0) return;
    if (players[myId]?.ll === 0) {
      addLog('<span class="log-check-fail">☠ Cannot act — you have been downed.</span>');
      return;
    }
    if (players[myId]?.enc) return;
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
      showExhaustedPanel(me);
      return;
    }

    // Fix: suppress action menu when resting — show resting banner instead
    if (uiResting.val) {
      showRestingPanel(mp);
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
      { id: ACT_SHELTER, icon: '\u2302', label: shelterLabel,    mpCost: shelterMpCost, desc: 'Construct shelter — needs scrap (1–2 MP, no roll)' },
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
      const terrAvail  = terr === null || (!shelterMaxed && actAvailable(def.id, terr));
      const available  = def.id === ACT_TRADE ? tradeAvail : terrAvail;
      const hasMP      = mp >= def.mpCost;
      const hasScrap   = def.id !== ACT_SHELTER || scrap > 0;
      // Actions that bypass the action slot (deterministic — no skill roll):
      const slotless   = def.id === ACT_SHELTER || def.id === ACT_WATER || def.id === ACT_TRADE
                       || (def.id === ACT_SURVEY && isScout);
      const slotFree   = slotless || !actUsed;
      const canAct     = available && hasMP && hasScrap && slotFree;

      // Dynamic desc: BUILD/UPGRADE SHELTER shows actual cost
      let desc = def.desc;
      if (def.id === ACT_SHELTER) {
        desc = getShelterDesc(shelterMaxed, shelterLevel, scrap, mp);
      }

      // Compute the inline block reason shown under the button label
      const blockReason = getBlockReason(def, shelterLevel, available, hasMP, slotFree, mp, scrap);

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
        if (!canAct) return;
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
    if (myId >= 0 && players[myId]?.ll === 0) return;
    if (myId >= 0 && players[myId]?.enc) return;
    if (uiResting.val || restSent) return;
    restSent = true; // NOSONAR — declared in network.js
    updateRestIndicator();
    send({ t: 'act', a: ACT_REST });
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
  globalThis.openActionPanel = openActionPanel;
}

function initTradeOverlay() {
  const overlay = document.getElementById('trade-overlay');
  let pendingTradeFrom = -1;
  let expireInterval   = null;
  let expireEnd        = 0;

  globalThis._openTradeOffer = function(fromPid, give, want) {
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
  globalThis._closeTradeOverlay = _closeTradeOverlay;

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

function populateSlider(sliderId, labelId, storageKey, defaultVal, offLabel) {
  const slider = document.getElementById(sliderId);
  if (!slider) return;
  const v = Number.parseInt(localStorage.getItem(storageKey) ?? String(defaultVal));
  slider.value = v;
  const lbl = document.getElementById(labelId);
  if (lbl) lbl.textContent = v === 0 ? offLabel : String(v);
}

function initMenuSystem() {
  const { div: md, span: ms, button: mb, h2: mh2, h3: mh3, p: mp, input: minput, br: mbr } = van.tags;

  const menuOverlay = document.getElementById('menu-overlay');
  van.derive(() => { menuOverlay.classList.toggle('open', uiMenuPage.val !== null); });

  // Pre-populate settings inputs after VanJS commits new DOM
  function populateSettingsInputs() {
    const inp = document.getElementById('menu-name-input');
    if (inp && myId >= 0) inp.value = players[myId].nm || '';
    const ssidInp = document.getElementById('wifi-ssid');
    const passInp = document.getElementById('wifi-pass');
    if (ssidInp) ssidInp.value = localStorage.getItem('wifi_ssid') || '';
    if (passInp) passInp.value = localStorage.getItem('wifi_pass') || '';
    populateSlider('k10-vol-slider', 'k10-vol-val', 'k10_audioVol',  5, '0 (mute)');
    populateSlider('k10-led-slider', 'k10-led-val', 'k10_ledBright', 5, '0 (off)');
  }

  van.derive(() => {
    if (uiMenuPage.val !== 'settings') return;
    setTimeout(populateSettingsInputs, 0);
  });

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
            ms({ class: 'ht-track-desc' }, 'R track. Gained entering rad-tagged terrain (failed Endure DN 6). R ≥ 7 = Dusk Check at day\'s end (Endure DN 8, fail = −1 LL). Spending a full day off rad terrain reduces R by 1 at dawn.')
          ),
        )
      ),

      sec('Actions',
        mp({ class: 'menu-text-body' },
          'Open the ☞ ACTION menu to choose an action. ' +
          'Actions cost MP and most require a skill check. Every action awards points (see Scoring).'
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
          'If you are well-fed (Food ≥ 4) and hydrated (Water ≥ 3), you recover 1 Life Level. ' +
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
            let vis;
            if (t.vis > 0) {
              vis = ms({ class: 'tr-vis-hi'  }, '\u25B2 HIGH');
            } else if (t.vis < 0) {
              vis = ms({ class: 'tr-vis-pen' }, '\u25BC MASK');
            } else {
              vis = ms({ class: 'vis-std' }, 'STD');
            }
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

      sec('Wounds',
        mp({ class: 'menu-text-body' },
          'Wounds reduce skill checks. Minor Wounds penalise Endure; Major Wounds penalise all skills. ' +
          'Grievous Wounds require a Settlement and a successful Treat to remove — and restore 1 LL when cleared.'
        )
      ),

      sec('Skill Checks',
        mp({ class: 'menu-text-body' },
          'Roll 2d6 + skill value + modifiers vs. the Difficulty Number (DN). ' +
          'Meeting or exceeding DN = success. ' +
          'Five skills: NAVIGATE · FORAGE · SCAVENGE · SHELTER · ENDURE.'
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
            mp({ class: 'ht-track-desc' }, 'All 6 slots: name, archetype, q/r position, survival tracks (ll/food/water/rad), wounds[3], skills[5], inv[5] quick totals + full invType/invQty grids, turn state (mp/actUsed/resting/radClean), chkSk/chkDn/chkBonus, score/steps. conn:false = empty slot.')
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
      ));

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
        mp({ class: 'settings-val' }, () => uiConn.val === 'Connected' ? (globalThis.location.hostname || 'unknown') : 'unknown')
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
                const v = Number.parseInt(e.target.value);
                const lbl = document.getElementById('k10-vol-val');
                if (lbl) lbl.textContent = v === 0 ? '0 (mute)' : String(v);
                localStorage.setItem('k10_audioVol', v);
                send({ t: 'settings', audioVol: v, ledBright: Number.parseInt(document.getElementById('k10-led-slider')?.value ?? '5') });
              }
            }),
            ms({ id: 'k10-vol-val', class: 'settings-slider-val' },
              (() => { const v = Number.parseInt(localStorage.getItem('k10_audioVol') ?? '5'); return v === 0 ? '0 (mute)' : String(v); })()
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
                const v = Number.parseInt(e.target.value);
                const lbl = document.getElementById('k10-led-val');
                if (lbl) lbl.textContent = v === 0 ? '0 (off)' : String(v);
                localStorage.setItem('k10_ledBright', v);
                send({ t: 'settings', audioVol: Number.parseInt(document.getElementById('k10-vol-slider')?.value ?? '5'), ledBright: v });
              }
            }),
            ms({ id: 'k10-led-val', class: 'settings-slider-val' },
              (() => { const v = Number.parseInt(localStorage.getItem('k10_ledBright') ?? '5'); return v === 0 ? '0 (off)' : String(v); })()
            )
          )
        ),
        md({ class: 'settings-row' },
          mp({ class: 'settings-label' }, 'K10 Screen Flip'),
          mb({
            class: 'menu-item-btn',
            style: () => `padding:3px 10px;font-size:var(--fs-d);border-color:${uiScreenFlip.val ? 'var(--gold)' : 'var(--bdr-mid)'}`,
            onclick: () => {
              uiScreenFlip.val = !uiScreenFlip.val;
              localStorage.setItem('k10_screenFlip', uiScreenFlip.val ? '1' : '0');
              send({ t: 'settings', audioVol: Number.parseInt(document.getElementById('k10-vol-slider')?.value ?? '5'), ledBright: Number.parseInt(document.getElementById('k10-led-slider')?.value ?? '5'), screenFlip: uiScreenFlip.val });
            }
          }, () => uiScreenFlip.val ? 'FLIPPED  ▣' : 'NORMAL  ▢')
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
            if (!ssid) return;
            localStorage.setItem('wifi_ssid', ssid);
            localStorage.setItem('wifi_pass', pass);
            send({ t: 'wifi', ssid, pass });
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
                showToast(`☠ ${a.name} is scrubbed from the wastes. Nothing remains.`);
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
  const { div, span, h2, p } = van.tags;
  const container = document.getElementById('char-select-content');
  if (!container) return;

  van.add(container, () => {
    const avail   = lobbyAvail.val;
    const pending = uiPickPending.val;
    const availSet = new Set(avail);
    console.log('[initCharSelect] reactive render — avail=%o', avail);

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
              pickTimeoutId = setTimeout(() => { // NOSONAR — declared in ui-state.js
                if (uiPickPending.val) {
                  uiPickPending.val = false;
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
