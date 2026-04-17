#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_random.h"

// Breakout, self-playing on EVERY face. Each face runs its own independent
// game: 3 rows of bricks along face-local y=0..2, an auto-tracking 3-wide
// paddle at y=7, and a ball bouncing between them. When a face clears its
// wall a new wall spawns. The paddle never misses — the "game" is
// ambience. Each face uses a slightly different ball speed and brick hue
// bank so the six boards don't sync up.

#define BRICK_ROWS 3
#define BRICK_COLS 8

typedef struct {
    bool    bricks[BRICK_ROWS][BRICK_COLS];
    uint8_t brick_hue[BRICK_ROWS][BRICK_COLS];
    float   ball_x, ball_y;
    float   ball_vx, ball_vy;
    int     paddle_x;
    float   flash_timer;
    int     flash_x, flash_y;
    uint8_t flash_hue;
    uint8_t hue_seed;   // per-face offset so boards look distinct
    float   speed_mul;  // per-face speed variation
} board_t;

static board_t s_boards[CUBE_FACE_COUNT];

static uint32_t rng(void)  { return esp_random(); }
static float    rngf(void) { return (rng() & 0xFFFFFF) / (float)0xFFFFFF; }

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b) {
    h = h - floorf(h);
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    int seg = (int)(h * 6.0f);
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

static void spawn_wall(board_t *b) {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            b->bricks[r][c] = true;
            b->brick_hue[r][c] = (uint8_t)(b->hue_seed + r * 60 + c * 5);
        }
    }
}

static void reset_ball(board_t *b) {
    b->ball_x = 3.5f;
    b->ball_y = 5.0f;
    float a = 0.9f + rngf() * 1.3f;
    float speed = 6.0f * b->speed_mul;
    b->ball_vx = cosf(a) * speed;
    b->ball_vy = -fabsf(sinf(a) * speed);
}

static void breakout_enter(void) {
    memset(s_boards, 0, sizeof(s_boards));
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        board_t *b = &s_boards[f];
        b->hue_seed  = (uint8_t)(f * 43);
        b->speed_mul = 0.85f + (f * 0.07f); // 0.85..1.20 — slight per-face variation
        b->paddle_x  = 4;
        spawn_wall(b);
        reset_ball(b);
    }
}

static int remaining_bricks(const board_t *b) {
    int n = 0;
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++)
            if (b->bricks[r][c]) n++;
    return n;
}

static void step_board(board_t *b, float dt) {
    int ball_col = (int)roundf(b->ball_x);
    if (b->paddle_x < ball_col) b->paddle_x++;
    else if (b->paddle_x > ball_col) b->paddle_x--;
    if (b->paddle_x < 1) b->paddle_x = 1;
    if (b->paddle_x > 6) b->paddle_x = 6;

    b->ball_x += b->ball_vx * dt;
    b->ball_y += b->ball_vy * dt;

    if (b->ball_x < 0) { b->ball_x = -b->ball_x;        b->ball_vx = -b->ball_vx; }
    if (b->ball_x > 7) { b->ball_x = 14.0f - b->ball_x; b->ball_vx = -b->ball_vx; }
    if (b->ball_y < 0) { b->ball_y = -b->ball_y;        b->ball_vy = -b->ball_vy; }
    if (b->ball_y > 7) {
        int bc = (int)roundf(b->ball_x);
        if (bc >= b->paddle_x - 1 && bc <= b->paddle_x + 1) {
            b->ball_y = 14.0f - b->ball_y;
            b->ball_vy = -b->ball_vy;
            b->ball_vx += (bc - b->paddle_x) * 1.5f;
        } else {
            reset_ball(b);
        }
    }

    int bx = (int)roundf(b->ball_x);
    int by = (int)roundf(b->ball_y);
    if (by >= 0 && by < BRICK_ROWS && bx >= 0 && bx < BRICK_COLS) {
        if (b->bricks[by][bx]) {
            b->bricks[by][bx] = false;
            b->flash_timer = 0.12f;
            b->flash_x = bx;
            b->flash_y = by;
            b->flash_hue = b->brick_hue[by][bx];
            b->ball_vy = -b->ball_vy;
        }
    }

    if (b->flash_timer > 0) b->flash_timer -= dt;
    if (remaining_bricks(b) == 0) {
        spawn_wall(b);
        reset_ball(b);
    }
}

static void draw_board(const board_t *b, cube_face_t face, bool single_pixel_ball) {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!b->bricks[r][c]) continue;
            uint8_t rr, gg, bb;
            hsv_to_rgb(b->brick_hue[r][c] / 255.0f, 1.0f, 0.85f, &rr, &gg, &bb);
            render_set(face, c, r, rr, gg, bb);
        }
    }
    if (b->flash_timer > 0) {
        uint8_t rr, gg, bb;
        hsv_to_rgb(b->flash_hue / 255.0f, 0.6f, 1.0f, &rr, &gg, &bb);
        render_add(face, b->flash_x, b->flash_y, rr, gg, bb);
    }
    for (int dx = -1; dx <= 1; dx++) {
        int px = b->paddle_x + dx;
        if (px < 0 || px > 7) continue;
        render_set(face, px, 7, 180, 180, 200);
    }
    if (single_pixel_ball) {
        // Snap to nearest pixel — crisp, retro look.
        int ix = (int)roundf(b->ball_x);
        int iy = (int)roundf(b->ball_y);
        if (ix >= 0 && ix <= 7 && iy >= 0 && iy <= 7)
            render_add(face, ix, iy, 255, 255, 255);
    } else {
        // Bilinear splat across up to 4 pixels — smooth sub-pixel motion.
        int ix = (int)floorf(b->ball_x), iy = (int)floorf(b->ball_y);
        float frx = b->ball_x - ix, fry = b->ball_y - iy;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int px = ix + dx, py = iy + dy;
                if (px < 0 || px > 7 || py < 0 || py > 7) continue;
                float w = (dx ? frx : 1 - frx) * (dy ? fry : 1 - fry);
                uint8_t v = (uint8_t)(w * 255);
                render_add(face, px, py, v, v, v);
            }
        }
    }
}

static void breakout_step(float dt) {
    if (dt > 0.05f) dt = 0.05f;

    config_lock();
    bool single_px = config_get()->breakout_single_pixel != 0;
    config_unlock();

    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        step_board(&s_boards[f], dt);
        draw_board(&s_boards[f], (cube_face_t)f, single_px);
    }
}

const effect_vtable_t g_effect_breakout = {
    .name = "breakout", .id = EFFECT_BREAKOUT,
    .enter = breakout_enter, .step = breakout_step,
};
