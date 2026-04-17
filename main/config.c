#include "config.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";
static const char *NVS_NS = "mcube";

static app_config_t  s_cfg;
static SemaphoreHandle_t s_mutex;

static void apply_defaults(app_config_t *c) {
    // Identity panel map: physical i -> face i. User will calibrate.
    for (int i = 0; i < CUBE_FACE_COUNT; i++) {
        c->calib.panel_map[i]    = (cube_face_t)i;
        c->calib.panel_rot[i]    = 0;
        c->calib.panel_mirror[i] = 0;
    }
    c->calib.serpentine = true;
    c->calibrated       = false;

    // Default effect is a regular visual — not a calibration mode. The
    // post-load "!calibrated" block below still forces CALIB_FACE on a truly
    // uncalibrated cube. Using RAINBOW as the default means that if the NVS
    // "effect" key is ever missing while "calibrated" is true, we recover
    // gracefully instead of silently falling back into face calibration.
    c->effect           = EFFECT_RAINBOW;
    c->brightness       = 64;                // ~25%
    c->max_ma           = 4000;              // 4A safety cap by default
    c->solid_r = 255; c->solid_g = 255; c->solid_b = 255;

    c->rain_density     = 40;
    c->rain_speed       = 150;
    c->fire_intensity   = 180;
    c->fire_cooling     = 55;
    c->rainbow_speed    = 80;

    c->calib_step       = 0;

    c->startup_mode       = STARTUP_LAST;
    c->startup_effect     = EFFECT_RAINBOW;
    c->random_interval_s  = 30;

    c->orientation        = ORIENT_FACE_UP;

    snprintf(c->ap_ssid, sizeof(c->ap_ssid), "MatrixCube");
    c->ap_pass[0]       = '\0';
    c->ap_channel       = 6;
}

static void nvs_load_blob(nvs_handle_t h, const char *key, void *out, size_t sz) {
    size_t got = sz;
    esp_err_t err = nvs_get_blob(h, key, out, &got);
    if (err != ESP_OK || got != sz) {
        ESP_LOGI(TAG, "nvs %s missing (%s); keeping defaults", key, esp_err_to_name(err));
    }
}

static void nvs_load_str(nvs_handle_t h, const char *key, char *out, size_t sz) {
    size_t got = sz;
    esp_err_t err = nvs_get_str(h, key, out, &got);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nvs %s missing (%s)", key, esp_err_to_name(err));
    }
}

static void nvs_load_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *out = v;
}

static void nvs_load_u32(nvs_handle_t h, const char *key, uint32_t *out) {
    uint32_t v;
    if (nvs_get_u32(h, key, &v) == ESP_OK) *out = v;
}

static void nvs_load_u16(nvs_handle_t h, const char *key, uint16_t *out) {
    uint16_t v;
    if (nvs_get_u16(h, key, &v) == ESP_OK) *out = v;
}

void config_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    apply_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_load_blob(h, "panel_map", s_cfg.calib.panel_map, sizeof(s_cfg.calib.panel_map));
        nvs_load_blob(h, "panel_rot", s_cfg.calib.panel_rot, sizeof(s_cfg.calib.panel_rot));
        nvs_load_blob(h, "panel_mir", s_cfg.calib.panel_mirror, sizeof(s_cfg.calib.panel_mirror));

        uint8_t serp = s_cfg.calib.serpentine;
        nvs_load_u8(h, "serpentine", &serp);
        s_cfg.calib.serpentine = serp != 0;

        uint8_t cal = s_cfg.calibrated;
        nvs_load_u8(h, "calibrated", &cal);
        s_cfg.calibrated = cal != 0;

        uint8_t effect = s_cfg.effect;
        nvs_load_u8(h, "effect", &effect);
        s_cfg.effect = (effect_id_t)effect;

        nvs_load_u8(h, "brightness", &s_cfg.brightness);
        nvs_load_u32(h, "max_ma", &s_cfg.max_ma);

        nvs_load_u8(h, "solid_r", &s_cfg.solid_r);
        nvs_load_u8(h, "solid_g", &s_cfg.solid_g);
        nvs_load_u8(h, "solid_b", &s_cfg.solid_b);

        nvs_load_u8(h, "rain_d", &s_cfg.rain_density);
        nvs_load_u8(h, "rain_s", &s_cfg.rain_speed);
        nvs_load_u8(h, "fire_i", &s_cfg.fire_intensity);
        nvs_load_u8(h, "fire_c", &s_cfg.fire_cooling);
        nvs_load_u8(h, "rbow_s", &s_cfg.rainbow_speed);

        nvs_load_str(h, "ap_ssid", s_cfg.ap_ssid, sizeof(s_cfg.ap_ssid));
        nvs_load_str(h, "ap_pass", s_cfg.ap_pass, sizeof(s_cfg.ap_pass));
        nvs_load_u8(h, "ap_chan", &s_cfg.ap_channel);

        uint8_t sm = (uint8_t)s_cfg.startup_mode;
        nvs_load_u8(h, "startup_m", &sm);
        s_cfg.startup_mode = (startup_mode_t)sm;
        uint8_t se = (uint8_t)s_cfg.startup_effect;
        nvs_load_u8(h, "startup_e", &se);
        s_cfg.startup_effect = (effect_id_t)se;
        nvs_load_u16(h, "rand_int", &s_cfg.random_interval_s);

        uint8_t o = (uint8_t)s_cfg.orientation;
        nvs_load_u8(h, "orient", &o);
        s_cfg.orientation = (orient_mode_t)o;

        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "NVS namespace %s empty; using defaults", NVS_NS);
    }

    // If calibration invalid (first boot or corruption), force calibration.
    if (!s_cfg.calibrated) {
        s_cfg.effect = EFFECT_CALIB_FACE;
    } else if (s_cfg.effect == EFFECT_CALIB_FACE ||
               s_cfg.effect == EFFECT_CALIB_EDGE ||
               s_cfg.effect == EFFECT_FACE_TEST) {
        // Cube is calibrated, but NVS last stored a calibration-mode effect
        // (typically because the user clicked "Save calibration" which
        // switches to the edge-match preview). Promote to a regular visual
        // effect on boot so the cube isn't stuck showing calibration art.
        ESP_LOGI(TAG, "calibrated; promoting boot effect %s -> rainbow",
                 effect_name(s_cfg.effect));
        s_cfg.effect = EFFECT_RAINBOW;
    }

    cube_set_calibration(&s_cfg.calib);
    ESP_LOGI(TAG, "config loaded: effect=%s brightness=%u max_ma=%u calibrated=%d",
             effect_name(s_cfg.effect), s_cfg.brightness,
             (unsigned)s_cfg.max_ma, s_cfg.calibrated);
}

void config_save(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return; }

    nvs_set_blob(h, "panel_map", s_cfg.calib.panel_map, sizeof(s_cfg.calib.panel_map));
    nvs_set_blob(h, "panel_rot", s_cfg.calib.panel_rot, sizeof(s_cfg.calib.panel_rot));
    nvs_set_blob(h, "panel_mir", s_cfg.calib.panel_mirror, sizeof(s_cfg.calib.panel_mirror));
    nvs_set_u8  (h, "serpentine", s_cfg.calib.serpentine ? 1 : 0);
    nvs_set_u8  (h, "calibrated", s_cfg.calibrated ? 1 : 0);
    nvs_set_u8  (h, "effect",     (uint8_t)s_cfg.effect);
    nvs_set_u8  (h, "brightness", s_cfg.brightness);
    nvs_set_u32 (h, "max_ma",     s_cfg.max_ma);
    nvs_set_u8  (h, "solid_r",    s_cfg.solid_r);
    nvs_set_u8  (h, "solid_g",    s_cfg.solid_g);
    nvs_set_u8  (h, "solid_b",    s_cfg.solid_b);
    nvs_set_u8  (h, "rain_d",     s_cfg.rain_density);
    nvs_set_u8  (h, "rain_s",     s_cfg.rain_speed);
    nvs_set_u8  (h, "fire_i",     s_cfg.fire_intensity);
    nvs_set_u8  (h, "fire_c",     s_cfg.fire_cooling);
    nvs_set_u8  (h, "rbow_s",     s_cfg.rainbow_speed);
    nvs_set_str (h, "ap_ssid",    s_cfg.ap_ssid);
    nvs_set_str (h, "ap_pass",    s_cfg.ap_pass);
    nvs_set_u8  (h, "ap_chan",    s_cfg.ap_channel);
    nvs_set_u8  (h, "startup_m",  (uint8_t)s_cfg.startup_mode);
    nvs_set_u8  (h, "startup_e",  (uint8_t)s_cfg.startup_effect);
    nvs_set_u16 (h, "rand_int",   s_cfg.random_interval_s);
    nvs_set_u8  (h, "orient",     (uint8_t)s_cfg.orientation);

    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit: %s", esp_err_to_name(err));
    nvs_close(h);
}

app_config_t *config_get(void) { return &s_cfg; }
void config_lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void config_unlock(void) { xSemaphoreGive(s_mutex); }

void config_set_effect(effect_id_t e) {
    config_lock();
    s_cfg.effect = e;
    config_unlock();
    config_save();
}

void config_set_brightness(uint8_t b) {
    config_lock();
    s_cfg.brightness = b;
    config_unlock();
    config_save();
}

void config_set_panel_face(int physical_panel, cube_face_t face) {
    if (physical_panel < 0 || physical_panel >= CUBE_FACE_COUNT) return;
    if (face >= CUBE_FACE_COUNT) return;
    config_lock();
    s_cfg.calib.panel_map[physical_panel] = face;
    cube_set_calibration(&s_cfg.calib);
    config_unlock();
    config_save();
}

void config_set_panel_rot(int physical_panel, uint8_t rot) {
    if (physical_panel < 0 || physical_panel >= CUBE_FACE_COUNT) return;
    config_lock();
    s_cfg.calib.panel_rot[physical_panel] = rot & 3;
    cube_set_calibration(&s_cfg.calib);
    config_unlock();
    config_save();
}

void config_set_panel_mirror(int physical_panel, uint8_t mirror) {
    if (physical_panel < 0 || physical_panel >= CUBE_FACE_COUNT) return;
    config_lock();
    s_cfg.calib.panel_mirror[physical_panel] = mirror ? 1 : 0;
    cube_set_calibration(&s_cfg.calib);
    config_unlock();
    config_save();
}

void config_set_calibrated(bool c) {
    config_lock();
    s_cfg.calibrated = c;
    config_unlock();
    config_save();
    ESP_LOGI(TAG, "calibrated = %d (saved)", c);
}

void config_set_solid(uint8_t r, uint8_t g, uint8_t b) {
    config_lock();
    s_cfg.solid_r = r; s_cfg.solid_g = g; s_cfg.solid_b = b;
    config_unlock();
    config_save();
}

void config_swap_east_west(void) {
    config_lock();
    int e = -1, w = -1;
    for (int p = 0; p < CUBE_FACE_COUNT; p++) {
        if (s_cfg.calib.panel_map[p] == FACE_EAST) e = p;
        else if (s_cfg.calib.panel_map[p] == FACE_WEST) w = p;
    }
    if (e >= 0 && w >= 0) {
        s_cfg.calib.panel_map[e] = FACE_WEST;
        s_cfg.calib.panel_map[w] = FACE_EAST;
        cube_set_calibration(&s_cfg.calib);
        ESP_LOGI(TAG, "swapped EAST(panel=%d) <-> WEST(panel=%d)", e, w);
    } else {
        ESP_LOGW(TAG, "swap_east_west: no EAST/WEST panel mapped yet");
    }
    config_unlock();
    config_save();
}

const char *effect_name(effect_id_t e) {
    switch (e) {
        case EFFECT_OFF:         return "off";
        case EFFECT_SOLID:       return "solid";
        case EFFECT_RAIN:        return "rain";
        case EFFECT_FIRE:        return "fire";
        case EFFECT_RAINBOW:     return "rainbow";
        case EFFECT_CALIB_FACE:  return "calib_face";
        case EFFECT_CALIB_EDGE:  return "calib_edge";
        case EFFECT_FACE_TEST:   return "face_test";
        case EFFECT_RADIAL:      return "radial";
        case EFFECT_BALLS:       return "balls";
        case EFFECT_CHASE:       return "chase";
        case EFFECT_PLASMA:      return "plasma";
        case EFFECT_PINGPONG:    return "pingpong";
        case EFFECT_FIREWORKS:   return "fireworks";
        case EFFECT_MATRIX:      return "matrix";
        case EFFECT_GALAXY:      return "galaxy";
        case EFFECT_SPIRAL:      return "spiral";
        case EFFECT_RIPPLE:      return "ripple";
        case EFFECT_WARP:        return "warp";
        case EFFECT_AURORA:      return "aurora";
        case EFFECT_LIGHTNING:   return "lightning";
        case EFFECT_BREAKOUT:    return "breakout";
        case EFFECT_PULSE:       return "pulse";
        case EFFECT_TETRIS:      return "tetris";
        case EFFECT_PENDULUM:    return "pendulum";
        default:                 return "?";
    }
}

effect_id_t effect_from_name(const char *s) {
    if (!s) return EFFECT_OFF;
    if (!strcmp(s, "off"))        return EFFECT_OFF;
    if (!strcmp(s, "solid"))      return EFFECT_SOLID;
    if (!strcmp(s, "rain"))       return EFFECT_RAIN;
    if (!strcmp(s, "fire"))       return EFFECT_FIRE;
    if (!strcmp(s, "rainbow"))    return EFFECT_RAINBOW;
    if (!strcmp(s, "calib_face")) return EFFECT_CALIB_FACE;
    if (!strcmp(s, "calib_edge")) return EFFECT_CALIB_EDGE;
    if (!strcmp(s, "face_test"))  return EFFECT_FACE_TEST;
    if (!strcmp(s, "radial"))     return EFFECT_RADIAL;
    if (!strcmp(s, "balls"))      return EFFECT_BALLS;
    if (!strcmp(s, "chase"))      return EFFECT_CHASE;
    if (!strcmp(s, "plasma"))     return EFFECT_PLASMA;
    if (!strcmp(s, "pingpong"))   return EFFECT_PINGPONG;
    if (!strcmp(s, "fireworks"))  return EFFECT_FIREWORKS;
    if (!strcmp(s, "matrix"))     return EFFECT_MATRIX;
    if (!strcmp(s, "galaxy"))     return EFFECT_GALAXY;
    if (!strcmp(s, "spiral"))     return EFFECT_SPIRAL;
    if (!strcmp(s, "ripple"))     return EFFECT_RIPPLE;
    if (!strcmp(s, "warp"))       return EFFECT_WARP;
    if (!strcmp(s, "aurora"))     return EFFECT_AURORA;
    if (!strcmp(s, "lightning"))  return EFFECT_LIGHTNING;
    if (!strcmp(s, "breakout"))   return EFFECT_BREAKOUT;
    if (!strcmp(s, "pulse"))      return EFFECT_PULSE;
    if (!strcmp(s, "tetris"))     return EFFECT_TETRIS;
    if (!strcmp(s, "pendulum"))   return EFFECT_PENDULUM;
    return EFFECT_OFF;
}

const char *startup_mode_name(startup_mode_t m) {
    switch (m) {
        case STARTUP_LAST:     return "last";
        case STARTUP_RANDOM:   return "random";
        case STARTUP_SPECIFIC: return "specific";
        default:               return "last";
    }
}

startup_mode_t startup_mode_from_name(const char *s) {
    if (!s) return STARTUP_LAST;
    if (!strcmp(s, "random"))   return STARTUP_RANDOM;
    if (!strcmp(s, "specific")) return STARTUP_SPECIFIC;
    return STARTUP_LAST;
}

void config_set_orientation(orient_mode_t m) {
    config_lock();
    s_cfg.orientation = m;
    config_unlock();
    orient_set(m);     // apply immediately so running effects feel it next frame
    config_save();
}

void config_set_startup(startup_mode_t mode, effect_id_t e, uint16_t interval_s) {
    if (interval_s < 5)    interval_s = 5;
    if (interval_s > 3600) interval_s = 3600;
    if (e >= EFFECT_COUNT) e = EFFECT_RAINBOW;
    config_lock();
    s_cfg.startup_mode      = mode;
    s_cfg.startup_effect    = e;
    s_cfg.random_interval_s = interval_s;
    config_unlock();
    config_save();
}
