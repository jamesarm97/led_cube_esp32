#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"
#include "esp_random.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Black hole: a dark event horizon sits at the cube center. Around it,
// a thin accretion disk in the cube's equatorial plane glows with a
// blue-white-hot inner edge fading to orange outside. A slowly rotating
// spiral pattern reads as material swirling inward; a Doppler
// asymmetry brightens the side where matter approaches the viewer on
// each face. Outside the disk plane a faint photon ring wraps around
// the event horizon, and occasional in-falling stars trail into the
// singularity.
//
// Orientation-independent — disk lives in cube-world y = 0.5.

#define BH_PARTICLES 10

typedef struct {
    float r;          // current radius from axis (on the disk plane)
    float theta;      // current angular position
    float life;       // 0..1, 0 = just spawned, 1 = consumed
    bool  alive;
} particle_t;

static particle_t s_pp[BH_PARTICLES];
static float s_px[CUBE_FACE_COUNT][8][8];
static float s_py[CUBE_FACE_COUNT][8][8];
static float s_pz[CUBE_FACE_COUNT][8][8];
static float s_time;
static float s_spawn_cd;

static float rngf(void) { return (esp_random() & 0xFFFFFF) / (float)0xFFFFFF; }

static void bh_enter(void) {
    s_time = 0;
    s_spawn_cd = 0;
    memset(s_pp, 0, sizeof(s_pp));
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y], &s_py[f][x][y], &s_pz[f][x][y]);
            }
        }
    }
}

static void spawn_particle(void) {
    for (int i = 0; i < BH_PARTICLES; i++) {
        if (s_pp[i].alive) continue;
        s_pp[i].alive = true;
        s_pp[i].r = 0.50f + rngf() * 0.08f;
        s_pp[i].theta = rngf() * 6.2832f;
        s_pp[i].life = 0;
        return;
    }
}

// Smoothstep-clamped.
static inline float ss(float lo, float hi, float x) {
    if (x <= lo) return 0;
    if (x >= hi) return 1;
    float t = (x - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);
}

static void bh_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();
    float rot = 0.6f + (speed_knob / 255.0f) * 1.8f;
    s_time += dt * rot;

    // Advance infalling particles: shrink radius + spin up with 1/r^1.5 (Kepler).
    s_spawn_cd -= dt;
    if (s_spawn_cd <= 0) {
        spawn_particle();
        s_spawn_cd = 0.35f + rngf() * 0.80f;
    }
    for (int i = 0; i < BH_PARTICLES; i++) {
        if (!s_pp[i].alive) continue;
        float inv = 1.0f / (s_pp[i].r + 0.05f);
        s_pp[i].theta += dt * rot * 2.5f * inv;
        s_pp[i].r     -= dt * rot * 0.12f * inv;
        s_pp[i].life  += dt * 0.55f;
        if (s_pp[i].r < 0.14f || s_pp[i].life > 1.0f) {
            s_pp[i].alive = false;
        }
    }

    const float cx = 0.5f, cy = 0.5f, cz = 0.5f;
    const float r_event   = 0.14f;   // absolute dark radius
    const float r_photon  = 0.20f;   // photon ring peak
    const float r_inner   = 0.22f;
    const float r_outer   = 0.70f;   // disk extends near the face edges
    const float disk_half = 0.13f;   // disk half-thickness (cube y-units)

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px = s_px[f][x][y];
                float py = s_py[f][x][y];
                float pz = s_pz[f][x][y];

                float dxp = px - cx;
                float dzp = pz - cz;
                float dyp = py - cy;
                float rh  = sqrtf(dxp * dxp + dzp * dzp);  // cylindrical radius
                float r3  = sqrtf(rh * rh + dyp * dyp);

                float rr = 0, gg = 0, bb = 0;

                // Event horizon: pure dark, no light.
                if (r3 < r_event) {
                    render_set((cube_face_t)f, x, y, 0, 0, 0);
                    continue;
                }

                // Photon ring — bright halo around the hole in all directions.
                float ring_d = r3 - r_photon;
                float ring = expf(-(ring_d * ring_d) * (1.0f / (2.0f * 0.05f * 0.05f)));
                rr += 255.0f * ring * 0.70f;
                gg += 170.0f * ring * 0.70f;
                bb +=  70.0f * ring * 0.70f;

                // Broad gravitational halo, visible even on the TOP/BOTTOM
                // faces where the disk never passes. Falls off gently with
                // cube-space radius.
                float halo = expf(-(ring_d * ring_d) * (1.0f / (2.0f * 0.22f * 0.22f)));
                rr += 180.0f * halo * 0.25f;
                gg +=  90.0f * halo * 0.25f;
                bb +=  40.0f * halo * 0.25f;

                // Accretion disk: thin slab in y, annular in r.
                float depth_env = 1.0f - fabsf(dyp) / disk_half;
                if (depth_env > 0 && rh > r_inner - 0.03f && rh < r_outer + 0.04f) {
                    depth_env = depth_env * depth_env * (3.0f - 2.0f * depth_env);
                    float radial_env =
                        ss(r_inner - 0.03f, r_inner + 0.04f, rh) *
                        (1.0f - ss(r_outer - 0.06f, r_outer + 0.04f, rh));

                    float theta = atan2f(dzp, dxp);
                    // Two-arm trailing spiral with angular position + radial phase.
                    float pattern = 0.5f + 0.5f *
                        sinf(2.0f * theta - s_time * 3.2f - rh * 7.0f);
                    pattern = powf(pattern, 1.6f);

                    // Doppler: material on one side (positive rotation direction,
                    // +dz at theta=0) approaches — brighten. dampen on the other.
                    float doppler = 0.65f + 0.45f * cosf(theta);

                    // Color: blue-white inner edge, orange outer edge.
                    float t = (rh - r_inner) / (r_outer - r_inner);
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    float dr = 200.0f + t *  55.0f;
                    float dg = 210.0f - t * 120.0f;
                    float db = 255.0f - t * 215.0f;

                    float w = depth_env * radial_env * pattern * doppler * 1.4f;
                    rr += dr * w;
                    gg += dg * w;
                    bb += db * w;
                }

                // Infalling particle streaks — sampled in cylindrical coords.
                for (int i = 0; i < BH_PARTICLES; i++) {
                    if (!s_pp[i].alive) continue;
                    float ppx = cx + s_pp[i].r * cosf(s_pp[i].theta);
                    float ppz = cz + s_pp[i].r * sinf(s_pp[i].theta);
                    float ddx = px - ppx;
                    float ddz = pz - ppz;
                    float ddy = py - cy;
                    float d2 = ddx * ddx + ddy * ddy * 1.5f + ddz * ddz;
                    if (d2 > 0.012f) continue;
                    float k = expf(-d2 * 380.0f) * (1.0f - s_pp[i].life);
                    rr += 255.0f * k;
                    gg += 230.0f * k;
                    bb += 200.0f * k;
                }

                if (rr < 0.5f && gg < 0.5f && bb < 0.5f) continue;
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb > 255) bb = 255;
                render_set((cube_face_t)f, x, y,
                           (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
            }
        }
    }
}

const effect_vtable_t g_effect_blackhole = {
    .name = "blackhole", .id = EFFECT_BLACKHOLE,
    .enter = bh_enter, .step = bh_step,
};
