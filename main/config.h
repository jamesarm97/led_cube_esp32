#pragma once
// NVS-backed configuration: panel calibration, brightness, current effect,
// WiFi AP credentials.

#include <stdint.h>
#include <stdbool.h>
#include "cube.h"
#include "orient.h"

typedef enum {
    EFFECT_OFF = 0,
    EFFECT_SOLID,
    EFFECT_RAIN,
    EFFECT_FIRE,
    EFFECT_RAINBOW,
    EFFECT_CALIB_FACE,  // face-ID calibration (one panel lit at a time)
    EFFECT_CALIB_EDGE,  // edge-match / rotation calibration
    EFFECT_FACE_TEST,   // show face letter on each face, used for final QA
    // New effects are appended so NVS-stored ids from older firmware stay valid.
    EFFECT_RADIAL,      // color wave TOP-center -> sides -> BOTTOM-center
    EFFECT_BALLS,       // bouncing balls with gravity
    EFFECT_CHASE,       // snakes running on 3 great circles
    EFFECT_PLASMA,      // sinusoidal plasma field
    EFFECT_PINGPONG,    // 6 independent bouncing-ball games, one per face
    EFFECT_FIREWORKS,   // rockets -> explosion near top -> spill onto TOP
    EFFECT_MATRIX,      // Matrix-movie green rain on the 4 side faces
    EFFECT_GALAXY,      // rotating log-spiral arms per face
    EFFECT_SPIRAL,      // outer-edge inward spiral per face
    EFFECT_RIPPLE,      // expanding concentric rings from random drop points
    EFFECT_WARP,        // per-face warp-speed starfield radiating from center
    EFFECT_AURORA,      // flowing aurora color bands on TOP, fading to sides
    EFFECT_LIFE,        // (removed; enum slot kept to preserve NVS effect-id ordering)
    EFFECT_LIGHTNING,   // jagged lightning bolts top-to-bottom on side faces
    EFFECT_BREAKOUT,    // ball bouncing off bricks + auto paddle on TOP
    EFFECT_PULSE,       // concentric pulses emanating from the top pole
    EFFECT_TETRIS,      // tetrominoes fall on the 4 side faces, stack, clear
    EFFECT_PENDULUM,    // swinging pendulums — one per side face
    EFFECT_COUNT,
} effect_id_t;

// What to do with the active effect when the cube boots.
typedef enum {
    STARTUP_LAST     = 0,  // keep whatever was running pre-reboot (persisted)
    STARTUP_RANDOM   = 1,  // cycle random non-calibration effects every N sec
    STARTUP_SPECIFIC = 2,  // force a specific effect_id_t on boot
} startup_mode_t;

typedef struct {
    // Panel calibration.
    cube_calib_t calib;
    bool         calibrated;        // true once user confirmed face IDs

    // Runtime state.
    effect_id_t  effect;
    uint8_t      brightness;        // 0..255, global scale applied in render
    uint32_t     max_ma;            // per-frame current cap in milliamps
    uint8_t      solid_r, solid_g, solid_b;

    // Effect-specific knobs (small enough to keep in one struct).
    uint8_t      rain_density;      // 0..255, drops per frame probability
    uint8_t      rain_speed;        // 0..255, fall velocity
    uint8_t      fire_intensity;    // 0..255
    uint8_t      fire_cooling;      // 0..255
    uint8_t      rainbow_speed;     // 0..255

    // Calibration runtime state (not persisted).
    uint8_t      calib_step;        // 0..5 which physical panel we're lighting

    // Startup behavior.
    startup_mode_t startup_mode;      // LAST | RANDOM | SPECIFIC
    effect_id_t    startup_effect;    // used when startup_mode == SPECIFIC
    uint16_t       random_interval_s; // used when startup_mode == RANDOM (or random toggled at runtime)

    // Physical orientation of the cube in its stand.
    orient_mode_t  orientation;       // FACE_UP | CORNER_UP

    // WiFi AP credentials.
    char         ap_ssid[33];
    char         ap_pass[65];       // empty = open
    uint8_t      ap_channel;
} app_config_t;

// Load from NVS. Applies defaults on first boot. Writes defaults back so the
// partition is consistent.
void config_init(void);

// Push the current in-memory config to NVS. Small fields only; no effect runtime.
void config_save(void);

// Atomically mutate config. Any caller that changes fields must hold the lock
// via config_lock()/config_unlock() (the render loop + HTTP handlers do this).
app_config_t *config_get(void);
void config_lock(void);
void config_unlock(void);

// Convenience: update one field and persist.
void config_set_effect(effect_id_t e);
void config_set_brightness(uint8_t b);
void config_set_panel_face(int physical_panel, cube_face_t face);
void config_set_panel_rot(int physical_panel, uint8_t rot);
void config_set_panel_mirror(int physical_panel, uint8_t mirror);
void config_set_calibrated(bool calibrated);
void config_set_solid(uint8_t r, uint8_t g, uint8_t b);

// Swap which physical panel is mapped to FACE_EAST vs FACE_WEST. Handy when
// the user realizes post-calibration that the two are 180° reversed —
// saves re-running full face-ID calibration.
void config_swap_east_west(void);

// Startup settings.
void config_set_startup(startup_mode_t mode, effect_id_t e, uint16_t interval_s);

// Physical orientation — also updates the live orient module immediately.
void config_set_orientation(orient_mode_t m);

// Serialize/restore for the HTTP API.
const char *effect_name(effect_id_t e);
effect_id_t effect_from_name(const char *s);
const char *startup_mode_name(startup_mode_t m);
startup_mode_t startup_mode_from_name(const char *s);
