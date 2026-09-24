// ── The scene layer ──────────────────────────────────────────────
// Everything that turns "somebody is in an encounter" into "somebody is in
// the Gutted Pharmacy looking at a padlocked dispensary cage".
//
// /state gives encActive, encNode, encCanBank, encLoot — and, since the three
// lines added to game-server.hpp for this screen, encBiome + encId, which are
// exactly the GET /enc?biome=&id= address of the file being played. That file
// is 111-files-worth of hand-written scene prose, and it is the largest body
// of voice in the project. This module fetches it once and never again.
//
// /enc TAKES G.mutex (500 ms timeout) AND READS SD. It is not pollable. One
// fetch per file for the life of the page; the result is cached forever.
(function (root) {
  'use strict';
  const OBS = root.OBS || (root.OBS = {});

  // Loot/penalty resource order, matching pendingLoot[5] in the .ino and
  // res 0-4 in an encounter file's loot entries.
  const RES_SHORT = ['water', 'food', 'fuel', 'med', 'scrap'];
  // Hazard penalty blocks key resources by name; same order.
  const PEN_RES_KEYS = ['water', 'food', 'fuel', 'med', 'scrap'];
  // Skill names as the stakes board shows them. SK_NAMES in game-data.js is
  // the long form the game client prints; a TV at three metres wants the stub.
  const SK_SHORT = ['NAV', 'FORAGE', 'SCAV', 'SHELT', 'ENDURE'];

  // A failed fetch is cached like a successful one — /enc must never be
  // polled — but a genuinely transient blip should not blank the scene for
  // the rest of the evening, so ONE retry is allowed, and only when the file
  // is opened again by a fresh THRESHOLD.
  const MAX_RETRIES = 1;

  const Scene = {
    cache: new Map(),   // "biome/id" -> { json, err, tries, pending }
    fetches: 0,         // HTTP requests actually issued — the Phase 5 check

    key(biome, id) { return String(biome || '') + '/' + (id | 0); },

    // Cached file or null. Synchronous: the render never waits.
    get(biome, id) {
      const e = this.cache.get(this.key(biome, id));
      return e && e.json ? e.json : null;
    },

    // Fetch once, ever. `fresh` marks a new THRESHOLD, which is the only
    // moment a previously failed file is allowed another go.
    ensure(host, biome, id, fresh) {
      const k = this.key(biome, id);
      if (!biome || !(id >= 0)) return Promise.resolve(null);
      let e = this.cache.get(k);
      if (!e) { e = { json: null, err: null, tries: 0, pending: null }; this.cache.set(k, e); }
      if (e.json) return Promise.resolve(e.json);
      if (e.pending) return e.pending;
      if (e.err && !(fresh && e.tries <= MAX_RETRIES)) return Promise.resolve(null);

      e.tries++;
      this.fetches++;
      const url = host + '/enc?biome=' + encodeURIComponent(biome) + '&id=' + (id | 0);
      e.pending = fetch(url, { cache: 'force-cache' })
        .then((r) => (r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status))))
        .then((j) => { e.json = j; e.err = null; e.pending = null; return j; })
        .catch((err) => { e.err = err; e.pending = null; return null; });
      return e.pending;
    },

    // ── Placeholders ──────────────────────────────────────────────
    // "The {{adjective}} Pharmacy" is Gutted on one run and Bleached on the
    // next; the file holds the LIST, not the pick, and the pick lives only in
    // the player's own client. Choosing one here would contradict the screen
    // in their hand, so the observer says the bare noun instead. The article
    // rule keeps mid-sentence placeholders ("a {{find}} —") from leaving a
    // dangling article on a television.
    despell(s) {
      return String(s || '')
        // "a {{find}} —" would leave a dangling article on a television, so
        // the article goes with the placeholder when one is followed by
        // punctuation.
        .replace(/\b(?:a|an|the)\s+\{\{\w+\}\}(?=\s*[—\-,.;:])/gi, '')
        .replace(/\{\{\w+\}\}\s*/g, '')
        // Close up space before ,.;: only. NOT before an em-dash: the prose
        // uses spaced em-dashes throughout and tightening them turned "still
        // padlocked — someone welded it shut" into "padlocked— someone".
        .replace(/\s+([,.;:])/g, '$1')
        .replace(/\s{2,}/g, ' ')
        .trim();
    },

    title(json) {
      if (!json) return 'somewhere';
      return this.despell(json.title) || 'somewhere';
    },

    node(json, key) {
      if (!json || !json.nodes) return null;
      return json.nodes[key] || null;
    },

    // Node keys are written as identifiers ("rear_office"); on screen they are
    // the name of a room.
    roomName(key) {
      const k = String(key || '').replace(/_/g, ' ').trim();
      if (!k) return 'room';
      return k === 'entry' || k === 'start' ? 'way in' : k;
    },

    nodeText(json, key) {
      const n = this.node(json, key);
      return n ? this.despell(n.text) : '';
    },

    // The prose trimmed to one clause. The narrator prefers the file's own
    // words to any bank line — a generic template laid over hand-written
    // scene text is a downgrade — but it gets one line, not a paragraph.
    clause(text) {
      const t = String(text || '').trim();
      if (!t) return '';
      // Whole sentences, taken until there is enough of one to be a line.
      // A single-sentence cut was wrong at both ends: "You stop." is not a
      // line, and a 200-word paragraph is not one either.
      let out = '';
      const re = /[^.!?]*[.!?]/g;
      let m;
      while ((m = re.exec(t)) !== null) {
        out += m[0];
        if (out.trim().length >= 25 || out.length > 150) break;
      }
      out = (out || t.slice(0, 140)).trim();
      // Too long for a TV line. Cut at a clause boundary rather than mid-word:
      // a hard character cut ended one of the tower's rooms on "that means
      // fire or.", which is worse than saying less.
      if (out.length > 160) {
        const head = out.slice(0, 150);
        const cut = Math.max(head.lastIndexOf(', '), head.lastIndexOf('; '), head.lastIndexOf('— '));
        out = cut > 60 ? head.slice(0, cut) : head.replace(/\s+\S*$/, '');
      }
      out = out.replace(/[\s—,;:]+$/, '');
      if (!/[.!?]$/.test(out)) out += '.';
      return out.charAt(0).toLowerCase() + out.slice(1);
    },

    // ── The greed meter ───────────────────────────────────────────
    // encCanBank says they may walk out with encLoot right now. The doors say
    // what walking deeper costs. Both numbers, together, on screen.
    greed(json, nodeKey, encLoot, encCanBank) {
      const n = this.node(json, nodeKey);
      const doors = [];
      const choices = (n && Array.isArray(n.choices)) ? n.choices : [];
      for (const c of choices) {
        const risk = Math.max(0, Math.min(100, c.base_risk == null ? 50 : c.base_risk | 0));
        const sk = Math.max(0, Math.min(SK_SHORT.length - 1, c.skill | 0));
        const haz = (c.hazard_id && json.hazards) ? json.hazards[c.hazard_id] : null;
        doors.push({
          label: this.despell(c.label) || 'go on',
          risk, skill: SK_SHORT[sk],
          hazardId: c.hazard_id || '',
          ends: !!(haz && haz.ends_encounter),
          to: c.success_node || '',
        });
      }
      return {
        canBank: !!encCanBank,
        // choices: [] means terminal — they are through, and only banking is
        // left. That is a beat, not an absence.
        terminal: !!n && choices.length === 0,
        haul: this.haulList(encLoot),
        doors,
        topRisk: doors.reduce((a, d) => Math.max(a, d.risk), 0),
      };
    },

    // encLoot -> [{res, name, qty}], biggest first. Empty when the bag is.
    haulList(encLoot) {
      const out = [];
      const l = Array.isArray(encLoot) ? encLoot : [];
      for (let i = 0; i < RES_SHORT.length; i++) {
        if ((l[i] | 0) > 0) out.push({ res: i, name: RES_SHORT[i], qty: l[i] | 0 });
      }
      return out.sort((a, b) => b.qty - a.qty);
    },

    haulText(encLoot) {
      const h = this.haulList(encLoot);
      if (!h.length) return 'nothing';
      return h.map((x) => x.name + ' x' + x.qty).join(', ');
    },

    // ── Hazard signature matching ─────────────────────────────────
    // Polling cannot see the roll, only its aftermath. What it CAN see is the
    // exact damage, and every hazard reachable from the node they are
    // standing on has a written signature: penalty.ll, wound[], radiation,
    // resource costs. Match the two and the observer can read the hazard's
    // own prose instead of a template.
    //
    // A hazard with an empty penalty (the pharmacy's cage_trap: no damage,
    // ends_encounter true) is unmatchable HERE by construction — nothing
    // moved. It is identified at THE EXIT instead, by the loot going missing.
    matchHazard(json, nodeKey, delta) {
      const n = this.node(json, nodeKey);
      if (!json || !n || !Array.isArray(n.choices)) return null;
      let best = null, bestScore = 0;
      for (const c of n.choices) {
        const id = c.hazard_id;
        const haz = id && json.hazards ? json.hazards[id] : null;
        if (!haz) continue;
        const sc = this.signatureScore(haz, delta);
        if (sc > bestScore) { bestScore = sc; best = { id, haz }; }
      }
      if (!best) return null;
      return {
        id: best.id,
        hazard: best.haz,
        text: this.despell(best.haz.text),
        ends: !!best.haz.ends_encounter,
        confidence: bestScore,
      };
    },

    signatureScore(haz, delta) {
      const pen = haz.penalty || {};
      const wnd = Array.isArray(haz.wound) ? haz.wound : [0, 0];
      const hasSignature = (pen.ll | 0) || (pen.radiation | 0) || (wnd[0] | 0) || (wnd[1] | 0)
        || PEN_RES_KEYS.some((k) => (pen[k] | 0));
      if (!hasSignature) return 0;

      let score = 0;
      // penalty.ll is written negative in the files; the observed drop is
      // positive. Exact is worth a lot, adjacent very little — another source
      // (exposure, a dawn tick) can land in the same poll.
      const wantLL = -(pen.ll | 0);
      if (wantLL > 0) {
        if (delta.ll === wantLL) score += 4;
        else if (delta.ll > 0 && Math.abs(delta.ll - wantLL) <= 1) score += 1;
        else if (delta.ll <= 0) score -= 2;
      }
      const wantRad = pen.radiation | 0;
      if (wantRad > 0) {
        if (delta.rad === wantRad) score += 2;
        else if (delta.rad > 0) score += 1;
        else score -= 1;
      }
      for (let w = 0; w < 2; w++) {
        const want = wnd[w] | 0;
        if (!want) continue;
        if (delta.wounds[w] === want) score += 3;
        else if (delta.wounds[w] > 0) score += 1;
        else score -= 2;
      }
      for (let i = 0; i < PEN_RES_KEYS.length; i++) {
        const want = -(pen[PEN_RES_KEYS[i]] | 0);
        if (want <= 0) continue;
        if (delta.res[i] === want) score += 2;
        else if (delta.res[i] > 0) score += 1;
      }
      return score;
    },

    // The hazard on this node that throws you out, if there is one. Used to
    // read an exit where the loot vanished and nothing else moved.
    endingHazard(json, nodeKey) {
      const n = this.node(json, nodeKey);
      if (!json || !n || !Array.isArray(n.choices)) return null;
      for (const c of n.choices) {
        const haz = c.hazard_id && json.hazards ? json.hazards[c.hazard_id] : null;
        if (haz && haz.ends_encounter) {
          return { id: c.hazard_id, hazard: haz, text: this.despell(haz.text), ends: true };
        }
      }
      return null;
    },

    RES_SHORT, SK_SHORT,
  };

  OBS.Scene = Scene;
}(typeof globalThis !== 'undefined' ? globalThis : this));
