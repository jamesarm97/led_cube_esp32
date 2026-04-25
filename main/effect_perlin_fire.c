#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Perlin fire: a volumetric heat field fills the entire cube. 3D value
// noise (two octaves, with the second octave advected upward over time)
// drives local heat at each pixel, multiplied by a "fuel" term that
// falls off with height. Hot cells glow white-yellow; cooler cells fade
// through orange, red, and finally black. Unlike the face-local fire
// effect, this one is volumetric — every face reads a consistent 3D
// flame rising from the cube floor.
//
// Orientation-independent — fuel is measured against cube-world +y.

static float s_px[CUBE_FACE_COUNT][8][8];
static float s_py[CUBE_FACE_COUNT][8][8];
static float s_pz[CUBE_FACE_COUNT][8][8];
static float s_t;

static inline uint32_t hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u
               + (uint32_t)y * 668265263u
               + (uint32_t)z * 1274126177u;
    h ^= (h >> 13);
    h *= 1274126177u;
    return h ^ (h >> 16);
}

static inline float vat(int x, int y, int z) {
    return ((int32_t)(hash3(x, y, z) & 0xFFFFu) - 32768) * (1.0f / 32768.0f);
}

static inline float fadef(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

static float noise3(float x, float y, float z) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float dx = x - xi, dy = y - yi, dz = z - zi;
    float u = fadef(dx), v = fadef(dy), w = fadef(dz);
    float c000 = vat(xi, yi, zi),     c100 = vat(xi + 1, yi, zi);
    float c010 = vat(xi, yi + 1, zi), c110 = vat(xi + 1, yi + 1, zi);
    float c001 = vat(xi, yi, zi + 1), c101 = vat(xi + 1, yi, zi + 1);
    float c011 = vat(xi, yi + 1, zi + 1), c111 = vat(xi + 1, yi + 1, zi + 1);
    float a0 = c000 * (1 - u) + c100 * u;
    float a1 = c010 * (1 - u) + c110 * u;
    float a2 = c001 * (1 - u) + c101 * u;
    float a3 = c011 * (1 - u) + c111 * u;
    float b0 = a0 * (1 - v) + a1 * v;
    float b1 = a2 * (1 - v) + a3 * v;
    return b0 * (1 - w) + b1 * w;
}

// Heat 0..1 -> fire palette: black -> red -> orange -> yellow -> white.
static void heat_rgb(float h, float *r, float *g, float *b) {
    if (h < 0) h = 0;
    if (h > 1) h = 1;
    if (h < 0.33f) {
        float t = h / 0.33f;
        *r = t * 220.0f; *g = 0; *b = 0;
    } else if (h < 0.66f) {
        float t = (h - 0.33f) / 0.33f;
        *r = 220.0f + t * 35.0f;
        *g = t * 180.0f;
        *b = 0;
    } else {
        float t = (h - 0.66f) / 0.34f;
        *r = 255.0f;
        *g = 180.0f + t * 75.0f;
        *b = t * 210.0f;
    }
}

static void perlin_fire_enter(void) {
    s_t = 0;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y], &s_py[f][x][y], &s_pz[f][x][y]);
            }
        }
    }
}

static void perlin_fire_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();

    // rainbow_speed drives how quickly the flames rise and turbulence stirs.
    float rise = 0.9f + (speed_knob / 255.0f) * 2.2f;
    s_t += dt * rise;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px = s_px[f][x][y];
                float py = s_py[f][x][y];
                float pz = s_pz[f][x][y];

                // Two-octave turbulence; upward advection makes noise flow up.
                float n1 = noise3(px * 3.2f,  py * 3.2f  - s_t * 1.1f, pz * 3.2f);
                float n2 = noise3(px * 6.8f + 7.3f,
                                  py * 6.8f - s_t * 2.0f,
                                  pz * 6.8f + 11.1f) * 0.5f;
                float n  = 0.5f + 0.5f * (n1 + n2);

                // Fuel: concentrated at floor, dies off with height.
                float fuel = 1.0f - py;
                if (fuel < 0) fuel = 0;
                fuel = fuel * fuel;                         // sharper falloff

                float heat = n * fuel * 1.9f - 0.12f;
                if (heat <= 0) continue;
                if (heat > 1) heat = 1;

                float r, g, b;
                heat_rgb(heat, &r, &g, &b);
                render_set((cube_face_t)f, x, y,
                           (uint8_t)r, (uint8_t)g, (uint8_t)b);
            }
        }
    }
}

const effect_vtable_t g_effect_perlin_fire = {
    .name = "perlin_fire", .id = EFFECT_PERLIN_FIRE,
    .enter = perlin_fire_enter, .step = perlin_fire_step,
};
