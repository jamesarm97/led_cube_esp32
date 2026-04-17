#pragma once
// Board-level hardware configuration. Edit these for your wiring.
//
// Target: ESP32-S3 N4R2 (4MB flash, 2MB quad PSRAM) + one WS2812 data line
// driving 6 daisy-chained 8x8 panels (384 LEDs total).

// GPIO that drives the first LED in the chain. Pick a pin that is:
//   - safe at boot (not a strapping pin like GPIO0/45/46),
//   - not used by USB-Serial/JTAG (GPIO19/20 on S3),
//   - not consumed by octal PSRAM (GPIO33..37 on N8R8; unused on N4R2).
// GPIO48 is the on-board RGB LED on most S3 DevKit-C boards and works fine
// as an external data line too.
#define CUBE_DATA_GPIO   4
