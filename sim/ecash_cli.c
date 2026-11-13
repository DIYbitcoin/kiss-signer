// sim/ecash_cli.c: the ecash spend signer on the desktop, for interop tests
// against a real mint. Uses the DEV seed the test runner leaves stored under
// KISS_SIM_TMP (run kisstest first, same KISS_SIM_TMP). Never a real seed.
//
//   ecashcli xpub          prints the ecash export
//   ecashcli pub <counter> prints the lock key at counter
//   ecashcli sign          reads a spend request on stdin, prints the
//                          summary on stderr and the reply on stdout
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wally_bip32.h>

#include "kiss_crypto.h"
#include "kiss_ecash.h"
#include "kiss_simpath.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ecashcli xpub | pub <counter> | sign\n");
        return 2;
    }
    kiss_sim_lock("ecashcli");
    if (kiss_session_open(NULL) != 0) {
        fprintf(stderr, "no dev seed stored: run kisstest first\n");
        return 1;
    }
    int rc = 0;
    if (!strcmp(argv[1], "xpub")) {
        char out[160];
        rc = kiss_session_ecash_export(out, sizeof out);
        if (rc == 0)
            printf("%s\n", out);
    } else if (!strcmp(argv[1], "pub") && argc > 2) {
        struct ext_key acct;
        uint8_t pub[33];
        rc = kiss_ecash_account(kiss_session_master(), &acct) ||
             kiss_ecash_pubkey(&acct, (uint32_t)strtoul(argv[2], NULL, 10), pub);
        if (rc == 0) {
            for (int i = 0; i < 33; i++)
                printf("%02x", pub[i]);
            printf("\n");
        }
    } else if (!strcmp(argv[1], "sign")) {
        static char req[KISS_ECASH_REQ_MAX + 2];
        size_t n = fread(req, 1, sizeof req, stdin);
        kiss_ecash_summary_t sum;
        rc = kiss_ecash_review(req, n, &sum);
        if (rc != 0) {
            fprintf(stderr, "review refused: %d\n", rc);
            return 1;
        }
        fprintf(stderr, "mint %s unit %s: %d in, leaving %llu, change %llu\n",
                sum.mint, sum.unit, sum.n_in,
                (unsigned long long)sum.leaving, (unsigned long long)sum.change);
        char reply[256];
        rc = kiss_ecash_sign_reviewed(reply, sizeof reply);
        if (rc == 0)
            printf("%s\n", reply);
        else
            fprintf(stderr, "sign refused: %d\n", rc);
    } else {
        rc = 2;
    }
    kiss_session_close();
    return rc ? 1 : 0;
}
