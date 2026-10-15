// Test-only bridge for coordinator interoperability. Public development seed,
// testnet, native SegWit; no wallet data or device transport is accessed.
#include <stdio.h>
#include <stdlib.h>
#include "kiss_crypto.h"
#include "kiss_psbt.h"
#include "kiss_seed.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s request.psbt signed.psbt\n", argv[0]);
        return 2;
    }
    unsigned char request[65536], signed_psbt[65536];
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 2;
    size_t n = fread(request, 1, sizeof request, in);
    int invalid = ferror(in) || n == 0 || n == sizeof request;
    fclose(in);
    if (invalid) return 2;
    const char *scratch = getenv("KISS_SIM_TMP");
    if (!scratch || !*scratch) {
        fprintf(stderr, "set KISS_SIM_TMP to an isolated test directory\n");
        return 2;
    }
    if (kiss_seed_open_temp("abandon abandon abandon abandon abandon abandon "
                            "abandon abandon abandon abandon abandon about") != 0)
        return 2;
    kiss_set_network(1);
    kiss_set_script(WSCRIPT_NATIVE);
    if (kiss_session_open("") != 0) return 2;
    wpsbt_summary_t summary = {0};
    size_t written = 0;
    int rc = 0;
    if (kiss_psbt_load(request, n, &summary) != 0) rc = 2;
    else if (summary.status == WPSBT_STOP) {
        fprintf(stderr, "STOP: %s\n", summary.reason);
        rc = 3;
    } else if (summary.status != WPSBT_READY ||
               kiss_psbt_sign(signed_psbt, sizeof signed_psbt, &written) != 0 ||
               written == 0) rc = 2;
    if (!rc) {
        FILE *out = fopen(argv[2], "wbx");
        if (!out) rc = 2;
        else {
            if (fwrite(signed_psbt, 1, written, out) != written) rc = 2;
            if (fclose(out) != 0) rc = 2;
        }
    }
    kiss_psbt_free();
    kiss_session_close();
    return rc;
}
