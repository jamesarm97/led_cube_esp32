#include "net.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "net";

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    switch (id) {
        case WIFI_EVENT_AP_START:      ESP_LOGI(TAG, "AP started"); break;
        case WIFI_EVENT_AP_STOP:       ESP_LOGI(TAG, "AP stopped"); break;
        case WIFI_EVENT_AP_STACONNECTED:    ESP_LOGI(TAG, "station connected");    break;
        case WIFI_EVENT_AP_STADISCONNECTED: ESP_LOGI(TAG, "station disconnected"); break;
        default: break;
    }
}

void net_start_ap(void) {
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap = {0};
    config_lock();
    app_config_t *c = config_get();
    strlcpy((char*)ap.ap.ssid, c->ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(c->ap_ssid);
    ap.ap.channel  = c->ap_channel ? c->ap_channel : 6;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    if (c->ap_pass[0]) {
        strlcpy((char*)ap.ap.password, c->ap_pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    config_unlock();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP up: ssid=%s chan=%d auth=%s",
             ap.ap.ssid, ap.ap.channel, ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");
}
