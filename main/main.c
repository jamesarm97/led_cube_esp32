// MatrixCube app entry. Boot order:
//   1. NVS + config (panel calibration, brightness, active effect, AP creds).
//   2. LED render driver (RMT + framebuffer).
//   3. Effect engine (60Hz task; reads config to pick effect).
//   4. WiFi SoftAP (exposes a control plane on 192.168.4.1).
//   5. Captive portal DNS (redirects all lookups to our AP IP).
//   6. HTTP server (static web UI + JSON config API).
//
// The effect task is pinned to core 1. WiFi/HTTP run on core 0 so RMT output
// timing isn't starved by network work.

#include "config.h"
#include "render.h"
#include "effects.h"
#include "net.h"
#include "http.h"
#include "dns_captive.h"
#include "orient.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "app";

void app_main(void) {
    ESP_LOGI(TAG, "MatrixCube booting");

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    config_init();
    orient_set(config_get()->orientation);  // sync orient module with persisted config
    render_start();
    effects_start();

    // Apply startup mode if the cube is calibrated; otherwise config_init
    // already forced calib_face and we skip the boot override.
    config_lock();
    app_config_t *c = config_get();
    bool calibrated = c->calibrated;
    startup_mode_t sm = c->startup_mode;
    effect_id_t se = c->startup_effect;
    config_unlock();
    if (calibrated) {
        switch (sm) {
            case STARTUP_LAST: break;  // use whatever NVS restored
            case STARTUP_SPECIFIC:
                config_set_effect(se);
                effects_notify_effect_changed();
                break;
            case STARTUP_RANDOM:
                effects_set_random(true);
                break;
        }
    }

    net_start_ap();         // SoftAP on 192.168.4.1
    dns_captive_start();    // redirects all DNS queries to the AP IP
    http_start();           // serves web UI + /api/*

    ESP_LOGI(TAG, "MatrixCube ready (startup=%s calibrated=%d)",
             startup_mode_name(sm), calibrated);
}
