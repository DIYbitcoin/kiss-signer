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

#include "ur_decoder.h"
#include "ur_encoder.h"
#include "types/psbt.h"

#define PMOFN_MAX_PARTS 64
#define PMOFN_CHUNK     100   // base64 chars per pMofN part
#define UR_MAX_FRAGMENT 120   // bytes per UR fragment (part str ~330 chars)
#define STATIC_MAX_B64  2900  // one-QR ceiling (v40 binary mode is 2953)

static const char UR_PSBT_PREFIX[] = "ur:crypto-psbt/";

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

// ---- parser ----
struct qrt_parser {
    int fmt;
    bool complete;
    uint8_t psbt[QRT_MAX_PSBT];
    size_t psbt_len;
    // pMofN state
    char *parts[PMOFN_MAX_PARTS];
    int total;
    int seen;
    // UR state
    ur_decoder_t *ur;
};

qrt_parser_t *qrt_parser_new(void) {
    return calloc(1, sizeof(qrt_parser_t));
}

void qrt_parser_free(qrt_parser_t *p) {
    if (!p) return;
    for (int i = 0; i < PMOFN_MAX_PARTS; i++) free(p->parts[i]);
    if (p->ur) ur_decoder_free(p->ur);
    // PSBT bytes passed through here; don't leave them in freed heap
    memset(p->psbt, 0, sizeof p->psbt);
    free(p);
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
    p->parts[m - 1] = strndup(sp + 1, plen);
    if (!p->parts[m - 1]) return -1;
    p->seen++;
    if (p->seen == p->total && pmofn_assemble(p) != 0) return -1;
    return 0;
}

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
                rc = 0;   // late extra part after completion: no-op
            else
                rc = ur_decoder_receive_part(p->ur, low) ? 0 : -1;
            if (p->ur && ur_decoder_is_complete(p->ur) && ur_decoder_is_success(p->ur))
                p->complete = true;
        }
    }
    free(low);
    return rc;
}

int qrt_parser_feed(qrt_parser_t *p, const char *data, size_t len) {
    if (!p || !data || len == 0) return -1;

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
            if (len > sizeof p->psbt) break;
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
        int rc = -1;
        if (bytes && n > 0 && n <= cap) {
            memcpy(out, bytes, n);
            *out_len = n;
            rc = 0;
        }
        psbt_free(pd);
        return rc;
    }

    if (p->psbt_len == 0 || p->psbt_len > cap) return -1;
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

qrt_encoder_t *qrt_encoder_new_frag(int fmt, const uint8_t *psbt, size_t len, int frag) {
    if (!psbt || len == 0 || len > QRT_MAX_PSBT) return NULL;
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
        if (cb) free(cb);
        if (pd) psbt_free(pd);
        if (!e->ur) { free(e); return NULL; }
        e->total = (int)ur_encoder_seq_len(e->ur);
        return e;
    }

    if (fmt == QRT_FMT_PMOFN || fmt == QRT_FMT_STATIC) {
        size_t cap = ((len + 2) / 3) * 4 + 8;
        e->b64 = malloc(cap);
        if (!e->b64 || b64_encode(psbt, len, e->b64, cap) != 0) {
            free(e->b64);
            free(e);
            return NULL;
        }
        e->b64_len = strlen(e->b64);
        if (fmt == QRT_FMT_STATIC) {
            if (e->b64_len > STATIC_MAX_B64) { free(e->b64); free(e); return NULL; }
            e->total = 1;
        } else {
            e->chunk = frag > 0 ? (size_t)frag : PMOFN_CHUNK;
            e->total = (int)((e->b64_len + e->chunk - 1) / e->chunk);
            if (e->total < 1) e->total = 1;
            if (e->total > PMOFN_MAX_PARTS) { free(e->b64); free(e); return NULL; }
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
    free(e->b64);
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
