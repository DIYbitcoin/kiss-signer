// Desktop tests for main/qr_transport.c (step 6 QR transport).
// Inputs are crafted with cUR / libwally primitives directly, independent of
// the glue under test, plus cUR's own multipart UR test vectors.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr_transport.h"

#include <wally_core.h>      // wally_base64_from_bytes (cross-check only)
#include "ur_encoder.h"      // craft single-part UR input
#include "types/psbt.h"      // crypto-psbt CBOR wrap for crafting
#include "fountain_decoder.h"  // hostile headers, below the bytewords layer
#include "fountain_utils.h"    // the PRNG that picks fragment indexes
#include "utils.h"             // ur_alloc_arm / ur_alloc_hits: the injection
#include "k_quirc_internal.h"  // quirc_version_db: the block tables
int k_quirc_alpha_char(int v);  // k_quirc_decode.c, split out to be testable

#ifndef KISS_ROOT
#define KISS_ROOT "."
#endif
#define UR_VECTOR_TXT  KISS_ROOT "/components/cUR/tests/test_cases/PSBTs/PSBT_1.UR_fragments.txt"
#define UR_VECTOR_BIN  KISS_ROOT "/components/cUR/tests/test_cases/PSBTs/PSBT_1.psbt.bin"

static int qfails;

static void qchki(const char *name, long long got, long long want) {
    if (got == want) printf("PASS: %s -> %lld\n", name, got);
    else { printf("FAIL: %s\n  got  %lld\n  want %lld\n", name, got, want); qfails++; }
}

static void qchkb(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); qfails++; }
}

static uint8_t *read_bin(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

// Pull the "ur:..." strings out of the (python-list formatted) vector file.
static int read_ur_lines(const char *path, char **out, int max) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char *a = strchr(line, '"');
        if (!a) continue;
        char *b = strchr(a + 1, '"');
        if (!b) continue;
        *b = 0;
        out[n++] = strdup(a + 1);
    }
    fclose(f);
    return n;
}

// A UR part's header declares how long the whole message is, and the decoder
// believed it. seq_len fragments of data_len bytes can only ever carry
// seq_len*data_len bytes, so a header claiming more describes a message the
// frames cannot contain: the join allocates message_len UNINITIALISED, fills
// what the fragments hold, and then CRCs -- and, on a checksum collision,
// hands back -- whatever the heap happened to have in the rest.
//
// The cap is 256 KB, so one frame could ask for a quarter megabyte of it.
// Nothing needs to be malformed at any layer below: the bytewords CRC is
// good, the CBOR parses, the seq numbers agree with the URI path. Only the
// three numbers read together are a lie, which is why this is tested here and
// not at the parser.
// A pMofN set is bounded by the BYTES it carries, not only by how many parts
// it names. The parser counted parts alone, so 64 QRs of any size each were
// each strndup'd into heap while the camera streamed, and the total was
// refused only at assemble time -- after the memory had been spent. These
// pin the refusal at feed time, and pin that a refused parser stays refused
// until it is reset (its held parts belong to a set it will never finish).
static void qr_test_pmofn_bounds(void) {
    const size_t cap_b64 = (size_t)QRT_MAX_PSBT * 4 / 3 + 8;

    {   // one part larger than the whole budget: refused on its own
        qrt_parser_t *p = qrt_parser_new();
        char *big = malloc(cap_b64 + 64);
        if (!p || !big) { qchkb("pmofn bounds allocs", 0); free(big); qrt_parser_free(p); return; }
        memset(big, 'A', cap_b64 + 63);
        memcpy(big, "p1of4 ", 6);
        big[cap_b64 + 63] = 0;
        qchki("qr pMofN oversize part refused", qrt_parser_feed(p, big, strlen(big)),
              QRT_FEED_TOO_BIG);
        qchki("qr pMofN nothing kept", qrt_parser_seen(p), 0);
        // and it stays refused: a later, perfectly ordinary part must not be
        // folded into the set it half-holds
        qchki("qr pMofN latched", qrt_parser_feed(p, "p2of4 QUJD", 10),
              QRT_FEED_TOO_BIG);
        qrt_parser_reset(p);
        qchki("qr pMofN reset clears the latch", qrt_parser_feed(p, "p2of4 QUJD", 10), 0);
        free(big);
        qrt_parser_free(p);
    }

    {   // parts each well inside the budget, together past it
        qrt_parser_t *p = qrt_parser_new();
        size_t chunk = cap_b64 / 3;
        char *part = malloc(chunk + 16);
        if (!p || !part) { qchkb("pmofn sum allocs", 0); free(part); qrt_parser_free(p); return; }
        int rc = 0, accepted = 0;
        for (int i = 1; i <= 4 && rc == 0; i++) {
            snprintf(part, 16, "p%dof4 ", i);
            size_t hlen = strlen(part);
            memset(part + hlen, 'A', chunk);
            part[hlen + chunk] = 0;
            rc = qrt_parser_feed(p, part, hlen + chunk);
            if (rc == 0) accepted++;
        }
        qchki("qr pMofN sum refused before the set completes", rc, QRT_FEED_TOO_BIG);
        qchkb("qr pMofN sum refused before assembly", accepted < 4);
        free(part);
        qrt_parser_free(p);
    }

    {   // a static binary PSBT past the cap answers TOO_BIG, not "unknown QR"
        qrt_parser_t *p = qrt_parser_new();
        size_t n = QRT_MAX_PSBT + 64;
        char *raw = malloc(n);
        if (!p || !raw) { qchkb("static bounds allocs", 0); free(raw); qrt_parser_free(p); return; }
        memset(raw, 0x5A, n);
        memcpy(raw, "psbt\xff", 5);
        qchki("qr static oversize refused", qrt_parser_feed(p, raw, n), QRT_FEED_TOO_BIG);
        free(raw);
        qrt_parser_free(p);
    }
}

static void qr_test_hostile_header(void) {
    const size_t body_len = 10;

    struct { const char *name; size_t seq_len; size_t message_len; bool ok; } cases[] = {
        // one 10-byte fragment cannot be a 256 KB message
        { "message_len far past what the frames carry", 1, 256u * 1024u, false },
        // nor one byte past
        { "message_len one byte past the frames",       1, 11,           false },
        // seq_len sized for a message that ended two fragments ago: fragment 3
        // of 3 would be entirely padding, which no encoder produces
        { "seq_len larger than the message needs",      3, 10,           false },
        // the honest shapes still load
        { "exact single fragment",                      1, 10,           true  },
        { "final fragment part-full",                   3, 25,           true  },
    };

    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        fountain_decoder_t *d = fountain_decoder_new();
        uint8_t *body = malloc(body_len);      // the decoder MOVES data out
        if (!d || !body) { qchkb("hostile-header decoder allocs", 0); return; }
        memset(body, 0xA5, body_len);
        fountain_encoder_part_t part = {
            .seq_num = 1,
            .seq_len = cases[c].seq_len,
            .message_len = cases[c].message_len,
            .checksum = 0,
            .data = body,
            .data_len = body_len,
        };
        bool got = fountain_decoder_receive_part(d, &part);
        char nm[96];
        snprintf(nm, sizeof nm, "qr ur %s", cases[c].name);
        qchkb(nm, got == cases[c].ok);
        free(part.data);                       // NULL once the decoder took it
        fountain_decoder_free(d);
    }
}

// prng_next_double's divisor is (double)UINT64_MAX + 1.0, and both halves of
// that round to 2**64 -- so the top handful of PRNG outputs come back as
// EXACTLY 1.0. choose_fragments scales that onto [0, remaining_count-1] and
// indexes an array of seq_len with the result: on the first draw
// remaining_count is seq_len, and 1.0 lands one element past the end.
//
// Reaching it through the PRNG means solving xoshiro256** backwards, so the
// scaling is tested where the pathological value enters it. That the value is
// reachable is arithmetic, not luck.
static void qr_test_prng_range(void) {
    qchkb("qr ur prng scale keeps 1.0 inside the range",
          prng_scale_double(1.0, 0, 9) == 9);
    qchkb("qr ur prng scale keeps the ordinary values",
          prng_scale_double(0.0, 0, 9) == 0 &&
          prng_scale_double(0.5, 0, 9) == 5 &&
          prng_scale_double(0.999, 0, 9) == 9);
}


// The alphanumeric alphabet has 45 entries and BOTH fields that index it are
// wider: a pair is 11 bits, so d/45 reaches 45 (the NUL), and a lone character
// is 6 bits, so it reaches 63 -- up to 17 bytes past the end of a 46 byte
// literal, copied straight into the decoded payload. This is the first code in
// the signer to touch bytes off a QR code held up to the camera.
static void qr_test_alpha_bounds(void) {
    static const char *ALPHA = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    int ok = 1;
    for (int v = 0; v < 45; v++)
        if (k_quirc_alpha_char(v) != (int)(unsigned char)ALPHA[v]) ok = 0;
    qchkb("qr alpha map is unchanged for every valid value", ok);

    int refused = 1;
    for (int v = 45; v < 64; v++)          // 6-bit field reaches 63
        if (k_quirc_alpha_char(v) != -1) refused = 0;
    qchkb("qr alpha map refuses 45..63 instead of reading past the end", refused);
    qchkb("qr alpha map refuses the 11-bit pair overflow", k_quirc_alpha_char(45) == -1);
    qchkb("qr alpha map refuses a negative index", k_quirc_alpha_char(-1) == -1);
}

// read_data derives the long-block count from data_bytes, so every row of the
// version database has to satisfy ns*bs + lb*(bs+1) == data_bytes exactly, with
// lb a whole number. A row that does not is a version+ECC combination that can
// never decode however clean the scan is -- v25-Q shipped that way, at ns=3
// where only 7 closes it. This checks the invariant the decoder actually uses,
// so it needs no copy of the spec tables to be right.
static void qr_test_version_tables(void) {
    int bad = 0;
    for (int v = 1; v <= QUIRC_MAX_VERSION; v++) {
        const struct quirc_version_info *info = &quirc_version_db[v];
        for (int e = 0; e < 4; e++) {
            const struct quirc_rs_params *p = &info->ecc[e];
            if (!p->bs || !p->ns) { bad++; continue; }
            int rest = info->data_bytes - p->bs * p->ns;
            if (rest < 0 || rest % (p->bs + 1) != 0) {
                printf("  version %d ecc[%d]: bs=%d ns=%d does not tile %d bytes\n",
                       v, e, p->bs, p->ns, info->data_bytes);
                bad++;
            }
        }
    }
    qchkb("qr every version/ecc row tiles its data_bytes exactly", bad == 0);
}

// Feed one part; count it against the test only while the decode is still
// live -- once complete, refusals are the expected clean behavior. Returns
// -1 on encoder failure, 0 while feeding continues, 1 once complete.
static int cur_feed(fountain_decoder_t *d, fountain_encoder_t *e,
                    fountain_encoder_part_t *p, size_t *fed, size_t *refused) {
    if (!fountain_encoder_next_part(e, p)) return -1;
    if (!fountain_decoder_receive_part(d, p) &&
        !fountain_decoder_is_complete(d))
        (*refused)++;
    free(p->data);
    (*fed)++;
    return fountain_decoder_is_complete(d) ? 1 : 0;
}

// The mixed-part budget is the device's protection against a fragment feed
// that would fill the heap: parts past MAX_MIXED_BYTES are refused, not kept.
// The accounting behind that ceiling must charge what an entry actually holds
// (payload, both duplicated index arrays, and the entry itself) and must
// follow entries as they shrink and vanish during reduction. A counter that
// only ever grew would drift past the ceiling on churn, then refuse parts of
// a UR it could still decode -- on a session that had already spent the
// memory. The assertions read the budget directly (peak bytes, refusal count,
// via fountain_decoder_mixed_stats): the ceiling activates, the peak stays
// under it, and the decode still completes afterwards.
//
// This is a LOW-LEVEL fountain stress test, not a product scenario: both
// fixtures are well past the product's 16 KiB UR ceiling (the PSBT behind it
// is 4 KiB), which is deliberate -- the mixed hash must hold its 96 KiB bound
// even on a message bigger than anything the device accepts.
//
// first_seq_num selects where the encoder starts: 0 yields the raw fragments
// (parts 1..seq_len), 60 skips past them into the deterministic redundant
// parts, so "redundant first" and "raw fragments" come from separate
// encoders. Encoder output is deterministic per message (choose_fragments
// seeds from seq_num and the message checksum), so a second encoder with the
// same start yields identical parts: the duplicates below are exact re-feeds,
// and the churn is reproducible run to run.
static void qr_test_fountain_cap_churn(void) {
    // Churn fixture: 60 KiB of 1 KiB fragments. The working set stays under
    // the ceiling and every feed is accepted while the decode is live.
    const size_t frag = 1024, msg_len = 60 * 1024;
    uint8_t *msg = malloc(msg_len);
    if (!msg) { qchkb("fountain cap allocs", 0); return; }
    for (size_t i = 0; i < msg_len; i++) msg[i] = (uint8_t)(i * 31 + 7);

    fountain_encoder_t *e1 = fountain_encoder_new(msg, msg_len, frag, 60, frag);
    fountain_encoder_t *e2 = fountain_encoder_new(msg, msg_len, frag, 60, frag);
    fountain_encoder_t *e3 = fountain_encoder_new(msg, msg_len, frag, 0, frag);
    if (!e1 || !e2 || !e3) { qchkb("fountain cap encoders", 0); goto done; }

    qchki("fountain cap seq_len", fountain_encoder_seq_len(e1), msg_len / frag);

    {   // churn: redundant parts first, then exact duplicates of some, then
        // every raw fragment. No feed may be refused while the decode is
        // still live, and the decode must complete with the exact message.
        // Redundant degree-1 parts already mark fragments, so completion can
        // legitimately land before the raw loop ends: feeding stops there.
        fountain_decoder_t *d = fountain_decoder_new();
        fountain_encoder_part_t p1 = {0}, p2 = {0}, p3 = {0};
        size_t fed = 0, refused = 0;
        int ok = 1, done = 0, r;
        for (size_t i = 0; i < 90 && ok && !done; i++) {
            r = cur_feed(d, e1, &p1, &fed, &refused);
            if (r < 0) ok = 0; else if (r) done = 1;
        }
        for (size_t i = 0; i < 40 && ok && !done; i++) {   // dups of 61..100
            r = cur_feed(d, e2, &p2, &fed, &refused);
            if (r < 0) ok = 0; else if (r) done = 1;
        }
        for (size_t i = 0; i < 60 && ok && !done; i++) {   // raw fragments
            r = cur_feed(d, e3, &p3, &fed, &refused);
            if (r < 0) ok = 0; else if (r) done = 1;
        }
        qchkb("fountain cap no feed refused while live", ok && refused == 0);
        qchkb("fountain cap completed under churn", ok && done);
        qchkb("fountain cap bytes match",
              ok && fountain_decoder_result_message_len(d) == msg_len &&
              fountain_decoder_result_message(d) &&
              memcmp(fountain_decoder_result_message(d), msg, msg_len) == 0);
        {
            size_t peak = 0, drops = 0;
            fountain_decoder_mixed_stats(d, &peak, &drops);
            qchkb("fountain cap peak under the 96 KiB ceiling",
                  ok && peak <= 96 * 1024);
            qchkb("fountain cap churn peak a real working set",
                  ok && peak >= 64 * 1024);
            qchkb("fountain cap churn refused nothing", ok && drops == 0);
        }
        if (ok) {   // a completed decoder refuses everything, cleanly
            if (!fountain_encoder_next_part(e1, &p1)) ok = 0;
            else {
                qchkb("fountain cap feeds after completion refused",
                      !fountain_decoder_receive_part(d, &p1));
                free(p1.data);
            }
        }
        qchkb("fountain cap churn run held together", ok);
        fountain_decoder_free(d);
    }

    {   // the budget hammered: 4 KiB fragments over a 96 KiB message, so the
        // mixed working set exceeds the 96 KiB ceiling before the fountain
        // system completes. The budget must refuse mid-session (silently, by
        // design), the decoder must not wedge, completion must still land,
        // and once complete it must refuse every further feed cleanly.
        const size_t hfrag = 4096, hlen = 96 * 1024;
        uint8_t *hmsg = malloc(hlen);
        if (!hmsg) { qchkb("fountain cap hammered allocs", 0); goto done; }
        for (size_t i = 0; i < hlen; i++) hmsg[i] = (uint8_t)(i * 31 + 7);
        fountain_encoder_t *he =
            fountain_encoder_new(hmsg, hlen, hfrag, 60, hfrag);
        if (!he) { qchkb("fountain cap hammered encoder", 0); free(hmsg); goto done; }
        fountain_decoder_t *d = fountain_decoder_new();
        fountain_encoder_part_t p1 = {0};
        int ok = 1, done = 0;
        for (size_t rep = 0; rep < 400; rep++) {
            if (!fountain_encoder_next_part(he, &p1)) { ok = 0; break; }
            fountain_decoder_receive_part(d, &p1);
            free(p1.data);
            if (fountain_decoder_is_complete(d)) { done = 1; break; }
        }
        qchkb("fountain cap hammered feed completes", ok && done);
        {
            size_t peak = 0, drops = 0;
            fountain_decoder_mixed_stats(d, &peak, &drops);
            qchkb("fountain cap peak under the 96 KiB ceiling",
                  ok && peak <= 96 * 1024);
            qchkb("fountain cap budget refused at least once", ok && drops >= 1);
            qchkb("fountain cap peak near the ceiling",
                  ok && peak > 90 * 1024);
        }
        for (size_t rep = 0; rep < 50 && ok; rep++) {
            if (!fountain_encoder_next_part(he, &p1)) { ok = 0; break; }
            if (fountain_decoder_receive_part(d, &p1)) ok = 0;   // must refuse
            free(p1.data);
        }
        qchkb("fountain cap completed decoder refuses everything", ok && done);
        qchkb("fountain cap hammered bytes match",
              ok && fountain_decoder_result_message_len(d) == hlen &&
              fountain_decoder_result_message(d) &&
              memcmp(fountain_decoder_result_message(d), hmsg, hlen) == 0);
        qchkb("fountain cap hammered run held together", ok);
        fountain_decoder_free(d);
        fountain_encoder_free(he);
        free(hmsg);
    }

    {   // The public difference helper supports a populated output. A failed
        // exact allocation leaves that output untouched; a successful retry
        // replaces it without leaking the old buffer.
        size_t ai[] = {1, 2, 4, 8}, bi[] = {2, 8};
        part_indexes_t a = {.indexes = ai, .count = 4, .capacity = 4};
        part_indexes_t b = {.indexes = bi, .count = 2, .capacity = 2};
        part_indexes_t result = {0};
        result.indexes = malloc(2 * sizeof(size_t));
        if (!result.indexes) {
            qchkb("fountain difference reuse alloc", 0);
        } else {
            result.count = result.capacity = 2;
            result.indexes[0] = 77;
            result.indexes[1] = 88;
            size_t *old = result.indexes;
            int prev = ur_site_enter(UR_SITE_DIFF);
            ur_alloc_arm(UR_SITE_DIFF, 1);
            bool failed = part_indexes_difference(&a, &b, &result);
            ur_site_leave(prev);
            qchkb("fountain difference failure preserves populated result",
                  !failed && ur_alloc_hits() == 1 && result.indexes == old &&
                  result.count == 2 && result.capacity == 2 &&
                  result.indexes[0] == 77 && result.indexes[1] == 88);
            ur_alloc_disarm();
            qchkb("fountain difference populated result can be replaced",
                  part_indexes_difference(&a, &b, &result) &&
                  result.count == 2 && result.capacity == 2 &&
                  result.indexes[0] == 1 && result.indexes[1] == 4);
            free(result.indexes);
        }
    }

    {   // The encoder mixes the payload before it copies the chosen indexes
        // into its next-part state. An allocation failure in that last copy
        // must release both temporary allocations, return an empty part, and
        // leave the encoder usable for a retry.
        fountain_encoder_t *e =
            fountain_encoder_new(msg, msg_len, frag, 60, frag);
        fountain_encoder_part_t p = {0};
        ur_alloc_arm(UR_SITE_ENC_COPY, 1);
        bool refused = e && !fountain_encoder_next_part(e, &p);
        qchkb("fountain encoder copy allocation failure is refused cleanly",
              refused && ur_alloc_hits() == 1 && !p.data && p.data_len == 0);
        ur_alloc_disarm();
        bool recovered = e && fountain_encoder_next_part(e, &p);
        qchkb("fountain encoder recovers after copy allocation failure",
              recovered && p.data && p.data_len == frag);
        fountain_encoder_part_free(&p);
        if (e) fountain_encoder_free(e);
    }

    {   // Allocation-failure injection, aimed at reduce_mixed_by's OWN
        // allocations. Every arm gets a fresh deterministic decoder, so a
        // previous injection cannot complete the shared fixture and silently
        // skip the next one. The site-tagged, fail-on-Nth hook proves the
        // intended allocation was reached, then disarms; reconstruction work
        // on the way into the reduction is deliberately outside the tag.
        //
        // DIFF/1 skips one reduction with no replacement allocated. COPY/1
        // frees its newly calculated key and leaves the entry untouched.
        // COPY/2 lets one entry commit, then fails the next entry's value-index
        // copy: the old half-rewrite bug would corrupt that second entry after
        // XOR/key mutation and the final bytes would no longer match.
        static const struct { int site; int at; } arms[] = {
            { UR_SITE_DIFF, 1 },
            { UR_SITE_COPY, 1 },
            { UR_SITE_COPY, 2 },
        };
        int all_ok = 1;
        for (size_t a = 0; a < sizeof arms / sizeof arms[0]; a++) {
            fountain_encoder_t *redundant =
                fountain_encoder_new(msg, msg_len, frag, 60, frag);
            fountain_encoder_t *raw =
                fountain_encoder_new(msg, msg_len, frag, 0, frag);
            fountain_decoder_t *d = fountain_decoder_new();
            fountain_encoder_part_t p = {0};
            int ok = redundant && raw && d;
            int done = 0;

            // Seed a mixed working set, but stay well short of this fixture's
            // deterministic completion point so the armed allocation remains
            // mandatory rather than becoming conditional on !done.
            for (size_t i = 0; i < 16 && ok && !done; i++) {
                if (!fountain_encoder_next_part(redundant, &p)) { ok = 0; break; }
                fountain_decoder_receive_part(d, &p);
                free(p.data);
                if (fountain_decoder_is_complete(d)) done = 1;
            }

            ur_alloc_arm(arms[a].site, arms[a].at);
            for (size_t i = 0; i < 120 && ok && !done &&
                            ur_alloc_hits() < (unsigned)arms[a].at; i++) {
                if (!fountain_encoder_next_part(redundant, &p)) { ok = 0; break; }
                fountain_decoder_receive_part(d, &p);
                free(p.data);
                if (fountain_decoder_is_complete(d)) done = 1;
            }
            bool hit = ur_alloc_hits() >= (unsigned)arms[a].at;
            qchkb("fountain cap injection reached its target allocation",
                  ok && hit);
            ur_alloc_disarm();
            if (!hit) ok = 0;

            for (size_t i = 0; i < 180 && ok && !done; i++) {
                if (!fountain_encoder_next_part(redundant, &p)) { ok = 0; break; }
                fountain_decoder_receive_part(d, &p);
                free(p.data);
                if (fountain_decoder_is_complete(d)) done = 1;
            }
            for (size_t i = 0; i < 60 && ok && !done; i++) {
                if (!fountain_encoder_next_part(raw, &p)) { ok = 0; break; }
                fountain_decoder_receive_part(d, &p);
                free(p.data);
                if (fountain_decoder_is_complete(d)) done = 1;
            }

            bool bytes_ok = ok && done &&
                fountain_decoder_result_message_len(d) == msg_len &&
                fountain_decoder_result_message(d) &&
                memcmp(fountain_decoder_result_message(d), msg, msg_len) == 0;
            qchkb("fountain cap injected decode completes", ok && done);
            qchkb("fountain cap injected decode bytes match", bytes_ok);
            if (!bytes_ok) all_ok = 0;

            if (d) fountain_decoder_free(d);
            if (raw) fountain_encoder_free(raw);
            if (redundant) fountain_encoder_free(redundant);
        }
        ur_alloc_disarm();
        qchkb("fountain cap all allocation-failure cases held together", all_ok);
    }

done:
    if (e3) fountain_encoder_free(e3);
    if (e2) fountain_encoder_free(e2);
    if (e1) fountain_encoder_free(e1);
    free(msg);
}

int test_qr_transport(const uint8_t *psbt, size_t psbt_len) {
    uint8_t out[QRT_MAX_PSBT];
    size_t on = 0;
    char *b64 = NULL;
    wally_base64_from_bytes(psbt, psbt_len, 0, &b64);
    if (!b64) { printf("FAIL: qr fixture base64\n"); return 1; }

    // ---- static base64 in one QR ----
    qrt_parser_t *p = qrt_parser_new();
    qchkb("qr b64 parser allocs", p != NULL);
    qchki("qr b64 feed rc", qrt_parser_feed(p, b64, strlen(b64)), 0);
    qchkb("qr b64 complete after one", qrt_parser_complete(p));
    qchki("qr b64 format", qrt_parser_format(p), QRT_FMT_STATIC);
    qchki("qr b64 result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
    qchkb("qr b64 bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
    qrt_parser_free(p);

    // ---- static raw binary (compact QR, has NULs) ----
    p = qrt_parser_new();
    qchki("qr raw feed rc", qrt_parser_feed(p, (const char *)psbt, psbt_len), 0);
    qchkb("qr raw complete", qrt_parser_complete(p));
    on = 0;
    qchki("qr raw result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
    qchkb("qr raw bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
    qrt_parser_free(p);

    // ---- junk is refused and doesn't wedge the parser ----
    p = qrt_parser_new();
    qchkb("qr junk rejected", qrt_parser_feed(p, "hello world", 11) != 0);
    qchkb("qr empty rejected", qrt_parser_feed(p, "", 0) != 0);
    qchkb("qr junk leaves incomplete", !qrt_parser_complete(p));
    qchki("qr junk then b64 still works", qrt_parser_feed(p, b64, strlen(b64)), 0);
    qchkb("qr junk then b64 completes", qrt_parser_complete(p));
    qrt_parser_free(p);

    // ---- single-part UR (crafted with cUR directly) ----
    psbt_data_t *pd = psbt_new(psbt, psbt_len);
    size_t cbl = 0;
    uint8_t *cb = pd ? psbt_to_cbor(pd, &cbl) : NULL;
    char *sur = NULL;
    qchkb("qr cUR single-part encodes", cb && ur_encoder_encode_single("crypto-psbt", cb, cbl, &sur));
    if (sur) {
        p = qrt_parser_new();
        qchki("qr UR single feed rc", qrt_parser_feed(p, sur, strlen(sur)), 0);
        qchkb("qr UR single complete", qrt_parser_complete(p));
        qchki("qr UR format", qrt_parser_format(p), QRT_FMT_UR);
        on = 0;
        qchki("qr UR single result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr UR single bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        free(sur);
    }
    if (cb) free(cb);
    if (pd) psbt_free(pd);

    // ---- multipart UR from cUR's PSBT_1 vectors (file order is shuffled) ----
    char *frag[16] = {0};
    int nfrag = read_ur_lines(UR_VECTOR_TXT, frag, 16);
    size_t vlen = 0;
    uint8_t *vbin = read_bin(UR_VECTOR_BIN, &vlen);
    qchkb("qr vector files load", nfrag == 8 && vbin != NULL);
    if (nfrag > 0 && vbin) {
        p = qrt_parser_new();
        qchki("qr UR multi first feed rc", qrt_parser_feed(p, frag[0], strlen(frag[0])), 0);
        qchki("qr UR multi total known", qrt_parser_total(p), 8);
        qchki("qr UR multi seen 1", qrt_parser_seen(p), 1);
        qchki("qr UR multi dup tolerated", qrt_parser_feed(p, frag[0], strlen(frag[0])), 0);
        qchkb("qr UR multi not complete early", !qrt_parser_complete(p));
        // format lock: a pMofN part must be refused mid-UR
        qchkb("qr UR multi refuses pMofN", qrt_parser_feed(p, "p1of2 cHNidP", 12) != 0);
        for (int i = 1; i < nfrag; i++)
            qrt_parser_feed(p, frag[i], strlen(frag[i]));
        qchkb("qr UR multi completes", qrt_parser_complete(p));
        on = 0;
        qchki("qr UR multi result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr UR multi bytes match vector", on == vlen && memcmp(out, vbin, on) == 0);
        qrt_parser_free(p);
    }
    for (int i = 0; i < nfrag; i++) free(frag[i]);
    if (vbin) free(vbin);

    // ---- pMofN, out of order + duplicate ----
    {
        size_t bl = strlen(b64), half = bl / 2;
        char q1[4200], q2[4200];
        snprintf(q1, sizeof q1, "p1of2 %.*s", (int)half, b64);
        snprintf(q2, sizeof q2, "p2of2 %s", b64 + half);
        p = qrt_parser_new();
        qchki("qr pMofN feed 2 first rc", qrt_parser_feed(p, q2, strlen(q2)), 0);
        qchki("qr pMofN total", qrt_parser_total(p), 2);
        qchki("qr pMofN seen 1", qrt_parser_seen(p), 1);
        qchkb("qr pMofN not complete", !qrt_parser_complete(p));
        qchki("qr pMofN dup rc", qrt_parser_feed(p, q2, strlen(q2)), 0);
        qchki("qr pMofN seen still 1", qrt_parser_seen(p), 1);
        qchki("qr pMofN feed 1 rc", qrt_parser_feed(p, q1, strlen(q1)), 0);
        qchkb("qr pMofN complete", qrt_parser_complete(p));
        qchki("qr pMofN format", qrt_parser_format(p), QRT_FMT_PMOFN);
        on = 0;
        qchki("qr pMofN result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr pMofN bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
    }

    // ---- result cap guard ----
    p = qrt_parser_new();
    qrt_parser_feed(p, b64, strlen(b64));
    qchkb("qr result refuses tiny cap", qrt_parser_result(p, out, 16, &on) != 0);
    qrt_parser_free(p);

    // ---- encoder: UR fountain roundtrip through our own parser ----
    {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_UR, psbt, psbt_len);
        qchkb("qr UR encoder allocs", e != NULL);
        int nparts = qrt_encoder_parts(e);
        qchkb("qr UR encoder is multi-part", nparts >= 2);
        p = qrt_parser_new();
        char part[600];
        int fed = 0, gen_fail = 0, prefix_ok = 1;
        while (p && e && !qrt_parser_complete(p) && fed < nparts * 4 + 8) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) { gen_fail = 1; break; }
            // cUR emits uppercase on purpose: QR alphanumeric mode = smaller QR
            if (strncmp(part, "UR:CRYPTO-PSBT/", 15) != 0) prefix_ok = 0;
            qrt_parser_feed(p, part, strlen(part));
            fed++;
        }
        qchkb("qr UR parts generate", !gen_fail && fed > 0);
        qchkb("qr UR parts carry UR:CRYPTO-PSBT/", prefix_ok);
        qchkb("qr UR roundtrip completes", p && qrt_parser_complete(p));
        on = 0;
        qchki("qr UR roundtrip result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr UR roundtrip bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        qrt_encoder_free(e);
    }

    // ---- encoder: pMofN roundtrip ----
    {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_PMOFN, psbt, psbt_len);
        qchkb("qr pMofN encoder allocs", e != NULL);
        int nparts = qrt_encoder_parts(e);
        qchkb("qr pMofN encoder is multi-part", nparts >= 2);
        p = qrt_parser_new();
        char part[600];
        int gen_fail = 0, prefix_ok = 1;
        for (int i = 0; e && i < nparts; i++) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) { gen_fail = 1; break; }
            char want[16];
            snprintf(want, sizeof want, "p%dof%d ", i + 1, nparts);
            if (strncmp(part, want, strlen(want)) != 0) prefix_ok = 0;
            qrt_parser_feed(p, part, strlen(part));
        }
        qchkb("qr pMofN parts generate", !gen_fail);
        qchkb("qr pMofN prefixes correct", prefix_ok);
        qchkb("qr pMofN roundtrip completes", p && qrt_parser_complete(p));
        on = 0;
        qchki("qr pMofN roundtrip result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr pMofN roundtrip bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        qrt_encoder_free(e);
    }

    // ---- encoder: easy-scan fragment override still roundtrips (the SIGNED
    // screen's EASY SCAN mode re-encodes at UR 60 / pMofN 50) ----
    {
        qrt_encoder_t *e = qrt_encoder_new_frag(QRT_FMT_UR, psbt, psbt_len, 60);
        qrt_encoder_t *d = qrt_encoder_new(QRT_FMT_UR, psbt, psbt_len);
        qchkb("qr ez UR encoder allocs", e != NULL);
        qchkb("qr ez UR has more, smaller parts",
              e && d && qrt_encoder_parts(e) > qrt_encoder_parts(d));
        p = qrt_parser_new();
        char part[600];
        int fed = 0, nparts = e ? qrt_encoder_parts(e) : 0;
        while (p && e && !qrt_parser_complete(p) && fed < nparts * 4 + 8) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) break;
            qrt_parser_feed(p, part, strlen(part));
            fed++;
        }
        qchkb("qr ez UR roundtrip completes", p && qrt_parser_complete(p));
        on = 0;
        qchki("qr ez UR roundtrip result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr ez UR roundtrip bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        qrt_encoder_free(e);
        qrt_encoder_free(d);
    }
    {
        qrt_encoder_t *e = qrt_encoder_new_frag(QRT_FMT_PMOFN, psbt, psbt_len, 50);
        qchkb("qr ez pMofN encoder allocs", e != NULL);
        p = qrt_parser_new();
        char part[600];
        int nparts = e ? qrt_encoder_parts(e) : 0;
        for (int i = 0; e && i < nparts; i++) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) break;
            qrt_parser_feed(p, part, strlen(part));
        }
        qchkb("qr ez pMofN roundtrip completes", p && qrt_parser_complete(p));
        on = 0;
        qchki("qr ez pMofN roundtrip result rc", qrt_parser_result(p, out, sizeof out, &on), 0);
        qchkb("qr ez pMofN roundtrip bytes match", on == psbt_len && memcmp(out, psbt, on) == 0);
        qrt_parser_free(p);
        qrt_encoder_free(e);
    }

    // ---- encoder: static equals libwally's base64 (validates local encoder) ----
    {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_STATIC, psbt, psbt_len);
        qchkb("qr static encoder allocs", e != NULL);
        qchki("qr static parts", qrt_encoder_parts(e), 1);
        char part[6000];
        qchki("qr static next rc", qrt_encoder_next(e, part, sizeof part), 0);
        qchkb("qr static matches wally base64", strcmp(part, b64) == 0);
        qrt_encoder_free(e);
    }

    qr_test_alpha_bounds();
    qr_test_version_tables();
    qr_test_hostile_header();
    qr_test_pmofn_bounds();
    qr_test_prng_range();
    qr_test_fountain_cap_churn();

    wally_free_string(b64);
    return qfails;
}
