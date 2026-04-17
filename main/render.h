#pragma once
// Framebuffer + WS2812 render task.
//
// Effects draw into a float RGB framebuffer keyed on (face, x, y). A dedicated
// FreeRTOS task flushes the framebuffer to the physical strip at ~60Hz using
// the led_strip RMT driver, applying global brightness and a simple per-frame
// current cap.

#include <stdint.h>
#include "cube.h"
#include "hardware.h"  // CUBE_DATA_GPIO and other board pin assignments

// Pixel write helpers — thread-safe to call from the effect task only.
void render_clear(void);
void render_set(cube_face_t face, int x, int y, uint8_t r, uint8_t g, uint8_t b);
void render_add(cube_face_t face, int x, int y, uint8_t r, uint8_t g, uint8_t b);
void render_fill_face(cube_face_t face, uint8_t r, uint8_t g, uint8_t b);

// Directly set a physical panel by index (used for face-ID calibration only).
void render_fill_physical_panel(int phys, uint8_t r, uint8_t g, uint8_t b);

// Request a flush to the LED strip. Called by effect loop once per frame.
void render_flush(void);

// Start the driver. Must be called once after config_init().
void render_start(void);
