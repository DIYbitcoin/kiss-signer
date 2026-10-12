// CCID reader protocol and T=1. See kiss_ccid.h.
#include "kiss_ccid.h"

#include <string.h>

#include "kiss_wipe.h"

// PC_to_RDR and RDR_to_PC message types (CCID 1.1, section 6)
#define PC_SET_PARAMETERS 0x61
#define PC_POWER_ON       0x62
#define PC_POWER_OFF      0x63
#define PC_SLOT_STATUS    0x65
#define PC_XFR_BLOCK      0x6F
#define RDR_DATA_BLOCK    0x80
#define RDR_SLOT_STATUS   0x81
#define RDR_PARAMETERS    0x82

#define FEAT_TPDU      0x00010000u
#define FEAT_SHORT     0x00020000u
#define FEAT_EXTENDED  0x00040000u

#define ICC_ABSENT      2
#define CMD_FAILED      1
#define CMD_TIME_EXT    2

#define TIMEOUT_MS      5000
#define MAX_TIME_EXT    60          // how many "still working" a command may take

// T=1 (ISO/IEC 7816-3, 11.6)
#define T1_INF_MAX      254
#define T1_S_IFS_REQ    0xC1
#define T1_S_IFS_RESP   0xE1
#define T1_S_WTX_REQ    0xC3
#define T1_S_WTX_RESP   0xE3
#define T1_MAX_STEPS    64

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ---- configuration descriptor ----
int kiss_ccid_parse_config(const uint8_t *cfg, size_t len, kiss_ccid_reader *out)
{
    memset(out, 0, sizeof *out);
    if (!cfg || len < 9 || cfg[1] != 0x02)
        return -1;
    size_t total = (size_t)cfg[2] | (size_t)cfg[3] << 8;
    if (total > len)
        total = len;
    bool in_ccid = false, have_intf = false, have_func = false;
    for (size_t off = 0; off + 2 <= total; ) {
        const uint8_t *d = cfg + off;
        uint8_t dl = d[0], type = d[1];
        if (dl < 2 || off + dl > total)
            break;
        if (type == 0x04 && dl >= 9) {                 // interface
            if (have_intf && in_ccid)
                break;                                 // the next interface: done
            in_ccid = d[5] == 0x0B;                    // smart card class
            if (in_ccid) {
                have_intf = true;
                out->interface_num = d[2];
                out->alt_setting = d[3];
            }
        } else if (in_ccid && type == 0x21 && dl >= 54) {   // CCID class descriptor
            have_func = true;
            out->protocols = le32(d + 6);
            out->max_ifsd = le32(d + 28);
            out->features = le32(d + 40);
            out->max_msg = le32(d + 44);
        } else if (in_ccid && type == 0x05 && dl >= 7) {    // endpoint
            uint8_t addr = d[2], attr = d[3] & 0x03;
            uint16_t mps = (uint16_t)(d[4] | d[5] << 8);
            if (attr == 0x02 && (addr & 0x80) && !out->bulk_in) {
                out->bulk_in = addr;
                out->bulk_in_mps = mps;
            } else if (attr == 0x02 && !(addr & 0x80) && !out->bulk_out) {
                out->bulk_out = addr;
                out->bulk_out_mps = mps;
            }
        }
        off += dl;
    }
    if (!have_intf || !have_func || !out->bulk_in || !out->bulk_out)
        return -1;
    return 0;
}

void kiss_ccid_init(kiss_ccid *c, const kiss_ccid_reader *rd,
                    uint16_t vid, uint16_t pid,
                    kiss_ccid_io_fn io, void *io_ctx)
{
    memset(c, 0, sizeof *c);
    c->rd = *rd;
    c->io = io;
    c->io_ctx = io_ctx;
    bool apdu_level = (rd->features & (FEAT_SHORT | FEAT_EXTENDED)) != 0;
    // The ACS ACR39U-NF PocketMate II reports no exchange level at all and
    // behaves as a TPDU reader (found by KernSigner on the same chip).
    bool acr39u_nf = vid == 0x072F && pid == 0xB100;
    c->tpdu = !apdu_level && ((rd->features & FEAT_TPDU) || acr39u_nf);
}

// ---- one CCID command and its answer ----
// Sends msg (its bSeq is set here) and reads until the reader's answer for
// that sequence number arrives, sitting through time extensions. The answer
// is checked: its type, its length against the bytes that came, slot 0, the
// sequence number, and the command status.
static int transact(kiss_ccid *c, uint8_t *msg, size_t msg_len, uint8_t want,
                    uint8_t *in, size_t in_cap, size_t *in_len)
{
    uint8_t seq = c->seq++;
    msg[5] = 0;
    msg[6] = seq;
    const uint8_t *out = msg;
    size_t out_len = msg_len;
    for (int ext = 0; ext <= MAX_TIME_EXT; ext++) {
        *in_len = 0;
        if (c->io(c->io_ctx, out, out_len, in, in_cap, in_len, TIMEOUT_MS) != 0)
            return CCID_ERR_IO;
        out = NULL;
        out_len = 0;                   // later rounds only read
        if (*in_len < 10 || *in_len > in_cap || le32(in + 1) != *in_len - 10)
            return CCID_ERR_READER;
        if (in[5] != 0 || in[6] != seq)
            return CCID_ERR_READER;
        uint8_t cmd = in[7] >> 6, icc = in[7] & 0x03;
        if (cmd == CMD_TIME_EXT)
            continue;                  // the card is still working
        if (in[0] != want)
            return CCID_ERR_READER;
        if (cmd == CMD_FAILED)
            return icc == ICC_ABSENT ? CCID_ERR_NOCARD : CCID_ERR_CARD;
        if (icc == ICC_ABSENT)
            return CCID_ERR_NOCARD;
        return CCID_OK;
    }
    return CCID_ERR_CARD;
}

static void header(uint8_t *m, uint8_t type, uint32_t len)
{
    memset(m, 0, 10);
    m[0] = type;
    put_le32(m + 1, len);
}

int kiss_ccid_slot_status(kiss_ccid *c)
{
    uint8_t m[10], in[64];
    size_t n;
    header(m, PC_SLOT_STATUS, 0);
    return transact(c, m, sizeof m, RDR_SLOT_STATUS, in, sizeof in, &n);
}

void kiss_ccid_power_off(kiss_ccid *c)
{
    uint8_t m[10], in[64];
    size_t n;
    header(m, PC_POWER_OFF, 0);
    (void)transact(c, m, sizeof m, RDR_SLOT_STATUS, in, sizeof in, &n);
    c->atr_len = 0;
}

// ---- ATR ----
int kiss_ccid_parse_atr(kiss_ccid *c, const uint8_t *atr, size_t len)
{
    c->t1 = false;
    c->ta1 = 0x11;
    c->tc1 = 0x00;
    c->bwi_cwi = 0x4D;
    c->ifsc = 0x20;
    if (len < 2 || (atr[0] != 0x3B && atr[0] != 0x3F))
        return -1;
    c->tccks = atr[0] == 0x3F ? 0x12 : 0x10;   // inverse convention, LRC
    size_t i = 2;
    uint8_t y = atr[1] >> 4, k = atr[1] & 0x0F, proto = 0;
    bool tck = false;
    for (int group = 1; ; group++) {
        uint8_t ta = 0, tb = 0, tc = 0, td = 0;
        bool hta = y & 1, htb = y & 2, htc = y & 4, htd = y & 8;
        if (i + hta + htb + htc + htd > len)
            return -1;
        if (hta) ta = atr[i++];
        if (htb) tb = atr[i++];
        if (htc) tc = atr[i++];
        if (htd) td = atr[i++];
        if (group == 1) {
            if (hta) c->ta1 = ta;
            if (htc) c->tc1 = tc;
        }
        // the bytes after a TD naming T=1 (group 3 on) are T=1's own
        if (group >= 3 && proto == 1) {
            if (hta && ta != 0x00 && ta != 0xFF) c->ifsc = ta;
            if (htb) c->bwi_cwi = tb;
            if (htc) {
                if (tc & 0x01)
                    return -1;                       // CRC: this driver does LRC
                c->tccks = (uint8_t)((c->tccks & 0x02) | 0x10);
            }
        }
        if (!htd)
            break;
        proto = td & 0x0F;
        if (proto == 1)
            c->t1 = true;
        if (proto != 0)
            tck = true;
        y = td >> 4;
        if (group > 8)
            return -1;
    }
    if (i + k + (tck ? 1 : 0) > len)
        return -1;
    if (tck) {
        uint8_t x = 0;
        for (size_t j = 1; j < i + k + 1; j++)
            x ^= atr[j];
        if (x != 0)
            return -1;
    }
    return 0;
}

int kiss_ccid_power_on(kiss_ccid *c)
{
    uint8_t m[10], in[10 + CCID_ATR_MAX + 8];
    size_t n;
    c->atr_len = 0;
    c->ns = 0;
    c->nr = 0;
    header(m, PC_POWER_ON, 0);         // bPowerSelect 0: the reader picks
    int rc = transact(c, m, sizeof m, RDR_DATA_BLOCK, in, sizeof in, &n);
    if (rc != CCID_OK)
        return rc;
    size_t al = n - 10;
    if (al == 0 || al > CCID_ATR_MAX)
        return CCID_ERR_CARD;
    memcpy(c->atr, in + 10, al);
    c->atr_len = al;
    if (kiss_ccid_parse_atr(c, c->atr, al) != 0)
        return CCID_ERR_CARD;
    if (!c->tpdu)
        return CCID_OK;
    if (!c->t1)
        return CCID_ERR_PROTO;         // T=0 framing is not implemented

    // SetParameters for T=1 (CCID 6.1.7): the reader times and checks the
    // blocks; some readers configure themselves from the ATR and refuse
    // this, which is fine, so its answer is not a verdict.
    uint8_t ifsc = c->ifsc;
    if (c->rd.max_ifsd && ifsc > c->rd.max_ifsd)
        ifsc = (uint8_t)c->rd.max_ifsd;
    uint8_t sp[10 + 7];
    header(sp, PC_SET_PARAMETERS, 7);
    sp[7] = 0x01;                       // bProtocolNum: T=1
    sp[10] = c->ta1;
    sp[11] = c->tccks;
    sp[12] = c->tc1;
    sp[13] = c->bwi_cwi;
    sp[14] = 0x00;                      // clock stop not allowed
    sp[15] = ifsc;
    sp[16] = 0x00;                      // NAD
    rc = transact(c, sp, sizeof sp, RDR_PARAMETERS, in, sizeof in, &n);
    if (rc == CCID_ERR_IO || rc == CCID_ERR_NOCARD)
        return rc;
    return CCID_OK;
}

// ---- T=1 ----
static uint8_t lrc(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++)
        x ^= p[i];
    return x;
}

static bool is_i(uint8_t pcb) { return (pcb & 0x80) == 0; }
static bool is_r(uint8_t pcb) { return (pcb & 0xC0) == 0x80; }

// Send one block in an XfrBlock and take the card's block back, checked.
// bwi is the wait-time multiplier for this exchange (0 = default).
static int t1_block(kiss_ccid *c, const uint8_t *blk, size_t blen, uint8_t bwi,
                    uint8_t *rb, size_t *rl)
{
    uint8_t m[10 + 4 + T1_INF_MAX], in[10 + 4 + T1_INF_MAX + 8];
    size_t n;
    header(m, PC_XFR_BLOCK, (uint32_t)blen);
    m[7] = bwi;
    memcpy(m + 10, blk, blen);
    int rc = transact(c, m, 10 + blen, RDR_DATA_BLOCK, in, sizeof in, &n);
    kiss_wipe(m, sizeof m);
    if (rc != CCID_OK)
        return rc;
    size_t bl = n - 10;
    const uint8_t *b = in + 10;
    if (bl < 4 || bl != 4u + b[2] || b[0] != 0x00 || lrc(b, bl) != 0) {
        kiss_wipe(in, sizeof in);
        return CCID_ERR_CARD;
    }
    memcpy(rb, b, bl);
    *rl = bl;
    kiss_wipe(in, sizeof in);
    return CCID_OK;
}

static size_t build(uint8_t *out, uint8_t pcb, const uint8_t *inf, size_t inf_len)
{
    out[0] = 0x00;
    out[1] = pcb;
    out[2] = (uint8_t)inf_len;
    if (inf_len)
        memcpy(out + 3, inf, inf_len);
    out[3 + inf_len] = lrc(out, 3 + inf_len);
    return 4 + inf_len;
}

// Send blk, then answer the card's supervisory requests (more time, a new
// block size) until it says something else, which lands in rb.
static int t1_exchange(kiss_ccid *c, const uint8_t *blk, size_t blen,
                       uint8_t *rb, size_t *rl, int *steps)
{
    uint8_t s[5];
    int rc = t1_block(c, blk, blen, 0, rb, rl);
    while (rc == CCID_OK && (rb[1] == T1_S_WTX_REQ || rb[1] == T1_S_IFS_REQ)) {
        if (++*steps > T1_MAX_STEPS || rb[2] != 1)
            return CCID_ERR_CARD;
        uint8_t v = rb[3];
        if (rb[1] == T1_S_IFS_REQ) {
            if (v == 0x00 || v == 0xFF)
                return CCID_ERR_CARD;
            c->ifsc = v;
            rc = t1_block(c, s, build(s, T1_S_IFS_RESP, &v, 1), 0, rb, rl);
        } else {
            rc = t1_block(c, s, build(s, T1_S_WTX_RESP, &v, 1), v, rb, rl);
        }
    }
    return rc;
}

static int t1_apdu(kiss_ccid *c, const uint8_t *apdu, size_t len,
                   uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t blk[4 + T1_INF_MAX], rb[4 + T1_INF_MAX];
    size_t rl = 0;
    int steps = 0, rc = CCID_OK;
    size_t ifsc = c->ifsc ? c->ifsc : 0x20;

    // Command: I-blocks of at most IFSC bytes, chained with the M bit; each
    // one but the last is acknowledged by an R-block naming the next N(S).
    for (size_t off = 0; ; ) {
        size_t chunk = len - off < ifsc ? len - off : ifsc;
        bool more = off + chunk < len;
        uint8_t pcb = (uint8_t)((c->ns << 6) | (more ? 0x20 : 0));
        rc = t1_exchange(c, blk, build(blk, pcb, apdu + off, chunk), rb, &rl, &steps);
        if (rc != CCID_OK)
            goto out;
        c->ns ^= 1;
        off += chunk;
        if (!more)
            break;
        if (!is_r(rb[1]) || (rb[1] & 0x0F) || ((rb[1] >> 4) & 1) != c->ns) {
            rc = CCID_ERR_CARD;
            goto out;
        }
        if (++steps > T1_MAX_STEPS) { rc = CCID_ERR_CARD; goto out; }
    }

    // Response: I-blocks, chained the same way; each chained one is
    // acknowledged with an R-block naming the next N(S) we expect.
    *out_len = 0;
    for (;;) {
        if (!is_i(rb[1]) || ((rb[1] >> 6) & 1) != c->nr) {
            rc = CCID_ERR_CARD;
            goto out;
        }
        c->nr ^= 1;
        size_t il = rb[2];
        if (*out_len + il > cap) {
            rc = CCID_ERR_SIZE;
            goto out;
        }
        memcpy(out + *out_len, rb + 3, il);
        *out_len += il;
        if (!(rb[1] & 0x20))
            break;
        if (++steps > T1_MAX_STEPS) { rc = CCID_ERR_CARD; goto out; }
        uint8_t r[4];
        rc = t1_exchange(c, r, build(r, (uint8_t)(0x80 | (c->nr << 4)), NULL, 0),
                         rb, &rl, &steps);
        if (rc != CCID_OK)
            goto out;
    }
out:
    kiss_wipe(blk, sizeof blk);
    kiss_wipe(rb, sizeof rb);
    return rc;
}

int kiss_ccid_apdu(kiss_ccid *c, const uint8_t *apdu, size_t apdu_len,
                   uint8_t *resp, size_t resp_cap, size_t *resp_len,
                   uint16_t *sw)
{
    *resp_len = 0;
    *sw = 0;
    if (!c->atr_len)
        return CCID_ERR_NOCARD;
    if (apdu_len < 4 || apdu_len > 261)
        return CCID_ERR_PROTO;

    uint8_t r[258 + 2];
    size_t rl = 0;
    int rc;
    if (c->tpdu) {
        rc = t1_apdu(c, apdu, apdu_len, r, sizeof r, &rl);
    } else {
        uint8_t m[10 + 261], in[10 + sizeof r];
        size_t n;
        header(m, PC_XFR_BLOCK, (uint32_t)apdu_len);
        memcpy(m + 10, apdu, apdu_len);
        rc = transact(c, m, 10 + apdu_len, RDR_DATA_BLOCK, in, sizeof in, &n);
        kiss_wipe(m, sizeof m);
        if (rc == CCID_OK) {
            rl = n - 10;
            memcpy(r, in + 10, rl);
        }
        kiss_wipe(in, sizeof in);
    }
    if (rc == CCID_OK && rl < 2)
        rc = CCID_ERR_CARD;
    if (rc == CCID_OK && rl - 2 > resp_cap)
        rc = CCID_ERR_SIZE;
    if (rc == CCID_OK) {
        memcpy(resp, r, rl - 2);
        *resp_len = rl - 2;
        *sw = (uint16_t)(r[rl - 2] << 8 | r[rl - 1]);
    }
    kiss_wipe(r, sizeof r);
    return rc;
}
