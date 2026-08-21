// The coordinator's usage payload: the parser, the height gate, and the facts
// the Receive chip is built out of. No LVGL here — kisstest links kiss_usage.c
// and not kiss_recv.c, so everything below the screen is provable on desktop.
#include <stdio.h>
#include <string.h>

#include "kiss_crypto.h"   // WSCRIPT_*
#include "kiss_usage.h"

void kiss_seed_test_set_flash_encrypted(int on);   // sim/test_backup.c owns the hook

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

// A coordinator's claim is stored beside — never merged into — what this device
// witnessed. The two answer different questions and only the local one is
// evidence, so only the local one is monotonic.
static void test_chain_store(void)
{
    const uint8_t fp[4] = { 0xec, 0x5a, 0x45, 0x95 };
    int high; uint32_t height;

    kiss_usage_wipe();
    uchk("nothing is known before a coordinator speaks",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 0);

    uchk("a first claim is accepted",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 29, 1000) == 1);
    uchk("  ...and reads back",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 29 && height == 1000);

    uchk("an older claim is inert",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 99, 999) == 0);
    uchk("  ...and changed nothing",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 29);
    uchk("re-showing the same QR is inert",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 29, 1000) == 0);

    uchk("a newer claim lands even when it LOWERS the index",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 12, 1001) == 1);
    uchk("  ...because the coordinator is the chain's source of truth",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 12 && height == 1001);

    uchk("none used is a real answer",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, -1, 1002) == 1 &&
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == -1);

    uchk("a height of zero is not a claim",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 5, 0) == 0);
    uchk("an index past the cap is refused",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, KISS_USAGE_MAX_INDEX + 1, 2000) == 0);

    // Buckets do not bleed: another address type is another account key.
    uchk("another script type is its own bucket",
         kiss_usage_chain_known(fp, 1, WSCRIPT_LEGACY, &high, &height) == 0);
    uchk("another network is its own bucket",
         kiss_usage_chain_known(fp, 0, WSCRIPT_NATIVE, &high, &height) == 0);
    const uint8_t other[4] = { 0x00, 0x11, 0x22, 0x33 };
    uchk("another fingerprint is its own bucket",
         kiss_usage_chain_known(other, 1, WSCRIPT_NATIVE, &high, &height) == 0);

    // The local mark is evidence and a camera may not lower it.
    kiss_usage_mark(fp, 1, WSCRIPT_NATIVE, 40);
    uchk("a coordinator claim never touches the local mark",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 3, 3000) == 1 &&
         kiss_usage_high(fp, 1, WSCRIPT_NATIVE) == 40);
    uchk("  ...and the claim is still its own answer",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 3);

    kiss_usage_wipe();
    uchk("a wipe takes the coordinator's claim with it",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 0);
    uchk("  ...and the local mark with it",
         kiss_usage_high(fp, 1, WSCRIPT_NATIVE) == -1);
}

// The branch above only reaches the session table: may_persist() also wants
// flash encryption, and the sim is left on the beta lane where it is off. The
// stored path has the two things the session path does not -- a write that
// outlives the RAM table, and the promote-on-read that pulls it back in -- so
// it gets its own pass with the hook on.
static void test_chain_store_persistent(void)
{
    const uint8_t fp[4] = { 0xab, 0xcd, 0xef, 0x01 };
    int high; uint32_t height;

    kiss_seed_test_set_flash_encrypted(1);
    kiss_usage_wipe();

    uchk("stored: a claim is accepted",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NESTED, 17, 5000) == 1);
    kiss_usage_forget_session();          // the RAM table is gone; storage is not
    uchk("stored: it survives the session table",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NESTED, &high, &height) == 1 &&
         high == 17 && height == 5000);
    uchk("stored: the height gate still holds after a promote",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NESTED, 99, 4999) == 0);
    uchk("stored: and a newer claim still lands",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NESTED, 4, 5001) == 1 &&
         kiss_usage_chain_known(fp, 1, WSCRIPT_NESTED, &high, &height) == 1 &&
         high == 4);

    // A session-only claim promoted by the flush, which is the door a mode
    // change comes through.
    kiss_seed_test_set_flash_encrypted(0);
    uchk("stored: a beta lane claim is session only",
         kiss_usage_chain_set(fp, 0, WSCRIPT_NATIVE, 8, 6000) == 1);
    kiss_seed_test_set_flash_encrypted(1);
    kiss_usage_persist_session();
    kiss_usage_forget_session();
    uchk("stored: the flush carried it across",
         kiss_usage_chain_known(fp, 0, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 8 && height == 6000);

    kiss_usage_wipe();
    uchk("stored: a wipe clears it",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NESTED, &high, &height) == 0);
    kiss_seed_test_set_flash_encrypted(0);   // leave the sim on the beta lane
}

int test_usage(void)
{
    ufails = 0;
    test_parse_good();
    test_parse_refusals();
    test_parse_adversarial();
    test_index_does_not_describe_address();
    test_chain_store();
    test_chain_store_persistent();
    return ufails;
}
