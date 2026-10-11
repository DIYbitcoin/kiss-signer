// SeedKeeper: list a card's entries and load one as words. See
// kiss_seedkeeper.h. The card session itself is kiss_satocard.
#include "kiss_seedkeeper.h"

#include <string.h>

#include <wally_bip39.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_seed.h"
#include "kiss_wipe.h"

#define SK_CLA 0xB0
#define INS_EXPORT_SECRET 0xA2
#define INS_LIST_HEADERS  0xA6
#define OP_INIT    0x01
#define OP_PROCESS 0x02
#define SW_NOT_FOUND    0x9C08
#define SW_SEQUENCE_END 0x9C12

#define SK_HEADER_LEN 13           // type .. label_size, before the label
#define SK_EXPORT_MAX 512          // a mnemonic, a passphrase, a descriptor

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ---- list ----
static void parse_header(const uint8_t *r, kiss_sk_header *h)
{
    h->id = be16(r);
    h->type = r[2];
    h->origin = r[3];
    h->export_ctrl = r[4] & 0x03;
    memcpy(h->fingerprint, r + 8, 4);
    h->subtype = r[12];
}

// The label is text the owner typed on another device. Keep printable ASCII
// and show anything else as '?': this screen's fonts cannot be trusted to
// carry arbitrary UTF-8, and a label must never be able to fake the UI.
static void copy_label(char *out, const uint8_t *p, size_t n)
{
    if (n > SK_LABEL_MAX)
        n = SK_LABEL_MAX;
    for (size_t i = 0; i < n; i++)
        out[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
    out[n] = 0;
}

int kiss_sk_list(kiss_satocard *c, kiss_sk_header *out, int max, int *count)
{
    *count = 0;
    uint8_t resp[2 + SK_HEADER_LEN + 255];
    for (int i = 0; ; i++) {
        if (i > 255)
            return SC_ERR_PROTOCOL;   // a card that never says "end"
        uint8_t apdu[4] = { SK_CLA, INS_LIST_HEADERS, 0x00,
                            (uint8_t)(i == 0 ? OP_INIT : OP_PROCESS) };
        size_t rlen;
        int rc = kiss_satocard_xfer(c, apdu, sizeof apdu, resp, sizeof resp, &rlen);
        if (rc == SC_ERR_SW && c->last_sw == SW_SEQUENCE_END)
            return SC_OK;
        if (rc != SC_OK)
            return rc;
        if (rlen < 2 + SK_HEADER_LEN ||
            rlen != 2u + SK_HEADER_LEN + resp[2 + SK_HEADER_LEN - 1])
            return SC_ERR_PROTOCOL;
        if (*count >= max)
            continue;             // keep walking so the card's cursor ends
        kiss_sk_header *h = &out[(*count)++];
        memset(h, 0, sizeof *h);
        parse_header(resp, h);
        copy_label(h->label, resp + 2 + SK_HEADER_LEN, resp[2 + SK_HEADER_LEN - 1]);
    }
}

bool kiss_sk_loadable(const kiss_sk_header *h)
{
    if (h->export_ctrl != SK_EXPORT_ALLOWED)
        return false;
    if (h->type == SK_TYPE_BIP39)
        return true;
    return h->type == SK_TYPE_MASTERSEED && h->subtype == SK_SUBTYPE_BIP39;
}

// ---- decode ----
static bool ascii_printable(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] >= 0x7F)
            return false;
    return true;
}

static int count_words(const char *w)
{
    int n = 0;
    for (const char *p = w; *p; p++)
        if (*p != ' ' && (p == w || p[-1] == ' '))
            n++;
    return n;
}

int kiss_sk_decode(uint8_t type, uint8_t subtype,
                   const uint8_t *data, size_t len,
                   char *words, size_t words_cap,
                   char *passphrase, size_t pass_cap)
{
    if (!words || !passphrase || words_cap < 2 || pass_cap < 1)
        return SC_ERR_FORMAT;
    words[0] = 0;
    passphrase[0] = 0;
    int rc = SC_ERR_FORMAT;
    const uint8_t *pass = NULL;
    size_t pass_len = 0;
    const uint8_t *seed = NULL;
    size_t seed_len = 0;

    if (type == SK_TYPE_BIP39) {
        // [m_len | mnemonic | p_len | passphrase | (desc_len(2) | descriptor)]
        if (len < 1 || (size_t)data[0] + 1 > len)
            goto out;
        size_t m = data[0];
        if (m == 0 || m >= words_cap || !ascii_printable(data + 1, m))
            goto out;
        memcpy(words, data + 1, m);
        words[m] = 0;
        if (m + 1 < len) {
            pass_len = data[m + 1];
            if (m + 2 + pass_len > len)
                goto out;
            pass = data + m + 2;
        }
    } else if (type == SK_TYPE_MASTERSEED && subtype == SK_SUBTYPE_BIP39) {
        // [ms_len | ms | wordlist | ent_len | entropy | p_len | passphrase]
        if (len < 1 || (size_t)data[0] + 3 > len)
            goto out;
        seed_len = data[0];
        seed = data + 1;
        size_t o = 1 + seed_len;
        if (data[o] != 0x00)          // only the English wordlist
            goto out;
        size_t e = data[o + 1];
        if ((e != 16 && e != 32) || o + 2 + e > len)
            goto out;
        if (kiss_seed_from_entropy(data + o + 2, e, words, words_cap) != 0)
            goto out;
        o += 2 + e;
        if (o < len) {
            pass_len = data[o];
            if (o + 1 + pass_len > len)
                goto out;
            pass = data + o + 1;
        }
    } else {
        goto out;
    }

    int nw = count_words(words);
    if ((nw != 12 && nw != 24) || kiss_seed_validate(words) != 0)
        goto out;

    // A passphrase this device cannot type is refused rather than passed on:
    // BIP39 wants NFKD, which this firmware does not implement, so a
    // non-ASCII one could open a different wallet than the card's owner made.
    if (pass_len) {
        if (pass_len >= pass_cap || !ascii_printable(pass, pass_len))
            goto out;
        memcpy(passphrase, pass, pass_len);
        passphrase[pass_len] = 0;
    }

    // When the entry also stores the BIP39 seed, the words and passphrase
    // must reproduce it: a mismatch means a corrupt or mis-written entry,
    // and opening it would show a wallet that is not the one on the card.
    if (seed) {
        uint8_t check[64];
        size_t n = 0;
        if (seed_len != 64 ||
            bip39_mnemonic_to_seed(words, passphrase, check, sizeof check, &n) != WALLY_OK ||
            n != 64 || memcmp(check, seed, 64) != 0) {
            kiss_wipe(check, sizeof check);
            goto out;
        }
        kiss_wipe(check, sizeof check);
    }
    rc = SC_OK;
out:
    if (rc != SC_OK) {
        kiss_wipe(words, words_cap);
        kiss_wipe(passphrase, pass_cap);
    }
    return rc;
}

// ---- export ----
// INIT returns [id(2) | header(13) | label]; each PROCESS returns
// [len(2) | chunk], and the last one adds [sig_len(2) | sig]: the
// authentikey's signature over id, header, label and the whole secret.
int kiss_sk_load(kiss_satocard *c, uint16_t id,
                 char *words, size_t words_cap,
                 char *passphrase, size_t pass_cap)
{
    if (words && words_cap) words[0] = 0;
    if (passphrase && pass_cap) passphrase[0] = 0;

    uint8_t signed_buf[2 + SK_HEADER_LEN + 255 + SK_EXPORT_MAX];
    uint8_t resp[300];
    size_t rlen, hlen, total = 0;
    int rc;

    uint8_t init[7] = { SK_CLA, INS_EXPORT_SECRET, SK_EXPORT_ALLOWED, OP_INIT,
                        2, (uint8_t)(id >> 8), (uint8_t)id };
    rc = kiss_satocard_xfer(c, init, sizeof init, resp, sizeof resp, &rlen);
    if (rc == SC_ERR_SW && c->last_sw == SW_NOT_FOUND)
        rc = SC_ERR_FORMAT;
    if (rc != SC_OK)
        goto out;
    if (rlen < 2 + SK_HEADER_LEN || be16(resp) != id ||
        rlen != 2u + SK_HEADER_LEN + resp[2 + SK_HEADER_LEN - 1]) {
        rc = SC_ERR_PROTOCOL;
        goto out;
    }
    uint8_t type = resp[2], subtype = resp[2 + 10];
    hlen = rlen;
    memcpy(signed_buf, resp, hlen);

    const uint8_t *sig = NULL;
    size_t sig_len = 0;
    for (int guard = 0; !sig; guard++) {
        uint8_t apdu[7] = { SK_CLA, INS_EXPORT_SECRET, SK_EXPORT_ALLOWED, OP_PROCESS,
                            2, (uint8_t)(id >> 8), (uint8_t)id };
        rc = kiss_satocard_xfer(c, apdu, sizeof apdu, resp, sizeof resp, &rlen);
        if (rc != SC_OK)
            goto out;
        if (rlen < 2 || guard > 8) { rc = SC_ERR_PROTOCOL; goto out; }
        size_t n = be16(resp);
        if (2 + n > rlen || total + n > SK_EXPORT_MAX) { rc = SC_ERR_PROTOCOL; goto out; }
        memcpy(signed_buf + hlen + total, resp + 2, n);
        total += n;
        if (rlen > 2 + n) {
            size_t o = 2 + n;
            if (o + 2 > rlen) { rc = SC_ERR_PROTOCOL; goto out; }
            sig_len = be16(resp + o);
            if (sig_len == 0 || o + 2 + sig_len != rlen) { rc = SC_ERR_PROTOCOL; goto out; }
            sig = resp + o + 2;
        }
    }

    uint8_t h[32];
    (void)wally_sha256(signed_buf, hlen + total, h, 32);
    if (!kiss_satocard_sig_ok(sig, sig_len, h, c->authentikey)) {
        rc = SC_ERR_NOT_GENUINE;
        goto out;
    }
    rc = kiss_sk_decode(type, subtype, signed_buf + hlen, total,
                        words, words_cap, passphrase, pass_cap);
out:
    kiss_wipe(signed_buf, sizeof signed_buf);
    kiss_wipe(resp, sizeof resp);
    return rc;
}
