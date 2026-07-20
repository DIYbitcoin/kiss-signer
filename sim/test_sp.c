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

int test_sp(void) {
    sp_fails = 0;
    sp_probe_libwally();
    sp_test_address();
    return sp_fails;
}
