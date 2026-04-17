#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Cube-shaped "disco ball". A chosen axis runs through the cube — vertical
// (Y-axis) in face-up mode, or the diagonal through the up-corner and down-
// corner in corner-up mode. N vertical stripes are defined at fixed
// azimuth angles around that axis; each stripe is a vertical gradient
// (its color hue drifts with height along the axis). The whole stripe set
// rotates once every few seconds.
//
// Every pixel has two precomputed scalars: its azimuth angle around the
// axis, and its normalized height (-1 at the down pole, +1 at the up
// pole). Each frame we:
//   1. Decay a per-pixel 3-channel fade buffer (exponential).
//   2. For each pixel, check how close its azimuth is to any rotating
//      stripe; if close, paint the stripe's per-height color into the
//      fade buffer (taking the max so overlapping stripes look additive).
//   3. Render base_color + fade_buffer, with the base itself shaded by
//      height so the cube has a subtle top/bottom tint.
//
// The fade buffer is what gives the "trail" — after a stripe sweeps past
// a pixel, the paint fades back to base color over ~0.5 s.

#define N_STRIPES       6
// Half-thickness of each stripe, measured in CUBE-world units (perp
// distance from the stripe's plane). 0.17 gives roughly the same coverage
// as a ~22° angular width at the equator (radius ≈ 0.5) but — crucially —
// also illuminates pixels right next to the axis at the poles, so stripes
// converge visibly into the TOP/BOTTOM face centers instead of fading out.
#define STRIPE_WIDTH_W  0.17f

static float s_axis[3];
static float s_basis_u[3];
static float s_basis_v[3];
static float s_height_max;

// Precomputed per-pixel geometry: (pu, pv) = pixel's position in the 2D
// perpendicular plane (projected onto the axis basis); height = normalized
// axial coordinate in [-1, +1] (−1 at the down pole, +1 at the up pole).
static float s_pu    [CUBE_FACE_COUNT][8][8];
static float s_pv    [CUBE_FACE_COUNT][8][8];
static float s_height[CUBE_FACE_COUNT][8][8];
static float s_fade  [CUBE_FACE_COUNT][8][8][3]; // RGB fade accumulator

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

// Set up axis and a perpendicular basis (u, v) for mapping (px, py, pz)
// onto (height, azimuth).
static void setup_axes(void) {
    if (orient_get() == ORIENT_CORNER_UP) {
        // Axis runs through the down-corner (1, 0, 0) and up-corner
        // (0, 1, 1). Unit vector from cube center toward up-corner:
        //   up_corner - center = (-0.5, +0.5, +0.5) → unit = (-1, 1, 1)/√3.
        float inv = 1.0f / sqrtf(3.0f);
        s_axis[0] = -inv; s_axis[1] = inv; s_axis[2] = inv;

        // Perpendicular basis: u = normalize(axis × X̂).
        //   axis × (1, 0, 0) = (0, axis_z, -axis_y) = (0, 1/√3, -1/√3)
        //   |.| = √(2/3)
        float mag = sqrtf(2.0f / 3.0f);
        s_basis_u[0] = 0.0f;
        s_basis_u[1] = inv / mag;
        s_basis_u[2] = -inv / mag;
        // v = axis × u (right-handed → forms an orthonormal frame)
        s_basis_v[0] = s_axis[1] * s_basis_u[2] - s_axis[2] * s_basis_u[1];
        s_basis_v[1] = s_axis[2] * s_basis_u[0] - s_axis[0] * s_basis_u[2];
        s_basis_v[2] = s_axis[0] * s_basis_u[1] - s_axis[1] * s_basis_u[0];

        // Axial projections of cube corners span ±√3/2 around center.
        s_height_max = sqrtf(3.0f) / 2.0f;
    } else {
        // Face-up: Y-axis vertical, basis = (X, Z).
        s_axis[0] = 0; s_axis[1] = 1; s_axis[2] = 0;
        s_basis_u[0] = 1; s_basis_u[1] = 0; s_basis_u[2] = 0;
        s_basis_v[0] = 0; s_basis_v[1] = 0; s_basis_v[2] = 1;
        s_height_max = 0.5f;  // |py - 0.5| reaches 0.5 at TOP/BOTTOM faces
    }
}

static void precompute_pixels(void) {
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px, py, pz;
                orient_pixel_pos3d((cube_face_t)f, x, y, &px, &py, &pz);
                float dx = px - 0.5f;
                float dy = py - 0.5f;
                float dz = pz - 0.5f;
                float h = dx * s_axis[0] + dy * s_axis[1] + dz * s_axis[2];
                float perp_x = dx - h * s_axis[0];
                float perp_y = dy - h * s_axis[1];
                float perp_z = dz - h * s_axis[2];
                float pu = perp_x * s_basis_u[0] + perp_y * s_basis_u[1] + perp_z * s_basis_u[2];
                float pv = perp_x * s_basis_v[0] + perp_y * s_basis_v[1] + perp_z * s_basis_v[2];
                s_pu    [f][x][y] = pu;
                s_pv    [f][x][y] = pv;
                s_height[f][x][y] = h / s_height_max;
                if (s_height[f][x][y] < -1) s_height[f][x][y] = -1;
                if (s_height[f][x][y] >  1) s_height[f][x][y] =  1;
            }
        }
    }
}

static void disco_enter(void) {
    setup_axes();
    precompute_pixels();
    memset(s_fade, 0, sizeof(s_fade));
    s_phase = 0;
}

// Stripe i's color at normalized height h (in [-1, +1]): a gradient between
// two hues, picked from the color wheel to be complementary so the gradient
// reads clearly. hue_top = i/N, hue_bot = (i + N/2)/N wraps around.
static void stripe_color(int i, float h, uint8_t *r, uint8_t *g, uint8_t *b) {
    float hue_top = (float)i / N_STRIPES;
    float hue_bot = fmodf(hue_top + 0.5f, 1.0f);
    float t = (h + 1.0f) * 0.5f; // 0 at bottom pole, 1 at top pole
    // Pick shortest-path hue interpolation by picking the hue closer on
    // the color wheel.
    float diff = hue_top - hue_bot;
    if (diff > 0.5f)  diff -= 1.0f;
    if (diff < -0.5f) diff += 1.0f;
    float hue = hue_bot + diff * t;
    hsv_to_rgb(hue, 1.0f, 1.0f, r, g, b);
}

static void disco_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();

    // Rotation: ~0.6 rad/s (one turn per ~10 s) at min speed,
    // ~3.0 rad/s (one turn per ~2 s) at max.
    s_phase += dt * (0.6f + (speed / 255.0f) * 2.4f);

    // Fade decay. decay_k = 2 means fade reaches ~e^-2 ≈ 13% after 1 s.
    const float decay_k = 2.2f;
    float decay = expf(-decay_k * dt);

    // Base cube color — dim muted purple so stripe color pops off it.
    const float BASE_R = 18.0f, BASE_G = 8.0f, BASE_B = 28.0f;

    // Precompute (cos, sin) for each stripe's angle once per frame rather
    // than inside the tight pixel loop.
    float stripe_c[N_STRIPES], stripe_s[N_STRIPES];
    for (int i = 0; i < N_STRIPES; i++) {
        float a = s_phase + (i * 6.2831853f / N_STRIPES);
        stripe_c[i] = cosf(a);
        stripe_s[i] = sinf(a);
    }

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                // Decay existing fade first.
                s_fade[f][x][y][0] *= decay;
                s_fade[f][x][y][1] *= decay;
                s_fade[f][x][y][2] *= decay;

                float pu = s_pu    [f][x][y];
                float pv = s_pv    [f][x][y];
                float hh = s_height[f][x][y];

                // Each stripe is a half-plane containing the axis and
                // extending outward in direction (cos θ_s, sin θ_s) in the
                // perpendicular plane. For a pixel at (pu, pv):
                //   along = pu·cos + pv·sin   (component along stripe ray)
                //   dperp = |pv·cos - pu·sin| (perpendicular distance to stripe plane)
                // The along > 0 check keeps the stripe as a RAY (not a line
                // through the axis), so opposite stripes don't both claim
                // the same pixel. Pixels near the axis have tiny (pu, pv)
                // and small dperp, so every stripe whose ray happens to
                // point toward them contributes — that's what gives the
                // starburst convergence at the TOP/BOTTOM pole.
                for (int i = 0; i < N_STRIPES; i++) {
                    float c = stripe_c[i], sn = stripe_s[i];
                    float along = pu * c + pv * sn;
                    if (along < 0) continue;
                    float dperp = fabsf(pv * c - pu * sn);
                    if (dperp > STRIPE_WIDTH_W) continue;
                    float k = 1.0f - dperp / STRIPE_WIDTH_W;
                    k = k * k;
                    // Boost intensity near the pole so the converging
                    // starburst pops — at the equator the intensity is
                    // already dominated by k, but near the pole the pixel
                    // sits dead on a single stripe and we want it bright.
                    float intensity = k * 1.4f;

                    uint8_t sr, sg, sb;
                    stripe_color(i, hh, &sr, &sg, &sb);
                    float cr = sr * intensity;
                    float cg = sg * intensity;
                    float cb = sb * intensity;
                    if (cr > s_fade[f][x][y][0]) s_fade[f][x][y][0] = cr;
                    if (cg > s_fade[f][x][y][1]) s_fade[f][x][y][1] = cg;
                    if (cb > s_fade[f][x][y][2]) s_fade[f][x][y][2] = cb;
                }

                // Base color shaded subtly: a bit brighter at the equator
                // and dimmer at the poles (|height| close to 1), preserving
                // a sense of a 3D object when no stripe is present.
                float equator_hint = 1.0f - fabsf(hh);
                if (equator_hint < 0) equator_hint = 0;
                float base_shade = 0.65f + 0.35f * equator_hint;
                float fr = BASE_R * base_shade + s_fade[f][x][y][0];
                float fg = BASE_G * base_shade + s_fade[f][x][y][1];
                float fb = BASE_B * base_shade + s_fade[f][x][y][2];
                if (fr > 255) fr = 255;
                if (fg > 255) fg = 255;
                if (fb > 255) fb = 255;
                render_set((cube_face_t)f, x, y,
                           (uint8_t)fr, (uint8_t)fg, (uint8_t)fb);
            }
        }
    }
}

const effect_vtable_t g_effect_disco = {
    .name = "disco", .id = EFFECT_DISCO,
    .enter = disco_enter, .step = disco_step,
};
