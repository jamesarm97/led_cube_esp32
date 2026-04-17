#pragma once
// Cube coordinate abstraction.
//
// Logical model:
//   - 6 faces: TOP, BOTTOM, NORTH, SOUTH, EAST, WEST.
//   - Each face has an 8x8 grid in cube-world coordinates (x right, y down when
//     viewed from outside the cube facing that face, with "up" defined per-face
//     below). (0,0) is the face's upper-left when looking at it from outside.
//   - Each face's "up" edge is the edge adjacent to the face directly above it
//     in cube-world. See face_adjacency[] for the canonical topology.
//
// Physical model:
//   - 6 WS2812 panels, each 8x8 = 64 LEDs, daisy-chained into one strip of 384.
//   - Panels are serpentine-wired by default (row 0 L->R, row 1 R->L, ...).
//   - panel_map[physical_index 0..5] = face label: which face is each physical
//     panel in the chain?
//   - panel_rot[physical_index 0..5] = 0/90/180/270: how is the panel rotated
//     relative to cube-world for its face?
//
// Effects write to (face, x, y) in cube-world. The cube module translates to a
// strip index using panel_map + panel_rot.

#include <stdint.h>
#include <stdbool.h>

#define CUBE_PANEL_W        8
#define CUBE_PANEL_H        8
#define CUBE_PANEL_PIXELS   (CUBE_PANEL_W * CUBE_PANEL_H)
#define CUBE_FACE_COUNT     6
#define CUBE_TOTAL_PIXELS   (CUBE_FACE_COUNT * CUBE_PANEL_PIXELS)

typedef enum {
    FACE_TOP    = 0,
    FACE_BOTTOM = 1,
    FACE_NORTH  = 2,
    FACE_SOUTH  = 3,
    FACE_EAST   = 4,
    FACE_WEST   = 5,
    FACE_UNKNOWN = 0xFF,
} cube_face_t;

typedef enum {
    EDGE_TOP    = 0,  // y = 0
    EDGE_BOTTOM = 1,  // y = 7
    EDGE_LEFT   = 2,  // x = 0
    EDGE_RIGHT  = 3,  // x = 7
} cube_edge_t;

// Adjacency entry: face.edge meets neighbor_face.neighbor_edge. flip = true
// means the coordinate along the edge is reversed when crossing.
typedef struct {
    cube_face_t neighbor_face;
    cube_edge_t neighbor_edge;
    bool flip;
} cube_adj_t;

// Current calibration — set by config module, read by cube module.
typedef struct {
    cube_face_t panel_map[CUBE_FACE_COUNT];    // physical -> face
    uint8_t     panel_rot[CUBE_FACE_COUNT];    // 0/1/2/3 => 0/90/180/270
    uint8_t     panel_mirror[CUBE_FACE_COUNT]; // 0 or 1; 1 = horizontal flip applied BEFORE rotation
    bool        serpentine;                    // 8x8 panel serpentine wiring
} cube_calib_t;

// Initialize / update calibration in the cube module. Safe to call at runtime.
void cube_set_calibration(const cube_calib_t *calib);
void cube_get_calibration(cube_calib_t *out);

// Translate logical (face, x, y) -> physical strip index [0..CUBE_TOTAL_PIXELS).
// Returns -1 if coordinates are out of range or face isn't mapped to a panel.
int cube_logical_to_strip(cube_face_t face, int x, int y);

// Adjacency: what's on the other side of this (face, edge)?
const cube_adj_t *cube_adjacent(cube_face_t face, cube_edge_t edge);

// Given a face and an out-of-range (x,y) that has just stepped off one edge,
// return the wrapped position on the neighbor face. Returns false if still in
// range (caller can just use it directly).
bool cube_step_over_edge(cube_face_t *face, int *x, int *y);

// Human-readable helpers.
const char *cube_face_name(cube_face_t f);
cube_face_t cube_face_from_char(char c); // 'T','B','N','S','E','W'

// Get the color used by the edge-match calibration for a specific edge-shared
// "seam". Each of the 12 cube edges has a unique color so adjacent panels can
// show the same color on their meeting edge.
void cube_edge_seam_color(cube_face_t face, cube_edge_t edge,
                          uint8_t *r, uint8_t *g, uint8_t *b);
