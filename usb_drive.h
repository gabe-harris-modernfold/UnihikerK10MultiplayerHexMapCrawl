// usb_drive.h — USB Mass Storage mode for WASTELAND CRAWL
// Hold Button A during the SD-mount splash to expose the SD card as a USB drive.
// Never returns; the game does not start in this mode.
// Reconnect USB cable after reboot to return to game mode.
//
// HOW IT WORKS
//   SD.begin() registers the SD card with FatFS as physical drive 0.
//   The TinyUSB MSC class uses disk_read(0,...) / disk_write(0,...) to give the
//   USB host raw sector-level access to that same drive — no double-init needed.
//   Both CDC (Serial) and MSC are enabled in the UNIHIKER sdkconfig, so the USB
//   device reconnects as a CDC+MSC composite after MSC.begin() is called.

#pragma once

#include "USB.h"
#include "USBMSC.h"
// ff.h defines BYTE/DWORD/UINT/DRESULT; diskio.h uses them but doesn't include ff.h itself.
// Both headers have their own __cplusplus guards, so no outer extern "C" needed.
#include "ff.h"       // FatFS types: BYTE, DWORD, UINT, DRESULT …
#include "diskio.h"   // disk_read, disk_write, disk_ioctl, GET_SECTOR_COUNT

static USBMSC _msc;   // global — TinyUSB registers MSC interface on begin()

// ── FatFS disk-layer callbacks ────────────────────────────────────────────────
static int32_t _mscRead(uint32_t lba, uint32_t /*offset*/, void* buf, uint32_t sz) {
  DRESULT r = disk_read(0, (BYTE*)buf, (DWORD)lba, (UINT)(sz / 512));
  return (r == RES_OK) ? (int32_t)sz : -1;
}
static int32_t _mscWrite(uint32_t lba, uint32_t /*offset*/, uint8_t* buf, uint32_t sz) {
  DRESULT r = disk_write(0, (const BYTE*)buf, (DWORD)lba, (UINT)(sz / 512));
  return (r == RES_OK) ? (int32_t)sz : -1;
}
static bool _mscStartStop(uint8_t /*power*/, bool /*start*/, bool /*eject*/) {
  return true;
}

// Call AFTER SD.begin() succeeded and k10 screen is ready.  Never returns.
static void enterUSBDriveMode(UNIHIKER_K10& k10) {
  // ── Get total sector count via FatFS diskio ───────────────
  DWORD sectors = 0;
  disk_ioctl(0, GET_SECTOR_COUNT, &sectors);
  uint32_t mb = (uint32_t)(sectors / 2048UL);   // 512-byte sectors → MiB

  // ── Configure and start TinyUSB MSC ──────────────────────
  _msc.vendorID("WASTE");
  _msc.productID("SD Card");
  _msc.productRevision("1.0");
  _msc.onRead(_mscRead);
  _msc.onWrite(_mscWrite);
  _msc.onStartStop(_mscStartStop);
  _msc.mediaPresent(true);
  _msc.begin(sectors, 512);   // triggers USB disconnect+reconnect as CDC+MSC
  USB.begin();

  // ── K10 status screen ────────────────────────────────────
  k10.canvas->canvasRectangle(0, 0, 240, 320, 0x000000, 0x000000, true);
  k10.canvas->canvasSetLineWidth(2);
  k10.canvas->canvasLine(16,  56, 224,  56, 0x004080);
  k10.canvas->canvasText("USB DRIVE", 44, 64, 0x00C0FF, Canvas::eCNAndENFont24, 50, false);
  k10.canvas->canvasLine(16, 100, 224, 100, 0x004080);
  k10.canvas->canvasSetLineWidth(1);
  char szBuf[30];
  snprintf(szBuf, 30, "SD card  %u MB", (unsigned)mb);
  k10.canvas->canvasText(szBuf, 8, 112, 0x40A0C0, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("Drive visible on PC",  8, 136, 0x60C060, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(16, 162, 224, 162, 0x202020);
  k10.canvas->canvasText("Copy /data/ folder",   8, 172, 0x707070, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("Safely eject on PC",   8, 192, 0x707070, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("Then reboot K10",      8, 212, 0x707070, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasLine(16, 238, 224, 238, 0x181818);
  k10.canvas->canvasText("/data/index.html",     8, 248, 0x404050, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("/data/*.css  *.js",    8, 266, 0x404050, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->canvasText("/data/img/hex*.png",   8, 284, 0x404050, Canvas::eCNAndENFont16, 50, false);
  k10.canvas->updateCanvas();

  Serial.printf("[USB] Drive mode. %u sectors  %u MB. Waiting for host.\n",
                (unsigned)sectors, (unsigned)mb);
  for (;;) delay(1000);   // TinyUSB is serviced by background tasks
}
