#pragma once
// ── WebSocket message dispatch and event dispatcher ───────────────────────────
// Included from Esp32HexMapCrawl.ino after all network-msg-*.hpp files.

static void handleMessage(AsyncWebSocketClient* client, char* data, size_t len) {
  const char* tp = strstr(data, "\"t\"");
  if (!tp) {
    Log.warning("WS malformed msg id=%u (no \"t\" key) head=%.20s",
                (unsigned)client->id(), data);
    return;
  }
  const char* tv = strchr(tp + 3, '"');
  if (!tv) { Log.warning("WS malformed msg id=%u (no t-value open quote)", (unsigned)client->id()); return; }
  tv++;
  const char* te = strchr(tv, '"');
  if (!te) { Log.warning("WS malformed msg id=%u (no t-value close quote)", (unsigned)client->id()); return; }
  size_t tl = (size_t)(te - tv);

  Log.verbose("WS msg id=%u len=%u type=%.*s", (unsigned)client->id(), (unsigned)len, (int)tl, tv);

  if      (strncmp(tv, "pick",          tl) == 0) handleMsg_pick(client, data, len);
  else if (strncmp(tv, "m",             tl) == 0) handleMsg_move(client, data, len);
  else if (strncmp(tv, "n",             tl) == 0) handleMsg_name(client, data, len);
  else if (strncmp(tv, "wifi",          tl) == 0) handleMsg_wifi(client, data, len);
  else if (strncmp(tv, "check",         tl) == 0) handleMsg_check(client, data, len);
  else if (strncmp(tv, "regen",         tl) == 0) handleMsg_regen(client, data, len);
  else if (strncmp(tv, "eraseslot",     tl) == 0) handleMsg_eraseslot(client, data, len);
  else if (strncmp(tv, "act",           tl) == 0) handleMsg_act(client, data, len);
  else if (strncmp(tv, "trade_offer",   tl) == 0) handleMsg_trade_offer(client, data, len);
  else if (strncmp(tv, "trade_accept",  tl) == 0) handleMsg_trade_accept(client, data, len);
  else if (strncmp(tv, "trade_decline", tl) == 0) handleMsg_trade_decline(client, data, len);
  else if (strncmp(tv, "use_item",      tl) == 0) handleMsg_use_item(client, data, len);
  else if (strncmp(tv, "equip_item",    tl) == 0) handleMsg_equip_item(client, data, len);
  else if (strncmp(tv, "unequip_item",  tl) == 0) handleMsg_unequip_item(client, data, len);
  else if (strncmp(tv, "drop_item",     tl) == 0) handleMsg_drop_item(client, data, len);
  else if (strncmp(tv, "pickup_item",   tl) == 0) handleMsg_pickup_item(client, data, len);
  else if (strncmp(tv, "settings",      tl) == 0) handleMsg_settings(client, data, len);
  else if (strncmp(tv, "enc_start",     tl) == 0) handleMsg_enc_start(client, data, len);
  else if (strncmp(tv, "enc_choice",    tl) == 0) handleMsg_enc_choice(client, data, len);
  else if (strncmp(tv, "enc_bank",      tl) == 0) handleMsg_enc_bank(client, data, len);
  else if (strncmp(tv, "enc_abort",     tl) == 0) handleMsg_enc_abort(client, data, len);
  else {
    Log.warning("WS unknown msg type=%.*s id=%u", (int)tl, tv, (unsigned)client->id());
  }
}

static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  const char* tn;
  switch (type) {
    case WS_EVT_CONNECT:    tn = "CONNECT"; break;
    case WS_EVT_DISCONNECT: tn = "DISCONNECT"; break;
    case WS_EVT_DATA:       tn = "DATA"; break;
    case WS_EVT_PONG:       tn = "PONG"; break;
    case WS_EVT_ERROR:      tn = "ERROR"; break;
    default:                tn = "?"; break;
  }
  Log.verbose("WS event id=%u type=%s", (unsigned)client->id(), tn);

  switch (type) {
    case WS_EVT_CONNECT:    handleConnect(client); break;
    case WS_EVT_DISCONNECT: handleDisconnect(client); break;
    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        handleMessage(client, (char*)data, len);
      } else {
        Log.warning("WS frame drop id=%u final=%d idx=%u len=%u opcode=%d",
                    (unsigned)client->id(), (int)info->final,
                    (unsigned)info->index, (unsigned)info->len, (int)info->opcode);
      }
      break;
    }
    case WS_EVT_ERROR: {
      Log.error("WS ERROR id=%u len=%u", (unsigned)client->id(), (unsigned)len);
      break;
    }
    default: break;
  }
}
