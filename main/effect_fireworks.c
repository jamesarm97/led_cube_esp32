#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Fireworks:
//   1. A rocket spawns at the bottom of a random side face (y=7) with an
//      upward velocity. It decelerates due to gravity and leaves a dim
//      trail.
//   2. Near apex (velocity crosses zero or after a random delay) it
//      detonates into N radiating sparks, each with random velocity.
//   3. Sparks fall under gravity, fade over ~1.5 s, and spill naturally
//      onto the TOP face when their y steps past 0 on the side — handled
//      by cube_step_over_edge().
//   4. Rockets are spawned at a steady rate so several can be in flight.

#define MAX_ROCKETS  4
#define MAX_SPARKS   128
#define SPARKS_PER_BURST  30

typedef struct {
    bool     alive;
    bool     on_top;   // true once the rocket has crossed the top seam onto TOP
    cube_face_t face;
    float    x, y;
    float    vx, vy;
    uint8_t  r, g, b;
    float    fuse;   // seconds until forced detonation
} rocket_t;

typedef struct {
    bool     alive;
    cube_face_t face;
    float    x, y;
    float    vx, vy;
    uint8_t  r, g, b;
    float    age;    // seconds since spawn
    float    life;   // total lifespan in seconds
} spark_t;

static rocket_t s_rockets[MAX_ROCKETS];
static spark_t  s_sparks[MAX_SPARKS];
static float    s_spawn_timer;

static const cube_face_t SIDES[4] = {FACE_NORTH, FACE_SOUTH, FACE_EAST, FACE_WEST};

// Rockets experience upward thrust but deceleration (we model it as the ball
// slowing against "gravity" pointing down = +y).
static const float GRAVITY_SIDE = 14.0f;

static uint32_t rng(void)   { return esp_random(); }
static float    rngf(void)  { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void random_bright_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    // Pick among a palette of classic firework colors (brights only).
    static const uint8_t pal[8][3] = {
        {255,  60,  60}, {255, 200,  40}, {255, 255,  80}, {120, 255,  80},
        { 60, 255, 220}, { 80, 140, 255}, {220,  80, 255}, {255, 255, 255},
    };
    int i = rng() & 7;
    *r = pal[i][0]; *g = pal[i][1]; *b = pal[i][2];
}

static void spawn_rocket(void) {
    for (int i = 0; i < MAX_ROCKETS; i++) {
        if (s_rockets[i].alive) continue;
        s_rockets[i].alive = true;
        s_rockets[i].on_top = false;
        s_rockets[i].face  = SIDES[rng() & 3];
        s_rockets[i].x     = 1.0f + rngf() * 6.0f;
        s_rockets[i].y     = 7.5f;                     // just off bottom
        s_rockets[i].vx    = (rngf() - 0.5f) * 1.0f;   // gentle drift
        // Enough upward velocity to cross the top seam with ~6 px/s to spare
        // (solved from v² = u² - 2·g·7.5 with g=GRAVITY_SIDE).
        s_rockets[i].vy    = -15.0f - rngf() * 3.0f;   // -15..-18
        s_rockets[i].fuse  = 1.3f + rngf() * 0.3f;     // ~1.3..1.6s
        random_bright_color(&s_rockets[i].r, &s_rockets[i].g, &s_rockets[i].b);
        return;
    }
}

static void explode(rocket_t *rk) {
    // On TOP (or BOTTOM) there's no meaningful "up" in face-local coords, so
    // radiate uniformly. On a side face, bias upward so sparks bloom toward
    // the top seam and spill onto TOP.
    bool uniform = (rk->face == FACE_TOP || rk->face == FACE_BOTTOM);
    float vy_bias = uniform ? 0.0f : -1.5f;

    for (int n = 0; n < SPARKS_PER_BURST; n++) {
        for (int s = 0; s < MAX_SPARKS; s++) {
            if (s_sparks[s].alive) continue;
            s_sparks[s].alive = true;
            s_sparks[s].face  = rk->face;
            s_sparks[s].x     = rk->x;
            s_sparks[s].y     = rk->y;
            float a = rngf() * 6.2832f;
            float speed = 2.5f + rngf() * 3.5f;
            s_sparks[s].vx = cosf(a) * speed;
            s_sparks[s].vy = sinf(a) * speed + vy_bias;
            s_sparks[s].r  = rk->r;
            s_sparks[s].g  = rk->g;
            s_sparks[s].b  = rk->b;
            s_sparks[s].age  = 0;
            s_sparks[s].life = 1.2f + rngf() * 0.6f;
            break;
        }
    }
    rk->alive = false;
}

static void fireworks_enter(void) {
    memset(s_rockets, 0, sizeof(s_rockets));
    memset(s_sparks,  0, sizeof(s_sparks));
    s_spawn_timer = 0;
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

// Cross face seams for a float position. On success updates face/x/y to the
// neighbor's frame. TOP face is treated as a "flat tray" — sparks on TOP
// have no gravity and drift with friction.
static void cross_seam(cube_face_t *face, float *x, float *y) {
    int ix = (int)floorf(*x);
    int iy = (int)floorf(*y);
    if (ix >= 0 && ix < 8 && iy >= 0 && iy < 8) return;
    cube_face_t nf = *face;
    cube_step_over_edge(&nf, &ix, &iy);
    // Preserve fractional part.
    float frac_x = *x - floorf(*x);
    float frac_y = *y - floorf(*y);
    *face = nf;
    *x = ix + frac_x;
    *y = iy + frac_y;
}

static void fireworks_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    // Spawn cadence: ~1 rocket every 0.9 s, varied.
    s_spawn_timer -= dt;
    if (s_spawn_timer <= 0) {
        spawn_rocket();
        s_spawn_timer = 0.7f + rngf() * 0.5f;
    }

    // Rockets: integrate, leave trail, detonate at apex or when fuse ends.
    for (int i = 0; i < MAX_ROCKETS; i++) {
        rocket_t *rk = &s_rockets[i];
        if (!rk->alive) continue;

        if (!rk->on_top) {
            rk->vy += GRAVITY_SIDE * dt;
        } else {
            // On TOP there's no "up" in face-local coords; just coast with
            // mild friction so the rocket keeps drifting inward briefly.
            float decay = expf(-0.9f * dt);
            rk->vx *= decay;
            rk->vy *= decay;
        }
        rk->x += rk->vx * dt;
        rk->y += rk->vy * dt;
        rk->fuse -= dt;

        // Cross seam when the rocket steps off its current face. If it just
        // entered the TOP face, reorient velocity to continue "inward" at
        // preserved speed — the per-side coord flips done by cube_adjacent
        // make it easier to do this via the rocket's landing point than by
        // unpacking the flip math.
        if (rk->x < 0 || rk->x >= 8.0f || rk->y < 0 || rk->y >= 8.0f) {
            cube_face_t prev = rk->face;
            cross_seam(&rk->face, &rk->x, &rk->y);
            if (rk->face == FACE_TOP && prev != FACE_TOP && !rk->on_top) {
                float speed = sqrtf(rk->vx * rk->vx + rk->vy * rk->vy);
                if (speed < 3.0f) speed = 3.0f;
                int ix = (int)floorf(rk->x), iy = (int)floorf(rk->y);
                // cross_seam snaps the perpendicular coord to the seam edge,
                // so exactly one of these conditions fires.
                if (iy == 0)      { rk->vx = 0;      rk->vy =  speed; }
                else if (iy == 7) { rk->vx = 0;      rk->vy = -speed; }
                else if (ix == 0) { rk->vx =  speed; rk->vy = 0;      }
                else if (ix == 7) { rk->vx = -speed; rk->vy = 0;      }
                rk->on_top = true;
                // Short delay before detonation so the rocket traces a
                // visible arc onto the top face before exploding.
                rk->fuse = 0.20f + rngf() * 0.10f;
            }
        }

        // Trail: white-hot head + dim echo below (TOP face just shows a dim
        // twin, which reads as a plume).
        splat(rk->face, rk->x, rk->y, rk->r, rk->g, rk->b);
        splat(rk->face, rk->x, rk->y + (rk->on_top ? 0.4f : 0.6f),
              (uint8_t)(rk->r / 4), (uint8_t)(rk->g / 4), (uint8_t)(rk->b / 4));

        // Detonation triggers:
        //   - fuse expired (any face)
        //   - on a side face and already past apex (wasn't fast enough to
        //     clear the seam; detonate in place rather than falling back)
        bool apex_on_side = !rk->on_top && rk->vy >= 0;
        if (rk->fuse <= 0 || apex_on_side) {
            explode(rk);
        }
    }

    // Sparks: gravity on side faces, friction-only on TOP, fade by age.
    for (int i = 0; i < MAX_SPARKS; i++) {
        spark_t *sp = &s_sparks[i];
        if (!sp->alive) continue;
        sp->age += dt;
        if (sp->age >= sp->life) { sp->alive = false; continue; }

        bool is_top    = (sp->face == FACE_TOP);
        bool is_bottom = (sp->face == FACE_BOTTOM);

        if (!is_top && !is_bottom) {
            sp->vy += GRAVITY_SIDE * dt;
        } else {
            // On top/bottom, apply a small isotropic friction.
            float decay = expf(-0.9f * dt);
            sp->vx *= decay;
            sp->vy *= decay;
        }
        sp->x += sp->vx * dt;
        sp->y += sp->vy * dt;

        // Seam crossing: sparks naturally spill onto TOP when y goes negative
        // on a side face. On TOP they may then wander off another edge.
        if (sp->x < 0 || sp->x >= 8.0f || sp->y < 0 || sp->y >= 8.0f) {
            cross_seam(&sp->face, &sp->x, &sp->y);
            // Sanity clamp in case step_over_edge couldn't map.
            if (sp->x < 0)    sp->x = 0;
            if (sp->x > 7.9f) sp->x = 7.9f;
            if (sp->y < 0)    sp->y = 0;
            if (sp->y > 7.9f) sp->y = 7.9f;
        }

        float t = sp->age / sp->life;
        float fade = (1.0f - t);
        fade = fade * fade; // quadratic fade looks more "sparky"
        splat(sp->face, sp->x, sp->y,
              (uint8_t)(sp->r * fade),
              (uint8_t)(sp->g * fade),
              (uint8_t)(sp->b * fade));
    }
}

const effect_vtable_t g_effect_fireworks = {
    .name = "fireworks", .id = EFFECT_FIREWORKS,
    .enter = fireworks_enter, .step = fireworks_step,
};
