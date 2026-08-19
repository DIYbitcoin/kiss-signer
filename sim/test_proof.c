// Host tests for main/kiss_proof.c: the CAMERA AUDIT pipeline.
//
// The feature's whole claim is "the file, the hash and the words agree, and
// anyone can check that off the device". So the tests pin all three against
// values computed independently of libwally (python hashlib + the vendored
// wordlist file), and then check the failure edges: a card that lies must
// surface as an error and leave no proof file behind, because a half written
// file that still matches the screen's hash would be a proof of nothing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "platform_sd.h"
#include "kiss_simpath.h"

// The fake card, wherever this run put it: see kiss_simpath.h. These were
// literal "/tmp/simsd/" concatenations, which agreed with SD_BASE only by
// coincidence and stopped agreeing the moment a run could own its own scratch.
// A small ring so the call sites stay one expression, as they were when they
// were two literals side by side.
static const char *sdp(const char *name)
{
    static char buf[4][256];
    static int slot;
    char base[192];
    slot = (slot + 1) & 3;
    snprintf(buf[slot], sizeof buf[slot], "%s/%s",
             kiss_sim_path(base, sizeof base, "simsd"), name);
    return buf[slot];
}
#include "verify_page.h"
#include "kiss_proof.h"

static int fails;

static void ok(const char *name, int cond)
{
    if (cond) { printf("PASS: %s\n", name); }
    else      { printf("FAIL: %s\n", name); fails++; }
}

// Pinned independently:
//   python3: buf[i] = (i*31+7) & 0xFF over the 1,875,328 frame bytes;
//   hashlib.sha256 for the digest; BIP39 words from the digest via the
//   wordlist in components/libwally-core/upstream/src/data/wordlists/english.c
//   (first 16 bytes of the digest ‖ first 4 bits of SHA256 over those 16,
//   12 x 11-bit indices -- the 12 word length this signer actually creates).
//   tools/verify_proof.py runs the same recipe against a real proof file.
static const char *VEC_HASH =
    "c6f9982e11f767f5e17fb8321323a2609c9d69dd787d9b69039612564ef097aa";
static const char *VEC_WORDS =
    "shoulder smoke argue catalog island wife magnet warfare craft october trigger science";

static void hex32(const uint8_t h[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}

static uint8_t *pattern_frame(void)
{
    uint8_t *buf = malloc(WPROOF_FRAME_BYTES);
    if (!buf) return NULL;
    for (size_t i = 0; i < WPROOF_FRAME_BYTES; i++)
        buf[i] = (uint8_t)(i * 31 + 7);
    return buf;
}

static void test_vector_and_file(void)
{
    uint8_t *frame = pattern_frame();
    ok("host can hold a frame", frame != NULL);
    if (!frame) return;

    platform_sd_delete(WPROOF_NAME);

    uint8_t hash[32] = {0};
    char words[512] = {0};
    ok("proof run returns OK",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_OK);

    char got[65]; hex32(hash, got);
    ok("hash matches the independent vector", strcmp(got, VEC_HASH) == 0);
    if (strcmp(got, VEC_HASH) != 0) printf("  got %s\n  want %s\n", got, VEC_HASH);

    ok("words match the independent vector", strcmp(words, VEC_WORDS) == 0);
    if (strcmp(words, VEC_WORDS) != 0) printf("  got %s\n  want %s\n", words, VEC_WORDS);

    // The file half of the property: the card holds the SUBSAMPLED bytes --
    // every second pixel of every second row -- and nothing else. Rebuilt
    // here independently of kiss_proof.c, so the two loops agreeing is the
    // test, not one loop trusted twice.
    FILE *f = fopen(sdp(WPROOF_NAME), "rb");
    ok("proof file exists", f != NULL);
    if (f) {
        uint8_t *back = malloc(WPROOF_FILE_BYTES + 1);
        size_t rd = back ? fread(back, 1, WPROOF_FILE_BYTES + 1, f) : 0;
        ok("proof file is exactly the subsampled length", rd == WPROOF_FILE_BYTES);
        bool same = back && rd == WPROOF_FILE_BYTES;
        if (same) {
            for (size_t y = 0; y < WPROOF_FILE_H && same; y++)
                for (size_t x = 0; x < WPROOF_FILE_W && same; x++) {
                    const uint8_t *sp = frame + ((y * 2) * WPROOF_FRAME_W + x * 2) * 2;
                    const uint8_t *dp = back + (y * WPROOF_FILE_W + x) * 2;
                    if (sp[0] != dp[0] || sp[1] != dp[1]) same = false;
                }
        }
        ok("proof file is the frame, every second pixel of every second row", same);
        free(back);
        fclose(f);
    }

    // The courtesy half: the checker page landed beside the frame, byte
    // identical to the embedded copy. STATELESS on purpose: the page used to
    // carry this run's hash baked in, which meant two files that had to
    // agree -- and a run interrupted between the two writes left a page
    // swearing to a frame from a run before (found on a real card). The
    // claim now lives on the device screen; the page only computes.
    f = fopen(sdp(WPROOF_PAGE_NAME), "rb");
    ok("page file exists", f != NULL);
    if (f) {
        char *back = malloc(verify_page_html_len + 2);
        size_t rd = back ? fread(back, 1, verify_page_html_len + 1, f) : 0;
        if (back) back[rd < verify_page_html_len + 1 ? rd : 0] = 0;
        ok("page file is exactly the embedded length", rd == verify_page_html_len);
        ok("page file is byte identical to the embedded page",
           back && rd == verify_page_html_len &&
           memcmp(back, verify_page_html, verify_page_html_len) == 0);
        ok("no baked claim mechanism survives anywhere",
           memmem(verify_page_html, verify_page_html_len,
                  "KISS_CLAIM", 10) == NULL);
        ok("embedded page opens like a page",
           verify_page_html_len > 14 &&
           memcmp(verify_page_html, "<!doctype html", 14) == 0);
        free(back);
        fclose(f);
    }

    // Same frame, same answers: the proof is a function of the bytes alone.
    uint8_t hash2[32] = {0};
    char words2[512] = {0};
    kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash2, words2, sizeof words2);
    ok("proof is deterministic",
       memcmp(hash, hash2, 32) == 0 && strcmp(words, words2) == 0);

    free(frame);
}

static void test_faults(void)
{
    uint8_t *frame = pattern_frame();
    if (!frame) { ok("host can hold a frame (faults)", 0); return; }

    uint8_t hash[32]; char words[512];

    // A card that refuses the write is an error, and no file of either name
    // survives to contradict the screen. Earlier OK runs left both files, so
    // each fault case starts from a bare card.
    platform_sd_delete(WPROOF_NAME);
    platform_sd_delete(WPROOF_PAGE_NAME);
    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_WRITE);
    ok("write fault surfaces as SD error",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_ERR_SD);
    FILE *f = fopen(sdp(WPROOF_NAME), "rb");
    ok("write fault leaves no proof file", f == NULL);
    if (f) fclose(f);
    f = fopen(sdp(WPROOF_PAGE_NAME), "rb");
    ok("write fault leaves no page file", f == NULL);
    if (f) fclose(f);

    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_RENAME);
    ok("rename fault surfaces as SD error",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_ERR_SD);
    f = fopen(sdp(WPROOF_NAME), "rb");
    ok("rename fault leaves no proof file", f == NULL);
    if (f) fclose(f);

    // Aim the fault at the SECOND write: the frame commits, the page fails,
    // and the run must pull the frame back out -- the header's "on error
    // neither file exists" contract, exercised end to end.
    platform_sd_test_fail_skip(1);
    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_WRITE);
    ok("page write fault surfaces as SD error",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_ERR_SD);
    f = fopen(sdp(WPROOF_NAME), "rb");
    ok("page write fault deletes the committed frame", f == NULL);
    if (f) fclose(f);
    f = fopen(sdp(WPROOF_PAGE_NAME), "rb");
    ok("page write fault leaves no page file", f == NULL);
    if (f) fclose(f);

    // No card, no proof.
    platform_sd_test_set_present(0);
    ok("absent card surfaces as SD error",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_ERR_SD);
    platform_sd_test_set_present(1);

    // A stale sidecar after a committed, verified write is success: the
    // header's CLEANUP contract, exercised end to end.
    ok("proof run succeeds again after faults",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_OK);
    platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_BAK_DELETE);
    ok("stale sidecar cleanup is still success",
       kiss_proof_run(frame, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_OK);

    free(frame);
}

static void test_args(void)
{
    uint8_t hash[32]; char words[512];
    uint8_t byte = 0;

    ok("NULL frame is refused",
       kiss_proof_run(NULL, WPROOF_FRAME_BYTES, hash, words, sizeof words)
           == WPROOF_ERR_ARG);
    ok("short frame is refused: the recipe names one exact size",
       kiss_proof_run(&byte, 1, hash, words, sizeof words) == WPROOF_ERR_ARG);
    ok("zero length is refused",
       kiss_proof_run(&byte, 0, hash, words, sizeof words) == WPROOF_ERR_ARG);
}

int test_proof(void)
{
    fails = 0;
    printf("\n-- camera audit --\n");
    platform_sd_mount();
    test_vector_and_file();
    test_faults();
    test_args();
    platform_sd_delete(WPROOF_NAME);
    platform_sd_delete(WPROOF_PAGE_NAME);
    return fails;
}
