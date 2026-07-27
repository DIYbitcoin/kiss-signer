// Desktop tests for main/wallet_seed_sd.c: the sealed seed blob written to
// the SD card.
//
// The whole security claim of SD mode is "the card alone is useless", so
// these tests are mostly about rejection: a wrong device key, a flipped byte
// anywhere in the blob, a truncated file, a lying length field. Every one of
// them must fail closed with the output buffer zeroed, because the caller
// hands that buffer straight to the BIP39 layer.
#include <stdio.h>
#include <string.h>

#include "wallet_seed.h"
#include "wallet_seed_sd.h"

#define SD_WORDS "abandon abandon abandon abandon abandon abandon " \
                 "abandon abandon abandon abandon abandon about"
#define SD_WORDS24 "legal winner thank year wave sausage worth useful legal " \
                   "winner thank yellow legal winner thank year wave sausage " \
                   "worth useful legal winner thank yellow"

static int dfails;

static void dchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); dfails++; }
}

static int all_zero(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

int test_sdseed_layer(void) {
    uint8_t key[32], key2[32];
    uint8_t blob[SDSEED_MAX_BLOB], blob2[SDSEED_MAX_BLOB];
    size_t len = 0, len2 = 0;
    char got[WSEED_MAX_MNEMONIC];

    // ---- the device key ----
    dchk("device key reads", sd_seed_device_key(key) == 0);
    dchk("device key is not all zero", !all_zero(key, sizeof key));
    dchk("device key is stable across calls",
         sd_seed_device_key(key2) == 0 && memcmp(key, key2, 32) == 0);

    // ---- roundtrip ----
    dchk("seal 12 words", sd_seed_seal(key, SD_WORDS, blob, sizeof blob, &len) == 0);
    dchk("blob carries the magic", memcmp(blob, SDSEED_MAGIC, SDSEED_MAGIC_LEN) == 0);
    dchk("blob is header + ciphertext + tag",
         len > SDSEED_HDR_LEN + SDSEED_TAG_LEN && len <= SDSEED_MAX_BLOB);
    dchk("the mnemonic is not sitting in the blob in clear",
         memmem(blob, len, "abandon", 7) == NULL);
    dchk("open 12 words", sd_seed_open(key, blob, len, got, sizeof got) == 0);
    dchk("12 words round-trip exactly", strcmp(got, SD_WORDS) == 0);

    dchk("seal 24 words", sd_seed_seal(key, SD_WORDS24, blob2, sizeof blob2, &len2) == 0);
    dchk("open 24 words", sd_seed_open(key, blob2, len2, got, sizeof got) == 0);
    dchk("24 words round-trip exactly", strcmp(got, SD_WORDS24) == 0);

    // A fresh iv every write, so two seals of the same words are not the same
    // bytes on the card. Otherwise anyone holding two cards learns whether
    // they carry the same wallet without decrypting either.
    {
        uint8_t again[SDSEED_MAX_BLOB];
        size_t again_len = 0;
        dchk("seal the same words twice",
             sd_seed_seal(key, SD_WORDS, again, sizeof again, &again_len) == 0);
        dchk("two seals of one mnemonic differ (fresh iv)",
             again_len == len && memcmp(again, blob, len) != 0);
    }

    // ---- a wrong device key ----
    {
        uint8_t wrong[32];
        memcpy(wrong, key, 32);
        wrong[31] ^= 0x01;                 // one bit
        memset(got, 'x', sizeof got);
        dchk("wrong device key is refused",
             sd_seed_open(wrong, blob, len, got, sizeof got) != 0);
        dchk("wrong device key leaks nothing", all_zero(got, sizeof got));
    }

    // ---- every single-byte corruption ----
    {
        int bad = 0, leaked = 0;
        for (size_t i = 0; i < len; i++) {
            uint8_t save = blob[i];
            blob[i] ^= 0x40;
            memset(got, 'x', sizeof got);
            if (sd_seed_open(key, blob, len, got, sizeof got) == 0) bad++;
            else if (!all_zero(got, sizeof got)) leaked++;
            blob[i] = save;
        }
        dchk("every single-byte flip is rejected", bad == 0);
        dchk("no rejected blob leaves plaintext behind", leaked == 0);
    }

    // ---- truncation at every offset ----
    {
        int bad = 0;
        for (size_t n = 0; n < len; n++) {
            memset(got, 'x', sizeof got);
            if (sd_seed_open(key, blob, n, got, sizeof got) == 0) bad++;
        }
        dchk("every truncation is rejected", bad == 0);
    }

    // ---- a lying length field ----
    {
        uint8_t l[SDSEED_MAX_BLOB];
        memcpy(l, blob, len);
        l[24] = 0xff; l[25] = 0xff; l[26] = 0xff; l[27] = 0xff;
        memset(got, 'x', sizeof got);
        dchk("a huge declared length is refused",
             sd_seed_open(key, l, len, got, sizeof got) != 0);
        memcpy(l, blob, len);
        l[24] = l[25] = l[26] = l[27] = 0;      // zero-length ciphertext
        memset(got, 'x', sizeof got);
        dchk("a zero declared length is refused",
             sd_seed_open(key, l, len, got, sizeof got) != 0);
        memcpy(l, blob, len);
        l[24] = 7;                              // not a whole AES block
        memset(got, 'x', sizeof got);
        dchk("a non-block-multiple length is refused",
             sd_seed_open(key, l, len, got, sizeof got) != 0);
    }

    // A big, internally consistent file on the card. The header agrees with
    // its own size, so only the ciphertext bound stops this: nothing about a
    // 4KB "mnemonic" should ever reach the cipher.
    {
        static uint8_t big[4096];
        memcpy(big, blob, SDSEED_HDR_LEN);
        size_t ct = sizeof big - SDSEED_HDR_LEN - SDSEED_TAG_LEN;
        ct -= ct % 16;
        big[24] = (uint8_t)ct; big[25] = (uint8_t)(ct >> 8);
        big[26] = (uint8_t)(ct >> 16); big[27] = (uint8_t)(ct >> 24);
        memset(got, 'x', sizeof got);
        dchk("an oversized ciphertext is refused",
             sd_seed_open(key, big, SDSEED_HDR_LEN + ct + SDSEED_TAG_LEN,
                          got, sizeof got) != 0);
        dchk("an oversized blob leaks nothing", all_zero(got, sizeof got));
    }

    // ---- a foreign magic ----
    {
        uint8_t m[SDSEED_MAX_BLOB];
        memcpy(m, blob, len);
        m[7] = '2';                             // KISSSD02, a format we do not know
        memset(got, 'x', sizeof got);
        dchk("an unknown format version is refused",
             sd_seed_open(key, m, len, got, sizeof got) != 0);
    }

    // ---- caller buffer too small ----
    {
        char tiny[8];
        memset(tiny, 'x', sizeof tiny);
        dchk("a too-small output buffer is refused",
             sd_seed_open(key, blob, len, tiny, sizeof tiny) != 0);
        dchk("a refused open still zeroes the small buffer",
             all_zero(tiny, sizeof tiny));
    }

    // ---- seal refuses what it cannot fit ----
    {
        uint8_t small[32];
        size_t n = 12345;
        dchk("seal refuses a buffer it would overrun",
             sd_seed_seal(key, SD_WORDS, small, sizeof small, &n) != 0);
        dchk("a refused seal reports no length", n == 0);
    }

    return dfails;
}
