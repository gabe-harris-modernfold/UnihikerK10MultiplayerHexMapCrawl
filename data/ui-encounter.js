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
//
// The one exception is `enc_bank`, which carries `keep:[5]` — how much of each
// resource the player chose to take on the haul tray's steppers. It is a
// request, not an instruction: the server clamps every entry against its own
// pendingLoot[] and banks the smaller of the two, so a drifted (or hostile)
// client can never bank more than it actually won. Omitting `keep` means "bank
// everything", which is exactly what an older client sends.

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
// `assists` = teammates behind the next roll (enc_assist): encRunChoice()
// takes ENC_ASSIST_DN off for each, after the clamp, floor 2.
function encComputeDN(me, baseRisk, assists = 0) {
  const tc = gameState?.tc ?? 0;
  let risk = Math.min(baseRisk, 100);
  if (tc >= 5)  risk += 5;
  if (tc >= 9)  risk += 5;
  if (tc >= 13) risk += 5;
  if (tc >= 17) risk += 5;
  risk = Math.max(0, Math.min(100, risk));
  let dn = 5 + Math.floor((risk * 7) / 100);   // mirrors computeEncounterDN()
  let bonus = 0;
  const ll  = me?.ll  ?? 7;
  const rad = me?.rad ?? 0;
  if (ll > 4)  bonus += Math.floor((ll - 4) / 2);
  if (rad > 3) dn    += Math.floor((rad - 3) / 2);
  dn = Math.max(2, Math.min(12, dn - bonus));
  return assists > 0 ? Math.max(2, dn - assists * ENC_ASSIST_DN) : dn;
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
  const haulEl    = document.getElementById('enc-haul');
  const haulHead  = document.getElementById('enc-haul-head');
  const haulSum   = document.getElementById('enc-haul-sum');
  const haulPack  = document.getElementById('enc-haul-pack');
  const haulTog   = document.getElementById('enc-haul-toggle');
  const haulRes   = document.getElementById('enc-haul-res');
  const haulItems = document.getElementById('enc-haul-items');
  const haulEmpty = document.getElementById('enc-haul-empty');
  const haulCarry = document.getElementById('enc-haul-carry');
  const leaveBtn  = document.getElementById('enc-leave-btn');
  const leaveHint = document.getElementById('enc-leave-hint');

  const RES_NAMES_ENC = ['Water', 'Food', 'Fuel', 'Meds', 'Scrap'];
  const RES_DOT_CLASS = ['dot-water', 'dot-food', 'dot-fuel', 'dot-med', 'dot-scrap'];
  const ROLL_TIMEOUT_MS = 8000;
  // Mirrors ENC_MAX_ITEMS in Esp32HexMapCrawl.ino — the firmware holds at most
  // this many typed items for a whole scene, counting loot-table rolls the
  // enc_res event never carries.
  const ENC_MAX_ITEMS = 3;

  // ── State ───────────────────────────────────────────────────────
  let enc          = null;    // loaded encounter JSON
  let picked       = {};      // resolved placeholders for this visit
  let node         = null;    // current node
  let nodeKey      = '';
  let phase        = 'idle';  // idle | reading | rolling | leaving | ejected
  let pendingLoot  = [0, 0, 0, 0, 0];
  let keepLoot     = [0, 0, 0, 0, 0];  // how much of pendingLoot the player will actually take.
                               // Rises with pendingLoot as loot is won, then the tray's steppers
                               // trim it down; sent as `keep` on enc_bank. The difference is
                               // left behind on the way out — not dropped on the hex, just gone.
  let pendingItems = [];      // [{id, qty}] rolled from loot tables, banked on leave
  let pendingRecipes = 0;     // bitmask (bit id-1) of recipes learned this scene, banked on leave —
                               // a scene can walk through several nodes before ever banking, each
                               // granting a different recipe, so this accumulates rather than overwrites
  let terminal     = false;   // reached a node with no choices
  let pendingNext  = '';      // node key we move to if the pending roll succeeds
  let pendingHaz   = '';      // hazard copy shown if the pending roll fails
  let rollTimer    = 0;
  let confirmTimer = 0;
  let leaveArmed   = false;   // two-tap confirm when leaving would drop loot
  let trapMode     = false;   // a booby trap opened this scene under a step (traps.hpp):
                               // no walking away at the start, and leaving with nothing
                               // leaves it armed -- on your map
  let panelMode    = false;   // a tunnel control panel (a node carries "lights": true,
                               // tunnels.hpp): it stays put until its section is lit
  let assistN      = 0;       // teammates behind the next roll (ev enc_ast "n"); every roll
                               // spends them, so it resets on each enc_res
  let kaneCh       = 0;       // 1-5: one of Machine Gun Kane's chapters (the file's top-level
                               // "kane": {"chapter": n}, kane.hpp) -- walking away leaves it
                               // where it was, for later
  let vacPart      = 0;       // 1-4: one of VACANCY's parts (the file's top-level
                               // "vacancy": {"part": n}, vacancy.hpp) -- likewise
  let labPart      = 0;       // 1-4: one of RUNAWAY SCIENCE's parts (the file's top-level
                               // "lab": {"part": n}, lab.hpp) -- likewise
  let bonSec       = 0;       // 1-2: a section of ONE-HAND BONNEY (the file's top-level
                               // "bonney": {"section": n}, bonney.hpp) -- likewise
  let pendingPaid  = 0;       // points the choice being rolled costs ("cost": {"score": n}):
                               // the log says what Gaunt took (labPaid, lab.js)
  let fetchGen     = 0;       // bumped by closeEncounter(): a /enc fetch still in flight when
                               // the scene ends (dawn, a death, a reconnect) must not open it
  let fetchKey     = '';      // "biome/id" of the /enc fetch in flight ('' = none)
  let encKey       = '';      // "biome/id" of the scene on screen
  let quietEnd     = false;   // the enc_end on its way answers a load failure that already
                               // said so: no second toast (_encTakeQuietEnd, network.js)
  // The way out (enc_bank / enc_abort), sent again until the scene's own end
  // arrives. A refusal while the board was busy (a 20 ms G.mutex miss) is
  // silent -- the browser asks for no reply -- and closing on the click left
  // the survivor inside a scene with nothing on screen: no moves until dawn.
  let exitMsg      = null;
  let exitTries    = 0;
  let exitTimer    = 0;
  let exitGiveUp   = null;
  const EXIT_RETRY_MS = 1500, EXIT_TRIES = 4;
  // A fetch the board never answers does not fail on its own -- the browser
  // waits minutes -- so each try is cut off, and the next one asked.
  const ENC_FETCH_TRIES = 4, ENC_FETCH_RETRY_MS = 700, ENC_FETCH_TIMEOUT_MS = 6000;

  const me = () => (myId >= 0 ? players[myId] : null);

  // A scene is on screen, on its way (/enc in flight), or on its way out:
  // network.js asks for a lost scene only when none of these is true.
  globalThis._encSceneBusy = () => !!enc || !!fetchKey || !!exitMsg;

  function sendExit(msg, onGiveUp) {
    exitMsg = msg; exitTries = 1; exitGiveUp = onGiveUp ?? null;
    send(msg);
    armExitRetry();
  }
  function armExitRetry() {
    clearTimeout(exitTimer);
    exitTimer = setTimeout(() => {
      if (!exitMsg) return;
      // The state tick says we are out: the end event was simply missed.
      if (!me()?.enc) { exitDone(); if (enc) closeEncounter(); return; }
      if (exitTries < EXIT_TRIES) { exitTries++; send(exitMsg); armExitRetry(); return; }
      const giveUp = exitGiveUp;
      exitDone();
      giveUp?.();
    }, EXIT_RETRY_MS);
  }
  function exitDone() {
    clearTimeout(exitTimer);
    exitMsg = null; exitTries = 0; exitGiveUp = null;
  }

  // ── Helpers ─────────────────────────────────────────────────────
  // Recipe ids set in a bitmask (bit id-1 per recipe) — mirrors the server's
  // knownRecipes/pendingRecipes representation.
  function recipeIdsInMask(mask) {
    const out = [];
    for (let rid = 1; rid <= 32; rid++) if ((mask >>> (rid - 1)) & 1) out.push(rid);
    return out;
  }

  // What leaving right now would actually bank. This reads keepLoot, not
  // pendingLoot, so trimming the tray down to nothing correctly turns
  // "TAKE HAUL & LEAVE" back into a walk-out and stops the drop-haul confirm
  // from firing over loot the player already decided not to take.
  function haulCount() {
    return keepLoot.reduce((a, b) => a + b, 0) + pendingItems.reduce((a, it) => a + it.qty, 0)
      + recipeIdsInMask(pendingRecipes).length;
  }

  const keptTokens  = () => keepLoot.reduce((a, b) => a + b, 0);
  // Token load once the kept haul lands — per type, so canteen water is
  // priced the way the server's tokenLoad() will price it.
  const loadAfterHaul = () => {
    const p = me();
    if (!p) return keptTokens();
    const inv = p.inv ?? [];
    return tokenLoadOf(p, keepLoot.map((v, i) => (inv[i] || 0) + v));
  };
  const haulTrimmed = () => pendingLoot.some((v, i) => keepLoot[i] < v);

  // Pack size = archetype base (server `is`) + equipment slot bonuses, capped at
  // the 12-slot grid — the same mirror of effectiveInvSlots() that
  // renderInventory() uses in ui-items.js.
  function packSlots() {
    const p = me();
    if (!p) return 8;
    return packSlotsOf(p);   // server `is` is already base + equipment
  }

  function canBankHere() { return terminal || !!(node?.can_bank); }
  // An "escape" node is a way out, not the bottom of the place: leaving it
  // banks nothing and scores no full clear (a trap stays armed).
  function atEscape() { return terminal && !!node?.escape; }

  // Keys the server's encounter_engine.hpp understands, for the authoring warnings.
  const COST_KEYS = new Set(['ll', 'radiation', 'food', 'water', 'scrap', 'med', 'score']);
  const PEN_KEYS  = new Set(['ll', 'radiation', 'water', 'food', 'fuel', 'med', 'scrap', 'steal']);
  // Keys allowed on the hazard object itself (alongside `penalty`).
  const HAZ_KEYS  = new Set(['text', 'penalty', 'wound', 'wound_max', 'ends_encounter', 'dshingus']);

  // Encounter JSON is hand-authored; an unrecognised key would otherwise be
  // dropped in silence and the choice would simply be free. Say so instead.
  function warnUnknownKeys(obj, allowed, where) {
    for (const k of Object.keys(obj ?? {})) {
      if (!allowed.has(k)) console.warn(`[enc] ${where}: unknown key "${k}" — ignored`);
    }
  }

  function costChips(cost) {
    // cost keys: ll, radiation, food, water, scrap, med, score (points -- the
    // score itself, RUNAWAY SCIENCE's currency). A negative cost is a gain.
    const out = [];
    const spend = (v, name) => { if (v) out.push({ txt: `${v > 0 ? '−' : '+'}${Math.abs(v)} ${name}`, bad: v > 0 }); };
    spend(cost?.score, 'Points');
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
    if ((cost?.score ?? 0) > (p.sc ?? 0))                    return false;
    return true;
  }

  // "requires_item": N -- the choice only exists for a survivor who has item
  // N: WORN if it is equipment (the Dirty Glove), anywhere in the pack if not.
  // Mirrors playerHasForChoice() in inventory_items.hpp, which refuses it
  // server-side for anyone else. Hidden rather than greyed out: carrying the
  // right instrument is what opens the door, not something to be told about.
  // With "spends_item": true as well, it is a trade: a success hands one over
  // (spendItemForChoice() in inventory_items.hpp), and the pack arrives with
  // the targeted item_result act "enc_spend".
  function hasForChoice(ch) {
    const id = ch?.requires_item | 0;
    if (!id) return true;
    const p = me(); if (!p) return false;
    const item = getItemById(id);
    if (!item) return false;
    if (item.category === ITEM_CATEGORY.EQUIPMENT) return (p.eq ?? []).includes(id);
    return (p.it ?? []).some((t, i) => t === id && (p.iq?.[i] ?? 0) > 0);
  }

  // checkSkillMod() (survival_skills.hpp) rides every roll: -1 per major
  // wound on any skill, -1 per minor wound on ENDURE, +1 on ENDURE for the
  // Endurer (archetype 5; a seat is its archetype, handleMsg_pick). Leaving
  // it out showed a survivor with one major wound 58% on a 42% roll.
  const SK_ENDURE_SLOT = 4, ARCH_ENDURER = 5;
  function checkMod(p, skill) {
    const wnd = p?.wnd ?? [0, 0];
    let mod = -(wnd[1] | 0);
    if (skill === SK_ENDURE_SLOT) {
      mod -= (wnd[0] | 0);
      if (myId === ARCH_ENDURER) mod++;
    }
    return mod;
  }

  function oddsFor(choice) {
    const p = me();
    const dn = encComputeDN(p, choice.base_risk ?? 50, assistN);
    const sk = encSkillSlot(choice.skill ?? 0);
    const sv = (p?.sk?.[sk] ?? 0) + checkMod(p, sk);
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
  // Tray = a header row (HAUL · summary · pack load · minimize) over a body of
  // resource stepper cells, item/recipe chips and the carry line. On a phone
  // the body folds away behind the header (style.css "Phone"); desktop CSS
  // never hides it, so the toggle and summary only exist at phone width.
  //
  // Folded is remembered per browser: a player who folds the tray away keeps
  // it folded from scene to scene. Storage can be missing or throw (private
  // window, cleared site data), and then it simply starts open.
  const HAUL_FOLD_KEY = 'encHaulFolded';
  let haulFolded = false;
  try { haulFolded = localStorage.getItem(HAUL_FOLD_KEY) === '1'; } catch { /* no storage */ }

  function applyHaulFold() {
    haulEl.classList.toggle('folded', haulFolded);
    haulTog.setAttribute('aria-expanded', String(!haulFolded));
    haulTog.setAttribute('aria-label', haulFolded ? 'show haul' : 'minimize haul');
  }
  // The whole header row is the toggle on a phone (a thumb-wide strip), but
  // only while the toggle is actually displayed -- a desktop click on "HAUL"
  // must not quietly fold a tray that will be folded on the next phone visit.
  haulHead.addEventListener('click', () => {
    if (haulTog.hidden || !haulTog.getClientRects().length) return;
    haulFolded = !haulFolded;
    try { localStorage.setItem(HAUL_FOLD_KEY, haulFolded ? '1' : '0'); } catch { /* no storage */ }
    applyHaulFold();
  });
  applyHaulFold();

  // New loot while folded: flash the HAUL label so it is not missed.
  function bumpHaul() {
    haulEl.classList.remove('bump');
    void haulEl.offsetWidth;   // restart the animation
    haulEl.classList.add('bump');
  }

  // One line for the folded header: what is in the tray, by kind.
  function haulSummary() {
    const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;
    const parts  = [];
    const t  = keptTokens();
    const ni = pendingItems.reduce((a, it) => a + it.qty, 0);
    const nr = recipeIdsInMask(pendingRecipes).length;
    if (t || haulTrimmed()) parts.push(plural(t, 'token', 'tokens'));
    if (ni) parts.push(plural(ni, 'item', 'items'));
    if (nr) parts.push(plural(nr, 'recipe', 'recipes'));
    return parts.join(' · ');
  }

  function stepBtn(cls, glyph, label, enabled, onStep) {
    const b = el('button', cls, glyph);
    b.type = 'button';
    b.disabled = !enabled;
    b.setAttribute('aria-label', label);
    b.addEventListener('click', (e) => { e.preventDefault(); e.stopPropagation(); onStep(); });
    return b;
  }

  function renderHaul() {
    haulRes.innerHTML = '';
    haulItems.innerHTML = '';
    let any = false;
    pendingLoot.forEach((v, i) => {
      if (v <= 0) return;
      any = true;
      const keep = keepLoot[i];
      const name = RES_NAMES_ENC[i];
      const cell = el('div', 'enc-haul-cell'
        + (keep < v ? ' trimmed' : '') + (keep === 0 ? ' zeroed' : ''));

      const lbl = el('span', 'enc-haul-name');
      lbl.append(el('span', `res-dot ${RES_DOT_CLASS[i]}`), el('span', null, name));

      const count = el('span', 'enc-haul-count');
      count.appendChild(el('b', 'enc-haul-qty', `${keep}`));
      // The "/total" shows up only once this resource has been trimmed, so an
      // untouched tray reads as plain amounts.
      if (keep < v) count.appendChild(el('span', 'enc-haul-of', `/${v}`));

      cell.append(
        lbl,
        stepBtn('enc-haul-dec', '−', `take one less ${name}`, keep > 0,
          () => { if (keepLoot[i] > 0) { keepLoot[i]--; renderHaul(); renderLeave(); } }),
        count,
        stepBtn('enc-haul-inc', '+', `take one more ${name}`, keep < v,
          () => { if (keepLoot[i] < v) { keepLoot[i]++; renderHaul(); renderLeave(); } }));
      haulRes.appendChild(cell);
    });
    pendingItems.forEach(it => {
      any = true;
      const def  = typeof getItemById === 'function' ? getItemById(it.id) : null;
      const chip = el('span', 'enc-haul-chip');
      if (def) {
        const img = document.createElement('img');
        img.src = getItemIcon(it.id, 14); img.alt = ''; img.width = 14; img.height = 14;
        chip.appendChild(img);
      }
      if (it.qty > 1) chip.appendChild(el('span', 'enc-haul-qty', `${it.qty}×`));
      chip.appendChild(el('span', 'enc-haul-label', def?.name ?? `Item ${it.id}`));
      haulItems.appendChild(chip);
    });
    recipeIdsInMask(pendingRecipes).forEach(rid => {
      any = true;
      const rname = typeof getRecipeById === 'function' ? getRecipeById(rid)?.name : null;
      const chip = el('span', 'enc-haul-chip');
      chip.appendChild(el('span', 'enc-haul-label', `⚒ ${rname ?? `Recipe ${rid}`}`));
      haulItems.appendChild(chip);
    });
    // A trimmed-to-zero tray still has steppers to bring things back.
    haulEmpty.hidden = any;
    haulEl.classList.toggle('empty', !any);
    haulTog.hidden = !any;
    haulSum.textContent = any ? haulSummary() : '';
    renderCarry();
  }

  // The reason the steppers exist, made visible. Banking is the one path that
  // can push a survivor past their pack size — handleMsg_enc_bank clamps each
  // resource to 99, not to effectiveInvSlots() — and every token over the cap
  // costs 1 AP at dawn and refuses every later collect until it's dropped at
  // -10 pts each. A token left here costs only the 3 pts the bank would pay.
  // The pack load sits in the header so it stays in view with the tray folded;
  // the carry line under the steppers says what it costs and pays.
  function renderCarry() {
    const kept = keptTokens();
    haulCarry.innerHTML = '';
    haulPack.innerHTML  = '';
    if (!kept && !haulTrimmed()) { haulCarry.hidden = true; haulPack.hidden = true; return; }
    haulCarry.hidden = false;
    haulPack.hidden  = false;
    const after = loadAfterHaul();
    const cap   = packSlots();
    const over  = after > cap;
    haulPack.classList.toggle('over', over);
    haulPack.append('PACK ', el('b', null, `${after}/${cap}`));
    haulPack.title = over ? `Over your pack by ${after - cap}` : 'Pack load once this haul is banked';
    haulCarry.appendChild(el('span', 'pts', `+${kept * 3} pts`));
    if (over) haulCarry.appendChild(el('span', 'cap-over', ' · −1 AP at dawn'));
    if (haulTrimmed()) {
      const left = pendingLoot.reduce((a, v, i) => a + (v - keepLoot[i]), 0);
      haulCarry.appendChild(el('span', null, ` · ${left} left behind`));
    }
  }

  function renderLeave() {
    disarmLeave();
    leaveBtn.disabled = (phase === 'rolling' || phase === 'leaving');
    leaveBtn.classList.remove('primary', 'danger');
    const n = haulCount();

    if (phase === 'leaving') {
      leaveBtn.textContent = 'LEAVING…';
      leaveHint.textContent = 'Waiting for the way out.';
      return;
    }

    if (phase === 'ejected') {
      leaveBtn.textContent = 'LEAVE';
      leaveBtn.classList.add('primary');
      leaveHint.textContent = trapMode  ? 'It went off. It will not go off again.'
                            : panelMode ? 'The panel is still back there, in the dark. Try it again.'
                            : kaneCh    ? 'His story is still there. Come back to it.'
                            : vacPart   ? 'The sign is still on. Come back to it.'
                            : labPart   ? 'Gaunt will still be here. Obviously.'
                            : bonSec    ? 'Her fire will still be there. So will she.'
                            : 'The encounter is over.';
      return;
    }
    if (atEscape()) {
      leaveBtn.textContent = 'BACK AWAY';
      leaveBtn.classList.add('primary');
      leaveHint.textContent = trapMode
        ? 'It is still armed. You will know it when you see it again.'
        : 'You get out with nothing.';
      return;
    }
    if (trapMode && !terminal && !canBankHere()) {
      // The server would take an enc_abort here as the escape roll anyway
      // (handleMsg_enc_abort), but that choice should be the player's to see.
      leaveBtn.textContent = 'NO WAY OUT';
      leaveBtn.disabled = true;
      leaveHint.textContent = 'You cannot just walk off this one. Back out, or work it.';
      return;
    }
    if (terminal) {
      leaveBtn.textContent = n ? 'TAKE HAUL & LEAVE' : 'FINISH & LEAVE';
      leaveBtn.classList.add('primary');
      // A trap left with nothing is no clear: it stays armed and pays nothing
      // (handleMsg_enc_bank's took-anything test).
      leaveHint.textContent = (trapMode && !n)
        ? 'Take nothing and it stays armed. You will know it when you see it again.'
        : 'Nothing more here. Leaving now scores a full-clear bonus.';
    } else if (n && canBankHere()) {
      leaveBtn.textContent = 'TAKE HAUL & LEAVE';
      leaveBtn.classList.add('primary');
      leaveHint.textContent = trapMode
        ? 'Taking anything disarms it. Or push on for more.'
        : 'Pocket what you have, or push on for more.';
    } else if (n) {
      leaveBtn.textContent = 'LEAVE · DROPS HAUL';
      leaveBtn.classList.add('danger');
      leaveHint.textContent = 'You can’t carry loot out from here. Push on to secure it.';
      return;   // nothing banks here anyway — the trim note below would only confuse
    } else if (haulTrimmed() || trapMode) {
      // Trimmed the whole tray away. There is nothing left to bank, but this is
      // a deliberate choice rather than an empty-handed walk-out. In a trap it
      // is the same choice either way: the server leaves it armed, and known.
      leaveBtn.textContent = 'LEAVE IT ALL';
      leaveHint.textContent = trapMode
        ? 'Take nothing and it stays armed — on your map.'
        : 'You take nothing. What you leave behind stays behind.';
      return;
    } else {
      leaveBtn.textContent = 'WALK AWAY';
      leaveHint.textContent = panelMode
        ? 'Leave it dark for now. The panel will still be here.'
        : kaneCh ? 'Leave it for now. It will still be here.'
        : vacPart ? 'Leave it for now. The sign is very patient.'
        : labPart ? 'Leave it for now. Gaunt has nowhere to be.'
        : bonSec ? 'Leave it for now. Bonney is not going anywhere.'
        : 'Leave empty-handed. The place stays closed to you.';
      return;
    }
    // Trimmed tokens are not dropped on the hex and there is no refund — say so
    // plainly, on the one screen where the player can still change their mind.
    if (haulTrimmed()) leaveHint.textContent = 'What you leave behind stays behind.';
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
      choiceEl.appendChild(el('div', 'enc-terminal', node?.escape
        ? (trapMode ? 'You are out. It is still there.' : 'You are out.')
        : 'You’ve seen all there is to see here.'));
      return;
    }
    // Teammates on the hex who have backed this roll (enc_assist). The odds
    // on every card below already count them.
    if (assistN > 0)
      choiceEl.appendChild(el('div', 'enc-assist-note',
        `${assistN} ${assistN === 1 ? 'ally has' : 'allies have'} your back — next roll DN −${assistN * ENC_ASSIST_DN}`));
    // Numbered by what is on screen; the ci sent is still the choice's index
    // in the file (sendChoice looks it up), so hidden choices shift nothing.
    node.choices.filter(hasForChoice).forEach((ch, idx) => {
      const btn = el('button', 'enc-choice');
      btn.type = 'button';
      const affordable = canAfford(ch.cost);
      // "sure": paid for, never rolled (encRunChoice) -- no odds to show.
      const { pct } = ch.sure ? { pct: 100 } : oddsFor(ch);

      const num = el('span', 'enc-choice-num', `${idx + 1}`);
      const body = el('span', 'enc-choice-body');
      body.appendChild(el('span', 'enc-choice-label', resolveText(ch.label, picked)));

      const meta = el('span', 'enc-choice-meta');
      const odds = el('span', `enc-odds ${oddsClass(pct)}`);
      // 10-cell gauge, rounded up so a long shot still lights one cell
      const bar = el('span', 'enc-odds-bar');
      bar.style.setProperty('--p', Math.ceil(pct / 10) * 10);
      bar.setAttribute('aria-hidden', 'true');
      odds.appendChild(bar);
      odds.appendChild(el('b', '', ch.sure ? 'SURE THING' : `${pct}%`));
      odds.appendChild(el('span', '', ch.sure ? 'NO ROLL' : encSkillLabel(ch.skill ?? 0)));
      meta.appendChild(odds);
      costChips(ch.cost).forEach(c => meta.appendChild(el('span', `enc-cost ${c.bad ? 'bad' : 'good'}`, c.txt)));
      // "spends_item": a trade. The gated item goes on success only, so this
      // reads like a cost: what you hand over if it works.
      if (ch.spends_item && ch.requires_item) {
        const given = getItemById(ch.requires_item | 0);
        if (given) meta.appendChild(el('span', 'enc-cost bad', `−1 ${given.name}`));
      }
      if (!affordable) meta.appendChild(el('span', 'enc-cost cant', 'CAN’T AFFORD'));
      body.appendChild(meta);

      btn.appendChild(num);
      btn.appendChild(body);
      btn.disabled = !affordable || phase === 'rolling' || phase === 'leaving';
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
    pendingPaid = Math.max(0, cost.score | 0);

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
    // The roll spent whatever help was behind it. A "sure" choice is not
    // rolled, and the server keeps the help for the next one that is.
    if (!ev.sure) assistN = 0;
    if (!enc || phase !== 'rolling') return;
    clearTimeout(rollTimer);

    const rollTxt = ev.sure ? 'Paid. No roll.'
      : (ev.tot !== undefined && ev.dn !== undefined) ? `Rolled ${ev.tot} vs ${ev.dn}` : '';
    // Points are gone whichever way the roll went: the cost was taken first.
    const paid = pendingPaid;
    pendingPaid = 0;
    if (paid && typeof labPaid === 'function') labPaid(paid, labPart);

    if (ev.out) {
      const deltas = [];
      if (Array.isArray(ev.loot)) {
        ev.loot.forEach((v, i) => {
          if (v > 0) {
            // Newly won loot defaults to kept. The firmware clamps its own
            // pendingLoot to 99 per resource, so clamp here too or the steppers
            // would offer more than the server is actually holding.
            const was = pendingLoot[i];
            pendingLoot[i] = Math.min(99, was + v);
            keepLoot[i]    = Math.min(99, keepLoot[i] + (pendingLoot[i] - was));
            deltas.push({ txt: `+${v} ${RES_NAMES_ENC[i]}`, pos: true });
          }
        });
      }
      // Typed items the server granted: an explicit node "item" entry and/or a
      // loot-table roll (two at most per scene). The firmware caps the whole
      // scene at ENC_MAX_ITEMS — including loot-table rolls this event never
      // carries — so stop at the same ceiling rather than advertising items
      // enc_bank will never grant.
      [[ev.it, ev.iq], [ev.it2, ev.iq2]].forEach(([id, qty]) => {
        if (!id || !qty || pendingItems.length >= ENC_MAX_ITEMS) return;
        pendingItems.push({ id, qty });
        const def = typeof getItemById === 'function' ? getItemById(id) : null;
        deltas.push({ txt: `+${qty > 1 ? qty + '× ' : ''}${def?.name ?? 'Item'}`, pos: true });
      });
      if (ev.rec) {
        pendingRecipes |= (1 << (ev.rec - 1));
        const rname = typeof getRecipeById === 'function' ? getRecipeById(ev.rec)?.name : null;
        deltas.push({ txt: `Recipe: ${rname ?? ev.rec}`, pos: true });
      }
      if (paid) deltas.unshift({ txt: `−${paid} Points`, pos: false });
      const next = pendingNext;
      pendingNext = ''; pendingHaz = '';
      if (next && enc.nodes?.[next]) renderNode(next);
      else { terminal = true; phase = 'reading'; renderChoices(); renderHaul(); renderLeave(); }
      if (deltas.some((d) => d.pos)) bumpHaul();
      showResult({
        ok: true,
        verdict: 'YOU GET THROUGH',
        roll: rollTxt,
        deltas,
        note: deltas.some((d) => d.pos) ? 'Added to your haul.' : '',
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
      choiceEl.appendChild(el('div', 'enc-terminal bad', trapMode
        ? 'It goes off. It is spent now — and so, very nearly, are you.'
        : 'You’re driven out. Whatever you hadn’t pocketed is lost.'));
      pendingLoot = [0, 0, 0, 0, 0]; keepLoot = [0, 0, 0, 0, 0]; pendingItems = []; pendingRecipes = 0;
      renderHaul(); haulEmpty.textContent = 'lost'; renderLeave();
      showResult({ ok: false, verdict: trapMode ? 'IT GOES OFF' : 'DRIVEN OUT', roll: rollTxt, text: hazText, deltas });
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

  // Only these refuse the choice in flight (encRunChoice, network-msg-encounter.hpp;
  // encChoice in the mock). Any other error that lands mid-roll -- a move
  // refused with "Cannot move during encounter", a trade, an act -- is about
  // something else: treating it as a refusal rolled the panel back to the old
  // node while the server had already moved on, and every click after that
  // was "not open to you".
  const CHOICE_REFUSALS = ['That choice is not open to you', 'Cannot afford cost'];
  globalThis._onEncError = function(msg) {
    if (!enc || phase !== 'rolling') return false;  // not ours — let the caller fall back to a toast
    if (!CHOICE_REFUSALS.some(s => (msg || '').startsWith(s))) return false;
    clearTimeout(rollTimer);
    phase = 'reading';
    renderChoices();
    renderLeave();
    showResult({ ok: false, verdict: 'NOT POSSIBLE', text: msg || 'The server refused that choice.' });
    return true;
  };

  globalThis._onEncBank = function() { closeEncounter(); };

  // A teammate spent a token on our next roll (network.js _evEncAssist).
  globalThis._onEncAssist = function(ev) {
    assistN = Math.max(0, Math.min(ENC_ASSIST_CAP, ev.n | 0));
    if (enc && phase === 'reading' && !terminal) renderChoices();
  };

  globalThis._onEncEnd = function(ev) {
    // A hazard that ends the encounter already rendered its own "DRIVEN OUT"
    // screen — leave it up so the player can read what happened.
    if (ev?.reason === 'hazard' && phase === 'ejected') return;
    closeEncounter();
  };

  // ── Open / close ───────────────────────────────────────────────
  // `resume`: an enc_path "rs" -- the board re-sent a scene already under way
  // (handleMsg_enc_start). Its node, tray and allies are the board's, so the
  // panel opens where the survivor actually is, holding what they won.
  function openEncounter(json, trap = false, resume = null) {
    enc          = json;
    trapMode     = !!trap;
    panelMode    = !trapMode && Object.values(json.nodes ?? {}).some(n => n?.lights);
    kaneCh       = trapMode ? 0 : (json.kane?.chapter | 0);
    vacPart      = trapMode ? 0 : (json.vacancy?.part | 0);
    labPart      = trapMode ? 0 : (json.lab?.part | 0);
    bonSec       = trapMode ? 0 : (json.bonney?.section | 0);
    overlay.classList.toggle('trap', trapMode);
    picked       = pickPlaceholders(json.placeholders);
    // Bonney's {{apple}}: what is in Turnbull's mouth (bonney.js, from the enc_path).
    if (bonSec && typeof bonneyPlaceholders === 'function') Object.assign(picked, bonneyPlaceholders(bonSec));
    pendingLoot  = [0, 0, 0, 0, 0];
    keepLoot     = [0, 0, 0, 0, 0];
    pendingItems = [];
    pendingRecipes = 0;
    terminal     = false;
    pendingNext  = ''; pendingHaz = '';
    haulEmpty.textContent = 'nothing yet';
    hideResult();

    titleEl.textContent = resolveText(json.title || 'Encounter', picked);
    const p = me();
    // The board the player is actually on -- underground, p.q/p.r are the hatch.
    const cell = p ? (typeof myBoardCell === 'function' ? myBoardCell() : gameMap[p.r]?.[p.q]) : null;
    const tname = cell ? (TERRAIN[cell.terrain]?.name ?? '') : '';
    const kind  = trapMode ? 'TRAP' : panelMode ? 'CONTROL PANEL' : 'ENCOUNTER';
    kickerEl.textContent = kaneCh ? `MACHINE GUN KANE · CHAPTER ${kaneCh} OF 5`
                         : vacPart ? `VACANCY · ${['', 'THE SIGN', 'THE REGISTER', 'THE KEY', 'CHECK-OUT'][vacPart] || 'THE SIGN'}`
                         : labPart ? `RUNAWAY SCIENCE · ${['', 'THE SHARPS WARD', 'THE PANTRY', 'THE PATIENT', 'THE GRAFT'][labPart] || 'THE CLINIC'}`
                         : bonSec ? `ONE-HAND BONNEY · ${BONNEY_SECTION_NAMES[bonSec] || 'HER FIRE'}`
                         : tname ? `${kind} · ${tname.toUpperCase()}` : kind;

    if (resume) {
      const pl = Array.isArray(resume.pl) ? resume.pl : [];
      pendingLoot    = [0, 1, 2, 3, 4].map(i => Math.max(0, Math.min(99, pl[i] | 0)));
      keepLoot       = [...pendingLoot];
      pendingItems   = (Array.isArray(resume.pi) ? resume.pi : [])
        .filter(e => Array.isArray(e) && (e[0] | 0) > 0 && (e[1] | 0) > 0)
        .slice(0, ENC_MAX_ITEMS).map(([id, qty]) => ({ id: id | 0, qty: qty | 0 }));
      pendingRecipes = (resume.pr ?? 0) >>> 0;
      assistN        = Math.max(0, Math.min(ENC_ASSIST_CAP, resume.as | 0));
    }

    overlay.classList.add('open');
    overlay.style.display = '';
    const startKey = json.nodes?.[json.start_node] ? json.start_node : Object.keys(json.nodes ?? {})[0];
    const resumeKey = resume && json.nodes?.[resume.node] ? resume.node : '';
    if (resumeKey) renderNode(resumeKey);
    else if (startKey) {
      renderNode(startKey);
      // Past the end of the file (a choice whose destination is missing ends
      // the scene as a full clear, encResolveChoice): nothing left but to leave.
      if (resume?.tm) { terminal = true; renderChoices(); renderHaul(); renderLeave(); }
    }
    else { console.error('[ENC] encounter has no nodes', json); send({ t: 'enc_abort' }); closeEncounter(); }
    if (resume && haulCount()) bumpHaul();
    scrollEl.scrollTop = 0;
    leaveBtn.blur();
  }

  function closeEncounter() {
    clearTimeout(rollTimer);
    exitDone();
    fetchGen++;   // a /enc fetch still in flight belongs to a scene that is over
    fetchKey = ''; encKey = '';
    disarmLeave();
    overlay.classList.remove('open');
    overlay.style.display = 'none';
    enc = null; node = null; nodeKey = '';
    phase = 'idle';
    trapMode = false;
    panelMode = false;
    kaneCh = 0;
    vacPart = 0;
    labPart = 0;
    bonSec = 0;
    pendingPaid = 0;
    assistN = 0;
    overlay.classList.remove('trap');
    pendingLoot = [0, 0, 0, 0, 0]; keepLoot = [0, 0, 0, 0, 0]; pendingItems = []; pendingRecipes = 0;
    terminal = false; pendingNext = ''; pendingHaz = '';
    hideResult();
    choiceEl.innerHTML = ''; haulRes.innerHTML = ''; haulItems.innerHTML = '';
    haulEl.classList.remove('bump');
  }

  // Bonney's {{stolen}} arrives mid-scene (ev bonney "pinch", bonney.js): the
  // nodes still to come read it.
  globalThis._encSetPlaceholder = function(key, val) { if (enc) picked[key] = val; };

  // Called from network.js enc_path handler. `trap`: a booby trap opened this
  // scene under a step (traps.hpp) rather than the player walking into it.
  // Opened only if the scene is still the one asked for: closeEncounter()
  // bumps fetchGen, so a scene that ended while its file was in flight (dawn,
  // a death, a reconnect) no longer opens over whatever came next.
  globalThis._startEncounterFetch = function(biome, id, trap = false, resume = null) {
    const key = `${biome}/${id}`;
    // The same scene again -- a resent enc_path ("rs"), or one the socket
    // delivered twice -- while it is on screen or on its way: nothing to do.
    // Reopening would throw away the tray the panel is holding.
    if ((enc && encKey === key) || fetchKey === key) {
      console.log(`[ENC] enc_path for ${key} — already ${enc ? 'open' : 'loading'}`);
      return;
    }
    // A different scene than the one on screen: the board has moved on (a
    // missed enc_end). Its scene is the one that counts.
    if (enc) closeEncounter();
    const gen = ++fetchGen;
    fetchKey = key;
    const url = `/enc?biome=${encodeURIComponent(biome)}&id=${encodeURIComponent(id)}`;
    const giveUp = (e) => {
      fetchKey = '';
      console.error('[ENC] fetch failed, aborting:', e.message);
      // A trap cannot be declined: the server takes this enc_abort as the
      // escape roll (handleMsg_enc_abort), so say so rather than "blocked".
      if (trap) { showToast?.('⚠ Something clicks underfoot. You throw yourself back.'); sendExit({ t: 'enc_abort' }); return; }
      // Kane's chapters, VACANCY, the clinic and Bonney keep their place on
      // any abort; a plain place keeps it on "lf". Either way it is still
      // there, and network.js tries a plain place's door once more.
      showToast?.('⊙ The way in won’t open yet. (encounter failed to load — it is still there)');
      // "lf": the scene never showed, so the server keeps the place and the
      // threat clock. A plain enc_abort was taken as walking away from it,
      // and closed it for good.
      quietEnd = true;
      sendExit({ t: 'enc_abort', lf: 1 });
      globalThis._encRetryAfterLoadFail?.();
    };
    const attempt = (n) => {
      // Cut off a request the board never answers (a wedged socket): fetch()
      // alone would wait minutes, with the survivor held in a scene they
      // cannot see.
      const ctl = typeof AbortController === 'function' ? new AbortController() : null;
      const cut = ctl ? setTimeout(() => ctl.abort(), ENC_FETCH_TIMEOUT_MS) : 0;
      return fetch(url, ctl ? { signal: ctl.signal } : undefined)
        .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json(); })
        .finally(() => clearTimeout(cut))
        .then(json => {
          if (gen !== fetchGen) return;
          fetchKey = '';
          try { openEncounter(json, trap, resume); encKey = key; } catch (e) { closeEncounter(); giveUp(e); }
        }, e => {
          if (gen !== fetchGen) return;
          // A busy board drops the odd request (heap, a wedged socket): the
          // server has already opened this scene, so ask again before giving up.
          if (n < ENC_FETCH_TRIES) {
            console.warn(`[ENC] fetch ${n}/${ENC_FETCH_TRIES} failed (${e.message}), retrying`);
            setTimeout(() => { if (gen === fetchGen) attempt(n + 1); }, ENC_FETCH_RETRY_MS * n);
            return;
          }
          giveUp(e);
        });
    };
    attempt(1);
  };

  // Read-and-clear for _evEncEnd (network.js): this end answers a load
  // failure whose toast already said what happened.
  globalThis._encTakeQuietEnd = function() { const q = quietEnd; quietEnd = false; return q; };

  // ── Leave button ───────────────────────────────────────────────
  // The overlay closes on the server's answer -- enc_bank, or enc_end
  // (_onEncBank / _onEncEnd) -- not on the click. sendExit() asks again while
  // it waits; if it never hears, the scene comes back so LEAVE can be retried.
  function leaveWith(msg) {
    phase = 'leaving';
    setChoicesEnabled(false);
    renderLeave();
    sendExit(msg, () => {
      if (!enc || phase !== 'leaving') return;
      phase = 'reading';
      renderChoices();
      renderLeave();
      showResult({ ok: false, verdict: 'NO ANSWER', text: 'The way out didn’t open. Try again.' });
    });
  }

  leaveBtn.addEventListener('click', () => {
    if (phase === 'rolling' || phase === 'leaving' || !enc) return;
    if (phase === 'ejected') { closeEncounter(); return; }

    const n = haulCount();
    // `keep` is what we're asking to bank; the server clamps it to its own
    // pendingLoot[] and leaves the remainder behind.
    if (terminal || (n && canBankHere())) {
      leaveWith({ t: 'enc_bank', keep: [...keepLoot] });
      return;
    }

    if (n && !leaveArmed) {
      // Two-tap confirm: leaving here forfeits the haul.
      leaveArmed = true;
      leaveBtn.classList.add('armed');
      leaveBtn.textContent = 'DROP HAUL? TAP AGAIN';
      confirmTimer = setTimeout(() => renderLeave(), 3000);
      return;
    }
    leaveWith({ t: 'enc_abort' });
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
