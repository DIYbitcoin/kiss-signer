// The USB smartcard reader on the board's USB OTG port: the device half of
// the card path, under kiss_ccid.c.
//
// Every call here runs on ONE task, the card worker (kiss_cardtask.c): it
// pumps the USB host client's events itself while it waits, so nothing else
// may call in. The host library gets its own small task for its events.
//
// The reader needs power the port does not give: a powered OTG Y cable or a
// powered hub sits between the board and the reader.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USBCARD_OK          0
#define USBCARD_NO_READER  -1   // nothing usable plugged in (yet)
#define USBCARD_NOT_CCID   -2   // a USB device that is not a card reader
#define USBCARD_NO_CARD    -3   // a reader, but no card in it
#define USBCARD_CARD_ERR   -4   // the card or reader would not talk
#define USBCARD_FAIL       -5   // the USB host could not start

// Install the USB host on the OTG port. Idempotent.
int kiss_usbcard_start(void);

// Wait up to timeout_ms for a CCID reader to be plugged in and opened.
int kiss_usbcard_wait_reader(uint32_t timeout_ms);

// Power the card in the reader and read its ATR.
int kiss_usbcard_card_on(void);

// The transport for kiss_satocard (kiss_sc_xfer_fn); ctx is unused.
int kiss_usbcard_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                      uint8_t *resp, size_t resp_cap, size_t *resp_len,
                      uint16_t *sw);

// Is the reader still there and a card still in it? Cheap: one CCID status.
bool kiss_usbcard_card_present(void);

// Power the card down. The reader stays open for the next session.
void kiss_usbcard_card_off(void);
