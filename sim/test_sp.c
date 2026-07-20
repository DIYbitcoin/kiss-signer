// sim/test_sp.c — silent payments (BIP352/374/375) test suite.
// Task 2 probe: pin down how the vendored libwally 1.5.4 handles a
// coordinator-style BIP375 PSBTv2. Findings this suite enforces forever:
//   * strict parse REJECTS it (BIP370 makes PSBT_OUT_SCRIPT mandatory; BIP375
//     relaxes that, libwally predates the relaxation) -> the loader must retry
//     with WALLY_PSBT_PARSE_FLAG_LOOSE for exactly this shape
//   * loose parse preserves the SP fields in the unknowns maps
//   * serialization stays impossible until the script is filled, and after
//     filling it the result strict-parses, keeps 0x09, and is byte-stable
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <wally_core.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_map.h>

#include "sp_test_vectors.h"
#include "wallet_sp.h"
#include "wallet_psbt.h"
#include "wallet_crypto.h"

static int sp_fails;

static void spchk(const char *name, int ok) {
    if (ok) printf("PASS: sp: %s\n", name);
    else { printf("FAIL: sp: %s\n", name); sp_fails++; }
}

static struct wally_psbt *sp_parse_b64(const char *b64, uint32_t flags) {
    uint8_t buf[4096];
    size_t wr = 0;
    if (wally_base64_to_bytes(b64, 0, buf, sizeof buf, &wr) != WALLY_OK || !wr)
        return NULL;
    struct wally_psbt *p = NULL;
    if (wally_psbt_from_bytes(buf, wr, flags, &p) != WALLY_OK)
        return NULL;
    return p;
}

// Look up a full raw key (keytype||keydata) in an output's unknowns map.
// Returns value length via *vlen and fills val (cap bytes); 0 = found.
static int sp_output_unknown(const struct wally_psbt *p, size_t out_idx,
                             const uint8_t *key, size_t key_len,
                             uint8_t *val, size_t cap, size_t *vlen) {
    size_t idx = 0;
    if (wally_psbt_find_output_unknown((struct wally_psbt *)p, out_idx,
                                       key, key_len, &idx) != WALLY_OK || !idx)
        return -1;
    if (wally_psbt_get_output_unknown_len(p, out_idx, idx - 1, vlen) != WALLY_OK)
        return -2;
    size_t wr = 0;
    if (wally_psbt_get_output_unknown(p, out_idx, idx - 1, val, cap, &wr) != WALLY_OK)
        return -3;
    return 0;
}

static void sp_probe_libwally(void) {
    static const uint8_t key_info[1] = { 0x09 };
    uint8_t val[128];
    size_t vlen = 0;

    // strict parse must fail on the script-less SP output (documented above)
    struct wally_psbt *p = sp_parse_b64(SPV_PSBT_B64, 0);
    spchk("strict parse rejects script-less SP output", p == NULL);
    if (p) { wally_psbt_free(p); }

    p = sp_parse_b64(SPV_PSBT_B64, WALLY_PSBT_PARSE_FLAG_LOOSE);
    spchk("loose parse accepts v2 fixture", p != NULL);
    if (!p) return;

    spchk("psbt version is 2", p->version == 2);
    spchk("v2 has no global tx", p->tx == NULL);
    spchk("2 outputs, 1 input", p->num_outputs == 2 && p->num_inputs == 1);
    spchk("SP output has no script yet",
          p->outputs[0].script == NULL && p->outputs[0].script_len == 0);
    spchk("change output kept its script",
          p->outputs[1].script != NULL && p->outputs[1].script_len == 22);

    int rc = sp_output_unknown(p, 0, key_info, sizeof key_info, val, sizeof val, &vlen);
    spchk("output 0 carries 0x09 SP info", rc == 0 && vlen == 66);
    spchk("SP info = scan||spend keys",
          rc == 0 && vlen == 66 &&
          memcmp(val, SPV_ADDR_SCAN, 33) == 0 &&
          memcmp(val + 33, SPV_ADDR_SPEND, 33) == 0);
    spchk("change output has no SP info",
          sp_output_unknown(p, 1, key_info, sizeof key_info, val, sizeof val, &vlen) != 0);

    // script still missing -> serialization must refuse (nothing to leak)
    uint8_t out1[4096];
    size_t len1 = 0;
    spchk("serialize refuses while script missing",
          wally_psbt_to_bytes(p, 0, out1, sizeof out1, &len1) != WALLY_OK);

    // fill a placeholder P2TR script the way the signer will after deriving
    uint8_t scr[34] = { 0x51, 0x20 };
    memset(scr + 2, 0x11, 32);
    spchk("script fill works",
          wally_psbt_set_output_script(p, 0, scr, sizeof scr) == WALLY_OK);
    spchk("serializes after fill",
          wally_psbt_to_bytes(p, 0, out1, sizeof out1, &len1) == WALLY_OK && len1 > 0);

    struct wally_psbt *p2 = NULL;
    spchk("strict re-parse accepts filled psbt",
          wally_psbt_from_bytes(out1, len1, 0, &p2) == WALLY_OK);
    if (p2) {
        rc = sp_output_unknown(p2, 0, key_info, sizeof key_info, val, sizeof val, &vlen);
        spchk("0x09 survives round-trip", rc == 0 && vlen == 66);
        uint8_t out2[4096];
        size_t len2 = 0;
        spchk("round-trip byte-stable",
              wally_psbt_to_bytes(p2, 0, out2, sizeof out2, &len2) == WALLY_OK &&
              len2 == len1 && memcmp(out1, out2, len1) == 0);
        wally_psbt_free(p2);
    }
    wally_psbt_free(p);
}

static void sp_test_address(void) {
    char buf[120];
    spchk("address encode ok",
          sp_address_encode(SPV_ADDR_SCAN, SPV_ADDR_SPEND, true, buf, sizeof buf) == 0);
    spchk("address matches embit encoding", strcmp(buf, SPV_ADDR_EXPECT) == 0);
    // cap one byte too small must refuse, not truncate
    char tiny[117];
    spchk("address encode refuses short buffer",
          sp_address_encode(SPV_ADDR_SCAN, SPV_ADDR_SPEND, true, tiny, sizeof tiny) != 0);
    // mainnet flavor: same data, sp1 prefix
    spchk("mainnet flavor ok",
          sp_address_encode(SPV_ADDR_SCAN, SPV_ADDR_SPEND, false, buf, sizeof buf) == 0 &&
          strncmp(buf, "sp1", 3) == 0 && strlen(buf) == 116);
}

// Replicates create_outputs' recipient handling: walk recipients in order,
// group by scan key (first-appearance order), k = position within the group.
// The vector's expected data is NALT acceptable output SETS (k assignment is
// ordering-dependent), so the derived outputs multiset-match against any one.
static void sp_run_352_vector(int vi, size_t nin, size_t nout, size_t nalt,
                              const uint8_t *privs, const uint8_t *xonly,
                              const uint8_t *outpoints, const uint8_t *asum,
                              const uint8_t *recipkeys, const uint8_t *expect) {
    char name[64];
    bool xflags[8];
    for (size_t i = 0; i < nin && i < 8; i++) xflags[i] = xonly[i] != 0;

    uint8_t a_sum[32], a_pub[33], ih[32];
    snprintf(name, sizeof name, "v%d key sum", vi);
    spchk(name, sp_sum_privkeys(privs, xflags, nin, a_sum, a_pub) == 0 &&
                memcmp(a_sum, asum, 32) == 0);
    snprintf(name, sizeof name, "v%d input hash", vi);
    spchk(name, sp_input_hash(outpoints, nin, a_pub, ih) == 0);

    // derive all outputs: group recipients by scan key, k within group
    uint8_t derived[8][32];
    sp_recip_t recips[8];
    int group_of[8], order[8], n_groups = 0, derived_ok = 1;
    for (size_t o = 0; o < nout && o < 8; o++) {
        int g = -1;
        for (int j = 0; j < n_groups; j++)
            if (memcmp(recipkeys + order[j] * 66, recipkeys + o * 66, 33) == 0) { g = j; break; }
        if (g < 0) { g = n_groups++; order[g] = (int)o; }
        group_of[o] = g;
    }
    for (int g = 0; g < n_groups && derived_ok; g++) {
        size_t n_in_group = 0, src_idx[8];
        for (size_t o = 0; o < nout; o++)
            if (group_of[o] == g) {
                memcpy(recips[n_in_group].scan, recipkeys + o * 66, 33);
                memcpy(recips[n_in_group].spend, recipkeys + o * 66 + 33, 33);
                src_idx[n_in_group++] = o;
            }
        uint8_t share[33];
        if (sp_ecdh_share(a_sum, recips[0].scan, share) != 0 ||
            sp_derive_group(share, ih, recips, n_in_group) != 0) {
            derived_ok = 0;
            break;
        }
        for (size_t j = 0; j < n_in_group; j++)
            memcpy(derived[src_idx[j]], recips[j].xonly_out, 32);
    }

    // multiset-compare the derived set against each acceptable alternative
    int any_alt = 0;
    for (size_t a = 0; a < nalt && derived_ok && !any_alt; a++) {
        const uint8_t *alt = expect + a * nout * 32;
        int used[8] = { 0 }, all = 1;
        for (size_t o = 0; o < nout && all; o++) {
            int hit = 0;
            for (size_t e = 0; e < nout; e++)
                if (!used[e] && memcmp(derived[o], alt + e * 32, 32) == 0) {
                    used[e] = 1; hit = 1; break;
                }
            if (!hit) all = 0;
        }
        any_alt = all;
    }
    snprintf(name, sizeof name, "v%d derived outputs match BIP352 vectors", vi);
    spchk(name, derived_ok && any_alt);
}

static void sp_test_bip352(void) {
#define RUN352(i) sp_run_352_vector(i, SPV352_##i##_NIN, SPV352_##i##_NOUT, \
    SPV352_##i##_NALT, spv352_##i##_privs, spv352_##i##_xonly, \
    spv352_##i##_outpoints, spv352_##i##_asum, spv352_##i##_recipkeys, \
    spv352_##i##_expect)
    RUN352(0);
    RUN352(1);
    RUN352(2);
#undef RUN352
}

static void sp_test_dleq(void) {
    char name[64];
    uint8_t proof[64];
    int all = 1;

    // generation vectors (custom G + bound message)
#define DG(i) do { \
        int ok = sp_dleq_prove(spvdg_##i##_a, spvdg_##i##_B, spvdg_##i##_aux, \
                               spvdg_##i##_msg, spvdg_##i##_G, proof) == 0 && \
                 memcmp(proof, spvdg_##i##_proof, 64) == 0; \
        if (!ok) all = 0; \
    } while (0)
    DG(0); DG(1); DG(2); DG(3); DG(4); DG(5); DG(6); DG(7);
#undef DG
    spchk("dleq generation matches all BIP374 vectors", all);

    // verification vectors (positive and negative rows)
    all = 1;
#define DV(i) do { \
        int ok = sp_dleq_verify(spvdv_##i##_A, spvdv_##i##_B, spvdv_##i##_C, \
                                spvdv_##i##_proof, spvdv_##i##_msg, \
                                spvdv_##i##_G) == 0; \
        if (ok != spvdv_##i##_ok) { all = 0; \
            snprintf(name, sizeof name, "dleq verify vector %d wrong", i); \
            puts(name); } \
    } while (0)
    DV(0); DV(1); DV(2); DV(3); DV(4); DV(5); DV(6); DV(7);
    DV(8); DV(9); DV(10); DV(11); DV(12); DV(13); DV(14);
#undef DV
    spchk("dleq verification matches all BIP374 vectors", all);

    // BIP375 shape: standard G, no message, real BIP352 values
    uint8_t a_sum[32], a_pub[33], share[33];
    bool xflags[8] = { false, false };
    sp_sum_privkeys(spv352_0_privs, xflags, SPV352_0_NIN, a_sum, a_pub);
    sp_ecdh_share(a_sum, spv352_0_recipkeys, share);
    uint8_t aux[32] = { 0x42 };
    spchk("dleq prove/verify roundtrip (BIP375 shape)",
          sp_dleq_prove(a_sum, spv352_0_recipkeys, aux, NULL, NULL, proof) == 0 &&
          sp_dleq_verify(a_pub, spv352_0_recipkeys, share, proof, NULL, NULL) == 0);
    proof[10] ^= 1;
    spchk("dleq tampered proof rejected",
          sp_dleq_verify(a_pub, spv352_0_recipkeys, share, proof, NULL, NULL) != 0);
}

// Full loader-path tests: the coordinator fixture must verify READY with the
// SP output derived + displayed as its tsp1 address; the negative fixtures
// must STOP with their specific reasons. Session (abandon-mnemonic dev seed)
// is already open from the earlier suites; fixtures are testnet.
static void sp_test_load(void) {
    wpsbt_summary_t sum;
    wallet_set_network(1);

    int rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_B64,
                              strlen(SPV_PSBT_B64), &sum);
    spchk("SP fixture load rc", rc == 0);
    if (rc != 0) printf("  load rc=%d\n", rc);
    else if (sum.status != WPSBT_READY) printf("  status=%d reason=%s\n", sum.status, sum.reason);
    spchk("SP fixture READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("one SP output counted", sum.n_sp == 1 && sum.n_out == 2);
    spchk("SP output flagged + shown as tsp1 address",
          sum.outs[0].is_sp && strcmp(sum.outs[0].addr, SPV_ADDR_EXPECT) == 0);
    spchk("change still re-derives under v2",
          sum.outs[1].is_change && sum.change_sats == 20000);
    spchk("v2 fee math", sum.send_sats == 95000 && sum.fee_sats == 5000);
    spchk("no unknown-field stop for SP fields", sum.n_unknown == 0);
    wallet_psbt_free();

    // coordinator-supplied per-input shares get wiped, load stays READY
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_FOREIGN_SHARE_B64,
                          strlen(SPV_PSBT_FOREIGN_SHARE_B64), &sum);
    spchk("foreign-share fixture still READY",
          rc == 0 && sum.status == WPSBT_READY && sum.n_sp == 1);
    wallet_psbt_free();

    // SP info inside a v0 PSBT -> STOP
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_V0_SP_B64,
                          strlen(SPV_PSBT_V0_SP_B64), &sum);
    spchk("v0 + SP info stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "PSBTv2"));
    wallet_psbt_free();

    // sighash SINGLE with an SP output -> STOP (existing ALL-only gate)
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_SIGHASH_B64,
                          strlen(SPV_PSBT_SIGHASH_B64), &sum);
    spchk("SP sighash SINGLE stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "sighash"));
    wallet_psbt_free();

    // BIP376 receive-side field -> STOP
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_BIP376_B64,
                          strlen(SPV_PSBT_BIP376_B64), &sum);
    spchk("BIP376 receive field stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "receive"));
    wallet_psbt_free();

    // same fixture on MAINNET -> wrong-network STOP (input path is 84h/1h)
    wallet_set_network(0);
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_B64,
                          strlen(SPV_PSBT_B64), &sum);
    spchk("SP fixture on mainnet stops as wrong network",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "network"));
    wallet_psbt_free();
}

int test_sp(void) {
    sp_fails = 0;
    sp_probe_libwally();
    sp_test_address();
    sp_test_bip352();
    sp_test_dleq();
    sp_test_load();
    return sp_fails;
}
