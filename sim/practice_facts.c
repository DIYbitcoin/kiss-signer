// What KISS itself says about each practice transaction, read by the device's
// own verify code on the practice seed and on testnet: the verdict, the
// amounts and the reason. The page prints these numbers, so they come from
// here and never from a hand typed sentence that can drift from the fixture.
// It also signs every one the verdict allows, and fails if any does not sign.
// usage: practice_facts <file.psbt>...    prints one JSON object per line
#include <stdio.h>
#include <string.h>
#include "kiss_crypto.h"
#include "kiss_psbt.h"
#include "kiss_seed.h"

static const char *st(int s)
{
    return s == WPSBT_READY ? "READY" : s == WPSBT_CAUTION ? "CAUTION" : "STOP";
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (kiss_seed_store("abandon abandon abandon abandon abandon abandon "
                        "abandon abandon abandon abandon abandon about") != 0)
        return 1;
    kiss_set_network(1);
    kiss_set_script(WSCRIPT_NATIVE);
    if (kiss_session_open("") != 0)
        return 1;
    static uint8_t buf[8192], out[16384];
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f) fclose(f);
        wpsbt_summary_t s;
        if (!n || kiss_psbt_load(buf, n, &s) != 0) {
            fprintf(stderr, "%s: did not load\n", argv[i]);
            return 1;
        }
        size_t w = 0;
        int signs = s.status != WPSBT_STOP &&
                    kiss_psbt_sign(out, sizeof out, &w) == 0 && w > 0;
        if (s.status != WPSBT_STOP && !signs) {
            fprintf(stderr, "%s: accepted but did not sign\n", argv[i]);
            bad = 1;
        }
        printf("{\"file\":\"%s\",\"status\":\"%s\",\"send\":%llu,\"fee\":%llu,"
               "\"change\":%llu,\"inputs\":%u,\"signs\":%s,\"reason\":\"%s\"}\n",
               argv[i], st(s.status), (unsigned long long)s.send_sats,
               (unsigned long long)s.fee_sats, (unsigned long long)s.change_sats,
               (unsigned)s.n_in, signs ? "true" : "false", s.reason);
        kiss_psbt_free();
    }
    kiss_session_close();
    return bad;
}
