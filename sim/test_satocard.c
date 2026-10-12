// Desktop tests for the Satochip card family: kiss_satocard.c (channel, PIN,
// genuine check), kiss_satochip.c (keys and signatures from the card) and
// kiss_seedkeeper.c (loading a seed).
//
// The card is emulated here in C from the applets' own code: the same
// response layouts, the same IV and MAC rules, the same chunked export. The
// emulator shares the HMAC with the module under test, so the channel's
// primitives are pinned separately: HMAC-SHA1 by RFC 2202, and the first
// wrapped command byte for byte against values computed with openssl.
//
// The certificates are a throwaway test chain made with openssl: a
// secp256k1 "sub CA", a card certificate under it (SHA-512 and SHA-256
// variants), and a self-signed rogue one.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_cardwallet.h"
#include "kiss_crypto.h"
#include "kiss_satochip.h"
#include "kiss_satocard.h"
#include "kiss_seed.h"
#include "kiss_seedkeeper.h"

static int tfails;
static void tchk(const char *name, int ok)
{
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); tfails++; }
}

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        unsigned v;
        if (sscanf(h, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
    }
    return n;
}

static const uint8_t CERT_CARD[] = {
    0x30, 0x82, 0x01, 0x8f, 0x30, 0x82, 0x01, 0x36, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x01, 0x07, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce,
    0x3d, 0x04, 0x03, 0x04, 0x30, 0x2f, 0x31, 0x0d, 0x30, 0x0b, 0x06, 0x03,
    0x55, 0x04, 0x0a, 0x0c, 0x04, 0x54, 0x65, 0x73, 0x74, 0x31, 0x1e, 0x30,
    0x1c, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x15, 0x54, 0x65, 0x73, 0x74,
    0x20, 0x53, 0x65, 0x65, 0x64, 0x4b, 0x65, 0x65, 0x70, 0x65, 0x72, 0x20,
    0x53, 0x75, 0x62, 0x43, 0x41, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x31,
    0x30, 0x31, 0x30, 0x30, 0x33, 0x32, 0x33, 0x33, 0x37, 0x5a, 0x17, 0x0d,
    0x34, 0x36, 0x31, 0x30, 0x30, 0x35, 0x30, 0x33, 0x32, 0x33, 0x33, 0x37,
    0x5a, 0x30, 0x33, 0x31, 0x31, 0x30, 0x2f, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x28, 0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34,
    0x35, 0x35, 0x36, 0x36, 0x37, 0x37, 0x38, 0x38, 0x39, 0x39, 0x61, 0x61,
    0x62, 0x62, 0x63, 0x63, 0x64, 0x64, 0x65, 0x65, 0x66, 0x66, 0x30, 0x30,
    0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x30, 0x56, 0x30, 0x10, 0x06, 0x07,
    0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x05, 0x2b, 0x81, 0x04,
    0x00, 0x0a, 0x03, 0x42, 0x00, 0x04, 0x1f, 0xd4, 0x62, 0xf3, 0x65, 0xd1,
    0xc6, 0x07, 0x00, 0x4e, 0xb6, 0x92, 0x08, 0x2a, 0xdc, 0x91, 0x1b, 0xfd,
    0x69, 0x05, 0x0a, 0xfe, 0x88, 0x02, 0x57, 0x6a, 0xd4, 0xf1, 0xad, 0x09,
    0x57, 0x1f, 0xae, 0x1d, 0xee, 0xe5, 0x4e, 0x3f, 0xe3, 0x51, 0x1a, 0xc1,
    0x50, 0x3f, 0x49, 0x52, 0xcf, 0x2f, 0xc0, 0xe0, 0x28, 0xfa, 0x8e, 0x60,
    0xb3, 0xb1, 0xc9, 0xba, 0x31, 0x6d, 0xfb, 0x91, 0x43, 0xe3, 0xa3, 0x42,
    0x30, 0x40, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04,
    0x14, 0x3f, 0x7d, 0xe6, 0xf8, 0x35, 0xa2, 0xce, 0xbb, 0xfd, 0xd5, 0x76,
    0x39, 0xd8, 0x79, 0x12, 0x06, 0x98, 0x47, 0x19, 0xdc, 0x30, 0x1f, 0x06,
    0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xd1, 0xf6,
    0x08, 0x0b, 0xe1, 0xf5, 0x74, 0x63, 0xa7, 0x73, 0xd8, 0x4f, 0x59, 0x8c,
    0x6e, 0x75, 0xa1, 0xbb, 0x39, 0xce, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86,
    0x48, 0xce, 0x3d, 0x04, 0x03, 0x04, 0x03, 0x47, 0x00, 0x30, 0x44, 0x02,
    0x20, 0x1a, 0x84, 0x0b, 0xbf, 0xe7, 0xb0, 0x96, 0xa1, 0xab, 0x90, 0xf3,
    0x3a, 0x15, 0xd1, 0x15, 0x6b, 0xe7, 0x1d, 0xd9, 0x82, 0xc4, 0x2c, 0x8b,
    0x34, 0x73, 0x30, 0xf9, 0x4a, 0x41, 0xb0, 0xd1, 0x7b, 0x02, 0x20, 0x0d,
    0x88, 0xa1, 0xf1, 0x9c, 0xad, 0x73, 0xfe, 0x18, 0x7a, 0x9b, 0xa4, 0xd5,
    0x94, 0x54, 0xd5, 0xac, 0xc5, 0x04, 0x18, 0x17, 0x76, 0xb3, 0xbd, 0x8d,
    0x99, 0xc0, 0xfa, 0x1f, 0x41, 0x7a, 0x19
};
static const uint8_t CERT_CARD256[] = {
    0x30, 0x82, 0x01, 0x90, 0x30, 0x82, 0x01, 0x36, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x01, 0x08, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce,
    0x3d, 0x04, 0x03, 0x02, 0x30, 0x2f, 0x31, 0x0d, 0x30, 0x0b, 0x06, 0x03,
    0x55, 0x04, 0x0a, 0x0c, 0x04, 0x54, 0x65, 0x73, 0x74, 0x31, 0x1e, 0x30,
    0x1c, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x15, 0x54, 0x65, 0x73, 0x74,
    0x20, 0x53, 0x65, 0x65, 0x64, 0x4b, 0x65, 0x65, 0x70, 0x65, 0x72, 0x20,
    0x53, 0x75, 0x62, 0x43, 0x41, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x31,
    0x30, 0x31, 0x30, 0x30, 0x33, 0x32, 0x33, 0x33, 0x37, 0x5a, 0x17, 0x0d,
    0x34, 0x36, 0x31, 0x30, 0x30, 0x35, 0x30, 0x33, 0x32, 0x33, 0x33, 0x37,
    0x5a, 0x30, 0x33, 0x31, 0x31, 0x30, 0x2f, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x28, 0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34,
    0x35, 0x35, 0x36, 0x36, 0x37, 0x37, 0x38, 0x38, 0x39, 0x39, 0x61, 0x61,
    0x62, 0x62, 0x63, 0x63, 0x64, 0x64, 0x65, 0x65, 0x66, 0x66, 0x30, 0x30,
    0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x30, 0x56, 0x30, 0x10, 0x06, 0x07,
    0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x05, 0x2b, 0x81, 0x04,
    0x00, 0x0a, 0x03, 0x42, 0x00, 0x04, 0x1f, 0xd4, 0x62, 0xf3, 0x65, 0xd1,
    0xc6, 0x07, 0x00, 0x4e, 0xb6, 0x92, 0x08, 0x2a, 0xdc, 0x91, 0x1b, 0xfd,
    0x69, 0x05, 0x0a, 0xfe, 0x88, 0x02, 0x57, 0x6a, 0xd4, 0xf1, 0xad, 0x09,
    0x57, 0x1f, 0xae, 0x1d, 0xee, 0xe5, 0x4e, 0x3f, 0xe3, 0x51, 0x1a, 0xc1,
    0x50, 0x3f, 0x49, 0x52, 0xcf, 0x2f, 0xc0, 0xe0, 0x28, 0xfa, 0x8e, 0x60,
    0xb3, 0xb1, 0xc9, 0xba, 0x31, 0x6d, 0xfb, 0x91, 0x43, 0xe3, 0xa3, 0x42,
    0x30, 0x40, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04,
    0x14, 0x3f, 0x7d, 0xe6, 0xf8, 0x35, 0xa2, 0xce, 0xbb, 0xfd, 0xd5, 0x76,
    0x39, 0xd8, 0x79, 0x12, 0x06, 0x98, 0x47, 0x19, 0xdc, 0x30, 0x1f, 0x06,
    0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xd1, 0xf6,
    0x08, 0x0b, 0xe1, 0xf5, 0x74, 0x63, 0xa7, 0x73, 0xd8, 0x4f, 0x59, 0x8c,
    0x6e, 0x75, 0xa1, 0xbb, 0x39, 0xce, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86,
    0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45, 0x02,
    0x20, 0x43, 0xf8, 0x99, 0x5a, 0x29, 0xb9, 0x87, 0x13, 0x2a, 0xb9, 0xa3,
    0x81, 0xb2, 0xf4, 0x46, 0x77, 0xa7, 0x80, 0xd3, 0xd9, 0xac, 0xca, 0xac,
    0xff, 0xdc, 0x2f, 0x7b, 0xd3, 0xed, 0x7b, 0xae, 0x22, 0x02, 0x21, 0x00,
    0xbc, 0x10, 0x44, 0xf5, 0x2d, 0xc6, 0x74, 0x36, 0xd7, 0x74, 0xd6, 0x4f,
    0x7e, 0x21, 0xa3, 0xa0, 0xd1, 0x04, 0x4c, 0x81, 0x5e, 0x19, 0x3b, 0xb7,
    0x0f, 0xe5, 0x03, 0xe7, 0xd1, 0xa3, 0x3d, 0xc0
};
static const uint8_t CERT_ROGUE[] = {
    0x30, 0x82, 0x01, 0x73, 0x30, 0x82, 0x01, 0x18, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x14, 0x32, 0x88, 0x91, 0x0b, 0x08, 0xcb, 0x96, 0x9d, 0xfc,
    0x11, 0x89, 0x80, 0x2d, 0x6c, 0x78, 0x3e, 0x89, 0x3c, 0x9d, 0xec, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04, 0x30,
    0x10, 0x31, 0x0e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x05,
    0x72, 0x6f, 0x67, 0x75, 0x65, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x31,
    0x30, 0x31, 0x30, 0x30, 0x33, 0x32, 0x34, 0x32, 0x36, 0x5a, 0x17, 0x0d,
    0x34, 0x36, 0x31, 0x30, 0x30, 0x35, 0x30, 0x33, 0x32, 0x34, 0x32, 0x36,
    0x5a, 0x30, 0x10, 0x31, 0x0e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x05, 0x72, 0x6f, 0x67, 0x75, 0x65, 0x30, 0x56, 0x30, 0x10, 0x06,
    0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x05, 0x2b, 0x81,
    0x04, 0x00, 0x0a, 0x03, 0x42, 0x00, 0x04, 0x91, 0x5e, 0xba, 0x69, 0x1a,
    0xcd, 0xcd, 0xb0, 0xe3, 0x0b, 0x9d, 0x6b, 0xe4, 0x46, 0x9a, 0xb1, 0x66,
    0x47, 0x98, 0x16, 0xa0, 0x37, 0xbf, 0xfc, 0x02, 0x2c, 0x9d, 0xa4, 0x5c,
    0xb7, 0x68, 0xe0, 0xe8, 0x8a, 0x19, 0xfc, 0xb1, 0xf6, 0x46, 0xa5, 0xb4,
    0x26, 0x82, 0xf6, 0xfd, 0xe2, 0x5e, 0x0d, 0x28, 0x58, 0x2d, 0xc8, 0xc3,
    0xfb, 0xd7, 0x47, 0x7c, 0xbe, 0xbf, 0xa4, 0x93, 0x8c, 0xe0, 0x47, 0xa3,
    0x53, 0x30, 0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16,
    0x04, 0x14, 0xa5, 0xd5, 0x9c, 0x1f, 0xee, 0xdb, 0x37, 0x58, 0xfb, 0xa3,
    0x66, 0xe3, 0x87, 0xe9, 0x01, 0xcb, 0x2b, 0xf6, 0xcb, 0x47, 0x30, 0x1f,
    0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xa5,
    0xd5, 0x9c, 0x1f, 0xee, 0xdb, 0x37, 0x58, 0xfb, 0xa3, 0x66, 0xe3, 0x87,
    0xe9, 0x01, 0xcb, 0x2b, 0xf6, 0xcb, 0x47, 0x30, 0x0f, 0x06, 0x03, 0x55,
    0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff,
    0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04,
    0x03, 0x49, 0x00, 0x30, 0x46, 0x02, 0x21, 0x00, 0xe7, 0xbc, 0x64, 0x60,
    0x0c, 0x1e, 0xc4, 0x74, 0x10, 0x02, 0xa2, 0x29, 0xd2, 0x2b, 0x0f, 0x21,
    0x3a, 0xa0, 0x76, 0x4e, 0x3c, 0x90, 0x2c, 0xa6, 0xf8, 0x08, 0xa4, 0xad,
    0xf6, 0xd8, 0x6e, 0x3a, 0x02, 0x21, 0x00, 0xdd, 0x27, 0x08, 0xec, 0x9d,
    0x9b, 0x6b, 0xae, 0xde, 0x74, 0xa2, 0x4b, 0x5b, 0x2d, 0xee, 0xb4, 0x9a,
    0x40, 0xac, 0xe6, 0xf5, 0xdc, 0x9c, 0x31, 0x8f, 0xd0, 0x26, 0x33, 0x48,
    0xab, 0xe6, 0xd2
};

// openssl: the test sub CA, the card's authentikey, a rogue key
static const char *SUBCA_PUB = "0480a6708bbf740c692e2c616f2f25daed13c9da01d771d7fe0352d57fd72194e9308af2a5db9bf8c9322a62de74ff326e458cf23b4a3123facc3a894a66fc6264";
static const char *CARD_PRIV = "3716af8a8951c95a82131a65a887afbd549f9f8c8c1b944e3d0d15321b4fe7f4";
static const char *CARD_PUB  = "041fd462f365d1c607004eb692082adc911bfd69050afe8802576ad4f1ad09571fae1deee54e3fe3511ac1503f4952cf2fc0e028fa8e60b3b1c9ba316dfb9143e3";
static const char *ROGUE_PRIV = "e3a4b2bbeefb395f6302c7a617b8d8ff74a9685e23bfe2aeda25bbbf1b563d7c";

#define DEV_WORDS "abandon abandon abandon abandon abandon abandon " \
                  "abandon abandon abandon abandon abandon about"
#define ALT_WORDS "legal winner thank year wave sausage worth useful legal " \
                  "winner thank yellow"

static secp256k1_context *T_CTX;

// ---- the emulated card ----
typedef struct {
    uint16_t id;
    uint8_t type, subtype, export_ctrl;
    const char *label;
    uint8_t data[400];
    size_t len;
} fake_secret;

typedef struct {
    kiss_sc_applet applet;
    bool selected, setup_done, needs_2fa;
    bool fail_io;
    bool high_s;              // sign with the high-S form, as JavaCard may
    bool channel_by_rogue;    // a reader in the middle: its own channel key
    bool sign_wrong_key;      // Satochip: answer with a different key's sig
    bool tamper_export;       // SeedKeeper: change data after signing it
    uint8_t eph_priv[32];
    uint8_t auth_priv[32];
    const uint8_t *cert;
    size_t cert_len;

    bool sc;
    uint8_t enc[16], mac[20];
    uint32_t iv_last;
    char pin[17];
    int tries;
    bool pin_ok;

    // Satochip
    struct ext_key master;
    uint8_t last_priv[32];
    bool have_last;

    // SeedKeeper
    fake_secret secrets[6];
    int nsecrets;
    int cursor;
    int exp_idx;
    size_t exp_off;
    uint8_t exp_stored[416];
    size_t exp_stored_len;

    // capture
    uint8_t first_wrapped[160];
    size_t first_wrapped_len;
    int wrapped;
} fake_card;

static void pub_of(const uint8_t priv[32], uint8_t out65[65])
{
    secp256k1_pubkey pk;
    size_t n = 65;
    (void)secp256k1_ec_pubkey_create(T_CTX, &pk, priv);
    secp256k1_ec_pubkey_serialize(T_CTX, out65, &n, &pk, SECP256K1_EC_UNCOMPRESSED);
}

static size_t card_sign(const fake_card *fc, const uint8_t priv[32],
                        const uint8_t *msg, size_t n, bool prehashed, uint8_t *der)
{
    uint8_t h[32], c64[64];
    if (prehashed) memcpy(h, msg, 32);
    else (void)wally_sha256(msg, n, h, 32);
    secp256k1_ecdsa_signature sig;
    (void)secp256k1_ecdsa_sign(T_CTX, &sig, h, priv, NULL, NULL);
    if (fc->high_s) {
        secp256k1_ecdsa_signature_serialize_compact(T_CTX, c64, &sig);
        (void)secp256k1_ec_seckey_negate(T_CTX, c64 + 32);
        (void)secp256k1_ecdsa_signature_parse_compact(T_CTX, &sig, c64);
    }
    size_t l = 72;
    (void)secp256k1_ecdsa_signature_serialize_der(T_CTX, der, &l, &sig);
    return l;
}

static int ecdh_x(unsigned char *out, const unsigned char *x32,
                  const unsigned char *y32, void *data)
{
    (void)y32; (void)data;
    memcpy(out, x32, 32);
    return 1;
}

static void put16(uint8_t *p, size_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// One command inside the channel, already decrypted. Fills out/olen, returns SW.
static uint16_t card_inner(fake_card *fc, const uint8_t *p, size_t n,
                           uint8_t *out, size_t *olen)
{
    *olen = 0;
    if (n < 4) return 0x6700;
    uint8_t ins = p[1], p1 = p[2], p2 = p[3];
    size_t lc = n > 4 ? p[4] : 0;
    const uint8_t *d = p + 5;

    if (ins == 0x42) {
        if (fc->tries == 0) return 0x9C0C;
        if (lc == strlen(fc->pin) && memcmp(d, fc->pin, lc) == 0) {
            fc->pin_ok = true;
            fc->tries = 3;
            return 0x9000;
        }
        fc->tries--;
        return (uint16_t)(0x63C0 + fc->tries);
    }
    if (!fc->pin_ok) return 0x9C06;

    if (ins == 0x93) {
        if (p2 == 0x01) { put16(out, fc->cert ? fc->cert_len : 0); *olen = 2; return 0x9000; }
        size_t off = (size_t)d[0] << 8 | d[1], len = d[3];
        if (!fc->cert || off + len > fc->cert_len) return 0x6700;
        memcpy(out, fc->cert + off, len);
        *olen = len;
        return 0x9000;
    }
    if (ins == 0x9A) {
        uint8_t msg[10 + 64];
        memcpy(msg, "Challenge:", 10);
        memset(msg + 10, 0x33, 32);
        memcpy(msg + 42, d, 32);
        memset(out, 0x33, 32);
        size_t l = card_sign(fc, fc->auth_priv, msg, sizeof msg, false, out + 34);
        put16(out + 32, l);
        *olen = 34 + l;
        return 0x9000;
    }

    if (fc->applet == SC_APPLET_SATOCHIP && ins == 0x6D) {
        uint32_t path[10];
        int depth = p1;
        for (int i = 0; i < depth; i++)
            path[i] = (uint32_t)d[4*i] << 24 | (uint32_t)d[4*i+1] << 16 |
                      (uint32_t)d[4*i+2] << 8 | d[4*i+3];
        struct ext_key k;
        if (depth == 0) k = fc->master;
        else if (bip32_key_from_parent_path(&fc->master, path, (size_t)depth,
                                            BIP32_FLAG_KEY_PRIVATE, &k) != WALLY_OK)
            return 0x9C0E;
        memcpy(fc->last_priv, k.priv_key + 1, 32);
        fc->have_last = true;
        uint8_t pub[65];
        pub_of(fc->last_priv, pub);
        memcpy(out, k.chain_code, 32);
        put16(out + 32, 32);
        memcpy(out + 34, pub + 1, 32);
        size_t s1 = card_sign(fc, fc->last_priv, out, 66, false, out + 68);
        put16(out + 66, s1);
        size_t o2 = 68 + s1;
        size_t s2 = card_sign(fc, fc->auth_priv, out, o2, false, out + o2 + 2);
        put16(out + o2, s2);
        *olen = o2 + 2 + s2;
        return 0x9000;
    }
    if (fc->applet == SC_APPLET_SATOCHIP && ins == 0x7A) {
        if (!fc->have_last || p1 != 0xFF) return 0x9C10;
        uint8_t other[32];
        memset(other, 0x44, 32);
        *olen = card_sign(fc, fc->sign_wrong_key ? other : fc->last_priv, d, 32, true, out);
        return 0x9000;
    }

    if (fc->applet == SC_APPLET_SEEDKEEPER && ins == 0xA6) {
        fc->cursor = p2 == 0x01 ? 0 : fc->cursor + 1;
        if (fc->cursor >= fc->nsecrets) return 0x9C12;
        const fake_secret *s = &fc->secrets[fc->cursor];
        size_t ll = strlen(s->label);
        put16(out, s->id);
        uint8_t *h = out + 2;
        memset(h, 0, 13);
        h[0] = s->type; h[1] = 1; h[2] = s->export_ctrl;
        uint8_t fp[32];
        (void)wally_sha256(s->data, s->len, fp, 32);
        memcpy(h + 6, fp, 4);
        h[10] = s->subtype; h[12] = (uint8_t)ll;
        memcpy(out + 15, s->label, ll);
        *olen = 15 + ll;
        return 0x9000;
    }
    if (fc->applet == SC_APPLET_SEEDKEEPER && ins == 0xA2) {
        if (p1 != 0x01) return 0x9C0F;
        uint16_t id = (uint16_t)(d[0] << 8 | d[1]);
        if (p2 == 0x01) {
            fc->exp_idx = -1;
            for (int i = 0; i < fc->nsecrets; i++)
                if (fc->secrets[i].id == id) fc->exp_idx = i;
            if (fc->exp_idx < 0) return 0x9C08;
            const fake_secret *s = &fc->secrets[fc->exp_idx];
            if (s->export_ctrl != 0x01) return 0x9C31;
            // what the card stores: the data padded to the AES block
            size_t pad = 16 - s->len % 16;
            memcpy(fc->exp_stored, s->data, s->len);
            memset(fc->exp_stored + s->len, (int)pad, pad);
            fc->exp_stored_len = s->len + pad;
            fc->exp_off = 0;
            size_t ll = strlen(s->label);
            put16(out, id);
            uint8_t *h = out + 2;
            memset(h, 0, 13);
            h[0] = s->type; h[1] = 1; h[2] = s->export_ctrl; h[3] = 1;
            uint8_t fp[32];
            (void)wally_sha256(s->data, s->len, fp, 32);
            memcpy(h + 6, fp, 4);
            h[10] = s->subtype; h[12] = (uint8_t)ll;
            memcpy(out + 15, s->label, ll);
            *olen = 15 + ll;
            return 0x9000;
        }
        if (fc->exp_idx < 0) return 0x9C30;
        const fake_secret *s = &fc->secrets[fc->exp_idx];
        size_t remaining = fc->exp_stored_len - fc->exp_off;
        if (remaining > 128) {
            put16(out, 128);
            memcpy(out + 2, fc->exp_stored + fc->exp_off, 128);
            fc->exp_off += 128;
            *olen = 130;
            return 0x9000;
        }
        size_t padv = fc->exp_stored[fc->exp_stored_len - 1];
        size_t dlen = remaining - padv;
        put16(out, dlen);
        memcpy(out + 2, fc->exp_stored + fc->exp_off, dlen);
        // the signature covers id, header, label and the whole secret
        uint8_t signed_buf[600];
        size_t ll = strlen(s->label), o = 0;
        put16(signed_buf, s->id); o = 2;
        uint8_t *h = signed_buf + o;
        memset(h, 0, 13);
        h[0] = s->type; h[1] = 1; h[2] = s->export_ctrl; h[3] = 1;
        uint8_t fp[32];
        (void)wally_sha256(s->data, s->len, fp, 32);
        memcpy(h + 6, fp, 4);
        h[10] = s->subtype; h[12] = (uint8_t)ll;
        o += 13;
        memcpy(signed_buf + o, s->label, ll); o += ll;
        memcpy(signed_buf + o, s->data, s->len); o += s->len;
        size_t sl = card_sign(fc, fc->auth_priv, signed_buf, o, false, out + 2 + dlen + 2);
        put16(out + 2 + dlen, sl);
        if (fc->tamper_export && dlen)
            out[2] ^= 0x01;
        fc->exp_idx = -1;
        *olen = 2 + dlen + 2 + sl;
        return 0x9000;
    }
    return 0x6D00;
}

static int fake_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                     uint8_t *resp, size_t cap, size_t *resp_len, uint16_t *sw)
{
    fake_card *fc = ctx;
    *resp_len = 0;
    if (fc->fail_io) return -1;
    if (cmd_len < 4) { *sw = 0x6700; return 0; }
    uint8_t ins = cmd[1];

    if (ins == 0xA4) {
        static const uint8_t sato[] = { 'S','a','t','o','C','h','i','p' };
        static const uint8_t seed[] = { 'S','e','e','d','K','e','e','p','e','r' };
        const uint8_t *aid = fc->applet == SC_APPLET_SATOCHIP ? sato : seed;
        size_t al = fc->applet == SC_APPLET_SATOCHIP ? sizeof sato : sizeof seed;
        fc->sc = false;
        fc->pin_ok = false;
        fc->selected = cmd_len == 5 + al && memcmp(cmd + 5, aid, al) == 0;
        *sw = fc->selected ? 0x9000 : 0x6A82;
        return 0;
    }
    if (!fc->selected) { *sw = 0x6985; return 0; }
    if (ins == 0x3C) {
        memset(resp, 0, 13);
        resp[1] = 12; resp[3] = 6;
        resp[4] = (uint8_t)fc->tries; resp[5] = 1;
        resp[8] = fc->needs_2fa; resp[9] = 1; resp[10] = fc->setup_done; resp[11] = 1;
        *resp_len = 13;
        *sw = 0x9000;
        return 0;
    }
    if (!fc->setup_done) { *sw = 0x9C04; return 0; }
    if (ins == 0x81) {
        secp256k1_pubkey host;
        uint8_t x[32], k[20];
        if (cmd_len != 70 || !secp256k1_ec_pubkey_parse(T_CTX, &host, cmd + 5, 65) ||
            !secp256k1_ecdh(T_CTX, x, &host, fc->eph_priv, ecdh_x, NULL)) {
            *sw = 0x9C0F;
            return 0;
        }
        kiss_satocard_hmac_sha1(x, 32, (const uint8_t *)"sc_key", 6, k);
        memcpy(fc->enc, k, 16);
        kiss_satocard_hmac_sha1(x, 32, (const uint8_t *)"sc_mac", 6, fc->mac);
        fc->iv_last = 0;
        fc->sc = true;
        uint8_t eph[65], ap[65];
        pub_of(fc->eph_priv, eph);
        put16(resp, 32);
        memcpy(resp + 2, eph + 1, 32);
        size_t s1 = card_sign(fc, fc->eph_priv, resp, 34, false, resp + 36);
        put16(resp + 34, s1);
        size_t o2 = 36 + s1;
        uint8_t rogue[32];
        unhex(ROGUE_PRIV, rogue, 32);
        const uint8_t *signer = fc->channel_by_rogue ? rogue : fc->auth_priv;
        size_t s2 = card_sign(fc, signer, resp, o2, false, resp + o2 + 2);
        put16(resp + o2, s2);
        size_t o3 = o2 + 2 + s2;
        if (fc->applet == SC_APPLET_SEEDKEEPER) {   // SeedKeeper adds the key's x
            pub_of(signer, ap);
            put16(resp + o3, 32);
            memcpy(resp + o3 + 2, ap + 1, 32);
            o3 += 34;
        }
        *resp_len = o3;
        *sw = 0x9000;
        return 0;
    }
    if (ins != 0x82) { *sw = 0x9C20; return 0; }

    // ---- the secure channel, the card's side ----
    if (!fc->sc) { *sw = 0x9C21; return 0; }
    if (fc->wrapped++ == 0 && cmd_len <= sizeof fc->first_wrapped) {
        memcpy(fc->first_wrapped, cmd, cmd_len);
        fc->first_wrapped_len = cmd_len;
    }
    const uint8_t *d = cmd + 5;
    size_t dl = cmd[4];
    if (dl + 5 != cmd_len || dl < 18) { *sw = 0x6700; return 0; }
    size_t ct = (size_t)d[16] << 8 | d[17];
    if (18 + ct + 2 + 20 != dl || d[18 + ct] != 0 || d[19 + ct] != 20) { *sw = 0x9C23; return 0; }
    uint8_t m[20];
    kiss_satocard_hmac_sha1(fc->mac, 20, d, 18 + ct, m);
    if (memcmp(m, d + 20 + ct, 20) != 0) { *sw = 0x9C23; return 0; }
    uint32_t ctr = (uint32_t)d[12] << 24 | (uint32_t)d[13] << 16 | (uint32_t)d[14] << 8 | d[15];
    if (!(ctr & 1) || ctr <= fc->iv_last) { *sw = 0x9C22; return 0; }
    fc->iv_last = ctr;
    uint8_t plain[128];
    size_t pl = 0;
    if (wally_aes_cbc(fc->enc, 16, d, 16, d + 18, ct, AES_FLAG_DECRYPT,
                      plain, sizeof plain, &pl) != WALLY_OK) { *sw = 0x6F00; return 0; }

    uint8_t out[400];
    size_t olen;
    *sw = card_inner(fc, plain, pl, out, &olen);
    if (*sw != 0x9000 || olen == 0)
        return 0;
    uint8_t iv[16];
    memset(iv, 0x5A, 16);
    memcpy(resp, iv, 16);
    size_t cl = 0;
    if (wally_aes_cbc(fc->enc, 16, iv, 16, out, olen, AES_FLAG_ENCRYPT,
                      resp + 18, (cap - 18) & ~(size_t)15, &cl) != WALLY_OK ||
        cl > cap - 18) {
        *sw = 0x6F00;
        return 0;
    }
    put16(resp + 16, cl);
    *resp_len = 18 + cl;
    return 0;
}

// The host's randomness: 32-byte draws (keys, challenges) are 0x11, 12-byte
// IV draws are 0xA5. Fixed, so the first wrapped command is reproducible.
static void fixed_rng(uint8_t *out, size_t n)
{
    memset(out, n == 12 ? 0xA5 : 0x11, n);
}

static void card_new(fake_card *fc, kiss_sc_applet applet)
{
    memset(fc, 0, sizeof *fc);
    fc->applet = applet;
    fc->setup_done = true;
    memset(fc->eph_priv, 0x22, 32);
    unhex(CARD_PRIV, fc->auth_priv, 32);
    fc->cert = CERT_CARD;
    fc->cert_len = sizeof CERT_CARD;
    snprintf(fc->pin, sizeof fc->pin, "123456");
    fc->tries = 3;
    fc->exp_idx = -1;
    uint8_t seed[64];
    size_t n;
    (void)bip39_mnemonic_to_seed(DEV_WORDS, NULL, seed, sizeof seed, &n);
    (void)bip32_key_from_seed(seed, 64, BIP32_VER_MAIN_PRIVATE, 0, &fc->master);
}

// open + PIN + genuine, the order every session takes
static int unlock(kiss_satocard *c, fake_card *fc)
{
    kiss_satocard_init(c, fc->applet, fake_xfer, fc, fixed_rng);
    int rc = kiss_satocard_open(c);
    if (rc == SC_OK)
        rc = kiss_satocard_verify_pin(c, "123456", 6, NULL);
    if (rc == SC_OK)
        rc = kiss_satocard_genuine(c);
    if (rc != SC_OK)          // the step is in the log when a check fails
        printf("  (unlock: rc %d, last sw %04X)\n", rc, c->last_sw);
    return rc;
}

static void test_primitives(void)
{
    uint8_t out[20], want[20], key[80];

    memset(key, 0x0b, 20);
    kiss_satocard_hmac_sha1(key, 20, (const uint8_t *)"Hi There", 8, out);
    unhex("b617318655057264e28bc0b6fb378c8ef146be00", want, 20);
    tchk("hmac-sha1: RFC 2202 case 1", memcmp(out, want, 20) == 0);

    const char *m2 = "what do ya want for nothing?";
    kiss_satocard_hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)m2, strlen(m2), out);
    unhex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", want, 20);
    tchk("hmac-sha1: RFC 2202 case 2", memcmp(out, want, 20) == 0);

    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    memset(key, 0xaa, 80);
    kiss_satocard_hmac_sha1(key, 80, (const uint8_t *)m6, strlen(m6), out);
    unhex("aa4ae5e15272d00e95705637ce8a3b55ed402112", want, 20);
    tchk("hmac-sha1: RFC 2202 case 6 (long key)", memcmp(out, want, 20) == 0);
}

static void test_cert(void)
{
    uint8_t anchor[65], pub[65], want[65], bad[sizeof CERT_CARD];
    unhex(SUBCA_PUB, anchor, 65);
    unhex(CARD_PUB, want, 65);

    tchk("cert: SHA-512 card certificate verifies",
         kiss_satocard_check_cert(CERT_CARD, sizeof CERT_CARD, anchor, pub) == SC_OK &&
         memcmp(pub, want, 65) == 0);
    tchk("cert: SHA-256 card certificate verifies",
         kiss_satocard_check_cert(CERT_CARD256, sizeof CERT_CARD256, anchor, pub) == SC_OK &&
         memcmp(pub, want, 65) == 0);
    tchk("cert: self-signed rogue refused",
         kiss_satocard_check_cert(CERT_ROGUE, sizeof CERT_ROGUE, anchor, pub) == SC_ERR_NOT_GENUINE);
    uint8_t other[65];
    memcpy(other, want, 65);
    tchk("cert: wrong anchor refused",
         kiss_satocard_check_cert(CERT_CARD, sizeof CERT_CARD, other, pub) == SC_ERR_NOT_GENUINE);
    for (size_t cut = 0; cut < sizeof CERT_CARD; cut += 37)
        if (kiss_satocard_check_cert(CERT_CARD, cut, anchor, pub) != SC_ERR_NOT_GENUINE) {
            tchk("cert: every truncation refused", 0);
            break;
        }
    int flips_ok = 1;
    for (size_t i = 0; i < sizeof CERT_CARD; i++) {
        memcpy(bad, CERT_CARD, sizeof bad);
        bad[i] ^= 0x04;
        if (kiss_satocard_check_cert(bad, sizeof bad, anchor, pub) != SC_ERR_NOT_GENUINE)
            flips_ok = 0;
    }
    tchk("cert: every single flipped bit refused", flips_ok);
}

static void test_channel_vector(void)
{
    // The first wrapped command (VERIFY PIN "123456") with host key 0x11..,
    // card ephemeral key 0x22.., IV a5*12 | 00000001, computed with openssl:
    // pkeyutl -derive, dgst -hmac sha1, enc -aes-128-cbc.
    fake_card fc;
    kiss_satocard c;
    card_new(&fc, SC_APPLET_SATOCHIP);
    kiss_satocard_init(&c, SC_APPLET_SATOCHIP, fake_xfer, &fc, fixed_rng);
    tchk("vector: open", kiss_satocard_open(&c) == SC_OK);
    tchk("vector: PIN accepted", kiss_satocard_verify_pin(&c, "123456", 6, NULL) == SC_OK);
    uint8_t want[5 + 16 + 2 + 16 + 2 + 20];
    size_t n = unhex("b082000038"
                     "a5a5a5a5a5a5a5a5a5a5a5a500000001" "0010"
                     "4051ee26d22c5f2377dd2586a0829a68" "0014"
                     "46659b64bd8b42a1baef43f9844cd01dcebc1b1b", want, sizeof want);
    tchk("vector: wrapped VERIFY PIN matches openssl byte for byte",
         fc.first_wrapped_len == n && memcmp(fc.first_wrapped, want, n) == 0);
    kiss_satocard_close(&c);
}

static void test_session_rules(void)
{
    fake_card fc;
    kiss_satocard c;
    uint8_t anchor[65], want[65];
    unhex(SUBCA_PUB, anchor, 65);
    unhex(CARD_PUB, want, 65);

    // the real sub CA must refuse the test chain
    kiss_satocard_test_set_anchor(NULL);
    card_new(&fc, SC_APPLET_SATOCHIP);
    tchk("genuine: test card refused under the real Satochip sub CA",
         unlock(&c, &fc) == SC_ERR_NOT_GENUINE && !c.genuine);
    kiss_satocard_test_set_anchor(anchor);

    card_new(&fc, SC_APPLET_SATOCHIP);
    tchk("genuine: test card passes under the test sub CA", unlock(&c, &fc) == SC_OK);
    tchk("genuine: authentikey is the certified key",
         memcmp(c.authentikey, want, 65) == 0);

    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.high_s = true;
    tchk("genuine: high-S card signatures still verify", unlock(&c, &fc) == SC_OK);

    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.channel_by_rogue = true;
    tchk("genuine: a reader in the middle (own channel key) is refused",
         unlock(&c, &fc) == SC_ERR_NOT_GENUINE && !c.genuine);
    uint8_t pub33[33], cc[32];
    uint32_t path[1] = { 0x80000054 };
    tchk("genuine: nothing else runs after a failed check",
         kiss_satochip_pubkey(&c, path, 1, pub33, cc) == SC_ERR_ORDER);

    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.cert = CERT_ROGUE;
    fc.cert_len = sizeof CERT_ROGUE;
    tchk("genuine: a certificate from another CA is refused",
         unlock(&c, &fc) == SC_ERR_NOT_GENUINE);

    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.cert = NULL;
    tchk("genuine: a card with no certificate says so",
         unlock(&c, &fc) == SC_ERR_NO_CERT);

    card_new(&fc, SC_APPLET_SATOCHIP);
    kiss_satocard_init(&c, SC_APPLET_SATOCHIP, fake_xfer, &fc, fixed_rng);
    tchk("order: open", kiss_satocard_open(&c) == SC_OK);
    tchk("order: genuine refused before the PIN", kiss_satocard_genuine(&c) == SC_ERR_ORDER);
    int left = 9;
    tchk("pin: wrong PIN refused", kiss_satocard_verify_pin(&c, "000000", 6, &left) == SC_ERR_PIN);
    tchk("pin: tries left reported", left == 2);
    tchk("pin: a second wrong one", kiss_satocard_verify_pin(&c, "000000", 6, &left) == SC_ERR_PIN && left == 1);
    tchk("pin: the last try reports blocked",
         kiss_satocard_verify_pin(&c, "000000", 6, &left) == SC_ERR_BLOCKED && left == 0);
    tchk("pin: a blocked card stays blocked",
         kiss_satocard_verify_pin(&c, "123456", 6, &left) == SC_ERR_BLOCKED);
    tchk("pin: too short never sent", kiss_satocard_verify_pin(&c, "12", 2, NULL) == SC_ERR_PIN);

    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.setup_done = false;
    kiss_satocard_init(&c, SC_APPLET_SATOCHIP, fake_xfer, &fc, fixed_rng);
    tchk("open: a blank card says set it up", kiss_satocard_open(&c) == SC_ERR_SETUP);

    card_new(&fc, SC_APPLET_SEEDKEEPER);
    kiss_satocard_init(&c, SC_APPLET_SATOCHIP, fake_xfer, &fc, fixed_rng);
    tchk("open: a SeedKeeper is not a Satochip", kiss_satocard_open(&c) == SC_ERR_NOT_APPLET);

    card_new(&fc, SC_APPLET_SATOCHIP);
    kiss_satocard_init(&c, SC_APPLET_SATOCHIP, fake_xfer, &fc, fixed_rng);
    tchk("io: open", kiss_satocard_open(&c) == SC_OK);
    fc.fail_io = true;
    tchk("io: a pulled card is an IO error, not a wrong PIN",
         kiss_satocard_verify_pin(&c, "123456", 6, NULL) == SC_ERR_IO);

    kiss_satocard_close(&c);
    uint8_t zero[sizeof c.enc_key] = { 0 };
    tchk("close: keys wiped", memcmp(c.enc_key, zero, sizeof zero) == 0 && !c.genuine &&
         c.xfer == fake_xfer);
}

static void test_satochip(void)
{
    fake_card fc;
    kiss_satocard c;
    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.high_s = true;
    tchk("satochip: unlock", unlock(&c, &fc) == SC_OK);

    // m/84'/1'/0'/0/3 against libwally on the same seed
    uint32_t path[5] = { 0x80000054, 0x80000001, 0x80000000, 0, 3 };
    struct ext_key want;
    (void)bip32_key_from_parent_path(&fc.master, path, 5, BIP32_FLAG_KEY_PRIVATE, &want);
    uint8_t pub33[33], cc[32];
    tchk("satochip: pubkey rc", kiss_satochip_pubkey(&c, path, 5, pub33, cc) == SC_OK);
    tchk("satochip: pubkey matches the seed's",
         memcmp(pub33, want.pub_key, 33) == 0 && memcmp(cc, want.chain_code, 32) == 0);
    tchk("satochip: master key at depth 0",
         kiss_satochip_pubkey(&c, path, 0, pub33, cc) == SC_OK &&
         memcmp(pub33, fc.master.pub_key, 33) == 0);

    uint8_t hash[32], der[72];
    size_t dl;
    memset(hash, 0x5C, 32);
    tchk("satochip: sign rc", kiss_satochip_sign(&c, path, 5, hash, der, &dl) == SC_OK);
    secp256k1_ecdsa_signature sig, norm;
    secp256k1_pubkey pk;
    int ok = secp256k1_ecdsa_signature_parse_der(T_CTX, &sig, der, dl) &&
             secp256k1_ec_pubkey_parse(T_CTX, &pk, want.pub_key, 33) &&
             secp256k1_ecdsa_verify(T_CTX, &sig, hash, &pk) == 1;
    tchk("satochip: signature verifies under the path's key", ok);
    tchk("satochip: signature is low-S (the card's was high)",
         ok && !secp256k1_ecdsa_signature_normalize(T_CTX, &norm, &sig));

    fc.sign_wrong_key = true;
    tchk("satochip: a signature by another key is refused",
         kiss_satochip_sign(&c, path, 5, hash, der, &dl) == SC_ERR_NOT_GENUINE && dl == 0);
    fc.sign_wrong_key = false;

    uint32_t deep[11] = { 0 };
    tchk("satochip: too deep a path refused",
         kiss_satochip_pubkey(&c, deep, 11, pub33, cc) == SC_ERR_FORMAT);

    card_new(&fc, SC_APPLET_SATOCHIP);
    fc.needs_2fa = true;
    tchk("satochip: 2FA card unlocks", unlock(&c, &fc) == SC_OK);
    tchk("satochip: but cannot sign offline",
         kiss_satochip_sign(&c, path, 5, hash, der, &dl) == SC_ERR_DENIED);
    kiss_satocard_close(&c);
}

static void add_secret(fake_card *fc, uint16_t id, uint8_t type, uint8_t subtype,
                       uint8_t exp, const char *label, const uint8_t *data, size_t len)
{
    fake_secret *s = &fc->secrets[fc->nsecrets++];
    s->id = id; s->type = type; s->subtype = subtype; s->export_ctrl = exp;
    s->label = label;
    memcpy(s->data, data, len);
    s->len = len;
}

static size_t bip39_entry(uint8_t *out, const char *words, const char *pass,
                          size_t desc_len)
{
    size_t m = strlen(words), p = strlen(pass), o = 0;
    out[o++] = (uint8_t)m; memcpy(out + o, words, m); o += m;
    out[o++] = (uint8_t)p; memcpy(out + o, pass, p); o += p;
    if (desc_len) {
        put16(out + o, desc_len); o += 2;
        memset(out + o, 'd', desc_len); o += desc_len;
    }
    return o;
}

static size_t masterseed_entry(uint8_t *out, const uint8_t *ent, size_t el,
                               const char *pass, bool corrupt)
{
    char words[WSEED_MAX_MNEMONIC];
    uint8_t seed[64];
    size_t n, o = 0, p = strlen(pass);
    (void)kiss_seed_from_entropy(ent, el, words, sizeof words);
    (void)bip39_mnemonic_to_seed(words, pass, seed, sizeof seed, &n);
    if (corrupt) seed[7] ^= 1;
    out[o++] = 64; memcpy(out + o, seed, 64); o += 64;
    out[o++] = 0x00;
    out[o++] = (uint8_t)el; memcpy(out + o, ent, el); o += el;
    out[o++] = (uint8_t)p; memcpy(out + o, pass, p); o += p;
    return o;
}

static void test_seedkeeper(void)
{
    fake_card fc;
    kiss_satocard c;
    uint8_t buf[400], ent[32];
    size_t n;
    card_new(&fc, SC_APPLET_SEEDKEEPER);
    n = bip39_entry(buf, ALT_WORDS, "TREZOR", 0);
    add_secret(&fc, 1, SK_TYPE_BIP39, 0, 1, "main", buf, n);
    memset(ent, 0x80, 32);
    n = masterseed_entry(buf, ent, 32, "", false);
    add_secret(&fc, 2, SK_TYPE_MASTERSEED, SK_SUBTYPE_BIP39, 1, "vault\x01", buf, n);
    n = masterseed_entry(buf, ent, 32, "x", true);
    add_secret(&fc, 3, SK_TYPE_MASTERSEED, SK_SUBTYPE_BIP39, 1, "bad", buf, n);
    n = bip39_entry(buf, DEV_WORDS, "", 0);
    add_secret(&fc, 4, SK_TYPE_BIP39, 0, 0, "locked", buf, n);
    n = bip39_entry(buf, DEV_WORDS, "", 200);     // past one 128-byte chunk
    add_secret(&fc, 5, SK_TYPE_BIP39, 0, 1, "long", buf, n);
    uint8_t pw[] = { 3, 'a', 'b', 'c' };
    add_secret(&fc, 6, 0x90, 0, 1, "password", pw, sizeof pw);

    tchk("seedkeeper: unlock", unlock(&c, &fc) == SC_OK);
    kiss_sk_header h[8];
    int count = 0;
    tchk("seedkeeper: list", kiss_sk_list(&c, h, 8, &count) == SC_OK && count == 6);
    tchk("seedkeeper: list ids", h[0].id == 1 && h[5].id == 6);
    tchk("seedkeeper: label kept as printable ASCII",
         strcmp(h[0].label, "main") == 0 && strcmp(h[1].label, "vault?") == 0);
    tchk("seedkeeper: loadable flags",
         kiss_sk_loadable(&h[0]) && kiss_sk_loadable(&h[1]) && !kiss_sk_loadable(&h[3]) &&
         !kiss_sk_loadable(&h[5]));

    char words[WSEED_MAX_MNEMONIC], pass[128], want[WSEED_MAX_MNEMONIC];
    tchk("seedkeeper: load a mnemonic with its passphrase",
         kiss_sk_load(&c, 1, words, sizeof words, pass, sizeof pass) == SC_OK &&
         strcmp(words, ALT_WORDS) == 0 && strcmp(pass, "TREZOR") == 0);
    (void)kiss_seed_from_entropy(ent, 32, want, sizeof want);
    tchk("seedkeeper: load a master seed by its entropy (24 words, seed checked)",
         kiss_sk_load(&c, 2, words, sizeof words, pass, sizeof pass) == SC_OK &&
         strcmp(words, want) == 0 && pass[0] == 0);
    tchk("seedkeeper: a master seed that disagrees with its words is refused",
         kiss_sk_load(&c, 3, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT &&
         words[0] == 0);
    tchk("seedkeeper: an entry the card will not export",
         kiss_sk_load(&c, 4, words, sizeof words, pass, sizeof pass) == SC_ERR_DENIED);
    tchk("seedkeeper: a multi-chunk export",
         kiss_sk_load(&c, 5, words, sizeof words, pass, sizeof pass) == SC_OK &&
         strcmp(words, DEV_WORDS) == 0);
    tchk("seedkeeper: a password entry is not a seed",
         kiss_sk_load(&c, 6, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT);
    fc.tamper_export = true;
    tchk("seedkeeper: an export changed after signing is refused",
         kiss_sk_load(&c, 1, words, sizeof words, pass, sizeof pass) == SC_ERR_NOT_GENUINE &&
         words[0] == 0);
    kiss_satocard_close(&c);

    card_new(&fc, SC_APPLET_SEEDKEEPER);
    fc.channel_by_rogue = true;
    n = bip39_entry(buf, ALT_WORDS, "", 0);
    add_secret(&fc, 1, SK_TYPE_BIP39, 0, 1, "main", buf, n);
    tchk("seedkeeper: a reader in the middle is refused", unlock(&c, &fc) == SC_ERR_NOT_GENUINE);
    tchk("seedkeeper: and nothing exports",
         kiss_sk_load(&c, 1, words, sizeof words, pass, sizeof pass) == SC_ERR_ORDER);

    // decode on its own: the shapes that must be refused
    const char *nonascii = "p\xc3\xa4ss";
    n = bip39_entry(buf, ALT_WORDS, nonascii, 0);
    tchk("decode: a passphrase this device cannot type is refused",
         kiss_sk_decode(SK_TYPE_BIP39, 0, buf, n, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT);
    const char *w18 = "abandon abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon agent";
    n = bip39_entry(buf, w18, "", 0);
    tchk("decode: 18 words refused",
         kiss_sk_decode(SK_TYPE_BIP39, 0, buf, n, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT);
    n = bip39_entry(buf, ALT_WORDS, "", 0);
    buf[0] += 3;
    tchk("decode: a length running past the data refused",
         kiss_sk_decode(SK_TYPE_BIP39, 0, buf, n, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT);
    n = masterseed_entry(buf, ent, 32, "", false);
    buf[1 + 64] = 0x01;     // a non-English wordlist
    tchk("decode: a non-English wordlist refused",
         kiss_sk_decode(SK_TYPE_MASTERSEED, SK_SUBTYPE_BIP39, buf, n, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT);
    tchk("decode: an Electrum seed is not BIP39",
         kiss_sk_decode(SK_TYPE_ELECTRUM, 0, buf, n, words, sizeof words, pass, sizeof pass) == SC_ERR_FORMAT);
    kiss_satocard_close(&c);
}

// ---- for test_crypto.c: a card session on the emulated card ----
// The card holds the dev seed, so its wallet is the one the PSBT fixtures
// are built for, and seed mode and card mode can be compared output for
// output.
static fake_card s_sess_card;

int test_satocard_open_session(void)
{
    if (!T_CTX)
        T_CTX = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t anchor[65];
    unhex(SUBCA_PUB, anchor, 65);
    card_new(&s_sess_card, SC_APPLET_SATOCHIP);
    s_sess_card.high_s = true;
    kiss_satocard_test_set_anchor(anchor);
    int tries = 0, rc = kiss_cardwallet_connect(fake_xfer, &s_sess_card, fixed_rng, &tries);
    if (rc == SC_OK)
        rc = kiss_cardwallet_unlock("123456", 6, NULL, NULL);
    kiss_satocard_test_set_anchor(NULL);
    if (rc == SC_OK)
        rc = kiss_session_open_card();
    return rc;
}

// A fresh emulated Satochip holding the dev seed, reached one APDU at a time
// (for the CCID tests, which put a reader between it and the card layers).
void test_satocard_card_reset(void)
{
    if (!T_CTX)
        T_CTX = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    card_new(&s_sess_card, SC_APPLET_SATOCHIP);
}

int test_satocard_apdu(const uint8_t *cmd, size_t len, uint8_t *resp, size_t cap,
                       size_t *rlen, uint16_t *sw)
{
    return fake_xfer(&s_sess_card, cmd, len, resp, cap, rlen, sw);
}

void test_satocard_anchor(int on)
{
    uint8_t anchor[65];
    unhex(SUBCA_PUB, anchor, 65);
    kiss_satocard_test_set_anchor(on ? anchor : NULL);
}

// 0 none, 1 the card is pulled, 2 the card signs with a different key
void test_satocard_fault(int fault)
{
    s_sess_card.fail_io = fault == 1;
    s_sess_card.sign_wrong_key = fault == 2;
}

int test_satocard(void)
{
    tfails = 0;
    T_CTX = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t anchor[65];
    unhex(SUBCA_PUB, anchor, 65);

    test_primitives();
    test_cert();
    kiss_satocard_test_set_anchor(anchor);
    test_channel_vector();
    test_session_rules();
    test_satochip();
    test_seedkeeper();
    kiss_satocard_test_set_anchor(NULL);

    return tfails;
}
