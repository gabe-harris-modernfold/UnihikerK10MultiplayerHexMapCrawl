// ── Encounter overlay + ally banner ──────────────────────────────
function resolveText(text, placeholders) {
  if (!placeholders) return text;
  return text.replaceAll(/\{\{(\w+)\}\}/g, (_, key) => {
    const opts = placeholders[key];
    return (Array.isArray(opts) && opts.length) ? opts[Math.floor(Math.random() * opts.length)] : key;
  });
}

function initEncounterOverlay() {
  const overlay    = document.getElementById('enc-overlay');
  const nodeText   = document.getElementById('enc-node-text');

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

  function renderLoot() {
    const parts = pendingLoot.map((v, i) => v > 0 ? `${RES_NAMES_ENC[i]}×${v}` : null).filter(Boolean);
    lootDisp.textContent = parts.length ? `Pending: ${parts.join('  ')}` : '';
  }


  function renderNode(node) {
    console.log('%c[ENC] renderNode', 'color:#c0f', `keys=[${Object.keys(node).join(',')}] choices=${node.choices?.length ?? 0} can_bank=${node.can_bank ?? false}`);
    currentNode = node;
    nodeText.textContent = resolveText(node.text, currentEnc.placeholders);

    choiceList.innerHTML = '';
    (node.choices ?? []).forEach(ch => {
      const btn = document.createElement('button');
      btn.className = 'enc-choice-btn';
      btn.innerHTML =
        `<span>${escHtml(resolveText(ch.label, currentEnc.placeholders))}</span>`;
      btn.addEventListener('click', () => sendChoice(ch));
      choiceList.appendChild(btn);
    });

    canBank = node.can_bank ?? false;
    bankRow.style.display = canBank ? '' : 'none';
    renderLoot();
  }

  function sendChoice(ch) {
    console.log('%c[ENC] sendChoice', 'color:#c0f', `label="${ch.label}" success_node="${ch.success_node ?? ''}" base_risk=${ch.base_risk ?? 50} hazard=${ch.hazard_id ?? 'none'}`);
    const haz    = (ch.hazard_id && currentEnc.hazards) ? (currentEnc.hazards[ch.hazard_id] ?? {}) : {};
    pendingHazText = haz.text ? resolveText(haz.text, currentEnc.placeholders) : '';
    const hazPen = haz.penalty ?? {};

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
      cost_rad:    cost.radiation ?? 0,
      cost_food:   cost.food      ?? 0,
      cost_water:  cost.water     ?? 0,
      haz_ll:      hazPen.ll        ?? 0,
      haz_rad:     hazPen.radiation ?? 0,
      haz_st:      haz.status       ?? 0,
      haz_wt:      0,
      haz_wc:      0,
      haz_ends:    haz.ends_encounter ? 1 : 0,
      is_terminal: nextKey === '' ? 1 : 0,
    });
    pendingNextKey = nextKey;
    choiceList.innerHTML = '';
  }

  function openEncounter(enc) {
    console.log('%c[ENC] openEncounter', 'color:#c0f;font-weight:bold', `id=${enc.id} start_node=${enc.start_node} nodeCount=${Object.keys(enc.nodes ?? {}).length}`);
    currentEnc  = enc;
    pendingLoot = [0,0,0,0,0];
    overlay.classList.add('open');
    overlay.style.display = '';
    const startNode = enc.nodes?.[enc.start_node] ?? Object.values(enc.nodes ?? {})[0];
    if (startNode) renderNode(startNode);
    else console.error('[ENC] No start node found — encounter dialog will be empty', enc);
  }

  function closeEncounter() {
    console.log('%c[ENC] closeEncounter', 'color:#c0f', `id=${currentEnc?.id ?? 'none'} pendingLoot=${JSON.stringify(pendingLoot)}`);
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

  // Called from network.js enc_path handler
  globalThis._startEncounterFetch = function(biome, id) {
    const url = `/enc?biome=${encodeURIComponent(biome)}&id=${encodeURIComponent(id)}`;
    const t0  = Date.now();
    console.log('%c[ENC] fetch start', 'color:#c0f', `GET ${url}`);
    fetch(url)
      .then(r => {
        console.log('[ENC] fetch response', `HTTP ${r.status} t+${Date.now()-t0}ms`);
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        return r.text();
      })
      .then(txt => {
        console.log('[ENC] fetch body', `${txt.length} bytes t+${Date.now()-t0}ms`);
        let enc;
        try { enc = JSON.parse(txt); }
        catch(error_) {
          console.error('[ENC] JSON parse error:', error_, 'body=', txt.slice(0, 200));
          throw new Error(`JSON parse: ${error_.message}`);
        }
        console.log('[ENC] parsed ok', `nodes=${Object.keys(enc.nodes ?? {}).length} start_node=${enc.start_node} t+${Date.now()-t0}ms`);
        openEncounter(enc);
      })
      .catch(e => {
        console.error('[ENC] Fetch failed — sending enc_abort:', e.message, `t+${Date.now()-t0}ms`);
        send({ t: 'enc_abort' });
      });
  };

  const SUCCESS_PHRASES = [
    'You push through.', 'Barely.', 'Fortune holds.', 'Against the odds.',
    'Clean exit.', 'You manage.', 'Just in time.', 'Through.'
  ];

  // Called from engine.js enc_res handler — show outcome then advance or stay
  globalThis._onEncResult = function(ev) {
    console.log('%c[ENC] _onEncResult', 'color:#c0f', `out=${ev.out} ends=${ev.ends} penLL=${ev.penLL ?? 0} penRad=${ev.penRad ?? 0} loot=${JSON.stringify(ev.loot)}`);
    if (ev.ends) { closeEncounter(); return; }

    const nextKey = pendingNextKey;
    pendingNextKey = '';

    // Build delta line
    const deltaItems = [];
    if (ev.out && Array.isArray(ev.loot)) {
      ev.loot.forEach((v, i) => { if (v > 0) deltaItems.push({ txt: `+${v} ${RES_NAMES_ENC[i]}`, pos: true }); });
    } else {
      if (ev.penLL  < 0) deltaItems.push({ txt: `${ev.penLL} Life`,         pos: false });
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
          const contBtn = document.createElement('button');
          contBtn.className = 'chk-action-btn';
          contBtn.textContent = 'Continue \u2192';
          contBtn.addEventListener('click', () => {
            contBtn.remove();
            // Guard: encounter may have been closed (abort/bank/ev.ends) while button was visible
            if (currentEnc?.nodes?.[nextKey]) renderNode(currentEnc.nodes[nextKey]);
          }, { once: true });
          choiceList.appendChild(contBtn);
        } else {
          renderLoot();
          choiceList.innerHTML = '';
          canBank = true;
          bankRow.style.display = '';
        }
      // Guard: closeEncounter() may have fired during the 2200ms outcome window
      } else if (currentNode) {
        renderNode(currentNode);
      }
    }, 4000);
  };

  globalThis._onEncBank = function() { closeEncounter(); };
  globalThis._onEncEnd  = function() { closeEncounter(); };

  bankBtn.addEventListener('click', () => {
    send({ t: 'enc_bank' });
    closeEncounter();
  });

  abortBtn.addEventListener('click', () => {
    send({ t: 'enc_abort' });
    closeEncounter();
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
