// ── Fire Field ─────────────────────────────────────────────────────────────
// Unlike the quake field's one-shot pulse, fire is persistent, server-
// authoritative hex state: worldState.fire is a sparse [[q,r,intensity],...]
// list refreshed on every world/state sync. This class just indexes the
// latest list for the terrain pass to query — no client-side animation/decay
// of its own. A hex silently drops out of the list once it burns out
// server-side; the accompanying `fire_spread` event (intensity 0) is handled
// in network.js to flip that hex's local terrain to Ash Dunes at the same
// moment (see world-system.hpp's onFireExtinguished()).

class FireField {
  constructor() {
    this._cellIndex = new Map(); // "q_r" -> intensity (1-3)
  }

  // Replaces the whole set from the latest world.fire sync. Called from
  // network.js's _applyWorldState(), not per render frame — the data only
  // changes when a sync arrives, so there's nothing to recompute between.
  setFromSync(fireList) {
    this._cellIndex.clear();
    for (const entry of fireList ?? []) {
      const [q, r, intensity] = entry;
      this._cellIndex.set(`${q}_${r}`, intensity);
    }
  }

  // 0 if not burning, else 1-3.
  intensityAt(mapQ, mapR) {
    return this._cellIndex.get(`${mapQ}_${mapR}`) || 0;
  }
}
