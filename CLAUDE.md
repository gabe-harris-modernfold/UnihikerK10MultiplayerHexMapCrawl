# Claude / agent guidance

Read **[docs/dev-loop.md](docs/dev-loop.md)** first. It is the canonical, terse
build / flash / deploy / mock-dev reference for this project — every command,
port, and on-device verification step lives there.

Other key references:

- `scripts/` — `build.ps1`, `flash.ps1`, `sync_data.ps1` (push `data/` over
  HTTP `/upload`), `sync_data.sh` (bash mirror). `build.ps1` pins libraries
  into repo-local `.arduino\` (`setup_libs.ps1`, versions in
  `_arduino-env.ps1`) and stages the sketch — always build through it, not a
  bare `arduino-cli compile`. Extra compiler flags go in `build_opt.h`.
- `mock-server/` — Node mock of `/ws` + `/upload` for offline UI work
  (`npm run dev` on `:8765`).
- **[docs/bot-testing.md](docs/bot-testing.md)** — `bots/`, the Python harness
  that plays the game over `/ws` against a real K10 so balance can be measured.
  Read it before touching `bots/` or drawing conclusions from a run: the
  protocol has several failure modes that are completely silent (a refused
  `pick`, an `enc_start` without `q`/`r`, a dirty board at reset).
- `usb_drive.h` — Hold-A-at-boot USB-MSC mode. Coexists with the live
  `/upload` flow; do not replace one with the other.
- `ui-upload.hpp` — on-device "FILE UPLOAD" progress screen, driven by the
  `/upload` handler in `game-server.hpp` and rendered by the LCD refresh path
  in `Esp32HexMapCrawl.ino`.

If you're about to add a new firmware screen, route, or deploy script, check
`docs/dev-loop.md` first — most of the workflow is already wired and the doc
calls out the gotchas (manifest skip, PSRAM cache, AsyncTCP task isolation).
