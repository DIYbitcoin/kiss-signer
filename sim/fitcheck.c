// Explainer fit report: does each body of copy render at font28 (readable at
// arm's length) or fall back to font14, in every locale?
//
// wt_body_font() makes that decision at runtime from lv_text_get_size in the
// ACTIVE locale's font, so guessing line widths by hand is worthless — a line
// one character too long wraps and silently costs a whole 37px row. This tool
// asks LVGL the same question the UI does, for all 21 locales at once, and
// prints the measured height against the budget so a translation can be
// shortened by exactly as much as it overflows.
//
// Build: bash sim/build_fitcheck.sh   Run: /tmp/kissfit [locale ...]
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "i18n.h"
#include "wallet_theme.h"

// Every wt_body_font() call site in the UI: the copy it measures and the
// (width, height) box it has to live inside. Keep in sync with the sources
// listed in the comment beside each entry.
typedef struct {
    const char *surface;   // where the user sees it
    int key;               // STR_* (or -1 for a composed body)
    int w, h;
    int may_be_small;      // 1 = font14 is the accepted outcome here
} slot_t;

static const slot_t SLOTS[] = {
    // wallet_setup.c:279 — amber line under the word grid
    { "setup/paper-only", STR_W_PAPER_ONLY, 700,  40 },
    // wallet_info.c — "?" cards (155 with a diagram, 225 without; the
    // scan-key warning below pairs a short body with three visual facts)
    { "wallet/?fp",       STR_I_H_FP_B,     720, 155 },
    { "wallet/?type",     STR_I_H_TYPE_B,   720, 225 },
    { "wallet/?pair",     STR_I_H_PAIR_B,   720, 155 },
    { "wallet/?addr",     STR_I_H_ADDR_B,   720, 225 },
    // wallet_info.c:263,326 — full-screen warnings
    { "wallet/sp-warn",   STR_R_SP_WARN_B,  700, 145 },
    { "wallet/sp-find",   STR_R_SP_FACT_FIND,     482, 29 },
    { "wallet/sp-spend",  STR_R_SP_FACT_NO_SPEND, 482, 29 },
    { "wallet/sp-forever",STR_R_SP_FACT_FOREVER,  482, 29 },
    { "wallet/words-warn",STR_I_WARN_B,     700, 270 },
    // wallet_sign.c:355,394,966
    { "sign/why",         -1,               720, 300 },   // composed below
    { "sign/rbf-on",      STR_S_RBF_B_ON,   720, 230 },
    { "sign/rbf-off",     STR_S_RBF_B_OFF,  720, 230 },
    { "sign/?coord",      STR_S_COORD_B,    720, 144 },
    // wallet_ui.c:469,769 — login warning + passphrase intro
    { "login/warn",       STR_L_WARN_B,     740, 204 },
    // The passphrase intro and the backup check both went from one 704px
    // paragraph to a PAIR of wt_why_blocks, so each body is measured in the
    // narrower box it actually renders in: 344 wide less the block's own 14px
    // inset, and 166 tall less the font14 heading above it (HEAD_ROOM 46) and
    // the 8px wt_why_block costs for its own metrics. A body that only fits at
    // 704 wide is exactly the regression this row exists to catch.
    { "login/pp-w1",      STR_L_PPINTRO_W1_B, 330, 112 },
    { "login/pp-w2",      STR_L_PPINTRO_W2_B, 330, 112 },
    { "setup/verify-w1",  STR_W_VINTRO_W1_B,  330, 112 },
    { "setup/verify-w2",  STR_W_VINTRO_W2_B,  330, 112 },
    // wallet_setup.c — wizard explainers
    { "setup/checksum",   STR_W_CHECK_B,    704, 256 },
    { "setup/verify-ok",  STR_W_VOK_B,      704, 190 },
    { "setup/verify-bad", STR_W_VBAD_B,     704, 190 },
    // Three-mode storage appears in both setup and Settings with the same
    // side-by-side geometry, so all four notes are measured at the same box.
    // The SD note used to carry a line about the mode being unavailable in
    // normal builds. It is not: wallet_seed_sd_supported() returns true on
    // every build, and the unreachable disabled path was deleted, so the note
    // is now ordinary copy with nothing special about it.
    { "storage/flash",    STR_W_KEEP_NOTE,        420, 87 },
    { "storage/flash-enc",STR_W_FLASH_ENC_NOTE,   420, 87 },
    { "storage/sd",       STR_W_SD_NOTE,          420, 87 },
    { "storage/amnesic",  STR_W_AMNESIC_NOTE,     420, 87 },
    { "storage/current",  STR_G_STORAGE_CURRENT_FMT, 704, 30 },
    { "storage/confirm-flash", STR_G_STORAGE_CONFIRM_FLASH_B,   704, 238 },
    { "storage/confirm-sd",    STR_G_STORAGE_CONFIRM_SD_B,      704, 238 },
    { "storage/confirm-amn",   STR_G_STORAGE_CONFIRM_AMNESIC_B, 704, 238 },
    { "storage/ok",       STR_G_STORAGE_OK_FMT,       704, 230 },
    { "storage/ok-amn",   STR_G_STORAGE_OK_AMNESIC_B, 704, 230 },
    { "storage/fail-card",STR_G_STORAGE_FAIL_CARD_B,  704, 230 },
    { "storage/fail-vfy", STR_G_STORAGE_FAIL_VERIFY_B,704, 230 },
    { "storage/fail",     STR_G_STORAGE_FAIL_GENERIC_B,704,230 },
    { "storage/cleanup",  STR_G_STORAGE_CLEANUP_B,    704, 230 },
    { "storage/sd-missing",STR_W_SD_MISSING_B,         704, 226 },
    { "storage/sd-corrupt",STR_W_SD_CORRUPT_B,         704, 226 },
    { "storage/sd-io",    STR_W_SD_IO_B,              704, 226 },
    // wallet_settings.c: the two wipe overlays
    { "wipe/confirm",     STR_G_WIPEC_B,    704, 190 },
    { "wipe/erased",      STR_G_ERASED_B,   704, 160 },
    { "wipe/not-erased",  STR_G_NOERASE_B,  704, 160 },
    // amnesic mode: seed-QR import + passphrase-from-QR
    { "setup/qr-bad",     STR_W_QRBAD_B,    704, 240 },
    // wallet_sign.c: the screens BEFORE and AFTER the detail page. The detail
    // page was swept first and these were missed, so the refusal to sign, the
    // two SD prompts and every instruction on the signed-QR page were still at
    // font14 -- on the flow that moves money.
    { "sign/point-cam",   STR_S_POINT_CAM,      322, 116 },
    { "sign/or-load",     STR_S_OR_LOAD,        322,  58 },
    { "sign/read-fail",   STR_S_READ_FAIL,      704, 232 },
    { "sign/rm-confirm",  STR_S_RM_C_B,         704, 100 },
    { "sign/not-psbt",    STR_S_NOT_PSBT,       704, 232 },
    { "sign/scan-bad",    STR_S_SCAN_NOT_PSBT,  704, 232 },
    { "sign/insert-card", STR_S_INSERT_CARD,    704, 116 },
    { "sign/sparrow-save",STR_S_SPARROW_SAVE,   704, 116 },
    // wallet_sign.c sd_open — the one hint line at y=98, drawn at font14 by
    // design (may_be_small), one line wide as the whole content lane. The %d
    // pair expands to at most 2 digits each, no wider than the specifiers.
    { "sign/files-more",  STR_S_FILES_MORE_FMT, 704, 29, 1 },
    { "sign/qr-loop",     STR_S_QR_LOOP,        322,  29 },
    // wallet_sign.c glossary_cb() -- SIMPLE EXPLAINERS, eight definitions in a
    // 704x294 overlay. Registered at 232, not the 294 the screen allows: eight
    // lines at font23 is exactly 8 x 29, so the box IS the no-wrap condition
    // and one wrapped definition (261) fails here. Written that way because
    // 294 hid the real state of this page -- eleven locales were rendering the
    // whole thing at font14 because enough definitions wrapped to overflow at
    // 23, and a definition that wraps mid-clause is the one place a glossary
    // must not be hard to read.
    { "sign/glossary",    STR_S_GLOSSARY_B,     704, 232 },
    { "sign/no-network",  STR_S_NO_NETWORK,     322,  29 },
    { "sign/ez-note",     STR_S_EZ_NOTE,        322,  87 },
    { "sign/saved-note",  STR_S_SAVED_NOTE,     704,  90 },
    { "login/qr-warn",    STR_L_SCAN_WARN_B,704, 274 },
    // wallet_settings.c — the notes under each chooser. These sit in gaps
    // between controls, so 23 (not 28) is the realistic top rung; what matters
    // is that none of them falls to 14.
    { "set/net-main",     STR_G_MAINNET_NOTE, 340, 50 },
    { "set/net-test",     STR_G_TESTNET_NOTE, 340, 58 },
    // ADDRESS TYPE is a full-width subpage now, not three pills crammed into a
    // 360px column, so these notes stopped being 82px-wide fragments that had
    // no rung above 14 to reach. They get the 664x29 the subpage actually draws
    // (wallet_settings.c type_open_cb) and no may_be_small: what the user is
    // choosing between must be readable.
    { "set/ty-native",    STR_G_TY_NATIVE_NOTE, 664, 29 },
    { "set/ty-nested",    STR_G_TY_NESTED_NOTE, 664, 29 },
    { "set/ty-legacy",    STR_G_TY_LEGACY_NOTE, 664, 29 },
    // set/separate and sub/addr-type used to sit here, both measuring
    // STR_G_SEPARATE, "each network + type is its own separate wallet". The
    // string is gone: it was the ADDRESS TYPE subtitle and, doing second duty,
    // the filler in a decoy session's duress slot. It told nobody anything the
    // three rows above already say. Neither box has a string to measure now.
    { "set/create-note",  STR_G_CREATE_NOTE,  340, 34, 1 },
    { "set/words-note",   STR_I_WORDS_BTN_NOTE,340,34, 1 },
    // wallet_duress_ui.c ST_DONE: wt_why_body at y=250 under the two-ways
    // diagram, so the body has WT_CONTENT_BOTTOM - 250 = 148 to live in.
    { "duress/done",      STR_GD_DONE_B,      700, 148, 0 },
    // wallet_recv.c / wallet_info.c — instructions the user has to act on
    // wt_screen() subtitles: one line, 704px wide, between title and content.
    { "sub/receive",      STR_R_S,            704, 30, 0 },
    { "sub/wallet",       STR_I_S,            340, 58, 0 },
    { "sub/verify",       STR_R_VS,           704, 30, 0 },
    { "sub/words-warn",   STR_I_WARN_S,       704, 30, 0 },
    { "sub/sp-export",    STR_R_SP_EXPORT_S,  704, 30, 0 },
    { "sub/sp-warn",      STR_R_SP_WARN_S,    704, 30, 0 },
    { "sub/pair",         STR_I_PAIR_S,       704, 30, 0 },
    // Every remaining wt_screen subtitle. One line at 23 or it drops to 14 --
    // the header geometry in wt_screen() is fixed, so the only lever here is
    // the length of the sentence.
    { "sub/words",        STR_I_WORDS_S,      704, 30, 0 },
    { "sub/setup",        STR_W_SETUP_S,      704, 30, 0 },
    { "sub/write",        STR_W_WRITE_S,      704, 30, 0 },
    // These two draw their own subtitle (mk_screen2 in wallet_setup.c) because
    // their first content sits well below the y=96 line, so they get the two
    // lines their copy was written for.
    { "sub/rand",         STR_W_RAND_S,       704, 58, 0 },
    { "sub/prove",        STR_W_PROVE_S,      704, 58, 0 },
    { "sub/proof",        STR_W_PROOF_S,      704, 58, 0 },
    // CAMERA AUDIT (docs/specs/prove-it.md). The why pair shares rule 2's 330x112
    // body budget; the burned line takes the paper-only slot; the notes live
    // in wt_row_x subs (380-wide file row, 716-wide gate rows, both 96 tall)
    // and under the 300-wide viewfinder, so font14 is an accepted outcome
    // there. The hash caption shares its card row with the mono14 filename,
    // so it is measured in the width that leaves.
    { "sub/proof-r",      STR_W_PROOF_R_S,       704,  30, 0 },
    { "proof/check",      STR_W_PROOF_CHECK_B,   330, 112 },
    { "proof/burn",       STR_W_PROOF_BURN_B,    330, 112 },
    { "proof/burned",     STR_W_PROOF_BURNED,    700,  40 },
    { "proof/file-note",  STR_W_PROOF_FILE_NOTE, 282,  80, 1 },
    { "proof/sd",         STR_W_PROOF_SD_B,      620,  46, 1 },
    { "proof/fail",       STR_W_PROOF_FAIL_B,    620,  46, 1 },
    { "proof/saving",     STR_W_PROOF_SAVING,    300,  40, 1 },
    { "proof/hash-cap",   STR_W_PROOF_HASH_CAP,  545,  40, 1 },
    // dice screens: never registered before the quality check landed, which is
    // how the samey nudge shipped unmeasured. The verdict subtitles are one
    // line on wt_screen; the verify note gets two card lines; the two why
    // blocks share rule 2's 330x112 body budget.
    { "sub/dice",         STR_W_DICE_S,           704, 30, 0 },
    { "sub/dice-uneven",  STR_W_DICE_UNEVEN_S,    704, 30, 0 },
    { "sub/dice-pattern", STR_W_DICE_PATTERN_S,   704, 30, 0 },
    { "setup/dice-verify",STR_W_DICE_VERIFY_NOTE, 564, 36, 1 },
    { "setup/dice-w1",    STR_W_DICE_W1_B,        330, 112 },
    { "setup/dice-w2",    STR_W_DICE_W2_B,        330, 112 },
    { "sub/restore",      STR_W_RESTORE_S,    704, 30, 0 },
    // cards mode (MY OWN WORDS): subtitles, the method-row note, both why
    // pairs and the checksum page's one number line. The candidate pills are
    // dynamic English BIP39 words and are deliberately not rows here.
    { "sub/cards",        STR_W_CARDS_S,      704, 30, 0 },
    { "sub/cksum",        STR_W_CKSUM_S,      704, 30, 0 },
    { "sub/cards-pick",   STR_W_CARDS_PICK_S, 704, 30, 0 },
    { "setup/cards-note", STR_W_CARDS_NOTE,   420, 87 },
    { "setup/cards-w1",   STR_W_CARDS_W1_B,   330, 112 },
    { "setup/cards-w2",   STR_W_CARDS_W2_B,   330, 112 },
    { "setup/cksum-w1",   STR_W_CKSUM_W1_B,   330, 112 },
    { "setup/cksum-w2",   STR_W_CKSUM_W2_B,   330, 112 },
    { "setup/cksum-fit",  STR_W_CKSUM_FIT_FMT, 704, 20, 1 },
    { "sub/vfy-backup",   STR_W_VERIFY_S,     704, 30, 0 },
    { "sub/qr-warn",      STR_L_SCAN_WARN_S,  704, 30, 0 },
    { "sub/ppintro",      STR_L_PPINTRO_S,    704, 30, 0 },
    // and the signing flow's own subtitles
    { "sub/get-tx",       STR_S_GET_TX,       704, 30, 0 },
    { "sub/choose-file",  STR_S_CHOOSE_FILE,  704, 30, 0 },
    { "sub/sd",           STR_S_SD_SUB,       704, 30, 0 },
    { "sub/qr-out",       STR_S_QR_SUB,       704, 30, 0 },
    { "sub/done-sd",      STR_S_DONE_SD_SUB,  704, 58, 0 },  // own 2-line subtitle
    { "sub/qr-fail",      STR_S_QR_FAIL_ENC,  704, 30, 0 },
    // Procedural, read once with the device in hand, and wedged into a 360px
    // column beside a QR. They auto-fit like everything else, so they grow if
    // the copy is ever shortened -- but font14 is the accepted answer today.
    // recv/verify measured STR_R_VERIFY_NOTE, a sentence explaining the VERIFY
    // button. The note was cut long ago and the string is now gone too, so
    // this slot was measuring text no screen drew. The button remains, with
    // its own label, on the receive detail screen.
    // Under the QR on the address detail screen. This is the whole standing
    // privacy reminder now, so the zoomable smaller card gives it three lines
    // at font23.
    { "recv/one-each",    STR_R_ONE_EACH,     308, 87, 0 },
    { "pair/sparrow",     STR_I_NOTE_SPARROW, 360, 86, 1 },
    { "pair/bluewallet",  STR_I_NOTE_BW,      360, 86, 1 },
    { "pair/prove",       STR_I_PROVE,        360, 72, 1 },
    // wallet_info.c — the note under each action pill
    // Raised out of a hardcoded font14 in the readability sweep. Listed here
    // so the boxes they were given are checked against every translation, not
    // just the English they were measured with.
    { "login/cancel",     STR_L_CANCEL_SETUP_B, 500, 124 },
    { "login/fail-setup", STR_L_FAIL_SETUP_B,   720,  58 },
    { "login/fail-open",  STR_L_FAIL_OPEN_B,    720,  58 },
    { "login/weak",       STR_L_WEAK_ACK,       640, 124 },
    // The keyboard caption when it is carrying state, not a field name: 486px
    // is what is left of the row once SCAN and SHOW take the right side.
    { "login/cap-again",  STR_L_TYPE_AGAIN,     486,  58 },
    { "login/cap-nomatch",STR_L_NO_MATCH,       486,  58 },
    { "login/cap-badpass",STR_L_BACKUP_PASS_BAD,486,  58 },
    { "login/cap-verify", STR_L_VERIFY_PASS,    486,  58 },
    { "login/fp-note",    STR_L_FP_NOTE,        700,  58 },
    { "login/fp-note2",   STR_L_FP_NOTE2,       700,  58 },
    { "scan/sub",         STR_N_S,              530,  29 },
    // wallet_recv.c sp_help_cb(): the sp1-vs-bc1p explainer overlay. Measured
    // with the raw "%s" in place, which is ~2px narrower per prefix than the
    // 3 to 4 characters that get substituted, so this reads slightly optimistic.
    { "recv/sp-why",      STR_R_SP_WHY_B,       720, 238 },
    { "wallet/sp-note",   STR_R_SP_EXPORT_NOTE, 360, 140 },
    // Same string, second home: the note under SCAN KEY on the WALLET page.
    // That box is the tighter of the two, so measuring only the 360x140 one
    // let this render at 14 next to a PAIR COORDINATOR note at 23.
    { "wallet/sp-btn",    STR_R_SP_EXPORT_NOTE, 340,  90 },
    { "wallet/pair-note", STR_I_PAIR_BTN_NOTE,  340,  62 },
    // The two word-count notes sit in the 80px gaps of a three-pill stack (12
    // WORDS at y=150, 24 WORDS at 230, SCAN SEED QR at 310) in a 340px column.
    // Both run to three or four lines at 23 -- 87px and 116px -- so neither can
    // reach it without restacking the page or cutting the copy. Same situation
    // as the ADDRESS TYPE notes above, and recorded for the same reason.
    { "setup/12-note",    STR_W_12_NOTE,        340,  76, 1 },
    { "setup/24-note",    STR_W_24_NOTE,        340,  76, 1 },
};
#define NSLOT ((int)(sizeof SLOTS / sizeof SLOTS[0]))

// wt_row LABELS, which nothing measured until a rename made one of them fail.
//
// These are not pills and not bodies: wt_row_x draws the label at a FIXED
// font23, pinned to one line, with LV_LABEL_LONG_DOT. There is no font ladder
// to fall down, so an over-long translation does not shrink and does not wrap
// -- it silently ellipsises, and it looks completely deliberate. No gate saw
// it. overlapcheck compares boxes and the box is exactly the width it was
// given; fitcheck only knew about copy that shrinks. So "Address type" drew as
// "Address t..." in ENGLISH, on the default screen, and the walk was clean.
//
// The budget is wt_row_x's own arithmetic, not a guess:
//   right = w - 10 - chevron - 10;  lw = right - vw - (vw ? 12 : 0) - 14
// so the value competes with the label for the row, which is why a row whose
// value is TRANSLATED has to be measured with its longest value.
typedef struct {
    const char *surface;
    int key;               // STR_* of the label
    int val_key;           // STR_* of the widest value it sits against, or -1
    const char *val_lit;   // ... or an untranslated literal, or NULL
    int w;                 // the row's width
} row_t;
static const row_t ROWS[] = {
    // wallet_settings.c, left column at SG_L_W = 365
    { "set/network",  STR_I_ROW_NETWORK, -1, NULL,    365 - 176 },  // segmented track
    { "set/type",     STR_I_ROW_TYPE,    -1, "m/n...", 365 },
    { "set/storage",  STR_I_ROW_STORAGE, STR_W_AMNESIC_BTN, NULL, 365 },
    { "set/duress",   STR_I_ROW_DURESS,  STR_GD_OFF, NULL, 365 },
    // right column, SG_R_W = 365. None of these carry a value.
    { "set/words",    STR_I_ROW_WORDS,   -1, NULL, 365 },
    { "set/replace",  STR_I_ROW_REPLACE, -1, NULL, 365 },
    { "set/erase",    STR_I_ROW_ERASE,   -1, NULL, 365 },
};
#define NROW ((int)(sizeof ROWS / sizeof ROWS[0]))

// Row labels that ellipsise TODAY, recorded the day the check was written.
//
// Shrink only, like OC_BARE_BACKLOG in sim/overlapcheck.c. A check that starts
// out failing gets switched off, and a check that starts out silent never
// catches the next one, so it starts out honest instead: everything already
// broken is listed by name, anything not on the list fails the build, and the
// run prints how many are left. Delete a line when the copy is fixed. Never
// add one.
//
// Two rows account for almost all of it. "Words live in" and "Duress wallet"
// are short in English and become a clause in most other languages, and they
// sit against a translated value ("AMNESIC", "NOT SET") that eats the same
// row. Fixing them means shorter labels in ten locales or a wider left column,
// which is a copy pass of its own, not a rename.
static const struct { const char *lang, *surface; } ROW_BACKLOG[] = {
    { "cs-CZ", "set/duress" },
    { "de",    "set/duress" },  { "de",    "set/storage" },
    { "es-ES", "set/duress" },  { "es-ES", "set/storage" },
    { "es-MX", "set/duress" },  { "es-MX", "set/storage" },
    { "fr",    "set/duress" },  { "fr",    "set/storage" },
    { "hr-HR", "set/duress" },
    { "it",    "set/duress" },  { "it",    "set/storage" },
    // 324px against a 317px budget: seven pixels, on the row that erases.
    { "it",    "set/erase"  },
    { "nl",    "set/duress" },  { "nl",    "set/storage" },
    { "pl",    "set/duress" },
    { "pt-BR", "set/duress" },  { "pt-BR", "set/storage" },
    { "pt-PT", "set/duress" },  { "pt-PT", "set/storage" },
    { "ru",    "set/duress" },  { "ru",    "set/storage" },
    { "tr",    "set/duress" },  { "tr",    "set/storage" },
};
#define NROW_BACKLOG ((int)(sizeof ROW_BACKLOG / sizeof ROW_BACKLOG[0]))

static bool row_backlogged(const char *lang, const char *surface)
{
    for (int i = 0; i < NROW_BACKLOG; i++)
        if (strcmp(ROW_BACKLOG[i].lang, lang) == 0 &&
            strcmp(ROW_BACKLOG[i].surface, surface) == 0)
            return true;
    return false;
}

// The label box wt_row_x will give this row, in the ACTIVE locale.
static int row_label_budget(const row_t *r)
{
    lv_point_t sz;
    lv_text_get_size(&sz, LV_SYMBOL_RIGHT, wt_font23(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int right = r->w - 10 - (int)sz.x - 10;
    int vw = 0;
    const char *val = r->val_lit ? r->val_lit
                    : r->val_key >= 0 ? tr(r->val_key) : NULL;
    if (val && *val) {
        lv_text_get_size(&sz, val, wt_font23(), 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        vw = (int)sz.x + 12;
    }
    return right - vw - 14;
}

// Pill labels. A button must never be smaller than the note beside it, and
// notes cap at 23, so font14 here is a FAILURE: it means the box is too narrow
// for that translation and the pill needs widening (or the word shortening).
// `key_action` marks a button whose label the user has to READ to act, and act
// correctly: the one that spends, the one that erases, the one that proves an
// address. Those may never render at font14 in any locale -- if one does, this
// program exits nonzero and CI stops. The rest are navigation ("BACK", "NEXT"):
// shorter words, and the user already knows what they do, so 14 is survivable.
// `icon` is the WT_ICON_* a pill prefixes to its label, or NULL. It has to be
// here rather than assumed away: the icon is part of the string the screen
// draws, so a table that measured the bare translation would be checking text
// no one ever sees, and would keep reporting a comfortable fit while the real
// label overflowed.
typedef struct {
    const char *surface;
    int key, w, h, primary, key_action;
    const char *icon;
} pill_t;
static const pill_t PILLS[] = {
    { "sign/hold",        STR_S_HOLD_TO_SIGN, 272, 66, 1, 1 },
    { "sign/ack",         STR_C_I_UNDERSTAND, 252, 66, 1, 1 },
    { "sign/details",     STR_S_DETAILS,      170, 66, 0, 0 },
    { "sign/back",        STR_C_BACK,         140, 66, 0, 0 },
    { "wallet/pair",      STR_I_PAIR_T,       340, 66, 0, 1, WT_ICON_KEY },
    // The two ways a transaction gets in. Never measured before, and now they
    // carry icons, so they are worth a row each: SCAN QR is the primary action
    // of the whole signing flow.
    { "sign/scanqr",      STR_S_SCAN_QR,      340, 52, 1, 1, WT_ICON_QR },
    { "sign/fromsd",      STR_S_FROM_SD,      340, 52, 1, 1, WT_ICON_SD },
    { "proof/open",       STR_W_PROOF_BTN,       220, 52, 0, 1 },
    { "proof/capture",    STR_W_PROOF_SHOT,      300, 52, 1, 1 },
    { "proof/words",      STR_W_PROOF_WORDS_BTN, 300, 52, 1, 1 },
    { "storage/flash",    STR_W_KEEP_BTN,      252, 52, 0, 1 },
    { "storage/sd",       STR_W_SD_BTN,        252, 52, 0, 1 },
    { "storage/amnesic",  STR_W_AMNESIC_BTN,   252, 52, 0, 1 },
    { "storage/main",     STR_G_STORAGE_SEC,   340, 72 - 35, 0, 1 },
    { "storage/hold-move",STR_G_STORAGE_HOLD_MOVE,330, 66, 0, 1 },
    { "storage/hold-amn", STR_G_STORAGE_HOLD_AMNESIC,330,66,0,1 },
    { "set/words",        STR_I_WORDS_BTN,    340, 52, 0, 1 },
    { "set/wipe",         STR_G_WIPE,         340, 52, 0, 1 },
    { "recv/verify",      STR_R_VERIFY,       222, 52, 0, 1 },
    // wallet_sign.c coord_step(): a 580px label at a FIXED font23 with
    // LONG_CLIP. There is no font fallback here, so an over-long translation
    // is silently cut off mid-word rather than shrinking. Registered as
    // 580+28 so the reported budget is the real 580.
    { "sign/flow1",       STR_S_FLOW_1,       672, 44, 0, 0 },
    { "sign/flow2",       STR_S_FLOW_2,       672, 44, 0, 0 },
    { "sign/flow3",       STR_S_FLOW_3,       672, 44, 0, 0 },
    { "recv/sp",          STR_S_SP_BADGE,     220, 52, 0, 0 },
    { "recv/sp-show-full",STR_R_SP_SHOW_FULL,  280, 52, 0, 0 },
    { "recv/sp-show-short",STR_R_SP_SHOW_SHORT,280, 52, 0, 0 },
    { "wallet/sp-hold",   STR_R_SP_SHOW,       330, 66, 0, 1 },
    // Settings ADDRESS TYPE: one pill carrying the type NAME over the example
    // address. The example is a readable 23 now, so it reserves 35px of the
    // 72px pill and the name is fitted against what is left -- the same 29px
    // the name had when the pill was 60 tall with a 14px example under it.
    { "set/tyname-native",STR_S_TY_NATIVE,    340, 72 - 35, 0, 0 },
    { "set/tyname-nested",STR_S_TY_NESTED,    340, 72 - 35, 0, 0 },
    { "set/tyname-legacy",STR_S_TY_LEGACY,    340, 72 - 35, 0, 0 },
    // 340, not the 190 this row carried for a while: that width was written
    // when SCAN KEY was a small button on the pairing screen, and it stayed
    // behind when the export was promoted to its own 340px pill on the wallet
    // page. The table was quietly measuring a button that no longer existed.
    { "pair/scankey",     STR_R_SP_SCAN_BTN,  340, 72 - 35, 0, 0, WT_ICON_SECRET },
    { "pair/desktop",     STR_I_DESKTOP,      175, 60 - 22, 0, 0 },
    { "pair/mobile",      STR_I_MOBILE,       175, 60 - 22, 0, 0 },
    { "common/back",      STR_C_BACK,         140, 44, 0, 0 },
    { "common/done",      STR_C_DONE,         140, 52, 0, 0 },
    { "common/ok",        STR_C_OK,           200, 52, 0, 0 },
    { "common/cancel",    STR_C_CANCEL,       165, 52, 0, 0 },
    { "setup/full-verify",STR_L_VERIFY_FULL_BACKUP, 300, 66, 0, 1 },
    { "setup/understand", STR_C_I_UNDERSTAND, 320, 66, 0, 1 },
    // Naming the weak-passphrase outcome IS the safety of that card: a user who
    // cannot read this button has no idea which of the two presses keeps the
    // short passphrase. key_action, same tier as the one that spends.
    { "login/use-anyway", STR_L_USE_ANYWAY,   314, 56, 1, 1 },
    // The dice verdict row: all three are key_action, because a holder who
    // cannot read them cannot tell which press keeps the flagged rolls.
    { "setup/dice-more",  STR_W_DICE_MORE,    216, 66, 0, 1 },
    // cards mode: the two primary pills that advance the flow. TYPE MY WORDS
    // also serves the backup-check intro at the same 300 width.
    { "setup/cards-type", STR_W_TYPE_MY_WORDS, 300, 66, 1, 1 },
    { "setup/cksum-go",   STR_W_CKSUM_GO,      300, 66, 1, 1 },
    { "setup/dice-over",  STR_W_START_OVER,   216, 66, 0, 1 },
    { "setup/dice-use",   STR_L_USE_ANYWAY,   216, 66, 1, 1 },
    { "setup/dice-undo",  STR_W_DICE_UNDO,    200, 52, 0, 0 },
};
#define NPILL ((int)(sizeof PILLS / sizeof PILLS[0]))

// sign/why is built at runtime from up to three reasons plus the footer; the
// worst case (all three flagged) is what has to fit.
static void compose_why(char *out, size_t cap)
{
    snprintf(out, cap, "%s\n%s\n%s\n\n%s",
             tr(STR_S_WHY_HIGHFEE), tr(STR_S_WHY_DUSTIN),
             tr(STR_S_WHY_TINYCH), tr(STR_S_WHY_FOOT));
}

int main(int argc, char **argv)
{
    lv_init();
    static uint8_t buf[800 * 40 * 2];
    lv_display_t *d = lv_display_create(800, 480);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);

    int total_small = 0, key_small = 0, nfail = 0, en_small = 0;
    int row_cut = 0, row_known = 0;
#define MAXFAIL 64
    static char fails[MAXFAIL][96];
    for (int l = 0; l < I18N_LANG_N; l++) {
        const i18n_lang_t *li = i18n_lang_info(l);
        if (argc > 1) {                       // only the locales asked for
            int want = 0;
            for (int a = 1; a < argc; a++)
                if (strcmp(argv[a], li->code) == 0) want = 1;
            if (!want) continue;
        }
        i18n_set_lang(l);
        const int is_en = strncmp(li->code, "en", 2) == 0;

        int small = 0;
        char lines[NSLOT][160];
        for (int i = 0; i < NSLOT; i++) {
            char composed[1024];
            const char *txt;
            if (SLOTS[i].key < 0) { compose_why(composed, sizeof composed); txt = composed; }
            else                  { txt = tr(SLOTS[i].key); }

            const lv_font_t *f = wt_body_font(txt, SLOTS[i].w, SLOTS[i].h);
            // Only font14 counts as a failure now. 23 is a real reading size,
            // and the notes wedged between controls can never reach 28.
            int rung = f == wt_font28() ? 28 : f == wt_font23() ? 23 : 14;
            int bad  = rung == 14 && !SLOTS[i].may_be_small;
            if (bad) small++;
            // how far the copy overflows at 23 is what a translator must delete
            lv_point_t sz;
            lv_text_get_size(&sz, txt, wt_font23(), 0, 0, SLOTS[i].w, LV_TEXT_FLAG_NONE);
            int over = sz.y - SLOTS[i].h;
            // English is the reference layout: every box on this device was
            // measured against it, so a 14 here is not a translation that ran
            // long, it is a box that was built too small. That is a bug in the
            // layout and CI stops for it. Other locales stay advisory until
            // English is signed off.
            if (bad && is_en) {
                snprintf(fails[nfail < MAXFAIL ? nfail : MAXFAIL - 1],
                         sizeof fails[0], "slot %s (%dpx of %dpx at 23)",
                         SLOTS[i].surface, (int)sz.y, SLOTS[i].h);
                if (nfail < MAXFAIL) nfail++;
                en_small++;
            }
            snprintf(lines[i], sizeof lines[i], "  %-18s font%-2d  %3dpx / %3dpx%s",
                     SLOTS[i].surface, rung, (int)sz.y, SLOTS[i].h,
                     bad && over > 0 ? "  cut " : "");
            if (bad && over > 0) {
                char n[24];
                snprintf(n, sizeof n, "%d row(s) @23", (over + 28) / 29);
                strncat(lines[i], n, sizeof lines[i] - strlen(lines[i]) - 1);
            }
        }
        int pbad = 0;
        char plines[NPILL][160];
        char rlines[NROW][160];
        for (int i = 0; i < NPILL; i++) {
            const char *txt = tr(PILLS[i].key);
            char ibuf[WT_ICON_TEXT_MAX];
            if (PILLS[i].icon) {          // measure the string the screen draws
                wt_icon_text(ibuf, sizeof ibuf, PILLS[i].icon, txt);
                txt = ibuf;
            }
            wt_pill_fit_t fit = wt_pill_fit(txt, PILLS[i].w, PILLS[i].h,
                                            PILLS[i].primary);
            const lv_font_t *f = fit.font;
            bool wrap = fit.wrap;
            int rung = f == wt_font28() ? 28 : f == wt_font23() ? 23 : 14;
            lv_point_t sz;
            lv_text_get_size(&sz, txt, wt_font23(), 1, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            if (rung == 14) {
                pbad++;
                if (PILLS[i].key_action) {
                    key_small++;
                    snprintf(fails[nfail < MAXFAIL ? nfail : MAXFAIL - 1],
                             sizeof fails[0], "%s  %s (%dpx of %dpx at 23)",
                             li->code, PILLS[i].surface, (int)sz.x,
                             PILLS[i].w - 28);
                    if (nfail < MAXFAIL) nfail++;
                }
            }
            snprintf(plines[i], sizeof plines[i],
                     "  pill %-15s font%-2d%s  %3dpx / %3dpx%s", PILLS[i].surface,
                     rung, wrap ? " wrap" : "     ", (int)sz.x, PILLS[i].w - 28,
                     rung == 14 ? (PILLS[i].key_action ? "  FAIL" : "  widen") : "");
        }
        // Row labels: fixed font23, one line, ellipsis on overflow. Measured
        // unwrapped so the answer is the width the words actually need.
        int rbad = 0;
        for (int i = 0; i < NROW; i++) {
            const char *txt = tr(ROWS[i].key);
            int budget = row_label_budget(&ROWS[i]);
            lv_point_t sz;
            lv_text_get_size(&sz, txt, wt_font23(), 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            rlines[i][0] = '\0';
            if ((int)sz.x > budget) {
                rbad++;
                bool known = row_backlogged(li->code, ROWS[i].surface);
                if (known) row_known++;
                else       row_cut++;
                snprintf(rlines[i], sizeof rlines[i],
                         "  row  %-15s %3dpx / %3dpx  %s  \"%s\"",
                         ROWS[i].surface, (int)sz.x, budget,
                         known ? "ellipsis (backlog)" : "ELLIPSIS", txt);
            }
        }

        printf("%-6s %-22s %2d/%d at font14, %d/%d pills, %d/%d rows\n",
               li->code, li->native, small, NSLOT, pbad, NPILL, rbad, NROW);
        for (int i = 0; i < NROW; i++)
            if (rlines[i][0]) puts(rlines[i]);
        for (int i = 0; i < NSLOT; i++)
            if (strstr(lines[i], "cut ")) puts(lines[i]);
        for (int i = 0; i < NPILL; i++)
            if (strstr(plines[i], "widen") || strstr(plines[i], "FAIL"))
                puts(plines[i]);
        total_small += small + pbad;
    }
    printf("\ntotal at font14: %d\n", total_small);
    printf("row labels ellipsised: %d backlogged, %d new\n", row_known, row_cut);

    // Every icon a pill draws must exist, at every size, with real ink in it.
    //
    // This check used to justify itself by saying a missing codepoint spins
    // LVGL's renderer. It does not. CONFIG_LV_USE_FONT_PLACEHOLDER is y in
    // sdkconfig and LV_USE_FONT_PLACEHOLDER is 1 in sim/lv_conf.h, and
    // lv_font_get_glyph_dsc walks the fallback chain and then returns cleanly
    // with resolved_font NULL, format A1 and a box half the line height wide.
    // Nothing hangs; you get an empty rectangle.
    //
    // The check is worth just as much for the true reason. An empty rectangle
    // where a key or a lock should be is a defect on a device that has no way
    // to report it, and the icons here sit on the buttons that pair a
    // coordinator and export a scan key. So ask directly rather than
    // trusting that gen_fonts.sh and WT_ICON_* were edited on the same day --
    // a run that survived is not evidence, it only means the walk never
    // rendered the missing one.
    static const struct { const char *name; const char *utf8; } ICONS[] = {
        { "WT_ICON_QR",     WT_ICON_QR     },
        { "WT_ICON_KEY",    WT_ICON_KEY    },
        { "WT_ICON_SECRET", WT_ICON_SECRET },
        { "WT_ICON_SD",     WT_ICON_SD     },
        { "WT_ICON_LOCK",   WT_ICON_LOCK   },
        { "WT_ICON_REPLACE", WT_ICON_REPLACE },
    };
    const struct { const char *name; const lv_font_t *f; } FACES[] = {
        { "font14", wt_font14() }, { "font23", wt_font23() },
        { "font28", wt_font28() }, { "font34", wt_font34() },
    };
    int icon_bad = 0;
    for (size_t i = 0; i < sizeof ICONS / sizeof *ICONS; i++) {
        const unsigned char *u = (const unsigned char *)ICONS[i].utf8;
        uint32_t cp = ((u[0] & 0x0Fu) << 12) |     // every WT_ICON_* is 3-byte
                      ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
        for (size_t j = 0; j < sizeof FACES / sizeof *FACES; j++) {
            lv_font_glyph_dsc_t g;
            bool ok = lv_font_get_glyph_dsc(FACES[j].f, &g, cp, 0);
            if (!ok || g.box_w == 0 || g.adv_w == 0) {
                printf("FAIL: %s (U+%04X) %s in %s\n", ICONS[i].name,
                       (unsigned)cp, ok ? "is blank" : "is MISSING",
                       FACES[j].name);
                icon_bad++;
            }
        }
    }
    if (icon_bad) {
        puts("\nAdd the codepoint to SYMS in tools/fonts/gen_fonts.sh and\n"
             "re-run it. A label may never reference a glyph the font lacks:\n"
             "LVGL draws an empty placeholder box, on a screen nobody can\n"
             "file a bug from.");
        return 1;
    }
    printf("pill icons: %d present and inked at 14/23/28/34\n",
           (int)(sizeof ICONS / sizeof *ICONS));

    // Two-line pills: the second line is a fixed size and never wraps, so
    // pill_sub_line() drops it to 14 when the asked-for size will not fit.
    // That is the right thing to do on the device and the wrong thing to find
    // out there, so name the locales it happens in. Same measurement
    // pill_sub_line makes, against the same box.
    static const struct { const char *surface; int key, w, row_h; } SUBS[] = {
        // wallet_info.c: the badge under SCAN KEY, the only thing on that
        // screen naming which kind of address the key belongs to.
        { "wallet/sp-badge", STR_S_SP_BADGE, 340, 35 },
    };
    for (size_t i = 0; i < sizeof SUBS / sizeof *SUBS; i++) {
        int at14 = 0;
        char who[256] = "";
        for (int l = 0; l < I18N_LANG_N; l++) {
            i18n_set_lang(l);
            lv_point_t sz;
            lv_text_get_size(&sz, tr(SUBS[i].key), wt_font23(), 0, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x > SUBS[i].w - 28 || sz.y > SUBS[i].row_h) {
                at14++;
                strncat(who, i18n_lang_info(l)->code,
                        sizeof who - strlen(who) - 2);
                strncat(who, " ", sizeof who - strlen(who) - 1);
            }
        }
        printf("pill sub %-16s %s\n", SUBS[i].surface,
               at14 ? who : "23 in every locale");
    }

    if (key_small || en_small) {
        printf("\nFAIL: %d key-action button(s) and %d English slot(s) "
               "fell to font14:\n", key_small, en_small);
        for (int i = 0; i < nfail; i++)
            printf("  %s\n", fails[i]);
        puts("\nA button that spends, erases or verifies must not be the\n"
             "smallest type on its screen. Fix by shortening that locale's\n"
             "label, widening the pill, or making it tall enough to wrap.\n"
             "\nAn ENGLISH slot at font14 is a layout bug, not a long\n"
             "translation: the box was measured against this very text. Give\n"
             "it the height 23 needs, or mark it may_be_small WITH a comment\n"
             "saying which neighbour stops it from growing.");
        return 1;
    }

    // Every locale, not just English. This number was printed and never acted
    // on for as long as it was nonzero, which is the wrong way round: a figure
    // nobody can fail is a figure that only ever grows. It is 0 across all 21
    // locales now, so the sweep that got it there is worth keeping, and the
    // cheapest way to keep it is to refuse to build with it above zero.
    //
    // Deliberately counts what the English gate above does not: a slot that is
    // only too small in Polish, and a pill that drops to 14 without being a
    // key action. Neither is a translation problem. Both mean a box measured
    // against English that the device will render in something else.
    if (row_cut) {
        printf("\nFAIL: %d row label(s) ellipsise and are not in ROW_BACKLOG.\n",
               row_cut);
        puts("A wt_row label is drawn at a FIXED font23 on one line with\n"
             "LV_LABEL_LONG_DOT. It does not shrink and it does not wrap, it\n"
             "just loses its last words, and no other gate can see that.\n"
             "Shorten that locale's label, or shorten the VALUE it sits\n"
             "against -- the value takes its width off the label's budget.");
        return 1;
    }

    if (total_small) {
        printf("\nFAIL: %d slot(s) fall to font14 in at least one locale.\n",
               total_small);
        puts("The lines above marked \"cut\" or \"widen\" name them. Give the\n"
             "box the height or width 23 needs, shorten the string, or mark\n"
             "the slot may_be_small WITH a comment saying which neighbour\n"
             "stops it growing. font14 is for metadata: units, tags, raw\n"
             "values. Never for something the holder has to read and act on.");
        return 1;
    }
    return 0;
}
