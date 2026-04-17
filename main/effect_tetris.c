#include "effects.h"
#include "render.h"
#include "cube.h"

#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Tetris, self-playing per side face. Each of the 4 side faces runs its own
// 8x8 board. A random 4-cell tetromino drops from the top; when it
// collides with either the floor or a landed block, it freezes in place.
// Any completely-full row clears with a quick flash. A simple "AI"
// chooses a random column for each piece. The BOTTOM face is left dark;
// TOP shows a faint brightness matching the current fastest-filling board
// as a "hurry-up" indicator.

#define ROWS 8
#define COLS 8

typedef struct { int8_t dx, dy; } cell_off_t;

// Each tetromino is 4 cells relative to a pivot. O-piece omitted to
// simplify rotations.
#define NUM_SHAPES 5
static const cell_off_t SHAPES[NUM_SHAPES][4] = {
    // I (horizontal)
    { {-1, 0}, { 0, 0}, { 1, 0}, { 2, 0} },
    // L
    { { 0, 0}, { 0, 1}, { 0, 2}, { 1, 2} },
    // T
    { {-1, 0}, { 0, 0}, { 1, 0}, { 0, 1} },
    // S
    { { 0, 0}, { 1, 0}, { 0, 1}, {-1, 1} },
    // Z
    { {-1, 0}, { 0, 0}, { 0, 1}, { 1, 1} },
};
static const uint8_t SHAPE_HUE[NUM_SHAPES] = { 0x28, 0x80, 0xC0, 0x10, 0xE0 };

typedef struct {
    uint8_t cells[COLS][ROWS]; // 0 = empty; else hue byte of the stacked block
    int8_t  piece;
    int8_t  piece_x;
    int8_t  piece_y;
    float   drop_timer;
    float   clear_flash;      // seconds of row-clear flash remaining
    int8_t  flash_row;        // which row is flashing, -1 if none
} board_t;

#define SIDE_COUNT 4
static const cube_face_t SIDES[SIDE_COUNT] = {FACE_NORTH, FACE_SOUTH, FACE_EAST, FACE_WEST};

static board_t s_boards[SIDE_COUNT];

static uint32_t rng(void)  { return esp_random(); }

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b) {
    // Local copy to avoid cross-file dep.
    while (h < 0)     h += 1.0f;
    while (h >= 1.0f) h -= 1.0f;
    float c = v * s;
    float hp = h * 6.0f;
    int seg = (int)hp;
    float f = hp - seg;
    float x = (seg & 1) ? c * (1 - f) : c * f;
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    switch (seg) {
        case 0: rp = c; gp = x; break;
        case 1: rp = x; gp = c; break;
        case 2: gp = c; bp = x; break;
        case 3: gp = x; bp = c; break;
        case 4: rp = x; bp = c; break;
        default:rp = c; bp = x; break;
    }
    *r = (uint8_t)((rp + m) * 255);
    *g = (uint8_t)((gp + m) * 255);
    *b = (uint8_t)((bp + m) * 255);
}

// Try to place the current piece at (x, y). Returns false if any cell is
// out of bounds or collides with a stacked block.
static bool fits(const board_t *b, int px, int py) {
    for (int k = 0; k < 4; k++) {
        int cx = px + SHAPES[b->piece][k].dx;
        int cy = py + SHAPES[b->piece][k].dy;
        if (cx < 0 || cx >= COLS || cy >= ROWS) return false;
        if (cy < 0) continue; // still dropping in from above
        if (b->cells[cx][cy]) return false;
    }
    return true;
}

static void spawn_piece(board_t *b) {
    b->piece   = (int8_t)(rng() % NUM_SHAPES);
    b->piece_x = 2 + (int8_t)(rng() % 4);
    b->piece_y = 0;
    if (!fits(b, b->piece_x, b->piece_y)) {
        // Board full — clear everything and try again.
        memset(b->cells, 0, sizeof(b->cells));
    }
}

static void lock_piece(board_t *b) {
    for (int k = 0; k < 4; k++) {
        int cx = b->piece_x + SHAPES[b->piece][k].dx;
        int cy = b->piece_y + SHAPES[b->piece][k].dy;
        if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) continue;
        b->cells[cx][cy] = SHAPE_HUE[b->piece];
    }

    // Check for any fully-filled row; clear one at a time with a flash.
    for (int y = ROWS - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < COLS; x++) if (!b->cells[x][y]) { full = false; break; }
        if (full) {
            b->clear_flash = 0.18f;
            b->flash_row   = (int8_t)y;
            // Shift rows above down by 1.
            for (int yy = y; yy > 0; yy--)
                for (int x = 0; x < COLS; x++)
                    b->cells[x][yy] = b->cells[x][yy - 1];
            for (int x = 0; x < COLS; x++) b->cells[x][0] = 0;
            break; // handle multi-clears across frames
        }
    }
    spawn_piece(b);
}

static void tetris_enter(void) {
    memset(s_boards, 0, sizeof(s_boards));
    for (int s = 0; s < SIDE_COUNT; s++) {
        s_boards[s].flash_row = -1;
        spawn_piece(&s_boards[s]);
    }
}

static void tetris_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    float drop_interval = 0.40f - (speed / 255.0f) * 0.28f; // ~0.12-0.40s per step

    for (int s = 0; s < SIDE_COUNT; s++) {
        board_t *b = &s_boards[s];
        if (b->clear_flash > 0) b->clear_flash -= dt;

        b->drop_timer += dt;
        while (b->drop_timer >= drop_interval) {
            b->drop_timer -= drop_interval;
            if (fits(b, b->piece_x, b->piece_y + 1)) {
                b->piece_y++;
            } else {
                lock_piece(b);
            }
        }

        cube_face_t face = SIDES[s];

        // Draw stacked cells.
        for (int y = 0; y < ROWS; y++) {
            for (int x = 0; x < COLS; x++) {
                uint8_t h = b->cells[x][y];
                if (!h) continue;
                uint8_t r, g, bb;
                hsv_to_rgb(h / 255.0f, 0.9f, 0.75f, &r, &g, &bb);
                render_set(face, x, y, r, g, bb);
            }
        }
        // Flash the just-cleared row white.
        if (b->clear_flash > 0 && b->flash_row >= 0) {
            float k = b->clear_flash / 0.18f;
            for (int x = 0; x < COLS; x++)
                render_add(face, x, b->flash_row,
                           (uint8_t)(k * 255), (uint8_t)(k * 255), (uint8_t)(k * 255));
        }
        // Draw the live piece brighter.
        for (int k = 0; k < 4; k++) {
            int cx = b->piece_x + SHAPES[b->piece][k].dx;
            int cy = b->piece_y + SHAPES[b->piece][k].dy;
            if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) continue;
            uint8_t r, g, bb;
            hsv_to_rgb(SHAPE_HUE[b->piece] / 255.0f, 1.0f, 1.0f, &r, &g, &bb);
            render_set(face, cx, cy, r, g, bb);
        }
    }
}

const effect_vtable_t g_effect_tetris = {
    .name = "tetris", .id = EFFECT_TETRIS,
    .enter = tetris_enter, .step = tetris_step,
};
