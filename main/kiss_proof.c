// The CAMERA AUDIT pipeline. See kiss_proof.h for the contract and
// docs/specs/prove-it.md for what the proof does and does not prove.
//
// Order of operations: hash first, then the frame's atomic write, then the
// checker page, then the words. The write's read back and byte compare is the
// file half of the property -- if the card holds different bytes than the ones
// hashed, the write fails and no proof file exists to contradict the screen.
// The frame goes first because it is the artifact; the page is the courtesy,
// and a card that takes 1.9MB and then refuses 26KB is a card that lies, so a
// failed page write pulls the frame back out and reports the same SD error.
#include "kiss_proof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "platform_sd.h"
#include "verify_page.h"
#include "kiss_seed.h"
#include "kiss_wipe.h"

// The checker page, written as-is every run. It used to carry this run's
// hash baked over a placeholder, and that made the card hold TWO files that
// had to agree: back out (or pull the card) between the frame write and the
// page write and the pair is torn -- which is exactly the state one bench
// test produced, a page swearing to a hash from a run before. The page is
// stateless now: it computes hash and words from whatever file is dropped on
// it, and the claim to compare against is the one on the device's screen.
// Same bytes every run, so there is nothing to tear.
static int write_page(void)
{
    int rc = platform_sd_write_atomic(WPROOF_PAGE_NAME, verify_page_html,
                                      verify_page_html_len);
    return (rc == 0 || rc == PLATFORM_SD_ATOMIC_CLEANUP) ? 0 : -1;
}

int kiss_proof_run(const uint8_t *frame, size_t len,
                     uint8_t hash_out[32], char *words_out, size_t words_len)
{
    if (!frame || len != WPROOF_FRAME_BYTES || !hash_out || !words_out)
        return WPROOF_ERR_ARG;

    // Every second pixel of every second row, two bytes per pixel. The hash
    // is of the FILE, computed after the copy, so what the screen claims and
    // what a checker rederives are the same bytes by construction.
    uint8_t *file = malloc(WPROOF_FILE_BYTES);
    if (!file)
        return WPROOF_ERR_DERIVE;
    for (size_t y = 0; y < WPROOF_FILE_H; y++) {
        const uint8_t *src = frame + (y * 2) * (size_t)WPROOF_FRAME_W * 2;
        uint8_t *dst = file + y * (size_t)WPROOF_FILE_W * 2;
        for (size_t x = 0; x < WPROOF_FILE_W; x++) {
            dst[x * 2]     = src[x * 4];
            dst[x * 2 + 1] = src[x * 4 + 1];
        }
    }

    uint8_t h[32];
    int rc = wally_sha256(file, WPROOF_FILE_BYTES, h, sizeof h) == WALLY_OK
               ? WPROOF_OK : WPROOF_ERR_DERIVE;

    // The UI gates on a present card, but a card can be pulled between that
    // screen and this write. Mount is idempotent and cheap when it is still
    // there, and turns "pulled at the worst moment" into a clean SD error.
    if (rc == WPROOF_OK && platform_sd_mount() != 0)
        rc = WPROOF_ERR_SD;
    if (rc == WPROOF_OK) {
        int w = platform_sd_write_atomic(WPROOF_NAME, file, WPROOF_FILE_BYTES);
        if (w != 0 && w != PLATFORM_SD_ATOMIC_CLEANUP)
            rc = WPROOF_ERR_SD;
    }
    kiss_wipe(file, WPROOF_FILE_BYTES);   // these bytes derive a (burned) seed
    free(file);
    if (rc != WPROOF_OK)
        return rc;

    if (write_page() != 0) {
        (void)platform_sd_delete(WPROOF_NAME);
        return WPROOF_ERR_SD;
    }

    // The FIRST 16 BYTES of the hash, which is 12 words.
    //
    // It used to be all 32, which is 24, and 24 is a number this signer never
    // produces: every creation path pins 12 (kiss_setup.c method_cam_cb,
    // method_dice_cb, method_cards_cb) and 24 survives only on RESTORE, to
    // match paper an owner already has. So the one screen whose entire job is
    // demonstrating how this device turns entropy into seed words was
    // demonstrating it with a seed this device cannot make, and a reader who
    // took the page at its word came away believing the signer offers 24.
    //
    // What is given up, stated because the spec has to record it: the audit no
    // longer shows that the other 16 bytes of the hash went unused. It never
    // showed much -- an owner who rederives gets the same 12 words from the
    // same file either way -- and it is a poor trade against a page teaching a
    // capability that is not there.
    //
    // The recipe now lives in four places and they move together or the audit
    // stops verifying: here, docs/verify.html (which this device writes onto
    // the card beside the frame, so a card carries its own matching checker),
    // tools/verify_proof.py, and docs/specs/prove-it.md.
    if (kiss_seed_from_entropy(h, WPROOF_ENTROPY_BYTES, words_out, words_len) != 0)
        return WPROOF_ERR_DERIVE;

    memcpy(hash_out, h, sizeof h);
    return WPROOF_OK;
}
