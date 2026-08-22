// The post quantum half of the firmware update gate.
//
// An image only becomes bootable if TWO signatures check out: the secp256r1 one
// esp_ota already verifies against the key in the running app, and an
// SLH-DSA-SHA2-128s one over the same bytes, verified here. Both, never either
// -- this is a second lock on the same door, not a replacement for the first.
//
// The reason is narrow and worth stating plainly. Whoever can forge the release
// signing key can hand every KISS signer a firmware image it will install and
// trust, and that key is an elliptic curve key, which is the kind a quantum
// computer breaks. SLH-DSA rests on SHA-256 preimage resistance instead, where
// the best quantum attack is Grover's and halves the exponent rather than
// collapsing it.
//
// What this is NOT is a way to spend bitcoin. No consensus rule accepts a hash
// based signature: BIP-360 merged as Pay to Merkle Root with the post quantum
// signatures taken out of it. Nothing here goes near a key that holds coins.
#ifndef KISS_PQSIG_H
#define KISS_PQSIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha2_api.h"   // components/slhdsa: sha2_256_t

// ---- the trailer ----
//
// The signature rides on the END of the update .bin rather than in a file
// beside it. One file is what the card has always held and what G_FW_WHERE_B
// says, and a signature in a separate file is a signature somebody copies
// without -- which turns a refusal into a support question rather than a
// caught attack.
//
// It is a trailer and not a segment because an app image cannot contain a
// signature over itself. The device hands esp_ota_write everything EXCEPT these
// last bytes, so the image that lands in the slot is byte identical to the one
// espsecure signed, and the ESP signature check downstream sees exactly what it
// saw before this existed.
#define KISS_PQSIG_TRAILER_LEN 8192u
#define KISS_PQSIG_MAGIC       "KPQ1"
#define KISS_PQSIG_MAGIC_LEN   4u
#define KISS_PQSIG_SCHEME_SLH_DSA_SHA2_128S 1u
#define KISS_PQSIG_SIG_LEN     7856u   // slh_sig_sz(&slh_dsa_sha2_128s)
#define KISS_PQSIG_PK_LEN      32u
// magic[4] | scheme le16 | siglen le16 | signature | zero padding
#define KISS_PQSIG_HDR_LEN     8u

// The context string, so a signature made over one kind of blob can never be
// replayed as another. FIPS 205 folds it into the message with a length prefix,
// so this is domain separation and not decoration.
#define KISS_PQSIG_CTX "kiss-signer fw v1"

enum {
    KISS_PQSIG_OK          =  0,
    KISS_PQSIG_ERR_NO_KEY  = -1,   // this build has no public key compiled in
    KISS_PQSIG_ERR_MAGIC   = -2,   // no trailer, or not one of ours
    KISS_PQSIG_ERR_SCHEME  = -3,   // a scheme this firmware does not know
    KISS_PQSIG_ERR_LENGTH  = -4,   // header fields disagree with the trailer
    KISS_PQSIG_ERR_PADDING = -5,   // slack after the signature is not zero
    KISS_PQSIG_ERR_VERIFY  = -6,   // the signature did not check out
};

// True when a public key was compiled in. False is the honest state of a build
// made without one, and the update screen must say the image cannot be checked
// rather than install it -- same shape as kiss_fw_available().
bool kiss_pqsig_available(void);

// ---- splitting the trailer off a stream ----
//
// The card cannot seek (platform_sd has no such call), so the trailer cannot be
// read before the image is written. It is held back instead: everything more
// than KISS_PQSIG_TRAILER_LEN bytes from the end is released to the writer as it
// arrives and folded into the digest, and what is still in hand at the end IS
// the trailer.
//
// This lives here, behind a sink, rather than inside kiss_fw_install's loop,
// because no desktop gate compiles that loop -- kiss_fw_install is stubbed off
// ESP_PLATFORM. Split out it can be driven at every chunk boundary from
// sim/test_pq.c, which is the only way an off by one in it is ever found.
typedef int (*kiss_pqsig_sink_fn)(const uint8_t *data, size_t len, void *ud);

typedef struct {
    sha2_256_t sha;                 // over the image bytes, never the trailer
    uint8_t  tail[KISS_PQSIG_TRAILER_LEN];
    size_t   tail_len;
    uint64_t image_len;             // bytes released to the sink so far
    int      sink_rc;               // first nonzero the sink returned
} kiss_pqsig_stream_t;

void kiss_pqsig_stream_init(kiss_pqsig_stream_t *s);

// Feed one chunk. Returns 0, or whatever the sink returned when it refused --
// after which the stream is spent and further feeds do nothing.
int kiss_pqsig_stream_feed(kiss_pqsig_stream_t *s, const uint8_t *buf, size_t len,
                           kiss_pqsig_sink_fn sink, void *ud);

// Close the stream. Fills digest with SHA-256 of everything the sink was given,
// and points trailer at the bytes held back. Returns KISS_PQSIG_ERR_LENGTH if
// the file was too short to hold a trailer at all.
int kiss_pqsig_stream_end(kiss_pqsig_stream_t *s, uint8_t digest[32],
                          const uint8_t **trailer, size_t *trailer_len);

// Bytes the sink was given, which is the size of the image itself. A caller that
// told esp_ota_begin a size has to be told the same number back.
uint64_t kiss_pqsig_stream_image_len(const kiss_pqsig_stream_t *s);

// ---- the check ----
//
// digest is SHA-256 of the image; trailer is what the stream held back. Returns
// KISS_PQSIG_OK or one of the codes above. About 2100 SHA-256 compressions, so
// a few milliseconds on this part -- roughly a thousandth of what producing the
// signature costs, which is the whole shape of a hash based scheme.
int kiss_pqsig_check(const uint8_t digest[32], const uint8_t *trailer, size_t trailer_len);

// ---- selftest ----
//
// Verifies one NIST ACVP vector. On the device this is the ONLY thing that
// exercises the SHA accelerator's register mapping: every desktop gate runs the
// portable C, so a wrong mapping would pass all of them and then refuse every
// real firmware image on the bench. Returns 0, or nonzero naming the stage.
int kiss_pqsig_selftest(void);

#ifndef ESP_PLATFORM
// Desktop only. Swap the key the check runs against, so the suite can mint a
// throwaway keypair, build a real trailer with it, and watch this accept the
// one it signed and refuse everything else. NULL puts the compiled in key back.
void kiss_pqsig_test_set_pubkey(const uint8_t *pk);
#endif

#endif // KISS_PQSIG_H
