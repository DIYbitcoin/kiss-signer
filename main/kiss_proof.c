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

// The card's copy carries this run's hash, so the owner opens it, drops the
// file and reads the verdict -- the QR and the URL fragment are for a copy
// fetched from somewhere this device cannot reach. The page's placeholder is
// overwritten in place: same length, and the claim lives inside the script
// that reads it rather than trailing the document.
static int write_claimed_page(const uint8_t h[32])
{
    const size_t slot_len = sizeof WPROOF_CLAIM_SLOT - 1;   // 64

    size_t at = 0;
    while (at + slot_len <= verify_page_html_len &&
           memcmp(verify_page_html + at, WPROOF_CLAIM_SLOT, slot_len) != 0)
        at++;
    if (at + slot_len > verify_page_html_len) return -1;    // gate-checked, but

    // Hex first, then a fixed 64 byte copy: writing the digits straight into
    // the page would leave snprintf's NUL on the character after the slot.
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);

    uint8_t *buf = malloc(verify_page_html_len);
    if (!buf) return -1;
    memcpy(buf, verify_page_html, verify_page_html_len);
    memcpy(buf + at, hex, slot_len);

    int rc = platform_sd_write_atomic(WPROOF_PAGE_NAME, buf, verify_page_html_len);
    free(buf);
    return (rc == 0 || rc == PLATFORM_SD_ATOMIC_CLEANUP) ? 0 : -1;
}

int kiss_proof_run(const uint8_t *frame, size_t len,
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

    if (write_claimed_page(h) != 0) {
        (void)platform_sd_delete(WPROOF_NAME);
        return WPROOF_ERR_SD;
    }

    if (kiss_seed_from_entropy(h, sizeof h, words_out, words_len) != 0)
        return WPROOF_ERR_DERIVE;

    memcpy(hash_out, h, sizeof h);
    return WPROOF_OK;
}
