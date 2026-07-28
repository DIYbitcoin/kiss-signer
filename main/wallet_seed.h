// Step 7: the user's own seed. The mnemonic lives in flash (NVS) on the
// device — plaintext until step 8 turns flash encryption on — and in a plain
// file for desktop tests. The passphrase is NEVER stored anywhere (that is
// the whole deniability model).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WSEED_MAX_MNEMONIC 256   // 24 words comfortably
#define WSEED_MAX_WORDS     24   // BIP39 tops out here; callers clamp to it

// ---- storage mode ----
// KEEP:    the mnemonic lives in flash/NVS. Unlock is passphrase only.
// AMNESIC: nothing is ever written. Every session is: load the seed (type the
//          words or scan a QR you made elsewhere), passphrase, sign, power off.
//          RAM is the only copy, so wallet_session_close() takes it with it and
//          a device that crosses a border holds no wallet bytes at all.
// SD:      the mnemonic is a sealed file on the card. Its device key lives in
//          encrypted NVS, so the card and this signer are both required.
#define WSEED_MODE_KEEP    0
#define WSEED_MODE_AMNESIC 1
#define WSEED_MODE_SD      2
#define WSEED_MODE_INVALID (-1)  // corrupt/unreadable metadata; never factory-fresh

// Public storage results. Existing callers may continue treating any nonzero
// value as failure; Settings uses the distinct values to say what is actionable.
#define WSEED_OK                  0
#define WSEED_ERR_INVALID        -1
#define WSEED_ERR_NO_SEED        -2
#define WSEED_ERR_SD_MISSING     -3
#define WSEED_ERR_SD_IO          -4
#define WSEED_ERR_SD_CORRUPT     -5
#define WSEED_ERR_VERIFY         -6
#define WSEED_ERR_CLEANUP        -7
#define WSEED_ERR_ROLLBACK       -8

// Is the seed at rest actually encrypted (flash encryption burned in eFuse)?
// Only the storage NOTES need this, to tell the truth about what a chip dump
// would reveal. Host/simulator: false, since the desktop store is a plain file.
int wallet_seed_flash_encrypted(void);

// The staged choice if setup is mid-flight, otherwise what flash holds.
int  wallet_seed_mode(void);
// Persists the choice IMMEDIATELY. Switching TO amnesic wipes any stored seed,
// so the mode shown on screen is always the truth about what is on the device.
// Not for the setup wizard: see wallet_seed_stage_mode.
// Returns 0 only after the write/erase has been verified.
int wallet_seed_set_mode(int mode);
// Migrate an existing wallet. The destination is written, read back, validated
// and byte-compared before the source is removed. AMNESIC remains in RAM until
// wallet_session_close(). WSEED_ERR_CLEANUP means the verified destination is
// active but an old-source artifact could not be removed. WSEED_ERR_ROLLBACK
// means the original source is still active but an uncommitted destination
// artifact could not be removed.
int wallet_seed_move_to(int mode);
// The wizard's version: remember the answer, touch nothing. The wizard asks
// KEEP vs NOTHING SAVED before the new wallet exists, so applying it there
// would erase a wallet the user might still back out and keep.
// wallet_seed_commit applies it; wallet_seed_discard forgets it.
void wallet_seed_stage_mode(int mode);

// 1 if a wallet is configured. In SD mode this intentionally stays 1 while the
// card is absent/corrupt, so boot cannot mistake it for a factory-fresh device.
int wallet_seed_exists(void);

// Validate (BIP39 checksum, wordlist) and persist. 0 on success.
int wallet_seed_store(const char *mnemonic);

// Two-phase persistence for setup (P0 safety net). The wizard STAGES a
// validated mnemonic in RAM; it is only written to flash by wallet_seed_commit
// once the full setup ritual (passphrase typed twice + fingerprint recorded)
// finishes. Cancelling anywhere before commit calls wallet_seed_discard, so an
// abandoned setup leaves no half-made wallet. While staged, load/exists/session
// all see the staged mnemonic (so the fingerprint can be shown pre-commit).
int wallet_seed_stage(const char *mnemonic);   // validate + hold in RAM
int wallet_seed_commit(void);                  // staged RAM + mode -> flash
void wallet_seed_discard(void);                // drop the staged mnemonic + mode

// Copy the stored mnemonic into out. 0 on success, nonzero if none/too small.
int wallet_seed_load(char *out, size_t out_len);

// Erase the stored seed. 0 on success (also 0 when nothing was stored).
int wallet_seed_wipe(void);

// Lock hook, called from wallet_session_close(). In AMNESIC mode the staged
// mnemonic IS the wallet, so locking has to drop it; in KEEP mode this does
// nothing (flash still holds the words, which is the point of that mode).
void wallet_seed_forget(void);

// BIP39 checksum + wordlist validation only (nothing stored). 0 = valid.
int wallet_seed_validate(const char *mnemonic);

// Entropy -> mnemonic words. len must be 16 (12 words) or 32 (24 words).
// 0 on success; out is NUL-terminated.
int wallet_seed_from_entropy(const uint8_t *entropy, size_t len,
                             char *out, size_t out_len);

// ---- QR seed import ----
// One decoded QR payload -> a checksum-valid mnemonic. Three shapes, told
// apart by content (they cannot collide: the digit forms are 48/96 bytes, the
// binary forms 16/32):
//   * plain text mnemonic, as a wallet or a text QR would write it
//   * numeric SeedQR (SeedSigner/Krux): 48 or 96 ASCII digits, four per
//     wordlist index, zero padded
//   * CompactSeedQR: 16 or 32 raw entropy bytes
// data may contain NULs, so len is authoritative. 0 on success; out is always
// NUL-terminated and is CLEARED on any failure (never a stale half-seed).
// KISS deliberately has no matching export: it reads seed QRs, never makes one.
int wallet_seed_from_qr(const char *data, size_t len, char *out, size_t out_len);

// ---- wordlist access (RESTORE autocomplete + prove-backup quiz decoys) ----
// Up to n wordlist words starting with prefix into out[]; returns the count.
// Pointers are into the static wordlist — do not free.
int wallet_seed_suggest(const char *prefix, const char *out[], int n);

// Wordlist word by index (0..2047). 0 on success.
int wallet_seed_word(int index, const char **out);

// ---- backup verification ----
// Compare two space-separated mnemonics word by word. Returns the 0-based index
// of the FIRST differing word (so the UI can say "word #N"), or -1 if identical.
// A different word count counts as a mismatch at the first missing/extra word.
// Reveals only the position, never the correct word.
int wallet_seed_diff_word(const char *typed, const char *stored);

#ifndef ESP_PLATFORM
// Host-only persistence fault seam used by transition tests.
#define WSEED_TEST_FAIL_MODE_WRITE  (1u << 0)
#define WSEED_TEST_FAIL_SEED_REMOVE (1u << 1)
void wallet_seed_test_fail_next(unsigned flags);
#endif
