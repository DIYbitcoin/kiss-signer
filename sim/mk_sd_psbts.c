// Emit TESTNET typed PSBT fixtures for on-device SD-sign testing.
// Build+run: sim/mk_sd_psbts.sh <out-dir>   (out-dir = the SD card root)
//
// Derives from the standard BIP39 dev seed (abandon x11 + about, no passphrase,
// master fingerprint 73C5DA0A) exactly as the device does. Each file is a
// 1-in (ours) / 2-out (60k external + 39k change) PSBT at coin type 1h for the
// given script type. The prevout is synthetic, so a SIGNED file is NOT
// broadcastable — this exercises the device's verify/sign UI + parser, not the
// chain. (Real broadcastable roundtrip = Sparrow, separately.)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <wally_core.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>

#include "wallet_crypto.h"   // WSCRIPT_* enum

#define H 0x80000000u

static struct ext_key t_master;
static uint8_t t_fp[4];

static int setup_master(void) {
    uint8_t seed[BIP39_SEED_LEN_512]; size_t sl = 0;
    if (bip39_mnemonic_to_seed(
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about",
            NULL, seed, sizeof seed, &sl) != WALLY_OK) return 1;
    if (bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0, &t_master) != WALLY_OK) return 2;
    if (bip32_key_get_fingerprint(&t_master, t_fp, sizeof t_fp) != WALLY_OK) return 3;
    return 0;
}

static void derive5(uint32_t purpose, uint32_t coin, uint32_t chg, uint32_t idx, struct ext_key *o) {
    const uint32_t p[5] = {H + purpose, H + coin, H, chg, idx};
    bip32_key_from_parent_path(&t_master, p, 5, BIP32_FLAG_KEY_PRIVATE, o);
}

static void build_spk(int script, const uint8_t pub[33], uint8_t *out, size_t *len) {
    size_t w = 0;
    if (script == WSCRIPT_LEGACY) {
        wally_scriptpubkey_p2pkh_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, out, 25, len);
    } else if (script == WSCRIPT_NESTED) {
        uint8_t r[22];
        wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, r, 22, &w);
        wally_scriptpubkey_p2sh_from_bytes(r, 22, WALLY_SCRIPT_HASH160, out, 23, len);
    } else {
        wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, out, 22, len);
    }
}

// 1-in / 2-out PSBT for the given script type at the given coin type. The
// input is always 100000 sats, so send + change decides the fee, which is
// how the caution fixture is built without a second code path.
static size_t mk_typed_psbt(int script, uint32_t purpose, uint32_t coin,
                            uint64_t send, uint64_t change,
                            uint8_t *out, size_t cap) {
    struct ext_key kin, kchg;
    derive5(purpose, coin, 0, 0, &kin);
    derive5(purpose, coin, 1, 0, &kchg);
    uint8_t in_spk[25], chg_spk[25];
    size_t in_len = 0, chg_len = 0;
    build_spk(script, kin.pub_key, in_spk, &in_len);
    build_spk(script, kchg.pub_key, chg_spk, &chg_len);
    uint8_t ext_spk[22] = {0x00, 0x14};
    memset(ext_spk + 2, 0x11, 20);

    struct wally_tx *prev = NULL;
    uint8_t txid[32];
    if (script == WSCRIPT_LEGACY) {          // legacy needs the real prev-tx txid
        wally_tx_init_alloc(2, 0, 1, 1, &prev);
        uint8_t dt[32]; memset(dt, 0xBB, 32);
        wally_tx_add_raw_input(prev, dt, 32, 0, 0xFFFFFFFF, NULL, 0, NULL, 0);
        wally_tx_add_raw_output(prev, 100000, in_spk, in_len, 0);
        wally_tx_get_txid(prev, txid, 32);
    } else {
        memset(txid, 0xAA, 32);
    }

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 1, 2, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, send, ext_spk, 22, 0);
    wally_tx_add_raw_output(tx, change, chg_spk, chg_len, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 1, 2, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);

    if (script == WSCRIPT_LEGACY) {
        wally_psbt_set_input_utxo(p, 0, prev);
        wally_tx_free(prev);
    } else {
        struct wally_tx_output *u = NULL;
        wally_tx_output_init_alloc(100000, in_spk, in_len, &u);
        wally_psbt_set_input_witness_utxo(p, 0, u);
        wally_tx_output_free(u);
        if (script == WSCRIPT_NESTED) {      // redeem script needed to sign p2sh-p2wpkh
            uint8_t redeem[22]; size_t w = 0;
            wally_witness_program_from_bytes(kin.pub_key, 33, WALLY_SCRIPT_HASH160,
                                             redeem, 22, &w);
            wally_psbt_set_input_redeem_script(p, 0, redeem, 22);
        }
    }

    const uint32_t pin[5]  = {H + purpose, H + coin, H, 0, 0};
    const uint32_t pchg[5] = {H + purpose, H + coin, H, 1, 0};
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kin.pub_key, 33, t_fp, 4, pin, 5);
    wally_psbt_set_input_keypaths(p, 0, m);
    wally_map_free(m); m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kchg.pub_key, 33, t_fp, 4, pchg, 5);
    wally_psbt_set_output_keypaths(p, 1, m);
    wally_map_free(m);

    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, cap, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    wally_bzero(&kin, sizeof kin);
    wally_bzero(&kchg, sizeof kchg);
    return wr;
}

static int emit(const char *dir, const char *name, int script, uint32_t purpose,
                uint32_t coin, uint64_t send, uint64_t change) {
    uint8_t buf[4096];
    size_t n = mk_typed_psbt(script, purpose, coin, send, change, buf, sizeof buf);
    if (n == 0) { fprintf(stderr, "FAIL build %s\n", name); return 1; }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "FAIL open %s\n", path); return 1; }
    fwrite(buf, 1, n, f);
    fclose(f);
    printf("wrote %-22s %4zu bytes  (coin %uh, purpose %uh)\n", name, n, coin, purpose);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <out-dir>\n", argv[0]); return 2; }
    wally_init(0);
    if (setup_master() != 0) { fprintf(stderr, "seed setup failed\n"); return 1; }
    printf("dev seed fingerprint %02X%02X%02X%02X (want 73C5DA0A)\n",
           t_fp[0], t_fp[1], t_fp[2], t_fp[3]);
    if (!(t_fp[0]==0x73 && t_fp[1]==0xC5 && t_fp[2]==0xDA && t_fp[3]==0x0A)) {
        fprintf(stderr, "FINGERPRINT MISMATCH — refusing to write\n"); return 1;
    }
    const char *d = argv[1];
    int rc = 0;
    rc |= emit(d, "1-native.psbt",       WSCRIPT_NATIVE, 84, 1, 60000, 39000);  // type = NATIVE
    rc |= emit(d, "2-nested.psbt",       WSCRIPT_NESTED, 49, 1, 60000, 39000);  // type = NESTED
    rc |= emit(d, "3-legacy.psbt",       WSCRIPT_LEGACY, 44, 1, 60000, 39000);  // type = LEGACY
    rc |= emit(d, "4-stop-wrongnet.psbt", WSCRIPT_NATIVE, 84, 0, 60000, 39000); // mainnet coin -> STOP
    // The CAUTION case, which had no fixture at all: three READY and one STOP
    // meant the acknowledgement gate, the one screen on this device with a
    // second confirm in front of it, could not be reached on hardware without
    // hand building a PSBT. wallet_psbt.c raises WPSBT_C_HIGHFEE when the fee
    // is a tenth of the send or more; 8000 out of a 100000 input leaves 10000
    // of fee against an 8000 send, which is over that line and nowhere near
    // the dust rules, so exactly one flag fires and the screen is predictable.
    rc |= emit(d, "5-caution-highfee.psbt", WSCRIPT_NATIVE, 84, 1, 8000, 82000);
    wally_cleanup(0);
    return rc;
}
