/**
 * Event Handlers Module (Tier 2.1)
 * Modularizes event handling from the monolithic handleEvent() function.
 * Each handler is focused and testable, reducing cognitive load.
 *
 * Usage in engine.js:
 *   import { eventHandlers } from './event-handlers.js';
 *
 *   function handleEvent(ev) {
 *     const handler = eventHandlers[ev.k];
 *     if (handler) {
 *       handler(ev, { gameState, players, gameMap, myId, ... });
 *     } else {
 *       console.warn(`Unknown event type: ${ev.k}`);
 *     }
 *   }
 */

/**
 * Handle collection event (player picks up a resource).
 * Updates local gameMap and displays feedback to player.
 * @param {Object} ev - Event { k:'col', q, r, pid, res, amt }
 * @param {Object} ctx - Context { gameMap, players, myId, myVisionR, ... }
 */
function handleCollect(ev, ctx) {
  const me = ctx.myId >= 0 ? ctx.players[ctx.myId] : null;
  if (me && ctx.hexDistWrap(me.q, me.r, ev.q, ev.r) <= ctx.myVisionR) {
    if (ctx.gameMap[ev.r] && ctx.gameMap[ev.r][ev.q]) {
      ctx.gameMap[ev.r][ev.q] = {
        ...ctx.gameMap[ev.r][ev.q],
        resource: 0,
        amount: 0
      };
    }
  }

  const who = ev.pid === ctx.myId ? 'You' : (ctx.players[ev.pid]?.nm || `P${ev.pid}`);
  ctx.addLog(`<span class="log-col">${ctx.escHtml(who)} +${ev.amt}× ${ctx.RES_NAMES[ev.res]}</span>`);

  if (ev.pid === ctx.myId) {
    // Optimistically update inventory
    const idx = ev.res - 1;
    if (idx >= 0 && idx < 5) {
      ctx.players[ctx.myId].inv[idx] = (ctx.players[ctx.myId].inv[idx] ?? 0) + ev.amt;
    }
    ctx.showToast(`+${ev.amt} ${ctx.RES_NAMES[ev.res]}`);
    ctx.updateSidebar();
  }
}

/**
 * Handle resource spawn event (new resource appears on map).
 * @param {Object} ev - Event { k:'rsp', q, r, res, amt }
 * @param {Object} ctx - Context { gameMap, ... }
 */
function handleResourceSpawn(ev, ctx) {
  if (ctx.gameMap[ev.r] && ctx.gameMap[ev.r][ev.q]) {
    ctx.gameMap[ev.r][ev.q] = {
      ...ctx.gameMap[ev.r][ev.q],
      resource: ev.res,
      amount: ev.amt
    };
  }
}

/**
 * Handle movement event (player moves to new hex).
 * Updates position, footprints, radiation, exploration, and narration.
 * @param {Object} ev - Event { k:'mv', pid, q, r, mp, radd, exploD, ... }
 * @param {Object} ctx - Context { players, gameMap, myId, myVisionR, surveyedCells, footprintTimestamps, ... }
 */
function handleMovement(ev, ctx) {
  const pm = ctx.players[ev.pid];
  if (!pm) {
    console.warn(`Movement: invalid player ID ${ev.pid}`);
    return;
  }

  if (ev.mp !== undefined) {
    pm.mp = ev.mp;
    if (ev.pid === ctx.myId) ctx.updateSidebar();
  }

  pm.q = ev.q;
  pm.r = ev.r;

  // Clear surveyed cells on player move
  if (ev.pid === ctx.myId) ctx.surveyedCells.clear();

  // Record footprint for animation
  if (ctx.gameMap[ev.r]?.[ev.q]) {
    const c = ctx.gameMap[ev.r][ev.q];
    ctx.gameMap[ev.r][ev.q] = {
      ...c,
      footprints: (c.footprints || 0) | (1 << ev.pid)
    };
  }
  ctx.footprintTimestamps.set(`${ev.q}_${ev.r}_${ev.pid}`, Date.now());

  // Handle radiation exposure
  if (ev.radd && ev.radd > 0) {
    pm.rad = ev.rad ?? pm.rad;
    if (ev.pid === ctx.myId) {
      ctx.uiRad.val = pm.rad;
      ctx.showToast(`☢ +${ev.radd} Radiation (R:${pm.rad})`);
      ctx.addLog(`<span class="log-check-fail">☢ Entered rad zone +${ev.radd}R → R:${pm.rad}</span>`);
    }
  }

  // Exploration bonus
  if (ev.exploD && ev.exploD > 0 && ev.pid === ctx.myId) {
    ctx.addLog(`<span class="log-mv">⭐ New hex — +${ev.exploD} exploration pt</span>`);
  }

  // Narrate position for accessibility
  if (ev.pid === ctx.myId) {
    const TNAME = ['Open Scrub', 'Ash Dunes', 'Rust Forest', 'Marsh', 'Broken Urban',
                   'Flooded Ruins', 'Glass Fields', 'Rolling Hills', 'Mountain', 'Settlement', 'Nuke Crater', 'River Channel'];
    const cell = ctx.gameMap[ev.r]?.[ev.q];
    const tName = TNAME[cell?.terrain ?? 0] ?? 'Unknown';
    const shelterNote = cell?.shelter ? ' Shelter present.' : '';
    const exploNote = ev.exploD > 0 ? ` +${ev.exploD}pt.` : '';
    ctx.narrateState(`Moved to ${tName} at q:${ev.q},r:${ev.r}. MP:${ev.mp}.${shelterNote}${exploNote}`);
  }
}

/**
 * Handle player join event.
 * @param {Object} ev - Event { k:'join', pid }
 * @param {Object} ctx - Context { players, addLog, updateRestBubbles, ... }
 */
function handlePlayerJoin(ev, ctx) {
  if (ev.pid < 0 || ev.pid >= ctx.players.length) {
    console.warn(`Join: invalid player ID ${ev.pid}`);
    return;
  }
  ctx.players[ev.pid].on = true;
  ctx.addLog(`<span class="log-join">▶ Survivor ${ev.pid} appeared</span>`);
  ctx.updateRestBubbles();
}

/**
 * Handle player leave event.
 * @param {Object} ev - Event { k:'left', pid }
 * @param {Object} ctx - Context { players, addLog, updateRestBubbles, ... }
 */
function handlePlayerLeft(ev, ctx) {
  if (ev.pid < 0 || ev.pid >= ctx.players.length) {
    console.warn(`Left: invalid player ID ${ev.pid}`);
    return;
  }
  ctx.players[ev.pid].on = false;
  ctx.addLog(`<span class="log-left">◀ Survivor ${ev.pid} vanished</span>`);
  ctx.updateRestBubbles();
}

/**
 * Handle downed event (player reaches 0 LL and awaits dawn resurrection).
 * @param {Object} ev - Event { k:'downed', pid, ll }
 * @param {Object} ctx - Context { players, myId, addLog, narrateState, ... }
 */
function handleDowned(ev, ctx) {
  if (ev.pid < 0 || ev.pid >= ctx.players.length) {
    console.warn(`Downed: invalid player ID ${ev.pid}`);
    return;
  }

  const pm = ctx.players[ev.pid];
  pm.ll = 0;
  const who = ev.pid === ctx.myId ? 'You' : (pm.nm || `P${ev.pid}`);
  ctx.addLog(`<span class="log-downed">◈ ${ctx.escHtml(who)} is downed</span>`);

  if (ev.pid === ctx.myId) {
    ctx.narrateState('You have been downed. Awaiting dawn...');
  }
}

/**
 * Handle regen event (dawn recovery from downed state).
 * @param {Object} ev - Event { k:'regen', pid }
 * @param {Object} ctx - Context { players, myId, addLog, ... }
 */
function handleRegen(ev, ctx) {
  if (ev.pid < 0 || ev.pid >= ctx.players.length) {
    console.warn(`Regen: invalid player ID ${ev.pid}`);
    return;
  }

  const pm = ctx.players[ev.pid];
  pm.ll = 0; // Restored but still at 0 LL until actions taken
  const who = ev.pid === ctx.myId ? 'You' : (pm.nm || `P${ev.pid}`);
  ctx.addLog(`<span class="log-regen">✦ ${ctx.escHtml(who)} recovered at dawn</span>`);
}

/**
 * Handle dawn event (new day transition).
 * Updates threat clock, day counter, and triggers special effects.
 * @param {Object} ev - Event { k:'dawn', mp, ... }
 * @param {Object} ctx - Context { gameState, players, myId, uiMaxMP, updateSidebar, showToast, ... }
 */
function handleDawn(ev, ctx) {
  if (ev.pid === ctx.myId) {
    // Update max MP and trigger night fade effect
    const maxMP = ev.mp;
    ctx.uiMaxMP.val = maxMP;
    ctx.displayMP = maxMP;
    ctx.nightFade = ctx.NIGHT_FADE_INIT;
  }

  const pm = ctx.players[ev.pid];
  if (pm) {
    pm.ll = ev.ll;
    pm.mp = ev.mp;
    pm.rest = false;
  }

  ctx.gameState.dc = ev.dc ?? ctx.gameState.dc; // Update day counter
  ctx.gameState.tc = ev.tc ?? ctx.gameState.tc; // Update threat clock

  ctx.showToast('☀ DAWN');
  ctx.updateSidebar();
}

/**
 * Handle incoming trade offer (another player wants to trade).
 * Logs the offer for all; opens the accept/decline overlay for the target only.
 * @param {Object} ev - Event { k:'trd_off', from, to, give:[5], want:[5] }
 * @param {Object} ctx - Context { players, myId, addLog, escHtml, showToast }
 */
function handleTradeOffer(ev, ctx) {
  const fromName = ctx.players[ev.from]?.nm || 'P' + ev.from;
  const toName   = ctx.players[ev.to]?.nm   || 'P' + ev.to;
  const giveTxt  = ev.give.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join(' ') || '\u2014';
  const wantTxt  = ev.want.map((v, i) => v > 0 ? RES_SHORT[i] + '\u00d7' + v : null).filter(Boolean).join(' ') || '\u2014';
  ctx.addLog(`<span class="log-col">\u21C4 ${ctx.escHtml(fromName)} offers ${ctx.escHtml(toName)}: ${giveTxt} for ${wantTxt}</span>`);
  if (ev.to === ctx.myId) {
    window._openTradeOffer?.(ev.from, ev.give, ev.want);
  }
}

/**
 * Handle trade result (accepted, declined, or expired).
 * @param {Object} ev - Event { k:'trd_res', from, to, res }  res: 1=accepted 2=declined 3=expired
 * @param {Object} ctx - Context { players, myId, addLog, escHtml, showToast, updateSidebar }
 */
function handleTradeResult(ev, ctx) {
  const fromName    = ctx.players[ev.from]?.nm || 'P' + ev.from;
  const toName      = ctx.players[ev.to]?.nm   || 'P' + ev.to;
  const LABELS      = ['', 'ACCEPTED', 'DECLINED', 'EXPIRED'];
  const label       = LABELS[ev.res] ?? 'UNKNOWN';
  const cls         = ev.res === 1 ? 'log-col' : 'log-check-fail';
  ctx.addLog(`<span class="${cls}">\u21C4 Trade ${label}: ${ctx.escHtml(fromName)} \u2194 ${ctx.escHtml(toName)}</span>`);
  if (ev.from === ctx.myId || ev.to === ctx.myId) {
    ctx.showToast('\u21C4 Trade ' + label);
    window._closeTradeOverlay?.();
    ctx.updateSidebar();
  }
}

/**
 * All event handlers by type.
 * Add new handlers here as event types are added.
 */
const eventHandlers = {
  col:     handleCollect,
  rsp:     handleResourceSpawn,
  mv:      handleMovement,
  join:    handlePlayerJoin,
  left:    handlePlayerLeft,
  downed:  handleDowned,
  regen:   handleRegen,
  dawn:    handleDawn,
  trd_off: handleTradeOffer,
  trd_res: handleTradeResult,
  // Additional handlers would be added here:
  // nm: handleNameChange,
  // chk: handleCheck,
  // act: handleAction,
  // surv: handleSurvey,
};
