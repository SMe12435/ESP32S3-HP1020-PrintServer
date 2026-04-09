#include "cloud_client.h"
#include "usb_printer.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "cloud";

static struct {
    esp_websocket_client_handle_t ws;
    char server_url[256];
    char api_key[128];
    cloud_job_cb_t on_new_job;
    void *cb_ctx;
    bool connected;
} s_cloud;

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WebSocket connected");
        s_cloud.connected = true;
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf),
                 "{\"type\":\"printer_status\",\"state\":%d,\"wifi\":true}",
                 (int)usb_printer_get_state());
        esp_websocket_client_send_text(s_cloud.ws, status_buf, strlen(status_buf),
                                        pdMS_TO_TICKS(2000));
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_cloud.connected = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 0x01 && ev->data_len > 0) {
            char *json_str = strndup((const char *)ev->data_ptr, ev->data_len);
            if (!json_str) break;

            cJSON *root = cJSON_Parse(json_str);
            free(json_str);
            if (!root) break;

            cJSON *type = cJSON_GetObjectItem(root, "type");
            if (type && cJSON_IsString(type) && strcmp(type->valuestring, "new_job") == 0) {
                cloud_job_t job = {};
                cJSON *jid = cJSON_GetObjectItem(root, "job_id");
                cJSON *pages = cJSON_GetObjectItem(root, "pages");
                if (jid && cJSON_IsString(jid)) {
                    strncpy(job.job_id, jid->valuestring, CLOUD_MAX_JOB_ID_LEN - 1);
                }
                if (pages && cJSON_IsNumber(pages)) {
                    job.total_pages = (uint32_t)pages->valuedouble;
                }
                ESP_LOGI(TAG, "new job: %s (%lu pages)",
                         job.job_id, (unsigned long)job.total_pages);
                if (s_cloud.on_new_job) {
                    s_cloud.on_new_job(&job, s_cloud.cb_ctx);
                }
            }
            cJSON_Delete(root);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

esp_err_t cloud_client_init(const cloud_client_config_t *cfg)
{
    if (s_cloud.ws) {
        ESP_LOGW(TAG, "cloud client already running, tearing down first");
        esp_websocket_client_stop(s_cloud.ws);
        esp_websocket_client_destroy(s_cloud.ws);
        s_cloud.ws = NULL;
        s_cloud.connected = false;
    }

    memset(&s_cloud, 0, sizeof(s_cloud));
    strncpy(s_cloud.server_url, cfg->server_url, sizeof(s_cloud.server_url) - 1);
    strncpy(s_cloud.api_key, cfg->api_key, sizeof(s_cloud.api_key) - 1);
    s_cloud.on_new_job = cfg->on_new_job;
    s_cloud.cb_ctx = cfg->cb_ctx;

    char ws_url[320];
    snprintf(ws_url, sizeof(ws_url), "%s/ws/printer?key=%s",
             cfg->server_url, cfg->api_key);

    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 4096,
    };

    s_cloud.ws = esp_websocket_client_init(&ws_cfg);
    if (!s_cloud.ws) {
        ESP_LOGE(TAG, "ws init failed");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_cloud.ws, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_cloud.ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "cloud client started -> %s", cfg->server_url);
    return ESP_OK;
}

esp_err_t cloud_client_download_page(const char *job_id, int page_num,
                                      uint8_t **out_buf, size_t *out_len)
{
    char url[384];
    snprintf(url, sizeof(url), "%s/api/jobs/%s/page/%d.zjs",
             s_cloud.server_url, job_id, page_num);

    /* Replace wss:// with https:// for HTTP requests */
    if (strncmp(url, "wss://", 6) == 0) {
        memmove(url + 8, url + 6, strlen(url + 6) + 1);
        memcpy(url, "https://", 8);
    } else if (strncmp(url, "ws://", 5) == 0) {
        memmove(url + 7, url + 5, strlen(url + 5) + 1);
        memcpy(url, "http://", 7);
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 8192,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_FAIL;

    char auth_hdr[160];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_cloud.api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) content_length = 5 * 1024 * 1024;

    uint8_t *buf = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "failed to alloc %d bytes for page", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read = esp_http_client_read(client, (char *)buf + total_read,
                                         content_length - total_read);
        if (read <= 0) break;
        total_read += read;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 && status != 0) {
        free(buf);
        ESP_LOGE(TAG, "page download HTTP %d", status);
        return ESP_FAIL;
    }

    if (content_length != 5 * 1024 * 1024 && total_read != content_length) {
        ESP_LOGW(TAG, "page %d incomplete: got %d of %d bytes",
                 page_num, total_read, content_length);
        free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    *out_len = total_read;
    ESP_LOGI(TAG, "downloaded page %d (%d bytes)", page_num, total_read);
    return ESP_OK;
}

esp_err_t cloud_client_report_status(const char *job_id, const char *status,
                                      int page_done, int page_total)
{
    if (!s_cloud.connected) return ESP_ERR_INVALID_STATE;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddStringToObject(root, "job_id", job_id);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "page", page_done);
    cJSON_AddNumberToObject(root, "of", page_total);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        esp_websocket_client_send_text(s_cloud.ws, json, strlen(json), pdMS_TO_TICKS(2000));
        free(json);
    }
    return ESP_OK;
}

esp_err_t cloud_client_complete_job(const char *job_id)
{
    return cloud_client_report_status(job_id, "completed", 0, 0);
}

void cloud_client_send_printer_status(const char *status_json)
{
    if (s_cloud.connected && s_cloud.ws) {
        esp_websocket_client_send_text(s_cloud.ws, status_json, strlen(status_json),
                                        pdMS_TO_TICKS(1000));
    }
}

void cloud_client_deinit(void)
{
    if (s_cloud.ws) {
        esp_websocket_client_stop(s_cloud.ws);
        esp_websocket_client_destroy(s_cloud.ws);
        s_cloud.ws = NULL;
    }
}
