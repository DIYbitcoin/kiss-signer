// Step 7: the user's own seed. The mnemonic lives in flash (NVS) on the
// device — plaintext until step 8 turns flash encryption on — and in a plain
// file for desktop tests. The passphrase is NEVER stored anywhere (that is
// the whole deniability model).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WSEED_MAX_MNEMONIC 256   // 24 words comfortably

// ---- storage mode ----
// KEEP:    the mnemonic lives in flash. Unlock is passphrase only.
// AMNESIC: nothing is ever written. Every session is: load the seed (type the
//          words or scan a QR you made elsewhere), passphrase, sign, power off.
//          RAM is the only copy, so wallet_session_close() takes it with it and
//          a device that crosses a border holds no wallet bytes at all.
#define WSEED_MODE_KEEP    0
#define WSEED_MODE_AMNESIC 1

int  wallet_seed_mode(void);
// Persists the choice. Switching TO amnesic wipes any stored seed, so the mode
// shown on screen is always the truth about what is on the device.
void wallet_seed_set_mode(int mode);

// 1 if a seed is stored on this device.
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
int wallet_seed_commit(void);                  // staged RAM -> flash
void wallet_seed_discard(void);                // drop the staged mnemonic

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
