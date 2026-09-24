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

  LOG_VERBOSE("WS msg id=%u len=%u type=%.*s", (unsigned)client->id(), (unsigned)len, (int)tl, tv);

  // Exact match on the whole type. This was strncmp(tv, NAME, tl) with tl the
  // *sender's* length, so any prefix matched the first name it began: "e" ran
  // eraseslot, "r" ran regen, "" ran pick -- one typo away from a wiped world.
#define CMD_IS(name) (tl == sizeof(name) - 1 && memcmp(tv, name, tl) == 0)
  wsReqBegin(data, tv, tl);
  if      (CMD_IS("pick")         ) handleMsg_pick(client, data, len);
  else if (CMD_IS("m")            ) handleMsg_move(client, data, len);
  else if (CMD_IS("n")            ) handleMsg_name(client, data, len);
  else if (CMD_IS("wifi")         ) handleMsg_wifi(client, data, len);
  else if (CMD_IS("wifi_forget")  ) handleMsg_wifi_forget(client, data, len);
  else if (CMD_IS("check")        ) handleMsg_check(client, data, len);
  else if (CMD_IS("regen")        ) handleMsg_regen(client, data, len);
  else if (CMD_IS("eraseslot")    ) handleMsg_eraseslot(client, data, len);
  else if (CMD_IS("act")          ) handleMsg_act(client, data, len);
  else if (CMD_IS("trade_offer")  ) handleMsg_trade_offer(client, data, len);
  else if (CMD_IS("trade_accept") ) handleMsg_trade_accept(client, data, len);
  else if (CMD_IS("trade_decline")) handleMsg_trade_decline(client, data, len);
  else if (CMD_IS("car_trade")    ) handleMsg_caravan_trade(client, data, len);
  else if (CMD_IS("car_buy")      ) handleMsg_caravan_buy(client, data, len);
  else if (CMD_IS("use_item")     ) handleMsg_use_item(client, data, len);
  else if (CMD_IS("equip_item")   ) handleMsg_equip_item(client, data, len);
  else if (CMD_IS("unequip_item") ) handleMsg_unequip_item(client, data, len);
  else if (CMD_IS("drop_item")    ) handleMsg_drop_item(client, data, len);
  else if (CMD_IS("drop_res")     ) handleMsg_drop_res(client, data, len);
  else if (CMD_IS("pickup_item")  ) handleMsg_pickup_item(client, data, len);
  else if (CMD_IS("settings")     ) handleMsg_settings(client, data, len);
  else if (CMD_IS("enc_start")    ) handleMsg_enc_start(client, data, len);
  else if (CMD_IS("enc_choice")   ) handleMsg_enc_choice(client, data, len);
  else if (CMD_IS("enc_bank")     ) handleMsg_enc_bank(client, data, len);
  else if (CMD_IS("enc_abort")    ) handleMsg_enc_abort(client, data, len);
  else {
    Log.warning("WS unknown msg type=%.*s id=%u", (int)tl, tv, (unsigned)client->id());
    wsNack(client, "unknown_cmd");
  }
#undef CMD_IS
  wsReqEnd(client);
}

static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  const char* tn;
  switch (type) {
    case WS_EVT_CONNECT:    tn = "CONNECT"; break;
    case WS_EVT_DISCONNECT: tn = "DISCONNECT"; break;
    case WS_EVT_DATA:       tn = "DATA"; break;
    case WS_EVT_PING:       tn = "PING"; break;
    case WS_EVT_PONG:       tn = "PONG"; break;
    case WS_EVT_ERROR:      tn = "ERROR"; break;
    default:                tn = "?"; break;
  }
  LOG_VERBOSE("WS event id=%u type=%s", (unsigned)client->id(), tn);

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
      // Only raised when the peer sends a WS close frame with reason code >
      // 1001 (see AsyncWebSocket.cpp) — arg is that code, data/len its reason
      // string, so this is a real signal worth reading rather than dropping.
      uint16_t code = arg ? *(uint16_t*)arg : 0;
      Log.error("WS ERROR id=%u code=%u reason=%.*s",
                (unsigned)client->id(), code, (int)len, (const char*)data);
      break;
    }
    default: break;
  }
}
