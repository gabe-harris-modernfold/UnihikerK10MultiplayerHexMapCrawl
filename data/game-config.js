/**
 * Centralized Game Configuration Module
 * Consolidates all game balancing parameters, terrain definitions, and constants.
 * Separates configuration from code logic for easier tuning and server sync.
 */

// ────────────────────────────────────────────────────────────────
// MAP & WORLD CONFIGURATION
// ────────────────────────────────────────────────────────────────
const GAME_CONFIG = {
  map: {
    cols: 25,
    rows: 19,
    wrapping: 'toroidal',
    defaultFog: null,
  },

  players: {
    max: 6,
    defaultVisionRadius: 1,
  },

  // ────────────────────────────────────────────────────────────
  // ANIMATION & TIMING CONFIGURATION
  // ────────────────────────────────────────────────────────────
  animation: {
    lerp: 0.26,                    // position interpolation per frame (higher = snappier)
    moveLinearCooldown: 220,       // base movement cooldown in ms
    nightFadeInit: 0.72,           // post-dawn overlay initial alpha
    nightFadeDecayRate: 0.004,     // per-frame fade decay (~3s at 60fps)
    restingLerpRate: 0.003,        // MP display lerp while resting (slow)
    movingLerpRate: 0.08,          // MP display lerp while moving (fast)
    footprintFadeMs: 4000,         // time to fully fade footprint from bright to dark
  },

  // ────────────────────────────────────────────────────────────
  // RENDERING CONFIGURATION
  // ────────────────────────────────────────────────────────────
  rendering: {
    hexSize: {
      min: 36,
      max: 64,
      viewportDivisor: 11,        // canvas width ÷ this = hex size in pixels
    },
    icon: {
      sizeScale: 0.44,            // icon size as fraction of hex
      shadowHorizontal: 0.36,     // shadow ellipse horizontal scale
      shadowVertical: 0.09,       // shadow ellipse vertical scale
      arrowScale: 0.62,           // arrow (▼) size scale
      headRadiusScale: 1.05,      // font size multiplier for head label
    },
    fog: {
      innerAlpha: 0.78,           // inner fog ring opacity (more visible)
      outerAlpha: 0.92,           // outer fog ring opacity
      shadowAlpha: 0.72,          // character name label background opacity
      groundShadowAlpha: 0.4,     // ground shadow under characters
    },
  },

  // ────────────────────────────────────────────────────────────
  // NETWORK CONFIGURATION
  // ────────────────────────────────────────────────────────────
  network: {
    wifiCredsDelay: 300,          // ms before auto-sending WiFi credentials
    reconnectDelay: 2000,         // ms before attempting reconnect on disconnect
  },

  // ────────────────────────────────────────────────────────────
  // RESOURCE DEFINITIONS
  // (From game-data.js — can be synced from server)
  // ────────────────────────────────────────────────────────────
  resources: [
    { id: 1, name: 'Water', label: '≈', color: '#2A5C8A', icon: '💧' },
    { id: 2, name: 'Food', label: '◉', color: '#D08050', icon: '🍎' },
    { id: 3, name: 'Fuel', label: '✱', color: '#FFB800', icon: '⛽' },
    { id: 4, name: 'Medical', label: '◆', color: '#7DD3C0', icon: '⚕️' },
    { id: 5, name: 'Scrap', label: '●', color: '#A8A8A8', icon: '🔩' },
  ],
};

/**
 * Apply configuration from server (for synced values like terrain variants).
 * Merges server config with local defaults.
 * @param {Object} serverConfig - Configuration object from server
 */
function applyServerConfig(serverConfig) {
  if (!serverConfig) return;
  // Deep merge server values into GAME_CONFIG
  Object.keys(serverConfig).forEach(key => {
    if (typeof serverConfig[key] === 'object') {
      Object.assign(GAME_CONFIG[key] || {}, serverConfig[key]);
    } else {
      GAME_CONFIG[key] = serverConfig[key];
    }
  });
}

// ────────────────────────────────────────────────────────────────
// CONVENIENCE GETTERS (for easy access in render loops)
// ────────────────────────────────────────────────────────────────

const getConfig = (path) => {
  // Dot-notation accessor: getConfig('rendering.hexSize.min') → 36
  return path.split('.').reduce((obj, key) => obj?.[key], GAME_CONFIG);
};
