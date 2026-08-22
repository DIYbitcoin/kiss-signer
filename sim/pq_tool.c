// The other end of main/kiss_pqsig.c: the release side of the post quantum
// firmware signature.
//
// One implementation signs and the device verifies, which is the arrangement
// that hides its own bugs -- so neither side is trusted on its own. Both are
// held to NIST's FIPS 205 vectors in sim/test_pq.c, and `verify` here exists so
// a release can be checked with the same code the device runs before anybody
// puts a card in a slot.
//
// Build: sim/build_pqtool.sh
//
//   pq_tool keygen <out.key>          mint a release key. Secret to out.key
//                                     (0600); public key printed as hex.
//   pq_tool pubkey <key>              the public half, as hex
//   pq_tool header <key>              main/pq_release_pubkey.h, on stdout
//   pq_tool sign   <key> <image.bin>  append the 8192 byte trailer, in place
//   pq_tool verify <key> <image.bin>  check one, exactly as the device does
//
// The secret key never leaves the maintainer's machine; only the public half
// enters the repository, the same arrangement minisign already has here.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "slh_dsa.h"
#include "sha2_api.h"
#include "pq_hw_sha.h"
#include "kiss_pqsig.h"

#define KEY_MAGIC "KPQK"
#define KEY_LEN   (4 + 1 + 64 + 32)     // magic | version | sk | pk

static const slh_param_t *PRM = &slh_dsa_sha2_128s;

static int die(const char *what)
{
    fprintf(stderr, "pq_tool: %s%s%s\n", what,
            errno ? ": " : "", errno ? strerror(errno) : "");
    return 1;
}

// The system CSPRNG and nothing else. A release key minted from anything
// weaker is a release key somebody else can also mint.
static int rbg(uint8_t *x, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(x, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}

static int read_key(const char *path, uint8_t sk[64], uint8_t pk[32])
{
    uint8_t buf[KEY_LEN];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (got != sizeof buf || memcmp(buf, KEY_MAGIC, 4) != 0 || buf[4] != 1)
        return -1;
    if (sk) memcpy(sk, buf + 5, 64);
    if (pk) memcpy(pk, buf + 5 + 64, 32);
    // The public half is stored beside the secret one for convenience, so it
    // has to be the half that belongs to it -- a mismatched pair signs releases
    // no device can ever check. FIPS 205 puts the private key together as
    // SK.seed | SK.prf | PK.seed | PK.root, and the last 32 of those bytes ARE
    // the public key, so the two are comparable with no derivation at all.
    return memcmp(buf + 5 + 32, buf + 5 + 64, 32) == 0 ? 0 : -1;
}

// SHA-256 of a whole file, streamed. The device computes the same digest as the
// image goes past it into flash, which is the only way it can: the card cannot
// seek and the image does not fit in RAM.
static int digest_file(const char *path, size_t take, uint8_t out[32], size_t *total)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha2_256_t sha;
    sha2_256_init(&sha);
    static uint8_t buf[65536];
    size_t seen = 0;
    for (;;) {
        size_t want = sizeof buf;
        if (take && seen + want > take) want = take - seen;
        if (want == 0) break;
        size_t n = fread(buf, 1, want, f);
        if (n == 0) break;
        sha2_256_update(&sha, buf, n);
        seen += n;
    }
    if (!take) {
        // Consume the rest so *total is the file, not the part hashed.
        for (;;) {
            size_t n = fread(buf, 1, sizeof buf, f);
            if (n == 0) break;
            seen += n;
        }
    } else {
        fseek(f, 0, SEEK_END);
        seen = (size_t)ftell(f);
    }
    fclose(f);
    sha2_256_final(&sha, out);
    if (total) *total = seen;
    return 0;
}

static void put_hex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02X", b[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: pq_tool keygen <out.key>\n"
            "       pq_tool pubkey <key>\n"
            "       pq_tool header <key>\n"
            "       pq_tool sign   <key> <image.bin>\n"
            "       pq_tool verify <key> <image.bin>\n");
        return 2;
    }
    const char *cmd = argv[1], *keyp = argv[2];

    if (strcmp(cmd, "keygen") == 0) {
        uint8_t sk[64], pk[32], buf[KEY_LEN];
        if (slh_keygen(sk, pk, rbg, PRM) != 0) return die("keygen failed");
        memcpy(buf, KEY_MAGIC, 4);
        buf[4] = 1;
        memcpy(buf + 5, sk, 64);
        memcpy(buf + 5 + 64, pk, 32);
        // O_EXCL, and 0600 at creation rather than a chmod afterwards. A
        // release key is not recoverable and the whole update story rests on
        // it: a second keygen over the first strands every device carrying the
        // old public half, and a key that is briefly world readable has been
        // world readable.
        int fd = open(keyp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return die(keyp);
        FILE *f = fdopen(fd, "wb");
        if (!f) return die(keyp);
        size_t w = fwrite(buf, 1, sizeof buf, f);
        if (fclose(f) != 0 || w != sizeof buf) return die("write failed");
        fprintf(stderr, "wrote %s (keep it, there is no second copy)\n", keyp);
        put_hex(pk, 32);
        return 0;
    }

    uint8_t sk[64], pk[32];
    if (read_key(keyp, sk, pk) != 0)
        return die("not a pq_tool key file (or the pair does not match)");

    if (strcmp(cmd, "pubkey") == 0) { put_hex(pk, 32); return 0; }

    if (strcmp(cmd, "header") == 0) {
        printf("// GENERATED by sim/pq_tool.c -- do not edit.\n");
        printf("//\n");
        printf("// The SLH-DSA-SHA2-128s public key this firmware checks update"
               " images against.\n");
        printf("// All zeroes means NO KEY, and a build with no key refuses to"
               " install anything\n");
        printf("// rather than installing whatever it is handed -- see"
               " kiss_pqsig_available().\n");
        printf("//\n");
        printf("// The SECRET half never enters this repository. Public keys do:"
               " docs/installer/\n");
        printf("// kiss_signer.pub is here for the same reason.\n");
        printf("#pragma once\n#include <stdint.h>\n\n");
        printf("static const uint8_t PQ_RELEASE_PUBKEY[32] = {\n");
        for (int i = 0; i < 32; i += 12) {
            printf("   ");
            for (int j = i; j < i + 12 && j < 32; j++) printf(" 0x%02X,", pk[j]);
            printf("\n");
        }
        printf("};\n");
        return 0;
    }

    if (argc < 4) return die("need an image");
    const char *img = argv[3];

    if (strcmp(cmd, "sign") == 0) {
        uint8_t digest[32];
        size_t len = 0;
        if (digest_file(img, 0, digest, &len) != 0) return die(img);
        if (len == 0) return die("empty image");

        static uint8_t trailer[KISS_PQSIG_TRAILER_LEN];
        memset(trailer, 0, sizeof trailer);
        memcpy(trailer, KISS_PQSIG_MAGIC, KISS_PQSIG_MAGIC_LEN);
        trailer[4] = KISS_PQSIG_SCHEME_SLH_DSA_SHA2_128S;
        trailer[6] = (uint8_t)(KISS_PQSIG_SIG_LEN & 0xFF);
        trailer[7] = (uint8_t)(KISS_PQSIG_SIG_LEN >> 8);

        pq_hw_sha_begin();
        size_t n = slh_sign(trailer + KISS_PQSIG_HDR_LEN, digest, 32,
                            (const uint8_t *)KISS_PQSIG_CTX,
                            sizeof KISS_PQSIG_CTX - 1, sk, NULL, PRM);
        pq_hw_sha_end();
        if (n != KISS_PQSIG_SIG_LEN) return die("sign failed");

        // Check before writing, with the device's own code. A release that
        // ships a trailer nothing can verify is a release that bricks the
        // update path for everyone who installs it.
        kiss_pqsig_test_set_pubkey(pk);
        if (kiss_pqsig_check(digest, trailer, sizeof trailer) != KISS_PQSIG_OK)
            return die("the signature does not verify against its own key");

        FILE *f = fopen(img, "ab");
        if (!f) return die(img);
        size_t w = fwrite(trailer, 1, sizeof trailer, f);
        if (fclose(f) != 0 || w != sizeof trailer) return die("append failed");
        fprintf(stderr, "signed %s: %zu byte image + %u byte trailer\n",
                img, len, (unsigned)KISS_PQSIG_TRAILER_LEN);
        return 0;
    }

    if (strcmp(cmd, "verify") == 0) {
        // Deliberately NOT an fseek to the tail. This streams the file through
        // kiss_pqsig_stream_*, the same splitter kiss_fw_install feeds off the
        // card, so what is checked here is the code path the device takes and
        // not a convenient re-reading of it. The card cannot seek; neither does
        // this.
        static kiss_pqsig_stream_t st;
        kiss_pqsig_stream_init(&st);
        FILE *f = fopen(img, "rb");
        if (!f) return die(img);
        static uint8_t buf[4096];       // FW_CHUNK, as kiss_fw.c reads it
        for (;;) {
            size_t n = fread(buf, 1, sizeof buf, f);
            if (n == 0) break;
            if (kiss_pqsig_stream_feed(&st, buf, n, NULL, NULL) != 0) {
                fclose(f); return die("stream failed");
            }
        }
        fclose(f);

        uint8_t digest[32];
        const uint8_t *trailer = NULL;
        size_t tlen = 0;
        if (kiss_pqsig_stream_end(&st, digest, &trailer, &tlen) != KISS_PQSIG_OK)
            return die("no trailer, or a file shorter than one");

        kiss_pqsig_test_set_pubkey(pk);
        int rc = kiss_pqsig_check(digest, trailer, tlen);
        if (rc != KISS_PQSIG_OK) {
            fprintf(stderr, "pq_tool: %s does NOT verify (kiss_pqsig rc %d)\n", img, rc);
            return 1;
        }
        fprintf(stderr, "pq_tool: %s verifies (%llu byte image)\n",
                img, (unsigned long long)kiss_pqsig_stream_image_len(&st));
        return 0;
    }

    return die("unknown command");
}
