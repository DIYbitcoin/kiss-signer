// Host tests for the tap-entropy fold and the three-way mix.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "wallet_crypto.h"

static int fails;

static void ok(const char *name, int cond)
{
    if (cond) { printf("PASS: %s\n", name); }
    else      { printf("FAIL: %s\n", name); fails++; }
}

// SHA256 of 96 bytes: 0x00 x32 ‖ 0x11 x32 ‖ 0x22 x32.
// Independently computed:
//   python3 -c "import hashlib;print(hashlib.sha256(bytes(32)+b'\x11'*32+b'\x22'*32).hexdigest())"
static const char *MIX3_ABC =
    "b647d2614ad2099840c899399acf3b264e8f40f9eb0d0f997c2e6e1c3ffc2da6";

static void hex32(const uint8_t h[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}

static void test_mix3(void)
{
    uint8_t a[32], b[32], c[32], out[32], out2[32];
    memset(a, 0x00, 32); memset(b, 0x11, 32); memset(c, 0x22, 32);

    ok("mix3 returns 0", wallet_entropy_mix3(a, b, c, out) == 0);

    char got[65]; hex32(out, got);
    ok("mix3 matches the independent vector", strcmp(got, MIX3_ABC) == 0);
    if (strcmp(got, MIX3_ABC) != 0) printf("  got %s\n  want %s\n", got, MIX3_ABC);

    // order matters: a hash that ignored ordering would let whoever controls
    // one source decide which of the three dominates
    wallet_entropy_mix3(c, b, a, out2);
    ok("mix3 is order sensitive", memcmp(out, out2, 32) != 0);

    // every input reaches the digest
    a[31] ^= 1; wallet_entropy_mix3(a, b, c, out2);
    ok("mix3 depends on a", memcmp(out, out2, 32) != 0);
    a[31] ^= 1; b[31] ^= 1; wallet_entropy_mix3(a, b, c, out2);
    ok("mix3 depends on b", memcmp(out, out2, 32) != 0);
    b[31] ^= 1; c[31] ^= 1; wallet_entropy_mix3(a, b, c, out2);
    ok("mix3 depends on c", memcmp(out, out2, 32) != 0);

    ok("mix3 rejects NULL", wallet_entropy_mix3(NULL, b, c, out) != 0);
}

int test_tapent(void)
{
    fails = 0;
    printf("\n-- tap entropy --\n");
    test_mix3();
    return fails;
}
