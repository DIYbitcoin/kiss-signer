// sim/test_sp.c — silent payments (BIP352/374/375) test suite.
// Task 2 probe: pin down how the vendored libwally (1.6.0) handles a
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
#include <wally_bip32.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_map.h>
#include <wally_script.h>
#include <secp256k1.h>

#include "sp_test_vectors.h"
#include "sp_spend_vectors.h"
#include "sign_vectors.h"   // SV_SCHNORR_SP_*: golden Schnorr sigs (BIP340 ref)
#include "sp_shared_vectors.h"
#include "kiss_seed.h"
#include "kiss_sp.h"
#include "kiss_psbt.h"
#include "kiss_crypto.h"

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

    kiss_set_network(0);
    spchk("address validator accepts current-network sp1",
          kiss_address_validate(main_addr) == WADDR_CURRENT_NETWORK);
    spchk("address validator marks tsp1 as wrong network",
          kiss_address_validate(test_addr) == WADDR_WRONG_NETWORK);
    kiss_set_network(1);
    spchk("address validator accepts current-network tsp1",
          kiss_address_validate(test_addr) == WADDR_CURRENT_NETWORK);
    spchk("address validator marks sp1 as wrong network",
          kiss_address_validate(main_addr) == WADDR_WRONG_NETWORK);
    kiss_set_network(0);
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

    const struct ext_key *m = kiss_session_master();
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
        kiss_set_network(0);
        spchk("session sp address mainnet rc", kiss_session_sp_address(sess, sizeof sess) == 0);
        slen = (int)strlen(sess);
        if (strcmp(sess, SP1_MAIN) != 0) printf("  got  %s\n  want %s\n", sess, SP1_MAIN);
        spchk("session sp address mainnet full + correct",
              slen == 116 && strcmp(sess, SP1_MAIN) == 0);
        kiss_set_network(1);
        spchk("session sp address testnet rc", kiss_session_sp_address(sess, sizeof sess) == 0);
        slen = (int)strlen(sess);
        if (strcmp(sess, TSP1_TEST) != 0) printf("  got  %s (%d)\n  want %s (%d)\n",
                                                 sess, slen, TSP1_TEST, (int)strlen(TSP1_TEST));
        spchk("session sp address testnet full + correct",
              slen == 117 && strcmp(sess, TSP1_TEST) == 0);
        kiss_set_network(0);
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

    kiss_set_network(0);
    spchk("scan export mainnet rc", kiss_session_sp_scan_export(out, sizeof out) == 0);
    spchk("scan export mainnet matches embit sp(spscan)",
          strcmp(out, SPV_SPSCAN_MAIN) == 0);

    kiss_set_network(1);
    spchk("scan export testnet rc", kiss_session_sp_scan_export(out, sizeof out) == 0);
    spchk("scan export testnet matches embit sp(tspscan)",
          strcmp(out, SPV_SPSCAN_TEST) == 0);

    // low-level encoder: hrp + version-0 + convertbits(scan_priv||spend_pub)
    const struct ext_key *m = kiss_session_master();
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
    kiss_set_network(0);
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
                              const uint8_t *outkey, const uint8_t *sighash,
                              const char *golden) {
    char name[80];
    wpsbt_summary_t sum;
    uint8_t out1[4096], out2[4096];
    size_t w1 = 0, w2 = 0;
    kiss_set_network(1);

    int rc = kiss_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    snprintf(name, sizeof name, "%s loads READY", tag);
    if (rc == 0 && sum.status != WPSBT_READY)
        printf("  status=%d reason=%s\n", sum.status, sum.reason);
    spchk(name, rc == 0 && sum.status == WPSBT_READY);
    snprintf(name, sizeof name, "%s counted as 1 received-SP input", tag);
    spchk(name, sum.n_sp_in == 1 && sum.n_in == 1);
    snprintf(name, sizeof name, "%s input amount + fee", tag);
    spchk(name, sum.in_sats == 100000 && sum.send_sats == 95000 && sum.fee_sats == 5000);

    snprintf(name, sizeof name, "%s sign rc", tag);
    spchk(name, kiss_psbt_sign(out1, sizeof out1, &w1) == 0 && w1 > 0);
    kiss_psbt_free();
    // The QR copy of a received-SP spend is the whole signed PSBT: its
    // BIP376 fields are not trimmed through (kiss_psbt_export_copy).
    snprintf(name, sizeof name, "%s QR copy is the whole PSBT", tag);
    spchk(name, kiss_psbt_export_copy(out1, w1, out2, sizeof out2, &w2) == 0 &&
                w2 == w1 && memcmp(out1, out2, w1) == 0);
    w2 = 0;

    struct wally_psbt *p = NULL;
    snprintf(name, sizeof name, "%s signed psbt strict-parses", tag);
    spchk(name, wally_psbt_from_bytes(out1, w1, 0, &p) == WALLY_OK);
    if (p) {
        uint8_t sig[65];
        size_t siglen = 0;
        int have = sp_tap_key_sig(p, 0, sig, &siglen) == 0 && siglen == 64;
        snprintf(name, sizeof name, "%s carries a 64-byte taproot key sig", tag);
        spchk(name, have);
        // Golden vector: the exact Schnorr bytes the BIP340 reference signer
        // produced independently for this spend, plain aux_rand = 0 over the
        // embit-computed sighash. Any conforming BIP340 signer reaches the same
        // bytes, which is the point; a drift in KISS's nonce fails here.
        char sighex[130] = {0};
        if (have) {
            char *sh = NULL; wally_hex_from_bytes(sig, 64, &sh);
            if (sh) { snprintf(sighex, sizeof sighex, "%s", sh); wally_free_string(sh); }
        }
        snprintf(name, sizeof name, "%s signature is the golden byte string", tag);
        if (strcmp(sighex, golden) != 0)
            printf("  got  %s\n  want %s\n", sighex, golden);
        spchk(name, strcmp(sighex, golden) == 0);
        // the crux: our signature verifies under embit's output key AND embit's
        // sighash - so our tweak math and our BIP341 sighash both match embit
        snprintf(name, sizeof name, "%s sig verifies vs embit outkey+sighash", tag);
        spchk(name, have && sp_schnorr_verify(outkey, sighash, sig) == 0);
        wally_psbt_free(p);
    }

    // determinism: identical load+sign yields identical bytes
    rc = kiss_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    spchk(rc == 0 ? "spend re-load READY" : "spend re-load", rc == 0 && sum.status == WPSBT_READY);
    spchk("spend re-sign rc", kiss_psbt_sign(out2, sizeof out2, &w2) == 0);
    snprintf(name, sizeof name, "%s sign is deterministic", tag);
    spchk(name, w1 == w2 && memcmp(out1, out2, w1) == 0);
    kiss_psbt_free();
    kiss_set_network(0);
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

    kiss_set_network(1);
    int rc = kiss_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    spchk("explicit SIGHASH_ALL loads READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("explicit SIGHASH_ALL signs",
          kiss_psbt_sign(out, sizeof out, &w) == 0 && w > 0);
    kiss_psbt_free();

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
    kiss_set_network(0);
}

static void sp_test_spend(void) {
    sp_test_spend_one("spend even-Y", SPV_SPEND_EVEN_B64,
                      SPV_SPEND_EVEN_OUTKEY, SPV_SPEND_EVEN_SIGHASH,
                      SV_SCHNORR_SP_EVEN);
    sp_test_spend_one("spend odd-Y", SPV_SPEND_ODD_B64,
                      SPV_SPEND_ODD_OUTKEY, SPV_SPEND_ODD_SIGHASH,
                      SV_SCHNORR_SP_ODD);

    // foreign tweak: the PSBT's tweak does NOT reproduce the on-chain P2TR key.
    // The signer MUST refuse (BIP376 anti-theft), never emit a signature.
    wpsbt_summary_t sum;
    kiss_set_network(1);
    int rc = kiss_psbt_load((const uint8_t *)SPV_SPEND_FOREIGN_B64,
                              strlen(SPV_SPEND_FOREIGN_B64), &sum);
    spchk("foreign-tweak spend stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "not this wallet"));
    kiss_psbt_free();
    kiss_set_network(0);
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

// BIP374 0.3.0: e is int(hash) mod n, written reduced, and a proof with e >= n
// is refused. A real challenge hash reaches n about once in 2^128, so the hook
// stands in for it; prove and verify both read it, as they both read the hash.
static const uint8_t SPT_N[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
    0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
    0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41
};

// out = x + small for a 32 byte big endian x (no carry out of the top byte)
static void spt_add_small(uint8_t out[32], const uint8_t x[32], unsigned small)
{
    unsigned carry = small;
    for (int i = 31; i >= 0; i--) {
        unsigned v = x[i] + carry;
        out[i] = (uint8_t)v;
        carry = v >> 8;
    }
}

static void sp_test_dleq_challenge_bounds(const uint8_t a_sum[32],
                                          const uint8_t a_pub[33],
                                          const uint8_t share[33])
{
    const uint8_t *B = spv352_0_recipkeys;
    uint8_t aux[32] = { 0x42 };
    uint8_t proof[64], hash[32], want[32];
    struct { const char *name; unsigned over; int top; } cases[] = {
        { "hash = n", 0, 0 },          // reduces to e = 0, s = k
        { "hash = n + 5", 5, 0 },
        { "hash = 2^256 - 1", 0, 1 },  // the largest hash there is
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        char name[96];
        if (cases[c].top) {
            memset(hash, 0xff, 32);
            // 2^256 - 1 - n, the value it must reduce to
            int borrow = 0;
            for (int i = 31; i >= 0; i--) {
                int d = 0xff - (int)SPT_N[i] - borrow;
                borrow = d < 0;
                want[i] = (uint8_t)(d + (borrow ? 256 : 0));
            }
        } else {
            spt_add_small(hash, SPT_N, cases[c].over);
            memset(want, 0, 32);
            want[31] = (uint8_t)cases[c].over;
        }
        sp_test_challenge_hash = hash;
        int rc = sp_dleq_prove(a_sum, B, aux, NULL, NULL, proof);
        snprintf(name, sizeof name, "dleq %s: proof written with e reduced mod n",
                 cases[c].name);
        spchk(name, rc == 0 && memcmp(proof, want, 32) == 0);
        snprintf(name, sizeof name, "dleq %s: reduced proof verifies", cases[c].name);
        spchk(name, sp_dleq_verify(a_pub, B, share, proof, NULL, NULL) == 0);
        // The same proof with e left unreduced is what a pre-0.3.0 prover wrote.
        memcpy(proof, hash, 32);
        snprintf(name, sizeof name, "dleq %s: unreduced e refused", cases[c].name);
        spchk(name, sp_dleq_verify(a_pub, B, share, proof, NULL, NULL) != 0);
        sp_test_challenge_hash = NULL;
    }

    // s = k + e*a = 0, which BIP374 allows. The nonce k does not depend on
    // the challenge, so one proof under a pinned e = 1 gives k = s - a, and a
    // second under e = -k / a (here a = 1, so e = -k) lands s exactly on 0.
    {
        uint8_t one[32] = { 0 }, a1[32] = { 0 }, A1[33], C1[33], k[32], neg_a[32];
        one[31] = 1;
        a1[31] = 1;
        memcpy(A1, spvdg_8_G, 33);                  // A = 1*G = G (vector 8's G)
        int ok = sp_ecdh_share(a1, B, C1) == 0;     // C = 1*B = B
        sp_test_challenge_hash = one;
        ok = ok && sp_dleq_prove(a1, B, aux, NULL, NULL, proof) == 0;
        memcpy(k, proof + 32, 32);                  // s = k + 1
        memcpy(neg_a, a1, 32);
        ok = ok && secp256k1_ec_seckey_negate(secp256k1_context_static, neg_a) &&
             secp256k1_ec_seckey_tweak_add(secp256k1_context_static, k, neg_a) &&
             secp256k1_ec_seckey_negate(secp256k1_context_static, k);  // now -k
        sp_test_challenge_hash = k;
        int rc = ok ? sp_dleq_prove(a1, B, aux, NULL, NULL, proof) : -99;
        static const uint8_t zero[32] = { 0 };
        spchk("dleq s = 0 is generated, not refused",
              rc == 0 && memcmp(proof + 32, zero, 32) == 0);
        spchk("dleq s = 0 proof verifies",
              rc == 0 && sp_dleq_verify(A1, B, C1, proof, NULL, NULL) == 0);
        sp_test_challenge_hash = NULL;
    }

    // Without the hook: a genuine proof with e or s pushed to n or above.
    spchk("dleq genuine proof still verifies with the hook off",
          sp_dleq_prove(a_sum, B, aux, NULL, NULL, proof) == 0 &&
          sp_dleq_verify(a_pub, B, share, proof, NULL, NULL) == 0);
    uint8_t bad[64];
    memcpy(bad, proof, 64);
    memset(bad, 0xff, 32);
    spchk("dleq e = 2^256 - 1 refused",
          sp_dleq_verify(a_pub, B, share, bad, NULL, NULL) != 0);
    memcpy(bad, proof, 64);
    spt_add_small(bad + 32, SPT_N, 1);
    spchk("dleq s = n + 1 refused",
          sp_dleq_verify(a_pub, B, share, bad, NULL, NULL) != 0);
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
    DG(0); DG(1); DG(2); DG(3); DG(4); DG(5); DG(6); DG(7); DG(8); DG(9);
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
    DV(8); DV(9); DV(10); DV(11); DV(12); DV(13); DV(14); DV(15); DV(16);
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

    sp_test_dleq_challenge_bounds(a_sum, a_pub, share);
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

        // The label belongs to this output's 0x09 recipient metadata. Build a
        // raw malformed fixture by removing exactly that serialized field.
        // libwally itself refuses to serialize the orphan, so the byte edit is
        // deliberate: it tests hostile input rather than its builder API.
        static const uint8_t key_info[1] = { 0x09 };
        uint8_t info[66];
        size_t info_len = 0;
        int have_info = sp_output_unknown(p, 1, key_info, sizeof key_info,
                                          info, sizeof info, &info_len) == 0 &&
                        info_len == sizeof info;
        wally_psbt_free(p);

        uint8_t raw[4096];
        size_t raw_len = 0;
        int decoded = have_info &&
            wally_base64_to_bytes(SPARROW_SPEND_B64, 0, raw, sizeof raw,
                                  &raw_len) == WALLY_OK;
        size_t off = raw_len;
        for (size_t i = 0; decoded && i + 69 <= raw_len; i++)
            if (raw[i] == 0x01 && raw[i + 1] == 0x09 &&
                raw[i + 2] == 0x42 &&
                memcmp(raw + i + 3, info, sizeof info) == 0) {
                off = i;
                break;
            }
        int removed = off < raw_len;
        if (removed) {
            memmove(raw + off, raw + off + 69, raw_len - off - 69);
            raw_len -= 69;
        }

        wpsbt_summary_t orphan;
        kiss_set_network(1);
        int orphan_rc = removed
            ? kiss_psbt_load(raw, raw_len, &orphan) : -1;
        if (!(orphan_rc == 0 && orphan.status == WPSBT_STOP &&
              strstr(orphan.reason, "label without output info") != NULL))
            printf("  orphan probe: decoded=%d removed=%d len=%zu rc=%d "
                   "status=%d reason=%s\n", decoded, removed, raw_len,
                   orphan_rc, orphan_rc == 0 ? orphan.status : -1,
                   orphan_rc == 0 ? orphan.reason : "(load failed)");
        spchk("sparrow: orphan SP output label rejected",
              orphan_rc == 0 && orphan.status == WPSBT_STOP &&
              strstr(orphan.reason, "label without output info") != NULL);
        kiss_psbt_free();
    }

    wpsbt_summary_t sum;
    kiss_set_network(1);
    int rc = kiss_psbt_load((const uint8_t *)SPARROW_SPEND_B64,
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
    kiss_set_network(1);

    int rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_B64,
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
    kiss_psbt_free();

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
    rc = kiss_psbt_load(hybrid, sizeof hybrid, &sum);
    spchk("v2 + embedded global tx hybrid refused",
          !(rc == 0 && sum.status == WPSBT_READY));
    if (rc == 0) kiss_psbt_free();

    // coordinator-supplied per-input shares get wiped, load stays READY
    rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_FOREIGN_SHARE_B64,
                          strlen(SPV_PSBT_FOREIGN_SHARE_B64), &sum);
    spchk("foreign-share fixture still READY",
          rc == 0 && sum.status == WPSBT_READY && sum.n_sp == 1);
    kiss_psbt_free();

    // SP info inside a v0 PSBT -> STOP
    rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_V0_SP_B64,
                          strlen(SPV_PSBT_V0_SP_B64), &sum);
    spchk("v0 + SP info stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "PSBTv2"));
    kiss_psbt_free();

    // sighash SINGLE with an SP output -> STOP (existing ALL-only gate)
    rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_SIGHASH_B64,
                          strlen(SPV_PSBT_SIGHASH_B64), &sum);
    spchk("SP sighash SINGLE stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "sighash"));
    kiss_psbt_free();

    // A BIP376 SP tweak (0x20) on a NON-taproot input is malformed/hostile: SP
    // spend fields only apply to a received P2TR coin, so the signer must refuse
    // (this fixture bolts a tweak onto a P2WPKH input). Real P2TR BIP376 spends
    // are exercised by sp_test_spend against embit-built fixtures.
    rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_BIP376_B64,
                          strlen(SPV_PSBT_BIP376_B64), &sum);
    spchk("SP tweak on a non-taproot input stops",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "taproot"));
    kiss_psbt_free();

    // same fixture on MAINNET -> wrong-network STOP (input path is 84h/1h)
    kiss_set_network(0);
    rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_B64,
                          strlen(SPV_PSBT_B64), &sum);
    spchk("SP fixture on mainnet stops as wrong network",
          rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, "network"));
    kiss_psbt_free();
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
    kiss_set_network(1);

    int rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_B64,
                              strlen(SPV_PSBT_B64), &sum);
    spchk("sign: fixture loads READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("sign rc", kiss_psbt_sign(out1, sizeof out1, &w1) == 0 && w1 > 0);
    kiss_psbt_free();
    // The send's shares, proofs and SP output info are what the coordinator
    // checks the derived outputs with, so its QR copy is never trimmed.
    spchk("sign: QR copy is the whole PSBT",
          kiss_psbt_export_copy(out1, w1, out2, sizeof out2, &w2) == 0 &&
          w2 == w1 && memcmp(out1, out2, w1) == 0);
    w2 = 0;

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
    rc = kiss_psbt_load((const uint8_t *)SPV_PSBT_B64,
                          strlen(SPV_PSBT_B64), &sum);
    spchk("re-load READY", rc == 0 && sum.status == WPSBT_READY);
    spchk("re-sign rc", kiss_psbt_sign(out2, sizeof out2, &w2) == 0);
    spchk("sign is deterministic", w1 == w2 && memcmp(out1, out2, w1) == 0);
    kiss_psbt_free();

    kiss_set_network(0);
}

// A silent-payment input's amount is covered by the BIP341 sighash and by
// nothing else: it carries no keypath, so the load loop proves ownership from
// the tweak and takes the number from the witness_utxo. The details page used
// to re-derive that number instead of reading what load decided, and its rule
// was "a prev tx is here, so the amount is proven" -- a rule whose ONE
// justification is the txid check load runs, which the SP branch never reaches.
//
// So a coordinator could staple any transaction to an SP input and the one
// screen whose job is saying which amounts are proven would show its number
// under a green tick, disagreeing with the total the same load computed.
static void sp_test_spend_stapled_prevtx(void) {
    struct wally_psbt *p = NULL;
    struct wally_tx *fake = NULL;
    struct wally_tx_output *o = NULL;
    char *b64 = NULL;
    // the coin the fixture really spends: 100000 sats to this P2TR key
    const uint64_t real_sats = 100000;
    const uint8_t spk[34] = {
        0x51, 0x20, 0x83, 0x2e, 0xac, 0x66, 0xec, 0xbc, 0xfc, 0x00, 0x75, 0x8a,
        0x69, 0xb1, 0x7f, 0x25, 0xd4, 0x82, 0xe6, 0xc4, 0xff, 0x4a, 0x55, 0xb4,
        0xac, 0xfc, 0x83, 0x1c, 0x15, 0x8f, 0x90, 0xe5, 0x7a, 0x25 };
    const uint64_t lie_sats = 100000000000ULL;   // 1000 BTC, absurd on purpose
    uint8_t dummy[32];
    memset(dummy, 0x77, sizeof dummy);

    // A transaction that has nothing to do with this outpoint (the fixture
    // spends cd..cd:0), carrying the input's own scriptPubKey so that only the
    // NUMBER differs. An output-only tx will not serialize, hence the input.
    if (wally_tx_init_alloc(2, 0, 1, 1, &fake) != WALLY_OK ||
        wally_tx_add_raw_input(fake, dummy, sizeof dummy, 0, 0xffffffff,
                               NULL, 0, NULL, 0) != WALLY_OK ||
        wally_tx_output_init_alloc(lie_sats, spk, sizeof spk, &o) != WALLY_OK ||
        wally_tx_add_output(fake, o) != WALLY_OK ||
        wally_psbt_from_base64(SPV_SPEND_EVEN_B64, 0, &p) != WALLY_OK ||
        wally_psbt_set_input_utxo(p, 0, fake) != WALLY_OK ||
        wally_psbt_to_base64(p, 0, &b64) != WALLY_OK) {
        spchk("stapled-prevtx fixture builds", 0);
        goto done;
    }
    spchk("stapled-prevtx fixture builds", 1);

    wpsbt_summary_t sum;
    wpsbt_details_t det;
    kiss_set_network(1);
    int rc = kiss_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    // The stapled tx does not hash to the outpoint, so it proves nothing. Load
    // may refuse it or ignore it, but it must never become a number on screen.
    if (rc == 0 && sum.status == WPSBT_READY) {
        spchk("stapled prev tx is not counted in the total",
              sum.in_sats == real_sats);
        spchk("stapled prev tx leaves the input unproven",
              kiss_psbt_details(&det) == 0 && det.n_in == 1 &&
              !det.ins[0].proven && det.ins[0].sats == real_sats);
    } else {
        spchk("stapled prev tx is not counted in the total",
              rc == 0 && sum.status == WPSBT_STOP);
        spchk("stapled prev tx leaves the input unproven",
              kiss_psbt_details(&det) != 0);
    }
    kiss_psbt_free();
    kiss_set_network(0);

done:
    if (b64) wally_free_string(b64);
    if (p) wally_psbt_free(p);
    if (o) wally_tx_output_free(o);
    if (fake) wally_tx_free(fake);
}

// Walk one embit-style PSBT map: keypairs (varint keylen, key, varint vallen,
// value) until a zero keylen -- embit's fork writes no field-count varint.
// Reports the value offset of the first field with `want` as its one-byte key.
// Returns the offset just past the map's terminator, or 0 on a malformed map.
static size_t sp_map_end(const uint8_t *b, size_t n, size_t i, uint8_t want,
                         size_t *want_val_off)
{
    if (want_val_off) *want_val_off = 0;
    while (i < n) {
        size_t kl = b[i++];
        if (kl == 0) return i;
        if (i + kl > n) return 0;
        bool hit = kl == 1 && b[i] == want;
        i += kl;
        if (i + 1 > n) return 0;
        size_t vl = b[i++];
        if (i + vl > n) return 0;
        if (hit && want_val_off) *want_val_off = i;
        i += vl;
    }
    return 0;
}

// The unproven-inputs refusal must fire on a MIXED send: a BIP376 input
// (received silent-payment coin, amount covered by the BIP341 sighash) spent
// alongside our own P2WPKH input that carries only a witness_utxo (amount
// covered by BIP143 alone). A coordinator can run two signing sessions with
// different amounts on that P2WPKH input, combine the signatures, and
// broadcast a fee neither screen showed. The whole-transaction suppression
// (`!sp_send`) used to silence exactly this shape; the check now keys off
// the inputs themselves, and STOPs rather than warns (see kiss_psbt.c).
//
// The fixture is the pinned BIP376 spend (SPV_SPEND_EVEN_B64) with a second
// input map spliced in: embit-style PSBT maps have no field-count varint
// (key/value pairs until the 0x00 terminator), so inserting one requires
// locating the first input map's end. The P2WPKH input's keypath + script
// are re-derived from the session master, so ownership holds and only the
// unproven-amount caution can fire. The output amount is patched to keep the
// fee at 5000 (in 300000, out 295000).
static void sp_test_mixed_unproven(void) {
    const struct ext_key *master = kiss_session_master();
    uint8_t raw[4096], mixed[4096];
    size_t raw_len = 0;
    if (!master ||
        wally_base64_to_bytes(SPV_SPEND_EVEN_B64, 0, raw, sizeof raw, &raw_len) != WALLY_OK ||
        !raw_len) {
        spchk("mixed fixture decodes", 0);
        return;
    }
    size_t in_count_off = 0, ins_end = 0, amt_off = 0;
    size_t i = 5;                                     // "psbt\xff"
    i = sp_map_end(raw, raw_len, i, 0x04, &in_count_off);   // globals: INPUT_COUNT
    i = sp_map_end(raw, raw_len, i, 0xff, NULL);            // input 0 map
    ins_end = i;
    if (!i) {
        spchk("mixed fixture layout found", 0);
        return;
    }
    i = sp_map_end(raw, raw_len, i, 0x03, &amt_off);        // output map: AMOUNT
    if (!i || !in_count_off || !ins_end || !amt_off) {
        spchk("mixed fixture layout found", 0);
        return;
    }
    spchk("mixed fixture layout found", 1);

    // The second input's keypath + script, re-derived from the session master
    // (dev seed, testnet): m/84'/1'/0'/0/0.
    uint32_t path[5] = { 84 + BIP32_INITIAL_HARDENED_CHILD,
                         1 + BIP32_INITIAL_HARDENED_CHILD,
                         BIP32_INITIAL_HARDENED_CHILD, 0, 0 };
    struct ext_key k;
    struct ext_key m = *master;
    uint8_t fp[4];
    if (bip32_key_from_parent_path(master, path, 5, BIP32_FLAG_KEY_PRIVATE, &k) != WALLY_OK ||
        bip32_key_get_fingerprint(&m, fp, sizeof fp) != WALLY_OK) {
        spchk("mixed P2WPKH key derives", 0);
        return;
    }
    uint8_t scr[22];
    size_t scr_len = 0;
    if (wally_witness_program_from_bytes(k.pub_key, 33, WALLY_SCRIPT_HASH160,
                                         scr, sizeof scr, &scr_len) != WALLY_OK ||
        scr_len != 22) {
        wally_bzero(&k, sizeof k);
        spchk("mixed P2WPKH key derives", 0);
        return;
    }
    uint8_t extra[256];
    size_t x = 0;
    // witness_utxo: key 0x01, value = 8-byte value LE + varint scriptlen + script
    extra[x++] = 0x01; extra[x++] = 0x01;
    extra[x++] = (uint8_t)(8 + 1 + scr_len);
    for (int b = 0; b < 8; b++) extra[x++] = (uint8_t)(200000ULL >> (8 * b));
    extra[x++] = (uint8_t)scr_len;
    memcpy(extra + x, scr, scr_len); x += scr_len;
    // bip32_derivation: key 0x06||pubkey, value = fp || path (LE32 x5)
    extra[x++] = (uint8_t)(1 + 33);
    extra[x++] = 0x06;
    memcpy(extra + x, k.pub_key, 33); x += 33;
    extra[x++] = (uint8_t)(4 + 5 * 4);
    memcpy(extra + x, fp, 4); x += 4;
    for (int d = 0; d < 5; d++)
        for (int b = 0; b < 4; b++)
            extra[x++] = (uint8_t)(path[d] >> (8 * b));
    // prev txid / vout / sequence: the v2 fields every input must carry
    extra[x++] = 0x01; extra[x++] = 0x0e; extra[x++] = 32;
    memset(extra + x, 0xbe, 32); x += 32;
    extra[x++] = 0x01; extra[x++] = 0x0f; extra[x++] = 4;
    extra[x++] = 1; extra[x++] = 0; extra[x++] = 0; extra[x++] = 0;
    extra[x++] = 0x01; extra[x++] = 0x10; extra[x++] = 4;
    extra[x++] = 0xfe; extra[x++] = 0xff; extra[x++] = 0xff; extra[x++] = 0xff;
    extra[x++] = 0;                                    // map terminator
    wally_bzero(&k, sizeof k);

    // splice: globals + input 0 + new input + rest
    size_t n = 0;
    memcpy(mixed + n, raw, ins_end); n += ins_end;
    memcpy(mixed + n, extra, x); n += x;
    memcpy(mixed + n, raw + ins_end, raw_len - ins_end); n += raw_len - ins_end;
    mixed[in_count_off] = 2;                           // INPUT_COUNT: 1 -> 2
    // output AMOUNT: 95000 -> 295000 (in 300000, fee stays 5000). The offset
    // was measured in the unspliced buffer, so shift by the inserted map.
    for (int b = 0; b < 8; b++)
        mixed[amt_off + x + b] = (uint8_t)(295000ULL >> (8 * b));

    wpsbt_summary_t sum;
    uint8_t out1[4096];
    size_t w1 = 0;
    kiss_set_network(1);
    int rc = kiss_psbt_load(mixed, n, &sum);
    if (rc != 0) printf("  mixed load rc=%d\n", rc);
    else if (sum.status != WPSBT_STOP)
        printf("  mixed status=%d reason=%s\n", sum.status, sum.reason);
    spchk("mixed counts 1 BIP376 + 1 P2WPKH input",
          sum.n_in == 2 && sum.n_sp_in == 1);
    // Still summed, and still worth asserting: the STOP screen shows what it
    // refused, and the parse must reach the end to know the fee at all.
    spchk("mixed fee math", sum.in_sats == 300000 && sum.fee_sats == 5000 &&
          sum.send_sats + sum.change_sats == 295000);
    spchk("unproven-inputs refusal fires", sum.status == WPSBT_STOP &&
          strstr(sum.reason, "not proven") != NULL);
    // BOTH inputs count as unproven -- a BIP376 input carries a witness_utxo
    // and nothing else, by construction. What makes this transaction refuse is
    // not the count but the mix: BIP341 covers the SP input's amount, and the
    // P2WPKH input beside it is signed under BIP143, which does not.
    spchk("mixed counts both inputs unproven", sum.n_unproven_in == 2);
    spchk("mixed refuses to sign", kiss_psbt_sign(out1, sizeof out1, &w1) != 0);
    kiss_psbt_free();
    kiss_set_network(0);
}

// ---- a silent payment two signers pay together (BIP375 per input shares) ----
// Wallet A is the dev seed this suite already has open, wallet B the seed the
// other signer's bridge defaults to. Every expected value in
// sp_shared_vectors.h was computed by embit, and the output script there from
// the SUM OF THE PRIVATE KEYS: the route a single signer takes, where the code
// under test has to arrive at the same script from the sum of the shares.

static int sps_in_field(const struct wally_psbt *p, size_t i, uint8_t type,
                        const uint8_t scan[33], uint8_t *val, size_t want)
{
    uint8_t key[34];
    size_t item = 0;
    key[0] = type;
    memcpy(key + 1, scan, 33);
    if (i >= p->num_inputs ||
        wally_map_find(&p->inputs[i].unknowns, key, sizeof key, &item) != WALLY_OK ||
        !item || p->inputs[i].unknowns.items[item - 1].value_len != want)
        return -1;
    memcpy(val, p->inputs[i].unknowns.items[item - 1].value, want);
    return 0;
}

static bool sps_has_global_share(const struct wally_psbt *p)
{
    for (size_t j = 0; j < p->unknowns.num_items; j++) {
        const struct wally_map_item *it = &p->unknowns.items[j];
        if (it->key_len == 34 && (it->key[0] == 0x07 || it->key[0] == 0x08))
            return true;
    }
    return false;
}

static bool sps_has_sp_info(const struct wally_psbt *p, size_t j)
{
    static const uint8_t k09[1] = { 0x09 };
    uint8_t val[128];
    size_t vlen = 0;
    return sp_output_unknown(p, j, k09, 1, val, sizeof val, &vlen) == 0 && vlen == 66;
}

// Switch the session to another seed (NULL = back to the stored dev seed).
static int sps_open(const char *mnemonic)
{
    kiss_session_close();
    if (mnemonic && kiss_seed_open_temp(mnemonic) != 0)
        return -1;
    return kiss_session_open("");
}

// One pass of one signer: load, then share or sign by what load decided.
// Returns 0 with the PSBT that leaves in out; *sum is load's summary.
static int sps_pass(const uint8_t *in, size_t n, wpsbt_summary_t *sum,
                    uint8_t *out, size_t cap, size_t *w)
{
    *w = 0;
    int rc = kiss_psbt_load(in, n, sum);
    if (rc != 0)
        return -1;
    if (sum->status == WPSBT_STOP) {
        printf("  stop: %s\n", sum->reason);
        kiss_psbt_free();
        return -2;
    }
    rc = sum->share_only ? kiss_psbt_share(out, cap, w)
                         : kiss_psbt_sign(out, cap, w);
    kiss_psbt_free();
    return rc == 0 && *w ? 0 : -3;
}

static void sps_amounts(const char *tag, const wpsbt_summary_t *s,
                        uint64_t own_in, uint64_t own_change)
{
    char name[96];
    snprintf(name, sizeof name, "shared %s: fee is all inputs less all outputs", tag);
    spchk(name, s->in_sats == SPS_IN_A + SPS_IN_B && s->fee_sats == SPS_FEE);
    snprintf(name, sizeof name, "shared %s: own inputs and own change", tag);
    spchk(name, s->own_in_sats == own_in && s->change_sats == own_change &&
                s->n_foreign_in == 1);
    snprintf(name, sizeof name, "shared %s: the other change is an ordinary output", tag);
    spchk(name, s->send_sats == SPS_PAY + (SPS_IN_A + SPS_IN_B - SPS_FEE - SPS_PAY - own_change) &&
                s->n_out == 3 && s->outs[0].is_sp && s->n_sp == 1);
    snprintf(name, sizeof name, "shared %s: raised as a caution", tag);
    spchk(name, s->status == WPSBT_CAUTION && (s->caution_flags & WPSBT_C_SHARED));
}

// The share only pass, for each wallet: its own share and proof exactly as
// embit computes them, nothing signed, the script still absent.
static void sps_test_share_only(void)
{
    static uint8_t out[8192];
    size_t w = 0;
    wpsbt_summary_t sum;

    int rc = kiss_psbt_load((const uint8_t *)SPS_R0_B64, strlen(SPS_R0_B64), &sum);
    if (rc == 0 && sum.status == WPSBT_STOP) printf("  stop: %s\n", sum.reason);
    spchk("shared A first: loads as share only",
          rc == 0 && sum.status != WPSBT_STOP && sum.share_only);
    sps_amounts("share only", &sum, SPS_IN_A, SPS_CHANGE_A);
    spchk("shared share only: sign refuses", kiss_psbt_sign(out, sizeof out, &w) != 0);
    wpsbt_details_t det;
    spchk("shared share only: details page reads the PSBT fields",
          kiss_psbt_details(&det) == 0 && det.n_in == 2 && det.ins[1].foreign &&
          !det.ins[0].foreign && det.txid[0] == 0);
    rc = kiss_psbt_share(out, sizeof out, &w);
    spchk("shared share only: share rc", rc == 0 && w > 0);
    kiss_psbt_free();
    uint8_t cp[8192];
    size_t cw = 0;
    spchk("shared share only: the QR sends it whole",
          kiss_psbt_export_copy(out, w, cp, sizeof cp, &cw) != 0);

    struct wally_psbt *p = NULL;
    spchk("shared share only: what leaves parses",
          wally_psbt_from_bytes(out, w, WALLY_PSBT_PARSE_FLAG_LOOSE, &p) == WALLY_OK && p);
    if (p) {
        uint8_t share[33], proof[64];
        spchk("shared A share equals embit's",
              sps_in_field(p, 0, 0x1d, SPS_SCAN, share, 33) == 0 &&
              memcmp(share, SPS_A_SHARE, 33) == 0);
        spchk("shared A proof equals embit's BIP374 proof",
              sps_in_field(p, 0, 0x1e, SPS_SCAN, proof, 64) == 0 &&
              memcmp(proof, SPS_A_PROOF_R0, 64) == 0);
        spchk("shared A proof verifies against A_i",
              sp_dleq_verify(SPS_A_PUB, SPS_SCAN, share, proof, NULL, NULL) == 0);
        spchk("shared share only: B's input untouched",
              sps_in_field(p, 1, 0x1d, SPS_SCAN, share, 33) != 0 &&
              sps_in_field(p, 1, 0x1e, SPS_SCAN, proof, 64) != 0);
        spchk("shared share only: no script on the silent payment output",
              !p->outputs[0].script && p->outputs[0].script_len == 0);
        spchk("shared share only: SP info and the other scripts kept",
              sps_has_sp_info(p, 0) && p->outputs[1].script && p->outputs[2].script);
        spchk("shared share only: nothing signed",
              p->inputs[0].signatures.num_items == 0 &&
              p->inputs[1].signatures.num_items == 0);
        spchk("shared share only: no global share, flags 0",
              !sps_has_global_share(p) && p->tx_modifiable_flags == 0);
        wally_psbt_free(p);
    }

    spchk("shared: open wallet B", sps_open(SPS_MNEMONIC_B) == 0);
    rc = sps_pass((const uint8_t *)SPS_R0_B64, strlen(SPS_R0_B64), &sum,
                  out, sizeof out, &w);
    spchk("shared B first: share only pass", rc == 0 && sum.share_only);
    sps_amounts("B share only", &sum, SPS_IN_B, SPS_CHANGE_B);
    p = NULL;
    if (rc == 0 &&
        wally_psbt_from_bytes(out, w, WALLY_PSBT_PARSE_FLAG_LOOSE, &p) == WALLY_OK && p) {
        uint8_t share[33], proof[64];
        spchk("shared B share and proof equal embit's",
              sps_in_field(p, 1, 0x1d, SPS_SCAN, share, 33) == 0 &&
              memcmp(share, SPS_B_SHARE, 33) == 0 &&
              sps_in_field(p, 1, 0x1e, SPS_SCAN, proof, 64) == 0 &&
              memcmp(proof, SPS_B_PROOF_R0, 64) == 0);
        wally_psbt_free(p);
    }
    spchk("shared: back to wallet A", sps_open(NULL) == 0);
}

// The complete pass on embit's own request (B's share already there): the
// script derived from the share sum, A's input signed, B's left alone.
static void sps_test_complete_fixture(void)
{
    static uint8_t out[8192];
    size_t w = 0;
    wpsbt_summary_t sum;
    int rc = kiss_psbt_load((const uint8_t *)SPS_RB_B64, strlen(SPS_RB_B64), &sum);
    if (rc == 0 && sum.status == WPSBT_STOP) printf("  stop: %s\n", sum.reason);
    spchk("shared complete: loads, not share only",
          rc == 0 && sum.status != WPSBT_STOP && !sum.share_only);
    sps_amounts("complete", &sum, SPS_IN_A, SPS_CHANGE_A);
    wpsbt_details_t det;
    spchk("shared complete: details has a txid and marks the foreign input",
          kiss_psbt_details(&det) == 0 && det.txid[0] != 0 && det.ins[1].foreign);
    spchk("shared complete: share refuses", kiss_psbt_share(out, sizeof out, &w) != 0);
    rc = kiss_psbt_sign(out, sizeof out, &w);
    spchk("shared complete: sign rc", rc == 0 && w > 0);
    kiss_psbt_free();
    uint8_t cp[8192];
    size_t cw = 0;
    spchk("shared complete: the QR copy is the whole PSBT",
          kiss_psbt_export_copy(out, w, cp, sizeof cp, &cw) == 0 &&
          cw == w && memcmp(cp, out, w) == 0);
    struct wally_psbt *p = NULL;
    spchk("shared complete: strict parses",
          wally_psbt_from_bytes(out, w, 0, &p) == WALLY_OK && p);
    if (!p) return;
    uint8_t share[33], proof[64];
    spchk("shared complete: script equals embit's from the private key sum",
          p->outputs[0].script_len == 34 &&
          memcmp(p->outputs[0].script, SPS_SCRIPT, 34) == 0);
    spchk("shared complete: A share and proof equal embit's",
          sps_in_field(p, 0, 0x1d, SPS_SCAN, share, 33) == 0 &&
          memcmp(share, SPS_A_SHARE, 33) == 0 &&
          sps_in_field(p, 0, 0x1e, SPS_SCAN, proof, 64) == 0 &&
          memcmp(proof, SPS_A_PROOF_RB, 64) == 0);
    spchk("shared complete: B's share kept as it came",
          sps_in_field(p, 1, 0x1d, SPS_SCAN, share, 33) == 0 &&
          memcmp(share, SPS_B_SHARE, 33) == 0);
    spchk("shared complete: own input signed, the other not",
          p->inputs[0].signatures.num_items == 1 &&
          p->inputs[1].signatures.num_items == 0);
    spchk("shared complete: no global share, SP info kept, flags 0",
          !sps_has_global_share(p) && sps_has_sp_info(p, 0) &&
          p->tx_modifiable_flags == 0);
    wally_psbt_free(p);
}

// Both visit orders, end to end with two seeds and nothing from embit in
// between: each signer's output is the next one's input.
static void sps_test_chain(bool b_first)
{
    static uint8_t s1[8192], s2[8192], s3[8192];
    size_t w1 = 0, w2 = 0, w3 = 0;
    wpsbt_summary_t sum;
    const char *tag = b_first ? "B, A, B" : "A, B, A";
    char name[96];
    const char *first = b_first ? SPS_MNEMONIC_B : NULL;
    const char *second = b_first ? NULL : SPS_MNEMONIC_B;

    int ok = sps_open(first) == 0 &&
             sps_pass((const uint8_t *)SPS_R0_B64, strlen(SPS_R0_B64), &sum,
                      s1, sizeof s1, &w1) == 0 && sum.share_only;
    snprintf(name, sizeof name, "shared %s: first pass is share only", tag);
    spchk(name, ok);
    ok = ok && sps_open(second) == 0 &&
         sps_pass(s1, w1, &sum, s2, sizeof s2, &w2) == 0 && !sum.share_only;
    snprintf(name, sizeof name, "shared %s: second pass derives and signs", tag);
    spchk(name, ok);
    ok = ok && sps_open(first) == 0 &&
         sps_pass(s2, w2, &sum, s3, sizeof s3, &w3) == 0 && !sum.share_only;
    snprintf(name, sizeof name, "shared %s: third pass verifies and signs", tag);
    spchk(name, ok);
    sps_open(NULL);
    struct wally_psbt *p = NULL;
    if (!ok || wally_psbt_from_bytes(s3, w3, 0, &p) != WALLY_OK || !p) {
        snprintf(name, sizeof name, "shared %s: final PSBT parses", tag);
        spchk(name, 0);
        return;
    }
    uint8_t share[33];
    snprintf(name, sizeof name, "shared %s: script equals embit's", tag);
    spchk(name, p->outputs[0].script_len == 34 &&
                memcmp(p->outputs[0].script, SPS_SCRIPT, 34) == 0);
    snprintf(name, sizeof name, "shared %s: both shares equal embit's", tag);
    spchk(name, sps_in_field(p, 0, 0x1d, SPS_SCAN, share, 33) == 0 &&
                memcmp(share, SPS_A_SHARE, 33) == 0 &&
                sps_in_field(p, 1, 0x1d, SPS_SCAN, share, 33) == 0 &&
                memcmp(share, SPS_B_SHARE, 33) == 0);
    snprintf(name, sizeof name, "shared %s: both inputs signed once", tag);
    spchk(name, p->inputs[0].signatures.num_items == 1 &&
                p->inputs[1].signatures.num_items == 1);
    snprintf(name, sizeof name, "shared %s: finalizes", tag);
    spchk(name, wally_psbt_finalize(p, 0) == WALLY_OK &&
                wally_psbt_is_finalized(p, &(size_t){0}) == WALLY_OK);
    wally_psbt_free(p);
}

static void sps_refuse(const char *name, const char *b64, const char *why)
{
    wpsbt_summary_t sum;
    uint8_t out[8192];
    size_t w = 0;
    int rc = kiss_psbt_load((const uint8_t *)b64, strlen(b64), &sum);
    int ok = rc == 0 && sum.status == WPSBT_STOP && strstr(sum.reason, why) != NULL;
    if (!ok) printf("  rc=%d status=%d reason=%s\n", rc, sum.status, sum.reason);
    spchk(name, ok);
    if (rc == 0) {
        char n2[128];
        snprintf(n2, sizeof n2, "%s: nothing leaves", name);
        spchk(n2, kiss_psbt_sign(out, sizeof out, &w) != 0 &&
                  kiss_psbt_share(out, sizeof out, &w) != 0);
        kiss_psbt_free();
    }
}

// The complete shared send with the foreign signer's proof challenge set to
// e32, through the same load and sign calls a scanned PSBT goes through.
// BIP374 0.3.0: an e of n or more is refused, never reduced. The bytes are
// patched in place: libwally will not reserialize a PSBT whose silent payment
// output has no script yet, which is exactly the state this one is in.
static void sps_refuse_foreign_e(const char *name, const uint8_t e32[32])
{
    uint8_t raw[4096], key[36];
    size_t len = 0, at = 0;
    char *b64 = NULL;
    key[0] = 34;                       // compact size of the key
    key[1] = 0x1e;                     // PSBT_IN_SP_DLEQ
    memcpy(key + 2, SPS_SCAN, 33);
    key[35] = 64;                      // compact size of the value
    int ok = wally_base64_to_bytes(SPS_RB_B64, 0, raw, sizeof raw, &len) == WALLY_OK;
    int found = 0;
    for (size_t i = 0; ok && i + sizeof key + 64 <= len; i++)
        if (memcmp(raw + i, key, sizeof key) == 0) { at = i + sizeof key; found++; }
    ok = ok && found == 1;
    if (ok) {
        memcpy(raw + at, e32, 32);
        ok = wally_base64_from_bytes(raw, len, 0, &b64) == WALLY_OK;
    }
    if (!ok) {
        spchk(name, 0);
        return;
    }
    sps_refuse(name, b64, "does not verify");
    wally_free_string(b64);
}

static void sps_test_refusals(void)
{
    static const uint8_t N[32] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
        0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
        0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41
    };
    uint8_t top[32];
    memset(top, 0xff, sizeof top);
    sps_refuse_foreign_e("shared refuses a foreign proof with e = n", N);
    sps_refuse_foreign_e("shared refuses a foreign proof with e = 2^256 - 1", top);
    sps_refuse("shared refuses a bad foreign proof", SPS_N_BADPROOF_B64, "does not verify");
    sps_refuse("shared refuses a proof made with another key", SPS_N_WRONGKEY_B64, "does not verify");
    sps_refuse("shared refuses a share without proof", SPS_N_NOPROOF_B64, "no proof");
    sps_refuse("shared refuses a proof without share", SPS_N_NOSHARE_B64, "no proof");
    sps_refuse("shared refuses a script before all shares", SPS_N_EARLYSCRIPT_B64, "before all shares");
    sps_refuse("shared refuses a wrong script", SPS_N_WRONGSCRIPT_B64, "does not match");
    sps_refuse("shared refuses a global share", SPS_N_GLOBAL_B64, "global share");
    sps_refuse("shared refuses an ineligible foreign input", SPS_N_INELIGIBLE_B64, "P2WPKH or P2TR");
    sps_refuse("shared refuses a foreign key that does not hash", SPS_N_NOKEY_B64, "key does not match");
    sps_refuse("shared refuses an unproven foreign amount", SPS_N_UNPROVEN_B64, "not proven");
    sps_refuse("shared refuses a foreign prev tx mismatch", SPS_N_PREVMISMATCH_B64, "previous transaction");
    sps_refuse("shared refuses a foreign sighash", SPS_N_SIGHASH_B64, "sighash");
    sps_refuse("shared refuses a foreign odd Y share", SPS_N_FTR_ODD_B64, "does not verify");
    // no silent payment output: exactly the refusal it always was
    sps_refuse("foreign input without SP output still refused", SPS_N_NOSP_B64,
               "not this wallet");
}

// A complete pass that must reach embit's script, with what it checks.
static void sps_complete(const char *tag, const char *b64, size_t out_idx,
                         const uint8_t script[34], const uint8_t *a_share)
{
    static uint8_t out[8192];
    size_t w = 0;
    wpsbt_summary_t sum;
    char name[128];
    int rc = sps_pass((const uint8_t *)b64, strlen(b64), &sum, out, sizeof out, &w);
    snprintf(name, sizeof name, "shared %s: complete and signed", tag);
    spchk(name, rc == 0 && !sum.share_only);
    struct wally_psbt *p = NULL;
    if (rc != 0 || wally_psbt_from_bytes(out, w, 0, &p) != WALLY_OK || !p)
        return;
    snprintf(name, sizeof name, "shared %s: script equals embit's", tag);
    spchk(name, p->outputs[out_idx].script_len == 34 &&
                memcmp(p->outputs[out_idx].script, script, 34) == 0);
    if (a_share) {
        uint8_t share[33];
        snprintf(name, sizeof name, "shared %s: own share equals embit's", tag);
        spchk(name, sps_in_field(p, 0, 0x1d, SPS_SCAN, share, 33) == 0 &&
                    memcmp(share, a_share, 33) == 0);
    }
    snprintf(name, sizeof name, "shared %s: only the own input signed", tag);
    spchk(name, (p->inputs[0].signatures.num_items == 1 ||
                 wally_map_get_integer(&p->inputs[0].psbt_fields, 0x13)) &&
                p->inputs[1].signatures.num_items == 0 &&
                !wally_map_get_integer(&p->inputs[1].psbt_fields, 0x13));
    wally_psbt_free(p);
}

static void sps_test_variants(void)
{
    // a share planted on our own input is wiped and recomputed
    sps_complete("planted share on own input", SPS_PLANTED_B64, 0, SPS_SCRIPT, SPS_A_SHARE);
    sps_complete("foreign P2TR, odd Y", SPS_FTR_B64, 0, SPS_FTR_SCRIPT, SPS_A_SHARE);
    sps_complete("own BIP376 input, foreign P2TR", SPS_OTR_B64, 0, SPS_OTR_SCRIPT,
                 SPS_OTR_A_SHARE);
    sps_complete("two scan keys", SPS_TWO_B64, 3, SPS_TWO_SCRIPT3, NULL);

    wpsbt_summary_t sum;
    int rc = kiss_psbt_load((const uint8_t *)SPS_TWO_HALF_B64,
                            strlen(SPS_TWO_HALF_B64), &sum);
    spchk("shared two scan keys, one share missing: share only",
          rc == 0 && sum.status != WPSBT_STOP && sum.share_only);
    if (rc == 0) kiss_psbt_free();
}

// The arithmetic on a third implementation's numbers: the BIP375 vector with
// per input shares derives its script from those shares alone.
static void sps_test_bip375(void)
{
#ifdef SPS_HAVE_BIP375
    uint8_t csum[33], asum[33], ih[32];
    bool cinf = true, ainf = true;
    int ok = 1;
    for (int i = 0; i < SPS375_NIN; i++) {
        ok &= sp_dleq_verify(sps375_pubs + 33 * i, sps375_scan, sps375_shares + 33 * i,
                             sps375_proofs + 64 * i, NULL, NULL) == 0;
        ok &= sp_point_sum(csum, &cinf, sps375_shares + 33 * i) == 0;
        ok &= sp_point_sum(asum, &ainf, sps375_pubs + 33 * i) == 0;
    }
    spchk("BIP375 per input proofs verify", ok);
    sp_recip_t r;
    memcpy(r.scan, sps375_scan, 33);
    memcpy(r.spend, sps375_spend, 33);
    ok = ok && !cinf && !ainf &&
         sp_input_hash(sps375_outpoints, SPS375_NIN, asum, ih) == 0 &&
         sp_derive_group(csum, ih, &r, 1) == 0 &&
         memcmp(r.xonly_out, sps375_script + 2, 32) == 0;
    spchk("BIP375 script from the sum of per input shares", ok);
    spchk("BIP375 invalid per input proof refused",
          sp_dleq_verify(sps375_bad_pub, sps375_scan, sps375_bad_share,
                         sps375_bad_proof, NULL, NULL) != 0);
#endif
}

// A point and its negation sum to infinity, and a third point brings it back.
static void sps_test_point_sum(void)
{
    uint8_t neg[33], sum[33];
    bool inf = true;
    memcpy(neg, SPS_A_PUB, 33);
    neg[0] ^= 1;                                   // same x, other Y = -A
    int ok = sp_point_sum(sum, &inf, SPS_A_PUB) == 0 && !inf &&
             sp_point_sum(sum, &inf, neg) == 0 && inf &&
             sp_point_sum(sum, &inf, SPS_B_PUB) == 0 && !inf &&
             memcmp(sum, SPS_B_PUB, 33) == 0;
    spchk("point sum carries the point at infinity", ok);
    static const uint8_t junk[33] = { 0x05 };
    spchk("point sum refuses a non point", sp_point_sum(sum, &inf, junk) != 0);
}

static void sp_test_shared(void)
{
    kiss_set_network(1);
    uint8_t fp[4];
    sps_test_point_sum();
    sps_test_bip375();
    sps_test_share_only();
    sps_test_complete_fixture();
    sps_test_chain(true);
    sps_test_chain(false);
    sps_test_refusals();
    sps_test_variants();
    spchk("shared: dev wallet open again",
          kiss_session_fingerprint(fp) == 0 && fp[0] == 0x73 && fp[1] == 0xc5);
    kiss_set_network(0);
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
    sp_test_spend_stapled_prevtx();
    sp_test_mixed_unproven();
    sp_test_shared();
    return sp_fails;
}
