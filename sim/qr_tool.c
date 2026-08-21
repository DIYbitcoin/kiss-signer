// Desktop QR-transport driver for interop tests (BlueWallet rig, tools/bw_interop):
//   kissqr emit <psbt-file>   -> UR crypto-psbt frames, one per line
//                                (cycles 3x the pure-fragment count, like the
//                                 device's looping animated QR)
//   kissqr parse              -> frames on stdin (any order/dupes) -> raw PSBT
//                                bytes on stdout once assembly completes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr_transport.h"

static int read_psbt(const char *path, uint8_t *out, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }

    size_t n = fread(out, 1, cap, f);
    if (ferror(f)) {
        fprintf(stderr, "%s: read failed\n", path);
        fclose(f);
        return -1;
    }
    if (n == cap) {
        int extra = fgetc(f);
        if (extra != EOF) {
            fprintf(stderr, "%s: input exceeds QRT_MAX_PSBT (%zu bytes)\n",
                    path, cap);
            fclose(f);
            return -1;
        }
        if (ferror(f)) {
            fprintf(stderr, "%s: read failed\n", path);
            fclose(f);
            return -1;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "%s: close failed\n", path);
        return -1;
    }
    *len = n;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "emit") == 0) {
        static uint8_t psbt[QRT_MAX_PSBT];
        size_t len = 0;
        if (read_psbt(argv[2], psbt, sizeof psbt, &len) != 0) return 1;
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_UR, psbt, len);
        if (!e) { fprintf(stderr, "encoder failed\n"); return 1; }
        int parts = qrt_encoder_parts(e);
        char out[600];
        for (int i = 0; i < parts * 3; i++) {         // enough fountain frames to converge
            if (qrt_encoder_next(e, out, sizeof out) != 0) break;
            puts(out);
        }
        qrt_encoder_free(e);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "parse") == 0) {
        qrt_parser_t *p = qrt_parser_new();
        char line[4096];
        while (fgets(line, sizeof line, stdin)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (!n) continue;
            qrt_parser_feed(p, line, n);
            if (qrt_parser_complete(p)) break;
        }
        if (!qrt_parser_complete(p)) { fprintf(stderr, "incomplete\n"); return 1; }
        static uint8_t psbt[QRT_MAX_PSBT];
        size_t len = 0;
        if (qrt_parser_result(p, psbt, sizeof psbt, &len) != 0) {
            fprintf(stderr, "result failed\n");
            return 1;
        }
        fwrite(psbt, 1, len, stdout);
        qrt_parser_free(p);
        return 0;
    }
    fprintf(stderr, "usage: kissqr emit <psbt-file> | kissqr parse\n");
    return 2;
}
