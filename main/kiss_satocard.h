// The protocol shared by Satochip's card applets: select, status, secure
// channel, PIN, and the genuine card check. kiss_satochip.c (signing) and
// kiss_seedkeeper.c (loading a seed) sit on top of it.
//
// Written from the protocol (Toporin/SatochipApplet, Toporin/Seedkeeper-Applet
// and Seedkeeper's Specifications.md), not ported from pysatochip.
//
// Below this sits a transport: anything that can send one APDU and return the
// response with its status word. The USB reader driver is one; the host tests
// plug in an emulated card.
//
// Trust. The card signs the setup of each secure channel with its
// authentikey, and its certificate, issued under Satochip's sub CA for that
// applet, carries the same key. kiss_satocard_genuine() checks the
// certificate, checks that the certified key is the one that signed this
// session's channel, and has the card sign a fresh challenge. A reader that
// sits in the middle has to present a channel key of its own, so it fails
// here. The cards release their certificate only after the PIN, so the order
// is fixed: open, PIN, genuine, then anything else, which refuses until the
// check has passed.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_SW_OK 0x9000

// Send cmd (a complete short APDU) and fill resp with the response DATA, not
// counting the two status bytes, which go to *sw. Returns 0 when the card
// answered, nonzero when the transport failed (reader gone, card pulled,
// timeout): that is a different thing from the card saying no.
typedef int (*kiss_sc_xfer_fn)(void *ctx, const uint8_t *cmd, size_t cmd_len,
                               uint8_t *resp, size_t resp_cap,
                               size_t *resp_len, uint16_t *sw);

// Randomness for the channel's key, IVs and challenges. The device passes
// kiss_trng_fill; tests pass a fixed stream.
typedef void (*kiss_sc_rng_fn)(uint8_t *out, size_t n);

// Results. Every call returns one of these.
#define SC_OK               0
#define SC_ERR_IO          -1   // transport failed: card or reader went away
#define SC_ERR_NOT_APPLET  -2   // the card does not carry this applet
#define SC_ERR_SETUP       -3   // a blank card: set it up with the Satochip tools
#define SC_ERR_PROTOCOL    -4   // the card answered something malformed
#define SC_ERR_CHANNEL     -5   // the secure channel did not verify
#define SC_ERR_PIN         -6   // wrong PIN; tries_left says how many remain
#define SC_ERR_BLOCKED     -7   // PIN blocked: the card is locked
#define SC_ERR_NOT_GENUINE -8   // certificate or key check failed
#define SC_ERR_NO_CERT     -9   // the card carries no certificate at all
#define SC_ERR_DENIED     -10   // the card refuses this (export policy, 2FA)
#define SC_ERR_FORMAT     -11   // data this signer cannot use
#define SC_ERR_SW         -12   // any other refusal; last_sw has the code
#define SC_ERR_ORDER      -13   // called before the PIN or the genuine check

typedef enum {
    SC_APPLET_SATOCHIP,
    SC_APPLET_SEEDKEEPER,
} kiss_sc_applet;

// One card session. Holds channel keys, so it is wiped by kiss_satocard_close
// and must not be copied.
typedef struct {
    kiss_sc_xfer_fn xfer;
    void *xfer_ctx;
    kiss_sc_rng_fn rng;
    kiss_sc_applet applet;

    uint8_t version[4];        // protocol major/minor, applet major/minor
    uint8_t pin_tries;         // from status
    bool    setup_done;
    bool    seeded;            // Satochip: holds a wallet
    bool    needs_2fa;         // Satochip: signing wants a second device

    bool    channel;
    uint8_t enc_key[16];
    uint8_t mac_key[20];
    uint32_t iv_counter;
    // The channel setup as the card signed it, kept until the genuine check
    // can say whose key that was.
    uint8_t setup_hash[32];
    uint8_t setup_sig[80];
    uint8_t setup_sig_len;

    bool    pin_ok;
    bool    genuine;
    uint8_t authentikey[65];   // set by the genuine check: certified and bound

    uint16_t last_sw;
} kiss_satocard;

void kiss_satocard_init(kiss_satocard *c, kiss_sc_applet applet,
                        kiss_sc_xfer_fn xfer, void *ctx, kiss_sc_rng_fn rng);

// SELECT the applet, read its status, open the secure channel.
int kiss_satocard_open(kiss_satocard *c);

// One attempt, never retried here: a timeout may still have reached the card.
// SC_ERR_PIN fills *tries_left when the card says; SC_ERR_BLOCKED means it is
// locked (including when that attempt used the last try). pin is the caller's
// to wipe.
int kiss_satocard_verify_pin(kiss_satocard *c, const char *pin, size_t pin_len,
                             int *tries_left);

// The genuine card check; see the top of this file.
int kiss_satocard_genuine(kiss_satocard *c);

// Send one command inside the secure channel. Refused with SC_ERR_ORDER until
// the genuine check has passed. out gets the decrypted response data.
int kiss_satocard_xfer(kiss_satocard *c, const uint8_t *apdu, size_t apdu_len,
                       uint8_t *out, size_t cap, size_t *out_len);

// Wipe keys and state. Safe to call twice.
void kiss_satocard_close(kiss_satocard *c);

// ---- shared with the applet modules and the tests ----
// Verify a DER ECDSA signature (any S) over hash32 by an uncompressed key.
bool kiss_satocard_sig_ok(const uint8_t *der, size_t der_len,
                          const uint8_t hash32[32], const uint8_t pub65[65]);
// The cards name a key by its x coordinate and a signature by it: find the
// key, among the ones the signature could have come from, whose x matches.
// That is also the signature's verification.
int kiss_satocard_recover_by_x(const uint8_t *der, size_t der_len,
                               const uint8_t hash32[32], const uint8_t x32[32],
                               uint8_t out65[65]);
int kiss_satocard_hmac_sha1(const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len, uint8_t out[20]);
// Parse a DER certificate signed under anchor65 (an uncompressed secp256k1
// key); on success *pub65 gets the certificate's own key.
int kiss_satocard_check_cert(const uint8_t *der, size_t len,
                             const uint8_t anchor65[65], uint8_t pub65[65]);
#ifndef ESP_PLATFORM
// Swap the trusted sub CA key for a test one. NULL restores the real ones.
void kiss_satocard_test_set_anchor(const uint8_t anchor65[65]);
#endif
