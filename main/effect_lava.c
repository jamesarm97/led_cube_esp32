#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"
#include "esp_random.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Lava lamp: several warm metaballs drift inside the cube. Each pixel
// sums the influence field from every ball, and pixels whose accumulated
// field exceeds a threshold light up in a lava-lamp palette (deep red
// at the cool bottom through orange/yellow at the hot top). Balls
// buoy upward while hot, cool at the ceiling, sink, then reheat at the
// floor — the classic convection cycle that powers real lava lamps.
//
// Orientation-independent — "up" here always means cube-world +y.

#define LAVA_N 5

typedef struct {
    float x, y, z;       // position in cube-world [0,1]^3
    float vx, vy, vz;    // velocity
    float radius;        // ball radius (squared and used as influence scale)
    float heat;          // 0 = cool/sinking, 1 = hot/rising
} blob_t;

static blob_t s_balls[LAVA_N];
static float  s_px[CUBE_FACE_COUNT][8][8];
static float  s_py[CUBE_FACE_COUNT][8][8];
static float  s_pz[CUBE_FACE_COUNT][8][8];
static float  s_wobble;

static float rngf(void) { return (esp_random() & 0xFFFFFF) / (float)0xFFFFFF; }

static void lava_enter(void) {
    for (int i = 0; i < LAVA_N; i++) {
        s_balls[i].x = 0.22f + rngf() * 0.56f;
        s_balls[i].y = 0.12f + rngf() * 0.75f;
        s_balls[i].z = 0.22f + rngf() * 0.56f;
        s_balls[i].vx = (rngf() - 0.5f) * 0.06f;
        s_balls[i].vy = (rngf() - 0.5f) * 0.04f;
        s_balls[i].vz = (rngf() - 0.5f) * 0.06f;
        s_balls[i].radius = 0.15f + rngf() * 0.09f;
        s_balls[i].heat   = rngf();
    }
    s_wobble = 0;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y], &s_py[f][x][y], &s_pz[f][x][y]);
            }
        }
    }
}

static void lava_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();
    float speed = 0.45f + (speed_knob / 255.0f) * 1.6f;
    s_wobble += dt;

    // Integrate each ball. Positive "heat-above-neutral" buoys it up;
    // negative heat sinks it. Heat relaxes toward (1 - y) so a ball
    // at the floor wants to be hot and a ball at the ceiling wants cool.
    for (int i = 0; i < LAVA_N; i++) {
        blob_t *b = &s_balls[i];
        float ay = (b->heat - 0.5f) * 0.85f * speed;
        b->vy += ay * dt;
        b->vx += sinf(s_wobble * 0.70f + i * 1.27f) * 0.035f * dt;
        b->vz += cosf(s_wobble * 0.85f + i * 2.11f) * 0.035f * dt;
        b->vx *= 0.985f; b->vy *= 0.985f; b->vz *= 0.985f;
        b->x  += b->vx * dt * speed;
        b->y  += b->vy * dt * speed;
        b->z  += b->vz * dt * speed;
        if (b->x < 0.10f) { b->x = 0.10f; b->vx = -b->vx * 0.4f; }
        if (b->x > 0.90f) { b->x = 0.90f; b->vx = -b->vx * 0.4f; }
        if (b->z < 0.10f) { b->z = 0.10f; b->vz = -b->vz * 0.4f; }
        if (b->z > 0.90f) { b->z = 0.90f; b->vz = -b->vz * 0.4f; }
        if (b->y < 0.08f) { b->y = 0.08f; b->vy = fabsf(b->vy) * 0.2f; }
        if (b->y > 0.92f) { b->y = 0.92f; b->vy = -fabsf(b->vy) * 0.2f; }
        float target = 1.0f - b->y;
        b->heat += (target - b->heat) * dt * 0.45f * speed;
    }

    const float field_threshold = 0.80f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px = s_px[f][x][y];
                float py = s_py[f][x][y];
                float pz = s_pz[f][x][y];

                float field = 0;
                float hot_acc = 0;
                for (int i = 0; i < LAVA_N; i++) {
                    blob_t *b = &s_balls[i];
                    float dx = px - b->x, dy = py - b->y, dz = pz - b->z;
                    float d2 = dx * dx + dy * dy + dz * dz + 0.0010f;
                    float contrib = (b->radius * b->radius) / d2;
                    field += contrib;
                    hot_acc += contrib * b->heat;
                }
                if (field < field_threshold) continue;

                float over = field - field_threshold;
                if (over > 1.0f) over = 1.0f;
                float intensity = over * over * (3.0f - 2.0f * over); // smoothstep

                float heat_avg = hot_acc / (field + 1e-3f);

                // Deep red at cool heat, orange/yellow when hot.
                float r = (150.0f + heat_avg *  95.0f) * intensity;
                float g = ( 10.0f + heat_avg * 190.0f) * intensity;
                float bl = (30.0f * (1.0f - heat_avg)) * intensity;
                if (r  > 255) r  = 255;
                if (g  > 255) g  = 255;
                if (bl > 255) bl = 255;
                render_set((cube_face_t)f, x, y,
                           (uint8_t)r, (uint8_t)g, (uint8_t)bl);
            }
        }
    }
}

const effect_vtable_t g_effect_lava = {
    .name = "lava", .id = EFFECT_LAVA,
    .enter = lava_enter, .step = lava_step,
};
