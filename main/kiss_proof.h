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
// claimed, so opening it and dropping the file is the entire check. It comes
// from the device being audited, so a doubter fetches the same page from the
// repo or the site the page itself names, and checks with that copy instead.
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

enum {
    WPROOF_OK         = 0,
    WPROOF_ERR_ARG    = -1,   // NULL frame, or len != WPROOF_FRAME_BYTES
    WPROOF_ERR_SD     = -2,   // write or verify failed; no proof file exists
    WPROOF_ERR_DERIVE = -3,   // hash or BIP39 failed (never expected)
};

// Subsample the frame, SHA256 the subsampled bytes, write them to WPROOF_NAME (atomic form, so
// the card copy is read back and byte compared before it gets the name), write
// the embedded checker page plus this run's claim as WPROOF_PAGE_NAME the same
// way, then derive the 24 words from the hash alone. hash_out and words_out are filled only on
// WPROOF_OK; words_len must be >= WSEED_MAX_MNEMONIC. A stale-sidecar cleanup
// result from either write is success: the target is committed and verified.
// On WPROOF_ERR_SD neither file exists (a failed page write deletes the
// already committed frame), so the fail screen's "nothing was kept" is true.
int kiss_proof_run(const uint8_t *frame, size_t len,
                     uint8_t hash_out[32], char *words_out, size_t words_len);
