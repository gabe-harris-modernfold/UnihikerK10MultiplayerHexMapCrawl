# Dev Loop — code / build / flash / deploy / mock

Canonical reference for working on the Unihiker K10 Wasteland firmware **and**
its `data/` SPA. Every command in this doc has been verified on Windows 11 +
PowerShell + arduino-cli + a K10 attached to a USB-C port.

## For AI coding agents (read first, ≤ 1 min)

- **Compile:** `.\scripts\build.ps1` (wraps `arduino-cli compile`,
  `UNIHIKER:esp32:k10`, `build.cdc_on_boot=1`). Libraries are pinned and
  repo-local (`.arduino\`, installed on first build) — see "Library pinning".
  Don't point `arduino-cli compile` at the repo folder directly: the folder
  name doesn't match the `.ino`, and the script also works around the
  build-speed traps listed under "Build".
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
    entered from [Esp32HexMapCrawl.ino:1261](../Esp32HexMapCrawl.ino:1261)).
    To verify/bulk-sync, hash-diff repo `data/` against the mounted drive
    rather than trusting it's current — see "Verifying / bulk-syncing via
    USB-MSC" below.
  - **HTTP `/upload` while running** → the LCD auto-flips to a "FILE UPLOAD"
    screen for ~1.5 s after the last chunk. Module: [ui-upload.hpp](../ui-upload.hpp).
    Handler: [game-server.hpp:729](../game-server.hpp:729).
- **Wi-Fi is multi-network:** the board remembers the last 8 networks it
  joined ([wifi-store.hpp](../wifi-store.hpp), NVS namespace `wifinets`) and
  rejoins whichever one is in range — carry it to another house and it finds
  that house's network by itself. See "Wi-Fi: known networks and roaming".
- **Offline UI work:** `cd mock-server && npm install && npm run dev` →
  serves `data/` on `http://localhost:8765/` and accepts the same `/upload`
  POSTs (drop on disk under `mock-server/uploads/`). Use this before flashing
  whenever the change is in `data/*.{html,js,css}`.
- **Finding the board:** it answers mDNS as `k10.local` (and DHCP hostname
  `k10`), so the address no longer has to be re-found after every reboot.
  `/state` → `boot.reset` says why it last started (`PANIC` / `*WDT` = it
  crashed), and `boot.crash` carries the task, PC and backtrace from the core
  dump if one is in flash.
- **WS replies for tooling:** any message with `"rid":N` gets exactly one
  `ack` / `nack` back ([network-reply.hpp](../network-reply.hpp)). Adding a
  handler? Every refusal path must call `wsNack(client, "<why>")` or it acks a
  request that did nothing. Codes are listed in
  [bot-testing.md](bot-testing.md) "Replies". Bump `PROTO_VERSION` in the
  `.ino` when a message changes shape.
- **Pointers:** `/upload` handler [game-server.hpp:729](../game-server.hpp:729),
  USB-MSC entry [Esp32HexMapCrawl.ino:1261](../Esp32HexMapCrawl.ino:1261),
  upload screen [ui-upload.hpp](../ui-upload.hpp), LCD refresh switch around
  [Esp32HexMapCrawl.ino:1382](../Esp32HexMapCrawl.ino:1382).

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

### Library pinning (re-verified 2026-09-24, 22% flash / 17% static RAM)

Libraries come **only** from the repo-local, gitignored sketchbook
`.arduino\` — `build.ps1` / `flash.ps1` set `ARDUINO_DIRECTORIES_USER` to it,
so the machine-wide sketchbook (OneDrive `Documents\Arduino`) is never
searched. That sketchbook had drifted to `AsyncTCP 1.1.4` / `ESP Async
WebServer 2.10.8`, which the current code no longer compiles against.
Versions are pinned in [scripts/_arduino-env.ps1](../scripts/_arduino-env.ps1)
(`$PinnedLibs`); `scripts/setup_libs.ps1` installs them, and build.ps1 runs it
automatically when `.arduino\pinned.txt` doesn't match. To bump one: edit
`$PinnedLibs`, build.

| Library | Pinned | Notes |
|---|---|---|
| `ESP Async WebServer` (ESP32Async) | `3.10.3` | Provides `beginResponse(int, contentType, const uint8_t*, len)` (used for every PSRAM-served asset) — `beginResponse_P` still compiles but is deprecated. Adds `Connection: close` to every response; there is no keep-alive, so every asset the browser fetches is a new TCP connection. Per-client WS queue capped at 8 via `build_opt.h` (below). |
| `Async TCP` (ESP32Async) | `3.4.10` | Event queue `CONFIG_ASYNC_TCP_QUEUE_SIZE=64`; when the queue is ≥¾ full it starts *discarding poll events* and throttling — that is where `ERR_CONNECTION_RESET` under a request burst comes from. Priority 10, 16 KB stack. |
| `ArduinoLog` | `1.1.1` | Required transitively. |
| `LovyanGFX` | `1.1.16` | **Not 1.2.x**: 1.2.x adds `src/lgfx/v1/lv_font/font_fmt_txt.c`, whose `lv_font_*_fmt_txt` symbols collide with the core's `liblvgl.a` at link time (`multiple definition of lv_font_get_bitmap_fmt_txt`). `setup_libs.ps1` also renames `Bus_RGB.cpp` / `Panel_RGB.cpp` under `src/lgfx/v1/platforms/esp32s3/` to `*.cpp.disabled` — unused (K10 is SPI ILI9341), and their `<hal/gdma_ll.h>` collides with the SDK's `esp_private/gdma.h`. |
| `unihiker_k10`, `lv_lib_qrcode`, `TFT_eSPI` | bundled with `UNIHIKER:esp32 0.0.3` (arduino-esp32 2.0.x core) | No action. `unihiker_k10.h` drags in TFT_eSPI (~25 KB flash; ~25% of the sketch's preprocessed lines) and LVGL headers, though only `begin()`, the buttons and `rgb` are used. |

If the build fails with `'GDMA_TRIG_PERIPH_*' conflicts with a previous
declaration`, the RGB files weren't disabled — run
`.\scripts\setup_libs.ps1 -Force`.

### Build flags: `build_opt.h`

[build_opt.h](../build_opt.h) (repo root) is the platform's hook for extra
compiler flags: it is passed as `@build_opt.h` to **every** compile —
sketch, libraries and core — so a library's `#ifndef` default can be
overridden consistently. It is a gcc response file: flags only, no comments.

- `-DWS_MAX_QUEUED_MESSAGES=8` — per-client WebSocket send queue (library
  default 32). Each queued `broadcastState` tick is a ~3–4 KB buffer, and on
  this core malloc keeps anything ≤ 4 KB on the **internal** heap
  (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` is 4096 in the linked IDF), so one
  stalled client could pin ~100 KB of internal heap — the HTTP-wedge failure
  mode. With `setCloseClientOnQueueFull(false)` a full queue drops the
  message; broadcasts are superseded 100 ms later anyway.
- Optional `-DLOG_STRIP_VERBOSE` — compiles out every `LOG_VERBOSE` /
  `LOG_FN` call (see [logging.hpp](../logging.hpp)). Off by default because
  this doc and the bots rely on verbose lines such as `gameLoop wm:`.

arduino-cli's dependency tracking can't see `@file` flags, so an edit to
`build_opt.h` would leave library objects built with the old flags;
`build.ps1` hashes the file and passes `--clean` when it changes.

### PSRAM placement (why static RAM must stay low)

Internal DRAM is the scarce resource. Until 2026-09-12 static `.bss` was 64%
(`Global variables use 212864 bytes`), leaving **~46 KB** of internal heap at
boot for the Wi-Fi driver, LWIP and AsyncTCP. A page load dips the heap by
~100 KB transiently (measured: 176 KB idle → 78 KB minimum during the
80-image terrain fetch), so with 46 KB idle the Wi-Fi driver's DMA buffer
allocations failed mid-burst and the whole network stack wedged — no ping,
no HTTP, until power-cycle. That was the "HTTP wedge".

Fix: the large buffers moved to the 8 MB PSRAM, static RAM is now 17%
(58 KB, 2026-09-24) and idle heap ~180 KB. Two helpers in the `.ino`:

- `PSRAM_STATIC(T, name, [dims])` — a function-local static array that lives
  in PSRAM but keeps array semantics (`sizeof(name)`, `name[r][c]`). Used
  for `sendSync`'s 40 KB buffer, `broadcastState`, `generateMap` scratch,
  `spreadFire` `next`, `drawMapScreen` `terr`, `efxNarrative` `surveyed`,
  the WS handlers' reply buffers (`network-msg-*.hpp`), and `drainEvents`'
  6.4 KB `snapshot[]` (which let the GameLoop task stack drop 24 → 18 KB).
- `allocPsramGlobals()` — first call in `setup()`; allocates `G.map`,
  `W_hex`, `pendingEvents`, `itemRegistry`, `imgCache`, `webFiles`,
  `G.players`, `lootTables`, `k10Log`, `g_knownNets`. These are pointers
  now: use `MAP_BYTES` / `W_HEX_BYTES` / `sizeof(T) * N` instead of
  `sizeof()`.

A `PSRAM_STATIC` buffer is one shared copy, so use it only where a single
task touches the buffer (the WS handlers all run on async_tcp; `drainEvents`
only on GameLoop), or where one lock is held across *every* use of it —
including the `client->text()` that copies it out. `sendSync`'s 40 KB `buf`
is the second kind: it runs on async_tcp (`pick`) and GameLoop (regen
resync), so it sends before releasing `G.mutex`. `sendTunnelSync`'s 1.3 KB
`buf` stays on the stack for the same reason — it is reached from both tasks
via `sendSync`.

Heap, not just `.bss`: task stacks and every malloc ≤ 4 KB come out of
internal RAM (bigger blocks go to PSRAM automatically). So a 10 KB `String`
is harmless, while thousands of small reallocs (the old `/enc`
`f.readString()`) or a queue of ~4 KB WS messages are not.

Rule: any new buffer over ~1 KB goes through one of those. Check the build
line — if `Global variables` climbs back toward 30%+, find it with
`xtensa-esp32s3-elf-nm --size-sort -S -r <elf> | grep " [bBdD] "`.

## Build

```powershell
.\scripts\build.ps1
```

Expected: "Sketch uses ~22% flash", "Global variables use ~17%" — exit 0. If static RAM is back above ~30%, something large landed in internal .bss; see "PSRAM placement" above. The first build installs the pinned
libraries and compiles them plus the core (~6 min). After that, an edit to
any firmware file rebuilds in ~20–40 s, and a no-change build takes ~20–30 s
(arduino-cli start-up plus the link).

What `build.ps1` does that a bare `arduino-cli compile .` doesn't — each item
cost real time or correctness before 2026-09-24:

- **Stages the sketch** in `%LOCALAPPDATA%\k10-sketch-stage\Esp32HexMapCrawl\`
  as hard links to the repo-root `*.ino` / `*.h` / `*.hpp` / `*.c` / `*.cpp`
  plus `partitions.csv`. arduino-cli requires folder name == `.ino` name, and
  it copies every `.h/.c/.cpp/.json/.md` *anywhere* under the sketch folder
  into the build dir on every build — with `.arduino\`, `data\` and
  `docs\` that was 136 MB and ~35 s per build. Compiler errors therefore
  show the stage path; since those are hard links, editing that path edits
  the repo file. New root-level source files are picked up automatically.
- **Shortens the SDK path** (`compiler.sdk.path` → junction
  `%LOCALAPPDATA%\k10sdk`). The platform emits ~290 SDK `-I` flags; at the
  full Arduino15 path the sketch command line nears Windows' 32K limit,
  arduino-cli relativizes its paths, its own `.d` check then fails
  ("Depfile is about different object file") and the sketch recompiled on
  every build. Debug with `arduino-cli compile --log --log-level debug`.
- **Pins libraries** and **cleans on a `build_opt.h` change** (both above).

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

## Wi-Fi: known networks and roaming

The ESP32 itself stores exactly **one** STA credential, which is why the board
used to need its password retyped after every move. [wifi-store.hpp](../wifi-store.hpp)
adds a second, larger list on top of that.

| Piece | Where |
|---|---|
| Known-network list (8 max, most-recent first, LRU eviction) | [wifi-store.hpp](../wifi-store.hpp), NVS namespace `wifinets` (`n`, `s0..s7`, `p0..p7`) |
| Boot: join the ESP32's own single credential (fast, no scan) | [game-server.hpp](../game-server.hpp) `setupWiFiAndServer()` |
| Roaming sweep: scan, rank known SSIDs by RSSI, join strongest | [network-session.hpp](../network-session.hpp) `wifiAutoJoinTask` / `wifiStartAutoJoin()` |
| Sweep scheduler (backoff 30s → 60 → 120 → 240 → 300) | `loop()` in [Esp32HexMapCrawl.ino](../Esp32HexMapCrawl.ino), `wifiNextSweepMs` / `wifiSweepBackoff` |
| Settings UI ("REMEMBERED NETWORKS", ✕ = forget) | [data/ui-panels.js](../data/ui-panels.js), state in [data/ui-state.js](../data/ui-state.js) |

Boot order, in one breath: softAP `WASTELAND` comes up first and never drops →
the board tries its last network directly (connected in ~2-4 s at home, no
scan) → if that times out (12 s) the sweep starts, scans, and joins the best
known network that answered → if nothing known is in range it keeps the AP and
retries later, backing off to one sweep per 5 minutes.

A network is remembered **only after a successful join** — from the settings
panel, from the boot credential, or from the sweep itself. So the friend's
house flow is: join their AP `WASTELAND` once, type their SSID + password in
Settings → Wi-Fi Network, connect. Every later visit is automatic.

Protocol (all over `/ws`):

| Message | Direction | Meaning |
|---|---|---|
| `{t:"wifi",ssid,pass}` | client → board | join now, and remember on success |
| `{t:"wifi_forget",ssid}` | client → board | drop from the list; an active link stays up |
| `{t:"wifi",status:"nets",cur,nets:[…]}` | board → all | the full list + the SSID currently joined (SSIDs only, never passwords) |
| `{t:"wifi",status:"forgot",ssid}` | board → all | clients clear that SSID from `localStorage`, else the next reconnect would auto-send it and re-add the network |
| `{t:"wifi",status:"saved"\|"ok"\|"fail"\|"busy"}` | board → client | unchanged from before |

Gotchas:

- **A scan stalls the softAP for a second or two.** That's why sweeps only run
  while the board is off every known network, never during an `/upload`, and
  back off to 5-minute spacing.
- **Forgetting does not disconnect.** It only removes the network from future
  auto-joins, so you can drop a network you're standing in without dropping the
  players on it.
- Serial log lines to grep for: `wifiStore:`, `AutoJoin sweep`, `AutoJoin try`,
  `AutoJoin connected`, `AutoJoin no known network in range`.
- The mock-server fakes the whole list (seeded `WASTELAND-HOME` +
  `friends-house-5G`) so the panel can be exercised offline.

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
curl.exe -s http://k10.local/state | ConvertFrom-Json | Select pv, evSeq, evtDrops, boot   # protocol, event loss, last reset / crash
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

## Art assets (`data/img/`)

Two scripts, both dry-run by default and both needing only Pillow + numpy.
Neither changes a filename, an extension or a route, so no firmware, engine.js
or MIME change is involved.

```bash
python scripts/optimize_art.py                  # dry run: prints the table
python scripts/optimize_art.py --apply          # rewrite in place
python scripts/gen_missing_tiles.py --apply     # fill empty variant slots
```

- **`optimize_art.py`** re-encodes everything under `data/img/` as palette PNG
  with a real alpha ramp (`scripts/png_quant.py`), picking the smallest colour
  count that stays inside `--quality` (visible RMSE, measured after
  compositing over the map background — the naive RGB metric scores a visually
  perfect requantise at 87 because it is reading the transparent corners).
  It also caps hex tiles at `--max-edge 256`: tiles are drawn at
  `imgSz = HEX_SZ * 2` and `HEX_SZ` tops out near 125, so 250 CSS px is the
  widest one is ever painted. It only ever downscales, only rewrites a file
  that actually got smaller, and only resizes `hex*` / `poi_*`.
  **2360 KB → 679 KB (71%) on the first pass, no visible change.**
  Already-indexed (mode `P`) files are skipped so the pass is idempotent:
  re-quantising a quantised image scores its error against the *degraded*
  version and will shave another 10% every time you run it, which is visible
  banding after a few passes. `--force` if you really mean it.
- **`gen_missing_tiles.py`** fills empty `hex<Name><N>.png` slots with flat
  labelled placeholders — terrain name, variant number and the target filename
  printed on the tile, ~4.3 KB each. It only appends at the next free index,
  so it cannot open a gap in the numbering that `setupVariantCounts()` would
  silently turn into a wasted slot. Skips River Channel (drawn with the
  animated `drawRiverRipples()` on purpose) and the tunnel terrains unless
  asked. `--regen` redraws its own earlier output (tagged in a PNG tEXt chunk,
  so hand-painted art is never touched); `--no-guide` drops the square overlay.
- **Tile art belongs on a SQUARE canvas.** `renderHexContent()` draws into
  `imgSz = HEX_SZ * 2` on *both* axes, so a non-square source is stretched, not
  letterboxed — while the hex `drawHexPath()` strokes is only `√3 × HEX_SZ`
  tall. Correct authoring is a square canvas with the hexagon at full width and
  the middle 86.6% of the height. The placeholders are built that way and draw
  the box as a dashed square with corner brackets. The existing painted tiles
  are *not*: their hexagon fills the canvas, so it renders 7–15% too tall and
  overhangs its neighbours. It reads as a slight overlap rather than a fault,
  so this is a note for new art, not a bug to go fix.
- **Don't re-encode `data/img/survivors/*.jpg`.** They are already near the
  knee: q85 saves 13% for a visible generation loss, and the portrait panel is
  300 CSS px with `background-size:cover`, so 280×420 is already short of what
  a 2× display wants. The only real win there is WebP, which costs the
  `.png`/`.jpg` literals in engine.js, the suffix test in
  `setupVariantCounts()` and the MIME literal in `game-server.hpp`.
- `img/ui_glyphs.png` is skipped by the optimiser: `_glyphTile()` recolours it
  with `source-in`, so only its alpha reaches the screen, and snapping that to
  6 levels would chew the antialiasing to save under a kilobyte.
- Prompts for the real hand-painted art: [HEX_TILE_PROMPTS.md](../data/img/HEX_TILE_PROMPTS.md)
  and [items/ICON_PROMPTS.md](../data/img/items/ICON_PROMPTS.md).

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
client's copy of the JSON is for display only.

The one client→server payload with gameplay meaning is `enc_bank`'s optional
`keep:[w,f,fu,m,s]` — how much of each resource the player left on the haul
tray's +/− steppers. It is a *request*: `handleMsg_enc_bank` banks
`min(pendingLoot[i], keep[i])`, so a drifted or hostile client can only ever
take **less** than it won. An absent `keep` means "bank everything", which is
what an older client sends. Trimmed tokens are left behind and gone — there is
no ground drop and no score refund, unlike `drop_res`. Score follows the
trimmed total (3 pts/token), and the `enc_bank` event reports the *banked*
amounts, not the rolled ones, so the client's `inv[]` stays in sync.

Two test-only
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
send({ t: 'dbg_flood' });                         // force-flood the sender's hex at intensity 2, ignoring the water/storm gates
send({ t: 'dbg_tunnel', h: 2 });                  // stand the sender on bunker hatch #h and refill MP (tunnel-system-spec.md)
send({ t: 'dbg_tunnel', h: 2, below: 1, mp: 0 }); // ...underground on that shaft instead, with a pinned MP budget
send({ t: 'dbg_collapse', d: 0 });                // cave in the tunnel hex in direction d from the sender
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
- **The mock applies `WEATHER_VIS_PENALTY` to the surface vis radius**
  (`surfaceVis()` in `mock-server/server.js`), so storm/chem/strangle-fog
  blind you there the same way they do on the board. It used to send a flat
  `vr: 4`, which hid every bug in anything gated on fog of war. Like the
  firmware, the radius is only resampled when a vis disk is sent (on move) —
  a phase change while you stand still does not take effect until you step.
  Terrain/Scout/equipment vision modifiers are still not modelled.
- **Don't `git add mock-server/uploads/` or `node_modules/`.** Both are
  ignored at the repo root.
- **Every file under `data/img/` (one subdir deep) is loaded into the PSRAM
  image cache at boot, capped at `MAX_IMG_CACHE = 160` (currently 114 used,
  1116 KB).** Anything past the cap is silently skipped and served as 204.
  That count is *files*, not images — the three `*_PROMPTS.md` / `IMAGES_NEEDED.md`
  notes under `data/img/` burn three slots and ~29 KB of PSRAM for text the
  board never serves. Pixel glyphs
  (item fallback icons, `img/ui_glyphs.png` sprite strip) are hand-drawn ASCII
  in `scripts/gen_pixel_glyphs.py` — edit the grids there and re-run it rather
  than adding one PNG per glyph.
- **`sw.js` caches `/img/*` cache-first forever.** If you change an image
  in place (same filename), bump the `CACHE` name in `data/sw.js` or clients
  keep the old bytes.
