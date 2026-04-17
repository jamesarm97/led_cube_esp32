#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>

// Spiral galaxy: each face hosts a rotating 2-arm log-spiral. Arms are
// brightest on their leading edge and fade out behind — that's the "trail
// dimming off as it gets left behind" look.
//
// Per-pixel math, in face-local coords (center = 3.5, 3.5):
//     r      = distance from center
//     theta  = angle
//     arm_t  = theta * N_ARMS + k * log(r + 0.5) - phase
//   intensity = saturate(1 - frac(arm_t / 2pi) ^ gamma)
//
// Small frac values (just after the arm's leading edge) -> bright. Large
// frac values (just before the next arm) -> dim. This makes every arm look
// like a streak with a sharp head and a fading tail, which wraps naturally
// as the galaxy spins.
//
// BOTTOM spins opposite to TOP so the cube reads as one rotating body;
// side faces inherit TOP's phase offset by face index, which keeps the
// motion coherent at the seams even though the math is per-face.

#define N_ARMS   2
static const float K_PITCH = 2.0f;   // how tight the spiral is (bigger = tighter)
static const float GAMMA   = 2.2f;   // trail falloff exponent

// Differential rotation: inner pixels spin faster than outer ones, mimicking
// a real galaxy's rotation curve. omega(r) = 1 + K / (r + eps).
static const float DIFF_K   = 1.8f;
static const float DIFF_EPS = 0.8f;

// Each face has a distinct "edge" hue; the center is a consistent deep
// purple. Hue is HSV-space [0..1]: 0.5≈cyan, 0.62≈blue, 0.72≈violet, 0.82≈magenta.
static const float CENTER_HUE = 0.82f;               // deep purple/magenta
static const float EDGE_HUE[CUBE_FACE_COUNT] = {
    [FACE_TOP]    = 0.58f,  // azure
    [FACE_BOTTOM] = 0.62f,  // blue
    [FACE_NORTH]  = 0.50f,  // cyan
    [FACE_SOUTH]  = 0.70f,  // blue-violet
    [FACE_EAST]   = 0.48f,  // teal
    [FACE_WEST]   = 0.76f,  // violet
};

static float s_phase;

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b) {
    h = h - floorf(h);
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    int seg = (int)(h * 6.0f);
    switch (seg) {
        case 0: rp = c; gp = x; bp = 0; break;
        case 1: rp = x; gp = c; bp = 0; break;
        case 2: rp = 0; gp = c; bp = x; break;
        case 3: rp = 0; gp = x; bp = c; break;
        case 4: rp = x; gp = 0; bp = c; break;
        default:rp = c; gp = 0; bp = x; break;
    }
    *r = (uint8_t)((rp + m) * 255);
    *g = (uint8_t)((gp + m) * 255);
    *b = (uint8_t)((bp + m) * 255);
}

static void galaxy_enter(void) { s_phase = 0; }

static void galaxy_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();

    // Overall spin is roughly twice as fast as before. Positive = CCW on TOP.
    s_phase += dt * (0.7f + speed / 255.0f * 2.5f);

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        // BOTTOM spins the other way so top/bottom together read as a disk.
        float face_phase = s_phase + f * 0.35f;
        float spin_dir   = (f == FACE_BOTTOM) ? -1.0f : 1.0f;

        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float dx = x - 3.5f;
                float dy = y - 3.5f;
                float r  = sqrtf(dx * dx + dy * dy);
                if (r < 0.001f) r = 0.001f;
                float theta = atan2f(dy, dx);

                // Differential rotation: the per-pixel angular velocity
                // factor grows as r shrinks, so the core spins noticeably
                // faster than the rim. This distorts a rigid log-spiral but
                // that distortion is *what a galaxy actually does* — arms
                // wind up tighter over time in the center.
                float omega = 1.0f + DIFF_K / (r + DIFF_EPS);
                float arm_t = theta * N_ARMS
                            + K_PITCH * logf(r + 0.5f)
                            - spin_dir * face_phase * omega;

                // Normalize arm_t / (2pi) into a [0, 1) "position behind the arm head".
                float frac = arm_t / 6.2832f;
                frac -= floorf(frac);

                // Fade from bright (frac=0, just past the head) to dim (frac→1).
                float tail = 1.0f - frac;
                float intensity = powf(tail, GAMMA);

                // Radial density: bright center, dimmer edges.
                float radial = expf(-r * 0.22f);
                intensity *= radial * 1.5f;
                if (intensity > 1.0f) intensity = 1.0f;
                if (intensity < 0.02f) continue;

                // Color: lerp from CENTER_HUE (deep purple) at r=0 to the
                // face's distinct EDGE_HUE at r=5. Saturation also climbs
                // with r so the core reads cleaner/whiter before blooming
                // to a saturated edge hue.
                float t = r / 5.0f; if (t > 1) t = 1;
                float hue = CENTER_HUE + (EDGE_HUE[f] - CENTER_HUE) * t;
                float sat = 0.55f + 0.45f * t;
                uint8_t rr, gg, bb;
                hsv_to_rgb(hue, sat, intensity, &rr, &gg, &bb);
                render_set((cube_face_t)f, x, y, rr, gg, bb);
            }
        }
    }
}

const effect_vtable_t g_effect_galaxy = {
    .name = "galaxy", .id = EFFECT_GALAXY,
    .enter = galaxy_enter, .step = galaxy_step,
};
