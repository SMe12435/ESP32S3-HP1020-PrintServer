#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const uint32_t BACKOFF_MS[] = { 1000, 2000, 5000, 10000, 30000, 60000 };
#define BACKOFF_STEPS (sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]))

static EventGroupHandle_t s_wifi_events;
static int s_retry_count = 0;
static bool s_connected = false;
static wifi_connected_cb_t s_on_connect = NULL;
static void *s_cb_ctx = NULL;
static esp_netif_t *s_sta_netif = NULL;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_on_connect) s_on_connect(false, s_cb_ctx);

        uint32_t idx = s_retry_count < (int)BACKOFF_STEPS ? s_retry_count : BACKOFF_STEPS - 1;
        uint32_t delay = BACKOFF_MS[idx];
        s_retry_count++;
        ESP_LOGW(TAG, "disconnected, retry #%d in %lu ms", s_retry_count, (unsigned long)delay);
        vTaskDelay(pdMS_TO_TICKS(delay));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_on_connect) s_on_connect(true, s_cb_ctx);
    }
}

esp_err_t wifi_manager_init(const wifi_manager_config_t *cfg)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_wifi_events = xEventGroupCreate();
    s_on_connect = cfg->on_connect;
    s_cb_ctx = cfg->cb_ctx;

    err = esp_netif_init();
    if (err != ESP_OK) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    esp_event_handler_instance_t h_wifi, h_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         &event_handler, NULL, &h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         &event_handler, NULL, &h_ip);

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, cfg->ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, cfg->password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "connecting to %s", cfg->ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                            WIFI_CONNECTED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "initial STA connect timed out, retries continue in background");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_manager_start_ap(const char *ap_ssid, const char *ap_pass)
{
    esp_wifi_stop();

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .channel = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP started: %s", ap_ssid);
    return ESP_OK;
}

void wifi_manager_deinit(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_wifi_events) vEventGroupDelete(s_wifi_events);
}
