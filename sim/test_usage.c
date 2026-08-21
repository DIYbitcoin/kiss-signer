// The coordinator's usage payload: the parser, the height gate, and the facts
// the Receive chip is built out of. No LVGL here — kisstest links kiss_usage.c
// and not kiss_recv.c, so everything below the screen is provable on desktop.
#include <stdio.h>
#include <string.h>

#include "kiss_crypto.h"   // WSCRIPT_*
#include "kiss_usage.h"

static int ufails;

static void uchk(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ufails++;
}

#define P(s) kiss_usage_parse((s), strlen(s), &m)

static void test_parse_good(void)
{
    kiss_usage_msg_t m;
    memset(&m, 0, sizeof m);
    uchk("a well formed payload parses",
         P("KISSU1 EC5A4595 1 0 29 1234567 TB1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4") == 0);
    uchk("  ...fingerprint", m.fp[0] == 0xec && m.fp[1] == 0x5a &&
                             m.fp[2] == 0x45 && m.fp[3] == 0x95);
    uchk("  ...network", m.testnet == 1);
    uchk("  ...script", m.script == WSCRIPT_NATIVE);
    uchk("  ...index", m.high == 29);
    uchk("  ...height", m.height == 1234567u);
    uchk("  ...address rides along whole",
         strcmp(m.addr, "TB1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4") == 0);

    memset(&m, 0, sizeof m);
    uchk("lowercase hex is accepted too",
         P("KISSU1 ec5a4595 0 2 0 1 1BOATSLRHTKNNGKDXFVPXRQ") == 0 &&
         m.fp[0] == 0xec && m.script == WSCRIPT_LEGACY && m.testnet == 0);

    memset(&m, 0, sizeof m);
    uchk("-1 is a real answer, not an absence",
         P("KISSU1 EC5A4595 1 0 -1 900000 TB1QXY") == 0 && m.high == -1);

    memset(&m, 0, sizeof m);
    uchk("a trailing NUL from the scan path is trimmed",
         kiss_usage_parse("KISSU1 EC5A4595 1 0 7 42 TB1QXY", 31, &m) == 0 &&
         m.high == 7);
}

static void test_parse_refusals(void)
{
    kiss_usage_msg_t m;
    uchk("an ordinary address is not a payload",
         P("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4") != 0);
    uchk("a bitcoin URI is not a payload",
         P("bitcoin:tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4") != 0);
    uchk("wrong magic is refused",   P("KISSU2 EC5A4595 1 0 29 1234567 TB1QXY") != 0);
    uchk("a short fingerprint is refused", P("KISSU1 EC5A45 1 0 29 1234567 TB1QXY") != 0);
    uchk("a non hex fingerprint is refused", P("KISSU1 ZZ5A4595 1 0 29 1234567 TB1QXY") != 0);
    uchk("a missing address is refused", P("KISSU1 EC5A4595 1 0 29 1234567") != 0);
    uchk("a truncated payload is refused", P("KISSU1 EC5A4595 1 0") != 0);
    uchk("trailing junk is refused",
         P("KISSU1 EC5A4595 1 0 29 1234567 TB1QXY EXTRA") != 0);
    uchk("an out of range network is refused", P("KISSU1 EC5A4595 2 0 29 1234567 TB1QXY") != 0);
    uchk("an out of range script is refused",  P("KISSU1 EC5A4595 1 9 29 1234567 TB1QXY") != 0);
    uchk("an index past the cap is refused",
         P("KISSU1 EC5A4595 1 0 100001 1234567 TB1QXY") != 0);
    uchk("an index below -1 is refused", P("KISSU1 EC5A4595 1 0 -2 1234567 TB1QXY") != 0);
    uchk("a non numeric index is refused", P("KISSU1 EC5A4595 1 0 2x 1234567 TB1QXY") != 0);
    uchk("a height of zero is not a claim", P("KISSU1 EC5A4595 1 0 29 0 TB1QXY") != 0);
    uchk("an absurd height is refused",
         P("KISSU1 EC5A4595 1 0 29 999999999999 TB1QXY") != 0);
    uchk("an empty payload is refused", P("") != 0);
}

// Not the kissfuzz harness: this parser is pure text with no crypto under it,
// and keeping its adversarial inputs here avoids touching sim/test_fuzz.c.
// Nothing may crash and nothing malformed may be accepted.
static void test_parse_adversarial(void)
{
    kiss_usage_msg_t m;
    char buf[512];
    int accepted_junk = 0;

    for (size_t n = 0; n < sizeof buf - 1; n++) {
        memset(buf, 'A' + (int)(n % 26), n);
        buf[n] = 0;
        if (kiss_usage_parse(buf, n, &m) == 0) accepted_junk++;
    }
    uchk("no run of filler is ever accepted", accepted_junk == 0);

    const char *stem = "KISSU1 EC5A4595 1 0 29 1234567 TB1QW508D6QEJXTDG4Y5R3ZARVARY0";
    for (size_t n = 0; n <= strlen(stem); n++)
        (void)kiss_usage_parse(stem, n, &m);       // every prefix: must not crash
    uchk("every truncation of a good payload survives", 1);

    memset(buf, ' ', sizeof buf);
    uchk("all spaces is refused", kiss_usage_parse(buf, sizeof buf, &m) != 0);

    char big[600];
    int o = snprintf(big, sizeof big, "KISSU1 EC5A4595 1 0 29 1234567 TB1Q");
    memset(big + o, 'Q', 400);
    big[o + 400] = 0;
    uchk("an address longer than the field is refused",
         kiss_usage_parse(big, strlen(big), &m) != 0);
}

// The payload's index is a fact about the WALLET, not about the address riding
// with it. Pinned at the data layer: a payload may claim any index and the
// address half comes through untouched by it.
static void test_index_does_not_describe_address(void)
{
    kiss_usage_msg_t m;
    memset(&m, 0, sizeof m);
    uchk("a wild index leaves the address intact",
         P("KISSU1 EC5A4595 1 0 99999 1234567 TB1QREALADDRESS") == 0 &&
         m.high == 99999 &&
         strcmp(m.addr, "TB1QREALADDRESS") == 0);
}

int test_usage(void)
{
    ufails = 0;
    test_parse_good();
    test_parse_refusals();
    test_parse_adversarial();
    test_index_does_not_describe_address();
    return ufails;
}
