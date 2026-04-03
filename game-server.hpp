#pragma once
// ── game-server.hpp ─────────────────────────────────────────────────────────
// Game loop task (Core 1) and server setup helpers extracted from setup().
// Included LAST, after all gameplay and network .hpp files.

// ── Game loop task (Core 1) ────────────────────────────────────
static void gameLoopTask(void* param) {
  Serial.printf("[SETUP] Game loop on Core %d\n", xPortGetCoreID());
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TICK_MS));
    tickGame();
    drainEvents();
    // ── Trade offer expiry sweep ──────────────────────────────────
    {
      uint32_t nowMs = millis();
      for (int ti = 0; ti < MAX_PLAYERS; ti++) {
        if (tradeOffers[ti].active && nowMs >= tradeOffers[ti].expiresMs) {
          if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            if (tradeOffers[ti].active && nowMs >= tradeOffers[ti].expiresMs) {
              GameEvent tev = {};
              tev.type        = EVT_TRADE_RESULT;
              tev.pid         = tradeOffers[ti].fromPid;
              tev.tradeTo     = tradeOffers[ti].toPid;
              tev.tradeResult = 3; // expired
              tradeOffers[ti].active = false;
              enqEvt(tev);
            }
            xSemaphoreGive(G.mutex);
          }
        }
      }
    }
    broadcastState();
  }
}

// ── Derive image variant counts from PSRAM imgCache ────────────
// Called from setup() after loadWebFilesToRAM(). Populates terrainVariantCount[],
// shelterVariantCount[], and forrageAnimalCount by scanning cached filenames.
static void setupVariantCounts() {
  Serial.print("[SETUP] Variants:");
  for (int i = 0; i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    for (int t = 0; t < NUM_TERRAIN; t++) {
      String pfx = String("hex") + TERRAIN_IMG_NAME[t];
      if (fname.startsWith(pfx) && fname.endsWith(".png")) {
        String numStr = fname.substring(pfx.length(), fname.length() - 4);
        if (numStr.length() > 0) {
          bool dig = true;
          for (int k = 0; k < (int)numStr.length(); k++)
            if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
          if (dig) {
            int idx = numStr.toInt();
            if (idx + 1 > (int)terrainVariantCount[t])
              terrainVariantCount[t] = (uint8_t)min(idx + 1, 255);
          }
        }
        break;
      }
    }
  }
  const char* SHELTER_PFX[2] = { "shelterBasic", "shelterImproved" };
  for (int i = 0; i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    for (int s = 0; s < 2; s++) {
      String pfx = String(SHELTER_PFX[s]);
      if (fname.startsWith(pfx) && fname.endsWith(".png")) {
        String numStr = fname.substring(pfx.length(), fname.length() - 4);
        if (numStr.length() > 0) {
          bool dig = true;
          for (int k = 0; k < (int)numStr.length(); k++)
            if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
          if (dig) {
            int idx = numStr.toInt();
            if (idx + 1 > (int)shelterVariantCount[s])
              shelterVariantCount[s] = (uint8_t)min(idx + 1, 255);
          }
        }
        break;
      }
    }
  }
  for (int i = 0; i < imgCacheCount; i++) {
    String fname = String(imgCache[i].name);
    if (fname.startsWith("forrageAnimal") && fname.endsWith(".png")) {
      String numStr = fname.substring(13, fname.length() - 4);
      if (numStr.length() > 0) {
        bool dig = true;
        for (int k = 0; k < (int)numStr.length(); k++)
          if (!isDigit((unsigned char)numStr[k])) { dig = false; break; }
        if (dig) {
          int idx = numStr.toInt();
          if (idx + 1 > (int)forrageAnimalCount)
            forrageAnimalCount = (uint8_t)min(idx + 1, 255);
        }
      }
    }
  }
  for (int t = 0; t < NUM_TERRAIN; t++)
    Serial.printf(" %s:%d", T_SHORT[t], terrainVariantCount[t]);
  Serial.printf("  shelterBasic:%d shelterImproved:%d forrageAnimal:%d",
    shelterVariantCount[0], shelterVariantCount[1], forrageAnimalCount);
  Serial.println();
}

// ── WiFi, HTTP routes, and WebSocket setup ──────────────────────
// Extracted from setup() to keep that function concise.
static void setupWiFiAndServer() {
  splashAdd("Starting WiFi...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  {
    wifi_config_t staCfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &staCfg) == ESP_OK && staCfg.sta.ssid[0]) {
      strlcpy(savedSsid, (char*)staCfg.sta.ssid, sizeof(savedSsid));
      splashAdd("Joining saved WiFi...", 0x4080C0);
      Serial.printf("[WIFI] Stored creds found for \"%s\" — connecting\n", savedSsid);
      WiFi.begin();
      bootWifiPending = true;
      bootWifiStartMs = millis();
    } else {
      Serial.println("[WIFI] No stored STA creds — AP only");
    }
  }
  { char wb[30]; snprintf(wb, 30, "AP: %s", WiFi.softAPIP().toString().c_str());
    splashAdd(wb, 0x60A040); }
  Serial.printf("[SETUP] WiFi AP: \"%s\"  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent); ws.enable(true); server.addHandler(&ws);

  // Static web assets served from PSRAM
  for (int i = 0; i < WEB_FILE_COUNT; i++) {
    server.on(WEB_FILES[i].url, HTTP_GET, [i](AsyncWebServerRequest* req) {
      if (WEB_FILES[i].buf) {
        req->send(200, WEB_FILES[i].mime, WEB_FILES[i].buf, WEB_FILES[i].len);
      } else {
        req->send(503, "text/plain", "Web file not cached - check SD & reboot");
      }
    });
  }
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) { req->send(204); });

  // Captive-portal redirects
  auto toGame = [](AsyncWebServerRequest* req) { req->redirect("/"); };
  server.on("/generate_204",              HTTP_GET, toGame);
  server.on("/gen_204",                   HTTP_GET, toGame);
  server.on("/hotspot-detect.html",       HTTP_GET, toGame);
  server.on("/library/test/success.html", HTTP_GET, toGame);
  server.on("/ncsi.txt",                  HTTP_GET, toGame);
  server.on("/connecttest.txt",           HTTP_GET, toGame);
  server.on("/fwlink",                    HTTP_GET, toGame);

  // /state — full game-state JSON endpoint
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    static const char* TNAME_FULL[NUM_TERRAIN] = {
      "Open Scrub","Ash Dunes","Rust Forest","Marsh","Broken Urban",
      "Flooded Ruins","Glass Fields","Rolling Hills","Mountain","Settlement","Nuke Crater","River Channel"
    };
    static const char* RES_NAME_L[6] = {"none","water","food","fuel","medicine","scrap"};
    String j;
    j.reserve(10240);
    if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      j += "{\"day\":";         j += G.dayCount;
      j += ",\"dayTick\":";     j += G.dayTick;
      j += ",\"tickId\":";      j += G.tickId;
      j += ",\"tc\":";          j += G.threatClock;
      j += ",\"crisis\":";      j += G.crisisState  ? "true" : "false";
      j += ",\"connected\":";   j += G.connectedCount;
      j += ",\"sharedFood\":";  j += G.sharedFood;
      j += ",\"sharedWater\":"; j += G.sharedWater;
      j += ",\"evtQueue\":";    j += pendingCount;

      int shelters = 0, impShelters = 0, poiCount = 0;
      int resCnt[6] = {0,0,0,0,0,0};
      int terrCnt[NUM_TERRAIN] = {};
      for (int row = 0; row < MAP_ROWS; row++) {
        for (int col = 0; col < MAP_COLS; col++) {
          const HexCell& c = G.map[row][col];
          if (c.shelter == 1) shelters++;
          else if (c.shelter == 2) impShelters++;
          if (c.resource > 0 && c.resource < 6) resCnt[c.resource] += c.amount;
          if (c.terrain < NUM_TERRAIN) terrCnt[c.terrain]++;
          if (c.poi) poiCount++;
        }
      }
      j += ",\"map\":{\"cells\":"; j += (MAP_ROWS * MAP_COLS);
      j += ",\"shelters\":";    j += shelters;
      j += ",\"impShelters\":"; j += impShelters;
      j += ",\"pois\":";        j += poiCount;
      j += ",\"res\":{\"water\":";  j += resCnt[1];
      j += ",\"food\":";  j += resCnt[2];
      j += ",\"fuel\":";  j += resCnt[3];
      j += ",\"med\":";   j += resCnt[4];
      j += ",\"scrap\":"; j += resCnt[5];
      j += "},\"terrain\":[";
      for (int t = 0; t < NUM_TERRAIN; t++) {
        if (t) j += ",";
        j += "{\"id\":"; j += t;
        j += ",\"name\":\""; j += T_SHORT[t]; j += "\"";
        j += ",\"count\":"; j += terrCnt[t];
        j += "}";
      }
      j += "]}";

      if (req->hasParam("pid")) {
        int vpid = req->getParam("pid")->value().toInt();
        if (vpid >= 0 && vpid < MAX_PLAYERS && G.players[vpid].connected) {
          const Player& vp = G.players[vpid];
          int visR; bool mr;
          playerVisParams(vpid, &visR, &mr);
          j += ",\"view\":{\"pid\":"; j += vpid;
          j += ",\"name\":\""; j += vp.name; j += "\"";
          j += ",\"q\":"; j += vp.q;
          j += ",\"r\":"; j += vp.r;
          j += ",\"visR\":"; j += visR;
          j += ",\"cells\":[";
          bool first = true;
          for (int dr = -visR; dr <= visR; dr++) {
            for (int dq = -visR; dq <= visR; dq++) {
              int s = -(dq + dr);
              if (abs(dq) + abs(dr) + abs(s) > 2 * visR) continue;
              int cq = wrapQ(vp.q + dq);
              int cr = wrapR(vp.r + dr);
              const HexCell& cell = G.map[cr][cq];
              uint8_t tt  = cell.terrain  < NUM_TERRAIN ? cell.terrain  : 0;
              uint8_t res = cell.resource < 6           ? cell.resource : 0;
              if (!first) j += ",";
              first = false;
              j += "{\"q\":"; j += cq;
              j += ",\"r\":"; j += cr;
              j += ",\"dq\":"; j += dq;
              j += ",\"dr\":"; j += dr;
              j += ",\"terrain\":"; j += tt;
              j += ",\"terrainName\":\""; j += TNAME_FULL[tt]; j += "\"";
              j += ",\"shelter\":"; j += cell.shelter;
              j += ",\"resource\":"; j += res;
              j += ",\"resourceName\":\""; j += RES_NAME_L[res]; j += "\"";
              j += ",\"amount\":"; j += cell.amount;
              j += ",\"footprints\":"; j += cell.footprints;
              j += ",\"poi\":";        j += cell.poi ? "true" : "false";
              j += "}";
            }
          }
          j += "]}";
        }
      }

      j += ",\"players\":[";
      for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player& p = G.players[i];
        if (i) j += ",";
        j += "{";
        j += "\"pid\":";          j += i;
        j += ",\"conn\":";        j += p.connected ? "true" : "false";
        j += ",\"wsClientId\":";  j += p.wsClientId;
        j += ",\"connectMs\":";   j += p.connectMs;
        j += ",\"lastMoveMs\":";  j += p.lastMoveMs;
        j += ",\"name\":\"";     j += p.name; j += "\"";
        j += ",\"arch\":";        j += p.archetype;
        j += ",\"archName\":\""; j += (p.archetype < NUM_ARCHETYPES ? ARCHETYPE_NAME[p.archetype] : "?"); j += "\"";
        j += ",\"invSlots\":";    j += p.invSlots;
        j += ",\"q\":";           j += p.q;
        j += ",\"r\":";           j += p.r;
        j += ",\"ll\":";          j += p.ll;
        j += ",\"food\":";        j += p.food;
        j += ",\"water\":";       j += p.water;
        j += ",\"fatigue\":";     j += p.fatigue;
        j += ",\"rad\":";         j += p.radiation;
        j += ",\"resolve\":";     j += p.resolve;
        j += ",\"sb\":";          j += p.statusBits;
        j += ",\"mp\":";          j += p.movesLeft;
        j += ",\"encPenApplied\":"; j += p.encPenApplied ? "true" : "false";
        j += ",\"resting\":";     j += p.resting       ? "true" : "false";
        j += ",\"radClean\":";    j += p.radClean      ? "true" : "false";
        j += ",\"fThreshBelow\":"; j += p.fThreshBelow;
        j += ",\"wThreshBelow\":"; j += p.wThreshBelow;
        j += ",\"wounds\":[";
        j += p.wounds[0]; j += ","; j += p.wounds[1]; j += ","; j += p.wounds[2];
        j += "]";
        j += ",\"skills\":[";
        for (int s = 0; s < NUM_SKILLS; s++) { if (s) j += ","; j += p.skills[s]; }
        j += "]";
        j += ",\"inv\":[";
        for (int s = 0; s < 5; s++) { if (s) j += ","; j += p.inv[s]; }
        j += "]";
        j += ",\"invType\":[";
        for (int s = 0; s < INV_SLOTS_MAX; s++) { if (s) j += ","; j += p.invType[s]; }
        j += "]";
        j += ",\"invQty\":[";
        for (int s = 0; s < INV_SLOTS_MAX; s++) { if (s) j += ","; j += p.invQty[s]; }
        j += "]";
        j += ",\"score\":";       j += p.score;
        j += ",\"steps\":";       j += p.steps;
        j += ",\"encActive\":";   j += encounters[i].active ? "true" : "false";
        if (encounters[i].active) {
          j += ",\"encQ\":";      j += encounters[i].hexQ;
          j += ",\"encR\":";      j += encounters[i].hexR;
          j += ",\"encLoot\":[";
          for (int s = 0; s < 5; s++) { if (s) j += ","; j += encounters[i].pendingLoot[s]; }
          j += "]";
        }
        j += "}";
      }
      j += "]";
      j += "}";
      xSemaphoreGive(G.mutex);
    } else {
      j = "{\"error\":\"mutex timeout\"}";
    }
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", j);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
  });

  // /enc?biome=X&id=Y — serve encounter JSON from SD card
  server.on("/enc", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("biome") || !req->hasParam("id")) {
      req->send(400, "text/plain", "Missing biome or id"); return;
    }
    String biome = req->getParam("biome")->value();
    String id    = req->getParam("id")->value();
    // Validate: only allow alphanumeric + underscore in both params (prevent path traversal)
    for (unsigned i = 0; i < biome.length(); i++) {
      char c = biome[i];
      if (!isAlphaNumeric(c) && c != '_') { req->send(400, "text/plain", "Invalid biome"); return; }
    }
    for (unsigned i = 0; i < id.length(); i++) {
      char c = id[i];
      if (!isDigit(c)) { req->send(400, "text/plain", "Invalid id"); return; }
    }
    char path[56];
    snprintf(path, sizeof(path), "/encounters/%s/%s.json", biome.c_str(), id.c_str());
    if (!SD.exists(path)) { req->send(404, "text/plain", "Encounter not found"); return; }
    req->send(SD, path, "application/json");
  });

  // /img/*.png served from PSRAM imgCache
  server.onNotFound([](AsyncWebServerRequest* req) {
    String url = req->url();
    if (url.startsWith("/img/")) {
      String filename = url.substring(5);
      for (int i = 0; i < imgCacheCount; i++) {
        if (filename.equalsIgnoreCase(imgCache[i].name)) {
          AsyncWebServerResponse* resp = req->beginResponse(
              200, "image/png", imgCache[i].buf, imgCache[i].len);
          resp->addHeader("Cache-Control", "public, max-age=86400");
          req->send(resp);
          return;
        }
      }
    }
    req->send(404, "text/plain", "Not found: " + url);
  });

  server.begin();
  { char hb[30]; snprintf(hb, 30, "Heap: %ukB free", (unsigned)(ESP.getFreeHeap()/1024));
    splashAdd("HTTP+WS ready", 0x60A040);
    splashAdd(hb, 0x406030); }
  Serial.println("[SETUP] HTTP+WS server started");
  Serial.printf("[SETUP] Free heap after init: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.println("[SETUP] ─── Ready. Connect to \"WASTELAND\" → http://192.168.4.1 ───\n");
}
