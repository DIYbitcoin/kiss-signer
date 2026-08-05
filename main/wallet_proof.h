// The CAMERA AUDIT pipeline: one camera frame becomes a file on the SD card, a
// SHA256 and 24 burned words. The owner checks all three on any computer:
// shasum of the file must match the screen, and any BIP39 tool fed that hash
// as entropy must produce the same words. See docs/specs/prove-it.md for what
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
// owner sees in a file listing). The card copy carries the hash this run
// claimed, appended as WPROOF_CLAIM_FMT, so opening it and dropping the file
// is the entire check: no QR, no typing. It still comes from the device being
// audited, so the independent copy lives at WPROOF_VERIFY_URL, which the AUDIT
// RESULT QR extends with #h=<hash> to make that copy compare too.
#define WPROOF_PAGE_NAME   "kiss-verify.html"
#define WPROOF_VERIFY_URL  "https://kkdao.github.io/kiss-signer/verify.html"
// The page ships a 64 dash placeholder; the card's copy gets this run's hash
// written over it, in place, so the file keeps its length and the claim sits
// in the page's own script rather than after it.
#define WPROOF_CLAIM_SLOT \
    "----------------------------------------------------------------"

// The whole negotiated sensor frame, raw RGB565. The pinned test vector and
// the owner's recipe both depend on this exact size, so a frame of any other
// length is refused rather than adapted to.
#define WPROOF_FRAME_W     1288
#define WPROOF_FRAME_H     728
#define WPROOF_FRAME_BYTES ((size_t)WPROOF_FRAME_W * WPROOF_FRAME_H * 2)

enum {
    WPROOF_OK         = 0,
    WPROOF_ERR_ARG    = -1,   // NULL frame, or len != WPROOF_FRAME_BYTES
    WPROOF_ERR_SD     = -2,   // write or verify failed; no proof file exists
    WPROOF_ERR_DERIVE = -3,   // hash or BIP39 failed (never expected)
};

// SHA256 the frame, write those exact bytes to WPROOF_NAME (atomic form, so
// the card copy is read back and byte compared before it gets the name), write
// the embedded checker page plus this run's claim as WPROOF_PAGE_NAME the same
// way, then derive the 24 words from the hash alone. hash_out and words_out are filled only on
// WPROOF_OK; words_len must be >= WSEED_MAX_MNEMONIC. A stale-sidecar cleanup
// result from either write is success: the target is committed and verified.
// On WPROOF_ERR_SD neither file exists (a failed page write deletes the
// already committed frame), so the fail screen's "nothing was kept" is true.
int wallet_proof_run(const uint8_t *frame, size_t len,
                     uint8_t hash_out[32], char *words_out, size_t words_len);
