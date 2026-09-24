#pragma once
// ── Known WiFi networks: a small NVS-backed roaming list ─────────────────────
// The ESP32 keeps exactly ONE set of STA credentials in its own internal NVS,
// so carrying the board between locations (home → a friend's place → home)
// meant retyping credentials on every move. This store remembers the last
// WIFI_MAX_NETS networks the board actually joined; the auto-join sweep in
// network-session.hpp scans the air and joins whichever known network is in
// range with the strongest signal.
//
// Storage: Preferences namespace "wifinets" — "n" (count) plus "s0".."s7" /
// "p0".."p7". Order is most-recently-joined first and the oldest entry is
// evicted when the list is full. Nothing is written unless the list actually
// changed, so booting onto the same network every day costs no NVS wear.
//
// Included from Esp32HexMapCrawl.ino right after logging.hpp (needs Log).

#include <Preferences.h>

static constexpr uint8_t WIFI_MAX_NETS = 8;

struct KnownNet {
  char ssid[33];
  char pass[65];
};

static KnownNet* g_knownNets = nullptr;   // [WIFI_MAX_NETS], PSRAM (allocPsramGlobals in the .ino)
static uint8_t  g_knownCount = 0;

static int wifiStoreFind(const char* ssid) {
  if (!ssid || !ssid[0]) return -1;
  for (uint8_t i = 0; i < g_knownCount; i++)
    if (strcmp(g_knownNets[i].ssid, ssid) == 0) return (int)i;
  return -1;
}

static void wifiStoreSave() {
  Preferences p;
  if (!p.begin("wifinets", false)) { Log.error("wifiStore: NVS open for write failed"); return; }
  char key[6];
  p.putUChar("n", g_knownCount);
  for (uint8_t i = 0; i < g_knownCount; i++) {
    snprintf(key, sizeof(key), "s%u", (unsigned)i); p.putString(key, g_knownNets[i].ssid);
    snprintf(key, sizeof(key), "p%u", (unsigned)i); p.putString(key, g_knownNets[i].pass);
  }
  // Drop slots past the current count — otherwise a forget() would leave a
  // ghost entry that the next load could resurrect.
  for (uint8_t i = g_knownCount; i < WIFI_MAX_NETS; i++) {
    snprintf(key, sizeof(key), "s%u", (unsigned)i); p.remove(key);
    snprintf(key, sizeof(key), "p%u", (unsigned)i); p.remove(key);
  }
  p.end();
  Log.notice("wifiStore: saved %d network(s)", (int)g_knownCount);
}

static void wifiStoreLoad() {
  Preferences p;
  g_knownCount = 0;
  if (!p.begin("wifinets", true)) { LOG_VERBOSE("wifiStore: no NVS namespace yet"); return; }
  uint8_t n = p.getUChar("n", 0);
  if (n > WIFI_MAX_NETS) n = WIFI_MAX_NETS;
  char key[6];
  for (uint8_t i = 0; i < n; i++) {
    snprintf(key, sizeof(key), "s%u", (unsigned)i);
    String s = p.getString(key, "");
    if (!s.length()) continue;
    snprintf(key, sizeof(key), "p%u", (unsigned)i);
    String pw = p.getString(key, "");
    strlcpy(g_knownNets[g_knownCount].ssid, s.c_str(), sizeof(g_knownNets[0].ssid));
    strlcpy(g_knownNets[g_knownCount].pass, pw.c_str(), sizeof(g_knownNets[0].pass));
    g_knownCount++;
  }
  p.end();
  Log.notice("wifiStore: %d known network(s) loaded", (int)g_knownCount);
  for (uint8_t i = 0; i < g_knownCount; i++)
    LOG_VERBOSE("wifiStore: known[%d] ssid=%s", (int)i, g_knownNets[i].ssid);
}

// Record a network that was just joined successfully. An existing entry moves
// to the front (most recent first) and picks up the new password; a new one is
// inserted at the front, evicting the oldest. Returns true if NVS was written.
static bool wifiStoreRemember(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return false;
  if (!pass) pass = "";

  int idx = wifiStoreFind(ssid);
  if (idx == 0 && strcmp(g_knownNets[0].pass, pass) == 0) return false;  // already on top, unchanged

  KnownNet entry;
  if (idx > 0) {
    entry = g_knownNets[idx];
    for (int i = idx; i > 0; i--) g_knownNets[i] = g_knownNets[i - 1];
  } else if (idx == 0) {
    entry = g_knownNets[0];
  } else {
    memset(&entry, 0, sizeof(entry));
    strlcpy(entry.ssid, ssid, sizeof(entry.ssid));
    uint8_t shiftFrom = (g_knownCount < WIFI_MAX_NETS) ? g_knownCount
                                                       : (uint8_t)(WIFI_MAX_NETS - 1);
    for (int i = shiftFrom; i > 0; i--) g_knownNets[i] = g_knownNets[i - 1];
    if (g_knownCount < WIFI_MAX_NETS) g_knownCount++;
  }
  strlcpy(entry.pass, pass, sizeof(entry.pass));
  g_knownNets[0] = entry;

  Log.notice("wifiStore: remember ssid=%s (%d known)", ssid, (int)g_knownCount);
  wifiStoreSave();
  return true;
}

static bool wifiStoreForget(const char* ssid) {
  int idx = wifiStoreFind(ssid);
  if (idx < 0) return false;
  for (uint8_t i = (uint8_t)idx; i + 1 < g_knownCount; i++) g_knownNets[i] = g_knownNets[i + 1];
  g_knownCount--;
  memset(&g_knownNets[g_knownCount], 0, sizeof(KnownNet));
  Log.notice("wifiStore: forget ssid=%s (%d left)", ssid, (int)g_knownCount);
  wifiStoreSave();
  return true;
}

static const char* wifiStorePass(const char* ssid) {
  int idx = wifiStoreFind(ssid);
  return idx < 0 ? nullptr : g_knownNets[idx].pass;
}

// Build {"t":"wifi","status":"nets","cur":"<ssid|>","nets":["a","b"]} into out.
// SSIDs are JSON-escaped; passwords are never sent in this message.
static int wifiStoreNetsJson(char* out, size_t cap, const char* current) {
  size_t w = 0;
  auto put = [&](const char* s) { while (*s && w + 1 < cap) out[w++] = *s++; };
  auto putEsc = [&](const char* s) {
    for (; *s && w + 2 < cap; s++) {
      if (*s == '"' || *s == '\\') out[w++] = '\\';
      else if ((unsigned char)*s < 0x20) continue;
      out[w++] = *s;
    }
  };
  put("{\"t\":\"wifi\",\"status\":\"nets\",\"cur\":\"");
  putEsc(current ? current : "");
  put("\",\"nets\":[");
  for (uint8_t i = 0; i < g_knownCount; i++) {
    if (i) put(",");
    put("\"");
    putEsc(g_knownNets[i].ssid);
    put("\"");
  }
  put("]}");
  if (w >= cap) w = cap - 1;
  out[w] = '\0';
  return (int)w;
}
