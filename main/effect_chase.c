#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "esp_random.h"

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

#define PATH_COUNT   6
#define PATH_LEN     32
#define SNAKE_LEN    9

// Each path is one of three kinds: equator, meridian-1 (N/S poles),
// meridian-2 (E/W poles). We run two of each kind at different starting
// offsets / hues so the cube always has six simultaneous snakes wandering
// around. Each path drifts independently via drift_offset() on every lap,
// so identical-kind paths diverge over time.
typedef enum { KIND_EQUATOR = 0, KIND_MERIDIAN_1 = 1, KIND_MERIDIAN_2 = 2 } path_kind_t;

static const path_kind_t s_path_kind[PATH_COUNT] = {
    KIND_EQUATOR, KIND_MERIDIAN_1, KIND_MERIDIAN_2,
    KIND_EQUATOR, KIND_MERIDIAN_1, KIND_MERIDIAN_2,
};

static path_point_t s_paths[PATH_COUNT][PATH_LEN];
static float        s_heads[PATH_COUNT];
static const float   s_speeds[PATH_COUNT]    = {1.00f, 1.12f, 0.91f, 1.07f, 0.94f, 1.18f};
static const uint8_t s_hue_base[PATH_COUNT]  = {   0,   85,  170,   43,  128,  213};
static const uint8_t s_init_offsets[PATH_COUNT] = {   4,    4,    3,    1,    2,    5};

// Per-path offset in [0, 7]. See build_path() for what each offset means
// for each path kind.
static uint8_t s_offsets[PATH_COUNT];

// Rebuild one path using its current s_offsets[] entry. Paths are traced
// against the corrected s_adj in cube.c so successive points are always
// edge-adjacent on the cube surface. Flip comments record the math that
// connects each face's offset expression to the next.
static void build_path(int p) {
    int i = 0;
    int o = s_offsets[p];

    switch (s_path_kind[p]) {
    case KIND_EQUATOR: {
        // --- Equator: constant y across all 4 side faces.
        //   NORTH(7,y) ↔ WEST(7,y) via EDGE_RIGHT↔EDGE_RIGHT (no flip)
        //   WEST(0,y)  ↔ SOUTH(0,y) via EDGE_LEFT↔EDGE_LEFT  (no flip)
        //   SOUTH(7,y) ↔ EAST(7,y)  via EDGE_RIGHT↔EDGE_RIGHT (no flip)
        //   EAST(0,y)  ↔ NORTH(0,y) via EDGE_LEFT↔EDGE_LEFT  (no flip)
        // y is shared; offset = y on every side face. WEST and EAST are
        // walked in reverse x to keep direction continuous.
        int8_t y = (int8_t)o;
        for (int x = 0; x < 8; x++)  s_paths[p][i++] = (path_point_t){FACE_NORTH, (int8_t)x, y};
        for (int x = 7; x >= 0; x--) s_paths[p][i++] = (path_point_t){FACE_WEST,  (int8_t)x, y};
        for (int x = 0; x < 8; x++)  s_paths[p][i++] = (path_point_t){FACE_SOUTH, (int8_t)x, y};
        for (int x = 7; x >= 0; x--) s_paths[p][i++] = (path_point_t){FACE_EAST,  (int8_t)x, y};
        break;
    }
    case KIND_MERIDIAN_1: {
        // --- Meridian 1 (through NORTH-pole and SOUTH-pole).
        //   NORTH(x_n,7) ↔ BOTTOM(7-x_n,7)  (EDGE_BOTTOM↔EDGE_BOTTOM, flip=true)
        //   BOTTOM(x,0)  ↔ SOUTH(x,7)       (EDGE_TOP↔EDGE_BOTTOM, flip=false)
        //   SOUTH(x,0)   ↔ TOP(x,7)         (EDGE_TOP↔EDGE_BOTTOM, flip=false)
        //   TOP(x,0)     ↔ NORTH(7-x,0)     (EDGE_TOP↔EDGE_TOP, flip=true)
        // So BOTTOM/SOUTH/TOP share column x = o; NORTH column = 7-o.
        int8_t m   = (int8_t)o;
        int8_t m_n = (int8_t)(7 - o);
        for (int y = 0; y < 8; y++)  s_paths[p][i++] = (path_point_t){FACE_NORTH,  m_n, (int8_t)y};
        for (int y = 7; y >= 0; y--) s_paths[p][i++] = (path_point_t){FACE_BOTTOM, m,   (int8_t)y};
        for (int y = 7; y >= 0; y--) s_paths[p][i++] = (path_point_t){FACE_SOUTH,  m,   (int8_t)y};
        for (int y = 7; y >= 0; y--) s_paths[p][i++] = (path_point_t){FACE_TOP,    m,   (int8_t)y};
        break;
    }
    case KIND_MERIDIAN_2: {
        // --- Meridian 2 (through EAST-pole and WEST-pole).
        //   EAST(x_e,7)  ↔ BOTTOM(7,7-x_e) (EDGE_BOTTOM↔EDGE_RIGHT, flip=true)
        //   BOTTOM(0,y)  ↔ WEST(y,7)       (EDGE_LEFT↔EDGE_BOTTOM, flip=false)
        //   WEST(x_w,0)  ↔ TOP(0,7-x_w)    (EDGE_TOP↔EDGE_LEFT, flip=true)
        //   TOP(7,y)     ↔ EAST(y,0)       (EDGE_RIGHT↔EDGE_TOP, flip=false)
        // offset = EAST x = TOP y; BOTTOM y = WEST x = 7 - offset.
        int8_t m    = (int8_t)o;
        int8_t m_bw = (int8_t)(7 - o);
        for (int y = 0; y < 8; y++)  s_paths[p][i++] = (path_point_t){FACE_EAST,   m,    (int8_t)y};
        for (int x = 7; x >= 0; x--) s_paths[p][i++] = (path_point_t){FACE_BOTTOM, (int8_t)x, m_bw};
        for (int y = 7; y >= 0; y--) s_paths[p][i++] = (path_point_t){FACE_WEST,   m_bw, (int8_t)y};
        for (int x = 0; x < 8; x++)  s_paths[p][i++] = (path_point_t){FACE_TOP,    (int8_t)x, m};
        break;
    }
    }
}

static void build_paths(void) {
    for (int p = 0; p < PATH_COUNT; p++) build_path(p);
}

// Shift one path's offset by ±1 or ±2 (wrapping [0, 7]). Called each time a
// snake completes a full lap so the lines drift around the face instead of
// sitting on the same row/column forever.
static void drift_offset(int p) {
    static const int8_t STEPS[] = {-2, -1, 1, 2};
    int step = STEPS[esp_random() & 3];
    int o = (int)s_offsets[p] + step;
    o = ((o % 8) + 8) % 8;
    s_offsets[p] = (uint8_t)o;
    build_path(p);
}

static void wheel(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (pos < 85)       { *r = 255 - pos*3; *g = pos*3;        *b = 0; }
    else if (pos < 170) { pos -= 85; *r = 0; *g = 255 - pos*3; *b = pos*3; }
    else                { pos -= 170; *r = pos*3; *g = 0;      *b = 255 - pos*3; }
}

// Per-path animation state. Each snake runs a linear lifecycle rather than
// wrapping: head_pos increases from 0 to PATH_LEN + SNAKE_LEN, at which
// point the whole body has walked off the end of the path. We then pause
// briefly, drift the offset, rebuild the path, and reset head_pos to 0 for
// the next lap. This avoids the mid-panel jump that happened when we
// shifted the offset instantly on wrap.
typedef struct {
    float head_pos;    // 0 .. PATH_LEN + SNAKE_LEN; >= that => lap done
    float pause_left;  // seconds remaining in inter-lap pause; 0 = running
} snake_t;

static snake_t s_snakes[PATH_COUNT];

static const float PAUSE_SECONDS = 0.35f; // gap between laps

static void chase_enter(void) {
    // Starting offsets match the original hard-coded paths so the first
    // lap looks the same as before; subsequent laps drift via drift_offset.
    //   path 0 equator   : y = 4
    //   path 1 meridian 1: BOTTOM/SOUTH/TOP x = 4  (-> NORTH x = 3)
    //   path 2 meridian 2: EAST x = TOP y = 3      (-> BOTTOM y = WEST x = 4)
    for (int p = 0; p < PATH_COUNT; p++) s_offsets[p] = s_init_offsets[p];
    build_paths();

    // Stagger the three snakes through different points in their lifecycle
    // so they don't all hit the pause / restart boundary simultaneously.
    for (int p = 0; p < PATH_COUNT; p++) {
        s_snakes[p].head_pos   = p * ((PATH_LEN + SNAKE_LEN) / (float)PATH_COUNT);
        s_snakes[p].pause_left = 0;
    }
    // s_heads[] is no longer used; keep the array populated for ABI safety.
    for (int p = 0; p < PATH_COUNT; p++) s_heads[p] = s_snakes[p].head_pos;
}

static void chase_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    float base_px_per_sec = 5.0f + speed * 0.10f; // ~5..30 px/s

    for (int p = 0; p < PATH_COUNT; p++) {
        snake_t *s = &s_snakes[p];

        // Pause phase — countdown, then restart with new offset/path.
        if (s->pause_left > 0) {
            s->pause_left -= dt;
            if (s->pause_left <= 0) {
                drift_offset(p);   // pick a new row/column for next lap
                s->head_pos   = 0; // head re-enters at path[0] alone
                s->pause_left = 0;
            }
            continue; // nothing drawn while paused
        }

        // Active phase — head advances linearly along the path.
        s->head_pos += dt * base_px_per_sec * s_speeds[p];
        if (s->head_pos >= PATH_LEN + SNAKE_LEN) {
            // Tail just exited the end of the path: lap is fully done.
            s->pause_left = PAUSE_SECONDS;
            continue;
        }

        // Draw body: cells at head_pos-i for i in [0, SNAKE_LEN). Cells with
        // idx < 0 (head still entering) or idx >= PATH_LEN (body exited at
        // the end of path) simply aren't drawn — that's the natural
        // "enter from first face, exit past last face" behavior.
        int head = (int)s->head_pos;
        for (int i = 0; i < SNAKE_LEN; i++) {
            int idx = head - i;
            if (idx < 0 || idx >= PATH_LEN) continue;
            const path_point_t *pt = &s_paths[p][idx];
            float bright = (SNAKE_LEN - i) / (float)SNAKE_LEN;
            uint8_t r, g, b;
            wheel((uint8_t)(s_hue_base[p] + idx * 6), &r, &g, &b);
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
