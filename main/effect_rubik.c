#include "effects.h"
#include "render.h"
#include "cube.h"
#include "esp_random.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Self-playing Rubik's cube. 6 faces × 3×3 stickers; every beat fires one
// of three moves — U (top layer), D (bottom layer), or E (equator middle
// slice). U/D rotate their whole 3×3 face in-place while cycling the
// adjacent row around the 4 side faces; E cycles just the middle row.
//
// The move animates for real. Ring faces: the affected pixel row slides
// sideways by one full face-width (8 px) over the animation window,
// adjacent faces dragging along to form a continuous 32-pixel belt.
// Rotating face: its 3×3 pattern rotates smoothly through 90° in place,
// sampled per-pixel each frame. At end-of-animation the sticker state
// snaps to match whatever is on screen, so the cut-over is invisible.
//
// Adjacent faces on the cube have mirrored face-x axes, so the belt
// table below encodes each face's direction (step ±1) from its
// belt_pos = 0 anchor. That means col indices flip correctly across
// every seam — stickers trace a geometrically consistent ring around
// the cube rather than recoloring in place.

static const uint8_t COLORS[6][3] = {
    [FACE_TOP]    = { 230, 230, 230 },
    [FACE_BOTTOM] = { 230, 210,   0 },
    [FACE_NORTH]  = {   0,  80, 220 },
    [FACE_SOUTH]  = {  40, 190,  80 },
    [FACE_EAST]   = { 220,  30,  15 },
    [FACE_WEST]   = { 240, 120,  10 },
};

// s_state[face][row][col]: color index (== face id when solved).
static uint8_t s_state[6][3][3];

typedef enum { MOVE_U = 0, MOVE_D, MOVE_E, MOVE_COUNT } move_t;

// One entry describes a ring face's slice of the 32-position pixel belt.
// belt_pos = start_bp + step * face_x, where step is ±1.
typedef struct {
    uint8_t face;
    uint8_t row;      // 0, 1, or 2 — sticker row on this face
    int8_t  start_bp; // belt position at face_x = 0
    int8_t  step;     // +1 or -1
} belt_entry_t;

typedef struct {
    belt_entry_t ring[4];
    int8_t       rot_face;  // -1 if no in-place face rotation (E move)
} move_desc_t;

// U: top slab, non-reverse = CCW from above; ring order around cube
//    E → N → W → S.
// D: bottom slab, non-reverse = CW from above (CCW from below viewer);
//    ring order N → E → S → W.
// E: middle slab, non-reverse = same direction as U (E → N → W → S).
static const move_desc_t MOVES[MOVE_COUNT] = {
    [MOVE_U] = {
        .ring = {
            { FACE_EAST,  0,  7, -1 },
            { FACE_NORTH, 0,  8, +1 },
            { FACE_WEST,  0, 23, -1 },
            { FACE_SOUTH, 0, 24, +1 },
        },
        .rot_face = FACE_TOP,
    },
    [MOVE_D] = {
        .ring = {
            { FACE_NORTH, 2,  7, -1 },
            { FACE_EAST,  2,  8, +1 },
            { FACE_SOUTH, 2, 23, -1 },
            { FACE_WEST,  2, 24, +1 },
        },
        .rot_face = FACE_BOTTOM,
    },
    [MOVE_E] = {
        .ring = {
            { FACE_EAST,  1,  7, -1 },
            { FACE_NORTH, 1,  8, +1 },
            { FACE_WEST,  1, 23, -1 },
            { FACE_SOUTH, 1, 24, +1 },
        },
        .rot_face = -1,
    },
};

static float  s_clock;
static float  s_rest_len = 0.65f;
static float  s_anim_len = 0.80f;
static move_t s_move;
static int    s_dir;           // +1 = non-reverse, -1 = reverse
static bool   s_move_applied;

// 8 pixels partitioned 2-gutter-2-gutter-2 into 3 stickers.
static inline int pixel_to_sticker(int v) {
    static const int8_t MAP[8] = { 0, 0, -1, 1, 1, -1, 2, 2 };
    return MAP[v];
}

// Inverse belt lookup: given bp ∈ [0, 32), find the entry and face_x.
static const belt_entry_t *belt_lookup(const move_desc_t *m,
                                       int bp, int *out_x) {
    for (int i = 0; i < 4; i++) {
        const belt_entry_t *e = &m->ring[i];
        int x = (bp - e->start_bp) * e->step;
        if (x >= 0 && x < 8) { *out_x = x; return e; }
    }
    return NULL;
}

// Sticker color at a pixel-level belt position. Gutters → black.
static void belt_color(const move_desc_t *m, int bp, int row,
                       uint8_t *rr, uint8_t *gg, uint8_t *bb) {
    bp = ((bp % 32) + 32) % 32;
    int x;
    const belt_entry_t *e = belt_lookup(m, bp, &x);
    if (!e) { *rr = *gg = *bb = 0; return; }
    int sc = pixel_to_sticker(x);
    if (sc < 0) { *rr = *gg = *bb = 0; return; }
    uint8_t ci = s_state[e->face][row][sc];
    *rr = COLORS[ci][0]; *gg = COLORS[ci][1]; *bb = COLORS[ci][2];
}

// Nearest-neighbor sample of rotating face at fractional (sx, sy).
static void face_sample(int face, float sx, float sy,
                        uint8_t *rr, uint8_t *gg, uint8_t *bb) {
    int ix = (int)floorf(sx + 0.5f);
    int iy = (int)floorf(sy + 0.5f);
    if (ix < 0 || ix > 7 || iy < 0 || iy > 7) {
        *rr = *gg = *bb = 0; return;
    }
    int sc = pixel_to_sticker(ix);
    int sr = pixel_to_sticker(iy);
    if (sc < 0 || sr < 0) { *rr = *gg = *bb = 0; return; }
    uint8_t ci = s_state[face][sr][sc];
    *rr = COLORS[ci][0]; *gg = COLORS[ci][1]; *bb = COLORS[ci][2];
}

// Commit a move to s_state: cycle the belt by ±8 pixels (= 3 stickers)
// and rotate the face 90°.
static void apply_move(const move_desc_t *m, int dir) {
    // Snapshot sticker colors along the 32-position belt.
    uint8_t  snap_ci[32];
    bool     snap_is_sticker[32] = { 0 };
    for (int i = 0; i < 4; i++) {
        const belt_entry_t *e = &m->ring[i];
        for (int x = 0; x < 8; x++) {
            int bp = e->start_bp + e->step * x;
            int sc = pixel_to_sticker(x);
            snap_is_sticker[bp] = (sc >= 0);
            if (sc >= 0) snap_ci[bp] = s_state[e->face][e->row][sc];
        }
    }
    // Write back shifted. Sticker pixels of a face sit at the same
    // mod-8 offsets as on the source face, so bp_src is also a sticker.
    for (int i = 0; i < 4; i++) {
        const belt_entry_t *e = &m->ring[i];
        for (int x = 0; x < 8; x++) {
            int sc = pixel_to_sticker(x);
            if (sc < 0) continue;
            int bp = e->start_bp + e->step * x;
            int bp_src = ((bp - dir * 8) % 32 + 32) % 32;
            if (!snap_is_sticker[bp_src]) continue;
            s_state[e->face][e->row][sc] = snap_ci[bp_src];
        }
    }
    // Rotate the in-place face 90°. dir>0 = CCW (as seen from outside
    // the face), matching the belt direction.
    if (m->rot_face >= 0) {
        uint8_t t[3][3];
        memcpy(t, s_state[m->rot_face], sizeof(t));
        if (dir > 0) {
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    s_state[m->rot_face][r][c] = t[c][2 - r];
        } else {
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    s_state[m->rot_face][r][c] = t[2 - c][r];
        }
    }
}

static void draw_face_rest(int f) {
    for (int y = 0; y < 8; y++) {
        int sr = pixel_to_sticker(y);
        if (sr < 0) continue;
        for (int x = 0; x < 8; x++) {
            int sc = pixel_to_sticker(x);
            if (sc < 0) continue;
            uint8_t ci = s_state[f][sr][sc];
            render_set((cube_face_t)f, x, y,
                       COLORS[ci][0], COLORS[ci][1], COLORS[ci][2]);
        }
    }
}

static void rubik_enter(void) {
    for (int f = 0; f < 6; f++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                s_state[f][r][c] = (uint8_t)f;
    s_clock = 0;
    s_move = MOVE_U;
    s_dir = +1;
    s_move_applied = true;
}

static void rubik_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();
    float speed = 0.55f + (speed_knob / 255.0f) * 1.6f;
    s_clock += dt * speed;

    float phase_len = s_rest_len + s_anim_len;
    if (s_clock >= phase_len) {
        s_clock -= phase_len;
        s_move = (move_t)(esp_random() % MOVE_COUNT);
        s_dir  = (esp_random() & 1) ? +1 : -1;
        s_move_applied = false;
    }

    bool in_anim = s_clock >= s_rest_len;
    float anim_t = 0.0f;
    if (in_anim) {
        anim_t = (s_clock - s_rest_len) / s_anim_len;
        if (anim_t > 1.0f) anim_t = 1.0f;
    }

    // Baseline: every face drawn from current s_state. Animation below
    // overwrites only the affected pixels on ring + rotating faces.
    for (int f = 0; f < 6; f++) draw_face_rest(f);

    if (in_anim && !s_move_applied) {
        const move_desc_t *m = &MOVES[s_move];

        // Ring belt slide. Sticker now at bp_dst came from bp_src =
        // bp_dst − dir*anim_t*8. Bilinear-in-bp sampling keeps the slide
        // smooth across the ~48 animation frames.
        float shift = (float)s_dir * anim_t * 8.0f;
        for (int i = 0; i < 4; i++) {
            const belt_entry_t *e = &m->ring[i];
            int y0 = e->row * 3;   // pixel rows 0-1, 3-4, or 6-7
            for (int y = y0; y < y0 + 2; y++) {
                for (int x = 0; x < 8; x++) {
                    int bp_dst = e->start_bp + e->step * x;
                    float bp_src_f = (float)bp_dst - shift;
                    while (bp_src_f <  0)    bp_src_f += 32.0f;
                    while (bp_src_f >= 32.f) bp_src_f -= 32.0f;
                    int bp_lo = (int)floorf(bp_src_f);
                    int bp_hi = (bp_lo + 1) % 32;
                    float frac = bp_src_f - (float)bp_lo;
                    uint8_t r0, g0, b0, r1, g1, b1;
                    belt_color(m, bp_lo, e->row, &r0, &g0, &b0);
                    belt_color(m, bp_hi, e->row, &r1, &g1, &b1);
                    uint8_t rr = (uint8_t)((1.0f - frac) * r0 + frac * r1);
                    uint8_t gg = (uint8_t)((1.0f - frac) * g0 + frac * g1);
                    uint8_t bb = (uint8_t)((1.0f - frac) * b0 + frac * b1);
                    render_set((cube_face_t)e->face, x, y, rr, gg, bb);
                }
            }
        }

        // In-place face rotation. Inverse-rotate each pixel by −θ back
        // to its pre-move source on the same face, then nearest-sample.
        if (m->rot_face >= 0) {
            float theta = (float)s_dir * anim_t * 1.5707963f;
            float ca = cosf(theta), sa = sinf(theta);
            int rf = m->rot_face;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    float u = (float)x - 3.5f, v = (float)y - 3.5f;
                    float us =  u * ca + v * sa;
                    float vs = -u * sa + v * ca;
                    uint8_t rr, gg, bb;
                    face_sample(rf, us + 3.5f, vs + 3.5f, &rr, &gg, &bb);
                    render_set((cube_face_t)rf, x, y, rr, gg, bb);
                }
            }
        }

        if (anim_t >= 1.0f) {
            apply_move(m, s_dir);
            s_move_applied = true;
        }
    }
}

const effect_vtable_t g_effect_rubik = {
    .name = "rubik", .id = EFFECT_RUBIK,
    .enter = rubik_enter, .step = rubik_step,
};
