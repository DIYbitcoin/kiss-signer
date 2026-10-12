// USB smartcard reader on the OTG port. See kiss_usbcard.h.
#include "kiss_usbcard.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "kiss_ccid.h"
#include "kiss_wipe.h"

static const char *TAG = "usbcard";

// One bulk transfer holds any CCID message this driver sends or expects,
// and is a multiple of every bulk max packet size (64 full speed, 512 high).
#define XFER_SIZE 512

static bool s_started;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_new_addr;             // set by the client callback
static bool s_gone;
static bool s_claimed;
static kiss_ccid_reader s_rd;
static kiss_ccid s_ccid;
static usb_transfer_t *s_out, *s_in;
static volatile bool s_xfer_done;

static void host_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        if (usb_host_lib_handle_events(portMAX_DELAY, &flags) != ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        s_new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == s_dev)
        s_gone = true;
}

static void xfer_cb(usb_transfer_t *t)
{
    (void)t;
    s_xfer_done = true;
}

static void close_device(void)
{
    if (s_out) { usb_host_transfer_free(s_out); s_out = NULL; }
    if (s_in)  { usb_host_transfer_free(s_in);  s_in = NULL; }
    if (s_dev && s_claimed)
        usb_host_interface_release(s_client, s_dev, s_rd.interface_num);
    s_claimed = false;
    if (s_dev)
        usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    s_gone = false;
    memset(&s_ccid, 0, sizeof s_ccid);
}

int kiss_usbcard_start(void)
{
    if (s_started)
        return USBCARD_OK;
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return USBCARD_FAIL;
    if (xTaskCreatePinnedToCore(host_lib_task, "usb_host", 4096, NULL, 5, NULL, 1) != pdPASS)
        return USBCARD_FAIL;
    const usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    if (usb_host_client_register(&ccfg, &s_client) != ESP_OK)
        return USBCARD_FAIL;
    s_started = true;
    return USBCARD_OK;
}

// Run one bulk transfer to completion, pumping the client's events while it
// is in flight. A transfer that does not finish in time is halted, flushed
// and waited out before its buffer is reused.
static int bulk(usb_transfer_t *t, uint8_t ep, int num_bytes, uint32_t timeout_ms)
{
    t->device_handle = s_dev;
    t->bEndpointAddress = ep;
    t->num_bytes = num_bytes;
    t->callback = xfer_cb;
    t->context = NULL;
    s_xfer_done = false;
    if (usb_host_transfer_submit(t) != ESP_OK)
        return -1;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!s_xfer_done && !s_gone && (int32_t)(until - xTaskGetTickCount()) > 0)
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    if (!s_xfer_done) {
        usb_host_endpoint_halt(s_dev, ep);
        usb_host_endpoint_flush(s_dev, ep);
        for (int i = 0; i < 100 && !s_xfer_done; i++)
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        usb_host_endpoint_clear(s_dev, ep);
        return -1;
    }
    return t->status == USB_TRANSFER_STATUS_COMPLETED ? 0 : -1;
}

// kiss_ccid_io_fn over the claimed bulk endpoints. out_len 0 only reads.
static int ccid_io(void *ctx, const uint8_t *out, size_t out_len,
                   uint8_t *in, size_t in_cap, size_t *in_len, uint32_t timeout_ms)
{
    (void)ctx;
    *in_len = 0;
    if (!s_dev || s_gone)
        return -1;
    if (out_len) {
        if (out_len > XFER_SIZE)
            return -1;
        memcpy(s_out->data_buffer, out, out_len);
        int rc = bulk(s_out, s_rd.bulk_out, (int)out_len, timeout_ms);
        kiss_wipe(s_out->data_buffer, XFER_SIZE);
        if (rc != 0)
            return -1;
    }
    if (bulk(s_in, s_rd.bulk_in, XFER_SIZE, timeout_ms) != 0)
        return -1;
    size_t n = (size_t)s_in->actual_num_bytes;
    if (n > in_cap) {
        kiss_wipe(s_in->data_buffer, XFER_SIZE);
        return -1;
    }
    memcpy(in, s_in->data_buffer, n);
    kiss_wipe(s_in->data_buffer, XFER_SIZE);
    *in_len = n;
    return 0;
}

static int open_device(uint8_t addr)
{
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return USBCARD_NO_READER;
    }
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    if (usb_host_get_device_descriptor(s_dev, &dd) != ESP_OK ||
        usb_host_get_active_config_descriptor(s_dev, &cd) != ESP_OK) {
        close_device();
        return USBCARD_NO_READER;
    }
    if (kiss_ccid_parse_config((const uint8_t *)cd, cd->wTotalLength, &s_rd) != 0) {
        ESP_LOGW(TAG, "%04x:%04x is not a CCID reader", dd->idVendor, dd->idProduct);
        close_device();
        return USBCARD_NOT_CCID;
    }
    if (usb_host_interface_claim(s_client, s_dev, s_rd.interface_num, s_rd.alt_setting) != ESP_OK) {
        close_device();
        return USBCARD_CARD_ERR;
    }
    s_claimed = true;
    if (usb_host_transfer_alloc(XFER_SIZE, 0, &s_out) != ESP_OK ||
        usb_host_transfer_alloc(XFER_SIZE, 0, &s_in) != ESP_OK) {
        close_device();
        return USBCARD_FAIL;
    }
    kiss_ccid_init(&s_ccid, &s_rd, dd->idVendor, dd->idProduct, ccid_io, NULL);
    ESP_LOGI(TAG, "reader %04x:%04x features 0x%08lx %s", dd->idVendor, dd->idProduct,
             (unsigned long)s_rd.features, s_ccid.tpdu ? "TPDU" : "APDU");
    return USBCARD_OK;
}

int kiss_usbcard_wait_reader(uint32_t timeout_ms)
{
    if (!s_started)
        return USBCARD_FAIL;
    if (s_dev && s_gone)
        close_device();
    if (s_dev)
        return USBCARD_OK;
    // A reader plugged in before the host started was enumerated before this
    // client could hear about it: look at what is already there first.
    uint8_t addrs[4];
    int n = 0;
    int last = USBCARD_NO_READER;
    if (usb_host_device_addr_list_fill(4, addrs, &n) == ESP_OK)
        for (int i = 0; i < n && i < 4; i++)
            if ((last = open_device(addrs[i])) == USBCARD_OK)
                return USBCARD_OK;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    do {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));
        if (s_new_addr) {
            uint8_t a = s_new_addr;
            s_new_addr = 0;
            last = open_device(a);
            if (last == USBCARD_OK)
                return USBCARD_OK;
        }
    } while ((int32_t)(until - xTaskGetTickCount()) > 0);
    return last;
}

int kiss_usbcard_card_on(void)
{
    if (!s_dev || s_gone)
        return USBCARD_NO_READER;
    int rc = kiss_ccid_power_on(&s_ccid);
    if (rc == CCID_OK)
        return USBCARD_OK;
    if (rc == CCID_ERR_IO)
        close_device();
    return rc == CCID_ERR_NOCARD ? USBCARD_NO_CARD
         : rc == CCID_ERR_IO     ? USBCARD_NO_READER
                                 : USBCARD_CARD_ERR;
}

int kiss_usbcard_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                      uint8_t *resp, size_t resp_cap, size_t *resp_len,
                      uint16_t *sw)
{
    (void)ctx;
    if (!s_dev || s_gone)
        return -1;
    int rc = kiss_ccid_apdu(&s_ccid, cmd, cmd_len, resp, resp_cap, resp_len, sw);
    if (rc == CCID_ERR_IO)
        close_device();
    return rc == CCID_OK ? 0 : -1;
}

bool kiss_usbcard_card_present(void)
{
    if (!s_dev || s_gone)
        return false;
    int rc = kiss_ccid_slot_status(&s_ccid);
    if (rc == CCID_ERR_IO)
        close_device();
    return rc == CCID_OK;
}

void kiss_usbcard_card_off(void)
{
    if (s_dev && !s_gone)
        kiss_ccid_power_off(&s_ccid);
}
