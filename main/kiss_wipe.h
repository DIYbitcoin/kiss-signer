// One secure-zero, for every translation unit that holds a secret.
//
// memset can be optimized away the moment the compiler can see the buffer is
// dead, and every wipe of a secret in this tree is exactly that shape: the
// last read of the seed, the mnemonic, the passphrase or a source of entropy
// is the line above the wipe. A volatile store loop is not removable, and it
// needs no library.
//
// libwally has wally_bzero and the device build links it, but the SIMULATOR
// build does not link libwally into kiss_ui.c, kiss_info.c, kiss_setup.c,
// kiss_word_ui.c or kiss_scan.c -- so "just call wally_bzero" breaks the
// desktop simulator that every gate in this project runs through. Hence a
// header both sides can include. Device-only files that already call
// wally_bzero keep it; there is nothing to gain from churning correct calls.
//
// This replaces four identical static copies (kiss_setup.c wz_bzero,
// kiss_scan.c scan_bzero, kiss_seed_sd.c sd_bzero, kiss_sp.c sp_bzero), the
// first of which had a comment asking for exactly this.
#ifndef KISS_WIPE_H
#define KISS_WIPE_H

#include <stddef.h>
#include <stdint.h>

static inline void kiss_wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}

#endif  // KISS_WIPE_H
