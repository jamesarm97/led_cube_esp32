#include "cube.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "cube";

static cube_calib_t s_calib;
static int s_face_to_panel[CUBE_FACE_COUNT]; // inverse of panel_map, -1 if unmapped

// Canonical cube topology. Viewed with TOP up, NORTH away, SOUTH toward us,
// EAST right, WEST left, BOTTOM below.
//
// Per-face local coords (looking at the face from outside the cube):
//   TOP    : x=east,   y=south (you're looking down; "up" in the image is NORTH)
//   BOTTOM : x=east,   y=north
//   NORTH  : x=west,   y=down   ("up" is TOP)
//   SOUTH  : x=east,   y=down
//   EAST   : x=south,  y=down
//   WEST   : x=north,  y=down
//
// Edges: EDGE_TOP = y=0, EDGE_BOTTOM = y=7, EDGE_LEFT = x=0, EDGE_RIGHT = x=7.
//
// For each face/edge we list (neighbor_face, neighbor_edge, flip). `flip`
// indicates whether the coordinate along the shared edge is reversed when
// crossing the seam.
static const cube_adj_t s_adj[CUBE_FACE_COUNT][4] = {
    // TOP. Looking down: x=east, y=south.
    [FACE_TOP] = {
        [EDGE_TOP]    = { FACE_NORTH, EDGE_TOP,    true  }, // top of TOP meets top of NORTH, reversed along x
        [EDGE_BOTTOM] = { FACE_SOUTH, EDGE_TOP,    false }, // bottom of TOP meets top of SOUTH
        [EDGE_LEFT]   = { FACE_WEST,  EDGE_TOP,    false }, // left of TOP meets top of WEST
        [EDGE_RIGHT]  = { FACE_EAST,  EDGE_TOP,    true  }, // right of TOP meets top of EAST, reversed along y
    },
    // BOTTOM. Looking up from below: x=east, y=north.
    [FACE_BOTTOM] = {
        [EDGE_TOP]    = { FACE_SOUTH, EDGE_BOTTOM, true  },
        [EDGE_BOTTOM] = { FACE_NORTH, EDGE_BOTTOM, false },
        [EDGE_LEFT]   = { FACE_WEST,  EDGE_BOTTOM, true  },
        [EDGE_RIGHT]  = { FACE_EAST,  EDGE_BOTTOM, false },
    },
    // NORTH. Facing north from outside: x=west, y=down.
    [FACE_NORTH] = {
        [EDGE_TOP]    = { FACE_TOP,    EDGE_TOP,    true  },
        [EDGE_BOTTOM] = { FACE_BOTTOM, EDGE_BOTTOM, false },
        [EDGE_LEFT]   = { FACE_EAST,   EDGE_RIGHT,  false },
        [EDGE_RIGHT]  = { FACE_WEST,   EDGE_LEFT,   false },
    },
    // SOUTH. Facing south from outside: x=east, y=down.
    [FACE_SOUTH] = {
        [EDGE_TOP]    = { FACE_TOP,    EDGE_BOTTOM, false },
        [EDGE_BOTTOM] = { FACE_BOTTOM, EDGE_TOP,    true  },
        [EDGE_LEFT]   = { FACE_WEST,   EDGE_RIGHT,  false },
        [EDGE_RIGHT]  = { FACE_EAST,   EDGE_LEFT,   false },
    },
    // EAST. Facing east from outside: x=south, y=down.
    [FACE_EAST] = {
        [EDGE_TOP]    = { FACE_TOP,    EDGE_RIGHT,  true  },
        [EDGE_BOTTOM] = { FACE_BOTTOM, EDGE_RIGHT,  false },
        [EDGE_LEFT]   = { FACE_NORTH,  EDGE_LEFT,   false },
        [EDGE_RIGHT]  = { FACE_SOUTH,  EDGE_RIGHT,  false },
    },
    // WEST. Facing west from outside: x=north, y=down.
    [FACE_WEST] = {
        [EDGE_TOP]    = { FACE_TOP,    EDGE_LEFT,   false },
        [EDGE_BOTTOM] = { FACE_BOTTOM, EDGE_LEFT,   true  },
        [EDGE_LEFT]   = { FACE_SOUTH,  EDGE_LEFT,   false },
        [EDGE_RIGHT]  = { FACE_NORTH,  EDGE_RIGHT,  false },
    },
};

// Each of the 12 cube edges gets a distinct color for assembly calibration.
// Indexed by a canonical edge id. We look up a canonical id from (face, edge).
typedef struct { cube_face_t a; cube_edge_t ae; cube_face_t b; cube_edge_t be; uint8_t r, g, bl; } seam_t;

static const seam_t s_seams[] = {
    // Top's four edges.
    { FACE_TOP, EDGE_TOP,    FACE_NORTH, EDGE_TOP,    0xFF, 0x00, 0x00 }, // red   : TOP<->NORTH
    { FACE_TOP, EDGE_BOTTOM, FACE_SOUTH, EDGE_TOP,    0xFF, 0xFF, 0x00 }, // yellow: TOP<->SOUTH
    { FACE_TOP, EDGE_LEFT,   FACE_WEST,  EDGE_TOP,    0x00, 0xFF, 0x00 }, // green : TOP<->WEST
    { FACE_TOP, EDGE_RIGHT,  FACE_EAST,  EDGE_TOP,    0x00, 0x80, 0xFF }, // blue  : TOP<->EAST
    // Bottom's four edges.
    { FACE_BOTTOM, EDGE_TOP,    FACE_SOUTH, EDGE_BOTTOM, 0xFF, 0x40, 0x00 }, // orange: BOTTOM<->SOUTH
    { FACE_BOTTOM, EDGE_BOTTOM, FACE_NORTH, EDGE_BOTTOM, 0xC0, 0x00, 0xFF }, // purple: BOTTOM<->NORTH
    { FACE_BOTTOM, EDGE_LEFT,   FACE_WEST,  EDGE_BOTTOM, 0x00, 0xFF, 0xFF }, // cyan  : BOTTOM<->WEST
    { FACE_BOTTOM, EDGE_RIGHT,  FACE_EAST,  EDGE_BOTTOM, 0xFF, 0x00, 0xFF }, // magenta:BOTTOM<->EAST
    // Four vertical edges.
    { FACE_NORTH, EDGE_LEFT,   FACE_EAST,  EDGE_RIGHT,  0xFF, 0x80, 0x80 }, // pink  : NE corner
    { FACE_NORTH, EDGE_RIGHT,  FACE_WEST,  EDGE_LEFT,   0x80, 0xFF, 0x80 }, // lime  : NW corner
    { FACE_SOUTH, EDGE_LEFT,   FACE_WEST,  EDGE_RIGHT,  0x80, 0x80, 0xFF }, // lavender: SW corner
    { FACE_SOUTH, EDGE_RIGHT,  FACE_EAST,  EDGE_LEFT,   0xFF, 0xFF, 0x80 }, // pale yellow: SE corner
};

// --- Calibration management ------------------------------------------------

static void rebuild_face_to_panel(void) {
    for (int i = 0; i < CUBE_FACE_COUNT; i++) s_face_to_panel[i] = -1;
    for (int p = 0; p < CUBE_FACE_COUNT; p++) {
        cube_face_t f = s_calib.panel_map[p];
        if (f < CUBE_FACE_COUNT) s_face_to_panel[f] = p;
    }
}

void cube_set_calibration(const cube_calib_t *calib) {
    s_calib = *calib;
    rebuild_face_to_panel();
    ESP_LOGI(TAG, "calibration updated. panel_map=[%d,%d,%d,%d,%d,%d] serpentine=%d",
             s_calib.panel_map[0], s_calib.panel_map[1], s_calib.panel_map[2],
             s_calib.panel_map[3], s_calib.panel_map[4], s_calib.panel_map[5],
             s_calib.serpentine);
}

void cube_get_calibration(cube_calib_t *out) { *out = s_calib; }

// --- Mapping ---------------------------------------------------------------

// Rotate a point (x,y) within an 8x8 panel by rot (0=0°, 1=90° CW, 2=180°, 3=270° CW).
// The rotation is applied to go from cube-world panel coords to physical panel coords.
static void rotate_xy(int rot, int x, int y, int *px, int *py) {
    switch (rot & 3) {
        default:
        case 0: *px = x;              *py = y;              break;
        case 1: *px = 7 - y;          *py = x;              break; // 90 CW
        case 2: *px = 7 - x;          *py = 7 - y;          break;
        case 3: *px = y;              *py = 7 - x;          break; // 270 CW
    }
}

static int panel_xy_to_local(int px, int py, bool serpentine) {
    if (px < 0 || px >= 8 || py < 0 || py >= 8) return -1;
    if (serpentine && (py & 1)) {
        return py * 8 + (7 - px);
    }
    return py * 8 + px;
}

int cube_logical_to_strip(cube_face_t face, int x, int y) {
    if (face >= CUBE_FACE_COUNT) return -1;
    if (x < 0 || x >= 8 || y < 0 || y >= 8) return -1;
    int panel = s_face_to_panel[face];
    if (panel < 0) return -1;
    int px, py;
    rotate_xy(s_calib.panel_rot[panel], x, y, &px, &py);
    int local = panel_xy_to_local(px, py, s_calib.serpentine);
    if (local < 0) return -1;
    return panel * CUBE_PANEL_PIXELS + local;
}

const cube_adj_t *cube_adjacent(cube_face_t face, cube_edge_t edge) {
    if (face >= CUBE_FACE_COUNT) return NULL;
    if ((unsigned)edge > 3) return NULL;
    return &s_adj[face][edge];
}

// Given a position that has stepped off one edge (one of x,y is -1 or 8), move
// it onto the neighbor face. Returns true if wrapping occurred.
bool cube_step_over_edge(cube_face_t *face, int *x, int *y) {
    cube_edge_t edge;
    int along; // coordinate along the shared edge in source frame (0..7)
    if (*y < 0)      { edge = EDGE_TOP;    along = *x; }
    else if (*y > 7) { edge = EDGE_BOTTOM; along = *x; }
    else if (*x < 0) { edge = EDGE_LEFT;   along = *y; }
    else if (*x > 7) { edge = EDGE_RIGHT;  along = *y; }
    else return false;

    const cube_adj_t *a = cube_adjacent(*face, edge);
    if (!a) return false;
    int ax = a->flip ? (7 - along) : along;

    // Place onto neighbor's edge, stepping one pixel in from the seam.
    switch (a->neighbor_edge) {
        case EDGE_TOP:    *x = ax;    *y = 0;    break;
        case EDGE_BOTTOM: *x = ax;    *y = 7;    break;
        case EDGE_LEFT:   *x = 0;     *y = ax;   break;
        case EDGE_RIGHT:  *x = 7;     *y = ax;   break;
    }
    *face = a->neighbor_face;
    return true;
}

const char *cube_face_name(cube_face_t f) {
    switch (f) {
        case FACE_TOP:    return "TOP";
        case FACE_BOTTOM: return "BOTTOM";
        case FACE_NORTH:  return "NORTH";
        case FACE_SOUTH:  return "SOUTH";
        case FACE_EAST:   return "EAST";
        case FACE_WEST:   return "WEST";
        default:          return "?";
    }
}

cube_face_t cube_face_from_char(char c) {
    switch (c) {
        case 'T': case 't': return FACE_TOP;
        case 'B': case 'b': return FACE_BOTTOM;
        case 'N': case 'n': return FACE_NORTH;
        case 'S': case 's': return FACE_SOUTH;
        case 'E': case 'e': return FACE_EAST;
        case 'W': case 'w': return FACE_WEST;
        default: return FACE_UNKNOWN;
    }
}

void cube_edge_seam_color(cube_face_t face, cube_edge_t edge,
                          uint8_t *r, uint8_t *g, uint8_t *b) {
    for (size_t i = 0; i < sizeof(s_seams) / sizeof(s_seams[0]); i++) {
        const seam_t *s = &s_seams[i];
        if ((s->a == face && s->ae == edge) || (s->b == face && s->be == edge)) {
            *r = s->r; *g = s->g; *b = s->bl;
            return;
        }
    }
    *r = *g = *b = 0;
}
