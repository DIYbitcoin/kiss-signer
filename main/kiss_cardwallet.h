// The wallet on a Satochip card, as this signer sees it: public keys read
// once at unlock, and signatures asked for one at a time. No private key and
// no seed ever comes into this device.
//
// The session runs: connect (select the applet, open the secure channel),
// PIN (one attempt; on success the genuine card check runs and the public
// keys are read), then signing until close. Every call blocks on the card, so
// the device runs them on a worker task, never on the UI task.
//
// What is read at unlock: the master public key (its fingerprint names the
// wallet) and, for purposes 44, 49 and 84 on mainnet and on the test coin,
// the account key m/purpose'/coin'/0' with its parent's fingerprint, so
// addresses, descriptors and exports serialize exactly as a wallet that held
// the seed would. Thirteen derivations on the card, once.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wally_bip32.h>

#include "kiss_satocard.h"

// Select the Satochip applet and open the channel. On SC_OK *pin_tries says
// how many PIN attempts the card has left.
int kiss_cardwallet_connect(kiss_sc_xfer_fn xfer, void *ctx, kiss_sc_rng_fn rng,
                            int *pin_tries);

// One PIN attempt, then the genuine card check and the public keys. SC_OK
// means the card wallet is open. SC_ERR_PIN fills *tries_left; on every
// failure after the PIN itself the card is closed and must be reconnected.
// progress, when given, is told how many of the thirteen keys are read.
int kiss_cardwallet_unlock(const char *pin, size_t pin_len, int *tries_left,
                           void (*progress)(int done, int total));

bool kiss_cardwallet_active(void);

// After connect: the card has 2FA on, so it signs only with a second device's
// approval, which an offline signer can never give. Refuse before the PIN.
bool kiss_cardwallet_needs_2fa(void);

// The wallet's master fingerprint.
int kiss_cardwallet_fingerprint(uint8_t out[4]);

// The account key m/purpose'/coin'/0' as a public ext_key (version MAIN or
// TEST public, parent fingerprint set). NULL if not read.
const struct ext_key *kiss_cardwallet_account(uint32_t purpose, bool testnet);

// Sign hash32 with the key at path (m/... in full, depth entries). The
// signature is low-S DER, already verified against that key.
int kiss_cardwallet_sign(const uint32_t *path, int depth, const uint8_t hash32[32],
                         uint8_t *der, size_t *der_len);

// End the card session: keys, channel, everything cached. Safe to call twice.
void kiss_cardwallet_close(void);
