#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>

// Rainbow: per-face radial cycle. Hue sweeps from the outer ring inward,
// giving 4 concentric rings (dist 0=outer, 1, 2, 3=center 2x2).

static float s_phase;

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h = h - floorf(h);
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    int seg = (int)(h * 6.0f);
    switch (seg) {
        case 0: rp = c; gp = x; bp = 0; break;
        case 1: rp = x; gp = c; bp = 0; break;
        case 2: rp = 0; gp = c; bp = x; break;
        case 3: rp = 0; gp = x; bp = c; break;
        case 4: rp = x; gp = 0; bp = c; break;
        default:rp = c; gp = 0; bp = x; break;
    }
    *r = (uint8_t)((rp + m) * 255);
    *g = (uint8_t)((gp + m) * 255);
    *b = (uint8_t)((bp + m) * 255);
}

static void rainbow_enter(void) { s_phase = 0; }

static void rainbow_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    s_phase += dt * (speed / 255.0f + 0.1f) * 0.5f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                // Ring index 0..3 = distance to nearest edge.
                int d = x;
                if (7 - x < d) d = 7 - x;
                if (y < d) d = y;
                if (7 - y < d) d = 7 - y;
                // Hue: ring 0 is phase, ring 3 is phase + 0.25.
                float h = s_phase + d * 0.08f + f * 0.04f;
                uint8_t r, g, b;
                hsv_to_rgb(h, 1.0f, 1.0f, &r, &g, &b);
                render_set((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

const effect_vtable_t g_effect_rainbow = {
    .name = "rainbow", .id = EFFECT_RAINBOW, .enter = rainbow_enter, .step = rainbow_step,
};
