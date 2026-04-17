#include "orient.h"

#include <math.h>
#include <string.h>

static orient_mode_t s_mode = ORIENT_FACE_UP;

void orient_set(orient_mode_t m)  { s_mode = m; }
orient_mode_t orient_get(void)    { return s_mode; }

void orient_pixel_pos3d(cube_face_t f, int x, int y,
                        float *px, float *py, float *pz) {
    // Pixel center in face-local [0, 1] range.
    float fx = (x + 0.5f) / 8.0f;
    float fy = (y + 0.5f) / 8.0f;

    // Face-local (fx, fy) mapped to cube-world (X=east, Y=up, Z=south).
    // Axes per face are documented in cube.c:
    //   TOP    x=east,  y=south        -> (fx, 1,   fy)
    //   BOTTOM x=east,  y=north        -> (fx, 0,   1-fy)
    //   NORTH  x=west,  y=down         -> (1-fx, 1-fy, 0)
    //   SOUTH  x=east,  y=down         -> (fx,   1-fy, 1)
    //   EAST   x=south, y=down         -> (1,    1-fy, fx)
    //   WEST   x=north, y=down         -> (0,    1-fy, 1-fx)
    switch (f) {
        case FACE_TOP:    *px = fx;       *py = 1.0f;     *pz = fy;       break;
        case FACE_BOTTOM: *px = fx;       *py = 0.0f;     *pz = 1.0f - fy;break;
        case FACE_NORTH:  *px = 1.0f - fx; *py = 1.0f - fy; *pz = 0.0f;   break;
        case FACE_SOUTH:  *px = fx;        *py = 1.0f - fy; *pz = 1.0f;   break;
        case FACE_EAST:   *px = 1.0f;      *py = 1.0f - fy; *pz = fx;     break;
        case FACE_WEST:   *px = 0.0f;      *py = 1.0f - fy; *pz = 1.0f - fx; break;
        default:          *px = *py = *pz = 0.5f; break;
    }
}

float orient_flow(cube_face_t f, int x, int y) {
    float px, py, pz;
    orient_pixel_pos3d(f, x, y, &px, &py, &pz);
    if (s_mode == ORIENT_FACE_UP) {
        // y = 1 at TOP (flow = 0), y = 0 at BOTTOM (flow = 1).
        float flow = 1.0f - py;
        if (flow < 0) flow = 0;
        if (flow > 1) flow = 1;
        return flow;
    }
    // Corner-up: up-axis points toward the TOP+WEST+SOUTH corner = (0, 1, 1).
    // As a unit vector from cube center: (-1, +1, +1) / sqrt(3).
    // Projection of (pos - center) onto this unit vector:
    //     proj = (-(px-0.5) + (py-0.5) + (pz-0.5)) / sqrt(3)
    // Range: -sqrt(3)/2 .. +sqrt(3)/2 across the cube's two extreme corners.
    // flow = 0.5 - proj / sqrt(3)     -> 0 at up-corner, 1 at down-corner.
    const float INV_SQRT3 = 0.57735027f;
    float proj = (-(px - 0.5f) + (py - 0.5f) + (pz - 0.5f)) * INV_SQRT3;
    float flow = 0.5f - proj * INV_SQRT3;
    if (flow < 0) flow = 0;
    if (flow > 1) flow = 1;
    return flow;
}

// ---- BFS flow field -------------------------------------------------------

int16_t        orient_flow_dist[CUBE_FACE_COUNT][8][8];
orient_cell_t  orient_flow_next[CUBE_FACE_COUNT][8][8][ORIENT_MAX_NEXT];
uint8_t        orient_flow_next_count[CUBE_FACE_COUNT][8][8];
orient_cell_t  orient_flow_seeds[8];
int            orient_flow_seed_count;
int16_t        orient_flow_max_dist;

// One-step neighbor with face-seam wrapping.
static bool flow_neighbor(cube_face_t f, int x, int y, int dx, int dy,
                          orient_cell_t *out) {
    int nx = x + dx, ny = y + dy;
    cube_face_t nf = f;
    if (nx < 0 || nx > 7 || ny < 0 || ny > 7) {
        if (!cube_step_over_edge(&nf, &nx, &ny)) return false;
    }
    out->face = (int8_t)nf;
    out->x = (int8_t)nx;
    out->y = (int8_t)ny;
    return true;
}

static void seeds_top(void) {
    orient_flow_seed_count = 0;
    if (s_mode == ORIENT_CORNER_UP) {
        // Three pixels clustered at the TOP+WEST+SOUTH corner.
        //   TOP   : x=east, y=south -> south-west = (0, 7)
        //   WEST  : x=north, y=down -> south-top  = (0, 0)
        //   SOUTH : x=east, y=down  -> west-top   = (0, 0)
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_TOP,   0, 7};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_WEST,  0, 0};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_SOUTH, 0, 0};
    } else {
        // Face-up: 2x2 center of TOP.
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_TOP, 3, 3};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_TOP, 4, 3};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_TOP, 3, 4};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_TOP, 4, 4};
    }
}

static void seeds_bottom(void) {
    orient_flow_seed_count = 0;
    if (s_mode == ORIENT_CORNER_UP) {
        // Opposite corner: BOTTOM+EAST+NORTH vertex. Each face's corner pixel
        // at that vertex.
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_BOTTOM, 7, 7};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_EAST,   0, 7};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_NORTH,  0, 7};
    } else {
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_BOTTOM, 3, 3};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_BOTTOM, 4, 3};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_BOTTOM, 3, 4};
        orient_flow_seeds[orient_flow_seed_count++] = (orient_cell_t){FACE_BOTTOM, 4, 4};
    }
}

static void run_bfs(void) {
    for (int f = 0; f < CUBE_FACE_COUNT; f++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                orient_flow_dist[f][x][y] = INT16_MAX;

    static orient_cell_t queue[CUBE_FACE_COUNT * 8 * 8];
    int head = 0, tail = 0;
    for (int i = 0; i < orient_flow_seed_count; i++) {
        orient_cell_t s = orient_flow_seeds[i];
        orient_flow_dist[s.face][s.x][s.y] = 0;
        queue[tail++] = s;
    }

    static const int DX[4] = {+1, -1, 0, 0};
    static const int DY[4] = {0, 0, +1, -1};

    while (head < tail) {
        orient_cell_t c = queue[head++];
        int16_t cd = orient_flow_dist[c.face][c.x][c.y];
        for (int d = 0; d < 4; d++) {
            orient_cell_t n;
            if (!flow_neighbor((cube_face_t)c.face, c.x, c.y, DX[d], DY[d], &n)) continue;
            if (orient_flow_dist[n.face][n.x][n.y] > cd + 1) {
                orient_flow_dist[n.face][n.x][n.y] = cd + 1;
                queue[tail++] = n;
            }
        }
    }

    // Collect downstream neighbors (those with distance = self+1).
    orient_flow_max_dist = 0;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int16_t cd = orient_flow_dist[f][x][y];
                if (cd > orient_flow_max_dist) orient_flow_max_dist = cd;
                int count = 0;
                for (int d = 0; d < 4; d++) {
                    orient_cell_t n;
                    if (!flow_neighbor((cube_face_t)f, x, y, DX[d], DY[d], &n)) continue;
                    if (orient_flow_dist[n.face][n.x][n.y] == cd + 1) {
                        orient_flow_next[f][x][y][count++] = n;
                    }
                }
                orient_flow_next_count[f][x][y] = (uint8_t)count;
            }
        }
    }
}

void orient_build_flow_from_top(void)    { seeds_top();    run_bfs(); }
void orient_build_flow_from_bottom(void) { seeds_bottom(); run_bfs(); }
