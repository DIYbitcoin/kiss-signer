// Read the practice page's codes back the way the device does, and fail
// unless they give back exactly what they were made from.
//
// Each symbol is drawn as a grey image (4 px a module, the 4 module quiet zone
// the page draws) and handed to k_quirc, the device's QR decoder. The parts of
// a transaction go through qrt_parser, the device's UR reader, and must rebuild
// the PSBT byte for byte. The seed code goes through kiss_seed_from_qr, the
// device's seed QR reader, and must give the practice seed's 12 words.
//
// What this cannot see is the camera: glare, focus and a phone's own screen.
// That is the device test.
//
// usage: practice_readback <seed.qr> (<name.qr> <name.psbt>)...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k_quirc.h"
#include "qr_transport.h"
#include "kiss_seed.h"

#define PX 4
#define QUIET 4

static char line[200000];

// The next symbol in f, decoded by k_quirc. Returns the payload length, or -1
// at the end of the file, or -2 when k_quirc cannot read it.
static int next_symbol(FILE *f, uint8_t *out, size_t cap)
{
    if (!fgets(line, sizeof line, f))
        return -1;
    char *sp = strchr(line, ' ');
    if (!sp)
        return -2;
    int n = atoi(line);
    const char *bits = sp + 1;
    int side = (n + 2 * QUIET) * PX;
    uint8_t *img = malloc((size_t)side * side);
    memset(img, 255, (size_t)side * side);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (bits[y * n + x] == '1')
                for (int dy = 0; dy < PX; dy++)
                    memset(img + (size_t)((y + QUIET) * PX + dy) * side + (x + QUIET) * PX, 0, PX);
    static k_quirc_result_t r[2];
    int got = k_quirc_decode_grayscale(img, side, side, r, 2, false);
    free(img);
    if (got != 1 || !r[0].valid || (size_t)r[0].data.payload_len >= cap)
        return -2;
    memcpy(out, r[0].data.payload, (size_t)r[0].data.payload_len);
    return r[0].data.payload_len;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (argc - 2) % 2)
        return 2;
    static uint8_t pay[8192], want[16384], back[16384];
    int bad = 0;

    // the seed
    FILE *f = fopen(argv[1], "r");
    int l = f ? next_symbol(f, pay, sizeof pay) : -2;
    if (f) fclose(f);
    char words[512];
    if (l < 0 || kiss_seed_from_qr((const char *)pay, (size_t)l, words, sizeof words) != 0 ||
        strcmp(words, "abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon about") != 0) {
        fprintf(stderr, "seed code: the device does not read back the practice seed\n");
        bad = 1;
    } else {
        printf("seed code: read back as the practice seed\n");
    }

    // the transactions
    for (int a = 2; a < argc; a += 2) {
        FILE *p = fopen(argv[a + 1], "rb");
        size_t wn = p ? fread(want, 1, sizeof want, p) : 0;
        if (p) fclose(p);
        FILE *q = fopen(argv[a], "r");
        qrt_parser_t *par = qrt_parser_new();
        int frames = 0, unread = 0;
        while (q && par) {
            int k = next_symbol(q, pay, sizeof pay);
            if (k == -1) break;
            frames++;
            if (k < 0) { unread++; continue; }
            pay[k] = 0;
            if (qrt_parser_feed(par, (const char *)pay, (size_t)k) < 0) unread++;
        }
        if (q) fclose(q);
        size_t bn = 0;
        int ok = par && !unread && wn && qrt_parser_complete(par) &&
                 qrt_parser_result(par, back, sizeof back, &bn) == 0 &&
                 bn == wn && memcmp(back, want, wn) == 0;
        qrt_parser_free(par);
        printf("%s: %d frame(s), %s\n", argv[a], frames,
               ok ? "read back as the exact transaction" : "DOES NOT READ BACK");
        bad |= !ok;
    }
    return bad;
}
