// Host tests for the dice-entropy module. The SHA256 vectors are the proof of
// verifiability: they must equal `printf '<rolls>' | sha256sum`.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "wally_core.h"
#include "wally_crypto.h"
#include "wallet_dice.h"
#include "wallet_seed.h"

static int fails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); fails++; }
}

static void hex(const uint8_t *b, unsigned n, char *out)
{
    for (unsigned i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
}

static void roll_str(const char *s)
{
    wallet_dice_reset();
    for (const char *p = s; *p; p++) wallet_dice_roll(*p - '0');
}

// printf '12345'*10 (50 chars) | sha256sum
static const char *KAT50 =
    "5eca9288344f8143aa96673f67faf41314a3157e2b4defca2acfafb6f6c29fbd";
// printf '123456'*16 '123' (99 chars) | sha256sum
static const char *KAT99 =
    "5588d3630bd19f6375b7bd922457af34ea9c74f00807566a1cf808e445dc8c20";
static const char *R50 =
    "12345123451234512345123451234512345123451234512345";
static const char *R99 =
    "123456123456123456123456123456123456123456123456"
    "123456123456123456123456123456123456123456123456123";

int test_dice(void)
{
    fails = 0;
    printf("\n-- dice entropy --\n");

    // roll / count / invalid / undo
    wallet_dice_reset();
    ok("starts empty", wallet_dice_count() == 0);
    ok("valid face accepted", wallet_dice_roll(4) == 1);
    ok("count is 1", wallet_dice_count() == 1);
    ok("face 0 rejected", wallet_dice_roll(0) == 0);
    ok("face 7 rejected", wallet_dice_roll(7) == 0);
    ok("count unchanged after invalid", wallet_dice_count() == 1);
    ok("undo removes it", wallet_dice_undo() == 1);
    ok("empty again", wallet_dice_count() == 0);
    ok("undo on empty is 0", wallet_dice_undo() == 0);
    wallet_dice_roll(1); wallet_dice_roll(2);
    ok("digits reflect rolls", strcmp(wallet_dice_digits(), "12") == 0);

    // ---- floor gate ----
    uint8_t e[32]; char got[65];
    roll_str(R50);                                  // 50 rolls
    ok("50 rolls: 12-word take ok", wallet_dice_take(e, 16) == 0);
    ok("50 rolls: 24-word take refused", wallet_dice_take(e, 32) == -1);
    wallet_dice_undo();                             // 49 rolls
    ok("49 rolls: 12-word take refused", wallet_dice_take(e, 16) == -1);

    // ---- KAT: 12-word entropy == first 16 bytes of SHA256(R50) ----
    roll_str(R50);
    wallet_dice_take(e, 16); hex(e, 16, got);
    ok("12-word entropy == SHA256(R50)[0..16]", strncmp(got, KAT50, 32) == 0);
    if (strncmp(got, KAT50, 32) != 0) printf("  got %s\n  want %.32s\n", got, KAT50);

    // ---- KAT: 24-word entropy == SHA256(R99) ----
    roll_str(R99);
    ok("99 rolls: 24-word take ok", wallet_dice_take(e, 32) == 0);
    hex(e, 32, got);
    ok("24-word entropy == SHA256(R99)", strcmp(got, KAT99) == 0);
    if (strcmp(got, KAT99) != 0) printf("  got %s\n  want %s\n", got, KAT99);

    // ---- entropy -> a real, deterministic BIP39 mnemonic ----
    // (self-consistency here; the human off-device cross-check against an
    // external BIP39 tool is the device-acceptance step in the spec.)
    char words[256], words2[256];
    ok("entropy -> mnemonic rc",
       wallet_seed_from_entropy(e, 32, words, sizeof words) == 0);
    roll_str(R99); wallet_dice_take(e, 32);
    ok("same rolls -> same mnemonic",
       wallet_seed_from_entropy(e, 32, words2, sizeof words2) == 0 &&
       strcmp(words, words2) == 0);

    // ---- length validation + wipe ----
    ok("bad len rejected", wallet_dice_take(e, 20) == -1);
    ok("NULL out rejected", wallet_dice_take(NULL, 32) == -1);
    wallet_dice_reset();
    ok("reset clears count", wallet_dice_count() == 0);
    ok("reset clears digits", wallet_dice_digits()[0] == '\0');

    return fails;
}
