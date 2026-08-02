// Desktop tests for main/wallet_seed_sd.c: the sealed seed blob written to
// the SD card.
//
// The whole security claim of SD mode is "the card alone is useless", so
// these tests are mostly about rejection: a wrong device key, a flipped byte
// anywhere in the blob, a truncated file, a lying length field. Every one of
// them must fail closed with the output buffer zeroed, because the caller
// hands that buffer straight to the BIP39 layer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_sd.h"
#include "wallet_seed.h"
#include "wallet_seed_sd.h"

#define SD_WORDS "abandon abandon abandon abandon abandon abandon " \
                 "abandon abandon abandon abandon abandon about"
#define SD_WORDS24 "legal winner thank year wave sausage worth useful legal " \
                   "winner thank yellow legal winner thank year wave sausage " \
                   "worth useful legal winner thank yellow"
#define SD_WORDS_ALT "legal winner thank year wave sausage worth useful legal " \
                     "winner thank yellow"

static int dfails;

static void dchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); dfails++; }
}

static int all_zero(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static int seed_loads_as(const char *want) {
    char got[WSEED_MAX_MNEMONIC];
    int ok = wallet_seed_load(got, sizeof got) == WSEED_OK &&
             strcmp(got, want) == 0;
    memset(got, 0, sizeof got);
    return ok;
}

// The Sign screen's file list, at the layer that decides what it can show.
// The cap used to bite BEFORE the sort, so a full card showed an arbitrary
// FAT-order subset and the files that vanished left no trace -- on a real
// device test that read as "the card has two files" when it had nine. The
// contract now: the names kept are the FIRST `max` in display order (unsigned
// A-Z, then signed), and *total always says how many the card really holds.
static int test_sd_list(void) {
    system("rm -f /tmp/simsd/*.psbt");
    char names[4][SD_NAME_LEN];
    int total = 0;

    // seed 6 files, written in an order that is neither A-Z nor grouped, with
    // dot-junk that must never count. b- and d- are signed so the unsigned
    // three sort ahead of them despite the alphabet saying otherwise.
    static const char *fs[] = {
        "d-two-signed.psbt", "c-mid.psbt", "._c-mid.psbt",
        "a-first.psbt", "b-one-signed.psbt", "e-last.psbt",
    };
    for (size_t i = 0; i < sizeof fs / sizeof *fs; i++) {
        char p[96]; snprintf(p, sizeof p, "/tmp/simsd/%s", fs[i]);
        FILE *f = fopen(p, "wb");
        if (f) { fputs("x", f); fclose(f); }
    }

    int n = platform_sd_list_psbt(names, 4, &total);
    dchk("sd list: window filled", n == 4);
    dchk("sd list: total counts past the window", total == 5);
    dchk("sd list: unsigned lead in A-Z order",
         strcmp(names[0], "a-first.psbt") == 0 &&
         strcmp(names[1], "c-mid.psbt") == 0 &&
         strcmp(names[2], "e-last.psbt") == 0);
    dchk("sd list: first signed file takes the last slot",
         strcmp(names[3], "b-one-signed.psbt") == 0);

    // under the cap: everything shows and total agrees, so no hint fires
    remove("/tmp/simsd/d-two-signed.psbt");
    remove("/tmp/simsd/e-last.psbt");
    n = platform_sd_list_psbt(names, 4, &total);
    dchk("sd list: under cap shows all", n == 3 && total == 3);

    system("rm -f /tmp/simsd/*.psbt");
    return 0;
}

int test_sdseed_layer(void) {
    test_sd_list();
    uint8_t key[32], key2[32];
    uint8_t blob[SDSEED_MAX_BLOB], blob2[SDSEED_MAX_BLOB];
    size_t len = 0, len2 = 0;
    char got[WSEED_MAX_MNEMONIC];

    // ---- the device key ----
    dchk("device key reads", sd_seed_device_key(key) == 0);
    dchk("device key is not all zero", !all_zero(key, sizeof key));
    dchk("device key is stable across calls",
         sd_seed_device_key(key2) == 0 && memcmp(key, key2, 32) == 0);

    // ---- roundtrip ----
    dchk("seal 12 words", sd_seed_seal(key, SD_WORDS, blob, sizeof blob, &len) == 0);
    dchk("blob carries the magic", memcmp(blob, SDSEED_MAGIC, SDSEED_MAGIC_LEN) == 0);
    dchk("blob is header + ciphertext + tag",
         len > SDSEED_HDR_LEN + SDSEED_TAG_LEN && len <= SDSEED_MAX_BLOB);
    dchk("the mnemonic is not sitting in the blob in clear",
         memmem(blob, len, "abandon", 7) == NULL);
    dchk("open 12 words", sd_seed_open(key, blob, len, got, sizeof got) == 0);
    dchk("12 words round-trip exactly", strcmp(got, SD_WORDS) == 0);

    dchk("seal 24 words", sd_seed_seal(key, SD_WORDS24, blob2, sizeof blob2, &len2) == 0);
    dchk("open 24 words", sd_seed_open(key, blob2, len2, got, sizeof got) == 0);
    dchk("24 words round-trip exactly", strcmp(got, SD_WORDS24) == 0);

    // A fresh iv every write, so two seals of the same words are not the same
    // bytes on the card. Otherwise anyone holding two cards learns whether
    // they carry the same wallet without decrypting either.
    {
        uint8_t again[SDSEED_MAX_BLOB];
        size_t again_len = 0;
        dchk("seal the same words twice",
             sd_seed_seal(key, SD_WORDS, again, sizeof again, &again_len) == 0);
        dchk("two seals of one mnemonic differ (fresh iv)",
             again_len == len && memcmp(again, blob, len) != 0);
    }

    // ---- a wrong device key ----
    {
        uint8_t wrong[32];
        memcpy(wrong, key, 32);
        wrong[31] ^= 0x01;                 // one bit
        memset(got, 'x', sizeof got);
        dchk("wrong device key is refused",
             sd_seed_open(wrong, blob, len, got, sizeof got) != 0);
        dchk("wrong device key leaks nothing", all_zero(got, sizeof got));
    }

    // ---- every single-byte corruption ----
    {
        int bad = 0, leaked = 0;
        for (size_t i = 0; i < len; i++) {
            uint8_t save = blob[i];
            blob[i] ^= 0x40;
            memset(got, 'x', sizeof got);
            if (sd_seed_open(key, blob, len, got, sizeof got) == 0) bad++;
            else if (!all_zero(got, sizeof got)) leaked++;
            blob[i] = save;
        }
        dchk("every single-byte flip is rejected", bad == 0);
        dchk("no rejected blob leaves plaintext behind", leaked == 0);
    }

    // ---- truncation at every offset ----
    {
        int bad = 0;
        for (size_t n = 0; n < len; n++) {
            memset(got, 'x', sizeof got);
            if (sd_seed_open(key, blob, n, got, sizeof got) == 0) bad++;
        }
        dchk("every truncation is rejected", bad == 0);
    }

    // ---- a lying length field ----
    {
        uint8_t l[SDSEED_MAX_BLOB];
        memcpy(l, blob, len);
        l[24] = 0xff; l[25] = 0xff; l[26] = 0xff; l[27] = 0xff;
        memset(got, 'x', sizeof got);
        dchk("a huge declared length is refused",
             sd_seed_open(key, l, len, got, sizeof got) != 0);
        memcpy(l, blob, len);
        l[24] = l[25] = l[26] = l[27] = 0;      // zero-length ciphertext
        memset(got, 'x', sizeof got);
        dchk("a zero declared length is refused",
             sd_seed_open(key, l, len, got, sizeof got) != 0);
        memcpy(l, blob, len);
        l[24] = 7;                              // not a whole AES block
        memset(got, 'x', sizeof got);
        dchk("a non-block-multiple length is refused",
             sd_seed_open(key, l, len, got, sizeof got) != 0);
    }

    // A big, internally consistent file on the card. The header agrees with
    // its own size, so only the ciphertext bound stops this: nothing about a
    // 4KB "mnemonic" should ever reach the cipher.
    {
        static uint8_t big[4096];
        memcpy(big, blob, SDSEED_HDR_LEN);
        size_t ct = sizeof big - SDSEED_HDR_LEN - SDSEED_TAG_LEN;
        ct -= ct % 16;
        big[24] = (uint8_t)ct; big[25] = (uint8_t)(ct >> 8);
        big[26] = (uint8_t)(ct >> 16); big[27] = (uint8_t)(ct >> 24);
        memset(got, 'x', sizeof got);
        dchk("an oversized ciphertext is refused",
             sd_seed_open(key, big, SDSEED_HDR_LEN + ct + SDSEED_TAG_LEN,
                          got, sizeof got) != 0);
        dchk("an oversized blob leaks nothing", all_zero(got, sizeof got));
    }

    // ---- a foreign magic ----
    {
        uint8_t m[SDSEED_MAX_BLOB];
        memcpy(m, blob, len);
        m[7] = '2';                             // KISSSD02, a format we do not know
        memset(got, 'x', sizeof got);
        dchk("an unknown format version is refused",
             sd_seed_open(key, m, len, got, sizeof got) != 0);
    }

    // ---- caller buffer too small ----
    {
        char tiny[8];
        memset(tiny, 'x', sizeof tiny);
        dchk("a too-small output buffer is refused",
             sd_seed_open(key, blob, len, tiny, sizeof tiny) != 0);
        dchk("a refused open still zeroes the small buffer",
             all_zero(tiny, sizeof tiny));
    }

    // ---- seal refuses what it cannot fit ----
    {
        uint8_t small[32];
        size_t n = 12345;
        dchk("seal refuses a buffer it would overrun",
             sd_seed_seal(key, SD_WORDS, small, sizeof small, &n) != 0);
        dchk("a refused seal reports no length", n == 0);
    }

    // ---- integrated storage modes ---------------------------------------
    // All six directed transitions. The host SD seam is fault-injectable so
    // destination-first behavior is tested rather than inferred from comments.
    platform_sd_test_set_present(1);
    platform_sd_test_fail_next(0);
    wallet_seed_test_fail_next(0);
    dchk("storage: clean slate", wallet_seed_wipe() == WSEED_OK);
    dchk("storage: start in KEEP", wallet_seed_store(SD_WORDS) == WSEED_OK);

    uint8_t move_key[32], same_key[32];
    dchk("storage: pre-create device key", sd_seed_device_key(move_key) == 0);

    // Failure before destination commit: source/mode stay KEEP.
    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_WRITE);
    dchk("storage: injected SD write fails",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_ERR_SD_IO);
    dchk("storage: failed write leaves KEEP", wallet_seed_mode() == WSEED_MODE_KEEP);
    dchk("storage: failed write preserves words", seed_loads_as(SD_WORDS));

    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_RENAME);
    dchk("storage: injected atomic rename fails",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_ERR_SD_IO);
    dchk("storage: failed rename leaves KEEP", wallet_seed_mode() == WSEED_MODE_KEEP);
    dchk("storage: failed rename preserves words", seed_loads_as(SD_WORDS));

    platform_sd_test_set_present(0);
    dchk("storage: absent card is distinct",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_ERR_SD_MISSING);
    dchk("storage: absent card leaves KEEP words", seed_loads_as(SD_WORDS));
    platform_sd_test_set_present(1);

    // A metadata failure before publish rolls the verified card back. If mode
    // SD did publish but old-flash cleanup failed, CLEANUP means card is active.
    wallet_seed_test_fail_next(WSEED_TEST_FAIL_MODE_WRITE);
    dchk("storage: SD publish-mode failure is reported",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_ERR_SD_IO);
    dchk("storage: publish-mode failure leaves KEEP source",
         wallet_seed_mode() == WSEED_MODE_KEEP && seed_loads_as(SD_WORDS));
    dchk("storage: uncommitted card rolled back",
         platform_sd_read(SDSEED_FILENAME, blob2, sizeof blob2, &len2) != 0);

    wallet_seed_test_fail_next(WSEED_TEST_FAIL_SEED_REMOVE);
    dchk("storage: post-publish cleanup failure is explicit",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_ERR_CLEANUP);
    dchk("storage: cleanup failure keeps authoritative SD",
         wallet_seed_mode() == WSEED_MODE_SD && seed_loads_as(SD_WORDS));
    dchk("storage: cleanup state can move safely back to KEEP",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_OK);

    // Setup commit uses the same publish contract. CLEANUP after mode SD is
    // active must retain the card, and replacing SD -> SD needs no mode publish.
    wallet_seed_stage_mode(WSEED_MODE_SD);
    dchk("storage: stage SD setup replacement",
         wallet_seed_stage(SD_WORDS_ALT) == WSEED_OK);
    wallet_seed_test_fail_next(WSEED_TEST_FAIL_SEED_REMOVE);
    dchk("storage: SD setup post-publish cleanup is explicit",
         wallet_seed_commit() == WSEED_ERR_CLEANUP);
    dchk("storage: cleanup setup committed SD words",
         wallet_seed_mode() == WSEED_MODE_SD && seed_loads_as(SD_WORDS_ALT));
    wallet_seed_stage_mode(WSEED_MODE_SD);
    dchk("storage: stage SD -> SD replacement",
         wallet_seed_stage(SD_WORDS) == WSEED_OK);
    dchk("storage: SD -> SD replacement commits",
         wallet_seed_commit() == WSEED_OK);
    dchk("storage: SD -> SD replacement words", seed_loads_as(SD_WORDS));
    dchk("storage: return replacement fixture to KEEP",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_OK);

    // KEEP -> SD: encrypted destination verifies before one metadata publish.
    dchk("storage: capture key before KEEP -> SD",
         sd_seed_device_key(move_key) == 0);
    dchk("storage: KEEP -> SD", wallet_seed_move_to(WSEED_MODE_SD) == WSEED_OK);
    dchk("storage: mode reads SD", wallet_seed_mode() == WSEED_MODE_SD);
    dchk("storage: SD words round-trip", seed_loads_as(SD_WORDS));
    dchk("storage: dkey survives KEEP -> SD",
         sd_seed_device_key(same_key) == 0 &&
         memcmp(move_key, same_key, sizeof move_key) == 0);

    // Missing/corrupt second factor is not factory-fresh and never falls back
    // to stale NVS words.
    platform_sd_test_set_present(0);
    memset(got, 'x', sizeof got);
    dchk("storage: configured SD still exists without card",
         wallet_seed_exists() == 1);
    dchk("storage: SD load reports missing card",
         wallet_seed_load(got, sizeof got) == WSEED_ERR_SD_MISSING);
    dchk("storage: missing-card load clears output", all_zero(got, sizeof got));
    dchk("storage: missing card cannot move to KEEP",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_ERR_SD_MISSING);
    dchk("storage: missing card leaves mode SD", wallet_seed_mode() == WSEED_MODE_SD);
    platform_sd_test_set_present(1);

    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_READ);
    dchk("storage: injected SD read is distinct",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_ERR_SD_IO);
    dchk("storage: failed read leaves SD", wallet_seed_mode() == WSEED_MODE_SD);

    wallet_seed_test_fail_next(WSEED_TEST_FAIL_MODE_WRITE);
    dchk("storage: SD -> KEEP mode-write failure rolls back",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_ERR_SD_IO);
    dchk("storage: rolled-back KEEP move leaves SD usable",
         wallet_seed_mode() == WSEED_MODE_SD && seed_loads_as(SD_WORDS));

    wallet_seed_test_fail_next(WSEED_TEST_FAIL_MODE_WRITE |
                               WSEED_TEST_FAIL_SEED_REMOVE);
    dchk("storage: failed KEEP rollback is explicit cleanup",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_ERR_CLEANUP);
    dchk("storage: cleanup result has KEEP destination active",
         wallet_seed_mode() == WSEED_MODE_KEEP && seed_loads_as(SD_WORDS));

    // A no-op retry is safe after the destination committed with cleanup.
    dchk("storage: KEEP retry after cleanup", wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_OK);
    dchk("storage: KEEP words after SD move", seed_loads_as(SD_WORDS));
    dchk("storage: SD file removed after KEEP",
         platform_sd_read(SDSEED_FILENAME, blob2, sizeof blob2, &len2) != 0);

    // A cleanup failure is explicit, but the verified destination is active.
    dchk("storage: KEEP -> SD again", wallet_seed_move_to(WSEED_MODE_SD) == WSEED_OK);
    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_DELETE);
    dchk("storage: SD -> KEEP reports cleanup failure",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_ERR_CLEANUP);
    dchk("storage: cleanup failure still commits KEEP",
         wallet_seed_mode() == WSEED_MODE_KEEP && seed_loads_as(SD_WORDS));
    platform_sd_test_fail_next(0);
    dchk("storage: remove injected leftover",
         platform_sd_delete(SDSEED_FILENAME) == 0);

    // KEEP -> AMNESIC -> KEEP. The destination is RAM until lock.
    dchk("storage: KEEP -> AMNESIC",
         wallet_seed_move_to(WSEED_MODE_AMNESIC) == WSEED_OK);
    dchk("storage: AMNESIC mode active",
         wallet_seed_mode() == WSEED_MODE_AMNESIC && seed_loads_as(SD_WORDS));
    wallet_seed_forget();
    dchk("storage: AMNESIC forget clears RAM",
         wallet_seed_exists() == 0 &&
         wallet_seed_load(got, sizeof got) == WSEED_ERR_NO_SEED);
    dchk("storage: stage AMNESIC words", wallet_seed_stage(SD_WORDS_ALT) == WSEED_OK);
    dchk("storage: AMNESIC -> KEEP",
         wallet_seed_move_to(WSEED_MODE_KEEP) == WSEED_OK);
    dchk("storage: AMNESIC -> KEEP words", seed_loads_as(SD_WORDS_ALT));

    // SD -> AMNESIC -> SD completes the other two directed transitions.
    dchk("storage: KEEP -> SD for amnesic path",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_OK);
    dchk("storage: SD -> AMNESIC",
         wallet_seed_move_to(WSEED_MODE_AMNESIC) == WSEED_OK);
    dchk("storage: SD -> AMNESIC words remain in RAM", seed_loads_as(SD_WORDS_ALT));
    wallet_seed_forget();
    dchk("storage: stage for AMNESIC -> SD", wallet_seed_stage(SD_WORDS) == WSEED_OK);
    dchk("storage: AMNESIC -> SD",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_OK);
    dchk("storage: AMNESIC -> SD words", seed_loads_as(SD_WORDS));

    // WIPE is complete once the only device key is destroyed, even if deleting
    // the now-useless ciphertext fails. Prove that first with a present card.
    size_t old_blob_len = 0;
    uint8_t old_blob[SDSEED_MAX_BLOB], old_key[32], new_key[32];
    dchk("storage: capture card before delete-failure wipe",
         platform_sd_read(SDSEED_FILENAME, old_blob, sizeof old_blob,
                          &old_blob_len) == 0 &&
         sd_seed_device_key(old_key) == 0);
    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_DELETE);
    dchk("storage: wipe succeeds when card delete fails",
         wallet_seed_wipe() == WSEED_OK);
    dchk("storage: delete-failure wipe resets to empty KEEP",
         wallet_seed_mode() == WSEED_MODE_KEEP && wallet_seed_exists() == 0);
    dchk("storage: delete-failure wipe rotates device key",
         sd_seed_device_key(new_key) == 0 &&
         memcmp(old_key, new_key, sizeof old_key) != 0);
    memset(got, 'x', sizeof got);
    dchk("storage: leftover card cannot decrypt after wipe",
         sd_seed_open(new_key, old_blob, old_blob_len, got, sizeof got) != 0);
    dchk("storage: delete leftover after wipe test",
         platform_sd_delete(SDSEED_FILENAME) == 0);

    // A card elsewhere cannot be deleted either. Reinsert its captured blob
    // under the newly generated key: authentication must still fail.
    dchk("storage: restore KEEP for absent-card wipe",
         wallet_seed_store(SD_WORDS) == WSEED_OK);
    dchk("storage: restore SD for absent-card wipe",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_OK);
    dchk("storage: capture card before absent-card wipe",
         platform_sd_read(SDSEED_FILENAME, old_blob, sizeof old_blob,
                          &old_blob_len) == 0 &&
         sd_seed_device_key(old_key) == 0);
    platform_sd_test_set_present(0);
    dchk("storage: wipe succeeds with card absent", wallet_seed_wipe() == WSEED_OK);
    dchk("storage: wipe resets to empty KEEP",
         wallet_seed_mode() == WSEED_MODE_KEEP && wallet_seed_exists() == 0);
    platform_sd_test_set_present(1);
    dchk("storage: wipe generated key is different",
         sd_seed_device_key(new_key) == 0 &&
         memcmp(old_key, new_key, sizeof old_key) != 0);
    memset(got, 'x', sizeof got);
    dchk("storage: absent card copy no longer decrypts",
         sd_seed_open(new_key, old_blob, old_blob_len, got, sizeof got) != 0);
    dchk("storage: unreadable old card leaks no words", all_zero(got, sizeof got));
    dchk("storage: delete reinserted invalidated card",
         platform_sd_delete(SDSEED_FILENAME) == 0);

    // Authenticated corruption is distinct from absence and still cannot make
    // an SD-configured signer look factory fresh.
    dchk("storage: restore KEEP for corruption test",
         wallet_seed_store(SD_WORDS) == WSEED_OK);
    dchk("storage: move corruption fixture to SD",
         wallet_seed_move_to(WSEED_MODE_SD) == WSEED_OK);
    static const uint8_t junk[] = { 'n', 'o', 't', '-', 'a', '-', 's', 'e', 'e', 'd' };
    dchk("storage: overwrite card with authenticated-invalid bytes",
         platform_sd_write_atomic(SDSEED_FILENAME, junk, sizeof junk) == 0);
    memset(got, 'x', sizeof got);
    dchk("storage: corrupt card is distinct",
         wallet_seed_load(got, sizeof got) == WSEED_ERR_SD_CORRUPT);
    dchk("storage: corrupt card still counts configured",
         wallet_seed_exists() == 1);
    dchk("storage: corrupt-card load clears output", all_zero(got, sizeof got));

    // Leave the suite's canonical development mnemonic in KEEP.
    dchk("storage: final wipe", wallet_seed_wipe() == WSEED_OK);
    dchk("storage: restore dev words for suite",
         wallet_seed_store(SD_WORDS) == WSEED_OK);
    memset(move_key, 0, sizeof move_key);
    memset(same_key, 0, sizeof same_key);
    memset(old_blob, 0, sizeof old_blob);
    memset(old_key, 0, sizeof old_key);
    memset(new_key, 0, sizeof new_key);

    return dfails;
}
