// See kiss_pqsig.h.
#include "kiss_pqsig.h"

#include <string.h>

#include "slh_dsa.h"
#include "pq_hw_sha.h"
#include "pq_release_pubkey.h"

// ---- the key ----

// The key in force. Only ever the compiled in one on the device; the desktop
// suite points it at a throwaway keypair so it can build a real trailer and
// watch the check accept and refuse it. Same shape and the same reason as
// kiss_fw_test_set_available: a gate that can only ever see the "no key" branch
// has not seen the gate.
static const uint8_t *s_pk = PQ_RELEASE_PUBKEY;

#ifndef ESP_PLATFORM
void kiss_pqsig_test_set_pubkey(const uint8_t *pk)
{
    s_pk = pk ? pk : PQ_RELEASE_PUBKEY;
}
#endif

bool kiss_pqsig_available(void)
{
    // A key of all zeroes is the placeholder, not a key.
    uint8_t any = 0;
    for (size_t i = 0; i < KISS_PQSIG_PK_LEN; i++) any |= s_pk[i];
    return any != 0;
}

// ---- splitting the trailer off a stream ----

void kiss_pqsig_stream_init(kiss_pqsig_stream_t *s)
{
    memset(s, 0, sizeof *s);
    sha2_256_init(&s->sha);
}

// Release n bytes to the writer and fold them into the digest. The two always
// happen together and in that order, so the digest can only ever cover bytes
// that actually reached flash.
static int emit(kiss_pqsig_stream_t *s, const uint8_t *p, size_t n,
                kiss_pqsig_sink_fn sink, void *ud)
{
    if (n == 0) return 0;
    int rc = sink ? sink(p, n, ud) : 0;
    if (rc != 0) { s->sink_rc = rc; return rc; }
    sha2_256_update(&s->sha, p, n);
    s->image_len += n;
    return 0;
}

int kiss_pqsig_stream_feed(kiss_pqsig_stream_t *s, const uint8_t *buf, size_t len,
                           kiss_pqsig_sink_fn sink, void *ud)
{
    if (s->sink_rc != 0) return s->sink_rc;
    if (len == 0) return 0;

    const size_t cap = KISS_PQSIG_TRAILER_LEN;

    // Everything still within a trailer's length of the end has to stay in hand,
    // because nothing has arrived yet to prove it is not the trailer.
    if (s->tail_len + len <= cap) {
        memcpy(s->tail + s->tail_len, buf, len);
        s->tail_len += len;
        return 0;
    }

    size_t excess = s->tail_len + len - cap;   // bytes now known to be image

    // The oldest bytes go first, and they are the ones being held.
    size_t from_tail = excess < s->tail_len ? excess : s->tail_len;
    if (from_tail) {
        int rc = emit(s, s->tail, from_tail, sink, ud);
        if (rc != 0) return rc;
        s->tail_len -= from_tail;
        memmove(s->tail, s->tail + from_tail, s->tail_len);
        excess -= from_tail;
    }
    // Then whatever of this chunk is old enough to be past the window.
    if (excess) {
        int rc = emit(s, buf, excess, sink, ud);
        if (rc != 0) return rc;
    }

    // What is left of the chunk refills the window, which is now exactly full.
    memcpy(s->tail + s->tail_len, buf + excess, len - excess);
    s->tail_len += len - excess;
    return 0;
}

int kiss_pqsig_stream_end(kiss_pqsig_stream_t *s, uint8_t digest[32],
                          const uint8_t **trailer, size_t *trailer_len)
{
    if (s->sink_rc != 0) return s->sink_rc;
    // A file shorter than a trailer never filled the window, so there is no
    // image under it and nothing to check. Refuse rather than hash the fragment.
    if (s->tail_len != KISS_PQSIG_TRAILER_LEN) return KISS_PQSIG_ERR_LENGTH;
    sha2_256_final(&s->sha, digest);
    if (trailer) *trailer = s->tail;
    if (trailer_len) *trailer_len = s->tail_len;
    return KISS_PQSIG_OK;
}

uint64_t kiss_pqsig_stream_image_len(const kiss_pqsig_stream_t *s)
{
    return s->image_len;
}

// ---- the check ----

int kiss_pqsig_check(const uint8_t digest[32], const uint8_t *trailer, size_t trailer_len)
{
    if (!kiss_pqsig_available()) return KISS_PQSIG_ERR_NO_KEY;
    if (!trailer || trailer_len != KISS_PQSIG_TRAILER_LEN) return KISS_PQSIG_ERR_LENGTH;
    if (memcmp(trailer, KISS_PQSIG_MAGIC, KISS_PQSIG_MAGIC_LEN) != 0)
        return KISS_PQSIG_ERR_MAGIC;

    unsigned scheme = (unsigned)trailer[4] | ((unsigned)trailer[5] << 8);
    unsigned siglen = (unsigned)trailer[6] | ((unsigned)trailer[7] << 8);
    if (scheme != KISS_PQSIG_SCHEME_SLH_DSA_SHA2_128S) return KISS_PQSIG_ERR_SCHEME;
    if (siglen != KISS_PQSIG_SIG_LEN) return KISS_PQSIG_ERR_LENGTH;

    // The slack after the signature carries nothing and has to stay that way.
    // 328 bytes of anything, riding inside a signed release and reaching flash,
    // is a channel whether or not anybody meant it as one.
    for (size_t i = KISS_PQSIG_HDR_LEN + siglen; i < trailer_len; i++)
        if (trailer[i] != 0) return KISS_PQSIG_ERR_PADDING;

    pq_hw_sha_begin();
    int ok = slh_verify(digest, 32,
                        trailer + KISS_PQSIG_HDR_LEN, siglen,
                        (const uint8_t *)KISS_PQSIG_CTX, sizeof KISS_PQSIG_CTX - 1,
                        s_pk, &slh_dsa_sha2_128s);
    pq_hw_sha_end();
    return ok == 1 ? KISS_PQSIG_OK : KISS_PQSIG_ERR_VERIFY;
}

// ---- selftest ----

#include "pq_selftest_vectors.h"

int kiss_pqsig_selftest(void)
{
    // Sizes first. If the vendored parameter set ever moved, everything below
    // would still "pass" against a signature of a different shape.
    if (slh_sig_sz(&slh_dsa_sha2_128s) != KISS_PQSIG_SIG_LEN) return 1;
    if (slh_pk_sz(&slh_dsa_sha2_128s) != KISS_PQSIG_PK_LEN) return 2;

    pq_hw_sha_begin();
    int ok = slh_verify(PQ_SELFTEST_MSG, PQ_SELFTEST_MSG_LEN,
                        PQ_SELFTEST_SIG, PQ_SELFTEST_SIG_LEN,
                        PQ_SELFTEST_CTX, PQ_SELFTEST_CTX_LEN,
                        PQ_SELFTEST_PK, &slh_dsa_sha2_128s);
    pq_hw_sha_end();
    if (ok != 1) return 3;

    // And that it is checking rather than agreeing. A verifier wired to a dead
    // SHA engine returns all zero digests, which compare equal to each other --
    // so the accepting case alone cannot tell a working engine from a stopped
    // one. This flipped bit can.
    static uint8_t bad[PQ_SELFTEST_SIG_LEN];
    memcpy(bad, PQ_SELFTEST_SIG, sizeof bad);
    bad[sizeof bad / 2] ^= 0x01;
    pq_hw_sha_begin();
    ok = slh_verify(PQ_SELFTEST_MSG, PQ_SELFTEST_MSG_LEN,
                    bad, sizeof bad,
                    PQ_SELFTEST_CTX, PQ_SELFTEST_CTX_LEN,
                    PQ_SELFTEST_PK, &slh_dsa_sha2_128s);
    pq_hw_sha_end();
    if (ok != 0) return 4;

    return 0;
}
