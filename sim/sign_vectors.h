// GENERATED, in two halves, by tools/sign_fixtures/gen_sign_vectors.py (the
// SV_ECDSA_* block) and gen_sp_sign_vectors.py (the SV_SCHNORR_SP_* block).
// Do not hand-edit, and do not redirect either generator over this file --
// neither prints the other's half.
//
// Golden signatures for the verifiable-determinism suite. Each is the exact
// byte string (DER signature + trailing sighash byte, lower-case hex) that an
// INDEPENDENT signer -- embit's low-R ECDSA, which grinds the RFC6979 nonce the
// same way libwally's EC_FLAG_GRIND_R does -- produces for the fixed dev seed
// (mnemonic "abandon abandon ... about", fingerprint 73C5DA0A) over the fixed
// test PSBTs in sim/test_crypto.c.
//
// These are NOT copied from KISS's own output. gen_sign_vectors.py signs the
// same PSBTs with embit and prints these lines; the C test signs them with the
// device code and asserts equality. If KISS's nonce derivation ever drifts, the
// two disagree and the build fails. See docs/specs/verifiable-determinism.md.
#pragma once

#define SV_ECDSA_LEGACY "304402202113f27e8fff9fe6aa3ccda7a954dffa738725be100597a1188963d8b0d967ec022064260f6205c2bcc4a2e4f5745e4ae6e72c60e95a2e09acffd647da798ee07b6d01"
#define SV_ECDSA_NESTED "304402203c7cd538720247ffe06df2168876adcf4a206e84f31aed9bf81ec02f8e5814c302206dff5b063fe2aa381e536ffddc4a08bd498ec5457546219257d505ca77f8cc0301"
#define SV_ECDSA_NATIVE "3044022075deee1de97201a2e93f4570ebf5e46e0ab59913b45f99902ef0e4fc7ba028e4022056129256869ccf4248455afd62d9faa136a15bff8f27a5dbd70f754b6af8c56901"

// Silent-payment Schnorr spends: plain BIP340, aux_rand = 0. Reproduced
// independently by the BIP340 reference signer in
// tools/sign_fixtures/gen_sp_sign_vectors.py, over the fixtures in
// main/sp_spend_vectors.h -- the taproot sighash there is itself embit-computed,
// so nothing in this line traces back to KISS's own signer. Because the aux is
// the standard one, ANY conforming BIP340 signer holding the same key produces
// these bytes, which is what makes an off-the-shelf second signer usable for
// the Dark Skippy check. 64-byte SIGHASH_DEFAULT form.
#define SV_SCHNORR_SP_EVEN "fad02bde2c752a6dc7ac1bb20eb78219393302f6479811a0e67e24260fbba48cee043e2ca482436dfe5794d95337d7378a76098411dd2420bfa216e4e6ef39c9"
#define SV_SCHNORR_SP_ODD  "1c7c27ac05b897266151e8d2b903531004b3e610c274c7b38c35833526e052f8cb9c45627fbb3b54b66ab10cb21b5db7f3643eb6fbd09ec96ff5c43c38f54193"
