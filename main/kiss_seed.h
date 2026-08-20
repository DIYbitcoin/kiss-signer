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
//          RAM is the only copy, so kiss_session_close() takes it with it and
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
// The one result a caller must NOT answer by discarding what it staged.
//
// Returned when a KEEP wallet was replacing another and the replacement may
// have taken the old one with it. Two ways in, both on that transition:
//
//   * the residue scrub erased the store and could not write the mnemonic back
//   * storage_write_keep committed the new blob over the old one and then
//     failed its readback (WSEED_ERR_VERIFY), so neither is readable
//
// Either way flash may now hold no usable wallet, the staged copy in RAM is the
// last one in existence, and throwing it away is the loss itself rather than
// the report of one.
//
// Every OTHER nonzero result means the write never reached the old wallet, or
// there was no old wallet, so staging is safe to drop.
#define WSEED_ERR_RECOVER        -9

// Is the seed at rest actually encrypted (flash encryption burned in eFuse)?
// Only the storage NOTES need this, to tell the truth about what a chip dump
// would reveal. Host/simulator: false, since the desktop store is a plain file.
int kiss_seed_flash_encrypted(void);

// The staged choice if setup is mid-flight, otherwise what flash holds.
int  kiss_seed_mode(void);
// Persists the choice IMMEDIATELY. Switching TO amnesic wipes any stored seed,
// so the mode shown on screen is always the truth about what is on the device.
// Not for the setup wizard: see kiss_seed_stage_mode.
// Returns 0 only after the write/erase has been verified.
int kiss_seed_set_mode(int mode);
// Migrate an existing wallet. The destination is written, read back, validated
// and byte-compared before the source is removed. AMNESIC remains in RAM until
// kiss_session_close(). WSEED_ERR_CLEANUP means the verified destination is
// active but an old-source artifact could not be removed. WSEED_ERR_ROLLBACK
// means the original source is still active but an uncommitted destination
// artifact could not be removed.
int kiss_seed_move_to(int mode);
// The wizard's version: remember the answer, touch nothing. The wizard asks
// KEEP vs NOTHING SAVED before the new wallet exists, so applying it there
// would erase a wallet the user might still back out and keep.
// kiss_seed_commit applies it; kiss_seed_discard forgets it.
void kiss_seed_stage_mode(int mode);

// 1 if a wallet is configured. In SD mode this intentionally stays 1 while the
// card is absent/corrupt, so boot cannot mistake it for a factory-fresh device.
int kiss_seed_exists(void);

// Validate (BIP39 checksum, wordlist) and persist. 0 on success.
int kiss_seed_store(const char *mnemonic);

// Two-phase persistence for setup (P0 safety net). The wizard STAGES a
// validated mnemonic in RAM; it is only written to flash by kiss_seed_commit
// once the full setup ritual (passphrase typed twice + fingerprint recorded)
// finishes. Cancelling anywhere before commit calls kiss_seed_discard, so an
// abandoned setup leaves no half-made wallet. While staged, load/exists/session
// all see the staged mnemonic (so the fingerprint can be shown pre-commit).
int kiss_seed_stage(const char *mnemonic);   // validate + hold in RAM
int kiss_seed_commit(void);                  // staged RAM + mode -> flash
void kiss_seed_discard(void);                // drop the staged mnemonic + mode

// Copy the stored mnemonic into out. 0 on success, nonzero if none/too small.
int kiss_seed_load(char *out, size_t out_len);

// Erase the stored seed. 0 on success (also 0 when nothing was stored).
int kiss_seed_wipe(void);

// Lock hook, called from kiss_session_close(). In AMNESIC mode the staged
// mnemonic IS the wallet, so locking has to drop it; in KEEP mode this does
// nothing (flash still holds the words, which is the point of that mode).
void kiss_seed_forget(void);

// BIP39 checksum + wordlist validation only (nothing stored). 0 = valid.
int kiss_seed_validate(const char *mnemonic);

// 1 when a mnemonic's entropy carries no secret at all: a degenerate byte
// pattern, or a word sequence the blind draw's judge blocks (WC_F_DEGEN).
// Import and creation entry points only — kiss_seed_from_qr calls it, and the
// typed restore judges the same class for itself so the walk can render it.
//
// NEVER call this from kiss_seed_validate or kiss_seed_stage. The storage read
// back paths revalidate through both, so a gate there would refuse a seed the
// device already holds and lock the owner out of a wallet at unlock. Refusing
// to TAKE a seed and refusing to OPEN one are not the same act.
int kiss_seed_degenerate(const char *mnemonic);

// Entropy -> mnemonic words. len must be 16 (12 words) or 32 (24 words).
// 0 on success; out is NUL-terminated.
int kiss_seed_from_entropy(const uint8_t *entropy, size_t len,
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
// KISS deliberately has no matching PLAINTEXT export: it reads seed QRs and
// never draws one. The one sanctioned export is the KEF encrypted backup
// (kiss_kef.h), which leaves the box only under a password.
int kiss_seed_from_qr(const char *data, size_t len, char *out, size_t out_len);

// ---- wordlist access (RESTORE autocomplete + prove-backup quiz decoys) ----
// Up to n wordlist words starting with prefix into out[]; returns the count.
// Pointers are into the static wordlist — do not free.
int kiss_seed_suggest(const char *prefix, const char *out[], int n);

// Wordlist word by index (0..2047). 0 on success.
int kiss_seed_word(int index, const char **out);

// ---- backup verification ----
// Compare two space-separated mnemonics word by word. Returns the 0-based index
// of the FIRST differing word (so the UI can say "word #N"), or -1 if identical.
// A different word count counts as a mismatch at the first missing/extra word.
// Reveals only the position, never the correct word.
int kiss_seed_diff_word(const char *typed, const char *stored);

#ifndef ESP_PLATFORM
// Host-only persistence fault seam used by transition tests.
#define WSEED_TEST_FAIL_MODE_WRITE  (1u << 0)
#define WSEED_TEST_FAIL_SEED_REMOVE (1u << 1)
// The residue scrub, failing the one way that matters: the store is erased and
// the mnemonic cannot be put back. There is no way to provoke it through the
// other seams -- storage_write_keep consumes the one-shot MODE_WRITE before the
// scrub ever runs -- and it is the only path that returns WSEED_ERR_RECOVER.
#define WSEED_TEST_FAIL_SCRUB       (1u << 2)
// storage_write_keep's readback, failing after the write committed. On a
// REPLACEMENT that is a destroyed old wallet plus an unverifiable new one,
// which is the second way into WSEED_ERR_RECOVER and had no way to be
// reached from a test.
#define WSEED_TEST_FAIL_VERIFY      (1u << 3)
void kiss_seed_test_fail_next(unsigned flags);
#endif

// ---- entropy quality note ----
// What the device thought of the draw its seed was made from: 0 = nothing to
// say, otherwise a verdict. Every seed creating path writes it, so a clean
// rebuild clears a previous one.
//
// This exists because a warning shown once, at the most excited moment of
// setup, is a warning the product forgot on the owner's behalf. The seed is a
// hash by the time anything downstream sees it, so nothing later can notice the
// input was fifty presses of one key; the judgement happens on the raw material
// (kiss_dice_q.c, kiss_cards_q.c) and this is where the answer is kept.
//
// NOT host only, despite sitting next to the test seam above. Both callers --
// kiss_setup.c (writes) and kiss_info.c (reads) -- are unguarded, and the
// definitions in kiss_seed.c are unguarded too, so a declaration hidden from
// the device build is an implicit declaration on the device build and nothing
// else. It was inside the #ifndef until this comment was written.
//
// Device wide, never per wallet, and deliberately not keyed by fingerprint:
// the draw made one master seed and every passphrase wallet descends from it,
// so this reveals nothing about how many wallets exist.
// Packed so the reader can tell WHICH path spoke, because the two wear
// different titles. 0 is "nothing to say" and every path writes it on a clean
// seed, which is what clears a previous device's verdict.
//
// 1..15 are dice verdicts written by firmware from before dice became a hard
// stop. Nothing writes them any more; they are still READ so a device that
// upgrades keeps telling the truth about the seed it already has.
#define WSEED_ENTQ_NONE   0
#define WSEED_ENTQ_DICE   0x00   // legacy: a bare WD_Q_* in 1..15
#define WSEED_ENTQ_CARDS  0x20   // this bit set, WC_Q_* in the low nibble
#define WSEED_ENTQ_IS_CARDS(v) (((v) & 0xF0) == WSEED_ENTQ_CARDS)

void kiss_seed_set_entropy_note(int v);
int  kiss_seed_entropy_note(void);

// ---- how this seed was made ----
// Which path produced the seed the device is holding. The note above says what
// the device THOUGHT of the draw; this says where the draw came from, and it is
// the one fact an owner cannot recover by looking at the words.
//
// Device wide and written by every staging path, like the note. 0 means nothing
// was recorded -- a seed made before this existed, which is a real answer and
// not an error.
#define WSEED_SRC_NONE     0
#define WSEED_SRC_MIX      1   // camera + chip + taps + timing
#define WSEED_SRC_DICE     2   // d6 rolls, hashed
#define WSEED_SRC_CARDS    3   // the owner's own blind draw
#define WSEED_SRC_RESTORE  4   // words typed in from elsewhere
#define WSEED_SRC_QR       5   // a seed QR made on another signer
#define WSEED_SRC_KEF      6   // an encrypted backup

void kiss_seed_set_source(int v);
int  kiss_seed_source(void);
