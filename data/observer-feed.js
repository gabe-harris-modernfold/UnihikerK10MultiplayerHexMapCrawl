// ── The synthetic feed (?feed=fake) ──────────────────────────────
// A scripted six-player run that walks players through a REAL encounter file
// (data/encounters/urban/1.json — the pharmacy) and kills everybody on a
// timer. It is the only way to test the five acts, the greed meter, the exit
// inference and the obituaries deterministically, and the only way to tune
// line banks with no hardware and no patience.
//
// It produces raw /state-shaped objects, so it enters observer.js through the
// same normalise() -> diff() -> director -> narrator -> render path the board
// drives. Nothing downstream knows the difference. /enc is still fetched for
// real, from whatever is serving the page.
//
// This file is dev-only, and like the rest of the observer it is NOT in
// data/web-assets.json.
(function (root) {
  'use strict';
  const OBS = root.OBS || (root.OBS = {});

  const ARCH = ['Guide', 'Quartermaster', 'Medic', 'Mule', 'Scout', 'Endurer'];
  const DAY_TICKS = 3000;
  const INV_SLOTS_MAX = 18;
  const SCRIPT_LEN = 380;          // ticks; then a fresh generation takes the board

  function mkPlayer(pid, gen, now) {
    return {
      pid, conn: false,
      // A new generation gets a NEW connectMs, which is what makes the
      // recycled slot a different survivor to the shadow roster.
      connectMs: 1000 + gen * 100000 + pid * 137,
      name: ARCH[pid], arch: pid, archName: ARCH[pid],
      q: 20 + pid * 3, r: 14 + (pid % 3) * 5,
      ll: 7, llCap: 7 + (pid === 3 ? 2 : 0),
      food: 6, water: 6, rad: 0, mp: 6,
      wounds: [0, 0], resting: false,
      score: 0, steps: 0,
      inv: [0, 0, 0, 0, 0],
      invType: new Array(INV_SLOTS_MAX).fill(0),
      invQty: new Array(INV_SLOTS_MAX).fill(0),
      equip: [0, 0, 0, 0, 0],
      skills: [1, 1, 1, 1, 1],
      enc: null,
    };
  }

  // ── The script ────────────────────────────────────────────────
  // [tick, fn(S)]. Applied once, in order. Everything the observer is
  // supposed to detect appears here at least once, and the pharmacy is walked
  // three different ways: banked, aborted, and straight into the cage trap.
  const BEATS = [
    [1,   (S) => { conn(S, 0); conn(S, 1); conn(S, 2); }],
    [8,   (S) => conn(S, 3)],
    [12,  (S) => { S.p[0].score += 14; S.p[0].inv[4] += 2; }],                    // HAUL

    // ── The pharmacy, banked. All five acts, in order. ──────────
    [20,  (S) => encOpen(S, 1, 'urban', 1, 'entry', false, [0, 0, 0, 0, 0])],     // THRESHOLD
    [27,  (S) => { S.p[1].ll -= 3; S.p[1].wounds[1] += 1; }],                     // PRICE: shelf_collapse
    [35,  (S) => encMove(S, 1, 'dispensary', true, [0, 0, 0, 1, 0])],             // DOOR + CAN_LEAVE + DEEPER
    [40,  (S) => encOpen(S, 2, 'urban', 3, 'entry', false, [0, 0, 0, 0, 0])],     // a SECOND scene while the camera is locked -> queued + promised
    [45,  (S) => encMove(S, 1, 'dispensary', true, [0, 0, 0, 2, 0])],             // DEEPER
    [52,  (S) => encMove(S, 1, 'stockroom', true, [1, 0, 0, 3, 0])],              // DOOR + TERMINAL
    [60,  (S) => encBank(S, 1)],                                                  // EXIT: banked

    [64,  (S) => encMove(S, 2, 'ground_units', true, [0, 1, 0, 0, 1])],            // the promised scene, once the camera arrives
    [70,  (S) => { S.p[2].score = 330; }],
    [72,  (S) => encBank(S, 2)],
    [74,  (S) => { S.p[2].score = 372; }],                                        // PASSED_THE_DEAD (TILLY MOSS 350)
    [80,  (S) => conn(S, 4)],
    [88,  (S) => dawn(S)],

    // ── The pharmacy, walked away from. The good-judgement beat. ─
    [95,  (S) => encOpen(S, 3, 'urban', 1, 'entry', false, [0, 0, 0, 0, 0])],
    [104, (S) => encMove(S, 3, 'dispensary', true, [0, 0, 0, 2, 0])],
    [118, (S) => encAbort(S, 3)],                                                 // EXIT: abort (tc ticks)

    [126, (S) => { S.weather = 2; }],                                             // WEATHER_TURN: storm
    [132, (S) => flipGround(S)],                                                  // GROUND_TURNED under the camera

    // ── The pharmacy, into the cage. cage_trap: empty penalty, ends. ─
    [140, (S) => encOpen(S, 0, 'urban', 1, 'entry', false, [0, 0, 0, 0, 0])],
    [150, (S) => encMove(S, 0, 'dispensary', true, [0, 0, 0, 2, 0])],
    [162, (S) => encTrap(S, 0)],                                                  // EXIT: hazard, loot gone

    [172, (S) => { S.tc = 5; }],                                                  // ESCALATION
    [178, (S) => { S.p[2].food = 0; S.p[2].water = 1; }],                          // HUNGER
    [184, (S) => { S.p[2].ll = 3; }],                                             // FAILING
    [190, (S) => { S.p[2].ll = 1; }],
    [198, (S) => kill(S, 2)],                                                     // first death -> THE REAPING

    [206, (S) => conn(S, 5)],
    [230, (S) => { S.p[4].wounds[1] += 2; S.p[4].ll = 2; }],                       // MAULED
    [232, (S) => kill(S, 4)],                                                     // TEETH (inside the 3-sample window)

    // ── Died in the scene. The ledger names the room. ───────────
    [246, (S) => encOpen(S, 3, 'urban', 1, 'entry', false, [0, 0, 0, 0, 0])],
    [254, (S) => { S.p[3].ll -= 4; S.p[3].wounds[1] += 1; }],                      // PRICE: ambush_outside
    [262, (S) => { S.p[3].ll = 1; }],
    [268, (S) => kill(S, 3)],                                                     // SCENE obituary

    [280, (S) => { S.p[0].score += 60; }],
    [292, (S) => kill(S, 0)],                                                     // healthy -> WALKED_OUT, no eulogy
    [300, (S) => dawn(S)],
    [306, (S) => { S.p[1].food = 5; }],
    [310, (S) => { S.p[1].water = 0; }],
    [318, (S) => { S.p[1].ll = 4; }],
    [330, (S) => { S.p[1].ll = 1; }],
    [338, (S) => kill(S, 1)],                                                     // THIRST
    [352, (S) => { S.p[5].score += 90; S.tc = 9; }],                               // ESCALATION
    [366, (S) => { S.p[5].ll = 2; }],
    [374, (S) => kill(S, 5)],
  ];

  // step() walks this in order and never looks back, so it has to be sorted
  // by tick -- sorted here rather than trusted, because the table is edited by
  // hand and a beat inserted in the wrong place would silently never fire.
  BEATS.sort((a, b) => a[0] - b[0]);

  function conn(S, pid) { S.p[pid].conn = true; }
  function kill(S, pid) { S.p[pid].conn = false; S.p[pid].enc = null; }
  function dawn(S) { S.day++; S.dayTick = 0; }

  function encOpen(S, pid, biome, id, node, canBank, loot) {
    S.p[pid].enc = { biome, id, node, canBank, loot: loot.slice() };
    S.tc = Math.min(20, S.tc + 1);    // enc_start bumps the clock, same as the board
  }
  function encMove(S, pid, node, canBank, loot) {
    const e = S.p[pid].enc;
    if (!e) return;
    e.node = node; e.canBank = canBank; e.loot = loot.slice();
  }
  function encBank(S, pid) {
    const p = S.p[pid], e = p.enc;
    if (!e) return;
    for (let i = 0; i < 5; i++) { p.inv[i] += e.loot[i]; p.score += e.loot[i] * 3; }
    p.enc = null;
  }
  // Walked out on purpose: loot gone, nothing damaged, and the threat clock
  // ticks — which is the one thing that tells this apart from the cage trap.
  function encAbort(S, pid) { S.p[pid].enc = null; S.tc = Math.min(20, S.tc + 1); }
  // The cage trap: ends_encounter with an empty penalty block. Loot gone,
  // nothing damaged, and NO clock tick.
  function encTrap(S, pid) { S.p[pid].enc = null; }
  // GROUND_TURNED only fires for a survivor who STOOD STILL while the hex
  // under them changed, and only on the hex the camera is looking at, so the
  // fixture has to pin whoever is on screen and flip the ground beneath them.
  function flipGround(S) {
    const p = S.p[S.lastPid] || S.p.find((x) => x.conn) || S.p[0];
    S.flipQ = p.q; S.flipR = p.r; S.groundFlip = true;
    p.pinUntil = S.tick + 8;
  }

  // A deterministic board under the camera, so the hex disk has something to
  // draw and GROUND_TURNED has something to turn.
  function terrainAt(S, q, r) {
    if (S.groundFlip && q === S.flipQ && r === S.flipR) return 5;   // Flooded Ruins
    const h = ((q * 73856093) ^ (r * 19349663)) >>> 0;
    return h % 12;
  }

  function viewFor(S, pid) {
    const p = S.p[pid];
    if (!p || !p.conn) return null;
    const visR = 4;
    const cells = [];
    for (let dr = -visR; dr <= visR; dr++) {
      for (let dq = -visR; dq <= visR; dq++) {
        const s = -(dq + dr);
        if (Math.abs(dq) + Math.abs(dr) + Math.abs(s) > 2 * visR) continue;
        const q = p.q + dq, r = p.r + dr;
        const h = ((q * 2654435761) ^ (r * 40503)) >>> 0;
        const t = terrainAt(S, q, r);
        cells.push({
          q, r, dq, dr,
          terrain: t, terrainName: '',
          shelter: h % 23 === 3 ? 1 : 0,
          resource: h % 17 === 0 ? (h % 5) + 1 : 0,
          resourceName: '', amount: h % 17 === 0 ? 2 : 0,
          footprints: 0, tireTrack: false,
          // Which /img/hex<Name><N>.png the hex wears. No vc on this feed, so
          // the observer wraps it against nothing and falls back to variant 0
          // -- the field is here so the SHAPE matches the board, not to make
          // the fixture pretty.
          variant: h % 10,
          poi: h % 31 === 7,
        });
      }
    }
    return { pid, name: p.name, q: p.q, r: p.r, visR, cells };
  }

  function build(S, pid) {
    const players = S.p.map((p) => {
      const blk = {
        pid: p.pid, conn: p.conn, wsClientId: p.conn ? p.pid + 1 : 0,
        // The firmware sets connectMs on pick and only clears it on a world
        // reset -- a slot that empties keeps its stale value. Mirrored here so
        // the observer is tested against the board’s actual behaviour.
        connectMs: p.connectMs, lastMoveMs: 0,
        name: p.name, arch: p.arch, archName: p.archName,
        invSlots: 8, invSlotsEff: 8, equip: p.equip.slice(),
        q: p.q, r: p.r,
        ll: Math.max(0, p.ll), llCap: p.llCap,
        food: p.food, water: p.water, rad: p.rad, mp: p.mp,
        wounds: p.wounds.slice(), resting: p.resting, radClean: false,
        fThreshBelow: 0, wThreshBelow: 0,
        skills: p.skills.slice(), inv: p.inv.slice(),
        invType: p.invType.slice(), invQty: p.invQty.slice(),
        score: p.score, steps: p.steps,
        encActive: !!p.enc,
      };
      if (p.enc) {
        blk.encQ = p.q; blk.encR = p.r; blk.encNode = p.enc.node;
        blk.encId = p.enc.id; blk.encBiome = p.enc.biome;
        blk.encCanBank = p.enc.canBank; blk.encLoot = p.enc.loot.slice();
      }
      return blk;
    });
    const out = {
      day: S.day, dayTick: S.dayTick, tickId: S.tickId,
      tc: S.tc, weather: S.weather,
      connected: S.p.filter((p) => p.conn).length,
      evtQueue: 0,
      mem: { heap: 180000, minHeap: 150000, maxBlock: 90000, psram: 4000000, uptimeMs: S.tick * 1000 },
      rtc: { synced: false },
      map: { cells: 4275, shelters: 180, impShelters: 4, pois: 138,
             res: { water: 120, food: 120, fuel: 120, med: 120, scrap: 120 }, terrain: [] },
      players,
    };
    const v = viewFor(S, pid);
    if (v) out.view = v;
    return out;
  }

  function step(S) {
    S.tick++;
    S.tickId++;
    S.dayTick = (S.dayTick + 12) % DAY_TICKS;

    while (S.next < BEATS.length && BEATS[S.next][0] <= S.tick) {
      BEATS[S.next][1](S);
      S.next++;
    }

    // Slow drift so the meters are never completely still and the low-severity
    // render-only events (PICKING, LOOT, PATCHED) actually happen.
    for (const p of S.p) {
      if (!p.conn) continue;
      p.steps += (S.tick % 3 === 0) ? 1 : 0;
      if (S.tick % 7 === 0 && !p.enc) p.score += 1 + (p.pid % 3);
      if (S.tick % 23 === 0 && p.food > 0) p.food--;
      if (S.tick % 29 === 0 && p.water > 0) p.water--;
      // Drift nothing that reads as a hazard while somebody is in a scene:
      // a stray rad tick mid-encounter is a PRICE the script did not write,
      // and this file exists to be deterministic.
      if (S.tick % 41 === 0 && p.rad < 10 && !p.enc) p.rad++;
      if (S.tick % 19 === 0 && !p.enc && S.tick > (p.pinUntil || 0)) p.q += (p.pid % 2) ? 1 : -1;
    }

    // The run does not end. A fresh generation takes the board with new
    // connectMs values, which is also the only way to exercise a recycled
    // slot not being mistaken for a recovery.
    if (S.tick > SCRIPT_LEN) {
      S.gen++;
      S.tick = 0; S.next = 0;
      S.tc = Math.max(0, S.tc - 6);
      S.weather = (S.weather + 1) % 6;
      S.groundFlip = false;
      S.p = S.p.map((_, i) => mkPlayer(i, S.gen, 0));
    }
  }

  OBS.FakeFeed = {
    create(qs) {
      const S = {
        tick: 0, tickId: 0, next: 0, gen: 0,
        day: 1, dayTick: 0, tc: 0, weather: 0,
        groundFlip: false, flipQ: -1, flipR: -1, lastPid: 0,
        p: [],
      };
      S.p = [0, 1, 2, 3, 4, 5].map((i) => mkPlayer(i, 0, 0));
      const from = parseInt((qs && qs.get('t')) || '0', 10) || 0;
      for (let i = 0; i < from; i++) step(S);   // ?t=N to jump to a beat
      return {
        snapshot(pid) {
          S.lastPid = pid >= 0 ? pid : S.lastPid;
          step(S);
          return build(S, pid >= 0 ? pid : (S.p.find((p) => p.conn) || { pid: 0 }).pid);
        },
        state: S,
      };
    },
  };
}(typeof globalThis !== 'undefined' ? globalThis : this));
