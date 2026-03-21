/**
 * Game State Management Tests (Tier 3.3)
 * Example test suite for GameState using Jest/Vitest.
 *
 * Run with: npm test
 * or: npx vitest
 */

import { GameState, gameStateReducer, createInitialState } from '../data/state-manager.js';

describe('GameState - State Management', () => {
  let gameState;

  beforeEach(() => {
    gameState = new GameState();
  });

  describe('Initial State', () => {
    test('should create initial state with correct structure', () => {
      const state = gameState.getSnapshot();
      expect(state.myId).toBe(-1);
      expect(state.players).toHaveLength(6);
      expect(state.map).toHaveLength(19);
      expect(state.map[0]).toHaveLength(25);
    });

    test('should have all players offline by default', () => {
      const state = gameState.getSnapshot();
      state.players.forEach(p => {
        expect(p.on).toBe(false);
        expect(p.id).toBeGreaterThanOrEqual(0);
        expect(p.id).toBeLessThan(6);
      });
    });
  });

  describe('Player Events', () => {
    test('should handle player position change', () => {
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });

      const state = gameState.getSnapshot();
      expect(state.players[0].q).toBe(5);
      expect(state.players[0].r).toBe(7);
    });

    test('should handle multiple player moves', () => {
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 1,
        q: 10,
        r: 3,
      });

      const state = gameState.getSnapshot();
      expect(state.players[0].q).toBe(5);
      expect(state.players[1].q).toBe(10);
    });

    test('should handle player join', () => {
      gameState.applyEvent({
        type: 'PLAYER_JOINED',
        playerId: 0,
      });

      const state = gameState.getSnapshot();
      expect(state.players[0].on).toBe(true);
    });

    test('should handle player leave', () => {
      gameState.applyEvent({
        type: 'PLAYER_JOINED',
        playerId: 0,
      });
      gameState.applyEvent({
        type: 'PLAYER_LEFT',
        playerId: 0,
      });

      const state = gameState.getSnapshot();
      expect(state.players[0].on).toBe(false);
    });

    test('should update player stats', () => {
      gameState.applyEvent({
        type: 'PLAYER_STATS_UPDATED',
        playerId: 0,
        updates: {
          mp: 3,
          ll: 5,
          rad: 2,
        },
      });

      const state = gameState.getSnapshot();
      expect(state.players[0].mp).toBe(3);
      expect(state.players[0].ll).toBe(5);
      expect(state.players[0].rad).toBe(2);
    });
  });

  describe('Map Events', () => {
    test('should update map cells', () => {
      gameState.applyEvent({
        type: 'MAP_CELL_UPDATED',
        q: 5,
        r: 7,
        updates: {
          terrain: 2,
          resource: 1,
          amount: 3,
        },
      });

      const state = gameState.getSnapshot();
      const cell = state.map[7][5];
      expect(cell.terrain).toBe(2);
      expect(cell.resource).toBe(1);
      expect(cell.amount).toBe(3);
    });
  });

  describe('State History & Replay', () => {
    test('should track event history', () => {
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 0 });
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 1 });
      gameState.applyEvent({ type: 'PLAYER_LEFT', playerId: 0 });

      const history = gameState.getHistory();
      expect(history).toHaveLength(3);
      expect(history[0].type).toBe('PLAYER_JOINED');
      expect(history[2].type).toBe('PLAYER_LEFT');
    });

    test('should get state at any point in history', () => {
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 10,
        r: 10,
      });

      const stateAfterFirst = gameState.getStateAt(0);
      expect(stateAfterFirst.players[0].q).toBe(5);

      const stateAfterSecond = gameState.getStateAt(1);
      expect(stateAfterSecond.players[0].q).toBe(10);
    });

    test('should replay events correctly', () => {
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 0 });
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });

      // Modify state manually
      gameState.state.players[0].on = false;

      // Replay should restore correct state
      gameState.replay();
      const state = gameState.getSnapshot();
      expect(state.players[0].on).toBe(true);
      expect(state.players[0].q).toBe(5);
    });
  });

  describe('Observers/Listeners', () => {
    test('should notify listeners on state change', () => {
      const listener = jest.fn();
      gameState.subscribe(listener);

      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });

      expect(listener).toHaveBeenCalledTimes(1);
      const [newState, event] = listener.mock.calls[0];
      expect(event.type).toBe('PLAYER_POSITION_CHANGED');
    });

    test('should allow unsubscribing', () => {
      const listener = jest.fn();
      const unsubscribe = gameState.subscribe(listener);

      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 0 });
      expect(listener).toHaveBeenCalledTimes(1);

      unsubscribe();
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 1 });
      expect(listener).toHaveBeenCalledTimes(1); // Still 1, not called again
    });
  });

  describe('Serialization', () => {
    test('should serialize state for export', () => {
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 0 });
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });

      const serialized = gameState.serialize();
      expect(serialized.events).toHaveLength(2);
      expect(serialized.currentState).toBeDefined();
      expect(serialized.metadata.eventCount).toBe(2);
    });

    test('should deserialize and replay', () => {
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 0 });
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });

      const serialized = gameState.serialize();
      const restored = GameState.deserialize(serialized);

      const state = restored.getSnapshot();
      expect(state.players[0].on).toBe(true);
      expect(state.players[0].q).toBe(5);
    });

    test('should export and import log', () => {
      gameState.applyEvent({ type: 'PLAYER_JOINED', playerId: 0 });
      gameState.applyEvent({
        type: 'PLAYER_POSITION_CHANGED',
        playerId: 0,
        q: 5,
        r: 7,
      });

      const log = gameState.exportLog();
      expect(typeof log).toBe('string');

      const newGameState = new GameState();
      newGameState.importLog(log);

      const state = newGameState.getSnapshot();
      expect(state.players[0].on).toBe(true);
      expect(state.players[0].q).toBe(5);
    });
  });
});

describe('gameStateReducer - Pure State Updates', () => {
  test('should return new state object on any change', () => {
    const initialState = createInitialState();
    const newState = gameStateReducer(initialState, {
      type: 'PLAYER_JOINED',
      playerId: 0,
    });

    expect(newState).not.toBe(initialState);
    expect(newState.players[0].on).toBe(true);
    expect(initialState.players[0].on).toBe(false); // Original unchanged
  });

  test('should handle unknown event types gracefully', () => {
    const initialState = createInitialState();
    const newState = gameStateReducer(initialState, {
      type: 'UNKNOWN_EVENT',
    });

    expect(newState).toBe(initialState); // Unchanged
  });
});
