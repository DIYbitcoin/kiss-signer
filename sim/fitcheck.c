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
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "i18n.h"
#include "kiss_theme.h"

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
    // kiss_setup.c:279 — amber line under the word grid
    { "setup/paper-only", STR_I_WORDS_S,    700,  40 },
    // kiss_info.c — "?" cards (155 with a diagram, 225 without; the
    // scan-key warning below pairs a short body with three visual facts).
    // The PAIRING one is gone with its card: that page's [ ? 2 ] opens
    // DESCRIPTOR and FINGERPRINT as term rows now, and a definition row is
    // measured by the TERM gate rather than by a slot here.
    { "wallet/?fp",       STR_I_H_FP_B,     720, 155 },
    { "wallet/?type",     STR_I_H_TYPE_B,   720, 225 },
    // kiss_info.c:263,326 — full-screen warnings
    { "wallet/sp-warn",   STR_R_SP_WARN_B,  700, 145 },
    { "wallet/sp-find",   STR_R_SP_FACT_FIND,     482, 29 },
    { "wallet/sp-spend",  STR_R_SP_FACT_NO_SPEND, 482, 29 },
    { "wallet/sp-forever",STR_R_SP_FACT_FOREVER,  482, 29 },
    // kiss_recv.c: the fitted full-width kind line on a verified silent-payment
    // address. This replaced the stale two-line SCAN KEY sub-line lane below.
    { "recv/sp-badge",    STR_S_SP_BADGE,          700, 29 },
    // kiss_sign.c:355,394,966
    { "sign/why",         -1,               720, 300 },   // composed below
    { "sign/?address",    STR_S_ADDR_HELP_B, 720, 230 },
    { "sign/?coins",      STR_S_COINS_HELP_B, 720, 230 },
    // kiss_ui.c:469,769 — login warning + passphrase intro
    // The warn bodies render as ruled blocks split on their blank lines, so a
    // whole-key slot can only bound the single-claim key at the wide block's
    // width; the sliced no-passphrase pair is covered by the walk's three
    // rendered states instead.
    { "login/warn",       STR_L_WARN_B,     690, 160 },
    // The why-block PAIRS that used to be measured here are gone: the
    // passphrase intro, the backup check, both blind draw screens, the dice
    // verdict and the checksum page all say their two claims as fact ROWS
    // now. A row's caption and value are pinned to one line each and
    // ellipsise rather than shrink, so the check that sees them is CUT in the
    // walk (wt_sub_measure, measured as the label is built), not a body
    // budget here. Slots for them would measure a box no screen draws.
    // kiss_setup.c — wizard explainers
    { "setup/checksum",   STR_W_CHECK_B,    704, 256 },
    { "setup/verify-ok",  STR_W_VOK_B,      704, 190 },
    { "setup/verify-bad", STR_W_VBAD_B,     704, 190 },
    // The setup wizard's three storage cards. Settings used to share this
    // geometry and no longer does: its own chooser states each mode on a row
    // sub-line instead, measured further down as set/store-*.
    // The SD note used to carry a line about the mode being unavailable in
    // normal builds. It is not: kiss_seed_sd_supported() returns true on
    // every build, and the unreachable disabled path was deleted, so the note
    // is now ordinary copy with nothing special about it.
    { "storage/flash",    STR_W_KEEP_NOTE,        420, 87 },
    { "storage/flash-enc",STR_W_FLASH_ENC_NOTE,   420, 87 },
    { "storage/sd",       STR_W_SD_NOTE,          420, 87 },
    { "storage/amnesic",  STR_W_AMNESIC_NOTE,     420, 87 },
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
    // kiss_settings.c: the two wipe overlays
    { "wipe/confirm",     STR_G_WIPEC_B,    704, 190 },
    { "wipe/erased",      STR_G_ERASED_B,   704, 160 },
    { "wipe/not-erased",  STR_G_NOERASE_B,  704, 160 },
    // amnesic mode: seed-QR import + passphrase-from-QR
    { "setup/qr-bad",     STR_W_QRBAD_B,    704, 240 },
    // kiss_sign.c: the screens BEFORE and AFTER the detail page. The detail
    // page was swept first and these were missed, so the refusal to sign, the
    // two SD prompts and every instruction on the signed-QR page were still at
    // font14 -- on the flow that moves money.
    // The SCAN QR tab's one instruction, centered under the airgap diagram.
    // may_be_small: it renders at a FIXED chrome23, never on the ladder this
    // slot models -- a long translation ellipsises rather than shrinking,
    // and the sweep is where that gets caught and cut.
    { "sign/point-cam",   STR_S_POINT_CAM,      704,  29, 1 },
    { "sign/read-fail",   STR_S_READ_FAIL,      704, 232 },
    { "sign/rm-confirm",  STR_S_RM_C_B,         704, 100 },
    { "sign/not-psbt",    STR_S_NOT_PSBT,       704, 232 },
    { "sign/scan-bad",    STR_S_SCAN_NOT_PSBT,  704, 232 },
    { "sign/insert-card", STR_S_INSERT_CARD,    704, 116 },
    { "sign/sparrow-save",STR_S_SPARROW_SAVE,   704, 116 },
    // kiss_sign.c sd_open — the one hint line at y=98, drawn at font14 by
    // design (may_be_small), one line wide as the whole content lane. The %d
    // pair expands to at most 2 digits each, no wider than the specifiers.
    { "sign/files-more",  STR_S_FILES_MORE_FMT, 704, 29, 1 },
    { "sign/qr-loop",     STR_S_QR_LOOP,        322,  29 },
    // kiss_sign.c glossary_cb() -- SIMPLE EXPLAINERS, eight definitions in a
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
    // kiss_settings.c — the network row's sub-line on the SIGNER tab. It is a
    // ROW SUB now rather than a note floating in a gap between controls: one
    // line, font14 by the kit's own rule for row sub-lines, in the lane the
    // value chip leaves it (268..538 at the chip's 190px minimum).
    // may_be_small, because font14 here is the row idiom and not a budget
    // thrown away in code.
    { "set/net-main",     STR_G_MAINNET_NOTE, 270, 19, 1 },
    { "set/net-test",     STR_G_TESTNET_NOTE, 270, 19, 1 },
    // The same sub-lines, on the same 270px lane, for every row that states
    // what its value MEANS. They appear twice each: under the label on the
    // settings row, and again on the storage chooser, where the lane is wider
    // (no value chip, only a chevron) -- so 270 is the binding box of the two.
    //
    // These went unmeasured for a commit, which is how the storage list
    // shipped reading "SD C...": the dropdown it lived in was 280px wide and
    // "on the card you carry" took the lane, leaving the NAME to ellipsise.
    // Nothing on that path had a box in this table.
    { "set/store-flash",  STR_I_STORE_FLASH_SUB,     270, 19, 1 },
    { "set/store-fl-enc", STR_I_STORE_FLASH_ENC_SUB, 270, 19, 1 },
    { "set/store-sd",     STR_I_STORE_SD_SUB,        270, 19, 1 },
    { "set/store-amn",    STR_I_STORE_AMN_SUB,       270, 19, 1 },
    // Persist, whose sub follows the STATE: what is kept, or that nothing is.
    { "set/hist-on",      STR_I_HIST_SHORT,          270, 19, 1 },
    { "set/hist-off",     STR_I_POP_NOTHING,         270, 19, 1 },
    // The NO UNDO tab is a CLAIM PAIR now, not one paragraph: two heads and
    // two bodies in two 340px columns, measured by wt_body_font2_head. Its old
    // single-paragraph key went with the shape, along with the caption beside
    // the button that explained the page's other four tabs from inside this
    // one. The pair's own halves are measured two entries below.
    // set/separate and sub/addr-type used to sit here, both measuring
    // STR_G_SEPARATE, "each network + type is its own separate wallet". The
    // string is gone: it was the ADDRESS TYPE subtitle and, doing second duty,
    // the filler in a decoy session's duress slot. It told nobody anything the
    // three rows above already say. Neither box has a string to measure now.
    { "set/create-note",  STR_G_CREATE_NOTE,  340, 34, 1 },
    { "set/words-note",   STR_I_WORDS_BTN_NOTE,340,34, 1 },
    // kiss_duress_ui.c ST_DONE: wt_why_body at y=250 under the two-ways
    // diagram, so the body has WT_CONTENT_BOTTOM - 250 = 148 to live in.
    { "duress/done",      STR_GD_DONE_B,      700, 148, 0 },
    // kiss_recv.c / kiss_info.c — instructions the user has to act on
    // wt_screen() subtitles: one line, 704px wide, between title and content.
    { "sub/receive",      STR_R_S,            704, 30, 0 },
    { "sub/wallet",       STR_I_S,            340, 58, 0 },
    { "sub/verify",       STR_R_VS,           704, 30, 0 },
    { "sub/sp-warn",      STR_R_SP_WARN_S,    704, 30, 0 },
    // Every remaining wt_screen subtitle. One line at 23 or it drops to 14 --
    // the header geometry in wt_screen() is fixed, so the only lever here is
    // the length of the sentence.
    { "sub/words",        STR_I_WORDS_S,      704, 30, 0 },
    { "sub/setup",        STR_W_SETUP_S,      704, 30, 0 },
    { "sub/write",        STR_W_WRITE_S,      704, 30, 0 },
    // These two draw their own subtitle (mk_screen2 in kiss_setup.c) because
    // their first content sits well below the y=96 line, so they get the two
    // lines their copy was written for.
    { "sub/rand",         STR_W_RAND_S,       704, 58, 0 },
    { "sub/prove",        STR_W_PROVE_S,      704, 58, 0 },
    // W_RNG_S is no longer a subtitle; it survives as the audit chooser row's
    // sub, where the CUT sink measures it as the label is built.
    { "rng/src-sub",      STR_W_RNG_SRC_SUB,     480,  46, 1 },
    // 330, not 344: the block's rule bar eats 14px of body width. Height is
    // the pair budget (194) minus a measured one-line heading (35).
    { "rng/retry",        STR_W_RNG_RETRY,       704,  34, 0 },
    { "rng/pass-note",    STR_W_RNG_PASS_NOTE,   704,  34, 0 },
    // dice screens: never registered before the quality check landed, which is
    // how the samey nudge shipped unmeasured. The verdict subtitles are one
    // line on wt_screen and the verify note gets two card lines; the two
    // claims are fact rows and belong to CUT.
    { "sub/dice",         STR_W_DICE_S,           704, 30, 0 },
    { "sub/dice-uneven",  STR_W_DICE_UNEVEN_S,    704, 30, 0 },
    { "sub/dice-pattern", STR_W_DICE_PATTERN_S,   704, 30, 0 },
    { "setup/dice-verify",STR_W_DICE_VERIFY_NOTE, 564, 36, 1 },
    { "sub/restore",      STR_W_RESTORE_S,    704, 30, 0 },
    // cards mode (BLIND DRAW): subtitles, the method-row note and the
    // checksum page's one number line. The candidate word actions are dynamic
    // English BIP39 words and are deliberately not rows here.
    { "sub/cards",        STR_W_CARDS_S,      704, 30, 0 },
    { "sub/cksum",        STR_W_CKSUM_S,      704, 30, 0 },
    { "sub/cards-pick",   STR_W_CARDS_PICK_S, 704, 30, 0 },
    { "setup/cards-note", STR_W_CARDS_NOTE,   420, 87 },
    { "setup/cksum-fit",  STR_W_CKSUM_FIT_FMT, 704, 20, 1 },
    // cards_help_cb() -- THE 2048 WORD LIST, three definitions in an icon grid.
    // Measured at the REAL cell lane, not the page: explain_grid deals two
    // columns, so each definition wraps inside 300px and gets pitch minus its
    // heading, about 133px. 3 x 133 is that allowance for all three cells.
    //
    // Deliberately NOT sign/glossary's 704 no-wrap box. That one exists because
    // a glossary definition must never break mid clause and eight rows leave no
    // slack; three rows here have room to wrap and wrapping is what the grid
    // does. Copying the stricter proxy would have bought terser copy for no
    // reason. This is still the first setup wizard explainer with ANY fit
    // coverage -- the dice and entropy ones have none.
    { "setup/cards-help", STR_W_CARDS_HELP_B,  300, 399 },
    // the two verdict screens: five subtitles, one per rule. The five bodies
    // they used to pair with are single line fact values now.
    { "sub/cards-same",   STR_W_CARDS_SAME_S,   704, 30, 0 },
    { "sub/cards-period", STR_W_CARDS_PERIOD_S, 704, 30, 0 },
    { "sub/cards-clust",  STR_W_CARDS_CLUST_S,  704, 30, 0 },
    { "sub/cards-sorted", STR_W_CARDS_SORTED_S, 704, 30, 0 },
    { "sub/cards-dup",    STR_W_CARDS_DUP_S,    704, 30, 0 },
    { "sub/vfy-backup",   STR_W_VERIFY_S,     704, 30, 0 },
    { "sub/qr-warn",      STR_L_SCAN_WARN_S,  704, 30, 0 },
    // and the signing flow's own subtitles. The chooser, the file list and
    // the SD empty states swapped theirs for the trail, so only the signed
    // pages still carry one.
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
    // kiss_info.c — the note under each action
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
    // The scan page's right column: the safety sentence wraps freely in the
    // 356 lane above the band, so its budget is the room down to 398.
    // may_be_small for the same reason as sign/point-cam: it renders at a
    // FIXED mono18, not on the ladder, and the sweep owns the long locales.
    { "scan/no-sign",     STR_N_NOTHING_SIGNED, 356, 166, 1 },
    // kiss_recv.c sp_help_cb(): the sp1-vs-bc1p explainer overlay. Measured
    // with the raw "%s" in place, which is ~2px narrower per prefix than the
    // 3 to 4 characters that get substituted, so this reads slightly optimistic.
    { "recv/sp-why",      STR_R_SP_WHY_B,       720, 238 },
    { "wallet/sp-note",   STR_R_SP_EXPORT_NOTE, 360, 140 },
    // Same string, second home: the note under SCAN KEY on the WALLET page.
    // That box is the tighter of the two, so measuring only the 360x140 one
    // let this render at 14 next to a PAIR COORDINATOR note at 23.
    { "wallet/sp-btn",    STR_R_SP_EXPORT_NOTE, 340,  90 },
    { "wallet/pair-note", STR_I_PAIR_BTN_NOTE,  340,  62 },
    // The two word-count notes sit in the 80px gaps of a three-action stack (12
    // WORDS at y=150, 24 WORDS at 230, SCAN LOCKED QR at 310) in a 340px column.
    // Both run to three or four lines at 23 -- 87px and 116px -- so neither can
    // reach it without restacking the page or cutting the copy. Same situation
    // as the ADDRESS TYPE notes above, and recorded for the same reason.
    { "setup/12-note",    STR_W_12_NOTE,        340,  76, 1 },
    { "setup/24-note",    STR_W_24_NOTE,        340,  76, 1 },
};
#define NSLOT ((int)(sizeof SLOTS / sizeof SLOTS[0]))

// wt_row LABELS, which nothing measured until a rename made one of them fail.
//
// These are not fitted bodies: wt_row_x draws the label at a FIXED
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
    const int *val_set;    // ... or a -1 terminated set, measured at its widest
} row_t;

// The ways in row does not show ONE value, it shows whichever of seven applies:
// NOT SET, or the name of the stroke that reaches the real wallet. Measuring it
// against NOT SET alone was the reason a row reading "Duress w... LINE THROUGH"
// on a real device came back clean here. Which of the seven is longest changes
// with the language, so the budget takes the widest in the ACTIVE locale.
static const int DURESS_VALS[] = {
    STR_GD_OFF,   STR_GD_UNDERLINE, STR_GD_OVERLINE, STR_GD_STRIKE,
    STR_GD_SLASH, STR_GD_CIRCLE,    STR_GD_CHECK,    -1,
};

static const row_t ROWS[] = {
    // kiss_settings.c, left column at SG_L_W = 365
    { "set/network",  STR_I_ROW_NETWORK, -1, NULL,    365 - 176 },  // segmented track
    { "set/type",     STR_I_ROW_TYPE,    -1, "m/n...", 365 },
    { "set/storage",  STR_I_ROW_STORAGE, STR_W_AMNESIC_BTN, NULL, 365 },
    // Full width under both columns now: SG_FULL_W, not SG_L_W.
    { "set/duress",   STR_I_ROW_WAYSIN,  -1, NULL, 752, DURESS_VALS },
    // right column, SG_R_W = 365. None of these carry a value.
    { "set/words",    STR_I_ROW_WORDS,   -1, NULL, 365 },
    { "set/endwords", STR_I_ROW_ENDWORDS, -1, NULL, 365 },
    // The randomness audit's provenance row: label against its wider value.
    { "rng/src",      STR_W_RNG_SRC,     STR_W_RNG_OFF, NULL, 716 },
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
// "Words live in" is what is left. It is short in English, becomes a clause in
// most other languages, and sits against a translated value ("AMNESIC") that
// eats the same row.
//
// "Duress wallet" used to be the other half of this list, in thirteen locales.
// The note here said fixing it meant shorter labels in ten locales OR a wider
// column; the row went full width under both columns instead, so all thirteen
// came off. That is the shrink this backlog exists to record.
//
// "it / set/erase" came off the same way: it was Italian's "Cancella questo
// portafoglio" overshooting by seven pixels, and the two NO UNDO rows have
// since merged into one, so the label it named is gone.
// EMPTY, and collected rather than left standing. Every entry here named a
// locale whose row label overshot its lane by a few pixels, and every one of
// them said the fix was the same thing: shorter copy, from a translation
// sweep that had not happened yet. It has now. All ten "set/storage" entries
// and all nine that arrived with the mono faces were de, es, fr, it, nl, pt,
// ru and tr, and the sweep cut each of those labels to fit -- ALMACENAJE for
// ALMACENAMIENTO, TIPO INDIRIZZO for TIPO DI INDIRIZZO, ПАМЯТЬ for ХРАНЕНИЕ.
//
// Measured before deleting, not assumed: the full 21 locale run reports
// "row labels ellipsised: 0 backlogged, 0 new". A backlog nothing fires on is
// a list of excuses for defects that no longer exist, and the next person to
// read it would take it for work outstanding.
static const struct { const char *lang, *surface; } ROW_BACKLOG[] = {
    { NULL, NULL },   // keep the array non-empty; row_backlogged skips NULL
};
#define NROW_BACKLOG ((int)(sizeof ROW_BACKLOG / sizeof ROW_BACKLOG[0]))

static bool row_backlogged(const char *lang, const char *surface)
{
    for (int i = 0; i < NROW_BACKLOG; i++)
        if (ROW_BACKLOG[i].lang &&
            strcmp(ROW_BACKLOG[i].lang, lang) == 0 &&
            strcmp(ROW_BACKLOG[i].surface, surface) == 0)
            return true;
    return false;
}

// The SUB-LINE under a row label, which had the same blind spot the labels had
// and kept it one release longer.
//
// A standard row's sub is drawn at font14, one line, LV_LABEL_LONG_DOT
// (kiss_theme.c, the `else` branch of wt_row_x's sub block). Same failure as
// the label: it does not shrink, it does not wrap, it loses its last words and
// looks deliberate. The check above measured the label and stopped there, so
// "die jetzigen Wörter gelten dann nicht mehr" under START A NEW WALLET was
// free to ellipsise in German and nothing said so.
//
// The budget is the same number the label gets -- wt_row_x computes
// `lw = right - vw - (vw ? 12 : 0) - lx` for the label and `sw = right - lx`
// AFTER `right -= vw + 12`, which is the same arithmetic twice -- so this
// reuses row_label_budget() and changes only the face it measures with.
//
// A flat table rather than a field on row_t: a row shows a DIFFERENT sub
// depending on state, and each variant has to be measured on its own. Taking
// the longest translation of one representative variant would be a guess, and
// the WORDS row is the case that proves it -- its two subs carry different
// marks and only one takes a fingerprint.
typedef struct {
    const char *surface;   // the ROWS[] entry whose budget this shares
    int key;               // STR_* of the sub
    const char *pfx;       // the mark and gap the screen prepends, or NULL
    const char *arg;       // the %s a _FMT sub is given, or NULL
} sub_t;
static const sub_t SUBROWS[] = {
    // The duress row's OWN sub. This has now been wrong twice: it measured
    // STR_GD_SET_SUB (a caption from another screen), then the WAYS IN
    // subtitle after that string had moved on to being the duress SCREEN's
    // header. The row draws STR_I_WAYSIN_SHORT and this measures that.
    { "set/duress",  STR_I_WAYSIN_SHORT, NULL, NULL },
    // kiss_settings.c:1195-1207 -- both branches prefix a mark and two
    // spaces, and the checked one interpolates the 8 hex digits of a
    // fingerprint. Measured with a real one, because "%s" is two characters
    // wide and the thing the screen draws is eight.
    { "set/words",   STR_I_WORDS_VERIFIED_FMT, LV_SYMBOL_OK "  ", "A1B2C3D4" },
    { "set/words",   STR_I_WORDS_UNVERIFIED, LV_SYMBOL_WARNING "  ", NULL },
    // NO UNDO. One row now, and its sub-line is the whole reason there is only
    // one: whichever door you pick, the words go. That claim has to survive the
    // narrowest locale or the merge just hides the cost again.
};
#define NSUBROW ((int)(sizeof SUBROWS / sizeof SUBROWS[0]))

// Row SUB-LINES that ellipsise today, recorded the day the check was written.
// Shrink only, exactly like ROW_BACKLOG above: delete a line when the copy is
// fixed, never add one.
//
// Twenty eight entries, and not one of them is set/endwords -- NO UNDO was
// reworded in the same commit that added this check and fits in all 21
// locales, which is the whole reason the check could be turned on. It stayed
// clean through the merge that made those two rows one.
//
// The two rows here are the two rows ROW_BACKLOG already names, for the same
// reason: DURESS sits against a translated value ("NOT SET" becomes
// "ISKLJUČENO"), and the value takes its width off the sub as well as off the
// label, so Croatian is measuring a 204px sentence against 72px of row. WORDS
// has no value but its sub carries a mark, two spaces and eight hex digits of
// fingerprint before the sentence even starts. Both want shorter copy in a
// dozen locales, or a wider left column -- a pass of its own, not a rename.
// Parked for the translation sweep (decided 2026-08-18): every entry here is
// translation-bound and English fits everywhere, so the sweep is where these
// get their shorter copy and this list shrinks there, not before.
// EMPTY, collected the way ROW_BACKLOG was and for the same reason. Thirty
// two entries across seventeen locales, every one of them parked on the same
// sentence: "translation-bound, English fits everywhere, the sweep is where
// these get their shorter copy and this list shrinks there, not before."
// The sweep has now been through all seventeen.
//
// set/words came off in the last four the same way each time -- the sub is a
// mark, two spaces, eight hex digits of fingerprint and then a sentence, and
// I_WORDS_VERIFIED_FMT was a sentence where English is two words. It is
// "tjekket · %s", "sjekket · %s", "kollad · %s", "provjereno · %s" now, and
// each of those took its row from 342-366px in a 317px lane to inside it.
//
// The mono-face cluster came off too, which was the open question: the file
// said those six arrived because the fixed-pitch faces run ~8% wider on the
// same sub, a LANE problem rather than a copy-length one, and they might
// have survived shorter copy. They did not -- cutting the format string took
// more than the faces had added.
//
// Measured before deleting: the full 21 locale run reports "row subs
// ellipsised: 0 backlogged, 0 new".
static const struct { const char *lang, *surface; } ROWSUB_BACKLOG[] = {
    { NULL, NULL },   // keep the array non-empty; rowsub_backlogged skips NULL
};
#define NSUBROW_BACKLOG \
    ((int)(sizeof ROWSUB_BACKLOG / sizeof ROWSUB_BACKLOG[0]))

static bool rowsub_backlogged(const char *lang, const char *surface)
{
    for (int i = 0; i < NSUBROW_BACKLOG; i++)
        if (ROWSUB_BACKLOG[i].lang &&
            strcmp(ROWSUB_BACKLOG[i].lang, lang) == 0 &&
            strcmp(ROWSUB_BACKLOG[i].surface, surface) == 0)
            return true;
    return false;
}

static const row_t *row_by_surface(const char *surface)
{
    for (int i = 0; i < NROW; i++)
        if (strcmp(ROWS[i].surface, surface) == 0) return &ROWS[i];
    return NULL;
}

// The label box wt_row_x will give this row, in the ACTIVE locale.
static int row_label_budget(const row_t *r)
{
    lv_point_t sz;
    lv_text_get_size(&sz, LV_SYMBOL_RIGHT, wt_font23(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int right = r->w - 10 - (int)sz.x - 10;
    int vw = 0;
    if (r->val_set) {
        // widest of the set, in this locale
        for (int i = 0; r->val_set[i] >= 0; i++) {
            const char *v = tr(r->val_set[i]);
            if (!v || !*v) continue;
            lv_text_get_size(&sz, v, wt_font23(), 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            if ((int)sz.x > vw) vw = (int)sz.x;
        }
        if (vw) vw += 12;
    } else {
        const char *val = r->val_lit ? r->val_lit
                        : r->val_key >= 0 ? tr(r->val_key) : NULL;
        if (val && *val) {
            lv_text_get_size(&sz, val, wt_font23(), 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            vw = (int)sz.x + 12;
        }
    }
    return right - vw - 14;
}

// ---- the measurements, in one place so the selftest exercises the gate ----
//
// Every one of these was written inline in main. Pulling them out is not
// tidying: a selftest that measures with its own copy of the arithmetic
// proves the copy works, which is the failure this file already had once in
// a worse form -- it did not COMPILE for a stretch (43847834, a reference to
// a deleted key), and a gate that never runs looks exactly like a gate that
// passes. Now that ROW_BACKLOG is a bare sentinel, "0 backlogged, 0 new" is
// also what a check that stopped measuring would print.

// The rung wt_body_font picks for a slot: 28, 23 or 14.
static int slot_rung(const char *txt, int w, int h)
{
    const lv_font_t *f = wt_body_font(txt, w, h);
    return f == wt_font28() ? 28 : f == wt_font23() ? 23 : 14;
}

// A wt_row label is font23 on ONE line, a sub is font14 on one line, and both
// ellipsise past their budget -- so width on an unbounded lane is the whole
// question for each.
static int text_px(const char *txt, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)sz.x;
}

// Present in the face, and with real ink in it rather than a placeholder box.
static bool icon_inked(const lv_font_t *f, uint32_t cp)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(f, &g, cp, 0) && g.box_w != 0 && g.adv_w != 0;
}

// What wt_title_fit lands on for this title in this lane. Builds and deletes
// the real widgets, because the picker's answer depends on what else is in
// the header row.
static const lv_font_t *title_pick(const char *txt, int lane, bool chrome,
                                   int *letter_space)
{
    lv_obj_t *scr = wt_screen(lv_screen_active(), txt, NULL);
    if (chrome) wt_chrome_head(scr);
    wt_title_fit(scr, lane);
    lv_obj_t *cap = wt_screen_title(scr);
    const lv_font_t *picked = lv_obj_get_style_text_font(cap, 0);
    if (letter_space) *letter_space = lv_obj_get_style_text_letter_space(cap, 0);
    lv_obj_delete(scr);
    return picked;
}

static bool title_is_smallest(const lv_font_t *f)
{
    return f == wt_font_mono18() || f == wt_font23();
}

// The pill lane is gone with the pills. Every action is an arrow or word
// action now: a content-sized single line at chrome23 that never re-fonts,
// so there is no rung to fall off -- a long locale gets wider, and the
// overlap walk is the gate that sees a collision.

// sign/why is built at runtime from up to FIVE reasons, and the worst case is
// all five flagged. Three of them plus a footer was the old shape, and this
// stayed behind when 43847834 gave every line its own action and dropped
// S_WHY_FOOT: the key went, this reference did not, and sim/fitcheck.c has
// not COMPILED since -- so the whole type-size ratchet was absent, in the one
// state that looks nothing like a red gate. Mirror kiss_sign.c's builder, and
// keep mirroring it: a reason added there and not here is measured by nothing.
static void compose_why(char *out, size_t cap)
{
    char merge[256];
    snprintf(merge, sizeof merge, tr(STR_S_WHY_MERGE_FMT), 9u);
    snprintf(out, cap, "%s\n%s\n%s\n%s\n%s",
             tr(STR_S_WHY_HIGHFEE), tr(STR_S_WHY_DUSTIN), merge,
             tr(STR_S_WHY_GAPCH), tr(STR_S_WHY_TINYCH));
}

// wt_group4 blocks a string in fours for comparison against a coordinator, so
// a character it drops is a character the owner compares against nothing. It
// writes no ellipsis and leaves no gap: a truncated run just ends in a short
// group, which is what the last group of a real address often looks like
// anyway. Nothing on the glass can tell you it happened.
//
// The txid on DETAILS is the case that matters and the case that was broken:
// 64 hex characters block to 79, gt[80] holds exactly that plus the NUL, and
// the owner is invited to compare the result against their coordinator or copy
// it down to look the transaction up later.
//
// Checked here because fitcheck is the gate that links the theme, and this is
// the same question fitcheck exists to ask -- does the text survive the box --
// asked one layer below the renderer.
static int check_group4(void)
{
    struct { const char *name; int in_len; size_t cap; } cases[] = {
        // a txid, in the buffer the sign screen actually gives it
        { "txid into gt[80]", 64, 80 },
        // the proof hash and the receive addresses, which have slack
        { "hash into grp[96]", 64, 96 },
        { "bech32 into grouped[120]", 62, 120 },
        { "silent payment into grouped[200]", 117, 200 },
    };
    int bad = 0;
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        char in[256], out[256];
        for (int i = 0; i < cases[c].in_len; i++) in[i] = "0123456789abcdef"[i % 16];
        in[cases[c].in_len] = 0;
        wt_group4(in, out, cases[c].cap);
        int kept = 0;
        for (size_t i = 0; out[i]; i++) if (out[i] != ' ') kept++;
        if (kept != cases[c].in_len) {
            printf("  %-34s kept %d of %d characters\n",
                   cases[c].name, kept, cases[c].in_len);
            bad++;
        }
    }
    printf("group4: %d of %zu buffers drop a character\n",
           bad, sizeof cases / sizeof cases[0]);
    return bad;
}

// The accent on an address means exactly one thing: THESE are the characters to
// compare against your coordinator. So the two renderings of an address have to
// mark the same ones, or the screen teaches two rules and the caption under
// them ("compare these 8") is true of at most one.
//
// wt_addr_spans shows the whole address and lights the last eight. wt_addr_short
// elides the middle and lit a different eight -- the four after the prefix plus
// the last four -- so the only characters the two agreed on were the final four.
// And the elided line is drawn only for a single recipient, so an owner who
// learned to check the head four found no head marking at all on a
// multi-recipient transaction, where there are more addresses to get wrong.
static void lit_chars(lv_obj_t *sg, char *out, size_t cap)
{
    size_t o = 0;
    lv_color_t accent = wt_accent();
    uint32_t n = lv_spangroup_get_span_count(sg);
    for (uint32_t i = 0; i < n && o + 1 < cap; i++) {
        lv_span_t *sp = lv_spangroup_get_child(sg, (int32_t)i);
        if (!sp) continue;
        lv_style_value_t v;
        if (lv_style_get_prop(lv_span_get_style(sp), LV_STYLE_TEXT_COLOR, &v)
            != LV_STYLE_RES_FOUND)
            continue;
        if (v.color.red != accent.red || v.color.green != accent.green ||
            v.color.blue != accent.blue)
            continue;
        const char *t = lv_span_get_text(sp);
        for (; t && *t && o + 1 < cap; t++)
            if (*t != ' ') out[o++] = *t;   // spaces are grouping, not content
    }
    out[o] = 0;
}

static int check_addr_marks(void)
{
    // ONE silent-payment address, too long for a source line. Named rather
    // than written as two adjacent literals inside the array: adjacent
    // literals in an array initializer are exactly what a missing comma looks
    // like, and -Wstring-concatenation is right to ask.
    static const char SP_ADDR[] =
        "tsp1qqfaysl7pn7mknpmmsapdd6sczx8ncnnjk84gcm0xq2n66jjpm0sxsq"
        "mpuxc7nhj7gt9jqplhef2tncx40mgnjw8664kn7x09w5f63l8q8ymd0lna";
    static const char *ADDRS[] = {
        "bc1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz",   // mainnet segwit
        "tb1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz",   // testnet segwit
        "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2",           // base58: no prefix skip
        SP_ADDR,
    };
    lv_obj_t *scr = lv_obj_create(NULL);
    int bad = 0;
    for (size_t i = 0; i < sizeof ADDRS / sizeof ADDRS[0]; i++) {
        char body[80], elided[80];
        lit_chars(wt_addr_spans(scr, ADDRS[i], 700, wt_font_mono14()),
                  body, sizeof body);
        lit_chars(wt_addr_short(scr, ADDRS[i], wt_font_mono23()),
                  elided, sizeof elided);
        if (strcmp(body, elided) != 0) {
            printf("  %.14s... body lights \"%s\", elided lights \"%s\"\n",
                   ADDRS[i], body, elided);
            bad++;
        }
    }
    lv_obj_delete(scr);
    printf("address marks: %d of %zu addresses mark two different runs\n",
           bad, sizeof ADDRS / sizeof ADDRS[0]);
    return bad;
}

// The compared tail is what the owner is asked to check, so where nothing else
// on the screen shows it larger it renders one rung above the body. That rule
// was written for font14 and tested `f == wt_font14()` -- the PROPORTIONAL face
// -- while all four callers pass a mono one, so it could never fire and the
// tail never grew. This pins both halves: lifted where asked, and NOT lifted
// otherwise, because the single-recipient verify panel has 7px of slack and
// spends it on the caption.
static int check_addr_lift(void)
{
    static const char *A =
        "bc1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz";
    static const char *SP =
        "tsp1qqfaysl7pn7mknpmmsapdd6sczx8ncnnjk84gcm0xq2n66jjpm0sxsq"
        "mpuxc7nhj7gt9jqplhef2tncx40mgnjw8664kn7x09w5f63l8q8ymd0lna";
    lv_obj_t *scr = lv_obj_create(NULL);
    int bad = 0;
    struct { const char *name; const char *addr; int w; } cases[] = {
        { "bech32 in the change lane", A,  438 },
        { "bech32 in the full lane",   A,  722 },
        { "silent payment, change",    SP, 438 },
        { "silent payment, full",      SP, 722 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        lv_obj_t *flat = wt_addr_spans(scr, cases[i].addr, cases[i].w,
                                       wt_font_mono14());
        lv_obj_t *lift = wt_addr_spans_lift(scr, cases[i].addr, cases[i].w,
                                            wt_font_mono14());
        lv_obj_update_layout(flat);
        lv_obj_update_layout(lift);
        int hf = (int)lv_obj_get_height(flat), hl = (int)lv_obj_get_height(lift);
        // lifted must actually be taller (the bug was that it was not) ...
        if (hl <= hf) {
            printf("  %-28s lift did nothing (%dpx both)\n", cases[i].name, hf);
            bad++;
        }
        // ... and must not buy that with a whole extra line of address
        if (hl - hf > 8) {
            printf("  %-28s lift cost %dpx, a wrapped line\n",
                   cases[i].name, hl - hf);
            bad++;
        }
    }
    // Above the 14 rung the accent alone marks the tail: lifting is a no-op.
    lv_obj_t *a23 = wt_addr_spans(scr, A, 722, wt_font_mono23());
    lv_obj_t *b23 = wt_addr_spans_lift(scr, A, 722, wt_font_mono23());
    lv_obj_update_layout(a23);
    lv_obj_update_layout(b23);
    if (lv_obj_get_height(a23) != lv_obj_get_height(b23)) {
        puts("  mono23                       lift changed a body that already reads");
        bad++;
    }
    lv_obj_delete(scr);
    printf("address tail lift: %d problem(s)\n", bad);
    return bad;
}

// ---- FITCHECK_SELFTEST=1: prove each measurement still REPORTS -------------
//
// Every check in this file is defined by what it EXCUSES, so a clean sweep
// says nothing on its own. That was tolerable while ROW_BACKLOG held nineteen
// entries: the run printed them, and a number that moves is a number that is
// being computed. The backlog is a bare sentinel now, and "row labels
// ellipsised: 0 backlogged, 0 new" is a line a gate that stopped measuring
// would print WORD FOR WORD.
//
// Both directions, every case. A check that fires on everything passes the
// must-fire half exactly as a dead one passes the must-not, so neither half
// is worth having alone. The shapes go through the helpers above, which is
// the same code the sweep runs -- a selftest with its own arithmetic tests
// its own arithmetic.
static int selftest(void)
{
    int cases = 0, bad = 0;
#define CHK(what, cond) do {                                   \
        cases++;                                               \
        if (!(cond)) { printf("  selftest FAIL: %s\n", (what)); bad++; } \
    } while (0)

    // Long enough to overrun every lane on this device, short enough that no
    // budget below has to be invented to make the point.
    static const char LONG[] =
        "a sentence far too long for any row label on this device to hold";
    const int BUDGET = 200;

    // slot rung -- the font14 ratchet
    CHK("a long body in a small box falls to font14",
        slot_rung(LONG, 300, 40) == 14);
    CHK("a short body in a tall box does not",
        slot_rung("ok", 600, 200) != 14);

    // row label and sub width -- the two faces the rows are pinned to
    CHK("a long label overruns its budget at font23",
        text_px(LONG, wt_font23()) > BUDGET);
    CHK("a short label does not",
        text_px("ok", wt_font23()) <= BUDGET);
    CHK("a long sub overruns its budget at font14",
        text_px(LONG, wt_font14()) > BUDGET);
    CHK("a short sub does not",
        text_px("ok", wt_font14()) <= BUDGET);
    CHK("the same string measures wider at font23 than at font14",
        text_px(LONG, wt_font23()) > text_px(LONG, wt_font14()));

    // the backlogs. ROW_BACKLOG is {NULL, NULL} now, so the lookup has to
    // survive the sentinel AND match nothing; ROWSUB_BACKLOG still holds
    // entries, so it has to still find one. An emptied backlog whose lookup
    // silently matched everything would excuse the whole check.
    CHK("the emptied row backlog matches a surface it used to hold",
        !row_backlogged("de", "set/storage"));
    CHK("the emptied row backlog matches nothing else either",
        !row_backlogged("xx", "no-such-surface"));
    CHK("the emptied sub backlog matches a surface it used to hold",
        !rowsub_backlogged("nb-NO", "set/duress"));
    CHK("the emptied sub backlog matches nothing else either",
        !rowsub_backlogged("xx", "no-such-surface"));

    // kit icons. WT_ICON_* are all 3-byte UTF-8, decoded here the way the
    // sweep decodes them. U+E000 opens the private use area: nothing this
    // repo generates puts a glyph there.
    const unsigned char *k = (const unsigned char *)WT_ICON_KEY;
    const uint32_t keycp = ((uint32_t)(k[0] & 0x0Fu) << 12) |
                           ((uint32_t)(k[1] & 0x3Fu) << 6) | (k[2] & 0x3Fu);
    CHK("a kit icon is inked at font23", icon_inked(wt_font23(), keycp));
    CHK("a kit icon is inked at font34", icon_inked(wt_font34(), keycp));
    CHK("a codepoint no face has is not inked",
        !icon_inked(wt_font23(), 0xE000));

    // screen titles -- the real widgets and the real picker
    CHK("a long title in a narrow lane lands on the smallest rung",
        title_is_smallest(title_pick(LONG, 300, true, NULL)));
    CHK("a short title in a full lane does not",
        !title_is_smallest(title_pick("OK", 704, true, NULL)));

#undef CHK
    // A case deleted is a case that stops failing, and a selftest that
    // quietly shrinks is the thing it was written to prevent one level up.
    // Raise this WITH the case, never to make a run go green.
    enum { FIT_SELFTEST_CASES = 16 };
    if (cases != FIT_SELFTEST_CASES) {
        printf("  selftest FAIL: %d cases, expected %d -- a case was removed\n",
               cases, FIT_SELFTEST_CASES);
        bad++;
    }
    printf("fit selftest: %d cases, %d broken\n", cases, bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    lv_init();
    static uint8_t buf[800 * 40 * 2];
    lv_display_t *d = lv_display_create(800, 480);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Before the sweep, not instead of it: the sweep's answer is only worth
    // reading once the measurements behind it have been shown to still fire.
    if (getenv("FITCHECK_SELFTEST") && selftest()) return 1;

    int total_small = 0, key_small = 0, nfail = 0, en_small = 0;
    int row_cut = 0, row_known = 0;
    int sub_cut = 0, sub_known = 0;
    // SIM_LANG narrows the sweep to one locale, exactly as it does for the
    // walk. This is the English-only rule made real for THIS gate: the other
    // twenty locales carry wording that waits for the translation sweep, and
    // the mono faces pushed hundreds of those parked strings over their
    // budgets in one day -- failures no wording change of ours may touch.
    // The sweep runs the binary with SIM_LANG unset and sees all 21 again.
    const char *only = getenv("SIM_LANG");
    if (only && !*only) only = NULL;
#define MAXFAIL 64
    static char fails[MAXFAIL][96];
    for (int l = 0; l < I18N_LANG_N; l++) {
        const i18n_lang_t *li = i18n_lang_info(l);
        if (only && strcmp(only, li->code) != 0) continue;
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

            // Only font14 counts as a failure now. 23 is a real reading size,
            // and the notes wedged between controls can never reach 28.
            int rung = slot_rung(txt, SLOTS[i].w, SLOTS[i].h);
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
        char rlines[NROW][160];
        char slines[NSUBROW][160];
        int rbad = 0;
        for (int i = 0; i < NROW; i++) {
            const char *txt = tr(ROWS[i].key);
            int budget = row_label_budget(&ROWS[i]);
            const int px = text_px(txt, wt_font23());
            rlines[i][0] = '\0';
            if (px > budget) {
                rbad++;
                bool known = row_backlogged(li->code, ROWS[i].surface);
                if (known) row_known++;
                else       row_cut++;
                snprintf(rlines[i], sizeof rlines[i],
                         "  row  %-15s %3dpx / %3dpx  %s  \"%s\"",
                         ROWS[i].surface, px, budget,
                         known ? "ellipsis (backlog)" : "ELLIPSIS", txt);
            }
        }

        // Row sub-lines: fixed font14, one line, ellipsis on overflow. Same
        // budget as the label above, measured with the smaller face.
        int sbad = 0;
        for (int i = 0; i < NSUBROW; i++) {
            const row_t *r = row_by_surface(SUBROWS[i].surface);
            char body[192];
            if (SUBROWS[i].arg) {
                // The format string comes from the locale table, so the
                // compiler cannot check it. That is the point of the check:
                // measure what the screen draws, not the unexpanded template.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
                snprintf(body, sizeof body, tr(SUBROWS[i].key),
                         SUBROWS[i].arg);
#pragma GCC diagnostic pop
            } else {
                snprintf(body, sizeof body, "%s", tr(SUBROWS[i].key));
            }
            char built[256];
            snprintf(built, sizeof built, "%s%s",
                     SUBROWS[i].pfx ? SUBROWS[i].pfx : "", body);
            int budget = row_label_budget(r);
            const int px = text_px(built, wt_font14());
            slines[i][0] = '\0';
            if (px > budget) {
                sbad++;
                bool known = rowsub_backlogged(li->code, SUBROWS[i].surface);
                if (known) sub_known++;
                else       sub_cut++;
                snprintf(slines[i], sizeof slines[i],
                         "  sub  %-15s %3dpx / %3dpx  %s  \"%s\"",
                         SUBROWS[i].surface, px, budget,
                         known ? "ellipsis (backlog)" : "ELLIPSIS", built);
            }
        }

        printf("%-6s %-22s %2d/%d at font14, %d/%d rows, %d/%d subs\n",
               li->code, li->native, small, NSLOT, rbad, NROW,
               sbad, NSUBROW);
        for (int i = 0; i < NROW; i++)
            if (rlines[i][0]) puts(rlines[i]);
        for (int i = 0; i < NSUBROW; i++)
            if (slines[i][0]) puts(slines[i]);
        for (int i = 0; i < NSLOT; i++)
            if (strstr(lines[i], "cut ")) puts(lines[i]);
        total_small += small;
    }
    printf("\ntotal at font14: %d\n", total_small);
    printf("row labels ellipsised: %d backlogged, %d new\n", row_known, row_cut);
    printf("row subs ellipsised:   %d backlogged, %d new\n", sub_known, sub_cut);

    // Every icon the kit draws must exist, at every size, with real ink in it.
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
        { "WT_ICON_LANG",   WT_ICON_LANG   },
        { "WT_ICON_SHIELD", WT_ICON_SHIELD },
        { "WT_ICON_WHAT",   WT_ICON_WHAT   },
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
            if (icon_inked(FACES[j].f, cp)) continue;
            // Only now is the distinction worth the second lookup: absent
            // from the chain, or present as an empty placeholder box.
            lv_font_glyph_dsc_t g;
            bool ok = lv_font_get_glyph_dsc(FACES[j].f, &g, cp, 0);
            printf("FAIL: %s (U+%04X) %s in %s\n", ICONS[i].name,
                   (unsigned)cp, ok ? "is blank" : "is MISSING",
                   FACES[j].name);
            icon_bad++;
        }
    }
    if (icon_bad) {
        puts("\nAdd the codepoint to SYMS in tools/fonts/gen_fonts.sh and\n"
             "re-run it. A label may never reference a glyph the font lacks:\n"
             "LVGL draws an empty placeholder box, on a screen nobody can\n"
             "file a bug from.");
        return 1;
    }
    printf("kit icons: %d present and inked at 14/23/28/34\n",
           (int)(sizeof ICONS / sizeof *ICONS));

    // A lane used to sit here measuring the sub-line of the two-line SCAN KEY
    // control against its 340px box. The helper that drew that sub-line is
    // gone and so is the box. S_SP_BADGE is a fitted full-width note in
    // SLOTS above and a narrowed screen title in TITLE_SLOTS below, so the
    // old lane was measuring a surface no screen draws.

    if (key_small || en_small) {
        printf("\nFAIL: %d key-action button(s) and %d English slot(s) "
               "fell to font14:\n", key_small, en_small);
        for (int i = 0; i < nfail; i++)
            printf("  %s\n", fails[i]);
        puts("\nA button that spends, erases or verifies must not be the\n"
             "smallest type on its screen. Fix by shortening that locale's\n"
             "label, widening the box, or making it tall enough to wrap.\n"
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
    // only too small in Polish, and a box that drops to 14 without being a
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

    if (sub_cut) {
        printf("\nFAIL: %d row sub-line(s) ellipsise and are not in "
               "ROWSUB_BACKLOG.\n", sub_cut);
        puts("A standard wt_row sub is drawn at a FIXED font14 on one line\n"
             "with LV_LABEL_LONG_DOT, on the same budget as the label above\n"
             "it. It loses its last words silently, and on the NO UNDO rows\n"
             "the last words are the ones saying whether you keep a wallet.\n"
             "Shorten that locale's sub-line.");
        return 1;
    }

    // ---- screen titles against the lanes they actually get ----
    // Exercise the real widgets and wt_title_fit rather than duplicating its
    // measurement. Chrome titles walk mono 28 -> 23 -> 18 when their glyphs
    // allow it; ordinary wt_screen titles walk sans 34 -> 28 -> 23. Anything
    // put beside a title takes width away from it, and the only symptom is a
    // title two sizes smaller. The overlap gate cannot see that -- a smaller
    // title overlaps nothing.
    //
    // Settings is the screen this was written for. It USED to hand
    // wt_title_fit a narrowed lane, because three header pills shared the
    // title's row -- LANGUAGE (170), FIRMWARE (170) and a wordless theme chip
    // -- and this check is what sized that chip down to 44, because de's title
    // needs 269px to hold font28.
    //
    // Direction 1b moved all three into rows on the DEVICE tab, so Settings
    // gets the full lane again. The rest of this table covers every explicit
    // narrowed-title call plus RECEIVE's silent-payment help chip, whose lane
    // is now narrowed to match the identical fingerprint arrangement.
    {
        static const struct {
            const char *surface;
            int key, lane;
            bool chrome;
        } TITLE_SLOTS[] = {
            { "set/main",    STR_G_T,          704, true  },
            { "recv/sp",     STR_S_SP_BADGE,   640, true  },
            { "setup/prove", STR_W_PROVE_T,    436, true  },
            { "setup/taps",  STR_W_ENT_TAP_T,  436, true  },
            { "setup/rand",  STR_W_RAND_T,     436, true  },
            { "setup/words", STR_W_WRITE_T,    594, true  },
            { "setup/coin",  STR_W_COIN_T,     436, true  },
            { "setup/dice",  STR_W_DICE_T,     436, true  },
            { "setup/start", STR_W_SETUP_T,    496, true  },
            { "wallet/fp",   STR_D_FINGERPRINT,640, false },
        };
        int title_small = 0;
        for (size_t t = 0; t < sizeof TITLE_SLOTS / sizeof *TITLE_SLOTS; t++) {
            for (int l = 0; l < I18N_LANG_N; l++) {
                if (only && strcmp(only, i18n_lang_info(l)->code) != 0) continue;
                i18n_set_lang(l);
                const char *txt = tr(TITLE_SLOTS[t].key);
                int ls = 0;
                const lv_font_t *picked = title_pick(txt, TITLE_SLOTS[t].lane,
                                                     TITLE_SLOTS[t].chrome, &ls);
                if (title_is_smallest(picked)) {
                    lv_point_t sz;
                    lv_text_get_size(&sz, txt, picked, ls, 0, LV_COORD_MAX,
                                     LV_TEXT_FLAG_NONE);
                    printf("  title %-11s %-6s smallest  %dpx / %dpx  FAIL\n",
                           TITLE_SLOTS[t].surface, i18n_lang_info(l)->code,
                           sz.x, TITLE_SLOTS[t].lane);
                    title_small++;
                }
            }
        }
        printf("screen titles: %d lane(s), %d title-locale pair(s) at smallest rung\n",
               (int)(sizeof TITLE_SLOTS / sizeof *TITLE_SLOTS), title_small);
        if (title_small) {
            puts("\nFAIL: something beside a screen title has squeezed the\n"
                 "title to its smallest size. Narrow it, move it into a row, or\n"
                 "shorten that locale's title. A title on its smallest rung is a\n"
                 "title the owner no longer uses to know which screen they are on.");
            return 1;
        }
    }

    if (check_addr_lift()) {
        puts("\nFAIL: the compared tail does not size the way its callers ask.\n"
             "wt_addr_spans_lift exists because a tail at font14 marked only by\n"
             "colour is the whole of what a multi-recipient list shows about an\n"
             "address; wt_addr_spans exists because the single-recipient panel\n"
             "draws that run again, larger, and has no room to do it twice.");
        return 1;
    }

    if (check_addr_marks()) {
        puts("\nFAIL: the two renderings of an address light different\n"
             "characters. The accent is this screen's instruction about what to\n"
             "compare, and two instructions is none: an owner who learns one is\n"
             "misled by the other, and the elided line is not even drawn on a\n"
             "multi-recipient transaction. Mark the same run in both.");
        return 1;
    }

    if (check_group4()) {
        puts("\nFAIL: wt_group4 dropped a character. A grouped string is shown\n"
             "to be compared character by character against a coordinator, and\n"
             "a silently short one is compared against nothing. Give the caller\n"
             "the room, or stop the helper reserving space it does not use.");
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
