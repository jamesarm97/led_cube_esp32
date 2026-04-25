#include "effects.h"
#include "render.h"
#include "cube.h"
#include "esp_random.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Self-playing Rubik's cube. 6 faces × 3×3 stickers; every beat fires one
// of six moves:
//   U/D — top/bottom horizontal layer.
//   E   — equator (middle horizontal slice, no face rotation).
//   L/R — west/east vertical column.
//   M   — middle vertical slice between L and R, no face rotation.
//
// Each U/D/L/R rotates a 3×3 face in-place while cycling the adjacent
// 3-sticker strip around the 4 ring faces. E and M just cycle a middle
// strip with no in-place face rotation.
//
// The move animates for real. Ring faces: the affected pixel strip slides
// by one full face-width (8 px) over the animation window, adjacent faces
// dragging along to form a continuous 32-pixel belt. Rotating face: its
// 3×3 pattern rotates smoothly through 90° in place, sampled per-pixel
// each frame. At end-of-animation the sticker state snaps to match
// whatever is on screen, so the cut-over is invisible.
//
// Adjacent faces on the cube have mirrored axes, so the belt table below
// encodes each ring face's direction (step ±1) along its varying axis,
// and an axis flag (0 = horizontal, vary x; 1 = vertical, vary y).

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

typedef enum {
    MOVE_U = 0, MOVE_D, MOVE_E,
    MOVE_L,     MOVE_R, MOVE_M,
    MOVE_COUNT
} move_t;

// One entry describes a ring face's slice of the 32-position pixel belt.
// belt_pos = start_bp + step * v, where v is the varying-axis pixel index
// (face_x for axis=0, face_y for axis=1). `fixed` is the perpendicular
// sticker index (sticker_row for axis=0, sticker_col for axis=1).
typedef struct {
    uint8_t face;
    uint8_t axis;     // 0 = horizontal (vary x), 1 = vertical (vary y)
    uint8_t fixed;    // 0, 1, or 2 — sticker row (axis=0) or sticker col (axis=1)
    int8_t  start_bp;
    int8_t  step;
} belt_entry_t;

typedef struct {
    belt_entry_t ring[4];
    int8_t       rot_face;  // -1 if no in-place face rotation (E, M)
} move_desc_t;

// Direction conventions (dir = +1 unless noted):
//   U: top slab, ring E → N → W → S (CCW from above).
//   D: bottom slab, ring N → E → S → W.
//   E: equator, ring same as U.
//   L: west slab, ring TOP → SOUTH → BOTTOM → NORTH (CW from west viewer).
//   R: east slab, ring TOP → NORTH → BOTTOM → SOUTH (CW from east viewer).
//   M: middle vertical slab, ring same as L.
static const move_desc_t MOVES[MOVE_COUNT] = {
    [MOVE_U] = {
        .ring = {
            { .face=FACE_EAST,  .axis=0, .fixed=0, .start_bp= 7, .step=-1 },
            { .face=FACE_NORTH, .axis=0, .fixed=0, .start_bp= 8, .step=+1 },
            { .face=FACE_WEST,  .axis=0, .fixed=0, .start_bp=23, .step=-1 },
            { .face=FACE_SOUTH, .axis=0, .fixed=0, .start_bp=24, .step=+1 },
        },
        .rot_face = FACE_TOP,
    },
    [MOVE_D] = {
        .ring = {
            { .face=FACE_NORTH, .axis=0, .fixed=2, .start_bp= 7, .step=-1 },
            { .face=FACE_EAST,  .axis=0, .fixed=2, .start_bp= 8, .step=+1 },
            { .face=FACE_SOUTH, .axis=0, .fixed=2, .start_bp=23, .step=-1 },
            { .face=FACE_WEST,  .axis=0, .fixed=2, .start_bp=24, .step=+1 },
        },
        .rot_face = FACE_BOTTOM,
    },
    [MOVE_E] = {
        .ring = {
            { .face=FACE_EAST,  .axis=0, .fixed=1, .start_bp= 7, .step=-1 },
            { .face=FACE_NORTH, .axis=0, .fixed=1, .start_bp= 8, .step=+1 },
            { .face=FACE_WEST,  .axis=0, .fixed=1, .start_bp=23, .step=-1 },
            { .face=FACE_SOUTH, .axis=0, .fixed=1, .start_bp=24, .step=+1 },
        },
        .rot_face = -1,
    },
    [MOVE_L] = {
        .ring = {
            { .face=FACE_TOP,    .axis=1, .fixed=0, .start_bp= 0, .step=+1 },
            { .face=FACE_SOUTH,  .axis=1, .fixed=0, .start_bp= 8, .step=+1 },
            { .face=FACE_BOTTOM, .axis=1, .fixed=0, .start_bp=16, .step=+1 },
            { .face=FACE_NORTH,  .axis=1, .fixed=2, .start_bp=31, .step=-1 },
        },
        .rot_face = FACE_WEST,
    },
    [MOVE_R] = {
        .ring = {
            { .face=FACE_TOP,    .axis=1, .fixed=2, .start_bp= 7, .step=-1 },
            { .face=FACE_NORTH,  .axis=1, .fixed=0, .start_bp= 8, .step=+1 },
            { .face=FACE_BOTTOM, .axis=1, .fixed=2, .start_bp=23, .step=-1 },
            { .face=FACE_SOUTH,  .axis=1, .fixed=2, .start_bp=31, .step=-1 },
        },
        .rot_face = FACE_EAST,
    },
    [MOVE_M] = {
        .ring = {
            { .face=FACE_TOP,    .axis=1, .fixed=1, .start_bp= 0, .step=+1 },
            { .face=FACE_SOUTH,  .axis=1, .fixed=1, .start_bp= 8, .step=+1 },
            { .face=FACE_BOTTOM, .axis=1, .fixed=1, .start_bp=16, .step=+1 },
            { .face=FACE_NORTH,  .axis=1, .fixed=1, .start_bp=31, .step=-1 },
        },
        .rot_face = -1,
    },
};

static float  s_clock;
static float  s_rest_len = 0.55f;
static float  s_anim_len = 0.70f;
static move_t s_move;
static int    s_dir;           // +1 = non-reverse, -1 = reverse
static bool   s_move_applied;

// Scramble → solve → celebrate → scramble cycle. After ~30 seconds of
// random moves, the recorded history is replayed in reverse to actually
// solve the cube. Once solved, the whole cube pulses dim → bright a few
// times then holds bright before reseeding.
typedef enum {
    PHASE_SCRAMBLE = 0,
    PHASE_SOLVE,
    PHASE_CELEBRATE,
} rubik_phase_t;

typedef struct {
    uint8_t move;
    int8_t  dir;
} move_rec_t;

#define HIST_MAX 96

static move_rec_t   s_history[HIST_MAX];
static int          s_history_count;
static rubik_phase_t s_phase;
static float        s_phase_time;      // seconds elapsed in current phase
static float        s_bright_mult = 1.0f;  // effect-local brightness scale
static const float  SCRAMBLE_DURATION    = 30.0f;
static const float  CELEB_PULSE_PERIOD   = 0.5f;
static const int    CELEB_PULSE_COUNT    = 3;
static const float  CELEB_PULSE_DUR      = CELEB_PULSE_PERIOD * CELEB_PULSE_COUNT;
static const float  CELEB_STAY_BRIGHT    = 1.5f;
static const float  CELEBRATE_DURATION   = CELEB_PULSE_DUR + CELEB_STAY_BRIGHT;

// 8 pixels partitioned 2-gutter-2-gutter-2 into 3 stickers.
static inline int pixel_to_sticker(int v) {
    static const int8_t MAP[8] = { 0, 0, -1, 1, 1, -1, 2, 2 };
    return MAP[v];
}

// render_set wrapper that applies the effect-local brightness multiplier.
// Used during the celebrate phase to pulse the whole cube; transparent
// (mult=1) the rest of the time.
static inline void set_pixel(cube_face_t face, int x, int y,
                             uint8_t r, uint8_t g, uint8_t b) {
    if (s_bright_mult >= 0.999f) {
        render_set(face, x, y, r, g, b);
        return;
    }
    render_set(face, x, y,
               (uint8_t)((float)r * s_bright_mult),
               (uint8_t)((float)g * s_bright_mult),
               (uint8_t)((float)b * s_bright_mult));
}

// (sr, sc) lookup that hides axis branching from callers.
static inline void entry_sticker_indices(const belt_entry_t *e, int sc_var,
                                         int *sr, int *sc) {
    if (e->axis == 0) { *sr = e->fixed; *sc = sc_var;   }
    else              { *sr = sc_var;   *sc = e->fixed; }
}

// Inverse belt lookup: given bp ∈ [0, 32), find the entry and var index.
static const belt_entry_t *belt_lookup(const move_desc_t *m,
                                       int bp, int *out_v) {
    for (int i = 0; i < 4; i++) {
        const belt_entry_t *e = &m->ring[i];
        int v = (bp - e->start_bp) * e->step;
        if (v >= 0 && v < 8) { *out_v = v; return e; }
    }
    return NULL;
}

// Sticker color at a pixel-level belt position. Gutters → black.
static void belt_color(const move_desc_t *m, int bp,
                       uint8_t *rr, uint8_t *gg, uint8_t *bb) {
    bp = ((bp % 32) + 32) % 32;
    int v;
    const belt_entry_t *e = belt_lookup(m, bp, &v);
    if (!e) { *rr = *gg = *bb = 0; return; }
    int sc_var = pixel_to_sticker(v);
    if (sc_var < 0) { *rr = *gg = *bb = 0; return; }
    int sr, sc;
    entry_sticker_indices(e, sc_var, &sr, &sc);
    uint8_t ci = s_state[e->face][sr][sc];
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
        for (int v = 0; v < 8; v++) {
            int bp = e->start_bp + e->step * v;
            int sc_var = pixel_to_sticker(v);
            snap_is_sticker[bp] = (sc_var >= 0);
            if (sc_var < 0) continue;
            int sr, sc;
            entry_sticker_indices(e, sc_var, &sr, &sc);
            snap_ci[bp] = s_state[e->face][sr][sc];
        }
    }
    // Write back shifted. Sticker pixels of a face sit at the same
    // mod-8 offsets as on the source face, so bp_src is also a sticker.
    for (int i = 0; i < 4; i++) {
        const belt_entry_t *e = &m->ring[i];
        for (int v = 0; v < 8; v++) {
            int sc_var = pixel_to_sticker(v);
            if (sc_var < 0) continue;
            int bp = e->start_bp + e->step * v;
            int bp_src = ((bp - dir * 8) % 32 + 32) % 32;
            if (!snap_is_sticker[bp_src]) continue;
            int sr, sc;
            entry_sticker_indices(e, sc_var, &sr, &sc);
            s_state[e->face][sr][sc] = snap_ci[bp_src];
        }
    }
    // Rotate the in-place face 90°. dir>0 = CCW in face-local coords,
    // matching the belt direction.
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
            set_pixel((cube_face_t)f, x, y,
                      COLORS[ci][0], COLORS[ci][1], COLORS[ci][2]);
        }
    }
}

static void rubik_reset_to_solved(void) {
    for (int f = 0; f < 6; f++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                s_state[f][r][c] = (uint8_t)f;
    s_history_count = 0;
}

static void rubik_enter(void) {
    rubik_reset_to_solved();
    s_clock = 0;
    s_move = MOVE_U;
    s_dir = +1;
    s_move_applied = true;
    s_phase = PHASE_SCRAMBLE;
    s_phase_time = 0;
}

// Pick a random scramble move, avoiding an immediate no-op inverse of the
// last move (which would simply cancel it out visually).
static void pick_scramble_move(void) {
    move_t m = (move_t)(esp_random() % MOVE_COUNT);
    int d = (esp_random() & 1) ? +1 : -1;
    if (s_history_count > 0) {
        move_rec_t *last = &s_history[s_history_count - 1];
        if (last->move == m && last->dir != d) d = last->dir;
    }
    s_move = m;
    s_dir  = d;
    if (s_history_count < HIST_MAX) {
        s_history[s_history_count++] = (move_rec_t){(uint8_t)m, (int8_t)d};
    }
}

static void rubik_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed_knob = config_get()->rainbow_speed;
    config_unlock();
    float speed = 0.55f + (speed_knob / 255.0f) * 1.6f;
    s_clock += dt * speed;
    s_phase_time += dt * speed;

    float phase_len = s_rest_len + s_anim_len;
    if (s_clock >= phase_len) {
        s_clock -= phase_len;

        // Commit the move whose animation just finished. anim_t can never
        // reach 1.0 in the body below because the wrap above pre-empts it,
        // so this is the only place s_state actually advances.
        if (!s_move_applied) {
            apply_move(&MOVES[s_move], s_dir);
            s_move_applied = true;
        }

        // End-of-phase transitions.
        if (s_phase == PHASE_SCRAMBLE &&
            s_phase_time >= SCRAMBLE_DURATION &&
            s_history_count > 0) {
            s_phase = PHASE_SOLVE;
            s_phase_time = 0;
        } else if (s_phase == PHASE_CELEBRATE &&
                   s_phase_time >= CELEBRATE_DURATION) {
            s_phase = PHASE_SCRAMBLE;
            s_phase_time = 0;
            rubik_reset_to_solved();   // fresh solved state, empty history
        }

        // Pick the next move (or enter celebration if solve just finished).
        if (s_phase == PHASE_SCRAMBLE) {
            pick_scramble_move();
            s_move_applied = false;
        } else if (s_phase == PHASE_SOLVE) {
            if (s_history_count > 0) {
                s_history_count--;
                s_move = (move_t)s_history[s_history_count].move;
                s_dir  = -(int)s_history[s_history_count].dir;
                s_move_applied = false;
            } else {
                // All moves reversed — cube is solved. Pulse-celebrate
                // before the next scramble.
                s_phase = PHASE_CELEBRATE;
                s_phase_time = 0;
                s_move_applied = true;
            }
        } else {
            // PHASE_CELEBRATE: no move animation, just show solved state.
            s_move_applied = true;
        }
    }

    // Effect-local brightness multiplier. During the celebrate phase the
    // first CELEB_PULSE_DUR seconds drive a cosine that sweeps between
    // 0.15 and 1.0 with period CELEB_PULSE_PERIOD — starts bright, dims,
    // brightens, repeats CELEB_PULSE_COUNT times, then locks at 1.0.
    if (s_phase == PHASE_CELEBRATE && s_phase_time < CELEB_PULSE_DUR) {
        float c = cosf(6.2831853f * s_phase_time / CELEB_PULSE_PERIOD);
        s_bright_mult = 0.575f + 0.425f * c;
    } else {
        s_bright_mult = 1.0f;
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
        // smooth across the ~48 animation frames. The strip is 2 pixels
        // wide perpendicular to the belt direction; both pixels show the
        // same sticker color at any given bp.
        float shift = (float)s_dir * anim_t * 8.0f;
        for (int i = 0; i < 4; i++) {
            const belt_entry_t *e = &m->ring[i];
            int p0 = e->fixed * 3;   // perpendicular pixel range (2 pixels wide)
            for (int u = p0; u < p0 + 2; u++) {
                for (int v = 0; v < 8; v++) {
                    int bp_dst = e->start_bp + e->step * v;
                    float bp_src_f = (float)bp_dst - shift;
                    while (bp_src_f <  0)    bp_src_f += 32.0f;
                    while (bp_src_f >= 32.f) bp_src_f -= 32.0f;
                    int bp_lo = (int)floorf(bp_src_f);
                    int bp_hi = (bp_lo + 1) % 32;
                    float frac = bp_src_f - (float)bp_lo;
                    uint8_t r0, g0, b0, r1, g1, b1;
                    belt_color(m, bp_lo, &r0, &g0, &b0);
                    belt_color(m, bp_hi, &r1, &g1, &b1);
                    uint8_t rr = (uint8_t)((1.0f - frac) * r0 + frac * r1);
                    uint8_t gg = (uint8_t)((1.0f - frac) * g0 + frac * g1);
                    uint8_t bb = (uint8_t)((1.0f - frac) * b0 + frac * b1);
                    int px = (e->axis == 0) ? v : u;
                    int py = (e->axis == 0) ? u : v;
                    set_pixel((cube_face_t)e->face, px, py, rr, gg, bb);
                }
            }
        }

        // In-place face rotation. Inverse-rotate each pixel by −θ back
        // to its pre-move source on the same face, then nearest-sample.
        // Negative sign aligns the visual rotation with apply_move's
        // CCW-for-dir>0 convention — otherwise the commit step rotated
        // the opposite way from the animation and the cube appeared to
        // snap back at the end of each move.
        if (m->rot_face >= 0) {
            float theta = -(float)s_dir * anim_t * 1.5707963f;
            float ca = cosf(theta), sa = sinf(theta);
            int rf = m->rot_face;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    float u = (float)x - 3.5f, v = (float)y - 3.5f;
                    float us =  u * ca + v * sa;
                    float vs = -u * sa + v * ca;
                    uint8_t rr, gg, bb;
                    face_sample(rf, us + 3.5f, vs + 3.5f, &rr, &gg, &bb);
                    set_pixel((cube_face_t)rf, x, y, rr, gg, bb);
                }
            }
        }
    }
}

const effect_vtable_t g_effect_rubik = {
    .name = "rubik", .id = EFFECT_RUBIK,
    .enter = rubik_enter, .step = rubik_step,
};
