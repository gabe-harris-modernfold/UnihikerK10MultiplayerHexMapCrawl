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
  const DAYS_PER_YEAR = 365;    // matches Esp32HexMapCrawl.ino — the day goes 365 -> 1
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
    COLS: (typeof MAP_COLS !== 'undefined') ? MAP_COLS : 75,
    ROWS: (typeof MAP_ROWS !== 'undefined') ? MAP_ROWS : 57,
    ARCH: (typeof ARCHETYPES !== 'undefined') ? ARCHETYPES : [],
    ARCH_COLORS: (typeof ARCHETYPE_COLORS !== 'undefined') ? ARCHETYPE_COLORS : [],
  };
  const terrainOf = (t) => GD.TERRAIN[t] || null;
  const terrainName = (t) => { const x = terrainOf(t); return x ? x.name : 'ground'; };
  // A forest oasis (docs/forest-oasis.md): Rust Forest pinned to 8..12, as
  // oasisCell() in hex-map.hpp reads it. Inline rather than game-data.js's
  // isOasisCell(), so a TV without that file still knows one.
  const isGrove = (c) => !!c && c.terrain === 2 && (c.variant | 0) >= 8 && (c.variant | 0) <= 12;
  const cellName = (c) => (isGrove(c) ? 'Forest Oasis' : terrainName(c && c.terrain));
  const weatherName = (w) => GD.WEATHER[w] || 'CLEAR';
  // Dawns from day `a` to day `b`. The day goes 365 -> 1, so 365 to 1 is one
  // dawn; a jump back (a regen restarts at day 1) is none.
  const daysPassed = (a, b) => {
    const d = (((b - a) % DAYS_PER_YEAR) + DAYS_PER_YEAR) % DAYS_PER_YEAR;
    return d < DAYS_PER_YEAR / 2 ? d : 0;
  };

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
        // The 19 hexes round them (game-server.hpp "near"), for their panel.
        near: Array.isArray(p.near) ? p.near : null,
        ll: p.ll | 0, llCap: Math.max(1, p.llCap | 0),
        food: p.food | 0, water: p.water | 0, rad: p.rad | 0, ap: p.ap | 0,
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
      // VACANCY (vacancy.hpp): the sign, its town and its count, if the
      // board is new enough to have it.
      vac: raw.vac || null,
      // Machine Gun Kane (kane.hpp): only read for the house in the glass's
      // landmark tile (questTileKey) -- the TV has no jeep yet.
      kane: raw.kane || null,
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
          firstSeen: Date.now(), lastSeen: Date.now(), lastDay: null,
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
        if (e.lastDay == null) e.lastDay = snap.day;
        e.name = p.name; e.archName = p.archName;
        e.lastSeen = snap.t;
        e.last = p;
        e.peakScore = Math.max(e.peakScore, p.score);
        e.tally.steps = p.steps;
        // Summed dawn by dawn: the day itself starts over after 365.
        e.tally.days += daysPassed(e.lastDay, snap.day);
        e.lastDay = snap.day;
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
    if (daysPassed(prev.day, next.day)) ev(out, 'DAWN', null, 'mid', { day: next.day });
    if (next.weather !== prev.weather) {
      ev(out, 'WEATHER_TURN', null, 'mid', { weather: next.weather, name: weatherName(next.weather) });
    }
    for (const step of TC_STEPS) {
      if (prev.tc < step && next.tc >= step) ev(out, 'ESCALATION', null, 'high', { tc: next.tc, step });
    }
    // VACANCY: the sign over one town. The board's, not anybody's, so these
    // carry no key and reach the narrator whoever is on camera; whoever was
    // in the scene that moved it -- now, or on the last poll, since a bank
    // closes the scene -- is named in the line.
    const va = prev.vac, vb = next.vac;
    if (vb && vb.q >= 0) {
      const reader = (part) => {
        const hit = (s) => s.players.find((x) => x.encActive && x.encBiome === 'vacancy' && x.encId === part);
        const p = hit(next) || hit(prev);
        return p ? p.name : 'somebody';
      };
      if (!va || va.q !== vb.q || va.r !== vb.r) {
        if (va && vb.st === 0) ev(out, 'VAC_SIGN', null, 'mid', {});
      } else {
        if (va.st === 0 && vb.st === 1) ev(out, 'VAC_HOOK', null, 'high', { n: vb.rm, name: reader(1) });
        if (!(va.g & 1) && (vb.g & 1)) ev(out, 'VAC_DESK', null, 'mid', { name: reader(2) });
        if (!(va.g & 2) && (vb.g & 2)) ev(out, 'VAC_KEYS', null, 'mid', { name: reader(3) });
        if (va.st === 1 && vb.st === 1 && vb.rm < va.rm) ev(out, 'VAC_ROOM', null, vb.rm <= 2 ? 'high' : 'mid', { n: vb.rm });
        if (va.st <= 1 && vb.st === 2) ev(out, 'VAC_SAVED', null, 'max', { name: reader(4) });
        // The sign's own fall empties the count; a quake that got there first does not.
        if (va.st <= 1 && vb.st === 3) ev(out, vb.rm === 0 ? 'VAC_FALLEN' : 'VAC_LOST', null, vb.rm === 0 ? 'max' : 'high', {});
      }
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
    if (daysPassed(prev.day, next.day)) return Object.assign(base, { reason: 'dawn' });
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
      // mistimes it both ways: five players limping trips it before anything
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
  // degenerates into a five-player status console with jokes in it — five
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
          // VACANCY's two endings get the headline: they change the board for
          // everybody, and one of them deletes a town.
          case 'VAC_SAVED':
          case 'VAC_FALLEN':
            this.say(this.pick(e.kind, phase, '', ctx), 'max', {
              headline: true, sub: e.kind === 'VAC_SAVED' ? 'the sign is dark' : 'the town is full',
            });
            break;
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
        ? cellName(centreCell(snap.view) || {}) : 'ground',
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
      if (e.data.name) ctx.name = e.data.name;   // a world event that knows who (VAC_*)
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

  // MIRRORED from vacTileKey() (vacancy.js) and kaneTileKey() (kane-jeep.js),
  // through questTileKey() in engine.js: the landmark tile a quest site put
  // down during play wears, by position, or null. Keep the keys in step with
  // scripts/hex_sheets.py LANDMARKS.
  function questTileKey(snap, q, r, terrain) {
    const v = snap.vac;
    if (v && v.q >= 0) {
      if (v.dq === q && v.dr === r) return terrain === 1 ? '1_15' : '0_12';
      if (v.kq === q && v.kr === r) return terrain === 5 || terrain === 3 ? '5_15' : '4_13';
      if (v.q === q && v.r === r && v.st === 3 && v.rm === 0) return '4_14';
    }
    const k = snap.kane;
    if (k && k.gq >= 0 && k.gq === q && k.gr === r) return '6_14';
    return null;
  }

  // MIRRORED from altPick() in engine.js: which of n tiles sharing a slot
  // the hex at map (q, r) wears. Must stay bit-identical, or the TV shows a
  // different building from the one the players are standing in.
  function altPick(q, r, n) {
    let h = (Math.imul(q | 0, 73856093) ^ Math.imul(r | 0, 19349663)) >>> 0;
    h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d) >>> 0;
    h = Math.imul(h ^ (h >>> 12), 0x297a2d39) >>> 0;
    return ((h ^ (h >>> 15)) >>> 0) % n;
  }

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

    // The tile atlas (one sheet per terrain, scripts/hex_sheets.py; mirrors
    // loadTerrainVariants() in engine.js): undefined until asked for,
    // 'loading', the parsed manifest with its pages, or 'failed' -- in which
    // case the per-file tiles below are used exactly as before.
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
    // q, r: map coords, which pick among a slot's alternates (tiles.json
    // `alts`) exactly as terrainTile() in engine.js does. quest: a landmark
    // key from questTileKey() that overrides the variant on the wire.
    tile(host, t, variant, vc, q, r, quest) {
      this.loadAtlas(host);
      const A = this.atlas;
      if (A === 'loading') return null;
      const qa = quest && A && A !== 'failed' && A.poi[quest];
      if (qa) {
        const img = A.imgs[qa[0]];
        return img && img.ok ? { img, sx: qa[1], sy: qa[2], m: A } : null;
      }
      if (A && A !== 'failed') {
        const pool = A.tiles[t] || [];
        const poi = A.poi[t + '_' + variant];
        const v = poi || !pool.length ? variant : (((variant | 0) % pool.length) + pool.length) % pool.length;
        let at = poi || (pool.length ? pool[v] : null);
        const alts = at && q !== undefined && A.alts && A.alts[t] && A.alts[t][v];
        if (alts && alts.length) {
          const k = altPick(q, r, alts.length + 1);
          if (k) at = alts[k - 1];
        }
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

    // -> { img, sx, sy, s } from a shelter sheet (tiles.json `shelters`),
    // { img } per-file, or null.
    shelter(host, kind, q, r, sv) {
      const name = SHELTER_IMG_NAMES[kind - 1];
      if (!name) return null;
      this.loadAtlas(host);
      const A = this.atlas;
      if (A === 'loading') return null;
      // No variant on the wire for shelters, so pick one by position: stable
      // for a given hex, which is what matters, and wrong in a way nobody
      // standing three metres away can see.
      const pick = (n) => ((((q * 73856093) ^ (r * 19349663)) >>> 0) % n);
      if (A && A !== 'failed' && Array.isArray(A.shelters)) {
        const pool = A.shelters[kind - 1] || [];
        const at = pool.length ? pool[pick(pool.length)] : null;
        const img = at && A.imgs[at[0]];
        return img && img.ok ? { img, sx: at[1], sy: at[2], s: (A.shelterCell || [224])[0] } : null;
      }
      // Per file. No counts, same fallback as the terrain tiles: variant 0.
      const count = sv ? (sv[kind - 1] | 0) : 0;
      const img = this.get(host + '/img/' + name + (count ? pick(count) : 0) + '.png');
      return img && img.ok ? { img } : null;
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
      +     '<div id="disk-bg"><canvas id="s-disk"></canvas><canvas id="s-air"></canvas></div>'
      +     '<canvas id="s-cams"></canvas>'
      +     '<div id="who">'
      +       '<h1 id="w-name">—</h1>'
      +       '<div id="w-bar"><i></i></div>'
      +       '<div id="w-cond">—</div>'
      +     '</div>'
      +     '<aside id="admired"><canvas id="adm-plate"></canvas>'
      +       '<h4><canvas id="adm-medal"></canvas><span>THE ADMIRED</span></h4><div id="adm-sub"></div>'
      +       '<ol id="adm-rows"></ol><div id="adm-next"></div><div id="adm-stamp"></div></aside>'
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
      'stage', 'disk-bg', 's-disk', 's-air', 's-cams', 'who', 'w-name', 'w-bar', 'w-cond', 'adm-next', 'adm-medal', 'adm-stamp', 'adm-plate', 'adm-sub',
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
    // dimmer at night and comes back at dawn (worldSun).
    D.documentElement.style.setProperty('--night', worldSun(snap.dayTick).light.toFixed(3));

    // ── The stage ────────────────────────────────────────────
    const entry = Director.subject ? Roster.byKey(Director.subject) : null;
    const p = entry && !entry.gone ? snap.byKey.get(Director.subject) : null;
    const inScene = !!(p && p.encActive);
    EL.stage.className = inScene ? 'scene' : 'open';
    Glide.note(snap, now);

    if (!p) {
      EL['w-name'].textContent = snap.live.length ? 'THE WASTELAND' : 'NOBODY LEFT';
      EL['w-cond'].textContent = '';
      EL['w-place'].textContent = '';
      EL.doors.innerHTML = '';
      renderDisk(snap, null);
      EL.greed.className = '';
    } else {
      EL['w-name'].textContent = p.name.toUpperCase();
      EL['w-bar'].firstChild.style.width = clamp(p.llFrac, 0, 1) * 100 + '%';
      EL['w-bar'].className = conditionClass(p);
      EL['w-cond'].textContent = conditionWord(p);
      EL['w-cond'].className = conditionClass(p);
      if (inScene) renderScene(snap, p);
      else renderOpen(snap, p);
      renderDisk(snap, p);   // in a scene too: their panel says INSIDE
    }
    renderCams(snap, p);

    renderAdmired(snap, now);
    renderCast(snap);
    renderVoice(now);
    fitStage();
    fitAdmired();
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
  // No meters. The terrain names the place and the survivor's own state is
  // the condition word; the ground itself is in their panel (renderDisk).
  function renderOpen(snap, p) {
    const view = snap.view && snap.view.pid === p.pid ? snap.view : null;
    const here = view ? (centreCell(view) || {}) : {};
    const t = here.terrain;
    EL['w-place'].textContent = (t == null ? 'the open'
      : isGrove(here) ? 'Forest Oasis \u00b7 safe rest' : terrainName(t)).toUpperCase();
    EL.doors.innerHTML = '';
    EL.greed.className = '';
  }

  // ── The ground, as the game draws it ──────────────────────────
  // hexToPixel here is the game renderer's own formula (renderer.js:79), so
  // the ground sits the same way round as the map in the players' hands, and
  // each tile lands on the hex exactly like renderHexContent() -- the PNGs
  // are hex-shaped with transparent corners.
  //
  // One painter for both pictures on this stage: the camera subject's vision
  // disk (renderDisk) and everybody's panel (renderCams). The caller has
  // already translated the context to where the camera is looking. `k` is a
  // glide still to run, in hexes (Glide): the ground is drawn that far back
  // along the step so the survivor walks into the middle instead of jumping.
  const SQ3 = Math.sqrt(3);
  const hexXY = (dq, dr, size) => [size * 1.5 * dq, size * (SQ3 / 2 * dq + SQ3 * dr)];
  const hexLen = (dq, dr) => (Math.abs(dq) + Math.abs(dr) + Math.abs(dq + dr)) / 2;
  // q, r wrap independently on the torus (renderer.js:131 measures it so).
  const wrapDelta = (d, n) => { d = ((d % n) + n) % n; return d > n / 2 ? d - n : d; };
  const torusOff = (q1, r1, q2, r2) => [wrapDelta(q2 - q1, GD.COLS), wrapDelta(r2 - r1, GD.ROWS)];

  function drawGround(ctx, snap, cells, size, k) {
    const host = ST.host;
    const kq = k ? k[0] : 0, kr = k ? k[1] : 0;
    // Back to front, as renderHexTerrain() does it: a diorama atlas's tiles
    // stand up into the hex behind, so a row has to land on top of the row
    // behind it. (Flat sheet tiles don't care.)
    cells = cells.slice().sort((a, b) => (a.dr - b.dr) || (a.dq - b.dq));
    const out = [];
    for (const c of cells) {
      const [x, y] = hexXY(c.dq + kq, c.dr + kr, size);
      const tile = Art.tile(host, c.terrain, c.variant, snap.vc, c.q, c.r, questTileKey(snap, c.q, c.r, c.terrain));
      out.push({ x, y, q: c.q, r: c.r, terrain: c.terrain, tile });
      // Masked to the hex, as renderHexContent() does it (drawTileInHex): the
      // sheet tiles carry bleed past the rim, and unmasked it spilled onto the
      // neighbours. Only a diorama atlas (not flat) stands up into the hex
      // behind, so only it goes unclipped.
      const masked = tile && !(tile.m && !tile.m.flat);
      if (masked) { ctx.save(); hexPath(ctx, x, y, size); ctx.clip(); }
      if (tile && tile.m) {
        // An atlas cell, anchored at the hex centre. A flat sheet cell (anchor
        // = centre of a square cell) lands on the same 2*size square as below.
        const m = tile.m, s = size / m.radius;
        ctx.drawImage(tile.img, tile.sx, tile.sy, m.cell[0], m.cell[1],
          x - m.anchor[0] * s, y - m.anchor[1] * s, m.cell[0] * s, m.cell[1] * s);
      } else if (tile) {
        // A per-file tile: a 2*size square centred on the hex, as ever.
        ctx.drawImage(tile.img, x - size, y - size, size * 2, size * 2);
      } else {
        // No art for this terrain, or it has not decoded yet. TERRAIN[]'s own
        // fill is the fallback, and it is also all an older board can give.
        // The divider pass below outlines it like any other hex.
        const T = terrainOf(c.terrain) || { fill: '#161008' };
        hexPath(ctx, x, y, size);
        ctx.fillStyle = T.fill; ctx.fill();
      }
      if (masked) ctx.restore();

      if (c.shelter) {
        const sh = Art.shelter(host, c.shelter, c.q, c.r, snap.sv);
        if (sh && sh.s) ctx.drawImage(sh.img, sh.sx, sh.sy, sh.s, sh.s, x - size * 0.6, y - size * 0.6, size * 1.2, size * 1.2);
        else if (sh) ctx.drawImage(sh.img, x - size * 0.6, y - size * 0.6, size * 1.2, size * 1.2);
        else { ctx.strokeStyle = '#C0C0B0'; ctx.lineWidth = size * 0.05; ctx.strokeRect(x - size * 0.2, y - size * 0.2, size * 0.4, size * 0.4); }
      }
      if (c.poi) { ctx.fillStyle = '#E8A828'; dot(ctx, x, y, size * 0.17); }
      else if (c.resource) { ctx.fillStyle = RES_DOT[c.resource] || '#7FB8D8'; dot(ctx, x, y, size * 0.1); }
    }
    // The hex divider, as renderGridLines() draws it: one black line per
    // shared edge -- every hex in ONE path on the true edge, stroked once, so
    // neighbours trace the same pixels and the coverage doesn't stack. Width
    // keeps the game's 1px-at-56px ratio: the TV's hexes are far bigger.
    ctx.beginPath();
    for (const o of out) {
      for (let i = 0; i < 6; i++) {
        const a = Math.PI / 3 * i;
        const px = o.x + size * Math.cos(a), py = o.y + size * Math.sin(a);
        if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py);
      }
      ctx.closePath();
    }
    ctx.strokeStyle = '#000'; ctx.lineWidth = Math.max(1, size / 56); ctx.stroke();
    // A forest oasis: a soft green ring just inside each grove hex, so the
    // safe ground reads at TV distance over the canopy art.
    const groves = cells.filter(isGrove);
    if (groves.length) {
      ctx.save();
      ctx.strokeStyle = 'rgba(150, 220, 120, 0.55)'; ctx.lineWidth = Math.max(2, size * 0.045);
      for (const c of groves) {   // hexPath() starts its own path, so one stroke each
        const [x, y] = hexXY(c.dq + kq, c.dr + kr, size);
        hexPath(ctx, x, y, size * 0.86);
        ctx.stroke();
      }
      ctx.restore();
    }
    // VACANCY's sign, when its town is on this patch of ground.
    const vac = snap.vac;
    const town = vac && vac.q >= 0 ? cells.find((c) => c.q === vac.q && c.r === vac.r) : null;
    if (town) { const [x, y] = hexXY(town.dq + kq, town.dr + kr, size); drawVacSign(ctx, x, y, size, vac); }
    return out;
  }

  // ── The survivors on it ───────────────────────────────────────
  // The game's pawn (renderer.js drawCharIcon) cut down to what reads from
  // across a room: a ground shadow, the archetype's portrait in its ring and,
  // for anybody but the one the picture is about, a call sign over it.
  function pawnImg(arch) {
    const a = GD.ARCH[arch];
    return a && a.name ? Art.get(ST.host + '/img/survivors/' + a.name.toLowerCase() + 'Pawn.jpg') : null;
  }
  const STENCIL = "Impact, 'Arial Narrow', 'Arial Black', sans-serif";
  function drawPawn(ctx, x, y, size, p, tag) {
    // The one the picture is about stands a size up from company.
    const r = Math.max(8, size * (tag ? 0.3 : 0.38));
    const cy = y - size * 0.1;
    const color = GD.ARCH_COLORS[p.arch] || '#E8A828';
    ctx.save();
    ctx.fillStyle = 'rgba(0,0,0,0.5)';
    ctx.beginPath(); ctx.ellipse(x, cy + r * 0.95, r * 0.72, r * 0.2, 0, 0, Math.PI * 2); ctx.fill();
    const img = pawnImg(p.arch);
    ctx.beginPath(); ctx.arc(x, cy, r, 0, Math.PI * 2);
    if (img && img.ok) { ctx.save(); ctx.clip(); ctx.drawImage(img, x - r, cy - r, r * 2, r * 2); ctx.restore(); }
    else { ctx.fillStyle = color; ctx.fill(); }
    const ring = Math.max(1.5, r * 0.13);
    ctx.beginPath(); ctx.arc(x, cy, r, 0, Math.PI * 2);
    ctx.lineWidth = ring; ctx.strokeStyle = color; ctx.stroke();
    ctx.beginPath(); ctx.arc(x, cy, r + ring, 0, Math.PI * 2);
    ctx.lineWidth = Math.max(1, r * 0.05); ctx.strokeStyle = '#000'; ctx.stroke();
    if (tag) {
      const fs = Math.max(9, Math.round(r * 0.62));
      ctx.font = fs + 'px ' + STENCIL;
      ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
      const label = p.name.substring(0, 10).toUpperCase();
      const tw = ctx.measureText(label).width, ty = cy - r - fs * 0.35;
      ctx.fillStyle = 'rgba(6,4,3,0.82)';
      ctx.fillRect(x - tw / 2 - fs * 0.3, ty - fs * 1.05, tw + fs * 0.6, fs * 1.15);
      ctx.fillStyle = color; ctx.fillText(label, x, ty);
    }
    ctx.restore();
  }

  // Everybody standing within R of `owner`, as hex offsets from where the
  // owner is drawn: their own glide folded in, the owner's added by the
  // caller. Two on one hex step apart a little, or the second portrait hides
  // the first.
  function drawPawns(ctx, snap, owner, R, size, k, now) {
    const kq = k ? k[0] : 0, kr = k ? k[1] : 0;
    const list = [];
    for (const o of snap.live) {
      const [dq, dr] = torusOff(owner.q, owner.r, o.q, o.r);
      if (hexLen(dq, dr) > R) continue;
      const me = o.key === owner.key;
      const g = me ? null : Glide.off(o.key, now);
      list.push({ p: o, me, dq: me ? 0 : dq - (g ? g[0] : 0) + kq, dr: me ? 0 : dr - (g ? g[1] : 0) + kr });
    }
    const seen = new Map();
    for (const w of list.slice().sort((a, b) => b.me - a.me)) {
      const h = Math.round(w.dq) + ',' + Math.round(w.dr);
      const n = seen.get(h) || 0;
      seen.set(h, n + 1);
      if (n) { w.dq += 0.3 * Math.cos(n * 2.1); w.dr += 0.3 * Math.sin(n * 2.1); }
    }
    // Back to front, like the tiles, so a pawn in the row in front overlaps.
    list.sort((a, b) => (a.dr + a.dq / 2) - (b.dr + b.dq / 2));
    for (const w of list) {
      const [x, y] = hexXY(w.dq, w.dr, size);
      drawPawn(ctx, x, y, size, w.p, !w.me);
    }
  }

  // ── Walking, not teleporting ──────────────────────────────────
  // /state is a snapshot a second, so a step arrives as a jump. A survivor who
  // moved one to three hexes since the last poll glides there instead, the
  // ground sliding under them the way the game's camera follows its pawn
  // (lerpPlayerPositions). Paced by timers, like the air: a TV browser that
  // never ticks rAF still has to walk.
  const GLIDE_MS = 520;
  const Glide = {
    pos: new Map(),   // key -> { q, r } last seen
    runs: new Map(),  // key -> { dq, dr, t0 }: the step still to animate
    timer: 0,

    note(snap, now) {
      for (const p of snap.live) {
        const was = this.pos.get(p.key);
        this.pos.set(p.key, { q: p.q, r: p.r });
        if (!was || (was.q === p.q && was.r === p.r)) continue;
        const [dq, dr] = torusOff(was.q, was.r, p.q, p.r);
        const n = hexLen(dq, dr);
        if (n >= 1 && n <= 3) this.runs.set(p.key, { dq, dr, t0: now });
        else this.runs.delete(p.key);
      }
    },

    // -> [dq, dr] of the step not yet walked, or null once they have arrived.
    off(key, now) {
      const g = this.runs.get(key);
      if (!g) return null;
      const t = (now - g.t0) / GLIDE_MS;
      if (t >= 1) return null;
      const left = 1 - t * t * (3 - 2 * t);
      return [g.dq * left, g.dr * left];
    },

    // Anybody still mid-step? Finished runs are dropped here, once their
    // last frame has been asked for.
    moving(now) {
      for (const [key, g] of this.runs) if (now - g.t0 >= GLIDE_MS) this.runs.delete(key);
      return this.runs.size > 0;
    },

    // Keep redrawing while anybody is mid-step, then once more at rest.
    kick() {
      if (this.timer) return;
      this.timer = setTimeout(() => {
        this.timer = 0;
        if (!ST.snap) return;
        const more = this.moving(Date.now());
        renderDisk(ST.snap, diskSubject, true);
        renderCams(ST.snap, diskSubject, true);
        if (more) this.kick();
      }, 40);
    },
  };

  // ══════════════════════════════════════════════════════════════
  // Grime
  //
  // Salvage, drawn procedurally in the house palette (FX_PAL's soot, rust
  // and brick rungs): plate steel mottled and rusting, scratched, riveted
  // with the stains running down from every rivet, shot through, taped up,
  // spray-painted and left to drip. The rule from the LCD holds -- generated
  // by maths, never looking like it: the shapes come off value noise and a
  // hash, so nothing is a clean circle or a straight line. THE ADMIRED is
  // painted on one plate; the panels are framed in it.
  // ══════════════════════════════════════════════════════════════
  const Grime = (() => {
    // lowbias32 and value noise, as observer-fx.js and the board have them
    const hash = (x) => { x >>>= 0; x ^= x >>> 16; x = Math.imul(x, 0x7feb352d); x ^= x >>> 15; x = Math.imul(x, 0x846ca68b); x ^= x >>> 16; return x >>> 0; };
    const h2 = (a, b) => hash((Math.imul(a >>> 0, 0x9E3779B1) + hash(b)) >>> 0);
    const U = (h) => (h >>> 8) / 16777216;
    const sm = (t) => t * t * (3 - 2 * t);
    function n2(x, y, s) {
      const fx = Math.floor(x), fy = Math.floor(y);
      const tx = sm(x - fx), ty = sm(y - fy);
      const s0 = h2(fy, s), s1 = h2(fy + 1, s);
      const a = U(h2(fx, s0)), b = U(h2(fx + 1, s0)), c = U(h2(fx, s1)), d = U(h2(fx + 1, s1));
      const ab = a + (b - a) * tx, cd = c + (d - c) * tx;
      return ab + (cd - ab) * ty;
    }
    function fbm(x, y, s) {
      let v = 0, a = 0.5, f = 1;
      for (let i = 0; i < 4; i++) { v += a * n2(x * f, y * f, s + i * 101); f *= 2.03; a *= 0.5; }
      return v / 0.9375;
    }
    function rng(seed) {
      let s = seed >>> 0;
      return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return U(hash(s)); };
    }

    // The steel itself, at half resolution and smoothed up: dark mottled
    // plate, rust blooming where the noise runs high (stretched downward,
    // because rust runs), bright worn edges, grain, soot settling at the foot.
    function steel(ctx, w, h, seed, o) {
      const sw = Math.max(2, Math.ceil(w / 2)), sh = Math.max(2, Math.ceil(h / 2));
      const lo = D.createElement('canvas'); lo.width = sw; lo.height = sh;
      const lc = lo.getContext('2d'), im = lc.createImageData(sw, sh), d = im.data;
      const k = 1 / 26, dark = o.dark || 1, rustAmt = o.rust == null ? 0.5 : o.rust;
      for (let y = 0; y < sh; y++) {
        for (let x = 0; x < sw; x++) {
          const m = fbm(x * k, y * k, seed);
          const r = fbm(x * k * 0.8 + 9.1, y * k * 0.35, seed + 7);
          const g = U(h2(x + y * 4099, seed));
          const e = Math.min(x, y, sw - 1 - x, sh - 1 - y) / Math.min(sw, sh);
          let R = 26 + 40 * m, G = 12 + 18 * m, B = 4 + 7 * m;
          const rust = sm(Math.max(0, Math.min(1, (r - (0.66 - rustAmt * 0.22)) / 0.14)));
          const rg = 0.55 + 0.45 * g;
          R += rust * (128 * rg - R); G += rust * (46 * rg - G); B += rust * (12 - B);
          // a dark rim round each bloom, where the paint lifts
          const rim = Math.max(0, 1 - Math.abs(r - (0.6 - rustAmt * 0.22)) / 0.03) * 0.35;
          R *= 1 - rim; G *= 1 - rim; B *= 1 - rim;
          const wear = e < 0.045 ? (1 - e / 0.045) * sm(n2(x * 0.35, y * 0.35, seed + 3)) : 0;
          R += wear * 70; G += wear * 34; B += wear * 10;
          const gr = (g - 0.5) * 16; R += gr; G += gr * 0.5; B += gr * 0.2;
          const soot = o.soot ? sm(Math.max(0, Math.min(1, (y / sh - 0.5) / 0.5))) * 0.6 * (0.55 + 0.45 * m) : 0;
          const f = (1 - soot) * dark;
          const i = (y * sw + x) * 4;
          d[i] = R * f; d[i + 1] = G * f; d[i + 2] = B * f; d[i + 3] = 255;
        }
      }
      lc.putImageData(im, 0, 0);
      ctx.imageSmoothingEnabled = true;
      ctx.drawImage(lo, 0, 0, w, h);
    }

    // Scratches: short wandering strokes, a bright gouge with a dark lip.
    function scratches(ctx, w, h, rnd, n) {
      ctx.save();
      ctx.lineCap = 'round'; ctx.lineJoin = 'round';
      for (let i = 0; i < n; i++) {
        let x = rnd() * w, y = rnd() * h, a = rnd() * Math.PI * 2;
        const segs = 3 + (rnd() * 7 | 0), len = 4 + rnd() * 16;
        const pts = [[x, y]];
        for (let s = 0; s < segs; s++) { a += (rnd() - 0.5) * 0.5; x += Math.cos(a) * len; y += Math.sin(a) * len; pts.push([x, y]); }
        for (const [dx, col] of [[1, 'rgba(0,0,0,0.35)'], [0, 'rgba(255,200,120,' + (0.07 + rnd() * 0.16).toFixed(3) + ')']]) {
          ctx.strokeStyle = col; ctx.lineWidth = 0.6 + rnd() * 0.8;
          ctx.beginPath();
          pts.forEach(([px, py], j) => (j ? ctx.lineTo(px + dx, py + dx) : ctx.moveTo(px + dx, py + dx)));
          ctx.stroke();
        }
      }
      ctx.restore();
    }

    // A rivet: a dome lit from the top left, its shadow, and the rust it has
    // been weeping down the plate for twenty years.
    function rivet(ctx, x, y, r, rnd, weep) {
      if (weep) {
        const len = r * (3 + rnd() * 9);
        const g = ctx.createLinearGradient(x, y, x, y + len);
        g.addColorStop(0, 'rgba(140,58,18,0.55)'); g.addColorStop(1, 'rgba(140,58,18,0)');
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.moveTo(x - r * 0.7, y);
        ctx.quadraticCurveTo(x - r * 0.5 + (rnd() - 0.5) * r, y + len * 0.6, x + (rnd() - 0.5) * r, y + len);
        ctx.quadraticCurveTo(x + r * 0.5 + (rnd() - 0.5) * r, y + len * 0.6, x + r * 0.7, y);
        ctx.fill();
      }
      ctx.fillStyle = 'rgba(0,0,0,0.6)';
      ctx.beginPath(); ctx.ellipse(x + r * 0.25, y + r * 0.3, r * 1.05, r * 0.95, 0, 0, Math.PI * 2); ctx.fill();
      const g = ctx.createRadialGradient(x - r * 0.35, y - r * 0.4, r * 0.1, x, y, r);
      g.addColorStop(0, '#D88A30'); g.addColorStop(0.35, '#7A4214'); g.addColorStop(1, '#1E0A00');
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill();
    }

    // A bullet hole: torn steel curling outward round a black hole, the paint
    // flaked off in a ring, a couple of cracks running away from it.
    function hole(ctx, x, y, r, rnd) {
      ctx.save();
      ctx.fillStyle = 'rgba(168,74,20,0.35)';
      ctx.beginPath();
      for (let i = 0; i <= 14; i++) {
        const a = i / 14 * Math.PI * 2, rr = r * (2.1 + rnd() * 0.9);
        const px = x + Math.cos(a) * rr, py = y + Math.sin(a) * rr;
        if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py);
      }
      ctx.fill();
      ctx.strokeStyle = 'rgba(255,190,110,0.55)'; ctx.lineWidth = Math.max(1, r * 0.22);
      for (let i = 0; i < 7; i++) {
        const a = rnd() * Math.PI * 2;
        ctx.beginPath();
        ctx.moveTo(x + Math.cos(a) * r * 0.8, y + Math.sin(a) * r * 0.8);
        ctx.lineTo(x + Math.cos(a + 0.2) * r * (1.4 + rnd() * 0.5), y + Math.sin(a + 0.2) * r * (1.4 + rnd() * 0.5));
        ctx.stroke();
      }
      ctx.fillStyle = '#000';
      ctx.beginPath();
      for (let i = 0; i <= 10; i++) {
        const a = i / 10 * Math.PI * 2, rr = r * (0.8 + rnd() * 0.35);
        const px = x + Math.cos(a) * rr, py = y + Math.sin(a) * rr;
        if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py);
      }
      ctx.fill();
      ctx.strokeStyle = 'rgba(0,0,0,0.55)'; ctx.lineWidth = 1;
      for (let i = 0; i < 2; i++) {
        let a = rnd() * Math.PI * 2, px = x + Math.cos(a) * r, py = y + Math.sin(a) * r;
        ctx.beginPath(); ctx.moveTo(px, py);
        for (let s = 0; s < 5; s++) { a += (rnd() - 0.5) * 0.9; px += Math.cos(a) * r * 0.9; py += Math.sin(a) * r * 0.9; ctx.lineTo(px, py); }
        ctx.stroke();
      }
      ctx.restore();
    }

    // Duct tape: a torn strip, the weave showing, dirt in the glue.
    function tape(ctx, x, y, len, wid, ang, rnd) {
      ctx.save();
      ctx.translate(x, y); ctx.rotate(ang);
      const jag = () => (rnd() - 0.5) * wid * 0.32;
      ctx.beginPath();
      ctx.moveTo(-len / 2 + jag(), -wid / 2);
      ctx.lineTo(len / 2 + jag(), -wid / 2);
      for (let i = 1; i <= 6; i++) ctx.lineTo(len / 2 + jag(), -wid / 2 + wid * i / 6);
      ctx.lineTo(-len / 2 + jag(), wid / 2);
      for (let i = 5; i >= 1; i--) ctx.lineTo(-len / 2 + jag(), -wid / 2 + wid * i / 6);
      ctx.closePath();
      ctx.fillStyle = 'rgba(132,118,92,0.9)';
      ctx.shadowColor = 'rgba(0,0,0,0.6)'; ctx.shadowBlur = 3; ctx.shadowOffsetY = 1;
      ctx.fill();
      ctx.shadowColor = 'transparent';
      ctx.clip();
      ctx.strokeStyle = 'rgba(40,30,18,0.25)'; ctx.lineWidth = 0.7;
      for (let i = -len; i < len; i += 3) { ctx.beginPath(); ctx.moveTo(i, -wid); ctx.lineTo(i + wid, wid); ctx.stroke(); }
      for (let i = 0; i < 14; i++) {
        ctx.fillStyle = 'rgba(30,14,4,' + (0.1 + rnd() * 0.25).toFixed(3) + ')';
        ctx.beginPath(); ctx.arc((rnd() - 0.5) * len, (rnd() - 0.5) * wid, 0.6 + rnd() * 2.2, 0, Math.PI * 2); ctx.fill();
      }
      const sheen = ctx.createLinearGradient(0, -wid / 2, 0, wid / 2);
      sheen.addColorStop(0, 'rgba(255,235,200,0.25)'); sheen.addColorStop(0.4, 'rgba(255,235,200,0)');
      ctx.fillStyle = sheen; ctx.fillRect(-len, -wid, len * 2, wid * 2);
      ctx.restore();
    }

    // Spray paint left to run: drips hanging off the underside of a line of
    // lettering, each a thin run ending in a bead.
    function drips(ctx, x0, x1, y, rnd, n, maxLen, col) {
      ctx.save();
      ctx.fillStyle = col; ctx.strokeStyle = col; ctx.lineCap = 'round';
      for (let i = 0; i < n; i++) {
        const x = x0 + rnd() * (x1 - x0), len = maxLen * (0.15 + rnd() * rnd()), w = 1 + rnd() * 2.2;
        ctx.lineWidth = w;
        ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + (rnd() - 0.5) * 1.5, y + len); ctx.stroke();
        ctx.beginPath(); ctx.ellipse(x, y + len, w * 0.9, w * 1.25, 0, 0, Math.PI * 2); ctx.fill();
      }
      ctx.restore();
    }

    // Cracked glass from one strike: crooked spokes, and the rings that join
    // them, each line with a dark twin so it reads as a break, not a drawing.
    function cracks(ctx, cx, cy, R, rnd) {
      ctx.save();
      ctx.lineJoin = 'round';
      const spokes = [], n = 7 + (rnd() * 4 | 0);
      for (let i = 0; i < n; i++) {
        let a = i / n * Math.PI * 2 + (rnd() - 0.5) * 0.5, x = cx, y = cy;
        const pts = [[x, y]];
        const steps = 6 + (rnd() * 5 | 0);
        for (let s = 0; s < steps; s++) {
          a += (rnd() - 0.5) * 0.35;
          const l = R / steps * (0.6 + rnd() * 0.9);
          x += Math.cos(a) * l; y += Math.sin(a) * l;
          pts.push([x, y]);
        }
        spokes.push(pts);
      }
      const line = (pts, col, wd, off) => {
        ctx.strokeStyle = col; ctx.lineWidth = wd;
        ctx.beginPath();
        pts.forEach(([x, y], j) => (j ? ctx.lineTo(x + off, y + off) : ctx.moveTo(x + off, y + off)));
        ctx.stroke();
      };
      for (const p of spokes) { line(p, 'rgba(0,0,0,0.5)', 1.6, 1); line(p, 'rgba(255,236,200,0.6)', 0.9, 0); }
      for (const ring of [2, 4]) {
        for (let i = 0; i < n; i++) {
          if (rnd() < 0.3) continue;
          const a = spokes[i][Math.min(ring, spokes[i].length - 1)], b = spokes[(i + 1) % n][Math.min(ring, spokes[(i + 1) % n].length - 1)];
          const mid = [(a[0] + b[0]) / 2 + (rnd() - 0.5) * 6, (a[1] + b[1]) / 2 + (rnd() - 0.5) * 6];
          line([a, mid, b], 'rgba(0,0,0,0.4)', 1.3, 1); line([a, mid, b], 'rgba(255,236,200,0.45)', 0.7, 0);
        }
      }
      // the white star where it was hit
      const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, R * 0.12);
      g.addColorStop(0, 'rgba(255,240,210,0.7)'); g.addColorStop(1, 'rgba(255,240,210,0)');
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(cx, cy, R * 0.12, 0, Math.PI * 2); ctx.fill();
      ctx.restore();
    }

    // A tile of plain plate for patterns (the panel frames), cached.
    let tileCv = null;
    function tile() {
      if (tileCv) return tileCv;
      tileCv = D.createElement('canvas'); tileCv.width = tileCv.height = 224;
      const c = tileCv.getContext('2d');
      steel(c, 224, 224, 913, { rust: 0.75 });
      scratches(c, 224, 224, rng(77), 26);
      return tileCv;
    }

    return { steel, scratches, rivet, hole, tape, drips, cracks, tile, rng, hash };
  })();

  // ── Where everything goes ─────────────────────────────────────
  // The survivors' ground is the show: everybody on the board gets a panel,
  // the camera subject's first (nearest the type) and marked, and between
  // them they take the whole stage right of the type -- THE ADMIRED steps
  // out of the way for them (observer.css). The area is cut into whichever
  // grid leaves each panel biggest, so one survivor gets all of it, two
  // split it, and five share a 3 x 2.
  function gridIn(z, n, gap) {
    let best = null;
    for (let cols = 1; cols <= n; cols++) {
      const rows = Math.ceil(n / cols);
      const cw = (z.w - (cols - 1) * gap) / cols, ch = (z.h - (rows - 1) * gap) / rows;
      if (cw <= 0 || ch <= 0) continue;
      // Scored by the smaller side, height counted a little long: the foot
      // takes some of it, and a panel wider than tall shows more ground.
      const score = Math.min(cw, ch * 1.15);
      if (!best || score > best.score) best = { cols, rows, cw: Math.min(cw, ch * 1.8), ch, score };
    }
    if (!best) return [];
    // Centred in the area, so a grid that does not fill it is not jammed
    // into one corner of it.
    const x0 = z.x + (z.w - best.cols * best.cw - (best.cols - 1) * gap) / 2;
    const used = Math.min(n, best.cols);
    const panels = [];
    for (let i = 0; i < n; i++) {
      const c = i % best.cols, r = Math.floor(i / best.cols);
      // A short last row is centred under the full rows above it.
      const inRow = r === best.rows - 1 ? n - r * best.cols : used;
      const rx = x0 + (best.cols - inRow) * (best.cw + gap) / 2;
      panels.push({ x: Math.round(rx + c * (best.cw + gap)), y: Math.round(z.y + r * (best.ch + gap)),
                    w: Math.round(best.cw), h: Math.round(best.ch) });
    }
    return panels;
  }

  // Right edge of the type actually on the stage, in stage px: the text
  // itself, not its 60%-wide boxes.
  function typeRight(stLeft) {
    let right = 0;
    const range = D.createRange();
    for (const el of [EL.who, EL.place, EL.greed, EL.doors, EL.admired]) {
      if (!el || !el.offsetParent) continue;
      if (el === EL.admired) { right = Math.max(right, el.getBoundingClientRect().right); continue; }
      const bar = el.querySelector('#w-bar');
      if (bar) right = Math.max(right, bar.getBoundingClientRect().right);
      const walk = D.createTreeWalker(el, NodeFilter.SHOW_TEXT);
      for (let t = walk.nextNode(); t; t = walk.nextNode()) {
        if (!t.nodeValue.trim()) continue;
        range.selectNodeContents(t);
        const b = range.getBoundingClientRect();
        if (b.width) right = Math.max(right, b.right);
      }
    }
    return right - stLeft;
  }

  // Everybody's panel, the subject's first. -> [{ o, b }]
  function camLayout(snap, p) {
    const st = EL.stage.getBoundingClientRect();
    const W = st.width, H = st.height;
    const gap = Math.max(6, Math.round(H * 0.022));
    const who = snap.live.slice();
    if (p) { const i = who.findIndex((o) => o.key === p.key); if (i > 0) who.unshift(who.splice(i, 1)[0]); }
    // Never narrower than the right 52%, whatever the type does.
    const L = Math.min(W * 0.48, Math.max(W * 0.3, typeRight(st.left) + gap * 2));
    const panels = gridIn({ x: L, y: gap, w: W - gap - L, h: H - 2 * gap }, who.length, gap);
    return who.map((o, i) => ({ o, b: panels[i] }));
  }

  // ── One survivor's panel ──────────────────────────────────────
  // Zoomed in on its owner the way their own phone is: the ground round
  // them, the pawn in the middle, anybody near on their own hex, and the
  // name and the condition word along the foot. The subject's comes off the
  // full vision disk (?pid=); everybody else's off /state's "near"
  // (game-server.hpp), the 19 hexes within 2.
  function footOf(b) { return Math.max(16, Math.round(Math.min(b.h * 0.14, b.w * 0.1))); }
  // Ring 1 whole and ring 2 cut by the frame: close enough to see what they
  // are standing on, wide enough to see what is next to it.
  function camSize(b) { return Math.min(b.w / 4.4, (b.h - footOf(b)) / (2 * SQ3 * 1.1)); }

  // What the TV has decided about each of them, on a strip of tape stuck to
  // their panel. Picked by state, then by seat and day, so it holds for a
  // while and changes at dawn instead of flickering every poll.
  const EPITHETS = {
    inside: ['WENT IN. NOTED.', 'DOOR SHUT BEHIND THEM', 'NOT WAVING', 'INSIDE. REGRETTABLY.', 'GONE LOOKING FOR TROUBLE'],
    crit:   ['MEASURE FOR A BOX', 'CALL IT', 'SAYING GOODBYES', 'ON THE WAY OUT', 'ASK FOR THEIR BOOTS'],
    warn:   ['LEAKING', 'SEEN BETTER DAYS', 'HOLDING. BARELY.', 'MOSTLY INTACT', 'STILL HAS MOST FINGERS'],
    dry:    ['DRY AS A BONE, SOON', 'EYEING THE OTHERS', 'LICKING ROCKS', 'WOULD KILL FOR A DRINK'],
    fine:   ['NOT DEAD YET', 'ALIVE, ALLEGEDLY', 'STILL TECHNICALLY FINE', 'UNREMARKABLE', 'TOO STUBBORN TO DIE', 'NOTHING TO REPORT. YET.'],
  };
  function epithet(o, day) {
    const cls = conditionClass(o);
    const k = o.encActive ? 'inside' : (o.water === 0 || o.food === 0) ? 'dry'
      : cls === 'crit' ? 'crit' : cls === 'warn' ? 'warn' : 'fine';
    const list = EPITHETS[k];
    return list[(o.pid * 7 + (day | 0) * 3) % list.length];
  }
  const camSeed = (o) => Grime.hash(o.pid * 977 + (o.connectMs | 0));

  function drawCam(ctx, snap, o, b, now, cells, R, focus) {
    const color = GD.ARCH_COLORS[o.arch] || '#E8A828';
    const foot = footOf(b), size = camSize(b);
    const seed = camSeed(o), rnd = Grime.rng(seed);
    const t = Math.max(6, Math.round(Math.min(b.w, b.h) * 0.032));   // the frame's steel
    ctx.save();
    ctx.beginPath(); ctx.rect(b.x, b.y, b.w, b.h); ctx.clip();
    ctx.fillStyle = '#060403'; ctx.fillRect(b.x, b.y, b.w, b.h);
    ctx.save();
    ctx.translate(b.x + b.w / 2, b.y + (b.h - foot) / 2);
    const k = Glide.off(o.key, now);
    const out = cells ? drawGround(ctx, snap, cells, size, k) : [];
    drawPawns(ctx, snap, o, R, size, k, now);
    ctx.restore();
    // Soot in the corners: a picture through a dirty window, not a map.
    const gx = b.x + b.w / 2, gy = b.y + (b.h - foot) / 2;
    const vg = ctx.createRadialGradient(gx, gy, Math.min(b.w, b.h) * 0.3, gx, gy, Math.max(b.w, b.h) * 0.72);
    vg.addColorStop(0, 'rgba(6,4,3,0)'); vg.addColorStop(1, 'rgba(6,4,3,0.7)');
    ctx.fillStyle = vg; ctx.fillRect(b.x, b.y, b.w, b.h - foot);
    // Somebody on their last legs gets the glass cracked over them.
    if (!o.encActive && conditionClass(o) === 'crit') {
      Grime.cracks(ctx, b.x + b.w * (0.25 + rnd() * 0.5), b.y + (b.h - foot) * (0.2 + rnd() * 0.5), Math.max(b.w, b.h) * 0.55, rnd);
    }
    // The foot: a strip of the same plate, darker, names stencilled on it.
    ctx.fillStyle = ctx.createPattern(Grime.tile(), 'repeat');
    ctx.fillRect(b.x, b.y + b.h - foot, b.w, foot);
    ctx.fillStyle = 'rgba(6,4,3,0.55)'; ctx.fillRect(b.x, b.y + b.h - foot, b.w, foot);
    ctx.fillStyle = 'rgba(0,0,0,0.8)'; ctx.fillRect(b.x, b.y + b.h - foot, b.w, 2);
    const fs = Math.round(foot * 0.66);
    ctx.font = fs + 'px ' + STENCIL;
    ctx.textBaseline = 'middle';
    const pad = Math.round(t + foot * 0.25), my = b.y + b.h - foot / 2 - t * 0.3;
    ctx.shadowColor = '#000'; ctx.shadowOffsetX = 1; ctx.shadowOffsetY = 2;
    ctx.textAlign = 'left'; ctx.fillStyle = color;
    ctx.fillText((focus ? '▶ ' : '') + o.name.toUpperCase(), b.x + pad, my);
    const cls = conditionClass(o);
    ctx.textAlign = 'right';
    ctx.fillStyle = o.encActive ? '#E8A828' : cls === 'crit' ? '#D8502A' : cls === 'warn' ? '#E09A20' : '#A07828';
    ctx.fillText(o.encActive ? 'INSIDE' : conditionWord(o), b.x + b.w - pad, my);
    ctx.shadowColor = 'transparent';
    // The epithet, in marker on a strip of tape over the top-left corner.
    const tf = Math.max(10, Math.round(foot * 0.46));
    ctx.font = tf + 'px ' + STENCIL;
    const label = epithet(o, snap.day);
    const tw = ctx.measureText(label).width + tf * 1.4, ang = -0.05 + rnd() * 0.06;
    const tx = b.x + t + tw / 2 + tf * 0.2, ty = b.y + t + tf * 0.9;
    Grime.tape(ctx, tx, ty, tw, tf * 1.5, ang, rnd);
    ctx.save();
    ctx.translate(tx, ty); ctx.rotate(ang);
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillStyle = 'rgba(14,8,4,0.92)';
    ctx.fillText(label, 0, tf * 0.06);
    ctx.restore();
    ctx.restore();
    drawFrame(ctx, b, t, focus ? '#E8A828' : color, focus, rnd);
    return { out, size, foot };
  }

  // The frame: a band of salvaged plate, a lip of shadow inside it, a stripe
  // of their colour painted round it and mostly chipped off, and rivets.
  function drawFrame(ctx, b, t, color, focus, rnd) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(b.x, b.y, b.w, b.h);
    ctx.rect(b.x + t, b.y + t, b.w - 2 * t, b.h - 2 * t);
    ctx.fillStyle = ctx.createPattern(Grime.tile(), 'repeat');
    ctx.fill('evenodd');
    ctx.strokeStyle = 'rgba(0,0,0,0.75)'; ctx.lineWidth = 2;
    ctx.strokeRect(b.x + t + 1, b.y + t + 1, b.w - 2 * t - 2, b.h - 2 * t - 2);
    ctx.strokeStyle = 'rgba(255,200,120,0.18)'; ctx.lineWidth = 1;
    ctx.strokeRect(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1);
    const i = t * 0.5;
    ctx.strokeStyle = color; ctx.globalAlpha = focus ? 0.95 : 0.75;
    ctx.lineWidth = Math.max(1.5, t * (focus ? 0.45 : 0.3)); ctx.lineCap = 'butt';
    const sides = [[b.x + i, b.y + i, b.x + b.w - i, b.y + i], [b.x + b.w - i, b.y + i, b.x + b.w - i, b.y + b.h - i],
                   [b.x + b.w - i, b.y + b.h - i, b.x + i, b.y + b.h - i], [b.x + i, b.y + b.h - i, b.x + i, b.y + i]];
    ctx.beginPath();
    for (const [x0, y0, x1, y1] of sides) {
      const L = Math.hypot(x1 - x0, y1 - y0), ux = (x1 - x0) / L, uy = (y1 - y0) / L;
      let p = 0;
      while (p < L) {
        const seg = L * (0.04 + rnd() * (focus ? 0.4 : 0.22));
        const q = Math.min(L, p + seg);
        ctx.moveTo(x0 + ux * p, y0 + uy * p); ctx.lineTo(x0 + ux * q, y0 + uy * q);
        p = q + L * (rnd() < 0.6 ? rnd() * (focus ? 0.03 : 0.07) : 0);
      }
    }
    ctx.stroke();
    ctx.globalAlpha = 1;
    const rr = Math.max(2, t * 0.3);
    for (const [x, y] of [[b.x + i, b.y + i], [b.x + b.w - i, b.y + i], [b.x + i, b.y + b.h - i], [b.x + b.w - i, b.y + b.h - i],
                          [b.x + b.w / 2, b.y + i], [b.x + b.w / 2, b.y + b.h - i]]) {
      Grime.rivet(ctx, x, y, rr, rnd, false);
    }
    ctx.restore();
  }

  function nearCells(o) {
    const n = o.near;
    if (!n || n.length < 57) return null;   // an older board: the pawn alone
    const out = [];
    let i = 0;
    for (let dr = -2; dr <= 2; dr++) {
      for (let dq = -2; dq <= 2; dq++) {
        if (hexLen(dq, dr) > 2) continue;
        const s = n[i + 2] | 0;
        out.push({ dq, dr, q: (o.q + dq + GD.COLS) % GD.COLS, r: (o.r + dr + GD.ROWS) % GD.ROWS,
                   terrain: n[i] | 0, variant: n[i + 1] | 0, shelter: s & 3, poi: !!(s & 4), resource: 0 });
        i += 3;
      }
    }
    return out;
  }

  function fitCanvas(cv, box) {
    const dpr = Math.min(root.devicePixelRatio || 1, 2);
    const W = Math.max(1, Math.round(box.width)), H = Math.max(1, Math.round(box.height));
    if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';   // the tiles are 110px; a 4K TV upscales them
    return { ctx, W, H, dpr };
  }

  // ── The camera subject's panel ────────────────────────────────
  // On its own canvas (#s-disk), because the air (wind-driven water, heat,
  // cloud shadows: Air below) is drawn over this one panel and its shimmer
  // reads the picture under it in place. Redrawn on a signature change (or
  // when a tile finishes decoding), not per poll, and every frame only while
  // somebody is mid-step.
  let diskSig = '';
  let diskSubject = null;
  function renderDisk(snap, p, anim) {
    diskSubject = p;
    const cv = EL['s-disk'];
    const view = p && snap.view && snap.view.pid === p.pid ? snap.view : null;
    const slot = p ? camLayout(snap, p).find((s) => s.o.key === p.key) : null;
    if (!slot || !slot.b) {
      if (diskSig !== 'empty') { diskSig = 'empty'; Air.setDisk(null); cv.getContext('2d').clearRect(0, 0, cv.width, cv.height); }
      return;
    }
    const b = slot.b, now = Date.now();
    const box = EL['disk-bg'].getBoundingClientRect();
    const moving = Glide.runs.size > 0;
    const sig = p.key + ':' + p.q + ',' + p.r + ':' + (view ? view.visR : -1) + ':'
      + Math.round(box.width) + 'x' + Math.round(box.height) + ':' + b.x + ',' + b.y + ',' + b.w + ',' + b.h + ':'
      + (view ? view.cells.map((c) => c.terrain + '.' + (c.variant | 0) + '.' + c.shelter + (c.poi ? 'P' : '') + (c.resource || '')).join('')
              : (p.near || []).join('.'))
      + ':' + conditionWord(p) + (p.encActive ? 'E' : '') + ':d' + snap.day
      + ':' + snap.live.map((o) => o.pid + '@' + o.q + ',' + o.r).join(';')
      + (snap.vac ? ':v' + snap.vac.q + ',' + snap.vac.r + ',' + snap.vac.st + ',' + snap.vac.rm
                  + ',' + snap.vac.dq + ',' + snap.vac.dr + ',' + snap.vac.kq + ',' + snap.vac.kr : '')
      + (snap.kane ? ':k' + snap.kane.gq + ',' + snap.kane.gr : '');   // the quest tiles (questTileKey)
    if (sig === diskSig && !moving && !anim) { Air.kick(); return; }
    diskSig = moving ? '' : sig;
    Art.onload = artLoaded;

    const { ctx, W, H, dpr } = fitCanvas(cv, box);
    // The view while it is the subject's own; "near" for the poll or two
    // after a cut, before the new pid's disk has come back.
    const cells = view ? view.cells : nearCells(p);
    const R = view ? Math.max(1, view.visR) : 2;
    const { out, size, foot } = drawCam(ctx, snap, p, b, now, cells, R, true);

    if (moving || !view || EL.stage.className !== 'open') { Air.setDisk(null); if (moving) Glide.kick(); return; }
    // The camera the game renderer would have for this panel: map hex
    // (q, r) lands at size * world(q, r) + (ox, oy), so the wind's
    // torus-tiled clouds and gusts sit on the same ground they do in the
    // players' hands. Clipped to the panel above its foot.
    const cx = b.x + b.w / 2, cy = b.y + (b.h - foot) / 2;
    const clip = { x: b.x, y: b.y, w: b.w, h: b.h - foot };
    Air.setDisk({
      W, H, dpr, size, q: view.q, r: view.r, clip,
      cells: out.filter((c) => Math.abs(c.x) < b.w / 2 + size && Math.abs(c.y) < clip.h / 2 + size)
        .map((c) => ({ cx: cx + c.x, cy: cy + c.y, q: c.q, r: c.r, terrain: c.terrain, tile: c.tile })),
      cam: { ox: cx - size * 1.5 * view.q, oy: cy - size * (SQ3 / 2 * view.q + SQ3 * view.r),
             meRp: { q: view.q, r: view.r } },
    });
  }

  // ── Everybody else's panels ───────────────────────────────────
  // On #s-cams. Both canvases sit over the plinth, so the panels stay up
  // through a scene as well -- the room is the subject's, the board is
  // still running.
  let camsSig = '';
  function renderCams(snap, p, anim) {
    const cv = EL['s-cams'];
    if (!cv) return;
    const now = Date.now();
    const box = EL.stage.getBoundingClientRect();
    const slots = camLayout(snap, p).filter((s) => s.b && (!p || s.o.key !== p.key));
    const moving = Glide.runs.size > 0;
    const sig = Math.round(box.width) + 'x' + Math.round(box.height) + '|'
      + slots.map((s) => s.b.x + ',' + s.b.y + ',' + s.b.w + ',' + s.b.h + ':' + s.o.pid + '@' + s.o.q + ',' + s.o.r
                        + ':' + (s.o.near || []).join('.') + ':' + conditionWord(s.o) + (s.o.encActive ? 'E' : '')).join('|')
      + '|' + snap.live.map((o) => o.pid + '@' + o.q + ',' + o.r).join(';') + '|d' + snap.day;
    if (sig === camsSig && !moving && !anim) return;
    camsSig = moving ? '' : sig;
    Art.onload = artLoaded;

    const { ctx } = fitCanvas(cv, box);
    for (const s of slots) drawCam(ctx, snap, s.o, s.b, now, nearCells(s.o), 2, false);
    if (moving) Glide.kick();
  }

  // A tile or a portrait finished decoding: both pictures redraw next poll.
  function artLoaded() { diskSig = ''; camsSig = ''; }

  // ── The air over the disk ─────────────────────────────────────
  // The game's own wind.js and ground-fx.js: water that sways and glitters,
  // heat shimmer, gusts over the scrub, glass glints, cloud shadows and the
  // odd dust devil, on a transparent canvas (#s-air) laid over the disk. The
  // disk stays a still picture, redrawn only when the ground changes; the
  // shimmer reads it in place (GroundFx opts.source), so a tick costs the
  // effects and nothing else. Paced by timers like observer-fx.js -- a TV
  // browser that never ticks rAF must not freeze the water -- and only while
  // the open-ground disk is actually on screen.
  const AIR_PERIOD = 40;   // ms: 25 ticks a second is plenty at three metres
  const Air = {
    gfx: null, disk: null, running: false,

    setDisk(d) {
      this.disk = d;
      if (d) this.kick();
    },

    kick() {
      if (this.running || !this.disk) return;
      if (typeof GroundFx !== 'function' || typeof Wind !== 'object') return;   // an older board's files
      if (!this.gfx) this.gfx = new GroundFx({ source: EL['s-disk'], shimmerEvery: 2 });
      this.running = true;
      setTimeout(() => this.tick(), 0);
    },

    tick() {
      const d = this.disk, snap = ST.snap, cv = EL['s-air'], base = EL['s-disk'];
      const on = d && snap && cv && base && EL.stage.className === 'open' && !D.hidden;
      if (!on) {
        this.running = false;
        if (cv) cv.getContext('2d').clearRect(0, 0, cv.width, cv.height);
        return;
      }
      const now = Date.now();
      try {
        if (cv.width !== base.width || cv.height !== base.height) { cv.width = base.width; cv.height = base.height; }
        const ctx = cv.getContext('2d');
        ctx.setTransform(d.dpr, 0, 0, d.dpr, 0, 0);
        ctx.clearRect(0, 0, d.W, d.H);
        // Only over the subject's panel, never its foot or its neighbours.
        ctx.save();
        if (d.clip) { ctx.beginPath(); ctx.rect(d.clip.x, d.clip.y, d.clip.w, d.clip.h); ctx.clip(); }
        Wind.update(now, snap.weather, 0, d.q, d.r);
        const g = this.gfx;
        g.beginFrame();
        // Only the hexes inside the panel (renderDisk filtered them):
        // animating the rest would be wasted work on a TV's GPU.
        for (const c of d.cells) g.collect(c.cx, c.cy, c, c.q, c.r, c.tile, 0);
        const sun = airSun(snap);
        g.drawGround(ctx, d.cam, now, d.size, d.W, d.H, sun, snap.weather);
        g.drawDevils(ctx, d.cam, now, d.size, d.W, d.H, sun, snap.weather);
        g.drawClouds(ctx, d.cam, d.size, d.W, d.H, sun, snap.weather);
        ctx.restore();
      } catch (e) {
        // Scenery must never take the screen down with it.
        console.warn('[observer] air:', e);
        this.running = false;
        return;
      }
      // A TV too slow for 25 a second gets fewer rather than none of the
      // rest of the page: never more than a third of the time in here.
      const took = Date.now() - now;
      setTimeout(() => this.tick(), Math.max(8, AIR_PERIOD - took, took * 2));
    },
  };

  // ground-fx.js's sun, on the TV's clock -- the world's dayTick, the same
  // arc that dims the whole page (render(), --night) -- rather than one
  // survivor's AP the way the game client tells it.
  function airSun(snap) { return worldSun(snap.dayTick); }

  // The sun on the board's clock: dawn at tick 0, dusk at 78% of the day
  // (the SS_DUSK sting, kane.hpp KANE_DUSK_TICK), night until the next dawn.
  // elev is its height: 1 at noon, 0 on the horizon, below 0 after dusk.
  // light: how lit the page is (--night, 1 = full day). day: direct sun at
  // all; noon: high and hot (heat shimmer); low: just over the horizon,
  // either end of the day (the glass catches it).
  const DUSK_FRAC = 0.78;
  function worldSun(dayTick) {
    const f = clamp(dayTick / DAY_TICKS, 0, 1);
    const elev = f < DUSK_FRAC ? Math.sin(Math.PI * f / DUSK_FRAC)
                               : -Math.sin(Math.PI * (f - DUSK_FRAC) / (1 - DUSK_FRAC));
    const sm = (e0, e1, x) => { const t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); };
    return { elev, light: sm(-0.35, 0.2, elev), day: sm(0, 0.3, elev), noon: sm(0.75, 0.97, elev),
             low: elev > 0 ? Math.max(0, 1 - Math.abs(elev - 0.22) / 0.2) : 0 };
  }

  // A still of the game's neon (vacancy.js), since the disk only redraws on
  // a change: a rusted box slumped on a pole off the town's upper right,
  // VACANCY in pink with one tube dead while the town is counting, every
  // tube steady red over NO VACANCY once it is full, and dead glass once it
  // is saved. The dead tube is fixed per town, like the game's.
  function drawVacSign(ctx, x, y, size, vac) {
    // lost (st 3 with rooms still on the count): the sign went with the town
    const full = vac.st === 3 && vac.rm === 0, dark = vac.st === 2 || (vac.st === 3 && vac.rm > 0);
    const bx = x + size * 0.3, by = y + size * 0.24, poleH = size * 0.88;
    const W = size * 1.36, H = size * 0.36;
    ctx.save();
    ctx.fillStyle = '#3a2618'; ctx.strokeStyle = '#080503'; ctx.lineWidth = 1;
    ctx.fillRect(bx - size * 0.03, by - poleH, size * 0.06, poleH);
    ctx.strokeRect(bx - size * 0.03, by - poleH, size * 0.06, poleH);
    ctx.translate(bx, by - poleH);
    ctx.rotate(0.08);
    ctx.fillStyle = '#33200f'; ctx.strokeStyle = '#0d0704'; ctx.lineWidth = Math.max(1, size * 0.025);
    ctx.fillRect(-W / 2, -H, W, H); ctx.strokeRect(-W / 2, -H, W, H);
    const word = 'NO VACANCY';
    const px = Math.round(H * 0.6);
    ctx.font = 'bold ' + px + 'px "Trebuchet MS", Arial, sans-serif';
    ctx.textBaseline = 'middle'; ctx.textAlign = 'left';
    const ws = word.split('').map((ch) => ctx.measureText(ch).width + px * 0.06);
    let xx = -ws.reduce((a, b) => a + b, 0) / 2;
    const deadAt = 3 + (((vac.q * 7 + vac.r * 13) % 7) + 7) % 7;   // one of the VACANCY letters
    word.split('').forEach((ch, i) => {
      const no = i < 2;
      const lit = !dark && (full || (!no && i !== deadAt));
      if (ch !== ' ') {
        if (lit) {
          const rgb = full ? '255,40,40' : '255,62,150';
          ctx.shadowColor = 'rgba(' + rgb + ',0.95)'; ctx.shadowBlur = px * 0.8;
          ctx.fillStyle = 'rgba(' + rgb + ',1)'; ctx.fillText(ch, xx, -H / 2);
          ctx.shadowBlur = 0;
          ctx.fillStyle = 'rgba(255,236,244,0.85)'; ctx.fillText(ch, xx + px * 0.03, -H / 2);
        } else {
          ctx.strokeStyle = no ? 'rgba(176,112,104,0.8)' : 'rgba(184,138,150,0.8)';
          ctx.lineWidth = Math.max(1, px * 0.06);
          ctx.strokeText(ch, xx, -H / 2);
        }
      }
      xx += ws[i];
    });
    ctx.restore();
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
      if (k.id === 'disk-bg' || k.id === 'admired' || k.id === 's-cams') continue;
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
    drawMedal();
    paintAdmired();
    EL['adm-sub'].textContent = ADM_SUBS[(snap.day | 0) % ADM_SUBS.length];
    const sig = rows.map((r) => r.e.key + ':' + r.e.last.score + ':' + r.trend
      + (r.e.key === Director.subject ? '*' : '')).join('|') + '|' + (Director.subject || '');
    if (sig === admSig) return;
    admSig = sig;
    EL.admired.style.display = rows.length ? '' : 'none';
    const sub = Director.subject ? (snap.byKey.get(Director.subject) || null) : null;
    typeNext(standingLine(sub));

    // The ladder moves. Each row is compared with where it stood last time:
    // a climb slides up from its old place, a drop slides down and shakes,
    // and points gained float up off the score while it rolls to the new
    // number. The very first draw has nothing to compare with and just sits.
    const first = !admPrev.size;
    const lead = rows.length ? rows[0].e.key : '';
    const rolls = [];
    EL['adm-rows'].innerHTML = rows.map((r, i) => {
      const was = admPrev.get(r.e.key);
      const moved = was ? was.rank - i : 0;
      const gain = was ? r.e.last.score - was.score : 0;
      const cls = (r.e.key === Director.subject ? 'on ' : '') + (i === 0 ? 'top ' : '')
        + (moved > 0 ? 'climb' : moved < 0 ? 'drop' : '');
      if (gain > 0) rolls.push({ i, from: was.score, to: r.e.last.score });
      return '<li class="' + cls.trim() + '"' + (moved ? ' style="--from:' + moved + '"' : '') + '>'
        + '<span class="a-n">' + (i + 1) + '</span>'
        + '<span class="a-name">' + esc(r.e.name) + '</span>'
        + '<span class="a-sc' + (gain > 0 ? ' bump' : '') + '">'
        +   (gain > 0 ? was.score : r.e.last.score).toLocaleString() + '</span>'
        + '<span class="a-t t-' + r.trend + '">'
        + (r.trend === 'up' ? '▴' : r.trend === 'dn' ? '▾' : '·')
        + '</span>'
        + (gain > 0 ? '<i class="a-plus">+' + gain.toLocaleString() + '</i>' : '')
        + (moved > 0 ? '<i class="a-tag">CLAWING UP</i>' : moved < 0 ? '<i class="a-tag">SLIPPING</i>' : '')
        + '</li>';
    }).join('');
    admPrev.clear();
    rows.forEach((r, i) => admPrev.set(r.e.key, { rank: i, score: r.e.last.score }));
    for (const r of rolls) rollScore(EL['adm-rows'].children[r.i], r.from, r.to);
    // A new name at the top of the ladder gets the stamp.
    if (!first && lead && lead !== admLead) stamp(rows[0].e.name);
    admLead = lead;
  }

  // Out in the open the ladder shares the type column with the name and the
  // place; when it would run up into them it shrinks from its bottom-left
  // corner instead, never below 55%.
  function fitAdmired() {
    const a = EL.admired;
    if (!a || !a.offsetHeight || EL.stage.className !== 'open') return;
    a.style.transform = '';
    const st = EL.stage.getBoundingClientRect(), place = EL.place.getBoundingClientRect();
    const room = st.bottom - place.bottom - st.height * 0.03 - (parseFloat(getComputedStyle(a).bottom) || 0);
    const k = clamp(room / a.offsetHeight, 0.55, 1);
    if (k < 0.995) { a.style.transformOrigin = '0 100%'; a.style.transform = 'scale(' + k.toFixed(3) + ')'; }
  }

  const admPrev = new Map();   // key -> { rank, score } as last drawn
  // Under the title, one a day: the small print on the honour.
  const ADM_SUBS = [
    'STANDINGS OF THE NOT YET DEAD', 'RANKED WHILE STOCKS LAST', 'POINTS DO NOT PASS TO NEXT OF KIN',
    'NO REFUNDS ON SURVIVAL', 'ENVY IS FREE. FOOD IS NOT.', 'ORDER OF DEPARTURE, REVERSED',
  ];
  let admLead = '';

  // The score counts up to its new value, an odometer and not a swap.
  // setTimeout-paced, like everything here that moves.
  function rollScore(li, from, to) {
    const el = li && li.querySelector('.a-sc');
    if (!el) return;
    const t0 = Date.now(), DUR = 700;
    const step = () => {
      if (!el.isConnected) return;
      const t = Math.min(1, (Date.now() - t0) / DUR);
      el.textContent = Math.round(from + (to - from) * (1 - (1 - t) * (1 - t))).toLocaleString();
      if (t < 1) setTimeout(step, 40);
    };
    step();
  }

  // The stamp across the ladder when the lead changes hands. The copy is the
  // ladder's own voice: an honour nobody asked for.
  const STAMP_LINES = ['NEW FAVOURITE (FOR NOW)', 'PROVISIONALLY ADMIRED', 'MOST LIKELY TO BE ROBBED',
    'PROMOTED. CONDOLENCES.', "TOP OF THE HEAP. DON'T LOOK DOWN."];
  let stampN = 0;
  function stamp(name) {
    const el = EL['adm-stamp'];
    if (!el) return;
    el.innerHTML = esc(STAMP_LINES[stampN++ % STAMP_LINES.length]) + '<b>' + esc(name.toUpperCase()) + '</b>';
    el.classList.remove('go');
    void el.offsetWidth;   // restart the animation
    el.classList.add('go');
  }

  // "120 short of THE AVERAGE MAN", typed out on the terminal a character
  // at a time whenever it changes, cursor left blinking after it.
  let nextText = null, nextTimer = 0;
  function typeNext(text) {
    const el = EL['adm-next'];
    if (text === nextText) return;
    nextText = text;
    clearTimeout(nextTimer);
    el.style.display = text ? '' : 'none';
    if (!text) { el.textContent = ''; return; }
    let n = 0;
    const tick = () => {
      n = Math.min(text.length, n + 2);
      el.textContent = text.slice(0, n);
      const cur = D.createElement('span');
      cur.className = 'cur';
      el.appendChild(cur);
      if (n < text.length) nextTimer = setTimeout(tick, 45);
    };
    tick();
  }

  // The plate THE ADMIRED is painted on: rusting steel, scratched, riveted,
  // shot twice, taped where it is coming apart, and the spray-painted title
  // left to run. Repainted only when the box changes size.
  let platePx = '';
  function paintAdmired() {
    const cv = EL['adm-plate'], box = EL.admired;
    if (!cv || !box || !box.offsetWidth) return;
    const W = box.offsetWidth, H = box.offsetHeight;
    const title = box.querySelector('h4 span');
    // Rects come back scaled when fitAdmired() has shrunk the box; the
    // plate is painted in its own unscaled px.
    const br = box.getBoundingClientRect(), sx = br.width / W || 1;
    const t0 = title ? title.getBoundingClientRect() : null;
    const tr = t0 && { left: (t0.left - br.left) / sx, right: (t0.right - br.left) / sx,
                       bottom: (t0.bottom - br.top) / sx, height: t0.height / sx, width: t0.width / sx };
    const key = W + 'x' + H + ':' + (tr ? Math.round(tr.left) + ',' + Math.round(tr.width) : '');
    if (key === platePx) return;
    platePx = key;
    const dpr = Math.min(root.devicePixelRatio || 1, 2);
    cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const rnd = Grime.rng(0xAD1);
    Grime.steel(ctx, W, H, 4242, { soot: true, rust: 0.42, dark: 0.72 });
    Grime.scratches(ctx, W, H, rnd, Math.round(W * H / 2600));
    if (tr && tr.width) {
      Grime.drips(ctx, tr.left, tr.right, tr.bottom - tr.height * 0.2,
        rnd, 11, tr.height * 1.4, 'rgba(224,124,28,0.75)');
    }
    Grime.hole(ctx, W * 0.87, H * 0.7, Math.max(3, W * 0.011), rnd);
    Grime.hole(ctx, W * 0.93, H * 0.79, Math.max(2, W * 0.007), rnd);
    const ri = Math.max(8, W * 0.022), rr = Math.max(3, W * 0.009);
    for (const [x, y] of [[ri, ri], [W - ri, ri], [ri, H - ri], [W - ri, H - ri], [W / 2, ri], [W / 2, H - ri]]) {
      Grime.rivet(ctx, x, y, rr, rnd, y < H / 2);
    }
    Grime.tape(ctx, W * 0.93, H * 0.07, W * 0.22, Math.max(12, H * 0.065), 0.62, rnd);
    Grime.tape(ctx, W * 0.06, H * 0.93, W * 0.17, Math.max(12, H * 0.06), 0.55, rnd);
  }

  // The medal by the title: a bottle cap, the currency of better days, hung
  // off a bent nail on a twist of wire and stamped 1ST by somebody with a
  // hammer. CSS swings it.
  let medalPx = 0;
  function drawMedal() {
    const cv = EL['adm-medal'];
    if (!cv) return;
    const r = cv.getBoundingClientRect();
    const dpr = Math.min(root.devicePixelRatio || 1, 2);
    const pw = Math.round(r.width * dpr), ph = Math.round(r.height * dpr);
    if (!pw || !ph || pw === medalPx) return;
    medalPx = pw;
    cv.width = pw; cv.height = ph;
    const ctx = cv.getContext('2d');
    const rnd = Grime.rng(1977);
    const cx = pw / 2, R = pw * 0.42, cy = ph - R - pw * 0.05;
    // the wire, twisted down from the nail to a hole punched in the cap
    ctx.strokeStyle = '#8A7A5C'; ctx.lineWidth = Math.max(1, pw * 0.035); ctx.lineCap = 'round';
    ctx.beginPath(); ctx.moveTo(cx, pw * 0.06);
    const top = cy - R * 0.78, steps = 6;
    for (let i = 1; i <= steps; i++) ctx.lineTo(cx + (i % 2 ? 1 : -1) * pw * 0.05, pw * 0.06 + (top - pw * 0.06) * i / steps);
    ctx.stroke();
    ctx.fillStyle = '#5A4A36'; ctx.fillRect(cx - pw * 0.09, 0, pw * 0.18, pw * 0.07);   // the nail head
    // the crimp: 21 teeth, no two pressed the same
    ctx.beginPath();
    for (let i = 0; i <= 42; i++) {
      const a = i / 42 * Math.PI * 2, rr = i % 2 ? R : R * (0.85 + rnd() * 0.05);
      const x = cx + Math.cos(a) * rr, y = cy + Math.sin(a) * rr;
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    }
    ctx.closePath();
    let g = ctx.createRadialGradient(cx - R * 0.3, cy - R * 0.35, R * 0.1, cx, cy, R);
    g.addColorStop(0, '#E8A030'); g.addColorStop(0.55, '#8C3A12'); g.addColorStop(1, '#2E1206');
    ctx.fillStyle = g; ctx.fill();
    ctx.strokeStyle = '#0C0400'; ctx.lineWidth = Math.max(1, pw * 0.02); ctx.stroke();
    // the face, dented
    g = ctx.createRadialGradient(cx + R * 0.2, cy + R * 0.25, R * 0.05, cx, cy, R * 0.72);
    g.addColorStop(0, '#6E2C0E'); g.addColorStop(1, '#B05A18');
    ctx.fillStyle = g;
    ctx.beginPath(); ctx.arc(cx, cy, R * 0.72, 0, Math.PI * 2); ctx.fill();
    // rust eating in from the rim
    for (let i = 0; i < 9; i++) {
      const a = rnd() * Math.PI * 2, d = R * (0.45 + rnd() * 0.35);
      ctx.fillStyle = 'rgba(60,20,6,' + (0.35 + rnd() * 0.4).toFixed(2) + ')';
      ctx.beginPath(); ctx.arc(cx + Math.cos(a) * d, cy + Math.sin(a) * d, R * (0.05 + rnd() * 0.09), 0, Math.PI * 2); ctx.fill();
    }
    // the stamp, hammered in crooked
    ctx.save();
    ctx.translate(cx, cy + R * 0.04); ctx.rotate(-0.12);
    ctx.font = Math.round(R * 0.62) + 'px ' + STENCIL;
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillStyle = 'rgba(255,214,140,0.35)'; ctx.fillText('1ST', 1, 1);
    ctx.fillStyle = '#1E0A00'; ctx.fillText('1ST', 0, 0);
    ctx.restore();
    // the hole the wire goes through
    ctx.fillStyle = '#000';
    ctx.beginPath(); ctx.arc(cx, top + R * 0.05, Math.max(1, pw * 0.035), 0, Math.PI * 2); ctx.fill();
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
    // The board's clock: its wind spreads the fire and carries the Doom's
    // scent, so the air drawn over the disk follows it (wind.js).
    if (raw && raw.wt != null && typeof Wind === 'object') Wind.serverClock(raw.wt);
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
  OBS.handle = handle; OBS.Art = Art; OBS.Glide = Glide;

  if (D.readyState === 'loading') D.addEventListener('DOMContentLoaded', boot);
  else boot();
}(typeof globalThis !== 'undefined' ? globalThis : this));
