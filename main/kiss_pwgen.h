// The backup password the DEVICE makes, so the owner does not have to invent
// a strong one.
//
// A KEF envelope (kiss_kef.h) is meant to be carried: printed, photographed,
// left on a card in a drawer. Everything protecting it is PBKDF2 over a
// password the owner chose, so a carried QR is an offline target that can be
// ground at whatever rate the attacker's hardware allows. The meter in
// kiss_ui.c judges what they typed, which helps; it cannot make a memorable
// password strong, and "invent a strong password and still have it in five
// years" is the request that produces `bitcoin2024`.
//
// So the device offers to draw one. Five words from the BIP39 list already in
// flash is 55 bits, uniform, and it is written down rather than remembered --
// which is the honest model for a thing opened once in five years.
//
// 2048 is a power of two, so 11 bits IS a word index: no rejection sampling,
// no modulo bias, and the whole draw is 7 bytes. Nothing here is a seed and
// nothing here is stored; the string is shown once, sealed into the envelope
// and wiped.
//
// Pure and host testable, kiss_dice / kiss_lastword's shape: the draw takes
// bytes rather than reaching for the TRNG, so a test can hand it a known
// stream and assert the exact indices.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Five words, 11 bits each. Not a knob: the copy on the screen says five, and
// four would be 44 bits, which is inside reach of a rented GPU rig against
// KEF's iteration count.
#define PWGEN_WORDS 5

// Bytes the draw consumes. 5 x 11 = 55 bits, so the 8th bit of the last byte
// is unused and deliberately ignored rather than folded in.
#define PWGEN_BYTES 7

// Longest string: five 8-char words, four spaces, NUL.
#define PWGEN_MAX   46

// Split `rnd` into PWGEN_WORDS wordlist indices, big-endian bit order.
// Returns 0, or -1 on NULL or fewer than PWGEN_BYTES bytes.
int kiss_pwgen_draw(const uint8_t *rnd, size_t n, uint16_t idx[PWGEN_WORDS]);

// The indices as one space separated string. 0 on success, -1 on NULL, a
// short buffer, or an index the wordlist does not answer to.
int kiss_pwgen_join(const uint16_t idx[PWGEN_WORDS], char *out, size_t out_len);

// Draw against the device TRNG and join. -1 when the TRNG has not been
// started (kiss_crypto.h): a password minted from a source that was never
// switched on is the one failure this must never hand back looking valid.
int kiss_pwgen_make(char *out, size_t out_len);
