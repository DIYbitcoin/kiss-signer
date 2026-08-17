// The KEF envelope: GCM against the spec vectors, the envelope against
// golden vectors produced by the reference implementation, and the sniff
// against every seed QR shape it must never claim.
//
// The goldens are the interop proof and their provenance matters: each was
// produced by the Krux project's kef.py (the format's reference) driving an
// independently validated AES-GCM, then decrypted back by the same before
// being baked here. If kiss_kef_open reads them and kiss_kef_seal (pinned
// IV) reproduces them byte for byte, a backup made on this signer opens on a
// Krux device and vice versa — without either device in the room.
#include <stdio.h>
#include <string.h>

#include "kiss_kef.h"
#include "kiss_seed.h"
#include "wally_bip39.h"

static int dfails;

static void dchk(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) dfails++;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned b;
        if (sscanf(s, "%2x", &b) != 1) return 0;
        out[n++] = (uint8_t)b;
        s += 2;
    }
    return n;
}

// ---- AES-256-GCM spec vectors (keystream + full tag) --------------------
// First two are the published zero-key test cases; all four cross-checked
// against an independently validated implementation.
static const struct {
    const char *name, *key, *iv, *pt, *ct, *tag;
} GCM[] = {
    { "gcm zero key, empty pt",
      "0000000000000000000000000000000000000000000000000000000000000000",
      "000000000000000000000000", "", "",
      "530f8afbc74536b9a963b4f1c4cb738b" },
    { "gcm zero key, one block",
      "0000000000000000000000000000000000000000000000000000000000000000",
      "000000000000000000000000",
      "00000000000000000000000000000000",
      "cea7403d4d606b6e074ec5d3baf39d18",
      "d0d1c8a799996bf0265b98b5d48ab919" },
    { "gcm two blocks",
      "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
      "cafebabefacedbaddecaf888",
      "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72",
      "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa",
      "35c7d52cb3badf61223e2d2f98ce8ee7" },
    { "gcm ragged tail",
      "1111111111111111111111111111111111111111111111111111111111111111",
      "222222222222222222222222", "33333333333333333333333333",
      "24c4347af3fcac6cd60ced0f7b",
      "3f7cf930e520f08b97413314a5f546cc" },
};

static void test_gcm_vectors(void)
{
    for (size_t i = 0; i < sizeof GCM / sizeof GCM[0]; i++) {
        uint8_t key[32], iv[12], pt[64], want_ct[64], want_tag[16];
        uint8_t ct[64] = { 0 }, tag[16] = { 0 };
        unhex(GCM[i].key, key, sizeof key);
        unhex(GCM[i].iv, iv, sizeof iv);
        size_t n = unhex(GCM[i].pt, pt, sizeof pt);
        unhex(GCM[i].ct, want_ct, sizeof want_ct);
        unhex(GCM[i].tag, want_tag, sizeof want_tag);
        int rc = kiss_kef_test_gcm(key, iv, pt, n, ct, tag);
        dchk(GCM[i].name, rc == 0 && memcmp(ct, want_ct, n) == 0
                              && memcmp(tag, want_tag, 16) == 0);
    }
}

// ---- golden envelopes from the reference implementation -----------------
static const struct {
    const char *name, *password, *iv, *plain, *env;
    int reseal;                 // 1 when iterations match what seal writes
} GOLD[] = {
    { "golden: fingerprint id, 16B entropy", "test password",
      "000102030405060708090a0b",
      "00112233445566778899aabbccddeeff",
      "0837334335444130411400000a000102030405060708090a0b"
      "1ca567170a34839c507e98865210aba80de4fc5a", 1 },
    { "golden: empty id, 32B entropy, raw iterations", "KISS",
      "f0e1d2c3b4a5968778695a4b",
      "7f0623ab29bd6f9a9895e9ac8b57d2ee0af21c1e7fa4b1c0eafe0c9d2b6f7d31",
      "0014002711f0e1d2c3b4a5968778695a4bcc419726eba0ef6230dbe8d3b95ad9"
      "a33486c2b831c31fb31be383d7c03e1414e85c5ae2", 0 },
    { "golden: utf-8 password, text plaintext", "sat\xc3\xb3shi nakam\xc3\xb3to",
      "000102030405060708090a0b",
      "637261776c2061696d2062726965662067617370206272696566206d696e6420"
      "6a617a7a2062656c7420746f6e677565206b6e6f77207374617920746f6e65",
      "066261636b757014000019000102030405060708090a0b01745064a89f26c45f"
      "a20099afcf0f2fe00699a29d520075d2aaa4ca9112a244975c2b54b7a16988e2"
      "90bd90c475cf11ceb392a7ba034fab5a4c46b6d0200c053710f3", 0 },
    { "golden: NUL tail plaintext, 32 char id", "x",
      "aabbccddeeff001122334455", "deadbeef00",
      "2041414141414141414141414141414141414141414141414141414141414141"
      "411400000aaabbccddeeff0011223344552c1b610bdf325a7261", 1 },
};

static void test_goldens(void)
{
    for (size_t i = 0; i < sizeof GOLD / sizeof GOLD[0]; i++) {
        uint8_t env[KEF_MAX_ENV], want_plain[128], plain[128];
        size_t elen = unhex(GOLD[i].env, env, sizeof env);
        size_t wlen = unhex(GOLD[i].plain, want_plain, sizeof want_plain);
        size_t plen = 0;
        int rc = kiss_kef_open(GOLD[i].password, strlen(GOLD[i].password),
                               env, elen, plain, sizeof plain, &plen);
        dchk(GOLD[i].name, rc == 0 && plen == wlen
                               && memcmp(plain, want_plain, wlen) == 0);

        if (!GOLD[i].reseal) continue;
        kef_env_t e;
        uint8_t iv[KEF_IV_LEN], out[KEF_MAX_ENV];
        size_t olen = 0;
        unhex(GOLD[i].iv, iv, sizeof iv);
        kef_parse(env, elen, &e);
        kiss_kef_test_fix_iv(iv);
        rc = kiss_kef_seal(e.id, e.id_len, GOLD[i].password,
                           strlen(GOLD[i].password), want_plain, wlen,
                           out, sizeof out, &olen);
        dchk("  ...and seal reproduces it byte for byte",
             rc == 0 && olen == elen && memcmp(out, env, elen) == 0);
    }
}

// ---- iteration rule edges ----------------------------------------------
static size_t mk_env(uint32_t iter_raw, uint8_t version, size_t payload_len,
                     uint8_t *out, size_t cap)
{
    size_t h = kef_emit_header(out, cap, (const uint8_t *)"id", 2, version,
                               iter_raw);
    if (!h || h + payload_len > cap) return 0;
    memset(out + h, 0x5a, payload_len);
    return h + payload_len;
}

static void test_iterations(void)
{
    uint8_t env[64];
    kef_env_t e;

    size_t n = mk_env(10, 20, 17, env, sizeof env);
    dchk("stored 10 -> effective 100000",
         kef_parse(env, n, &e) == 0 && e.iter_eff == 100000);

    n = mk_env(10001, 20, 17, env, sizeof env);
    dchk("stored 10001 -> effective 10001",
         kef_parse(env, n, &e) == 0 && e.iter_eff == 10001);

    n = mk_env(10000, 20, 17, env, sizeof env);
    dchk("stored 10000 -> effective 100M, parses",
         kef_parse(env, n, &e) == 0 && e.iter_eff == 100000000);
    uint8_t plain[64];
    size_t plen;
    dchk("  ...but open refuses over the cap",
         kiss_kef_open("x", 1, env, n, plain, sizeof plain, &plen) == -1);

    // raw zero cannot be emitted, and a hand-built zero cannot parse
    dchk("emit refuses raw 0", kef_emit_header(env, sizeof env, NULL, 0,
                                               20, 0) == 0);
    n = mk_env(1, 20, 17, env, sizeof env);
    env[4] = 0;                                  // id "id" -> iters at 4..6
    env[5] = 0;
    env[6] = 0;
    dchk("raw 0 refused by parse", kef_parse(env, n, &e) == -1);
}

// ---- roundtrips and refusals -------------------------------------------
static void test_roundtrip(void)
{
    uint8_t plain16[16], plain32[32], env[KEF_MAX_ENV], back[64];
    size_t elen = 0, blen = 0;
    for (int i = 0; i < 16; i++) plain16[i] = (uint8_t)(0xa0 + i);
    for (int i = 0; i < 32; i++) plain32[i] = (uint8_t)(0x30 + i);

    dchk("roundtrip 16B entropy, 8 char id",
         kiss_kef_seal((const uint8_t *)"73C5DA0A", 8, "pw", 2, plain16, 16,
                       env, sizeof env, &elen) == 0
             && kiss_kef_open("pw", 2, env, elen, back, sizeof back,
                              &blen) == 0
             && blen == 16 && memcmp(back, plain16, 16) == 0);

    dchk("wrong password refused, output zeroed",
         kiss_kef_open("pW", 2, env, elen, back, sizeof back, &blen) == -1
             && blen == 0 && back[0] == 0 && back[15] == 0);

    // flip every byte once: parse or tag must refuse each
    int tampered_ok = 1;
    for (size_t i = 0; i < elen; i++) {
        env[i] ^= 0xff;
        if (kiss_kef_open("pw", 2, env, elen, back, sizeof back,
                          &blen) == 0) tampered_ok = 0;
        env[i] ^= 0xff;
    }
    dchk("every single byte tamper refused", tampered_ok);

    int truncated_ok = 1;
    for (size_t i = 0; i < elen; i++)
        if (kiss_kef_open("pw", 2, env, i, back, sizeof back, &blen) == 0)
            truncated_ok = 0;
    dchk("every truncation refused", truncated_ok);

    // still opens after the sweep (the sweep restored every byte)
    dchk("envelope survives the sweep intact",
         kiss_kef_open("pw", 2, env, elen, back, sizeof back, &blen) == 0);

    uint8_t id252[252];
    memset(id252, 'K', sizeof id252);
    dchk("roundtrip 32B entropy, empty id",
         kiss_kef_seal(NULL, 0, "pw2", 3, plain32, 32, env, sizeof env,
                       &elen) == 0
             && kiss_kef_open("pw2", 3, env, elen, back, sizeof back,
                              &blen) == 0
             && blen == 32 && memcmp(back, plain32, 32) == 0);
    dchk("roundtrip max 252 byte id",
         kiss_kef_seal(id252, 252, "pw3", 3, plain16, 16, env, sizeof env,
                       &elen) == 0
             && kiss_kef_open("pw3", 3, env, elen, back, sizeof back,
                              &blen) == 0
             && blen == 16 && memcmp(back, plain16, 16) == 0);
    dchk("id over 252 refused by seal",
         kiss_kef_seal(id252, 253, "pw", 2, plain16, 16, env, sizeof env,
                       &elen) == -1);
    dchk("empty password refused by seal",
         kiss_kef_seal(NULL, 0, "", 0, plain16, 16, env, sizeof env,
                       &elen) == -1);

    // every other version refused identically by open
    kiss_kef_seal((const uint8_t *)"id", 2, "pw", 2, plain16, 16, env,
                  sizeof env, &elen);
    const uint8_t others[] = { 0, 1, 5, 6, 7, 10, 11, 12, 15, 16, 21, 99 };
    int versions_ok = 1;
    for (size_t i = 0; i < sizeof others; i++) {
        env[3] = others[i];                      // version byte after "id"
        if (kiss_kef_open("pw", 2, env, elen, back, sizeof back,
                          &blen) != -1) versions_ok = 0;
    }
    env[3] = KEF_VERSION_AES_GCM;
    dchk("all non-20 versions refused by open", versions_ok);
}

// ---- the sniff must never claim a seed QR shape -------------------------
static void test_sniff(void)
{
    const char *words =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    dchk("sniff: text mnemonic is not KEF",
         kef_sniff((const uint8_t *)words, strlen(words)) == 0);

    char digits[97];
    memset(digits, '0', 96);
    digits[96] = 0;
    dchk("sniff: 96 digit SeedQR is not KEF",
         kef_sniff((const uint8_t *)digits, 96) == 0);
    dchk("sniff: 48 digit SeedQR is not KEF",
         kef_sniff((const uint8_t *)digits, 48) == 0);

    // worst-case CompactSeedQR bytes crafted to look as KEF-ish as a 16/32
    // byte buffer can: the length rule alone must throw both out
    uint8_t entropy[32] = { 0 };
    entropy[0] = 0;                              // len_id 0
    entropy[1] = 20;                             // version 20
    entropy[4] = 10;                             // iterations 10
    dchk("sniff: 16B CompactSeedQR is not KEF", kef_sniff(entropy, 16) == 0);
    dchk("sniff: 32B CompactSeedQR is not KEF", kef_sniff(entropy, 32) == 0);

    uint8_t env[64], plain16[16] = { 1 };
    size_t elen = 0;
    kiss_kef_seal((const uint8_t *)"73C5DA0A", 8, "pw", 2, plain16, 16, env,
                  sizeof env, &elen);
    dchk("sniff: a real envelope is KEF", kef_sniff(env, elen) == 1);
    env[9] = 15;                                 // other versions still route
    dchk("sniff: a CTR envelope still routes to KEF",
         kef_sniff(env, elen) == 1);
}

// ---- the whole backup path against the stored dev seed ------------------
static void test_seal_seed(void)
{
    char words[WSEED_MAX_MNEMONIC];
    if (kiss_seed_load(words, sizeof words) != 0) {
        dchk("seal_seed: dev seed present", 0);
        return;
    }
    uint8_t env[KEF_MAX_ENV], plain[64];
    size_t elen = 0, plen = 0;
    char id_hex[9] = { 0 };
    int rc = kiss_kef_seal_seed(words, "correct horse", 13, env, sizeof env,
                                &elen, id_hex);
    dchk("seal_seed makes an envelope", rc == 0 && elen > 0
                                            && strlen(id_hex) == 8);

    kef_env_t e;
    dchk("  ...with the fingerprint as the visible id",
         kef_parse(env, elen, &e) == 0 && e.id_len == 8
             && memcmp(e.id, id_hex, 8) == 0
             && e.iter_eff == 100000);

    char *mnem = NULL;
    rc = kiss_kef_open("correct horse", 13, env, elen, plain, sizeof plain,
                       &plen);
    dchk("  ...that opens back to entropy", rc == 0
                                                && (plen == 16 || plen == 32));
    rc = bip39_mnemonic_from_bytes(NULL, plain, plen, &mnem);
    dchk("  ...that rebuilds the exact stored words",
         rc == WALLY_OK && mnem && strcmp(mnem, words) == 0);
    if (mnem) wally_free_string(mnem);

    memset(words, 0, sizeof words);
    memset(plain, 0, sizeof plain);
}

int test_kef(void)
{
    dfails = 0;
    test_gcm_vectors();
    test_goldens();
    test_iterations();
    test_roundtrip();
    test_sniff();
    test_seal_seed();
    return dfails;
}
