// Sealed seed blob for the SD card. See wallet_seed_sd.h for the layout and
// for why the key never leaves this device.
#include "wallet_seed_sd.h"

#include <stdio.h>
#include <string.h>

#include "wally_core.h"
#include "wally_crypto.h"

#include "wallet_crypto.h"   // wallet_trng_live: the chip RNG is not on by default

#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#else
#include <errno.h>
#include <stdlib.h>
#define DKEY_FILE "/tmp/kiss_device_key.bin"
#endif

// Two independent subkeys from the one device key, so the cipher and the MAC
// never share bytes. Domain strings carry the version: a future format can
// derive different subkeys from the same stored device key.
#define ENC_INFO "kiss-sd-enc-v1"
#define MAC_INFO "kiss-sd-mac-v1"

static void sd_bzero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}

// Length-independent compare. A MAC check that returns early on the first
// differing byte tells an attacker how much of a forged tag was right.
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// Everything random on this card comes through here: the device key, and the
// IV of every blob written under it. Never the chip on its own.
//
// The seed screens fold three sources because the owner is present to supply
// two of them. Nothing here has that: the key is minted the first time a card
// is written, and an IV is minted mid write. So the second source is the one
// that needs no owner, timing jitter (wallet_crypto.h), and the shape is the
// same as the seed's — SHA256(chip || jitter), no weaker than the chip alone.
//
// The wallet_trng_live gate stays, and is not made redundant by the fold. The
// two answer different questions. The gate is provenance: esp_fill_random
// reports success whether or not a noise source is behind it, so key material
// must refuse to exist while nobody has switched one on. The fold is the case
// the gate cannot see, a source that is on and still returning something
// degenerate. A key that failed both would pass every check downstream of
// here, which is why neither is dropped for the other.
static int fill_random(uint8_t *out, size_t len)
{
    if (!out || len > 32) return -1;      // key is 32, iv is 16; nothing else
    uint8_t chip[32], jit[32], mixed[32];
    int rc;

#ifdef ESP_PLATFORM
    if (!wallet_trng_live()) return -1;
    esp_fill_random(chip, sizeof chip);
    rc = 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    rc = fread(chip, 1, sizeof chip, f) == sizeof chip ? 0 : -1;
    fclose(f);
#endif

    if (rc == 0) rc = wallet_jitter(jit);
    if (rc == 0) rc = wallet_entropy_mix(chip, jit, mixed);
    if (rc == 0) memcpy(out, mixed, len);

    sd_bzero(chip, sizeof chip);
    sd_bzero(jit, sizeof jit);
    sd_bzero(mixed, sizeof mixed);
    return rc;
}

static void subkeys(const uint8_t key32[32], uint8_t enc[32], uint8_t mac[32])
{
    wally_hmac_sha256(key32, 32, (const uint8_t *)ENC_INFO, sizeof ENC_INFO - 1,
                      enc, 32);
    wally_hmac_sha256(key32, 32, (const uint8_t *)MAC_INFO, sizeof MAC_INFO - 1,
                      mac, 32);
}

// ---- the device key ----------------------------------------------------
// Generated once, kept here, never shown. On a release build NVS itself is
// encrypted (tools/build_encrypted_release.sh), so a flash dump does not
// yield it; on a dev build it is plaintext, which is exactly why the storage
// screen must not present the three options as equally safe.
int sd_seed_device_key(uint8_t key32[32])
{
    if (!key32) return -1;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    size_t n = 32;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t err = nvs_get_blob(h, "dkey", key32, &n);
    if (err == ESP_OK && n == 32) {
        nvs_close(h);
        return 0;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {     // a real read failure, not "first use"
        nvs_close(h);
        return -1;
    }
    if (fill_random(key32, 32) != 0) {
        nvs_close(h);
        return -1;
    }
    int rc = nvs_set_blob(h, "dkey", key32, 32) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    if (rc != 0) {
        sd_bzero(key32, 32);
        return -1;
    }
    return 0;
#else
    FILE *f = fopen(DKEY_FILE, "rb");
    if (f) {
        size_t n = fread(key32, 1, 32, f);
        fclose(f);
        if (n == 32) return 0;
    }
    if (fill_random(key32, 32) != 0)
        return -1;
    f = fopen(DKEY_FILE, "wb");
    if (!f) { sd_bzero(key32, 32); return -1; }
    int rc = fwrite(key32, 1, 32, f) == 32 ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) sd_bzero(key32, 32);
    return rc;
#endif
}

int sd_seed_forget_device_key(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t err = nvs_erase_key(h, "dkey");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return -1;
    }
    int rc = nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
#else
    return remove(DKEY_FILE) == 0 || errno == ENOENT ? 0 : -1;
#endif
}

// ---- seal / open -------------------------------------------------------
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int sd_seed_seal(const uint8_t key32[32], const char *mnemonic,
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t enc[32], mac[32];
    size_t written = 0;
    int ret = -1;

    if (out_len) *out_len = 0;
    if (!key32 || !mnemonic || !out || !out_len) return -1;

    size_t m_len = strlen(mnemonic);
    if (m_len == 0 || m_len >= WSEED_SD_MAX_PLAIN) return -1;

    // PKCS7 always adds a block when the input is already aligned, so the
    // ciphertext is strictly longer than the plaintext.
    size_t ct_max = ((m_len / AES_BLOCK_LEN) + 1) * AES_BLOCK_LEN;
    if (out_cap < SDSEED_HDR_LEN + ct_max + SDSEED_TAG_LEN) return -1;

    subkeys(key32, enc, mac);
    memcpy(out, SDSEED_MAGIC, SDSEED_MAGIC_LEN);
    if (fill_random(out + SDSEED_MAGIC_LEN, SDSEED_IV_LEN) != 0) goto out;

    if (wally_aes_cbc(enc, 32, out + SDSEED_MAGIC_LEN, SDSEED_IV_LEN,
                      (const uint8_t *)mnemonic, m_len, AES_FLAG_ENCRYPT,
                      out + SDSEED_HDR_LEN, ct_max, &written) != WALLY_OK)
        goto out;
    if (written == 0 || written % AES_BLOCK_LEN != 0) goto out;

    put_le32(out + SDSEED_MAGIC_LEN + SDSEED_IV_LEN, (uint32_t)written);
    if (wally_hmac_sha256(mac, 32, out, SDSEED_HDR_LEN + written,
                          out + SDSEED_HDR_LEN + written, SDSEED_TAG_LEN) != WALLY_OK)
        goto out;

    *out_len = SDSEED_HDR_LEN + written + SDSEED_TAG_LEN;
    ret = 0;
out:
    sd_bzero(enc, sizeof enc);
    sd_bzero(mac, sizeof mac);
    if (ret != 0) {
        sd_bzero(out, out_cap);
        *out_len = 0;
    }
    return ret;
}

int sd_seed_open(const uint8_t key32[32], const uint8_t *blob, size_t blob_len,
                 char *out, size_t out_cap)
{
    uint8_t enc[32], mac[32], tag[SDSEED_TAG_LEN];
    uint8_t plain[WSEED_SD_MAX_PLAIN + AES_BLOCK_LEN];
    size_t written = 0;
    int ret = -1;

    if (!out || !out_cap) return -1;
    sd_bzero(out, out_cap);
    if (!key32 || !blob) return -1;

    // Header, magic, then the declared length: all three before the key is
    // touched, so a malformed file costs nothing.
    if (blob_len < SDSEED_HDR_LEN + AES_BLOCK_LEN + SDSEED_TAG_LEN) return -1;
    if (memcmp(blob, SDSEED_MAGIC, SDSEED_MAGIC_LEN) != 0) return -1;

    uint32_t ct_len = get_le32(blob + SDSEED_MAGIC_LEN + SDSEED_IV_LEN);
    if (ct_len == 0 || ct_len % AES_BLOCK_LEN != 0) return -1;
    if (ct_len > sizeof plain) return -1;
    if ((size_t)SDSEED_HDR_LEN + ct_len + SDSEED_TAG_LEN != blob_len) return -1;

    subkeys(key32, enc, mac);

    // Verify BEFORE decrypting: a wrong device key or a tampered card is a
    // MAC failure, and the cipher never sees attacker-chosen bytes.
    if (wally_hmac_sha256(mac, 32, blob, SDSEED_HDR_LEN + ct_len,
                          tag, sizeof tag) != WALLY_OK)
        goto out;
    if (!ct_equal(tag, blob + SDSEED_HDR_LEN + ct_len, SDSEED_TAG_LEN))
        goto out;

    if (wally_aes_cbc(enc, 32, blob + SDSEED_MAGIC_LEN, SDSEED_IV_LEN,
                      blob + SDSEED_HDR_LEN, ct_len, AES_FLAG_DECRYPT,
                      plain, sizeof plain, &written) != WALLY_OK)
        goto out;
    if (written == 0 || written >= out_cap) goto out;

    memcpy(out, plain, written);
    out[written] = '\0';
    ret = 0;
out:
    sd_bzero(enc, sizeof enc);
    sd_bzero(mac, sizeof mac);
    sd_bzero(tag, sizeof tag);
    sd_bzero(plain, sizeof plain);
    if (ret != 0) sd_bzero(out, out_cap);
    return ret;
}
