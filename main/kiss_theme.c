// Shared wallet UI kit. See kiss_theme.h. Every builder here matches the
// house style the screens shipped with, so porting a screen to the kit must
// not change a rendered pixel while the accent is MONO.
#include "kiss_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "kiss_fonts.h"

// Object identity, stamped into user_data. The addresses are what matter, not
// the strings: they let action_bar_ensure tell a screen built by wt_screen from
// an explainer card that happens to be the same size, and find the one bar it
// already made without keeping a static pointer that a screen teardown would
// leave dangling.
static const char WT_SCREEN_TAG[] = "wt_screen";
static const char WT_BAR_TAG[]    = "wt_action_bar";
// The squared-off top rung of the bar, tagged so the slide can carry it up
// with the fill when it grows the band under a screen that already built one.
static const char WT_BARCAP_TAG[] = "wt_action_bar_cap";
// The band slide's hit box. It is the one control that decides its screen's
// content line by itself, and it is not always on a wt_screen -- the receive
// gate builds its own -- so the ANSWER cannot come from the bar alone.
static const char WT_SLIDEBAND_TAG[] = "wt_slide_band";
// The [ ? ] tab, so a trail sharing its strip can stop before it.
static const char WT_HELPTAB_TAG[] = "wt_help_tab";
// The word on a band slide, so a caller can change it on arrival without
// counting children -- the sign screen turns SLIDE TO SIGN into SIGNING.
static const char WT_SLIDELBL_TAG[] = "wt_slide_label";
static const char WT_TITLE_TAG[]  = "wt_title";
// The blinking block after the title, tagged for the same reason the title is:
// a screen that puts something ELSE on the title row moves the title, and the
// cursor was measured off the title's old x. Reaching for it by child index
// is the mistake kiss_theme.h's note on wt_screen_title already records.
static const char WT_CURSOR_TAG[] = "wt_cursor";
static const char WT_SUB_TAG[]    = "wt_subtitle";

// The screen system's glyph guard and its guarded faces (defined with the
// chrome block below, used by every control that takes the mono scale).
static bool mono_can(const char *s);
static const lv_font_t *chrome18(const char *s);
static const lv_font_t *chrome21(const char *s);
static const lv_font_t *chrome23(const char *s);
static const lv_font_t *chrome28(const char *s);
// The public faces of the same guards, for pages composing their own
// chrome content.
const lv_font_t *wt_chrome18(const char *s) { return chrome18(s); }
const lv_font_t *wt_chrome21(const char *s) { return chrome21(s); }
const lv_font_t *wt_chrome23(const char *s) { return chrome23(s); }
const lv_font_t *wt_chrome28(const char *s) { return chrome28(s); }
static const char WT_DECOR_TAG[]  = "wt_decor";
static const char WT_ROW_ICON_TAG[] = "wt_row_icon";
// The wide row's label, so the "?" chip can be measured against the TEXT
// rather than the 250px box the label is capped to. Asking the object how
// wide it is answers 250 and puts the chip on top of the words.
static const char WT_ROW_LABEL_TAG[] = "wt_row_label";
// The wide row's CONTROL -- the chip a value sits in, or the value itself on a
// row that only opens a screen. Tagged so the page can land it a beat after
// the row it belongs to without knowing which of the four kinds built it.
static const char WT_ROW_CTRL_TAG[] = "wt_row_ctrl";

void wt_mark_decor(lv_obj_t *o)
{
    if (o) lv_obj_set_user_data(o, (void *)WT_DECOR_TAG);
}

bool wt_is_decor(const lv_obj_t *o)
{
    return o && lv_obj_get_user_data((lv_obj_t *)o) == (void *)WT_DECOR_TAG;
}

// Find one of wt_screen's own children by its tag. By tag and not by index:
// the subtitle is only child 1 on the screens that HAVE a subtitle, and on the
// ones that do not, child 1 is whatever the screen built first.
static lv_obj_t *wt_tagged(lv_obj_t *scr, const char *tag)
{
    if (!scr) return NULL;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (lv_obj_get_user_data(c) == (void *)tag) return c;
    }
    return NULL;
}

// The nat ("native") composites start with Montserrat for ASCII, symbols, and
// Latin text, then fall back directly to the active locale's regional CJK
// font. A single ja -> ko -> zh chain would render shared Han codepoints with
// whichever font appeared first, mixing Japanese glyph forms into Simplified
// Chinese.
static lv_font_t s_nat14[I18N_FC_ZH + 1];
static lv_font_t s_nat23[I18N_FC_ZH + 1];
static lv_font_t s_nat28[I18N_FC_ZH + 1];
// The body composites the accessors hand out: IoskeleyMono first, the nat
// chain behind it. English renders pure mono; an accent resolves in
// Montserrat and CJK in the regional face, per glyph. These are COPIES of the
// mono faces -- wt_font_monoNN() keeps returning the raw chainless ones, so a
// localised string pointed at a data face still fails loudly.
static lv_font_t s_font14[I18N_FC_ZH + 1];
static lv_font_t s_font23[I18N_FC_ZH + 1];
static lv_font_t s_font28[I18N_FC_ZH + 1];
// 34 exists for Latin/Cyrillic ONLY -- the CJK subsets at this size would add
// ~4.5MB to an app already using 8.9MB of a 12MB partition, and CJK glyphs read
// considerably larger than Latin at the same pixel size anyway. Hence a single
// face, not an array: there is deliberately no per-class variant to pick.
static lv_font_t s_nat34;
static lv_font_t s_font34;
static bool s_fonts_ready;

static void fonts_init(void)
{
    if (s_fonts_ready) return;
    for (int i = 0; i <= I18N_FC_ZH; i++) {
        s_nat14[i] = font_kiss_lat14;
        s_nat23[i] = font_kiss_lat23;
        s_nat28[i] = font_kiss_lat28;
    }
    s_nat14[I18N_FC_JA].fallback = &font_kiss_ja14;
    s_nat14[I18N_FC_KO].fallback = &font_kiss_ko14;
    s_nat14[I18N_FC_ZH].fallback = &font_kiss_zh14;
    s_nat23[I18N_FC_JA].fallback = &font_kiss_ja23;
    s_nat23[I18N_FC_KO].fallback = &font_kiss_ko23;
    s_nat23[I18N_FC_ZH].fallback = &font_kiss_zh23;
    s_nat28[I18N_FC_JA].fallback = &font_kiss_ja28;
    s_nat28[I18N_FC_KO].fallback = &font_kiss_ko28;
    s_nat28[I18N_FC_ZH].fallback = &font_kiss_zh28;
    // Chains to the 28px Japanese face, the largest CJK size that exists. A
    // glyph missing from an LVGL font is an infinite loop in the renderer, not
    // a tofu box, so this must never dead-end -- even though wt_font34() is
    // supposed to keep CJK locales away from this face entirely. Belt and
    // braces, because the failure mode is a hung device.
    s_nat34 = font_kiss_lat34;
    s_nat34.fallback = &font_kiss_ja28;
    for (int i = 0; i <= I18N_FC_ZH; i++) {
        s_font14[i] = font_kiss_mono14;
        s_font14[i].fallback = &s_nat14[i];
        s_font23[i] = font_kiss_mono23;
        s_font23[i].fallback = &s_nat23[i];
        s_font28[i] = font_kiss_mono28;
        s_font28[i].fallback = &s_nat28[i];
    }
    s_font34 = font_kiss_mono34;
    s_font34.fallback = &s_nat34;
    s_fonts_ready = true;
}

static int font_class_for_lang(int lang)
{
    int fc = i18n_lang_info(lang)->font_class;
    return (fc >= I18N_FC_LAT && fc <= I18N_FC_ZH) ? fc : I18N_FC_LAT;
}

// The nat faces, by current locale. The chrome guards and the language picker
// want the Montserrat-primary composites: a native name like "Español" set on
// the mono-primary face would mix two typefaces inside one word.
static const lv_font_t *nat14(void)
{
    fonts_init();
    return &s_nat14[font_class_for_lang(i18n_get_lang())];
}
static const lv_font_t *nat23(void)
{
    fonts_init();
    return &s_nat23[font_class_for_lang(i18n_get_lang())];
}
static const lv_font_t *nat28(void)
{
    fonts_init();
    return &s_nat28[font_class_for_lang(i18n_get_lang())];
}

const lv_font_t *wt_font14_for_lang(int lang)
{
    fonts_init();
    return &s_nat14[font_class_for_lang(lang)];
}

// NOT wt_font14_for_lang: that one is the picker's and returns the nat face.
const lv_font_t *wt_font14(void)
{
    fonts_init();
    return &s_font14[font_class_for_lang(i18n_get_lang())];
}
const lv_font_t *wt_font23(void)
{
    fonts_init();
    return &s_font23[font_class_for_lang(i18n_get_lang())];
}
const lv_font_t *wt_font28(void)
{
    fonts_init();
    return &s_font28[font_class_for_lang(i18n_get_lang())];
}

// The top rung, for page titles and primary buttons. Latin/Cyrillic locales
// get the real 34px face; every CJK locale gets 28 instead, because no CJK
// face exists at 34 and handing a Latin-only font to a locale whose every
// string is CJK would put the renderer on the fallback path for the whole
// screen. 28 is not a downgrade there: Han and Kana fill their em box far more
// than Latin does, so a 28px CJK title already reads about as large as a 34px
// Latin one.
const lv_font_t *wt_font34(void)
{
    fonts_init();
    return font_class_for_lang(i18n_get_lang()) == I18N_FC_LAT
             ? &s_font34
             : &s_font28[font_class_for_lang(i18n_get_lang())];
}

// The fixed pitch faces. No array, no fallback, no per locale variant, and
// none of that is an oversight.
//
// They exist for strings that are never translated: addresses, fingerprints,
// derivation paths, and amounts. A locale cannot change any of those, so there
// is nothing for a font class to select between, and a CJK subset at these
// sizes would cost more than the whole Latin set does.
//
// Deliberately NO fallback chain. Everywhere else a chain is load bearing,
// because a missing glyph should degrade to a smaller face rather than vanish.
// Here the opposite is wanted: if a localised string is ever pointed at one of
// these by mistake, it must be obvious. With CONFIG_LV_USE_FONT_PLACEHOLDER=y
// the lookup ends in a blank box half a line wide, which someone will file a
// bug about. A chain would render it one size small and nobody would notice
// the wiring is wrong. (The renderer does not hang on a missing glyph; the
// comment above about s_font34 predates LV_USE_FONT_PLACEHOLDER being on.)
const lv_font_t *wt_font_mono14(void) { return &font_kiss_mono14; }
const lv_font_t *wt_font_mono18(void) { return &font_kiss_mono18; }
const lv_font_t *wt_font_mono21(void) { return &font_kiss_mono21; }
const lv_font_t *wt_font_mono23(void) { return &font_kiss_mono23; }
const lv_font_t *wt_font_mono28(void) { return &font_kiss_mono28; }
const lv_font_t *wt_font_mono34(void) { return &font_kiss_mono34; }

// The Sign hero, and nothing else. Thirteen glyphs, digits and space and full
// stop, so it cannot represent a letter even if handed one.
const lv_font_t *wt_font_num48(void) { return &font_kiss_num48; }

// The sink from kiss_theme.h. NULL on device and in any host build that has not
// asked, so this costs a null check on a path that already measured text.
#ifndef ESP_PLATFORM
static wt_fit_sink_t s_fit_sink;
void wt_fit_set_sink(wt_fit_sink_t fn) { s_fit_sink = fn; }
#define WT_FIT_GAVE_UP(kind_, txt_, w_, h_) \
    do { if (s_fit_sink) s_fit_sink((kind_), (txt_), (w_), (h_)); } while (0)

static wt_cut_sink_t s_cut_sink;
void wt_cut_set_sink(wt_cut_sink_t fn) { s_cut_sink = fn; }
// Measured before the label is handed the string, because LVGL replaces the
// text with the dotted form and the original is unrecoverable afterwards.
// `ls` is the label's letter spacing, and it is not optional: a row LABEL is
// drawn at ls 2 and a sub-line at 0, so measuring both at 0 under-reports every
// label by two pixels a character -- which is most of a word on a 250px lane.
// ---- reading level (see the sink's `kind` note in kiss_theme.h) ------------
// Two cheap measures, both of them about the same thing: whether a sentence
// can be read once. Words per sentence is the honest one -- it is what every
// readability formula is mostly measuring -- and syllables catch the word
// that is short to write and hard to read.
//
// Vowel GROUPS, not vowels: "queue" is one, "coordinator" is five. A trailing
// silent e does not count, "le" after a consonant does ("table"). It is a
// heuristic and it is allowed to be: it only has to separate "keys" from
// "deterministic", and it never fires on a TECHNICAL line, where the real
// terms are as long as the standard made them.
static int wt_syllables(const char *w, int n)
{
    int syl = 0;
    bool prev_vowel = false;
    for (int i = 0; i < n; i++) {
        const char c = (w[i] >= 'A' && w[i] <= 'Z') ? (char)(w[i] + 32) : w[i];
        const bool v = c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
                       c == 'u' || c == 'y';
        if (v && !prev_vowel) syl++;
        prev_vowel = v;
    }
    // A silent trailing e, unless it is the only vowel ("the") or follows a
    // consonant as "le" ("table", "little").
    if (n >= 2 && (w[n - 1] == 'e' || w[n - 1] == 'E') && syl > 1) {
        const char p2 = (w[n - 2] >= 'A' && w[n - 2] <= 'Z')
                            ? (char)(w[n - 2] + 32) : w[n - 2];
        if (p2 != 'l') syl--;
    }
    return syl < 1 ? 1 : syl;
}

static void wt_read_measure(const char *txt);
static void wt_term_report(const char *body, int want, int floor_y);

// ---- the widow: a two line body whose second line is a stub ---------------
// A paragraph that wraps to two lines and leaves three words on the second is
// a paragraph two words too long, and nothing in the source says so: the
// string looks fine, every fit helper is happy, and the screen has a ragged
// hole in it. G_HELP_BODY shipped that way -- "Keys come from your seed words
// and / passphrase." -- and came off the bench as "it is two lines!!! you can
// make it ONE".
//
// ENGLISH ONLY, like READ and for the same reason: the wrap is simulated the
// way LVGL breaks Latin text, and a script that breaks per character has no
// widows to find. overlapcheck filters it to en.
#define WT_WIDOW_PCT 35    // a last line under this much of the lane is a stub

// The break LVGL would pick: the last space at which the head still fits.
// Returns the width of what is left after it, or -1 when there is no break.
static int widow_tail_w(const char *txt, const lv_font_t *f, int lane)
{
    char buf[256];
    const size_t n = strlen(txt);
    if (n == 0 || n >= sizeof buf) return -1;
    size_t brk = 0;
    for (size_t p = 0; p < n; p++) {
        if (txt[p] != ' ') continue;
        lv_memcpy(buf, txt, p);
        buf[p] = 0;
        lv_point_t s;
        lv_text_get_size(&s, buf, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (s.x > lane) break;
        brk = p;
    }
    if (brk == 0) return -1;
    lv_point_t s;
    lv_text_get_size(&s, txt + brk + 1, f, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return (int)s.x;
}

static void wt_widow_measure(const char *txt, const lv_font_t *f, int lane)
{
    if (!s_cut_sink || !txt || !*txt || lane <= 0) return;
    // A hard break is the author's own line and never a widow.
    if (strchr(txt, '\n')) return;
    const int lh = lv_font_get_line_height(f);
    if (lh <= 0) return;
    lv_point_t all;
    lv_text_get_size(&all, txt, f, 0, 0, lane, LV_TEXT_FLAG_NONE);
    // TWO lines exactly. One is already right, and three or more is a body
    // rather than a sentence that nearly fits.
    if (all.y <= lh || all.y > lh * 2) return;
    const int tail = widow_tail_w(txt, f, lane);
    if (tail <= 0) return;
    if (tail * 100 < lane * WT_WIDOW_PCT)
        s_cut_sink("widow", txt, tail, lane);
}

static void wt_sub_measure(const char *kind, const char *txt,
                           const lv_font_t *f, int ls, int lane)
{
    if (!s_cut_sink || !txt || !*txt) return;
    lv_point_t sz;
    lv_text_get_size(&sz, txt, f, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > lane) s_cut_sink(kind, txt, (int)sz.x, lane);
}

static void wt_term_report(const char *body, int want, int floor_y)
{
    if (s_cut_sink) s_cut_sink("term", body, want, floor_y);
}

// A word that cannot begin a NAME. Each of these opens a clause, and the word
// after it settles nothing -- by the time one has been read the caption is a
// sentence, whatever follows.
static const char *const WT_CAP_CLAUSE[] = {
    "WHO", "WHAT", "WHEN", "WHERE", "WHY", "HOW", "WHICH", "WHETHER",
    "ONCE", "AFTER", "BEFORE", "UNTIL", "WHILE", "SINCE", "IF",
};

// wt_value_card's CAPTION, which is a MARK and is drawn at font14 for that
// reason: an eyebrow naming the figure under it, the way FINGERPRINT names a
// fingerprint. Hand that slot a clause and the half of the card an owner has
// to READ is set in the mark size -- the same rule the fit helpers above
// exist to enforce, broken from the other end, and invisible to every one of
// them because nothing overflowed and no font was chosen.
//
// It shipped that way and came straight off the bench: "why is WHO CAN SEE IT
// so tiny and ANYONE, FOREVER so big", about ONCE YOU SIGN over NOBODY CAN
// REDIRECT IT and WHO CAN SEE IT over ANYONE, FOREVER.
//
// BOTH tells are needed and neither is redundant. No length separates these:
// the captions that were right run one to three words (USED OF TOTAL,
// RECIPIENT GETS, SEED WORDS) and the two that were wrong were three and
// four. So a count catches WHO CAN SEE IT and only the leading word catches
// ONCE YOU SIGN.
//
// Scoped to the VALUE CARD on purpose. wt_row's sub-line and the entropy
// screen's SOURCE 1 WHAT YOU POINT AT are also font14 captions, and both are
// headers over a body that carries the content -- here the caption and the
// value are the whole card, so prose in the caption has nowhere else to be
// read. English only, like "words" and "long" above.
static void wt_cap_measure(const char *txt)
{
    if (!s_cut_sink || !txt || !*txt) return;
    int words = 0;
    char first[16];
    int flen = 0;
    const char *p = txt;
    while (*p) {
        const char *w = p;
        bool has_alpha = false;
        while (*p && *p != ' ' && *p != '\n') {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
                has_alpha = true;
            p++;
        }
        if (has_alpha && ++words == 1) {
            int n = (int)(p - w);
            if (n > (int)sizeof first - 1) n = (int)sizeof first - 1;
            for (int i = 0; i < n; i++) {
                const char c = w[i];
                first[flen++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            first[flen] = 0;
        }
        while (*p == ' ' || *p == '\n') p++;
    }
    for (unsigned i = 0; i < sizeof WT_CAP_CLAUSE / sizeof WT_CAP_CLAUSE[0]; i++)
        if (strcmp(first, WT_CAP_CLAUSE[i]) == 0) {
            s_cut_sink("mark", txt, words, 0);
            return;
        }
    if (words > WT_CAP_MAX_WORDS)
        s_cut_sink("mark", txt, words, WT_CAP_MAX_WORDS);
}

// One pass over a body: the longest sentence, and the longest word. Sentences
// break on . ! ? and on a newline, because a paragraph break ends one too.
static void wt_read_measure(const char *txt)
{
    if (!s_cut_sink || !txt || !*txt) return;
    int words = 0, worst_words = 0;
    const char *p = txt;
    while (*p) {
        // A WORD is a run containing at least one ASCII letter, so a mark, a
        // figure and a bare "#0" are all worth nothing.
        const char *w = p;
        bool has_alpha = false;
        while (*p && *p != ' ' && *p != '\n') {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
                has_alpha = true;
            p++;
        }
        int n = (int)(p - w);
        // Trailing punctuation belongs to the sentence, not to the word.
        int core = n;
        while (core > 0 && !((w[core-1] >= 'a' && w[core-1] <= 'z') ||
                             (w[core-1] >= 'A' && w[core-1] <= 'Z')))
            core--;
        if (has_alpha) {
            words++;
            const int syl = wt_syllables(w, core);
            if (syl > WT_READ_MAX_SYLL) {
                char one[48];
                int k = core < (int)sizeof one - 1 ? core : (int)sizeof one - 1;
                lv_memcpy(one, w, (size_t)k);
                one[k] = 0;
                s_cut_sink("long", one, syl, WT_READ_MAX_SYLL);
            }
        }
        // Did this word end a sentence?
        bool ends = false;
        for (int i = core; i < n; i++)
            if (w[i] == '.' || w[i] == '!' || w[i] == '?') ends = true;
        if (*p == '\n') ends = true;
        if (ends) {
            if (words > worst_words) worst_words = words;
            words = 0;
        }
        while (*p == ' ' || *p == '\n') p++;
    }
    if (words > worst_words) worst_words = words;
    if (worst_words > WT_READ_MAX_WORDS)
        s_cut_sink("words", txt, worst_words, WT_READ_MAX_WORDS);
}
#else
#define WT_FIT_GAVE_UP(kind_, txt_, w_, h_) ((void)0)
#define wt_sub_measure(kind_, txt_, f_, ls_, lane_) ((void)0)
#define wt_widow_measure(txt_, f_, lane_) ((void)0)
// The reading level and the term line measure the same way and report through
// the same sink, so they compile out with it. This block is the OUTER else --
// nesting a second ESP guard inside the host-only half is how the two of them
// reached the device build undeclared, which only the device compiler saw.
#define wt_read_measure(txt_) ((void)0)
#define wt_term_report(b_, w_, f_) ((void)0)
#define wt_cap_measure(txt_) ((void)0)
#endif

// The ladder, without the report. For text that is the OWNER'S and not the
// product's -- the passphrase echo is the only one -- where landing on font14
// is the honest answer to a 90 character secret and there is no copy for
// anybody to cut. A backlog entry cannot cover this: it would have to match the
// walk's own test passphrase, and real input is whatever somebody types.
static const lv_font_t *body_font_ladder(const char *txt, int w, int max_h,
                                         bool report)
{
    if (!txt || !*txt)
        return wt_font28();
    lv_point_t sz;
    // Three rungs, and the last one is 21. This used to fall straight from 28
    // to 14, so one row of overflow cost a reader 64% of the glyph size for
    // nothing; then it fell to 14 at the bottom anyway. 23 is the same face at
    // line height 29 and takes most of what 28 cannot, and 21 catches the
    // rest.
    // Measured per locale: the same sentence is far taller in ja/ko/zh, and a
    // Cyrillic or Vietnamese translation often runs 40% longer than the English.
    lv_text_get_size(&sz, txt, wt_font28(), 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y <= max_h)
        return wt_font28();
    lv_text_get_size(&sz, txt, wt_font23(), 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y <= max_h)
        return wt_font23();
    // It gave up, so it says so -- the same sink wt_note_fit feeds. This was
    // the hole: EVERY tall row's sub-line and every wt_wraph body comes
    // through here, and only the note path was ever reported, so THIS DEVICE,
    // the audit chooser and the noise-source row all shipped at font14 with
    // the FIT gate green. The size filter lives in the gate, not here: this
    // function has no idea whether 24px is a caution row's lane or a thrown
    // away budget, and the gate does.
    // THE FLOOR. 21 where the string can be set in mono, 23 where it cannot,
    // and font14 only on the typed path -- an owner's own 90 character
    // passphrase has no copy for anybody to cut, and that is the whole of the
    // exemption.
    if (!report) return wt_font14();
    const lv_font_t *floor = mono_can(txt) ? wt_font_mono21() : wt_font23();
    // The report fires when the FLOOR is not enough, not when 23 was not.
    // Landing on 21 is a legal rung and most of the device's longer bodies do;
    // landing UNDER it is impossible, so a body that still overflows here is a
    // string too long for its block and the gate has to say which one.
    lv_text_get_size(&sz, txt, floor, 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y > max_h) WT_FIT_GAVE_UP("body", txt, w, max_h);
    return floor;
}

// Amber is a MARK colour (see the note on WT_WARN): a caution's WORDS take the
// accent, its GLYPH and its lamp keep WT_WARN. Every text site that may be
// handed a status colour runs it through here, so the rule lives in one place
// and a caller that means "this is a caution" still says so.
lv_color_t wt_ink_for(lv_color_t col)
{
    return lv_color_eq(col, WT_WARN) ? wt_accent() : col;
}

const lv_font_t *wt_body_font(const char *txt, int w, int max_h)
{
    return body_font_ladder(txt, w, max_h, true);
}

const lv_font_t *wt_body_font_typed(const char *txt, int w, int max_h)
{
    return body_font_ladder(txt, w, max_h, false);
}

static int s_accent = WT_ACC_MONO;

static const uint32_t ACC_HEX[WT_ACC_N] = {
    // MONO: a cool pale STEEL, not WT_INK. It was 0xE8EEF7 -- the ink, to the
    // byte -- which was survivable only while amber carried the emphasis on
    // every screen. The moment amber became a mark colour, MONO had no
    // emphasis channel at all: every value, title, action and chevron rendered
    // as the same white as the body text, and the SIGN screen came back from
    // the bench as flat.
    //
    // The gate knew first. oc_check_colour_roles opens by returning early when
    // the accent equals the ink, so MONO was the one theme with no colour
    // supervision anywhere in the app -- not a carve-out anybody chose, just
    // what an accent that is not a colour forces.
    //
    // 0x9FB6D4 stays monochrome to look at (a desaturated blue grey, no hue a
    // reader would name) and is far enough off the ink to do the accent's job.
    0x9FB6D4,   // MONO
    0x35D07F,   // GREEN (matches the home art dot)
    // CYPHERPINK started at 0xFF3EA5 and went to 0xC45CE8, and that overshot:
    // the only real complaint about the original was that at R=255 it sat about
    // 24 degrees of hue from WT_STOP (0xFF4D5E), so a red warning and ordinary
    // accent chrome read as the same family on a lit panel. Solving that by
    // going purple solved a problem nobody had -- a theme called CYPHERPINK
    // should be pink.
    //
    // 0xE85AB8 is the answer to the actual constraint. The red channel drops
    // from 255 to 232 and the hue moves further round, so it is not in WT_STOP's
    // family; it is plainly pink rather than violet; and its relative luminance
    // sits between the two it replaces, so nothing about contrast changes.
    // Status colours never move, so the accent is the one that has to.
    0xE85AB8,   // CYPHERPINK
    0xFF8A3D,   // ORANGE
};
static const uint32_t ACC_BG_HEX[WT_ACC_N] = {
    0x232E42,   // MONO: cool ink glass, bright enough that "selected" is obvious
    0x102417,   // GREEN
    0x261724,   // CYPHERPINK
    0x2B190D,   // ORANGE
};
// Each row is its accent scaled by the same per-channel ratios the pink pair
// has always used, so a new accent gets a fill and a pressed fill that sit at
// the same depth below it rather than being picked by eye.
static const uint32_t ACC_PRESS_HEX[WT_ACC_N] = {
    // MONO's pressed fill was already a steel blue, chosen against an accent
    // that was pure ink; against the steel accent it is the same relative
    // darkening the other three use, so it does not move.
    0x33405A,
    0x173823,
    0x342135,
    0x3A2513,
};

void wt_accent_set(int id) { s_accent = (id >= 0 && id < WT_ACC_N) ? id : WT_ACC_MONO; }
int  wt_accent_get(void)   { return s_accent; }
lv_color_t wt_accent(void) { return lv_color_hex(ACC_HEX[s_accent]); }
lv_color_t wt_primary(void) { return wt_accent(); }
lv_color_t wt_accent_bg(void) { return lv_color_hex(ACC_BG_HEX[s_accent]); }
const char *wt_accent_name(void)
{
    static const char *NM[WT_ACC_N] = {"MONO", "GREEN", "CYPHERPINK", "ORANGE"};
    return NM[s_accent];
}
lv_color_t wt_accent_pressed(void) { return lv_color_hex(ACC_PRESS_HEX[s_accent]); }

void wt_lock_565(int *r5, int *g6, int *b5)
{
    // The accent, in every theme. MONO used to be special-cased to WT_OK here
    // because its accent WAS the ink and a white lock would have said nothing
    // -- the comment called it "the only place on the device a status colour
    // and an accent trade places", which was true and was a symptom. MONO has
    // a real accent now, so the exception goes with the reason for it.
    uint32_t hex = ACC_HEX[s_accent];
    if (r5) *r5 = (int)((hex >> 19) & 0x1F);
    if (g6) *g6 = (int)((hex >> 10) & 0x3F);
    if (b5) *b5 = (int)((hex >>  3) & 0x1F);
}

// largest of {23, 14} that fits (defined with wt_note); used by the subtitle too
static const lv_font_t *note_font(const char *txt, int w, int max_h);

// The card every wallet screen sits inside. Purely decorative: it is the FIRST
// child, so it draws behind everything, and every screen's absolute coordinates
// are untouched by its arrival. Inset 8 with radius 16 and a WT_EDGE hairline,
// which is what turns a set of objects floating on the panel into one surface
// with a boundary -- the single biggest difference between the shipped screens
// and the design review's drawings.
//
// Not clickable and not scrollable, for the same reason the action bar is not:
// a tap that misses a control must fall through to whatever is behind it.
//
// The radius is named because the action bar has to reproduce it: the bar fills
// the bottom of this card, so anything that changes the curve here has to change
// the curve there in the same edit or the boundary breaks at the two corners.
#define WT_CARD_R 16
static void screen_card(lv_obj_t *scr)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 8, 8);
    lv_obj_set_size(card, 784, 464);
    lv_obj_set_style_radius(card, WT_CARD_R, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, WT_EDGE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

#ifdef SIMULATOR
// ---- screen coverage, simulator only ----
//
// A layout gate that never draws a screen cannot have an opinion about it.
// whatseed_open was a title, a subtitle and one 704x232 paragraph -- BARE by
// rule 1, on the screen a newcomer opens to find out what a seed is -- for its
// entire life, and every gate reported clean the whole time, because no walk
// stop ever rendered it. It was found by accident, the day it got a stop.
//
// So the walk records both halves: which screen titles it BUILT, and which of
// those a save() actually captured for the gate to question. The difference is
// the list of screens nothing has ever checked. Titles are the key because
// they are what wt_screen already has and what identifies a screen to a
// reader; a screen with no translated title is skipped rather than guessed at.
static uint8_t s_wt_built[STR_N];
static uint8_t s_wt_captured[STR_N];
static int     s_wt_cur = -1;

// Pointer identity, not strcmp: tr() hands back the table entry itself, so the
// match is exact and costs no string compares. English is checked second
// because tr() falls back to it for a key a locale has not filled in.
static int wt_title_id(const char *title)
{
    if (!title || !*title) return -1;
    const char *const *tbl = i18n_tables[i18n_get_lang()];
    for (int i = 0; i < STR_N; i++) if (tbl[i] == title) return i;
    const char *const *en = i18n_tables[I18N_EN];
    for (int i = 0; i < STR_N; i++) if (en[i] == title) return i;
    return -1;   // a literal title: nothing to name it by, so not tracked
}

// Called by the walk's save(). The active screen is the most recently built
// one -- mk_screen deletes its predecessor -- and an overlay saved on top of a
// screen still means that screen was on the panel, which is what is being
// claimed.
void wt_sim_capture(void)
{
    if (s_wt_cur >= 0) s_wt_captured[s_wt_cur] = 1;
}

// The English title, which names the screen to a reader better than a key
// would. The generated header carries no key-name table and this needs no new
// one: the title IS how anyone refers to the screen.
const char *wt_sim_title_key(int id)
{
    if (id < 0 || id >= STR_N) return "?";
    const char *s = i18n_tables[I18N_EN][id];
    return s ? s : "?";
}

// Fills `out` with the ids of screens built but never captured. Returns how
// many there were, which may exceed max.
int wt_sim_uncaptured(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < STR_N; i++) {
        if (s_wt_built[i] && !s_wt_captured[i]) {
            if (n < max) out[n] = i;
            n++;
        }
    }
    return n;
}

// Every title the walk built at all. This is the other half of the question:
// "built but never captured" cannot see a screen the walk never opens, and
// that is exactly the state whatseed was in. tools/check_screen_coverage.py
// diffs this against the title keys in the source.
int wt_sim_built(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < STR_N; i++) {
        if (s_wt_built[i]) {
            if (n < max) out[n] = i;
            n++;
        }
    }
    return n;
}
#endif

#ifndef ESP_PLATFORM
// Host only, the CUT sink's own guard: the sink type is declared in the same
// gate-only block of the header, and firmware has nothing to report to.
static wt_screen_sink_t s_screen_sink;
void wt_screen_set_sink(wt_screen_sink_t fn) { s_screen_sink = fn; }
#endif

lv_obj_t *wt_screen(lv_obj_t *parent, const char *title, const char *sub)
{
#ifndef ESP_PLATFORM
    if (s_screen_sink) s_screen_sink(title);
#endif
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, WT_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    // NOT scrollable, which lv_obj_create makes it by default. No page in this
    // app scrolls -- every screen is laid out absolutely and overlapcheck fails
    // anything below WT_CONTENT_BOTTOM, so a scroll offset can only ever be
    // damage. What it actually cost: LVGL hands a press to the nearest
    // SCROLLABLE ancestor as soon as the finger moves past its scroll limit,
    // and sends PRESS_LOST to whatever was under it. Both screens where the
    // owner DRAWS -- the duress stroke and their own letters -- are a
    // transparent catcher on a wt_screen, so every stroke was being stolen a
    // few pixels in and the ink stopped following the finger. Neither screen
    // had a walk stop, so no gate had ever drawn on either of them.
    // Scrolling lists (kiss_recv.c's address list) set the flag on their own
    // container and are untouched by this.
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(scr);
    screen_card(scr);
#ifdef SIMULATOR
    s_wt_cur = wt_title_id(title);
    if (s_wt_cur >= 0) s_wt_built[s_wt_cur] = 1;
#endif

    // The page title is the one label on every screen, so it sets the tone for
    // how big the device "feels". wt_font34 gives Latin/Cyrillic a real 34px
    // face and hands CJK locales 28, which is why this can grow without a CJK
    // font at 34 existing.
    //
    // Header geometry is tight and deliberate: 34 has a ~45px line box, so the
    // title moved up to y=18 to keep its descenders off the subtitle, and the
    // subtitle moved to y=66 with a 29px budget -- exactly one line at font23,
    // landing on 95, one pixel clear of the y=96 content line every screen
    // builds against. Loosen any of those three numbers and the subtitle either
    // drops to font14 or collides with the first row of content.
    lv_obj_t *cap = lv_label_create(scr);   // note_font: defined with wt_note below
    lv_label_set_text(cap, title);
    lv_obj_set_style_text_color(cap, wt_accent(), 0);
    lv_obj_set_style_text_font(cap, wt_font34(), 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 48, 18);
    lv_obj_set_user_data(cap, (void *)WT_TITLE_TAG);

    if (sub) {
        // The subtitle gets ONE line, between the title and content at y=96.
        // Sized to fit, not assumed to fit: at a fixed font23 the longer
        // subtitles ran straight off the right edge of the panel, and a label
        // with no width clips silently instead of wrapping.
        lv_obj_t *s = lv_label_create(scr);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_color(s, WT_MUT, 0);
        lv_obj_set_style_text_font(s, note_font(sub, 704, 29), 0);
        lv_obj_set_width(s, 704);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s, 48, 66);
        lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
    }
    lv_obj_set_user_data(scr, (void *)WT_SCREEN_TAG);
    wt_title_fit(scr, 704);   // 48..752, the page margins
    return scr;
}

lv_obj_t *wt_screen_title(lv_obj_t *scr)
{
    return wt_tagged(scr, WT_TITLE_TAG);
}

lv_obj_t *wt_screen_cursor(lv_obj_t *scr)
{
    return wt_tagged(scr, WT_CURSOR_TAG);
}

void wt_title_fit(lv_obj_t *scr, int w)
{
    lv_obj_t *cap = wt_tagged(scr, WT_TITLE_TAG);
    if (!cap) return;
    const char *txt = lv_label_get_text(cap);
    if (!txt || !*txt) return;

    // One line the whole way. A title is the one label that must not wrap:
    // wt_screen puts the subtitle 3px under its box, so a second line lands
    // on top of the subtitle rather than pushing it down. Measured unwrapped
    // (LV_COORD_MAX) so the answer is the real width the words need, not the
    // widest line of a wrap that already went wrong.
    //
    // TWO ladders, picked by what the title already wears: a chrome head is
    // mono and steps 28 -> 23 -> 18 inside its own family, because a fit
    // that reached for the sans faces would quietly undo the contract on
    // whichever locale happened to be long. The sans ladder is 34 -> 28 ->
    // 23, as it has always been.
    //
    // The tracking shrinks with the size for the same reason it always has
    // on fitted labels:
    // 3px between letters is presence at the top rung and lost width below.
    static const int space[3] = { 3, 2, 2 };
    const lv_font_t *cur = lv_obj_get_style_text_font(cap, 0);
    const bool mono = cur == wt_font_mono28() || cur == wt_font_mono23() ||
                      cur == wt_font_mono18();
    const lv_font_t *f[3];
    if (mono) {
        f[0] = wt_font_mono28(); f[1] = wt_font_mono23();
        f[2] = wt_font_mono18();
    } else {
        f[0] = wt_font34(); f[1] = wt_font28(); f[2] = wt_font23();
    }
    int pick = 2;
    for (int i = 0; i < 3; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, txt, f[i], space[i], 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (sz.x <= w) { pick = i; break; }
    }
    lv_obj_set_style_text_font(cap, f[pick], 0);
    lv_obj_set_style_text_letter_space(cap, space[pick], 0);
}

void wt_sub_fit(lv_obj_t *scr, int w)
{
    lv_obj_t *s = wt_tagged(scr, WT_SUB_TAG);
    if (!s) return;
    const char *txt = lv_label_get_text(s);
    if (!txt) return;
    // Re-fit as well as re-width: the subtitle gets ONE line inside a 29px
    // budget, and narrowing the lane without re-measuring would simply wrap it
    // onto the y=96 content line.
    lv_obj_set_style_text_font(s, note_font(txt, w, 29), 0);
    lv_obj_set_width(s, w);
}

// ---- the action bar (see kiss_theme.h) ----
// Built on demand by the first control placed on the action row (the arrow
// actions and the slide rule call it), so it exists exactly on the screens that have an
// action row and never has to be remembered.
//
// Created ONCE and never re-raised. LVGL paints in tree order, so the bar lands
// above everything built before the row's first control and below every control
// built after it, which is the stacking this wants. Raising it again on the
// second control would put it over the first one. It also means anything a screen
// deliberately draws INSIDE the band after its buttons, such as the build
// identity line along the bottom edge of Settings, still draws on top of the
// bar rather than being swallowed by it.
//
// `top` is where the band starts. Every screen but one passes
// WT_CONTENT_BOTTOM; the slide's band grows upward to WT_ACTION_Y_SLIDE
// because a 44px knob does not fit 52px. Whichever control builds the bar
// picks the height, and there is only ever one bar, so a screen carrying a
// band slide has no other control that could ask for the short one first --
// the slide is always built before the exit beside it.
static void action_bar_ensure_at(lv_obj_t *scr, int top)
{
    if (!scr || lv_obj_get_user_data(scr) != (void *)WT_SCREEN_TAG) return;

    // A bar already there is either the right height or too short. Too short
    // happens whenever the screen's SHAPE built the band before its slide did
    // -- wt_gate does, and the two words gates are exactly that screen -- so
    // the bar GROWS rather than the slide hanging above it in content. It only
    // ever grows: nothing shrinks a band back under a control already on it.
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        const void *tag = lv_obj_get_user_data(c);
        if (tag != (void *)WT_BAR_TAG && tag != (void *)WT_BARCAP_TAG) continue;
        // THE STYLE PROPERTY. lv_obj_get_y reads obj->coords, which no layout
        // pass has filled yet on a bar this same build just created -- it
        // answered 0, "0 <= top" is true for every top, and the band never
        // grew for anyone. A zero falling through an if, which is the class
        // check_layout_reads.py exists for and the reason it now watches x
        // and y as well.
        if (lv_obj_get_style_y(c, LV_PART_MAIN) <= top) return;
        lv_obj_set_y(c, top);
        if (tag == (void *)WT_BAR_TAG) lv_obj_set_height(c, 471 - top);
    }
    for (uint32_t i = 0; i < n; i++)
        if (lv_obj_get_user_data(lv_obj_get_child(scr, i)) == (void *)WT_BAR_TAG)
            return;

    // Inset to the card screen_card draws, not the full panel width: the row is
    // the bottom of one surface, so its hairline has to stop where that surface
    // stops. 9 and 782 sit one pixel inside the card's 8..792 border, and 73
    // takes the fill down to the card's inner bottom edge at 471.
    //
    // Rounded to WT_CARD_R - 1. The card's border is 1px, so the hole it encloses
    // has radius 15 and its bottom corners curve from (9,455) to (24,470); a
    // square fill covering that band paints a solid shoulder OVER the arc, which
    // is why the screen border used to run down both sides, stop dead, and pick
    // up again 11px along the bottom. The corner was never missing, it was
    // buried. Matching the hole's radius exactly puts the fill inside the curve
    // instead of across it.
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_user_data(bar, (void *)WT_BAR_TAG);
    lv_obj_set_pos(bar, 9, top);
    lv_obj_set_size(bar, 782, 471 - top);
    lv_obj_set_style_radius(bar, WT_CARD_R - 1, 0);
    lv_obj_set_style_bg_color(bar, WT_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Not clickable and not scrollable: it is a surface, and a tap that misses a
    // button must fall through to whatever is behind rather than being eaten.
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // A radius rounds all four corners, and the bar's TOP corners are in open
    // content, not on the card edge -- rounding them bites two notches of page
    // background out of the band and bends the ends of its hairline. So the top
    // rung of the fill is squared off by a second slab drawn over it, which also
    // carries the hairline. Two flat objects rather than clip_corner on the card:
    // clip_corner refreshes the children into an ARGB8888 layer 784px wide, and
    // this panel's draw pool cannot hand out a buffer that size -- lv_draw_dispatch
    // would retry the allocation forever, the same hang transform_scale causes.
    lv_obj_t *cap = lv_obj_create(scr);
    lv_obj_remove_style_all(cap);
    lv_obj_set_user_data(cap, (void *)WT_BARCAP_TAG);
    lv_obj_set_pos(cap, 9, top);
    lv_obj_set_size(cap, 782, WT_CARD_R - 1);
    lv_obj_set_style_bg_color(cap, WT_BAR, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cap, WT_HAIR, 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_side(cap, LV_BORDER_SIDE_TOP, 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
}

static void action_bar_ensure(lv_obj_t *scr)
{
    action_bar_ensure_at(scr, WT_CONTENT_BOTTOM);
}

bool wt_is_slide_band(lv_obj_t *o)
{
    return o && lv_obj_get_user_data(o) == (void *)WT_SLIDEBAND_TAG;
}

// ---- tap feedback ----
// The device has no haptics, so a press can only be answered optically, and
// until now the whole answer was the background changing colour instantly.
//
// Two more style properties carry the rest. The control translates 2px down
// while held, so it reads as pushed in; and an outline ring travels between its
// edge and 10px outside it, opaque at the pressed end and invisible at the
// resting end. Pressing pulls the ring in as it appears, releasing pushes it
// back out as it fades -- an optical tick at each end of the tap.
//
// The ring needs no press/release handlers because BOTH ends are ordinary
// states: the resting state simply owns the wide invisible outline and the
// pressed state owns the narrow opaque one, and the transition below animates
// whichever direction the finger goes.
//
// Deliberately NOT transform_scale. A scaled object forces LVGL to allocate a
// draw layer, the pool on this panel cannot hold one that size, and
// lv_draw_dispatch then retries the allocation forever -- a hard hang, not a
// dropped frame. calculate_layer_type() in lv_obj_style.c triggers only on
// rotation, scale, skew, layered opacity, bitmap masks and blend modes; every
// property used here is a plain paint value with no layer behind it.
//
// One descriptor on the DEFAULT state rather than a fast-in/slow-out pair,
// because a transition is looked up on the state being entered: a descriptor
// living only on the pressed style would animate the press and then sit out
// the release, which is the half that matters most.
#define WT_TAP_RING 10          // how far outside the edge the ring travels
#define WT_TAP_MS   160

static lv_style_transition_dsc_t s_tap_tr;
static bool s_tap_tr_ready;

void wt_tap_feedback(lv_obj_t *p)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR, LV_STYLE_TRANSLATE_Y,
        LV_STYLE_OUTLINE_WIDTH, LV_STYLE_OUTLINE_OPA,
        LV_STYLE_PROP_INV                       // terminator
    };
    if (!s_tap_tr_ready) {
        lv_style_transition_dsc_init(&s_tap_tr, props, lv_anim_path_ease_out,
                                     WT_TAP_MS, 0, NULL);
        s_tap_tr_ready = true;
    }
    lv_obj_set_style_outline_color(p, wt_primary(), 0);
    lv_obj_set_style_outline_width(p, WT_TAP_RING, 0);
    lv_obj_set_style_outline_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(p, 0, 0);
    lv_obj_set_style_transition(p, &s_tap_tr, 0);

    lv_obj_set_style_outline_width(p, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_opa(p, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(p, 2, LV_STATE_PRESSED);
}

static lv_obj_t *round_chip(lv_obj_t *parent, const char *symbol,
                            int x, int y, lv_color_t color,
                            lv_event_cb_t cb, void *ud)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 30, 30);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_style_radius(chip, 15, 0);
    lv_obj_set_style_bg_color(chip, WT_KEY, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, color, 0);
    // A chip painted in the accent has to say so on BOTH channels, or the rim
    // and the glyph drift apart at the next theme change. Guarded on the
    // colour rather than a parameter: callers pass a status colour here too,
    // and a status colour must never be repainted by a theme.
    const bool acc = lv_color_eq(color, wt_accent());
    if (acc) lv_obj_add_flag(chip, WT_FLAG_ACCENT_BORDER);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(chip, 12);       // 54px effective target
    wt_tap_feedback(chip);
    if (cb) lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, color, 0);
    if (acc) lv_obj_add_flag(label, WT_FLAG_ACCENT);
    // font21 in a 30px ring, not font14. The LADDER gate found this one: a
    // "?" four rungs under the NEVER CHECKED it sits beside is a speck with a
    // circle drawn round it, and the circle was doing all the work. 21 still
    // clears the rim by 4px each side.
    //
    // GUARDED, not mono21 flat. The mono faces are ASCII and two of this
    // function's three callers hand it an LV_SYMBOL: the QR card's zoom cue
    // and the zoom overlay's close button were both drawing LVGL's missing
    // glyph box -- a hollow rectangle inside a ring, on every screen with a QR
    // on it, on the device and in every captured frame identically. The "?"
    // is ASCII and keeps mono21; a symbol takes the sans face that carries it.
    lv_obj_set_style_text_font(label, wt_chrome21(symbol), 0);
    lv_obj_center(label);
    return chip;
}

lv_obj_t *wt_help_chip(lv_obj_t *parent, int x, int y, lv_color_t color,
                       lv_event_cb_t cb, void *ud)
{
    return round_chip(parent, "?", x, y, color, cb, ud);
}

// Two spaces, not one: at the labels' tracking a single space let the icon
// crowd the first letter and the pair read as one damaged glyph.
void wt_icon_text(char *out, size_t out_len, const char *icon, const char *txt)
{
    snprintf(out, out_len, "%s  %s", icon, txt);
}

// ---- slide to confirm (see kiss_theme.h) ----
// The successor to hold-to-confirm, at the bench's own request: press the
// bar and DRAG right, the fill following the finger's travel; reaching the
// far end fires, letting go earlier runs the fill back. Distance, not
// position -- the press can land anywhere on the bar, and the travel it
// takes to fire is always the bar's own width, so a stray brush against the
// right edge cannot complete anything. No timer: the finger is the clock.
// State hangs off the bar so several could coexist, freed on DELETE, so a
// screen torn down mid-slide leaves nothing.
// The gesture's own numbers, all of them measured against the 44px knob the
// bench asked for and the 127px band that had to grow to hold it.
//
//   band  344..471         the bar, WT_ACTION_Y_SLIDE
//   cap   344..359         the band's squared top rung and its hairline
//   label 364..389         mono23 at ls 2
//   knob  410..454         44px, centred on the track
//   track 430..434         4px, the full width of the control
//
// ONE line of words, not two. The window's offer was drafted as a second line
// under the label and there is no room for one: stacked it collides with the
// knob, and beside it -- on a 330px control -- it collides with the label. It
// is also the wrong moment. A finger already travelling does not need to be
// told a lift is safe; a finger that has just LIFTED does, and by then the
// label is free to say it. So the label carries the whole gesture: the word at
// rest, KEEP SLIDING under the finger, KEEP GOING in the window.
#define WT_SLIDE_KNOB      44
#define WT_SLIDE_TRACK      4
#define WT_SLIDE_LBL_Y     20
#define WT_SLIDE_TRACK_Y   86
// 8px in either direction before the knob moves at all. A brush along the band
// while reaching for the exit is not a gesture, and it must not light a fill
// that then has to be seen retracting.
#define WT_SLIDE_DEAD       8
// Past 85% the knob finishes the trip itself. The far end of a 704px lane is
// where a thumb runs out of reach, and a gesture only a large hand can finish
// is a gesture asking the wrong question.
#define WT_SLIDE_SNAP      85
#define WT_SLIDE_SNAP_MS  120
// A lift short of the end banks the travel for this long. Long enough to
// change grip, short enough that a slide left behind on the screen does not
// still be armed when the next person picks the device up.
#define WT_SLIDE_PAUSE_MS 800

typedef struct {
    lv_obj_t *fill;
    // The KNOB: 44px of it, on the track, at the fill's leading edge. It was
    // 16 and it was the bench's loudest note twice -- once for not existing at
    // all ("THERE SHOULD BE A SQUARE DOT AT BEGINNING OF LINE UNDERNEATH TO
    // SLIDE THAT ALL THE WAY OVER"), and once, after it did, for being too
    // small to put a thumb on. 44 is the size a thumb lands on without aiming.
    lv_obj_t *knob;
    // The lock, inside the knob, hidden until the gesture arrives. The knob is
    // the only thing on the control that has moved, so it is the only thing
    // whose change the eye is already on.
    lv_obj_t *mark;
    // The first leg's fill, left behind under the second one at 40% so the
    // track says how much of a two leg gesture has been spent.
    lv_obj_t *spent;
    lv_obj_t *lbl;
    int w;                // the control's width
    int travel;           // w - WT_SLIDE_KNOB: the knob ends flush, not past
    int x0;               // the press's screen x; travel measures from here
    int base;             // travel banked by a paused gesture, added to this one
    int at;               // the drag's current travel, for the release test
    bool fired;           // done() ran -- ignore every later event
    bool armed;           // snapped home; further PRESSING is ignored
    bool saying_held;     // the KEEP SLIDING swap, made once, not per event
    bool band;            // the tall shape: a word over a 44px knob
    bool twice;           // two legs, out and back: the erase gate only
    int  pass;            // 0 = out, 1 = back
    const char *again;    // the word between the legs
    bool live;            // this press was accepted; PRESSING may drive
    const char *txt, *held;
    int release_ms;
    lv_timer_t *pause;    // the 800ms window a lift opens, NULL when closed
    uint32_t born;        // when the bar was built, for deaf_ms
    uint32_t deaf_ms;
    void (*move)(int per255, void *ud);
    void (*done)(void *);
    void *ud;
} wt_hold_t;

static void an_w(void *v, int32_t w) { lv_obj_set_width(v, w); }

// The fill AND the knob, from one number, so the run-back animation carries
// both. The animation's var is the CONTEXT rather than the fill, which means
// LVGL's object destructor will not reap it -- hold_reset deletes it by hand
// on the DELETE path, which it already did for the fill.
//
// The fill ends under the knob's MIDDLE, not at its leading edge: a bar ending
// short of the knob reads as the knob outrunning its own fill.
static void an_slide(void *v, int32_t at)
{
    wt_hold_t *h = v;
    const int len = (int)at + WT_SLIDE_KNOB / 2;
    if (h->pass == 0) {
        if (h->fill) { lv_obj_set_x(h->fill, 0); lv_obj_set_width(h->fill, len); }
        if (h->knob) lv_obj_set_x(h->knob, (int)at);
    } else {
        // The return leg. Same number, mirrored: the knob comes back from the
        // far end and the fill grows inward from the right edge, so the track
        // reads as being closed from both sides.
        if (h->fill) {
            lv_obj_set_width(h->fill, len);
            lv_obj_set_x(h->fill, h->w - len);
        }
        if (h->knob) lv_obj_set_x(h->knob, h->travel - (int)at);
    }
    // Reported from HERE and not from the press handler, so the run back and
    // the snap carry whatever the caller hung off the travel just as the drag
    // does. A graph that only follows the finger forward is a graph that stays
    // committed after the gesture was abandoned.
    if (h->move) h->move((int)(at * 255 / (h->travel > 0 ? h->travel : 1)),
                         h->ud);
}

// The label and its arrow are two objects, not one formatted string. The walk
// finds a control by the WORDS on it -- exactly, or as the "icon  LABEL" form
// wt_icon_text composes -- so a trailing arrow baked into the text makes the
// control unfindable, and every hold on this screen would silently do nothing.
// wt_arrow_action splits them for the same reason.
static void hold_rule_say(wt_hold_t *h, const char *txt)
{
    if (h->lbl && txt) lv_label_set_text(h->lbl, txt);
}

static void hold_pause_close(wt_hold_t *h)
{
    if (!h->pause) return;
    lv_timer_delete(h->pause);
    h->pause = NULL;
}

// `animate` is false on the two paths where the object is going away or the
// screen is being replaced under it -- DELETE, and the tick that fires done().
// Starting an animation on either is the use-after-free this whole idiom has
// to avoid, and neither would ever be seen.
// The end of a run back. Only now is the pass reset: the retraction animates
// along the leg it is undoing, and flipping the mapping halfway would teleport
// the knob across the track.
static void hold_home(wt_hold_t *h)
{
    h->pass = 0;
    if (h->spent) lv_obj_add_flag(h->spent, LV_OBJ_FLAG_HIDDEN);
    an_slide(h, 0);
}

static void hold_home_cb(lv_anim_t *a) { hold_home(a->var); }

static void hold_reset(wt_hold_t *h, bool animate)
{
    hold_pause_close(h);
    h->saying_held = false;
    h->armed = false;
    h->base = 0;
    h->at = 0;
    if (h->lbl && h->txt) hold_rule_say(h, h->txt);
    if (h->mark) lv_obj_add_flag(h->mark, LV_OBJ_FLAG_HIDDEN);
    if (!h->fill) return;
    lv_anim_delete(h, an_slide);
    // How far along the CURRENT leg, which on the return leg is the knob's
    // distance from the far end rather than from zero.
    const int kx = lv_obj_get_x(h->knob);
    int32_t at = h->pass == 0 ? kx : h->travel - kx;
    if (!animate || h->release_ms <= 0 || at <= 0) {
        hold_home(h);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, h);
    lv_anim_set_exec_cb(&a, an_slide);
    lv_anim_set_values(&a, at, 0);
    lv_anim_set_duration(&a, h->release_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, hold_home_cb);
    lv_anim_start(&a);
}

// The window ran out with no finger back on the track. Only now does the fill
// run home.
static void hold_pause_expire(lv_timer_t *t)
{
    wt_hold_t *h = lv_timer_get_user_data(t);
    h->pause = NULL;                  // the timer deletes itself after this run
    hold_reset(h, true);
}

// Past the snap point the gesture finishes itself. The last stretch of a slide
// is where a thumb runs out of glass and out of reach, and a gesture that can
// be completed only by a hand large enough for the panel is a gesture that
// asks the wrong question.
static void hold_snap(wt_hold_t *h)
{
    h->armed = true;
    const int32_t from = lv_obj_get_x(h->knob);
    h->at = h->travel;
    lv_anim_delete(h, an_slide);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, h);
    lv_anim_set_exec_cb(&a, an_slide);
    lv_anim_set_values(&a, from, h->travel);
    lv_anim_set_duration(&a, WT_SLIDE_SNAP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    if (h->mark) lv_obj_remove_flag(h->mark, LV_OBJ_FLAG_HIDDEN);
}

static void hold_press_cb(lv_event_t *e)
{
    wt_hold_t *h = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        // Not yet listening: this press is the tail of the one that built the
        // bar, landing on the control that replaced what it hit.
        if (h->deaf_ms && lv_tick_elaps(h->born) < h->deaf_ms) return;
        h->live = true;
        lv_indev_t *in = lv_indev_active();
        lv_point_t pt = { 0, 0 };
        if (in) lv_indev_get_point(in, &pt);
        h->x0 = pt.x;
        // A press inside the pause window RESUMES: whatever the last drag
        // reached is banked and this one adds to it. Anywhere on the track --
        // the owner is re-gripping, not aiming.
        h->base = h->pause ? h->at : 0;
        hold_pause_close(h);
        h->at = h->base;
        h->fired = false;
        lv_anim_delete(h, an_slide);
    } else if (c == LV_EVENT_PRESSING) {
        if (!h->live || h->fired || h->armed) return;
        lv_indev_t *in = lv_indev_active();
        if (!in) return;
        lv_point_t pt;
        lv_indev_get_point(in, &pt);
        // THE DEADBAND. Under 8px in either direction the knob does not move
        // at all, so a brush along the band lights nothing and has nothing to
        // retract. Past it the travel counts from the edge of the band rather
        // than from the press, so the knob starts moving smoothly instead of
        // jumping the 8px it owed.
        // Out on the first leg, BACK on the second. One number either way:
        // the travel is always how far this finger has come along its own leg.
        int raw = h->pass == 0 ? pt.x - h->x0 : h->x0 - pt.x;
        if (raw > WT_SLIDE_DEAD)       raw -= WT_SLIDE_DEAD;
        else if (raw < -WT_SLIDE_DEAD) raw += WT_SLIDE_DEAD;
        else                           raw = 0;
        int px = h->base + raw;
        if (px < 0) px = 0;
        if (px > h->travel) px = h->travel;
        h->at = px;
        an_slide(h, px);
        // The swap happens once the slide is clearly a slide, and swaps back
        // if the finger retreats -- the words track the gesture, not the tap.
        const bool committed = px > h->travel / 6;
        if (h->lbl && h->held && committed != h->saying_held) {
            h->saying_held = committed;
            hold_rule_say(h, committed ? h->held : h->txt);
        }
        if (px >= h->travel * WT_SLIDE_SNAP / 100) hold_snap(h);
    } else {                           // RELEASED, PRESS_LOST, or DELETE
        const bool was_live = h->live;
        h->live = false;
        if (!was_live && c != LV_EVENT_DELETE) return;
        // Full travel ARMS; the LIFT fires. Firing mid-drag replaced the
        // screen under a finger still down, and the indev re-targets a live
        // press -- so the drag's tail pressed whatever the new screen put
        // there, on the glass exactly as in the walk. PRESS_LOST never
        // fires: a press the system took away is not a decision.
        // The first leg ARRIVES rather than fires. The knob stays at the far
        // end, its fill goes dim and stays on the track as the record of what
        // has been spent, and the word asks for the other half.
        if (c == LV_EVENT_RELEASED && !h->fired && h->armed &&
            h->twice && h->pass == 0) {
            // The snap that armed this leg is still animating, and its last
            // tick would be re-read through the RETURN leg's mapping -- which
            // put the knob back at zero, the one place it must not be.
            lv_anim_delete(h, an_slide);
            h->pass = 1;
            h->armed = false;
            h->at = h->base = 0;
            h->saying_held = false;
            if (h->spent) lv_obj_remove_flag(h->spent, LV_OBJ_FLAG_HIDDEN);
            if (h->mark)  lv_obj_add_flag(h->mark, LV_OBJ_FLAG_HIDDEN);
            hold_rule_say(h, h->again ? h->again : h->txt);
            an_slide(h, 0);
            return;
        }
        if (c == LV_EVENT_RELEASED && !h->fired && h->armed) {
            h->fired = true;
            void (*done)(void *) = h->done;
            void *ud = h->ud;
            // SETTLE, not reset. The control STAYS as the gesture left it --
            // fill full, lock in the knob -- because that mark is the whole
            // answer to the bench's complaint that the reward for finishing
            // was the bar disappearing. Only the pause window is closed.
            hold_pause_close(h);
            lv_anim_delete(h, an_slide);
            if (done) done(ud);        // may delete the bar: touch nothing after
            return;
        }
        // A lift short of the end PAUSES rather than abandons. The fill stays
        // where the finger left it for 800ms and the label stops giving an
        // instruction and starts making an offer. An unsteady hand, a finger
        // that ran off the edge of the panel, a grip changed halfway: none of
        // those is a decision to cancel, and all three used to be treated as
        // one. PRESS_LOST is not offered the window -- a press the system took
        // away is not a hand that means to come back.
        if (!h->fired && c == LV_EVENT_RELEASED && h->at > 0 && h->band) {
            // KEEP GOING already ships in 21 locales as the passphrase
            // keyboard's encouragement, and it is the same sentence to the
            // same person: the thing you were doing is not lost.
            hold_rule_say(h, tr(STR_L_KEEP_GOING));
                    h->saying_held = false;
            hold_pause_close(h);
            h->pause = lv_timer_create(hold_pause_expire, WT_SLIDE_PAUSE_MS, h);
            if (h->pause) {
                lv_timer_set_repeat_count(h->pause, 1);
                return;
            }
        }
        if (!h->fired) hold_reset(h, c != LV_EVENT_DELETE);
        if (c == LV_EVENT_DELETE) {
            hold_pause_close(h);
            lv_anim_delete(h, an_slide);
            lv_free(h);
        }
    }
}

// One builder for both faces of the slide bar: `ink`/`fill` NULL means the
// accent, flagged so the theme repaints it; a stated colour means a DANGER,
// which never restyles, so the flags stay off.
//
// Two shapes, one gesture. A slide on the ACTION BAND gets the tall band, its
// word above the track and a line under that; a slide inside a ROW has room
// for neither, so its mark rides inside the knob and the knob is the whole
// control. Both have the same 44px of thumb and the same travel arithmetic --
// what changes is where the words go, and the row shape has none.
lv_obj_t *wt_slide(lv_obj_t *scr, const wt_slide_t *cfg)
{
    const char *txt = cfg->txt, *held = cfg->held;
    const int x = cfg->x, w = cfg->w;
    int y = cfg->y;
    const lv_color_t *ink = cfg->ink, *fill = cfg->fill;
    const bool inert = cfg->inert;
    // A y anywhere in the action band means the band shape, and the band shape
    // has ONE geometry: 344. The test is against WT_ACTION_Y_SLIDE and not
    // WT_CONTENT_BOTTOM, because 344 is ABOVE 398 -- a call site that already
    // says WT_ACTION_Y_SLIDE has to pass, and so does one still saying
    // WT_ACTION_Y, which gets the right control rather than a 44px knob
    // hanging out of a 52px row.
    const bool band = (y >= WT_ACTION_Y_SLIDE);
    int bh = WT_ACTION_H;
    if (band) {
        y = WT_ACTION_Y_SLIDE;
        bh = 471 - WT_ACTION_Y_SLIDE;
        action_bar_ensure_at(scr, WT_ACTION_Y_SLIDE);
    }

    wt_hold_t *h = lv_malloc(sizeof *h);
    if (!h) return NULL;
    lv_memzero(h, sizeof *h);
    h->w = w;
    h->band = band;
    h->travel = w - WT_SLIDE_KNOB;
    if (h->travel < 1) h->travel = 1;
    h->txt = txt;
    h->held = held;
    // 200, once, for every hold on a rule. The firmware handoff wrote 180 and
    // the screen system wrote 200 for the same gesture; a 20ms disagreement
    // is not two designs, it is one number written twice, and the system pass
    // is the one that covers the whole device. What is load bearing is that
    // the fill RUNS back instead of vanishing -- a fill that disappears reads
    // as a completed action.
    h->release_ms = 200;
    h->born = lv_tick_get();
    h->deaf_ms = cfg->deaf_ms;
    h->move = cfg->move;
    h->twice = cfg->twice;
    h->again = cfg->again;
    h->done = cfg->done;
    h->ud = cfg->ud;

    // The hit box is the rule's whole width and the band's height. No fill, no
    // border, no radius: the control IS the label, the track and the knob, and
    // anything drawn around them would be the pill this replaces.
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, bh);
    lv_obj_set_pos(p, x, y);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    if (!inert) lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    // A slide IS a horizontal stroke, and some of these bars live on deck
    // pages whose screen watches for exactly that. The gesture must not
    // bubble out of the bar, or dragging the confirm would turn the page
    // under it.
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (band) lv_obj_set_user_data(p, (void *)WT_SLIDEBAND_TAG);

    const lv_color_t inkc  = inert ? WT_DIM  : (ink  ? *ink  : wt_accent());
    const lv_color_t fillc = inert ? WT_EDGE : (fill ? *fill : wt_accent());
    // Inert wears no accent flag anywhere, so a theme repaint cannot relight
    // a control that is not answering.
    const bool flagged = !inert && !fill;

    // The track and the fill first, so the knob draws over them.
    const int ty = band ? WT_SLIDE_TRACK_Y : (bh - WT_SLIDE_TRACK) / 2;
    lv_obj_t *track = lv_obj_create(p);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, w, WT_SLIDE_TRACK);
    lv_obj_set_pos(track, 0, ty);
    lv_obj_set_style_radius(track, WT_SLIDE_TRACK / 2, 0);
    lv_obj_set_style_bg_color(track, WT_DIV, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *f = lv_obj_create(p);
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, WT_SLIDE_KNOB / 2, WT_SLIDE_TRACK);
    lv_obj_set_pos(f, 0, ty);
    lv_obj_set_style_radius(f, WT_SLIDE_TRACK / 2, 0);
    lv_obj_set_style_bg_color(f, fillc, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    if (flagged) lv_obj_add_flag(f, WT_FLAG_ACCENT_FILL);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    h->fill = f;

    // The first leg's fill, drawn under the live one and hidden until there
    // IS a first leg to remember. Full width, 40%: spent, not active.
    if (cfg->twice) {
        lv_obj_t *sp = lv_obj_create(p);
        lv_obj_remove_style_all(sp);
        // As long as the LEG, not as long as the control: the fill never
        // reaches the last half knob of the track, so a full width bar would
        // claim travel the first leg did not have.
        lv_obj_set_size(sp, h->travel + WT_SLIDE_KNOB / 2, WT_SLIDE_TRACK);
        lv_obj_set_pos(sp, 0, ty);
        lv_obj_set_style_radius(sp, WT_SLIDE_TRACK / 2, 0);
        lv_obj_set_style_bg_color(sp, fillc, 0);
        lv_obj_set_style_bg_opa(sp, LV_OPA_40, 0);
        if (flagged) lv_obj_add_flag(sp, WT_FLAG_ACCENT_FILL);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(sp, LV_OBJ_FLAG_HIDDEN);
        h->spent = sp;
    }

    // The knob. Square with a 6px radius -- a circle would read as a lamp, and
    // every other round filled thing on this device is one. It is a SIBLING of
    // the fill, not a child: LVGL clips children to their parent, and a knob
    // inside a short fill would be clipped at exactly the moment it has to say
    // "start here".
    lv_obj_t *k = lv_obj_create(p);
    lv_obj_remove_style_all(k);
    lv_obj_set_size(k, WT_SLIDE_KNOB, WT_SLIDE_KNOB);
    lv_obj_set_pos(k, 0, ty + WT_SLIDE_TRACK / 2 - WT_SLIDE_KNOB / 2);
    lv_obj_set_style_radius(k, 6, 0);
    lv_obj_set_style_bg_color(k, fillc, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    if (flagged) lv_obj_add_flag(k, WT_FLAG_ACCENT_FILL);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    h->knob = k;

    // The label swaps between its word and KEEP SLIDING mid-drag, so the
    // mono rung is taken only when BOTH fit the face -- a font swap on a
    // held control would make the track jump under the finger.
    const lv_font_t *lf2 = (mono_can(txt) && mono_can(held))
                               ? wt_font_mono23() : wt_font23();
    if (band) {
        lv_obj_t *l = wt_lbl(p, txt, 0, WT_SLIDE_LBL_Y, lf2, inkc);
        lv_obj_set_user_data(l, (void *)WT_SLIDELBL_TAG);
        lv_obj_set_style_text_letter_space(l, 2, 0);
        if (!inert && !ink) lv_obj_add_flag(l, WT_FLAG_ACCENT);
        h->lbl = l;
    } else {
        // No room for a word beside a 44px knob in a 52px row, so the mark
        // rides ON the knob. It is the only slide on the device whose label is
        // a single glyph, which is why this shape can exist at all.
        lv_obj_t *l = wt_lbl(k, txt, 0, 0, lf2, inkc);
        lv_obj_center(l);
        h->lbl = l;
    }

    // The lock, waiting inside the knob for the snap. WT_BAR, so it reads as
    // cut out of the knob rather than printed on it.
    lv_obj_t *mk = wt_lbl(k, WT_ICON_LOCK, 0, 0, wt_font23(), WT_BAR);
    lv_obj_center(mk);
    lv_obj_add_flag(mk, LV_OBJ_FLAG_HIDDEN);
    h->mark = mk;

    // DELETE is wired even when inert: the context is malloc'd either way and
    // a screen torn down must free it.
    lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_DELETE, h);
    if (!inert) {
        lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_PRESSED, h);
        lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_PRESSING, h);
        lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_RELEASED, h);
        lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_PRESS_LOST, h);
    }
    return p;
}

lv_obj_t *wt_slide_label(lv_obj_t *bar)
{
    if (!bar) return NULL;
    const uint32_t n = lv_obj_get_child_count(bar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(bar, i);
        if (lv_obj_get_user_data(c) == (void *)WT_SLIDELBL_TAG) return c;
    }
    return NULL;
}

lv_obj_t *wt_slide_rule(lv_obj_t *scr, const char *txt, const char *held,
                        int x, int y, int w,
                        void (*done)(void *), void *ud)
{
    wt_slide_t s = { .txt = txt, .held = held, .x = x, .y = y, .w = w,
                     .done = done, .ud = ud };
    return wt_slide(scr, &s);
}

lv_obj_t *wt_slide_rule_c(lv_obj_t *scr, const char *txt, const char *held,
                          int x, int y, int w,
                          lv_color_t ink, lv_color_t fill,
                          void (*done)(void *), void *ud)
{
    wt_slide_t s = { .txt = txt, .held = held, .x = x, .y = y, .w = w,
                     .ink = &ink, .fill = &fill, .done = done, .ud = ud };
    return wt_slide(scr, &s);
}

// Paragraph text is drawn as SPANS, so the full stop of each sentence can
// carry the accent. Declared here because the side-note helpers below are the
// first users and the builder lives with the explainers.
static void spans_fill(lv_obj_t *sg, const char *txt, const char *hi);
static lv_obj_t *spans_new(lv_obj_t *par, int x, int y, int w);

lv_obj_t *wt_lbl(lv_obj_t *scr, const char *txt, int x, int y,
                 const lv_font_t *f, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Side note with a KNOWN vertical budget: picks the largest of the three fonts
// that fits, exactly like an explainer card. A blanket font bump here does not
// work -- these sit in gaps between other controls, so the size has to be
// derived from the gap, and short copy is what earns the big one.
lv_obj_t *wt_wraph(lv_obj_t *scr, const char *txt, int x, int y, int w, int h)
{
    lv_obj_t *l = spans_new(scr, x, y, w);
    wt_wrap_fit(l, txt, w, h);
    return l;
}

// Re-fit a wt_wraph label whose text changes after it is built (the settings
// chooser captions swap on every tap). The font has to be recomputed with the
// text: leaving the old one is how a longer translation silently overflows.
void wt_wrap_fit(lv_obj_t *l, const char *txt, int w, int h)
{
    if (!l) return;
    lv_obj_set_style_text_font(l, wt_body_font(txt, w, h), 0);
    spans_fill(l, txt, NULL);
}

// A note that BELONGS to a control: same auto-fit, capped at 23. Left uncapped,
// a three-word note under a button renders at 28 and ends up shouting louder
// than the button itself. Explainer cards keep the full ladder; these do not.
static const lv_font_t *note_font(const char *txt, int w, int max_h)
{
    if (!txt || !*txt) return wt_font23();
    lv_point_t sz;
    lv_text_get_size(&sz, txt, wt_font23(), 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y <= max_h) return wt_font23();
    WT_FIT_GAVE_UP("note", txt, w, max_h);
    return wt_font14();
}

void wt_note_fit(lv_obj_t *l, const char *txt, int w, int h)
{
    if (!l) return;
    lv_obj_set_style_text_font(l, note_font(txt, w, h), 0);
    spans_fill(l, txt, NULL);
}

lv_obj_t *wt_note(lv_obj_t *scr, const char *txt, int x, int y, int w, int h)
{
    lv_obj_t *l = spans_new(scr, x, y, w);
    wt_note_fit(l, txt, w, h);
    return l;
}

lv_obj_t *wt_wrap(lv_obj_t *scr, const char *txt, int x, int y, int w, int max_h)
{
    // The TEXT comes in, so the size can be chosen. It used to hard-code
    // font14 and return an empty label for the caller to fill, which is the
    // font14-is-a-bug shape with the ladder missing rather than overruled:
    // its three callers are the sentences that explain why an address did not
    // match, on the screen where an owner decides whether to trust one.
    lv_obj_t *l = spans_new(scr, x, y, w);
    lv_obj_set_style_text_font(l, wt_body_font(txt ? txt : "", w, max_h), 0);
    spans_fill(l, txt, NULL);
    return l;
}

// Column captions ("NETWORK", "WALLET"). Deliberately SMALL: these are
// eyebrows, not content. They were briefly font23 and it inverted the whole
// hierarchy, the label shouting while the value under it whispered. The size
// belongs to the thing you actually read.
//
// Small and WT_MUT together was the mistake: the caption lost twice, once on
// scale and again on contrast, and on the device panel ADDRESS TYPE was
// reported as barely visible. The desktop simulator never showed it, because a
// monitor renders #7A869C on #070A10 far more generously than the panel does.
// So the rank is carried by SIZE alone and the ink stays full strength. A
// caption you cannot read is not a subtle caption, it is a missing one.
lv_obj_t *wt_section(lv_obj_t *scr, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, WT_INK, 0);
    // mono18, the kit's own caption rung. A SECTION HEAD names the block under
    // it and this was font14 -- the size reserved for marks -- so on the
    // details deck the head sat smaller than every row it introduced.
    lv_obj_set_style_text_font(l, wt_font_mono18(), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *qr_card_raw(lv_obj_t *scr, lv_obj_t **qr,
                             int x, int y, int card_px, int qr_px)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_px, card_px);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, WT_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *q = lv_qrcode_create(card);
    if (q) {
        lv_qrcode_set_size(q, qr_px);
        lv_qrcode_set_dark_color(q, lv_color_hex(0x0B0E14));
        lv_qrcode_set_light_color(q, WT_CARD);
        lv_obj_remove_flag(q, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(q);
    }
    if (qr) *qr = q;
    return card;
}

typedef struct {
    lv_obj_t *card;
    lv_obj_t *qr;
    lv_obj_t *zoom;
    lv_obj_t *zoom_qr;
    uint8_t *data;
    uint32_t data_len;
    uint32_t data_cap;
} wt_qr_state_t;

// QR payloads are usually public, but the same component also renders the
// private Silent Payments scan key. Keep the cache generic without weakening
// key hygiene: volatile writes prevent the compiler from eliding the wipe.
static void qr_payload_zero(void *data, uint32_t len)
{
    volatile uint8_t *p = data;
    while (p && len--) *p++ = 0;
}

static void qr_payload_free(wt_qr_state_t *s)
{
    if (!s || !s->data) return;
    qr_payload_zero(s->data, s->data_cap);
    lv_free(s->data);
    s->data = NULL;
    s->data_len = 0;
    s->data_cap = 0;
}

static void qr_zoom_close_cb(lv_event_t *e)
{
    wt_qr_state_t *s = lv_event_get_user_data(e);
    if (!s || !s->zoom) return;
    lv_obj_t *zoom = s->zoom;
    s->zoom = NULL;
    s->zoom_qr = NULL;
    // Animated output updates only the enlarged QR while it is visible. Put
    // the latest cached frame back on the underlying card before returning.
    if (s->qr && s->data && s->data_len)
        lv_qrcode_update(s->qr, s->data, s->data_len);
    lv_obj_delete_async(zoom);
}

static void qr_zoom_open_cb(lv_event_t *e);

// The overlay itself, reachable without an event: RECEIVE's "TAP TO ENLARGE"
// line is a second way in, and lv_event_t is opaque outside LVGL's private
// header, so a synthesised event is not an option.
static void qr_zoom_open(wt_qr_state_t *s)
{
    if (!s || s->zoom || !s->data || !s->data_len) return;

    lv_obj_t *parent = lv_obj_get_parent(s->card);
    lv_obj_t *ovl = lv_obj_create(parent);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, WT_BG, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ovl, qr_zoom_close_cb, LV_EVENT_CLICKED, s);
    lv_obj_move_foreground(ovl);
    s->zoom = ovl;

    // 392px gives even a 117-character Silent Payment code materially larger
    // modules while leaving a full white quiet zone and a visible close target.
    lv_obj_t *zoom_card = qr_card_raw(ovl, &s->zoom_qr, 188, 28, 424, 392);
    lv_obj_add_flag(zoom_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zoom_card, qr_zoom_close_cb, LV_EVENT_CLICKED, s);
    if (s->zoom_qr)
        lv_qrcode_update(s->zoom_qr, s->data, s->data_len);
    round_chip(ovl, LV_SYMBOL_CLOSE, 748, 20, WT_MUT,
               qr_zoom_close_cb, s);
}

static void qr_state_delete_cb(lv_event_t *e)
{
    wt_qr_state_t *s = lv_event_get_user_data(e);
    if (!s) return;
    qr_payload_free(s);
    lv_free(s);
}

// A QR whose payload could not be derived must VANISH, not encode the failure.
// Five screens fed tr(STR_C_SESSION_LOCKED) straight into wt_qr_update when a
// locked session refused them, so the device showed a scannable code whose
// content was the words "SESSION LOCKED" -- on RECEIVE that is the square a
// sender is invited to scan as a payment address. Hiding the CARD is the
// contract: a blank white card reads as a broken render, and the qr alone is
// not the tap target, its card is.
void wt_qr_refusal(lv_obj_t *qr, bool locked)
{
    if (!qr) return;
    lv_obj_t *card = lv_obj_get_parent(qr);
    if (!card) return;
    if (locked) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *wt_qr_card(lv_obj_t *scr, lv_obj_t **qr,
                     int x, int y, int card_px, int qr_px)
{
    lv_obj_t *q = NULL;
    lv_obj_t *card = qr_card_raw(scr, &q, x, y, card_px, qr_px);
    if (!q) {
        if (qr) *qr = NULL;
        return card;
    }

    wt_qr_state_t *s = lv_malloc_zeroed(sizeof *s);
    if (!s) {
        if (qr) *qr = q;
        return card;                         // QR still works; only zoom is absent
    }
    s->card = card;
    s->qr = q;
    lv_obj_set_user_data(q, s);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    wt_tap_feedback(card);
    lv_obj_add_event_cb(card, qr_zoom_open_cb, LV_EVENT_CLICKED, s);
    lv_obj_add_event_cb(card, qr_state_delete_cb, LV_EVENT_DELETE, s);
    // Outside the white card, so the cue never damages the QR quiet zone.
    round_chip(scr, LV_SYMBOL_PLUS, x - 36, y + 8, WT_MUT,
               qr_zoom_open_cb, s);

    if (qr) *qr = q;
    return card;
}

static void qr_zoom_open_cb(lv_event_t *e)
{
    qr_zoom_open(lv_event_get_user_data(e));
}

void wt_qr_zoom(lv_obj_t *qr)
{
    // The zoom, from a control that is not the card. RECEIVE's "TAP TO
    // ENLARGE" line is a second way into the same overlay, and the state the
    // opener needs is already hanging off the QR -- so this is one lookup, not
    // a second implementation of the overlay.
    if (!qr) return;
    wt_qr_state_t *s = lv_obj_get_user_data(qr);
    if (!s) return;
    qr_zoom_open(s);
}

lv_result_t wt_qr_update(lv_obj_t *qr, const void *data, uint32_t data_len)
{
    if (!qr) return LV_RESULT_INVALID;
    wt_qr_state_t *s = lv_obj_get_user_data(qr);
    if (!s) return lv_qrcode_update(qr, data, data_len);

    uint32_t old_len = s->data_len;
    if (data_len > s->data_cap) {
        uint32_t cap = (data_len + 127u) & ~127u;
        // Do not use realloc here: if it moves the block, the allocator frees
        // the old secret-bearing buffer before we have a chance to wipe it.
        uint8_t *next = lv_malloc(cap);
        if (!next) return lv_qrcode_update(qr, data, data_len);
        qr_payload_free(s);
        s->data = next;
        s->data_cap = cap;
        old_len = 0;
    }
    lv_memcpy(s->data, data, data_len);
    if (old_len > data_len)
        qr_payload_zero(s->data + data_len, old_len - data_len);
    s->data_len = data_len;

    // The underlying card is completely covered while zoomed. Updating just
    // the visible QR avoids encoding every animated fragment twice.
    return lv_qrcode_update(s->zoom_qr ? s->zoom_qr : qr, data, data_len);
}

// The bitmap, not just the cache. lv_qrcode draws into an I1 lv_draw_buf and
// lv_draw_buf_clear is what lv_qrcode_update itself uses before re-encoding,
// so this leaves the object in the state it has before any payload -- valid to
// draw, holding nothing. The zoom's own qr is a second encoding of the same
// secret and is cleared with it.
void wt_qr_scrub(lv_obj_t *qr)
{
    if (!qr) return;
    wt_qr_state_t *s = lv_obj_get_user_data(qr);
    if (s) {
        qr_payload_free(s);
        if (s->zoom_qr) {
            lv_draw_buf_t *zb = lv_canvas_get_draw_buf(s->zoom_qr);
            if (zb) lv_draw_buf_clear(zb, NULL);
            lv_obj_invalidate(s->zoom_qr);
        }
    }
    lv_draw_buf_t *b = lv_canvas_get_draw_buf(qr);
    if (b) lv_draw_buf_clear(b, NULL);
    lv_obj_invalidate(qr);
}

// Only the TAIL is lit. The first characters of a bech32 address are the human
// readable part and the witness version: every Native SegWit mainnet address
// starts bc1q and every testnet/signet one tb1q. Highlighting them taught
// people to compare a constant, which is worse than useless -- it feels like
// checking while confirming nothing, and an address swapped by malware matches
// there for free. The last 8 (two groups) carry real entropy and include the
// bech32 checksum, so any altered address differs in them.
#define ADDR_TAIL_CHARS 8

// A spangroup is clickable out of the box, and an address is text, not a
// button. Left alone it silently eats every press that lands on it: put one
// inside a tappable row and the row goes dead exactly where the address is
// printed -- which is the middle, and the first place a finger goes.
static void addr_spans_no_click(lv_obj_t *sg)
{
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
}

static lv_span_t *addr_span(lv_obj_t *sg, const char *txt, bool lit)
{
    lv_span_t *s = lv_spangroup_new_span(sg);
    lv_span_set_text(s, txt);
    // Lit groups stand out by brightness alone (accent/ink vs muted grey). No
    // underline: it read as a link or a spelling error on the address, and the
    // contrast already carries the distinction.
    lv_style_set_text_color(lv_span_get_style(s), lit ? wt_accent() : WT_MUT);
    return s;
}

// The same fold as wt_addr_short, as plain text: prefix, the four after it, an
// ellipsis, and the last twelve in three blocks. No lit spans, because this is
// for places that take a STRING and not an object -- a row's sub-line -- and
// because the lighting means "compare these", which is a job that belongs on
// RECEIVE where the whole address is on the glass beside its QR.
void wt_addr_fold(const char *addr, char *out, size_t len)
{
    size_t n = addr ? strlen(addr) : 0;
    if (!out || !len) return;
    if (n < 20) { snprintf(out, len, "%s", addr ? addr : ""); return; }
    int pre = !strncmp(addr, "tsp1", 4) ? 5
            : (!strncmp(addr, "bc1", 3) || !strncmp(addr, "tb1", 3) ||
               !strncmp(addr, "sp1", 3)) ? 4 : 0;
    const char *t = addr + n - 12;
    snprintf(out, len, "%.*s %.4s \xE2\x80\xA6 %.4s %.4s %.4s",
             pre, addr, addr + pre, t, t + 4, t + 8);
}

const char *wt_name_fold(const char *name, const lv_font_t *f, int lane,
                         char *out, size_t len)
{
    if (!out || !len) return "";
    if (!name || !*name) { out[0] = 0; return out; }
    lv_point_t sz;
    lv_text_get_size(&sz, name, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= lane) { snprintf(out, len, "%s", name); return out; }

    // Give away from the MIDDLE, one character at a time from the head, until
    // it fits. The tail is what distinguishes, so it is the last thing cut:
    // head shrinks to nothing before the tail gives up a character.
    const size_t n = strlen(name);
    size_t tail = n / 2 > 10 ? 10 : n / 2;
    for (;;) {
        for (size_t head = n - tail; head > 0; head--) {
            snprintf(out, len, "%.*s\xE2\x80\xA6%s",
                     (int)head, name, name + n - tail);
            lv_text_get_size(&sz, out, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x <= lane) return out;
        }
        if (tail <= 3) break;
        tail--;                       // nothing fits with this tail; shorten it
    }
    // A lane too narrow for even three characters and an ellipsis. The caller
    // decides what to do with a name it cannot show; this hands back the tail
    // rather than a string of dots.
    snprintf(out, len, "%s", name + n - 3);
    return out;
}

lv_obj_t *wt_addr_short(lv_obj_t *par, const char *addr, const lv_font_t *f)
{
    size_t n = strlen(addr);
    // Not an address at all -- a locked-session message, an error string. Show
    // it as plain words rather than slicing arbitrary text into fake blocks.
    if (n < 20)
        return wt_lbl(par, addr, 0, 0, f, WT_MUT);

    // bech32 opens with a constant prefix through the first data character:
    // bc1q/tb1q for SegWit and sp1q/tsp1q for silent payments. It is shown as
    // its own block so the address still reads as one, but it is not marked and
    // neither is the block after it.
    //
    // This line used to light the four after the prefix AND the last four,
    // while wt_addr_spans -- the full address, directly above it on the verify
    // and receive screens -- lights the last EIGHT. Two renderings of one
    // address, marking two different runs, under a caption that says "compare
    // these 8". The runs overlapped in only their final four, so an owner who
    // learned this line's rule was checking characters the line above left
    // grey, and vice versa.
    //
    // The last eight wins, for the reason written at ADDR_TAIL_CHARS: the tail
    // carries real entropy and the bech32 checksum, so any altered address
    // differs there, while a mark near the front is the part an attacker gets
    // to match cheaply. It is also the only rule that can be taught on a
    // MULTI-recipient panel, which draws no elided line at all -- so choosing
    // the other one would leave the rule unavailable exactly where there are
    // most addresses to get wrong.
    //
    // The rendered string is unchanged, character for character. Only which
    // span carries the accent moved.
    int pre = !strncmp(addr, "tsp1", 4) ? 5
            : (!strncmp(addr, "bc1", 3) || !strncmp(addr, "tb1", 3) ||
               !strncmp(addr, "sp1", 3)) ? 4 : 0;
    char head[8] = {0}, key[8] = {0}, mid[32] = {0}, tail[16] = {0};
    lv_memcpy(head, addr, (size_t)pre);
    lv_memcpy(key, addr + pre, 4);
    // Twelve from the end, in three blocks of four. Chunking from the RIGHT is
    // the point: 42 characters do not divide by four, so grouping from the left
    // would leave the final block short and the lit run would straddle a gap.
    // The first of the three stays grey; the last two ARE the eight.
    const char *t = addr + n - 12;
    snprintf(mid, sizeof mid, "  \xE2\x80\xA6  %.4s ", t);
    snprintf(tail, sizeof tail, "%.4s %.4s", t + 4, t + 8);

    lv_obj_t *sg = lv_spangroup_create(par);
    addr_spans_no_click(sg);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);   // one line, sized to fit
    lv_obj_set_style_text_font(sg, f, 0);
    if (pre) {
        char pfx[8];
        snprintf(pfx, sizeof pfx, "%s ", head);
        addr_span(sg, pfx, false);
    }
    addr_span(sg, key, false);
    addr_span(sg, mid, false);
    addr_span(sg, tail, true);
    // The same flag addr_spans sets, and for the same reason: a span carries
    // its own style, so accent_walk has a spangroup branch that repaints the
    // LAST one -- which is this tail by construction. Without the flag it was
    // skipped, and the folded addresses on the sign graph were the one accent
    // painted thing on the device a theme change left behind. In GREEN the
    // whole screen went green and "g3zy g3h8 ffkz" stayed MONO blue.
    lv_obj_add_flag(sg, WT_FLAG_ACCENT);
    lv_spangroup_refresh(sg);
    return sg;
}

static lv_obj_t *addr_spans(lv_obj_t *par, const char *grouped, int w,
                            const lv_font_t *f, bool lift)
{
    int len = (int)strlen(grouped);
    int t = 0, raw = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (grouped[i] != ' ' && ++raw == ADDR_TAIL_CHARS) { t = i; break; }
    }
    // Snap to a GROUP boundary. An address is rarely a multiple of 4, so a
    // fixed 8-character tail starts mid-group and the highlight breaks a block
    // in half -- which then wraps, orphaning two characters on their own line.
    // Whole groups only: still "the last few", but always readable as blocks.
    //
    // ONLY when there ARE groups. Every caller passed a wt_group4 string until
    // the verify screen passed a raw address -- deliberately, because grouped it
    // measures 437px against a 438px box and wraps onto the line the comparison
    // belongs on. With no space to stop at, this walk ran t down to 0: the muted
    // head became empty and the accent span became the WHOLE address, so the one
    // screen that asks you to compare the last eight characters drew all
    // forty-two in one flat colour with nothing marked at all. Inverted, not
    // missing -- and with two or more recipients there is no second lit line
    // under it to fall back on.
    if (strchr(grouped, ' '))
        while (t > 0 && grouped[t - 1] != ' ') t--;
    char head[256];           // fits a grouped silent-payment addr (~146 chars)
    snprintf(head, sizeof head, "%.*s", t, grouped);

    lv_obj_t *sg = lv_spangroup_create(par);
    addr_spans_no_click(sg);
    lv_obj_set_width(sg, w);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, f, 0);
    lv_span_t *s1 = lv_spangroup_new_span(sg);
    lv_span_set_text(s1, head);
    lv_style_set_text_color(lv_span_get_style(s1), WT_MUT);
    lv_span_t *s2 = lv_spangroup_new_span(sg);
    lv_span_set_text(s2, grouped + t);
    lv_style_set_text_color(lv_span_get_style(s2), wt_accent());   // brightness, no underline
    // The tail is the part you are actually asked to compare, so where the body
    // is too small to compare comfortably the tail renders one rung ABOVE it.
    // Blowing up the whole string instead would push the other outputs off a
    // scrolling list -- this buys the legibility where it counts for 7px.
    //
    // Only at the 14 rung: at 23 and 28 the body already reads at arm's length
    // and the accent alone marks the tail, which is what the receive screen has
    // always done at 28.
    //
    // This used to test `f == wt_font14()` -- the PROPORTIONAL face -- while all
    // four callers pass a mono one, so the branch could not be taken by anything
    // and the tail never grew. Measured both ways at mono14: +7px, and the tail
    // stays on the same line in every box it is drawn in (438 and 722, a 42
    // character bech32 and a 117 character silent payment). It costs height, not
    // a wrap.
    //
    // And it is the CALLER's call, not a rule the font can carry. Whether the
    // tail must carry legibility on its own depends on what else is on the
    // screen: the single-recipient verify panel draws the compared runs again
    // beneath, blocked and at mono23, so lifting the body tail there renders the
    // same eight characters at the same size twice and pushed that panel into a
    // scrollbar with the caption at the fold. A multi-recipient list draws no
    // such line, so the body tail is the only marking there is, and at 14 it was
    // being carried by colour alone.
    if (lift) {
        if (f == wt_font_mono14())      lv_style_set_text_font(lv_span_get_style(s2), wt_font_mono23());
        else if (f == wt_font14())      lv_style_set_text_font(lv_span_get_style(s2), wt_font23());
    }
    // The last span is the lit one, and the flag is what makes accent_walk
    // repaint it when the theme moves. Every address on this device comes
    // through here, so this is the one place it needs saying.
    lv_obj_add_flag(sg, WT_FLAG_ACCENT);
    lv_spangroup_refresh(sg);
    return sg;
}

// The tail is marked by colour, at the body's own size. Use this wherever a
// larger copy of the compared run is drawn elsewhere on the screen.
lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f)
{
    return addr_spans(par, grouped, w, f, false);
}

// The tail is ALSO lifted a rung, for the screens where this line is the only
// place the compared run appears. A no-op above the 14 rung.
lv_obj_t *wt_addr_spans_lift(lv_obj_t *par, const char *grouped, int w,
                             const lv_font_t *f)
{
    return addr_spans(par, grouped, w, f, true);
}

// Section eyebrows carry the ACCENT. They were WT_MUT, which made the theme
// almost invisible outside the page title and one selected dot -- the owner
// could change it and struggle to tell.
//
// An eyebrow is the safest place on the device to spend the accent, and that
// matters because GREEN is byte identical to WT_OK and ORANGE is a near match
// for WT_WARN. Anywhere those can be confused, the accent has to stand aside:
// a green bordered ERASE reads as safe. An eyebrow names a GROUP, never a
// state, so it can never be mistaken for one -- and the one eyebrow that IS a
// state, NO UNDO, overrides this colour to STOP_COL right after the call and
// keeps doing so.
// Repaint everything wearing the accent under scr. The theme can be changed
// while a screen is up, and eyebrows and chevrons are built by shared helpers
// scattered across six files -- keeping a static list of them in every screen
// that has some is how they get missed. A flag on the object and one walk finds
// them wherever they were made.
//
// Colour only. The chevron's opacity is set once when it is built and has to
// survive this, or every theme change makes the arrows a little louder.
static void accent_walk(lv_obj_t *o)
{
    // A paragraph: only its full stops move. Checked BEFORE the flag below
    // because the two must never both be set on one object -- WT_FLAG_ACCENT
    // paints the group's text colour, and a paragraph's ordinary runs inherit
    // exactly that.
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_STOPS)) {
        uint32_t sn = lv_spangroup_get_span_count(o);
        for (uint32_t i = 0; i < sn; i++) {
            lv_span_t *sp = lv_spangroup_get_child(o, (int32_t)i);
            const char *t = sp ? lv_span_get_text(sp) : NULL;
            // The stop spans are the ones spans_fill made, and it makes them
            // out of exactly these two strings.
            if (t && (strcmp(t, ". ") == 0 || strcmp(t, ".") == 0))
                lv_style_set_text_color(lv_span_get_style(sp), wt_accent());
        }
        if (sn) lv_spangroup_refresh(o);
    }
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT)) {
        // Text first and unconditionally, which is what this flag has always
        // done and what every label under it still needs. Then the two classes
        // that carry their ink somewhere else: setting a text colour on a line
        // is not wrong, it is simply invisible, and that is exactly how the
        // change strand wore a stale accent with the flag correctly set.
        lv_obj_set_style_text_color(o, wt_accent(), 0);
        if (lv_obj_check_type(o, &lv_line_class))
            lv_obj_set_style_line_color(o, wt_accent(), 0);
        else if (lv_obj_check_type(o, &lv_arc_class))
            lv_obj_set_style_arc_color(o, wt_accent(), LV_PART_INDICATOR);
        else if (lv_obj_check_type(o, &lv_spangroup_class)) {
            // A span carries its own style and a text colour on the group is
            // invisible, exactly like the line above. Every lit address on
            // this device is built the same way -- a grey head and the last
            // eight as the FINAL span -- so the last span is the accented one
            // by construction, and repainting it is the whole job. Without
            // this the address tails were the one accent-painted thing on the
            // device that a theme change left behind, on the screens that show
            // the most of them.
            uint32_t sn = lv_spangroup_get_span_count(o);
            if (sn) {
                lv_span_t *last = lv_spangroup_get_child(o, (int32_t)sn - 1);
                if (last) {
                    lv_style_set_text_color(lv_span_get_style(last),
                                            wt_accent());
                    lv_spangroup_refresh(o);
                }
            }
        }
    }
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_BORDER))
        lv_obj_set_style_border_color(o, wt_accent(), 0);
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_BG)) {
        lv_obj_set_style_bg_color(o, wt_accent_bg(), 0);
        lv_obj_set_style_bg_color(o, wt_accent_pressed(), LV_STATE_PRESSED);
    }
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_FILL))
        lv_obj_set_style_bg_color(o, wt_accent(), 0);
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_SCROLL))
        lv_obj_set_style_bg_color(o, wt_accent(), LV_PART_SCROLLBAR);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) accent_walk(lv_obj_get_child(o, i));
}

void wt_accent_restyle(lv_obj_t *scr)
{
    if (scr) accent_walk(scr);
}

// ---- a tall row's sub-line lane, and the ONE size a list of them shares ----
//
// wt_row_x sizes a tall row's sub with the fit ladder, which picks the biggest
// rung that FITS. On a lone row that is right. On a LIST of rows it makes the
// type size a function of how long each string happens to be, so siblings on
// one screen land on different rungs: SET UP THIS SIGNER drew "12 new seed
// words, made on this signer" at 23 and "keys from seed words you already
// have" at 28, side by side in identical cards, and WHERE TO KEEP YOUR SEED
// WORDS drew two options at 23 and AMNESIC at 28. It reads as a mistake
// because it is one -- nothing about AMNESIC is more important, its sentence
// is merely shorter.
//
// The kit already had the answer for the pair of why-blocks next door:
// "a PAIR of blocks that must share one size", smaller rung wins. This is the
// same rule for rows, and the reason it is a KIT call rather than a note is
// that the caller cannot work out the lane -- it is what the icon, the chevron
// and the value leave behind, inside the row.
//
// So the arithmetic lives here ONCE and wt_row_x uses these too. A private
// copy in the sizer that drifted by a pixel would pick a rung the row it is
// sizing does not, which is worse than not sharing at all.
static int row_sub_lane_w(int w, bool icon, bool sel, bool cb, int vw)
{
    int right = w - 12;
    if (sel || cb) {
        lv_point_t cs;
        lv_text_get_size(&cs, sel ? LV_SYMBOL_OK : LV_SYMBOL_RIGHT,
                         sel ? wt_font23() : wt_font14(), 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        right = w - 10 - (int)cs.x - 10;
    }
    if (vw) right -= vw + 12;
    const int lx = icon ? 52 : 14;
    const int sw = right - lx;
    return sw > 40 ? sw : 40;
}

// The label is PINNED to one line at font23, so this needs no measuring.
static int row_sub_lane_h(int rowh)
{
    const int sh = rowh - (7 + lv_font_get_line_height(wt_font23()) + 3) - 12;
    return sh < 20 ? 20 : sh;
}

const lv_font_t *wt_row_sub_font(const char *const *subs, int n, int w, int h,
                                 bool icon, bool cb)
{
    const int sw = row_sub_lane_w(w, icon, false, cb, 0);
    const int sh = row_sub_lane_h(h);
    const lv_font_t *best = wt_font28();
    const char *worst = NULL;
    for (int i = 0; i < n; i++) {
        if (!subs[i] || !*subs[i]) continue;
        // The QUIET ladder in the loop, so a list of five does not file five
        // findings for one lane. The report is made once, below, against the
        // string that actually forced the rung down.
        const lv_font_t *f = body_font_ladder(subs[i], sw, sh, false);
        if (f == wt_font14()) { worst = subs[i]; best = wt_font14(); break; }
        if (f == wt_font23() && best == wt_font28()) { worst = subs[i]; best = wt_font23(); }
    }
    if (best == wt_font14() && worst) WT_FIT_GAVE_UP("row", worst, sw, sh);
    (void)worst;
    return best;
}

// Is this label a ROW's sub-line? The tag is private and has to stay that way
// -- it is compared by POINTER, so a copy of the string in another file would
// never match. The RAGGED gate needs to ask, so it asks here.
bool wt_is_row_sub(const lv_obj_t *o)
{
    return o && lv_obj_get_user_data((lv_obj_t *)o) == (void *)WT_SUB_TAG;
}

lv_obj_t *wt_row_head(lv_obj_t *scr, const char *txt, int x, int y, int w)
{
    lv_obj_t *h = wt_lbl(scr, txt, x, y, wt_font14(), wt_accent());
    lv_obj_add_flag(h, WT_FLAG_ACCENT);
    lv_obj_set_style_text_letter_space(h, 2, 0);
    lv_obj_set_width(h, w);
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    return h;
}

lv_obj_t *wt_row(lv_obj_t *scr, const char *label, const char *sub,
                 const char *val, lv_color_t vcol, int x, int y, int w,
                 lv_event_cb_t cb, void *ud)
{
    return wt_row_f(scr, label, sub, NULL, val, NULL, vcol, x, y, w, cb, ud);
}

lv_obj_t *wt_row_f(lv_obj_t *scr, const char *label, const char *sub,
                   const lv_font_t *sf, const char *val, const lv_font_t *vf,
                   lv_color_t vcol, int x, int y, int w,
                   lv_event_cb_t cb, void *ud)
{
    return wt_row_x(scr, NULL, label, sub, sf, val, vf, vcol, false,
                    x, y, w, 0, cb, ud);
}

lv_obj_t *wt_row_x(lv_obj_t *scr, const char *icon, const char *label,
                   const char *sub, const lv_font_t *sf,
                   const char *val, const lv_font_t *vf, lv_color_t vcol,
                   bool sel, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud)
{
    // A NULL sf means two different things depending on the height, so the
    // answer has to be taken before the default lands on it: on a standard row
    // it is font23, on a tall one it is "measure the box and pick".
    //
    // It said font14 until now, and that was the carve-out CLAUDE.md removed:
    // a sub-line is a SENTENCE -- "not real bitcoin", "opens your real keys"
    // -- and every teaching line on the settings page is one, so exempting
    // sub-lines exempted that page's entire body copy. The default moved and
    // the comment did not, which is enough to send the next reader looking for
    // a font14 slot that has not existed for some time. One did.
    const bool sf_auto = (sf == NULL);
    const int rowh = h > 0 ? h : WT_ROW_H;
    if (!sf) sf = wt_font23();
    if (!vf) vf = wt_font23();
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, w, rowh);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    // A CARD, not a hairline-separated list line. This used to be a bottom rule
    // and no fill, on the argument that a box around every line turns a list
    // into a stack of cards competing for attention. That argument lost to the
    // drawing, and to the device: redraw 05 gives every row a WT_PANEL fill, a
    // WT_HAIR border and a 10px radius, and the reason is legibility rather than
    // decoration. A label sitting on its own fill has an edge to be read
    // against; the same label floating on the page reads as weak type, which is
    // exactly what the flat list looked like on glass.
    //
    // wt_row_sev() then tints the whole card by severity, which is the other
    // thing the flat list could not do: a warning had to rest on the colour of
    // one small note inside it instead of on the box around it.
    lv_obj_set_style_bg_color(row, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, WT_HAIR, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        wt_tap_feedback(row);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    // The chevron first, so everything else can be measured against it.
    //
    // SELECTED rows get a TICK there instead, in the accent, and the accent on
    // the border. That swap is the whole reason this row exists: a chooser is
    // not navigation. A chevron on STORAGE's three options promises each one
    // leads somewhere, when what they actually do is turn on. A tick answers
    // the only question the screen is asking -- which of these is live -- and
    // it answers it in a mark rather than a word, in every locale at once.
    //
    // Not a filled row. A filled card in a list of three reads as "pressed",
    // and these are not momentary; the accent edge says selected without
    // borrowing the language of a button being held.
    int right = w - 12;
    if (sel) {
        // Flagged as well as painted, all three accents: a selected row built
        // before a theme change would otherwise keep the old accent on its
        // border, tick and icon while everything around it moved -- the
        // chevron branch below has carried its flag from the start, and paint
        // without the flag is exactly the stale-strand defect accent_walk's
        // own comment describes.
        lv_obj_set_style_border_color(row, wt_accent(), 0);
        lv_obj_add_flag(row, WT_FLAG_ACCENT_BORDER);
        lv_obj_t *ok = wt_lbl(row, LV_SYMBOL_OK, 0, 0, wt_font23(),
                              wt_accent());
        lv_obj_add_flag(ok, WT_FLAG_ACCENT);
        lv_obj_update_layout(ok);
        lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -10, 0);
        right = w - 10 - lv_obj_get_width(ok) - 10;
    } else if (cb) {
        // The accent, at 150 opacity. It was WT_DIM on the argument that a
        // chevron is an affordance rather than content and should be the
        // quietest ink on the card. The first half of that is exactly why it
        // should be TINTED: the accent is the device's "this is yours to touch"
        // colour, and the chevron is the mark that says a row is touchable.
        //
        // The opacity is what keeps the second half true. At full strength a
        // page of six rows becomes six bright arrows and the accent stops
        // meaning anything; at 150 it reads as a tint at arm's length and as a
        // colour up close, which is the job.
        //
        // Safe from the status collision for the same reason the eyebrows are:
        // a chevron says a row OPENS, never how it is doing.
        lv_obj_t *ch = wt_lbl(row, LV_SYMBOL_RIGHT, 0, 0, wt_font14(), wt_accent());
        lv_obj_set_style_text_opa(ch, 150, 0);
        lv_obj_add_flag(ch, WT_FLAG_ACCENT);
        lv_obj_update_layout(ch);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
        right = w - 10 - lv_obj_get_width(ch) - 10;
    }

    // The icon badge, and the lane every text on the row starts from. 14 with
    // no icon, 52 with one: a 30px glyph at 14 plus an 8px gutter.
    //
    // The badge is a bare glyph and not a chip. A chip around it would be a
    // third box inside a box inside a card, and the row's own border is already
    // doing the framing this needs -- the explainer's badge is a chip precisely
    // because it floats on an open page with nothing else to hold it.
    const int lx = (icon && *icon) ? 52 : 14;
    if (icon && *icon) {
        // The badge takes the ACCENT, always. It used to be accent only when
        // the row was SELECTED, which meant every list without a current item
        // -- the sign chooser, the file list, the SD screen, the setup pickers
        // -- painted its marks the colour this kit uses for a row that cannot
        // be tapped. Three call sites had already reached for
        // wt_row_icon_accent by hand to undo it.
        //
        // Unselected sits at the CHEVRON's opacity, not full: a page of six
        // rows at full strength is six bright marks and the accent stops
        // meaning anything, which is the argument written above the chevron
        // and it applies here for the same reason. Selected stays at full, so
        // "this is the one you are on" is still louder than "this is a row".
        lv_obj_t *ic = wt_lbl(row, icon, 0, 0, wt_font23(), wt_accent());
        if (!sel) lv_obj_set_style_text_opa(ic, 150, 0);
        lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        lv_obj_set_user_data(ic, (void *)WT_ROW_ICON_TAG);
        lv_obj_update_layout(ic);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    }

    // The LABEL gets the full width on its own line, and the sub-line below
    // shares its line with the value. The obvious arrangement -- label left,
    // value vertically centred on the right -- does not survive this type
    // scale: a row label at font23 is proportionally far wider than the 15px
    // the drawing used, so "Recovery words live in" beside "FLASH" left the
    // label about 150px and it wrapped into its own sub-line. Full width on top
    // means no label in any locale can collide with a value, and the pairing of
    // a small grey fact with the figure it describes on the same line reads
    // better than the figure floating between two rows of text.
    // The VALUE is built and measured BEFORE the label, so the label's box can
    // exclude it. Sizing the label to the chevron alone let a long label run
    // under a wide value: "Recovery words live in" against "SD CARD" collided
    // where the same row against "FLASH" did not, so the defect only appeared
    // once storage had been migrated.
    // The value keeps its natural size: no width is set, so it lays out on one
    // line at its content width. Do NOT give it a long mode -- LONG_CLIP on a
    // label with no explicit size collapses its height to nothing.
    lv_obj_t *v = NULL;
    int vw = 0;
    if (val && *val) {
        v = wt_lbl(row, val, 0, 0, vf, wt_ink_for(vcol));
        lv_obj_update_layout(v);
        vw = lv_obj_get_width(v);
    }

    // The label takes the full width up to the chevron and is pinned to ONE
    // line. Pinning is what makes it safe to be that wide: a label allowed to
    // wrap grew a second line and that line landed on the value's row, which is
    // how "Recovery words live in" collided with "SD CARD" while the same row
    // against "FLASH" was clean. One line means the row's internal geometry is
    // the same in every locale, and a translation too long to fit ellipsises
    // rather than rearranging the row.
    lv_obj_t *l = wt_lbl(row, label, lx, sub && *sub ? 7 : 18,
                         wt_font23(), WT_INK);
    // Width stops short of the value, and the height is pinned to one line.
    // Both are needed. Pinning alone left the label's BOX spanning to the
    // chevron, and because the value sits only a few pixels below the label's
    // baseline the two boxes still shared a 2px band that the overlap gate
    // rightly called a collision. Excluding the value's width as well means the
    // two never share a pixel in any locale, whatever the translation does to
    // either one.
    // The label and the sub BOTH stop short of the value, and the value sits
    // vertically centred on the row's right. That is what redraw 05 draws: the
    // value's baseline falls between the label's and the sub's, not on either.
    //
    // Bounding the label horizontally is not optional. The overlap gate compares
    // BOXES, and a label box spanning to the chevron contains the value's box
    // whatever their baselines do, so a full-width label reported a collision
    // with every value on the page. The labels are short enough for this to cost
    // nothing: the one that was not, "Recovery words live in", is "Words live
    // in" now.
    int lw = right - vw - (vw ? 12 : 0) - lx;
    lv_obj_set_width(l, lw > 60 ? lw : 60);
    lv_obj_set_height(l, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_update_layout(l);

    int liney = (sub && *sub) ? 7 + lv_obj_get_height(l) + 3 : 0;

    if (v) {
        lv_obj_set_pos(v, right - vw, (rowh - lv_obj_get_height(v)) / 2);
        right -= vw + 12;
    }

    if (sub && *sub) {
        int sw = right - lx > 40 ? right - lx : 40;
        if (rowh > WT_ROW_H) {
            // A TALL row's sub is a paragraph, not a caption, and it wraps. This
            // is the chooser case: the sentence under "FLASH" is the reason
            // somebody picks "SD CARD" instead, and an ellipsis through it takes
            // out the second half, which is where the warning lives. Sized to
            // the box that is actually left so a three line translation drops a
            // font size rather than running out of the card.
            // The SAME arithmetic wt_row_sub_font predicts, from the same
            // helper: a private copy here is how the two drift apart.
            const int sh = row_sub_lane_h(rowh);
            const lv_font_t *ssf = sf_auto ? wt_body_font(sub, sw, sh) : sf;
            lv_obj_t *s = wt_lbl(row, sub, lx, liney, ssf, WT_MUT);
            lv_obj_set_width(s, sw);
            lv_obj_set_height(s, sh);
            lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
            lv_obj_set_user_data(s, (void *)WT_SUB_TAG);

            // THE PAIR IS CENTRED, and the 3px above is not the gap it looks
            // like: on a 64px row the label owns 7..37 and the sub owns what
            // is left, so there is nowhere to put a gap and the numbers are
            // the box. A CHOICE row is 94 and the same arithmetic left the
            // sub tucked under the label with 12px of nothing beneath it --
            // "the text below RANDOMNESS AUDIT is quite fucking close to it".
            //
            // The lane is unchanged, so a translation that needs two lines
            // still gets them and still picks its own rung. What moves is
            // where the finished pair SITS: measured, not guessed, because a
            // sub that wrapped is twice as tall and a block centred on the
            // one-line case would hang out of the bottom of the card.
            lv_point_t ss;
            lv_text_get_size(&ss, sub, ssf, 0, 0, sw, LV_TEXT_FLAG_NONE);
            const int subh  = ss.y < sh ? ss.y : sh;
            const int lh    = lv_obj_get_height(l);
            const int total = lh + WT_ROW_SUB_GAP + subh;
            int top = (rowh - total) / 2;
            if (top < 7) top = 7;
            lv_obj_set_height(s, subh);
            lv_obj_set_y(l, top);
            lv_obj_set_y(s, top + lh + WT_ROW_SUB_GAP);
        } else {
            // MEASURED, through the same sink the wide row and the def rows
            // use. This row is where CUT was NOT wired, and it is the row the
            // whole device is built out of: the sub is pinned to one line with
            // LONG_DOT, LVGL rewrites the label's own text to insert the dots,
            // and a gate reading the finished tree finds a string exactly one
            // lane wide with no evidence anything was lost.
            wt_sub_measure("sub", sub, sf, 0, sw);
            lv_obj_t *s = wt_lbl(row, sub, lx, liney, sf, WT_MUT);
            // Declared font14, and it is the BOX deciding rather than the
            // copy: a WT_ROW_H row is 64 tall, its label owns 7..35 at font23
            // and 8px closes the card, so a font23 sub-line would need 36..65
            // and there is nowhere for it to go. A page that wants a readable
            // sub-line uses the def row (chrome23 by default) or passes a
            // TALLER height here, which is the chooser branch above -- and
            // that branch stays visible to the gate, because that is where
            // every real font14 body on this device turned out to be.
            if (rowh <= WT_ROW_H) wt_tiny_ok(s);
            lv_obj_set_width(s, sw);
            // ONE line, height pinned. The width left for the sub depends on how
            // wide the VALUE turned out, and a translated value ("DESACTIVADO"
            // for OFF) squeezes it enough to wrap: the second line then fell past
            // the row's bottom edge and was clipped in seven locales while
            // English was clean. Pinning the height makes LONG_DOT truncate
            // instead of wrap, so the row is the same height whatever the
            // translation does.
            lv_obj_set_height(s, lv_font_get_line_height(sf));
            lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
            lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
        }
    }
    return row;
}

// Recolour just a row's sub-line. For the one case where the explanation is a
// warning and the option is not: FLASH storage on a chip with encryption off is
// a legitimate mode somebody may deliberately want, so the CARD stays neutral
// and only the sentence saying what it costs turns amber. wt_row_sev would wash
// the whole row, and a permanently amber option in a list of three reads as
// broken rather than as cautioned.
//
// By tag, not by child index: a row's children depend on which of the icon, the
// value and the chevron it happened to be given.
void wt_row_sub_color(lv_obj_t *row, lv_color_t c)
{
    c = wt_ink_for(c);
    lv_obj_t *s = wt_tagged(row, WT_SUB_TAG);
    if (s) lv_obj_set_style_text_color(s, c, 0);
}

// Recolour just a row's icon badge from WT_MUT to the accent. For the one case
// where the mark is an identity and not decoration: the secret glyph on the
// KEYS card is drawn in the accent, and the same glyph on a row in another
// screen has to match, or one mark reads as two different things.
//
// By tag, not by child index, for the same reason wt_row_sub_color is: which
// children a row has depends on whether it was given a chevron, a tick, a
// value or a sub. The flag hands the label to accent_walk, so a theme change
// repaints it along with every other accent-inked label.
void wt_row_icon_accent(lv_obj_t *row)
{
    lv_obj_t *ic = wt_tagged(row, WT_ROW_ICON_TAG);
    if (!ic) return;
    lv_obj_set_style_text_color(ic, wt_accent(), 0);
    // FULL strength, which is what this call means now that every badge is
    // already accent at the page tint: it lifts one row's mark above the rest.
    lv_obj_set_style_text_opa(ic, LV_OPA_COVER, 0);
    lv_obj_add_flag(ic, WT_FLAG_ACCENT);
}

// The opt-out, and it is not decoration: a row wearing WT_SEV_OK or WT_SEV_WARN
// has already spent its colour, and on the GREEN theme the accent is WT_OK to
// the byte -- an accent badge on a green card is a second verification tick
// nobody wrote. Three rows need this and they are named where they are called.
void wt_row_icon_mute(lv_obj_t *row)
{
    lv_obj_t *ic = wt_tagged(row, WT_ROW_ICON_TAG);
    if (!ic) return;
    lv_obj_remove_flag(ic, WT_FLAG_ACCENT);
    lv_obj_set_style_text_color(ic, WT_MUT, 0);
    lv_obj_set_style_text_opa(ic, LV_OPA_COVER, 0);
}

// Tint a built row by severity. Redraw 05 colours the BOX, not just a note
// inside it: the green card is a state that is satisfied, the amber one a
// warning about where the words live, the red pair the two actions that cannot
// be undone. Measured off the drawing, which uses a 4 to 5 percent fill under a
// 30 percent border of the same hue -- barely a wash, but enough that the group
// reads before any of its words do.
//
// Opacities are LV_OPA values (0..255): 13 is the 5 percent fill, 77 the 30
// percent border. Status hues are never themed, so this takes no accent.
void wt_row_sev(lv_obj_t *row, int sev)
{
    if (!row) return;
    lv_color_t c;
    switch (sev) {
        case WT_SEV_OK:   c = WT_OK;   break;
        case WT_SEV_WARN: c = WT_WARN; break;
        case WT_SEV_STOP: c = WT_STOP; break;
        default:
            lv_obj_set_style_bg_color(row, WT_PANEL, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(row, WT_HAIR, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
            return;
    }
    lv_obj_set_style_bg_color(row, c, 0);
    lv_obj_set_style_bg_opa(row, 13, 0);
    // The RAIL, and it sets its own width. This painted a border colour and an
    // opacity and left the width to whoever built the row -- which was fine
    // while every row was a card with a 1px edge, and became nothing at all
    // the moment rows went borderless. A tappable row still showed it, because
    // wt_line_press happens to put a 3px left border there for the pressed
    // state; an INERT row showed a 13-opacity wash and no edge whatever, which
    // is the state the NO UNDO comment already measured as invisible.
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, c, 0);
    lv_obj_set_style_border_opa(row, 77, 0);
}

// The box every Settings row is made of, with nothing in it. Extracted so the
// screens whose content is not a ROW can still wear it: an address block, a
// camera viewport, a status line. Same fill, same hairline, same 10px radius, so
// a card on Receive and a card on Settings are the same object to the eye, and
// wt_row_sev() tints either one.
//
// The fill is what does the work. Type on WT_PANEL has an edge to be read
// against; the same type floating on WT_BG reads as weak, which is the whole
// difference between the settings screen the owner liked and the flat list it
// replaced. Any screen with a block of content and no card is the flat list
// again under a different name.
lv_obj_t *wt_card(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, WT_HAIR, 0);
    lv_obj_set_style_bg_color(card, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// ---- SETTINGS: the section tabs (see kiss_theme.h) ----
//
// A designated initialiser that leaves a colour out gives you {0,0,0}, and
// pure black is the one value nothing on this device is painted in -- no ink,
// no border, no fill. So it reads as "unset" and the field takes its default,
// which keeps the wt_wide_t and wt_pop_item_t literals in kiss_settings.c
// short instead of charging every one of them a WT_MUT it did not want to
// think about. Cheaper than a parallel bool per colour, and impossible to get
// half right the way a bool can be.
static lv_color_t col_or(lv_color_t c, lv_color_t dflt)
{
    return (c.red || c.green || c.blue) ? c : dflt;
}

#define WT_TAB_GAP    9    // icon to label, and label to dot
#define WT_TAB_DOT    7
#define WT_TAB_SPACE  2    // the tracking a font14 label wears at this size

// The highlight's two skins as ONE number: 0 is the ordinary group's panel
// fill and edge, 255 is the destructive group's stop tint. A move into or out
// of NO UNDO changes the fill, the border and both opacities at once, so the
// slide carries a single mix rather than swapping four styles at whichever
// moment happens to look least wrong.
#define WT_TAB_STOP_BG_OPA 13
#define WT_TAB_STOP_BD_OPA 77

static void tab_hl_skin(lv_obj_t *hl, int32_t t)
{
    lv_obj_set_style_bg_color(hl, lv_color_mix(WT_STOP, WT_PANEL, (uint8_t)t), 0);
    lv_obj_set_style_bg_opa(hl, (lv_opa_t)(LV_OPA_COVER
        + (WT_TAB_STOP_BG_OPA - LV_OPA_COVER) * t / 255), 0);
    lv_obj_set_style_border_color(hl, lv_color_mix(WT_STOP, WT_EDGE, (uint8_t)t), 0);
    lv_obj_set_style_border_opa(hl, (lv_opa_t)(LV_OPA_COVER
        + (WT_TAB_STOP_BD_OPA - LV_OPA_COVER) * t / 255), 0);
}

static void tab_hl_x(void *v, int32_t x)   { lv_obj_set_x((lv_obj_t *)v, x); }
static void tab_hl_mix(void *v, int32_t t) { tab_hl_skin((lv_obj_t *)v, t); }

lv_obj_t *wt_tabs(lv_obj_t *scr, const wt_tab_t *tabs, int n, int sel,
                  int x, int y, lv_event_cb_t cb)
{
    const lv_font_t *f = wt_font14();

    // FIRST, so every button draws over it. It is the only part of the strip
    // that moves, and the buttons above it are identical to each other.
    lv_obj_t *hl = lv_obj_create(scr);
    lv_obj_remove_style_all(hl);
    lv_obj_set_pos(hl, x + sel * WT_TAB_PITCH, y);
    lv_obj_set_size(hl, WT_TAB_W, WT_TAB_H);
    lv_obj_set_style_radius(hl, 10, 0);
    lv_obj_set_style_border_width(hl, 1, 0);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);   // the button over it takes the tap
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
    // The strip's own origin, which wt_tabs_select needs to place tab `to` and
    // cannot recover from a highlight caught mid slide.
    lv_obj_set_user_data(hl, (void *)(intptr_t)x);
    tab_hl_skin(hl, (sel >= 0 && sel < n && tabs[sel].stop) ? 255 : 0);

    for (int i = 0; i < n; i++) {
        const wt_tab_t *t = &tabs[i];

        lv_obj_t *b = lv_obj_create(scr);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, x + i * WT_TAB_PITCH, y);
        lv_obj_set_size(b, WT_TAB_W, WT_TAB_H);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // The same press answer every row on the device gives, so a tab reads
        // as the same family of object as the rows it switches between.
        lv_obj_set_style_bg_color(b, wt_accent_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        wt_tap_feedback(b);
        if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // The highlight is a FILL and an edge, never the accent, and it is the
        // object created above rather than anything set here. Which also puts
        // the labels back in line: LV_ALIGN_LEFT_MID aligns to the CONTENT
        // area, so the one tab wearing a 1px border used to hold its label a
        // pixel right of the other four, and the word jumped when you selected
        // it. Every settings frame in the walk moved by exactly that pixel.
        //
        // A tab strip is
        // navigation: it says where you are, which is not a status and not an
        // action, so it stays in the surface colours and leaves the accent to
        // the chevrons that say a row opens.
        lv_color_t ink  = t->stop ? WT_STOP_INK : WT_INK;
        lv_color_t mark = t->stop ? WT_STOP : WT_MUT;

        // Measured and centred as a group, because the icon, the label and the
        // dot are three objects and only their TOTAL can be centred. Letter
        // spacing is measured with the label, or the centring is off by two
        // pixels per character in every locale.
        lv_point_t is = { 0, 0 }, ls;
        if (t->icon && *t->icon)
            lv_text_get_size(&is, t->icon, f, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
        lv_text_get_size(&ls, t->label, f, WT_TAB_SPACE, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        int iw = is.x ? is.x + WT_TAB_GAP : 0;
        int dw = t->dot ? WT_TAB_GAP + WT_TAB_DOT : 0;
        int lw = ls.x;
        // A locale whose word does not fit LOSES LETTERS. The tab does not
        // widen: 144 on a 152 pitch is what puts five groups in the 752 lane,
        // and one long translation may not move the other four.
        int room = WT_TAB_W - 8 - iw - dw;
        if (lw > room) lw = room;
        int px = (WT_TAB_W - (iw + lw + dw)) / 2;
        if (px < 4) px = 4;

        if (iw) {
            lv_obj_t *ic = wt_lbl(b, t->icon, 0, 0, f, mark);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, px, 0);
        }
        lv_obj_t *l = wt_lbl(b, t->label, 0, 0, f, ink);
        lv_obj_set_style_text_letter_space(l, WT_TAB_SPACE, 0);
        lv_obj_set_width(l, lw);
        lv_obj_set_height(l, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, px + iw, 0);

        if (t->dot) {
            lv_obj_t *d = lv_obj_create(b);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, WT_TAB_DOT, WT_TAB_DOT);
            lv_obj_set_style_radius(d, WT_TAB_DOT, 0);
            lv_obj_set_style_bg_color(d, WT_WARN, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);   // the tab takes the tap
            lv_obj_align(d, LV_ALIGN_LEFT_MID, px + iw + lw + WT_TAB_GAP, 0);
        }
    }
    return hl;
}

void wt_tabs_select(lv_obj_t *hl, int from, int to, bool stop)
{
    (void)from;
    if (!hl || !lv_obj_is_valid(hl)) return;
    const int x0 = (int)(intptr_t)lv_obj_get_user_data(hl);

    // Both start from where the highlight IS, not from where the last tap
    // meant it to end up. Tapping across the strip faster than 200ms is one
    // continuous slide rather than five jumps to the left edge.
    lv_anim_delete(hl, tab_hl_x);
    lv_anim_delete(hl, tab_hl_mix);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, hl);
    lv_anim_set_duration(&a, WT_TAB_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, tab_hl_x);
    lv_anim_set_values(&a, lv_obj_get_x(hl), x0 + to * WT_TAB_PITCH);
    lv_anim_start(&a);

    // The mix is read back off the fill rather than remembered, so an
    // interrupted cross fade resumes from the colour on the glass. Skipped
    // when there is nowhere to travel: every pair of ordinary tabs wears the
    // same skin, and animating 255 -> 0 between two of them flashes red.
    const int32_t cur = lv_obj_get_style_bg_opa(hl, LV_PART_MAIN);
    const int32_t t0 = (LV_OPA_COVER - cur) * 255
                     / (LV_OPA_COVER - WT_TAB_STOP_BG_OPA);
    const int32_t t1 = stop ? 255 : 0;
    if (t0 != t1) {
        lv_anim_set_exec_cb(&a, tab_hl_mix);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_values(&a, t0, t1);
        lv_anim_start(&a);
    }
}

// ---- KEYS / RECEIVE: the borderless idioms (see kiss_theme.h) ----
// The kit's three animation exec callbacks live with the pane code below;
// the bracket strip is the one caller above them.
static void an_tx(void *v, int32_t x);
static void an_opa(void *v, int32_t o);

void wt_line_press(lv_obj_t *row)
{
    // The rail. A left border already present at opacity 0 costs one style
    // property and no second object; the pressed state flips its opa and
    // lights a 3px accent edge down the row under the finger. That plus the
    // wash is the only feedback a borderless row can give, so it is not
    // decoration -- without it the row is an unmarked target.
    lv_obj_set_style_radius(row, 6, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, wt_accent(), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, wt_accent_pressed(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, WT_FLAG_ACCENT_BORDER);
    lv_obj_add_flag(row, WT_FLAG_ACCENT_BG);
    // A style transition, not an lv_anim: LVGL runs it on the state change
    // itself, so a press released mid-fade reverses rather than finishing and
    // snapping back.
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_OPA, LV_STYLE_BORDER_OPA, LV_STYLE_PROP_INV
    };
    static lv_style_transition_dsc_t tr;
    static bool tr_ready;
    if (!tr_ready) {
        lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out,
                                     160, 0, NULL);
        tr_ready = true;
    }
    lv_obj_set_style_transition(row, &tr, 0);
}

int wt_line_val_y(void)
{
    return WT_LINE_CAP_Y + lv_font_get_line_height(wt_font23()) - 2;
}

lv_obj_t *wt_line_rule(lv_obj_t *par, int x, int y, int w)
{
    lv_obj_t *r = lv_obj_create(par);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_style_bg_color(r, WT_DIV, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    // The pivot the entry animation needs, set HERE rather than at the call
    // site. transform_scale_x with the default centre pivot draws the rule
    // from its middle outward, which inverts the whole effect -- and it does
    // it silently, because a rule at half scale still measures as a rule.
    lv_obj_set_style_transform_pivot_x(r, 0, 0);
    return r;
}

void wt_line_rule_draw(lv_obj_t *rule, int delay_ms, int ms)
{
    if (!rule) return;
    lv_obj_update_layout(rule);
    const int w = lv_obj_get_width(rule);
    if (w <= 0) return;
    lv_obj_set_width(rule, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rule);
    lv_anim_set_exec_cb(&a, an_w);
    lv_anim_set_values(&a, 0, w);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void wt_line_row_stage(lv_obj_t *row, int k)
{
    if (!row) return;
    // The VALUE, which is the tagged one: a row also holds a caption, a sub and
    // an arrow, and all three belong to the line rather than to the reading.
    lv_obj_t *v = NULL;
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, i);
        if (lv_obj_get_user_data(c) == (void *)WT_LINE_VAL_TAG) { v = c; break; }
    }
    if (!v) return;
    const int delay = 42 * k + 90;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, v);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, 7, 0);
    lv_anim_set_exec_cb(&a, an_tx);
    lv_anim_start(&a);
    lv_obj_set_style_opa(v, LV_OPA_TRANSP, 0);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, an_opa);
    lv_anim_start(&a);
}

lv_obj_t *wt_help_mark(lv_obj_t *par, int x, int y)
{
    lv_obj_t *m = lv_obj_create(par);
    lv_obj_remove_style_all(m);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 19, 19);
    lv_obj_set_style_radius(m, 10, 0);
    lv_obj_set_style_border_width(m, 1, 0);
    lv_obj_set_style_border_color(m, wt_accent(), 0);
    lv_obj_set_style_border_opa(m, 115, 0);
    lv_obj_add_flag(m, WT_FLAG_ACCENT_BORDER);
    // Takes no taps: the row under it is the target and a 19px circle inside a
    // 468px row that already opens the same explainer would only ever steal
    // presses from it.
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *q = wt_lbl(m, "?", 0, 0, wt_font14(), wt_accent());
    lv_obj_add_flag(q, WT_FLAG_ACCENT);
    lv_obj_center(q);
    return m;
}

lv_obj_t *wt_title_cursor(lv_obj_t *scr)
{
    lv_obj_t *t = wt_screen_title(scr);
    if (!t) return NULL;
    // MEASURED, not laid out. lv_obj_update_layout on the title runs a full
    // screen layout pass, and that pass does not always settle: LVGL loops
    // until nothing changes, and a label whose content width lands exactly on
    // a wrap boundary flips between one line and two forever. Korean found it
    // -- the sign page's TERMS tab hung the device at 95% CPU, inside this
    // call, with the walk stopped dead and no output to say why.
    //
    // Nothing here needed the layout anyway. The title is a plain label at a
    // position this file sets itself (48, 18), so its origin is known, and
    // lv_text_get_size answers the rest without touching the tree. It is also
    // what check_layout_reads.py asks for: measure the text, do not ask the
    // tree what it did with it.
    lv_point_t ts;
    const char *ttxt = lv_label_get_text(t);
    lv_text_get_size(&ts, ttxt ? ttxt : "",
                     lv_obj_get_style_text_font(t, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(t, LV_PART_MAIN), 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_t *cur = lv_obj_create(scr);
    lv_obj_remove_style_all(cur);
    lv_obj_set_user_data(cur, (void *)WT_CURSOR_TAG);
    lv_obj_set_size(cur, 10, 22);
    // Centred on the title's cap height rather than its box: a font34 line box
    // carries descender room no capital reaches into, so centring on the box
    // sits the block visibly low against KEYS and RECEIVE, which have no
    // descenders at all.
    //
    // THE STYLE PROPERTY, not lv_obj_get_x. Dropping the layout above took
    // the title's COORDS with it: lv_obj_set_pos only writes LV_STYLE_X and
    // marks the tree dirty, and lv_obj_get_x reads obj->coords, which the
    // next layout pass fills. On a title positioned three lines earlier both
    // reads are 0, so the block landed 48 left and 14 high -- a clipped
    // sliver against the top bezel, on every page that has a head. The style
    // getter reads back exactly what wt_chrome_head wrote and needs nothing
    // laid out, which is the whole point of measuring instead.
    lv_obj_set_pos(cur, lv_obj_get_style_x(t, LV_PART_MAIN) + ts.x + 12,
                   lv_obj_get_style_y(t, LV_PART_MAIN) + (ts.y - 22) / 2 - 2);
    lv_obj_set_style_bg_color(cur, wt_accent(), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_obj_add_flag(cur, WT_FLAG_ACCENT_FILL);
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_SCROLLABLE);

    // step, not a fade: a cursor BLINKS. An eased opacity ramp reads as a
    // pulse, which is the device's "something is happening" language and this
    // is not that -- it is the page saying it is waiting for you.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cur);
    lv_anim_set_exec_cb(&a, an_opa);
    // 850 out and 850 back is the handoff's 1700ms cycle: lv_anim_path_step
    // holds the start value for the whole duration and only then jumps, so one
    // 1700ms leg would be 1700ms lit and one frame dim.
    lv_anim_set_values(&a, LV_OPA_COVER, 31);
    lv_anim_set_duration(&a, 850);
    lv_anim_set_playback_duration(&a, 850);
    lv_anim_set_path_cb(&a, lv_anim_path_step);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    return cur;
}

lv_obj_t *wt_line_row(lv_obj_t *par, int x, int y, int w, int h,
                      const char *cap, const char *val, const lv_font_t *vf,
                      lv_color_t vcol, const char *sub, const lv_font_t *sf,
                      lv_event_cb_t cb, void *ud)
{
    if (!sf) sf = wt_font23();
    lv_obj_t *row = lv_obj_create(par);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, w, h);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
        wt_line_press(row);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    // The arrow FIRST, so the sub can be measured against the lane it leaves.
    // Absent, not dimmed, on a row that opens nothing: a mark at low opacity
    // still says "there is something here", which is the opposite of true.
    int right = w - WT_LINE_PAD;
    if (cb) {
        lv_obj_t *ar = wt_lbl(row, WT_ICON_ARR_R, 0, 0, wt_font23(),
                              wt_accent());
        lv_obj_add_flag(ar, WT_FLAG_ACCENT);
        lv_obj_update_layout(ar);
        lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -4 - (24 - lv_obj_get_width(ar)) / 2,
                     0);
        right = w - 4 - 24;
    }

    // The page font, not the mono one. The mono face is for DATA -- a version,
    // an address, a fingerprint, a path -- where fixed pitch is what lets an
    // owner compare two of them character by character. A caption is a WORD,
    // and a word set in a second typeface next to a page of Montserrat reads
    // as a rendering fault rather than as a distinction. Reported from the
    // bench as exactly that: "idk why smaller fonts are different fonts".
    // Letter space 2, which is what wt_row_head has always used for the same
    // kind of label.
    lv_obj_t *c = wt_lbl(row, cap, WT_LINE_PAD, WT_LINE_CAP_Y,
                         wt_font23(), WT_MUT);
    lv_obj_set_style_text_letter_space(c, 1, 0);

    // font23, and NOT font14. A sub-line here is a SENTENCE -- "names these
    // keys", "compare the last 8", "so it finds these payments" -- and the
    // house rule about tiny type is about exactly these. It shipped at 14 on
    // the argument that a sub beside a font23 value is metadata; that argument
    // came back off the bench as "its fucking tiny", which is the fifth time
    // the same rule has been reported. There is no version of this where the
    // reader is wrong.
    //
    // The lane grows to 300 to pay for it and the VALUE gives up the width,
    // which is the right way round: a value is one short string and a sub is
    // the sentence explaining it.
    //
    // The SUB is measured before either it or the value is placed, and its box
    // is sized to the text rather than to the lane. A label pinned to a fixed
    // 200 and right-aligned inside it leaves an empty box reaching back across
    // the row, and overlapcheck compares BOXES -- so a short sub beside a long
    // value read as an overlap that nothing on the glass could show.
    // 320, not 300. STR_S_CMP_8 -- "compare the lit characters", the one
    // sentence three screens share -- wants 308 at font23, and CUT reported it
    // ellipsised. The lane is a constant chosen here, not a geometry the page
    // is stuck with like the 196px tab, so it gives the eight pixels and the
    // value gives them up. A 21 locale string does not get cut to save a
    // number that was picked round.
    const int lane = 320;
    int subw = 0;
    if (sub && *sub) {
        lv_point_t ss;
        lv_text_get_size(&ss, sub, sf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        subw = ss.x > lane ? lane : ss.x;
    }
    // 46 in from the row's right, ALWAYS: 4 of lane inset, the arrow's 24, and
    // 18 of gap. Measured off the arrow instead, the one row with no arrow
    // would hang its sub 14px right of every other and the column would not
    // read straight down.
    const int sub_x = w - 46 - subw;

    if (val) {
        // Stops 18 clear of whatever is to its right -- the sub if there is
        // one, the arrow's lane if there is not. The value is the row's
        // subject and is never ellipsised by choice, but a locale that
        // overruns has to lose letters rather than run under the sub.
        const int vw = (subw ? sub_x : right) - 18 - WT_LINE_PAD;
        lv_obj_t *v = wt_lbl(row, val, WT_LINE_PAD, wt_line_val_y(),
                             vf ? vf : wt_font23(), wt_ink_for(vcol));
        lv_obj_set_user_data(v, (void *)WT_LINE_VAL_TAG);
        lv_obj_set_width(v, vw);
        lv_obj_set_height(v, lv_font_get_line_height(vf ? vf : wt_font23()));
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    }

    if (subw) {
        lv_obj_t *sl = wt_lbl(row, sub, 0, 0, sf, WT_DIM);
        wt_sub_measure("sub", sub, sf, 0, lane);
        lv_obj_set_width(sl, subw);
        lv_obj_set_height(sl, lv_font_get_line_height(sf));
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -46, 0);
    }
    return row;
}

// ---- the arrow action ----
void wt_arrow_action_set_text(lv_obj_t *ctrl, const char *txt)
{
    if (!ctrl || !txt) return;
    lv_obj_t *a = NULL, *l = NULL;
    const uint32_t n = lv_obj_get_child_count(ctrl);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(ctrl, i);
        if (!lv_obj_check_type(c, &lv_label_class)) continue;
        const char *t = lv_label_get_text(c);
        // The arrow is one glyph from the symbol range; the word is not.
        if (!a && t && (unsigned char)t[0] == 0xEF) a = c;
        else l = c;
    }
    if (!l || !a) return;
    const lv_font_t *f = chrome23(txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_point_t ls, as;
    lv_text_get_size(&ls, txt, f, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&as, lv_label_get_text(a), wt_font23(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const bool back = lv_obj_get_x(a) <= lv_obj_get_x(l);
    lv_label_set_text(l, txt);
    lv_obj_set_width(ctrl, ls.x + 12 + as.x);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, back ? 0 : ls.x + 12, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, back ? as.x + 12 : 0, 0);
}

lv_obj_t *wt_arrow_action(lv_obj_t *scr, const char *txt, bool back,
                          bool primary, int x, int y, int w, bool right,
                          lv_event_cb_t cb, void *ud)
{
    if (y >= WT_CONTENT_BOTTOM) action_bar_ensure(scr);

    // Two faces on one control, on purpose: the WORD takes the pass's mono23
    // (with the locale guard), and the arrow stays on the Latin face because
    // IoskeleyMono carries no FontAwesome at all -- the same split every tab
    // strip already makes.
    const lv_font_t *f  = chrome23(txt);
    const lv_font_t *af = wt_font23();
    lv_point_t ls, as;
    const char *arrow = back ? WT_ICON_ARR_L : WT_ICON_ARR_R;
    lv_text_get_size(&ls, txt, f, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&as, arrow, af, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int cw = ls.x + 12 + as.x;

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    // The hit box is the action row's full height and the content's width. No
    // fill, no border, no radius: the box IS the two labels, and anything
    // drawn around them would be the pill this replaces.
    lv_obj_set_size(p, cw, WT_ACTION_H);
    lv_obj_set_pos(p, right ? x + w - cw : x, y);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, ud);

    // The whole control shifts 5px the way its arrow points. Not a sink: the
    // tap feedback's 2px drop reads as a button being pushed into the page, and this
    // is not a button -- it is a direction, so the feedback is movement along
    // it. A style transition so a released press reverses rather than snaps.
    {
        static const lv_style_prop_t props[] = {
            LV_STYLE_TRANSLATE_X, LV_STYLE_PROP_INV
        };
        static lv_style_transition_dsc_t tr;
        static bool tr_ready;
        if (!tr_ready) {
            lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out,
                                         170, 0, NULL);
            tr_ready = true;
        }
        lv_obj_set_style_translate_x(p, 0, 0);
        lv_obj_set_style_translate_x(p, back ? -5 : 5, LV_STATE_PRESSED);
        lv_obj_set_style_transition(p, &tr, 0);
    }

    // The arrow points WHERE THE TAP TAKES YOU: leading the label on the way
    // out, trailing it on the way in.
    lv_obj_t *a = wt_lbl(p, arrow, 0, 0, af, wt_accent());
    lv_obj_add_flag(a, WT_FLAG_ACCENT);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, back ? 0 : ls.x + 12, 0);

    // The primary takes the accent on its LABEL as well, and takes nothing
    // else: there is no fill left to give it.
    lv_obj_t *l = wt_lbl(p, txt, 0, 0, f, primary ? wt_accent() : WT_INK);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    if (primary) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, back ? as.x + 12 : 0, 0);
    return p;
}

// ---- the word action (see kiss_theme.h) ----
lv_obj_t *wt_word_action(lv_obj_t *par, const char *mark, const char *txt,
                         bool lead, lv_color_t col, bool accent,
                         lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = lv_obj_create(par);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 40);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 10, 0);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(c, 8);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    }
    for (int pass = 0; pass < 2; pass++) {
        const bool mark_turn = (pass == 0) == lead;
        if (!(mark_turn ? mark : txt)) continue;
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, mark_turn ? mark : txt);
        lv_obj_set_style_text_font(l, mark_turn ? wt_font23()
                                                : chrome23(txt), 0);
        if (!mark_turn) lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_obj_set_style_text_color(l, col, 0);
        if (accent) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    }
    return c;
}

// ---- the SCREEN SYSTEM: chrome, [ ? ], definition rows (see kiss_theme.h) --

static void an_ty(void *v, int32_t y);
static void an_h(void *v, int32_t h)   { lv_obj_set_height(v, h); }
static void an_y(void *v, int32_t y)   { lv_obj_set_y(v, y); }
static void an_rot(void *v, int32_t r)
{
    lv_obj_set_style_transform_rotation(v, r, 0);
}

// Whether the mono faces can draw every glyph of `s`: ASCII 0x20-0x7E plus
// the three marks gen_fonts.sh gives them. The chrome is specified in mono,
// but the OTHER twenty locales still carry their shipped strings under the
// English-only rule, and an accented or CJK title pointed at IoskeleyMono
// draws placeholder boxes on a screen the owner cannot file a bug from. So
// every chrome string checks itself and degrades to the locale face -- the
// same second line of defence wt_font34 keeps for CJK titles. The sweep
// decides the real per-locale answer; until then the device stays readable.
static bool mono_can(const char *s)
{
    if (!s) return true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n' || (*p >= 0x20 && *p < 0x7F)) continue;
        if (*p == 0xC2 && p[1] == 0xB7) { p++; continue; }               /* · */
        if (*p == 0xE2 && p[1] == 0x80 && (p[2] == 0xA2 || p[2] == 0xA6)) {
            p += 2;                                                  /* • … */
            continue;
        }
        return false;
    }
    return true;
}

// The contract's faces, each with its locale fallback. 18 and 21 fall to the
// 14 and 23 sans rungs -- one size DOWN, not up, so a translated string can
// never grow into the hairline under it. The fallbacks are the NAT faces, not
// the body composites: a string that failed mono_can is mostly non-ASCII, and
// the sans-primary face keeps it in one typeface instead of setting its
// stray ASCII in mono.
static const lv_font_t *chrome18(const char *s)
{
    return mono_can(s) ? wt_font_mono18() : nat14();
}
static const lv_font_t *chrome21(const char *s)
{
    return mono_can(s) ? wt_font_mono21() : nat23();
}
static const lv_font_t *chrome23(const char *s)
{
    return mono_can(s) ? wt_font_mono23() : nat23();
}
static const lv_font_t *chrome28(const char *s)
{
    return mono_can(s) ? wt_font_mono28() : nat28();
}

void wt_chrome_head(lv_obj_t *scr)
{
    lv_obj_t *t = wt_screen_title(scr);
    if (t) {
        // The contract restyles what wt_screen built rather than building a
        // second header: one code path keeps the walk's screen bookkeeping,
        // and the title keeps its tag so nothing downstream loses it. INK,
        // not the accent: the cursor is the accent's one appearance up here.
        const char *txt = lv_label_get_text(t);
        lv_obj_set_style_text_font(t, chrome28(txt), 0);
        lv_obj_set_style_text_letter_space(t, 3, 0);
        lv_obj_set_style_text_color(t, WT_INK, 0);
        lv_obj_set_pos(t, WT_LANE_X, WT_CHROME_TITLE_Y);
        // wt_screen fitted the sans face it created. Re-fit after changing to
        // chrome so the full-width case gets the same 28 -> 23 -> 18 ladder as
        // callers that later narrow the lane around a header control.
        wt_title_fit(scr, WT_LANE_W);
    }
    wt_title_cursor(scr);
}

lv_obj_t *wt_chrome(lv_obj_t *parent, const char *title)
{
    lv_obj_t *scr = wt_screen(parent, title, NULL);
    wt_chrome_head(scr);
    wt_line_rule(scr, WT_LANE_X, WT_CHROME_RULE_Y, WT_LANE_W);
    // The band exists on every chrome page, control or no control: it carries
    // the standing statement and the first-run hint, and a page whose band
    // appeared only once a button did would visibly re-floor itself.
    action_bar_ensure(scr);
    return scr;
}

// One flex tab's dress. The brackets go TRANSPARENT rather than away when
// unselected -- the space stays reserved, so nothing shifts as selection
// moves -- and the destructive tab's label keeps its full WT_STOP in every
// state: the red is the tab's meaning, not its selection.
static void tabs_flex_paint(lv_obj_t *b, bool selected)
{
    const bool stop = lv_obj_get_user_data(b) == (void *)(intptr_t)1;
    // Found, not indexed: a tab may or may not carry its mark (the strip
    // draws icons only when the whole row has room), so the label's slot
    // moves. The mark, when present, sits between [ and the word.
    lv_obj_t *lb = NULL, *rb = NULL, *l = NULL, *ic = NULL;
    const uint32_t n = lv_obj_get_child_count(b);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(b, i);
        if (!lv_obj_check_type(c, &lv_label_class)) continue;   // the dot
        const char *t = lv_label_get_text(c);
        if (t && !strcmp(t, "["))      lb = c;
        else if (t && !strcmp(t, "]")) rb = c;
        else if (!ic && lb && !l)      ic = c;   // tentatively the mark
        else                           l  = c;
    }
    if (!l && ic) { l = ic; ic = NULL; }         // a tab with no mark
    if (l) lv_obj_set_style_text_color(l, stop ? WT_STOP
                                              : (selected ? WT_INK : WT_DIM),
                                       0);
    if (ic) {
        lv_obj_set_style_text_color(ic, stop ? WT_STOP
                                             : (selected ? wt_accent()
                                                         : WT_DIM), 0);
        // Only the SELECTED icon is accent-painted, so only it may carry
        // the flag, or a restyle would light every tab's mark at once.
        if (selected && !stop) lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        else                   lv_obj_remove_flag(ic, WT_FLAG_ACCENT);
    }
    lv_obj_t *br[2] = { lb, rb };
    for (int i = 0; i < 2; i++) {
        if (!br[i]) continue;
        lv_obj_set_style_opa(br[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP,
                             0);
        lv_obj_set_style_text_color(br[i], wt_accent(), 0);
        if (selected) lv_obj_add_flag(br[i], WT_FLAG_ACCENT);
        else          lv_obj_remove_flag(br[i], WT_FLAG_ACCENT);
    }
}

lv_obj_t *wt_tabs_flex(lv_obj_t *scr, const wt_tab_t *tabs, int n, int sel,
                       lv_event_cb_t cb)
{
    lv_obj_t *strip = lv_obj_create(scr);
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, WT_LANE_X, WT_CHROME_STRIP_Y);
    lv_obj_set_size(strip, 620, WT_BR_H);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    // CENTERED GROUP, not SPACE_BETWEEN and not packed left. Spreading pushed
    // two short tabs to opposite ends of 620, packing left grouped them but
    // left the pair hugging one edge of the lane -- and the bench asked for
    // both halves: together AND centered. The gap is set below, once the
    // tabs are measured: 28 where the lane has it, the leftover where it
    // does not, so a full strip renders exactly as it always did.
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_font_t *bf = wt_font_mono18();
    // The labels are chrome23 now -- a tab TITLE the owner reported reading
    // as fine print at 18 -- and mono at 23 wants no letter spacing, so the
    // measure below uses none either. The brackets stay a rung down: they
    // are punctuation, and at 23 their keep alone would push the five-up
    // settings strip past the lane.
    //
    // The overflow cap, from load rather than by fiat. This was a flat 140
    // ("a fifth of the lane less the brackets' keep"), sized for the five-up
    // settings strip -- and the moment a THREE tab page borrowed the strip,
    // 140 ellipsised two of its three labels in a lane with 120px to spare.
    // So measure first: if every label plus the bracket keep fits the 620,
    // nothing is capped; only when the strip genuinely overflows does each
    // oversized label fall back to its fair share. 30 is two brackets and
    // their pads. No minimum air between tabs: the brackets ARE the
    // separation.
    //
    // The MARK, when the strip has room for it. This strip dropped every
    // icon it was handed for as long as it existed; now it measures them
    // first and draws them only when labels + keep + icons all fit -- the
    // five-up settings strip has no room and stays words-only, the two and
    // three tab pages get their marks. Icons come off the Latin face via
    // wt_font23's fallback: IoskeleyMono carries no FontAwesome.
    const int keep = 30 * n;
    int need = keep, ineed = 0;
    for (int i = 0; i < n; i++) {
        lv_point_t ls;
        lv_text_get_size(&ls, tabs[i].label, chrome23(tabs[i].label), 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        need += ls.x;
        if (tabs[i].icon && *tabs[i].icon) {
            lv_point_t is;
            lv_text_get_size(&is, tabs[i].icon, wt_font23(), 0, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            ineed += is.x + 4;
        }
    }
    const bool icons = ineed > 0 && need + ineed <= 620;
    const int fair = (620 - keep) / n;
    const bool over = need > 620;
    // The inter-tab gap, from the room the measured tabs leave: 28 keeps a
    // two or three tab strip reading as one group, and a strip that already
    // fills the lane drops to whatever is left (the shipped five-up runs at
    // ~3px, which is what SPACE_BETWEEN was giving it anyway).
    if (n > 1) {
        int gap = (620 - (need + (icons ? ineed : 0))) / (n - 1);
        if (gap > 28) gap = 28;
        if (gap < 2)  gap = 2;
        lv_obj_set_style_pad_column(strip, gap, 0);
    }
    for (int i = 0; i < n; i++) {
        const wt_tab_t *t = &tabs[i];
        lv_obj_t *b = lv_obj_create(strip);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, WT_BR_H);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        wt_tap_feedback(b);
        if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)i);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, 4, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)(t->stop ? 1 : 0));

        wt_lbl(b, "[", 0, 0, bf, wt_accent());
        if (icons && t->icon && *t->icon)
            wt_lbl(b, t->icon, 0, 0, wt_font23(), WT_DIM);
        const lv_font_t *lf = chrome23(t->label);
        lv_obj_t *l = wt_lbl(b, t->label, 0, 0, lf, WT_DIM);
        // A translation too wide for its share loses letters; the lane never
        // widens and the strip never wraps. But only when the strip as a
        // whole overflows: a label under the fair share never pays for one
        // over it, and a strip that fits is never cut at all.
        lv_point_t ls;
        lv_text_get_size(&ls, t->label, lf, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (over && ls.x > fair) {
            lv_obj_set_width(l, fair);
            lv_obj_set_height(l, lv_font_get_line_height(lf));
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        }
        wt_lbl(b, "]", 0, 0, bf, wt_accent());
        if (t->dot) {
            // FLOATING: the layout skips it, so the unread mark costs the
            // strip no width -- as a flex child its 7px plus a pad per dot
            // is what pushed the packed five-up strip past 620 and clipped
            // NO UNDO's bracket off the edge. It rides the tab's top right
            // corner instead, the shape a badge already has.
            lv_obj_t *d = lv_obj_create(b);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, 7, 7);
            lv_obj_set_style_radius(d, 4, 0);
            lv_obj_set_style_bg_color(d, WT_WARN, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(d, LV_OBJ_FLAG_FLOATING);
            lv_obj_align(d, LV_ALIGN_TOP_RIGHT, 0, 0);
            // The mark breathes. A static dot was filed from the bench as
            // "not pulsing"; the ring flare answers a TAP, this answers a
            // glance.
            wt_dot_breathe(d, 7, 3, true);
        }
        tabs_flex_paint(b, i == sel);
    }
    return strip;
}

void wt_tabs_flex_select(lv_obj_t *strip, int from, int to, bool stop)
{
    (void)stop;
    if (!strip) return;
    int n = (int)lv_obj_get_child_count(strip);
    if (from >= 0 && from < n)
        tabs_flex_paint(lv_obj_get_child(strip, from), false);
    if (to >= 0 && to < n)
        tabs_flex_paint(lv_obj_get_child(strip, to), true);
}

void wt_tabs_flex_help(lv_obj_t *strip, int cur, bool open)
{
    wt_tabs_flex_select(strip, open ? cur : -1, open ? -1 : cur, false);
}

lv_obj_t *wt_trail(lv_obj_t *scr, const char *icon, const char *path,
                   bool stop)
{
    int x = WT_LANE_X;
    if (icon && *icon) {
        // The mark comes off the Latin face; IoskeleyMono carries no
        // FontAwesome, same as the tab strip's marks. On the erase gate it
        // is full WT_STOP -- the red reaches the breadcrumb before the
        // sentence -- and a danger never wears the accent's flag.
        lv_obj_t *ic = wt_lbl(scr, icon, x, 0, wt_font23(),
                              stop ? WT_STOP : wt_accent());
        if (!stop) lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        lv_obj_update_layout(ic);
        lv_obj_set_y(ic, WT_CHROME_STRIP_Y +
                         (WT_BR_H - lv_obj_get_height(ic)) / 2);
        x += lv_obj_get_width(ic) + 10;
    }
    const lv_font_t *f = chrome23(path);
    lv_obj_t *l = wt_lbl(scr, path, x, 0, f, WT_DIM);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_y(l, WT_CHROME_STRIP_Y +
                    (WT_BR_H - lv_font_get_line_height(f)) / 2);
    // 752 unless a [ ? ] is already on this strip, in which case the trail
    // stops 12 short of it. They share one 30px row and the mark is pinned by
    // its RIGHT edge, so a trail running the full lane overlaps the brackets
    // whatever its text says -- the box is what collides, not the words. A
    // page that wants both builds the TAB FIRST.
    int right = 752;
    const uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (lv_obj_get_user_data(c) != (void *)WT_HELPTAB_TAG) continue;
        right = lv_obj_get_x(c) - 12;
        break;
    }
    if (right < x + 40) right = x + 40;
    lv_obj_set_width(l, right - x);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

// The dot both band idioms share: 8px, `col`, optionally breathing. The
// breath is ease_in_out opacity -- the device's "alive" language -- and it
// dies with the object, so no screen has to remember it on the way out.
// SIZE, never transform_scale: a transform puts LVGL on the layer path, which
// allocates a buffer the size of the object every frame and spins in
// LV_ASSERT_MALLOC when that fails -- the walk once hung on the single frame
// that flipped a lamp. Size plus a translate is the same picture and
// allocates nothing.
static void an_dot_size(void *v, int32_t d)
{
    lv_obj_set_size(v, d, d);
    lv_obj_set_style_radius(v, d / 2 + 1, 0);
}

// The breathe: opacity and size together, in phase, forever. An opacity-only
// breath on a 7px dot came off the bench as "not pulsing"; the size is what
// makes it visible across the room. The growth is re-centred with translate
// styles (layout-free), and the anchor says which corner the object's own
// geometry pins: a set_pos dot grows down-right, an ALIGN_TOP_RIGHT dot grows
// down-left, and the translate leans against that so the dot swells about its
// middle.
void wt_dot_breathe(lv_obj_t *d, int base, int grow, bool anchor_right)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);

    lv_anim_set_exec_cb(&a, an_opa);
    lv_anim_set_values(&a, 100, 255);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, an_dot_size);
    lv_anim_set_values(&a, base, base + grow);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, an_tx);
    lv_anim_set_values(&a, 0, anchor_right ? grow / 2 : -grow / 2);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, an_ty);
    lv_anim_set_values(&a, 0, -grow / 2);
    lv_anim_start(&a);
}

static lv_obj_t *band_dot(lv_obj_t *scr, int x, int y, lv_color_t col,
                          bool pulse)
{
    lv_obj_t *d = lv_obj_create(scr);
    lv_obj_remove_style_all(d);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, 4, 0);
    lv_obj_set_style_bg_color(d, col, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    if (pulse) wt_dot_breathe(d, 8, 3, false);
    return d;
}

lv_obj_t *wt_standing(lv_obj_t *scr, const char *txt, lv_color_t col,
                      bool pulse)
{
    action_bar_ensure(scr);
    const lv_font_t *f = chrome18(txt);
    int lh = lv_font_get_line_height(f);
    int y  = WT_ACTION_Y + (WT_ACTION_H - lh) / 2;
    band_dot(scr, WT_ACT_X, y + (lh - 8) / 2, col, pulse);
    // The DOT carries the colour; the sentence takes the accent whenever that
    // colour was the caution. A standing line is read, and amber is a mark.
    const bool warn = lv_color_eq(col, WT_WARN);
    lv_obj_t *l = wt_lbl(scr, txt, WT_ACT_X + 8 + 10, y, f,
                         warn ? wt_accent() : col);
    if (warn) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    // One line, in HEIGHT as well as width: the statement shares the band
    // with the exit, and LONG_DOT only elides once the box stops growing.
    // 592 is where the exit's lane begins.
    lv_obj_set_width(l, 592 - 12 - (WT_ACT_X + 18));
    lv_obj_set_height(l, lh);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

// ---- the [ ? ] explainer tab ----

static bool s_help_seen;
static void (*s_help_persist)(void);

bool wt_help_seen(void)               { return s_help_seen; }
void wt_help_seen_set(bool seen)      { s_help_seen = seen; }
void wt_help_seen_hook(void (*persist)(void)) { s_help_persist = persist; }

// The SECOND first-run bit: has a row that GROWS ever been opened. Same shape
// as the one above and for the same reason -- the plus is a new idiom on this
// device and a mark nobody has pressed teaches nothing.
static bool s_row_seen;
static void (*s_row_persist)(void);
bool wt_row_seen(void)                { return s_row_seen; }
void wt_row_seen_set(bool seen)       { s_row_seen = seen; }
void wt_row_seen_hook(void (*persist)(void)) { s_row_persist = persist; }
void wt_row_seen_mark(void)
{
    if (s_row_seen) return;
    s_row_seen = true;
    if (s_row_persist) s_row_persist();
}

typedef struct {
    lv_obj_t *tab;    // what breathes
    lv_obj_t *hint;   // the band line, or NULL
    bool pulse;       // there is something unread: the breathe is not the
                      // first-run teach and does not end with it
} wt_help_ctx_t;

static void help_free_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

// Runs BEFORE the page's own handler (registered first), so by the time the
// page swaps its lane in, the hint is already gone for good.
static void help_first_cb(lv_event_t *e)
{
    wt_help_ctx_t *c = lv_event_get_user_data(e);
    if (s_help_seen) return;
    s_help_seen = true;
    if (c->tab && !c->pulse) {
        lv_anim_delete(c->tab, an_opa);
        lv_obj_set_style_opa(c->tab, LV_OPA_COVER, 0);
    }
    if (c->hint) lv_obj_add_flag(c->hint, LV_OBJ_FLAG_HIDDEN);
    if (s_help_persist) s_help_persist();
}

lv_obj_t *wt_help_tab(lv_obj_t *scr, const char *hint,
                      lv_event_cb_t cb, void *ud)
{
    return wt_help_tab_n(scr, hint, 0, cb, ud);
}

lv_obj_t *wt_help_tab_n(lv_obj_t *scr, const char *hint, int unread,
                        lv_event_cb_t cb, void *ud)
{
    // The divider that holds the mark off the real tabs: it is not a third
    // section and the 1px line is what says so.
    lv_obj_t *dv = lv_obj_create(scr);
    lv_obj_remove_style_all(dv);
    lv_obj_set_pos(dv, 668, 75);
    lv_obj_set_size(dv, 1, 20);
    lv_obj_set_style_bg_color(dv, WT_DIV, 0);
    lv_obj_set_style_bg_opa(dv, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dv, LV_OBJ_FLAG_SCROLLABLE);

    // Brackets off the mono face -- they are ASCII -- and the mark off the
    // Latin face, which is where FontAwesome lives.
    //
    // DECIDED: the mark is font23, not the font14 every other mark on this
    // device wears. It shipped at 14 and came off the bench as too small to
    // see and too small to aim at -- the same report the content tab LABELS
    // got when they were 18, and this tab sits in the same 30px strip beside
    // them. So the mark takes the tab rung, chrome23, and the brackets stay
    // mono18 punctuation a rung below it exactly as they do on a content tab.
    // Measured: the glyph goes 11x17 -> 17x25 in a strip 30 tall.
    const lv_font_t *bf = wt_font_mono18();
    const lv_font_t *mf = wt_font23();
    lv_point_t bs, ms;
    lv_text_get_size(&bs, "[", bf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&ms, WT_ICON_WHAT, mf, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    // The count, when there is one. Its own label at the brackets' rung, so
    // it reads as part of the mark rather than a number parked beside it.
    char cnt[8] = {0};
    lv_point_t cs = {0, 0};
    if (unread > 0) {
        snprintf(cnt, sizeof cnt, "%d", unread > 99 ? 99 : unread);
        lv_text_get_size(&cs, cnt, bf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    // The strip ends at WT_LANE_X + 620 = 668, which is where the divider is,
    // so this tab's left edge is not free to travel: [ ? 3 ] measured 78 wide
    // and landed on 674, six pixels of air. The bigger mark spends exactly
    // those six, so the air around it comes back out of the gap -- 4 reads
    // right against a 17px glyph anyway, where 6 was air around an 11px one.
    const int gap = 4, pad = 8;
    int w = 2 * pad + 2 * bs.x + 2 * gap + ms.x + (cs.x ? cs.x + gap : 0);

    lv_obj_t *b = lv_obj_create(scr);
    lv_obj_remove_style_all(b);
    // Pinned by the RIGHT edge: the mark's rendered width moves with the
    // accent's glyph metrics, and a computed left edge drifts.
    lv_obj_set_pos(b, 752 - w, WT_CHROME_STRIP_Y);
    lv_obj_set_user_data(b, (void *)WT_HELPTAB_TAG);
    lv_obj_set_size(b, w, WT_BR_H);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    // The drawing is capped by the strip it sits in; the TARGET is not. 12
    // takes a 63x30 tab to 87x54, which is what wt_help_chip already gives a
    // mark half this one's size, and it costs the layout nothing.
    lv_obj_set_ext_click_area(b, 12);
    wt_tap_feedback(b);

    // All three parts wear the accent in EVERY state: unlike a content tab
    // its brackets never dim, because it is always available.
    lv_obj_t *lb = wt_lbl(b, "[", 0, 0, bf, wt_accent());
    lv_obj_add_flag(lb, WT_FLAG_ACCENT);
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, pad, 0);
    lv_obj_t *mk = wt_lbl(b, WT_ICON_WHAT, 0, 0, mf, wt_accent());
    lv_obj_add_flag(mk, WT_FLAG_ACCENT);
    lv_obj_align(mk, LV_ALIGN_LEFT_MID, pad + bs.x + gap, 0);
    int rx = pad + bs.x + gap + ms.x + gap;
    if (cs.x) {
        lv_obj_t *nb = wt_lbl(b, cnt, 0, 0, bf, wt_accent());
        lv_obj_add_flag(nb, WT_FLAG_ACCENT);
        lv_obj_align(nb, LV_ALIGN_LEFT_MID, rx, 0);
        rx += cs.x + gap;
    }
    lv_obj_t *rb = wt_lbl(b, "]", 0, 0, bf, wt_accent());
    lv_obj_add_flag(rb, WT_FLAG_ACCENT);
    lv_obj_align(rb, LV_ALIGN_LEFT_MID, rx, 0);

    wt_help_ctx_t *c = lv_calloc(1, sizeof *c);
    if (c) {
        c->tab = b;
        // DECIDED: the tab breathes whenever it has something UNREAD, not only
        // until the first open ever. It pulsed once, on the first [ ? ] an
        // owner ever met, and was still forever after -- so [ ? 3 ] drew the
        // count and then sat there, which is a badge you have to be looking at
        // to notice. The attention dot on a content tab has answered the same
        // question since it was filed from the bench as "not pulsing", and it
        // answers a GLANCE. This is the same statement, so it is the same
        // motion: 100..255 over 1200ms ease in out, the values wt_dot_breathe
        // uses, rather than the 71..230 this one had of its own. The size and
        // translate halves of a dot's breathe do not come with it -- growing a
        // tab in a 30px strip moves the brackets, and the pixels are spent.
        //
        // It stops on its own. The count comes from kiss_terms_unread at
        // build, the explainer swaps the screen, and coming back rebuilds the
        // tab with whatever is left; at zero there is no animation to delete.
        const bool teach = !s_help_seen;   // never opened, on this device
        c->pulse = unread > 0;
        if (teach || c->pulse) {
            // Motion 19.
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, b);
            lv_anim_set_exec_cb(&a, an_opa);
            lv_anim_set_values(&a, 100, 255);
            lv_anim_set_duration(&a, 1200);
            lv_anim_set_playback_duration(&a, 1200);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
        // The band line is the TEACH half and only that: it says what the
        // mark is, which an owner who has opened one already knows.
        if (teach && hint && *hint) {
            action_bar_ensure(scr);
            const lv_font_t *hf = chrome18(hint);
            int lh = lv_font_get_line_height(hf);
            c->hint = wt_lbl(scr, hint, WT_ACT_X,
                             WT_ACTION_Y + (WT_ACTION_H - lh) / 2, hf,
                             WT_DIM);
            lv_obj_set_width(c->hint, 592 - 12 - WT_ACT_X);
            lv_obj_set_height(c->hint, lh);
            lv_label_set_long_mode(c->hint, LV_LABEL_LONG_DOT);
        }
        lv_obj_add_event_cb(b, help_first_cb, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(b, help_free_cb, LV_EVENT_DELETE, c);
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

// The headline rung. 34 is "the answer, once" and it is the only place on the
// device that face is spent, so it is worth a measurement rather than a
// LONG_DOT: a headline that elides has lost the sentence the page exists to
// say. It drops ONE rung and reports, which says the copy is 34 characters
// too long -- there is no lane to widen, the lane is the screen.
// The headline's own line box is 38 at mono34 and it starts at 118, so the
// paragraph cannot sit at 152 any more -- it did, and the two overlapped by
// 4px on all three [ ? ] pages the moment the rung went up.
#define WT_EXPLAIN_PARA_Y 168

static const lv_font_t *explain_head_font(const char *s, int w)
{
    if (s && *s && mono_can(s)) {
        lv_point_t sz;
        lv_text_get_size(&sz, s, wt_font_mono34(), 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (sz.x <= w) return wt_font_mono34();
        WT_FIT_GAVE_UP("head", s, w,
                       lv_font_get_line_height(wt_font_mono34()));
    }
    return chrome28(s);
}

// The paragraph rung: 28, two lines at 690, one rung down if it needs three.
// Two lines is not a layout preference -- the facts under it are pinned at
// 200 and a third line is what walks into them.
static const lv_font_t *explain_para_font(const char *s, int w, int lines)
{
    if (!s || !*s) return chrome28(s);
    const lv_font_t *f = chrome28(s);
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y <= lines * lv_font_get_line_height(f)) return f;
    WT_FIT_GAVE_UP("para", s, w, lines * lv_font_get_line_height(f));
    return chrome23(s);
}

// The headline is a SENTENCE, and its stop wears the accent like every other
// one on the page. A label holds a single colour, so the stop is a second
// label butted against the end of the first -- which needs the headline to
// actually FIT its lane, because the position is measured from the text.
//
// When it does not fit, nothing happens and the label keeps LV_LABEL_LONG_DOT:
// an ellipsised headline ends in dots rather than a stop, and there is nothing
// there to colour. Same reasoning as everywhere else in this file -- the
// fallback is the shape that was already correct, not a guess.
static lv_obj_t *head_stop(lv_obj_t *scr, const char *txt, int x, int y,
                           const lv_font_t *f, int lane)
{
    lv_point_t whole;
    lv_text_get_size(&whole, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    // It does not fit its lane: keep the label, which ellipsises. A headline
    // that ends in dots has no stop to colour, and two objects here would only
    // overlap -- which is exactly what a first attempt at this did, a "." label
    // dropped on top of a full width one, reported by the TEXT check.
    if (whole.x > lane) return NULL;
    lv_obj_t *sg = spans_new(scr, x, y, lane);
    lv_obj_set_style_text_color(sg, WT_INK, 0);
    lv_obj_set_style_text_font(sg, f, 0);
    spans_fill(sg, txt, NULL);
    return sg;
}

// Every paragraph on this device is drawn by this, so the accent stop is one
// path rather than a habit each screen has to remember. Declared here because
// the explainers are built well above it.
static lv_obj_t *body_spans_hi(lv_obj_t *par, const char *txt, int x, int y,
                               const lv_font_t *f, int w, const char *hi);

int wt_facts_height(const wt_fact_t *facts, int n)
{
    int h = 0;
    for (int i = 0; i < n && facts; i++) {
        const int lh = LV_MAX(
            lv_font_get_line_height(chrome28(facts[i].cap)),
            lv_font_get_line_height(chrome23(facts[i].val)));
        h += lh + 14;
    }
    return h ? h - 14 : 0;      // no pad under the last row
}

static void explain_to(lv_obj_t *scr, const char *headline, const char *para,
                       const char *hi, const wt_fact_t *facts, int n,
                       int bottom)
{
    const lv_font_t *hf = explain_head_font(headline, WT_LANE_W);
    // The headline is a sentence and its stop wears the accent too, which
    // needs it drawn as spans. That costs LV_LABEL_LONG_DOT, so it is only
    // taken when the headline measurably FITS -- and the label with its
    // ellipsis is what happens when it does not.
    lv_obj_t *h = head_stop(scr, headline, WT_LANE_X, 118, hf, WT_LANE_W);
    if (!h) {
        h = wt_lbl(scr, headline, WT_LANE_X, 118, hf, WT_INK);
        lv_obj_set_width(h, WT_LANE_W);
        // HEIGHT TOO. LONG_DOT only elides once the box stops growing, so a
        // headline with a width and no height does not ellipsise -- it wraps,
        // and the paragraph pinned at 152 is then printed straight through its
        // second line. Which is exactly what a longer SIGN headline did the
        // hour it was written. Same lesson as every other pinned one-liner.
        lv_obj_set_height(h, lv_font_get_line_height(hf));
        lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    }

    // mono28, the reading rung. This was chrome18, then chrome23, and it is
    // 28 now because 28 is what a SENTENCE is set in on this device -- the
    // ladder is four rungs with one job each and a [ ? ] page's paragraph is
    // the same kind of thing as a definition's.
    wt_read_measure(headline);
    wt_read_measure(para);
    const lv_font_t *pf = explain_para_font(para, 690, 2);
    // ONE path, highlight or not. This used to be two: a spangroup when the
    // page named a term and a plain label otherwise, and the accent stop was
    // added to neither -- which is why every [ ? ] page on the device was
    // missing it while the twelve takeover pages had it. A paragraph is a
    // paragraph.
    body_spans_hi(scr, para, WT_LANE_X, WT_EXPLAIN_PARA_Y, pf, 690, hi);
    lv_point_t ps;
    lv_text_get_size(&ps, para, pf, 0, 0, 690, LV_TEXT_FLAG_NONE);
    wt_widow_measure(para, pf, 690);

    // The facts start where the paragraph ends, and never above the line TWO
    // paragraph lines would reach: the caption lane holds still whether this
    // page's paragraph used one line or both, so the three [ ? ] pages an
    // owner meets do not each put their facts somewhere different.
    //
    // The floor is computed from the paragraph's OWN face, not written down.
    // It was 200, which was two chrome23 lines plus the pad; the paragraph is
    // 28 now and 200 became a number that meant nothing, with the headline's
    // 38px line box landing on top of it.
    //
    // THREE facts is what this fits at 28. 246 + 3 * 46 + 32 = 416, so a
    // fourth crosses WT_CONTENT_BOTTOM and the CONTENT check says so. Every
    // page in the pass carries three.
    const int para_lines = 2;
    int y = WT_EXPLAIN_PARA_Y + ps.y + 14;
    const int floor_y = WT_EXPLAIN_PARA_Y +
                        para_lines * lv_font_get_line_height(pf) + 14;
    if (y < floor_y) y = floor_y;
    // ...and then the block HANGS from the band rather than resting on the
    // paragraph. It rested on the paragraph, which put two facts on the KEF
    // screen with 27px of glass under them and 43px of glass over them, and
    // the bench asked for them "slightly moved down, close to the lower
    // band". Every explainer on the device had the same gap, in the amount
    // its own paragraph did not use.
    //
    // Hanging is not less consistent than resting -- it is the same promise
    // measured from the other end, and it is the end an owner's eye actually
    // lands on, because the band under it does not move. The floor above
    // still wins if a long paragraph would otherwise be printed through.
    const int hang = bottom - WT_FACT_BAND_GAP - wt_facts_height(facts, n);
    if (hang > y) y = hang;
    wt_facts(scr, y, facts, n);
}

void wt_explain_hi(lv_obj_t *scr, const char *headline, const char *para,
                   const char *hi, const wt_fact_t *facts, int n)
{
    explain_to(scr, headline, para, hi, facts, n, WT_CONTENT_BOTTOM);
}

void wt_explain_to(lv_obj_t *scr, const char *headline, const char *para,
                   const wt_fact_t *facts, int n, int bottom)
{
    explain_to(scr, headline, para, NULL, facts, n, bottom);
}

// The rows themselves, so a screen whose top band is already a card can
// draw the identical ones under it. See the header.
int wt_facts(lv_obj_t *scr, int y, const wt_fact_t *facts, int n)
{
    return wt_facts_in(scr, WT_LANE_X, y, WT_LANE_W, facts, n);
}

int wt_facts_in(lv_obj_t *par, int x, int y, int w,
                const wt_fact_t *facts, int n)
{
    lv_obj_t *scr = par;               /* the rows' parent, page or card */
    const int right = x + w;
    // A fact with a mark indents every caption, so the column stays a column
    // whether one row carries an icon or all of them do.
    bool marks = false;
    for (int i = 0; i < n && facts; i++)
        if (facts[i].icon) marks = true;
    const int cap_x = marks ? x + 38 : x;

    for (int i = 0; i < n && facts; i++) {
        // The CAPTION is the big one, and the value beside it is a rung
        // smaller. It shipped the other way round -- caption and mark at 23,
        // value at 28 -- and came off the bench as "the text to the right
        // cannot be bigger than the text on the left with the icons".
        //
        // The bench is right and the reason is what the row IS. A caption
        // names the fact and its mark is part of the name; the value answers
        // it. Setting the answer larger than the question made the accent
        // caption and its icon read as chrome hung off a grey headline, and
        // the mark -- which rule 4 says every row carries -- ended up the
        // smallest thing in a row it is supposed to open.
        // The biggest rung the caption FITS at, floor 23. English captions
        // are three or four short words and sit at 28; a translated one runs
        // half again as long -- WENN SIE ES VERGESSEN, NICHT GESPEICHERT --
        // and 296px of mono28 is about fifteen characters. Pinned at 28 those
        // shipped ellipsised, which says nothing and looks like nothing.
        //
        // 23 is the floor and not a rung below it, because 23 is what the
        // VALUE is set at: a caption that dropped further would put the big
        // type back on the right, which is the thing this row was just
        // rebuilt to stop. So the pair is "caption bigger" in English and
        // "caption equal" where the language is long, and never the other way
        // round.
        const lv_font_t *cf = chrome28(facts[i].cap);
        {
            lv_point_t cs;
            lv_text_get_size(&cs, facts[i].cap, cf, 2, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            if (cs.x > WT_FACT_CAP_W - 4) cf = chrome23(facts[i].cap);
        }
        if (facts[i].icon) {
            // Its own label, never composed into the caption: an icon in a
            // chrome string falls out of the mono face and drags the whole
            // label down a rung.
            const lv_color_t mc = col_or(facts[i].icon_col, wt_accent());
            // The mark rides the caption's rung. A 28px icon beside a 23px
            // caption is the same inversion one line up, in the other
            // direction.
            lv_obj_t *ic = wt_lbl(scr, facts[i].icon, x, y - 1,
                                  cf == chrome23(facts[i].cap) ? wt_font23()
                                                               : wt_font28(),
                                  mc);
            // Only an accent mark repaints with the theme. A caution's amber
            // is a severity and never becomes the accent's colour.
            if (lv_color_eq(mc, wt_accent()))
                lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        }
        lv_obj_t *cap = wt_lbl(scr, facts[i].cap, cap_x, y, cf,
                               wt_accent());
        lv_obj_add_flag(cap, WT_FLAG_ACCENT);
        lv_obj_set_style_text_letter_space(cap, 2, 0);
        // 214 wide, ONE line, no wrapping: a caption that would wrap gets
        // shorter copy for that locale. The lane never widens, and the
        // height is what makes LONG_DOT elide instead of stacking.
        //
        // MEASURED FIRST, through the same sink CUT uses. LVGL rewrites the
        // label's own text to insert the dots, so a gate walking the finished
        // tree finds a string exactly one lane wide and no evidence at all.
        // The caption went from mono21 to mono23 in this pass and the lane
        // did not, which took WHAT IT SHARES from fitting to "WHAT IT SH..."
        // with nothing anywhere saying so.
        wt_sub_measure("cap", facts[i].cap, cf, 2, WT_FACT_CAP_W - 4);
        lv_obj_set_width(cap, WT_FACT_CAP_W);
        lv_obj_set_height(cap, lv_font_get_line_height(cf));
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
        const lv_font_t *vf = chrome23(facts[i].val);
        // 14 of gutter, the pad every other pair on this device sits on. The
        // value started where the caption's box ended, so a caption using its
        // whole lane touched the value beside it.
        const int vx = cap_x + WT_FACT_CAP_W + 14;
        wt_sub_measure("fact", facts[i].val, vf, 0, right - vx);
        // Centred in the caption's line box rather than sharing its top: the
        // two faces are a rung apart now, and a smaller label pinned to the
        // same y sits high enough to read as a superscript.
        const int vdy = (lv_font_get_line_height(cf) -
                         lv_font_get_line_height(vf)) / 2;
        lv_obj_t *val = wt_lbl(scr, facts[i].val, vx, y + vdy, vf,
                               WT_MUT);
        lv_obj_set_width(val, right - vx);
        lv_obj_set_height(val, lv_font_get_line_height(vf));
        lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
        // The pitch follows the TALLER of the two, whichever that is. It
        // followed the value while the value was the big one, and that is a
        // pin to whichever face happens to be larger today: swapping the two
        // rungs would otherwise have stacked every row 5px into the one above.
        const int lh = LV_MAX(lv_font_get_line_height(cf),
                              lv_font_get_line_height(vf));
        y += lh + 14;
    }
    return y;
}

void wt_explain(lv_obj_t *scr, const char *headline, const char *para,
                const wt_fact_t *facts, int n)
{
    wt_explain_hi(scr, headline, para, NULL, facts, n);
}

// ---- the in-place definition ----

#define WT_DEF_MAX    6
#define DEF_CAP_W   210    // the caption lane, fixed -- "FIRST ADDRESS" at
                           // chrome23 ls2 measures 205
#define DEF_VAL_X   238    // WT_LINE_PAD + 210 + 14
#define DEF_ARR_W    26    // the arrow's lane, fixed
#define DEF_HEAD_PAD 18    // the open row's head, down from the top

enum { DEF_CLOSED, DEF_OPEN, DEF_GHOST };

typedef struct {
    wt_def_t   def;
    lv_obj_t  *row, *cap, *lamp, *val, *sub, *arrow, *rail, *plain, *term;
    lv_obj_t  *rule;
    int        val_x;      // DEF_VAL_X, plus the lamp's lane when it has one
    // A CYCLE row's mark does not live in the pinned right lane. It sits
    // immediately after the value it changes, because that lane is where the
    // chevron lives and a chevron means "this opens a screen": the bench read
    // the settings ADDRESS TYPE row as the KEYS one and could not tell that
    // one of them changes the setting. So the promise travels with the thing
    // it promises about, and the sub-line gets the whole right end back --
    // which is the same finding, filed as "text too close to the icons".
    bool       cyc;        // the mark is a LOOP, not the pinned chevron
    bool       grows;      // a DEFINITION row: its mark is + / -, not a chevron
    int        vw_big;     // the value's width at the closed/open font
    int        vw_ghost;   // ...and at the ghost font
    int        mark_w;     // the LOOP's own width, so a "?" chip can clear it
    // The value's face per state, decided at build. The caller's strings are
    // free to live on its stack -- labels copy them, and nothing here reads
    // a def's pointer after wt_def_list returns.
    const lv_font_t *vf_closed, *vf_open, *vf_ghost;
} wt_defrow_t;

typedef struct {
    int  n;
    int  open;                        // -1: everything closed
    void (*on_change)(int, void *);
    void *ud;
    wt_defrow_t r[WT_DEF_MAX];
} wt_defs_t;

static void defs_free_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

// One row's dress for one state. Fonts, colours and visibility only --
// heights and y belong to the animation, and the font swap happens HERE, on
// the animation's ready, never mid-flight (motion 22).
static void def_apply(wt_defs_t *d, int k, int mode)
{
    wt_defrow_t *r = &d->r[k];

    const lv_font_t *vf = mode == DEF_OPEN  ? r->vf_open
                        : mode == DEF_GHOST ? r->vf_ghost
                                            : r->vf_closed;
    lv_obj_set_style_text_font(r->val, vf, 0);
    if (lv_obj_check_type(r->val, &lv_spangroup_class)) {
        // The address idiom: the head span grey, the tail span lit. A ghost
        // dims both and gives up its ACCENT flag, exactly as the arrow does,
        // or the next theme change would relight a ghost's tail.
        lv_span_t *hd = lv_spangroup_get_child(r->val, 0);
        lv_span_t *tl = lv_spangroup_get_child(r->val, 1);
        if (hd) lv_style_set_text_color(lv_span_get_style(hd),
                                        mode == DEF_GHOST ? WT_DIM : WT_MUT);
        if (tl) lv_style_set_text_color(lv_span_get_style(tl),
                                        mode == DEF_GHOST ? WT_DIM
                                                          : wt_accent());
        if (mode == DEF_GHOST) lv_obj_remove_flag(r->val, WT_FLAG_ACCENT);
        else                   lv_obj_add_flag(r->val, WT_FLAG_ACCENT);
        lv_spangroup_refresh(r->val);
    } else {
        // A caution VALUE takes the accent: its lamp is the amber, and the
        // lamp is what the eye lands on first anyway.
        lv_color_t vc = col_or(r->def.val_col, WT_INK);
        const bool lifted = lv_color_eq(vc, WT_WARN);
        if (lifted) vc = wt_accent();
        // ...and it must be REPAINTED, because it is the accent now. A caution
        // that is lifted into the theme's colour and then not flagged is
        // correct once and stale for every theme after -- TESTNET and "not
        // real bitcoin" both sat in the old accent on a settings page that
        // had just changed it, with the chevrons beside them repainted.
        if (lifted && mode != DEF_GHOST) lv_obj_add_flag(r->val, WT_FLAG_ACCENT);
        else                             lv_obj_remove_flag(r->val, WT_FLAG_ACCENT);
        lv_obj_set_style_text_color(r->val, mode == DEF_GHOST ? WT_DIM : vc, 0);
    }
    lv_obj_set_style_text_color(r->cap, mode == DEF_GHOST ? WT_DIM : WT_MUT,
                                0);

    // The sub is the closed row's extra; open hides it because the definition
    // says more, and a ghost hides it because a ghost is a name, not a row.
    if (r->sub) {
        if (mode == DEF_CLOSED) lv_obj_remove_flag(r->sub, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_add_flag(r->sub, LV_OBJ_FLAG_HIDDEN);
    }

    // The arrow: the accent everywhere except ghost, where it is furniture --
    // and the FLAG moves with the colour, or the next accent change would
    // repaint a ghost's arrow back to life.
    if (r->arrow) {
        if (mode == DEF_GHOST) {
            lv_obj_remove_flag(r->arrow, WT_FLAG_ACCENT);
            lv_obj_set_style_text_color(r->arrow, WT_DIV, 0);
        } else {
            lv_obj_add_flag(r->arrow, WT_FLAG_ACCENT);
            lv_obj_set_style_text_color(r->arrow, wt_accent(), 0);
        }
    }

    // Vertical: a closed or ghost row CENTRES its head; only an open row
    // top-pads. Top-padding a ghost crops it -- the spec's own warning.
    if (mode == DEF_OPEN) {
        lv_obj_update_layout(r->val);
        int vh = lv_font_get_line_height(vf);
        int cy = DEF_HEAD_PAD +
                 (vh - lv_font_get_line_height(
                           lv_obj_get_style_text_font(r->cap, 0))) / 2;
        lv_obj_align(r->cap, LV_ALIGN_TOP_LEFT, WT_LINE_PAD, cy);
        if (r->lamp)
            lv_obj_align(r->lamp, LV_ALIGN_TOP_LEFT, DEF_VAL_X,
                         DEF_HEAD_PAD + (vh - 8) / 2);
        lv_obj_align(r->val, LV_ALIGN_TOP_LEFT, r->val_x, DEF_HEAD_PAD);
        if (r->arrow) {
            if (r->cyc)
                lv_obj_align(r->arrow, LV_ALIGN_TOP_LEFT,
                             r->val_x + r->vw_big + 14, DEF_HEAD_PAD);
            else
                lv_obj_align(r->arrow, LV_ALIGN_TOP_RIGHT, -WT_LINE_PAD,
                             DEF_HEAD_PAD);
        }
    } else {
        lv_obj_align(r->cap, LV_ALIGN_LEFT_MID, WT_LINE_PAD, 0);
        if (r->lamp) lv_obj_align(r->lamp, LV_ALIGN_LEFT_MID, DEF_VAL_X, 0);
        lv_obj_align(r->val, LV_ALIGN_LEFT_MID, r->val_x, 0);
        if (r->sub && mode == DEF_CLOSED)
            lv_obj_align(r->sub, LV_ALIGN_RIGHT_MID,
                         (r->cyc || !r->arrow)
                             ? -WT_LINE_PAD
                             : -(WT_LINE_PAD + DEF_ARR_W + 20), 0);
        if (r->arrow) {
            if (r->cyc) {
                const int vw = mode == DEF_GHOST ? r->vw_ghost : r->vw_big;
                lv_obj_align(r->arrow, LV_ALIGN_LEFT_MID,
                             r->val_x + vw + 14, 0);
            } else {
                lv_obj_align(r->arrow, LV_ALIGN_RIGHT_MID, -WT_LINE_PAD, 0);
            }
        }
    }

    // The open dressing: the pressed-accent wash, the 2px rail, the body.
    bool open = mode == DEF_OPEN;
    lv_obj_set_style_bg_opa(r->row, open ? 18 : LV_OPA_TRANSP, 0);
    if (r->rail) {
        if (open) lv_obj_remove_flag(r->rail, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(r->rail, LV_OBJ_FLAG_HIDDEN);
    }
    if (!open) {
        if (r->plain) lv_obj_add_flag(r->plain, LV_OBJ_FLAG_HIDDEN);
        if (r->term)  lv_obj_add_flag(r->term, LV_OBJ_FLAG_HIDDEN);
    }
}

// Motion 20's path, shared by every height and travel in the transition.
static void def_anim(lv_obj_t *var, lv_anim_exec_xcb_t exec, int32_t from,
                     int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&a, 205, 717, 307, 1024);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void def_h_done(lv_anim_t *a)
{
    lv_obj_t *row = a->var;
    lv_obj_t *list = lv_obj_get_parent(row);
    wt_defs_t *d = lv_obj_get_user_data(list);
    if (!d) return;
    int k = (int)lv_obj_get_index(row);
    def_apply(d, k, d->open < 0 ? DEF_CLOSED
                                : (k == d->open ? DEF_OPEN : DEF_GHOST));
}

static void def_tap_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    lv_obj_t *list = lv_obj_get_parent(row);
    wt_defs_t *d = lv_obj_get_user_data(list);
    if (!d) return;
    int k = (int)lv_obj_get_index(row);
    // A GO row leaves the page rather than opening in place.
    if (d->r[k].def.go) { d->r[k].def.go(e); return; }
    wt_def_list_open(list, d->open == k ? -1 : k);
}

void wt_def_list_open(lv_obj_t *list, int idx)
{
    wt_defs_t *d = lv_obj_get_user_data(list);
    if (!d || idx == d->open || idx >= d->n) return;
    d->open = idx;

    int y = 0;
    for (int k = 0; k < d->n; k++) {
        wt_defrow_t *r = &d->r[k];
        int mode = idx < 0 ? DEF_CLOSED : (k == idx ? DEF_OPEN : DEF_GHOST);
        int h = mode == DEF_OPEN   ? wt_def_h_open(d->n)
              : mode == DEF_GHOST  ? wt_def_h_ghost()
              : r->def.closed_h    ? r->def.closed_h
                                   : wt_def_h_closed(d->n);

        // Every row's height moves in the SAME tick -- the open one growing,
        // the rest collapsing. Real height and real y, not translates: the
        // gates read settled positions, and settled is what these leave.
        def_anim(r->row, an_h, lv_obj_get_height(r->row), h, def_h_done);
        def_anim(r->row, an_y, lv_obj_get_y(r->row), y, NULL);

        // Motion 23. A chevron TURNS as the row opens; a plus does not turn,
        // it becomes a minus -- rotating a plus 90 degrees produces a plus,
        // and rotating it any other amount produces a mistake. The glyph
        // swaps and fades back in on the same beat.
        if (r->arrow && r->grows) {
            lv_label_set_text(r->arrow, mode == DEF_OPEN ? LV_SYMBOL_MINUS
                                                         : LV_SYMBOL_PLUS);
            def_anim(r->arrow, an_opa, 0, 255, NULL);
        } else if (r->arrow) {
            def_anim(r->arrow, an_rot,
                     lv_obj_get_style_transform_rotation(r->arrow, 0),
                     mode == DEF_OPEN ? 900 : 0, NULL);
        }

        // The definition body rides in behind the growing row (motion 21):
        // unhidden now, clipped by the still-short row, drawn in on a 90ms
        // delay. The row clips its children, so nothing leaks onto a ghost.
        if (mode == DEF_OPEN && r->plain) {
            lv_obj_remove_flag(r->plain, LV_OBJ_FLAG_HIDDEN);
            if (r->term) lv_obj_remove_flag(r->term, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *body[2] = { r->plain, r->term };
            for (int i = 0; i < 2; i++) {
                if (!body[i]) continue;
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, body[i]);
                lv_anim_set_duration(&a, 260);
                lv_anim_set_delay(&a, 90);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_set_values(&a, 9, 0);
                lv_anim_set_exec_cb(&a, an_ty);
                lv_anim_start(&a);
                lv_obj_set_style_opa(body[i], LV_OPA_TRANSP, 0);
                lv_anim_set_values(&a, 0, 255);
                lv_anim_set_path_cb(&a, lv_anim_path_linear);
                lv_anim_set_exec_cb(&a, an_opa);
                lv_anim_start(&a);
            }
        }
        y += h;
    }
    if (d->on_change) d->on_change(idx, d->ud);
}

static lv_obj_t *def_list_build(lv_obj_t *scr, const wt_def_t *defs, int n,
                                bool still)
{
    if (n > WT_DEF_MAX) n = WT_DEF_MAX;

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, WT_LANE_X, WT_LANE_Y);
    lv_obj_set_size(list, WT_LANE_W, WT_DEF_LANE);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    wt_defs_t *d = lv_calloc(1, sizeof *d);
    if (!d) { lv_obj_delete(list); return NULL; }
    d->n = n;
    d->open = -1;
    lv_obj_set_user_data(list, d);
    lv_obj_add_event_cb(list, defs_free_cb, LV_EVENT_DELETE, d);

    const int oh = wt_def_h_open(n);
    int ry = 0;
    for (int k = 0; k < n; k++) {
        wt_defrow_t *r = &d->r[k];
        r->def = defs[k];
        const int ch = defs[k].closed_h ? defs[k].closed_h
                                        : wt_def_h_closed(n);

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, ry);
        lv_obj_set_size(row, WT_LANE_W, ch);
        ry += ch;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        // The open wash: the accent at seven percent. The colour is kept
        // fresh by the FILL flag; the opacity is this row's own and stays
        // zero until def_apply raises it.
        lv_obj_set_style_bg_color(row, wt_accent(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(row, WT_FLAG_ACCENT_FILL);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        wt_line_press(row);
        lv_obj_add_event_cb(row, def_tap_cb, LV_EVENT_CLICKED, NULL);
        r->row = row;

        // The head, on the four fixed lanes: caption 210, value never
        // yielding, sub taking what is left and yielding first, arrow 26.
        // Every pinned label gets ONE line of height as well as its lane --
        // LONG_DOT only elides once the box stops growing, so a lane without
        // a height is a lane that wraps into the row below it.
        const lv_font_t *cf = chrome23(defs[k].cap);
        wt_sub_measure("label", defs[k].cap, cf, 2, DEF_CAP_W);
        r->cap = wt_lbl(row, defs[k].cap, 0, 0, cf, WT_MUT);
        lv_obj_set_style_text_letter_space(r->cap, 2, 0);
        lv_obj_set_width(r->cap, DEF_CAP_W);
        lv_obj_set_height(r->cap, lv_font_get_line_height(cf));
        lv_label_set_long_mode(r->cap, LV_LABEL_LONG_DOT);

        r->val_x = DEF_VAL_X;
        if (defs[k].lamp) {
            r->lamp = band_dot(row, DEF_VAL_X, 0, defs[k].lamp_col,
                               defs[k].lamp_pulse);
            // The lamp's glow, RECEIVE's own: the same colour, wider than
            // the dot, read as light rather than as a second ring.
            lv_obj_set_style_shadow_color(r->lamp, defs[k].lamp_col, 0);
            lv_obj_set_style_shadow_width(r->lamp, 10, 0);
            lv_obj_set_style_shadow_opa(r->lamp, 140, 0);
            r->val_x += 8 + 12;
        }

        // A CLOSED row's value sits at the caption's own rung, not one above
        // it. It was chrome28 against a chrome23 caption and a chrome23 tab
        // strip, so on SETTINGS the answer was bigger than the row that names
        // it AND bigger than the tab naming the page -- reported from the
        // bench looking at BACKUP, where CHECKED and SD CARD were the largest
        // words on screen after the title. The value keeps its rank the way it
        // always did, in INK against the caption's MUT; it does not need a
        // second channel saying the same thing louder.
        //
        // The OPEN row keeps 28. There the value IS the subject of an expanded
        // row, and the step up is what says so.
        r->vf_closed = chrome23(defs[k].val);
        r->vf_open   = chrome28(defs[k].val);
        r->vf_ghost  = chrome18(defs[k].val);
        const lv_font_t *vf = r->vf_closed;
        lv_point_t vs;
        if (defs[k].val_tail && *defs[k].val_tail) {
            // The address idiom: a spangroup, head then lit tail, the same
            // two-span shape accent_walk repaints everywhere else. It eats
            // presses out of the box, inside a row whose whole box is the
            // control -- so it takes none.
            lv_obj_t *sg = lv_spangroup_create(row);
            lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
            lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);
            lv_obj_set_style_text_font(sg, vf, 0);
            lv_span_set_text(lv_spangroup_new_span(sg), defs[k].val);
            lv_span_set_text(lv_spangroup_new_span(sg), defs[k].val_tail);
            lv_spangroup_refresh(sg);
            r->val = sg;
            lv_point_t ts;
            lv_text_get_size(&vs, defs[k].val, vf, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            lv_text_get_size(&ts, defs[k].val_tail, vf, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            vs.x += ts.x;
        } else {
            r->val = wt_lbl(row, defs[k].val, 0, 0, vf, WT_INK);
            lv_text_get_size(&vs, defs[k].val, vf, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
        }

        // ABSENT, not dimmed, on a row that opens nothing. The chevron is a
        // promise -- "this takes you somewhere" -- and a row of pure readout
        // (the build, the radio, the noise source) has nowhere to take
        // anybody. It was drawn unconditionally, so THIS DEVICE's four fact
        // rows each wore an arrow to nothing.
        const bool leads = defs[k].go || defs[k].plain || defs[k].mark;
        r->cyc    = defs[k].mark != NULL;
        r->vw_big = vs.x;
        {
            lv_point_t gs;
            lv_text_get_size(&gs, defs[k].val ? defs[k].val : "", r->vf_ghost,
                             0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            r->vw_ghost = gs.x;
        }
        r->arrow = NULL;
        r->grows = defs[k].plain && *defs[k].plain && !defs[k].mark;
        if (leads) {
            // A row that GROWS takes PLUS. The chevron promises a screen --
            // it is why SETTINGS resolves its in-place cycles with LOOP --
            // and a definition row wearing one was read from the bench as a
            // door to somewhere.
            const char *mk = defs[k].mark ? defs[k].mark
                           : r->grows     ? LV_SYMBOL_PLUS
                                          : LV_SYMBOL_RIGHT;
            r->arrow = wt_lbl(row, mk, 0, 0, wt_font23(), wt_accent());
            lv_obj_add_flag(r->arrow, WT_FLAG_ACCENT);
            lv_obj_update_layout(r->arrow);
            r->mark_w = lv_obj_get_width(r->arrow);
            lv_obj_set_style_transform_pivot_x(
                r->arrow, lv_obj_get_width(r->arrow) / 2, 0);
            lv_obj_set_style_transform_pivot_y(
                r->arrow, lv_obj_get_height(r->arrow) / 2, 0);
        }

        if (defs[k].sub && *defs[k].sub) {
            // A row's sub is measured against the CLOSED value, the widest
            // layout it shares a line with. A CYCLE row's mark has already
            // left the right lane and taken its place beside the value, so
            // the sub starts after the MARK and runs to the row's own margin;
            // a chevron row still stops short of the pinned lane.
            //
            // 20 of air, not 8. Eight was measured against LV_SYMBOL_RIGHT --
            // a narrow chevron whose glyph leaves most of its 26px lane empty
            // -- and LV_SYMBOL_LOOP fills the lane, so the same number that
            // looked generous beside a chevron rendered "not real bitcoin"
            // hard against the loop.
            int lane = !leads
                     ? WT_LANE_W - WT_LINE_PAD - (r->val_x + vs.x + 16)
                     : r->cyc
                     ? WT_LANE_W - WT_LINE_PAD
                       - (r->val_x + vs.x + 14 + r->mark_w + 20)
                     : WT_LANE_W - WT_LINE_PAD - DEF_ARR_W - 20
                       - (r->val_x + vs.x + 16);
            if (lane > 40) {
                const lv_font_t *sf = chrome23(defs[k].sub);
                // Measured, like every other pinned one-liner. This lane was
                // the LAST unmeasured one, and it found "forgets every s..."
                // the same hour it was added: the def sub's lane is what the
                // value and its mark leave behind, so a longer VALUE (ENABLED
                // to DISABLED) shortens it under copy that fitted a moment ago.
                wt_sub_measure("sub", defs[k].sub, sf, 0, lane);
                lv_color_t sc = col_or(defs[k].sub_col, WT_DIM);
                const bool lift = lv_color_eq(sc, WT_WARN);
                if (lift) sc = wt_accent();
                r->sub = wt_lbl(row, defs[k].sub, 0, 0, sf, sc);
                // Same lift, same requirement: see the value above.
                if (lift) lv_obj_add_flag(r->sub, WT_FLAG_ACCENT);
                lv_obj_set_width(r->sub, lane);
                lv_obj_set_height(r->sub, lv_font_get_line_height(sf));
                lv_obj_set_style_text_align(r->sub, LV_TEXT_ALIGN_RIGHT, 0);
                lv_label_set_long_mode(r->sub, LV_LABEL_LONG_DOT);
            }
        }

        // The open dressing, built once and hidden: the rail rides the row's
        // height as a percentage, so the animation never has to know it.
        r->rail = lv_obj_create(row);
        lv_obj_remove_style_all(r->rail);
        lv_obj_set_pos(r->rail, 0, 0);
        lv_obj_set_size(r->rail, 2, lv_pct(100));
        lv_obj_set_style_bg_color(r->rail, wt_accent(), 0);
        lv_obj_set_style_bg_opa(r->rail, LV_OPA_COVER, 0);
        lv_obj_add_flag(r->rail, WT_FLAG_ACCENT_FILL);
        lv_obj_add_flag(r->rail, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->rail, LV_OBJ_FLAG_CLICKABLE);

        if (defs[k].plain && *defs[k].plain) {
            // The definition: plain sentence first, the real term
            // underneath, never the term alone. Geometry computed against
            // the OPEN height, where it will be seen.
            const lv_font_t *pf = chrome23(defs[k].plain);
            int hy = DEF_HEAD_PAD +
                     lv_font_get_line_height(chrome28(defs[k].val)) + 14;
            r->plain = wt_lbl(row, defs[k].plain, WT_LINE_PAD, hy, pf,
                              WT_INK);
            lv_obj_set_style_text_opa(r->plain, 200, 0);
            lv_obj_set_width(r->plain, 646);
            lv_label_set_long_mode(r->plain, LV_LABEL_LONG_WRAP);
            lv_obj_add_flag(r->plain, LV_OBJ_FLAG_HIDDEN);
            lv_point_t ps;
            lv_text_get_size(&ps, defs[k].plain, pf, 0, 0, 646,
                             LV_TEXT_FLAG_NONE);
            wt_read_measure(defs[k].plain);
            wt_widow_measure(defs[k].plain, pf, 646);
            if (defs[k].term && *defs[k].term) {
                const int ty2 = hy + ps.y + 14;
                r->term = wt_term_line(row, defs[k].term_label, defs[k].term,
                                       WT_LINE_PAD, ty2, 646);
                if (r->term) {
                    lv_obj_update_layout(r->term);
                    // Against the ROW'S FLOOR, not against the card. A three
                    // line definition still fits a 216px row and lands on the
                    // term line -- nothing overlaps until the row is OPEN and
                    // settled, which is the state no sweep had ever measured.
                    const int bottom = ty2 + lv_obj_get_height(r->term);
                    if (bottom > oh) wt_term_report(defs[k].plain, bottom, oh);
                    lv_obj_add_flag(r->term, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // The rule under the row, aligned to its bottom so the height
        // animation carries it; drawn in on the entry beat below. The LAST
        // row draws none: a separator separates, and under the final row it
        // read from the bench as a stray underline beneath the value.
        r->rule = NULL;
        if (k < n - 1) {
            r->rule = wt_line_rule(row, 0, 0, WT_LANE_W);
            lv_obj_align(r->rule, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }

        def_apply(d, k, DEF_CLOSED);

        // The entry: rise and fade on the row's beat, the rule drawing in
        // behind it -- KEYS/RECEIVE's own welcome, motions 1 to 3. A settled
        // build skips all of it; the rule is born full width, so skipping
        // the draw-in IS the settled state.
        if (!still) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, row);
            lv_anim_set_duration(&a, 260);
            lv_anim_set_delay(&a, 42 * k);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_values(&a, 9, 0);
            lv_anim_set_exec_cb(&a, an_ty);
            lv_anim_start(&a);
            lv_obj_set_style_opa(row, LV_OPA_TRANSP, 0);
            lv_anim_set_values(&a, 0, 255);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_set_exec_cb(&a, an_opa);
            lv_anim_start(&a);
            if (r->rule) wt_line_rule_draw(r->rule, 42 * k + 110, 320);
        }
    }
    return list;
}

lv_obj_t *wt_def_list(lv_obj_t *scr, const wt_def_t *defs, int n)
{
    return def_list_build(scr, defs, n, false);
}

lv_obj_t *wt_def_list_still(lv_obj_t *scr, const wt_def_t *defs, int n)
{
    return def_list_build(scr, defs, n, true);
}

void wt_def_row_read(lv_obj_t *list, int idx)
{
    wt_defs_t *d = list ? lv_obj_get_user_data(list) : NULL;
    if (!d || idx < 0 || idx >= d->n) return;
    lv_obj_t *lamp = d->r[idx].lamp;
    if (lamp) lv_obj_add_flag(lamp, LV_OBJ_FLAG_HIDDEN);
}

void wt_def_list_on_change(lv_obj_t *list, void (*cb)(int, void *), void *ud)
{
    wt_defs_t *d = lv_obj_get_user_data(list);
    if (!d) return;
    d->on_change = cb;
    d->ud = ud;
}

lv_obj_t *wt_def_row_help(lv_obj_t *list, int k, lv_event_cb_t cb, void *ud)
{
    wt_defs_t *d = lv_obj_get_user_data(list);
    if (!d || k < 0 || k >= d->n) return NULL;
    wt_defrow_t *r = &d->r[k];
    // After the VALUE, not the caption: the caption lane is fixed at 210 and
    // the captions that earn a "?" already fill it, so a chip there costs the
    // words it explains. The value is short by contract.
    lv_point_t vs;
    lv_text_get_size(&vs, r->def.val, r->vf_closed, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    // The sub's lane starts 16 past the value, so the chip's 30px sits inside
    // the sub's BOX even when their pixels never touch -- the same by-box
    // overlap wt_row_wide_help documents. The sub is right aligned, so it
    // gives the chip room from its left edge and nothing moves.
    // Past the CYCLE MARK, when there is one. The mark moved out of the pinned
    // right lane and into the space immediately after the value, which is
    // where this chip already stood -- so on the one row that has both (address
    // type) the loop was drawn underneath the "?" and vanished. The mark comes
    // first because it belongs to the VALUE; the chip explains the idea.
    //
    // THIRTY, not ten. At ten the loop and the "?" read as one two-part
    // control -- "does it have to be so fucking close to the cycle icon" is
    // the bench looking at ADDRESS TYPE, the only row on the device that
    // carries both. Fourteen still separates the value from its loop, because
    // the loop belongs to the value it cycles; a full chip of air separates
    // the loop from the chip, because the chip belongs to the row. The sub
    // yields the difference for free -- mx is subtracted from its width
    // below.
    const int mx = r->cyc ? r->mark_w + 30 : 0;
    // ...and the sub yields the mark's width as well. The shrink below was
    // written for a chip sitting straight after the value and was never told
    // the chip had moved right past the loop, so on the ONE row that has both
    // the sub's box still reached back under it -- reported as "Native SegWit"
    // and "?" sharing 8px the moment the SIGNER tab lost a row and the lane
    // grew enough for the text to reach that far.
    if (r->sub) {
        // LAID OUT FIRST. lv_obj_get_width on a label whose width was set a
        // moment ago and never laid out returns 0, so `w > 40` was false and
        // this whole shrink has been a no-op since it was written -- which is
        // why the "?" sat inside the sub's box on the one row that has both,
        // reported as "Native SegWit" and "?" sharing 8x18.
        lv_obj_update_layout(r->sub);
        int w = lv_obj_get_width(r->sub) - (14 + 30 + 10 - 16) - mx;
        if (w > 40) lv_obj_set_width(r->sub, w);
    }
    lv_obj_t *chip = wt_help_chip(r->row, 0, 0, wt_accent(), cb, ud);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, r->val_x + vs.x + 14 + mx, 0);
    return chip;
}

// ---- shape 6: the outcome (see kiss_theme.h) ----

void wt_outcome(lv_obj_t *scr, const wt_outcome_t *o)
{
    const lv_color_t col = o->ok ? WT_OK : WT_WARN;
    const lv_font_t *hf = chrome28(o->headline);
    const int hh = lv_font_get_line_height(hf);

    // The lamp: a 14px dot in an 18px glow, the state's own colour in all
    // four themes -- RECEIVE's discipline, at the size a verdict earns.
    lv_obj_t *d = lv_obj_create(scr);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 14, 14);
    lv_obj_set_style_radius(d, 7, 0);
    lv_obj_set_style_bg_color(d, col, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(d, col, 0);
    lv_obj_set_style_shadow_width(d, 18, 0);
    lv_obj_set_style_shadow_opa(d, 140, 0);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(d, WT_LANE_X, 138 + (hh - 14) / 2);

    lv_obj_t *h = wt_lbl(scr, o->headline, WT_LANE_X + 14 + 16, 138, hf,
                         WT_INK);
    lv_obj_set_width(h, WT_LANE_W - 14 - 16);
    lv_obj_set_height(h, hh);
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);

    lv_obj_t *p = wt_lbl(scr, o->para, WT_LANE_X, 196, chrome18(o->para),
                         WT_MUT);
    lv_obj_set_width(p, 690);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);

    if (!o->f1c || !o->f1v) return;
    wt_line_rule(scr, WT_LANE_X, 262, WT_LANE_W);
    const struct { const char *cap, *val; } facts[2] = {
        { o->f1c, o->f1v }, { o->f2c, o->f2v },
    };
    for (int i = 0; i < 2 && facts[i].cap && facts[i].val; i++) {
        int y = i == 0 ? 280 : 322;
        const lv_font_t *cf = chrome18(facts[i].cap);
        lv_obj_t *cap = wt_lbl(scr, facts[i].cap, WT_LANE_X, y, cf, WT_MUT);
        lv_obj_set_style_text_letter_space(cap, 2, 0);
        lv_obj_set_width(cap, 168);
        lv_obj_set_height(cap, lv_font_get_line_height(cf));
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
        const lv_font_t *vf = chrome18(facts[i].val);
        lv_obj_t *val = wt_lbl(scr, facts[i].val, WT_LANE_X + 168 + 14, y,
                               vf, WT_INK);
        lv_obj_set_width(val, WT_LANE_W - 168 - 14);
        lv_obj_set_height(val, lv_font_get_line_height(vf));
        lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    }
}

// ---- shape 3: the grid you read aloud (see kiss_theme.h) ----

void wt_word_grid(lv_obj_t *scr, const char *const *words, int n, int first)
{
    if (n > 12) n = 12;
    const lv_font_t *nf = wt_font_mono18();
    const lv_font_t *wf = wt_font_mono28();
    for (int k = 0; k < n; k++) {
        const int cx = 62 + (k / 4) * 232;
        const int cy = 126 + (k % 4) * 62;
        // Clamped, so the device compiler can PROVE the buffer: a word number
        // is 1..24 by construction, but "first + k + 1" is unbounded to the
        // truncation gate, and that gate is the one lane that has caught
        // every silent cut in this file's history.
        int wnum = first + k + 1;
        if (wnum < 1) wnum = 1;
        if (wnum > 99) wnum = 99;
        char num[8];
        snprintf(num, sizeof num, "%d", wnum);
        // The number right-aligned in its 30px lane, dim: it keeps the
        // owner's place and then gets out of the word's way.
        lv_obj_t *nl = wt_lbl(scr, num, cx, 0, nf, WT_DIM);
        lv_obj_set_width(nl, 30);
        lv_obj_set_height(nl, lv_font_get_line_height(nf));
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_y(nl, cy + (lv_font_get_line_height(wf) -
                               lv_font_get_line_height(nf)) / 2);
        // BIP39 words are ASCII by construction, so the mono face is safe by
        // the same argument as a fingerprint; the guard stays for the day a
        // caller hands this something else.
        lv_obj_t *wl = wt_lbl(scr, words[first + k], cx + 30 + 16, cy,
                              chrome28(words[first + k]), WT_INK);
        lv_obj_set_height(wl, lv_font_get_line_height(wf));
    }
}

void wt_sheet_dots(lv_obj_t *scr, int n, int cur)
{
    for (int i = 0; i < n; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_pos(d, 524 + i * 14, WT_CHROME_STRIP_Y + (WT_BR_H - 6) / 2);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        if (i == cur) {
            lv_obj_set_style_bg_color(d, wt_accent(), 0);
            lv_obj_add_flag(d, WT_FLAG_ACCENT_FILL);
        } else {
            lv_obj_set_style_bg_color(d, WT_DIM, 0);
        }
    }
}

// ---- shape 4: the gate (see kiss_theme.h) ----

void wt_gate(lv_obj_t *scr, const wt_gate_t *g)
{
    const lv_color_t mark = g->stop ? WT_STOP : WT_WARN;
    // ONE amber thing on a walk-back gate, and it is the caution -- not the
    // headline, which is a statement of what the screen is for. The scan key
    // gate had three amber runs (the sentence, the warn line and the slide
    // label) and the bench counted them: "out of the three yellow text
    // phrases only the caution one should be yellow". The MARK keeps the
    // severity colour, so the amber still arrives before the words.
    //
    // A STOP gate is untouched. There the sentence IS the danger, and
    // WT_STOP_INK on it is the whole point of the shape.
    const lv_color_t ink  = g->stop ? WT_STOP_INK : wt_accent();

    // The mark and the sentence share a baseline at the top of the lane. The
    // mark takes the FULL danger colour; the sentence the readable tint --
    // full WT_STOP as text is the one combination that vibrates.
    lv_obj_t *ic = wt_lbl(scr, g->mark ? g->mark : WT_ICON_ERASE, WT_LANE_X,
                          0, wt_font23(), mark);
    lv_obj_update_layout(ic);
    const lv_font_t *sf = chrome28(g->sentence);
    lv_obj_set_y(ic, 124 + (lv_font_get_line_height(sf) -
                            lv_obj_get_height(ic)) / 2);
    lv_obj_t *s = wt_lbl(scr, g->sentence,
                         WT_LANE_X + lv_obj_get_width(ic) + 14, 124, sf,
                         ink);
    if (!g->stop) lv_obj_add_flag(s, WT_FLAG_ACCENT);
    lv_obj_set_width(s, WT_LANE_W - lv_obj_get_width(ic) - 14);
    lv_obj_set_height(s, lv_font_get_line_height(sf));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);

    // ---- everything from here down was PINNED, and that was the bug -------
    //
    // The paragraph sat at y=176 in chrome18, the rule at 260, and the two
    // facts at 278 and 318, whatever any of them measured. Two things came
    // out of that. The SHOW gate -- one short paragraph, no warn line -- had
    // 36px of nothing under a body set two rungs below the sentence above it,
    // reported from the bench as tiny text on a screen with room to spare.
    // The scan key gate has the opposite problem and its warn line ran into
    // the rule.
    //
    // So: measure, and take the largest rung the WHOLE block fits at. One
    // rung for the paragraph and both facts together, because they are one
    // thing to read and a screen that sets them at sizes chosen separately
    // reads as three decisions. The warn line is not in the ladder -- it was
    // raised to 23 on its own after "THAT TEXT SHOULD BE BIGGER ITS FUCKING
    // TINY" and it does not go back down.
    // 170, 12, 12, 6 -- and every one of those numbers is the arithmetic of
    // fitting a one line paragraph AND a one line warn at the reading rung.
    // At 176 with 18px gaps the block needs 107px of chrome and leaves 53 for
    // the copy, which is one line at mono23 and nothing else: every gate that
    // carries a warn was forced to the floor by the gaps alone. These leave
    // 74, which is exactly a line, the 12px step and the warn.
    const int TOP   = 170;
    const int GAP   = 12;                      // above and below the rule
    const int FGAP  = 6;                       // between the two facts
    const int FLOOR = WT_ACTION_Y_SLIDE - 8;   // nothing may touch the slide
    const int PARA_W = 690;
    const int MARK_W = 34;                     // the tick's lane, fixed
    // A paragraph is OPTIONAL. The SHOW gate has nothing to put in one: its
    // sentence says what is about to happen and its two facts say what that
    // costs and what it does not, so the slot was filled with the copying
    // instruction from a different screen -- "Your seed words appear on this
    // screen." followed by "on paper, in order.", which is an answer to a
    // question the sentence did not ask. Cut, rather than reworded: the screen
    // was already complete without it.
    const bool has_para = g->para && *g->para;
    const bool has_warn = g->warn && *g->warn;
    const lv_font_t *wf = has_warn ? chrome23(g->warn) : NULL;
    const lv_font_t *mkf = wt_font23();

    const lv_font_t *pf = NULL, *f0 = NULL, *f1 = NULL;
    int ph = 0, wh = 0, h0 = 0, h1 = 0;
    for (int rung = 0; rung < 2; rung++) {
        pf = rung ? chrome18(has_para ? g->para : "") : chrome23(has_para ? g->para : "");
        f0 = rung ? chrome18(g->surv) : chrome23(g->surv);
        f1 = rung ? chrome18(g->goes) : chrome23(g->goes);
        ph = 0;
        if (has_para) {
            lv_point_t ps;
            lv_text_get_size(&ps, g->para, pf, 0, 0, PARA_W, LV_TEXT_FLAG_NONE);
            ph = ps.y;
        }
        wh = 0;
        if (has_warn) {
            lv_point_t ws;
            lv_text_get_size(&ws, g->warn, wf, 0, 0, PARA_W - MARK_W,
                             LV_TEXT_FLAG_NONE);
            wh = 12 + ws.y;
        }
        h0 = lv_font_get_line_height(f0);
        h1 = lv_font_get_line_height(f1);
        if (TOP + ph + wh + GAP + GAP + h0 + FGAP + h1 <= FLOOR)
            break;
        // The floor is chrome18, which is where every one of these screens
        // already was. Landing on it means the COPY is too long for the space
        // between the sentence and the slide, which is what the sink is for:
        // silent was the old behaviour and it is what let four text runs sit
        // two rungs small with nothing to say so.
        if (rung == 1 && has_para)
            WT_FIT_GAVE_UP("gate", g->para, PARA_W, FLOOR - TOP);
    }

    // A gate with no paragraph would otherwise hug its sentence and leave the
    // whole bottom of the lane empty, which is what the SHOW gate looked like
    // the moment its paragraph was cut. Drop by a third of the slack, the same
    // move wt_body_para makes and for the same reason: centring outright
    // floats the block away from the sentence it belongs to. The full gates
    // have no slack, so nothing moves on them.
    const int used = ph + wh + GAP + GAP + h0 + FGAP + h1;
    int y = TOP;
    if (FLOOR - TOP > used) y += (FLOOR - TOP - used) / 3;
    if (has_para) {
        lv_obj_t *p = wt_lbl(scr, g->para, WT_LANE_X, y, pf, WT_MUT);
        lv_obj_set_width(p, PARA_W);
        lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
        y += ph;
    }

    if (has_warn) {
        // The one amber line a stop gate may carry: a caution the owner can
        // still walk back and fix -- paper never checked -- sitting beside
        // the red it might spare them.
        //
        // The GLYPH is its own label so it can stay amber while the sentence
        // takes the accent. Composed into one string they could only ever be
        // one colour, and the colour the bench wants on the words is not the
        // colour it wants on the mark.
        lv_obj_t *wm = wt_lbl(scr, LV_SYMBOL_WARNING, WT_LANE_X, y + 12,
                              mkf, WT_WARN);
        lv_obj_update_layout(wm);
        lv_obj_t *w = wt_lbl(scr, g->warn, WT_LANE_X + MARK_W, y + 12, wf,
                             g->stop ? WT_STOP_INK : wt_accent());
        if (!g->stop) lv_obj_add_flag(w, WT_FLAG_ACCENT);
        lv_obj_set_y(wm, y + 12 + (lv_font_get_line_height(wf) -
                                   lv_obj_get_height(wm)) / 2);
        lv_obj_set_width(w, PARA_W - MARK_W);
        lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
        y += wh;
    }

    y += GAP;
    wt_line_rule(scr, WT_LANE_X, y, WT_LANE_W);
    y += GAP;

    // The two lines that ARE the shape: what survives this, and what does
    // not. A TICK and a CROSS, not two captions. WHAT SURVIVES and WHAT DOES
    // NOT were spending a 168px lane on words a mark says without them, and
    // that lane was the reason the answers themselves could not grow: "the
    // old stored copy, once the new one verifies" needs 630px at mono23 and
    // had 522. Both glyphs are already in SYMS, so nothing rebuilds.
    const struct { const char *sym; lv_color_t col; const char *val;
                   const lv_font_t *f; int h; } facts[2] = {
        { LV_SYMBOL_OK,    WT_OK,                          g->surv, f0, h0 },
        { LV_SYMBOL_CLOSE, g->stop ? WT_STOP : WT_MUT,     g->goes, f1, h1 },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *m = wt_lbl(scr, facts[i].sym, WT_LANE_X, y, mkf,
                             facts[i].col);
        lv_obj_update_layout(m);
        lv_obj_set_y(m, y + (facts[i].h - lv_obj_get_height(m)) / 2);
        lv_obj_t *val = wt_lbl(scr, facts[i].val, WT_LANE_X + MARK_W, y,
                               facts[i].f, WT_INK);
        lv_obj_set_width(val, WT_LANE_W - MARK_W);
        lv_obj_set_height(val, facts[i].h);
        lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
        y += facts[i].h + FGAP;
    }
}

// ---- SETTINGS: the full-lane row (see kiss_theme.h) ----
#define WT_WIDE_LX      18    // the label's lane, row local
#define WT_WIDE_LW     250    // ...and its cap. See the header: the gate
                              // compares boxes, not ink.
#define WT_WIDE_CHIP_W 150    // the chip's MINIMUM; a long value widens it.
                              // 190 predates the mono23/28 bump: the value is
                              // left-aligned INSIDE the chip, so the unused
                              // minimum was dead space billed to the sub's
                              // lane -- at 150 the short-value column still
                              // lines up and the sub gets its 40px back.
#define WT_WIDE_CHIP_H  40
#define WT_WIDE_SWATCH  16
#define WT_HELP_CHIP_W  30    // wt_help_chip's own size

lv_obj_t *wt_row_wide(lv_obj_t *scr, int y, const wt_wide_t *r)
{
    const bool inert = r->kind == WT_WIDE_INERT;
    // The def rows' scale, with the locale guard every chrome string carries:
    // caption and sub at mono23, the value at mono28, marks at font23. This
    // was 18/18/21 -- one to two rungs under the KEYS page in the same lane --
    // and the bench read the gap as SETTINGS being fine print. The sub is the
    // page's TEACHING copy ("not real bitcoin", "opens your real keys") and
    // now sits on the same rung the def rows teach at. A translation the mono
    // faces cannot draw falls to the sans family at the same size, never up
    // into the hairline. Copy that no longer fits its lane at this size gets
    // CUT, not shrunk -- the CUT check reports the ellipsis.
    const lv_font_t *lf = chrome23(r->label);
    const lv_font_t *sf = chrome23(r->sub);
    const lv_font_t *vf = r->vf ? r->vf : chrome28(r->val);
    const lv_font_t *cf = wt_font23();       // marks: the def rows' own size

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, WT_WIDE_X, y);
    lv_obj_set_size(row, WT_WIDE_W, WT_WIDE_H);
    // BORDERLESS, like every other row on the device now. This was a card --
    // a WT_PANEL fill, a WT_HAIR edge and a 10px radius -- and a page of six
    // cards is six boxes competing before a word is read. What replaces the
    // edge is a 1px rule UNDER the row and, under a finger, the accent rail
    // and wash wt_line_row wears. Same information, same one-line geometry
    // that lets the page be read straight down the value column; the boxes go.
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (r->cb && !inert) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        wt_line_press(row);
        lv_obj_add_event_cb(row, r->cb, LV_EVENT_CLICKED, r->ud);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    // An inert row is present, stated and dead. With no fill left to keep, what
    // says so is its ink, which the colours below already handle -- and it
    // still gets the rule, because a missing rule would read as a missing row.
    wt_line_rule(scr, WT_WIDE_X, y + WT_WIDE_H, WT_WIDE_W);
    if (r->sev) wt_row_sev(row, r->sev);

    // Caption MUT and sub DIM, the def rows' own ranks: the caption labels a
    // readout, the sub is the quiet sentence under it, and the VALUE is the
    // thing in ink. A stated sub_col (the amber network note, the green
    // verified line) still outranks the default -- a state keeps its colour.
    lv_color_t ink  = inert ? WT_DIM : WT_MUT;
    lv_color_t subc = inert ? WT_DIM : wt_ink_for(col_or(r->sub_col, WT_DIM));
    lv_color_t vcol = inert ? WT_DIM : wt_ink_for(col_or(r->vcol, WT_INK));
    // wt_ink_for hands a caution's WORDS the accent, so either of these can BE
    // the accent -- and then it has to be repainted like everything else that
    // is. The def rows above wear the same lift and needed the same flag.
    const bool sub_acc = lv_color_eq(subc, wt_accent());
    const bool val_acc = lv_color_eq(vcol, wt_accent());

    // THE CONTROL FIRST, so the sub-line's lane can be measured against what
    // is actually there. Sizing the sub to the row and hoping is how a
    // translated value ("DESACTIVADO" for OFF) ends up sitting on the words
    // that explain it.
    int lane_end = WT_WIDE_W - 12;
    lv_point_t vs = { 0, 0 };
    if (r->val && *r->val)
        lv_text_get_size(&vs, r->val, vf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    if (r->kind == WT_WIDE_CYCLE || r->kind == WT_WIDE_CHIP) {
        // The mark is the promise: LOOP advances the value where it stands,
        // RIGHT opens a screen. Measured before the chip is sized, because the
        // two glyphs are not the same width.
        const char *mark = r->kind == WT_WIDE_CYCLE
                         ? LV_SYMBOL_LOOP : LV_SYMBOL_RIGHT;
        lv_point_t cs;
        lv_text_get_size(&cs, mark, cf, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        int sw = r->swatch ? WT_WIDE_SWATCH + 10 : 0;
        // The chip grows LEFTWARDS out of its minimum, taking the room from
        // the sub-line rather than from the page margin: the right edge is
        // where the eye reads the value column down, so it does not move.
        int cw = sw + vs.x + 12 + cs.x + 12;
        if (cw < WT_WIDE_CHIP_W) cw = WT_WIDE_CHIP_W;
        int cx = WT_WIDE_W - 12 - cw;

        lv_obj_t *chip = lv_obj_create(row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_pos(chip, cx, (WT_WIDE_H - WT_WIDE_CHIP_H) / 2);
        lv_obj_set_size(chip, cw, WT_WIDE_CHIP_H);
        // No box. The WHOLE ROW is the control -- it carries the callback and
        // the pressed rail -- so an edge drawn around the value was a second
        // control drawn inside the first, and the page read as boxes inside
        // boxes. What says "this one changes" is the mark beside the value,
        // which is what it always was; the border was only ever holding it.
        lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);   // the row takes the tap
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(chip, (void *)WT_ROW_CTRL_TAG);

        int vx = 0;
        if (r->swatch) {
            lv_obj_t *d = lv_obj_create(chip);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, WT_WIDE_SWATCH, WT_WIDE_SWATCH);
            lv_obj_set_style_radius(d, WT_WIDE_SWATCH, 0);
            lv_obj_set_style_bg_color(d, wt_accent(), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            // The swatch IS the value: it has to follow a theme change, and a
            // fill needs the FILL flag -- the plain accent flag only ever
            // repaints text and would fail here in silence.
            lv_obj_add_flag(d, WT_FLAG_ACCENT_FILL);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(d, LV_ALIGN_LEFT_MID, vx, 0);
            vx += WT_WIDE_SWATCH + 10;
        }
        if (r->val && *r->val) {
            lv_obj_t *v = wt_lbl(chip, r->val, 0, 0, vf, vcol);
            if (val_acc) lv_obj_add_flag(v, WT_FLAG_ACCENT);
            lv_obj_align(v, LV_ALIGN_LEFT_MID, vx, 0);
        }
        lv_obj_t *ch = wt_lbl(chip, mark, 0, 0, cf, wt_accent());
        lv_obj_add_flag(ch, WT_FLAG_ACCENT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -12, 0);

        lane_end = cx - 12;
    } else if (r->kind == WT_WIDE_OPEN) {
        int right = WT_WIDE_W - 12;
        if (r->cb) {
            lv_obj_t *ch = wt_lbl(row, LV_SYMBOL_RIGHT, 0, 0, cf, wt_accent());
            lv_obj_add_flag(ch, WT_FLAG_ACCENT);
            lv_obj_update_layout(ch);
            lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -12, 0);
            right -= lv_obj_get_width(ch) + 12;
        }
        if (r->val && *r->val) {
            lv_obj_t *v = wt_lbl(row, r->val, 0, 0, vf, vcol);
            if (val_acc) lv_obj_add_flag(v, WT_FLAG_ACCENT);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, right - WT_WIDE_W, 0);
            lv_obj_set_user_data(v, (void *)WT_ROW_CTRL_TAG);
            right -= vs.x + 12;
        }
        lane_end = right;
    } else {
        // Inert: no chip and no chevron, because there is nothing to open and
        // nothing to pick. The value sits where a chip's text would have.
        if (r->val && *r->val) {
            lv_obj_t *v = wt_lbl(row, r->val, 0, 0, vf, vcol);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -18, 0);
            lane_end = WT_WIDE_W - 18 - vs.x - 12;
        } else {
            lane_end = WT_WIDE_W - 18;
        }
    }

    // The label, capped and pinned to one line. Both matter: the cap keeps the
    // label's BOX off the value's, and the pin stops a long translation
    // growing a second line into the sub-line beside it.
    wt_sub_measure("label", r->label, lf, 2, WT_WIDE_LW);
    lv_obj_t *l = wt_lbl(row, r->label, WT_WIDE_LX, 0, lf, ink);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_width(l, WT_WIDE_LW);
    lv_obj_set_height(l, lv_font_get_line_height(lf));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_y(l, (WT_WIDE_H - lv_font_get_line_height(lf)) / 2);
    lv_obj_set_user_data(l, (void *)WT_ROW_LABEL_TAG);

    if (r->sub && *r->sub) {
        int sx = WT_WIDE_LX + WT_WIDE_LW;    // 268: where the label's box ends
        int sw = lane_end - sx;
        if (sw < 40) sw = 40;
        wt_sub_measure("sub", r->sub, sf, 0, sw);
        lv_obj_t *s = wt_lbl(row, r->sub, sx, 0, sf, subc);
        if (sub_acc) lv_obj_add_flag(s, WT_FLAG_ACCENT);
        lv_obj_set_width(s, sw);
        lv_obj_set_height(s, lv_font_get_line_height(sf));
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_y(s, (WT_WIDE_H - lv_font_get_line_height(sf)) / 2);
        lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
    }
    return row;
}

lv_obj_t *wt_row_wide_ctrl(lv_obj_t *row)
{
    return wt_tagged(row, WT_ROW_CTRL_TAG);
}

lv_obj_t *wt_row_wide_help(lv_obj_t *row, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *l = wt_tagged(row, WT_ROW_LABEL_TAG);
    if (!l) return NULL;
    const char *txt = lv_label_get_text(l);
    if (!txt) return NULL;

    // Measure the TEXT, never the label. wt_row_wide caps the box at 250 so a
    // long translation ellipsises, so asking the object how wide it is answers
    // 250 for every row in every locale and puts the chip on top of the words.
    // chrome23: the font the row actually set. Measuring at a smaller rung
    // shrank the box under the words and elided the label itself.
    lv_point_t sz;
    lv_text_get_size(&sz, txt, chrome23(txt), 2, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    int lw = sz.x;
    int cap = WT_WIDE_LW - WT_HELP_CHIP_W - 10;
    if (lw > cap) lw = cap;
    // The label gives up the rest of its lane, so the chip lands after the
    // words rather than inside the label's box -- which the overlap gate reads
    // as the "?" and the label sharing pixels, because by box they do.
    lv_obj_set_width(l, lw);

    // The "?" is a TAP TARGET, so it wears the accent like every other one on
    // the device rather than the colour of a row that cannot be pressed. The
    // guard in round_chip flags both its rim and its glyph.
    lv_obj_t *chip = wt_help_chip(row, 0, 0, wt_accent(), cb, ud);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, WT_WIDE_LX + lw + 10, 0);
    return chip;
}


// The explainer under a group: ONE line, at font23, in the page's own margin.
// Never two, and never smaller: a translation that does not fit on one line at
// this size is copy to shorten, not a paragraph to wrap. This is the shape that
// replaced RECOVERY WORDS' three paragraph body -- one muted line per group
// says what the group is for, and the group itself says the rest.
void wt_group_note(lv_obj_t *pane, int rows, const char *txt)
{
    // Prose rides the pass's mono23 rung with the locale guard: the one
    // sentence under a group is a sentence the owner READS, so it sits at
    // the same size as the subs above it. The lane holds 54 mono cells;
    // longer copy gets cut, not shrunk.
    const lv_font_t *f = chrome23(txt);
    lv_obj_t *l = wt_lbl(pane, txt, WT_WIDE_X, WT_WIDE_EXPL_Y(rows), f,
                         WT_MUT);
    lv_obj_set_width(l, WT_WIDE_W);
    lv_obj_set_height(l, lv_font_get_line_height(f));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

// ---- the group that MOVES ----------------------------------------------
// Lifted out of kiss_settings.c when a second page wanted the same chrome.
// Everything here was already page-agnostic -- it only ever reached four
// statics and one tag -- so the move is those five things becoming a context
// the caller owns. Nothing about the timing or the curves changed.
//
// Three jobs, in the order they matter. If any of this ever has to be cut,
// cut from the bottom:
//
//   1. the caution on a flagged row is pointed AT, once, after the row lands.
//      This is the reason a page animates rather than decorating it.
//   2. the destructive group arrives unlike its neighbours -- it rises rather
//      than sliding, slower, without the overshoot, and the pane reddens. The
//      owner knows which group they are in before reading a word.
//   3. the value lands a beat after its label, so the eye reads the setting's
//      NAME and then what it is set to, instead of a grid arriving at once.
//
// Confined to a TAB CHANGE. Walking in from elsewhere, and coming back from
// any screen a row opens, paint settled: a value chip rebuilds the whole page
// on every tap, and three taps to reach SIGNET replaying the entry under the
// owner's finger is not a design, it is a flicker.
#define MO_IN_MS      260   // a row arriving
#define MO_IN_DX       56
#define MO_IN_STEP     38   // and the beat between rows down the group
#define MO_UP_MS      320   // ...except in the stop group, which rises
#define MO_UP_DY       14
#define MO_UP_STEP     44
#define MO_OUT_MS     200   // a row leaving
#define MO_OUT_DX      44
#define MO_OUT_STEP    26
#define MO_FADE_IN     200
#define MO_FADE_UP     240
#define MO_FADE_OUT    160
#define MO_CTRL_MS    220   // the value, a beat behind its label
#define MO_CTRL_DX      7
#define MO_CTRL_LAG    90
#define MO_FLARE_UP   220   // the caution, pointed at once and let go
#define MO_FLARE_DOWN 200   // 200 and not the 420 the handoff drew: at 420 the
                            // page is still moving at 976ms, past the 800 the
                            // kit allows a page change
#define MO_FLARE_LAG  260
#define MO_WASH_MS    280

// A child of a pane that is scenery rather than a row -- the stop group's
// wash. It fades on its own schedule and must not be dealt a row's slide.
static const char WT_PANE_SCENERY[] = "wt_pane_scenery";

void wt_pane_scenery(lv_obj_t *child)
{
    if (child) lv_obj_set_user_data(child, (void *)WT_PANE_SCENERY);
}

static void an_tx(void *v, int32_t x)  { lv_obj_set_style_translate_x(v, x, 0); }
static void an_ty(void *v, int32_t y)  { lv_obj_set_style_translate_y(v, y, 0); }
static void an_opa(void *v, int32_t o) { lv_obj_set_style_opa(v, (lv_opa_t)o, 0); }

// The prototype's ease, cubic-bezier(.17,.84,.32,1.05), typed in as itself.
//
// The handoff calls this "about 5% past the mark, then back" and spends a
// paragraph on how to reproduce the overshoot without LVGL's stock one, which
// is far stronger and reads as bouncy on a page of settings. There is no
// overshoot to reproduce: 1.05 is a CONTROL POINT, not the curve's maximum,
// and the curve it controls peaks at 1.0069 -- four tenths of a pixel on a
// 56px travel, in the browser as much as here. Measured off the frames, the
// row arrives at 25 and stays at 25.
//
// The curve is still not ease_out. It is front loaded: most of the distance is
// gone in the first third, so a row reads as arriving rather than as being
// slid. That is what it is here for, and the overshoot never existed.
static void an_path_settle(lv_anim_t *a)
{
    lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(a, LV_BEZIER_VAL_FLOAT(0.17),
                                 LV_BEZIER_VAL_FLOAT(0.84),
                                 LV_BEZIER_VAL_FLOAT(0.32),
                                 LV_BEZIER_VAL_FLOAT(1.05));
}

// The caution, and it is a RING rather than the dot the prototype draws. The
// rows already carry a warning glyph and an amber rim; a 7px dot growing to 14
// beside them is a detail nobody at the bench would see, and it would be a
// fifth mark on a row that has four. The ring is the row's own edge,
// brightened once. It is a separate object so an interrupted pulse is deleted
// rather than unwound -- there is no half-restored border colour to put back.
static void flare_del(lv_anim_t *a) { lv_obj_delete(a->var); }

static void flare_down(lv_anim_t *a)
{
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, a->var);
    lv_anim_set_exec_cb(&b, an_opa);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&b, MO_FLARE_DOWN);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&b, flare_del);
    lv_anim_start(&b);
}

static void flare(lv_obj_t *row, int delay)
{
    lv_obj_t *ring = lv_obj_create(row);
    lv_obj_remove_style_all(ring);
    lv_obj_set_pos(ring, 0, 0);
    lv_obj_set_size(ring, WT_WIDE_W, WT_WIDE_H);
    lv_obj_set_style_radius(ring, 10, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, WT_WARN, 0);
    lv_obj_set_style_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, an_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, MO_FLARE_UP);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, flare_down);
    lv_anim_start(&a);
}

// The two callbacks that reach past their own object. They go through the
// animation's user_data and never a captured pointer: by the time either
// fires, the pane it meant may already have been deleted, by a second tab tap
// or by the screen closing over it. The CONTEXT outlives both -- it belongs to
// the page's module, not to the objects -- so it is the safe thing to hold.
static void enter_done(lv_anim_t *a)
{
    wt_pane_t *p = lv_anim_get_user_data(a);
    if (p) p->entering = false;
}

static void pane_out_done(lv_anim_t *a)
{
    wt_pane_t *p = a ? lv_anim_get_user_data(a) : NULL;
    if (p && p->pane_out) { lv_obj_delete(p->pane_out); p->pane_out = NULL; }
}

// Asked of the ROW rather than of the page's state, so the pulse and the amber
// card can never disagree about which row it is: WT_SEV_WARN is the page's own
// answer to "does this want reading", and this reads the answer back off the
// object it was written on.
static bool row_wants_reading(lv_obj_t *c)
{
    return lv_obj_get_style_border_opa(c, LV_PART_MAIN) == 77
        && lv_color_eq(lv_obj_get_style_border_color(c, LV_PART_MAIN), WT_WARN);
}

void wt_pane_point(const wt_pane_t *p)
{
    if (!p || !p->pane) return;
    uint32_t n = lv_obj_get_child_count(p->pane);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(p->pane, i);
        if (row_wants_reading(c)) flare(c, 0);
    }
}

void wt_pane_enter(wt_pane_t *p, int dir, bool rise)
{
    if (!p || !p->pane) return;
    uint32_t n = lv_obj_get_child_count(p->pane);
    int k = 0;
    lv_obj_t *last = NULL;       // the last row DEALT, which the latch hangs on
    p->entering = true;

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(p->pane, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);

        // The wash is not a row. It has no lane to come in from: it is the
        // colour of the page changing, so it only deepens.
        if (lv_obj_get_user_data(c) == (void *)WT_PANE_SCENERY) {
            lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
            lv_anim_set_exec_cb(&a, an_opa);
            lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&a, MO_WASH_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
            continue;
        }

        const int delay = k * (rise ? MO_UP_STEP : MO_IN_STEP);
        lv_anim_set_delay(&a, delay);
        if (rise) {
            lv_anim_set_exec_cb(&a, an_ty);
            lv_anim_set_values(&a, MO_UP_DY, 0);
            lv_anim_set_duration(&a, MO_UP_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);   // never overshoot
        } else {
            lv_anim_set_exec_cb(&a, an_tx);
            lv_anim_set_values(&a, dir > 0 ? MO_IN_DX : -MO_IN_DX, 0);
            lv_anim_set_duration(&a, MO_IN_MS);
            an_path_settle(&a);
        }
        // The flag comes off the LAST ROW DEALT, which is not the same as the
        // last child: the wash `continue`s above without an entry animation,
        // and a group whose scenery happened to be built last would leave
        // p->entering true for ever. The symptom of that is silent -- every
        // later tab change would drop its outgoing group instead of sliding
        // it -- so it is guarded here rather than by remembering the order a
        // group builds in.
        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_start(&a);
        last = c;

        lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, rise ? MO_FADE_UP : MO_FADE_IN);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);

        // The value, 90ms behind the label it belongs to. Its own opacity, on
        // top of the row's, so it is still climbing after the row has arrived.
        lv_obj_t *ctrl = wt_row_wide_ctrl(c);
        if (ctrl) {
            lv_obj_set_style_opa(ctrl, LV_OPA_TRANSP, 0);
            lv_anim_set_var(&a, ctrl);
            lv_anim_set_delay(&a, delay + MO_CTRL_LAG);
            lv_anim_set_duration(&a, MO_CTRL_MS);
            lv_anim_start(&a);
            lv_anim_set_exec_cb(&a, an_tx);
            lv_anim_set_values(&a, MO_CTRL_DX, 0);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }

        if (row_wants_reading(c)) flare(c, delay + MO_FLARE_LAG);

        k++;
    }

    // A group of nothing but scenery never settles, so it is already settled.
    if (!last) { p->entering = false; return; }

    // Re-armed on the row that actually finishes last, over its own travel:
    // restarting the same (var, exec_cb) pair replaces the animation LVGL is
    // already running for it rather than adding a second one.
    lv_anim_t z;
    lv_anim_init(&z);
    lv_anim_set_var(&z, last);
    lv_anim_set_exec_cb(&z, rise ? an_ty : an_tx);
    lv_anim_set_values(&z, rise ? MO_UP_DY : (dir > 0 ? MO_IN_DX : -MO_IN_DX), 0);
    lv_anim_set_duration(&z, rise ? MO_UP_MS : MO_IN_MS);
    lv_anim_set_delay(&z, (k - 1) * (rise ? MO_UP_STEP : MO_IN_STEP));
    if (rise) lv_anim_set_path_cb(&z, lv_anim_path_ease_out);
    else      an_path_settle(&z);
    lv_anim_set_user_data(&z, p);
    lv_anim_set_completed_cb(&z, enter_done);
    lv_anim_start(&z);
}

void wt_pane_exit(wt_pane_t *p, int dir)
{
    if (!p) return;
    lv_obj_t *pane = p->pane_out;
    if (!pane) return;
    uint32_t n = lv_obj_get_child_count(pane);
    if (!n) { lv_obj_delete(pane); p->pane_out = NULL; return; }

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(pane, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_delay(&a, i * MO_OUT_STEP);
        lv_anim_set_exec_cb(&a, an_tx);
        lv_anim_set_values(&a, 0, dir > 0 ? -MO_OUT_DX : MO_OUT_DX);
        lv_anim_set_duration(&a, MO_OUT_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        // The slide outlasts the fade, so the pane goes when the LAST row has
        // finished travelling and not when it stopped being visible.
        if (i + 1 == n) {
            lv_anim_set_user_data(&a, p);
            lv_anim_set_completed_cb(&a, pane_out_done);
        }
        lv_anim_start(&a);

        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, MO_FADE_OUT);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    }
}

// Everything moving, stopped, and both lanes accounted for. Deleting a pane
// takes its animations with it -- lv_obj's destructor calls lv_anim_delete --
// which is what makes the callback that would have deleted it never fire.
void wt_pane_stop(wt_pane_t *p)
{
    if (!p) return;
    if (p->pane_out) { lv_obj_delete(p->pane_out); p->pane_out = NULL; }
    p->entering = false;
}

// Every route off a tabbed page drops the screen, and a dozen of them do it
// without a word to the context: the exit, the idle lock, and every row that
// opens a screen of its own. Each deletes the screen and builds its own into
// the same parent, and the pane goes with it while the context still names it.
// The next reopen then deletes a freed object.
//
// So the OBJECT says when it is gone, rather than ten call sites remembering
// to. Comparing against the context is what makes it safe when a page has
// already been replaced: an older pane's delete arrives after the new one has
// been named, matches nothing, and does nothing.
static void pane_gone(lv_event_t *e)
{
    wt_pane_t *p = lv_event_get_user_data(e);
    lv_obj_t  *o = lv_event_get_target(e);
    if (!p) return;
    if (o == p->pane)     p->pane = NULL;
    if (o == p->pane_out) p->pane_out = NULL;
}

static void tabs_gone(lv_event_t *e)
{
    wt_pane_t *p = lv_event_get_user_data(e);
    if (p && lv_event_get_target(e) == p->tabs) p->tabs = NULL;
}

lv_obj_t *wt_pane_new(wt_pane_t *p)
{
    lv_obj_t *o = lv_obj_create(p->scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    // The whole page, and never clipped: a row leaving travels 44px past the
    // lane, and a container sized to the rows would cut it in half. It takes
    // no taps of its own, so the strip and the exit under it stay reachable --
    // LVGL only ever hands a press to a CLICKABLE object and walks past this
    // one to the siblings beneath.
    lv_obj_set_size(o, 800, 480);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, pane_gone, LV_EVENT_DELETE, p);
    return o;
}

void wt_pane_tabs_watch(wt_pane_t *p)
{
    if (p && p->tabs) lv_obj_add_event_cb(p->tabs, tabs_gone, LV_EVENT_DELETE, p);
}

// The whole tab change, which both pages were going to write identically:
// close what the old group owned, drop a group that never finished arriving,
// slide the highlight, build the new one, and send the old one out. `build` is
// the page's own switch over p->tab.
void wt_pane_go(wt_pane_t *p, int tab, bool stop, void (*build)(void))
{
    if (!p || tab == p->tab) return;
    const int dir  = tab > p->tab ? 1 : -1;
    const int from = p->tab;
    p->tab = tab;

    const bool was_moving = p->entering;
    wt_pane_stop(p);
    if (was_moving && p->pane) {
        // Tapping faster than the page settles: the group that never finished
        // arriving is dropped outright rather than sent back out. Sliding a
        // row that has not appeared yet is a flicker, not a transition.
        lv_obj_delete(p->pane);
        p->pane = NULL;
    }

    p->pane_out = p->pane;
    p->pane = wt_pane_new(p);
    build();
    // The new rows were built after the page's own restyle() had already run,
    // so the accent flags on them have never been walked.
    wt_accent_restyle(p->pane);

    if (p->select) p->select(p->tabs, from, tab, stop);
    else           wt_tabs_select(p->tabs, from, tab, stop);
    wt_pane_enter(p, dir, stop);
    wt_pane_exit(p, dir);
}

// ---- swipe: the page as a horizontal deck ---------------------------------
// One gesture, one meaning: move sideways. A tabbed page is a deck of panes;
// a paged list inside a tab extends the deck with its pages. The SIGN file
// list shipped this shape and the owner asked for it everywhere, so the
// plumbing lives here now.

// A finished horizontal stroke, anywhere on the page that owns it.
// LV_EVENT_GESTURE fires mid-press the moment the stroke crosses the indev's
// 50px limit; wait_release then swallows the rest of the press, so the same
// stroke can never also CLICK the row it started on.
int wt_swipe_step(lv_event_t *e)
{
    lv_indev_t *ind = lv_event_get_indev(e);
    if (!ind) return 0;
    lv_dir_t d = lv_indev_get_gesture_dir(ind);
    int step = d == LV_DIR_LEFT ? 1 : d == LV_DIR_RIGHT ? -1 : 0;
    if (step) lv_indev_wait_release(ind);
    return step;
}

// The indev delivers LV_EVENT_GESTURE to the first ancestor WITHOUT
// GESTURE_BUBBLE -- with the whole chain bubbling it walks off the root and
// the event is dropped -- so the page screen clears the flag on itself and
// becomes the terminus.
//
// The screen also takes CLICKABLE back. wt_screen removes it, and the indev
// emits no gesture at all when a press lands on nothing (indev_gesture:
// act_obj NULL, return) -- so on a deck page every patch of empty glass was
// a dead zone for the stroke. NO UNDO is mostly empty glass, which is how
// "can swipe in but not out" came off the bench. Children are hit-tested
// first, so rows and tabs are untouched, and the screen has no click
// handler, so a stray tap on the glass still does nothing.
void wt_swipe_watch(lv_obj_t *scr, lv_event_cb_t cb)
{
    if (!scr) return;
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(scr, cb, LV_EVENT_GESTURE, NULL);
}

// The deck's foot: a one-line count/hint at the lane's left, page dots at its
// right. Dots only when they say something a glance can use -- past
// WT_PAGER_DOTS_MAX the row would be a ruler, so the count line keeps the
// whole lane and carries the position alone (the address list runs to 34
// pages).
lv_obj_t *wt_pager_line(lv_obj_t *p, const char *txt, bool warn, int page,
                        int npages, int bottom)
{
    const bool dots = npages >= 2 && npages <= WT_PAGER_DOTS_MAX;
    const lv_font_t *nf = chrome23(txt);
    // 40 above the band the CALLER names. It was briefly read off the tree
    // instead, and the tree lies at build time: a page reached by BACK is
    // constructed while the page it came from is still waiting on its async
    // delete, so the walk found the OLD screen's slide and pinned this line 54
    // pixels into a list that had none.
    const int py = bottom - 40;
    lv_obj_t *note = wt_lbl(p, txt, WT_LANE_X, py, nf,
                            warn ? WT_WARN : WT_MUT);
    // The dots' lane comes off the note only when dots exist: the sort hint
    // on a sparse list is 4px longer than the shared lane, and DOT ate its
    // last clause without a word from any gate.
    lv_obj_set_width(note, dots ? WT_LANE_W - 150 : WT_LANE_W);
    // Pinned to ONE line: a label allowed to grow is a budget given away.
    lv_obj_set_height(note, lv_font_get_line_height(nf));
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
    // Scenery, all of it: the pager is chrome, and chrome does not ride the
    // slide it drives.
    wt_pane_scenery(note);
    if (!dots) return note;
    const int pitch = 18;
    const int x0 = WT_LANE_X + WT_LANE_W - (npages * pitch - 10);
    for (int i = 0; i < npages; i++) {
        lv_obj_t *d = lv_obj_create(p);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_pos(d, x0 + i * pitch, py + 8);  // centred on the 25px line
        lv_obj_set_style_radius(d, 5, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, i == page ? wt_accent() : WT_DIM, 0);
        if (i == page) lv_obj_add_flag(d, WT_FLAG_ACCENT);
        wt_pane_scenery(d);
    }
    return note;
}

// Rebuild a context's pane and slide it in from the side the flip came from.
// NOT wt_pane_go: that refuses a same-tab call by design, because a tab
// change carries a direction along a strip and a page change has none of its
// own -- the swipe is the direction.
void wt_page_flip(wt_pane_t *ctx, void (*build)(void), int dir)
{
    wt_pane_stop(ctx);
    if (ctx->pane) { lv_obj_delete(ctx->pane); ctx->pane = NULL; }
    ctx->pane = wt_pane_new(ctx);
    build();
    wt_accent_restyle(ctx->pane);
    wt_pane_enter(ctx, dir, false);
}

// The bar on a scrolling list. Four lists wrote this by hand and a fifth wrote
// nothing at all -- and the one that had already been accented was stale on
// every theme change, because accent_walk repaints LV_PART_MAIN and a
// scrollbar is a PART. WT_FLAG_ACCENT_SCROLL is what reaches it.
//
// OPA_50 so it is a tint rather than a stripe, and the caller still decides ON
// or OFF: a bar that appears only after you have scrolled answers the wrong
// question, which is the argument written out in kiss_recv.c.
void wt_list_scrollbar(lv_obj_t *list)
{
    if (!list) return;
    lv_obj_set_style_bg_color(list, wt_accent(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
    lv_obj_add_flag(list, WT_FLAG_ACCENT_SCROLL);
}

// ---- overlays (see kiss_theme.h) ----
lv_obj_t *wt_overlay_box(lv_obj_t *scr, lv_obj_t **scrim_out, int x, int y,
                         int w, int h, int radius, lv_event_cb_t close_cb)
{
    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_70, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    if (close_cb) lv_obj_add_event_cb(scrim, close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(scrim);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, WT_BAR, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, WT_EDGE, 0);
    lv_obj_set_style_shadow_width(box, 40, 0);
    lv_obj_set_style_shadow_offset_y(box, 18, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(box, 140, 0);
    // Clickable with no callback: a tap on the box's own padding is aimed at
    // the box, not past it, so it must not fall through to the scrim's close.
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    if (scrim_out) *scrim_out = scrim;
    return box;
}


lv_obj_t *wt_alert_chip(lv_obj_t *scr, const char *txt,
                        lv_event_cb_t cb, void *ud)
{
    const lv_font_t *lf = wt_font23(), *mf = wt_font14();
    // It stands on the action bar, so make sure there is one. Every screen
    // that grows this chip has an exit too, but the order the two are built in
    // belongs to the caller and this may not depend on it.
    action_bar_ensure(scr);

    lv_point_t is, ls, cs;
    lv_text_get_size(&is, LV_SYMBOL_WARNING, mf, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_text_get_size(&ls, txt, lf, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&cs, LV_SYMBOL_RIGHT, mf, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    int w = is.x + 12 + ls.x + 12 + cs.x;

    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, WT_ACT_X, WT_ACTION_Y);
    lv_obj_set_size(c, w, WT_ACTION_H);
    // No box, like everything else in an action bar now. It was a tinted fill
    // and an amber edge, which is a BOX -- the one shape this look removes --
    // and it was the last one left on the settings page. The mark carries the
    // caution; amber ink says the rest. It still shifts under a finger the way
    // an arrow action does, because it is still a control.
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_translate_x(c, 0, 0);
    lv_obj_set_style_translate_x(c, 5, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *ic = wt_lbl(c, LV_SYMBOL_WARNING, 0, 0, mf, WT_WARN);
    // Placed from ZERO, not from the 18px inset the border used to hold. The
    // inset went with the box and the three parts have to close up behind it,
    // or the chevron sits on top of the last letter of the label.
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    // The MARK is the amber; the words and the chevron are the accent.
    lv_obj_t *l = wt_lbl(c, txt, 0, 0, lf, wt_accent());
    lv_obj_add_flag(l, WT_FLAG_ACCENT);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, is.x + 12, 0);
    lv_obj_t *ch = wt_lbl(c, LV_SYMBOL_RIGHT, 0, 0, mf, wt_accent());
    lv_obj_add_flag(ch, WT_FLAG_ACCENT);
    lv_obj_set_style_text_opa(ch, 180, 0);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
    return c;
}

// A camera viewport: the card, a WT_EDGE edge, and four bracket corners drawn
// OUTSIDE it. Outside is the whole point. On the device the camera writes
// straight to the panel over exactly this rectangle, so anything inside these
// bounds is gone the moment the first frame lands; the brackets live in LVGL's
// own pixels and survive. They are also what tells an empty preview from a
// rendering fault, which is what both camera screens look like in the simulator
// and on a device whose camera failed to start.
lv_obj_t *wt_viewfinder(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *vp = wt_card(scr, x, y, w, h);
    lv_obj_set_style_border_color(vp, WT_EDGE, 0);

    const int L = 16, T = 2, G = 5;      // arm length, thickness, gap from the box
    const int x0 = x - G, y0 = y - G, x1 = x + w + G, y1 = y + h + G;
    // { x, y, w, h } per arm, two arms per corner, clockwise from top left.
    const int arm[8][4] = {
        { x0,     y0,     L, T }, { x0,     y0,     T, L },
        { x1 - L, y0,     L, T }, { x1 - T, y0,     T, L },
        { x1 - L, y1 - T, L, T }, { x1 - T, y1 - L, T, L },
        { x0,     y1 - T, L, T }, { x0,     y1 - L, T, L },
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *a = lv_obj_create(scr);
        lv_obj_remove_style_all(a);
        lv_obj_set_pos(a, arm[i][0], arm[i][1]);
        lv_obj_set_size(a, arm[i][2], arm[i][3]);
        lv_obj_set_style_bg_color(a, WT_EDGE, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    }
    return vp;
}

lv_obj_t *wt_value_card(lv_obj_t *scr, const char *cap, const char *val,
                        int x, int y, int w, bool big)
{
    lv_obj_t *card = wt_card(scr, x, y, w, 0);

    // CENTRED, both of them. The fingerprint reveal builds its own box by hand
    // and centres (kiss_ui.c), this one pinned everything at x=16, and the same
    // eight characters therefore sat in two different places depending on which
    // screen asked -- on the passphrase warning it read as a form field with a
    // wide empty right half. The value needs a width before it can be centred:
    // wt_lbl leaves it content sized, which is its own bounding box, so an
    // alignment inside it would mean nothing.
    // The caption takes the accent, the same fix wt_section already had: it is
    // the eyebrow over a figure, which is furniture, and at font14 in WT_MUT it
    // came off the bench as barely visible. Nine call sites in four files draw
    // FINGERPRINT through here, so this has to be the theme's decision or the
    // same caption reads as four different marks.
    wt_cap_measure(cap);
    lv_obj_t *c = wt_lbl(card, cap, 16, 12, wt_font14(), wt_accent());
    lv_obj_add_flag(c, WT_FLAG_ACCENT);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_width(c, w - 32);
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(c);

    int vy = 12 + lv_obj_get_height(c) + 8;
    // THE FACE IS ASKED, not assumed. The mono faces carry ASCII plus three
    // marks and NOTHING else, so an unconditional wt_font_mono here draws
    // LVGL's placeholder box for every character outside that set -- and the
    // value on this card is a translated WORD as often as it is a fingerprint.
    // Russian was five empty boxes where the dice source should be, on the one
    // screen that says how an owner's keys were made; Swedish read KRYPTERAD
    // S[]KERHETSKOPIA. Fifteen locales put non ASCII through this call.
    //
    // chrome23/chrome28 are the guard the rest of the chrome already uses, and
    // this is exactly what they are for: mono when mono can draw it, the
    // native face when it cannot. A fingerprint, an address and a block number
    // are ASCII and keep the mono they were given; only words fall back, which
    // is the half that was never comparable character by character anyway.
    lv_obj_t *v = wt_lbl(card, val, 16, vy,
                         big ? chrome28(val) : chrome23(val), WT_INK);
    lv_obj_set_style_text_letter_space(v, 2, 0);
    lv_obj_set_width(v, w - 32);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(v);
    // Sized to its content, never to a guess: the caption is translated and the
    // value can be four characters or forty.
    lv_obj_set_size(card, w, vy + lv_obj_get_height(v) + 14);
    return card;
}


// ---- the explainer card ----
// Every "?" on the device opens one of these, and they were all the same thing:
// a centred title over a centred paragraph, floating in the middle of a dimmed
// screen. The design handoff draws them as a page like any other -- title top
// left, the value the card is about in a bordered box, and the prose as two
// claims with coloured rules beside them rather than one block nobody finishes.
//
// The two-column split costs NOTHING in translation, which is the only reason it
// is affordable: the explainer bodies were already written as two or three
// paragraphs separated by a blank line, in all twenty one locales, so this
// splits a string that exists rather than asking for a string that does not.
// Anything past the second paragraph joins the second block, so a three
// paragraph body stays two columns instead of inventing a third.
static void explain_close_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

const char *wt_split_colon(const char *line, char *head, size_t head_len)
{
    if (!line || !head || head_len == 0) return NULL;
    const char *c = NULL;
    size_t skip = 0;
    for (const char *p = line; *p; p++) {
        if (*p == ':') { c = p; skip = 1; break; }
        // U+FF1A FULLWIDTH COLON, EF BC 9A
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC &&
            (unsigned char)p[2] == 0x9A) { c = p; skip = 3; break; }
    }
    if (!c) {
        snprintf(head, head_len, "%s", line);
        return NULL;
    }
    size_t n = (size_t)(c - line);
    while (n && line[n - 1] == ' ') n--;     // "locktime :" in French
    if (n >= head_len) n = head_len - 1;
    lv_memcpy(head, line, n);
    head[n] = 0;
    const char *tail = c + skip;
    while (*tail == ' ') tail++;
    return *tail ? tail : NULL;
}

// ---- how the body is laid out ----
// The old rule was: split at the FIRST blank line, paragraph one left, everything
// else right, both measured against a 330px column. Three faults, and they
// compounded. It never considered the full width lane, so a body that would have
// read at font28 across 704 was measured against 330 and dropped a size. It split
// by position rather than by length, so the home fingerprint card -- whose body
// grows a third paragraph for the escape hint -- put four words in the left
// column and two paragraphs in the right. And the pair share one font by design,
// chosen by the taller half, so those four words rendered at font14 beside a
// column that needed it. That is the "tiny text" the device showed.
//
// Now both arrangements are measured at every size and the first that fits wins.
#define EXP_MAX_PARA 6
#define EXP_FULL_W   704
// Measure against the TEXT lane, not the block. wt_why_block spends 14 on the
// coloured rule and its gutter, so a measurement taken at the block's own width
// comes back short and the last line lands under the OK action. That is not
// hypothetical: measuring the full lane at 704 instead of 690 put the silent
// payment card 19px past WT_CONTENT_BOTTOM in five locales.
#define EXP_RULE_W   14
#define EXP_FULL_TXT (EXP_FULL_W - EXP_RULE_W)

typedef struct {
    const char *p[EXP_MAX_PARA];
    int         n[EXP_MAX_PARA];    // byte length of each
    int         count;
} exp_paras_t;

static void exp_split(const char *body, exp_paras_t *o)
{
    o->count = 0;
    const char *s = body;
    while (s && *s && o->count < EXP_MAX_PARA) {
        const char *brk = strstr(s, "\n\n");
        // The last slot swallows whatever is left, so a seven paragraph string
        // still renders whole rather than losing its tail.
        if (!brk || o->count == EXP_MAX_PARA - 1) {
            o->p[o->count] = s;
            o->n[o->count] = (int)strlen(s);
            o->count++;
            return;
        }
        o->p[o->count] = s;
        o->n[o->count] = (int)(brk - s);
        o->count++;
        s = brk + 2;
    }
}

// Height of paragraphs [a, b) joined by blank lines, at font f and width w.
static int exp_height(const exp_paras_t *ps, int a, int b, const lv_font_t *f, int w)
{
    int h = 0;
    for (int i = a; i < b; i++) {
        lv_point_t sz;
        char buf[512];
        int n = ps->n[i];
        if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
        lv_memcpy(buf, ps->p[i], (size_t)n);
        buf[n] = 0;
        lv_text_get_size(&sz, buf, f, 0, 0, w, LV_TEXT_FLAG_NONE);
        h += sz.y;
        if (i + 1 < b) h += lv_font_get_line_height(f);   // the blank line back
    }
    return h;
}

// Copy paragraphs [a, b) back into one string, blank line separated, so
// wt_why_block sees the same shape the locale wrote.
static void exp_join(const exp_paras_t *ps, int a, int b, char *out, size_t len)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = a; i < b && o + 1 < len; i++) {
        int n = ps->n[i];
        if (o + (size_t)n + 3 >= len) n = (int)(len - o - 3);
        if (n < 0) break;
        lv_memcpy(out + o, ps->p[i], (size_t)n);
        o += (size_t)n;
        if (i + 1 < b && o + 2 < len) { out[o++] = '\n'; out[o++] = '\n'; }
    }
    out[o] = 0;
}


// Lay a body out as ruled blocks in the room between `y` and WT_CONTENT_BOTTOM,
// choosing the arrangement and the font size that read best: full width when the
// text can earn the 704 lane, two balanced columns otherwise, dropping a rung
// before it ever overflows. `sev` colours the first block, WT_MUT the second.
//
// This was the private guts of wt_explain_open and it is public now because the
// explainer cards were not the only screens with a wall of grey text in them.
// Nine more screens had one, and giving each its own hand placed layout is how
// the walls got there in the first place. The explainer card calls this too, so
// there is one body layout on the device rather than ten.
//
// It costs NOTHING in translation: every one of these bodies is already written
// as two or three paragraphs separated by a blank line in all 21 locales, so
// this splits a string that exists rather than asking for one that does not.
// The term line: the real word for what the body just explained in plain
// ones, under the body and never instead of it. Two labels, not one string --
// the LABEL is tracked at ls 2 like every other caption on the device and the
// TERM is not, because tracking a 34 character term is what pushed it onto a
// second line in the draft that tried.
//
// IT NEVER WRAPS. The longest in the set measures 470 of the 496 this leaves,
// so a term that would wrap gets a shorter term, exactly the way a caption
// does. Pinning the height is what makes LONG_DOT elide rather than stack: a
// label with a width and no height grows downward through whatever is under
// it, which is the same lesson as every other one-liner in this file.
//
// The kit stays string-free, so `label` arrives translated. It is the same
// word on every one of these lines -- one key, and the reason it is a
// parameter rather than a constant is that the kit does not read i18n.
// 150 is the LABEL's lane, not the gutter: the word measures 140 at chrome23
// ls 2, so the term would start 10px after it ends and the two read as one
// string -- "TECHNICALACCOUNT" was the first frame this drew. 14 more, the
// same pad every other pair on this device sits on, leaves the longest term
// in the set 482px against the 470 it needs.
#define WT_TERM_LABEL_W 150
#define WT_TERM_GUTTER   14
// ONE OBJECT, holding two labels. A caller has one thing to place, one thing
// to measure against the floor under it and one thing to hide -- the def row
// shows and hides this line on every open and close, and chasing two labels
// through that is how one of them gets left behind.
lv_obj_t *wt_term_line(lv_obj_t *par, const char *label, const char *term,
                       int x, int y, int w)
{
    if (!par || !term || !*term) return NULL;
    const lv_font_t *tf = chrome23(term);
    // The TERM's own face, and only its own. Sizing this box from the taller
    // of the term and its label was tried and is wrong: half the callers pin
    // this line by hand at a y that assumes the term's height, so growing the
    // box a pixel walked those lines into the action band. The one caller
    // that pins by measurement -- wt_explain_open -- measures the children.
    const int h = lv_font_get_line_height(tf);

    lv_obj_t *box = lv_obj_create(par);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    int tx = 0;
    if (label && *label) {
        const lv_font_t *lf = chrome23(label);
        lv_obj_t *l = wt_lbl(box, label, 0, 0, lf, WT_MUT);
        lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_obj_set_width(l, WT_TERM_LABEL_W);
        lv_obj_set_height(l, lv_font_get_line_height(lf));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        tx = WT_TERM_LABEL_W + WT_TERM_GUTTER;
    }
    lv_obj_t *t = wt_lbl(box, term, tx, 0, tf, wt_accent());
    lv_obj_add_flag(t, WT_FLAG_ACCENT);
    lv_obj_set_width(t, w - tx);
    lv_obj_set_height(t, h);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    return box;
}

// The device's body renderer. It used to have a second arrangement -- two
// grey columns with a coloured rule down the side of each, wt_why_block -- and
// that shape is gone: the bench asked for the vertical lines to leave, and the
// screens that wore them are built from a headline, a paragraph and captioned
// fact rows now. What is left is the MEASURING, which was always the valuable
// half: pick the largest rung the copy fits at, and report when even the floor
// will not hold it.
// One run of ordinary body text, with the page's own term lifted into the ink
// if it appears inside this run. Split out because the run is built in two
// places (mid-paragraph and at the end) and the highlight has to happen in
// both.
// An ordinary run of body text carries NO span style, so it inherits the
// spangroup's own text colour. That is what keeps these a drop-in replacement
// for the labels they came from: a caller that reaches for
// lv_obj_set_style_text_color still recolours the whole paragraph, and only
// the stops and the highlighted term hold a colour of their own.
static void span_run(lv_obj_t *sg, const char *txt, const char *hi)
{
    const char *at = hi && *hi ? strstr(txt, hi) : NULL;
    if (!at) {
        lv_span_set_text(lv_spangroup_new_span(sg), txt);
        return;
    }
    if (at > txt) {
        char head[640];
        snprintf(head, sizeof head, "%.*s", (int)(at - txt), txt);
        lv_span_set_text(lv_spangroup_new_span(sg), head);
    }
    lv_span_t *s2 = lv_spangroup_new_span(sg);
    lv_span_set_text(s2, hi);
    lv_style_set_text_color(lv_span_get_style(s2), WT_INK);
    if (at[strlen(hi)])
        lv_span_set_text(lv_spangroup_new_span(sg), at + strlen(hi));
}

// Refill an EXISTING spangroup. The three side-note helpers below all have a
// _fit twin that re-texts them after the fact -- the settings chooser caption
// swaps on every tap -- so the rebuild has to be a first class operation, not
// something only the constructor can do.
static void spans_fill(lv_obj_t *sg, const char *txt, const char *hi)
{
    // The _fit helpers are public and are called on labels this file did not
    // create -- kiss_sign.c re-texts two of its own with wt_note_fit. Reading
    // a label as a spangroup walks a linked list that is not there, which is a
    // segfault with no output at all. So the label case stays a label.
    if (!lv_obj_check_type(sg, &lv_spangroup_class)) {
        lv_label_set_text(sg, txt ? txt : "");
        return;
    }
    while (lv_spangroup_get_span_count(sg))
        lv_spangroup_delete_span(sg, lv_spangroup_get_child(sg, 0));
    if (!txt) txt = "";
    size_t i = 0, run = 0;
    char buf[640];
    while (txt[i]) {
        const bool stop = txt[i] == '.' && i > 0 &&
                          ((txt[i - 1] >= 'a' && txt[i - 1] <= 'z') ||
                           (txt[i - 1] >= 'A' && txt[i - 1] <= 'Z') ||
                           (txt[i - 1] >= '0' && txt[i - 1] <= '9')) &&
                          (txt[i + 1] == '\0' || txt[i + 1] == ' ' ||
                           txt[i + 1] == '\n');
        if (!stop) {
            if (run + 1 < sizeof buf) buf[run++] = txt[i];
            i++;
            continue;
        }
        if (run) { buf[run] = 0; span_run(sg, buf, hi); run = 0; }
        // The stop takes the space after it. Left on the front of the next
        // span, that space becomes the first character of a wrapped LINE and
        // indents it -- which a plain label never does, because it collapses
        // whitespace at the break. Carried on the stop it sits at the end of
        // the line instead, where it costs nothing.
        lv_span_t *dot = lv_spangroup_new_span(sg);
        lv_span_set_text(dot, txt[i + 1] == ' ' ? ". " : ".");
        lv_style_set_text_color(lv_span_get_style(dot), wt_accent());
        i += txt[i + 1] == ' ' ? 2 : 1;
    }
    if (run) { buf[run] = 0; span_run(sg, buf, hi); }
    lv_spangroup_refresh(sg);
}

// A wrapping paragraph that carries its accent stops, in place of a label.
// Same geometry a label had: a fixed width, a content height, and the colour
// on the object rather than on the text.
static lv_obj_t *spans_new(lv_obj_t *par, int x, int y, int w)
{
    lv_obj_t *sg = lv_spangroup_create(par);
    lv_obj_remove_style_all(sg);
    lv_obj_set_style_text_color(sg, WT_MUT, 0);
    lv_obj_set_pos(sg, x, y);
    lv_obj_set_width(sg, w);
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    // A LABEL is neither clickable nor scrollable and a spangroup is both, so
    // dropping one in place of the other silently eats the press meant for the
    // card underneath it. The setup chooser's "new here?" card stopped opening
    // the moment its note became spans, and the frame showed the chooser still
    // on screen with nothing to say why.
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_SCROLLABLE);
    // STOPS, not the whole thing. WT_FLAG_ACCENT sets the object's text
    // colour, and a paragraph's ordinary runs deliberately carry no style of
    // their own so they inherit the group's -- so that flag painted every
    // sentence on the device the accent colour the moment a theme was applied.
    lv_obj_add_flag(sg, WT_FLAG_ACCENT_STOPS);
    return sg;
}

// A paragraph as spans, with the full stop of each sentence in the accent.
//
// The accent stop marks where one thought ends and the next begins, so a two
// sentence body is scannable before it is read. Three other treatments were
// tried and this is the one that shipped: splitting the body by WEIGHT --
// answer in ink, qualifier in grey -- looks tidy and teaches the wrong thing,
// because the qualifier is usually the half carrying the risk ("Miners keep
// it.", "It cannot spend them.", "Left empty, it opens the decoy."). Dimming
// that says skip this. One ink for both, and the punctuation does the
// structural work.
//
// A FULL STOP, not every dot. A '.' only counts when a letter or digit sits
// before it and a space or the end of the string sits after, so a version
// number or a decimal keeps its own colour.
//
// WT_FLAG_ACCENT on each stop, so wt_accent_restyle repaints them with every
// other accent when the theme changes -- the flag exists because a list of
// accent-bearing objects built by shared helpers is a list that goes stale.
static lv_obj_t *body_spans(lv_obj_t *par, const char *txt, int x, int y,
                            const lv_font_t *f, int w)
{
    return body_spans_hi(par, txt, x, y, f, w, NULL);
}

// The same paragraph, with ONE term inside it lifted into the ink. The mono
// face has no bold, so contrast is the only emphasis there is, and the term a
// page exists to teach is the one thing on it worth that. It is a parameter
// rather than a second function because the stop colouring has to run either
// way -- the branch that highlighted a term used to be the branch with no
// accent stops in it, which is how every [ ? ] page lost them.
static lv_obj_t *body_spans_hi(lv_obj_t *par, const char *txt, int x, int y,
                               const lv_font_t *f, int w, const char *hi)
{
    lv_obj_t *sg = spans_new(par, x, y, w);
    lv_obj_set_style_text_font(sg, f, 0);
    spans_fill(sg, txt, hi);
    return sg;
}

static void body_to(lv_obj_t *par, const char *body, int y, int bottom)
{
    if (!par || !body || !*body) return;
    int room = bottom - y;
    if (room < 40) room = 40;

exp_paras_t ps;
    exp_split(body, &ps);

    // The rungs, and the FLOOR. There is no font14 rung any more. A body is
    // the thing an owner READS, and 14 is the size this device keeps for
    // MARKS: a body that will not fit at the floor has copy too long for its
    // room, which is what the report at the bottom of the loop says. The
    // ladder used to fall 28 -> 23 -> 14, and that last step gave away 40% of
    // a glyph in one move, so the rung it landed on was never a decision
    // anybody made -- it came off the bench four separate times before
    // anything on this device reported it.
    //
    // 21 is a rung for a body that CAN be set in mono, and only then.
    // wt_font21 does not exist: the third rung is the raw mono face, whose
    // line box is 23 against mono23's 25. A body that failed mono_can is
    // mostly non-ASCII and belongs on the sans-primary faces, whose 21 is
    // nat23 at a line box of 29 -- TALLER than the rung above it, which is a
    // ladder that climbs. So those bodies floor at 23 and this one stops at
    // two rungs.
    const lv_font_t *ladder[3];
    int rungs = 2;
    ladder[0] = wt_font28();
    ladder[1] = wt_font23();
    if (mono_can(body)) { ladder[2] = wt_font_mono21(); rungs = 3; }

    const lv_font_t *f = ladder[rungs - 1];
    int used = room;

    for (int r = 0; r < rungs; r++) {
        const lv_font_t *cand = ladder[r];
        const int hf = exp_height(&ps, 0, ps.count, cand, EXP_FULL_TXT);
        if (hf <= room) { f = cand; used = hf; break; }

        // Nothing fits at the floor either: keep the floor, clamp, and SAY SO,
        // because the copy is what has to give. This path was silent for its
        // whole life -- the ladder picked 14, the string never looked like a
        // bug in the source, and it came off the bench four separate times
        // before anything reported it.
        if (r == rungs - 1) {
            f = cand;
            used = room;
            WT_FIT_GAVE_UP("body", body, EXP_FULL_TXT, room);
        }
    }

    // Drop the band by a third of what is left over. Centring it outright
    // floats the text away from the title it answers; hugging the top, which
    // is what this did before, leaves the whole bottom of the card empty.
    int slack = room - used;
    if (slack > 0) { y += slack / 3; room -= slack / 3; }

    // ONE LABEL PER PARAGRAPH, on the content lane, exactly where
    // wt_explain_hi puts one.
    //
    // Separately, because a paragraph is a CLAIM and the device's own gate
    // agrees: BARE looks for a wrapping label 560 wide and 90 tall, which is
    // three lines. One label holding two claims and the blank line between
    // them is a wall by that measure however short each claim is, and it reads
    // as one too. Drawn apart, each claim stands or falls on its own length --
    // and a claim that is still three lines by itself is copy to cut, which is
    // what the house rules say to do about it.
    //
    // The gap is exp_height's own model of a blank line, so the ladder above
    // measured exactly what is drawn here.
    int py = y;
    const int gap = lv_font_get_line_height(f);
    for (int i = 0; i < ps.count; i++) {
        char one[640];
        exp_join(&ps, i, i + 1, one, sizeof one);
        lv_obj_t *p = body_spans(par, one, 48, py, f, EXP_FULL_TXT);
        wt_widow_measure(one, f, EXP_FULL_TXT);
        lv_obj_update_layout(p);
        py += lv_obj_get_height(p) + gap;
    }
}

void wt_body_para_to(lv_obj_t *par, const char *body, int y, int bottom)
{
    body_to(par, body, y, bottom);
}

void wt_body_para(lv_obj_t *par, const char *body, int y)
{
    wt_body_para_to(par, body, y, WT_CONTENT_BOTTOM);
}

// ---- WT_GRID_ICONS ----
// A glossary is a LIST, and it was being rendered as prose: eight lines of the
// same grey at the same size, so the eight terms it defines had to be read in
// order to find any one of them. Every line is written `TERM: definition` in all
// 21 locales, so an icon badge and a heading can be lifted straight out of the
// string that already exists. Nothing new to translate.
#define GRID_COLS   2
#define GRID_MAXN   12
#define GRID_BADGE  34
#define GRID_GUT    12
// One entry, in bytes. The longest today is the Russian dust attack line at 189
// and Cyrillic runs two bytes a character, so a 192 byte buffer would truncate
// the next translation that grows -- mid codepoint, because the copy below is a
// byte copy. Sized so the line the check gate measures is the line drawn.
#define GRID_LINE_MAX 256

static void grid_badge(lv_obj_t *par, const char *glyph, int x, int y,
                       lv_color_t col)
{
    lv_obj_t *b = lv_obj_create(par);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, GRID_BADGE, GRID_BADGE);
    lv_obj_set_style_radius(b, GRID_BADGE / 2, 0);
    lv_obj_set_style_bg_color(b, WT_KEY, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, WT_EDGE, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(wt_lbl(b, glyph, 0, 0, wt_font14(), col));
}

static void explain_grid(lv_obj_t *ovl, const wt_explain_t *e, int y, int room,
                         lv_color_t sev)
{
    // One line per entry, and NOT a count of eight: a locale is free to ship
    // seven or nine and the grid has to draw what it was handed.
    const char *ln[GRID_MAXN];
    int len[GRID_MAXN], n = 0;
    for (const char *s = e->body; s && *s && n < GRID_MAXN; ) {
        const char *nl = strchr(s, '\n');
        ln[n] = s;
        len[n] = nl ? (int)(nl - s) : (int)strlen(s);
        n++;
        if (!nl) break;
        s = nl + 1;
    }
    if (!n) return;

    // Two columns is what a LIST needs. One entry is not a list, and putting it
    // in a 346px lane leaves the right half of the card empty while wrapping
    // one sentence over five short lines. A single caution is the common case
    // on this card -- most flagged transactions trip exactly one -- and no walk
    // stop ever opened it, so it drew that way for its whole life.
    const int cols = (n == 1) ? 1 : GRID_COLS;
    int rows = (n + cols - 1) / cols;
    int cw   = (EXP_FULL_W - (cols - 1) * GRID_GUT) / cols;   // 346 at two
    int tw   = cw - GRID_BADGE - GRID_GUT;               // text lane beside it
    int pitch = room / rows;

    // TERM AND DEFINITION ON ONE WRAPPED RUN, which is what buys font23. They
    // were a font14 heading over a font14 definition, and a 300px column
    // cannot hold that pair at 23: a heading line plus a three line body is
    // 99px against a 92px budget, so every one of these pages -- WHY FLAGGED,
    // WHY FOUR SOURCES, and a dozen more, all of them pure teaching -- fell to
    // the size this device keeps for MARKS. No fit helper reported it because
    // the ladder here is local, and no reader could report it either.
    //
    // Run them together and the term costs a few words of the first line
    // instead of a line of its own, which is exactly the room needed. The
    // glossary page solved it this way first.
    const lv_font_t *bf = wt_font23();
    for (int pass = 0; pass < 2; pass++) {
        int tallest = 0;
        for (int i = 0; i < n; i++) {
            char line[GRID_LINE_MAX], head[64];
            int l = len[i] < (int)sizeof line ? len[i] : (int)sizeof line - 1;
            lv_memcpy(line, ln[i], (size_t)l);
            line[l] = 0;
            const char *def = wt_split_colon(line, head, sizeof head);
            if (!def) continue;
            char run[GRID_LINE_MAX + 72];
            snprintf(run, sizeof run, "%s %s", head, def);
            lv_point_t sz;
            lv_text_get_size(&sz, run, bf, 0, 0, tw, LV_TEXT_FLAG_NONE);
            if (sz.y > tallest) tallest = sz.y;
        }
        if (tallest <= pitch - 6) break;
        // DECIDED: the icon grid's ladder floors at 21 and no longer has a font14 rung.
        // THE FLOOR IS 21, NOT 14, which is the same floor wt_body_para has
        // and for the same reason: font14 is for MARKS -- chip labels, unit
        // suffixes, chevrons -- and every string in this grid is a SENTENCE an
        // owner reads before signing. WHY FLAGGED is the case that proves it:
        // five caution rows explaining why a payment was flagged, all of them
        // at the size this device keeps for punctuation.
        //
        // mono21 only where the copy CAN be mono, which is what the body
        // ladder asks too. Where it cannot, the rung stays 23 and the overflow
        // is reported rather than shrunk away -- copy too long for its box is
        // copy to cut, and a silent drop is what hid this for the grid's whole
        // life.
        bool mono_ok = true;
        for (int i = 0; i < n && mono_ok; i++) {
            char line[GRID_LINE_MAX];
            int l = len[i] < (int)sizeof line ? len[i] : (int)sizeof line - 1;
            lv_memcpy(line, ln[i], (size_t)l);
            line[l] = 0;
            if (!mono_can(line)) mono_ok = false;
        }
        if (pass == 0 && mono_ok) { bf = wt_font_mono21(); continue; }
        WT_FIT_GAVE_UP("grid", ln[0], tw, pitch - 6);
        break;
    }

    for (int i = 0; i < n; i++) {
        char line[GRID_LINE_MAX], head[64];
        int l = len[i] < (int)sizeof line ? len[i] : (int)sizeof line - 1;
        lv_memcpy(line, ln[i], (size_t)l);
        line[l] = 0;
        const char *def = wt_split_colon(line, head, sizeof head);

        int cx = 48 + (i % cols) * (cw + GRID_GUT);
        int cy = y + (i / cols) * pitch;

        if (e->icons && (size_t)i < e->icons_count && e->icons[i])
            grid_badge(ovl, e->icons[i], cx, cy, sev);

        int tx = cx + GRID_BADGE + GRID_GUT;
        if (!def) continue;

        // A spangroup, so the term and its explanation wrap as ONE paragraph
        // in two colours -- the term in the page's own ink, the sentence in
        // MUT. Two labels could not do it: the second would start on a fresh
        // line whatever room the first left.
        lv_obj_t *sg = lv_spangroup_create(ovl);
        lv_obj_set_pos(sg, tx, cy + 2);
        lv_obj_set_width(sg, tw);
        lv_obj_set_height(sg, pitch - 6);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_SCROLLABLE);
        lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
        lv_obj_set_style_text_font(sg, bf, 0);
        char hbuf[72];
        snprintf(hbuf, sizeof hbuf, "%s ", head);
        lv_span_t *sp1 = lv_spangroup_new_span(sg);
        lv_span_set_text(sp1, hbuf);
        lv_style_set_text_color(lv_span_get_style(sp1), sev);
        lv_span_t *sp2 = lv_spangroup_new_span(sg);
        lv_span_set_text(sp2, def);
        lv_style_set_text_color(lv_span_get_style(sp2), WT_MUT);
        lv_spangroup_refresh(sg);
    }
}

lv_obj_t *wt_explain_open(lv_obj_t *parent, const wt_explain_t *e)
{
    if (!parent || !e) return NULL;

    lv_obj_t *ovl = lv_obj_create(parent);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, WT_BG, 0);
    // Opaque, not 245. Ten parts in 255 of a light grey word list on a near
    // black ground is still legible: the seed words behind the explainer read
    // straight through it, competing with the card for the same eye. The card
    // has a title, a rule and its own frame -- it does not need a ghost of the
    // page under it to say it is a layer.
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);          // swallow stray taps
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ovl, explain_close_cb, LV_EVENT_CLICKED, ovl);

    // The title, and the first block's rule, carry the card's severity. A
    // caution explainer opened from an amber row should not arrive wearing the
    // accent: the colour is what says which of the eleven cards you are on
    // before a word of it is read.
    lv_color_t sev = e->sev == WT_SEV_OK   ? WT_OK
                   // wt_ink_for: a caution TITLE takes the accent. The badge
                   // beside it still carries the warning glyph in WT_WARN,
                   // which is where the amber belongs.
                   : e->sev == WT_SEV_WARN ? wt_ink_for(WT_WARN)
                   : e->sev == WT_SEV_STOP ? WT_STOP : wt_accent();

    // The subject badge, right of the title row. An icon is worth more than the
    // word it replaces only if it is the SAME icon the reader met on the screen
    // that sent them here, so callers pass the one their control already wears.
    const int icon_w = 48;
    if (e->icon && *e->icon) {
        lv_obj_t *chip = lv_obj_create(ovl);
        lv_obj_remove_style_all(chip);
        lv_obj_set_pos(chip, 752 - icon_w, 20);
        lv_obj_set_size(chip, icon_w, icon_w);
        lv_obj_set_style_radius(chip, icon_w / 2, 0);
        lv_obj_set_style_bg_color(chip, WT_KEY, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, WT_EDGE, 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *g = wt_lbl(chip, e->icon, 0, 0, wt_font28(), sev);
        lv_obj_center(g);
    }

    // Title and subtitle exactly where wt_screen puts them, because an explainer
    // is a page and should not announce itself as a different kind of object.
    int lane = e->icon && *e->icon ? 704 - icon_w - 16 : 704;
    lv_obj_t *t = wt_lbl(ovl, e->title, 48, 18, wt_font28(), sev);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_set_width(t, lane);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

    int y = 76;
    if (e->sub && *e->sub) {
        lv_obj_t *s = wt_lbl(ovl, e->sub, 48, 64, wt_body_font(e->sub, lane, 29),
                             WT_MUT);
        lv_obj_set_width(s, lane);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        y = 104;
    } else {
        lv_obj_t *pt = parent ? wt_screen_title(parent) : NULL;
        const char *pn = pt ? lv_label_get_text(pt) : NULL;
        if (pn && *pn) {
            wt_trail(ovl, WT_ICON_WHAT, pn, false);
            y = WT_CHROME_STRIP_Y + WT_BR_H;
        }
    }

    // Band one: the value this card is about, and whatever diagram the caller
    // draws. Either may be absent; with both, they share the row.
    int band = 0;
    bool has_val = e->val && *e->val;
    if (has_val) {
        lv_obj_t *vc = wt_value_card(ovl, e->cap ? e->cap : "", e->val,
                                     48, y, e->aside ? 340 : 704, true);
        lv_obj_update_layout(vc);
        band = lv_obj_get_height(vc);
    }
    if (e->aside) {
        int ax = has_val ? 408 : 48, aw = has_val ? 344 : 704;
        int ah = e->aside(ovl, ax, y, aw);
        if (ah > band) band = ah;
    }
    if (band) y += band + 20;

    // Band two: the body. WT_CONTENT_BOTTOM is the floor and everything is
    // measured against what is left above it, so a long translation drops a font
    // size instead of running under the OK action.
    // The term line takes its own line off the bottom of the lane BEFORE the
    // body is measured. Measuring the body against the lane and then drawing
    // a term line into it is how a three line body lands on top of the term
    // it is defining -- which a bottom-vs-height check cannot see, because
    // both of them fit.
    int bottom = WT_CONTENT_BOTTOM;
    const bool has_term = e->term && *e->term;
    if (has_term)
        bottom -= lv_font_get_line_height(chrome23(e->term)) + 14;

    if (e->body && *e->body) {
        int room = bottom - y;
        if (room < 40) room = 40;

        if (e->mode == WT_GRID_ICONS) {
            explain_grid(ovl, e, y, room, sev);
        } else {
            wt_body_para_to(ovl, e->body, y, bottom);
        }
    }
    if (has_term) {
        // Its BOTTOM on the floor, not its top a line height above it. A
        // label's rendered box is not its font's line height -- the term line
        // pinned by arithmetic came out 2px into the action row, which the
        // CONTENT check saw and nothing else would have.
        lv_obj_t *tl = wt_term_line(ovl, e->term_label, e->term, 48, bottom + 14,
                                    704);
        lv_obj_update_layout(tl);
        // ...measured from what is INSIDE it, not from the box. chrome23
        // answers per string, so a label can be a pixel taller than the box
        // holding it and hang out of the bottom -- which is a pixel into the
        // action band once the box's own bottom is on the floor. CONTENT
        // reported exactly that on the TXID card while the card next door,
        // whose term carries a middle dot and lands on a taller face,
        // measured clean.
        int th = lv_obj_get_height(tl);
        for (uint32_t i = 0; i < lv_obj_get_child_count(tl); i++) {
            lv_obj_t *c = lv_obj_get_child(tl, i);
            // The FONT's line box where it is taller than the label's own. A
            // label's box is what LVGL gave it; the glyphs are what the reader
            // and the CONTENT check both see. On the TXID card the labels sit
            // 3px inside this container and paint 25 tall, needing 28 -- and
            // measuring the box answered 25, which put the line's last row of
            // pixels exactly on the band.
            //
            // RELATIVE positions, deliberately. An earlier attempt at this
            // measured absolute coords right after moving the container, and
            // LVGL had not resolved the children yet, so it read the line as
            // already clear and corrected nothing.
            int ch = lv_obj_get_height(c);
            const int lh = (int)lv_font_get_line_height(
                               lv_obj_get_style_text_font(c, LV_PART_MAIN));
            if (lh > ch) ch = lh;
            const int cb = lv_obj_get_y(c) + ch;
            if (cb > th) th = cb;
        }
        // TWO CLEAR of the floor, not flush on it. 398 is where the action
        // band starts, and a line whose last pixel is the band's first is
        // touching a control -- which CONTENT reads as a crossing and a reader
        // reads as cramped. It also stops the arithmetic being exact: this
        // line lands a pixel low on some cards and not others, and chasing
        // that pixel through chrome23, the intro stagger and the value card
        // above it bought nothing a two pixel gap does not.
        lv_obj_set_y(tl, WT_CONTENT_BOTTOM - th - 2);

    }

    // 552..752: the corner, like every other way off a screen. It was centred
    // at 300, which matched neither the old rule nor the new one -- and this is
    // the most opened bar in the app, behind all ten explainers and every "?".
    lv_obj_t *ok = wt_arrow_action(ovl, e->ok_txt, true, false, 552,
                                   WT_ACTION_Y, 200, true,
                                   explain_close_cb, ovl);
    lv_obj_remove_flag(ok, LV_OBJ_FLAG_IGNORE_LAYOUT);
    wt_card_intro(ovl);
    return ovl;
}

// One label carrying its own bordered box, not a container plus a child: LVGL
// labels take border and background styles, so LV_SIZE_CONTENT plus padding
// gives a chip that measures itself against whatever the translation turns out
// to be. A container with a child label needs two layout passes and was the
// shape that segfaulted the first time this was tried.
void wt_tiny_ok(lv_obj_t *l)
{
    if (l) lv_obj_add_flag(l, WT_FLAG_TINY_OK);
}

// ONE label still, with LVGL's inline RECOLOR doing what two objects would
// have. The comment above is not decoration: a container plus a child label
// needs two layout passes and segfaulted the first time this chip was tried,
// and a spangroup -- the other obvious answer -- is not an lv_label, so
// everything that finds a control by its text stops finding it. Six walk
// assertions failed within a minute of trying that.
//
// So the MARK is wrapped in a #RRGGBB..# run and the words are left plain.
// The glyph keeps its status colour, the label's own colour carries the
// words, and the rim follows the words -- no single object wears the accent
// and a status at once, which is what the ROLE gate exists to catch. The raw
// text still CONTAINS the words, so substring assertions are unaffected, and a
// chip with no glyph gets no markup at all, so exact ones are too.
lv_obj_t *wt_state_chip(lv_obj_t *par, const char *txt, lv_color_t col)
{
    lv_obj_t *c = lv_label_create(par);
    lv_label_set_recolor(c, true);
    lv_obj_set_style_text_font(c, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    // NO RIM. A rounded outline around a word is the pill this device does
    // not draw any more -- the sign pill, the settings pill, the header pills
    // and the auto-lock banner all went, and this was the last shape still
    // wearing one, on every screen at once because it is the kit's.
    // The state is carried by the tint and the ink, which is what the flat
    // chrome does everywhere else.
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 5, 0);
    lv_obj_set_style_bg_opa(c, 36, 0);        // ~14 percent: the rim's job now
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    // Declared font14 for the whole widget: a state chip is a BADGE, which is
    // what font14 is for, and a rim plus a tint plus a mark is how it is read
    // rather than by reading it. A call site that deliberately lifts one -- the
    // touch-dead banner is the only one -- sets font23 afterwards, and the gate
    // reads the rendered size, so this declaration cannot silence that.
    wt_tiny_ok(c);
    wt_state_chip_set(c, txt, col);
    return c;
}

void wt_state_chip_set(lv_obj_t *chip, const char *txt, lv_color_t col)
{
    if (!chip) return;
    const char *t = txt ? txt : "";
    const lv_color_t ink = wt_ink_for(col);
    // The split is the two spaces wt_icon_text composes, the same form the
    // walk finds controls by.
    const char *split = strstr(t, "  ");
    if (lv_color_eq(ink, col) || !split || split == t) {
        lv_label_set_text(chip, t);
    } else {
        char buf[160];
        snprintf(buf, sizeof buf, "#%02X%02X%02X %.*s#%s",
                 col.red, col.green, col.blue, (int)(split - t), t, split);
        lv_label_set_text(chip, buf);
    }
    lv_obj_set_style_text_color(chip, ink, 0);
    lv_obj_set_style_border_color(chip, ink, 0);
    lv_obj_set_style_bg_color(chip, ink, 0);
    // wt_ink_for hands a caution's WORDS the accent and leaves the GLYPH at the
    // severity, so this label's own colour IS the accent on every chip that
    // carries one -- and nothing repainted it. TESTNET sat in MONO blue on a
    // GREEN sign screen, beside a strand and a slide that had both moved. It is
    // the kit's chip, so that was every state chip on the device.
    //
    // Conditional, because the ink is only the accent when it is: a chip whose
    // colour needs no lift keeps its own, and flagging that one would paint a
    // status colour with the theme.
    //
    // The MARK's colour is baked into the markup above and is right to be:
    // that is the severity, which does not move with the theme -- the same
    // direction the bundle note rows take.
    if (lv_color_eq(ink, wt_accent())) lv_obj_add_flag(chip, WT_FLAG_ACCENT);
    else                               lv_obj_remove_flag(chip, WT_FLAG_ACCENT);
    lv_obj_update_layout(chip);
}

static void ci_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void ci_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void ci_bg(void *o, int32_t v)  { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

void wt_card_intro(lv_obj_t *card)
{
    // the dim backdrop eases in first
    lv_opa_t bg = lv_obj_get_style_bg_opa(card, 0);
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_anim_t d;
    lv_anim_init(&d);
    lv_anim_set_var(&d, card);
    lv_anim_set_values(&d, 0, bg);
    lv_anim_set_duration(&d, 140);
    lv_anim_set_path_cb(&d, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&d, ci_bg);
    lv_anim_start(&d);

    // then the content settles in, one element after the next. translate_y is a
    // render offset, so it composes cleanly with lv_obj_align and reverts to 0.
    // The whole stagger fits in a fixed window, however many children there
    // are. It used to be a flat 60ms per child, which was fine for a card of
    // four and became a 900ms drip once the explainer grew an icon badge, a
    // value card, a diagram and two blocks: the OK button arrived a second
    // after the title. Spreading a constant budget keeps the cadence and caps
    // the wait.
    uint32_t n = lv_obj_get_child_count(card);
    uint32_t step = n > 1 ? 300 / (n - 1) : 0;
    if (step > 60) step = 60;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(card, i);
        lv_obj_set_style_opa(ch, 0, 0);
        lv_obj_set_style_translate_y(ch, 14, 0);
        uint32_t delay = 40 + i * step;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ch);
        lv_anim_set_values(&a, 0, LV_OPA_COVER);
        lv_anim_set_duration(&a, 220);
        lv_anim_set_delay(&a, delay);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, ci_opa);
        lv_anim_start(&a);
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, ch);
        lv_anim_set_values(&b, 14, 0);
        lv_anim_set_duration(&b, 240);
        lv_anim_set_delay(&b, delay);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&b, ci_ty);
        lv_anim_start(&b);
    }
}

// ---- chip diagrams (shared by the "?" cards) ----
lv_obj_t *wt_diagram_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *wt_chip(lv_obj_t *row, const char *txt, bool accent)
{
    // An UNDERLINED TERM, not a box. The bordered pill this used to be put
    // every teaching diagram on the device in font14 fine print inside
    // little frames, and the bench finally said so. The underline is the
    // whole token mark now -- and it is still a border (bottom side only),
    // so ent_chip_lit's border_color repaint and every caller that reads
    // child 0 for the label keep working untouched.
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 6, 0);
    lv_obj_set_style_pad_bottom(c, 5, 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, accent ? wt_primary() : WT_DIM, 0);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, accent ? WT_INK : WT_MUT, 0);
    // chrome21: the term is the payload of the diagram it sits in, and any
    // icon composed into the string resolves through the nat fallback at
    // the same size, exactly as the tab strip's marks do.
    lv_obj_set_style_text_font(l, chrome21(txt), 0);
    lv_obj_center(l);
    return c;
}

lv_obj_t *wt_diagram_op(lv_obj_t *row, const char *txt)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, txt);
    // The operator is the only part of an equation that is pure grammar -- the
    // plus and the arrow between chips that carry the terms -- so it is the
    // part the theme should own. Three callers pass a STATUS glyph through
    // here instead of an operator; they clear the flag at the call site.
    lv_obj_set_style_text_color(l, wt_accent(), 0);
    lv_obj_add_flag(l, WT_FLAG_ACCENT);
    lv_obj_set_style_text_font(l, wt_font23(), 0);
    return l;
}

// Each term carries its own mark, because this equation is read at a glance or
// not at all: three same-shaped word chips are three things to READ before the
// arrow means anything, and the reader is here precisely because words did not
// land the first time. The list, the lock and the key say the whole sentence
// before the labels are parsed, and the labels stay under them because three
// unlabelled icons would be a riddle rather than a shortcut.
//
// All three codepoints are already in SYMS in tools/fonts/gen_fonts.sh at every
// size a chip can take, so this costs nothing in flash and needs no font rebuild
// — and the key is the same mark the card's own badge carries, which is what
// ties the answer to the question the reader tapped.
static void wt_chip_icon(lv_obj_t *row, const char *icon, const char *txt,
                         bool accent)
{
    char buf[WT_ICON_TEXT_MAX];
    snprintf(buf, sizeof buf, "%s %s", icon, txt);
    wt_chip(row, buf, accent);
}

// What words and a passphrase MAKE. The outcome used to be FINGERPRINT, and
// that was true and useless: a newcomer meeting this on the seed explainer, the
// passphrase intro and the fingerprint help card learned that two things they
// had just been told to protect add up to an eight character code, which is not
// what they add up to. They add up to KEYS. The code is what those keys are
// CALLED, which is a different sentence and now has its own diagram below.
//
// Not "YOUR SIGNER" either, though it was asked for: the signer is this box and
// it does not change when a different passphrase is typed. Teaching that would
// have to be untaught the first time the owner read anything else about bitcoin.
void wt_diagram_fp(lv_obj_t *parent)
{
    // One mark, on the result. Three marked terms at chrome21 measured wider
    // than the 704 card on the seed explainer and clipped at both ends; the
    // key on YOUR KEYS is the icon doing work -- it is the same mark the
    // KEYS page and the fingerprint badge wear -- and the two inputs say
    // themselves.
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip(row, tr(STR_D_WORDS), false);
    wt_diagram_op(row, "+");
    wt_chip(row, tr(STR_D_PASSPHRASE), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip_icon(row, WT_ICON_KEY, tr(STR_D_KEYS), true);
}

// The fingerprint explainer's OWN picture, and the reason it exists: the "?" on
// the fingerprint screen used to open a card drawing wt_diagram_fp, which is the
// diagram already on the screen behind it. Tapping for help repeated the answer
// the reader had just decided was not enough.
//
// Two chips, so unlike wt_diagram_fp (about 600px in English, wider in half the
// locales -- see the note in kiss_ui.c's show_fingerprint) this one fits a 344
// column and can go anywhere the pair geometry goes.
//
// The code is passed in rather than read from a seam, because the same card is
// opened for the live wallet from three places and for nothing at all before
// setup. Empty falls back to the word, which is what the title does too.
void wt_diagram_fpid(lv_obj_t *parent, const char *code)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip_icon(row, WT_ICON_KEY, tr(STR_D_KEYS), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, code && code[0] ? code : tr(STR_D_FINGERPRINT), true);
}

// What a backup check actually claims: RECOVERY WORDS -> THIS WALLET. Two chips
// and one arrow, because the whole screen is one assertion and a longer diagram
// would be inventing steps to look busy.
//
// Both labels already exist in every locale (STR_D_WORDS is the fingerprint
// equation's own first chip, so the two diagrams agree on what words are
// called), and both marks are in SYMS at every chip size. The tick is accented
// because the match is the outcome being proven, the same way the fingerprint
// equation accents its result.
//
// STR_I_T and not STR_I_SEC_THIS_WALLET, which reads better in English and is
// the string this was first written with: that key is the literal text "THIS
// WALLET" in all twenty one locales, so on a Russian screen it sat in English
// between two translated chips. It is a section HEADING everywhere else it
// appears, where nobody had noticed; in a diagram beside translated words it is
// obvious. STR_I_T is the WALLET page's own title and is properly localised.
void wt_diagram_verify(lv_obj_t *parent)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip_icon(row, LV_SYMBOL_LIST, tr(STR_D_WORDS), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    // D_KEYS, not I_T. This chip used to borrow the WALLET page's title because
    // that title was the localised word for a key set. It is not any more -- it
    // named the box for a while and now it says KEYS -- and a diagram that
    // depends on a page title is one rename away from claiming that recovery
    // words rebuild a signer. They rebuild keys, which is what D_KEYS is for.
    wt_chip_icon(row, LV_SYMBOL_OK, tr(STR_D_KEYS), true);
}

void wt_diagram_pair(lv_obj_t *parent)
{
    // the airgap: an online app and the offline signer, bridged only by QR.
    // A COLUMN, not a row: its one home is the PAIR card's 344px aside, and
    // COORDINATOR WALLET beside KISS OFFLINE stopped fitting a lane that
    // narrow the day the chips grew to chrome21. Stacked, the op line reads
    // the traffic too -- the unsigned PSBT comes down, the signed one goes
    // back up, and nothing but QR light crosses either way.
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    wt_chip(col, tr(STR_D_ONLINE_APP), false);
    wt_diagram_op(col, LV_SYMBOL_DOWN " QR " LV_SYMBOL_UP);
    wt_chip(col, tr(STR_D_KISS_OFFLINE), true);
}

// ---- the airgap, drawn ---------------------------------------------------
//
// The SIGN page's hero: a phone SHOWING a QR, this signer's camera FRAMING
// it, and a dashed break between them that nothing but light crosses. The
// same fact wt_diagram_pair states in words, drawn as the two machines --
// because the chip row on that page was the first thing the bench saw and
// it read as fine print, not a picture.
//
// Fixed geometry in figure coordinates, like the bundle graph: the figure is
// AG_W x AG_H and the caller places it. The 60px side margins are load
// bearing -- COORDINATOR WALLET at mono18 is ~194px, wider than the phone
// it is centred under, and the spill has to land inside the figure.

// The coordinator is EITHER machine: the laptop carries the block (Sparrow
// is a desktop app) and a phone stands against its edge (Nunchuk is not),
// because a laptop alone came off the bench as claiming the coordinator is
// only ever a computer.
#define AG_CO_X   40    // coordinator block's left edge
#define AG_CO_W  200    // its base bar, the block's full width
#define AG_CO_SW 168    // the screen, centred over the base
#define AG_CO_SH 100
#define AG_CO_BH   8    // the base bar's height
#define AG_GAP   130    // the airgap itself
#define AG_SG_W  200    // this signer, landscape like the real panel
#define AG_SG_H  120
#define AG_BAND  120    // the machines' shared vertical band (the signer's)
#define AG_LBL_Y (AG_BAND + 12)
#define AG_W     (AG_CO_X + AG_CO_W + AG_GAP + AG_SG_W + 40)
#define AG_H     (AG_LBL_Y + 24)

// A QR mark drawn rather than typed: three finder squares and a scatter of
// modules, s pixels on a side. The font's QR glyph tops out at 28px, which
// on a 150px phone reads as an app icon; this one scales with the machine
// showing it.
static void ag_qr(lv_obj_t *par, int x, int y, int s, lv_color_t col)
{
    const int f = s * 7 / 24;            // finder square side
    const int m = s / 8;                 // one module
    static const struct { int fx, fy; } F[3] = { {0,0}, {1,0}, {0,1} };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *sq = lv_obj_create(par);
        lv_obj_remove_style_all(sq);
        lv_obj_set_pos(sq, x + F[i].fx * (s - f), y + F[i].fy * (s - f));
        lv_obj_set_size(sq, f, f);
        lv_obj_set_style_border_width(sq, 2, 0);
        lv_obj_set_style_border_color(sq, col, 0);
        lv_obj_t *dot = lv_obj_create(sq);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, f - 8, f - 8);
        lv_obj_set_style_bg_color(dot, col, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_center(dot);
    }
    // The data modules, a fixed constellation in the quadrant the finders
    // leave open (and one straggler under the top right finder). Fractions
    // of s so the mark scales as one shape.
    static const struct { int mx, my; } D[6] = {
        { 14, 14 }, { 19, 16 }, { 16, 19 }, { 21, 21 }, { 14, 21 }, { 21, 10 },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *d = lv_obj_create(par);
        lv_obj_remove_style_all(d);
        lv_obj_set_pos(d, x + D[i].mx * s / 24, y + D[i].my * s / 24);
        lv_obj_set_size(d, m, m);
        lv_obj_set_style_bg_color(d, col, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
}

lv_obj_t *wt_diagram_airgap(lv_obj_t *parent)
{
    lv_obj_t *fig = lv_obj_create(parent);
    lv_obj_remove_style_all(fig);
    lv_obj_set_size(fig, AG_W, AG_H);
    lv_obj_remove_flag(fig, LV_OBJ_FLAG_SCROLLABLE);

    // The laptop: a screen over a base bar, muted stroke -- somebody else's
    // machine, showing the QR.
    const int co_y = (AG_BAND - AG_CO_SH - AG_CO_BH - 2) / 2;
    lv_obj_t *ph = lv_obj_create(fig);
    lv_obj_remove_style_all(ph);
    lv_obj_set_pos(ph, AG_CO_X + (AG_CO_W - AG_CO_SW) / 2, co_y);
    lv_obj_set_size(ph, AG_CO_SW, AG_CO_SH);
    lv_obj_set_style_radius(ph, 8, 0);
    lv_obj_set_style_border_width(ph, 2, 0);
    lv_obj_set_style_border_color(ph, WT_MUT, 0);
    ag_qr(ph, (AG_CO_SW - 56) / 2, (AG_CO_SH - 56) / 2, 56, WT_INK);
    lv_obj_t *base = lv_obj_create(fig);
    lv_obj_remove_style_all(base);
    lv_obj_set_pos(base, AG_CO_X, co_y + AG_CO_SH + 2);
    lv_obj_set_size(base, AG_CO_W, AG_CO_BH);
    lv_obj_set_style_radius(base, 4, 0);
    lv_obj_set_style_border_width(base, 2, 0);
    lv_obj_set_style_border_color(base, WT_MUT, 0);
    // The phone, standing against the laptop's right edge on the same floor.
    // Drawn after both so it fronts them, with an opaque ground fill doing
    // the occluding -- the overlap is what says "beside", not "as well as
    // this other block". Blank screen on purpose: the QR stays on the laptop,
    // because two QRs would read as two sources. The home dot is what makes
    // 34x58 read as a phone rather than a stray card edge.
    {
        const int pw = 34, phh = 58;
        const int px = AG_CO_X + AG_CO_W + 10 - pw;
        const int py = co_y + AG_CO_SH + 2 + AG_CO_BH - phh;
        lv_obj_t *pho = lv_obj_create(fig);
        lv_obj_remove_style_all(pho);
        lv_obj_set_pos(pho, px, py);
        lv_obj_set_size(pho, pw, phh);
        lv_obj_set_style_radius(pho, 8, 0);
        lv_obj_set_style_bg_color(pho, WT_BG, 0);
        lv_obj_set_style_bg_opa(pho, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pho, 2, 0);
        lv_obj_set_style_border_color(pho, WT_MUT, 0);
        lv_obj_t *hd = lv_obj_create(pho);
        lv_obj_remove_style_all(hd);
        lv_obj_set_size(hd, 4, 4);
        lv_obj_set_style_radius(hd, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(hd, WT_MUT, 0);
        lv_obj_set_style_bg_opa(hd, LV_OPA_COVER, 0);
        lv_obj_align(hd, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    // This signer: landscape like the panel it is, and the one element the
    // accent claims -- the same "this box" cue KISS OFFLINE's chip carries.
    const int sx = AG_CO_X + AG_CO_W + AG_GAP;
    const int sy = (AG_BAND - AG_SG_H) / 2;
    lv_obj_t *sg = lv_obj_create(fig);
    lv_obj_remove_style_all(sg);
    lv_obj_set_pos(sg, sx, sy);
    lv_obj_set_size(sg, AG_SG_W, AG_SG_H);
    lv_obj_set_style_radius(sg, 10, 0);
    lv_obj_set_style_border_width(sg, 2, 0);
    lv_obj_set_style_border_color(sg, wt_primary(), 0);
    // Viewfinder corners around what the camera is framing: the same QR the
    // phone shows, dimmer, because this side is a camera's view of it.
    static const struct { int cx, cy; lv_border_side_t side; } C[4] = {
        { 0, 0, LV_BORDER_SIDE_TOP    | LV_BORDER_SIDE_LEFT  },
        { 1, 0, LV_BORDER_SIDE_TOP    | LV_BORDER_SIDE_RIGHT },
        { 0, 1, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT  },
        { 1, 1, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *cn = lv_obj_create(sg);
        lv_obj_remove_style_all(cn);
        lv_obj_set_size(cn, 14, 14);
        lv_obj_set_pos(cn, C[i].cx ? AG_SG_W - 14 - 14 : 14,
                       C[i].cy ? AG_SG_H - 14 - 14 : 14);
        lv_obj_set_style_border_width(cn, 2, 0);
        lv_obj_set_style_border_color(cn, wt_primary(), 0);
        lv_obj_set_style_border_side(cn, C[i].side, 0);
    }
    ag_qr(sg, (AG_SG_W - 40) / 2, (AG_SG_H - 40) / 2, 40, WT_MUT);

    // The gap: a dashed line with the QR mark punched through the middle of
    // it, a chevron either side -- the file goes over and comes back, and
    // nothing but light crosses. The dash renders because the segment is
    // exactly horizontal -- the renderer dashes h/v lines only (the bundle
    // graph note).
    const int ly = AG_BAND / 2;
    static lv_point_precise_t pts[2];
    pts[0].x = 0; pts[0].y = 0;
    pts[1].x = AG_GAP - 32; pts[1].y = 0;
    lv_obj_t *ln = lv_line_create(fig);
    lv_line_set_points(ln, pts, 2);
    lv_obj_set_pos(ln, AG_CO_X + AG_CO_W + 16, ly);
    lv_obj_set_style_line_width(ln, 2, 0);
    lv_obj_set_style_line_color(ln, WT_DIM, 0);
    lv_obj_set_style_line_dash_width(ln, 6, 0);
    lv_obj_set_style_line_dash_gap(ln, 6, 0);
    lv_obj_t *qr = lv_label_create(fig);
    // font14, not mono18: the chevrons only exist in the sans fallback, and a
    // mono face handed an icon draws the placeholder box.
    lv_label_set_text(qr, LV_SYMBOL_LEFT " QR " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(qr, wt_font14(), 0);
    lv_obj_set_style_text_color(qr, WT_MUT, 0);
    lv_obj_set_style_bg_color(qr, WT_BG, 0);
    lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(qr, 6, 0);
    lv_obj_update_layout(qr);
    lv_obj_set_pos(qr, AG_CO_X + AG_CO_W + AG_GAP / 2 - lv_obj_get_width(qr) / 2,
                   ly - lv_obj_get_height(qr) / 2);

    // The names, under the machines rather than boxed around words.
    lv_obj_t *cl = lv_label_create(fig);
    lv_label_set_text(cl, tr(STR_D_ONLINE_APP));
    lv_obj_set_style_text_font(cl, chrome18(tr(STR_D_ONLINE_APP)), 0);
    lv_obj_set_style_text_color(cl, WT_MUT, 0);
    lv_obj_update_layout(cl);
    lv_obj_set_pos(cl, AG_CO_X + AG_CO_W / 2 - lv_obj_get_width(cl) / 2,
                   AG_LBL_Y);
    lv_obj_t *sl = lv_label_create(fig);
    lv_label_set_text(sl, tr(STR_D_KISS_OFFLINE));
    lv_obj_set_style_text_font(sl, chrome18(tr(STR_D_KISS_OFFLINE)), 0);
    lv_obj_set_style_text_color(sl, WT_INK, 0);
    lv_obj_update_layout(sl);
    lv_obj_set_pos(sl, sx + AG_SG_W / 2 - lv_obj_get_width(sl) / 2, AG_LBL_Y);
    return fig;
}

// ---- the bundle graph ----------------------------------------------------
//
// Geometry, all of it, in box coordinates. The drawing's path data is written
// in the same box at 1:1, so these ARE the numbers in the appendix rather than
// a reading of them.
//
//   junction      (BJ_X, h/2)          2c h=118 -> 59, 3a h=110 -> 55, 3b h=90 -> 45
//   outputs end   BO_X
//   labels start  BL_X
//   rows          evenly spaced from BMARG to h - BMARG
//
// The row rule is worth stating because one of its consequences is load
// bearing. pitch = (h - 2*BMARG) / (n - 1) reproduces frame 2c exactly (three
// rows at 12 / 59 / 106) and lands within 2px of 3a and 3b -- but the part that
// matters is that with an ODD row count the middle row falls exactly on the
// junction. The elided group strand is that middle row, it is therefore
// perfectly horizontal, and LVGL's software renderer dashes horizontal and
// vertical segments ONLY (draw_line_skew has no dash path at all). A group
// strand one pixel off the junction is a group strand drawn solid, which reads
// as one coin -- the exact thing the dash exists to deny.
#define BMARG   12
// The junction, and where the output lane starts. Moved LEFT by 40 the day
// every destination started carrying its address: an output row is an amount,
// a label and a folded address, an input row is an amount, and the split
// should say so. 312px could not hold the fold at mono18 and cut it a
// character short of the lit run -- which is the run being compared.
//
// The inputs keep more than they use: their lane is clamped to BLANE_MAX 206
// and the junction is at 290, so there is 84px of slack on that side.
#define BJ_X   290
#define BO_X   390
#define BL_X   400
#define BJ_R     5
// The lane the input amounts are right aligned in, measured from the widest of
// them and clamped. 104 is frame 2c's lane, 206 is frame 3a's, where the group
// row carries a count and a total on one line.
#define BLANE_MIN 104
#define BLANE_MAX 206
#define BLANE_GAP  16
// Points per curve. The longest strand spans 210px, so 16 puts a vertex every
// ~14px; with line_rounded the joins disappear at this stroke width.
#define BSEG 16
// The junction at rest and at the end of a hold. Rest is not this animation's
// to move: the resting frame is drawn and approved, so growth is what carries
// the hold and the accent that arrives afterwards means the signature.
#define BJ_HOLD_R 8
// Vertical slack around the box so a row's LABEL, which is centred on its
// strand and therefore reaches above the top row, is not clipped by the
// container. LVGL clips children to their parent.
#define BPAD 16

typedef struct {
    lv_point_precise_t *pts;      // one block for every strand; lv_line borrows it
    uint16_t            n_line;
    lv_obj_t           *line[WT_BUNDLE_MAX];
    lv_obj_t           *amount[WT_BUNDLE_MAX];
    lv_obj_t           *note[WT_BUNDLE_MAX];
    uint8_t             role[WT_BUNDLE_MAX];
    bool                flag[WT_BUNDLE_MAX];  // a caution points at this strand
    // The hold overlay: one accent line per INPUT strand, drawn over its
    // resting one and truncated to how far the hold has got. Its own point
    // block, because the resting strand's is what the truncation is measured
    // FROM and both are live at once.
    lv_point_precise_t *hpts;
    lv_obj_t           *hline[WT_BUNDLE_MAX];
    uint8_t             nseg[WT_BUNDLE_MAX];  // 2 on a flat strand, BSEG on a curve
    uint16_t            n_in;
    lv_obj_t           *dot;      // the junction, which grows with the hold
    // The output side, and what it takes to redraw its strands when it moves.
    lv_obj_t           *col;      // the PAGED output column, NULL if fixed
    lv_obj_t           *sbox;     // clips the output strands to the graph band
    lv_obj_t           *row[WT_BUNDLE_MAX];   // one per output, in column order
    uint16_t            n_out;
    uint16_t            out0;     // index in line[] where the outputs start
    int16_t             jy;       // the junction, in box coordinates
    // PAGES, not a scroll. A free scroller comes to rest wherever the finger
    // leaves it, so the row at the fold is sliced through its own address --
    // which is the defect RECEIVE's ALL ADDRESSES fixed by paging, in the words
    // "the page IS the window, so nothing can come to rest cut through its own
    // caption any more". Boundaries are MEASURED rather than counted: rows are
    // content sized and a silent payment row is a paragraph, so a page ends
    // where the next row would not fit whole.
    int16_t             ptop[WT_BUNDLE_MAX];  // scroll offset of each page
    uint8_t             prow[WT_BUNDLE_MAX];  // first row index on each page
    uint8_t             npage, page;
    int16_t             bh;       // the graph band's height, for the fan below
    // Each output's destination, kept so a caller can make the ROW the tap
    // target for its own full address. The pointers are the caller's and
    // outlive the build, exactly as wt_strand_t.addr already promises.
    const char         *addr[WT_BUNDLE_MAX];
    lv_obj_t           *fold[WT_BUNDLE_MAX];  // the address line, the control
} wt_bundle_t;

// lv_line_set_points stores the POINTER, not a copy (see kiss_word_ui.c:82 for
// the bug that taught this file the same lesson). The block outlives every
// strand and dies with the container.
static void bundle_delete_cb(lv_event_t *e)
{
    wt_bundle_t *b = lv_event_get_user_data(e);
    if (!b) return;
    lv_free(b->pts);
    lv_free(b->hpts);
    lv_free(b);
}

int wt_strand_px(uint64_t sats, uint64_t max_sats)
{
    if (!max_sats) return 2;
    int px = (int)((sats * 11) / max_sats);       // 11px is the widest strand
    return px < 2 ? 2 : px;                       // 2px floor, or dust vanishes
}

// A cubic sampled into `out`. Both control points share a y with the end they
// belong to, which is what makes these read as one strand bending rather than
// two lines meeting: the curve leaves its row horizontally and arrives at the
// junction horizontally.
static void bundle_curve(lv_point_precise_t *out, int x0, int y0, int x1, int y1,
                         int c0_num, int c1_num)
{
    const int span = x1 - x0;
    const int cx0 = x0 + (span * c0_num) / 100;
    const int cx1 = x0 + (span * c1_num) / 100;
    for (int i = 0; i < BSEG; i++) {
        // Fixed point at 1/1024: this runs on a chip with no FPU worth using
        // and the answer is rounded to a pixel either way.
        const int32_t t  = (int32_t)i * 1024 / (BSEG - 1);
        const int32_t it = 1024 - t;
        const int64_t a = (int64_t)it * it * it;          // (1-t)^3
        const int64_t b = 3LL * it * it * t;              // 3(1-t)^2 t
        const int64_t c = 3LL * it * t * t;               // 3(1-t) t^2
        const int64_t d = (int64_t)t * t * t;             // t^3
        const int64_t den = 1024LL * 1024 * 1024;
        out[i].x = (lv_value_precise_t)((a * x0 + b * cx0 + c * cx1 + d * x1) / den);
        out[i].y = (lv_value_precise_t)((a * y0 + b * y0  + c * y1  + d * y1) / den);
    }
}

// A signature outranks a caution: once the strand is in the accent the screen
// is saying a signature exists, and that claim may not be overpainted by one
// the owner has already acknowledged to get here.
static lv_color_t bundle_col(uint8_t role, bool flagged, bool signed_ok)
{
    if (signed_ok) return wt_accent();
    if (flagged)   return WT_WARN;
    switch (role) {
    case WT_STRAND_SEND:   return WT_INK;
    case WT_STRAND_FEE:    return WT_DIM;
    case WT_STRAND_CHANGE: return wt_accent();
    default:               return WT_MUT;
    }
}

static lv_obj_t *bundle_strand(lv_obj_t *par, const wt_strand_t *s,
                               lv_point_precise_t *pts, int npts, int px)
{
    lv_obj_t *l = lv_line_create(par);
    lv_obj_set_pos(l, 0, 0);
    lv_line_set_points(l, pts, (uint32_t)npts);
    lv_obj_set_style_line_width(l, px, 0);
    lv_obj_set_style_line_color(l, bundle_col(s->role, s->flagged, s->signed_ok), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    if (s->is_group) {
        // Many coins must never read as one coin. This is the only dashed line
        // on the device, and it only renders because the row it sits on is the
        // junction row -- see the geometry note above.
        lv_obj_set_style_line_dash_width(l, 3, 0);
        lv_obj_set_style_line_dash_gap(l, 5, 0);
    }
    // A flagged change strand is amber, not the accent, so it must NOT carry the
    // flag that repaints the accent: a theme switch would put the accent back
    // over a caution the screen is still making.
    if ((s->role == WT_STRAND_CHANGE && !s->flagged) || s->signed_ok)
        lv_obj_add_flag(l, WT_FLAG_ACCENT);
    return l;
}

// One row of text beside a strand, as a flex line so a label and an amount can
// be two different faces on the same baseline: the group row is proportional
// words plus a monospaced total, and the output rows are a monospaced amount
// plus proportional words. `end` right aligns the row in its lane, which is
// what the input side needs and the output side must not have.
static lv_obj_t *bundle_row(lv_obj_t *box, int x, int y, int w, bool end)
{
    lv_obj_t *r = lv_obj_create(box);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, LV_SIZE_CONTENT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, end ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    // Content sized, so it never actually clips anything -- and saying so
    // matters beyond drawing. A clip is what decides whether a cut off label is
    // "below a fold the reader can scroll" or "text nobody can ever read", and
    // the answer is taken from the nearest clipping ancestor. Left unsaid, a
    // row that clips nothing still answers that question, with its own
    // unscrollable self, for a column that scrolls perfectly well.
    lv_obj_add_flag(r, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_pos(r, x, y);
    return r;
}

// A STRAND NOTE on the sign graph -- "#0", "no change", "out of reach #99999"
// -- hung off the lines of a drawing that already fills 64..390 and is device
// tested. Declared font14 when the caller asks for it: these annotate a figure
// rather than being read as prose, the figure has no pixels to give, and the
// numbers the owner actually reads are mono23 and mono28 beside them.
static lv_obj_t *bundle_txt(lv_obj_t *row, const char *s, const lv_font_t *f,
                            lv_color_t col, bool accent)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    if (accent) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    if (f == wt_font14() || f == wt_font_mono14()) wt_tiny_ok(l);
    return l;
}

// Point every output strand at where its row actually IS, measured rather than
// assumed, and let the box clip whatever leaves the band.
//
// This is the rule the scrolling column has to obey and the reason the strands
// are recomputed instead of scrolled: the junction end is fixed and the row end
// is not, so the two halves of one line move differently. Scrolling the strands
// with the column would carry the junction off with them. Clamping the row end
// to the edge would be worse than either -- the strand would appear to arrive
// somewhere its row is not, which is the one thing a line between two facts may
// never do. So it is drawn to the true position and cut where it leaves.
// THE FAN STAYS WHOLE. A strand aimed at a row that is not on this page used to
// be clipped at the graph's edge and simply vanish, so seven outputs drew four
// strands and the reader could not tell that was not all of them. The picture
// is the thing this device has that Coldcard, Passport, Jade and Trezor do not
// -- they step one output per screen and never draw the flow -- so losing it
// mid-read gives away the only advantage.
//
// Every output gets a strand on every page now. An ON-PAGE strand ends at its
// row, in full ink. An OFF-PAGE one ends at an evenly spaced point on the
// band's right edge, the same BMARG..h-BMARG distribution the INPUT side
// already uses, drawn at the resting mute.
//
// Thickness never changes between the two, and that is what makes this legal:
// wt_bundle is handed max_sats for the WHOLE transaction precisely so a strand
// means the same thing whatever is on screen.
static void bundle_relink(wt_bundle_t *b)
{
    if (!b->col) return;
    // Row positions are box coordinates; the strands live in sbox, which is the
    // graph BAND with no padding, so everything crossing over loses BPAD. No
    // scroll offset enters this: the column pages by HIDING the rows that are
    // not on the page, so its scroll is zero for the whole of its life.
    const int cy = lv_obj_get_y(b->col) - BPAD;
    const int band = b->bh > 0 ? b->bh : 1;
    for (uint16_t i = 0; i < b->n_out; i++) {
        lv_obj_t *ln = b->line[b->out0 + i];
        lv_obj_t *rw = b->row[i];
        if (!ln || !rw) continue;
        // On this page means DRAWN. A page hides the rows that are not on it
        // rather than scrolling past them, because a scrolled column still
        // shows the top of the next row -- which is the slice this whole change
        // exists to remove. Asking the row is exact where asking its geometry
        // was a guess.
        const bool on = !lv_obj_has_flag(rw, LV_OBJ_FLAG_HIDDEN);
        int ry;
        if (on) {
            ry = cy + lv_obj_get_y(rw) + lv_obj_get_height(rw) / 2;
        } else {
            ry = b->n_out < 2 ? band / 2
               : BMARG + (int)i * (band - 2 * BMARG) / (int)(b->n_out - 1);
        }
        // DECIDED: an output not on this page draws NO STRAND. It used to draw
        // a dimmed one, on the reasoning that the shape of the transaction
        // should not leave while its detail is read -- and what that produced
        // was a line running to blank glass, because the row it aims at is
        // hidden. There is nothing at the end of it and nothing that says why,
        // so it reads as a destination the screen will not name: the one thing
        // this graph exists to never do. It was reported from the bench as a
        // strand "going to nowhere", twice, once about its colour and once
        // about the strand itself.
        //
        // What is lost is the fan on a paged spend, and the counter on the
        // caption line carries that instead -- 1/2 is on the glass beside
        // WHERE IT GOES, and the read-to-the-end gate holds the slide until
        // every page has been turned, so no signature can happen from one
        // page's worth of strands.
        lv_obj_set_style_line_color(ln, bundle_col(b->role[b->out0 + i],
                                                   b->flag[b->out0 + i],
                                                   false), 0);
        if (on) lv_obj_remove_flag(ln, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(ln, LV_OBJ_FLAG_HIDDEN);
        lv_point_precise_t *pp = b->pts + (size_t)(b->out0 + i) * BSEG;
        int npts = BSEG;
        if (ry == b->jy) {
            pp[0].x = BJ_X; pp[0].y = b->jy;
            pp[1].x = BO_X; pp[1].y = ry;
            npts = 2;
        } else {
            bundle_curve(pp, BJ_X, b->jy, BO_X, ry, 80, 20);
        }
        lv_line_set_points(ln, pp, (uint32_t)npts);
    }
}

static wt_bundle_t *bundle_state(lv_obj_t *bundle);

// The page API. wt_bundle_page_set is the only way the column moves now: the
// column is not scrollable, so there is no free offset for anything else to
// leave it at.
int wt_bundle_pages(lv_obj_t *bundle)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    return b && b->npage ? b->npage : 1;
}

int wt_bundle_page(lv_obj_t *bundle)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    return b ? b->page : 0;
}

// Make every output row that carries an address the tap target for it. With
// several recipients there was no way to reach a full address from the verify
// screen at all -- the card that was the target only ever existed for one.
void wt_bundle_addr_tap(lv_obj_t *bundle, lv_event_cb_t cb)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    if (!b || !cb) return;
    // On the ADDRESS LINE, not the whole row. The row's other line is the
    // amount, and every amount on this device is already a control: tapping
    // one flips sats and BTC across the whole device. A handler on the row
    // would have been shadowed by that on the half of it a finger is most
    // likely to land on, which is exactly what happened -- the press flipped
    // the unit and the card never opened.
    //
    // Two lines, two controls, and each one is the thing under the finger.
    for (uint16_t i = 0; i < b->n_out; i++) {
        if (!b->fold[i] || !b->addr[i]) continue;
        lv_obj_add_flag(b->fold[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(b->fold[i], 8);
        lv_obj_set_style_translate_x(b->fold[i], 0, 0);
        lv_obj_set_style_translate_x(b->fold[i], 4, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b->fold[i], cb, LV_EVENT_CLICKED,
                            (void *)b->addr[i]);
    }
}

// A page HIDES the rows that are not on it. Scrolling to a boundary was the
// first attempt and it does not work: a scrolled column still draws the top of
// the next row, which is the slice this change exists to remove. Hidden rows
// leave the flex column laying out only what is on the page, from the top, so
// the page genuinely IS the window.
void wt_bundle_page_set(lv_obj_t *bundle, int page)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    if (!b || !b->col || !b->npage) return;
    if (page < 0) page = 0;
    if (page >= b->npage) page = b->npage - 1;
    b->page = (uint8_t)page;
    const uint16_t first = b->prow[b->page], last = (uint16_t)b->ptop[b->page];
    for (uint16_t i = 0; i < b->n_out; i++) {
        if (!b->row[i]) continue;
        if (i >= first && i <= last) lv_obj_remove_flag(b->row[i], LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(b->row[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_update_layout(b->col);
    bundle_relink(b);
}

lv_obj_t *wt_bundle(lv_obj_t *scr, int x, int y, int w, int h,
                    const wt_strand_t *in,  size_t n_in,
                    const wt_strand_t *out, size_t n_out,
                    uint64_t max_sats)
{
    if (n_in > WT_BUNDLE_MAX)  n_in  = WT_BUNDLE_MAX;
    if (n_out > WT_BUNDLE_MAX) n_out = WT_BUNDLE_MAX;

    wt_bundle_t *b = lv_malloc(sizeof *b);
    if (!b) return NULL;
    lv_memzero(b, sizeof *b);
    b->pts = lv_malloc(sizeof(lv_point_precise_t) * BSEG * (n_in + n_out));
    if (!b->pts) { lv_free(b); return NULL; }
    // Inputs only: the hold is about the coins being committed, and the output
    // side is already standing down by the time any of this moves.
    b->hpts = lv_malloc(sizeof(lv_point_precise_t) * BSEG * (n_in ? n_in : 1));
    if (!b->hpts) { lv_free(b->pts); lv_free(b); return NULL; }

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y - BPAD);
    lv_obj_set_size(box, w, h + 2 * BPAD);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(box, bundle_delete_cb, LV_EVENT_DELETE, b);

    const int jy = BPAD + h / 2;
    char amt[32];

    // ONE input row is the input TOTAL, and the total is half the arithmetic a
    // reader does on this screen: what came in, less what goes out, is the fee.
    // So it takes the same rung as the amounts on the other side of the
    // junction rather than the rung of a breakdown line. Reported from the
    // bench as the input total being the smallest text on the screen, which it
    // was: mono14, against mono23 outputs 300px to its right.
    //
    // More than one row and they are a BREAKDOWN -- these coins, this size
    // each -- so they stay at mono14 and the caller puts the total on the
    // caption line above them, where there is room for it at mono23 and where
    // no coin row can be mistaken for the sum.
    const lv_font_t *in_f = (n_in == 1) ? wt_font_mono23() : wt_font_mono14();

    // The input lane, measured rather than assumed: the group row carries a
    // count AND a total on one line, which is why frame 3a's lane is twice
    // frame 2c's.
    int lane = BLANE_MIN;
    for (size_t i = 0; i < n_in; i++) {
        lv_point_t ts;
        int wid = 0;
        wt_fmt_amount(in[i].sats, amt, sizeof amt);
        lv_text_get_size(&ts, amt, in_f, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        wid = ts.x;
        if (in[i].label) {
            lv_text_get_size(&ts, in[i].label, wt_font14(), 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            wid += ts.x + 8;                  // + bundle_row's pad_column
        }
        if (wid > lane) lane = wid;
    }
    if (lane > BLANE_MAX) lane = BLANE_MAX;
    const int ix0 = lane + BLANE_GAP;

    lv_point_precise_t *pp = b->pts;
    const int in_lh  = lv_font_get_line_height(in_f);
    const int out_lh = lv_font_get_line_height(wt_font_mono23());

    for (size_t i = 0; i < n_in; i++) {
        const int ry = BPAD + (n_in < 2 ? h / 2
                        : BMARG + (int)i * (h - 2 * BMARG) / (int)(n_in - 1));
        int npts = BSEG;
        if (ry == jy) {                       // flat: two points, so it dashes
            pp[0].x = ix0;  pp[0].y = ry;
            pp[1].x = BJ_X; pp[1].y = jy;
            npts = 2;
        } else {
            bundle_curve(pp, ix0, ry, BJ_X, jy, 43, 62);
        }
        const int k = b->n_line;
        const int px = wt_strand_px(in[i].sats, max_sats);
        b->line[k] = bundle_strand(box, &in[i], pp, npts, px);
        b->role[k] = in[i].role;
        b->flag[k] = in[i].flagged;
        b->nseg[k] = (uint8_t)npts;
        // The overlay, built now and hidden, so a tick allocates nothing and
        // creates nothing. Same width and same dash as the strand underneath:
        // a dashed strand means MANY COINS, and a solid accent line drawn over
        // it during the hold would say one coin was committing.
        b->hline[k] = bundle_strand(box, &in[i], pp, npts, px);
        lv_obj_set_style_line_color(b->hline[k], wt_accent(), 0);
        lv_obj_add_flag(b->hline[k], WT_FLAG_ACCENT);
        lv_obj_add_flag(b->hline[k], LV_OBJ_FLAG_HIDDEN);
        pp += BSEG;

        const bool acc = in[i].signed_ok;
        lv_obj_t *row = bundle_row(box, 0, ry - in_lh / 2, lane, true);
        wt_fmt_amount(in[i].sats, amt, sizeof amt);
        // One input is the whole input side, and it was the only figure in the
        // graph with no word anywhere near it: the caller puts the unit on the
        // input TOTAL, and with one coin there is no separate total to put it
        // on. It came back from the bench as "what are they, sat amounts?".
        //
        // The caller labels it TOTAL through in[0].label instead. Both will not
        // fit: the lane is capped at BLANE_MAX so the strands keep their run to
        // the junction, and the word plus the figure plus the unit is past it.
        // The word wins -- the hero four lines up already carries the unit, and
        // what the row needed was to say WHAT it is, not what it is counted in.
        if (in[i].label)                      // the group row: words, then the total
            b->note[k] = bundle_txt(row, in[i].label, wt_font14(),
                                    acc ? wt_accent() : WT_MUT, acc);
        // WT_INK when this row IS the total, WT_MUT when it is one coin of
        // several: the number a reader has to READ is never the dimmest thing
        // on the screen, and the breakdown under a total is not that number.
        // The signature still lands on it -- SIGNING already paints inputs
        // WT_INK and the reveal crosses from there to the accent, so a total
        // that starts at WT_INK loses a step it never used and keeps the one
        // that says a signature exists.
        b->amount[k] = bundle_txt(row, amt, in_f,
                                  acc ? wt_accent()
                                      : (n_in == 1 ? WT_INK : WT_MUT), acc);
        wt_denom_bind(b->amount[k]);   // every figure is the switch, not one
        // The unit, on the one-coin row only. Muted and outside the repaint
        // arrays on purpose: it is a suffix, not a figure, so it stays WT_MUT
        // when the signature lands exactly as the hero's own "sats" does.
        b->n_line++;
    }

    b->n_in = b->n_line;

    // ---- the output column ----
    //
    // Outputs are NEVER elided, at any count. Bundling inputs is safe -- they
    // are all yours and their total is the fact -- but each output is a place
    // your money goes, and one folded into a group would be a destination
    // visible nowhere. The column scrolls instead, exactly as the panel it
    // replaces did, and the caller keeps its read-to-the-end gate.
    //
    // Rows are content-sized inside a flex column rather than placed at
    // computed y's, because one of them may be a paragraph: a silent payment
    // says a second thing about itself. Their real positions are read back
    // after layout, which is also what makes the strands correct while
    // scrolling.
    // The pitch is set in TWO PASSES, because a row's height is not knowable
    // until it is built: a row is an amount, and now also a label and a folded
    // address, and a silent payment row is a paragraph on top of that. Pass one
    // builds with no gap; pass two measures what was built and distributes the
    // slack. Assuming one mono23 line here -- which is what this did -- spread
    // two-line rows across a band they then overflowed, and the last
    // destination fell onto a second page of a three-output transaction.
    const int row_h = out_lh;
    b->out0 = b->n_line;
    b->n_out = (uint16_t)n_out;
    b->jy    = (int16_t)(jy - BPAD);      // sbox coordinates

    // The output strands get their own clip, and it is not the box. The box
    // carries BPAD of slack top and bottom so a label centred on the first row
    // is not cut in half -- but a strand aimed at a row that has scrolled out
    // of view must stop at the GRAPH, not BPAD above it, or it runs through the
    // caption sitting on that line. Same reason the strand is clipped rather
    // than clamped: where it stops has to be a boundary of the drawing, not a
    // number chosen to make it look tidy.
    lv_obj_t *sbox = lv_obj_create(box);
    lv_obj_remove_style_all(sbox);
    lv_obj_set_pos(sbox, 0, BPAD);
    lv_obj_set_size(sbox, w, h);
    lv_obj_remove_flag(sbox, LV_OBJ_FLAG_SCROLLABLE);
    b->sbox = sbox;

    // The column's own box is the clip, and where it starts matters twice over.
    //
    // It may not reach the box's top edge: the box carries BPAD of slack, and a
    // row scrolled to the top of a full column would rise into it and share
    // pixels with the caption sitting on that line -- "WHERE IT GOES" and a
    // recipient's amount, overlapping, on the screen that says where the money
    // goes. And it may not end on the box's edge either: a row cut by the BOX
    // is a row cut by something that does not scroll, which reads as text
    // clipped rather than text below a fold, and the gate says so.
    //
    // So the column clips itself, strictly inside the box, and starts 2px above
    // the first row's band so the rows still land on the spread the appendix
    // draws rather than half a line height below it.
    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, BL_X, BPAD + BMARG - row_h / 2);
    lv_obj_set_size(col, w - BL_X, h + row_h - 2 * BMARG);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 0, 0);           // pass two sets the real gap
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_style_width(col, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(col, WT_MUT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(col, LV_OPA_50, LV_PART_SCROLLBAR);
    b->col = col;

    for (size_t i = 0; i < n_out; i++) {
        const int k = b->n_line;
        // Placed at the junction for now; bundle_relink puts every one of them
        // on its row once the column has been laid out.
        bundle_curve(pp, BJ_X, jy - BPAD, BO_X, jy - BPAD, 80, 20);
        if (!out[i].note_only) {
            b->line[k] = bundle_strand(sbox, &out[i], pp, BSEG,
                                       wt_strand_px(out[i].sats, max_sats));
            b->role[k] = out[i].role;
            b->flag[k] = out[i].flagged;
        }
        pp += BSEG;

        // A COLUMN, because a row may be more than one line: an amount and its
        // word, then the address it pays. The strand aims at the whole row's
        // middle, so a row that grows stays joined to its line.
        const int roww = w - BL_X - 8;             // 8 clear of the scrollbar
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, roww);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   // see bundle_row
        b->row[i] = row;

        if (out[i].note_only) {           // words only: the row without an output
            lv_obj_t *n = bundle_txt(row, out[i].label ? out[i].label : "",
                                     wt_font14(), WT_MUT, false);
            lv_obj_set_width(n, roww);
            lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
            b->note[k] = n;
            b->n_line++;
            continue;
        }

        lv_obj_t *line = lv_obj_create(row);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, roww);
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(line, 8, 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // see bundle_row

        const bool acc = (out[i].role == WT_STRAND_CHANGE);
        // The STRAND is DIM for a fee and INK for the send -- that is the
        // thickness reading, and it is in the appendix. The AMOUNT is ink
        // whatever the row, because WT_DIM is the colour of something present
        // but inert, and a fee is neither: it is the number an owner is most
        // likely to be checking. Only change takes the accent, and it is the
        // only accent text in the graph.
        const lv_color_t oc = acc ? wt_accent() : WT_INK;
        wt_fmt_amount(out[i].sats, amt, sizeof amt);
        b->amount[k] = bundle_txt(line, amt, wt_font_mono23(), oc, acc);
        wt_denom_bind(b->amount[k]);
        // The MARK takes the accent and the WORDS stay muted, on every row.
        // Change used to be the one row whose label was accent too, which drew
        // the whole row in the theme colour -- and it is the AMOUNT that comes
        // back to the owner, not the word for it. So the accent lands on the
        // figure and on the marks, and no row's words are louder than another's.
        //
        // ONE label, with LVGL's inline RECOLOR doing what two objects would --
        // the same trick wt_state_chip uses, and for the same reason it gives
        // there: two objects in this row is a second child in a flex line and
        // the walk's coordinate taps land somewhere else.
        //
        // The colours are the right way round for a THEME. The label's own
        // colour is the accent and carries the MARK, so WT_FLAG_ACCENT
        // repaints it when the theme changes; the words are pinned to WT_MUT
        // by markup, which never goes stale because WT_MUT never moves. The
        // other way round would bake the accent into a string and leave it
        // behind the first time somebody switches theme on this screen.
        if (out[i].mark) {
            char m[192];
            size_t o = 0;
            o += (size_t)snprintf(m, sizeof m, "%s", out[i].mark);
            // CHANGE keeps its WORD in the accent too. It is the one row whose
            // subject is money coming BACK, and the bench asked for the whole
            // of it lit rather than the figure alone -- in the theme's colour,
            // which is what the accent has always been.
            // NO MARKUP ON THE CHANGE ROW. The comment above states the rule
            // -- the object's own colour is the accent and a theme change
            // repaints it, the words are pinned to WT_MUT by markup "which
            // never goes stale because WT_MUT never moves" -- and the change
            // row broke it by pinning its words to wt_accent() instead, which
            // is exactly the string with the accent baked into it that the
            // rule exists to prevent. It was still MONO blue on a GREEN
            // screen, beside a strand that had repainted correctly.
            //
            // The change row wants its MARK and its WORD both in the accent,
            // and that is what a plain label already is: no markup, the whole
            // string takes the object's colour, and WT_FLAG_ACCENT repaints
            // all of it. bundle_note_ink then finds no '#' and leaves it to
            // the object colour, which is the right answer for a stand down
            // too.
            const lv_color_t wc = WT_MUT;
            if (acc) {
                /* whole label in the accent: mark and word together */
            } else if (out[i].label && o + 2 < sizeof m) {
                o += (size_t)snprintf(m + o, sizeof m - o, "  #%02X%02X%02X ",
                                      wc.red, wc.green, wc.blue);
                // '#' opens a colour run, so a literal one -- the change row's
                // address index -- has to be doubled or LVGL eats the digits
                // after it as a colour.
                for (const char *q = out[i].label; *q && o + 3 < sizeof m; q++) {
                    if (*q == '#') m[o++] = '#';
                    m[o++] = *q;
                }
                if (o + 2 < sizeof m) m[o++] = '#';
                m[o] = 0;
            }
            if (acc && out[i].label && o + 2 < sizeof m)
                snprintf(m + o, sizeof m - o, "  %s", out[i].label);
            b->note[k] = bundle_txt(line, m, wt_font14(), wt_accent(), true);
            if (!acc) lv_label_set_recolor(b->note[k], true);
        } else if (out[i].label) {
            b->note[k] = bundle_txt(line, out[i].label, wt_font14(),
                                    WT_MUT, false);
        }
        // A destination these keys have paid before. A bare mark, no word: the
        // row already carries an amount, a label and an address, and the one
        // thing being added is "you have been here". The words for it are on
        // the address card behind the "?" -- see kiss_payee.h for why nothing
        // is drawn on a first payment.
        if (out[i].known)
            bundle_txt(line, LV_SYMBOL_REFRESH, wt_font14(), WT_MUT, false);
        // THE FOLD, the same one RECEIVE and the single-recipient card draw:
        // prefix, the four after it, an ellipsis, the last twelve with the
        // final eight lit. One line per destination, whatever its length.
        //
        // It used to be the whole address wrapped in a 304px lane, which took
        // two lines for a bech32 and four for a silent payment -- so a column
        // sized for three rows held one and a half destinations, and the row at
        // the fold was cut through the middle of an address rather than at a
        // line. Folding is what makes a row a row here.
        //
        // Same habit on every screen that shows a destination, which is the
        // whole reason the single-recipient card folds too.
        // mono18, up a rung from 14. The band grew by the height of the
        // address card it replaced, so a row can afford the size the thing it
        // carries is read at -- character by character, against a coordinator.
        if (out[i].addr) {
            b->fold[i] = wt_addr_short(row, out[i].addr, wt_font_mono18());
            b->addr[i] = out[i].addr;
        }
        b->n_line++;
    }

    // Measured, not counted: whether this column overflows depends on the
    // locale and on whether any row is a paragraph, which no output count can
    // answer. MODE_ON rather than AUTO for the same reason the panel used it --
    // a list with more below the fold must not look identical to one that ends
    // there.
    // PASS TWO: the rows exist, so their real height is knowable, and both the
    // gap and the page boundaries come off it. A row is an amount, a label and
    // a folded address, and a silent payment row is a paragraph on top of that
    // -- assuming one mono23 line here, which is what this did, spread two-line
    // rows across a band they then overflowed.
    lv_obj_update_layout(col);
    b->bh    = (int16_t)h;
    b->page  = 0;
    b->npage = 0;
    {
        const int colh = lv_obj_get_height(col);
        // TWO gaps, because one number was doing two jobs and they disagree.
        // GAP_MIN is the tightest spacing a reader can still tell two rows
        // apart at; GAP_PAGED is what the column prefers when it cannot spread.
        //
        // The split below measures at the FLOOR, so a page is cut only when the
        // ROWS do not fit -- never when the SPACING does not. That distinction
        // is the whole bug it fixes: an ordinary spend is a recipient, a fee
        // and change (49 + 25 + 25 = 99), the column is 111 tall with a caution
        // bar under the graph, and at ten pixels between rows it needs 119. So
        // the commonest FLAGGED transaction on the device put the money coming
        // back to its owner on a second page, and missed the first by eight
        // pixels of decoration.
        //
        // Nothing above it can give those pixels back: the graph's box is
        // h + 2 * BPAD, so at SG_GRAPH_H_C it already reaches 298 against the
        // caution bar's hairline at 289, and the bar cannot move down either --
        // 290 + 44 is two clear of the slide band. SG_GRAPH_H_C is 8 less than
        // SG_GRAPH_H for exactly that reason.
        const int GAP_MIN   = 4;
        const int GAP_PAGED = 10;
        int content = 0;
        for (uint16_t i = 0; i < b->n_out; i++)
            if (b->row[i]) content += lv_obj_get_height(b->row[i]) + GAP_MIN;
        content -= content ? GAP_MIN : 0;

        // Cut a page where the next row would not fit WHOLE. At least one row
        // per page whatever its height: a paragraph taller than the band still
        // has to be reachable, and a page holding nothing would never advance.
        // prow is the first row on each page, ptop the last.
        uint16_t i = 0;
        while (i < b->n_out && b->npage < WT_BUNDLE_MAX) {
            const uint16_t first = i;
            int used = 0;
            b->prow[b->npage] = (uint8_t)i;
            while (i < b->n_out) {
                const int rh  = b->row[i] ? lv_obj_get_height(b->row[i]) : 0;
                const int add = used ? GAP_MIN + rh : rh;
                if (i > first && used + add > colh) break;
                used += add;
                i++;
            }
            b->ptop[b->npage] = (int16_t)(i - 1);
            b->npage++;
        }
        if (!b->npage) b->npage = 1;

        // Everything fits: distribute the slack so three destinations read as a
        // list down the band rather than bunched at its top. Measured from the
        // same floor `content` was, so the arithmetic is invariant -- every
        // column that already fit renders at exactly the gap it did before, and
        // the one that did not lands on the tightest gap that holds it.
        if (b->npage == 1 && n_out > 1) {
            int g = (colh - content) / (int)(n_out - 1) + GAP_MIN;
            if (g > 28) g = 28;             // a list, not a scatter
            if (g < GAP_MIN) g = GAP_MIN;
            lv_obj_set_style_pad_row(col, g, 0);
        } else {
            lv_obj_set_style_pad_row(col, GAP_PAGED, 0);
        }
    }

    // NOT SCROLLABLE. The point of paging is that a finger cannot leave the
    // column resting between two rows, and leaving the flag on is exactly how
    // it would.
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    wt_bundle_page_set(box, 0);

    // The junction, last, so it sits over every strand that reaches it. A dot
    // rather than a joint: the strands genuinely meet here, and a gap where
    // eleven pixels of stroke cross two of stroke reads as a rendering fault.
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, BJ_R * 2, BJ_R * 2);
    lv_obj_set_pos(dot, BJ_X - BJ_R, jy - BJ_R);
    lv_obj_set_style_radius(dot, BJ_R, 0);
    lv_obj_set_style_bg_color(dot, WT_INK, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    b->dot = dot;
    return box;
}

// The state block is hung off the delete callback rather than user_data, which
// wt_screen already spends on its own tag. Reading it back through the same
// event is the one place that is not a layering violation.
static wt_bundle_t *bundle_state(lv_obj_t *bundle)
{
    uint32_t n = lv_obj_get_event_count(bundle);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(bundle, i);
        if (lv_event_dsc_get_cb(d) == bundle_delete_cb)
            return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

// Two accent flags, not one, because a flagged change strand splits them: the
// AMOUNT still comes back to the owner and keeps the accent, while the LINE is
// amber and must not be repainted by a theme switch. Every other state passes
// the same value twice.
static void bundle_repaint(wt_bundle_t *b, int k, lv_color_t line_col,
                           lv_color_t txt_col, bool line_acc, bool txt_acc)
{
    if (b->line[k]) {
        lv_obj_set_style_line_color(b->line[k], line_col, 0);
        if (line_acc) lv_obj_add_flag(b->line[k], WT_FLAG_ACCENT);
        else          lv_obj_remove_flag(b->line[k], WT_FLAG_ACCENT);
    }
    lv_obj_t *t[2] = { b->amount[k], b->note[k] };
    for (int i = 0; i < 2; i++) {
        if (!t[i]) continue;
        lv_obj_set_style_text_color(t[i], txt_col, 0);
        if (txt_acc) lv_obj_add_flag(t[i], WT_FLAG_ACCENT);
        else         lv_obj_remove_flag(t[i], WT_FLAG_ACCENT);
    }
}

// The folded address stands down with the row it belongs to.
//
// It is a SPANGROUP, so bundle_repaint above never reached it -- the spans
// carry their own colours and a text colour on the group is invisible, exactly
// like the line colour case that comment already describes. Until this existed
// the output column half dimmed: the amount and the note went to WT_EDGE while
// the address beside them kept a WT_MUT head and a full strength accent tail.
//
// From the bench, holding the slide on a twenty input merge: "why is this
// screen so blurry". That is what half a column at one ink and half at another
// looks like -- not a state, a rendering fault. The stand down is meant to say
// the destinations are settled, and an address is the most destination-like
// thing on the row.
//
// wt_addr_short returns a plain label for anything under 20 characters -- a
// locked-session message, an error string -- so the type is checked rather
// than assumed.
static void bundle_fold_ink(wt_bundle_t *b, int i, bool dim)
{
    lv_obj_t *sg = (i >= 0 && i < WT_BUNDLE_MAX) ? b->fold[i] : NULL;
    if (!sg || !lv_obj_check_type(sg, &lv_spangroup_class)) return;
    const uint32_t n = lv_spangroup_get_span_count(sg);
    for (uint32_t k = 0; k < n; k++) {
        lv_span_t *sp = lv_spangroup_get_child(sg, (int32_t)k);
        if (!sp) continue;
        // The LAST span is the lit one by construction -- the same fact
        // accent_walk relies on for every address on this device.
        const bool lit = (k + 1 == n);
        lv_style_set_text_color(lv_span_get_style(sp),
                                dim ? WT_EDGE : (lit ? wt_accent() : WT_MUT));
    }
    // And the FLAG follows the ink. A theme switched while a finger is down
    // would otherwise walk this group and paint the tail back up to the new
    // accent, one lit run in a column that has stood down.
    if (dim) lv_obj_remove_flag(sg, WT_FLAG_ACCENT);
    else     lv_obj_add_flag(sg, WT_FLAG_ACCENT);
    if (n) lv_spangroup_refresh(sg);
}

// The note's WORDS follow the row too, and they are the second half of the same
// defect the fold had. An output note is a RECOLOR label: the object's own
// colour carries the MARK so a theme change repaints it, and the words are
// pinned by `#RRGGBB ...#` markup a few characters into the string. So
// bundle_repaint's text colour moved the scissors and left NETWORK FEE where it
// was -- measured at WT_MUT (123,133,156) with the amount beside it at WT_EDGE
// (41,48,65), on a row that is supposed to have stood down.
//
// The six hex digits are patched in place rather than the string rebuilt: the
// label owns the only copy of its own words -- out[i].label is the caller's and
// is not kept -- and turning recolor OFF is not an option, LVGL then draws the
// markup as literal text.
//
// The FIRST '#' is always the colour opener: the mark carries no '#', and a
// literal one in the words is doubled by the builder above.
static void bundle_note_ink(wt_bundle_t *b, int k, lv_color_t c)
{
    lv_obj_t *n = b->note[k];
    if (!n) return;
    char buf[192];
    snprintf(buf, sizeof buf, "%s", lv_label_get_text(n));
    char *h = strchr(buf, '#');
    if (!h || strlen(h) < 7) return;          // a plain note: the colour did it
    static const char HEX[] = "0123456789ABCDEF";
    const uint8_t v[3] = { c.red, c.green, c.blue };
    for (int i = 0; i < 3; i++) {
        h[1 + i * 2] = HEX[(v[i] >> 4) & 0xF];
        h[2 + i * 2] = HEX[v[i] & 0xF];
    }
    lv_label_set_text(n, buf);
}

// The junction, sized and placed from one radius. Its centre never moves; only
// the radius does, so the grow and the retract are the same line of arithmetic.
static void bundle_dot_r(wt_bundle_t *b, int r)
{
    if (!b->dot) return;
    lv_obj_set_size(b->dot, r * 2, r * 2);
    lv_obj_set_pos(b->dot, BJ_X - r, b->jy + BPAD - r);
    lv_obj_set_style_radius(b->dot, r, 0);
}

void wt_bundle_hold(lv_obj_t *bundle, uint8_t progress)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    if (!b) return;

    for (uint16_t k = 0; k < b->n_in; k++) {
        lv_obj_t *ov = b->hline[k];
        if (!ov) continue;
        if (!progress) { lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN); continue; }

        // Walk the table the strand was already sampled into at build time.
        // The cubic is not re-evaluated here: sixteen points per strand exist
        // precisely so a tick every 30ms is a copy and one interpolation, not
        // four multiplies per point on a chip with no FPU worth using.
        //
        // n is not always BSEG. An input whose row lands ON the junction row is
        // written as two points so it can dash, and at twenty inputs that flat
        // row is the grouped strand -- the common case, not the corner.
        const int n = b->nseg[k];
        const lv_point_precise_t *src = b->pts  + (size_t)k * BSEG;
        lv_point_precise_t       *dst = b->hpts + (size_t)k * BSEG;
        const int32_t t   = (int32_t)progress * (n - 1);
        const int     q   = t / 255;
        const int32_t rem = t - (int32_t)q * 255;
        int npts = q + 1;
        for (int i = 0; i <= q; i++) dst[i] = src[i];
        if (q < n - 1) {
            // The head of the line, between two sampled points. Without it the
            // strand would advance a vertex at a time and read as sixteen steps
            // rather than one continuous reach.
            const int32_t ax = (int32_t)src[q].x,     ay = (int32_t)src[q].y;
            const int32_t bx = (int32_t)src[q + 1].x, by = (int32_t)src[q + 1].y;
            dst[q + 1].x = (lv_value_precise_t)(ax + (bx - ax) * rem / 255);
            dst[q + 1].y = (lv_value_precise_t)(ay + (by - ay) * rem / 255);
            npts = q + 2;
        }
        lv_line_set_points(ov, dst, (uint32_t)npts);
        lv_obj_remove_flag(ov, LV_OBJ_FLAG_HIDDEN);
    }

    bundle_dot_r(b, BJ_R + (BJ_HOLD_R - BJ_R) * progress / 255);
}

void wt_bundle_state(lv_obj_t *bundle, int state)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    if (!b) return;
    // SIGNING keeps the overlay exactly where the hold left it -- full length,
    // in the accent. Hiding it there would paint the inputs back down to INK
    // for the few hundred milliseconds libwally is busy and then up to the
    // accent again, and a strand that dims at the moment of commitment says the
    // opposite of what happened. LIVE and SIGNED both take it away, and SIGNED
    // does it in the same call that paints the strands underneath, so the
    // pixels do not change.
    if (state != WT_BUNDLE_SIGNING && state != WT_BUNDLE_HOLDING)
        for (uint16_t k = 0; k < b->n_in; k++)
            if (b->hline[k]) lv_obj_add_flag(b->hline[k], LV_OBJ_FLAG_HIDDEN);
    if (state == WT_BUNDLE_LIVE) bundle_dot_r(b, BJ_R);
    if (b->dot) {
        // Every input strand arrives in the accent, so the thing they arrive AT
        // wears it too. Eleven pixels of accent stroke ending on a white disc
        // is the seam this dot exists to prevent, and it would appear at the
        // one moment the drawing is being read hardest.
        const bool acc = (state == WT_BUNDLE_SIGNED);
        lv_obj_set_style_bg_color(b->dot, acc ? wt_accent() : WT_INK, 0);
        if (acc) lv_obj_add_flag(b->dot, WT_FLAG_ACCENT_FILL);
        else     lv_obj_remove_flag(b->dot, WT_FLAG_ACCENT_FILL);
    }
    bool live_out = false;
    for (int k = 0; k < (int)b->n_line; k++) {
        const bool is_in = (k < (int)b->out0);
        if (state == WT_BUNDLE_HOLDING) {
            // Outputs stand down, and so does a flagged input's warn colour --
            // to the plain mute, not up to WT_INK. Both directions matter and
            // for the same reason: the fill drawn over these is the accent, and
            // it needs something to be visible against. WT_INK is too close to
            // the accent in MONO, and WT_WARN's amber is too close to it in
            // ORANGE -- on a merge, in that theme, the whole animation
            // disappeared into a strand that was already orange.
            //
            // The caution is not being retracted: the bar below still says it,
            // it has already been acknowledged to get here, and the strands
            // wear it again the moment the hold is let go. What the screen is
            // about for these 1200ms is the commitment, not the warning.
            bundle_repaint(b, k, is_in ? WT_MUT : WT_EDGE,
                           is_in ? (b->n_in == 1 ? WT_INK : WT_MUT) : WT_EDGE,
                           false, false);
            if (!is_in) {
                bundle_fold_ink(b, k - (int)b->out0, true);
                bundle_note_ink(b, k, WT_EDGE);
            }
        } else if (state == WT_BUNDLE_SIGNING) {
            // Inputs at full strength, outputs stood down. The note rows go with
            // their side: a silent payment's claim is about an output, so it
            // dims with the output it belongs to.
            bundle_repaint(b, k, is_in ? WT_INK : WT_EDGE,
                           is_in ? WT_INK : WT_EDGE, false, false);
            if (!is_in) {
                bundle_fold_ink(b, k - (int)b->out0, true);
                bundle_note_ink(b, k, WT_EDGE);
            }
        } else if (state == WT_BUNDLE_SIGNED && is_in) {
            bundle_repaint(b, k, wt_accent(), wt_accent(), true, true);
        } else if (!is_in) {
            // Back to what the row means, taken from its role rather than
            // remembered: the destinations are readable again the moment there
            // is a signature over them.
            // The amount keeps what it MEANS and the strand keeps what is
            // WRONG with it. A dust change output still comes back to the
            // owner, so its figure stays in the accent; the line it rides is
            // the caution.
            const bool acc = (b->role[k] == WT_STRAND_CHANGE);
            bundle_repaint(b, k, bundle_col(b->role[k], b->flag[k], false),
                           acc ? wt_accent() : (b->amount[k] ? WT_INK : WT_MUT),
                           acc && !b->flag[k], acc);
            bundle_fold_ink(b, k - (int)b->out0, false);
            live_out = true;   // the page dim below is what this just undid
            // The note's own colour is its MARK, which is the accent on every
            // row -- its words carry WT_MUT in markup. bundle_repaint paints
            // the note with the amount, so this puts the mark back.
            if (b->note[k]) {
                lv_obj_set_style_text_color(b->note[k], wt_accent(), 0);
                // ...and its WORDS back to what they were built with: WT_MUT,
                // or the accent on the one row whose subject is money coming
                // back. Same test the builder used.
                bundle_note_ink(b, k, acc ? wt_accent() : WT_MUT);
            }
        } else {
            // An input at rest, taken from its role rather than assumed to be
            // muted: a flagged one wears WT_WARN and has to come back to it
            // after a hold is let go. The LABEL stays muted either way -- the
            // strand is what the caution is about, and an amount in WT_WARN
            // would read as something wrong with that number.
            //
            // One input row is the input total and rests at WT_INK; several
            // are a breakdown and rest muted. Same test wt_bundle built them
            // under, so a hold let go puts back what was drawn.
            bundle_repaint(b, k, bundle_col(b->role[k], b->flag[k], false),
                           b->n_in == 1 ? WT_INK : WT_MUT, false, false);
        }
    }
    // The loop above paints an output from its ROLE, which knows nothing about
    // pages -- so a strand whose row is not on this page came back at full
    // strength. It only matters coming back to rest (a hold let go), which is
    // the one path that repaints outputs without the page having moved.
    if (live_out) bundle_relink(b);
}

// ---- the signature landing (see kiss_theme.h) ----
// One value, every input strand, and no per input timing anywhere in it. The
// end state is exactly what wt_bundle_state(SIGNED) paints, and it is that call
// that lands it -- this only fills in the frames between SIGNING and there.
static void bundle_reveal_exec(void *var, int32_t v)
{
    wt_bundle_t *b = bundle_state((lv_obj_t *)var);
    if (!b) return;
    const lv_color_t acc = wt_accent();
    for (int k = 0; k < (int)b->out0; k++) {         // inputs only
        // WT_INK is where SIGNING left them, which is what makes this a
        // crossing rather than a jump from whatever each strand happened to be.
        lv_color_t c = lv_color_mix(acc, WT_INK, (uint8_t)v);
        if (b->line[k])   lv_obj_set_style_line_color(b->line[k], c, 0);
        if (b->amount[k]) lv_obj_set_style_text_color(b->amount[k], c, 0);
    }
    // The junction takes the same value, so the discs and the strokes meeting
    // at it are never two different colours -- the seam wt_bundle_state's dot
    // branch exists to prevent, in the frames it does not cover.
    if (b->dot)
        lv_obj_set_style_bg_color(b->dot, lv_color_mix(acc, WT_INK, (uint8_t)v), 0);
}

static void bundle_reveal_done(lv_anim_t *a)
{
    wt_bundle_state((lv_obj_t *)a->var, WT_BUNDLE_SIGNED);
}

void wt_bundle_signed_reveal(lv_obj_t *bundle, uint32_t ms)
{
    if (!bundle || !bundle_state(bundle)) return;
    if (!ms) { wt_bundle_state(bundle, WT_BUNDLE_SIGNED); return; }
    // The hold overlay goes FIRST and on its own. It is the accent already, at
    // full length, so leaving it up would put the finished colour over strands
    // still crossing to it and the crossing would be invisible under its own
    // answer.
    wt_bundle_t *b = bundle_state(bundle);
    for (uint16_t k = 0; k < b->n_in; k++)
        if (b->hline[k]) lv_obj_add_flag(b->hline[k], LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bundle);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, ms);
    // ease_out, the same curve the explainer card enters on: fast where the
    // answer arrives, slow where it settles.
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, bundle_reveal_exec);
    lv_anim_set_completed_cb(&a, bundle_reveal_done);
    lv_anim_start(&a);
}


// Reserve exactly what this iteration writes, which is a character and, only
// on a group boundary, a space before it.
//
// The old guard reserved two every time, so a caller whose buffer was sized to
// the exact answer lost its final character. That is not a hypothetical: a
// txid is 64 hex characters, blocks to 79, and gt[80] on the DETAILS page is
// 79 plus the NUL -- correct, and the one size the over-reservation bit. The
// id rendered 63 characters long with no ellipsis and no gap, its last block
// three wide instead of four, which is what plenty of honest addresses look
// like. An owner comparing it against a coordinator matched every leading
// block and had nothing to tell them the end was missing; an owner who copied
// it down to look the transaction up later wrote down an id that matches
// nothing on chain.
//
// Every other caller had slack and is unaffected, byte for byte. fitcheck
// keeps it that way.
void wt_group4(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!out || !out_len) return;
    // A trailing group of one or two characters JOINS the group before it. An
    // address is rarely a multiple of four -- a 42 character bech32 leaves two
    // -- and that stub is a separate token, so a line that is one group too
    // long wraps it alone: the verify screen showed forty characters on one
    // line and "kz" on the next, which reads as the address being cut off.
    // Merged, the last token is five or six characters and there is no orphan
    // to strand. Nothing is dropped and no other group changes.
    size_t n = strlen(in);
    size_t rem = n % 4;
    size_t last = (rem == 1 || rem == 2) && n > 4 ? n - rem - 4 : n;
    for (size_t i = 0; in[i]; i++) {
        size_t need = (i && i % 4 == 0 && i <= last) ? 2u : 1u;
        if (o + need + 1 > out_len) break;          // +1 keeps room for the NUL
        if (need == 2) out[o++] = ' ';
        out[o++] = in[i];
    }
    out[o] = 0;
}

void wt_fmt_bytes(uint64_t bytes, char *out, size_t out_len)
{
    // One decimal, unit picked by size: a firmware image reads in MB, a card
    // in GB -- "31981.6 MB" is a number nobody can compare with the sticker on
    // the card. uint64 arithmetic throughout: the old MB-only helper multiplied
    // a size_t by ten, which overflows 32 bits past ~400 MB on the device.
    if (bytes >= 1073741824ull) {
        unsigned gb10 = (unsigned)((bytes * 10 + 536870912ull) / 1073741824ull);
        snprintf(out, out_len, "%u.%u GB", gb10 / 10, gb10 % 10);
    } else {
        unsigned mb10 = (unsigned)((bytes * 10 + 524288) / 1048576);
        snprintf(out, out_len, "%u.%u MB", mb10 / 10, mb10 % 10);
    }
}

void wt_fmt_btc(uint64_t sats, char *out, size_t out_len)
{
    // full 8 decimals, never abbreviated: this string exists to be compared
    // digit-by-digit against a coordinator that displays BTC
    snprintf(out, out_len, "%llu.%08llu",
             (unsigned long long)(sats / 100000000ULL),
             (unsigned long long)(sats % 100000000ULL));
}

// The owner's unit. Sats is the default because this is a signer and the
// numbers it shows are compared against a coordinator's transaction view,
// which counts in sats far more often than not; BTC is one row away for anyone
// whose coordinator counts the other way. It is a display preference and
// nothing else -- every amount on this device is a uint64 of satoshis, and the
// setting never reaches storage, a PSBT, or a signature.
static int s_denom = WT_DENOM_SATS;
int  wt_denom(void)        { return s_denom; }
void wt_denom_set(int d)   { s_denom = d == WT_DENOM_BTC ? WT_DENOM_BTC
                                                         : WT_DENOM_SATS; }
const char *wt_denom_unit(void)
{
    return s_denom == WT_DENOM_BTC ? "BTC" : "sats";
}

// The same amount in the OTHER unit, for the places that show both: the sign
// screen prints the total large in the chosen one and small in the other, so
// whichever way a coordinator counts, the number is on the glass without a
// trip to Settings.
// Any label carrying an amount becomes the switch. The preference belongs to
// the number, not to a settings page: wherever a figure is being compared
// against a coordinator that counts the other way, the fix is a tap on the
// figure. wt_denom_bind is what every screen calls after drawing one.
static void (*s_denom_tap)(void);
void wt_denom_on_tap(void (*fn)(void)) { s_denom_tap = fn; }

static void denom_lbl_cb(lv_event_t *e)
{
    (void)e;
    if (s_denom_tap) s_denom_tap();
}

void wt_denom_bind(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    // 12 in every direction: an amount is type, not a button, and a bare
    // label's box is exactly its glyphs. This is what makes a 14px figure
    // reachable without giving it a border it should not have.
    lv_obj_set_ext_click_area(o, 12);
    lv_obj_add_event_cb(o, denom_lbl_cb, LV_EVENT_CLICKED, NULL);
}

void wt_fmt_amount(uint64_t sats, char *out, size_t out_len)
{
    if (s_denom == WT_DENOM_BTC) wt_fmt_btc(sats, out, out_len);
    else                         wt_fmt_sats(sats, out, out_len);
}

void wt_fmt_amount_alt(uint64_t sats, char *out, size_t out_len)
{
    if (s_denom == WT_DENOM_BTC) wt_fmt_sats(sats, out, out_len);
    else                         wt_fmt_btc(sats, out, out_len);
}

const char *wt_denom_unit_alt(void)
{
    return s_denom == WT_DENOM_BTC ? "sats" : "BTC";
}

void wt_fmt_sats(uint64_t v, char *out, size_t out_len)
{
    char raw[24];
    int n = snprintf(raw, sizeof raw, "%llu", (unsigned long long)v);
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < out_len; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = 0;
}
