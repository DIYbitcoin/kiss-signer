// Source: BIP39 words the owner drew off paper cards and typed in by hand.
// This judges the draw BEFORE the checksum word joins it and makes every set
// look finished. Mirrors wallet_dice_q: pure, no UI, host-testable.
//
// Deliberately a separate file from wallet_lastword.c: that one pulls in
// wally_bip39.h and is stubbed out by the simulator, while this one needs no
// crypto and no wordlist at all — so the sim links the REAL judgement and every
// gate renders the true verdicts in all 21 locales. Same split, same reason, as
// wallet_dice_q.c beside wallet_dice.c.
//
// It is also separate because the two modules must be free to DISAGREE about
// the same input. "abandon" x11 is the canonical BIP39 zero entropy vector, so
// wallet_lastword_candidates() must keep answering 128 for it forever — and it
// is also a seed with no secret in it, so this module must keep refusing it.
// sim/test_cards_q.c pins both halves of that sentence.
#pragma once
#include <stdint.h>

#define WC_MIN    11    // typed words for a 12 word seed; below this, nothing to judge
#define WC_MAX    23    // typed words for a 24 word seed
#define WC_LIST 2048    // the null model's alphabet: the BIP39 English wordlist

// |index gap| that counts two consecutive draws as neighbours: a word and the
// eight either side of it, a 17 word window, one 120th of the list. Chosen with
// the counts below because 8 is the WIDEST window where both seed sizes still
// land on the dice judge's 1 in a million line — and a wider window catches
// more real failures, so width is worth buying up to that limit.
#define WC_NEAR    8

// Per size thresholds: the smallest count whose false alarm rate under a blind
// draw is at or under 1.5e-6. The two sizes differ because the integers do not
// line up, not because two ideas are in play — see the table in
// wallet_cards_q.c. Sizes between 11 and 23 cannot be reached from the UI and
// are judged on the 11 word constants, whose rate at those sizes is not
// characterised.
#define WC_DUP_MIN(n)   ((n) >= WC_MAX ? 5u : 3u)   // excess copies
#define WC_NEAR_MIN(n)  ((n) >= WC_MAX ? 5u : 4u)   // neighbouring pairs

enum { WC_Q_SHORT = 0,  // fewer than WC_MIN words: nothing to judge yet
       WC_Q_OK,
       WC_Q_SAME,       // every word is the same word
       WC_Q_PERIOD,     // one short run, typed over and over
       WC_Q_CLUSTER,    // the draws sit side by side on the list
       WC_Q_SORTED,     // list order, so only the multiset survives
       WC_Q_DUP };      // words repeat more than a full deck would

// Every rule that fired, not just the headline one. `verdict` is the most
// severe; the UI headlines that and the tests pin all of them, so a change to
// the precedence order cannot quietly change what is DETECTED.
#define WC_F_SAME     (1u << 0)
#define WC_F_PERIOD   (1u << 1)
#define WC_F_CLUSTER  (1u << 2)
#define WC_F_SORTED   (1u << 3)
#define WC_F_DUP      (1u << 4)

// The rules that REFUSE, as one definition rather than a test at each call
// site. Only the two that prove the set carries nothing: a false block costs a
// redraw, where a false warn costs one tap. Adding WC_F_CLUSTER here is a
// deliberate one line change and nothing else moves — its rate (9.5e-7) is
// already inside the block rules' budget.
#define WC_F_BLOCK    (WC_F_SAME | WC_F_PERIOD)

typedef struct {
    int      verdict;    // WC_Q_*
    unsigned flags;      // WC_F_*, every rule that fired
    unsigned n;          // words judged
    unsigned distinct;   // how many different words
    unsigned dups;       // n - distinct: excess copies
    unsigned period;     // 0, or the block length the sequence repeats at
    unsigned near;       // consecutive pairs within WC_NEAR of each other
    int      sorted;     // 0, +1 non decreasing, -1 non increasing
} wallet_cards_q_t;

// Judge a typed draw. `idx` is one BIP39 wordlist index per word, in the order
// the owner typed them. Pure: no globals, no clock, no crypto, no wordlist,
// integer only. Fills every field of `out`.
void wallet_cards_judge(const uint16_t *idx, unsigned n, wallet_cards_q_t *out);

// 1 when the verdict refuses. The UI must not spell this out itself; see
// WC_F_BLOCK above for why there is exactly one definition.
int  wallet_cards_blocked(const wallet_cards_q_t *q);

// ---- what this cannot catch ------------------------------------------------
// Every rule here asks one question: does this sequence carry STRUCTURE a blind
// draw would not produce? A set with no structure passes, and "no structure" is
// not the same as "no attacker knows it".
//
//   * A memorised phrase, a song lyric, a famous quote, or any published test
//     vector that is not one word repeated. All of these look exactly like a
//     blind draw here, and all of them are already written down somewhere.
//   * A set the owner chose word by word while feeling random. Human choice has
//     no signature at 11 samples.
//   * A set somebody else handed the owner.
//   * A loose cluster typed in scattered order. The neighbour rule reads
//     CONSECUTIVE pairs, so only a tight cluster cannot escape it. The stronger
//     statistic is the RANGE of the indices, which at the same 1e-6 line would
//     flag a window of ~405 at 11 words; it is deliberately NOT implemented,
//     and this line is the record of that gap rather than a claim it is absent.
//   * A constant step wider than WC_NEAR. Every tenth card off an unshuffled
//     deck carries about 11 bits and passes every rule below.
//
// So the module makes one promise and no other: a set it BLOCKS carries no
// secret at all, and a set it WARNS about carries provably less than the owner
// thinks. It never promises the reverse.
