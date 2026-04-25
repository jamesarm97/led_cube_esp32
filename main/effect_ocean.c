#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Ocean: a volumetric sea fills the cube to its equator. Traveling waves
// roll across the (x, z) plane, building up on a slow "storm" cycle that
// swells from glassy calm to chop and back. Where crests peak and the
// wave face is steep, the surface breaks into white foam caps. Side
// faces show the cross-section of the water with a moving surface line;
// TOP shows the sea surface from above; BOTTOM is the dark seabed.
//
// Orientation-independent: everything is computed in cube-world (+y up).

#define OCEAN_COMPONENTS 4

typedef struct {
    float kx, kz;    // wavenumber vector (radians / cube-unit)
    float omega;     // temporal frequency (radians / sec)
    float base_amp;  // baseline amplitude, in cube-units
    float swell_f;   // per-component breathing frequency
    float swell_ph;  // per-component breathing phase offset
} wave_c_t;

static const wave_c_t s_waves[OCEAN_COMPONENTS] = {
    //   kx     kz    omega  base_amp  swell_f  swell_ph
    {   8.0f,  3.0f,  1.30f,   0.028f,   0.18f,   0.00f },
    {   4.0f, -7.0f,  1.05f,   0.035f,   0.13f,   1.30f },
    {  12.0f,  6.0f,  1.70f,   0.018f,   0.27f,   2.50f },
    {   3.0f,  2.0f,  0.55f,   0.046f,   0.09f,   4.10f },
};

static float s_time;          // wave-phase clock (sped by rainbow_speed knob)
static float s_storm_clock;   // drives the build-up -> break -> calm cycle

static float s_px[CUBE_FACE_COUNT][8][8];
static float s_py[CUBE_FACE_COUNT][8][8];
static float s_pz[CUBE_FACE_COUNT][8][8];

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Slow bell-curve envelope: starts near 0 (glassy), ramps up to a peak
// in the middle of the cycle, falls back to glassy. Never exactly 0 so
// there's always a subtle ripple.
static float storm_envelope(void) {
    const float cycle = 22.0f;
    float p = fmodf(s_storm_clock, cycle) / cycle;   // 0..1
    float s = sinf(p * 3.14159265f);                 // 0 -> 1 -> 0
    s = s * s;                                        // sharpen to a bell
    return 0.20f + s * 1.55f;                         // 0.20 .. 1.75
}

// Water surface displacement at horizontal (x, z) at current time. Sum
// of 4 traveling sines each modulated by its own slow breathing gain,
// scaled by the global storm envelope. Return is signed, zero = calm
// water at y = water_level.
static float surface_h(float x, float z, float storm) {
    float h = 0;
    for (int i = 0; i < OCEAN_COMPONENTS; i++) {
        const wave_c_t *w = &s_waves[i];
        float env   = 0.55f + 0.45f * sinf(s_storm_clock * w->swell_f + w->swell_ph);
        float phase = w->kx * x + w->kz * z - w->omega * s_time;
        h += w->base_amp * env * sinf(phase);
    }
    return h * storm;
}

static void ocean_enter(void) {
    s_time = 0;
    s_storm_clock = 0;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y], &s_py[f][x][y], &s_pz[f][x][y]);
            }
        }
    }
}

static void ocean_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();

    // rainbow_speed knob drives wave travel and storm build-up speed.
    // Low knob = placid sea, high knob = stormy chop.
    float speed = 0.45f + (speed_knob / 255.0f) * 1.5f;
    s_time        += dt * speed;
    s_storm_clock += dt * speed * 0.80f;

    const float water_level = 0.50f;   // mean-water plane in cube-world
    const float wave_scale  = 0.22f;   // vertical reach of surface_h=1
    const float foam_band   = 0.055f;  // thickness of surface band
    const float body_sigma  = 0.45f;   // depth-of-light decay

    float storm = storm_envelope();

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px = s_px[f][x][y];
                float py = s_py[f][x][y];
                float pz = s_pz[f][x][y];

                float h_here    = surface_h(px, pz, storm);
                float surface_y = water_level + h_here * wave_scale;
                float rel       = py - surface_y; // + above water, - below

                // Cheap early-out: most pixels are clearly above or below
                // the thin surface band. Only compute the slope needed for
                // white caps when we're inside the band.
                float rr = 0, gg = 0, bb = 0;

                if (rel > foam_band) {
                    // Above water: the sky over the ocean is dark.
                    continue;
                } else if (rel > -foam_band) {
                    // Surface band: compute local slope via central
                    // differences. A steep wave face + high crest = foam.
                    const float eps = 0.06f;
                    float hx = (surface_h(px + eps, pz, storm)
                              - surface_h(px - eps, pz, storm)) * (1.0f / (2 * eps));
                    float hz = (surface_h(px, pz + eps, storm)
                              - surface_h(px, pz - eps, storm)) * (1.0f / (2 * eps));
                    float slope = sqrtf(hx * hx + hz * hz);

                    // Crest-ness: how high this point is relative to the
                    // maximum the sea is currently reaching. Only the
                    // upper portion of each swell foams.
                    float peak_scale = 1.0f / (0.18f * storm + 0.02f);
                    float crest = clampf(h_here * wave_scale * peak_scale, 0, 1);
                    float steep = clampf(slope * 0.30f, 0, 1);
                    float foam  = clampf(crest * 0.55f + steep * 0.75f - 0.25f, 0, 1);
                    foam = foam * foam * (3.0f - 2.0f * foam);   // smoothstep

                    // Soft envelope around the exact surface.
                    float band_env = 1.0f - (fabsf(rel) / foam_band);
                    if (band_env < 0) band_env = 0;
                    band_env = band_env * band_env * (3.0f - 2.0f * band_env);

                    // Base sea surface is turquoise; foam washes it to white.
                    float wr = 30.0f  + foam * 225.0f;
                    float wg = 140.0f + foam * 115.0f;
                    float wb = 210.0f + foam * 45.0f;
                    rr = wr * band_env;
                    gg = wg * band_env;
                    bb = wb * band_env;
                } else {
                    // Below surface. Light attenuates with depth; color
                    // drifts turquoise -> navy as you sink.
                    float depth = -rel;
                    float k = expf(-depth / body_sigma);
                    float t = clampf(depth / 0.45f, 0, 1);
                    float shallow_r = 15.0f, shallow_g = 85.0f,  shallow_b = 165.0f;
                    float deep_r    =  1.0f, deep_g    =  8.0f,  deep_b    =  45.0f;
                    float tint = 0.40f + 0.60f * k;
                    rr = (shallow_r * (1 - t) + deep_r * t) * tint;
                    gg = (shallow_g * (1 - t) + deep_g * t) * tint;
                    bb = (shallow_b * (1 - t) + deep_b * t) * tint;

                    // Subtle caustic shimmer near the surface.
                    float caustic = 0.5f + 0.5f *
                        sinf((px * 17.0f + pz * 13.0f) - s_time * 2.2f);
                    caustic *= k * 0.30f;
                    rr +=  8.0f * caustic;
                    gg += 35.0f * caustic;
                    bb += 55.0f * caustic;
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

const effect_vtable_t g_effect_ocean = {
    .name = "ocean", .id = EFFECT_OCEAN,
    .enter = ocean_enter, .step = ocean_step,
};
