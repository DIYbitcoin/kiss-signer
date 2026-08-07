// The last word of a BIP39 mnemonic is part checksum: after the first 11 or
// 23 words, only 128 (resp. 8) of the 2048 list words complete a valid set.
// This module enumerates them, so the owner picks the last word and no
// machine randomness enters a cards built seed.
// See docs/superpowers/specs/2026-08-04-cards-lastword-design.md
// Mirrors wallet_dice: pure, no UI, host testable.
#pragma once
#include <stdint.h>

#define WLAST_MAX 128   // 11 word prefix leaves 7 free bits -> 2^7 candidates

// `partial` = 11 or 23 space separated BIP39 words. Fills out[] with every
// wordlist index whose word completes a checksum valid mnemonic, ascending.
// Returns the count (128 or 8 when every word is on the list), 0 when some
// word is off the list, -1 on NULL or a word count that is not 11 or 23.
int wallet_lastword_candidates(const char *partial, uint16_t out[WLAST_MAX]);

// The BIP39 word at index 0..2047. Stable pointer into wally's own list,
// never freed (bip39_get_word_by_index). NULL out of range.
const char *wallet_lastword_word(uint16_t index);

// Wordlist index of `w`, or -1 when it is not an English BIP39 word. The
// inverse of wallet_lastword_word, which wally does not expose. Binary search:
// the English list is lexicographic, and sim/test_lastword.c asserts that
// outright so the search can never outlive its premise.
int wallet_lastword_index(const char *w);

// ---- this module answers a maths question and does not judge the answer ----
// "abandon" x11 is the canonical BIP39 zero entropy vector and candidates()
// must keep returning 128 for it forever. Whether a set of words is worth
// making a seed from is wallet_cards_q.h's question, deliberately asked in a
// different module so the two can disagree about the same input.
