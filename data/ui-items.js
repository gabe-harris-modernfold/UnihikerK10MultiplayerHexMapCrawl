// ── Item System UI ────────────────────────────────────────────────

let _lastEqKey = '';  // moved here from ui-hud.js; used by renderEquipment + char-overlay derive
function resetLastEqKey() { _lastEqKey = ''; }

const CAT_NAMES = ['Gulpable', 'Bolt-On', 'Salvage', 'Relic'];
const CAT_CLASSES = ['cat-consumable', 'cat-equipment', 'cat-material', 'cat-key'];

// Badge src for an item, drawn at the size the <img> shows it (item-icons.js).
function _itemIcon(id, px) {
  return getItemIcon?.(id, px) || ITEM_ICON_PLACEHOLDER;
}

// Inline onerror for item <img>s: swap to the category fallback icon once.
// `this.onerror=null` stops a loop if the fallback itself is missing.
function _iconOnError(id) {
  const fb = getItemIconFallback?.(id) ?? ITEM_ICON_PLACEHOLDER;
  return `this.onerror=null;this.src='${fb}'`;
}

// Tiles are drawn at the size CSS shows them — keep these three in step
// with the icon sizes in style.css "Character Sheet" / "Item system".
const _csWide  = window.matchMedia('(min-width:700px) and (min-height:521px)');
const _csShort = window.matchMedia('(max-width:699px) and (max-height:720px)');
function _csIconPx(phone, short, wide) {
  if (_csWide.matches)  return wide;
  if (_csShort.matches) return short;
  return phone;
}
for (const mq of [_csWide, _csShort]) {
  mq.addEventListener?.('change', () => {
    if (!uiCharOpen.val) return;
    resetLastEqKey();
    renderInventory();
    renderEquipment();
  });
}

// Click + Enter/Space for the div tiles (role="button")
function _onActivate(el, fn) {
  el.setAttribute('role', 'button');
  el.tabIndex = 0;
  el.addEventListener('click', fn);
  el.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); fn(); }
  });
}

// Render typed inventory slots into #cs-item-grid
function renderInventory() {
  const grid = document.getElementById('cs-item-grid');
  if (!grid || myId < 0) return;
  const me = players[myId];
  // Pack size comes straight from the server: `is` IS effectiveInvSlots(),
  // base plus equipment. This used to add computeEquipBonuses().slots on top
  // of it, double-counting every slot bonus — the old Math.min(12) clamp hid
  // that, and with INV_SLOTS_MAX at 18 it no longer would.
  const slots = packSlotsOf(me);
  const occupied = (me.it ?? []).filter(id => id > 0).length;
  console.log('%c[INV] renderInventory', 'color:#fc0', `myId=${myId} slots=${slots} occupied=${occupied}`);
  const count = document.getElementById('cs-pack-count');
  if (count) count.textContent = `${occupied}/${slots} slots`;
  const px = _csIconPx(48, 42, 56);
  grid.innerHTML = '';
  for (let i = 0; i < slots; i++) {
    const typeId = me.it?.[i] ?? 0;
    const qty    = me.iq?.[i] ?? 0;
    const div    = document.createElement('div');
    div.className = 'item-slot' + (typeId ? ' occupied' : '');
    div.dataset.slot = i;
    if (typeId) {
      const item = getItemById?.(typeId);
      const cat  = item?.category ?? 0;
      const name = item?.name ?? '?';
      div.dataset.cat = cat;
      div.title = `${name} — ${CAT_NAMES[cat] ?? '?'}`;
      div.setAttribute('aria-label', `${name}${qty > 1 ? ' ×' + qty : ''}, ${CAT_NAMES[cat] ?? ''}`);
      div.innerHTML =
        `<img class="item-slot-icon item-icon-img" src="${escHtml(_itemIcon(typeId, px))}" alt="" width="${px}" height="${px}" onerror="${_iconOnError(typeId)}">` +
        (qty > 1 ? `<span class="item-slot-qty">×${qty}</span>` : '') +
        `<span class="item-slot-name">${escHtml(name)}</span>`;
      _onActivate(div, () => openItemMenu(i, false));
    } else {
      div.setAttribute('aria-label', 'empty slot');
    }
    grid.appendChild(div);
  }
}

// Format a single mods object into compact "MP +1 \u00b7 LL +2" style.
// Display only \u2014 server is authoritative for actual gameplay effects.
function _formatMods(m) {
  if (!m) return '';
  const parts = [];
  const sign = (n) => (n > 0 ? '+' : '') + n;
  if (m.mp)        parts.push(`MP ${sign(m.mp)}`);
  if (m.ll)        parts.push(`LL ${sign(m.ll)}`);
  if (m.slots)     parts.push(`SLOTS ${sign(m.slots)}`);
  if (m.waterCap)  parts.push(`WATER CAP ${sign(m.waterCap)}`);
  if (m.vision)    parts.push(`VIS ${sign(m.vision)}`);
  if (m.rad)       parts.push(`RAD ${sign(m.rad)}/dawn`);
  if (m.fuelCost)  parts.push(`-${m.fuelCost} FUEL/dawn`);
  if (m.waterCost) parts.push(`-${m.waterCost} WATER/dawn`);
  if (m.foodCost)  parts.push(`-${m.foodCost} FOOD/dawn`);
  if (m.medCost)   parts.push(`-${m.medCost} MED/dawn`);
  if (m.scrapCost) parts.push(`-${m.scrapCost} SCRAP/dawn`);
  return parts.join(' \u00b7 ');
}

// Sum equipment bonuses across all equipped slots; collect qualitative notes.
// Effective pack size for a player, straight from the server's `is`.
// The ONE place the client decides how many slots a survivor has; the encounter
// haul tray and the craft panel both call it rather than re-deriving it.
function packSlotsOf(player) {
  const n = player?.is ?? ARCHETYPES[player?.arch ?? 0]?.invSlots ?? 8;
  return Math.max(1, Math.min(INV_SLOTS_MAX, n));
}

// Resource tokens that count against packSlotsOf(): everything in `inv` less
// the water riding in a canteen. `wc` is the server's canteenCap() — mirrors
// tokenLoad() in inventory_items.hpp. Pass `inv` to price a pack that isn't
// held yet (the encounter haul tray).
function tokenLoadOf(player, inv = player?.inv) {
  if (!Array.isArray(inv)) return 0;
  const total = inv.reduce((a, b) => a + (b || 0), 0);
  return total - Math.min(inv[0] || 0, player?.wc || 0);
}

// Does this item's MP bonus depend on paying a daily resource cost?
// applyDawnItemCosts() only grants such a bonus on a dawn where the cost was
// actually paid, so the panel must not promise it unconditionally.
function _isCostGated(m) {
  return !!(m && (m.fuelCost || m.waterCost || m.foodCost || m.medCost || m.scrapCost));
}

function computeEquipBonuses(player) {
  const tot = { mp:0, ll:0, slots:0, waterCap:0, vision:0, rad:0,
                fuelCost:0, waterCost:0, foodCost:0, medCost:0, scrapCost:0 };
  const notes = [];
  const dormant = [];   // cost-gated items that went unpaid at the last dawn
  if (!player?.eq) return { tot, notes, dormant };
  const unf = (myId >= 0 && player === players[myId]) ? (uiUnfuelled.val | 0) : 0;
  player.eq.forEach((id, slot) => {
    if (!id) return;
    const m = getItemMods?.(id);
    if (!m) return;
    const starved = _isCostGated(m) && !!(unf & (1 << slot));
    for (const k in tot) {
      if (!m[k]) continue;
      // A Motorbike with no fuel in the pack grants nothing that day: the
      // server skips its STAT_MP and reports the slot in EVT_DAWN "unf".
      // This panel used to add the +5 regardless, which is a good part of
      // what "the MP bonus doesn't work" looked like from the player's side.
      if (starved && (k === 'mp' || k.endsWith('Cost'))) continue;
      tot[k] += m[k];
    }
    const item = getItemById?.(id);
    if (starved) dormant.push({ name: item?.name ?? `Item #${id}` });
    if (m.note)  notes.push({ name: item?.name ?? `Item #${id}`, note: m.note });
  });
  return { tot, notes, dormant };
}

// Render equipment slots into #cs-equip-grid (EQUIP_HEAD..VEHICLE, equip[0..4])
// and their summed bonuses into #cs-equip-totals. Per-item mods and notes are
// on the item sheet (openItemMenu) — a phone-width slot can't hold them.
function renderEquipment() {
  const grid = document.getElementById('cs-equip-grid');
  if (!grid || myId < 0) return;
  const me = players[myId];
  const eqKey = JSON.stringify(me.eq ?? []);
  if (eqKey === _lastEqKey) { console.log('[INV] renderEquipment — cache hit, skip'); return; }
  _lastEqKey = eqKey;
  console.log('%c[INV] renderEquipment', 'color:#fc0', `myId=${myId} eq=${eqKey}`);
  const SLOT_LABELS = ['NOGGIN', 'HIDE', 'MITTS', 'HOOVES', 'RUST BUCKET'];
  const px = _csIconPx(40, 34, 48);
  grid.innerHTML = '';
  for (let s = 0; s < 5; s++) {
    const itemId = me.eq?.[s] ?? 0;
    const div    = document.createElement('div');
    div.className = 'equip-slot' + (itemId ? ' filled' : '');
    div.dataset.eslot = s;
    if (itemId) {
      const item = getItemById?.(itemId);
      const name = item?.name ?? '?';
      const modsLine = _formatMods(getItemMods?.(itemId));
      div.title = modsLine ? `${name} — ${modsLine}` : name;
      div.setAttribute('aria-label', `${SLOT_LABELS[s]}: ${name}`);
      div.innerHTML =
        `<span class="equip-slot-name">${SLOT_LABELS[s]}</span>` +
        `<img class="equip-slot-icon item-icon-img" src="${escHtml(_itemIcon(itemId, px))}" alt="" width="${px}" height="${px}" onerror="${_iconOnError(itemId)}">` +
        `<span class="equip-slot-label">${escHtml(name)}</span>`;
      _onActivate(div, () => openItemMenu(s, true));
    } else {
      div.setAttribute('aria-label', `${SLOT_LABELS[s]}: nothing`);
      div.innerHTML =
        `<span class="equip-slot-name">${SLOT_LABELS[s]}</span>` +
        `<span class="equip-slot-empty">─</span>`;
    }
    grid.appendChild(div);
  }

  // Totals — sums bonuses across all equipped items. Display only; gameplay
  // is still driven by server-side calculations. Notes show at ≥700px.
  const totals = document.getElementById('cs-equip-totals');
  if (!totals) return;
  const { tot, notes, dormant } = computeEquipBonuses(me);
  const totalLine = _formatMods(tot);
  totals.innerHTML =
    (totalLine ? `<div class="eq-tot-line">${escHtml(totalLine)}</div>` : '') +
    notes.map(n => `<div class="eq-tot-note">• ${escHtml(n.name)}: ${escHtml(n.note)}</div>`).join('') +
    dormant.map(d => `<div class="eq-tot-warn">NO FUEL: ${escHtml(d.name)} grants nothing today</div>`).join('');
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
  const _itemName = getItemById?.(itemId)?.name ?? ('Item #' + itemId);
  console.log('%c[INV] openItemMenu', 'color:#fc0;font-weight:bold', `slot=${slotIdx} itemId=${itemId} name="${_itemName}" qty=${itemQty} isEquipped=${isEquipped}`);

  const item   = getItemById?.(itemId);
  const name   = item?.name ?? ('Item #' + itemId);
  const story  = item?.story ?? null;
  const isEquip = !isEquipped && item?.category === 1; // ITEM_EQUIPMENT=1
  const isCons  = !isEquipped && item?.category === 0; // ITEM_CONSUMABLE=0
  // ITEM_KEY=3 with a real server effect (Doomed Diary, Cursed Device, Pre-War
  // Net Map) — mirrors isKeyWithEffect in useItem() (inventory_items.hpp).
  // Never consumed: useItem() returns before decrementing qty for these.
  const isReadable = !isEquipped && item?.category === 3 && item?.usable;

  const menuIcon = document.getElementById('item-menu-icon');
  menuIcon.onerror = () => { menuIcon.onerror = null; menuIcon.src = getItemIconFallback?.(itemId) ?? ITEM_ICON_PLACEHOLDER; };
  menuIcon.src = _itemIcon(itemId, 44);
  document.getElementById('item-menu-name').textContent = name;

  // Category + bonuses: the sheet's tiles only show icon and name
  const modsEl = document.getElementById('item-menu-mods');
  if (modsEl) {
    const cat  = item?.category ?? 0;
    const mods = getItemMods?.(itemId);
    const line = _formatMods(mods);
    modsEl.innerHTML =
      `<span class="item-cat-badge ${CAT_CLASSES[cat] ?? 'cat-consumable'}">${escHtml(CAT_NAMES[cat] ?? '?')}</span>` +
      (line ? `<span class="item-menu-mods-line">${escHtml(line)}</span>` : '') +
      (mods?.note ? `<span class="item-menu-mods-note">${escHtml(mods.note)}</span>` : '');
  }

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
      console.log('%c[INV] use_item', 'color:#fc0;font-weight:bold', `slot=${slotIdx} itemId=${itemId} name="${name}"`);
      send({ t: 'use_item', slot: slotIdx });
    });
  }
  if (isReadable) {
    const preUse = item?.preUse ?? null;
    addBtn('\u25A4 Read' + (preUse ? ' \u2014 ' + preUse : ''), '', () => {
      closeItemMenu();
      if (item?.postUse) showBanner(item.postUse, null);
      console.log('%c[INV] use_item', 'color:#fc0;font-weight:bold', `slot=${slotIdx} itemId=${itemId} name="${name}" (key/read)`);
      send({ t: 'use_item', slot: slotIdx });
    });
  }
  if (isEquip) {
    addBtn('\u25A3 Strap On', '', () => {
      console.log('%c[INV] equip_item', 'color:#fc0;font-weight:bold', `slot=${slotIdx} itemId=${itemId} name="${name}"`);
      closeItemMenu();
      send({ t: 'equip_item', slot: slotIdx });
    });
  }
  if (isEquipped) {
    addBtn('\u25A1 Tear Off', '', () => {
      console.log('%c[INV] unequip_item', 'color:#fc0;font-weight:bold', `eslot=${slotIdx} itemId=${itemId} name="${name}"`);
      closeItemMenu();
      send({ t: 'unequip_item', eslot: slotIdx });
    });
  }
  if (!isEquipped && item?.category !== 3) { // KEY items not shown drop button
    addBtn('\u25BC Abandon', 'danger', () => {
      console.log('%c[INV] drop_item', 'color:#fc0;font-weight:bold', `slot=${slotIdx} itemId=${itemId} name="${name}" qty=${itemQty}`);
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
  console.log('[INV] closeItemMenu');
  document.getElementById('item-action-menu')?.classList.remove('open');
  document.getElementById('item-menu-backdrop')?.classList.remove('open');
}

// ── Remains ───────────────────────────────────────────────────────
// Where a survivor fell: `remains` (engine.js) holds {q,r,pid,nm,d,res[5]}
// per grave, from sync / ground_update "rm". The tokens are taken with
// {t:'loot',res} (0 = everything that fits); the fallen survivor's items are
// ordinary ground piles on the same hex and use pickup_item like any other.

// The label players actually see on the map (renderHexLabels), not q/r.
function hexNameOf(q, r) {
  return (typeof hexLabel !== 'undefined') ? `hex ${hexLabel[r * MAP_COLS + q]}` : `(${q},${r})`;
}

function remainsAt(q, r) {
  return (typeof remains === 'undefined' ? [] : remains).find(rm => rm.q === q && rm.r === r) ?? null;
}

// Game-days before the dawn sweep takes something stamped day `d` --
// groundAgeOut() clears it at the dawn that makes it GROUND_AGE_DAYS old.
// dayCount is a uint16 on the board, hence the mask.
function groundDaysLeft(d) {
  return Math.max(0, GROUND_AGE_DAYS - (((gameState?.dc ?? 0) - (d ?? 0)) & 0xFFFF));
}
function groundDaysLabel(d) {
  const left = groundDaysLeft(d);
  return left <= 1 ? 'gone at dawn' : `${left} days left`;
}

// Arriving on a hex with remains says so once: the grave marker is easy to
// read from a distance but the pickup chips live in the hex panel.
let _remainsNotedKey = '';
function noteRemainsUnderfoot(q, r) {
  const key = `${q}_${r}`;
  const rm  = myDepth ? null : remainsAt(q, r);
  if (!rm) { _remainsNotedKey = ''; return; }
  if (key === _remainsNotedKey) return;
  _remainsNotedKey = key;
  const tokens = rm.res.reduce((a, b) => a + b, 0);
  const piles  = (groundItems ?? []).filter(gi => gi.q === q && gi.r === r && gi.id > 0).length;
  const what   = [tokens ? `${tokens} supplies` : '', piles ? `${piles} item${piles > 1 ? 's' : ''}` : '']
    .filter(Boolean).join(' and ');
  showToast(`☠ ${rm.nm || 'Someone'}'s remains — ${what || 'picked clean'}. Open the hex panel to take them.`);
}

// Ground items for the hex info panel
function renderHexGroundItems(q, r) {
  const row  = document.getElementById('hi-ground-row');
  const list = document.getElementById('hi-ground-list');
  if (!list || !row) return;
  // GroundItem has no depth -- the table is surface coordinates only, so
  // underground a q/r match would list whatever lies on some surface hex.
  // Remains are the same: a fall below lands on the hatch above.
  const here = (typeof groundItems === 'undefined' || myDepth ? [] : groundItems)
    .filter(gi => gi.q === q && gi.r === r && gi.id > 0);
  const rm = myDepth ? null : remainsAt(q, r);
  console.log('[INV] renderHexGroundItems', `q=${q} r=${r} itemsFound=${here.length} remains=${!!rm}`);
  if (here.length === 0 && !rm) {
    row.style.display = 'none';
    list.innerHTML = '';
    return;
  }
  row.style.display = '';
  list.innerHTML = '';
  if (rm) {
    const head = document.createElement('div');
    head.className = 'hi-remains-head';
    head.innerHTML = `☠ <span class="hi-remains-who">${escHtml(rm.nm || 'Someone')}</span> fell here · ${groundDaysLabel(rm.d)}`;
    list.appendChild(head);
    const kinds = [];
    (rm.res ?? []).forEach((n, k) => { if (n > 0) kinds.push({ n, k }); });
    kinds.forEach(({ n, k }) => {
      const name = RES_NAMES[k + 1];
      const span = document.createElement('span');
      span.className = 'hi-ground-pickup';
      span.title = `Take ${name}`;
      span.innerHTML = `<span class="dot ${RES_DOT_CLASSES[k]}"></span>${escHtml(name)} ×${n} <span class="gp-plus">+</span>`;
      span.addEventListener('click', () => {
        console.log('%c[INV] loot', 'color:#fc0;font-weight:bold', `res=${k + 1} have=${n}`);
        send({ t: 'loot', res: k + 1 });
      });
      list.appendChild(span);
    });
    if (kinds.length > 1) {
      const all = document.createElement('span');
      all.className = 'hi-ground-pickup hi-remains-all';
      all.title = 'Take every supply that fits';
      all.innerHTML = `TAKE ALL <span class="gp-plus">+</span>`;
      all.addEventListener('click', () => {
        console.log('%c[INV] loot', 'color:#fc0;font-weight:bold', 'res=all');
        send({ t: 'loot' });
      });
      list.appendChild(all);
    }
  }
  here.forEach(gi => {
    const item = getItemById?.(gi.id);
    const name = item?.name ?? ('Item #' + gi.id);
    const span = document.createElement('span');
    span.className = 'hi-ground-pickup';
    span.title = `Pick up ${name}` + (gi.d !== undefined ? ` · ${groundDaysLabel(gi.d)}` : '');
    span.innerHTML =
      `<img class="item-icon-img" src="${escHtml(_itemIcon(gi.id, 16))}" alt="" width="16" height="16" onerror="${_iconOnError(gi.id)}">` +
      `${escHtml(name)}` +
      (gi.n > 1 ? ` \u00d7${gi.n}` : '') +
      ` <span class="gp-plus">+</span>`;
    span.addEventListener('click', () => {
      console.log('%c[INV] pickup_item', 'color:#fc0;font-weight:bold', `gslot=${gi.g} itemId=${gi.id} name="${name}" qty=${gi.n ?? 1}`);
      send({ t: 'pickup_item', gslot: gi.g });
    });
    list.appendChild(span);
  });
}

// Close item menu on cancel button or backdrop tap
document.getElementById('item-menu-close')?.addEventListener('click', closeItemMenu);
document.getElementById('item-menu-backdrop')?.addEventListener('click', closeItemMenu);

// ── Resource drop sheet ───────────────────────────────────────────
// The five char-sheet inventory boxes (.inv-box[data-res]) are the only way to
// dump resource *tokens*, which is what the carry cap counts — `drop_item`
// only touches the typed item grid. Clicking one asks how many to abandon and
// sends {t:'drop_res',res,qty}; the server drops them on the current hex when
// it can hold them (see dropResource() in survival_state.hpp) and refunds the
// 10 pts/token the pickup awarded.
const RES_DOT_CLASSES = ['dot-water', 'dot-food', 'dot-fuel', 'dot-med', 'dot-scrap'];

let _resDropRes = 0;   // 1-5 while the sheet is open, 0 when closed

function _resDropHave() {
  if (_resDropRes < 1 || myId < 0) return 0;
  return players[myId]?.inv?.[_resDropRes - 1] ?? 0;
}

// Clamp the box to 1..have and mirror it onto the buttons/confirm label.
function _resDropSync() {
  const have  = _resDropHave();
  const input = document.getElementById('res-drop-qty');
  if (!input) return 0;
  let qty = parseInt(input.value, 10);
  if (!Number.isFinite(qty)) qty = 1;
  qty = Math.max(1, Math.min(have, qty));
  input.value = String(qty);
  input.max   = String(have);
  document.getElementById('res-drop-have').textContent = String(have);
  document.getElementById('res-drop-confirm-qty').textContent = String(qty);
  document.getElementById('res-drop-minus').disabled = qty <= 1;
  document.getElementById('res-drop-plus').disabled  = qty >= have;
  document.getElementById('res-drop-all').disabled   = qty >= have;
  return qty;
}

function openResDropMenu(res) {
  if (myId < 0 || res < 1 || res > 5) return;
  const have = players[myId]?.inv?.[res - 1] ?? 0;
  const name = RES_NAMES[res];
  console.log('%c[INV] openResDropMenu', 'color:#fc0;font-weight:bold', `res=${res} name="${name}" have=${have}`);
  if (have <= 0) { showToast(`Nothing to abandon \u2014 you carry no ${name}.`); return; }
  _resDropRes = res;

  document.getElementById('res-drop-dot').className  = 'dot ' + RES_DOT_CLASSES[res - 1];
  document.getElementById('res-drop-name').textContent = 'Abandon ' + name;
  document.getElementById('res-drop-qty').value = '1';

  // Tell the player where the tokens go before they commit: a hex already
  // holding a different resource cannot take them and they are gone for good.
  const me   = players[myId];
  const cell = gameMap[me.r]?.[me.q];
  const note = document.getElementById('res-drop-note');
  const lost = !!(cell && cell.resource > 0 && cell.resource !== res);
  note.classList.toggle('warn', lost);
  note.textContent = lost
    ? `This hex already holds ${RES_NAMES[cell.resource]} \u2014 anything you drop here is lost for good.`
    : 'Dropped on this hex \u2014 you can pick it back up. Costs back the 10 pts per token you scored for it.';

  _resDropSync();
  document.getElementById('res-drop-menu').classList.add('open');
  document.getElementById('res-drop-backdrop').classList.add('open');
}

function closeResDropMenu() {
  _resDropRes = 0;
  document.getElementById('res-drop-menu')?.classList.remove('open');
  document.getElementById('res-drop-backdrop')?.classList.remove('open');
}

// Wire the five inventory boxes (click + keyboard, they are role="button")
document.querySelectorAll('.inv-box[data-res]').forEach(box => {
  const res = parseInt(box.dataset.res, 10);
  box.addEventListener('click', () => openResDropMenu(res));
  box.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openResDropMenu(res); }
  });
});

// ...and the sidebar Supplies DROP buttons, which open the same sheet. Real
// <button>s, so Enter/Space already fire click - no keydown handler here.
document.querySelectorAll('.res-drop-btn[data-res]').forEach(btn => {
  btn.addEventListener('click', () => openResDropMenu(parseInt(btn.dataset.res, 10)));
});

document.getElementById('res-drop-minus')?.addEventListener('click', () => {
  const input = document.getElementById('res-drop-qty');
  input.value = String(parseInt(input.value, 10) - 1);
  _resDropSync();
});
document.getElementById('res-drop-plus')?.addEventListener('click', () => {
  const input = document.getElementById('res-drop-qty');
  input.value = String(parseInt(input.value, 10) + 1);
  _resDropSync();
});
document.getElementById('res-drop-all')?.addEventListener('click', () => {
  document.getElementById('res-drop-qty').value = String(_resDropHave());
  _resDropSync();
});
document.getElementById('res-drop-qty')?.addEventListener('input', _resDropSync);
document.getElementById('res-drop-qty')?.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); document.getElementById('res-drop-confirm').click(); }
});
document.getElementById('res-drop-confirm')?.addEventListener('click', () => {
  const res = _resDropRes;
  const qty = _resDropSync();
  if (res < 1 || qty < 1) { closeResDropMenu(); return; }
  console.log('%c[INV] drop_res', 'color:#fc0;font-weight:bold', `res=${res} name="${RES_NAMES[res]}" qty=${qty}`);
  closeResDropMenu();
  send({ t: 'drop_res', res, qty });
});
document.getElementById('res-drop-close')?.addEventListener('click', closeResDropMenu);
document.getElementById('res-drop-backdrop')?.addEventListener('click', closeResDropMenu);
