#include "cloud_client.h"
#include "usb_printer.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "cloud";

#define POLL_INTERVAL_MS  10000
#define POLL_TASK_STACK   8192
#define POLL_TASK_PRIO    4
#define HTTP_BUF_SIZE     2048

static struct {
    char server_url[256];
    char api_key[128];
    cloud_job_cb_t on_new_job;
    void *cb_ctx;
    TaskHandle_t poll_task;
    bool running;
} s_cloud;


static esp_err_t http_post_json(const char *path, const char *json_body)
{
    char url[384];
    snprintf(url, sizeof(url), "%s%s", s_cloud.server_url, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    char auth_hdr[160];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_cloud.api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST %s failed: %s", path, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "POST %s returned %d", path, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}


static void send_printer_status(void)
{
    char body[128];
    snprintf(body, sizeof(body),
             "{\"type\":\"printer_status\",\"state\":%d,\"wifi\":%s}",
             (int)usb_printer_get_state(),
             wifi_manager_is_connected() ? "true" : "false");
    http_post_json("/api/device/status", body);
}


static void poll_for_jobs(void)
{
    char url[384];
    snprintf(url, sizeof(url), "%s/api/poll", s_cloud.server_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = HTTP_BUF_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;

    char auth_hdr[160];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_cloud.api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) content_length = HTTP_BUF_SIZE;
    if (content_length > HTTP_BUF_SIZE) content_length = HTTP_BUF_SIZE;

    char *buf = malloc(content_length + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int rd = esp_http_client_read(client, buf + total_read, content_length - total_read);
        if (rd <= 0) break;
        total_read += rd;
    }
    buf[total_read] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 || total_read == 0) {
        free(buf);
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *jobs = cJSON_GetObjectItem(root, "jobs");
    if (jobs && cJSON_IsArray(jobs)) {
        int count = cJSON_GetArraySize(jobs);
        for (int i = 0; i < count; i++) {
            cJSON *entry = cJSON_GetArrayItem(jobs, i);
            if (!entry) continue;

            cJSON *jid = cJSON_GetObjectItem(entry, "job_id");
            cJSON *pages = cJSON_GetObjectItem(entry, "pages");

            cloud_job_t job = {};
            if (jid && cJSON_IsString(jid)) {
                strncpy(job.job_id, jid->valuestring, CLOUD_MAX_JOB_ID_LEN - 1);
            }
            if (pages && cJSON_IsNumber(pages)) {
                job.total_pages = (uint32_t)pages->valuedouble;
            }

            ESP_LOGI(TAG, "polled job: %s (%lu pages)",
                     job.job_id, (unsigned long)job.total_pages);

            if (s_cloud.on_new_job) {
                s_cloud.on_new_job(&job, s_cloud.cb_ctx);
            }
        }
    }

    cJSON_Delete(root);
}


static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "poll task started (every %d ms)", POLL_INTERVAL_MS);

    while (s_cloud.running) {
        if (wifi_manager_is_connected()) {
            send_printer_status();
            poll_for_jobs();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "poll task stopped");
    vTaskDelete(NULL);
}


esp_err_t cloud_client_init(const cloud_client_config_t *cfg)
{
    if (s_cloud.poll_task) {
        ESP_LOGW(TAG, "cloud client already running, stopping first");
        cloud_client_deinit();
    }

    memset(&s_cloud, 0, sizeof(s_cloud));
    strncpy(s_cloud.server_url, cfg->server_url, sizeof(s_cloud.server_url) - 1);
    strncpy(s_cloud.api_key, cfg->api_key, sizeof(s_cloud.api_key) - 1);
    s_cloud.on_new_job = cfg->on_new_job;
    s_cloud.cb_ctx = cfg->cb_ctx;
    s_cloud.running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        poll_task, "cloud_poll", POLL_TASK_STACK,
        NULL, POLL_TASK_PRIO, &s_cloud.poll_task, 0);

    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "failed to create poll task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "cloud client started -> %s (HTTP polling)", cfg->server_url);
    return ESP_OK;
}


esp_err_t cloud_client_download_page(const char *job_id, int page_num,
                                      uint8_t **out_buf, size_t *out_len)
{
    char url[384];
    snprintf(url, sizeof(url), "%s/api/jobs/%s/page/%d.zjs",
             s_cloud.server_url, job_id, page_num);

    esp_http_client_config_t http_cfg = {
        .url = url,
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
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "job_status");
    cJSON_AddStringToObject(root, "job_id", job_id);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "page", page_done);
    cJSON_AddNumberToObject(root, "of", page_total);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    esp_err_t err = ESP_FAIL;
    if (json) {
        err = http_post_json("/api/device/status", json);
        free(json);
    }
    return err;
}


esp_err_t cloud_client_complete_job(const char *job_id)
{
    return cloud_client_report_status(job_id, "completed", 0, 0);
}


void cloud_client_deinit(void)
{
    s_cloud.running = false;
    if (s_cloud.poll_task) {
        vTaskDelay(pdMS_TO_TICKS(500));
        s_cloud.poll_task = NULL;
    }
}
