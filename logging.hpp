#pragma once
// ── logging.hpp ──────────────────────────────────────────────────────────────
// Centralised Arduino-Log facade. Timestamps: UTC milliseconds once NTP syncs,
// otherwise millis() ticks ("t+NNNms"). Include once from Esp32HexMapCrawl.ino
// before any other project header that logs.

#include <ArduinoLog.h>
#include <sys/time.h>
#include <time.h>

// Minimum plausible UTC epoch (2023-11-14) — below this we treat the clock as
// unsynced and fall back to tick timestamps.
#ifndef LOG_NTP_MIN_EPOCH
#define LOG_NTP_MIN_EPOCH 1700000000L
#endif

static bool g_logWallClockSeen = false;

static void logPrintPrefix(Print* out, int logLevel) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  char tsBuf[96];
  if (tv.tv_sec > LOG_NTP_MIN_EPOCH) {
    struct tm t;
    gmtime_r(&tv.tv_sec, &t);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (long)(tv.tv_usec / 1000));
    if (!g_logWallClockSeen) {
      g_logWallClockSeen = true;
      out->print('[');
      out->print(tsBuf);
      out->print("] [N] Wall clock acquired\r\n");
    }
  } else {
    snprintf(tsBuf, sizeof(tsBuf), "t+%lums", (unsigned long)millis());
  }
  const char* lvl;
  switch (logLevel) {
    case 0: lvl = "S"; break;  // silent (never prefixed)
    case 1: lvl = "F"; break;  // fatal
    case 2: lvl = "E"; break;  // error
    case 3: lvl = "W"; break;  // warning
    case 4: lvl = "N"; break;  // notice
    case 5: lvl = "T"; break;  // trace
    case 6: lvl = "V"; break;  // verbose
    default: lvl = "?"; break;
  }
  out->print('[');
  out->print(tsBuf);
  out->print("] [");
  out->print(lvl);
  out->print("] ");
}

static void logPrintSuffix(Print* out, int /*logLevel*/) {
  out->print("\r\n");
}

static void logInit(unsigned long baud = 115200) {
  if (!Serial) Serial.begin(baud);
  Log.begin(LOG_LEVEL_VERBOSE, &Serial, false);
  Log.setPrefix(logPrintPrefix);
  Log.setSuffix(logPrintSuffix);
}

// ── Convenience macros for common call-site shapes ──────────────────────────
#define LOG_FN()         Log.verbose(">> %s", __func__)
#define LOG_SD_READ(p,sz,ms) Log.notice("SD READ: %s size=%u took=%ums", (p), (unsigned)(sz), (unsigned)(ms))
#define LOG_SD_WRITE(p)  Log.notice("SD WRITE: %s", (p))
#define LOG_SD_MISS(p)   Log.warning("SD MISSING: %s", (p))
#define LOG_SD_OPEN_FAIL(p) Log.error("SD OPEN FAIL: %s", (p))
