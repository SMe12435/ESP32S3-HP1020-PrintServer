#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "wifi_manager.h"
#include "cloud_client.h"
#include "printer_task.h"
#include "usb_printer.h"

static const char *TAG = "main";

/* TODO: move to NVS configuration / captive portal setup */
#define WIFI_SSID       CONFIG_WIFI_SSID
#define WIFI_PASS       CONFIG_WIFI_PASS
#define CLOUD_URL       CONFIG_CLOUD_URL
#define CLOUD_API_KEY   CONFIG_CLOUD_API_KEY

static void load_printer_firmware(void)
{
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/data",
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }

    FILE *f = fopen("/data/sihp1020.dl", "rb");
    if (!f) {
        ESP_LOGW(TAG, "sihp1020.dl not found in SPIFFS — printer will need firmware");
        esp_vfs_spiffs_unregister("storage");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fw_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *fw_buf = malloc(fw_size);
    if (!fw_buf) {
        fclose(f);
        esp_vfs_spiffs_unregister("storage");
        return;
    }

    fread(fw_buf, 1, fw_size, f);
    fclose(f);
    esp_vfs_spiffs_unregister("storage");

    ESP_LOGI(TAG, "loaded firmware %ld bytes", fw_size);

    for (int i = 0; i < 30; i++) {
        if (usb_printer_get_state() >= PRINTER_STATE_CONNECTED) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (usb_printer_get_state() >= PRINTER_STATE_CONNECTED) {
        usb_printer_upload_firmware(fw_buf, fw_size);
    } else {
        ESP_LOGW(TAG, "printer not connected, skipping firmware upload");
    }
    free(fw_buf);
}

static void on_printer_state(printer_state_t state, void *ctx)
{
    const char *names[] = {
        "DISCONNECTED", "CONNECTED", "FW_UPLOADING", "READY", "PRINTING", "ERROR"
    };
    ESP_LOGI(TAG, "printer: %s", names[state]);
}

static void on_new_job(const cloud_job_t *job, void *ctx)
{
    printer_task_enqueue_job(job);
}

static void on_wifi(bool connected, void *ctx)
{
    if (connected) {
        ESP_LOGI(TAG, "WiFi connected, starting cloud client");
        cloud_client_config_t ccfg = {
            .server_url = CLOUD_URL,
            .api_key = CLOUD_API_KEY,
            .on_new_job = on_new_job,
            .cb_ctx = NULL,
        };
        cloud_client_init(&ccfg);
    } else {
        ESP_LOGW(TAG, "WiFi disconnected, tearing down cloud client");
        cloud_client_deinit();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== HP 1020 Wireless Print Server ===");

    /* 1. Start USB Host + printer driver */
    usb_printer_config_t pcfg = {
        .on_state_change = on_printer_state,
        .cb_ctx = NULL,
    };
    ESP_ERROR_CHECK(usb_printer_init(&pcfg));

    /* 2. Start print queue task */
    ESP_ERROR_CHECK(printer_task_init());

    /* 3. Load and upload printer firmware from SPIFFS */
    load_printer_firmware();

    /* 4. Start WiFi (cloud client starts after connection) */
    wifi_manager_config_t wcfg = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASS,
        .on_connect = on_wifi,
        .cb_ctx = NULL,
    };
    wifi_manager_init(&wcfg);

    /* Main loop: periodic status reporting */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        printer_state_t ps = usb_printer_get_state();
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf),
                 "{\"type\":\"printer_status\",\"state\":%d,\"wifi\":%s}",
                 ps, wifi_manager_is_connected() ? "true" : "false");
        cloud_client_send_printer_status(status_buf);
    }
}
