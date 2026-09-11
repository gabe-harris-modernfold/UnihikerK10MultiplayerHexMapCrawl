// ── Encounter overlay ─────────────────────────────────────────────
//
// One screen, one flow:
//
//   [title]                  ← encounter title + terrain kicker
//   [result strip]           ← last roll: verdict, dice, hazard text, gains/losses
//   [story text]
//   [choice cards]           ← label + odds + skill + cost; disabled when unaffordable
//   [haul tray] [LEAVE btn]  ← pending loot always visible; one exit button whose
//                              label says exactly what leaving does
//
// No timers gate the player. A roll result appears inline and stays until the
// next action. On success the next scene renders immediately beneath the
// result. On a setback the same choices come back so the player can retry or
// leave.
//
// The server is authoritative. `enc_choice` carries only the choice index;
// the firmware reads costs, hazards, loot and can_bank from the same JSON file
// (encounter_engine.hpp). The copy fetched here is for display only — the odds,
// cost chips and haul tray are previews, and the `enc_res` event is the truth.

// Resolve {{placeholders}} deterministically: pick one option per key up front
// so the same name is used in title, text, and hazard copy for the whole visit.
function pickPlaceholders(placeholders) {
  const picked = {};
  if (!placeholders) return picked;
  for (const [key, opts] of Object.entries(placeholders)) {
    picked[key] = (Array.isArray(opts) && opts.length)
      ? opts[Math.floor(Math.random() * opts.length)]
      : key;
  }
  return picked;
}

function resolveText(text, picked) {
  if (!text) return '';
  return String(text).replaceAll(/\{\{(\w+)\}\}/g, (_, key) => picked?.[key] ?? key);
}

// P(2d6 >= n)
const ENC_P2D6 = { 2:36, 3:35, 4:33, 5:30, 6:26, 7:21, 8:15, 9:10, 10:6, 11:3, 12:1 };
function encPct2d6(need) {
  if (need <= 2)  return 100;
  if (need > 12)  return 0;
  return Math.round((ENC_P2D6[need] / 36) * 100);
}

// Mirror of the server's computeEncounterDN() (boot-assets.hpp) so the odds
// shown on a card match what the server will actually roll against.
function encComputeDN(me, baseRisk) {
  const tc = gameState?.tc ?? 0;
  let risk = Math.min(baseRisk, 100);
  if (tc >= 5)  risk += 5;
  if (tc >= 9)  risk += 5;
  if (tc >= 13) risk += 5;
  if (tc >= 17) risk += 5;
  risk = Math.max(0, Math.min(100, risk));
  let dn = 2 + Math.floor((risk * 10) / 100);
  let bonus = 0;
  const ll  = me?.ll  ?? 7;
  const rad = me?.rad ?? 0;
  if (ll > 4)  bonus += Math.floor((ll - 4) / 2);
  if (rad > 3) dn    += Math.floor((rad - 3) / 2);
  return Math.max(2, Math.min(12, dn - bonus));
}

// Encounter JSON skill ids match the firmware's 5-skill enum:
// 0 NAVIGATE · 1 FORAGE · 2 SCAVENGE · 3 SHELTER · 4 ENDURE.
// Anything out of range is treated as skill 0, exactly as resolveCheck() does.
function encSkillSlot(skill) { return (skill >= 0 && skill < SK_NAMES.length) ? skill : 0; }
function encSkillLabel(skill) { return SK_NAMES[encSkillSlot(skill)]; }

function initEncounterOverlay() {
  const overlay   = document.getElementById('enc-overlay');
  const titleEl   = document.getElementById('enc-title');
  const kickerEl  = document.getElementById('enc-kicker');
  const scrollEl  = document.getElementById('enc-scroll');
  const resultEl  = document.getElementById('enc-result');
  const resVerd   = document.getElementById('enc-res-verdict');
  const resRoll   = document.getElementById('enc-res-roll');
  const resText   = document.getElementById('enc-res-text');
  const resDelta  = document.getElementById('enc-res-delta');
  const nodeText  = document.getElementById('enc-node-text');
  const choiceEl  = document.getElementById('enc-choices');
  const haulItems = document.getElementById('enc-haul-items');
  const haulEmpty = document.getElementById('enc-haul-empty');
  const leaveBtn  = document.getElementById('enc-leave-btn');
  const leaveHint = document.getElementById('enc-leave-hint');

  const RES_NAMES_ENC = ['Water', 'Food', 'Fuel', 'Meds', 'Scrap'];
  const RES_DOT_CLASS = ['dot-water', 'dot-food', 'dot-fuel', 'dot-med', 'dot-scrap'];
  const ROLL_TIMEOUT_MS = 8000;

  // ── State ───────────────────────────────────────────────────────
  let enc          = null;    // loaded encounter JSON
  let picked       = {};      // resolved placeholders for this visit
  let node         = null;    // current node
  let nodeKey      = '';
  let phase        = 'idle';  // idle | reading | rolling | ejected
  let pendingLoot  = [0, 0, 0, 0, 0];
  let pendingItems = [];      // [{id, qty}] rolled from loot tables, banked on leave
  let terminal     = false;   // reached a node with no choices
  let pendingNext  = '';      // node key we move to if the pending roll succeeds
  let pendingHaz   = '';      // hazard copy shown if the pending roll fails
  let rollTimer    = 0;
  let confirmTimer = 0;
  let leaveArmed   = false;   // two-tap confirm when leaving would drop loot

  const me = () => (myId >= 0 ? players[myId] : null);

  // ── Helpers ─────────────────────────────────────────────────────
  function haulCount() {
    return pendingLoot.reduce((a, b) => a + b, 0) + pendingItems.reduce((a, it) => a + it.qty, 0);
  }

  function canBankHere() { return terminal || !!(node?.can_bank); }

  // Keys the server's encounter_engine.hpp understands, for the authoring warnings.
  const COST_KEYS = new Set(['ll', 'radiation', 'food', 'water', 'scrap', 'med']);
  const PEN_KEYS  = new Set(['ll', 'radiation', 'water', 'food', 'fuel', 'med', 'scrap']);
  // Keys allowed on the hazard object itself (alongside `penalty`).
  const HAZ_KEYS  = new Set(['text', 'penalty', 'wound', 'ends_encounter']);

  // Encounter JSON is hand-authored; an unrecognised key would otherwise be
  // dropped in silence and the choice would simply be free. Say so instead.
  function warnUnknownKeys(obj, allowed, where) {
    for (const k of Object.keys(obj ?? {})) {
      if (!allowed.has(k)) console.warn(`[enc] ${where}: unknown key "${k}" — ignored`);
    }
  }

  function costChips(cost) {
    // cost keys: ll, radiation, food, water, scrap, med. A negative cost is a gain.
    const out = [];
    const spend = (v, name) => { if (v) out.push({ txt: `${v > 0 ? '−' : '+'}${Math.abs(v)} ${name}`, bad: v > 0 }); };
    spend(cost?.ll,    'Life');
    spend(cost?.food,  'Food');
    spend(cost?.water, 'Water');
    spend(cost?.scrap, 'Scrap');
    spend(cost?.med,   'Meds');
    const rad = cost?.radiation ?? 0;
    if (rad) out.push({ txt: `${rad > 0 ? '+' : '−'}${Math.abs(rad)} Rad`, bad: rad > 0 });
    return out;
  }

  function canAfford(cost) {
    const p = me(); if (!p) return true;
    if ((cost?.ll ?? 0) > (p.ll ?? 0))                       return false;
    if ((p.rad ?? 0) + (cost?.radiation ?? 0) > 10)          return false;
    if ((cost?.food  ?? 0) > (p.inv?.[1] ?? 0))              return false;
    if ((cost?.water ?? 0) > (p.inv?.[0] ?? 0))              return false;
    if ((cost?.scrap ?? 0) > (p.inv?.[4] ?? 0))              return false;
    if ((cost?.med   ?? 0) > (p.inv?.[3] ?? 0))              return false;
    return true;
  }

  function oddsFor(choice) {
    const p = me();
    const dn = encComputeDN(p, choice.base_risk ?? 50);
    const sv = p?.sk?.[encSkillSlot(choice.skill ?? 0)] ?? 0;
    return { pct: encPct2d6(dn - sv), dn };
  }

  function oddsClass(pct) {
    if (pct >= 70) return 'odds-hi';
    if (pct >= 45) return 'odds-mid';
    return 'odds-lo';
  }

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  // ── Render: haul tray + leave button ───────────────────────────
  function renderHaul() {
    haulItems.innerHTML = '';
    let any = false;
    pendingLoot.forEach((v, i) => {
      if (v <= 0) return;
      any = true;
      const chip = el('span', 'enc-haul-chip');
      chip.appendChild(el('span', `res-dot ${RES_DOT_CLASS[i]}`));
      chip.appendChild(el('span', 'enc-haul-qty', `${v}`));
      chip.appendChild(el('span', 'enc-haul-name', RES_NAMES_ENC[i]));
      haulItems.appendChild(chip);
    });
    pendingItems.forEach(it => {
      any = true;
      const def  = typeof getItemById === 'function' ? getItemById(it.id) : null;
      const chip = el('span', 'enc-haul-chip enc-haul-item');
      if (def?.icon) {
        const img = document.createElement('img');
        img.src = def.icon; img.alt = ''; img.width = 14; img.height = 14;
        chip.appendChild(img);
      }
      chip.appendChild(el('span', 'enc-haul-qty', it.qty > 1 ? `${it.qty}×` : ''));
      chip.appendChild(el('span', 'enc-haul-name', def?.name ?? `Item ${it.id}`));
      haulItems.appendChild(chip);
    });
    haulEmpty.hidden = any;
  }

  function renderLeave() {
    disarmLeave();
    leaveBtn.disabled = (phase === 'rolling');
    leaveBtn.classList.remove('primary', 'danger');
    const n = haulCount();

    if (phase === 'ejected') {
      leaveBtn.textContent = 'LEAVE';
      leaveBtn.classList.add('primary');
      leaveHint.textContent = 'The encounter is over.';
      return;
    }
    if (terminal) {
      leaveBtn.textContent = n ? 'TAKE HAUL & LEAVE' : 'FINISH & LEAVE';
      leaveBtn.classList.add('primary');
      leaveHint.textContent = 'Nothing more here. Leaving now scores a full-clear bonus.';
      return;
    }
    if (n && canBankHere()) {
      leaveBtn.textContent = 'TAKE HAUL & LEAVE';
      leaveBtn.classList.add('primary');
      leaveHint.textContent = 'Pocket what you have, or push on for more.';
      return;
    }
    if (n) {
      leaveBtn.textContent = 'LEAVE · DROPS HAUL';
      leaveBtn.classList.add('danger');
      leaveHint.textContent = 'You can’t carry loot out from here. Push on to secure it.';
      return;
    }
    leaveBtn.textContent = 'WALK AWAY';
    leaveHint.textContent = 'Leave empty-handed. The place stays closed to you.';
  }

  function disarmLeave() {
    leaveArmed = false;
    clearTimeout(confirmTimer);
    leaveBtn.classList.remove('armed');
  }

  // ── Render: result strip ───────────────────────────────────────
  function hideResult() {
    resultEl.hidden = true;
    resultEl.className = '';
    resVerd.textContent = ''; resRoll.textContent = '';
    resText.textContent = ''; resDelta.innerHTML = '';
  }

  function showResult({ ok, verdict, roll, text, deltas, note }) {
    resultEl.hidden = false;
    resultEl.className = ok ? 'ok' : 'bad';
    resVerd.textContent = verdict;
    resRoll.textContent = roll ?? '';
    resText.textContent = text ?? '';
    resText.hidden = !text;
    resDelta.innerHTML = '';
    (deltas ?? []).forEach(d => resDelta.appendChild(el('span', `enc-delta ${d.pos ? 'pos' : 'neg'}`, d.txt)));
    if (note) resDelta.appendChild(el('span', 'enc-delta-note', note));
    scrollEl.scrollTop = 0;
  }

  // ── Render: story + choices ────────────────────────────────────
  function renderNode(key) {
    const n = enc?.nodes?.[key];
    if (!n) { console.error('[ENC] missing node', key); return; }
    node = n; nodeKey = key;
    terminal = !(Array.isArray(n.choices) && n.choices.length);
    phase = 'reading';

    nodeText.textContent = resolveText(n.text, picked);
    renderChoices();
    renderHaul();
    renderLeave();
  }

  function renderChoices() {
    choiceEl.innerHTML = '';
    if (terminal) {
      choiceEl.appendChild(el('div', 'enc-terminal', 'You’ve seen all there is to see here.'));
      return;
    }
    node.choices.forEach((ch, idx) => {
      const btn = el('button', 'enc-choice');
      btn.type = 'button';
      const affordable = canAfford(ch.cost);
      const { pct } = oddsFor(ch);

      const num = el('span', 'enc-choice-num', `${idx + 1}`);
      const body = el('span', 'enc-choice-body');
      body.appendChild(el('span', 'enc-choice-label', resolveText(ch.label, picked)));

      const meta = el('span', 'enc-choice-meta');
      const odds = el('span', `enc-odds ${oddsClass(pct)}`);
      odds.appendChild(el('b', '', `${pct}%`));
      odds.appendChild(el('span', '', ` ${encSkillLabel(ch.skill ?? 0)}`));
      meta.appendChild(odds);
      costChips(ch.cost).forEach(c => meta.appendChild(el('span', `enc-cost ${c.bad ? 'bad' : 'good'}`, c.txt)));
      if (!affordable) meta.appendChild(el('span', 'enc-cost cant', 'CAN’T AFFORD'));
      body.appendChild(meta);

      btn.appendChild(num);
      btn.appendChild(body);
      btn.disabled = !affordable || phase === 'rolling';
      btn.addEventListener('click', () => sendChoice(ch, btn));
      choiceEl.appendChild(btn);
    });
  }

  function setChoicesEnabled(on) {
    choiceEl.querySelectorAll('.enc-choice').forEach(b => {
      b.classList.remove('rolling');
      b.disabled = !on || b.dataset.cant === '1';
    });
  }

  // ── Send a choice ──────────────────────────────────────────────
  function sendChoice(ch, btn) {
    if (phase !== 'reading') return;
    phase = 'rolling';
    disarmLeave();
    hideResult();

    const haz    = (ch.hazard_id && enc.hazards) ? (enc.hazards[ch.hazard_id] ?? {}) : {};
    const hazPen = haz.penalty ?? {};
    const cost   = ch.cost ?? {};
    const nextKey  = ch.success_node ?? '';

    // Authoring aid only: flag keys the server will ignore.
    warnUnknownKeys(cost,   COST_KEYS, `choice "${ch.label}" cost`);
    warnUnknownKeys(hazPen, PEN_KEYS,  `hazard "${ch.hazard_id}" penalty`);
    warnUnknownKeys(haz,    HAZ_KEYS,  `hazard "${ch.hazard_id}"`);

    pendingNext = nextKey;
    pendingHaz  = haz.text ? resolveText(haz.text, picked) : '';

    // The server resolves everything from its own copy of this file.
    const ci = Array.isArray(node?.choices) ? node.choices.indexOf(ch) : -1;
    send({ t: 'enc_choice', ci });

    // Lock the cards; mark the one we picked.
    choiceEl.querySelectorAll('.enc-choice').forEach(b => { b.dataset.cant = b.disabled ? '1' : '0'; b.disabled = true; });
    btn.classList.add('rolling');
    leaveBtn.disabled = true;

    clearTimeout(rollTimer);
    rollTimer = setTimeout(() => {
      if (phase !== 'rolling') return;
      phase = 'reading';
      setChoicesEnabled(true);
      renderLeave();
      showResult({ ok: false, verdict: 'NO ANSWER', text: 'The server didn’t respond. Try again.' });
    }, ROLL_TIMEOUT_MS);
  }

  // ── Server callbacks ───────────────────────────────────────────
  globalThis._onEncResult = function(ev) {
    if (!enc || phase !== 'rolling') return;
    clearTimeout(rollTimer);

    const rollTxt = (ev.tot !== undefined && ev.dn !== undefined)
      ? `Rolled ${ev.tot} vs ${ev.dn}` : '';

    if (ev.out) {
      const deltas = [];
      if (Array.isArray(ev.loot)) {
        ev.loot.forEach((v, i) => {
          if (v > 0) { pendingLoot[i] += v; deltas.push({ txt: `+${v} ${RES_NAMES_ENC[i]}`, pos: true }); }
        });
      }
      // Typed items the server granted: an explicit node "item" entry and/or a
      // loot-table roll (two at most per scene).
      [[ev.it, ev.iq], [ev.it2, ev.iq2]].forEach(([id, qty]) => {
        if (!id || !qty) return;
        pendingItems.push({ id, qty });
        const def = typeof getItemById === 'function' ? getItemById(id) : null;
        deltas.push({ txt: `+${qty > 1 ? qty + '× ' : ''}${def?.name ?? 'Item'}`, pos: true });
      });
      const next = pendingNext;
      pendingNext = ''; pendingHaz = '';
      if (next && enc.nodes?.[next]) renderNode(next);
      else { terminal = true; phase = 'reading'; renderChoices(); renderHaul(); renderLeave(); }
      showResult({
        ok: true,
        verdict: 'YOU GET THROUGH',
        roll: rollTxt,
        deltas,
        note: deltas.length ? 'Added to your haul.' : '',
      });
      return;
    }

    // Setback
    const deltas = [];
    if (ev.penLL  < 0) deltas.push({ txt: `${ev.penLL} Life`,     pos: false });
    if (ev.penRad > 0) deltas.push({ txt: `+${ev.penRad} Rad`,    pos: false });
    (ev.penRes ?? []).forEach((v, i) => {
      if (v > 0) deltas.push({ txt: `-${v} ${RES_NAMES_ENC[i]}`, pos: false });
    });
    const hazText = pendingHaz || 'The wasteland takes its toll.';
    pendingNext = ''; pendingHaz = '';

    if (ev.ends) {
      phase = 'ejected';
      choiceEl.innerHTML = '';
      choiceEl.appendChild(el('div', 'enc-terminal bad', 'You’re driven out. Whatever you hadn’t pocketed is lost.'));
      pendingLoot = [0, 0, 0, 0, 0]; pendingItems = [];
      renderHaul(); haulEmpty.textContent = 'lost'; renderLeave();
      showResult({ ok: false, verdict: 'DRIVEN OUT', roll: rollTxt, text: hazText, deltas });
      return;
    }

    phase = 'reading';
    renderChoices();  // rebuild: odds may have shifted with LL / rad
    renderLeave();
    showResult({
      ok: false, verdict: 'SETBACK', roll: rollTxt, text: hazText, deltas,
      note: 'The way is still open. Choose again, or leave.',
    });
  };

  globalThis._onEncError = function(msg) {
    if (!enc || phase !== 'rolling') return;
    clearTimeout(rollTimer);
    phase = 'reading';
    renderChoices();
    renderLeave();
    showResult({ ok: false, verdict: 'NOT POSSIBLE', text: msg || 'The server refused that choice.' });
  };

  globalThis._onEncBank = function() { closeEncounter(); };

  globalThis._onEncEnd = function(ev) {
    // A hazard that ends the encounter already rendered its own "DRIVEN OUT"
    // screen — leave it up so the player can read what happened.
    if (ev?.reason === 'hazard' && phase === 'ejected') return;
    closeEncounter();
  };

  // ── Open / close ───────────────────────────────────────────────
  function openEncounter(json) {
    enc          = json;
    picked       = pickPlaceholders(json.placeholders);
    pendingLoot  = [0, 0, 0, 0, 0];
    pendingItems = [];
    terminal     = false;
    pendingNext  = ''; pendingHaz = '';
    haulEmpty.textContent = 'nothing yet';
    hideResult();

    titleEl.textContent = resolveText(json.title || 'Encounter', picked);
    const p = me();
    const cell = p ? gameMap[p.r]?.[p.q] : null;
    const tname = cell ? (TERRAIN[cell.terrain]?.name ?? '') : '';
    kickerEl.textContent = tname ? `ENCOUNTER · ${tname.toUpperCase()}` : 'ENCOUNTER';

    overlay.classList.add('open');
    overlay.style.display = '';
    const startKey = json.nodes?.[json.start_node] ? json.start_node : Object.keys(json.nodes ?? {})[0];
    if (startKey) renderNode(startKey);
    else { console.error('[ENC] encounter has no nodes', json); send({ t: 'enc_abort' }); closeEncounter(); }
    scrollEl.scrollTop = 0;
    leaveBtn.blur();
  }

  function closeEncounter() {
    clearTimeout(rollTimer);
    disarmLeave();
    overlay.classList.remove('open');
    overlay.style.display = 'none';
    enc = null; node = null; nodeKey = '';
    phase = 'idle';
    pendingLoot = [0, 0, 0, 0, 0]; pendingItems = [];
    terminal = false; pendingNext = ''; pendingHaz = '';
    hideResult();
    choiceEl.innerHTML = ''; haulItems.innerHTML = '';
  }

  // Called from network.js enc_path handler
  globalThis._startEncounterFetch = function(biome, id) {
    const url = `/enc?biome=${encodeURIComponent(biome)}&id=${encodeURIComponent(id)}`;
    fetch(url)
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json(); })
      .then(openEncounter)
      .catch(e => {
        console.error('[ENC] fetch failed, aborting:', e.message);
        showToast?.('⊙ The way in is blocked. (encounter failed to load)');
        send({ t: 'enc_abort' });
      });
  };

  // ── Leave button ───────────────────────────────────────────────
  leaveBtn.addEventListener('click', () => {
    if (phase === 'rolling' || !enc) return;
    if (phase === 'ejected') { closeEncounter(); return; }

    const n = haulCount();
    if (terminal || (n && canBankHere())) { send({ t: 'enc_bank' }); closeEncounter(); return; }

    if (n && !leaveArmed) {
      // Two-tap confirm: leaving here forfeits the haul.
      leaveArmed = true;
      leaveBtn.classList.add('armed');
      leaveBtn.textContent = 'DROP HAUL? TAP AGAIN';
      confirmTimer = setTimeout(() => renderLeave(), 3000);
      return;
    }
    send({ t: 'enc_abort' });
    closeEncounter();
  });

  // Number keys pick choices; nothing on Escape (no accidental exits).
  document.addEventListener('keydown', e => {
    if (!enc || phase !== 'reading' || terminal) return;
    if (e.target && /INPUT|TEXTAREA/.test(e.target.tagName)) return;
    const k = Number.parseInt(e.key, 10);
    if (k >= 1 && k <= 9) {
      const btn = choiceEl.querySelectorAll('.enc-choice')[k - 1];
      if (btn && !btn.disabled) btn.click();
    }
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
