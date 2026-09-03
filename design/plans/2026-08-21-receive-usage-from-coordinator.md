# Address usage from the coordinator — firmware half

**Goal:** The Receive screen stops claiming UNUSED on evidence it does not have, and gains a way to be told.

**Architecture:** A coordinator sends one line of text in the QR the owner already scans at RECEIVE > VERIFY. `kiss_usage` learns a second fact per wallet — what a coordinator claims the chain shows, and the height it claimed it at — kept separate from the high-water mark this device witnessed itself. The badge becomes three states: USED on local evidence alone, UNUSED only once a coordinator has spoken, and USAGE UNKNOWN when it has not.

**Tech Stack:** C11, LVGL, ESP-IDF v6.0.1, NVS. Desktop sim + `kisstest` for everything below the screen.

**Design spec:** `design/specs/2026-08-21-receive-usage-from-coordinator-design.md` (commits `2276bcd`, `14ec734`).

---

## Context

The Receive screen badges every address UNUSED. A signer has no chain view, so it cannot know that — observed on a wallet with real history at indices 0..29, all thirty read UNUSED while the coordinator's `next_unused_address()` correctly answered 30.

The chip's own comment at `main/kiss_recv.c:249` already argues the case, and half of it holds. USED is provable here: `mark_used_receives` (`main/kiss_sign.c:745`) records the index of every receive input this device signs, and spending an output proves it was funded. UNUSED is a negative claim about the chain and needs the chain. This splits them.

**Scope: firmware only.** The coordinator half (`address --qr`) is deferred — `signet-support` rewrites 417 lines of the same `src/main.rs` and deletes the `NETWORK` const it would build on, and `silent-payments` is committing actively. Firmware tests use hand-written payload vectors; the wire format is pinned by the spec so neither half can drift.

## Workspace and collision notes

Work on `feat/receive-usage-from-coordinator`, in an isolated checkout.

Every file below is clean on `develop` **except** `sim/test_crypto.c`, `sim/test_fuzz.c` and `sim/sim_main.c`, which carry in-flight edits. The plan touches `test_crypto.c` for exactly two lines (a declaration and a call, appended to existing lists), never touches `test_fuzz.c` (adversarial inputs live in the new `test_usage.c` instead), and isolates the `sim_main.c` walk stop into the last task so it can be dropped or rebased alone. `main/kiss_scan.c` is dirty and is **not** needed — `kiss_scan_open_raw` is used exactly as it is.

Isolate every gate run so it cannot collide with another:

```bash
export KISS_SIM_TMP=/tmp/kiss-$$
```

## Files

| File | Responsibility |
| --- | --- |
| `main/kiss_usage.h` (modify) | the payload struct, the parser, the two chain accessors |
| `main/kiss_usage.c` (modify) | parse; store chain claim beside the local mark in `kissu` |
| `main/kiss_recv.c` (modify) | route the payload at VERIFY, tri-state chip, the line under the QR |
| `i18n/*.json` ×21 (modify) | four new strings, English in every locale |
| `main/i18n_keys.h`, `main/i18n_tables.c` | **generated** — never hand-edit, run `tools/gen_i18n.py` |
| `sim/test_usage.c` (create) | the whole suite, including adversarial inputs |
| `sim/build_test.sh` (modify) | one filename |
| `sim/test_crypto.c` (modify) | two lines: declare + call the suite |
| `sim/sim_main.c` (modify, last) | one walk stop that photographs the three chip states |

---

## Task 1: The payload parser

**Files:** Modify `main/kiss_usage.h`, `main/kiss_usage.c` · Create `sim/test_usage.c` · Modify `sim/build_test.sh`, `sim/test_crypto.c`

- [ ] **Step 1: Declare the contract in `main/kiss_usage.h`**

Add `#include <stddef.h>` beside the existing `<stdint.h>`, then append:

```c
// ---- what a coordinator says the chain shows ----
// This device has no chain view, so UNUSED is a claim it cannot make alone. A
// coordinator can, and sends one line of text in the QR the Receive screen
// already scans at VERIFY.
//
// DISPLAY DATA. It picks a chip's text and colour and reaches no derivation, no
// address search and no signature. A wrong value costs the owner a fresher
// address than they needed, which is the direction to be wrong in.

// Not a wallet's ceiling: an absurdity bound, so a scanned number cannot strand
// the chip somewhere no owner could walk back from with NEXT.
#define KISS_USAGE_MAX_INDEX 100000

typedef struct {
    uint8_t  fp[4];
    int      testnet;    // 0 / 1
    int      script;     // WSCRIPT_*
    int      high;       // highest used external index; -1 = chain shows none
    uint32_t height;     // chain tip the coordinator synced to
    char     addr[128];  // the address to verify; holds a silent payment whole
} kiss_usage_msg_t;

// Parse "KISSU1 <FP8> <net> <script> <high> <height> <ADDR>".
// 0 = a well formed payload. Nonzero means this is not one, and the caller
// feeds it to the address path unchanged.
int kiss_usage_parse(const char *txt, size_t len, kiss_usage_msg_t *out);

// Has a coordinator spoken about these keys? 1 = yes, and *high / *height are
// filled. *high may be -1, meaning the chain shows nothing used, which is a
// real answer and not an absence.
int kiss_usage_chain_known(const uint8_t fp[4], int testnet, int script,
                           int *high, uint32_t *height);

// Record what a coordinator claims. Accepted only when height beats the stored
// one, which makes a re-shown QR inert and still lets a real correction land
// after a reorg or a rebuilt wallet. 1 = accepted, 0 = older, equal or refused.
int kiss_usage_chain_set(const uint8_t fp[4], int testnet, int script,
                         int high, uint32_t height);
```

- [ ] **Step 2: Write the failing test — create `sim/test_usage.c`**

```c
// The coordinator's usage payload: the parser, the height gate, and the three
// states the Receive chip can be in. No LVGL here — kisstest links kiss_usage.c
// and not kiss_recv.c, so everything below the screen is provable on desktop.
#include <stdio.h>
#include <string.h>

#include "kiss_crypto.h"   // WSCRIPT_*
#include "kiss_usage.h"

static int ufails;

static void uchk(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ufails++;
}

#define P(s) kiss_usage_parse((s), strlen(s), &m)

static void test_parse_good(void)
{
    kiss_usage_msg_t m;
    memset(&m, 0, sizeof m);
    uchk("a well formed payload parses",
         P("KISSU1 EC5A4595 1 0 29 1234567 TB1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4") == 0);
    uchk("  ...fingerprint", m.fp[0] == 0xec && m.fp[1] == 0x5a &&
                             m.fp[2] == 0x45 && m.fp[3] == 0x95);
    uchk("  ...network", m.testnet == 1);
    uchk("  ...script", m.script == WSCRIPT_NATIVE);
    uchk("  ...index", m.high == 29);
    uchk("  ...height", m.height == 1234567u);
    uchk("  ...address rides along whole",
         strcmp(m.addr, "TB1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4") == 0);

    memset(&m, 0, sizeof m);
    uchk("lowercase hex is accepted too",
         P("KISSU1 ec5a4595 0 2 0 1 1BOATSLRHTKNNGKDXFVPXRQ") == 0 &&
         m.fp[0] == 0xec && m.script == WSCRIPT_LEGACY && m.testnet == 0);

    memset(&m, 0, sizeof m);
    uchk("-1 is a real answer, not an absence",
         P("KISSU1 EC5A4595 1 0 -1 900000 TB1QXY") == 0 && m.high == -1);

    memset(&m, 0, sizeof m);
    uchk("a trailing NUL from the scan path is trimmed",
         kiss_usage_parse("KISSU1 EC5A4595 1 0 7 42 TB1QXY", 31, &m) == 0 &&
         m.high == 7);
}

static void test_parse_refusals(void)
{
    kiss_usage_msg_t m;
    uchk("an ordinary address is not a payload",
         P("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4") != 0);
    uchk("a bitcoin URI is not a payload",
         P("bitcoin:tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4") != 0);
    uchk("wrong magic is refused",   P("KISSU2 EC5A4595 1 0 29 1234567 TB1QXY") != 0);
    uchk("a short fingerprint is refused", P("KISSU1 EC5A45 1 0 29 1234567 TB1QXY") != 0);
    uchk("a non hex fingerprint is refused", P("KISSU1 ZZ5A4595 1 0 29 1234567 TB1QXY") != 0);
    uchk("a missing address is refused", P("KISSU1 EC5A4595 1 0 29 1234567") != 0);
    uchk("a truncated payload is refused", P("KISSU1 EC5A4595 1 0") != 0);
    uchk("trailing junk is refused",
         P("KISSU1 EC5A4595 1 0 29 1234567 TB1QXY EXTRA") != 0);
    uchk("an out of range network is refused", P("KISSU1 EC5A4595 2 0 29 1234567 TB1QXY") != 0);
    uchk("an out of range script is refused",  P("KISSU1 EC5A4595 1 9 29 1234567 TB1QXY") != 0);
    uchk("an index past the cap is refused",
         P("KISSU1 EC5A4595 1 0 100001 1234567 TB1QXY") != 0);
    uchk("an index below -1 is refused", P("KISSU1 EC5A4595 1 0 -2 1234567 TB1QXY") != 0);
    uchk("a non numeric index is refused", P("KISSU1 EC5A4595 1 0 2x 1234567 TB1QXY") != 0);
    uchk("an absurd height is refused",
         P("KISSU1 EC5A4595 1 0 29 999999999999 TB1QXY") != 0);
    uchk("an empty payload is refused", P("") != 0);
}

// Not the kissfuzz harness: this parser is pure text with no crypto under it,
// and keeping its adversarial inputs here avoids touching sim/test_fuzz.c.
// Nothing may crash and nothing malformed may be accepted.
static void test_parse_adversarial(void)
{
    kiss_usage_msg_t m;
    char buf[512];
    int accepted_junk = 0;

    for (size_t n = 0; n < sizeof buf - 1; n++) {
        memset(buf, 'A' + (int)(n % 26), n);
        buf[n] = 0;
        if (kiss_usage_parse(buf, n, &m) == 0) accepted_junk++;
    }
    uchk("no run of filler is ever accepted", accepted_junk == 0);

    const char *stem = "KISSU1 EC5A4595 1 0 29 1234567 TB1QW508D6QEJXTDG4Y5R3ZARVARY0";
    for (size_t n = 0; n <= strlen(stem); n++)
        (void)kiss_usage_parse(stem, n, &m);       // every prefix: must not crash
    uchk("every truncation of a good payload survives", 1);

    memset(buf, ' ', sizeof buf);
    uchk("all spaces is refused", kiss_usage_parse(buf, sizeof buf, &m) != 0);

    snprintf(buf, sizeof buf, "KISSU1 EC5A4595 1 0 29 1234567 %.*s", 400,
             "TB1QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ");
    uchk("an address longer than the field is refused",
         kiss_usage_parse(buf, strlen(buf), &m) != 0);
}

int test_usage(void)
{
    ufails = 0;
    test_parse_good();
    test_parse_refusals();
    test_parse_adversarial();
    return ufails;
}
```

- [ ] **Step 3: Wire the suite in**

In `sim/build_test.sh:47`, append `sim/test_usage.c` to the `sim/test_*.c` list (after `sim/test_art.c`).

In `sim/test_crypto.c`, add beside the other suite declarations near line 58:

```c
// sim/test_usage.c — the coordinator's address usage payload
int test_usage(void);
```

and beside the other calls near line 775:

```c
    fails += test_usage();
```

- [ ] **Step 4: Run it and watch it fail**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ && bash sim/build_test.sh
```

Expected: the build fails with `undefined reference to kiss_usage_parse`.

- [ ] **Step 5: Implement the parser in `main/kiss_usage.c`**

Add near the top of the file, after the existing `usage_key` helper:

```c
// ---- the coordinator's payload ----
// Fields are fixed width or bounded decimals, and the address goes last because
// it is the only variable length one. Everything is read out of a bounded copy:
// the scan path hands over whatever a camera decoded, which is not trusted to
// be terminated, terminated where it claims, or terminated at all.
static const char *utok(const char *p, const char *end, char *out, size_t cap)
{
    while (p < end && *p == ' ') p++;
    size_t n = 0;
    while (p < end && *p != ' ') {
        if (n + 1 >= cap) return NULL;          // longer than the field allows
        out[n++] = *p++;
    }
    out[n] = 0;
    return n ? p : NULL;                        // an empty field is malformed
}

static int udec(const char *s, long lo, long hi, long *out)
{
    int neg = *s == '-';
    if (neg) s++;
    if (!*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        if (v > 100000000L) return -1;          // bounded well under LONG_MAX
        v = v * 10 + (*s - '0');
    }
    if (neg) v = -v;
    if (v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int uhexnib(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0');      return 0; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return 0; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return 0; }
    return -1;
}

int kiss_usage_parse(const char *txt, size_t len, kiss_usage_msg_t *out)
{
    if (!txt || !out) return -1;
    // The coordinator emits uppercase so the whole payload stays inside the QR
    // alphanumeric charset, but the parser is liberal about case: that choice
    // is the sender's density trick, not a rule the reader gets to enforce.
    while (len && (txt[len - 1] == 0   || txt[len - 1] == '\n' ||
                   txt[len - 1] == '\r' || txt[len - 1] == ' '))
        len--;

    const char *p = txt, *end = txt + len;
    char f[24];
    long v;

    if (!(p = utok(p, end, f, sizeof f)) || strcmp(f, "KISSU1") != 0) return -1;

    if (!(p = utok(p, end, f, sizeof f)) || strlen(f) != 8) return -1;
    for (int i = 0; i < 4; i++) {
        uint8_t hi, lo;
        if (uhexnib(f[i * 2], &hi) || uhexnib(f[i * 2 + 1], &lo)) return -1;
        out->fp[i] = (uint8_t)((hi << 4) | lo);
    }

    if (!(p = utok(p, end, f, sizeof f)) || udec(f, 0, 1, &v)) return -1;
    out->testnet = (int)v;
    if (!(p = utok(p, end, f, sizeof f)) || udec(f, 0, 2, &v)) return -1;
    out->script = (int)v;
    if (!(p = utok(p, end, f, sizeof f)) || udec(f, -1, KISS_USAGE_MAX_INDEX, &v)) return -1;
    out->high = (int)v;
    if (!(p = utok(p, end, f, sizeof f)) || udec(f, 1, 100000000L, &v)) return -1;
    out->height = (uint32_t)v;

    if (!(p = utok(p, end, out->addr, sizeof out->addr))) return -1;
    while (p < end && *p == ' ') p++;
    return p == end ? 0 : -1;                   // trailing junk is malformed
}
```

Note the height's low bound is 1, not 0: a coordinator that has never synced has no claim to make, and a zero would otherwise win the height gate against nothing and then block the real one.

- [ ] **Step 6: Run the tests**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ && bash sim/build_test.sh && "$KISS_SIM_TMP/kisstest" 2>&1 | grep -E "FAIL|usage|payload" | head -40
```

Expected: every `test_usage` line PASSes, and the run's total is the previous count plus the new assertions.

- [ ] **Step 7: Commit**

```bash
git add main/kiss_usage.h main/kiss_usage.c sim/test_usage.c sim/build_test.sh sim/test_crypto.c
git commit -F - <<'MSG'
the signer learns to read what a coordinator says about the chain

One line of text, six fixed fields then the address, parsed with no trust in
what a camera hands over: not that it is terminated, not that it is terminated
where it claims, not that it is terminated at all. Every field is bounded
before it is believed and the address is the only variable length one, so it
goes last.

The index is capped at 100000. That is not a wallet's ceiling, it is an
absurdity bound: a scanned number that lands the chip past where NEXT can walk
back from is a number nobody can undo without a wipe.

Height's low bound is 1 rather than 0. A coordinator that has never synced has
no claim to make, and a zero would otherwise win the height gate against
nothing and then block the first real one behind it.

The adversarial inputs live in the new suite rather than sim/test_fuzz.c: this
parser is pure text with no crypto under it, and kissfuzz's harness buys it
nothing that a run of every truncation and every filler length does not.
MSG
```

---

## Task 2: Storing what a coordinator claims

**Files:** Modify `main/kiss_usage.c` · Modify `sim/test_usage.c`

- [ ] **Step 1: Write the failing tests — append to `sim/test_usage.c`**

```c
// A coordinator's claim is stored beside — never merged into — what this device
// witnessed. The two answer different questions, and only the local one is
// evidence.
static void test_chain_store(void)
{
    const uint8_t fp[4] = { 0xec, 0x5a, 0x45, 0x95 };
    int high; uint32_t height;

    kiss_usage_wipe();
    uchk("nothing is known before a coordinator speaks",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 0);

    uchk("a first claim is accepted",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 29, 1000) == 1);
    uchk("  ...and reads back",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 29 && height == 1000);

    uchk("an older claim is inert",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 99, 999) == 0);
    uchk("  ...and changed nothing",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 29);
    uchk("re-showing the same QR is inert",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 29, 1000) == 0);

    uchk("a newer claim lands even when it LOWERS the index",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 12, 1001) == 1);
    uchk("  ...because the coordinator is the chain's source of truth",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == 12 && height == 1001);

    uchk("none used is a real answer",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, -1, 1002) == 1 &&
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 1 &&
         high == -1);

    uchk("a height of zero is not a claim",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 5, 0) == 0);
    uchk("an index past the cap is refused",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, KISS_USAGE_MAX_INDEX + 1, 2000) == 0);

    // Buckets do not bleed: a different address type is a different wallet view.
    uchk("another script type is its own bucket",
         kiss_usage_chain_known(fp, 1, WSCRIPT_LEGACY, &high, &height) == 0);
    const uint8_t other[4] = { 0x00, 0x11, 0x22, 0x33 };
    uchk("another fingerprint is its own bucket",
         kiss_usage_chain_known(other, 1, WSCRIPT_NATIVE, &high, &height) == 0);

    // The local mark is evidence and a camera may not lower it.
    kiss_usage_mark(fp, 1, WSCRIPT_NATIVE, 40);
    uchk("a coordinator claim never touches the local mark",
         kiss_usage_chain_set(fp, 1, WSCRIPT_NATIVE, 3, 3000) == 1 &&
         kiss_usage_high(fp, 1, WSCRIPT_NATIVE) == 40);

    kiss_usage_wipe();
    uchk("a wipe takes the coordinator's claim with it",
         kiss_usage_chain_known(fp, 1, WSCRIPT_NATIVE, &high, &height) == 0);
}
```

Add `test_chain_store();` to `test_usage()` after `test_parse_adversarial();`.

- [ ] **Step 2: Run it and watch it fail**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ && bash sim/build_test.sh
```

Expected: `undefined reference to kiss_usage_chain_known` / `kiss_usage_chain_set`.

- [ ] **Step 3: Widen the session row in `main/kiss_usage.c`**

Replace the `struct usage_row` definition and the two table helpers:

```c
#define UMAX 32
// chain/cheight hold what a coordinator claimed, beside — never merged into —
// the witnessed mark in v. cheight doubles as the presence flag: a real payload
// always carries a height of at least 1, so zero means nobody has spoken.
struct usage_row { char key[16]; uint32_t v; uint32_t chain; uint32_t cheight; };
static struct usage_row s_session[UMAX];
static int s_session_n;

static struct usage_row *tab_row(struct usage_row *tab, int *n, const char *key,
                                 int create)
{
    for (int i = 0; i < *n; i++)
        if (strcmp(tab[i].key, key) == 0)
            return &tab[i];
    if (!create || *n >= UMAX)
        return NULL;
    struct usage_row *r = &tab[*n];
    memset(r, 0, sizeof *r);
    snprintf(r->key, sizeof r->key, "%s", key);
    (*n)++;
    return r;
}

static int tab_high(const struct usage_row *tab, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tab[i].key, key) == 0)
            return (int)tab[i].v;
    return -1;
}

static void tab_mark(struct usage_row *tab, int *n, const char *key, uint32_t idx)
{
    struct usage_row *r = tab_row(tab, n, key, 1);
    if (r && idx > r->v) r->v = idx;      // monotonic: never lower the mark
}

static void tab_chain_get(const struct usage_row *tab, int n, const char *key,
                          uint32_t *chain, uint32_t *cheight)
{
    *chain = *cheight = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(tab[i].key, key) == 0) {
            *chain = tab[i].chain; *cheight = tab[i].cheight;
            return;
        }
}

static void tab_chain_set(struct usage_row *tab, int *n, const char *key,
                          uint32_t chain, uint32_t cheight)
{
    struct usage_row *r = tab_row(tab, n, key, 1);
    if (r) { r->chain = chain; r->cheight = cheight; }
}
```

`tab_mark`'s original guard — a first mark of index 0 must still create the row — is preserved: `tab_row(..., 1)` zeroes the row, so `idx > r->v` is `0 > 0` and the row exists with `v == 0`, which is what the old code produced too.

- [ ] **Step 4: Add the two NVS keys**

In the `#ifdef ESP_PLATFORM` block, beside `persistent_high` / `persistent_mark`:

```c
// Two more keys in the same namespace, so the erase-all in persistent_wipe and
// the PERSIST switch cover them without a path of their own. "c" holds the
// claimed index plus one (0 would be ambiguous with an unset key) and "h" holds
// the height it was claimed at.
static void chain_persistent_get(const char *key, uint32_t *chain, uint32_t *cheight)
{
    *chain = *cheight = 0;
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READONLY, &h) != ESP_OK)
        return;
    char k[18];
    snprintf(k, sizeof k, "c%s", key);
    if (nvs_get_u32(h, k, chain) != ESP_OK) *chain = 0;
    snprintf(k, sizeof k, "h%s", key);
    if (nvs_get_u32(h, k, cheight) != ESP_OK) *cheight = 0;
    nvs_close(h);
}

static void chain_persistent_set(const char *key, uint32_t chain, uint32_t cheight)
{
    nvs_handle_t h;
    bool own = !s_batch_active;
    if (own && nvs_open("kissu", NVS_READWRITE, &h) != ESP_OK)
        return;
    if (!own) h = s_batch;
    char k[18];
    snprintf(k, sizeof k, "c%s", key);
    nvs_set_u32(h, k, chain);
    snprintf(k, sizeof k, "h%s", key);
    nvs_set_u32(h, k, cheight);
    if (own) { nvs_commit(h); nvs_close(h); }
}
```

In the `#else` host block, **after** the `s_persistent` / `s_persistent_n` declarations and beside the RAM `persistent_high` / `persistent_mark`:

```c
static void chain_persistent_get(const char *key, uint32_t *chain, uint32_t *cheight)
{
    tab_chain_get(s_persistent, s_persistent_n, key, chain, cheight);
}

static void chain_persistent_set(const char *key, uint32_t chain, uint32_t cheight)
{
    tab_chain_set(s_persistent, &s_persistent_n, key, chain, cheight);
}
```

- [ ] **Step 5: Implement the two accessors**

Append to `main/kiss_usage.c`, after `kiss_usage_mark`:

```c
int kiss_usage_chain_known(const uint8_t fp[4], int testnet, int script,
                           int *high, uint32_t *height)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    uint32_t c, h;
    if (may_persist()) {
        chain_persistent_get(key, &c, &h);
        if (h) tab_chain_set(s_session, &s_session_n, key, c, h);   // promote, as the mark does
    } else {
        tab_chain_get(s_session, s_session_n, key, &c, &h);
    }
    if (!h) return 0;
    if (high)   *high   = (int)c - 1;      // stored as high+1 so 0 stays free
    if (height) *height = h;
    return 1;
}

int kiss_usage_chain_set(const uint8_t fp[4], int testnet, int script,
                         int high, uint32_t height)
{
    if (height == 0 || high < -1 || high > KISS_USAGE_MAX_INDEX)
        return 0;
    int cur_high; uint32_t cur_height;
    // Strictly newer, and then the index is TAKEN rather than raised. A
    // coordinator is the chain's source of truth, so a genuine correction after
    // a reorg or a rebuilt wallet has to be able to come down; an old QR shown
    // again cannot, because it loses the height comparison.
    if (kiss_usage_chain_known(fp, testnet, script, &cur_high, &cur_height) &&
        height <= cur_height)
        return 0;
    char key[16];
    usage_key(fp, testnet, script, key);
    tab_chain_set(s_session, &s_session_n, key, (uint32_t)(high + 1), height);
    if (may_persist())
        chain_persistent_set(key, (uint32_t)(high + 1), height);
    return 1;
}
```

- [ ] **Step 6: Carry the claim through a session flush**

In `kiss_usage_persist_session`, the loop must write the chain fields too or a
mode change would drop them:

```c
    kiss_usage_batch_begin();
    for (int i = 0; i < s_session_n; i++) {
        persistent_mark(s_session[i].key, s_session[i].v);
        if (s_session[i].cheight)
            chain_persistent_set(s_session[i].key, s_session[i].chain,
                                 s_session[i].cheight);
    }
    kiss_usage_batch_end();
```

- [ ] **Step 7: Run the tests**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ && bash sim/build_test.sh && "$KISS_SIM_TMP/kisstest" 2>&1 | tail -5
```

Expected: `PASS` on every new line, and the suite total ends `FAILURES: 0`.

- [ ] **Step 8: Commit**

```bash
git add main/kiss_usage.c sim/test_usage.c
git commit -F - <<'MSG'
what a coordinator claims is kept beside what this device witnessed

Two facts per wallet now, and they are not the same kind of thing. The mark in
v is evidence: this signer signed a spend from that index, so the output was
funded and nothing a camera says can lower it. The claim in chain is hearsay
from something that can see the chain, which is exactly the half this device
cannot check.

Merging them into one number would have been smaller and would have thrown away
the distinction the Receive screen needs: without it there is no way to tell "no
badge" from "unused", which is the whole bug.

cheight doubles as the presence flag. A real payload always carries a height of
at least 1, so zero means nobody has spoken and no separate byte has to agree
with itself.

Accepted only on a strictly newer height, and then the index is TAKEN rather
than raised. A coordinator is the chain's source of truth, so a correction after
a reorg or a rebuilt wallet has to be able to come DOWN; an old QR shown again
cannot, because it loses the comparison. Monotonic-on-the-index would have made
a wrong value permanent until a wipe.

Both keys live in the kissu namespace, so the erase-all in persistent_wipe, the
PERSIST switch and amnesic mode cover them with no path of their own -- and the
session flush carries them, or a mode change would drop what it just promoted.
MSG
```

---

## Task 3: The four strings

**Files:** Modify `i18n/*.json` (all 21) · Regenerate `main/i18n_keys.h`, `main/i18n_tables.c`

`tools/gen_i18n.py` exits 1 on a key-set mismatch (`tools/gen_i18n.py:260`), so a key added to `en.json` alone breaks the build. Every locale gets the key with the English string, which is how `fdf2d46` shipped its rename.

- [ ] **Step 1: Insert the keys into all 21 locales**

```bash
python3 - <<'PY'
import json, pathlib, collections
NEW = collections.OrderedDict([
    ("R_USAGE_UNKNOWN",  "USAGE UNKNOWN"),
    ("R_CHAIN_UPTO_FMT", "coordinator: used up to #%u"),
    ("R_CHAIN_CLEAN",    "coordinator: none used yet"),
    ("R_CHAIN_ASK",      "VERIFY with your coordinator to see which are used"),
])
for p in sorted(pathlib.Path("i18n").glob("*.json")):
    d = json.loads(p.read_text(encoding="utf-8"),
                   object_pairs_hook=collections.OrderedDict)
    if "R_HANDED_ALREADY" not in d:
        raise SystemExit(f"{p}: anchor key missing")
    out = collections.OrderedDict()
    for k, v in d.items():
        out[k] = v
        if k == "R_HANDED_ALREADY":
            for nk, nv in NEW.items():
                out.setdefault(nk, nv)
    p.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n",
                 encoding="utf-8")
    print("updated", p)
PY
```

- [ ] **Step 2: Check the diff did not reflow the files**

```bash
git diff --stat i18n/ | tail -3
```

Expected: 21 files, roughly `+4` each. If a file shows hundreds of changed lines the indent or key order differs from the repo's — inspect `git diff i18n/de.json | head -30` and match the existing formatting before continuing.

- [ ] **Step 3: Regenerate and confirm the generated files moved**

```bash
python3 tools/gen_i18n.py && git diff --stat main/i18n_keys.h main/i18n_tables.c
```

Expected: no FATAL, and both generated files show additions. Warnings about translations being long are fine.

- [ ] **Step 4: Commit**

```bash
git add i18n/ main/i18n_keys.h main/i18n_tables.c
git commit -F - <<'MSG'
the receive screen gets words for what it does not know

Four strings. USAGE UNKNOWN is the chip's third state and is deliberately not a
claim about the address: it is the device saying what it has not been told.

The other three are the line under the QR. Two of them report a coordinator's
claim and one asks for it, and the reporting pair are separate strings rather
than one format because "used up to #-1" is not a sentence -- a wallet the
chain shows nothing on needs its own words.

"used up to" and not a count, on purpose. A high water mark cannot see gaps, so
a mark of 29 does not mean thirty addresses were used, and a line claiming a
count would be inventing one.

English in all 21 locales. gen_i18n.py exits 1 on a key-set mismatch, so a key
in en.json alone does not build; the sweep translates them with the rest.
MSG
```

---

## Task 4: The three states on the screen

**Files:** Modify `main/kiss_recv.c`

- [ ] **Step 1: Add the label handle**

At `main/kiss_recv.c:115`, extend the declaration to `static lv_obj_t *s_state_chip, *s_chain_lbl;`.

A stale pointer must not survive a screen swap, so null it everywhere its neighbours are nulled — lines **147, 807, 814 and 816**. Lines 147 and 814 already read `s_state_chip = NULL;`; make both `s_state_chip = s_chain_lbl = NULL;`. Lines 807 and 816 read `s_addr_card = s_cmp_lbl = s_addr_more = NULL;`; add `s_chain_lbl = NULL;` beside each.

- [ ] **Step 2: Create it under the QR in `recv_detail_open`**

Immediately after the `wt_qr_card(s_scr, &s_qr, 48, 112, 238, 202);` call:

```c
  // Under the QR, in the band the card leaves free: it ends at 314 and content
  // runs to WT_CONTENT_BOTTOM. This is the screen's KNOWLEDGE, not this
  // address's state -- the chip beside the address owns that -- so it sits with
  // the QR and not in the right column. font14 is the intended size here: it is
  // metadata, and the sentence an owner must read to act is the chip.
  s_chain_lbl = wt_lbl(s_scr, "", 48, 322, wt_font14(), WT_MUT);
  lv_obj_set_width(s_chain_lbl, 238);
  lv_label_set_long_mode(s_chain_lbl, LV_LABEL_LONG_WRAP);
```

- [ ] **Step 3: Replace the chip block in `recv_refresh`**

Replace the whole `if (s_state_chip) { ... }` block (currently `main/kiss_recv.c:259`, along with the comment above it that argues for two states) with:

```c
  // The state chip, and the one line that says where its confidence came from.
  //
  // USED and UNUSED are not the same kind of claim and never were. Spending an
  // output proves it was funded, so an index this device signed from is USED on
  // its own evidence -- mark_used_receives records exactly that. UNUSED is a
  // negative claim about the chain, and a signer has no chain view to make it
  // with. The screen used to make both from one number, which is how a wallet
  // with history at 0..29 read UNUSED on all thirty.
  //
  // So the third state is not an absence. Drawing nothing was honest about the
  // address and silent about the silence: the screen looked finished and
  // nothing on it said the answer was obtainable.
  {
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    int net = kiss_testnet() ? 1 : 0, sc = kiss_script();
    int high = kiss_usage_high(fp, net, sc);
    int chain = -1; uint32_t cheight = 0;
    int known = kiss_usage_chain_known(fp, net, sc, &chain, &cheight);
    if (known && chain > high) high = chain;

    if (s_state_chip) {
      const char *txt; lv_color_t col;
      if ((int)s_idx <= high)  { txt = tr(STR_R_HANDED_ALREADY); col = WT_WARN; }
      else if (known)          { txt = tr(STR_R_NEVER_HANDED);   col = WT_OK;   }
      else                     { txt = tr(STR_R_USAGE_UNKNOWN);  col = WT_MUT;  }
      wt_state_chip_set(s_state_chip, txt, col);
      // Right aligned to x=752 on the ADDRESS #N row. Recomputed every refresh
      // because all three labels differ in length, and per locale.
      lv_obj_set_pos(s_state_chip, 752 - lv_obj_get_width(s_state_chip), 96);
    }

    if (s_chain_lbl) {
      char buf[128];
      if (known && chain >= 0)
        snprintf(buf, sizeof buf, tr(STR_R_CHAIN_UPTO_FMT), (unsigned)chain);
      else
        snprintf(buf, sizeof buf, "%s",
                 tr(known ? STR_R_CHAIN_CLEAN : STR_R_CHAIN_ASK));
      wt_note_fit(s_chain_lbl, buf, 238, WT_CONTENT_BOTTOM - 322);
    }
  }
```

- [ ] **Step 4: Build the sim and look at the frame**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ && bash sim/build_sim.sh && "$KISS_SIM_TMP/fruitsim"
```

Then open `$KISS_SIM_TMP/sim_recv.ppm`. Expected: the chip on the address row reads USAGE UNKNOWN in the muted ink, and the line under the QR asks for a coordinator, wrapped to at most two lines and clear of y=398. The house rules require looking at this frame — no gate sees type size or overlap here.

- [ ] **Step 5: Commit**

```bash
git add main/kiss_recv.c
git commit -F - <<'MSG'
the receive chip stops claiming a chain view this device does not have

A wallet with real history at 0..29 read UNUSED on all thirty, because the
screen made two different claims out of one number. Spending an output proves
it was funded, so an index this device signed from is USED on its own evidence.
UNUSED is a negative claim about the chain and needs something that can see the
chain. Those are not the same kind of statement and they no longer share a
branch.

The third state is a muted chip and not an absence. Drawing nothing was honest
about the address and silent about its own silence: the screen looked finished,
and nothing on it would have told an owner the answer was obtainable at all.
USAGE UNKNOWN is not a claim about the address, it is the device saying what it
has not been told, which is the one thing it can say truthfully there.

The line under the QR carries where the confidence came from, in the band the
QR card leaves free at 314. It reports "used up to" rather than a count,
because a high water mark cannot see gaps and thirty addresses were not
necessarily used.

The chip's position is still recomputed every refresh. It always was, for two
labels and 21 locales; there are three labels now and the reason is unchanged.
MSG
```

---

## Task 5: Reading the payload at VERIFY

**Files:** Modify `main/kiss_recv.c`

- [ ] **Step 1: Route ahead of the address path**

At the top of `vfy_result` (`main/kiss_recv.c:356`), before the existing `vfy_norm` call, insert:

```c
  // A coordinator's usage payload rides in the same QR as the address, so this
  // runs on the RAW text before vfy_norm touches it. Anything without the magic
  // falls straight through to the address path, unchanged.
  //
  // The index in the payload says NOTHING about this address. It is a fact
  // about the wallet, and vfy_find goes on re-deriving and searching exactly as
  // it did: the ownership answer is what this screen exists for and nothing
  // scanned is allowed to shortcut it.
  kiss_usage_msg_t um;
  int have_um = kiss_usage_parse(txt, len, &um) == 0;
  int um_recorded = 0, um_mine = 0, um_view = 0;
  if (have_um) {
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    um_mine = memcmp(fp, um.fp, 4) == 0;
    // Stored in the bucket the payload NAMES, which may not be the one this
    // screen is showing. A different purpose is a different account key, so a
    // coordinator on another address type really is watching other keys, and
    // the note below says so rather than reporting an update over a chip that
    // did not move.
    um_view = um_mine && um.testnet == (kiss_testnet() ? 1 : 0) &&
              um.script == kiss_script();
    if (um_mine)
      um_recorded = kiss_usage_chain_set(um.fp, um.testnet, um.script,
                                         um.high, um.height);
    txt = um.addr;                 // the address half, for the check below
    len = strlen(um.addr);
  }
```

`(void)len;` at the head of the function must go, since `len` is now read.

- [ ] **Step 2: Say what happened, under the ownership answer**

At the end of `vfy_result`, immediately before the `again` pill is created:

```c
  // One muted line, under the answer the owner came for. A mismatched
  // fingerprint does not refuse the scan: the ownership question is answerable
  // without trusting anything in the payload, and refusing would withhold the
  // one answer this screen owes.
  if (have_um) {
    char note[160];
    if (!um_view)
      snprintf(note, sizeof note, "%s", tr(STR_R_UM_OTHER_KEYS));
    else if (!um_recorded)
      snprintf(note, sizeof note, "%s", tr(STR_R_UM_NO_CHANGE));
    else if (um.high >= 0)
      snprintf(note, sizeof note, tr(STR_R_CHAIN_UPTO_FMT), (unsigned)um.high);
    else
      snprintf(note, sizeof note, "%s", tr(STR_R_CHAIN_CLEAN));
    lv_obj_t *l = wt_lbl(s_scr, "", 48, WT_ACTION_Y - 40, wt_font14(), WT_MUT);
    lv_obj_set_width(l, 700);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    wt_note_fit(l, note, 700, 34);
  }
```

- [ ] **Step 2a: Add the two result-screen strings**

Re-run Task 3's insertion script with `NEW` replaced by:

```python
NEW = collections.OrderedDict([
    ("R_UM_OTHER_KEYS", "that coordinator is watching different keys"),
    ("R_UM_NO_CHANGE",  "already up to date; nothing moved"),
])
```

then `python3 tools/gen_i18n.py`.

- [ ] **Step 3: Prove the index cannot shortcut the search — append to `sim/test_usage.c`**

```c
// The payload's index is a fact about the WALLET, not about the address riding
// with it. This pins the separation at the data layer: a payload may claim any
// index and the address half is untouched by it.
static void test_index_does_not_describe_address(void)
{
    kiss_usage_msg_t m;
    uchk("a wild index leaves the address intact",
         P("KISSU1 EC5A4595 1 0 99999 1234567 TB1QREALADDRESS") == 0 &&
         m.high == 99999 &&
         strcmp(m.addr, "TB1QREALADDRESS") == 0);
}
```

Add `test_index_does_not_describe_address();` to `test_usage()`.

- [ ] **Step 4: Run the gates that can see this**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ \
  && bash sim/build_test.sh && "$KISS_SIM_TMP/kisstest" | tail -3 \
  && bash sim/build_fitcheck.sh && "$KISS_SIM_TMP/kissfit" | tail -20
```

Expected: `kisstest` ends with zero failures. `kissfit` reports no new ellipsis and nothing newly at font14 other than the two lines that are deliberately font14 — if a new string lands at font14 in a body, the copy is too long and words get cut rather than the box being grown.

- [ ] **Step 5: Commit**

```bash
git add main/kiss_recv.c sim/test_usage.c i18n/ main/i18n_keys.h main/i18n_tables.c
git commit -F - <<'MSG'
one scan answers both questions the receive screen is asked

The usage rides in the QR the owner already scans, so VERIFY does both jobs in
the gesture the pairing screen already teaches. Its own copy has said "prove
it: RECEIVE > VERIFY" for as long as it has existed; this makes the badge
update a side effect of the check they came to do rather than a second errand
with a second square nothing on the screen mentions.

Routing runs on the RAW scanned text, ahead of vfy_norm. Anything without the
magic falls straight through and the address path is untouched.

The index in the payload says nothing about the address riding with it. It is a
fact about the wallet; vfy_find goes on re-deriving and searching exactly as it
did, and a test pins that a payload claiming index 99999 leaves the address
half intact. The ownership answer is what this screen is FOR and nothing
scanned gets to shortcut it.

A mismatched fingerprint does not refuse the scan. That question is answerable
without trusting a byte of the payload, and refusing would withhold the one
answer this screen owes in order to punish the half that is only ever display
data. The usage is dropped, the line says whose keys those were, and the
ownership answer lands as normal.

A payload for another address type is stored in the bucket it names and
reported the same way. A different purpose is a different account key, so that
coordinator really is watching other keys -- and saying "updated" over a chip
that did not move would be the more confusing of the two answers.
MSG
```

---

## Task 6: The walk stop *(contended file — do last, droppable)*

**Files:** Modify `sim/sim_main.c`

`sim/sim_main.c` carries in-flight edits on `develop`. This task is isolated so it can be rebased or dropped without touching anything above it.

- [ ] **Step 1: Photograph the three states**

Beside the existing VERIFY injections (`sim/sim_main.c:2266`), add a stop that injects a `KISSU1` payload and saves the receive detail before and after, so the walk carries a frame of each chip state:

```c
  // The three states the chip can be in. Injected rather than derived, because
  // a coordinator is the only thing that can produce the middle one and the sim
  // has no chain either.
  save("/tmp/sim_recv_usage_unknown.ppm");
  const char *usage = "KISSU1 EC5A4595 1 0 29 1234567 TB1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4";
  kiss_scan_inject(usage, strlen(usage)); pump(6);
  save("/tmp/sim_recv_usage_recorded.ppm");
```

The literal `/tmp/sim_*.ppm` form is correct here: `save()` (`sim/sim_main.c:874`) remaps the path under `KISS_SIM_TMP` itself, which is why every surrounding call site writes it that way.

- [ ] **Step 2: Run the walk gates**

```bash
export KISS_SIM_TMP=/tmp/kiss-$$ \
  && bash sim/build_sim.sh && bash sim/run_overlapcheck.sh \
  && "$KISS_SIM_TMP/fruitsim" && python3 tools/check_sim_taps.py \
  && python3 tools/check_screen_coverage.py
```

Expected: overlapcheck reports 0 findings in en, taps that hit nothing is 0, coverage reports 0 never-opened screens.

- [ ] **Step 3: Commit**

```bash
git add sim/sim_main.c
git commit -m "the walk carries a frame of each state the receive chip can be in"
```

---

## Verification

Run all of it from the worktree, isolated:

```bash
export KISS_SIM_TMP=/tmp/kiss-$$
bash sim/build_test.sh && "$KISS_SIM_TMP/kisstest"
bash sim/build_fitcheck.sh && "$KISS_SIM_TMP/kissfit"
bash sim/build_themecheck.sh && "$KISS_SIM_TMP/kisstheme"
bash sim/build_osdcheck.sh && "$KISS_SIM_TMP/kissosd"
bash sim/build_sim.sh && bash sim/run_overlapcheck.sh
"$KISS_SIM_TMP/fruitsim" && python3 tools/check_sim_taps.py
python3 tools/check_screen_coverage.py
python3 tools/gen_docs_shots.py --check
```

Anything touching `main/` also runs the device compiler — it is the only lane with `-Wformat-truncation`, and this change adds `snprintf` into fixed buffers on a screen that takes translated format strings, which is exactly the class that gate catches:

```bash
docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.0.1 idf.py -B /tmp/idfbuild-usage build
```

`-B /tmp/idfbuild-usage` rather than the documented `/tmp/idfbuild`: `KISS_SIM_TMP` does not cover the docker build directory, and two builds sharing one would walk each other's objects.

**Look at the frame.** `$KISS_SIM_TMP/sim_recv.ppm` after `fruitsim` — the chip and the line under the QR. No gate sees type size, and the house rules exist because every one of those was visible at a glance and caught by nobody.

## Device test verdict

**DEVICE TEST: REQUIRED.** New payload through the camera decode path, new copy on a dense screen. The simulator does not compile `main/camera_spike.c`, so no gate above can see whether the camera reads this off a real monitor. Passing gates are not this verdict.

Flows to run on hardware:

1. Scan a real coordinator payload at RECEIVE > VERIFY — confirm it answers ownership **and** the chip changes.
2. Read the chip on an address above the mark (UNUSED), at or below it (ALREADY USED), and on a device that has never been told (USAGE UNKNOWN).
3. Confirm the line under the QR fits in 238 without passing two lines, and clears `WT_CONTENT_BOTTOM`.
4. Scan a payload whose fingerprint does not match — the ownership answer must still land.
5. Scan the same QR twice — the second reports that nothing moved.
6. Switch the signer's address type in Settings, then scan a payload for the old type — the note must say it is other keys, and the chip on screen must not move. `um_view` is UI logic in `kiss_recv.c`, which `kisstest` does not link, so this case has no unit test and only hardware or the sim walk can show it.

## Open item for the coordinator half

`address --qr` emits the payload once a base branch is stable. It needs `parse_kiss_descriptor` to return the fingerprint and purpose it already validates at `src/lib.rs:66`, `wallet.spk_index().last_used_index(KeychainKind::External)`, and `wallet.latest_checkpoint().height()` — both verified present in `bdk_wallet` 3.1.0 / `bdk_chain` 0.23.3. It must refuse when the height is 0. Decide the base once `signet-support` and `silent-payments` land.
