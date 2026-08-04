// KISS Signer crypto layer (libwally). Step 1: prove the crypto stack.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Runs the BIP39/BIP32 test vector (standard "abandon ... about" dev mnemonic,
// empty passphrase) through libwally and checks the master fingerprint.
// Returns 0 on pass; nonzero identifies the failing stage.
// out_fingerprint (optional, 4 bytes) receives the computed fingerprint.
int wallet_selftest(uint8_t out_fingerprint[4]);

// Master fingerprint of the DEV seed with the given BIP39 passphrase
// (NULL/empty = no passphrase). Returns 0 on success.
// The fruitsim build provides a fake stub (no libwally on the sim).
int wallet_fingerprint(const char *passphrase, uint8_t out_fingerprint[4]);

// ---- seed entropy mixing ----
// out = SHA256(a || b): combine two independent 32-byte entropy sources so
// neither can weaken the seed below the strength of the other. Used at seed
// creation: a = SHA256 of the camera frame, b = hardware TRNG bytes.
int wallet_entropy_mix(const uint8_t a[32], const uint8_t b[32], uint8_t out[32]);

// Flat three-input fold: out = SHA256(a || b || c). Camera, chip TRNG, taps —
// in that order, matching the source numbers the setup screens show. Flat
// rather than nested mix() calls so the one question that matters (what went
// into this seed) is answerable by reading one line.
int wallet_entropy_mix3(const uint8_t a[32], const uint8_t b[32],
                        const uint8_t c[32], uint8_t out[32]);

// ---- hardware entropy source ----
// The chip's RNG only emits TRUE random numbers while a physical noise source
// is feeding it, and on this board none of the ways that happens by accident
// apply. There is no radio to switch one on (enforced at link time), the P4
// revision this firmware targets predates the dedicated TRNG block, and the
// second stage bootloader turns the SAR ADC source OFF on its way out —
// "Disabling RNG early entropy source" is the last thing it logs. Espressif's
// own word for what esp_random returns in that state is "pseudo-random".
//
// It still returns bytes. It still reports success. That is the whole hazard:
// it is the shape of bug that reached Coldcard through random.bytes(), where
// the call ran, the numbers looked fine, and the source behind them was not
// the one anybody believed it was. No statistical check finds this, because a
// decent PRNG passes them all. Only provenance answers it.
//
// So the app switches the source on itself, once, and leaves it on. It clashes
// with only the ADC and the radio, and this firmware uses neither.
void wallet_trng_start(void);

// False until wallet_trng_start has run. Key material with no second source
// folded into it MUST refuse to be generated while this is false, rather than
// quietly handing back a weak key that every later check will call valid.
bool wallet_trng_live(void);

// ---- timing jitter ----
// A second source for the key that cannot have the camera or the taps folded
// into it. The seed gets three sources because the owner is standing there
// supplying two of them; the SD device key is minted the first time a card is
// written, long after those screens wiped their material, so at that moment
// the chip is the only thing in the room.
//
// This reads the CPU cycle counter either side of a system timer read, a few
// hundred times over. The two clocks are not driven from the same place, so
// the width of that gap moves with cache, bus contention and interrupt
// arrival rather than with anything the caller sets. It is the one physical
// source on this board that does not run through the circuit esp_random does,
// which is the whole reason it is worth folding: the failure being defended
// against is that circuit being off, and a source sharing it would go down
// with it.
//
// No bit count is claimed, and none should be. Measuring jitter honestly means
// measuring it on the hardware in hand, and a number printed next to it would
// be exactly the unprovable quality score the tap screen refuses to show. It
// is folded, not trusted: SHA256(chip || jitter) cannot be weaker than the
// chip alone, and it stops a chip returning constants from picking the key by
// itself. Returns 0 on success.
int wallet_jitter(uint8_t out[32]);

// ---- network (mainnet / testnet) ----
// Affects derivation coin type (84h/0h vs 84h/1h), address hrp (bc/tb) and the
// descriptor xpub/tpub serialization. The master key itself is network-free,
// so this can be flipped any time — no session re-open needed. Default mainnet.
void wallet_set_network(int testnet);
int wallet_testnet(void);

// ---- address script type ----
// Native segwit (bc1..., BIP84) is the default. Nested segwit (3..., BIP49)
// and legacy (1..., BIP44) exist for older exchanges/wallets. Each is a
// separate account (different derivation purpose), so switching gives a
// different set of addresses from the same seed + passphrase.
enum { WSCRIPT_NATIVE = 0, WSCRIPT_NESTED = 1, WSCRIPT_LEGACY = 2 };
void wallet_set_script(int script);
int wallet_script(void);

// ---- step 4: wallet session ----
// The passphrase itself is wiped at login (deniability); what survives, in RAM
// only, is the derived master key — opened at unlock, wiped again at wallet lock.
// All of these return 0 on success; the sim build stubs them (no libwally).
#include <stddef.h>
int wallet_session_open(const char *passphrase);
// Transactional replacement: derive beside the current session, then publish
// only after wallet_seed_commit succeeds.
int wallet_session_prepare(const char *passphrase);
int wallet_session_activate_prepared(void);
void wallet_session_discard_prepared(void);
void wallet_session_close(void);
// 1 while the OPEN session is the one an empty passphrase derives -- the decoy
// signer. Screens that would reveal a second signer exists (the unlock-stroke
// settings) must be absent, not disabled, whenever this is true.
int wallet_session_decoy(void);
// BIP84 mainnet address at m/84h/0h/0h/<change>/<index> (native segwit, bc1q...)
int wallet_session_address(int change, uint32_t index, char *out, size_t out_len);

enum {
    WADDR_INVALID = 0,
    WADDR_CURRENT_NETWORK = 1,
    WADDR_WRONG_NETWORK = 2,
};
// Syntax/checksum + network validation for standard Bitcoin and BIP352
// addresses. It does not answer ownership; Receive > Verify does that by
// deriving and comparing this wallet's addresses.
int wallet_address_validate(const char *addr);

// BIP352 silent-payment receive address (sp1/tsp1) for the current network.
// Static/reusable by design. Returns 0 on success.
int wallet_session_sp_address(char *out, size_t out_len);
// Watch-only SCAN-KEY export: "sp([<fp>/352h/<coin>h/0h]spscan1<...>)" (BIP-392).
// Exposes the scan PRIVATE key so a coordinator can detect payments to this
// wallet's silent-payment address; it still cannot spend. Returns 0 on success.
int wallet_session_sp_scan_export(char *out, size_t out_len);
// Watch-only export: "wpkh([<fp>/84h/0h/0h]<xpub>/<0;1>/*)" for Sparrow etc.
int wallet_session_descriptor(char *out, size_t out_len);
// BlueWallet-flavored export: "[<fp>/84'/0'/0']<zpub>" — key origin + SLIP-132
// prefix (zpub/ypub per script type; vpub/upub on testnet; legacy stays xpub).
// BlueWallet doesn't read descriptor strings; it reads exactly this.
int wallet_session_bw_export(char *out, size_t out_len);

// INTERNAL — signing module (wallet_psbt.c) only. The session's bip32 master,
// or NULL when locked. Never expose beyond the crypto layer.
struct ext_key;
const struct ext_key *wallet_session_master(void);
