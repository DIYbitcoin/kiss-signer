// The CAMERA AUDIT pipeline. See wallet_proof.h for the contract and
// docs/specs/prove-it.md for what the proof does and does not prove.
//
// Order of operations: hash first, then the frame's atomic write, then the
// checker page, then the words. The write's read back and byte compare is the
// file half of the property -- if the card holds different bytes than the ones
// hashed, the write fails and no proof file exists to contradict the screen.
// The frame goes first because it is the artifact; the page is the courtesy,
// and a card that takes 1.9MB and then refuses 26KB is a card that lies, so a
// failed page write pulls the frame back out and reports the same SD error.
#include "wallet_proof.h"

#include <string.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "platform_sd.h"
#include "verify_page.h"
#include "wallet_seed.h"

int wallet_proof_run(const uint8_t *frame, size_t len,
                     uint8_t hash_out[32], char *words_out, size_t words_len)
{
    if (!frame || len != WPROOF_FRAME_BYTES || !hash_out || !words_out)
        return WPROOF_ERR_ARG;

    uint8_t h[32];
    if (wally_sha256(frame, len, h, sizeof h) != WALLY_OK)
        return WPROOF_ERR_DERIVE;

    // The UI gates on a present card, but a card can be pulled between that
    // screen and this write. Mount is idempotent and cheap when it is still
    // there, and turns "pulled at the worst moment" into a clean SD error.
    if (platform_sd_mount() != 0)
        return WPROOF_ERR_SD;

    int rc = platform_sd_write_atomic(WPROOF_NAME, frame, len);
    if (rc != 0 && rc != PLATFORM_SD_ATOMIC_CLEANUP)
        return WPROOF_ERR_SD;

    rc = platform_sd_write_atomic(WPROOF_PAGE_NAME, verify_page_html,
                                  verify_page_html_len);
    if (rc != 0 && rc != PLATFORM_SD_ATOMIC_CLEANUP) {
        (void)platform_sd_delete(WPROOF_NAME);
        return WPROOF_ERR_SD;
    }

    if (wallet_seed_from_entropy(h, sizeof h, words_out, words_len) != 0)
        return WPROOF_ERR_DERIVE;

    memcpy(hash_out, h, sizeof h);
    return WPROOF_OK;
}
