// QR transport for PSBTs (step 6). Glue over components/cUR for BC-UR
// (fountain-coded animated QR, the coordinator standard); pMofN and static
// base64/binary handled locally. Tested by sim/test_qr.c on desktop.
//
// Only ur:crypto-psbt is accepted on the UR path — anything else a QR could
// carry is rejected at feed time, before it reaches the CBOR layer.
#include "qr_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiss_wipe.h"
#include "ur_decoder.h"
#include "ur_encoder.h"
#include "types/psbt.h"

#define PMOFN_MAX_PARTS 64
// Output has a larger byte ceiling than input because signatures and BIP375
// self-verification fields grow the PSBT. At the signed ceiling, easy-scan's
// 50-character chunks need 243 parts; the encoder streams them and holds no
// per-part pointer array, so this does not relax the decoder's 64-part cap.
#define PMOFN_MAX_OUTPUT_PARTS 256
#define PMOFN_CHUNK     100   // base64 chars per pMofN part
#define UR_MAX_FRAGMENT 120   // bytes per UR fragment (part str ~330 chars)
#define STATIC_MAX_B64  2900  // one-QR ceiling (v40 binary mode is 2953)

static const char UR_PSBT_PREFIX[] = "ur:crypto-psbt/";

// psbt_free is the vendored free and does not wipe. What it holds is the
// owner's transaction, on both the decode and the encode side, so the one
// place that knows the length wipes it first.
static void psbt_release(psbt_data_t *pd) {
    if (!pd) return;
    size_t n = 0;
    const uint8_t *b = psbt_get_data(pd, &n);
    if (b && n > 0) kiss_wipe((void *)(uintptr_t)b, n);
    psbt_free(pd);
}

// ---- local base64 (standard alphabet; no external deps so the sim build
// gets the real parser without linking libwally) ----
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *s, size_t len, uint8_t *out, size_t cap, size_t *olen) {
    while (len && s[len - 1] == '=') len--;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_val(s[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    *olen = n;
    return 0;
}

static int b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((n + 2) / 3) * 4 + 1;
    if (cap < need) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? A[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? A[v & 63] : '=';
    }
    out[o] = 0;
    return 0;
}

// Base64 of the largest PSBT this device can hold, and therefore what a whole
// pMofN set is allowed to add up to. The only ceiling before this was the
// array of 64 part POINTERS: nothing measured the parts, so a sender could
// hand over 64 QRs of any size each and every byte was kept in heap -- 165KB+
// of a ~340KB internal heap, allocated while the camera streams -- and the
// total was refused at assemble time, after the memory had been spent.
#define PMOFN_MAX_B64 ((size_t)QRT_MAX_PSBT * 4 / 3 + 8)

// ---- parser ----
struct qrt_parser {
    int fmt;
    bool complete;
    bool too_big;             // refused once: every later feed refuses too
    bool corrupt;             // terminal checksum/encoding failure
    uint8_t psbt[QRT_MAX_PSBT];
    size_t psbt_len;
    // pMofN state
    char *parts[PMOFN_MAX_PARTS];
    size_t b64_held;          // running sum of the part payloads kept
    int total;
    int seen;
    // UR state
    ur_decoder_t *ur;
};

qrt_parser_t *qrt_parser_new(void) {
    return calloc(1, sizeof(qrt_parser_t));
}

// Parts hold base64 of a transaction. Wipe before free, the same reason the
// assembled bytes below are wiped: this is the owner's transaction, and
// freed heap is read by whatever allocates next.
//
// kiss_wipe rather than memset, because every one of these is the last write
// to a buffer that is freed on the next line -- the exact dead store a
// compiler is entitled to drop, and the reason kiss_wipe.h exists.
static void parts_release(qrt_parser_t *p) {
    for (int i = 0; i < PMOFN_MAX_PARTS; i++) {
        if (!p->parts[i]) continue;
        kiss_wipe(p->parts[i], strlen(p->parts[i]));
        free(p->parts[i]);
        p->parts[i] = NULL;
    }
    p->b64_held = 0;
}

void qrt_parser_free(qrt_parser_t *p) {
    if (!p) return;
    parts_release(p);
    if (p->ur) ur_decoder_free(p->ur);
    // PSBT bytes passed through here; don't leave them in freed heap
    kiss_wipe(p->psbt, sizeof p->psbt);
    free(p);
}

void qrt_parser_reset(qrt_parser_t *p) {
    if (!p) return;
    parts_release(p);
    if (p->ur) { ur_decoder_free(p->ur); p->ur = NULL; }
    kiss_wipe(p->psbt, sizeof p->psbt);
    p->psbt_len = 0;
    p->fmt = QRT_FMT_NONE;
    p->complete = false;
    p->too_big = false;
    p->corrupt = false;
    p->total = 0;
    p->seen = 0;
}

// Assemble a completed pMofN set: concatenated base64 -> psbt bytes.
static int pmofn_assemble(qrt_parser_t *p) {
    char b64[QRT_MAX_PSBT * 4 / 3 + 8];
    size_t o = 0;
    for (int i = 0; i < p->total; i++) {
        size_t l = strlen(p->parts[i]);
        if (o + l >= sizeof b64) return -1;
        memcpy(b64 + o, p->parts[i], l);
        o += l;
    }
    if (b64_decode(b64, o, p->psbt, sizeof p->psbt, &p->psbt_len) != 0)
        return -1;
    p->complete = true;
    return 0;
}

static int feed_pmofn(qrt_parser_t *p, const char *data, size_t len) {
    // Header is parsed from a bounded copy, never sscanf'd straight off the
    // camera buffer (which is length-delimited, not NUL-terminated). The header
    // is short: "pMofN " up to the first space.
    const char *sp = memchr(data, ' ', len);
    if (!sp || sp + 1 >= data + len) return -1;
    size_t hlen = (size_t)(sp - data);
    char hdr[24];
    if (hlen == 0 || hlen >= sizeof hdr) return -1;
    memcpy(hdr, data, hlen);
    hdr[hlen] = 0;
    int m = 0, n = 0;
    if (sscanf(hdr, "p%dof%d", &m, &n) != 2) return -1;
    if (m < 1 || n < 1 || m > n || n > PMOFN_MAX_PARTS) return -1;
    size_t plen = (size_t)(data + len - (sp + 1));
    if (p->total == 0) p->total = n;
    else if (p->total != n) return -1;
    if (p->parts[m - 1]) return 0;   // duplicate scan, harmless
    // Before the allocation, not after the set completes. One oversized part
    // is refused on its own, and so is a set whose parts are individually
    // reasonable and collectively too large.
    if (plen >= PMOFN_MAX_B64 || p->b64_held + plen >= PMOFN_MAX_B64)
        return QRT_FEED_TOO_BIG;
    p->parts[m - 1] = strndup(sp + 1, plen);
    if (!p->parts[m - 1]) return -1;
    p->b64_held += plen;
    p->seen++;
    if (p->seen == p->total && pmofn_assemble(p) != 0)
        return QRT_FEED_CORRUPT;
    return 0;
}

// The UR message is the CBOR wrapper, not the PSBT: a byte string of this size
// class costs a 0x59 head and two length bytes. Eight is that with room, and
// small enough that nothing over the cap slips under it.
#define UR_CBOR_OVERHEAD 8

static int feed_ur(qrt_parser_t *p, const char *data, size_t len) {
    // Only crypto-psbt; reject other UR types before the CBOR layer sees them.
    if (len < sizeof UR_PSBT_PREFIX - 1) return -1;
    char *low = strndup(data, len);
    if (!low) return -1;
    for (char *c = low; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    int rc = -1;
    if (strncmp(low, UR_PSBT_PREFIX, sizeof UR_PSBT_PREFIX - 1) == 0) {
        if (!p->ur) p->ur = ur_decoder_new();
        if (p->ur) {
            if (ur_decoder_is_complete(p->ur))
                rc = ur_decoder_is_success(p->ur) ? 0 : QRT_FEED_CORRUPT;
            else if (ur_decoder_receive_part(p->ur, low))
                rc = 0;
            else
                // Past the decoder's own ceiling. Its own answer, because -1
                // here means "some other QR in view" and leaves the counter
                // sitting at its old value with nothing said.
                rc = ur_decoder_get_last_error(p->ur)
                             == UR_DECODER_ERROR_MESSAGE_TOO_LARGE
                         ? QRT_FEED_TOO_BIG
                         : -1;
            // The decoder's ceiling is generic; this one is the product's, and
            // it is the smaller of the two. Refused on the part that declares
            // it rather than after the set assembles: a transfer that finishes
            // and is then dropped is indistinguishable from a cancel, which is
            // what this used to look like.
            if (rc == 0 &&
                ur_decoder_expected_message_len(p->ur) >
                    (size_t)QRT_MAX_PSBT + UR_CBOR_OVERHEAD)
                rc = QRT_FEED_TOO_BIG;
            if (rc == 0 && ur_decoder_is_complete(p->ur)) {
                if (ur_decoder_is_success(p->ur))
                    p->complete = true;
                else
                    rc = QRT_FEED_CORRUPT;
            }
        }
    }
    // Bytewords of the owner's transaction; freed heap is read by whatever
    // allocates next.
    kiss_wipe(low, len);
    free(low);
    return rc;
}

int qrt_parser_feed(qrt_parser_t *p, const char *data, size_t len) {
    if (!p || !data || len == 0) return -1;
    if (p->too_big) return QRT_FEED_TOO_BIG;
    if (p->corrupt) return QRT_FEED_CORRUPT;

    int kind;
    if (len >= 5 && memcmp(data, "psbt\xff", 5) == 0) kind = QRT_FMT_STATIC;
    else if (len >= 6 && strncmp(data, "cHNidP", 6) == 0) kind = QRT_FMT_STATIC;
    else if (len >= 3 && (strncmp(data, "ur:", 3) == 0 || strncmp(data, "UR:", 3) == 0))
        kind = QRT_FMT_UR;
    else if (data[0] == 'p' && len >= 6 && memchr(data, ' ', len)) kind = QRT_FMT_PMOFN;
    else return -1;

    if (p->fmt != QRT_FMT_NONE && p->fmt != kind) return -1;

    int rc = -1;
    switch (kind) {
    case QRT_FMT_STATIC:
        if (p->complete) { rc = 0; break; }
        if (data[0] == 'p') {   // raw binary
            if (len > sizeof p->psbt) { rc = QRT_FEED_TOO_BIG; break; }
            memcpy(p->psbt, data, len);
            p->psbt_len = len;
            p->complete = true;
            rc = 0;
        } else {
            if (b64_decode(data, len, p->psbt, sizeof p->psbt, &p->psbt_len) == 0) {
                p->complete = true;
                rc = 0;
            }
        }
        break;
    case QRT_FMT_PMOFN:
        rc = feed_pmofn(p, data, len);
        break;
    case QRT_FMT_UR:
        rc = feed_ur(p, data, len);
        break;
    }
    if (rc == 0 && p->fmt == QRT_FMT_NONE) p->fmt = kind;
    if (rc == QRT_FEED_TOO_BIG) p->too_big = true;
    if (rc == QRT_FEED_CORRUPT) p->corrupt = true;
    return rc;
}

bool qrt_parser_complete(const qrt_parser_t *p) {
    return p && p->complete;
}

int qrt_parser_format(const qrt_parser_t *p) {
    return p ? p->fmt : QRT_FMT_NONE;
}

int qrt_parser_seen(const qrt_parser_t *p) {
    if (!p) return 0;
    if (p->fmt == QRT_FMT_UR && p->ur)
        return (int)ur_decoder_processed_parts_count(p->ur);
    if (p->fmt == QRT_FMT_STATIC) return p->complete ? 1 : 0;
    return p->seen;
}

int qrt_parser_total(const qrt_parser_t *p) {
    if (!p) return 0;
    if (p->fmt == QRT_FMT_UR && p->ur) {
        int n = (int)ur_decoder_expected_part_count(p->ur);
        if (n == 0 && p->complete) n = 1;   // single-part UR
        return n;
    }
    if (p->fmt == QRT_FMT_STATIC) return 1;
    return p->total;
}

int qrt_parser_result(qrt_parser_t *p, uint8_t *out, size_t cap, size_t *out_len) {
    if (!p || !out || !out_len || !p->complete) return -1;

    if (p->fmt == QRT_FMT_UR) {
        ur_result_t *r = ur_decoder_get_result(p->ur);
        if (!r || !r->type || strcmp(r->type, "crypto-psbt") != 0) return -1;
        psbt_data_t *pd = psbt_from_cbor(r->cbor_data, r->cbor_len);
        if (!pd) return -1;
        size_t n = 0;
        const uint8_t *bytes = psbt_get_data(pd, &n);
        // Too large gets its own answer here too, so the caller can say which
        // of the two things went wrong. Feed time refuses almost all of these
        // now; what still arrives is a payload inside the CBOR head allowance.
        int rc = bytes && n > cap ? QRT_FEED_TOO_BIG : -1;
        if (bytes && n > 0 && n <= cap) {
            memcpy(out, bytes, n);
            *out_len = n;
            rc = 0;
        }
        psbt_release(pd);
        return rc;
    }

    if (p->psbt_len > cap) return QRT_FEED_TOO_BIG;
    if (p->psbt_len == 0) return -1;
    memcpy(out, p->psbt, p->psbt_len);
    *out_len = p->psbt_len;
    return 0;
}

// ---- encoder ----
struct qrt_encoder {
    int fmt;
    // static / pMofN
    char *b64;
    size_t b64_len;
    size_t chunk;      // pMofN chars per part (PMOFN_CHUNK unless overridden)
    int total;
    int next_i;
    // UR
    ur_encoder_t *ur;
};

// The base64 of the signed transaction, on every path that drops it.
static void b64_release(qrt_encoder_t *e) {
    if (!e->b64) return;
    kiss_wipe(e->b64, e->b64_len ? e->b64_len : strlen(e->b64));
    free(e->b64);
    e->b64 = NULL;
    e->b64_len = 0;
}

qrt_encoder_t *qrt_encoder_new_frag(int fmt, const uint8_t *psbt, size_t len, int frag) {
    if (!psbt || len == 0 || len > QRT_MAX_SIGNED_PSBT) return NULL;
    qrt_encoder_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->fmt = fmt;

    if (fmt == QRT_FMT_UR) {
        psbt_data_t *pd = psbt_new(psbt, len);
        size_t cbl = 0;
        uint8_t *cb = pd ? psbt_to_cbor(pd, &cbl) : NULL;
        if (cb)
            e->ur = ur_encoder_new("crypto-psbt", cb, cbl,
                                   frag > 0 ? (size_t)frag : UR_MAX_FRAGMENT, 0, 10);
        if (cb) { kiss_wipe(cb, cbl); free(cb); }
        psbt_release(pd);
        if (!e->ur) { free(e); return NULL; }
        e->total = (int)ur_encoder_seq_len(e->ur);
        return e;
    }

    if (fmt == QRT_FMT_PMOFN || fmt == QRT_FMT_STATIC) {
        size_t cap = ((len + 2) / 3) * 4 + 8;
        e->b64 = malloc(cap);
        if (!e->b64 || b64_encode(psbt, len, e->b64, cap) != 0) {
            // A failed encode may have left it unterminated, so wipe the
            // allocation rather than what strlen would find in it.
            if (e->b64) { kiss_wipe(e->b64, cap); free(e->b64); }
            free(e);
            return NULL;
        }
        e->b64_len = strlen(e->b64);
        if (fmt == QRT_FMT_STATIC) {
            if (e->b64_len > STATIC_MAX_B64) { b64_release(e); free(e); return NULL; }
            e->total = 1;
        } else {
            e->chunk = frag > 0 ? (size_t)frag : PMOFN_CHUNK;
            e->total = (int)((e->b64_len + e->chunk - 1) / e->chunk);
            if (e->total < 1) e->total = 1;
            if (e->total > PMOFN_MAX_OUTPUT_PARTS) {
                b64_release(e);
                free(e);
                return NULL;
            }
        }
        return e;
    }

    free(e);
    return NULL;
}

qrt_encoder_t *qrt_encoder_new(int fmt, const uint8_t *psbt, size_t len) {
    return qrt_encoder_new_frag(fmt, psbt, len, 0);
}

void qrt_encoder_free(qrt_encoder_t *e) {
    if (!e) return;
    if (e->ur) ur_encoder_free(e->ur);
    b64_release(e);
    free(e);
}

int qrt_encoder_parts(const qrt_encoder_t *e) {
    return e ? e->total : 0;
}

int qrt_encoder_next(qrt_encoder_t *e, char *out, size_t cap) {
    if (!e || !out || cap == 0) return -1;

    if (e->fmt == QRT_FMT_UR) {
        char *part = NULL;
        if (!ur_encoder_next_part(e->ur, &part) || !part) return -1;
        size_t l = strlen(part);
        int rc = -1;
        if (l < cap) {
            memcpy(out, part, l + 1);
            rc = 0;
        }
        kiss_wipe(part, l);
        free(part);
        return rc;
    }

    if (e->fmt == QRT_FMT_STATIC) {
        if (e->b64_len + 1 > cap) return -1;
        memcpy(out, e->b64, e->b64_len + 1);
        return 0;
    }

    // pMofN cycles forever
    int i = e->next_i % e->total;
    e->next_i = (e->next_i + 1) % e->total;
    size_t off = (size_t)i * e->chunk;
    size_t l = e->b64_len - off;
    if (l > e->chunk) l = e->chunk;
    int n = snprintf(out, cap, "p%dof%d %.*s", i + 1, e->total, (int)l, e->b64 + off);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}
