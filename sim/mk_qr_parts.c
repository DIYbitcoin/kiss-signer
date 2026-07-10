// Emit QR part strings for a PSBT file using the DEVICE's own encoder
// (main/qr_transport.c over components/cUR), so a test page built from this
// output is byte-for-byte what the firmware expects to parse. Each emitted
// part is also fed back through qrt_parser as a roundtrip check: the tool
// fails unless the parts reassemble into the exact input bytes.
// Build+run: sim/mk_test_qrs.sh (which also renders the HTML page).
// usage: mk_qr_parts <file.psbt> static|pmofn|ur
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr_transport.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.psbt> static|pmofn|ur\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static uint8_t psbt[QRT_MAX_PSBT];
    size_t n = fread(psbt, 1, sizeof psbt, f);
    fclose(f);

    int fmt = !strcmp(argv[2], "static") ? QRT_FMT_STATIC
            : !strcmp(argv[2], "pmofn")  ? QRT_FMT_PMOFN
            : !strcmp(argv[2], "ur")     ? QRT_FMT_UR : QRT_FMT_NONE;
    if (fmt == QRT_FMT_NONE) { fprintf(stderr, "bad format %s\n", argv[2]); return 2; }

    qrt_encoder_t *e = qrt_encoder_new(fmt, psbt, n);
    if (!e) { fprintf(stderr, "encoder refused (too big for one QR?)\n"); return 1; }
    qrt_parser_t *p = qrt_parser_new();
    int parts = qrt_encoder_parts(e);

    // emit at least one full cycle; fountain UR may need a few extra parts
    char out[1200];
    int emitted = 0;
    for (int i = 0; i < parts * 3 + 1; i++) {
        if (qrt_encoder_next(e, out, sizeof out) != 0) { fprintf(stderr, "next failed\n"); return 1; }
        puts(out);
        emitted++;
        if (qrt_parser_feed(p, out, strlen(out)) < 0) { fprintf(stderr, "feed rejected\n"); return 1; }
        if (qrt_parser_complete(p) && emitted >= parts) break;
    }

    static uint8_t back[QRT_MAX_PSBT];
    size_t bn = 0;
    if (!qrt_parser_complete(p) ||
        qrt_parser_result(p, back, sizeof back, &bn) != 0 ||
        bn != n || memcmp(back, psbt, n) != 0) {
        fprintf(stderr, "ROUNDTRIP FAIL\n");
        return 1;
    }
    fprintf(stderr, "roundtrip OK: %d part(s), %zu bytes\n", emitted, n);
    qrt_parser_free(p);
    qrt_encoder_free(e);
    return 0;
}
