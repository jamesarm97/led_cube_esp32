#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// Chase: three rainbow snakes run simultaneously on three orthogonal "great
// circles" around the cube. Each path is a closed 32-pixel loop:
//
//   EQUATOR   : NORTH -> WEST  -> SOUTH  -> EAST    (y=4 on every side)
//   MERIDIAN1 : NORTH -> BOTTOM -> SOUTH -> TOP      (through north/south poles)
//   MERIDIAN2 : EAST  -> BOTTOM -> WEST  -> TOP      (through east/west poles)
//
// Each snake has its own hue offset and advances at a slightly different
// speed so they drift in and out of phase — crossing points brighten
// additively, which is the whole point of the effect.

typedef struct { cube_face_t face; int8_t x, y; } path_point_t;

#define PATH_COUNT   3
#define PATH_LEN     32
#define SNAKE_LEN    9

static path_point_t s_paths[PATH_COUNT][PATH_LEN];
static float        s_heads[PATH_COUNT];
static const float  s_speeds[PATH_COUNT]   = {1.00f, 1.12f, 0.91f}; // relative
static const uint8_t s_hue_base[PATH_COUNT] = {  0,  85, 170 };      // 0/120/240°

static void build_paths(void) {
    int i;

    // --- Equator: y=4 on NORTH, WEST, SOUTH, EAST (in that traversal order).
    i = 0;
    const cube_face_t eq_order[4] = {FACE_NORTH, FACE_WEST, FACE_SOUTH, FACE_EAST};
    for (int f = 0; f < 4; f++)
        for (int x = 0; x < 8; x++)
            s_paths[0][i++] = (path_point_t){eq_order[f], (int8_t)x, 4};

    // --- Meridian 1 (through NORTH-pole and SOUTH-pole).
    // Walk:   NORTH(3, 0..7) -> BOTTOM(3, 7..0) -> SOUTH(4, 7..0) -> TOP(4, 7..0)
    // These coordinates were derived from the cube_adj flip/neighbor table so
    // that adjacent points are edge-adjacent pixels on the cube surface.
    i = 0;
    for (int y = 0; y < 8; y++)  s_paths[1][i++] = (path_point_t){FACE_NORTH,  3, (int8_t)y};
    for (int y = 7; y >= 0; y--) s_paths[1][i++] = (path_point_t){FACE_BOTTOM, 3, (int8_t)y};
    for (int y = 7; y >= 0; y--) s_paths[1][i++] = (path_point_t){FACE_SOUTH,  4, (int8_t)y};
    for (int y = 7; y >= 0; y--) s_paths[1][i++] = (path_point_t){FACE_TOP,    4, (int8_t)y};

    // --- Meridian 2 (through EAST-pole and WEST-pole).
    // Walk:   EAST(3, 0..7) -> BOTTOM(7..0, 3) -> WEST(4, 7..0) -> TOP(0..7, 4)
    i = 0;
    for (int y = 0; y < 8; y++)  s_paths[2][i++] = (path_point_t){FACE_EAST,   3, (int8_t)y};
    for (int x = 7; x >= 0; x--) s_paths[2][i++] = (path_point_t){FACE_BOTTOM, (int8_t)x, 3};
    for (int y = 7; y >= 0; y--) s_paths[2][i++] = (path_point_t){FACE_WEST,   4, (int8_t)y};
    for (int x = 0; x < 8; x++)  s_paths[2][i++] = (path_point_t){FACE_TOP,    (int8_t)x, 4};
}

static void wheel(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (pos < 85)       { *r = 255 - pos*3; *g = pos*3;        *b = 0; }
    else if (pos < 170) { pos -= 85; *r = 0; *g = 255 - pos*3; *b = pos*3; }
    else                { pos -= 170; *r = pos*3; *g = 0;      *b = 255 - pos*3; }
}

static void chase_enter(void) {
    build_paths();
    for (int p = 0; p < PATH_COUNT; p++) s_heads[p] = p * (PATH_LEN / (float)PATH_COUNT);
}

static void chase_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    float base_px_per_sec = 5.0f + speed * 0.10f; // ~5..30 px/s

    for (int p = 0; p < PATH_COUNT; p++) {
        s_heads[p] += dt * base_px_per_sec * s_speeds[p];
        while (s_heads[p] >= PATH_LEN) s_heads[p] -= PATH_LEN;

        for (int i = 0; i < SNAKE_LEN; i++) {
            float idx = s_heads[p] - i;
            while (idx < 0) idx += PATH_LEN;
            int ii = (int)idx;
            const path_point_t *pt = &s_paths[p][ii % PATH_LEN];
            float bright = (SNAKE_LEN - i) / (float)SNAKE_LEN;
            uint8_t r, g, b;
            // Shift hue along body so the snake itself rainbows.
            wheel((uint8_t)(s_hue_base[p] + ii * 6), &r, &g, &b);
            render_add(pt->face, pt->x, pt->y,
                       (uint8_t)(r * bright),
                       (uint8_t)(g * bright),
                       (uint8_t)(b * bright));
        }
    }
}

const effect_vtable_t g_effect_chase = {
    .name = "chase", .id = EFFECT_CHASE,
    .enter = chase_enter, .step = chase_step,
};
