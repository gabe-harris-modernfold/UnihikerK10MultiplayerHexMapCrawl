// ── The observer screen ──────────────────────────────────────────
// A full-screen page you load on a TV and leave running. It watches a live
// game over GET /state, picks whose story to tell, and narrates it.
//
// See docs/observer-screen-spec.md. Three things there shape everything here:
//
//  1. THE OBSERVER NEVER OPENS A WEBSOCKET. handleConnect() in
//     network-session.hpp hands every /ws client a PLAYER slot and there are
//     six. A spectator on the socket eats a seat and draws itself on
//     everyone's map. /state is read-only, CORS-open, and consumes nothing.
//  2. /state IS SNAPSHOTS, NOT EVENTS. Everything interesting is a difference
//     between two of them, so this file carries a diff engine. That is not
//     purely a workaround: deltas are what a commentator talks about anyway.
//  3. ENCOUNTERS ARE THE STORY. Everything else on the board is weather and
//     arithmetic. The encounter is the only place a survivor makes a decision
//     an audience can second-guess, so the scene is the spine and the
//     survival meters are the consequences it leaves behind.
//
// Loads game-data.js first for ADMIRED / TERRAIN / WEATHER_PHASE_NAMES — the
// board of the admired is 36 finished one-liners in exactly this voice and
// re-writing them would be strictly worse. NONE of these files belong in
// data/web-assets.json: that is the game SPA's bundle order, and adding the
// observer to it would load a spectator screen into every player's client.
(function (root) {
  'use strict';
  const OBS = root.OBS || (root.OBS = {});
  const D = root.document;

  // ── Polling contract ──────────────────────────────────────────
  const POLL_MS      = 1000;    // below the 100 ms game tick, above what the board minds
  const BACKOFF_MIN  = 2500;    // the board wedges under HTTP load; do not pile on
  const BACKOFF_MAX  = 8000;
  const STALL_MS     = 6000;    // abort rather than let a dead socket hold the slot
  const DAY_TICKS    = 3000;    // matches Esp32HexMapCrawl.ino — the time-of-day arc
  const SAMPLES_MAX  = 30;      // shadow-roster window, per player
  const TC_MAX       = 20;      // threat clock ceiling, escalating at 5/9/13/17
  const TC_STEPS     = [5, 9, 13, 17];

  // ── Director knobs ────────────────────────────────────────────
  const MIN_DWELL_MS = 16000;   // below this it is unwatchable
  const MAX_DWELL_MS = 40000;   // force a cut so it never stares (suspended in a scene)
  const CUT_FLOOR_MS = 4000;    // no cut within 4 s of a cut, whatever happens
  const CUT_MARGIN   = 1.20;    // a challenger needs 20 % more heat, or the camera oscillates
  const PHASE_DEBOUNCE = 3;     // the half-the-board threshold must hold 3 polls running

  // ── Narrator pacing ───────────────────────────────────────────
  const TICKER_MS    = 4000;
  const HEADLINE_MS  = 8000;
  const HEADLINE_DEATH_MS  = 12000;   // the death is the set-piece; it earns the hold
  const HEADLINE_PASSED_MS = 4000;    // and nothing else on screen while it is up
  const STALE_MID_MS = 20000;         // drop unread mid lines rather than narrate the past
  const SCENE_GRACE_MS = 3000;        // how long a line will wait on its /enc fetch
  const REPEAT_WINDOW_MS = 60000;     // nothing is said twice inside a minute
  const STALE_HIGH_MS = 45000;        // a high line waits longer, but not forever
  const PENDING_MAX  = 24;            // ceiling on the unread queue
  const TICKER_KEEP  = 40;
  const TICKER_SHOW  = 4;             // a cut carries at most four lines; show that many
  const RAIL_MAX     = 16;            // living always kept; oldest dead drop off first

  const SEV_W = { low: 0.15, mid: 0.5, high: 1, max: 1.6 };

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const esc = (s) => String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

  // game-data.js is a classic script whose top-level `const`s land in the
  // GLOBAL LEXICAL environment, not on window — so they are reachable by bare
  // name from here and NOT as root.TERRAIN. Bound once, defensively, so a
  // missing game-data.js degrades instead of throwing on every poll.
  const GD = {
    TERRAIN: (typeof TERRAIN !== 'undefined') ? TERRAIN : [],
    ADMIRED: (typeof ADMIRED !== 'undefined') ? ADMIRED : [],
    WEATHER: (typeof WEATHER_PHASE_NAMES !== 'undefined') ? WEATHER_PHASE_NAMES : [],
    admiredPassed: (typeof admiredPassed === 'function') ? admiredPassed : null,
    admiredNextAbove: (typeof admiredNextAbove === 'function') ? admiredNextAbove : null,
  };
  const terrainOf = (t) => GD.TERRAIN[t] || null;
  const terrainName = (t) => { const x = terrainOf(t); return x ? x.name : 'ground'; };
  const weatherName = (w) => GD.WEATHER[w] || 'CLEAR';

  // ══ State ══════════════════════════════════════════════════════
  const ST = {
    host: '', feed: null,
    prev: null, snap: null,
    status: 'connecting', statusText: 'connecting',
    pollErrors: 0, polls: 0, backoff: 0,
    events: [],           // this poll's derived events
    recent: [],           // last 60 s of derived events, for heat
    startedAt: Date.now(),
  };

  // ══════════════════════════════════════════════════════════════
  // normalise — one shape for the rest of the file, so the synthetic feed is
  // a drop-in replacement for fetch and the render stays pure.
  // ══════════════════════════════════════════════════════════════
  function normalise(raw, t) {
    const players = [];
    const byKey = new Map();
    for (const p of (raw.players || [])) {
      const np = {
        // pid ALONE IS NOT AN IDENTITY. EVT_DOWNED clears connected, zeroes
        // wsClientId and hands the slot straight back to the lobby for
        // re-pick, so pid 3 on two consecutive polls can be two different
        // people. pid + connectMs is the pair that survives that.
        key: p.pid + ':' + (p.connectMs | 0),
        pid: p.pid | 0, connectMs: p.connectMs | 0,
        conn: !!p.conn,
        name: p.name || ('P' + p.pid), arch: p.arch | 0, archName: p.archName || '?',
        q: p.q | 0, r: p.r | 0,
        ll: p.ll | 0, llCap: Math.max(1, p.llCap | 0),
        food: p.food | 0, water: p.water | 0, rad: p.rad | 0, mp: p.mp | 0,
        wounds: [(p.wounds || [0, 0])[0] | 0, (p.wounds || [0, 0])[1] | 0],
        resting: !!p.resting,
        score: p.score | 0, steps: p.steps | 0,
        inv: (p.inv || [0, 0, 0, 0, 0]).map((n) => n | 0),
        invType: (p.invType || []).map((n) => n | 0),
        invQty: (p.invQty || []).map((n) => n | 0),
        equip: (p.equip || []).map((n) => n | 0),
        skills: (p.skills || []).map((n) => n | 0),
        encActive: !!p.encActive,
        encQ: p.encQ | 0, encR: p.encR | 0,
        encNode: p.encNode || '',
        // The three lines added to game-server.hpp for this screen. Without
        // them every scene is anonymous and the observer narrates "she is in
        // a building" instead of the Gutted Pharmacy.
        encBiome: p.encBiome || '', encId: p.encId == null ? -1 : (p.encId | 0),
        encCanBank: !!p.encCanBank,
        encLoot: (p.encLoot || [0, 0, 0, 0, 0]).map((n) => n | 0),
      };
      np.llFrac = np.ll / np.llCap;
      np.invTotal = np.inv.reduce((a, b) => a + b, 0);
      players.push(np);
      if (np.conn) byKey.set(np.key, np);
    }
    return {
      t,
      day: raw.day | 0, dayTick: raw.dayTick | 0, tickId: raw.tickId | 0,
      tc: raw.tc | 0, weather: raw.weather | 0,
      connected: raw.connected | 0,
      mem: raw.mem || {}, map: raw.map || {},
      // Per-terrain and per-shelter art variant counts. Same "vc"/"sv" the
      // lobby and sync messages carry; absent on firmware older than the
      // /state change, in which case the disk falls back to flat colour.
      vc: raw.vc || null, sv: raw.sv || null,
      view: raw.view || null,
      players, byKey,
      live: players.filter((p) => p.conn),
    };
  }

  // ══════════════════════════════════════════════════════════════
  // The shadow roster
  //
  // /state cannot tell a death from a quit: EVT_DOWNED clears `connected`
  // and the slot goes back to the lobby, which is exactly what a disconnect
  // looks like. So we keep our own record — the last 30 samples per player, a
  // running tally, and the encounter debt ledger. The samples infer the cause
  // of death; the ledger is what lets the obituary name a room.
  // ══════════════════════════════════════════════════════════════
  const Roster = {
    entries: new Map(),   // key -> entry

    entry(p) {
      let e = this.entries.get(p.key);
      if (!e) {
        e = {
          key: p.key, pid: p.pid, connectMs: p.connectMs,
          name: p.name, archName: p.archName, arch: p.arch,
          firstSeen: Date.now(), lastSeen: Date.now(), firstDay: 0,
          samples: [],
          // Encounters are not just beats, they are the CAUSES every later
          // beat pays off. Four LL bought in a pharmacy on day 6 is what
          // kills somebody in a rainstorm on day 9, and without this the
          // decline is a gauge moving instead of a debt coming due.
          ledger: [],
          tally: { steps: 0, scoreGain: 0, llLost: 0, encEntered: 0, encBanked: 0, encLost: 0, encAborted: 0, days: 0 },
          camera: { cuts: 0, lastSubjectAt: 0, totalMs: 0 },
          peakScore: 0,
          gone: false, goneAt: 0, cause: null, obituary: '',
          last: null,
        };
        this.entries.set(p.key, e);
      }
      return e;
    },

    push(snap) {
      for (const p of snap.live) {
        const e = this.entry(p);
        if (!e.firstDay) e.firstDay = snap.day;
        e.name = p.name; e.archName = p.archName;
        e.lastSeen = snap.t;
        e.last = p;
        e.peakScore = Math.max(e.peakScore, p.score);
        e.tally.steps = p.steps;
        e.tally.days = Math.max(0, snap.day - e.firstDay);
        e.samples.push({
          t: snap.t, day: snap.day,
          ll: p.ll, llCap: p.llCap, llFrac: p.llFrac,
          food: p.food, water: p.water, rad: p.rad,
          wounds: [p.wounds[0], p.wounds[1]],
          score: p.score, steps: p.steps,
          encActive: p.encActive, encBiome: p.encBiome, encId: p.encId, encNode: p.encNode,
          q: p.q, r: p.r,
        });
        if (e.samples.length > SAMPLES_MAX) e.samples.shift();
      }
    },

    // A player only stops being on the board here, never in the DOM: the
    // board filling up with the dead is the arc, and it is the only element
    // on the screen that moves one way.
    markGone(key, snap) {
      const e = this.entries.get(key);
      if (!e || e.gone) return null;
      e.gone = true; e.goneAt = snap.t;
      e.cause = causeOfDeath(e);
      e.obituary = writeObituary(e, snap);
      return e;
    },

    all() { return Array.from(this.entries.values()); },
    live() { return this.all().filter((e) => !e.gone); },
    dead() { return this.all().filter((e) => e.gone); },
    byKey(k) { return this.entries.get(k) || null; },

    // Score gained in the last `ms`, from the sample window.
    scoreVel(e, ms, now) {
      const w = e.samples.filter((s) => now - s.t <= ms);
      if (w.length < 2) return 0;
      return Math.max(0, w[w.length - 1].score - w[0].score);
    },
    // LL LOST in the last `ms`, as a fraction of cap — the reaping's engine.
    llVelDown(e, ms, now) {
      const w = e.samples.filter((s) => now - s.t <= ms);
      if (w.length < 2) return 0;
      const drop = w[0].llFrac - w[w.length - 1].llFrac;
      return clamp(drop, 0, 1);
    },
  };

  // ── Bookkeeping, for everybody, on and off camera ─────────────
  // The debt ledger and the tallies are ROSTER state, not narration. They
  // used to be written inside the narrator, which only ever speaks about the
  // camera subject — so a survivor who took four off in a pharmacy while the
  // camera was elsewhere reached their own obituary with an empty ledger and
  // no room to name. Encounters are the CAUSES every later beat pays off;
  // they have to be recorded whether or not anybody was watching.
  function recordEvents(events, snap) {
    for (const e of events) {
      const entry = e.key ? Roster.byKey(e.key) : null;
      if (!entry || !entry.last) continue;
      switch (e.kind) {
        case 'THRESHOLD':
          entry.tally.encEntered++;
          break;
        case 'PRICE': {
          // The ledger records FACTS, not prose: which file, which room, what
          // it cost. Names and hazard text are resolved when something asks
          // for them (ledgerScene/ledgerHazard), because /enc may not have
          // landed yet when the damage lands — and an entry that captured
          // "somewhere" at write time would poison the obituary permanently.
          entry.ledger.push({
            day: snap.day, biome: entry.last.encBiome, id: entry.last.encId,
            node: e.data.node,
            ll: e.data.ll, rad: e.data.rad,
            wounds: e.data.wounds.slice(), res: e.data.res.slice(),
            loot: entry.last.encLoot.slice(),
          });
          entry.tally.llLost += e.data.ll;
          break;
        }
        case 'EXIT':
          if (e.data.reason === 'banked') entry.tally.encBanked++;
          else if (e.data.reason === 'abort') entry.tally.encAborted++;
          else if (e.data.reason === 'hazard') entry.tally.encLost++;
          break;
        default: break;
      }
    }
  }

  // Ledger readers. Polling cannot see the roll, only its aftermath — but
  // every hazard reachable from the node they were standing on has a written
  // signature, so the damage identifies which one fired and the narrator can
  // use the file's own prose instead of a template.
  const ledgerScene = (d) => (d ? OBS.Scene.title(OBS.Scene.get(d.biome, d.id)) : 'somewhere');
  const ledgerRoom = (d) => (d ? OBS.Scene.roomName(d.node) : 'room');
  const ledgerHazard = (d) => (d ? OBS.Scene.matchHazard(OBS.Scene.get(d.biome, d.id), d.node, d) : null);

  // ── Cause of death ────────────────────────────────────────────
  // First match wins, and THE HEALTHY CHECK RUNS FIRST. It sat sixth in the
  // first draft of the spec, which let a quitter who happened to be
  // mid-encounter collect a hero's eulogy. Nothing cheapens a death board
  // faster than burying somebody whose battery died.
  function causeOfDeath(e) {
    const s = e.samples;
    if (!s.length) return 'UNKNOWN';
    const last = s[s.length - 1];
    if (last.llFrac >= 0.6) return 'WALKED_OUT';
    if (last.encActive) return 'SCENE';
    const w3 = s.slice(-3);
    if (w3.length >= 2 && w3[w3.length - 1].wounds[1] > w3[0].wounds[1]) return 'TEETH';
    const w5 = s.slice(-5);
    if (w5.filter((x) => x.food === 0).length >= 3) return 'STARVED';
    if (w5.filter((x) => x.water === 0).length >= 3) return 'THIRST';
    if (s[s.length - 1].rad > s[0].rad) return 'GLOW';
    return 'UNKNOWN';
  }

  // Whatever rule fired, the obituary also reads the debt ledger: the scene
  // that started the fall gets named even when thirst finished the job. That
  // is the difference between a death and a story.
  function writeObituary(e, snap) {
    const last = e.samples[e.samples.length - 1] || {};
    const debt = e.ledger[e.ledger.length - 1] || null;
    const ctx = {
      name: e.name, arch: e.archName,
      day: last.day == null ? snap.day : last.day,
      score: e.peakScore, n: last.ll == null ? 0 : last.ll,
      terrain: 'wasteland',
      scene: ledgerScene(debt),
      room: ledgerRoom(debt),
      haul: debt ? OBS.Scene.haulText(debt.loot) : 'nothing',
      item: '', risk: '',
    };
    // Died on the threshold, before anything had cost them anything: the
    // ledger is empty but the last sample still knows which door they were
    // standing in, and naming it is the entire point of the rule.
    if (e.cause === 'SCENE' && last.encActive) {
      ctx.scene = OBS.Scene.title(OBS.Scene.get(last.encBiome, last.encId));
      ctx.room = OBS.Scene.roomName(last.encNode);
    }
    let line = Narrator.pickFlat('OBIT_' + e.cause, e.key, ctx);
    if (!line) line = Narrator.pickFlat('OBIT_UNKNOWN', e.key, ctx);
    // Rules 3-6 name the scene that started the decline. Rule 7 does not need
    // help: "cause recorded as the wasteland" is honest and it is the funniest
    // line on the page.
    if (debt && debt.ll > 0 && e.cause !== 'SCENE' && e.cause !== 'WALKED_OUT' && e.cause !== 'UNKNOWN') {
      line += ' ' + ledgerScene(debt).toLowerCase() + ' took ' + debt.ll
            + ' off on day ' + debt.day + '. paid it back ever since.';
    }
    return line;
  }

  // ══════════════════════════════════════════════════════════════
  // The diff engine
  //
  // Each entry emits a DerivedEvent { kind, key, pid, sev, data }. Severity
  // drives both the narrator's bank and the director's interrupt logic.
  // `low` exists FOR THE RENDER, NOT THE VOICE — those move a meter on a card
  // and say nothing.
  // ══════════════════════════════════════════════════════════════
  function ev(out, kind, p, sev, data) {
    out.push({ kind, key: p ? p.key : '', pid: p ? p.pid : -1, sev, data: data || {}, t: Date.now() });
  }

  function diff(prev, next) {
    const out = [];
    if (!prev) return out;

    // The threat clock is the one thing that separates an abort from a
    // hazard end — see readExit(). Both are computed once here because both
    // are board-wide facts, not per-player ones.
    const tcRose = next.tc > prev.tc;
    const anyThreshold = next.players.some(
      (p) => p.encActive && !(prev.byKey.get(p.key) || {}).encActive);

    // ── World ────────────────────────────────────────────────
    if (next.day > prev.day) ev(out, 'DAWN', null, 'mid', { day: next.day });
    if (next.weather !== prev.weather) {
      ev(out, 'WEATHER_TURN', null, 'mid', { weather: next.weather, name: weatherName(next.weather) });
    }
    for (const step of TC_STEPS) {
      if (prev.tc < step && next.tc >= step) ev(out, 'ESCALATION', null, 'high', { tc: next.tc, step });
    }

    // ── Disappearances ───────────────────────────────────────
    // Walked over the PREVIOUS snapshot's connected roster, not the current
    // array, because a slot that empties does not necessarily keep its
    // connectMs — the mock forgets the player outright and reports 0, and a
    // recycled slot comes back with a new one. Either way the old key simply
    // stops appearing, which is the only signal that survives both.
    //
    // Death and quit look IDENTICAL from here: EVT_DOWNED clears `connected`
    // and hands the slot back to the lobby, exactly like a dropped socket.
    // The shadow roster decides which it was; this event only says the slot
    // went quiet.
    for (const [key, o] of prev.byKey) {
      if (!next.byKey.has(key)) ev(out, 'VANISHED', o, 'max', {});
    }

    // ── Per player ───────────────────────────────────────────
    for (const p of next.live) {
      const o = prev.byKey.get(p.key);

      if (!o) {
        // New key: a fresh arrival, or a slot recycled under a new
        // connectMs. Either way the shadow roster has never met this
        // survivor, and a respawn is never confused for a recovery.
        if (prev.players.length) ev(out, 'ARRIVED', p, 'mid', {});
        continue;
      }

      const dLL = o.ll - p.ll;                 // positive = lost
      const dRad = p.rad - o.rad;
      const dWnd = [p.wounds[0] - o.wounds[0], p.wounds[1] - o.wounds[1]];
      const dRes = p.inv.map((n, i) => n - o.inv[i]);
      const dScore = p.score - o.score;

      // ── The scene, which is the spine ──────────────────────
      if (p.encActive && !o.encActive) {
        ev(out, 'THRESHOLD', p, 'max', {
          biome: p.encBiome, id: p.encId, node: p.encNode, loot: p.encLoot.slice(),
        });
      } else if (p.encActive && o.encActive) {
        if (p.encNode !== o.encNode) {
          ev(out, 'DOOR', p, 'high', { from: o.encNode, node: p.encNode });
        }
        if (dLL > 0 || dWnd[0] > 0 || dWnd[1] > 0 || dRad > 0) {
          // A failed choice with ends_encounter false leaves encNode
          // UNCHANGED and drops LL. That is its own beat: still in there, and
          // worse. Match against the node they were standing on.
          ev(out, 'PRICE', p, 'high', {
            node: o.encNode, ll: dLL, rad: dRad, wounds: dWnd,
            res: dRes.map((n) => (n < 0 ? -n : 0)),
          });
        }
        if (p.encCanBank && !o.encCanBank) {
          ev(out, 'CAN_LEAVE', p, 'high', { loot: p.encLoot.slice(), node: p.encNode });
        }
        const grew = p.encLoot.some((n, i) => n > o.encLoot[i]);
        if (grew) ev(out, 'DEEPER', p, 'mid', { loot: p.encLoot.slice() });
      } else if (!p.encActive && o.encActive) {
        ev(out, 'EXIT', p, 'high', readExit(o, p, prev, next, dRes, dScore, dLL,
                                            tcRose && !anyThreshold));
      }

      // ── Survival: the consequences ─────────────────────────
      if (p.encActive) {
        // Inside a scene the meters are the scene's doing; PRICE already has
        // them and a second line about the same damage is noise.
      } else {
        if (dWnd[1] > 0) ev(out, 'MAULED', p, 'high', { n: p.wounds[1] });
        if (dLL >= 3)      ev(out, 'HURT_BAD', p, 'high', { n: p.ll, lost: dLL });
        else if (dLL > 0)  ev(out, 'HURT', p, 'mid', { n: p.ll, lost: dLL });
        else if (dLL < 0)  ev(out, 'PATCHED', p, 'low', { n: p.ll });
        if (dRad > 0)      ev(out, 'GLOW', p, 'mid', { n: p.rad });
      }
      if (crossedDown(o.llFrac, p.llFrac, 0.5) || crossedDown(o.llFrac, p.llFrac, 0.25)) {
        ev(out, 'FAILING', p, 'high', { n: p.ll, frac: p.llFrac });
      }
      if (crossedDown(o.food, p.food, 4) || crossedDown(o.food, p.food, 1)) {
        ev(out, 'HUNGER', p, 'mid', { n: p.food });
      }
      if (crossedDown(o.water, p.water, 3) || crossedDown(o.water, p.water, 1)) {
        ev(out, 'THIRST', p, 'mid', { n: p.water });
      }
      if (dScore >= 10)     ev(out, 'HAUL', p, 'mid', { n: dScore, score: p.score });
      else if (dScore > 0)  ev(out, 'PICKING', p, 'low', { n: dScore });
      if (p.resting && !o.resting) ev(out, 'CAMPED', p, 'low', {});
      if (p.invQty.some((n, i) => n > (o.invQty[i] | 0))) ev(out, 'LOOT', p, 'low', {});

      // admiredPassed() in game-data.js already returns exactly the rows a
      // score delta crossed, lowest first — a +20 can clear two of the
      // crowded middle rows at once and both deserve their line.
      if (dScore > 0 && GD.admiredPassed) {
        for (const a of GD.admiredPassed(o.score, p.score)) {
          ev(out, 'PASSED_THE_DEAD', p, 'high', { admired: a });
        }
      }

      // The terrain under the camera subject, which is the only hex we are
      // ever given twice in a row. Flood and fire convert ground underfoot —
      // so this requires the survivor to have STOOD STILL. Without the
      // p.q === o.q test every ordinary step onto different terrain reads as
      // the ground turning, which is both wrong and constant.
      if (next.view && prev.view && next.view.pid === p.pid && prev.view.pid === p.pid
          && p.q === o.q && p.r === o.r
          && next.view.q === p.q && next.view.r === p.r
          && prev.view.q === o.q && prev.view.r === o.r) {
        const a = centreCell(prev.view), b = centreCell(next.view);
        if (a && b && a.terrain !== b.terrain) {
          ev(out, 'GROUND_TURNED', p, 'high', { from: a.terrain, to: b.terrain });
        }
      }
    }
    return out;
  }

  const crossedDown = (a, b, th) => a >= th && b < th;
  const centreCell = (view) => (view.cells || []).find((c) => c.dq === 0 && c.dr === 0) || null;

  // ── Reading the exit ──────────────────────────────────────────
  // encActive going false has four different stories behind it and the
  // firmware already enumerates them as ENC_END_* with labels the game client
  // mirrors in data/network.js — reuse those words. We cannot see the reason,
  // so we infer it.
  //
  // The abort row is the most under-rated beat in the game: a survivor
  // standing in a room full of medicine who decides it is not worth it. The
  // whole stance of this voice is bad judgement, so the one time somebody
  // shows GOOD judgement is worth a line of its own — which makes getting
  // this row RIGHT matter more than the others.
  //
  // The spec's table separates abort from hazard on "no damage", and that is
  // not enough: the pharmacy's cage_trap has an EMPTY penalty block and
  // ends_encounter true, so a trap firing and a survivor walking away look
  // identical in the meters. `tcHint` is the discriminator, and it comes
  // straight off the firmware — handleMsg_enc_abort bumps G.threatClock
  // (network-msg-encounter.hpp:328) and the hazard end at :231 does not. The
  // only other thing that bumps it is opening an encounter, which we can see.
  // (This is the sort of thing /chronicle would simply tell us — see the
  // spec's open decisions.)
  function readExit(o, p, prev, next, dRes, dScore, dLL, tcHint) {
    const json = OBS.Scene.get(o.encBiome, o.encId);
    const h = OBS.Scene.endingHazard(json, o.encNode);
    const base = {
      biome: o.encBiome, id: o.encId, node: o.encNode,
      loot: o.encLoot.slice(), canBank: o.encCanBank,
      scene: OBS.Scene.title(json), room: OBS.Scene.roomName(o.encNode),
    };
    const landed = dRes.some((n, i) => n > 0 && o.encLoot[i] > 0)
      || (dScore > 0 && o.encLoot.some((n) => n > 0));
    // There is no `downed` row here: a survivor who dies in a scene never
    // reaches this function, because the slot stops appearing and the
    // disappearance pass above has already fired VANISHED. The obituary is
    // the beat, not the exit.
    if (landed)              return Object.assign(base, { reason: 'banked', gained: dRes.map((n) => Math.max(0, n)), score: dScore });
    if (next.day > prev.day) return Object.assign(base, { reason: 'dawn' });
    if (dLL > 0)             return Object.assign(base, { reason: 'hazard', hazard: h });
    if (tcHint)              return Object.assign(base, { reason: 'abort' });
    // No damage, no clock. If nothing on this node could have thrown them
    // out, nothing did: they walked.
    if (o.encCanBank && !h)  return Object.assign(base, { reason: 'abort' });
    return Object.assign(base, { reason: 'hazard', hazard: h });
  }

  // ══════════════════════════════════════════════════════════════
  // The director
  //
  // Ranks on ll / llCap, NEVER raw ll: llCap is effectiveMaxLL() and moves
  // with equipment, so raw LL is not comparable across players.
  // ══════════════════════════════════════════════════════════════
  const Director = {
    subject: null,            // roster key
    phase: 'LEDGER',
    phaseFlippedAt: 0,
    subjectSince: 0,
    lastCutAt: 0,
    belowStreak: 0,
    queue: [],                // keys promised a scene of their own
    promises: [],             // queued THIS poll, so the show can say so out loud
    lastCutReason: '',
    cuts: 0,
    hardCuts: 0,

    consider(snap, events, now) {
      // ── When the turn happens ────────────────────────────
      // On the FIRST DEATH, or on half or more of the connected players
      // sitting at ll <= llCap/2 — whichever comes first. The threshold alone
      // mistimes it both ways: six players limping trips it before anything
      // has happened, and one catastrophic death among five healthy players
      // does not trip it at all, which is the exact moment an audience feels
      // a show change gear.
      if (this.phase === 'LEDGER') {
        const died = events.some((e) => e.kind === 'VANISHED'
          && Roster.byKey(e.key) && Roster.byKey(e.key).cause !== 'WALKED_OUT');
        const live = snap.live;
        const half = live.length > 0 && live.filter((p) => p.ll <= p.llCap / 2).length * 2 >= live.length;
        this.belowStreak = half ? this.belowStreak + 1 : 0;
        // A death needs no debounce; it is unambiguous.
        if (died || this.belowStreak >= PHASE_DEBOUNCE) {
          this.phase = 'REAPING';
          this.phaseFlippedAt = now;
          // v1: the flip is ONE-WAY. A deliberate dramatic choice, flagged in
          // the spec's open decisions so it is a choice and not an accident —
          // this codebase has been bitten before by a one-way door nobody
          // picked on purpose (the creeping doom's awareness latch).
        }
      }

      const subjEntry = this.subject ? Roster.byKey(this.subject) : null;
      const subjP = subjEntry && !subjEntry.gone ? snap.byKey.get(this.subject) : null;

      // ── The encounter lock ───────────────────────────────
      // An open encounter outranks everything. A scene is a complete story
      // with an ending; cutting away mid-room to show somebody else's water
      // meter throws away the only three-act structure the game has.
      if (subjP && subjP.encActive) {
        const vanished = events.find((e) => e.kind === 'VANISHED');
        if (vanished) return this.cutTo(vanished.key, now, 'vanished-breaks-lock');
        for (const e of events) {
          if (e.kind === 'THRESHOLD' && e.key !== this.subject && !this.queue.includes(e.key)) {
            // The camera finishes the scene it is on and the second is
            // queued — with a one-line promise that it is coming. The show
            // is allowed to say "we will get to that."
            this.queue.push(e.key);
            this.promises.push(e.key);
          }
        }
        return null;
      }

      // A scene we promised, still running, takes the camera the moment the
      // current one lets go. The key only comes off the queue when the cut
      // actually happens: refused by the cut floor, the promise waits for the
      // next poll rather than being silently dropped.
      while (this.queue.length) {
        const k = this.queue[0];
        const q = snap.byKey.get(k);
        if (!q || !q.encActive) { this.queue.shift(); continue; }  // scene over, promise void
        const c = this.cutTo(k, now, 'queued-scene');
        if (c) this.queue.shift();
        return c;
      }

      const ranked = this.rank(snap, now);
      if (!ranked.length) return null;
      const best = ranked[0];

      if (!subjP) return this.cutTo(best.key, now, this.subject ? 'subject-gone' : 'open');

      const dwell = now - this.subjectSince;
      // (The cut floor is enforced in cutTo(), for every path.)

      // Anybody opening a scene takes the camera: it outranks the meters. A
      // THRESHOLD is severity max, so it is a legitimate hard interrupt even
      // before MIN_DWELL -- but it has to be LOGGED as one, because "no cut
      // shorter than MIN_DWELL except on a logged hard interrupt" is only a
      // meaningful check if the counter sees every one of them.
      const opened = events.find((e) => e.kind === 'THRESHOLD');
      if (opened) {
        const c = this.cutTo(opened.key, now, 'threshold');
        if (c && dwell < MIN_DWELL_MS) this.hardCuts++;
        return c;
      }

      if (dwell < MIN_DWELL_MS) {
        const hard = events.find((e) => (e.sev === 'high' || e.sev === 'max') && e.key && e.key !== this.subject);
        // Counted only when it actually cuts -- this number is the evidence
        // that nothing shorter than MIN_DWELL happened without a reason.
        if (hard) {
          const c = this.cutTo(hard.key, now, 'hard:' + hard.kind);
          if (c) this.hardCuts++;
          return c;
        }
        return null;
      }
      if (dwell > MAX_DWELL_MS) {
        // Force a cut so the camera never stares -- but at somebody ELSE. If the
        // subject is still the hottest thing on the board there is nowhere to
        // go, and cutting to nobody is not a cut.
        const other = ranked.find((r) => r.key !== this.subject);
        return other ? this.cutTo(other.key, now, 'max-dwell') : null;
      }
      const mine = ranked.find((r) => r.key === this.subject);
      if (best.key !== this.subject && best.heat > (mine ? mine.heat : 0) * CUT_MARGIN) {
        return this.cutTo(best.key, now, 'heat');
      }
      return null;
    },

    cutTo(key, now, reason) {
      if (!key) return null;
      // The floor lives HERE, not at each call site, because "whatever
      // happens" is not a thing you can enforce in five places.
      if (now - this.lastCutAt < CUT_FLOOR_MS) return null;
      // Cutting to whoever is already on camera is not a cut. It used to be
      // reachable -- the THRESHOLD branch fires for ANY opener, including the
      // subject -- and it emitted a Leave and an Establish about the same
      // survivor back to back: "we leave X where X wants to be. this is X."
      if (key === this.subject) return null;
      const prev = this.subject;
      if (prev && prev !== key) {
        const pe = Roster.byKey(prev);
        if (pe) pe.camera.totalMs += now - this.subjectSince;
      }
      this.subject = key;
      this.subjectSince = now;
      this.lastCutAt = now;
      this.lastCutReason = reason;
      this.cuts++;
      const e = Roster.byKey(key);
      if (e) { e.camera.cuts++; e.camera.lastSubjectAt = now; }
      return { key, reason, from: prev };
    },

    // ── Phase heat ───────────────────────────────────────────
    rank(snap, now) {
      const live = snap.live;
      if (!live.length) return [];
      const maxScore = Math.max(1, ...live.map((p) => p.score));
      const maxVel = Math.max(1, ...live.map((p) => Roster.scoreVel(Roster.entry(p), 60000, now)));
      const rows = [];
      for (const p of live) {
        const e = Roster.entry(p);
        // returnBias is not garnish. Pure heat ranking churns the subject and
        // the audience never learns a name; a recurring character is the
        // difference between a story and a scoreboard.
        const bias = clamp(e.camera.cuts / 4, 0, 1);
        let heat;
        if (this.phase === 'LEDGER') {
          heat = 0.45 * (p.score / maxScore)
               + 0.30 * (Roster.scoreVel(e, 60000, now) / maxVel)
               + 0.15 * this.eventWeight(p.key, now, 20000)
               + 0.10 * bias;
        } else {
          heat = 0.50 * (1 - p.llFrac)
               + 0.25 * Roster.llVelDown(e, 60000, now)
               + 0.15 * this.pressure(p)
               + 0.10 * bias;
        }
        // An open encounter outranks everything, and that has to be true of
        // the RANKING as well as the lock. The lock is armed by a THRESHOLD,
        // which only fires on a transition — so a scene already running when
        // the page loads, or when the previous scene released the camera,
        // would otherwise never be picked up at all.
        if (p.encActive) heat += 1;
        rows.push({ key: p.key, pid: p.pid, heat });
      }
      return rows.sort((a, b) => b.heat - a.heat);
    },

    eventWeight(key, now, ms) {
      let w = 0;
      for (const e of ST.recent) if (e.key === key && now - e.t <= ms) w += SEV_W[e.sev] || 0;
      return clamp(w / 3, 0, 1);
    },

    pressure(p) {
      return clamp((p.food === 0 ? 0.3 : 0) + (p.water === 0 ? 0.3 : 0)
                 + clamp(p.rad / 20, 0, 0.2) + clamp((p.wounds[0] + p.wounds[1] * 2) / 6, 0, 0.3), 0, 1);
    },
  };

  // ══════════════════════════════════════════════════════════════
  // The narrator
  //
  // It sees every derived event and may speak about exactly three things:
  // THE CAMERA SUBJECT, THE WORLD, AND A HARD INTERRUPT. Everything else is a
  // meter moving and belongs in the render. Without that rule the ticker
  // degenerates into a six-player status console with jokes in it — six
  // threads, no thread.
  // ══════════════════════════════════════════════════════════════
  const Narrator = {
    rings: new Map(),      // bank key -> recent indices
    said: new Map(),       // exact text -> when, for the prose anti-repeat
    pending: [],           // priority queue of lines not yet shown
    ticker: [],            // what is on screen
    headline: null,        // { text, until, kind }
    lastTickerAt: 0,
    lines: 0,

    // Anti-repeat: a ring of the last min(3, len-1) indices per bank; the
    // pick is seeded by (pid ^ tickId) so it is deterministic against a
    // replay, then linear-probes past anything in the ring.
    pickFrom(bank, bankKey, seed, ctx) {
      if (!Array.isArray(bank) || !bank.length) return '';
      const ringMax = Math.min(3, bank.length - 1);
      let ring = this.rings.get(bankKey);
      if (!ring) { ring = []; this.rings.set(bankKey, ring); }
      let i = Math.abs(seed | 0) % bank.length;
      for (let n = 0; n < bank.length && ring.includes(i); n++) i = (i + 1) % bank.length;
      ring.push(i);
      while (ring.length > ringMax) ring.shift();
      return fill(bank[i], ctx);
    },

    pick(kind, phase, key, ctx) {
      const b = OBS.LINES[kind];
      if (!b) return '';
      const bank = b[phase] || b.ANY || b.LEDGER;
      if (!bank) return '';
      const bankKey = kind + ':' + (b[phase] ? phase : 'ANY');
      return this.pickFrom(bank, bankKey, seedFor(key), ctx);
    },

    // The ring stops a BANK repeating. It does nothing about the scene's own
    // prose, which has no bank and no ring: two survivors reaching the same
    // room inside a minute both get "the dispensary cage is still padlocked",
    // and an audience hears that as a bug. Prose goes through here, and a
    // repeat falls through to the bank line instead.
    fresh(text) {
      if (!text) return '';
      const at = this.said.get(text);
      return (at && Date.now() - at < REPEAT_WINDOW_MS) ? '' : text;
    },

    pickFlat(kind, key, ctx) {
      const bank = OBS.LINES[kind];
      if (!Array.isArray(bank)) return '';
      return this.pickFrom(bank, kind, seedFor(key), ctx);
    },

    say(text, sev, opts) {
      if (!text) return;
      this.pending.push(Object.assign({ text, sev: sev || 'mid', at: Date.now() }, opts || {}));
      this.pending.sort((a, b) => (SEV_W[b.sev] || 0) - (SEV_W[a.sev] || 0) || a.at - b.at);
    },

    // A line that names the scene cannot be written until the file is in
    // hand, and /enc is fired on the same poll the THRESHOLD is detected — so
    // writing the headline immediately produces "into somewhere, on the
    // strength of no information at all", which is funny once and wrong every
    // time. Pass a thunk plus what it is waiting for; tick() holds the slot
    // until the fetch lands, and after SCENE_GRACE_MS gives up and says it
    // anyway, because a late headline is worse than a vague one.
    sayWhenLoaded(biome, id, make, sev, opts) {
      if (OBS.Scene.get(biome, id)) { this.say(make(), sev, opts); return; }
      this.say(make, sev, Object.assign({ needs: [biome, id] }, opts || {}));
    },

    // Out in the open a cut carries at most four lines: Establish on the cut,
    // Develop on mid+ events, Turn on a high event, Leave on the cut-away.
    // In a scene the acts already supply the shape, so the narrator follows
    // the encounter instead of imposing on it. Establish and Leave stay
    // mandatory in both — Leave is written KNOWING the beat is over, which is
    // where the comedy lands.
    // The promise. It is the one line the narrator says about somebody who is
    // deliberately NOT the camera subject, which is why it does not go
    // through emit()'s subject filter.
    announceQueued(snap) {
      for (const key of Director.promises) {
        const e = Roster.byKey(key);
        if (!e || !e.last) continue;
        this.sayWhenLoaded(e.last.encBiome, e.last.encId,
          () => this.pick('QUEUED', Director.phase, key, ctxFor(e, snap)), 'mid');
      }
      Director.promises.length = 0;
    },

    onCut(cut, snap) {
      if (cut.from) {
        const oe = Roster.byKey(cut.from);
        if (oe) this.say(this.pick('LEAVE', Director.phase, cut.from, ctxFor(oe, snap)), 'mid');
      }
      const e = Roster.byKey(cut.key);
      if (e) this.say(this.pick('ESTABLISH', Director.phase, cut.key, ctxFor(e, snap)), 'high');
    },

    emit(events, snap, now) {
      const phase = Director.phase;
      for (const e of events) {
        if (e.sev === 'low') continue;               // moves a meter, says nothing
        const isSubject = e.key && e.key === Director.subject;
        const isWorld = !e.key;
        const isHard = (e.sev === 'max' || e.kind === 'PASSED_THE_DEAD' || e.kind === 'ESCALATION');
        if (!isSubject && !isWorld && !isHard) continue;

        const entry = e.key ? Roster.byKey(e.key) : null;
        // Everything keyed needs a sampled entry: the scene branches read
        // entry.last. Roster.push() runs before this, so it is always there —
        // but an unattended screen does not get to find out otherwise.
        if (e.key && (!entry || (!entry.last && e.kind !== 'VANISHED'))) continue;
        const ctx = ctxFor(entry, snap, e);
        switch (e.kind) {
          case 'VANISHED': this.onVanished(e, entry, snap); break;
          case 'PASSED_THE_DEAD': {
            // The passed name, its own finished line, and nothing else on
            // screen for four seconds. ADMIRED is the proof this voice is at
            // its best writing epitaphs.
            ctx.scene = e.data.admired.nm;
            this.say(this.pick('PASSED_THE_DEAD', phase, e.key, ctx), 'high', {
              headline: true, hold: HEADLINE_PASSED_MS, sub: e.data.admired.ln, kind: 'PASSED',
            });
            break;
          }
          case 'THRESHOLD':
            // Name the place, and what they were carrying when they went in.
            // Both come out of the file, so this one waits for it.
            this.sayWhenLoaded(e.data.biome, e.data.id,
              () => this.pick('THRESHOLD', phase, e.key, ctxFor(entry, snap, e)),
              'max', { headline: true });
            break;
          case 'DOOR': {
            // The scene's own prose beats a template. 111 files of
            // hand-written text is the largest body of voice in the project;
            // a generic bank line laid on top of it is a downgrade.
            const b = entry.last.encBiome, id = entry.last.encId;
            this.sayWhenLoaded(b, id, () => {
              const prose = this.fresh(OBS.Scene.clause(OBS.Scene.nodeText(OBS.Scene.get(b, id), e.data.node)));
              return prose || this.pick('DOOR', phase, e.key, ctxFor(entry, snap, e));
            }, 'high');
            break;
          }
          case 'PRICE': this.onPrice(e, entry, snap, ctx); break;
          case 'CAN_LEAVE':
            this.sayWhenLoaded(entry.last.encBiome, entry.last.encId,
              () => this.pick('CAN_LEAVE', phase, e.key, ctxFor(entry, snap, e)),
              'high', { headline: true });
            break;
          case 'DEEPER': this.say(this.pick('DEEPER', phase, e.key, ctx), 'mid'); break;
          case 'EXIT': this.onExit(e, entry, snap); break;
          case 'DAWN': this.say(this.pick('DAWN', phase, '', ctx), 'mid'); break;
          case 'WEATHER_TURN': ctx.terrain = e.data.name; this.say(this.pick('WEATHER_TURN', phase, '', ctx), 'mid'); break;
          case 'ESCALATION': ctx.n = e.data.tc; this.say(this.pick('ESCALATION', phase, '', ctx), 'high'); break;
          case 'GROUND_TURNED': ctx.terrain = terrainName(e.data.to); this.say(this.pick('GROUND_TURNED', phase, e.key, ctx), 'high'); break;
          default: {
            if (OBS.LINES[e.kind]) this.say(this.pick(e.kind, phase, e.key, ctx), e.sev);
          }
        }
      }
      this.tick(now);
    },

    // The hazard's own prose, trimmed to one line, whenever the signature
    // could be matched against the file; the bank only fills the gap.
    onPrice(e, entry, snap, ctx) {
      const debt = entry.ledger[entry.ledger.length - 1];
      if (!debt) return;
      this.sayWhenLoaded(debt.biome, debt.id, () => {
        const hit = ledgerHazard(debt);
        return (hit && this.fresh(OBS.Scene.clause(hit.text)))
          || this.pick('PRICE', Director.phase, e.key, ctxFor(entry, snap, e));
      }, 'high');
    },

    // The verdict — and for a bank, what the whole thing cost. Names are
    // resolved at say-time, same as everywhere else, because the player has
    // already left the encounter and entry.last no longer knows which file it
    // was: only the event does.
    onExit(e, entry, snap) {
      const d = e.data;
      const phase = Director.phase;
      const mk = () => {
        const ctx = ctxFor(entry, snap, e);
        const json = OBS.Scene.get(d.biome, d.id);
        ctx.scene = OBS.Scene.title(json);
        ctx.room = OBS.Scene.roomName(d.node);
        ctx.haul = OBS.Scene.haulText(d.loot);
        ctx.n = entry.last ? entry.last.ll : 0;
        return ctx;
      };
      if (d.reason === 'banked') {
        this.sayWhenLoaded(d.biome, d.id, () => this.pick('BANKED', phase, e.key, mk()), 'high', { headline: true });
      } else if (d.reason === 'abort') {
        this.sayWhenLoaded(d.biome, d.id, () => this.pick('ABORTED', phase, e.key, mk()), 'high', { headline: true });
      } else if (d.reason === 'dawn') {
        this.sayWhenLoaded(d.biome, d.id, () => this.pick('EXIT_DAWN', phase, e.key, mk()), 'mid');
      } else {
        this.sayWhenLoaded(d.biome, d.id, () => {
          const h = OBS.Scene.endingHazard(OBS.Scene.get(d.biome, d.id), d.node);
          return (h && this.fresh(OBS.Scene.clause(h.text))) || this.pick('LOST_IT', phase, e.key, mk());
        }, 'max', { headline: true });
      }
    },

    // The death is the set-piece; everything else is build-up. It holds the
    // headline for 12 s — longer than anything else earns — and the roster
    // card keeps the obituary permanently.
    onVanished(e, entry, snap) {
      if (!entry) return;
      const ctx = ctxFor(entry, snap);
      if (entry.cause === 'WALKED_OUT') {
        this.say(entry.obituary, 'high', { headline: true });
      } else {
        this.say(this.pick('VANISHED', Director.phase, e.key, ctx), 'max', {
          headline: true, hold: HEADLINE_DEATH_MS, sub: entry.obituary, kind: 'DEATH',
        });
      }
    },

    tick(now) {
      if (this.headline && now >= this.headline.until) this.headline = null;
      // Drop unread lines rather than narrate the past: mid after 20 s, high
      // after 45 s. max never expires — a death waits its turn. The hard cap
      // is the unattended-run backstop: without it a busy board queues faster
      // than one line per 4 s and the queue is a slow leak with a voice.
      this.pending = this.pending.filter((l) => (l.sev === 'max')
        || (l.sev === 'high' ? now - l.at < STALE_HIGH_MS : now - l.at < STALE_MID_MS));
      if (this.pending.length > PENDING_MAX) {
        this.pending = this.pending.slice(0, PENDING_MAX);
      }
      if (!this.pending.length) return;
      const next = this.pending[0];

      // Resolve a deferred line, or hold the slot while /enc is in flight.
      let text = next.text;
      if (typeof text === 'function') {
        const ready = !next.needs || !!OBS.Scene.get(next.needs[0], next.needs[1]);
        if (!ready && now - next.at < SCENE_GRACE_MS) return;
        text = next.text();
        if (!text) { this.pending.shift(); return; }
      }

      if (next.headline) {
        if (this.headline && now < this.headline.until && next.sev !== 'max') return;
        this.pending.shift();
        this.headline = {
          text, sub: next.sub || '', kind: next.kind || '',
          until: now + (next.hold || HEADLINE_MS),
        };
        this.push(text);
        return;
      }
      // A set-piece holds the screen alone. The death earns 12 s and a
      // passed name earns 4; running the ticker underneath either of them is
      // exactly the "six threads, no thread" failure the subject filter
      // exists to prevent, one layer down.
      if (this.headline && now < this.headline.until
          && (this.headline.kind === 'DEATH' || this.headline.kind === 'PASSED')) return;
      if (now - this.lastTickerAt < TICKER_MS) return;
      this.pending.shift();
      this.lastTickerAt = now;
      this.push(text);
    },

    push(text) {
      if (!text) return;
      const now = Date.now();
      this.lines++;
      this.said.set(text, now);
      if (this.said.size > 256) {
        for (const [k, at] of this.said) if (now - at > REPEAT_WINDOW_MS) this.said.delete(k);
      }
      this.ticker.push({ text, at: now });
      while (this.ticker.length > TICKER_KEEP) this.ticker.shift();
    },
  };

  function seedFor(key) {
    const pid = key ? parseInt(String(key).split(':')[0], 10) || 0 : 0;
    return (pid ^ (ST.snap ? ST.snap.tickId : 0)) + Narrator.lines * 7;
  }

  function fill(tpl, ctx) {
    return String(tpl).replace(/\{(\w+)\}/g, (m, k) => (ctx[k] == null ? '' : String(ctx[k])));
  }

  function ctxFor(entry, snap, e) {
    const p = entry && entry.last ? entry.last : null;
    const json = p && p.encActive ? OBS.Scene.get(p.encBiome, p.encId) : null;
    const ctx = {
      name: entry ? entry.name : 'somebody',
      // handleMsg_pick names a survivor after their archetype
      // (network-msg-player.hpp:47), so on a real board {name} and {arch} are
      // the SAME WORD and '{name} the {arch}' reads 'Guide the guide'. When
      // they collide the slot falls back to the plain noun.
      arch: (entry && String(entry.archName).toLowerCase() !== String(entry.name).toLowerCase())
        ? String(entry.archName).toLowerCase() : 'survivor',
      day: snap ? snap.day : 0,
      score: p ? p.score : (entry ? entry.peakScore : 0),
      n: p ? p.ll : 0,
      terrain: snap && snap.view && p && snap.view.pid === p.pid
        ? terrainName((centreCell(snap.view) || {}).terrain) : 'ground',
      scene: json ? OBS.Scene.title(json) : 'somewhere',
      room: p ? OBS.Scene.roomName(p.encNode) : 'room',
      haul: p ? OBS.Scene.haulText(p.encLoot) : 'nothing',
      item: '',
      risk: '',
    };
    if (json && p) {
      const g = OBS.Scene.greed(json, p.encNode, p.encLoot, p.encCanBank);
      if (g.topRisk) ctx.risk = g.topRisk + '%';
      if (g.haul.length) ctx.item = g.haul[0].name;
    }
    if (e && e.data) {
      if (e.data.n != null) ctx.n = e.data.n;
      if (e.kind === 'PRICE') ctx.n = e.data.ll;
      if (e.kind === 'ESCALATION') ctx.n = e.data.tc;
    }
    return ctx;
  }

  // ══════════════════════════════════════════════════════════════
  // Hex art
  //
  // The first build drew the vision disk as flat TERRAIN[] colours, because
  // /state's cells had no `variant` and there was no way to know WHICH of
  // /img/hex<Name><N>.png a hex was wearing. That was a real gap, not a
  // style: the board is showing the players hand-painted tiles and the
  // spectator screen was showing lozenges.
  //
  // `variant` and the `vc` counts are on the wire now, so this loads the same
  // files renderer.js does, by the same rule (terrainImgVariants[t][v], and
  // POI_ART for a hex whose variant was pinned for a named landmark). The
  // flat colour stays underneath as the backing: it is what shows before an
  // image decodes, and what an older board still gets.
  // ══════════════════════════════════════════════════════════════

  // MIRRORED from engine.js:202. If a terrain is added there it has to be
  // added here too, or its tiles quietly stop loading on this screen only.
  const TERRAIN_IMG_NAMES = [
    'OpenScrub', 'AshDunes', 'RustForest', 'Marsh',
    'BrokenUrban', 'FloodedDistrict', 'GlassFields',
    'Ridge', 'Mountain', 'Settlement', 'NukeCrater', 'RiverChannel',
    'BunkerEntrance', 'VentShaft', 'TunnelFloor', 'TunnelCollapsed',
  ];
  const SHELTER_IMG_NAMES = ['shelterBasic', 'shelterImproved'];
  // Named art for one specific guaranteed-encounter hex, keyed by the
  // "terrain_variant" the firmware pins on it (hex-map.hpp Phase 5.5).
  // Mirrors POI_ART in engine.js.
  const POI_ART_NAMES = { '0_10': 'poi_jacks_chopper' };

  const Art = {
    imgs: new Map(),      // url -> Image (img.ok true once decoded)
    onload: null,         // set by the disk so a late decode triggers a redraw

    get(url) {
      let img = this.imgs.get(url);
      if (img) return img;
      img = new Image();
      img.ok = false;
      // Resolves once the image has decoded or is given up on (a 404, a bad
      // body, the retry budget) -- what the boot screen waits on.
      img.settled = new Promise((res) => { img.settle = res; });
      img.onload = () => { img.ok = true; img.settle(); if (this.onload) this.onload(); };
      img.onerror = () => { img.ok = false; img.settle(); };
      this.imgs.set(url, img);
      this.queue.push({ url, img, tries: 0 });
      this.pump();
      return img;
    },

    // The board serves at most MAX_CONCURRENT_ASSET_REQS (4) files at once
    // and answers the rest 429 (game-server.hpp admitAssetRequest). A bare
    // `img.src` per tile fired every terrain at once on the first draw, and
    // since an <img> can't tell a 429 from a 404, the rejected ones were
    // cached as permanent misses -- whole terrains blank on the TV for the
    // night. So tiles go through fetch(), two at a time: a 404 is still a
    // permanent miss (the disk redraws about once a second and a 404 loop
    // would be a request storm), but 429 / 5xx / a dropped connection is
    // retried with backoff.
    queue: [],
    inflight: 0,
    MAX_INFLIGHT: 2,
    MAX_TRIES: 6,

    pump() {
      while (this.inflight < this.MAX_INFLIGHT && this.queue.length) {
        const job = this.queue.shift();
        this.inflight++;
        fetch(job.url)
          .then((r) => {
            if (r.status === 404) return 'miss';
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.blob();
          })
          .then((b) => { if (b !== 'miss') job.img.src = URL.createObjectURL(b); else job.img.settle(); })
          .catch(() => {
            if (++job.tries >= this.MAX_TRIES) { job.img.settle(); return; }
            const wait = Math.min(1000 * 2 ** (job.tries - 1), 16000) * (0.75 + Math.random() * 0.5);
            setTimeout(() => { this.queue.push(job); this.pump(); }, wait);
          })
          .finally(() => { this.inflight--; this.pump(); });
      }
    },

    // The tile atlas (scripts/tilegen, mirrors loadTerrainVariants() in
    // engine.js): undefined until asked for, 'loading', the parsed manifest
    // with its pages, or 'failed' -- in which case the per-file tiles below
    // are used exactly as before.
    atlas: undefined,
    atlasReady: null,     // settles when tiles.json has been read (or failed)

    loadAtlas(host) {
      if (this.atlas !== undefined) return;
      this.atlas = 'loading';
      this.atlasReady = fetch(host + '/img/tiles.json', { cache: 'no-cache' })
        .then((r) => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then((m) => {
          if (!m || !Array.isArray(m.pages) || !m.pages.length) throw new Error('no pages');
          m.imgs = m.pages.map((p) => this.get(host + '/img/' + p + '?v=' + encodeURIComponent(m.version || '')));
          this.atlas = m;
          if (this.onload) this.onload();
        })
        .catch(() => { this.atlas = 'failed'; if (this.onload) this.onload(); });
    },

    // The boot screen holds on this: the atlas pages are the art every disk
    // draw needs, so the first frame the audience sees has its tiles. With no
    // atlas (an older board) there is nothing knowable to wait for -- the
    // per-file tiles depend on which hexes /state shows.
    preload(host, onProgress) {
      this.loadAtlas(host);
      return this.atlasReady.then(() => {
        const imgs = (this.atlas && this.atlas.imgs) || [];
        let n = 0;
        onProgress(0, imgs.length);
        return Promise.all(imgs.map((img) => img.settled.then(() => onProgress(++n, imgs.length))));
      });
    },

    // -> { img, sx, sy, m } from the atlas, { img } per-file, or null.
    tile(host, t, variant, vc) {
      this.loadAtlas(host);
      const A = this.atlas;
      if (A === 'loading') return null;
      if (A && A !== 'failed') {
        const pool = A.tiles[t] || [];
        const at = A.poi[t + '_' + variant] ||
          (pool.length ? pool[(((variant | 0) % pool.length) + pool.length) % pool.length] : null);
        const img = at && A.imgs[at[0]];
        return img && img.ok ? { img, sx: at[1], sy: at[2], m: A } : null;
      }
      const img = this.terrain(host, t, variant, vc);
      return img && img.ok ? { img } : null;
    },

    terrain(host, t, variant, vc) {
      const name = TERRAIN_IMG_NAMES[t];
      if (!name) return null;
      const poi = POI_ART_NAMES[t + '_' + variant];
      if (poi) return this.get(host + '/img/' + poi + '.png');
      const count = vc ? (vc[t] | 0) : 0;
      // Wrap, exactly like renderHexContent() does: the variant on the wire
      // can exceed the art count (the mock packs it from a hash, and a
      // pinned landmark variant has no tile of its own).
      //
      // With no counts at all -- an older board, or the synthetic feed --
      // fall back to variant 0, which exists for every terrain that has any
      // art. Less variety, but real tiles instead of lozenges, and a terrain
      // with no art 404s once and is cached as a permanent miss.
      const v = count ? (((variant | 0) % count + count) % count) : 0;
      return this.get(host + '/img/hex' + name + v + '.png');
    },

    shelter(host, kind, q, r, sv) {
      const name = SHELTER_IMG_NAMES[kind - 1];
      if (!name) return null;
      const count = sv ? (sv[kind - 1] | 0) : 0;
      // No variant on the wire for shelters, so pick one by position: stable
      // for a given hex, which is what matters, and wrong in a way nobody
      // standing three metres away can see. No counts, same fallback as the
      // terrain tiles.
      const v = count ? ((((q * 73856093) ^ (r * 19349663)) >>> 0) % count) : 0;
      return this.get(host + '/img/' + name + v + '.png');
    },
  };

  // ══════════════════════════════════════════════════════════════
  // Render — pure against one view object, so the synthetic feed drives the
  // exact same code path the board does.
  // ══════════════════════════════════════════════════════════════
  const EL = {};
  let castSig = '';
  let lastTakeover = '';

  // ── The condition word ────────────────────────────────────────
  // A gauge is a number you have to read. A word is a thing you already know.
  // "NEARLY GONE" lands from across a room in a way that "LL 2/7" never will,
  // and the whole point of this screen is that nobody is sitting close to it.
  // The hairline under the name carries the actual fraction for anyone who
  // wants it.
  const CONDITION = [
    [0.86, 'INTACT'], [0.62, 'HOLDING'], [0.42, 'HURTING'],
    [0.26, 'FAILING'], [0.01, 'NEARLY GONE'], [-1, 'DOWN'],
  ];
  function conditionWord(p) {
    // Running out of something outranks the wound count: a survivor at full
    // health with no water is in more trouble than the bar suggests, and that
    // is exactly the reversal the show is for.
    if (p.water === 0) return 'PARCHED';
    if (p.food === 0) return 'STARVING';
    if (p.wounds[1] > 0 && p.llFrac > 0.42) return 'BLEEDING';
    for (const [th, w] of CONDITION) if (p.llFrac > th) return w;
    return 'DOWN';
  }
  function conditionClass(p) {
    return p.llFrac <= 0.26 ? 'crit' : p.llFrac <= 0.5 ? 'warn' : '';
  }

  // The rank title a score currently occupies, and the one above it. A live
  // player sitting directly under THE AVERAGE MAN ("got exactly this far,
  // like almost all of you. admired for the punctuality") does more comedic
  // work than anything written fresh, so it goes under the name in place of a
  // score readout.
  function currentAdmired(score) {
    let best = null;
    for (const a of GD.ADMIRED) if (a.sc <= score && (!best || a.sc > best.sc)) best = a;
    return best;
  }
  // What the camera subject is climbing toward. It used to sit under the name
  // as "3,180 - 120 short of THE AVERAGE MAN", which repeated the score THE
  // ADMIRED panel was already showing two inches away, in the smallest type
  // on the stage. The joke is about the ladder, so it lives at the foot of
  // the ladder -- and the line it vacated is what the rest of the type grew
  // into.
  function standingLine(p) {
    if (!p) return '';
    const next = GD.admiredNextAbove ? GD.admiredNextAbove(p.score) : null;
    if (next) return (next.sc - p.score) + ' short of ' + next.nm;
    const here = currentAdmired(p.score);
    return here ? 'past ' + here.nm : '';
  }

  function buildSkeleton() {
    D.body.innerHTML = ''
      + '<div id="obs">'
      // A thin, quiet strip. The world is context, not the show.
      +   '<header id="strip">'
      +     '<span id="w-day">DAY 1</span>'
      +     '<span id="w-weather">CLEAR</span>'
      +     '<span id="w-alive">6 ALIVE</span>'
      +     '<span id="w-threat"><i></i></span>'
      +     '<span id="w-status">connecting</span>'
      +   '</header>'

      // The stage. Everything the audience is meant to look at is here, big.
      +   '<main id="stage">'
      +     '<div id="disk-bg"><canvas id="s-disk"></canvas></div>'
      +     '<div id="who">'
      +       '<h1 id="w-name">—</h1>'
      +       '<div id="w-bar"><i></i></div>'
      +       '<div id="w-cond">—</div>'
      +     '</div>'
      +     '<aside id="admired"><h4>THE ADMIRED</h4><ol id="adm-rows"></ol>'
      +       '<div id="adm-next"></div></aside>'
      +     '<div id="place"><span id="w-place"></span></div>'
      +     '<div id="greed">'
      +       '<div id="g-bank">THE DOOR OUT IS OPEN</div>'
      +       '<div id="g-haul"></div>'
      +     '</div>'
      +     '<ul id="doors"></ul>'
      +   '</main>'

      // The voice, at the size the voice deserves.
      +   '<section id="voice">'
      +     '<p id="line"></p>'
      +   '</section>'

      // The cast: names, and which ones are struck through. No bars.
      +   '<footer id="cast"></footer>'

      // Full-screen takeovers. A death is not a row that greys out.
      +   '<div id="takeover"><div class="to-kicker"></div><div class="to-name"></div><div class="to-line"></div></div>'
      + '</div>';
    for (const id of ['strip', 'w-day', 'w-weather', 'w-alive', 'w-threat', 'w-status',
      'stage', 'disk-bg', 's-disk', 'who', 'w-name', 'w-bar', 'w-cond', 'adm-next',
      'place', 'w-place', 'greed', 'g-bank', 'g-haul', 'doors', 'admired', 'adm-rows',
      'voice', 'line', 'cast', 'takeover']) {
      EL[id] = D.getElementById(id);
    }
  }

  function render(snap, now) {
    // The status word updates even with no snapshot at all. A TV that cannot
    // reach the board has to SAY so rather than sit on "connecting" all
    // evening, and with no board there is never a snapshot to hang it on.
    if (EL['w-status']) {
      EL['w-status'].textContent = ST.statusText;
      EL['w-status'].className = 'st-' + ST.status;
    }
    if (!snap) return;

    // ── The strip ────────────────────────────────────────────
    EL['w-day'].textContent = 'DAY ' + snap.day;
    EL['w-weather'].textContent = weatherName(snap.weather);
    EL['w-weather'].className = 'w' + snap.weather;
    EL['w-alive'].textContent = snap.live.length + ' ALIVE';
    EL['w-threat'].firstChild.style.width = clamp(snap.tc / TC_MAX, 0, 1) * 100 + '%';
    EL['w-threat'].className = snap.tc >= 13 ? 'hot' : snap.tc >= 5 ? 'warm' : '';
    // dayTick is a real time-of-day arc, so the whole page gets colder and
    // dimmer at night and comes back at dawn.
    const light = clamp((Math.cos((snap.dayTick / DAY_TICKS) * Math.PI * 2) + 1) / 2, 0, 1);
    D.documentElement.style.setProperty('--night', light.toFixed(3));

    // ── The stage ────────────────────────────────────────────
    const entry = Director.subject ? Roster.byKey(Director.subject) : null;
    const p = entry && !entry.gone ? snap.byKey.get(Director.subject) : null;
    const inScene = !!(p && p.encActive);
    EL.stage.className = inScene ? 'scene' : 'open';

    if (!p) {
      EL['w-name'].textContent = snap.live.length ? 'THE WASTELAND' : 'NOBODY LEFT';
      EL['w-cond'].textContent = '';
      EL['w-place'].textContent = '';
      EL.doors.innerHTML = '';
      EL['s-disk'].innerHTML = '';
      EL.greed.className = '';
    } else {
      EL['w-name'].textContent = p.name.toUpperCase();
      EL['w-bar'].firstChild.style.width = clamp(p.llFrac, 0, 1) * 100 + '%';
      EL['w-bar'].className = conditionClass(p);
      EL['w-cond'].textContent = conditionWord(p);
      EL['w-cond'].className = conditionClass(p);
      if (inScene) renderScene(snap, p);
      else renderOpen(snap, p);
    }

    renderAdmired(snap, now);
    renderCast(snap);
    renderVoice(now);
    fitStage();
  }

  // ── In a scene: the stakes ────────────────────────────────────
  // The greed meter is the centrepiece and it is a single sentence in the
  // largest type on the page, because the joke is that the audience can see
  // the exit and is watching somebody not take it.
  //
  // THE ROOM'S PROSE IS NOT DRAWN HERE. A paragraph is something you read,
  // and nobody reads a paragraph on a television from across a room -- it
  // just sits there being long, and it is the one block on the stage that
  // says nothing new between one poll and the next. The file's writing still
  // carries the scene, but through the NARRATOR, one clause at a time, on the
  // beat it belongs to (Scene.clause(), in the DOOR and PRICE acts). The
  // stage shows what is at stake; the voice does the describing.
  function renderScene(snap, p) {
    const json = OBS.Scene.get(p.encBiome, p.encId);
    const g = OBS.Scene.greed(json, p.encNode, p.encLoot, p.encCanBank);
    EL['w-place'].textContent = json ? OBS.Scene.title(json) : 'somewhere inside';
    EL['g-haul'].textContent = g.haul.length
      ? g.haul.map((h) => h.qty + ' ' + h.name.toUpperCase()).join('  ·  ') + ' IN THE BAG'
      : 'NOTHING IN THE BAG YET';
    EL.greed.className = g.canBank ? 'armed' : 'empty';
    EL.doors.innerHTML = g.terminal
      ? '<li class="terminal">NOTHING FURTHER IN</li>'
      // Odds, then the label on a line of its own, then the skill and the
      // one-way flag under it. All three used to sit on ONE flex row, which
      // meant a long label -- and they are sentences, "force the cage with
      // the bolt-cutter" -- got an ellipsis while a two-word ONE WAY badge
      // wrapped to two lines beside it. The door is the decision; it does not
      // get abbreviated.
      : g.doors.map((d) => '<li' + (d.ends ? ' class="ends"' : '') + '>'
          + '<b class="risk">' + d.risk + '%</b>'
          + '<span class="d-body">'
          +   '<span class="lbl">' + esc(d.label.toUpperCase()) + '</span>'
          +   '<span class="meta">' + esc(d.skill)
          +     (d.ends ? '<i>ONE WAY</i>' : '') + '</span>'
          + '</span></li>').join('');
  }

  // ── Out in the open ───────────────────────────────────────────
  // No meters. The hex disk goes behind everything as scenery, the terrain
  // names the place, and the survivor's own state is the condition word.
  function renderOpen(snap, p) {
    const view = snap.view && snap.view.pid === p.pid ? snap.view : null;
    const t = view ? (centreCell(view) || {}).terrain : undefined;
    EL['w-place'].textContent = (t == null ? 'the open' : terrainName(t)).toUpperCase();
    EL.doors.innerHTML = '';
    EL.greed.className = '';
    renderDisk(snap, p);
  }

  // The vision disk from ?pid=, drawn with the board's own hand-painted tiles.
  //
  // A canvas rather than SVG, for the same reason the game uses one: this is
  // 61 bitmaps, and rebuilding 61 <image> elements every second would churn
  // the DOM for a picture that only changes when somebody walks. It redraws
  // on a signature change (or when a tile finishes decoding), not per poll.
  //
  // hexToPixel here is the game renderer's own formula (renderer.js:79), so
  // the ground sits the same way round as the map in the players' hands, and
  // the image is drawn as a 2*size square centred on the hex exactly like
  // renderHexContent() -- the PNGs are hex-shaped with transparent corners.
  let diskSig = '';
  function renderDisk(snap, p) {
    const view = snap.view && snap.view.pid === p.pid ? snap.view : null;
    const cv = EL['s-disk'];
    if (!view) { diskSig = ''; const c = cv.getContext('2d'); c.clearRect(0, 0, cv.width, cv.height); return; }
    const box = EL['disk-bg'].getBoundingClientRect();
    const sig = view.pid + ':' + view.q + ',' + view.r + ':' + view.visR + ':'
      + Math.round(box.width) + 'x' + Math.round(box.height) + ':'
      + view.cells.map((c) => c.terrain + '.' + (c.variant | 0) + '.' + c.shelter + (c.poi ? 'P' : '') + (c.resource || '')).join('');
    if (sig === diskSig) return;
    diskSig = sig;
    Art.onload = () => { diskSig = ''; };   // a late decode redraws on the next poll

    const dpr = Math.min(root.devicePixelRatio || 1, 2);
    const W = Math.max(1, Math.round(box.width)), H = Math.max(1, Math.round(box.height));
    if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';   // the tiles are 110px; a 4K TV upscales them
    ctx.translate(W / 2, H / 2);

    const SQ3 = Math.sqrt(3);
    const R = Math.max(1, view.visR);
    // COVER the frame: a 61-cell disk shrunk to fit a wide panel is a diagram
    // floating in the middle of the screen. Oversized and bleeding off the
    // edges, it is a place the survivor is standing in.
    const size = Math.max(W / (3 * R + 2), H / (2 * SQ3 * R + 2));
    const host = ST.host;

    // Back to front, as renderHexTerrain() does it: atlas tiles are 3/4
    // dioramas whose props stand up into the hex behind, so a row has to
    // land on top of the row behind it.
    const cells = view.cells.slice().sort((a, b) => (a.dr - b.dr) || (a.dq - b.dq));
    for (const c of cells) {
      const x = size * 1.5 * c.dq;
      const y = size * (SQ3 / 2 * c.dq + SQ3 * c.dr);
      const tile = Art.tile(host, c.terrain, c.variant, snap.vc);
      if (tile && tile.m) {
        // An atlas cell, anchored at the hex centre (see build_tiles.py):
        // unclipped, so peaks and towers overlap the hex behind them.
        const m = tile.m, k = size / m.radius;
        ctx.drawImage(tile.img, tile.sx, tile.sy, m.cell[0], m.cell[1],
          x - m.anchor[0] * k, y - m.anchor[1] * k, m.cell[0] * k, m.cell[1] * k);
      } else if (tile) {
        // Drawn exactly as renderHexContent() does it: a 2*size SQUARE centred
        // on the hex, unclipped and unstroked. The PNGs are hex-shaped with
        // transparent corners and are authored to butt against their
        // neighbours -- clipping them to the hex path and stroking an outline
        // put a dark seam around every tile and made a continuous map look
        // like a board game.
        ctx.drawImage(tile.img, x - size, y - size, size * 2, size * 2);
      } else {
        // No art for this terrain, or it has not decoded yet. TERRAIN[]'s own
        // fill is the fallback, and it is also all an older board can give.
        const T = terrainOf(c.terrain) || { fill: '#161008', stroke: '#332211' };
        hexPath(ctx, x, y, size);
        ctx.fillStyle = T.fill; ctx.fill();
        ctx.strokeStyle = T.stroke; ctx.lineWidth = Math.max(1, size * 0.02); ctx.stroke();
      }

      if (c.shelter) {
        const sh = Art.shelter(host, c.shelter, c.q, c.r, snap.sv);
        if (sh && sh.ok) ctx.drawImage(sh, x - size * 0.6, y - size * 0.6, size * 1.2, size * 1.2);
        else { ctx.strokeStyle = '#C0C0B0'; ctx.lineWidth = size * 0.05; ctx.strokeRect(x - size * 0.2, y - size * 0.2, size * 0.4, size * 0.4); }
      }
      if (c.poi) { ctx.fillStyle = '#E8A828'; dot(ctx, x, y, size * 0.17); }
      else if (c.resource) { ctx.fillStyle = RES_DOT[c.resource] || '#7FB8D8'; dot(ctx, x, y, size * 0.1); }
    }
    // Where the camera subject is standing.
    ctx.strokeStyle = '#E8A828'; ctx.lineWidth = Math.max(2, size * 0.035);
    ctx.beginPath(); ctx.arc(0, 0, size * 0.42, 0, Math.PI * 2); ctx.stroke();
  }

  const RES_DOT = [null, '#7FB8D8', '#8FC050', '#D08030', '#D06060', '#A0A0A0'];
  function hexPath(ctx, cx, cy, size) {
    ctx.beginPath();
    for (let i = 0; i < 6; i++) {
      const a = Math.PI / 180 * (60 * i);
      const px = cx + size * Math.cos(a), py = cy + size * Math.sin(a);
      if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py);
    }
    ctx.closePath();
  }
  function dot(ctx, x, y, r) { ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill(); }

  // ── Fit the type to the screen ────────────────────────────────
  // Everything here is sized off one unit derived from the viewport, and for
  // a long time that unit was hand-tuned against a pixel budget: measure the
  // stage, find it four pixels over, shave a padding, measure again. That
  // only ever calibrated ONE window size. A 16:10 laptop, a 21:9 panel or a
  // 4:3 projector each have a different amount of room for the same six
  // blocks, and the budget was wrong for all of them.
  //
  // So the page measures itself instead. --fit scales the unit until the
  // stage content just fills its box: the type ends up AS LARGE AS THE
  // SCREEN ALLOWS, everywhere, instead of as large as one test window
  // allowed. A survivor whose door label wraps to two lines gets slightly
  // smaller type for as long as they are on camera, which is the right
  // trade: the alternative is the top of their name clipped off.
  //
  // DAMPED, and RATCHETED.
  //
  // Damped because a full proportional step rings: growing the type makes a
  // door label wrap, wrapping makes the content taller, taller shrinks the
  // type, and the label unwraps again -- the screen breathes in and out
  // forever. A third of the way per tick settles in a handful of seconds.
  //
  // Ratcheted because damping alone was not enough. The stage holds far less
  // out in the open (a name, a condition, a place) than it does in a scene (a
  // greed slab, a haul, two doors), so a free-running fit grew to its cap on
  // open ground and slammed back down at every THRESHOLD -- a 13-minute soak
  // caught it swinging 0.87 to 1.6 and moving 79 times, which on a television
  // is the type visibly breathing every ten seconds.
  //
  // So the cap only ever comes DOWN. Whatever the tightest layout of the
  // session needed is what everything gets, and the size stops being a thing
  // the audience can notice. Resizing the window resets it, because that is
  // the one moment a new size is expected.
  // FIT_MAX bounds the STARTUP transient, not the final size. The ratchet
  // drives everything down to whatever the tightest layout needs, so a
  // generous cap buys nothing and costs a visible twenty seconds of shrinking
  // text on a television somebody has just switched on: a page that loads out
  // in the open finds room for 1.58, then a scene opens and it has to fall to
  // 0.98. Capped close to the real answer, the settle is barely a flicker.
  const FIT_MIN = 0.62, FIT_MAX = 1.15;
  const FIT_GAIN = 0.34;        // fraction of the error corrected per tick
  const FIT_GROW_SLACK = 0.04;  // only grow when this much of the box is empty
  const FIT_DEADBAND = 0.008;
  let fit = 1;
  let fitCap = FIT_MAX;         // the ratchet: lowered by any overflow, never raised
  function fitStage() {
    const st = EL.stage;
    if (!st) return;
    const cs = getComputedStyle(st);
    let content = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
    let rows = 0;
    for (const k of st.children) {
      // disk-bg is absolute scenery and admired is pinned to the corner;
      // neither is in the column whose height has to fit.
      if (k.id === 'disk-bg' || k.id === 'admired') continue;
      if (getComputedStyle(k).display === 'none') continue;
      content += k.getBoundingClientRect().height;
      rows++;
    }
    if (rows > 1) content += (parseFloat(cs.rowGap) || 0) * (rows - 1);   // 'normal' where flex gap is unsupported
    const avail = st.getBoundingClientRect().height;
    if (!(avail > 0) || !(content > 0)) return;
    const slack = avail - content;
    const target = clamp(fit * (avail / content) * 0.97, FIT_MIN, FIT_MAX);
    if (content > avail) {
      // Something is clipped. Shrink, and remember that this much was needed.
      fitCap = Math.min(fitCap, target);
    } else {
      if (slack < avail * FIT_GROW_SLACK) return;   // close enough
      if (fit >= fitCap - FIT_DEADBAND) return;     // the ratchet says no
    }
    const want = clamp(fit + (Math.min(target, fitCap) - fit) * FIT_GAIN, FIT_MIN, FIT_MAX);
    if (Math.abs(want - fit) < FIT_DEADBAND) return;
    fit = want;
    D.documentElement.style.setProperty('--fit', fit.toFixed(3));
  }

  // ── THE ADMIRED ───────────────────────────────────────────────
  // The standings, ranked on score, in the corner. It is named after the
  // board in game-data.js for a reason: the same ladder, except these people
  // are still on it.
  //
  // The arrow is "how they are doing", which is deliberately NOT the same
  // question as "what is their score". A survivor can be gaining points and
  // dying at the same time -- that is most of them -- so a falling LL
  // outranks a rising score and shows a down arrow anyway. The whole thesis
  // of this screen is that the ledger is not the thing running out.
  const ADM_MAX = 6;
  let admSig = '';

  function trendOf(e, now) {
    if (Roster.llVelDown(e, 60000, now) > 0.02) return 'dn';
    if (Roster.scoreVel(e, 60000, now) > 0) return 'up';
    return 'flat';
  }

  function renderAdmired(snap, now) {
    const rows = Roster.live()
      .filter((e) => e.last && snap.byKey.has(e.key))
      .sort((a, b) => b.last.score - a.last.score)
      .slice(0, ADM_MAX)
      .map((e) => ({ e, trend: trendOf(e, now) }));
    const sig = rows.map((r) => r.e.key + ':' + r.e.last.score + ':' + r.trend
      + (r.e.key === Director.subject ? '*' : '')).join('|') + '|' + (Director.subject || '');
    if (sig === admSig) return;
    admSig = sig;
    EL.admired.style.display = rows.length ? '' : 'none';
    const sub = Director.subject ? (snap.byKey.get(Director.subject) || null) : null;
    const nextUp = standingLine(sub);
    EL['adm-next'].textContent = nextUp;
    EL['adm-next'].style.display = nextUp ? '' : 'none';
    EL['adm-rows'].innerHTML = rows.map((r, i) =>
      '<li class="' + (r.e.key === Director.subject ? 'on' : '') + '">'
      + '<span class="a-n">' + (i + 1) + '</span>'
      + '<span class="a-name">' + esc(r.e.name) + '</span>'
      + '<span class="a-sc">' + r.e.last.score.toLocaleString() + '</span>'
      + '<span class="a-t t-' + r.trend + '">'
      + (r.trend === 'up' ? '\u25b4' : r.trend === 'dn' ? '\u25be' : '\u00b7')
      + '</span></li>').join('');
  }

  // ── The cast ──────────────────────────────────────────────────
  // Names, and which of them are struck through. That is the whole arc, and
  // it does not need a bar chart. The dead never leave it.
  function renderCast(snap) {
    const all = Roster.all().sort((a, b) => {
      if (a.gone !== b.gone) return a.gone ? 1 : -1;
      return (b.last ? b.last.score : b.peakScore) - (a.last ? a.last.score : a.peakScore);
    });
    const rows = all.length <= RAIL_MAX ? all
      : all.filter((r) => !r.gone).concat(
        all.filter((r) => r.gone).sort((a, b) => b.goneAt - a.goneAt)
          .slice(0, Math.max(0, RAIL_MAX - all.filter((r) => !r.gone).length)));
    const sig = rows.map((r) => r.key + (r.gone ? 'D' : 'L')
      + (r.key === Director.subject ? '*' : '')
      + (!r.gone && snap.byKey.get(r.key) && snap.byKey.get(r.key).encActive ? 'E' : '')).join('|');
    if (sig === castSig) return;
    castSig = sig;
    EL.cast.innerHTML = rows.map((r) => {
      const p = r.gone ? null : snap.byKey.get(r.key);
      const cls = 'c' + (r.gone ? ' dead' : '') + (r.key === Director.subject ? ' on' : '')
        + (p && p.encActive ? ' in-scene' : '');
      return '<span class="' + cls + '">' + esc(r.name)
        + (r.gone ? '<i>†</i>' : (p && p.encActive ? '<i>◈</i>' : '')) + '</span>';
    }).join('');
  }

  // ── The voice ─────────────────────────────────────────────────
  // One line, as big as the page can carry it, and a whisper of the last two
  // underneath. A death or a passed name takes the screen outright.
  function renderVoice(now) {
    const h = Narrator.headline;
    const live = h && now < h.until;
    EL.voice.className = live ? ('on' + (h.kind ? ' k-' + h.kind : '')) : '';
    const text = live ? h.text : (Narrator.ticker.length ? Narrator.ticker[Narrator.ticker.length - 1].text : '');
    if (EL.line.textContent !== text) {
      EL.line.textContent = text;
      EL.line.classList.remove('hit');
      void EL.line.offsetWidth;             // restart the animation
      EL.line.classList.add('hit');
    }
    // ONE line, and no history under it. There used to be a dim echo of the
    // previous line here; it was the smallest and faintest text on the page,
    // which is exactly what "too hard to read" means, and the band it sat in
    // is the only slack the rest of the layout had to grow into. A headline's
    // sub-line is not shown here either -- the only two kinds that have one
    // (a death, a passed name) take the whole screen.

    // Takeovers: the set-pieces get the whole screen, which is the difference
    // between a scoreboard going grey and something happening.
    const kind = live && (h.kind === 'DEATH' || h.kind === 'PASSED') ? h.kind + ':' + h.text : '';
    if (kind !== lastTakeover) {
      lastTakeover = kind;
      if (!kind) {
        EL.takeover.className = '';
      } else {
        EL.takeover.className = 'on k-' + h.kind;
        EL.takeover.children[0].textContent = h.kind === 'DEATH' ? 'GONE' : 'PASSED';
        EL.takeover.children[1].textContent = h.text;
        EL.takeover.children[2].textContent = h.sub || '';
      }
    }
  }

  // ══════════════════════════════════════════════════════════════
  // The poll loop
  // ══════════════════════════════════════════════════════════════
  let inflight = null;
  let timer = 0;

  function subjectPid() {
    const e = Director.subject ? Roster.byKey(Director.subject) : null;
    return e && !e.gone ? e.pid : -1;
  }

  function schedule(ms) {
    clearTimeout(timer);
    timer = setTimeout(poll, ms);
  }

  async function poll() {
    // One in flight, always — a stalled fetch must not queue a second. It
    // still has to re-arm the timer: dropping out here without scheduling
    // ends the poll loop permanently, which on a screen nobody is sitting in
    // front of means a TV showing a frozen board all evening.
    if (inflight) { schedule(POLL_MS); return; }
    const pid = subjectPid();
    const url = ST.host + '/state' + (pid >= 0 ? '?pid=' + pid : '');
    if (ST.feed) {                             // ?feed=fake — no network at all
      // Scheduled BEFORE handle(), because a camera cut inside handle() calls
      // refetchNow() to pull the new pid's vision disk immediately, and
      // schedule() replaces whatever timer is pending. Scheduling afterwards
      // silently cancelled every one of those refetches and the map lagged
      // the headline by a second, which is the exact thing they exist to fix.
      schedule(POLL_MS);
      handle(ST.feed.snapshot(pid));
      return;
    }
    const ac = new AbortController();
    inflight = ac;
    const stall = setTimeout(() => ac.abort(), STALL_MS);
    try {
      const r = await fetch(url, { signal: ac.signal, cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const raw = await r.json();
      if (raw.error) throw new Error(raw.error);
      ST.backoff = 0; ST.pollErrors = 0;
      ST.status = 'live'; ST.statusText = 'live';
      schedule(POLL_MS);      // before handle() — see the ?feed=fake branch
      handle(raw);
    } catch (err) {
      ST.pollErrors++;
      ST.status = 'stalled';
      ST.statusText = (err && err.name === 'AbortError' ? 'stalled' : 'no board') + ' · ' + ST.pollErrors;
      // The board wedges under HTTP load. Back off, do not pile on.
      ST.backoff = ST.backoff ? Math.min(BACKOFF_MAX, ST.backoff * 2) : BACKOFF_MIN;
      render(ST.snap, Date.now());   // status chip, with or without a snapshot
      schedule(ST.backoff);
    } finally {
      clearTimeout(stall);
      inflight = null;
    }
  }

  // On a camera cut, fire one extra immediate fetch with the new pid rather
  // than waiting for the next tick, or the map lags the headline by a second.
  function refetchNow() {
    if (inflight) inflight.abort();
    inflight = null;
    schedule(0);
  }

  function handle(raw) {
    const now = Date.now();
    ST.polls++;
    const snap = normalise(raw, now);

    const events = diff(ST.snap, snap);
    ST.prev = ST.snap;
    ST.snap = snap;
    ST.events = events;

    Roster.push(snap);
    // The scene layer: one /enc fetch per file, ever, fired on the threshold.
    // Ahead of recordEvents() so the hazard signature has a file to match
    // against on the very next poll.
    for (const p of snap.live) {
      if (!p.encActive || !p.encBiome) continue;
      const fresh = events.some((e) => e.kind === 'THRESHOLD' && e.key === p.key);
      OBS.Scene.ensure(ST.host, p.encBiome, p.encId, fresh);
    }
    recordEvents(events, snap);
    // After the ledger: the obituary reads it.
    for (const e of events) if (e.kind === 'VANISHED') Roster.markGone(e.key, snap);
    // The LCD's comic cut-ins (observer-fx.js). After markGone, because a
    // VANISHED is only a death once the roster has called it one.
    if (OBS.FX) OBS.FX.onPoll(events, snap, raw);

    ST.recent = ST.recent.concat(events).filter((e) => now - e.t <= 60000);

    const cut = Director.consider(snap, events, now);
    if (cut) Narrator.onCut(cut, snap);
    if (cut && OBS.FX) OBS.FX.onCut();      // a blade across the glass on every cut
    Narrator.announceQueued(snap);
    Narrator.emit(events, snap, now);
    render(snap, now);
    if (cut) refetchNow();
  }

  // The narrator paces itself between polls, so the ticker keeps moving even
  // when the board has nothing new to say.
  // The voice paces itself between polls, so a line still lands on the beat
  // it was written for even when the board has nothing new to say.
  function paceLoop() {
    const now = Date.now();
    Narrator.tick(now);
    if (ST.snap) renderVoice(now);
    setTimeout(paceLoop, 400);
  }

  // Burn-in avoidance: a TV left on this page for an evening should not etch
  // the rail into the panel. A few pixels on a slow cycle is enough and is
  // invisible at three metres.
  function driftLoop() {
    const t = Date.now() / 60000;
    D.documentElement.style.setProperty('--dx', (Math.sin(t) * 4).toFixed(2) + 'px');
    D.documentElement.style.setProperty('--dy', (Math.cos(t * 0.7) * 3).toFixed(2) + 'px');
    setTimeout(driftLoop, 5000);
  }

  function boot() {
    const qs = new URLSearchParams(root.location.search);
    // Served from the board as data/observer.html, so host = origin and the
    // TV never needs an IP typed into it with a remote control. ?host= keeps
    // desktop development easy and makes the board's dynamic IP stop
    // mattering — the TV bookmarks whatever URL it uses.
    ST.host = (qs.get('host') || '').replace(/\/$/, '');
    if (ST.host && !/^https?:/.test(ST.host)) ST.host = 'http://' + ST.host;
    if (!ST.host) ST.host = root.location.origin;
    if (qs.get('feed') === 'fake' && OBS.FakeFeed) {
      ST.feed = OBS.FakeFeed.create(qs);
      // /state is synthetic but /enc is not: the scene layer still fetches
      // real encounter files from whatever is serving this page, which is
      // the whole reason the fixture uses real biome/id pairs.
      ST.host = root.location.origin;
      ST.status = 'live'; ST.statusText = 'synthetic feed';
    }
    // buildSkeleton() replaces <body>; the boot screen (observer.html) has to
    // survive that, and stays up until the tile atlas is in.
    const bootEl = D.getElementById('obs-boot');
    buildSkeleton();
    if (bootEl) D.body.appendChild(bootEl);
    const BOOT = root.OBS_BOOT;
    if (BOOT) {
      BOOT.status('Loading art\u2026');
      Art.preload(ST.host, (n, total) => BOOT.progress(n, total, 'tile atlas'))
        .catch((e) => console.warn('[observer] art preload failed:', e))
        .then(() => BOOT.done());
    }
    // A new window is the one moment a new type size is expected, so the
    // ratchet is released and allowed to find the size again.
    root.addEventListener('resize', () => { fitCap = FIT_MAX; });
    render(null, Date.now());
    paceLoop();
    driftLoop();
    poll();
  }

  // Exposed for the console and for the synthetic run's own assertions.
  // handle() in particular lets a test step the whole pipeline a frame at a
  // time — diff, roster, ledger, director, narrator, render — without waiting
  // out the poll interval.
  OBS.ST = ST; OBS.Roster = Roster; OBS.Director = Director; OBS.Narrator = Narrator;
  OBS.diff = diff; OBS.normalise = normalise; OBS.causeOfDeath = causeOfDeath;
  OBS.handle = handle; OBS.Art = Art;

  if (D.readyState === 'loading') D.addEventListener('DOMContentLoaded', boot);
  else boot();
}(typeof globalThis !== 'undefined' ? globalThis : this));
