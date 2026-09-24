#pragma once
// ── Request / reply bookkeeping (ack / nack) ──────────────────────────────────
// A client that puts "rid":N in a message is asking to be told what became of
// it. Exactly one reply comes back per request:
//
//   {"t":"ack","rid":N,"cmd":"m"}                    applied
//   {"t":"nack","rid":N,"cmd":"m","why":"no_mp"}     refused, nothing changed
//
// Without "rid" nothing changes: the browser never sends one, so its traffic
// and the existing err / item_result / trade_fail replies are untouched. Those
// still go out alongside a nack -- the nack is an addition, never a
// replacement.
//
// Before this, most refusals were silent: a return with no reply. docs/bot-
// testing.md lists several that each cost a wasted run to find. A request id
// turns every one of them into something a test can assert on.
//
// An ack means "no refusal path fired", so every refusal path in a handler
// must call wsNack() -- one that returns without it acks a request that did
// nothing. A skill check that rolls and fails is not a refusal (MP was spent,
// the world changed): that is an ack, and the outcome rides the usual event.
//
// Every WS handler runs on the async_tcp task, one message at a time, so a
// single "current request" is safe. handleMessage() opens it before dispatch
// and closes it after.

struct WsRequest {
  bool     hasRid;
  bool     replied;
  uint32_t rid;
  char     cmd[16];
};
static WsRequest g_req = {};

static void wsReqBegin(const char* data, const char* tv, size_t tl) {
  g_req = {};
  size_t cl = tl < sizeof(g_req.cmd) - 1 ? tl : sizeof(g_req.cmd) - 1;
  memcpy(g_req.cmd, tv, cl);
  g_req.cmd[cl] = 0;
  const char* rp = strstr(data, "\"rid\"");
  if (!rp) return;
  const char* rv = strchr(rp + 5, ':');
  if (!rv) return;
  g_req.rid    = (uint32_t)strtoul(rv + 1, nullptr, 10);
  g_req.hasRid = true;
}

// Refuse the current request. `why` is a short snake_case code (a string
// literal); see docs/bot-testing.md "Replies" for the list. Logged verbose
// whether or not the client asked for a reply, so serial shows every refusal.
static void wsNack(AsyncWebSocketClient* client, const char* why) {
  LOG_VERBOSE("WS nack id=%u cmd=%s why=%s",
              client ? (unsigned)client->id() : 0u, g_req.cmd, why);
  if (!client || !g_req.hasRid || g_req.replied) return;
  g_req.replied = true;
  char b[112];
  int n = snprintf(b, sizeof(b),
    "{\"t\":\"nack\",\"rid\":%lu,\"cmd\":\"%s\",\"why\":\"%s\"}",
    (unsigned long)g_req.rid, g_req.cmd, why);
  if (n > 0) client->text(b, (size_t)min(n, (int)sizeof(b) - 1));
}

static void wsReqEnd(AsyncWebSocketClient* client) {
  if (!client || !g_req.hasRid || g_req.replied) return;
  g_req.replied = true;
  char b[80];
  int n = snprintf(b, sizeof(b), "{\"t\":\"ack\",\"rid\":%lu,\"cmd\":\"%s\"}",
                   (unsigned long)g_req.rid, g_req.cmd);
  if (n > 0) client->text(b, (size_t)min(n, (int)sizeof(b) - 1));
}

