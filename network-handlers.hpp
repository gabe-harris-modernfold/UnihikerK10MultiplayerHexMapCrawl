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
  else if (CMD_IS("loot")         ) handleMsg_loot(client, data, len);
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

// Last time async_tcp delivered any WS event -- the network watchdog
// (game-server.hpp) reports it, so a wedge shows how long the socket side
// has been silent.
static volatile uint32_t g_lastWsEvtMs  = 0;
static volatile uint32_t g_lastWsDataMs = 0;  // inbound traffic only (DATA / PING / PONG): a
                                              // DISCONNECT is lwIP giving up, not the link working
static volatile uint32_t g_wsEvtN       = 0;  // every WS event; NETWD reports the rate

// ── Reassembly of messages that arrive in pieces ─────────────────────────────
// The library raises WS_EVT_DATA once per chunk: a message split across TCP
// segments, or sent as several WS frames, comes in two or more calls. Only
// the one-call case used to be handled, so anything else was dropped with no
// reply -- fuzz.py's n:long-1400/4000 got silence (2026-10-12), and a small
// command split across segments by a retransmit would have vanished the same
// way. Pieces are gathered per client here and dispatched whole. Only the
// async_tcp task runs onWsEvent, so none of this needs a lock.
static constexpr size_t WS_MSG_MAX   = 4096;  // a client command is ~300 B at most
static constexpr int    WS_ASM_SLOTS = 8;     // clients mid-message at once
struct WsAsm { uint32_t id; uint32_t len; bool over; };
static WsAsm s_wsAsm[WS_ASM_SLOTS] = {};

static char* wsAsmBuf(int slot) {
  PSRAM_STATIC(char, bufs, [WS_ASM_SLOTS][WS_MSG_MAX + 1]);
  return bufs[slot];
}

static void wsAsmFree(uint32_t id) {
  for (int i = 0; i < WS_ASM_SLOTS; i++) if (s_wsAsm[i].id == id) s_wsAsm[i].id = 0;
}

static void wsDataEvent(AsyncWebSocketClient* client, AwsFrameInfo* info,
                        uint8_t* data, size_t len) {
  uint32_t id    = client->id();
  // A message starts on a frame's first byte that is not a continuation; it
  // ends on the last byte of its final frame. (info->num is no use here: the
  // library resets it whenever a frame is split.)
  bool     start = info->index == 0 && info->opcode != WS_CONTINUATION;
  bool     end   = info->final && info->index + len == info->len;

  if (info->message_opcode != WS_TEXT) {   // binary is never a command
    if (end) Log.warning("WS binary msg dropped id=%u len=%u", (unsigned)id, (unsigned)info->len);
    wsAsmFree(id);
    return;
  }
  if (start && end) {                      // the usual case: whole in one call
    wsAsmFree(id);
    data[len] = 0;                         // the library leaves room for it
    handleMessage(client, (char*)data, len);
    return;
  }

  int s = -1;
  for (int i = 0; i < WS_ASM_SLOTS && s < 0; i++) if (s_wsAsm[i].id == id) s = i;
  if (start) {
    if (s < 0) for (int i = 0; i < WS_ASM_SLOTS && s < 0; i++) if (!s_wsAsm[i].id) s = i;
    if (s < 0) { Log.warning("WS reassembly full, msg dropped id=%u", (unsigned)id); return; }
    s_wsAsm[s] = { id, 0, false };
  } else if (s < 0) {
    return;                                // the rest of a message already given up on
  }
  WsAsm& a = s_wsAsm[s];
  if (!a.over) {
    if (a.len + len > WS_MSG_MAX) {
      a.over = true;                       // no reply: its rid cannot be trusted
      Log.warning("WS msg over %u B dropped id=%u", (unsigned)WS_MSG_MAX, (unsigned)id);
    } else {
      memcpy(wsAsmBuf(s) + a.len, data, len);
      a.len += len;
    }
  }
  if (end) {
    char* buf = wsAsmBuf(s);
    bool  ok  = !a.over;
    uint32_t n = a.len;
    a.id = 0;                              // free before dispatch: a handler may send
    if (ok) {
      buf[n] = 0;
      LOG_VERBOSE("WS reassembled id=%u len=%u", (unsigned)id, (unsigned)n);
      handleMessage(client, buf, n);
    }
  }
}

static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  g_lastWsEvtMs = millis();
  if (type == WS_EVT_DATA || type == WS_EVT_PING || type == WS_EVT_PONG) g_lastWsDataMs = g_lastWsEvtMs;
  g_wsEvtN++;
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
    case WS_EVT_DISCONNECT: wsAsmFree(client->id()); handleDisconnect(client); break;
    case WS_EVT_DATA:       wsDataEvent(client, (AwsFrameInfo*)arg, data, len); break;
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
