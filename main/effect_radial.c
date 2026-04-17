#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>

// Radial wave: a color band flows out from the TOP face's center, spreads
// across the 4 side faces (every side simultaneously), then converges into
// the BOTTOM face's center. Implemented as a per-pixel geodesic "distance
// from TOP center" field; hue = phase - d * k, so as phase advances, the
// color band moves outward and the rainbow crawls in the same direction.

// Distance zones, measured in pixels along the cube surface:
//   TOP     :   d ~ 0..5    (euclidean from center 3.5, 3.5)
//   SIDES   :   d ~ 4..11   (distance from top edge + 4 buffer)
//   BOTTOM  :   d ~ 12..16  (12 at edge, +4 at center)

static float s_dist[CUBE_FACE_COUNT][8][8];
static float s_phase;

static float max_dist = 1.0f;

static float dist_top(int x, int y) {
    float dx = x - 3.5f, dy = y - 3.5f;
    return sqrtf(dx * dx + dy * dy);
}

static float dist_bottom(int x, int y) {
    // Invert: closer to center is farther from TOP center.
    float dx = x - 3.5f, dy = y - 3.5f;
    float d_center = sqrtf(dx * dx + dy * dy);
    return 12.0f + (5.0f - d_center);
}

static float dist_side(int x, int y) {
    (void)x;
    // Approximate: linear distance down the face.
    return 4.0f + (float)y;
}

static void precompute_distances(void) {
    max_dist = 0.0f;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float d;
                if (f == FACE_TOP)         d = dist_top(x, y);
                else if (f == FACE_BOTTOM) d = dist_bottom(x, y);
                else                        d = dist_side(x, y);
                s_dist[f][x][y] = d;
                if (d > max_dist) max_dist = d;
            }
        }
    }
}

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

static void radial_enter(void) {
    precompute_distances();
    s_phase = 0;
}

static void radial_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed; // reuse rainbow_speed knob
    config_unlock();

    s_phase += dt * (speed / 255.0f + 0.2f) * 0.4f;

    // Visible wavelength: we want a full color cycle to span ~10 pixels so the
    // user clearly sees the band emerging from the top center.
    const float k = 1.0f / 10.0f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float d = s_dist[f][x][y];
                float h = s_phase - d * k;
                uint8_t r, g, b;
                hsv_to_rgb(h, 1.0f, 1.0f, &r, &g, &b);
                render_set((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

const effect_vtable_t g_effect_radial = {
    .name = "radial", .id = EFFECT_RADIAL,
    .enter = radial_enter, .step = radial_step,
};
