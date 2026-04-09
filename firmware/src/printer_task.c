#include "printer_task.h"
#include "cloud_client.h"
#include "usb_printer.h"
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

static QueueHandle_t s_job_queue;
static TaskHandle_t  s_task_hdl;

static void process_job(const cloud_job_t *job)
{
    ESP_LOGI(TAG, "processing job %s (%lu pages)", job->job_id, (unsigned long)job->total_pages);

    if (usb_printer_get_state() < PRINTER_STATE_READY) {
        ESP_LOGE(TAG, "printer not ready");
        cloud_client_report_status(job->job_id, "error", 0, job->total_pages);
        return;
    }

    uint32_t failed_pages = 0;

    for (uint32_t pg = 0; pg < job->total_pages; pg++) {
        cloud_client_report_status(job->job_id, "printing", pg + 1, job->total_pages);

        uint8_t *zjs_data = NULL;
        size_t zjs_len = 0;
        esp_err_t err = cloud_client_download_page(job->job_id, pg, &zjs_data, &zjs_len);
        if (err != ESP_OK || !zjs_data || zjs_len == 0) {
            ESP_LOGE(TAG, "page %lu download failed", (unsigned long)pg);
            if (zjs_data) free(zjs_data);
            failed_pages++;
            continue;
        }

        ESP_LOGI(TAG, "sending page %lu (%d bytes) to printer",
                 (unsigned long)(pg + 1), (int)zjs_len);

        err = usb_printer_send_data(zjs_data, zjs_len, 30000);
        free(zjs_data);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB send failed for page %lu: %s",
                     (unsigned long)(pg + 1), esp_err_to_name(err));
            cloud_client_report_status(job->job_id, "error", pg + 1, job->total_pages);
            return;
        }

        ESP_LOGI(TAG, "page %lu done", (unsigned long)(pg + 1));
    }

    if (failed_pages > 0) {
        ESP_LOGE(TAG, "job %s finished with %lu failed pages",
                 job->job_id, (unsigned long)failed_pages);
        cloud_client_report_status(job->job_id, "error",
                                   job->total_pages - failed_pages, job->total_pages);
    } else {
        cloud_client_complete_job(job->job_id);
        ESP_LOGI(TAG, "job %s completed", job->job_id);
    }
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
