#include "effects.h"
#include "render.h"
#include "cube.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_random.h"

// Rain: blue "clouds" form on the TOP face, drops fall off the TOP edges and
// continue down the side faces (N/S/E/W), then pool on the BOTTOM face and up
// to ~2 rows of the side faces near the floor.
//
// Model:
//   - drop_t[]: active drops. Each drop has (face, x, y, age).
//     On TOP face it moves randomly one step per tick choosing the "south-ish"
//     direction tied to which edge it will fall off. For simplicity, we spawn
//     drops on TOP with a preferred edge (T_TOP/T_BOTTOM/T_LEFT/T_RIGHT) and
//     walk one pixel per `rain_speed`-tick toward that edge, then step over
//     onto the side face via cube_step_over_edge().
//   - On a side face, drops fall straight down (y++) until they hit the
//     BOTTOM face. They then become a puddle pixel on BOTTOM.
//   - Puddle: a per-face framebuffer `puddle[face][x][y]` in 0..255. It ages
//     down each frame. Primarily BOTTOM, plus the bottom 2 rows of N/S/E/W.
//
// Colors: deep blue on TOP (clouds), medium blue for falling drops, light
// blue for puddles so each layer reads distinctly.

#define MAX_DROPS 80

typedef struct {
    cube_face_t face;
    int8_t x, y;
    uint8_t fall_counter;      // advances each frame; falls when it hits threshold
    cube_edge_t target_edge;   // only used while on TOP
    bool on_top;
    bool alive;
} drop_t;

static drop_t s_drops[MAX_DROPS];

// Puddles: small value per (face,x,y). Only BOTTOM and the bottom 2 rows of
// side faces ever receive puddle energy, but we keep the full array for
// simplicity.
static uint8_t s_puddle[CUBE_FACE_COUNT][8][8];

// Cloud: a slowly-drifting noise on TOP face, used as the visible layer and
// also as the drop spawn-rate modulator per column.
static float s_cloud[8][8];     // 0..1
static float s_cloud_phase;

static uint32_t rng_u32(void) { return esp_random(); }
static float rng_unit(void)   { return (rng_u32() & 0xFFFFFF) / (float)0xFFFFFF; }

static void rain_enter(void) {
    memset(s_drops, 0, sizeof(s_drops));
    memset(s_puddle, 0, sizeof(s_puddle));
    memset(s_cloud, 0, sizeof(s_cloud));
    s_cloud_phase = 0;
}

// Cheap fBm-ish cloud update: advect a sparse set of gaussian bumps.
static void advance_cloud(float dt) {
    s_cloud_phase += dt * 0.25f;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float fx = x / 8.0f, fy = y / 8.0f;
            float v = 0.5f + 0.5f * sinf((fx * 2.1f + s_cloud_phase) * 6.283f) *
                             sinf((fy * 1.7f - s_cloud_phase * 0.8f) * 6.283f);
            v = v * 0.6f + 0.2f;   // bias darker clouds
            s_cloud[y][x] = v;
        }
    }
}

static cube_edge_t pick_target_edge(int x, int y) {
    // Choose the closer edge on the TOP face.
    int dtop = y, dbot = 7 - y, dleft = x, dright = 7 - x;
    int m = dtop;       cube_edge_t e = EDGE_TOP;
    if (dbot < m)   { m = dbot;   e = EDGE_BOTTOM; }
    if (dleft < m)  { m = dleft;  e = EDGE_LEFT; }
    if (dright < m) { m = dright; e = EDGE_RIGHT; }
    return e;
}

static void spawn_drop(uint8_t density) {
    // Spawn probability per frame scales with density.
    // density=255 => ~one spawn per frame; density=0 => no spawns.
    int attempts = (density + 64) / 64; // 1..5
    for (int a = 0; a < attempts; a++) {
        if (rng_unit() > (density / 255.0f) * 0.6f) continue;
        for (int i = 0; i < MAX_DROPS; i++) {
            if (!s_drops[i].alive) {
                int x = rng_u32() % 8, y = rng_u32() % 8;
                // Favor spawning where the cloud is denser.
                if (rng_unit() > s_cloud[y][x]) continue;
                s_drops[i].alive = true;
                s_drops[i].face = FACE_TOP;
                s_drops[i].x = x;
                s_drops[i].y = y;
                s_drops[i].fall_counter = 0;
                s_drops[i].on_top = true;
                s_drops[i].target_edge = pick_target_edge(x, y);
                break;
            }
        }
    }
}

static void step_drops(uint8_t speed) {
    // Advance every drop. Each drop moves ~1 pixel when fall_counter overflows.
    // Higher `speed` -> lower threshold.
    uint8_t threshold = (uint8_t)(255 - speed); // 0..255, smaller = faster
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!s_drops[i].alive) continue;
        s_drops[i].fall_counter += 64;
        if (s_drops[i].fall_counter < threshold) continue;
        s_drops[i].fall_counter = 0;

        if (s_drops[i].on_top) {
            // Walk 1 pixel toward target_edge. Add small jitter perpendicular.
            int dx = 0, dy = 0;
            switch (s_drops[i].target_edge) {
                case EDGE_TOP:    dy = -1; dx = (int)(rng_unit() * 3) - 1; break;
                case EDGE_BOTTOM: dy =  1; dx = (int)(rng_unit() * 3) - 1; break;
                case EDGE_LEFT:   dx = -1; dy = (int)(rng_unit() * 3) - 1; break;
                case EDGE_RIGHT:  dx =  1; dy = (int)(rng_unit() * 3) - 1; break;
            }
            int nx = s_drops[i].x + dx;
            int ny = s_drops[i].y + dy;
            cube_face_t nf = s_drops[i].face;
            if (cube_step_over_edge(&nf, &nx, &ny)) {
                // Fell off TOP onto a side face — start falling straight down.
                s_drops[i].face = nf;
                s_drops[i].x = nx;
                s_drops[i].y = ny;
                s_drops[i].on_top = false;
            } else {
                if (nx < 0) nx = 0; else if (nx > 7) nx = 7;
                if (ny < 0) ny = 0; else if (ny > 7) ny = 7;
                s_drops[i].x = nx;
                s_drops[i].y = ny;
            }
        } else {
            // Side face: fall straight down (y++). When we step off, we land
            // on BOTTOM and become a puddle pixel.
            int nx = s_drops[i].x;
            int ny = s_drops[i].y + 1;
            cube_face_t nf = s_drops[i].face;
            if (cube_step_over_edge(&nf, &nx, &ny)) {
                // Landed on BOTTOM. Deposit puddle + occasionally climb 1-2 rows
                // back up the side face we just came from (splash).
                s_puddle[nf][nx][ny] = 255;
                if (rng_unit() < 0.4f) {
                    cube_face_t sf = s_drops[i].face;
                    int sx = s_drops[i].x, sy = 7; // bottom row of side
                    if (s_puddle[sf][sx][sy] < 200) s_puddle[sf][sx][sy] = 200;
                    if (rng_unit() < 0.2f && s_puddle[sf][sx][6] < 150)
                        s_puddle[sf][sx][6] = 150;
                }
                s_drops[i].alive = false;
            } else {
                s_drops[i].y = ny;
            }
        }
    }
}

static void decay_puddles(float dt) {
    int decay = (int)(dt * 80.0f); // ~80 units/sec
    if (decay < 1) decay = 1;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int v = s_puddle[f][x][y] - decay;
                s_puddle[f][x][y] = v < 0 ? 0 : v;
            }
        }
    }
}

static void draw_scene(void) {
    // 1. Clouds on TOP face: deep blue modulated by s_cloud.
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float c = s_cloud[y][x];
            uint8_t b = (uint8_t)(c * 120);       // blue channel dominant
            uint8_t g = (uint8_t)(c * 30);
            render_set(FACE_TOP, x, y, 0, g, b);
        }
    }
    // 2. Drops: medium blue, brighter while falling.
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!s_drops[i].alive) continue;
        uint8_t r = 0, g = 80, b = 255;
        if (s_drops[i].on_top) { g = 20; b = 180; }
        render_add(s_drops[i].face, s_drops[i].x, s_drops[i].y, r, g, b);
    }
    // 3. Puddles: light blue, intensity scaled by s_puddle.
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        // Only draw puddles where they should physically be: BOTTOM entirely,
        // plus bottom two rows on the 4 side faces.
        bool is_side = (f == FACE_NORTH || f == FACE_SOUTH ||
                        f == FACE_EAST  || f == FACE_WEST);
        int y_min = (f == FACE_BOTTOM) ? 0 : (is_side ? 6 : 8);
        int y_max = (f == FACE_BOTTOM) ? 8 : (is_side ? 8 : 8);
        for (int y = y_min; y < y_max; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t v = s_puddle[f][x][y];
                if (!v) continue;
                uint8_t r = (uint8_t)(v / 4);
                uint8_t g = (uint8_t)(v / 2);
                uint8_t b = v;
                render_add((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

static void rain_step(float dt) {
    config_lock();
    uint8_t density = config_get()->rain_density;
    uint8_t speed   = config_get()->rain_speed;
    config_unlock();

    advance_cloud(dt);
    spawn_drop(density);
    step_drops(speed);
    decay_puddles(dt);
    draw_scene();
}

const effect_vtable_t g_effect_rain = {
    .name = "rain", .id = EFFECT_RAIN, .enter = rain_enter, .step = rain_step,
};
