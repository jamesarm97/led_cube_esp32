#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"
#include "esp_random.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// DNA double helix: two phosphate-backbone strands twist around the
// vertical cube axis. A series of short "rung" segments connect
// matching points on the two strands at regular y intervals, each
// colored as a random A-T (red/blue) or C-G (yellow/green) base pair.
// The whole helix rotates slowly around the vertical axis so all four
// side faces see the strands crossing in and out of view.
//
// Orientation-independent — always twists around cube-world +y.

#define DNA_RUNGS 10

typedef struct {
    uint8_t r_left,  g_left,  b_left;   // color at strand A end
    uint8_t r_right, g_right, b_right;  // color at strand B end
} rung_color_t;

static rung_color_t s_rungs[DNA_RUNGS];
static float s_px[CUBE_FACE_COUNT][8][8];
static float s_py[CUBE_FACE_COUNT][8][8];
static float s_pz[CUBE_FACE_COUNT][8][8];
static float s_time;

// Strand shape constants. The helix lives just inside the cube shell so
// the strands actually graze the side faces as they twist — otherwise a
// narrow helix sits in the middle of the cube and renders almost nothing
// on the outward-facing LEDs.
static const float S_CX      = 0.5f;     // helix axis x
static const float S_CZ      = 0.5f;     // helix axis z
static const float S_RADIUS  = 0.46f;    // strand radius from axis
static const float S_TWIST   = 11.0f;    // twist rate (radians per cube-y)
static const float S_R_STRND = 0.14f;    // thickness of a strand
static const float S_R_RUNG  = 0.10f;    // thickness of a rung
static const float S_Y_LO    = 0.05f;    // helix y-range
static const float S_Y_HI    = 0.95f;

static inline float sqf(float v) { return v * v; }

// Signed square distance from point P to the infinite line segment
// running from A to B (clamped to the segment).
static float seg_dist2(float px, float py, float pz,
                       float ax, float ay, float az,
                       float bx, float by, float bz) {
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    float l2 = dx * dx + dy * dy + dz * dz;
    if (l2 < 1e-6f) {
        return sqf(px - ax) + sqf(py - ay) + sqf(pz - az);
    }
    float t = ((px - ax) * dx + (py - ay) * dy + (pz - az) * dz) / l2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float cx = ax + dx * t, cy = ay + dy * t, cz = az + dz * t;
    return sqf(px - cx) + sqf(py - cy) + sqf(pz - cz);
}

static void dna_enter(void) {
    s_time = 0;
    // Seed random base pairs. A-T = red/blue, T-A = blue/red,
    // C-G = yellow/green, G-C = green/yellow.
    for (int i = 0; i < DNA_RUNGS; i++) {
        uint32_t r = esp_random() & 3;
        switch (r) {
            case 0:
                s_rungs[i] = (rung_color_t){ 220, 40, 40,   40, 60, 220 }; break; // A-T
            case 1:
                s_rungs[i] = (rung_color_t){  40, 60,220,  220, 40, 40 }; break;  // T-A
            case 2:
                s_rungs[i] = (rung_color_t){ 220,200, 40,   40,200, 80 }; break;  // C-G
            default:
                s_rungs[i] = (rung_color_t){  40,200, 80,  220,200, 40 }; break;  // G-C
        }
    }
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y], &s_py[f][x][y], &s_pz[f][x][y]);
            }
        }
    }
}

// Strand A: angle theta_A(y,t) = S_TWIST*y + rot(t)
// Strand B: theta_B = theta_A + pi
static void strand_pos(int which, float y, float t,
                       float *sx, float *sy, float *sz) {
    float theta = S_TWIST * y + t;
    if (which) theta += 3.14159265f;
    *sx = S_CX + S_RADIUS * cosf(theta);
    *sy = y;
    *sz = S_CZ + S_RADIUS * sinf(theta);
}

static void dna_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();
    float rot_speed = 0.30f + (speed_knob / 255.0f) * 1.10f;
    s_time += dt * rot_speed;

    // Precompute rung endpoints for this frame.
    float rung_y[DNA_RUNGS];
    float a_x[DNA_RUNGS], a_z[DNA_RUNGS];
    float b_x[DNA_RUNGS], b_z[DNA_RUNGS];
    for (int i = 0; i < DNA_RUNGS; i++) {
        float fr = (i + 0.5f) / (float)DNA_RUNGS;
        float y  = S_Y_LO + (S_Y_HI - S_Y_LO) * fr;
        rung_y[i] = y;
        float sx, sy, sz;
        strand_pos(0, y, s_time, &sx, &sy, &sz);
        a_x[i] = sx; a_z[i] = sz;
        strand_pos(1, y, s_time, &sx, &sy, &sz);
        b_x[i] = sx; b_z[i] = sz;
    }

    const float r_strand2 = S_R_STRND * S_R_STRND;
    const float r_rung2   = S_R_RUNG  * S_R_RUNG;

    // Sample the strand at many small segments. Walking y from S_Y_LO..S_Y_HI
    // in 40 steps and testing consecutive pairs as line segments is enough
    // for smooth antialiased-looking helices on an 8x8 panel.
    const int STEPS = 40;
    float s0x[2][STEPS + 1], s0y[STEPS + 1], s0z[2][STEPS + 1];
    for (int k = 0; k <= STEPS; k++) {
        float y = S_Y_LO + (S_Y_HI - S_Y_LO) * k / STEPS;
        s0y[k] = y;
        float sx, sy, sz;
        strand_pos(0, y, s_time, &sx, &sy, &sz);
        s0x[0][k] = sx; s0z[0][k] = sz;
        strand_pos(1, y, s_time, &sx, &sy, &sz);
        s0x[1][k] = sx; s0z[1][k] = sz;
    }

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px = s_px[f][x][y];
                float py = s_py[f][x][y];
                float pz = s_pz[f][x][y];

                if (py < S_Y_LO - 0.08f || py > S_Y_HI + 0.08f) continue;

                // Nearest strand A segment.
                float best_a = 1e9f, best_b = 1e9f;
                int a_idx = 0, b_idx = 0;
                for (int k = 0; k < STEPS; k++) {
                    float d2a = seg_dist2(px, py, pz,
                                          s0x[0][k],     s0y[k],     s0z[0][k],
                                          s0x[0][k + 1], s0y[k + 1], s0z[0][k + 1]);
                    if (d2a < best_a) { best_a = d2a; a_idx = k; }
                    float d2b = seg_dist2(px, py, pz,
                                          s0x[1][k],     s0y[k],     s0z[1][k],
                                          s0x[1][k + 1], s0y[k + 1], s0z[1][k + 1]);
                    if (d2b < best_b) { best_b = d2b; b_idx = k; }
                }

                float rr = 0, gg = 0, bb = 0;
                bool hit = false;

                if (best_a < r_strand2) {
                    // Backbone A — warm lavender-white.
                    float k = 1.0f - sqrtf(best_a) / S_R_STRND;
                    if (k < 0) k = 0;
                    rr += 220.0f * k; gg += 180.0f * k; bb += 255.0f * k;
                    hit = true;
                    (void)a_idx;
                }
                if (best_b < r_strand2) {
                    float k = 1.0f - sqrtf(best_b) / S_R_STRND;
                    if (k < 0) k = 0;
                    rr += 220.0f * k; gg += 180.0f * k; bb += 255.0f * k;
                    hit = true;
                    (void)b_idx;
                }

                // Nearest rung.
                for (int i = 0; i < DNA_RUNGS; i++) {
                    float dy = py - rung_y[i];
                    if (fabsf(dy) > S_R_RUNG * 1.8f) continue;
                    float d2 = seg_dist2(px, py, pz,
                                         a_x[i], rung_y[i], a_z[i],
                                         b_x[i], rung_y[i], b_z[i]);
                    if (d2 >= r_rung2) continue;
                    // Interpolate color along rung from A-color to B-color.
                    float dax = px - a_x[i], daz = pz - a_z[i];
                    float dbx = b_x[i] - a_x[i], dbz = b_z[i] - a_z[i];
                    float len2 = dbx * dbx + dbz * dbz + 1e-6f;
                    float t = (dax * dbx + daz * dbz) / len2;
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    float k = 1.0f - sqrtf(d2) / S_R_RUNG;
                    if (k < 0) k = 0;
                    float cr = s_rungs[i].r_left  * (1 - t) + s_rungs[i].r_right * t;
                    float cg = s_rungs[i].g_left  * (1 - t) + s_rungs[i].g_right * t;
                    float cb = s_rungs[i].b_left  * (1 - t) + s_rungs[i].b_right * t;
                    rr += cr * k;
                    gg += cg * k;
                    bb += cb * k;
                    hit = true;
                }

                if (!hit) continue;
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb > 255) bb = 255;
                render_set((cube_face_t)f, x, y,
                           (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
            }
        }
    }
}

const effect_vtable_t g_effect_dna = {
    .name = "dna", .id = EFFECT_DNA,
    .enter = dna_enter, .step = dna_step,
};
