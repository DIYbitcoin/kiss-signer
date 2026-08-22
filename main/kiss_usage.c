// See kiss_usage.h. Device backend = NVS namespace "kissu" (separate from the
// "kiss" namespace so a usage wipe can erase-all without touching settings or
// the seed). Host backend = a small RAM table (the desktop tests and the sim
// run in one process, so persistence-across-boot is a device-only concern).
#include "kiss_usage.h"
#include "kiss_payee.h"   // kiss_persist_apply speaks for both stores
#include "kiss_seed.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// key = "<fp8hex><net><type>", e.g. "ec5a459501" -> 10 chars (NVS limit 15)
static void usage_key(const uint8_t fp[4], int testnet, int script, char out[16])
{
    snprintf(out, 16, "%02x%02x%02x%02x%d%d",
             fp[0], fp[1], fp[2], fp[3], testnet ? 1 : 0, script);
}

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
    while (len && (txt[len - 1] == 0    || txt[len - 1] == '\n' ||
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
    // Low bound 1, not 0: a coordinator that has never synced has no claim to
    // make, and a zero would win the height gate against nothing and then block
    // the first real one behind it.
    if (!(p = utok(p, end, f, sizeof f)) || udec(f, 1, 100000000L, &v)) return -1;
    out->height = (uint32_t)v;

    if (!(p = utok(p, end, out->addr, sizeof out->addr))) return -1;
    while (p < end && *p == ' ') p++;
    return p == end ? 0 : -1;                   // trailing junk is malformed
}

#define UMAX 32
// chain/cheight hold what a coordinator claimed, beside — never merged into —
// the witnessed mark in v. cheight doubles as the presence flag: a real payload
// always carries a height of at least 1, so zero means nobody has spoken and no
// second byte has to stay in agreement with this one.
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

#ifdef ESP_PLATFORM
#include "nvs.h"

// ---- batching ----
// A burst of marks used to pay for its own open/commit/close each: a
// multi-input spend marks one receive per input, and a session flush marks up
// to UMAX rows. begin opens one handle and defers the commit; end commits once
// and closes. Power lost mid burst loses the batch -- these are reuse-guard
// marks, best effort, and the RAM session table still carries them.
static nvs_handle_t s_batch = 0;
static bool s_batch_active = false;

void kiss_usage_batch_begin(void)
{
    if (s_batch_active) return;
    s_batch_active = nvs_open("kissu", NVS_READWRITE, &s_batch) == ESP_OK;
}

void kiss_usage_batch_end(void)
{
    if (!s_batch_active) return;
    nvs_commit(s_batch);
    nvs_close(s_batch);
    s_batch_active = false;
}

static int persistent_high(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READONLY, &h) != ESP_OK)
        return -1;
    uint32_t v = 0;
    int rc = nvs_get_u32(h, key, &v) == ESP_OK ? (int)v : -1;
    nvs_close(h);
    return rc;
}

static void persistent_mark(const char *key, uint32_t idx)
{
    nvs_handle_t h;
    bool own = !s_batch_active;
    if (own && nvs_open("kissu", NVS_READWRITE, &h) != ESP_OK)
        return;
    if (!own) h = s_batch;
    uint32_t cur = 0;
    bool have = nvs_get_u32(h, key, &cur) == ESP_OK;
    if (!have || idx > cur) {           // monotonic: never lower the high-water mark
        nvs_set_u32(h, key, idx);
        if (own) nvs_commit(h);
    }
    if (own) nvs_close(h);
}

// Two more keys in the same namespace, so the erase-all in persistent_wipe and
// the PERSIST switch cover them with no path of their own. "c" holds the
// claimed index plus one (0 alone could not be told from an unset key) and "h"
// holds the height it was claimed at.
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

static void persistent_wipe(void)
{
    kiss_usage_batch_end();             // a pending batch must not resurrect after the wipe
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

#else   // host (sim + desktop tests): RAM table

void kiss_usage_batch_begin(void) {}
void kiss_usage_batch_end(void)   {}

static struct usage_row s_persistent[UMAX];
static int s_persistent_n;

static int persistent_high(const char *key)
{
    return tab_high(s_persistent, s_persistent_n, key);
}

static void persistent_mark(const char *key, uint32_t idx)
{
    tab_mark(s_persistent, &s_persistent_n, key, idx);
}

static void chain_persistent_get(const char *key, uint32_t *chain, uint32_t *cheight)
{
    tab_chain_get(s_persistent, s_persistent_n, key, chain, cheight);
}

static void chain_persistent_set(const char *key, uint32_t chain, uint32_t cheight)
{
    tab_chain_set(s_persistent, &s_persistent_n, key, chain, cheight);
}

static void persistent_wipe(void)
{
    memset(s_persistent, 0, sizeof s_persistent);
    s_persistent_n = 0;
}

#endif

// The key is built from the master fingerprint, so writing one to plaintext
// NVS records WHICH wallet was used. On a device with a passphrase that is the
// whole game: a decoy holds its own fingerprint, and a second one in the same
// namespace proves a second wallet exists, then serves as an offline oracle to
// grind passphrases against. That defeats what kiss_duress.h is for.
//
// So persistence needs encrypted flash under it, not merely a non AMNESIC mode.
// On the beta lane this costs the cross boot memory of the receive high water
// mark; within a session the table below still carries it. A convenience is the
// right thing to lose here.
//
// Read and write move together. Gating only the write would leave the reader
// asking NVS for a record nothing writes any more, so the mark would appear to
// vanish the instant it was made — a silently dead feature rather than a
// deliberately session scoped one.
static bool may_persist(void)
{
    return kiss_persist_enabled() &&
           kiss_seed_mode() != WSEED_MODE_AMNESIC && kiss_seed_flash_encrypted();
}

// ---- the PERSIST preference ----
// Lives here rather than in kiss_settings.c because this module is linked into
// every lane (device, sim, tests) and is one of the gates the flag feeds;
// kiss_settings.c is UI-only, absent from the test binary, and its store_u8
// asks the same accessor before writing a settings byte.
static uint8_t s_persist = 1;

int  kiss_persist_enabled(void)        { return s_persist; }
void kiss_persist_set_enabled(int on)  { s_persist = on != 0; }

void kiss_persist_apply(int on)
{
    s_persist = on != 0;
    if (!s_persist) {
        // The wipes take the session tables too, deliberately: reads promote
        // persisted rows into session RAM, so sparing the session copy would
        // spare exactly the data the owner just asked to destroy. Marks made
        // from here on still guard within this session.
        kiss_usage_wipe();
        kiss_payee_wipe();
    } else {
        kiss_usage_persist_session();
        kiss_payee_persist_session();
    }
}

int kiss_usage_high(const uint8_t fp[4], int testnet, int script)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    if (!may_persist())
        return tab_high(s_session, s_session_n, key);
    int v = persistent_high(key);
    if (v >= 0) tab_mark(s_session, &s_session_n, key, (uint32_t)v);
    return v;
}

void kiss_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    tab_mark(s_session, &s_session_n, key, idx);
    if (may_persist())
        persistent_mark(key, idx);
}

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
    if (high)   *high   = (int)c - 1;      // stored as high+1, so 0 stays free
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
    // a reorg or a rebuilt wallet has to be able to come DOWN; an old QR shown
    // again cannot, because it loses the height comparison. Monotonic on the
    // index would have made a wrong value permanent until a wipe.
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

// Second door onto the same NVS keys: the mode change paths in kiss_seed.c
// flush the whole session table at once. Gating only kiss_usage_mark would
// leave every fingerprint to land here instead.
void kiss_usage_persist_session(void)
{
    if (!may_persist())
        return;
    kiss_usage_batch_begin();
    for (int i = 0; i < s_session_n; i++) {
        persistent_mark(s_session[i].key, s_session[i].v);
        // The claim rides the same flush, or a mode change would drop what it
        // had just promoted into the session table.
        if (s_session[i].cheight)
            chain_persistent_set(s_session[i].key, s_session[i].chain,
                                 s_session[i].cheight);
    }
    kiss_usage_batch_end();
}

void kiss_usage_forget_session(void)
{
    memset(s_session, 0, sizeof s_session);
    s_session_n = 0;
}

void kiss_usage_wipe(void)
{
    persistent_wipe();
    kiss_usage_forget_session();
}
