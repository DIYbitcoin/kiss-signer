// See wallet_lastword.h.
#include "wallet_lastword.h"

#include <stdio.h>

#include "wally_bip39.h"
#include "wally_core.h"

#include "wallet_seed.h"   // WSEED_MAX_MNEMONIC

const char *wallet_lastword_word(uint16_t index)
{
    if (index > 2047) return NULL;
    return bip39_get_word_by_index(NULL, index);
}

int wallet_lastword_candidates(const char *partial, uint16_t out[WLAST_MAX])
{
    if (!partial || !out) return -1;

    unsigned words = 1;
    for (const char *p = partial; *p; p++) {
        if (*p == ' ') words++;
    }
    if (words != 11 && words != 23) return -1;

    // Try all 2048 list words in the last slot; the validator's checksum is
    // the filter. 2048 SHA256s, done once when the picker opens.
    char m[WSEED_MAX_MNEMONIC];
    int n = 0;
    for (int i = 0; i < 2048; i++) {
        int len = snprintf(m, sizeof m, "%s %s", partial,
                           bip39_get_word_by_index(NULL, (size_t)i));
        if (len < 0 || (size_t)len >= sizeof m) { n = 0; break; }
        if (bip39_mnemonic_validate(NULL, m) == WALLY_OK) {
            out[n++] = (uint16_t)i;
        }
    }
    wally_bzero(m, sizeof m);
    return n;
}
