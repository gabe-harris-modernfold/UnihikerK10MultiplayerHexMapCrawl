#pragma once
// ── Encounter message handlers: enc_start, enc_choice, enc_bank, enc_abort ───
// The server is authoritative: the client sends only which hex it is on, which
// choice index it picked, and — on enc_bank — how much of the haul it wants to
// keep.  That last one is a request, clamped against pendingLoot[] here; the
// loot itself is still rolled server-side.  See encounter_engine.hpp.

static void handleMsg_enc_start(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  // The q/r check is the one docs/bot-testing.md calls out: {"t":"enc_start"}
  // alone used to be discarded here with no reply at all.
  const char* qp = strstr(data, "\"q\""); if (!qp) { wsNack(client, "parse"); return; }
  const char* qv = strchr(qp + 3, ':');  if (!qv) { wsNack(client, "parse"); return; }
  const char* rp = strstr(data, "\"r\""); if (!rp) { wsNack(client, "parse"); return; }
  const char* rv = strchr(rp + 3, ':');  if (!rv) { wsNack(client, "parse"); return; }
  int hq = atoi(qv + 1), hr = atoi(rv + 1);
  // Bounds are checked against whichever board the player is on, below --
  // the tunnel board is 16x10, not 75x57. Reject obvious garbage here.
  if (hq < 0 || hr < 0) { wsNack(client, "bad_arg"); return; }

  // Longer than the 5-20 ms the other handlers wait: an enc_start is sent once,
  // on arrival, and a miss here used to be silent -- the browser asks for no
  // reply -- so the survivor stood on the place and nothing happened. It is
  // one message per POI, not per keypress. (The browser also asks again.)
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(60)) != pdTRUE) {
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"mutex_timeout\"}");
    wsNack(client, "busy");
    return;
  }
  int pid = findSlot(client->id());
  if (pid < 0) {
    client->text("{\"t\":\"enc_dbg\",\"msg\":\"no_slot\"}");
    wsNack(client, "not_seated");
  } else if (encounters[pid].active) {
    // Already inside one: the client has lost it -- its enc_path was dropped
    // or its /enc fetch never came back -- since a survivor who can see their
    // scene has no way to send this. That used to be refused ("abort first",
    // with no scene on screen to abort), and every step after it was "Cannot
    // move during encounter" until dawn. Send the scene again, with where it
    // is and what is on the tray, and let them carry on.
    char resume[288];
    const int rn = encPathJson(pid, resume, sizeof(resume), /*resume=*/true);
    xSemaphoreGive(G.mutex);
    Log.notice("enc_start pid=%d: already in a scene -- resent (%d B)", pid, rn);
    if (rn > 0) client->text(resume, (size_t)rn);
    return;
  } else if (G.players[pid].ll == 0) {
    client->text("{\"t\":\"err\",\"msg\":\"Cannot enter — you are downed\"}");
    wsNack(client, "downed");
  } else {
    Player&  p    = G.players[pid];
    bool     below = (p.depth != 0);
    bool     inBounds = below ? tunIn(hq, hr)
                              : (hq < MAP_COLS && hr < MAP_ROWS);
    if (!inBounds) {
      client->text("{\"t\":\"err\",\"msg\":\"No such hex\"}");
      wsNack(client, "bad_arg");
      xSemaphoreGive(G.mutex);
      return;
    }
    HexCell& cell = below ? G.tunnel[hr][hq] : G.map[hr][hq];
    bool claimed = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (i == pid || !encounters[i].active) continue;
      // Depth too: a tunnel hex and a surface hex can share coordinates.
      if (G.players[i].depth != p.depth) continue;
      if (encounters[i].hexQ == (uint8_t)hq && encounters[i].hexR == (uint8_t)hr) { claimed = true; break; }
    }
    uint8_t terrain = cell.terrain;
    int myQ = below ? (int)p.tq : (int)p.q;
    int myR = below ? (int)p.tr : (int)p.r;
    if (myQ != hq || myR != hr) {
      client->text("{\"t\":\"err\",\"msg\":\"Not at that hex\"}");
      wsNack(client, "not_here");
    } else if (cell.poi == 0) {
      client->text("{\"t\":\"err\",\"msg\":\"Already looted\"}");
      wsNack(client, "no_poi");
    } else if (claimed) {
      client->text("{\"t\":\"err\",\"msg\":\"Another survivor is already inside\"}");
      wsNack(client, "claimed");
    } else {
      // Ground with no scenes of its own (a tunnel cell restamped as a hatch
      // or shaft, a terrain index.json has no folder for on this card) still
      // has a place on it: draw from the plainest pool of its board instead
      // of refusing one the map is showing.
      const uint8_t fallback = below ? 14 : 0;
      if (terrain >= NUM_TERRAIN || encPools[terrain].count == 0) {
        Log.warning("enc_start pid=%d (%d,%d) dp=%d: terrain %d has no pool, using %d",
                    pid, hq, hr, (int)below, (int)terrain, (int)fallback);
        terrain = fallback;
      }
      // The id may belong to the terrain this hex had before a fire, flood or
      // founding changed it (encPoolId). enc.encIdx keeps the one that
      // loaded -- encOpenPoolFile() moves on to the next file of the pool if
      // this one will not -- so the choices and a restored POI read the same
      // file.
      uint8_t idx = 0;
      const char* json = encPools[terrain].count
                       ? encOpenPoolFile(terrain, encPoolId(terrain, cell.poi), &idx) : nullptr;
      if (!json) {
        client->text("{\"t\":\"err\",\"msg\":\"The way in is blocked\"}");
        wsNack(client, "load_failed");
      } else {
        cell.poi = 0;  // consume POI; restored if the encounter ends involuntarily
        if (G.threatClock < 20) G.threatClock++;
        ActiveEncounter& enc = encounters[pid];
        enc = {};
        enc.active  = 1;
        enc.encIdx  = idx;
        enc.hexQ    = (uint8_t)hq;
        enc.hexR    = (uint8_t)hr;
        enc.depth   = below ? 1 : 0;   // which board hexQ/hexR index
        enc.terrain = terrain;
        encEnterStartNode(json, enc);
        char pathBuf[96];
        int pathLen = encPathJson(pid, pathBuf, sizeof(pathBuf), /*resume=*/false);
        client->text(pathBuf, (size_t)pathLen);
        GameEvent ev = {};
        ev.type = EVT_ENC_START; ev.pid = (uint8_t)pid;
        ev.q = (int16_t)hq; ev.r = (int16_t)hr;
        // "dp" on the wire: without it every client cleared the POI on the
        // surface hex sharing a tunnel panel's coordinates instead of the panel.
        ev.depth = enc.depth;
        enqEvt(ev);
      }
    }
  }
  xSemaphoreGive(G.mutex);
}

// Place a typed item into the player's pack via addItemToInv() (stacks first,
// then empty slots — the same rule pickups and crafting use); overflow goes
// to the ground where the player stands -- the tunnel cell, when the scene was
// below -- via groundPutHere().  Returns true when some of it landed on the
// ground, so the caller can tell the clients.  Caller holds G.mutex.
static bool grantItemOrDrop(Player& p, uint8_t itemId, uint8_t qty) {
  if (!itemId || !qty) return false;
  qty = (uint8_t)(qty - addItemToInv(p, itemId, qty));
  if (!qty) return false;
  return groundPutHere(p, itemId, qty);   // pack full — the rest lands where they stand
}

// A "spends_item" choice just took its item out of pid's pack (encRunChoice):
// handleMsg_enc_choice owes them an item_result. Per survivor, so a flag left
// by a path that sends no ack only ever reaches its own survivor, later.
static bool encPackSpent[MAX_PLAYERS];
static bool encTakePackSpent(int pid) {
  const bool w = encPackSpent[pid];
  encPackSpent[pid] = false;
  return w;
}

// Resolve choice `ci` of pid's active scene: the cost, the roll, then the
// destination's loot or the hazard, and the events that report it. Shared by
// enc_choice and by enc_abort on a trap's start node -- a trap cannot be
// declined, so "walk away" there means taking the escape door, roll and all.
//
// Returns nullptr once the roll has happened (*outSuccess says which way it
// went), else the nack code for a refusal that changed nothing; the refusal's
// "err" text goes to `client` when there is one. Caller holds G.mutex.
static const char* encRunChoice(AsyncWebSocketClient* client, int pid, int ci, bool* outSuccess) {
  if (outSuccess) *outSuccess = false;
  Player&          p   = G.players[pid];
  ActiveEncounter& enc = encounters[pid];

  const char* json = encLoadFile(enc.terrain, enc.encIdx);
  EncChoice ch;
  if (!json || !encResolveChoice(json, enc.nodeKey, ci, ch)) {
    Log.warning("enc_choice pid=%d node=%s ci=%d: not found", pid, enc.nodeKey, ci);
    if (client) client->text("{\"t\":\"err\",\"msg\":\"That choice is not open to you\"}");
    return "no_choice";
  }
  // "requires_item": the client hides such a choice from anyone without the
  // item, so reaching here means a stale pack or a hand-rolled ci. Refused
  // before the cost, like every other refusal here: nothing has changed.
  if (ch.reqItem && !playerHasForChoice(pid, ch.reqItem)) {
    if (client) client->text("{\"t\":\"err\",\"msg\":\"That choice is not open to you\"}");
    return "no_item";
  }

  bool canAfford = (p.ll >= ch.costLL) && (p.radiation + ch.costRad <= 10) &&
                   (p.inv[1] >= ch.costFood) && (p.inv[0] >= ch.costWat) &&
                   (p.inv[4] >= ch.costScrap) && (p.inv[3] >= ch.costMed) &&
                   ((int)p.score >= ch.costScore);
  if (!canAfford) {
    if (client) client->text("{\"t\":\"err\",\"msg\":\"Cannot afford cost\"}");
    return "no_res";
  }
  // Deduct costs (a negative cost is a gain; LL gains respect the ceiling)
  if (ch.costLL) { p.ll = (uint8_t)constrain((int)p.ll - ch.costLL, 0, (int)effectiveMaxLL(pid)); if (p.ll == 0) p.apLeft = 0; }
  p.radiation = (uint8_t)constrain((int)p.radiation + ch.costRad, 0, 10);
  p.inv[1]    = (uint8_t)constrain((int)p.inv[1] - ch.costFood,  0, 99);
  p.inv[0]    = (uint8_t)constrain((int)p.inv[0] - ch.costWat,   0, 99);
  p.inv[4]    = (uint8_t)constrain((int)p.inv[4] - ch.costScrap, 0, 99);
  p.inv[3]    = (uint8_t)constrain((int)p.inv[3] - ch.costMed,   0, 99);
  // Points: the score, spent. Nothing else in the game takes it; Doctor
  // Gaunt's clinic (lab.hpp) takes nothing else. It rides the state tick.
  if (ch.costScore) p.score = (uint16_t)constrain((int)p.score - ch.costScore, 0, 65535);
  const bool costKilled = (p.ll == 0);   // the price of the choice took the last LL

  // Allies who steadied this roll (handleMsg_enc_assist, below). Their tokens
  // were spent when they offered; the help is spent here, hit or miss, so
  // every roll after this one has to be backed afresh.
  // A "sure" choice is paid for, not rolled: it always goes through, and
  // whatever help was offered waits for the next roll that needs it.
  const uint8_t helpers = ch.sure ? 0 : enc.assistMask;
  const int     nHelp   = __builtin_popcount(helpers);
  if (!ch.sure) enc.assistMask = 0;
  uint8_t dn = ch.sure ? 0 : computeEncounterDN(pid, (uint8_t)ch.baseRisk, ch.skill);
  if (nHelp) dn = (uint8_t)max(2, (int)dn - nHelp * (int)ENC_ASSIST_DN);
  CheckResult cr = {};
  if (ch.sure) cr.success = true;
  else         cr = resolveCheck(pid, ch.skill, dn, 0);
  if (outSuccess) *outSuccess = cr.success;
  GameEvent ev = {};
  ev.type      = EVT_ENC_RESULT;
  ev.pid       = (uint8_t)pid;
  ev.encSkill  = ch.skill;
  ev.encDN     = dn;
  ev.encTotal  = (int8_t)cr.total;
  ev.encOut    = cr.success ? 1 : 0;
  ev.encAssist = (uint8_t)nHelp;
  ev.encSure   = ch.sure ? 1 : 0;
  ev.amt       = enc.trap ? 1 : 0;   // EVT_ENC_RESULT-specific: a trap scene (the LCD tells it differently)
  bool encounterEnded = false;
  // A roll that came off shares its credit with whoever backed it. Score
  // rides the state tick, so the helpers need no event of their own.
  if (cr.success)
    for (int a = 0; a < MAX_PLAYERS; a++)
      if ((helpers & (1u << a)) && G.players[a].connected)
        G.players[a].score = (uint16_t)min((int)G.players[a].score + (int)ENC_ASSIST_SCORE, 65535);
  if (cr.success) {
    // A trade: the gated item changes hands now, on the roll, like a cost
    // that is only paid if it works. What it buys is the destination's loot,
    // pending until the bank like any other.
    if (ch.spendItem && spendItemForChoice(pid, ch.reqItem)) encPackSpent[pid] = true;
    // Kane's chapters and VACANCY's parts reopen until read: once someone
    // has banked out of the middle of one, its middle nodes' tokens are gone.
    if (!ch.nextTerminal && ((enc.kane && kaneMidPaid(enc.kane)) || (enc.vac && vacMidPaid(enc.vac))))
      memset(ch.loot, 0, sizeof(ch.loot));
    uint8_t lootBefore[5];
    memcpy(lootBefore, enc.pendingLoot, 5);
    for (int i = 0; i < 5; i++)
      enc.pendingLoot[i] = (uint8_t)min(99, (int)enc.pendingLoot[i] + (int)ch.loot[i]);
    // Typed items: explicit "item" loot entries first, then the loot-table roll.
    uint8_t itm[3] = { ch.itemType[0], ch.itemType[1], 0 };
    uint8_t qty[3] = { ch.itemQty[0],  ch.itemQty[1],  0 };
    if (ch.lootTable[0]) rollLootTable(ch.lootTable, &itm[2], &qty[2]);
    int shown = 0;
    for (int k = 0; k < 3; k++) {
      if (!itm[k] || !qty[k] || enc.pendingItemCount >= ENC_MAX_ITEMS) continue;
      enc.pendingItemType[enc.pendingItemCount] = itm[k];
      enc.pendingItemQty[enc.pendingItemCount]  = qty[k];
      enc.pendingItemCount++;
      if (shown == 0)      { ev.encItemType  = itm[k]; ev.encItemQty  = qty[k]; }
      else if (shown == 1) { ev.encItemType2 = itm[k]; ev.encItemQty2 = qty[k]; }
      shown++;
    }
    // A recipe is a one-time knowledge grant, pending like the loot above
    // until the player banks — see handleMsg_enc_bank. OR'd into a bitmask
    // (not overwritten) since a single scene can walk through several nodes,
    // each granting a different recipe, before ever banking.
    // Bound the id before the shift (1u << 32+ is UB) — content is hand-authored
    // and should never exceed MAX_RECIPES, but this is parsed from a data file.
    if (ch.recipeId && ch.recipeId <= MAX_RECIPES) {
      enc.pendingRecipes |= (1u << (ch.recipeId - 1));
      ev.encRecipe = ch.recipeId;
    }
    // A tunnel control panel's switch: the section comes on now, for
    // everyone, whatever happens in the rest of the scene (tunnels.hpp).
    if (ch.nextLights && enc.depth) tunnelLightSection(pid, enc.hexQ, enc.hexR);
    // Advance to the destination node
    strncpy(enc.nodeKey, ch.nextKey, ENC_KEY_LEN - 1); enc.nodeKey[ENC_KEY_LEN - 1] = 0;
    enc.canBank = ch.nextCanBank ? 1 : 0;
    enc.escape  = ch.nextEscape  ? 1 : 0;
    if (ch.nextTerminal) enc.active |= (1 << 7);
    // The last node of a Kane chapter: it is read, whatever the survivor
    // banks (kane.hpp). An escape node is a way out, not the end of it.
    if (enc.kane && ch.nextTerminal && !ch.nextEscape) kaneChapterRead(pid, json, ch.nextKey);
    // Likewise the town's and Check-Out's last node (vacancy.hpp). The desk
    // and the corridor count on the bank instead (vacOnBank).
    if (enc.vac && ch.nextTerminal && !ch.nextEscape) vacPartRead(pid, json, ch.nextKey);
    // And the Graft's (lab.hpp); the three buildings count on the bank (labOnBank).
    if (enc.lab && ch.nextTerminal && !ch.nextEscape) labPartRead(pid, json, ch.nextKey);
    // And each of Bonney's two sections (bonney.hpp): its "bonney_ending".
    if (enc.bonney && ch.nextTerminal && !ch.nextEscape) bonneySectionRead(pid, json, ch.nextKey);
    // What this roll put on the tray, after the hooks above: Bonney's real
    // apple is added by bonneySectionRead(). The client builds its tray --
    // and so the keep[] it banks with -- from this, so loot it is not told
    // about is trimmed off at the bank.
    for (int i = 0; i < 5; i++)
      ev.encLoot[i] = enc.pendingLoot[i] > lootBefore[i] ? (uint8_t)(enc.pendingLoot[i] - lootBefore[i]) : 0;
    // cost_ll can take the last point of LL even on a success.
    if (p.ll == 0) encounterEnded = true;
  } else {
    // A sprung trap is a spent trap, so every trap hazard ends the scene
    // whatever the file says -- and it never takes the last point of LL.
    // Traps maim; the wasteland kills. A survivor left on 1 LL with two major
    // wounds twenty hexes from water walks home through systems that already
    // know how to finish the job (docs/trap-system-spec.md, "Tuning").
    if (enc.trap) ch.hazEnds = true;
    const int llBefore = p.ll;
    if (ch.hazLL > 0) {
      p.ll = (uint8_t)min((int)p.ll + ch.hazLL, (int)effectiveMaxLL(pid));
    } else if (ch.hazLL < 0) {
      const int floorLL = enc.trap ? min(1, llBefore) : 0;
      p.ll = (uint8_t)max(floorLL, (int)p.ll + ch.hazLL);
      if (p.ll == 0) { p.apLeft = 0; ledFlash(255, 0, 0); }
      ecoNoteHurt(pid, DC_ENC_HAZARD);   // an injury: the daisies seed on the encounter's hex (ecology.hpp)
    }
    // What the hazard actually cost, not what the file asked for: a range
    // rolls, and the LL floor (or the ground at 0) clips it.
    ev.encPenLL  = (int8_t)(ch.hazLL > 0 ? ch.hazLL : (int)p.ll - llBefore);
    ev.encPenRad = (int8_t)ch.hazRad;
    ev.encEnds   = ch.hazEnds ? 1 : 0;
    p.radiation = (uint8_t)constrain((int)p.radiation + ch.hazRad, 0, 10);
    for (int i = 0; i < 5; i++) {
      int take = min((int)ch.hazRes[i], (int)p.inv[i]);
      p.inv[i] = (uint8_t)(p.inv[i] - take);
      ev.encPenRes[i] = (uint8_t)take;
    }
    ev.encPenWndMin = addWound(p, WOUND_MINOR, ch.hazWMin);
    ev.encPenWndMaj = addWound(p, WOUND_MAJOR, ch.hazWMaj);
    // Dshinguest Ron's parting gift (dshingus.hpp): a hazard can give it.
    if (ch.hazDsh) dshCatch(pid, DSH_SRC_BAND);
    // Bonney's runaway hand (bonney.hpp): while you look at the one she has,
    // the other goes through your pack. Once a scene.
    if (ch.hazSteal && enc.bonney) bonneyPinch(pid);
    // A major wound costs AP for the rest of the day, not just from dawn.
    if (ev.encPenWndMaj) p.apLeft = (int8_t)max(0, (int)p.apLeft - (int)ev.encPenWndMaj);
    encounterEnded = (p.ll == 0) || ch.hazEnds;
    // Nothing is taken from survivors standing nearby. There used to be an
    // automatic drain here -- 1-2 tokens off everyone on the hex for every
    // hazard, for nothing in return -- which made standing beside a teammate
    // who opened a POI strictly worse than being anywhere else. Helping is
    // now opt-in and buys something: handleMsg_enc_assist.
  }
  // Result must be broadcast before EVT_ENC_END so allies see the outcome before the encounter clears.
  enqEvt(ev);
  if (encounterEnded) {
    if (p.ll == 0) {
      GameEvent devt = {}; devt.type = EVT_DOWNED; devt.pid = (uint8_t)pid; devt.evWsId = p.wsClientId;
      devt.res = (costKilled || cr.success) ? DC_ENC_COST : DC_ENC_HAZARD;
      enqEvt(devt);
    }
    endEncounter(pid, (p.ll == 0) ? ENC_END_DOWNED : ENC_END_HAZARD, /*restorePoi=*/false);
  }
  return nullptr;
}

// A section just came on (tunnelLightSection): everyone below gets a fresh vis
// disk, which now carries its lit cells -- that is what reveals them and walks
// the lights out on their screens. pushVisDisk() takes G.mutex itself, so this
// runs after the handler has let go of it.
static void pushLitVisDisks() {
  bool     below[MAX_PLAYERS] = {};
  uint32_t wsId[MAX_PLAYERS]  = {};
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  tunnelLightsPending = false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    below[i] = G.players[i].connected && G.players[i].depth;
    wsId[i]  = G.players[i].wsClientId;
  }
  xSemaphoreGive(G.mutex);
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (below[i])
      if (AsyncWebSocketClient* cl = ws.client(wsId[i])) pushVisDisk(cl, i);
}

static void handleMsg_enc_choice(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  // {"t":"enc_choice","ci":N}
  const char* cp = strstr(data, "\"ci\""); if (!cp) { wsNack(client, "parse"); return; }
  const char* cv = strchr(cp + 4, ':');   if (!cv) { wsNack(client, "parse"); return; }
  int ci = atoi(cv + 1);
  if (ci < 0 || ci > 15) { wsNack(client, "bad_arg"); return; }

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) { wsNack(client, "busy"); return; }
  int pid = findSlot(client->id());
  if (pid < 0 || !encounters[pid].active || G.players[pid].ll == 0) {
    xSemaphoreGive(G.mutex);
    wsNack(client, pid < 0 ? "not_seated" : !encounters[pid].active ? "no_enc" : "downed");
    return;
  }
  // Check-Out spends the register and the key (vacPartRead), and a
  // "spends_item" choice its item: like enc_bank, that pack change is owed a
  // targeted item_result, built here and sent after the mutex is released.
  PSRAM_STATIC(char, vacAck, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  vacAck[0] = '\0';
  const bool wasTrap = encounters[pid].trap != 0;
  const char* why = encRunChoice(client, pid, ci, nullptr);
  const bool ended = !encounters[pid].active;
  const bool kaneMoved = kaneTakeSaveWanted();   // a chapter was just read
  const bool vacMoved  = vacTakeSaveWanted();    // the town was read, or saved
  const bool labMoved  = labTakeSaveWanted();    // the Graft is done (lab.hpp)
  const bool bonMoved  = bonneyTakeSaveWanted(); // Pinch robbed someone, or a section was read (bonney.hpp)
  const bool vacOut    = vacTakePackChanged(pid);
  const bool labGraft  = labTakePackChanged(pid);   // three parts in, the heart and the note out
  const char* bonAct   = bonneyTakePackAct(pid);    // Pinch took something, or gave it back
  const bool spent     = encTakePackSpent(pid);  // a trade handed its item over
  if (vacOut || labGraft || bonAct[0] || spent) {
    int ap = appendFmt(vacAck, 512, 0, "{\"t\":\"item_result\",\"ok\":true,\"act\":\"%s\",\"pid\":%d,",
                       vacOut ? "vac_out" : labGraft ? "lab_graft" : bonAct[0] ? bonAct : "enc_spend", pid);
    ap = appendPackArrays(vacAck, 512, ap, pid);
    appendFmt(vacAck, 512, ap, "}");
  }
  int spillQ = 0, spillR = 0;
  bool spilled = labTakeSpill(spillQ, spillR);   // the Graft's gift beside a full pack
  if (!spilled) spilled = bonneyTakeSpill(spillQ, spillR);   // or what Bonney gave back
  const bool lit = tunnelLightsPending;          // a section of tunnel lights came on
  xSemaphoreGive(G.mutex);
  if (why) wsNack(client, why);
  if (vacAck[0]) client->text(vacAck);
  if (spilled) broadcastGroundUpdate(spillQ, spillR);
  // Everyone below sees it, and the board changed for the save.
  if (lit) { pushLitVisDisks(); requestSave(); }
  // A trap that went off changed the board for everyone (and for the save).
  if ((wasTrap && ended) || kaneMoved || vacMoved || labMoved || bonMoved) requestSave();
}

static void handleMsg_enc_bank(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  // Built inside the mutex below (if any items were pending) and sent after
  // it's released — same static-ack pattern as handleMsg_act's craft path.
  PSRAM_STATIC(char, itemAck, [512]);   // appendPackArrays() writes INV_SLOTS_MAX-wide arrays
  itemAck[0] = '\0';
  // Optional trim: {"t":"enc_bank","keep":[w,f,fu,m,s]} — how much of each
  // resource the player chose to take on the haul tray's steppers.  An absent
  // or malformed "keep" means "bank everything", which is what an older client
  // sends, so the default is wide open; every entry is clamped against
  // pendingLoot[] below, so this can only ever take less than was won.
  // Same array idiom as car_buy's "give" (network-msg-trade.hpp).
  uint8_t keep[5] = { 99, 99, 99, 99, 99 };
  const char* kp = strstr(data, "\"keep\"");
  if (kp) {
    const char* kb = strchr(kp + 6, '[');
    if (kb) {
      kb++;
      for (int i = 0; i < 5; i++) {
        while (*kb == ' ') kb++;
        keep[i] = (uint8_t)constrain(atoi(kb), 0, 99);
        const char* nx = strchr(kb, i < 4 ? ',' : ']');
        if (!nx) break;
        kb = nx + 1;
      }
    }
  }
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) { wsNack(client, "busy"); return; }
  bool banked = false;
  bool spilled = false;          // pack full -- some of the haul went on the ground
  int  spillQ = 0, spillR = 0;
  int pid = findSlot(client->id());
  if (pid < 0)                         wsNack(client, "not_seated");
  else if (!encounters[pid].active)    wsNack(client, "no_enc");
  if (pid >= 0 && encounters[pid].active) {
    Player& p = G.players[pid];
    ActiveEncounter& enc = encounters[pid];
    bool fullClear = (enc.active & (1 << 7)) != 0;
    if (!fullClear && !enc.canBank) {
      client->text("{\"t\":\"err\",\"msg\":\"You can't carry loot out from here\"}");
      wsNack(client, "cannot_bank");
    } else {
      int totalRes = 0;
      for (int i = 0; i < 5; i++) {
        // Take the smaller of what was rolled and what was asked for.  Writing
        // the result back into pendingLoot keeps the EVT_ENC_BANK memcpy below
        // honest: the event must report what was actually banked, or the
        // client's _evEncBank would add the untrimmed amount to its inv[].
        // What's left over is simply left behind — no ground drop, no refund.
        int take = min((int)enc.pendingLoot[i], (int)keep[i]);
        enc.pendingLoot[i] = (uint8_t)take;
        totalRes += take;
      }
      // Leaving with nothing from an escape node (either kind of scene), or
      // from a trap at all, is not a bank: no full-clear bonus, no score, no
      // encounter tally -- and a trap left this way is still armed, now on
      // this survivor's map. "Leave with nothing -> it is still there. Leave
      // with anything -> it is spent" (docs/trap-system-spec.md, Outcomes).
      const bool tookAnything = totalRes > 0 || enc.pendingItemCount > 0 || enc.pendingRecipes != 0;
      if (!tookAnything && (enc.trap || enc.escape)) {
        if (enc.trap) trapSettle(pid, TRAP_OUT_KNOWN);
        endEncounter(pid, ENC_END_ESCAPED, /*restorePoi=*/false);
        xSemaphoreGive(G.mutex);
        requestSave();
        return;
      }
      // Tokens carried out of the middle of a scene that reopens (kaneMidPaid,
      // vacMidPaid): that part's middle has paid.
      if (!fullClear && totalRes > 0) {
        if (enc.kane) kaneMidBank(enc.kane);
        if (enc.vac)  vacMidBank(enc.vac);
      }
      // A ghost riding them takes its third of the haul on the way out
      // (ghosts.hpp). After the took-anything test: a haul the survivor
      // carried out spends the place even if the ghost got all of it. Written
      // back to pendingLoot for the same reason as the trim, and the score
      // below is on what they kept.
      totalRes = 0;
      for (int i = 0; i < 5; i++) {
        enc.pendingLoot[i] = ghostTithe(pid, (uint8_t)i, enc.pendingLoot[i]);
        totalRes += enc.pendingLoot[i];
      }
      for (int i = 0; i < 5; i++) p.inv[i] = (uint8_t)min(99, (int)p.inv[i] + (int)enc.pendingLoot[i]);
      for (int j = 0; j < enc.pendingItemCount; j++)
        spilled |= grantItemOrDrop(p, enc.pendingItemType[j], enc.pendingItemQty[j]);
      { int16_t bq, br; boardPos(p, &bq, &br); spillQ = bq; spillR = br; }
      p.knownRecipes |= enc.pendingRecipes;
      // grantItemOrDrop() just mutated invType[]/invQty[], which — like
      // use_item/equip_item/craft — is private state never carried by the
      // broadcastState() tick or the 'ev' broadcast below. Without this
      // targeted snapshot the banked item sits in the player's save-state
      // but never reaches their on-screen pack until the next full sync.
      if (enc.pendingItemCount) {
        int ap = appendFmt(itemAck, sizeof(itemAck), 0,
          "{\"t\":\"item_result\",\"ok\":true,\"act\":\"enc_bank\",\"pid\":%d,", pid);
        ap = appendPackArrays(itemAck, sizeof(itemAck), ap, pid);
        appendFmt(itemAck, sizeof(itemAck), ap, "}");
      }
      // An escape node is terminal but it is a way out, not the bottom of
      // the place -- it never pays the full-clear bonus.
      int scoreGain = totalRes * 3 + ((fullClear && !enc.escape) ? 10 : 0);
      GameEvent ev = {};
      ev.type = EVT_ENC_BANK; ev.pid = (uint8_t)pid;
      ev.q = (int16_t)enc.hexQ; ev.r = (int16_t)enc.hexR;
      ev.depth = enc.depth;
      // EVT_ENC_BANK-specific: the full-clear bonus was paid. The LCD used to
      // infer it from scoreD >= 13, which five tokens also reach.
      ev.amt = (fullClear && !enc.escape) ? 1 : 0;
      memcpy(ev.encLoot, enc.pendingLoot, 5);
      ev.bankedRecipes = enc.pendingRecipes;
      addScore(p, ev, scoreGain);
      p.encCount++;
      enqEvt(ev);
      // Taking the bait is what disarms it. After the bank event, so every
      // client sees the haul land before the hex goes quiet.
      if (enc.trap) trapSettle(pid, TRAP_OUT_SPENT);
      // A tunnel panel banked short of its switch is still a panel -- see
      // endEncounter(). The tunnel scenes never offer a bank before the
      // lights, so this is the belt to their braces.
      if (enc.depth && !enc.trap && !enc.lab && tunnelPanelDark(enc.hexQ, enc.hexR) &&
          G.tunnel[enc.hexR][enc.hexQ].poi == 0)
        G.tunnel[enc.hexR][enc.hexQ].poi = enc.encIdx;
      // The front desk or the corridor (vacancy.hpp): done once its item is
      // out in the world. Reads the pending haul, so before the slot clears.
      if (enc.vac) vacOnBank(pid);
      // Likewise a clinic building (lab.hpp): done once its part is out.
      if (enc.lab) labOnBank(pid);
      enc = {};
      banked = true;
    }
  }
  xSemaphoreGive(G.mutex);
  // Persist outside the mutex (saveGame takes it itself) — same pattern as
  // handleMsg_act's craft path. The POI was consumed when the scene opened,
  // so any save between then and now (another survivor's craft/use_item, a
  // disconnect) already holds the emptied hex; without a save here a reboot
  // would keep that empty hex yet forget the loot, items and — worst of all —
  // the one-time recipes that were just banked from it.
  if (banked) requestSave();
  if (itemAck[0]) client->text(itemAck);
  // The overflow pile used to reach clients only with the next full sync.
  if (spilled) broadcastGroundUpdate(spillQ, spillR);
}

static void handleMsg_enc_abort(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* lfp = strstr(data, "\"lf\"");
  const char* lfv = lfp ? strchr(lfp + 4, ':') : nullptr;
  const bool  loadFailed = lfv && atoi(lfv + 1) != 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(20)) != pdTRUE) { wsNack(client, "busy"); return; }
  int pid = findSlot(client->id());
  const char* why = nullptr;
  bool trapChanged = false;
  if (pid < 0 || !encounters[pid].active) {
    why = (pid < 0) ? "not_seated" : "no_enc";
  } else if (encounters[pid].trap) {
    // There is no walking away from a trap -- that is the whole of it. What
    // "abort" means depends on where in the scene you are:
    //  * past the mechanism (a cache node) or already through the escape
    //    door: you walk off with nothing, so it stays armed, and you know it;
    //  * still at the start: you take the escape door, roll and all. The
    //    client never offers a walk-away there, so this is the path a client
    //    that failed to load the scene (or a bot) takes -- it resolves the
    //    scene instead of wedging the survivor in it until dawn.
    // The threat clock does not tick: nobody chose to be here.
    ActiveEncounter& enc = encounters[pid];
    trapChanged = true;
    const char* json = encLoadFile(enc.terrain, enc.encIdx);
    if (enc.canBank || (enc.active & (1 << 7))) {
      trapSettle(pid, TRAP_OUT_KNOWN);
      endEncounter(pid, ENC_END_ESCAPED, /*restorePoi=*/false);
    } else if (!json) {
      // The file will not load (SD trouble): nothing can be rolled, so put
      // the trap back exactly as it was and let them go.
      trapSettle(pid, TRAP_SETTLE_REARM);
      endEncounter(pid, ENC_END_ESCAPED, /*restorePoi=*/false);
    } else {
      int ci = encEscapeChoice(json, enc.nodeKey);
      if (ci < 0) {
        client->text("{\"t\":\"err\",\"msg\":\"There is no way back out from here. Choose.\"}");
        why = "trap";
        trapChanged = false;
      } else {
        bool ok = false;
        why = encRunChoice(client, pid, ci, &ok);
        if (!why && ok && encounters[pid].active) {
          trapSettle(pid, TRAP_OUT_KNOWN);
          endEncounter(pid, ENC_END_ESCAPED, /*restorePoi=*/false);
        }
        // On a failed roll encRunChoice already ended it: a trap hazard always does.
      }
    }
  } else if (encounters[pid].kane || encounters[pid].vac || encounters[pid].lab || encounters[pid].bonney) {
    // Walking away from one of Kane's chapters leaves it where it was -- his
    // camp, the house, his fire -- and nobody chose to be there, so the
    // threat clock does not tick either. The same for VACANCY's sign and
    // its two errands (vacancy.hpp), for Gaunt's clinic (lab.hpp), and for
    // Bonney's fire (bonney.hpp) -- though whatever Pinch took stays took.
    endEncounter(pid, ENC_END_ABORT, /*restorePoi=*/false);
  } else if (loadFailed) {
    // {"t":"enc_abort","lf":1}: the client never got the scene to show (its
    // /enc fetch failed). Nobody walked away from anything, so the place
    // stays and the clock holds -- what a dropped socket already gets
    // (releaseSeatLocked), so it opens no door that was not open. It used to
    // be taken as walking off: the POI gone for good, a tick on the clock,
    // and "The way in is blocked" the only word of it.
    endEncounter(pid, ENC_END_ABORT, /*restorePoi=*/true);
  } else {
    if (G.threatClock < 20) G.threatClock++;
    endEncounter(pid, ENC_END_ABORT, /*restorePoi=*/false);  // walking away closes the place for good
  }
  xSemaphoreGive(G.mutex);
  if (why) wsNack(client, why);
  if (trapChanged || loadFailed) requestSave();
}

// ── enc_assist: steady a teammate's next roll ────────────────────────────────
// {"t":"enc_assist","to":PID,"res":0-4}, from a survivor on the same hex (the
// same board, and underground the same corridor cell -- samehex()) as someone
// in a scene. The helper spends one token of `res`, and the scene's next roll
// is ENC_ASSIST_DN easier; at most ENC_ASSIST_CAP helpers per roll, one token
// each. encRunChoice() spends the help and credits the helpers if the roll
// comes off. The helper has to be free to act -- seated, standing, not in a
// scene of their own -- but needs no AP: it is a hand, not a day's work.
// No targeted ack: inv[] rides the state tick.
static void handleMsg_enc_assist(AsyncWebSocketClient* client, char* data, size_t len) {
  LOG_FN();
  const char* tp = strstr(data, "\"to\"");  if (!tp) { wsNack(client, "parse"); return; }
  const char* tv = strchr(tp + 4, ':');     if (!tv) { wsNack(client, "parse"); return; }
  const char* rp = strstr(data, "\"res\""); if (!rp) { wsNack(client, "parse"); return; }
  const char* rv = strchr(rp + 5, ':');     if (!rv) { wsNack(client, "parse"); return; }
  int to = atoi(tv + 1), res = atoi(rv + 1);
  if (to < 0 || to >= MAX_PLAYERS || res < 0 || res > 4) { wsNack(client, "bad_arg"); return; }

  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) != pdTRUE) { wsNack(client, "busy"); return; }
  int pid = findSlot(client->id());
  const ActiveEncounter& te = encounters[to];
  const char* refused =
      (pid < 0)                                          ? "not_seated"
    : (pid == to)                                        ? "self"
    : (G.players[pid].ll == 0)                           ? "downed"
    : encounters[pid].active                             ? "in_enc"
    : (!G.players[to].connected || !te.active)           ? "no_target"
    : (te.active & (1 << 7))                             ? "no_roll"       // a terminal node: nothing left to roll
    : !samehex(pid, to)                                  ? "not_same_hex"
    : (te.assistMask & (1u << pid))                      ? "dup_assist"
    : (__builtin_popcount(te.assistMask) >= ENC_ASSIST_CAP) ? "assist_full"
    : (G.players[pid].inv[res] == 0)                     ? "no_res"
    : nullptr;
  if (!refused) {
    G.players[pid].inv[res]--;
    encounters[to].assistMask |= (uint8_t)(1u << pid);
    GameEvent ev = {};
    ev.type      = EVT_ENC_ASSIST;
    ev.pid       = (uint8_t)pid;
    ev.amt       = (uint8_t)to;
    ev.res       = (uint8_t)res;
    ev.encAssist = (uint8_t)__builtin_popcount(encounters[to].assistMask);
    enqEvt(ev);
  }
  xSemaphoreGive(G.mutex);
  if (refused) wsNack(client, refused);
}
