// One QR symbol per input line, drawn by qrcodegen: the encoder LVGL builds
// into the firmware, so a QR on the practice page is made the way the device
// makes its own. Prints one line per symbol: the size, a space, then the
// modules row by row as 0 and 1. Low error correction, the smallest version
// that holds the text, no mask forced.
// usage: mk_qr_matrix < parts.txt
#include <stdio.h>
#include <string.h>
#include "qrcodegen.h"

int main(void)
{
    static char line[4096];
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    int n = 0;
    while (fgets(line, sizeof line, stdin)) {
        size_t l = strcspn(line, "\r\n");
        line[l] = 0;
        if (!l)
            continue;
        if (!qrcodegen_encodeText(line, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                  qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
            fprintf(stderr, "line %d does not fit one QR\n", n + 1);
            return 1;
        }
        int sz = qrcodegen_getSize(qr);
        printf("%d ", sz);
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++)
                putchar(qrcodegen_getModule(qr, x, y) ? '1' : '0');
        putchar('\n');
        n++;
    }
    return n ? 0 : 1;
}
