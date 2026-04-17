#pragma once
// Effect dispatcher. Runs a FreeRTOS task that, each frame, clears the
// framebuffer, calls the active effect's step() and flushes via render_flush().

#include "config.h"

typedef struct {
    const char *name;
    effect_id_t id;
    // Called once when this effect becomes active.
    void (*enter)(void);
    // Called each frame. dt is seconds since previous step (clamped).
    void (*step)(float dt);
} effect_vtable_t;

extern const effect_vtable_t g_effect_off;
extern const effect_vtable_t g_effect_solid;
extern const effect_vtable_t g_effect_rain;
extern const effect_vtable_t g_effect_fire;
extern const effect_vtable_t g_effect_rainbow;
extern const effect_vtable_t g_effect_calib_face;
extern const effect_vtable_t g_effect_calib_edge;
extern const effect_vtable_t g_effect_face_test;
extern const effect_vtable_t g_effect_radial;
extern const effect_vtable_t g_effect_balls;
extern const effect_vtable_t g_effect_chase;
extern const effect_vtable_t g_effect_plasma;
extern const effect_vtable_t g_effect_pingpong;
extern const effect_vtable_t g_effect_fireworks;
extern const effect_vtable_t g_effect_matrix;
extern const effect_vtable_t g_effect_galaxy;
extern const effect_vtable_t g_effect_spiral;
extern const effect_vtable_t g_effect_ripple;
extern const effect_vtable_t g_effect_warp;
extern const effect_vtable_t g_effect_aurora;
extern const effect_vtable_t g_effect_lightning;
extern const effect_vtable_t g_effect_breakout;
extern const effect_vtable_t g_effect_pulse;
extern const effect_vtable_t g_effect_tetris;
extern const effect_vtable_t g_effect_pendulum;

// Random mode: cycle through visual effects every N seconds. Interval comes
// from config_get()->random_interval_s. Automatically disables when the user
// selects an effect explicitly via config_set_effect().
void effects_set_random(bool on);
bool effects_random_active(void);

// Notify the engine that the user advanced/restarted a calibration step.
void effects_calib_nudge(void);

// Start the 60Hz effect task. Assumes render_start() + config_init() already ran.
void effects_start(void);

// Call when config effect changes so enter() is re-invoked.
void effects_notify_effect_changed(void);
