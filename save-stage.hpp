#pragma once
// ── Staged save files ────────────────────────────────────────────────────────
// saveGame() used to write its files to the SD card with G.mutex held: 270 ms
// median, 338 ms worst, on every dawn and every drop (bots/wroom_swarm,
// 2026-10-23: 332 saves in 15 minutes of five-seat play). Everything else that
// wanted the mutex waited it out or gave up -- broadcastState() skipped those
// ticks (a ~1 s freeze on every phone, every dawn), handlers nacked "busy",
// and the EVT_DOWNED handler missed its take on every death, which left the
// dead seat taken.
//
// So the save functions write into a SaveOut instead of a File: the same
// `if (!f)`, f.write(), f.close() as before, but into a PSRAM buffer per file,
// a few ms of memcpy under the mutex. saveStageFlush() puts the buffers on the
// card once saveGame() has given the mutex back. Only the Saver task (or, if
// it never started, GameLoop -- never both) runs saveGame(), so the buffers
// have one writer. They are kept between saves (~60 KB once the map is in),
// rather than allocated per save, so a save never fails for want of PSRAM
// after the first one worked.
//
// Included from Esp32HexMapCrawl.ino before the gameplay files that save.

struct SaveStage {
  const char* path;     // one of the SAVE_*_F literals
  uint8_t*    buf;      // PSRAM, grown on demand, kept
  size_t      len, cap;
  bool        used;     // staged by this save, not yet on the card
  bool        failed;   // a grow failed: skip the file rather than write it short
};
static SaveStage g_saveStage[12];
static int       g_saveStageN = 0;

class SaveOut {
 public:
  explicit SaveOut(SaveStage* s = nullptr) : s_(s) {}
  explicit operator bool() const { return s_ != nullptr; }
  size_t write(const uint8_t* p, size_t n) {
    if (!s_ || s_->failed) return 0;
    if (s_->len + n > s_->cap) {
      size_t cap = s_->cap ? s_->cap : 1024;
      while (cap < s_->len + n) cap *= 2;
      uint8_t* nb = (uint8_t*)heap_caps_realloc(s_->buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!nb) {
        Log.error("saveStage: PSRAM grow to %u B failed for %s", (unsigned)cap, s_->path);
        s_->failed = true;
        return 0;
      }
      s_->buf = nb; s_->cap = cap;
    }
    memcpy(s_->buf + s_->len, p, n);
    s_->len += n;
    return n;
  }
  void close() {}
 private:
  SaveStage* s_;
};

// FILE_WRITE semantics: the file starts over. append = true carries on after
// whatever this same save already staged for it (map.bin's ground block).
static SaveOut saveOpen(const char* path, bool append = false) {
  SaveStage* s = nullptr;
  for (int i = 0; i < g_saveStageN; i++)
    if (strcmp(g_saveStage[i].path, path) == 0) { s = &g_saveStage[i]; break; }
  if (!s) {
    if (g_saveStageN >= (int)(sizeof(g_saveStage) / sizeof(g_saveStage[0]))) {
      Log.error("saveOpen: no stage left for %s", path);
      return SaveOut();
    }
    s = &g_saveStage[g_saveStageN++];
    *s = SaveStage{path, nullptr, 0, 0, false, false};
  }
  if (!append || !s->used) { s->len = 0; s->failed = false; }
  s->used = true;
  return SaveOut(s);
}

// One staged file onto the card, written beside the old one as "<path>.new"
// and swapped in only once it is all there. Saves used to truncate the file
// and write it in place, so dying mid-save left it empty: on 2026-10-23 a
// board hung (task watchdog) while writing map.bin, booted to a 0-byte file
// and generated a new world over the old one. saveRecoverInterrupted() at
// boot finishes or drops a swap the board died in.
//
// Short writes are recovered the way uploadWriteChunk() (game-server.hpp)
// does: this card goes busy at 16 KB boundaries (12288, 28672, 45056, ...),
// FatFs latches the error on the handle and the file is left cut off there.
// Close, let the card settle, reopen for append and carry on from whatever
// size actually landed -- every byte is still in the stage. The old direct
// writes never checked: one map.bin in about a hundred went to the card as
// 6016 or 2010 bytes of 49035. One write per attempt, not small pieces:
// written 4 KB at a time, two saves in three hit the boundary failure.
static bool saveWriteStaged(const SaveStage& s) {
  String tmp = String(s.path) + ".new";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) { Log.error("SD OPEN FAIL (write): %s", tmp.c_str()); return false; }
  size_t onDisk = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    f.write(s.buf + onDisk, s.len - onDisk);
    f.flush();
    onDisk = f.size();
    if (onDisk == s.len) {
      f.close();
      if (attempt) Log.notice("SD WRITE %s recovered after %d short write(s)", tmp.c_str(), attempt);
      // The swap. Between the two calls neither name holds the file under its
      // own name, which is the case saveRecoverInterrupted() repairs.
      if (SD.exists(s.path) && !SD.remove(s.path)) {
        Log.error("SD remove FAIL: %s (new copy kept as %s)", s.path, tmp.c_str());
        return false;
      }
      if (!SD.rename(tmp, s.path)) {
        Log.error("SD rename FAIL: %s -> %s (boot will finish it)", tmp.c_str(), s.path);
        return false;
      }
      return true;
    }
    Log.warning("SD WRITE short: %s %u of %u B errno=%d (try %d)", tmp.c_str(), (unsigned)onDisk,
                (unsigned)s.len, errno, attempt + 1);
    f.close();
    vTaskDelay(pdMS_TO_TICKS(25 * (attempt + 1)));   // let the card finish its busy cycle
    f = SD.open(tmp, FILE_APPEND);
    if (!f) { Log.error("SD reopen FAIL: %s", tmp.c_str()); return false; }
    onDisk = f.size();
    if (onDisk > s.len) { Log.error("SD resume mismatch %s disk=%u > %u", tmp.c_str(), (unsigned)onDisk, (unsigned)s.len); break; }
  }
  f.close();
  return false;   // the old file is untouched; the half-written .new is dropped at the next boot
}

// Boot, before tryLoadSave(): a "<name>.new" in SAVE_DIR is a save the board
// died in. Next to its old file it may be unfinished, so it goes and the old
// file stands; alone, the board died between the remove and the rename in
// saveWriteStaged(), and it is the newest complete copy.
static void saveRecoverInterrupted() {
  File dir = SD.open(SAVE_DIR);
  if (!dir) return;
  String pending[16];
  int n = 0;
  for (File f = dir.openNextFile(); f && n < 16; f = dir.openNextFile()) {
    String name = f.name();
    if (!f.isDirectory() && name.endsWith(".new")) pending[n++] = String(SAVE_DIR) + "/" + name;
    f.close();
  }
  dir.close();
  for (int i = 0; i < n; i++) {
    String base = pending[i].substring(0, pending[i].length() - 4);
    if (SD.exists(base)) {
      SD.remove(pending[i]);
      Log.warning("save: dropped %s, unfinished when the board stopped; %s stands", pending[i].c_str(), base.c_str());
    } else if (SD.rename(pending[i], base)) {
      Log.warning("save: %s restored from %s (the board stopped mid-swap)", base.c_str(), pending[i].c_str());
    } else {
      Log.error("save: could not restore %s from %s", base.c_str(), pending[i].c_str());
    }
  }
}

// After G.mutex is given back: every file this save staged, onto the card in
// staging order. Returns the number of files that did not make it.
static int saveStageFlush() {
  int failed = 0;
  for (int i = 0; i < g_saveStageN; i++) {
    SaveStage& s = g_saveStage[i];
    if (!s.used) continue;
    s.used = false;
    if (s.failed || !saveWriteStaged(s)) failed++;   // a failed stage was logged by SaveOut::write
  }
  return failed;
}
