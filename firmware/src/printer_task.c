#include "printer_task.h"
#include "cloud_client.h"
#include "usb_printer.h"
#include "zjstream.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "print_task";

#define PRINT_QUEUE_LEN   4
#define PRINT_TASK_STACK   16384
#define PRINT_TASK_PRIO    5
#define BAND_LINES         128

static QueueHandle_t s_job_queue;
static TaskHandle_t  s_task_hdl;

/* Callback: ZjStream outputs chunks -> forward to USB printer */
static void zjs_output_cb(const uint8_t *data, size_t len, void *user)
{
    esp_err_t err = usb_printer_send_data(data, len, 10000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB send failed: %s", esp_err_to_name(err));
    }
}

/**
 * Parse a PBM P4 binary header and return pointer to pixel data.
 * Format: "P4\n<width> <height>\n<binary data>"
 */
static bool parse_pbm_header(const uint8_t *data, size_t len,
                              uint32_t *w, uint32_t *h, const uint8_t **pixels, size_t *pix_len)
{
    if (len < 4 || data[0] != 'P' || data[1] != '4') return false;

    const char *p = (const char *)data + 2;
    const char *end = (const char *)data + len;

    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    /* skip comments */
    while (p < end && *p == '#') {
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }

    *w = strtoul(p, (char **)&p, 10);
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
    *h = strtoul(p, (char **)&p, 10);

    if (p < end && *p == '\n') p++;

    *pixels = (const uint8_t *)p;
    *pix_len = len - (size_t)((const uint8_t *)p - data);
    return (*w > 0 && *h > 0);
}

static void process_job(const cloud_job_t *job)
{
    ESP_LOGI(TAG, "processing job %s (%lu pages)", job->job_id, (unsigned long)job->total_pages);

    if (usb_printer_get_state() < PRINTER_STATE_READY) {
        ESP_LOGE(TAG, "printer not ready");
        cloud_client_report_status(job->job_id, "error", 0, job->total_pages);
        return;
    }

    zjstream_config_t zcfg = {
        .dpi = 600,
        .paper = 1,
        .copies = 1,
        .cb = zjs_output_cb,
        .user = NULL,
    };

    zjstream_t *zjs = zjstream_create(&zcfg);
    if (!zjs) { cloud_client_report_status(job->job_id, "error", 0, 0); return; }

    zjstream_start_doc(zjs);

    for (uint32_t pg = 0; pg < job->total_pages; pg++) {
        cloud_client_report_status(job->job_id, "printing", pg + 1, job->total_pages);

        uint8_t *pbm_data = NULL;
        size_t pbm_len = 0;
        esp_err_t err = cloud_client_download_page(job->job_id, pg, &pbm_data, &pbm_len);
        if (err != ESP_OK || !pbm_data) {
            ESP_LOGE(TAG, "page %lu download failed", (unsigned long)pg);
            continue;
        }

        uint32_t pw, ph;
        const uint8_t *pixels;
        size_t pix_len;
        if (!parse_pbm_header(pbm_data, pbm_len, &pw, &ph, &pixels, &pix_len)) {
            ESP_LOGE(TAG, "invalid PBM data for page %lu", (unsigned long)pg);
            free(pbm_data);
            continue;
        }

        zjstream_start_page(zjs, pw, ph);

        uint32_t bpl = (pw + 7) / 8;
        uint32_t y = 0;
        while (y < ph) {
            uint32_t lines = BAND_LINES;
            if (y + lines > ph) lines = ph - y;

            const uint8_t *band = pixels + y * bpl;
            zjstream_write_band(zjs, band, lines);
            y += lines;
        }

        zjstream_end_page(zjs);
        free(pbm_data);

        ESP_LOGI(TAG, "page %lu done", (unsigned long)(pg + 1));
    }

    zjstream_end_doc(zjs);
    zjstream_destroy(zjs);

    cloud_client_complete_job(job->job_id);
    ESP_LOGI(TAG, "job %s completed", job->job_id);
}

static void printer_task(void *arg)
{
    cloud_job_t job;
    while (1) {
        if (xQueueReceive(s_job_queue, &job, portMAX_DELAY) == pdTRUE) {
            process_job(&job);
        }
    }
}

esp_err_t printer_task_init(void)
{
    s_job_queue = xQueueCreate(PRINT_QUEUE_LEN, sizeof(cloud_job_t));
    if (!s_job_queue) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreatePinnedToCore(printer_task, "printer", PRINT_TASK_STACK,
                                              NULL, PRINT_TASK_PRIO, &s_task_hdl, 1);
    return (ret == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t printer_task_enqueue_job(const cloud_job_t *job)
{
    if (xQueueSend(s_job_queue, job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "job queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
