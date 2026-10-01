// The KEF envelope: GCM against the spec vectors, the envelope against
// golden vectors produced by the reference implementation, and the sniff
// against every plaintext shape it must never claim.
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

// ---- Krux's other versions: read only -----------------------------------
// The first six are Krux's own stored vectors (tests/test_encryption.py,
// password "test key"), including the two its pre-25.09 firmware wrote
// (v0, v1). The rest came out of Krux's src/krux/kef.py driving pycryptodome
// the way its test suite does, each opened back by the same code first: the
// shapes a seed backup takes in every uncompressed mode, a text mnemonic, and
// the NUL padding cases older firmware could write, where a check or a
// plaintext ends in 0x00 and only the reader's retry finds it.
static const struct {
    const char *name, *password, *env, *plain;
} KRUX[] = {
    { "krux vector: oldecb qr (v0)", "test key",
      "07746573742049440000000a2ae19dc582c1199bb726f23f03c76ff6af9e8123"
      "462c5173e61debd159a02fcf",
      "350e7b31a36727c5f2fc76b58304668c" },
    { "krux vector: oldcbc qr (v1)", "test key",
      "07746573742049440100000a4f52a1936c3e3271209e9d64059ed78e01036075"
      "5fd7ab2f4ebc4019cc0a22c58a5e337874a4b30b4bca8a4082da7ad3",
      "350e7b31a36727c5f2fc76b58304668c" },
    { "krux vector: ecb qr (v5)", "test key",
      "07746573742049440500000a2ae19dc582c1199bb726f23f03c76ff6523e23",
      "350e7b31a36727c5f2fc76b58304668c" },
    { "krux vector: cbc qr (v10)", "test key",
      "07746573742049440a00000a4f52a1936c3e3271209e9d64059ed78e01036075"
      "5fd7ab2f4ebc4019cc0a22c543fa9690",
      "350e7b31a36727c5f2fc76b58304668c" },
    { "krux vector: ctr qr (v15)", "test key",
      "07746573742049440f00000a4f52a1936c3e3271209e9d64f49120c7a26d12de"
      "7b0ff0268c9691e689b02494f0af52c21c295b57b5347e9a86ee2a40",
      "ec54ea346caa1ed06cb35c96c4ef23228a26a2e0996c71d0608ec2ba49ce2316" },
    { "krux vector: gcm qr (v20)", "test key",
      "07746573742049441400000a4f52a1936c3e3271209e9d64bfb7766f5d5d8a4f"
      "908e86e738344c025d8fed54",
      "350e7b31a36727c5f2fc76b58304668c" },
    { "krux kef.py: v0 32B", "kiss test",
      "0837334335444130410000000ab8618190ce182b6bd317964a57bc39d810b7f8"
      "61e5f613b8eee67b344cbb805438a8eacbe90c91b68e40efbda2132322",
      "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f" },
    { "krux kef.py: v1 32B", "kiss test",
      "0837334335444130410100000aa0a1a2a3a4a5a6a7a8a9aaabacadaeafab6862"
      "bba92b2ca860ca4a3952b5052b9e1a63bddcd6ad6f0487736b9c13728ce89e2d"
      "3e8b975e56d440c8ed050210fd",
      "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f" },
    { "krux kef.py: v5 32B", "kiss test",
      "0837334335444130410500000ab8618190ce182b6bd317964a57bc39d810b7f8"
      "61e5f613b8eee67b344cbb805456f351",
      "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f" },
    { "krux kef.py: v10 20B", "kiss test",
      "0837334335444130410a00000aa0a1a2a3a4a5a6a7a8a9aaabacadaeafe2aab1"
      "0d8a8d834bb77e5e78f178cb2b1f6ab9c9d84b01e1a66016fb648110703174f1"
      "b8",
      "707172737475767778797a7b7c7d7e7f80818283" },
    { "krux kef.py: v15 24B", "kiss test",
      "0837334335444130410f00000ac0c1c2c3c4c5c6c7c8c9cacb1954605a6042c9"
      "6bd3bc5311ef0962b71d6ccced146b0100f86764ba",
      "1112131415161718191a1b1c1d1e1f202122232425262728" },
    { "krux kef.py: v6 16B ending 00", "kiss test",
      "0837334335444130410600000a86f106a71960587ecf76721c900b6bca081e47"
      "58848472473b05f68176dd6243",
      "0102030405060708090a0b0c0d0e0f00" },
    { "krux kef.py: v11 16B ending 00", "kiss test",
      "0837334335444130410b00000aa0a1a2a3a4a5a6a7a8a9aaabacadaeaf1b83ba"
      "b7e344d07cc922c2a80e8d7908970ccf8df1738f955c85139965a7874b",
      "0102030405060708090a0b0c0d0e0f00" },
    { "krux kef.py: v11 text mnemonic", "kiss test",
      "0837334335444130410b00000aa0a1a2a3a4a5a6a7a8a9aaabacadaeaf66e6e3"
      "223b4de4a9a577be6c24b40429d4031462a4f494de0310be489d8065441955dd"
      "81640d0ce5ec2ba702246ae63f8348198f2479595f924c8c1522acdd5f13479a"
      "1b1d5038df1062792da157be3d",
      "637275736820696e686572697420736d616c6c2065676720696e636c75646520"
      "7469746c6520736c6f67616e206d6f6d2072656d61696e20626c6f7573652062"
      "6f6f737420626f6e7573" },
    { "krux kef.py: v15 text mnemonic", "kiss test",
      "0837334335444130410f00000ac0c1c2c3c4c5c6c7c8c9cacb6b34063d1d74b7"
      "1da2c33a6486370efa5d2283e9542a4108c3c834743491420948574c8f779414"
      "ebcab9b7a1dffcea01655b009e82e5ae8d98bff82112567867f9ddd7b2fd7afc"
      "00bce0a2b943d2",
      "637275736820696e686572697420736d616c6c2065676720696e636c75646520"
      "7469746c6520736c6f67616e206d6f6d2072656d61696e20626c6f7573652062"
      "6f6f737420626f6e7573" },
    { "krux kef.py: v0 auth ends 00", "kiss test",
      "0837334335444130410000000a64acc48dd5f402dbff313d4a05b576f7990a6f"
      "27235c0ec37e3e82e31ab95ac0",
      "2515d9435df146d1588e66959f9ec3e3" },
    { "krux kef.py: v1 auth ends 00", "kiss test",
      "0837334335444130410100000aa0a1a2a3a4a5a6a7a8a9aaabacadaeaf1ae764"
      "27970bc7093f20acf515c8fbde2a2829376c22a96d365afa02e1f8b62b",
      "2515d9435df146d1588e66959f9ec3e3" },
    { "krux kef.py: v5 plain ends 00", "kiss test",
      "0837334335444130410500000a86f106a71960587ecf76721c900b6bca178f06",
      "0102030405060708090a0b0c0d0e0f00" },
    { "krux kef.py: v10 plain ends 00", "kiss test",
      "0837334335444130410a00000aa0a1a2a3a4a5a6a7a8a9aaabacadaeaf1b83ba"
      "b7e344d07cc922c2a80e8d79089f6a9f9b",
      "0102030405060708090a0b0c0d0e0f00" },
};

static void test_krux_versions(void)
{
    for (size_t i = 0; i < sizeof KRUX / sizeof KRUX[0]; i++) {
        uint8_t env[KEF_MAX_ENV], want[128], plain[128];
        size_t elen = unhex(KRUX[i].env, env, sizeof env);
        size_t wlen = unhex(KRUX[i].plain, want, sizeof want);
        size_t plen = 0;
        const char *pw = KRUX[i].password;
        int rc = kiss_kef_open(pw, strlen(pw), env, elen, plain, sizeof plain,
                               &plen);
        dchk(KRUX[i].name, rc == 0 && plen == wlen
                               && memcmp(plain, want, wlen) == 0);

        int wrong_ok = kiss_kef_open("wrong", 5, env, elen, plain,
                                     sizeof plain, &plen) == -1 && plen == 0;
        for (size_t k = 0; k < sizeof plain; k++)
            if (plain[k]) wrong_ok = 0;
        // One flip in every 16 bytes of the payload and one in its last
        // byte, so every iv, every cipher block and every appended check is
        // hit. The header is left to the goldens and test_iterations, and
        // every flip here costs a full key derivation.
        kef_env_t e;
        int flip_ok = kef_parse(env, elen, &e) == 0;
        size_t first = flip_ok ? (size_t)(e.payload - env) : elen;
        for (size_t k = first; flip_ok && k < elen;
             k = (k + 16 < elen || k == elen - 1) ? k + 16 : elen - 1) {
            env[k] ^= 0x01;
            if (kiss_kef_open(pw, strlen(pw), env, elen, plain, sizeof plain,
                              &plen) == 0) flip_ok = 0;
            env[k] ^= 0x01;
        }
        dchk("  ...wrong password refused, output zeroed", wrong_ok);
        dchk("  ...a flip in every payload block refused", flip_ok);
    }

    const uint8_t opens[] = { 0, 1, 5, 6, 10, 11, 15, 20 };
    const uint8_t never[] = { 2, 7, 12, 16, 21, 99 };
    int can_ok = 1;
    for (size_t i = 0; i < sizeof opens; i++)
        if (!kef_can_open(opens[i])) can_ok = 0;
    for (size_t i = 0; i < sizeof never; i++)
        if (kef_can_open(never[i])) can_ok = 0;
    dchk("kef_can_open: every uncompressed version, no compressed one",
         can_ok);

    // The plaintext buffer is the bound: too small refuses, zeroed.
    uint8_t env[KEF_MAX_ENV], small[16];
    size_t elen = unhex(KRUX[6].env, env, sizeof env), plen = 1;
    memset(small, 0xee, sizeof small);
    int small_ok = kiss_kef_open(KRUX[6].password, strlen(KRUX[6].password),
                                 env, elen, small, sizeof small, &plen) == -1
                   && plen == 0;
    for (size_t k = 0; k < sizeof small; k++)
        if (small[k]) small_ok = 0;
    dchk("older version too big for the caller's buffer refused, zeroed",
         small_ok);
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

    // The same bytes under any other version byte refused identically by
    // open: the compressed and unknown ones outright, the older ones because
    // their own check fails. IV pinned so the result cannot vary by run.
    static const uint8_t fixed_iv[KEF_IV_LEN] = { 7, 7, 7, 7, 7, 7,
                                                  7, 7, 7, 7, 7, 7 };
    kiss_kef_test_fix_iv(fixed_iv);
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
    dchk("a v20 envelope under every other version byte refused",
         versions_ok);
}

// ---- the sniff must never claim a plaintext shape -----------------------
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
    dchk("sniff: 96 ASCII digits is not KEF",
         kef_sniff((const uint8_t *)digits, 96) == 0);
    dchk("sniff: 48 ASCII digits is not KEF",
         kef_sniff((const uint8_t *)digits, 48) == 0);

    // worst-case raw entropy crafted to look as KEF-ish as a 16/32 byte
    // buffer can: the length rule alone must throw both out. This is the
    // shape a real envelope's own plaintext takes, so a hit here would let
    // an opened backup be mistaken for another envelope.
    uint8_t entropy[32] = { 0 };
    entropy[0] = 0;                              // len_id 0
    entropy[1] = 20;                             // version 20
    entropy[4] = 10;                             // iterations 10
    dchk("sniff: 16B raw entropy is not KEF", kef_sniff(entropy, 16) == 0);
    dchk("sniff: 32B raw entropy is not KEF", kef_sniff(entropy, 32) == 0);

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

// ---- the armor a Krux envelope arrives wearing ---------------------------
// The envelope below is synthetic and deliberately so: a valid v20 header
// with no crypto behind it, so this proves the ARMOR and nothing else. Both
// strings were produced by python3 outside this file -- base64 from stdlib,
// base43 from the big endian base conversion Electrum defines -- so a decoder
// that agrees with them agrees with something that is not our own encoder.
static void test_armor(void)
{
    static const uint8_t ENV[41] = {
        0x04,'T','E','S','T', 0x14, 0x00,0x00,0x0a,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
        0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
        0xaa,0xbb,0xcc,0xdd
    };
    static const char B43[] =
        "3+Z4DWYYSF-:$S4.SCQ93MYSAG*OBFZD-H8HEY:./Q.KQMXJ7V:NZS/Q-UQ1";
    static const char B64[] =
        "BFRFU1QUAAAKAQIDBAUGBwgJCgsMQEFCQ0RFRkdISUpLTE1OT6q7zN0=";

    uint8_t out[KEF_MAX_ENV];
    size_t n = 0;

    dchk("armor: the vector itself is a KEF envelope",
         kef_sniff(ENV, sizeof ENV) == 1);

    n = 0;
    dchk("armor: base43 out of a QR decodes to the envelope",
         kef_unarmor((const uint8_t *)B43, strlen(B43), out, sizeof out, &n) == 0
             && n == sizeof ENV && memcmp(out, ENV, sizeof ENV) == 0);

    n = 0;
    dchk("armor: base64 out of a file decodes to the envelope",
         kef_unarmor((const uint8_t *)B64, strlen(B64), out, sizeof out, &n) == 0
             && n == sizeof ENV && memcmp(out, ENV, sizeof ENV) == 0);

    // A file an editor has touched. The QR never carries one, the card does.
    char nl[128];
    snprintf(nl, sizeof nl, "%s\n", B64);
    n = 0;
    dchk("armor: a trailing newline does not stop it",
         kef_unarmor((const uint8_t *)nl, strlen(nl), out, sizeof out, &n) == 0
             && n == sizeof ENV);

    // -1 for a raw envelope is the contract, not a miss: the caller keeps
    // using its own buffer, which is the path every KISS backup takes.
    n = 0;
    dchk("armor: a raw envelope is left alone",
         kef_unarmor(ENV, sizeof ENV, out, sizeof out, &n) == -1);

    // Everything that must fall through to whoever else wants the payload.
    const char *words =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    n = 0;
    dchk("armor: a text mnemonic is not armored KEF",
         kef_unarmor((const uint8_t *)words, strlen(words), out, sizeof out,
                     &n) == -1);

    const char *desc = "wpkh([73c5da0a/84h/0h/0h]xpub6C.../0/*)";
    n = 0;
    dchk("armor: a descriptor is not armored KEF",
         kef_unarmor((const uint8_t *)desc, strlen(desc), out, sizeof out,
                     &n) == -1);

    // Valid base43 that decodes cleanly and is not an envelope. This is the
    // check that keeps the password keyboard off a stray alphanumeric QR.
    const char *junk = "HELLO4WORLD4THIS4IS4NOT4AN4ENVELOPE";
    n = 0;
    dchk("armor: clean base43 that is not an envelope is refused",
         kef_unarmor((const uint8_t *)junk, strlen(junk), out, sizeof out,
                     &n) == -1);

    // One character outside both alphabets kills the whole string.
    char bad[80];
    snprintf(bad, sizeof bad, "%s", B43);
    bad[10] = '\'';
    n = 0;
    dchk("armor: an out of alphabet character is refused",
         kef_unarmor((const uint8_t *)bad, strlen(bad), out, sizeof out,
                     &n) == -1);

    // The output buffer is the bound, not the input length.
    uint8_t tiny[8];
    n = 0;
    dchk("armor: a decode too big for the caller's buffer is refused",
         kef_unarmor((const uint8_t *)B43, strlen(B43), tiny, sizeof tiny,
                     &n) == -1);
}

int test_kef(void)
{
    dfails = 0;
    test_gcm_vectors();
    test_goldens();
    test_krux_versions();
    test_iterations();
    test_roundtrip();
    test_sniff();
    test_seal_seed();
    test_armor();
    return dfails;
}
