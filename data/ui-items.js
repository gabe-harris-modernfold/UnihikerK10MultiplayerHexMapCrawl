// ── Item System UI ────────────────────────────────────────────────

let _lastEqKey = '';  // moved here from ui-hud.js; used by renderEquipment + char-overlay derive
function resetLastEqKey() { _lastEqKey = ''; }

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
  const occupied = (me.it ?? []).filter(id => id > 0).length;
  console.log('%c[INV] renderInventory', 'color:#fc0', `myId=${myId} slots=${slots} occupied=${occupied}`);
  grid.innerHTML = '';
  for (let i = 0; i < slots; i++) {
    const typeId = me.it?.[i] ?? 0;
    const qty    = me.iq?.[i] ?? 0;
    const div    = document.createElement('div');
    div.className = 'item-slot' + (typeId ? ' occupied' : '');
    div.dataset.slot = i;
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

// Format a single mods object into compact "MP +1 \u00b7 LL +2" style.
// Display only \u2014 server is authoritative for actual gameplay effects.
function _formatMods(m) {
  if (!m) return '';
  const parts = [];
  const sign = (n) => (n > 0 ? '+' : '') + n;
  if (m.mp)        parts.push(`MP ${sign(m.mp)}`);
  if (m.ll)        parts.push(`LL ${sign(m.ll)}`);
  if (m.slots)     parts.push(`SLOTS ${sign(m.slots)}`);
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
function computeEquipBonuses(player) {
  const tot = { mp:0, ll:0, slots:0, vision:0, rad:0,
                fuelCost:0, waterCost:0, foodCost:0, medCost:0, scrapCost:0 };
  const notes = [];
  if (!player?.eq) return { tot, notes };
  for (const id of player.eq) {
    if (!id) continue;
    const m = getItemMods?.(id);
    if (!m) continue;
    for (const k in tot) if (m[k]) tot[k] += m[k];
    if (m.note) {
      const item = getItemById?.(id);
      notes.push({ name: item?.name ?? `Item #${id}`, note: m.note });
    }
  }
  return { tot, notes };
}

// Render equipment slots into #cs-equip-grid (EQUIP_HEAD..VEHICLE, equip[0..4])
function renderEquipment() {
  const grid = document.getElementById('cs-equip-grid');
  if (!grid || myId < 0) return;
  const me = players[myId];
  const eqKey = JSON.stringify(me.eq ?? []);
  if (eqKey === _lastEqKey) { console.log('[INV] renderEquipment — cache hit, skip'); return; }
  _lastEqKey = eqKey;
  console.log('%c[INV] renderEquipment', 'color:#fc0', `myId=${myId} eq=${eqKey}`);
  const SLOT_LABELS = ['NOGGIN', 'HIDE', 'MITTS', 'HOOVES', 'RUST BUCKET'];
  grid.innerHTML = '';
  for (let s = 0; s < 5; s++) {
    const itemId = me.eq?.[s] ?? 0;
    const div    = document.createElement('div');
    div.className = 'equip-slot' + (itemId ? ' filled' : '');
    div.dataset.eslot = s;
    if (itemId) {
      const item     = getItemById?.(itemId);
      const mods     = getItemMods?.(itemId);
      const modsLine = _formatMods(mods);
      const noteLine = mods?.note ? escHtml(mods.note) : '';
      div.innerHTML =
        `<span class="equip-slot-name">${SLOT_LABELS[s]}</span>` +
        `<img class="equip-slot-icon item-icon-img" src="${escHtml(_itemIcon(itemId))}" alt="" width="28" height="28" onerror="this.src='${ITEM_ICON_PLACEHOLDER}'">` +
        `<span class="equip-slot-label">${escHtml(item?.name ?? '?')}</span>` +
        (modsLine ? `<span class="equip-slot-bonus" style="display:block;font-size:10px;color:var(--gold,#ffd700);margin-top:2px;letter-spacing:0.5px">${escHtml(modsLine)}</span>` : '') +
        (noteLine ? `<span class="equip-slot-note" style="display:block;font-size:9px;color:var(--txt-dim,#888);font-style:italic;margin-top:1px">${noteLine}</span>` : '');
      div.addEventListener('click', () => openItemMenu(s, true));
    } else {
      div.innerHTML =
        `<span class="equip-slot-name">${SLOT_LABELS[s]}</span>` +
        `<span class="equip-slot-empty">\u2500</span>` +
        `<span class="equip-slot-label" style="color:var(--txt-dim)">nothing</span>`;
    }
    grid.appendChild(div);
  }

  // Totals summary block \u2014 sums bonuses across all equipped items.
  // Display only; gameplay still driven by server-side calculations.
  const { tot, notes } = computeEquipBonuses(me);
  const totals = document.createElement('div');
  totals.className = 'equip-totals';
  totals.style.cssText = 'grid-column:1 / -1;padding:8px;border-top:1px solid var(--bdr-mid,#333);margin-top:6px;font-size:12px';
  const totalLine = _formatMods(tot);
  const hasAny    = totalLine || notes.length;
  totals.innerHTML =
    `<div style="font-weight:bold;color:var(--gold,#ffd700);margin-bottom:4px;letter-spacing:1px">\u26a1 EQUIPMENT BONUSES</div>` +
    (totalLine
      ? `<div style="color:var(--txt-bright,#eee)">${escHtml(totalLine)}</div>`
      : (hasAny ? '' : '<div style="color:var(--txt-dim,#888)">\u2014 nothing strapped on \u2014</div>')) +
    notes.map(n =>
      `<div style="color:var(--txt-dim,#aaa);font-style:italic;margin-top:2px">\u2022 ${escHtml(n.name)}: ${escHtml(n.note)}</div>`
    ).join('');
  grid.appendChild(totals);
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
      console.log('%c[INV] use_item', 'color:#fc0;font-weight:bold', `slot=${slotIdx} itemId=${itemId} name="${name}"`);
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

// Ground items for the hex info panel
function renderHexGroundItems(q, r) {
  const row  = document.getElementById('hi-ground-row');
  const list = document.getElementById('hi-ground-list');
  if (!list || !row) return;
  const here = (typeof groundItems === 'undefined' ? [] : groundItems)
    .filter(gi => gi.q === q && gi.r === r && gi.id > 0);
  console.log('[INV] renderHexGroundItems', `q=${q} r=${r} itemsFound=${here.length}`);
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
      console.log('%c[INV] pickup_item', 'color:#fc0;font-weight:bold', `gslot=${gi.g} itemId=${gi.id} name="${name}" qty=${gi.n ?? 1}`);
      send({ t: 'pickup_item', gslot: gi.g });
    });
    list.appendChild(span);
  });
}

// Close item menu on cancel button or backdrop tap
document.getElementById('item-menu-close')?.addEventListener('click', closeItemMenu);
document.getElementById('item-menu-backdrop')?.addEventListener('click', closeItemMenu);
