#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>
#include "esp_random.h"

// Matrix rain: each column on each of the 4 side faces has its own falling
// stream with random length, speed, and spawn delay. TOP and BOTTOM stay
// dark so the rain reads as "falling around the cube".
//
// Rendering: the stream's head is near-white-green (brightest) and the trail
// fades to the dimmest green at its tail. Between streams, columns are
// completely dark — that's what gives the effect its signature spacing.

#define SIDE_COUNT 4
static const cube_face_t SIDES[SIDE_COUNT] = {FACE_NORTH, FACE_SOUTH, FACE_EAST, FACE_WEST};

typedef struct {
    float  head_y;    // sub-pixel head position; grows over time. negative = "waiting to spawn".
    float  speed;     // pixels per second
    uint8_t length;   // trail length in pixels (includes head)
} stream_t;

static stream_t s_streams[SIDE_COUNT][8];

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void respawn(stream_t *s) {
    s->speed  = 4.0f + rngf() * 10.0f;     // 4..14 px/s
    s->length = 3 + (rng() % 6);           // 3..8 pixels
    s->head_y = -(rngf() * 12.0f);         // start above the face; staggered delay
}

static void matrix_enter(void) {
    for (int f = 0; f < SIDE_COUNT; f++) {
        for (int x = 0; x < 8; x++) {
            respawn(&s_streams[f][x]);
            // Stagger initial phases so not everything starts at once.
            s_streams[f][x].head_y = -(rngf() * 20.0f);
        }
    }
}

static void matrix_step(float dt) {
    for (int f = 0; f < SIDE_COUNT; f++) {
        for (int x = 0; x < 8; x++) {
            stream_t *s = &s_streams[f][x];
            s->head_y += s->speed * dt;

            // Tail position is head - length + 1. Once the tail has fallen
            // past the bottom row, respawn the stream at the top.
            if (s->head_y - s->length > 7.0f) {
                respawn(s);
                continue;
            }

            int head  = (int)floorf(s->head_y);
            int tail  = head - s->length + 1;

            for (int y = tail; y <= head; y++) {
                if (y < 0 || y > 7) continue;
                int from_head = head - y; // 0 at head, grows toward tail
                uint8_t r, g, b;
                if (from_head == 0) {
                    // Head: near-white with a strong green bias.
                    r = 180; g = 255; b = 180;
                } else {
                    // Body: pure green, linearly fading with distance from head.
                    float t = (float)from_head / (float)(s->length);
                    uint8_t intensity = (uint8_t)(255 * (1.0f - t) * (1.0f - t));
                    r = 0; g = intensity; b = 0;
                }
                render_set(SIDES[f], x, y, r, g, b);
            }
        }
    }
}

const effect_vtable_t g_effect_matrix = {
    .name = "matrix", .id = EFFECT_MATRIX,
    .enter = matrix_enter, .step = matrix_step,
};
