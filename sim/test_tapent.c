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

#include "wallet_tapent.h"

static void test_debounce(void)
{
    uint8_t chain[32];
    wallet_tapent_reset();
    ok("starts at zero", wallet_tapent_count() == 0);

    // first tap always counts: there is no predecessor to be too close to
    ok("first tap counts", wallet_tapent_tap(1000000, 5000, 100, 200) == 1);
    ok("count is 1", wallet_tapent_count() == 1);

    // 29ms later: below WTAP_DEBOUNCE_US, a panel artifact rather than a hand
    ok("29ms is rejected", wallet_tapent_tap(1029000, 5001, 100, 200) == 0);
    ok("count still 1", wallet_tapent_count() == 1);

    // 31ms later: a real tap
    ok("31ms is accepted", wallet_tapent_tap(1060000, 5002, 101, 201) == 1);
    ok("count is 2", wallet_tapent_count() == 2);

    // a rejected tap must not become the new predecessor, or a fast drag would
    // ratchet the window forward and let the next artifact through
    ok("30ms after a REJECTED tap is measured from the accepted one",
       wallet_tapent_tap(1080000, 5003, 102, 202) == 0);

    ok("not done at 2 taps", wallet_tapent_take(chain) != 0);
}

static void test_fold(void)
{
    uint8_t chain_a[32], chain_b[32];

    // 64 taps at a fixed cadence completes
    wallet_tapent_reset();
    for (int i = 0; i < WTAP_TARGET; i++)
        wallet_tapent_tap(1000000 + (uint64_t)i * 50000, (uint32_t)i, 10, 10);
    ok("64 taps reach the target", wallet_tapent_count() == WTAP_TARGET);
    ok("take succeeds at the target", wallet_tapent_take(chain_a) == 0);

    // 63 taps does not
    wallet_tapent_reset();
    for (int i = 0; i < WTAP_TARGET - 1; i++)
        wallet_tapent_tap(1000000 + (uint64_t)i * 50000, (uint32_t)i, 10, 10);
    ok("63 taps do not", wallet_tapent_take(chain_b) != 0);

    // identical timing but one differing cycle count must change the chain:
    // this is the property the whole feature rests on
    wallet_tapent_reset();
    for (int i = 0; i < WTAP_TARGET; i++)
        wallet_tapent_tap(1000000 + (uint64_t)i * 50000,
                          (uint32_t)(i == 7 ? 999999 : i), 10, 10);
    wallet_tapent_take(chain_b);
    ok("one differing cycle count changes the chain",
       memcmp(chain_a, chain_b, 32) != 0);

    // reset must not leave the previous session's chain behind
    wallet_tapent_reset();
    ok("reset clears the count", wallet_tapent_count() == 0);
    ok("reset clears doneness", wallet_tapent_take(chain_b) != 0);
}

int test_tapent(void)
{
    fails = 0;
    printf("\n-- tap entropy --\n");
    test_mix3();
    test_debounce();
    test_fold();
    return fails;
}
