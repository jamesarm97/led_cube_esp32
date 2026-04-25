#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Supernova: a stellar core sits at the center of the cube, breathing
// and slowly building heat. Every few seconds it can no longer contain
// itself — it erupts in a flash and a spherical shockwave radiates
// outward through cube-world. On the 6 surface panels the shockwave
// reads as a bright ring that emerges at each face's center and sweeps
// out toward the corners. After the blast the core falls to a dim ember
// and the cycle begins again.
//
// Orientation-independent — the effect is anchored at (0.5, 0.5, 0.5)
// in cube-world, not tied to the up/down poles.

#define SN_MAX_WAVES 3

typedef struct {
    bool  alive;
    float age;     // seconds since birth
    float speed;   // world-units / sec (jittered per wave)
} sn_wave_t;

static sn_wave_t s_waves[SN_MAX_WAVES];

// Distance from each surface pixel to the cube center (0.5, 0.5, 0.5).
// Range is roughly [0.50, 0.81]: face-center pixels are closest, pixels
// clustered near cube corners are farthest.
static float s_dist[CUBE_FACE_COUNT][8][8];

static float s_phase;        // seconds into current eruption cycle
static float s_flash;        // 0..1 ignition flash, decays each frame
static float s_wobble;       // monotonic clock for breathing oscillators
static float s_mini_timer;   // gating between pre-eruption flickers

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void spawn_wave(void) {
    for (int i = 0; i < SN_MAX_WAVES; i++) {
        if (s_waves[i].alive) continue;
        s_waves[i].alive = true;
        s_waves[i].age   = 0;
        s_waves[i].speed = 0.55f + rngf() * 0.15f;
        return;
    }
}

// Map a "heat" value 0..~1.8 to an RGB core color on the ember->white-hot
// stellar axis: deep red embers at low heat, orange-yellow at mid, white
// at high, with a blue-white tint at the pre-detonation peak.
static void heat_color(float h, float *r, float *g, float *b) {
    if (h < 0) h = 0;
    if (h > 1.8f) h = 1.8f;
    if (h < 0.35f) {
        float t = h / 0.35f;
        *r = t * 180.0f; *g = t * 20.0f;  *b = 0;
    } else if (h < 0.75f) {
        float t = (h - 0.35f) / 0.40f;
        *r = 180.0f + t * 75.0f;
        *g = 20.0f  + t * 180.0f;
        *b = 0;
    } else if (h < 1.15f) {
        float t = (h - 0.75f) / 0.40f;
        *r = 255.0f;
        *g = 200.0f + t * 55.0f;
        *b = t * 220.0f;
    } else {
        float t = (h - 1.15f) / 0.65f;
        *r = 255.0f - t * 60.0f;
        *g = 255.0f - t * 20.0f;
        *b = 220.0f + t * 35.0f;
    }
}

static void supernova_enter(void) {
    memset(s_waves, 0, sizeof(s_waves));
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px, py, pz;
                orient_pixel_pos3d((cube_face_t)f, x, y, &px, &py, &pz);
                float dx = px - 0.5f, dy = py - 0.5f, dz = pz - 0.5f;
                s_dist[f][x][y] = sqrtf(dx*dx + dy*dy + dz*dz);
            }
        }
    }
    s_phase = 0;
    s_flash = 0;
    s_wobble = 0;
    s_mini_timer = 0;
}

static void supernova_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();

    // Rainbow-speed knob drives eruption cadence: slow knob ~7s/cycle,
    // fast knob ~3s/cycle.
    float cycle = 7.0f - (speed_knob / 255.0f) * 4.0f;

    s_phase      += dt;
    s_wobble     += dt;
    s_mini_timer -= dt;

    float t = s_phase / cycle;
    if (t >= 1.0f) {
        s_phase -= cycle;
        t = 0;
        spawn_wave();
        s_flash = 1.0f;
    }

    // Pre-eruption flickers: in the last 20% of the cycle the core
    // throws small warning flashes at random.
    if (t > 0.80f && s_mini_timer <= 0) {
        float extra = 0.25f + rngf() * 0.25f;
        if (extra > s_flash) s_flash = extra;
        s_mini_timer = 0.08f + rngf() * 0.18f;
    }

    // Ignition flash decays quickly.
    s_flash *= expf(-dt * 5.5f);

    // Advance shockwaves; retire when the ring has cleared the far corners.
    const float max_radius = 0.95f;
    for (int i = 0; i < SN_MAX_WAVES; i++) {
        if (!s_waves[i].alive) continue;
        s_waves[i].age += dt;
        if (s_waves[i].age * s_waves[i].speed > max_radius + 0.20f) {
            s_waves[i].alive = false;
        }
    }

    // Core "heat": breathing baseline + quadratic surge in the cycle's
    // second half + instantaneous ignition flash spike.
    float breath = 0.30f + 0.10f * sinf(s_wobble * 5.5f)
                         + 0.05f * sinf(s_wobble * 13.0f + 1.7f);
    float surge  = (t > 0.5f) ? powf((t - 0.5f) / 0.5f, 1.8f) * 0.95f : 0.0f;
    float heat   = breath + surge + s_flash * 0.70f;

    float core_r, core_g, core_b;
    heat_color(heat, &core_r, &core_g, &core_b);

    const float ring_sigma = 0.09f;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float d = s_dist[f][x][y];

                // Core falloff: remap d from [0.50, 0.82] -> [0, 1], invert,
                // smoothstep, and keep a 25% floor so corners stay faintly
                // lit rather than going black.
                float u = (d - 0.50f) / 0.32f;
                if (u < 0) u = 0;
                if (u > 1) u = 1;
                float cw = 1.0f - u;
                cw = cw * cw * (3.0f - 2.0f * cw);
                cw = 0.25f + cw * 0.75f;

                float rr = core_r * cw;
                float gg = core_g * cw;
                float bb = core_b * cw;

                // Accumulate all live shockwave rings.
                for (int i = 0; i < SN_MAX_WAVES; i++) {
                    if (!s_waves[i].alive) continue;
                    float radius = s_waves[i].age * s_waves[i].speed;
                    float delta  = d - radius;
                    if (fabsf(delta) > 3.0f * ring_sigma) continue;
                    float k = expf(-(delta * delta) / (2 * ring_sigma * ring_sigma));

                    // Ring color cools from hot blue-white at birth to
                    // orange by the time it reaches the corners.
                    float rt = radius / max_radius;
                    if (rt > 1) rt = 1;
                    float wr = 230.0f + rt *  25.0f;
                    float wg = 240.0f - rt * 120.0f;
                    float wb = 255.0f - rt * 235.0f;

                    // Energy decays with age so the ring thins as it travels.
                    float life_env = 1.0f - s_waves[i].age / 1.8f;
                    if (life_env < 0) life_env = 0;
                    float w = k * life_env * 1.6f;

                    rr += wr * w;
                    gg += wg * w;
                    bb += wb * w;
                }

                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb > 255) bb = 255;
                if (rr < 1 && gg < 1 && bb < 1) continue;
                render_set((cube_face_t)f, x, y,
                           (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
            }
        }
    }
}

const effect_vtable_t g_effect_supernova = {
    .name = "supernova", .id = EFFECT_SUPERNOVA,
    .enter = supernova_enter, .step = supernova_step,
};
