#include "render.h"
#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "led_strip.h"
#include "driver/rmt_tx.h"

static const char *TAG = "render";

// Two framebuffers:
//   s_fb  — written by effects in 8-bit sRGB (face,x,y).
// We translate directly through cube_logical_to_strip() at flush time.
static uint8_t s_fb[CUBE_TOTAL_PIXELS][3];

static led_strip_handle_t s_strip;

void render_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

void render_set(cube_face_t face, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    int idx = cube_logical_to_strip(face, x, y);
    if (idx < 0) return;
    s_fb[idx][0] = r; s_fb[idx][1] = g; s_fb[idx][2] = b;
}

static inline uint8_t sat_add(uint8_t a, uint8_t b) {
    unsigned s = (unsigned)a + b;
    return s > 255 ? 255 : (uint8_t)s;
}

void render_add(cube_face_t face, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    int idx = cube_logical_to_strip(face, x, y);
    if (idx < 0) return;
    s_fb[idx][0] = sat_add(s_fb[idx][0], r);
    s_fb[idx][1] = sat_add(s_fb[idx][1], g);
    s_fb[idx][2] = sat_add(s_fb[idx][2], b);
}

void render_fill_face(cube_face_t face, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            render_set(face, x, y, r, g, b);
}

void render_fill_physical_panel(int phys, uint8_t r, uint8_t g, uint8_t b) {
    if (phys < 0 || phys >= CUBE_FACE_COUNT) return;
    int base = phys * CUBE_PANEL_PIXELS;
    for (int i = 0; i < CUBE_PANEL_PIXELS; i++) {
        s_fb[base + i][0] = r; s_fb[base + i][1] = g; s_fb[base + i][2] = b;
    }
}

// Approximate per-channel current for WS2812 at full brightness: ~20mA per
// channel at 255, so a white pixel = 60mA. Sum R+G+B * factor and compare to
// max_ma, scaling if over.
static float compute_current_scale(uint8_t brightness, uint32_t max_ma) {
    uint64_t sum = 0;
    for (int i = 0; i < CUBE_TOTAL_PIXELS; i++) {
        sum += s_fb[i][0];
        sum += s_fb[i][1];
        sum += s_fb[i][2];
    }
    // Each unit on a channel at brightness=255 ~ (20.0/255) mA.
    // With brightness scale b/255, current ~ sum * (b/255) * (20/255) mA.
    float current_ma = ((float)sum * brightness * 20.0f) / (255.0f * 255.0f);
    if (current_ma <= (float)max_ma) return 1.0f;
    return (float)max_ma / current_ma;
}

void render_flush(void) {
    config_lock();
    uint8_t brightness = config_get()->brightness;
    uint32_t max_ma    = config_get()->max_ma;
    config_unlock();

    float cap = compute_current_scale(brightness, max_ma);
    float scale = ((float)brightness / 255.0f) * cap;

    for (int i = 0; i < CUBE_TOTAL_PIXELS; i++) {
        uint8_t r = (uint8_t)(s_fb[i][0] * scale);
        uint8_t g = (uint8_t)(s_fb[i][1] * scale);
        uint8_t b = (uint8_t)(s_fb[i][2] * scale);
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}

void render_start(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CUBE_DATA_GPIO,
        .max_leds       = CUBE_TOTAL_PIXELS,
        .led_model      = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = { .invert_out = 0 },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .mem_block_symbols = 64,
        .flags = { .with_dma = 0 },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "LED strip initialized on GPIO%d, %d pixels", CUBE_DATA_GPIO, CUBE_TOTAL_PIXELS);
}
