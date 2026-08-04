// Desktop tests for main/wallet_backup.c: "has this wallet's paper ever been
// proven against this device".
//
// The fact this module holds used to be a session flag that reset on every
// login-screen close and could only be set from inside the setup wizard, so the
// SETTINGS backup row was amber no matter what the owner did. These tests pin
// the three properties that made it worth persisting: it survives, it is per
// wallet, and it goes when the wallet does.
#include <stdio.h>
#include <string.h>

#include "wallet_backup.h"
#include "wallet_seed.h"

static int bfails;

static void bchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); bfails++; }
}

int test_backup_layer(void) {
    const uint8_t fp_a[4] = { 0xEC, 0x5A, 0x45, 0x95 };
    const uint8_t fp_b[4] = { 0x73, 0xC5, 0xDA, 0x0A };
    const uint8_t fp_zero[4] = { 0, 0, 0, 0 };

    // KEEP is the mode that persists. Start from a known-empty table.
    bchk("backup: KEEP mode for the persisting tests",
         wallet_seed_set_mode(WSEED_MODE_KEEP) == 0);
    wallet_backup_forget();

    bchk("backup: nothing checked to begin with", !wallet_backup_checked(fp_a));

    wallet_backup_mark(fp_a);
    bchk("backup: marked wallet reads checked", wallet_backup_checked(fp_a));

    // The whole point of keying on the fingerprint: a decoy or second
    // passphrase wallet answers for its own paper, not for wallet A's.
    bchk("backup: a different wallet is still unchecked",
         !wallet_backup_checked(fp_b));
    wallet_backup_mark(fp_b);
    bchk("backup: second wallet marks independently",
         wallet_backup_checked(fp_b));
    bchk("backup: marking the second did not disturb the first",
         wallet_backup_checked(fp_a));

    // An all-zero fingerprint is what wallet_ui reports before any unlock. It
    // must not accidentally share a slot with a real wallet.
    bchk("backup: the empty fingerprint is unchecked",
         !wallet_backup_checked(fp_zero));

    bchk("backup: marking is idempotent",
         (wallet_backup_mark(fp_a), wallet_backup_checked(fp_a)));

    wallet_backup_forget();
    bchk("backup: forget clears wallet A", !wallet_backup_checked(fp_a));
    bchk("backup: forget clears wallet B", !wallet_backup_checked(fp_b));

    // AMNESIC promises a device that gets searched holds no wallet metadata, so
    // the mark lives in session RAM only and dies with the mode change, exactly
    // as wallet_usage does.
    bchk("backup: switch to AMNESIC", wallet_seed_set_mode(WSEED_MODE_AMNESIC) == 0);
    wallet_backup_mark(fp_a);
    bchk("backup: amnesic still answers within the session",
         wallet_backup_checked(fp_a));
    bchk("backup: back to KEEP", wallet_seed_set_mode(WSEED_MODE_KEEP) == 0);
    bchk("backup: the amnesic mark never reached storage",
         !wallet_backup_checked(fp_a));

    wallet_backup_forget();
    return bfails;
}
