#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define HP1020_VID 0x03f0
#define HP1020_PID 0x2b17

typedef enum {
    PRINTER_STATE_DISCONNECTED,
    PRINTER_STATE_CONNECTED,
    PRINTER_STATE_FW_UPLOADING,
    PRINTER_STATE_READY,
    PRINTER_STATE_PRINTING,
    PRINTER_STATE_ERROR
} printer_state_t;

typedef void (*printer_state_cb_t)(printer_state_t state, void *ctx);

typedef struct {
    printer_state_cb_t on_state_change;
    void *cb_ctx;
} usb_printer_config_t;

esp_err_t usb_printer_init(const usb_printer_config_t *config);
printer_state_t usb_printer_get_state(void);
esp_err_t usb_printer_upload_firmware(const uint8_t *fw_data, size_t fw_len);
esp_err_t usb_printer_send_data(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t usb_printer_get_device_id(char *buf, size_t buf_len);
void usb_printer_deinit(void);
