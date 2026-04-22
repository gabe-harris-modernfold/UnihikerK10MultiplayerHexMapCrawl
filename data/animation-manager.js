/**
 * Animation State Manager Module (Tier 2.5)
 * Centralizes all animation-related state and timing logic.
 * Makes animations testable, pauseable, and independent of RAF timing.
 */

/**
 * Manages all animation state for the game client.
 * Encapsulates nightFade, displayMP, footprint animations, and related logic.
 */
class AnimationManager {
  nightFade = 0;
  mpTarget = 6;
  isResting = false;
  isPaused = false;
  footprintTimestamps = new Map(); // key: 'q_r_pid' → Date.now()

  constructor(config = {}) {
    // Time-of-day overlay fade (post-dawn rest effect)
    this.nightFadeDecayRate = config.nightFadeDecayRate || 0.004;
    this.nightFadeInit = config.nightFadeInit || 0.72;

    // MP display smoothing
    this.displayMP = config.initialMP || 6;
    this.restingLerpRate = config.restingLerpRate || 0.003;
    this.movingLerpRate = config.movingLerpRate || 0.08;

    this.footprintFadeMs = config.footprintFadeMs || 4000;
  }

  /**
   * Update all animation states based on elapsed time.
   * Call once per frame from render loop.
   * @param {Object} state - Current game state { resting, mpValue }
   */
  update(state = {}) {
    if (this.isPaused) return;

    // Update resting state
    this.isResting = state.resting || false;

    // Fade nightFade overlay (post-dawn decay)
    this.nightFade = Math.max(0, this.nightFade - this.nightFadeDecayRate);

    // Smooth MP display toward target
    this.mpTarget = this.isResting ? 0 : (state.mpValue || this.mpTarget);
    const lerpRate = this.isResting ? this.restingLerpRate : this.movingLerpRate;
    this.displayMP += (this.mpTarget - this.displayMP) * lerpRate;
  }

  /**
   * Trigger REST action — initialize night fade effect.
   */
  startRest() {
    this.nightFade = this.nightFadeInit;
  }

  /**
   * Record a footprint animation on a hex cell.
   * @param {number} q - Hex column
   * @param {number} r - Hex row
   * @param {number} playerId - ID of player creating footprint
   */
  recordFootprint(q, r, playerId) {
    this.footprintTimestamps.set(`${q}_${r}_${playerId}`, Date.now());
  }

  /**
   * Get footprint fade factor (0 = fresh, 1 = fully faded).
   * @param {number} q - Hex column
   * @param {number} r - Hex row
   * @param {number} playerId - Player ID
   * @returns {number} Fade factor (0-1)
   */
  getFootprintFade(q, r, playerId) {
    const key = `${q}_${r}_${playerId}`;
    const timestamp = this.footprintTimestamps.get(key);
    if (!timestamp) return 1; // fully faded if not found

    const age = Date.now() - timestamp;
    return Math.max(0, Math.min(1, age / this.footprintFadeMs));
  }

  /**
   * Clear all footprints (call when player moves).
   */
  clearFootprints() {
    this.footprintTimestamps.clear();
  }

  /**
   * Pause all animations (for debugging or menu).
   */
  pause() {
    this.isPaused = true;
  }

  /**
   * Resume animations.
   */
  resume() {
    this.isPaused = false;
  }

  /**
   * Reset all animation state to defaults.
   */
  reset(initialMP = 6) {
    this.nightFade = 0;
    this.displayMP = initialMP;
    this.mpTarget = initialMP;
    this.isResting = false;
    this.footprintTimestamps.clear();
    this.isPaused = false;
  }

  /**
   * Serialize animation state for debugging or replay.
   * @returns {Object} Serializable state snapshot
   */
  serialize() {
    return {
      nightFade: this.nightFade,
      displayMP: this.displayMP,
      mpTarget: this.mpTarget,
      isResting: this.isResting,
      footprints: Array.from(this.footprintTimestamps.entries()),
    };
  }

  /**
   * Restore animation state from serialized snapshot.
   * @param {Object} snapshot - Serialized state from serialize()
   */
  restore(snapshot) {
    if (!snapshot) return;
    this.nightFade = snapshot.nightFade || 0;
    this.displayMP = snapshot.displayMP || 6;
    this.mpTarget = snapshot.mpTarget || 6;
    this.isResting = snapshot.isResting || false;
    this.footprintTimestamps = new Map(snapshot.footprints || []);
  }
}

/**
 * Global singleton instance (if needed).
 * Alternatively, pass instance to engine as dependency.
 */
const animationManager = new AnimationManager();
