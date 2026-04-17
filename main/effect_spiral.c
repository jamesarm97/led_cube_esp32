#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>

// Per-face spiral: each face walks a 64-cell outer-to-inner spiral path.
// Four nested rectangular rings of widths 8, 6, 4, 2. A bright head moves
// along this path with a fading rainbow tail; on reaching the center 2x2,
// the whole thing pauses briefly, then restarts at the outer ring with a
// fresh hue offset. Each face is staggered so they don't all hit the
// center at the same moment.

#define SPIRAL_LEN    64
#define SPIRAL_TAIL   14

typedef struct { int8_t x, y; } face_cell_t;

static face_cell_t s_path[SPIRAL_LEN];
static float       s_head[CUBE_FACE_COUNT];
static float       s_pause[CUBE_FACE_COUNT];
static float       s_hue_seed[CUBE_FACE_COUNT];

static const float PAUSE_SECONDS = 0.35f;

static void build_spiral(void) {
    int i = 0;
    for (int r = 0; r < 4; r++) {
        int lo = r, hi = 7 - r;
        // Top row L→R
        for (int x = lo; x <= hi; x++) s_path[i++] = (face_cell_t){(int8_t)x, (int8_t)lo};
        // Right column T→B (skip top-right; it was just drawn)
        for (int y = lo + 1; y <= hi; y++) s_path[i++] = (face_cell_t){(int8_t)hi, (int8_t)y};
        // Bottom row R→L (only if ring is at least 2 rows tall)
        if (hi > lo)
            for (int x = hi - 1; x >= lo; x--) s_path[i++] = (face_cell_t){(int8_t)x, (int8_t)hi};
        // Left column B→T (only if ring has an interior column)
        if (hi > lo + 1)
            for (int y = hi - 1; y >= lo + 1; y--) s_path[i++] = (face_cell_t){(int8_t)lo, (int8_t)y};
    }
    // 28 + 20 + 12 + 4 == 64 cells; caller relies on SPIRAL_LEN.
}

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b) {
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

static void spiral_enter(void) {
    build_spiral();
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        // Stagger each face through a different lifecycle phase so they
        // aren't all at the same radius.
        s_head[f]     = f * ((SPIRAL_LEN + SPIRAL_TAIL) / (float)CUBE_FACE_COUNT);
        s_pause[f]    = 0;
        s_hue_seed[f] = f * 0.16f;
    }
}

static void spiral_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    // Step rate — spiral covers 64 cells in ~2-4 seconds depending on knob.
    float px_per_sec = 15.0f + speed * 0.20f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        if (s_pause[f] > 0) {
            s_pause[f] -= dt;
            if (s_pause[f] <= 0) {
                s_head[f]     = 0;
                s_hue_seed[f] += 0.17f; // shift palette between laps
                s_pause[f]    = 0;
            }
            continue;
        }
        s_head[f] += dt * px_per_sec;
        if (s_head[f] >= SPIRAL_LEN + SPIRAL_TAIL) {
            s_pause[f] = PAUSE_SECONDS;
            continue;
        }
        int head = (int)s_head[f];
        for (int i = 0; i < SPIRAL_TAIL; i++) {
            int idx = head - i;
            if (idx < 0 || idx >= SPIRAL_LEN) continue;
            face_cell_t c = s_path[idx];
            float bright = (SPIRAL_TAIL - i) / (float)SPIRAL_TAIL;
            float hue = s_hue_seed[f] + idx / (float)SPIRAL_LEN;
            uint8_t r, g, b;
            hsv_to_rgb(hue, 1.0f, bright, &r, &g, &b);
            render_add((cube_face_t)f, c.x, c.y, r, g, b);
        }
    }
}

const effect_vtable_t g_effect_spiral = {
    .name = "spiral", .id = EFFECT_SPIRAL,
    .enter = spiral_enter, .step = spiral_step,
};
