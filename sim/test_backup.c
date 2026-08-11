// Desktop tests for main/kiss_backup.c: "has this wallet's paper ever been
// proven against this device".
//
// The fact this module holds used to be a session flag that reset on every
// login-screen close and could only be set from inside the setup wizard, so the
// SETTINGS backup row was amber no matter what the owner did. These tests pin
// the three properties that made it worth persisting: it survives, it is per
// wallet, and it goes when the wallet does.
#include <stdio.h>
#include <string.h>

#include "kiss_backup.h"
#include "kiss_seed.h"

// sim_main.c: the paper check record carries a master fingerprint, so it is
// only written when flash encryption is under it. Tests drive both lanes.
void kiss_seed_test_set_flash_encrypted(int on);

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
    //
    // Flash encryption ON for this block: the record carries a master
    // fingerprint, so a plaintext lane deliberately keeps it in session RAM and
    // writes nothing. The unencrypted lane is tested at the end.
    kiss_seed_test_set_flash_encrypted(1);
    bchk("backup: KEEP mode for the persisting tests",
         kiss_seed_set_mode(WSEED_MODE_KEEP) == 0);
    kiss_backup_forget();

    bchk("backup: nothing checked to begin with", !kiss_backup_checked(fp_a));

    kiss_backup_mark(fp_a);
    bchk("backup: marked wallet reads checked", kiss_backup_checked(fp_a));

    // The whole point of keying on the fingerprint: a decoy or second
    // passphrase wallet answers for its own paper, not for wallet A's.
    bchk("backup: a different wallet is still unchecked",
         !kiss_backup_checked(fp_b));
    kiss_backup_mark(fp_b);
    bchk("backup: second wallet marks independently",
         kiss_backup_checked(fp_b));
    bchk("backup: marking the second did not disturb the first",
         kiss_backup_checked(fp_a));

    // An all-zero fingerprint is what kiss_ui reports before any unlock. It
    // must not accidentally share a slot with a real wallet.
    bchk("backup: the empty fingerprint is unchecked",
         !kiss_backup_checked(fp_zero));

    bchk("backup: marking is idempotent",
         (kiss_backup_mark(fp_a), kiss_backup_checked(fp_a)));

    kiss_backup_forget();
    bchk("backup: forget clears wallet A", !kiss_backup_checked(fp_a));
    bchk("backup: forget clears wallet B", !kiss_backup_checked(fp_b));

    // AMNESIC promises a device that gets searched holds no wallet metadata, so
    // the mark lives in session RAM only and dies with the mode change, exactly
    // as kiss_usage does.
    bchk("backup: switch to AMNESIC", kiss_seed_set_mode(WSEED_MODE_AMNESIC) == 0);
    kiss_backup_mark(fp_a);
    bchk("backup: amnesic still answers within the session",
         kiss_backup_checked(fp_a));
    bchk("backup: back to KEEP", kiss_seed_set_mode(WSEED_MODE_KEEP) == 0);
    bchk("backup: the amnesic mark never reached storage",
         !kiss_backup_checked(fp_a));

    kiss_backup_forget();

    // The unencrypted lane, which is what a beta device actually runs. A record
    // keyed by master fingerprint proves which wallet was used, and a second
    // one in the same namespace proves a passphrase wallet exists at all, so
    // below encrypted flash nothing may be written. It stays answerable for the
    // session and goes with the power.
    //
    // Reading it back cannot use the same lane: with persistence off the reader
    // deliberately answers from session RAM, which would say yes no matter what
    // storage held. So mark with encryption off, then turn it on and read. That
    // sends the reader to NVS, and a false there is proof nothing was written.
    kiss_seed_test_set_flash_encrypted(0);
    bchk("backup: unencrypted KEEP still answers within the session",
         (kiss_backup_mark(fp_a), kiss_backup_checked(fp_a)));
    kiss_seed_test_set_flash_encrypted(1);
    bchk("backup: unencrypted mark wrote nothing to storage",
         !kiss_backup_checked(fp_a));

    kiss_backup_forget();
    kiss_seed_test_set_flash_encrypted(0);   // leave the sim on the beta lane
    return bfails;
}
