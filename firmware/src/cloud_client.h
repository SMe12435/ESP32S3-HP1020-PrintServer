#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define CLOUD_MAX_JOB_ID_LEN 64

typedef struct {
    char job_id[CLOUD_MAX_JOB_ID_LEN];
    uint32_t total_pages;
} cloud_job_t;

typedef void (*cloud_job_cb_t)(const cloud_job_t *job, void *ctx);

typedef struct {
    const char *server_url;     // e.g. "wss://print.example.com"
    const char *api_key;
    cloud_job_cb_t on_new_job;
    void *cb_ctx;
} cloud_client_config_t;

esp_err_t cloud_client_init(const cloud_client_config_t *cfg);
esp_err_t cloud_client_download_page(const char *job_id, int page_num,
                                      uint8_t **out_buf, size_t *out_len);
esp_err_t cloud_client_report_status(const char *job_id, const char *status,
                                      int page_done, int page_total);
esp_err_t cloud_client_complete_job(const char *job_id);
void cloud_client_send_printer_status(const char *status_json);
void cloud_client_deinit(void);
