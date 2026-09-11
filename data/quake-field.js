// ── Quake Field ────────────────────────────────────────────────────────────
// Earthquakes: a straight line of 7-10 adjacent hexes ruptures, shakes hard
// for a couple seconds, kicks up dust, then settles. The server decides
// when/where a quake happens and which shelters it destroys (so every
// client sees the same fault line and the same losses) — this class only
// renders whatever fault line network.js hands it via spawnFromEvent().
// Same idea as the storm field — a localized effect over specific hexes
// rather than the whole screen. Plain global class — no ES module syntax.

const QUAKE_SHAKE_MS = 2000; // main violent-shake window
const QUAKE_FADE_MS  = 600;  // tail-off after the shake window
const QUAKE_RAMP_MS  = 200;  // ease-in so the shake doesn't snap to full strength in one frame

class QuakeField {
  constructor() {
    this.quakes = [];
    this._nextId = 0;
    this._cellIndex = new Map(); // "mapQ_mapR" -> { id, order, env }
  }

  // cells = [{q, r}, ...] in fault-line order, converted = [{q, r}, ...]
  // subset that had a Settlement leveled to Open Scrub — both as sent by the
  // server's 'quake' event, rendered exactly where the server put them.
  spawnFromEvent(cells, converted, now = Date.now()) {
    if (!cells?.length) return;
    const convertedSet = new Set((converted ?? []).map(c => `${c.q}_${c.r}`));
    this.quakes.push({ id: this._nextId++, cells, convertedSet, bornAt: now, env: 0 });
  }

  _envelope(quake, now) {
    const age = now - quake.bornAt;
    if (age < QUAKE_RAMP_MS) return age / QUAKE_RAMP_MS;  // ease in — no instant full-strength jolt
    if (age < QUAKE_SHAKE_MS) return 1;
    return Math.max(0, 1 - (age - QUAKE_SHAKE_MS) / QUAKE_FADE_MS);
  }

  // Drops finished quakes and rebuilds the per-frame cell index. Call once a
  // frame, before the terrain pass reads intensityAt()/cellInfo().
  update(now) {
    this.quakes = this.quakes.filter(q => now - q.bornAt < QUAKE_SHAKE_MS + QUAKE_FADE_MS);
    this._cellIndex.clear();
    for (const quake of this.quakes) {
      quake.env = this._envelope(quake, now);
      quake.cells.forEach((c, order) => {
        const key = `${c.q}_${c.r}`;
        this._cellIndex.set(key, { id: quake.id, order, env: quake.env, converted: quake.convertedSet.has(key) });
      });
    }
  }

  // 0..1 shake intensity for one hex this frame (0 if not on an active fault line).
  intensityAt(mapQ, mapR) {
    return this._cellIndex.get(`${mapQ}_${mapR}`)?.env || 0;
  }

  cellInfo(mapQ, mapR) {
    return this._cellIndex.get(`${mapQ}_${mapR}`) || null;
  }

  // Strongest current envelope across all active quakes — drives a whole-
  // camera shake so a tremor is felt a little even off the exact fault line.
  peakEnvelope() {
    let best = 0;
    for (const quake of this.quakes) best = Math.max(best, quake.env);
    return best;
  }
}
