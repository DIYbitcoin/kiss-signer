// SeedKeeper: read one seed off a Satochip SeedKeeper card.
//
// What a signer needs to LOAD a wallet from the card, on top of an open,
// PIN-verified, genuine kiss_satocard session (kiss_satocard.h): list what
// the card holds, export one entry and turn it into words. Nothing here
// writes to the card.
//
// Written from Toporin/Seedkeeper-Applet and its Specifications.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kiss_satocard.h"

#define SK_LABEL_MAX 64

// One entry as the card lists it. Plain metadata: the label is whatever the
// owner typed, untrusted text, reduced to printable ASCII.
typedef struct {
    uint16_t id;
    uint8_t  type;         // SK_TYPE_*
    uint8_t  subtype;
    uint8_t  origin;
    uint8_t  export_ctrl;  // SK_EXPORT_*
    uint8_t  fingerprint[4];
    char     label[SK_LABEL_MAX + 1];
} kiss_sk_header;

#define SK_TYPE_MASTERSEED 0x10
#define SK_TYPE_BIP39      0x30
#define SK_TYPE_ELECTRUM   0x40
#define SK_SUBTYPE_BIP39   0x01

#define SK_EXPORT_FORBIDDEN  0x00
#define SK_EXPORT_ALLOWED    0x01
#define SK_EXPORT_SECUREONLY 0x02

// Every entry on the card, up to max. *count is how many were written.
int kiss_sk_list(kiss_satocard *c, kiss_sk_header *out, int max, int *count);

// True when this signer can turn the entry into words: a BIP39 mnemonic, or
// a master seed stored with its BIP39 entropy, that the card lets out.
bool kiss_sk_loadable(const kiss_sk_header *h);

// Export one entry and decode it. words gets the mnemonic, passphrase
// whatever passphrase the entry stores (often empty). Both NUL-terminated
// and cleared on any failure. The export is signed by the card's certified
// key and refused if that signature does not hold.
int kiss_sk_load(kiss_satocard *c, uint16_t id,
                 char *words, size_t words_cap,
                 char *passphrase, size_t pass_cap);

// ---- exposed for the tests ----
// Decode an exported entry (type/subtype/data as the card returns them).
int kiss_sk_decode(uint8_t type, uint8_t subtype,
                   const uint8_t *data, size_t len,
                   char *words, size_t words_cap,
                   char *passphrase, size_t pass_cap);
