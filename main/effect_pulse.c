#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Pulse: periodic concentric shockwaves emanating from the orientation's
// top pole. In face-up mode that's the TOP face's center; in corner-up
// mode it's the 3 corner pixels at the TOP+WEST+SOUTH vertex. Each pulse
// is a BFS-distance ring that moves outward at a fixed step rate. Up to
// a few pulses coexist at different radii so the wave train reads clearly.

#define MAX_PULSES 4

typedef struct {
    bool     alive;
    float    front;   // current leading-edge BFS distance, advances each frame
    float    life;    // seconds since spawn
    uint8_t  r, g, b;
} pulse_t;

static pulse_t s_pulses[MAX_PULSES];
static float   s_spawn_timer;
static float   s_phase;

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void spawn_pulse(void) {
    for (int i = 0; i < MAX_PULSES; i++) {
        if (s_pulses[i].alive) continue;
        s_pulses[i].alive = true;
        s_pulses[i].front = -1.0f; // start just inside the top pole
        s_pulses[i].life  = 0;
        // Cycle through a few warm/cool palettes for variety.
        static const uint8_t PAL[][3] = {
            { 60, 180, 255}, {255, 180,  60}, {255,  80, 180},
            {120, 255, 200}, {220, 120, 255}, {255, 220, 120},
        };
        int p = rng() % 6;
        s_pulses[i].r = PAL[p][0];
        s_pulses[i].g = PAL[p][1];
        s_pulses[i].b = PAL[p][2];
        return;
    }
}

static void pulse_enter(void) {
    memset(s_pulses, 0, sizeof(s_pulses));
    orient_build_flow_from_top();
    s_spawn_timer = 0;
    s_phase = 0;
}

static void pulse_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;
    s_phase += dt;

    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();

    // Pulse expansion rate in BFS-steps per second. Higher speed knob =
    // faster shockwave.
    float expand_rate = 6.0f + (speed / 255.0f) * 10.0f;

    // Spawn cadence: one pulse every ~0.9 s.
    s_spawn_timer -= dt;
    if (s_spawn_timer <= 0) {
        spawn_pulse();
        s_spawn_timer = 0.8f + rngf() * 0.3f;
    }

    // Advance existing pulses and retire anything past max_dist + tail.
    float max_d = (float)orient_flow_max_dist;
    for (int i = 0; i < MAX_PULSES; i++) {
        pulse_t *pp = &s_pulses[i];
        if (!pp->alive) continue;
        pp->front += dt * expand_rate;
        pp->life  += dt;
        if (pp->front > max_d + 3.0f) pp->alive = false;
    }

    // Render: each pixel gets the summed contribution from all pulses.
    // A pulse at `front` contributes at pixel-dist d with a Gaussian shape
    // centered on `front`, width 1.2 steps.
    const float sigma = 1.2f;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float d = (float)orient_flow_dist[f][x][y];
                float rr = 0, gg = 0, bb = 0;
                for (int i = 0; i < MAX_PULSES; i++) {
                    if (!s_pulses[i].alive) continue;
                    float delta = d - s_pulses[i].front;
                    if (fabsf(delta) > 2.5f * sigma) continue;
                    float k = expf(-(delta * delta) / (2 * sigma * sigma));
                    // Global fade as the pulse ages so later rings dim naturally.
                    float age_k = 1.0f - s_pulses[i].life / 3.0f;
                    if (age_k < 0) age_k = 0;
                    float w = k * age_k;
                    rr += s_pulses[i].r * w;
                    gg += s_pulses[i].g * w;
                    bb += s_pulses[i].b * w;
                }
                if (rr < 2 && gg < 2 && bb < 2) continue;
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb > 255) bb = 255;
                render_set((cube_face_t)f, x, y,
                           (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
            }
        }
    }
}

const effect_vtable_t g_effect_pulse = {
    .name = "pulse", .id = EFFECT_PULSE,
    .enter = pulse_enter, .step = pulse_step,
};
