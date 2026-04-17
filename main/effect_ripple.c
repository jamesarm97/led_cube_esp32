#include "effects.h"
#include "render.h"
#include "cube.h"
#include "orient.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Ripple with cross-face interference. Each drop is a point in 3D cube-
// world space (not just on a single face), and every pixel on every face
// is illuminated by ALL active drops as a function of its 3D distance to
// them. A pixel adds contribution from drop i of:
//
//     intensity_i = gaussian(|dist - radius_i|, sigma) * envelope(age_i)
//
// Sums over drops, so when two wavefronts meet a pixel simultaneously the
// color adds — visible interference lobes. Because distance is measured
// in 3D, a drop on TOP sends its wave "around" onto the adjacent side
// faces naturally; drops on opposite faces rarely overlap (the wave dies
// before reaching them).

#define MAX_DROPS 10

typedef struct {
    bool     alive;
    float    cx, cy, cz;   // 3D center position in [0, 1]^3
    float    age;
    float    life;         // seconds until drop retires
    uint8_t  r, g, b;
} drop_t;

static drop_t s_drops[MAX_DROPS];
static float  s_spawn_timer;

// Precomputed 3D position of every pixel. Rebuilt in ripple_enter().
static float  s_px[CUBE_FACE_COUNT][8][8][3];

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void random_bright_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    float h = (rng() & 0xFFF) / 4096.0f;
    float c = 1.0f;
    float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1));
    int seg = (int)(h * 6.0f);
    float rp = 0, gp = 0, bp = 0;
    switch (seg) {
        case 0: rp = c; gp = x; break;
        case 1: rp = x; gp = c; break;
        case 2: gp = c; bp = x; break;
        case 3: gp = x; bp = c; break;
        case 4: rp = x; bp = c; break;
        default:rp = c; bp = x; break;
    }
    *r = (uint8_t)(rp * 255);
    *g = (uint8_t)(gp * 255);
    *b = (uint8_t)(bp * 255);
}

static void spawn_drop(void) {
    for (int i = 0; i < MAX_DROPS; i++) {
        if (s_drops[i].alive) continue;
        cube_face_t f = (cube_face_t)(rng() % CUBE_FACE_COUNT);
        int x = rng() & 7, y = rng() & 7;
        float px, py, pz;
        orient_pixel_pos3d(f, x, y, &px, &py, &pz);
        s_drops[i].alive = true;
        s_drops[i].cx = px;
        s_drops[i].cy = py;
        s_drops[i].cz = pz;
        s_drops[i].age  = 0;
        s_drops[i].life = 1.6f + rngf() * 0.9f;
        random_bright_color(&s_drops[i].r, &s_drops[i].g, &s_drops[i].b);
        return;
    }
}

static void ripple_enter(void) {
    memset(s_drops, 0, sizeof(s_drops));
    s_spawn_timer = 0;
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                orient_pixel_pos3d((cube_face_t)f, x, y,
                                   &s_px[f][x][y][0],
                                   &s_px[f][x][y][1],
                                   &s_px[f][x][y][2]);
            }
        }
    }
}

static void ripple_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    // Spawn cadence: one new drop every 0.15-0.25 s. MAX_DROPS caps simultaneous count.
    s_spawn_timer -= dt;
    while (s_spawn_timer <= 0) {
        spawn_drop();
        s_spawn_timer += 0.15f + rngf() * 0.10f;
    }

    // Wavefront travels at a fixed rate in CUBE-world units (the cube is
    // [0, 1]^3). A typical face-diagonal is ~sqrt(2)/2 ≈ 0.71 units. A drop
    // living ~2 s at 0.42 u/s propagates a radius of ~0.85 units — enough to
    // cross the near half of the cube and interfere with nearby drops.
    const float ring_speed = 0.42f;        // world-units / sec
    const float ring_sigma = 0.08f;        // ring thickness (world units)

    // Advance drops, retire expired.
    for (int i = 0; i < MAX_DROPS; i++) {
        drop_t *d = &s_drops[i];
        if (!d->alive) continue;
        d->age += dt;
        if (d->age >= d->life) d->alive = false;
    }

    // Render: every pixel on every face gets the summed contribution of
    // every live drop. Additive blending => interference pattern wherever
    // wavefronts overlap.
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                float ppx = s_px[f][x][y][0];
                float ppy = s_px[f][x][y][1];
                float ppz = s_px[f][x][y][2];

                float acc_r = 0, acc_g = 0, acc_b = 0;
                for (int i = 0; i < MAX_DROPS; i++) {
                    drop_t *d = &s_drops[i];
                    if (!d->alive) continue;

                    float dxp = ppx - d->cx;
                    float dyp = ppy - d->cy;
                    float dzp = ppz - d->cz;
                    float dist = sqrtf(dxp * dxp + dyp * dyp + dzp * dzp);
                    float radius = d->age * ring_speed;
                    float delta  = dist - radius;
                    if (fabsf(delta) > 3.0f * ring_sigma) continue;

                    float k = expf(-(delta * delta) / (2 * ring_sigma * ring_sigma));
                    float life_t = d->age / d->life;
                    float envelope = 1.0f - life_t;
                    envelope *= envelope;
                    // Distance-fade so far-away wavefronts don't outshine the
                    // pixel's own face — gives the effect a bias toward local
                    // interference.
                    float dist_fade = expf(-dist * 1.2f);
                    float w = k * envelope * dist_fade;

                    acc_r += d->r * w;
                    acc_g += d->g * w;
                    acc_b += d->b * w;
                }

                if (acc_r < 2 && acc_g < 2 && acc_b < 2) continue;
                if (acc_r > 255) acc_r = 255;
                if (acc_g > 255) acc_g = 255;
                if (acc_b > 255) acc_b = 255;
                render_add((cube_face_t)f, x, y,
                           (uint8_t)acc_r, (uint8_t)acc_g, (uint8_t)acc_b);
            }
        }
    }
}

const effect_vtable_t g_effect_ripple = {
    .name = "ripple", .id = EFFECT_RIPPLE,
    .enter = ripple_enter, .step = ripple_step,
};
