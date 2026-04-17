#include "http.h"
#include "config.h"
#include "effects.h"
#include "cube.h"
#include "orient.h"
#include "ota.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "http";

// --- Embedded web assets (see main/CMakeLists.txt EMBED_FILES) --------------
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");

static httpd_handle_t s_server;

// --- tiny helpers ----------------------------------------------------------

static esp_err_t send_embedded(httpd_req_t *req, const char *ctype,
                               const uint8_t *start, const uint8_t *end) {
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char*)start, end - start);
}

// Read a whole request body into a caller-provided buffer; null-terminates.
static int read_body(httpd_req_t *req, char *buf, size_t cap) {
    int total = 0;
    while (total < (int)cap - 1) {
        int r = httpd_req_recv(req, buf + total, cap - 1 - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        total += r;
        if (total >= req->content_len) break;
    }
    buf[total] = 0;
    return total;
}

// Extremely minimal JSON "parser" tailored to our small, fixed-shape bodies.
// Finds the first occurrence of `"<key>"`, skips the colon, returns a pointer
// to the start of the value (number, string contents, or object/array). The
// caller does its own parsing from there. Returns NULL if not found.
static const char *json_find(const char *json, const char *key) {
    char pattern[32];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) return NULL;
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += n;
    while (*p && *p != ':') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int json_int(const char *json, const char *key, int def) {
    const char *v = json_find(json, key);
    if (!v) return def;
    return atoi(v);
}

// Copy a JSON string value (dst includes NUL) from key. Returns true on match.
static bool json_str(const char *json, const char *key, char *dst, size_t cap) {
    const char *v = json_find(json, key);
    if (!v || *v != '"') return false;
    v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < cap) dst[i++] = *v++;
    dst[i] = 0;
    return true;
}

static esp_err_t json_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t json_err(httpd_req_t *req, int code, const char *msg) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// --- handlers --------------------------------------------------------------

static esp_err_t h_index(httpd_req_t *req) {
    return send_embedded(req, "text/html; charset=utf-8", index_html_start, index_html_end);
}
static esp_err_t h_appjs(httpd_req_t *req) {
    return send_embedded(req, "application/javascript", app_js_start, app_js_end);
}
static esp_err_t h_stylecss(httpd_req_t *req) {
    return send_embedded(req, "text/css", style_css_start, style_css_end);
}

// Captive-portal probes: every major OS pings a known URL to detect an
// internet connection. Returning a 302 to our root pops the "Sign in" UI.
static esp_err_t h_captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_state(httpd_req_t *req) {
    config_lock();
    app_config_t *c = config_get();
    char out[768];
    int n = snprintf(out, sizeof(out),
        "{\"effect\":\"%s\",\"brightness\":%u,\"calibrated\":%s,\"calib_step\":%u,"
        "\"max_ma\":%u,"
        "\"solid\":{\"r\":%u,\"g\":%u,\"b\":%u},"
        "\"rain\":{\"density\":%u,\"speed\":%u},"
        "\"fire\":{\"intensity\":%u,\"cooling\":%u},"
        "\"rainbow\":{\"speed\":%u},"
        "\"breakout\":{\"single_pixel\":%s},"
        "\"panel_map\":[%d,%d,%d,%d,%d,%d],"
        "\"panel_rot\":[%u,%u,%u,%u,%u,%u],"
        "\"panel_mirror\":[%u,%u,%u,%u,%u,%u],"
        "\"serpentine\":%s,"
        "\"ap\":{\"ssid\":\"%s\",\"channel\":%u},"
        "\"startup\":{\"mode\":\"%s\",\"effect\":\"%s\",\"interval_s\":%u,\"random_active\":%s},"
        "\"orientation\":\"%s\""
        "}",
        effect_name(c->effect), c->brightness, c->calibrated ? "true" : "false",
        c->calib_step, (unsigned)c->max_ma,
        c->solid_r, c->solid_g, c->solid_b,
        c->rain_density, c->rain_speed,
        c->fire_intensity, c->fire_cooling,
        c->rainbow_speed,
        c->breakout_single_pixel ? "true" : "false",
        c->calib.panel_map[0], c->calib.panel_map[1], c->calib.panel_map[2],
        c->calib.panel_map[3], c->calib.panel_map[4], c->calib.panel_map[5],
        c->calib.panel_rot[0], c->calib.panel_rot[1], c->calib.panel_rot[2],
        c->calib.panel_rot[3], c->calib.panel_rot[4], c->calib.panel_rot[5],
        c->calib.panel_mirror[0], c->calib.panel_mirror[1], c->calib.panel_mirror[2],
        c->calib.panel_mirror[3], c->calib.panel_mirror[4], c->calib.panel_mirror[5],
        c->calib.serpentine ? "true" : "false",
        c->ap_ssid, c->ap_channel,
        startup_mode_name(c->startup_mode),
        effect_name(c->startup_effect),
        c->random_interval_s,
        effects_random_active() ? "true" : "false",
        c->orientation == ORIENT_CORNER_UP ? "corner_up" : "face_up");
    config_unlock();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

static esp_err_t h_effect(httpd_req_t *req) {
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    char name[16] = {0};
    if (!json_str(body, "name", name, sizeof(name))) return json_err(req, 400, "name");
    effect_id_t e = effect_from_name(name);
    // User explicit pick exits random-cycle mode.
    effects_set_random(false);
    config_set_effect(e);
    effects_notify_effect_changed();
    return json_ok(req);
}

// POST /api/startup  body: { mode: "last"|"random"|"specific", effect: "rainbow", interval_s: 30 }
static esp_err_t h_startup(httpd_req_t *req) {
    char body[160];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    char mode[16] = {0}, eff[16] = {0};
    if (!json_str(body, "mode", mode, sizeof(mode))) return json_err(req, 400, "mode");
    json_str(body, "effect", eff, sizeof(eff));
    int iv = json_int(body, "interval_s", 30);
    config_set_startup(startup_mode_from_name(mode), effect_from_name(eff), (uint16_t)iv);
    return json_ok(req);
}

// POST /api/orientation  body: { mode: "face_up"|"corner_up" }
static esp_err_t h_orientation(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    char mode[16] = {0};
    if (!json_str(body, "mode", mode, sizeof(mode))) return json_err(req, 400, "mode");
    orient_mode_t m = (!strcmp(mode, "corner_up")) ? ORIENT_CORNER_UP : ORIENT_FACE_UP;
    config_set_orientation(m);
    // Re-enter the current effect so orientation-aware effects rebuild
    // their precomputed fields (matrix in particular).
    effects_notify_effect_changed();
    return json_ok(req);
}

// POST /api/random  body: { on: true|false }
static esp_err_t h_random(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    const char *on = json_find(body, "on");
    bool enable = on && (on[0] == 't');
    effects_set_random(enable);
    // If enabling, kick to a new effect immediately so the user sees it work.
    if (enable) effects_notify_effect_changed();
    return json_ok(req);
}

static esp_err_t h_brightness(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    int v = json_int(body, "value", -1);
    if (v < 0 || v > 255) return json_err(req, 400, "value");
    config_set_brightness((uint8_t)v);
    return json_ok(req);
}

// POST /api/breakout  body: { "single_pixel": true|false }
static esp_err_t h_breakout(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    const char *v = json_find(body, "single_pixel");
    bool on = v && (v[0] == 't');
    config_set_breakout_single_pixel(on ? 1 : 0);
    return json_ok(req);
}

static esp_err_t h_solid(httpd_req_t *req) {
    char body[96];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    int r = json_int(body, "r", -1);
    int g = json_int(body, "g", -1);
    int b = json_int(body, "b", -1);
    if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255)
        return json_err(req, 400, "rgb");
    config_set_solid((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return json_ok(req);
}

// /api/calib/step — select which physical panel is currently lit in calib_face.
static esp_err_t h_calib_step(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    int panel = json_int(body, "panel", -1);
    if (panel < 0 || panel >= CUBE_FACE_COUNT) return json_err(req, 400, "panel");
    config_lock();
    config_get()->calib_step = panel;
    config_unlock();
    return json_ok(req);
}

// /api/calib/face — the user identified the currently-lit panel as <face>.
static esp_err_t h_calib_face(httpd_req_t *req) {
    char body[96];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    int panel = json_int(body, "panel", -1);
    char f[4] = {0};
    if (!json_str(body, "face", f, sizeof(f))) return json_err(req, 400, "face");
    cube_face_t face = cube_face_from_char(f[0]);
    if (panel < 0 || panel >= CUBE_FACE_COUNT || face >= CUBE_FACE_COUNT)
        return json_err(req, 400, "args");
    config_set_panel_face(panel, face);
    return json_ok(req);
}

static esp_err_t h_calib_rot(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    int panel = json_int(body, "panel", -1);
    int rot   = json_int(body, "rot", -1);
    if (panel < 0 || panel >= CUBE_FACE_COUNT || rot < 0 || rot > 3)
        return json_err(req, 400, "args");
    config_set_panel_rot(panel, (uint8_t)rot);
    return json_ok(req);
}

// POST /api/calib/mirror  body: { panel: 0..5, mirror: 0|1 }
//
// Horizontal flip of a panel's pixel grid. Needed for any panel whose
// physical mount results in a left-handed orientation that can't be
// achieved by the 4 possible rotations.
static esp_err_t h_calib_mirror(httpd_req_t *req) {
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return json_err(req, 400, "read");
    int panel  = json_int(body, "panel", -1);
    int mirror = json_int(body, "mirror", -1);
    if (panel < 0 || panel >= CUBE_FACE_COUNT || mirror < 0 || mirror > 1)
        return json_err(req, 400, "args");
    config_set_panel_mirror(panel, (uint8_t)mirror);
    return json_ok(req);
}

static esp_err_t h_calib_done(httpd_req_t *req) {
    config_set_calibrated(true);
    // Switch to the edge-match view so the user can verify orientation.
    config_set_effect(EFFECT_CALIB_EDGE);
    effects_notify_effect_changed();
    return json_ok(req);
}

// POST /api/calib/swap_ew  (no body)
//
// Swaps which panel is FACE_EAST vs FACE_WEST in the saved panel map.
// Useful when face-ID calibration looked correct but the edge-match test
// shows E and W on the wrong physical sides (a 180° mislabel that's easy
// to make during a 6-button calibration).
static esp_err_t h_calib_swap_ew(httpd_req_t *req) {
    (void)req;
    config_swap_east_west();
    effects_notify_effect_changed();
    return json_ok(req);
}

static esp_err_t h_calib_reset(httpd_req_t *req) {
    config_lock();
    app_config_t *c = config_get();
    for (int i = 0; i < CUBE_FACE_COUNT; i++) {
        c->calib.panel_map[i]    = (cube_face_t)i;
        c->calib.panel_rot[i]    = 0;
        c->calib.panel_mirror[i] = 0;
    }
    c->calibrated = false;
    c->calib_step = 0;
    c->effect = EFFECT_CALIB_FACE;
    cube_set_calibration(&c->calib);
    config_unlock();
    config_save();
    effects_notify_effect_changed();
    return json_ok(req);
}

// --- registration ----------------------------------------------------------

static const httpd_uri_t s_uris[] = {
    { .uri="/",             .method=HTTP_GET,  .handler=h_index    },
    { .uri="/index.html",   .method=HTTP_GET,  .handler=h_index    },
    { .uri="/app.js",       .method=HTTP_GET,  .handler=h_appjs    },
    { .uri="/style.css",    .method=HTTP_GET,  .handler=h_stylecss },
    { .uri="/api/state",    .method=HTTP_GET,  .handler=h_state    },
    { .uri="/api/effect",   .method=HTTP_POST, .handler=h_effect   },
    { .uri="/api/brightness",.method=HTTP_POST,.handler=h_brightness },
    { .uri="/api/solid",    .method=HTTP_POST, .handler=h_solid    },
    { .uri="/api/breakout", .method=HTTP_POST, .handler=h_breakout },
    { .uri="/api/calib/step",.method=HTTP_POST,.handler=h_calib_step },
    { .uri="/api/calib/face",.method=HTTP_POST,.handler=h_calib_face },
    { .uri="/api/calib/rot",.method=HTTP_POST, .handler=h_calib_rot },
    { .uri="/api/calib/mirror",.method=HTTP_POST,.handler=h_calib_mirror },
    { .uri="/api/calib/done",.method=HTTP_POST,.handler=h_calib_done },
    { .uri="/api/calib/reset",.method=HTTP_POST,.handler=h_calib_reset },
    { .uri="/api/calib/swap_ew",.method=HTTP_POST,.handler=h_calib_swap_ew },
    { .uri="/api/startup",    .method=HTTP_POST, .handler=h_startup },
    { .uri="/api/random",     .method=HTTP_POST, .handler=h_random },
    { .uri="/api/orientation",.method=HTTP_POST, .handler=h_orientation },
    { .uri="/api/ota",        .method=HTTP_POST, .handler=ota_http_handler },
    // Captive portal probe endpoints — redirect to /.
    { .uri="/generate_204",      .method=HTTP_GET, .handler=h_captive_redirect },
    { .uri="/gen_204",           .method=HTTP_GET, .handler=h_captive_redirect },
    { .uri="/hotspot-detect.html",.method=HTTP_GET,.handler=h_captive_redirect },
    { .uri="/library/test/success.html",.method=HTTP_GET,.handler=h_captive_redirect },
    { .uri="/ncsi.txt",          .method=HTTP_GET, .handler=h_captive_redirect },
    { .uri="/connecttest.txt",   .method=HTTP_GET, .handler=h_captive_redirect },
};

// Everything else 404s with a redirect body so captive portal "is internet
// reachable?" checks that hit unknown paths still bounce to our UI.
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return h_captive_redirect(req);
}

void http_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = sizeof(s_uris) / sizeof(s_uris[0]) + 4;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // OTA uploads ~1 MB over WiFi; bump receive timeouts and stack so a
    // slow connection doesn't abort the transfer mid-stream.
    cfg.recv_wait_timeout  = 15;
    cfg.send_wait_timeout  = 15;
    cfg.stack_size         = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        httpd_register_uri_handler(s_server, &s_uris[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404);

    ESP_LOGI(TAG, "HTTP server up on :80");
}
