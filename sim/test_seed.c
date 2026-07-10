// Desktop tests for main/wallet_seed.c (step 7: user-owned seed).
// Runs FIRST in kisstest: it ends with the dev mnemonic stored, which the
// rest of the suite (addresses, PSBTs) relies on once wallet_session_open
// reads the stored seed instead of a compiled-in constant.
#include <stdio.h>
#include <string.h>

#include "wallet_crypto.h"
#include "wallet_seed.h"

#define DEV_WORDS "abandon abandon abandon abandon abandon abandon " \
                  "abandon abandon abandon abandon abandon about"
// another well-known BIP39 test-vector mnemonic (entropy 0x80..0 pattern)
#define ALT_WORDS "legal winner thank year wave sausage worth useful legal " \
                  "winner thank yellow"

static int sfails;

static void schk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); sfails++; }
}

int test_seed_layer(void) {
    char words[WSEED_MAX_MNEMONIC];
    uint8_t ent[32];

    // ---- entropy -> words (BIP39 published vectors) ----
    memset(ent, 0x00, sizeof ent);
    schk("entropy16(0x00) -> words rc",
         wallet_seed_from_entropy(ent, 16, words, sizeof words) == 0);
    schk("entropy16(0x00) -> abandon..about", strcmp(words, DEV_WORDS) == 0);
    schk("12-word result validates", wallet_seed_validate(words) == 0);

    memset(ent, 0xFF, sizeof ent);
    schk("entropy32(0xFF) -> words rc",
         wallet_seed_from_entropy(ent, 32, words, sizeof words) == 0);
    {
        int spaces = 0;
        for (const char *p = words; *p; p++) if (*p == ' ') spaces++;
        schk("entropy32 gives 24 words", spaces == 23);
    }
    schk("24-word result validates", wallet_seed_validate(words) == 0);
    schk("entropy of 20 bytes refused",
         wallet_seed_from_entropy(ent, 20, words, sizeof words) != 0);

    // ---- validation catches damage ----
    schk("valid dev words validate", wallet_seed_validate(DEV_WORDS) == 0);
    schk("checksum damage caught", wallet_seed_validate(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon") != 0);
    schk("non-wordlist word caught", wallet_seed_validate(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon fruitninja") != 0);
    schk("empty refused", wallet_seed_validate("") != 0);

    // ---- store / load / wipe ----
    schk("wipe (clean slate) ok", wallet_seed_wipe() == 0);
    schk("no seed after wipe", wallet_seed_exists() == 0);
    schk("load with none stored refuses", wallet_seed_load(words, sizeof words) != 0);
    schk("store rejects bad checksum", wallet_seed_store(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon") != 0);
    schk("still no seed", wallet_seed_exists() == 0);
    schk("store alt words ok", wallet_seed_store(ALT_WORDS) == 0);
    schk("seed exists now", wallet_seed_exists() == 1);
    schk("load rc", wallet_seed_load(words, sizeof words) == 0);
    schk("load roundtrips", strcmp(words, ALT_WORDS) == 0);

    // ---- the session must follow the STORED seed ----
    uint8_t fp[4] = {0};
    schk("fingerprint(alt seed) rc", wallet_fingerprint("", fp) == 0);
    int alt_is_dev = (fp[0] == 0x73 && fp[1] == 0xC5 && fp[2] == 0xDA && fp[3] == 0x0A);
    schk("alt seed is NOT the dev fingerprint", !alt_is_dev);

    schk("store dev words ok", wallet_seed_store(DEV_WORDS) == 0);
    schk("fingerprint(dev seed) rc", wallet_fingerprint("", fp) == 0);
    schk("dev fingerprint 73C5DA0A",
         fp[0] == 0x73 && fp[1] == 0xC5 && fp[2] == 0xDA && fp[3] == 0x0A);

    schk("session opens from stored seed", wallet_session_open("") == 0);
    char addr[92];
    schk("address from stored seed rc", wallet_session_address(0, 0, addr, sizeof addr) == 0);
    schk("address matches dev vector",
         strcmp(addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu") == 0);
    wallet_session_close();

    // wiped device refuses to open a session at all
    schk("wipe ok", wallet_seed_wipe() == 0);
    schk("open refused with no seed", wallet_session_open("") != 0);
    schk("fingerprint refused with no seed", wallet_fingerprint("", fp) != 0);

    // ---- wordlist access for the wizard ----
    {
        const char *w = NULL;
        schk("word(0) rc", wallet_seed_word(0, &w) == 0);
        schk("word(0) = abandon", w && strcmp(w, "abandon") == 0);
        schk("word(2047) rc", wallet_seed_word(2047, &w) == 0);
        schk("word(2047) = zoo", w && strcmp(w, "zoo") == 0);
        schk("word(2048) refused", wallet_seed_word(2048, &w) != 0);
        const char *sug[3];
        int n = wallet_seed_suggest("aban", sug, 3);
        schk("suggest(aban) finds abandon", n >= 1 && strcmp(sug[0], "abandon") == 0);
        n = wallet_seed_suggest("zo", sug, 3);
        schk("suggest(zo) finds zone first", n >= 2 && strcmp(sug[0], "zone") == 0);
        schk("suggest(zo) keeps early match intact", strcmp(sug[1], "zoo") == 0);
        schk("suggest(qqq) empty", wallet_seed_suggest("qqq", sug, 3) == 0);
    }

    // leave the dev seed stored: the rest of the suite depends on it
    schk("restore dev words for suite", wallet_seed_store(DEV_WORDS) == 0);
    return sfails;
}
