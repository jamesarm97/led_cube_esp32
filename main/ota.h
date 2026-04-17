#pragma once
// OTA firmware update over HTTP.
//
// The handler accepts a raw `.bin` firmware image as the POST body and
// streams it directly into the inactive OTA slot via the esp_ota_ops
// API. On success it marks that slot as the new boot partition, sends
// `{"ok":true,"reboot":true}` to the client, and calls esp_restart().
// On any failure (begin / write / end / set_boot / recv) the partial
// write is aborted via esp_ota_abort() so the currently-running
// firmware keeps booting.

#include "esp_http_server.h"

esp_err_t ota_http_handler(httpd_req_t *req);
