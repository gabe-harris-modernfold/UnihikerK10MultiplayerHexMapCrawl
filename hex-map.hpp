#pragma once
// ── hex-map.hpp ─────────────────────────────────────────────────────────────
// Hex math helpers, slot management, map generation, and vision encoding.
// Included by Esp32HexMapCrawl.ino before gameplay .hpp files.

// ── Hex math helpers ───────────────────────────────────────────
static inline int wrapQ(int q) { return ((q % MAP_COLS) + MAP_COLS) % MAP_COLS; }
static inline int wrapR(int r) { return ((r % MAP_ROWS) + MAP_ROWS) % MAP_ROWS; }

static int hexDistWrap(int q1, int r1, int q2, int r2) {
  int best = 0x7FFFFFFF;
  for (int dq = -1; dq <= 1; dq++) {
    for (int dr = -1; dr <= 1; dr++) {
      int aq   = q2 + dq * MAP_COLS - q1;
      int ar   = r2 + dr * MAP_ROWS - r1;
      int dist = (abs(aq) + abs(aq + ar) + abs(ar)) / 2;
      if (dist < best) best = dist;
    }
  }
  return best;
}

// ── Effective vision parameters for a player ──────────────────
// Call while holding G.mutex (reads map terrain at player position).
static void playerVisParams(int pid, int* outVisR, bool* outMaskRes) {
  uint8_t t = G.map[G.players[pid].r][G.players[pid].q].terrain;
  if (t >= NUM_TERRAIN) t = 0;
  int8_t vl = TERRAIN_VIS[t];
  if      (vl <= -3) { *outVisR = 0;            *outMaskRes = false; }
  else if (vl == -2) { *outVisR = 1;            *outMaskRes = true;  }
  else if (vl == -1) { *outVisR = 2;            *outMaskRes = false; }
  else if (vl ==  0) { *outVisR = VISION_R;     *outMaskRes = false; }
  else if (vl ==  1) { *outVisR = VISION_R + 1; *outMaskRes = false; }
  else               { *outVisR = VISION_R + 2; *outMaskRes = false; }
  if (G.players[pid].archetype == 4) *outVisR += 2;  // Scout: +2 vision radius
  // ── Weather visibility penalty (applied after Scout bonus) ──────────────────
  *outVisR = max(0, *outVisR - (int)WEATHER_VIS_PENALTY[G.weatherPhase]);
}

// ── Slot management ────────────────────────────────────────────
static int findSlot(uint32_t id) {
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (G.players[i].connected && G.players[i].wsClientId == id) return i;
  return -1;
}
static void enqEvt(GameEvent ev) {
  taskENTER_CRITICAL(&evtMux);
  if (pendingCount < EVT_QUEUE_SIZE) pendingEvents[pendingCount++] = ev;
  taskEXIT_CRITICAL(&evtMux);
}

// ── Terrain resource spawn helper ──────────────────────────────
static uint8_t terrainSpawnRes(uint8_t t, uint32_t rnd) {
  switch (t) {
    case 0: return 1 + rnd % 5;
    case 1: return (rnd & 1) ? 3 : 5;
    case 2: return (rnd & 1) ? 2 : 5;
    case 3: return (rnd & 1) ? 1 : 2;
    case 4: return (rnd & 1) ? 5 : 4;
    case 5: return 1;
    case 6: return 5;
    case 7: return (rnd & 1) ? 3 : 5;
    case 8: return (rnd & 1) ? 5 : 4;
    case 9: return 1 + rnd % 5;
    default: return 0;
  }
}

// ── Map generation data ────────────────────────────────────────
// Image variant counts (filled from SD scan before generateMap)
static uint8_t terrainVariantCount[NUM_TERRAIN] = {};  // 0 = no variants found
static uint8_t shelterVariantCount[2]           = {};  // [0]=basic, [1]=improved
static uint8_t forrageAnimalCount               = 0;   // forrageAnimal<N>.png

// Rank-quadratic weighted pick: variant 0 has weight (n)^2, variant n-1 has weight 1.
static uint8_t pickVariant(uint8_t n, uint32_t rnd) {
  if (n <= 1) return 0;
  uint32_t total = 0;
  for (uint8_t i = 0; i < n; i++) { uint32_t w = (uint32_t)(n - i) * (n - i); total += w; }
  uint32_t r = rnd % total;
  for (uint8_t i = 0; i < n; i++) {
    uint32_t w = (uint32_t)(n - i) * (n - i);
    if (r < w) return i;
    r -= w;
  }
  return 0;
}

static const uint8_t T_BASE[NUM_TERRAIN]  = { 66,  8, 10,  3,  2,  2,  0,  3,  2,  1,  3,  0 };

// Clump % per terrain
static const uint8_t TERRAIN_CLUMP[NUM_TERRAIN] = {
  15,  // 0 Open Scrub
  40,  // 1 Ash Dunes
  55,  // 2 Rust Forest
  45,  // 3 Marsh
  35,  // 4 Broken Urban
  50,  // 5 Flooded Ruins
  35,  // 6 Glass Fields
  40,  // 7 Rolling Hills
  55,  // 8 Mountain
  10,  // 9 Settlement
  55,  // 10 Nuke Crater
  95,  // 11 River Channel
};

static constexpr uint8_t SMOOTH_PASSES = 3;

static void generateMap() {
  // ── Build cumulative thresholds for Phase 1 ─────────────────
  uint8_t T_THRESH[NUM_TERRAIN];
  T_THRESH[0] = T_BASE[0];
  for (int t = 1; t < NUM_TERRAIN; t++)
    T_THRESH[t] = T_THRESH[t - 1] + T_BASE[t];

  // ── Phase 1: independent base fill ───────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      uint8_t  rv   = esp_random() % 100;
      uint8_t  t    = NUM_TERRAIN - 1;
      for (uint8_t i = 0; i < NUM_TERRAIN - 1; i++)
        if (rv < T_THRESH[i]) { t = i; break; }
      cell.terrain      = t;
      cell.resource     = 0;
      cell.amount       = 0;
      cell.respawnTimer = 0;
      cell.shelter      = 0;
      cell.footprints   = 0;
    }
  }

  // ── Phase 1.5: Forest Belt Pre-seeding ──────────────────────────
  {
    static const uint8_t AXIS_A[3] = { 0, 1, 2 };
    static const uint8_t AXIS_B[3] = { 3, 4, 5 };
    int numBelts = 4 + (int)(esp_random() % 3);
    for (int b = 0; b < numBelts; b++) {
      int bRow    = (int)(esp_random() % MAP_ROWS);
      int bCol    = (int)(esp_random() % MAP_COLS);
      int ax      = (int)(esp_random() % 3);
      int halfLen = 4 + (int)(esp_random() % 3);
      for (int side = 0; side < 2; side++) {
        int dir = (side == 0) ? AXIS_A[ax] : AXIS_B[ax];
        int cr = bRow, cc = bCol;
        for (int step = 0; step <= halfLen; step++) {
          uint8_t t = G.map[cr][cc].terrain;
          if (t != 8 && t != 10) G.map[cr][cc].terrain = 2;
          cr = wrapR(cr + DR[dir]);
          cc = wrapQ(cc + DQ[dir]);
        }
      }
    }
  }

  // ── Phase 2: clump smoothing passes ──────────────────────────
  static uint8_t scratch[MAP_ROWS][MAP_COLS];

  for (int pass = 0; pass < SMOOTH_PASSES; pass++) {
    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        uint8_t nCount[NUM_TERRAIN] = {0};
        for (int d = 0; d < 6; d++)
          nCount[G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain]++;

        uint32_t weights[NUM_TERRAIN];
        uint32_t total = 0;
        for (int t = 0; t < NUM_TERRAIN; t++) {
          weights[t] = (uint32_t)T_BASE[t] * (100u + (uint32_t)TERRAIN_CLUMP[t] * nCount[t]);
          total     += weights[t];
        }
        uint32_t pick = esp_random() % total;
        uint8_t  chosen = NUM_TERRAIN - 1;
        uint32_t cum    = 0;
        for (int t = 0; t < NUM_TERRAIN - 1; t++) {
          cum += weights[t];
          if (pick < cum) { chosen = (uint8_t)t; break; }
        }
        scratch[r][c] = chosen;
      }
    }
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.5: mountain-rolling hills transition pass ────────
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      scratch[r][c] = G.map[r][c].terrain;

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      if (G.map[r][c].terrain == 8) continue;
      uint8_t mNeigh = 0;
      for (int d = 0; d < 6; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 8) mNeigh++;
      if (mNeigh == 0) continue;
      uint32_t v    = (uint32_t)mNeigh * 40u;
      uint8_t  prob = (v >= 95u) ? 95u : (uint8_t)v;
      if ((esp_random() % 100) < prob)
        scratch[r][c] = 7;
    }
  }
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      G.map[r][c].terrain = scratch[r][c];

  // ── Phase 2.6: Glass Fields placement ────────────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      if (G.map[r][c].terrain == 6) G.map[r][c].terrain = 0;

  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      scratch[r][c] = G.map[r][c].terrain;

  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t t = G.map[r][c].terrain;
      if (t == 4 || t == 8 || t == 10) continue;

      bool nearCrater = false;
      for (int d = 0; d < 6 && !nearCrater; d++)
        if (G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain == 10) nearCrater = true;
      if (nearCrater) {
        if ((esp_random() % 100) < 80) scratch[r][c] = 6;
        continue;
      }

      bool nearQualBU = false;
      for (int d = 0; d < 6 && !nearQualBU; d++) {
        int nr = wrapR(r + DR[d]);
        int nc = wrapQ(c + DQ[d]);
        if (G.map[nr][nc].terrain != 4) continue;
        uint8_t buOfBU = 0;
        for (int d2 = 0; d2 < 6; d2++)
          if (G.map[wrapR(nr + DR[d2])][wrapQ(nc + DQ[d2])].terrain == 4) buOfBU++;
        if (buOfBU >= 1) nearQualBU = true;
      }
      if (!nearQualBU) continue;
      if ((esp_random() % 100) < 55) scratch[r][c] = 6;
    }
  }
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      G.map[r][c].terrain = scratch[r][c];

  // ── Phase 2.7: River Channel placement ───────────────────────────
  {
    const uint8_t T_RIVER = 11;
    const uint8_t DIR_W[6] = { 20, 0, 0, 0, 20, 60 };

    int numRivers = 1 + (int)(esp_random() % 2);
    for (int ri = 0; ri < numRivers; ri++) {
      int rq = (int)(esp_random() % MAP_COLS);
      int rr = 0;
      int steps = 0;
      const int MAX_STEPS = MAP_ROWS * 3;
      while (steps < MAX_STEPS) {
        uint8_t t = G.map[rr][rq].terrain;
        if (t != 8 && t != 10) {
          G.map[rr][rq].terrain = T_RIVER;
          if ((esp_random() % 100) < 35) {
            int wd = (int)(esp_random() % 6);
            int wr = wrapR(rr + DR[wd]);
            int wq2 = wrapQ(rq + DQ[wd]);
            uint8_t wt = G.map[wr][wq2].terrain;
            if (wt != 8 && wt != 10) G.map[wr][wq2].terrain = T_RIVER;
          }
        }
        uint32_t total = 0;
        for (int d = 0; d < 6; d++) total += DIR_W[d];
        uint32_t pick = esp_random() % total;
        uint32_t cum  = 0;
        int dir = 5;
        for (int d = 0; d < 6; d++) {
          cum += DIR_W[d];
          if (pick < cum) { dir = d; break; }
        }
        rq = wrapQ(rq + DQ[dir]);
        rr = wrapR(rr + DR[dir]);
        steps++;
        if (steps >= MAP_ROWS && (dir == 5 || dir == 0 || dir == 4)) break;
      }
    }
  }

  // ── Phase 2.8: Riverine Forest Fringe ───────────────────────────
  {
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        scratch[r][c] = G.map[r][c].terrain;

    for (int r = 0; r < MAP_ROWS; r++) {
      for (int c = 0; c < MAP_COLS; c++) {
        if (G.map[r][c].terrain != 11) continue;
        for (int d = 0; d < 6; d++) {
          int nr = wrapR(r + DR[d]);
          int nc = wrapQ(c + DQ[d]);
          uint8_t t = G.map[nr][nc].terrain;
          if (t == 8 || t == 9 || t == 10 || t == 11) continue;
          if ((esp_random() % 100) < 50) scratch[nr][nc] = 2;
        }
      }
    }
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++)
        G.map[r][c].terrain = scratch[r][c];
  }

  // ── Phase 2.9: Guaranteed minimums for Settlement and Nuke Crater ───────────
  {
    const uint8_t MIN_SETTLE = 3;
    const uint8_t MIN_CRATER = 2;

    uint8_t nSettle = 0, nCrater = 0;
    for (int r = 0; r < MAP_ROWS; r++)
      for (int c = 0; c < MAP_COLS; c++) {
        uint8_t t = G.map[r][c].terrain;
        if (t == 9)  nSettle++;
        if (t == 10) nCrater++;
      }

    for (uint8_t attempt = 0; nSettle < MIN_SETTLE && attempt < 200; attempt++) {
      int r = (int)(esp_random() % MAP_ROWS);
      int c = (int)(esp_random() % MAP_COLS);
      if (G.map[r][c].terrain != 0) continue;
      bool ok = true;
      for (int d = 0; d < 6 && ok; d++) {
        uint8_t nt = G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain;
        if (nt == 8 || nt == 9 || nt == 10 || nt == 11) ok = false;
      }
      if (!ok) continue;
      G.map[r][c].terrain = 9;
      nSettle++;
    }

    for (uint8_t attempt = 0; nCrater < MIN_CRATER && attempt < 200; attempt++) {
      int r = (int)(esp_random() % MAP_ROWS);
      int c = (int)(esp_random() % MAP_COLS);
      if (G.map[r][c].terrain != 0) continue;
      bool ok = true;
      for (int d = 0; d < 6 && ok; d++) {
        uint8_t nt = G.map[wrapR(r + DR[d])][wrapQ(c + DQ[d])].terrain;
        if (nt == 9 || nt == 10) ok = false;
      }
      if (!ok) continue;
      G.map[r][c].terrain = 10;
      nCrater++;
      for (int d = 0; d < 6; d++) {
        int nr = wrapR(r + DR[d]);
        int nc = wrapQ(c + DQ[d]);
        uint8_t nt = G.map[nr][nc].terrain;
        if (nt == 4 || nt == 8 || nt == 10) continue;
        if ((esp_random() % 100) < 80) G.map[nr][nc].terrain = 6;
      }
    }
  }

  // ── Phase 3: resource placement ───────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      uint8_t  t    = cell.terrain;
      if (t == 10 || t == 11) continue;

      uint32_t rnd = esp_random();
      uint8_t  r2  = (rnd >>  8) & 0xFF;
      uint8_t  r3  = (rnd >> 16) & 0xFF;
      uint8_t  r4  = (rnd >> 24) & 0xFF;
      if (r2 % 100 < 35) {
        cell.resource = terrainSpawnRes(t, r3);
        if (cell.resource > 0)
          cell.amount = 1 + r4 % 3;
      }
    }
  }

  // ── Phase 4: assign image variant per cell ───────────────────────
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c2 = 0; c2 < MAP_COLS; c2++) {
      HexCell& cell = G.map[r][c2];
      uint8_t  n    = terrainVariantCount[cell.terrain];
      cell.variant  = (n > 0) ? pickVariant(n, esp_random()) : 0;
    }

  // ── Phase 5: POI placement ────────────────────────────────────
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      G.map[r][c].poi = 0;
      uint8_t t = G.map[r][c].terrain;
      if (t >= NUM_TERRAIN || TERRAIN_POI_PCT[t] == 0) continue;
      if ((esp_random() % 100) < TERRAIN_POI_PCT[t]) {
        G.map[r][c].poi = 1;
      }
    }
  }

  // ── Post-generation map stats ────────────────────────────────
  uint16_t tCount[NUM_TERRAIN] = {0};
  uint16_t rCount[6]           = {0};
  uint16_t totalRes            = 0;

  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++) {
      HexCell& cell = G.map[r][c];
      tCount[cell.terrain]++;
      if (cell.resource > 0 && cell.resource < 6) { rCount[cell.resource]++; totalRes++; }
    }

  Serial.printf("[MAP] Generated %dx%d = %d cells | clump passes:%d\n",
    MAP_COLS, MAP_ROWS, MAP_COLS * MAP_ROWS, SMOOTH_PASSES);
  Serial.print ("[MAP] Terrain  : ");
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf("%s:%d ", T_SHORT[t], tCount[t]);
  Serial.println();
  Serial.print ("[MAP] Base wt% : ");
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf("%s:%-2d ", T_SHORT[t], T_BASE[t]);
  Serial.println();
  Serial.print ("[MAP] Clump %  : ");
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf("%s:%-2d ", T_SHORT[t], TERRAIN_CLUMP[t]);
  Serial.println();
  Serial.printf("[MAP] Resources: Water:%d Food:%d Fuel:%d Med:%d Scrap:%d | total:%d (%.1f%%)\n",
    rCount[1], rCount[2], rCount[3], rCount[4], rCount[5],
    totalRes, (float)totalRes * 100.0f / (MAP_COLS * MAP_ROWS));
}

// ── Map encode: fog masked ─────────────────────────────────────
static const char HEX_CH[] = "0123456789ABCDEF";

static int encodeMapFog(char* buf, int cap, int pq, int pr, int visR, bool maskRes) {
  int pos = 0;
  for (int r = 0; r < MAP_ROWS; r++) {
    for (int c = 0; c < MAP_COLS; c++) {
      uint8_t tt, dd, vv;
      if (hexDistWrap(pq, pr, c, r) <= visR) {
        HexCell& cell = G.map[r][c];
        tt = cell.terrain;
        dd = (cell.footprints & 0x3F) | ((cell.shelter ? 1 : 0) << 6);
        if (cell.poi) dd |= (1 << 7);
        vv = (cell.resource << 4) | (cell.variant & 0x0F);
      } else {
        tt = 0xFF; dd = 0x00; vv = 0x00;
      }
      if (pos + 6 < cap) {
        buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      }
    }
  }
  buf[pos] = 0;
  return pos;
}

// ── Build full vision-disk message ─────────────────────────────
static int buildVisDisk(char* buf, int cap, int pq, int pr, int visR, bool maskRes, int* outCells = nullptr) {
  int pos   = 0;
  int cells = 0;
  pos += snprintf(buf, cap, "{\"t\":\"vis\",\"vr\":%d,\"q\":%d,\"r\":%d,\"cells\":\"", visR, pq, pr);
  for (int dr = -visR; dr <= visR; dr++) {
    for (int dq = -visR; dq <= visR; dq++) {
      int s = -(dq + dr);
      if (abs(dq) + abs(dr) + abs(s) > 2 * visR) continue;
      int      cq   = wrapQ(pq + dq);
      int      cr   = wrapR(pr + dr);
      HexCell& cell = G.map[cr][cq];
      uint8_t  tt   = cell.terrain;
      uint8_t  dd   = (cell.footprints & 0x3F) | ((cell.shelter ? 1 : 0) << 6);
      if (cell.poi) dd |= (1 << 7);
      uint8_t  vv   = (maskRes ? 0 : (cell.resource << 4)) | (cell.variant & 0x0F);
      if (pos + 10 < cap) {
        buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
        buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
        buf[pos++] = HEX_CH[tt >> 4]; buf[pos++] = HEX_CH[tt & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
        cells++;
      }
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  if (outCells) *outCells = cells;
  return pos;
}

// ── Build survey-ring vision disk (one hex beyond visR) ────────
static int buildSurveyDisk(char* buf, int cap, int pq, int pr, int visR, int pid) {
  int pos = snprintf(buf, cap, "{\"t\":\"ev\",\"k\":\"surv\",\"pid\":%d,\"cells\":\"", pid);
  int ring = visR + 1;
  for (int dr = -ring; dr <= ring; dr++) {
    for (int dq = -ring; dq <= ring; dq++) {
      int s = -(dq + dr);
      if ((abs(dq) + abs(dr) + abs(s)) / 2 != ring) continue;
      int cq = wrapQ(pq + dq);
      int cr = wrapR(pr + dr);
      if (pos + 10 < cap) {
        HexCell& cell = G.map[cr][cq];
        uint8_t dd = (cell.footprints & 0x3F) | ((cell.shelter ? 1 : 0) << 6);
        if (cell.poi) dd |= (1 << 7);
        uint8_t vv = (cell.resource << 4) | (cell.variant & 0x0F);
        buf[pos++] = HEX_CH[cq >> 4]; buf[pos++] = HEX_CH[cq & 0xF];
        buf[pos++] = HEX_CH[cr >> 4]; buf[pos++] = HEX_CH[cr & 0xF];
        buf[pos++] = HEX_CH[cell.terrain >> 4]; buf[pos++] = HEX_CH[cell.terrain & 0xF];
        buf[pos++] = HEX_CH[dd >> 4]; buf[pos++] = HEX_CH[dd & 0xF];
        buf[pos++] = HEX_CH[vv >> 4]; buf[pos++] = HEX_CH[vv & 0xF];
      }
    }
  }
  pos += snprintf(buf + pos, cap - pos, "\"}");
  return pos;
}
