// Step 7: the user's own seed. The mnemonic lives in flash (NVS) on the
// device — plaintext until step 8 turns flash encryption on — and in a plain
// file for desktop tests. The passphrase is NEVER stored anywhere (that is
// the whole deniability model).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WSEED_MAX_MNEMONIC 256   // 24 words comfortably

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

// BIP39 checksum + wordlist validation only (nothing stored). 0 = valid.
int wallet_seed_validate(const char *mnemonic);

// Entropy -> mnemonic words. len must be 16 (12 words) or 32 (24 words).
// 0 on success; out is NUL-terminated.
int wallet_seed_from_entropy(const uint8_t *entropy, size_t len,
                             char *out, size_t out_len);

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
