#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>
#include "esp_random.h"

// Ping-pong: on each of the 6 faces, an independent ball bounces inside the
// 8x8 box. When the ball hits a wall, that wall briefly lights up as a
// "paddle" for a few frames. Each face uses a different ball hue so the
// whole cube reads as 6 tiny independent games.

typedef struct {
    float x, y;
    float vx, vy;
    // Per-face paddle flashes on each of the 4 walls. 0..1, decays each frame.
    // walls[] index: 0=top (y=0), 1=right (x=7), 2=bottom (y=7), 3=left (x=0)
    float walls[4];
    uint8_t hue;
} pp_t;

static pp_t s_pp[CUBE_FACE_COUNT];

static const uint8_t PALETTE[CUBE_FACE_COUNT][3] = {
    {255,  80,  80},  // TOP    - red
    { 80,  80, 255},  // BOTTOM - blue
    {255, 180,  60},  // NORTH  - orange
    { 60, 255, 180},  // SOUTH  - mint
    {200,  80, 255},  // EAST   - violet
    {255, 255,  80},  // WEST   - yellow
};

static float rngf(void) { return (esp_random() & 0xFFFFFF) / (float)0xFFFFFF; }

static void reset_face(int f) {
    s_pp[f].x = 0.5f + rngf() * 7.0f;
    s_pp[f].y = 0.5f + rngf() * 7.0f;
    float a = rngf() * 6.283f;
    float speed = 4.0f + rngf() * 3.0f;
    s_pp[f].vx = cosf(a) * speed;
    s_pp[f].vy = sinf(a) * speed;
    for (int w = 0; w < 4; w++) s_pp[f].walls[w] = 0;
}

static void pp_enter(void) {
    for (int f = 0; f < CUBE_FACE_COUNT; f++) reset_face(f);
}

static void pp_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        pp_t *s = &s_pp[f];

        s->x += s->vx * dt;
        s->y += s->vy * dt;

        // Bounce: clamp into [0, 7] and flip the crossing velocity. Flash the
        // corresponding wall at 1.0.
        if (s->x < 0)    { s->x = -s->x;       s->vx = -s->vx; s->walls[3] = 1.0f; }
        if (s->x > 7)    { s->x = 14.0f - s->x; s->vx = -s->vx; s->walls[1] = 1.0f; }
        if (s->y < 0)    { s->y = -s->y;       s->vy = -s->vy; s->walls[0] = 1.0f; }
        if (s->y > 7)    { s->y = 14.0f - s->y; s->vy = -s->vy; s->walls[2] = 1.0f; }

        // Decay wall flashes.
        for (int w = 0; w < 4; w++) {
            s->walls[w] -= dt * 3.0f; // ~0.33s flash
            if (s->walls[w] < 0) s->walls[w] = 0;
        }

        // Draw paddles (bright where flash > 0, dim always so the court is visible).
        uint8_t pr = PALETTE[f][0], pg = PALETTE[f][1], pb = PALETTE[f][2];
        for (int w = 0; w < 4; w++) {
            float v = 0.08f + s->walls[w] * 0.90f; // always show a dim court
            uint8_t r = (uint8_t)(pr * v);
            uint8_t g = (uint8_t)(pg * v);
            uint8_t b = (uint8_t)(pb * v);
            // Paddle line length: 3 pixels centered on the ball's position
            // along the paddle axis, clamped to the wall.
            int along_c = (w == 0 || w == 2) ? (int)roundf(s->x) : (int)roundf(s->y);
            if (along_c < 1) along_c = 1;
            if (along_c > 6) along_c = 6;
            for (int k = -1; k <= 1; k++) {
                int along = along_c + k;
                switch (w) {
                    case 0: render_add((cube_face_t)f, along, 0, r, g, b); break;
                    case 2: render_add((cube_face_t)f, along, 7, r, g, b); break;
                    case 3: render_add((cube_face_t)f, 0, along, r, g, b); break;
                    case 1: render_add((cube_face_t)f, 7, along, r, g, b); break;
                }
            }
        }

        // Draw ball (bright, bilinearly splatted).
        int ix = (int)floorf(s->x), iy = (int)floorf(s->y);
        float fx = s->x - ix, fy = s->y - iy;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int px = ix + dx, py = iy + dy;
                if (px < 0 || px > 7 || py < 0 || py > 7) continue;
                float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                render_add((cube_face_t)f, px, py,
                           (uint8_t)(255 * w),
                           (uint8_t)(255 * w),
                           (uint8_t)(255 * w));
            }
        }
    }
}

const effect_vtable_t g_effect_pingpong = {
    .name = "pingpong", .id = EFFECT_PINGPONG,
    .enter = pp_enter, .step = pp_step,
};
