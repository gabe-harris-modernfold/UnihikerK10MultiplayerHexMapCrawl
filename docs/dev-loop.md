# Dev Loop — code / build / flash / deploy / mock

Canonical reference for working on the Unihiker K10 Wasteland firmware **and**
its `data/` SPA. Every command in this doc has been verified on Windows 11 +
PowerShell + arduino-cli + a K10 attached to a USB-C port.

## For AI coding agents (read first, ≤ 1 min)

- **Compile:** `.\scripts\build.ps1` (wraps `arduino-cli compile`,
  `UNIHIKER:esp32:k10`, `build.cdc_on_boot=1`).
- **Flash:** `.\scripts\flash.ps1` (auto-detects COM port; pass `-Port COM3`
  to override; `-Build` chains compile + upload).
- **Push `data/` to a running board (don't curl files by hand):**
  `.\scripts\sync_data.ps1 192.168.4.72`. Re-runs are incremental —
  unchanged files are skipped via SHA-256 manifest at
  `data/.upload-manifest.json`. It runs `scripts/build_web.ps1` first
  (bundle + gzip + `assets.json`); `-NoBuild` skips that.
- **Adding a client `.js`/`.css` file:** drop it in `data/`, add it to
  `data/web-assets.json` (that list is the load order), sync. **No firmware
  change** — the K10 auto-discovers every file in the SD `/data` root at boot
  and the browser loader reads the manifest. See "Web asset pipeline" below.
- **Two upload modes coexist; do not add a third toggle:**
  - **Hold Button A at boot** → SD card mounts as USB-MSC (`usb_drive.h`,
    entered from [Esp32HexMapCrawl.ino:927](../Esp32HexMapCrawl.ino:927)).
    To verify/bulk-sync, hash-diff repo `data/` against the mounted drive
    rather than trusting it's current — see "Verifying / bulk-syncing via
    USB-MSC" below.
  - **HTTP `/upload` while running** → the LCD auto-flips to a "FILE UPLOAD"
    screen for ~1.5 s after the last chunk. Module: [ui-upload.hpp](../ui-upload.hpp).
    Handler: [game-server.hpp:628](../game-server.hpp:628).
- **Offline UI work:** `cd mock-server && npm install && npm run dev` →
  serves `data/` on `http://localhost:8765/` and accepts the same `/upload`
  POSTs (drop on disk under `mock-server/uploads/`). Use this before flashing
  whenever the change is in `data/*.{html,js,css}`.
- **Pointers:** `/upload` handler [game-server.hpp:628](../game-server.hpp:628),
  USB-MSC entry [Esp32HexMapCrawl.ino:927](../Esp32HexMapCrawl.ino:927),
  upload screen [ui-upload.hpp](../ui-upload.hpp), LCD refresh switch around
  [Esp32HexMapCrawl.ino:1010](../Esp32HexMapCrawl.ino:1010).

---

## Toolchain

- **Editor:** any (VS Code is fine).
- **Compiler/uploader:** Arduino CLI at
  `C:\Program Files\Arduino CLI\arduino-cli.exe`. Override with
  `scripts\build.ps1 -Cli <path>` if installed elsewhere.
- **Board package:** `UNIHIKER:esp32` (install via `arduino-cli core install`
  or the Arduino IDE Boards Manager).
- **FQBN:** `UNIHIKER:esp32:k10`.
- **Required build flag:** `build.cdc_on_boot=1` (already set by `build.ps1`).

### Library pinning (re-verified 2026-09-12, 21% flash / 63% static RAM)

The sketchbook (`arduino-cli config get directories.user` →
`...\Documents\Arduino\libraries\`, on this machine under OneDrive) is where
libraries actually resolve from. **Check with `arduino-cli lib list` rather
than trusting this table** — an earlier revision of it listed `AsyncTCP 1.1.4`
/ `ESP Async WebServer 2.10.8` while the build had been using the 3.x pair
for some time.

| Library | Verified version | Notes |
|---|---|---|
| `ESP Async WebServer` (ESP32Async) | `3.10.3` | Provides `beginResponse(int, contentType, const uint8_t*, len)` (used for every PSRAM-served asset) — `beginResponse_P` still compiles but is deprecated. Adds `Connection: close` to every response; there is no keep-alive, so every asset the browser fetches is a new TCP connection. |
| `Async TCP` (ESP32Async) | `3.4.10` | Event queue `CONFIG_ASYNC_TCP_QUEUE_SIZE=64`; when the queue is ≥¾ full it starts *discarding poll events* and throttling — that is where `ERR_CONNECTION_RESET` under a request burst comes from. Priority 10, 16 KB stack. |
| `ArduinoLog` | `1.1.1` | Required transitively. |
| `LovyanGFX` | `1.2.20` | `Bus_RGB.cpp` and `Panel_RGB.cpp` under `src/lgfx/v1/platforms/esp32s3/` must be renamed `*.cpp.disabled`. They aren't used (K10 is SPI ILI9341) and they `#include <hal/gdma_ll.h>` which collides with the SDK's `esp_private/gdma.h` declarations. |
| `unihiker_k10`, `lv_lib_qrcode`, `TFT_eSPI` | bundled with `UNIHIKER:esp32 0.0.3` (arduino-esp32 2.0.x core) | No action. |

If the build fails with `'GDMA_TRIG_PERIPH_*' conflicts with a previous
declaration`, your `LovyanGFX` install has the RGB platform files enabled —
disable them as above.

### PSRAM placement (why static RAM must stay low)

Internal DRAM is the scarce resource. Until 2026-09-12 static `.bss` was 64%
(`Global variables use 212864 bytes`), leaving **~46 KB** of internal heap at
boot for the Wi-Fi driver, LWIP and AsyncTCP. A page load dips the heap by
~100 KB transiently (measured: 176 KB idle → 78 KB minimum during the
80-image terrain fetch), so with 46 KB idle the Wi-Fi driver's DMA buffer
allocations failed mid-burst and the whole network stack wedged — no ping,
no HTTP, until power-cycle. That was the "HTTP wedge".

Fix: the large buffers moved to the 8 MB PSRAM, static RAM is now 21%
(70 KB) and idle heap ~180 KB. Two helpers in the `.ino`:

- `PSRAM_STATIC(T, name, [dims])` — a function-local static array that lives
  in PSRAM but keeps array semantics (`sizeof(name)`, `name[r][c]`). Used
  for `sendSync`'s 40 KB buffer, `broadcastState`, `generateMap` scratch,
  `spreadFire` `next`, `drawMapScreen` `terr`, `efxNarrative` `surveyed`.
- `allocPsramGlobals()` — first call in `setup()`; allocates `G.map`,
  `W_hex`, `pendingEvents`, `itemRegistry`, `imgCache`, `webFiles`. These
  are pointers now: use `MAP_BYTES` / `W_HEX_BYTES` instead of `sizeof()`.

Rule: any new buffer over ~1 KB goes through one of those. Check the build
line — if `Global variables` climbs back toward 30%+, find it with
`xtensa-esp32s3-elf-nm --size-sort -S -r <elf> | grep " [bBdD] "`.

## Build

```powershell
.\scripts\build.ps1
```

Expected: "Sketch uses ~21% flash", "Global variables use ~21%" — exit 0. If static RAM is back above ~30%, something large landed in internal .bss; see "PSRAM placement" below. First build pulls the
ESP32 toolchain (~5 min); subsequent builds are 30–60 s.

## Flash

```powershell
.\scripts\flash.ps1                # auto-detect port from `arduino-cli board list`
.\scripts\flash.ps1 -Port COM3
.\scripts\flash.ps1 -Build         # compile + upload in one shot
```

If `board list` shows nothing, the previous firmware crashed before USB-CDC
came up. Hold the K10 reset/boot button briefly while replugging USB to recover.

## Verify on the LCD

| Screen | Means |
|---|---|
| Black / nothing | Crash before `M5.begin()` or `k10.begin()` returned |
| WASTELAND splash → "SD %dMB/%dMB used" | Boot OK; if held, **A** triggers USB-MSC next |
| `USB DRIVE` (teal) | Hold-A path; SD is enumerated as a removable drive on the host |
| Player/Resources/Map screens (cycle with **B**) | Normal gameplay |
| `FILE UPLOAD` (teal banner, scrolling bar, byte counter) | A `/upload` POST is in progress; auto-clears 1.5 s after last byte |
| `OK` (green) / `FAIL` (red) on the upload screen | Final-chunk flash from [ui-upload.hpp](../ui-upload.hpp) |

The "FILE UPLOAD" screen takes precedence over the gameplay screens — it
suppresses the screen rotation while uploads are streaming so a partial sync
is unmistakable.

## Deploy `data/` to a running board

Preferred path — no SD-eject, no reboot:

```powershell
.\scripts\sync_data.ps1 192.168.4.72            # full sync (incremental)
.\scripts\sync_data.ps1 192.168.4.72 -DryRun    # show what would upload
.\scripts\sync_data.ps1 192.168.4.72 -Force     # ignore manifest, push everything
.\scripts\sync_data.ps1 localhost:8765          # against the mock-server
```

The script:

- Runs `scripts/build_web.ps1` first (skip with `-NoBuild`) so
  `app.bundle.js(.gz)`, `style.css.gz`, `index.html.gz` and `assets.json`
  are current before anything is pushed.
- Walks `data/` recursively.
- Hashes each file (SHA-256).
- POSTs only files whose hash differs from the per-host record in
  `data/.upload-manifest.json`.
- Sleeps 500 ms between POSTs (matches the Wi-Fi RX-buffer guidance from the
  original `build-run-test.txt`).

After the last file is acknowledged, the K10's "FILE UPLOAD" screen returns
to the player dashboard. **Power-cycle the board** to reload the new files
into PSRAM (the firmware caches `data/` at boot via `loadWebFilesToRAM()`).

### USB drive mode vs. live `/upload`

| Use this | When |
|---|---|
| **Hold Button A at boot** (USB-MSC) | First-time provisioning, no Wi-Fi creds yet, big bulk image refresh, board not on your LAN |
| **`scripts/sync_data.ps1`** | Iterating on HTML/CSS/JS in `data/`, board already on Wi-Fi, you want incremental and visible progress on the LCD |

Don't add a third "upload mode" toggle. The two paths coexist intentionally:
USB-MSC needs the SD bus exclusive (game halts), `/upload` runs while the
game keeps serving websockets.

### Verifying / bulk-syncing via USB-MSC

Once mounted (hold **A** at boot), the SD card shows up as a normal FAT32
drive (label `UNIHIKER`) — treat `<drive>:\data\` as the on-device mirror of
repo `data/` and hash-diff before copying, the same skip-on-hash logic
`sync_data.ps1` uses, just local instead of HTTP:

```powershell
Get-Volume | Where-Object FileSystemLabel -eq 'UNIHIKER'   # find the drive letter
# per file under data/: Get-FileHash repo copy vs. <drive>:\data\<rel>,
# copy only if missing or the hash differs. Leave the SD root's save/ and
# assets/ folders alone -- only data/ is this doc's territory.
```

A card that's been in the field a while can be surprisingly stale — a
2026-09-12 check found 134/221 `data/` files missing or outdated (pre-skill-
migration encounter JSON, several newer `.js` modules and item icons never
pushed). Don't assume a spot-check of a few files means the rest are current;
diff the whole tree.

**Ejecting the volume programmatically is unreliable.** Both the Shell COM
`InvokeVerb("Eject")` and `Dismount-Volume` (Storage module) routinely no-op
or aren't available in a non-interactive/agent session, and `Get-Volume` can
still show the drive mounted afterward. That's fine to proceed past: Windows
defaults removable USB drives to "Quick Removal" (write-through, no caching),
so file copies are already physically committed once `Copy-Item` returns —
it's safe to reboot the K10 without a confirmed eject.

## Web asset pipeline

How the browser gets the game from the K10, end to end. Designed around the
board's one real limit: **every request is a fresh TCP connection through
AsyncTCP on a single ESP32-S3**, and a burst of them (a browser's default 6
parallel sockets, ~18 `<script src>` tags, then ~110 `new Image()` after the
first sync) is what resets connections and wedges the server.

```
data/web-assets.json        hand-maintained: load-ordered list of scripts + styles
        │
        ▼  scripts/build_web.ps1  (or build_web.sh; sync_data runs it for you)
data/app.bundle.js(.gz)     all scripts concatenated, in order      ┐ generated,
data/style.css.gz  index.html.gz                                     │ gitignored,
data/assets.json            { version, styles:[{url,size,gz}],       │ live in data/ so
                              scripts:[{url:"app.bundle.js",…}] }    ┘ sync + USB-MSC copy them
        │
        ▼  boot: loadWebFilesToRAM()  (boot-assets.hpp)
K10 scans SD /data ROOT → PSRAM → one HTTP route per file ("/<name>", index.html also "/")
   • "<name>.gz" beats "<name>": served with Content-Encoding: gzip
   • ETag = FNV-1a of the bytes (stable across reboots → real 304s)
   • Cache-Control: no-cache for "/", sw.js, assets.json, web-assets.json;
     immutable for everything else (client appends ?v=<version>)
   • MAX_WEB_FILES = 48; unknown extensions (items.cfg, dotfiles) are skipped
        │
        ▼  index.html → window.AssetLoader (inline, runs before anything else)
GET assets.json → styles → scripts, in manifest order, through ONE queue:
   • MAX_CONCURRENT = 2 sockets, 20 s per attempt, 6 attempts,
     backoff 0.5→1→2→4→8 s + jitter; 404/204 fail fast (no hammering)
   • boot screen shows file n/m, the current retry, and a RETRY button
   • assets.json missing → falls back to web-assets.json (one file each)
   • engine.js createImageWithLoadTracking() → AssetLoader.image() so terrain /
     shelter / pawn art goes through the same queue (blob → img.src;
     img.dataset.src keeps the path). The settings "Asset viewer" too.
   • fires document event 'assets:ready'; sw.js is registered after that
```

Adding a client file: put it in `data/`, add it to `web-assets.json` at the
right position, sync. Nothing else. Removing one: the reverse. The old
hardcoded `WEB_FILES[]` table in the `.ino` is gone — that table is why
`world-entities.js` and the `*-field.js` files 404'd for a while.

Numbers as of 2026-09-12: 18 scripts → one 314 KB bundle → **92 KB gzipped**;
style.css 74 KB → 15 KB; index.html 35 KB → 11 KB. Cold page load is 5
requests (`/`, `assets.json`, `style.css`, `app.bundle.js`, `sw.js`) instead of
~22, and ~120 KB on the wire instead of ~460 KB.

Verifying on hardware (board on the LAN):

```powershell
curl.exe -sI -H "Accept-Encoding: gzip" http://192.168.4.72/app.bundle.js   # expect Content-Encoding: gzip + ETag
curl.exe -s http://192.168.4.72/assets.json                                   # version must match data/assets.json
curl.exe -s http://192.168.4.72/state | ConvertFrom-Json | Select -Expand mem  # heap / minHeap / maxBlock / psram
```

### Diagnosing HTTP stalls

If the game page stops loading part-way and the board stops answering:

1. Open the page once more and watch the boot screen — it names the file and
   the retry count, so you can tell "slow" from "dead".
2. Poll `/state` → `mem.heap` / `mem.minHeap` / `mem.maxBlock` between loads.
   A `minHeap` that only ever goes down across page loads is a leak; a
   `maxBlock` far below `heap` is fragmentation. Both starve LWIP.
3. On serial, every `HTTP GET …` line now ends in `heap=NNNKB`, and the
   `gameLoop wm:` line every 5 s has `heap=` and `psram=`.
4. The 2026-09-12 wedge (reproduced 5×, needed a power cycle each time) was
   **internal heap starvation**, not a leak and not connection count — it
   reproduced with the loader capped at 2 connections. See "PSRAM placement"
   above for the numbers and the fix. If it recurs, the first thing to check
   is `Global variables use …` in the build output and `mem.minHeap` here.
5. `mem.uploadResumes` / `mem.lastUploadErr` cover the other failure class:
   SD write errors during `/upload` (see below).

### `/upload` gotchas (fixed 2026-09-12, kept here because they were silent)

- **Raw-body POSTs never reached the SD before.** ESPAsyncWebServer routes a
  non-multipart body to the *body* callback, and only the *upload* (multipart)
  callback was registered. Every `sync_data` run got `OK` while writing
  nothing — which is why the card was found 134 files stale earlier that day.
  Both callbacks now feed `uploadChunk()` in `game-server.hpp`.
- **Short SD writes.** `f_write` intermittently fails with `EIO` at offsets
  16 KB apart (card busy at a physical block boundary); FatFs latches the
  error on the handle and the file is left truncated at a cluster boundary.
  The writer now flushes and confirms the on-disk size after every chunk and,
  on failure, reopens for append and rewrites the remainder. The reply is
  `OK <bytes>`; `sync_data.ps1` compares that with the local size, retries
  (`-Attempts`, default 4) and only records verified files in the manifest.
- After a sync, verify by size rather than trusting the manifest — the
  one-liner in "Verifying on hardware" above, or GET every file and compare
  `Content-Length` with the local file (all 120 web+image files were checked
  that way).

## Mock-server dev loop (no board needed)

```bash
cd mock-server
npm install      # first time — installs `ws`
npm run dev
# open http://localhost:8765/
```

The mock serves `data/` statically (with `Cache-Control: no-store`, so a plain
reload picks up edits), fakes `/ws` (lobby + map + player movement +
encounters), serves `GET /enc?biome=X&id=Y` from `data/encounters/`, and
accepts `POST /upload?dest=/data/...` writing to `mock-server/uploads/<dest>`
(gitignored).

`GET /assets.json` is **synthesised** from `data/web-assets.json` in dev mode,
so the browser loads every source file individually (real filenames and line
numbers in devtools, no build step). To exercise the exact bundle path the
K10 serves, run the built manifest instead:

```bash
node mock-server/server.js --bundle --port=8766     # or MOCK_BUNDLE=1 npm run dev
```

(`.claude/launch.json` has both as "Mock Server" and "Mock Server (bundle)".)
Run `scripts/build_web.ps1` first or `--bundle` mode 404s the manifest and the
loader falls back to per-file loading. Run the data sync against the mock to
exercise the upload pipeline without a board:

```powershell
.\scripts\sync_data.ps1 localhost:8765
```

### Encounter dialog dev loop

Stepping onto a POI hex (the eye icon) in the mock triggers the real
`enc_start → enc_path → GET /enc → enc_choice → enc_res → enc_bank/enc_end`
sequence; the roll mirrors `computeEncounterDN()` + 2d6. The server is
authoritative: `enc_choice` carries only `{ci: <choice index>}` and both the
firmware ([encounter_engine.hpp](../encounter_engine.hpp)) and the mock read
costs, hazards, loot and `can_bank` from the encounter JSON themselves. The
client's copy of the JSON is for display only. Two test-only
WebSocket messages exist in the mock (the firmware ignores them) — send them
from the browser console:

```js
send({ t: 'dbg_enc',   biome: 'urban', id: 3 });  // open a specific encounter JSON, ignoring position/POI
send({ t: 'dbg_force', out: 0 });                 // force the NEXT roll to fail (out: 1 = succeed)
send({ t: 'dbg_weather', phase: 2 });             // force weatherPhase (0 clear, 1 rain, 2 storm, 3 chem, 4 strangle fog, 5 mist/fog)
send({ t: 'dbg_quake' });                         // force an earthquake near the sender, ignoring the cooldown
send({ t: 'dbg_caravan' });                       // teleport the mock caravan onto the sender's hex (world-system-spec.md)
send({ t: 'dbg_ignite' });                        // ignite the sender's own hex at intensity 2 (world-system-spec.md)
send({ t: 'dbg_doom', awareness: 100 });          // teleport Creeping Doom adjacent to the sender, set its awareness (world-system-spec.md)
send({ t: 'dbg_settle' });                        // force a settlement to form on the sender's hex (actions_game_loop.hpp doShelter())
```

Encounter JSON `skill` ids use the firmware's 5-skill enum — 0 NAVIGATE,
1 FORAGE, 2 SCAVENGE, 3 SHELTER, 4 ENDURE. (The files were originally
authored against a 6-skill enum that included Treat; they were migrated in
Sept 2026. Don't reintroduce id 5.)

Client module: [data/ui-encounter.js](../data/ui-encounter.js). Markup lives in
`index.html` under `#enc-overlay`; styles under "Encounter overlay" in
`style.css`.

## Troubleshooting

- **`SD FAIL - insert card!` on boot:** card is missing or unreadable. Format
  FAT32, copy `data/` to the root, retry.
- **`/upload` returns OK but nothing changed on the device:** firmware caches
  `data/` into PSRAM at boot. Reboot to pick up new files.
- **"FILE UPLOAD" screen stays up forever:** the handler never saw a final
  chunk (client disconnect mid-stream). It auto-clears after 1.5 s of no
  activity; if not, reboot — that's a stuck upload state worth filing.
- **`.upload-manifest.json` says everything's current but the board is empty:**
  the K10 was reflashed/erased and the SD wasn't rebuilt. Run
  `sync_data.ps1 -Force` to ignore the manifest.
- **Boot screen says "No built manifest on the board — loading source files
  one by one":** `data/assets.json` isn't on the SD (bulk-copied without
  running `scripts/build_web.ps1`, or `MAX_WEB_FILES` overflowed — check
  serial for `WEB cache FULL`). The game still loads, just slower.
- **Boot screen stuck at "Contacting the K10…" or showing retries:** the
  board is not answering HTTP at all — see "Diagnosing HTTP stalls".
- **Browser runs old JS after a sync + reboot:** the loader appends
  `?v=<assets.json version>`; if the version didn't change, the bundle didn't
  change — rebuild (`build_web.ps1`) and re-sync. Hard-reload only if you
  edited `index.html` itself (it's `no-cache`, so a normal reload suffices).

## Constraints worth remembering

- **`/upload` runs on the AsyncTCP task, not the LCD task.** The upload-screen
  module ([ui-upload.hpp](../ui-upload.hpp)) only writes shared state from the
  handler; the main loop reads it and paints. Don't call canvas helpers from
  the handler — race-prone and stack-hostile.
- **Keep `data/.upload-manifest.json` out of `data/`-shaped iteration.** The
  script's file walk explicitly skips it, but anything else added under
  `data/` will be uploaded to the board.
- **Only the `/data` ROOT is auto-served.** Subdirectories are not routes:
  `img/` has its own PSRAM cache + `/img/*` handler, `encounters/` is read on
  demand via `/enc`. A new *directory* of web assets needs a firmware route;
  a new *file* in the root does not.
- **Never fire unbounded parallel requests at the board from the client.**
  Use `AssetLoader.fetch()` / `AssetLoader.image()` (index.html), which cap
  concurrency at 2 and retry. Plain `fetch()` / `new Image()` bursts are what
  wedged the HTTP server.
- **The mock-server is wire-compatible, not behaviour-compatible.** It will
  ack any `/upload` POST, but encounter logic / save persistence is faked.
  Validate game logic against real hardware.
- **Don't `git add mock-server/uploads/` or `node_modules/`.** Both are
  ignored at the repo root.
- **Every file under `data/img/` (one subdir deep) is loaded into the PSRAM
  image cache at boot, capped at `MAX_IMG_CACHE = 100` (currently 95 used).**
  Anything past the cap is silently skipped and served as 204. Pixel glyphs
  (item fallback icons, `img/ui_glyphs.png` sprite strip) are hand-drawn ASCII
  in `scripts/gen_pixel_glyphs.py` — edit the grids there and re-run it rather
  than adding one PNG per glyph.
- **`sw.js` caches `/img/*` cache-first forever.** If you change an image
  in place (same filename), bump the `CACHE` name in `data/sw.js` or clients
  keep the old bytes.
