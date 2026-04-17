#include "effects.h"
#include "render.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

static const char *TAG = "effects";

static volatile bool        s_effect_changed;
static volatile bool        s_calib_nudge;
static volatile bool        s_random_mode;
static float                s_random_timer;  // seconds since last random swap
static const effect_vtable_t *s_current;

// Effects eligible for the random cycler — visual effects only, no
// calibration modes, no OFF.
static const effect_id_t RANDOM_POOL[] = {
    EFFECT_SOLID, EFFECT_RAIN, EFFECT_FIRE, EFFECT_RAINBOW,
    EFFECT_RADIAL, EFFECT_BALLS, EFFECT_CHASE, EFFECT_PLASMA,
    EFFECT_PINGPONG, EFFECT_FIREWORKS, EFFECT_MATRIX, EFFECT_GALAXY,
    EFFECT_SPIRAL, EFFECT_RIPPLE, EFFECT_WARP,
    EFFECT_AURORA, EFFECT_LIGHTNING, EFFECT_BREAKOUT,
    EFFECT_PULSE, EFFECT_TETRIS, EFFECT_PENDULUM,
};
#define RANDOM_POOL_N  (sizeof(RANDOM_POOL) / sizeof(RANDOM_POOL[0]))

void effects_set_random(bool on) {
    s_random_mode = on;
    s_random_timer = 0;
}
bool effects_random_active(void) { return s_random_mode; }

static void pick_random_effect(void) {
    // Avoid picking the same effect we're already on.
    config_lock();
    effect_id_t cur = config_get()->effect;
    config_unlock();
    effect_id_t next = cur;
    for (int tries = 0; tries < 8 && next == cur; tries++) {
        next = RANDOM_POOL[esp_random() % RANDOM_POOL_N];
    }
    // Directly update config without config_set_effect() so we don't persist
    // every random swap to NVS.
    config_lock();
    config_get()->effect = next;
    config_unlock();
    s_effect_changed = true;
    ESP_LOGI(TAG, "random cycle -> %s", effect_name(next));
}

static const effect_vtable_t *vtable_for(effect_id_t id) {
    switch (id) {
        case EFFECT_OFF:         return &g_effect_off;
        case EFFECT_SOLID:       return &g_effect_solid;
        case EFFECT_RAIN:        return &g_effect_rain;
        case EFFECT_FIRE:        return &g_effect_fire;
        case EFFECT_RAINBOW:     return &g_effect_rainbow;
        case EFFECT_CALIB_FACE:  return &g_effect_calib_face;
        case EFFECT_CALIB_EDGE:  return &g_effect_calib_edge;
        case EFFECT_FACE_TEST:   return &g_effect_face_test;
        case EFFECT_RADIAL:      return &g_effect_radial;
        case EFFECT_BALLS:       return &g_effect_balls;
        case EFFECT_CHASE:       return &g_effect_chase;
        case EFFECT_PLASMA:      return &g_effect_plasma;
        case EFFECT_PINGPONG:    return &g_effect_pingpong;
        case EFFECT_FIREWORKS:   return &g_effect_fireworks;
        case EFFECT_MATRIX:      return &g_effect_matrix;
        case EFFECT_GALAXY:      return &g_effect_galaxy;
        case EFFECT_SPIRAL:      return &g_effect_spiral;
        case EFFECT_RIPPLE:      return &g_effect_ripple;
        case EFFECT_WARP:        return &g_effect_warp;
        case EFFECT_AURORA:      return &g_effect_aurora;
        case EFFECT_LIGHTNING:   return &g_effect_lightning;
        case EFFECT_BREAKOUT:    return &g_effect_breakout;
        case EFFECT_PULSE:       return &g_effect_pulse;
        case EFFECT_TETRIS:      return &g_effect_tetris;
        case EFFECT_PENDULUM:    return &g_effect_pendulum;
        default:                 return &g_effect_off;
    }
}

void effects_notify_effect_changed(void) { s_effect_changed = true; }
void effects_calib_nudge(void)           { s_calib_nudge = true; }

static void effect_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(16); // ~60Hz
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_us = esp_timer_get_time();

    // Seed current effect from config.
    config_lock();
    effect_id_t cur_id = config_get()->effect;
    config_unlock();
    s_current = vtable_for(cur_id);
    if (s_current->enter) s_current->enter();

    for (;;) {
        if (s_effect_changed) {
            s_effect_changed = false;
            config_lock();
            cur_id = config_get()->effect;
            config_unlock();
            s_current = vtable_for(cur_id);
            if (s_current->enter) s_current->enter();
            ESP_LOGI(TAG, "active effect: %s", s_current->name ? s_current->name : "?");
        }
        if (s_calib_nudge) {
            s_calib_nudge = false;
            // Re-enter to advance internal step cursor.
            if (s_current->enter) s_current->enter();
        }

        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - last_us) / 1e6f;
        if (dt > 0.1f) dt = 0.1f;
        last_us = now_us;

        // Random-cycle mode: swap active effect every random_interval_s.
        if (s_random_mode) {
            s_random_timer += dt;
            config_lock();
            uint16_t iv = config_get()->random_interval_s;
            config_unlock();
            if (iv < 5) iv = 5;
            if (s_random_timer >= (float)iv) {
                s_random_timer = 0;
                pick_random_effect();
            }
        }

        render_clear();
        if (s_current && s_current->step) s_current->step(dt);
        render_flush();

        vTaskDelayUntil(&last_wake, period);
    }
}

void effects_start(void) {
    xTaskCreatePinnedToCore(effect_task, "effects", 6144, NULL, 5, NULL, 1);
}
