function getShelterDesc(shelterMaxed, shelterLevel, scrap, ap) {
  if (shelterMaxed) return 'Improved shelter already here — nothing to build';
  // Mirrors doShelter(): a basic shelter can only be upgraded (2 scrap, 2 AP);
  // an improved one is finished.  Never downgrade, never pay out twice.
  if (shelterLevel === 1 && scrap >= 2 && ap >= 2) return '2 scrap → improved shelter \u2302 (2 AP, +8 pts)';
  if (shelterLevel === 1) return 'Basic shelter here — upgrading needs 2 scrap and 2 AP';
  if (scrap === 0) return 'Needs scrap — none in pack';
  if (scrap === 1) return '1 scrap → shelter \u2302 (1 AP, +4 pts)';
  if (ap < 2) return '1 scrap → shelter \u2302 (1 AP, +4 pts) — not enough AP for improved';
  return '2 scrap → improved shelter \u2302 (2 AP, +8 pts)';
}

// Scrap needed for the shelter action on this hex (2 to upgrade a basic one).
function shelterScrapNeeded(shelterLevel) { return shelterLevel === 1 ? 2 : 1; }

function getBlockReason(def, shelterLevel, available, hasAP, ap, scrap, med, majorWounds, ll, llCap, terr) {
  const shelterMaxed = def.id === ACT_SHELTER && shelterLevel >= 2;
  const hasScrap = def.id !== ACT_SHELTER || scrap >= shelterScrapNeeded(shelterLevel);
  if (shelterMaxed) return 'Max shelter built here';
  if (def.id === ACT_SHELTER && shelterLevel === 1 && !hasScrap) return 'Shelter here — upgrading needs 2 scrap';
  // TREAT without a Major Wound is the Settlement heal: 1 Medicine → +1 LL.
  if (def.id === ACT_TREAT && !majorWounds) {
    if (terr !== null && terr !== 9) return 'Heal needs a Settlement (or a Major Wound to treat)';
    if (ll >= llCap)                 return 'Life Level is already full';
  }
  if (!available) {
    if (def.id === ACT_FORAGE) return 'Needs Forage terrain (Open Scrub · Rust Forest · Marsh · River Channel)';
    if (def.id === ACT_WATER)  return 'Needs Water terrain (Marsh \u00b7 Flooded District \u00b7 River Channel)';
    if (def.id === ACT_SCAV)   return 'Needs Salvage terrain (Broken Urban · Flooded District · Glass Fields)';
    if (def.id === ACT_TRADE)  return 'No survivors or caravan on this hex';
    if (def.id === ACT_TREAT)  return 'Only the Medic can treat outside a Settlement';
    if (def.id === ACT_CRAFT)  return 'Needs a Settlement hex';
    if (def.id === ACT_SHELTER && terr === 9) return 'A Settlement is already shelter';
    if (def.id === ACT_SHELTER && terr === 2) return 'The oasis canopy is shelter enough';
    return 'Not available here';
  }
  if (!hasAP)    return `Needs ${def.apCost} AP (have ${ap})`;
  if (!hasScrap) return `Needs scrap (have ${scrap})`;
  if (def.id === ACT_TREAT && med < 1) return 'Needs 1 Medicine';
  return '';
}

function initActionPanel() {
  const actionPanel     = document.getElementById('action-panel');
  const actionStatusBar = document.getElementById('action-status-bar');
  const actionTitle     = actionPanel.querySelector('.act-title-line');
  let terrName = '';  // name of the hex under your feet, refreshed by computeActionDefs()

  // The #action-panel dialog is now only the home of the multi-step
  // sub-controls (TRADE, CRAFT). The one-tap actions live on the deck under
  // the map (renderActionDeck, below).
  function openSubPanel(title, ctrlId, note) {
    const me = myId >= 0 ? players[myId] : null;
    if (actionTitle) actionTitle.textContent = title;
    const _terrSub = document.getElementById('act-panel-terrain-sub');
    if (_terrSub) _terrSub.textContent = terrName ? 'IN THE ' + terrName.toUpperCase() : '';
    actionStatusBar.innerHTML =
      `<span class="act-ap-badge">AP: ${me?.ap ?? 0}</span>` +
      `<span class="act-terrain-ctx">${escHtml(note ?? terrName)}</span>`;
    ['action-trade-ctrl', 'action-craft-ctrl', 'action-treat-ctrl', 'action-assist-ctrl'].forEach(id => {
      document.getElementById(id).style.display = (id === ctrlId) ? '' : 'none';
    });
    actionPanel.classList.add('open');
    actionPanel.setAttribute('aria-hidden', 'false');
  }

  // ── Trade sub-control ──────────────────────────────────────────
  let tradeTargetPid = -1;
  const tradeGive = [0, 0, 0, 0, 0];
  const tradeWant = [0, 0, 0, 0, 0];

  // opts.skip(i) drops a resource from the row entirely; opts.label(i)
  // overrides the RES_SHORT caption — both used by the caravan's PAY WITH
  // row (no water column, "FUL×2").
  function buildStepperRow(rowId, arr, maxFn, onChange, opts) {
    const el = document.getElementById(rowId);
    el.innerHTML = '';
    RES_SHORT.forEach((label, i) => {
      if (opts?.skip?.(i)) return;
      const wrap = document.createElement('div');
      wrap.className = 'trade-stepper';
      const lbl = document.createElement('span');
      lbl.className   = 'trade-stepper-label';
      lbl.textContent = opts?.label?.(i) ?? label;
      const minus = document.createElement('button');
      minus.textContent = '\u2212';
      const valSpan = document.createElement('span');
      valSpan.className   = 'trade-stepper-val';
      valSpan.textContent = arr[i];
      const plus = document.createElement('button');
      plus.textContent = '+';
      minus.addEventListener('click', (e) => {
        e.preventDefault();
        if (arr[i] > 0) { arr[i]--; valSpan.textContent = arr[i]; onChange?.(); }
      });
      plus.addEventListener('click', (e) => {
        e.preventDefault();
        const max = maxFn(i);
        if (arr[i] < max) { arr[i]++; valSpan.textContent = arr[i]; onChange?.(); }
      });
      wrap.append(lbl, minus, valSpan, plus);
      el.appendChild(wrap);
    });
  }

  function buildTradeOfferForm() {
    // Reset arrays
    tradeGive.fill(0);
    tradeWant.fill(0);
    // The caravan only sells: its shelf replaces the token steppers, which
    // are for survivor↔survivor offers alone (there is no car_trade swap).
    const isCaravan = tradeTargetPid === CARAVAN_PID;
    document.getElementById('trade-swap-sec').style.display = isCaravan ? 'none' : '';
    if (!isCaravan) {
      buildStepperRow('trade-give-row', tradeGive, i => (myId >= 0 ? (players[myId]?.inv?.[i] ?? 0) : 0));
      // Cap at what the target actually holds — their inv is already visible
      // client-side via the state broadcast, so there's no reason to let the
      // player build an offer that's guaranteed to fail on accept.
      buildStepperRow('trade-want-row', tradeWant, i => players[tradeTargetPid]?.inv?.[i] ?? 0);
    }
    buildStockList();
    document.getElementById('action-trade-offer-form').style.display = '';
  }

  // ── Caravan shelf (consumables for resource tokens) ──────────────
  // worldState.caravan.stock is [[itemId, qty, pricePerUnit], ...] straight
  // from the server (network-sync.hpp appendCaravanStock). The price is in
  // token-worth, not token count: the caravan values each token per
  // CARAVAN_TOKEN_WORTH (game-data.js — water 0 and refused, fuel 2, the
  // rest 1). The PAY WITH steppers cap at what's still owed, so the total
  // lands on the price exactly unless the last fuel token overshoots by one
  // (the caravan doesn't make change). One unit per BUY; the server
  // re-validates stock, payment and pack room (handleMsg_caravan_buy).
  let tradeBuyItem  = 0;
  let tradeBuyPrice = 0;
  const tradePay = [0, 0, 0, 0, 0];

  const myTokens          = i  => (myId >= 0 ? (players[myId]?.inv?.[i] ?? 0) : 0);
  const tokenWorth        = i  => CARAVAN_TOKEN_WORTH[i] ?? 0;
  const myPurchasingPower = () => [0, 1, 2, 3, 4].reduce((a, i) => a + myTokens(i) * tokenWorth(i), 0);
  const paidValue         = () => tradePay.reduce((a, n, i) => a + n * tokenWorth(i), 0);

  function buildStockList() {
    const sec  = document.getElementById('trade-stock-sec');
    const list = document.getElementById('trade-stock-list');
    list.innerHTML = '';
    document.getElementById('trade-buy-form').style.display = 'none';
    tradeBuyItem = 0; tradeBuyPrice = 0; tradePay.fill(0);
    if (tradeTargetPid !== CARAVAN_PID) { sec.style.display = 'none'; return; }
    sec.style.display = '';
    const stock = Array.isArray(worldState.caravan?.stock) ? worldState.caravan.stock : [];
    if (stock.length === 0) {
      list.innerHTML = '<div class="action-no-cond">The shelves are bare — the caravan restocks on the road</div>';
      return;
    }
    const have = myPurchasingPower();
    stock.forEach(([id, qty, price]) => {
      const item = getItemById(id);
      const affordable = have >= price;
      const btn = document.createElement('button');
      btn.className = 'chk-action-btn stock-card' + (affordable ? '' : ' action-disabled');
      btn.dataset.item = id;
      // postUse doubles as the shop blurb: it is the one client-side string
      // that spells out what a Gulpable actually does ("... +2 LL.").
      btn.innerHTML =
        `<img class="stock-icon item-icon-img" src="${escHtml(getItemIcon(id, 26))}" alt="" width="26" height="26" onerror="${_iconOnError(id)}">` +
        `<span class="stock-body">` +
          `<span class="act-label">${escHtml(item?.name ?? 'Item #' + id)} <span class="stock-qty">×${qty}</span></span>` +
          `<span class="act-desc">${escHtml(item?.postUse || item?.preUse || '')}</span>` +
          (affordable ? '' : `<span class="act-unavail-reason">Needs ${price} — your tokens are worth ${have} to the caravan</span>`) +
        `</span>` +
        `<span class="stock-price">${price} ${price === 1 ? 'token' : 'tokens'}</span>`;
      btn.addEventListener('click', () => { if (affordable) selectStockItem(id, price); });
      list.appendChild(btn);
    });
  }

  function selectStockItem(id, price) {
    tradeBuyItem = id; tradeBuyPrice = price;
    document.querySelectorAll('#trade-stock-list .stock-card').forEach(b =>
      b.classList.toggle('selected', Number(b.dataset.item) === id));
    // Pre-fill the payment from whatever the player holds most of, so the
    // common case is one tap; still fully adjustable on the steppers. Water
    // is skipped (worth 0). Whole tokens only, so fuel (worth 2) is spent
    // while it still fits under what's owed; if only fuel can cover the last
    // point, one more fuel goes in and the caravan keeps the change.
    tradePay.fill(0);
    let owed = price;
    const payable = [0, 1, 2, 3, 4].filter(i => tokenWorth(i) > 0)
      .sort((a, b) => myTokens(b) - myTokens(a));
    for (const i of payable) {
      if (owed <= 0) break;
      const take = Math.min(myTokens(i), Math.floor(owed / tokenWorth(i)));
      tradePay[i] = take; owed -= take * tokenWorth(i);
    }
    if (owed > 0) {
      const spare = payable.find(i => myTokens(i) > tradePay[i]);
      if (spare !== undefined) { tradePay[spare]++; owed -= tokenWorth(spare); }
    }
    // Each stepper caps at what's still owed (in worth), so the total can't
    // overshoot by more than one fuel token's rounding.
    buildStepperRow('trade-pay-row', tradePay,
      i => {
        const remaining = tradeBuyPrice - (paidValue() - tradePay[i] * tokenWorth(i));
        return remaining > 0 ? Math.min(myTokens(i), Math.ceil(remaining / tokenWorth(i))) : 0;
      },
      updatePayTotal,
      { skip:  i => tokenWorth(i) === 0,
        label: i => RES_SHORT[i] + (tokenWorth(i) > 1 ? '×' + tokenWorth(i) : '') });
    document.getElementById('trade-buy-form').style.display = '';
    updatePayTotal();
  }

  function updatePayTotal() {
    const paid = paidValue();
    const tot  = document.getElementById('trade-pay-total');
    tot.textContent = `${paid} / ${tradeBuyPrice}` + (paid > tradeBuyPrice ? ' (no change given)' : '');
    tot.classList.toggle('paid', paid >= tradeBuyPrice);
    const buy = document.getElementById('action-trade-buy');
    buy.disabled = !tradeBuyItem || paid < tradeBuyPrice;
    buy.textContent = `⇄ BUY ${getItemById(tradeBuyItem)?.name ?? ''}`.trim();
  }

  document.getElementById('action-trade-buy').addEventListener('click', () => {
    if (myId < 0 || tradeTargetPid !== CARAVAN_PID || !tradeBuyItem) return;
    if (paidValue() < tradeBuyPrice) return;
    console.log('%c[TRADE] Sending car_buy', 'color:#fc0;font-weight:bold', `item=${tradeBuyItem} give=${JSON.stringify(tradePay)}`);
    send({ t: 'car_buy', item: tradeBuyItem, n: 1, give: [...tradePay] });
    closeActionPanel();
  });

  function buildTradeTargetList() {
    const el = document.getElementById('action-trade-target-list');
    el.innerHTML = '';
    // Picking a target (or the caravan auto-open below) hides this list; a
    // rebuild is a fresh visit, so bring it back or the panel opens blank.
    el.style.display = '';
    document.getElementById('action-trade-offer-form').style.display = 'none';
    tradeTargetPid = -1;
    if (myId < 0) return;
    const me = players[myId];
    const colocated = players.filter(p => p.id !== myId && p.on && sharesMyHex(p));
    const caravanHere = caravanSharesMyHex();
    if (colocated.length === 0 && !caravanHere) {
      el.innerHTML = '<div class="action-no-cond">No survivors or caravan on this hex</div>';
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
    if (caravanHere) {
      const btn = document.createElement('button');
      btn.className   = 'chk-action-btn';
      btn.textContent = '⇄ Buy from the Caravan';
      btn.addEventListener('click', () => {
        tradeTargetPid = CARAVAN_PID;
        el.style.display = 'none';
        buildTradeOfferForm();
      });
      el.appendChild(btn);
    }
  }

  // ── Caravan auto-open ────────────────────────────────────────────
  // Stepping onto the caravan's hex (or having it roll onto yours) opens the
  // shelf straight away — the same panel ACT_TRADE reaches, minus the menu
  // and a one-button target list. Edge-triggered client-side off the 100 ms
  // state broadcast rather than the server's EVT_CARAVAN_TRADE, which only
  // fires on the world tick (up to ~15 s late — resolveCaravanProximity() in
  // world-system.hpp). Re-arms on leaving, so returning prompts again.
  let lastAutoTradeHex = null;   // 'q_r' of the caravan hex we last popped

  function caravanOnMyHex() {
    return myId >= 0 && caravanSharesMyHex();
  }

  function openCaravanTrade() {
    if (!caravanOnMyHex()) return;
    // Trade is free, so neither the AP-0 nor the resting gate applies.
    const terr = myBoardCell()?.terrain ?? null;
    terrName = cellTerrainName(myBoardCell());
    tradeTargetPid = CARAVAN_PID;
    document.getElementById('action-trade-target-list').style.display = 'none';
    buildTradeOfferForm();
    openSubPanel('⇄ CARAVAN', 'action-trade-ctrl', '⇄ Caravan · buying costs no AP');
  }

  function maybeAutoOpenCaravanTrade() {
    if (!caravanOnMyHex()) { lastAutoTradeHex = null; return; }
    const key = worldState.caravan.q + '_' + worldState.caravan.r;
    if (lastAutoTradeHex === key) return;
    const me = players[myId];
    // Never pop over something the player is already reading — an encounter,
    // the char sheet, an incoming trade offer. Those stay un-armed, so the
    // shelf appears as soon as the screen is free again (next state tick).
    if (me.ll === 0 || me.enc) return;
    const BLOCKING = '#enc-overlay.open, #char-overlay.open, #trade-overlay.open, ' +
                     '#item-action-menu.open, #help-overlay.open, ' +
                     '#menu-overlay.open, #char-select-overlay.open';
    if (document.querySelector(BLOCKING)) return;
    // Already in a sub-panel (craft, a player trade): don't yank them
    // into the shelf, they can reach it from the deck's TRADE cell, which the
    // state tick has just relit.
    lastAutoTradeHex = key;
    if (actionPanel.classList.contains('open')) return;
    openCaravanTrade();
  }
  // network.js calls this off every state/sync message (see _msgState).
  globalThis.maybeAutoOpenCaravanTrade = maybeAutoOpenCaravanTrade;
  globalThis.openCaravanTrade          = openCaravanTrade;

  // ── Craft sub-control ────────────────────────────────────────────
  // Total qty of itemId held across a player's typed-item slots (it[]/iq[]
  // are index-stable, 0 = empty — mirrors the server's invType[]/invQty[]).
  function invCountOf(p, itemId) {
    let n = 0;
    const it = p?.it, iq = p?.iq;
    if (!Array.isArray(it)) return 0;
    for (let s = 0; s < it.length; s++) if (it[s] === itemId) n += iq?.[s] ?? 0;
    return n;
  }

  // Effective pack size. `is` from the server is already effectiveInvSlots()
  // — base plus equipment — so this just clamps it to the grid width.
  function packSlots(p) { return packSlotsOf(p); }

  // Would the recipe's output have somewhere to land once its materials are
  // consumed? Mirrors applyRecipe()'s dry-run on a scratch copy of the pack,
  // minus stack caps (the client has no per-item stack sizes): an empty slot
  // after consumption, or an existing stack of the output, counts as room.
  // The server has the final say and toasts an err if the only candidate
  // stack turns out to be full.
  function outputHasRoom(p, r) {
    const it = [...(p?.it ?? [])], iq = [...(p?.iq ?? [])];
    for (let m = 0; m < 3; m++) {
      const matId = r.matItem[m];
      if (!matId) continue;
      let need = r.matQty[m];
      for (let s = 0; s < it.length && need > 0; s++) {
        if (it[s] !== matId) continue;
        const take = Math.min(need, iq[s] ?? 0);
        iq[s] = (iq[s] ?? 0) - take; need -= take;
        if (!iq[s]) it[s] = 0;
      }
    }
    const slots = packSlots(p);
    // A token recipe (Medicine) needs token room once its cost is paid, not
    // an item slot -- the same order applyRecipe() checks it in.
    if (r.outputRes) {
      const inv = (p?.inv ?? [0, 0, 0, 0, 0]).map((n, i) => (n || 0) - (r.resCost[i] || 0));
      inv[r.outputRes - 1] += r.outputQty;
      return tokenLoadOf(p, inv) <= slots;
    }
    for (let s = 0; s < slots; s++) {
      if (!it[s] || it[s] === r.outputItem) return true;
    }
    return false;
  }

  function buildCraftList() {
    const el = document.getElementById('action-craft-list');
    el.innerHTML = '';
    if (myId < 0) return;
    const me = players[myId];
    const known = RECIPES.filter(r => knowsRecipe(me.kr, r.id));
    if (known.length === 0) {
      el.innerHTML = '<div class="action-no-cond">You don’t know how to make anything yet — find out in the wasteland</div>';
      return;
    }
    known.forEach(r => {
      const missing = [];
      for (let m = 0; m < 3; m++) {
        const matId = r.matItem[m];
        if (!matId) continue;
        const have = invCountOf(me, matId);
        const need = r.matQty[m];
        if (have < need) missing.push(`${getItemById(matId)?.name ?? '?'} ${have}/${need}`);
      }
      for (let i = 0; i < 5; i++) {
        const need = r.resCost[i];
        if (!need) continue;
        const have = me.inv?.[i] ?? 0;
        if (have < need) missing.push(`${RES_SHORT[i]} ${have}/${need}`);
      }
      // The tokens have to be carried here, and the pack holds one per slot
      // (canteen water aside). A pack too small for a recipe's tokens says
      // so here rather than leaving the player to work out why.
      const carry = tokenLoadOf(me, r.resCost);
      if (carry > packSlots(me)) missing.push(`a bigger pack (${carry} tokens to carry)`);
      if (!outputHasRoom(me, r)) missing.push(r.outputRes ? `room in the pack for ${recipeOutputName(r)}` : 'a free pack slot');
      const affordable = missing.length === 0;
      const reqParts = [];
      for (let m = 0; m < 3; m++) {
        if (r.matItem[m]) reqParts.push(`${r.matQty[m]}× ${getItemById(r.matItem[m])?.name ?? '?'}`);
      }
      for (let i = 0; i < 5; i++) {
        if (r.resCost[i]) reqParts.push(`${r.resCost[i]} ${RES_SHORT[i]}`);
      }
      const btn = document.createElement('button');
      btn.className = 'chk-action-btn craft-card' + (affordable ? '' : ' action-disabled');
      btn.innerHTML =
        `<span class="act-label">${escHtml(r.name)}</span>` +
        `<span class="act-desc">${escHtml(reqParts.join(' + '))} → ${escHtml(recipeOutputName(r))}</span>` +
        (affordable ? '' : `<span class="act-unavail-reason">Needs ${escHtml(missing.join(', '))}</span>`);
      btn.addEventListener('click', () => {
        if (!affordable) return;
        send({ t: 'act', a: ACT_CRAFT, r: r.id });
        closeActionPanel();
      });
      el.appendChild(btn);
    });
  }

  document.getElementById('action-trade-send').addEventListener('click', () => {
    // Survivors only — the caravan is bought from (action-trade-buy), never swapped with.
    if (myId < 0 || tradeTargetPid < 0 || tradeTargetPid === CARAVAN_PID) return;
    const allZero = tradeGive.every(v => v === 0) && tradeWant.every(v => v === 0);
    if (allZero) return;
    console.log('%c[TRADE] Sending trade_offer', 'color:#fc0;font-weight:bold', `to=${tradeTargetPid} give=${JSON.stringify(tradeGive)} want=${JSON.stringify(tradeWant)}`);
    send({ t: 'trade_offer', to: tradeTargetPid, give: [...tradeGive], want: [...tradeWant] });
    closeActionPanel();
  });

  function closeActionPanel() {
    actionPanel.setAttribute('aria-hidden', 'true');
    actionPanel.classList.remove('open');
    if (actionTitle) actionTitle.textContent = '☞ ACTIONS';
    const _terrSub = document.getElementById('act-panel-terrain-sub');
    if (_terrSub) _terrSub.textContent = terrName ? 'IN THE ' + terrName.toUpperCase() : '';
    document.getElementById('action-trade-ctrl').style.display = 'none';
    document.getElementById('action-craft-ctrl').style.display = 'none';
    document.getElementById('action-treat-ctrl').style.display = 'none';
    document.getElementById('action-assist-ctrl').style.display = 'none';
  }

  // ── Team play (docs/team-play.md) ────────────────────────────────
  // What TREAT would do for survivor p, or null: their wounds pick the job,
  // exactly as doTreat() reads them. The treater's archetype and the hex
  // decide whether it is allowed; the treater pays. capOverride stands in
  // for llCap when the wire has not carried it yet (our own survivor).
  function treatJobFor(p, treaterIsMedic, terr, capOverride) {
    if (!p) return null;
    if ((p.wnd?.[WOUND_MAJOR] ?? 0) > 0)
      return (treaterIsMedic || terr === 9) ? { label: 'TREAT WOUND', ap: 2 } : null;
    const cap = p.llCap || capOverride || 0;
    return (terr === 9 && cap > 0 && (p.ll ?? 0) < cap) ? { label: 'HEAL', ap: 1 } : null;
  }

  // Survivors on this hex who could be treated right now: up, and not in a scene.
  function treatableHere() {
    return players.filter(p => p.id !== myId && p.on && p.ll > 0 && !p.enc && sharesMyHex(p));
  }

  // TREAT patient picker: shown when someone besides you could use it.
  function openTreatPicker() {
    const el = document.getElementById('action-treat-list');
    el.innerHTML = '';
    const me = myId >= 0 ? players[myId] : null;
    if (!me) return;
    const terr    = myBoardCell()?.terrain ?? null;
    const isMedic = me.arch === 2;
    const ap  = me.ap ?? 0, med = me.inv?.[3] ?? 0;
    const myCap = me.llCap || uiLLCap.val || 0;
    [me, ...treatableHere()].forEach(p => {
      const self = p.id === myId;
      const job  = treatJobFor(p, isMedic, terr, self ? myCap : 0);
      if (!job) return;
      const can  = ap >= job.ap && med >= 1;
      const name = self ? 'Yourself' : (p.nm || 'P' + p.id);
      const maj  = p.wnd?.[WOUND_MAJOR] ?? 0;
      const cap  = self ? myCap : (p.llCap || '?');
      const btn  = document.createElement('button');
      btn.className = 'chk-action-btn craft-card' + (can ? '' : ' action-disabled');
      btn.innerHTML =
        `<span class="act-label">${escHtml(job.label)} — ${escHtml(name)}</span>` +
        `<span class="act-desc">LL ${p.ll ?? 0}/${cap}` +
          (maj ? ` · ${maj} major wound${maj > 1 ? 's' : ''} · your Endure vs DN${TREAT_DN}` : ' · no roll') +
          ` · ${job.ap} AP + 1 Medicine</span>` +
        (can ? '' : `<span class="act-unavail-reason">${med < 1 ? 'Needs 1 Medicine' : `Needs ${job.ap} AP (have ${ap})`}</span>`);
      btn.addEventListener('click', () => {
        if (!can) return;
        send(self ? { t: 'act', a: ACT_TREAT } : { t: 'act', a: ACT_TREAT, to: p.id });
        closeActionPanel();
      });
      el.appendChild(btn);
    });
    openSubPanel('✚ TREAT', 'action-treat-ctrl', 'Your AP and Medicine · their wounds');
  }

  // ASSIST picker: back the next roll of anyone on this hex who is in a
  // scene. One token each (enc_assist), no AP; the server spends the help on
  // that roll and credits the helpers if it comes off.
  function openAssistPicker() {
    const el = document.getElementById('action-assist-list');
    el.innerHTML = '';
    const me = myId >= 0 ? players[myId] : null;
    if (!me) return;
    const scenes = players.filter(p => p.id !== myId && p.on && p.enc && sharesMyHex(p));
    if (!scenes.length)
      el.innerHTML = '<div class="action-no-cond">Nobody on this hex is in a scene</div>';
    scenes.forEach(p => {
      const n    = p.encAs | 0;
      const mine = ((p.encAsBy | 0) >> myId) & 1;
      const head = document.createElement('div');
      head.className   = 'check-lbl';
      head.style.marginTop = '10px';
      head.textContent = `STEADY ${(p.nm || 'P' + p.id).toUpperCase()} — ${n}/${ENC_ASSIST_CAP} BEHIND THE NEXT ROLL`;
      el.appendChild(head);
      const full = mine ? 'You are already behind this roll'
                 : n >= ENC_ASSIST_CAP ? `${ENC_ASSIST_CAP} allies are already behind this roll` : '';
      if (full) {
        const d = document.createElement('div');
        d.className = 'action-no-cond'; d.textContent = full;
        el.appendChild(d);
        return;
      }
      const row = document.createElement('div');
      row.className = 'trade-res-row';
      RES_SHORT.forEach((lbl, i) => {
        const have = me.inv?.[i] ?? 0;
        const b = document.createElement('button');
        b.className   = 'chk-action-btn assist-res' + (have > 0 ? '' : ' action-disabled');
        b.textContent = `${lbl} ×${have}`;
        b.title       = have > 0 ? `Give 1 ${lbl}: their next roll DN −${ENC_ASSIST_DN}` : `No ${lbl} to give`;
        b.addEventListener('click', () => {
          if (have <= 0) return;
          send({ t: 'enc_assist', to: p.id, res: i });
          closeActionPanel();
        });
        row.appendChild(b);
      });
      el.appendChild(row);
    });
    openSubPanel('◉ ASSIST', 'action-assist-ctrl', `One token · their next roll DN −${ENC_ASSIST_DN} · free`);
  }

  // ── Action deck ──────────────────────────────────────────────────
  // The on-screen buttons under the map. Every action is one tap away and
  // every cell keeps its place: a cell that can't fire right now is dimmed,
  // not hidden, and tapping it toasts the reason, so the thumb learns
  // positions. Icon-only faces — the title / aria-label is the label.
  const actionDeck    = document.getElementById('action-deck');

  const DECK_ICON = {
    [ACT_FORAGE]: 'forage', [ACT_WATER]: 'water', [ACT_SCAV]: 'scav', [ACT_SHELTER]: 'shelter',
    [ACT_CRAFT]: 'craft', [ACT_TRADE]: 'trade', [ACT_TREAT]: 'treat', [ACT_SURVEY]: 'survey',
    [ACT_REST]: 'rest', [ACT_ASSIST]: 'assist',
  };

  // Everything the deck needs to light itself, from the current state.
  // Returns [{id, key, icon, label, apCost, desc, canAct, blockReason, cls}]
  // in deck order: the six core actions, then TREAT / SURVEY when they
  // apply, then REST. (SHEET is appended by renderActionDeck — it is a
  // utility, not an action.)
  function computeActionDefs() {
    const me   = myId >= 0 ? players[myId] : null;
    // The cell under your feet on the board you are on -- underground that
    // is the corridor (tq/tr), not the surface hatch me.q/r is pinned to.
    // Reading the hatch greyed out WATER and SCAVENGE, the two things the
    // tunnels are for, and lit SHELTER/SURVEY that the server refuses below.
    const cell = me ? myBoardCell() : null;
    const terr         = cell?.terrain ?? null;
    const ap           = me?.ap  ?? 0;
    const scrap        = me?.inv?.[4] ?? 0;
    const shelterLevel = cell?.shelter ?? 0;
    const isScout      = (me?.arch ?? -1) === 4;  // Scout: Survey is free
    const isMedic      = (me?.arch ?? -1) === 2;  // Medic: may TREAT anywhere
    const med          = me?.inv?.[3] ?? 0;
    const majorWounds  = me?.wnd?.[WOUND_MAJOR] ?? 0;
    const ll           = me?.ll ?? 0;
    const llCap        = me?.llCap || uiLLCap.val || 0;   // wire llCap is the effective cap

    terrName = cellTerrainName(cell);
    const oasis = isOasisCell(cell);   // a forest oasis: no shelter to build, and a free night

    // Whole-deck gates. TRADE is free and stays open when exhausted (a
    // co-located survivor or the caravan can still deal), nothing else does.
    let gate = '';
    if (!me)                    gate = 'No survivor claimed';
    else if (me.ll === 0)       gate = 'Downed — cannot act';
    else if (me.enc)            gate = 'In an encounter';
    else if (uiResting.val)     gate = 'Resting — waiting for dawn';
    else if (ap === 0)          gate = 'Exhausted — REST to recover AP';
    const gateExemptsTrade = !!me && me.ll > 0 && !me.enc && !uiResting.val && ap === 0;

    // Shelter cost mirrors doShelter(): improved when affordable, basic otherwise;
    // an existing basic shelter can only be upgraded (2 scrap, 2 AP).
    const shelterApCost = (shelterLevel === 1 || (scrap >= 2 && ap >= 2)) ? 2 : 1;
    const shelterLabel  = shelterLevel >= 1 ? 'UPGRADE SHELTER' : 'BUILD SHELTER';

    const defs = [
      { id: ACT_FORAGE,  label: 'FORAGE',        apCost: 2,             desc: 'Search for food (Skill check)' },
      { id: ACT_WATER,   label: 'COLLECT WATER', apCost: 1,             desc: 'Gather 1 water token (1 AP)' },
      { id: ACT_SCAV,    label: 'SCAVENGE',      apCost: 2,             desc: 'Search for items (Skill check)' },
      { id: ACT_SHELTER, label: shelterLabel,    apCost: shelterApCost, desc: 'Construct shelter — needs scrap (1–2 AP, no roll)' },
      { id: ACT_CRAFT,   label: 'CRAFT',         apCost: 1,             desc: 'Craft a known recipe — Settlement only' },
      { id: ACT_TRADE,   label: 'TRADE',         apCost: 0,             desc: 'Swap resources with a survivor, or buy from the caravan — free' },
    ];
    // TREAT is offered when there is a Major Wound to treat, or in a
    // Settlement where it becomes the heal: 1 Medicine + 1 AP → +1 LL, no roll.
    // Mirrors doTreat(): a wound always takes precedence over the heal.
    if (majorWounds > 0) {
      defs.push({ id: ACT_TREAT, label: 'TREAT WOUND', apCost: 2,
                  desc: `Close a Major Wound — Endure DN${TREAT_DN}, costs 1 Medicine` });
    } else if (terr === null || terr === 9) {
      defs.push({ id: ACT_TREAT, label: 'HEAL', apCost: 1,
                  desc: 'Settlement care — 1 Medicine restores 1 Life Level, no roll' });
    }
    // TREAT for a teammate on this hex (docs/team-play.md). When anyone else
    // here could use it, the cell becomes a patient picker -- its cost is the
    // cheapest job on offer, and the picker gates each patient on its own.
    const allyPatients = me ? treatableHere().filter(p => treatJobFor(p, isMedic, terr, 0)) : [];
    if (allyPatients.length) {
      const jobs = [treatJobFor(me, isMedic, terr, llCap), ...allyPatients.map(p => treatJobFor(p, isMedic, terr, 0))]
        .filter(Boolean);
      const teamDef = { id: ACT_TREAT, label: 'TREAT', apCost: Math.min(...jobs.map(j => j.ap)),
                        desc: 'Patch up yourself or a survivor on your hex — your AP, your Medicine',
                        teamTreat: true, fire: openTreatPicker };
      const at = defs.findIndex(d => d.id === ACT_TREAT);
      if (at >= 0) defs[at] = teamDef; else defs.push(teamDef);
    }
    // Scout-exclusive: SURVEY is hidden for non-Scouts
    if (isScout) {
      defs.push({ id: ACT_SURVEY, label: 'SURVEY', apCost: 0, desc: 'Reveal terrain beyond vision — free for Scout' });
    }

    // ASSIST: offered only while someone on this hex is in a scene. Free,
    // like TRADE -- it costs a token, not a day's work.
    if (me && players.some(p => p.id !== myId && p.on && p.enc && sharesMyHex(p))) {
      defs.push({ id: ACT_ASSIST, label: 'ASSIST', apCost: 0,
                  desc: `Spend one token to steady a teammate's next encounter roll — DN −${ENC_ASSIST_DN} each, ${ENC_ASSIST_CAP} allies at most`,
                  fire: openAssistPicker });
    }

    // TRADE availability: requires another connected player, or the caravan, on the same hex
    const tradeAvail = !!me && (players.some(p => p.id !== myId && p.on && sharesMyHex(p)) ||
      caravanSharesMyHex());
    // CRAFT: Settlement only, same terrain check TREAT uses for non-Medics — which
    // recipes are actually affordable is decided per-card inside the craft sub-panel.
    const craftAvail = terr === null || terr === 9;

    defs.forEach(def => {
      def.key  = def.id;
      def.icon = DECK_ICON[def.id];
      // Shelter unavailable if improved shelter already built here
      const shelterMaxed = def.id === ACT_SHELTER && shelterLevel >= 2;
      // If cell hasn't loaded yet (null — race between 'asgn' and 'sync' messages),
      // allow terrain-dependent actions optimistically; the server validates.
      const terrAvail  = terr === null || (!shelterMaxed && actAvailable(def.id, terr)
                                           && !(def.id === ACT_SHELTER && oasis));   // doShelter() refuses a grove
      // TREAT: the Medic closes a Major Wound anywhere; everyone else needs a
      // Settlement (9), and so does the no-wound heal for every archetype.
      const treatAvail = terr === null || terr === 9 || (isMedic && majorWounds > 0);
      const available  = def.id === ACT_TRADE ? tradeAvail
                       : def.id === ACT_ASSIST ? true
                       : def.teamTreat ? true
                       : def.id === ACT_TREAT ? treatAvail
                       : def.id === ACT_CRAFT ? craftAvail
                       :                        terrAvail;
      const hasAP      = ap >= def.apCost;
      const hasScrap   = def.id !== ACT_SHELTER || scrap >= shelterScrapNeeded(shelterLevel);
      const hasMed     = def.id !== ACT_TREAT || med >= 1;
      const hasWound   = def.id !== ACT_TREAT || def.teamTreat || majorWounds > 0 || ll < llCap;
      const hasTokens  = def.id !== ACT_ASSIST || (me?.inv ?? []).some(v => v > 0);
      let canAct       = available && hasAP && hasScrap && hasMed && hasWound && hasTokens;

      // Dynamic desc: BUILD/UPGRADE SHELTER shows actual cost
      if (def.id === ACT_SHELTER) def.desc = getShelterDesc(shelterMaxed, shelterLevel, scrap, ap);

      let blockReason = def.teamTreat        ? (med < 1 ? 'Needs 1 Medicine' : `Needs ${def.apCost} AP (have ${ap})`)
                      : def.id === ACT_ASSIST ? 'No tokens to give'
                      : getBlockReason(def, shelterLevel, available, hasAP, ap, scrap, med, majorWounds, ll, llCap, terr);
      // A Scout's SURVEY costs nothing (doSurvey() charges only the others),
      // so an empty AP budget does not close it either.
      const freeAct = def.id === ACT_TRADE || def.id === ACT_ASSIST ||
                      (def.id === ACT_SURVEY && isScout);
      if (gate && !(freeAct && gateExemptsTrade)) { canAct = false; blockReason = gate; }
      def.canAct = canAct;
      def.blockReason = canAct ? '' : blockReason;
    });

    // REST: the day's other verb. Same gates the FAB handler enforces.
    const restBlocked = !me ? 'No survivor claimed'
                      : me.ll === 0 ? 'Downed — cannot act'
                      : me.enc ? 'In an encounter'
                      : (uiResting.val || restSent) ? 'Already resting — waiting for dawn'
                      : '';
    defs.push({ id: ACT_REST, key: ACT_REST, icon: 'rest', label: 'REST', apCost: 0, cls: 'rest', domId: 'fab-rest-btn',
                desc: oasis ? 'Sleep safe in the oasis \u2014 heals at dawn with no food or water needed, nothing can reach you'
                            : 'Sleep until dawn — recover AP, heal a little',
                canAct: !restBlocked, blockReason: restBlocked });
    return defs;
  }

  function fireAction(def) {
    if (myId < 0) return;
    // WATER is one tap like FORAGE: 1 AP -> 1 water, tap again for more.
    if (def.id === ACT_WATER) { send({ t: 'act', a: ACT_WATER, ap: 1 }); return; }
    if (def.id === ACT_TRADE) {
      buildTradeTargetList();
      openSubPanel('⇄ TRADE', 'action-trade-ctrl');
      return;
    }
    if (def.id === ACT_CRAFT) {
      buildCraftList();
      openSubPanel('⚒ CRAFT', 'action-craft-ctrl');
      return;
    }
    if (def.id === ACT_REST) {
      if (uiResting.val || restSent) return;
      restSent = true; // NOSONAR — declared in network.js
      updateRestIndicator();
      send({ t: 'act', a: ACT_REST });
      return;
    }
    send({ t: 'act', a: def.id });
  }

  const deckCells = new Map();   // def.key -> <button>
  function deckCell(def) {
    let btn = deckCells.get(def.key);
    if (btn) return btn;
    btn = document.createElement('button');
    btn.type      = 'button';
    btn.id        = def.domId || ('action-btn-' + def.id);   // stable IDs for AI agents / tests
    btn.className = 'act-btn' + (def.cls ? ' ' + def.cls : '');
    btn.innerHTML = `<svg aria-hidden="true"><use href="#ai-${def.icon}"></use></svg><span class="act-cost-tag"></span>`;
    btn._def = def;
    btn.addEventListener('pointerdown',  e => { if (e.pointerType !== 'mouse' || e.button === 0) btn.classList.add('pressed'); });
    btn.addEventListener('pointerup',    () => btn.classList.remove('pressed'));
    btn.addEventListener('pointercancel',() => btn.classList.remove('pressed'));
    btn.addEventListener('pointerleave', () => btn.classList.remove('pressed'));
    btn.addEventListener('contextmenu',  e => e.preventDefault());   // long-press on phones
    btn.addEventListener('click', () => {
      const d = btn._def;
      if (!d.canAct) { showToast(`${d.label} — ${d.blockReason}`); return; }
      if (d.fire) d.fire(); else fireAction(d);
    });
    deckCells.set(def.key, btn);
    return btn;
  }

  function renderActionDeck() {
    if (!actionDeck) return;
    const defs = computeActionDefs();
    // SHEET rides the deck as a utility cell, set off from the actions by a rule.
    defs.push({ key: 'sheet', icon: 'sheet', label: 'SHEET', apCost: 0, cls: 'util', domId: 'fab-char-btn',
                desc: 'Survivor record — vitals, gear, pack', canAct: true, blockReason: '', fire: openCharSheet });
    const wanted = new Set(defs.map(d => d.key));
    for (const [key, btn] of deckCells) {
      if (!wanted.has(key)) { btn.remove(); deckCells.delete(key); }
    }
    let sep = actionDeck.querySelector('.act-sep');
    if (!sep) { sep = document.createElement('span'); sep.className = 'act-sep'; sep.setAttribute('aria-hidden', 'true'); }
    // Relight every cell, then reconcile the DOM order without touching a
    // node that is already where it belongs. This runs on every 100 ms state
    // broadcast, and the old appendChild-everything pass detached and
    // re-inserted all nine cells ten times a second. Chromium drops a click
    // whose mousedown target left the document before mouseup, so any press
    // that spanned a broadcast simply vanished -- in Brave/Chrome the deck
    // (SHEET included) only worked between ticks, while Firefox retargets
    // and never showed it. Seen on the K10 2026-09-28.
    const order = [];
    defs.forEach(def => {
      const btn = deckCell(def);
      btn._def = def;
      btn.classList.toggle('act-disabled', !def.canAct);
      const title = def.label + (def.blockReason ? ' — ' + def.blockReason : '');
      if (btn.title !== title) btn.title = title;
      const aria = def.label + (def.apCost > 0 ? ` (${def.apCost} AP)` : '') + (def.blockReason ? ' — ' + def.blockReason : '');
      if (btn.getAttribute('aria-label') !== aria) btn.setAttribute('aria-label', aria);
      const dis = def.canAct ? 'false' : 'true';
      if (btn.getAttribute('aria-disabled') !== dis) btn.setAttribute('aria-disabled', dis);
      const tag = btn.querySelector('.act-cost-tag');
      const cost = def.apCost > 0 ? String(def.apCost) : '';
      if (tag.textContent !== cost) tag.textContent = cost;
      if (def.key === 'sheet') order.push(sep);
      order.push(btn);
    });
    let cursor = actionDeck.firstChild;
    for (const node of order) {
      if (cursor === node) { cursor = cursor.nextSibling; continue; }
      actionDeck.insertBefore(node, cursor);   // only a cell out of place moves
    }
  }

  actionPanel.addEventListener('click', e => { if (e.target === actionPanel) closeActionPanel(); });
  document.getElementById('action-close').addEventListener('click', closeActionPanel);
  document.getElementById('action-close').addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); closeActionPanel(); } });

  renderActionDeck();
  van.derive(() => {
    // Read the states so the derive tracks them, then relight the deck.
    const ap = uiAP.val, resting = uiResting.val;
    renderActionDeck();
    const btn = document.getElementById('fab-rest-btn');
    if (!btn) return;
    btn.classList.toggle('rest-btn-used', resting);
    // Pulse when exhausted (out of AP) and not yet resting — nudge player to rest
    btn.classList.toggle('rest-exhausted', ap <= 0 && !resting);
  });
  // Bunker tunnels have no control of their own: stepping onto a Bunker
  // Entrance / Vent Shaft crosses boards by itself (tunnelStepDown/Up in
  // tunnels.hpp), so the D-pad is the only thing that moves you.
  globalThis.renderActionDeck = renderActionDeck;
  globalThis.openActionPanel  = renderActionDeck;   // legacy alias: callers only ever wanted a refresh
}

function initTradeOverlay() {
  const overlay = document.getElementById('trade-overlay');
  let pendingTradeFrom = -1;
  let expireInterval   = null;
  let expireEnd        = 0;

  globalThis._openTradeOffer = function(fromPid, give, want) {
    console.log('%c[TRADE] Incoming trade offer', 'color:#fc0;font-weight:bold', `from=${fromPid} give=${JSON.stringify(give)} want=${JSON.stringify(want)}`);
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
    console.log('[TRADE] closeTradeOverlay — from=%d', pendingTradeFrom);
    pendingTradeFrom = -1;
    clearInterval(expireInterval);
    overlay.classList.remove('open');
    overlay.style.display = 'none';
  }
  globalThis._closeTradeOverlay = _closeTradeOverlay;

  document.getElementById('trade-accept-btn').addEventListener('click', () => {
    if (pendingTradeFrom < 0) return;
    console.log('%c[TRADE] Accepting trade from %d', 'color:#fc0;font-weight:bold', pendingTradeFrom);
    send({ t: 'trade_accept', from: pendingTradeFrom });
    _closeTradeOverlay();
  });
  document.getElementById('trade-decline-btn').addEventListener('click', () => {
    if (pendingTradeFrom < 0) return;
    console.log('[TRADE] Declining trade from %d', pendingTradeFrom);
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
  const { div: md, span: ms, button: mb, h2: mh2, h3: mh3, p: mp, input: minput, br: mbr, img: mimg } = van.tags;

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
    populateSlider('k10-mus-slider', 'k10-mus-val', 'k10_musicVol',  6, '0 (off)');
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
      mb({ class: 'menu-item-btn', onclick: () => openMenu('admired')  }, '\u2620  THE ADMIRED'),
      mb({ class: 'menu-item-btn', onclick: () => openMenu('settings') }, '\u25C9  SETTINGS'),
      mb({ class: 'menu-item-btn', onclick: () => { closeMenu(); openCharSheet(); } }, '\u25C8  SURVIVOR'),
      mb({ class: 'menu-item-btn', onclick: () => openMenu('about')    }, '\u25A3  ABOUT'),
      mb({ class: 'menu-item-btn', onclick: () => { closeMenu(); const ov = document.getElementById('help-overlay'); ov.classList.add('open'); ov.removeAttribute('aria-hidden'); } }, '\u2139  AGENT HELP'),
      mb({ class: 'menu-resume-btn', onclick: closeMenu }, '\u25B6 RESUME'),
    );

    // ── The Admired ───────────────────────────────────────────────
    // The score ladder (ADMIRED in game-data.js) with the live party merged
    // into it by admiredBoard(), so the party reads as rows among the dead
    // rather than as a separate table. The living carry no citation — out
    // here the write-up is what you get instead of a future.
    if (page === 'admired') {
      // Snapshot, deliberately NOT a reactive read. uiPlayers/uiScore are
      // reassigned on every state broadcast, and touching them inside this
      // van.add derive would re-render the whole menu page — scroll position
      // and slide-in animation included — every few seconds while someone is
      // part way down the board. `players`/`myId` are the same numbers,
      // unwatched; the board is whatever was true when you opened it.
      const live  = players
        .map((p, i) => ({ p, i }))
        .filter(({ p }) => p.on)
        .map(({ p, i }) => ({ nm: p.nm || `P${i}`, sc: p.sc | 0, color: PLAYER_COLORS[i], isMe: i === myId }));
      const mine  = live.find(p => p.isMe) ?? null;
      const mySc  = mine ? mine.sc : 0;
      const next  = admiredNextAbove(mySc);
      const rows  = admiredBoard(live);
      const admRow = (r) => md({ class: `adm-row${r.live ? ' live' : ''}${r.isMe ? ' me' : ''}` },
        ms({ class: 'adm-rank' }, `#${r.rank}`),
        ms({ class: 'adm-sc' }, String(r.sc)),
        r.live ? md({ class: 'adm-dot', style: `--pc:${r.color}` }) : md({ class: 'adm-dot-sp' }),
        md({ class: 'adm-txt' },
          ms({ class: 'adm-nm' }, r.nm),
          ms({ class: 'adm-ln' }, r.live
            ? (r.isMe ? 'still walking. nothing written up yet.' : 'still walking. no citation.')
            : r.ln),
        ),
      );
      return wrap(
        back('main'),
        mh2({ class: 'menu-sub-title' }, '☠ THE ADMIRED'),
        mp({ class: 'adm-intro' },
          `${ADMIRED_WIN} points and you are out of the wasteland. the names below are ` +
          'what it kept instead. not one of them finished, and every one of them is admired.'
        ),
        next
          ? md({ class: 'adm-next' },
              ms({ class: 'adm-next-lbl' }, 'NEXT ABOVE YOU'),
              ms({ class: 'adm-next-nm'  }, next.nm),
              ms({ class: 'adm-next-gap' }, `${next.sc - mySc} pts`),
            )
          : md({ class: 'adm-next' },
              ms({ class: 'adm-next-lbl' }, 'NOTHING ABOVE YOU'),
              ms({ class: 'adm-next-nm'  }, 'THE WAY OUT'),
              ms({ class: 'adm-next-gap' }, 'walk'),
            ),
        md({ class: 'adm-board' }, ...rows.map(admRow)),
        mp({ class: 'adm-foot' },
          'anyone below the last name is still alive and therefore not admired.'
        ),
      );
    }

    // ── How to play ───────────────────────────────────────────────
    // The player's guide, in the house voice: short, bitter, a little damp.
    // Mechanics live in the game's own toasts and the encounter screen — this
    // page only has to get a new survivor on the air, on the TV and moving.
    // Numbers quoted here are the firmware's (survival_state.hpp dawn upkeep,
    // TERRAIN mc for move costs); keep them honest if those move.
    if (page === 'howto') {
      const em  = (t) => ms({ class: 'ht-em' }, t);
      const url = (t) => ms({ class: 'ht-url' }, t);
      const act = (name, cost, desc) => md({ class: 'ht-act-row' },
        ms({ class: 'ht-act-name' }, name),
        ms({ class: 'ht-act-cost' }, cost),
        ms({ class: 'ht-act-desc' }, desc)
      );
      const here = location.host || '192.168.47.1';   // the TV wants whichever network you are on
      return wrap(
        back('main'),
        mh2({ class: 'menu-sub-title' }, 'HOW TO PLAY'),

        mp({ class: 'ht-lead' },
          'The bombs fell, the sky curdled, and the survivors are mostly you. Up to five of you share ' +
          'one wasteland that wraps around at the edges like a bad smell. Walk it, eat what you find, ' +
          'drink what doesn’t fight back, and reach ', em(`${ADMIRED_WIN.toLocaleString('en-US')} points`),
          ' — that’s the ticket out. Nobody has used it.'
        ),

        sec('Getting On The Air',
          mp({ class: 'menu-text-body' },
            'The K10 is the whole operation: the world, the radio and the referee. It always throws up ' +
            'its own open hotspot, ', em('WASTELAND'), ' — no password, no internet, no help. Join it, ' +
            'then open ', url('http://192.168.47.1'), '. Plain http. If your phone whines about https, ' +
            'let it whine.'
          ),
          mp({ class: 'menu-text-body' },
            'That works anywhere — a car, a tent, a field full of strangers. The catch: one tiny antenna is ' +
            'hosting every phone and running the apocalypse at the same time. It has room for about eight ' +
            'devices (the five of you, a TV, a straggler) and it ', em('will'), ' stutter. That isn’t lag. ' +
            'That’s the board chewing.'
          ),
          mp({ class: 'menu-text-body' },
            'For a steadier world, feed the board your house Wi-Fi: ', em('☰ → SETTINGS → WIFI NETWORK'), '. ' +
            'It remembers up to eight networks and rejoins them by itself next time. Then everyone hops on ' +
            'that network and browses to the ', em('LAN'), ' address printed along the bottom of the K10’s ' +
            'main screen. WASTELAND stays up regardless, for whoever shows up late.'
          ),
          () => !uiApLink.val ? ms() :
            mp({ class: 'menu-text-hint' }, uiStaIp.val
              ? ['You’re on WASTELAND right now. The board also answers at ', url(uiStaIp.val),
                 ' — switch over and browse there.']
              : 'You’re on WASTELAND right now, and the board hasn’t been given anything better yet.')
        ),

        sec('The TV — Wasteland Observer',
          mp({ class: 'menu-text-body' },
            'Got a TV, laptop or dusty tablet with a browser? Put it on the same network and open ',
            url(`http://${here}/observer.html`), '. That’s the broadcast: a camera that hunts down ' +
            'whoever is in the deepest trouble, narrates their worst decisions in headline type, and keeps ',
            em('THE ADMIRED'), ' scoreboard — who’s ahead, who’s dead, and who’s next.'
          ),
          mp({ class: 'menu-text-body' },
            'It only watches. It takes no seat and moves no pawn, so leave it running and let the room ' +
            'heckle. The K10’s own screen keeps score too — press ', em('B'), ' to flip through its pages.'
          )
        ),

        sec('Walking',
          mp({ class: 'menu-text-body' },
            'Each hex costs ', em('1–4 Action Points'), ' — open scrub is cheap, marsh and mountain eat your ' +
            'legs. You get a fresh stack at dawn; wounds and a heavy pack shrink it. Every new hex you set ' +
            'foot on is a point, and the map wraps, so you can’t walk off the edge. Lord knows people try.'
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
              mp({ class: 'menu-text-hint' }, 'Arrow keys and numpad work too.')
            ),
            md({},
              mp({ class: 'ctrl-label' }, 'TOUCH'),
              mp({ class: 'menu-text-body' },
                'Swipe the map the way you want to go, or use the pad under it.'
              )
            )
          )
        ),

        sec('Staying Alive',
          mp({ class: 'menu-text-body' },
            'A day lasts five minutes. At dawn your body collects: ', em('1 food and 2 water'), ' straight ' +
            'out of your pack. Can’t pay, and hunger and thirst start taking Life instead. Life hits zero ' +
            'and you’re a smear.'
          ),
          mp({ class: 'menu-text-body' },
            'Sleep under something — a shelter, a Settlement, a mountainside. A night in the open takes ' +
            'Life too. It never takes your last point; it just leaves you there, shivering, for something ' +
            'else to finish.'
          ),
          mp({ class: 'menu-text-body' },
            '▼ REST ends your day early. Rest fed and watered and you heal — in a Settlement you always ' +
            'heal. Once everyone is resting, dawn comes early.'
          ),
          mp({ class: 'menu-text-body' },
            'Glowing ground soaks radiation into your bones; walk off it and stay off it. Hatches lead ' +
            'down into bunker tunnels: no weather down there, but the air is old and it wants you.'
          )
        ),

        sec('Things To Do',
          mp({ class: 'menu-text-body' },
            'The action deck shows what this hex allows. Most of it is a roll — two dice plus your ' +
            'skill against a target. Your skills are on ◈ SHEET.'
          ),
          md({ class: 'ht-act-list' },
            act('⚗ FORAGE',        '2 AP',   'Food, on green-ish ground. If it stops wriggling, it’s lunch.'),
            act('≈ COLLECT WATER', '1 AP',   'One token a tap, on wet ground. Don’t look at it too closely.'),
            act('⚲ SCAVENGE',      '2 AP',   'Elbow-deep in salvage for scrap and better junk.'),
            act('⌂ SHELTER',        '1–2 AP', 'Scrap becomes a roof. Everyone under it sleeps better.'),
            act('✚ TREAT / HEAL',  '1–2 AP', 'Medicine on wounds. Anyone can in a Settlement; the Medic can anywhere.'),
            act('⚒ CRAFT',         '1 AP',   'Settlement workbenches turn scrap into gear. Learn recipes in ruins.'),
            act('⇄ TRADE',         'free',   'With anyone on your hex, or the caravan when it rolls by.'),
          )
        ),

        sec('Ruins',
          mp({ class: 'menu-text-body' },
            'Some hexes hide a place worth robbing. Step onto one and a scene opens: pick a way in, roll, ' +
            'and grab what the room has. ', em('TAKE HAUL & LEAVE'), ' pockets it. Pushing deeper is how ' +
            'you get rich, or how you get found in three separate places.'
          ),
          mp({ class: 'menu-text-hint' },
            'Some rooms won’t let you carry anything out. Some are rigged. You’ll find out which.'
          )
        ),

        sec('Together',
          mp({ class: 'menu-text-body' },
            'Share a hex and you can ⇄ TRADE, ✚ TREAT each other, and ◉ ASSIST — slip a teammate one token ' +
            'mid-scene and their next roll gets easier. Three of you on one hex when an improved shelter ' +
            'goes up founds a ', em('Settlement'), ': +20 points to every founder.'
          ),
          mp({ class: 'menu-text-hint' },
            'But a crowd stinks. Every huddle wakes the Creeping Doom a little more, wherever it is lurking.'
          )
        ),

        sec('Dying & The Way Out',
          mp({ class: 'menu-text-body' },
            'You will die. Your pack drops where you fall and your tokens rot into Remains for whoever finds ' +
            'them — quite possibly you, on your next survivor.'
          ),
          mp({ class: 'menu-text-body' },
            `Points come from new ground, food, salvage, shelters, ruins and Settlements. ` +
            `${ADMIRED_WIN.toLocaleString('en-US')} of them and you walk out. ☠ THE ADMIRED — in this menu, ` +
            'on the K10 and on the TV — lists the dead you’re climbing past. Not one of them made it. All ' +
            'of them are admired.'
          )
        ),

        mp({ class: 'menu-text-hint' },
          'Bot, script or AI? ℹ AGENT HELP in the menu has the wiring.'
        ),
      );
    }

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

      md({ class: 'settings-row' },
        mp({ class: 'settings-label' }, 'Diagnostics'),
        mb({
          class: 'menu-item-btn',
          style: 'padding:3px 10px;font-size:var(--fs-d);',
          onclick: () => openMenu('assets')
        }, '◈  VIEW ASSETS')
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
        // The K10's generative score and ambience (snd-music.hpp), under the
        // volume above: 0 leaves only effects and the voice. The firmware keeps
        // it in NVS, so this slider only has to send when it moves.
        md({ class: 'settings-row' },
          mp({ class: 'settings-label' }, 'K10 Music'),
          md({ class: 'settings-slider-row' },
            minput({
              id: 'k10-mus-slider', type: 'range', min: '0', max: '9', step: '1',
              value: localStorage.getItem('k10_musicVol') ?? '6',
              class: 'settings-slider',
              oninput: e => {
                const v = Number.parseInt(e.target.value);
                const lbl = document.getElementById('k10-mus-val');
                if (lbl) lbl.textContent = v === 0 ? '0 (off)' : String(v);
                localStorage.setItem('k10_musicVol', v);
                send({ t: 'settings', musicVol: v });
              }
            }),
            ms({ id: 'k10-mus-val', class: 'settings-slider-val' },
              (() => { const v = Number.parseInt(localStorage.getItem('k10_musicVol') ?? '6'); return v === 0 ? '0 (off)' : String(v); })()
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
        // Standing notice while the player is on the board's own AP. The banner
        // at connect is easy to miss and gone in six seconds; this is the screen
        // they are on when they can actually do something about it.
        () => !uiApLink.val ? md({ style: 'display:none' }) :
          md({ class: 'wifi-uplink-warn' },
            mp({ class: 'wifi-uplink-title' }, '⚠ DIRECT UPLINK — DEGRADED'),
            mp({ class: 'settings-val' },
              'You are on WASTELAND, the board’s own radio. One antenna is beaconing, '
              + 'handing out addresses, sweeping for a network to join, and running the game. '
              + 'It seats ' + (uiApCap.val > 0 ? uiApCap.val + ' survivors' : 'very few') + '.'),
            mp({ class: 'settings-val' }, uiStaIp.val
              ? 'The board already answers at ' + uiStaIp.val + ' — rejoin that network yourself '
                + 'and browse there. The wasteland gets noticeably steadier.'
              : 'Give the board a network below. Then rejoin that network yourself and browse to the '
                + 'address it reports — the wasteland gets noticeably steadier.')
          ),
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
        }, '\u25B6 CONNECT TO NETWORK'),
        // Every network the board has joined before. It rejoins whichever one
        // is in range on its own, so a board carried to a friend's place only
        // ever needs its password typed once.
        md({ class: 'wifi-known' },
          mp({ class: 'settings-label' }, 'Remembered networks'),
          () => {
            const nets = uiWifiNets.val;
            if (!nets.length) {
              return mp({ class: 'settings-val' },
                'None yet \u2014 connect once and this board will rejoin that network anywhere.');
            }
            return md({ class: 'wifi-known-list' },
              ...nets.map(ssid => md({ class: 'wifi-known-row' },
                ms({ class: () => 'wifi-known-ssid' + (ssid === uiWifiCur.val ? ' wifi-known-cur' : '') },
                  () => (ssid === uiWifiCur.val ? '\u25C9 ' : '\u25CB ') + ssid),
                mb({
                  class: 'wifi-forget-btn', title: 'Forget this network',
                  onclick: () => {
                    if (!confirm(`Forget "${ssid}"?\nThe board will stop rejoining it automatically.`)) return;
                    send({ t: 'wifi_forget', ssid });
                  }
                }, '\u2715')
              ))
            );
          }
        )
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

    if (page === 'assets') {
      const assetProgress = van.state({ done: 0, total: 0 });
      const assetResults  = van.state(null);

      setTimeout(() => {
        const entries = [];

        // Survivor portraits + pawns
        for (const a of ARCHETYPES) {
          const n = a.name.toLowerCase();
          entries.push({ group: 'Survivor', label: a.name + ' Portrait', path: `img/survivors/${n}.jpg` });
          entries.push({ group: 'Survivor', label: a.name + ' Pawn',    path: `img/survivors/${n}Pawn.jpg` });
        }

        // Title screen
        entries.push({ group: 'UI', label: 'Title Screen', path: 'img/wastelandTitle0.png' });

        // Sprite sheets: one per terrain, per shelter kind, and the forage set (scripts/hex_sheets.py)
        for (const img of tileAtlas.pages) {
          const p = new URL(img.dataset?.src || img.src, window.location.href).pathname;
          const label = p.split('/').pop();
          const group = label.startsWith('shelter') ? 'Shelter' : label.startsWith('forrage') ? 'Forage' : 'Terrain';
          entries.push({ group, label, path: p });
        }

        // Terrain variants (per-file fallback, populated after server sync)
        for (const variants of terrainImgVariants) {
          for (const img of variants) {
            const p = new URL(img.dataset?.src || img.src, window.location.href).pathname;
            entries.push({ group: 'Terrain', label: p.split('/').pop(), path: p });
          }
        }

        // Forage animal images
        for (const img of forrageAnimalImgs) {
          const p = new URL(img.src, window.location.href).pathname;
          entries.push({ group: 'Forage', label: p.split('/').pop(), path: p });
        }

        // Shelter variants
        for (const variants of shelterImgs) {
          for (const img of (variants || [])) {
            const p = new URL(img.dataset?.src || img.src, window.location.href).pathname;
            entries.push({ group: 'Shelter', label: p.split('/').pop(), path: p });
          }
        }

        // No item rows: badges are drawn by item-icons.js, and the
        // item_<id>.png illustrations in ITEMS were never made and nothing
        // loads them, so listing them only reported every item as MISSING.

        assetProgress.val = { done: 0, total: entries.length };
        let completedCount = 0;

        // Through the AssetLoader queue (bounded concurrency) — a bare
        // Promise.all(fetch) here opened ~100 sockets against the K10 at once.
        const get = window.AssetLoader
          ? (p) => AssetLoader.fetch(p, { cache: 'no-store', attempts: 2 })
          : (p) => fetch(p, { cache: 'no-store' });
        Promise.all(entries.map(async entry => {
          const t0 = performance.now();
          let ok = false, size = 0;
          try {
            const resp = await get(entry.path);
            if (resp.ok) { const blob = await resp.blob(); ok = true; size = blob.size; }
          } catch (_) {}
          const elapsed = Math.round(performance.now() - t0);
          completedCount++;
          assetProgress.val = { done: completedCount, total: entries.length };
          return { ...entry, ok, size, elapsed };
        })).then(results => { assetResults.val = results; });
      }, 0);

      return wrap(
        back('settings'),
        mh2({ class: 'menu-sub-title' }, 'ASSET VIEWER'),
        () => {
          const prog    = assetProgress.val;
          const results = assetResults.val;

          if (prog.total === 0) return ms({ class: 'asset-scanning' }, 'Preparing scan...');

          if (!results) return md({ class: 'asset-scanning' },
            mp({ class: 'asset-prog-label' }, `Downloading ${prog.done} / ${prog.total} assets...`),
            md({ class: 'asset-prog-bar' },
              md({ class: 'asset-prog-fill', style: `width:${(prog.done / prog.total * 100).toFixed(0)}%` })
            )
          );

          const totalKB  = (results.reduce((s, r) => s + r.size, 0) / 1024).toFixed(1);
          const missing  = results.filter(r => !r.ok).length;
          const slowest  = results.reduce((m, r) => r.elapsed > m ? r.elapsed : m, 0);

          return md({ class: 'asset-viewer' },
            md({ class: 'asset-summary' },
              ms({}, `${results.length} assets  ·  ${totalKB} KB`),
              missing > 0 ? ms({ class: 'asset-miss-badge' }, ` · ${missing} MISSING`) : ms(),
              ms({ class: 'asset-slow' }, `  ·  slowest ${slowest} ms`)
            ),
            md({ class: 'asset-table' },
              md({ class: 'asset-table-head' },
                ms({ class: 'atc atc-grp'  }, 'Group'),
                ms({ class: 'atc atc-lbl'  }, 'Asset'),
                ms({ class: 'atc atc-prev' }, ''),
                ms({ class: 'atc atc-sz'   }, 'Size'),
                ms({ class: 'atc atc-ms'   }, 'Time'),
              ),
              ...results.map(r =>
                md({ class: `asset-row${r.ok ? '' : ' asset-row-miss'}` },
                  ms({ class: 'atc atc-grp'  }, r.group),
                  ms({ class: 'atc atc-lbl'  }, r.label),
                  ms({ class: 'atc atc-prev' }, r.ok ? mimg({ src: r.path, class: 'asset-thumb' }) : ''),
                  ms({ class: 'atc atc-sz'   }, r.ok ? (r.size / 1024).toFixed(1) + ' KB' : '—'),
                  ms({ class: 'atc atc-ms'   }, r.ok ? r.elapsed + ' ms' : 'MISSING'),
                )
              )
            )
          );
        }
      );
    }

    if (page === 'about') return wrap(
      back('main'),
      md({ class: 'about-header' },
        mp({ class: 'about-logo' }, '\u2620 WASTELAND CRAWL'),
        mp({ class: 'about-sub'  }, 'Post-Apocalyptic Hex Crawl', mbr(), '5-player co-op scavenging on ESP32-S3'),
        mp({ class: 'about-sub'  },
          'Explore a 25\u00D719 toroidal wasteland.', mbr(),
          'Scavenge water, food, fuel, medicine and scrap.', mbr(),
          'Survive.'
        ),
        mp({ class: 'about-ver' }, '11 terrain types \u00B7 5 resource types \u00B7 6 archetypes \u00B7 5 players'),
        mp({ class: 'about-ver' }, 'WebSocket \u00B7 AP mode \u00B7 SSID: WASTELAND \u00B7 192.168.47.1'),
      )
    );

    return ms(); // unreachable fallback
  });
}

// \u2500\u2500 Character Selection Screen \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
// A roster and a file. Six chips across the top -- a claimed survivor stays in
// the roster, stamped, instead of vanishing -- and under them the file of the
// one on show: portrait, trait, skills, and the pack they walk out with, drawn
// token by token against its slots. Nothing is sent until the CLAIM bar, which
// names who it claims; a tap used to pick whichever card it landed on.
// Swipe the file (touch) or use the arrow keys to page through the roster.
//
// The only reactive reads are lobbyAvail, uiPickPending and csShown. "Claimed
// by" names come from the plain `players` global, so a state broadcast never
// re-renders the page under the reader (the menu pages' van.js trap).
const csShown = van.state(-1);    // archetype on show; -1 = the first free one
let _csDir = 0;                   // +1 / -1: the side the next file slides in from
let _csClaimArch = -1, _csClaimAt = 0;   // last pick this client sent, and when
let _csSwipe = null;
let _csPreloaded = false;

const _CS_SK_WORD  = ['untrained', 'trained', 'expert'];
// Icons are drawn at the size they're shown -- keep in step with style.css
// "Character Selection" (.sv-slot img, .sv-bp img, .sv-bp-cost img).
const _CS_TOKEN_PX = 28;
const _CS_BP_PX    = 20;
const _CS_COST_PX  = 14;

function _csPortrait(arch) { return `img/survivors/${arch.name.toLowerCase()}.jpg`; }

function _csShownIdx() {
  const s = csShown.val;
  return s >= 0 && s < ARCHETYPES.length ? s : (lobbyAvail.val[0] ?? 0);
}

function _csShow(i, dir = 0) {
  const n = ARCHETYPES.length;
  _csDir = dir;
  csShown.val = ((i % n) + n) % n;
}

// Who holds a claimed seat, if this client has heard of them.
function _csHolder(i) {
  const p = typeof players !== 'undefined' ? players[i] : null;
  return p?.on && p.nm ? p.nm : '';
}

function _csClaim(i) {
  if (uiPickPending.val || !lobbyAvail.val.includes(i)) return;
  // If a survivor is already active, require confirmation before abandoning them
  if (myId >= 0 && players[myId]?.on) {
    if (!confirm('\u26A0 Abandon your current survivor?\nAll progress, score and position will be lost.')) return;
  }
  _csClaimArch = i;
  _csClaimAt = performance.now();
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

// Horizontal swipe on the file pages the roster. Touch/pen only; a vertical
// drag is the browser's (touch-action:pan-y) and arrives here as a cancel.
function _csSwipeStart(e) {
  if (e.pointerType === 'mouse') return;
  _csSwipe = { x: e.clientX, y: e.clientY, t: performance.now() };
}
function _csSwipeEnd(e) {
  const s = _csSwipe;
  _csSwipe = null;
  if (!s) return;
  const dx = e.clientX - s.x, dy = e.clientY - s.y;
  if (Math.abs(dx) < 48 || Math.abs(dx) < Math.abs(dy) * 1.6 || performance.now() - s.t > 700) return;
  const d = dx < 0 ? 1 : -1;
  _csShow(_csShownIdx() + d, d);
}

function initCharSelect() {
  const { div, span, h2, h3, p, img, button } = van.tags;
  const container = document.getElementById('char-select-content');
  if (!container) return;

  // A token badge, or its colour swatch on a page without item-icons.js.
  const token = (res, px) => {
    const src = getResIcon(res, px);
    return src
      ? img({ class: 'item-icon-img', src, alt: '', width: px, height: px, draggable: false })
      : span({ class: `dot ${RES_DOT_CLASSES[res - 1]}` });
  };

  const header = () => {
    const free = lobbyAvail.val.length;
    return p({ class: 'cs-sel-sub' }, free
      ? `${free} of ${ARCHETYPES.length} survivors unclaimed.`
      : 'Every survivor is out there. Wait for a seat to come free.');
  };

  const roster = () => {
    const availSet = new Set(lobbyAvail.val);
    const sel = _csShownIdx();
    return div({ class: 'sv-roster', role: 'tablist', 'aria-label': 'Survivors' },
      ...ARCHETYPES.map((arch, i) => {
        const taken = !availSet.has(i);
        return button({
          class: `sv-chip${i === sel ? ' sel' : ''}${taken ? ' taken' : ''}`,
          style: `--ring:${ARCHETYPE_COLORS[i]}`,
          role: 'tab',
          'aria-selected': String(i === sel),
          'aria-label': arch.name + (taken ? ', claimed' : ''),
          onclick: () => _csShow(i, Math.sign(i - sel)),
        },
          span({ class: 'sv-chip-face' },
            img({ src: `img/survivors/${arch.name.toLowerCase()}Pawn.jpg`, alt: '', draggable: false })),
          span({ class: 'sv-chip-name' }, arch.name)
        );
      })
    );
  };

  const file = () => {
    const i     = _csShownIdx();
    const arch  = ARCHETYPES[i];
    const taken = !lobbyAvail.val.includes(i);
    const kit   = archKit(arch);
    const bonus = arch.kitBonus ?? [];
    const slots = arch.invSlots;
    const load  = kit.reduce((a, b) => a + b, 0);
    const dir   = _csDir;
    _csDir = 0;

    // One tile per token, inv[] order; the role's extras are the last of
    // their kind and carry the + mark.
    const tiles = [];
    kit.forEach((n, r) => {
      for (let j = 0; j < n; j++) {
        const extra = j >= n - (bonus[r] || 0);
        tiles.push(div({ class: `sv-slot full${extra ? ' bonus' : ''}`,
                         title: RES_NAMES[r + 1] + (extra ? ' \u2014 role bonus' : '') },
          token(r + 1, _CS_TOKEN_PX)));
      }
    });
    while (tiles.length < slots) tiles.push(div({ class: 'sv-slot' }));
    const kitWords = kit.map((n, r) => (n ? `${n} ${RES_NAMES[r + 1]}` : '')).filter(Boolean).join(', ');
    const extras   = bonus.map((n, r) => (n ? `+${n} ${RES_NAMES[r + 1]}` : '')).filter(Boolean);

    // Starter recipes: the same for everyone, but they say the pack can grow.
    // Then this survivor's own trade (recipes.cfg known_by -- the Medic's).
    const bpChip = (rc) => {
      const item = rc.outputRes ? null : getItemById(rc.outputItem);
      const name = rc.outputRes ? rc.name : (item?.name ?? rc.name);
      const mods = rc.outputRes ? `makes ${recipeOutputName(rc)}`
                 : typeof _formatMods === 'function' ? _formatMods(getItemMods?.(rc.outputItem)) : '';
      const mats = rc.matItem.map((id, m) => (id ? `${rc.matQty[m]} ${getItemById(id)?.name ?? '?'}` : '')).filter(Boolean);
      const cost = [...mats, ...rc.resCost.map((n, r) => (n ? `${n} ${RES_NAMES[r + 1]}` : ''))].filter(Boolean).join(' + ');
      return span({ class: 'sv-bp', title: `${name}${mods ? ' (' + mods + ')' : ''} \u2014 costs ${cost}` },
        rc.outputRes ? token(rc.outputRes, _CS_BP_PX)
          : img({ class: 'item-icon-img', src: getItemIcon(rc.outputItem, _CS_BP_PX), alt: '',
                  width: _CS_BP_PX, height: _CS_BP_PX, draggable: false }),
        span({ class: 'sv-bp-name' }, name),
        ...rc.matItem.map((id, m) => (id ? span({ class: 'sv-bp-cost' }, `${rc.matQty[m]}\u00D7`,
          img({ class: 'item-icon-img', src: getItemIcon(id, _CS_COST_PX), alt: '',
                width: _CS_COST_PX, height: _CS_COST_PX, draggable: false })) : null)),
        ...rc.resCost.map((n, r) => (n ? span({ class: 'sv-bp-cost' }, `${n}\u00D7`, token(r + 1, _CS_COST_PX)) : null))
      );
    };
    const blueprints = RECIPES.filter(rc => rc.starter).map(bpChip);
    const ownBps     = RECIPES.filter(rc => rc.knownBy?.includes(i)).map(bpChip);

    return div({
      class: `sv-file${taken ? ' taken' : ''}${dir > 0 ? ' in-r' : dir < 0 ? ' in-l' : ''}`,
      style: `--ring:${ARCHETYPE_COLORS[i]}`,
      role: 'tabpanel',
      'aria-label': `${arch.name} survivor file`,
      onpointerdown: _csSwipeStart,
      onpointerup: _csSwipeEnd,
      onpointercancel: () => { _csSwipe = null; },
    },
      div({ class: 'sv-head' },
        div({ class: 'sv-portrait' },
          img({
            src: _csPortrait(arch), alt: '', draggable: false,
            // The first portrait in pulls the other five, so paging is instant.
            onload: () => {
              if (_csPreloaded) return;
              _csPreloaded = true;
              for (const a of ARCHETYPES) new Image().src = _csPortrait(a);
            },
          }),
          taken ? span({ class: 'sv-stamp' }, 'CLAIMED') : null
        ),
        div({ class: 'sv-id' },
          span({ class: 'sv-file-no' }, `SURVIVOR FILE ${String(i + 1).padStart(2, '0')}`),
          h3({ class: `sv-name${arch.name.length > 9 ? ' long' : ''}` }, arch.name),
          p({ class: 'sv-flavor' }, arch.flavor)
        )
      ),
      div({ class: 'sv-trait' },
        span({ class: 'sv-lbl' }, 'TRAIT'),
        span({ class: 'sv-trait-txt' }, arch.trait)),
      div({ class: 'sv-sec' },
        div({ class: 'sv-sec-head' },
          span({ class: 'sv-lbl' }, 'STARTING PACK'),
          span({ class: 'sv-aside' }, `${load} / ${slots} slots`)),
        div({ class: 'sv-pack', role: 'img', 'aria-label': `${kitWords}; ${load} of ${slots} slots filled` }, ...tiles),
        extras.length
          ? p({ class: 'sv-note' }, span({ class: 'sv-plus' }, '+'), ` role bonus: ${extras.join(', ')}`)
          : null,
        p({ class: 'sv-note' }, span({ class: 'sv-plus' }, '+'),
          ` ${START_PATCHES}× ${getItemById(START_PATCH_ITEM)?.name ?? 'Trauma Patch'}, ` +
          `${archRandomItems(i)} random items`)
      ),
      div({ class: 'sv-sec' },
        div({ class: 'sv-sec-head' },
          span({ class: 'sv-lbl' }, 'SKILLS'),
          span({ class: 'sv-aside' },
            span({ class: 'sv-key' }, span({ class: 'sv-pip on' })), 'trained ',
            span({ class: 'sv-key' }, span({ class: 'sv-pip on' }), span({ class: 'sv-pip on' })), 'expert')),
        div({ class: 'sv-skills' },
          ...SK_NAMES.map((sk, si) => {
            const lvl = arch.skills[si] | 0;
            return div({ class: `sv-skill lvl-${lvl}`, title: `${sk}: ${_CS_SK_WORD[lvl]}`,
                         'aria-label': `${sk}: ${_CS_SK_WORD[lvl]}` },
              span({ class: 'sv-sk-name' }, sk),
              span({ class: 'sv-pips' },
                span({ class: 'sv-pip' + (lvl >= 1 ? ' on' : '') }),
                span({ class: 'sv-pip' + (lvl >= 2 ? ' on' : '') })));
          }))
      ),
      p({ class: 'sv-desc' }, arch.desc),
      blueprints.length
        ? div({ class: 'sv-sec' },
            div({ class: 'sv-sec-head' },
              span({ class: 'sv-lbl' }, 'BLUEPRINTS'),
              span({ class: 'sv-aside' }, 'every survivor')),
            div({ class: 'sv-bps' }, ...blueprints))
        : null,
      ownBps.length
        ? div({ class: 'sv-sec' },
            div({ class: 'sv-sec-head' },
              span({ class: 'sv-lbl' }, 'TRADE SECRETS'),
              span({ class: 'sv-aside' }, `${arch.name} only`)),
            div({ class: 'sv-bps' }, ...ownBps))
        : null
    );
  };

  const bar = () => {
    const i       = _csShownIdx();
    const name    = ARCHETYPES[i].name;
    const avail   = lobbyAvail.val;
    const pending = uiPickPending.val;
    const taken   = !avail.includes(i);
    let main, sub;
    if (pending) {
      main = `CLAIMING THE ${name}\u2026`;
      sub  = 'waiting on the K10';
    } else if (taken) {
      const who    = _csHolder(i);
      const beaten = _csClaimArch === i && performance.now() - _csClaimAt < 15000;
      main = `THE ${name} IS CLAIMED`;
      sub  = beaten ? 'someone got there first \u2014 pick another'
           : who    ? `claimed by ${who}`
           : avail.length ? 'pick another survivor' : 'wait for a seat to come free';
    } else {
      main = `\u25B6 CLAIM THE ${name}`;
      sub  = 'final \u2014 no switching later';
    }
    return div({ class: 'sv-bar' },
      button({
        class: `sv-claim${pending ? ' pending' : ''}`,
        style: `--ring:${ARCHETYPE_COLORS[i]}`,
        disabled: pending || taken,
        onclick: () => _csClaim(i),
      },
        span({ class: 'sv-claim-main' }, main),
        span({ class: 'sv-claim-sub' }, sub))
    );
  };

  van.add(container, div({ class: 'cs-page' },
    h2({ class: 'cs-sel-title' }, '\u2620 CHOOSE YOUR SURVIVOR'),
    header,
    roster,
    file,
    bar
  ));

  document.addEventListener('keydown', e => {
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    if (!document.getElementById('char-select-overlay')?.classList.contains('open')) return;
    if (e.target?.closest?.('input, textarea, select')) return;
    e.preventDefault();
    const d = e.key === 'ArrowRight' ? 1 : -1;
    _csShow(_csShownIdx() + d, d);
  });
}
