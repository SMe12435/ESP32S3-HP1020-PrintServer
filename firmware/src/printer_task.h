#pragma once

#include "esp_err.h"
#include "cloud_client.h"

esp_err_t printer_task_init(void);
esp_err_t printer_task_enqueue_job(const cloud_job_t *job);
