/**
 * Game State Manager
 * Implements event sourcing pattern with full state history and replay capability.
 * Enables time-travel debugging, state snapshots, and perfect replay.
 */

/**
 * Reducer function that applies an event to the current state.
 * Pure function: (state, event) → new state
 * @param {Object} state - Current game state
 * @param {Object} event - Event to apply
 * @returns {Object} New state after applying event
 */
function gameStateReducer(state, event) {
  switch (event.type) {
    case 'SYNC':
      return {
        ...state,
        map: event.map || state.map,
        players: event.players || state.players,
        shared: { ...state.shared, ...event.shared },
      };

    case 'PLAYER_POSITION_CHANGED':
      return {
        ...state,
        players: state.players.map(p =>
          p.id === event.playerId
            ? { ...p, q: event.q, r: event.r }
            : p
        ),
      };

    case 'PLAYER_STATS_UPDATED':
      return {
        ...state,
        players: state.players.map(p =>
          p.id === event.playerId
            ? { ...p, ...event.updates }
            : p
        ),
      };

    case 'MAP_CELL_UPDATED':
      return {
        ...state,
        map: state.map.map((row, r) =>
          r === event.r
            ? row.map((cell, q) =>
                q === event.q ? { ...cell, ...event.updates } : cell
              )
            : row
        ),
      };

    case 'PLAYER_JOINED':
      return {
        ...state,
        players: state.players.map(p =>
          p.id === event.playerId ? { ...p, on: true } : p
        ),
      };

    case 'PLAYER_LEFT':
      return {
        ...state,
        players: state.players.map(p =>
          p.id === event.playerId ? { ...p, on: false } : p
        ),
      };

    case 'GAME_STATE_UPDATED':
      return {
        ...state,
        shared: { ...state.shared, ...event.updates },
      };

    case 'ENC_START':
      return {
        ...state,
        map: state.map.map((row, r) =>
          r === event.r
            ? row.map((cell, q) => q === event.q ? { ...cell, poi: 0 } : cell)
            : row
        ),
        encounters: {
          ...state.encounters,
          [event.pid]: { active: true, hexQ: event.q, hexR: event.r, pendingLoot: [0,0,0,0,0] },
        },
      };

    case 'ENC_ASSIST':
      return {
        ...state,
        players: state.players.map(p =>
          p.id === event.pid
            ? { ...p, inv: p.inv.map((v, i) => i === event.res ? Math.max(0, v - 1) : v) }
            : p
        ),
      };

    case 'ENC_RESULT': {
      const enc = state.encounters?.[event.pid] ?? {};
      const newLoot = (enc.pendingLoot ?? [0,0,0,0,0]).map((v, i) => v + (event.loot?.[i] ?? 0));
      return {
        ...state,
        players: state.players.map(p =>
          p.id === event.pid
            ? {
                ...p,
                ll:  Math.max(0, (p.ll  ?? 0) + (event.penLL  ?? 0)),
                fat: Math.max(0, (p.fat ?? 0) + (event.penFat ?? 0)),
                rad: Math.max(0, (p.rad ?? 0) + (event.penRad ?? 0)),
                ...(event.st ? { st: event.st } : {}),
              }
            : p
        ),
        encounters: event.ends
          ? { ...state.encounters, [event.pid]: undefined }
          : { ...state.encounters, [event.pid]: { ...enc, pendingLoot: newLoot } },
      };
    }

    case 'ENC_BANK':
      return {
        ...state,
        encounters: { ...state.encounters, [event.pid]: undefined },
      };

    case 'ENC_END':
      return {
        ...state,
        encounters: { ...state.encounters, [event.pid]: undefined },
      };

    default:
      return state;
  }
}

/**
 * Creates initial game state.
 * @param {number} maxPlayers - Maximum number of players
 * @param {number} mapRows - Map row count
 * @param {number} mapCols - Map column count
 * @returns {Object} Initial state
 */
function createInitialState(maxPlayers = 6, mapRows = 19, mapCols = 25) {
  return {
    myId: -1,
    players: Array.from({ length: maxPlayers }, (_, i) => ({
      id: i,
      on: false,
      q: 0,
      r: 0,
      mp: 6,
      ll: 7,
      food: 6,
      water: 6,
      rad: 0,
      inv: [0, 0, 0, 0, 0],
      nm: `Survivor${i}`,
    })),
    map: Array.from({ length: mapRows }, () => Array(mapCols).fill(null)),
    shared: {
      tc: 0,  // Threat clock
      dc: 0,  // Day counter
      sf: 30, // Shared food
      sw: 30, // Shared water
    },
  };
}

/**
 * GameState class using event sourcing pattern.
 * Maintains complete event history and allows state reconstruction from any point.
 */
class GameState {
  constructor(initialState = null) {
    this.events = [];
    this.state = initialState || createInitialState();
    this.listeners = new Set(); // Observer pattern
  }

  /**
   * Apply an event to the state and record it in history.
   * Notifies all listeners of state change.
   * @param {Object} event - Event to apply { type, ... }
   */
  applyEvent(event) {
    if (!event.type) {
      console.warn('Event missing type field', event);
      return;
    }

    this.events.push({ ...event, timestamp: Date.now() });
    this.state = gameStateReducer(this.state, event);
    this.notifyListeners();
  }

  /**
   * Get current state snapshot.
   * @returns {Object} Deep copy of current state
   */
  getSnapshot() {
    return structuredClone(this.state);
  }

  /**
   * Get state at a specific point in history.
   * @param {number} eventIndex - Index in event history (0 = initial state)
   * @returns {Object} State after applying first N events
   */
  getStateAt(eventIndex) {
    if (eventIndex < 0) return this.getSnapshot();

    let s = createInitialState();
    for (let i = 0; i <= Math.min(eventIndex, this.events.length - 1); i++) {
      s = gameStateReducer(s, this.events[i]);
    }
    return s;
  }

  /**
   * Replay game history: discard current state and replay all events from the beginning.
   * Useful for validating consistency or resetting after errors.
   */
  replay() {
    this.state = createInitialState();
    for (const event of this.events) {
      this.state = gameStateReducer(this.state, event);
    }
    this.notifyListeners();
  }

  /**
   * Subscribe to state changes.
   * @param {Function} listener - Callback(newState, event) when state changes
   * @returns {Function} Unsubscribe function
   */
  subscribe(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  /**
   * Notify all listeners of state change.
   * @private
   */
  notifyListeners() {
    const lastEvent = this.events[this.events.length - 1];
    for (const listener of this.listeners) {
      listener(this.state, lastEvent);
    }
  }

  /**
   * Get event history.
   * @param {number} limit - Max events to return (default all)
   * @returns {Array} Events in chronological order
   */
  getHistory(limit = null) {
    if (limit === null) return [...this.events];
    return this.events.slice(-limit);
  }

  /**
   * Serialize state and events for export/debugging.
   * @returns {Object} Serialized game state
   */
  serialize() {
    return {
      initialState: createInitialState(),
      events: this.events,
      currentState: this.state,
      metadata: {
        eventCount: this.events.length,
        timestamp: Date.now(),
      },
    };
  }

  /**
   * Restore from serialized state.
   * @param {Object} data - Serialized data from serialize()
   */
  static deserialize(data) {
    const gs = new GameState(createInitialState());
    if (data.events) {
      for (const event of data.events) {
        gs.applyEvent(event);
      }
    }
    return gs;
  }

  /**
   * Export game log for sharing/debugging.
   * @returns {string} JSON-formatted game events
   */
  exportLog() {
    return JSON.stringify(this.events, null, 2);
  }

  /**
   * Import game log.
   * @param {string} json - JSON-formatted events
   */
  importLog(json) {
    try {
      const imported = JSON.parse(json);
      if (Array.isArray(imported)) {
        this.events = imported;
        this.replay();
      }
    } catch (e) {
      console.error('Failed to import game log:', e);
    }
  }
}

/**
 * Example usage in engine.js:
 *
 * import { GameState } from './state-manager.js';
 *
 * const gameState = new GameState();
 *
 * // Subscribe to state changes
 * gameState.subscribe((newState, event) => {
 *   console.log('State changed:', event.type);
 *   updateUI(newState);
 * });
 *
 * // Apply events from server
 * socket.onmessage = (e) => {
 *   const msg = JSON.parse(e.data);
 *   if (msg.type === 'sync') {
 *     gameState.applyEvent({ type: 'SYNC', ...msg });
 *   } else if (msg.type === 'ev') {
 *     gameState.applyEvent({ type: msg.ev.type, ...msg.ev });
 *   }
 * };
 *
 * // Time-travel debugging
 * const stateAt100thEvent = gameState.getStateAt(99);
 *
 * // Export replay
 * const log = gameState.exportLog();
 */
