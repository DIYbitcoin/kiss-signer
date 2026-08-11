// Desktop tests for main/kiss_seed.c (step 7: user-owned seed).
// Runs FIRST in kisstest: it ends with the dev mnemonic stored, which the
// rest of the suite (addresses, PSBTs) relies on once kiss_session_open
// reads the stored seed instead of a compiled-in constant.
#include <stdio.h>
#include <string.h>

#include "kiss_backup.h"
#include "kiss_crypto.h"
#include "kiss_duress.h"
#include "kiss_seed.h"

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
         kiss_seed_from_entropy(ent, 16, words, sizeof words) == 0);
    schk("entropy16(0x00) -> abandon..about", strcmp(words, DEV_WORDS) == 0);
    schk("12-word result validates", kiss_seed_validate(words) == 0);

    memset(ent, 0xFF, sizeof ent);
    schk("entropy32(0xFF) -> words rc",
         kiss_seed_from_entropy(ent, 32, words, sizeof words) == 0);
    {
        int spaces = 0;
        for (const char *p = words; *p; p++) if (*p == ' ') spaces++;
        schk("entropy32 gives 24 words", spaces == 23);
    }
    schk("24-word result validates", kiss_seed_validate(words) == 0);
    schk("entropy of 20 bytes refused",
         kiss_seed_from_entropy(ent, 20, words, sizeof words) != 0);

    // ---- validation catches damage ----
    schk("valid dev words validate", kiss_seed_validate(DEV_WORDS) == 0);
    schk("checksum damage caught", kiss_seed_validate(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon") != 0);
    schk("non-wordlist word caught", kiss_seed_validate(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon fruitninja") != 0);
    schk("empty refused", kiss_seed_validate("") != 0);

    // ---- store / load / wipe ----
    schk("wipe (clean slate) ok", kiss_seed_wipe() == 0);
    schk("no seed after wipe", kiss_seed_exists() == 0);
    schk("load with none stored refuses", kiss_seed_load(words, sizeof words) != 0);
    schk("store rejects bad checksum", kiss_seed_store(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon") != 0);
    schk("still no seed", kiss_seed_exists() == 0);
    schk("store alt words ok", kiss_seed_store(ALT_WORDS) == 0);
    schk("seed exists now", kiss_seed_exists() == 1);
    schk("load rc", kiss_seed_load(words, sizeof words) == 0);
    schk("load roundtrips", strcmp(words, ALT_WORDS) == 0);

    // ---- the session must follow the STORED seed ----
    uint8_t fp[4] = {0};
    schk("fingerprint(alt seed) rc", kiss_fingerprint("", fp) == 0);
    int alt_is_dev = (fp[0] == 0x73 && fp[1] == 0xC5 && fp[2] == 0xDA && fp[3] == 0x0A);
    schk("alt seed is NOT the dev fingerprint", !alt_is_dev);

    schk("store dev words ok", kiss_seed_store(DEV_WORDS) == 0);
    schk("fingerprint(dev seed) rc", kiss_fingerprint("", fp) == 0);
    schk("dev fingerprint 73C5DA0A",
         fp[0] == 0x73 && fp[1] == 0xC5 && fp[2] == 0xDA && fp[3] == 0x0A);

    schk("session opens from stored seed", kiss_session_open("") == 0);
    char addr[92];
    schk("address from stored seed rc", kiss_session_address(0, 0, addr, sizeof addr) == 0);
    schk("address matches dev vector",
         strcmp(addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu") == 0);
    kiss_session_close();

    // wiped device refuses to open a session at all
    schk("wipe ok", kiss_seed_wipe() == 0);
    schk("open refused with no seed", kiss_session_open("") != 0);
    schk("fingerprint refused with no seed", kiss_fingerprint("", fp) != 0);

    // ---- wordlist access for the wizard ----
    {
        const char *w = NULL;
        schk("word(0) rc", kiss_seed_word(0, &w) == 0);
        schk("word(0) = abandon", w && strcmp(w, "abandon") == 0);
        schk("word(2047) rc", kiss_seed_word(2047, &w) == 0);
        schk("word(2047) = zoo", w && strcmp(w, "zoo") == 0);
        schk("word(2048) refused", kiss_seed_word(2048, &w) != 0);
        const char *sug[3];
        int n = kiss_seed_suggest("aban", sug, 3);
        schk("suggest(aban) finds abandon", n >= 1 && strcmp(sug[0], "abandon") == 0);
        n = kiss_seed_suggest("zo", sug, 3);
        schk("suggest(zo) finds zone first", n >= 2 && strcmp(sug[0], "zone") == 0);
        schk("suggest(zo) keeps early match intact", strcmp(sug[1], "zoo") == 0);
        schk("suggest(qqq) empty", kiss_seed_suggest("qqq", sug, 3) == 0);
    }

    // backup verification: word-by-word compare, first-mismatch index
    schk("diff: identical -> -1", kiss_seed_diff_word(DEV_WORDS, DEV_WORDS) == -1);
    schk("diff: leading/trailing spaces ignored",
         kiss_seed_diff_word("  " DEV_WORDS " ", DEV_WORDS) == -1);
    schk("diff: last word wrong -> 11",
         kiss_seed_diff_word(
             "abandon abandon abandon abandon abandon abandon "
             "abandon abandon abandon abandon abandon abandon", DEV_WORDS) == 11);
    schk("diff: first word wrong -> 0",
         kiss_seed_diff_word("ability winner thank year wave sausage worth "
                               "useful legal winner thank yellow", ALT_WORDS) == 0);
    schk("diff: word #3 wrong -> 2",
         kiss_seed_diff_word("legal winner THANK year wave sausage worth "
                               "useful legal winner thank yellow", ALT_WORDS) == 2);
    schk("diff: prefix-of-word is a mismatch (aban vs abandon)",
         kiss_seed_diff_word("aban", "abandon") == 0);
    schk("diff: fewer words -> mismatch at the short end",
         kiss_seed_diff_word("legal winner", ALT_WORDS) == 2);
    schk("diff: extra words -> mismatch at the extra one",
         kiss_seed_diff_word(ALT_WORDS " extra", ALT_WORDS) == 12);

    // ---- QR seed import (amnesic mode: load the seed, sign, power off) ----
    // KISS never EXPORTS a seed as a QR. It reads one a user already made
    // elsewhere (SeedSigner / Krux), which is the whole point of amnesic mode.
    {
        char got[WSEED_MAX_MNEMONIC];

        // 1. plain text mnemonic in a QR
        schk("qr: plain mnemonic rc",
             kiss_seed_from_qr(DEV_WORDS, strlen(DEV_WORDS), got, sizeof got) == 0);
        schk("qr: plain mnemonic roundtrips", strcmp(got, DEV_WORDS) == 0);
        schk("qr: plain mnemonic with stray spaces",
             kiss_seed_from_qr("  " DEV_WORDS "\n", strlen(DEV_WORDS) + 3,
                                 got, sizeof got) == 0 && strcmp(got, DEV_WORDS) == 0);
        schk("qr: plain mnemonic with a bad checksum refused",
             kiss_seed_from_qr("abandon abandon abandon abandon abandon abandon "
                                 "abandon abandon abandon abandon abandon abandon", 71,
                                 got, sizeof got) != 0);

        // 2. numeric SeedQR (SeedSigner): 4 digits per wordlist index.
        // abandon = 0000 (x11), about = 0003.
        static const char *SQR12 =
            "000000000000000000000000000000000000000000000003";
        schk("qr: numeric SeedQR 48 digits rc",
             kiss_seed_from_qr(SQR12, 48, got, sizeof got) == 0);
        schk("qr: numeric SeedQR = dev words", strcmp(got, DEV_WORDS) == 0);

        // 24-word numeric: build the digits from a known mnemonic by searching
        // the wordlist (word -> index), the opposite direction to the parser.
        memset(ent, 0xFF, sizeof ent);
        kiss_seed_from_entropy(ent, 32, words, sizeof words);
        {
            char digits[97];
            size_t d = 0;
            const char *p = words;
            while (*p && d + 4 < sizeof digits) {
                char w[12]; size_t n = 0;
                while (*p && *p != ' ' && n + 1 < sizeof w) w[n++] = *p++;
                w[n] = 0;
                if (*p == ' ') p++;
                int idx = -1;
                for (int i = 0; i < 2048; i++) {
                    const char *c = NULL;
                    if (kiss_seed_word(i, &c) == 0 && strcmp(c, w) == 0) { idx = i; break; }
                }
                d += (size_t)snprintf(digits + d, sizeof digits - d, "%04d", idx);
            }
            schk("qr: built 96 digits for 24 words", d == 96);
            schk("qr: numeric SeedQR 96 digits rc",
                 kiss_seed_from_qr(digits, 96, got, sizeof got) == 0);
            schk("qr: numeric SeedQR 96 roundtrips", strcmp(got, words) == 0);

            digits[3] = '9';        // index 0009 in slot 0: checksum must fail
            schk("qr: numeric SeedQR with a broken checksum refused",
                 kiss_seed_from_qr(digits, 96, got, sizeof got) != 0);
        }
        schk("qr: 47 digits refused",
             kiss_seed_from_qr(SQR12, 47, got, sizeof got) != 0);
        schk("qr: index 2048 out of range refused",
             kiss_seed_from_qr("204800000000000000000000"
                                 "000000000000000000000000", 48, got, sizeof got) != 0);

        // 3. CompactSeedQR: raw entropy bytes, 16 or 32
        // Ordinary entropy still goes straight through, and must agree with the
        // entropy path byte for byte.
        for (size_t i = 0; i < sizeof ent; i++) ent[i] = (uint8_t)(i * 37 + 11);
        kiss_seed_from_entropy(ent, 16, words, sizeof words);
        schk("qr: compact 16 bytes rc",
             kiss_seed_from_qr((const char *)ent, 16, got, sizeof got) == 0);
        schk("qr: compact 16 matches entropy path", strcmp(got, words) == 0);
        kiss_seed_from_entropy(ent, 32, words, sizeof words);
        schk("qr: compact 32 rc",
             kiss_seed_from_qr((const char *)ent, 32, got, sizeof got) == 0);
        schk("qr: compact 32 matches entropy path", strcmp(got, words) == 0);
        schk("qr: 20 raw bytes refused",
             kiss_seed_from_qr((const char *)ent, 20, got, sizeof got) != 0);

        // Degenerate entropy is refused. These are the shapes a blank, a solid
        // or a hand-drawn CompactSeedQR produces, and every one of them used to
        // build a real wallet without a word said. kiss_seed_from_entropy is
        // NOT the gate -- it happily turns 32 zero bytes into the dev mnemonic
        // -- so the check lives in kiss_seed_from_qr and these prove it.
        memset(ent, 0x00, sizeof ent);
        schk("qr: compact all-zero 16 refused",
             kiss_seed_from_qr((const char *)ent, 16, got, sizeof got) != 0);
        schk("qr: compact all-zero 32 refused",
             kiss_seed_from_qr((const char *)ent, 32, got, sizeof got) != 0);
        memset(ent, 0xFF, sizeof ent);
        schk("qr: compact all-ones 32 refused",
             kiss_seed_from_qr((const char *)ent, 32, got, sizeof got) != 0);
        memset(ent, 0xA5, sizeof ent);   // one repeated byte, half the bits set
        schk("qr: compact one repeated byte refused",
             kiss_seed_from_qr((const char *)ent, 32, got, sizeof got) != 0);
        // ... and a single bit set in 32 bytes: not all one value, still nothing
        memset(ent, 0x00, sizeof ent); ent[7] = 0x08;
        schk("qr: compact near-empty entropy refused",
             kiss_seed_from_qr((const char *)ent, 32, got, sizeof got) != 0);

        schk("qr: empty refused", kiss_seed_from_qr("", 0, got, sizeof got) != 0);
        schk("qr: garbage refused",
             kiss_seed_from_qr("hello world", 11, got, sizeof got) != 0);
        // a NUL-terminated buffer must not leak the old value on failure
        got[0] = 'x';
        kiss_seed_from_qr("hello world", 11, got, sizeof got);
        schk("qr: output cleared on failure", got[0] == 0);
    }

    // ---- storage mode: amnesic never touches persistent storage ----
    {
        char got[WSEED_MAX_MNEMONIC];

        schk("mode defaults to KEEP", kiss_seed_mode() == WSEED_MODE_KEEP);
        schk("wipe before mode tests", kiss_seed_wipe() == 0);

        schk("set AMNESIC succeeds",
             kiss_seed_set_mode(WSEED_MODE_AMNESIC) == 0);
        schk("mode reads back AMNESIC", kiss_seed_mode() == WSEED_MODE_AMNESIC);
        schk("amnesic: stage ok", kiss_seed_stage(ALT_WORDS) == 0);
        schk("amnesic: commit ok", kiss_seed_commit() == 0);
        // the session must work for as long as the device stays unlocked...
        schk("amnesic: seed visible while unlocked", kiss_seed_exists() == 1);
        schk("amnesic: load rc", kiss_seed_load(got, sizeof got) == 0);
        schk("amnesic: load roundtrips", strcmp(got, ALT_WORDS) == 0);
        schk("amnesic: session opens", kiss_session_open("") == 0);
        kiss_session_close();
        // ...and kiss_session_close is the lock: RAM is the ONLY copy, so
        // the seed has to be gone with it
        schk("amnesic: seed gone after lock", kiss_seed_exists() == 0);
        schk("amnesic: load refused after lock",
             kiss_seed_load(got, sizeof got) != 0);
        // nothing must have reached persistent storage at any point
        schk("set KEEP succeeds", kiss_seed_set_mode(WSEED_MODE_KEEP) == 0);
        schk("amnesic: nothing was persisted", kiss_seed_exists() == 0);

        // KEEP still survives a lock, which is the whole difference
        schk("keep: stage ok", kiss_seed_stage(ALT_WORDS) == 0);
        schk("keep: commit ok", kiss_seed_commit() == 0);
        schk("keep: session opens", kiss_session_open("") == 0);
        kiss_session_close();
        schk("keep: seed survives lock", kiss_seed_exists() == 1);
        schk("keep: load roundtrips",
             kiss_seed_load(got, sizeof got) == 0 && strcmp(got, ALT_WORDS) == 0);

        // switching to amnesic must not leave the old seed behind
        schk("switch to AMNESIC succeeds",
             kiss_seed_set_mode(WSEED_MODE_AMNESIC) == 0);
        schk("switching to amnesic wipes stored seed", kiss_seed_exists() == 0);
        schk("switch back to KEEP succeeds",
             kiss_seed_set_mode(WSEED_MODE_KEEP) == 0);
    }

    // ---- setup wizard: nothing reaches flash before commit ----
    // The wizard's FIRST screen asks KEEP vs NOTHING SAVED, long before any
    // new words exist. Applying that choice on the tap erased the wallet the
    // user still had: one BACK tap, or a power cut, and it was gone with
    // nothing to replace it. The choice is staged like the mnemonic is, and
    // kiss_seed_commit is the single moment flash changes.
    {
        char got[WSEED_MAX_MNEMONIC];

        schk("wizard: set KEEP succeeds",
             kiss_seed_set_mode(WSEED_MODE_KEEP) == 0);
        schk("wizard: a wallet is stored to begin with",
             kiss_seed_store(DEV_WORDS) == 0);

        kiss_seed_stage_mode(WSEED_MODE_AMNESIC);
        schk("wizard: staged mode reads back AMNESIC",
             kiss_seed_mode() == WSEED_MODE_AMNESIC);
        schk("wizard: picking NOTHING SAVED does not erase yet",
             kiss_seed_load(got, sizeof got) == 0 &&
             strcmp(got, DEV_WORDS) == 0);

        kiss_seed_discard();            // BACK, cancel, or a lost session
        schk("wizard: backing out restores the stored mode",
             kiss_seed_mode() == WSEED_MODE_KEEP);
        schk("wizard: backing out leaves the old wallet intact",
             kiss_seed_load(got, sizeof got) == 0 &&
             strcmp(got, DEV_WORDS) == 0);

        // finishing the ritual is what actually applies it
        kiss_seed_stage_mode(WSEED_MODE_AMNESIC);
        schk("wizard: stage the new words", kiss_seed_stage(ALT_WORDS) == 0);
        schk("wizard: commit ok", kiss_seed_commit() == 0);
        schk("wizard: commit applied the amnesic mode",
             kiss_seed_mode() == WSEED_MODE_AMNESIC);
        kiss_seed_forget();             // the amnesic lock drops the RAM copy
        schk("wizard: commit took the old wallet with it",
             kiss_seed_exists() == 0);

        // the same guarantee in the other direction: a KEEP wizard that is
        // abandoned must not overwrite the wallet already on the device
        schk("wizard: restore KEEP mode succeeds",
             kiss_seed_set_mode(WSEED_MODE_KEEP) == 0);
        schk("wizard: restore a stored wallet", kiss_seed_store(DEV_WORDS) == 0);
        schk("wizard: stage a replacement", kiss_seed_stage(ALT_WORDS) == 0);
        kiss_seed_discard();
        schk("wizard: abandoned replacement leaves the old words",
             kiss_seed_load(got, sizeof got) == 0 &&
             strcmp(got, DEV_WORDS) == 0);

        // START A NEW WALLET, KEEP -> KEEP: the transition that now runs a
        // residue scrub after the write, because nvs_set_str only overwrites
        // the replaced mnemonic logically and it stayed readable on its page.
        //
        // The host has no log-structured store, so these assert the CONTRACT
        // the scrub must not break -- right words, mode still KEEP, wallet
        // still there. Whether the old bytes are physically gone is a device
        // fact and only a chip dump can answer it.
        schk("replace: stage a KEEP replacement", kiss_seed_stage(ALT_WORDS) == 0);
        schk("replace: commit ok", kiss_seed_commit() == 0);
        schk("replace: mode is still KEEP", kiss_seed_mode() == WSEED_MODE_KEEP);
        schk("replace: a wallet still exists", kiss_seed_exists() == 1);
        schk("replace: the new words are what loads",
             kiss_seed_load(got, sizeof got) == 0 &&
             strcmp(got, ALT_WORDS) == 0);
        schk("replace: nothing is left staged", kiss_seed_commit() != 0);

        // and it survives being done twice in a row, which is the shape of an
        // owner who changes their mind at the wizard
        schk("replace: stage a second replacement", kiss_seed_stage(DEV_WORDS) == 0);
        schk("replace: second commit ok", kiss_seed_commit() == 0);
        schk("replace: second replacement loads",
             kiss_seed_load(got, sizeof got) == 0 &&
             strcmp(got, DEV_WORDS) == 0);

        // the paper check was a claim about the wallet that just went away
        {
            uint8_t fp[4] = { 0x11, 0x22, 0x33, 0x44 };
            kiss_backup_mark(fp);
            schk("replace: paper marked before the replacement",
                 kiss_backup_checked(fp));
            schk("replace: stage over a checked wallet",
                 kiss_seed_stage(ALT_WORDS) == 0);
            schk("replace: commit ok", kiss_seed_commit() == 0);
            schk("replace: the paper check did not survive",
                 !kiss_backup_checked(fp));
        }

        // and neither did the stroke that opened the wallet that went away.
        // On device the scrub's partition erase takes "greal" because it is
        // deliberately outside KEEP_KEYS; on host it is a static that nothing
        // erases, so the commit has to say so. Without that line a replacement
        // wallet still unlocks on its predecessor's decoy gesture HERE and not
        // on glass, which is the worst place for the two to disagree.
        {
            schk("replace: stroke set before the replacement",
                 kiss_duress_set(WDG_UNDERLINE) == 0 &&
                 kiss_duress_real() == WDG_UNDERLINE);
            schk("replace: stage over a wallet with a stroke",
                 kiss_seed_stage(DEV_WORDS) == 0);
            schk("replace: commit ok", kiss_seed_commit() == 0);
            schk("replace: the unlock stroke did not survive",
                 kiss_duress_real() == WDG_NONE);
        }

        // FIRST setup is not a replacement, and must not be treated as one.
        // storage_mode_read calls a factory-fresh store KEEP, so a scrub gated
        // on the mode alone would erase the partition on the way IN -- taking
        // the accent and language the owner had just picked, and opening a
        // power-cut window on a device whose only wallet is the one being
        // written. There has to be a wallet there already for there to be
        // residue.
        {
            uint8_t fp[4] = { 0x55, 0x66, 0x77, 0x88 };
            schk("first setup: wipe to factory", kiss_seed_wipe() == 0);
            schk("first setup: no wallet", kiss_seed_exists() == 0);
            kiss_backup_mark(fp);      // stands in for any pre-setup NVS state
            // and a stroke picked during setup, which is the same question
            // asked of the thing the replacement branch DOES scrub
            schk("first setup: set a stroke on the way in",
                 kiss_duress_set(WDG_UNDERLINE) == 0);
            schk("first setup: stage the first wallet",
                 kiss_seed_stage(DEV_WORDS) == 0);
            schk("first setup: commit ok", kiss_seed_commit() == 0);
            schk("first setup: the wallet is there",
                 kiss_seed_load(got, sizeof got) == 0 &&
                 strcmp(got, DEV_WORDS) == 0);
            schk("first setup: nothing was scrubbed on the way in",
                 kiss_backup_checked(fp));
            // had_prior_words is what keeps the scrub off the first commit, so
            // the stroke picked above has to still be there
            schk("first setup: the stroke was not scrubbed either",
                 kiss_duress_real() == WDG_UNDERLINE);
            kiss_duress_forget();
            kiss_backup_forget();
        }
    }

    // leave the dev seed stored: the rest of the suite depends on it
    schk("restore dev words for suite", kiss_seed_store(DEV_WORDS) == 0);
    return sfails;
}
