// KEF (Krux Encryption Format) envelope: the password-locked backup.
//
// Unlike kiss-seed.enc (kiss_seed_sd.h), which is keyed by a device key that
// never leaves the box, a KEF envelope is keyed by a password the owner
// carries in their head. That is the whole point: the QR or .kef file can sit
// in a drawer, a phone photo or someone else's pocket and stays ciphertext.
// It is a public cross-project format — a KISS backup opens on a Krux device
// and a Krux backup opens here.
//
// Format, byte for byte (all integers big endian):
//
//   off        len       field
//     0          1       len_id, 0..252
//     1     len_id       id — the PBKDF2 salt, shown in the clear. KISS
//                        writes the master fingerprint as 8 ASCII hex chars.
//   1+len_id     1       version. KISS makes only 20 (AES-256-GCM).
//   2+len_id     3       iterations, stored form (see below)
//   5+len_id     N       payload; for version 20:
//                          iv(12) | ciphertext | tag(4)
//
// key = PBKDF2-HMAC-SHA256(password, id, effective_iterations), 32 bytes.
// The tag is the AES-GCM tag over the ciphertext alone (no AAD), truncated
// to its first 4 bytes. No padding, no compression in version 20.
//
// Iterations, stored form: a value v <= 10000 means v * 10000 effective
// (Krux stores its 100,000 default as 10); a value > 10000 is the effective
// count itself. Open refuses anything effective below 10,000 — a hostile
// envelope must not get to choose a trivial work factor — and anything above
// KEF_MAX_EFF_ITER, so a 3-byte field cannot demand minutes of PBKDF2 on a
// 400MHz chip. The cap is a deliberate interop cut: the format allows up to
// 100M, Krux's own UI stays far below the cap.
//
// Encrypt strict, decrypt vague (per the KEF spec): seal makes only version
// 20 and fails loudly; open refuses every other version, every malformed
// header and every wrong password through the SAME single failure code, with
// the output buffer zeroed. Nothing downstream can tell which check failed,
// so nothing downstream can leak it.
#ifndef KISS_KEF_H
#define KISS_KEF_H

#include <stddef.h>
#include <stdint.h>

#define KEF_VERSION_AES_GCM 20
#define KEF_ID_MAX          252
#define KEF_ITER_STORED     10u        // what seal writes: 100,000 effective
#define KEF_MIN_EFF_ITER    10000u
#define KEF_MAX_EFF_ITER    1000000u
#define KEF_IV_LEN          12
#define KEF_TAG_LEN         4
// Largest envelope the UI ever handles (a seed one is ~61 bytes; leave room
// for other tools' longer ids and text payloads without approaching the
// 2560-byte single-QR ceiling).
#define KEF_MAX_ENV         512

typedef struct {
    const uint8_t *id;                 // points into the parsed buffer
    uint8_t        id_len;
    uint8_t        version;
    uint32_t       iter_raw;           // the stored 3-byte value
    uint32_t       iter_eff;           // after the *10000 rule
    const uint8_t *payload;            // points into the parsed buffer
    size_t         payload_len;
} kef_env_t;

// ---- pure half (kiss_kef.c): no crypto, compiled into every build --------

// Structural parse. 0 = a well formed envelope of a KNOWN version (out
// filled, pointers into buf); -1 otherwise. Mirrors the reference unwrap:
// unknown version bytes, impossible lengths, per-version payload floors and
// an effective iteration count below KEF_MIN_EFF_ITER are all -1. The
// KEF_MAX_EFF_ITER cap is NOT applied here — an over-cap envelope is still
// structurally KEF, and the caller wants to know that to refuse it as one.
int kef_parse(const uint8_t *buf, size_t len, kef_env_t *out);

// 1 = structurally KEF and worth routing to the password/refusal path;
// 0 = not KEF, and since the seed-QR door was removed that means the scan is
// refused outright. Lengths 16 and 32 are 0 unconditionally: those are the
// lengths of an envelope's own PLAINTEXT (raw BIP39 entropy), and no KEF
// envelope that small can hold a seed, so an opened backup can never be
// mistaken for another envelope. Every byte of a text mnemonic is >= 0x20 and
// every KEF version byte is < 0x20, so the other plaintext shape cannot
// collide either (both pinned by tests).
int kef_sniff(const uint8_t *buf, size_t len);

// Writes the 5+id_len byte header. Returns its length, or 0 if it does not
// fit or id_len > KEF_ID_MAX. iter_raw is written as stored form, verbatim.
size_t kef_emit_header(uint8_t *out, size_t cap, const uint8_t *id,
                       size_t id_len, uint8_t version, uint32_t iter_raw);

// ---- crypto half (kiss_kef_crypto.c): firmware + native tests ------------
// The UI sim stubs these in sim/sim_main.c, like every other crypto seam.
// Both return 0 or -1. One failure code; outputs zeroed on every failure.

int kiss_kef_seal(const uint8_t *id, size_t id_len,
                  const char *password, size_t pass_len,
                  const uint8_t *plain, size_t plain_len,
                  uint8_t *out, size_t out_cap, size_t *out_len);

int kiss_kef_open(const char *password, size_t pass_len,
                  const uint8_t *env, size_t env_len,
                  uint8_t *plain, size_t plain_cap, size_t *plain_len);

// The backup the UI makes: mnemonic -> BIP39 entropy bytes (what Krux stores,
// so either device restores the other's backup), id = the master fingerprint
// as 8 uppercase hex chars, which is also returned for display.
int kiss_kef_seal_seed(const char *mnemonic, const char *password,
                       size_t pass_len, uint8_t *out, size_t out_cap,
                       size_t *out_len, char id_hex_out[9]);

#ifndef ESP_PLATFORM
// Host tests only: pin the next seal's IV so golden vectors reproduce, and
// reach the raw GCM (keystream + full 16-byte tag) so the spec vectors can
// pin it without PBKDF2 in the way.
void kiss_kef_test_fix_iv(const uint8_t iv[KEF_IV_LEN]);
int kiss_kef_test_gcm(const uint8_t key[32], const uint8_t iv[KEF_IV_LEN],
                      const uint8_t *pt, size_t n, uint8_t *ct,
                      uint8_t tag[16]);
#endif

#endif
