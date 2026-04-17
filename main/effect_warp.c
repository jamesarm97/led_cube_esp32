#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Warp: per-face starfield. Each face has a pool of "stars" that spawn at
// a face-local center (3.5, 3.5), accelerate outward along random radial
// angles, and fade off the edge of the face. Sub-pixel positions with a
// short trail echo the warp-speed aesthetic.
//
// Running per-face (not 3D through-the-cube) because the cube's faces
// aren't arranged in a way where stars could plausibly stream through all
// six at once — and because six simultaneous fields of 12 stars each is
// already very busy.

#define STARS_PER_FACE 12
#define TRAIL_LEN      3

typedef struct {
    bool  alive;
    float x, y;
    float vx, vy;
    float speed_scale; // 1..3, bigger = faster & brighter
    uint8_t r, g, b;
    // Short history for trails.
    float hx[TRAIL_LEN];
    float hy[TRAIL_LEN];
} star_t;

static star_t s_stars[CUBE_FACE_COUNT][STARS_PER_FACE];

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void star_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    // Mostly white-hot, occasionally a color for visual interest.
    int pick = rng() & 7;
    if (pick < 5) {
        *r = 255; *g = 255; *b = 255;
    } else if (pick == 5) {
        *r = 180; *g = 220; *b = 255; // blue-white
    } else if (pick == 6) {
        *r = 255; *g = 220; *b = 160; // warm yellow
    } else {
        *r = 255; *g = 170; *b = 200; // pink
    }
}

static void spawn_star(star_t *s) {
    s->alive = true;
    // Start near the center with a tiny jitter — lots of stars converging
    // at (3.5, 3.5) looks like a noisy blob, so scatter over a ~1px radius.
    float a0 = rngf() * 6.2832f;
    s->x = 3.5f + cosf(a0) * 0.3f;
    s->y = 3.5f + sinf(a0) * 0.3f;
    // Outward direction, constant initial speed.
    float a = rngf() * 6.2832f;
    s->speed_scale = 1.0f + rngf() * 2.0f;
    s->vx = cosf(a) * 0.5f * s->speed_scale;
    s->vy = sinf(a) * 0.5f * s->speed_scale;
    star_color(&s->r, &s->g, &s->b);
    for (int t = 0; t < TRAIL_LEN; t++) {
        s->hx[t] = s->x;
        s->hy[t] = s->y;
    }
}

static void warp_enter(void) {
    memset(s_stars, 0, sizeof(s_stars));
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int i = 0; i < STARS_PER_FACE; i++) {
            spawn_star(&s_stars[f][i]);
            // Pre-age so some stars are already mid-flight on first frame.
            for (int pre = 0; pre < (int)(rngf() * 30.0f); pre++) {
                s_stars[f][i].vx *= 1.08f;
                s_stars[f][i].vy *= 1.08f;
                s_stars[f][i].x  += s_stars[f][i].vx * 0.03f;
                s_stars[f][i].y  += s_stars[f][i].vy * 0.03f;
            }
        }
    }
}

static void splat(cube_face_t face, float fx, float fy,
                  uint8_t r, uint8_t g, uint8_t b) {
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float frx = fx - ix, fry = fy - iy;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            int px = ix + dx, py = iy + dy;
            if (px < 0 || px > 7 || py < 0 || py > 7) continue;
            float w = (dx ? frx : 1 - frx) * (dy ? fry : 1 - fry);
            render_add(face, px, py,
                       (uint8_t)(r * w),
                       (uint8_t)(g * w),
                       (uint8_t)(b * w));
        }
    }
}

static void warp_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    // Warp factor: controls exponential acceleration per step.
    float warp = 1.04f + (speed / 255.0f) * 0.12f; // 1.04..1.16

    if (dt > 0.05f) dt = 0.05f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int i = 0; i < STARS_PER_FACE; i++) {
            star_t *s = &s_stars[f][i];
            if (!s->alive) { spawn_star(s); continue; }

            // Shift history.
            for (int t = TRAIL_LEN - 1; t > 0; t--) {
                s->hx[t] = s->hx[t - 1];
                s->hy[t] = s->hy[t - 1];
            }
            s->hx[0] = s->x;
            s->hy[0] = s->y;

            // Accelerate (warp speed increases as stars move outward).
            s->vx *= warp;
            s->vy *= warp;
            s->x  += s->vx * dt * 3.0f;
            s->y  += s->vy * dt * 3.0f;

            // Respawn when the star leaves the face.
            if (s->x < -1 || s->x > 8 || s->y < -1 || s->y > 8) {
                spawn_star(s);
                continue;
            }

            // Draw trail (dimmer further back), then head brightest.
            for (int t = TRAIL_LEN - 1; t >= 0; t--) {
                float k = (TRAIL_LEN - t) / (float)(TRAIL_LEN + 1);
                splat((cube_face_t)f, s->hx[t], s->hy[t],
                      (uint8_t)(s->r * k * 0.6f),
                      (uint8_t)(s->g * k * 0.6f),
                      (uint8_t)(s->b * k * 0.6f));
            }
            splat((cube_face_t)f, s->x, s->y, s->r, s->g, s->b);
        }
    }
}

const effect_vtable_t g_effect_warp = {
    .name = "warp", .id = EFFECT_WARP,
    .enter = warp_enter, .step = warp_step,
};
