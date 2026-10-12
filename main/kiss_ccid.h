// CCID: the protocol between this device and a USB smartcard reader.
//
// Pure protocol, no USB: the reader is reached through kiss_ccid_io_fn, which
// sends one CCID message to the bulk-out endpoint and returns the next one
// from bulk-in. kiss_usbcard.c supplies it on the device; the host tests
// supply an emulated reader.
//
// Readers come at two levels. An APDU-level reader takes a command APDU and
// hands back the response. A TPDU-level reader passes the card's own blocks
// through, so for a T=1 card this file speaks T=1 itself: I-blocks with
// chaining both ways, the card's wait-time extensions and IFS requests. A
// T=0-only card behind a TPDU reader is refused: every Satochip-family card
// (JCOP) speaks T=1.
//
// Recovery is deliberately short: anything a working reader and card never
// send is an error, the operation stops, and the card is powered down. The
// caller starts again from the top; nothing here retries a command, so a PIN
// is never sent twice.
//
// Written from the USB CCID specification rev 1.1 and ISO/IEC 7816-3. The
// ACR39U-NF quirk below was found by akg5188/KernSigner (MIT).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CCID_MSG_MAX   (10 + 261)       // header + the largest block we build
#define CCID_ATR_MAX   33

// Send out (one CCID message) and fill in with the reader's next message.
// 0 on success; nonzero when the USB transfer failed or timed out.
typedef int (*kiss_ccid_io_fn)(void *ctx, const uint8_t *out, size_t out_len,
                               uint8_t *in, size_t in_cap, size_t *in_len,
                               uint32_t timeout_ms);

#define CCID_OK          0
#define CCID_ERR_IO     -1   // the USB transfer failed: reader unplugged
#define CCID_ERR_NOCARD -2   // no card in the slot
#define CCID_ERR_READER -3   // the reader answered something malformed
#define CCID_ERR_CARD   -4   // the card broke the T=1 protocol
#define CCID_ERR_PROTO  -5   // a card or reader this driver does not speak
#define CCID_ERR_SIZE   -6   // a response larger than the caller's buffer

// What the configuration descriptor says about the reader.
typedef struct {
    uint8_t  interface_num;
    uint8_t  alt_setting;
    uint8_t  bulk_in, bulk_out;      // endpoint addresses
    uint16_t bulk_in_mps, bulk_out_mps;
    uint32_t protocols;              // dwProtocols: bit 0 T=0, bit 1 T=1
    uint32_t features;               // dwFeatures
    uint32_t max_msg;                // dwMaxCCIDMessageLength
    uint32_t max_ifsd;               // dwMaxIFSD
} kiss_ccid_reader;

// Find the CCID interface in a raw configuration descriptor. 0 when found.
int kiss_ccid_parse_config(const uint8_t *cfg, size_t len, kiss_ccid_reader *out);

// One reader session.
typedef struct {
    kiss_ccid_io_fn io;
    void *io_ctx;
    kiss_ccid_reader rd;
    bool tpdu;                       // this driver frames T=1 itself

    uint8_t atr[CCID_ATR_MAX];
    size_t  atr_len;
    bool    t1;                      // the card offers T=1
    uint8_t ta1, tc1, tccks, bwi_cwi, ifsc;

    uint8_t seq;                     // CCID bSeq
    uint8_t ns;                      // our next T=1 send sequence bit
    uint8_t nr;                      // the card's next expected sequence bit
} kiss_ccid;

// vid/pid pick known quirks.
void kiss_ccid_init(kiss_ccid *c, const kiss_ccid_reader *rd,
                    uint16_t vid, uint16_t pid,
                    kiss_ccid_io_fn io, void *io_ctx);

// Is a card in the slot? CCID_OK if present, CCID_ERR_NOCARD if not.
int kiss_ccid_slot_status(kiss_ccid *c);

// Power the card, read and check its ATR, and set T=1 parameters when this
// driver frames T=1.
int kiss_ccid_power_on(kiss_ccid *c);

// One command APDU, one response. resp gets the data without the status
// word, which goes to *sw.
int kiss_ccid_apdu(kiss_ccid *c, const uint8_t *apdu, size_t apdu_len,
                   uint8_t *resp, size_t resp_cap, size_t *resp_len,
                   uint16_t *sw);

void kiss_ccid_power_off(kiss_ccid *c);

// ---- exposed for the tests ----
// Fill the T=1 fields from an ATR. 0 when it parses (T=1 or not).
int kiss_ccid_parse_atr(kiss_ccid *c, const uint8_t *atr, size_t len);
