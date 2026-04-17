#pragma once
// Cube orientation: which way is physically "up" when the cube sits in its
// stand? Default is face-up (TOP face on top). corner-up treats the
// TOP+WEST+SOUTH vertex as the up pole — the cube rests on the opposite
// corner, making the physical silhouette a diamond.
//
// Effects that care about "up" (matrix flow, and future: fire, rain,
// fireworks, balls) should consult orient_flow() instead of assuming the
// TOP face is up. Face-local effects (rainbow, plasma, per-face galaxy,
// pingpong) don't need to change — they already look correct at any angle.

#include <stdint.h>
#include "cube.h"

typedef enum {
    ORIENT_FACE_UP   = 0,  // TOP face is up (standard orientation)
    ORIENT_CORNER_UP = 1,  // TOP+WEST+SOUTH corner is up (diamond-on-stand)
} orient_mode_t;

void orient_set(orient_mode_t m);
orient_mode_t orient_get(void);

// Flow scalar in [0, 1]: 0 at the "top pole" of the current orientation,
// 1 at the "bottom pole". For face-up this is 1 - y-height; for corner-up
// it's the projection onto the diagonal axis through the chosen corner.
//
// Effects can sample this at each pixel and move streams / gravity in the
// direction of increasing flow. Both pole locations shift automatically
// when orient_set() is called.
float orient_flow(cube_face_t f, int x, int y);

// 3D position of a pixel center on the cube surface, in [0, 1]^3. Cube
// axes: +x = east, +y = up, +z = south. Useful for effects that want to
// project their own axes. Not cached — recompute on demand.
void orient_pixel_pos3d(cube_face_t f, int x, int y,
                        float *px, float *py, float *pz);

// -- BFS "flow" field --------------------------------------------------------
//
// Effects like rain, fire, fireworks, matrix need to know, for every pixel,
// "which way is downstream toward the bottom pole" (or upstream, for heat-
// rising effects). The helpers below build a BFS from a chosen pole of the
// current orientation — face-up uses the TOP or BOTTOM face center; corner-
// up uses the 3 corner pixels clustered at the up- or down-vertex.
//
// The result is a shared global: only one effect is active at a time, and
// each effect rebuilds the field in its enter(). Each cell carries its BFS
// step-count and a list of up to 4 downstream neighbors (cells whose
// distance is self+1) — picking randomly among those at each hop makes
// streams fan out rather than funnel through one path.

typedef struct { int8_t face; int8_t x, y; } orient_cell_t;

#define ORIENT_MAX_NEXT 4

extern int16_t        orient_flow_dist[CUBE_FACE_COUNT][8][8];
extern orient_cell_t  orient_flow_next[CUBE_FACE_COUNT][8][8][ORIENT_MAX_NEXT];
extern uint8_t        orient_flow_next_count[CUBE_FACE_COUNT][8][8];
extern orient_cell_t  orient_flow_seeds[8];
extern int            orient_flow_seed_count;
extern int16_t        orient_flow_max_dist;

// Rebuild the flow from the top pole (cells with flow_t ≈ 0) outward.
// Downstream neighbors thus lead from top pole toward bottom pole.
void orient_build_flow_from_top(void);

// Rebuild the flow from the bottom pole. Downstream neighbors lead from
// bottom toward top — useful for effects where heat/rockets rise.
void orient_build_flow_from_bottom(void);
