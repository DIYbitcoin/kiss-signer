// The card worker. See kiss_cardtask.h.
#include "kiss_cardtask.h"

#include <string.h>

#include "kiss_wipe.h"

static volatile kiss_ct_state s_state = CT_IDLE;
static volatile int s_detail;
static volatile size_t s_signed_len;

kiss_ct_state kiss_cardtask_state(void) { return s_state; }
int kiss_cardtask_detail(void) { return s_detail; }
size_t kiss_cardtask_signed_len(void) { return s_signed_len; }

#ifdef SIMULATOR
// ---- the simulator: states on demand, nothing behind them ----
void kiss_cardtask_sim_set(kiss_ct_state st, int detail)
{
    s_detail = detail;
    s_state = st;
}
void kiss_cardtask_begin(void) { s_detail = 0; s_state = CT_FIND_READER; }
void kiss_cardtask_pin(const char *pin, size_t len)
{
    (void)pin; (void)len;
    s_detail = 0;
    s_state = CT_UNLOCKING;
}
void kiss_cardtask_sign(uint8_t *out, size_t cap)
{
    (void)out; (void)cap;
    s_detail = -1;
    s_state = CT_SIGNED;
}
void kiss_cardtask_cancel(void) { s_detail = 0; s_state = CT_IDLE; }
bool kiss_cardtask_busy(void) { return false; }

#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kiss_cardwallet.h"
#include "kiss_crypto.h"
#include "kiss_psbt.h"
#include "kiss_satocard.h"
#include "kiss_usbcard.h"

enum { CMD_NONE, CMD_BEGIN, CMD_PIN, CMD_SIGN, CMD_CANCEL };

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static volatile int s_cmd;
static char s_pin[17];
static size_t s_pin_len;
static uint8_t *s_out;
static size_t s_out_cap;

static void set(kiss_ct_state st, int detail)
{
    s_detail = detail;
    s_state = st;
}

static void post(int cmd)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cmd = cmd;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

static int take(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int c = s_cmd;
    s_cmd = CMD_NONE;
    xSemaphoreGive(s_lock);
    return c;
}

static int card_error(int sc_rc)
{
    switch (sc_rc) {
    case SC_ERR_IO:          return CT_E_GONE;
    case SC_ERR_NOT_APPLET:  return CT_E_NOT_SATOCHIP;
    case SC_ERR_SETUP:       return CT_E_BLANK;
    case SC_ERR_BLOCKED:     return CT_E_BLOCKED;
    case SC_ERR_NOT_GENUINE: return CT_E_NOT_GENUINE;
    case SC_ERR_NO_CERT:     return CT_E_NO_CERT;
    case SC_ERR_DENIED:      return CT_E_TWO_FA;
    default:                 return CT_E_CARD;
    }
}

static void close_all(void)
{
    kiss_cardwallet_close();
    kiss_usbcard_card_off();
}

static void progress(int done, int total)
{
    (void)total;
    s_detail = done;
}

// Look for a reader and a card until one connects or a new command comes.
static void find_and_connect(void)
{
    close_all();
    if (kiss_usbcard_start() != USBCARD_OK) {
        set(CT_FAILED, CT_E_CARD);
        return;
    }
    set(CT_FIND_READER, 0);
    for (;;) {
        if (s_cmd != CMD_NONE)
            return;                          // cancelled or restarted
        int rc = kiss_usbcard_wait_reader(500);
        if (rc == USBCARD_NOT_CCID) {
            set(CT_FAILED, CT_E_NOT_CCID);
            return;
        }
        if (rc != USBCARD_OK) {
            set(CT_FIND_READER, 0);
            continue;
        }
        rc = kiss_usbcard_card_on();
        if (rc == USBCARD_NO_CARD || rc == USBCARD_NO_READER) {
            set(rc == USBCARD_NO_CARD ? CT_FIND_CARD : CT_FIND_READER, 0);
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }
        if (rc != USBCARD_OK) {
            set(CT_FAILED, CT_E_CARD);
            return;
        }
        set(CT_CONNECTING, 0);
        int tries = 0;
        rc = kiss_cardwallet_connect(kiss_usbcard_xfer, NULL, kiss_trng_fill, &tries);
        if (rc != SC_OK) {
            close_all();
            set(CT_FAILED, card_error(rc));
            return;
        }
        if (kiss_cardwallet_needs_2fa()) {
            close_all();
            set(CT_FAILED, CT_E_TWO_FA);
            return;
        }
        set(tries == 0 ? CT_FAILED : CT_NEED_PIN, tries == 0 ? CT_E_BLOCKED : tries);
        return;
    }
}

static void unlock(void)
{
    set(CT_UNLOCKING, 0);
    int tries = -1;
    int rc = kiss_cardwallet_unlock(s_pin, s_pin_len, &tries, progress);
    kiss_wipe(s_pin, sizeof s_pin);
    s_pin_len = 0;
    if (rc == SC_OK)
        set(CT_READY, 0);
    else if (rc == SC_ERR_PIN)
        set(CT_NEED_PIN, tries);
    else {
        close_all();
        set(CT_FAILED, card_error(rc));
    }
}

static void sign(void)
{
    set(CT_SIGNING, 0);
    size_t n = 0;
    int rc = kiss_psbt_sign(s_out, s_out_cap, &n);
    s_signed_len = rc == 0 ? n : 0;
    set(CT_SIGNED, rc);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        // Idle with a card wallet open: wake every second to check the card
        // is still there. Otherwise sleep until the UI asks for something.
        bool watching = s_state == CT_READY || s_state == CT_SIGNED;
        ulTaskNotifyTake(pdTRUE, watching ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
        int cmd = take();
        if (cmd == CMD_NONE) {
            if ((s_state == CT_READY || s_state == CT_SIGNED) &&
                !kiss_usbcard_card_present()) {
                close_all();
                set(CT_REMOVED, 0);
            }
            continue;
        }
        switch (cmd) {
        case CMD_BEGIN:  find_and_connect(); break;
        case CMD_PIN:    if (s_state == CT_NEED_PIN) unlock(); break;
        case CMD_SIGN:   if (s_state == CT_READY || s_state == CT_SIGNED) sign(); break;
        case CMD_CANCEL: close_all(); set(CT_IDLE, 0); break;
        default:         break;
        }
        // a command that arrived while that one ran is picked up next round
        if (s_cmd != CMD_NONE)
            xTaskNotifyGive(s_task);
    }
}

static bool ensure_task(void)
{
    if (s_task)
        return true;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return false;
    // CPU1, at the UI task's priority, the way prep_task runs: real overlap
    // with the display rather than time sliced against it.
    return xTaskCreatePinnedToCore(worker, "kisscard", 8192, NULL,
                                   uxTaskPriorityGet(NULL), &s_task, 1) == pdPASS;
}

void kiss_cardtask_begin(void)
{
    if (!ensure_task()) {
        set(CT_FAILED, CT_E_CARD);
        return;
    }
    set(CT_FIND_READER, 0);
    post(CMD_BEGIN);
}

void kiss_cardtask_pin(const char *pin, size_t len)
{
    if (!s_task || s_state != CT_NEED_PIN || len >= sizeof s_pin)
        return;
    memcpy(s_pin, pin, len);
    s_pin_len = len;
    set(CT_UNLOCKING, 0);
    post(CMD_PIN);
}

void kiss_cardtask_sign(uint8_t *out, size_t cap)
{
    if (!s_task || (s_state != CT_READY && s_state != CT_SIGNED))
        return;
    s_out = out;
    s_out_cap = cap;
    s_signed_len = 0;
    set(CT_SIGNING, 0);
    post(CMD_SIGN);
}

void kiss_cardtask_cancel(void)
{
    if (!s_task) {
        set(CT_IDLE, 0);
        return;
    }
    post(CMD_CANCEL);
}

bool kiss_cardtask_busy(void)
{
    return s_state == CT_SIGNING || s_state == CT_UNLOCKING ||
           s_state == CT_CONNECTING;
}
#endif
