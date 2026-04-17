#include "effects.h"
#include "render.h"
#include "cube.h"

#include <string.h>
#include <math.h>
#include <stdint.h>
#include "esp_random.h"

// Bouncing balls. Each ball:
//   - spawns on the TOP face with random position and velocity,
//   - rolls (with tiny friction) until it leaves the TOP face,
//   - falls down a side face under gravity,
//   - bounces against the side's bottom edge (y=7) with energy loss,
//   - eventually comes to rest, then respawns on TOP with fresh velocity.
//
// Positions are sub-pixel floats. Crossing a face seam goes through
// cube_step_over_edge() so balls move continuously around the cube.

#define MAX_BALLS 8
#define TRAIL_LEN 3

typedef struct {
    cube_face_t face;
    float x, y;
    float vx, vy;
    uint8_t r, g, b;
    bool on_top;
    float rest_timer; // how long we've been "almost still" at rest; triggers respawn
    struct {
        cube_face_t face;
        float x, y;
    } trail[TRAIL_LEN];
} ball_t;

static ball_t s_balls[MAX_BALLS];

static const float GRAVITY       = 18.0f;  // pixels/s² down on side faces
static const float TOP_FRICTION  = 0.6f;   // velocity decay /sec on TOP
static const float SIDE_FRICTION = 0.15f;  // x-velocity decay /sec on side
static const float RESTITUTION   = 0.62f;  // coefficient of restitution on bounce
static const float REST_EPS      = 0.8f;   // |v| threshold to call "at rest"
static const float REST_RESPAWN  = 2.5f;   // seconds at rest before respawn

static uint32_t rng(void) { return esp_random(); }
static float rngf(void)   { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void randomize_color(ball_t *b) {
    // Bright saturated color — pick from a palette of 6 vivid hues.
    static const uint8_t pal[6][3] = {
        {255, 64, 64},  {255,160, 40}, {255,255, 64},
        { 64,255,128},  { 80,180,255}, {200,100,255},
    };
    int i = rng() % 6;
    b->r = pal[i][0]; b->g = pal[i][1]; b->b = pal[i][2];
}

static void spawn_ball(ball_t *b) {
    b->face = FACE_TOP;
    b->on_top = true;
    b->x = 1.0f + rngf() * 6.0f;
    b->y = 1.0f + rngf() * 6.0f;
    // Random direction, modest speed so the ball actually leaves TOP.
    float angle = rngf() * 6.283f;
    float speed = 4.0f + rngf() * 3.0f;
    b->vx = cosf(angle) * speed;
    b->vy = sinf(angle) * speed;
    b->rest_timer = 0;
    randomize_color(b);
    for (int t = 0; t < TRAIL_LEN; t++) {
        b->trail[t].face = b->face;
        b->trail[t].x = b->x;
        b->trail[t].y = b->y;
    }
}

static void balls_enter(void) {
    memset(s_balls, 0, sizeof(s_balls));
    for (int i = 0; i < MAX_BALLS; i++) spawn_ball(&s_balls[i]);
}

static void advance_trail(ball_t *b) {
    for (int t = TRAIL_LEN - 1; t > 0; t--) b->trail[t] = b->trail[t - 1];
    b->trail[0].face = b->face;
    b->trail[0].x = b->x;
    b->trail[0].y = b->y;
}

// Integrate one ball for dt seconds. Handles friction, gravity, edge crossing,
// and bouncing on the bottom of side faces.
static void step_ball(ball_t *b, float dt) {
    if (b->on_top) {
        // Friction: v *= exp(-k*dt).
        float decay = expf(-TOP_FRICTION * dt);
        b->vx *= decay;
        b->vy *= decay;
    } else {
        b->vy += GRAVITY * dt;
        float decay = expf(-SIDE_FRICTION * dt);
        b->vx *= decay;
    }

    b->x += b->vx * dt;
    b->y += b->vy * dt;

    // Bounce on the side face's bottom edge (y=7) — the seam to BOTTOM.
    if (!b->on_top && b->y >= 7.0f) {
        b->y = 7.0f - (b->y - 7.0f);
        b->vy = -b->vy * RESTITUTION;
        // Drag on horizontal component during the ground contact.
        b->vx *= 0.85f;
    }

    // Face crossings using integer grid, fall back to cube_step_over_edge.
    // We only cross once per step; if the ball moved fast enough to cross
    // multiple seams that's fine — next frame handles the rest.
    if (b->x < 0 || b->x > 7.9999f || b->y < 0 || b->y > 7.9999f) {
        int ix = (int)floorf(b->x);
        int iy = (int)floorf(b->y);
        cube_face_t nf = b->face;
        bool crossed = cube_step_over_edge(&nf, &ix, &iy);
        if (crossed) {
            // Re-derive sub-pixel position. step_over_edge has snapped onto
            // the first row of the new face's seam; preserve the fractional
            // part along the shared axis, and put us at the seam in the
            // perpendicular axis.
            float frac_x = b->x - floorf(b->x);
            float frac_y = b->y - floorf(b->y);
            b->face = nf;
            if (b->face == FACE_TOP) {
                // Just wrapped back onto the top (rare unless we flew hard).
                b->on_top = true;
            } else if (b->face != FACE_BOTTOM) {
                b->on_top = false;
            }
            b->x = ix + frac_x;
            b->y = iy + frac_y;

            // Velocity continues in the same "world direction". Our coord
            // frames differ per face but for a visually plausible result we
            // just keep |v| and flip the axis that crossed the seam (handled
            // implicitly by stepping across with continuous speed). As a
            // simple rule: when going from TOP -> side, re-orient so the
            // ball is now moving "downward" on the side.
            if (!b->on_top) {
                float speed = sqrtf(b->vx * b->vx + b->vy * b->vy);
                if (speed < 2.0f) speed = 2.0f;
                b->vx = (rngf() - 0.5f) * speed * 0.4f;
                b->vy = fabsf(b->vy) + 2.0f; // bias downward when entering side
            }
        } else {
            // Clamp if no valid wrap (shouldn't happen on a valid mesh).
            if (b->x < 0) b->x = 0;
            if (b->x > 7) b->x = 7;
            if (b->y < 0) b->y = 0;
            if (b->y > 7) b->y = 7;
        }
    }

    // Detect rest: on a side face, low |v| and near y=7 for REST_RESPAWN sec.
    if (!b->on_top && b->y > 6.2f &&
        fabsf(b->vx) < REST_EPS && fabsf(b->vy) < REST_EPS) {
        b->rest_timer += dt;
        if (b->rest_timer > REST_RESPAWN) spawn_ball(b);
    } else {
        b->rest_timer = 0;
    }
}

static void splat(cube_face_t face, float fx, float fy,
                  uint8_t r, uint8_t g, uint8_t b) {
    // Bilinear splat to 4 nearest pixels.
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

static void balls_step(float dt) {
    if (dt > 0.05f) dt = 0.05f; // cap step to keep physics stable

    for (int i = 0; i < MAX_BALLS; i++) {
        advance_trail(&s_balls[i]);
        step_ball(&s_balls[i], dt);
    }
    for (int i = 0; i < MAX_BALLS; i++) {
        ball_t *b = &s_balls[i];
        // Draw trail dimmer-to-brighter; head is drawn last.
        for (int t = TRAIL_LEN - 1; t >= 0; t--) {
            uint8_t s = 60 + (TRAIL_LEN - 1 - t) * 40;
            splat(b->trail[t].face, b->trail[t].x, b->trail[t].y,
                  (b->r * s) / 255, (b->g * s) / 255, (b->b * s) / 255);
        }
        splat(b->face, b->x, b->y, b->r, b->g, b->b);
    }
}

const effect_vtable_t g_effect_balls = {
    .name = "balls", .id = EFFECT_BALLS,
    .enter = balls_enter, .step = balls_step,
};
