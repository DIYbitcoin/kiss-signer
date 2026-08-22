// QR transport for PSBTs (step 6): assemble scanned QR strings into a PSBT,
// and emit a PSBT as QR part strings. Thin glue over components/cUR (BC-UR
// fountain codes); pMofN + static base64 handled locally. No LVGL, no camera,
// no libwally — pure data layer, tested on desktop by sim/test_qr.c.
#ifndef QR_TRANSPORT_H
#define QR_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Detected / requested wire format.
#define QRT_FMT_NONE   0
#define QRT_FMT_STATIC 1   // one QR: base64 text or raw binary PSBT
#define QRT_FMT_PMOFN  2   // "pMofN <base64>" legacy animated (Specter/Krux)
#define QRT_FMT_UR     3   // ur:crypto-psbt, single or fountain multi-part

#define QRT_MAX_PSBT          4096   // matches the Sign screen input buffer
// Signing adds fields to the parsed PSBT: up to 107 serialized bytes for each
// of 16 ECDSA signatures. A 16-recipient BIP375 transaction can also gain one
// 169-byte share/proof pair and a 37-byte output script per recipient, plus the
// 4-byte modifiable-flags field, while the signer self-verifies it. Reserve the
// full accepted-input ceiling plus that worst-case growth so every transaction
// that passes review can still be serialized and returned after signing.
#define QRT_MAX_SIGNED_PSBT   9108

// ---- decode: feed scanned QR payloads until complete ----
typedef struct qrt_parser qrt_parser_t;

qrt_parser_t *qrt_parser_new(void);
void qrt_parser_free(qrt_parser_t *p);

// A transfer too large for QRT_MAX_PSBT, refused at feed time. Distinct from
// -1 on purpose: -1 means "some other QR is in view", which the scan screen
// ignores by design, and a size refusal reported that way is a scan that sits
// on its part counter forever with nothing on screen saying why.
#define QRT_FEED_TOO_BIG (-2)

// A multipart transfer that reached its terminal part but failed its own
// checksum/encoding. Unlike an unrelated QR (-1), feeding more parts cannot
// repair this set: the caller must reset it before scanning again.
#define QRT_FEED_CORRUPT (-3)

// Feed one scanned QR payload (may contain NULs for binary QRs).
// 0 = accepted (including harmless duplicates), -1 = not usable for this scan
// (unknown format, or a format different from the one already in progress),
// QRT_FEED_TOO_BIG = larger than this device can hold, QRT_FEED_CORRUPT = a
// complete multipart set failed its checksum or encoding. Once a parser has
// answered either terminal error it keeps answering it until reset: the parts
// it holds are from a set it will never finish, and quietly accepting more
// would leave the scan screen wedged on an unfinishable transfer.
int qrt_parser_feed(qrt_parser_t *p, const char *data, size_t len);

// Back to what qrt_parser_new returns: no parts, no bytes, no format. Lets
// the scan screen abandon a refused transfer without tearing the camera down.
void qrt_parser_reset(qrt_parser_t *p);

bool qrt_parser_complete(const qrt_parser_t *p);
int qrt_parser_format(const qrt_parser_t *p);   // QRT_FMT_* (NONE until first feed)

// Progress for the scan UI. total = 0 while still unknown.
int qrt_parser_seen(const qrt_parser_t *p);
int qrt_parser_total(const qrt_parser_t *p);

// Copy the assembled raw PSBT into out. 0 on success (only when complete and
// it fits in cap); QRT_FEED_TOO_BIG when it assembled but does not fit, so the
// caller can say so rather than backing out; other nonzero otherwise.
int qrt_parser_result(qrt_parser_t *p, uint8_t *out, size_t cap, size_t *out_len);

// ---- encode: turn a PSBT into QR part strings ----
typedef struct qrt_encoder qrt_encoder_t;

// fmt = QRT_FMT_UR / QRT_FMT_PMOFN / QRT_FMT_STATIC. Encoders accept signed
// results up to QRT_MAX_SIGNED_PSBT; STATIC still refuses payloads too big for
// one QR.
qrt_encoder_t *qrt_encoder_new(int fmt, const uint8_t *psbt, size_t len);

// Same, with a fragment-size override (UR: bytes per fragment; pMofN: base64
// chars per part; <= 0 = defaults). Smaller fragments = sparser, easier-to-scan
// QRs — the SIGNED screen's easy-scan mode uses this.
qrt_encoder_t *qrt_encoder_new_frag(int fmt, const uint8_t *psbt, size_t len, int frag);
void qrt_encoder_free(qrt_encoder_t *e);

// Parts in one display cycle (1 = static / single-part UR).
int qrt_encoder_parts(const qrt_encoder_t *e);

// Write the next part string into out (NUL-terminated). Animated formats
// cycle forever (UR fountain parts keep evolving past the pure fragments —
// that is what makes lossy scanning converge). 0 on success.
int qrt_encoder_next(qrt_encoder_t *e, char *out, size_t cap);

#endif // QR_TRANSPORT_H
