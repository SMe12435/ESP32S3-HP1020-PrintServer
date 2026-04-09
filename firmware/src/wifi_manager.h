#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef void (*wifi_connected_cb_t)(bool connected, void *ctx);

typedef struct {
    const char *ssid;
    const char *password;
    wifi_connected_cb_t on_connect;
    void *cb_ctx;
} wifi_manager_config_t;

esp_err_t wifi_manager_init(const wifi_manager_config_t *cfg);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_start_ap(const char *ap_ssid, const char *ap_pass);
void wifi_manager_deinit(void);
