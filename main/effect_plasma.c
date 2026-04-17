#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>

// Plasma: classic interference pattern. Three sine fields combine into a
// scalar that maps to HSV hue. Each face adds a small bias so the pattern
// seams don't look identical across faces (keeps the cube feeling "alive").

static float s_t;

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

static void plasma_enter(void) { s_t = 0; }

static void plasma_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    s_t += dt * (0.2f + speed / 255.0f * 0.8f);

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        float fbias = f * 0.7f;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float fx = x - 3.5f, fy = y - 3.5f;
                float p1 = sinf(fx * 0.65f + s_t);
                float p2 = sinf(fy * 0.55f + s_t * 1.3f + fbias);
                float p3 = sinf(sqrtf(fx*fx + fy*fy) * 0.50f - s_t * 0.9f);
                float v = (p1 + p2 + p3) / 6.0f + 0.5f;  // 0..1
                uint8_t r, g, b;
                hsv_to_rgb(v, 1.0f, 1.0f, &r, &g, &b);
                render_set((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

const effect_vtable_t g_effect_plasma = {
    .name = "plasma", .id = EFFECT_PLASMA,
    .enter = plasma_enter, .step = plasma_step,
};
