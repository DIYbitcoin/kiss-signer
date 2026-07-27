// Sealed seed blob for the SD card.
//
// The card is a second factor, not a backup and not a password: the key lives
// on this device and is never shown, so the card alone is ciphertext and the
// card dies with the device. Paper stays the only backup, which is why the UI
// makes the user confirm they have one before choosing this mode.
//
// Format, deliberately boring:
//
//   off  len  field
//     0    8  magic "KISSSD01"
//     8   16  iv, fresh random per write
//    24    4  ciphertext length, little endian
//    28    N  AES-256-CBC(k_enc, iv) over the mnemonic, PKCS7 padded
//  28+N   32  HMAC-SHA256(k_mac) over bytes [0, 28+N)
//
// Encrypt then MAC: sd_seed_open verifies the tag over the whole header and
// ciphertext BEFORE it decrypts anything, so a tampered blob never reaches
// the cipher and a wrong device key is a MAC failure rather than garbage
// plaintext.
#ifndef WALLET_SEED_SD_H
#define WALLET_SEED_SD_H

#include <stddef.h>
#include <stdint.h>

// The longest thing that is ever sealed: a 24-word mnemonic is 215 bytes.
// Anything longer is not a BIP39 mnemonic and seal refuses it rather than
// silently truncating a wallet onto the card.
#define WSEED_SD_MAX_PLAIN 256

#define SDSEED_MAGIC     "KISSSD01"
#define SDSEED_MAGIC_LEN 8
#define SDSEED_IV_LEN    16
#define SDSEED_HDR_LEN   28          // magic + iv + length
#define SDSEED_TAG_LEN   32
// 24 words is 215 bytes at most; one AES block of padding on top, and the
// header and tag around it.
#define SDSEED_MAX_BLOB  (SDSEED_HDR_LEN + 272 + SDSEED_TAG_LEN)

// Seals `mnemonic` under the 32-byte device key. Returns 0 and writes
// *out_len bytes, or -1 (output buffer zeroed).
int sd_seed_seal(const uint8_t key32[32], const char *mnemonic,
                 uint8_t *out, size_t out_cap, size_t *out_len);

// Opens a blob. Returns 0 and a NUL-terminated mnemonic, or -1 with `out`
// zeroed. Every failure path zeroes `out`: a caller must never be able to
// read half-decrypted bytes out of it after a rejection.
int sd_seed_open(const uint8_t key32[32], const uint8_t *blob, size_t blob_len,
                 char *out, size_t out_cap);

// The device key: 32 random bytes generated once and kept on this device
// (NVS today, an eFuse-backed HMAC key later without changing the format
// above). Returns 0 on success. Never displayed, never exported.
int sd_seed_device_key(uint8_t key32[32]);

// Destroys the device key, which makes every card ever written by this device
// permanently unreadable. WIPE calls this, and that is what makes "the words
// go from this device now" true even when the card is not in the slot.
int sd_seed_forget_device_key(void);

#endif
