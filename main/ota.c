#include "ota.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define CHUNK_SIZE  1536   // per-recv buffer; traded for stack footprint

static esp_err_t reply_err(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, buf);
    return ESP_FAIL;
}

esp_err_t ota_http_handler(httpd_req_t *req) {
    int expected = req->content_len;
    ESP_LOGI(TAG, "upload starting: %d bytes", expected);
    if (expected <= 0) return reply_err(req, "400 Bad Request", "empty body");

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no OTA partition available (is the partition table OTA-capable?)");
        return reply_err(req, "500 Internal Server Error", "no ota partition");
    }
    ESP_LOGI(TAG, "writing to %s @ 0x%08lx (size 0x%lx)",
             target->label, (unsigned long)target->address, (unsigned long)target->size);
    if ((uint32_t)expected > target->size) {
        return reply_err(req, "400 Bad Request", "image too large for partition");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return reply_err(req, "500 Internal Server Error", "begin failed");
    }

    // Stream the body directly into flash.
    static char buf[CHUNK_SIZE];
    int total = 0;
    int last_log_pct = -1;
    while (total < expected) {
        int r = httpd_req_recv(req, buf, sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed at %d/%d: %d", total, expected, r);
            esp_ota_abort(handle);
            return reply_err(req, "400 Bad Request", "upload interrupted");
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return reply_err(req, "500 Internal Server Error", "write failed");
        }
        total += r;
        int pct = (total * 100) / expected;
        if (pct != last_log_pct && pct % 10 == 0) {
            ESP_LOGI(TAG, "progress: %d%% (%d/%d)", pct, total, expected);
            last_log_pct = pct;
        }
    }

    // esp_ota_end() validates the image magic + checksum.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
            return reply_err(req, "400 Bad Request", "image validate failed (not a valid firmware .bin?)");
        return reply_err(req, "500 Internal Server Error", "end failed");
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return reply_err(req, "500 Internal Server Error", "set_boot failed");
    }

    ESP_LOGI(TAG, "OTA success, rebooting");

    // Acknowledge before restart so the client's XHR resolves.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");

    // Small delay to let the response bytes actually leave the socket,
    // then restart into the freshly-written firmware.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; // unreachable
}
