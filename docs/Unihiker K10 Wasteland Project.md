# Unihiker K10 Wasteland Project

An embedded systems example for the **Unihiker K10** device (ESP32-S3) that provides modular hardware abstraction and 10 independent component tests. Designed for developers to explore sensor and application logic without wrestling with low-level hardware details.

### I2C Sensors (GPIO 47 SDA / 48 SCL)
- `aht20.h` — Temperature & humidity (address: 0x38)
- `ltr303.h` — Ambient light (address: 0x29)
- `sc7a20h.h` — 3-axis accelerometer (address: 0x19)
- `xl9535.h` / `xl9535_test.h` — GPIO expander for buttons & backlight (address: 0x20)

## Critical Hardware Constraints

⚠️ **GPIO 38**: Shared between **I2S LRCK** and **Font Chip CS** — cannot use audio and font ROM simultaneously

⚠️ **GPIO 40**: Shared between **SD_CS** and **Font Chip CS** with NPN inverter — HIGH selects font, LOW selects SD

⚠️ **I2C Bus** (GPIO 47/48): Multiple devices share this bus; camera driver temporarily removes/recreates it during init/deinit

These constraints are handled in the code, but important to know when extending functionality.

## Hardware Specs

- **MCU**: ESP32-S3 (240 MHz dual-core)
- **Display**: ILI9341 TFT 240×320 @ 40 MHz (FSPI, DMA)
- **Camera**: GC2145 (240×176 HQVGA, DVP parallel 8-bit)
- **Audio**: I2S TX (44.1 kHz speaker) / RX (16 kHz dual mic)
- **Storage**: Micro SD (HSPI) + 24 Mb Font ROM (SPI)
- **Sensors**: Temp/humidity, light, 3-axis accel
- **I2C**: 6 devices on shared GPIO 47/48
- **Buttons**: 2× GPIO via XL9535 expander
- **RAM**: 512 KB SRAM (+ PSRAM for canvas)

## Resources

- **LovyanGFX Library**: https://github.com/lovyan03/LovyanGFX
- **Adafruit_NeoPixel**: https://github.com/adafruit/Adafruit_NeoPixel
- **ESP32 Arduino Core**: https://github.com/espressif/arduino-esp32