#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Pendulum: each pendulum has a pivot pixel on some face and a "rest
// direction" (unit vector in face-local 2D) pointing toward the
// gravitational rest position. The bob = pivot + L * rotate(rest, θ);
// physics is θ̈ = -(g/L) sin θ - damping · θ̇. Low-amplitude kicks keep
// every pendulum alive.
//
// Face-up mode    : 4 pendulums on the 4 side faces, pivoted at the top
//                   center of each, rest direction straight down.
// Corner-up mode  : 3 pendulums on the 3 upper faces clustered at the
//                   TOP+WEST+SOUTH vertex, pivoted at that corner pixel
//                   on each face, rest direction along the face diagonal
//                   toward the opposite corner (the world "down" direction
//                   projected onto the face).

#define MAX_PENDULUMS 6
#define TRAIL_LEN     8

typedef struct {
    cube_face_t face;
    float pivot_x, pivot_y;
    float rest_dx, rest_dy;   // unit vector on face (rest hang direction)
    float rod_len;
    float theta;
    float omega;
    float hue;
    float trail_x[TRAIL_LEN];
    float trail_y[TRAIL_LEN];
} pendulum_t;

static pendulum_t s_p[MAX_PENDULUMS];
static int        s_n;

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b) {
    h = h - floorf(h);
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    int seg = (int)(h * 6.0f);
    switch (seg) {
        case 0: rp = c; gp = x; break;
        case 1: rp = x; gp = c; break;
        case 2: gp = c; bp = x; break;
        case 3: gp = x; bp = c; break;
        case 4: rp = x; bp = c; break;
        default:rp = c; bp = x; break;
    }
    *r = (uint8_t)((rp + m) * 255);
    *g = (uint8_t)((gp + m) * 255);
    *b = (uint8_t)((bp + m) * 255);
}

static void configure_face_up(void) {
    s_n = 4;
    const cube_face_t SIDES[4] = {FACE_NORTH, FACE_SOUTH, FACE_EAST, FACE_WEST};
    for (int i = 0; i < 4; i++) {
        s_p[i].face    = SIDES[i];
        s_p[i].pivot_x = 3.5f;
        s_p[i].pivot_y = 0.5f;
        s_p[i].rest_dx = 0.0f;
        s_p[i].rest_dy = 1.0f;       // rest hangs straight down on each side
        s_p[i].rod_len = 5.5f;
    }
}

static void configure_corner_up(void) {
    // Six pendulums — one per face. Each pivots at its face's corner that
    // sits highest under corner-up gravity, and hangs along the face's
    // "downhill" diagonal toward the opposite (lowest) corner of the face.
    //
    // Upper faces share the TOP+WEST+SOUTH vertex as their high corner:
    //   TOP    pivot (0, 7) -> diagonal (+1, -1)
    //   WEST   pivot (0, 0) -> diagonal (+1, +1)
    //   SOUTH  pivot (0, 0) -> diagonal (+1, +1)
    //
    // Lower faces each have their OWN high corner (different cube vertex
    // per face), derived by projecting every face corner onto the up-axis
    // and picking the maximum:
    //   BOTTOM pivot (0, 0) at vertex BOTTOM+WEST+SOUTH -> diag (+1, +1)
    //   EAST   pivot (7, 0) at vertex TOP+EAST+SOUTH    -> diag (-1, +1)
    //   NORTH  pivot (7, 0) at vertex TOP+WEST+NORTH    -> diag (-1, +1)
    s_n = 6;
    const float SQRT2 = 0.70710678f;

    // Upper faces
    s_p[0].face = FACE_TOP;
    s_p[0].pivot_x = 0.5f; s_p[0].pivot_y = 6.5f;
    s_p[0].rest_dx =  SQRT2; s_p[0].rest_dy = -SQRT2;

    s_p[1].face = FACE_WEST;
    s_p[1].pivot_x = 0.5f; s_p[1].pivot_y = 0.5f;
    s_p[1].rest_dx =  SQRT2; s_p[1].rest_dy =  SQRT2;

    s_p[2].face = FACE_SOUTH;
    s_p[2].pivot_x = 0.5f; s_p[2].pivot_y = 0.5f;
    s_p[2].rest_dx =  SQRT2; s_p[2].rest_dy =  SQRT2;

    // Lower faces
    s_p[3].face = FACE_BOTTOM;
    s_p[3].pivot_x = 0.5f; s_p[3].pivot_y = 0.5f;
    s_p[3].rest_dx =  SQRT2; s_p[3].rest_dy =  SQRT2;

    s_p[4].face = FACE_EAST;
    s_p[4].pivot_x = 6.5f; s_p[4].pivot_y = 0.5f;
    s_p[4].rest_dx = -SQRT2; s_p[4].rest_dy =  SQRT2;

    s_p[5].face = FACE_NORTH;
    s_p[5].pivot_x = 6.5f; s_p[5].pivot_y = 0.5f;
    s_p[5].rest_dx = -SQRT2; s_p[5].rest_dy =  SQRT2;

    for (int i = 0; i < s_n; i++) s_p[i].rod_len = 5.0f;
}

static void pendulum_enter(void) {
    memset(s_p, 0, sizeof(s_p));
    if (orient_get() == ORIENT_CORNER_UP) configure_corner_up();
    else                                   configure_face_up();

    for (int i = 0; i < s_n; i++) {
        s_p[i].theta = (rngf() - 0.5f) * 1.6f;
        s_p[i].omega = 0;
        s_p[i].hue   = 0.15f + i * 0.27f;
        // Seed trail at rest position.
        float bx = s_p[i].pivot_x + s_p[i].rest_dx * s_p[i].rod_len;
        float by = s_p[i].pivot_y + s_p[i].rest_dy * s_p[i].rod_len;
        for (int t = 0; t < TRAIL_LEN; t++) {
            s_p[i].trail_x[t] = bx;
            s_p[i].trail_y[t] = by;
        }
    }
}

static void draw_rod(cube_face_t face, float x0, float y0, float x1, float y1,
                     uint8_t r, uint8_t g, uint8_t b) {
    const int STEPS = 12;
    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / STEPS;
        float fx = x0 + (x1 - x0) * t;
        float fy = y0 + (y1 - y0) * t;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        float frx = fx - ix, fry = fy - iy;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int px = ix + dx, py = iy + dy;
                if (px < 0 || px > 7 || py < 0 || py > 7) continue;
                float w = (dx ? frx : 1 - frx) * (dy ? fry : 1 - fry);
                render_add(face, px, py,
                           (uint8_t)(r * w * 0.6f),
                           (uint8_t)(g * w * 0.6f),
                           (uint8_t)(b * w * 0.6f));
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

static void pendulum_step(float dt) {
    if (dt > 0.04f) dt = 0.04f;

    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();

    const float g = 40.0f;
    const float damping = 0.12f;
    float speed_mul = 0.6f + (speed / 255.0f) * 1.2f;

    for (int i = 0; i < s_n; i++) {
        pendulum_t *p = &s_p[i];

        float accel = -(g / p->rod_len) * sinf(p->theta) - damping * p->omega;
        p->omega += accel * dt * speed_mul;
        p->theta += p->omega * dt * speed_mul;

        float energy = 0.5f * p->omega * p->omega
                     + (g / p->rod_len) * (1 - cosf(p->theta));
        if (energy < 0.25f) {
            p->omega += (rngf() - 0.5f) * 4.0f;
            p->hue    = fmodf(p->hue + 0.15f, 1.0f);
        }

        // Rod direction = rotate(rest, θ). Standard 2D rotation: for a unit
        // vector (rx, ry), rotated by θ gives (rx cos θ - ry sin θ, rx sin θ + ry cos θ).
        float ct = cosf(p->theta), st = sinf(p->theta);
        float dirx = p->rest_dx * ct - p->rest_dy * st;
        float diry = p->rest_dx * st + p->rest_dy * ct;
        float bx = p->pivot_x + dirx * p->rod_len;
        float by = p->pivot_y + diry * p->rod_len;

        for (int t = TRAIL_LEN - 1; t > 0; t--) {
            p->trail_x[t] = p->trail_x[t - 1];
            p->trail_y[t] = p->trail_y[t - 1];
        }
        p->trail_x[0] = bx;
        p->trail_y[0] = by;

        uint8_t r, gg, b;
        hsv_to_rgb(p->hue, 0.9f, 1.0f, &r, &gg, &b);

        draw_rod(p->face, p->pivot_x, p->pivot_y, bx, by, r, gg, b);
        for (int t = TRAIL_LEN - 1; t > 0; t--) {
            float k = (TRAIL_LEN - t) / (float)TRAIL_LEN * 0.6f;
            splat(p->face, p->trail_x[t], p->trail_y[t],
                  (uint8_t)(r * k), (uint8_t)(gg * k), (uint8_t)(b * k));
        }
        splat(p->face, bx, by, r, gg, b);

        // Pivot marker so the anchor point is visible.
        int ipx = (int)p->pivot_x, ipy = (int)p->pivot_y;
        if (ipx >= 0 && ipx <= 7 && ipy >= 0 && ipy <= 7)
            render_add(p->face, ipx, ipy, 50, 50, 70);
    }
}

const effect_vtable_t g_effect_pendulum = {
    .name = "pendulum", .id = EFFECT_PENDULUM,
    .enter = pendulum_enter, .step = pendulum_step,
};
