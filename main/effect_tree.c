#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"
#include "esp_random.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// L-system tree: a trunk emerges from the floor, branches twice, blooms
// a canopy of green leaf-spheres, then cycles through the seasons —
// the canopy warms to autumn yellows/oranges/reds, leaves detach and
// flutter down under gravity, the tree goes bare. Then the whole cycle
// reseeds with a new tree shape.
//
// Orientation-independent — trunk is anchored at cube-world (0.5, 0, 0.5)
// growing along +y.

#define MAX_BRANCHES 24
#define MAX_LEAVES 18

typedef struct {
    float ax, ay, az;   // origin point
    float bx, by, bz;   // final tip once fully grown
    float start_t;      // seconds into cycle when growth starts
    float grow_dur;     // seconds to reach full length
    float thick;        // radius
    uint8_t depth;      // 0 = trunk, 1 = primary branch, 2 = twig
} branch_t;

typedef struct {
    float x, y, z;      // position
    float vx, vy, vz;   // velocity (used when detached)
    uint8_t alive;
    uint8_t attached;   // 1 = on the tree, 0 = free-falling
    uint8_t r, g, b;            // current color
    uint8_t base_r, base_g, base_b;  // summer green before any autumn tint
} leaf_t;

static branch_t s_br[MAX_BRANCHES];
static leaf_t   s_lv[MAX_LEAVES];
static int      s_br_count;
static int      s_lv_count;

static float s_px[CUBE_FACE_COUNT][8][8];
static float s_py[CUBE_FACE_COUNT][8][8];
static float s_pz[CUBE_FACE_COUNT][8][8];

static float s_phase_t;     // seconds into current cycle

static float rngf(void) { return (esp_random() & 0xFFFFFF) / (float)0xFFFFFF; }

static void add_branch(float ax, float ay, float az,
                       float bx, float by, float bz,
                       float start_t, float grow_dur,
                       float thick, int depth) {
    if (s_br_count >= MAX_BRANCHES) return;
    s_br[s_br_count++] = (branch_t){
        ax, ay, az, bx, by, bz, start_t, grow_dur, thick, (uint8_t)depth,
    };
}

static void seed_tree(void) {
    s_br_count = 0;
    s_lv_count = 0;

    // Trunk: straight up from the floor with slight jitter.
    float tip_x = 0.5f + (rngf() - 0.5f) * 0.06f;
    float tip_z = 0.5f + (rngf() - 0.5f) * 0.06f;
    float tip_y = 0.42f + rngf() * 0.08f;
    add_branch(0.5f, 0.02f, 0.5f, tip_x, tip_y, tip_z,
               0.0f, 2.2f, 0.055f, 0);

    // Primary branches: 3 around the trunk tip, spread ~120° apart.
    const int L1_N = 3;
    float base_angle = rngf() * 6.2832f;
    float L1_tip_x[3], L1_tip_y[3], L1_tip_z[3];
    for (int i = 0; i < L1_N; i++) {
        float a = base_angle + (6.2832f / L1_N) * i;
        float len = 0.18f + rngf() * 0.07f;
        L1_tip_x[i] = tip_x + cosf(a) * len * 0.85f;
        L1_tip_z[i] = tip_z + sinf(a) * len * 0.85f;
        L1_tip_y[i] = tip_y + len * 0.80f;
        add_branch(tip_x, tip_y, tip_z,
                   L1_tip_x[i], L1_tip_y[i], L1_tip_z[i],
                   2.2f, 1.6f, 0.040f, 1);
    }

    // Twigs: two per primary branch, each ending in a leaf.
    for (int i = 0; i < L1_N; i++) {
        for (int j = 0; j < 2; j++) {
            float a = base_angle + (6.2832f / L1_N) * i + (j ? 0.7f : -0.7f);
            float len = 0.13f + rngf() * 0.06f;
            float tx = L1_tip_x[i] + cosf(a) * len * 0.9f;
            float tz = L1_tip_z[i] + sinf(a) * len * 0.9f;
            float ty = L1_tip_y[i] + len * 0.55f;
            if (ty > 0.92f) ty = 0.92f;
            add_branch(L1_tip_x[i], L1_tip_y[i], L1_tip_z[i],
                       tx, ty, tz,
                       3.8f, 1.4f, 0.030f, 2);
            if (s_lv_count < MAX_LEAVES) {
                leaf_t *lv = &s_lv[s_lv_count++];
                lv->x = tx; lv->y = ty; lv->z = tz;
                lv->vx = lv->vy = lv->vz = 0;
                lv->alive = 1;
                lv->attached = 1;
                uint8_t gr = 100 + (esp_random() % 70);
                uint8_t gg = 170 + (esp_random() % 70);
                uint8_t gb =  30 + (esp_random() % 40);
                lv->base_r = gr; lv->base_g = gg; lv->base_b = gb;
                lv->r = gr; lv->g = gg; lv->b = gb;
            }
        }
    }
}

static void tree_enter(void) {
    s_phase_t = 0;
    seed_tree();
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y], &s_py[f][x][y], &s_pz[f][x][y]);
            }
        }
    }
}

static float seg_dist(float px, float py, float pz,
                      float ax, float ay, float az,
                      float bx, float by, float bz) {
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    float l2 = dx * dx + dy * dy + dz * dz;
    float t = 0;
    if (l2 > 1e-6f) {
        t = ((px - ax) * dx + (py - ay) * dy + (pz - az) * dz) / l2;
    }
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float cx = ax + dx * t, cy = ay + dy * t, cz = az + dz * t;
    float ex = px - cx, ey = py - cy, ez = pz - cz;
    return sqrtf(ex * ex + ey * ey + ez * ez);
}

// Blend current leaf color toward warm autumn colors by progress t in [0,1].
static void autumn_tint(leaf_t *lv, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    // Per-leaf target nudge so the canopy becomes a palette, not one color.
    uintptr_t h = (uintptr_t)lv;
    float tr = 210.0f + (h      & 0x1F);
    float tg =  70.0f + ((h>>2) & 0x3F);
    float tb =  10.0f;
    lv->r = (uint8_t)(lv->base_r * (1 - t) + tr * t);
    lv->g = (uint8_t)(lv->base_g * (1 - t) + tg * t);
    lv->b = (uint8_t)(lv->base_b * (1 - t) + tb * t);
}

static void tree_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();
    float speed = 0.55f + (speed_knob / 255.0f) * 1.40f;
    s_phase_t += dt * speed;

    // Phase markers in seconds.
    const float T_GROWN  = 5.8f;
    const float T_AUTUMN = 10.0f;
    const float T_FALL   = 14.0f;
    const float T_RESET  = 22.0f;

    if (s_phase_t > T_AUTUMN) {
        float at = (s_phase_t - T_AUTUMN) / (T_FALL - T_AUTUMN);
        if (at > 1) at = 1;
        for (int i = 0; i < s_lv_count; i++) {
            if (s_lv[i].alive) autumn_tint(&s_lv[i], at);
        }
    }

    if (s_phase_t > T_FALL) {
        // Detach leaves probabilistically so they fall staggered, not all at once.
        float into = (s_phase_t - T_FALL) / (T_RESET - T_FALL);
        for (int i = 0; i < s_lv_count; i++) {
            if (!s_lv[i].alive || !s_lv[i].attached) continue;
            if (rngf() < 0.04f * into + 0.003f) {
                s_lv[i].attached = 0;
                s_lv[i].vx = (rngf() - 0.5f) * 0.15f;
                s_lv[i].vy = -0.05f - rngf() * 0.08f;
                s_lv[i].vz = (rngf() - 0.5f) * 0.15f;
            }
        }
    }

    // Integrate falling leaves.
    for (int i = 0; i < s_lv_count; i++) {
        if (!s_lv[i].alive || s_lv[i].attached) continue;
        s_lv[i].vy -= 0.25f * dt;
        s_lv[i].vx += (rngf() - 0.5f) * 0.30f * dt;
        s_lv[i].vz += (rngf() - 0.5f) * 0.30f * dt;
        s_lv[i].vx *= 0.96f;
        s_lv[i].vy *= 0.98f;
        s_lv[i].vz *= 0.96f;
        s_lv[i].x += s_lv[i].vx * dt;
        s_lv[i].y += s_lv[i].vy * dt;
        s_lv[i].z += s_lv[i].vz * dt;
        if (s_lv[i].y < 0.02f) s_lv[i].alive = 0;
    }

    if (s_phase_t > T_RESET) {
        s_phase_t = 0;
        seed_tree();
    }

    const float leaf_r = 0.055f;
    const uint8_t TRUNK_R = 95, TRUNK_G = 58, TRUNK_B = 22;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float px = s_px[f][x][y];
                float py = s_py[f][x][y];
                float pz = s_pz[f][x][y];

                float rr = 0, gg = 0, bb = 0;
                bool hit = false;

                // Branches: animate growth by interpolating the endpoint.
                for (int i = 0; i < s_br_count; i++) {
                    branch_t *b = &s_br[i];
                    float age = s_phase_t - b->start_t;
                    if (age <= 0) continue;
                    float t = age / b->grow_dur;
                    if (t > 1) t = 1;
                    float ex = b->ax + (b->bx - b->ax) * t;
                    float ey = b->ay + (b->by - b->ay) * t;
                    float ez = b->az + (b->bz - b->az) * t;
                    float d = seg_dist(px, py, pz,
                                       b->ax, b->ay, b->az,
                                       ex, ey, ez);
                    if (d >= b->thick) continue;
                    float shade = 1.0f - d / b->thick;
                    float scale = 1.0f - b->depth * 0.12f;
                    rr += TRUNK_R * shade * scale;
                    gg += TRUNK_G * shade * scale;
                    bb += TRUNK_B * shade * scale;
                    hit = true;
                }

                // Leaves: only visible once bloom begins (just before T_GROWN).
                float bloom = (s_phase_t - (T_GROWN - 0.8f)) / 0.8f;
                if (bloom < 0) bloom = 0;
                if (bloom > 1) bloom = 1;
                for (int i = 0; i < s_lv_count; i++) {
                    leaf_t *lv = &s_lv[i];
                    if (!lv->alive) continue;
                    float env = lv->attached ? bloom : 1.0f;
                    float dx = px - lv->x, dy = py - lv->y, dz = pz - lv->z;
                    float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 >= leaf_r * leaf_r) continue;
                    float k = (1.0f - sqrtf(d2) / leaf_r) * env;
                    rr += lv->r * k;
                    gg += lv->g * k;
                    bb += lv->b * k;
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

const effect_vtable_t g_effect_tree = {
    .name = "tree", .id = EFFECT_TREE,
    .enter = tree_enter, .step = tree_step,
};
