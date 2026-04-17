#include "effects.h"
#include "render.h"
#include "cube.h"

#include <string.h>
#include <stdint.h>
#include <math.h>
#include "esp_random.h"

// Fire: classic "heat map then palette" effect. Each of the 4 side faces has
// its own 8x8 heat field that cools, rises, and gets random sparks injected
// at the bottom row. The TOP face shows a gentler continuation (flames that
// "lick" over the top edge) by sampling the side faces at their top row.
// BOTTOM is dark (cooler base).

#define SF 4  // number of side faces (N/S/E/W)
static const cube_face_t s_side_faces[SF] = {FACE_NORTH, FACE_SOUTH, FACE_EAST, FACE_WEST};

static uint8_t s_heat[SF][8][8];

// Smoke over the TOP center: a slow-rotating soft puff of white/grey covering
// the 4x4 central region, so the top face isn't black even when no flames are
// licking over the edges.
static float s_smoke_phase;

static inline uint8_t rand_u8(void) { return (uint8_t)(esp_random() & 0xFF); }

// Classic palette: black -> red -> yellow -> white.
static void palette(uint8_t t, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (t < 85) {
        *r = (t * 3); *g = 0; *b = 0;
    } else if (t < 170) {
        *r = 255; *g = ((t - 85) * 3); *b = 0;
    } else {
        *r = 255; *g = 255; *b = ((t - 170) * 3);
    }
}

static void fire_enter(void) {
    memset(s_heat, 0, sizeof(s_heat));
    s_smoke_phase = 0;
}

// Draw rotating smoke on the TOP face: two bright "heads" on a ring around
// center plus a dim grey haze over the 4x4 middle so the center LEDs are
// never fully black.
static void draw_smoke(float dt) {
    s_smoke_phase += dt * 0.8f; // radians/sec
    const float cx = 3.5f, cy = 3.5f;

    // Base haze: the center 4x4 gets a gentle grey that breathes with phase.
    float breathe = 0.35f + 0.15f * sinf(s_smoke_phase * 0.7f);
    for (int y = 2; y < 6; y++) {
        for (int x = 2; x < 6; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy);
            float atten = 1.0f - (r / 3.0f);
            if (atten < 0) atten = 0;
            uint8_t v = (uint8_t)(breathe * atten * 90.0f);
            render_add(FACE_TOP, x, y, v, v, v);
        }
    }

    // Two rotating bright heads on a ring r ~ 1.6 around the center.
    for (int head = 0; head < 2; head++) {
        float a = s_smoke_phase + head * 3.14159f;
        float hx = cx + cosf(a) * 1.6f;
        float hy = cy + sinf(a) * 1.6f;
        // Splat a 2x2 region bilinearly around (hx, hy).
        int ix = (int)floorf(hx), iy = (int)floorf(hy);
        float fx = hx - ix,        fy = hy - iy;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int px = ix + dx, py = iy + dy;
                if (px < 0 || px > 7 || py < 0 || py > 7) continue;
                float w = ((dx ? fx : 1 - fx) * (dy ? fy : 1 - fy));
                uint8_t v = (uint8_t)(w * 220.0f);
                render_add(FACE_TOP, px, py, v, v, v);
            }
        }
    }
}

static void fire_step(float dt) {
    config_lock();
    uint8_t intensity = config_get()->fire_intensity;
    uint8_t cooling   = config_get()->fire_cooling;
    config_unlock();

    // For each side face: cool, rise (copy row+1 <- row, with some blending),
    // and inject sparks at the bottom.
    for (int f = 0; f < SF; f++) {
        // 1. Cool every cell.
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t c = (rand_u8() % (cooling + 2));
                s_heat[f][x][y] = (s_heat[f][x][y] > c) ? (s_heat[f][x][y] - c) : 0;
            }
        }
        // 2. Heat rises: for y from 0 to 6, cell <- avg of (y+1, y+2, y+2).
        //    y=0 is the top row; y=7 is the bottom (flame source).
        for (int y = 0; y < 7; y++) {
            for (int x = 0; x < 8; x++) {
                int y1 = y + 1;
                int y2 = (y + 2 < 8) ? (y + 2) : 7;
                int sum = s_heat[f][x][y1] + s_heat[f][x][y2] + s_heat[f][x][y2];
                s_heat[f][x][y] = (uint8_t)(sum / 3);
            }
        }
        // 3. Sparks at bottom row: intensity controls spawn probability.
        for (int x = 0; x < 8; x++) {
            if ((rand_u8() % 256) < (intensity / 2 + 30)) {
                uint8_t spark = 160 + (rand_u8() & 0x5F); // 160..223
                s_heat[f][x][7] = spark;
            } else {
                // keep existing or decay a bit
                uint16_t h = s_heat[f][x][7] + (intensity / 6);
                s_heat[f][x][7] = (h > 255) ? 255 : (uint8_t)h;
            }
        }
    }

    // Draw: side faces directly from their heat map.
    for (int f = 0; f < SF; f++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t r, g, b;
                palette(s_heat[f][x][y], &r, &g, &b);
                render_set(s_side_faces[f], x, y, r, g, b);
            }
        }
    }

    // TOP face: flames "lick" over each of the 4 edges. For each top cell,
    // find which side face's top row it came from and sample that heat with
    // distance-based falloff.
    //
    // We sample each side's top row (y=0) and let its heat radiate 1-2 pixels
    // onto TOP. Use cube_adjacent to find the mapping.
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            // Take the max contribution from any of the 4 top edges based on
            // inverse distance to each edge.
            int dists[4] = { y, 7 - y, x, 7 - x }; // top,bot,left,right
            int heat_accum = 0;
            for (int e = 0; e < 4; e++) {
                if (dists[e] > 2) continue; // only affect top 2 rows from each edge
                const cube_adj_t *a = cube_adjacent(FACE_TOP, (cube_edge_t)e);
                if (!a) continue;
                // Figure out which side face that is.
                int side_idx = -1;
                for (int s = 0; s < SF; s++) if (s_side_faces[s] == a->neighbor_face) { side_idx = s; break; }
                if (side_idx < 0) continue;
                // Coord along the edge in TOP frame -> corresponding coord on
                // side face's top row (y=0). Apply flip.
                int along = 0;
                switch ((cube_edge_t)e) {
                    case EDGE_TOP:    along = x;         break;
                    case EDGE_BOTTOM: along = x;         break;
                    case EDGE_LEFT:   along = y;         break;
                    case EDGE_RIGHT:  along = y;         break;
                }
                int sx = a->flip ? (7 - along) : along;
                // Sample heat from top row of that side face.
                uint8_t h = s_heat[side_idx][sx][0];
                // Falloff by distance: 1.0 at the seam, 0.5 one row in, 0.2 two rows in.
                float scale = (dists[e] == 0) ? 0.9f : (dists[e] == 1 ? 0.4f : 0.15f);
                int contrib = (int)(h * scale);
                if (contrib > heat_accum) heat_accum = contrib;
            }
            uint8_t r, g, b;
            palette((uint8_t)(heat_accum > 255 ? 255 : heat_accum), &r, &g, &b);
            render_set(FACE_TOP, x, y, r, g, b);
        }
    }

    // Smoke over the TOP center: additive so it mixes with any nearby flames.
    draw_smoke(dt);

    // BOTTOM stays black (render_clear already did it).
}

const effect_vtable_t g_effect_fire = {
    .name = "fire", .id = EFFECT_FIRE, .enter = fire_enter, .step = fire_step,
};
