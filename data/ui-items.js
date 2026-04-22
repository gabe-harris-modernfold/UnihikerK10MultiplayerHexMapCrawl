// ── Item System UI ────────────────────────────────────────────────

let _lastEqKey = '';  // moved here from ui-hud.js; used by renderEquipment + char-overlay derive

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
  const eqKey = JSON.stringify(me.eq ?? []);
  if (eqKey === _lastEqKey) return;
  _lastEqKey = eqKey;
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
