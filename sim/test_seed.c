// Desktop tests for main/kiss_seed.c (step 7: user-owned seed).
// Runs FIRST in kisstest: it ends with the dev mnemonic stored, which the
// rest of the suite (addresses, PSBTs) relies on once kiss_session_open
// reads the stored seed instead of a compiled-in constant.
#include <stdio.h>
#include <string.h>

#include "kiss_backup.h"
#include "kiss_crypto.h"
#include "kiss_duress.h"
#include "kiss_cards_q.h"
#include "kiss_seed.h"

#define DEV_WORDS "abandon abandon abandon abandon abandon abandon " \
                  "abandon abandon abandon abandon abandon about"
// another well-known BIP39 test-vector mnemonic (entropy 0x80..0 pattern)
#define ALT_WORDS "legal winner thank year wave sausage worth useful legal " \
                  "winner thank yellow"

static int sfails;

// mnemonic -> numeric SeedQR digits. The parser that read these is gone; this
// builds a WELL-FORMED one so the tests can prove it is refused rather than
// only proving that junk is. Returns the digit count, or 0 if a word is not on
// the list.
static size_t seed_digits(const char *mnemonic, char *out, size_t out_len)
{
    size_t d = 0;
    const char *p = mnemonic;
    while (*p && d + 4 < out_len) {
        char w[12]; size_t n = 0;
        while (*p && *p != ' ' && n + 1 < sizeof w) w[n++] = *p++;
        w[n] = 0;
        if (*p == ' ') p++;
        int idx = -1;
        for (int i = 0; i < 2048; i++) {
            const char *c = NULL;
            if (kiss_seed_word(i, &c) == 0 && strcmp(c, w) == 0) { idx = i; break; }
        }
        if (idx < 0) return 0;
        d += (size_t)snprintf(out + d, out_len - d, "%04d", idx);
    }
    return d;
}

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
    kiss_set_network(0);          // the dev vector is a mainnet address
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

        // The famous vectors are all refused now, so every ACCEPT test needs a
        // mnemonic with real entropy behind it. Built here rather than pasted
        // so it cannot quietly become another published one.
        char good12[WSEED_MAX_MNEMONIC], good24[WSEED_MAX_MNEMONIC];
        for (size_t i = 0; i < sizeof ent; i++) ent[i] = (uint8_t)(i * 37 + 11);
        kiss_seed_from_entropy(ent, 16, good12, sizeof good12);
        kiss_seed_from_entropy(ent, 32, good24, sizeof good24);

        // 1. a text mnemonic, which Krux can seal instead of the entropy
        schk("plaintext: plain mnemonic rc",
             kiss_seed_from_plaintext(good12, strlen(good12), got, sizeof got) == 0);
        schk("plaintext: plain mnemonic roundtrips", strcmp(got, good12) == 0);
        {
            char padded[WSEED_MAX_MNEMONIC + 8];
            snprintf(padded, sizeof padded, "  %s\n", good12);
            schk("plaintext: plain mnemonic with stray spaces",
                 kiss_seed_from_plaintext(padded, strlen(padded), got, sizeof got) == 0
                 && strcmp(got, good12) == 0);
        }
        schk("plaintext: plain mnemonic with a bad checksum refused",
             kiss_seed_from_plaintext(
                 "abandon abandon abandon abandon abandon abandon "
                 "abandon abandon abandon abandon abandon abandon", 71,
                 got, sizeof got) != 0);
        // Valid BIP39 and still nothing: the canonical zero-entropy vector is
        // printed on every BIP39 explainer there is, so a backup of it is a
        // wallet the whole world can spend from. The checksum has no opinion.
        schk("plaintext: the abandon vector refused as text",
             kiss_seed_from_plaintext(DEV_WORDS, strlen(DEV_WORDS), got, sizeof got) != 0);
        schk("plaintext: the abandon vector leaves nothing behind", got[0] == 0);
        schk("plaintext: the 0x80 vector refused as text",
             kiss_seed_from_plaintext(ALT_WORDS, strlen(ALT_WORDS), got, sizeof got) != 0);

        // 2. numeric SeedQR: REMOVED, and pinned removed.
        // 48 or 96 ASCII digits, four per wordlist index, was a shape this
        // signer read straight off a printed square. It no longer does: a bare
        // seed square is a seed to whoever photographs it, this device never
        // wrote one, and the only thing a camera can hand it now is an
        // envelope a password still has to open.
        //
        // A WELL-FORMED one is the test that matters. Junk was always refused,
        // so junk proves nothing about the removal; digits built from a real
        // mnemonic used to restore it, and must not any more.
        {
            char d12[49], d24[97];
            schk("plaintext: built 48 digits for 12 words",
                 seed_digits(good12, d12, sizeof d12) == 48);
            schk("plaintext: built 96 digits for 24 words",
                 seed_digits(good24, d24, sizeof d24) == 96);
            schk("plaintext: a valid 48 digit SeedQR is refused",
                 kiss_seed_from_plaintext(d12, 48, got, sizeof got) != 0);
            schk("plaintext: a valid 96 digit SeedQR is refused",
                 kiss_seed_from_plaintext(d24, 96, got, sizeof got) != 0);
            schk("plaintext: a refused SeedQR leaves nothing behind",
                 got[0] == 0);
        }
        // abandon = 0000 (x11), about = 0003: the vector every BIP39 page
        // prints, refused twice over now.
        schk("plaintext: the abandon vector as digits is refused",
             kiss_seed_from_plaintext(
                 "000000000000000000000000000000000000000000000003", 48,
                 got, sizeof got) != 0);

        // 3. raw entropy bytes, 16 or 32: what Krux seals, and what this
        // signer seals
        // Ordinary entropy still goes straight through, and must agree with the
        // entropy path byte for byte.
        for (size_t i = 0; i < sizeof ent; i++) ent[i] = (uint8_t)(i * 37 + 11);
        kiss_seed_from_entropy(ent, 16, words, sizeof words);
        schk("plaintext: 16 bytes rc",
             kiss_seed_from_plaintext((const char *)ent, 16, got, sizeof got) == 0);
        schk("plaintext: 16 matches entropy path", strcmp(got, words) == 0);
        kiss_seed_from_entropy(ent, 32, words, sizeof words);
        schk("plaintext: 32 rc",
             kiss_seed_from_plaintext((const char *)ent, 32, got, sizeof got) == 0);
        schk("plaintext: 32 matches entropy path", strcmp(got, words) == 0);
        schk("plaintext: 20 raw bytes refused",
             kiss_seed_from_plaintext((const char *)ent, 20, got, sizeof got) != 0);

        // Degenerate entropy is refused on the way out of the envelope too.
        // kiss_seed_from_entropy is NOT the gate -- it happily turns 32 zero
        // bytes into the dev mnemonic -- so the check lives here and these
        // prove it.
        memset(ent, 0x00, sizeof ent);
        schk("plaintext: all-zero 16 refused",
             kiss_seed_from_plaintext((const char *)ent, 16, got, sizeof got) != 0);
        schk("plaintext: all-zero 32 refused",
             kiss_seed_from_plaintext((const char *)ent, 32, got, sizeof got) != 0);
        memset(ent, 0xFF, sizeof ent);
        schk("plaintext: all-ones 32 refused",
             kiss_seed_from_plaintext((const char *)ent, 32, got, sizeof got) != 0);
        memset(ent, 0xA5, sizeof ent);   // one repeated byte, half the bits set
        schk("plaintext: one repeated byte refused",
             kiss_seed_from_plaintext((const char *)ent, 32, got, sizeof got) != 0);
        // ... and a single bit set in 32 bytes: not all one value, still nothing
        memset(ent, 0x00, sizeof ent); ent[7] = 0x08;
        schk("plaintext: near-empty entropy refused",
             kiss_seed_from_plaintext((const char *)ent, 32, got, sizeof got) != 0);

        schk("plaintext: empty refused", kiss_seed_from_plaintext("", 0, got, sizeof got) != 0);
        schk("plaintext: garbage refused",
             kiss_seed_from_plaintext("hello world", 11, got, sizeof got) != 0);
        // a NUL-terminated buffer must not leak the old value on failure
        got[0] = 'x';
        kiss_seed_from_plaintext("hello world", 11, got, sizeof got);
        schk("plaintext: output cleared on failure", got[0] == 0);

        // ---- the gate itself, straight ----
        // Two arms: degenerate BYTES, and a word sequence the blind draw's
        // judge blocks. The abandon vector is the one that needs both -- its
        // entropy is all-zero, and its INDICES are ten zeros and a three, which
        // is what the word arm sees once the checksum word is excluded.
        schk("degen: the abandon vector", kiss_seed_degenerate(DEV_WORDS) == 1);
        schk("degen: the 0x80 vector", kiss_seed_degenerate(ALT_WORDS) == 1);
        schk("degen: real entropy passes", kiss_seed_degenerate(good12) == 0);
        schk("degen: real 24 word entropy passes", kiss_seed_degenerate(good24) == 0);
        memset(ent, 0xAA, sizeof ent);
        kiss_seed_from_entropy(ent, 16, words, sizeof words);
        schk("degen: one repeated byte", kiss_seed_degenerate(words) == 1);
        schk("degen: empty is not this function's question",
             kiss_seed_degenerate("") == 0 && kiss_seed_degenerate(NULL) == 0);
        schk("degen: not a mnemonic is not this function's question",
             kiss_seed_degenerate("hello world") == 0);

        // The two facts ABOUT a seed die with it. They are not settings and
        // are deliberately absent from KEEP_KEYS, so a wipe takes them -- and
        // the host has to reach the same state the device's partition erase
        // reaches for free, or the simulator answers "how were these keys
        // made?" about a wallet that was erased.
        kiss_seed_set_entropy_note(WSEED_ENTQ_CARDS | WC_Q_DUP);
        kiss_seed_set_source(WSEED_SRC_DICE);
        schk("wipe: the note and the source are readable first",
             kiss_seed_entropy_note() != 0 && kiss_seed_source() == WSEED_SRC_DICE);
        schk("wipe: rc", kiss_seed_wipe() == WSEED_OK);
        schk("wipe: the entropy note went with the seed",
             kiss_seed_entropy_note() == 0);
        schk("wipe: the source went with the seed",
             kiss_seed_source() == WSEED_SRC_NONE);
        schk("wipe: and the seed really is gone",
             kiss_seed_load(words, sizeof words) != 0);

        // The gate refuses to TAKE a seed. It must never refuse to OPEN one:
        // the storage read back paths run kiss_seed_validate, and a device that
        // already holds the abandon vector has to keep unlocking. The rest of
        // this suite depends on exactly that -- it stores DEV_WORDS below and
        // derives addresses from it -- so this states it rather than leaving it
        // as a side effect nobody would notice breaking.
        schk("degen: a degenerate seed still validates",
             kiss_seed_validate(DEV_WORDS) == 0);
        schk("degen: and still stores and loads", kiss_seed_store(DEV_WORDS) == 0);
        {
            char back[WSEED_MAX_MNEMONIC];
            schk("degen: load returns it unchanged",
                 kiss_seed_load(back, sizeof back) == 0 &&
                 strcmp(back, DEV_WORDS) == 0);
        }
    }

    // ---- storage mode: amnesic never touches persistent storage ----
    {
        char got[WSEED_MAX_MNEMONIC];
        char exact[sizeof ALT_WORDS];
        char short_out[sizeof ALT_WORDS - 1];

        schk("mode defaults to KEEP", kiss_seed_mode() == WSEED_MODE_KEEP);
        schk("wipe before mode tests", kiss_seed_wipe() == 0);

        schk("set AMNESIC succeeds",
             kiss_seed_set_mode(WSEED_MODE_AMNESIC) == 0);
        schk("mode reads back AMNESIC", kiss_seed_mode() == WSEED_MODE_AMNESIC);
        schk("amnesic: stage ok", kiss_seed_stage(ALT_WORDS) == 0);
        memset(short_out, 0xA5, sizeof short_out);
        schk("pending: too-small load refused and cleared",
             kiss_seed_load(short_out, sizeof short_out) == WSEED_ERR_INVALID &&
             short_out[0] == '\0');
        schk("pending: exact-size load succeeds",
             kiss_seed_load(exact, sizeof exact) == WSEED_OK &&
             strcmp(exact, ALT_WORDS) == 0);
        schk("amnesic: commit ok", kiss_seed_commit() == 0);
        // the session must work for as long as the device stays unlocked...
        schk("amnesic: seed visible while unlocked", kiss_seed_exists() == 1);
        memset(short_out, 0xA5, sizeof short_out);
        schk("amnesic: too-small load refused and cleared",
             kiss_seed_load(short_out, sizeof short_out) == WSEED_ERR_INVALID &&
             short_out[0] == '\0');
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
