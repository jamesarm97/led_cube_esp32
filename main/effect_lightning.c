#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Lightning storm: dark sky broken up by occasional bolts that zig-zag
// from the top of a random side face down to the bottom. A bolt flashes
// bright for a few frames (peak + two echoes for the "double strike"),
// then fades. Dim blue-grey ambient on TOP simulates cloud cover.

#define MAX_BOLTS    3
#define BOLT_MAX_LEN 8   // 8 = full height of a side face

typedef struct {
    bool     alive;
    int8_t   face;        // one of N/S/E/W
    int8_t   path_x[BOLT_MAX_LEN];
    float    age;         // seconds since strike
    float    life;        // seconds until bolt fully fades
    float    restrike_in; // seconds until a second echo flash (<0 = no echo)
    uint8_t  hue_tint;    // 0..255; small hue shift per bolt
} bolt_t;

static bolt_t  s_bolts[MAX_BOLTS];
static float   s_next_strike;
static float   s_cloud_phase;

static const cube_face_t SIDES[4] = {FACE_NORTH, FACE_SOUTH, FACE_EAST, FACE_WEST};

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void spawn_bolt(void) {
    for (int i = 0; i < MAX_BOLTS; i++) {
        if (s_bolts[i].alive) continue;
        s_bolts[i].alive = true;
        s_bolts[i].face  = SIDES[rng() & 3];
        // Jagged path: each row's x is previous x ±{0, 1} bounded to [0, 7].
        int x = rng() % 8;
        for (int y = 0; y < BOLT_MAX_LEN; y++) {
            s_bolts[i].path_x[y] = (int8_t)x;
            int step = (int)(rng() % 3) - 1; // -1, 0, +1
            x += step;
            if (x < 0) x = 0;
            if (x > 7) x = 7;
        }
        s_bolts[i].age   = 0;
        s_bolts[i].life  = 0.45f + rngf() * 0.25f;
        s_bolts[i].restrike_in = (rngf() < 0.4f) ? (0.08f + rngf() * 0.12f) : -1.0f;
        s_bolts[i].hue_tint = (uint8_t)(rng() & 0xFF);
        return;
    }
}

// -- Corner-up lightning -----------------------------------------------------
//
// In corner-up mode, "the top" is the TOP+WEST+SOUTH vertex and "down"
// is the opposite corner. Clouds glow on the 3 upper faces clustered
// around the vertex; each bolt is a BFS-walk along the corner-up flow
// map from one of the 3 corner seed pixels out toward the bottom corner,
// with random next-cell picks at every step for the jagged look.

#define CBOLT_PATH_MAX 16
typedef struct {
    bool          alive;
    int           path_len;
    orient_cell_t path[CBOLT_PATH_MAX];
    float         age;
    float         life;
    float         restrike_in;
    uint8_t       hue_tint;
} cbolt_t;

static cbolt_t s_cbolts[MAX_BOLTS];

// Forward decl — shared between face-up and corner-up renderers.
static void strike_pixel(cube_face_t f, int x, int y, float intensity, uint8_t tint);

static void corner_spawn_bolt(void) {
    for (int i = 0; i < MAX_BOLTS; i++) {
        if (s_cbolts[i].alive) continue;
        // Start at one of the 3 top-corner seed pixels.
        orient_cell_t c = orient_flow_seeds[rng() % orient_flow_seed_count];
        s_cbolts[i].path[0] = c;
        int len = 1;
        while (len < CBOLT_PATH_MAX) {
            int n = orient_flow_next_count[c.face][c.x][c.y];
            if (n == 0) break; // reached bottom-corner terminal
            c = orient_flow_next[c.face][c.x][c.y][rng() % n];
            s_cbolts[i].path[len++] = c;
        }
        s_cbolts[i].path_len = len;
        s_cbolts[i].alive    = true;
        s_cbolts[i].age      = 0;
        s_cbolts[i].life     = 0.45f + rngf() * 0.25f;
        s_cbolts[i].restrike_in = (rngf() < 0.4f) ? (0.08f + rngf() * 0.12f) : -1.0f;
        s_cbolts[i].hue_tint    = (uint8_t)(rng() & 0xFF);
        return;
    }
}

static void lightning_enter(void) {
    memset(s_bolts, 0, sizeof(s_bolts));
    memset(s_cbolts, 0, sizeof(s_cbolts));
    s_next_strike = 0.3f;
    s_cloud_phase = 0;
    if (orient_get() == ORIENT_CORNER_UP) {
        orient_build_flow_from_top();
    }
}

// Render cloud glow near the top corner (cells within a few BFS steps of
// the seed pixels). Intensity breathes with s_cloud_phase.
static void corner_draw_clouds(void) {
    float breathe = 0.75f + 0.25f * sinf(s_cloud_phase * 1.6f);
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int16_t d = orient_flow_dist[f][x][y];
                if (d > 3) continue;
                float k = (4.0f - d) / 4.0f;
                float v = 0.08f + 0.08f * k * breathe;
                uint8_t r = (uint8_t)(v * 60);
                uint8_t g = (uint8_t)(v * 75);
                uint8_t b = (uint8_t)(v * 110);
                render_add((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

static void corner_lightning_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    float avg_interval = 2.2f - (speed / 255.0f) * 1.5f;

    s_cloud_phase += dt * 0.4f;
    s_next_strike -= dt;
    if (s_next_strike <= 0) {
        corner_spawn_bolt();
        s_next_strike = avg_interval * (0.5f + rngf());
    }

    // Ambient cloud glow.
    corner_draw_clouds();

    // Bolts.
    for (int i = 0; i < MAX_BOLTS; i++) {
        cbolt_t *bt = &s_cbolts[i];
        if (!bt->alive) continue;
        bt->age += dt;

        if (bt->restrike_in > 0) {
            bt->restrike_in -= dt;
            if (bt->restrike_in <= 0) {
                bt->age  = 0;
                bt->life = 0.25f + rngf() * 0.15f;
                bt->restrike_in = -1.0f;
            }
        }
        if (bt->age >= bt->life) { bt->alive = false; continue; }

        float t = bt->age / bt->life;
        float env = (1.0f - t); env *= env;

        // Walk the bolt's path; each cell at full brightness, plus 1-cell
        // BFS-forward halo at lower intensity for the "glow sleeve".
        for (int k = 0; k < bt->path_len; k++) {
            orient_cell_t c = bt->path[k];
            strike_pixel((cube_face_t)c.face, c.x, c.y, env, bt->hue_tint);
            // Halo along 4-neighbors (cheap proxy for "glow around bolt").
            static const int DX[4] = {+1, -1, 0, 0};
            static const int DY[4] = {0, 0, +1, -1};
            for (int d = 0; d < 4; d++) {
                int nx = c.x + DX[d], ny = c.y + DY[d];
                cube_face_t nf = (cube_face_t)c.face;
                if (nx < 0 || nx > 7 || ny < 0 || ny > 7) {
                    if (!cube_step_over_edge(&nf, &nx, &ny)) continue;
                }
                strike_pixel(nf, nx, ny, env * 0.3f, bt->hue_tint);
            }
        }

        // On peak, add a bright flash over the top-corner cluster so the
        // whole "sky" lights up briefly — mirrors the face-up behavior.
        if (env > 0.6f) {
            float flash = (env - 0.6f) * 2.5f;
            for (int f = 0; f < CUBE_FACE_COUNT; f++) {
                for (int y = 0; y < 8; y++) {
                    for (int x = 0; x < 8; x++) {
                        if (orient_flow_dist[f][x][y] > 3) continue;
                        uint8_t v = (uint8_t)(flash * 70);
                        render_add((cube_face_t)f, x, y, v / 2, v / 2, v);
                    }
                }
            }
        }
    }
}

// Blend helper: adds a scaled bluish-white pixel.
static void strike_pixel(cube_face_t f, int x, int y, float intensity, uint8_t tint) {
    if (x < 0 || x > 7 || y < 0 || y > 7) return;
    if (intensity <= 0) return;
    // Tint shifts slightly blue ↔ slightly violet.
    uint8_t r = (uint8_t)(intensity * (190 + (int)(tint / 4) - 32));
    uint8_t g = (uint8_t)(intensity * 210);
    uint8_t b = (uint8_t)(intensity * 255);
    render_add(f, x, y, r, g, b);
}

static void lightning_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    if (orient_get() == ORIENT_CORNER_UP) {
        corner_lightning_step(dt);
        return;
    }

    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    // Strike cadence scales inversely with speed (higher speed -> more strikes).
    float avg_interval = 2.2f - (speed / 255.0f) * 1.5f; // 0.7 .. 2.2 s

    s_next_strike -= dt;
    if (s_next_strike <= 0) {
        spawn_bolt();
        s_next_strike = avg_interval * (0.5f + rngf());
    }

    // Dim cloud cover on TOP — slow blue-grey noise, breathing.
    s_cloud_phase += dt * 0.4f;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float u = (x - 3.5f) / 3.5f;
            float v = (y - 3.5f) / 3.5f;
            float n = sinf((u + s_cloud_phase) * 3.14f) *
                      sinf((v - s_cloud_phase * 0.7f) * 2.3f);
            float intensity = 0.08f + 0.06f * fmaxf(0.0f, n);
            uint8_t r = (uint8_t)(intensity * 60);
            uint8_t g = (uint8_t)(intensity * 75);
            uint8_t b = (uint8_t)(intensity * 110);
            render_add(FACE_TOP, x, y, r, g, b);
        }
    }

    // Update and draw bolts.
    for (int i = 0; i < MAX_BOLTS; i++) {
        bolt_t *bt = &s_bolts[i];
        if (!bt->alive) continue;
        bt->age += dt;

        // Echo strike after a short delay (double-flash realism).
        if (bt->restrike_in > 0) {
            bt->restrike_in -= dt;
            if (bt->restrike_in <= 0) {
                bt->age = 0;                   // reset for a second bright flash
                bt->life = 0.25f + rngf() * 0.15f;
                bt->restrike_in = -1.0f;
            }
        }

        if (bt->age >= bt->life) { bt->alive = false; continue; }

        // Envelope: sharp peak at start, quadratic fade-out.
        float t = bt->age / bt->life;
        float env = (1.0f - t); env *= env;

        // Main jagged path at full brightness.
        for (int y = 0; y < BOLT_MAX_LEN; y++) {
            int x = bt->path_x[y];
            strike_pixel((cube_face_t)bt->face, x, y, env, bt->hue_tint);
            // Glow halo: one pixel to each side at half intensity.
            strike_pixel((cube_face_t)bt->face, x - 1, y, env * 0.35f, bt->hue_tint);
            strike_pixel((cube_face_t)bt->face, x + 1, y, env * 0.35f, bt->hue_tint);
        }

        // Whole-TOP ambient flash on peak (sky briefly lights up).
        if (env > 0.6f) {
            float flash = (env - 0.6f) * 2.5f; // 0..1
            uint8_t b = (uint8_t)(flash * 70);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    render_add(FACE_TOP, x, y, b / 2, b / 2, b);
        }
    }
}

const effect_vtable_t g_effect_lightning = {
    .name = "lightning", .id = EFFECT_LIGHTNING,
    .enter = lightning_enter, .step = lightning_step,
};
