#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Matrix rain, orientation-aware. Flow field is built by a BFS from the
// "top pole" pixels so every non-terminal cell has at least one neighbor
// with BFS-distance = self + 1. For each cell we record the full list of
// such down-stream neighbors (up to 4); each stream hop picks one at
// random, which makes streams spread out across all faces instead of
// funneling through a single path. The head lights a pixel in a decaying
// fade buffer; the trail is whatever is still warm from previous heads.
//
// Face-up mode seeds the TOP 2x2 center → flow radiates across TOP, down
// every side, then converges on BOTTOM center.
// Corner-up mode seeds the 3 pixels at the TOP+WEST+SOUTH vertex → flow
// spreads around the three adjacent faces, across the hex equator, and
// converges at the opposite corner.

#define MAX_STREAMS   48
#define CELL_COUNT    (CUBE_FACE_COUNT * 8 * 8)

typedef struct { int8_t face; int8_t x, y; } cell_t;

static int16_t  s_dist[CUBE_FACE_COUNT][8][8];
static cell_t   s_next[CUBE_FACE_COUNT][8][8][4];
static uint8_t  s_next_count[CUBE_FACE_COUNT][8][8];
static cell_t   s_seeds[8];
static int      s_seed_count;

static uint8_t  s_fade[CUBE_FACE_COUNT][8][8];

typedef struct {
    bool     alive;
    cell_t   head;
    float    step_timer;
    float    step_interval;
} stream_t;

static stream_t s_streams[MAX_STREAMS];
static float    s_spawn_timer;

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

// 4-connected neighbor, crossing face seams when needed. Returns false only
// if the seam lookup fails (shouldn't happen on a well-formed cube).
static bool neighbor(cube_face_t f, int x, int y, int dx, int dy, cell_t *out) {
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

static void set_seeds(void) {
    s_seed_count = 0;
    if (orient_get() == ORIENT_CORNER_UP) {
        // The three pixels adjacent to the TOP+WEST+SOUTH corner.
        s_seeds[s_seed_count++] = (cell_t){FACE_TOP,   0, 7};
        s_seeds[s_seed_count++] = (cell_t){FACE_WEST,  7, 0};
        s_seeds[s_seed_count++] = (cell_t){FACE_SOUTH, 0, 0};
    } else {
        // Face-up: the 2x2 center of TOP.
        s_seeds[s_seed_count++] = (cell_t){FACE_TOP, 3, 3};
        s_seeds[s_seed_count++] = (cell_t){FACE_TOP, 4, 3};
        s_seeds[s_seed_count++] = (cell_t){FACE_TOP, 3, 4};
        s_seeds[s_seed_count++] = (cell_t){FACE_TOP, 4, 4};
    }
}

static void build_flow(void) {
    // Initialize distances to "infinity".
    for (int f = 0; f < CUBE_FACE_COUNT; f++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                s_dist[f][x][y] = INT16_MAX;

    // BFS queue — fixed-size ring, at most one entry per cell.
    static cell_t queue[CELL_COUNT];
    int head = 0, tail = 0;

    for (int i = 0; i < s_seed_count; i++) {
        cell_t s = s_seeds[i];
        s_dist[s.face][s.x][s.y] = 0;
        queue[tail++] = s;
    }

    static const int DX[4] = {+1, -1, 0, 0};
    static const int DY[4] = {0, 0, +1, -1};

    while (head < tail) {
        cell_t c = queue[head++];
        int16_t cd = s_dist[c.face][c.x][c.y];
        for (int d = 0; d < 4; d++) {
            cell_t n;
            if (!neighbor((cube_face_t)c.face, c.x, c.y, DX[d], DY[d], &n)) continue;
            if (s_dist[n.face][n.x][n.y] > cd + 1) {
                s_dist[n.face][n.x][n.y] = cd + 1;
                queue[tail++] = n;
            }
        }
    }

    // For every cell collect all neighbors whose BFS-distance is exactly
    // self+1. Streams will pick randomly from that list at each hop —
    // that's what gets streams visiting the whole cube instead of funneling.
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int16_t cd = s_dist[f][x][y];
                int count = 0;
                for (int d = 0; d < 4; d++) {
                    cell_t n;
                    if (!neighbor((cube_face_t)f, x, y, DX[d], DY[d], &n)) continue;
                    if (s_dist[n.face][n.x][n.y] == cd + 1) {
                        s_next[f][x][y][count++] = n;
                    }
                }
                s_next_count[f][x][y] = (uint8_t)count;
            }
        }
    }
}

static void spawn_stream(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (s_streams[i].alive) continue;
        s_streams[i].alive         = true;
        s_streams[i].head          = s_seeds[rng() % s_seed_count];
        s_streams[i].step_interval = 0.045f + rngf() * 0.09f;
        s_streams[i].step_timer    = 0;
        return;
    }
}

static void matrix_enter(void) {
    memset(s_streams, 0, sizeof(s_streams));
    memset(s_fade, 0, sizeof(s_fade));
    s_spawn_timer = 0;
    set_seeds();
    build_flow();
}

static void matrix_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    // Brisk spawn cadence — faster than the old effect so the cube stays
    // visibly full of streams.
    s_spawn_timer -= dt;
    while (s_spawn_timer <= 0) {
        spawn_stream();
        s_spawn_timer += 0.055f + rngf() * 0.05f;
    }

    // Advance every live stream. A stream dies when it hits a terminal
    // (cell with no down-stream neighbor — a BFS leaf, i.e., the bottom
    // pole cluster).
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &s_streams[i];
        if (!s->alive) continue;
        s->step_timer += dt;
        while (s->step_timer >= s->step_interval) {
            s->step_timer -= s->step_interval;
            s_fade[s->head.face][s->head.x][s->head.y] = 255;
            int n = s_next_count[s->head.face][s->head.x][s->head.y];
            if (n == 0) { s->alive = false; break; }
            s->head = s_next[s->head.face][s->head.x][s->head.y][rng() % n];
            s_fade[s->head.face][s->head.x][s->head.y] = 255;
        }
    }

    // Decay the fade buffer — trails visible for ~0.9s.
    int decay = (int)(dt * 260.0f);
    if (decay < 1) decay = 1;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int v = s_fade[f][x][y] - decay;
                s_fade[f][x][y] = (v < 0) ? 0 : (uint8_t)v;
            }
        }
    }

    // Render: near-white-green head, pure green quadratic trail.
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t v = s_fade[f][x][y];
                if (!v) continue;
                uint8_t r, g, b;
                if (v > 220) {
                    r = v - 180; g = v; b = v - 180;
                } else {
                    r = 0; g = (uint8_t)((int)v * v / 255); b = 0;
                }
                render_set((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

const effect_vtable_t g_effect_matrix = {
    .name = "matrix", .id = EFFECT_MATRIX,
    .enter = matrix_enter, .step = matrix_step,
};
