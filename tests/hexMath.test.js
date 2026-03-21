/**
 * Hex Math Tests (Tier 3.3)
 * Tests for coordinate transforms and distance calculations.
 * These are pure functions extracted from engine.js and should be testable.
 */

describe('Hex Math - Coordinate Transforms', () => {
  /**
   * Axial to pixel conversion for flat-top hexagons.
   * Can be extracted from engine.js lines 695-698.
   */
  function hexToPixel(q, r, size) {
    const SQRT3 = Math.sqrt(3);
    return {
      x: size * 1.5 * q,
      y: size * (SQRT3 / 2 * q + SQRT3 * r),
    };
  }

  /**
   * Pixel to axial conversion (inverse).
   */
  function pixelToHex(x, y, size) {
    const SQRT3 = Math.sqrt(3);
    const q = (2 / 3 * x) / size;
    const r = (-1 / 3 * x + Math.sqrt(3) / 3 * y) / size;
    return { q: Math.round(q), r: Math.round(r) };
  }

  test('should convert axial to pixel coordinates', () => {
    const pixel = hexToPixel(0, 0, 32);
    expect(pixel.x).toBe(0);
    expect(pixel.y).toBe(0);
  });

  test('should handle hex size scaling', () => {
    const small = hexToPixel(1, 0, 32);
    const large = hexToPixel(1, 0, 64);
    expect(large.x).toBe(small.x * 2);
    expect(large.y).toBe(small.y * 2);
  });

  test('should handle negative coordinates', () => {
    const pixel = hexToPixel(-5, -3, 32);
    expect(pixel.x).toBeLessThan(0);
    expect(pixel.y).toBeLessThan(0);
  });
});

describe('Hex Math - Distance', () => {
  /**
   * Cube distance for axial coordinates.
   * Can be extracted from engine.js lines 699-702.
   */
  function hexDist(q1, r1, q2, r2) {
    const dq = q2 - q1;
    const dr = r2 - r1;
    return (Math.abs(dq) + Math.abs(dq + dr) + Math.abs(dr)) / 2;
  }

  /**
   * Distance accounting for toroidal wrapping.
   * Can be extracted from engine.js lines 703-709.
   */
  function hexDistWrap(q1, r1, q2, r2, mapCols = 25, mapRows = 19) {
    let min = Infinity;
    for (let dq = -1; dq <= 1; dq++) {
      for (let dr = -1; dr <= 1; dr++) {
        const dist = hexDist(q1, r1, q2 + dq * mapCols, r2 + dr * mapRows);
        min = Math.min(min, dist);
      }
    }
    return min;
  }

  test('should calculate distance between adjacent hexes', () => {
    expect(hexDist(0, 0, 1, 0)).toBe(1);
    expect(hexDist(0, 0, 0, 1)).toBe(1);
    expect(hexDist(0, 0, -1, 1)).toBe(1);
  });

  test('should calculate distance for diagonal moves', () => {
    expect(hexDist(0, 0, 1, 1)).toBe(1);
    expect(hexDist(0, 0, 2, 2)).toBe(2);
  });

  test('should be symmetric', () => {
    expect(hexDist(0, 0, 5, 3)).toBe(hexDist(5, 3, 0, 0));
  });

  test('should handle wrapping distance on edges', () => {
    const mapCols = 25;
    const mapRows = 19;

    // Distance from right edge to left edge should be short
    const rightEdge = hexDistWrap(24, 0, 0, 0, mapCols, mapRows);
    expect(rightEdge).toBeLessThan(5);

    // Direct distance would be long
    const direct = hexDist(24, 0, 0, 0);
    expect(direct).toBeGreaterThan(rightEdge);
  });

  test('should return 0 for same position', () => {
    expect(hexDist(5, 5, 5, 5)).toBe(0);
    expect(hexDistWrap(5, 5, 5, 5)).toBe(0);
  });
});

describe('Hex Math - Vision Range', () => {
  function hexDist(q1, r1, q2, r2) {
    const dq = q2 - q1;
    const dr = r2 - r1;
    return (Math.abs(dq) + Math.abs(dq + dr) + Math.abs(dr)) / 2;
  }

  function getCellsInVisionRange(q, r, visionRadius, mapCols = 25, mapRows = 19) {
    const visible = [];
    for (let r2 = 0; r2 < mapRows; r2++) {
      for (let q2 = 0; q2 < mapCols; q2++) {
        if (hexDist(q, r, q2, r2) <= visionRadius) {
          visible.push({ q: q2, r: r2 });
        }
      }
    }
    return visible;
  }

  test('should find all cells in vision range', () => {
    const visible = getCellsInVisionRange(12, 9, 1);
    // Vision radius 1 should include roughly 7 hexes (center + 6 neighbors)
    expect(visible.length).toBeGreaterThan(0);
    expect(visible.some(c => c.q === 12 && c.r === 9)).toBe(true); // Center included
  });

  test('should respect vision radius boundary', () => {
    const visible = getCellsInVisionRange(12, 9, 2, 25, 19);
    const distances = visible.map(c => {
      const dq = c.q - 12;
      const dr = c.r - 9;
      return (Math.abs(dq) + Math.abs(dq + dr) + Math.abs(dr)) / 2;
    });

    // All distances should be <= vision radius
    expect(distances.every(d => d <= 2)).toBe(true);
    // At least one should be at the boundary
    expect(distances.some(d => d === 2)).toBe(true);
  });
});
