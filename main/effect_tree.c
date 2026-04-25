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
// Rendering model: the LED cube is a hollow shell, so a purely
// volumetric tree inside it would never touch the surface. Each face
// instead sees a 2D silhouette projection of the tree — branches and
// leaves are drawn using their in-plane distance on that face, with a
// gentle depth fade so elements farther from the face appear dimmer.
// The result is a consistent 3D tree seen from 6 viewpoints at once.
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
    float radius;       // leaf cluster radius
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

    // Trunk: straight up from the floor with slight jitter. Tall enough
    // that side-face silhouettes read as a tree, not a stump.
    float tip_x = 0.5f + (rngf() - 0.5f) * 0.06f;
    float tip_z = 0.5f + (rngf() - 0.5f) * 0.06f;
    float tip_y = 0.55f + rngf() * 0.08f;
    add_branch(0.5f, 0.02f, 0.5f, tip_x, tip_y, tip_z,
               0.0f, 2.2f, 0.10f, 0);

    // Primary branches: 3 around the trunk tip, spread ~120° apart.
    const int L1_N = 3;
    float base_angle = rngf() * 6.2832f;
    float L1_tip_x[3], L1_tip_y[3], L1_tip_z[3];
    for (int i = 0; i < L1_N; i++) {
        float a = base_angle + (6.2832f / L1_N) * i;
        float len = 0.26f + rngf() * 0.07f;
        L1_tip_x[i] = tip_x + cosf(a) * len * 0.95f;
        L1_tip_z[i] = tip_z + sinf(a) * len * 0.95f;
        L1_tip_y[i] = tip_y + len * 0.70f;
        if (L1_tip_y[i] > 0.92f) L1_tip_y[i] = 0.92f;
        add_branch(tip_x, tip_y, tip_z,
                   L1_tip_x[i], L1_tip_y[i], L1_tip_z[i],
                   2.2f, 1.6f, 0.075f, 1);
    }

    // Twigs: two per primary branch, each ending in a leaf.
    for (int i = 0; i < L1_N; i++) {
        for (int j = 0; j < 2; j++) {
            float a = base_angle + (6.2832f / L1_N) * i + (j ? 0.7f : -0.7f);
            float len = 0.17f + rngf() * 0.07f;
            float tx = L1_tip_x[i] + cosf(a) * len * 1.0f;
            float tz = L1_tip_z[i] + sinf(a) * len * 1.0f;
            float ty = L1_tip_y[i] + len * 0.50f;
            if (ty > 0.94f) ty = 0.94f;
            if (tx < 0.05f) tx = 0.05f;
            if (tx > 0.95f) tx = 0.95f;
            if (tz < 0.05f) tz = 0.05f;
            if (tz > 0.95f) tz = 0.95f;
            add_branch(L1_tip_x[i], L1_tip_y[i], L1_tip_z[i],
                       tx, ty, tz,
                       3.8f, 1.4f, 0.055f, 2);
            if (s_lv_count < MAX_LEAVES) {
                leaf_t *lv = &s_lv[s_lv_count++];
                lv->x = tx; lv->y = ty; lv->z = tz;
                lv->vx = lv->vy = lv->vz = 0;
                lv->alive = 1;
                lv->attached = 1;
                lv->radius = 0.13f + rngf() * 0.04f;
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

// 2D point-to-segment distance in the face plane, plus the fractional
// position t ∈ [0,1] of the closest point along the segment — used to
// sample the correct depth value of the tree element for depth fade.
static float seg_dist2d(float px, float py,
                        float ax, float ay, float bx, float by,
                        float *out_t) {
    float dx = bx - ax, dy = by - ay;
    float l2 = dx * dx + dy * dy;
    float t = 0;
    if (l2 > 1e-6f) {
        t = ((px - ax) * dx + (py - ay) * dy) / l2;
    }
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float cx = ax + dx * t, cy = ay + dy * t;
    float ex = px - cx, ey = py - cy;
    if (out_t) *out_t = t;
    return sqrtf(ex * ex + ey * ey);
}

// Cube-world axis indices (0=x, 1=y, 2=z) spanning each face's plane,
// plus the axis perpendicular to it and the face's coordinate along it.
static void face_basis(cube_face_t f, int *iu, int *iv, int *iw, float *w_at_face) {
    switch (f) {
        case FACE_TOP:    *iu = 0; *iv = 2; *iw = 1; *w_at_face = 1.0f; break;
        case FACE_BOTTOM: *iu = 0; *iv = 2; *iw = 1; *w_at_face = 0.0f; break;
        case FACE_NORTH:  *iu = 0; *iv = 1; *iw = 2; *w_at_face = 0.0f; break;
        case FACE_SOUTH:  *iu = 0; *iv = 1; *iw = 2; *w_at_face = 1.0f; break;
        case FACE_EAST:   *iu = 2; *iv = 1; *iw = 0; *w_at_face = 1.0f; break;
        case FACE_WEST:   *iu = 2; *iv = 1; *iw = 0; *w_at_face = 0.0f; break;
        default:          *iu = 0; *iv = 1; *iw = 2; *w_at_face = 0.5f; break;
    }
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

    const uint8_t TRUNK_R = 140, TRUNK_G = 78, TRUNK_B = 28;

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        int iu, iv, iw;
        float w_face;
        face_basis((cube_face_t)f, &iu, &iv, &iw, &w_face);

        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float pos[3] = {
                    s_px[f][x][y], s_py[f][x][y], s_pz[f][x][y],
                };
                float pu = pos[iu], pv = pos[iv];

                float rr = 0, gg = 0, bb = 0;
                bool hit = false;

                // Branches: project endpoints into the face plane, measure
                // 2D distance, fade with perpendicular depth.
                for (int i = 0; i < s_br_count; i++) {
                    branch_t *b = &s_br[i];
                    float age = s_phase_t - b->start_t;
                    if (age <= 0) continue;
                    float t_grow = age / b->grow_dur;
                    if (t_grow > 1) t_grow = 1;
                    float bend_x = b->ax + (b->bx - b->ax) * t_grow;
                    float bend_y = b->ay + (b->by - b->ay) * t_grow;
                    float bend_z = b->az + (b->bz - b->az) * t_grow;
                    float a3[3] = { b->ax, b->ay, b->az };
                    float e3[3] = { bend_x, bend_y, bend_z };
                    float seg_t;
                    float d = seg_dist2d(pu, pv,
                                         a3[iu], a3[iv],
                                         e3[iu], e3[iv],
                                         &seg_t);
                    if (d >= b->thick) continue;
                    float w_closest = a3[iw] + (e3[iw] - a3[iw]) * seg_t;
                    float depth = fabsf(w_face - w_closest);
                    // Depth fade: nearby silhouette nearly full brightness,
                    // far side of cube ~30% brightness.
                    float depth_k = 1.0f - depth * 0.85f;
                    if (depth_k < 0.25f) depth_k = 0.25f;
                    float shade = 1.0f - d / b->thick;
                    shade = shade * shade * (3.0f - 2.0f * shade); // smoothstep
                    float scale = (1.0f - b->depth * 0.10f) * depth_k;
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
                    float lpos[3] = { lv->x, lv->y, lv->z };
                    float du = pu - lpos[iu], dv = pv - lpos[iv];
                    float d = sqrtf(du * du + dv * dv);
                    if (d >= lv->radius) continue;
                    float depth = fabsf(w_face - lpos[iw]);
                    float depth_k = 1.0f - depth * 0.75f;
                    if (depth_k < 0.30f) depth_k = 0.30f;
                    float k = 1.0f - d / lv->radius;
                    k = k * k * (3.0f - 2.0f * k); // smoothstep
                    k *= env * depth_k;
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
