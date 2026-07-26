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
#include "sp_spend_vectors.h"
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
    char buf[120], test_addr[120], main_addr[120];
    spchk("address encode ok",
          sp_address_encode(SPV_ADDR_SCAN, SPV_ADDR_SPEND, true, buf, sizeof buf) == 0);
    spchk("address matches embit encoding", strcmp(buf, SPV_ADDR_EXPECT) == 0);
    snprintf(test_addr, sizeof test_addr, "%s", buf);
    spchk("testnet address network recognized", sp_address_network(test_addr) == 2);
    // cap one byte too small must refuse, not truncate
    char tiny[117];
    spchk("address encode refuses short buffer",
          sp_address_encode(SPV_ADDR_SCAN, SPV_ADDR_SPEND, true, tiny, sizeof tiny) != 0);
    // mainnet flavor: same data, sp1 prefix
    spchk("mainnet flavor ok",
          sp_address_encode(SPV_ADDR_SCAN, SPV_ADDR_SPEND, false, buf, sizeof buf) == 0 &&
          strncmp(buf, "sp1", 3) == 0 && strlen(buf) == 116);
    snprintf(main_addr, sizeof main_addr, "%s", buf);
    spchk("mainnet address network recognized", sp_address_network(main_addr) == 1);

    char corrupt[120];
    snprintf(corrupt, sizeof corrupt, "%s", main_addr);
    size_t n = strlen(corrupt);
    corrupt[n - 1] = corrupt[n - 1] == 'q' ? 'p' : 'q';
    spchk("silent-payment checksum corruption rejected",
          sp_address_network(corrupt) == 0);
    corrupt[n - 1] = 0;
    spchk("silent-payment wrong length rejected",
          sp_address_network(corrupt) == 0);

    wallet_set_network(0);
    spchk("address validator accepts current-network sp1",
          wallet_address_validate(main_addr) == WADDR_CURRENT_NETWORK);
    spchk("address validator marks tsp1 as wrong network",
          wallet_address_validate(test_addr) == WADDR_WRONG_NETWORK);
    wallet_set_network(1);
    spchk("address validator accepts current-network tsp1",
          wallet_address_validate(test_addr) == WADDR_CURRENT_NETWORK);
    spchk("address validator marks sp1 as wrong network",
          wallet_address_validate(main_addr) == WADDR_WRONG_NETWORK);
    wallet_set_network(0);
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

// Finding #1 was raised as an untested "same scan key, multiple outputs" path.
// It is already covered: vector 2's recipients 1 and 2 are the SAME silent
// payment address, so its outputs exercise k=0 then k=1 for one scan key against
// the reference. Pin that explicitly - deriving that recipient once vs twice
// must agree on k=0 (the counter is shared and starts at 0) and yield a distinct
// k=1 output, and both must be among vector 2's reference-expected keys.
static void sp_test_sameaddr(void) {
    bool xf[2] = { false, false };
    uint8_t a_sum[32], a_pub[33], ih[32];
    if (sp_sum_privkeys(spv352_2_privs, xf, 2, a_sum, a_pub) != 0 ||
        sp_input_hash(spv352_2_outpoints, 2, a_pub, ih) != 0) {
        spchk("same-address setup", 0);
        return;
    }
    const uint8_t *scan  = spv352_2_recipkeys + 66;        // recipient 1 == 2
    const uint8_t *spend = spv352_2_recipkeys + 66 + 33;
    spchk("vector 2 recipients 1 and 2 are the same address",
          memcmp(spv352_2_recipkeys + 66, spv352_2_recipkeys + 132, 66) == 0);

    uint8_t share[33];
    spchk("same-address ecdh share", sp_ecdh_share(a_sum, scan, share) == 0);

    sp_recip_t one[1];
    memcpy(one[0].scan, scan, 33);
    memcpy(one[0].spend, spend, 33);
    spchk("same-address derive x1", sp_derive_group(share, ih, one, 1) == 0);

    sp_recip_t two[2];
    for (int i = 0; i < 2; i++) {
        memcpy(two[i].scan, scan, 33);
        memcpy(two[i].spend, spend, 33);
    }
    spchk("same-address derive x2", sp_derive_group(share, ih, two, 2) == 0);

    spchk("k=0 identical whether derived alone or first in a group",
          memcmp(one[0].xonly_out, two[0].xonly_out, 32) == 0);
    spchk("k=1 output is distinct from k=0",
          memcmp(two[0].xonly_out, two[1].xonly_out, 32) != 0);

    int found0 = 0, found1 = 0;
    for (int e = 0; e < 3; e++) {
        if (memcmp(two[0].xonly_out, spv352_2_expect + e * 32, 32) == 0) found0 = 1;
        if (memcmp(two[1].xonly_out, spv352_2_expect + e * 32, 32) == 0) found1 = 1;
    }
    spchk("k=0 output matches a BIP352 reference key", found0);
    spchk("k=1 output matches a BIP352 reference key", found1);
}

// BIP375 recipient ordering. Within one scan-key group, k does NOT follow PSBT
// output order: the codes are sorted lexicographically by spend key ascending,
// and only a subgroup sharing BOTH scan and spend keys is ordered among itself
// by output index. Getting this wrong is silent and expensive -- BIP352
// scanning walks k = 0, 1, 2 ... and STOPS at the first one it cannot find, so
// a recipient handed k=1 where they expected k=0 never detects the payment at
// all. Two labelled addresses of one wallet share a scan key, so this is an
// ordinary payment, not a corner case.
static void sp_test_order(void) {
    sp_recip_t r[4];
    uint32_t idx[4] = { 7, 2, 5, 1 };
    // one shared scan key; spend keys deliberately DESCENDING, and the middle
    // two identical so the output-index tiebreak has something to do
    static const uint8_t SPEND_BYTE[4] = { 0xdd, 0x99, 0x99, 0x11 };
    for (int i = 0; i < 4; i++) {
        memset(r[i].scan, 0, 33);
        r[i].scan[0] = 0x02;
        r[i].scan[1] = 0xaa;
        memset(r[i].spend, 0, 33);
        r[i].spend[0] = 0x02;
        r[i].spend[1] = SPEND_BYTE[i];
    }

    sp_sort_group(r, idx, 4);

    spchk("order: lowest spend key takes k=0", r[0].spend[1] == 0x11);
    spchk("order: equal spend keys take the middle", r[1].spend[1] == 0x99 &&
                                                     r[2].spend[1] == 0x99);
    spchk("order: highest spend key takes k=3", r[3].spend[1] == 0xdd);
    spchk("order: equal spend keys break the tie on output index",
          idx[1] == 2 && idx[2] == 5);
    spchk("order: every output index follows its own recipient",
          idx[0] == 1 && idx[3] == 7);

    // already sorted stays put (the sort must be a no-op, not a shuffle)
    sp_sort_group(r, idx, 4);
    spchk("order: sorting twice changes nothing",
          r[0].spend[1] == 0x11 && r[3].spend[1] == 0xdd &&
          idx[0] == 1 && idx[1] == 2 && idx[2] == 5 && idx[3] == 7);
}

// Stage A receive: derive this wallet's own sp1/tsp1 from the session master
// (m/352'/coin'/0'/1'/0 scan, m/352'/coin'/0'/0'/0 spend). Reference pubkeys +
// addresses are from embit (independent of libwally) for the dev mnemonic
// "abandon...about". Catches a wrong path (coin type, hardening, scan/spend swap).
static void sp_test_receive(void) {
    static const uint8_t SP_SCAN_MAIN[33] = { 0x02, 0x41, 0x39, 0xb0, 0xf8, 0x10, 0x42, 0xe2, 0x43, 0xa9, 0x04, 0x78, 0xe4, 0x39, 0x90, 0xc6, 0xa2, 0x7b, 0xe0, 0xe3, 0x34, 0x6f, 0x0c, 0x71, 0xad, 0xbb, 0xcd, 0xd5, 0x11, 0xbe, 0xae, 0xa1, 0xe3 };
    static const uint8_t SP_SPEND_MAIN[33] = { 0x02, 0xfa, 0x21, 0x0b, 0x3c, 0x4a, 0x60, 0xb8, 0x0d, 0xd1, 0x61, 0x6f, 0x48, 0xae, 0x53, 0xbb, 0xdf, 0x0d, 0xb7, 0x44, 0xb3, 0xf9, 0x08, 0x33, 0x85, 0x10, 0x8f, 0x81, 0xbe, 0x0a, 0xcb, 0x58, 0xc6 };
    static const uint8_t SP_SCAN_TEST[33] = { 0x03, 0x43, 0x9f, 0xc2, 0x30, 0x18, 0x2b, 0x46, 0xff, 0x22, 0x03, 0x2c, 0xa7, 0xae, 0x2d, 0xff, 0x32, 0x95, 0x8c, 0x9a, 0x0d, 0x17, 0x7c, 0x7b, 0x70, 0x96, 0xdf, 0x0d, 0x4b, 0x71, 0x41, 0xeb, 0x21 };
    static const uint8_t SP_SPEND_TEST[33] = { 0x02, 0x83, 0x30, 0x85, 0xc9, 0xa7, 0x16, 0xd3, 0x6b, 0x46, 0x75, 0x52, 0xc0, 0x0d, 0x6a, 0xa8, 0xbd, 0x42, 0xe3, 0x9a, 0xdb, 0xe9, 0x8b, 0x05, 0xbc, 0x20, 0x31, 0x10, 0x17, 0x71, 0x92, 0xf7, 0x02 };
    static const char *SP1_MAIN  = "sp1qqfqnnv8czppwysafq3uwgwvsc638hc8rx3hscuddh0xa2yd746s7xqh6yy9ncjnqhqxazct0fzh98w7lpkm5fvlepqec2yy0sxlq4j6ccc3h6t0g";
    static const char *TSP1_TEST = "tsp1qqdpels3srq45dlezqvk20t3dlueftry6p5thc7msjm0s6jm3g84jzq5rxzzunfck6d45va2jcqxk429agt3e4klf3vzmcgp3zqthryhhqgnz4k3n";

    const struct ext_key *m = wallet_session_master();
    if (!m) { spchk("receive: session master available", 0); return; }

    uint8_t scan[33], spend[33];
    char addr[120];

    spchk("receive keys mainnet rc", sp_receive_keys(m, false, scan, spend) == 0);
    spchk("receive scan key mainnet", memcmp(scan, SP_SCAN_MAIN, 33) == 0);
    spchk("receive spend key mainnet", memcmp(spend, SP_SPEND_MAIN, 33) == 0);
    spchk("receive sp1 address matches embit",
          sp_address_encode(scan, spend, false, addr, sizeof addr) == 0 &&
          strcmp(addr, SP1_MAIN) == 0);

    // the exact call the Receive screen makes, at the buffer size it passes
    {
        char sess[128];
        int slen;
        wallet_set_network(0);
        spchk("session sp address mainnet rc", wallet_session_sp_address(sess, sizeof sess) == 0);
        slen = (int)strlen(sess);
        if (strcmp(sess, SP1_MAIN) != 0) printf("  got  %s\n  want %s\n", sess, SP1_MAIN);
        spchk("session sp address mainnet full + correct",
              slen == 116 && strcmp(sess, SP1_MAIN) == 0);
        wallet_set_network(1);
        spchk("session sp address testnet rc", wallet_session_sp_address(sess, sizeof sess) == 0);
        slen = (int)strlen(sess);
        if (strcmp(sess, TSP1_TEST) != 0) printf("  got  %s (%d)\n  want %s (%d)\n",
                                                 sess, slen, TSP1_TEST, (int)strlen(TSP1_TEST));
        spchk("session sp address testnet full + correct",
              slen == 117 && strcmp(sess, TSP1_TEST) == 0);
        wallet_set_network(0);
    }

    spchk("receive keys testnet rc", sp_receive_keys(m, true, scan, spend) == 0);
    spchk("receive scan key testnet", memcmp(scan, SP_SCAN_TEST, 33) == 0);
    spchk("receive spend key testnet", memcmp(spend, SP_SPEND_TEST, 33) == 0);
    spchk("receive tsp1 address matches embit",
          sp_address_encode(scan, spend, true, addr, sizeof addr) == 0 &&
          strcmp(addr, TSP1_TEST) == 0);
}

// Stage B: sp(spscan) scan-key export. The full descriptor (origin + spscan1
// key expression) must match embit's SilentPaymentDescriptor byte-for-byte for
// the dev mnemonic on both networks. Also pins the low-level sp_scan_encode.
static void sp_test_scan_export(void) {
    char out[200];

    wallet_set_network(0);
    spchk("scan export mainnet rc", wallet_session_sp_scan_export(out, sizeof out) == 0);
    spchk("scan export mainnet matches embit sp(spscan)",
          strcmp(out, SPV_SPSCAN_MAIN) == 0);

    wallet_set_network(1);
    spchk("scan export testnet rc", wallet_session_sp_scan_export(out, sizeof out) == 0);
    spchk("scan export testnet matches embit sp(tspscan)",
          strcmp(out, SPV_SPSCAN_TEST) == 0);

    // low-level encoder: hrp + version-0 + convertbits(scan_priv||spend_pub)
    const struct ext_key *m = wallet_session_master();
    uint8_t scan_priv[32], spend_pub[33];
    char key[120];
    spchk("scan export keys testnet rc", sp_scan_export_keys(m, true, scan_priv, spend_pub) == 0);
    spchk("sp_scan_encode tspscan prefix + length",
          sp_scan_encode(scan_priv, spend_pub, true, key, sizeof key) == 0 &&
          strncmp(key, "tspscan1", 8) == 0 && strstr(SPV_SPSCAN_TEST, key) != NULL);
    // one byte too small must refuse, never truncate a secret-bearing string
    char tiny[64];
    spchk("sp_scan_encode refuses short buffer",
          sp_scan_encode(scan_priv, spend_pub, true, tiny, sizeof tiny) != 0);
    wallet_set_network(0);
}

// Read a PSBT input's taproot key-path signature (PSBT_IN_TAP_KEY_SIG = 0x13,
// stored by wally in the psbt_fields map). BIP341 allows two encodings: 64
// bytes for SIGHASH_DEFAULT, or 65 with the hash type appended for anything
// else. Returns 0 and fills sig (and *len, 64 or 65) on success.
static int sp_tap_key_sig(const struct wally_psbt *p, size_t idx,
                          uint8_t sig[65], size_t *len) {
    const struct wally_map_item *it =
        wally_map_get_integer(&p->inputs[idx].psbt_fields, 0x13);
    if (!it || (it->value_len != 64 && it->value_len != 65)) return -1;
    memcpy(sig, it->value, it->value_len);
    *len = it->value_len;
    return 0;
}

// Stage C: BIP376 spend of a received silent-payment coin. The signer must
// recompute d = b_spend + tweak, refuse a tweak that doesn't reproduce the
// on-chain P2TR key, and Schnorr-sign the taproot key-path sighash. Verifying
// the emitted signature against embit's OUTKEY + SIGHASH cross-checks BOTH our
// tweaked key and our sighash against the independent implementation.
static void sp_test_spend_one(const char *tag, const char *b64,
                              const uint8_t *outkey, const uint8_t *sighash) {
    char name[80];
    wpsbt_summary_t sum;
    uint8_t out1[4096], out2[4096];
    size_t w1 = 0, w2 = 0;
    wallet_set_network(1);

    int rc = wallet_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    snprintf(name, sizeof name, "%s loads READY", tag);
    if (rc == 0 && sum.status != WPSBT_READY)
        printf("  status=%d reason=%s\n", sum.status, sum.reason);
    spchk(name, rc == 0 && sum.status == WPSBT_READY);
    snprintf(name, sizeof name, "%s counted as 1 received-SP input", tag);
    spchk(name, sum.n_sp_in == 1 && sum.n_in == 1);
    snprintf(name, sizeof name, "%s input amount + fee", tag);
    spchk(name, sum.in_sats == 100000 && sum.send_sats == 95000 && sum.fee_sats == 5000);

    snprintf(name, sizeof name, "%s sign rc", tag);
    spchk(name, wallet_psbt_sign(out1, sizeof out1, &w1) == 0 && w1 > 0);
    wallet_psbt_free();

    struct wally_psbt *p = NULL;
    snprintf(name, sizeof name, "%s signed psbt strict-parses", tag);
    spchk(name, wally_psbt_from_bytes(out1, w1, 0, &p) == WALLY_OK);
    if (p) {
        uint8_t sig[65];
        size_t siglen = 0;
        int have = sp_tap_key_sig(p, 0, sig, &siglen) == 0 && siglen == 64;
        snprintf(name, sizeof name, "%s carries a 64-byte taproot key sig", tag);
        spchk(name, have);
        // the crux: our signature verifies under embit's output key AND embit's
        // sighash - so our tweak math and our BIP341 sighash both match embit
        snprintf(name, sizeof name, "%s sig verifies vs embit outkey+sighash", tag);
        spchk(name, have && sp_schnorr_verify(outkey, sighash, sig) == 0);
        wally_psbt_free(p);
    }

    // determinism: identical load+sign yields identical bytes
    rc = wallet_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    spchk(rc == 0 ? "spend re-load READY" : "spend re-load", rc == 0 && sum.status == WPSBT_READY);
    spchk("spend re-sign rc", wallet_psbt_sign(out2, sizeof out2, &w2) == 0);
    snprintf(name, sizeof name, "%s sign is deterministic", tag);
    spchk(name, w1 == w2 && memcmp(out1, out2, w1) == 0);
    wallet_psbt_free();
    wallet_set_network(0);
}

// BIP341: a taproot signature is 64 bytes ONLY for SIGHASH_DEFAULT. With any
// explicit hash type the byte is appended, making 65 -- and a verifier reads a
// bare 64-byte signature as DEFAULT, so committing to 0x01 and then emitting 64
// bytes produces a signature that simply does not verify. The load gate accepts
// an explicit SIGHASH_ALL, so the signer has to encode one properly.
// wally handles this for its own bip32 inputs; the SP path is hand-rolled.
static void sp_test_spend_explicit_sighash(void) {
    struct wally_psbt *p = NULL;
    char *b64 = NULL;
    wpsbt_summary_t sum;
    uint8_t out[4096];
    size_t w = 0;

    // same fixture, with PSBT_IN_SIGHASH_TYPE = SIGHASH_ALL set explicitly
    if (wally_psbt_from_base64(SPV_SPEND_EVEN_B64, 0, &p) != WALLY_OK ||
        wally_psbt_set_input_sighash(p, 0, WALLY_SIGHASH_ALL) != WALLY_OK ||
        wally_psbt_to_base64(p, 0, &b64) != WALLY_OK) {
        spchk("explicit-sighash fixture builds", 0);
        if (p) wally_psbt_free(p);
        return;
    }
    wally_psbt_free(p);
    p = NULL;

    wallet_set_network(1);
    int rc = wallet_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    spchk("explicit SIGHASH_ALL loads READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("explicit SIGHASH_ALL signs",
          wallet_psbt_sign(out, sizeof out, &w) == 0 && w > 0);
    wallet_psbt_free();

    if (wally_psbt_from_bytes(out, w, 0, &p) == WALLY_OK) {
        uint8_t sig[65];
        size_t siglen = 0;
        int have = sp_tap_key_sig(p, 0, sig, &siglen) == 0;
        spchk("explicit SIGHASH_ALL yields a 65-byte taproot sig",
              have && siglen == 65);
        spchk("explicit SIGHASH_ALL appends the 0x01 hash type",
              have && siglen == 65 && sig[64] == 0x01);
        // and it must be a signature over the DIFFERENT message that hash type
        // implies, not the DEFAULT sighash with a byte stapled on the end
        spchk("explicit SIGHASH_ALL commits to its own sighash",
              have && sp_schnorr_verify(SPV_SPEND_EVEN_OUTKEY,
                                        SPV_SPEND_EVEN_SIGHASH, sig) != 0);
        wally_psbt_free(p);
    } else {
        spchk("explicit-sighash signed psbt strict-parses", 0);
    }
    wally_free_string(b64);
    wallet_set_network(0);
}

static void sp_test_spend(void) {
    sp_test_spend_one("spend even-Y", SPV_SPEND_EVEN_B64,
                      SPV_SPEND_EVEN_OUTKEY, SPV_SPEND_EVEN_SIGHASH);
    sp_test_spend_one("spend odd-Y", SPV_SPEND_ODD_B64,
                      SPV_SPEND_ODD_OUTKEY, SPV_SPEND_ODD_SIGHASH);

    // foreign tweak: the PSBT's tweak does NOT reproduce the on-chain P2TR key.
    // The signer MUST refuse (BIP376 anti-theft), never emit a signature.
    wpsbt_summary_t sum;
    wallet_set_network(1);
    int rc = wallet_psbt_load((const uint8_t *)SPV_SPEND_FOREIGN_B64,
                              strlen(SPV_SPEND_FOREIGN_B64), &sum);
    spchk("foreign-tweak spend stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "not this wallet"));
    wallet_psbt_free();
    wallet_set_network(0);
}

// change/self detection: the expected spend key for a labeled SP output is
// spend_pub + hash("BIP0352/Label", scan_priv||ser32(m))*G. label 0 = change.
static void sp_test_label(void) {
    uint8_t out[33];
    spchk("label 0 (change) spend key matches embit",
          sp_label_spend(SPV_LABEL_SCAN_PRIV, SPV_LABEL_SPEND_PUB, 0, out) == 0 &&
          memcmp(out, SPV_LABEL0_SPEND, 33) == 0);
    spchk("label 5 (self) spend key matches embit",
          sp_label_spend(SPV_LABEL_SCAN_PRIV, SPV_LABEL_SPEND_PUB, SPV_LABEL5, out) == 0 &&
          memcmp(out, SPV_LABEL5_SPEND, 33) == 0);
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


// ---------------------------------------------------------------------------
// FIELD REGRESSION: a real BIP376 spend PSBT produced by Sparrow, captured off
// the device on 2026-07-26 after it was refused with "input is not this
// wallet's". Sparrow's PSBT is CORRECT -- input 0 carries both
// PSBT_IN_SP_SPEND_BIP32_DERIVATION (0x1f) and PSBT_IN_SP_TWEAK (0x20), and
// output 1 carries PSBT_OUT_SP_V0_INFO (0x09) + label (0x0a). libwally's LOOSE
// parse exposes all four.
//
// The seed here is the dev mnemonic, NOT the wallet that owns this coin, so
// ownership MUST fail. What is being pinned is WHICH failure: seeing 0x20 sends
// the input down the BIP376 branch and the refusal names it ("silent-payment
// input is not this wallet's"). The generic "input is not this wallet's" means
// the tweak was never noticed at all, which is the bug this test exists for.
static const char SPARROW_SPEND_B64[] =
    "cHNidP8BAgQCAAAAAQMErzkCAAEEAQEBBQECAfsEAgAAAAABASvQBwAAAAAAACJRIDx5wy4r"
    "sRNB/bfoSQtFeH5AHoscbB9XDAfsT87c7TBfAQMEAAAAAAEOIBj3xTT4d5K75ronC2h3FWo9"
    "s1z2+R6xhhtsMdNhEcWyAQ8EAAAAAAEQBP3///8iHwMF6Au+pV0xZmB3F6+4+T5k8pWB76Sz"
    "XWHE0kxAwfafwxicdShHYAEAgAEAAIAAAACAAAAAgAAAAAABICBxTJj0H2t7aJZQxvk2q7yy"
    "Ql7rl0O+Cr+OncjggQbPQQABAwjcBQAAAAAAAAEEFgAU1roTeIWdd2qGmWOwxxtPJXRAF/wA"
    "AQMITgEAAAAAAAABCUICfmBbgjKQjFO7sPPQMzOZm1fuXNM5kkKz089x3z6aqW0DadAY8O+J"
    "DJjt0rn92PgzIPOrXdhwyubNL9WqtK4uGmgBCgQAAAAAAA==";

static void sp_test_sparrow_spend(void) {
    // libwally must expose the four SP fields at all (guards a wally bump).
    struct wally_psbt *p = sp_parse_b64(SPARROW_SPEND_B64, WALLY_PSBT_PARSE_FLAG_LOOSE);
    spchk("sparrow: loose parse", p != NULL);
    if (p) {
        int have_1f = 0, have_20 = 0;
        for (size_t j = 0; j < p->inputs[0].unknowns.num_items; j++) {
            const struct wally_map_item *it = &p->inputs[0].unknowns.items[j];
            if (it->key_len == 34 && it->key[0] == 0x1f) have_1f = 1;
            if (it->key_len == 1  && it->key[0] == 0x20 && it->value_len == 32) have_20 = 1;
        }
        spchk("sparrow: input carries 0x1f spend derivation", have_1f);
        spchk("sparrow: input carries 0x20 tweak", have_20);
        spchk("sparrow: output 1 carries SP info + label",
              p->outputs[1].unknowns.num_items == 2);
        wally_psbt_free(p);
    }

    wpsbt_summary_t sum;
    wallet_set_network(1);
    int rc = wallet_psbt_load((const uint8_t *)SPARROW_SPEND_B64,
                              strlen(SPARROW_SPEND_B64), &sum);
    spchk("sparrow: loads", rc == 0);
    if (rc != 0) { printf("  load rc=%d\n", rc); return; }
    printf("  status=%d n_sp=%u n_sp_in=%u reason=%s\n",
           sum.status, (unsigned)sum.n_sp, (unsigned)sum.n_sp_in, sum.reason);
    // The coin is not ours (dev seed), so it must be refused -- but as an SP
    // input, which is what proves the tweak was read.
    spchk("sparrow: refused as a SILENT-PAYMENT input, not a generic one",
          strstr(sum.reason, "silent-payment") != NULL);
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

    // Finding #2: a PSBTv2 that ALSO smuggles a global unsigned tx (0x00) is a
    // substitution trap - display reads the embedded tx, signing builds from the
    // v2 fields. Hand-build the raw hybrid (magic, global tx of 1-in/1-out,
    // global version=2, empty input+output maps), record what libwally's loose
    // parser yields, then require our loader to refuse it however wally sees it.
    static const uint8_t hybrid[] = {
        0x70,0x73,0x62,0x74,0xff,                 // "psbt\xff"
        0x01,0x00,                                // key: global unsigned tx
        0x52,                                     // value: 82-byte tx
          0x02,0x00,0x00,0x00,                    //   version
          0x01,                                   //   1 input
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,      //   prevout hash
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0x00,0x00,0x00,0x00,                  //   prevout index
            0x00,                                 //   scriptSig len
            0xff,0xff,0xff,0xff,                  //   sequence
          0x01,                                   //   1 output
            0x00,0xe1,0xf5,0x05,0x00,0x00,0x00,0x00, // value
            0x16,0x00,0x14,                       //   P2WPKH spk (22B)
            0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
            0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
          0x00,0x00,0x00,0x00,                    //   locktime
        0x01,0xfb,0x04,0x02,0x00,0x00,0x00,       // key: psbt version = 2
        0x00,                                     // end of globals
        0x00,                                     // input 0 map (empty)
        0x00,                                     // output 0 map (empty)
    };
    struct wally_psbt *hp = NULL;
    int hrc = wally_psbt_from_bytes(hybrid, sizeof hybrid,
                                    WALLY_PSBT_PARSE_FLAG_LOOSE, &hp);
    printf("  [hybrid probe] libwally loose rc=%d version=%d has_tx=%d\n",
           hrc, hp ? (int)hp->version : -1, hp ? (hp->tx != NULL) : -1);
    if (hp) wally_psbt_free(hp);
    rc = wallet_psbt_load(hybrid, sizeof hybrid, &sum);
    spchk("v2 + embedded global tx hybrid refused",
          !(rc == 0 && sum.status == WPSBT_READY));
    if (rc == 0) wallet_psbt_free();

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

    // A BIP376 SP tweak (0x20) on a NON-taproot input is malformed/hostile: SP
    // spend fields only apply to a received P2TR coin, so the signer must refuse
    // (this fixture bolts a tweak onto a P2WPKH input). Real P2TR BIP376 spends
    // are exercised by sp_test_spend against embit-built fixtures.
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_BIP376_B64,
                          strlen(SPV_PSBT_BIP376_B64), &sum);
    spchk("SP tweak on a non-taproot input stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "taproot"));
    wallet_psbt_free();

    // same fixture on MAINNET -> wrong-network STOP (input path is 84h/1h)
    wallet_set_network(0);
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_B64,
                          strlen(SPV_PSBT_B64), &sum);
    spchk("SP fixture on mainnet stops as wrong network",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "network"));
    wallet_psbt_free();
}

// Sign path: the loader already derived + filled everything, so signing is the
// normal signer - but the OUTPUT bytes must contain exactly what Sparrow will
// verify: the expected P2TR script, the global ECDH share + a valid DLEQ
// proof, locked modifiable flags, and a signature. Then determinism: same
// wallet + same psbt = byte-identical result.
static void sp_test_sign(void) {
    wpsbt_summary_t sum;
    uint8_t out1[4096], out2[4096];
    size_t w1 = 0, w2 = 0;
    wallet_set_network(1);

    int rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_B64,
                              strlen(SPV_PSBT_B64), &sum);
    spchk("sign: fixture loads READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("sign rc", wallet_psbt_sign(out1, sizeof out1, &w1) == 0 && w1 > 0);
    wallet_psbt_free();

    struct wally_psbt *p = NULL;
    spchk("signed psbt strict-parses",
          wally_psbt_from_bytes(out1, w1, 0, &p) == WALLY_OK);
    if (p) {
        spchk("SP output script is the expected P2TR",
              p->outputs[0].script_len == 34 &&
              memcmp(p->outputs[0].script, SPV_PSBT_EXPECT_SCRIPT, 34) == 0);
        spchk("modifiable flags locked", p->tx_modifiable_flags == 0);

        uint8_t key[34], share[33] = { 0 }, proof[64] = { 0 };
        size_t item = 0;
        key[0] = 0x07;
        memcpy(key + 1, SPV_ADDR_SCAN, 33);
        int have = wally_map_find(&p->unknowns, key, 34, &item) == WALLY_OK && item &&
                   p->unknowns.items[item - 1].value_len == 33;
        if (have) memcpy(share, p->unknowns.items[item - 1].value, 33);
        spchk("global ECDH share matches embit's",
              have && memcmp(share, SPV_PSBT_EXPECT_SHARE, 33) == 0);
        key[0] = 0x08;
        have = wally_map_find(&p->unknowns, key, 34, &item) == WALLY_OK && item &&
               p->unknowns.items[item - 1].value_len == 64;
        if (have) memcpy(proof, p->unknowns.items[item - 1].value, 64);
        spchk("global DLEQ proof verifies against A_sum",
              have && sp_dleq_verify(SPV_PSBT_ASUM_PUB, SPV_ADDR_SCAN, share,
                                     proof, NULL, NULL) == 0);
        spchk("input carries a signature", p->inputs[0].signatures.num_items > 0);
        wally_psbt_free(p);
    }

    // determinism: an identical load+sign yields identical bytes
    rc = wallet_psbt_load((const uint8_t *)SPV_PSBT_B64,
                          strlen(SPV_PSBT_B64), &sum);
    spchk("re-load READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("re-sign rc", wallet_psbt_sign(out2, sizeof out2, &w2) == 0);
    spchk("sign is deterministic", w1 == w2 && memcmp(out1, out2, w1) == 0);
    wallet_psbt_free();

    wallet_set_network(0);
}

int test_sp(void) {
    sp_fails = 0;
    sp_probe_libwally();
    sp_test_address();
    sp_test_bip352();
    sp_test_sameaddr();
    sp_test_order();
    sp_test_receive();
    sp_test_scan_export();
    sp_test_label();
    sp_test_dleq();
    sp_test_load();
    sp_test_sign();
    sp_test_spend();
    sp_test_sparrow_spend();
    sp_test_spend_explicit_sighash();
    return sp_fails;
}
