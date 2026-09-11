# Dev Loop — code / build / flash / deploy / mock

Canonical reference for working on the Unihiker K10 Wasteland firmware **and**
its `data/` SPA. Every command in this doc has been verified on Windows 11 +
PowerShell + arduino-cli + a K10 attached to a USB-C port.

## For AI coding agents (read first, ≤ 1 min)

- **Compile:** `pwsh .\scripts\build.ps1` (wraps `arduino-cli compile`,
  `UNIHIKER:esp32:k10`, `build.cdc_on_boot=1`).
- **Flash:** `pwsh .\scripts\flash.ps1` (auto-detects COM port; pass `-Port COM3`
  to override; `-Build` chains compile + upload).
- **Push `data/` to a running board (don't curl files by hand):**
  `pwsh .\scripts\sync_data.ps1 192.168.4.72`. Re-runs are incremental —
  unchanged files are skipped via SHA-256 manifest at
  `data/.upload-manifest.json`.
- **Two upload modes coexist; do not add a third toggle:**
  - **Hold Button A at boot** → SD card mounts as USB-MSC (`usb_drive.h`,
    entered from [Esp32HexMapCrawl.ino:834](../Esp32HexMapCrawl.ino:834)).
  - **HTTP `/upload` while running** → the LCD auto-flips to a "FILE UPLOAD"
    screen for ~1.5 s after the last chunk. Module: [ui-upload.hpp](../ui-upload.hpp).
    Handler: [game-server.hpp:472](../game-server.hpp:472).
- **Offline UI work:** `cd mock-server && npm install && npm run dev` →
  serves `data/` on `http://localhost:8765/` and accepts the same `/upload`
  POSTs (drop on disk under `mock-server/uploads/`). Use this before flashing
  whenever the change is in `data/*.{html,js,css}`.
- **Pointers:** `/upload` handler [game-server.hpp:472](../game-server.hpp:472),
  USB-MSC entry [Esp32HexMapCrawl.ino:834](../Esp32HexMapCrawl.ino:834),
  upload screen [ui-upload.hpp](../ui-upload.hpp), LCD refresh switch around
  [Esp32HexMapCrawl.ino:915](../Esp32HexMapCrawl.ino:915).

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

### Library pinning (verified 2026-05-08, 20% flash / 30% RAM)

The sketchbook (`Documents\Arduino\libraries\`) overrides arduino-cli's user
libraries, so libraries are pinned by what's installed there:

| Library | Verified version | Notes |
|---|---|---|
| `ESP Async WebServer` (ESP32Async fork) | `2.10.8` | `lacamera/ESPAsyncWebServer 3.1.0` and `ESP Async WebServer 3.0.6` both break the build — 3.1.0 dropped the `beginResponse(int, contentType, buf, len)` overload, 3.0.6 pulls `esp_private/gdma.h` which collides with the UNIHIKER 0.0.3 SDK. |
| `AsyncTCP` | `1.1.4` | Already in the sketchbook; do not upgrade past `1.1.x`. |
| `ArduinoLog` | `1.1.1` | Required transitively. |
| `LovyanGFX` | `1.1.16` | Newer 1.2.x is fine BUT `Bus_RGB.cpp` and `Panel_RGB.cpp` under `src/lgfx/v1/platforms/esp32s3/` must be renamed `*.cpp.disabled`. They aren't used (K10 is SPI ILI9341) and they `#include <hal/gdma_ll.h>` which collides with the SDK's `esp_private/gdma.h` declarations. |
| `unihiker_k10`, `lv_lib_qrcode`, `TFT_eSPI` | bundled with `UNIHIKER:esp32 0.0.3` | No action. |

If the build fails with `'GDMA_TRIG_PERIPH_*' conflicts with a previous
declaration`, your `LovyanGFX` install has the RGB platform files enabled —
disable them as above.

If the build fails with `no matching function for call to
'AsyncWebServerRequest::beginResponse(int, ..., uint8_t*, size_t)'`, your
`ESP Async WebServer` is too new — drop to `2.10.8`.

We use `beginResponse_P(int, contentType, buf, len)` (the `_P` variant) in
`game-server.hpp` because the non-`_P` raw-buffer overload was removed in
later versions. On ESP32-S3 the `_P` variant just memcpys from RAM — the
buffer doesn't need to be in flash.

## Build

```powershell
.\scripts\build.ps1
```

Expected: "Sketch uses ~20% flash, ~29% RAM" — exit 0. First build pulls the
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
(gitignored). Run the data sync against it to exercise the pipeline without a
board:

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

## Constraints worth remembering

- **`/upload` runs on the AsyncTCP task, not the LCD task.** The upload-screen
  module ([ui-upload.hpp](../ui-upload.hpp)) only writes shared state from the
  handler; the main loop reads it and paints. Don't call canvas helpers from
  the handler — race-prone and stack-hostile.
- **Keep `data/.upload-manifest.json` out of `data/`-shaped iteration.** The
  script's file walk explicitly skips it, but anything else added under
  `data/` will be uploaded to the board.
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
