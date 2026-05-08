# Claude / agent guidance

Read **[docs/dev-loop.md](docs/dev-loop.md)** first. It is the canonical, terse
build / flash / deploy / mock-dev reference for this project — every command,
port, and on-device verification step lives there.

Other key references:

- `scripts/` — `build.ps1`, `flash.ps1`, `sync_data.ps1` (push `data/` over
  HTTP `/upload`), `sync_data.sh` (bash mirror).
- `mock-server/` — Node mock of `/ws` + `/upload` for offline UI work
  (`npm run dev` on `:8765`).
- `usb_drive.h` — Hold-A-at-boot USB-MSC mode. Coexists with the live
  `/upload` flow; do not replace one with the other.
- `ui-upload.hpp` — on-device "FILE UPLOAD" progress screen, driven by the
  `/upload` handler in `game-server.hpp` and rendered by the LCD refresh path
  in `Esp32HexMapCrawl.ino`.

If you're about to add a new firmware screen, route, or deploy script, check
`docs/dev-loop.md` first — most of the workflow is already wired and the doc
calls out the gotchas (manifest skip, PSRAM cache, AsyncTCP task isolation).
