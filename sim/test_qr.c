// Desktop tests for main/qr_transport.c (step 6 QR transport).
// Inputs are crafted with cUR / libwally primitives directly, independent of
// the glue under test, plus cUR's own multipart UR test vectors.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr_transport.h"

#include <wally_core.h>      // wally_base64_from_bytes (cross-check only)
#include "ur_encoder.h"      // craft single-part UR input
#include "types/psbt.h"      // crypto-psbt CBOR wrap for crafting

#ifndef KISS_ROOT
#define KISS_ROOT "."
#endif
#define UR_VECTOR_TXT  KISS_ROOT "/components/cUR/tests/test_cases/PSBTs/PSBT_1.UR_fragments.txt"
#define UR_VECTOR_BIN  KISS_ROOT "/components/cUR/tests/test_cases/PSBTs/PSBT_1.psbt.bin"

static int qfails;

static void qchki(const char *name, long long got, long long want) {
    if (got == want) printf("PASS: %s -> %lld\n", name, got);
    else { printf("FAIL: %s\n  got  %lld\n  want %lld\n", name, got, want); qfails++; }
}

static void qchkb(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); qfails++; }
}

static uint8_t *read_bin(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

// Pull the "ur:..." strings out of the (python-list formatted) vector file.
static int read_ur_lines(const char *path, char **out, int max) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char *a = strchr(line, '"');
        if (!a) continue;
        char *b = strchr(a + 1, '"');
        if (!b) continue;
        *b = 0;
        out[n++] = strdup(a + 1);
    }
    fclose(f);
    return n;
}

int test_qr_transport(const uint8_t *psbt, size_t psbt_len) {
    uint8_t out[QRT_MAX_PSBT];
    size_t on = 0;
    char *b64 = NULL;
    wally_base64_from_bytes(psbt, psbt_len, 0, &b64);
    if (!b64) { printf("FAIL: qr fixture base64\n"); return 1; }

    // ---- static base64 in one QR ----
    qrt_parser_t *p = qrt_parser_new();
    qchkb("qr b64 parser allocs", p != NULL);
    qchki("qr b64 feed rc", qrt_parser_feed(p, b64, strlen(b64)), 0);
    qchkb("qr b64 complete after one", qrt_parser_complete(p));
    qchki("qr b64 format", qrt_parser_format(p), QRT_FMT_STATIC);
    qchki("qr b64 result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
    qchkb("qr b64 bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
    qrt_parser_free(p);

    // ---- static raw binary (compact QR, has NULs) ----
    p = qrt_parser_new();
    qchki("qr raw feed rc", qrt_parser_feed(p, (const char *)psbt, psbt_len), 0);
    qchkb("qr raw complete", qrt_parser_complete(p));
    on = 0;
    qchki("qr raw result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
    qchkb("qr raw bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
    qrt_parser_free(p);

    // ---- junk is refused and doesn't wedge the parser ----
    p = qrt_parser_new();
    qchkb("qr junk rejected", qrt_parser_feed(p, "hello world", 11) != 0);
    qchkb("qr empty rejected", qrt_parser_feed(p, "", 0) != 0);
    qchkb("qr junk leaves incomplete", !qrt_parser_complete(p));
    qchki("qr junk then b64 still works", qrt_parser_feed(p, b64, strlen(b64)), 0);
    qchkb("qr junk then b64 completes", qrt_parser_complete(p));
    qrt_parser_free(p);

    // ---- single-part UR (crafted with cUR directly) ----
    psbt_data_t *pd = psbt_new(psbt, psbt_len);
    size_t cbl = 0;
    uint8_t *cb = pd ? psbt_to_cbor(pd, &cbl) : NULL;
    char *sur = NULL;
    qchkb("qr cUR single-part encodes", cb && ur_encoder_encode_single("crypto-psbt", cb, cbl, &sur));
    if (sur) {
        p = qrt_parser_new();
        qchki("qr UR single feed rc", qrt_parser_feed(p, sur, strlen(sur)), 0);
        qchkb("qr UR single complete", qrt_parser_complete(p));
        qchki("qr UR format", qrt_parser_format(p), QRT_FMT_UR);
        on = 0;
        qchki("qr UR single result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr UR single bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        free(sur);
    }
    if (cb) free(cb);
    if (pd) psbt_free(pd);

    // ---- multipart UR from cUR's PSBT_1 vectors (file order is shuffled) ----
    char *frag[16] = {0};
    int nfrag = read_ur_lines(UR_VECTOR_TXT, frag, 16);
    size_t vlen = 0;
    uint8_t *vbin = read_bin(UR_VECTOR_BIN, &vlen);
    qchkb("qr vector files load", nfrag == 8 && vbin != NULL);
    if (nfrag > 0 && vbin) {
        p = qrt_parser_new();
        qchki("qr UR multi first feed rc", qrt_parser_feed(p, frag[0], strlen(frag[0])), 0);
        qchki("qr UR multi total known", qrt_parser_total(p), 8);
        qchki("qr UR multi seen 1", qrt_parser_seen(p), 1);
        qchki("qr UR multi dup tolerated", qrt_parser_feed(p, frag[0], strlen(frag[0])), 0);
        qchkb("qr UR multi not complete early", !qrt_parser_complete(p));
        // format lock: a pMofN part must be refused mid-UR
        qchkb("qr UR multi refuses pMofN", qrt_parser_feed(p, "p1of2 cHNidP", 12) != 0);
        for (int i = 1; i < nfrag; i++)
            qrt_parser_feed(p, frag[i], strlen(frag[i]));
        qchkb("qr UR multi completes", qrt_parser_complete(p));
        on = 0;
        qchki("qr UR multi result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr UR multi bytes match vector", on == vlen && memcmp(out, vbin, on) == 0);
        qrt_parser_free(p);
    }
    for (int i = 0; i < nfrag; i++) free(frag[i]);
    if (vbin) free(vbin);

    // ---- pMofN, out of order + duplicate ----
    {
        size_t bl = strlen(b64), half = bl / 2;
        char q1[4200], q2[4200];
        snprintf(q1, sizeof q1, "p1of2 %.*s", (int)half, b64);
        snprintf(q2, sizeof q2, "p2of2 %s", b64 + half);
        p = qrt_parser_new();
        qchki("qr pMofN feed 2 first rc", qrt_parser_feed(p, q2, strlen(q2)), 0);
        qchki("qr pMofN total", qrt_parser_total(p), 2);
        qchki("qr pMofN seen 1", qrt_parser_seen(p), 1);
        qchkb("qr pMofN not complete", !qrt_parser_complete(p));
        qchki("qr pMofN dup rc", qrt_parser_feed(p, q2, strlen(q2)), 0);
        qchki("qr pMofN seen still 1", qrt_parser_seen(p), 1);
        qchki("qr pMofN feed 1 rc", qrt_parser_feed(p, q1, strlen(q1)), 0);
        qchkb("qr pMofN complete", qrt_parser_complete(p));
        qchki("qr pMofN format", qrt_parser_format(p), QRT_FMT_PMOFN);
        on = 0;
        qchki("qr pMofN result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr pMofN bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
    }

    // ---- result cap guard ----
    p = qrt_parser_new();
    qrt_parser_feed(p, b64, strlen(b64));
    qchkb("qr result refuses tiny cap", qrt_parser_result(p, out, 16, &on) != 0);
    qrt_parser_free(p);

    // ---- encoder: UR fountain roundtrip through our own parser ----
    {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_UR, psbt, psbt_len);
        qchkb("qr UR encoder allocs", e != NULL);
        int nparts = qrt_encoder_parts(e);
        qchkb("qr UR encoder is multi-part", nparts >= 2);
        p = qrt_parser_new();
        char part[600];
        int fed = 0, gen_fail = 0, prefix_ok = 1;
        while (p && e && !qrt_parser_complete(p) && fed < nparts * 4 + 8) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) { gen_fail = 1; break; }
            // cUR emits uppercase on purpose: QR alphanumeric mode = smaller QR
            if (strncmp(part, "UR:CRYPTO-PSBT/", 15) != 0) prefix_ok = 0;
            qrt_parser_feed(p, part, strlen(part));
            fed++;
        }
        qchkb("qr UR parts generate", !gen_fail && fed > 0);
        qchkb("qr UR parts carry UR:CRYPTO-PSBT/", prefix_ok);
        qchkb("qr UR roundtrip completes", p && qrt_parser_complete(p));
        on = 0;
        qchki("qr UR roundtrip result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr UR roundtrip bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        qrt_encoder_free(e);
    }

    // ---- encoder: pMofN roundtrip ----
    {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_PMOFN, psbt, psbt_len);
        qchkb("qr pMofN encoder allocs", e != NULL);
        int nparts = qrt_encoder_parts(e);
        qchkb("qr pMofN encoder is multi-part", nparts >= 2);
        p = qrt_parser_new();
        char part[600];
        int gen_fail = 0, prefix_ok = 1;
        for (int i = 0; e && i < nparts; i++) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) { gen_fail = 1; break; }
            char want[16];
            snprintf(want, sizeof want, "p%dof%d ", i + 1, nparts);
            if (strncmp(part, want, strlen(want)) != 0) prefix_ok = 0;
            qrt_parser_feed(p, part, strlen(part));
        }
        qchkb("qr pMofN parts generate", !gen_fail);
        qchkb("qr pMofN prefixes correct", prefix_ok);
        qchkb("qr pMofN roundtrip completes", p && qrt_parser_complete(p));
        on = 0;
        qchki("qr pMofN roundtrip result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr pMofN roundtrip bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        qrt_encoder_free(e);
    }

    // ---- encoder: static equals libwally's base64 (validates local encoder) ----
    {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_STATIC, psbt, psbt_len);
        qchkb("qr static encoder allocs", e != NULL);
        qchki("qr static parts", qrt_encoder_parts(e), 1);
        char part[6000];
        qchki("qr static next rc", qrt_encoder_next(e, part, sizeof part), 0);
        qchkb("qr static matches wally base64", strcmp(part, b64) == 0);
        qrt_encoder_free(e);
    }

    wally_free_string(b64);
    return qfails;
}
