// Desktop tests for kiss_ccid.c: the reader protocol and T=1.
//
// The reader is emulated at the CCID message level, in both modes a real one
// comes in: APDU level (the reader frames for the card) and TPDU level (this
// driver speaks T=1 itself). In TPDU mode the emulator plays the card's side
// of T=1 with a small block size both ways, so every command and every
// answer is chained, and it can ask for more time or a new block size. The
// card behind it is test_satocard.c's emulated Satochip, so the last test
// runs the whole stack: card wallet, secure channel, CCID, T=1.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_cardwallet.h"
#include "kiss_ccid.h"
#include "kiss_satocard.h"

void test_satocard_card_reset(void);
int  test_satocard_apdu(const uint8_t *cmd, size_t len, uint8_t *resp, size_t cap,
                        size_t *rlen, uint16_t *sw);
void test_satocard_anchor(int on);

static int cfails;
static void cchk(const char *name, int ok)
{
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); cfails++; }
}

// ---- the emulated reader ----
typedef struct {
    bool tpdu;
    bool card_in;
    uint8_t atr[40];
    size_t atr_len;
    uint8_t ifsd;          // the most INF the card sends per block
    int wtx;               // ask for more time before this many answers
    bool ifs_req;          // ask once for a new IFSC before an answer
    int time_ext;          // the reader says "still working" this many times
    bool bad_seq;          // the reader answers with the wrong bSeq
    bool lrc_bad;          // the card's next block has a bad LRC

    uint8_t card_ns, card_nr;
    uint8_t cmd[300];
    size_t cmd_len;
    uint8_t rsp[300];
    size_t rsp_len, rsp_off;
    bool rsp_due;          // a WTX or IFS round is between command and answer

    uint8_t pend[600];
    size_t pend_len;
    int pend_ext;

    int cmd_blocks, rsp_blocks, wtx_seen, ifs_seen;
} fake_reader;

static uint8_t lrc(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

static size_t t1(uint8_t *out, uint8_t pcb, const uint8_t *inf, size_t n)
{
    out[0] = 0; out[1] = pcb; out[2] = (uint8_t)n;
    if (n) memcpy(out + 3, inf, n);
    out[3 + n] = lrc(out, 3 + n);
    return 4 + n;
}

static size_t next_rsp_block(fake_reader *r, uint8_t *out)
{
    size_t left = r->rsp_len - r->rsp_off;
    size_t n = left < r->ifsd ? left : r->ifsd;
    bool more = r->rsp_off + n < r->rsp_len;
    size_t l = t1(out, (uint8_t)((r->card_ns << 6) | (more ? 0x20 : 0)), r->rsp + r->rsp_off, n);
    r->card_ns ^= 1;
    r->rsp_off += n;
    r->rsp_blocks++;
    return l;
}

// The card's side of T=1: one block in, one block out.
static size_t card_t1(fake_reader *r, const uint8_t *b, size_t bl, uint8_t *out)
{
    if (bl < 4 || bl != 4u + b[2] || lrc(b, bl) != 0)
        return t1(out, (uint8_t)(0x81 | (r->card_nr << 4)), NULL, 0);   // R(error)
    uint8_t pcb = b[1];
    if ((pcb & 0x80) == 0) {                                 // I-block
        if (((pcb >> 6) & 1) != r->card_nr)
            return t1(out, (uint8_t)(0x82 | (r->card_nr << 4)), NULL, 0);
        r->card_nr ^= 1;
        memcpy(r->cmd + r->cmd_len, b + 3, b[2]);
        r->cmd_len += b[2];
        r->cmd_blocks++;
        if (pcb & 0x20)                                      // chained: ack it
            return t1(out, (uint8_t)(0x80 | (r->card_nr << 4)), NULL, 0);
        uint16_t sw = 0;
        size_t rl = 0;
        test_satocard_apdu(r->cmd, r->cmd_len, r->rsp, sizeof r->rsp - 2, &rl, &sw);
        r->rsp[rl] = (uint8_t)(sw >> 8);
        r->rsp[rl + 1] = (uint8_t)sw;
        r->rsp_len = rl + 2;
        r->rsp_off = 0;
        r->cmd_len = 0;
        if (r->ifs_req) {
            r->ifs_req = false;
            r->rsp_due = true;
            uint8_t v = 0x30;
            return t1(out, 0xC1, &v, 1);
        }
        if (r->wtx > 0) {
            r->wtx--;
            r->rsp_due = true;
            uint8_t v = 2;
            return t1(out, 0xC3, &v, 1);
        }
        return next_rsp_block(r, out);
    }
    if ((pcb & 0xC0) == 0x80) {                              // R-block: next piece
        if (((pcb >> 4) & 1) != r->card_ns || r->rsp_off >= r->rsp_len)
            return t1(out, (uint8_t)(0x82 | (r->card_nr << 4)), NULL, 0);
        return next_rsp_block(r, out);
    }
    if (pcb == 0xE3 && r->rsp_due) {                         // WTX answered
        r->wtx_seen++;
        r->rsp_due = false;
        return next_rsp_block(r, out);
    }
    if (pcb == 0xE1 && r->rsp_due) {                         // IFS answered
        r->ifs_seen++;
        r->rsp_due = false;
        return next_rsp_block(r, out);
    }
    return t1(out, (uint8_t)(0x82 | (r->card_nr << 4)), NULL, 0);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static size_t answer(uint8_t *out, uint8_t type, uint8_t seq, uint8_t status,
                     uint8_t err, const uint8_t *data, size_t n)
{
    out[0] = type;
    put32(out + 1, (uint32_t)n);
    out[5] = 0; out[6] = seq; out[7] = status; out[8] = err; out[9] = 0;
    if (n) memcpy(out + 10, data, n);
    return 10 + n;
}

static int reader_io(void *ctx, const uint8_t *out, size_t out_len,
                     uint8_t *in, size_t cap, size_t *in_len, uint32_t timeout_ms)
{
    fake_reader *r = ctx;
    (void)timeout_ms;
    *in_len = 0;
    if (!out_len) {                                          // a read only
        if (!r->pend_len) return -1;
        if (r->pend_ext > 0) {
            r->pend_ext--;
            uint8_t seq = r->pend[6];
            *in_len = answer(in, 0x80, seq, 0x80, 1, NULL, 0);
            return 0;
        }
        memcpy(in, r->pend, r->pend_len);
        *in_len = r->pend_len;
        r->pend_len = 0;
        return 0;
    }
    uint8_t seq = out[6], type = out[0];
    uint8_t ans_seq = r->bad_seq ? (uint8_t)(seq + 1) : seq;
    uint8_t icc = r->card_in ? 0 : 2;
    uint8_t buf[600];
    size_t n;
    if (type == 0x62) {
        if (!r->card_in)
            n = answer(buf, 0x80, ans_seq, 0x42, 0xFE, NULL, 0);
        else {
            r->card_ns = r->card_nr = 0;
            n = answer(buf, 0x80, ans_seq, 0, 0, r->atr, r->atr_len);
        }
    } else if (type == 0x61) {
        n = answer(buf, 0x82, ans_seq, icc, 0, out + 10, 7);
    } else if (type == 0x65 || type == 0x63) {
        n = answer(buf, 0x81, ans_seq, icc, 0, NULL, 0);
    } else if (type == 0x6F) {
        size_t dl = (size_t)out[1] | (size_t)out[2] << 8;
        uint8_t data[300];
        size_t rl = 0;
        if (r->tpdu) {
            rl = card_t1(r, out + 10, dl, data);
            if (r->lrc_bad) { data[rl - 1] ^= 0xFF; r->lrc_bad = false; }
        } else {
            uint16_t sw = 0;
            test_satocard_apdu(out + 10, dl, data, sizeof data - 2, &rl, &sw);
            data[rl] = (uint8_t)(sw >> 8); data[rl + 1] = (uint8_t)sw;
            rl += 2;
        }
        n = answer(buf, 0x80, ans_seq, icc, 0, data, rl);
    } else {
        n = answer(buf, 0x81, ans_seq, 0x40 | icc, 0, NULL, 0);
    }
    if (r->time_ext > 0) {                                   // "still working" first
        memcpy(r->pend, buf, n);
        r->pend_len = n;
        r->pend_ext = r->time_ext - 1;
        r->time_ext = 0;
        *in_len = answer(in, 0x80, ans_seq, 0x80, 1, NULL, 0);
        return 0;
    }
    if (n > cap) return -1;
    memcpy(in, buf, n);
    *in_len = n;
    return 0;
}

// a T=1 card: TD1 says T=1, TD2 says T=1 again with TA3 = IFSC, TB3 = BWI/CWI
static size_t t1_atr(uint8_t *atr, uint8_t ifsc)
{
    uint8_t a[] = { 0x3B, 0x80, 0x81, 0x31, ifsc, 0x45, 0x00 };
    a[6] = lrc(a + 1, 5);
    memcpy(atr, a, sizeof a);
    return sizeof a;
}

static void reader_new(fake_reader *r, bool tpdu, uint8_t ifsc, uint8_t ifsd)
{
    memset(r, 0, sizeof *r);
    r->tpdu = tpdu;
    r->card_in = true;
    r->ifsd = ifsd;
    r->atr_len = t1_atr(r->atr, ifsc);
    test_satocard_card_reset();
}

static kiss_ccid T_C;
static int ccid_xfer(void *ctx, const uint8_t *cmd, size_t len, uint8_t *resp,
                     size_t cap, size_t *rlen, uint16_t *sw)
{
    (void)ctx;
    return kiss_ccid_apdu(&T_C, cmd, len, resp, cap, rlen, sw) == CCID_OK ? 0 : -1;
}

static void rng(uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(0x5A + 3 * i);
}

static void test_descriptors(void)
{
    // config 9 + interface 9 (class 0B) + CCID 54 + bulk out, bulk in, intr in
    uint8_t cfg[9 + 9 + 54 + 21] = { 9, 0x02, sizeof cfg, 0, 1, 1, 0, 0x80, 50 };
    uint8_t *p = cfg + 9;
    const uint8_t intf[9] = { 9, 0x04, 0, 0, 3, 0x0B, 0, 0, 0 };
    memcpy(p, intf, 9); p += 9;
    p[0] = 54; p[1] = 0x21;
    put32(p + 6, 0x03);                 // T=0 and T=1
    put32(p + 28, 254);                 // dwMaxIFSD
    put32(p + 40, 0x000100BA);          // TPDU level
    put32(p + 44, 271);
    p += 54;
    const uint8_t eps[21] = { 7, 5, 0x02, 2, 64, 0, 0,  7, 5, 0x82, 2, 64, 0, 0,
                              7, 5, 0x83, 3, 8, 0, 24 };
    memcpy(p, eps, 21);
    kiss_ccid_reader rd;
    cchk("descriptor: CCID interface found", kiss_ccid_parse_config(cfg, sizeof cfg, &rd) == 0);
    cchk("descriptor: endpoints", rd.bulk_out == 0x02 && rd.bulk_in == 0x82 && rd.bulk_in_mps == 64);
    cchk("descriptor: features and IFSD", rd.features == 0x000100BA && rd.max_ifsd == 254);
    kiss_ccid c;
    kiss_ccid_init(&c, &rd, 0x1234, 0x5678, reader_io, NULL);
    cchk("descriptor: TPDU level reader framed by us", c.tpdu);

    rd.features = 0;
    kiss_ccid_init(&c, &rd, 0x072F, 0xB100, reader_io, NULL);
    cchk("quirk: ACR39U-NF with no level reported is TPDU", c.tpdu);
    kiss_ccid_init(&c, &rd, 0x1234, 0x5678, reader_io, NULL);
    cchk("quirk: an unknown reader with no level is not", !c.tpdu);

    cfg[9 + 5] = 0x03;                  // a HID interface instead
    cchk("descriptor: a non-CCID device is refused", kiss_ccid_parse_config(cfg, sizeof cfg, &rd) != 0);
    cfg[9 + 5] = 0x0B;
    cchk("descriptor: a truncated one is refused", kiss_ccid_parse_config(cfg, 40, &rd) != 0);
}

static void test_atr(void)
{
    kiss_ccid c;
    memset(&c, 0, sizeof c);
    // a J3H145 (JCOP3) as it answers on a contact reader
    const uint8_t j3h145[] = { 0x3B, 0xDC, 0x18, 0xFF, 0x81, 0x91, 0xFE, 0x1F, 0xC3,
                               0x80, 0x73, 0xC8, 0x21, 0x13, 0x66, 0x01, 0x06, 0x11,
                               0x59, 0x00, 0x01, 0x28 };
    cchk("atr: J3H145 parses", kiss_ccid_parse_atr(&c, j3h145, sizeof j3h145) == 0);
    cchk("atr: J3H145 speaks T=1 with IFSC 254, TA1 18",
         c.t1 && c.ifsc == 0xFE && c.ta1 == 0x18);
    uint8_t bad[sizeof j3h145];
    memcpy(bad, j3h145, sizeof bad);
    bad[sizeof bad - 1] ^= 1;
    cchk("atr: a wrong TCK is refused", kiss_ccid_parse_atr(&c, bad, sizeof bad) != 0);
    cchk("atr: a cut one is refused", kiss_ccid_parse_atr(&c, j3h145, 12) != 0);
    const uint8_t t0only[] = { 0x3B, 0x02, 0x14, 0x50 };
    cchk("atr: a T=0 card parses and is not T=1",
         kiss_ccid_parse_atr(&c, t0only, sizeof t0only) == 0 && !c.t1);
    uint8_t crc[] = { 0x3B, 0x80, 0x81, 0x41, 0x01, 0x00 };   // TC3 bit 0: CRC
    crc[5] = lrc(crc + 1, 4);
    cchk("atr: a CRC-only T=1 card is refused", kiss_ccid_parse_atr(&c, crc, sizeof crc) != 0);
    const uint8_t junk[] = { 0x12, 0x34 };
    cchk("atr: not an ATR", kiss_ccid_parse_atr(&c, junk, sizeof junk) != 0);
}

static int open_reader(fake_reader *r, uint32_t features)
{
    kiss_ccid_reader rd;
    memset(&rd, 0, sizeof rd);
    rd.bulk_in = 0x82; rd.bulk_out = 0x02;
    rd.features = features;
    rd.max_ifsd = 254;
    kiss_ccid_init(&T_C, &rd, 1, 2, reader_io, r);
    return kiss_ccid_power_on(&T_C);
}

static void test_reader(void)
{
    fake_reader r;
    const uint8_t select[] = { 0x00, 0xA4, 0x04, 0x00, 0x08, 'S','a','t','o','C','h','i','p' };
    const uint8_t status[] = { 0xB0, 0x3C, 0x00, 0x00 };
    uint8_t resp[300];
    size_t rl;
    uint16_t sw;

    reader_new(&r, false, 0x20, 0x20);
    cchk("apdu reader: power on", open_reader(&r, 0x00020000) == CCID_OK && T_C.atr_len == 7);
    cchk("apdu reader: select", kiss_ccid_apdu(&T_C, select, sizeof select, resp, sizeof resp, &rl, &sw) == CCID_OK && sw == 0x9000);
    cchk("apdu reader: status", kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_OK &&
         sw == 0x9000 && rl == 13);

    reader_new(&r, true, 0x08, 0x08);       // 8-byte blocks both ways
    cchk("t1: power on with SetParameters", open_reader(&r, 0x00010000) == CCID_OK && T_C.t1);
    cchk("t1: a 13-byte select goes out in two chained blocks",
         kiss_ccid_apdu(&T_C, select, sizeof select, resp, sizeof resp, &rl, &sw) == CCID_OK &&
         sw == 0x9000 && r.cmd_blocks == 2);
    r.cmd_blocks = r.rsp_blocks = 0;
    r.wtx = 1;
    cchk("t1: status through a wait-time extension",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_OK &&
         sw == 0x9000 && rl == 13 && r.wtx_seen == 1);
    cchk("t1: a 15-byte answer came in two chained blocks", r.rsp_blocks == 2);
    r.ifs_req = true;
    cchk("t1: the card asks for a new block size",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_OK &&
         r.ifs_seen == 1 && T_C.ifsc == 0x30);
    r.time_ext = 3;
    cchk("ccid: the reader's time extensions are sat through",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_OK && sw == 0x9000);
    r.bad_seq = true;
    cchk("ccid: an answer for another sequence number is refused",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_ERR_READER);
    r.bad_seq = false;

    reader_new(&r, true, 0x20, 0x20);
    open_reader(&r, 0x00010000);
    r.lrc_bad = true;
    cchk("t1: a block with a bad LRC stops the exchange",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_ERR_CARD);
    reader_new(&r, true, 0x20, 0x20);
    open_reader(&r, 0x00010000);
    kiss_ccid_apdu(&T_C, select, sizeof select, resp, sizeof resp, &rl, &sw);
    cchk("t1: a too-small buffer is a size error, not truncation",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, 5, &rl, &sw) == CCID_ERR_SIZE && rl == 0);

    reader_new(&r, true, 0x20, 0x20);
    r.card_in = false;
    cchk("ccid: no card in the slot", open_reader(&r, 0x00010000) == CCID_ERR_NOCARD);
    cchk("ccid: slot status says so too", kiss_ccid_slot_status(&T_C) == CCID_ERR_NOCARD);
    cchk("ccid: an APDU with no card powered is refused",
         kiss_ccid_apdu(&T_C, status, sizeof status, resp, sizeof resp, &rl, &sw) == CCID_ERR_NOCARD);

    reader_new(&r, true, 0x20, 0x20);
    r.atr_len = 4;
    memcpy(r.atr, (const uint8_t[]){ 0x3B, 0x02, 0x14, 0x50 }, 4);
    cchk("ccid: a T=0-only card behind a TPDU reader is refused",
         open_reader(&r, 0x00010000) == CCID_ERR_PROTO);
}

static void test_whole_stack(void)
{
    // card wallet -> secure channel -> CCID -> T=1 (8-byte blocks) -> card
    fake_reader r;
    reader_new(&r, true, 0x08, 0x08);
    r.wtx = 1000;                             // every answer waits once
    cchk("stack: reader powers the card", open_reader(&r, 0x00010000) == CCID_OK);
    test_satocard_anchor(1);
    int tries = 0;
    cchk("stack: connect", kiss_cardwallet_connect(ccid_xfer, NULL, rng, &tries) == SC_OK && tries == 3);
    cchk("stack: PIN, genuine check, thirteen keys",
         kiss_cardwallet_unlock("123456", 6, NULL, NULL) == SC_OK);
    test_satocard_anchor(0);
    uint8_t h[32], der[72], fp[4];
    size_t dl = 0;
    memset(h, 0x42, 32);
    const uint32_t path[5] = { 0x80000054, 0x80000000, 0x80000000, 0, 0 };
    cchk("stack: the card signs over T=1",
         kiss_cardwallet_sign(path, 5, h, der, &dl) == SC_OK && dl > 8);
    cchk("stack: dev seed fingerprint 73C5DA0A",
         kiss_cardwallet_fingerprint(fp) == 0 && fp[0] == 0x73 && fp[1] == 0xC5 &&
         fp[2] == 0xDA && fp[3] == 0x0A);
    printf("  (T=1 blocks: %d command, %d answer, %d waits)\n", r.cmd_blocks, r.rsp_blocks, r.wtx_seen);
    kiss_cardwallet_close();
    kiss_ccid_power_off(&T_C);
}

int test_ccid(void)
{
    cfails = 0;
    test_descriptors();
    test_atr();
    test_reader();
    test_whole_stack();
    return cfails;
}
