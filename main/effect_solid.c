#include "effects.h"
#include "render.h"

#include <stddef.h>

// Fills every face with (solid_r, solid_g, solid_b). Black is just solid=0.
static void solid_step(float dt) {
    (void)dt;
    config_lock();
    uint8_t r = config_get()->solid_r;
    uint8_t g = config_get()->solid_g;
    uint8_t b = config_get()->solid_b;
    config_unlock();
    for (int f = 0; f < CUBE_FACE_COUNT; f++) render_fill_face((cube_face_t)f, r, g, b);
}

// "Off" is just "don't draw anything" — effects.c already clears each frame.
static void off_step(float dt) { (void)dt; }

const effect_vtable_t g_effect_solid = {
    .name = "solid", .id = EFFECT_SOLID, .enter = NULL, .step = solid_step,
};

const effect_vtable_t g_effect_off = {
    .name = "off", .id = EFFECT_OFF, .enter = NULL, .step = off_step,
};
