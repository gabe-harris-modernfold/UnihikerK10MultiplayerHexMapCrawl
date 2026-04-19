#pragma once

#include <LovyanGFX.hpp>

// ======================================================
// LovyanGFX hardware configuration for ILI9341 on FSPI
// ESP32-S3: FSPI = SPI2_HOST, DMA enabled
// Unihiker K10 pin assignments (hardcoded — same as sensor node)
// ======================================================

// ILI9341 SPI pins
static constexpr int LGFX_TFT_CS   = 14;
static constexpr int LGFX_TFT_DC   = 13;
static constexpr int LGFX_TFT_MOSI = 21;
static constexpr int LGFX_TFT_SCLK = 12;
static constexpr int LGFX_TFT_MISO = -1;  // no MISO on this board

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;

public:
  LGFX() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;        // FSPI on ESP32-S3
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;         // 40 MHz
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = LGFX_TFT_SCLK;
      cfg.pin_mosi    = LGFX_TFT_MOSI;
      cfg.pin_miso    = LGFX_TFT_MISO;
      cfg.pin_dc      = LGFX_TFT_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs          = LGFX_TFT_CS;
      cfg.pin_rst         = -1;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable        = false;
      cfg.invert          = false;
      cfg.rgb_order       = false;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};
