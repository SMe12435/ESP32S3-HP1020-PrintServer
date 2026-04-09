#include "usb_printer.h"
#include "usb/usb_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "usb_printer";

#define USB_PRINTER_CLASS       0x07
#define USB_PRINTER_SUBCLASS    0x01
#define USB_PRINTER_PROTO_BIDIR 0x02

#define XFER_BUF_SIZE           4096
#define USB_TASK_STACK_SIZE     4096
#define DAEMON_TASK_STACK_SIZE  4096
#define DAEMON_TASK_PRIORITY    2
#define CLIENT_TASK_PRIORITY    3

#define EVT_DEVICE_CONNECTED    BIT0
#define EVT_DEVICE_GONE         BIT1
#define EVT_TRANSFER_DONE       BIT2

typedef struct {
    printer_state_t state;
    printer_state_cb_t on_state_change;
    void *cb_ctx;

    usb_host_client_handle_t client_hdl;
    usb_device_handle_t dev_hdl;
    uint8_t dev_addr;
    uint8_t intf_num;
    uint8_t bulk_out_ep;
    uint8_t bulk_in_ep;
    uint16_t bulk_out_mps;

    usb_transfer_t *xfer_out;
    usb_transfer_t *xfer_in;
    usb_transfer_t *xfer_ctrl;

    SemaphoreHandle_t xfer_done_sem;
    esp_err_t xfer_result;

    EventGroupHandle_t events;
    TaskHandle_t daemon_task;
    TaskHandle_t client_task;
    bool running;
} usb_printer_ctx_t;

static usb_printer_ctx_t *s_ctx = NULL;

static void set_state(printer_state_t new_state)
{
    if (s_ctx->state == new_state) return;
    s_ctx->state = new_state;
    ESP_LOGI(TAG, "state -> %d", new_state);
    if (s_ctx->on_state_change) {
        s_ctx->on_state_change(new_state, s_ctx->cb_ctx);
    }
}

static void xfer_out_cb(usb_transfer_t *transfer)
{
    usb_printer_ctx_t *ctx = (usb_printer_ctx_t *)transfer->context;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ctx->xfer_result = ESP_OK;
    } else {
        ESP_LOGE(TAG, "OUT transfer failed: status=%d", transfer->status);
        ctx->xfer_result = ESP_FAIL;
    }
    xSemaphoreGive(ctx->xfer_done_sem);
}

static void xfer_ctrl_cb(usb_transfer_t *transfer)
{
    usb_printer_ctx_t *ctx = (usb_printer_ctx_t *)transfer->context;
    ctx->xfer_result = (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
                       ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(ctx->xfer_done_sem);
}

static bool find_printer_interface(const usb_config_desc_t *config_desc,
                                   uint8_t *intf_num, uint8_t *ep_out, uint8_t *ep_in,
                                   uint16_t *mps_out)
{
    int offset = 0;
    const usb_intf_desc_t *intf = NULL;

    while (1) {
        intf = usb_parse_interface_descriptor(config_desc, offset, 0, &offset);
        if (intf == NULL) break;

        if (intf->bInterfaceClass == USB_PRINTER_CLASS &&
            intf->bInterfaceSubClass == USB_PRINTER_SUBCLASS &&
            (intf->bInterfaceProtocol == USB_PRINTER_PROTO_BIDIR ||
             intf->bInterfaceProtocol == 0x01)) {

            *intf_num = intf->bInterfaceNumber;
            *ep_out = 0;
            *ep_in = 0;

            int ep_offset = offset;
            for (int i = 0; i < intf->bNumEndpoints; i++) {
                const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(
                    intf, i, config_desc->wTotalLength, &ep_offset);
                if (ep == NULL) continue;

                if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                    USB_BM_ATTRIBUTES_XFER_BULK) {
                    if (ep->bEndpointAddress & 0x80) {
                        *ep_in = ep->bEndpointAddress;
                    } else {
                        *ep_out = ep->bEndpointAddress;
                        *mps_out = ep->wMaxPacketSize;
                    }
                }
            }

            if (*ep_out != 0) {
                ESP_LOGI(TAG, "found printer intf=%d ep_out=0x%02x ep_in=0x%02x mps=%d",
                         *intf_num, *ep_out, *ep_in, *mps_out);
                return true;
            }
        }
    }
    return false;
}

static void handle_new_device(uint8_t dev_addr)
{
    esp_err_t err;

    err = usb_host_device_open(s_ctx->client_hdl, dev_addr, &s_ctx->dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open failed: %s", esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dev_desc;
    err = usb_host_get_device_descriptor(s_ctx->dev_hdl, &dev_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_device_descriptor failed");
        usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
        return;
    }

    ESP_LOGI(TAG, "device VID=0x%04x PID=0x%04x", dev_desc->idVendor, dev_desc->idProduct);

    if (dev_desc->idVendor != HP1020_VID || dev_desc->idProduct != HP1020_PID) {
        ESP_LOGW(TAG, "not HP 1020, ignoring");
        usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
        s_ctx->dev_hdl = NULL;
        return;
    }

    const usb_config_desc_t *config_desc;
    err = usb_host_get_active_config_descriptor(s_ctx->dev_hdl, &config_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_config_descriptor failed");
        usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
        return;
    }

    if (!find_printer_interface(config_desc,
                                &s_ctx->intf_num, &s_ctx->bulk_out_ep,
                                &s_ctx->bulk_in_ep, &s_ctx->bulk_out_mps)) {
        ESP_LOGE(TAG, "no printer interface found");
        usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
        return;
    }

    err = usb_host_interface_claim(s_ctx->client_hdl, s_ctx->dev_hdl,
                                   s_ctx->intf_num, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
        return;
    }

    err = usb_host_transfer_alloc(XFER_BUF_SIZE, 0, &s_ctx->xfer_out);
    if (err != ESP_OK) goto alloc_fail;

    s_ctx->xfer_out->device_handle = s_ctx->dev_hdl;
    s_ctx->xfer_out->bEndpointAddress = s_ctx->bulk_out_ep;
    s_ctx->xfer_out->callback = xfer_out_cb;
    s_ctx->xfer_out->context = s_ctx;

    err = usb_host_transfer_alloc(XFER_BUF_SIZE, 0, &s_ctx->xfer_ctrl);
    if (err != ESP_OK) goto alloc_fail;
    s_ctx->xfer_ctrl->device_handle = s_ctx->dev_hdl;
    s_ctx->xfer_ctrl->callback = xfer_ctrl_cb;
    s_ctx->xfer_ctrl->context = s_ctx;

    if (s_ctx->bulk_in_ep) {
        err = usb_host_transfer_alloc(XFER_BUF_SIZE, 0, &s_ctx->xfer_in);
        if (err != ESP_OK) goto alloc_fail;
        s_ctx->xfer_in->device_handle = s_ctx->dev_hdl;
        s_ctx->xfer_in->bEndpointAddress = s_ctx->bulk_in_ep;
        s_ctx->xfer_in->context = s_ctx;
    }

    s_ctx->dev_addr = dev_addr;
    set_state(PRINTER_STATE_CONNECTED);
    return;

alloc_fail:
    ESP_LOGE(TAG, "transfer alloc failed");
    usb_host_interface_release(s_ctx->client_hdl, s_ctx->dev_hdl, s_ctx->intf_num);
    usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
}

static void handle_device_gone(void)
{
    if (s_ctx->xfer_out) { usb_host_transfer_free(s_ctx->xfer_out); s_ctx->xfer_out = NULL; }
    if (s_ctx->xfer_in)  { usb_host_transfer_free(s_ctx->xfer_in);  s_ctx->xfer_in = NULL; }
    if (s_ctx->xfer_ctrl) { usb_host_transfer_free(s_ctx->xfer_ctrl); s_ctx->xfer_ctrl = NULL; }

    if (s_ctx->dev_hdl) {
        usb_host_interface_release(s_ctx->client_hdl, s_ctx->dev_hdl, s_ctx->intf_num);
        usb_host_device_close(s_ctx->client_hdl, s_ctx->dev_hdl);
        s_ctx->dev_hdl = NULL;
    }
    set_state(PRINTER_STATE_DISCONNECTED);
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "new USB device addr=%d", event_msg->new_dev.address);
        s_ctx->dev_addr = event_msg->new_dev.address;
        xEventGroupSetBits(s_ctx->events, EVT_DEVICE_CONNECTED);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device removed");
        xEventGroupSetBits(s_ctx->events, EVT_DEVICE_GONE);
        break;
    }
}

static void usb_daemon_task(void *arg)
{
    while (s_ctx->running) {
        uint32_t event_flags;
        usb_host_lib_handle_events(200, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "no USB clients");
        }
    }
    vTaskDelete(NULL);
}

static void usb_client_task(void *arg)
{
    while (s_ctx->running) {
        EventBits_t bits = xEventGroupWaitBits(
            s_ctx->events, EVT_DEVICE_CONNECTED | EVT_DEVICE_GONE,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(500));

        if (bits & EVT_DEVICE_CONNECTED) {
            handle_new_device(s_ctx->dev_addr);
        }
        if (bits & EVT_DEVICE_GONE) {
            handle_device_gone();
        }

        usb_host_client_handle_events(s_ctx->client_hdl, 0);
    }
    vTaskDelete(NULL);
}

esp_err_t usb_printer_init(const usb_printer_config_t *config)
{
    if (s_ctx) return ESP_ERR_INVALID_STATE;

    s_ctx = calloc(1, sizeof(usb_printer_ctx_t));
    if (!s_ctx) return ESP_ERR_NO_MEM;

    s_ctx->state = PRINTER_STATE_DISCONNECTED;
    s_ctx->on_state_change = config->on_state_change;
    s_ctx->cb_ctx = config->cb_ctx;
    s_ctx->running = true;

    s_ctx->xfer_done_sem = xSemaphoreCreateBinary();
    s_ctx->events = xEventGroupCreate();

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        goto fail;
    }

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = s_ctx,
        },
    };
    err = usb_host_client_register(&client_config, &s_ctx->client_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed: %s", esp_err_to_name(err));
        usb_host_uninstall();
        goto fail;
    }

    xTaskCreatePinnedToCore(usb_daemon_task, "usb_daemon", DAEMON_TASK_STACK_SIZE,
                            NULL, DAEMON_TASK_PRIORITY, &s_ctx->daemon_task, 0);
    xTaskCreatePinnedToCore(usb_client_task, "usb_client", USB_TASK_STACK_SIZE,
                            NULL, CLIENT_TASK_PRIORITY, &s_ctx->client_task, 0);

    ESP_LOGI(TAG, "USB printer host initialized");
    return ESP_OK;

fail:
    if (s_ctx->xfer_done_sem) vSemaphoreDelete(s_ctx->xfer_done_sem);
    if (s_ctx->events) vEventGroupDelete(s_ctx->events);
    free(s_ctx);
    s_ctx = NULL;
    return err;
}

printer_state_t usb_printer_get_state(void)
{
    return s_ctx ? s_ctx->state : PRINTER_STATE_DISCONNECTED;
}

esp_err_t usb_printer_upload_firmware(const uint8_t *fw_data, size_t fw_len)
{
    if (!s_ctx || !s_ctx->dev_hdl) return ESP_ERR_INVALID_STATE;
    if (s_ctx->state < PRINTER_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;

    set_state(PRINTER_STATE_FW_UPLOADING);
    ESP_LOGI(TAG, "uploading firmware (%d bytes)", (int)fw_len);

    size_t sent = 0;
    while (sent < fw_len) {
        size_t chunk = fw_len - sent;
        if (chunk > XFER_BUF_SIZE) chunk = XFER_BUF_SIZE;

        memcpy(s_ctx->xfer_out->data_buffer, fw_data + sent, chunk);
        s_ctx->xfer_out->num_bytes = chunk;

        esp_err_t err = usb_host_transfer_submit(s_ctx->xfer_out);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fw transfer submit failed");
            set_state(PRINTER_STATE_ERROR);
            return err;
        }

        if (xSemaphoreTake(s_ctx->xfer_done_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "fw transfer timeout");
            set_state(PRINTER_STATE_ERROR);
            return ESP_ERR_TIMEOUT;
        }

        if (s_ctx->xfer_result != ESP_OK) {
            set_state(PRINTER_STATE_ERROR);
            return s_ctx->xfer_result;
        }

        sent += chunk;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "firmware uploaded");
    set_state(PRINTER_STATE_READY);
    return ESP_OK;
}

esp_err_t usb_printer_send_data(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_ctx || !s_ctx->dev_hdl || s_ctx->state < PRINTER_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > XFER_BUF_SIZE) chunk = XFER_BUF_SIZE;

        memcpy(s_ctx->xfer_out->data_buffer, data + sent, chunk);
        s_ctx->xfer_out->num_bytes = chunk;

        esp_err_t err = usb_host_transfer_submit(s_ctx->xfer_out);
        if (err != ESP_OK) return err;

        if (xSemaphoreTake(s_ctx->xfer_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }

        if (s_ctx->xfer_result != ESP_OK) return s_ctx->xfer_result;
        sent += chunk;
    }
    return ESP_OK;
}

esp_err_t usb_printer_get_device_id(char *buf, size_t buf_len)
{
    if (!s_ctx || !s_ctx->dev_hdl) return ESP_ERR_INVALID_STATE;

    usb_transfer_t *ctrl = s_ctx->xfer_ctrl;
    ctrl->num_bytes = sizeof(usb_setup_packet_t) + buf_len;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl->data_buffer;

    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = 0;
    setup->wValue = 0;
    setup->wIndex = s_ctx->intf_num;
    setup->wLength = buf_len;

    ctrl->bEndpointAddress = 0;
    esp_err_t err = usb_host_transfer_submit_control(s_ctx->client_hdl, ctrl);
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(s_ctx->xfer_done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ctx->xfer_result == ESP_OK) {
        int actual = ctrl->actual_num_bytes - sizeof(usb_setup_packet_t);
        if (actual > 0) {
            int copy_len = actual < (int)buf_len - 1 ? actual : (int)buf_len - 1;
            memcpy(buf, ctrl->data_buffer + sizeof(usb_setup_packet_t), copy_len);
            buf[copy_len] = '\0';
        }
    }
    return s_ctx->xfer_result;
}

void usb_printer_deinit(void)
{
    if (!s_ctx) return;

    s_ctx->running = false;
    vTaskDelay(pdMS_TO_TICKS(300));

    handle_device_gone();

    if (s_ctx->client_hdl) {
        usb_host_client_deregister(s_ctx->client_hdl);
    }
    usb_host_uninstall();

    vSemaphoreDelete(s_ctx->xfer_done_sem);
    vEventGroupDelete(s_ctx->events);
    free(s_ctx);
    s_ctx = NULL;
}
