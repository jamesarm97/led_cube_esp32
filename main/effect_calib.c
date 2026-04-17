#include "effects.h"
#include "render.h"
#include "cube.h"
#include "config.h"

#include <string.h>
#include <stddef.h>

// Three calibration effects:
//
// calib_face: Light exactly one physical panel at a time (bright white) and
//   let the user click which face that is in the web UI. `calib_step` in the
//   config tracks which physical panel is currently lit. The web POST
//   /api/calib/face advances it.
//
// calib_edge: For each face, draw the face's letter (T/B/N/S/E/W) in the
//   middle and paint every edge with the unique "seam color" shared with its
//   neighbor (from cube_edge_seam_color). When assembling, adjacent edges
//   show the same color on both sides, making orientation unambiguous.
//
// face_test: same as calib_edge but without the letter — useful as a final
//   visual check after the user commits calibration.

// --- tiny 6x6 font for T/B/N/S/E/W, rendered onto a 5x5 area centered in 8x8.
// Columns packed MSB=top.  Each glyph is 5 columns of 5 bits (top-aligned).
typedef struct { uint8_t col[5]; } glyph_t; // col is 5 bits in bits 4..0

static const glyph_t FONT_T = { { 0x1F, 0x04, 0x04, 0x04, 0x04 } };
static const glyph_t FONT_B = { { 0x1F, 0x15, 0x15, 0x15, 0x0A } };
static const glyph_t FONT_N = { { 0x1F, 0x08, 0x04, 0x02, 0x1F } };
static const glyph_t FONT_S = { { 0x12, 0x15, 0x15, 0x15, 0x09 } };
static const glyph_t FONT_E = { { 0x1F, 0x15, 0x15, 0x15, 0x11 } };
static const glyph_t FONT_W = { { 0x1F, 0x08, 0x04, 0x08, 0x1F } };

static const glyph_t *glyph_for(cube_face_t f) {
    switch (f) {
        case FACE_TOP:    return &FONT_T;
        case FACE_BOTTOM: return &FONT_B;
        case FACE_NORTH:  return &FONT_N;
        case FACE_SOUTH:  return &FONT_S;
        case FACE_EAST:   return &FONT_E;
        case FACE_WEST:   return &FONT_W;
        default:          return NULL;
    }
}

static void draw_letter(cube_face_t f, uint8_t r, uint8_t g, uint8_t b) {
    const glyph_t *gl = glyph_for(f);
    if (!gl) return;
    // 5x5 glyph centered in 8x8 -> origin (x=1, y=1)..(x=5, y=5).
    //
    // EAST and WEST have their face-local +x axis pointing toward the
    // viewer's LEFT when looking at the face from outside the cube (because
    // those faces' "x" axis runs south and north respectively). Writing the
    // glyph in face-local order would produce a mirrored letter. Flip the
    // column index for those two faces so the letter reads correctly.
    bool flip_x = (f == FACE_EAST || f == FACE_WEST);
    int ox = 1, oy = 1;
    for (int cx = 0; cx < 5; cx++) {
        uint8_t col = gl->col[cx];
        int dx = flip_x ? (4 - cx) : cx;
        for (int cy = 0; cy < 5; cy++) {
            if (col & (1 << (4 - cy))) {
                render_set(f, ox + dx, oy + cy, r, g, b);
            }
        }
    }
}

static void draw_edges_seam(cube_face_t f) {
    uint8_t r, g, b;
    // top row
    cube_edge_seam_color(f, EDGE_TOP, &r, &g, &b);
    for (int x = 0; x < 8; x++) render_set(f, x, 0, r, g, b);
    // bottom row
    cube_edge_seam_color(f, EDGE_BOTTOM, &r, &g, &b);
    for (int x = 0; x < 8; x++) render_set(f, x, 7, r, g, b);
    // left column
    cube_edge_seam_color(f, EDGE_LEFT, &r, &g, &b);
    for (int y = 0; y < 8; y++) render_set(f, 0, y, r, g, b);
    // right column
    cube_edge_seam_color(f, EDGE_RIGHT, &r, &g, &b);
    for (int y = 0; y < 8; y++) render_set(f, 7, y, r, g, b);
}

// --- calib_face ------------------------------------------------------------

static void calib_face_enter(void) {
    // Nothing persistent; render_clear has already fired this frame.
}

static void calib_face_step(float dt) {
    (void)dt;
    config_lock();
    int step = config_get()->calib_step;
    config_unlock();
    if (step < 0 || step >= CUBE_FACE_COUNT) step = 0;
    // Light ONE physical panel bright white so the user can identify it.
    render_fill_physical_panel(step, 255, 255, 255);
}

// --- calib_edge (with letter) ---------------------------------------------

static void calib_edge_step(float dt) {
    (void)dt;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        draw_edges_seam((cube_face_t)f);
        // Letter in a neutral warm tone so it reads on any edge color.
        draw_letter((cube_face_t)f, 180, 180, 180);
    }
}

// --- face_test (no letter, for final QA after calibrating) ----------------

static void face_test_step(float dt) {
    (void)dt;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        draw_edges_seam((cube_face_t)f);
    }
}

const effect_vtable_t g_effect_calib_face = {
    .name = "calib_face", .id = EFFECT_CALIB_FACE,
    .enter = calib_face_enter, .step = calib_face_step,
};

const effect_vtable_t g_effect_calib_edge = {
    .name = "calib_edge", .id = EFFECT_CALIB_EDGE,
    .enter = NULL, .step = calib_edge_step,
};

const effect_vtable_t g_effect_face_test = {
    .name = "face_test", .id = EFFECT_FACE_TEST,
    .enter = NULL, .step = face_test_step,
};
