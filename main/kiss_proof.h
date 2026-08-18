// The CAMERA AUDIT pipeline: one camera frame becomes a file on the SD card, a
// SHA256 and 12 burned words. The owner checks all three on any computer:
// shasum of the file must match the screen, and any BIP39 tool fed that hash
// as entropy must produce the same words -- fed the FIRST 16 BYTES of it, which
// is what makes the result 12 words, the only length this signer creates. See docs/specs/prove-it.md for what
// that does and does not prove.
//
// The words are a real seed sitting on the card in cleartext. Callers keep
// them in their own buffers, never the wizard's, and wipe on every exit.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Filename literal, never translated: the owner types it into a shell.
#define WPROOF_NAME        "kiss-proof.bin"

// The offline checker page, written beside the frame (same rule: a literal the
// owner sees in a file listing). The page is STATELESS -- it computes the hash
// and the words from whatever file is dropped on it, and the human compares
// the 12 words against the device screen. No claim is baked in: a card that
// carried one could swear to a hash from a run before (see kiss_proof.c). It
// comes from the device being audited, so a doubter fetches the same page from
// the repo or the site the page itself names, and checks with that copy.
#define WPROOF_PAGE_NAME   "kiss-verify.html"

// The whole negotiated sensor frame, raw RGB565. A frame of any other length
// is refused rather than adapted to.
#define WPROOF_FRAME_W     1288
#define WPROOF_FRAME_H     728
#define WPROOF_FRAME_BYTES ((size_t)WPROOF_FRAME_W * WPROOF_FRAME_H * 2)

// What actually lands on the card: every second pixel of every second row.
// Subsampled, not averaged, so anyone can reproduce the file from a raw
// frame with one loop -- and a quarter the bytes writes in a quarter the
// time, which is what made the audit feel broken on a real card. The hash,
// the page and the pinned vectors are all over THESE bytes.
#define WPROOF_FILE_W      (WPROOF_FRAME_W / 2)
#define WPROOF_FILE_H      (WPROOF_FRAME_H / 2)
#define WPROOF_FILE_BYTES  ((size_t)WPROOF_FILE_W * WPROOF_FILE_H * 2)

// How much of the hash becomes seed words: 16 bytes, so the audit demonstrates
// the same 12 word seed every creation path on this device produces. Taking all
// 32 made 24, a length the signer offers only on RESTORE, so the page teaching
// how entropy becomes seed words was teaching a capability it does not have.
//
// Changing this changes the recipe, and the recipe is written down in four
// places that must move together: here, docs/verify.html (embedded into
// verify_page.c and written onto the card beside every frame), verify_proof.py,
// and docs/specs/prove-it.md. A card written by older firmware carries the
// checker page that matches it, so old proofs keep verifying.
#define WPROOF_ENTROPY_BYTES 16

enum {
    WPROOF_OK         = 0,
    WPROOF_ERR_ARG    = -1,   // NULL frame, or len != WPROOF_FRAME_BYTES
    WPROOF_ERR_SD     = -2,   // write or verify failed; no proof file exists
    WPROOF_ERR_DERIVE = -3,   // hash or BIP39 failed (never expected)
};

// Subsample the frame, SHA256 the subsampled bytes, write them to WPROOF_NAME (atomic form, so
// the card copy is read back and byte compared before it gets the name), write
// the embedded checker page plus this run's claim as WPROOF_PAGE_NAME the same
// way, then derive the 12 words from the first WPROOF_ENTROPY_BYTES of the hash. hash_out and words_out are filled only on
// WPROOF_OK; words_len must be >= WSEED_MAX_MNEMONIC. A stale-sidecar cleanup
// result from either write is success: the target is committed and verified.
// On WPROOF_ERR_SD neither file exists (a failed page write deletes the
// already committed frame), so the fail screen's "nothing was kept" is true.
int kiss_proof_run(const uint8_t *frame, size_t len,
                     uint8_t hash_out[32], char *words_out, size_t words_len);
