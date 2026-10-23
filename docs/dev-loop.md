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
- **Exception — `data/observer*` is NOT in `web-assets.json` and must not
  be.** The observer screen is a standalone page with its own `<script>` tags
  ([docs/observer-screen-spec.md](observer-screen-spec.md)); `web-assets.json`
  is the *game client's* bundle order, so listing it there would load a
  spectator screen into every player's browser. It still deploys, because
  `sync_data.ps1` walks `data/` recursively and the board registers a route
  per discovered file. See "Observer screen" below.
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
- **Sound preview (no flash):** `python scripts/sndsim/sndsim.py [scene]`
  compiles the real `snd-*.hpp` engine with MSVC and writes WAVs (plus a
  K10-speaker simulation, spectrograms, piano rolls) to
  `scripts/sndsim/out/`. Speech vocabulary: `python scripts/lpc/gen_vocab.py`.
  Design, the story→sound map and tuning knobs:
  [sound-engine.md](sound-engine.md).
- **Sound tuning on the board (no flash):** `http://<board>/sound.html`, the
  sound desk ([data/sound.html](../data/sound.html)): every live knob, every
  effect / line / music style / story beat as a button, scripted listening
  tests, and SAVE TO BOARD (NVS, survives reboots). Standalone like the
  observer — **not** in `web-assets.json`. Routes `/sndinfo`, `/snddbg`,
  `/sndplay` in game-server.hpp; the mock-server fakes all three.
- **LCD FX preview (no flash):** `python scripts/fxsim/fxsim.py [scene]`
  compiles the real `ui-fx.hpp` with MSVC and writes GIFs to
  `scripts/fxsim/out/`. See "Previewing the LCD FX on the desktop" below.
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
| `ESP Async WebServer` (ESP32Async) | `3.12.1` | **Not below 3.12.0**: 3.10/3.11 tear the WebSocket stream under send-buffer pressure (upstream issue #453, fixed by PR #462). A failed `add()`/`send()` left a frame header in the TCP stream and the frame was then resent, so clients dropped with 1002 "expected a continuation frame" and saw messages duplicated. On 2026-10-12 a 5-bot run dropped all five sockets at once every 30–100 s and one `enc_start` arrived 7 times. Provides `beginResponse(int, contentType, const uint8_t*, len)` (used for every PSRAM-served asset) — `beginResponse_P` still compiles but is deprecated. Adds `Connection: close` to every response; there is no keep-alive, so every asset the browser fetches is a new TCP connection. Per-client WS queue capped at 8 via `build_opt.h` (below). |
| `Async TCP` (ESP32Async) | `3.5.0` | WebServer 3.12 requires ≥ 3.5.0. Event queue `CONFIG_ASYNC_TCP_QUEUE_SIZE=64`; when the queue is ≥¾ full it starts *discarding poll events* and throttling — that is where `ERR_CONNECTION_RESET` under a request burst comes from. Priority 10, 16 KB stack. |
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
  message; broadcasts are superseded 100 ms later anyway. **So the `s` tick
  never takes more than `WS_STATE_MAX_QUEUED` (2) of the 8 slots**
  (`broadcastState()`, network-sync.hpp): a backlogged client skips ticks and
  the other six stay free for acks, events and unicasts, which are not
  resent. Before 2026-10-12 a Wi-Fi stall filled all eight with stale ticks
  and every reply behind them was lost (an 8 s stall cost one bot 15 in a
  row). Same 5-bot run, before/after: unanswered requests 38 → 7, connection
  errors 14 → 0. The walk holds AsyncWebSocket's private client-list lock
  (`wsClientsLock()`), exactly as `textAll()` does.
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
resync), so it sends before releasing `G.mutex`. `sendTunnelSync`'s 1.4 KB
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
| The road / chronicle / resources / encounters / map / the admired (cycle with **B**) | Normal gameplay. All six are animated pictures -- 1 the road, 2 the chronicle (a three-panel comic page), 3 the stash, 4 the odds (a strength-tester), 5 the waste (radar map), 6 the admired -- from [ui-scenes.hpp](../ui-scenes.hpp); the join and LAN addresses are in the road screen's footer |
| `FILE UPLOAD` (teal banner, scrolling bar, byte counter) | A `/upload` POST is in progress; auto-clears 1.5 s after last byte |
| `OK` (green) / `FAIL` (red) on the upload screen | Final-chunk flash from [ui-upload.hpp](../ui-upload.hpp) |

The "FILE UPLOAD" screen takes precedence over the gameplay screens — it
suppresses the screen rotation while uploads are streaming so a partial sync
is unmistakable.

### LCD FX: comic cut-ins and the failing terminal ([ui-fx.hpp](../ui-fx.hpp))

The screens still render into `canvas` exactly as before; `fxPresent()`
composes canvas into a second PSRAM sprite (`fxOut`) and pushes that. Don't
draw FX into `canvas`, and don't push `canvas` directly from new gameplay code —
the upload screen and the FX-off switch transition are the only raw pushes.

- **Cues:** `fxCue(FXK_*, pid, chronicleLine)` from `drainEvents()` (plus
  `broadcastQuake()` and the threat-clock chime). Spinlocked, safe from any
  task. The style table `FX_STYLE` decides the SFX word, band fill, shake,
  flash, extras (cracks/bolt/fire/splatter/drips) and a per-kind cooldown.
- **Madness:** folded from `g_dread` each loop pass (`fxWantsFrame`); it drives
  tears, the hum bar, row shiver, static, the two-frame whispers and the eyes.
- **Pacing:** the loop sleeps 100 ms when nothing moves (no pushes at all) and
  one frame period (40–90 ms) while something does. `updateLEDs()` and
  `checkScoreAudio()` stay on their own 10 Hz tick — the lamp shapes count
  loop ticks. Serial prints `LCD FX: n frames/30s compose avg=… push avg=…`
  every 30 s while anything animated: that is the real frame budget.
- **Level** MADNESS / RESTRAINED / OFF is saved as `fx` in the `k10` NVS
  namespace (default MADNESS). OFF is the pre-FX behaviour exactly, including
  the old tube-dropout switch. There is no in-play control for it any more.
- **Button A** (free after the boot USB check) steps the speaker volume LOW
  (2) → MED (5) → HIGH (9) → OFF (0), toasted on screen, saved as `vol` — the
  same 0..9 the web slider sends (`checkFxButton`, `ui-screens.hpp`).
- **Memory:** ~430 KB PSRAM (two sprites, sticker pool, SDF scratch), ~4.5 KB
  internal `.bss`. `fxBegin()` forces OFF if any allocation fails.
- **The observer mirrors it:** `/state` carries `fx` (the last six cues, with
  a sequence number) and `doom` (`aw`, `cl`); `data/observer-fx.js` plays the
  same panels on the TV. See the observer section.
- **Cut scenes** are kinds whose fill is `FXF_NEST` or later (`fxIsScene()`):
  they hold the panel for seconds, draw no band, and sink the whole screen
  into screentone from the edges. Nothing cuts in over one except a
  catastrophe (prio 3), which ends it at once. The TV plays all three too
  (`observer-fx.js` sections 12-13): the nest and the crawl re-laid for a
  wide screen, the commendation as a landscape certificate.

  | Kind | Cued from | What | Length / cooldown |
  |---|---|---|---|
  | `FXK_BELOW` | `EVT_TUNNEL_ENTER` | the nest (section 12): fly, web, giant legs, skitterers, the widow on its thread, eight eyes | 7.1 s / 3 min |
  | `FXK_CRAFTED` | `EVT_ACTION`, CRAFT success | the commendation (section 13): guilloche certificate, order star, typed citation, rubber stamp | 6.2 s / 2 min |
  | `FXK_CRAWL` | `EVT_TUNNEL_EXIT` | one small spider walks across the live screen (not a scene: an `FXE_NOBAND` overlay that takes no turn in the queue) | ~4.5 s / 1 min |
  | `FXK_TRIPWIRE` | `EVT_ENC_START` with `amt` 1 (a booby trap fired, `traps.hpp`) | the wire (section 13b): a boot finds a wire nail-to-eye, it parts, the halves whip back frayed, the boot goes out from under, a deadfall of planks comes down over the glass | 3.2 s / 90 s |
  | `FXK_BEARTRAP` | `EVT_ENC_RESULT` failed + `encEnds` + `amt` 1 (a trap went off) | the jaws (section 13b): an open jaw trap in the dirt, a boot on the pan, the jaws shut round the ankle, chain snaps taut, teeth run | 4.7 s / 2 min |

  `FXK_CRAFTED` is the one cue whose `cap` is not the chronicle's sentence:
  it is the recipe's **name**, which the certificate cites. `FxStyleDef::coolMs`
  is `uint32_t` now; the old `uint16_t` capped every cooldown at 65 s.

#### Previewing the LCD FX on the desktop (MSVC, no flash)

MSVC is on this machine (Visual Studio 2019 Build Tools:
`C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat`),
so the FX layer is previewed by compiling the **actual** `ui-fx.hpp` natively
and rendering GIFs straight from it — nothing is ported to Python, so what you
look at is what gets flashed:

```powershell
python scripts/fxsim/fxsim.py                 # every scene
python scripts/fxsim/fxsim.py quake eye       # just these (--list for names)
python scripts/fxsim/fxsim.py below crafted crawl   # the cut scenes
python scripts/fxsim/fxsim.py death0 death1 death2  # the death takeover: the Doom laughing (doom-laugh.h)
python scripts/fxsim/fxsim.py tripwire beartrap      # the traps' two
python scripts/fxsim/fxsim.py road_day road_storm road_night admired   # the picture screens
python scripts/fxsim/fxsim.py chronicle chron_g0 chron_g10   # the comic page; chron_g0-10 = every event picture
python scripts/fxsim/fxsim.py stash odds_win odds_lose waste       # screens 3-5
python scripts/fxsim/fxsim.py figs figs_big figs_night              # the party: every archetype x pose (scFigure), no FX
```

The picture screens ([ui-scenes.hpp](../ui-scenes.hpp)) run through the same
harness: `runScene()` in `fxsim.cpp` redraws the scene into the canvas every
`scenePeriod()` ms from a hand-built `SceneSnap` (`baseSnap()` / `surv()`),
exactly as the board's loop does, and composes the FX layer over it. On the
board a scene screen repaints on that period instead of `SCREEN_MS`, never
takes the row-diff "reprint", and logs `LCD scene n: frames/30s draw avg=...`
next to the compositor's `LCD FX` line -- the two together are what an
animated screen costs.

Output lands in `scripts/fxsim/out/` (gitignored): `<scene>.gif` at 2x with
the board's own frame pacing (`fxFramePeriod()`, 100 ms when idle) and
`<scene>_sheet.png`, ten frames side by side. It rebuilds `fxsim.exe` only when
`fxsim.cpp` or `ui-fx.hpp` changed; a full run is a few seconds. Scenes are the
`run(...)` calls in `main()` of `scripts/fxsim/fxsim.cpp` — add one there. Base
screens are `scripts/fxsim/fixtures/*.png` (240x320, drawn by the offline PIL
previewers that port `ui-screens.hpp`); any 240x320 PNG works.

How it compiles unchanged: `ui-fx.hpp` builds under `FX_NATIVE`, and the
harness supplies the hooks its section 0 otherwise takes from the board —
`c16()`, the three font tables, `FX_LOCK`/`FX_UNLOCK` (no-ops), `fxAlloc`
(calloc), `fxNowMs` (a scripted clock) and `fxNameOf`. The two facts it had to
get right, both checked against LovyanGFX 1.2.20:

- **Font struct layouts.** `lgfx::GFXglyph` is `{uint32 bitmapOffset; uint8
  width, height, xAdvance; int8 xOffset, yOffset}` and `lgfx::GFXfont` is
  `{uint8* bitmap; GFXglyph* glyph; uint16 first, last; uint8 yAdvance}` after
  an `IFont` vtable base. A GFX glyph's bits are packed **continuously**,
  MSB-first, with no per-row padding (bit `y*width + x`). The headers
  (`src/lgfx/Fonts/GFXFF/*.h`) are plain C; the harness `#define`s
  `PROGMEM` empty and `GFXglyph`/`GFXfont` to field-identical shim structs
  around the include. Font0 (`glcdfont.h`, `font[]`) is 5 bytes per char,
  column-major, bit 0 = top row; Font2 (`Font16.h`) is rows of `(w+6)>>3`
  bytes MSB-first holding `w-1` real pixels (its width table has the +1
  margin baked in). On the board the same tables are reached through
  `fonts::Font0.chartbl`, `fonts::Font2.void_chartbl` / `.widthtbl` and
  `fonts::FreeSansBoldOblique24pt7b`.
- **Buffer byte order.** A 16-bpp `LGFX_Sprite` stores `swap565`: RGB565 with
  its two bytes swapped, i.e. big-endian in memory, so `getBuffer()` hands
  back `uint16_t`s that must be byte-swapped before the channels can be read.
  `ui-fx.hpp` works in that format end to end (`FX_PAL` is precomputed as
  `fxSwap(c16(rgb))`) and the harness's raw frames are the same bytes, which
  is why fixtures are written, and GIFs read, as big-endian RGB565.

What it cannot tell you is **speed**: an x64 desktop says nothing about the
ESP32-S3. The frame budget comes from the board's `LCD FX:` serial line.

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
retries later, backing off to one sweep per 5 minutes. From the second sweep
on, a sweep with anyone seated scans **only the softAP's own channel** (see
"Direct-connect play" below).

### Direct-connect play (no house network)

Players can join the board's own open hotspot `WASTELAND` (192.168.47.1) and
play with no router anywhere. Measured 2026-10-23 with `bots/wroom_swarm`
(an ESP32 filling all five seats over the hotspot; docs/bot-testing.md):

| What | Before | Now |
|---|---|---|
| Hotspot admits | 4 devices (core default) — the 5th phone could not join | `AP_MAX_CLIENTS` 8: the table, an observer TV, a spare |
| Roaming sweep with players seated | full scan, ~7 s off-channel: every seat froze 3.4–4.7 s, replies up to 9.2 s, every 30 s → 5 min for as long as the board ran | one channel, ~120 ms, no freeze |
| Dawn save | ~270 ms of `G.mutex`: ~1 s freezes, `busy` nacks | staged (`save-stage.hpp`), ~15–45 ms |
| A death | seat stayed taken until the ghost reaper (8 s after the socket closed) | freed at once (deferred retry if the mutex is busy) |
| Five picks at once (`bots/sync_burst.py`) | 4.9–8.5 s per sync, heap 118 → 56 KB, the next burst **aborted the board** | syncs paced one at a time: 0.14–0.63 s, heap ≥ 86 KB |

- **Never hand the WebSocket library a big message without room to copy it.**
  `text()` copies into a `std::vector`; when no block that big is left,
  `operator new` throws and the board aborts (no exception handler). Syncs go
  through `queueSync()` / `serviceSyncQueue()` (network-sync.hpp "Sync
  pacing": GameLoop sends one at a time, once the last has left its socket and
  `heap_caps_get_largest_free_block()` has room), and `broadcastState()` skips
  a tick rather than copy into nothing. `/state` → `mem.psramMax`,
  `syncWaits`, `syncQueued`, `broadcastNoRoom` show how close it runs.

- **The full sweep is still needed** to find a home network on a different
  channel than the softAP, so: the first sweep after boot is always full (the
  softAP starts on channel 1), and sweeps are quiet only while someone is
  *seated* — not merely associated, or an observer TV parked on the hotspot
  would keep the board off its home network for good.
- **`NETWD HEAL` only reassociates a station link.** With no house network it
  logs `skipped`: `WiFi.reconnect()` would send the station scanning every
  channel for a network that is not there.
- **Test it at the desk: type `away` on the USB serial console (COM5).** The
  board reboots once as if none of its known networks were in range (the boot
  join goes to an SSID nobody broadcasts, sweeps ignore the known list) without
  forgetting any password; the next reboot is normal. `swarm_log.py --k10-cmd
  away` does it for a run.

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

- **A full scan takes the softAP off its channel for ~7 s** (measured; it was
  thought to be a second or two). That's why sweeps only run while the board
  is off every known network, never during an `/upload`, back off to 5-minute
  spacing, and stay on the softAP's channel while anyone is seated.
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
   • "<name>.gz" beats "<name>": served with Content-Encoding: gzip, and the
     plain copy is not even read when its .gz is on the card. build_web
     writes a .gz beside EVERY root .js/.css/.html/.json, not just the bundle
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
   • boot screen then HOLDS (AssetLoader.holdBoot, registered in network.js)
     until the first lobby/sync arrives and every image it queued has loaded
     or failed (engine.js artSettled) — a visit or refresh never reaches the
     character picker with art still missing. Mid-session reconnects skip it.
   • fires document event 'assets:ready'; sw.js is registered after that
   • observer.html has its own boot screen: scripts, then the tile atlas pages
     (Art.preload) before it fades out
```

Adding a client file: put it in `data/`, add it to `web-assets.json` at the
right position, sync. Nothing else. Removing one: the reverse. The old
hardcoded `WEB_FILES[]` table in the `.ino` is gone — that table is why
`world-entities.js` and the `*-field.js` files 404'd for a while.

Numbers as of 2026-09-12: 18 scripts → one 314 KB bundle → **92 KB gzipped**;
style.css 74 KB → 15 KB; index.html 35 KB → 11 KB. Cold page load is 5
requests (`/`, `assets.json`, `style.css`, `app.bundle.js`, `sw.js`) instead of
~22, and ~120 KB on the wire instead of ~460 KB.

**PSRAM is the cache's limit, and it was full.** Everything in the `/data`
root and `/data/img` is cached at boot, and a file that does not fit is logged
`WEB/IMG cache ps_malloc FAIL` and then 404s (204 for an image) for the whole
boot. By 2026-10-23 the 50 image sheets were 5.2 MB of the ~7 MB left after
the LCD FX, and the plain scripts the bundle is made of (still cached one by
one, for the observer and the `web-assets.json` fallback) another 1.4 MB:
the River Channel sheet and five scripts, two of them the observer TV's, were
dropped at every boot. With a `.gz` beside every root web file those 1.4 MB are
~0.5 MB; everything fits with ~380 KB of PSRAM to spare (42 web files, 51
images). After adding art, check the boot log for `ps_malloc FAIL`.

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

### Saves (`save-stage.hpp`, 2026-10-23)

`saveGame()` (network-persistence.hpp, on the `Saver` task) used to write its
nine `/save/*.bin` files with `G.mutex` held: 270 ms median, 338 ms worst, on
every dawn and every drop. Everything else that wanted the mutex waited or gave
up — `broadcastState()` skipped those ticks (a ~1 s freeze on every phone at
dawn), handlers nacked `busy`, and the `EVT_DOWNED` handler's 20 ms take missed
on every death, which left the dead player's seat taken. Now:

- **Staged.** Each save function writes into a `SaveOut` (same `if (!f)`,
  `f.write()`, `f.close()` as a `File`) backed by a PSRAM buffer per file.
  The mutex is held only for that copy (~15–45 ms); the card writes happen
  after it is given back. The buffers are kept between saves (~60 KB).
- **Crash-safe.** Each file is written as `<path>.new`, its size verified,
  then swapped in (remove + rename). `saveRecoverInterrupted()` runs before
  `tryLoadSave()` at boot and finishes or drops a swap the board died in.
  Writing in place is how a world was lost on 2026-10-23: a board hung
  (task watchdog) while rewriting `map.bin`, booted to a 0-byte file and
  generated a new world.
- **Short writes recover** the `/upload` way (above): reopen for append and
  carry on from what landed. Write each file in ONE `f.write()` per attempt:
  4 KB pieces hit the 16 KB-boundary failure on two saves in three. The old
  unchecked writes put a short `map.bin` (6016 or 2010 of 49035 B) on the card
  about once per hundred saves.
- **At most one save per 2 s** (`SAVE_MIN_GAP_MS`). A dawn, a death and the
  disconnect after it each request one; requests in the gap fold into the
  next save, never lost.
- `/enc` no longer takes `G.mutex` (it was only there to keep its read off the
  card during a save). `encLoadFile()` still runs under the mutex, which
  guards its buffer; FATFS serialises the card.

**Never read big files off the SD card on the `async_tcp` task.** SD transfers
on this board are polled (the CPU spins) and the bus is shared with the LCD
(~10–30 KB/s). A 2026-10-23 experiment that streamed uncached web files from
SD tripped the task watchdog within one 439 KB request; the board hung in the
panic with the card mid-transaction, and the card would not mount again until
the K10 was power-cycled (a USB reset does not cut its power). Make the file
fit in PSRAM instead (see the web pipeline's `.gz` siblings).

## Art assets (`data/img/`)

**Hex terrain tiles, shelters and forage animals are sheets**: one per
terrain, one per shelter kind and one for the forage animals, not one file
each:

```bash
python scripts/hex_sheets.py build --check      # validate + sizes, write nothing
python scripts/hex_sheets.py build              # art/hex-sheets/*.png -> data/img/hex<Name>.webp + tiles.json
python scripts/hex_sheets.py split Ridge        # a sheet -> one PNG per cell, for per-tile tools
```

- **Edit the masters, not `data/img/`.** The lossless master is
  `art/hex-sheets/hex<Name>.png`, outside `data/` so `sync_data` never pushes
  it. The grid is 256 px cells, 4 across, with 32 px of clear space around
  and between each cell. Cell N is variant N, so order a sheet from common to
  rare: `pickVariant()` weights slot 0 heaviest. A fully transparent cell is
  empty, and the pool ends at the first empty cell.
- **Landmarks** are the cells from a terrain's first pinned sentinel on
  (`LANDMARKS` in the script). Open Scrub cell 10 is Jack's Chopper, and
  Broken Urban cells 10–12 are the downtown core. They go to
  `tiles.json` `poi` and stay out of `counts`.
- **Alternates** let one variant slot hold several tiles. A sidecar
  `art/hex-sheets/hex<Name>.json` lists them, `{"alts": {"<variant>":
  [cell, ...]}}`, and the cells sit past the pool on the same sheet. They go
  to `tiles.json` `alts`, never to `counts`, so the firmware's weighting is
  unchanged. The client deals one of a slot's tiles by a hash of the hex's
  map q/r: `altPick()` in engine.js, mirrored bit for bit in observer.js.
  Broken Urban (2026-09-28) is the first sheet that uses them: 10 slots and
  3 core pins, 24 buildings.
- **Shelters** are two sheets: `art/hex-sheets/shelterBasic.png` for
  `cell.shelter` 1 and `shelterImproved.png` for 2.
  - The grid is 224 px cells with a 16 px gutter.
  - The art is the user's 2026-09-27 AI re-render
    (`art/hex-sheets/src/shelter-sheet-improved.png`), cut back into cells.
  - `tiles.json` carries `shelterCounts` (the firmware's `sv`),
    `shelterCell` and `shelters`.
- **Forage animals** are one sheet, `art/hex-sheets/forrageAnimal.png`. They
  are drawn on food (resource 2) hexes.
  - The grid is 80 px cells with a 16 px gutter.
  - `tiles.json` carries `forageCounts` (the firmware's `forrageAnimalCount`,
    `fa` on the wire), `forageCell` and `forage`.
- **The caravan** is one sheet, `art/hex-sheets/caravan.png`: a single
  sticker of the user's APC leading a cargo truck, drawn by `renderCaravan()`
  (world-entities.js) as a 1.5×HEX_SZ square, under the survivor pawns.
  - The grid is 384 px cells with a 16 px gutter, lossy (q90, 25 KB).
  - The source is `art/hex-sheets/src/caravan-convoy-src.png` (two painted
    hexes). Both vehicles were matted off the sage ground with their halftone
    shadow kept as dots, like the shelters.
  - The art heads towards the viewer's lower left. The client mirrors it
    while the caravan travels east (`caravanFacing`).
  - `tiles.json` carries `caravanCounts` (the firmware ignores it),
    `caravanCell` and `caravan`. Without the sheet the client falls back to
    the old brown canvas badge.
- **The Motorbike** is one sheet, `art/hex-sheets/bike.png`: the user's
  chopper, drawn under a survivor's portrait while item 26 is equipped, so the
  bubble looks like it's riding it (`drawCharIcon()` in renderer.js).
  - The grid is 272 px cells with a 16 px gutter, lossy (q90, 22 KB).
  - The source is `art/hex-sheets/src/bike-chopper-src.png`. It was matted
    like the convoy. The spoke gaps are transparent and the halftone shadow
    is kept as dots.
  - `BIKE_RIDER` sets where the portrait sits on the seat and how big the
    bike is relative to it (~1.05×HEX_SZ). Rider plus bike is centred on the
    hex. The art faces right; it mirrors while the pawn heads west
    (`renderPos[i].face`).
  - `tiles.json` carries `bikeCounts` (the firmware ignores it), `bikeCell`
    and `bike`. Without the sheet, the pawn is the bare portrait.
  - Getting one: recipe 20 is a `starter` (7 scrap + 1 fuel + 1 Sparky Bits
    at a Settlement). Its 8 tokens fill a bare 8-slot pack with no room for
    food or water, so a Backpack (or Rig, or a Mule) comes first in
    practice; there is no worn-item check. A pack too small for a recipe's
    tokens gets "a bigger pack (N tokens to carry)" on the craft card. The bike adds
    `slots = +2` of its own. It is `trade = no` (out of the spawn draw);
    Jack's Chopper still gives one free. The `biker` bot builds and rides it
    (docs/bot-testing.md, "The Biker").
  - **Out of fuel, the bike stalls and fades off the map.** A daily-cost
    item (the bike, the Rust Rocket) grants its AP once a game-day, when that
    day's cost is paid (inventory_items.hpp):
    - It pays at dawn if it can (`applyDawnItemCosts()`).
    - Otherwise it pays the moment the pack can cover it: `kickStartItems()`
      is polled from `tickGame()` and skips anyone in a scene, resting or
      downed. The first fuel a stalled bike finds is burned, and +5 AP lands
      on today's budget (EVT_REFUEL, `ev refuel`: a log line and a toast for
      the rider).
    - An item put on mid-day pays the same way.
    - `s_paidItem` records the item ids paid for today, so unequipping and
      re-equipping can't buy the AP twice.
    - The paid list is not saved. After a boot, whatever is equipped counts
      as paid.
  - **Drawing it.** The tick broadcast carries `uf` (`unfuelledMask()`, the
    unpaid slots). `bikeStalled()` in renderer.js reads it, and the bike fades
    out over `BIKE_FADE_S` (0.9 s) while the rider slides off the seat onto
    the ground shadow. Fuel fades it back in. It stays equipped, and its +2
    slots still count. Equipping or unequipping fades it too (`easeBikes()`,
    `renderPos[i].bike`). A board without `uf` falls back to dawn's `unf`
    plus an empty pack.
  - Survivors sharing a hex ride as a pack (`packSlot()` in renderer.js): two
    staggered lanes, each pawn shrunk by `PACK_SCALE[n]`, call signs and
    scores hidden. This replaced the ring of full-size markers, which buried
    most faces under the bikes.
  - Riding throws dust (`data/bike-dust.js`, the `bike_dust` layer, under the
    pawns). It is kept in map space (hex units), not screen px: the camera
    follows your own bike, and screen-space dust would ride along with it.
    The colour is per terrain; wet terrain (Marsh, Flooded District, River
    Channel) throws none. Lifetimes count frames, like the other particle
    systems. To judge the timing, step `render()` by hand at 60 fps: the
    headless browser renders at about 19 fps.
- Neither shelters nor forage have a variant on the wire. The client picks a
  sprite by position, so order doesn't matter. Both draw through
  `drawAtlasCell()` in engine.js, which switches to the half- or
  quarter-size copy once a sprite is shrunk past 2×.
- **`build`** writes one WebP page per sheet.
  - **Terrain pages are lossy**, with the alpha kept lossless. For each sheet
    it picks the lowest of q90/92/95 that keeps the worst tile inside the 3.5
    visible-RMSE bar. Settlement tops out at q95 / 3.95, because 4:2:0 chroma
    smears the yellow school bus. Broken Urban tops out at q95 / 3.66
    (624 KB): its yard clutter is all fine detail.
  - **Shelter pages are lossy too.** At 224 px the re-rendered art passes
    the bar at q92 (Basic) and q90 (Improved).
  - **The forage page is lossless.** Its small ink-outlined sprites never
    get under the bar lossy. The old 112 px shelters were 4.2 even at q100.
- **`build` also writes `tiles.json`.** It holds `counts`, `shelterCounts`,
  `forageCounts`, `version`, `flat` and where each cell sits.
  - The same manifest feeds `setupVariantCounts()`, the mock's
    `scanVariantCounts()`, engine.js (`loadTerrainVariants()`,
    `shelterSprite()`, `forageSprite()`) and the observer.
  - The sheets add up to 18 files and 1.62 MB. Shelters are 165 KB of that,
    and forage is 55 KB.
- **To take in a re-render of a sheet:**
  1. Cut it into `<name><N>.png` cells.
  2. Run `hex_sheets.py import --src <dir> --force`. That re-seeds every
     master that has files in `<dir>`.
  3. Run `build`.
  4. Keep the source out of `data/`, because everything under `data/img`
     syncs to the board and takes a PSRAM slot.
- **`flat: true`** marks painted tiles, which draw exactly like the old
  per-file ones: a 2×HEX_SZ square with the grid stroked over it. Without it,
  a manifest means build_tiles.py's 3/4 dioramas (`tile.diorama` in
  renderer.js).
- **No `sw.js` bump for sheet edits.** Pages are fetched with `?v=<version>`,
  and every build changes the version.
- **Stale per-file tiles stay on the SD, harmlessly.** `sync_data` never
  deletes anything from the card. `boot-assets.hpp` skips `hex*.png`,
  `poi_*.png`, `shelter*.png`, `forrageAnimal*.png` and the unused
  `basicShelter*.png` whenever
  `tiles.json` is on the card.
- **Don't run `scripts/tilegen/build_tiles.py`.** It is the rejected
  procedural generator, and it overwrites `tiles.json`.

Two more scripts, both dry-run by default and both needing only Pillow +
numpy. Neither changes a filename, an extension or a route, so no firmware,
engine.js or MIME change is involved.

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
- **`gen_missing_tiles.py`** refuses to run while `data/img/tiles.json`
  exists. The per-file slots it fills are gone, so a placeholder now means
  painting a sheet cell. What it did: it filled empty `hex<Name><N>.png` slots with flat
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
encounters), serves `GET /enc?biome=X&id=Y` from `data/encounters/`, serves
`GET /state`, and accepts `POST /upload?dest=/data/...` writing to
`mock-server/uploads/<dest>` (gitignored).

`GET /state` mirrors the firmware's route in `game-server.hpp` field for
field, including `?pid=N` (that player's vision disk under `view`) and
`encBiome`/`encId` on an active encounter. `?sd=1` is accepted and ignored.
It exists because the observer screen polls it instead of taking a `/ws`
seat — every `/ws` client is a *player* and there are only six.

Two deliberate infidelities, both load-bearing:

- `TNAME_FULL` is 12 names wide for 16 terrains, reproducing the firmware's
  out-of-range read, so terrain 12-15 serialise with an empty `terrainName`
  offline as well as on hardware. Take terrain names from `TERRAIN[]`
  client-side.
- The mock **deletes** a player on disconnect, so the slot reports
  `connectMs: 0`; the firmware keeps the stale value until someone re-picks.
  Anything keyed on `pid` + `connectMs` has to survive both.

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

### The Understory in the mock

`mock-server/ecology.js` is the JS port of the slime mould and the Wasteland
Daisy ([ecology-spec.md](ecology-spec.md)); `server.js` ticks it, broadcasts
the `eco` message, bites on entry, seeds daisies at every LL-loss site and
reports `eco` in `/state` exactly as the firmware does. It is also the tuning
harness the spec asks for:

```bash
node mock-server/server.js --eco-speed=20        # 20x the board's 5 s eco tick
node mock-server/server.js --eco-seed=12345      # pin the genome (MOCK_ECO_SEED)
```

Drive the stages from the browser console (`socket.send(JSON.stringify(…))`):

```json
{"t":"dbg_eco","act":"fruit"}                         every foraging colony fruits
{"t":"dbg_eco","act":"burst"}                         every fruiting body bursts (spore flights)
{"t":"dbg_eco","act":"germinate"}                     every dormant spore wakes
{"t":"dbg_eco","act":"daisy","stage":3,"count":3}     a bloomed patch on your hex (or q/r) — step off and back to be bitten
{"t":"dbg_eco","act":"speed","n":50}                  eco clock multiplier
{"t":"dbg_eco","act":"regen","seed":0}                new species, scars cleared
{"t":"dbg_eco","act":"bite","on":0}                   the NVS eco/bite switch
{"t":"dbg_eco","act":"blight","on":0}                 the NVS eco/blight switch
```

`/state?ecobite=0`, `/state?ecoblight=0` and `/state?ecoseed=N` do the same as
on the board (`eco.eaten` counts resources the blight has eaten). The
mock's `eco` block adds `speed` and `pinnedSeed`. Nothing here is persisted:
scars live for the mock's process only.

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
send({ t: 'dbg_ignite', dq: 9, dr: -3 });         // ...or the hex at that axial offset from the sender (watch it spread without burning)
send({ t: 'dbg_doom', awareness: 100 });          // teleport Creeping Doom adjacent to the sender, set its awareness (world-system-spec.md)
send({ t: 'dbg_doom', awareness: 0, dq: 8, dr: 0 }); // ...or at that axial offset: e.g. 8 hexes downwind, to watch it catch the scent
send({ t: 'dbg_settle' });                        // force a settlement to form on the sender's hex (actions_game_loop.hpp doShelter())
send({ t: 'dbg_flood' });                         // force-flood the sender's hex at intensity 2, ignoring the water/storm gates
send({ t: 'dbg_tunnel', h: 2 });                  // stand the sender on bunker hatch #h and refill AP (tunnel-system-spec.md)
send({ t: 'dbg_tunnel', h: 2, below: 1, ap: 0 }); // ...underground on that shaft instead, with a pinned AP budget
send({ t: 'dbg_collapse', d: 0 });                // cave in the tunnel hex in direction d from the sender
send({ t: 'dbg_die' });                           // the sender's survivor goes down on the spot (death drops, below)
send({ t: 'dbg_age', days: 29 });                 // age every ground pile + remains by N days, then run the dawn sweep
send({ t: 'dbg_trap', d: 0, id: 16, known: 1 });  // arm trap file #id (default random) on the hex in direction d; known:1 = its icon on your map now
```

### Booby traps in the mock

Design and wire: [trap-system-spec.md](trap-system-spec.md); firmware:
[traps.hpp](../traps.hpp). The mock places traps at worldgen with the
firmware's bands and rates (`[trap] placed core=... open=... tunnels=...` in
the console), but its map is a baked stripe with no real cities, so the
counts are a stand-in — use `dbg_trap` to put one where you want it, then
step onto it. What to check:

- The panel opens **unprompted** (`enc_path` carries `"trap":1`), with the
  rust frame, no Walk Away, and the two doors — back out / work it.
- Backing out, or leaving a cache with nothing, is `enc_end reason:"escaped"`
  plus a unicast `ev trap out:"known"`: the glyph (strip index 21, top-right of
  the hex) appears, stays when you walk away (dimmed, from explored memory),
  and shows through weather. Stepping back on forces the scene again.
- Banking anything: `ev trap out:"spent"`, and the glyph is gone for good. A
  failed check: `out:"sprung"`, LL never below 1, and the scene ends.
- The TV's `TRIPWIRE` / `BEARTRAP` cut scenes come off `/state`'s `encTrap`
  when there is no fx ring (the mock has none).

Encounter JSON `skill` ids use the firmware's 5-skill enum — 0 NAVIGATE,
1 FORAGE, 2 SCAVENGE, 3 SHELTER, 4 ENDURE. (The files were originally
authored against a 6-skill enum that included Treat; they were migrated in
Sept 2026. Don't reintroduce id 5.)

Client module: [data/ui-encounter.js](../data/ui-encounter.js). Markup lives in
`index.html` under `#enc-overlay`; styles under "Encounter overlay" in
`style.css`.

### Death drops (remains)

A survivor who goes down leaves everything they carried on the hex where they
fell, for anyone to take — including the same player, walking back as whoever
they respawn as. Logic: the "Ground piles and remains" block in
[inventory_items.hpp](../inventory_items.hpp); mock mirror under "Ground
items" in `mock-server/server.js`.

- **Items** — every pack stack and every worn item becomes an ordinary ground
  pile (`groundItems[]`, `MAX_GROUND` now 64), taken with `pickup_item`.
- **Resource tokens** go into a `Remains` record (`remainsTable[12]`), because
  a `HexCell` holds one resource type and a survivor carries five. Taken with
  `{t:'loot',res:1-5}` or `{t:'loot'}` for everything that fits, capped by
  `tokenRoomFor()` like SCAVENGE. **No score** — the fallen survivor already
  scored them and score survives a fall, so paying again would make dying and
  walking back a score pump. Answered with
  `{t:'loot_result',ok,why,got[5],inv[5]}` (`why`: 0 ok, 1 nothing here,
  2 pack full).
- **Stepping on takes it.** A surface step onto the hex (or climbing out of a
  hatch onto it) runs `scoopGroundOnArrival()` from `handleMsg_move`: the
  remains' tokens and *every* pile there, anyone's, whatever fits. The mover
  gets the same `loot_result` with `auto:1`, `items:[[id,qty],...]`, the pack
  arrays, and `left:1` if something stayed behind (sent with `ok:false` too,
  so a full pack still hears about it). There is no pickup UI any more; the
  explicit `loot` / `pickup_item` messages remain for bots and tests.
- The record doubles as the grave marker (`renderRemains()` in renderer.js),
  drawn whatever the fog like the caravan and the Doom, and lives until its
  tokens are taken *and* the last pile on its hex is gone. It is drawn as two
  broken boards lashed into a cross with a faded rag in the fallen archetype's
  colour (`drawBrokenCross()`, `fadedRag()`). It has no name label and is
  meant to sit half hidden in the ground (the user's call, 2026-10-13).
- **Everything on the ground ages out `GROUND_AGE_DAYS` = 30 game-days after it
  was last added to** — dropped items and encounter overflow included, because
  a pile is one stack whoever left each part of it. Swept at dawn by
  `groundAgeOut()`; a full table also reclaims its oldest pile early rather
  than silently losing the new one.
- **Wire:** sync and every `ground_update` carry both halves: `gi` (piles, now
  with `d`, the day each was last added to) and `rm` (`{q,r,pid,nm,d,res[5]}`). `ground_update` has
  `why:"fell"` + `pid` for a death and `why:"aged"` for the dawn sweep; one
  helper, `broadcastGroundUpdate()`, builds them all. Every path that puts
  something on the ground now sends one — `enc_bank`'s overflow never did.
- **Surface only.** Piles and remains have no depth; a fall underground lands
  on the hatch above (q/r stay pinned there), and `pickupGroundItem()` /
  `loot` refuse at depth 1.
- **Save:** ages and remains ride `/save/ground.bin`, a file of their own, so
  `SAVE_VERSION` did not move. A save without it loads with every pile dated
  to the load day and no remains. `regen` deletes it with the other two.
- The single funnel is the `EVT_DOWNED` handler (`network-events.hpp`); a
  downed seat's re-pick runs `dropRemains()` again as a backstop (a no-op once
  the pack is empty). In the mock the funnel is `downPlayer()`, which also
  fixed two old mock-only bugs: `downed` was *broadcast* (every open tab went
  to the death screen) and the dead seat stayed "on" forever.

### Trails (footprints and tire tracks)

- **Bits say who, sides say which way.** `HexCell.footprints` / `tireTrack`
  are unchanged. [trails.hpp](../trails.hpp) keeps, per surface hex, each
  seat's and the caravan's *latest* pass: side in, side out, ridden. That's a
  PSRAM block, not HexCell, so `SAVE_VERSION` did not move. It's set by
  `movePlayer()`, the caravan tick, spawns (`trailStart`) and climbing out of
  a hatch. It's surface only; tunnel trails are inferred from their bits.
- **Wire (PROTO 6):** surface `vis`, `surv` and `sync` carry `"trl"`, 7 hex
  chars a record (QQ RR, S slot 0-5 seat / 6 caravan, BB byte). It's sent only
  while the cell's bit is set. `ev mv` carries `"d"`, the side stepped through.
- **Save:** `/save/trail.bin`, sparse, written only when changed and loaded
  only with map.bin. A save without it loads with no sides, and the client
  infers them. `regen` deletes it.
- **Client:** [data/trail-field.js](../data/trail-field.js) draws from its own
  recording first (localStorage `wl.trails.v1`), then the served sides
  (`cell.trl`), then inference. One sole per seat; a ridden step is a rut, not
  prints.
- **Mock:** sides use the firmware DQ/DR. The mock's DIR_DELTA 0/3 (SE/NW)
  steps jump two hexes, so they start a fresh pass and `d` is -1. Walk with
  1/2/4/5 to test.

### Wind (fire runs downwind, scent carries)

- **One wind, two halves.** [data/wind.js](../data/wind.js) draws it (smoke,
  rain, ash, clouds, dust devils, the Doom's gas tail). [wind.hpp](../wind.hpp)
  is the same closed form on the board, `windNow()`. Keep `WIND_TERMS`,
  `WIND_V` and `WIND_WEATHER` the same in both files. The mock loads `wind.js`
  itself through `vm`, so it has no copy to drift.
- **The board's clock is the wind's clock.** `gs.wt` (every `s` tick and
  `sync`) and `/state` `"wt"` carry `millis()`. `Wind.serverClock()` locks the
  game client and the observer onto it, so the wind drawn is the one being
  played. It doesn't use NTP because AP mode has none. A reboot just picks the
  wind up somewhere else, and nothing is saved.
- **Fire runs downwind** (`spreadFire`, world-system.hpp). Each neighbour's
  20% roll is scaled by `1 + FIRE_WIND_K * s * along`, clamped 0.25-2.25. In
  clear air that's about x1.7 downwind and x0.3 upwind. Total spread is
  unchanged.
- **Scent carries** (`hottestTrackWithin`). A trail `d` hexes off reads as
  `d * (1 - k * along)`, with `k = min(0.35, 0.4 * s)`. Clear air gives the
  Doom about +28% reach upwind of it and -18% downwind; a storm gives +54% and
  -26%. The Doom's gas tail leans downwind, so if its reek blows at you, it
  can't smell you.
- **Measuring it in the mock:** run `node mock-server/server.js
  --world-speed=15` (launch config "Mock Server (fast world)", port 8768).
  That's one world tick a second instead of fifteen. Combine it with the
  `dbg_ignite` / `dbg_doom` offsets above. On 2026-10-13, in a storm: a Doom
  8 hexes downwind caught the scent 4/4 times and one 5 hexes upwind 0/4
  (radius 6). In clear air, fire spread 15 downwind / 7 across / 5 up.
- **Bots don't know about the wind.** No policy reads it, so an arena
  measures what it does to wind-blind play. The A/B on 2026-10-13 compared a
  zero-gain control build against the wind build: 2 x 15-min runs each, 5
  `quester`, sprint. Doom deaths were 3 vs 3, and fire killed nobody in either
  arm. The overall gap (21 vs 37 deaths) came from the weather rolls: the wind
  arm drew a chem storm, which caused 7 of its deaths, and the control arm drew
  none. A policy that stays downwind of the Doom would be the real test.

## Observer screen (`data/observer.html`)

A full-screen page for a TV that watches a live game over `/state` and
narrates it. Design and rationale:
[docs/observer-screen-spec.md](observer-screen-spec.md).

```
http://<board>/observer.html                 live, served from the board
http://<board>/observer.html?feed=fake       scripted run, no game needed
http://localhost:8765/observer.html?host=192.168.1.42    desktop dev
```

- **It never opens a WebSocket.** `handleConnect()`
  ([network-session.hpp:54](../network-session.hpp:54)) hands every `/ws`
  client a player slot and there are six. A spectator on the socket eats a
  seat and draws itself on everyone's map. It polls `GET /state` at 1 Hz with
  one `AbortController` in flight and 2.5-8 s backoff on error.
- **`/enc` is fetched once per encounter file, ever, and cached.** That route
  takes `G.mutex` (500 ms) and reads SD. Never poll it. `OBS.Scene.fetches`
  in the console is the running count.
- **Seven files, none of them in `web-assets.json`:** `observer.html`,
  `observer.css`, `observer.js`, `observer-scene.js`, `observer-lines.js`,
  `observer-feed.js`, `observer-fx.js`. `observer.html` also loads
  `game-data.js` for `ADMIRED`/`TERRAIN`/`WEATHER_PHASE_NAMES`, and the game's
  own `wind.js` + `ground-fx.js` for the air over the disk (below). Deploy
  with the normal `.\scripts\sync_data.ps1 <board-ip>`.
- **The air over the disk:** the hex disk (`#s-disk`) is a still picture,
  redrawn only when the ground under the camera changes. Wind-driven water,
  heat shimmer, glass glints, gusts over scrub, cloud shadows and dust devils
  go on a transparent canvas laid over it (`#s-air`, `Air` in observer.js),
  ticked by `setTimeout` at 25 Hz only while the open-ground disk is on screen.
  The shimmer reads the still disk in place (`GroundFx` `opts.source`). A slow
  TV gets fewer ticks, never more than a third of the main thread. Its sun is
  `worldSun()` on the board's `dayTick`, which also drives the page's
  `--night` dimming: dawn at tick 0 and dusk at 78% (the SS_DUSK sting). The
  old `cos()` arc there was inverted and dimmed the page at mid-day.
- **`observer-fx.js` is the LCD's FX engine ported to a canvas overlay**
  (same hash, noise, halftone, SFX glyphs — embedded from LovyanGFX by
  `python scripts/gen_observer_fx_fonts.py`). With firmware that publishes
  `/state` `fx` it cuts in on exactly the LCD's beats; without it (the mock,
  `?feed=fake`) it derives panels from the observer's own diff events, and
  the tunnel scenes from each player's `/state` `dp` (0 surface, 1 below):
  up-then-down is the nest, down-then-up the crawl. A craft cannot be seen
  in `/state`, so the commendation only ever comes off the ring. To look at
  one on the desktop, `OBS.FX.cue('BELOW', 'Mox', 'goes down into the dark.')`
  (or `'CRAFTED', name, 'Sock Puppet Bandage'`, or `'CRAWL', name`) in the
  console. `?fx=reel` plays every panel in turn, `?fx=0|1|2` sets the level,
  `?fxmad=N` pins the madness. It is paced by `setTimeout`, not
  `requestAnimationFrame` — the desktop app's browser pane never ticks rAF.
- **Budget check before adding more:** the board caches one PSRAM entry and
  registers one HTTP route per file in the SD `/data` ROOT, capped at
  `MAX_WEB_FILES = 48` ([Esp32HexMapCrawl.ino:1046](../Esp32HexMapCrawl.ino:1046)).
  These six took the root to 36 files / ~132 KB. With `wind.js` and
  `ground-fx.js` (2026-10-13) it is 42 of 48 — a `.gz` and its plain twin share a
  slot, and `.cfg` files take none. The headroom is thin now: a new root file is
  a real cost, which is part of why the synthetic feed is one file and not three.
- **`?feed=fake`** replaces `fetch` with a scripted five-player run that walks
  a real encounter file three different ways and kills everybody by a
  different cause. Use it to tune line banks with no hardware; `?t=N` jumps
  to a beat.
- **Console handles:** `OBS.ST` (snapshot, poll counters), `OBS.Roster`
  (samples, debt ledger, obituaries), `OBS.Director` (subject, phase, cuts),
  `OBS.Narrator`, and `OBS.handle(raw)` to step the whole pipeline a frame at
  a time without waiting out the poll interval.
- **Two firmware dependencies**, both already in:
  - `encBiome` + `encId` on the `/state` player block
    ([game-server.hpp:582](../game-server.hpp:582)). Without them a scene is
    anonymous and the prose, choices and hazards are all unreachable.
  - `variant` on each `view.cells[]` entry, plus top-level `vc`/`sv` counts.
    Without them the vision disk can only draw flat `TERRAIN[]` colours; with
    them it draws the same terrain sheets (`/img/tiles.json`) the players see.
    A terrain with no art (River Channel) still falls back to flat colour.

**Gotcha this screen is uniquely exposed to:** the ways a bot run fails
silently. [bot-testing.md](bot-testing.md) warns that a `pick` can be refused
and an `enc_start` dropped when `q`/`r` are missing — on the observer both
look like a scene that simply never opens, with no error anywhere.

---

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
  image cache at boot, capped at `MAX_IMG_CACHE = 160`.** 44 slots are used,
  because the ~80 hex tiles, 13 shelter files and 6 forage animals are now
  18 sheets. Anything
  past the cap is silently skipped and served as 204.
  That count is *files*, not images — the three `*_PROMPTS.md` / `IMAGES_NEEDED.md`
  notes under `data/img/` burn three slots and ~29 KB of PSRAM for text the
  board never serves. Pixel glyphs
  (item fallback icons, `img/ui_glyphs.png` sprite strip) are hand-drawn ASCII
  in `scripts/gen_pixel_glyphs.py` — edit the grids there and re-run it rather
  than adding one PNG per glyph.
- **Item badges are not images at all.** `data/item-icons.js` draws each of
  the 65 items from distance-field shapes and writes its own tiny indexed PNG
  as a `data:` URL, at the size and device-pixel ratio the slot needs (so
  there is nothing under `data/img/items/` to add, sync or cache per item).
  `getItemIcon(id, px)` is the one entry point. Iterate on a drawing with
  `node scripts/item_icon_sheet.js --ids 5,16 --zoom 5` — a PNG contact sheet
  of every size the UI uses, no browser. Don't swap its encoder back to
  `canvas.toDataURL`: that is ~5 ms an icon, ten times the drawing.
- **`sw.js` caches `/img/*` cache-first forever.** If you change an image
  in place (same filename), bump the `CACHE` name in `data/sw.js` or clients
  keep the old bytes.
