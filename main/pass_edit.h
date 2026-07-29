// Passphrase buffer edits at a caret.
//
// Split out of wallet_ui.c and kept free of LVGL and of globals so the
// arithmetic that decides what a passphrase actually CONTAINS can be tested on
// its own (sim/test_passedit.c). Everything else about the entry screen is a
// display concern; this is not. An off-by-one here shows nothing on screen,
// because the field is a row of dots, and produces a passphrase the owner
// cannot reproduce, which is a wallet nobody can reopen.
//
// The caller owns the buffer, the length and the caret, and the caret is an
// insertion point in 0..len. len means "at the end", which is the ordinary
// typing case, so the common path needs no special handling.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Insert one character before *caret. False when the buffer is already full,
// in which case nothing is touched.
static inline bool pass_edit_insert(char *buf, int cap, int *len, int *caret,
                                    char c)
{
    if (*len >= cap) return false;
    // +1 carries the terminator along with the tail
    memmove(buf + *caret + 1, buf + *caret, (size_t)(*len - *caret) + 1);
    buf[*caret] = c;
    (*len)++;
    (*caret)++;
    return true;
}

// Delete the character BEFORE *caret, which is what backspace means whether
// the caret is at the end or somewhere a tap put it. False at the start of the
// buffer, where there is nothing behind the caret to remove.
static inline bool pass_edit_delete(char *buf, int *len, int *caret)
{
    if (*caret <= 0) return false;
    memmove(buf + *caret - 1, buf + *caret, (size_t)(*len - *caret) + 1);
    (*len)--;
    (*caret)--;
    return true;
}
